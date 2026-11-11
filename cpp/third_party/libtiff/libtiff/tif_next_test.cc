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

// Unit tests for libtiff/tif_next.c (NeXT 2-bit greyscale RLE codec).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

constexpr uint8_t kNextLiteralRow = 0x00;
constexpr uint8_t kNextLiteralSpan = 0x40;

constexpr uint8_t MakeNextRle(uint8_t grey, uint8_t count) {
  return static_cast<uint8_t>(((grey & 0x03U) << 6) | (count & 0x3fU));
}

bool HasDiagnosticContaining(const std::vector<DiagnosticRecord>& records,
                             std::string_view module, std::string_view substr) {
  return std::any_of(
      records.begin(), records.end(), [&](const DiagnosticRecord& rec) {
        return rec.module == module &&
               rec.message.find(substr) != std::string_view::npos;
      });
}

bool HasAnyErrorContaining(const std::vector<DiagnosticRecord>& records,
                           std::string_view substr) {
  return std::any_of(
      records.begin(), records.end(), [&](const DiagnosticRecord& rec) {
        return rec.message.find(substr) != std::string_view::npos;
      });
}

std::vector<uint8_t> BuildStripedNextTiff(
    bool big_endian, bool big_tiff, uint32_t width, uint32_t height,
    uint32_t rows_per_strip, uint16_t bits_per_sample,
    uint16_t samples_per_pixel, uint16_t planar_config,
    const std::vector<std::vector<uint8_t>>& strips) {
  RawTiffBuilder builder(big_endian, big_tiff);
  std::vector<uint32_t> offsets32;
  std::vector<uint32_t> counts32;
  std::vector<uint64_t> offsets64;
  std::vector<uint64_t> counts64;

  for (const auto& strip : strips) {
    const uint64_t off = builder.AppendBlob(strip);
    offsets32.push_back(static_cast<uint32_t>(off));
    counts32.push_back(static_cast<uint32_t>(strip.size()));
    offsets64.push_back(off);
    counts64.push_back(strip.size());
  }

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bits_per_sample),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NEXT),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, planar_config),
  };

  if (big_tiff) {
    entries.push_back(builder.MakeLong8Array(TIFFTAG_STRIPOFFSETS, offsets64));
    entries.push_back(
        builder.MakeLong8Array(TIFFTAG_STRIPBYTECOUNTS, counts64));
  } else {
    entries.push_back(builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offsets32));
    entries.push_back(builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, counts32));
  }

  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint8_t> BuildTiledNextTiff(
    bool big_endian, bool big_tiff, uint32_t width, uint32_t height,
    uint32_t tile_width, uint32_t tile_length,
    const std::vector<std::vector<uint8_t>>& tiles) {
  RawTiffBuilder builder(big_endian, big_tiff);
  std::vector<uint32_t> offsets32;
  std::vector<uint32_t> counts32;
  std::vector<uint64_t> offsets64;
  std::vector<uint64_t> counts64;

  for (const auto& tile : tiles) {
    const uint64_t off = builder.AppendBlob(tile);
    offsets32.push_back(static_cast<uint32_t>(off));
    counts32.push_back(static_cast<uint32_t>(tile.size()));
    offsets64.push_back(off);
    counts64.push_back(tile.size());
  }

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 2),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NEXT),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
      builder.MakeLong(TIFFTAG_TILEWIDTH, tile_width),
      builder.MakeLong(TIFFTAG_TILELENGTH, tile_length),
  };

  if (big_tiff) {
    entries.push_back(builder.MakeLong8Array(TIFFTAG_TILEOFFSETS, offsets64));
    entries.push_back(builder.MakeLong8Array(TIFFTAG_TILEBYTECOUNTS, counts64));
  } else {
    entries.push_back(builder.MakeLongArray(TIFFTAG_TILEOFFSETS, offsets32));
    entries.push_back(builder.MakeLongArray(TIFFTAG_TILEBYTECOUNTS, counts32));
  }

  builder.AddIfd(std::move(entries));
  return builder.Take();
}

TEST(TifNextTest, CodecRegisteredAndConfigured) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_NEXT), 1);
  const TIFFCodec* codec = TIFFFindCODEC(COMPRESSION_NEXT);
  ASSERT_NE(codec, nullptr);
  EXPECT_EQ(codec->scheme, COMPRESSION_NEXT);
  EXPECT_STREQ(codec->name, "NeXT");
  EXPECT_NE(codec->init, nullptr);
}

TEST(TifNextTest, EncodeRejectedAsUnimplemented) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("wl");
  ASSERT_NE(tif, nullptr);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 8U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 2U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{2}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 2U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
            1);
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NEXT), 1);

  uint16_t compression = 0;
  ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_COMPRESSION, &compression), 1);
  EXPECT_EQ(compression, COMPRESSION_NEXT);

  std::array<uint8_t, 2> row = {0x12, 0x34};
  EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), -1);
  EXPECT_TRUE(
      HasAnyErrorContaining(stream.diagnostics().errors(),
                            "NeXT scanline encoding is not implemented"))
      << "Expected encode-not-implemented diagnostic";
}

TEST(TifNextTest, DecodeLiteralRowAcrossEndiannessAndBitness) {
  struct ContainerCase {
    std::string_view name;
    bool big_endian;
    bool big_tiff;
  };
  constexpr std::array<ContainerCase, 4> kCases = {{
      {.name = "ClassicLe", .big_endian = false, .big_tiff = false},
      {.name = "ClassicBe", .big_endian = true, .big_tiff = false},
      {.name = "BigTiffLe", .big_endian = false, .big_tiff = true},
      {.name = "BigTiffBe", .big_endian = true, .big_tiff = true},
  }};

  // 16 pixels wide -> 4 bytes/scanline, 3 rows in 1 strip.
  const std::vector<uint8_t> encoded = {
      kNextLiteralRow, 0x00, 0x55, 0xaa, 0xff,
      kNextLiteralRow, 0x1b, 0xe4, 0x39, 0xc6,
      kNextLiteralRow, 0xf0, 0x0f, 0x5a, 0xa5,
  };
  const std::vector<uint8_t> expected = {
      0x00, 0x55, 0xaa, 0xff, 0x1b, 0xe4, 0x39, 0xc6, 0xf0, 0x0f, 0x5a, 0xa5,
  };

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    MemoryTiffStream stream(BuildStripedNextTiff(
        tc.big_endian, tc.big_tiff, /*width=*/16, /*height=*/3,
        /*rows_per_strip=*/3, /*bits_per_sample=*/2, /*samples_per_pixel=*/1,
        PLANARCONFIG_CONTIG, {encoded}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFIsByteSwapped(tif.get()) != 0, tc.big_endian);
    EXPECT_EQ(TIFFIsBigTIFF(tif.get()) != 0, tc.big_tiff);
    EXPECT_EQ(TIFFScanlineSize(tif.get()), 4);
    EXPECT_EQ(TIFFStripSize(tif.get()), 12);

    std::vector<uint8_t> strip_out(12, 0x11);
    EXPECT_EQ(
        TIFFReadEncodedStrip(tif.get(), 0, strip_out.data(), strip_out.size()),
        12);
    EXPECT_EQ(strip_out, expected);
    EXPECT_TRUE(stream.diagnostics().errors().empty());
    tif.reset();

    // Also verify scanline-by-scanline decoding across the same strip on a
    // fresh handle so tif_rawcp/tif_rawcc advance across TIFFReadScanline.
    UniqueTiff scanline_tif = stream.Open("r");
    ASSERT_NE(scanline_tif, nullptr);
    std::array<uint8_t, 4> row_out{};
    for (uint32_t r = 0; r < 3; ++r) {
      row_out.fill(0x22);
      EXPECT_EQ(TIFFReadScanline(scanline_tif.get(), row_out.data(), r, 0), 1);
      for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(row_out[i], expected[r * 4 + i]);
      }
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifNextTest, DecodeLiteralSpanPreservesWhiteBackgroundAndLargeOffsets) {
  {
    // 16 pixels -> 4 bytes/scanline, 2 rows.
    // Row 0: LITERALSPAN at offset 1, length 2 -> [0xff, 0x1b, 0xe4, 0xff].
    // Row 1: zero-length LITERALSPAN at offset 2 -> all 0xff.
    const std::vector<uint8_t> encoded = {
        kNextLiteralSpan, 0x00, 0x01, 0x00, 0x02, 0x1b, 0xe4,
        kNextLiteralSpan, 0x00, 0x02, 0x00, 0x00,
    };
    MemoryTiffStream stream(BuildStripedNextTiff(
        /*big_endian=*/false, /*big_tiff=*/false, /*width=*/16, /*height=*/2,
        /*rows_per_strip=*/2, /*bits_per_sample=*/2, /*samples_per_pixel=*/1,
        PLANARCONFIG_CONTIG, {encoded}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    std::vector<uint8_t> out(8, 0x00);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), out.size()), 8);
    const std::vector<uint8_t> expected = {
        0xff, 0x1b, 0xe4, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    EXPECT_EQ(out, expected);
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  {
    // Wide scanline (1040 pixels -> 260 bytes/row) to verify 16-bit big-endian
    // offset (bp[0]*256 + bp[1]) and count (bp[2]*256 + bp[3]) decoding.
    std::vector<uint8_t> encoded = {
        // Row 0: offset = 257 (0x01, 0x01), length = 2 (0x00, 0x02).
        kNextLiteralSpan,
        0x01,
        0x01,
        0x00,
        0x02,
        0x42,
        0x84,
        // Row 1: offset = 1 (0x00, 0x01), length = 258 (0x01, 0x02).
        kNextLiteralSpan,
        0x00,
        0x01,
        0x01,
        0x02,
    };
    std::vector<uint8_t> span_payload(258);
    for (size_t i = 0; i < span_payload.size(); ++i) {
      span_payload[i] = static_cast<uint8_t>((i * 7U + 3U) & 0xffU);
    }
    encoded.insert(encoded.end(), span_payload.begin(), span_payload.end());

    MemoryTiffStream stream(BuildStripedNextTiff(
        /*big_endian=*/false, /*big_tiff=*/false, /*width=*/1040, /*height=*/2,
        /*rows_per_strip=*/2, /*bits_per_sample=*/2, /*samples_per_pixel=*/1,
        PLANARCONFIG_CONTIG, {encoded}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFScanlineSize(tif.get()), 260);

    std::vector<uint8_t> out(520, 0x00);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), out.size()), 520);

    std::vector<uint8_t> expected(520, 0xff);
    expected[257] = 0x42;
    expected[258] = 0x84;
    std::copy(span_payload.begin(), span_payload.end(),
              expected.begin() + 260 + 1);
    EXPECT_EQ(out, expected);
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifNextTest, DecodeRleAllPixelPositionsZeroCountAndWidthClamp) {
  // 12 pixels wide -> 3 bytes/scanline, 3 rows in 2 strips (RowsPerStrip=2).
  // Row 0: zero-count RLE prefixes (grey=2 count=0, grey=3 count=0), then runs
  // covering all 4 grey levels and all 4 sub-byte pixel positions:
  //   grey=0 x3 -> pixels 0..2: 00 00 00
  //   grey=1 x2 -> pixel 3: 01 -> byte 0 = 0b00'00'00'01 = 0x01
  //                pixel 4: 01
  //   grey=2 x1 -> pixel 5: 10
  //   grey=3 x2 -> pixels 6..7: 11 11 -> byte 1 = 0b01'10'11'11 = 0x6f
  //   grey=0 x1 -> pixel 8: 00
  //   grey=1 x3 -> pixels 9..11: 01 01 01 -> byte 2 = 0b00'01'01'01 = 0x15
  const std::vector<uint8_t> strip0 = {
      MakeNextRle(2, 0),
      MakeNextRle(3, 0),
      MakeNextRle(0, 3),
      MakeNextRle(1, 2),
      MakeNextRle(2, 1),
      MakeNextRle(3, 2),
      MakeNextRle(0, 1),
      MakeNextRle(1, 3),
      // Row 1: RLE run overshooting width (count=20 > 12), grey=2 -> 0xaa.
      MakeNextRle(2, 20),
  };
  // Strip 1 (row 2): LITERALROW mixing white and grey.
  const std::vector<uint8_t> strip1 = {
      kNextLiteralRow,
      0xde,
      0xad,
      0xbe,
  };

  MemoryTiffStream stream(BuildStripedNextTiff(
      /*big_endian=*/false, /*big_tiff=*/false, /*width=*/12, /*height=*/3,
      /*rows_per_strip=*/2, /*bits_per_sample=*/2, /*samples_per_pixel=*/1,
      PLANARCONFIG_CONTIG, {strip0, strip1}));
  UniqueTiff tif = stream.Open("r");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(TIFFNumberOfStrips(tif.get()), 2U);

  std::vector<uint8_t> out0(6, 0x00);
  EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out0.data(), out0.size()), 6);
  const std::vector<uint8_t> expected0 = {
      0x01, 0x6f, 0x15, 0xaa, 0xaa, 0xaa,
  };
  EXPECT_EQ(out0, expected0);

  std::vector<uint8_t> out1(3, 0x00);
  EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 1, out1.data(), out1.size()), 3);
  const std::vector<uint8_t> expected1 = {0xde, 0xad, 0xbe};
  EXPECT_EQ(out1, expected1);
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifNextTest, DecodePartialByteWidthsAndSeparatePlanes) {
  // Width = 5 (2 bytes/scanline): 4 black + 1 grey=2 -> {0x00, 0x80}.
  {
    const std::vector<uint8_t> strip = {
        MakeNextRle(0, 4),
        MakeNextRle(2, 1),
    };
    MemoryTiffStream stream(BuildStripedNextTiff(
        /*big_endian=*/false, /*big_tiff=*/false, /*width=*/5, /*height=*/1,
        /*rows_per_strip=*/1, /*bits_per_sample=*/2, /*samples_per_pixel=*/1,
        PLANARCONFIG_CONTIG, {strip}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    std::array<uint8_t, 2> out{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), out.data(), 0, 0), 1);
    EXPECT_EQ(out[0], 0x00);
    EXPECT_EQ(out[1], 0x80);
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // Separate planes: 3 samples/pixel, width=8 (2 bytes/scanline), height=1.
  {
    const std::vector<uint8_t> plane0 = {MakeNextRle(0, 8)};
    const std::vector<uint8_t> plane1 = {MakeNextRle(1, 8)};
    const std::vector<uint8_t> plane2 = {MakeNextRle(2, 8)};
    MemoryTiffStream stream(BuildStripedNextTiff(
        /*big_endian=*/false, /*big_tiff=*/false, /*width=*/8, /*height=*/1,
        /*rows_per_strip=*/1, /*bits_per_sample=*/2, /*samples_per_pixel=*/3,
        PLANARCONFIG_SEPARATE, {plane0, plane1, plane2}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFNumberOfStrips(tif.get()), 3U);

    const std::array<uint8_t, 3> expected_fill = {0x00, 0x55, 0xaa};
    for (uint16_t s = 0; s < 3; ++s) {
      std::array<uint8_t, 2> row{};
      EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, s), 1);
      EXPECT_EQ(row[0], expected_fill[s]);
      EXPECT_EQ(row[1], expected_fill[s]);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifNextTest, DecodeTiledAndMemoryMappedImages) {
  // 16x4 image with two 16x2 tiles -> 4 bytes/row, 8 bytes/tile.
  const std::vector<uint8_t> tile0 = {
      MakeNextRle(1, 8),
      MakeNextRle(2, 8),
      kNextLiteralRow,
      0x12,
      0x34,
      0x56,
      0x78,
  };
  const std::vector<uint8_t> tile1 = {
      kNextLiteralSpan, 0x00, 0x01, 0x00, 0x02, 0xab, 0xcd, MakeNextRle(3, 16),
  };

  MemoryTiffStream stream(BuildTiledNextTiff(
      /*big_endian=*/false, /*big_tiff=*/false, /*width=*/16, /*height=*/4,
      /*tile_width=*/16, /*tile_length=*/2, {tile0, tile1}));
  stream.set_enable_mmap(true);
  UniqueTiff tif = stream.Open("r");
  ASSERT_NE(tif, nullptr);
  EXPECT_GT(stream.map_calls(), 0U);
  EXPECT_EQ(TIFFNumberOfTiles(tif.get()), 2U);
  EXPECT_EQ(TIFFTileSize(tif.get()), 8);

  std::vector<uint8_t> out0(8, 0x00);
  EXPECT_EQ(TIFFReadEncodedTile(tif.get(), 0, out0.data(), out0.size()), 8);
  const std::vector<uint8_t> expected0 = {
      0x55, 0x55, 0xaa, 0xaa, 0x12, 0x34, 0x56, 0x78,
  };
  EXPECT_EQ(out0, expected0);

  std::vector<uint8_t> out1(8, 0x00);
  EXPECT_EQ(TIFFReadTile(tif.get(), out1.data(), /*x=*/0, /*y=*/2, /*z=*/0,
                         /*s=*/0),
            8);
  const std::vector<uint8_t> expected1 = {
      0xff, 0xab, 0xcd, 0xff, 0xff, 0xff, 0xff, 0xff,
  };
  EXPECT_EQ(out1, expected1);
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifNextTest, DecodeTiledRejectsTileWidthExceedingScanlineSize) {
  // NeXTDecode compares op_offset against td_scanlinesize (from ImageWidth)
  // even when isTiled(tif) sets imagewidth = td_tilewidth. When TileWidth (16)
  // exceeds ImageWidth (4), td_scanlinesize = 1 < tilerowsize = 4, so an RLE
  // run stops at op_offset >= scanline with "Invalid data for scanline".
  const std::vector<uint8_t> tile = {
      MakeNextRle(2, 16),
      MakeNextRle(2, 16),
      MakeNextRle(2, 16),
      MakeNextRle(2, 16),
  };
  MemoryTiffStream stream(BuildTiledNextTiff(
      /*big_endian=*/false, /*big_tiff=*/false, /*width=*/4, /*height=*/4,
      /*tile_width=*/16, /*tile_length=*/4, {tile}));
  UniqueTiff tif = stream.Open("r");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(TIFFScanlineSize(tif.get()), 1);
  ASSERT_EQ(TIFFTileSize(tif.get()), 16);

  std::vector<uint8_t> out(16, 0x00);
  EXPECT_EQ(TIFFReadEncodedTile(tif.get(), 0, out.data(), out.size()),
            static_cast<tmsize_t>(-1));
  EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                      "NeXTDecode",
                                      "Invalid data for scanline"));
}

TEST(TifNextTest, DecodeViaUserBufferCoversStripedTiledPartialAndTrailing) {
  // Striped TIFF used as metadata handle for TIFFReadFromUserBuffer.
  {
    const std::vector<uint8_t> placeholder = {kNextLiteralRow, 0x00, 0x00};
    MemoryTiffStream stream(BuildStripedNextTiff(
        /*big_endian=*/false, /*big_tiff=*/false, /*width=*/8, /*height=*/2,
        /*rows_per_strip=*/2, /*bits_per_sample=*/2, /*samples_per_pixel=*/1,
        PLANARCONFIG_CONTIG, {placeholder}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFScanlineSize(tif.get()), 2);

    // 1) Decode 2 rows using RLE + LITERALSPAN with trailing ignored bytes.
    std::vector<uint8_t> in_trailing = {
        MakeNextRle(1, 8),
        kNextLiteralSpan,
        0x00,
        0x01,
        0x00,
        0x01,
        0x42,
        0xde,
        0xad,  // Trailing bytes after occ reaches 0.
    };
    std::vector<uint8_t> out(4, 0x00);
    EXPECT_EQ(
        TIFFReadFromUserBuffer(tif.get(), 0, in_trailing.data(),
                               in_trailing.size(), out.data(), out.size()),
        1);
    EXPECT_EQ(out, (std::vector<uint8_t>{0x55, 0x55, 0xff, 0x42}));

    // 2) Only 1 encoded row in a 2-row buffer: loop stops when cc == 0 and
    // leaves row 1 pre-filled with 0xff (white).
    std::vector<uint8_t> in_one_row = {kNextLiteralRow, 0x12, 0x34};
    out.assign(4, 0x00);
    EXPECT_EQ(TIFFReadFromUserBuffer(tif.get(), 0, in_one_row.data(),
                                     in_one_row.size(), out.data(), out.size()),
              1);
    EXPECT_EQ(out, (std::vector<uint8_t>{0x12, 0x34, 0xff, 0xff}));

    // 3) Fractional scanline output size (< scanline or non-multiple).
    std::vector<uint8_t> in_ok = {MakeNextRle(0, 8)};
    std::vector<uint8_t> out_frac(3, 0x00);
    EXPECT_EQ(TIFFReadFromUserBuffer(tif.get(), 0, in_ok.data(), in_ok.size(),
                                     out_frac.data(), out_frac.size()),
              0);
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "NeXTDecode",
                                        "Fractional scanlines cannot be read"));
  }

  // Tiled TIFF via TIFFReadFromUserBuffer.
  {
    const std::vector<uint8_t> placeholder = {MakeNextRle(3, 8),
                                              MakeNextRle(3, 8)};
    MemoryTiffStream stream(BuildTiledNextTiff(
        /*big_endian=*/false, /*big_tiff=*/false, /*width=*/8, /*height=*/2,
        /*tile_width=*/8, /*tile_length=*/2, {placeholder}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    std::vector<uint8_t> in_tile = {
        kNextLiteralRow, 0xaa, 0x55, MakeNextRle(0, 4), MakeNextRle(2, 4),
    };
    std::vector<uint8_t> out(4, 0x00);
    EXPECT_EQ(TIFFReadFromUserBuffer(tif.get(), 0, in_tile.data(),
                                     in_tile.size(), out.data(), out.size()),
              1);
    EXPECT_EQ(out, (std::vector<uint8_t>{0xaa, 0x55, 0x00, 0xaa}));
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifNextTest, RejectsUnsupportedBitsPerSample) {
  constexpr std::array<uint16_t, 4> kBadDepths = {1, 4, 8, 16};
  for (uint16_t bps : kBadDepths) {
    SCOPED_TRACE(bps);
    const std::vector<uint8_t> dummy_strip = {kNextLiteralRow, 0x00, 0x00, 0x00,
                                              0x00};
    MemoryTiffStream stream(BuildStripedNextTiff(
        /*big_endian=*/false, /*big_tiff=*/false, /*width=*/8, /*height=*/1,
        /*rows_per_strip=*/1, bps, /*samples_per_pixel=*/1, PLANARCONFIG_CONTIG,
        {dummy_strip}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    const tmsize_t strip_size = TIFFStripSize(tif.get());
    ASSERT_GT(strip_size, 0);
    std::vector<uint8_t> out(static_cast<size_t>(strip_size), 0x00);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), strip_size),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "NeXTPreDecode",
                                        "Unsupported BitsPerSample ="));
  }
}

TEST(TifNextTest, RejectsFractionalScanlinesAndTruncatedOrOverrunRows) {
  struct BadStreamCase {
    std::string_view name;
    std::vector<uint8_t> strip_bytes;
    tmsize_t read_size;  // -1 means full strip (8 bytes for 16x2).
    std::string_view expected_substr;
  };

  // 16 pixels wide -> scanline = 4 bytes, 2 rows -> stripsize = 8 bytes.
  const std::vector<BadStreamCase> kCases = {
      {.name = "FractionalScanline",
       .strip_bytes = {MakeNextRle(0, 16), MakeNextRle(0, 16)},
       .read_size = 3,
       .expected_substr = "Fractional scanlines cannot be read"},
      {.name = "LiteralRowTruncated",
       .strip_bytes = {kNextLiteralRow, 0xaa, 0xbb},
       .read_size = -1,
       .expected_substr = "Not enough data for scanline"},
      {.name = "LiteralSpanHeaderTruncated",
       .strip_bytes = {kNextLiteralSpan, 0x00, 0x01, 0x00},
       .read_size = -1,
       .expected_substr = "Not enough data for scanline"},
      {.name = "LiteralSpanPayloadTruncated",
       .strip_bytes = {kNextLiteralSpan, 0x00, 0x00, 0x00, 0x03, 0x11, 0x22},
       .read_size = -1,
       .expected_substr = "Not enough data for scanline"},
      {.name = "LiteralSpanExceedsScanline",
       .strip_bytes = {kNextLiteralSpan, 0x00, 0x02, 0x00, 0x03, 0x11, 0x22,
                       0x33},
       .read_size = -1,
       .expected_substr = "Not enough data for scanline"},
      {.name = "RleStreamTruncatedBeforeWidth",
       .strip_bytes = {MakeNextRle(0, 8), MakeNextRle(1, 4)},
       .read_size = -1,
       .expected_substr = "Not enough data for scanline"},
  };

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    MemoryTiffStream stream(BuildStripedNextTiff(
        /*big_endian=*/false, /*big_tiff=*/false, /*width=*/16, /*height=*/2,
        /*rows_per_strip=*/2, /*bits_per_sample=*/2, /*samples_per_pixel=*/1,
        PLANARCONFIG_CONTIG, {tc.strip_bytes}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    std::vector<uint8_t> out(8, 0x00);
    const tmsize_t size_arg = tc.read_size < 0 ? 8 : tc.read_size;
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), size_arg),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "NeXTDecode", tc.expected_substr));
  }
}

TEST(TifNextTest, RandomScanlineSeekRejectedAndReReadCurrentRowSucceeds) {
  const std::vector<uint8_t> strip = {
      MakeNextRle(0, 8),
      MakeNextRle(1, 8),
      MakeNextRle(2, 8),
  };
  MemoryTiffStream stream(BuildStripedNextTiff(
      /*big_endian=*/false, /*big_tiff=*/false, /*width=*/8, /*height=*/3,
      /*rows_per_strip=*/3, /*bits_per_sample=*/2, /*samples_per_pixel=*/1,
      PLANARCONFIG_CONTIG, {strip}));
  UniqueTiff tif = stream.Open("r");
  ASSERT_NE(tif, nullptr);

  std::array<uint8_t, 2> row{};
  // Skipping forward from row 0 to row 2 triggers _TIFFNoSeek.
  EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 2, 0), -1);
  EXPECT_TRUE(HasAnyErrorContaining(
      stream.diagnostics().errors(),
      "Compression algorithm does not support random access"));

  stream.diagnostics().Clear();
  EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), 1);
  EXPECT_EQ(row, (std::array<uint8_t, 2>{0x00, 0x00}));

  // Backward seek to row 0 restarts the strip and re-decodes row 0 cleanly.
  row.fill(0xff);
  EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), 1);
  EXPECT_EQ(row, (std::array<uint8_t, 2>{0x00, 0x00}));
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

}  // namespace
}  // namespace libtiff
