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

// Clean-room C++20 unit tests for third_party/tiff/libtiff/tif_lerc.c.
//
// google3 caveats:
//   - LERC_SUPPORT is defined only in internal tif_config.h (not public
//     tiffconf.h), so tests verify codec availability at runtime via
//     TIFFIsCODECConfigured(COMPRESSION_LERC).
//   - LIBDEFLATE_SUPPORT is disabled in google3; LERC_ADD_COMPRESSION_DEFLATE
//     uses zlib_rs, while LERC_ADD_COMPRESSION_ZSTD is enabled.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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

constexpr bool ContainsMessage(absl::Span<const DiagnosticRecord> records,
                               std::string_view needle) {
  for (const DiagnosticRecord& rec : records) {
    if (rec.message.find(needle) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

template <typename T>
void WriteStripAndClose(MemoryTiffStream& stream, const char* mode,
                        uint32_t width, uint32_t height,
                        uint32_t rows_per_strip, uint16_t bps, uint16_t spp,
                        uint16_t sample_format, uint16_t planar_config,
                        int add_compression, double max_z_error,
                        absl::Span<const T> pixels,
                        bool set_unass_alpha = false) {
  UniqueTiff writer = stream.Open(mode);
  ASSERT_NE(writer.get(), nullptr);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, bps), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, spp), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, sample_format), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                         spp >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK),
            1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, rows_per_strip),
            1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC),
            1);
  ASSERT_EQ(
      TIFFSetField(writer.get(), TIFFTAG_LERC_ADD_COMPRESSION, add_compression),
      1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_LERC_MAXZERROR, max_z_error), 1);
  if (set_unass_alpha && spp >= 2) {
    const uint16_t extra = EXTRASAMPLE_UNASSALPHA;
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_EXTRASAMPLES, uint16_t{1}, &extra),
        1);
  } else if (spp == 2) {
    const uint16_t extra = EXTRASAMPLE_UNSPECIFIED;
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_EXTRASAMPLES, uint16_t{1}, &extra),
        1);
  }

  const tmsize_t total_strips = TIFFNumberOfStrips(writer.get());
  size_t byte_offset = 0;
  std::vector<uint8_t> raw_bytes(pixels.size() * sizeof(T));
  if (!raw_bytes.empty()) {
    std::memcpy(raw_bytes.data(), pixels.data(), raw_bytes.size());
  }

  for (uint32_t s = 0; s < static_cast<uint32_t>(total_strips); ++s) {
    const tmsize_t strip_size = TIFFStripSize(writer.get());
    // Adjust final strip length in case rows_per_strip does not divide height.
    uint32_t strip_rows = rows_per_strip;
    const uint32_t strips_per_plane =
        (height + rows_per_strip - 1) / rows_per_strip;
    const uint32_t strip_in_plane = s % strips_per_plane;
    if ((strip_in_plane + 1) * rows_per_strip > height) {
      strip_rows = height - strip_in_plane * rows_per_strip;
    }
    const size_t expected_bytes =
        static_cast<size_t>(TIFFScanlineSize(writer.get())) * strip_rows;
    ASSERT_LE(expected_bytes, static_cast<size_t>(strip_size));
    ASSERT_LE(byte_offset + expected_bytes, raw_bytes.size());
    std::vector<uint8_t> mutable_strip(
        raw_bytes.begin() + static_cast<std::ptrdiff_t>(byte_offset),
        raw_bytes.begin() +
            static_cast<std::ptrdiff_t>(byte_offset + expected_bytes));
    ASSERT_EQ(
        TIFFWriteEncodedStrip(writer.get(), s, mutable_strip.data(),
                              static_cast<tmsize_t>(mutable_strip.size())),
        static_cast<tmsize_t>(mutable_strip.size()));
    byte_offset += expected_bytes;
  }
  ASSERT_EQ(byte_offset, raw_bytes.size());
  writer.reset();
}

template <typename T>
std::vector<T> ReadAllStrips(MemoryTiffStream& stream) {
  UniqueTiff reader = stream.Open("r");
  EXPECT_NE(reader.get(), nullptr);
  if (reader == nullptr) {
    return {};
  }
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t spp = 0;
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGELENGTH, &height), 1);
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_SAMPLESPERPIXEL, &spp), 1);

  const uint32_t total_strips = TIFFNumberOfStrips(reader.get());
  std::vector<uint8_t> out_bytes;
  const tmsize_t max_strip = TIFFStripSize(reader.get());
  std::vector<uint8_t> strip_buf(static_cast<size_t>(max_strip));
  for (uint32_t s = 0; s < total_strips; ++s) {
    const tmsize_t read_bytes = TIFFReadEncodedStrip(
        reader.get(), s, strip_buf.data(), static_cast<tmsize_t>(-1));
    EXPECT_GT(read_bytes, 0);
    if (read_bytes <= 0) {
      return {};
    }
    out_bytes.insert(out_bytes.end(), strip_buf.begin(),
                     strip_buf.begin() + read_bytes);
  }
  std::vector<T> result(out_bytes.size() / sizeof(T));
  if (!out_bytes.empty()) {
    std::memcpy(result.data(), out_bytes.data(), out_bytes.size());
  }
  return result;
}

template <typename T>
std::vector<uint8_t> EncodeSingleStripAndExtractRawBlob(
    uint32_t width, uint32_t height, uint16_t bps, uint16_t spp,
    uint16_t sample_format, uint16_t planar_config, int add_compression,
    absl::Span<const T> pixels, bool set_unass_alpha = false) {
  MemoryTiffStream stream;
  WriteStripAndClose<T>(stream, "wl", width, height, height, bps, spp,
                        sample_format, planar_config, add_compression, 0.0,
                        pixels, set_unass_alpha);
  UniqueTiff reader = stream.Open("r");
  EXPECT_NE(reader.get(), nullptr);
  if (reader == nullptr) {
    return {};
  }
  const tmsize_t raw_size = TIFFRawStripSize(reader.get(), 0);
  EXPECT_GT(raw_size, 0);
  std::vector<uint8_t> blob(static_cast<size_t>(raw_size));
  EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, blob.data(), raw_size), raw_size);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  EXPECT_EQ(stream.diagnostics().warning_count(), 0U)
      << (stream.diagnostics().warnings().empty()
              ? ""
              : stream.diagnostics().warnings()[0].message);
  return blob;
}

std::vector<uint8_t> BuildRawLercStripTiff(
    uint32_t width, uint32_t height, uint16_t bps, uint16_t spp,
    uint16_t sample_format, uint16_t planar_config,
    absl::Span<const uint8_t> strip_blob,
    absl::Span<const uint32_t> lerc_params = {},
    absl::Span<const uint16_t> extra_samples = {}, bool big_endian = false,
    bool big_tiff = false) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const uint64_t blob_offset = builder.AppendBlob(strip_blob);

  std::vector<uint16_t> bps_vec(spp, bps);
  std::vector<RawIfdEntry> entries;
  entries.push_back(builder.MakeLong(TIFFTAG_IMAGEWIDTH, width));
  entries.push_back(builder.MakeLong(TIFFTAG_IMAGELENGTH, height));
  entries.push_back(builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps_vec));
  entries.push_back(builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_LERC));
  entries.push_back(
      builder.MakeShort(TIFFTAG_PHOTOMETRIC,
                        spp >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK));
  if (big_tiff) {
    entries.push_back(builder.MakeLong8(TIFFTAG_STRIPOFFSETS, blob_offset));
  } else {
    entries.push_back(builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                                       static_cast<uint32_t>(blob_offset)));
  }
  entries.push_back(builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, spp));
  entries.push_back(builder.MakeLong(TIFFTAG_ROWSPERSTRIP, height));
  entries.push_back(builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                                     static_cast<uint32_t>(strip_blob.size())));
  entries.push_back(builder.MakeShort(TIFFTAG_PLANARCONFIG, planar_config));
  entries.push_back(builder.MakeShort(TIFFTAG_SAMPLEFORMAT, sample_format));
  if (!extra_samples.empty()) {
    entries.push_back(
        builder.MakeShortArray(TIFFTAG_EXTRASAMPLES, extra_samples));
  } else if (spp == 2) {
    const std::array<uint16_t, 1> extra = {EXTRASAMPLE_UNSPECIFIED};
    entries.push_back(builder.MakeShortArray(TIFFTAG_EXTRASAMPLES, extra));
  }
  if (!lerc_params.empty()) {
    entries.push_back(
        builder.MakeLongArray(TIFFTAG_LERC_PARAMETERS, lerc_params));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

TEST(TifLercTest, CodecConfiguredAtRuntime) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_LERC), 1);
  const TIFFCodec* codec = TIFFFindCODEC(COMPRESSION_LERC);
  ASSERT_NE(codec, nullptr);
  EXPECT_EQ(codec->scheme, COMPRESSION_LERC);
}

TEST(TifLercTest, TagGetSetValidationAndDefaults) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("wl");
  ASSERT_NE(tif.get(), nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC), 1);

  int version = -1;
  int add_comp = -1;
  double maxzerror = -1.0;
  int zstd_level = -1;
  int zip_quality = -99;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_LERC_VERSION, &version), 1);
  EXPECT_EQ(version, LERC_VERSION_2_4);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_LERC_ADD_COMPRESSION, &add_comp),
            1);
  EXPECT_EQ(add_comp, LERC_ADD_COMPRESSION_NONE);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_LERC_MAXZERROR, &maxzerror), 1);
  EXPECT_DOUBLE_EQ(maxzerror, 0.0);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_ZSTD_LEVEL, &zstd_level), 1);
  EXPECT_EQ(zstd_level, 9);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_ZIPQUALITY, &zip_quality), 1);
  EXPECT_EQ(zip_quality, -1);

  uint32_t param_count = 0;
  const void* param_ptr = nullptr;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_LERC_PARAMETERS, &param_count,
                         &param_ptr),
            1);
  ASSERT_EQ(param_count, 2U);
  ASSERT_NE(param_ptr, nullptr);
  std::array<uint32_t, 2> params_out{};
  std::memcpy(params_out.data(), param_ptr, sizeof(params_out));
  EXPECT_EQ(params_out[0], static_cast<uint32_t>(LERC_VERSION_2_4));
  EXPECT_EQ(params_out[1], static_cast<uint32_t>(LERC_ADD_COMPRESSION_NONE));

  // Valid sets and reads.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_LERC_MAXZERROR, 0.25), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_LERC_MAXZERROR, &maxzerror), 1);
  EXPECT_DOUBLE_EQ(maxzerror, 0.25);

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_LERC_ADD_COMPRESSION,
                         LERC_ADD_COMPRESSION_DEFLATE),
            1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_LERC_ADD_COMPRESSION, &add_comp),
            1);
  EXPECT_EQ(add_comp, LERC_ADD_COMPRESSION_DEFLATE);

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_LERC_ADD_COMPRESSION,
                         LERC_ADD_COMPRESSION_ZSTD),
            1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_LERC_ADD_COMPRESSION, &add_comp),
            1);
  EXPECT_EQ(add_comp, LERC_ADD_COMPRESSION_ZSTD);

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZIPQUALITY, 12), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_ZIPQUALITY, &zip_quality), 1);
  EXPECT_EQ(zip_quality, 12);

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZSTD_LEVEL, 3), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_ZSTD_LEVEL, &zstd_level), 1);
  EXPECT_EQ(zstd_level, 3);

  // Set TIFFTAG_LERC_PARAMETERS directly with valid count 2.
  int raw_params[2] = {LERC_VERSION_2_4, LERC_ADD_COMPRESSION_DEFLATE};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_LERC_PARAMETERS, 2, raw_params), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_LERC_ADD_COMPRESSION, &add_comp),
            1);
  EXPECT_EQ(add_comp, LERC_ADD_COMPRESSION_DEFLATE);

  // Set invalid values and check diagnostics without stderr spam.
  int bad_param[1] = {LERC_VERSION_2_4};
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_LERC_PARAMETERS, 1, bad_param), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_LERC_VERSION, 3), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_LERC_ADD_COMPRESSION, 77), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZIPQUALITY, -2), 0);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZIPQUALITY, 13), 0);

  // Out-of-range ZSTD_LEVEL warns but still returns 1.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZSTD_LEVEL, 0), 1);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZSTD_LEVEL, 9999), 1);

  EXPECT_TRUE(ContainsMessage(stream.diagnostics().errors(),
                              "Invalid count for LercParameters"));
  EXPECT_TRUE(ContainsMessage(stream.diagnostics().errors(),
                              "Invalid value for LercVersion"));
  EXPECT_TRUE(ContainsMessage(stream.diagnostics().errors(),
                              "Invalid value for LercAdditionalCompression"));
  EXPECT_TRUE(
      ContainsMessage(stream.diagnostics().errors(), "Invalid ZipQuality"));
  EXPECT_TRUE(ContainsMessage(stream.diagnostics().warnings(),
                              "ZSTD_LEVEL should be between"));
}

TEST(TifLercTest, IntegerAndFloatAllDataTypesRoundTripContigAndSeparate) {
  constexpr uint32_t kW = 5;
  constexpr uint32_t kH = 4;

  // int8 contig
  {
    MemoryTiffStream s;
    std::vector<int8_t> in(kW * kH * 2);
    for (size_t i = 0; i < in.size(); ++i) {
      in[i] = static_cast<int8_t>(-60 + static_cast<int>(i) * 3);
    }
    WriteStripAndClose<int8_t>(s, "wl", kW, kH, kH, 8, 2, SAMPLEFORMAT_INT,
                               PLANARCONFIG_CONTIG, LERC_ADD_COMPRESSION_NONE,
                               0.0, in);
    EXPECT_EQ((ReadAllStrips<int8_t>(s)), in);
    EXPECT_EQ(s.diagnostics().error_count(), 0);
  }

  // uint8 separate + deflate (with ZIPQUALITY=12 clamped internally to 9)
  {
    MemoryTiffStream s;
    std::vector<uint8_t> in(kW * kH * 3);
    for (size_t i = 0; i < in.size(); ++i) {
      in[i] = static_cast<uint8_t>((i * 19) & 0xff);
    }
    {
      UniqueTiff writer = s.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                             uint16_t{SAMPLEFORMAT_UINT}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_SEPARATE}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_RGB}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kH), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_LERC_ADD_COMPRESSION,
                             LERC_ADD_COMPRESSION_DEFLATE),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ZIPQUALITY, 12), 1);
      for (uint32_t p = 0; p < 3; ++p) {
        std::vector<uint8_t> plane(
            in.begin() + static_cast<std::ptrdiff_t>(p * kW * kH),
            in.begin() + static_cast<std::ptrdiff_t>((p + 1) * kW * kH));
        ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), p, plane.data(),
                                        static_cast<tmsize_t>(plane.size())),
                  static_cast<tmsize_t>(plane.size()));
      }
    }
    EXPECT_EQ((ReadAllStrips<uint8_t>(s)), in);
    EXPECT_EQ(s.diagnostics().error_count(), 0);
  }

  // int16 little endian & big endian
  for (std::string_view mode : {"wl", "wb"}) {
    SCOPED_TRACE(mode);
    MemoryTiffStream s;
    std::vector<int16_t> in(kW * kH);
    for (size_t i = 0; i < in.size(); ++i) {
      in[i] = static_cast<int16_t>(-5000 + static_cast<int>(i) * 431);
    }
    WriteStripAndClose<int16_t>(s, mode.data(), kW, kH, kH, 16, 1,
                                SAMPLEFORMAT_INT, PLANARCONFIG_CONTIG,
                                LERC_ADD_COMPRESSION_ZSTD, 0.0, in);
    EXPECT_EQ((ReadAllStrips<int16_t>(s)), in);
    EXPECT_EQ(s.diagnostics().error_count(), 0);
  }

  // uint16 big endian + BigTIFF
  {
    MemoryTiffStream s;
    std::vector<uint16_t> in(kW * kH);
    for (size_t i = 0; i < in.size(); ++i) {
      in[i] = static_cast<uint16_t>(100 + i * 997);
    }
    WriteStripAndClose<uint16_t>(s, "w8b", kW, kH, kH, 16, 1, SAMPLEFORMAT_UINT,
                                 PLANARCONFIG_CONTIG, LERC_ADD_COMPRESSION_NONE,
                                 0.0, in);
    EXPECT_EQ((ReadAllStrips<uint16_t>(s)), in);
    EXPECT_EQ(s.diagnostics().error_count(), 0);
  }

  // int32
  {
    MemoryTiffStream s;
    std::vector<int32_t> in(kW * kH);
    for (size_t i = 0; i < in.size(); ++i) {
      in[i] = -100000 + static_cast<int32_t>(i) * 12345;
    }
    WriteStripAndClose<int32_t>(s, "wl", kW, kH, kH, 32, 1, SAMPLEFORMAT_INT,
                                PLANARCONFIG_CONTIG, LERC_ADD_COMPRESSION_NONE,
                                0.0, in);
    EXPECT_EQ((ReadAllStrips<int32_t>(s)), in);
  }

  // uint32 big endian
  {
    MemoryTiffStream s;
    std::vector<uint32_t> in(kW * kH);
    for (size_t i = 0; i < in.size(); ++i) {
      in[i] = 1000000U + static_cast<uint32_t>(i) * 54321U;
    }
    WriteStripAndClose<uint32_t>(s, "wb", kW, kH, kH, 32, 1, SAMPLEFORMAT_UINT,
                                 PLANARCONFIG_CONTIG, LERC_ADD_COMPRESSION_NONE,
                                 0.0, in);
    EXPECT_EQ((ReadAllStrips<uint32_t>(s)), in);
  }

  // float32 & float64 finite values (nFoundMasks == 0 path on IEEEFP decode)
  {
    MemoryTiffStream s32;
    std::vector<float> f32(kW * kH);
    for (size_t i = 0; i < f32.size(); ++i) {
      f32[i] = static_cast<float>(i) * 0.25f - 1.5f;
    }
    WriteStripAndClose<float>(s32, "wb", kW, kH, kH, 32, 1, SAMPLEFORMAT_IEEEFP,
                              PLANARCONFIG_CONTIG, LERC_ADD_COMPRESSION_NONE,
                              0.0, f32);
    EXPECT_EQ((ReadAllStrips<float>(s32)), f32);

    MemoryTiffStream s64;
    std::vector<double> f64(kW * kH);
    for (size_t i = 0; i < f64.size(); ++i) {
      f64[i] = static_cast<double>(i) * 0.125 - 2.0;
    }
    WriteStripAndClose<double>(s64, "wb", kW, kH, kH, 64, 1,
                               SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG,
                               LERC_ADD_COMPRESSION_DEFLATE, 0.0, f64);
    EXPECT_EQ((ReadAllStrips<double>(s64)), f64);
  }
}

TEST(TifLercTest, LossyMaxZErrorWithinBound) {
  constexpr uint32_t kW = 16;
  constexpr uint32_t kH = 16;
  std::vector<float> in(kW * kH);
  for (size_t i = 0; i < in.size(); ++i) {
    in[i] = static_cast<float>(i) * 0.137f;
  }

  MemoryTiffStream lossless_stream;
  WriteStripAndClose<float>(lossless_stream, "wl", kW, kH, kH, 32, 1,
                            SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG,
                            LERC_ADD_COMPRESSION_NONE, 0.0, in);

  MemoryTiffStream lossy_stream;
  WriteStripAndClose<float>(lossy_stream, "wl", kW, kH, kH, 32, 1,
                            SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG,
                            LERC_ADD_COMPRESSION_NONE, 0.5, in);

  EXPECT_LT(lossy_stream.data().size(), lossless_stream.data().size());
  const std::vector<float> out = ReadAllStrips<float>(lossy_stream);
  ASSERT_EQ(out.size(), in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    EXPECT_LE(std::fabs(out[i] - in[i]), 0.5f + 1e-5f);
  }
}

TEST(TifLercTest, AlphaMaskExtractionAndBypassPaths) {
  // 1) Binary alpha (0/255) on 4x4 (nb_pixels=16 > dst_nbands=3): hits memmove
  //    AND memcpy loops in both encode (899-914) and decode (614-630).
  {
    constexpr uint32_t kW = 4;
    constexpr uint32_t kH = 4;
    std::vector<uint8_t> rgba(kW * kH * 4);
    for (size_t i = 0; i < kW * kH; ++i) {
      rgba[i * 4 + 0] = static_cast<uint8_t>(10 + i * 7);
      rgba[i * 4 + 1] = static_cast<uint8_t>(20 + i * 5);
      rgba[i * 4 + 2] = static_cast<uint8_t>(30 + i * 3);
      rgba[i * 4 + 3] = (i % 3 == 0) ? 0 : 255;
    }
    MemoryTiffStream stream;
    WriteStripAndClose<uint8_t>(stream, "wl", kW, kH, kH, 8, 4,
                                SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG,
                                LERC_ADD_COMPRESSION_NONE, 0.0, rgba,
                                /*set_unass_alpha=*/true);
    const std::vector<uint8_t> out = ReadAllStrips<uint8_t>(stream);
    ASSERT_EQ(out.size(), rgba.size());
    for (size_t i = 0; i < kW * kH; ++i) {
      EXPECT_EQ(out[i * 4 + 3], rgba[i * 4 + 3]);
      if (rgba[i * 4 + 3] == 255) {
        EXPECT_EQ(out[i * 4 + 0], rgba[i * 4 + 0]);
        EXPECT_EQ(out[i * 4 + 1], rgba[i * 4 + 1]);
        EXPECT_EQ(out[i * 4 + 2], rgba[i * 4 + 2]);
      }
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0);
  }

  // 2) Tiny 2x1 binary alpha (nb_pixels=2 < dst_nbands=3): hits overlap-only
  //    loop exit cleanly without reading out of bounds.
  {
    constexpr uint32_t kW = 2;
    constexpr uint32_t kH = 1;
    std::vector<uint8_t> rgba = {11, 22, 33, 255, 44, 55, 66, 0};
    MemoryTiffStream stream;
    WriteStripAndClose<uint8_t>(stream, "wl", kW, kH, kH, 8, 4,
                                SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG,
                                LERC_ADD_COMPRESSION_NONE, 0.0, rgba,
                                /*set_unass_alpha=*/true);
    const std::vector<uint8_t> out = ReadAllStrips<uint8_t>(stream);
    ASSERT_EQ(out.size(), rgba.size());
    EXPECT_EQ(out[0], 11);
    EXPECT_EQ(out[1], 22);
    EXPECT_EQ(out[2], 33);
    EXPECT_EQ(out[3], 255);
    EXPECT_EQ(out[7], 0);
  }

  // 3) Non-binary alpha (e.g., 128) forces use_mask=0 on encode and regular
  //    4-band decode.
  {
    constexpr uint32_t kW = 3;
    constexpr uint32_t kH = 2;
    std::vector<uint8_t> rgba(kW * kH * 4);
    for (size_t i = 0; i < kW * kH; ++i) {
      rgba[i * 4 + 0] = static_cast<uint8_t>(i * 15);
      rgba[i * 4 + 1] = static_cast<uint8_t>(i * 25);
      rgba[i * 4 + 2] = static_cast<uint8_t>(i * 35);
      rgba[i * 4 + 3] = (i == 2) ? 128 : 255;
    }
    MemoryTiffStream stream;
    WriteStripAndClose<uint8_t>(stream, "wl", kW, kH, kH, 8, 4,
                                SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG,
                                LERC_ADD_COMPRESSION_NONE, 0.0, rgba,
                                /*set_unass_alpha=*/true);
    EXPECT_EQ((ReadAllStrips<uint8_t>(stream)), rgba);
  }
}

TEST(TifLercTest, IeeefpNanHandlingAllModesFloat32AndFloat64) {
  const float f_nan = std::numeric_limits<float>::quiet_NaN();
  const double d_nan = std::numeric_limits<double>::quiet_NaN();

  // 1) Single-band & Separate float32/float64 with NaN.
  {
    std::vector<float> f32 = {1.0f, f_nan, 3.5f, f_nan, 5.25f, 6.5f};
    MemoryTiffStream s32;
    WriteStripAndClose<float>(s32, "wl", 3, 2, 2, 32, 1, SAMPLEFORMAT_IEEEFP,
                              PLANARCONFIG_CONTIG, LERC_ADD_COMPRESSION_NONE,
                              0.0, f32);
    const std::vector<float> out32 = ReadAllStrips<float>(s32);
    ASSERT_EQ(out32.size(), f32.size());
    for (size_t i = 0; i < f32.size(); ++i) {
      EXPECT_EQ(std::isnan(out32[i]), std::isnan(f32[i]));
      if (!std::isnan(f32[i])) EXPECT_FLOAT_EQ(out32[i], f32[i]);
    }

    std::vector<double> d64 = {1.5, d_nan, 2.5, 3.5, 4.5, d_nan, 6.5, 7.5};
    MemoryTiffStream s64;
    WriteStripAndClose<double>(s64, "wl", 2, 2, 2, 64, 2, SAMPLEFORMAT_IEEEFP,
                               PLANARCONFIG_SEPARATE, LERC_ADD_COMPRESSION_NONE,
                               0.0, d64);
    const std::vector<double> out64 = ReadAllStrips<double>(s64);
    ASSERT_EQ(out64.size(), d64.size());
    for (size_t i = 0; i < d64.size(); ++i) {
      EXPECT_EQ(std::isnan(out64[i]), std::isnan(d64[i]));
      if (!std::isnan(d64[i])) EXPECT_DOUBLE_EQ(out64[i], d64[i]);
    }
  }

  // 2) Multi-band contig shared-mask (all bands of a pixel are NaN together ->
  //    nRequestedMasks == 1).
  {
    std::vector<float> f32 = {1.0f, 2.0f, f_nan, f_nan, 3.0f, 4.0f, 5.0f, 6.0f};
    MemoryTiffStream s32;
    WriteStripAndClose<float>(s32, "wl", 2, 2, 2, 32, 2, SAMPLEFORMAT_IEEEFP,
                              PLANARCONFIG_CONTIG, LERC_ADD_COMPRESSION_NONE,
                              0.0, f32);
    const std::vector<float> out32 = ReadAllStrips<float>(s32);
    ASSERT_EQ(out32.size(), f32.size());
    for (size_t i = 0; i < f32.size(); ++i) {
      EXPECT_EQ(std::isnan(out32[i]), std::isnan(f32[i]));
      if (!std::isnan(f32[i])) EXPECT_FLOAT_EQ(out32[i], f32[i]);
    }

    std::vector<double> d64 = {1.0, 2.0, d_nan, d_nan, 3.0, 4.0, 5.0, 6.0};
    MemoryTiffStream s64;
    WriteStripAndClose<double>(s64, "wl", 2, 2, 2, 64, 2, SAMPLEFORMAT_IEEEFP,
                               PLANARCONFIG_CONTIG, LERC_ADD_COMPRESSION_NONE,
                               0.0, d64);
    const std::vector<double> out64 = ReadAllStrips<double>(s64);
    ASSERT_EQ(out64.size(), d64.size());
    for (size_t i = 0; i < d64.size(); ++i) {
      EXPECT_EQ(std::isnan(out64[i]), std::isnan(d64[i]));
      if (!std::isnan(d64[i])) EXPECT_DOUBLE_EQ(out64[i], d64[i]);
    }
  }

  // 3) Multi-band contig per-band mask (mixed NaN + finite across bands in same
  //    pixel -> nRequestedMasks == samplesperpixel).
  {
    std::vector<float> f32 = {1.0f, f_nan, f_nan, 2.0f,
                              3.0f, 4.0f,  5.0f,  f_nan};
    MemoryTiffStream s32;
    WriteStripAndClose<float>(s32, "wl", 2, 2, 2, 32, 2, SAMPLEFORMAT_IEEEFP,
                              PLANARCONFIG_CONTIG, LERC_ADD_COMPRESSION_NONE,
                              0.0, f32);
    const std::vector<float> out32 = ReadAllStrips<float>(s32);
    ASSERT_EQ(out32.size(), f32.size());
    for (size_t i = 0; i < f32.size(); ++i) {
      EXPECT_EQ(std::isnan(out32[i]), std::isnan(f32[i]));
      if (!std::isnan(f32[i])) EXPECT_FLOAT_EQ(out32[i], f32[i]);
    }

    std::vector<double> d64 = {1.0, d_nan, d_nan, 2.0, 3.0, 4.0, 5.0, d_nan};
    MemoryTiffStream s64;
    WriteStripAndClose<double>(s64, "wl", 2, 2, 2, 64, 2, SAMPLEFORMAT_IEEEFP,
                               PLANARCONFIG_CONTIG, LERC_ADD_COMPRESSION_NONE,
                               0.0, d64);
    const std::vector<double> out64 = ReadAllStrips<double>(s64);
    ASSERT_EQ(out64.size(), d64.size());
    for (size_t i = 0; i < d64.size(); ++i) {
      EXPECT_EQ(std::isnan(out64[i]), std::isnan(d64[i]));
      if (!std::isnan(d64[i])) EXPECT_DOUBLE_EQ(out64[i], d64[i]);
    }
  }
}

TEST(TifLercTest, ScanlineTilesAndBufferGrowthAcrossStrips) {
  // Multi-strip image with truncated last strip: read smaller strip 1 first,
  // then larger strip 0, exercising buffer growth reallocation paths in
  // SetupBuffers (uncompressed_buffer & mask_buffer), compressed_buffer, and
  // uncompressed_buffer_multiband.
  const float f_nan = std::numeric_limits<float>::quiet_NaN();
  constexpr uint32_t kW = 8;
  constexpr uint32_t kH = 5;
  constexpr uint32_t kRowsPerStrip = 4;  // Strip 0 = 4 rows, strip 1 = 1 row.
  std::vector<float> pixels(kW * kH * 2);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<float>(i) * 0.5f;
  }
  pixels[1] = f_nan;               // Mixed NaN in strip 0.
  pixels[kW * 4 * 2 + 1] = f_nan;  // Mixed NaN in strip 1.

  MemoryTiffStream stream;
  WriteStripAndClose<float>(stream, "wl", kW, kH, kRowsPerStrip, 32, 2,
                            SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG,
                            LERC_ADD_COMPRESSION_ZSTD, 0.0, pixels);

  {
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<float> strip1(kW * 1 * 2);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 1, strip1.data(),
                                   static_cast<tmsize_t>(strip1.size() * 4)),
              static_cast<tmsize_t>(strip1.size() * 4));
    std::vector<float> strip0(kW * 4 * 2);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip0.data(),
                                   static_cast<tmsize_t>(strip0.size() * 4)),
              static_cast<tmsize_t>(strip0.size() * 4));
    EXPECT_TRUE(std::isnan(strip0[1]));
    EXPECT_FLOAT_EQ(strip0[0], pixels[0]);
    EXPECT_TRUE(std::isnan(strip1[1]));
    EXPECT_FLOAT_EQ(strip1[0], pixels[kW * 4 * 2]);
  }

  // Row-by-row scanline write & read across a multi-row LERC strip.
  {
    constexpr uint32_t kSw = 6;
    constexpr uint32_t kSh = 4;
    MemoryTiffStream scan_stream;
    std::vector<uint16_t> expected(kSw * kSh);
    for (size_t i = 0; i < expected.size(); ++i) {
      expected[i] = static_cast<uint16_t>(i * 111 + 7);
    }
    {
      UniqueTiff writer = scan_stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kSw), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kSh), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{16}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                             uint16_t{SAMPLEFORMAT_UINT}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISBLACK}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kSh), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC), 1);
      for (uint32_t row = 0; row < kSh; ++row) {
        std::vector<uint16_t> row_copy(
            expected.begin() + static_cast<std::ptrdiff_t>(row * kSw),
            expected.begin() + static_cast<std::ptrdiff_t>((row + 1) * kSw));
        ASSERT_EQ(TIFFWriteScanline(writer.get(), row_copy.data(), row, 0), 1);
      }
    }
    {
      UniqueTiff reader = scan_stream.Open("r");
      ASSERT_NE(reader.get(), nullptr);
      std::vector<uint16_t> row_out(kSw);
      for (uint32_t row = 0; row < kSh; ++row) {
        ASSERT_EQ(TIFFReadScanline(reader.get(), row_out.data(), row, 0), 1);
        for (uint32_t c = 0; c < kSw; ++c) {
          EXPECT_EQ(row_out[c], expected[row * kSw + c]);
        }
      }
    }
  }

  // Tiled write & read via TIFFWriteTile / TIFFReadTile.
  {
    constexpr uint32_t kTw = 16;
    constexpr uint32_t kTh = 16;
    constexpr uint32_t kImgW = 20;
    constexpr uint32_t kImgH = 20;
    MemoryTiffStream tile_stream;
    std::vector<uint8_t> tile_data(kTw * kTh);
    for (size_t i = 0; i < tile_data.size(); ++i) {
      tile_data[i] = static_cast<uint8_t>((i * 7 + 3) & 0xff);
    }
    {
      UniqueTiff writer = tile_stream.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kImgW), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kImgH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, kTw), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, kTh), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                             uint16_t{SAMPLEFORMAT_UINT}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_MINISBLACK}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_LERC_ADD_COMPRESSION,
                             LERC_ADD_COMPRESSION_DEFLATE),
                1);
      for (uint32_t y = 0; y < kImgH; y += kTh) {
        for (uint32_t x = 0; x < kImgW; x += kTw) {
          std::vector<uint8_t> tile_copy = tile_data;
          ASSERT_EQ(TIFFWriteTile(writer.get(), tile_copy.data(), x, y, 0, 0),
                    static_cast<tmsize_t>(tile_copy.size()));
        }
      }
    }
    {
      UniqueTiff reader = tile_stream.Open("r");
      ASSERT_NE(reader.get(), nullptr);
      std::vector<uint8_t> tile_out(kTw * kTh);
      for (uint32_t y = 0; y < kImgH; y += kTh) {
        for (uint32_t x = 0; x < kImgW; x += kTw) {
          ASSERT_EQ(TIFFReadTile(reader.get(), tile_out.data(), x, y, 0, 0),
                    static_cast<tmsize_t>(tile_out.size()));
          EXPECT_EQ(tile_out, tile_data);
        }
      }
    }
    EXPECT_EQ(tile_stream.diagnostics().error_count(), 0);
  }
}

TEST(TifLercTest, ModeSwitchDecodeToEncodeAndBack) {
  // Exercises sp->state & LSTATE_INIT_DECODE in LERCSetupEncode and
  // sp->state & LSTATE_INIT_ENCODE in LERCSetupDecode via "r+" open mode.
  constexpr uint32_t kW = 4;
  constexpr uint32_t kH = 4;
  std::vector<uint8_t> pixels(kW * kH);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<uint8_t>(i + 10);
  }

  MemoryTiffStream stream;
  WriteStripAndClose<uint8_t>(stream, "wl", kW, kH, 2, 8, 1, SAMPLEFORMAT_UINT,
                              PLANARCONFIG_CONTIG, LERC_ADD_COMPRESSION_NONE,
                              0.0, pixels);

  {
    UniqueTiff rw = stream.Open("r+");
    ASSERT_NE(rw.get(), nullptr);
    std::vector<uint8_t> strip0(kW * 2);
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, strip0.data(),
                                   static_cast<tmsize_t>(strip0.size())),
              static_cast<tmsize_t>(strip0.size()));

    // Switch from decode mode to encode mode on strip 1.
    std::vector<uint8_t> new_strip1(kW * 2, 0x5A);
    ASSERT_EQ(TIFFWriteEncodedStrip(rw.get(), 1, new_strip1.data(),
                                    static_cast<tmsize_t>(new_strip1.size())),
              static_cast<tmsize_t>(new_strip1.size()));

    // Switch back from encode mode to decode mode by reading strip 0 again.
    std::vector<uint8_t> strip0_again(kW * 2);
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, strip0_again.data(),
                                   static_cast<tmsize_t>(strip0_again.size())),
              static_cast<tmsize_t>(strip0_again.size()));
    EXPECT_EQ(strip0_again, strip0);
    EXPECT_EQ(stream.diagnostics().error_count(), 0);
  }
}

TEST(TifLercTest, EncodeAndDecodeBufferBoundsAndUnsupportedFormats) {
  // Unsupported bitdepth/sampleformat on encode (GetLercDataType -> -1).
  {
    MemoryTiffStream s;
    UniqueTiff writer = s.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{64}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                           uint16_t{SAMPLEFORMAT_UINT}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC),
              1);
    std::vector<uint64_t> buf(16, 1);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                    static_cast<tmsize_t>(buf.size() * 8)),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(ContainsMessage(s.diagnostics().errors(),
                                "Unsupported combination of SampleFormat"));
  }

  // Unsupported bitdepth/sampleformat on decode.
  {
    std::array<uint8_t, 8> dummy_blob = {1, 2, 3, 4, 5, 6, 7, 8};
    MemoryTiffStream s(BuildRawLercStripTiff(4, 4, 64, 1, SAMPLEFORMAT_UINT,
                                             PLANARCONFIG_CONTIG, dummy_blob));
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint64_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size() * 8)),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(ContainsMessage(s.diagnostics().errors(),
                                "Unsupported combination of SampleFormat"));
  }

  // LERCEncode: Too many bytes written; LERCPostEncode: Unexpected byte count.
  {
    MemoryTiffStream s;
    UniqueTiff writer = s.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                           uint16_t{SAMPLEFORMAT_UINT}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC),
              1);

    std::vector<uint8_t> too_big(17, 1);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, too_big.data(),
                                    static_cast<tmsize_t>(too_big.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(
        ContainsMessage(s.diagnostics().errors(), "Too many bytes written"));

    s.diagnostics().Clear();
    std::vector<uint8_t> too_small(15, 1);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, too_small.data(),
                                    static_cast<tmsize_t>(too_small.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(ContainsMessage(s.diagnostics().errors(),
                                "Unexpected number of bytes in the buffer"));
  }

  // LERCDecode: Too many bytes read via TIFFReadFromUserBuffer.
  {
    std::vector<uint8_t> in(16, 42);
    MemoryTiffStream s;
    WriteStripAndClose<uint8_t>(s, "wl", 4, 4, 4, 8, 1, SAMPLEFORMAT_UINT,
                                PLANARCONFIG_CONTIG, LERC_ADD_COMPRESSION_NONE,
                                0.0, in);
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    const tmsize_t raw_size = TIFFRawStripSize(reader.get(), 0);
    std::vector<uint8_t> raw(static_cast<size_t>(raw_size));
    ASSERT_EQ(TIFFReadRawStrip(reader.get(), 0, raw.data(), raw_size),
              raw_size);
    std::vector<uint8_t> oversized_out(17);
    EXPECT_EQ(TIFFReadFromUserBuffer(
                  reader.get(), 0, raw.data(), raw_size, oversized_out.data(),
                  static_cast<tmsize_t>(oversized_out.size())),
              0);
    EXPECT_TRUE(
        ContainsMessage(s.diagnostics().errors(), "Too many bytes read"));
  }

  // Encode errors: negative MAXZERROR triggers lerc_encode() failed; bad
  // additional compression triggers Unhandled additional compression; write
  // failures during TIFFFlushData1 fail NONE, DEFLATE, and ZSTD encode paths.
  {
    MemoryTiffStream s;
    UniqueTiff writer = s.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                           uint16_t{SAMPLEFORMAT_UINT}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_LERC_MAXZERROR, -1.0), 1);
    std::vector<uint8_t> pixels(16, 5);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, pixels.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(
        ContainsMessage(s.diagnostics().errors(), "lerc_encode() failed"));
  }

  {
    MemoryTiffStream s;
    UniqueTiff writer = s.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                           uint16_t{SAMPLEFORMAT_UINT}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC),
              1);
    int bad_params[2] = {LERC_VERSION_2_4, 99};
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_LERC_PARAMETERS, 2, bad_params), 1);
    std::vector<uint8_t> pixels(16, 5);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, pixels.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(ContainsMessage(s.diagnostics().errors(),
                                "Unhandled additional compression"));
  }

  for (int add_comp : {LERC_ADD_COMPRESSION_NONE, LERC_ADD_COMPRESSION_DEFLATE,
                       LERC_ADD_COMPRESSION_ZSTD}) {
    SCOPED_TRACE(add_comp);
    MemoryTiffStream s;
    UniqueTiff writer = s.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                           uint16_t{SAMPLEFORMAT_UINT}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{4}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_LERC_ADD_COMPRESSION, add_comp), 1);
    std::vector<uint8_t> pixels(16, 7);
    s.FailWritesAfter(s.write_calls());
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, pixels.data(), 16),
              static_cast<tmsize_t>(-1));
    s.ResetFaults();
  }
}

TEST(TifLercTest, DecodeValidationErrorsAndCorruption) {
  std::vector<uint8_t> u8_1band(16);
  for (size_t i = 0; i < u8_1band.size(); ++i) {
    u8_1band[i] = static_cast<uint8_t>(i * 9);
  }
  const std::vector<uint8_t> u8_blob =
      EncodeSingleStripAndExtractRawBlob<uint8_t>(
          4, 4, 8, 1, SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG,
          LERC_ADD_COMPRESSION_NONE, u8_1band);

  std::vector<float> f32_2band_finite = {1.0f, 2.0f, 3.0f, 4.0f,
                                         5.0f, 6.0f, 7.0f, 8.0f};
  const std::vector<uint8_t> f32_2band_blob =
      EncodeSingleStripAndExtractRawBlob<float>(
          2, 2, 32, 2, SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG,
          LERC_ADD_COMPRESSION_NONE, f32_2band_finite);

  const float f_nan = std::numeric_limits<float>::quiet_NaN();
  std::vector<float> f32_2band_mixed = {1.0f, f_nan, 3.0f, 4.0f,
                                        5.0f, 6.0f,  7.0f, f_nan};
  const std::vector<uint8_t> f32_mixed_blob =
      EncodeSingleStripAndExtractRawBlob<float>(
          2, 2, 32, 2, SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG,
          LERC_ADD_COMPRESSION_NONE, f32_2band_mixed);

  std::vector<float> f32_1band = {1.0f, 2.0f, 3.0f, 4.0f};
  const std::vector<uint8_t> f32_1band_blob =
      EncodeSingleStripAndExtractRawBlob<float>(
          2, 2, 32, 1, SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG,
          LERC_ADD_COMPRESSION_NONE, f32_1band);

  // 1) Corrupt DEFLATE / ZSTD / unhandled additional compression / corrupt
  // LERC.
  {
    std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    std::array<uint32_t, 2> params_deflate = {LERC_VERSION_2_4,
                                              LERC_ADD_COMPRESSION_DEFLATE};
    MemoryTiffStream s(BuildRawLercStripTiff(4, 4, 8, 1, SAMPLEFORMAT_UINT,
                                             PLANARCONFIG_CONTIG, garbage,
                                             params_deflate));
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(ContainsMessage(s.diagnostics().errors(), "inflate() failed"));
  }
  {
    std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    std::array<uint32_t, 2> params_zstd = {LERC_VERSION_2_4,
                                           LERC_ADD_COMPRESSION_ZSTD};
    MemoryTiffStream s(BuildRawLercStripTiff(4, 4, 8, 1, SAMPLEFORMAT_UINT,
                                             PLANARCONFIG_CONTIG, garbage,
                                             params_zstd));
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(
        ContainsMessage(s.diagnostics().errors(), "Error in ZSTD_decompress"));
  }
  {
    std::array<uint32_t, 2> params_bad = {LERC_VERSION_2_4, 99};
    MemoryTiffStream s(BuildRawLercStripTiff(4, 4, 8, 1, SAMPLEFORMAT_UINT,
                                             PLANARCONFIG_CONTIG, u8_blob,
                                             params_bad));
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(ContainsMessage(s.diagnostics().errors(),
                                "Unhandled additional compression"));
  }
  {
    std::vector<uint8_t> bad_lerc = {0x4C, 0x65, 0x72, 0x63, 0x32, 0x20};
    MemoryTiffStream s(BuildRawLercStripTiff(4, 4, 8, 1, SAMPLEFORMAT_UINT,
                                             PLANARCONFIG_CONTIG, bad_lerc));
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(
        ContainsMessage(s.diagnostics().errors(), "lerc_getBlobInfo() failed"));
  }

  // 2) Corrupt payload after valid header triggers lerc_decode() failed.
  {
    std::vector<uint8_t> corrupted_payload = u8_blob;
    ASSERT_GT(corrupted_payload.size(), 4U);
    corrupted_payload[corrupted_payload.size() - 1] ^= 0xff;
    corrupted_payload[corrupted_payload.size() - 2] ^= 0xaa;
    corrupted_payload[corrupted_payload.size() - 3] ^= 0x55;
    MemoryTiffStream s(BuildRawLercStripTiff(
        4, 4, 8, 1, SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, corrupted_payload));
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(
        ContainsMessage(s.diagnostics().errors(), "lerc_decode() failed"));
  }

  // 3) Unexpected version number warning (LERC_PARAMETERS version=3 vs blob=4).
  {
    std::array<uint32_t, 2> params_v3 = {3, LERC_ADD_COMPRESSION_NONE};
    MemoryTiffStream s(BuildRawLercStripTiff(4, 4, 8, 1, SAMPLEFORMAT_UINT,
                                             PLANARCONFIG_CONTIG, u8_blob,
                                             params_v3));
    EXPECT_EQ((ReadAllStrips<uint8_t>(s)), u8_1band);
    EXPECT_TRUE(ContainsMessage(s.diagnostics().warnings(),
                                "Unexpected version number"));
  }

  // 4) Unexpected dataType.
  {
    MemoryTiffStream s(BuildRawLercStripTiff(4, 4, 8, 1, SAMPLEFORMAT_INT,
                                             PLANARCONFIG_CONTIG, u8_blob));
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<int8_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(
        ContainsMessage(s.diagnostics().errors(), "Unexpected dataType"));
  }

  // 5) Unexpected nDim (both float multi-band branch and general branch).
  {
    // Float 3-band IFD fed a 2-band float blob (nFoundDims=2 != 1 and != 3).
    MemoryTiffStream s(BuildRawLercStripTiff(
        2, 2, 32, 3, SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG, f32_2band_blob));
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<float> out(2 * 2 * 3);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size() * 4)),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(ContainsMessage(s.diagnostics().errors(), "Unexpected nDim"));
  }
  {
    // Uint8 3-band IFD fed a 1-band uint8 blob (nFoundDims=1 != 3).
    MemoryTiffStream s(BuildRawLercStripTiff(4, 4, 8, 3, SAMPLEFORMAT_UINT,
                                             PLANARCONFIG_CONTIG, u8_blob));
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> out(4 * 4 * 3);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(ContainsMessage(s.diagnostics().errors(), "Unexpected nDim"));
  }

  // 6) Unexpected nCols & Unexpected nRows.
  {
    MemoryTiffStream s_cols(BuildRawLercStripTiff(
        8, 4, 8, 1, SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, u8_blob));
    UniqueTiff r_cols = s_cols.Open("r");
    ASSERT_NE(r_cols.get(), nullptr);
    std::vector<uint8_t> out_cols(32);
    EXPECT_EQ(TIFFReadEncodedStrip(r_cols.get(), 0, out_cols.data(), 32),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(
        ContainsMessage(s_cols.diagnostics().errors(), "Unexpected nCols"));

    MemoryTiffStream s_rows(BuildRawLercStripTiff(
        4, 8, 8, 1, SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, u8_blob));
    UniqueTiff r_rows = s_rows.Open("r");
    ASSERT_NE(r_rows.get(), nullptr);
    std::vector<uint8_t> out_rows(32);
    EXPECT_EQ(TIFFReadEncodedStrip(r_rows.get(), 0, out_rows.data(), 32),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(
        ContainsMessage(s_rows.diagnostics().errors(), "Unexpected nRows"));
  }

  // 7) Unexpected nBands (both IEEEFP CONTIG spp>1 nFoundDims==1 and else
  //    branch).
  {
    // 2-band float CONTIG IFD fed 1-band float blob (nFoundDims=1,
    // nFoundBands=1 != 2).
    MemoryTiffStream s(BuildRawLercStripTiff(
        2, 2, 32, 2, SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG, f32_1band_blob));
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<float> out(2 * 2 * 2);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size() * 4)),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(ContainsMessage(s.diagnostics().errors(), "Unexpected nBands"));
  }
  {
    // 1-band float IFD fed mixed 2-band float blob (nFoundDims=1 == ndims,
    // nFoundBands=2 != 1).
    MemoryTiffStream s(BuildRawLercStripTiff(
        2, 2, 32, 1, SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG, f32_mixed_blob));
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<float> out(2 * 2);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size() * 4)),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(ContainsMessage(s.diagnostics().errors(), "Unexpected nBands"));
  }

  // 8) Unexpected blobSize (trailing garbage bytes appended after valid blob).
  {
    std::vector<uint8_t> padded = u8_blob;
    padded.push_back(0x00);
    padded.push_back(0x00);
    MemoryTiffStream s(BuildRawLercStripTiff(4, 4, 8, 1, SAMPLEFORMAT_UINT,
                                             PLANARCONFIG_CONTIG, padded));
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(
        ContainsMessage(s.diagnostics().errors(), "Unexpected blobSize"));
  }

  // 9) Too large uncompressed strip/tile (32-bit overflow check in
  // SetupBuffers).
  {
    RawTiffBuilder builder;
    const std::array<uint8_t, 8> tiny_tile = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint64_t tile_off = builder.AppendBlob(tiny_tile);
    std::vector<RawIfdEntry> entries;
    entries.push_back(builder.MakeLong(TIFFTAG_IMAGEWIDTH, 65536));
    entries.push_back(builder.MakeLong(TIFFTAG_IMAGELENGTH, 65536));
    entries.push_back(builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8));
    entries.push_back(builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_LERC));
    entries.push_back(
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    entries.push_back(builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1));
    entries.push_back(
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG));
    entries.push_back(builder.MakeLong(TIFFTAG_TILEWIDTH, 65536));
    entries.push_back(builder.MakeLong(TIFFTAG_TILELENGTH, 65536));
    entries.push_back(
        builder.MakeLong(TIFFTAG_TILEOFFSETS, static_cast<uint32_t>(tile_off)));
    entries.push_back(builder.MakeLong(
        TIFFTAG_TILEBYTECOUNTS, static_cast<uint32_t>(tiny_tile.size())));
    entries.push_back(
        builder.MakeShort(TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT));
    builder.AddIfd(std::move(entries));

    MemoryTiffStream s(builder.Take());
    UniqueTiff reader = s.Open("r");
    ASSERT_NE(reader.get(), nullptr);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, out.data(), 1),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(ContainsMessage(s.diagnostics().errors(),
                                "Too large uncompressed strip/tile"));
  }
}

TEST(TifLercTest, AllocationLimitsOomPathsCleanlyFail) {
  // 1) TIFFInitLERC OOM during _TIFFcallocExt(tif, 1, sizeof(LERCState)).
  //    Open with non-zero cumulated limit per alloc_limit_guard rules; allow
  //    _TIFFMergeFields (~48 bytes net growth) to succeed while failing
  //    LERCState allocation without triggering _TIFFMergeFields failure
  //    cleanup.
  {
    MemoryTiffStream stream;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif.get(), nullptr);
    {
      ScopedCumulatedMemAllocRemaining guard(tif.get(), 64);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC),
                0);
    }
    EXPECT_TRUE(ContainsMessage(stream.diagnostics().errors(),
                                "No space for LERC state block"));
  }

  // 2) SetupBuffers uncompressed_buffer OOM on decode.
  {
    constexpr uint32_t kW = 100;
    constexpr uint32_t kH = 100;
    std::vector<uint8_t> pixels(kW * kH, 7);
    MemoryTiffStream stream;
    WriteStripAndClose<uint8_t>(stream, "wl", kW, kH, kH, 8, 1,
                                SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG,
                                LERC_ADD_COMPRESSION_NONE, 0.0, pixels);
    stream.set_enable_mmap(true);
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    stream.diagnostics().Install(opts.get());
    UniqueTiff reader = stream.Open("r", opts.get());
    ASSERT_NE(reader.get(), nullptr);
    std::vector<uint8_t> out(kW * kH);
    {
      ScopedSingleMemAllocLimit limit(reader.get(), 5000);
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                     static_cast<tmsize_t>(out.size())),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_TRUE(ContainsMessage(stream.diagnostics().errors(),
                                "Cannot allocate buffer"));
  }

  // 3) SetupBuffers mask_buffer OOM on decode.
  {
    constexpr uint32_t kW = 128;
    constexpr uint32_t kH = 128;
    std::vector<float> pixels(kW * kH, 1.0f);
    MemoryTiffStream stream;
    WriteStripAndClose<float>(stream, "wl", kW, kH, kH, 32, 1,
                              SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG,
                              LERC_ADD_COMPRESSION_NONE, 0.0, pixels);
    stream.set_enable_mmap(true);
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);
    UniqueTiff reader = stream.Open("r", opts.get());
    ASSERT_NE(reader.get(), nullptr);

    const tmsize_t new_size = static_cast<tmsize_t>(kW) * kH * 4;
    const tmsize_t new_alloc = 256 + new_size + new_size / 3;
    std::vector<float> out(kW * kH);
    {
      ScopedCumulatedMemAllocRemaining guard(reader.get(), new_alloc + 1000);
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                     static_cast<tmsize_t>(out.size() * 4)),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_TRUE(ContainsMessage(stream.diagnostics().errors(),
                                "Cannot allocate buffer"));
  }

  // 4) LERCPreDecode compressed_buffer OOM when ZSTD additional compression is
  //    active.
  {
    constexpr uint32_t kW = 100;
    constexpr uint32_t kH = 100;
    std::vector<uint8_t> pixels(kW * kH, 13);
    MemoryTiffStream stream;
    WriteStripAndClose<uint8_t>(stream, "wl", kW, kH, kH, 8, 1,
                                SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG,
                                LERC_ADD_COMPRESSION_ZSTD, 0.0, pixels);
    stream.set_enable_mmap(true);
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);
    UniqueTiff reader = stream.Open("r", opts.get());
    ASSERT_NE(reader.get(), nullptr);

    const tmsize_t new_size = static_cast<tmsize_t>(kW) * kH;
    const tmsize_t new_alloc = 256 + new_size + new_size / 3;
    std::vector<uint8_t> out(kW * kH);
    {
      ScopedCumulatedMemAllocRemaining guard(reader.get(), new_alloc + 1000);
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                     static_cast<tmsize_t>(out.size())),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
  }

  // 5) LERCPreDecode uncompressed_buffer_multiband OOM on mixed-NaN 2-band
  //    float.
  {
    constexpr uint32_t kW = 100;
    constexpr uint32_t kH = 100;
    const float f_nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> pixels(kW * kH * 2, 1.0f);
    pixels[1] = f_nan;  // Mixed NaN forces per-band masks.
    MemoryTiffStream stream;
    WriteStripAndClose<float>(stream, "wl", kW, kH, kH, 32, 2,
                              SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG,
                              LERC_ADD_COMPRESSION_NONE, 0.0, pixels);
    stream.set_enable_mmap(true);
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);
    UniqueTiff reader = stream.Open("r", opts.get());
    ASSERT_NE(reader.get(), nullptr);

    const tmsize_t num_bytes_needed = static_cast<tmsize_t>(kW) * kH * 2 * 4;
    const tmsize_t new_alloc = 256 + num_bytes_needed + num_bytes_needed / 3;
    const tmsize_t mask_size = static_cast<tmsize_t>(kW) * kH * 2;
    std::vector<float> out(kW * kH * 2);
    {
      ScopedCumulatedMemAllocRemaining guard(reader.get(),
                                             new_alloc + mask_size + 5000);
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                     static_cast<tmsize_t>(out.size() * 4)),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
  }

  // 6) LERCPostEncode OOMs on strip 1 (mixed NaN) after strip 0 (finite):
  //    first fail uncompressed_buffer_multiband, then allow it and fail
  //    compressed_buffer expansion.
  {
    constexpr uint32_t kW = 100;
    constexpr uint32_t kStripRows = 50;
    constexpr uint32_t kH = 100;
    const float f_nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> finite_strip(kW * kStripRows * 2, 2.0f);
    std::vector<float> mixed_strip(kW * kStripRows * 2, 2.0f);
    mixed_strip[1] = f_nan;

    const tmsize_t num_bytes_needed =
        static_cast<tmsize_t>(kW) * kStripRows * 2 * 4;
    const tmsize_t uncompressed_alloc =
        256 + num_bytes_needed + num_bytes_needed / 3;
    const tmsize_t expanded_compressed =
        uncompressed_alloc + static_cast<tmsize_t>(kW) * kStripRows * 2 / 8;

    for (tmsize_t single_limit :
         {num_bytes_needed - 1, expanded_compressed - 1}) {
      SCOPED_TRACE(single_limit);
      MemoryTiffStream s;
      UniqueTiff writer = s.Open("wl");
      ASSERT_NE(writer.get(), nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kStripRows),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{32}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{2}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                             uint16_t{SAMPLEFORMAT_IEEEFP}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC), 1);

      std::vector<float> strip0 = finite_strip;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip0.data(),
                                      num_bytes_needed),
                num_bytes_needed);

      std::vector<float> strip1 = mixed_strip;
      {
        ScopedSingleMemAllocLimit limit(writer.get(), single_limit);
        EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, strip1.data(),
                                        num_bytes_needed),
                  static_cast<tmsize_t>(-1));
      }
      EXPECT_GT(s.diagnostics().error_count(), 0U);
    }
  }

  // 7) LERCPreEncode SetupBuffers OOM on write.
  {
    constexpr uint32_t kW = 200;
    constexpr uint32_t kH = 200;
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                           uint16_t{SAMPLEFORMAT_UINT}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kH), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LERC),
              1);
    std::vector<uint8_t> pixels(kW * kH, 9);
    {
      // Allow TIFFWriteBufferSetup (44000 bytes = strip + 10%) while failing
      // SetupBuffers (53589 bytes = 256 + strip + strip/3).
      ScopedSingleMemAllocLimit limit(writer.get(), 48000);
      EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, pixels.data(),
                                      static_cast<tmsize_t>(pixels.size())),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_TRUE(ContainsMessage(stream.diagnostics().errors(),
                                "Cannot allocate buffer"));
  }
}

}  // namespace
}  // namespace libtiff
