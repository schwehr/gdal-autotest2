// Copyright 2026 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests for GDAL's VAX/IEEE floating point conversion (cpl_vax.cpp).

#include "third_party/gdal/port/cpl_vax.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "testing/base/public/gunit.h"

namespace autotest2 {
namespace {

TEST(CplVaxTest, DoubleZeroAndNegativeZero) {
  double val = 0.0;
  CPLIEEEToVaxDouble(&val);
  uint64_t bits = 0;
  std::memcpy(&bits, &val, sizeof(bits));
  EXPECT_EQ(bits, 0u);

  CPLVaxToIEEEDouble(&val);
  EXPECT_EQ(val, 0.0);

  // Negative zero in IEEE has exponent == 0 and sign != 0, which enters the
  // non-underflow branch of CPLIEEEToVaxDouble.  The sign bit survives the
  // round trip.
  double neg_zero = -0.0;
  CPLIEEEToVaxDouble(&neg_zero);
  CPLVaxToIEEEDouble(&neg_zero);
  EXPECT_EQ(neg_zero, 0.0);
  EXPECT_TRUE(std::signbit(neg_zero));
}

TEST(CplVaxTest, DoubleRoundTripNormalAndRoundingBits) {
  const double kValues[] = {
      1.0,
      -1.0,
      3.141592653589793,
      -2.718281828459045,
      123456.78901234567,
      -987654.3210987654,
      1e-30,
      1e30,
  };
  for (double orig : kValues) {
    double v = orig;
    CPLIEEEToVaxDouble(&v);
    CPLVaxToIEEEDouble(&v);
    EXPECT_NEAR(v, orig, std::abs(orig) * 1e-15);
  }

  // Explicitly test CPLVaxToIEEEDouble with rndbits != 0 (low 3 bits of dt.lo
  // set after word swap) and rndbits == 0, for both positive and negative VAX
  // doubles.
  {
    double v = 1.5;
    CPLIEEEToVaxDouble(&v);
    uint8_t raw[8];
    std::memcpy(raw, &v, sizeof(raw));
    // Set bits in the least significant VAX word so rndbits != 0.
    raw[6] = 0xFF;
    raw[7] = 0xFF;
    std::memcpy(&v, raw, sizeof(raw));
    CPLVaxToIEEEDouble(&v);
    EXPECT_NEAR(v, 1.5, 1e-11);
  }
  {
    double v = -1.5;
    CPLIEEEToVaxDouble(&v);
    uint8_t raw[8];
    std::memcpy(raw, &v, sizeof(raw));
    raw[6] = 0xFF;
    raw[7] = 0xFF;
    std::memcpy(&v, raw, sizeof(raw));
    CPLVaxToIEEEDouble(&v);
    EXPECT_NEAR(v, -1.5, 1e-11);
  }
}

TEST(CplVaxTest, DoubleOverflowAndUnderflow) {
  // Positive overflow (exponent > 255 in VAX D-float).
  double pos_huge = 1e100;
  CPLIEEEToVaxDouble(&pos_huge);
  uint8_t raw[8];
  std::memcpy(raw, &pos_huge, sizeof(raw));
  EXPECT_EQ(raw[1], 0x7f);
  EXPECT_EQ(raw[0], 0xff);

  // Negative overflow.
  double neg_huge = -1e100;
  CPLIEEEToVaxDouble(&neg_huge);
  std::memcpy(raw, &neg_huge, sizeof(raw));
  EXPECT_EQ(raw[1], 0xff);
  EXPECT_EQ(raw[0], 0xff);

  // Underflow (exponent < 0 in VAX D-float).
  double tiny = 1e-100;
  CPLIEEEToVaxDouble(&tiny);
  uint64_t bits = 1;
  std::memcpy(&bits, &tiny, sizeof(bits));
  EXPECT_EQ(bits, 0u);

  double neg_tiny = -1e-100;
  CPLIEEEToVaxDouble(&neg_tiny);
  std::memcpy(&bits, &neg_tiny, sizeof(bits));
  EXPECT_EQ(bits, 0u);
}

TEST(CplVaxTest, FloatRoundTripNormalAndZero) {
  float zero = 0.0f;
  CPLIEEEToVaxFloat(&zero);
  uint32_t bits = 1;
  std::memcpy(&bits, &zero, sizeof(bits));
  EXPECT_EQ(bits, 0u);
  CPLVaxToIEEEFloat(&zero);
  EXPECT_EQ(zero, 0.0f);

  const float kValues[] = {1.0f, -1.0f, 0.5f, -123.456f, 1e10f, -1e-10f};
  for (float orig : kValues) {
    float v = orig;
    CPLIEEEToVaxFloat(&v);
    CPLVaxToIEEEFloat(&v);
    EXPECT_FLOAT_EQ(v, orig);
  }
}

TEST(CplVaxTest, VaxToIEEEFloatSpecialExponents) {
  // VAX dirty zero / reserved operand: exp == 0 with sign bit set (vaxf[0] ==
  // 0x80, which comes from from[1] == 0x80, from[0] == 0x00) -> IEEE NaN
  // (all 1s).
  {
    uint8_t raw[4] = {0x00, 0x80, 0x00, 0x00};
    CPLVaxToIEEEFloat(raw);
    float f = 0.0f;
    std::memcpy(&f, raw, sizeof(f));
    EXPECT_TRUE(std::isnan(f));
  }

  // VAX exp == 2: vaxf[0] = 0x01, vaxf[1] = 0x00 -> from[1] = 0x01, from[0] =
  // 0x00. Produces denormalized IEEE float (shifted right by 1).
  {
    uint8_t raw[4] = {0x12, 0x01, 0x34, 0x56};
    CPLVaxToIEEEFloat(raw);
    float f = 0.0f;
    std::memcpy(&f, raw, sizeof(f));
    EXPECT_GT(f, 0.0f);
    // Round-trip back through CPLIEEEToVaxFloat (hits denormalized 0x40
    // branch).
    CPLIEEEToVaxFloat(&f);
    CPLVaxToIEEEFloat(&f);
    EXPECT_GT(f, 0.0f);
  }

  // VAX exp == 1: vaxf[0] = 0x00, vaxf[1] = 0x80 -> from[1] = 0x00, from[0] =
  // 0x80. Produces denormalized IEEE float (shifted right by 2).
  {
    uint8_t raw[4] = {0x92, 0x00, 0x34, 0x56};
    CPLVaxToIEEEFloat(raw);
    float f = 0.0f;
    std::memcpy(&f, raw, sizeof(f));
    EXPECT_GT(f, 0.0f);
    // Round-trip back through CPLIEEEToVaxFloat (hits denormalized 0x20
    // branch).
    CPLIEEEToVaxFloat(&f);
    CPLVaxToIEEEFloat(&f);
    EXPECT_GT(f, 0.0f);
  }
}

TEST(CplVaxTest, IEEEToVaxFloatSpecialCases) {
  // Overflow / NaN / Infinity (exp == 255 or exp == 254).
  {
    float inf = std::numeric_limits<float>::infinity();
    CPLIEEEToVaxFloat(&inf);
    uint8_t raw[4];
    std::memcpy(raw, &inf, sizeof(raw));
    EXPECT_EQ(raw[0], 0xff);
    EXPECT_EQ(raw[1], 0x7f);
  }
  {
    // Biased IEEE exponent 254 (2^127).
    float max_flt = std::ldexp(1.5f, 127);
    CPLIEEEToVaxFloat(&max_flt);
    uint8_t raw[4];
    std::memcpy(raw, &max_flt, sizeof(raw));
    EXPECT_EQ(raw[0], 0xff);
    EXPECT_EQ(raw[1], 0x7f);
  }

  // Denormalized IEEE float with bit 0x40 set in ieee[1] (2^-127).
  {
    float denorm1 = std::ldexp(1.25f, -127);
    CPLIEEEToVaxFloat(&denorm1);
    CPLVaxToIEEEFloat(&denorm1);
    EXPECT_FLOAT_EQ(denorm1, std::ldexp(1.25f, -127));
  }

  // Denormalized IEEE float with bit 0x20 set in ieee[1] (2^-128).
  {
    float denorm2 = std::ldexp(1.25f, -128);
    CPLIEEEToVaxFloat(&denorm2);
    CPLVaxToIEEEFloat(&denorm2);
    EXPECT_FLOAT_EQ(denorm2, std::ldexp(1.25f, -128));
  }

  // Denormalized IEEE float smaller than 2^-128 (underflows to 0 in VAX).
  {
    float tiny_denorm = std::ldexp(1.0f, -130);
    CPLIEEEToVaxFloat(&tiny_denorm);
    uint32_t bits = 1;
    std::memcpy(&bits, &tiny_denorm, sizeof(bits));
    EXPECT_EQ(bits, 0u);
  }
}

}  // namespace
}  // namespace autotest2
