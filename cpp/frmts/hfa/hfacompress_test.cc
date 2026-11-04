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
// Tests HFA raster block RLE compression (hfacompress.cpp).
//
// See also:
//   https://gdal.org/drivers/raster/hfa.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hfa/hfacompress.cpp

#include <cstddef>
#include <iterator>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/hfa/hfa.h"
#include "third_party/gdal/frmts/hfa/hfa_p.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

struct DataTypeSupportCase {
  EPTType data_type;
  bool supported;
};

constexpr DataTypeSupportCase kDataTypeCases[] = {
    {.data_type = EPT_u1, .supported = true},
    {.data_type = EPT_u2, .supported = true},
    {.data_type = EPT_u4, .supported = true},
    {.data_type = EPT_u8, .supported = true},
    {.data_type = EPT_s8, .supported = true},
    {.data_type = EPT_u16, .supported = true},
    {.data_type = EPT_s16, .supported = true},
    {.data_type = EPT_u32, .supported = true},
    {.data_type = EPT_s32, .supported = true},
    {.data_type = EPT_f32, .supported = true},
    {.data_type = EPT_f64, .supported = false},
    {.data_type = EPT_c64, .supported = false},
    {.data_type = EPT_c128, .supported = false},
};
static_assert(std::size(kDataTypeCases) == EPT_MAX + 1);

struct TwoRunCase {
  EPTType data_type;
  GUInt32 first_half_val;
  GUInt32 second_half_val;
  GUInt32 expected_min;
  int expected_bits;
  GUInt32 expected_val_size;
};

constexpr TwoRunCase kTwoRunCases[] = {
    {.data_type = EPT_u16,
     .first_half_val = 500u,
     .second_half_val = 1500u,
     .expected_min = 500u,
     .expected_bits = 16,
     .expected_val_size = 4u},
    {.data_type = EPT_u32,
     .first_half_val = 100000u,
     .second_half_val = 300000u,
     .expected_min = 100000u,
     .expected_bits = 32,
     .expected_val_size = 8u},
};

template <typename PixelT>
void RunTwoRunCompressionCase(const TwoRunCase& tc) {
  // HFACompress reads native-endian pixel words from memory and emits
  // little-endian compressed streams.
  std::vector<PixelT> data(128, static_cast<PixelT>(tc.first_half_val));
  for (size_t i = 64; i < 128; ++i) {
    data[i] = static_cast<PixelT>(tc.second_half_val);
  }

  HFACompress compressor(data.data(),
                         static_cast<GUInt32>(data.size() * sizeof(PixelT)),
                         tc.data_type);
  ASSERT_TRUE(compressor.compressBlock());
  EXPECT_EQ(tc.expected_min, compressor.getMin());
  EXPECT_EQ(tc.expected_bits, compressor.getNumBits());
  EXPECT_EQ(2u, compressor.getNumRuns());
  EXPECT_EQ(tc.expected_val_size, compressor.getValueSize());
}

TEST(HfaCompressTest, QueryDataTypeSupported) {
  for (const DataTypeSupportCase& tc : kDataTypeCases) {
    EXPECT_EQ(tc.supported, HFACompress::QueryDataTypeSupported(tc.data_type))
        << HFAGetDataTypeName(tc.data_type);
  }
}

TEST(HfaCompressTest, UnsupportedDataTypeFails) {
  std::vector<double> data(64, 1.5);
  HFACompress compressor(
      data.data(), static_cast<GUInt32>(data.size() * sizeof(double)), EPT_f64);
  EXPECT_FALSE(compressor.compressBlock());
}

TEST(HfaCompressTest, CompressU8SmallRange) {
  std::vector<GByte> data(256, 20);
  for (size_t i = 0; i < 64; ++i) {
    data[i] = 10;
  }
  for (size_t i = 192; i < 256; ++i) {
    data[i] = 30;
  }

  HFACompress compressor(data.data(), static_cast<GUInt32>(data.size()),
                         EPT_u8);
  ASSERT_TRUE(compressor.compressBlock());
  EXPECT_EQ(10u, compressor.getMin());
  EXPECT_EQ(8, compressor.getNumBits());
  EXPECT_EQ(3u, compressor.getNumRuns());
  // Runs are 64, 128, and 64; each is >= 0x40 so each count uses 2 bytes.
  EXPECT_EQ(6u, compressor.getCountSize());
  EXPECT_EQ(3u, compressor.getValueSize());
  ASSERT_NE(nullptr, compressor.getCounts());
  ASSERT_NE(nullptr, compressor.getValues());
  EXPECT_EQ(0, compressor.getValues()[0]);
  EXPECT_EQ(10, compressor.getValues()[1]);
  EXPECT_EQ(20, compressor.getValues()[2]);
}

TEST(HfaCompressTest, CompressU16AndU32Ranges) {
  for (const TwoRunCase& tc : kTwoRunCases) {
    if (tc.data_type == EPT_u16) {
      RunTwoRunCompressionCase<GUInt16>(tc);
    } else if (tc.data_type == EPT_u32) {
      RunTwoRunCompressionCase<GUInt32>(tc);
    } else {
      FAIL() << "Unhandled pixel type: " << tc.data_type;
    }
  }
}

TEST(HfaCompressTest, CompressSubByteTypesU1U2U4) {
  // 1. 1-bit data: alternating 0x00 and 0xff across 64 bytes (512 pixels).
  std::vector<GByte> u1_data(64, 0x00);
  for (size_t i = 32; i < 64; ++i) {
    u1_data[i] = 0xff;
  }
  HFACompress u1_comp(u1_data.data(), static_cast<GUInt32>(u1_data.size()),
                      EPT_u1);
  ASSERT_TRUE(u1_comp.compressBlock());
  EXPECT_EQ(0u, u1_comp.getMin());
  EXPECT_EQ(2u, u1_comp.getNumRuns());

  // 2. 2-bit data: 4 runs of 32 bytes each (0x00, 0x55, 0xaa, 0xff) exercises
  //    all 4 2-bit pixel positions (% 4 == 0, 1, 2, 3) and compresses cleanly.
  std::vector<GByte> u2_data(128, 0x00);
  for (size_t i = 32; i < 64; ++i) u2_data[i] = 0x55;
  for (size_t i = 64; i < 96; ++i) u2_data[i] = 0xaa;
  for (size_t i = 96; i < 128; ++i) u2_data[i] = 0xff;
  HFACompress u2_comp(u2_data.data(), static_cast<GUInt32>(u2_data.size()),
                      EPT_u2);
  ASSERT_TRUE(u2_comp.compressBlock());
  EXPECT_EQ(0u, u2_comp.getMin());
  EXPECT_EQ(8, u2_comp.getNumBits());
  EXPECT_EQ(4u, u2_comp.getNumRuns());

  // 3. 4-bit data: 2 runs of 32 bytes each (0x00, 0xff) plus 1 byte of 0x5a
  //    exercises both low and high nibbles.
  std::vector<GByte> u4_data(64, 0x00);
  u4_data[31] = 0x5a;
  for (size_t i = 32; i < 64; ++i) u4_data[i] = 0xff;
  HFACompress u4_comp(u4_data.data(), static_cast<GUInt32>(u4_data.size()),
                      EPT_u4);
  ASSERT_TRUE(u4_comp.compressBlock());
  EXPECT_EQ(0u, u4_comp.getMin());
}

TEST(HfaCompressTest, LargeRunLengthsExerciseAllCountEncodings) {
  // 0x400005 (> 4,194,304) identical bytes in one run exercises the 4-byte
  // count encoding (>= 0x400000), plus 3-byte (>= 0x4000) and 2-byte (>= 0x40)
  // runs in the same block.
  constexpr GUInt32 kFourByteRun = 0x400005;
  constexpr GUInt32 kThreeByteRun = 0x4005;
  constexpr GUInt32 kTwoByteRun = 0x45;
  std::vector<GByte> data(kFourByteRun + kThreeByteRun + kTwoByteRun, 7);
  for (GUInt32 i = kFourByteRun; i < kFourByteRun + kThreeByteRun; ++i) {
    data[i] = 8;
  }
  for (GUInt32 i = kFourByteRun + kThreeByteRun; i < data.size(); ++i) {
    data[i] = 9;
  }

  HFACompress compressor(data.data(), static_cast<GUInt32>(data.size()),
                         EPT_u8);
  ASSERT_TRUE(compressor.compressBlock());
  EXPECT_EQ(3u, compressor.getNumRuns());
  // 4 bytes + 3 bytes + 2 bytes = 9 bytes for counts.
  EXPECT_EQ(9u, compressor.getCountSize());
  EXPECT_EQ(3u, compressor.getValueSize());
}

TEST(HfaCompressTest, IncompressibleBlocksReturnFalse) {
  // 1. Range 255 forces 16-bit value encoding on 8-bit pixels; alternating
  //    every pixel causes m_pCurrValues - m_pValues > m_nBlockSize mid-loop.
  std::vector<GByte> expanding(64);
  for (size_t i = 0; i < expanding.size(); ++i) {
    expanding[i] = (i % 2 == 0) ? 0 : 255;
  }
  HFACompress exp_comp(expanding.data(), static_cast<GUInt32>(expanding.size()),
                       EPT_u8);
  EXPECT_FALSE(exp_comp.compressBlock());

  // 2. Small block finishes loop but compressed payload + 13-byte header >=
  //    block size.
  std::vector<GByte> too_small = {1, 2, 3, 4, 5, 6, 7, 8};
  HFACompress small_comp(too_small.data(),
                         static_cast<GUInt32>(too_small.size()), EPT_u8);
  EXPECT_FALSE(small_comp.compressBlock());
}

// TODO: b/335317901 - Investigate whether the defensive default branch in
// valueAsUInt32 (hfacompress.cpp) is reachable for supported pixel types.

}  // namespace
}  // namespace autotest2
