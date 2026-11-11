// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/util/math.h"

#include <cmath>
#include <limits>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

TEST(MathTest, SymRoundRoundsHalfAwayFromZero) {
  // Fractional part < 0.5
  EXPECT_DOUBLE_EQ(geos::util::sym_round(0.0), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(-0.0), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(0.4), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(2.49), 2.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(-0.4), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(-2.49), -2.0);

  // Fractional part > 0.5
  EXPECT_DOUBLE_EQ(geos::util::sym_round(0.6), 1.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(2.51), 3.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(-0.6), -1.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(-2.51), -3.0);

  // Fractional part == 0.5 (symmetric away from zero)
  EXPECT_DOUBLE_EQ(geos::util::sym_round(0.5), 1.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(1.5), 2.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(2.5), 3.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(-0.5), -1.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(-1.5), -2.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(-2.5), -3.0);

  // Exact integers and non-finite inputs
  EXPECT_DOUBLE_EQ(geos::util::sym_round(4.0), 4.0);
  EXPECT_DOUBLE_EQ(geos::util::sym_round(-4.0), -4.0);
  EXPECT_TRUE(std::isnan(
      geos::util::sym_round(std::numeric_limits<double>::quiet_NaN())));
  EXPECT_EQ(geos::util::sym_round(std::numeric_limits<double>::infinity()),
            std::numeric_limits<double>::infinity());
  EXPECT_EQ(geos::util::sym_round(-std::numeric_limits<double>::infinity()),
            -std::numeric_limits<double>::infinity());
}

TEST(MathTest, JavaMathRoundRoundsHalfTowardsPositiveInfinity) {
  // Fractional part < 0.5
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(0.0), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(0.49), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(3.2), 3.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(-0.49), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(-3.2), -3.0);

  // Fractional part > 0.5
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(0.51), 1.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(3.8), 4.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(-0.51), -1.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(-3.8), -4.0);

  // Fractional part == 0.5 (asymmetric toward +infinity)
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(0.5), 1.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(1.5), 2.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(2.5), 3.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(-0.5), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(-1.5), -1.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(-2.5), -2.0);

  // Exact integers and non-finite inputs
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(7.0), 7.0);
  EXPECT_DOUBLE_EQ(geos::util::java_math_round(-7.0), -7.0);
  EXPECT_TRUE(std::isnan(
      geos::util::java_math_round(std::numeric_limits<double>::quiet_NaN())));
  EXPECT_EQ(
      geos::util::java_math_round(std::numeric_limits<double>::infinity()),
      std::numeric_limits<double>::infinity());
  EXPECT_EQ(
      geos::util::java_math_round(-std::numeric_limits<double>::infinity()),
      -std::numeric_limits<double>::infinity());
}

TEST(MathTest, DefaultRoundDelegatesToJavaMathRound) {
  constexpr double kSamples[] = {-3.7, -2.5, -1.5, -0.5, -0.2, 0.0,
                                 0.2,  0.5,  1.5,  2.5,  3.7};
  for (double val : kSamples) {
    EXPECT_DOUBLE_EQ(geos::util::round(val), geos::util::java_math_round(val));
  }
  EXPECT_TRUE(
      std::isnan(geos::util::round(std::numeric_limits<double>::quiet_NaN())));
}

TEST(MathTest, RintVcRoundsHalfToEven) {
  // Fractional part < 0.5
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(0.0), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(0.4), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(2.3), 2.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(-0.4), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(-2.3), -2.0);

  // Fractional part > 0.5
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(0.6), 1.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(2.7), 3.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(-0.6), -1.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(-2.7), -3.0);

  // Fractional part == 0.5 with even integer part (stays even)
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(0.5), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(2.5), 2.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(4.5), 4.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(-0.5), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(-2.5), -2.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(-4.5), -4.0);

  // Fractional part == 0.5 with odd integer part (rounds away from zero to
  // even)
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(1.5), 2.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(3.5), 4.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(-1.5), -2.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(-3.5), -4.0);

  // Exact integers and non-finite inputs
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(5.0), 5.0);
  EXPECT_DOUBLE_EQ(geos::util::rint_vc(-5.0), -5.0);
  EXPECT_TRUE(std::isnan(
      geos::util::rint_vc(std::numeric_limits<double>::quiet_NaN())));
  EXPECT_EQ(geos::util::rint_vc(std::numeric_limits<double>::infinity()),
            std::numeric_limits<double>::infinity());
  EXPECT_EQ(geos::util::rint_vc(-std::numeric_limits<double>::infinity()),
            -std::numeric_limits<double>::infinity());
}

TEST(MathTest, ClampHandlesBoundsAndSpecialValues) {
  EXPECT_DOUBLE_EQ(geos::util::clamp(-5.0, 0.0, 10.0), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::clamp(15.0, 0.0, 10.0), 10.0);
  EXPECT_DOUBLE_EQ(geos::util::clamp(4.25, 0.0, 10.0), 4.25);
  EXPECT_DOUBLE_EQ(geos::util::clamp(0.0, 0.0, 10.0), 0.0);
  EXPECT_DOUBLE_EQ(geos::util::clamp(10.0, 0.0, 10.0), 10.0);

  EXPECT_DOUBLE_EQ(geos::util::clamp(-5.0, -10.0, -1.0), -5.0);
  EXPECT_DOUBLE_EQ(geos::util::clamp(-20.0, -10.0, -1.0), -10.0);
  EXPECT_DOUBLE_EQ(geos::util::clamp(0.0, -10.0, -1.0), -1.0);

  EXPECT_DOUBLE_EQ(geos::util::clamp(1.0, 3.0, 3.0), 3.0);
  EXPECT_DOUBLE_EQ(geos::util::clamp(5.0, 3.0, 3.0), 3.0);
  EXPECT_DOUBLE_EQ(geos::util::clamp(3.0, 3.0, 3.0), 3.0);

  EXPECT_DOUBLE_EQ(
      geos::util::clamp(-std::numeric_limits<double>::infinity(), -1.0, 1.0),
      -1.0);
  EXPECT_DOUBLE_EQ(
      geos::util::clamp(std::numeric_limits<double>::infinity(), -1.0, 1.0),
      1.0);
  EXPECT_TRUE(std::isnan(
      geos::util::clamp(std::numeric_limits<double>::quiet_NaN(), 0.0, 10.0)));
}

}  // namespace
}  // namespace geos_google
