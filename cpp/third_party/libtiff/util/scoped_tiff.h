// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// RAII wrappers for libtiff handles, open options, RGBA readers, buffers,
// global registries/handlers, and re-entrant diagnostic capture.

#ifndef THIRD_PARTY_TIFF_GOOGLE_UTIL_SCOPED_TIFF_H_
#define THIRD_PARTY_TIFF_GOOGLE_UTIL_SCOPED_TIFF_H_

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "third_party/absl/base/thread_annotations.h"
#include "third_party/absl/synchronization/mutex.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {

struct TiffCloser {
  void operator()(TIFF* tif) const noexcept {
    if (tif != nullptr) {
      TIFFClose(tif);
    }
  }
};

using UniqueTiff = std::unique_ptr<TIFF, TiffCloser>;

struct TiffOptionsDeleter {
  void operator()(TIFFOpenOptions* opts) const noexcept {
    TIFFOpenOptionsFree(opts);  // Safe if nullptr.
  }
};

using UniqueTiffOptions = std::unique_ptr<TIFFOpenOptions, TiffOptionsDeleter>;

inline UniqueTiffOptions MakeUniqueTiffOptions() {
  return UniqueTiffOptions(TIFFOpenOptionsAlloc());
}

template <typename T>
struct TiffBufferDeleter {
  void operator()(T* ptr) const noexcept { _TIFFfree(ptr); }
};

template <typename T>
using UniqueTiffBuffer = std::unique_ptr<T, TiffBufferDeleter<T>>;

// RAII guard for TIFFRGBAImage initialization and teardown.
class ScopedRgbaImage {
 public:
  ScopedRgbaImage() = default;
  ~ScopedRgbaImage() { End(); }

  ScopedRgbaImage(const ScopedRgbaImage&) = delete;
  ScopedRgbaImage& operator=(const ScopedRgbaImage&) = delete;

  bool Begin(TIFF* tif, int stop_on_error, char emsg[1024]) {
    End();
    std::memset(&img_, 0, sizeof(img_));
    if (TIFFRGBAImageBegin(&img_, tif, stop_on_error, emsg) == 1) {
      active_ = true;
      return true;
    }
    return false;
  }

  int Get(uint32_t* raster, uint32_t width, uint32_t height) {
    if (!active_) return 0;
    return TIFFRGBAImageGet(&img_, raster, width, height);
  }

  void End() noexcept {
    if (active_) {
      TIFFRGBAImageEnd(&img_);
      active_ = false;
    }
  }

  TIFFRGBAImage* get() { return &img_; }
  const TIFFRGBAImage* get() const { return &img_; }
  bool is_active() const { return active_; }

 private:
  TIFFRGBAImage img_{};
  bool active_ = false;
};

// Saves and restores process-wide basic TIFF error and warning handlers.
class ScopedTiffErrorHandlers {
 public:
  ScopedTiffErrorHandlers(TIFFErrorHandler err, TIFFErrorHandler warn)
      : prev_err_(TIFFSetErrorHandler(err)),
        prev_warn_(TIFFSetWarningHandler(warn)) {}

  ~ScopedTiffErrorHandlers() {
    TIFFSetErrorHandler(prev_err_);
    TIFFSetWarningHandler(prev_warn_);
  }

  ScopedTiffErrorHandlers(const ScopedTiffErrorHandlers&) = delete;
  ScopedTiffErrorHandlers& operator=(const ScopedTiffErrorHandlers&) = delete;

 private:
  TIFFErrorHandler prev_err_;
  TIFFErrorHandler prev_warn_;
};

// Saves and restores process-wide extended TIFF error and warning handlers.
class ScopedTiffErrorHandlersExt {
 public:
  ScopedTiffErrorHandlersExt(TIFFErrorHandlerExt err, TIFFErrorHandlerExt warn)
      : prev_err_(TIFFSetErrorHandlerExt(err)),
        prev_warn_(TIFFSetWarningHandlerExt(warn)) {}

  ~ScopedTiffErrorHandlersExt() {
    TIFFSetErrorHandlerExt(prev_err_);
    TIFFSetWarningHandlerExt(prev_warn_);
  }

  ScopedTiffErrorHandlersExt(const ScopedTiffErrorHandlersExt&) = delete;
  ScopedTiffErrorHandlersExt& operator=(const ScopedTiffErrorHandlersExt&) =
      delete;

 private:
  TIFFErrorHandlerExt prev_err_;
  TIFFErrorHandlerExt prev_warn_;
};

// Saves and restores the process-wide TIFFSetTagExtender callback.
class ScopedTagExtender {
 public:
  explicit ScopedTagExtender(TIFFExtendProc extender)
      : prev_(TIFFSetTagExtender(extender)) {}

  ~ScopedTagExtender() { TIFFSetTagExtender(prev_); }

  ScopedTagExtender(const ScopedTagExtender&) = delete;
  ScopedTagExtender& operator=(const ScopedTagExtender&) = delete;

  TIFFExtendProc previous() const { return prev_; }

 private:
  TIFFExtendProc prev_;
};

// Registers a temporary codec via TIFFRegisterCODEC and removes it on exit.
class ScopedRegisteredCodec {
 public:
  ScopedRegisteredCodec(uint16_t scheme, const char* name, TIFFInitMethod init)
      : codec_(TIFFRegisterCODEC(scheme, name, init)) {}

  ~ScopedRegisteredCodec() {
    if (codec_ != nullptr) {
      TIFFUnRegisterCODEC(codec_);
    }
  }

  ScopedRegisteredCodec(const ScopedRegisteredCodec&) = delete;
  ScopedRegisteredCodec& operator=(const ScopedRegisteredCodec&) = delete;

  TIFFCodec* get() const { return codec_; }
  TIFFCodec* release() {
    TIFFCodec* c = codec_;
    codec_ = nullptr;
    return c;
  }

 private:
  TIFFCodec* codec_;
};

struct DiagnosticRecord {
  TIFF* tif = nullptr;
  std::string module;
  std::string message;
};

// Thread-safe re-entrant error/warning recorder for TIFFOpenOptions.
// By default returns 1 from ExtR handlers so stderr stays silent during
// negative tests. Keep this object alive longer than any opened TIFF*.
class DiagnosticRecorder {
 public:
  void Install(TIFFOpenOptions* opts) {
    if (opts == nullptr) return;
    TIFFOpenOptionsSetErrorHandlerExtR(opts, &OnError, this);
    TIFFOpenOptionsSetWarningHandlerExtR(opts, &OnWarning, this);
  }

  void set_chain_to_global(bool chain) {
    absl::MutexLock lock(mu_);
    chain_to_global_ = chain;
  }

  void Clear() {
    absl::MutexLock lock(mu_);
    errors_.clear();
    warnings_.clear();
  }

  std::vector<DiagnosticRecord> errors() const {
    absl::MutexLock lock(mu_);
    return errors_;
  }

  std::vector<DiagnosticRecord> warnings() const {
    absl::MutexLock lock(mu_);
    return warnings_;
  }

  size_t error_count() const {
    absl::MutexLock lock(mu_);
    return errors_.size();
  }

  size_t warning_count() const {
    absl::MutexLock lock(mu_);
    return warnings_.size();
  }

 private:
  static std::string FormatVa(const char* fmt, va_list ap) {
    if (fmt == nullptr) return {};
    va_list copy;
    va_copy(copy, ap);
    int len = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (len < 0) return {};
    std::string out(static_cast<size_t>(len) + 1, '\0');
    std::vsnprintf(out.data(), out.size(), fmt, ap);
    out.resize(static_cast<size_t>(len));
    return out;
  }

  static int OnError(TIFF* tif, void* user_data, const char* module,
                     const char* fmt, va_list ap) {
    auto* self = static_cast<DiagnosticRecorder*>(user_data);
    std::string msg = FormatVa(fmt, ap);
    absl::MutexLock lock(self->mu_);
    self->errors_.push_back({.tif = tif,
                             .module = module != nullptr ? module : "",
                             .message = std::move(msg)});
    return self->chain_to_global_ ? 0 : 1;
  }

  static int OnWarning(TIFF* tif, void* user_data, const char* module,
                       const char* fmt, va_list ap) {
    auto* self = static_cast<DiagnosticRecorder*>(user_data);
    std::string msg = FormatVa(fmt, ap);
    absl::MutexLock lock(self->mu_);
    self->warnings_.push_back({.tif = tif,
                               .module = module != nullptr ? module : "",
                               .message = std::move(msg)});
    return self->chain_to_global_ ? 0 : 1;
  }

  mutable absl::Mutex mu_;
  bool chain_to_global_ ABSL_GUARDED_BY(mu_) = false;
  std::vector<DiagnosticRecord> errors_ ABSL_GUARDED_BY(mu_);
  std::vector<DiagnosticRecord> warnings_ ABSL_GUARDED_BY(mu_);
};

}  // namespace libtiff

#endif  // THIRD_PARTY_TIFF_GOOGLE_UTIL_SCOPED_TIFF_H_
