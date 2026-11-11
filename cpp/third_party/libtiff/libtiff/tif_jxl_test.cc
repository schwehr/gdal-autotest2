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

// Unit tests for libtiff JPEG XL codec (third_party/tiff/libtiff/tif_jxl.c),
// covering COMPRESSION_JXL and COMPRESSION_JXL_DNG_1_7, pseudo-tags, integer
// and floating-point sample formats, multi-channel and extra-sample layouts,
// strips/tiles/planar configs, mode switching, corrupt bitstreams, and OOM.

#include "third_party/tiff/libtiff/tif_jxl.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;

constexpr std::array<uint16_t, 2> kJxlSchemes = {
    COMPRESSION_JXL,
    COMPRESSION_JXL_DNG_1_7,
};

// Exact IEEE 754 binary16 representations in [0.0, 4.0] for lossless float16.
constexpr std::array<uint16_t, 8> kExactHalfFloats = {
    0x0000,  // 0.0
    0x3400,  // 0.25
    0x3800,  // 0.5
    0x3A00,  // 0.75
    0x3C00,  // 1.0
    0x3E00,  // 1.5
    0x4000,  // 2.0
    0x4200,  // 3.0
};

constexpr std::array<float, 8> kExactFloats = {
    0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f,
};

struct ImageSpec {
  uint32_t width = 8;
  uint32_t height = 8;
  uint16_t compression = COMPRESSION_JXL;
  uint16_t photometric = PHOTOMETRIC_RGB;
  uint16_t bits_per_sample = 8;
  uint16_t sample_format = SAMPLEFORMAT_UINT;
  uint16_t samples_per_pixel = 3;
  uint16_t planar_config = PLANARCONFIG_CONTIG;
  uint32_t rows_per_strip = 8;
  std::vector<uint16_t> extra_samples;
  bool lossless = true;
  uint32_t effort = 5;
  float distance = 1.0f;
  float alpha_distance = -1.0f;
  const char* write_mode = "w";
};

size_t BytesPerSample(uint16_t bps) { return bps / 8U; }

std::vector<uint8_t> MakeDeterministicSamples(const ImageSpec& spec,
                                              size_t pixel_count,
                                              uint16_t channels) {
  const size_t bps_bytes = BytesPerSample(spec.bits_per_sample);
  const size_t total_samples = pixel_count * channels;
  std::vector<uint8_t> out(total_samples * bps_bytes, 0);

  for (size_t i = 0; i < total_samples; ++i) {
    if (spec.sample_format == SAMPLEFORMAT_UINT && spec.bits_per_sample == 8) {
      out[i] = static_cast<uint8_t>((i * 29U + 17U) & 0xffU);
    } else if (spec.sample_format == SAMPLEFORMAT_UINT &&
               spec.bits_per_sample == 16) {
      const uint16_t val = static_cast<uint16_t>((i * 757U + 123U) & 0xffffU);
      std::memcpy(out.data() + i * 2, &val, sizeof(val));
    } else if (spec.sample_format == SAMPLEFORMAT_IEEEFP &&
               spec.bits_per_sample == 16) {
      const uint16_t half = kExactHalfFloats[i % kExactHalfFloats.size()];
      std::memcpy(out.data() + i * 2, &half, sizeof(half));
    } else if (spec.sample_format == SAMPLEFORMAT_IEEEFP &&
               spec.bits_per_sample == 32) {
      const float flt = kExactFloats[i % kExactFloats.size()];
      std::memcpy(out.data() + i * 4, &flt, sizeof(flt));
    }
  }
  return out;
}

void ConfigureCommonTags(TIFF* tif, const ImageSpec& spec) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, spec.width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, spec.height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, spec.compression), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, spec.photometric), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, spec.bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, spec.sample_format), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, spec.samples_per_pixel),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, spec.planar_config), 1);
  if (!spec.extra_samples.empty()) {
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_EXTRASAMPLES,
                           static_cast<uint16_t>(spec.extra_samples.size()),
                           spec.extra_samples.data()),
              1);
  }
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_JXL_LOSSYNESS,
                         spec.lossless ? JXL_LOSSLESS : JXL_LOSSY),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_JXL_EFFORT, spec.effort), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_JXL_DISTANCE, spec.distance), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_JXL_ALPHA_DISTANCE, spec.alpha_distance),
            1);
}

void WriteStrippedImage(MemoryTiffStream& stream, const ImageSpec& spec,
                        const std::vector<uint8_t>& reference) {
  UniqueTiff writer = stream.Open(spec.write_mode);
  ASSERT_NE(writer, nullptr);
  ConfigureCommonTags(writer.get(), spec);
  ASSERT_EQ(
      TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, spec.rows_per_strip), 1);

  const uint32_t strips_per_plane =
      (spec.height + spec.rows_per_strip - 1U) / spec.rows_per_strip;
  const uint16_t planes = (spec.planar_config == PLANARCONFIG_SEPARATE)
                              ? spec.samples_per_pixel
                              : 1;
  const uint16_t channels_per_strip =
      (spec.planar_config == PLANARCONFIG_SEPARATE) ? 1
                                                    : spec.samples_per_pixel;
  const size_t bytes_per_row = static_cast<size_t>(spec.width) *
                               channels_per_strip *
                               BytesPerSample(spec.bits_per_sample);

  size_t offset = 0;
  for (uint16_t plane = 0; plane < planes; ++plane) {
    for (uint32_t s = 0; s < strips_per_plane; ++s) {
      const uint32_t row_start = s * spec.rows_per_strip;
      const uint32_t rows_in_strip =
          std::min(spec.rows_per_strip, spec.height - row_start);
      const tmsize_t strip_bytes =
          static_cast<tmsize_t>(rows_in_strip * bytes_per_row);
      // Pass a mutable copy because TIFFWriteEncodedStrip swaps in place.
      std::vector<uint8_t> mutable_strip(
          reference.begin() + offset, reference.begin() + offset + strip_bytes);
      const uint32_t strip_idx =
          static_cast<uint32_t>(plane) * strips_per_plane + s;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), strip_idx,
                                      mutable_strip.data(), strip_bytes),
                strip_bytes);
      offset += static_cast<size_t>(strip_bytes);
    }
  }
  ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  writer.reset();
}

std::vector<uint8_t> ReadStrippedImage(MemoryTiffStream& stream,
                                       const ImageSpec& spec) {
  UniqueTiff reader = stream.Open("r");
  EXPECT_NE(reader, nullptr);
  if (reader == nullptr) return {};

  uint16_t comp = 0;
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &comp), 1);
  EXPECT_EQ(comp, spec.compression);

  const uint32_t strips_per_plane =
      (spec.height + spec.rows_per_strip - 1U) / spec.rows_per_strip;
  const uint16_t planes = (spec.planar_config == PLANARCONFIG_SEPARATE)
                              ? spec.samples_per_pixel
                              : 1;
  const uint16_t channels_per_strip =
      (spec.planar_config == PLANARCONFIG_SEPARATE) ? 1
                                                    : spec.samples_per_pixel;
  const size_t bytes_per_row = static_cast<size_t>(spec.width) *
                               channels_per_strip *
                               BytesPerSample(spec.bits_per_sample);

  std::vector<uint8_t> out;
  out.reserve(static_cast<size_t>(spec.width) * spec.height *
              spec.samples_per_pixel * BytesPerSample(spec.bits_per_sample));

  for (uint16_t plane = 0; plane < planes; ++plane) {
    for (uint32_t s = 0; s < strips_per_plane; ++s) {
      const uint32_t row_start = s * spec.rows_per_strip;
      const uint32_t rows_in_strip =
          std::min(spec.rows_per_strip, spec.height - row_start);
      const tmsize_t strip_bytes =
          static_cast<tmsize_t>(rows_in_strip * bytes_per_row);
      std::vector<uint8_t> strip_buf(static_cast<size_t>(strip_bytes), 0);
      const uint32_t strip_idx =
          static_cast<uint32_t>(plane) * strips_per_plane + s;
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), strip_idx, strip_buf.data(),
                                     strip_bytes),
                strip_bytes);
      out.insert(out.end(), strip_buf.begin(), strip_buf.end());
    }
  }
  return out;
}

std::vector<uint8_t> ExtractSingleRawStrip(const ImageSpec& spec,
                                           const std::vector<uint8_t>& pixels) {
  MemoryTiffStream stream;
  WriteStrippedImage(stream, spec, pixels);
  UniqueTiff reader = stream.Open("r");
  EXPECT_NE(reader, nullptr);
  if (reader == nullptr) return {};
  const tmsize_t raw_size = TIFFRawStripSize(reader.get(), 0);
  EXPECT_GT(raw_size, 0);
  if (raw_size <= 0) return {};
  std::vector<uint8_t> raw(static_cast<size_t>(raw_size), 0);
  EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, raw.data(), raw_size), raw_size);
  return raw;
}

TEST(TifJxlTest, CodecRegistrationConfiguredAndNamed) {
  for (uint16_t scheme : kJxlSchemes) {
    SCOPED_TRACE(scheme);
    EXPECT_EQ(TIFFIsCODECConfigured(scheme), 1);
    const TIFFCodec* codec = TIFFFindCODEC(scheme);
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(codec->scheme, scheme);
    if (scheme == COMPRESSION_JXL) {
      EXPECT_STREQ(codec->name, "JXL");
    } else {
      EXPECT_STREQ(codec->name, "JXL_DNG_1_7");
    }
  }

  UniqueTiffBuffer<TIFFCodec> configured(TIFFGetConfiguredCODECs());
  ASSERT_NE(configured, nullptr);
  bool found_jxl = false;
  bool found_dng = false;
  for (const TIFFCodec* c = configured.get(); c->name != nullptr; ++c) {
    if (c->scheme == COMPRESSION_JXL) found_jxl = true;
    if (c->scheme == COMPRESSION_JXL_DNG_1_7) found_dng = true;
  }
  EXPECT_TRUE(found_jxl);
  EXPECT_TRUE(found_dng);
}

TEST(TifJxlTest, PseudoTagDefaultsValidationAndParentDelegation) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JXL), 1);

  uint32_t lossyness = 999;
  uint32_t effort = 0;
  float distance = -10.0f;
  float alpha_distance = 99.0f;

  ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_JXL_LOSSYNESS, &lossyness), 1);
  EXPECT_EQ(lossyness, static_cast<uint32_t>(JXL_LOSSLESS));

  ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_JXL_EFFORT, &effort), 1);
  EXPECT_EQ(effort, 5U);

  ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_JXL_DISTANCE, &distance), 1);
  EXPECT_FLOAT_EQ(distance, 1.0f);

  ASSERT_EQ(
      TIFFGetField(tif.get(), TIFFTAG_JXL_ALPHA_DISTANCE, &alpha_distance), 1);
  EXPECT_FLOAT_EQ(alpha_distance, -1.0f);

  // Set valid values and verify via TIFFGetField.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_LOSSYNESS,
                         static_cast<uint32_t>(JXL_LOSSY)),
            1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_EFFORT, 9U), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_DISTANCE, 2.5f), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_ALPHA_DISTANCE, 3.25f), 1);

  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_JXL_LOSSYNESS, &lossyness), 1);
  EXPECT_EQ(lossyness, static_cast<uint32_t>(JXL_LOSSY));
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_JXL_EFFORT, &effort), 1);
  EXPECT_EQ(effort, 9U);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_JXL_DISTANCE, &distance), 1);
  EXPECT_FLOAT_EQ(distance, 2.5f);
  EXPECT_EQ(
      TIFFGetField(tif.get(), TIFFTAG_JXL_ALPHA_DISTANCE, &alpha_distance), 1);
  EXPECT_FLOAT_EQ(alpha_distance, 3.25f);

  // Verify boundary valid values.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_EFFORT, 1U), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_DISTANCE, 0.0f), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_DISTANCE, 15.0f), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_ALPHA_DISTANCE, -1.0f), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_ALPHA_DISTANCE, 0.0f), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_ALPHA_DISTANCE, 15.0f), 1);

  // Verify parent tag delegation through JXLVSetField / JXLVGetField.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 32U), 1);
  uint32_t width = 0;
  ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
  EXPECT_EQ(width, 32U);

  // Invalid pseudo-tag values must fail without overwriting state.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_LOSSYNESS, 2U), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_EFFORT, 0U), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_EFFORT, 10U), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_DISTANCE, -0.1f), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_DISTANCE, 15.1f), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_ALPHA_DISTANCE, -0.5f), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_ALPHA_DISTANCE, -2.0f), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JXL_ALPHA_DISTANCE, 15.1f), 0);
  EXPECT_GE(stream.diagnostics().error_count(), 8U);

  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_JXL_LOSSYNESS, &lossyness), 1);
  EXPECT_EQ(lossyness, static_cast<uint32_t>(JXL_LOSSY));
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_JXL_EFFORT, &effort), 1);
  EXPECT_EQ(effort, 1U);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_JXL_DISTANCE, &distance), 1);
  EXPECT_FLOAT_EQ(distance, 15.0f);
  EXPECT_EQ(
      TIFFGetField(tif.get(), TIFFTAG_JXL_ALPHA_DISTANCE, &alpha_distance), 1);
  EXPECT_FLOAT_EQ(alpha_distance, 15.0f);
}

TEST(TifJxlTest, LosslessRoundTripAllDataTypesAndSchemes) {
  struct TypeCase {
    uint16_t sample_format;
    uint16_t bits_per_sample;
  };
  constexpr std::array<TypeCase, 4> kTypes = {{
      {SAMPLEFORMAT_UINT, 8},
      {SAMPLEFORMAT_UINT, 16},
      {SAMPLEFORMAT_IEEEFP, 16},
      {SAMPLEFORMAT_IEEEFP, 32},
  }};
  constexpr std::array<const char*, 4> kWriteModes = {"wl", "wb", "w8l", "w8b"};

  for (uint16_t scheme : kJxlSchemes) {
    for (const TypeCase& tc : kTypes) {
      for (uint16_t channels : {uint16_t{1}, uint16_t{3}}) {
        for (const char* mode : kWriteModes) {
          SCOPED_TRACE(scheme);
          SCOPED_TRACE(tc.sample_format);
          SCOPED_TRACE(tc.bits_per_sample);
          SCOPED_TRACE(channels);
          SCOPED_TRACE(mode);

          ImageSpec spec;
          spec.width = 8;
          spec.height = 8;
          spec.compression = scheme;
          spec.photometric =
              (channels == 1) ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB;
          spec.bits_per_sample = tc.bits_per_sample;
          spec.sample_format = tc.sample_format;
          spec.samples_per_pixel = channels;
          spec.rows_per_strip = 8;
          spec.write_mode = mode;

          const std::vector<uint8_t> reference =
              MakeDeterministicSamples(spec, 64, channels);

          MemoryTiffStream stream;
          WriteStrippedImage(stream, spec, reference);
          const std::vector<uint8_t> decoded = ReadStrippedImage(stream, spec);
          EXPECT_EQ(decoded, reference);
          EXPECT_EQ(stream.diagnostics().error_count(), 0U);
        }
      }
    }
  }
}

TEST(TifJxlTest, MultiStripScanlineAndPartialLastStrip) {
  ImageSpec spec;
  spec.width = 12;
  spec.height = 10;
  spec.compression = COMPRESSION_JXL;
  spec.photometric = PHOTOMETRIC_RGB;
  spec.bits_per_sample = 8;
  spec.sample_format = SAMPLEFORMAT_UINT;
  spec.samples_per_pixel = 3;
  spec.rows_per_strip = 4;  // Strips of 4, 4, and 2 rows.

  const size_t row_bytes = 12U * 3U;
  const std::vector<uint8_t> reference = MakeDeterministicSamples(spec, 120, 3);

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureCommonTags(writer.get(), spec);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, spec.rows_per_strip),
        1);
    for (uint32_t row = 0; row < spec.height; ++row) {
      std::vector<uint8_t> row_buf(reference.begin() + row * row_bytes,
                                   reference.begin() + (row + 1) * row_bytes);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), row, 0), 1);
    }
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  for (uint32_t row = 0; row < spec.height; ++row) {
    std::vector<uint8_t> got(row_bytes, 0);
    ASSERT_EQ(TIFFReadScanline(reader.get(), got.data(), row, 0), 1);
    std::vector<uint8_t> expected(reference.begin() + row * row_bytes,
                                  reference.begin() + (row + 1) * row_bytes);
    EXPECT_EQ(got, expected) << "row=" << row;
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifJxlTest, GrowingImageLengthReallocatesUncompressedBuffer) {
  ImageSpec spec;
  spec.width = 8;
  spec.height = 2;
  spec.rows_per_strip = 4;
  spec.samples_per_pixel = 3;
  const std::vector<uint8_t> small_strip =
      MakeDeterministicSamples(spec, 16, 3);
  spec.height = 4;
  const std::vector<uint8_t> full_strip = MakeDeterministicSamples(spec, 32, 3);

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    spec.height = 2;
    ConfigureCommonTags(writer.get(), spec);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, spec.rows_per_strip),
        1);
    std::vector<uint8_t> buf0 = small_strip;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf0.data(),
                                    static_cast<tmsize_t>(buf0.size())),
              static_cast<tmsize_t>(buf0.size()));

    // Growing IMAGELENGTH within strip 0 forces SetupUncompressedBuffer
    // realloc.
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 4U), 1);
    std::vector<uint8_t> buf1 = full_strip;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf1.data(),
                                    static_cast<tmsize_t>(buf1.size())),
              static_cast<tmsize_t>(buf1.size()));
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  spec.height = 4;
  EXPECT_EQ(ReadStrippedImage(stream, spec), full_strip);
}

TEST(TifJxlTest, TiledRoundTripContigAndSeparate) {
  for (uint16_t scheme : kJxlSchemes) {
    for (uint16_t planar :
         {uint16_t{PLANARCONFIG_CONTIG}, uint16_t{PLANARCONFIG_SEPARATE}}) {
      SCOPED_TRACE(scheme);
      SCOPED_TRACE(planar);

      ImageSpec spec;
      spec.width = 32;
      spec.height = 32;
      spec.compression = scheme;
      spec.photometric = PHOTOMETRIC_RGB;
      spec.bits_per_sample = 8;
      spec.sample_format = SAMPLEFORMAT_UINT;
      spec.samples_per_pixel = 3;
      spec.planar_config = planar;

      // TIFF tile width and length must be multiples of 16.
      const uint32_t tile_w = 16;
      const uint32_t tile_l = 16;
      const uint16_t planes = (planar == PLANARCONFIG_SEPARATE) ? 3 : 1;
      const uint16_t channels_per_tile =
          (planar == PLANARCONFIG_SEPARATE) ? 1 : 3;
      const size_t tile_bytes =
          static_cast<size_t>(tile_w) * tile_l * channels_per_tile;
      const uint32_t num_tiles = 4U * planes;

      std::vector<std::vector<uint8_t>> tile_refs;
      tile_refs.reserve(num_tiles);
      for (uint32_t t = 0; t < num_tiles; ++t) {
        std::vector<uint8_t> data(tile_bytes);
        for (size_t i = 0; i < tile_bytes; ++i) {
          data[i] = static_cast<uint8_t>((t * 53U + i * 11U) & 0xffU);
        }
        tile_refs.push_back(std::move(data));
      }

      MemoryTiffStream stream;
      {
        UniqueTiff writer = stream.Open("w");
        ASSERT_NE(writer, nullptr);
        ConfigureCommonTags(writer.get(), spec);
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, tile_w), 1);
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, tile_l), 1);
        ASSERT_EQ(TIFFNumberOfTiles(writer.get()), num_tiles);
        for (uint32_t t = 0; t < num_tiles; ++t) {
          std::vector<uint8_t> mutable_tile = tile_refs[t];
          ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), t, mutable_tile.data(),
                                         static_cast<tmsize_t>(tile_bytes)),
                    static_cast<tmsize_t>(tile_bytes));
        }
        ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
      }

      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      ASSERT_EQ(TIFFIsTiled(reader.get()), 1);
      for (uint32_t t = 0; t < num_tiles; ++t) {
        std::vector<uint8_t> got(tile_bytes, 0);
        ASSERT_EQ(TIFFReadEncodedTile(reader.get(), t, got.data(),
                                      static_cast<tmsize_t>(tile_bytes)),
                  static_cast<tmsize_t>(tile_bytes));
        EXPECT_EQ(got, tile_refs[t]) << "tile=" << t;
      }
      EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    }
  }
}

TEST(TifJxlTest, SeparatePlanesStrippedRoundTrip) {
  ImageSpec spec;
  spec.width = 16;
  spec.height = 12;
  spec.compression = COMPRESSION_JXL;
  spec.photometric = PHOTOMETRIC_RGB;
  spec.bits_per_sample = 8;
  spec.sample_format = SAMPLEFORMAT_UINT;
  spec.samples_per_pixel = 3;
  spec.planar_config = PLANARCONFIG_SEPARATE;
  spec.rows_per_strip = 6;

  const std::vector<uint8_t> reference =
      MakeDeterministicSamples(spec, 16U * 12U, 3);
  MemoryTiffStream stream;
  WriteStrippedImage(stream, spec, reference);
  EXPECT_EQ(ReadStrippedImage(stream, spec), reference);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifJxlTest, LossyEffortDistanceAndAlphaDistance) {
  struct LossyCase {
    uint16_t samples;
    std::vector<uint16_t> extra;
    bool lossless;
    uint32_t effort;
    float distance;
    float alpha_distance;
  };
  const std::vector<LossyCase> cases = {
      {3, {}, false, 1U, 1.5f, -1.0f},
      {3, {}, false, 9U, 0.5f, -1.0f},
      {4, {EXTRASAMPLE_UNASSALPHA}, false, 3U, 1.0f, 1.0f},
      {4, {EXTRASAMPLE_UNASSALPHA}, false, 3U, 1.0f, 2.0f},
      {4, {EXTRASAMPLE_UNASSALPHA}, true, 3U, 1.0f, 0.0f},
      {4, {EXTRASAMPLE_UNASSALPHA}, true, 3U, 1.0f, 1.5f},
      {4, {EXTRASAMPLE_UNSPECIFIED}, false, 3U, 1.2f, -1.0f},
      {5,
       {EXTRASAMPLE_UNASSALPHA, EXTRASAMPLE_UNSPECIFIED},
       false,
       3U,
       1.0f,
       2.0f},
  };

  for (size_t idx = 0; idx < cases.size(); ++idx) {
    SCOPED_TRACE(idx);
    const LossyCase& c = cases[idx];
    ImageSpec spec;
    spec.width = 16;
    spec.height = 16;
    spec.photometric = PHOTOMETRIC_RGB;
    spec.samples_per_pixel = c.samples;
    spec.extra_samples = c.extra;
    spec.rows_per_strip = 16;
    spec.lossless = c.lossless;
    spec.effort = c.effort;
    spec.distance = c.distance;
    spec.alpha_distance = c.alpha_distance;

    // Smooth non-wrapping 2D gradient so lossy compression stays accurate.
    std::vector<uint8_t> reference(16U * 16U * c.samples);
    for (uint32_t y = 0; y < 16; ++y) {
      for (uint32_t x = 0; x < 16; ++x) {
        for (uint16_t ch = 0; ch < c.samples; ++ch) {
          const size_t sample_idx = (y * 16U + x) * c.samples + ch;
          reference[sample_idx] = static_cast<uint8_t>(
              std::min<uint32_t>(255U, x * 8U + y * 6U + ch * 12U));
        }
      }
    }

    MemoryTiffStream stream;
    WriteStrippedImage(stream, spec, reference);
    const std::vector<uint8_t> decoded = ReadStrippedImage(stream, spec);
    ASSERT_EQ(decoded.size(), reference.size());
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    if (c.lossless && c.alpha_distance == 0.0f) {
      EXPECT_EQ(decoded, reference);
    } else if (c.lossless && c.alpha_distance > 0.0f) {
      int max_alpha_diff = 0;
      for (size_t i = 0; i < decoded.size(); ++i) {
        if ((i % c.samples) < 3) {
          EXPECT_EQ(decoded[i], reference[i]) << "i=" << i;
        } else {
          max_alpha_diff = std::max(max_alpha_diff,
                                    std::abs(static_cast<int>(decoded[i]) -
                                             static_cast<int>(reference[i])));
        }
      }
      EXPECT_LT(max_alpha_diff, 32);
    } else if (!c.lossless) {
      int max_diff = 0;
      for (size_t i = 0; i < decoded.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(static_cast<int>(decoded[i]) -
                                               static_cast<int>(reference[i])));
      }
      EXPECT_LT(max_diff, 32);
    }
  }
}

TEST(TifJxlTest, ChannelsAndExtraSamplesMatrix) {
  struct ChannelCase {
    uint16_t photometric;
    uint16_t samples;
    std::vector<uint16_t> extra;
    uint16_t sample_format;
    uint16_t bits_per_sample;
  };
  // Use 8x8 (64 pixels) so pixel count exceeds kThreshold in tif_jxl.c's
  // extra-channel reordering loop.
  const std::vector<ChannelCase> cases = {
      // Gray + unassociated alpha (embedded alpha).
      {PHOTOMETRIC_MINISBLACK,
       2,
       {EXTRASAMPLE_UNASSALPHA},
       SAMPLEFORMAT_UINT,
       8},
      // Gray + unspecified extra channel (mainPixSize == 1, 2, 4).
      {PHOTOMETRIC_MINISBLACK,
       2,
       {EXTRASAMPLE_UNSPECIFIED},
       SAMPLEFORMAT_UINT,
       8},
      {PHOTOMETRIC_MINISBLACK,
       2,
       {EXTRASAMPLE_UNSPECIFIED},
       SAMPLEFORMAT_UINT,
       16},
      {PHOTOMETRIC_MINISBLACK,
       2,
       {EXTRASAMPLE_UNSPECIFIED},
       SAMPLEFORMAT_IEEEFP,
       16},
      {PHOTOMETRIC_MINISBLACK,
       2,
       {EXTRASAMPLE_UNSPECIFIED},
       SAMPLEFORMAT_IEEEFP,
       32},
      // Gray + alpha + unspecified (bAlphaEmbedded=1, mainPixSize == 2, 4, 8).
      {PHOTOMETRIC_MINISBLACK,
       3,
       {EXTRASAMPLE_UNASSALPHA, EXTRASAMPLE_UNSPECIFIED},
       SAMPLEFORMAT_UINT,
       8},
      {PHOTOMETRIC_MINISBLACK,
       3,
       {EXTRASAMPLE_UNASSALPHA, EXTRASAMPLE_UNSPECIFIED},
       SAMPLEFORMAT_UINT,
       16},
      {PHOTOMETRIC_MINISBLACK,
       3,
       {EXTRASAMPLE_UNASSALPHA, EXTRASAMPLE_UNSPECIFIED},
       SAMPLEFORMAT_IEEEFP,
       32},
      // RGBA (4 channels).
      {PHOTOMETRIC_RGB, 4, {EXTRASAMPLE_UNASSALPHA}, SAMPLEFORMAT_UINT, 8},
      {PHOTOMETRIC_RGB, 4, {EXTRASAMPLE_UNASSALPHA}, SAMPLEFORMAT_UINT, 16},
      {PHOTOMETRIC_RGB, 4, {EXTRASAMPLE_UNASSALPHA}, SAMPLEFORMAT_IEEEFP, 32},
      // RGB + unspecified (mainPixSize == 3, 6, 12).
      {PHOTOMETRIC_RGB, 4, {EXTRASAMPLE_UNSPECIFIED}, SAMPLEFORMAT_UINT, 8},
      {PHOTOMETRIC_RGB, 4, {EXTRASAMPLE_UNSPECIFIED}, SAMPLEFORMAT_UINT, 16},
      {PHOTOMETRIC_RGB, 4, {EXTRASAMPLE_UNSPECIFIED}, SAMPLEFORMAT_IEEEFP, 32},
      // RGB + associated alpha (exercises alpha_premultiplied = JXL_TRUE).
      {PHOTOMETRIC_RGB, 4, {EXTRASAMPLE_ASSOCALPHA}, SAMPLEFORMAT_UINT, 8},
      // RGB + unassociated alpha + unspecified (mainPixSize == 4, 8, 16).
      {PHOTOMETRIC_RGB,
       5,
       {EXTRASAMPLE_UNASSALPHA, EXTRASAMPLE_UNSPECIFIED},
       SAMPLEFORMAT_UINT,
       8},
      {PHOTOMETRIC_RGB,
       5,
       {EXTRASAMPLE_UNASSALPHA, EXTRASAMPLE_UNSPECIFIED},
       SAMPLEFORMAT_UINT,
       16},
      {PHOTOMETRIC_RGB,
       5,
       {EXTRASAMPLE_UNASSALPHA, EXTRASAMPLE_UNSPECIFIED},
       SAMPLEFORMAT_IEEEFP,
       32},
      // RGB + unspecified + unassociated alpha: alpha_bits != 0 while
      // extra_channel_info[0].type != JXL_CHANNEL_ALPHA (line 375 false
      // branch).
      {PHOTOMETRIC_RGB,
       5,
       {EXTRASAMPLE_UNSPECIFIED, EXTRASAMPLE_UNASSALPHA},
       SAMPLEFORMAT_UINT,
       8},
      // CMYK / SEPARATED 4-channel fallback (1 color + 3 extra).
      {PHOTOMETRIC_SEPARATED, 4, {}, SAMPLEFORMAT_UINT, 8},
  };

  for (size_t idx = 0; idx < cases.size(); ++idx) {
    SCOPED_TRACE(idx);
    const ChannelCase& c = cases[idx];
    ImageSpec spec;
    spec.width = 8;
    spec.height = 8;
    spec.photometric = c.photometric;
    spec.samples_per_pixel = c.samples;
    spec.extra_samples = c.extra;
    spec.sample_format = c.sample_format;
    spec.bits_per_sample = c.bits_per_sample;
    spec.rows_per_strip = 8;

    const std::vector<uint8_t> reference =
        MakeDeterministicSamples(spec, 64, c.samples);
    MemoryTiffStream stream;
    WriteStrippedImage(stream, spec, reference);
    const std::vector<uint8_t> decoded = ReadStrippedImage(stream, spec);
    EXPECT_EQ(decoded, reference);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifJxlTest, RgbaImageReadRoundTrip) {
  ImageSpec spec;
  spec.width = 8;
  spec.height = 8;
  spec.photometric = PHOTOMETRIC_RGB;
  spec.samples_per_pixel = 4;
  spec.extra_samples = {EXTRASAMPLE_UNASSALPHA};
  spec.rows_per_strip = 8;

  std::vector<uint8_t> reference = MakeDeterministicSamples(spec, 64, 4);
  // Make pixel 0 fully opaque because TIFFReadRGBAImageOriented premultiplies
  // RGB by unassociated alpha.
  reference[3] = 255;
  MemoryTiffStream stream;
  WriteStrippedImage(stream, spec, reference);

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint32_t> raster(64, 0);
  ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), 8, 8, raster.data(),
                                      ORIENTATION_TOPLEFT, 1),
            1);
  EXPECT_EQ(TIFFGetR(raster[0]), reference[0]);
  EXPECT_EQ(TIFFGetG(raster[0]), reference[1]);
  EXPECT_EQ(TIFFGetB(raster[0]), reference[2]);
  EXPECT_EQ(TIFFGetA(raster[0]), reference[3]);
}

TEST(TifJxlTest, SmallRawOutputBufferForcesNeedMoreOutputLoopAndWriteFault) {
  ImageSpec spec;
  spec.width = 16;
  spec.height = 16;
  spec.photometric = PHOTOMETRIC_RGB;
  spec.samples_per_pixel = 3;
  spec.rows_per_strip = 16;
  const std::vector<uint8_t> reference = MakeDeterministicSamples(spec, 256, 3);

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureCommonTags(writer.get(), spec);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 16U), 1);
    // Force a tiny output buffer so JxlEncoderProcessOutput loops with
    // JXL_ENC_NEED_MORE_OUTPUT.
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 64), 1);

    std::vector<uint8_t> mutable_strip = reference;
    ASSERT_EQ(
        TIFFWriteEncodedStrip(writer.get(), 0, mutable_strip.data(),
                              static_cast<tmsize_t>(mutable_strip.size())),
        static_cast<tmsize_t>(mutable_strip.size()));
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
  EXPECT_EQ(ReadStrippedImage(stream, spec), reference);

  // Inject a write failure during TIFFFlushData1 inside JXLPostEncode.
  MemoryTiffStream failing_stream;
  {
    UniqueTiff writer = failing_stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureCommonTags(writer.get(), spec);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 16U), 1);
    failing_stream.FailWritesAfter(failing_stream.write_calls());
    std::vector<uint8_t> mutable_strip = reference;
    EXPECT_EQ(
        TIFFWriteEncodedStrip(writer.get(), 0, mutable_strip.data(),
                              static_cast<tmsize_t>(mutable_strip.size())),
        static_cast<tmsize_t>(-1));
    // Reset faults before writer closes so TIFFClose finishes cleanly.
    failing_stream.ResetFaults();
  }
}

TEST(TifJxlTest, EncodeErrorPathsUnsupportedTypesAndByteCountMismatch) {
  struct InvalidTypeCase {
    uint16_t sample_format;
    uint16_t bits_per_sample;
  };
  constexpr std::array<InvalidTypeCase, 6> kInvalidTypes = {{
      {SAMPLEFORMAT_INT, 8},
      {SAMPLEFORMAT_INT, 16},
      {SAMPLEFORMAT_INT, 32},
      {SAMPLEFORMAT_UINT, 32},
      {SAMPLEFORMAT_IEEEFP, 64},
      {SAMPLEFORMAT_VOID, 8},
  }};

  for (const InvalidTypeCase& tc : kInvalidTypes) {
    SCOPED_TRACE(tc.sample_format);
    SCOPED_TRACE(tc.bits_per_sample);

    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, 4U), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 4U), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JXL),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 4U), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, tc.bits_per_sample),
        1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, tc.sample_format), 1);

    std::vector<uint8_t> buf(4U * 4U * (tc.bits_per_sample / 8U), 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                    static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(-1));
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors().back().message,
                HasSubstr("Unsupported combination"));
  }

  // JXLEncode "Too many bytes written" and JXLPostEncode "Unexpected number
  // of bytes in the buffer".
  auto setup_4x4_writer = [](MemoryTiffStream& stream) -> UniqueTiff {
    UniqueTiff writer = stream.Open("w");
    if (writer == nullptr) return nullptr;
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, 4U), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 4U), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JXL),
              1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 4U), 1);
    return writer;
  };

  {
    MemoryTiffStream stream;
    UniqueTiff writer = setup_4x4_writer(stream);
    ASSERT_NE(writer, nullptr);
    std::vector<uint8_t> too_big(32, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, too_big.data(), 32),
              static_cast<tmsize_t>(-1));
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors().back().message,
                HasSubstr("Too many bytes written"));
  }
  {
    MemoryTiffStream stream;
    UniqueTiff writer = setup_4x4_writer(stream);
    ASSERT_NE(writer, nullptr);
    std::vector<uint8_t> too_small(8, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, too_small.data(), 8),
              static_cast<tmsize_t>(-1));
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors().back().message,
                HasSubstr("Unexpected number of bytes in the buffer"));
  }

  // Trigger JxlEncoderSetBasicInfo() failure in JXLPostEncode when
  // num_extra_channels exceeds libjxl's supported maximum (259 > 256).
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, 1U), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 1U), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JXL),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, 260), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 1U), 1);

    std::vector<uint8_t> pixel(260, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, pixel.data(),
                                    static_cast<tmsize_t>(pixel.size())),
              static_cast<tmsize_t>(-1));
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors().back().message,
                HasSubstr("JxlEncoderSetBasicInfo() failed"));
  }

  // Trigger JxlEncoderAddImageFrame() failure in JXLPostEncode by combining
  // JXL_LOSSY (uses_original_profile = false) with DISTANCE = 0.0 (which sets
  // lossless = true inside libjxl frame settings).
  {
    MemoryTiffStream stream;
    UniqueTiff writer = setup_4x4_writer(stream);
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JXL_LOSSYNESS,
                           static_cast<uint32_t>(JXL_LOSSY)),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JXL_DISTANCE, 0.0f), 1);

    std::vector<uint8_t> strip(16, 42);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                    static_cast<tmsize_t>(strip.size())),
              static_cast<tmsize_t>(-1));
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors().back().message,
                HasSubstr("JxlEncoderAddImageFrame() failed"));
  }

  // Trigger JxlEncoderProcessOutput() failure in JXLPostEncode via modular
  // int32 residual overflow when losslessly encoding IEEE 32-bit float pixels
  // transitioning between -1.0f (0xBF800000 = -1082130432) and +1.0f
  // (0x3F800000 = +1065353216), whose difference exceeds INT32_MAX.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, 2U), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 1U), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JXL),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, 32), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 1U), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JXL_LOSSYNESS,
                           static_cast<uint32_t>(JXL_LOSSLESS)),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JXL_EFFORT, 1U), 1);

    std::array<float, 2> step_floats = {-1.0f, 1.0f};
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, step_floats.data(),
                                    sizeof(step_floats)),
              static_cast<tmsize_t>(-1));
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors().back().message,
                HasSubstr("JxlEncoderProcessOutput() failed"));
  }
}

TEST(TifJxlTest, DecodeErrorPathsCorruptTruncatedAndMismatchedMetadata) {
  ImageSpec rgb_spec;
  rgb_spec.width = 8;
  rgb_spec.height = 8;
  rgb_spec.photometric = PHOTOMETRIC_RGB;
  rgb_spec.samples_per_pixel = 3;
  rgb_spec.rows_per_strip = 8;
  const std::vector<uint8_t> rgb_pixels =
      MakeDeterministicSamples(rgb_spec, 64, 3);
  const std::vector<uint8_t> valid_rgb_jxl =
      ExtractSingleRawStrip(rgb_spec, rgb_pixels);
  ASSERT_GT(valid_rgb_jxl.size(), 20U);

  ImageSpec extra5_spec = rgb_spec;
  extra5_spec.samples_per_pixel = 5;
  extra5_spec.extra_samples = {EXTRASAMPLE_UNASSALPHA, EXTRASAMPLE_UNSPECIFIED};
  const std::vector<uint8_t> extra5_pixels =
      MakeDeterministicSamples(extra5_spec, 64, 5);
  const std::vector<uint8_t> valid_extra5_jxl =
      ExtractSingleRawStrip(extra5_spec, extra5_pixels);
  ASSERT_GT(valid_extra5_jxl.size(), 20U);

  // Test garbage and progressively truncated JXL codestreams to trigger all
  // JxlDecoderProcessInput failure phases (BASIC_INFO, NEED_IMAGE_OUT_BUFFER,
  // FULL_IMAGE).
  auto test_corrupt_strip = [](const std::vector<uint8_t>& blob,
                               uint16_t samples,
                               const std::vector<uint16_t>& extra) {
    RawTiffBuilder builder;
    const uint64_t strip_off = builder.AppendBlob(blob);
    std::vector<RawIfdEntry> entries = {
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 8),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JXL),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE,
                               std::vector<uint16_t>(samples, 8)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                         static_cast<uint32_t>(strip_off)),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(blob.size())),
    };
    if (!extra.empty()) {
      entries.push_back(builder.MakeShortArray(TIFFTAG_EXTRASAMPLES, extra));
    }
    builder.AddIfd(std::move(entries));

    MemoryTiffStream stream(builder.Take());
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> out(64U * samples, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  };

  const std::vector<uint8_t> kGarbage = {0xde, 0xad, 0xbe, 0xef, 0x12, 0x34};
  test_corrupt_strip(kGarbage, 3, {});

  for (size_t cut :
       {size_t{2}, size_t{4}, size_t{6}, size_t{8}, size_t{10}, size_t{12},
        size_t{16}, valid_rgb_jxl.size() / 2, valid_rgb_jxl.size() - 1}) {
    if (cut < valid_rgb_jxl.size()) {
      SCOPED_TRACE(cut);
      test_corrupt_strip(std::vector<uint8_t>(valid_rgb_jxl.begin(),
                                              valid_rgb_jxl.begin() + cut),
                         3, {});
    }
  }
  for (size_t cut :
       {size_t{8}, size_t{12}, size_t{20}, valid_extra5_jxl.size() / 2,
        valid_extra5_jxl.size() - 1}) {
    if (cut < valid_extra5_jxl.size()) {
      SCOPED_TRACE(cut);
      test_corrupt_strip(std::vector<uint8_t>(valid_extra5_jxl.begin(),
                                              valid_extra5_jxl.begin() + cut),
                         5, {EXTRASAMPLE_UNASSALPHA, EXTRASAMPLE_UNSPECIFIED});
    }
  }

  // Metadata vs JXL basic_info mismatches.
  struct MismatchCase {
    uint32_t width = 8;
    uint32_t height = 8;
    uint16_t bps = 8;
    uint16_t sample_format = SAMPLEFORMAT_UINT;
    uint16_t samples = 3;
    uint16_t planar = PLANARCONFIG_CONTIG;
    uint16_t photometric = PHOTOMETRIC_RGB;
    const std::vector<uint8_t>* blob = nullptr;
  };

  ImageSpec gray_alpha_spec;
  gray_alpha_spec.width = 8;
  gray_alpha_spec.height = 8;
  gray_alpha_spec.photometric = PHOTOMETRIC_MINISBLACK;
  gray_alpha_spec.samples_per_pixel = 2;
  gray_alpha_spec.extra_samples = {EXTRASAMPLE_UNASSALPHA};
  const std::vector<uint8_t> valid_ga_jxl = ExtractSingleRawStrip(
      gray_alpha_spec, MakeDeterministicSamples(gray_alpha_spec, 64, 2));
  ASSERT_FALSE(valid_ga_jxl.empty());

  const std::vector<MismatchCase> mismatches = {
      // Unsupported SampleFormat on decode (GetJXLDataType < 0).
      {8, 8, 8, SAMPLEFORMAT_INT, 3, PLANARCONFIG_CONTIG, PHOTOMETRIC_RGB,
       &valid_rgb_jxl},
      // Width mismatch (segment_width != info.xsize).
      {16, 8, 8, SAMPLEFORMAT_UINT, 3, PLANARCONFIG_CONTIG, PHOTOMETRIC_RGB,
       &valid_rgb_jxl},
      // Height mismatch (segment_height != info.ysize).
      {8, 16, 8, SAMPLEFORMAT_UINT, 3, PLANARCONFIG_CONTIG, PHOTOMETRIC_RGB,
       &valid_rgb_jxl},
      // BitsPerSample mismatch (td_bitspersample != info.bits_per_sample).
      {8, 8, 16, SAMPLEFORMAT_UINT, 3, PLANARCONFIG_CONTIG, PHOTOMETRIC_RGB,
       &valid_rgb_jxl},
      // PLANARCONFIG_CONTIG channel mismatch (1 vs 3).
      {8, 8, 8, SAMPLEFORMAT_UINT, 1, PLANARCONFIG_CONTIG,
       PHOTOMETRIC_MINISBLACK, &valid_rgb_jxl},
      // PLANARCONFIG_SEPARATE with 3-channel JXL codestream.
      {8, 8, 8, SAMPLEFORMAT_UINT, 3, PLANARCONFIG_SEPARATE, PHOTOMETRIC_RGB,
       &valid_rgb_jxl},
      // PLANARCONFIG_SEPARATE with Gray+Alpha JXL codestream.
      {8, 8, 8, SAMPLEFORMAT_UINT, 2, PLANARCONFIG_SEPARATE,
       PHOTOMETRIC_MINISBLACK, &valid_ga_jxl},
  };

  for (size_t i = 0; i < mismatches.size(); ++i) {
    SCOPED_TRACE(i);
    const MismatchCase& m = mismatches[i];
    RawTiffBuilder builder;
    const uint64_t strip_off = builder.AppendBlob(*m.blob);
    const uint32_t strips = (m.planar == PLANARCONFIG_SEPARATE) ? m.samples : 1;
    std::vector<uint32_t> offsets(strips, static_cast<uint32_t>(strip_off));
    std::vector<uint32_t> counts(strips, static_cast<uint32_t>(m.blob->size()));
    std::vector<RawIfdEntry> entries = {
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, m.width),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, m.height),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JXL),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, m.photometric),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE,
                               std::vector<uint16_t>(m.samples, m.bps)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, m.samples),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, m.planar),
        builder.MakeShort(TIFFTAG_SAMPLEFORMAT, m.sample_format),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, m.height),
        builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offsets),
        builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, counts),
    };
    builder.AddIfd(std::move(entries));

    MemoryTiffStream stream(builder.Take());
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    const size_t buf_bytes =
        static_cast<size_t>(m.width) * m.height * m.samples * (m.bps / 8U);
    std::vector<uint8_t> out(buf_bytes, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // Overflow guard in SetupUncompressedBuffer ("Too large uncompressed
  // strip/tile"): 65536 x 65536 8-bit 1-channel tile exceeds 32-bit alloc.
  {
    RawTiffBuilder builder;
    const std::array<uint8_t, 8> dummy_tile = {0xff, 0x0a, 0x00, 0x00,
                                               0x00, 0x00, 0x00, 0x00};
    const uint64_t tile_off = builder.AppendBlob(dummy_tile);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 65536),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 65536),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JXL),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        builder.MakeLong(TIFFTAG_TILEWIDTH, 65536),
        builder.MakeLong(TIFFTAG_TILELENGTH, 65536),
        builder.MakeLong(TIFFTAG_TILEOFFSETS, static_cast<uint32_t>(tile_off)),
        builder.MakeLong(TIFFTAG_TILEBYTECOUNTS, dummy_tile.size()),
    });

    MemoryTiffStream stream(builder.Take());
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    std::array<uint8_t, 16> small_buf{};
    EXPECT_EQ(TIFFReadEncodedTile(tif.get(), 0, small_buf.data(),
                                  static_cast<tmsize_t>(small_buf.size())),
              static_cast<tmsize_t>(-1));
    const auto errors = stream.diagnostics().errors();
    ASSERT_FALSE(errors.empty());
    EXPECT_THAT(errors.back().message,
                HasSubstr("Too large uncompressed strip/tile"));
  }

  // JXLDecode "Too many bytes read" via TIFFReadFromUserBuffer with outsize
  // larger than sp->uncompressed_size.
  {
    MemoryTiffStream stream;
    WriteStrippedImage(stream, rgb_spec, rgb_pixels);
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> in_copy = valid_rgb_jxl;
    std::vector<uint8_t> big_out(rgb_pixels.size() + 64, 0);
    EXPECT_EQ(TIFFReadFromUserBuffer(reader.get(), 0, in_copy.data(),
                                     static_cast<tmsize_t>(in_copy.size()),
                                     big_out.data(),
                                     static_cast<tmsize_t>(big_out.size())),
              0);
    ASSERT_FALSE(stream.diagnostics().errors().empty());
    EXPECT_THAT(stream.diagnostics().errors().back().message,
                HasSubstr("Too many bytes read"));
  }
}

TEST(TifJxlTest, ReadWriteModeSwitchingOnSameHandle) {
  ImageSpec spec;
  spec.width = 8;
  spec.height = 8;
  spec.photometric = PHOTOMETRIC_RGB;
  spec.samples_per_pixel = 3;
  spec.rows_per_strip = 4;  // 2 strips.

  const std::vector<uint8_t> initial = MakeDeterministicSamples(spec, 64, 3);
  MemoryTiffStream stream;
  WriteStrippedImage(stream, spec, initial);

  const size_t strip_bytes = 8U * 4U * 3U;
  std::vector<uint8_t> replacement(strip_bytes, 0x5a);

  // Decode -> Encode -> Decode on the same handle exercises JXLSetupEncode and
  // JXLSetupDecode state-flag transitions.
  {
    UniqueTiff rw = stream.Open("r+");
    ASSERT_NE(rw, nullptr);
    std::vector<uint8_t> strip0(strip_bytes, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, strip0.data(),
                                   static_cast<tmsize_t>(strip_bytes)),
              static_cast<tmsize_t>(strip_bytes));
    EXPECT_EQ(
        std::vector<uint8_t>(initial.begin(), initial.begin() + strip_bytes),
        strip0);

    std::vector<uint8_t> mutable_rep = replacement;
    ASSERT_EQ(TIFFWriteEncodedStrip(rw.get(), 1, mutable_rep.data(),
                                    static_cast<tmsize_t>(strip_bytes)),
              static_cast<tmsize_t>(strip_bytes));

    std::fill(strip0.begin(), strip0.end(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, strip0.data(),
                                   static_cast<tmsize_t>(strip_bytes)),
              static_cast<tmsize_t>(strip_bytes));
    EXPECT_EQ(
        std::vector<uint8_t>(initial.begin(), initial.begin() + strip_bytes),
        strip0);
    ASSERT_EQ(TIFFRewriteDirectory(rw.get()), 1);
  }

  const std::vector<uint8_t> updated = ReadStrippedImage(stream, spec);
  ASSERT_EQ(updated.size(), initial.size());
  EXPECT_EQ(std::vector<uint8_t>(updated.begin() + strip_bytes, updated.end()),
            replacement);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifJxlTest, AllocationLimitsAndOomPaths) {
  // 1. OOM inside TIFFInitJXL when allocating JXLState via _TIFFcallocExt
  // while leaving enough budget for _TIFFMergeFields (+32 bytes).
  {
    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w", opts.get());
    ASSERT_NE(tif, nullptr);
    ScopedCumulatedMemAllocRemaining tight_budget(tif.get(), 40);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JXL), 0);
    EXPECT_GE(recorder.error_count(), 1U);
  }

  // 2. OOM inside SetupUncompressedBuffer on encode.
  {
    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w", opts.get());
    ASSERT_NE(writer, nullptr);
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.rows_per_strip = 64;
    ConfigureCommonTags(writer.get(), spec);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 64U), 1);

    // Pre-allocate the raw output buffer so BUFFERCHECK succeeds and OOM hits
    // JXLPreEncode -> SetupUncompressedBuffer directly.
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 1024), 1);

    std::vector<uint8_t> buf(64U * 64U * 3U, 0);
    ScopedSingleMemAllocLimit limit(writer.get(), 4000);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                    static_cast<tmsize_t>(buf.size())),
              static_cast<tmsize_t>(-1));
    ASSERT_FALSE(recorder.errors().empty());
    EXPECT_THAT(recorder.errors().back().message,
                HasSubstr("Cannot allocate buffer"));
  }

  // 3. OOM inside SetupUncompressedBuffer on decode.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.rows_per_strip = 64;
    // Uniform pixels compress to a tiny JXL strip (< 1 KB) while
    // uncompressed_buffer requires ~16 KB.
    std::vector<uint8_t> solid(64U * 64U * 3U, 42);
    MemoryTiffStream stream;
    WriteStrippedImage(stream, spec, solid);

    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    UniqueTiff reader = stream.Open("r", opts.get());
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(solid.size(), 0);
    ScopedSingleMemAllocLimit limit(reader.get(), 4096);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_GE(recorder.error_count(), 1U);
  }

  // 4. OOM when allocating extra_channel_buffer in JXLPreDecode on strip 1
  // after strip 0 has already allocated uncompressed_buffer.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.photometric = PHOTOMETRIC_RGB;
    spec.samples_per_pixel = 4;
    spec.extra_samples = {EXTRASAMPLE_UNSPECIFIED};
    spec.rows_per_strip = 16;  // 2 strips; each extra channel is 512 bytes.
    std::vector<uint8_t> solid(32U * 32U * 4U, 128);
    MemoryTiffStream stream;
    WriteStrippedImage(stream, spec, solid);

    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    UniqueTiff reader = stream.Open("r", opts.get());
    ASSERT_NE(reader, nullptr);
    const size_t strip_bytes = 32U * 16U * 4U;
    std::vector<uint8_t> strip0(strip_bytes, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip0.data(),
                                   static_cast<tmsize_t>(strip_bytes)),
              static_cast<tmsize_t>(strip_bytes));

    // Reuse existing uncompressed_buffer (~2830 bytes) and tif_rawdata (>=1024)
    // while failing extra_channel_buffer (512 bytes).
    std::vector<uint8_t> strip1(strip_bytes, 0);
    ScopedSingleMemAllocLimit limit(reader.get(), 400);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 1, strip1.data(),
                                   static_cast<tmsize_t>(strip_bytes)),
              static_cast<tmsize_t>(-1));
    EXPECT_GE(recorder.error_count(), 1U);
  }
}

}  // namespace
}  // namespace libtiff
