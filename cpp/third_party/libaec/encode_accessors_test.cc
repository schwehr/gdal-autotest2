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
// Tests libaec sample and RSI input accessors
// (third_party/libaec/src/encode_accessors.c).
// Note: WORDS_BIGENDIAN is undefined in google3 (little-endian), so
// aec_get_rsi_lsb_32 uses memcpy while aec_get_rsi_msb_32 swaps bytes.
//
// See also:
//   https://gitlab.dkrz.de/k202009/libaec/-/blob/main/src/encode_accessors.c
//   https://gitlab.dkrz.de/k202009/libaec/-/blob/main/src/encode_accessors.h

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/libaec/include/config.h"
#include "third_party/libaec/include/libaec.h"

// clang-format off
extern "C" {
#include "third_party/libaec/src/encode.h"
#include "third_party/libaec/src/encode_accessors.h"
}  // extern "C"
// clang-format on

namespace libaec {
namespace {

using ::testing::ElementsAreArray;

// Pin google3 little-endian config.h assumptions tested below.
static_assert(std::endian::native == std::endian::little);
static_assert(HAVE_DECL___BUILTIN_CLZLL == 1);
#if defined(WORDS_BIGENDIAN) || defined(HAVE_BSR64)
#error "Expected WORDS_BIGENDIAN and HAVE_BSR64 undefined in google3 config.h."
#endif

constexpr unsigned int kBlockSize = 8;
constexpr unsigned int kRsiBlocks = 2;
constexpr size_t kRsiSamples = kBlockSize * kRsiBlocks;

void VerifyGetRsi(unsigned int bits_per_sample, unsigned int flags,
                  void (*get_rsi_fn)(struct aec_stream*),
                  std::span<const uint8_t> input,
                  std::span<const uint32_t> expected,
                  size_t expected_trailing) {
  aec_stream enc{};
  enc.bits_per_sample = bits_per_sample;
  enc.block_size = kBlockSize;
  enc.rsi = kRsiBlocks;
  enc.flags = flags;
  ASSERT_EQ(AEC_OK, aec_encode_init(&enc));
  absl::Cleanup end_stream = [&enc] {
    EXPECT_EQ(AEC_OK, aec_encode_end(&enc));
  };

  enc.next_in = input.data();
  enc.avail_in = input.size();
  get_rsi_fn(&enc);

  const size_t consumed = input.size() - expected_trailing;
  EXPECT_EQ(enc.next_in, input.data() + consumed);
  EXPECT_EQ(enc.avail_in, expected_trailing);
  EXPECT_THAT(std::span<const uint32_t>(enc.state->data_raw, kRsiSamples),
              ElementsAreArray(expected));
}

TEST(EncodeAccessorsTest, GetSingleSampleAllWidthsAndByteOrders) {
  constexpr auto kBytes = std::to_array<uint8_t>({
      // 8-bit sample: 0xAB
      0xAB,
      // 16-bit LSB sample: 0x1234
      0x34,
      0x12,
      // 16-bit MSB sample: 0x9ABC (tests high-bit non-sign-extension)
      0x9A,
      0xBC,
      // 24-bit LSB sample: 0x89ABCD
      0xCD,
      0xAB,
      0x89,
      // 24-bit MSB sample: 0x765432
      0x76,
      0x54,
      0x32,
      // 32-bit LSB sample: 0xFEDCBA98
      0x98,
      0xBA,
      0xDC,
      0xFE,
      // 32-bit MSB sample: 0x12345678
      0x12,
      0x34,
      0x56,
      0x78,
      // Trailing unconsumed sentinel byte.
      0xEE,
  });

  aec_stream strm{};
  strm.next_in = kBytes.data();
  strm.avail_in = kBytes.size();

  EXPECT_EQ(uint32_t{0xAB}, aec_get_8(&strm));
  EXPECT_EQ(uint32_t{0x1234}, aec_get_lsb_16(&strm));
  EXPECT_EQ(uint32_t{0x9ABC}, aec_get_msb_16(&strm));
  EXPECT_EQ(uint32_t{0x89ABCD}, aec_get_lsb_24(&strm));
  EXPECT_EQ(uint32_t{0x765432}, aec_get_msb_24(&strm));
  EXPECT_EQ(uint32_t{0xFEDCBA98}, aec_get_lsb_32(&strm));
  EXPECT_EQ(uint32_t{0x12345678}, aec_get_msb_32(&strm));

  EXPECT_EQ(1u, strm.avail_in);
  EXPECT_EQ(kBytes.data() + kBytes.size() - 1, strm.next_in);
}

TEST(EncodeAccessorsTest, GetRsi8ReadsAllSamples) {
  constexpr size_t kTrailing = 3;
  std::vector<uint8_t> input;
  std::vector<uint32_t> expected;
  for (size_t i = 0; i < kRsiSamples; ++i) {
    const uint8_t sample = static_cast<uint8_t>(0x80u + i * 7u);
    input.push_back(sample);
    expected.push_back(sample);
  }
  input.insert(input.end(), kTrailing, 0xEE);

  ASSERT_NO_FATAL_FAILURE(VerifyGetRsi(8, AEC_DATA_PREPROCESS, aec_get_rsi_8,
                                       input, expected, kTrailing));
}

TEST(EncodeAccessorsTest, GetRsi16To32ReadsLsbAndMsb) {
  struct RsiOrderCase {
    const char* label;
    unsigned int bits_per_sample;
    size_t bytes_per_sample;
    unsigned int extra_flags;
    size_t trailing;
    uint32_t base_sample;
    uint32_t step;
    void (*lsb_fn)(struct aec_stream*);
    void (*msb_fn)(struct aec_stream*);
  };
  constexpr RsiOrderCase kCases[] = {
      {.label = "16-bit",
       .bits_per_sample = 16,
       .bytes_per_sample = 2,
       .extra_flags = 0,
       .trailing = 2,
       .base_sample = 0x8100u,
       .step = 0x0111u,
       .lsb_fn = aec_get_rsi_lsb_16,
       .msb_fn = aec_get_rsi_msb_16},
      {.label = "24-bit",
       .bits_per_sample = 24,
       .bytes_per_sample = 3,
       .extra_flags = AEC_DATA_3BYTE,
       .trailing = 1,
       .base_sample = 0x820304u,
       .step = 0x050301u,
       .lsb_fn = aec_get_rsi_lsb_24,
       .msb_fn = aec_get_rsi_msb_24},
      {.label = "32-bit",
       .bits_per_sample = 32,
       .bytes_per_sample = 4,
       .extra_flags = 0,
       .trailing = 4,
       .base_sample = 0x90000102u,
       .step = 0x01020304u,
       .lsb_fn = aec_get_rsi_lsb_32,
       .msb_fn = aec_get_rsi_msb_32},
  };

  for (const RsiOrderCase& tc : kCases) {
    std::vector<uint8_t> lsb_input;
    std::vector<uint8_t> msb_input;
    std::vector<uint32_t> expected;
    lsb_input.reserve(kRsiSamples * tc.bytes_per_sample + tc.trailing);
    msb_input.reserve(kRsiSamples * tc.bytes_per_sample + tc.trailing);
    expected.reserve(kRsiSamples);
    for (size_t i = 0; i < kRsiSamples; ++i) {
      const uint32_t v = tc.base_sample + static_cast<uint32_t>(i) * tc.step;
      expected.push_back(v);
      for (size_t b = 0; b < tc.bytes_per_sample; ++b) {
        lsb_input.push_back(static_cast<uint8_t>((v >> (b * 8)) & 0xFFu));
        msb_input.push_back(static_cast<uint8_t>(
            (v >> ((tc.bytes_per_sample - 1 - b) * 8)) & 0xFFu));
      }
    }
    lsb_input.insert(lsb_input.end(), tc.trailing, 0xEE);
    msb_input.insert(msb_input.end(), tc.trailing, 0xEE);

    {
      SCOPED_TRACE(std::string(tc.label) + " LSB");
      ASSERT_NO_FATAL_FAILURE(
          VerifyGetRsi(tc.bits_per_sample, AEC_DATA_PREPROCESS | tc.extra_flags,
                       tc.lsb_fn, lsb_input, expected, tc.trailing));
    }
    {
      SCOPED_TRACE(std::string(tc.label) + " MSB");
      ASSERT_NO_FATAL_FAILURE(
          VerifyGetRsi(tc.bits_per_sample,
                       AEC_DATA_PREPROCESS | AEC_DATA_MSB | tc.extra_flags,
                       tc.msb_fn, msb_input, expected, tc.trailing));
    }
  }
}

}  // namespace
}  // namespace libaec
