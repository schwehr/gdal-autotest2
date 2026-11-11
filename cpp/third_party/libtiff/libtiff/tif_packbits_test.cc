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

// Unit tests for libtiff/tif_packbits.c (COMPRESSION_PACKBITS codec).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/temp_file.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

constexpr uint32_t kDefaultAllocBudget = 1U << 24;

bool HasDiagnosticContaining(const std::vector<DiagnosticRecord>& records,
                             std::string_view module, std::string_view substr) {
  return std::any_of(
      records.begin(), records.end(), [&](const DiagnosticRecord& rec) {
        return rec.module == module &&
               rec.message.find(substr) != std::string_view::npos;
      });
}

bool HasAnyMessageContaining(const std::vector<DiagnosticRecord>& records,
                             std::string_view substr) {
  return std::any_of(
      records.begin(), records.end(), [&](const DiagnosticRecord& rec) {
        return rec.message.find(substr) != std::string_view::npos;
      });
}

void ConfigureStripedPackBits(TIFF* tif, uint32_t width, uint32_t height,
                              uint16_t bits_per_sample,
                              uint16_t samples_per_pixel,
                              uint32_t rows_per_strip,
                              uint16_t planar_config = PLANARCONFIG_CONTIG,
                              uint16_t fill_order = FILLORDER_MSB2LSB) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_FILLORDER, fill_order), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                         samples_per_pixel == 3 ? PHOTOMETRIC_RGB
                                                : PHOTOMETRIC_MINISBLACK),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS), 1);
}

void ConfigureTiledPackBits(TIFF* tif, uint32_t width, uint32_t height,
                            uint32_t tile_width, uint32_t tile_height,
                            uint16_t bits_per_sample,
                            uint16_t samples_per_pixel,
                            uint16_t planar_config = PLANARCONFIG_CONTIG) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile_width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILELENGTH, tile_height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                         samples_per_pixel == 3 ? PHOTOMETRIC_RGB
                                                : PHOTOMETRIC_MINISBLACK),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS), 1);
}

std::vector<uint8_t> BuildRawPackBitsStripTiff(
    bool big_endian, bool big_tiff, uint32_t width, uint32_t height,
    uint32_t rows_per_strip, uint16_t bits_per_sample,
    uint16_t samples_per_pixel, const std::vector<uint8_t>& raw_strip) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const uint64_t offset = builder.AppendBlob(raw_strip);
  const uint64_t byte_count = static_cast<uint64_t>(raw_strip.size());

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bits_per_sample),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
  };

  if (big_tiff) {
    const std::array<uint64_t, 1> offs = {offset};
    const std::array<uint64_t, 1> cnts = {byte_count};
    entries.push_back(builder.MakeLong8Array(TIFFTAG_STRIPOFFSETS, offs));
    entries.push_back(builder.MakeLong8Array(TIFFTAG_STRIPBYTECOUNTS, cnts));
  } else {
    const std::array<uint32_t, 1> offs = {static_cast<uint32_t>(offset)};
    const std::array<uint32_t, 1> cnts = {static_cast<uint32_t>(byte_count)};
    entries.push_back(builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offs));
    entries.push_back(builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, cnts));
  }

  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint8_t> MakeDistinctBytes(size_t count, uint8_t start = 0) {
  std::vector<uint8_t> out(count);
  for (size_t i = 0; i < count; ++i) {
    out[i] = static_cast<uint8_t>((start + i) & 0xffU);
  }
  return out;
}

TEST(TifPackBitsTest, CodecRegisteredAndConfigured) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_PACKBITS), 1);

  const TIFFCodec* codec = TIFFFindCODEC(COMPRESSION_PACKBITS);
  ASSERT_NE(codec, nullptr);
  EXPECT_EQ(codec->scheme, COMPRESSION_PACKBITS);
  EXPECT_STREQ(codec->name, "PackBits");
  EXPECT_EQ(codec->init, &TIFFInitPackBits);

  UniqueTiffBuffer<TIFFCodec> configured(TIFFGetConfiguredCODECs());
  ASSERT_NE(configured.get(), nullptr);
  bool found = false;
  for (const TIFFCodec* c = configured.get(); c->name != nullptr; ++c) {
    if (c->scheme == COMPRESSION_PACKBITS) {
      found = true;
      EXPECT_STREQ(c->name, "PackBits");
      EXPECT_EQ(c->init, &TIFFInitPackBits);
    }
  }
  EXPECT_TRUE(found);

  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("wl");
  ASSERT_NE(tif.get(), nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
            1);
  uint16_t compression = 0;
  ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_COMPRESSION, &compression), 1);
  EXPECT_EQ(compression, COMPRESSION_PACKBITS);
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 64ULL);
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifPackBitsTest, ExactEncodedWireBytesForStateTransitions) {
  struct WireCase {
    std::string_view name;
    std::vector<uint8_t> input;
    std::vector<uint8_t> expected_wire;
  };

  std::vector<WireCase> cases;

  // BASE -> LITERAL (1 byte)
  cases.push_back({
      .name = "SingleLiteral",
      .input = {0x42},
      .expected_wire = {0x00, 0x42},
  });

  // BASE -> LITERAL -> LITERAL (4 distinct bytes)
  cases.push_back({
      .name = "ShortLiteral",
      .input = {0x10, 0x20, 0x30, 0x40},
      .expected_wire = {0x03, 0x10, 0x20, 0x30, 0x40},
  });

  // LITERAL wrapping at *lastliteral == 127 -> BASE -> LITERAL (129 bytes)
  {
    std::vector<uint8_t> in = MakeDistinctBytes(129, 0);
    std::vector<uint8_t> wire = {0x7F};
    wire.insert(wire.end(), in.begin(), in.begin() + 128);
    wire.push_back(0x00);
    wire.push_back(in[128]);
    cases.push_back({
        .name = "LiteralWrap129",
        .input = std::move(in),
        .expected_wire = std::move(wire),
    });
  }

  // BASE -> RUN (2 bytes)
  cases.push_back({
      .name = "ShortRun2",
      .input = {0x55, 0x55},
      .expected_wire = {0xFF, 0x55},
  });

  // BASE -> RUN (exact 128 bytes)
  cases.push_back({
      .name = "MaxRun128",
      .input = std::vector<uint8_t>(128, 0xAA),
      .expected_wire = {0x81, 0xAA},
  });

  // BASE -> RUN (129 bytes -> -127 then literal 1)
  cases.push_back({
      .name = "Run129",
      .input = std::vector<uint8_t>(129, 0xAA),
      .expected_wire = {0x81, 0xAA, 0x00, 0xAA},
  });

  // BASE -> RUN (260 bytes -> 128 + 128 + 4)
  cases.push_back({
      .name = "Run260",
      .input = std::vector<uint8_t>(260, 0xAA),
      .expected_wire = {0x81, 0xAA, 0x81, 0xAA, 0xFD, 0xAA},
  });

  // RUN -> RUN (<= 128 and > 128)
  {
    std::vector<uint8_t> in = {0x11, 0x11};
    in.insert(in.end(), 135, 0x22);
    cases.push_back({
        .name = "RunToLongRun",
        .input = std::move(in),
        .expected_wire = {0xFF, 0x11, 0x81, 0x22, 0xFA, 0x22},
    });
  }

  // RUN -> RUN (<= 128) -> LITERAL
  cases.push_back({
      .name = "RunToShortRunToLiteral",
      .input = {0x11, 0x11, 0x22, 0x22, 0x22, 0x33},
      .expected_wire = {0xFF, 0x11, 0xFE, 0x22, 0x00, 0x33},
  });

  // LITERAL -> LITERAL_RUN (n > 128) -> RUN
  {
    std::vector<uint8_t> in = {0x01};
    in.insert(in.end(), 130, 0x22);
    cases.push_back({
        .name = "LiteralToLongRun",
        .input = std::move(in),
        .expected_wire = {0x00, 0x01, 0x81, 0x22, 0xFF, 0x22},
    });
  }

  // LITERAL_RUN merge (literal + 2-byte run + literal -> single literal)
  cases.push_back({
      .name = "LiteralRunLiteralMerge",
      .input = {0x01, 0x02, 0x02, 0x03},
      .expected_wire = {0x03, 0x01, 0x02, 0x02, 0x03},
  });

  // LITERAL_RUN merge hitting exact boundary *lastliteral == 127 -> BASE
  {
    std::vector<uint8_t> in = MakeDistinctBytes(126, 0);
    in.push_back(0xCC);
    in.push_back(0xCC);
    in.push_back(0xDD);
    std::vector<uint8_t> wire = {0x7F};
    wire.insert(wire.end(), in.begin(), in.begin() + 126);
    wire.push_back(0xCC);
    wire.push_back(0xCC);
    wire.push_back(0x00);
    wire.push_back(0xDD);
    cases.push_back({
        .name = "LiteralRunMergeHitsBaseAt127",
        .input = std::move(in),
        .expected_wire = std::move(wire),
    });
  }

  // LITERAL_RUN NOT merging when *lastliteral == 126 (127 literals)
  {
    std::vector<uint8_t> in = MakeDistinctBytes(127, 0);
    in.push_back(0xCC);
    in.push_back(0xCC);
    in.push_back(0xDD);
    std::vector<uint8_t> wire = {0x7E};
    wire.insert(wire.end(), in.begin(), in.begin() + 127);
    wire.push_back(0xFF);
    wire.push_back(0xCC);
    wire.push_back(0x00);
    wire.push_back(0xDD);
    cases.push_back({
        .name = "LiteralRunNoMergeAt126",
        .input = std::move(in),
        .expected_wire = std::move(wire),
    });
  }

  // LITERAL_RUN NOT merging when run length >= 3
  cases.push_back({
      .name = "LiteralRunNoMergeLen3",
      .input = {0x01, 0x02, 0x02, 0x02, 0x03},
      .expected_wire = {0x00, 0x01, 0xFE, 0x02, 0x00, 0x03},
  });

  // LITERAL_RUN followed by another run (n > 1)
  cases.push_back({
      .name = "LiteralRunToRun",
      .input = {0x01, 0x02, 0x02, 0x03, 0x03},
      .expected_wire = {0x00, 0x01, 0xFF, 0x02, 0xFF, 0x03},
  });

  for (const auto& tc : cases) {
    SCOPED_TRACE(tc.name);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      const auto width = static_cast<uint32_t>(tc.input.size());
      ConfigureStripedPackBits(writer.get(), width, 1, 8, 1, 1);
      std::vector<uint8_t> mutable_in = tc.input;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, mutable_in.data(),
                                      static_cast<tmsize_t>(mutable_in.size())),
                static_cast<tmsize_t>(tc.input.size()));
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    const tmsize_t raw_size = static_cast<tmsize_t>(tc.expected_wire.size());
    std::vector<uint8_t> actual_wire(tc.expected_wire.size(), 0);
    ASSERT_EQ(TIFFReadRawStrip(reader.get(), 0, actual_wire.data(), -1),
              raw_size);
    EXPECT_EQ(actual_wire, tc.expected_wire);

    std::vector<uint8_t> decoded(tc.input.size(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(), -1),
              static_cast<tmsize_t>(tc.input.size()));
    EXPECT_EQ(decoded, tc.input);
    EXPECT_TRUE(stream.diagnostics().errors().empty());
    EXPECT_TRUE(stream.diagnostics().warnings().empty());
  }
}

TEST(TifPackBitsTest, MultiRowStripPreservesRowBoundaries) {
  MemoryTiffStream stream;
  const std::vector<uint8_t> two_rows(8, 0x55);
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    ConfigureStripedPackBits(writer.get(), 4, 2, 8, 1, 2);
    std::vector<uint8_t> mutable_in = two_rows;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, mutable_in.data(),
                                    static_cast<tmsize_t>(mutable_in.size())),
              8);
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader.get(), nullptr);
  const std::vector<uint8_t> expected_wire = {0xFD, 0x55, 0xFD, 0x55};
  std::vector<uint8_t> actual_wire(expected_wire.size(), 0);
  ASSERT_EQ(TIFFReadRawStrip(reader.get(), 0, actual_wire.data(), -1),
            static_cast<tmsize_t>(expected_wire.size()));
  EXPECT_EQ(actual_wire, expected_wire);

  for (uint32_t row = 0; row < 2; ++row) {
    SCOPED_TRACE(row);
    std::array<uint8_t, 4> row_buf{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), row, 0), 1);
    EXPECT_EQ(row_buf, (std::array<uint8_t, 4>{0x55, 0x55, 0x55, 0x55}));
  }
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifPackBitsTest, ScanlineRoundTripAcrossModesAndBitDepths) {
  struct ScanlineCase {
    std::string_view name;
    const char* write_mode;
    bool expect_swapped;
    uint32_t width;
    uint32_t height;
    uint16_t bits_per_sample;
    uint16_t samples_per_pixel;
    uint32_t rows_per_strip;
    uint16_t planar_config;
    uint16_t fill_order;
  };

  constexpr std::array<ScanlineCase, 6> kCases = {{
      {.name = "ClassicLe8BitContig",
       .write_mode = "wl",
       .expect_swapped = false,
       .width = 37,
       .height = 13,
       .bits_per_sample = 8,
       .samples_per_pixel = 3,
       .rows_per_strip = 4,
       .planar_config = PLANARCONFIG_CONTIG,
       .fill_order = FILLORDER_MSB2LSB},
      {.name = "ClassicBe16BitContig",
       .write_mode = "wb",
       .expect_swapped = true,
       .width = 19,
       .height = 9,
       .bits_per_sample = 16,
       .samples_per_pixel = 2,
       .rows_per_strip = 3,
       .planar_config = PLANARCONFIG_CONTIG,
       .fill_order = FILLORDER_MSB2LSB},
      {.name = "BigTiffBe32BitSeparate",
       .write_mode = "w8b",
       .expect_swapped = true,
       .width = 11,
       .height = 7,
       .bits_per_sample = 32,
       .samples_per_pixel = 3,
       .rows_per_strip = 3,
       .planar_config = PLANARCONFIG_SEPARATE,
       .fill_order = FILLORDER_MSB2LSB},
      {.name = "ClassicLe1BitLsb2Msb",
       .write_mode = "wl",
       .expect_swapped = false,
       .width = 23,
       .height = 6,
       .bits_per_sample = 1,
       .samples_per_pixel = 1,
       .rows_per_strip = 2,
       .planar_config = PLANARCONFIG_CONTIG,
       .fill_order = FILLORDER_LSB2MSB},
      {.name = "BigTiffLe4BitContig",
       .write_mode = "w8l",
       .expect_swapped = false,
       .width = 15,
       .height = 5,
       .bits_per_sample = 4,
       .samples_per_pixel = 1,
       .rows_per_strip = 5,
       .planar_config = PLANARCONFIG_CONTIG,
       .fill_order = FILLORDER_MSB2LSB},
      {.name = "ClassicLe8BitSeparate",
       .write_mode = "wl",
       .expect_swapped = false,
       .width = 13,
       .height = 8,
       .bits_per_sample = 8,
       .samples_per_pixel = 3,
       .rows_per_strip = 3,
       .planar_config = PLANARCONFIG_SEPARATE,
       .fill_order = FILLORDER_MSB2LSB},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    MemoryTiffStream stream;
    tmsize_t scanline_size = 0;
    const uint16_t planes =
        tc.planar_config == PLANARCONFIG_SEPARATE ? tc.samples_per_pixel : 1;
    std::vector<std::vector<uint8_t>> expected_rows;

    {
      UniqueTiff writer = stream.Open(tc.write_mode);
      ASSERT_NE(writer.get(), nullptr);
      EXPECT_EQ(TIFFIsByteSwapped(writer.get()) != 0, tc.expect_swapped);
      ConfigureStripedPackBits(writer.get(), tc.width, tc.height,
                               tc.bits_per_sample, tc.samples_per_pixel,
                               tc.rows_per_strip, tc.planar_config,
                               tc.fill_order);
      scanline_size = TIFFScanlineSize(writer.get());
      ASSERT_GT(scanline_size, 0);

      for (uint16_t s = 0; s < planes; ++s) {
        for (uint32_t r = 0; r < tc.height; ++r) {
          std::vector<uint8_t> row(static_cast<size_t>(scanline_size));
          for (size_t c = 0; c < row.size(); ++c) {
            if ((r + s) % 3 == 0) {
              row[c] = static_cast<uint8_t>((r * 13U + s) & 0xffU);
            } else {
              row[c] = static_cast<uint8_t>((c * 29U + r * 7U + s) & 0xffU);
            }
          }
          expected_rows.push_back(row);
          // Pass mutable copy because tif_postdecode may swab in place.
          std::vector<uint8_t> write_buf = row;
          ASSERT_EQ(TIFFWriteScanline(writer.get(), write_buf.data(), r, s), 1);
        }
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    EXPECT_EQ(TIFFIsByteSwapped(reader.get()) != 0, tc.expect_swapped);
    ASSERT_EQ(TIFFScanlineSize(reader.get()), scanline_size);

    size_t idx = 0;
    for (uint16_t s = 0; s < planes; ++s) {
      for (uint32_t r = 0; r < tc.height; ++r) {
        std::vector<uint8_t> read_buf(static_cast<size_t>(scanline_size), 0);
        ASSERT_EQ(TIFFReadScanline(reader.get(), read_buf.data(), r, s), 1);
        EXPECT_EQ(read_buf, expected_rows[idx++]);
      }
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifPackBitsTest, EncodedStripPartialAndZeroLengthChunks) {
  // Multi-strip with partial final strip and PackBitsEncodeChunk cc < rowsize.
  {
    MemoryTiffStream stream;
    constexpr uint32_t kWidth = 17;
    constexpr uint32_t kHeight = 7;
    constexpr uint32_t kRowsPerStrip = 3;
    std::vector<std::vector<uint8_t>> strips;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      ConfigureStripedPackBits(writer.get(), kWidth, kHeight, 8, 1,
                               kRowsPerStrip);
      const uint32_t num_strips = TIFFNumberOfStrips(writer.get());
      ASSERT_EQ(num_strips, 3U);
      for (uint32_t s = 0; s < num_strips; ++s) {
        const uint32_t rows_in_strip = (s < 2) ? 3U : 1U;
        const tmsize_t strip_bytes =
            static_cast<tmsize_t>(rows_in_strip * kWidth);
        std::vector<uint8_t> data = MakeDistinctBytes(
            static_cast<size_t>(strip_bytes), static_cast<uint8_t>(s * 31U));
        strips.push_back(data);
        std::vector<uint8_t> mutable_data = data;
        ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), s, mutable_data.data(),
                                        strip_bytes),
                  strip_bytes);
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    for (uint32_t s = 0; s < 3; ++s) {
      std::vector<uint8_t> decoded(strips[s].size(), 0);
      ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), s, decoded.data(), -1),
                static_cast<tmsize_t>(strips[s].size()));
      EXPECT_EQ(decoded, strips[s]);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // Explicit partial chunk (cc < rowsize) and zero-byte chunk in
  // PackBitsEncodeChunk.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    ConfigureStripedPackBits(writer.get(), 12, 2, 8, 1, 1);

    std::array<uint8_t, 5> partial = {0x10, 0x10, 0x20, 0x30, 0x40};
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, partial.data(), 5), 5);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, partial.data(), 0), 0);
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifPackBitsTest, TiledRoundTripAndUserBufferDecode) {
  // Classic LE tiled 8-bit RGB with partial edge tiles via TIFFWriteTile.
  {
    MemoryTiffStream stream;
    constexpr uint32_t kWidth = 40;
    constexpr uint32_t kHeight = 36;
    constexpr uint32_t kTileW = 16;
    constexpr uint32_t kTileH = 16;
    std::vector<std::vector<uint8_t>> expected_tiles;
    tmsize_t tile_size = 0;

    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      ConfigureTiledPackBits(writer.get(), kWidth, kHeight, kTileW, kTileH, 8,
                             3, PLANARCONFIG_CONTIG);
      tile_size = TIFFTileSize(writer.get());
      ASSERT_EQ(tile_size, static_cast<tmsize_t>(kTileW * kTileH * 3));

      uint32_t tile_idx = 0;
      for (uint32_t y = 0; y < kHeight; y += kTileH) {
        for (uint32_t x = 0; x < kWidth; x += kTileW) {
          std::vector<uint8_t> tile(static_cast<size_t>(tile_size));
          for (size_t i = 0; i < tile.size(); ++i) {
            tile[i] = (tile_idx % 2 == 0)
                          ? static_cast<uint8_t>((i / 64U + tile_idx) & 0xffU)
                          : static_cast<uint8_t>((i * 19U + tile_idx) & 0xffU);
          }
          expected_tiles.push_back(tile);
          std::vector<uint8_t> mutable_tile = tile;
          ASSERT_EQ(
              TIFFWriteTile(writer.get(), mutable_tile.data(), x, y, 0, 0),
              tile_size);
          ++tile_idx;
        }
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    uint32_t tile_idx = 0;
    for (uint32_t y = 0; y < kHeight; y += kTileH) {
      for (uint32_t x = 0; x < kWidth; x += kTileW) {
        std::vector<uint8_t> decoded(static_cast<size_t>(tile_size), 0);
        ASSERT_EQ(TIFFReadTile(reader.get(), decoded.data(), x, y, 0, 0),
                  tile_size);
        EXPECT_EQ(decoded, expected_tiles[tile_idx]);

        // Also exercise TIFFReadFromUserBuffer on the raw encoded tile.
        const uint32_t tid = TIFFComputeTile(reader.get(), x, y, 0, 0);
        const uint64_t raw_bytes = TIFFGetStrileByteCount(reader.get(), tid);
        ASSERT_GT(raw_bytes, 0ULL);
        std::vector<uint8_t> raw_buf(static_cast<size_t>(raw_bytes));
        ASSERT_EQ(TIFFReadRawTile(reader.get(), tid, raw_buf.data(),
                                  static_cast<tmsize_t>(raw_bytes)),
                  static_cast<tmsize_t>(raw_bytes));
        std::vector<uint8_t> user_decoded(static_cast<size_t>(tile_size), 0);
        ASSERT_EQ(TIFFReadFromUserBuffer(reader.get(), tid, raw_buf.data(),
                                         static_cast<tmsize_t>(raw_bytes),
                                         user_decoded.data(), tile_size),
                  1);
        EXPECT_EQ(user_decoded, expected_tiles[tile_idx]);
        ++tile_idx;
      }
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // BigTIFF BE 16-bit separate planes tiled round-trip.
  {
    MemoryTiffStream stream;
    constexpr uint32_t kWidth = 32;
    constexpr uint32_t kHeight = 32;
    constexpr uint32_t kTileW = 16;
    constexpr uint32_t kTileH = 16;
    std::vector<std::vector<uint8_t>> expected_tiles;
    tmsize_t tile_size = 0;

    {
      UniqueTiff writer = stream.Open("w8b");
      ASSERT_NE(writer.get(), nullptr);
      ConfigureTiledPackBits(writer.get(), kWidth, kHeight, kTileW, kTileH, 16,
                             2, PLANARCONFIG_SEPARATE);
      tile_size = TIFFTileSize(writer.get());
      const uint32_t num_tiles = TIFFNumberOfTiles(writer.get());
      ASSERT_EQ(num_tiles, 8U);
      for (uint32_t t = 0; t < num_tiles; ++t) {
        std::vector<uint8_t> tile = MakeDistinctBytes(
            static_cast<size_t>(tile_size), static_cast<uint8_t>(t * 17U));
        expected_tiles.push_back(tile);
        std::vector<uint8_t> mutable_tile = tile;
        ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), t, mutable_tile.data(),
                                       tile_size),
                  tile_size);
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    EXPECT_EQ(TIFFIsByteSwapped(reader.get()), 1);
    for (uint32_t t = 0; t < expected_tiles.size(); ++t) {
      std::vector<uint8_t> decoded(static_cast<size_t>(tile_size), 0);
      ASSERT_EQ(TIFFReadEncodedTile(reader.get(), t, decoded.data(), -1),
                tile_size);
      EXPECT_EQ(decoded, expected_tiles[t]);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifPackBitsTest, BufferFlushDuringMultiRowLiteralAndRunEncoding) {
  // Case 1: Overflow tif_rawdata while state == LITERAL (slop copy path).
  {
    MemoryTiffStream stream;
    constexpr uint32_t kWidth = 128;
    constexpr uint32_t kRows = 90;
    std::vector<std::vector<uint8_t>> expected_rows;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      // Initial ImageLength=1 keeps tif_rawdatasize at 8192 so 90 rows flush.
      ConfigureStripedPackBits(writer.get(), kWidth, 1, 8, 1, 200);
      for (uint32_t r = 0; r < kRows; ++r) {
        std::vector<uint8_t> row =
            MakeDistinctBytes(kWidth, static_cast<uint8_t>(r));
        expected_rows.push_back(row);
        std::vector<uint8_t> write_buf = row;
        ASSERT_EQ(TIFFWriteScanline(writer.get(), write_buf.data(), r, 0), 1);
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    uint32_t length = 0;
    ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGELENGTH, &length), 1);
    EXPECT_EQ(length, kRows);
    for (uint32_t r = 0; r < kRows; ++r) {
      std::vector<uint8_t> decoded(kWidth, 0);
      ASSERT_EQ(TIFFReadScanline(reader.get(), decoded.data(), r, 0), 1);
      EXPECT_EQ(decoded, expected_rows[r]);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // Case 2: Overflow tif_rawdata while state == LITERAL_RUN (slop copy path).
  {
    MemoryTiffStream stream;
    constexpr uint32_t kWidth = 10000;
    std::vector<uint8_t> row0(kWidth);
    for (uint32_t i = 0; i < kWidth; ++i) {
      row0[i] = static_cast<uint8_t>(i % 251U);
    }
    // In row 1: 910 non-repeating bytes (encoded as 918 bytes after row 0's
    // 10079 bytes -> op at 10997 out of 11000), then 200 copies of 0xAA.
    // LITERAL transitions to LITERAL_RUN, emits (-127, 0xAA) to 10999, and
    // hits (op + 2 >= ep) at `again:` with state == LITERAL_RUN.
    std::vector<uint8_t> row1(kWidth, 0x5A);
    for (uint32_t i = 0; i < 910; ++i) {
      row1[i] = static_cast<uint8_t>(i % 251U);
    }
    std::fill(row1.begin() + 910, row1.begin() + 1110, 0xAA);

    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      // Initial ImageLength=1 sets tif_rawdatasize = 10000 * 1.1 = 11000.
      ConfigureStripedPackBits(writer.get(), kWidth, 1, 8, 1, 2);
      std::vector<uint8_t> w0 = row0;
      std::vector<uint8_t> w1 = row1;
      ASSERT_EQ(TIFFWriteScanline(writer.get(), w0.data(), 0, 0), 1);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), w1.data(), 1, 0), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> d0(kWidth, 0);
    std::vector<uint8_t> d1(kWidth, 0);
    ASSERT_EQ(TIFFReadScanline(reader.get(), d0.data(), 0, 0), 1);
    ASSERT_EQ(TIFFReadScanline(reader.get(), d1.data(), 1, 0), 1);
    EXPECT_EQ(d0, row0);
    EXPECT_EQ(d1, row1);
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // Case 3: Overflow tif_rawdata while state == RUN (else flush path).
  {
    MemoryTiffStream stream;
    constexpr uint32_t kWidth = 100;
    constexpr uint32_t kRows = 90;
    std::vector<uint8_t> pair_row(kWidth);
    for (uint32_t i = 0; i < kWidth; ++i) {
      pair_row[i] = static_cast<uint8_t>((i / 2U) & 0xffU);
    }
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      ConfigureStripedPackBits(writer.get(), kWidth, 1, 8, 1, 200);
      for (uint32_t r = 0; r < kRows; ++r) {
        std::vector<uint8_t> write_buf = pair_row;
        ASSERT_EQ(TIFFWriteScanline(writer.get(), write_buf.data(), r, 0), 1);
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    for (uint32_t r = 0; r < kRows; ++r) {
      std::vector<uint8_t> decoded(kWidth, 0);
      ASSERT_EQ(TIFFReadScanline(reader.get(), decoded.data(), r, 0), 1);
      EXPECT_EQ(decoded, pair_row);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifPackBitsTest, FlushFailureDuringEncodeReturnsZero) {
  // Failure inside literal slop-copy flush path.
  {
    MemoryTiffStream stream;
    constexpr uint32_t kWidth = 128;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    ConfigureStripedPackBits(writer.get(), kWidth, 1, 8, 1, 200);
    for (uint32_t r = 0; r < 63; ++r) {
      std::vector<uint8_t> row =
          MakeDistinctBytes(kWidth, static_cast<uint8_t>(r));
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), r, 0), 1);
    }
    stream.FailWritesAfter(stream.write_calls());
    std::vector<uint8_t> overflow_row = MakeDistinctBytes(kWidth, 63);
    EXPECT_EQ(TIFFWriteScanline(writer.get(), overflow_row.data(), 63, 0), 0);
    EXPECT_FALSE(stream.diagnostics().errors().empty());
  }

  // Failure inside non-literal (RUN) flush path.
  {
    MemoryTiffStream stream;
    constexpr uint32_t kWidth = 100;
    std::vector<uint8_t> pair_row(kWidth);
    for (uint32_t i = 0; i < kWidth; ++i) {
      pair_row[i] = static_cast<uint8_t>((i / 2U) & 0xffU);
    }
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    ConfigureStripedPackBits(writer.get(), kWidth, 1, 8, 1, 200);
    for (uint32_t r = 0; r < 81; ++r) {
      std::vector<uint8_t> write_buf = pair_row;
      ASSERT_EQ(TIFFWriteScanline(writer.get(), write_buf.data(), r, 0), 1);
    }
    stream.FailWritesAfter(stream.write_calls());
    std::vector<uint8_t> overflow_row = pair_row;
    EXPECT_EQ(TIFFWriteScanline(writer.get(), overflow_row.data(), 81, 0), 0);
    EXPECT_FALSE(stream.diagnostics().errors().empty());
  }
}

TEST(TifPackBitsTest, DecodeNoOpCodes) {
  // Valid strip interspersed with -128 (0x80) no-op markers.
  {
    const std::vector<uint8_t> raw = {0x80, 0x02, 0x11, 0x22, 0x33,
                                      0x80, 0xFE, 0x44, 0x80};
    MemoryTiffStream stream(
        BuildRawPackBitsStripTiff(false, false, 6, 1, 1, 8, 1, raw));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 6> decoded{};
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(), -1), 6);
    EXPECT_EQ(decoded,
              (std::array<uint8_t, 6>{0x11, 0x22, 0x33, 0x44, 0x44, 0x44}));
    EXPECT_TRUE(stream.diagnostics().warnings().empty());
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // Stream with only -128 no-op codes leaves occ > 0 and fails cleanly.
  {
    const std::vector<uint8_t> raw = {0x80, 0x80, 0x80};
    MemoryTiffStream stream(
        BuildRawPackBitsStripTiff(false, false, 4, 1, 1, 8, 1, raw));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 4> decoded = {0xAA, 0xAA, 0xAA, 0xAA};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(), -1),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(decoded, (std::array<uint8_t, 4>{0, 0, 0, 0}));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "PackBitsDecode",
                                        "Not enough data for scanline"));
  }
}

TEST(TifPackBitsTest, DecodeOverrunAndTruncatedStreams) {
  // 1. Replicate run exceeding output buffer is clamped with warning.
  {
    // -9 (0xF7) -> replicate 10 times into 4-byte strip.
    const std::vector<uint8_t> raw = {0xF7, 0xAB};
    MemoryTiffStream stream(
        BuildRawPackBitsStripTiff(false, false, 4, 1, 1, 8, 1, raw));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 4> decoded{};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(), -1), 4);
    EXPECT_EQ(decoded, (std::array<uint8_t, 4>{0xAB, 0xAB, 0xAB, 0xAB}));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().warnings(),
                                        "PackBitsDecode",
                                        "Discarding 6 bytes to avoid buffer"));
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // 2. Literal run exceeding output buffer is clamped with warning.
  {
    // n=5 -> 6 literal bytes into 3-byte strip.
    const std::vector<uint8_t> raw = {0x05, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    MemoryTiffStream stream(
        BuildRawPackBitsStripTiff(false, false, 3, 1, 1, 8, 1, raw));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 3> decoded{};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(), -1), 3);
    EXPECT_EQ(decoded, (std::array<uint8_t, 3>{0x10, 0x20, 0x30}));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().warnings(),
                                        "PackBitsDecode",
                                        "Discarding 3 bytes to avoid buffer"));
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // 3. Truncated replicate run (missing value byte after header byte).
  {
    const std::vector<uint8_t> raw = {0x01, 0xAA, 0xBB, 0xFE};
    MemoryTiffStream stream(
        BuildRawPackBitsStripTiff(false, false, 4, 1, 1, 8, 1, raw));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 4> decoded = {0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(), -1),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(decoded, (std::array<uint8_t, 4>{0xAA, 0xBB, 0x00, 0x00}));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().warnings(),
                                        "PackBitsDecode",
                                        "Terminating PackBitsDecode"));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "PackBitsDecode",
                                        "Not enough data for scanline"));
  }

  // 4. Truncated literal run (fewer payload bytes than count + 1).
  {
    const std::vector<uint8_t> raw = {0x00, 0xCC, 0x04, 0x11, 0x22};
    MemoryTiffStream stream(
        BuildRawPackBitsStripTiff(false, false, 5, 1, 1, 8, 1, raw));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 5> decoded = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(), -1),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(decoded, (std::array<uint8_t, 5>{0xCC, 0, 0, 0, 0}));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().warnings(),
                                        "PackBitsDecode",
                                        "Terminating PackBitsDecode"));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "PackBitsDecode",
                                        "Not enough data for scanline"));
  }

  // 5. Multi-row strip where row 0 succeeds and row 1 is short.
  {
    const std::vector<uint8_t> raw = {0x03, 0x11, 0x22, 0x33,
                                      0x44, 0x01, 0x55, 0x66};
    MemoryTiffStream stream(
        BuildRawPackBitsStripTiff(true, true, 4, 2, 2, 8, 1, raw));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 4> row0{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row0.data(), 0, 0), 1);
    EXPECT_EQ(row0, (std::array<uint8_t, 4>{0x11, 0x22, 0x33, 0x44}));

    std::array<uint8_t, 4> row1 = {0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row1.data(), 1, 0), -1);
    EXPECT_EQ(row1, (std::array<uint8_t, 4>{0x55, 0x66, 0x00, 0x00}));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "PackBitsDecode",
                                        "Not enough data for scanline 1"));
  }
}

TEST(TifPackBitsTest, PreEncodeOutOfMemoryFailure) {
  // Trigger PackBitsPreEncode OOM via ScopedSingleMemAllocLimit.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    ConfigureStripedPackBits(writer.get(), 16, 2, 8, 1, 1);

    std::array<uint8_t, 16> row0{};
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, row0.data(), 16), 16);

    ScopedSingleMemAllocLimit limit(writer.get(), sizeof(tmsize_t) - 1);
    std::array<uint8_t, 16> row1{};
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, row1.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_FALSE(stream.diagnostics().errors().empty());
  }

  // Trigger PackBitsPreEncode OOM via ScopedCumulatedMemAllocRemaining.
  {
    MemoryTiffStream stream;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts.get(), nullptr);
    stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), kDefaultAllocBudget);

    UniqueTiff writer = stream.Open("wl", opts.get());
    ASSERT_NE(writer.get(), nullptr);
    ConfigureStripedPackBits(writer.get(), 16, 2, 8, 1, 1);

    std::array<uint8_t, 16> row0{};
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, row0.data(), 16), 16);

    ScopedCumulatedMemAllocRemaining limit(writer.get(), sizeof(tmsize_t) - 1);
    std::array<uint8_t, 16> row1{};
    EXPECT_EQ(TIFFWriteScanline(writer.get(), row1.data(), 1, 0), -1);
    EXPECT_TRUE(HasAnyMessageContaining(stream.diagnostics().errors(),
                                        "cumulated byte limit"));
  }
}

TEST(TifPackBitsTest, MaxCompressionRatioRejectsSuspiciouslySmallLargeStrip) {
  // Uncompressed strip > 100 MiB (1024 * 105000 = 107,520,000 bytes), while
  // compressed byte count is only 10 bytes (< 107,520,000 / 64 = 1,680,000).
  const std::vector<uint8_t> small_blob = {0x81, 0x00, 0x81, 0x00, 0x81,
                                           0x00, 0x81, 0x00, 0x81, 0x00};
  MemoryTiffStream stream(BuildRawPackBitsStripTiff(false, false, 1024, 105000,
                                                    105000, 8, 1, small_blob));
  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader.get(), nullptr);
  EXPECT_EQ(TIFFGetMaxCompressionRatio(reader.get()), 64ULL);

  std::vector<uint8_t> scanline(1024, 0xFF);
  EXPECT_EQ(TIFFReadScanline(reader.get(), scanline.data(), 0, 0), -1);
  EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                      "TIFFFillStrip",
                                      "Likely invalid strip byte count"));
}

TEST(TifPackBitsTest, DiskFileAndMappedReadRoundTrip) {
  TempFile tiff_file("packbits_disk_mmap.tif");
  constexpr uint32_t kWidth = 24;
  constexpr uint32_t kHeight = 6;
  constexpr uint32_t kRowsPerStrip = 3;
  const std::vector<uint8_t> strip0 = MakeDistinctBytes(kWidth * 3, 5);
  const std::vector<uint8_t> strip1(kWidth * 3, 0x7E);

  {
    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts.get(), nullptr);
    recorder.Install(opts.get());
    UniqueTiff writer(TIFFOpenExt(tiff_file.c_str(), "wl", opts.get()));
    ASSERT_NE(writer.get(), nullptr);
    ConfigureStripedPackBits(writer.get(), kWidth, kHeight, 8, 1,
                             kRowsPerStrip);
    std::vector<uint8_t> s0 = strip0;
    std::vector<uint8_t> s1 = strip1;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, s0.data(),
                                    static_cast<tmsize_t>(s0.size())),
              static_cast<tmsize_t>(strip0.size()));
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, s1.data(),
                                    static_cast<tmsize_t>(s1.size())),
              static_cast<tmsize_t>(strip1.size()));
    EXPECT_TRUE(recorder.errors().empty());
  }

  for (const char* read_mode : {"rm", "rC"}) {
    SCOPED_TRACE(read_mode);
    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts.get(), nullptr);
    recorder.Install(opts.get());
    UniqueTiff reader(TIFFOpenExt(tiff_file.c_str(), read_mode, opts.get()));
    ASSERT_NE(reader.get(), nullptr);

    std::vector<uint8_t> out0(strip0.size(), 0);
    std::vector<uint8_t> out1(strip1.size(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out0.data(), -1),
              static_cast<tmsize_t>(strip0.size()));
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 1, out1.data(), -1),
              static_cast<tmsize_t>(strip1.size()));
    EXPECT_EQ(out0, strip0);
    EXPECT_EQ(out1, strip1);
    EXPECT_TRUE(recorder.errors().empty());
  }
}

TEST(TifPackBitsTest, MultiDirectoryStripedAndTiledSwitch) {
  MemoryTiffStream stream;
  const std::vector<uint8_t> dir0_data = MakeDistinctBytes(16, 11);
  const std::vector<uint8_t> dir1_tile(16 * 16, 0x3C);

  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    ConfigureStripedPackBits(writer.get(), 8, 2, 8, 1, 2);
    std::vector<uint8_t> d0 = dir0_data;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, d0.data(), 16), 16);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

    ConfigureTiledPackBits(writer.get(), 16, 16, 16, 16, 8, 1);
    std::vector<uint8_t> d1 = dir1_tile;
    ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 0, d1.data(), 256), 256);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader.get(), nullptr);
  EXPECT_EQ(TIFFIsTiled(reader.get()), 0);
  std::vector<uint8_t> out0(16, 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out0.data(), -1), 16);
  EXPECT_EQ(out0, dir0_data);

  ASSERT_EQ(TIFFSetDirectory(reader.get(), 1), 1);
  EXPECT_EQ(TIFFIsTiled(reader.get()), 1);
  std::vector<uint8_t> out1(256, 0);
  ASSERT_EQ(TIFFReadEncodedTile(reader.get(), 0, out1.data(), -1), 256);
  EXPECT_EQ(out1, dir1_tile);
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

}  // namespace
}  // namespace libtiff
