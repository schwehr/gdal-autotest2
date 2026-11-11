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
// RAII wrappers and log helpers for PROJ C API handles in C++20 unit tests.

#ifndef THIRD_PARTY_PROJ4_GOOGLE_UTIL_SCOPED_PROJ_H_
#define THIRD_PARTY_PROJ4_GOOGLE_UTIL_SCOPED_PROJ_H_

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "third_party/proj4/proj/src/proj.h"

namespace proj {

struct PjContextDeleter {
  void operator()(PJ_CONTEXT* ctx) const noexcept { proj_context_destroy(ctx); }
};

struct PjDeleter {
  void operator()(PJ* pj) const noexcept { proj_destroy(pj); }
};

struct PjAreaDeleter {
  void operator()(PJ_AREA* area) const noexcept { proj_area_destroy(area); }
};

struct PjObjListDeleter {
  void operator()(PJ_OBJ_LIST* list) const noexcept { proj_list_destroy(list); }
};

struct ProjStringListDeleter {
  void operator()(PROJ_STRING_LIST list) const noexcept {
    proj_string_list_destroy(list);
  }
};

struct ProjStringDeleter {
  void operator()(char* str) const noexcept { proj_string_destroy(str); }
};

struct ProjIntListDeleter {
  void operator()(int* list) const noexcept { proj_int_list_destroy(list); }
};

struct CrsListParamsDeleter {
  void operator()(PROJ_CRS_LIST_PARAMETERS* params) const noexcept {
    proj_get_crs_list_parameters_destroy(params);
  }
};

struct CrsInfoListDeleter {
  void operator()(PROJ_CRS_INFO** list) const noexcept {
    proj_crs_info_list_destroy(list);
  }
};

struct UnitInfoListDeleter {
  void operator()(PROJ_UNIT_INFO** list) const noexcept {
    proj_unit_list_destroy(list);
  }
};

struct CelestialBodyListDeleter {
  void operator()(PROJ_CELESTIAL_BODY_INFO** list) const noexcept {
    proj_celestial_body_list_destroy(list);
  }
};

struct OpFactoryCtxDeleter {
  void operator()(PJ_OPERATION_FACTORY_CONTEXT* fctx) const noexcept {
    proj_operation_factory_context_destroy(fctx);
  }
};

struct FreeDeleter {
  void operator()(void* ptr) const noexcept { std::free(ptr); }
};

using ScopedContext = std::unique_ptr<PJ_CONTEXT, PjContextDeleter>;
using ScopedPj = std::unique_ptr<PJ, PjDeleter>;
using ScopedArea = std::unique_ptr<PJ_AREA, PjAreaDeleter>;
using ScopedObjList = std::unique_ptr<PJ_OBJ_LIST, PjObjListDeleter>;
using ScopedStringList = std::unique_ptr<char*, ProjStringListDeleter>;
using ScopedProjString = std::unique_ptr<char, ProjStringDeleter>;
using ScopedIntList = std::unique_ptr<int, ProjIntListDeleter>;
using ScopedCrsListParams =
    std::unique_ptr<PROJ_CRS_LIST_PARAMETERS, CrsListParamsDeleter>;
using ScopedCrsInfoList = std::unique_ptr<PROJ_CRS_INFO*, CrsInfoListDeleter>;
using ScopedUnitInfoList =
    std::unique_ptr<PROJ_UNIT_INFO*, UnitInfoListDeleter>;
using ScopedCelestialBodyList =
    std::unique_ptr<PROJ_CELESTIAL_BODY_INFO*, CelestialBodyListDeleter>;
using ScopedOpFactoryContext =
    std::unique_ptr<PJ_OPERATION_FACTORY_CONTEXT, OpFactoryCtxDeleter>;
template <typename T>
using ScopedMalloc = std::unique_ptr<T, FreeDeleter>;

// RAII wrapper for PJ_INSERT_SESSION*, whose destroy API requires PJ_CONTEXT*.
class ScopedInsertSession {
 public:
  ScopedInsertSession(PJ_CONTEXT* ctx, PJ_INSERT_SESSION* session) noexcept
      : ctx_(ctx), session_(session) {}

  ~ScopedInsertSession() {
    if (session_ != nullptr) {
      proj_insert_object_session_destroy(ctx_, session_);
    }
  }

  ScopedInsertSession(const ScopedInsertSession&) = delete;
  ScopedInsertSession& operator=(const ScopedInsertSession&) = delete;

  ScopedInsertSession(ScopedInsertSession&& other) noexcept
      : ctx_(other.ctx_), session_(std::exchange(other.session_, nullptr)) {}
  ScopedInsertSession& operator=(ScopedInsertSession&& other) noexcept {
    if (this != &other) {
      if (session_ != nullptr) {
        proj_insert_object_session_destroy(ctx_, session_);
      }
      ctx_ = other.ctx_;
      session_ = std::exchange(other.session_, nullptr);
    }
    return *this;
  }

  PJ_INSERT_SESSION* get() const noexcept { return session_; }
  explicit operator bool() const noexcept { return session_ != nullptr; }

 private:
  PJ_CONTEXT* ctx_;
  PJ_INSERT_SESSION* session_;
};

// Creates a fresh per-test PJ_CONTEXT wrapped in RAII (never PJ_DEFAULT_CTX).
inline ScopedContext MakeContext() {
  return ScopedContext(proj_context_create());
}

// Temporarily suppresses PROJ log output on a context using PJ_LOG_NONE.
class ScopedLogSuppressor {
 public:
  explicit ScopedLogSuppressor(PJ_CONTEXT* ctx) noexcept
      : ctx_(ctx),
        previous_level_(ctx != nullptr ? proj_log_level(ctx, PJ_LOG_NONE)
                                       : PJ_LOG_NONE) {}

  ~ScopedLogSuppressor() {
    if (ctx_ != nullptr) {
      proj_log_level(ctx_, previous_level_);
    }
  }

  ScopedLogSuppressor(const ScopedLogSuppressor&) = delete;
  ScopedLogSuppressor& operator=(const ScopedLogSuppressor&) = delete;

 private:
  PJ_CONTEXT* ctx_;
  PJ_LOG_LEVEL previous_level_;
};

struct LogEntry {
  int level = 0;
  std::string message;
};

// Captures PROJ log messages on a local context into an owned vector and
// clears its logger pointer safely before destruction so ctx never holds a
// dangling app_data pointer.
class ScopedLogCapture {
 public:
  explicit ScopedLogCapture(PJ_CONTEXT* ctx, PJ_LOG_LEVEL level = PJ_LOG_TRACE)
      : ctx_(ctx),
        previous_level_(ctx != nullptr ? proj_log_level(ctx, level)
                                       : PJ_LOG_NONE) {
    if (ctx_ != nullptr) {
      proj_log_func(ctx_, this, &Callback);
    }
  }

  ~ScopedLogCapture() {
    if (ctx_ != nullptr) {
      // proj_log_func ignores a null function pointer; bind a stateless no-op
      // function with null user_data to detach `this`.
      proj_log_func(ctx_, nullptr, &NoOpCallback);
      proj_log_level(ctx_, previous_level_);
    }
  }

  ScopedLogCapture(const ScopedLogCapture&) = delete;
  ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

  const std::vector<LogEntry>& entries() const noexcept { return entries_; }

  bool ContainsMessage(const std::string& substring) const {
    for (const auto& entry : entries_) {
      if (entry.message.find(substring) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  void Clear() { entries_.clear(); }

 private:
  static void Callback(void* app_data, int level, const char* msg) {
    if (app_data != nullptr && msg != nullptr) {
      static_cast<ScopedLogCapture*>(app_data)->entries_.push_back(
          {.level = level, .message = msg});
    }
  }

  static void NoOpCallback(void*, int, const char*) {}

  PJ_CONTEXT* ctx_;
  PJ_LOG_LEVEL previous_level_;
  std::vector<LogEntry> entries_;
};

}  // namespace proj

#endif  // THIRD_PARTY_PROJ4_GOOGLE_UTIL_SCOPED_PROJ_H_
