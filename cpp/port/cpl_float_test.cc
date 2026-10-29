// Tests for cpl_float:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_float.h
//
// Copyright 2025 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// This is a complete rewrite of a file licensed as follows:
//
// Copyright (c) 2024, Even Rouault <even dot rouault at spatialys.com>
//
// SPDX-License-Identifier: MIT
//
// https://github.com/OSGeo/gdal/blob/master/autotest/cpp/testfloat16.cpp

#include "third_party/gdal/port/cpl_float.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

TEST(CplFloat16Test, Coversions) {
  for (int i = -2048; i <= +2048; ++i) {
    EXPECT_EQ(GFloat16(i), static_cast<double>(i));
    if (i >= -128 && i <= 127) {
      EXPECT_EQ(GFloat16(static_cast<signed char>(i)), static_cast<double>(i));
    }
    EXPECT_EQ(GFloat16(static_cast<short>(i)), static_cast<double>(i));
    EXPECT_EQ(GFloat16(static_cast<int>(i)), static_cast<double>(i));
    EXPECT_EQ(GFloat16(static_cast<long>(i)), static_cast<double>(i));
    EXPECT_EQ(GFloat16(static_cast<long long>(i)), static_cast<double>(i));
    if (i >= 0) {
      if (i <= 255) {
        EXPECT_EQ(GFloat16(static_cast<unsigned char>(i)),
                  static_cast<double>(i));
      }
      EXPECT_EQ(GFloat16(static_cast<unsigned short>(i)),
                static_cast<double>(i));
      EXPECT_EQ(GFloat16(static_cast<unsigned>(i)), static_cast<double>(i));
      EXPECT_EQ(GFloat16(static_cast<unsigned long>(i)),
                static_cast<double>(i));
      EXPECT_EQ(GFloat16(static_cast<unsigned long long>(i)),
                static_cast<double>(i));
    }
    EXPECT_EQ(GFloat16(i), GFloat16(i));
    EXPECT_EQ(GFloat16(i), static_cast<double>(i));
  }

  EXPECT_EQ(GFloat16(65504), 65504.0);
  EXPECT_EQ(GFloat16(-65504), -65504.0);
  // Work around the Windows compiler reporting "error C2124: divide
  // or mod by zero". See also
  // <https://stackoverflow.com/questions/3082508/msvc-erroring-on-a-divide-by-0-that-will-never-happen-fix>.
  volatile double zero = 0.0;
  EXPECT_EQ(GFloat16(1.0 / zero), 1.0 / zero);
  EXPECT_EQ(GFloat16(-1.0 / zero), -1.0 / zero);
  EXPECT_EQ(GFloat16(0.0), -0.0);
  EXPECT_EQ(GFloat16(-0.0), 0.0);
}

TEST(CplFloat16Test, Arithmetic) {
  for (int i = -100; i <= +100; ++i) {
    double x = i;

    EXPECT_EQ(+GFloat16(x), +x);
    EXPECT_EQ(-GFloat16(x), -x);
  }

  for (int i = -100; i <= +100; ++i) {
    for (int j = -100; j <= +100; ++j) {
      double x = i;
      double y = j;

      EXPECT_EQ(GFloat16(x) + GFloat16(y), x + y);
      EXPECT_EQ(GFloat16(x) - GFloat16(y), x - y);
      EXPECT_NEAR(GFloat16(x) * GFloat16(y), x * y, std::fabs(x * y / 1024));
      if (j != 0) {
        EXPECT_NEAR(GFloat16(x) / GFloat16(y), x / y, std::fabs(x / y / 1024));
      }
    }
  }
}

TEST(CplFloat16Test, Comparisons) {
  for (int i = -100; i <= +100; ++i) {
    for (int j = -100; j <= +100; ++j) {
      double x = i;
      double y = j;

      EXPECT_EQ(GFloat16(x) == GFloat16(y), x == y);
      EXPECT_EQ(GFloat16(x) != GFloat16(y), x != y);
      EXPECT_EQ(GFloat16(x) < GFloat16(y), x < y);
      EXPECT_EQ(GFloat16(x) > GFloat16(y), x > y);
      EXPECT_EQ(GFloat16(x) <= GFloat16(y), x <= y);
      EXPECT_EQ(GFloat16(x) >= GFloat16(y), x >= y);
    }
  }
}

TEST(CplFloat16Test, Math) {
  for (int i = -100; i <= +100; ++i) {
    double x = i;

    // TODO(schwehr): How to test cases with: "call to 'x' is ambiguous"?
    // EXPECT_EQ(std::isfinite(GFloat16(x)), isfinite(x));
    EXPECT_EQ(std::isinf(GFloat16(x)), isinf(x));
    EXPECT_EQ(std::isnan(GFloat16(x)), isnan(x));
    // EXPECT_EQ(std::abs(GFloat16(x)), abs(x));
    // EXPECT_NEAR(std::cbrt(GFloat16(x)), cbrt(x), fabs(cbrt(x) / 1024));
    // EXPECT_EQ(std::ceil(GFloat16(x)), ceil(x));
    // EXPECT_EQ(std::fabs(GFloat16(x)), fabs(x));
    // EXPECT_EQ(std::floor(GFloat16(x)), floor(x));
    // EXPECT_EQ(std::round(GFloat16(x)), round(x));
    // if (x >= 0) {
    //   EXPECT_NEAR(std::sqrt(GFloat16(x)), sqrt(x), fabs(sqrt(x) / 1024));
    // }
  }

  for (int i = -100; i <= +100; ++i) {
    for (int j = -100; j <= +100; ++j) {
      double x = i;
      double y = j;

      // EXPECT_EQ(std::fmax(GFloat16(x), GFloat16(y)), GFloat16(std::fmax(x,
      // y)));

      // EXPECT_EQ(std::fmin(GFloat16(x), GFloat16(y)), GFloat16(std::fmin(x,
      // y)));

      // EXPECT_EQ(std::hypot(GFloat16(x), GFloat16(y)), GFloat16(std::hypot(x,
      // y)));
      EXPECT_EQ(std::max(GFloat16(x), GFloat16(y)), GFloat16(std::max(x, y)));
      EXPECT_EQ(std::min(GFloat16(x), GFloat16(y)), GFloat16(std::min(x, y)));
    }
  }
}

TEST(CplFloat16Test, HalfAndTripleToFloatFunctions) {
  // CPLHalfToFloat and CPLConvertHalfToFloat:
  // +0 and -0
  EXPECT_EQ(0x00000000U, CPLHalfToFloat(0x0000));
  EXPECT_EQ(0x80000000U, CPLHalfToFloat(0x8000));
  EXPECT_FLOAT_EQ(0.0f, CPLConvertHalfToFloat(0x0000));

  // Denormalized half (exp = 0, mantissa != 0)
  EXPECT_NE(0U, CPLHalfToFloat(0x0001));
  EXPECT_GT(CPLConvertHalfToFloat(0x0001), 0.0f);

  // Normalized half (1.0 = 0x3C00 -> 0x3F800000)
  EXPECT_EQ(0x3F800000U, CPLHalfToFloat(0x3C00));
  EXPECT_FLOAT_EQ(1.0f, CPLConvertHalfToFloat(0x3C00));

  // +Inf and -Inf (exp = 31, mantissa = 0)
  EXPECT_EQ(0x7F800000U, CPLHalfToFloat(0x7C00));
  EXPECT_EQ(0xFF800000U, CPLHalfToFloat(0xFC00));
  EXPECT_TRUE(std::isinf(CPLConvertHalfToFloat(0x7C00)));

  // NaN (exp = 31, mantissa != 0)
  EXPECT_EQ(0x7F802000U, CPLHalfToFloat(0x7C01));
  EXPECT_TRUE(std::isnan(CPLConvertHalfToFloat(0x7C01)));

  // CPLTripleToFloat:
  // +0 and -0
  EXPECT_EQ(0x00000000U, CPLTripleToFloat(0x000000));
  EXPECT_EQ(0x80000000U, CPLTripleToFloat(0x800000));

  // Denormalized triple (exp = 0, mantissa != 0)
  EXPECT_NE(0U, CPLTripleToFloat(0x000001));

  // Normalized triple (exp = 63 -> 1.0 -> 0x3F0000 -> 0x3F800000)
  EXPECT_EQ(0x3F800000U, CPLTripleToFloat(0x3F0000));

  // +Inf and -Inf (exp = 127, mantissa = 0)
  EXPECT_EQ(0x7F800000U, CPLTripleToFloat(0x7F0000));
  EXPECT_EQ(0xFF800000U, CPLTripleToFloat(0xFF0000));

  // NaN (exp = 127, mantissa != 0)
  EXPECT_EQ(0x7F800080U, CPLTripleToFloat(0x7F0001));
}

TEST(CplFloat16Test, FloatToHalfFunctions) {
  bool has_warned = false;

  // +Inf and -Inf
  EXPECT_EQ(0x7C00, CPLFloatToHalf(0x7F800000U, has_warned));
  EXPECT_EQ(0xFC00, CPLFloatToHalf(0xFF800000U, has_warned));

  // NaN with upper mantissa bits set vs only lower mantissa bits set
  EXPECT_EQ(0x7E00, CPLFloatToHalf(0x7FC00000U, has_warned));
  EXPECT_EQ(0x7E00, CPLFloatToHalf(0x7F800001U, has_warned));

  // Zero / underflow to zero (iExponent == 0)
  EXPECT_EQ(0x0000, CPLFloatToHalf(0x00000000U, has_warned));
  EXPECT_EQ(0x8000, CPLFloatToHalf(0x80000000U, has_warned));

  // Denormalized float16 result (iExponent = 103..112, e.g. 105 << 23)
  EXPECT_NE(0x0000, CPLFloatToHalf(105U << 23, has_warned));

  // Normalized float16 result (1.0f = 0x3F800000 -> 0x3C00)
  EXPECT_EQ(0x3C00, CPLFloatToHalf(0x3F800000U, has_warned));
  EXPECT_EQ(0x3C00, CPLConvertFloatToHalf(1.0f));

  // Overflow beyond float16 range triggers warning once when bHasWarned=false
  CPLPushErrorHandler(CPLQuietErrorHandler);
  has_warned = false;
  GUInt32 big_pos = 0;
  float f_big = 100000.0f;
  std::memcpy(&big_pos, &f_big, 4);
  EXPECT_EQ(0x7C00, CPLFloatToHalf(big_pos, has_warned));
  EXPECT_TRUE(has_warned);
  // Second call with has_warned=true does not warn again.
  EXPECT_EQ(0x7C00, CPLFloatToHalf(big_pos, has_warned));

  has_warned = false;
  float f_neg_big = -100000.0f;
  GUInt32 big_neg = 0;
  std::memcpy(&big_neg, &f_neg_big, 4);
  EXPECT_EQ(0xFC00, CPLFloatToHalf(big_neg, has_warned));
  CPLPopErrorHandler();
}

TEST(CplFloat16Test, GreatestCommonDivisor) {
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // Zero or non-finite inputs return 0.
  EXPECT_DOUBLE_EQ(0.0, CPLGreatestCommonDivisor(0.0, 1.0));
  EXPECT_DOUBLE_EQ(0.0, CPLGreatestCommonDivisor(1.0, 0.0));
  EXPECT_DOUBLE_EQ(0.0, CPLGreatestCommonDivisor(
                            std::numeric_limits<double>::infinity(), 1.0));
  EXPECT_DOUBLE_EQ(0.0, CPLGreatestCommonDivisor(
                            1.0, std::numeric_limits<double>::quiet_NaN()));

  // Equal inputs.
  EXPECT_DOUBLE_EQ(2.5, CPLGreatestCommonDivisor(2.5, 2.5));

  // Integer multiple fast paths (a > b and b > a).
  EXPECT_DOUBLE_EQ(2.5, CPLGreatestCommonDivisor(10.0, 2.5));
  EXPECT_DOUBLE_EQ(2.5, CPLGreatestCommonDivisor(2.5, 10.0));

  // Fractional GCD using continued fractions.
  EXPECT_NEAR(0.5, CPLGreatestCommonDivisor(1.5, 2.5), 1e-9);
  EXPECT_NEAR(1.0 / 6.0, CPLGreatestCommonDivisor(0.5, 1.0 / 3.0), 1e-9);

  // Excessive disaggregation factor (> 10000) returns 0.
  EXPECT_DOUBLE_EQ(0.0, CPLGreatestCommonDivisor(1001.0, 0.3));

  // Values too large for uint64_t in FloatToFraction (> 1.84467e19) fail
  // approximation on first or second argument.
  EXPECT_DOUBLE_EQ(0.0, CPLGreatestCommonDivisor(2.3e19, 1.0e19));
  EXPECT_DOUBLE_EQ(0.0, CPLGreatestCommonDivisor(1.0e19, 2.3e19));

  CPLPopErrorHandler();
}

}  // namespace
}  // namespace autotest2
