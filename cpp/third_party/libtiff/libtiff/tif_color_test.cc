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

// Unit tests for libtiff color space conversions (libtiff/tif_color.c):
// CIE L*a*b* (8-bit and 16-bit) -> XYZ -> RGB and YCbCr -> RGB.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

constexpr TIFFDisplay MakeSrgbDisplay() {
  return TIFFDisplay{
      .d_mat =
          {
              {3.2410f, -1.5374f, -0.4986f},
              {-0.9692f, 1.8760f, 0.0416f},
              {0.0556f, -0.2040f, 1.0570f},
          },
      .d_YCR = 100.0f,
      .d_YCG = 100.0f,
      .d_YCB = 100.0f,
      .d_Vrwr = 255,
      .d_Vrwg = 255,
      .d_Vrwb = 255,
      .d_Y0R = 1.0f,
      .d_Y0G = 1.0f,
      .d_Y0B = 1.0f,
      .d_gammaR = 2.4f,
      .d_gammaG = 2.4f,
      .d_gammaB = 2.4f,
  };
}

constexpr std::array<float, 3> MakeD65WhitePoint() {
  return {D65_X0, D65_Y0, D65_Z0};
}

UniqueTiffBuffer<TIFFYCbCrToRGB> AllocateYcbcrState() {
  const tmsize_t size = static_cast<tmsize_t>(TIFFroundup_32(
                            sizeof(TIFFYCbCrToRGB), sizeof(intptr_t))) +
                        static_cast<tmsize_t>(4 * 256 * sizeof(TIFFRGBValue)) +
                        static_cast<tmsize_t>(2 * 256 * sizeof(int)) +
                        static_cast<tmsize_t>(3 * 256 * sizeof(int32_t));
  return UniqueTiffBuffer<TIFFYCbCrToRGB>(
      static_cast<TIFFYCbCrToRGB*>(_TIFFmalloc(size)));
}

TEST(TifColorTest, CIELabToRGBInitPopulatesTablesAndMetadata) {
  TIFFCIELabToRGB cielab{};
  const TIFFDisplay display = MakeSrgbDisplay();
  std::array<float, 3> ref_white = MakeD65WhitePoint();

  ASSERT_EQ(TIFFCIELabToRGBInit(&cielab, &display, ref_white.data()), 0);
  EXPECT_EQ(cielab.range, CIELABTORGB_TABLE_RANGE);
  EXPECT_FLOAT_EQ(cielab.X0, D65_X0);
  EXPECT_FLOAT_EQ(cielab.Y0, D65_Y0);
  EXPECT_FLOAT_EQ(cielab.Z0, D65_Z0);

  const float expected_step = (display.d_YCR - display.d_Y0R) /
                              static_cast<float>(CIELABTORGB_TABLE_RANGE);
  EXPECT_FLOAT_EQ(cielab.rstep, expected_step);
  EXPECT_FLOAT_EQ(cielab.gstep, expected_step);
  EXPECT_FLOAT_EQ(cielab.bstep, expected_step);

  EXPECT_FLOAT_EQ(cielab.Yr2r[0], 0.0f);
  EXPECT_FLOAT_EQ(cielab.Yg2g[0], 0.0f);
  EXPECT_FLOAT_EQ(cielab.Yb2b[0], 0.0f);
  EXPECT_FLOAT_EQ(cielab.Yr2r[CIELABTORGB_TABLE_RANGE], 255.0f);
  EXPECT_FLOAT_EQ(cielab.Yg2g[CIELABTORGB_TABLE_RANGE], 255.0f);
  EXPECT_FLOAT_EQ(cielab.Yb2b[CIELABTORGB_TABLE_RANGE], 255.0f);

  for (int i = 1; i <= CIELABTORGB_TABLE_RANGE; ++i) {
    EXPECT_GE(cielab.Yr2r[i], cielab.Yr2r[i - 1]);
    EXPECT_GE(cielab.Yg2g[i], cielab.Yg2g[i - 1]);
    EXPECT_GE(cielab.Yb2b[i], cielab.Yb2b[i - 1]);
  }

  const int mid = CIELABTORGB_TABLE_RANGE / 2;
  const float expected_mid = 255.0f * std::pow(0.5f, 1.0f / 2.4f);
  EXPECT_NEAR(cielab.Yr2r[mid], expected_mid, 1e-3f);
  EXPECT_NEAR(cielab.Yg2g[mid], expected_mid, 1e-3f);
  EXPECT_NEAR(cielab.Yb2b[mid], expected_mid, 1e-3f);
}

TEST(TifColorTest, CIELabToRGBInitSupportsCustomDisplayAndD50WhitePoint) {
  TIFFCIELabToRGB cielab{};
  const TIFFDisplay display = {
      .d_mat =
          {
              {1.0f, 0.0f, 0.0f},
              {0.0f, 1.0f, 0.0f},
              {0.0f, 0.0f, 1.0f},
          },
      .d_YCR = 100.0f,
      .d_YCG = 100.0f,
      .d_YCB = 100.0f,
      .d_Vrwr = 65535,
      .d_Vrwg = 1023,
      .d_Vrwb = 255,
      .d_Y0R = 0.0f,
      .d_Y0G = 0.0f,
      .d_Y0B = 0.0f,
      .d_gammaR = 1.0f,
      .d_gammaG = 2.0f,
      .d_gammaB = 2.2f,
  };
  std::array<float, 3> ref_white = {D50_X0, D50_Y0, D50_Z0};

  ASSERT_EQ(TIFFCIELabToRGBInit(&cielab, &display, ref_white.data()), 0);
  EXPECT_FLOAT_EQ(cielab.X0, D50_X0);
  EXPECT_FLOAT_EQ(cielab.Y0, D50_Y0);
  EXPECT_FLOAT_EQ(cielab.Z0, D50_Z0);

  // At 25% of table range: R is linear (0.25), G is sqrt(0.25)=0.5.
  const int quarter = CIELABTORGB_TABLE_RANGE / 4;
  EXPECT_NEAR(cielab.Yr2r[quarter], 65535.0f * 0.25f, 1e-1f);
  EXPECT_NEAR(cielab.Yg2g[quarter], 1023.0f * 0.5f, 1e-2f);
  EXPECT_NEAR(cielab.Yb2b[quarter], 255.0f * std::pow(0.25f, 1.0f / 2.2f),
              1e-2f);
}

TEST(TifColorTest, CIELab16ToXYZCoversLowHighLuminanceAndChromaBranches) {
  TIFFCIELabToRGB cielab{};
  const TIFFDisplay display = MakeSrgbDisplay();
  std::array<float, 3> ref_white = MakeD65WhitePoint();
  ASSERT_EQ(TIFFCIELabToRGBInit(&cielab, &display, ref_white.data()), 0);

  float X = -1.0f;
  float Y = -1.0f;
  float Z = -1.0f;

  // Black: L=0 (< 8.856), a=0, b=0 -> tmp < 0.2069 for both X and Z.
  TIFFCIELab16ToXYZ(&cielab, 0, 0, 0, &X, &Y, &Z);
  EXPECT_FLOAT_EQ(Y, 0.0f);
  EXPECT_NEAR(X, 0.0f, 1e-3f);
  EXPECT_NEAR(Z, 0.0f, 1e-3f);

  // Boundary just below L = 8.856f (5803 * 100 / 65535 ~= 8.85481f).
  float X_low = 0.0f;
  float Y_low = 0.0f;
  float Z_low = 0.0f;
  TIFFCIELab16ToXYZ(&cielab, 5803, 0, 0, &X_low, &Y_low, &Z_low);
  EXPECT_NEAR(Y_low, (5803.0f * 100.0f / 65535.0f) * D65_Y0 / 903.292f, 1e-4f);

  // Boundary just above L = 8.856f (5804 * 100 / 65535 ~= 8.85634f).
  float X_high = 0.0f;
  float Y_high = 0.0f;
  float Z_high = 0.0f;
  TIFFCIELab16ToXYZ(&cielab, 5804, 0, 0, &X_high, &Y_high, &Z_high);
  EXPECT_GT(Y_high, Y_low);
  EXPECT_NEAR(Y_high, Y_low, 0.02f);

  // Reference white: L=65535 (>= 8.856), a=0, b=0 -> tmp >= 0.2069.
  TIFFCIELab16ToXYZ(&cielab, 65535, 0, 0, &X, &Y, &Z);
  EXPECT_NEAR(X, D65_X0, 1e-4f);
  EXPECT_NEAR(Y, D65_Y0, 1e-4f);
  EXPECT_NEAR(Z, D65_Z0, 1e-4f);

  // Low L (< 8.856) paired with strong +a and -b so tmp >= 0.2069 for X and Z.
  TIFFCIELab16ToXYZ(&cielab, 0, 120 * 256, -120 * 256, &X, &Y, &Z);
  EXPECT_FLOAT_EQ(Y, 0.0f);
  EXPECT_GT(X, 3.0f);
  EXPECT_GT(Z, 30.0f);

  // High L (>= 8.856) paired with extreme -a and +b so tmp < 0.2069 for X and
  // Z, producing out-of-gamut negative X and Z before display clamping.
  TIFFCIELab16ToXYZ(&cielab, 6554, -32768, 32767, &X, &Y, &Z);
  EXPECT_GT(Y, 1.0f);
  EXPECT_LT(X, 0.0f);
  EXPECT_LT(Z, 0.0f);

  // At L~=50 with extreme -a and +b, X stays on the cubic branch while Z
  // crosses below tmp < 0.2069 into negative values.
  TIFFCIELab16ToXYZ(&cielab, 32768, -32768, 32767, &X, &Y, &Z);
  EXPECT_GT(Y, 15.0f);
  EXPECT_GT(X, 0.0f);
  EXPECT_LT(Z, 0.0f);

  // Positive a / negative b at mid-L must yield finite values.
  TIFFCIELab16ToXYZ(&cielab, 32768, 32767, -32768, &X, &Y, &Z);
  EXPECT_TRUE(std::isfinite(X));
  EXPECT_TRUE(std::isfinite(Y));
  EXPECT_TRUE(std::isfinite(Z));
  EXPECT_GT(X, Y * 0.5f);
  EXPECT_GT(Z, Y);
}

TEST(TifColorTest, CIELabToXYZMatches16BitScaling) {
  TIFFCIELabToRGB cielab{};
  const TIFFDisplay display = MakeSrgbDisplay();
  std::array<float, 3> ref_white = MakeD65WhitePoint();
  ASSERT_EQ(TIFFCIELabToRGBInit(&cielab, &display, ref_white.data()), 0);

  struct LabSample {
    uint32_t l;
    int32_t a;
    int32_t b;
  };
  constexpr std::array<LabSample, 7> kSamples = {{
      {.l = 0, .a = 0, .b = 0},
      {.l = 255, .a = 0, .b = 0},
      {.l = 128, .a = 0, .b = 0},
      {.l = 140, .a = 80, .b = 65},
      {.l = 120, .a = -70, .b = -50},
      {.l = 10, .a = -128, .b = 127},
      {.l = 245, .a = 127, .b = -128},
  }};

  for (const auto& s : kSamples) {
    SCOPED_TRACE(testing::Message()
                 << "l=" << s.l << " a=" << s.a << " b=" << s.b);
    float x8 = 0.0f, y8 = 0.0f, z8 = 0.0f;
    float x16 = 0.0f, y16 = 0.0f, z16 = 0.0f;
    TIFFCIELabToXYZ(&cielab, s.l, s.a, s.b, &x8, &y8, &z8);
    TIFFCIELab16ToXYZ(&cielab, s.l * 257U, s.a * 256, s.b * 256, &x16, &y16,
                      &z16);
    EXPECT_FLOAT_EQ(x8, x16);
    EXPECT_FLOAT_EQ(y8, y16);
    EXPECT_FLOAT_EQ(z8, z16);
  }

  // 8-bit L=255, a=0, b=0 must recover the reference white point exactly.
  float X = 0.0f, Y = 0.0f, Z = 0.0f;
  TIFFCIELabToXYZ(&cielab, 255, 0, 0, &X, &Y, &Z);
  EXPECT_NEAR(X, D65_X0, 1e-4f);
  EXPECT_NEAR(Y, D65_Y0, 1e-4f);
  EXPECT_NEAR(Z, D65_Z0, 1e-4f);
}

TEST(TifColorTest, XYZToRGBConvertsWhiteBlackAndClampsOutOfRange) {
  TIFFCIELabToRGB cielab{};
  const TIFFDisplay srgb = MakeSrgbDisplay();
  std::array<float, 3> ref_white = MakeD65WhitePoint();
  ASSERT_EQ(TIFFCIELabToRGBInit(&cielab, &srgb, ref_white.data()), 0);

  uint32_t r = 1, g = 1, b = 1;
  TIFFXYZToRGB(&cielab, D65_X0, D65_Y0, D65_Z0, &r, &g, &b);
  EXPECT_EQ(r, 255U);
  EXPECT_EQ(g, 255U);
  EXPECT_EQ(b, 255U);

  // Zero and negative XYZ clamp to d_Y0* -> 0.
  TIFFXYZToRGB(&cielab, 0.0f, 0.0f, 0.0f, &r, &g, &b);
  EXPECT_EQ(r, 0U);
  EXPECT_EQ(g, 0U);
  EXPECT_EQ(b, 0U);

  // Negative scaled white point (-D65) yields Yr,Yg,Yb ~= -100 -> clamped to 0.
  TIFFXYZToRGB(&cielab, -D65_X0, -D65_Y0, -D65_Z0, &r, &g, &b);
  EXPECT_EQ(r, 0U);
  EXPECT_EQ(g, 0U);
  EXPECT_EQ(b, 0U);

  // Out-of-gamut XYZ where Yr is positive while Yg and Yb clamp to d_Y0*.
  TIFFXYZToRGB(&cielab, -50.0f, -100.0f, -200.0f, &r, &g, &b);
  EXPECT_GT(r, 0U);
  EXPECT_LE(r, 255U);
  EXPECT_EQ(g, 0U);
  EXPECT_EQ(b, 0U);

  // Excessive XYZ clamps to d_YC* -> d_Vrw*.
  TIFFXYZToRGB(&cielab, 5000.0f, 5000.0f, 5000.0f, &r, &g, &b);
  EXPECT_EQ(r, 255U);
  EXPECT_EQ(g, 255U);
  EXPECT_EQ(b, 255U);

  // Asymmetric linear identity display to verify per-channel min/max clamping,
  // index range clamping (when d_YCG > d_YCR), and output d_Vrw* clamping.
  TIFFDisplay custom = {
      .d_mat =
          {
              {1.0f, 0.0f, 0.0f},
              {0.0f, 1.0f, 0.0f},
              {0.0f, 0.0f, 1.0f},
          },
      .d_YCR = 100.0f,
      .d_YCG = 200.0f,
      .d_YCB = 100.0f,
      // (float)16777219 rounds up to 16777220.0f -> tests TIFFmin(*r, d_Vrwr).
      .d_Vrwr = 16777219U,
      .d_Vrwg = 150,
      .d_Vrwb = 100,
      .d_Y0R = 10.0f,
      .d_Y0G = 10.0f,
      .d_Y0B = 10.0f,
      .d_gammaR = 1.0f,
      .d_gammaG = 1.0f,
      .d_gammaB = 1.0f,
  };
  ASSERT_EQ(TIFFCIELabToRGBInit(&cielab, &custom, ref_white.data()), 0);

  // X below black (0), Y beyond d_YCR so i > range clamped to 1500 (150),
  // Z at midpoint 55 -> 50.
  TIFFXYZToRGB(&cielab, -5.0f, 180.0f, 55.0f, &r, &g, &b);
  EXPECT_EQ(r, 0U);
  EXPECT_EQ(g, 150U);
  EXPECT_EQ(b, 50U);

  // X at max triggers d_Vrwr output clamp; Y below black -> 0; Z above max ->
  // 100.
  TIFFXYZToRGB(&cielab, 1000.0f, -5.0f, 1000.0f, &r, &g, &b);
  EXPECT_EQ(r, 16777219U);
  EXPECT_EQ(g, 0U);
  EXPECT_EQ(b, 100U);
}

TEST(TifColorTest, YCbCrToRGBInitDefaultCoefficientsAndTableInvariants) {
  UniqueTiffBuffer<TIFFYCbCrToRGB> ycbcr = AllocateYcbcrState();
  ASSERT_NE(ycbcr.get(), nullptr);

  std::array<float, 3> luma = {0.299f, 0.587f, 0.114f};
  std::array<float, 6> ref_black_white = {0.0f,   255.0f, 128.0f,
                                          255.0f, 128.0f, 255.0f};
  ASSERT_EQ(
      TIFFYCbCrToRGBInit(ycbcr.get(), luma.data(), ref_black_white.data()), 0);

  // Verify clamptab layout: [-256..-1] == 0, [0..255] == i, [256..767] == 255.
  for (int i = -256; i < 0; ++i) {
    EXPECT_EQ(ycbcr->clamptab[i], 0) << "i=" << i;
  }
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(ycbcr->clamptab[i], static_cast<TIFFRGBValue>(i)) << "i=" << i;
  }
  for (int i = 256; i < 768; ++i) {
    EXPECT_EQ(ycbcr->clamptab[i], 255) << "i=" << i;
  }

  // Neutral gray axis (Cb=128, Cr=128) must map Y -> (Y, Y, Y).
  for (uint32_t y = 0; y <= 255; ++y) {
    uint32_t r = 0, g = 0, b = 0;
    TIFFYCbCrtoRGB(ycbcr.get(), y, 128, 128, &r, &g, &b);
    EXPECT_EQ(r, y);
    EXPECT_EQ(g, y);
    EXPECT_EQ(b, y);
  }

  // Known primary colors under CCIR 601-1.
  uint32_t r = 0, g = 0, b = 0;
  TIFFYCbCrtoRGB(ycbcr.get(), 76, 85, 255, &r, &g, &b);  // Red
  EXPECT_NEAR(r, 254.0, 2.0);
  EXPECT_LE(g, 2U);
  EXPECT_LE(b, 2U);

  TIFFYCbCrtoRGB(ycbcr.get(), 150, 44, 21, &r, &g, &b);  // Green
  EXPECT_LE(r, 2U);
  EXPECT_NEAR(g, 255.0, 2.0);
  EXPECT_LE(b, 2U);

  TIFFYCbCrtoRGB(ycbcr.get(), 29, 255, 107, &r, &g, &b);  // Blue
  EXPECT_LE(r, 2U);
  EXPECT_LE(g, 2U);
  EXPECT_NEAR(b, 254.0, 2.0);
}

TEST(TifColorTest, YCbCrtoRGBClampsExtremeInputsAndOutputs) {
  UniqueTiffBuffer<TIFFYCbCrToRGB> ycbcr = AllocateYcbcrState();
  ASSERT_NE(ycbcr.get(), nullptr);

  std::array<float, 3> luma = {0.299f, 0.587f, 0.114f};
  std::array<float, 6> ref_black_white = {0.0f,   255.0f, 128.0f,
                                          255.0f, 128.0f, 255.0f};
  ASSERT_EQ(
      TIFFYCbCrToRGBInit(ycbcr.get(), luma.data(), ref_black_white.data()), 0);

  uint32_t r_ref = 0, g_ref = 0, b_ref = 0;
  uint32_t r = 0, g = 0, b = 0;

  // Y > 255 clamps to 255.
  TIFFYCbCrtoRGB(ycbcr.get(), 255, 128, 128, &r_ref, &g_ref, &b_ref);
  for (uint32_t y : {256U, 1000U, std::numeric_limits<uint32_t>::max()}) {
    TIFFYCbCrtoRGB(ycbcr.get(), y, 128, 128, &r, &g, &b);
    EXPECT_EQ(r, r_ref);
    EXPECT_EQ(g, g_ref);
    EXPECT_EQ(b, b_ref);
  }

  // Cb, Cr < 0 clamp to 0.
  TIFFYCbCrtoRGB(ycbcr.get(), 128, 0, 0, &r_ref, &g_ref, &b_ref);
  TIFFYCbCrtoRGB(ycbcr.get(), 128, -1, -1, &r, &g, &b);
  EXPECT_EQ(r, r_ref);
  EXPECT_EQ(g, g_ref);
  EXPECT_EQ(b, b_ref);

  TIFFYCbCrtoRGB(ycbcr.get(), 128, std::numeric_limits<int32_t>::min(),
                 std::numeric_limits<int32_t>::min(), &r, &g, &b);
  EXPECT_EQ(r, r_ref);
  EXPECT_EQ(g, g_ref);
  EXPECT_EQ(b, b_ref);

  // Cb, Cr > 255 clamp to 255.
  TIFFYCbCrtoRGB(ycbcr.get(), 128, 255, 255, &r_ref, &g_ref, &b_ref);
  TIFFYCbCrtoRGB(ycbcr.get(), 128, 256, 256, &r, &g, &b);
  EXPECT_EQ(r, r_ref);
  EXPECT_EQ(g, g_ref);
  EXPECT_EQ(b, b_ref);

  TIFFYCbCrtoRGB(ycbcr.get(), 128, std::numeric_limits<int32_t>::max(),
                 std::numeric_limits<int32_t>::max(), &r, &g, &b);
  EXPECT_EQ(r, r_ref);
  EXPECT_EQ(g, g_ref);
  EXPECT_EQ(b, b_ref);

  // Output underflow/overflow clamping on R, G, B independently.
  TIFFYCbCrtoRGB(ycbcr.get(), 0, 0, 0, &r, &g, &b);
  EXPECT_EQ(r, 0U);
  EXPECT_GT(g, 0U);
  EXPECT_EQ(b, 0U);

  TIFFYCbCrtoRGB(ycbcr.get(), 0, 255, 255, &r, &g, &b);
  EXPECT_GT(r, 0U);
  EXPECT_EQ(g, 0U);
  EXPECT_GT(b, 0U);

  TIFFYCbCrtoRGB(ycbcr.get(), 255, 255, 255, &r, &g, &b);
  EXPECT_EQ(r, 255U);
  EXPECT_LT(g, 255U);
  EXPECT_EQ(b, 255U);

  TIFFYCbCrtoRGB(ycbcr.get(), 255, 0, 0, &r, &g, &b);
  EXPECT_LT(r, 255U);
  EXPECT_EQ(g, 255U);
  EXPECT_LT(b, 255U);
}

TEST(TifColorTest, YCbCrToRGBInitExplicitRec709AndStudioSwing) {
  UniqueTiffBuffer<TIFFYCbCrToRGB> ycbcr_709 = AllocateYcbcrState();
  UniqueTiffBuffer<TIFFYCbCrToRGB> ycbcr_601 = AllocateYcbcrState();
  ASSERT_NE(ycbcr_709.get(), nullptr);
  ASSERT_NE(ycbcr_601.get(), nullptr);

  std::array<float, 3> luma_709 = {0.2126f, 0.7152f, 0.0722f};
  std::array<float, 3> luma_601 = {0.299f, 0.587f, 0.114f};
  std::array<float, 6> studio_rbw = {16.0f,  235.0f, 128.0f,
                                     240.0f, 128.0f, 240.0f};

  ASSERT_EQ(
      TIFFYCbCrToRGBInit(ycbcr_709.get(), luma_709.data(), studio_rbw.data()),
      0);
  ASSERT_EQ(
      TIFFYCbCrToRGBInit(ycbcr_601.get(), luma_601.data(), studio_rbw.data()),
      0);

  uint32_t r = 0, g = 0, b = 0;
  // Studio black (Y=16) and footroom (Y=0) map to (0, 0, 0).
  TIFFYCbCrtoRGB(ycbcr_709.get(), 16, 128, 128, &r, &g, &b);
  EXPECT_EQ(r, 0U);
  EXPECT_EQ(g, 0U);
  EXPECT_EQ(b, 0U);
  TIFFYCbCrtoRGB(ycbcr_709.get(), 0, 128, 128, &r, &g, &b);
  EXPECT_EQ(r, 0U);
  EXPECT_EQ(g, 0U);
  EXPECT_EQ(b, 0U);

  // Studio white (Y=235) and headroom (Y=255) map to (255, 255, 255).
  TIFFYCbCrtoRGB(ycbcr_709.get(), 235, 128, 128, &r, &g, &b);
  EXPECT_EQ(r, 255U);
  EXPECT_EQ(g, 255U);
  EXPECT_EQ(b, 255U);
  TIFFYCbCrtoRGB(ycbcr_709.get(), 255, 128, 128, &r, &g, &b);
  EXPECT_EQ(r, 255U);
  EXPECT_EQ(g, 255U);
  EXPECT_EQ(b, 255U);

  // Explicit luma changes chroma weights (Rec. 709 has stronger R-Cr gain than
  // CCIR 601-1 and weaker G-Cr/Cb coupling).
  uint32_t r709 = 0, g709 = 0, b709 = 0;
  uint32_t r601 = 0, g601 = 0, b601 = 0;
  TIFFYCbCrtoRGB(ycbcr_709.get(), 126, 128, 180, &r709, &g709, &b709);
  TIFFYCbCrtoRGB(ycbcr_601.get(), 126, 128, 180, &r601, &g601, &b601);
  EXPECT_GT(r709, r601);
  EXPECT_GT(g709, g601);
  EXPECT_EQ(b709, b601);
}

TEST(TifColorTest, YCbCrToRGBInitZeroDenominatorGuardAndClampwSaturation) {
  UniqueTiffBuffer<TIFFYCbCrToRGB> ycbcr = AllocateYcbcrState();
  ASSERT_NE(ycbcr.get(), nullptr);

  std::array<float, 3> luma = {0.299f, 0.587f, 0.114f};

  // Equal black/white reference levels trigger Code2V's zero-denominator guard
  // (falls back to divisor 1.0f instead of dividing by zero).
  std::array<float, 6> equal_rbw = {128.0f, 128.0f, 128.0f,
                                    128.0f, 128.0f, 128.0f};
  ASSERT_EQ(TIFFYCbCrToRGBInit(ycbcr.get(), luma.data(), equal_rbw.data()), 0);
  uint32_t r = 0, g = 0, b = 0;
  TIFFYCbCrtoRGB(ycbcr.get(), 128, 128, 128, &r, &g, &b);
  EXPECT_EQ(r, 0U);
  EXPECT_EQ(g, 0U);
  EXPECT_EQ(b, 0U);
  TIFFYCbCrtoRGB(ycbcr.get(), 255, 255, 255, &r, &g, &b);
  EXPECT_LE(r, 255U);
  EXPECT_LE(g, 255U);
  EXPECT_LE(b, 255U);

  // Tight 1-unit refBlackWhite span forces Code2V outside [-4096, 4096],
  // exercising both lower and upper branches of static helper CLAMPw().
  std::array<float, 6> tight_rbw = {128.0f, 129.0f, 128.0f,
                                    129.0f, 128.0f, 129.0f};
  ASSERT_EQ(TIFFYCbCrToRGBInit(ycbcr.get(), luma.data(), tight_rbw.data()), 0);
  EXPECT_EQ(ycbcr->Y_tab[0], -4096);
  EXPECT_EQ(ycbcr->Y_tab[255], 4096);
  TIFFYCbCrtoRGB(ycbcr.get(), 0, 0, 0, &r, &g, &b);
  EXPECT_EQ(r, 0U);
  EXPECT_LE(g, 255U);
  EXPECT_EQ(b, 0U);
  TIFFYCbCrtoRGB(ycbcr.get(), 255, 255, 255, &r, &g, &b);
  EXPECT_EQ(r, 255U);
  EXPECT_EQ(g, 0U);
  EXPECT_EQ(b, 255U);

  // Out-of-bounds luma coefficients test CLAMP(f1..f4, 0.0f, 2.0f).
  std::array<float, 6> full_rbw = {0.0f,   255.0f, 128.0f,
                                   255.0f, 128.0f, 255.0f};
  std::array<float, 3> extreme_luma_1 = {-0.5f, 0.2f, 1.5f};
  ASSERT_EQ(
      TIFFYCbCrToRGBInit(ycbcr.get(), extreme_luma_1.data(), full_rbw.data()),
      0);
  // LumaBlue=1.5 -> f3 = -1.0 clamped to 0.0 -> Cb_b_tab is all zeros.
  EXPECT_EQ(ycbcr->Cb_b_tab[0], 0);
  EXPECT_EQ(ycbcr->Cb_b_tab[255], 0);

  std::array<float, 3> extreme_luma_2 = {0.5f, 0.1f, 0.4f};
  ASSERT_EQ(
      TIFFYCbCrToRGBInit(ycbcr.get(), extreme_luma_2.data(), full_rbw.data()),
      0);
  TIFFYCbCrtoRGB(ycbcr.get(), 128, 64, 192, &r, &g, &b);
  EXPECT_LE(r, 255U);
  EXPECT_LE(g, 255U);
  EXPECT_LE(b, 255U);

  // NaN in LumaRed/LumaBlue exercises CLAMP's !((f) >= (min)) NaN guard.
  const float nan_val = std::numeric_limits<float>::quiet_NaN();
  std::array<float, 3> nan_luma = {nan_val, 0.5f, nan_val};
  ASSERT_EQ(TIFFYCbCrToRGBInit(ycbcr.get(), nan_luma.data(), full_rbw.data()),
            0);
  EXPECT_EQ(ycbcr->Cr_r_tab[0], 0);
  EXPECT_EQ(ycbcr->Cb_b_tab[255], 0);
  TIFFYCbCrtoRGB(ycbcr.get(), 100, 200, 50, &r, &g, &b);
  EXPECT_EQ(r, 100U);
  EXPECT_EQ(g, 100U);
  EXPECT_EQ(b, 100U);
}

TEST(TifColorTest, RgbaDecodeIntegrationCIELab8And16Bit) {
  // 8-bit CIELab image through TIFFRGBAImage ->
  // TIFFCIELabToRGBInit/ToXYZ/ToRGB.
  {
    MemoryTiffStream stream;
    std::array<float, 2> wp_d65 = {
        D65_X0 / (D65_X0 + D65_Y0 + D65_Z0),
        D65_Y0 / (D65_X0 + D65_Y0 + D65_Z0),
    };

    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer.get(), nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_CIELAB}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_NONE}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ORIENTATION,
                             uint16_t{ORIENTATION_TOPLEFT}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_WHITEPOINT, wp_d65.data()),
                1);

      // 4 pixels: black, white, mid-gray, warm tone.
      // Note: TIFF stores 8-bit CIELab a* and b* as signed 8-bit two's
      // complement in byte slots.
      std::array<uint8_t, 12> row = {
          0,   0,  0,                         // L=0, a=0, b=0
          255, 0,  0,                         // L=255, a=0, b=0
          128, 0,  0,                         // L=128, a=0, b=0
          140, 40, static_cast<uint8_t>(-30)  // L=140, a=40, b=-30
      };
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);

    const float* read_wp = nullptr;
    ASSERT_EQ(TIFFGetFieldDefaulted(reader.get(), TIFFTAG_WHITEPOINT, &read_wp),
              1);
    ASSERT_NE(read_wp, nullptr);

    TIFFCIELabToRGB ref_cielab{};
    const TIFFDisplay srgb = MakeSrgbDisplay();
    std::array<float, 3> ref_white = {
        read_wp[0] / read_wp[1] * 100.0f,
        100.0f,
        (1.0f - read_wp[0] - read_wp[1]) / read_wp[1] * 100.0f,
    };
    ASSERT_EQ(TIFFCIELabToRGBInit(&ref_cielab, &srgb, ref_white.data()), 0);

    ScopedRgbaImage rgba;
    char emsg[1024] = {};
    ASSERT_TRUE(rgba.Begin(reader.get(), 1, emsg)) << emsg;
    std::array<uint32_t, 4> raster{};
    ASSERT_EQ(rgba.Get(raster.data(), 4, 1), 1);

    const std::array<std::array<int32_t, 3>, 4> expected_lab = {{
        {0, 0, 0},
        {255, 0, 0},
        {128, 0, 0},
        {140, 40, -30},
    }};
    for (size_t i = 0; i < 4; ++i) {
      float X = 0.0f, Y = 0.0f, Z = 0.0f;
      uint32_t rr = 0, gg = 0, bb = 0;
      TIFFCIELabToXYZ(&ref_cielab, static_cast<uint32_t>(expected_lab[i][0]),
                      expected_lab[i][1], expected_lab[i][2], &X, &Y, &Z);
      TIFFXYZToRGB(&ref_cielab, X, Y, Z, &rr, &gg, &bb);
      EXPECT_EQ(TIFFGetR(raster[i]), rr) << "pixel " << i;
      EXPECT_EQ(TIFFGetG(raster[i]), gg) << "pixel " << i;
      EXPECT_EQ(TIFFGetB(raster[i]), bb) << "pixel " << i;
      EXPECT_EQ(TIFFGetA(raster[i]), 255U) << "pixel " << i;
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
  }

  // 16-bit CIELab image through TIFFRGBAImage -> TIFFCIELab16ToXYZ across
  // little-endian, big-endian, and BigTIFF containers.
  for (const char* open_mode : {"w", "wb", "w8"}) {
    SCOPED_TRACE(testing::Message() << "open_mode=" << open_mode);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(open_mode);
      ASSERT_NE(writer.get(), nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{3}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{16}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_CIELAB}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_NONE}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ORIENTATION,
                             uint16_t{ORIENTATION_TOPLEFT}),
                1);

      // Mutable local row buffer because TIFFWriteScanline swaps in place.
      std::array<uint16_t, 9> row = {
          0,
          0,
          0,
          65535,
          0,
          0,
          32768,
          static_cast<uint16_t>(30 * 256),
          static_cast<uint16_t>(-20 * 256),
      };
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);

    // Default WhitePoint when omitted is D50.
    const float* read_wp = nullptr;
    ASSERT_EQ(TIFFGetFieldDefaulted(reader.get(), TIFFTAG_WHITEPOINT, &read_wp),
              1);
    ASSERT_NE(read_wp, nullptr);

    TIFFCIELabToRGB ref_cielab{};
    const TIFFDisplay srgb = MakeSrgbDisplay();
    std::array<float, 3> ref_white = {
        read_wp[0] / read_wp[1] * 100.0f,
        100.0f,
        (1.0f - read_wp[0] - read_wp[1]) / read_wp[1] * 100.0f,
    };
    ASSERT_EQ(TIFFCIELabToRGBInit(&ref_cielab, &srgb, ref_white.data()), 0);

    ScopedRgbaImage rgba;
    char emsg[1024] = {};
    ASSERT_TRUE(rgba.Begin(reader.get(), 1, emsg)) << emsg;
    std::array<uint32_t, 3> raster{};
    ASSERT_EQ(rgba.Get(raster.data(), 3, 1), 1);

    const std::array<std::array<int32_t, 3>, 3> expected_lab16 = {{
        {0, 0, 0},
        {65535, 0, 0},
        {32768, 30 * 256, -20 * 256},
    }};
    for (size_t i = 0; i < 3; ++i) {
      float X = 0.0f, Y = 0.0f, Z = 0.0f;
      uint32_t rr = 0, gg = 0, bb = 0;
      TIFFCIELab16ToXYZ(&ref_cielab,
                        static_cast<uint32_t>(expected_lab16[i][0]),
                        expected_lab16[i][1], expected_lab16[i][2], &X, &Y, &Z);
      TIFFXYZToRGB(&ref_cielab, X, Y, Z, &rr, &gg, &bb);
      EXPECT_EQ(TIFFGetR(raster[i]), rr) << "pixel " << i;
      EXPECT_EQ(TIFFGetG(raster[i]), gg) << "pixel " << i;
      EXPECT_EQ(TIFFGetB(raster[i]), bb) << "pixel " << i;
      EXPECT_EQ(TIFFGetA(raster[i]), 255U) << "pixel " << i;
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
  }
}

TEST(TifColorTest, RgbaDecodeIntegrationYCbCrDefaultAndExplicitTags) {
  for (bool use_explicit : {false, true}) {
    SCOPED_TRACE(testing::Message() << "use_explicit=" << use_explicit);
    MemoryTiffStream stream;
    std::array<float, 3> explicit_luma = {0.2126f, 0.7152f, 0.0722f};
    std::array<float, 6> explicit_rbw = {16.0f,  235.0f, 128.0f,
                                         240.0f, 128.0f, 240.0f};

    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer.get(), nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{1}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_YCBCR}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_NONE}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ORIENTATION,
                             uint16_t{ORIENTATION_TOPLEFT}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRSUBSAMPLING,
                             uint16_t{1}, uint16_t{1}),
                1);
      if (use_explicit) {
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRCOEFFICIENTS,
                               explicit_luma.data()),
                  1);
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_REFERENCEBLACKWHITE,
                               explicit_rbw.data()),
                  1);
      }

      std::array<uint8_t, 12> row = {
          16, 128, 128, 235, 128, 128, 76, 85, 255, 150, 44, 21,
      };
      ASSERT_NE(TIFFWriteEncodedStrip(writer.get(), 0, row.data(),
                                      static_cast<tmsize_t>(row.size())),
                static_cast<tmsize_t>(-1));
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader.get(), nullptr);

    const float* luma_ptr = nullptr;
    const float* rbw_ptr = nullptr;
    ASSERT_EQ(TIFFGetFieldDefaulted(reader.get(), TIFFTAG_YCBCRCOEFFICIENTS,
                                    &luma_ptr),
              1);
    ASSERT_EQ(TIFFGetFieldDefaulted(reader.get(), TIFFTAG_REFERENCEBLACKWHITE,
                                    &rbw_ptr),
              1);
    ASSERT_NE(luma_ptr, nullptr);
    ASSERT_NE(rbw_ptr, nullptr);

    std::array<float, 3> luma{};
    std::array<float, 6> rbw{};
    std::memcpy(luma.data(), luma_ptr, sizeof(luma));
    std::memcpy(rbw.data(), rbw_ptr, sizeof(rbw));

    UniqueTiffBuffer<TIFFYCbCrToRGB> ref_ycbcr = AllocateYcbcrState();
    ASSERT_NE(ref_ycbcr.get(), nullptr);
    ASSERT_EQ(TIFFYCbCrToRGBInit(ref_ycbcr.get(), luma.data(), rbw.data()), 0);

    ScopedRgbaImage rgba;
    char emsg[1024] = {};
    ASSERT_TRUE(rgba.Begin(reader.get(), 1, emsg)) << emsg;
    std::array<uint32_t, 4> raster{};
    ASSERT_EQ(rgba.Get(raster.data(), 4, 1), 1);

    const std::array<std::array<uint32_t, 3>, 4> pixels = {{
        {16, 128, 128},
        {235, 128, 128},
        {76, 85, 255},
        {150, 44, 21},
    }};
    for (size_t i = 0; i < 4; ++i) {
      uint32_t rr = 0, gg = 0, bb = 0;
      TIFFYCbCrtoRGB(ref_ycbcr.get(), pixels[i][0],
                     static_cast<int32_t>(pixels[i][1]),
                     static_cast<int32_t>(pixels[i][2]), &rr, &gg, &bb);
      EXPECT_EQ(TIFFGetR(raster[i]), rr) << "pixel " << i;
      EXPECT_EQ(TIFFGetG(raster[i]), gg) << "pixel " << i;
      EXPECT_EQ(TIFFGetB(raster[i]), bb) << "pixel " << i;
      EXPECT_EQ(TIFFGetA(raster[i]), 255U) << "pixel " << i;
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
  }
}

}  // namespace
}  // namespace libtiff
