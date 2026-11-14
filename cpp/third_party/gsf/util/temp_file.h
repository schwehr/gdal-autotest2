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
// RAII temporary directory and environment variable helpers scoped to
// ::testing::TempDir().

#ifndef THIRD_PARTY_GSF_GOOGLE_UTIL_TEMP_FILE_H_
#define THIRD_PARTY_GSF_GOOGLE_UTIL_TEMP_FILE_H_

#include <stdlib.h>

#include <cstdlib>
#include <filesystem>  // NOLINT
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "testing/base/public/gunit.h"

namespace gsf {

// Creates an empty directory under ::testing::TempDir() and removes its entire
// tree upon destruction (including .gsf, .nsf, and GSFTMPDIR .ndx files).
class TempDir {
 public:
  explicit TempDir(std::string_view dirname)
      : path_(std::filesystem::path(::testing::TempDir()) / dirname) {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }

  ~TempDir() { Cleanup(); }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  TempDir(TempDir&& other) noexcept : path_(std::move(other.path_)) {
    other.path_.clear();
  }
  TempDir& operator=(TempDir&& other) noexcept {
    if (this != &other) {
      Cleanup();
      path_ = std::move(other.path_);
      other.path_.clear();
    }
    return *this;
  }

  std::filesystem::path file(std::string_view filename) const {
    return path_ / filename;
  }
  const std::filesystem::path& path() const { return path_; }
  const std::string& string() const { return path_.native(); }
  const char* c_str() const { return path_.c_str(); }

 private:
  void Cleanup() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }

  std::filesystem::path path_;
};

// Saves an environment variable on construction, updates or unsets it, and
// restores the previous value (or unsets it if absent) on destruction.
class ScopedEnvVar {
 public:
  ScopedEnvVar(std::string name, std::optional<std::string> new_value)
      : name_(std::move(name)) {
    if (const char* prev = std::getenv(name_.c_str()); prev != nullptr) {
      old_value_ = prev;
    }
    if (new_value.has_value()) {
      setenv(name_.c_str(), new_value->c_str(), /*replace=*/1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ~ScopedEnvVar() {
    if (old_value_.has_value()) {
      setenv(name_.c_str(), old_value_->c_str(), /*replace=*/1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

 private:
  std::string name_;
  std::optional<std::string> old_value_;
};

}  // namespace gsf

#endif  // THIRD_PARTY_GSF_GOOGLE_UTIL_TEMP_FILE_H_
