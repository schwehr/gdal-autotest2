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

// In-memory TIFFClientOpenExt stream harness supporting read/write/seek/mmap
// and deterministic I/O fault injection.

#ifndef THIRD_PARTY_TIFF_GOOGLE_UTIL_TIFF_MEMORY_STREAM_H_
#define THIRD_PARTY_TIFF_GOOGLE_UTIL_TIFF_MEMORY_STREAM_H_

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {

// In-memory backing buffer for TIFFClientOpen / TIFFClientOpenExt.
// Declare this before UniqueTiff handles so TIFFClose flushes while alive.
class MemoryTiffStream {
 public:
  MemoryTiffStream() = default;
  explicit MemoryTiffStream(std::vector<uint8_t> initial_bytes)
      : data_(std::move(initial_bytes)) {}

  MemoryTiffStream(const MemoryTiffStream&) = delete;
  MemoryTiffStream& operator=(const MemoryTiffStream&) = delete;

  // Opens this memory buffer with TIFFClientOpenExt. When opts is nullptr,
  // installs an internal DiagnosticRecorder so expected error paths stay quiet.
  UniqueTiff Open(const char* mode, TIFFOpenOptions* opts = nullptr,
                  const char* name = "memory.tiff") {
    TIFFOpenOptions* effective_opts = opts;
    UniqueTiffOptions owned_opts;
    if (effective_opts == nullptr) {
      owned_opts = MakeUniqueTiffOptions();
      diagnostics_.Install(owned_opts.get());
      effective_opts = owned_opts.get();
    }
    closed_ = false;
    pos_ = 0;
    return UniqueTiff(TIFFClientOpenExt(
        name, mode, static_cast<thandle_t>(this), &ReadProc, &WriteProc,
        &SeekProc, &CloseProc, &SizeProc, enable_mmap_ ? &MapProc : nullptr,
        enable_mmap_ ? &UnmapProc : nullptr, effective_opts));
  }

  void set_enable_mmap(bool enable) { enable_mmap_ = enable; }

  void FailReadsAfter(size_t ok_calls, tmsize_t error_return = -1) {
    fail_reads_after_ = ok_calls;
    read_error_return_ = error_return;
  }

  void FailWritesAfter(size_t ok_calls, tmsize_t error_return = -1) {
    fail_writes_after_ = ok_calls;
    write_error_return_ = error_return;
  }

  void FailSeeksAfter(size_t ok_calls) { fail_seeks_after_ = ok_calls; }

  void ForceSizeFailure(bool fail, toff_t forced_size = 0) {
    force_size_failure_ = fail;
    forced_size_ = forced_size;
  }

  void ForceCloseReturn(int ret) { forced_close_return_ = ret; }

  void ResetFaults() {
    fail_reads_after_.reset();
    fail_writes_after_.reset();
    fail_seeks_after_.reset();
    force_size_failure_ = false;
    forced_size_ = 0;
    forced_close_return_ = 0;
  }

  void ResetPosition() { pos_ = 0; }

  const std::vector<uint8_t>& data() const { return data_; }
  std::vector<uint8_t>& mutable_data() { return data_; }
  absl::Span<const uint8_t> span() const { return data_; }

  DiagnosticRecorder& diagnostics() { return diagnostics_; }
  const DiagnosticRecorder& diagnostics() const { return diagnostics_; }

  size_t read_calls() const { return read_calls_; }
  size_t write_calls() const { return write_calls_; }
  size_t seek_calls() const { return seek_calls_; }
  size_t size_calls() const { return size_calls_; }
  size_t close_calls() const { return close_calls_; }
  size_t map_calls() const { return map_calls_; }
  size_t unmap_calls() const { return unmap_calls_; }
  bool is_closed() const { return closed_; }

  static tmsize_t ReadProc(thandle_t handle, void* buf, tmsize_t size) {
    auto* self = static_cast<MemoryTiffStream*>(handle);
    if (self == nullptr || size < 0) return -1;
    const size_t call_idx = self->read_calls_++;
    if (self->fail_reads_after_.has_value() &&
        call_idx >= *self->fail_reads_after_) {
      return self->read_error_return_;
    }
    if (size == 0) return 0;
    if (self->pos_ >= self->data_.size()) return 0;
    const size_t remaining =
        self->data_.size() - static_cast<size_t>(self->pos_);
    const size_t n = std::min(static_cast<size_t>(size), remaining);
    if (n > 0) {
      std::memcpy(buf, self->data_.data() + self->pos_, n);
      self->pos_ += n;
    }
    return static_cast<tmsize_t>(n);
  }

  static tmsize_t WriteProc(thandle_t handle, void* buf, tmsize_t size) {
    auto* self = static_cast<MemoryTiffStream*>(handle);
    if (self == nullptr || size < 0) return -1;
    const size_t call_idx = self->write_calls_++;
    if (self->fail_writes_after_.has_value() &&
        call_idx >= *self->fail_writes_after_) {
      return self->write_error_return_;
    }
    if (size == 0) return 0;
    const size_t u_size = static_cast<size_t>(size);
    if (self->pos_ > std::numeric_limits<size_t>::max() - u_size) {
      return -1;
    }
    const size_t end_pos = static_cast<size_t>(self->pos_) + u_size;
    if (end_pos > self->data_.size()) {
      self->data_.resize(end_pos, 0);
    }
    std::memcpy(self->data_.data() + self->pos_, buf, u_size);
    self->pos_ = static_cast<toff_t>(end_pos);
    return size;
  }

  static toff_t SeekProc(thandle_t handle, toff_t off, int whence) {
    auto* self = static_cast<MemoryTiffStream*>(handle);
    if (self == nullptr) return static_cast<toff_t>(-1);
    const size_t call_idx = self->seek_calls_++;
    if (self->fail_seeks_after_.has_value() &&
        call_idx >= *self->fail_seeks_after_) {
      return static_cast<toff_t>(-1);
    }
    int64_t new_pos = -1;
    switch (whence) {
      case SEEK_SET:
        if (off > static_cast<toff_t>(std::numeric_limits<int64_t>::max())) {
          return static_cast<toff_t>(-1);
        }
        new_pos = static_cast<int64_t>(off);
        break;
      case SEEK_CUR: {
        const int64_t delta = static_cast<int64_t>(off);
        if (self->pos_ >
            static_cast<toff_t>(std::numeric_limits<int64_t>::max())) {
          return static_cast<toff_t>(-1);
        }
        const int64_t cur = static_cast<int64_t>(self->pos_);
        if (delta > 0 && cur > std::numeric_limits<int64_t>::max() - delta) {
          return static_cast<toff_t>(-1);
        }
        new_pos = cur + delta;
        break;
      }
      case SEEK_END: {
        const int64_t delta = static_cast<int64_t>(off);
        if (self->data_.size() >
            static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
          return static_cast<toff_t>(-1);
        }
        const int64_t end = static_cast<int64_t>(self->data_.size());
        if (delta > 0 && end > std::numeric_limits<int64_t>::max() - delta) {
          return static_cast<toff_t>(-1);
        }
        new_pos = end + delta;
        break;
      }
      default:
        return static_cast<toff_t>(-1);
    }
    if (new_pos < 0) return static_cast<toff_t>(-1);
    self->pos_ = static_cast<toff_t>(new_pos);
    return self->pos_;
  }

  static int CloseProc(thandle_t handle) {
    auto* self = static_cast<MemoryTiffStream*>(handle);
    if (self == nullptr) return -1;
    ++self->close_calls_;
    self->closed_ = true;
    return self->forced_close_return_;
  }

  static toff_t SizeProc(thandle_t handle) {
    auto* self = static_cast<MemoryTiffStream*>(handle);
    if (self == nullptr) return 0;
    ++self->size_calls_;
    if (self->force_size_failure_) return self->forced_size_;
    return static_cast<toff_t>(self->data_.size());
  }

  static int MapProc(thandle_t handle, void** base, toff_t* size) {
    auto* self = static_cast<MemoryTiffStream*>(handle);
    if (self == nullptr || !self->enable_mmap_ || self->data_.empty()) {
      return 0;
    }
    ++self->map_calls_;
    *base = self->data_.data();
    *size = static_cast<toff_t>(self->data_.size());
    return 1;
  }

  static void UnmapProc(thandle_t handle, void* /*base*/, toff_t /*size*/) {
    auto* self = static_cast<MemoryTiffStream*>(handle);
    if (self == nullptr) return;
    ++self->unmap_calls_;
  }

 private:
  std::vector<uint8_t> data_;
  toff_t pos_ = 0;
  bool enable_mmap_ = false;
  bool closed_ = false;

  std::optional<size_t> fail_reads_after_;
  tmsize_t read_error_return_ = -1;
  std::optional<size_t> fail_writes_after_;
  tmsize_t write_error_return_ = -1;
  std::optional<size_t> fail_seeks_after_;
  bool force_size_failure_ = false;
  toff_t forced_size_ = 0;
  int forced_close_return_ = 0;

  size_t read_calls_ = 0;
  size_t write_calls_ = 0;
  size_t seek_calls_ = 0;
  size_t size_calls_ = 0;
  size_t close_calls_ = 0;
  size_t map_calls_ = 0;
  size_t unmap_calls_ = 0;

  DiagnosticRecorder diagnostics_;
};

}  // namespace libtiff

#endif  // THIRD_PARTY_TIFF_GOOGLE_UTIL_TIFF_MEMORY_STREAM_H_
