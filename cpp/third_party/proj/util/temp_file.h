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
// RAII temporary file and directory helpers scoped to ::testing::TempDir().

#ifndef THIRD_PARTY_PROJ4_GOOGLE_UTIL_TEMP_FILE_H_
#define THIRD_PARTY_PROJ4_GOOGLE_UTIL_TEMP_FILE_H_

#include <cstdint>
#include <filesystem>  // NOLINT
#include <fstream>
#include <span>  // NOLINT(build/c++20)
#include <string>
#include <string_view>
#include <system_error>  // NOLINT(build/c++11)
#include <utility>

#include "testing/base/public/gunit.h"

namespace proj {

// Creates a temporary file path under ::testing::TempDir() and removes any
// pre-existing or created file upon destruction.
class TempFile {
 public:
  explicit TempFile(std::string_view filename)
      : path_(std::filesystem::path(::testing::TempDir()) / filename) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    std::filesystem::remove(path_, ec);
  }

  ~TempFile() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove(path_, ec);
    }
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  TempFile(TempFile&& other) noexcept : path_(std::move(other.path_)) {
    other.path_.clear();
  }
  TempFile& operator=(TempFile&& other) noexcept {
    if (this != &other) {
      if (!path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
      }
      path_ = std::move(other.path_);
      other.path_.clear();
    }
    return *this;
  }

  const std::filesystem::path& path() const { return path_; }
  const std::string& string() const { return path_.native(); }
  const char* c_str() const { return path_.c_str(); }

 private:
  std::filesystem::path path_;
};

// Creates an empty directory under ::testing::TempDir() and removes its entire
// tree upon destruction.
class TempDir {
 public:
  explicit TempDir(std::string_view dirname)
      : path_(std::filesystem::path(::testing::TempDir()) / dirname) {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }

  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  TempDir(TempDir&& other) noexcept : path_(std::move(other.path_)) {
    other.path_.clear();
  }
  TempDir& operator=(TempDir&& other) noexcept {
    if (this != &other) {
      if (!path_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
      }
      path_ = std::move(other.path_);
      other.path_.clear();
    }
    return *this;
  }

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path child(std::string_view name) const {
    return path_ / name;
  }
  const std::string& string() const { return path_.native(); }
  const char* c_str() const { return path_.c_str(); }

 private:
  std::filesystem::path path_;
};

inline bool WriteBinaryFile(const std::filesystem::path& path,
                            std::span<const uint8_t> data) {
  std::error_code ec;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  if (!ofs) {
    return false;
  }
  if (!data.empty()) {
    ofs.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
  }
  ofs.flush();
  return static_cast<bool>(ofs);
}

inline bool WriteTextFile(const std::filesystem::path& path,
                          std::string_view text) {
  return WriteBinaryFile(
      path, std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

}  // namespace proj

#endif  // THIRD_PARTY_PROJ4_GOOGLE_UTIL_TEMP_FILE_H_
