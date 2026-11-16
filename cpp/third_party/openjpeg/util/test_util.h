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
// Shared C++20 RAII wrappers, in-memory opj_stream_t adapters, temporary path
// helpers, and synthetic test-image / codestream builders for openjpeg2 tests.

#ifndef THIRD_PARTY_OPENJPEG2_GOOGLE_UTIL_TEST_UTIL_H_
#define THIRD_PARTY_OPENJPEG2_GOOGLE_UTIL_TEST_UTIL_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/openjpeg2/src/lib/openjp2/openjpeg.h"

namespace openjpeg2 {

struct OpjCodecDeleter {
  void operator()(opj_codec_t* codec) const { opj_destroy_codec(codec); }
};
using OpjCodecPtr = std::unique_ptr<opj_codec_t, OpjCodecDeleter>;

struct OpjStreamDeleter {
  void operator()(opj_stream_t* stream) const { opj_stream_destroy(stream); }
};
using OpjStreamPtr = std::unique_ptr<opj_stream_t, OpjStreamDeleter>;

struct OpjImageDeleter {
  void operator()(opj_image_t* image) const { opj_image_destroy(image); }
};
using OpjImagePtr = std::unique_ptr<opj_image_t, OpjImageDeleter>;

// RAII wrapper for opj_codestream_info_v2_t*.
// Guards against nullptr because opj_destroy_cstr_info(&ptr) dereferences *ptr
// unconditionally in openjpeg.c.
class ScopedCstrInfo {
 public:
  explicit ScopedCstrInfo(opj_codestream_info_v2_t* ptr = nullptr)
      : ptr_(ptr) {}
  ~ScopedCstrInfo() { reset(); }

  ScopedCstrInfo(const ScopedCstrInfo&) = delete;
  ScopedCstrInfo& operator=(const ScopedCstrInfo&) = delete;

  void reset(opj_codestream_info_v2_t* ptr = nullptr) {
    if (ptr_ != nullptr) {
      opj_destroy_cstr_info(&ptr_);
    }
    ptr_ = ptr;
  }

  opj_codestream_info_v2_t* get() const { return ptr_; }
  opj_codestream_info_v2_t* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }

 private:
  opj_codestream_info_v2_t* ptr_;
};

// RAII wrapper for opj_codestream_index_t*.
class ScopedCstrIndex {
 public:
  explicit ScopedCstrIndex(opj_codestream_index_t* ptr = nullptr) : ptr_(ptr) {}
  ~ScopedCstrIndex() { reset(); }

  ScopedCstrIndex(const ScopedCstrIndex&) = delete;
  ScopedCstrIndex& operator=(const ScopedCstrIndex&) = delete;

  void reset(opj_codestream_index_t* ptr = nullptr) {
    if (ptr_ != nullptr) {
      opj_destroy_cstr_index(&ptr_);
    }
    ptr_ = ptr;
  }

  opj_codestream_index_t* get() const { return ptr_; }
  opj_codestream_index_t* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }

 private:
  opj_codestream_index_t* ptr_;
};

// Captures OpenJPEG error, warning, and info callback messages.
struct EventCapture {
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  std::vector<std::string> infos;

  static void OnError(const char* msg, void* user_data) {
    if (user_data != nullptr && msg != nullptr) {
      static_cast<EventCapture*>(user_data)->errors.emplace_back(msg);
    }
  }

  static void OnWarning(const char* msg, void* user_data) {
    if (user_data != nullptr && msg != nullptr) {
      static_cast<EventCapture*>(user_data)->warnings.emplace_back(msg);
    }
  }

  static void OnInfo(const char* msg, void* user_data) {
    if (user_data != nullptr && msg != nullptr) {
      static_cast<EventCapture*>(user_data)->infos.emplace_back(msg);
    }
  }

  void AttachTo(opj_codec_t* codec) {
    opj_set_error_handler(codec, &OnError, this);
    opj_set_warning_handler(codec, &OnWarning, this);
    opj_set_info_handler(codec, &OnInfo, this);
  }

  void Clear() {
    errors.clear();
    warnings.clear();
    infos.clear();
  }
};

// In-memory readable opj_stream_t adapter. Must outlive the returned stream.
struct MemReadStream {
  std::vector<uint8_t> data;
  size_t pos = 0;
  bool allow_seek = true;
  bool allow_skip = true;
  bool fail_seek = false;
  bool fail_skip = false;
  int fail_seek_on_call = 0;
  int seek_calls = 0;
  int fail_skip_on_call = 0;
  int skip_calls = 0;
  size_t max_read_chunk = std::numeric_limits<size_t>::max();
  size_t max_bytes_before_fail = std::numeric_limits<size_t>::max();
  size_t bytes_served = 0;

  explicit MemReadStream(std::span<const uint8_t> input)
      : data(input.begin(), input.end()) {}

  static OPJ_SIZE_T ReadCallback(void* buffer, OPJ_SIZE_T num_bytes,
                                 void* user_data) {
    auto* self = static_cast<MemReadStream*>(user_data);
    if (self == nullptr || num_bytes == 0 || self->pos >= self->data.size()) {
      return static_cast<OPJ_SIZE_T>(-1);
    }
    if (self->bytes_served >= self->max_bytes_before_fail) {
      return static_cast<OPJ_SIZE_T>(-1);
    }
    const size_t avail = self->data.size() - self->pos;
    const size_t cap = self->max_bytes_before_fail - self->bytes_served;
    const size_t to_read = std::min(
        {static_cast<size_t>(num_bytes), self->max_read_chunk, avail, cap});
    if (to_read == 0) {
      return static_cast<OPJ_SIZE_T>(-1);
    }
    std::memcpy(buffer, self->data.data() + self->pos, to_read);
    self->pos += to_read;
    self->bytes_served += to_read;
    return to_read;
  }

  static OPJ_OFF_T SkipCallback(OPJ_OFF_T num_bytes, void* user_data) {
    auto* self = static_cast<MemReadStream*>(user_data);
    if (self == nullptr || num_bytes < 0) {
      return -1;
    }
    ++self->skip_calls;
    if (self->fail_skip || (self->fail_skip_on_call > 0 &&
                            self->skip_calls == self->fail_skip_on_call)) {
      return -1;
    }
    const size_t offset = static_cast<size_t>(num_bytes);
    if (offset > self->data.size() - self->pos) {
      self->pos = self->data.size();
      return -1;
    }
    self->pos += offset;
    return num_bytes;
  }

  static OPJ_BOOL SeekCallback(OPJ_OFF_T num_bytes, void* user_data) {
    auto* self = static_cast<MemReadStream*>(user_data);
    if (self == nullptr || num_bytes < 0) {
      return OPJ_FALSE;
    }
    ++self->seek_calls;
    if (self->fail_seek || (self->fail_seek_on_call > 0 &&
                            self->seek_calls == self->fail_seek_on_call)) {
      return OPJ_FALSE;
    }
    const size_t target = static_cast<size_t>(num_bytes);
    if (target > self->data.size()) {
      return OPJ_FALSE;
    }
    self->pos = target;
    return OPJ_TRUE;
  }

  OpjStreamPtr CreateStream(size_t buffer_size = OPJ_J2K_STREAM_CHUNK_SIZE) {
    pos = 0;
    bytes_served = 0;
    seek_calls = 0;
    skip_calls = 0;
    OpjStreamPtr stream(opj_stream_create(buffer_size, OPJ_STREAM_READ));
    if (!stream) {
      return nullptr;
    }
    opj_stream_set_user_data(stream.get(), this, nullptr);
    opj_stream_set_user_data_length(stream.get(),
                                    static_cast<OPJ_UINT64>(data.size()));
    opj_stream_set_read_function(stream.get(), &ReadCallback);
    if (allow_skip) {
      opj_stream_set_skip_function(stream.get(), &SkipCallback);
    }
    if (allow_seek) {
      opj_stream_set_seek_function(stream.get(), &SeekCallback);
    }
    return stream;
  }
};

// In-memory writable opj_stream_t adapter supporting random-access seek/patch.
// Must outlive the returned stream.
struct MemWriteStream {
  std::vector<uint8_t> data;
  size_t pos = 0;
  bool allow_seek = true;
  bool allow_skip = true;
  bool fail_seek = false;
  bool fail_skip = false;
  int fail_seek_on_call = 0;
  int seek_calls = 0;
  int fail_skip_on_call = 0;
  int skip_calls = 0;
  size_t max_bytes_before_fail = std::numeric_limits<size_t>::max();
  size_t bytes_written = 0;

  static OPJ_SIZE_T WriteCallback(void* buffer, OPJ_SIZE_T num_bytes,
                                  void* user_data) {
    auto* self = static_cast<MemWriteStream*>(user_data);
    if (self == nullptr) {
      return static_cast<OPJ_SIZE_T>(-1);
    }
    if (self->bytes_written >= self->max_bytes_before_fail ||
        static_cast<size_t>(num_bytes) >
            self->max_bytes_before_fail - self->bytes_written) {
      return static_cast<OPJ_SIZE_T>(-1);
    }
    const size_t count = static_cast<size_t>(num_bytes);
    if (self->pos + count > self->data.size()) {
      self->data.resize(self->pos + count, 0);
    }
    if (count > 0) {
      std::memcpy(self->data.data() + self->pos, buffer, count);
      self->pos += count;
      self->bytes_written += count;
    }
    return num_bytes;
  }

  static OPJ_OFF_T SkipCallback(OPJ_OFF_T num_bytes, void* user_data) {
    auto* self = static_cast<MemWriteStream*>(user_data);
    if (self == nullptr || num_bytes < 0) {
      return -1;
    }
    ++self->skip_calls;
    if (self->fail_skip || (self->fail_skip_on_call > 0 &&
                            self->skip_calls == self->fail_skip_on_call)) {
      return -1;
    }
    const size_t target = self->pos + static_cast<size_t>(num_bytes);
    if (target > self->data.size()) {
      self->data.resize(target, 0);
    }
    self->pos = target;
    return num_bytes;
  }

  static OPJ_BOOL SeekCallback(OPJ_OFF_T num_bytes, void* user_data) {
    auto* self = static_cast<MemWriteStream*>(user_data);
    if (self == nullptr || num_bytes < 0) {
      return OPJ_FALSE;
    }
    ++self->seek_calls;
    if (self->fail_seek || (self->fail_seek_on_call > 0 &&
                            self->seek_calls == self->fail_seek_on_call)) {
      return OPJ_FALSE;
    }
    const size_t target = static_cast<size_t>(num_bytes);
    if (target > self->data.size()) {
      self->data.resize(target, 0);
    }
    self->pos = target;
    return OPJ_TRUE;
  }

  OpjStreamPtr CreateStream(size_t buffer_size = OPJ_J2K_STREAM_CHUNK_SIZE) {
    data.clear();
    pos = 0;
    bytes_written = 0;
    seek_calls = 0;
    skip_calls = 0;
    OpjStreamPtr stream(opj_stream_create(buffer_size, OPJ_STREAM_WRITE));
    if (!stream) {
      return nullptr;
    }
    opj_stream_set_user_data(stream.get(), this, nullptr);
    opj_stream_set_write_function(stream.get(), &WriteCallback);
    if (allow_skip) {
      opj_stream_set_skip_function(stream.get(), &SkipCallback);
    }
    if (allow_seek) {
      opj_stream_set_seek_function(stream.get(), &SeekCallback);
    }
    return stream;
  }
};

// RAII wrapper for C FILE* handles.
struct ScopedFile {
  FILE* fp = nullptr;
  ~ScopedFile() { reset(); }
  void reset(FILE* next = nullptr) {
    if (fp != nullptr) {
      std::fclose(fp);
    }
    fp = next;
  }
};

// Big-endian integer encoding/decoding helpers for synthetic J2K/JP2 streams.
inline void AppendBe16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline void AppendBe32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline uint16_t ReadBe16(std::span<const uint8_t> bytes, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8) |
                               bytes[offset + 1]);
}

inline uint32_t ReadBe32(std::span<const uint8_t> bytes, size_t offset) {
  return (static_cast<uint32_t>(bytes[offset]) << 24) |
         (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<uint32_t>(bytes[offset + 3]);
}

// RAII temporary file under ::testing::TempDir().
class TempFile {
 public:
  explicit TempFile(std::string_view filename)
      : path_(std::filesystem::path(::testing::TempDir()) / filename) {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::string path_str() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

// RAII temporary directory under ::testing::TempDir().
class TempDir {
 public:
  explicit TempDir(std::string_view dirname)
      : path_(std::filesystem::path(::testing::TempDir()) / dirname) {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::string path_str() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

// Creates a synthetic deterministic image of the requested geometry/precision.
inline OpjImagePtr CreateTestImage(
    uint32_t width, uint32_t height, uint32_t numcomps, uint32_t prec = 8,
    uint32_t sgnd = 0, OPJ_COLOR_SPACE color_space = OPJ_CLRSPC_SRGB,
    uint32_t dx = 1, uint32_t dy = 1, uint32_t x0 = 0, uint32_t y0 = 0) {
  std::vector<opj_image_cmptparm_t> params(numcomps);
  for (uint32_t c = 0; c < numcomps; ++c) {
    const uint32_t comp_dx = (c == 0) ? 1 : dx;
    const uint32_t comp_dy = (c == 0) ? 1 : dy;
    params[c] = opj_image_cmptparm_t{
        .dx = comp_dx,
        .dy = comp_dy,
        .w = (width + comp_dx - 1) / comp_dx,
        .h = (height + comp_dy - 1) / comp_dy,
        .x0 = x0,
        .y0 = y0,
        .prec = prec,
        .sgnd = sgnd,
    };
  }
  OpjImagePtr image(opj_image_create(numcomps, params.data(), color_space));
  if (!image) {
    return nullptr;
  }
  image->x0 = x0;
  image->y0 = y0;
  image->x1 = x0 + width;
  image->y1 = y0 + height;

  const uint64_t mask = (prec >= 32) ? 0xFFFFFFFFULL : ((1ULL << prec) - 1ULL);
  const int32_t bias = (sgnd != 0 && prec > 0 && prec < 32)
                           ? static_cast<int32_t>(1ULL << (prec - 1))
                           : 0;
  for (uint32_t c = 0; c < numcomps; ++c) {
    const uint32_t cw = image->comps[c].w;
    const uint32_t ch = image->comps[c].h;
    for (uint32_t y = 0; y < ch; ++y) {
      for (uint32_t x = 0; x < cw; ++x) {
        const uint64_t raw =
            (x * 13ULL + y * 29ULL + c * 53ULL + ((x ^ y) * 7ULL)) & mask;
        image->comps[c].data[y * cw + x] = static_cast<OPJ_INT32>(
            static_cast<int64_t>(raw) - static_cast<int64_t>(bias));
      }
    }
  }
  return image;
}

// Deep-clones a valid opj_image_t* (including component sample buffers and ICC
// profile, if present) so encoder calls that consume comps[c].data never
// invalidate the caller's source image.
inline OpjImagePtr CloneTestImage(const opj_image_t* image) {
  if (image == nullptr || image->numcomps == 0 || image->comps == nullptr) {
    return nullptr;
  }
  std::vector<opj_image_cmptparm_t> cmpt_params(image->numcomps);
  for (OPJ_UINT32 c = 0; c < image->numcomps; ++c) {
    cmpt_params[c].dx = image->comps[c].dx;
    cmpt_params[c].dy = image->comps[c].dy;
    cmpt_params[c].w = image->comps[c].w;
    cmpt_params[c].h = image->comps[c].h;
    cmpt_params[c].x0 = image->comps[c].x0;
    cmpt_params[c].y0 = image->comps[c].y0;
    cmpt_params[c].prec = image->comps[c].prec;
    cmpt_params[c].sgnd = image->comps[c].sgnd;
  }
  OpjImagePtr cloned_image(opj_image_tile_create(
      image->numcomps, cmpt_params.data(), image->color_space));
  if (!cloned_image) {
    return nullptr;
  }
  cloned_image->x0 = image->x0;
  cloned_image->y0 = image->y0;
  cloned_image->x1 = image->x1;
  cloned_image->y1 = image->y1;
  for (OPJ_UINT32 c = 0; c < image->numcomps; ++c) {
    cloned_image->comps[c].resno_decoded = image->comps[c].resno_decoded;
    cloned_image->comps[c].factor = image->comps[c].factor;
    cloned_image->comps[c].alpha = image->comps[c].alpha;
    if (image->comps[c].data != nullptr) {
      const size_t w = image->comps[c].w;
      const size_t h = image->comps[c].h;
      if (h != 0 &&
          w > std::numeric_limits<size_t>::max() / h / sizeof(OPJ_INT32)) {
        return nullptr;
      }
      const size_t bytes = w * h * sizeof(OPJ_INT32);
      cloned_image->comps[c].data =
          static_cast<OPJ_INT32*>(opj_image_data_alloc(bytes > 0 ? bytes : 1));
      if (cloned_image->comps[c].data == nullptr) {
        return nullptr;
      }
      if (bytes > 0) {
        std::memcpy(cloned_image->comps[c].data, image->comps[c].data, bytes);
      }
    }
  }
  if (image->icc_profile_buf != nullptr && image->icc_profile_len > 0) {
    // opj_image_destroy frees icc_profile_buf via opj_free; std::malloc is safe
    // because opj_free == free in both opj_malloc.c and alloc_fault_injector.
    cloned_image->icc_profile_buf =
        static_cast<OPJ_BYTE*>(std::malloc(image->icc_profile_len));
    if (cloned_image->icc_profile_buf == nullptr) {
      return nullptr;
    }
    std::memcpy(cloned_image->icc_profile_buf, image->icc_profile_buf,
                image->icc_profile_len);
    cloned_image->icc_profile_len = image->icc_profile_len;
  }
  return cloned_image;
}

// Snapshots all component sample buffers from `image` before encoding or for
// bit-exact round-trip comparison.
inline std::vector<std::vector<OPJ_INT32>> SnapshotImagePixels(
    const opj_image_t* image) {
  std::vector<std::vector<OPJ_INT32>> out;
  if (image == nullptr || image->comps == nullptr) {
    return out;
  }
  out.resize(image->numcomps);
  for (uint32_t c = 0; c < image->numcomps; ++c) {
    if (image->comps[c].data != nullptr) {
      const size_t n =
          static_cast<size_t>(image->comps[c].w) * image->comps[c].h;
      out[c].assign(image->comps[c].data, image->comps[c].data + n);
    }
  }
  return out;
}

// Verifies that every component in `actual` matches `expected` bit-exactly.
inline void ExpectImagePixelsEqual(
    const std::vector<std::vector<OPJ_INT32>>& expected,
    const opj_image_t* actual) {
  ASSERT_NE(actual, nullptr);
  ASSERT_EQ(actual->numcomps, expected.size());
  for (uint32_t c = 0; c < actual->numcomps; ++c) {
    const size_t n =
        static_cast<size_t>(actual->comps[c].w) * actual->comps[c].h;
    ASSERT_NE(actual->comps[c].data, nullptr) << "comp " << c;
    EXPECT_THAT(std::span<const OPJ_INT32>(actual->comps[c].data, n),
                ::testing::ElementsAreArray(expected[c]))
        << "comp " << c;
  }
}

struct TileGeometry {
  uint32_t tdx = 0;
  uint32_t tdy = 0;
  uint32_t tx0 = 0;
  uint32_t ty0 = 0;
};

// Returns lossless 5-3 single-layer encoder parameters with optional
// resolution count and tile grid geometry.
inline opj_cparameters_t MakeLosslessParams(
    uint32_t numresolution = 0,
    std::optional<TileGeometry> tile = std::nullopt) {
  opj_cparameters_t params;
  opj_set_default_encoder_parameters(&params);
  params.tcp_numlayers = 1;
  params.tcp_rates[0] = 0;
  params.cp_disto_alloc = 1;
  if (numresolution > 0) {
    params.numresolution = static_cast<int>(numresolution);
  }
  if (tile.has_value()) {
    params.tile_size_on = OPJ_TRUE;
    params.cp_tdx = static_cast<int>(tile->tdx);
    params.cp_tdy = static_cast<int>(tile->tdy);
    params.cp_tx0 = static_cast<int>(tile->tx0);
    params.cp_ty0 = static_cast<int>(tile->ty0);
  }
  return params;
}

// Encodes an opj_image_t* in memory as J2K or JP2 using optional encoder
// parameters, NULL-terminated extra_options, event capture, thread count, and
// optional post-setup mutation hook. Deep-clones valid input images so
// opj_start_compress() never consumes the caller's component buffers.
inline std::optional<std::vector<uint8_t>> CompressImageToBytes(
    const opj_image_t* image, OPJ_CODEC_FORMAT format,
    opj_cparameters_t* custom_params = nullptr,
    const char* const* extra_options = nullptr, EventCapture* events = nullptr,
    int num_threads = 0,
    std::function<bool(opj_codec_t*, opj_image_t*)> post_setup = {}) {
  OpjCodecPtr codec(opj_create_compress(format));
  if (!codec) {
    return std::nullopt;
  }
  if (events != nullptr) {
    events->AttachTo(codec.get());
  }

  OpjImagePtr cloned_image;
  opj_image_t* encode_image = const_cast<opj_image_t*>(image);
  if (image != nullptr && image->numcomps > 0 && image->comps != nullptr) {
    cloned_image = CloneTestImage(image);
    if (!cloned_image) {
      return std::nullopt;
    }
    encode_image = cloned_image.get();
  }

  opj_cparameters_t default_params = MakeLosslessParams();
  opj_cparameters_t* params =
      (custom_params != nullptr) ? custom_params : &default_params;
  if (!opj_setup_encoder(codec.get(), params, encode_image)) {
    return std::nullopt;
  }
  if (extra_options != nullptr &&
      !opj_encoder_set_extra_options(codec.get(), extra_options)) {
    return std::nullopt;
  }
  if (num_threads > 0 && !opj_codec_set_threads(codec.get(), num_threads)) {
    return std::nullopt;
  }
  if (post_setup && !post_setup(codec.get(), encode_image)) {
    return std::nullopt;
  }

  MemWriteStream mem_out;
  OpjStreamPtr stream = mem_out.CreateStream();
  if (!stream) {
    return std::nullopt;
  }
  if (!opj_start_compress(codec.get(), encode_image, stream.get()) ||
      !opj_encode(codec.get(), stream.get()) ||
      !opj_end_compress(codec.get(), stream.get())) {
    return std::nullopt;
  }
  stream.reset();
  return mem_out.data;
}

struct DecodeOptions {
  opj_dparameters_t* custom_params = nullptr;
  EventCapture* events = nullptr;
  std::optional<bool> strict = std::nullopt;
  std::optional<int> threads = std::nullopt;
  std::optional<uint32_t> layer = std::nullopt;
  std::optional<uint32_t> resolution_factor = std::nullopt;
  std::optional<std::array<int32_t, 4>> area = std::nullopt;
  bool ignore_pclr_cmap_cdef = false;
};

struct DecodeSession {
  MemReadStream mem_in;
  OpjStreamPtr stream;
  OpjCodecPtr codec;
  OpjImagePtr image;

  explicit DecodeSession(std::span<const uint8_t> bytes) : mem_in(bytes) {}
};

// Opens an in-memory J2K or JP2 stream through opj_read_header() and returns an
// active DecodeSession (or nullptr on error).
inline std::unique_ptr<DecodeSession> OpenForDecode(
    std::span<const uint8_t> bytes, OPJ_CODEC_FORMAT format,
    const DecodeOptions& opts = {}) {
  auto session = std::make_unique<DecodeSession>(bytes);
  session->stream = session->mem_in.CreateStream();
  if (!session->stream) {
    return nullptr;
  }
  session->codec.reset(opj_create_decompress(format));
  if (!session->codec) {
    return nullptr;
  }
  if (opts.events != nullptr) {
    opts.events->AttachTo(session->codec.get());
  }
  opj_dparameters_t default_params;
  opj_set_default_decoder_parameters(&default_params);
  opj_dparameters_t* params =
      (opts.custom_params != nullptr) ? opts.custom_params : &default_params;
  if (opts.layer.has_value()) {
    params->cp_layer = *opts.layer;
  }
  if (opts.resolution_factor.has_value()) {
    params->cp_reduce = *opts.resolution_factor;
  }
  if (opts.ignore_pclr_cmap_cdef) {
    params->flags |= OPJ_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG;
  }
  if (!opj_setup_decoder(session->codec.get(), params)) {
    return nullptr;
  }
  if (opts.strict.has_value() &&
      !opj_decoder_set_strict_mode(session->codec.get(),
                                   *opts.strict ? OPJ_TRUE : OPJ_FALSE)) {
    return nullptr;
  }
  if (opts.threads.has_value() &&
      !opj_codec_set_threads(session->codec.get(), *opts.threads)) {
    return nullptr;
  }
  opj_image_t* raw_image = nullptr;
  if (!opj_read_header(session->stream.get(), session->codec.get(),
                       &raw_image)) {
    opj_image_destroy(raw_image);
    return nullptr;
  }
  session->image.reset(raw_image);
  if (opts.resolution_factor.has_value() &&
      !opj_set_decoded_resolution_factor(session->codec.get(),
                                         *opts.resolution_factor)) {
    return nullptr;
  }
  if (opts.area.has_value()) {
    const auto& a = *opts.area;
    if (!opj_set_decode_area(session->codec.get(), session->image.get(), a[0],
                             a[1], a[2], a[3])) {
      return nullptr;
    }
  }
  return session;
}

// Decodes an in-memory J2K or JP2 stream to an opj_image_t (or nullptr on
// failure).
inline OpjImagePtr DecompressBytesToImage(std::span<const uint8_t> bytes,
                                          OPJ_CODEC_FORMAT format,
                                          const DecodeOptions& opts = {}) {
  std::unique_ptr<DecodeSession> session = OpenForDecode(bytes, format, opts);
  if (!session ||
      !opj_decode(session->codec.get(), session->stream.get(),
                  session->image.get()) ||
      !opj_end_decompress(session->codec.get(), session->stream.get())) {
    return nullptr;
  }
  return std::move(session->image);
}

}  // namespace openjpeg2

#endif  // THIRD_PARTY_OPENJPEG2_GOOGLE_UTIL_TEST_UTIL_H_
