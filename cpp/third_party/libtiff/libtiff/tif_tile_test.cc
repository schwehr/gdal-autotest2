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

// Clean-room C++20 unit tests for libtiff tiled image support routines
// (libtiff/tif_tile.c).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_tile.c

#include <array>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

bool ContainsDiagnostic(const std::vector<DiagnosticRecord>& records,
                        std::string_view needle) {
  for (const DiagnosticRecord& rec : records) {
    if (rec.message.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// Opens an empty ClassicTIFF handle in header-only read mode ("rh") so tests
// can safely inspect TIFFDirectory edge cases (such as 0 or UINT32_MAX tile
// dimensions) without triggering directory flush on TIFFClose.
UniqueTiff OpenHeaderOnlyTiff(MemoryTiffStream* stream) {
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  stream->mutable_data() = builder.Take();
  return stream->Open("rh");
}

void ConfigureTiledImage(TIFF* tif, uint32_t width, uint32_t length,
                         uint32_t tile_width, uint32_t tile_length,
                         uint16_t bits_per_sample, uint16_t samples_per_pixel,
                         uint16_t planar_config,
                         uint16_t photometric = PHOTOMETRIC_MINISBLACK,
                         uint16_t compression = COMPRESSION_NONE) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, length), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile_width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILELENGTH, tile_length), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, photometric), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, compression), 1);
}

std::vector<uint8_t> BuildRawTiledTiffBytes(
    bool big_endian, bool big_tiff, uint32_t width, uint32_t length,
    uint32_t tile_width, uint32_t tile_length, uint16_t bits_per_sample,
    uint16_t samples_per_pixel, uint16_t planar_config, uint16_t photometric,
    std::array<uint16_t, 2> ycbcr_subsampling = {2, 2},
    uint32_t image_depth = 1, uint32_t tile_depth = 1) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const uint64_t xpt = (width + tile_width - 1) / tile_width;
  const uint64_t ypt = (length + tile_length - 1) / tile_length;
  const uint64_t zpt = (image_depth + tile_depth - 1) / tile_depth;
  uint64_t ntiles = xpt * ypt * zpt;
  if (planar_config == PLANARCONFIG_SEPARATE) {
    ntiles *= samples_per_pixel;
  }

  std::vector<uint8_t> dummy_tile(16, 0);
  const uint64_t tile_off = builder.AppendBlob(dummy_tile);
  std::vector<uint64_t> offsets(static_cast<size_t>(ntiles), tile_off);
  std::vector<uint64_t> bytecounts(static_cast<size_t>(ntiles),
                                   dummy_tile.size());

  std::vector<RawIfdEntry> entries;
  entries.push_back(builder.MakeLong(TIFFTAG_IMAGEWIDTH, width));
  entries.push_back(builder.MakeLong(TIFFTAG_IMAGELENGTH, length));
  entries.push_back(builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bits_per_sample));
  entries.push_back(builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE));
  entries.push_back(builder.MakeShort(TIFFTAG_PHOTOMETRIC, photometric));
  entries.push_back(
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel));
  entries.push_back(builder.MakeShort(TIFFTAG_PLANARCONFIG, planar_config));
  entries.push_back(builder.MakeLong(TIFFTAG_TILEWIDTH, tile_width));
  entries.push_back(builder.MakeLong(TIFFTAG_TILELENGTH, tile_length));
  if (image_depth > 1) {
    entries.push_back(builder.MakeLong(TIFFTAG_IMAGEDEPTH, image_depth));
  }
  if (tile_depth > 1) {
    entries.push_back(builder.MakeLong(TIFFTAG_TILEDEPTH, tile_depth));
  }
  if (photometric == PHOTOMETRIC_YCBCR) {
    entries.push_back(
        builder.MakeShortArray(TIFFTAG_YCBCRSUBSAMPLING, ycbcr_subsampling));
  }
  if (big_tiff) {
    entries.push_back(builder.MakeLong8Array(TIFFTAG_TILEOFFSETS, offsets));
    entries.push_back(
        builder.MakeLong8Array(TIFFTAG_TILEBYTECOUNTS, bytecounts));
  } else {
    std::vector<uint32_t> off32(offsets.begin(), offsets.end());
    std::vector<uint32_t> bc32(bytecounts.begin(), bytecounts.end());
    entries.push_back(builder.MakeLongArray(TIFFTAG_TILEOFFSETS, off32));
    entries.push_back(builder.MakeLongArray(TIFFTAG_TILEBYTECOUNTS, bc32));
  }

  builder.AddIfd(std::move(entries));
  return builder.Take();
}

int g_global_error_count = 0;
void CountingGlobalErrorHandler(const char* /*module*/, const char* /*fmt*/,
                                va_list /*ap*/) {
  ++g_global_error_count;
}
void SilentGlobalWarningHandler(const char* /*module*/, const char* /*fmt*/,
                                va_list /*ap*/) {}

// ============================================================================
// TIFFDefaultTileSize / _TIFFDefaultTileSize
// ============================================================================

TEST(TifTileTest, DefaultTileSizeClampsNonPositiveAndRoundsToSixteen) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, uint16_t{COMPRESSION_NONE}),
      1);

  struct Case {
    uint32_t in_w;
    uint32_t in_h;
    uint32_t out_w;
    uint32_t out_h;
  };
  constexpr std::array<Case, 8> kCases = {{
      {0, 0, 256, 256},
      {static_cast<uint32_t>(-1), static_cast<uint32_t>(-100), 256, 256},
      {0x80000000U, 0xFFFFFFFFU, 256, 256},
      {1, 15, 16, 16},
      {16, 32, 16, 32},
      {17, 33, 32, 48},
      {256, 257, 256, 272},
      {512, 1024, 512, 1024},
  }};

  for (size_t i = 0; i < kCases.size(); ++i) {
    SCOPED_TRACE(::testing::Message() << "case=" << i);
    uint32_t tw = kCases[i].in_w;
    uint32_t th = kCases[i].in_h;
    TIFFDefaultTileSize(tif.get(), &tw, &th);
    EXPECT_EQ(tw, kCases[i].out_w);
    EXPECT_EQ(th, kCases[i].out_h);

    uint32_t direct_tw = kCases[i].in_w;
    uint32_t direct_th = kCases[i].in_h;
    _TIFFDefaultTileSize(nullptr, &direct_tw, &direct_th);
    EXPECT_EQ(direct_tw, kCases[i].out_w);
    EXPECT_EQ(direct_th, kCases[i].out_h);
  }
}

TEST(TifTileTest, DefaultTileSizeRespectsJpegCodecSubsamplingHook) {
  ASSERT_TRUE(TIFFIsCODECConfigured(COMPRESSION_JPEG));

  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, uint16_t{COMPRESSION_JPEG}),
      1);
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, uint16_t{PHOTOMETRIC_YCBCR}),
      1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, uint16_t{4},
                         uint16_t{2}),
            1);

  // JPEGDefaultTileSize calls _TIFFDefaultTileSize first (16, 16), then rounds
  // up to multiples of (h_sub * DCTSIZE = 32, v_sub * DCTSIZE = 16).
  uint32_t tw = 16;
  uint32_t th = 16;
  TIFFDefaultTileSize(tif.get(), &tw, &th);
  EXPECT_EQ(tw, 32U);
  EXPECT_EQ(th, 16U);
}

// ============================================================================
// TIFFComputeTile / TIFFCheckTile / TIFFNumberOfTiles
// ============================================================================

TEST(TifTileTest, ComputeCheckAndCount2DContigAndSeparate) {
  MemoryTiffStream stream_contig;
  UniqueTiff tif_contig = stream_contig.Open("w");
  ASSERT_NE(tif_contig, nullptr);
  ConfigureTiledImage(tif_contig.get(), /*width=*/50, /*length=*/35,
                      /*tile_width=*/16, /*tile_length=*/16,
                      /*bits_per_sample=*/8, /*samples_per_pixel=*/3,
                      PLANARCONFIG_CONTIG);

  // 4 tiles across x (50 / 16 -> ceil 4), 3 tiles down y (35 / 16 -> ceil 3).
  EXPECT_EQ(TIFFNumberOfTiles(tif_contig.get()), 12U);

  // When ImageDepth == 1, non-zero z is ignored (clamped to 0) by
  // TIFFComputeTile, and PLANARCONFIG_CONTIG ignores s.
  EXPECT_EQ(TIFFComputeTile(tif_contig.get(), 0, 0, 0, 0), 0U);
  EXPECT_EQ(TIFFComputeTile(tif_contig.get(), 15, 15, 99, 2), 0U);
  EXPECT_EQ(TIFFComputeTile(tif_contig.get(), 16, 0, 0, 0), 1U);
  EXPECT_EQ(TIFFComputeTile(tif_contig.get(), 49, 0, 0, 0), 3U);
  EXPECT_EQ(TIFFComputeTile(tif_contig.get(), 32, 16, 42, 5), 6U);
  EXPECT_EQ(TIFFComputeTile(tif_contig.get(), 49, 34, 0, 0), 11U);

  // TIFFCheckTile ignores s under PLANARCONFIG_CONTIG.
  EXPECT_EQ(TIFFCheckTile(tif_contig.get(), 0, 0, 0, 0), 1);
  EXPECT_EQ(TIFFCheckTile(tif_contig.get(), 49, 34, 0, 99), 1);

  MemoryTiffStream stream_sep;
  UniqueTiff tif_sep = stream_sep.Open("w");
  ASSERT_NE(tif_sep, nullptr);
  ConfigureTiledImage(tif_sep.get(), /*width=*/50, /*length=*/35,
                      /*tile_width=*/16, /*tile_length=*/16,
                      /*bits_per_sample=*/8, /*samples_per_pixel=*/3,
                      PLANARCONFIG_SEPARATE);

  EXPECT_EQ(TIFFNumberOfTiles(tif_sep.get()), 36U);
  EXPECT_EQ(TIFFComputeTile(tif_sep.get(), 32, 16, 7, 0), 6U);
  EXPECT_EQ(TIFFComputeTile(tif_sep.get(), 32, 16, 7, 1), 18U);
  EXPECT_EQ(TIFFComputeTile(tif_sep.get(), 49, 34, 0, 2), 35U);
  EXPECT_EQ(TIFFCheckTile(tif_sep.get(), 49, 34, 0, 2), 1);
}

TEST(TifTileTest, CheckTileAndComputeTileOutOfRangeDiagnostics) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ConfigureTiledImage(tif.get(), /*width=*/32, /*length=*/32,
                      /*tile_width=*/16, /*tile_length=*/16,
                      /*bits_per_sample=*/8, /*samples_per_pixel=*/3,
                      PLANARCONFIG_SEPARATE);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEDEPTH, uint32_t{4}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEDEPTH, uint32_t{2}), 1);

  EXPECT_EQ(TIFFCheckTile(tif.get(), 31, 31, 3, 2), 1);

  stream.diagnostics().Clear();
  EXPECT_EQ(TIFFCheckTile(tif.get(), 32, 0, 0, 0), 0);
  EXPECT_TRUE(
      ContainsDiagnostic(stream.diagnostics().errors(), "Col out of range"));

  stream.diagnostics().Clear();
  EXPECT_EQ(TIFFCheckTile(tif.get(), 0, 32, 0, 0), 0);
  EXPECT_TRUE(
      ContainsDiagnostic(stream.diagnostics().errors(), "Row out of range"));

  stream.diagnostics().Clear();
  EXPECT_EQ(TIFFCheckTile(tif.get(), 0, 0, 4, 0), 0);
  EXPECT_TRUE(
      ContainsDiagnostic(stream.diagnostics().errors(), "Depth out of range"));

  stream.diagnostics().Clear();
  EXPECT_EQ(TIFFCheckTile(tif.get(), 0, 0, 0, 3), 0);
  EXPECT_TRUE(
      ContainsDiagnostic(stream.diagnostics().errors(), "Sample out of range"));

  stream.diagnostics().Clear();
  EXPECT_EQ(TIFFComputeTile(tif.get(), 0, 0, 0, 3), 0U);
  EXPECT_TRUE(
      ContainsDiagnostic(stream.diagnostics().errors(), "Sample out of range"));
}

TEST(TifTileTest, CheckTileGlobalErrorHandlerSuppressionCheck) {
  // Verify TIFFCheckTile emits via global error handler when opened without
  // ExtR, while restoring process-wide handlers via ScopedTiffErrorHandlers.
  MemoryTiffStream backing;
  {
    UniqueTiff writer = backing.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureTiledImage(writer.get(), 16, 16, 16, 16, 8, 1,
                        PLANARCONFIG_CONTIG);
    std::vector<uint8_t> tile(256, 0x5A);
    ASSERT_EQ(TIFFWriteTile(writer.get(), tile.data(), 0, 0, 0, 0), 256);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  backing.ResetPosition();
  g_global_error_count = 0;
  ScopedTiffErrorHandlers error_guard(&CountingGlobalErrorHandler,
                                      &SilentGlobalWarningHandler);
  UniqueTiff tif(TIFFClientOpen(
      "global_err_check.tiff", "r", static_cast<thandle_t>(&backing),
      &MemoryTiffStream::ReadProc, &MemoryTiffStream::WriteProc,
      &MemoryTiffStream::SeekProc, &MemoryTiffStream::CloseProc,
      &MemoryTiffStream::SizeProc, nullptr, nullptr));
  ASSERT_NE(tif, nullptr);
  EXPECT_EQ(TIFFCheckTile(tif.get(), 16, 0, 0, 0), 0);
  EXPECT_EQ(g_global_error_count, 1);
}

TEST(TifTileTest, Tiled3DDepthGreaterThanOneContigAndSeparate) {
  for (uint16_t planar : {PLANARCONFIG_CONTIG, PLANARCONFIG_SEPARATE}) {
    SCOPED_TRACE(::testing::Message() << "planar=" << planar);
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ConfigureTiledImage(tif.get(), /*width=*/33, /*length=*/17,
                        /*tile_width=*/16, /*tile_length=*/16,
                        /*bits_per_sample=*/8, /*samples_per_pixel=*/3, planar);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEDEPTH, uint32_t{5}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEDEPTH, uint32_t{2}), 1);

    // xpt = 3, ypt = 2, zpt = 3 -> 18 tiles per sample plane.
    const uint32_t expected_tiles = planar == PLANARCONFIG_SEPARATE ? 54U : 18U;
    EXPECT_EQ(TIFFNumberOfTiles(tif.get()), expected_tiles);

    // Slice z=0..1 -> z_tile=0; z=2..3 -> z_tile=1; z=4 -> z_tile=2.
    EXPECT_EQ(TIFFComputeTile(tif.get(), 0, 0, 0, 0), 0U);
    EXPECT_EQ(TIFFComputeTile(tif.get(), 32, 16, 1, 0), 5U);
    EXPECT_EQ(TIFFComputeTile(tif.get(), 0, 0, 2, 0), 6U);
    EXPECT_EQ(TIFFComputeTile(tif.get(), 16, 16, 3, 0), 10U);
    EXPECT_EQ(TIFFComputeTile(tif.get(), 32, 16, 4, 0), 17U);

    if (planar == PLANARCONFIG_SEPARATE) {
      EXPECT_EQ(TIFFComputeTile(tif.get(), 32, 16, 4, 1), 35U);
      EXPECT_EQ(TIFFComputeTile(tif.get(), 32, 16, 4, 2), 53U);
    } else {
      EXPECT_EQ(TIFFComputeTile(tif.get(), 32, 16, 4, 2), 17U);
    }

    EXPECT_EQ(TIFFCheckTile(tif.get(), 32, 16, 4, 2), 1);
    EXPECT_EQ(TIFFCheckTile(tif.get(), 0, 0, 5, 0), 0);
  }
}

// ============================================================================
// TIFFTileRowSize* / TIFFVTileSize* / TIFFTileSize* across bit depths & planar
// ============================================================================

struct TileSizeCase {
  std::string test_name;
  uint32_t tile_width;
  uint32_t tile_length;
  uint16_t bits_per_sample;
  uint16_t samples_per_pixel;
  uint16_t planar_config;
  uint64_t expected_row_size;
  uint64_t expected_tile_size;
};

class TifTileSizeParamTest : public ::testing::TestWithParam<TileSizeCase> {};

TEST_P(TifTileSizeParamTest, ComputesExpectedRowAndTileSizes) {
  const TileSizeCase& tc = GetParam();
  // Use RawTiffBuilder in "r" mode so both standard (multiple of 16) and
  // nonstandard read-only tile widths (e.g. 17 pixels at 1-bit) are tested.
  std::vector<uint8_t> tiff_bytes = BuildRawTiledTiffBytes(
      /*big_endian=*/false, /*big_tiff=*/false,
      /*width=*/tc.tile_width * 2, /*length=*/tc.tile_length * 2, tc.tile_width,
      tc.tile_length, tc.bits_per_sample, tc.samples_per_pixel,
      tc.planar_config, PHOTOMETRIC_MINISBLACK);

  MemoryTiffStream stream(std::move(tiff_bytes));
  UniqueTiff tif = stream.Open("r");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(TIFFTileRowSize64(tif.get()), tc.expected_row_size);
  EXPECT_EQ(TIFFTileRowSize(tif.get()),
            static_cast<tmsize_t>(tc.expected_row_size));
  EXPECT_EQ(TIFFTileSize64(tif.get()), tc.expected_tile_size);
  EXPECT_EQ(TIFFTileSize(tif.get()),
            static_cast<tmsize_t>(tc.expected_tile_size));
  EXPECT_EQ(TIFFVTileSize64(tif.get(), tc.tile_length), tc.expected_tile_size);
  EXPECT_EQ(TIFFVTileSize(tif.get(), tc.tile_length),
            static_cast<tmsize_t>(tc.expected_tile_size));
  EXPECT_EQ(TIFFVTileSize64(tif.get(), 0), 0ULL);
  EXPECT_EQ(TIFFVTileSize(tif.get(), 0), 0);
  EXPECT_EQ(TIFFVTileSize64(tif.get(), 3), 3ULL * tc.expected_row_size);
  EXPECT_EQ(TIFFVTileSize(tif.get(), 3),
            static_cast<tmsize_t>(3ULL * tc.expected_row_size));
}

INSTANTIATE_TEST_SUITE_P(
    BitDepthsAndPlanarConfigs, TifTileSizeParamTest,
    ::testing::Values(TileSizeCase{"OneBitContig1Spp", 16, 16, 1, 1,
                                   PLANARCONFIG_CONTIG, 2, 32},
                      TileSizeCase{"OneBitUnalignedReadOnly1Spp", 17, 16, 1, 1,
                                   PLANARCONFIG_CONTIG, 3, 48},
                      TileSizeCase{"OneBitContig3Spp", 16, 16, 1, 3,
                                   PLANARCONFIG_CONTIG, 6, 96},
                      TileSizeCase{"OneBitSeparate3Spp", 16, 16, 1, 3,
                                   PLANARCONFIG_SEPARATE, 2, 32},
                      TileSizeCase{"FourBitContig3Spp", 16, 16, 4, 3,
                                   PLANARCONFIG_CONTIG, 24, 384},
                      TileSizeCase{"EightBitContig3Spp", 16, 32, 8, 3,
                                   PLANARCONFIG_CONTIG, 48, 1536},
                      TileSizeCase{"EightBitSeparate3Spp", 16, 32, 8, 3,
                                   PLANARCONFIG_SEPARATE, 16, 512},
                      TileSizeCase{"TwelveBitUnaligned3Spp", 3, 16, 12, 3,
                                   PLANARCONFIG_CONTIG, 14, 224},
                      TileSizeCase{"SixteenBitContig4Spp", 32, 16, 16, 4,
                                   PLANARCONFIG_CONTIG, 256, 4096},
                      TileSizeCase{"SixteenBitSeparate4Spp", 32, 16, 16, 4,
                                   PLANARCONFIG_SEPARATE, 64, 1024},
                      TileSizeCase{"ThirtyTwoBitContig3Spp", 16, 16, 32, 3,
                                   PLANARCONFIG_CONTIG, 192, 3072},
                      TileSizeCase{"ThirtyTwoBitSeparate3Spp", 16, 16, 32, 3,
                                   PLANARCONFIG_SEPARATE, 64, 1024}),
    [](const ::testing::TestParamInfo<TifTileSizeParamTest::ParamType>& info) {
      return info.param.test_name;
    });

// ============================================================================
// YCbCr Subsampling
// ============================================================================

TEST(TifTileTest, YCbCrSubsamplingTileSizesAndInvalidConfigs) {
  struct YCbCrCase {
    uint32_t tile_width;
    uint32_t tile_length;
    uint16_t h_sub;
    uint16_t v_sub;
    uint32_t nrows;
    uint64_t expected_tile_size;
    uint64_t expected_vtile_size;
  };
  constexpr std::array<YCbCrCase, 5> kCases = {{
      // 16x16, 1x1: block=3 samples, 16*3=48 bytes/row * 16 rows = 768.
      {16, 16, 1, 1, 5, 768, 240},
      // 16x16, 2x2: block=6 samples, 8 blocks/row -> 48 bytes * 8 rows = 384.
      // nrows=3 -> ceil(3/2)=2 block rows -> 96 bytes.
      {16, 16, 2, 2, 3, 384, 96},
      // 16x16, 2x1: block=4 samples, 8 blocks/row -> 32 bytes * 16 rows = 512.
      {16, 16, 2, 1, 3, 512, 96},
      // 16x16, 4x2: block=10 samples, 4 blocks/row -> 40 bytes * 8 rows = 320.
      {16, 16, 4, 2, 5, 320, 120},
      // Read-only non-multiple 18x18, 4x4: ceil(18/4)=5 blocks * 18 = 90 bytes
      // per block row * ceil(18/4)=5 block rows = 450.
      {18, 18, 4, 4, 5, 450, 180},
  }};

  for (size_t i = 0; i < kCases.size(); ++i) {
    const YCbCrCase& c = kCases[i];
    SCOPED_TRACE(::testing::Message()
                 << "case=" << i << " sub=" << c.h_sub << "x" << c.v_sub);
    std::vector<uint8_t> tiff_bytes = BuildRawTiledTiffBytes(
        /*big_endian=*/false, /*big_tiff=*/false, c.tile_width, c.tile_length,
        c.tile_width, c.tile_length, /*bits_per_sample=*/8,
        /*samples_per_pixel=*/3, PLANARCONFIG_CONTIG, PHOTOMETRIC_YCBCR,
        {c.h_sub, c.v_sub});
    MemoryTiffStream stream(std::move(tiff_bytes));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    EXPECT_EQ(TIFFTileSize64(tif.get()), c.expected_tile_size);
    EXPECT_EQ(TIFFTileSize(tif.get()),
              static_cast<tmsize_t>(c.expected_tile_size));
    EXPECT_EQ(TIFFVTileSize64(tif.get(), c.nrows), c.expected_vtile_size);
    EXPECT_EQ(TIFFVTileSize(tif.get(), c.nrows),
              static_cast<tmsize_t>(c.expected_vtile_size));
  }

  // Separate YCbCr planes fall back to standard row_size * nrows.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ConfigureTiledImage(tif.get(), 16, 16, 16, 16, 8, 3, PLANARCONFIG_SEPARATE,
                        PHOTOMETRIC_YCBCR);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, uint16_t{2},
                           uint16_t{2}),
              1);
    EXPECT_EQ(TIFFTileRowSize64(tif.get()), 16ULL);
    EXPECT_EQ(TIFFTileSize64(tif.get()), 256ULL);
  }

  // Invalid YCbCr samplesperpixel (!= 3) returns 0 with error.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ConfigureTiledImage(tif.get(), 16, 16, 16, 16, 8, 1, PLANARCONFIG_CONTIG,
                        PHOTOMETRIC_YCBCR);
    EXPECT_EQ(TIFFTileSize64(tif.get()), 0ULL);
    EXPECT_EQ(TIFFTileSize(tif.get()), 0);
    EXPECT_TRUE(ContainsDiagnostic(stream.diagnostics().errors(),
                                   "Invalid td_samplesperpixel"));
  }

  // Invalid YCbCr subsampling factor (e.g. 3x2) returns 0 with error.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = OpenHeaderOnlyTiff(&stream);
    ASSERT_NE(tif, nullptr);
    tif->tif_dir.td_imagewidth = 16;
    tif->tif_dir.td_imagelength = 16;
    tif->tif_dir.td_tilewidth = 16;
    tif->tif_dir.td_tilelength = 16;
    tif->tif_dir.td_tiledepth = 1;
    tif->tif_dir.td_bitspersample = 8;
    tif->tif_dir.td_samplesperpixel = 3;
    tif->tif_dir.td_planarconfig = PLANARCONFIG_CONTIG;
    tif->tif_dir.td_photometric = PHOTOMETRIC_YCBCR;
    tif->tif_dir.td_ycbcrsubsampling[0] = 3;
    tif->tif_dir.td_ycbcrsubsampling[1] = 2;

    EXPECT_EQ(TIFFTileSize64(tif.get()), 0ULL);
    EXPECT_TRUE(ContainsDiagnostic(stream.diagnostics().errors(),
                                   "Invalid YCbCr subsampling"));
  }
}

// ============================================================================
// Endianness & ClassicTIFF / BigTIFF Read/Write Round-Trips
// ============================================================================

struct RoundTripCase {
  std::string test_name;
  std::string_view open_mode;
  uint16_t bits_per_sample;
  uint16_t samples_per_pixel;
  uint16_t planar_config;
  uint32_t image_depth;
};

class TifTileRoundTripTest : public ::testing::TestWithParam<RoundTripCase> {};

TEST_P(TifTileRoundTripTest, WritesAndReadsTilesVerifyingGeometryAndPixels) {
  const RoundTripCase& tc = GetParam();
  constexpr uint32_t kWidth = 20;
  constexpr uint32_t kLength = 18;
  constexpr uint32_t kTileW = 16;
  constexpr uint32_t kTileL = 16;
  constexpr uint32_t kTileD = 1;

  const uint32_t xpt = (kWidth + kTileW - 1) / kTileW;   // 2
  const uint32_t ypt = (kLength + kTileL - 1) / kTileL;  // 2
  const uint32_t planes =
      tc.planar_config == PLANARCONFIG_SEPARATE ? tc.samples_per_pixel : 1U;
  const uint32_t expected_ntiles = xpt * ypt * tc.image_depth * planes;

  const uint64_t bits_per_row =
      static_cast<uint64_t>(kTileW) * tc.bits_per_sample *
      (tc.planar_config == PLANARCONFIG_CONTIG ? tc.samples_per_pixel : 1U);
  const uint64_t expected_row_bytes = (bits_per_row + 7U) / 8U;
  const tmsize_t expected_tile_bytes =
      static_cast<tmsize_t>(expected_row_bytes * kTileL);

  MemoryTiffStream stream;
  std::vector<std::vector<uint8_t>> expected_tiles(expected_ntiles);

  {
    UniqueTiff writer = stream.Open(std::string(tc.open_mode).c_str());
    ASSERT_NE(writer, nullptr);
    ConfigureTiledImage(writer.get(), kWidth, kLength, kTileW, kTileL,
                        tc.bits_per_sample, tc.samples_per_pixel,
                        tc.planar_config);
    if (tc.image_depth > 1) {
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEDEPTH, tc.image_depth),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEDEPTH, kTileD), 1);
    }

    EXPECT_EQ(TIFFNumberOfTiles(writer.get()), expected_ntiles);
    EXPECT_EQ(TIFFTileRowSize64(writer.get()), expected_row_bytes);
    EXPECT_EQ(TIFFTileRowSize(writer.get()),
              static_cast<tmsize_t>(expected_row_bytes));
    EXPECT_EQ(TIFFTileSize64(writer.get()),
              static_cast<uint64_t>(expected_tile_bytes));
    EXPECT_EQ(TIFFTileSize(writer.get()), expected_tile_bytes);

    for (uint16_t s = 0; s < planes; ++s) {
      for (uint32_t z = 0; z < tc.image_depth; ++z) {
        for (uint32_t y = 0; y < kLength; y += kTileL) {
          for (uint32_t x = 0; x < kWidth; x += kTileW) {
            ASSERT_EQ(TIFFCheckTile(writer.get(), x, y, z, s), 1);
            const uint32_t tile_idx = TIFFComputeTile(writer.get(), x, y, z, s);
            ASSERT_LT(tile_idx, expected_ntiles);

            std::vector<uint8_t> tile_data(
                static_cast<size_t>(expected_tile_bytes));
            for (size_t b = 0; b < tile_data.size(); ++b) {
              tile_data[b] = static_cast<uint8_t>((tile_idx * 37U + b) & 0xffU);
            }
            expected_tiles[tile_idx] = tile_data;

            // Pass a mutable copy because TIFFWriteTile swaps in place on BE.
            std::vector<uint8_t> mutable_tile = tile_data;
            ASSERT_EQ(
                TIFFWriteTile(writer.get(), mutable_tile.data(), x, y, z, s),
                expected_tile_bytes);
          }
        }
      }
    }
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(TIFFIsTiled(reader.get()), 1);
  EXPECT_EQ(TIFFIsBigEndian(reader.get()),
            tc.open_mode.find('b') != std::string_view::npos ? 1 : 0);
  EXPECT_EQ(TIFFIsBigTIFF(reader.get()),
            tc.open_mode.find('8') != std::string_view::npos ? 1 : 0);
  EXPECT_EQ(TIFFNumberOfTiles(reader.get()), expected_ntiles);
  EXPECT_EQ(TIFFTileRowSize(reader.get()),
            static_cast<tmsize_t>(expected_row_bytes));
  EXPECT_EQ(TIFFTileSize(reader.get()), expected_tile_bytes);

  for (uint16_t s = 0; s < planes; ++s) {
    for (uint32_t z = 0; z < tc.image_depth; ++z) {
      for (uint32_t y = 0; y < kLength; y += kTileL) {
        for (uint32_t x = 0; x < kWidth; x += kTileW) {
          SCOPED_TRACE(::testing::Message()
                       << "x=" << x << " y=" << y << " z=" << z << " s=" << s);
          const uint32_t tile_idx = TIFFComputeTile(reader.get(), x, y, z, s);
          ASSERT_LT(tile_idx, expected_ntiles);
          std::vector<uint8_t> read_buf(
              static_cast<size_t>(expected_tile_bytes), 0);
          ASSERT_EQ(TIFFReadTile(reader.get(), read_buf.data(), x, y, z, s),
                    expected_tile_bytes);
          EXPECT_EQ(read_buf, expected_tiles[tile_idx]);
        }
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    EndiannessBitnessAndDepth, TifTileRoundTripTest,
    ::testing::Values(RoundTripCase{"ClassicLE1BitContig", "wl", 1, 1,
                                    PLANARCONFIG_CONTIG, 1},
                      RoundTripCase{"ClassicBE8BitContig", "wb", 8, 3,
                                    PLANARCONFIG_CONTIG, 1},
                      RoundTripCase{"ClassicLE16BitSeparate", "wl", 16, 3,
                                    PLANARCONFIG_SEPARATE, 1},
                      RoundTripCase{"ClassicBE32BitContig", "wb", 32, 2,
                                    PLANARCONFIG_CONTIG, 1},
                      RoundTripCase{"BigTiffLE16BitContig3D", "w8l", 16, 2,
                                    PLANARCONFIG_CONTIG, 2},
                      RoundTripCase{"BigTiffBE32BitSeparate3D", "w8b", 32, 3,
                                    PLANARCONFIG_SEPARATE, 2}),
    [](const ::testing::TestParamInfo<TifTileRoundTripTest::ParamType>& info) {
      return info.param.test_name;
    });

// ============================================================================
// Sentinel Values (-1 / 0), Zero Guards, and Integer Overflow Guards
// ============================================================================

TEST(TifTileTest, SentinelMinusOneTileDimensionsAndZeroDimensions) {
  // Public TIFFSetField allows TILEDEPTH = UINT32_MAX (-1), which clamps dz to
  // td_imagedepth in TIFFComputeTile and TIFFNumberOfTiles.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ConfigureTiledImage(tif.get(), 32, 16, 16, 16, 8, 1, PLANARCONFIG_CONTIG);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEDEPTH, uint32_t{4}), 1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_TILEDEPTH, static_cast<uint32_t>(-1)),
        1);

    EXPECT_EQ(TIFFNumberOfTiles(tif.get()), 2U);
    EXPECT_EQ(TIFFComputeTile(tif.get(), 16, 0, 3, 0), 1U);
  }

  // Sentinel -1 for dx/dy/dz via TIFFDirectory: treated as full image dims.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = OpenHeaderOnlyTiff(&stream);
    ASSERT_NE(tif, nullptr);
    tif->tif_dir.td_imagewidth = 40;
    tif->tif_dir.td_imagelength = 30;
    tif->tif_dir.td_imagedepth = 4;
    tif->tif_dir.td_tilewidth = static_cast<uint32_t>(-1);
    tif->tif_dir.td_tilelength = static_cast<uint32_t>(-1);
    tif->tif_dir.td_tiledepth = static_cast<uint32_t>(-1);
    tif->tif_dir.td_samplesperpixel = 3;
    tif->tif_dir.td_planarconfig = PLANARCONFIG_SEPARATE;

    EXPECT_EQ(TIFFNumberOfTiles(tif.get()), 3U);
    EXPECT_EQ(TIFFComputeTile(tif.get(), 39, 29, 3, 0), 0U);
    EXPECT_EQ(TIFFComputeTile(tif.get(), 39, 29, 3, 2), 2U);
  }

  // Zero tile dimensions: TIFFNumberOfTiles, TIFFTileRowSize64, and
  // TIFFVTileSize64 return 0 with appropriate error messages.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{32}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{32}), 1);

    // Default tilewidth=0, tilelength=0.
    EXPECT_EQ(TIFFNumberOfTiles(tif.get()), 0U);
    EXPECT_EQ(TIFFTileSize64(tif.get()), 0ULL);
    EXPECT_EQ(TIFFTileSize(tif.get()), 0);
    // Note: upstream TIFFComputeTile initializes `tile = 1` and returns it
    // unchanged when dx/dy/dz == 0 rather than 0; exercise without asserting 1.
    (void)TIFFComputeTile(tif.get(), 0, 0, 0, 0);

    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFTileRowSize64(tif.get()), 0ULL);
    EXPECT_EQ(TIFFTileRowSize(tif.get()), 0);
    EXPECT_TRUE(ContainsDiagnostic(stream.diagnostics().errors(),
                                   "Tile length is zero"));

    // Set tilelength > 0 while keeping tilewidth == 0.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, uint32_t{16}), 1);
    EXPECT_EQ(TIFFNumberOfTiles(tif.get()), 0U);
    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFTileRowSize64(tif.get()), 0ULL);
    EXPECT_TRUE(ContainsDiagnostic(stream.diagnostics().errors(),
                                   "Tile width is zero"));

    // Zero tilelength, tilewidth, and tiledepth individually in TIFFComputeTile
    // and TIFFNumberOfTiles.
    {
      MemoryTiffStream zstream;
      UniqueTiff ztif = OpenHeaderOnlyTiff(&zstream);
      ASSERT_NE(ztif, nullptr);
      ztif->tif_dir.td_imagewidth = 32;
      ztif->tif_dir.td_imagelength = 32;
      ztif->tif_dir.td_imagedepth = 2;
      ztif->tif_dir.td_tilewidth = 0;
      ztif->tif_dir.td_tilelength = 16;
      ztif->tif_dir.td_tiledepth = 1;
      EXPECT_EQ(TIFFNumberOfTiles(ztif.get()), 0U);
      (void)TIFFComputeTile(ztif.get(), 0, 0, 0, 0);

      ztif->tif_dir.td_tilewidth = 16;
      ztif->tif_dir.td_tilelength = 0;
      EXPECT_EQ(TIFFNumberOfTiles(ztif.get()), 0U);
      (void)TIFFComputeTile(ztif.get(), 0, 0, 0, 0);

      ztif->tif_dir.td_tilelength = 16;
      ztif->tif_dir.td_tiledepth = 0;
      EXPECT_EQ(TIFFNumberOfTiles(ztif.get()), 0U);
      (void)TIFFComputeTile(ztif.get(), 0, 0, 0, 0);

      // Zero imagewidth gives xpt == 0 -> TIFFNumberOfTiles == 0,
      // TIFFComputeTile == 0 without overflow diagnostics.
      ztif->tif_dir.td_tiledepth = 1;
      ztif->tif_dir.td_imagewidth = 0;
      EXPECT_EQ(TIFFNumberOfTiles(ztif.get()), 0U);
      EXPECT_EQ(TIFFComputeTile(ztif.get(), 0, 0, 0, 0), 0U);
      EXPECT_EQ(zstream.diagnostics().error_count(), 0U);
    }

    // Set tilewidth > 0 and bitspersample = 0 -> computed tile row size is 0.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, uint32_t{16}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{0}), 1);
    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFTileRowSize64(tif.get()), 0ULL);
    EXPECT_TRUE(ContainsDiagnostic(stream.diagnostics().errors(),
                                   "Computed tile row size is zero"));
  }

  // Zero samplesperpixel in CONTIG mode and zero tiledepth.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = OpenHeaderOnlyTiff(&stream);
    ASSERT_NE(tif, nullptr);
    tif->tif_dir.td_imagewidth = 32;
    tif->tif_dir.td_imagelength = 32;
    tif->tif_dir.td_tilewidth = 16;
    tif->tif_dir.td_tilelength = 16;
    tif->tif_dir.td_tiledepth = 0;
    tif->tif_dir.td_bitspersample = 8;
    tif->tif_dir.td_samplesperpixel = 0;
    tif->tif_dir.td_planarconfig = PLANARCONFIG_CONTIG;

    EXPECT_EQ(TIFFNumberOfTiles(tif.get()), 0U);
    EXPECT_EQ(TIFFTileSize64(tif.get()), 0ULL);

    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFTileRowSize64(tif.get()), 0ULL);
    EXPECT_TRUE(ContainsDiagnostic(stream.diagnostics().errors(),
                                   "Samples per pixel is zero"));
  }
}

TEST(TifTileTest, OverflowGuardsInNumberOfTilesComputeTileAndTileSize) {
  // 1. xpt * ypt overflow in TIFFComputeTile & TIFFNumberOfTiles.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ConfigureTiledImage(tif.get(), /*width=*/0xF0000000U,
                        /*length=*/0xF0000000U, /*tile_width=*/16,
                        /*tile_length=*/16, 8, 1, PLANARCONFIG_CONTIG);

    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFNumberOfTiles(tif.get()), 0U);
    EXPECT_TRUE(ContainsDiagnostic(stream.diagnostics().errors(),
                                   "Integer overflow in TIFFNumberOfTiles"));

    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFComputeTile(tif.get(), 16, 16, 0, 0), 0U);
    EXPECT_TRUE(ContainsDiagnostic(stream.diagnostics().errors(),
                                   "Integer overflow in TIFFComputeTile"));
  }

  // 2. (xpt * ypt) * zpt overflow in TIFFComputeTile & TIFFNumberOfTiles.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    // xpt = 65536, ypt = 16 -> xpt*ypt = 2^20; zpt = 8192 -> 2^33 overflows.
    ConfigureTiledImage(tif.get(), /*width=*/65536U * 16U,
                        /*length=*/16U * 16U, /*tile_width=*/16,
                        /*tile_length=*/16, 8, 1, PLANARCONFIG_CONTIG);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEDEPTH, uint32_t{8192}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEDEPTH, uint32_t{1}), 1);

    EXPECT_EQ(TIFFNumberOfTiles(tif.get()), 0U);
    EXPECT_EQ(TIFFComputeTile(tif.get(), 0, 0, 0, 0), 0U);
  }

  // 3. Separate planarconfig ntiles * samplesperpixel overflow.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    // xpt = 65536, ypt = 32768 -> xpt*ypt = 2^31 (fits uint32_t); * 3
    // overflows.
    ConfigureTiledImage(tif.get(), /*width=*/65536U * 16U,
                        /*length=*/32768U * 16U, /*tile_width=*/16,
                        /*tile_length=*/16, 8, 3, PLANARCONFIG_SEPARATE);

    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFNumberOfTiles(tif.get()), 0U);
    EXPECT_TRUE(ContainsDiagnostic(stream.diagnostics().errors(),
                                   "Integer overflow in TIFFNumberOfTiles"));
  }

  // 4. uint64_t -> uint32_t cast overflow in TIFFComputeTile.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ConfigureTiledImage(tif.get(), /*width=*/65536U * 16U, /*length=*/16,
                        /*tile_width=*/16, /*tile_length=*/16, 8, 1,
                        PLANARCONFIG_CONTIG);
    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFComputeTile(tif.get(), 0, 65536U * 16U, 0, 0), 0U);
    EXPECT_TRUE(
        ContainsDiagnostic(stream.diagnostics().errors(), "Integer overflow"));
  }

  // 5. uint64_t additions overflow in TIFFComputeTile:
  //    (a) z_offset + y_offset > UINT64_MAX
  //    (b) (z_offset + y_offset) + (x / dx) > UINT64_MAX
  //    (c) sample_offset + tile64 > UINT64_MAX
  {
    MemoryTiffStream stream;
    UniqueTiff tif = OpenHeaderOnlyTiff(&stream);
    ASSERT_NE(tif, nullptr);
    tif->tif_dir.td_imagewidth = 0xFFFFFFFEU;
    tif->tif_dir.td_imagelength = 1;
    tif->tif_dir.td_imagedepth = 2;
    tif->tif_dir.td_tilewidth = 1;
    tif->tif_dir.td_tilelength = 1;
    tif->tif_dir.td_tiledepth = 2;
    tif->tif_dir.td_samplesperpixel = 3;
    tif->tif_dir.td_planarconfig = PLANARCONFIG_SEPARATE;
    // xpt = 0xFFFFFFFE, ypt = 1, zpt = 1, xpt_ypt_zpt = 0xFFFFFFFE.

    // (a) z/2 = 0x7FFFFFFF, y = 0xFFFFFFFF -> z_offset + y_offset overflows.
    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFComputeTile(tif.get(), 0, 0xFFFFFFFFU, 0xFFFFFFFFU, 0), 0U);
    EXPECT_TRUE(ContainsDiagnostic(stream.diagnostics().errors(),
                                   "Integer overflow in TIFFComputeTile"));

    // (b) z = 6 (z/2 = 3), y = 0xFFFFFFFF -> z_offset + y_offset =
    //     UINT64_MAX - 3; adding x = 4 overflows UINT64_MAX.
    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFComputeTile(tif.get(), 4, 0xFFFFFFFFU, 6, 0), 0U);
    EXPECT_TRUE(ContainsDiagnostic(stream.diagnostics().errors(),
                                   "Integer overflow in TIFFComputeTile"));

    // (c) z = 6, y = 0xFFFFFFFF, x = 0 -> tile64 = UINT64_MAX - 3;
    //     adding sample_offset = 0xFFFFFFFE (s = 1) overflows UINT64_MAX.
    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFComputeTile(tif.get(), 0, 0xFFFFFFFFU, 6, 1), 0U);
    EXPECT_TRUE(ContainsDiagnostic(stream.diagnostics().errors(),
                                   "Integer overflow in TIFFComputeTile"));
  }

  // 6. TIFFTileSize / TIFFVTileSize signed tmsize_t overflow when 64-bit size
  //    exceeds TIFF_TMSIZE_T_MAX (INT64_MAX on 64-bit).
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    // Row size = 0x10000000 * 32 * 4 / 8 = 4 GiB; 0x80000000 rows = 2^63 bytes.
    ConfigureTiledImage(tif.get(), /*width=*/0x10000000U,
                        /*length=*/0x80000000U, /*tile_width=*/0x10000000U,
                        /*tile_length=*/0x80000000U, 32, 4,
                        PLANARCONFIG_CONTIG);

    EXPECT_EQ(TIFFTileRowSize64(tif.get()), 0x100000000ULL);
    EXPECT_EQ(TIFFTileSize64(tif.get()), 0x8000000000000000ULL);
    EXPECT_EQ(TIFFVTileSize64(tif.get(), 0x80000000U), 0x8000000000000000ULL);

    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFTileSize(tif.get()), 0);
    EXPECT_TRUE(
        ContainsDiagnostic(stream.diagnostics().errors(), "Integer overflow"));

    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFVTileSize(tif.get(), 0x80000000U), 0);
    EXPECT_TRUE(
        ContainsDiagnostic(stream.diagnostics().errors(), "Integer overflow"));
  }
}

// ============================================================================
// Allocation Limits and Truncated Tiled Streams
// ============================================================================

TEST(TifTileTest, SingleAndCumulatedAllocLimitsAndTruncatedStream) {
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureTiledImage(writer.get(), 32, 32, 16, 16, 8, 1,
                        PLANARCONFIG_CONTIG);
    std::vector<uint8_t> tile(256, 0x3C);
    for (uint32_t y = 0; y < 32; y += 16) {
      for (uint32_t x = 0; x < 32; x += 16) {
        std::vector<uint8_t> copy = tile;
        ASSERT_EQ(TIFFWriteTile(writer.get(), copy.data(), x, y, 0, 0), 256);
      }
    }
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  // Open with MaxCumulatedMemAlloc > 0 so ScopedCumulatedMemAllocRemaining is
  // safe to tighten and restore.
  {
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    UniqueTiff reader = stream.Open("r", opts.get());
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFNumberOfTiles(reader.get()), 4U);
    EXPECT_EQ(TIFFTileSize(reader.get()), 256);

    std::vector<uint8_t> out(256, 0);
    {
      ScopedSingleMemAllocLimit single_guard(reader.get(), 16);
      EXPECT_EQ(TIFFReadTile(reader.get(), out.data(), 0, 0, 0, 0),
                static_cast<tmsize_t>(-1));
    }
    {
      ScopedCumulatedMemAllocRemaining cum_guard(reader.get(), 8);
      EXPECT_EQ(TIFFReadTile(reader.get(), out.data(), 16, 0, 0, 0),
                static_cast<tmsize_t>(-1));
    }

    // After guards unwind, reading succeeds normally.
    EXPECT_EQ(TIFFReadTile(reader.get(), out.data(), 0, 0, 0, 0), 256);
    EXPECT_EQ(out[0], 0x3C);
  }

  // Truncated/short tile bytecount stream: IFD metadata loads cleanly while
  // TIFFReadTile fails cleanly returning -1 without leaks.
  {
    std::vector<uint8_t> short_tile_bytes = BuildRawTiledTiffBytes(
        /*big_endian=*/false, /*big_tiff=*/false, /*width=*/16, /*length=*/16,
        /*tile_width=*/16, /*tile_length=*/16, /*bits_per_sample=*/8,
        /*samples_per_pixel=*/1, PLANARCONFIG_CONTIG, PHOTOMETRIC_MINISBLACK);
    MemoryTiffStream trunc_stream(std::move(short_tile_bytes));
    UniqueTiff trunc_reader = trunc_stream.Open("r");
    ASSERT_NE(trunc_reader, nullptr);
    EXPECT_EQ(TIFFNumberOfTiles(trunc_reader.get()), 1U);
    EXPECT_EQ(TIFFTileSize(trunc_reader.get()), 256);
    std::vector<uint8_t> out(256, 0);
    EXPECT_EQ(TIFFReadTile(trunc_reader.get(), out.data(), 0, 0, 0, 0),
              static_cast<tmsize_t>(-1));
  }

  // Simulated stream read failure during tile read.
  {
    MemoryTiffStream fail_stream(stream.data());
    UniqueTiff fail_reader = fail_stream.Open("r");
    ASSERT_NE(fail_reader, nullptr);
    fail_stream.FailReadsAfter(fail_stream.read_calls());
    std::vector<uint8_t> out(256, 0);
    EXPECT_EQ(TIFFReadTile(fail_reader.get(), out.data(), 0, 0, 0, 0),
              static_cast<tmsize_t>(-1));
  }
}

}  // namespace
}  // namespace libtiff
