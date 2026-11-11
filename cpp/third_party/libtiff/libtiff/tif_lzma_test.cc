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

// Unit tests for third_party/tiff/libtiff/tif_lzma.c (COMPRESSION_LZMA).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"
#include "third_party/liblzma/lzma.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/temp_file.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tif_predict.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

constexpr int kLzmaPresetDefault = 6;
constexpr int kLzmaPresetExtreme = static_cast<int>(UINT32_C(1) << 31);
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

std::string FormatDiagnostics(const std::vector<DiagnosticRecord>& records) {
  std::string out;
  for (const DiagnosticRecord& rec : records) {
    out.append(rec.module);
    out.append(": ");
    out.append(rec.message);
    out.push_back('\n');
  }
  return out;
}

constexpr uint32_t Crc32Ieee(absl::Span<const uint8_t> data) {
  uint32_t crc = 0xFFFFFFFFU;
  for (uint8_t byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = (crc & 1U) ? 0xEDB88320U : 0U;
      crc = (crc >> 1) ^ mask;
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

std::vector<uint8_t> MakeDeterministicBytes(size_t count, uint32_t seed) {
  std::vector<uint8_t> out(count);
  uint32_t state = seed;
  for (size_t i = 0; i < count; ++i) {
    state = state * 1664525U + 1013904223U;
    out[i] = static_cast<uint8_t>((state >> 24) ^ ((i * 13U) & 0xFFU));
  }
  return out;
}

void ConfigureStripedLzma(TIFF* tif, uint32_t width, uint32_t height,
                          uint16_t bits_per_sample, uint16_t samples_per_pixel,
                          uint32_t rows_per_strip,
                          uint16_t planar_config = PLANARCONFIG_CONTIG) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                         samples_per_pixel == 3 ? PHOTOMETRIC_RGB
                                                : PHOTOMETRIC_MINISBLACK),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZMA), 1);
}

void ConfigureTiledLzma(TIFF* tif, uint32_t width, uint32_t height,
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
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZMA), 1);
}

std::vector<uint8_t> ExtractValidLzmaStrip(uint32_t width, uint32_t height,
                                           uint32_t rows_per_strip,
                                           absl::Span<const uint8_t> pixels) {
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    EXPECT_NE(writer, nullptr);
    if (writer == nullptr) return {};
    ConfigureStripedLzma(writer.get(), width, height, 8, 1, rows_per_strip);
    const tmsize_t row_bytes = static_cast<tmsize_t>(width);
    EXPECT_EQ(TIFFScanlineSize(writer.get()), row_bytes);
    for (uint32_t r = 0; r < height; ++r) {
      std::vector<uint8_t> row_copy(pixels.begin() + r * width,
                                    pixels.begin() + (r + 1) * width);
      EXPECT_EQ(TIFFWriteScanline(writer.get(), row_copy.data(), r, 0), 1);
    }
  }
  UniqueTiff reader = stream.Open("r");
  EXPECT_NE(reader, nullptr);
  if (reader == nullptr) return {};
  const uint64_t* bytecounts = nullptr;
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_STRIPBYTECOUNTS, &bytecounts),
            1);
  if (bytecounts == nullptr || bytecounts[0] == 0) return {};
  std::vector<uint8_t> raw(static_cast<size_t>(bytecounts[0]));
  const tmsize_t got = TIFFReadRawStrip(reader.get(), 0, raw.data(),
                                        static_cast<tmsize_t>(raw.size()));
  EXPECT_EQ(got, static_cast<tmsize_t>(raw.size()));
  return raw;
}

std::vector<uint8_t> BuildTwoStripRawLzmaTiff(
    uint32_t width, uint32_t height, uint32_t rows_per_strip,
    absl::Span<const uint8_t> strip0, absl::Span<const uint8_t> strip1) {
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  const uint64_t off0 = builder.AppendBlob(strip0);
  const uint64_t off1 = builder.AppendBlob(strip1);
  const std::array<uint32_t, 2> offs = {
      static_cast<uint32_t>(off0),
      static_cast<uint32_t>(off1),
  };
  const std::array<uint32_t, 2> cnts = {
      static_cast<uint32_t>(strip0.size()),
      static_cast<uint32_t>(strip1.size()),
  };
  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_LZMA),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
      builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offs),
      builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, cnts),
  };
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

TEST(TifLzmaTest, CodecIsConfiguredAndCleanupWithoutIo) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_LZMA), 1);
  const TIFFCodec* codec = TIFFFindCODEC(COMPRESSION_LZMA);
  ASSERT_NE(codec, nullptr);
  EXPECT_EQ(codec->scheme, COMPRESSION_LZMA);
  EXPECT_NE(codec->init, nullptr);

  MemoryTiffStream stream;
  {
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZMA),
              1);

    uint16_t comp = 0;
    ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_COMPRESSION, &comp), 1);
    EXPECT_EQ(comp, COMPRESSION_LZMA);

    int preset = -1;
    ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_LZMAPRESET, &preset), 1);
    EXPECT_EQ(preset, kLzmaPresetDefault);

    // Verify vsetparent / vgetparent delegation for non-LZMA tags.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 32U), 1);
    uint32_t width = 0;
    ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
    EXPECT_EQ(width, 32U);

    EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 7000ULL);

    // Switching compression while sp->state == 0 runs LZMACleanup without
    // active lzma_stream state.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    // Re-enable and close without writing any strips/scanlines.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZMA),
              1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 1U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
  }
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifLzmaTest, PresetLevelsAndExtremeFlagRoundTrip) {
  constexpr uint32_t kWidth = 32;
  constexpr uint32_t kHeight = 8;
  constexpr uint32_t kRowsPerStrip = 4;
  constexpr std::array<int, 7> kPresets = {
      0, 1, 3, 6, 9, 0 | kLzmaPresetExtreme, 6 | kLzmaPresetExtreme,
  };
  const std::vector<uint8_t> pixels =
      MakeDeterministicBytes(kWidth * kHeight * 3, 0x12345678U);

  for (int preset : kPresets) {
    SCOPED_TRACE(preset);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedLzma(writer.get(), kWidth, kHeight, 8, 3, kRowsPerStrip);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_LZMAPRESET, preset), 1);
      int actual_preset = -1;
      ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_LZMAPRESET, &actual_preset),
                1);
      EXPECT_EQ(actual_preset, preset);

      const size_t row_bytes = kWidth * 3;
      for (uint32_t r = 0; r < kHeight; ++r) {
        std::vector<uint8_t> row_copy(pixels.begin() + r * row_bytes,
                                      pixels.begin() + (r + 1) * row_bytes);
        ASSERT_EQ(TIFFWriteScanline(writer.get(), row_copy.data(), r, 0), 1);
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    const size_t row_bytes = kWidth * 3;
    std::vector<uint8_t> actual(row_bytes);
    for (uint32_t r = 0; r < kHeight; ++r) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), actual.data(), r, 0), 1);
      EXPECT_EQ(
          std::memcmp(actual.data(), pixels.data() + r * row_bytes, row_bytes),
          0);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifLzmaTest, InvalidPresetsAndMidEncodePresetUpdate) {
  constexpr uint32_t kWidth = 32;
  constexpr uint32_t kHeight = 8;
  constexpr uint32_t kRowsPerStrip = 4;
  const std::vector<uint8_t> strip0_pixels =
      MakeDeterministicBytes(kWidth * kRowsPerStrip, 0xABCDEF01U);
  const std::vector<uint8_t> strip1_pixels =
      MakeDeterministicBytes(kWidth * kRowsPerStrip, 0x01020304U);

  // Invalid presets keep LZMA functioning without errors.
  for (int bad_preset : {10, -1, 0x100}) {
    SCOPED_TRACE(bad_preset);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedLzma(writer.get(), kWidth, kRowsPerStrip, 8, 1,
                           kRowsPerStrip);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_LZMAPRESET, bad_preset), 1);
      int stored = 0;
      EXPECT_EQ(TIFFGetField(writer.get(), TIFFTAG_LZMAPRESET, &stored), 1);
      EXPECT_EQ(stored, bad_preset);
      std::vector<uint8_t> copy = strip0_pixels;
      ASSERT_NE(TIFFWriteEncodedStrip(writer.get(), 0, copy.data(),
                                      static_cast<tmsize_t>(copy.size())),
                tmsize_t(-1));
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> decoded(strip0_pixels.size());
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                   static_cast<tmsize_t>(decoded.size())),
              static_cast<tmsize_t>(strip0_pixels.size()));
    EXPECT_EQ(decoded, strip0_pixels);
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // Update preset after strip 0 when LSTATE_INIT_ENCODE is active.
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedLzma(writer.get(), kWidth, kHeight, 8, 1, kRowsPerStrip);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_LZMAPRESET, 1), 1);
    std::vector<uint8_t> s0 = strip0_pixels;
    ASSERT_NE(TIFFWriteEncodedStrip(writer.get(), 0, s0.data(),
                                    static_cast<tmsize_t>(s0.size())),
              tmsize_t(-1));
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_LZMAPRESET, 3), 1);
    std::vector<uint8_t> s1 = strip1_pixels;
    ASSERT_NE(TIFFWriteEncodedStrip(writer.get(), 1, s1.data(),
                                    static_cast<tmsize_t>(s1.size())),
              tmsize_t(-1));
  }
  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> out0(strip0_pixels.size());
  std::vector<uint8_t> out1(strip1_pixels.size());
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out0.data(),
                                 static_cast<tmsize_t>(out0.size())),
            static_cast<tmsize_t>(strip0_pixels.size()));
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 1, out1.data(),
                                 static_cast<tmsize_t>(out1.size())),
            static_cast<tmsize_t>(strip1_pixels.size()));
  EXPECT_EQ(out0, strip0_pixels);
  EXPECT_EQ(out1, strip1_pixels);
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifLzmaTest, BitsPerSampleDeltaDistanceVariants) {
  constexpr std::array<uint16_t, 7> kBitDepths = {1, 4, 8, 16, 24, 32, 64};
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 8;
  constexpr uint32_t kRowsPerStrip = 4;

  for (uint16_t bps : kBitDepths) {
    SCOPED_TRACE(bps);
    MemoryTiffStream stream;
    std::vector<uint8_t> expected;
    tmsize_t strip_size = 0;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      // Set BITSPERSAMPLE before COMPRESSION so TIFFInitLZMA picks up
      // opt_delta.dist cleanly for both % 8 != 0 and % 8 == 0.
      ConfigureStripedLzma(writer.get(), kWidth, kHeight, bps, 1,
                           kRowsPerStrip);
      if (bps == 32 || bps == 64) {
        ASSERT_EQ(
            TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT),
            1);
      }
      strip_size = TIFFStripSize(writer.get());
      ASSERT_GT(strip_size, 0);
      expected = MakeDeterministicBytes(static_cast<size_t>(strip_size) * 2,
                                        0x5000U + bps);
      for (uint32_t s = 0; s < 2; ++s) {
        std::vector<uint8_t> s_copy(expected.begin() + s * strip_size,
                                    expected.begin() + (s + 1) * strip_size);
        ASSERT_NE(
            TIFFWriteEncodedStrip(writer.get(), s, s_copy.data(), strip_size),
            tmsize_t(-1));
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFStripSize(reader.get()), strip_size);
    for (uint32_t s = 0; s < 2; ++s) {
      std::vector<uint8_t> actual(static_cast<size_t>(strip_size));
      ASSERT_EQ(
          TIFFReadEncodedStrip(reader.get(), s, actual.data(), strip_size),
          strip_size);
      EXPECT_EQ(std::memcmp(actual.data(), expected.data() + s * strip_size,
                            static_cast<size_t>(strip_size)),
                0);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifLzmaTest, PredictorCombinationsRoundTrip) {
  struct PredictorCase {
    uint16_t predictor;
    uint16_t bps;
    uint16_t spp;
    uint16_t sample_format;
  };
  constexpr std::array<PredictorCase, 7> kCases = {{
      {.predictor = PREDICTOR_NONE,
       .bps = 8,
       .spp = 3,
       .sample_format = SAMPLEFORMAT_UINT},
      {.predictor = PREDICTOR_HORIZONTAL,
       .bps = 8,
       .spp = 1,
       .sample_format = SAMPLEFORMAT_UINT},
      {.predictor = PREDICTOR_HORIZONTAL,
       .bps = 8,
       .spp = 3,
       .sample_format = SAMPLEFORMAT_UINT},
      {.predictor = PREDICTOR_HORIZONTAL,
       .bps = 16,
       .spp = 3,
       .sample_format = SAMPLEFORMAT_UINT},
      {.predictor = PREDICTOR_HORIZONTAL,
       .bps = 32,
       .spp = 1,
       .sample_format = SAMPLEFORMAT_UINT},
      {.predictor = PREDICTOR_FLOATINGPOINT,
       .bps = 32,
       .spp = 1,
       .sample_format = SAMPLEFORMAT_IEEEFP},
      {.predictor = PREDICTOR_FLOATINGPOINT,
       .bps = 64,
       .spp = 1,
       .sample_format = SAMPLEFORMAT_IEEEFP},
  }};
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 6;
  constexpr uint32_t kRowsPerStrip = 3;

  for (const PredictorCase& c : kCases) {
    SCOPED_TRACE(c.predictor * 1000 + c.bps * 10 + c.spp);
    MemoryTiffStream stream;
    std::vector<uint8_t> expected;
    tmsize_t row_bytes = 0;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedLzma(writer.get(), kWidth, kHeight, c.bps, c.spp,
                           kRowsPerStrip);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, c.sample_format), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PREDICTOR, c.predictor), 1);

      uint16_t got_pred = 0;
      ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_PREDICTOR, &got_pred), 1);
      EXPECT_EQ(got_pred, c.predictor);

      row_bytes = TIFFScanlineSize(writer.get());
      ASSERT_GT(row_bytes, 0);
      if (c.sample_format == SAMPLEFORMAT_IEEEFP) {
        expected.resize(static_cast<size_t>(row_bytes) * kHeight);
        if (c.bps == 32) {
          for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
            const float v = static_cast<float>(i) * 0.375F - 10.5F;
            std::memcpy(expected.data() + i * sizeof(float), &v, sizeof(float));
          }
        } else {
          for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
            const double v = static_cast<double>(i) * 0.125 - 42.25;
            std::memcpy(expected.data() + i * sizeof(double), &v,
                        sizeof(double));
          }
        }
      } else {
        expected = MakeDeterministicBytes(
            static_cast<size_t>(row_bytes) * kHeight, 0x9E37U + c.bps);
      }

      for (uint32_t r = 0; r < kHeight; ++r) {
        std::vector<uint8_t> row_copy(expected.begin() + r * row_bytes,
                                      expected.begin() + (r + 1) * row_bytes);
        ASSERT_EQ(TIFFWriteScanline(writer.get(), row_copy.data(), r, 0), 1);
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> actual(static_cast<size_t>(row_bytes));
    for (uint32_t r = 0; r < kHeight; ++r) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), actual.data(), r, 0), 1);
      EXPECT_EQ(std::memcmp(actual.data(), expected.data() + r * row_bytes,
                            static_cast<size_t>(row_bytes)),
                0);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifLzmaTest, TiledAndSeparatePlanarRoundTrip) {
  // Separate planar strips written/read per sample plane.
  {
    constexpr uint32_t kWidth = 16;
    constexpr uint32_t kHeight = 8;
    constexpr uint32_t kRowsPerStrip = 4;
    constexpr uint16_t kSamples = 3;
    MemoryTiffStream stream;
    const std::vector<uint8_t> planes =
        MakeDeterministicBytes(kWidth * kHeight * kSamples, 0x22334455U);
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedLzma(writer.get(), kWidth, kHeight, 8, kSamples,
                           kRowsPerStrip, PLANARCONFIG_SEPARATE);
      for (uint16_t s = 0; s < kSamples; ++s) {
        for (uint32_t r = 0; r < kHeight; ++r) {
          const size_t off = (static_cast<size_t>(s) * kHeight + r) * kWidth;
          std::vector<uint8_t> row_copy(planes.begin() + off,
                                        planes.begin() + off + kWidth);
          ASSERT_EQ(TIFFWriteScanline(writer.get(), row_copy.data(), r, s), 1);
        }
      }
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> row(kWidth);
    for (uint16_t s = 0; s < kSamples; ++s) {
      for (uint32_t r = 0; r < kHeight; ++r) {
        const size_t off = (static_cast<size_t>(s) * kHeight + r) * kWidth;
        ASSERT_EQ(TIFFReadScanline(reader.get(), row.data(), r, s), 1);
        EXPECT_EQ(std::memcmp(row.data(), planes.data() + off, kWidth), 0);
      }
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // Tiled contiguous and separate planar round-trips.
  for (uint16_t planar : {PLANARCONFIG_CONTIG, PLANARCONFIG_SEPARATE}) {
    SCOPED_TRACE(planar);
    constexpr uint32_t kWidth = 32;
    constexpr uint32_t kHeight = 32;
    constexpr uint32_t kTileW = 16;
    constexpr uint32_t kTileH = 16;
    constexpr uint16_t kSamples = 3;

    MemoryTiffStream stream;
    tmsize_t tile_size = 0;
    uint32_t num_tiles = 0;
    std::vector<uint8_t> tile_data;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureTiledLzma(writer.get(), kWidth, kHeight, kTileW, kTileH, 8,
                         kSamples, planar);
      tile_size = TIFFTileSize(writer.get());
      num_tiles = TIFFNumberOfTiles(writer.get());
      ASSERT_GT(tile_size, 0);
      ASSERT_EQ(num_tiles, planar == PLANARCONFIG_CONTIG ? 4U : 12U);
      tile_data = MakeDeterministicBytes(
          static_cast<size_t>(tile_size) * num_tiles, 0x776655U + planar);
      for (uint32_t t = 0; t < num_tiles; ++t) {
        std::vector<uint8_t> t_copy(tile_data.begin() + t * tile_size,
                                    tile_data.begin() + (t + 1) * tile_size);
        ASSERT_EQ(
            TIFFWriteEncodedTile(writer.get(), t, t_copy.data(), tile_size),
            tile_size);
      }
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    for (uint32_t t = 0; t < num_tiles; ++t) {
      std::vector<uint8_t> actual(static_cast<size_t>(tile_size));
      ASSERT_EQ(TIFFReadEncodedTile(reader.get(), t, actual.data(), tile_size),
                tile_size);
      EXPECT_EQ(std::memcmp(actual.data(), tile_data.data() + t * tile_size,
                            static_cast<size_t>(tile_size)),
                0);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifLzmaTest, EndiannessBigTiffMmapAndDiskRgba) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 8;
  constexpr uint32_t kRowsPerStrip = 4;
  const std::vector<uint8_t> expected =
      MakeDeterministicBytes(kWidth * kHeight * sizeof(uint16_t), 0xCAFEBABEU);

  for (const char* mode : {"wl", "wb", "w8l", "w8b"}) {
    SCOPED_TRACE(mode);
    MemoryTiffStream stream;
    stream.set_enable_mmap(true);
    {
      UniqueTiff writer = stream.Open(mode);
      ASSERT_NE(writer, nullptr);
      ConfigureStripedLzma(writer.get(), kWidth, kHeight, 16, 1, kRowsPerStrip);
      const tmsize_t row_bytes = kWidth * sizeof(uint16_t);
      for (uint32_t r = 0; r < kHeight; ++r) {
        std::vector<uint8_t> row_copy(expected.begin() + r * row_bytes,
                                      expected.begin() + (r + 1) * row_bytes);
        ASSERT_EQ(TIFFWriteScanline(writer.get(), row_copy.data(), r, 0), 1);
      }
    }

    UniqueTiff reader = stream.Open("rM");
    ASSERT_NE(reader, nullptr);
    EXPECT_GT(stream.map_calls(), 0U);
    const tmsize_t row_bytes = kWidth * sizeof(uint16_t);
    std::vector<uint8_t> actual(static_cast<size_t>(row_bytes));
    for (uint32_t r = 0; r < kHeight; ++r) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), actual.data(), r, 0), 1);
      EXPECT_EQ(std::memcmp(actual.data(), expected.data() + r * row_bytes,
                            static_cast<size_t>(row_bytes)),
                0);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // Disk file write + read + RGBA image decode.
  TempFile disk_file("lzma_rgb.tif");
  DiagnosticRecorder diag;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diag.Install(opts.get());

  constexpr uint32_t kRgbaW = 8;
  constexpr uint32_t kRgbaH = 4;
  std::vector<uint8_t> rgb(kRgbaW * kRgbaH * 3);
  for (uint32_t y = 0; y < kRgbaH; ++y) {
    for (uint32_t x = 0; x < kRgbaW; ++x) {
      const size_t idx = (y * kRgbaW + x) * 3;
      rgb[idx + 0] = static_cast<uint8_t>(x * 30U);
      rgb[idx + 1] = static_cast<uint8_t>(y * 60U);
      rgb[idx + 2] = static_cast<uint8_t>(255U - x * 20U);
    }
  }
  {
    UniqueTiff writer(TIFFOpenExt(disk_file.c_str(), "wl", opts.get()));
    ASSERT_NE(writer, nullptr);
    ConfigureStripedLzma(writer.get(), kRgbaW, kRgbaH, 8, 3, kRgbaH);
    std::vector<uint8_t> copy = rgb;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, copy.data(),
                                    static_cast<tmsize_t>(copy.size())),
              static_cast<tmsize_t>(copy.size()));
  }
  {
    UniqueTiff reader(TIFFOpenExt(disk_file.c_str(), "r", opts.get()));
    ASSERT_NE(reader, nullptr);
    ScopedRgbaImage img;
    char emsg[1024] = {};
    ASSERT_TRUE(img.Begin(reader.get(), /*stop_on_error=*/1, emsg)) << emsg;
    std::vector<uint32_t> raster(kRgbaW * kRgbaH);
    ASSERT_EQ(img.Get(raster.data(), kRgbaW, kRgbaH), 1);
    for (uint32_t y = 0; y < kRgbaH; ++y) {
      for (uint32_t x = 0; x < kRgbaW; ++x) {
        const uint32_t px = raster[(kRgbaH - 1U - y) * kRgbaW + x];
        const size_t idx = (y * kRgbaW + x) * 3;
        EXPECT_EQ(TIFFGetR(px), rgb[idx + 0]);
        EXPECT_EQ(TIFFGetG(px), rgb[idx + 1]);
        EXPECT_EQ(TIFFGetB(px), rgb[idx + 2]);
        EXPECT_EQ(TIFFGetA(px), 255U);
      }
    }
  }
  EXPECT_TRUE(diag.errors().empty());
}

TEST(TifLzmaTest, MultiChunkOutputFlushInEncodeAndPostEncode) {
  // Use a tiny 16-byte output buffer plus preset 0 and multi-row strip data so
  // LZMAEncode hits sp->stream.avail_out == 0 repeatedly (as stream/block
  // headers and compressed chunks exceed 16 bytes) and LZMAPostEncode also
  // flushes across multiple chunks.
  constexpr uint32_t kWidth = 64;
  constexpr uint32_t kHeight = 256;
  const std::vector<uint8_t> pixels =
      MakeDeterministicBytes(kWidth * kHeight, 0xD00DFEEDU);

  MemoryTiffStream stream;
  std::vector<uint8_t> small_out_buf(16);
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedLzma(writer.get(), kWidth, kHeight, 8, 1, kHeight);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_LZMAPRESET, 0), 1);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), small_out_buf.data(),
                                   static_cast<tmsize_t>(small_out_buf.size())),
              1);
    std::vector<uint8_t> strip_copy = pixels;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip_copy.data(),
                                    static_cast<tmsize_t>(strip_copy.size())),
              static_cast<tmsize_t>(strip_copy.size()));
    EXPECT_GT(stream.write_calls(), 3U);
    // Switch back to a library-owned buffer before TIFFFlush resets td_*.
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, -1), 1);
    ASSERT_EQ(TIFFFlush(writer.get()), 1);
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> decoded(pixels.size());
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                 static_cast<tmsize_t>(decoded.size())),
            static_cast<tmsize_t>(pixels.size()));
  EXPECT_EQ(decoded, pixels);
  EXPECT_TRUE(stream.diagnostics().errors().empty())
      << FormatDiagnostics(stream.diagnostics().errors());
}

TEST(TifLzmaTest, ReadWriteUpdateModeSwitching) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 8;
  constexpr uint32_t kRowsPerStrip = 4;
  const std::vector<uint8_t> orig0 =
      MakeDeterministicBytes(kWidth * kRowsPerStrip, 0x11111111U);
  const std::vector<uint8_t> orig1 =
      MakeDeterministicBytes(kWidth * kRowsPerStrip, 0x22222222U);
  const std::vector<uint8_t> updated0 =
      MakeDeterministicBytes(kWidth * kRowsPerStrip, 0x33333333U);

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedLzma(writer.get(), kWidth, kHeight, 8, 1, kRowsPerStrip);
    std::vector<uint8_t> c0 = orig0;
    std::vector<uint8_t> c1 = orig1;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, c0.data(),
                                    static_cast<tmsize_t>(c0.size())),
              static_cast<tmsize_t>(c0.size()));
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, c1.data(),
                                    static_cast<tmsize_t>(c1.size())),
              static_cast<tmsize_t>(c1.size()));
  }

  // Open in r+ mode: decode strip 1 -> encode strip 0 -> decode strip 0.
  {
    UniqueTiff rw = stream.Open("r+");
    ASSERT_NE(rw, nullptr);

    std::vector<uint8_t> buf(orig1.size());
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 1, buf.data(),
                                   static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(orig1.size()));
    EXPECT_EQ(buf, orig1);

    // Encode after decode: triggers LZMASetupEncode's LSTATE_INIT_DECODE reset.
    std::vector<uint8_t> wbuf = updated0;
    ASSERT_EQ(TIFFWriteEncodedStrip(rw.get(), 0, wbuf.data(),
                                    static_cast<tmsize_t>(wbuf.size())),
              static_cast<tmsize_t>(updated0.size()));

    // Decode after encode: triggers LZMASetupDecode's LSTATE_INIT_ENCODE reset.
    std::fill(buf.begin(), buf.end(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, buf.data(),
                                   static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(updated0.size()));
    EXPECT_EQ(buf, updated0);
  }

  // Reopen cleanly to verify persisted directory and strips.
  UniqueTiff verify = stream.Open("r");
  ASSERT_NE(verify, nullptr);
  std::vector<uint8_t> out0(updated0.size());
  std::vector<uint8_t> out1(orig1.size());
  ASSERT_EQ(TIFFReadEncodedStrip(verify.get(), 0, out0.data(),
                                 static_cast<tmsize_t>(out0.size())),
            static_cast<tmsize_t>(updated0.size()));
  ASSERT_EQ(TIFFReadEncodedStrip(verify.get(), 1, out1.data(),
                                 static_cast<tmsize_t>(out1.size())),
            static_cast<tmsize_t>(orig1.size()));
  EXPECT_EQ(out0, updated0);
  EXPECT_EQ(out1, orig1);
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifLzmaTest, RandomScanlineSeekBehavior) {
  constexpr uint32_t kWidth = 32;
  constexpr uint32_t kHeight = 8;
  constexpr uint32_t kRowsPerStrip = 4;
  const std::vector<uint8_t> pixels =
      MakeDeterministicBytes(kWidth * kHeight, 0x44556677U);

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedLzma(writer.get(), kWidth, kHeight, 8, 1, kRowsPerStrip);
    for (uint32_t r = 0; r < kHeight; ++r) {
      std::vector<uint8_t> copy(pixels.begin() + r * kWidth,
                                pixels.begin() + (r + 1) * kWidth);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), copy.data(), r, 0), 1);
    }
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> row(kWidth);

  auto check_row = [&](uint32_t r) {
    ASSERT_EQ(TIFFReadScanline(reader.get(), row.data(), r, 0), 1);
    EXPECT_EQ(std::memcmp(row.data(), pixels.data() + r * kWidth, kWidth), 0);
  };

  // Sequential into strip 0.
  check_row(0);
  check_row(1);
  // Seek backwards to start of strip 0 (re-runs LZMAPreDecode).
  check_row(0);
  // Jump to strip 1 start, then forward within strip 1.
  check_row(4);
  check_row(5);
  // Jump back to strip 0 start.
  check_row(0);
  EXPECT_TRUE(stream.diagnostics().errors().empty());

  // Forward skip within the same strip fails because LZMA uses _TIFFNoSeek.
  EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 2, 0), -1);
  EXPECT_FALSE(stream.diagnostics().errors().empty());
}

TEST(TifLzmaTest, CorruptTruncatedAndShortXzDecodeErrors) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 8;
  constexpr uint32_t kRowsPerStrip = 4;
  const std::vector<uint8_t> strip1_pixels =
      MakeDeterministicBytes(kWidth * kRowsPerStrip, 0x0F1E2D3CU);
  const std::vector<uint8_t> valid_strip = ExtractValidLzmaStrip(
      kWidth, kRowsPerStrip, kRowsPerStrip, strip1_pixels);
  ASSERT_GT(valid_strip.size(), 24U);

  // Also encode a shorter 16-byte valid .xz strip (smaller than 64-byte strip).
  const std::vector<uint8_t> short_pixels =
      MakeDeterministicBytes(kWidth, 0xAABBCCDDU);
  const std::vector<uint8_t> valid_short_xz =
      ExtractValidLzmaStrip(kWidth, 1, 1, short_pixels);
  ASSERT_GT(valid_short_xz.size(), 16U);

  // 1. LZMA_FORMAT_ERROR + sticky read_error + recovery on strip 1.
  {
    const std::array<uint8_t, 12> non_xz = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                            0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
    MemoryTiffStream stream(BuildTwoStripRawLzmaTiff(
        kWidth, kHeight, kRowsPerStrip, non_xz, valid_strip));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> row(kWidth, 0xAAU);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "LZMADecode",
                                        "file format not recognized"));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "LZMADecode", "Not enough data"));

    // Next scanline in same failing strip hits sticky sp->read_error and zeroes
    // output buffer.
    std::fill(row.begin(), row.end(), 0xCCU);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 1, 0), -1);
    EXPECT_TRUE(
        std::all_of(row.begin(), row.end(), [](uint8_t b) { return b == 0; }));
    EXPECT_TRUE(
        HasDiagnosticContaining(stream.diagnostics().errors(), "LZMADecode",
                                "cannot be read due to previous error"));

    // Moving to strip 1 runs LZMAPreDecode (resets read_error = 0) and decodes
    // valid data.
    ASSERT_EQ(TIFFReadScanline(reader.get(), row.data(), 4, 0), 1);
    EXPECT_EQ(std::memcmp(row.data(), strip1_pixels.data(), kWidth), 0);
  }

  // 2. LZMA_BUF_ERROR: truncated valid .xz stream (stream header + partial
  //    block without stream footer).
  {
    std::vector<uint8_t> truncated(valid_strip.begin(),
                                   valid_strip.begin() + 14);
    MemoryTiffStream stream(BuildTwoStripRawLzmaTiff(
        kWidth, kHeight, kRowsPerStrip, truncated, valid_strip));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> buf(strip1_pixels.size(), 0x55U);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, buf.data(),
                                   static_cast<tmsize_t>(buf.size())),
              tmsize_t(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "LZMADecode", "truncated or corrupt"));
  }

  // 3. LZMA_DATA_ERROR: valid magic with corrupted header CRC32.
  {
    std::vector<uint8_t> corrupt = valid_strip;
    corrupt[8] ^= 0xFFU;
    MemoryTiffStream stream(BuildTwoStripRawLzmaTiff(
        kWidth, kHeight, kRowsPerStrip, corrupt, valid_strip));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> buf(strip1_pixels.size());
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, buf.data(),
                                   static_cast<tmsize_t>(buf.size())),
              tmsize_t(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "LZMADecode", "data is corrupt"));
  }

  // 4. LZMA_OPTIONS_ERROR: valid .xz magic + reserved flag bit + matching
  //    IEEE CRC32.
  {
    std::array<uint8_t, 12> bad_flags_xz = {
        0xFD, '7',  'z',  'X',  'Z', 0x00,  // .xz stream header magic
        0x01, 0x00,                         // Reserved non-zero stream flags
        0x00, 0x00, 0x00, 0x00,             // CRC32 placeholder
    };
    const uint32_t crc =
        Crc32Ieee(absl::Span<const uint8_t>(bad_flags_xz.data() + 6, 2));
    bad_flags_xz[8] = static_cast<uint8_t>(crc & 0xFFU);
    bad_flags_xz[9] = static_cast<uint8_t>((crc >> 8) & 0xFFU);
    bad_flags_xz[10] = static_cast<uint8_t>((crc >> 16) & 0xFFU);
    bad_flags_xz[11] = static_cast<uint8_t>((crc >> 24) & 0xFFU);

    MemoryTiffStream stream(BuildTwoStripRawLzmaTiff(
        kWidth, kHeight, kRowsPerStrip, bad_flags_xz, valid_strip));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> buf(strip1_pixels.size());
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, buf.data(),
                                   static_cast<tmsize_t>(buf.size())),
              tmsize_t(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "LZMADecode",
                                        "invalid or unsupported options"));
  }

  // 5. LZMA_STREAM_END early termination when decoded .xz stream has fewer
  //    bytes than expected strip size.
  {
    MemoryTiffStream stream(BuildTwoStripRawLzmaTiff(
        kWidth, kHeight, kRowsPerStrip, valid_short_xz, valid_strip));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> buf(strip1_pixels.size(), 0x7EU);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, buf.data(),
                                   static_cast<tmsize_t>(buf.size())),
              tmsize_t(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "LZMADecode", "Not enough data"));
    // Tail bytes past decoded prefix must be zero-filled.
    EXPECT_TRUE(std::all_of(buf.begin() + short_pixels.size(), buf.end(),
                            [](uint8_t b) { return b == 0; }));
  }
}

TEST(TifLzmaTest, EncodeErrorsAndWriteFaultInjection) {
  // 1. opt_delta.dist > LZMA_DELTA_DIST_MAX (256) when BITSPERSAMPLE == 2056:
  //    triggers LZMA_OPTIONS_ERROR in LZMAPreEncode, then again in
  //    LZMAVSetField while sp->state & LSTATE_INIT_ENCODE, and LZMA_PROG_ERROR
  //    if LZMAEncode is subsequently entered without valid encoder init.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedLzma(writer.get(), 2, 2, 2056, 1, 2);
      std::vector<uint8_t> row(
          static_cast<size_t>(TIFFScanlineSize(writer.get())), 0x11U);
      EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), -1);
      EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                          "LZMAPreEncode",
                                          "invalid or unsupported options"));

      // Re-configuring preset while LSTATE_INIT_ENCODE is set hits
      // LZMAVSetField's error path as well.
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_LZMAPRESET, 3), 1);
      EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                          "LZMAVSetField",
                                          "invalid or unsupported options"));

      // Re-invoking row 0 directly calls LZMAEncode on uninitialized stream;
      // TIFFWriteScanline forwards tif_encoderow's 0 return value.
      EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 0);
      EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                          "LZMAEncode", "programming error"));
    }
  }

  // 2. Calling TIFFFlushData mid-strip finishes the .xz stream; subsequent
  //    TIFFWriteScanline in the same strip hits LZMA_STREAM_END in LZMAEncode.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedLzma(writer.get(), 16, 4, 8, 1, 4);
      std::vector<uint8_t> row0(16, 0x2AU);
      std::vector<uint8_t> row1(16, 0x3BU);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row0.data(), 0, 0), 1);
      ASSERT_EQ(TIFFFlushData(writer.get()), 1);
      EXPECT_EQ(TIFFWriteScanline(writer.get(), row1.data(), 1, 0), 0);
      EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                          "LZMAEncode",
                                          "end of stream was reached"));
    }
  }

  // 3. Write I/O failure inside LZMAEncode's output flush (16-byte buffer
  //    overflows during stream/block header emission in LZMAEncode).
  {
    MemoryTiffStream stream;
    std::vector<uint8_t> small_out_buf(16);
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedLzma(writer.get(), 64, 128, 8, 1, 128);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_LZMAPRESET, 0), 1);
      ASSERT_EQ(
          TIFFWriteBufferSetup(writer.get(), small_out_buf.data(),
                               static_cast<tmsize_t>(small_out_buf.size())),
          1);
      std::vector<uint8_t> strip =
          MakeDeterministicBytes(64 * 128, 0x55AA1234U);
      // Allow header write during open, fail on the very first compressed
      // strip chunk write triggered inside LZMAEncode.
      stream.FailWritesAfter(stream.write_calls());
      EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                      static_cast<tmsize_t>(strip.size())),
                tmsize_t(-1));
      EXPECT_FALSE(stream.diagnostics().errors().empty());
      stream.ResetFaults();
      ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, -1), 1);
    }
  }

  // 4. Write I/O failure inside LZMAPostEncode's final TIFFFlushData1 call.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedLzma(writer.get(), 16, 4, 8, 1, 4);
      std::vector<uint8_t> strip = MakeDeterministicBytes(16 * 4, 0x99887766U);
      // Strip fits in default 8 KiB raw buffer, so WriteProc is only called
      // during LZMAPostEncode -> TIFFFlushData1.
      stream.FailWritesAfter(stream.write_calls());
      EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                      static_cast<tmsize_t>(strip.size())),
                tmsize_t(-1));
      EXPECT_FALSE(stream.diagnostics().errors().empty());
      stream.ResetFaults();
    }
  }
}

TEST(TifLzmaTest, InitLzmaStateAllocationOomFailsCleanly) {
  DiagnosticRecorder diag;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  diag.Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), kDefaultAllocBudget);

  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("wl", opts.get());
  ASSERT_NE(tif, nullptr);

  // Leave enough budget for the small _TIFFMergeFields pointer array growth,
  // but starve sizeof(LZMAState) (> 300 bytes) in TIFFInitLZMA.
  {
    ScopedCumulatedMemAllocRemaining oom(tif.get(), 64);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZMA),
              0);
  }
  EXPECT_TRUE(HasDiagnosticContaining(diag.errors(), "TIFFInitLZMA",
                                      "No space for LZMA2 state block"));
  EXPECT_TRUE(HasAnyMessageContaining(diag.errors(), "LZMA2 state block"));
}

// Prefix matching the leading fields of LZMAState in
// third_party/tiff/libtiff/tif_lzma.c. Coupled directly to upstream internal
// struct layout because no public hook exists for allocator failure injection;
// verify field ordering against LZMAState on Copybara upstream upgrades.
struct LzmaStatePrefix {
  TIFFPredictorState predict;
  int read_error;
  lzma_stream stream;
};

void* FailingLzmaAlloc(void* /*opaque*/, size_t /*nmemb*/, size_t /*size*/) {
  return nullptr;
}

void StdFreeLzma(void* /*opaque*/, void* ptr) { std::free(ptr); }

TEST(TifLzmaTest, PreDecodeStreamDecoderOomViaCustomAllocator) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 4;
  const std::vector<uint8_t> pixels =
      MakeDeterministicBytes(kWidth * kHeight, 0x13572468U);
  const std::vector<uint8_t> strip0 =
      ExtractValidLzmaStrip(kWidth, kHeight, kHeight, pixels);
  ASSERT_FALSE(strip0.empty());

  const std::vector<uint8_t> raw_tiff =
      BuildTwoStripRawLzmaTiff(kWidth, kHeight * 2, kHeight, strip0, strip0);

  // Declare custom allocator before UniqueTiff so it outlives LZMACleanup().
  lzma_allocator fail_alloc = {};
  fail_alloc.alloc = &FailingLzmaAlloc;
  fail_alloc.free = &StdFreeLzma;

  MemoryTiffStream stream(raw_tiff);
  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  auto* sp =
      static_cast<LzmaStatePrefix*>(static_cast<void*>(reader->tif_data));
  ASSERT_NE(sp, nullptr);
  sp->stream.allocator = &fail_alloc;

  std::vector<uint8_t> decoded(kWidth * kHeight, 0xAAU);
  EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                 static_cast<tmsize_t>(decoded.size())),
            tmsize_t(-1));
  EXPECT_TRUE(HasDiagnosticContaining(
      stream.diagnostics().errors(), "LZMAPreDecode", "cannot allocate memory"))
      << FormatDiagnostics(stream.diagnostics().errors());
}

TEST(TifLzmaTest, DecodeMemlimitErrorRecovery) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 4;
  const std::vector<uint8_t> pixels =
      MakeDeterministicBytes(kWidth * kHeight, 0x24681357U);

  MemoryTiffStream write_stream;
  {
    UniqueTiff writer = write_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedLzma(writer.get(), kWidth, kHeight, 8, 1, kHeight);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_LZMAPRESET, 6), 1);
    std::vector<uint8_t> copy = pixels;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, copy.data(),
                                    static_cast<tmsize_t>(copy.size())),
              static_cast<tmsize_t>(copy.size()));
  }

  // Extract strip 0 and keep it alive longer than UniqueTiff reader.
  std::vector<uint8_t> raw_strip;
  {
    UniqueTiff raw_reader = write_stream.Open("r");
    ASSERT_NE(raw_reader, nullptr);
    const uint64_t* bytecounts = nullptr;
    ASSERT_EQ(
        TIFFGetField(raw_reader.get(), TIFFTAG_STRIPBYTECOUNTS, &bytecounts),
        1);
    ASSERT_NE(bytecounts, nullptr);
    raw_strip.resize(static_cast<size_t>(bytecounts[0]));
    ASSERT_EQ(TIFFReadRawStrip(raw_reader.get(), 0, raw_strip.data(),
                               static_cast<tmsize_t>(raw_strip.size())),
              static_cast<tmsize_t>(raw_strip.size()));
  }

  MemoryTiffStream stream(write_stream.data());
  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  ASSERT_EQ(TIFFReadBufferSetup(reader.get(), raw_strip.data(),
                                static_cast<tmsize_t>(raw_strip.size())),
            1);
  reader->tif_rawcp = reader->tif_rawdata;
  reader->tif_rawcc = static_cast<tmsize_t>(raw_strip.size());
  ASSERT_EQ(reader->tif_predecode(reader.get(), 0), 1);

  auto* sp =
      static_cast<LzmaStatePrefix*>(static_cast<void*>(reader->tif_data));
  ASSERT_NE(sp, nullptr);
  // Lower memlimit to current base usage before block 1 header is parsed so
  // LZMADecode receives LZMA_MEMLIMIT_ERROR, raises the limit, and recovers.
  ASSERT_EQ(lzma_memlimit_set(&sp->stream, lzma_memusage(&sp->stream)),
            LZMA_OK);

  std::vector<uint8_t> decoded(pixels.size(), 0);
  ASSERT_EQ(reader->tif_decodestrip(reader.get(), decoded.data(),
                                    static_cast<tmsize_t>(decoded.size()), 0),
            1);
  EXPECT_EQ(decoded, pixels);
  EXPECT_TRUE(stream.diagnostics().errors().empty())
      << FormatDiagnostics(stream.diagnostics().errors());
}

TEST(TifLzmaTest, PostEncodeErrorOnEndedStream) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 4;
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedLzma(writer.get(), kWidth, kHeight, 8, 1, kHeight);
    std::vector<uint8_t> row0(kWidth, 0x5AU);
    ASSERT_EQ(TIFFWriteScanline(writer.get(), row0.data(), 0, 0), 1);

    auto* sp =
        static_cast<LzmaStatePrefix*>(static_cast<void*>(writer->tif_data));
    ASSERT_NE(sp, nullptr);
    lzma_end(&sp->stream);

    EXPECT_EQ(TIFFFlushData(writer.get()), 0);
  }
  EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                      "LZMAPostEncode", "programming error"))
      << FormatDiagnostics(stream.diagnostics().errors());
}

}  // namespace
}  // namespace libtiff
