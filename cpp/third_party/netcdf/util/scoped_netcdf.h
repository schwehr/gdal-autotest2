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
// Move-only RAII wrappers for netCDF dataset handles, in-memory buffers,
// heap-returned strings/vlens, and process-global configuration state.

#ifndef THIRD_PARTY_NETCDF_GOOGLE_UTIL_SCOPED_NETCDF_H_
#define THIRD_PARTY_NETCDF_GOOGLE_UTIL_SCOPED_NETCDF_H_

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {

// Move-only RAII guard for an open netCDF file ID (`ncid >= 0`).
class ScopedNcFile {
 public:
  static constexpr int kInvalidId = -1;

  ScopedNcFile() = default;
  explicit ScopedNcFile(int ncid) : ncid_(ncid) {}

  ~ScopedNcFile() { static_cast<void>(close()); }

  ScopedNcFile(const ScopedNcFile&) = delete;
  ScopedNcFile& operator=(const ScopedNcFile&) = delete;

  ScopedNcFile(ScopedNcFile&& other) noexcept
      : ncid_(std::exchange(other.ncid_, kInvalidId)) {}

  ScopedNcFile& operator=(ScopedNcFile&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  int close() {
    if (ncid_ >= 0) {
      const int id = std::exchange(ncid_, kInvalidId);
      return nc_close(id);
    }
    ncid_ = kInvalidId;
    return NC_NOERR;
  }

  int abort() {
    if (ncid_ >= 0) {
      const int id = std::exchange(ncid_, kInvalidId);
      return nc_abort(id);
    }
    ncid_ = kInvalidId;
    return NC_NOERR;
  }

  void reset(int new_id = kInvalidId) {
    if (ncid_ >= 0) {
      nc_close(ncid_);
    }
    ncid_ = new_id;
  }

  int release() { return std::exchange(ncid_, kInvalidId); }
  int get() const { return ncid_; }
  int* ptr() {
    reset();
    return &ncid_;
  }
  operator int() const { return ncid_; }  // NOLINT(google-explicit-constructor)
  bool valid() const { return ncid_ >= 0; }

 private:
  int ncid_ = kInvalidId;
};

// Move-only RAII owner for an `NC_memio` heap buffer (e.g. from
// `nc_close_memio()` or `malloc()`).
class ScopedMemio {
 public:
  ScopedMemio() = default;
  explicit ScopedMemio(NC_memio memio) : memio_(memio) {}

  ~ScopedMemio() { reset(); }

  ScopedMemio(const ScopedMemio&) = delete;
  ScopedMemio& operator=(const ScopedMemio&) = delete;

  ScopedMemio(ScopedMemio&& other) noexcept
      : memio_(std::exchange(other.memio_, NC_memio{})) {}

  ScopedMemio& operator=(ScopedMemio&& other) noexcept {
    if (this != &other) {
      reset();
      memio_ = std::exchange(other.memio_, NC_memio{});
    }
    return *this;
  }

  void reset() {
    if (memio_.memory != nullptr) {
      std::free(memio_.memory);
    }
    memio_ = NC_memio{};
  }

  NC_memio release() { return std::exchange(memio_, NC_memio{}); }
  NC_memio* get() { return &memio_; }
  const NC_memio* get() const { return &memio_; }
  const unsigned char* bytes() const {
    return static_cast<const unsigned char*>(memio_.memory);
  }
  size_t size() const { return memio_.size; }

 private:
  NC_memio memio_{};
};

// RAII buffer of `char*` elements freed via `nc_free_string()`.
class ScopedNcStrings {
 public:
  explicit ScopedNcStrings(size_t count = 0) : ptrs_(count, nullptr) {}

  ~ScopedNcStrings() { clear(); }

  ScopedNcStrings(const ScopedNcStrings&) = delete;
  ScopedNcStrings& operator=(const ScopedNcStrings&) = delete;

  ScopedNcStrings(ScopedNcStrings&&) noexcept = default;
  ScopedNcStrings& operator=(ScopedNcStrings&&) noexcept = default;

  void clear() {
    if (!ptrs_.empty()) {
      nc_free_string(ptrs_.size(), ptrs_.data());
      ptrs_.clear();
    }
  }

  void resize(size_t count) {
    clear();
    ptrs_.assign(count, nullptr);
  }

  char** data() { return ptrs_.data(); }
  size_t size() const { return ptrs_.size(); }
  char*& operator[](size_t index) { return ptrs_[index]; }
  const char* operator[](size_t index) const { return ptrs_[index]; }

 private:
  std::vector<char*> ptrs_;
};

// RAII buffer of `nc_vlen_t` elements freed via `nc_free_vlens()`.
class ScopedNcVlens {
 public:
  explicit ScopedNcVlens(size_t count = 0) : vlens_(count) {}

  ~ScopedNcVlens() { clear(); }

  ScopedNcVlens(const ScopedNcVlens&) = delete;
  ScopedNcVlens& operator=(const ScopedNcVlens&) = delete;

  ScopedNcVlens(ScopedNcVlens&&) noexcept = default;
  ScopedNcVlens& operator=(ScopedNcVlens&&) noexcept = default;

  void clear() {
    if (!vlens_.empty()) {
      nc_free_vlens(vlens_.size(), vlens_.data());
      vlens_.clear();
    }
  }

  void resize(size_t count) {
    clear();
    vlens_.assign(count, nc_vlen_t{});
  }

  nc_vlen_t* data() { return vlens_.data(); }
  size_t size() const { return vlens_.size(); }
  nc_vlen_t& operator[](size_t index) { return vlens_[index]; }
  const nc_vlen_t& operator[](size_t index) const { return vlens_[index]; }

 private:
  std::vector<nc_vlen_t> vlens_;
};

// Saves and restores the process-wide default creation format.
class ScopedDefaultFormat {
 public:
  explicit ScopedDefaultFormat(int new_format) {
    saved_ = (nc_set_default_format(new_format, &prev_format_) == NC_NOERR);
  }

  ~ScopedDefaultFormat() {
    if (saved_) {
      nc_set_default_format(prev_format_, nullptr);
    }
  }

  ScopedDefaultFormat(const ScopedDefaultFormat&) = delete;
  ScopedDefaultFormat& operator=(const ScopedDefaultFormat&) = delete;

  int previous_format() const { return prev_format_; }

 private:
  int prev_format_ = NC_FORMAT_CLASSIC;
  bool saved_ = false;
};

// Saves and restores the process-wide HDF5 chunk cache settings.
class ScopedChunkCache {
 public:
  ScopedChunkCache() {
    saved_ = (nc_get_chunk_cache(&prev_size_, &prev_nelems_,
                                 &prev_preemption_) == NC_NOERR);
  }

  ~ScopedChunkCache() {
    if (saved_) {
      nc_set_chunk_cache(prev_size_, prev_nelems_, prev_preemption_);
    }
  }

  ScopedChunkCache(const ScopedChunkCache&) = delete;
  ScopedChunkCache& operator=(const ScopedChunkCache&) = delete;

 private:
  size_t prev_size_ = 0;
  size_t prev_nelems_ = 0;
  float prev_preemption_ = 0.0F;
  bool saved_ = false;
};

#ifndef NO_NETCDF_2
// Saves and disables/sets NetCDF-2 global `ncopts` and `ncerr`, restoring both
// on scope exit so negative V2 tests never trigger fatal `exit(ncopts)`.
class ScopedNcV2Opts {
 public:
  explicit ScopedNcV2Opts(int new_opts = 0)
      : prev_opts_(ncopts), prev_err_(ncerr) {
    ncopts = new_opts;
    ncerr = NC_NOERR;
  }

  ~ScopedNcV2Opts() {
    ncopts = prev_opts_;
    ncerr = prev_err_;
  }

  ScopedNcV2Opts(const ScopedNcV2Opts&) = delete;
  ScopedNcV2Opts& operator=(const ScopedNcV2Opts&) = delete;

 private:
  int prev_opts_;
  int prev_err_;
};
#endif  // NO_NETCDF_2

using ScopedCString = std::unique_ptr<char, decltype(&std::free)>;

inline ScopedCString MakeScopedCString(char* ptr) {
  return ScopedCString(ptr, &std::free);
}

}  // namespace netcdf

#endif  // THIRD_PARTY_NETCDF_GOOGLE_UTIL_SCOPED_NETCDF_H_
