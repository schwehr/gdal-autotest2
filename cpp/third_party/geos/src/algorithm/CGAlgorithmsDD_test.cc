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

// Unit tests for third_party/geos/src/algorithm/CGAlgorithmsDD.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/CGAlgorithmsDD.cpp

#include "third_party/geos/include/geos/algorithm/CGAlgorithmsDD.h"

#include <array>
#include <cmath>
#include <limits>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/math/DD.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::algorithm::CGAlgorithmsDD;
using geos::geom::CoordinateXY;
using geos::math::DD;
using geos::util::IllegalArgumentException;

static_assert(CGAlgorithmsDD::CLOCKWISE == -1);
static_assert(CGAlgorithmsDD::COLLINEAR == 0);
static_assert(CGAlgorithmsDD::COUNTERCLOCKWISE == 1);
static_assert(CGAlgorithmsDD::RIGHT == -1);
static_assert(CGAlgorithmsDD::STRAIGHT == 0);
static_assert(CGAlgorithmsDD::LEFT == 1);
static_assert(CGAlgorithmsDD::FAILURE == 2);

struct OrientationCase {
  std::string_view name;
  double p1x;
  double p1y;
  double p2x;
  double p2y;
  double qx;
  double qy;
  int expected;
};

TEST(CGAlgorithmsDDTest, OrientationFromScalar) {
  EXPECT_EQ(CGAlgorithmsDD::orientation(-1.5), CGAlgorithmsDD::RIGHT);
  EXPECT_EQ(CGAlgorithmsDD::orientation(-1e-300), CGAlgorithmsDD::RIGHT);
  EXPECT_EQ(CGAlgorithmsDD::orientation(0.0), CGAlgorithmsDD::STRAIGHT);
  EXPECT_EQ(CGAlgorithmsDD::orientation(-0.0), CGAlgorithmsDD::STRAIGHT);
  EXPECT_EQ(CGAlgorithmsDD::orientation(1e-300), CGAlgorithmsDD::LEFT);
  EXPECT_EQ(CGAlgorithmsDD::orientation(2.5), CGAlgorithmsDD::LEFT);
}

TEST(CGAlgorithmsDDTest, OrientationIndexFilterFastAndFailurePaths) {
  EXPECT_EQ(
      CGAlgorithmsDD::orientationIndexFilter(0.0, 0.0, 10.0, 0.0, 5.0, 3.0),
      CGAlgorithmsDD::LEFT);
  EXPECT_EQ(
      CGAlgorithmsDD::orientationIndexFilter(0.0, 0.0, 10.0, 0.0, 5.0, -3.0),
      CGAlgorithmsDD::RIGHT);
  EXPECT_EQ(
      CGAlgorithmsDD::orientationIndexFilter(0.0, 0.0, 10.0, 0.0, 5.0, 0.0),
      CGAlgorithmsDD::STRAIGHT);

  // Non-axis-aligned collinear points produce detleft == detright != 0,
  // triggering the error bound and returning FAILURE.
  EXPECT_EQ(
      CGAlgorithmsDD::orientationIndexFilter(0.0, 0.0, 1.0, 1.0, 2.0, 2.0),
      CGAlgorithmsDD::FAILURE);

  // Coincident endpoints at the query point yield det == 0 and error == 0.
  EXPECT_EQ(
      CGAlgorithmsDD::orientationIndexFilter(1.0, 1.0, 1.0, 1.0, 1.0, 1.0),
      CGAlgorithmsDD::STRAIGHT);
  // Coincident endpoints away from the query point trigger FAILURE.
  EXPECT_EQ(
      CGAlgorithmsDD::orientationIndexFilter(1.0, 1.0, 1.0, 1.0, 3.0, 4.0),
      CGAlgorithmsDD::FAILURE);

  // Catastrophic cancellation in double arithmetic rounds det to 0 so the
  // filter returns FAILURE and lets DoubleDouble resolve the orientation.
  const double a = std::ldexp(1.0, -27);
  const double b = std::ldexp(1.0, -28);
  EXPECT_EQ(CGAlgorithmsDD::orientationIndexFilter(1.0 + a, 1.0 + a + b, 1.0,
                                                   1.0 + b, 0.0, 0.0),
            CGAlgorithmsDD::FAILURE);

  // Non-zero double determinant smaller than the Ozaki error bound returns
  // FAILURE.
  EXPECT_EQ(CGAlgorithmsDD::orientationIndexFilter(1.0 + std::ldexp(1.0, -52),
                                                   1.0, 1.0, 1.0, 0.0, 0.0),
            CGAlgorithmsDD::FAILURE);

  // Large-coordinate subtraction rounding cancels det in double, returning
  // FAILURE.
  const double big = std::ldexp(1.0, 52);
  EXPECT_EQ(CGAlgorithmsDD::orientationIndexFilter(big, big, big + 1.0,
                                                   big + 1.0, 0.0, 0.25),
            CGAlgorithmsDD::FAILURE);
}

TEST(CGAlgorithmsDDTest, OrientationIndexStandardCases) {
  constexpr auto kCases = std::to_array<OrientationCase>({
      {.name = "left_above_horizontal",
       .p1x = 0.0,
       .p1y = 0.0,
       .p2x = 10.0,
       .p2y = 0.0,
       .qx = 5.0,
       .qy = 1.0,
       .expected = CGAlgorithmsDD::LEFT},
      {.name = "right_below_horizontal",
       .p1x = 0.0,
       .p1y = 0.0,
       .p2x = 10.0,
       .p2y = 0.0,
       .qx = 5.0,
       .qy = -1.0,
       .expected = CGAlgorithmsDD::RIGHT},
      {.name = "collinear_horizontal",
       .p1x = 0.0,
       .p1y = 0.0,
       .p2x = 10.0,
       .p2y = 0.0,
       .qx = 15.0,
       .qy = 0.0,
       .expected = CGAlgorithmsDD::STRAIGHT},
      {.name = "collinear_vertical",
       .p1x = 2.0,
       .p1y = -3.0,
       .p2x = 2.0,
       .p2y = 5.0,
       .qx = 2.0,
       .qy = 1.0,
       .expected = CGAlgorithmsDD::STRAIGHT},
      {.name = "collinear_diagonal_dd_fallback",
       .p1x = 0.0,
       .p1y = 0.0,
       .p2x = 1.0,
       .p2y = 1.0,
       .qx = 2.0,
       .qy = 2.0,
       .expected = CGAlgorithmsDD::STRAIGHT},
      {.name = "left_diagonal",
       .p1x = -2.0,
       .p1y = -2.0,
       .p2x = 3.0,
       .p2y = 3.0,
       .qx = 0.0,
       .qy = 1.0,
       .expected = CGAlgorithmsDD::LEFT},
      {.name = "right_diagonal",
       .p1x = -2.0,
       .p1y = -2.0,
       .p2x = 3.0,
       .p2y = 3.0,
       .qx = 1.0,
       .qy = 0.0,
       .expected = CGAlgorithmsDD::RIGHT},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(CGAlgorithmsDD::orientationIndex(tc.p1x, tc.p1y, tc.p2x, tc.p2y,
                                               tc.qx, tc.qy),
              tc.expected);
    EXPECT_EQ(CGAlgorithmsDD::orientationIndex(CoordinateXY(tc.p1x, tc.p1y),
                                               CoordinateXY(tc.p2x, tc.p2y),
                                               CoordinateXY(tc.qx, tc.qy)),
              tc.expected);
  }
}

TEST(CGAlgorithmsDDTest, OrientationIndexExtendedPrecisionCancellation) {
  // Exact determinant:
  // (1 + 2^-27) * (1 + 2^-28) - (1 + 2^-27 + 2^-28) * 1 = +2^-55.
  // Plain IEEE-754 double drops 2^-55 when q is at the origin, whereas
  // orientationIndexFilter returns FAILURE and DoubleDouble resolves it.
  const double a = std::ldexp(1.0, -27);
  const double b = std::ldexp(1.0, -28);
  const CoordinateXY p1(1.0 + a, 1.0 + a + b);
  const CoordinateXY p2(1.0, 1.0 + b);
  const CoordinateXY origin(0.0, 0.0);

  ASSERT_EQ(CGAlgorithmsDD::orientationIndexFilter(p1.x, p1.y, p2.x, p2.y,
                                                   origin.x, origin.y),
            CGAlgorithmsDD::FAILURE);
  EXPECT_EQ(CGAlgorithmsDD::orientationIndex(p1, p2, origin),
            CGAlgorithmsDD::LEFT);
  ASSERT_EQ(CGAlgorithmsDD::orientationIndexFilter(p2.x, p2.y, p1.x, p1.y,
                                                   origin.x, origin.y),
            CGAlgorithmsDD::FAILURE);
  EXPECT_EQ(CGAlgorithmsDD::orientationIndex(p2, p1, origin),
            CGAlgorithmsDD::RIGHT);

  // Shifted cancellation where DD products retain non-zero low-order bits.
  const CoordinateXY s2(1.0 + a, 1.0 + a + b);
  const CoordinateXY s3(2.0 + a, 2.0 + 2.0 * a);
  ASSERT_EQ(CGAlgorithmsDD::orientationIndexFilter(origin.x, origin.y, s2.x,
                                                   s2.y, s3.x, s3.y),
            CGAlgorithmsDD::FAILURE);
  EXPECT_EQ(CGAlgorithmsDD::orientationIndex(origin, s2, s3),
            CGAlgorithmsDD::LEFT);
  ASSERT_EQ(CGAlgorithmsDD::orientationIndexFilter(s2.x, s2.y, origin.x,
                                                   origin.y, s3.x, s3.y),
            CGAlgorithmsDD::FAILURE);
  EXPECT_EQ(CGAlgorithmsDD::orientationIndex(s2, origin, s3),
            CGAlgorithmsDD::RIGHT);

  // Filter fails with small non-zero det below the Ozaki error threshold.
  const CoordinateXY tiny_p1(1.0 + std::ldexp(1.0, -52), 1.0);
  ASSERT_EQ(CGAlgorithmsDD::orientationIndexFilter(tiny_p1.x, tiny_p1.y, 1.0,
                                                   1.0, 0.0, 0.0),
            CGAlgorithmsDD::FAILURE);
  EXPECT_EQ(
      CGAlgorithmsDD::orientationIndex(tiny_p1, CoordinateXY(1.0, 1.0), origin),
      CGAlgorithmsDD::LEFT);
  EXPECT_EQ(
      CGAlgorithmsDD::orientationIndex(CoordinateXY(1.0, 1.0), tiny_p1, origin),
      CGAlgorithmsDD::RIGHT);

  // Large coordinates where double subtraction loses sub-ULP offsets.
  const double big = std::ldexp(1.0, 52);
  const CoordinateXY big_seg1(big, big);
  const CoordinateXY big_seg2(big + 1.0, big + 1.0);
  const CoordinateXY q_left(0.0, 0.25);
  const CoordinateXY q_right(0.25, 0.0);
  ASSERT_EQ(
      CGAlgorithmsDD::orientationIndexFilter(big_seg1.x, big_seg1.y, big_seg2.x,
                                             big_seg2.y, q_left.x, q_left.y),
      CGAlgorithmsDD::FAILURE);
  EXPECT_EQ(CGAlgorithmsDD::orientationIndex(big_seg1, big_seg2, q_left),
            CGAlgorithmsDD::LEFT);
  ASSERT_EQ(
      CGAlgorithmsDD::orientationIndexFilter(big_seg1.x, big_seg1.y, big_seg2.x,
                                             big_seg2.y, q_right.x, q_right.y),
      CGAlgorithmsDD::FAILURE);
  EXPECT_EQ(CGAlgorithmsDD::orientationIndex(big_seg1, big_seg2, q_right),
            CGAlgorithmsDD::RIGHT);

  // Shifted large coordinates where the filter fails on collinear points.
  const CoordinateXY big_p1(1e12, 1e12);
  const CoordinateXY big_p2(1e12 + 1.0, 1e12 + 1.0);
  const CoordinateXY big_q(1e12 + 2.0, 1e12 + 2.0);
  EXPECT_EQ(CGAlgorithmsDD::orientationIndex(big_p1, big_p2, big_q),
            CGAlgorithmsDD::STRAIGHT);

  // Zero-length reference vector falls back to DD and resolves to STRAIGHT.
  EXPECT_EQ(CGAlgorithmsDD::orientationIndex(1.0, 1.0, 1.0, 1.0, 3.0, 4.0),
            CGAlgorithmsDD::STRAIGHT);
}

TEST(CGAlgorithmsDDTest, OrientationIndexThrowsOnNonFiniteQuery) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  EXPECT_THROW(CGAlgorithmsDD::orientationIndex(0.0, 0.0, 1.0, 1.0, nan, 0.0),
               IllegalArgumentException);
  EXPECT_THROW(CGAlgorithmsDD::orientationIndex(0.0, 0.0, 1.0, 1.0, 0.0, nan),
               IllegalArgumentException);
  EXPECT_THROW(CGAlgorithmsDD::orientationIndex(0.0, 0.0, 1.0, 1.0, inf, 0.0),
               IllegalArgumentException);
  EXPECT_THROW(CGAlgorithmsDD::orientationIndex(0.0, 0.0, 1.0, 1.0, 0.0, -inf),
               IllegalArgumentException);
  EXPECT_THROW(CGAlgorithmsDD::orientationIndex(CoordinateXY(0.0, 0.0),
                                                CoordinateXY(1.0, 1.0),
                                                CoordinateXY(nan, inf)),
               IllegalArgumentException);
}

class ExposedCGAlgorithmsDD : public CGAlgorithmsDD {
 public:
  using CGAlgorithmsDD::signOfDet2x2;
};

TEST(CGAlgorithmsDDTest, SignOfDet2x2) {
  EXPECT_EQ(CGAlgorithmsDD::signOfDet2x2(2.0, 1.0, 1.0, 3.0),
            CGAlgorithmsDD::LEFT);
  EXPECT_EQ(CGAlgorithmsDD::signOfDet2x2(1.0, 3.0, 2.0, 1.0),
            CGAlgorithmsDD::RIGHT);
  EXPECT_EQ(CGAlgorithmsDD::signOfDet2x2(2.0, 4.0, 3.0, 6.0),
            CGAlgorithmsDD::STRAIGHT);

  // Extended-precision cancellation in 2x2 determinant.
  const double a = std::ldexp(1.0, -27);
  const double b = std::ldexp(1.0, -28);
  EXPECT_EQ(CGAlgorithmsDD::signOfDet2x2(1.0 + a, 1.0 + a + b, 1.0, 1.0 + b),
            CGAlgorithmsDD::LEFT);
  EXPECT_EQ(CGAlgorithmsDD::signOfDet2x2(1.0, 1.0 + b, 1.0 + a, 1.0 + a + b),
            CGAlgorithmsDD::RIGHT);

  // Protected DD overload resolves signs determined solely by lo components.
  const DD one_plus_eps(1.0, std::ldexp(1.0, -60));
  const DD one(1.0, 0.0);
  EXPECT_EQ(ExposedCGAlgorithmsDD::signOfDet2x2(one_plus_eps, one, one, one),
            CGAlgorithmsDD::LEFT);
  EXPECT_EQ(ExposedCGAlgorithmsDD::signOfDet2x2(one, one_plus_eps, one, one),
            CGAlgorithmsDD::RIGHT);
  EXPECT_EQ(
      ExposedCGAlgorithmsDD::signOfDet2x2(one_plus_eps, one_plus_eps, one, one),
      CGAlgorithmsDD::STRAIGHT);
}

TEST(CGAlgorithmsDDTest, SignOfDet2x2ThrowsOnNonFinite) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  EXPECT_THROW(CGAlgorithmsDD::signOfDet2x2(nan, 1.0, 1.0, 1.0),
               IllegalArgumentException);
  EXPECT_THROW(CGAlgorithmsDD::signOfDet2x2(1.0, inf, 1.0, 1.0),
               IllegalArgumentException);
  EXPECT_THROW(CGAlgorithmsDD::signOfDet2x2(1.0, 1.0, -inf, 1.0),
               IllegalArgumentException);
  EXPECT_THROW(CGAlgorithmsDD::signOfDet2x2(1.0, 1.0, 1.0, nan),
               IllegalArgumentException);
}

TEST(CGAlgorithmsDDTest, DetDD) {
  EXPECT_EQ(CGAlgorithmsDD::detDD(3.0, 4.0, 2.0, 5.0), DD(7.0));
  EXPECT_EQ(CGAlgorithmsDD::detDD(2.0, 4.0, 3.0, 6.0), DD(0.0));

  const double a = std::ldexp(1.0, -27);
  const double b = std::ldexp(1.0, -28);
  const DD det = CGAlgorithmsDD::detDD(1.0 + a, 1.0 + a + b, 1.0, 1.0 + b);
  EXPECT_DOUBLE_EQ(det.ToDouble(), std::ldexp(1.0, -55));

  // Verify the 4-DD overload retains low-order terms in both hi and lo slots.
  const DD x1(1.0, std::ldexp(1.0, -55));
  const DD y1(1.0 + std::ldexp(1.0, -40), std::ldexp(1.0, -55));
  const DD x2(1.0, 0.0);
  const DD y2(1.0 + std::ldexp(1.0, -40), 0.0);
  const DD det_dd = CGAlgorithmsDD::detDD(x1, y1, x2, y2);
  EXPECT_DOUBLE_EQ(det_dd.ToDouble(), std::ldexp(1.0, -95));
}

TEST(CGAlgorithmsDDTest, IntersectionCrossingAndExtendedLines) {
  const CoordinateXY cross = CGAlgorithmsDD::intersection(
      CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 2.0), CoordinateXY(0.0, 2.0),
      CoordinateXY(2.0, 0.0));
  EXPECT_DOUBLE_EQ(cross.x, 1.0);
  EXPECT_DOUBLE_EQ(cross.y, 1.0);

  // Intersects infinite extensions even when finite segments are disjoint.
  const CoordinateXY extended = CGAlgorithmsDD::intersection(
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0), CoordinateXY(3.0, 1.0),
      CoordinateXY(3.0, 5.0));
  EXPECT_DOUBLE_EQ(extended.x, 3.0);
  EXPECT_DOUBLE_EQ(extended.y, 0.0);

  // Nearly parallel lines:
  // Line 1: y = 2^-20 * x
  // Line 2: y = 1 + (2^-20 - 2^-60) * x
  // Exact intersection: (2^60, 2^40).
  const double x_step = std::ldexp(1.0, 20);
  const CoordinateXY near_parallel = CGAlgorithmsDD::intersection(
      CoordinateXY(0.0, 0.0), CoordinateXY(x_step, 1.0), CoordinateXY(0.0, 1.0),
      CoordinateXY(x_step, 2.0 - std::ldexp(1.0, -40)));
  EXPECT_DOUBLE_EQ(near_parallel.x, std::ldexp(1.0, 60));
  EXPECT_DOUBLE_EQ(near_parallel.y, std::ldexp(1.0, 40));
}

TEST(CGAlgorithmsDDTest, IntersectionParallelCollinearAndOverflowReturnNull) {
  const CoordinateXY parallel = CGAlgorithmsDD::intersection(
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(0.0, 1.0),
      CoordinateXY(1.0, 2.0));
  EXPECT_TRUE(parallel.isNull());

  const CoordinateXY collinear = CGAlgorithmsDD::intersection(
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 2.0),
      CoordinateXY(3.0, 3.0));
  EXPECT_TRUE(collinear.isNull());

  const CoordinateXY degenerate = CGAlgorithmsDD::intersection(
      CoordinateXY(1.0, 1.0), CoordinateXY(1.0, 1.0), CoordinateXY(0.0, 0.0),
      CoordinateXY(2.0, 0.0));
  EXPECT_TRUE(degenerate.isNull());

  // Intersects at finite x = 1e20 while y overflows IEEE-754 double to +Inf.
  const CoordinateXY y_overflow = CGAlgorithmsDD::intersection(
      CoordinateXY(0.0, 1e200), CoordinateXY(1e-100, 2e200),
      CoordinateXY(1e20, 0.0), CoordinateXY(1e20, 1.0));
  EXPECT_TRUE(y_overflow.isNull());

  // Intersects at finite y = 1e20 while x overflows IEEE-754 double.
  const CoordinateXY x_overflow = CGAlgorithmsDD::intersection(
      CoordinateXY(1e200, 0.0), CoordinateXY(2e200, 1e-100),
      CoordinateXY(0.0, 1e20), CoordinateXY(1.0, 1e20));
  EXPECT_TRUE(x_overflow.isNull());
}

TEST(CGAlgorithmsDDTest, CircumcentreDD) {
  const CoordinateXY same = CGAlgorithmsDD::circumcentreDD(
      CoordinateXY(3.5, -2.5), CoordinateXY(3.5, -2.5),
      CoordinateXY(3.5, -2.5));
  EXPECT_DOUBLE_EQ(same.x, 3.5);
  EXPECT_DOUBLE_EQ(same.y, -2.5);

  // Right triangle: circumcentre is midpoint of hypotenuse.
  const CoordinateXY right_cc = CGAlgorithmsDD::circumcentreDD(
      CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0), CoordinateXY(0.0, 6.0));
  EXPECT_DOUBLE_EQ(right_cc.x, 2.0);
  EXPECT_DOUBLE_EQ(right_cc.y, 3.0);

  // Obtuse triangle: circumcentre lies outside the triangle.
  const CoordinateXY a(0.0, 0.0);
  const CoordinateXY b(4.0, 0.0);
  const CoordinateXY c(2.0, 1.0);
  const CoordinateXY obtuse_cc = CGAlgorithmsDD::circumcentreDD(a, b, c);
  EXPECT_DOUBLE_EQ(obtuse_cc.x, 2.0);
  EXPECT_DOUBLE_EQ(obtuse_cc.y, -1.5);
  EXPECT_DOUBLE_EQ(obtuse_cc.distance(a), 2.5);
  EXPECT_DOUBLE_EQ(obtuse_cc.distance(b), 2.5);
  EXPECT_DOUBLE_EQ(obtuse_cc.distance(c), 2.5);

  // Large-coordinate offset preserves accuracy via local DD centering.
  const double offset = 1e9;
  const CoordinateXY shifted_cc = CGAlgorithmsDD::circumcentreDD(
      CoordinateXY(offset, offset), CoordinateXY(offset + 4.0, offset),
      CoordinateXY(offset, offset + 6.0));
  EXPECT_DOUBLE_EQ(shifted_cc.x, offset + 2.0);
  EXPECT_DOUBLE_EQ(shifted_cc.y, offset + 3.0);

  // Nearly flat isosceles triangle where DD preserves exact circumcentre.
  const CoordinateXY flat_cc = CGAlgorithmsDD::circumcentreDD(
      CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0),
      CoordinateXY(1.0, std::ldexp(1.0, -25)));
  EXPECT_DOUBLE_EQ(flat_cc.x, 1.0);
  EXPECT_DOUBLE_EQ(flat_cc.y, std::ldexp(1.0, -26) - std::ldexp(1.0, 24));

  // Collinear and two-coincident-vertex degenerate inputs produce NaN.
  const CoordinateXY collinear_cc = CGAlgorithmsDD::circumcentreDD(
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 2.0));
  EXPECT_TRUE(std::isnan(collinear_cc.x));
  EXPECT_TRUE(std::isnan(collinear_cc.y));

  const CoordinateXY two_same_cc = CGAlgorithmsDD::circumcentreDD(
      CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 3.0));
  EXPECT_TRUE(std::isnan(two_same_cc.x));
  EXPECT_TRUE(std::isnan(two_same_cc.y));

  const CoordinateXY a_eq_c_cc = CGAlgorithmsDD::circumcentreDD(
      CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 3.0), CoordinateXY(0.0, 0.0));
  EXPECT_TRUE(std::isnan(a_eq_c_cc.x));
  EXPECT_TRUE(std::isnan(a_eq_c_cc.y));

  const CoordinateXY b_eq_c_cc = CGAlgorithmsDD::circumcentreDD(
      CoordinateXY(2.0, 3.0), CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 0.0));
  EXPECT_TRUE(std::isnan(b_eq_c_cc.x));
  EXPECT_TRUE(std::isnan(b_eq_c_cc.y));
}

}  // namespace
}  // namespace geos_google
