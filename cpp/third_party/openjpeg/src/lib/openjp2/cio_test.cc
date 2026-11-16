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
// Tests OpenJPEG byte I/O (CIO) endian conversion helpers and buffered stream
// operations (src/lib/openjp2/cio.c).
//
// Caveats:
// - google3 targets are little-endian (OPJ_BIG_ENDIAN is undefined), so
//   opj_write_bytes / opj_read_bytes / opj_write_double / opj_read_double /
//   opj_write_float / opj_read_float map to the *_LE functions in cio.h.
// - In OpenJPEG's naming convention, *_LE functions run on little-endian CPUs
//   (byte-swapping to/from big-endian JPEG 2000 stream order), whereas *_BE
//   functions run on big-endian CPUs (direct host-order memcpy).
// - opj_{write,read}_bytes_{BE,LE} assert 1 <= p_nb_bytes <= 4;
//   opj_stream_read_skip, opj_stream_skip, and opj_stream_seek assert
//   p_size >= 0; opj_stream_get_number_byte_left asserts m_byte_offset >= 0 and
//   m_user_data_length >= static_cast<OPJ_UINT64>(m_byte_offset).
// - White-box tests inspect opj_stream_private_t internals (cio.h) and pin
//   exact upstream EVT_INFO strings; ChunkedStreamState is kept local because
//   it models dynamic grow_on_write / grow_on_skip callback storage.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/cio.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/cio.h
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/openjpeg.h

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::IsEmpty;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(OPJ_STREAM_READ == OPJ_TRUE);
static_assert(OPJ_STREAM_WRITE == OPJ_FALSE);
static_assert(sizeof(OPJ_UINT32) == 4);
static_assert(sizeof(OPJ_FLOAT32) == 4);
static_assert(sizeof(OPJ_FLOAT64) == 8);
static_assert(std::is_same_v<OPJ_BYTE, unsigned char>);
static_assert(std::endian::native == std::endian::little);

constexpr OPJ_BYTE kBufferSentinel = 0xCC;
constexpr OPJ_SIZE_T kStreamReadWriteError = static_cast<OPJ_SIZE_T>(-1);
constexpr OPJ_OFF_T kStreamSkipError = static_cast<OPJ_OFF_T>(-1);

opj_stream_private_t* AsPrivate(const OpjStreamPtr& stream) {
  return reinterpret_cast<opj_stream_private_t*>(stream.get());
}

std::vector<OPJ_BYTE> MakeSequenceBytes(size_t count, OPJ_BYTE start = 0) {
  std::vector<OPJ_BYTE> bytes(count);
  for (size_t i = 0; i < count; ++i) {
    bytes[i] = static_cast<OPJ_BYTE>((static_cast<size_t>(start) + i) & 0xFFU);
  }
  return bytes;
}

std::vector<OPJ_BYTE> BytesFromText(std::string_view text) {
  std::vector<OPJ_BYTE> out(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    out[i] = static_cast<OPJ_BYTE>(text[i]);
  }
  return out;
}

// Standalone user-data lifecycle tracker for opj_stream_destroy tests.
struct UserDataTracker {
  size_t free_calls = 0;
  void* freed_ptr = nullptr;

  static void Free(void* user_data) {
    auto* self = static_cast<UserDataTracker*>(user_data);
    if (self != nullptr) {
      ++self->free_calls;
      self->freed_ptr = user_data;
    }
  }
};

// Configurable in-memory stream backend supporting chunked reads/writes/skips,
// deterministic failure injection, and invocation recording.
struct ChunkedStreamState {
  std::vector<OPJ_BYTE> data;
  size_t pos = 0;
  bool grow_on_skip = false;
  bool grow_on_seek = false;

  size_t max_read_chunk = std::numeric_limits<size_t>::max();
  size_t max_write_chunk = std::numeric_limits<size_t>::max();
  size_t max_skip_chunk = std::numeric_limits<size_t>::max();

  size_t fail_after_reads = std::numeric_limits<size_t>::max();
  size_t fail_after_writes = std::numeric_limits<size_t>::max();
  size_t fail_after_skips = std::numeric_limits<size_t>::max();
  bool fail_seek = false;

  size_t read_calls = 0;
  size_t write_calls = 0;
  size_t skip_calls = 0;
  size_t seek_calls = 0;

  std::vector<OPJ_SIZE_T> read_requests;
  std::vector<OPJ_SIZE_T> write_requests;
  std::vector<OPJ_OFF_T> skip_requests;
  std::vector<OPJ_OFF_T> seek_targets;

  static OPJ_SIZE_T Read(void* buffer, OPJ_SIZE_T nb_bytes, void* user_data) {
    auto* self = static_cast<ChunkedStreamState*>(user_data);
    if (self == nullptr) {
      return kStreamReadWriteError;
    }
    self->read_requests.push_back(nb_bytes);
    if (self->read_calls >= self->fail_after_reads || nb_bytes == 0 ||
        self->pos >= self->data.size()) {
      ++self->read_calls;
      return kStreamReadWriteError;
    }
    ++self->read_calls;
    const size_t avail = self->data.size() - self->pos;
    const size_t count =
        std::min({static_cast<size_t>(nb_bytes), avail, self->max_read_chunk});
    if (count == 0) {
      return kStreamReadWriteError;
    }
    std::memcpy(buffer, self->data.data() + self->pos, count);
    self->pos += count;
    return static_cast<OPJ_SIZE_T>(count);
  }

  static OPJ_SIZE_T Write(void* buffer, OPJ_SIZE_T nb_bytes, void* user_data) {
    auto* self = static_cast<ChunkedStreamState*>(user_data);
    if (self == nullptr) {
      return kStreamReadWriteError;
    }
    self->write_requests.push_back(nb_bytes);
    if (self->write_calls >= self->fail_after_writes) {
      ++self->write_calls;
      return kStreamReadWriteError;
    }
    ++self->write_calls;
    const size_t count =
        std::min(static_cast<size_t>(nb_bytes), self->max_write_chunk);
    if (self->pos + count > self->data.size()) {
      self->data.resize(self->pos + count, 0);
    }
    if (count > 0) {
      std::memcpy(self->data.data() + self->pos, buffer, count);
      self->pos += count;
    }
    return static_cast<OPJ_SIZE_T>(count);
  }

  static OPJ_OFF_T Skip(OPJ_OFF_T nb_bytes, void* user_data) {
    auto* self = static_cast<ChunkedStreamState*>(user_data);
    if (self == nullptr) {
      return kStreamSkipError;
    }
    self->skip_requests.push_back(nb_bytes);
    if (self->skip_calls >= self->fail_after_skips || nb_bytes < 0) {
      ++self->skip_calls;
      return kStreamSkipError;
    }
    ++self->skip_calls;
    if (!self->grow_on_skip) {
      if (self->pos >= self->data.size()) {
        return kStreamSkipError;
      }
      const size_t avail = self->data.size() - self->pos;
      const size_t count = std::min(
          {static_cast<size_t>(nb_bytes), avail, self->max_skip_chunk});
      if (count == 0) {
        return kStreamSkipError;
      }
      self->pos += count;
      return static_cast<OPJ_OFF_T>(count);
    }
    const size_t count =
        std::min(static_cast<size_t>(nb_bytes), self->max_skip_chunk);
    const size_t target = self->pos + count;
    if (target > self->data.size()) {
      self->data.resize(target, 0);
    }
    self->pos = target;
    return static_cast<OPJ_OFF_T>(count);
  }

  static OPJ_BOOL Seek(OPJ_OFF_T nb_bytes, void* user_data) {
    auto* self = static_cast<ChunkedStreamState*>(user_data);
    if (self == nullptr) {
      return OPJ_FALSE;
    }
    self->seek_targets.push_back(nb_bytes);
    ++self->seek_calls;
    if (self->fail_seek || nb_bytes < 0) {
      return OPJ_FALSE;
    }
    const size_t target = static_cast<size_t>(nb_bytes);
    if (!self->grow_on_seek) {
      if (target > self->data.size()) {
        return OPJ_FALSE;
      }
      self->pos = target;
      return OPJ_TRUE;
    }
    if (target > self->data.size()) {
      self->data.resize(target, 0);
    }
    self->pos = target;
    return OPJ_TRUE;
  }

  OpjStreamPtr CreateInputStream(size_t buffer_size, bool with_skip = true,
                                 bool with_seek = true) {
    pos = 0;
    grow_on_skip = false;
    grow_on_seek = false;
    OpjStreamPtr stream(opj_stream_create(buffer_size, OPJ_STREAM_READ));
    if (!stream) {
      return nullptr;
    }
    opj_stream_set_user_data(stream.get(), this, nullptr);
    opj_stream_set_user_data_length(stream.get(),
                                    static_cast<OPJ_UINT64>(data.size()));
    opj_stream_set_read_function(stream.get(), &Read);
    if (with_skip) {
      opj_stream_set_skip_function(stream.get(), &Skip);
    }
    if (with_seek) {
      opj_stream_set_seek_function(stream.get(), &Seek);
    }
    return stream;
  }

  OpjStreamPtr CreateOutputStream(size_t buffer_size, bool with_skip = true,
                                  bool with_seek = true) {
    data.clear();
    pos = 0;
    grow_on_skip = true;
    grow_on_seek = true;
    OpjStreamPtr stream(opj_stream_create(buffer_size, OPJ_STREAM_WRITE));
    if (!stream) {
      return nullptr;
    }
    opj_stream_set_user_data(stream.get(), this, nullptr);
    opj_stream_set_write_function(stream.get(), &Write);
    if (with_skip) {
      opj_stream_set_skip_function(stream.get(), &Skip);
    }
    if (with_seek) {
      opj_stream_set_seek_function(stream.get(), &Seek);
    }
    return stream;
  }
};

struct BytesLeCase {
  std::string_view name;
  OPJ_UINT32 value;
  OPJ_UINT32 nb_bytes;
  std::array<OPJ_BYTE, 4> expected_wire_bytes;
  OPJ_UINT32 expected_decoded;
};

constexpr BytesLeCase kBytesLeCases[] = {
    {.name = "1_byte_low",
     .value = 0x12345678U,
     .nb_bytes = 1,
     .expected_wire_bytes = {0x78, 0, 0, 0},
     .expected_decoded = 0x00000078U},
    {.name = "2_bytes_low",
     .value = 0x12345678U,
     .nb_bytes = 2,
     .expected_wire_bytes = {0x56, 0x78, 0, 0},
     .expected_decoded = 0x00005678U},
    {.name = "3_bytes_low",
     .value = 0x12345678U,
     .nb_bytes = 3,
     .expected_wire_bytes = {0x34, 0x56, 0x78, 0},
     .expected_decoded = 0x00345678U},
    {.name = "4_bytes_full",
     .value = 0x12345678U,
     .nb_bytes = 4,
     .expected_wire_bytes = {0x12, 0x34, 0x56, 0x78},
     .expected_decoded = 0x12345678U},
    {.name = "1_byte_high_bit",
     .value = 0xA55A00FFU,
     .nb_bytes = 1,
     .expected_wire_bytes = {0xFF, 0, 0, 0},
     .expected_decoded = 0x000000FFU},
    {.name = "2_bytes_zero_low",
     .value = 0xA55A00FFU,
     .nb_bytes = 2,
     .expected_wire_bytes = {0x00, 0xFF, 0, 0},
     .expected_decoded = 0x000000FFU},
    {.name = "3_bytes_mixed",
     .value = 0xA55A00FFU,
     .nb_bytes = 3,
     .expected_wire_bytes = {0x5A, 0x00, 0xFF, 0},
     .expected_decoded = 0x005A00FFU},
    {.name = "4_bytes_mixed",
     .value = 0xA55A00FFU,
     .nb_bytes = 4,
     .expected_wire_bytes = {0xA5, 0x5A, 0x00, 0xFF},
     .expected_decoded = 0xA55A00FFU},
    {.name = "zero_4_bytes",
     .value = 0x00000000U,
     .nb_bytes = 4,
     .expected_wire_bytes = {0x00, 0x00, 0x00, 0x00},
     .expected_decoded = 0x00000000U},
    {.name = "all_ones_3_bytes",
     .value = 0xFFFFFFFFU,
     .nb_bytes = 3,
     .expected_wire_bytes = {0xFF, 0xFF, 0xFF, 0},
     .expected_decoded = 0x00FFFFFFU},
};

TEST(CioTest, WriteAndReadBytesLeRoundTripsAndWritesBigEndianWireBytes) {
  for (const BytesLeCase& tc : kBytesLeCases) {
    SCOPED_TRACE(tc.name);
    std::array<OPJ_BYTE, 6> buf{};
    buf.fill(kBufferSentinel);

    opj_write_bytes_LE(buf.data() + 1, tc.value, tc.nb_bytes);
    EXPECT_EQ(kBufferSentinel, buf[0]);
    for (OPJ_UINT32 i = 0; i < tc.nb_bytes; ++i) {
      EXPECT_EQ(tc.expected_wire_bytes[i], buf[1 + i]) << "byte " << i;
    }
    for (size_t i = 1 + tc.nb_bytes; i < buf.size(); ++i) {
      EXPECT_EQ(kBufferSentinel, buf[i]) << "sentinel " << i;
    }

    // Pre-fill decoded with a non-zero sentinel to verify *p_value = 0 clears
    // unused upper bytes.
    OPJ_UINT32 decoded = 0xDEADBEEFU;
    opj_read_bytes_LE(buf.data() + 1, &decoded, tc.nb_bytes);
    EXPECT_EQ(tc.expected_decoded, decoded);

    // Also verify the platform macro aliases from cio.h match *_LE on LE hosts.
    buf.fill(kBufferSentinel);
    opj_write_bytes(buf.data() + 1, tc.value, tc.nb_bytes);
    decoded = 0xCAFEBABEU;
    opj_read_bytes(buf.data() + 1, &decoded, tc.nb_bytes);
    EXPECT_EQ(tc.expected_decoded, decoded);
  }
}

TEST(CioTest, WriteAndReadBytesBeRoundTripsHostOrderSlice) {
  constexpr OPJ_UINT32 kValues[] = {0x12345678U, 0x89ABCDEFU, 0x00000000U,
                                    0xFFFFFFFFU};

  for (const OPJ_UINT32 value : kValues) {
    const auto host_bytes = std::bit_cast<std::array<OPJ_BYTE, 4>>(value);
    for (OPJ_UINT32 nb_bytes = 1; nb_bytes <= 4; ++nb_bytes) {
      SCOPED_TRACE(::testing::Message()
                   << "value=" << value << ", nb=" << nb_bytes);
      std::array<OPJ_BYTE, 6> buf{};
      buf.fill(kBufferSentinel);

      opj_write_bytes_BE(buf.data() + 1, value, nb_bytes);
      EXPECT_EQ(kBufferSentinel, buf[0]);
      const size_t offset = 4 - nb_bytes;
      for (OPJ_UINT32 i = 0; i < nb_bytes; ++i) {
        EXPECT_EQ(host_bytes[offset + i], buf[1 + i]) << "byte " << i;
      }
      for (size_t i = 1 + nb_bytes; i < buf.size(); ++i) {
        EXPECT_EQ(kBufferSentinel, buf[i]) << "sentinel " << i;
      }

      OPJ_UINT32 decoded = 0xDEADBEEFU;
      opj_read_bytes_BE(buf.data() + 1, &decoded, nb_bytes);
      const OPJ_UINT32 expected_decoded =
          (nb_bytes == 4) ? value
                          : (value & (0xFFFFFFFFU << (8U * (4U - nb_bytes))));
      EXPECT_EQ(expected_decoded, decoded);
    }
  }
}

TEST(CioTest, WriteAndReadFloatAndDoubleLeAndBeRoundTrip) {
  struct FloatCase {
    std::string_view name;
    OPJ_FLOAT32 value;
  };
  constexpr FloatCase kFloatCases[] = {
      {.name = "pos_zero", .value = 0.0f},
      {.name = "neg_zero", .value = -0.0f},
      {.name = "one_point_five", .value = 1.5f},
      {.name = "neg_fraction", .value = -42.25f},
      {.name = "min_normal", .value = std::numeric_limits<float>::min()},
      {.name = "denorm_min", .value = std::numeric_limits<float>::denorm_min()},
      {.name = "pos_inf", .value = std::numeric_limits<float>::infinity()},
      {.name = "neg_inf", .value = -std::numeric_limits<float>::infinity()},
      {.name = "quiet_nan", .value = std::numeric_limits<float>::quiet_NaN()},
  };

  for (const FloatCase& tc : kFloatCases) {
    SCOPED_TRACE(tc.name);
    const uint32_t expected_bits = std::bit_cast<uint32_t>(tc.value);
    const auto host_bytes = std::bit_cast<std::array<OPJ_BYTE, 4>>(tc.value);

    // 1. *_LE byte-swaps host bytes and round-trips exact bit pattern.
    std::array<OPJ_BYTE, 6> le_buf{};
    le_buf.fill(kBufferSentinel);
    opj_write_float_LE(le_buf.data() + 1, tc.value);
    EXPECT_EQ(kBufferSentinel, le_buf[0]);
    EXPECT_EQ(kBufferSentinel, le_buf[5]);
    for (size_t i = 0; i < 4; ++i) {
      EXPECT_EQ(host_bytes[3 - i], le_buf[1 + i]) << "le byte " << i;
    }
    OPJ_FLOAT32 le_out = 123.0f;
    opj_read_float_LE(le_buf.data() + 1, &le_out);
    EXPECT_EQ(expected_bits, std::bit_cast<uint32_t>(le_out));

    // 2. *_BE copies host bytes directly and round-trips exact bit pattern.
    std::array<OPJ_BYTE, 6> be_buf{};
    be_buf.fill(kBufferSentinel);
    opj_write_float_BE(be_buf.data() + 1, tc.value);
    EXPECT_EQ(kBufferSentinel, be_buf[0]);
    EXPECT_EQ(kBufferSentinel, be_buf[5]);
    for (size_t i = 0; i < 4; ++i) {
      EXPECT_EQ(host_bytes[i], be_buf[1 + i]) << "be byte " << i;
    }
    OPJ_FLOAT32 be_out = 123.0f;
    opj_read_float_BE(be_buf.data() + 1, &be_out);
    EXPECT_EQ(expected_bits, std::bit_cast<uint32_t>(be_out));
  }

  // Verify explicit IEEE-754 big-endian wire encoding for 1.5f (0x3FC00000).
  std::array<OPJ_BYTE, 4> wire_f{};
  opj_write_float(wire_f.data(), 1.5f);
  EXPECT_THAT(wire_f, ElementsAre(0x3F, 0xC0, 0x00, 0x00));
  OPJ_FLOAT32 macro_f = 0.0f;
  opj_read_float(wire_f.data(), &macro_f);
  EXPECT_FLOAT_EQ(1.5f, macro_f);

  struct DoubleCase {
    std::string_view name;
    OPJ_FLOAT64 value;
  };
  constexpr DoubleCase kDoubleCases[] = {
      {.name = "pos_zero", .value = 0.0},
      {.name = "neg_zero", .value = -0.0},
      {.name = "one", .value = 1.0},
      {.name = "neg_decimal", .value = -123.456789},
      {.name = "min_normal", .value = std::numeric_limits<double>::min()},
      {.name = "denorm_min",
       .value = std::numeric_limits<double>::denorm_min()},
      {.name = "pos_inf", .value = std::numeric_limits<double>::infinity()},
      {.name = "neg_inf", .value = -std::numeric_limits<double>::infinity()},
      {.name = "quiet_nan", .value = std::numeric_limits<double>::quiet_NaN()},
  };

  for (const DoubleCase& tc : kDoubleCases) {
    SCOPED_TRACE(tc.name);
    const uint64_t expected_bits = std::bit_cast<uint64_t>(tc.value);
    const auto host_bytes = std::bit_cast<std::array<OPJ_BYTE, 8>>(tc.value);

    // 3. Double *_LE byte-swaps host bytes and round-trips exact bit pattern
    //    (byte-wise memcpy avoids FP canonicalization on NaN payloads).
    std::array<OPJ_BYTE, 10> le_buf{};
    le_buf.fill(kBufferSentinel);
    opj_write_double_LE(le_buf.data() + 1, tc.value);
    EXPECT_EQ(kBufferSentinel, le_buf[0]);
    EXPECT_EQ(kBufferSentinel, le_buf[9]);
    for (size_t i = 0; i < 8; ++i) {
      EXPECT_EQ(host_bytes[7 - i], le_buf[1 + i]) << "le byte " << i;
    }
    OPJ_FLOAT64 le_out = 999.0;
    opj_read_double_LE(le_buf.data() + 1, &le_out);
    EXPECT_EQ(expected_bits, std::bit_cast<uint64_t>(le_out));

    // 4. Double *_BE copies host bytes directly and round-trips exact pattern.
    std::array<OPJ_BYTE, 10> be_buf{};
    be_buf.fill(kBufferSentinel);
    opj_write_double_BE(be_buf.data() + 1, tc.value);
    EXPECT_EQ(kBufferSentinel, be_buf[0]);
    EXPECT_EQ(kBufferSentinel, be_buf[9]);
    for (size_t i = 0; i < 8; ++i) {
      EXPECT_EQ(host_bytes[i], be_buf[1 + i]) << "be byte " << i;
    }
    OPJ_FLOAT64 be_out = 999.0;
    opj_read_double_BE(be_buf.data() + 1, &be_out);
    EXPECT_EQ(expected_bits, std::bit_cast<uint64_t>(be_out));
  }

  // Verify explicit IEEE-754 big-endian wire encoding for 1.0 (0x3FF0...0).
  std::array<OPJ_BYTE, 8> wire_d{};
  opj_write_double(wire_d.data(), 1.0);
  EXPECT_THAT(wire_d,
              ElementsAre(0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));
  OPJ_FLOAT64 macro_d = 0.0;
  opj_read_double(wire_d.data(), &macro_d);
  EXPECT_DOUBLE_EQ(1.0, macro_d);
}

TEST(CioTest, StreamCreateDefaultCreateAndDestroyLifecycle) {
  // 1. opj_stream_destroy(nullptr) is a safe no-op.
  opj_stream_destroy(nullptr);

  // 2. Zero buffer size causes opj_malloc(0) -> nullptr, triggering clean error
  // cleanup without OOM.
  EXPECT_EQ(nullptr, opj_stream_create(0, OPJ_STREAM_READ));
  EXPECT_EQ(nullptr, opj_stream_create(0, OPJ_STREAM_WRITE));

  // 3. opj_stream_default_create initializes OPJ_J2K_STREAM_CHUNK_SIZE buffers
  // and direction-specific dispatch pointers.
  OpjStreamPtr def_in(opj_stream_default_create(OPJ_STREAM_READ));
  ASSERT_NE(nullptr, def_in);
  opj_stream_private_t* const priv_in = AsPrivate(def_in);
  EXPECT_EQ(OPJ_J2K_STREAM_CHUNK_SIZE, priv_in->m_buffer_size);
  EXPECT_NE(nullptr, priv_in->m_stored_data);
  EXPECT_EQ(priv_in->m_stored_data, priv_in->m_current_data);
  EXPECT_EQ(OPJ_STREAM_STATUS_INPUT, priv_in->m_status);
  EXPECT_EQ(&opj_stream_read_skip, priv_in->m_opj_skip);
  EXPECT_EQ(&opj_stream_read_seek, priv_in->m_opj_seek);
  EXPECT_EQ(&opj_stream_default_read, priv_in->m_read_fn);
  EXPECT_EQ(&opj_stream_default_write, priv_in->m_write_fn);
  EXPECT_EQ(&opj_stream_default_skip, priv_in->m_skip_fn);
  EXPECT_EQ(&opj_stream_default_seek, priv_in->m_seek_fn);
  EXPECT_EQ(nullptr, priv_in->m_user_data);
  EXPECT_EQ(nullptr, priv_in->m_free_user_data_fn);
  EXPECT_EQ(0u, priv_in->m_user_data_length);
  EXPECT_EQ(0u, priv_in->m_bytes_in_buffer);
  EXPECT_EQ(0, priv_in->m_byte_offset);

  OpjStreamPtr def_out(opj_stream_default_create(OPJ_STREAM_WRITE));
  ASSERT_NE(nullptr, def_out);
  opj_stream_private_t* const priv_out = AsPrivate(def_out);
  EXPECT_EQ(OPJ_J2K_STREAM_CHUNK_SIZE, priv_out->m_buffer_size);
  EXPECT_EQ(OPJ_STREAM_STATUS_OUTPUT, priv_out->m_status);
  EXPECT_EQ(&opj_stream_write_skip, priv_out->m_opj_skip);
  EXPECT_EQ(&opj_stream_write_seek, priv_out->m_opj_seek);

  // 4. Custom buffer size without free_user_data_fn leaves user data untouched.
  UserDataTracker untouched_tracker;
  {
    OpjStreamPtr stream(opj_stream_create(32, OPJ_STREAM_READ));
    ASSERT_NE(nullptr, stream);
    EXPECT_EQ(32u, AsPrivate(stream)->m_buffer_size);
    opj_stream_set_user_data(stream.get(), &untouched_tracker, nullptr);
  }
  EXPECT_EQ(0u, untouched_tracker.free_calls);

  // 5. Destroy invokes free_user_data_fn exactly once with m_user_data.
  UserDataTracker tracker;
  {
    OpjStreamPtr stream(opj_stream_create(16, OPJ_STREAM_WRITE));
    ASSERT_NE(nullptr, stream);
    opj_stream_set_user_data(stream.get(), &tracker, &UserDataTracker::Free);
  }
  EXPECT_EQ(1u, tracker.free_calls);
  EXPECT_EQ(&tracker, tracker.freed_ptr);

  // 6. Heap-allocated user data is freed via free_user_data_fn without leak.
  auto owned_payload = std::make_unique<int>(42);
  OpjStreamPtr owning_stream(opj_stream_create(16, OPJ_STREAM_READ));
  ASSERT_NE(nullptr, owning_stream);
  opj_stream_set_user_data(owning_stream.get(), owned_payload.release(),
                           [](void* p) { delete static_cast<int*>(p); });
}

TEST(CioTest, SetStreamFunctionsHandlesNullAndDirectionGuards) {
  // 1. All setters ignore a nullptr stream cleanly.
  opj_stream_set_read_function(nullptr, &ChunkedStreamState::Read);
  opj_stream_set_write_function(nullptr, &ChunkedStreamState::Write);
  opj_stream_set_skip_function(nullptr, &ChunkedStreamState::Skip);
  opj_stream_set_seek_function(nullptr, &ChunkedStreamState::Seek);
  opj_stream_set_user_data(nullptr, nullptr, &UserDataTracker::Free);
  opj_stream_set_user_data_length(nullptr, 123);

  // 2. Input stream accepts read/skip/seek/user_data/length and ignores write.
  ChunkedStreamState state;
  OpjStreamPtr in_stream(opj_stream_create(16, OPJ_STREAM_READ));
  ASSERT_NE(nullptr, in_stream);
  opj_stream_private_t* const priv_in = AsPrivate(in_stream);
  EXPECT_EQ(OPJ_FALSE, opj_stream_has_seek(priv_in));

  opj_stream_set_write_function(in_stream.get(), &ChunkedStreamState::Write);
  EXPECT_EQ(&opj_stream_default_write, priv_in->m_write_fn);

  opj_stream_set_read_function(in_stream.get(), &ChunkedStreamState::Read);
  opj_stream_set_skip_function(in_stream.get(), &ChunkedStreamState::Skip);
  opj_stream_set_seek_function(in_stream.get(), &ChunkedStreamState::Seek);
  opj_stream_set_user_data(in_stream.get(), &state, nullptr);
  opj_stream_set_user_data_length(in_stream.get(), 4096u);

  EXPECT_EQ(&ChunkedStreamState::Read, priv_in->m_read_fn);
  EXPECT_EQ(&ChunkedStreamState::Skip, priv_in->m_skip_fn);
  EXPECT_EQ(&ChunkedStreamState::Seek, priv_in->m_seek_fn);
  EXPECT_EQ(&state, priv_in->m_user_data);
  EXPECT_EQ(4096u, priv_in->m_user_data_length);
  EXPECT_EQ(OPJ_TRUE, opj_stream_has_seek(priv_in));

  // Restoring default seek flips opj_stream_has_seek back to OPJ_FALSE.
  opj_stream_set_seek_function(in_stream.get(), &opj_stream_default_seek);
  EXPECT_EQ(OPJ_FALSE, opj_stream_has_seek(priv_in));

  // 3. Output stream accepts write/skip/seek/user_data/length and ignores read.
  OpjStreamPtr out_stream(opj_stream_create(16, OPJ_STREAM_WRITE));
  ASSERT_NE(nullptr, out_stream);
  opj_stream_private_t* const priv_out = AsPrivate(out_stream);

  opj_stream_set_read_function(out_stream.get(), &ChunkedStreamState::Read);
  EXPECT_EQ(&opj_stream_default_read, priv_out->m_read_fn);

  opj_stream_set_write_function(out_stream.get(), &ChunkedStreamState::Write);
  opj_stream_set_skip_function(out_stream.get(), &ChunkedStreamState::Skip);
  opj_stream_set_seek_function(out_stream.get(), &ChunkedStreamState::Seek);
  opj_stream_set_user_data(out_stream.get(), &state, nullptr);
  opj_stream_set_user_data_length(out_stream.get(), 512u);

  EXPECT_EQ(&ChunkedStreamState::Write, priv_out->m_write_fn);
  EXPECT_EQ(&ChunkedStreamState::Skip, priv_out->m_skip_fn);
  EXPECT_EQ(&ChunkedStreamState::Seek, priv_out->m_seek_fn);
  EXPECT_EQ(&state, priv_out->m_user_data);
  EXPECT_EQ(512u, priv_out->m_user_data_length);
  EXPECT_EQ(OPJ_TRUE, opj_stream_has_seek(priv_out));
}

TEST(CioTest, DefaultStreamCallbacksReturnFailureSentinels) {
  std::array<OPJ_BYTE, 8> buf{};
  EXPECT_EQ(kStreamReadWriteError,
            opj_stream_default_read(buf.data(), buf.size(), nullptr));
  EXPECT_EQ(kStreamReadWriteError,
            opj_stream_default_write(buf.data(), buf.size(), nullptr));
  EXPECT_EQ(kStreamSkipError, opj_stream_default_skip(4, nullptr));
  EXPECT_EQ(OPJ_FALSE, opj_stream_default_seek(4, nullptr));

  // Default input stream fails read, skip, and seek cleanly.
  EventCapture in_events;
  opj_event_mgr_t in_mgr = MakeEventManager(&in_events);
  OpjStreamPtr in_stream(opj_stream_create(8, OPJ_STREAM_READ));
  ASSERT_NE(nullptr, in_stream);
  opj_stream_private_t* const priv_in = AsPrivate(in_stream);
  opj_stream_set_user_data_length(in_stream.get(), 64);

  EXPECT_EQ(OPJ_FALSE, opj_stream_has_seek(priv_in));
  EXPECT_EQ(OPJ_FALSE, opj_stream_seek(priv_in, 4, &in_mgr));
  EXPECT_NE(0u, priv_in->m_status & OPJ_STREAM_STATUS_END);
  EXPECT_EQ(kStreamReadWriteError,
            opj_stream_read_data(priv_in, buf.data(), 4, &in_mgr));
  EXPECT_EQ(kStreamSkipError, opj_stream_skip(priv_in, 4, &in_mgr));

  // Also test default_skip when END is not yet set and length > 0.
  OpjStreamPtr unskippable_in(opj_stream_create(8, OPJ_STREAM_READ));
  ASSERT_NE(nullptr, unskippable_in);
  opj_stream_private_t* const priv_unskippable = AsPrivate(unskippable_in);
  opj_stream_set_user_data_length(unskippable_in.get(), 64);
  EXPECT_EQ(kStreamSkipError, opj_stream_skip(priv_unskippable, 4, &in_mgr));
  EXPECT_NE(0u, priv_unskippable->m_status & OPJ_STREAM_STATUS_END);
  EXPECT_THAT(in_events.errors, IsEmpty());
  EXPECT_THAT(in_events.warnings, IsEmpty());
  EXPECT_THAT(in_events.infos, ElementsAre("Stream reached its end !\n"));

  // Default output stream fails flush, overflow write, skip, and seek cleanly.
  EventCapture out_events;
  opj_event_mgr_t out_mgr = MakeEventManager(&out_events);
  OpjStreamPtr out_stream(opj_stream_create(8, OPJ_STREAM_WRITE));
  ASSERT_NE(nullptr, out_stream);
  opj_stream_private_t* const priv_out = AsPrivate(out_stream);

  EXPECT_EQ(4u, opj_stream_write_data(priv_out, buf.data(), 4, &out_mgr));
  EXPECT_EQ(OPJ_FALSE, opj_stream_flush(priv_out, &out_mgr));
  EXPECT_NE(0u, priv_out->m_status & OPJ_STREAM_STATUS_ERROR);
  EXPECT_EQ(kStreamReadWriteError,
            opj_stream_write_data(priv_out, buf.data(), 4, &out_mgr));
  EXPECT_EQ(kStreamSkipError, opj_stream_skip(priv_out, 4, &out_mgr));
  EXPECT_THAT(out_events.infos, ElementsAre("Error on writing stream!\n"));

  // Default skip and seek on empty-buffer output streams invoke default_skip
  // and default_seek directly.
  OpjStreamPtr skip_fail_out(opj_stream_create(8, OPJ_STREAM_WRITE));
  ASSERT_NE(nullptr, skip_fail_out);
  EXPECT_EQ(kStreamSkipError,
            opj_stream_skip(AsPrivate(skip_fail_out), 4, &out_mgr));
  EXPECT_NE(0u, AsPrivate(skip_fail_out)->m_status & OPJ_STREAM_STATUS_ERROR);

  OpjStreamPtr seek_fail_out(opj_stream_create(8, OPJ_STREAM_WRITE));
  ASSERT_NE(nullptr, seek_fail_out);
  EXPECT_EQ(OPJ_FALSE, opj_stream_seek(AsPrivate(seek_fail_out), 0, &out_mgr));
  EXPECT_NE(0u, AsPrivate(seek_fail_out)->m_status & OPJ_STREAM_STATUS_ERROR);
}

TEST(CioTest, ReadDataBufferedExactBoundaryAndDirectMultiChunk) {
  // 1. Buffered reads across partial, exact-drain, refill, and span boundaries.
  ChunkedStreamState state;
  state.data = MakeSequenceBytes(29);
  OpjStreamPtr stream = state.CreateInputStream(/*buffer_size=*/8);
  ASSERT_NE(nullptr, stream);
  opj_stream_private_t* const priv = AsPrivate(stream);

  EventCapture events;
  opj_event_mgr_t mgr = MakeEventManager(&events);

  std::vector<OPJ_BYTE> out(32, kBufferSentinel);

  // Zero-byte read succeeds without calling read_fn.
  EXPECT_EQ(0u, opj_stream_read_data(priv, out.data(), 0, &mgr));
  EXPECT_EQ(kBufferSentinel, out[0]);
  EXPECT_EQ(0u, state.read_calls);
  EXPECT_EQ(0, opj_stream_tell(priv));
  EXPECT_EQ(29, opj_stream_get_number_byte_left(priv));

  // Read 3 (< 8): reads an 8-byte chunk and leaves 5 bytes buffered.
  ASSERT_EQ(3u, opj_stream_read_data(priv, out.data(), 3, &mgr));
  EXPECT_EQ(1u, state.read_calls);
  EXPECT_THAT(state.read_requests, ElementsAre(8u));
  EXPECT_EQ(5u, priv->m_bytes_in_buffer);
  EXPECT_EQ(3, opj_stream_tell(priv));
  EXPECT_EQ(26, opj_stream_get_number_byte_left(priv));
  EXPECT_THAT(std::span<const OPJ_BYTE>(out.data(), 3), ElementsAre(0, 1, 2));
  EXPECT_EQ(kBufferSentinel, out[3]);

  // Read 3 from buffer without invoking read_fn (leaves 2 bytes buffered).
  ASSERT_EQ(3u, opj_stream_read_data(priv, out.data() + 3, 3, &mgr));
  EXPECT_EQ(1u, state.read_calls);
  EXPECT_EQ(2u, priv->m_bytes_in_buffer);
  EXPECT_EQ(6, opj_stream_tell(priv));

  // Read exact remaining 2 buffered bytes (drains buffer to 0).
  ASSERT_EQ(2u, opj_stream_read_data(priv, out.data() + 6, 2, &mgr));
  EXPECT_EQ(1u, state.read_calls);
  EXPECT_EQ(0u, priv->m_bytes_in_buffer);
  EXPECT_EQ(8, opj_stream_tell(priv));

  // Read 4 when m_bytes_in_buffer == 0: resets m_current_data and refills.
  ASSERT_EQ(4u, opj_stream_read_data(priv, out.data() + 8, 4, &mgr));
  EXPECT_EQ(2u, state.read_calls);
  EXPECT_EQ(4u, priv->m_bytes_in_buffer);
  EXPECT_EQ(12, opj_stream_tell(priv));

  // Read 7 (4 buffered + 3 from next 8-byte chunk): leaves 5 buffered.
  ASSERT_EQ(7u, opj_stream_read_data(priv, out.data() + 12, 7, &mgr));
  EXPECT_EQ(3u, state.read_calls);
  EXPECT_EQ(5u, priv->m_bytes_in_buffer);
  EXPECT_EQ(19, opj_stream_tell(priv));

  // Read remaining 10 bytes (5 buffered + 5 from final chunk).
  ASSERT_EQ(10u, opj_stream_read_data(priv, out.data() + 19, 10, &mgr));
  EXPECT_EQ(4u, state.read_calls);
  EXPECT_EQ(0u, priv->m_bytes_in_buffer);
  EXPECT_EQ(29, opj_stream_tell(priv));
  EXPECT_EQ(0, opj_stream_get_number_byte_left(priv));
  EXPECT_THAT(std::span<const OPJ_BYTE>(out.data(), 29),
              ElementsAreArray(state.data));
  EXPECT_EQ(kBufferSentinel, out[29]);

  // Next read hits EOF and sets OPJ_STREAM_STATUS_END.
  EXPECT_EQ(kStreamReadWriteError,
            opj_stream_read_data(priv, out.data(), 1, &mgr));
  EXPECT_NE(0u, priv->m_status & OPJ_STREAM_STATUS_END);
  EXPECT_THAT(events.infos, ElementsAre("Stream reached its end !\n"));

  // 2. Direct-read path (p_size >= m_buffer_size), with and without buffered
  // prefix.
  ChunkedStreamState direct_state;
  direct_state.data = MakeSequenceBytes(24, /*start=*/10);
  OpjStreamPtr direct_stream =
      direct_state.CreateInputStream(/*buffer_size=*/8);
  ASSERT_NE(nullptr, direct_stream);
  opj_stream_private_t* const priv_direct = AsPrivate(direct_stream);

  std::vector<OPJ_BYTE> direct_out(24, kBufferSentinel);
  // Exact buffer_size read goes directly to destination buffer.
  ASSERT_EQ(8u, opj_stream_read_data(priv_direct, direct_out.data(), 8, &mgr));
  EXPECT_EQ(0u, priv_direct->m_bytes_in_buffer);
  EXPECT_THAT(direct_state.read_requests, ElementsAre(8u));

  // Read 3 (< 8) to buffer 5 bytes, then read 13 (5 buffered + 8 direct).
  ASSERT_EQ(3u,
            opj_stream_read_data(priv_direct, direct_out.data() + 8, 3, &mgr));
  EXPECT_EQ(5u, priv_direct->m_bytes_in_buffer);
  ASSERT_EQ(
      13u, opj_stream_read_data(priv_direct, direct_out.data() + 11, 13, &mgr));
  EXPECT_EQ(0u, priv_direct->m_bytes_in_buffer);
  EXPECT_EQ(24, opj_stream_tell(priv_direct));
  EXPECT_THAT(direct_state.read_requests, ElementsAre(8u, 8u, 8u));
  EXPECT_THAT(direct_out, ElementsAreArray(direct_state.data));
}

TEST(CioTest, ReadDataShortChunksPartialReadsAndEof) {
  EventCapture events;
  opj_event_mgr_t mgr = MakeEventManager(&events);

  // 1. Short chunks in buffered path (p_size < m_buffer_size), then partial EOF
  // and post-END read.
  {
    ChunkedStreamState state;
    state.data = MakeSequenceBytes(10, /*start=*/50);
    state.max_read_chunk = 3;
    OpjStreamPtr stream = state.CreateInputStream(/*buffer_size=*/16);
    ASSERT_NE(nullptr, stream);
    opj_stream_private_t* const priv = AsPrivate(stream);

    std::vector<OPJ_BYTE> out(12, kBufferSentinel);
    // Requests 8 bytes (< 16): callback returns 3, 3, 3 (leaving 1 buffered).
    ASSERT_EQ(8u, opj_stream_read_data(priv, out.data(), 8, &mgr));
    EXPECT_EQ(3u, state.read_calls);
    EXPECT_EQ(1u, priv->m_bytes_in_buffer);
    EXPECT_EQ(8, opj_stream_tell(priv));

    // Request 5 bytes: consumes 1 buffered + 1 final byte -> returns 2 on EOF.
    ASSERT_EQ(2u, opj_stream_read_data(priv, out.data() + 8, 5, &mgr));
    EXPECT_EQ(5u, state.read_calls);
    EXPECT_EQ(10, opj_stream_tell(priv));
    EXPECT_NE(0u, priv->m_status & OPJ_STREAM_STATUS_END);
    EXPECT_THAT(std::span<const OPJ_BYTE>(out.data(), 10),
                ElementsAreArray(state.data));
    EXPECT_EQ(kBufferSentinel, out[10]);

    // Reading again after OPJ_STREAM_STATUS_END returns -1 without read_fn.
    const size_t calls_before = state.read_calls;
    EXPECT_EQ(kStreamReadWriteError,
              opj_stream_read_data(priv, out.data(), 4, &mgr));
    EXPECT_EQ(calls_before, state.read_calls);
  }

  // 2. Short chunks in direct-read path transitioning into buffered tail + EOF.
  {
    ChunkedStreamState state;
    state.data = MakeSequenceBytes(12, /*start=*/70);
    state.max_read_chunk = 5;
    OpjStreamPtr stream = state.CreateInputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, stream);
    opj_stream_private_t* const priv = AsPrivate(stream);

    std::vector<OPJ_BYTE> out(16, kBufferSentinel);
    // Request 16 (>= 8): delivers 5 (rem=11 >= 8), 5 (rem=6 < 8), 2, then EOF.
    ASSERT_EQ(12u, opj_stream_read_data(priv, out.data(), 16, &mgr));
    EXPECT_THAT(state.read_requests, ElementsAre(16u, 11u, 8u, 8u));
    EXPECT_NE(0u, priv->m_status & OPJ_STREAM_STATUS_END);
    EXPECT_THAT(std::span<const OPJ_BYTE>(out.data(), 12),
                ElementsAreArray(state.data));
    EXPECT_EQ(kBufferSentinel, out[12]);
  }

  // 3. Direct-read path EOF with partial bytes and with zero bytes (plus null
  // event manager safety).
  {
    ChunkedStreamState partial_state;
    partial_state.data = MakeSequenceBytes(6);
    OpjStreamPtr partial_stream =
        partial_state.CreateInputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, partial_stream);
    std::vector<OPJ_BYTE> out(16, kBufferSentinel);
    ASSERT_EQ(6u, opj_stream_read_data(AsPrivate(partial_stream), out.data(),
                                       16, nullptr));
    EXPECT_THAT(partial_state.read_requests, ElementsAre(16u, 10u));
    EXPECT_NE(0u, AsPrivate(partial_stream)->m_status & OPJ_STREAM_STATUS_END);

    ChunkedStreamState empty_state;
    OpjStreamPtr empty_direct =
        empty_state.CreateInputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, empty_direct);
    EXPECT_EQ(
        kStreamReadWriteError,
        opj_stream_read_data(AsPrivate(empty_direct), out.data(), 8, nullptr));
    EXPECT_NE(0u, AsPrivate(empty_direct)->m_status & OPJ_STREAM_STATUS_END);

    OpjStreamPtr empty_buf = empty_state.CreateInputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, empty_buf);
    EXPECT_EQ(
        kStreamReadWriteError,
        opj_stream_read_data(AsPrivate(empty_buf), out.data(), 4, nullptr));
    EXPECT_NE(0u, AsPrivate(empty_buf)->m_status & OPJ_STREAM_STATUS_END);
  }

  // 4. Mid-stream read callback error on non-empty input sets END and returns
  // bytes read prior to the failing callback.
  {
    ChunkedStreamState err_state;
    err_state.data = MakeSequenceBytes(24);
    err_state.max_read_chunk = 3;
    err_state.fail_after_reads = 1;
    OpjStreamPtr err_stream = err_state.CreateInputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, err_stream);
    std::vector<OPJ_BYTE> out(8, kBufferSentinel);
    ASSERT_EQ(3u,
              opj_stream_read_data(AsPrivate(err_stream), out.data(), 6, &mgr));
    EXPECT_NE(0u, AsPrivate(err_stream)->m_status & OPJ_STREAM_STATUS_END);
    EXPECT_THAT(std::span<const OPJ_BYTE>(out.data(), 3), ElementsAre(0, 1, 2));
    EXPECT_EQ(kBufferSentinel, out[3]);
    EXPECT_EQ(kStreamReadWriteError,
              opj_stream_read_data(AsPrivate(err_stream), out.data(), 1, &mgr));
  }
}

TEST(CioTest, WriteDataAndFlushBufferedOverflowMultiChunkAndErrors) {
  EventCapture events;
  opj_event_mgr_t mgr = MakeEventManager(&events);

  // 1. Buffered, exact-fill, l_remaining_bytes == 0 overflow, and multi-chunk.
  {
    ChunkedStreamState state;
    OpjStreamPtr stream = state.CreateOutputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, stream);
    opj_stream_private_t* const priv = AsPrivate(stream);

    const std::vector<OPJ_BYTE> payload = MakeSequenceBytes(31, /*start=*/1);

    EXPECT_EQ(OPJ_TRUE, opj_stream_flush(priv, &mgr));
    EXPECT_EQ(0u, state.write_calls);
    EXPECT_EQ(0u, opj_stream_write_data(priv, payload.data(), 0, &mgr));
    EXPECT_EQ(0u, state.write_calls);

    // Write 5 bytes (buffered), then 3 bytes (exact buffer fill).
    ASSERT_EQ(5u, opj_stream_write_data(priv, payload.data(), 5, &mgr));
    EXPECT_EQ(0u, state.write_calls);
    EXPECT_EQ(5u, priv->m_bytes_in_buffer);

    ASSERT_EQ(3u, opj_stream_write_data(priv, payload.data() + 5, 3, &mgr));
    EXPECT_EQ(0u, state.write_calls);
    EXPECT_EQ(8u, priv->m_bytes_in_buffer);
    EXPECT_EQ(8, opj_stream_tell(priv));

    // Write 2 bytes when l_remaining_bytes == 0: flushes 8, buffers 2.
    ASSERT_EQ(2u, opj_stream_write_data(priv, payload.data() + 8, 2, &mgr));
    EXPECT_EQ(1u, state.write_calls);
    EXPECT_EQ(2u, priv->m_bytes_in_buffer);
    EXPECT_EQ(10, opj_stream_tell(priv));

    // Write 21 bytes (> 2 * buffer_size): triggers two flushes, buffers 7.
    ASSERT_EQ(21u, opj_stream_write_data(priv, payload.data() + 10, 21, &mgr));
    EXPECT_EQ(3u, state.write_calls);
    EXPECT_EQ(7u, priv->m_bytes_in_buffer);
    EXPECT_EQ(31, opj_stream_tell(priv));

    ASSERT_EQ(OPJ_TRUE, opj_stream_flush(priv, &mgr));
    EXPECT_EQ(4u, state.write_calls);
    EXPECT_EQ(0u, priv->m_bytes_in_buffer);
    EXPECT_THAT(state.write_requests, ElementsAre(8u, 8u, 8u, 7u));
    EXPECT_THAT(state.data, ElementsAreArray(payload));
  }

  // 2. Partial write callback loops inside opj_stream_flush.
  {
    ChunkedStreamState state;
    state.max_write_chunk = 3;
    OpjStreamPtr stream = state.CreateOutputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, stream);
    opj_stream_private_t* const priv = AsPrivate(stream);

    const std::vector<OPJ_BYTE> payload = MakeSequenceBytes(8, /*start=*/100);
    ASSERT_EQ(8u, opj_stream_write_data(priv, payload.data(), 8, &mgr));
    ASSERT_EQ(OPJ_TRUE, opj_stream_flush(priv, &mgr));
    EXPECT_THAT(state.write_requests, ElementsAre(8u, 5u, 2u));
    EXPECT_THAT(state.data, ElementsAreArray(payload));
  }

  // 3. Flush error after partial write sets OPJ_STREAM_STATUS_ERROR.
  {
    events.Clear();
    ChunkedStreamState state;
    state.max_write_chunk = 3;
    state.fail_after_writes = 1;
    OpjStreamPtr stream = state.CreateOutputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, stream);
    opj_stream_private_t* const priv = AsPrivate(stream);

    const std::vector<OPJ_BYTE> payload = MakeSequenceBytes(7);
    ASSERT_EQ(7u, opj_stream_write_data(priv, payload.data(), 7, &mgr));
    EXPECT_EQ(OPJ_FALSE, opj_stream_flush(priv, &mgr));
    EXPECT_NE(0u, priv->m_status & OPJ_STREAM_STATUS_ERROR);
    EXPECT_THAT(events.infos, ElementsAre("Error on writing stream!\n"));

    // Subsequent write_data calls fail immediately without calling write_fn.
    const size_t calls_before = state.write_calls;
    EXPECT_EQ(kStreamReadWriteError,
              opj_stream_write_data(priv, payload.data(), 0, &mgr));
    EXPECT_EQ(calls_before, state.write_calls);
  }

  // 4. Overflow flush failure inside opj_stream_write_data (MemWriteStream).
  {
    MemWriteStream mem_out;
    mem_out.max_bytes_before_fail = 4;
    OpjStreamPtr stream = mem_out.CreateStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, stream);
    const std::vector<OPJ_BYTE> payload = MakeSequenceBytes(10);
    EXPECT_EQ(kStreamReadWriteError,
              opj_stream_write_data(AsPrivate(stream), payload.data(),
                                    payload.size(), &mgr));
    EXPECT_NE(0u, AsPrivate(stream)->m_status & OPJ_STREAM_STATUS_ERROR);
  }
}

TEST(CioTest, ReadSkipInBufferCrossBufferSeekAndFailures) {
  EventCapture events;
  opj_event_mgr_t mgr = MakeEventManager(&events);

  // 1. In-buffer skip, exact drain, and chunked cross-buffer skip via skip_fn.
  {
    ChunkedStreamState state;
    state.data = MakeSequenceBytes(30);
    state.max_skip_chunk = 3;
    OpjStreamPtr stream = state.CreateInputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, stream);
    opj_stream_private_t* const priv = AsPrivate(stream);

    std::array<OPJ_BYTE, 4> byte_buf{};
    ASSERT_EQ(2u, opj_stream_read_data(priv, byte_buf.data(), 2, &mgr));
    EXPECT_THAT(std::span<const OPJ_BYTE>(byte_buf.data(), 2),
                ElementsAre(0, 1));
    EXPECT_EQ(6u, priv->m_bytes_in_buffer);

    // Zero skip is an in-buffer no-op.
    EXPECT_EQ(0, opj_stream_skip(priv, 0, &mgr));
    EXPECT_EQ(0u, state.skip_calls);
    EXPECT_EQ(2, opj_stream_tell(priv));

    // In-buffer skip (4 < 6), then exact drain (2 == 2).
    EXPECT_EQ(4, opj_stream_skip(priv, 4, &mgr));
    EXPECT_EQ(0u, state.skip_calls);
    EXPECT_EQ(2u, priv->m_bytes_in_buffer);
    EXPECT_EQ(6, opj_stream_tell(priv));

    EXPECT_EQ(2, opj_stream_skip(priv, 2, &mgr));
    EXPECT_EQ(0u, state.skip_calls);
    EXPECT_EQ(0u, priv->m_bytes_in_buffer);
    EXPECT_EQ(8, opj_stream_tell(priv));

    // Read 1 byte (buffers 7 bytes), then skip 14 (7 buffered + 7 via skip_fn
    // in chunks of 3, 3, 1).
    ASSERT_EQ(1u, opj_stream_read_data(priv, byte_buf.data(), 1, &mgr));
    EXPECT_EQ(8, byte_buf[0]);
    EXPECT_EQ(7u, priv->m_bytes_in_buffer);

    EXPECT_EQ(14, opj_stream_skip(priv, 14, &mgr));
    EXPECT_THAT(state.skip_requests, ElementsAre(7, 4, 1));
    EXPECT_EQ(0u, priv->m_bytes_in_buffer);
    EXPECT_EQ(23, opj_stream_tell(priv));
    EXPECT_EQ(7, opj_stream_get_number_byte_left(priv));

    ASSERT_EQ(1u, opj_stream_read_data(priv, byte_buf.data(), 1, &mgr));
    EXPECT_EQ(23, byte_buf[0]);
    EXPECT_EQ(6, opj_stream_read_skip(priv, 6, &mgr));
    EXPECT_EQ(30, opj_stream_tell(priv));
    EXPECT_EQ(0, opj_stream_get_number_byte_left(priv));
  }

  // 2. Cross-buffer skip from m_bytes_in_buffer == 0, then skip past
  // m_user_data_length via seek.
  {
    events.Clear();
    MemReadStream mem_in(MakeSequenceBytes(20));
    OpjStreamPtr stream = mem_in.CreateStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, stream);
    opj_stream_private_t* const priv = AsPrivate(stream);

    EXPECT_EQ(5, opj_stream_skip(priv, 5, &mgr));
    EXPECT_EQ(5, opj_stream_tell(priv));
    EXPECT_EQ(15, opj_stream_get_number_byte_left(priv));

    // Skipping past m_user_data_length invokes opj_stream_read_seek to end.
    EXPECT_EQ(15, opj_stream_skip(priv, 100, &mgr));
    EXPECT_NE(0u, priv->m_status & OPJ_STREAM_STATUS_END);
    EXPECT_EQ(20, opj_stream_tell(priv));
    EXPECT_EQ(0, opj_stream_get_number_byte_left(priv));
    EXPECT_THAT(events.infos, ElementsAre("Stream reached its end !\n"));

    // Subsequent skip when OPJ_STREAM_STATUS_END is set returns -1.
    EXPECT_EQ(kStreamSkipError, opj_stream_skip(priv, 1, &mgr));

    // Also verify skip at exact m_user_data_length before END is set -> -1.
    OpjStreamPtr exact_stream = mem_in.CreateStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, exact_stream);
    opj_stream_private_t* const priv_exact = AsPrivate(exact_stream);
    std::vector<OPJ_BYTE> full(20);
    ASSERT_EQ(20u, opj_stream_read_data(priv_exact, full.data(), 20, &mgr));
    EXPECT_EQ(0u, priv_exact->m_status & OPJ_STREAM_STATUS_END);
    EXPECT_EQ(kStreamSkipError, opj_stream_skip(priv_exact, 1, &mgr));
    EXPECT_NE(0u, priv_exact->m_status & OPJ_STREAM_STATUS_END);

    // Pin upstream quirk in opj_stream_read_skip() (`the flag is not set, we
    // copy data and then do an actual skip` branch): when buffered bytes exist
    // before skipping beyond m_user_data_length, m_byte_offset advances to 20
    // and END is set, while the returned skip count omits the 7 buffered bytes
    // (12).
    OpjStreamPtr quirk_stream = mem_in.CreateStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, quirk_stream);
    opj_stream_private_t* const priv_quirk = AsPrivate(quirk_stream);
    OPJ_BYTE first = 0;
    ASSERT_EQ(1u, opj_stream_read_data(priv_quirk, &first, 1, &mgr));
    EXPECT_EQ(7u, priv_quirk->m_bytes_in_buffer);
    EXPECT_EQ(12, opj_stream_skip(priv_quirk, 50, &mgr));
    EXPECT_EQ(20, opj_stream_tell(priv_quirk));
    EXPECT_NE(0u, priv_quirk->m_status & OPJ_STREAM_STATUS_END);
  }

  // 3. Unseekable skip past end and skip_fn failure (immediate and partial).
  {
    MemReadStream unseekable_in(MakeSequenceBytes(20));
    unseekable_in.allow_seek = false;
    OpjStreamPtr unseekable = unseekable_in.CreateStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, unseekable);
    opj_stream_private_t* const priv_unseek = AsPrivate(unseekable);
    EXPECT_EQ(OPJ_FALSE, opj_stream_has_seek(priv_unseek));
    EXPECT_EQ(5, opj_stream_skip(priv_unseek, 5, &mgr));
    EXPECT_EQ(15, opj_stream_skip(priv_unseek, 50, &mgr));
    EXPECT_NE(0u, priv_unseek->m_status & OPJ_STREAM_STATUS_END);

    // Buffered + partial skip before skip_fn failure returns accumulated count.
    ChunkedStreamState partial_fail;
    partial_fail.data = MakeSequenceBytes(15);
    partial_fail.max_skip_chunk = 3;
    OpjStreamPtr stream = partial_fail.CreateInputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, stream);
    opj_stream_private_t* const priv = AsPrivate(stream);
    // Inflate declared length so read_skip invokes m_skip_fn past data.size().
    opj_stream_set_user_data_length(stream.get(), 100);

    std::array<OPJ_BYTE, 2> head{};
    ASSERT_EQ(2u, opj_stream_read_data(priv, head.data(), 2, &mgr));
    // 6 buffered + (3 + 3 + 1) skipped before skip_fn returns -1 -> 13.
    EXPECT_EQ(13, opj_stream_read_skip(priv, 20, &mgr));
    EXPECT_NE(0u, priv->m_status & OPJ_STREAM_STATUS_END);
    EXPECT_EQ(15, opj_stream_tell(priv));

    // Direct skip failure with 0 bytes skipped returns -1.
    ChunkedStreamState immediate_fail;
    immediate_fail.data = MakeSequenceBytes(4);
    immediate_fail.fail_after_skips = 0;
    OpjStreamPtr fail_stream =
        immediate_fail.CreateInputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, fail_stream);
    opj_stream_set_user_data_length(fail_stream.get(), 100);
    EXPECT_EQ(kStreamSkipError,
              opj_stream_read_skip(AsPrivate(fail_stream), 10, &mgr));
    EXPECT_NE(0u, AsPrivate(fail_stream)->m_status & OPJ_STREAM_STATUS_END);
  }
}

TEST(CioTest, WriteSkipFlushChunkedAndFailurePaths) {
  EventCapture events;
  opj_event_mgr_t mgr = MakeEventManager(&events);

  // 1. Write skip flushes buffered data, skips in chunks, and handles 0 skip.
  {
    ChunkedStreamState state;
    state.max_skip_chunk = 3;
    OpjStreamPtr stream = state.CreateOutputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, stream);
    opj_stream_private_t* const priv = AsPrivate(stream);

    const std::vector<OPJ_BYTE> abcd = BytesFromText("ABCD");
    ASSERT_EQ(4u, opj_stream_write_data(priv, abcd.data(), abcd.size(), &mgr));
    EXPECT_EQ(0u, state.write_calls);

    EXPECT_EQ(5, opj_stream_skip(priv, 5, &mgr));
    EXPECT_EQ(1u, state.write_calls);
    EXPECT_THAT(state.skip_requests, ElementsAre(5, 2));
    EXPECT_EQ(0u, priv->m_bytes_in_buffer);
    EXPECT_EQ(9, opj_stream_tell(priv));

    const std::vector<OPJ_BYTE> xy = BytesFromText("XY");
    ASSERT_EQ(2u, opj_stream_write_data(priv, xy.data(), xy.size(), &mgr));
    // Zero write skip flushes buffered data without calling skip_fn.
    EXPECT_EQ(0, opj_stream_write_skip(priv, 0, &mgr));
    EXPECT_EQ(2u, state.write_calls);
    EXPECT_EQ(2u, state.skip_calls);
    EXPECT_EQ(11, opj_stream_tell(priv));
    EXPECT_THAT(state.data,
                ElementsAre('A', 'B', 'C', 'D', 0, 0, 0, 0, 0, 'X', 'Y'));
  }

  // 2. Flush failure inside opj_stream_write_skip aborts before skip_fn.
  {
    ChunkedStreamState state;
    state.fail_after_writes = 0;
    OpjStreamPtr stream = state.CreateOutputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, stream);
    opj_stream_private_t* const priv = AsPrivate(stream);

    const std::vector<OPJ_BYTE> ab = BytesFromText("AB");
    ASSERT_EQ(2u, opj_stream_write_data(priv, ab.data(), ab.size(), &mgr));
    EXPECT_EQ(kStreamSkipError, opj_stream_skip(priv, 4, &mgr));
    EXPECT_EQ(0u, state.skip_calls);
    EXPECT_EQ(0u, priv->m_bytes_in_buffer);
    EXPECT_NE(0u, priv->m_status & OPJ_STREAM_STATUS_ERROR);

    // Subsequent write_skip immediately returns -1 on OPJ_STREAM_STATUS_ERROR.
    const size_t writes_before = state.write_calls;
    EXPECT_EQ(kStreamSkipError, opj_stream_write_skip(priv, 4, &mgr));
    EXPECT_EQ(writes_before, state.write_calls);
    EXPECT_EQ(0u, state.skip_calls);
  }

  // 3. Immediate and partial skip_fn failure inside opj_stream_write_skip.
  {
    events.Clear();
    ChunkedStreamState immediate_state;
    immediate_state.fail_after_skips = 0;
    OpjStreamPtr imm_stream =
        immediate_state.CreateOutputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, imm_stream);
    opj_stream_private_t* const priv_imm = AsPrivate(imm_stream);

    const std::vector<OPJ_BYTE> ab = BytesFromText("AB");
    ASSERT_EQ(2u, opj_stream_write_data(priv_imm, ab.data(), ab.size(), &mgr));
    EXPECT_EQ(kStreamSkipError, opj_stream_write_skip(priv_imm, 4, &mgr));
    EXPECT_NE(0u, priv_imm->m_status & OPJ_STREAM_STATUS_ERROR);
    EXPECT_EQ(2, opj_stream_tell(priv_imm));
    EXPECT_THAT(events.infos, ElementsAre("Stream error!\n"));

    events.Clear();
    ChunkedStreamState partial_state;
    partial_state.max_skip_chunk = 3;
    partial_state.fail_after_skips = 1;
    OpjStreamPtr part_stream =
        partial_state.CreateOutputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, part_stream);
    opj_stream_private_t* const priv_part = AsPrivate(part_stream);

    EXPECT_EQ(3, opj_stream_write_skip(priv_part, 7, &mgr));
    EXPECT_NE(0u, priv_part->m_status & OPJ_STREAM_STATUS_ERROR);
    EXPECT_EQ(3, opj_stream_tell(priv_part));
    EXPECT_THAT(events.infos, ElementsAre("Stream error!\n"));
  }
}

TEST(CioTest, ReadSeekWriteSeekTellAndBytesLeft) {
  EventCapture events;
  opj_event_mgr_t mgr = MakeEventManager(&events);

  // 1. opj_stream_get_number_byte_left returns 0 when m_user_data_length == 0.
  {
    OpjStreamPtr empty_len(opj_stream_create(8, OPJ_STREAM_READ));
    ASSERT_NE(nullptr, empty_len);
    EXPECT_EQ(0, opj_stream_tell(AsPrivate(empty_len)));
    EXPECT_EQ(0, opj_stream_get_number_byte_left(AsPrivate(empty_len)));
  }

  // 2. Input stream seek discards buffered data, clears END, and handles fail.
  {
    MemReadStream mem_in(MakeSequenceBytes(16));
    OpjStreamPtr stream = mem_in.CreateStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, stream);
    opj_stream_private_t* const priv = AsPrivate(stream);

    EXPECT_EQ(OPJ_TRUE, opj_stream_has_seek(priv));
    EXPECT_EQ(16, opj_stream_get_number_byte_left(priv));

    std::array<OPJ_BYTE, 12> buf{};
    ASSERT_EQ(3u, opj_stream_read_data(priv, buf.data(), 3, &mgr));
    EXPECT_EQ(5u, priv->m_bytes_in_buffer);
    EXPECT_EQ(3, opj_stream_tell(priv));
    EXPECT_EQ(13, opj_stream_get_number_byte_left(priv));

    // Seek forward discards remaining 5 buffered bytes.
    ASSERT_EQ(OPJ_TRUE, opj_stream_seek(priv, 10, &mgr));
    EXPECT_EQ(0u, priv->m_bytes_in_buffer);
    EXPECT_EQ(10, opj_stream_tell(priv));
    EXPECT_EQ(6, opj_stream_get_number_byte_left(priv));

    // Read to EOF and set OPJ_STREAM_STATUS_END.
    ASSERT_EQ(6u, opj_stream_read_data(priv, buf.data(), 10, &mgr));
    EXPECT_NE(0u, priv->m_status & OPJ_STREAM_STATUS_END);
    EXPECT_EQ(16, opj_stream_tell(priv));
    EXPECT_EQ(0, opj_stream_get_number_byte_left(priv));

    // Seek backwards clears OPJ_STREAM_STATUS_END and restores readability.
    ASSERT_EQ(OPJ_TRUE, opj_stream_read_seek(priv, 4, &mgr));
    EXPECT_EQ(0u, priv->m_status & OPJ_STREAM_STATUS_END);
    EXPECT_EQ(4, opj_stream_tell(priv));
    EXPECT_EQ(12, opj_stream_get_number_byte_left(priv));

    ASSERT_EQ(2u, opj_stream_read_data(priv, buf.data(), 2, &mgr));
    EXPECT_THAT(std::span<const OPJ_BYTE>(buf.data(), 2), ElementsAre(4, 5));

    // Out-of-bounds and negative read_seek fail, set END, and keep offset.
    EXPECT_EQ(OPJ_FALSE, opj_stream_seek(priv, 100, &mgr));
    EXPECT_NE(0u, priv->m_status & OPJ_STREAM_STATUS_END);
    EXPECT_EQ(6, opj_stream_tell(priv));
    EXPECT_EQ(OPJ_FALSE, opj_stream_read_seek(priv, -1, &mgr));
    EXPECT_NE(0u, priv->m_status & OPJ_STREAM_STATUS_END);
    EXPECT_EQ(6, opj_stream_tell(priv));
  }

  // 3. Output stream seek flushes buffered bytes, patches earlier bytes, and
  // handles flush and seek failures.
  {
    MemWriteStream mem_out;
    OpjStreamPtr stream = mem_out.CreateStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, stream);
    opj_stream_private_t* const priv = AsPrivate(stream);

    const std::vector<OPJ_BYTE> initial = BytesFromText("HELLO_WORLD");
    ASSERT_EQ(initial.size(), opj_stream_write_data(priv, initial.data(),
                                                    initial.size(), &mgr));
    EXPECT_EQ(3u, priv->m_bytes_in_buffer);

    ASSERT_EQ(OPJ_TRUE, opj_stream_seek(priv, 6, &mgr));
    EXPECT_EQ(0u, priv->m_bytes_in_buffer);
    EXPECT_EQ(6, opj_stream_tell(priv));

    const std::vector<OPJ_BYTE> patch = BytesFromText("OPENJ");
    ASSERT_EQ(patch.size(),
              opj_stream_write_data(priv, patch.data(), patch.size(), &mgr));
    ASSERT_EQ(OPJ_TRUE, opj_stream_flush(priv, &mgr));
    EXPECT_EQ(11, opj_stream_tell(priv));
    EXPECT_THAT(mem_out.data, ElementsAreArray(BytesFromText("HELLO_OPENJ")));

    // Seek failure sets OPJ_STREAM_STATUS_ERROR without advancing offset.
    mem_out.fail_seek = true;
    EXPECT_EQ(OPJ_FALSE, opj_stream_write_seek(priv, 2, &mgr));
    EXPECT_NE(0u, priv->m_status & OPJ_STREAM_STATUS_ERROR);
    EXPECT_EQ(11, opj_stream_tell(priv));

    // Flush failure inside opj_stream_write_seek aborts before calling seek_fn.
    ChunkedStreamState fail_state;
    fail_state.fail_after_writes = 0;
    OpjStreamPtr fail_stream = fail_state.CreateOutputStream(/*buffer_size=*/8);
    ASSERT_NE(nullptr, fail_stream);
    opj_stream_private_t* const priv_fail = AsPrivate(fail_stream);
    const std::vector<OPJ_BYTE> ab = BytesFromText("AB");
    ASSERT_EQ(2u, opj_stream_write_data(priv_fail, ab.data(), ab.size(), &mgr));
    EXPECT_EQ(OPJ_FALSE, opj_stream_seek(priv_fail, 0, &mgr));
    EXPECT_EQ(0u, fail_state.seek_calls);
    EXPECT_NE(0u, priv_fail->m_status & OPJ_STREAM_STATUS_ERROR);
  }
}

// TODO(schwehr): Cover both opj_stream_create() OOM branches (`!l_stream`
// opj_calloc and `!l_stream->m_stored_data` opj_malloc) via
// :openjpeg2_internal_no_malloc and ScopedOpjAllocFailCountdown.

}  // namespace
}  // namespace openjpeg2
