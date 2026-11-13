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

#ifndef THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_TEMP_FILE_H_
#define THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_TEMP_FILE_H_

#include <cassert>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "testing/base/public/gunit.h"

namespace stcorp_coda {

// Writes raw bytes to `path`, truncating any existing file. Returns true on
// success.
inline bool WriteFileBytes(const std::filesystem::path& path,
                           std::span<const uint8_t> data) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  if (!data.empty()) {
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
  }
  out.flush();
  return out.good();
}

// Writes text to `path`, truncating any existing file. Returns true on success.
inline bool WriteFileText(const std::filesystem::path& path,
                          std::string_view text) {
  return WriteFileBytes(
      path, std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

// Creates a temporary file path under ::testing::TempDir() and removes any
// pre-existing or created file upon destruction.
class TempFile {
 public:
  explicit TempFile(std::string_view filename)
      : path_(std::filesystem::path(::testing::TempDir()) / filename) {
    assert(!filename.empty());
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

  bool WriteBytes(std::span<const uint8_t> data) const {
    return WriteFileBytes(path_, data);
  }

  bool WriteText(std::string_view text) const {
    return WriteFileText(path_, text);
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
    assert(!dirname.empty());
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

}  // namespace stcorp_coda

#endif  // THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_TEMP_FILE_H_
