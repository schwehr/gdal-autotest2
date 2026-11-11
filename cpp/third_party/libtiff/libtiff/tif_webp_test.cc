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

// Clean-room unit tests for libtiff/tif_webp.c (COMPRESSION_WEBP codec).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_webp.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"
#include "third_party/libwebp/webp/decode.h"
#include "third_party/libwebp/webp/encode.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

constexpr uint32_t kDefaultAllocBudget = 1U << 24;

std::vector<uint8_t> MakeSmoothRgbPixels(uint32_t width, uint32_t height,
                                         uint32_t seed = 0) {
  std::vector<uint8_t> out(static_cast<size_t>(width) * height * 3);
  const uint32_t max_w = std::max(width, 1U);
  const uint32_t max_h = std::max(height, 1U);
  const uint32_t max_sum = std::max(width + height, 1U);
  const uint32_t offset = (seed * 17U) % 45U;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t idx = (static_cast<size_t>(y) * width + x) * 3;
      out[idx + 0] = static_cast<uint8_t>(20U + offset + (x * 160U) / max_w);
      out[idx + 1] = static_cast<uint8_t>(30U + offset + (y * 160U) / max_h);
      out[idx + 2] =
          static_cast<uint8_t>(40U + offset + ((x + y) * 140U) / max_sum);
    }
  }
  return out;
}

std::vector<uint8_t> MakeTexturedRgbPixels(uint32_t width, uint32_t height,
                                           uint32_t seed = 0x13579bdfU) {
  std::vector<uint8_t> out(static_cast<size_t>(width) * height * 3);
  uint32_t state = seed ? seed : 1;
  for (size_t i = 0; i < out.size(); ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    out[i] = static_cast<uint8_t>(state & 0xffU);
  }
  return out;
}

std::vector<uint8_t> MakeRgbaPixels(uint32_t width, uint32_t height,
                                    bool opaque_only = false,
                                    bool include_zero_alpha_rgb = false) {
  std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
  const uint32_t max_w = std::max(width, 1U);
  const uint32_t max_h = std::max(height, 1U);
  const uint32_t max_sum = std::max(width + height, 1U);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
      if (include_zero_alpha_rgb && y < height / 2) {
        out[idx + 0] = 123;
        out[idx + 1] = 45;
        out[idx + 2] = 67;
        out[idx + 3] = 0;
      } else if (opaque_only) {
        out[idx + 0] = static_cast<uint8_t>(20U + (x * 180U) / max_w);
        out[idx + 1] = static_cast<uint8_t>(25U + (y * 180U) / max_h);
        out[idx + 2] = static_cast<uint8_t>(35U + ((x + y) * 160U) / max_sum);
        out[idx + 3] = 255;
      } else {
        // Keep alpha >= max(R, G, B) so associated-alpha checks stay valid.
        const uint8_t alpha =
            static_cast<uint8_t>(170U + ((x + y) * 70U) / max_sum);
        out[idx + 0] = static_cast<uint8_t>(16U + (x * 120U) / max_w);
        out[idx + 1] = static_cast<uint8_t>(24U + (y * 120U) / max_h);
        out[idx + 2] = static_cast<uint8_t>(32U + ((x + y) * 100U) / max_sum);
        out[idx + 3] = alpha;
      }
    }
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

void ConfigureStripedWebp(TIFF* tif, uint32_t width, uint32_t height,
                          uint16_t samples_per_pixel, uint32_t rows_per_strip,
                          uint16_t extra_sample = EXTRASAMPLE_UNASSALPHA) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
  if (samples_per_pixel == 4) {
    ASSERT_EQ(
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, uint16_t{1}, &extra_sample), 1);
  }
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_WEBP), 1);
}

void ConfigureTiledWebp(TIFF* tif, uint32_t width, uint32_t height,
                        uint32_t tile_width, uint32_t tile_height,
                        uint16_t samples_per_pixel,
                        uint16_t extra_sample = EXTRASAMPLE_UNASSALPHA) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile_width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILELENGTH, tile_height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
  if (samples_per_pixel == 4) {
    ASSERT_EQ(
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, uint16_t{1}, &extra_sample), 1);
  }
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_WEBP), 1);
}

std::vector<uint8_t> ExtractRawWebpStrip(uint32_t width, uint32_t height,
                                         uint16_t samples_per_pixel,
                                         absl::Span<const uint8_t> pixels,
                                         bool lossless = true) {
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    EXPECT_NE(writer, nullptr);
    if (writer == nullptr) return {};
    ConfigureStripedWebp(writer.get(), width, height, samples_per_pixel,
                         height);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, lossless ? 1 : 0), 1);
    std::vector<uint8_t> scratch(pixels.begin(), pixels.end());
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, scratch.data(),
                                    static_cast<tmsize_t>(scratch.size())),
              static_cast<tmsize_t>(scratch.size()));
  }
  UniqueTiff reader = stream.Open("r");
  EXPECT_NE(reader, nullptr);
  if (reader == nullptr) return {};
  const tmsize_t raw_size = TIFFRawStripSize(reader.get(), 0);
  EXPECT_GT(raw_size, 0);
  if (raw_size <= 0) return {};
  std::vector<uint8_t> blob(static_cast<size_t>(raw_size));
  EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, blob.data(), raw_size), raw_size);
  return blob;
}

std::vector<uint8_t> BuildRawWebpSingleStripTiff(
    bool big_endian, bool big_tiff, uint32_t width, uint32_t height,
    uint16_t samples_per_pixel, uint16_t bits_per_sample,
    uint16_t sample_format, uint16_t planar_config, uint32_t rows_per_strip,
    absl::Span<const uint8_t> blob) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const uint64_t offset = builder.AppendBlob(blob);
  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShortArray(
          TIFFTAG_BITSPERSAMPLE,
          std::vector<uint16_t>(samples_per_pixel, bits_per_sample)),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_WEBP),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
      builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(offset)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                       static_cast<uint32_t>(blob.size())),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, planar_config),
      builder.MakeShort(TIFFTAG_SAMPLEFORMAT, sample_format),
  };
  if (samples_per_pixel == 4) {
    entries.push_back(
        builder.MakeShort(TIFFTAG_EXTRASAMPLES, EXTRASAMPLE_UNASSALPHA));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint8_t> BuildRawWebpSingleTileTiff(
    uint32_t width, uint32_t height, uint32_t tile_width, uint32_t tile_height,
    uint16_t samples_per_pixel, absl::Span<const uint8_t> blob) {
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  const uint64_t offset = builder.AppendBlob(blob);
  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE,
                             std::vector<uint16_t>(samples_per_pixel, 8)),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_WEBP),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
      builder.MakeLong(TIFFTAG_TILEWIDTH, tile_width),
      builder.MakeLong(TIFFTAG_TILELENGTH, tile_height),
      builder.MakeLong(TIFFTAG_TILEOFFSETS, static_cast<uint32_t>(offset)),
      builder.MakeLong(TIFFTAG_TILEBYTECOUNTS,
                       static_cast<uint32_t>(blob.size())),
  };
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

TEST(TifWebpTest, CodecConfiguredAndRegistered) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_WEBP), 1);
  const TIFFCodec* codec = TIFFFindCODEC(COMPRESSION_WEBP);
  ASSERT_NE(codec, nullptr);
  EXPECT_EQ(codec->scheme, COMPRESSION_WEBP);
  EXPECT_STREQ(codec->name, "WEBP");
  EXPECT_NE(codec->init, nullptr);

  UniqueTiffBuffer<TIFFCodec> configured(TIFFGetConfiguredCODECs());
  ASSERT_NE(configured, nullptr);
  bool found = false;
  for (const TIFFCodec* c = configured.get(); c->name != nullptr; ++c) {
    if (c->scheme == COMPRESSION_WEBP) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST(TifWebpTest, WebpTagsDefaultsSetGetDelegationAndCompressionRatio) {
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 16, 4, 3, 4);

    // Defaults: level=75, lossless=0, lossless_exact=1.
    int level = -1;
    int lossless = -1;
    int lossless_exact = -1;
    ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_WEBP_LEVEL, &level), 1);
    ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, &lossless), 1);
    ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_WEBP_LOSSLESS_EXACT,
                           &lossless_exact),
              1);
    EXPECT_EQ(level, 75);
    EXPECT_EQ(lossless, 0);
    EXPECT_EQ(lossless_exact, 1);

    // Compression ratio estimates depend on samplesperpixel (3 vs 4).
    EXPECT_EQ(TIFFGetMaxCompressionRatio(writer.get()), 78146ULL);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{4}),
              1);
    EXPECT_EQ(TIFFGetMaxCompressionRatio(writer.get()), 104194ULL);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
              1);

    // Parent delegation through TWebPVSetField / TWebPVGetField.
    uint32_t width = 0;
    ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
    EXPECT_EQ(width, 16U);

    // Valid WEBP_LEVEL values.
    for (int valid_level : {1, 50, 100}) {
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LEVEL, valid_level), 1);
      int got = -1;
      ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_WEBP_LEVEL, &got), 1);
      EXPECT_EQ(got, valid_level);
    }

    // Out-of-range WEBP_LEVEL emits a warning via TWebPVSetField but returns 1.
    for (int bad_level : {0, -5, 101}) {
      stream.diagnostics().Clear();
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LEVEL, bad_level), 1);
      EXPECT_TRUE(HasDiagnosticContaining(
          stream.diagnostics().warnings(), "WebPVSetField",
          "WEBP_LEVEL should be between 1 and 100"))
          << "bad_level=" << bad_level;
      int got = -999;
      ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_WEBP_LEVEL, &got), 1);
      EXPECT_EQ(got, bad_level);
    }

    // Setting TIFFTAG_WEBP_LOSSLESS to non-zero forces quality_level = 100.
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LEVEL, 42), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);
    int got_level = -1;
    int got_lossless = -1;
    ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_WEBP_LEVEL, &got_level), 1);
    ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, &got_lossless),
              1);
    EXPECT_EQ(got_lossless, 1);
    EXPECT_EQ(got_level, 100);

    // TIFFTAG_WEBP_LOSSLESS_EXACT toggles cleanly.
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS_EXACT, 0), 1);
    int got_exact = -1;
    ASSERT_EQ(
        TIFFGetField(writer.get(), TIFFTAG_WEBP_LOSSLESS_EXACT, &got_exact), 1);
    EXPECT_EQ(got_exact, 0);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS_EXACT, 1), 1);
    ASSERT_EQ(
        TIFFGetField(writer.get(), TIFFTAG_WEBP_LOSSLESS_EXACT, &got_exact), 1);
    EXPECT_EQ(got_exact, 1);

    // Finish with a valid lossless strip so reader succeeds cleanly.
    const std::vector<uint8_t> pixels = MakeSmoothRgbPixels(16, 4);
    std::vector<uint8_t> scratch = pixels;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, scratch.data(),
                                    static_cast<tmsize_t>(scratch.size())),
              static_cast<tmsize_t>(scratch.size()));
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> decoded(16 * 4 * 3, 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                 static_cast<tmsize_t>(decoded.size())),
            static_cast<tmsize_t>(decoded.size()));
  EXPECT_EQ(decoded, MakeSmoothRgbPixels(16, 4));
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifWebpTest, StripedLosslessExactAndTransparentRgbPreservation) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 8;
  const std::vector<uint8_t> expected =
      MakeRgbaPixels(kWidth, kHeight, /*opaque_only=*/false,
                     /*include_zero_alpha_rgb=*/true);

  // 1) Default lossless_exact = 1 preserves RGB in alpha=0 pixels bit-for-bit.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedWebp(writer.get(), kWidth, kHeight, 4, kHeight,
                           EXTRASAMPLE_UNASSALPHA);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS_EXACT, 1), 1);
      std::vector<uint8_t> scratch = expected;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, scratch.data(),
                                      static_cast<tmsize_t>(scratch.size())),
                static_cast<tmsize_t>(scratch.size()));
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> decoded(expected.size(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                   static_cast<tmsize_t>(decoded.size())),
              static_cast<tmsize_t>(decoded.size()));
    EXPECT_EQ(decoded, expected);
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // 2) lossless_exact = 0 drops RGB values where alpha == 0 while keeping
  // opaque pixels exact.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedWebp(writer.get(), kWidth, kHeight, 4, kHeight,
                           EXTRASAMPLE_UNASSALPHA);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS_EXACT, 0), 1);
      std::vector<uint8_t> scratch = expected;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, scratch.data(),
                                      static_cast<tmsize_t>(scratch.size())),
                static_cast<tmsize_t>(scratch.size()));
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> decoded(expected.size(), 0xff);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                   static_cast<tmsize_t>(decoded.size())),
              static_cast<tmsize_t>(decoded.size()));
    const size_t half_bytes = expected.size() / 2;
    // Top half has alpha == 0: RGB is cleared to 0 by WebP exact=0.
    for (size_t i = 0; i < half_bytes; i += 4) {
      EXPECT_EQ(decoded[i + 0], 0);
      EXPECT_EQ(decoded[i + 1], 0);
      EXPECT_EQ(decoded[i + 2], 0);
      EXPECT_EQ(decoded[i + 3], 0);
    }
    // Bottom half has non-zero alpha: bit-for-bit identical.
    EXPECT_EQ(
        std::vector<uint8_t>(decoded.begin() + half_bytes, decoded.end()),
        std::vector<uint8_t>(expected.begin() + half_bytes, expected.end()));
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifWebpTest, StripedRgbAndRgbaLossyLosslessEndiansAndOpaqueAlpha) {
  struct StripCase {
    std::string_view name;
    const char* write_mode;
    bool expect_swapped;
    bool expect_big_tiff;
    uint32_t width;
    uint32_t height;
    uint32_t rows_per_strip;
    uint16_t samples;
    uint16_t extra_sample;
    bool lossless;
    bool opaque_rgba;
    int level;
  };

  constexpr std::array<StripCase, 6> kCases = {{
      {.name = "ClassicLeRgbLossyMultiStripRemainder",
       .write_mode = "wl",
       .expect_swapped = false,
       .expect_big_tiff = false,
       .width = 20,
       .height = 10,
       .rows_per_strip = 4,
       .samples = 3,
       .extra_sample = EXTRASAMPLE_UNASSALPHA,
       .lossless = false,
       .opaque_rgba = false,
       .level = 90},
      {.name = "ClassicBeRgbaAssocAlphaLossless",
       .write_mode = "wb",
       .expect_swapped = true,
       .expect_big_tiff = false,
       .width = 16,
       .height = 9,
       .rows_per_strip = 4,
       .samples = 4,
       .extra_sample = EXTRASAMPLE_ASSOCALPHA,
       .lossless = true,
       .opaque_rgba = false,
       .level = 100},
      {.name = "BigTiffLeRgbaOpaqueAlphaException",
       .write_mode = "w8l",
       .expect_swapped = false,
       .expect_big_tiff = true,
       .width = 16,
       .height = 8,
       .rows_per_strip = 8,
       .samples = 4,
       .extra_sample = EXTRASAMPLE_UNASSALPHA,
       .lossless = true,
       .opaque_rgba = true,
       .level = 100},
      {.name = "BigTiffBeRgbLossless",
       .write_mode = "w8b",
       .expect_swapped = true,
       .expect_big_tiff = true,
       .width = 12,
       .height = 7,
       .rows_per_strip = 3,
       .samples = 3,
       .extra_sample = EXTRASAMPLE_UNASSALPHA,
       .lossless = true,
       .opaque_rgba = false,
       .level = 100},
      {.name = "ClassicLeRgbaLossy",
       .write_mode = "wl",
       .expect_swapped = false,
       .expect_big_tiff = false,
       .width = 24,
       .height = 12,
       .rows_per_strip = 6,
       .samples = 4,
       .extra_sample = EXTRASAMPLE_UNASSALPHA,
       .lossless = false,
       .opaque_rgba = false,
       .level = 85},
      {.name = "ClassicLeRgbaLossyOpaque",
       .write_mode = "wl",
       .expect_swapped = false,
       .expect_big_tiff = false,
       .width = 16,
       .height = 8,
       .rows_per_strip = 8,
       .samples = 4,
       .extra_sample = EXTRASAMPLE_UNASSALPHA,
       .lossless = false,
       .opaque_rgba = true,
       .level = 85},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    MemoryTiffStream stream;
    std::vector<std::vector<uint8_t>> expected_strips;
    const std::vector<uint8_t> full_image =
        tc.samples == 3
            ? MakeSmoothRgbPixels(tc.width, tc.height, 11)
            : MakeRgbaPixels(tc.width, tc.height, tc.opaque_rgba, false);

    {
      UniqueTiff writer = stream.Open(tc.write_mode);
      ASSERT_NE(writer, nullptr);
      ConfigureStripedWebp(writer.get(), tc.width, tc.height, tc.samples,
                           tc.rows_per_strip, tc.extra_sample);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LEVEL, tc.level), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS,
                             tc.lossless ? 1 : 0),
                1);

      const uint32_t num_strips = TIFFNumberOfStrips(writer.get());
      expected_strips.reserve(num_strips);
      const size_t row_bytes = static_cast<size_t>(tc.width) * tc.samples;
      for (uint32_t strip = 0; strip < num_strips; ++strip) {
        const uint32_t first_row = strip * tc.rows_per_strip;
        const uint32_t strip_rows =
            std::min(tc.rows_per_strip, tc.height - first_row);
        const tmsize_t strip_bytes = TIFFVStripSize(writer.get(), strip_rows);
        ASSERT_EQ(strip_bytes, static_cast<tmsize_t>(strip_rows * row_bytes));

        const auto begin = full_image.begin() + first_row * row_bytes;
        std::vector<uint8_t> chunk(begin, begin + strip_bytes);
        expected_strips.push_back(chunk);
        ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), strip, chunk.data(),
                                        strip_bytes),
                  strip_bytes);
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFIsByteSwapped(reader.get()), tc.expect_swapped ? 1 : 0);
    EXPECT_EQ(TIFFIsBigTIFF(reader.get()), tc.expect_big_tiff ? 1 : 0);

    const uint32_t num_strips = TIFFNumberOfStrips(reader.get());
    ASSERT_EQ(num_strips, expected_strips.size());
    for (uint32_t strip = 0; strip < num_strips; ++strip) {
      std::vector<uint8_t> actual(expected_strips[strip].size(), 0);
      ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), strip, actual.data(),
                                     static_cast<tmsize_t>(actual.size())),
                static_cast<tmsize_t>(actual.size()));
      if (tc.lossless) {
        EXPECT_EQ(actual, expected_strips[strip]);
      } else {
        int max_diff = 0;
        uint64_t sum_abs_diff = 0;
        for (size_t i = 0; i < actual.size(); ++i) {
          const int diff =
              std::abs(static_cast<int>(actual[i]) -
                       static_cast<int>(expected_strips[strip][i]));
          max_diff = std::max(max_diff, diff);
          sum_abs_diff += static_cast<uint64_t>(diff);
        }
        const double mae = static_cast<double>(sum_abs_diff) /
                           static_cast<double>(actual.size());
        EXPECT_LE(max_diff, 35) << "strip=" << strip;
        EXPECT_LE(mae, 6.0) << "strip=" << strip;
        if (tc.samples == 4 && tc.opaque_rgba) {
          for (size_t i = 3; i < actual.size(); i += 4) {
            EXPECT_EQ(actual[i], 255) << "strip=" << strip;
          }
        }
      }
    }

    if (tc.extra_sample == EXTRASAMPLE_ASSOCALPHA && tc.lossless) {
      std::vector<uint32_t> raster(static_cast<size_t>(tc.width) * tc.height,
                                   0);
      ASSERT_EQ(TIFFReadRGBAImage(reader.get(), tc.width, tc.height,
                                  raster.data(), 1),
                1);
      // Top-left pixel appears in bottom row of TIFFReadRGBAImage output.
      const uint32_t px = raster[static_cast<size_t>(tc.height - 1) * tc.width];
      EXPECT_EQ(TIFFGetR(px), full_image[0]);
      EXPECT_EQ(TIFFGetG(px), full_image[1]);
      EXPECT_EQ(TIFFGetB(px), full_image[2]);
      EXPECT_EQ(TIFFGetA(px), full_image[3]);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }
}

TEST(TifWebpTest, TiledEncodeAndDecodeRoundTrip) {
  struct TileCase {
    std::string_view name;
    const char* mode;
    uint32_t width;
    uint32_t height;
    uint32_t tile_w;
    uint32_t tile_h;
    uint16_t samples;
    bool lossless;
  };

  constexpr std::array<TileCase, 3> kCases = {{
      {.name = "ClassicLeRgbTiledLossless",
       .mode = "wl",
       .width = 40,
       .height = 36,
       .tile_w = 16,
       .tile_h = 16,
       .samples = 3,
       .lossless = true},
      {.name = "ClassicBeRgbaTiledLossless",
       .mode = "wb",
       .width = 32,
       .height = 32,
       .tile_w = 16,
       .tile_h = 16,
       .samples = 4,
       .lossless = true},
      {.name = "BigTiffLeRgbTiledLossy",
       .mode = "w8l",
       .width = 32,
       .height = 32,
       .tile_w = 16,
       .tile_h = 16,
       .samples = 3,
       .lossless = false},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    MemoryTiffStream stream;
    std::vector<std::vector<uint8_t>> expected_tiles;
    tmsize_t tile_size = 0;

    {
      UniqueTiff writer = stream.Open(tc.mode);
      ASSERT_NE(writer, nullptr);
      ConfigureTiledWebp(writer.get(), tc.width, tc.height, tc.tile_w,
                         tc.tile_h, tc.samples);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS,
                             tc.lossless ? 1 : 0),
                1);
      if (!tc.lossless) {
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LEVEL, 95), 1);
      }

      tile_size = TIFFTileSize(writer.get());
      ASSERT_EQ(tile_size,
                static_cast<tmsize_t>(tc.tile_w * tc.tile_h * tc.samples));
      const uint32_t num_tiles = TIFFNumberOfTiles(writer.get());
      expected_tiles.reserve(num_tiles);

      for (uint32_t tile = 0; tile < num_tiles; ++tile) {
        std::vector<uint8_t> tile_data =
            tc.samples == 3
                ? MakeSmoothRgbPixels(tc.tile_w, tc.tile_h, tile * 17U)
                : MakeRgbaPixels(tc.tile_w, tc.tile_h, false, false);
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
      if (tc.lossless) {
        EXPECT_EQ(actual, expected_tiles[tile]);
      } else {
        int max_diff = 0;
        uint64_t sum_abs_diff = 0;
        for (size_t i = 0; i < actual.size(); ++i) {
          const int diff = std::abs(static_cast<int>(actual[i]) -
                                    static_cast<int>(expected_tiles[tile][i]));
          max_diff = std::max(max_diff, diff);
          sum_abs_diff += static_cast<uint64_t>(diff);
        }
        const double mae = static_cast<double>(sum_abs_diff) /
                           static_cast<double>(actual.size());
        EXPECT_LE(max_diff, 30) << "tile=" << tile;
        EXPECT_LE(mae, 6.0) << "tile=" << tile;
      }
    }

    // Exercise TIFFReadTile on tile (0, 0).
    std::vector<uint8_t> tile0(static_cast<size_t>(tile_size), 0);
    ASSERT_EQ(TIFFReadTile(reader.get(), tile0.data(), 0, 0, 0, 0), tile_size);
    if (tc.lossless) {
      EXPECT_EQ(tile0, expected_tiles[0]);
    } else {
      int max_diff = 0;
      for (size_t i = 0; i < tile0.size(); ++i) {
        max_diff = std::max(max_diff,
                            std::abs(static_cast<int>(tile0[i]) -
                                     static_cast<int>(expected_tiles[0][i])));
      }
      EXPECT_LE(max_diff, 30);
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // Also cover TIFFWriteTile coordinate write helper on a single-tile RGBA
  // image.
  MemoryTiffStream tile_stream;
  const std::vector<uint8_t> expected = MakeRgbaPixels(16, 16, false, false);
  {
    UniqueTiff writer = tile_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureTiledWebp(writer.get(), 16, 16, 16, 16, 4);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);
    std::vector<uint8_t> scratch = expected;
    ASSERT_EQ(TIFFWriteTile(writer.get(), scratch.data(), 0, 0, 0, 0),
              static_cast<tmsize_t>(expected.size()));
  }
  UniqueTiff reader = tile_stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> actual(expected.size(), 0);
  ASSERT_EQ(TIFFReadTile(reader.get(), actual.data(), 0, 0, 0, 0),
            static_cast<tmsize_t>(actual.size()));
  EXPECT_EQ(actual, expected);
}

TEST(TifWebpTest, ScanlineWriteReadReorderedStripsAndPartialClose) {
  constexpr uint32_t kWidth = 8;
  constexpr uint32_t kHeight = 6;
  constexpr uint32_t kRowsPerStrip = 4;
  // Strip 0: 4 rows -> 96 bytes; Strip 1: 2 rows -> 48 bytes.
  const std::vector<uint8_t> full_image = MakeSmoothRgbPixels(kWidth, kHeight);
  const size_t stride = static_cast<size_t>(kWidth) * 3;

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), kWidth, kHeight, 3, kRowsPerStrip);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);

    // Always write all rows of each strip so sp->pBuffer is fully initialized.
    for (uint32_t row = 0; row < kHeight; ++row) {
      std::vector<uint8_t> row_data(full_image.begin() + row * stride,
                                    full_image.begin() + (row + 1) * stride);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row_data.data(), row, 0), 1);
    }
    ASSERT_EQ(TIFFFlush(writer.get()), 1);
  }

  // 1) Sequential scanline read of all rows across multiple strips.
  {
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> row_buf(stride, 0);
    for (uint32_t row = 0; row < kHeight; ++row) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), row, 0), 1);
      EXPECT_EQ(
          std::memcmp(row_buf.data(), full_image.data() + row * stride, stride),
          0)
          << "row=" << row;
    }
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // 2) Read smaller tail strip (strip 1, 2 rows) first, then switch to larger
  //    head strip (strip 0, 4 rows) to trigger:
  //    - TWebPPreDecode cleanup of active sp->psDecoder,
  //    - TWebPDecode reallocation when new buffer_size > sp->buffer_size,
  //    - TWebPCleanup freeing active psDecoder and pBuffer on partial-strip
  //    close.
  {
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> row_buf(stride, 0);

    ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 4, 0), 1);
    EXPECT_EQ(
        std::memcmp(row_buf.data(), full_image.data() + 4 * stride, stride), 0);

    ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 0, 0), 1);
    EXPECT_EQ(std::memcmp(row_buf.data(), full_image.data(), stride), 0);
    // Close without reading rows 1..3 so TWebPCleanup tears down live
    // psDecoder.
  }
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifWebpTest, UpdateModeSwitchesBetweenDecodeAndEncodeOnSameHandle) {
  constexpr uint32_t kWidth = 8;
  constexpr uint32_t kHeight = 4;
  constexpr uint32_t kRowsPerStrip = 2;
  const size_t strip_bytes = static_cast<size_t>(kWidth) * kRowsPerStrip * 3;
  const std::vector<uint8_t> strip0_orig =
      MakeSmoothRgbPixels(kWidth, kRowsPerStrip, 1);
  const std::vector<uint8_t> strip1_orig =
      MakeSmoothRgbPixels(kWidth, kRowsPerStrip, 50);
  const std::vector<uint8_t> strip1_new =
      MakeSmoothRgbPixels(kWidth, kRowsPerStrip, 120);

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), kWidth, kHeight, 3, kRowsPerStrip);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);
    std::vector<uint8_t> s0 = strip0_orig;
    std::vector<uint8_t> s1 = strip1_orig;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, s0.data(),
                                    static_cast<tmsize_t>(strip_bytes)),
              static_cast<tmsize_t>(strip_bytes));
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, s1.data(),
                                    static_cast<tmsize_t>(strip_bytes)),
              static_cast<tmsize_t>(strip_bytes));
  }

  {
    UniqueTiff rw = stream.Open("r+");
    ASSERT_NE(rw, nullptr);

    // Partial scanline read on strip 0 leaves sp->state = LSTATE_INIT_DECODE
    // and sp->psDecoder != NULL.
    std::vector<uint8_t> row0(static_cast<size_t>(kWidth) * 3, 0);
    ASSERT_EQ(TIFFReadScanline(rw.get(), row0.data(), 0, 0), 1);
    EXPECT_EQ(std::memcmp(row0.data(), strip0_orig.data(), row0.size()), 0);

    // Encoding strip 1 switches LSTATE_INIT_DECODE -> LSTATE_INIT_ENCODE inside
    // TWebPSetupEncode, freeing the active WebPIDecoder. Enable lossless so
    // the updated strip matches bit-for-bit on readback.
    ASSERT_EQ(TIFFSetField(rw.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);
    std::vector<uint8_t> s1_mut = strip1_new;
    ASSERT_EQ(TIFFWriteEncodedStrip(rw.get(), 1, s1_mut.data(),
                                    static_cast<tmsize_t>(strip_bytes)),
              static_cast<tmsize_t>(strip_bytes));
    ASSERT_EQ(TIFFFlush(rw.get()), 1);

    // Decoding strip 0 switches LSTATE_INIT_ENCODE -> LSTATE_INIT_DECODE inside
    // TWebPSetupDecode, freeing sPicture and pBuffer.
    std::vector<uint8_t> out0(strip_bytes, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, out0.data(),
                                   static_cast<tmsize_t>(strip_bytes)),
              static_cast<tmsize_t>(strip_bytes));
    EXPECT_EQ(out0, strip0_orig);
  }

  UniqueTiff verify = stream.Open("r");
  ASSERT_NE(verify, nullptr);
  std::vector<uint8_t> final0(strip_bytes, 0);
  std::vector<uint8_t> final1(strip_bytes, 0);
  ASSERT_EQ(TIFFReadEncodedStrip(verify.get(), 0, final0.data(),
                                 static_cast<tmsize_t>(strip_bytes)),
            static_cast<tmsize_t>(strip_bytes));
  ASSERT_EQ(TIFFReadEncodedStrip(verify.get(), 1, final1.data(),
                                 static_cast<tmsize_t>(strip_bytes)),
            static_cast<tmsize_t>(strip_bytes));
  EXPECT_EQ(final0, strip0_orig);
  EXPECT_EQ(final1, strip1_new);
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifWebpTest, SwitchingCompressionCleansUpStateAndMultiDir) {
  // 1) Switch from COMPRESSION_WEBP to COMPRESSION_NONE after setting tags.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 8, 2, 3, 2);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    uint16_t scheme = 0;
    ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_COMPRESSION, &scheme), 1);
    EXPECT_EQ(scheme, COMPRESSION_NONE);
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // 2) Write WebP IFD 0 then uncompressed IFD 1 so TWebPCleanup frees encoder
  //    state on write and decoder state on read directory advance.
  MemoryTiffStream stream;
  const std::vector<uint8_t> webp_pixels = MakeSmoothRgbPixels(8, 2);
  const std::vector<uint8_t> raw_pixels = MakeSmoothRgbPixels(8, 2, 77);
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 8, 2, 3, 2);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);
    std::vector<uint8_t> s0 = webp_pixels;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, s0.data(),
                                    static_cast<tmsize_t>(s0.size())),
              static_cast<tmsize_t>(s0.size()));
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);

    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{8}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{2}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{2}), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    std::vector<uint8_t> s1 = raw_pixels;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, s1.data(),
                                    static_cast<tmsize_t>(s1.size())),
              static_cast<tmsize_t>(s1.size()));
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> out0(webp_pixels.size(), 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out0.data(),
                                 static_cast<tmsize_t>(out0.size())),
            static_cast<tmsize_t>(out0.size()));
  EXPECT_EQ(out0, webp_pixels);

  ASSERT_EQ(TIFFReadDirectory(reader.get()), 1);
  uint16_t comp = 0;
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &comp), 1);
  EXPECT_EQ(comp, COMPRESSION_NONE);
  std::vector<uint8_t> out1(raw_pixels.size(), 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out1.data(),
                                 static_cast<tmsize_t>(out1.size())),
            static_cast<tmsize_t>(out1.size()));
  EXPECT_EQ(out1, raw_pixels);
  EXPECT_TRUE(stream.diagnostics().errors().empty());
}

TEST(TifWebpTest, RejectsInvalidPlanarSamplesDepthFormatAndPredictor) {
  // 1) Encode rejects SamplesPerPixel != 3 and != 4.
  for (uint16_t bad_samples : {uint16_t{1}, uint16_t{2}, uint16_t{5}}) {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, bad_samples),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(
                  writer.get(), TIFFTAG_PHOTOMETRIC,
                  bad_samples < 3 ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_WEBP),
              1);

    std::vector<uint8_t> buf(static_cast<size_t>(16) * bad_samples, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                    static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPSetupEncode",
                                        "Must be 3 (RGB) or 4 (RGBA) bands"));
  }

  // 2) Encode rejects BitsPerSample != 8 and SampleFormat != SAMPLEFORMAT_UINT.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 4, 4, 3, 4);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{16}),
              1);
    std::vector<uint8_t> buf(4 * 4 * 3 * 2, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                    static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "WebPSetupEncode",
        "WEBP driver requires 8 bit unsigned data"));
  }
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 4, 4, 3, 4);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_INT), 1);
    std::vector<uint8_t> buf(4 * 4 * 3, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                    static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "WebPSetupEncode",
        "WEBP driver requires 8 bit unsigned data"));
  }

  // 3) TIFFTAG_PREDICTOR is not supported with COMPRESSION_WEBP.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 4, 4, 3, 4);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL), 0);
    EXPECT_FALSE(stream.diagnostics().errors().empty());
  }

  const std::vector<uint8_t> valid_blob = ExtractRawWebpStrip(
      4, 4, 3, MakeSmoothRgbPixels(4, 4), /*lossless=*/true);
  ASSERT_FALSE(valid_blob.empty());

  // 4) Decode rejects PLANARCONFIG_SEPARATE during directory fixup.
  {
    RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
    const std::array<uint64_t, 3> offsets = {
        builder.AppendBlob(valid_blob),
        builder.AppendBlob(valid_blob),
        builder.AppendBlob(valid_blob),
    };
    const std::array<uint32_t, 3> off32 = {
        static_cast<uint32_t>(offsets[0]),
        static_cast<uint32_t>(offsets[1]),
        static_cast<uint32_t>(offsets[2]),
    };
    const std::array<uint32_t, 3> bc32 = {
        static_cast<uint32_t>(valid_blob.size()),
        static_cast<uint32_t>(valid_blob.size()),
        static_cast<uint32_t>(valid_blob.size()),
    };
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 4),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE,
                               std::array<uint16_t, 3>{8, 8, 8}),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_WEBP),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
        builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, off32),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 4),
        builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, bc32),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE),
    });

    MemoryTiffStream stream(builder.Take());
    UniqueTiff reader = stream.Open("r");
    // TIFFReadDirectory invokes TWebPFixupTags but ignores its return value,
    // so open succeeds while logging the planar configuration error.
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "TWebPFixupTags",
        "TIFF WEBP requires data to be stored contiguously"));
  }

  // 5) Decode rejects invalid SamplesPerPixel (1, 2, 5).
  for (uint16_t bad_samples : {uint16_t{1}, uint16_t{2}, uint16_t{5}}) {
    MemoryTiffStream stream(BuildRawWebpSingleStripTiff(
        /*big_endian=*/false, /*big_tiff=*/false, 4, 4, bad_samples, 8,
        SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, 4, valid_blob));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(static_cast<size_t>(16) * bad_samples, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPSetupDecode",
                                        "Must be 3 (RGB) or 4 (RGBA) bands"));
  }

  // 6) Decode rejects BitsPerSample != 8 && SampleFormat != 1.
  {
    MemoryTiffStream stream(BuildRawWebpSingleStripTiff(
        /*big_endian=*/false, /*big_tiff=*/false, 4, 4, 3, 16, SAMPLEFORMAT_INT,
        PLANARCONFIG_CONTIG, 4, valid_blob));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(4 * 4 * 3 * 2, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "WebPSetupDecode",
        "WEBP driver requires 8 bit unsigned data"));
  }
}

TEST(TifWebpTest, RejectsDimensionsExceedingWebpLimit) {
  // 1) Encode rejection for striped width > 16383 and height > 16383.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 16384, 1, 3, 1);
    std::vector<uint8_t> buf(16384 * 3, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                    static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "TWebPPreEncode",
        "WEBP maximum image dimensions are 16383 x 16383"));
  }
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 1, 16384, 3, 16384);
    std::vector<uint8_t> buf(16384 * 3, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                    static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "TWebPPreEncode",
        "WEBP maximum image dimensions are 16383 x 16383"));
  }

  // 1b) Encode rejection for tiled width > 16383 and height > 16383.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureTiledWebp(writer.get(), 16384, 16, 16384, 16, 3);
    std::vector<uint8_t> buf(static_cast<size_t>(16384) * 16 * 3, 0);
    EXPECT_EQ(TIFFWriteEncodedTile(writer.get(), 0, buf.data(),
                                   static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "TWebPPreEncode",
        "WEBP maximum image dimensions are 16383 x 16383"));
  }

  // 2) Decode rejection for striped and tiled dimensions > 16383.
  const std::array<uint8_t, 8> dummy_blob = {1, 2, 3, 4, 5, 6, 7, 8};
  {
    MemoryTiffStream stream(BuildRawWebpSingleStripTiff(
        /*big_endian=*/false, /*big_tiff=*/false, 16384, 1, 3, 8,
        SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, 1, dummy_blob));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(16384 * 3, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "TWebPPreDecode",
        "WEBP maximum image dimensions are 16383 x 16383"));
  }
  {
    MemoryTiffStream stream(
        BuildRawWebpSingleTileTiff(16384, 16, 16384, 16, 3, dummy_blob));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(16384 * 16 * 3, 0);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, out.data(),
                                  static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "TWebPPreDecode",
        "WEBP maximum image dimensions are 16383 x 16383"));
  }
}

TEST(TifWebpTest,
     InvalidEncoderConfigOversizedChunkSmallBufferAndFlushFailure) {
  // 1) Invalid quality_level (-1 / 101) fails WebPConfigInitInternal in
  //    TWebPSetupEncode.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 8, 8, 3, 8);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LEVEL, -1), 1);
    std::vector<uint8_t> buf = MakeSmoothRgbPixels(8, 8);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                    static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "WebPSetupEncode",
        "Error creating WebP encoder configuration"));
  }

  // 2) Invalid lossless / lossless_exact (>1) passes WebPConfigInitInternal but
  //    fails WebPValidateConfig in TWebPSetupEncode.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 8, 8, 3, 8);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 2), 1);
    std::vector<uint8_t> buf = MakeSmoothRgbPixels(8, 8);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                    static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "WebPSetupEncode",
        "Error with WebP encoder configuration"));
  }
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 8, 8, 3, 8);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS_EXACT, 2), 1);
    std::vector<uint8_t> buf = MakeSmoothRgbPixels(8, 8);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                    static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "WebPSetupEncode",
        "Error with WebP encoder configuration"));
  }

  // 3) Passing cc > strip buffer_size triggers TWebPEncode overflow guard.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 4, 4, 3, 4);
    std::vector<uint8_t> oversized(4 * 4 * 3 + 12, 0x55);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, oversized.data(),
                                    static_cast<tmsize_t>(oversized.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "TWebPEncode",
                                        "Too many bytes to be written"));
  }

  // 4) Small tif_rawdata buffer (16 bytes) with non-trivial image triggers
  //    TWebPDatasetWriter overflow and VP8_ENC_ERROR_BAD_WRITE in PostEncode.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 16, 16, 3, 16);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 16), 1);
    std::vector<uint8_t> noisy = MakeTexturedRgbPixels(16, 16);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, noisy.data(),
                                    static_cast<tmsize_t>(noisy.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "TWebPDatasetWriter",
                                        "Buffer too small by"));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPPostEncode",
                                        "Error while fludshing bytes"));
  }

  // 5) Write failure during TWebPPostEncode's TIFFFlushData1 call.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 8, 8, 3, 8);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);
    std::vector<uint8_t> pixels = MakeSmoothRgbPixels(8, 8);
    stream.FailWritesAfter(stream.write_calls());
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, pixels.data(),
                                    static_cast<tmsize_t>(pixels.size())),
              static_cast<tmsize_t>(-1));
    stream.ResetFaults();
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPPostEncode",
                                        "Error flushing TIFF WebP encoder"));
  }
}

TEST(TifWebpTest, CorruptTruncatedMismatchedAndFractionalDecode) {
  const std::vector<uint8_t> valid_rgb_4x4 = ExtractRawWebpStrip(
      4, 4, 3, MakeSmoothRgbPixels(4, 4), /*lossless=*/true);
  const std::vector<uint8_t> valid_rgba_4x4 = ExtractRawWebpStrip(
      4, 4, 4, MakeRgbaPixels(4, 4, /*opaque_only=*/false), /*lossless=*/true);
  const std::vector<uint8_t> textured_rgb_64x64 = ExtractRawWebpStrip(
      64, 64, 3, MakeTexturedRgbPixels(64, 64), /*lossless=*/true);
  ASSERT_FALSE(valid_rgb_4x4.empty());
  ASSERT_FALSE(valid_rgba_4x4.empty());
  ASSERT_GT(textured_rgb_64x64.size(), 100U);

  // 1) Garbage payload fails WebPGetInfo, sets sp->read_error, and subsequent
  //    scanline read in the same strip hits sp->read_error early exit.
  {
    const std::array<uint8_t, 8> garbage = {0xde, 0xad, 0xbe, 0xef,
                                            0x01, 0x02, 0x03, 0x04};
    MemoryTiffStream stream(BuildRawWebpSingleStripTiff(
        /*big_endian=*/false, /*big_tiff=*/false, 4, 4, 3, 8, SAMPLEFORMAT_UINT,
        PLANARCONFIG_CONTIG, 4, garbage));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> row0(4 * 3, 0xaa);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row0.data(), 0, 0), -1);
    EXPECT_EQ(row0, std::vector<uint8_t>(4 * 3, 0));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPDecode", "WebPGetInfo() failed"));

    std::vector<uint8_t> row1(4 * 3, 0xbb);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row1.data(), 1, 0), -1);
    EXPECT_EQ(row1, std::vector<uint8_t>(4 * 3, 0));
    EXPECT_TRUE(
        HasDiagnosticContaining(stream.diagnostics().errors(), "WebPDecode",
                                "cannot be read due to previous error"));
  }

  // 2) WebP blob dimension mismatch (width 4!=8, height 4!=8).
  for (auto [w, h] : std::array<std::pair<uint32_t, uint32_t>, 2>{{
           {8, 4},
           {4, 8},
       }}) {
    MemoryTiffStream stream(BuildRawWebpSingleStripTiff(
        /*big_endian=*/false, /*big_tiff=*/false, w, h, 3, 8, SAMPLEFORMAT_UINT,
        PLANARCONFIG_CONTIG, h, valid_rgb_4x4));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 3, 0xaa);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPDecode",
                                        "WebP blob dimension is 4x4"));
  }

  // 3) WebP blob band count mismatch (4-band RGBA blob in 3-sample TIFF).
  {
    MemoryTiffStream stream(BuildRawWebpSingleStripTiff(
        /*big_endian=*/false, /*big_tiff=*/false, 4, 4, 3, 8, SAMPLEFORMAT_UINT,
        PLANARCONFIG_CONTIG, 4, valid_rgba_4x4));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(4 * 4 * 3, 0xaa);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(
        HasDiagnosticContaining(stream.diagnostics().errors(), "WebPDecode",
                                "WebP blob band count is 4. Expected 3"));
  }

  // 4) Fractional scanline read returns -1 without setting read_error, so
  //    re-reading the full strip recovers cleanly.
  {
    MemoryTiffStream stream(BuildRawWebpSingleStripTiff(
        /*big_endian=*/false, /*big_tiff=*/false, 4, 4, 3, 8, SAMPLEFORMAT_UINT,
        PLANARCONFIG_CONTIG, 4, valid_rgb_4x4));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> partial(5, 0xcc);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, partial.data(), 5),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(partial, std::vector<uint8_t>(5, 0));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPDecode",
                                        "Fractional scanlines cannot be read"));

    stream.diagnostics().Clear();
    std::vector<uint8_t> recovered(4 * 4 * 3, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, recovered.data(),
                                   static_cast<tmsize_t>(recovered.size())),
              static_cast<tmsize_t>(recovered.size()));
    EXPECT_EQ(recovered, MakeSmoothRgbPixels(4, 4));
    EXPECT_TRUE(stream.diagnostics().errors().empty());
  }

  // 5) Corrupt VP8L bitstream body after valid RIFF/WEBP/VP8L header ->
  //    WebPIAppend returns VP8_STATUS_BITSTREAM_ERROR ("Unrecognized error.").
  {
    std::vector<uint8_t> corrupt_body = textured_rgb_64x64;
    for (size_t i = 36; i < corrupt_body.size(); ++i) {
      corrupt_body[i] ^= 0xffU;
    }
    MemoryTiffStream stream(BuildRawWebpSingleStripTiff(
        /*big_endian=*/false, /*big_tiff=*/false, 64, 64, 3, 8,
        SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, 64, corrupt_body));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(64 * 64 * 3, 0xaa);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPDecode", "Unrecognized error"));
  }

  // 6) Truncated WebP bitstream after valid header -> WebPIAppend returns
  //    VP8_STATUS_SUSPENDED and incomplete image fails ("Unable to decode WebP
  //    data.").
  {
    const absl::Span<const uint8_t> truncated(textured_rgb_64x64.data(), 48);
    MemoryTiffStream stream(BuildRawWebpSingleStripTiff(
        /*big_endian=*/false, /*big_tiff=*/false, 64, 64, 3, 8,
        SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, 64, truncated));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(64 * 64 * 3, 0xaa);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPDecode",
                                        "Unable to decode WebP data."));
  }

  // 7) Truncated lossy VP8 bitstream also fails cleanly in TWebPDecode.
  {
    std::vector<uint8_t> lossy_blob = ExtractRawWebpStrip(
        64, 64, 3, MakeTexturedRgbPixels(64, 64), /*lossless=*/false);
    ASSERT_GT(lossy_blob.size(), 64U);
    const absl::Span<const uint8_t> truncated(lossy_blob.data(), 48);
    MemoryTiffStream stream(BuildRawWebpSingleStripTiff(
        /*big_endian=*/false, /*big_tiff=*/false, 64, 64, 3, 8,
        SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, 64, truncated));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(64 * 64 * 3, 0xaa);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_FALSE(stream.diagnostics().errors().empty());
  }
}

TEST(TifWebpTest, AllocationLimitsAndOomPaths) {
  // 1) TIFFInitWebP fails cleanly when _TIFFmallocExt(sizeof(WebPState)) hits
  //    the cumulated memory allocation budget (leaving headroom for
  //    _TIFFMergeFields).
  {
    MemoryTiffStream stream;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), kDefaultAllocBudget);

    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif, nullptr);
    {
      ScopedCumulatedMemAllocRemaining budget(tif.get(), 32);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_WEBP),
                0);
    }
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "TIFFInitWebP",
                                        "No space for WebP state block"));
  }

  // 2) TWebPPreEncode OOM when allocating sp->pBuffer.
  {
    MemoryTiffStream stream;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), kDefaultAllocBudget);

    UniqueTiff writer = stream.Open("wl", opts.get());
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 64, 64, 4, 64);
    // Pre-allocate small raw output buffer so BUFFERCHECK doesn't hit limit
    // before TWebPPreEncode tries to allocate 64 * 64 * 4 = 16384 bytes.
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 1024), 1);

    std::vector<uint8_t> pixels = MakeRgbaPixels(64, 64, /*opaque_only=*/false);
    {
      ScopedCumulatedMemAllocRemaining budget(writer.get(), 4096);
      EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, pixels.data(),
                                      static_cast<tmsize_t>(pixels.size())),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "TWebPPreEncode",
                                        "Cannot allocate buffer"));
  }

  // 3) TWebPDecode OOM when allocating sp->pBuffer during scanline decode.
  {
    const std::vector<uint8_t> solid_rgba(64U * 64U * 4U, 0x7fU);
    const std::vector<uint8_t> small_blob =
        ExtractRawWebpStrip(64, 64, 4, solid_rgba, /*lossless=*/true);
    ASSERT_GT(small_blob.size(), 0U);
    ASSERT_LT(small_blob.size(), 1024U);

    MemoryTiffStream stream(BuildRawWebpSingleStripTiff(
        /*big_endian=*/false, /*big_tiff=*/false, 64, 64, 4, 8,
        SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, 64, small_blob));
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), kDefaultAllocBudget);

    UniqueTiff reader = stream.Open("r", opts.get());
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> row_buf(64 * 4, 0xaa);
    {
      // Allow TIFFFillStrip's small raw buffer (<1 KiB), fail TWebPDecode's
      // 16 KiB sp->pBuffer allocation via ScopedSingleMemAllocLimit.
      ScopedSingleMemAllocLimit limit(reader.get(), 2048);
      EXPECT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 0, 0), -1);
    }
    EXPECT_EQ(row_buf, std::vector<uint8_t>(64 * 4, 0));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "WebPDecode", "Cannot allocate buffer"));
  }
}

TEST(TifWebpTest, MemoryMappedAndUserBufferDecode) {
  const std::vector<uint8_t> expected = MakeSmoothRgbPixels(16, 8);
  MemoryTiffStream stream;
  stream.set_enable_mmap(true);
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), 16, 8, 3, 8);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 1), 1);
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

// Mirrors WebPState in third_party/tiff/libtiff/tif_webp.c so tests can inject
// WebPPicture / WebPConfig error states deterministically right before
// TIFFFlushData(). Coupled directly to upstream internal struct layout because
// no public hook exists for progress/config fault injection; verify field
// ordering against WebPState on Copybara upstream upgrades.
struct WebPStateLayout {
  uint16_t nSamples;
  int read_error;
  int lossless;
  int lossless_exact;
  int quality_level;
  WebPPicture sPicture;
  WebPConfig sEncoderConfig;
  uint8_t* pBuffer;
  unsigned int buffer_offset;
  unsigned int buffer_size;
  WebPIDecoder* psDecoder;
  WebPDecBuffer sDecBuffer;
  int last_y;
  int state;
  TIFFVGetMethod vgetparent;
  TIFFVSetMethod vsetparent;
};

int AbortProgressHook(int /*percent*/, const WebPPicture* /*picture*/) {
  return 0;
}

TEST(TifWebpTest, PostEncodeErrorBranchesViaEncoderState) {
  constexpr uint32_t kWidth = 4;
  constexpr uint32_t kHeight = 2;

  auto prepare_writer = [&](MemoryTiffStream& stream, UniqueTiff& writer,
                            uint16_t samples) {
    writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedWebp(writer.get(), kWidth, kHeight, samples, kHeight);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WEBP_LOSSLESS, 0), 1);

    const std::vector<uint8_t> pixels =
        samples == 3 ? MakeSmoothRgbPixels(kWidth, kHeight)
                     : MakeRgbaPixels(kWidth, kHeight, /*opaque_only=*/false);
    const size_t row_bytes = static_cast<size_t>(kWidth) * samples;
    // Write both rows of the strip so sp->pBuffer is completely initialized.
    for (uint32_t row = 0; row < kHeight; ++row) {
      std::vector<uint8_t> row_buf(pixels.begin() + row * row_bytes,
                                   pixels.begin() + (row + 1) * row_bytes);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), row, 0), 1);
    }
  };

  // 1) WebPPictureImportRGBA() failure when width == 0 (spp == 4).
  {
    MemoryTiffStream stream;
    UniqueTiff writer;
    ASSERT_NO_FATAL_FAILURE(prepare_writer(stream, writer, /*samples=*/4));
    auto* sp = reinterpret_cast<WebPStateLayout*>(writer->tif_data);
    ASSERT_NE(sp, nullptr);
    ASSERT_EQ(sp->nSamples, 4);
    ASSERT_EQ(sp->sPicture.width, static_cast<int>(kWidth));
    ASSERT_EQ(sp->sPicture.height, static_cast<int>(kHeight));
    sp->sPicture.width = 0;
    EXPECT_EQ(TIFFFlushData(writer.get()), 0);
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPPostEncode",
                                        "WebPPictureImportRGBA() failed"));
  }

  // 2) WebPPictureImportRGB() failure when width == 0 (spp == 3).
  {
    MemoryTiffStream stream;
    UniqueTiff writer;
    ASSERT_NO_FATAL_FAILURE(prepare_writer(stream, writer, /*samples=*/3));
    auto* sp = reinterpret_cast<WebPStateLayout*>(writer->tif_data);
    ASSERT_NE(sp, nullptr);
    ASSERT_EQ(sp->nSamples, 3);
    ASSERT_EQ(sp->sPicture.width, static_cast<int>(kWidth));
    ASSERT_EQ(sp->sPicture.height, static_cast<int>(kHeight));
    sp->sPicture.width = 0;
    EXPECT_EQ(TIFFFlushData(writer.get()), 0);
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPPostEncode",
                                        "WebPPictureImportRGB() failed"));
  }

  // 3) WebPEncode() failure with VP8_ENC_ERROR_INVALID_CONFIGURATION.
  {
    MemoryTiffStream stream;
    UniqueTiff writer;
    ASSERT_NO_FATAL_FAILURE(prepare_writer(stream, writer, /*samples=*/3));
    auto* sp = reinterpret_cast<WebPStateLayout*>(writer->tif_data);
    ASSERT_NE(sp, nullptr);
    ASSERT_EQ(sp->sPicture.width, static_cast<int>(kWidth));
    sp->sEncoderConfig.quality = -1.0f;
    EXPECT_EQ(TIFFFlushData(writer.get()), 0);
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPPostEncode",
                                        "Configuration is invalid"));
  }

  // 4) WebPEncode() failure with VP8_ENC_ERROR_USER_ABORT via progress_hook.
  {
    MemoryTiffStream stream;
    UniqueTiff writer;
    ASSERT_NO_FATAL_FAILURE(prepare_writer(stream, writer, /*samples=*/3));
    auto* sp = reinterpret_cast<WebPStateLayout*>(writer->tif_data);
    ASSERT_NE(sp, nullptr);
    ASSERT_EQ(sp->sPicture.width, static_cast<int>(kWidth));
    sp->sPicture.progress_hook = &AbortProgressHook;
    EXPECT_EQ(TIFFFlushData(writer.get()), 0);
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "WebPPostEncode", "User interrupted"));
  }
}

}  // namespace
}  // namespace libtiff
