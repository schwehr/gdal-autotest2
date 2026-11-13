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
// RAII wrappers for CODA public C handles and session initialization.

#ifndef THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_SCOPED_CODA_H_
#define THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_SCOPED_CODA_H_

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

#include "third_party/stcorp_coda/libcoda/coda.h"
#include "third_party/stcorp_coda/libcoda/hashtable.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/ziparchive.h"
}  // extern "C"

namespace stcorp_coda {

// Calls coda_init() on Init() and pairs it with coda_done() on destruction.
// Also resets coda_errno to CODA_SUCCESS on destruction so error state does
// not leak across tests.
class ScopedCodaSession {
 public:
  ScopedCodaSession() = default;

  ~ScopedCodaSession() { Reset(); }

  ScopedCodaSession(const ScopedCodaSession&) = delete;
  ScopedCodaSession& operator=(const ScopedCodaSession&) = delete;

  int Init() {
    if (active_) {
      return CODA_SUCCESS;
    }
    const int status = coda_init();
    if (status == CODA_SUCCESS) {
      active_ = true;
    }
    return status;
  }

  void Reset() {
    if (active_) {
      coda_done();
      active_ = false;
    }
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  bool active() const { return active_; }

 private:
  bool active_ = false;
};

// RAII wrapper for coda_product* (closes via coda_close).
// Always destroy ScopedProduct before ScopedCodaSession.
class ScopedProduct {
 public:
  ScopedProduct() = default;
  explicit ScopedProduct(coda_product* product) : product_(product) {}

  ~ScopedProduct() { Reset(); }

  ScopedProduct(const ScopedProduct&) = delete;
  ScopedProduct& operator=(const ScopedProduct&) = delete;

  ScopedProduct(ScopedProduct&& other) noexcept
      : product_(std::exchange(other.product_, nullptr)) {}

  ScopedProduct& operator=(ScopedProduct&& other) noexcept {
    if (this != &other) {
      Reset();
      product_ = std::exchange(other.product_, nullptr);
    }
    return *this;
  }

  void Reset(coda_product* product = nullptr) {
    if (product_ != nullptr) {
      coda_close(product_);
    }
    product_ = product;
  }

  coda_product** OutParam() {
    Reset();
    return &product_;
  }

  coda_product* get() const { return product_; }
  coda_product* release() { return std::exchange(product_, nullptr); }
  explicit operator bool() const { return product_ != nullptr; }

 private:
  coda_product* product_ = nullptr;
};

// RAII wrapper for coda_expression* (deletes via coda_expression_delete).
class ScopedExpr {
 public:
  ScopedExpr() = default;
  explicit ScopedExpr(coda_expression* expr) : expr_(expr) {}

  ~ScopedExpr() { Reset(); }

  ScopedExpr(const ScopedExpr&) = delete;
  ScopedExpr& operator=(const ScopedExpr&) = delete;

  ScopedExpr(ScopedExpr&& other) noexcept
      : expr_(std::exchange(other.expr_, nullptr)) {}

  ScopedExpr& operator=(ScopedExpr&& other) noexcept {
    if (this != &other) {
      Reset();
      expr_ = std::exchange(other.expr_, nullptr);
    }
    return *this;
  }

  void Reset(coda_expression* expr = nullptr) {
    if (expr_ != nullptr) {
      coda_expression_delete(expr_);
    }
    expr_ = expr;
  }

  coda_expression** OutParam() {
    Reset();
    return &expr_;
  }

  coda_expression* get() const { return expr_; }
  coda_expression* release() { return std::exchange(expr_, nullptr); }
  explicit operator bool() const { return expr_ != nullptr; }

 private:
  coda_expression* expr_ = nullptr;
};

// RAII wrapper for heap strings freed with coda_free (such as
// coda_expression_eval_string output).
class ScopedCodaString {
 public:
  ScopedCodaString() = default;
  explicit ScopedCodaString(char* str) : str_(str) {}

  ~ScopedCodaString() { Reset(); }

  ScopedCodaString(const ScopedCodaString&) = delete;
  ScopedCodaString& operator=(const ScopedCodaString&) = delete;

  void Reset(char* str = nullptr) {
    if (str_ != nullptr) {
      coda_free(str_);
    }
    str_ = str;
  }

  char** OutParam() {
    Reset();
    return &str_;
  }

  const char* get() const { return str_; }

 private:
  char* str_ = nullptr;
};

// RAII wrapper for za_file* (closes via coda_za_close).
class ScopedZaFile {
 public:
  ScopedZaFile() = default;
  explicit ScopedZaFile(za_file* zf) : zf_(zf) {}

  ~ScopedZaFile() { Reset(); }

  ScopedZaFile(const ScopedZaFile&) = delete;
  ScopedZaFile& operator=(const ScopedZaFile&) = delete;

  void Reset(za_file* zf = nullptr) {
    if (zf_ != nullptr) {
      coda_za_close(zf_);
    }
    zf_ = zf;
  }

  za_file* get() const { return zf_; }
  explicit operator bool() const { return zf_ != nullptr; }

 private:
  za_file* zf_ = nullptr;
};

// RAII wrapper for hashtable* (deletes via coda_hashtable_delete).
class ScopedHashtable {
 public:
  ScopedHashtable() = default;
  explicit ScopedHashtable(hashtable* ht) : ht_(ht) {}

  ~ScopedHashtable() { Reset(); }

  ScopedHashtable(const ScopedHashtable&) = delete;
  ScopedHashtable& operator=(const ScopedHashtable&) = delete;

  void Reset(hashtable* ht = nullptr) {
    if (ht_ != nullptr) {
      coda_hashtable_delete(ht_);
    }
    ht_ = ht;
  }

  hashtable* get() const { return ht_; }
  explicit operator bool() const { return ht_ != nullptr; }

 private:
  hashtable* ht_ = nullptr;
};

// RAII wrapper for heap strings freed with std::free (e.g., coda_path_* or
// coda_identifier_from_name outputs).
class ScopedCString {
 public:
  ScopedCString() = default;
  explicit ScopedCString(char* str) : str_(str) {}

  ~ScopedCString() { Reset(); }

  ScopedCString(const ScopedCString&) = delete;
  ScopedCString& operator=(const ScopedCString&) = delete;

  void Reset(char* str = nullptr) {
    if (str_ != nullptr) {
      std::free(str_);
    }
    str_ = str;
  }

  char** OutParam() {
    Reset();
    return &str_;
  }

  const char* get() const { return str_; }
  explicit operator bool() const { return str_ != nullptr; }

 private:
  char* str_ = nullptr;
};

// Saves and restores a process environment variable on destruction.
class ScopedEnvVar {
 public:
  explicit ScopedEnvVar(const char* name) : name_(name) {
    if (const char* value = std::getenv(name_)) {
      saved_ = value;
    }
  }

  ~ScopedEnvVar() {
    if (saved_.has_value()) {
      setenv(name_, saved_->c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

  void Set(const char* value) const { setenv(name_, value, 1); }
  void Unset() const { unsetenv(name_); }

 private:
  const char* name_;
  std::optional<std::string> saved_;
};

// Saves and restores all public CODA runtime option globals on destruction and
// resets coda_errno to CODA_SUCCESS.
class ScopedCodaOptions {
 public:
  ScopedCodaOptions()
      : bypass_special_types_(coda_get_option_bypass_special_types()),
        perform_boundary_checks_(coda_get_option_perform_boundary_checks()),
        perform_conversions_(coda_get_option_perform_conversions()),
        use_fast_size_expressions_(coda_get_option_use_fast_size_expressions()),
        use_mmap_(coda_get_option_use_mmap()) {}

  ~ScopedCodaOptions() {
    coda_set_option_bypass_special_types(bypass_special_types_);
    coda_set_option_perform_boundary_checks(perform_boundary_checks_);
    coda_set_option_perform_conversions(perform_conversions_);
    coda_set_option_use_fast_size_expressions(use_fast_size_expressions_);
    coda_set_option_use_mmap(use_mmap_);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ScopedCodaOptions(const ScopedCodaOptions&) = delete;
  ScopedCodaOptions& operator=(const ScopedCodaOptions&) = delete;

 private:
  int bypass_special_types_;
  int perform_boundary_checks_;
  int perform_conversions_;
  int use_fast_size_expressions_;
  int use_mmap_;
};

}  // namespace stcorp_coda

#endif  // THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_SCOPED_CODA_H_
