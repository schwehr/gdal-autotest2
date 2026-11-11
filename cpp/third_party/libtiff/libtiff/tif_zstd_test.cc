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

// Unit tests for libtiff/tif_zstd.c (COMPRESSION_ZSTD codec).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_zstd.c

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

constexpr uint32_t kDefaultAllocBudget = 1U << 24;

std::vector<uint8_t> MakeDeterministicBytes(size_t count, uint32_t seed = 1) {
  std::vector<uint8_t> out(count);
  uint32_t state = seed ? seed : 1;
  for (size_t i = 0; i < count; ++i) {
    // xorshift32 for pseudo-random incompressible data when testing chunked
    // streaming flushes and large strips.
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    out[i] = static_cast<uint8_t>(state & 0xffU);
  }
  return out;
}

std::vector<uint8_t> MakePatternBytes(size_t count, uint8_t seed = 0x1d) {
  std::vector<uint8_t> out(count);
  for (size_t i = 0; i < count; ++i) {
    out[i] = static_cast<uint8_t>((i * 17U + (i >> 3) + seed) & 0xffU);
  }
  return out;
}

std::vector<uint8_t> MakeFloat32Bytes(size_t count) {
  std::vector<uint8_t> out(count * sizeof(float));
  for (size_t i = 0; i < count; ++i) {
    const float value =
        static_cast<float>((static_cast<int>(i % 97) - 48)) * 0.125f +
        static_cast<float>(i) * 0.001f;
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    std::memcpy(out.data() + i * sizeof(float), &bits, sizeof(bits));
  }
  return out;
}

std::vector<uint8_t> MakeFloat64Bytes(size_t count) {
  std::vector<uint8_t> out(count * sizeof(double));
  for (size_t i = 0; i < count; ++i) {
    const double value =
        static_cast<double>((static_cast<int>(i % 67) - 33)) * 0.0625 +
        static_cast<double>(i) * 0.0005;
    const uint64_t bits = std::bit_cast<uint64_t>(value);
    std::memcpy(out.data() + i * sizeof(double), &bits, sizeof(bits));
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

void ConfigureStripedZstd(TIFF* tif, uint32_t width, uint32_t height,
                          uint16_t bits_per_sample, uint16_t samples_per_pixel,
                          uint32_t rows_per_strip,
                          uint16_t planar_config = PLANARCONFIG_CONTIG,
                          uint16_t sample_format = SAMPLEFORMAT_UINT) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, sample_format), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                         samples_per_pixel >= 3 ? PHOTOMETRIC_RGB
                                                : PHOTOMETRIC_MINISBLACK),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ZSTD), 1);
}

void ConfigureTiledZstd(TIFF* tif, uint32_t width, uint32_t height,
                        uint32_t tile_width, uint32_t tile_height,
                        uint16_t bits_per_sample, uint16_t samples_per_pixel,
                        uint16_t planar_config = PLANARCONFIG_CONTIG,
                        uint16_t sample_format = SAMPLEFORMAT_UINT) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile_width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILELENGTH, tile_height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, sample_format), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                         samples_per_pixel >= 3 ? PHOTOMETRIC_RGB
                                                : PHOTOMETRIC_MINISBLACK),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ZSTD), 1);
}

TEST(TifZstdTest, CodecConfiguredAndRegistered) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_ZSTD), 1);
  const TIFFCodec* codec = TIFFFindCODEC(COMPRESSION_ZSTD);
  ASSERT_NE(codec, nullptr);
  EXPECT_EQ(codec->scheme, COMPRESSION_ZSTD);
  EXPECT_STREQ(codec->name, "ZSTD");
  EXPECT_NE(codec->init, nullptr);

  UniqueTiffBuffer<TIFFCodec> configured(TIFFGetConfiguredCODECs());
  ASSERT_NE(configured, nullptr);
  bool found = false;
  for (const TIFFCodec* c = configured.get(); c->name != nullptr; ++c) {
    if (c->scheme == COMPRESSION_ZSTD) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST(TifZstdTest, ZstdLevelTagDefaultsValidAndOutOfRange) {
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedZstd(writer.get(), 16, 4, 8, 1, 4);

    // Default ZSTD compression level is 9.
    int level = 0;
    ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_ZSTD_LEVEL, &level), 1);
    EXPECT_EQ(level, 9);

    // Verify non-ZSTD tags still delegate cleanly through ZSTDVSetField/VGet.
    uint32_t width = 0;
    ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
    EXPECT_EQ(width, 16U);

    // Valid compression levels.
    for (int valid_level : {1, 3, 9, 22}) {
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ZSTD_LEVEL, valid_level), 1);
      int got = -1;
      ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_ZSTD_LEVEL, &got), 1);
      EXPECT_EQ(got, valid_level);
    }

    // Out-of-range levels warn via TIFFWarningExtR but still return 1.
    for (int bad_level : {0, -5, 100}) {
      stream.diagnostics().Clear();
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ZSTD_LEVEL, bad_level), 1);
      EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().warnings(),
                                          "ZSTDVSetField",
                                          "ZSTD_LEVEL should be between 1 and"))
          << "bad_level=" << bad_level;
      int got = 999;
      ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_ZSTD_LEVEL, &got), 1);
      EXPECT_EQ(got, bad_level);
    }

    // Reset to valid level 3 and write data to confirm round-trip.
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ZSTD_LEVEL, 3), 1);
    const std::vector<uint8_t> payload = MakePatternBytes(16 * 4);
    std::vector<uint8_t> mutable_payload = payload;
    ASSERT_EQ(
        TIFFWriteEncodedStrip(writer.get(), 0, mutable_payload.data(),
                              static_cast<tmsize_t>(mutable_payload.size())),
        static_cast<tmsize_t>(payload.size()));
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  uint16_t compression = 0;
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &compression), 1);
  EXPECT_EQ(compression, COMPRESSION_ZSTD);

  std::vector<uint8_t> decoded(16 * 4, 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                 static_cast<tmsize_t>(decoded.size())),
            static_cast<tmsize_t>(decoded.size()));
  EXPECT_EQ(decoded, MakePatternBytes(16 * 4));
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifZstdTest, ScanlineRoundTripModesBitDepthsAndPlanar) {
  struct ScanlineCase {
    std::string_view name;
    const char* write_mode;
    bool expect_swapped;
    bool expect_big_tiff;
    uint32_t width;
    uint32_t height;
    uint16_t bits_per_sample;
    uint16_t samples_per_pixel;
    uint32_t rows_per_strip;
    uint16_t planar_config;
    int zstd_level;
  };

  constexpr std::array<ScanlineCase, 5> kCases = {{
      {.name = "ClassicLe8BitContig",
       .write_mode = "wl",
       .expect_swapped = false,
       .expect_big_tiff = false,
       .width = 19,
       .height = 10,
       .bits_per_sample = 8,
       .samples_per_pixel = 3,
       .rows_per_strip = 3,
       .planar_config = PLANARCONFIG_CONTIG,
       .zstd_level = 1},
      {.name = "ClassicBe16BitContig",
       .write_mode = "wb",
       .expect_swapped = true,
       .expect_big_tiff = false,
       .width = 13,
       .height = 9,
       .bits_per_sample = 16,
       .samples_per_pixel = 3,
       .rows_per_strip = 4,
       .planar_config = PLANARCONFIG_CONTIG,
       .zstd_level = 6},
      {.name = "BigTiffLe32BitMono",
       .write_mode = "w8l",
       .expect_swapped = false,
       .expect_big_tiff = true,
       .width = 11,
       .height = 11,
       .bits_per_sample = 32,
       .samples_per_pixel = 1,
       .rows_per_strip = 5,
       .planar_config = PLANARCONFIG_CONTIG,
       .zstd_level = 9},
      {.name = "BigTiffBe8BitSeparate",
       .write_mode = "w8b",
       .expect_swapped = true,
       .expect_big_tiff = true,
       .width = 15,
       .height = 7,
       .bits_per_sample = 8,
       .samples_per_pixel = 3,
       .rows_per_strip = 3,
       .planar_config = PLANARCONFIG_SEPARATE,
       .zstd_level = 12},
      {.name = "ClassicLe16BitSeparate",
       .write_mode = "wl",
       .expect_swapped = false,
       .expect_big_tiff = false,
       .width = 12,
       .height = 6,
       .bits_per_sample = 16,
       .samples_per_pixel = 2,
       .rows_per_strip = 2,
       .planar_config = PLANARCONFIG_SEPARATE,
       .zstd_level = 19},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    MemoryTiffStream stream;
    std::vector<std::vector<uint8_t>> expected_rows;
    tmsize_t scanline_size = 0;

    {
      UniqueTiff writer = stream.Open(tc.write_mode);
      ASSERT_NE(writer, nullptr);
      ConfigureStripedZstd(writer.get(), tc.width, tc.height,
                           tc.bits_per_sample, tc.samples_per_pixel,
                           tc.rows_per_strip, tc.planar_config);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ZSTD_LEVEL, tc.zstd_level),
                1);

      scanline_size = TIFFScanlineSize(writer.get());
      ASSERT_GT(scanline_size, 0);

      const uint16_t planes =
          tc.planar_config == PLANARCONFIG_SEPARATE ? tc.samples_per_pixel : 1;
      expected_rows.reserve(static_cast<size_t>(planes) * tc.height);
      for (uint16_t s = 0; s < planes; ++s) {
        for (uint32_t row = 0; row < tc.height; ++row) {
          std::vector<uint8_t> line =
              MakePatternBytes(static_cast<size_t>(scanline_size),
                               static_cast<uint8_t>(row * 11U + s * 37U + 5U));
          expected_rows.push_back(line);
          // TIFFWriteScanline may swab the caller buffer in place.
          std::vector<uint8_t> scratch = line;
          ASSERT_EQ(TIFFWriteScanline(writer.get(), scratch.data(), row, s), 1);
        }
      }
      ASSERT_EQ(TIFFFlush(writer.get()), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFIsByteSwapped(reader.get()), tc.expect_swapped ? 1 : 0);
    EXPECT_EQ(TIFFIsBigTIFF(reader.get()), tc.expect_big_tiff ? 1 : 0);
    EXPECT_EQ(TIFFScanlineSize(reader.get()), scanline_size);

    const uint16_t planes =
        tc.planar_config == PLANARCONFIG_SEPARATE ? tc.samples_per_pixel : 1;
    size_t idx = 0;
    for (uint16_t s = 0; s < planes; ++s) {
      for (uint32_t row = 0; row < tc.height; ++row) {
        std::vector<uint8_t> actual(static_cast<size_t>(scanline_size), 0);
        ASSERT_EQ(TIFFReadScanline(reader.get(), actual.data(), row, s), 1);
        EXPECT_EQ(actual, expected_rows[idx++]);
      }
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifZstdTest, EncodedStripRoundTripWithPredictors) {
  struct StripPredictorCase {
    std::string_view name;
    const char* write_mode;
    uint32_t width;
    uint32_t height;
    uint16_t bits_per_sample;
    uint16_t samples_per_pixel;
    uint32_t rows_per_strip;
    uint16_t planar_config;
    uint16_t predictor;
    uint16_t sample_format;
  };

  constexpr std::array<StripPredictorCase, 7> kCases = {{
      {.name = "NoPredictor8Bit",
       .write_mode = "wl",
       .width = 24,
       .height = 10,
       .bits_per_sample = 8,
       .samples_per_pixel = 3,
       .rows_per_strip = 4,
       .planar_config = PLANARCONFIG_CONTIG,
       .predictor = PREDICTOR_NONE,
       .sample_format = SAMPLEFORMAT_UINT},
      {.name = "Horizontal8BitLe",
       .write_mode = "wl",
       .width = 25,
       .height = 9,
       .bits_per_sample = 8,
       .samples_per_pixel = 4,
       .rows_per_strip = 3,
       .planar_config = PLANARCONFIG_CONTIG,
       .predictor = PREDICTOR_HORIZONTAL,
       .sample_format = SAMPLEFORMAT_UINT},
      {.name = "Horizontal16BitBe",
       .write_mode = "wb",
       .width = 20,
       .height = 8,
       .bits_per_sample = 16,
       .samples_per_pixel = 3,
       .rows_per_strip = 3,
       .planar_config = PLANARCONFIG_CONTIG,
       .predictor = PREDICTOR_HORIZONTAL,
       .sample_format = SAMPLEFORMAT_UINT},
      {.name = "Horizontal32BitBeSeparate",
       .write_mode = "wb",
       .width = 16,
       .height = 6,
       .bits_per_sample = 32,
       .samples_per_pixel = 2,
       .rows_per_strip = 4,
       .planar_config = PLANARCONFIG_SEPARATE,
       .predictor = PREDICTOR_HORIZONTAL,
       .sample_format = SAMPLEFORMAT_UINT},
      {.name = "Horizontal64BitLe",
       .write_mode = "wl",
       .width = 8,
       .height = 5,
       .bits_per_sample = 64,
       .samples_per_pixel = 1,
       .rows_per_strip = 2,
       .planar_config = PLANARCONFIG_CONTIG,
       .predictor = PREDICTOR_HORIZONTAL,
       .sample_format = SAMPLEFORMAT_UINT},
      {.name = "FloatingPoint32BitLe",
       .write_mode = "wl",
       .width = 20,
       .height = 6,
       .bits_per_sample = 32,
       .samples_per_pixel = 3,
       .rows_per_strip = 3,
       .planar_config = PLANARCONFIG_CONTIG,
       .predictor = PREDICTOR_FLOATINGPOINT,
       .sample_format = SAMPLEFORMAT_IEEEFP},
      {.name = "FloatingPoint64BitBe",
       .write_mode = "wb",
       .width = 12,
       .height = 5,
       .bits_per_sample = 64,
       .samples_per_pixel = 2,
       .rows_per_strip = 2,
       .planar_config = PLANARCONFIG_CONTIG,
       .predictor = PREDICTOR_FLOATINGPOINT,
       .sample_format = SAMPLEFORMAT_IEEEFP},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    MemoryTiffStream stream;
    std::vector<std::vector<uint8_t>> expected_strips;

    {
      UniqueTiff writer = stream.Open(tc.write_mode);
      ASSERT_NE(writer, nullptr);
      ConfigureStripedZstd(writer.get(), tc.width, tc.height,
                           tc.bits_per_sample, tc.samples_per_pixel,
                           tc.rows_per_strip, tc.planar_config,
                           tc.sample_format);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PREDICTOR, tc.predictor), 1);

      uint16_t got_predictor = 0;
      ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_PREDICTOR, &got_predictor),
                1);
      EXPECT_EQ(got_predictor, tc.predictor);

      const uint32_t num_strips = TIFFNumberOfStrips(writer.get());
      const uint32_t strips_per_plane =
          (tc.height + tc.rows_per_strip - 1) / tc.rows_per_strip;
      expected_strips.reserve(num_strips);

      for (uint32_t strip = 0; strip < num_strips; ++strip) {
        const uint32_t strip_in_plane = strip % strips_per_plane;
        const uint32_t remaining_rows =
            tc.height - strip_in_plane * tc.rows_per_strip;
        const uint32_t strip_rows = std::min(tc.rows_per_strip, remaining_rows);
        const tmsize_t bytes_needed = TIFFVStripSize(writer.get(), strip_rows);
        ASSERT_GT(bytes_needed, 0);

        std::vector<uint8_t> strip_data;
        if (tc.sample_format == SAMPLEFORMAT_IEEEFP &&
            tc.bits_per_sample == 32) {
          strip_data = MakeFloat32Bytes(static_cast<size_t>(bytes_needed) /
                                        sizeof(float));
        } else if (tc.sample_format == SAMPLEFORMAT_IEEEFP &&
                   tc.bits_per_sample == 64) {
          strip_data = MakeFloat64Bytes(static_cast<size_t>(bytes_needed) /
                                        sizeof(double));
        } else {
          strip_data = MakePatternBytes(static_cast<size_t>(bytes_needed),
                                        static_cast<uint8_t>(strip * 19U + 3U));
        }
        expected_strips.push_back(strip_data);
        std::vector<uint8_t> scratch = strip_data;
        ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), strip, scratch.data(),
                                        bytes_needed),
                  bytes_needed);
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    const uint32_t num_strips = TIFFNumberOfStrips(reader.get());
    ASSERT_EQ(num_strips, expected_strips.size());
    for (uint32_t strip = 0; strip < num_strips; ++strip) {
      std::vector<uint8_t> actual(expected_strips[strip].size(), 0);
      const tmsize_t n =
          TIFFReadEncodedStrip(reader.get(), strip, actual.data(),
                               static_cast<tmsize_t>(actual.size()));
      ASSERT_EQ(n, static_cast<tmsize_t>(actual.size()));
      EXPECT_EQ(actual, expected_strips[strip]);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifZstdTest, TiledRoundTripContigSeparateAndPredictors) {
  struct TileCase {
    std::string_view name;
    const char* write_mode;
    uint32_t width;
    uint32_t height;
    uint32_t tile_width;
    uint32_t tile_height;
    uint16_t bits_per_sample;
    uint16_t samples_per_pixel;
    uint16_t planar_config;
    uint16_t predictor;
    uint16_t sample_format;
  };

  constexpr std::array<TileCase, 3> kCases = {{
      {.name = "ClassicLeContig8Bit",
       .write_mode = "wl",
       .width = 40,
       .height = 35,
       .tile_width = 16,
       .tile_height = 16,
       .bits_per_sample = 8,
       .samples_per_pixel = 3,
       .planar_config = PLANARCONFIG_CONTIG,
       .predictor = PREDICTOR_HORIZONTAL,
       .sample_format = SAMPLEFORMAT_UINT},
      {.name = "ClassicBeSeparate16Bit",
       .write_mode = "wb",
       .width = 32,
       .height = 32,
       .tile_width = 16,
       .tile_height = 16,
       .bits_per_sample = 16,
       .samples_per_pixel = 3,
       .planar_config = PLANARCONFIG_SEPARATE,
       .predictor = PREDICTOR_NONE,
       .sample_format = SAMPLEFORMAT_UINT},
      {.name = "BigTiffLeFloat32FpPredictor",
       .write_mode = "w8l",
       .width = 32,
       .height = 24,
       .tile_width = 16,
       .tile_height = 16,
       .bits_per_sample = 32,
       .samples_per_pixel = 2,
       .planar_config = PLANARCONFIG_CONTIG,
       .predictor = PREDICTOR_FLOATINGPOINT,
       .sample_format = SAMPLEFORMAT_IEEEFP},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    MemoryTiffStream stream;
    std::vector<std::vector<uint8_t>> expected_tiles;
    tmsize_t tile_size = 0;

    {
      UniqueTiff writer = stream.Open(tc.write_mode);
      ASSERT_NE(writer, nullptr);
      ConfigureTiledZstd(writer.get(), tc.width, tc.height, tc.tile_width,
                         tc.tile_height, tc.bits_per_sample,
                         tc.samples_per_pixel, tc.planar_config,
                         tc.sample_format);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PREDICTOR, tc.predictor), 1);

      tile_size = TIFFTileSize(writer.get());
      ASSERT_GT(tile_size, 0);
      const uint32_t num_tiles = TIFFNumberOfTiles(writer.get());
      expected_tiles.reserve(num_tiles);

      for (uint32_t tile = 0; tile < num_tiles; ++tile) {
        std::vector<uint8_t> tile_data;
        if (tc.sample_format == SAMPLEFORMAT_IEEEFP) {
          tile_data =
              MakeFloat32Bytes(static_cast<size_t>(tile_size) / sizeof(float));
        } else {
          tile_data = MakePatternBytes(static_cast<size_t>(tile_size),
                                       static_cast<uint8_t>(tile * 23U + 7U));
        }
        expected_tiles.push_back(tile_data);
        std::vector<uint8_t> scratch = tile_data;
        ASSERT_EQ(
            TIFFWriteEncodedTile(writer.get(), tile, scratch.data(), tile_size),
            tile_size);
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFIsTiled(reader.get()), 1);
    const uint32_t num_tiles = TIFFNumberOfTiles(reader.get());
    ASSERT_EQ(num_tiles, expected_tiles.size());
    for (uint32_t tile = 0; tile < num_tiles; ++tile) {
      std::vector<uint8_t> actual(static_cast<size_t>(tile_size), 0);
      ASSERT_EQ(
          TIFFReadEncodedTile(reader.get(), tile, actual.data(), tile_size),
          tile_size);
      EXPECT_EQ(actual, expected_tiles[tile]);
    }
    // Also exercise TIFFReadTile at tile (0, 0).
    std::vector<uint8_t> first_tile(static_cast<size_t>(tile_size), 0);
    ASSERT_EQ(TIFFReadTile(reader.get(), first_tile.data(), 0, 0, 0, 0),
              tile_size);
    EXPECT_EQ(first_tile, expected_tiles[0]);
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifZstdTest, SmallOutputBufferAndLargeStripStreamingFlush) {
  // Use >128 KiB (ZSTD max block size) of incompressible data with a 1 KiB
  // raw write buffer so ZSTD_compressStream emits at least one full block
  // during ZSTDEncode, hitting sp->out_buffer.pos == sp->out_buffer.size and
  // TIFFFlushData1 inside ZSTDEncode, plus multi-chunk ZSTDPostEncode flushing.
  constexpr uint32_t kWidth = 512;
  constexpr uint32_t kHeight = 512;
  const std::vector<uint8_t> expected =
      MakeDeterministicBytes(static_cast<size_t>(kWidth) * kHeight, 0x12345678);
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedZstd(writer.get(), kWidth, kHeight, 8, 1, kHeight);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 1024), 1);

    std::vector<uint8_t> scratch = expected;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, scratch.data(),
                                    static_cast<tmsize_t>(scratch.size())),
              static_cast<tmsize_t>(expected.size()));
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> decoded(expected.size(), 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                 static_cast<tmsize_t>(decoded.size())),
            static_cast<tmsize_t>(decoded.size()));
  EXPECT_EQ(decoded, expected);

  // Also decode row-by-row from the multi-block compressed strip via
  // TIFFReadScanline on a fresh reader handle to exercise incremental
  // ZSTDDecode resumption.
  UniqueTiff row_reader = stream.Open("r");
  ASSERT_NE(row_reader, nullptr);
  std::vector<uint8_t> row_buf(kWidth, 0);
  for (uint32_t row = 0; row < kHeight; ++row) {
    ASSERT_EQ(TIFFReadScanline(row_reader.get(), row_buf.data(), row, 0), 1);
    const size_t offset = static_cast<size_t>(row) * kWidth;
    ASSERT_EQ(std::memcmp(row_buf.data(), expected.data() + offset, kWidth), 0)
        << "row=" << row;
  }
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifZstdTest, WriteFailureDuringEncodeAndPostEncodeFlush) {
  // 1) Inject write failure during ZSTDEncode's TIFFFlushData1 call by feeding
  //    >128 KiB of incompressible data with a 1 KiB write buffer.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedZstd(writer.get(), 512, 512, 8, 1, 512);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 1024), 1);

    std::vector<uint8_t> payload =
        MakeDeterministicBytes(512U * 512U, 0x99887766);
    stream.FailWritesAfter(stream.write_calls());
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, payload.data(),
                                    static_cast<tmsize_t>(payload.size())),
              static_cast<tmsize_t>(-1));
    stream.ResetFaults();
  }

  // 2) Inject write failure during ZSTDPostEncode's TIFFFlushData1 call.
  //    A 64-byte strip stays inside ZSTD's stream buffer until ZSTD_endStream
  //    runs in ZSTDPostEncode, which then invokes TIFFFlushData1.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedZstd(writer.get(), 64, 1, 8, 1, 1);

    std::vector<uint8_t> payload = MakeDeterministicBytes(64, 0xabcdef01);
    stream.FailWritesAfter(stream.write_calls());
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, payload.data(),
                                    static_cast<tmsize_t>(payload.size())),
              static_cast<tmsize_t>(-1));
    stream.ResetFaults();
  }
}

TEST(TifZstdTest, UpdateModeSwitchesBetweenDecodeAndEncodeOnSameHandle) {
  const std::vector<uint8_t> strip0_orig = MakePatternBytes(32, 0x11);
  const std::vector<uint8_t> strip1_orig = MakePatternBytes(32, 0x44);
  const std::vector<uint8_t> strip1_new = MakePatternBytes(32, 0x88);

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedZstd(writer.get(), 16, 4, 8, 1, 2);
    std::vector<uint8_t> s0 = strip0_orig;
    std::vector<uint8_t> s1 = strip1_orig;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, s0.data(), 32), 32);
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, s1.data(), 32), 32);
  }

  // Open in r+ mode to exercise decode -> encode -> decode transitions on the
  // same TIFF* without resetting TIFF_CODERSETUP, triggering ZSTDPreEncode ->
  // ZSTDSetupEncode (freeing dstream) and ZSTDPreDecode -> ZSTDSetupDecode
  // (freeing cstream).
  {
    UniqueTiff rw = stream.Open("r+");
    ASSERT_NE(rw, nullptr);

    std::vector<uint8_t> buf(32, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, buf.data(), 32), 32);
    EXPECT_EQ(buf, strip0_orig);

    // Transition from LSTATE_INIT_DECODE to LSTATE_INIT_ENCODE.
    std::vector<uint8_t> s1_mut = strip1_new;
    ASSERT_EQ(TIFFWriteEncodedStrip(rw.get(), 1, s1_mut.data(), 32), 32);
    ASSERT_EQ(TIFFFlush(rw.get()), 1);

    // Transition from LSTATE_INIT_ENCODE back to LSTATE_INIT_DECODE by reading
    // a different strip (strip 0 != td_curstrip 1).
    std::fill(buf.begin(), buf.end(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, buf.data(), 32), 32);
    EXPECT_EQ(buf, strip0_orig);
  }

  UniqueTiff verify = stream.Open("r");
  ASSERT_NE(verify, nullptr);
  std::vector<uint8_t> out0(32, 0);
  std::vector<uint8_t> out1(32, 0);
  ASSERT_EQ(TIFFReadEncodedStrip(verify.get(), 0, out0.data(), 32), 32);
  ASSERT_EQ(TIFFReadEncodedStrip(verify.get(), 1, out1.data(), 32), 32);
  EXPECT_EQ(out0, strip0_orig);
  EXPECT_EQ(out1, strip1_new);
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifZstdTest, SwitchingCompressionSchemeTriggersCleanup) {
  // 1) Switching compression before writing data triggers ZSTDCleanup with
  //    cstream/dstream still null.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedZstd(writer.get(), 8, 2, 8, 1, 2);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ZSTD_LEVEL, 5), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    uint16_t scheme = 0;
    ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_COMPRESSION, &scheme), 1);
    EXPECT_EQ(scheme, COMPRESSION_NONE);
  }

  // 2) Multi-directory write & read transitions: writing IFD 0 invokes
  //    ZSTDCleanup with cstream active, and advancing from IFD 0 to IFD 1 on
  //    read invokes ZSTDCleanup with dstream active.
  MemoryTiffStream stream;
  const std::vector<uint8_t> zstd_strip = MakePatternBytes(16, 0x05);
  const std::vector<uint8_t> raw_strip = MakePatternBytes(16, 0x7a);
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedZstd(writer.get(), 8, 2, 8, 1, 2);
    std::vector<uint8_t> s0 = zstd_strip;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, s0.data(), 16), 16);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{8}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{2}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{2}), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    std::vector<uint8_t> s1 = raw_strip;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, s1.data(), 16), 16);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> out0(16, 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out0.data(), 16), 16);
  EXPECT_EQ(out0, zstd_strip);

  ASSERT_EQ(TIFFReadDirectory(reader.get()), 1);
  uint16_t dir1_compression = 0;
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &dir1_compression),
            1);
  EXPECT_EQ(dir1_compression, COMPRESSION_NONE);
  std::vector<uint8_t> out1(16, 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out1.data(), 16), 16);
  EXPECT_EQ(out1, raw_strip);
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifZstdTest, CorruptTruncatedAndShortZstdFramesFailCleanly) {
  // First generate a valid 4-byte ZSTD compressed strip and a valid 64-byte
  // ZSTD compressed strip to inspect raw compressed payloads.
  std::vector<uint8_t> small_zstd_frame;
  {
    MemoryTiffStream src_stream;
    {
      UniqueTiff w = src_stream.Open("wl");
      ASSERT_NE(w, nullptr);
      ConfigureStripedZstd(w.get(), 4, 1, 8, 1, 1);
      std::vector<uint8_t> four_bytes = {10, 20, 30, 40};
      ASSERT_EQ(TIFFWriteEncodedStrip(w.get(), 0, four_bytes.data(), 4), 4);
    }
    UniqueTiff r = src_stream.Open("r");
    ASSERT_NE(r, nullptr);
    const tmsize_t raw_size = TIFFRawStripSize(r.get(), 0);
    ASSERT_GT(raw_size, 0);
    small_zstd_frame.resize(static_cast<size_t>(raw_size));
    ASSERT_EQ(TIFFReadRawStrip(r.get(), 0, small_zstd_frame.data(), raw_size),
              raw_size);
  }

  std::vector<uint8_t> larger_zstd_frame;
  {
    MemoryTiffStream src_stream;
    {
      UniqueTiff w = src_stream.Open("wl");
      ASSERT_NE(w, nullptr);
      ConfigureStripedZstd(w.get(), 128, 1, 8, 1, 1);
      std::vector<uint8_t> data = MakeDeterministicBytes(128, 0x55aa3311);
      ASSERT_EQ(TIFFWriteEncodedStrip(w.get(), 0, data.data(), 128), 128);
    }
    UniqueTiff r = src_stream.Open("r");
    ASSERT_NE(r, nullptr);
    const tmsize_t raw_size = TIFFRawStripSize(r.get(), 0);
    ASSERT_GT(raw_size, 12);
    larger_zstd_frame.resize(static_cast<size_t>(raw_size));
    ASSERT_EQ(TIFFReadRawStrip(r.get(), 0, larger_zstd_frame.data(), raw_size),
              raw_size);
  }

  auto build_single_strip_tiff = [](uint32_t width,
                                    absl::Span<const uint8_t> strip_blob) {
    RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
    const uint64_t offset = builder.AppendBlob(strip_blob);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 1),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_ZSTD),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(offset)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(strip_blob.size())),
    });
    return builder.Take();
  };

  // 1) Corrupt ZSTD frame bytes -> ZSTD_decompressStream error.
  {
    const std::array<uint8_t, 8> corrupt = {0xde, 0xad, 0xbe, 0xef,
                                            0x01, 0x02, 0x03, 0x04};
    MemoryTiffStream stream(build_single_strip_tiff(8, corrupt));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> out(8, 0xaa);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 8),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(out, std::vector<uint8_t>(8, 0));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "ZSTDDecode",
                                        "Error in ZSTD_decompressStream()"));
  }

  // 2) Valid ZSTD frame that outputs only 4 bytes when 16 bytes are expected
  // -> "Not enough data at scanline" and trailing bytes zeroed.
  {
    MemoryTiffStream stream(build_single_strip_tiff(16, small_zstd_frame));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> out(16, 0xaa);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ((std::vector<uint8_t>(out.begin(), out.begin() + 4)),
              (std::vector<uint8_t>{10, 20, 30, 40}));
    EXPECT_EQ((std::vector<uint8_t>(out.begin() + 4, out.end())),
              std::vector<uint8_t>(12, 0));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "ZSTDDecode",
                                        "Not enough data at scanline"));
  }

  // 3) Truncated ZSTD frame -> fails cleanly without buffer overread.
  {
    const absl::Span<const uint8_t> truncated(larger_zstd_frame.data(), 8);
    MemoryTiffStream stream(build_single_strip_tiff(128, truncated));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> out(128, 0xaa);
    EXPECT_EQ(TIFFReadScanline(reader.get(), out.data(), 0, 0), -1);
    EXPECT_FALSE(stream.diagnostics().errors().empty());
  }
}

TEST(TifZstdTest, MemoryMappedAndUserBufferDecode) {
  const std::vector<uint8_t> expected = MakePatternBytes(32 * 8, 0x2f);
  MemoryTiffStream stream;
  stream.set_enable_mmap(true);
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedZstd(writer.get(), 32, 8, 8, 1, 8);
    std::vector<uint8_t> scratch = expected;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, scratch.data(),
                                    static_cast<tmsize_t>(scratch.size())),
              static_cast<tmsize_t>(scratch.size()));
  }

  UniqueTiff reader = stream.Open("rM");
  ASSERT_NE(reader, nullptr);
  EXPECT_GT(stream.map_calls(), 0U);

  std::vector<uint8_t> decoded(expected.size(), 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                 static_cast<tmsize_t>(decoded.size())),
            static_cast<tmsize_t>(decoded.size()));
  EXPECT_EQ(decoded, expected);

  const tmsize_t raw_size = TIFFRawStripSize(reader.get(), 0);
  ASSERT_GT(raw_size, 0);
  std::vector<uint8_t> raw(static_cast<size_t>(raw_size));
  ASSERT_EQ(TIFFReadRawStrip(reader.get(), 0, raw.data(), raw_size), raw_size);

  std::vector<uint8_t> user_decoded(expected.size(), 0);
  ASSERT_EQ(TIFFReadFromUserBuffer(reader.get(), 0, raw.data(), raw_size,
                                   user_decoded.data(),
                                   static_cast<tmsize_t>(user_decoded.size())),
            1);
  EXPECT_EQ(user_decoded, expected);
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifZstdTest, InitZstdFailsCleanlyWhenStateAllocationHitsLimit) {
  MemoryTiffStream stream;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  stream.diagnostics().Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), kDefaultAllocBudget);

  UniqueTiff tif = stream.Open("wl", opts.get());
  ASSERT_NE(tif, nullptr);

  // Leave enough budget for _TIFFMergeFields (+1 TIFFField* = 8 bytes) so we
  // isolate the _TIFFmallocExt(tif, sizeof(ZSTDState)) failure path in
  // TIFFInitZSTD without triggering the _TIFFMergeFields realloc leak.
  {
    ScopedCumulatedMemAllocRemaining budget(tif.get(), 32);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_ZSTD),
              0);
  }
  EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                      "TIFFInitZSTD",
                                      "No space for ZSTD state block"));
}

}  // namespace
}  // namespace libtiff
