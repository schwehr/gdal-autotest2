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

// Unit tests for third_party/geos/src/math/DD.cpp and
// third_party/geos/include/geos/math/DD.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/math/DD.cpp

#include "third_party/geos/include/geos/math/DD.h"

#include <cmath>
#include <limits>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::math::DD;

TEST(DDTest, ConstructorsAndComparisons) {
  const DD zero;
  EXPECT_TRUE(zero.isZero());
  EXPECT_DOUBLE_EQ(zero.doubleValue(), 0.0);
  EXPECT_DOUBLE_EQ(zero.ToDouble(), 0.0);
  EXPECT_EQ(zero.intValue(), 0);
  EXPECT_EQ(zero, DD(0.0));
  EXPECT_EQ(zero, DD(0.0, 0.0));
  EXPECT_EQ(zero, DD(-0.0, -0.0));

  const DD scalar(3.25);
  EXPECT_DOUBLE_EQ(scalar.doubleValue(), 3.25);
  EXPECT_DOUBLE_EQ(scalar.ToDouble(), 3.25);
  EXPECT_EQ(scalar.intValue(), 3);

  const DD neg_scalar(-42.75);
  EXPECT_EQ(neg_scalar.intValue(), -42);

  // When hi is equal, ordering depends on lo.
  const DD one(1.0);
  const DD above(1.0, 1e-20);
  const DD below(1.0, -1e-20);
  EXPECT_NE(above, one);
  EXPECT_GT(above, one);
  EXPECT_GE(above, one);
  EXPECT_LT(one, above);
  EXPECT_LE(one, above);

  EXPECT_NE(below, one);
  EXPECT_LT(below, one);
  EXPECT_LE(below, one);
  EXPECT_GT(one, below);
  EXPECT_GE(one, below);

  // Equal hi and equal lo.
  const DD copy(1.0, 1e-20);
  EXPECT_TRUE(above == copy);
  EXPECT_FALSE(above != copy);
  EXPECT_FALSE(above < copy);
  EXPECT_TRUE(above <= copy);
  EXPECT_FALSE(above > copy);
  EXPECT_TRUE(above >= copy);

  // Different hi takes precedence over lo.
  const DD low_hi(1.0, 0.5);
  const DD high_hi(2.0, -0.5);
  EXPECT_LT(low_hi, high_hi);
  EXPECT_LE(low_hi, high_hi);
  EXPECT_GT(high_hi, low_hi);
  EXPECT_GE(high_hi, low_hi);
  EXPECT_NE(low_hi, high_hi);

  // NaN never compares equal or ordered.
  const DD nan(std::numeric_limits<double>::quiet_NaN());
  EXPECT_FALSE(nan == nan);
  EXPECT_TRUE(nan != nan);
  EXPECT_FALSE(nan < zero);
  EXPECT_FALSE(nan <= zero);
  EXPECT_FALSE(nan > zero);
  EXPECT_FALSE(nan >= zero);
}

TEST(DDTest, SignPredicatesAndNaN) {
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const double kInf = std::numeric_limits<double>::infinity();

  const DD nan_hi(kNaN);
  const DD nan_pair(kNaN, 1.0);
  EXPECT_TRUE(nan_hi.isNaN());
  EXPECT_TRUE(nan_pair.isNaN());
  EXPECT_FALSE(nan_hi.isZero());
  EXPECT_FALSE(nan_hi.isPositive());
  EXPECT_FALSE(nan_hi.isNegative());

  EXPECT_FALSE(DD(0.0).isNaN());
  EXPECT_FALSE(DD(kInf).isNaN());
  EXPECT_FALSE(DD(-kInf).isNaN());

  EXPECT_TRUE(DD(0.0).isZero());
  EXPECT_TRUE(DD(-0.0, -0.0).isZero());
  EXPECT_FALSE(DD(0.0, 1e-30).isZero());
  EXPECT_FALSE(DD(0.0, -1e-30).isZero());
  EXPECT_FALSE(DD(1.0, 0.0).isZero());

  EXPECT_TRUE(DD(1.0).isPositive());
  EXPECT_TRUE(DD(0.0, 1e-30).isPositive());
  EXPECT_TRUE(DD(-0.0, 1e-30).isPositive());
  EXPECT_FALSE(DD(0.0).isPositive());
  EXPECT_FALSE(DD(0.0, -1e-30).isPositive());
  EXPECT_FALSE(DD(-1.0, 1e-30).isPositive());

  EXPECT_TRUE(DD(-1.0).isNegative());
  EXPECT_TRUE(DD(0.0, -1e-30).isNegative());
  EXPECT_TRUE(DD(-0.0, -1e-30).isNegative());
  EXPECT_FALSE(DD(0.0).isNegative());
  EXPECT_FALSE(DD(0.0, 1e-30).isNegative());
  EXPECT_FALSE(DD(1.0, -1e-30).isNegative());

  // intValue truncates hi without rounding lo, while doubleValue sums hi + lo.
  const DD just_below_five(5.0, -1e-20);
  EXPECT_EQ(just_below_five.intValue(), 5);
  const DD exact_sum(5.0, 0.25);
  EXPECT_DOUBLE_EQ(exact_sum.doubleValue(), 5.25);
  EXPECT_DOUBLE_EQ(exact_sum.ToDouble(), 5.25);
}

TEST(DDTest, AdditionAndSelfAddPreserveLowOrderBits) {
  const double half_ulp = std::ldexp(1.0, -53);
  DD acc(1.0);
  acc.selfAdd(half_ulp);
  EXPECT_EQ(acc, DD(1.0, half_ulp));

  // Adding another half-ulp carries cleanly into hi.
  acc.selfAdd(half_ulp);
  EXPECT_EQ(acc, DD(1.0 + std::ldexp(1.0, -52), 0.0));

  // Non-overlapping scales (~80+20 = 100 bits) stay exact across selfAdd forms.
  const double big_pow2 = std::ldexp(1.0, 80);
  const double small_pow2 = std::ldexp(1.0, -20);
  DD b1(big_pow2);
  b1.selfAdd(1.0, small_pow2);
  EXPECT_EQ(b1, DD(big_pow2, 1.0 + small_pow2));
  EXPECT_EQ(b1 - big_pow2, DD(1.0 + small_pow2));

  DD b2(big_pow2);
  b2.selfAdd(DD(1.0, small_pow2));
  EXPECT_EQ(b2, b1);

  // Commutativity regardless of operand magnitude order.
  EXPECT_EQ(DD(half_ulp) + DD(1.0), DD(1.0, half_ulp));
  EXPECT_EQ(DD(1.0) + half_ulp, DD(1.0, half_ulp));

  // Self-aliasing in selfAdd(const DD&) is safe.
  DD self(1.5, half_ulp);
  self.selfAdd(self);
  EXPECT_EQ(self, DD(3.0, std::ldexp(1.0, -52)));

  // NaN propagation.
  const DD nan_sum = DD(1.0) + std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(nan_sum.isNaN());
}

TEST(DDTest, SubtractionAndSelfSubtract) {
  const double half_ulp = std::ldexp(1.0, -53);
  const DD one_plus = DD(1.0) + half_ulp;
  EXPECT_EQ(one_plus - 1.0, DD(half_ulp));
  EXPECT_EQ(one_plus - DD(1.0, -half_ulp), DD(std::ldexp(1.0, -52)));

  DD sub_dd(10.0, half_ulp);
  sub_dd.selfSubtract(DD(10.0));
  EXPECT_EQ(sub_dd, DD(half_ulp));

  DD sub_pair(10.0, half_ulp);
  sub_pair.selfSubtract(10.0, half_ulp);
  EXPECT_TRUE(sub_pair.isZero());

  DD sub_scalar(10.0, half_ulp);
  sub_scalar.selfSubtract(10.0);
  EXPECT_EQ(sub_scalar, DD(half_ulp));

  // Large cancellation retains lower-word bits.
  const double big_pow2 = std::ldexp(1.0, 80);
  const DD big_plus_one(big_pow2, 1.0);
  EXPECT_EQ(big_plus_one - big_pow2, DD(1.0));
  EXPECT_EQ(big_plus_one - DD(big_pow2), DD(1.0));

  // Self-subtraction aliasing yields zero.
  DD self(3.25, -1e-20);
  self.selfSubtract(self);
  EXPECT_TRUE(self.isZero());
}

TEST(DDTest, MultiplicationAndSelfMultiply) {
  const double delta = std::ldexp(1.0, -30);
  const DD factor = DD(1.0) + delta;
  // (1 + 2^-30)^2 = 1 + 2^-29 + 2^-60 (2^-60 would be lost in double).
  EXPECT_EQ(factor * factor,
            DD(1.0 + std::ldexp(1.0, -29), std::ldexp(1.0, -60)));

  // Difference of squares: (1 + 2^-40) * (1 - 2^-40) = 1 - 2^-80.
  const double d40 = std::ldexp(1.0, -40);
  EXPECT_EQ((DD(1.0) + d40) * (DD(1.0) - d40), DD(1.0) - std::ldexp(1.0, -80));

  // Operator*(DD, double) and all selfMultiply overloads.
  const double tiny = std::ldexp(1.0, -60);
  EXPECT_EQ(DD(1.0, tiny) * 3.0, DD(3.0, 3.0 * tiny));

  DD m1(1.0, tiny);
  m1.selfMultiply(DD(2.0, tiny));
  EXPECT_EQ(m1, DD(2.0, 3.0 * tiny));

  DD m2(1.0, tiny);
  m2.selfMultiply(2.0, tiny);
  EXPECT_EQ(m2, DD(2.0, 3.0 * tiny));

  DD m3(1.0, tiny);
  m3.selfMultiply(3.0);
  EXPECT_EQ(m3, DD(3.0, 3.0 * tiny));

  // Self-aliasing squaring, zero, sign flip, and NaN propagation.
  DD sq(factor);
  sq.selfMultiply(sq);
  EXPECT_EQ(sq, DD(1.0 + std::ldexp(1.0, -29), std::ldexp(1.0, -60)));

  EXPECT_TRUE((DD(12.5, tiny) * 0.0).isZero());
  EXPECT_EQ(DD(1.0, tiny) * -1.0, DD(-1.0, -tiny));
  EXPECT_TRUE((DD(1.0) * std::numeric_limits<double>::quiet_NaN()).isNaN());
}

TEST(DDTest, DivisionSelfDivideAndReciprocal) {
  const double d40 = std::ldexp(1.0, -40);
  const DD exact_quot = (DD(1.0) - std::ldexp(1.0, -80)) / (DD(1.0) + d40);
  EXPECT_LT(DD::abs(exact_quot - (DD(1.0) - d40)), DD(std::ldexp(1.0, -102)));

  // Repeating binary expansion 1/3 * 3 == 1 to ~100+ bits.
  const DD third = DD(1.0) / 3.0;
  EXPECT_LT(DD::abs(third * 3.0 - 1.0), DD(1e-30));

  // Non-zero low words in both numerator and denominator.
  const DD num = DD(10.0) / 7.0;
  const DD den = DD(2.0) / 7.0;
  EXPECT_LT(DD::abs(num / den - 5.0), DD(1e-30));

  DD d1(1.0);
  d1.selfDivide(DD(3.0));
  EXPECT_EQ(d1, third);

  const double eps = std::ldexp(1.0, -60);
  DD d2(1.0);
  d2.selfDivide(1.0, eps);
  EXPECT_LT(DD::abs(d2 * DD(1.0, eps) - 1.0), DD(1e-30));

  DD d3(1.0);
  d3.selfDivide(3.0);
  EXPECT_EQ(d3, third);

  // Self-aliasing division returns 1.
  DD self = third;
  self.selfDivide(self);
  EXPECT_EQ(self, DD(1.0));

  // Reciprocal cases including non-zero lo and NaN.
  EXPECT_EQ(DD(2.0).reciprocal(), DD(0.5));
  EXPECT_EQ(DD(-4.0).reciprocal(), DD(-0.25));
  EXPECT_LT(DD::abs(DD(3.0).reciprocal() * 3.0 - 1.0), DD(1e-30));

  const DD perturbed(1.0, std::ldexp(1.0, -53));
  EXPECT_LT(DD::abs(perturbed * perturbed.reciprocal() - 1.0), DD(1e-30));

  EXPECT_TRUE(
      DD(std::numeric_limits<double>::quiet_NaN()).reciprocal().isNaN());
}

TEST(DDTest, NegateAndAbs) {
  EXPECT_EQ(DD(3.5, 1e-20).negate(), DD(-3.5, -1e-20));
  EXPECT_EQ(DD(-3.5, -1e-20).negate(), DD(3.5, 1e-20));
  EXPECT_EQ(DD(0.0, -1e-20).negate(), DD(-0.0, 1e-20));
  EXPECT_TRUE(DD(0.0, -1e-20).negate().isPositive());
  EXPECT_TRUE(DD(0.0).negate().isZero());

  const DD nan(std::numeric_limits<double>::quiet_NaN());
  EXPECT_TRUE(nan.negate().isNaN());

  EXPECT_EQ(DD::abs(DD(3.5, 1e-20)), DD(3.5, 1e-20));
  EXPECT_EQ(DD::abs(DD(-3.5, -1e-20)), DD(3.5, 1e-20));
  EXPECT_EQ(DD::abs(DD(0.0, -1e-20)), DD(-0.0, 1e-20));
  EXPECT_TRUE(DD::abs(DD(0.0, -1e-20)).isPositive());
  EXPECT_TRUE(DD::abs(DD(0.0)).isZero());
  EXPECT_TRUE(DD::abs(DD(-0.0, 0.0)).isZero());
  EXPECT_TRUE(DD::abs(nan).isNaN());
}

TEST(DDTest, FloorCeilAndTrunc) {
  const DD nan(std::numeric_limits<double>::quiet_NaN());
  EXPECT_TRUE(nan.floor().isNaN());
  EXPECT_TRUE(nan.ceil().isNaN());
  EXPECT_TRUE(DD::trunc(nan).isNaN());

  // Non-integral hi.
  EXPECT_EQ(DD(3.75, 1e-20).floor(), DD(3.0));
  EXPECT_EQ(DD(-3.25, 1e-20).floor(), DD(-4.0));
  EXPECT_EQ(DD(3.25, -1e-20).ceil(), DD(4.0));
  EXPECT_EQ(DD(-3.75, 1e-20).ceil(), DD(-3.0));

  // Integral hi with zero or same-sign fractional lo.
  EXPECT_EQ(DD(5.0, 0.0).floor(), DD(5.0));
  EXPECT_EQ(DD(5.0, 1e-20).floor(), DD(5.0));
  EXPECT_EQ(DD(-5.0, 0.0).ceil(), DD(-5.0));
  EXPECT_DOUBLE_EQ(DD(5.0, -1e-20).ceil().doubleValue(), 5.0);

  // Integral hi where lo crosses the integer boundary.
  const DD floor_cross = DD(5.0, -1e-20).floor();
  EXPECT_DOUBLE_EQ(floor_cross.doubleValue(), 4.0);
  EXPECT_TRUE((floor_cross - DD(4.0)).isZero());

  const DD neg_floor_cross = DD(-5.0, -1e-20).floor();
  EXPECT_DOUBLE_EQ(neg_floor_cross.doubleValue(), -6.0);
  EXPECT_TRUE((neg_floor_cross - DD(-6.0)).isZero());

  const DD ceil_cross = DD(5.0, 1e-20).ceil();
  EXPECT_DOUBLE_EQ(ceil_cross.doubleValue(), 6.0);
  EXPECT_TRUE((ceil_cross - DD(6.0)).isZero());

  const DD neg_ceil_cross = DD(-5.0, 1e-20).ceil();
  EXPECT_DOUBLE_EQ(neg_ceil_cross.doubleValue(), -4.0);
  EXPECT_TRUE((neg_ceil_cross - DD(-4.0)).isZero());

  // Truncation towards zero across positive, negative, zero, and boundary
  // values.
  EXPECT_EQ(DD::trunc(DD(3.75, 1e-20)), DD(3.0));
  EXPECT_EQ(DD::trunc(DD(-3.75, 1e-20)), DD(-3.0));
  EXPECT_TRUE(DD::trunc(DD(0.0)).isZero());
  EXPECT_TRUE(DD::trunc(DD(0.0, 1e-20)).isZero());
  EXPECT_TRUE(DD::trunc(DD(0.0, -1e-20)).isZero());
  EXPECT_DOUBLE_EQ(DD::trunc(DD(5.0, -1e-20)).doubleValue(), 4.0);
  EXPECT_DOUBLE_EQ(DD::trunc(DD(-5.0, 1e-20)).doubleValue(), -4.0);
}

TEST(DDTest, DeterminantAvoidsCatastrophicCancellation) {
  // (2^28 + 1)^2 - 2^28 * (2^28 + 2) == 1, whereas double rounds to 0.
  const double k = std::ldexp(1.0, 28);
  ASSERT_DOUBLE_EQ((k + 1.0) * (k + 1.0) - k * (k + 2.0), 0.0);

  EXPECT_EQ(DD::determinant(k + 1.0, k, k + 2.0, k + 1.0), DD(1.0));
  EXPECT_EQ(DD::determinant(k + 2.0, k + 1.0, k + 1.0, k), DD(-1.0));
  EXPECT_EQ(DD::determinant(2.0, 4.0, 3.0, 6.0), DD(0.0));

  const double eps = std::ldexp(1.0, -53);
  EXPECT_EQ(DD::determinant(DD(1.0, eps), DD(1.0), DD(1.0), DD(1.0, eps)),
            DD(std::ldexp(1.0, -52)));
}

TEST(DDTest, PowHandlesZeroPositiveNegativeAndHighPrecision) {
  const DD nan(std::numeric_limits<double>::quiet_NaN());

  // Exponent 0 always returns 1.0.
  EXPECT_EQ(DD::pow(DD(5.0), 0), DD(1.0));
  EXPECT_EQ(DD::pow(DD(0.0), 0), DD(1.0));
  EXPECT_EQ(DD::pow(DD(-3.5), 0), DD(1.0));
  EXPECT_EQ(DD::pow(nan, 0), DD(1.0));

  // Exponents 1 and -1 bypass the binary exponentiation loop.
  EXPECT_EQ(DD::pow(DD(4.0, 1e-20), 1), DD(4.0, 1e-20));
  EXPECT_EQ(DD::pow(DD(4.0), -1), DD(0.25));
  EXPECT_LT(DD::abs(DD::pow(DD(3.0), -1) * 3.0 - 1.0), DD(1e-30));

  // Positive exponents (> 1), even and odd, positive, negative, and zero bases.
  EXPECT_EQ(DD::pow(DD(2.0), 2), DD(4.0));
  EXPECT_EQ(DD::pow(DD(2.0), 3), DD(8.0));
  EXPECT_EQ(DD::pow(DD(2.0), 10), DD(1024.0));
  EXPECT_EQ(DD::pow(DD(-2.0), 5), DD(-32.0));
  EXPECT_EQ(DD::pow(DD(-2.0), 6), DD(64.0));
  EXPECT_EQ(DD::pow(DD(0.0), 4), DD(0.0));

  const double delta = std::ldexp(1.0, -30);
  const DD base = DD(1.0) + delta;
  EXPECT_EQ(DD::pow(base, 2),
            DD(1.0 + std::ldexp(1.0, -29), std::ldexp(1.0, -60)));

  // Negative exponents (< -1).
  EXPECT_EQ(DD::pow(DD(2.0), -3), DD(0.125));
  EXPECT_EQ(DD::pow(DD(2.0), -10), DD(std::ldexp(1.0, -10)));
  EXPECT_LT(DD::abs(DD::pow(DD(3.0), -5) * 243.0 - 1.0), DD(1e-30));
  EXPECT_LT(DD::abs(DD::pow(base, -5) * DD::pow(base, 5) - 1.0), DD(1e-29));
}

}  // namespace
}  // namespace geos_google
