// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Shared in-memory TIFF byte stream and RAII libtiff wrappers for tests using
// //third_party/libgeotiff:xtiff without depending directly on
// //third_party/tiff.

#ifndef THIRD_PARTY_LIBGEOTIFF_GOOGLE_UTIL_TIFF_MEM_STREAM_H_
#define THIRD_PARTY_LIBGEOTIFF_GOOGLE_UTIL_TIFF_MEM_STREAM_H_

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "third_party/libgeotiff/libxtiff/xtiffio.h"  // IWYU pragma: keep

namespace libgeotiff {

struct TiffDeleter {
  void operator()(TIFF* tif) const { XTIFFClose(tif); }
};

using ScopedTiff = std::unique_ptr<TIFF, TiffDeleter>;

struct TiffOpenOptionsDeleter {
  void operator()(TIFFOpenOptions* opts) const { TIFFOpenOptionsFree(opts); }
};

using ScopedTiffOpenOptions =
    std::unique_ptr<TIFFOpenOptions, TiffOpenOptionsDeleter>;

struct TiffErrorRecord {
  std::string module;
  std::string message;
};

struct TiffErrorCapture {
  std::vector<TiffErrorRecord> errors;
  std::vector<TiffErrorRecord> warnings;
};

namespace internal {

inline int CaptureTiffMessage(std::vector<TiffErrorRecord>* out,
                              const char* module, const char* fmt,
                              va_list args) {
  if (out != nullptr && fmt != nullptr) {
    va_list copy;
    va_copy(copy, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::string formatted;
    if (needed > 0) {
      formatted.resize(static_cast<size_t>(needed) + 1);
      std::vsnprintf(formatted.data(), formatted.size(), fmt, args);
      formatted.resize(static_cast<size_t>(needed));
    }
    out->push_back(TiffErrorRecord{
        .module = module != nullptr ? module : "",
        .message = std::move(formatted),
    });
  }
  return 1;  // Suppress process-global stderr handlers.
}

inline int OnTiffErrorExtR(TIFF* /*tif*/, void* user_data, const char* module,
                           const char* fmt, va_list args) {
  auto* capture = static_cast<TiffErrorCapture*>(user_data);
  return CaptureTiffMessage(capture != nullptr ? &capture->errors : nullptr,
                            module, fmt, args);
}

inline int OnTiffWarningExtR(TIFF* /*tif*/, void* user_data, const char* module,
                             const char* fmt, va_list args) {
  auto* capture = static_cast<TiffErrorCapture*>(user_data);
  return CaptureTiffMessage(capture != nullptr ? &capture->warnings : nullptr,
                            module, fmt, args);
}

}  // namespace internal

inline ScopedTiffOpenOptions MakeTiffOpenOptions(
    TiffErrorCapture* capture = nullptr) {
  ScopedTiffOpenOptions opts(TIFFOpenOptionsAlloc());
  if (opts != nullptr) {
    TIFFOpenOptionsSetErrorHandlerExtR(opts.get(), internal::OnTiffErrorExtR,
                                       capture);
    TIFFOpenOptionsSetWarningHandlerExtR(opts.get(),
                                         internal::OnTiffWarningExtR, capture);
  }
  return opts;
}

inline bool WriteMinimalBaselineImage(TIFF* tif) {
  if (tif == nullptr) {
    return false;
  }
  if (TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t{1}) == 0 ||
      TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t{1}) == 0 ||
      TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8}) == 0 ||
      TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}) == 0 ||
      TIFFSetField(tif, TIFFTAG_PLANARCONFIG, uint16_t{PLANARCONFIG_CONTIG}) ==
          0) {
    return false;
  }
  uint8_t pixel = 0;
  return TIFFWriteScanline(tif, &pixel, 0, 0) == 1;
}

struct MemoryTiffStream {
  static constexpr size_t kMaxStreamBytes = size_t{1} << 30;  // 1 GiB cap.

  std::vector<uint8_t> data;
  size_t pos = 0;

  void ResetForOpen(const char* mode) {
    if (mode != nullptr && mode[0] == 'w') {
      data.clear();
    }
    pos = 0;
  }

  ScopedTiff Open(const char* mode, const char* name = "mem.tif") {
    ResetForOpen(mode);
    return ScopedTiff(XTIFFClientOpen(name, mode, this, &ReadProc, &WriteProc,
                                      &SeekProc, &CloseProc, &SizeProc,
                                      &MapProc, &UnmapProc));
  }

  ScopedTiff OpenExt(const char* mode, TIFFOpenOptions* opts,
                     const char* name = "mem.tif") {
    ResetForOpen(mode);
    return ScopedTiff(
        XTIFFClientOpenExt(name, mode, this, &ReadProc, &WriteProc, &SeekProc,
                           &CloseProc, &SizeProc, &MapProc, &UnmapProc, opts));
  }

  static tmsize_t ReadProc(thandle_t handle, void* buf, tmsize_t size) {
    auto* self = static_cast<MemoryTiffStream*>(handle);
    if (self == nullptr || buf == nullptr || size <= 0 ||
        self->pos >= self->data.size()) {
      return 0;
    }
    const size_t available = self->data.size() - self->pos;
    const size_t count = std::min(available, static_cast<size_t>(size));
    std::memcpy(buf, self->data.data() + self->pos, count);
    self->pos += count;
    return static_cast<tmsize_t>(count);
  }

  static tmsize_t WriteProc(thandle_t handle, void* buf, tmsize_t size) {
    auto* self = static_cast<MemoryTiffStream*>(handle);
    if (self == nullptr || size < 0 || (size > 0 && buf == nullptr)) {
      return -1;
    }
    if (size == 0) {
      return 0;
    }
    const size_t count = static_cast<size_t>(size);
    if (count > kMaxStreamBytes || self->pos > kMaxStreamBytes - count) {
      return -1;
    }
    const size_t needed = self->pos + count;
    if (needed > self->data.size()) {
      self->data.resize(needed, 0);
    }
    std::memcpy(self->data.data() + self->pos, buf, count);
    self->pos = needed;
    return size;
  }

  static toff_t SeekProc(thandle_t handle, toff_t off, int whence) {
    auto* self = static_cast<MemoryTiffStream*>(handle);
    if (self == nullptr) {
      return static_cast<toff_t>(-1);
    }
    constexpr int64_t kMaxOffset = static_cast<int64_t>(kMaxStreamBytes);
    const int64_t signed_off = static_cast<int64_t>(off);
    if (signed_off < -kMaxOffset || signed_off > kMaxOffset) {
      return static_cast<toff_t>(-1);
    }
    int64_t base = 0;
    switch (whence) {
      case SEEK_SET:
        base = 0;
        break;
      case SEEK_CUR:
        if (self->pos > kMaxStreamBytes) {
          return static_cast<toff_t>(-1);
        }
        base = static_cast<int64_t>(self->pos);
        break;
      case SEEK_END:
        if (self->data.size() > kMaxStreamBytes) {
          return static_cast<toff_t>(-1);
        }
        base = static_cast<int64_t>(self->data.size());
        break;
      default:
        return static_cast<toff_t>(-1);
    }
    const int64_t target = base + signed_off;
    if (target < 0 || target > kMaxOffset) {
      return static_cast<toff_t>(-1);
    }
    self->pos = static_cast<size_t>(target);
    return static_cast<toff_t>(self->pos);
  }

  static int CloseProc(thandle_t /*handle*/) { return 0; }

  static toff_t SizeProc(thandle_t handle) {
    auto* self = static_cast<MemoryTiffStream*>(handle);
    return self != nullptr ? static_cast<toff_t>(self->data.size()) : 0;
  }

  static int MapProc(thandle_t /*handle*/, void** /*base*/, toff_t* /*size*/) {
    return 0;
  }

  static void UnmapProc(thandle_t /*handle*/, void* /*base*/, toff_t /*size*/) {
  }
};

}  // namespace libgeotiff

#endif  // THIRD_PARTY_LIBGEOTIFF_GOOGLE_UTIL_TIFF_MEM_STREAM_H_
