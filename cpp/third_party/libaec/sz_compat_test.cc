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
// Tests libaec SZIP compatibility layer (third_party/libaec/src/sz_compat.c).
// Note: SZ_BufftoBuffCompress sets strm.bits_per_sample = 8 when
// bits_per_pixel is 32 or 64, skipping the sourceLen modulus check; 32/64-bit
// inputs must use sourceLen divisible by 4 or 8 to keep interleave_buffer
// initialized for MSan.
//
// See also:
//   https://gitlab.dkrz.de/k202009/libaec/-/blob/main/src/sz_compat.c
//   https://gitlab.dkrz.de/k202009/libaec/-/blob/main/include/szlib.h

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>  // NOLINT
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/libaec/include/libaec.h"
#include "third_party/libaec/include/szlib.h"

extern "C" char SZ_Compress(void);

namespace libaec {
namespace {

using ::testing::Each;
using ::testing::ElementsAreArray;

static_assert(SZ_OK == 0);
static_assert(SZ_PARAM_ERROR == AEC_CONF_ERROR);

void VerifySzRoundTrip(int bits_per_pixel, int pixels_per_block,
                       int pixels_per_scanline, int options_mask,
                       std::span<const uint8_t> input,
                       bool oversized_decompress_dest) {
  SZ_com_t param{};
  param.options_mask = options_mask;
  param.bits_per_pixel = bits_per_pixel;
  param.pixels_per_block = pixels_per_block;
  param.pixels_per_scanline = pixels_per_scanline;

  std::vector<uint8_t> compressed(input.size() * 4 + 256, 0xA5);
  size_t compressed_len = compressed.size();
  ASSERT_EQ(SZ_OK, SZ_BufftoBuffCompress(compressed.data(), &compressed_len,
                                         input.data(), input.size(), &param));
  ASSERT_GT(compressed_len, 0u);
  compressed.resize(compressed_len);

  const size_t extra = oversized_decompress_dest ? 32 : 0;
  std::vector<uint8_t> decompressed(input.size() + extra + 8, 0xA5);
  size_t decompressed_len = input.size() + extra;
  ASSERT_EQ(SZ_OK, SZ_BufftoBuffDecompress(decompressed.data(),
                                           &decompressed_len, compressed.data(),
                                           compressed.size(), &param));
  EXPECT_EQ(input.size(), decompressed_len);
  EXPECT_THAT(std::span<const uint8_t>(decompressed).first(input.size()),
              ElementsAreArray(input));
  EXPECT_THAT(std::span<const uint8_t>(decompressed).subspan(input.size()),
              Each(0xA5));
}

TEST(SzCompatTest, EncoderEnabledAndConfigureProbeReturnExpectedValues) {
  EXPECT_EQ(1, SZ_encoder_enabled());
  EXPECT_EQ(SZ_OK, SZ_Compress());
}

TEST(SzCompatTest, RoundTripsAcrossWidthsOptionsAndScanlinePadding) {
  // 1. 8-bit unpadded and padded scanlines across option masks (pp == 0 and
  // pp != 0, plus partial final scanline and oversized *destLen clamping).
  {
    std::vector<uint8_t> eight_bit(32);
    for (size_t i = 0; i < eight_bit.size(); ++i) {
      eight_bit[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    VerifySzRoundTrip(8, 8, 16, 0, eight_bit, false);
    VerifySzRoundTrip(8, 8, 16, SZ_NN_OPTION_MASK, eight_bit, true);

    // Non-multiple scanline (10 vs block 8 -> pad_scanline = 2) with and
    // without SZ_NN_OPTION_MASK across 25 bytes (2.5 scanlines).
    std::vector<uint8_t> padded(25);
    for (size_t i = 0; i < padded.size(); ++i) {
      padded[i] = static_cast<uint8_t>(200 - i * 3);
    }
    VerifySzRoundTrip(8, 8, 10, 0, padded, false);
    VerifySzRoundTrip(8, 8, 10,
                      SZ_ALLOW_K13_OPTION_MASK | SZ_CHIP_OPTION_MASK |
                          SZ_EC_OPTION_MASK | SZ_LSB_OPTION_MASK |
                          SZ_MSB_OPTION_MASK | SZ_NN_OPTION_MASK |
                          SZ_RAW_OPTION_MASK,
                      padded, false);
  }

  // 2. 12-bit and 16-bit (pixel_size = 2) unpadded and padded. Ensure 12-bit
  // values fit strictly in [0, 4095] across 16-bit storage slots.
  {
    std::vector<uint8_t> twelve_bit(32);
    for (size_t i = 0; i < 16; ++i) {
      const uint16_t v = static_cast<uint16_t>((i * 193u) & 0x0FFFu);
      twelve_bit[2 * i] = static_cast<uint8_t>(v & 0xFFu);
      twelve_bit[2 * i + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    }
    VerifySzRoundTrip(12, 8, 16, SZ_NN_OPTION_MASK, twelve_bit, false);

    std::vector<uint8_t> sixteen_bit(32);
    for (size_t i = 0; i < 16; ++i) {
      const uint16_t v = static_cast<uint16_t>(0x1200u + i * 71u);
      sixteen_bit[2 * i] = static_cast<uint8_t>((v >> 8) & 0xFFu);
      sixteen_bit[2 * i + 1] = static_cast<uint8_t>(v & 0xFFu);
    }
    VerifySzRoundTrip(16, 8, 10, SZ_MSB_OPTION_MASK | SZ_NN_OPTION_MASK,
                      sixteen_bit, false);
  }

  // 3. 24-bit non-interleaved (pixel_size = 4).
  {
    std::vector<uint8_t> twenty_four_bit(64);
    for (size_t i = 0; i < 16; ++i) {
      twenty_four_bit[4 * i] = static_cast<uint8_t>(i * 5);
      twenty_four_bit[4 * i + 1] = 0x12;
      twenty_four_bit[4 * i + 2] = 0x34;
      twenty_four_bit[4 * i + 3] = 0x00;
    }
    VerifySzRoundTrip(24, 8, 16, SZ_NN_OPTION_MASK, twenty_four_bit, true);
  }

  // 4. 32-bit interleaved (sourceLen divisible by 4) & 64-bit interleaved
  // (sourceLen divisible by 8).
  {
    std::vector<uint8_t> data32(16 * 4);
    for (size_t i = 0; i < data32.size(); ++i) {
      data32[i] = static_cast<uint8_t>((i * 13) ^ (i & 3));
    }
    VerifySzRoundTrip(32, 8, 16, SZ_NN_OPTION_MASK, data32, true);
    VerifySzRoundTrip(32, 8, 10, 0, data32, false);

    std::vector<uint8_t> data64(16 * 8);
    for (size_t i = 0; i < data64.size(); ++i) {
      data64[i] = static_cast<uint8_t>(i * 3 + 7);
    }
    VerifySzRoundTrip(64, 8, 16, SZ_NN_OPTION_MASK, data64, false);
  }
}

TEST(SzCompatTest, RejectsInvalidParametersSizesAndCorruptInputs) {
  struct BadParamCase {
    int bits_per_pixel;
    int pixels_per_block;
    int pixels_per_scanline;
  };
  constexpr auto kBadParams = std::to_array<BadParamCase>({
      {.bits_per_pixel = 8, .pixels_per_block = 8, .pixels_per_scanline = 0},
      {.bits_per_pixel = 8, .pixels_per_block = 8, .pixels_per_scanline = 4097},
      {.bits_per_pixel = 8, .pixels_per_block = 0, .pixels_per_scanline = 16},
      {.bits_per_pixel = 8, .pixels_per_block = 5, .pixels_per_scanline = 16},
      {.bits_per_pixel = 0, .pixels_per_block = 8, .pixels_per_scanline = 16},
      {.bits_per_pixel = 33, .pixels_per_block = 8, .pixels_per_scanline = 16},
      {.bits_per_pixel = 65, .pixels_per_block = 8, .pixels_per_scanline = 16},
  });

  // 1. Out-of-bounds configuration parameters fail both compress & decompress.
  std::array<uint8_t, 64> buf{};
  for (size_t i = 0; i < kBadParams.size(); ++i) {
    SCOPED_TRACE(i);
    SZ_com_t param{};
    param.bits_per_pixel = kBadParams[i].bits_per_pixel;
    param.pixels_per_block = kBadParams[i].pixels_per_block;
    param.pixels_per_scanline = kBadParams[i].pixels_per_scanline;

    size_t out_len = buf.size();
    EXPECT_EQ(SZ_PARAM_ERROR, SZ_BufftoBuffCompress(buf.data(), &out_len,
                                                    buf.data(), 16, &param));
    EXPECT_EQ(SZ_PARAM_ERROR, SZ_BufftoBuffDecompress(buf.data(), &out_len,
                                                      buf.data(), 16, &param));
  }

  // 2. Unaligned sourceLen for 16-bit samples (pixel_size == 2) ->
  // SZ_PARAM_ERROR.
  {
    SZ_com_t param{};
    param.bits_per_pixel = 16;
    param.pixels_per_block = 8;
    param.pixels_per_scanline = 8;
    size_t out_len = buf.size();
    EXPECT_EQ(SZ_PARAM_ERROR, SZ_BufftoBuffCompress(buf.data(), &out_len,
                                                    buf.data(), 3, &param));
  }

  // 3. padbuf_size arithmetic overflow returns SZ_PARAM_ERROR before malloc().
  {
    SZ_com_t param{};
    param.bits_per_pixel = 24;
    param.pixels_per_block = 4;
    param.pixels_per_scanline = 1;
    size_t out_len = buf.size();
    const size_t huge_len =
        (std::numeric_limits<size_t>::max() / 2) & ~size_t{3};
    EXPECT_EQ(SZ_PARAM_ERROR, SZ_BufftoBuffCompress(buf.data(), &out_len,
                                                    nullptr, huge_len, &param));
  }

  // 4. Tiny output buffer -> SZ_OUTBUFF_FULL (AEC_STREAM_ERROR mapped to 2).
  {
    SZ_com_t param{};
    param.bits_per_pixel = 8;
    param.pixels_per_block = 8;
    param.pixels_per_scanline = 16;
    std::array<uint8_t, 16> src{};
    for (size_t i = 0; i < src.size(); ++i)
      src[i] = static_cast<uint8_t>(i * 19);
    std::array<uint8_t, 1> tiny{};
    size_t out_len = tiny.size();
    EXPECT_EQ(SZ_OUTBUFF_FULL,
              SZ_BufftoBuffCompress(tiny.data(), &out_len, src.data(),
                                    src.size(), &param));
  }

  // 5. Corrupt compressed input (invalid SE code terminated at > 90 zeros) ->
  // AEC_DATA_ERROR from SZ_BufftoBuffDecompress.
  {
    SZ_com_t param{};
    param.bits_per_pixel = 8;
    param.pixels_per_block = 8;
    param.pixels_per_scanline = 8;
    std::vector<uint8_t> corrupt(32, 0x00);
    corrupt[0] = 0b00010000;  // id = 0, low_entropy = 1 (SE)
    corrupt[13] = 0x80;       // terminates FS at > SE_TABLE_SIZE (90) zeros
    std::array<uint8_t, 16> out{};
    size_t out_len = out.size();
    EXPECT_EQ(AEC_DATA_ERROR,
              SZ_BufftoBuffDecompress(out.data(), &out_len, corrupt.data(),
                                      corrupt.size(), &param));
  }
}

// TODO(schwehr): Cover malloc OOM cleanup branches in SZ_BufftoBuffCompress and
// SZ_BufftoBuffDecompress if allocator fault injection is added.

}  // namespace
}  // namespace libaec
