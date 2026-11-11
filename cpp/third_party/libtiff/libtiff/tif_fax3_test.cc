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

// Clean-room C++20 unit tests for libtiff/tif_fax3.c covering CCITT Group 3
// (1D/2D), Group 4, and Modified Huffman RLE / word-aligned RLE codecs, tag
// handlers, printdir formatting, setup validation/OOM/overflow, encoding,
// decoding, _TIFFFax3fillruns, and corrupt/EOF/threshold error paths.
//
// Note: tif_fax3.h declares TIFFFaxMainTable, TIFFFaxWhiteTable, and
// TIFFFaxBlackTable outside its extern "C" block, so it is included at global
// scope before tiffiop.h and namespace libtiff.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
// Include tif_fax3.h after gmock/gunit so its GetBits macro does not collide
// with transitive headers under --config=no_modules.
#include "third_party/tiff/libtiff/tif_fax3.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;

std::vector<uint8_t> BuildSingleStripFaxTiff(
    uint16_t compression, uint32_t width, uint32_t height,
    std::vector<uint8_t> strip_bytes, uint32_t group3_options = 0,
    bool has_group3_options = false, uint32_t group4_options = 0,
    bool has_group4_options = false, uint16_t fill_order = FILLORDER_MSB2LSB,
    uint16_t bits_per_sample = 1, uint16_t samples_per_pixel = 1,
    uint16_t planar_config = PLANARCONFIG_CONTIG,
    std::vector<RawIfdEntry> extra_entries = {}) {
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  const uint64_t offset = builder.AppendBlob(strip_bytes);
  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bits_per_sample),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
      builder.MakeShort(TIFFTAG_FILLORDER, fill_order),
      builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(offset)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, height),
      builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                       static_cast<uint32_t>(strip_bytes.size())),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, planar_config),
  };
  if (has_group3_options) {
    entries.push_back(builder.MakeLong(TIFFTAG_GROUP3OPTIONS, group3_options));
  }
  if (has_group4_options) {
    entries.push_back(builder.MakeLong(TIFFTAG_GROUP4OPTIONS, group4_options));
  }
  for (auto& e : extra_entries) {
    entries.push_back(std::move(e));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint8_t> BuildSingleTileFaxTiff(uint16_t compression,
                                            uint32_t width, uint32_t height,
                                            std::vector<uint8_t> tile_bytes,
                                            uint32_t group3_options = 0,
                                            bool has_group3_options = false) {
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  const uint64_t offset = builder.AppendBlob(tile_bytes);
  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 1),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
      builder.MakeShort(TIFFTAG_FILLORDER, FILLORDER_MSB2LSB),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
      builder.MakeLong(TIFFTAG_TILEWIDTH, width),
      builder.MakeLong(TIFFTAG_TILELENGTH, height),
      builder.MakeLong(TIFFTAG_TILEOFFSETS, static_cast<uint32_t>(offset)),
      builder.MakeLong(TIFFTAG_TILEBYTECOUNTS,
                       static_cast<uint32_t>(tile_bytes.size())),
  };
  if (has_group3_options) {
    entries.push_back(builder.MakeLong(TIFFTAG_GROUP3OPTIONS, group3_options));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::string PrintDirectoryToString(TIFF* tif,
                                   long flags = 0) {  // NOLINT(runtime/int)
  char* buf = nullptr;
  size_t len = 0;
  FILE* mem = open_memstream(&buf, &len);
  auto cleanup_buf = absl::MakeCleanup([&buf] { std::free(buf); });
  if (mem == nullptr) {
    ADD_FAILURE() << "open_memstream failed";
    return {};
  }
  {
    auto close_mem = absl::MakeCleanup([mem] { std::fclose(mem); });
    TIFFPrintDirectory(tif, mem, flags);
  }
  return std::string(buf != nullptr ? buf : "", len);
}

void InvertingFillRuns(unsigned char* buf, uint32_t* runs, uint32_t* erun,
                       uint32_t lastx) {
  _TIFFFax3fillruns(buf, runs, erun, lastx);
  const uint32_t nbytes = (lastx + 7U) / 8U;
  for (uint32_t i = 0; i < nbytes; ++i) {
    buf[i] ^= 0xffU;
  }
}

void VerifyScanlineRoundTrip(uint16_t compression, uint32_t width,
                             uint32_t height,
                             const std::vector<std::vector<uint8_t>>& rows,
                             uint16_t fill_order = FILLORDER_MSB2LSB,
                             uint32_t group3_options = 0, int fax_mode = -1,
                             uint16_t res_unit = RESUNIT_INCH,
                             float y_resolution = 0.0f,
                             const char* write_mode = "wl",
                             uint32_t rows_per_strip = 0) {
  ASSERT_EQ(rows.size(), height);
  const size_t row_bytes = (width + 7U) / 8U;
  for (uint32_t r = 0; r < height; ++r) {
    ASSERT_EQ(rows[r].size(), row_bytes);
  }

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open(write_mode);
    ASSERT_NE(writer.get(), nullptr);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, width), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, height), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, compression), 1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
        1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_FILLORDER, fill_order), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP,
                           rows_per_strip > 0 ? rows_per_strip : height),
              1);
    if (compression == COMPRESSION_CCITTFAX3 && group3_options != 0) {
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_GROUP3OPTIONS, group3_options), 1);
    }
    if (fax_mode >= 0) {
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_FAXMODE, fax_mode), 1);
    }
    if (y_resolution > 0.0f) {
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_RESOLUTIONUNIT, res_unit),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_YRESOLUTION,
                             static_cast<double>(y_resolution)),
                1);
    }

    for (uint32_t r = 0; r < height; ++r) {
      std::vector<uint8_t> mutable_row = rows[r];
      ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row.data(), r, 0), 1);
    }
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader.get(), nullptr);
  if (fax_mode == FAXMODE_NOEOL) {
    EXPECT_EQ(TIFFSetField(reader.get(), TIFFTAG_FAXMODE, FAXMODE_NOEOL), 1);
  }
  ASSERT_EQ(TIFFScanlineSize(reader.get()), static_cast<tmsize_t>(row_bytes));
  std::vector<uint8_t> got(row_bytes, 0);
  for (uint32_t r = 0; r < height; ++r) {
    SCOPED_TRACE(testing::Message() << "row=" << r << " comp=" << compression);
    std::fill(got.begin(), got.end(), 0);
    ASSERT_EQ(TIFFReadScanline(reader.get(), got.data(), r, 0), 1);
    EXPECT_EQ(got, rows[r]);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

void VerifyMaskTailEqual(absl::Span<const uint8_t> got,
                         absl::Span<const uint8_t> expected, uint32_t width) {
  ASSERT_EQ(got.size(), expected.size());
  const size_t full_bytes = width / 8U;
  for (size_t i = 0; i < full_bytes; ++i) {
    EXPECT_EQ(got[i], expected[i]) << "byte " << i;
  }
  const uint32_t rem = width % 8U;
  if (rem > 0) {
    const uint8_t mask = static_cast<uint8_t>(0xffU << (8U - rem));
    EXPECT_EQ(got[full_bytes] & mask, expected[full_bytes] & mask);
  }
}

void CustomPrintDirHook(TIFF* /*tif*/, FILE* fd,
                        long /*flags*/) {  // NOLINT(runtime/int)
  std::fprintf(fd, "  CustomParentPrintDir: active\n");
}

void InstallParentPrintDirExtender(TIFF* tif) {
  tif->tif_tagmethods.printdir = &CustomPrintDirHook;
}

TEST(TifFax3Test, CodecRegistrationDefaultsAndMaxCompressionRatio) {
  for (uint16_t scheme : {COMPRESSION_CCITTFAX3, COMPRESSION_CCITTFAX4,
                          COMPRESSION_CCITTRLE, COMPRESSION_CCITTRLEW}) {
    EXPECT_EQ(TIFFIsCODECConfigured(scheme), 1);
    EXPECT_NE(TIFFFindCODEC(scheme), nullptr);
  }

  struct SchemeExpectation {
    uint16_t scheme;
    int expected_mode;
    uint64_t expected_strip_ratio;
  };
  constexpr std::array<SchemeExpectation, 4> kCases = {{
      {COMPRESSION_CCITTFAX3, FAXMODE_CLASSF, 250ULL},
      {COMPRESSION_CCITTFAX4, FAXMODE_NORTC, 320ULL},
      {COMPRESSION_CCITTRLE, FAXMODE_NORTC | FAXMODE_NOEOL | FAXMODE_BYTEALIGN,
       250ULL},
      {COMPRESSION_CCITTRLEW, FAXMODE_NORTC | FAXMODE_NOEOL | FAXMODE_WORDALIGN,
       250ULL},
  }};

  for (const auto& c : kCases) {
    SCOPED_TRACE(testing::Message() << "scheme=" << c.scheme);
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{320}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{8}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, c.scheme), 1);

    int mode = -1;
    ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_FAXMODE, &mode), 1);
    EXPECT_EQ(mode, c.expected_mode);

    TIFFFaxFillFunc fill = nullptr;
    ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_FAXFILLFUNC, &fill), 1);
    EXPECT_EQ(fill, _TIFFFax3fillruns);

    EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), c.expected_strip_ratio);
  }

  // Verify Tiled CCITTFAX4 uses tilewidth for GetMaxCompressionRatio and
  // Fax3Cleanup runs cleanly when switching away from CCITTFAX4 after setup.
  {
    MemoryTiffStream stream;
    std::vector<uint8_t> tile(16 * (128 / 8), 0);
    {
      UniqueTiff tif = stream.Open("wl");
      ASSERT_NE(tif.get(), nullptr);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{512}), 1);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{64}), 1);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}), 1);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
                1);
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4),
          1);
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
          1);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, uint32_t{128}), 1);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, uint32_t{16}), 1);
      EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 128ULL);
      for (uint32_t t = 0; t < TIFFNumberOfTiles(tif.get()); ++t) {
        std::vector<uint8_t> mutable_tile = tile;
        ASSERT_EQ(TIFFWriteEncodedTile(tif.get(), t, mutable_tile.data(),
                                       static_cast<tmsize_t>(tile.size())),
                  static_cast<tmsize_t>(tile.size()));
      }
    }

    // Reopen for read, decode tile 0 so runs and refline are allocated, then
    // switch to COMPRESSION_NONE (allowed since TIFF_BEENWRITING is not set)
    // to exercise Fax3Cleanup freeing runs, refline, and tif_data.
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> decoded(tile.size(), 0);
    ASSERT_EQ(TIFFReadEncodedTile(reader.get(), 0, decoded.data(),
                                  static_cast<tmsize_t>(decoded.size())),
              static_cast<tmsize_t>(decoded.size()));
    EXPECT_EQ(decoded, tile);
    EXPECT_EQ(TIFFSetField(reader.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    EXPECT_EQ(reader->tif_data, nullptr);
  }
}

TEST(TifFax3Test, TagSetGetRoundTripMismatchAndCustomFillFunc) {
  // 1. Set, get, persist, and re-read Fax tags on CCITTFAX3.
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{2}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX3),
        1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
        1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{2}), 1);

    constexpr uint32_t kG3Opts = GROUP3OPT_2DENCODING | GROUP3OPT_FILLBITS;
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_GROUP3OPTIONS, kG3Opts), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_CLEANFAXDATA,
                           uint16_t{CLEANFAXDATA_REGENERATED}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BADFAXLINES, uint32_t{5}), 1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_CONSECUTIVEBADFAXLINES, uint32_t{3}),
        1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_FAXMODE, FAXMODE_NORTC), 1);

    uint32_t got_u32 = 0;
    uint16_t got_u16 = 0;
    int got_mode = 0;
    EXPECT_EQ(TIFFGetField(writer.get(), TIFFTAG_GROUP3OPTIONS, &got_u32), 1);
    EXPECT_EQ(got_u32, kG3Opts);
    EXPECT_EQ(TIFFGetField(writer.get(), TIFFTAG_CLEANFAXDATA, &got_u16), 1);
    EXPECT_EQ(got_u16, CLEANFAXDATA_REGENERATED);
    EXPECT_EQ(TIFFGetField(writer.get(), TIFFTAG_BADFAXLINES, &got_u32), 1);
    EXPECT_EQ(got_u32, 5U);
    EXPECT_EQ(
        TIFFGetField(writer.get(), TIFFTAG_CONSECUTIVEBADFAXLINES, &got_u32),
        1);
    EXPECT_EQ(got_u32, 3U);
    EXPECT_EQ(TIFFGetField(writer.get(), TIFFTAG_FAXMODE, &got_mode), 1);
    EXPECT_EQ(got_mode, FAXMODE_NORTC);
    // Verify non-fax tags chain cleanly through Fax3VGetField vgetparent.
    EXPECT_EQ(TIFFGetField(writer.get(), TIFFTAG_IMAGEWIDTH, &got_u32), 1);
    EXPECT_EQ(got_u32, 16U);

    std::array<uint8_t, 2> row0 = {0xaa, 0x55};
    std::array<uint8_t, 2> row1 = {0x00, 0xff};
    ASSERT_EQ(TIFFWriteScanline(writer.get(), row0.data(), 0, 0), 1);
    ASSERT_EQ(TIFFWriteScanline(writer.get(), row1.data(), 1, 0), 1);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  {
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    uint32_t got_u32 = 0;
    uint16_t got_u16 = 0;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_GROUP3OPTIONS, &got_u32), 1);
    EXPECT_EQ(got_u32, GROUP3OPT_2DENCODING | GROUP3OPT_FILLBITS);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_CLEANFAXDATA, &got_u16), 1);
    EXPECT_EQ(got_u16, CLEANFAXDATA_REGENERATED);
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_BADFAXLINES, &got_u32), 1);
    EXPECT_EQ(got_u32, 5U);
    EXPECT_EQ(
        TIFFGetField(reader.get(), TIFFTAG_CONSECUTIVEBADFAXLINES, &got_u32),
        1);
    EXPECT_EQ(got_u32, 3U);

    // Override TIFFTAG_FAXFILLFUNC and verify decoded output is bit-inverted.
    EXPECT_EQ(
        TIFFSetField(reader.get(), TIFFTAG_FAXFILLFUNC, &InvertingFillRuns), 1);
    TIFFFaxFillFunc got_fill = nullptr;
    EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_FAXFILLFUNC, &got_fill), 1);
    EXPECT_EQ(got_fill, &InvertingFillRuns);

    std::array<uint8_t, 2> decoded{};
    ASSERT_EQ(TIFFReadScanline(reader.get(), decoded.data(), 0, 0), 1);
    EXPECT_EQ(decoded, (std::array<uint8_t, 2>{0x55, 0xaa}));
    ASSERT_EQ(TIFFReadScanline(reader.get(), decoded.data(), 1, 0), 1);
    EXPECT_EQ(decoded, (std::array<uint8_t, 2>{0xff, 0x00}));
  }

  // 2. Switching compression preserves earlier registered field definitions so
  // Fax3VSetField executes the compression-mismatch guards safely.
  {
    MemoryTiffStream s;
    UniqueTiff tif = s.Open("wl");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX3), 1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4), 1);
    // GROUP3OPTIONS is merged, but td_compression != CCITTFAX3 -> ignored.
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_GROUP3OPTIONS, GROUP3OPT_2DENCODING),
        1);
    uint32_t opts = 999U;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_GROUP3OPTIONS, &opts), 1);
    EXPECT_EQ(opts, 0U);

    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX3), 1);
    // GROUP4OPTIONS is merged, but td_compression != CCITTFAX4 -> ignored.
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_GROUP4OPTIONS, GROUP4OPT_UNCOMPRESSED),
        1);
    opts = 999U;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_GROUP4OPTIONS, &opts), 1);
    EXPECT_EQ(opts, 0U);
  }
}

TEST(TifFax3Test, PrintDirCoversAllOptionsCleanFaxDataAndParentChaining) {
  // 1. G3 with all options (2D + fillbits + uncompressed), CLEAN, BadFaxLines,
  // and ConsecutiveBadFaxLines, plus parent printdir chaining.
  {
    ScopedTagExtender extender(&InstallParentPrintDirExtender);
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{32}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}), 1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX3), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_GROUP3OPTIONS,
                           GROUP3OPT_2DENCODING | GROUP3OPT_FILLBITS |
                               GROUP3OPT_UNCOMPRESSED),
              1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_CLEANFAXDATA,
                           uint16_t{CLEANFAXDATA_CLEAN}),
              1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BADFAXLINES, uint32_t{12}), 1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_CONSECUTIVEBADFAXLINES, uint32_t{4}),
        1);

    const std::string printed = PrintDirectoryToString(tif.get());
    EXPECT_THAT(printed, HasSubstr("Group 3 Options: 2-d encoding+EOL padding+"
                                   "uncompressed data (7 = 0x7)"));
    EXPECT_THAT(printed, HasSubstr("Fax Data: clean (0 = 0x0)"));
    EXPECT_THAT(printed, HasSubstr("Bad Fax Lines: 12"));
    EXPECT_THAT(printed, HasSubstr("Consecutive Bad Fax Lines: 4"));
    EXPECT_THAT(printed, HasSubstr("CustomParentPrintDir: active"));
  }

  // 2. G3 with only FILLBITS (tests initial sep = " ") and REGENERATED.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX3), 1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_GROUP3OPTIONS, GROUP3OPT_FILLBITS), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_CLEANFAXDATA,
                           uint16_t{CLEANFAXDATA_REGENERATED}),
              1);

    const std::string printed = PrintDirectoryToString(tif.get());
    EXPECT_THAT(printed, HasSubstr("Group 3 Options: EOL padding (4 = 0x4)"));
    EXPECT_THAT(printed, HasSubstr("Fax Data: receiver regenerated (1 = 0x1)"));
  }

  // 3. G4 with GROUP4OPT_UNCOMPRESSED and UNCLEAN.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4), 1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_GROUP4OPTIONS, GROUP4OPT_UNCOMPRESSED),
        1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_CLEANFAXDATA,
                           uint16_t{CLEANFAXDATA_UNCLEAN}),
              1);

    const std::string printed = PrintDirectoryToString(tif.get());
    EXPECT_THAT(printed,
                HasSubstr("Group 4 Options: uncompressed data (2 = 0x2)"));
    EXPECT_THAT(printed, HasSubstr("Fax Data: uncorrected errors (2 = 0x2)"));
  }

  // 4. Unknown CLEANFAXDATA default branch + G4 without GROUP4OPT_UNCOMPRESSED.
  {
    MemoryTiffStream stream(BuildSingleStripFaxTiff(
        COMPRESSION_CCITTFAX4, 16, 1, {0x00, 0x10, 0x01}, 0, false,
        /*group4_options=*/0, /*has_group4_options=*/true));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_CLEANFAXDATA, uint16_t{99}), 1);
    const std::string printed = PrintDirectoryToString(tif.get());
    EXPECT_THAT(printed, HasSubstr("Group 4 Options: (0 = 0x0)"));
    EXPECT_THAT(printed, HasSubstr("Fax Data: (99 = 0x63)"));
  }

  // 5. G3 with no fax option/badfax/cleanfax tags set.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX3), 1);
    const std::string printed = PrintDirectoryToString(tif.get());
    EXPECT_THAT(printed, testing::Not(HasSubstr("Group 3 Options:")));
    EXPECT_THAT(printed, testing::Not(HasSubstr("Fax Data:")));
    EXPECT_THAT(printed, testing::Not(HasSubstr("Bad Fax Lines:")));
    EXPECT_THAT(printed, testing::Not(HasSubstr("Consecutive Bad Fax Lines:")));
  }
}

TEST(TifFax3Test, SetupStateValidationOverflowAndOom) {
  // 1. Reject BitsPerSample != 1 on write and read.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX3),
        1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
        1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{1}), 1);
    std::array<uint8_t, 16> row{};
    EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), -1);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }
  {
    MemoryTiffStream stream(BuildSingleStripFaxTiff(
        COMPRESSION_CCITTFAX3, 16, 1, {0x00, 0x10}, 0, false, 0, false,
        FILLORDER_MSB2LSB, /*bits_per_sample=*/8, /*samples_per_pixel=*/1));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 16> row{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 2. Reject SamplesPerPixel != 1 with PlanarConfig == CONTIG.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
              1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX3),
        1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
        1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{1}), 1);
    std::array<uint8_t, 8> row{};
    EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), -1);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }
  {
    MemoryTiffStream stream(BuildSingleStripFaxTiff(
        COMPRESSION_CCITTFAX4, 16, 1, {0x80}, 0, false, 0, false,
        FILLORDER_MSB2LSB, /*bits_per_sample=*/1, /*samples_per_pixel=*/2,
        PLANARCONFIG_CONTIG));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 8> row{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 3. Accept and round-trip SamplesPerPixel = 2 with PLANARCONFIG_SEPARATE.
  {
    constexpr uint32_t kWidth = 32;
    constexpr uint32_t kHeight = 2;
    const std::array<std::array<uint8_t, 4>, 4> planes = {{
        {0xf0, 0x0f, 0xaa, 0x55},
        {0x00, 0xff, 0x00, 0xff},
        {0x12, 0x34, 0x56, 0x78},
        {0xff, 0xfe, 0xfc, 0xf8},
    }};
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kWidth), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kHeight), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
                1);
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{2}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             PLANARCONFIG_SEPARATE),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             COMPRESSION_CCITTFAX3),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             PHOTOMETRIC_MINISWHITE),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{2}),
                1);
      for (uint16_t s = 0; s < 2; ++s) {
        for (uint32_t r = 0; r < kHeight; ++r) {
          auto buf = planes[s * kHeight + r];
          ASSERT_EQ(TIFFWriteScanline(writer.get(), buf.data(), r, s), 1);
        }
      }
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    for (uint16_t s = 0; s < 2; ++s) {
      for (uint32_t r = 0; r < kHeight; ++r) {
        std::array<uint8_t, 4> got{};
        ASSERT_EQ(TIFFReadScanline(reader.get(), got.data(), r, s), 1);
        EXPECT_EQ(got, planes[s * kHeight + r]);
      }
    }
  }

  // 4. Reject inconsistent rowbytes < (rowpixels + 7)/8 (TileLength == 0 makes
  // TIFFTileRowSize return 0 while rowpixels == TileWidth > 0).
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX3), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, uint32_t{16}), 1);
    // TileLength deliberately unset (= 0) -> TIFFTileRowSize returns 0.
    EXPECT_EQ(tif->tif_setupencode(tif.get()), 0);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 5. Reject integer overflows in Fax3SetupState run array sizing.
  for (const auto& [comp, width] : std::array<std::pair<uint16_t, uint32_t>, 3>{
           {{COMPRESSION_CCITTFAX3, 0xffffffffU},
            {COMPRESSION_CCITTFAX3, 0x80000000U},
            {COMPRESSION_CCITTFAX4, 0x40000000U}}}) {
    SCOPED_TRACE(testing::Message() << "comp=" << comp << " width=" << width);
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, width), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, comp), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{1}), 1);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 16), 1);
    std::array<uint8_t, 8> dummy{};
    EXPECT_EQ(TIFFWriteScanline(tif.get(), dummy.data(), 0, 0), -1);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 6. OOM in InitCCITTFax3 (_TIFFmallocExt(Fax3CodecState)) using
  // COMPRESSION_CCITTFAX4, CCITTRLE, and CCITTRLEW (not CCITTFAX3, due to
  // upstream tif_fax3.c:1627 bug). Use TIFFOpenOptionsSetMaxCumulatedMemAlloc
  // > 0 on open before narrowing.
  for (uint16_t scheme :
       {COMPRESSION_CCITTFAX4, COMPRESSION_CCITTRLE, COMPRESSION_CCITTRLEW}) {
    SCOPED_TRACE(testing::Message() << "scheme=" << scheme);
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts.get(), nullptr);
    diag.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif.get(), nullptr);

    // Budget sufficient for _TIFFMergeFields(faxFields, 5) delta (~40 bytes)
    // but strictly below sizeof(Fax3CodecState).
    constexpr tmsize_t kMergeFaxFieldsOnlyBudget =
        static_cast<tmsize_t>(5 * sizeof(void*) + 16);
    {
      ScopedCumulatedMemAllocRemaining budget(tif.get(),
                                              kMergeFaxFieldsOnlyBudget);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, scheme), 0);
    }
    EXPECT_EQ(tif->tif_data, nullptr);
    EXPECT_GE(diag.error_count(), 1U);
  }

  // 7. OOM in Fax3SetupState: first fail dsp->runs via single-alloc limit,
  // then fail esp->refline on a fresh handle via cumulated budget while
  // dsp->runs succeeds, then retry via TIFFWriteEncodedStrip with full budget
  // to exercise freeing existing dsp->runs (lines 640-644). Note:
  // TIFFWriteScanline caches td_curstrip=0 before setupencode fails, so
  // TIFFWriteEncodedStrip is used for the clean retry.
  {
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts.get(), nullptr);
    diag.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif.get(), nullptr);

    constexpr uint32_t kWidth = 256;
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, kWidth), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4), 1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
        1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{1}), 1);
    ASSERT_EQ(TIFFSetupStrips(tif.get()), 1);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 16), 1);

    std::array<uint8_t, kWidth / 8> row{};
    {
      ScopedSingleMemAllocLimit single_limit(tif.get(), 128);
      EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, row.data(),
                                      static_cast<tmsize_t>(row.size())),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_GE(diag.error_count(), 1U);
  }
  {
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts.get(), nullptr);
    diag.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif.get(), nullptr);

    constexpr uint32_t kWidth = 256;
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, kWidth), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4), 1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
        1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{1}), 1);
    ASSERT_EQ(TIFFSetupStrips(tif.get()), 1);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 16), 1);

    std::array<uint8_t, kWidth / 8> row{};
    // For CCITTFAX4 at width=256: nruns = 2 * 288 = 576; runs = 4608 bytes;
    // refline = 32 bytes.
    const tmsize_t exact_runs_bytes = static_cast<tmsize_t>(
        2U * 2U * TIFFroundup_32(kWidth + 1, 32) * sizeof(uint32_t));
    {
      ScopedCumulatedMemAllocRemaining budget(tif.get(), exact_runs_bytes);
      EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, row.data(),
                                      static_cast<tmsize_t>(row.size())),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_GE(diag.error_count(), 1U);

    // Retry with budget restored: frees previous dsp->runs and succeeds.
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, row.data(),
                                    static_cast<tmsize_t>(row.size())),
              static_cast<tmsize_t>(row.size()));
  }
}

TEST(TifFax3Test, EncodeAndDecodeRoundTripsAllSchemesAndOptions) {
  // 1. Small/non-byte-multiple widths across G3-1D, G3-2D, G4, RLE, RLEW.
  // Use 1 row per strip for CCITTRLEW per upstream tif_fax3.c:1835-1841 bug.
  for (uint32_t width :
       {uint32_t{1}, uint32_t{5}, uint32_t{13}, uint32_t{31}, uint32_t{133}}) {
    const size_t row_bytes = (width + 7U) / 8U;
    std::vector<std::vector<uint8_t>> rows(4, std::vector<uint8_t>(row_bytes));
    std::fill(rows[0].begin(), rows[0].end(), 0x00);
    std::fill(rows[1].begin(), rows[1].end(), 0xff);
    for (size_t i = 0; i < row_bytes; ++i) {
      rows[2][i] = static_cast<uint8_t>(0xaaU);
      rows[3][i] = static_cast<uint8_t>(0x55U);
    }
    // Mask unused padding bits in last byte so reference bytes match padded
    // scanline bits cleanly.
    const uint32_t rem = width % 8U;
    if (rem > 0) {
      const uint8_t mask = static_cast<uint8_t>(0xffU << (8U - rem));
      for (auto& row : rows) {
        row.back() &= mask;
      }
    }

    auto verify_masked = [&](uint16_t comp, uint32_t g3_opts, uint32_t rps) {
      MemoryTiffStream stream;
      {
        UniqueTiff writer = stream.Open("wl");
        ASSERT_NE(writer.get(), nullptr);
        EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, width), 1);
        EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}),
                  1);
        EXPECT_EQ(
            TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}), 1);
        EXPECT_EQ(
            TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
            1);
        EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, comp), 1);
        EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                               PHOTOMETRIC_MINISWHITE),
                  1);
        EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                               PLANARCONFIG_CONTIG),
                  1);
        EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, rps), 1);
        if (comp == COMPRESSION_CCITTFAX3 && g3_opts != 0) {
          EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_GROUP3OPTIONS, g3_opts),
                    1);
        }
        for (uint32_t r = 0; r < 4; ++r) {
          std::vector<uint8_t> mutable_row = rows[r];
          ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row.data(), r, 0),
                    1);
        }
      }
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader.get(), nullptr);
      std::vector<uint8_t> got(row_bytes, 0);
      for (uint32_t r = 0; r < 4; ++r) {
        ASSERT_EQ(TIFFReadScanline(reader.get(), got.data(), r, 0), 1);
        VerifyMaskTailEqual(got, rows[r], width);
      }
    };

    verify_masked(COMPRESSION_CCITTFAX3, 0, 4);
    verify_masked(COMPRESSION_CCITTFAX3,
                  GROUP3OPT_2DENCODING | GROUP3OPT_FILLBITS, 4);
    verify_masked(COMPRESSION_CCITTFAX4, 0, 4);
    verify_masked(COMPRESSION_CCITTRLE, 0, 4);
    verify_masked(COMPRESSION_CCITTRLEW, 0, 1);
  }

  // 2. Specific 1-row CCITTRLEW case hitting odd cp advancement when BitsAvail
  // == 0 at row end (White 0 [8b] + Black 1 [3b] + White 1 [6b] = 17 bits ->
  // 3 bytes + 1 word-align pad byte), plus unaligned-right black span clamp
  // in find1span (width=2, {0x60} where unused bit 2 is also set to 1).
  VerifyScanlineRoundTrip(COMPRESSION_CCITTRLEW, 2, 1, {{0x80}},
                          FILLORDER_MSB2LSB, 0, -1, RESUNIT_INCH, 0.0f, "wl",
                          1);
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{2}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
                1);
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             COMPRESSION_CCITTFAX3),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             PHOTOMETRIC_MINISWHITE),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{1}),
                1);
      uint8_t row =
          0x60;  // pixel 0=0, pixel 1=1, pad bit 2=1 -> black span>bits
      ASSERT_EQ(TIFFWriteScanline(writer.get(), &row, 0, 0), 1);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    uint8_t got = 0xff;
    ASSERT_EQ(TIFFReadScanline(reader.get(), &got, 0, 0), 1);
    EXPECT_EQ(got & 0xc0U, 0x40U);
  }

  // 3. Very wide rows (5400 px = 675 bytes) forcing >= 2624 make-up loop
  // multiple iterations for both white and black, find0span/find1span int64_t
  // loops, black-ending vs white-ending rows, and LSB2MSB + big-endian +
  // BigTIFF.
  {
    constexpr uint32_t kWidth = 5400;
    constexpr uint32_t kRowBytes = kWidth / 8U;
    std::vector<std::vector<uint8_t>> rows(
        6, std::vector<uint8_t>(kRowBytes, 0x00));
    std::fill(rows[1].begin(), rows[1].end(), 0xff);
    std::fill(rows[2].begin() + 1, rows[2].end(), 0xff);
    rows[2][0] = 0x7f;  // Single white pixel then 5399 black pixels.
    std::fill(rows[3].begin(), rows[3].end(), 0xaa);
    std::fill(rows[4].begin(), rows[4].end(), 0x55);
    rows[5][0] = 0xff;
    rows[5][kRowBytes - 1] = 0xff;

    VerifyScanlineRoundTrip(COMPRESSION_CCITTFAX3, kWidth, 6, rows,
                            FILLORDER_MSB2LSB, 0, FAXMODE_CLASSIC, RESUNIT_INCH,
                            0.0f, "wl");
    VerifyScanlineRoundTrip(COMPRESSION_CCITTFAX3, kWidth, 6, rows,
                            FILLORDER_LSB2MSB, GROUP3OPT_FILLBITS,
                            FAXMODE_CLASSIC, RESUNIT_INCH, 0.0f, "wb");
    VerifyScanlineRoundTrip(COMPRESSION_CCITTFAX4, kWidth, 6, rows,
                            FILLORDER_LSB2MSB, 0, -1, RESUNIT_INCH, 0.0f, "w8");
  }

  // 4. Exercise find0span/find1span early exit inside unaligned prefix loop
  // when remaining span >= 128 bits and caller row buffer starts at odd byte.
  {
    constexpr uint32_t kWidth = 256;
    constexpr uint32_t kRowBytes = kWidth / 8U;
    std::vector<uint8_t> unaligned_storage(kRowBytes + 2, 0x00);
    uint8_t* unaligned = unaligned_storage.data();
    if ((reinterpret_cast<uintptr_t>(unaligned) & 7U) == 0U) {
      ++unaligned;
    }

    MemoryTiffStream stream;
    std::vector<uint8_t> expected_white_prefix(kRowBytes, 0x00);
    expected_white_prefix[2] = 0x0f;
    std::vector<uint8_t> expected_black_prefix(kRowBytes, 0xff);
    expected_black_prefix[2] = 0xf0;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kWidth), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{2}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
                1);
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             COMPRESSION_CCITTFAX3),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             PHOTOMETRIC_MINISWHITE),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{2}),
                1);

      std::memcpy(unaligned, expected_white_prefix.data(), kRowBytes);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), unaligned, 0, 0), 1);
      std::memcpy(unaligned, expected_black_prefix.data(), kRowBytes);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), unaligned, 1, 0), 1);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> got(kRowBytes, 0);
    ASSERT_EQ(TIFFReadScanline(reader.get(), got.data(), 0, 0), 1);
    EXPECT_EQ(got, expected_white_prefix);
    ASSERT_EQ(TIFFReadScanline(reader.get(), got.data(), 1, 0), 1);
    EXPECT_EQ(got, expected_black_prefix);
  }

  // 5. G3 2D encoding covering both maxk=2 (<=150 lpi) and maxk=4 (>150 lpi)
  // across RESUNIT_INCH and RESUNIT_CENTIMETER, k == 0 reset to G3_1D, and
  // all Fax3Encode2DRow modes (a0+a1==0, black->white horiz, white->black
  // horiz, pass, vertical offsets -3..+3).
  {
    constexpr uint32_t kWidth = 128;
    constexpr uint32_t kRowBytes = kWidth / 8U;
    std::vector<std::vector<uint8_t>> rows(
        10, std::vector<uint8_t>(kRowBytes, 0x00));
    rows[0][0] = 0x00;  // All white -> row 1 tests black start (a0+a1 == 0)
    rows[1][0] = 0xff;  // 8 black at col 0 vs 128 white ref -> horiz a0+a1==0
    rows[2][0] = 0xfe;  // VL(1)
    rows[3][0] = 0xf8;  // VL(2)
    rows[4][0] = 0xe0;  // VL(3)
    rows[4][3] = 0xff;
    rows[4][4] = 0xff;
    rows[5][0] = 0xf0;  // VR(1) and pass over ref [24..39]
    rows[6][0] = 0xfc;  // VR(2)
    rows[6][8] = 0x0f;  // white->black ref for row 7 black->white horiz
    rows[7][0] = 0xff;  // VR(3)
    rows[7][7] = 0xff;  // starts black at bit 56 while ref was white until 60
    rows[7][8] = 0x00;
    rows[8][2] = 0x3c;
    rows[9] = rows[8];

    VerifyScanlineRoundTrip(COMPRESSION_CCITTFAX3, kWidth, 10, rows,
                            FILLORDER_MSB2LSB, GROUP3OPT_2DENCODING,
                            FAXMODE_CLASSIC, RESUNIT_INCH, 100.0f, "wl");
    VerifyScanlineRoundTrip(COMPRESSION_CCITTFAX3, kWidth, 10, rows,
                            FILLORDER_MSB2LSB,
                            GROUP3OPT_2DENCODING | GROUP3OPT_FILLBITS,
                            FAXMODE_CLASSIC, RESUNIT_INCH, 200.0f, "wl");
    VerifyScanlineRoundTrip(COMPRESSION_CCITTFAX3, kWidth, 10, rows,
                            FILLORDER_LSB2MSB, GROUP3OPT_2DENCODING,
                            FAXMODE_NORTC, RESUNIT_CENTIMETER, 50.0f, "wl");
    VerifyScanlineRoundTrip(COMPRESSION_CCITTFAX3, kWidth, 10, rows,
                            FILLORDER_MSB2LSB, GROUP3OPT_2DENCODING,
                            FAXMODE_CLASSIC, RESUNIT_CENTIMETER, 60.0f, "wl");
  }

  // 6. FAXMODE_FILLBITS bit alignment variations (align < bit, align == bit,
  // align > bit) in Fax3PutEOL plus FAXMODE_CLASSIC close with tif_rawcp ==
  // NULL and FAXMODE_NOEOL round-trip.
  {
    std::vector<std::vector<uint8_t>> rows(9, std::vector<uint8_t>(4, 0));
    for (uint32_t r = 0; r < 9; ++r) {
      rows[r][0] = static_cast<uint8_t>((1U << (r % 8U)) - 1U);
      rows[r][1] = static_cast<uint8_t>(r * 31U);
      rows[r][2] = static_cast<uint8_t>(r * 17U ^ 0x5aU);
      rows[r][3] = 0x80U;
    }
    VerifyScanlineRoundTrip(COMPRESSION_CCITTFAX3, 32, 9, rows,
                            FILLORDER_MSB2LSB, GROUP3OPT_FILLBITS,
                            FAXMODE_CLASSIC);
    VerifyScanlineRoundTrip(COMPRESSION_CCITTFAX3, 32, 9, rows,
                            FILLORDER_MSB2LSB, 0, FAXMODE_NOEOL);
  }
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX3), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_FAXMODE, FAXMODE_CLASSIC), 1);
    // Close without writing any scanlines so tif_rawcp is NULL in _Fax3Close.
  }

  // 7. Tiled G3-1D, G3-2D, and G4 encode/decode (via TIFFWriteEncodedTile,
  // TIFFReadEncodedTile, and TIFFReadTile).
  for (const auto& [comp, g3_opts] :
       std::array<std::pair<uint16_t, uint32_t>, 3>{
           {{COMPRESSION_CCITTFAX3, 0U},
            {COMPRESSION_CCITTFAX3, GROUP3OPT_2DENCODING | GROUP3OPT_FILLBITS},
            {COMPRESSION_CCITTFAX4, 0U}}}) {
    SCOPED_TRACE(testing::Message()
                 << "tiled comp=" << comp << " g3_opts=" << g3_opts);
    constexpr uint32_t kImgW = 64;
    constexpr uint32_t kImgH = 16;
    constexpr uint32_t kTileW = 32;
    constexpr uint32_t kTileH = 16;
    std::vector<uint8_t> tile((kTileW / 8U) * kTileH, 0);
    for (size_t i = 0; i < tile.size(); ++i) {
      tile[i] = static_cast<uint8_t>((i * 41U + 3U) & 0xffU);
    }

    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kImgW), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kImgH), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, kTileW), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, kTileH), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
                1);
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, comp), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             PHOTOMETRIC_MINISWHITE),
                1);
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      if (comp == COMPRESSION_CCITTFAX3 && g3_opts != 0) {
        EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_GROUP3OPTIONS, g3_opts),
                  1);
      }
      ASSERT_EQ(TIFFNumberOfTiles(writer.get()), 2U);
      for (uint32_t t = 0; t < 2; ++t) {
        std::vector<uint8_t> mutable_tile = tile;
        ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), t, mutable_tile.data(),
                                       static_cast<tmsize_t>(tile.size())),
                  static_cast<tmsize_t>(tile.size()));
      }
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> got(tile.size(), 0);
    ASSERT_EQ(TIFFReadTile(reader.get(), got.data(), 0, 0, 0, 0),
              static_cast<tmsize_t>(tile.size()));
    EXPECT_EQ(got, tile);
    std::fill(got.begin(), got.end(), 0);
    ASSERT_EQ(TIFFReadEncodedTile(reader.get(), 1, got.data(),
                                  static_cast<tmsize_t>(tile.size())),
              static_cast<tmsize_t>(tile.size()));
    EXPECT_EQ(got, tile);
  }
}

TEST(TifFax3Test, EncodeFlushBufferAndWriteErrorsPropagateCleanly) {
  // 1. Fractional scanline writes fail in Fax3Encode and Fax4Encode.
  for (const auto& [comp, g3_opts] :
       std::array<std::pair<uint16_t, uint32_t>, 3>{
           {{COMPRESSION_CCITTFAX3, 0U},
            {COMPRESSION_CCITTFAX3, GROUP3OPT_2DENCODING},
            {COMPRESSION_CCITTFAX4, 0U}}}) {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{32}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, comp), 1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
        1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{1}), 1);
    if (comp == COMPRESSION_CCITTFAX3 && g3_opts != 0) {
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_GROUP3OPTIONS, g3_opts), 1);
    }
    std::array<uint8_t, 3> fractional{};
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, fractional.data(), 3),
              static_cast<tmsize_t>(-1));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 2. Tiny raw buffer forces TIFFFlushData1 mid-row while still succeeding
  // cleanly on round-trip.
  {
    constexpr uint32_t kWidth = 256;
    constexpr uint32_t kHeight = 8;
    std::vector<std::vector<uint8_t>> rows(
        kHeight, std::vector<uint8_t>(kWidth / 8U, 0xaa));
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kWidth), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kHeight), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
                1);
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             COMPRESSION_CCITTFAX3),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             PHOTOMETRIC_MINISWHITE),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kHeight), 1);
      ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 4), 1);
      for (uint32_t r = 0; r < kHeight; ++r) {
        std::vector<uint8_t> mutable_row = rows[r];
        ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row.data(), r, 0), 1);
      }
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> got(kWidth / 8U, 0);
    for (uint32_t r = 0; r < kHeight; ++r) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), got.data(), r, 0), 1);
      EXPECT_EQ(got, rows[r]);
    }
  }

  // 3. Inject WriteProc errors across small raw output buffer sizes to hit
  // !TIFFFlushData1 failure returns across putspan, Fax3PutEOL,
  // Fax3Encode1DRow, Fax3Encode2DRow (horiz, pass, vcodes), Fax3Encode,
  // Fax4Encode, Fax3PostEncode, and Fax4PostEncode.
  struct WriteFailConfig {
    uint16_t compression;
    uint32_t g3_options;
    int fax_mode;
    tmsize_t raw_buf_size;
    size_t ok_writes_after_open;
    uint32_t width;
    uint32_t height;
    uint8_t fill_pattern;
  };
  constexpr std::array<WriteFailConfig, 9> kWriteFailCases = {{
      // Flush fail in Fax3PutEOL / Fax3Encode1DRow / putspan.
      {COMPRESSION_CCITTFAX3, 0, FAXMODE_CLASSIC, 1, 0, 64, 4, 0xaa},
      {COMPRESSION_CCITTFAX3, GROUP3OPT_FILLBITS, FAXMODE_CLASSIC, 2, 0, 64, 4,
       0xaa},
      {COMPRESSION_CCITTFAX3, GROUP3OPT_FILLBITS, FAXMODE_CLASSIC, 4, 1, 64, 4,
       0xaa},
      // Flush fail in Fax3Encode2DRow (horiz, pass, vcode) and G3 2D.
      {COMPRESSION_CCITTFAX3, GROUP3OPT_2DENCODING, FAXMODE_CLASSIC, 1, 0, 128,
       6, 0x55},
      {COMPRESSION_CCITTFAX3, GROUP3OPT_2DENCODING, FAXMODE_CLASSIC, 2, 1, 128,
       6, 0x3c},
      // Flush fail in Fax4Encode / Fax4PostEncode.
      {COMPRESSION_CCITTFAX4, 0, -1, 1, 0, 128, 6, 0xaa},
      {COMPRESSION_CCITTFAX4, 0, -1, 3, 1, 128, 6, 0x0f},
      // Flush fail in CCITTRLE bytealign and CCITTRLEW wordalign Fax3FlushBits.
      {COMPRESSION_CCITTRLE, 0, -1, 1, 0, 64, 4, 0xaa},
      {COMPRESSION_CCITTRLEW, 0, -1, 2, 0, 16, 4, 0xaa},
  }};

  for (const auto& cfg : kWriteFailCases) {
    SCOPED_TRACE(testing::Message()
                 << "comp=" << cfg.compression
                 << " raw_buf=" << cfg.raw_buf_size
                 << " ok_writes=" << cfg.ok_writes_after_open);
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, cfg.width), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, cfg.height), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, cfg.compression),
              1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
        1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, cfg.height), 1);
    if (cfg.compression == COMPRESSION_CCITTFAX3 && cfg.g3_options != 0) {
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_GROUP3OPTIONS, cfg.g3_options), 1);
    }
    if (cfg.fax_mode >= 0) {
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_FAXMODE, cfg.fax_mode), 1);
    }
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, cfg.raw_buf_size), 1);

    const size_t base_writes = stream.write_calls();
    stream.FailWritesAfter(base_writes + cfg.ok_writes_after_open);

    std::vector<uint8_t> strip((cfg.width / 8U) * cfg.height, cfg.fill_pattern);
    // Vary alternating rows slightly so 2D encoding triggers Horiz, Pass, and
    // V.
    for (uint32_t r = 1; r < cfg.height; r += 2) {
      strip[r * (cfg.width / 8U)] ^= 0xffU;
    }
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                    static_cast<tmsize_t>(strip.size())),
              static_cast<tmsize_t>(-1));
  }

  // Explicitly target Fax3PostEncode flush failure after writing 1 partial
  // byte with Fax3Close RTC failure on close as well.
  for (size_t ok_writes : {size_t{0}, size_t{1}, size_t{2}, size_t{3}}) {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{32}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX3),
        1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
        1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{1}), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_FAXMODE, FAXMODE_CLASSIC), 1);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 1), 1);
    stream.FailWritesAfter(stream.write_calls() + ok_writes);
    std::array<uint8_t, 4> strip = {0xaa, 0xaa, 0xaa, 0xaa};
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(), 4),
              static_cast<tmsize_t>(-1));
  }

  // Sweep small write-failure budgets (raw_buf=1) across crafted 2D strips in
  // CCITTFAX4 and CCITTFAX3+2D so flush failures occur inside Fax3Encode2DRow
  // (Pass, Horiz W+B, Horiz B+W, V) and Fax3Encode (Fax3PutEOL + 2D row).
  for (uint16_t comp : {COMPRESSION_CCITTFAX4, COMPRESSION_CCITTFAX3}) {
    for (size_t ok_writes = 0; ok_writes < 16; ++ok_writes) {
      MemoryTiffStream stream;
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{64}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{3}),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
                1);
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, comp), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             PHOTOMETRIC_MINISWHITE),
                1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{3}),
                1);
      if (comp == COMPRESSION_CCITTFAX3) {
        EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_GROUP3OPTIONS,
                               GROUP3OPT_2DENCODING),
                  1);
      }
      ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 1), 1);
      stream.FailWritesAfter(stream.write_calls() + ok_writes);
      // Row 0: [W=8, B=8, W=8, B=8, W=8, B=8, W=8, B=8]
      // Row 1: V0 at 8 then Horiz B=16 + W=16 + B=8 + W=16
      // Row 2: Pass over B=16 then Horiz W=24 + B=8 + W=8
      std::array<uint8_t, 24> strip = {
          0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff,  // Row 0
          0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00,  // Row 1
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,  // Row 2
      };
      const tmsize_t rc = TIFFWriteEncodedStrip(
          writer.get(), 0, strip.data(), static_cast<tmsize_t>(strip.size()));
      if (rc == static_cast<tmsize_t>(-1)) {
        EXPECT_GE(stream.diagnostics().error_count(), 1U);
      } else {
        EXPECT_EQ(rc, static_cast<tmsize_t>(strip.size()));
      }
    }
  }
}

TEST(TifFax3Test, DirectFillRunsCoversEdgeCasesAndUnalignedBuffers) {
  // 1. Odd run count appends trailing 0 black run; runs > lastx get clamped.
  {
    alignas(8) std::array<uint8_t, 64> buf{};
    buf.fill(0xaa);
    // Space for 4 uint32_t runs so odd count (erun - runs == 3) safely writes
    // erun[0] = 0.
    std::array<uint32_t, 4> runs = {200, 0, 0, 999};
    _TIFFFax3fillruns(buf.data(), runs.data(), runs.data() + 1, 64);
    EXPECT_EQ(runs[0], 64U);
    EXPECT_EQ(runs[1], 0U);
    for (int i = 0; i < 8; ++i) {
      EXPECT_EQ(buf[i], 0x00U);
    }
  }

  // 2. Long (> 128-bit) white and black runs starting at an unaligned byte
  // pointer to hit alignment loops, int64_t bulk fill loops, tail byte masks,
  // and final black-run clamping (x + run > lastx).
  {
    alignas(8) std::array<uint8_t, 80> backing{};
    backing.fill(0x55);
    uint8_t* unaligned = backing.data() + 1;

    // 3 bits white (byte 0 partial), 140 bits black (>128 -> int64 loop),
    // 140 bits white (>128 -> int64 loop), 2 bits black, 4 bits white,
    // 999 bits black (clamped to remainder of 301 total pixels).
    constexpr uint32_t kLastX = 301;
    std::array<uint32_t, 8> runs = {3, 140, 140, 2, 4, 999, 0, 0};
    _TIFFFax3fillruns(unaligned, runs.data(), runs.data() + 6, kLastX);
    EXPECT_EQ(runs[5], 12U);

    auto get_bit = [&](uint32_t idx) -> int {
      return (unaligned[idx >> 3] >> (7U - (idx & 7U))) & 1;
    };
    uint32_t pos = 0;
    for (uint32_t i = 0; i < 3; ++i, ++pos) EXPECT_EQ(get_bit(pos), 0);
    for (uint32_t i = 0; i < 140; ++i, ++pos) EXPECT_EQ(get_bit(pos), 1);
    for (uint32_t i = 0; i < 140; ++i, ++pos) EXPECT_EQ(get_bit(pos), 0);
    for (uint32_t i = 0; i < 2; ++i, ++pos) EXPECT_EQ(get_bit(pos), 1);
    for (uint32_t i = 0; i < 4; ++i, ++pos) EXPECT_EQ(get_bit(pos), 0);
    for (uint32_t i = 0; i < 12; ++i, ++pos) EXPECT_EQ(get_bit(pos), 1);
    EXPECT_EQ(pos, kLastX);
  }
}

TEST(TifFax3Test, DecodeErrorPathsAndTruncatedStreams) {
  // 1. Fractional scanline reads rejected across G3-1D, G3-2D, G4, and RLE.
  for (const auto& [comp, g3_opts] :
       std::array<std::pair<uint16_t, uint32_t>, 4>{
           {{COMPRESSION_CCITTFAX3, 0U},
            {COMPRESSION_CCITTFAX3, GROUP3OPT_2DENCODING},
            {COMPRESSION_CCITTFAX4, 0U},
            {COMPRESSION_CCITTRLE, 0U}}}) {
    MemoryTiffStream stream(BuildSingleStripFaxTiff(
        comp, 32, 1, {0x00, 0x10, 0x00}, g3_opts, g3_opts != 0));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 2> small_buf{};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, small_buf.data(), 2),
              static_cast<tmsize_t>(-1));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 2. Fax3Unexpected in both strip and tile, hitting White/Black/Main/VL/EOL.
  // Strip: G3-1D EOL (000000000001) + invalid White code (000000001).
  {
    MemoryTiffStream stream(BuildSingleStripFaxTiff(
        COMPRESSION_CCITTFAX3, 64, 1, {0x00, 0x10, 0x08, 0x00}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 8> row{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
    ASSERT_GE(stream.diagnostics().error_count(), 1U);
    EXPECT_THAT(stream.diagnostics().errors().front().message,
                HasSubstr("Bad code word at line 0 of strip 0"));
  }
  // Tile: G4 width=64, height=16: Row 0 = Horiz(White 0 [00110101], Black 63
  // [000001100111]) + V0 [1] (24 bits), Row 1 = VL(1) [010] with b1=0 < a0+1
  // => unexpected("VL", 0) on line 1 of tile 0, then EOF on row 2 returns 1
  // since sp->line != start.
  {
    MemoryTiffStream stream(BuildSingleTileFaxTiff(
        COMPRESSION_CCITTFAX4, 64, 16, {0x26, 0xa0, 0xcf, 0x40}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 128> tile{};
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, tile.data(), 128), 128);
    ASSERT_GE(stream.diagnostics().error_count(), 1U);
    EXPECT_THAT(stream.diagnostics().errors().front().message,
                HasSubstr("Bad code word at line 1 of tile 0"));
  }
  // Strip: G4 S_EOL followed by non-zero 4 bits -> unexpected("EOL", a0) and
  // EOFG4 error return (-1) on row 0 via TIFFReadEncodedStrip.
  {
    MemoryTiffStream stream(
        BuildSingleStripFaxTiff(COMPRESSION_CCITTFAX4, 16, 1, {0x01, 0xe0}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 2> strip{};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip.data(), 2),
              static_cast<tmsize_t>(-1));
    ASSERT_GE(stream.diagnostics().error_count(), 1U);
    EXPECT_THAT(stream.diagnostics().errors().front().message,
                HasSubstr("Bad code word at line 0 of strip 0"));
  }

  // 3. Fax3Extension (S_Ext prefix 0000001) on both strip and tile.
  {
    MemoryTiffStream stream(
        BuildSingleStripFaxTiff(COMPRESSION_CCITTFAX4, 16, 1, {0x02, 0x00}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 2> row{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
    ASSERT_GE(stream.diagnostics().error_count(), 1U);
    EXPECT_THAT(stream.diagnostics().errors().front().message,
                HasSubstr("Uncompressed data (not supported)"));
  }
  {
    MemoryTiffStream stream(
        BuildSingleTileFaxTiff(COMPRESSION_CCITTFAX4, 16, 16, {0x02, 0x00}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 32> tile{};
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, tile.data(), 32), 32);
    ASSERT_GE(stream.diagnostics().error_count(), 1U);
    EXPECT_THAT(stream.diagnostics().errors().front().message,
                HasSubstr("of tile 0"));
  }

  // 4. Fax3BadLength: both a0 < lastx ("Premature EOL") and a0 > lastx
  // ("Line length mismatch"), on strip and tile.
  {
    // Strip G3-1D width=64: EOL [000000000001] + White 128 MakeUp [10010] +
    // White 0 [00110101] + EOL [000000000001] -> 128 > 64.
    // Bits: 0000_0000 0001_1001 0_0011010 1_0000000 00001_000 ->
    // 0x00, 0x19, 0x1a, 0x80, 0x08
    MemoryTiffStream stream(BuildSingleStripFaxTiff(
        COMPRESSION_CCITTFAX3, 64, 1, {0x00, 0x19, 0x1a, 0x80, 0x08}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 8> row{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
    ASSERT_GE(stream.diagnostics().warning_count(), 1U);
    EXPECT_THAT(stream.diagnostics().warnings().front().message,
                HasSubstr("Line length mismatch at line 0 of strip 0"));
  }
  {
    // Tile G3-1D width=64, height=16: Row 0 = EOL [000000000001] + White 2
    // [0111], followed by 15 valid EOL + White 64 rows -> "Premature EOL at
    // line 0 of tile 0" without triggering nonstandard tile length warnings.
    std::vector<uint8_t> tile_bytes;
    uint32_t acc = 0;
    int bits = 0;
    auto push_bits = [&](uint32_t v, int n) {
      for (int i = n - 1; i >= 0; --i) {
        acc = (acc << 1U) | ((v >> i) & 1U);
        if (++bits == 8) {
          tile_bytes.push_back(static_cast<uint8_t>(acc));
          acc = 0;
          bits = 0;
        }
      }
    };
    push_bits(0x001U, 12);  // Row 0 EOL
    push_bits(0x7U, 4);     // Row 0 White 2 (< 64)
    for (int r = 1; r < 16; ++r) {
      push_bits(0x001U, 12);  // EOL
      push_bits(0x1bU, 5);    // MakeUpW 64
      push_bits(0x35U, 8);    // TermW 0
    }
    push_bits(0x001U, 12);  // Trailing EOL
    if (bits > 0) {
      tile_bytes.push_back(static_cast<uint8_t>(acc << (8 - bits)));
    }
    MemoryTiffStream stream(
        BuildSingleTileFaxTiff(COMPRESSION_CCITTFAX3, 64, 16, tile_bytes));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 128> tile{};
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, tile.data(), 128), 128);
    ASSERT_GE(stream.diagnostics().warning_count(), 1U);
    EXPECT_THAT(stream.diagnostics().warnings().front().message,
                HasSubstr("Premature EOL at line 0 of tile 0"));
  }

  // 5. Premature EOF paths across Fax3Decode1D, Fax3Decode2D, Fax4Decode,
  // and Fax3DecodeRLE.
  {
    // G3-1D empty strip -> EOF1D.
    MemoryTiffStream stream(
        BuildSingleStripFaxTiff(COMPRESSION_CCITTFAX3, 64, 1, {}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 8> row{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
  }
  {
    // G3-1D truncated mid-run -> EOF1Da.
    MemoryTiffStream stream(BuildSingleStripFaxTiff(COMPRESSION_CCITTFAX3, 4096,
                                                    1, {0x00, 0x10, 0x1f}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 512> row{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
    EXPECT_GE(stream.diagnostics().warning_count(), 1U);
  }
  {
    // G3-2D EOF right after EOL before 1D/2D tag bit ({0x00, 0x01}) -> EOF2D.
    MemoryTiffStream stream(
        BuildSingleStripFaxTiff(COMPRESSION_CCITTFAX3, 64, 1, {0x00, 0x01},
                                GROUP3OPT_2DENCODING, true));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 8> row{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
  }
  {
    // G3-2D EOF after is1D=1 ({0x00, 0x03}) -> EXPAND1D(EOF2Da).
    MemoryTiffStream stream(
        BuildSingleStripFaxTiff(COMPRESSION_CCITTFAX3, 64, 1, {0x00, 0x03},
                                GROUP3OPT_2DENCODING, true));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 8> row{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
  }
  {
    // G3-2D EOF after is1D=0 ({0x00, 0x02}) -> EXPAND2D(EOF2Da).
    MemoryTiffStream stream(
        BuildSingleStripFaxTiff(COMPRESSION_CCITTFAX3, 64, 1, {0x00, 0x02},
                                GROUP3OPT_2DENCODING, true));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 8> row{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
  }
  {
    // G4 2-row strip: row 0 decodes cleanly (V0 '1'), row 1 hits premature EOF
    // -> Fax4Decode returns 1 because sp->line != start!
    MemoryTiffStream stream(
        BuildSingleStripFaxTiff(COMPRESSION_CCITTFAX4, 16, 2, {0x80}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 4> strip{};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip.data(), 4), 4);
    EXPECT_GE(stream.diagnostics().warning_count(), 1U);
  }
  {
    // CCITTRLE truncated mid-run -> EOFRLE.
    MemoryTiffStream stream(BuildSingleStripFaxTiff(COMPRESSION_CCITTRLE, 8192,
                                                    1, {0x01, 0xf0, 0x1f}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 1024> row{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
    EXPECT_GE(stream.diagnostics().warning_count(), 1U);
  }

  // 6. tryG3WithoutEOL retry paths for 1D, 2D, and tile.
  {
    // G3-1D width=16 without EOL: White 16 [101010] + zero padding ->
    // 0xa8, 0x00, 0x00 triggers RETRY_WITHOUT_EOL_1D and decodes all white.
    MemoryTiffStream stream(BuildSingleStripFaxTiff(COMPRESSION_CCITTFAX3, 16,
                                                    1, {0xa8, 0x00, 0x00}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 2> row = {0xff, 0xff};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
    EXPECT_EQ(row, (std::array<uint8_t, 2>{0x00, 0x00}));
    ASSERT_GE(stream.diagnostics().warning_count(), 1U);
    EXPECT_THAT(stream.diagnostics().warnings().front().message,
                HasSubstr("Try to decode (read) fax Group 3 data without EOL"));
  }
  {
    // G3-2D width=16 without EOL: 1 (is1D) + White 16 [101010] + zero pad ->
    // 0xd4, 0x00, 0x00 triggers RETRY_WITHOUT_EOL_2D and decodes all white.
    MemoryTiffStream stream(BuildSingleStripFaxTiff(
        COMPRESSION_CCITTFAX3, 16, 1, {0xd4, 0x00, 0x00}, GROUP3OPT_2DENCODING,
        true));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 2> row = {0xff, 0xff};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
    EXPECT_EQ(row, (std::array<uint8_t, 2>{0x00, 0x00}));
    ASSERT_GE(stream.diagnostics().warning_count(), 1U);
    EXPECT_THAT(stream.diagnostics().warnings().front().message,
                HasSubstr("without EOL"));
  }
  {
    // Tiled G3-1D without EOL exercises tile diagnostic string in
    // Fax3TryG3WithoutEOL: 16 rows of White 16 [101010] (12 bytes of 0xaa, no
    // 11 consecutive zeros) followed by 2 trailing zero bytes triggers retry
    // without EOL and decodes all 16 white rows cleanly.
    std::vector<uint8_t> tile_bytes(12, 0xaa);
    tile_bytes.push_back(0x00);
    tile_bytes.push_back(0x00);
    MemoryTiffStream stream(
        BuildSingleTileFaxTiff(COMPRESSION_CCITTFAX3, 16, 16, tile_bytes));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 32> tile{};
    tile.fill(0xff);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, tile.data(), 32), 32);
    EXPECT_EQ(tile, (std::array<uint8_t, 32>{}));
    ASSERT_GE(stream.diagnostics().warning_count(), 1U);
    EXPECT_THAT(stream.diagnostics().warnings().front().message,
                HasSubstr("of tile 0"));
  }
}

TEST(TifFax3Test, ReachedCountThresholdsAbortRunawaySingleStripDecode) {
  constexpr uint32_t kManyRows = 8200;

  // 1. eofReachedCount >= 8192 in CCITTRLE.
  {
    MemoryTiffStream stream(BuildSingleStripFaxTiff(
        COMPRESSION_CCITTRLE, 8192, kManyRows, {0x01, 0xf0, 0x1f}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> row(8192 / 8U, 0);
    for (uint32_t r = 0; r < kManyRows; ++r) {
      EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), r, 0), -1);
      if (stream.diagnostics().error_count() > 0) {
        break;
      }
    }
    ASSERT_GE(stream.diagnostics().error_count(), 1U);
    EXPECT_THAT(
        stream.diagnostics().errors().front().message,
        HasSubstr("End of file (EOF) has already been reached 8192 times"));
  }

  // 2. eolReachedCount >= 8192 in CCITTFAX3.
  {
    MemoryTiffStream stream(
        BuildSingleStripFaxTiff(COMPRESSION_CCITTFAX3, 64, kManyRows, {0x00}));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 8> row{};
    bool aborted = false;
    for (uint32_t r = 0; r < kManyRows; ++r) {
      const int ret = TIFFReadScanline(reader.get(), row.data(), r, 0);
      if (ret == -1) {
        aborted = true;
        break;
      }
      EXPECT_EQ(ret, 1);
    }
    EXPECT_TRUE(aborted);
    ASSERT_GE(stream.diagnostics().error_count(), 1U);
    EXPECT_THAT(
        stream.diagnostics().errors().front().message,
        HasSubstr("Bad line length (EOL) has already been reached 8192 times"));
  }

  // 3. unexpectedReachedCount >= 8192 in CCITTFAX3 (EOL + White64 + S_Null per
  // row keeps a0 == lastx == 64 so eolReachedCount stays 0).
  {
    std::vector<uint8_t> strip;
    strip.reserve((kManyRows + 5U) * 4U);
    for (uint32_t i = 0; i < kManyRows + 5U; ++i) {
      strip.push_back(0x00);
      strip.push_back(0x1d);
      strip.push_back(0x80);
      strip.push_back(0x40);
    }
    MemoryTiffStream stream(BuildSingleStripFaxTiff(
        COMPRESSION_CCITTFAX3, 64, kManyRows, std::move(strip)));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 8> row{};
    bool aborted = false;
    for (uint32_t r = 0; r < kManyRows; ++r) {
      if (TIFFReadScanline(reader.get(), row.data(), r, 0) == -1) {
        aborted = true;
        break;
      }
    }
    EXPECT_TRUE(aborted);
    ASSERT_GE(stream.diagnostics().error_count(), 1U);
    bool found_threshold_error = false;
    for (const auto& err : stream.diagnostics().errors()) {
      if (err.message.find(
              "Bad code word (unexpected) has already been reached 8192") !=
          std::string::npos) {
        found_threshold_error = true;
        break;
      }
    }
    EXPECT_TRUE(found_threshold_error);
  }

  // 4. eolReachedCount >= 8192 in G3-2D (Fax3Decode2D) and eofReachedCount >=
  // 8192 in G4 (Fax4Decode).
  for (const auto& [comp, g3_opts, payload] :
       std::array<std::tuple<uint16_t, uint32_t, std::vector<uint8_t>>, 2>{
           {{COMPRESSION_CCITTFAX3, GROUP3OPT_2DENCODING, {0x00, 0x02}},
            {COMPRESSION_CCITTFAX4, 0U, {0x01}}}}) {
    SCOPED_TRACE(testing::Message() << "comp=" << comp);
    MemoryTiffStream stream(BuildSingleStripFaxTiff(
        comp, 64, kManyRows, payload, g3_opts, g3_opts != 0));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 8> row{};
    bool aborted = false;
    for (uint32_t r = 0; r < kManyRows; ++r) {
      if (TIFFReadScanline(reader.get(), row.data(), r, 0) == -1 &&
          stream.diagnostics().error_count() > 0 &&
          stream.diagnostics().errors().back().message.find(
              "has already been reached 8192") != std::string::npos) {
        aborted = true;
        break;
      }
    }
    EXPECT_TRUE(aborted);
  }
}

}  // namespace
}  // namespace libtiff
