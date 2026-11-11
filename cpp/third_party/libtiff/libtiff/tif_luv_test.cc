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

// Unit tests for libtiff/tif_luv.c (SGILog / SGILog24 codecs and public
// LogL / LogLuv / XYZ / u'v' conversion routines).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"
#include "third_party/tiff/libtiff/uvcode.h"

namespace libtiff {
namespace {

constexpr uint32_t kDefaultAllocBudget = 1U << 24;

bool HasAnyMessageContaining(const std::vector<DiagnosticRecord>& records,
                             std::string_view substr) {
  return std::any_of(
      records.begin(), records.end(), [&](const DiagnosticRecord& rec) {
        return rec.message.find(substr) != std::string_view::npos;
      });
}

bool HasDiagnosticContaining(const std::vector<DiagnosticRecord>& records,
                             std::string_view module, std::string_view substr) {
  return std::any_of(
      records.begin(), records.end(), [&](const DiagnosticRecord& rec) {
        return rec.module == module &&
               rec.message.find(substr) != std::string_view::npos;
      });
}

void ConfigureStriped(TIFF* tif, uint32_t width, uint32_t height,
                      uint32_t rows_per_strip, uint16_t compression,
                      uint16_t photometric, int data_fmt,
                      uint16_t samples_per_pixel = 0) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, photometric), 1);
  if (samples_per_pixel != 0) {
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  } else {
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL,
                           photometric == PHOTOMETRIC_LOGL ? 1 : 3),
              1);
  }
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, compression), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SGILOGDATAFMT, data_fmt), 1);
}

void ConfigureTiled(TIFF* tif, uint32_t width, uint32_t height,
                    uint32_t tile_width, uint32_t tile_height,
                    uint16_t compression, uint16_t photometric, int data_fmt) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile_width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILELENGTH, tile_height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, photometric), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL,
                         photometric == PHOTOMETRIC_LOGL ? 1 : 3),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, compression), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SGILOGDATAFMT, data_fmt), 1);
}

std::vector<uint8_t> BuildStripedSgilogTiff(
    uint32_t width, uint32_t height, uint32_t rows_per_strip,
    uint16_t compression, uint16_t photometric, uint16_t samples_per_pixel,
    uint16_t bits_per_sample, std::optional<uint16_t> sample_format,
    uint16_t planar_config, const std::vector<std::vector<uint8_t>>& strips,
    bool big_endian = false) {
  RawTiffBuilder builder(big_endian, /*big_tiff=*/false);
  std::vector<uint32_t> offsets;
  std::vector<uint32_t> counts;
  offsets.reserve(strips.size());
  counts.reserve(strips.size());
  for (const auto& strip : strips) {
    offsets.push_back(static_cast<uint32_t>(builder.AppendBlob(strip)));
    counts.push_back(static_cast<uint32_t>(strip.size()));
  }

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bits_per_sample),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, photometric),
      builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offsets),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, counts),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, planar_config),
  };
  if (sample_format.has_value()) {
    entries.push_back(builder.MakeShort(TIFFTAG_SAMPLEFORMAT, *sample_format));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint8_t> BuildTiledSgilogTiff(
    uint32_t width, uint32_t height, uint32_t tile_width, uint32_t tile_height,
    uint16_t compression, uint16_t photometric, uint16_t samples_per_pixel,
    uint16_t bits_per_sample, uint16_t sample_format,
    const std::vector<std::vector<uint8_t>>& tiles) {
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  std::vector<uint32_t> offsets;
  std::vector<uint32_t> counts;
  offsets.reserve(tiles.size());
  counts.reserve(tiles.size());
  for (const auto& tile : tiles) {
    offsets.push_back(static_cast<uint32_t>(builder.AppendBlob(tile)));
    counts.push_back(static_cast<uint32_t>(tile.size()));
  }
  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bits_per_sample),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, photometric),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
      builder.MakeLong(TIFFTAG_TILEWIDTH, tile_width),
      builder.MakeLong(TIFFTAG_TILELENGTH, tile_height),
      builder.MakeLongArray(TIFFTAG_TILEOFFSETS, offsets),
      builder.MakeLongArray(TIFFTAG_TILEBYTECOUNTS, counts),
      builder.MakeShort(TIFFTAG_SAMPLEFORMAT, sample_format),
  };
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

TEST(TifLuvTest, CodecRegistrationAndConfiguredList) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_SGILOG), 1);
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_SGILOG24), 1);

  const TIFFCodec* sgilog = TIFFFindCODEC(COMPRESSION_SGILOG);
  ASSERT_NE(sgilog, nullptr);
  EXPECT_STREQ(sgilog->name, "SGILog");
  EXPECT_EQ(sgilog->scheme, COMPRESSION_SGILOG);

  const TIFFCodec* sgilog24 = TIFFFindCODEC(COMPRESSION_SGILOG24);
  ASSERT_NE(sgilog24, nullptr);
  EXPECT_STREQ(sgilog24->name, "SGILog24");
  EXPECT_EQ(sgilog24->scheme, COMPRESSION_SGILOG24);

  UniqueTiffBuffer<TIFFCodec> configured(TIFFGetConfiguredCODECs());
  ASSERT_NE(configured.get(), nullptr);
  bool found32 = false;
  bool found24 = false;
  for (const TIFFCodec* c = configured.get(); c->name != nullptr; ++c) {
    if (c->scheme == COMPRESSION_SGILOG) found32 = true;
    if (c->scheme == COMPRESSION_SGILOG24) found24 = true;
  }
  EXPECT_TRUE(found32);
  EXPECT_TRUE(found24);
}

TEST(TifLuvTest, LogL16PublicConversionsAndBoundaries) {
  EXPECT_DOUBLE_EQ(LogL16toY(0), 0.0);
  EXPECT_DOUBLE_EQ(LogL16toY(0x8000), 0.0);

  // Positive, negative, and boundary inputs (NODITHER).
  EXPECT_EQ(LogL16fromY(0.0, SGILOGENCODE_NODITHER), 0);
  EXPECT_EQ(LogL16fromY(1e-20, SGILOGENCODE_NODITHER), 0);
  EXPECT_EQ(LogL16fromY(-1e-20, SGILOGENCODE_NODITHER), 0);
  EXPECT_EQ(LogL16fromY(std::numeric_limits<double>::quiet_NaN(),
                        SGILOGENCODE_NODITHER),
            0);
  EXPECT_EQ(LogL16fromY(1.8371976e19, SGILOGENCODE_NODITHER), 0x7fff);
  EXPECT_EQ(LogL16fromY(1e25, SGILOGENCODE_NODITHER), 0x7fff);
  EXPECT_EQ(LogL16fromY(-1.8371976e19, SGILOGENCODE_NODITHER), 0xffff);
  EXPECT_EQ(LogL16fromY(-1e25, SGILOGENCODE_NODITHER), 0xffff);

  for (double y : {1e-10, 1e-4, 0.25, 1.0, 42.5, 1e6, 1e14}) {
    const int p16_pos = LogL16fromY(y, SGILOGENCODE_NODITHER);
    EXPECT_GT(p16_pos, 0);
    EXPECT_LT(p16_pos, 0x7fff);
    const double decoded_pos = LogL16toY(p16_pos);
    EXPECT_NEAR(decoded_pos, y, y * 0.01);

    const int p16_neg = LogL16fromY(-y, SGILOGENCODE_NODITHER);
    EXPECT_NE(p16_neg & 0x8000, 0);
    const double decoded_neg = LogL16toY(p16_neg & 0xffff);
    EXPECT_LT(decoded_neg, 0.0);
    EXPECT_NEAR(decoded_neg, -y, y * 0.01);
  }

  // Random dithering should stay within a tight tolerance band.
  for (int i = 0; i < 8; ++i) {
    const int p_rand = LogL16fromY(2.5, SGILOGENCODE_RANDITHER);
    EXPECT_NEAR(LogL16toY(p_rand), 2.5, 0.05);
    const int p_neg_rand = LogL16fromY(-2.5, SGILOGENCODE_RANDITHER);
    EXPECT_NEAR(LogL16toY(p_neg_rand & 0xffff), -2.5, 0.05);
  }
}

TEST(TifLuvTest, LogL10AndXYZtoRGB24PublicConversions) {
  EXPECT_DOUBLE_EQ(LogL10toY(0), 0.0);
  EXPECT_EQ(LogL10fromY(0.0, SGILOGENCODE_NODITHER), 0);
  EXPECT_EQ(LogL10fromY(0.00024283, SGILOGENCODE_NODITHER), 0);
  EXPECT_EQ(LogL10fromY(-5.0, SGILOGENCODE_NODITHER), 0);
  EXPECT_EQ(LogL10fromY(15.742, SGILOGENCODE_NODITHER), 0x3ff);
  EXPECT_EQ(LogL10fromY(100.0, SGILOGENCODE_NODITHER), 0x3ff);

  for (double y : {0.001, 0.05, 0.5, 1.0, 7.5}) {
    const int p10 = LogL10fromY(y, SGILOGENCODE_NODITHER);
    EXPECT_GT(p10, 0);
    EXPECT_LT(p10, 0x3ff);
    EXPECT_NEAR(LogL10toY(p10), y, y * 0.03);

    const int p10_rand = LogL10fromY(y, SGILOGENCODE_RANDITHER);
    EXPECT_GE(p10_rand, 0);
    EXPECT_LE(p10_rand, 0x3ff);
    EXPECT_NEAR(LogL10toY(p10_rand), y, y * 0.06);
  }

  // XYZtoRGB24 zero, clamped low, mid, and clamped high.
  std::array<uint8_t, 3> rgb{};
  std::array<float, 3> xyz_zero = {0.0f, 0.0f, 0.0f};
  XYZtoRGB24(xyz_zero.data(), rgb.data());
  EXPECT_EQ(rgb, (std::array<uint8_t, 3>{0, 0, 0}));

  std::array<float, 3> xyz_high = {2.0f, 2.0f, 2.0f};
  XYZtoRGB24(xyz_high.data(), rgb.data());
  EXPECT_EQ(rgb, (std::array<uint8_t, 3>{255, 255, 255}));

  std::array<float, 3> xyz_blue_heavy = {0.15f, 0.08f, 1.5f};
  XYZtoRGB24(xyz_blue_heavy.data(), rgb.data());
  EXPECT_EQ(rgb[0], 0);
  EXPECT_GT(rgb[1], 0);
  EXPECT_EQ(rgb[2], 255);

  std::array<float, 3> xyz_mid = {0.35f, 0.35f, 0.35f};
  XYZtoRGB24(xyz_mid.data(), rgb.data());
  EXPECT_GT(rgb[0], 100);
  EXPECT_LT(rgb[0], 200);
  EXPECT_GT(rgb[1], 100);
  EXPECT_GT(rgb[2], 100);
}

TEST(TifLuvTest, UvEncodeDecodeTableAndOutOfGamutBoundaries) {
  // Invalid decode indices.
  double u = 0.0;
  double v = 0.0;
  EXPECT_EQ(uv_decode(&u, &v, -1), -1);
  EXPECT_EQ(uv_decode(&u, &v, UV_NDIVS), -1);
  EXPECT_EQ(uv_decode(&u, &v, UV_NDIVS + 100), -1);

  // NaN maps to neutral white point.
  const double nan_val = std::numeric_limits<double>::quiet_NaN();
  const int c_nan_u = uv_encode(nan_val, V_NEU, SGILOGENCODE_NODITHER);
  const int c_nan_v = uv_encode(U_NEU, nan_val, SGILOGENCODE_NODITHER);
  EXPECT_EQ(c_nan_u, c_nan_v);
  EXPECT_EQ(uv_decode(&u, &v, c_nan_u), 0);
  EXPECT_NEAR(u, U_NEU, 0.005);
  EXPECT_NEAR(v, V_NEU, 0.005);

  // In-gamut encoding with NODITHER and RANDITHER.
  const int c_neu = uv_encode(U_NEU, V_NEU, SGILOGENCODE_NODITHER);
  EXPECT_GE(c_neu, 0);
  EXPECT_LT(c_neu, UV_NDIVS);
  EXPECT_EQ(uv_decode(&u, &v, c_neu), 0);
  EXPECT_NEAR(u, U_NEU, 0.005);
  EXPECT_NEAR(v, V_NEU, 0.005);

  const int c_neu_rand = uv_encode(U_NEU, V_NEU, SGILOGENCODE_RANDITHER);
  EXPECT_GE(c_neu_rand, 0);
  EXPECT_LT(c_neu_rand, UV_NDIVS);

  // Exercise exact binary search boundaries (first, middle exact ncum hit,
  // last) and sweep across every uv_row entry.
  for (int vi = 0; vi < UV_NVS; ++vi) {
    const int c_start = uv_row[vi].ncum;
    ASSERT_EQ(uv_decode(&u, &v, c_start), 0);
    const double expected_u =
        static_cast<double>(uv_row[vi].ustart) + 0.5 * UV_SQSIZ;
    const double expected_v =
        static_cast<double>(UV_VSTART) + (vi + 0.5) * UV_SQSIZ;
    EXPECT_NEAR(u, expected_u, 1e-6);
    EXPECT_NEAR(v, expected_v, 1e-6);
  }
  EXPECT_EQ(uv_decode(&u, &v, UV_NDIVS - 1), 0);

  // Exercise all out-of-gamut perimeter cases (v < UV_VSTART, vi >= UV_NVS,
  // u < ustart, ui >= nus) and full hue angle sweep for oog_encode.
  EXPECT_GE(uv_encode(U_NEU, -0.1, SGILOGENCODE_NODITHER), 0);
  EXPECT_GE(uv_encode(U_NEU, 1.0, SGILOGENCODE_NODITHER), 0);
  EXPECT_GE(uv_encode(-0.1, 0.30, SGILOGENCODE_NODITHER), 0);
  EXPECT_GE(uv_encode(0.95, 0.30, SGILOGENCODE_NODITHER), 0);

  constexpr double kPi = 3.14159265358979323846;
  for (int step = 0; step < 180; ++step) {
    const double angle = (2.0 * kPi * step) / 180.0;
    const double uu = U_NEU + 0.45 * std::cos(angle);
    const double vv = V_NEU + 0.45 * std::sin(angle);
    const int c_oog = uv_encode(uu, vv, SGILOGENCODE_NODITHER);
    EXPECT_GE(c_oog, 0);
    EXPECT_LT(c_oog, UV_NDIVS);
  }
}

TEST(TifLuvTest, LogLuv24AndLogLuv32PublicConversions) {
  std::array<float, 3> out_xyz{};

  // Zero luminance in 24-bit and 32-bit formats.
  std::array<float, 3> xyz_zero = {0.0f, 0.0f, 0.0f};
  EXPECT_NE(LogLuv24fromXYZ(xyz_zero.data(), SGILOGENCODE_NODITHER) >> 14, 1);
  LogLuv24toXYZ(0, out_xyz.data());
  EXPECT_EQ(out_xyz, (std::array<float, 3>{0.0f, 0.0f, 0.0f}));

  LogLuv32toXYZ(0, out_xyz.data());
  EXPECT_EQ(out_xyz, (std::array<float, 3>{0.0f, 0.0f, 0.0f}));
  EXPECT_EQ(LogLuv32fromXYZ(xyz_zero.data(), SGILOGENCODE_NODITHER) >> 16, 0u);

  // s <= 0 fallback to neutral chromaticity in both 24 and 32 bit.
  std::array<float, 3> xyz_neg_s = {-5.0f, 0.5f, -5.0f};
  const uint32_t p24_neg_s =
      LogLuv24fromXYZ(xyz_neg_s.data(), SGILOGENCODE_NODITHER);
  LogLuv24toXYZ(p24_neg_s, out_xyz.data());
  EXPECT_NEAR(out_xyz[1], 0.5f, 0.02f);
  EXPECT_NEAR(out_xyz[0], out_xyz[1], 0.05f);

  const uint32_t p32_neg_s =
      LogLuv32fromXYZ(xyz_neg_s.data(), SGILOGENCODE_NODITHER);
  LogLuv32toXYZ(p32_neg_s, out_xyz.data());
  EXPECT_NEAR(out_xyz[1], 0.5f, 0.01f);
  EXPECT_NEAR(out_xyz[0], out_xyz[1], 0.02f);

  // Invalid Ce (>= UV_NDIVS, within 14 bits) falls back to neutral in
  // LogLuv24toXYZ.
  const uint32_t crafted_bad_ce = (500u << 14) | 16300u;
  LogLuv24toXYZ(crafted_bad_ce, out_xyz.data());
  EXPECT_GT(out_xyz[1], 0.0f);
  EXPECT_NEAR(out_xyz[0], out_xyz[1], out_xyz[1] * 0.01f);
  EXPECT_NEAR(out_xyz[2], out_xyz[1], out_xyz[1] * 0.01f);

  // Normal round-trip for 24-bit and 32-bit (NODITHER and RANDITHER).
  for (const std::array<float, 3> sample : {
           std::array<float, 3>{1.0f, 1.0f, 1.0f},
           std::array<float, 3>{0.55f, 0.30f, 0.08f},
           std::array<float, 3>{0.18f, 0.09f, 0.85f},
       }) {
    std::array<float, 3> in24 = sample;
    const uint32_t p24 = LogLuv24fromXYZ(in24.data(), SGILOGENCODE_NODITHER);
    LogLuv24toXYZ(p24, out_xyz.data());
    EXPECT_NEAR(out_xyz[0], sample[0], 0.05f);
    EXPECT_NEAR(out_xyz[1], sample[1], 0.03f);
    EXPECT_NEAR(out_xyz[2], sample[2], 0.05f);

    const uint32_t p24r = LogLuv24fromXYZ(in24.data(), SGILOGENCODE_RANDITHER);
    LogLuv24toXYZ(p24r, out_xyz.data());
    EXPECT_NEAR(out_xyz[1], sample[1], 0.06f);

    std::array<float, 3> in32 = sample;
    const uint32_t p32 = LogLuv32fromXYZ(in32.data(), SGILOGENCODE_NODITHER);
    LogLuv32toXYZ(p32, out_xyz.data());
    EXPECT_NEAR(out_xyz[0], sample[0], 0.02f);
    EXPECT_NEAR(out_xyz[1], sample[1], 0.01f);
    EXPECT_NEAR(out_xyz[2], sample[2], 0.02f);

    const uint32_t p32r = LogLuv32fromXYZ(in32.data(), SGILOGENCODE_RANDITHER);
    LogLuv32toXYZ(p32r, out_xyz.data());
    EXPECT_NEAR(out_xyz[1], sample[1], 0.02f);
  }

  // Clamping branches in LogLuv32fromXYZ: u <= 0, ue > 255, v <= 0, ve > 255.
  std::array<float, 3> xyz_u_low = {-1.0f, 0.5f, 0.5f};
  EXPECT_EQ(
      (LogLuv32fromXYZ(xyz_u_low.data(), SGILOGENCODE_NODITHER) >> 8) & 0xffu,
      0u);

  std::array<float, 3> xyz_u_high = {100.0f, 0.5f, 0.0f};
  EXPECT_EQ(
      (LogLuv32fromXYZ(xyz_u_high.data(), SGILOGENCODE_NODITHER) >> 8) & 0xffu,
      255u);

  std::array<float, 3> xyz_v_low = {10.0f, -0.05f, 0.0f};
  EXPECT_EQ(LogLuv32fromXYZ(xyz_v_low.data(), SGILOGENCODE_NODITHER) & 0xffu,
            0u);

  std::array<float, 3> xyz_v_high = {0.0f, 1.0f, -4.5f};
  EXPECT_EQ(LogLuv32fromXYZ(xyz_v_high.data(), SGILOGENCODE_NODITHER) & 0xffu,
            255u);
}

TEST(TifLuvTest, SgiLogLogLFloat16Bit8BitAndRgbaRoundTrip) {
  constexpr uint32_t kWidth = 8;
  constexpr uint32_t kHeight = 4;
  MemoryTiffStream stream;

  const std::vector<float> input = {
      0.0f,  1e-5f, 0.04f, 0.25f, 0.50f, 1.0f,  4.0f,   -0.25f,
      1e-3f, 0.09f, 0.36f, 0.64f, 0.81f, 2.25f, 16.0f,  -4.0f,
      0.01f, 0.16f, 0.49f, 1.50f, 3.0f,  8.5f,  100.0f, -1e-2f,
      0.0f,  0.20f, 0.75f, 1.0f,  2.0f,  5.0f,  25.0f,  0.0f,
  };

  {
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), kWidth, kHeight, /*rows_per_strip=*/2,
                     COMPRESSION_SGILOG, PHOTOMETRIC_LOGL, SGILOGDATAFMT_FLOAT);
    int data_fmt = -1;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SGILOGDATAFMT, &data_fmt), 1);
    EXPECT_EQ(data_fmt, SGILOGDATAFMT_FLOAT);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SGILOGENCODE, SGILOGENCODE_RANDITHER),
        1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_STONITS, 1.0), 1);

    for (uint32_t r = 0; r < kHeight; ++r) {
      std::vector<float> row(input.begin() + r * kWidth,
                             input.begin() + (r + 1) * kWidth);
      ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), r, 0), 1);
    }
  }

  // Read back as FLOAT.
  {
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    uint16_t spp = 0;
    uint16_t bps = 0;
    uint16_t sfmt = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp), 1);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SAMPLEFORMAT, &sfmt), 1);
    EXPECT_EQ(spp, 1);
    EXPECT_EQ(bps, 16);
    EXPECT_EQ(sfmt, SAMPLEFORMAT_INT);

    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_FLOAT), 1);
    std::vector<float> row(kWidth);
    for (uint32_t r = 0; r < kHeight; ++r) {
      ASSERT_EQ(TIFFReadScanline(tif.get(), row.data(), r, 0), 1);
      for (uint32_t c = 0; c < kWidth; ++c) {
        const float expected = input[r * kWidth + c];
        if (expected == 0.0f) {
          EXPECT_FLOAT_EQ(row[c], 0.0f);
        } else {
          EXPECT_NEAR(row[c], expected, std::abs(expected) * 0.02f);
        }
      }
    }
  }

  // Read back as 8BIT grayscale (L16toGry).
  {
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_8BIT), 1);
    std::vector<uint8_t> row(kWidth);
    ASSERT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), 1);
    EXPECT_EQ(row[0], 0);    // 0.0 -> 0
    EXPECT_EQ(row[5], 255);  // 1.0 -> 255
    EXPECT_EQ(row[6], 255);  // 4.0 -> 255
    EXPECT_EQ(row[7], 0);    // -0.25 -> 0
    EXPECT_GT(row[3], 100);  // 0.25 -> ~128
    EXPECT_LT(row[3], 150);
  }

  // Read back via ScopedRgbaImage.
  {
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    char emsg[1024] = {};
    ScopedRgbaImage rgba;
    ASSERT_TRUE(rgba.Begin(tif.get(), /*stop_on_error=*/1, emsg)) << emsg;
    std::vector<uint32_t> raster(kWidth * kHeight, 0);
    EXPECT_EQ(rgba.Get(raster.data(), kWidth, kHeight), 1);
    EXPECT_GT(TIFFGetR(raster[5]), 200u);
  }

  // Direct 16BIT LogL round-trip with RowsPerStrip >= ImageLength.
  MemoryTiffStream s16;
  const std::vector<int16_t> raw16 = {
      0, 12000, 14000, 16384, 16384, 16384, 16384, -10000,
      1, 2,     3,     4,     5,     6,     7,     8,
  };
  {
    UniqueTiff tif = s16.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), 8, 2, /*rows_per_strip=*/4, COMPRESSION_SGILOG,
                     PHOTOMETRIC_LOGL, SGILOGDATAFMT_16BIT);
    std::vector<int16_t> write_buf = raw16;
    ASSERT_EQ(TIFFWriteEncodedStrip(
                  tif.get(), 0, write_buf.data(),
                  static_cast<tmsize_t>(write_buf.size() * sizeof(int16_t))),
              static_cast<tmsize_t>(write_buf.size() * sizeof(int16_t)));
  }
  {
    UniqueTiff tif = s16.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_16BIT), 1);
    std::vector<int16_t> out(raw16.size(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(
                  tif.get(), 0, out.data(),
                  static_cast<tmsize_t>(out.size() * sizeof(int16_t))),
              static_cast<tmsize_t>(out.size() * sizeof(int16_t)));
    EXPECT_EQ(out, raw16);
  }
}

TEST(TifLuvTest, SgiLogLogLuvFloat16BitRaw8BitAndRgbaRoundTrip) {
  constexpr uint32_t kWidth = 6;
  constexpr uint32_t kHeight = 2;
  const std::vector<float> xyz_pixels = {
      0.0f,  0.0f,  0.0f,   // black
      1.0f,  1.0f,  1.0f,   // white
      0.5f,  0.25f, 0.05f,  // warm
      0.2f,  0.6f,  0.1f,   // green
      0.15f, 0.08f, 0.8f,   // blue
      2.5f,  2.5f,  2.5f,   // bright white
      0.4f,  0.4f,  0.4f,  0.9f, 0.3f, 0.1f, 0.1f, 0.7f, 0.2f,
      0.2f,  0.1f,  0.9f,  0.0f, 0.0f, 0.0f, 0.6f, 0.6f, 0.6f,
  };

  // FLOAT round-trip + 8BIT decode + RGBA image.
  MemoryTiffStream float_stream;
  {
    UniqueTiff tif = float_stream.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), kWidth, kHeight, /*rows_per_strip=*/1,
                     COMPRESSION_SGILOG, PHOTOMETRIC_LOGLUV,
                     SGILOGDATAFMT_FLOAT);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SGILOGENCODE, SGILOGENCODE_RANDITHER),
        1);
    for (uint32_t r = 0; r < kHeight; ++r) {
      std::vector<float> row(xyz_pixels.begin() + r * kWidth * 3,
                             xyz_pixels.begin() + (r + 1) * kWidth * 3);
      ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), r, 0), 1);
    }
  }
  {
    UniqueTiff tif = float_stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_FLOAT), 1);
    std::vector<float> row(kWidth * 3);
    ASSERT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), 1);
    EXPECT_FLOAT_EQ(row[0], 0.0f);
    EXPECT_FLOAT_EQ(row[1], 0.0f);
    EXPECT_FLOAT_EQ(row[2], 0.0f);
    EXPECT_NEAR(row[3], 1.0f, 0.03f);
    EXPECT_NEAR(row[4], 1.0f, 0.02f);
    EXPECT_NEAR(row[5], 1.0f, 0.03f);
  }
  {
    UniqueTiff tif = float_stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_8BIT), 1);
    std::vector<uint8_t> rgb(kWidth * 3);
    ASSERT_EQ(TIFFReadScanline(tif.get(), rgb.data(), 0, 0), 1);
    EXPECT_EQ(rgb[0], 0);
    EXPECT_EQ(rgb[1], 0);
    EXPECT_EQ(rgb[2], 0);
    EXPECT_GE(rgb[3], 240);
    EXPECT_GE(rgb[4], 240);
    EXPECT_GE(rgb[5], 240);
  }
  {
    UniqueTiff tif = float_stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    char emsg[1024] = {};
    ScopedRgbaImage rgba;
    ASSERT_TRUE(rgba.Begin(tif.get(), /*stop_on_error=*/1, emsg)) << emsg;
    std::vector<uint32_t> raster(kWidth * kHeight, 0);
    EXPECT_EQ(rgba.Get(raster.data(), kWidth, kHeight), 1);
  }

  // 16BIT (Luv48) round-trip under both NODITHER and RANDITHER.
  for (int enc : {SGILOGENCODE_NODITHER, SGILOGENCODE_RANDITHER}) {
    MemoryTiffStream luv48_stream;
    const std::vector<int16_t> luv48_in = {
        16384, 6800, 15500, 12000,  10000, 12000,
        18000, 4000, 18000, -14000, 7000,  15000,
    };
    {
      UniqueTiff tif = luv48_stream.Open("w");
      ASSERT_NE(tif.get(), nullptr);
      ConfigureStriped(tif.get(), 2, 2, /*rows_per_strip=*/2,
                       COMPRESSION_SGILOG, PHOTOMETRIC_LOGLUV,
                       SGILOGDATAFMT_16BIT);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SGILOGENCODE, enc), 1);
      std::vector<int16_t> write_buf = luv48_in;
      ASSERT_EQ(TIFFWriteEncodedStrip(
                    tif.get(), 0, write_buf.data(),
                    static_cast<tmsize_t>(write_buf.size() * sizeof(int16_t))),
                static_cast<tmsize_t>(write_buf.size() * sizeof(int16_t)));
    }
    {
      UniqueTiff tif = luv48_stream.Open("r");
      ASSERT_NE(tif.get(), nullptr);
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_16BIT),
          1);
      std::vector<int16_t> out(luv48_in.size(), 0);
      ASSERT_EQ(TIFFReadEncodedStrip(
                    tif.get(), 0, out.data(),
                    static_cast<tmsize_t>(out.size() * sizeof(int16_t))),
                static_cast<tmsize_t>(out.size() * sizeof(int16_t)));
      for (size_t i = 0; i < luv48_in.size(); i += 3) {
        EXPECT_EQ(out[i], luv48_in[i]);
        EXPECT_NEAR(out[i + 1], luv48_in[i + 1], 300);
        EXPECT_NEAR(out[i + 2], luv48_in[i + 2], 300);
      }
    }
  }

  // RAW uint32_t round-trip (verifies LogLuvVSetField sets SAMPLESPERPIXEL=1
  // and LogLuvClose restores SAMPLESPERPIXEL=3 on disk).
  MemoryTiffStream raw_stream;
  const std::vector<uint32_t> raw_in = {0x400056c2u, 0x35003080u, 0x80004040u,
                                        0x400056c2u};
  {
    UniqueTiff tif = raw_stream.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), 4, 1, /*rows_per_strip=*/1, COMPRESSION_SGILOG,
                     PHOTOMETRIC_LOGLUV, SGILOGDATAFMT_RAW);
    uint16_t spp_in_ram = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp_in_ram), 1);
    EXPECT_EQ(spp_in_ram, 1);
    std::vector<uint32_t> write_buf = raw_in;
    ASSERT_EQ(TIFFWriteScanline(tif.get(), write_buf.data(), 0, 0), 1);
  }
  {
    UniqueTiff tif = raw_stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    uint16_t spp_on_disk = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp_on_disk),
              1);
    EXPECT_EQ(spp_on_disk, 3);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_RAW),
              1);
    std::vector<uint32_t> out(4, 0);
    ASSERT_EQ(TIFFReadScanline(tif.get(), out.data(), 0, 0), 1);
    EXPECT_EQ(out, raw_in);
  }
}

TEST(TifLuvTest, SgiLog24LogLuvFloat16BitRaw8BitAndInvalidCeFallback) {
  constexpr uint32_t kWidth = 4;
  constexpr uint32_t kHeight = 2;
  const std::vector<float> xyz_pixels = {
      0.0f, 0.0f, 0.0f, 1.0f,  1.0f,  1.0f,  0.6f, 0.3f, 0.1f, 0.2f, 0.5f, 0.9f,
      0.4f, 0.4f, 0.4f, 0.01f, 0.02f, 0.03f, 5.0f, 5.0f, 5.0f, 0.8f, 0.2f, 0.1f,
  };

  MemoryTiffStream float_stream;
  {
    UniqueTiff tif = float_stream.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), kWidth, kHeight, /*rows_per_strip=*/1,
                     COMPRESSION_SGILOG24, PHOTOMETRIC_LOGLUV,
                     SGILOGDATAFMT_FLOAT);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SGILOGENCODE, SGILOGENCODE_NODITHER),
        1);
    for (uint32_t r = 0; r < kHeight; ++r) {
      std::vector<float> row(xyz_pixels.begin() + r * kWidth * 3,
                             xyz_pixels.begin() + (r + 1) * kWidth * 3);
      ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), r, 0), 1);
    }
  }
  {
    UniqueTiff tif = float_stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_FLOAT), 1);
    std::vector<float> row(kWidth * 3);
    ASSERT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), 1);
    EXPECT_FLOAT_EQ(row[0], 0.0f);
    EXPECT_FLOAT_EQ(row[1], 0.0f);
    EXPECT_FLOAT_EQ(row[2], 0.0f);
    EXPECT_NEAR(row[4], 1.0f, 0.03f);
  }
  {
    UniqueTiff tif = float_stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_8BIT), 1);
    std::vector<uint8_t> rgb(kWidth * 3);
    ASSERT_EQ(TIFFReadScanline(tif.get(), rgb.data(), 0, 0), 1);
    EXPECT_EQ(rgb[0], 0);
    EXPECT_GE(rgb[3], 240);
    EXPECT_GE(rgb[4], 240);
    EXPECT_GE(rgb[5], 240);
  }
  {
    UniqueTiff tif = float_stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    char emsg[1024] = {};
    ScopedRgbaImage rgba;
    ASSERT_TRUE(rgba.Begin(tif.get(), /*stop_on_error=*/1, emsg)) << emsg;
    std::vector<uint32_t> raster(kWidth * kHeight, 0);
    EXPECT_EQ(rgba.Get(raster.data(), kWidth, kHeight), 1);
  }

  // 16BIT (Luv48 <-> Luv24): cover <= 0, >= (1<<12)+3314, and valid [3314,
  // 7409] under both NODITHER and RANDITHER. (Avoid (0, 3314) due to upstream
  // signed negative right shift in Luv24fromLuv48.)
  for (int enc : {SGILOGENCODE_NODITHER, SGILOGENCODE_RANDITHER}) {
    MemoryTiffStream luv48_stream;
    const std::vector<int16_t> luv48_in = {
        0,    6800, 15500, -100, 6800, 15500, 3314, 6800, 15500,
        5500, 7000, 15000, 7410, 6800, 15500, 9000, 6800, 15500,
    };
    {
      UniqueTiff tif = luv48_stream.Open("w");
      ASSERT_NE(tif.get(), nullptr);
      ConfigureStriped(tif.get(), 6, 1, /*rows_per_strip=*/1,
                       COMPRESSION_SGILOG24, PHOTOMETRIC_LOGLUV,
                       SGILOGDATAFMT_16BIT);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SGILOGENCODE, enc), 1);
      std::vector<int16_t> write_buf = luv48_in;
      ASSERT_EQ(TIFFWriteScanline(tif.get(), write_buf.data(), 0, 0), 1);
    }
    {
      UniqueTiff tif = luv48_stream.Open("r");
      ASSERT_NE(tif.get(), nullptr);
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_16BIT),
          1);
      std::vector<int16_t> out(luv48_in.size(), 0);
      ASSERT_EQ(TIFFReadScanline(tif.get(), out.data(), 0, 0), 1);
      EXPECT_EQ(out[0], 13314);
      EXPECT_EQ(out[3], 13314);
      EXPECT_GT(out[9], 13314);
      EXPECT_EQ(out[12], 17406);
      EXPECT_EQ(out[15], 17406);
    }
  }

  // RAW 24-bit round-trip + crafted Ce >= UV_NDIVS fallback in Luv24toLuv48
  // and Luv24toXYZ.
  MemoryTiffStream raw24_stream;
  const uint32_t valid_p24 = (512u << 14) | 1000u;
  const uint32_t invalid_ce_p24 = (512u << 14) | 16300u;
  const std::vector<uint32_t> raw_in = {valid_p24, invalid_ce_p24};
  {
    UniqueTiff tif = raw24_stream.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), 2, 1, /*rows_per_strip=*/1,
                     COMPRESSION_SGILOG24, PHOTOMETRIC_LOGLUV,
                     SGILOGDATAFMT_RAW);
    std::vector<uint32_t> write_buf = raw_in;
    ASSERT_EQ(TIFFWriteScanline(tif.get(), write_buf.data(), 0, 0), 1);
  }
  {
    UniqueTiff tif = raw24_stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_RAW),
              1);
    std::vector<uint32_t> raw_out(2, 0);
    ASSERT_EQ(TIFFReadScanline(tif.get(), raw_out.data(), 0, 0), 1);
    EXPECT_EQ(raw_out, raw_in);
  }
  {
    UniqueTiff tif = raw24_stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_16BIT), 1);
    std::vector<int16_t> luv48_out(6, 0);
    ASSERT_EQ(TIFFReadScanline(tif.get(), luv48_out.data(), 0, 0), 1);
    EXPECT_NEAR(luv48_out[4], static_cast<int16_t>(U_NEU * 32768), 2);
    EXPECT_NEAR(luv48_out[5], static_cast<int16_t>(V_NEU * 32768), 2);
  }
}

TEST(TifLuvTest, TiledRoundTripsEndiannessAndBigTiff) {
  // Tiled LogL, LogLuv32, and LogLuv24 across multi-row tiles.
  for (const uint16_t compression :
       {COMPRESSION_SGILOG, COMPRESSION_SGILOG24}) {
    for (const uint16_t photometric : {PHOTOMETRIC_LOGL, PHOTOMETRIC_LOGLUV}) {
      if (compression == COMPRESSION_SGILOG24 &&
          photometric == PHOTOMETRIC_LOGL) {
        continue;
      }
      MemoryTiffStream stream;
      const uint32_t kWidth = 32;
      const uint32_t kHeight = 32;
      const uint32_t kTileW = 16;
      const uint32_t kTileH = 16;
      const uint32_t comps = photometric == PHOTOMETRIC_LOGL ? 1 : 3;
      std::vector<float> tile_in(kTileW * kTileH * comps, 0.75f);
      for (size_t i = 0; i < tile_in.size(); ++i) {
        tile_in[i] = 0.25f + 0.01f * static_cast<float>(i % 20);
      }

      {
        UniqueTiff tif = stream.Open("w");
        ASSERT_NE(tif.get(), nullptr);
        ConfigureTiled(tif.get(), kWidth, kHeight, kTileW, kTileH, compression,
                       photometric, SGILOGDATAFMT_FLOAT);
        const tmsize_t tsize = TIFFTileSize(tif.get());
        ASSERT_EQ(tsize, static_cast<tmsize_t>(tile_in.size() * sizeof(float)));
        for (uint32_t t = 0; t < TIFFNumberOfTiles(tif.get()); ++t) {
          std::vector<float> tile_write = tile_in;
          ASSERT_EQ(
              TIFFWriteEncodedTile(tif.get(), t, tile_write.data(), tsize),
              tsize);
        }
      }

      {
        UniqueTiff tif = stream.Open("r");
        ASSERT_NE(tif.get(), nullptr);
        EXPECT_EQ(
            TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_FLOAT),
            1);
        const tmsize_t tsize = TIFFTileSize(tif.get());
        std::vector<float> tile_out(kTileW * kTileH * comps, 0.0f);
        for (uint32_t t = 0; t < TIFFNumberOfTiles(tif.get()); ++t) {
          ASSERT_EQ(TIFFReadEncodedTile(tif.get(), t, tile_out.data(), tsize),
                    tsize);
          for (size_t i = 0; i < tile_out.size(); ++i) {
            EXPECT_NEAR(tile_out[i], tile_in[i], 0.05f);
          }
        }
      }
    }
  }

  // Native little-endian ClassicTIFF & BigTIFF write/read round-trips, plus
  // reading big-endian ('MM') ClassicTIFF and BigTIFF SGILog streams (testing
  // LogLuvSetupDecode overriding tif_postdecode to _TIFFNoPostDecode). Note:
  // writing opposite-endian ('wb'/'w8b') SGILog is skipped here because
  // upstream LogLuvSetupEncode omits tif->tif_postdecode = _TIFFNoPostDecode.
  for (const char* mode : {"wl", "w8l"}) {
    SCOPED_TRACE(mode);
    MemoryTiffStream stream;
    const std::vector<float> xyz_in = {0.8f, 0.9f, 1.0f, 0.3f, 0.4f, 0.2f};
    {
      UniqueTiff tif = stream.Open(mode);
      ASSERT_NE(tif.get(), nullptr);
      ConfigureStriped(tif.get(), 2, 1, /*rows_per_strip=*/1,
                       COMPRESSION_SGILOG, PHOTOMETRIC_LOGLUV,
                       SGILOGDATAFMT_FLOAT);
      std::vector<float> write_row = xyz_in;
      ASSERT_EQ(TIFFWriteScanline(tif.get(), write_row.data(), 0, 0), 1);
    }
    {
      UniqueTiff tif = stream.Open("r");
      ASSERT_NE(tif.get(), nullptr);
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_FLOAT),
          1);
      std::vector<float> xyz_out(6, 0.0f);
      ASSERT_EQ(TIFFReadScanline(tif.get(), xyz_out.data(), 0, 0), 1);
      for (size_t i = 0; i < xyz_in.size(); ++i) {
        EXPECT_NEAR(xyz_out[i], xyz_in[i], 0.02f);
      }
    }
  }

  for (const bool big_tiff : {false, true}) {
    SCOPED_TRACE(big_tiff ? "BigEndianBigTiffRead" : "BigEndianClassicRead");
    // 2 identical pixels encoded as SGILog32: 0x400056c2 (near white Y=1).
    const std::vector<uint8_t> strip = {0x02, 0x40, 0x40, 0x02, 0x00, 0x00,
                                        0x02, 0x56, 0x56, 0x02, 0xc2, 0xc2};
    RawTiffBuilder builder(/*big_endian=*/true, big_tiff);
    const uint64_t offset = builder.AppendBlob(strip);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 2),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 1),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 16),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_SGILOG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_LOGLUV),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(offset)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 1),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(strip.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        builder.MakeShort(TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_INT),
    });
    MemoryTiffStream s(builder.Take());
    UniqueTiff tif = s.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(TIFFIsByteSwapped(tif.get()), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_RAW),
              1);
    std::array<uint32_t, 2> out{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), out.data(), 0, 0), 1);
    EXPECT_EQ(out[0], 0x400056c2u);
    EXPECT_EQ(out[1], 0x400056c2u);
  }
}

TEST(TifLuvTest, RleEncodeEdgesAndRawBufferFlushAndIoErrors) {
  // Craft patterns covering every RLE branch in LogL16Encode and
  // LogLuvEncode32: >=4 runs, >129 capped runs, short 2-3 runs before a >=4 run
  // and at end of row, short-run candidate with mismatch [A,B,C,B,B,B,B],
  // >127 distinct literals, and tiny raw buffer flushes (size >= 132
  // invariant).

  // Pattern A: short runs, mismatches, capped long runs, >127 literals.
  std::vector<int16_t> logl_pattern;
  // Row starts with 2-run (short run) followed by 4-run.
  logl_pattern.insert(logl_pattern.end(), {0x1111, 0x1111});
  logl_pattern.insert(logl_pattern.end(), 4, 0x2222);
  // Mismatch inside short-run candidate: A, B, C then 4-run of D.
  logl_pattern.insert(logl_pattern.end(), {0x3101, 0x3202, 0x3303});
  logl_pattern.insert(logl_pattern.end(), 4, 0x4444);
  // 140 identical values (> 129 cap).
  logl_pattern.insert(logl_pattern.end(), 140, 0x5555);
  // 260 distinct values (> 127 literal split & triggers occ < j+3 flush on 256B
  // buffer).
  for (int i = 0; i < 260; ++i) {
    logl_pattern.push_back(
        static_cast<int16_t>(((i & 0x7f) << 8) | ((i * 17 + 3) & 0xff)));
  }
  // 140 distinct 4-runs (triggers occ < 4 flush on 256B buffer).
  for (int i = 0; i < 140; ++i) {
    const int16_t val =
        static_cast<int16_t>((((i + 1) & 0xff) << 8) | ((i * 5 + 1) & 0xff));
    logl_pattern.insert(logl_pattern.end(), 4, val);
  }
  // Trailing short 3-run at end of row (beg == npixels, beg - i == 3 < MINRUN).
  logl_pattern.insert(logl_pattern.end(), 3, 0x6666);

  MemoryTiffStream stream16;
  {
    UniqueTiff tif = stream16.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), static_cast<uint32_t>(logl_pattern.size()), 1,
                     /*rows_per_strip=*/1, COMPRESSION_SGILOG, PHOTOMETRIC_LOGL,
                     SGILOGDATAFMT_16BIT);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 256), 1);
    std::vector<int16_t> logl_write = logl_pattern;
    ASSERT_EQ(TIFFWriteScanline(tif.get(), logl_write.data(), 0, 0), 1);
  }
  {
    UniqueTiff tif = stream16.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_16BIT), 1);
    std::vector<int16_t> decoded(logl_pattern.size(), 0);
    ASSERT_EQ(TIFFReadScanline(tif.get(), decoded.data(), 0, 0), 1);
    EXPECT_EQ(decoded, logl_pattern);
  }

  // Same edge pattern matrix for LogLuvEncode32 (RAW uint32_t).
  std::vector<uint32_t> luv32_pattern;
  for (int16_t v : logl_pattern) {
    const uint32_t b0 = static_cast<uint16_t>(v) >> 8;
    const uint32_t b1 = static_cast<uint16_t>(v) & 0xff;
    luv32_pattern.push_back((b0 << 24) | (b1 << 16) | (b0 << 8) | b1);
  }
  MemoryTiffStream stream32;
  {
    UniqueTiff tif = stream32.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), static_cast<uint32_t>(luv32_pattern.size()), 1,
                     /*rows_per_strip=*/1, COMPRESSION_SGILOG,
                     PHOTOMETRIC_LOGLUV, SGILOGDATAFMT_RAW);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 256), 1);
    std::vector<uint32_t> luv32_write = luv32_pattern;
    ASSERT_EQ(TIFFWriteScanline(tif.get(), luv32_write.data(), 0, 0), 1);
  }
  {
    UniqueTiff tif = stream32.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_RAW),
              1);
    std::vector<uint32_t> decoded(luv32_pattern.size(), 0);
    ASSERT_EQ(TIFFReadScanline(tif.get(), decoded.data(), 0, 0), 1);
    EXPECT_EQ(decoded, luv32_pattern);
  }

  // Also trigger raw buffer flush in LogLuvEncode24 (256-byte buffer, >86
  // pixels = >258 bytes).
  MemoryTiffStream stream24;
  std::vector<uint32_t> luv24_pattern(120, (500u << 14) | 1000u);
  {
    UniqueTiff tif = stream24.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), static_cast<uint32_t>(luv24_pattern.size()), 1,
                     /*rows_per_strip=*/1, COMPRESSION_SGILOG24,
                     PHOTOMETRIC_LOGLUV, SGILOGDATAFMT_RAW);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 256), 1);
    std::vector<uint32_t> luv24_write = luv24_pattern;
    ASSERT_EQ(TIFFWriteScanline(tif.get(), luv24_write.data(), 0, 0), 1);
  }
  {
    UniqueTiff tif = stream24.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_RAW),
              1);
    std::vector<uint32_t> decoded(luv24_pattern.size(), 0);
    ASSERT_EQ(TIFFReadScanline(tif.get(), decoded.data(), 0, 0), 1);
    EXPECT_EQ(decoded, luv24_pattern);
  }

  // Inject write errors during TIFFFlushData1 inside LogL16Encode (both
  // literal flush and run flush), LogLuvEncode32 (both), and LogLuvEncode24,
  // plus LogLuvEncodeStrip and LogLuvEncodeTile error propagation.
  auto inject_write_fail = [](uint16_t compression, uint16_t photometric,
                              int data_fmt, bool force_runs) {
    MemoryTiffStream fail_stream;
    UniqueTiff tif = fail_stream.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    constexpr uint32_t kLen = 600;
    ConfigureStriped(tif.get(), kLen, 1, /*rows_per_strip=*/1, compression,
                     photometric, data_fmt);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 256), 1);
    fail_stream.FailWritesAfter(fail_stream.write_calls());
    if (data_fmt == SGILOGDATAFMT_16BIT) {
      std::vector<int16_t> buf(kLen);
      for (uint32_t i = 0; i < kLen; ++i) {
        buf[i] = force_runs ? static_cast<int16_t>(((i / 4) & 0xff) << 8)
                            : static_cast<int16_t>(((i & 0xff) << 8) | i);
      }
      EXPECT_EQ(TIFFWriteScanline(tif.get(), buf.data(), 0, 0), 0);
    } else {
      std::vector<uint32_t> buf(kLen);
      for (uint32_t i = 0; i < kLen; ++i) {
        buf[i] = force_runs ? (((i / 4) & 0xffu) << 24)
                            : (((i & 0xffu) << 24) | ((i * 3u) & 0xffu));
      }
      EXPECT_EQ(TIFFWriteScanline(tif.get(), buf.data(), 0, 0), 0);
    }
    fail_stream.ResetFaults();
  };
  inject_write_fail(COMPRESSION_SGILOG, PHOTOMETRIC_LOGL, SGILOGDATAFMT_16BIT,
                    /*force_runs=*/false);
  inject_write_fail(COMPRESSION_SGILOG, PHOTOMETRIC_LOGL, SGILOGDATAFMT_16BIT,
                    /*force_runs=*/true);
  inject_write_fail(COMPRESSION_SGILOG, PHOTOMETRIC_LOGLUV, SGILOGDATAFMT_RAW,
                    /*force_runs=*/false);
  inject_write_fail(COMPRESSION_SGILOG, PHOTOMETRIC_LOGLUV, SGILOGDATAFMT_RAW,
                    /*force_runs=*/true);
  inject_write_fail(COMPRESSION_SGILOG24, PHOTOMETRIC_LOGLUV, SGILOGDATAFMT_RAW,
                    /*force_runs=*/false);

  // EncodeStrip and EncodeTile returning 0 on underlying flush failure.
  {
    MemoryTiffStream s;
    UniqueTiff tif = s.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), 120, 1, 1, COMPRESSION_SGILOG24,
                     PHOTOMETRIC_LOGLUV, SGILOGDATAFMT_RAW);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 256), 1);
    s.FailWritesAfter(s.write_calls());
    std::vector<uint32_t> strip(120, 0x100u);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, strip.data(), 120 * 4),
              static_cast<tmsize_t>(-1));
    s.ResetFaults();
  }
  {
    MemoryTiffStream s;
    UniqueTiff tif = s.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureTiled(tif.get(), 16, 16, 16, 16, COMPRESSION_SGILOG24,
                   PHOTOMETRIC_LOGLUV, SGILOGDATAFMT_RAW);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 256), 1);
    s.FailWritesAfter(s.write_calls());
    std::vector<uint32_t> tile(256, 0x100u);
    EXPECT_EQ(TIFFWriteEncodedTile(tif.get(), 0, tile.data(), 256 * 4),
              static_cast<tmsize_t>(-1));
    s.ResetFaults();
  }
}

TEST(TifLuvTest, CorruptAndTruncatedDecodeErrorsAndTranslationBufferCheck) {
  // LogL16Decode corrupt/truncated bitstreams:
  // 1) single noop byte {0x00} (0 pixels decoded), 2) 1-byte run header {0x85},
  // 3) truncated literal, 4) short on low-byte plane (shft=0),
  // 5) noop + short plane 2, 6) overlong run/literal clamped cleanly.
  const std::vector<std::vector<uint8_t>> bad_logl_strips = {
      {0x00},                             // short on plane 1 (i==0)
      {0x85},                             // run with cc < 2
      {0x04, 0x11, 0x22},                 // truncated literal
      {0x04, 1, 2, 3, 4},                 // short on second byte plane
      {0x00, 0x04, 1, 2, 3, 4, 0x02, 5},  // noop literal + short plane 2
      {0x8a, 0x12, 0x0a, 1, 2, 3},        // overlong run clamped, short p2
  };
  for (const auto& strip : bad_logl_strips) {
    std::vector<uint8_t> bytes = BuildStripedSgilogTiff(
        /*width=*/4, /*height=*/1, /*rows_per_strip=*/1, COMPRESSION_SGILOG,
        PHOTOMETRIC_LOGL, /*spp=*/1, /*bps=*/16, SAMPLEFORMAT_INT,
        PLANARCONFIG_CONTIG, {strip});
    MemoryTiffStream s(std::move(bytes));
    UniqueTiff tif = s.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    std::array<int16_t, 4> row{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(s.diagnostics().errors(),
                                        "LogL16Decode", "Not enough data"));
  }

  // Mid-strip (2 rows in strip) and mid-tile (2 rows in tile) failure in
  // LogLuvDecodeStrip and LogLuvDecodeTile.
  {
    // Row 0 valid 4-pixel LogL: plane 1 (0x82, 0x10 = 4 bytes) + plane 2
    // (0x82, 0x20 = 4 bytes), then Row 1 truncated (0x82 only).
    std::vector<uint8_t> two_row_strip = {0x82, 0x10, 0x82, 0x20, 0x82};
    std::vector<uint8_t> bytes = BuildStripedSgilogTiff(
        /*width=*/4, /*height=*/2, /*rows_per_strip=*/2, COMPRESSION_SGILOG,
        PHOTOMETRIC_LOGL, 1, 16, SAMPLEFORMAT_INT, PLANARCONFIG_CONTIG,
        {two_row_strip});
    MemoryTiffStream s(std::move(bytes));
    UniqueTiff tif = s.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    std::vector<int16_t> strip_buf(8, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, strip_buf.data(), 8 * 2),
              static_cast<tmsize_t>(-1));
  }
  {
    std::vector<uint8_t> bad_tile = {0x8e, 0x10, 0x8e, 0x20, 0x01};
    std::vector<uint8_t> bytes = BuildTiledSgilogTiff(
        16, 16, 16, 16, COMPRESSION_SGILOG, PHOTOMETRIC_LOGL, 1, 16,
        SAMPLEFORMAT_INT, {bad_tile});
    MemoryTiffStream s(std::move(bytes));
    UniqueTiff tif = s.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    std::vector<int16_t> tile_buf(256, 0);
    EXPECT_EQ(TIFFReadEncodedTile(tif.get(), 0, tile_buf.data(), 256 * 2),
              static_cast<tmsize_t>(-1));
  }

  // LogLuvDecode32 corrupt strips & overlong clamping.
  const std::vector<std::vector<uint8_t>> bad_luv32_strips = {
      {0x00},
      {0x85},
      {0x04, 0x11, 0x22},
      {0x00, 0x82, 0x40, 0x82, 0x50},  // short on plane 3
  };
  for (const auto& strip : bad_luv32_strips) {
    std::vector<uint8_t> bytes = BuildStripedSgilogTiff(
        /*width=*/4, /*height=*/1, /*rows_per_strip=*/1, COMPRESSION_SGILOG,
        PHOTOMETRIC_LOGLUV, /*spp=*/3, /*bps=*/16, SAMPLEFORMAT_INT,
        PLANARCONFIG_CONTIG, {strip});
    MemoryTiffStream s(std::move(bytes));
    UniqueTiff tif = s.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_RAW),
              1);
    std::array<uint32_t, 4> row{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(s.diagnostics().errors(),
                                        "LogLuvDecode32", "Not enough data"));
  }
  // LogLuvDecode32 with valid 4-byte planes containing overlong run clamped to
  // npixels.
  {
    std::vector<uint8_t> clamped_strip = {0x90, 0x40, 0x90, 0x00,
                                          0x90, 0x56, 0x90, 0xc2};
    std::vector<uint8_t> bytes = BuildStripedSgilogTiff(
        4, 1, 1, COMPRESSION_SGILOG, PHOTOMETRIC_LOGLUV, 3, 16,
        SAMPLEFORMAT_INT, PLANARCONFIG_CONTIG, {clamped_strip});
    MemoryTiffStream s(std::move(bytes));
    UniqueTiff tif = s.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_RAW),
              1);
    std::array<uint32_t, 4> row{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), 1);
    EXPECT_EQ(row[0], 0x400056c2u);
  }

  // LogLuvDecode24 short strips (2 bytes and 1 byte for width=1) and
  // mid-strip failure.
  for (const std::vector<uint8_t>& strip :
       {std::vector<uint8_t>{0x12, 0x34}, std::vector<uint8_t>{0x12}}) {
    std::vector<uint8_t> bytes = BuildStripedSgilogTiff(
        1, 1, 1, COMPRESSION_SGILOG24, PHOTOMETRIC_LOGLUV, 3, 16,
        SAMPLEFORMAT_INT, PLANARCONFIG_CONTIG, {strip});
    MemoryTiffStream s(std::move(bytes));
    UniqueTiff tif = s.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_RAW),
              1);
    uint32_t px = 0;
    EXPECT_EQ(TIFFReadScanline(tif.get(), &px, 0, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(s.diagnostics().errors(),
                                        "LogLuvDecode24", "Not enough data"));
  }

  // "Translation buffer too short" on LogL16Decode, LogLuvDecode32, and
  // LogLuvDecode24: decode row 0 in small pixel format (8BIT, tbuflen = w),
  // then switch TIFFTAG_SGILOGDATAFMT to FLOAT on row 1 (td_scanlinesize grows
  // 4x while sp->tbuflen and sp->pixel_size keep row 0 values -> npixels >
  // tbuflen).
  auto test_short_tbuf = [](uint16_t compression, uint16_t photometric,
                            std::string_view expected_module) {
    MemoryTiffStream stream;
    constexpr uint32_t w = 4;
    constexpr uint32_t h = 2;
    {
      UniqueTiff tif = stream.Open("w");
      ASSERT_NE(tif.get(), nullptr);
      ConfigureStriped(tif.get(), w, h, 1, compression, photometric,
                       SGILOGDATAFMT_FLOAT);
      std::vector<float> row(w * (photometric == PHOTOMETRIC_LOGL ? 1 : 3),
                             0.5f);
      ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 1);
      ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 1, 0), 1);
    }
    {
      UniqueTiff tif = stream.Open("r");
      ASSERT_NE(tif.get(), nullptr);
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_8BIT),
          1);
      std::vector<uint8_t> row0(w * 3, 0);
      ASSERT_EQ(TIFFReadScanline(tif.get(), row0.data(), 0, 0), 1);

      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_FLOAT),
          1);
      std::vector<float> row1(w * 3, 0.0f);
      EXPECT_EQ(TIFFReadScanline(tif.get(), row1.data(), 1, 0), -1);
      EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                          expected_module,
                                          "Translation buffer too short"));
    }
  };
  test_short_tbuf(COMPRESSION_SGILOG, PHOTOMETRIC_LOGL, "LogL16Decode");
  test_short_tbuf(COMPRESSION_SGILOG, PHOTOMETRIC_LOGLUV, "LogLuvDecode32");
  test_short_tbuf(COMPRESSION_SGILOG24, PHOTOMETRIC_LOGLUV, "LogLuvDecode24");
}

TEST(TifLuvTest, GuessDataFmtTagsSetupErrorsAndOom) {
  // Invalid SGILOGDATAFMT and SGILOGENCODE tag values + encoder_state == 0 on
  // close.
  {
    MemoryTiffStream s;
    UniqueTiff tif = s.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), 4, 1, 1, COMPRESSION_SGILOG, PHOTOMETRIC_LOGLUV,
                     SGILOGDATAFMT_FLOAT);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SGILOGDATAFMT, 99), 0);
    EXPECT_TRUE(HasAnyMessageContaining(s.diagnostics().errors(),
                                        "Unknown data format 99"));
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_SGILOGENCODE, 99), 0);
    EXPECT_TRUE(HasDiagnosticContaining(
        s.diagnostics().errors(), "LogLuvVSetField", "Unknown encoding 99"));
    // Closing without writing leaves sp->encoder_state == 0 and sp->tbuf ==
    // NULL.
  }

  // Test LogL16GuessDataFmt when SGILOGDATAFMT is NOT explicitly set.
  {
    // 1 pixel LogL strip: 0x01,0x40 (high byte), 0x01,0x00 (low byte) ->
    // 0x4000.
    const std::vector<uint8_t> logl_1px = {0x01, 0x40, 0x01, 0x00};
    struct LogLGuessCase {
      uint16_t spp;
      uint16_t bps;
      std::optional<uint16_t> sfmt;
      bool expect_ok;
    };
    const std::vector<LogLGuessCase> cases = {
        {1, 32, SAMPLEFORMAT_IEEEFP, true}, {1, 16, std::nullopt, true},
        {1, 16, SAMPLEFORMAT_INT, true},    {1, 16, SAMPLEFORMAT_UINT, true},
        {1, 8, std::nullopt, true},         {1, 8, SAMPLEFORMAT_UINT, true},
        {1, 8, SAMPLEFORMAT_INT, false},    {1, 32, SAMPLEFORMAT_UINT, false},
        {2, 16, SAMPLEFORMAT_INT, false},
    };
    for (const auto& tc : cases) {
      std::vector<uint8_t> bytes = BuildStripedSgilogTiff(
          1, 1, 1, COMPRESSION_SGILOG, PHOTOMETRIC_LOGL, tc.spp, tc.bps,
          tc.sfmt, PLANARCONFIG_CONTIG, {logl_1px});
      MemoryTiffStream s(std::move(bytes));
      UniqueTiff tif = s.Open("r");
      ASSERT_NE(tif.get(), nullptr);
      std::array<uint8_t, 8> buf{};
      const int rc = TIFFReadScanline(tif.get(), buf.data(), 0, 0);
      EXPECT_EQ(rc, tc.expect_ok ? 1 : -1);
    }
  }

  // Test LogLuvGuessDataFmt when SGILOGDATAFMT is NOT explicitly set (24-bit
  // 1px = 3 bytes).
  {
    const std::vector<uint8_t> luv24_1px = {0x10, 0x02, 0x00};
    struct LuvGuessCase {
      uint16_t spp;
      uint16_t bps;
      std::optional<uint16_t> sfmt;
      uint16_t planar;
      bool expect_ok;
    };
    const std::vector<LuvGuessCase> cases = {
        {3, 32, SAMPLEFORMAT_IEEEFP, PLANARCONFIG_CONTIG, true},
        {3, 32, SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, false},
        {1, 32, std::nullopt, PLANARCONFIG_CONTIG, true},
        {1, 32, SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, true},
        {1, 32, SAMPLEFORMAT_INT, PLANARCONFIG_CONTIG, true},
        {1, 16, SAMPLEFORMAT_INT, PLANARCONFIG_CONTIG, false},
        {3, 16, std::nullopt, PLANARCONFIG_CONTIG, true},
        {3, 16, SAMPLEFORMAT_INT, PLANARCONFIG_CONTIG, true},
        {3, 16, SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, true},
        {3, 8, std::nullopt, PLANARCONFIG_CONTIG, true},
        {3, 8, SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, true},
        {3, 8, SAMPLEFORMAT_INT, PLANARCONFIG_CONTIG, false},
        {4, 8, SAMPLEFORMAT_UINT, PLANARCONFIG_CONTIG, false},
        {3, 16, SAMPLEFORMAT_INT, PLANARCONFIG_SEPARATE, false},
    };
    for (const auto& tc : cases) {
      std::vector<uint8_t> bytes = BuildStripedSgilogTiff(
          1, 1, 1, COMPRESSION_SGILOG24, PHOTOMETRIC_LOGLUV, tc.spp, tc.bps,
          tc.sfmt, tc.planar, {luv24_1px});
      MemoryTiffStream s(std::move(bytes));
      UniqueTiff tif = s.Open("r");
      ASSERT_NE(tif.get(), nullptr);
      std::array<uint8_t, 16> buf{};
      const int rc = TIFFReadScanline(tif.get(), buf.data(), 0, 0);
      EXPECT_EQ(rc, tc.expect_ok ? 1 : -1);
    }
  }

  // Unsupported photometric / format rejections on encode and decode.
  // Wrong photometric on decode.
  {
    std::vector<uint8_t> bytes = BuildStripedSgilogTiff(
        1, 1, 1, COMPRESSION_SGILOG, PHOTOMETRIC_RGB, 3, 8, SAMPLEFORMAT_UINT,
        PLANARCONFIG_CONTIG, {{0x01, 0x00}});
    MemoryTiffStream s(std::move(bytes));
    UniqueTiff tif = s.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    std::array<uint8_t, 4> buf{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), buf.data(), 0, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(s.diagnostics().errors(),
                                        "LogLuvSetupDecode",
                                        "Inappropriate photometric"));
  }
  // Wrong photometric on encode.
  {
    MemoryTiffStream s;
    UniqueTiff tif = s.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), 2, 1, 1, COMPRESSION_SGILOG, PHOTOMETRIC_RGB,
                     SGILOGDATAFMT_16BIT, 3);
    std::array<int16_t, 6> row{};
    EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(s.diagnostics().errors(),
                                        "LogLuvSetupEncode",
                                        "Inappropriate photometric"));
  }
  // 8BIT encode unsupported for LOGL, LOGLUV+SGILOG, and LOGLUV+SGILOG24; plus
  // RAW unsupported for LOGL.
  for (const auto& [comp, photo, fmt, err_substr] : {
           std::tuple{COMPRESSION_SGILOG, PHOTOMETRIC_LOGL, SGILOGDATAFMT_8BIT,
                      "Y, L"},
           std::tuple{COMPRESSION_SGILOG, PHOTOMETRIC_LOGLUV,
                      SGILOGDATAFMT_8BIT, "XYZ, Luv"},
           std::tuple{COMPRESSION_SGILOG24, PHOTOMETRIC_LOGLUV,
                      SGILOGDATAFMT_8BIT, "XYZ, Luv"},
           std::tuple{COMPRESSION_SGILOG, PHOTOMETRIC_LOGL, SGILOGDATAFMT_RAW,
                      "No support for converting user data format to LogL"},
       }) {
    MemoryTiffStream s;
    UniqueTiff tif = s.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    ConfigureStriped(tif.get(), 2, 1, 1, comp, photo, fmt);
    std::array<uint32_t, 6> row{};
    EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasAnyMessageContaining(s.diagnostics().errors(), err_substr));
  }

  // OOM in LogL16InitState & LogLuvInitState for sp->tbuf via
  // ScopedSingleMemAllocLimit (pre-allocate small raw buffer before tightening
  // limit so only sp->tbuf triggers the limit).
  for (const uint16_t photometric : {PHOTOMETRIC_LOGL, PHOTOMETRIC_LOGLUV}) {
    MemoryTiffStream s;
    UniqueTiff tif = s.Open("w");
    ASSERT_NE(tif.get(), nullptr);
    constexpr uint32_t kWidth = 2048;
    ConfigureStriped(tif.get(), kWidth, 1, 1, COMPRESSION_SGILOG, photometric,
                     SGILOGDATAFMT_FLOAT);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 256), 1);
    std::vector<float> row(kWidth * (photometric == PHOTOMETRIC_LOGL ? 1 : 3),
                           0.5f);
    ScopedSingleMemAllocLimit limit(tif.get(), 512);
    EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasAnyMessageContaining(
        s.diagnostics().errors(), "No space for SGILog translation buffer"));
  }

  // OOM in TIFFInitSGILog for LogLuvState: open with >0 cumulated budget, allow
  // _TIFFMergeFields (+16 bytes) to succeed, and fail LogLuvState allocation.
  {
    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), kDefaultAllocBudget);

    MemoryTiffStream s;
    UniqueTiff tif = s.Open("w", opts.get());
    ASSERT_NE(tif.get(), nullptr);
    {
      ScopedCumulatedMemAllocRemaining remaining(tif.get(), 24);
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_SGILOG), 0);
    }
    EXPECT_TRUE(HasDiagnosticContaining(recorder.errors(), "TIFFInitSGILog",
                                        "No space for LogLuv state block"));
  }
}

}  // namespace
}  // namespace libtiff
