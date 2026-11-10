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
// Tests libaec streaming and buffer encoder (third_party/libaec/src/encode.c).
// Note: ENABLE_RSI_PADDING is disabled in google3, so AEC_PAD_RSI is a no-op
// during encoding. Never include src/decode.h here (colliding internal_state).
//
// See also:
//   https://gitlab.dkrz.de/k202009/libaec/-/blob/main/src/encode.c
//   https://gitlab.dkrz.de/k202009/libaec/-/blob/main/src/encode.h

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>  // NOLINT
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/libaec/include/libaec.h"

// clang-format off
extern "C" {
#include "third_party/libaec/src/encode.h"
}  // extern "C"
// clang-format on

namespace libaec {
namespace {

using ::testing::Each;
using ::testing::ElementsAreArray;
using ::testing::IsEmpty;
using ::testing::Not;

static_assert(AEC_OK == 0);
// Pin frozen include/libaec.h version macros (excluded in copy.bara.sky) and
// disabled ENABLE_RSI_PADDING build configuration in google3.
static_assert(AEC_VERSION_MAJOR == 1);
static_assert(AEC_VERSION_MINOR == 1);
static_assert(AEC_VERSION_PATCH == 3);
static_assert(std::string_view(AEC_VERSION_STR) == "1.1.3");
#ifdef ENABLE_RSI_PADDING
#error "Expected ENABLE_RSI_PADDING to be disabled in google3."
#endif

// Encodes input with aec_buffer_encode and verifies lossless decode via the
// public aec_buffer_decode API. Allocates generous output capacity so
// direct_out copy64() writes stay in bounds, then slices to enc.total_out.
void VerifyRoundTrip(unsigned int bits_per_sample, unsigned int block_size,
                     unsigned int rsi, unsigned int flags,
                     std::span<const uint8_t> input) {
  std::vector<uint8_t> encoded(input.size() * 2 + 128, 0xA5);
  aec_stream enc{};
  enc.bits_per_sample = bits_per_sample;
  enc.block_size = block_size;
  enc.rsi = rsi;
  enc.flags = flags;
  enc.next_in = input.data();
  enc.avail_in = input.size();
  enc.next_out = encoded.data();
  enc.avail_out = encoded.size();
  ASSERT_EQ(AEC_OK, aec_buffer_encode(&enc));

  const std::span<const uint8_t> compressed =
      std::span<const uint8_t>(encoded).first(enc.total_out);
  EXPECT_THAT(compressed, Not(IsEmpty()));

  constexpr size_t kTrail = 16;
  std::vector<uint8_t> decoded(input.size() + kTrail, 0xA5);
  aec_stream dec{};
  dec.bits_per_sample = bits_per_sample;
  dec.block_size = block_size;
  dec.rsi = rsi;
  dec.flags = flags;
  dec.next_in = compressed.data();
  dec.avail_in = compressed.size();
  dec.next_out = decoded.data();
  dec.avail_out = input.size();
  ASSERT_EQ(AEC_OK, aec_buffer_decode(&dec));
  EXPECT_EQ(input.size(), dec.total_out);
  EXPECT_THAT(std::span<const uint8_t>(decoded).first(input.size()),
              ElementsAreArray(input));
  EXPECT_THAT(std::span<const uint8_t>(decoded).subspan(input.size()),
              Each(0xA5));
}

uint32_t NextPseudorandom(uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  return state;
}

TEST(EncodeTest, InitValidatesConfigurationParameters) {
  struct InitCase {
    unsigned int bits_per_sample;
    unsigned int block_size;
    unsigned int rsi;
    unsigned int flags;
    int expected_status;
    int expected_id_len;
  };
  constexpr auto kCases = std::to_array<InitCase>({
      // Invalid bits_per_sample and rsi bounds.
      {.bits_per_sample = 0,
       .block_size = 8,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 33,
       .block_size = 8,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 8,
       .block_size = 8,
       .rsi = 0,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 8,
       .block_size = 8,
       .rsi = 4097,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      // Standard block size enforcement (must be 8, 16, 32, or 64).
      {.bits_per_sample = 8,
       .block_size = 10,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 8,
       .block_size = 128,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      // Non-enforced block size validation (even, 2..256).
      {.bits_per_sample = 8,
       .block_size = 0,
       .rsi = 1,
       .flags = AEC_NOT_ENFORCE,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 8,
       .block_size = 3,
       .rsi = 1,
       .flags = AEC_NOT_ENFORCE,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 8,
       .block_size = 258,
       .rsi = 1,
       .flags = AEC_NOT_ENFORCE,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      // AEC_RESTRICTED allows 1..4 bits, rejects 5..8, and is ignored >8.
      {.bits_per_sample = 1,
       .block_size = 8,
       .rsi = 1,
       .flags = AEC_RESTRICTED,
       .expected_status = AEC_OK,
       .expected_id_len = 1},
      {.bits_per_sample = 4,
       .block_size = 8,
       .rsi = 1,
       .flags = AEC_RESTRICTED,
       .expected_status = AEC_OK,
       .expected_id_len = 2},
      {.bits_per_sample = 5,
       .block_size = 8,
       .rsi = 1,
       .flags = AEC_RESTRICTED,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 8,
       .block_size = 8,
       .rsi = 1,
       .flags = AEC_RESTRICTED,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 12,
       .block_size = 8,
       .rsi = 1,
       .flags = AEC_RESTRICTED,
       .expected_status = AEC_OK,
       .expected_id_len = 4},
      // Standard valid block sizes and max non-enforced size.
      {.bits_per_sample = 8,
       .block_size = 16,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_OK,
       .expected_id_len = 3},
      {.bits_per_sample = 8,
       .block_size = 32,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_OK,
       .expected_id_len = 3},
      {.bits_per_sample = 8,
       .block_size = 64,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_OK,
       .expected_id_len = 3},
      {.bits_per_sample = 24,
       .block_size = 256,
       .rsi = 1,
       .flags = AEC_NOT_ENFORCE | AEC_DATA_3BYTE | AEC_DATA_MSB,
       .expected_status = AEC_OK,
       .expected_id_len = 5},
  });

  for (size_t i = 0; i < kCases.size(); ++i) {
    const InitCase& tc = kCases[i];
    SCOPED_TRACE(i);
    aec_stream enc{};
    enc.bits_per_sample = tc.bits_per_sample;
    enc.block_size = tc.block_size;
    enc.rsi = tc.rsi;
    enc.flags = tc.flags;
    const int status = aec_encode_init(&enc);
    EXPECT_EQ(tc.expected_status, status);
    if (status == AEC_OK) {
      // Only register aec_encode_end after AEC_OK; failed init frees state
      // without clearing enc.state.
      absl::Cleanup end_stream = [&enc] {
        EXPECT_EQ(AEC_OK, aec_encode_end(&enc));
      };
      EXPECT_EQ(tc.expected_id_len, enc.state->id_len);
    }
    if (tc.expected_status != AEC_OK) {
      EXPECT_EQ(tc.expected_status, aec_buffer_encode(&enc));
    }
  }
}

TEST(EncodeTest, PreprocessUnsignedCoversAllDeltaBranches) {
  // Craft consecutive 8-bit unsigned samples hitting all four branches of
  // preprocess_unsigned():
  //   1. x[1] >= x[0], D <= x[0]  (100 -> 120)
  //   2. x[1] >= x[0], D > x[0]   (10  -> 200)
  //   3. x[1] <  x[0], D <= xmax - x[0] (100 -> 50)
  //   4. x[1] <  x[0], D >  xmax - x[0] (240 -> 10)
  constexpr auto kInput = std::to_array<uint8_t>({
      100,
      120,
      10,
      200,
      100,
      50,
      240,
      10,
      10,
      10,
      20,
      5,
      250,
      2,
      128,
      129,
  });
  VerifyRoundTrip(8, 8, 2, AEC_DATA_PREPROCESS, kInput);
}

TEST(EncodeTest, PreprocessSignedCoversAllDeltaBranches) {
  // Craft consecutive 8-bit signed samples hitting all four branches of
  // preprocess_signed():
  //   1. x[1] <  x[0], D <= xmax - x[0] (10   -> -20)
  //   2. x[1] <  x[0], D >  xmax - x[0] (120  -> -50)
  //   3. x[1] >= x[0], D <= x[0] - xmin (-10  -> 20)
  //   4. x[1] >= x[0], D >  x[0] - xmin (-120 -> 50)
  constexpr auto kSigned = std::to_array<int8_t>({
      10,
      -20,
      120,
      -50,
      -10,
      20,
      -120,
      50,
      50,
      50,
      -5,
      -40,
      125,
      -1,
      -127,
      127,
  });
  constexpr auto kBytes =
      std::bit_cast<std::array<uint8_t, kSigned.size()>>(kSigned);
  VerifyRoundTrip(8, 8, 2, AEC_DATA_SIGNED | AEC_DATA_PREPROCESS, kBytes);
}

TEST(EncodeTest, RoundTripsAllStorageWidthsAndByteOrders) {
  uint32_t rng = 0x12345678u;
  constexpr size_t kSamples = 32;  // 2 blocks * 2 RSI, block_size = 8.

  // 1. 16-bit LSB and MSB (both preprocessed smooth ramp and high-entropy).
  {
    std::vector<uint8_t> smooth_lsb(kSamples * 2);
    std::vector<uint8_t> noise_msb(kSamples * 2);
    for (size_t i = 0; i < kSamples; ++i) {
      const uint16_t s = static_cast<uint16_t>(1000 + i * 3);
      smooth_lsb[2 * i] = static_cast<uint8_t>(s & 0xFFu);
      smooth_lsb[2 * i + 1] = static_cast<uint8_t>((s >> 8) & 0xFFu);
      const uint16_t n = static_cast<uint16_t>(NextPseudorandom(rng) >> 16);
      noise_msb[2 * i] = static_cast<uint8_t>((n >> 8) & 0xFFu);
      noise_msb[2 * i + 1] = static_cast<uint8_t>(n & 0xFFu);
    }
    VerifyRoundTrip(16, 8, 2, AEC_DATA_PREPROCESS, smooth_lsb);
    VerifyRoundTrip(16, 8, 2, AEC_DATA_MSB | AEC_DATA_SIGNED, noise_msb);
  }

  // 2. 24-bit 3-byte LSB and MSB (exercise emit() multi-byte loop and uncomp).
  {
    std::vector<uint8_t> lsb_24(kSamples * 3);
    std::vector<uint8_t> msb_24(kSamples * 3);
    for (size_t i = 0; i < kSamples; ++i) {
      const uint32_t v = (i < 16) ? (0x400000u + static_cast<uint32_t>(i) * 19u)
                                  : (NextPseudorandom(rng) & 0xFFFFFFu);
      lsb_24[3 * i] = static_cast<uint8_t>(v & 0xFFu);
      lsb_24[3 * i + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
      lsb_24[3 * i + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
      msb_24[3 * i] = static_cast<uint8_t>((v >> 16) & 0xFFu);
      msb_24[3 * i + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
      msb_24[3 * i + 2] = static_cast<uint8_t>(v & 0xFFu);
    }
    VerifyRoundTrip(24, 8, 2, AEC_DATA_3BYTE | AEC_DATA_PREPROCESS, lsb_24);
    VerifyRoundTrip(
        24, 8, 2, AEC_DATA_3BYTE | AEC_DATA_MSB | AEC_DATA_PREPROCESS, msb_24);
  }

  // 3. 32-bit 4-byte LSB and MSB (hits emit() >8 bit loop and high-k
  // splitting).
  {
    std::vector<uint8_t> lsb_32(kSamples * 4);
    std::vector<uint8_t> msb_32(kSamples * 4);
    for (size_t i = 0; i < kSamples; ++i) {
      const uint32_t v = (i < 16)
                             ? (0x7FFFFF00u + static_cast<uint32_t>(i) * 5u)
                             : NextPseudorandom(rng);
      lsb_32[4 * i] = static_cast<uint8_t>(v & 0xFFu);
      lsb_32[4 * i + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
      lsb_32[4 * i + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
      lsb_32[4 * i + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
      msb_32[4 * i] = static_cast<uint8_t>((v >> 24) & 0xFFu);
      msb_32[4 * i + 1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
      msb_32[4 * i + 2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
      msb_32[4 * i + 3] = static_cast<uint8_t>(v & 0xFFu);
    }
    VerifyRoundTrip(32, 8, 2, AEC_DATA_PREPROCESS, lsb_32);
    VerifyRoundTrip(32, 8, 2, AEC_DATA_MSB | AEC_DATA_SIGNED, msb_32);
  }
}

TEST(EncodeTest, ZeroBlocksTriggerShortLongAndRosRuns) {
  // 1. Short zero runs (1..4 blocks) + non-zero block, plus >=5 zero blocks +
  // non-zero block inside one RSI of 16 blocks (block_size = 8).
  {
    std::vector<uint8_t> input(16 * 8, 0);
    // Block 0: zeros (1 block before non-zero block 1).
    // Block 1: non-zero (triggers m_encode_zero with 1 block, then
    // block_nonzero).
    std::fill_n(input.begin() + 8, 8, 0x55);
    // Blocks 2..4: 3 zero blocks.
    // Block 5: non-zero (triggers 3 zero blocks).
    std::fill_n(input.begin() + 5 * 8, 8, 0x33);
    // Blocks 6..11: 6 zero blocks (>= 5).
    // Block 12: non-zero (triggers zero_blocks >= 5 path + emitfs crossing
    // byte).
    std::fill_n(input.begin() + 12 * 8, 8, 0x77);
    // Blocks 13..15: 3 trailing zero blocks at end of RSI.
    VerifyRoundTrip(8, 8, 16, 0, input);
  }

  // 2. ROS at segment boundary (blocks_dispensed % 64 == 0) with >4 zeros, and
  // ROS at end of RSI with reference sample preserved (AEC_DATA_PREPROCESS).
  {
    // 70 blocks of 8 samples = 560 samples.
    // First 64 blocks = constant 42 -> deltas are 0 -> segment boundary hits
    // zero_blocks == ROS with zero_ref == 1!
    // Remaining 6 blocks = zeros (different constant) -> hits end-of-RSI ROS.
    std::vector<uint8_t> input(70 * 8, 42);
    VerifyRoundTrip(8, 8, 70, AEC_DATA_PREPROCESS, input);
  }

  // 3. Long unpreprocessed zero run exceeding 16-bit emitfs with >4 ending ROS.
  {
    std::vector<uint8_t> input(8 * 8, 0);
    VerifyRoundTrip(8, 8, 8, 0, input);
  }
}

TEST(EncodeTest, SecondExtensionAndRestrictedOneBitModes) {
  // 1. 2-bit restricted mode forces id_len == 1, disabling splitting option
  // (split_len = UINT32_MAX) and selecting SE vs uncomp directly.
  {
    std::vector<uint8_t> alternating;
    // Block 0 (low entropy pairs 0/1) favors SE; Block 1 (high values 2/3)
    // favors uncomp.
    for (int i = 0; i < 8; ++i) {
      alternating.push_back(static_cast<uint8_t>(i & 1));
    }
    for (int i = 0; i < 8; ++i) {
      alternating.push_back(static_cast<uint8_t>(2 + (i & 1)));
    }
    VerifyRoundTrip(2, 8, 2, AEC_RESTRICTED, alternating);
    // Block 0 preprocessed has a single non-zero delta ({0,0,0,0,0,0,0,1}),
    // forcing m_encode_se() with state->ref == 1.
    constexpr auto kSeRefBlock = std::to_array<uint8_t>(
        {2, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2});
    VerifyRoundTrip(2, 8, 2, AEC_RESTRICTED | AEC_DATA_PREPROCESS, kSeRefBlock);
  }

  // 2. Standard 8-bit alternating {0, 1} pairs trigger Second Extension with
  // multi-byte emitfs when pair sums produce larger FS codes (e.g., {1, 2}).
  {
    std::vector<uint8_t> se_data;
    for (int i = 0; i < 8; ++i) {
      se_data.push_back(static_cast<uint8_t>((i & 1) ? 2 : 1));
    }
    for (int i = 0; i < 8; ++i) {
      se_data.push_back(static_cast<uint8_t>(i & 1));
    }
    VerifyRoundTrip(8, 8, 2, 0, se_data);
  }
}

TEST(EncodeTest, SplittingSearchTurnaroundAndEmitBlockCases) {
  // 1. Craft consecutive 16-bit blocks (no preprocessing) to walk splitting
  // search up (dir = 1), reverse on exact tie/increase (dir = 0, no_turn = 0),
  // step down (k--), and emit blocks across diverse k values (hitting switch
  // cases 0..48 and default in emitblock, plus multi-word emitblock_fs).
  constexpr unsigned int kBlockSize = 8;
  constexpr unsigned int kRsi = 12;
  std::vector<uint8_t> samples;
  auto push16 = [&samples](uint16_t v) {
    samples.push_back(static_cast<uint8_t>(v & 0xFFu));
    samples.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  };
  // Block 0: values = 10 pushes optimal k from 0 upward to k = 2.
  for (unsigned int i = 0; i < kBlockSize; ++i) push16(10);
  // Block 1: values = 4; at previous k = 2, fs_len == 8 == this_bs, len(2)=32,
  // len(3)=32 (len >= len_min with !no_turn -> reverses in
  // assess_splitting_option()).
  for (unsigned int i = 0; i < kBlockSize; ++i) push16(4);
  // Block 2: values = 4096 (high k = 11..12).
  for (unsigned int i = 0; i < kBlockSize; ++i) push16(4096);
  // Block 3: values = 1; fs_len at k = 11 is 0 < this_bs -> reverses in
  // assess_splitting_option() and steps k-- down to 0.
  for (unsigned int i = 0; i < kBlockSize; ++i) push16(1);
  // Blocks 4..10: escalating shifted patterns hitting distinct k values and
  // switch (p & ~7) remainders in emitblock().
  for (uint16_t val : {uint16_t{2}, uint16_t{6}, uint16_t{14}, uint16_t{30},
                       uint16_t{62}, uint16_t{126}, uint16_t{254}}) {
    for (unsigned int i = 0; i < kBlockSize; ++i) {
      push16(static_cast<uint16_t>(val + (i & 1)));
    }
  }
  // Block 11: large values near 65000 with one low-bit outlier -> forces high
  // FS total per block (> 63 bits) to exercise emitblock_fs copy64() loop.
  for (unsigned int i = 0; i < kBlockSize; ++i) {
    push16(static_cast<uint16_t>(65000u + i));
  }
  VerifyRoundTrip(16, kBlockSize, kRsi, 0, samples);

  // 2. Large block_size = 64 with varied k to hit all switch (p & ~7) paths and
  // emitblock_fs while(used > 63) across 32-bit samples.
  std::vector<uint8_t> wide_samples;
  for (uint32_t shift = 1; shift <= 15; ++shift) {
    for (int i = 0; i < 64; ++i) {
      const uint32_t v = (uint32_t{1} << shift) +
                         static_cast<uint32_t>((i * 3) & ((1u << shift) - 1u));
      wide_samples.push_back(static_cast<uint8_t>(v & 0xFFu));
      wide_samples.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
      wide_samples.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
      wide_samples.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    }
  }
  VerifyRoundTrip(32, 64, 15, 0, wide_samples);

  // 3. Non-enforced minimal block_size = 4 with k = 1 exercises small-p
  // emitblock() default branch (p >= 56 after emitting k LSBs).
  constexpr auto kSmallSplit = std::to_array<uint8_t>({2, 3, 2, 2});
  VerifyRoundTrip(8, 4, 1, AEC_NOT_ENFORCE, kSmallSplit);
}

TEST(EncodeTest, StreamingOneByteAtATimeAndPartialRsiFlush) {
  // Construct 27 samples (1 full RSI of 16 samples + 11 samples = 1 block +
  // 3 partial samples) to test:
  //   - m_get_rsi_resumable waiting on avail_in = 1
  //   - m_flush_block_resumable with avail_out = 1 (direct_out = 0)
  //   - switching between buffered and direct_out within the same stream
  //   - partial final RSI padding on AEC_FLUSH (i % block_size != 0)
  std::vector<uint8_t> input(27);
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<uint8_t>(10 + (i * 5) % 200);
  }

  aec_stream enc{};
  enc.bits_per_sample = 8;
  enc.block_size = 8;
  enc.rsi = 2;
  enc.flags = AEC_DATA_PREPROCESS;
  ASSERT_EQ(AEC_OK, aec_encode_init(&enc));
  absl::Cleanup end_stream = [&enc] {
    EXPECT_EQ(AEC_OK, aec_encode_end(&enc));
  };

  std::vector<uint8_t> encoded;
  std::array<uint8_t, 64> out_chunk{};
  size_t in_pos = 0;

  // 1. Feed 1 byte in and drain 1 byte out at a time.
  while (in_pos < 20) {
    enc.next_in = input.data() + in_pos;
    enc.avail_in = 1;
    enc.next_out = out_chunk.data();
    enc.avail_out = 1;
    ASSERT_EQ(AEC_OK, aec_encode(&enc, AEC_NO_FLUSH));
    in_pos += 1 - enc.avail_in;
    const size_t produced = 1 - enc.avail_out;
    encoded.insert(encoded.end(), out_chunk.data(),
                   out_chunk.data() + produced);
    // Drain any buffered block output remaining in cds_buf.
    while (enc.avail_out == 0) {
      enc.next_out = out_chunk.data();
      enc.avail_out = 1;
      ASSERT_EQ(AEC_OK, aec_encode(&enc, AEC_NO_FLUSH));
      encoded.insert(encoded.end(), out_chunk.data(),
                     out_chunk.data() + (1 - enc.avail_out));
    }
  }

  // 2. Stop mid-RSI with generous avail_out (triggers post-FSM
  // if (state->direct_out) flush on aec_encode() exit).
  enc.next_in = input.data() + in_pos;
  enc.avail_in = input.size() - in_pos;
  enc.next_out = out_chunk.data();
  enc.avail_out = out_chunk.size();
  ASSERT_EQ(AEC_OK, aec_encode(&enc, AEC_NO_FLUSH));
  encoded.insert(encoded.end(), out_chunk.data(),
                 out_chunk.data() + (out_chunk.size() - enc.avail_out));

  // 3. Trigger AEC_FLUSH with avail_out = 0 so final byte waits, then drain in
  // buffered mode (avail_out = 2 <= cds_len) so !direct_out writes the trailing
  // padded byte.
  enc.avail_in = 0;
  enc.next_out = out_chunk.data();
  enc.avail_out = 0;
  ASSERT_EQ(AEC_OK, aec_encode(&enc, AEC_FLUSH));

  for (int i = 0; i < 64 && !enc.state->flushed; ++i) {
    enc.next_out = out_chunk.data();
    // Keep avail_out <= state->cds_len so buffered mode (!direct_out) runs.
    enc.avail_out = 2;
    ASSERT_EQ(AEC_OK, aec_encode(&enc, AEC_FLUSH));
    encoded.insert(encoded.end(), out_chunk.data(),
                   out_chunk.data() + (2 - enc.avail_out));
  }
  ASSERT_TRUE(enc.state->flushed);

  // 4. Decode the reassembled stream and verify lossless round-trip.
  std::vector<uint8_t> decoded(input.size(), 0xA5);
  aec_stream dec{};
  dec.bits_per_sample = 8;
  dec.block_size = 8;
  dec.rsi = 2;
  dec.flags = AEC_DATA_PREPROCESS;
  dec.next_in = encoded.data();
  dec.avail_in = encoded.size();
  dec.next_out = decoded.data();
  dec.avail_out = decoded.size();
  ASSERT_EQ(AEC_OK, aec_buffer_decode(&dec));
  EXPECT_EQ(input.size(), dec.total_out);
  EXPECT_THAT(decoded, ElementsAreArray(input));
}

TEST(EncodeTest, PartialRsiAlignedToBlockAndUnflushedEndErrors) {
  // 1. Partial final RSI exact multiple of block_size (8 of 16 samples): hits
  // state->i % strm->block_size == 0 path in m_get_rsi_resumable().
  {
    constexpr auto kEightSamples =
        std::to_array<uint8_t>({1, 2, 3, 4, 5, 6, 7, 8});
    VerifyRoundTrip(8, 8, 2, AEC_DATA_PREPROCESS, kEightSamples);
  }

  // 2. When AEC_FLUSH cannot write its final byte (avail_out == 0),
  // aec_encode_end() frees state cleanly and returns AEC_STREAM_ERROR.
  {
    constexpr auto kBlock = std::to_array<uint8_t>({1, 2, 3, 4, 5, 6, 7, 8});
    std::array<uint8_t, 64> out_buf{};

    aec_stream enc{};
    enc.bits_per_sample = 8;
    enc.block_size = 8;
    enc.rsi = 1;
    enc.flags = 0;
    ASSERT_EQ(AEC_OK, aec_encode_init(&enc));
    absl::Cleanup verify_stream_error_end = [&enc] {
      EXPECT_EQ(AEC_STREAM_ERROR, aec_encode_end(&enc));
    };

    enc.next_in = kBlock.data();
    enc.avail_in = kBlock.size();
    enc.next_out = out_buf.data();
    enc.avail_out = 0;
    // Emit partial block buffered in cds_buf with zero avail_out, then request
    // AEC_FLUSH while avail_out remains 0 so state->flushed stays 0.
    ASSERT_EQ(AEC_OK, aec_encode(&enc, AEC_FLUSH));
  }
}

TEST(EncodeTest, RsiOffsetsLifecycleAndErrorHandling) {
  constexpr unsigned int kBlockSize = 8;
  constexpr unsigned int kRsi = 2;
  std::vector<uint8_t> input(3 * kRsi * kBlockSize);
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<uint8_t>((i * 13u) & 0x7Fu);
  }
  std::vector<uint8_t> output(input.size() * 2 + 128, 0xA5);

  aec_stream enc{};
  enc.bits_per_sample = 8;
  enc.block_size = kBlockSize;
  enc.rsi = kRsi;
  enc.flags = AEC_DATA_PREPROCESS;
  ASSERT_EQ(AEC_OK, aec_encode_init(&enc));
  absl::Cleanup end_stream = [&enc] {
    EXPECT_EQ(AEC_OK, aec_encode_end(&enc));
  };

  // 1. Querying offsets before enabling returns AEC_RSI_OFFSETS_ERROR.
  size_t count = 99;
  EXPECT_EQ(AEC_RSI_OFFSETS_ERROR, aec_encode_count_offsets(&enc, &count));
  EXPECT_EQ(0u, count);
  std::array<size_t, 4> small_offsets{};
  EXPECT_EQ(
      AEC_RSI_OFFSETS_ERROR,
      aec_encode_get_offsets(&enc, small_offsets.data(), small_offsets.size()));

  // 2. Enabling offsets twice fails with AEC_RSI_OFFSETS_ERROR.
  ASSERT_EQ(AEC_OK, aec_encode_enable_offsets(&enc));
  EXPECT_EQ(AEC_RSI_OFFSETS_ERROR, aec_encode_enable_offsets(&enc));

  // 3. Encode 3 RSIs and verify expected offset count (initial 0 + 3 RSIs).
  enc.next_in = input.data();
  enc.avail_in = input.size();
  enc.next_out = output.data();
  enc.avail_out = output.size();
  ASSERT_EQ(AEC_OK, aec_encode(&enc, AEC_FLUSH));

  ASSERT_EQ(AEC_OK, aec_encode_count_offsets(&enc, &count));
  ASSERT_EQ(4u, count);  // Initial offset 0 + 3 completed RSIs.

  // 4. Undersized caller buffer returns AEC_MEM_ERROR before valid retrieval.
  std::array<size_t, 2> too_small{};
  EXPECT_EQ(AEC_MEM_ERROR,
            aec_encode_get_offsets(&enc, too_small.data(), too_small.size()));

  std::vector<size_t> offsets(count);
  ASSERT_EQ(AEC_OK,
            aec_encode_get_offsets(&enc, offsets.data(), offsets.size()));
  EXPECT_EQ(0u, offsets.front());
  for (size_t i = 1; i < offsets.size(); ++i) {
    EXPECT_GT(offsets[i], offsets[i - 1]);
  }
}

TEST(EncodeTest, PinsRsiPaddingNoopAndDirectOutCdsLenBoundary) {
  // 1. ENABLE_RSI_PADDING is disabled on encode in google3, so AEC_PAD_RSI
  // produces the exact same unpadded bitstream as flags == 0.
  constexpr auto kUnalignedInput =
      std::to_array<uint8_t>({1, 2, 1, 2, 1, 2, 1, 2});
  auto encode_once = [&kUnalignedInput](unsigned int flags) {
    std::vector<uint8_t> out(64, 0xA5);
    aec_stream enc{};
    enc.bits_per_sample = 8;
    enc.block_size = 8;
    enc.rsi = 1;
    enc.flags = flags;
    enc.next_in = kUnalignedInput.data();
    enc.avail_in = kUnalignedInput.size();
    enc.next_out = out.data();
    enc.avail_out = out.size();
    EXPECT_EQ(AEC_OK, aec_buffer_encode(&enc));
    out.resize(enc.total_out);
    return out;
  };
  EXPECT_THAT(encode_once(AEC_PAD_RSI), ElementsAreArray(encode_once(0)));

  // 2. Pin init_output() direct_out threshold (avail_out > state->cds_len)
  // vs buffered mode (avail_out == state->cds_len) and confirm copy64() writes
  // stay strictly within avail_out.
  constexpr auto kIncompressible =
      std::to_array<uint8_t>({10, 200, 30, 220, 40, 240, 50, 250});
  for (size_t extra : {size_t{0}, size_t{1}}) {
    aec_stream enc{};
    enc.bits_per_sample = 8;
    enc.block_size = 8;
    enc.rsi = 1;
    ASSERT_EQ(AEC_OK, aec_encode_init(&enc));
    absl::Cleanup end_stream = [&enc] {
      EXPECT_EQ(AEC_OK, aec_encode_end(&enc));
    };

    const size_t cap = enc.state->cds_len + extra;
    std::vector<uint8_t> out(cap + 16, 0xA5);
    enc.next_in = kIncompressible.data();
    enc.avail_in = kIncompressible.size();
    enc.next_out = out.data();
    enc.avail_out = cap;
    ASSERT_EQ(AEC_OK, aec_encode(&enc, AEC_FLUSH));
    EXPECT_LE(enc.total_out, cap);
    EXPECT_THAT(std::span<const uint8_t>(out).subspan(cap), Each(0xA5));
  }
}

// TODO(schwehr): Cover malloc OOM cleanup branches and unreachable non-OK
// aec_encode() return check in aec_buffer_encode() if fault injection is added.

}  // namespace
}  // namespace libaec
