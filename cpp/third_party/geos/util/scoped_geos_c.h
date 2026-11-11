// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Move-only C++20 RAII wrappers for reentrant GEOS C API context handles,
// geometry/reader/writer/index handles, buffers, and message capture.

#ifndef THIRD_PARTY_GEOS_GOOGLE_UTIL_SCOPED_GEOS_C_H_
#define THIRD_PARTY_GEOS_GOOGLE_UTIL_SCOPED_GEOS_C_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "third_party/geos/capi/geos_c.h"

namespace geos_google {

namespace internal {

inline void CaptureStringMessage(const char* message, void* userdata) {
  if (message != nullptr && userdata != nullptr) {
    static_cast<std::vector<std::string>*>(userdata)->emplace_back(message);
  }
}

}  // namespace internal

struct GeosMessageLog {
  std::vector<std::string> notices;
  std::vector<std::string> errors;
};

// Move-only RAII guard for a GEOSContextHandle_t with stable heap-backed
// notice/error message capture across moves.
class ScopedContext {
 public:
  ScopedContext()
      : handle_(GEOS_init_r()), log_(std::make_unique<GeosMessageLog>()) {
    if (handle_ != nullptr) {
      GEOSContext_setNoticeMessageHandler_r(
          handle_, &internal::CaptureStringMessage, &log_->notices);
      GEOSContext_setErrorMessageHandler_r(
          handle_, &internal::CaptureStringMessage, &log_->errors);
    }
  }

  explicit ScopedContext(GEOSContextHandle_t handle)
      : handle_(handle), log_(std::make_unique<GeosMessageLog>()) {}

  ScopedContext(const ScopedContext&) = delete;
  ScopedContext& operator=(const ScopedContext&) = delete;

  ScopedContext(ScopedContext&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)),
        log_(std::move(other.log_)) {}

  ScopedContext& operator=(ScopedContext&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.handle_, nullptr));
      log_ = std::move(other.log_);
    }
    return *this;
  }

  ~ScopedContext() { reset(); }

  void reset(GEOSContextHandle_t new_handle = nullptr) {
    if (handle_ != nullptr) {
      GEOS_finish_r(handle_);
    }
    handle_ = new_handle;
  }

  GEOSContextHandle_t release() { return std::exchange(handle_, nullptr); }
  GEOSContextHandle_t get() const { return handle_; }
  operator GEOSContextHandle_t() const {  // NOLINT(google-explicit-constructor)
    return handle_;
  }
  bool valid() const { return handle_ != nullptr; }

  const std::vector<std::string>& notices() const { return log_->notices; }
  const std::vector<std::string>& errors() const { return log_->errors; }
  void ClearMessages() {
    log_->notices.clear();
    log_->errors.clear();
  }

 private:
  GEOSContextHandle_t handle_ = nullptr;
  std::unique_ptr<GeosMessageLog> log_;
};

// Move-only RAII wrapper for GEOS C API objects destroyed by `DestroyFn(ctx,
// ptr)`.
template <typename T, void (*DestroyFn)(GEOSContextHandle_t, T*)>
class ScopedGeosObj {
 public:
  ScopedGeosObj() = default;
  ScopedGeosObj(GEOSContextHandle_t ctx, T* ptr) : ctx_(ctx), ptr_(ptr) {}

  ScopedGeosObj(const ScopedGeosObj&) = delete;
  ScopedGeosObj& operator=(const ScopedGeosObj&) = delete;

  ScopedGeosObj(ScopedGeosObj&& other) noexcept
      : ctx_(other.ctx_), ptr_(std::exchange(other.ptr_, nullptr)) {}

  ScopedGeosObj& operator=(ScopedGeosObj&& other) noexcept {
    if (this != &other) {
      reset();
      ctx_ = other.ctx_;
      ptr_ = std::exchange(other.ptr_, nullptr);
    }
    return *this;
  }

  ~ScopedGeosObj() { reset(); }

  void reset(T* new_ptr = nullptr) {
    if (ptr_ != nullptr && ctx_ != nullptr) {
      DestroyFn(ctx_, ptr_);
    }
    ptr_ = new_ptr;
  }

  void reset(GEOSContextHandle_t ctx, T* new_ptr) {
    reset();
    ctx_ = ctx;
    ptr_ = new_ptr;
  }

  T* release() { return std::exchange(ptr_, nullptr); }
  T* get() const { return ptr_; }
  operator T*() const { return ptr_; }  // NOLINT(google-explicit-constructor)
  explicit operator bool() const { return ptr_ != nullptr; }

 private:
  GEOSContextHandle_t ctx_ = nullptr;
  T* ptr_ = nullptr;
};

using ScopedGeom = ScopedGeosObj<GEOSGeometry, GEOSGeom_destroy_r>;
using ScopedCoordSeq = ScopedGeosObj<GEOSCoordSequence, GEOSCoordSeq_destroy_r>;
using ScopedPreparedGeom =
    ScopedGeosObj<const GEOSPreparedGeometry, GEOSPreparedGeom_destroy_r>;
using ScopedBufferParams =
    ScopedGeosObj<GEOSBufferParams, GEOSBufferParams_destroy_r>;
using ScopedCoverageCleanParams =
    ScopedGeosObj<GEOSCoverageCleanParams, GEOSCoverageCleanParams_destroy_r>;
using ScopedMakeValidParams =
    ScopedGeosObj<GEOSMakeValidParams, GEOSMakeValidParams_destroy_r>;
using ScopedClusterInfo =
    ScopedGeosObj<GEOSClusterInfo, GEOSClusterInfo_destroy_r>;
using ScopedSTRtree = ScopedGeosObj<GEOSSTRtree, GEOSSTRtree_destroy_r>;
using ScopedWKTReader = ScopedGeosObj<GEOSWKTReader, GEOSWKTReader_destroy_r>;
using ScopedWKTWriter = ScopedGeosObj<GEOSWKTWriter, GEOSWKTWriter_destroy_r>;
using ScopedWKBReader = ScopedGeosObj<GEOSWKBReader, GEOSWKBReader_destroy_r>;
using ScopedWKBWriter = ScopedGeosObj<GEOSWKBWriter, GEOSWKBWriter_destroy_r>;
using ScopedGeoJSONReader =
    ScopedGeosObj<GEOSGeoJSONReader, GEOSGeoJSONReader_destroy_r>;
using ScopedGeoJSONWriter =
    ScopedGeosObj<GEOSGeoJSONWriter, GEOSGeoJSONWriter_destroy_r>;

// Move-only RAII wrapper for GEOS C API buffers freed with GEOSFree_r.
template <typename T = void>
class ScopedGeosBuffer {
 public:
  ScopedGeosBuffer() = default;
  ScopedGeosBuffer(GEOSContextHandle_t ctx, T* ptr) : ctx_(ctx), ptr_(ptr) {}

  ScopedGeosBuffer(const ScopedGeosBuffer&) = delete;
  ScopedGeosBuffer& operator=(const ScopedGeosBuffer&) = delete;

  ScopedGeosBuffer(ScopedGeosBuffer&& other) noexcept
      : ctx_(other.ctx_), ptr_(std::exchange(other.ptr_, nullptr)) {}

  ScopedGeosBuffer& operator=(ScopedGeosBuffer&& other) noexcept {
    if (this != &other) {
      reset();
      ctx_ = other.ctx_;
      ptr_ = std::exchange(other.ptr_, nullptr);
    }
    return *this;
  }

  ~ScopedGeosBuffer() { reset(); }

  void reset(T* new_ptr = nullptr) {
    if (ptr_ != nullptr && ctx_ != nullptr) {
      GEOSFree_r(ctx_, ptr_);
    }
    ptr_ = new_ptr;
  }

  T* release() { return std::exchange(ptr_, nullptr); }
  T* get() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }

 private:
  GEOSContextHandle_t ctx_ = nullptr;
  T* ptr_ = nullptr;
};

using ScopedCString = ScopedGeosBuffer<char>;
using ScopedBytes = ScopedGeosBuffer<unsigned char>;

}  // namespace geos_google

#endif  // THIRD_PARTY_GEOS_GOOGLE_UTIL_SCOPED_GEOS_C_H_
