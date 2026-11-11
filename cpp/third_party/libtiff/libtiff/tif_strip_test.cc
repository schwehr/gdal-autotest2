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

// Unit tests for libtiff tif_strip.c (strip/scanline/raster size calculation,
// strip indexing, YCbCr subsampling, planar configs, BigTIFF, and guards).
//
// google3 caveats:
//   - Host is little-endian; big-endian behavior is exercised via "wb" mode
//     and big-endian RawTiffBuilder streams.
//   - Internal tiffiop.h fields are touched only for defensive zero/overflow
//     guards that TIFFSetField and TIFFReadDirectory reject before strip calls.
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_strip.c

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiff.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

void SetBasicStripTags(TIFF* tif, uint32_t width, uint32_t length,
                       uint16_t bits_per_sample, uint16_t samples_per_pixel,
                       uint16_t planar_config) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, length), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
}

TEST(TifStripTest, ComputeStripContigAndSeparate) {
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    SetBasicStripTags(tif.get(), 16, 10, 8, 3, PLANARCONFIG_CONTIG);

    // Before setting RowsPerStrip, default is (uint32_t)-1 so every row maps to
    // strip 0.
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 0, 0), 0U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 9, 2), 0U);

    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 3), 1);
    EXPECT_EQ(TIFFNumberOfStrips(tif.get()), 4U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 0, 0), 0U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 2, 2), 0U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 3, 0), 1U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 5, 1), 1U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 6, 0), 2U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 9, 2), 3U);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }

  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    SetBasicStripTags(tif.get(), 16, 10, 8, 3, PLANARCONFIG_SEPARATE);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 3), 1);
    ASSERT_EQ(TIFFSetupStrips(tif.get()), 1);

    EXPECT_EQ(TIFFNumberOfStrips(tif.get()), 12U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 0, 0), 0U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 3, 0), 1U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 9, 0), 3U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 0, 1), 4U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 6, 1), 6U);
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 9, 2), 11U);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);

    // Sample >= SamplesPerPixel triggers an out-of-range error and returns 0.
    EXPECT_EQ(TIFFComputeStrip(tif.get(), 0, 3), 0U);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifStripTest, NumberOfStripsDefaultSeparateAndOverflow) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  SetBasicStripTags(tif.get(), 8, 10, 8, 4, PLANARCONFIG_CONTIG);

  // Default RowsPerStrip == (uint32_t)-1 yields 1 strip per plane.
  EXPECT_EQ(TIFFNumberOfStrips(tif.get()), 1U);

  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE), 1);
  EXPECT_EQ(TIFFNumberOfStrips(tif.get()), 4U);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 4), 1);
  EXPECT_EQ(TIFFNumberOfStrips(tif.get()), 12U);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 0), 1);
  EXPECT_EQ(TIFFNumberOfStrips(tif.get()), 0U);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  // Separate planar multiplication overflow: (0x80000000 / 1) * 3 overflows
  // uint32_t inside _TIFFMultiply32.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 0x80000000U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 1), 1);
  EXPECT_EQ(TIFFNumberOfStrips(tif.get()), 0U);
  EXPECT_GT(stream.diagnostics().error_count(), 0U);
}

TEST(TifStripTest, DefensiveZeroRowsPerStripAndComputeStripOverflow) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  SetBasicStripTags(tif.get(), 8, 4, 8, 4, PLANARCONFIG_SEPARATE);

  // Force internal td_rowsperstrip = 0 (rejected by TIFFSetField) to exercise
  // the zero-divisor guards in TIFFComputeStrip and TIFFNumberOfStrips.
  tif->tif_dir.td_rowsperstrip = 0;
  EXPECT_EQ(TIFFComputeStrip(tif.get(), 0, 0), 0U);
  EXPECT_GT(stream.diagnostics().error_count(), 0U);
  EXPECT_EQ(TIFFNumberOfStrips(tif.get()), 0U);
  EXPECT_GT(stream.diagnostics().warning_count(), 0U);

  stream.diagnostics().Clear();
  tif->tif_dir.td_rowsperstrip = 1;
  tif->tif_dir.td_stripsperimage = 0x80000000U;
  // sample=2, stripsperimage=0x80000000 -> 0x100000000 overflows uint32_t.
  EXPECT_EQ(TIFFComputeStrip(tif.get(), 0, 2), 0U);
  EXPECT_GT(stream.diagnostics().error_count(), 0U);
}

TEST(TifStripTest, ScanlineAndRasterScanlineSizes) {
  struct ScanlineCase {
    uint32_t width;
    uint16_t bps;
    uint16_t spp;
    uint16_t planar;
    uint64_t expected_scanline;
    uint64_t expected_raster;
  };
  constexpr std::array<ScanlineCase, 8> kCases = {{
      {.width = 1,
       .bps = 1,
       .spp = 1,
       .planar = PLANARCONFIG_CONTIG,
       .expected_scanline = 1,
       .expected_raster = 1},
      {.width = 9,
       .bps = 1,
       .spp = 1,
       .planar = PLANARCONFIG_CONTIG,
       .expected_scanline = 2,
       .expected_raster = 2},
      {.width = 3,
       .bps = 4,
       .spp = 1,
       .planar = PLANARCONFIG_CONTIG,
       .expected_scanline = 2,
       .expected_raster = 2},
      {.width = 5,
       .bps = 8,
       .spp = 3,
       .planar = PLANARCONFIG_CONTIG,
       .expected_scanline = 15,
       .expected_raster = 15},
      {.width = 7,
       .bps = 16,
       .spp = 4,
       .planar = PLANARCONFIG_CONTIG,
       .expected_scanline = 56,
       .expected_raster = 56},
      {.width = 3,
       .bps = 1,
       .spp = 3,
       .planar = PLANARCONFIG_CONTIG,
       .expected_scanline = 2,
       .expected_raster = 2},
      {.width = 3,
       .bps = 1,
       .spp = 3,
       .planar = PLANARCONFIG_SEPARATE,
       .expected_scanline = 1,
       .expected_raster = 3},
      {.width = 11,
       .bps = 8,
       .spp = 3,
       .planar = PLANARCONFIG_SEPARATE,
       .expected_scanline = 11,
       .expected_raster = 33},
  }};

  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  for (size_t i = 0; i < kCases.size(); ++i) {
    const auto& tc = kCases[i];
    SCOPED_TRACE(i);
    SetBasicStripTags(tif.get(), tc.width, 4, tc.bps, tc.spp, tc.planar);
    EXPECT_EQ(TIFFScanlineSize64(tif.get()), tc.expected_scanline);
    EXPECT_EQ(TIFFScanlineSize(tif.get()),
              static_cast<tmsize_t>(tc.expected_scanline));
    EXPECT_EQ(TIFFRasterScanlineSize64(tif.get()), tc.expected_raster);
    EXPECT_EQ(TIFFRasterScanlineSize(tif.get()),
              static_cast<tmsize_t>(tc.expected_raster));
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  // Zero-width image triggers zero computed scanline size error.
  SetBasicStripTags(tif.get(), 0, 4, 8, 1, PLANARCONFIG_CONTIG);
  EXPECT_EQ(TIFFScanlineSize64(tif.get()), 0U);
  EXPECT_EQ(TIFFScanlineSize(tif.get()), 0);
  EXPECT_EQ(TIFFRasterScanlineSize64(tif.get()), 0U);
  EXPECT_EQ(TIFFRasterScanlineSize(tif.get()), 0);
  EXPECT_GT(stream.diagnostics().error_count(), 0U);
}

TEST(TifStripTest, YCbCrSubsamplingStripAndScanlineSizes) {
  struct YCbCrCase {
    uint32_t width;
    uint32_t nrows;
    uint16_t bps;
    uint16_t sub_h;
    uint16_t sub_v;
    uint64_t expected_scanline;
    uint64_t expected_vstrip;
  };
  constexpr std::array<YCbCrCase, 6> kCases = {{
      {.width = 5,
       .nrows = 3,
       .bps = 8,
       .sub_h = 1,
       .sub_v = 1,
       .expected_scanline = 15,
       .expected_vstrip = 45},
      {.width = 5,
       .nrows = 3,
       .bps = 8,
       .sub_h = 2,
       .sub_v = 1,
       .expected_scanline = 12,
       .expected_vstrip = 36},
      {.width = 5,
       .nrows = 3,
       .bps = 8,
       .sub_h = 2,
       .sub_v = 2,
       .expected_scanline = 9,
       .expected_vstrip = 36},
      {.width = 5,
       .nrows = 3,
       .bps = 8,
       .sub_h = 4,
       .sub_v = 2,
       .expected_scanline = 10,
       .expected_vstrip = 40},
      {.width = 5,
       .nrows = 5,
       .bps = 8,
       .sub_h = 4,
       .sub_v = 4,
       .expected_scanline = 9,
       .expected_vstrip = 72},
      {.width = 3,
       .nrows = 3,
       .bps = 4,
       .sub_h = 2,
       .sub_v = 2,
       .expected_scanline = 3,
       .expected_vstrip = 12},
  }};

  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
            1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 3), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 8), 1);

  for (size_t i = 0; i < kCases.size(); ++i) {
    const auto& tc = kCases[i];
    SCOPED_TRACE(i);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, tc.width), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, tc.bps), 1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, tc.sub_h, tc.sub_v),
        1);
    EXPECT_EQ(TIFFScanlineSize64(tif.get()), tc.expected_scanline);
    EXPECT_EQ(TIFFVStripSize64(tif.get(), tc.nrows), tc.expected_vstrip);
    EXPECT_EQ(TIFFVStripSize(tif.get(), tc.nrows),
              static_cast<tmsize_t>(tc.expected_vstrip));
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  // 1-bit 1-pixel YCbCr with 2x4 subsampling yields samplingrow_size=2,
  // 2/4=0 -> triggers computed scanline size is zero error.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 1), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 1), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, 2, 4), 1);
  EXPECT_EQ(TIFFScanlineSize64(tif.get()), 0U);
  EXPECT_GT(stream.diagnostics().error_count(), 0U);

  // Separate planar YCbCr bypasses packed subsampling and uses 1 plane.
  stream.diagnostics().Clear();
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 8), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, 2, 2), 1);
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE), 1);
  EXPECT_EQ(TIFFScanlineSize64(tif.get()), 8U);
  EXPECT_EQ(TIFFVStripSize64(tif.get(), 3), 24U);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifStripTest, YCbCrInvalidSamplesSubsamplingAndUpsampledJpeg) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 16), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 16), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
            1);

  // SamplesPerPixel != 3 in contiguous YCbCr fails _TIFFStrileSize64.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 4), 1);
  EXPECT_EQ(TIFFVStripSize64(tif.get(), 4), 0U);
  EXPECT_GT(stream.diagnostics().error_count(), 0U);

  // Invalid YCbCrSubsampling pairs fail both _TIFFStrileSize64 and
  // TIFFScanlineSize64.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 3), 1);
  constexpr std::array<std::array<uint16_t, 2>, 5> kInvalidSubsampling = {{
      {3, 2},
      {2, 3},
      {0, 2},
      {2, 0},
      {8, 1},
  }};
  for (const auto& pair : kInvalidSubsampling) {
    SCOPED_TRACE(pair[0]);
    SCOPED_TRACE(pair[1]);
    stream.diagnostics().Clear();
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, pair[0], pair[1]), 1);
    EXPECT_EQ(TIFFVStripSize64(tif.get(), 4), 0U);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFScanlineSize64(tif.get()), 0U);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
  }

  // With COMPRESSION_JPEG + JPEGCOLORMODE_RGB, libtiff sets TIFF_UPSAMPLED,
  // so strip and scanline sizes reflect full 3-channel RGB instead of
  // subsampled YCbCr.
  ASSERT_TRUE(TIFFIsCODECConfigured(COMPRESSION_JPEG));
  stream.diagnostics().Clear();
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, 2, 2), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB),
            1);
  ASSERT_EQ(TIFFIsUpSampled(tif.get()), 1);
  EXPECT_EQ(TIFFScanlineSize64(tif.get()), 48U);
  EXPECT_EQ(TIFFVStripSize64(tif.get(), 4), 192U);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RAW),
            1);
  ASSERT_EQ(TIFFIsUpSampled(tif.get()), 0);
  EXPECT_EQ(TIFFScanlineSize64(tif.get()), 24U);
  EXPECT_EQ(TIFFVStripSize64(tif.get(), 4), 96U);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifStripTest, StripSizeClampingVStripMinusOneAndTileStrileSize) {
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    SetBasicStripTags(tif.get(), 10, 6, 8, 3, PLANARCONFIG_CONTIG);

    // RowsPerStrip > ImageLength (default -1 or explicit 20) clamps to
    // ImageLength = 6.
    EXPECT_EQ(TIFFStripSize64(tif.get()), 180U);
    EXPECT_EQ(TIFFStripSize(tif.get()), 180);

    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 20), 1);
    EXPECT_EQ(TIFFStripSize64(tif.get()), 180U);

    // RowsPerStrip <= ImageLength uses RowsPerStrip = 4.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 4), 1);
    EXPECT_EQ(TIFFStripSize64(tif.get()), 120U);
    EXPECT_EQ(TIFFStripSize(tif.get()), 120);

    // TIFFVStripSize64 with nrows = -1 expands to td_imagelength = 6.
    EXPECT_EQ(TIFFVStripSize64(tif.get(), static_cast<uint32_t>(-1)), 180U);
    EXPECT_EQ(TIFFVStripSize(tif.get(), static_cast<uint32_t>(-1)), 180);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }

  // Fresh handle (without setting RowsPerStrip, which also populates
  // td_tilelength/td_tilewidth) exercises _TIFFStrileSize64(..., isStrip=FALSE)
  // via TIFFVTileSize64 / TIFFTileSize64.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    SetBasicStripTags(tif.get(), 10, 6, 8, 3, PLANARCONFIG_CONTIG);

    EXPECT_EQ(TIFFVTileSize64(tif.get(), 16), 0U);  // td_tilelength == 0.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, 16), 1);
    EXPECT_EQ(TIFFVTileSize64(tif.get(), 16), 0U);  // td_tilewidth == 0.

    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, 16), 1);
    EXPECT_EQ(TIFFVTileSize64(tif.get(), 16), 768U);
    EXPECT_EQ(TIFFTileSize64(tif.get()), 768U);

    // Tiled YCbCr 2x2 subsampled hits width = td_tilewidth inside
    // _TIFFStrileSize64.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
              1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, 2, 2), 1);
    // 16x16 tile: (16/2)*6 = 48 samples/pair-row * 8 pair-rows = 384 bytes.
    EXPECT_EQ(TIFFVTileSize64(tif.get(), 16), 384U);
    EXPECT_EQ(TIFFTileSize64(tif.get()), 384U);

    // Force td_tiledepth = 0 (rejected by TIFFSetField) to cover the
    // tiledepth == 0 guard in _TIFFStrileSize64.
    tif->tif_dir.td_tiledepth = 0;
    EXPECT_EQ(TIFFVTileSize64(tif.get(), 16), 0U);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifStripTest, IntegerOverflowGuardsInStripSizeFunctions) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  SetBasicStripTags(tif.get(), 0xFFFFFFFFU, 200000000U, 32, 4,
                    PLANARCONFIG_CONTIG);

  // ScanlineSize64 = 0xFFFFFFFF * 16 = 68,719,476,720.
  // 200,000,000 * 68,719,476,720 = 13,743,895,344,000,000,000 (in
  // (INT64_MAX, UINT64_MAX]), so TIFFVStripSize64 succeeds while TIFFVStripSize
  // and TIFFStripSize fail _TIFFCastUInt64ToSSize.
  constexpr uint64_t kExpectedHugeStrip =
      200000000ULL * (uint64_t{0xFFFFFFFFU} * 16ULL);
  EXPECT_EQ(TIFFVStripSize64(tif.get(), 200000000U), kExpectedHugeStrip);
  EXPECT_EQ(TIFFStripSize64(tif.get()), kExpectedHugeStrip);

  EXPECT_EQ(TIFFVStripSize(tif.get(), 200000000U), 0);
  EXPECT_GT(stream.diagnostics().error_count(), 0U);
  stream.diagnostics().Clear();
  EXPECT_EQ(TIFFStripSize(tif.get()), 0);
  EXPECT_GT(stream.diagnostics().error_count(), 0U);

  // Overflow uint64_t inside _TIFFMultiply64 in _TIFFStrileSize64 (else path).
  stream.diagnostics().Clear();
  EXPECT_EQ(TIFFVStripSize64(tif.get(), 0x80000000U), 0U);
  EXPECT_GT(stream.diagnostics().error_count(), 0U);

  // Overflow uint64_t inside _TIFFMultiply64 in _TIFFStrileSize64 (YCbCr path).
  // Use 0xFFFFFFF0U (< 0xFFFFFFFFU) so TIFFhowmany_32(..., 1) does not wrap to
  // 0 and nrows != (uint32_t)(-1).
  stream.diagnostics().Clear();
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 0xFFFFFFF0U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 3), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, 1, 1), 1);
  EXPECT_EQ(TIFFVStripSize64(tif.get(), 0xFFFFFFF0U), 0U);
  EXPECT_GT(stream.diagnostics().error_count(), 0U);
}

TEST(TifStripTest, DefaultStripSizeHeuristicsAndJpegRounding) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  SetBasicStripTags(tif.get(), 64, 128, 8, 1, PLANARCONFIG_CONTIG);

  // Positive request >= 1 is returned directly.
  EXPECT_EQ(TIFFDefaultStripSize(tif.get(), 1), 1U);
  EXPECT_EQ(TIFFDefaultStripSize(tif.get(), 17), 17U);

  // Request < 1 uses STRIP_SIZE_DEFAULT (8192) / scanlinesize.
  EXPECT_EQ(TIFFDefaultStripSize(tif.get(), 0), 128U);
  EXPECT_EQ(TIFFDefaultStripSize(tif.get(), static_cast<uint32_t>(-1)), 128U);

  // Scanlinesize > STRIP_SIZE_DEFAULT clamps rows down to 1.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 10000), 1);
  EXPECT_EQ(TIFFDefaultStripSize(tif.get(), 0), 1U);

  // Zero scanlinesize clamps divisor to 1 -> STRIP_SIZE_DEFAULT rows.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 0), 1);
  EXPECT_EQ(TIFFDefaultStripSize(tif.get(), 0), 8192U);

  // JPEG compression overrides tif_defstripsize to round up to v_sampling * 8.
  ASSERT_TRUE(TIFFIsCODECConfigured(COMPRESSION_JPEG));
  stream.diagnostics().Clear();
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 64), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 3), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, 2, 2), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG), 1);
  // 8192 / 96 = 85 -> rounded up to multiple of (2 * 8) = 96.
  EXPECT_EQ(TIFFDefaultStripSize(tif.get(), 0), 96U);
}

TEST(TifStripTest, RawStripSizeRoundTripClassicAndBigTiffAndEndianness) {
  std::vector<uint8_t> expected0(60);
  for (size_t i = 0; i < expected0.size(); i += 2) {
    expected0[i] = 0x12;
    expected0[i + 1] = 0x34;
  }
  std::vector<uint8_t> expected1(40);
  for (size_t i = 0; i < expected1.size(); i += 2) {
    expected1[i] = 0x56;
    expected1[i + 1] = 0x78;
  }

  for (const char* write_mode : {"wl", "wb", "w8"}) {
    SCOPED_TRACE(write_mode);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(write_mode);
      ASSERT_NE(writer, nullptr);
      SetBasicStripTags(writer.get(), 10, 5, 16, 1, PLANARCONFIG_CONTIG);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 3), 1);

      EXPECT_EQ(TIFFStripSize(writer.get()), 60);
      EXPECT_EQ(TIFFScanlineSize(writer.get()), 20);
      EXPECT_EQ(TIFFNumberOfStrips(writer.get()), 2U);

      // Copy into mutable vectors because libtiff swabs 16-bit write buffers
      // in place on big-endian opens ("wb").
      std::vector<uint8_t> strip0 = expected0;
      std::vector<uint8_t> strip1 = expected1;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip0.data(), 60), 60);
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, strip1.data(), 40), 40);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFIsBigTIFF(reader.get()),
              std::string_view(write_mode) == "w8" ? 1 : 0);
    EXPECT_EQ(TIFFNumberOfStrips(reader.get()), 2U);
    EXPECT_EQ(TIFFStripSize64(reader.get()), 60U);
    EXPECT_EQ(TIFFRawStripSize64(reader.get(), 0), 60U);
    EXPECT_EQ(TIFFRawStripSize(reader.get(), 0), 60);
    EXPECT_EQ(TIFFRawStripSize64(reader.get(), 1), 40U);
    EXPECT_EQ(TIFFRawStripSize(reader.get(), 1), 40);

    std::vector<uint8_t> read0(60, 0);
    std::vector<uint8_t> read1(40, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, read0.data(), 60), 60);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 1, read1.data(), 40), 40);
    EXPECT_EQ(read0, expected0);
    EXPECT_EQ(read1, expected1);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);

    // Out-of-range strip index yields bytecount == 0 -> -1 from both variants.
    EXPECT_EQ(TIFFRawStripSize64(reader.get(), 2), static_cast<uint64_t>(-1));
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFRawStripSize(reader.get(), 2), static_cast<tmsize_t>(-1));
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifStripTest, RawStripSizeZeroByteCountAndBigTiff64BitCount) {
  // ClassicTIFF with 2 strips where strip 1 has byte count 0.
  {
    RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
    const std::vector<uint8_t> payload(16, 0x5A);
    const uint32_t blob_off =
        static_cast<uint32_t>(builder.AppendBlob(payload));
    const std::array<uint32_t, 2> offsets = {blob_off, blob_off};
    const std::array<uint32_t, 2> counts = {16, 0};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 4),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offsets),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 2),
        builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, counts),
    });

    MemoryTiffStream stream(builder.Take());
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFRawStripSize64(tif.get(), 0), 16U);
    EXPECT_EQ(TIFFRawStripSize(tif.get(), 0), 16);
    EXPECT_EQ(TIFFRawStripSize64(tif.get(), 1), static_cast<uint64_t>(-1));
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFRawStripSize(tif.get(), 1), static_cast<tmsize_t>(-1));
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
  }

  // Big-endian BigTIFF with a >4 GiB strip byte count (fits in positive
  // int64_t tmsize_t).
  {
    RawTiffBuilder builder(/*big_endian=*/true, /*big_tiff=*/true);
    const std::vector<uint8_t> payload(16, 0xA5);
    const uint64_t blob_off = builder.AppendBlob(payload);
    const std::array<uint64_t, 2> offsets = {blob_off, blob_off};
    constexpr uint64_t kLargeByteCount = 0x100000005ULL;
    const std::array<uint64_t, 2> counts = {kLargeByteCount, 16};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 4),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong8Array(TIFFTAG_STRIPOFFSETS, offsets),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 2),
        builder.MakeLong8Array(TIFFTAG_STRIPBYTECOUNTS, counts),
    });

    MemoryTiffStream stream(builder.Take());
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFIsBigEndian(tif.get()), 1);
    EXPECT_EQ(TIFFIsBigTIFF(tif.get()), 1);
    EXPECT_EQ(TIFFRawStripSize64(tif.get(), 0), kLargeByteCount);
    EXPECT_EQ(TIFFRawStripSize(tif.get(), 0),
              static_cast<tmsize_t>(kLargeByteCount));
    EXPECT_EQ(TIFFRawStripSize64(tif.get(), 1), 16U);
    EXPECT_EQ(TIFFRawStripSize(tif.get(), 1), 16);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

}  // namespace
}  // namespace libtiff
