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

// Unit tests for libtiff/tif_dumpmode.c (COMPRESSION_NONE / DumpMode codec).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

constexpr uint32_t kDefaultAllocBudget = 1U << 24;

std::vector<uint8_t> MakePatternBytes(size_t count, uint8_t seed = 0x13) {
  std::vector<uint8_t> out(count);
  for (size_t i = 0; i < count; ++i) {
    out[i] = static_cast<uint8_t>((i * 29U + seed) & 0xffU);
  }
  return out;
}

bool HasDiagnosticContaining(const std::vector<DiagnosticRecord>& records,
                             std::string_view module, std::string_view substr) {
  return std::any_of(records.begin(), records.end(),
                     [&](const DiagnosticRecord& rec) {
                       return rec.module == module &&
                              rec.message.find(substr) != std::string::npos;
                     });
}

void ConfigureStripedFields(TIFF* tif, uint32_t width, uint32_t height,
                            uint16_t bits_per_sample,
                            uint16_t samples_per_pixel, uint32_t rows_per_strip,
                            uint16_t planar_config = PLANARCONFIG_CONTIG,
                            uint16_t photometric = PHOTOMETRIC_MINISBLACK) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, photometric), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
}

void ConfigureTiledFields(TIFF* tif, uint32_t width, uint32_t height,
                          uint32_t tile_width, uint32_t tile_height,
                          uint16_t bits_per_sample, uint16_t samples_per_pixel,
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
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
}

TEST(TifDumpModeTest, CodecRegisteredAndConfigured) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_NONE), 1);
  const TIFFCodec* codec = TIFFFindCODEC(COMPRESSION_NONE);
  ASSERT_NE(codec, nullptr);
  EXPECT_EQ(codec->scheme, COMPRESSION_NONE);
  EXPECT_NE(codec->init, nullptr);
}

TEST(TifDumpModeTest, ScanlineRoundTripAcrossModesAndBitDepths) {
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

  constexpr std::array<ScanlineCase, 5> kCases = {{
      {.name = "ClassicLe8BitContig",
       .write_mode = "wl",
       .expect_swapped = false,
       .width = 17,
       .height = 9,
       .bits_per_sample = 8,
       .samples_per_pixel = 3,
       .rows_per_strip = 4,
       .planar_config = PLANARCONFIG_CONTIG,
       .fill_order = FILLORDER_MSB2LSB},
      {.name = "ClassicBe16BitContig",
       .write_mode = "wb",
       .expect_swapped = true,
       .width = 11,
       .height = 7,
       .bits_per_sample = 16,
       .samples_per_pixel = 2,
       .rows_per_strip = 3,
       .planar_config = PLANARCONFIG_CONTIG,
       .fill_order = FILLORDER_MSB2LSB},
      {.name = "BigTiffBe32BitSeparate",
       .write_mode = "w8b",
       .expect_swapped = true,
       .width = 5,
       .height = 6,
       .bits_per_sample = 32,
       .samples_per_pixel = 3,
       .rows_per_strip = 2,
       .planar_config = PLANARCONFIG_SEPARATE,
       .fill_order = FILLORDER_MSB2LSB},
      {.name = "ClassicLe1BitLsb2Msb",
       .write_mode = "wl",
       .expect_swapped = false,
       .width = 23,
       .height = 5,
       .bits_per_sample = 1,
       .samples_per_pixel = 1,
       .rows_per_strip = 5,
       .planar_config = PLANARCONFIG_CONTIG,
       .fill_order = FILLORDER_LSB2MSB},
      {.name = "BigTiffLe16BitSeparate",
       .write_mode = "w8l",
       .expect_swapped = false,
       .width = 8,
       .height = 4,
       .bits_per_sample = 16,
       .samples_per_pixel = 2,
       .rows_per_strip = 2,
       .planar_config = PLANARCONFIG_SEPARATE,
       .fill_order = FILLORDER_MSB2LSB},
  }};

  for (const ScanlineCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    MemoryTiffStream stream;
    std::vector<uint8_t> expected;
    tmsize_t scanline_size = 0;
    const uint16_t planes =
        tc.planar_config == PLANARCONFIG_SEPARATE ? tc.samples_per_pixel : 1;

    {
      UniqueTiff writer = stream.Open(tc.write_mode);
      ASSERT_NE(writer, nullptr);
      ConfigureStripedFields(
          writer.get(), tc.width, tc.height, tc.bits_per_sample,
          tc.samples_per_pixel, tc.rows_per_strip, tc.planar_config,
          tc.samples_per_pixel == 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_FILLORDER, tc.fill_order),
                1);

      scanline_size = TIFFScanlineSize(writer.get());
      ASSERT_GT(scanline_size, 0);
      expected = MakePatternBytes(
          static_cast<size_t>(scanline_size) * tc.height * planes, 0x31);

      for (uint16_t s = 0; s < planes; ++s) {
        for (uint32_t row = 0; row < tc.height; ++row) {
          const size_t offset = (static_cast<size_t>(s) * tc.height + row) *
                                static_cast<size_t>(scanline_size);
          // TIFFWriteScanline may byte-swap in place on the caller's buffer.
          std::vector<uint8_t> scratch(
              expected.begin() + offset,
              expected.begin() + offset + scanline_size);
          ASSERT_EQ(TIFFWriteScanline(writer.get(), scratch.data(), row, s), 1);
        }
      }
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFIsByteSwapped(reader.get()), tc.expect_swapped ? 1 : 0);

    uint16_t compression = 0;
    ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &compression), 1);
    EXPECT_EQ(compression, COMPRESSION_NONE);

    ASSERT_EQ(TIFFScanlineSize(reader.get()), scanline_size);
    std::vector<uint8_t> actual(static_cast<size_t>(scanline_size));
    for (uint16_t s = 0; s < planes; ++s) {
      for (uint32_t row = 0; row < tc.height; ++row) {
        SCOPED_TRACE(row);
        std::fill(actual.begin(), actual.end(), 0);
        ASSERT_EQ(TIFFReadScanline(reader.get(), actual.data(), row, s), 1);
        const size_t offset = (static_cast<size_t>(s) * tc.height + row) *
                              static_cast<size_t>(scanline_size);
        EXPECT_EQ(actual, std::vector<uint8_t>(
                              expected.begin() + offset,
                              expected.begin() + offset + scanline_size));
      }
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifDumpModeTest, EncodedStripPartialMappedAndUserBufferDecode) {
  MemoryTiffStream stream;
  constexpr uint32_t kWidth = 12;
  constexpr uint32_t kHeight = 8;
  constexpr uint32_t kRowsPerStrip = 4;
  std::vector<uint8_t> expected;
  tmsize_t strip_size = 0;

  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedFields(writer.get(), kWidth, kHeight, 8, 3, kRowsPerStrip,
                           PLANARCONFIG_CONTIG, PHOTOMETRIC_RGB);
    strip_size = TIFFStripSize(writer.get());
    ASSERT_GT(strip_size, 0);
    expected = MakePatternBytes(static_cast<size_t>(strip_size) * 2, 0x55);

    for (uint32_t strip = 0; strip < 2; ++strip) {
      std::vector<uint8_t> scratch(
          expected.begin() + static_cast<size_t>(strip) * strip_size,
          expected.begin() + static_cast<size_t>(strip + 1) * strip_size);
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), strip, scratch.data(),
                                      strip_size),
                strip_size);
    }
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  // Full strip read with size = -1 bypasses TIFFReadEncodedStrip shortcut and
  // enters DumpModeDecode via tif_decodestrip.
  {
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> buf(static_cast<size_t>(strip_size));
    for (uint32_t strip = 0; strip < 2; ++strip) {
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), strip, buf.data(),
                                     static_cast<tmsize_t>(-1)),
                strip_size);
      EXPECT_EQ(
          buf,
          std::vector<uint8_t>(
              expected.begin() + static_cast<size_t>(strip) * strip_size,
              expected.begin() + static_cast<size_t>(strip + 1) * strip_size));
    }

    // Partial strip read (< strip_size) also routes through DumpModeDecode.
    const tmsize_t partial_size = strip_size / 2;
    std::vector<uint8_t> partial(static_cast<size_t>(partial_size));
    EXPECT_EQ(
        TIFFReadEncodedStrip(reader.get(), 1, partial.data(), partial_size),
        partial_size);
    EXPECT_EQ(partial, std::vector<uint8_t>(
                           expected.begin() + static_cast<size_t>(strip_size),
                           expected.begin() + static_cast<size_t>(strip_size) +
                               partial_size));
  }

  // Memory-mapped open ("rM") routes TIFFReadEncodedStrip through
  // DumpModeDecode as well (note 'm' disables mmap while 'M' enables it).
  stream.set_enable_mmap(true);
  {
    UniqueTiff mapped_reader = stream.Open("rM");
    ASSERT_NE(mapped_reader, nullptr);
    EXPECT_GT(stream.map_calls(), 0U);
    std::vector<uint8_t> buf(static_cast<size_t>(strip_size));
    EXPECT_EQ(
        TIFFReadEncodedStrip(mapped_reader.get(), 0, buf.data(), strip_size),
        strip_size);
    EXPECT_EQ(buf, std::vector<uint8_t>(expected.begin(),
                                        expected.begin() + strip_size));

    // TIFFReadFromUserBuffer exercises DumpModeDecode both with separate output
    // buffer and in-place zero-copy (inbuf == outbuf).
    std::vector<uint8_t> user_in(expected.begin() + strip_size, expected.end());
    std::vector<uint8_t> user_out(static_cast<size_t>(strip_size));
    EXPECT_EQ(TIFFReadFromUserBuffer(mapped_reader.get(), 1, user_in.data(),
                                     strip_size, user_out.data(), strip_size),
              1);
    EXPECT_EQ(user_out, std::vector<uint8_t>(expected.begin() + strip_size,
                                             expected.end()));

    EXPECT_EQ(TIFFReadFromUserBuffer(mapped_reader.get(), 1, user_in.data(),
                                     strip_size, user_in.data(), strip_size),
              1);
    EXPECT_EQ(user_in, std::vector<uint8_t>(expected.begin() + strip_size,
                                            expected.end()));
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifDumpModeTest, TiledWriteReadMappedAndUserBuffer) {
  MemoryTiffStream stream;
  constexpr uint32_t kWidth = 32;
  constexpr uint32_t kHeight = 32;
  constexpr uint32_t kTileSize = 16;
  std::vector<uint8_t> expected;
  tmsize_t tile_bytes = 0;
  uint32_t num_tiles = 0;

  {
    UniqueTiff writer = stream.Open("wb");
    ASSERT_NE(writer, nullptr);
    ConfigureTiledFields(writer.get(), kWidth, kHeight, kTileSize, kTileSize,
                         16, 1);
    tile_bytes = TIFFTileSize(writer.get());
    ASSERT_GT(tile_bytes, 0);
    num_tiles = TIFFNumberOfTiles(writer.get());
    ASSERT_EQ(num_tiles, 4U);

    expected =
        MakePatternBytes(static_cast<size_t>(tile_bytes) * num_tiles, 0x7a);
    for (uint32_t y = 0; y < kHeight; y += kTileSize) {
      for (uint32_t x = 0; x < kWidth; x += kTileSize) {
        const uint32_t tile = TIFFComputeTile(writer.get(), x, y, 0, 0);
        std::vector<uint8_t> scratch(
            expected.begin() + static_cast<size_t>(tile) * tile_bytes,
            expected.begin() + static_cast<size_t>(tile + 1) * tile_bytes);
        if (tile < 2) {
          ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), tile, scratch.data(),
                                         tile_bytes),
                    tile_bytes);
        } else {
          ASSERT_EQ(TIFFWriteTile(writer.get(), scratch.data(), x, y, 0, 0),
                    tile_bytes);
        }
      }
    }
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  // TIFFReadTile calls TIFFReadEncodedTile with size = -1 -> DumpModeDecode.
  {
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> tile_buf(static_cast<size_t>(tile_bytes));
    for (uint32_t y = 0; y < kHeight; y += kTileSize) {
      for (uint32_t x = 0; x < kWidth; x += kTileSize) {
        const uint32_t tile = TIFFComputeTile(reader.get(), x, y, 0, 0);
        ASSERT_EQ(TIFFReadTile(reader.get(), tile_buf.data(), x, y, 0, 0),
                  tile_bytes);
        EXPECT_EQ(
            tile_buf,
            std::vector<uint8_t>(
                expected.begin() + static_cast<size_t>(tile) * tile_bytes,
                expected.begin() + static_cast<size_t>(tile + 1) * tile_bytes));
      }
    }

    // Partial encoded tile read (< tile_bytes).
    const tmsize_t half = tile_bytes / 2;
    std::vector<uint8_t> partial(static_cast<size_t>(half));
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 2, partial.data(), half), half);
    EXPECT_EQ(
        partial,
        std::vector<uint8_t>(
            expected.begin() + static_cast<size_t>(2) * tile_bytes,
            expected.begin() + static_cast<size_t>(2) * tile_bytes + half));
  }

  stream.set_enable_mmap(true);
  {
    UniqueTiff mapped = stream.Open("rM");
    ASSERT_NE(mapped, nullptr);
    EXPECT_GT(stream.map_calls(), 0U);
    std::vector<uint8_t> tile_buf(static_cast<size_t>(tile_bytes));
    EXPECT_EQ(TIFFReadEncodedTile(mapped.get(), 3, tile_buf.data(), tile_bytes),
              tile_bytes);
    EXPECT_EQ(tile_buf,
              std::vector<uint8_t>(
                  expected.begin() + static_cast<size_t>(3) * tile_bytes,
                  expected.end()));
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifDumpModeTest, EncodeHandlesSmallBufferChunkingAndDynamicGrowth) {
  // Small custom write buffer forces DumpModeEncode to split single scanlines
  // across multiple TIFFFlushData1 calls (tif_rawcc + n > tif_rawdatasize).
  {
    MemoryTiffStream stream;
    constexpr uint32_t kWidth = 12;
    constexpr uint32_t kHeight = 5;
    std::vector<uint8_t> expected = MakePatternBytes(kWidth * kHeight, 0x21);

    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedFields(writer.get(), kWidth, kHeight, 8, 1, kHeight);
      ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 5), 1);
      for (uint32_t row = 0; row < kHeight; ++row) {
        std::vector<uint8_t> row_buf(
            expected.begin() + static_cast<size_t>(row) * kWidth,
            expected.begin() + static_cast<size_t>(row + 1) * kWidth);
        ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), row, 0), 1);
      }
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> actual(kWidth);
    for (uint32_t row = 0; row < kHeight; ++row) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), actual.data(), row, 0), 1);
      EXPECT_EQ(actual,
                std::vector<uint8_t>(
                    expected.begin() + static_cast<size_t>(row) * kWidth,
                    expected.begin() + static_cast<size_t>(row + 1) * kWidth));
    }
  }

  // Dynamic image growth beyond STRIP_SIZE_DEFAULT (8192 bytes) overflows the
  // default internally-allocated write buffer inside DumpModeEncode.
  {
    MemoryTiffStream stream;
    constexpr uint32_t kWidth = 512;
    constexpr uint32_t kFinalHeight = 20;  // 10240 bytes > 8192 default buffer.
    std::vector<uint8_t> expected =
        MakePatternBytes(kWidth * kFinalHeight, 0x44);

    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedFields(writer.get(), kWidth, 1, 8, 1, kFinalHeight);
      for (uint32_t row = 0; row < kFinalHeight; ++row) {
        std::vector<uint8_t> row_buf(
            expected.begin() + static_cast<size_t>(row) * kWidth,
            expected.begin() + static_cast<size_t>(row + 1) * kWidth);
        ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), row, 0), 1);
      }
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    uint32_t length = 0;
    ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGELENGTH, &length), 1);
    EXPECT_EQ(length, kFinalHeight);
    std::vector<uint8_t> actual(kWidth);
    for (uint32_t row = 0; row < kFinalHeight; ++row) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), actual.data(), row, 0), 1);
      EXPECT_EQ(actual,
                std::vector<uint8_t>(
                    expected.begin() + static_cast<size_t>(row) * kWidth,
                    expected.begin() + static_cast<size_t>(row + 1) * kWidth));
    }
  }
}

TEST(TifDumpModeTest, ZeroCopyWriteAndReadBufferSetup) {
  MemoryTiffStream stream;
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 1;
  // User buffer must outlive UniqueTiff handles configured with it.
  std::vector<uint8_t> raw_write_buf = MakePatternBytes(kWidth * 2, 0x62);
  std::vector<uint8_t> expected(raw_write_buf.begin(),
                                raw_write_buf.begin() + kWidth);

  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedFields(writer.get(), kWidth, kHeight, 8, 1, 1);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), raw_write_buf.data(),
                                   static_cast<tmsize_t>(raw_write_buf.size())),
              1);
    // tif_rawcp == pp skips _TIFFmemcpy inside DumpModeEncode.
    ASSERT_EQ(TIFFWriteScanline(writer.get(), raw_write_buf.data(), 0, 0), 1);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  {
    std::vector<uint8_t> raw_read_buf(kWidth * 2, 0);
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFReadBufferSetup(reader.get(), raw_read_buf.data(),
                                  static_cast<tmsize_t>(raw_read_buf.size())),
              1);
    // tif_rawcp == buf skips _TIFFmemcpy inside DumpModeDecode.
    ASSERT_EQ(TIFFReadScanline(reader.get(), raw_read_buf.data(), 0, 0), 1);
    EXPECT_EQ(std::vector<uint8_t>(raw_read_buf.begin(),
                                   raw_read_buf.begin() + kWidth),
              expected);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifDumpModeTest, EncodeFlushFailureReturnsError) {
  MemoryTiffStream stream;
  UniqueTiff writer = stream.Open("wl");
  ASSERT_NE(writer, nullptr);
  ConfigureStripedFields(writer.get(), 8, 2, 8, 1, 2);
  ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 4), 1);

  std::vector<uint8_t> row_buf = MakePatternBytes(8, 0x11);
  // Fail the underlying WriteProc when DumpModeEncode flushes the full 4-byte
  // buffer via TIFFFlushData1. TIFFWriteScanline forwards tif_encoderow's 0
  // return directly on encode failure.
  stream.FailWritesAfter(stream.write_calls());
  EXPECT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), 0, 0), 0);
  EXPECT_GT(stream.diagnostics().error_count(), 0U);
  stream.ResetFaults();
}

TEST(TifDumpModeTest, SeekForwardAndRewindWithinStrip) {
  MemoryTiffStream stream;
  constexpr uint32_t kWidth = 8;
  constexpr uint32_t kHeight = 6;
  std::vector<uint8_t> expected = MakePatternBytes(kWidth * kHeight, 0x88);

  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedFields(writer.get(), kWidth, kHeight, 8, 1, kHeight);
    std::vector<uint8_t> scratch = expected;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, scratch.data(),
                                    static_cast<tmsize_t>(scratch.size())),
              static_cast<tmsize_t>(scratch.size()));
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> row_buf(kWidth);

  // Seek forward from row 0 -> row 3, forward -> row 5, rewind -> row 1, row 0.
  for (uint32_t row : {3U, 5U, 1U, 0U, 4U}) {
    SCOPED_TRACE(row);
    ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), row, 0), 1);
    EXPECT_EQ(row_buf,
              std::vector<uint8_t>(
                  expected.begin() + static_cast<size_t>(row) * kWidth,
                  expected.begin() + static_cast<size_t>(row + 1) * kWidth));
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifDumpModeTest, DecodeAndSeekErrorPathsOnTruncatedData) {
  // 1. TIFFReadFromUserBuffer with short input for striped and tiled images.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedFields(writer.get(), 8, 2, 8, 1, 2);
      std::vector<uint8_t> strip = MakePatternBytes(16, 0x10);
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(), 16), 16);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> short_in(6, 0xaa);
    std::vector<uint8_t> out(16, 0);
    EXPECT_EQ(TIFFReadFromUserBuffer(reader.get(), 0, short_in.data(), 6,
                                     out.data(), 16),
              0);
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "DumpModeDecode", "Not enough data"));
  }
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureTiledFields(writer.get(), 16, 16, 16, 16, 8, 1);
      std::vector<uint8_t> tile = MakePatternBytes(256, 0x20);
      ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 0, tile.data(), 256), 256);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> short_in(100, 0xbb);
    std::vector<uint8_t> out(256, 0);
    EXPECT_EQ(TIFFReadFromUserBuffer(reader.get(), 0, short_in.data(), 100,
                                     out.data(), 256),
              0);
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "DumpModeDecode", "Not enough data"));
  }

  // 2. Crafted two-strip TIFF where StripByteCounts is shorter than a full
  // strip (two strips avoid ByteCountLooksBad() estimating bytecounts):
  // triggers both DumpModeSeek beyond end of buffer and DumpModeDecode short
  // rawcc.
  {
    RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
    const std::vector<uint8_t> strip_bytes = MakePatternBytes(8, 0x33);
    const uint32_t strip0_off =
        static_cast<uint32_t>(builder.AppendBlob(strip_bytes));
    const uint32_t strip1_off =
        static_cast<uint32_t>(builder.AppendBlob(strip_bytes));
    const std::array<uint32_t, 2> offsets = {strip0_off, strip1_off};
    const std::array<uint32_t, 2> counts = {
        static_cast<uint32_t>(strip_bytes.size()),
        static_cast<uint32_t>(strip_bytes.size())};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 8),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offsets),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 4),
        builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, counts),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });

    MemoryTiffStream stream(builder.Take());
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> row(4, 0xee);
    // Row 3 seeks 12 bytes into an 8-byte raw buffer -> DumpModeSeek fails.
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 3, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "DumpModeSeek",
                                        "Seek beyond end of raw data buffer"));

    // Row 2 seeks 8 bytes (succeeds), leaving 0 raw bytes -> DumpModeDecode
    // fails.
    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 2, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "DumpModeDecode", "Not enough data"));

    // Encoded strip read with size = -1 fails in DumpModeDecode.
    stream.diagnostics().Clear();
    std::vector<uint8_t> strip_out(16, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip_out.data(),
                                   static_cast<tmsize_t>(-1)),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "DumpModeDecode", "Not enough data"));
  }

  // 3. Crafted BigTIFF tiled image with short TileByteCounts -> DumpModeDecode.
  {
    RawTiffBuilder builder(/*big_endian=*/true, /*big_tiff=*/true);
    const std::vector<uint8_t> tile_bytes = MakePatternBytes(64, 0x77);
    const uint64_t tile_off = builder.AppendBlob(tile_bytes);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        builder.MakeLong(TIFFTAG_TILEWIDTH, 16),
        builder.MakeLong(TIFFTAG_TILELENGTH, 16),
        builder.MakeLong8(TIFFTAG_TILEOFFSETS, tile_off),
        builder.MakeLong8(TIFFTAG_TILEBYTECOUNTS, tile_bytes.size()),
    });

    MemoryTiffStream stream(builder.Take());
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> tile_out(256, 0);
    EXPECT_EQ(TIFFReadTile(reader.get(), tile_out.data(), 0, 0, 0, 0),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "DumpModeDecode", "Not enough data"));
  }
}

TEST(TifDumpModeTest, WriteSeekBeyondBufferedDataRejected) {
  MemoryTiffStream stream;
  constexpr uint32_t kWidth = 8;
  constexpr uint32_t kHeight = 4;
  std::vector<uint8_t> row0 = MakePatternBytes(kWidth, 0x3c);

  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedFields(writer.get(), kWidth, kHeight, 8, 1, kHeight);

    std::vector<uint8_t> scratch0 = row0;
    ASSERT_EQ(TIFFWriteScanline(writer.get(), scratch0.data(), 0, 0), 1);

    // Skipping rows within the same strip seeks past the 1 row written into
    // tif_rawdata, triggering DumpModeSeek error.
    std::vector<uint8_t> scratch3 = MakePatternBytes(kWidth, 0x91);
    EXPECT_EQ(TIFFWriteScanline(writer.get(), scratch3.data(), 3, 0), -1);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 1U);
  EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
  EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                      "DumpModeSeek",
                                      "Seek beyond end of raw data buffer"));

  stream.diagnostics().Clear();
  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> actual(kWidth, 0);
  EXPECT_EQ(TIFFReadScanline(reader.get(), actual.data(), 0, 0), 1);
  EXPECT_EQ(actual, row0);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifDumpModeTest, SeekIntegerOverflowRejected) {
  MemoryTiffStream stream;
  // User buffer must outlive UniqueTiff configured with TIFFWriteBufferSetup.
  std::array<uint8_t, 16> small_buf{};

  {
    UniqueTiff writer = stream.Open("w8l");
    ASSERT_NE(writer, nullptr);
    // 2^30 width * 4 samples * 2 bytes = 2^33 bytes scanlinesize; seeking 2^30
    // rows exceeds INT64_MAX (TIFF_TMSIZE_T_MAX) without allocating gigabytes.
    ConfigureStripedFields(writer.get(), 0x40000000U, 1, 16, 4, 0xFFFFFFFFU);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), small_buf.data(),
                                   static_cast<tmsize_t>(small_buf.size())),
              1);

    EXPECT_EQ(TIFFWriteScanline(writer.get(), small_buf.data(), 0x40000000U, 0),
              -1);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 1U);
  EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
  EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                      "DumpModeSeek",
                                      "Integer overflow computing seek size"));
}

TEST(TifDumpModeTest, AllocationLimitsWithDumpMode) {
  MemoryTiffStream stream;
  DiagnosticRecorder recorder;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  recorder.Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), kDefaultAllocBudget);

  const std::vector<uint8_t> expected_strip = MakePatternBytes(64, 0x99);
  {
    UniqueTiff writer = stream.Open("wl", opts.get());
    ASSERT_NE(writer, nullptr);
    ConfigureStripedFields(writer.get(), 16, 4, 8, 1, 4);
    {
      // 512 bytes covers TIFFSetupStrips arrays, while failing the 8192-byte
      // default raw write buffer allocation in TIFFWriteBufferSetup.
      ScopedCumulatedMemAllocRemaining tight_budget(writer.get(), 512);
      std::vector<uint8_t> row(16, 0x5a);
      EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), -1);
    }
    EXPECT_TRUE(HasDiagnosticContaining(recorder.errors(),
                                        "TIFFWriteBufferSetup",
                                        "No space for output buffer"));
    recorder.Clear();

    // Succeeds once budget is restored.
    std::vector<uint8_t> scratch = expected_strip;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, scratch.data(), 64), 64);
  }

  UniqueTiff reader = stream.Open("r", opts.get());
  ASSERT_NE(reader, nullptr);
  {
    ScopedSingleMemAllocLimit tiny_single(reader.get(), 32);
    std::vector<uint8_t> row(16, 0);
    // TIFFReadBufferSetup rounds raw buffer size up to 1024 > 32 -> fails.
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
  }
  EXPECT_GT(recorder.error_count(), 0U);
  recorder.Clear();

  std::vector<uint8_t> row(16, 0);
  EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
  EXPECT_EQ(row, std::vector<uint8_t>(expected_strip.begin(),
                                      expected_strip.begin() + 16));
  EXPECT_EQ(recorder.error_count(), 0U);
}

}  // namespace
}  // namespace libtiff
