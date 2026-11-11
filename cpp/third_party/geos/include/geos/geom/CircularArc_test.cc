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

// Unit tests for third_party/geos/include/geos/geom/CircularArc.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/geom/CircularArc.h

#include "third_party/geos/include/geos/geom/CircularArc.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <numbers>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::DoubleInfinity;
using ::geos::DoubleNotANumber;
using ::geos::MATH_PI;
using ::geos::algorithm::Orientation;
using ::geos::geom::CircularArc;
using ::geos::geom::CoordinateXY;
using ::geos::util::IllegalArgumentException;

static_assert(std::is_same_v<CircularArc::CoordinateXY, CoordinateXY>);
static_assert(std::is_same_v<CircularArc::Iterator::iterator_category,
                             std::forward_iterator_tag>);
static_assert(
    std::is_same_v<CircularArc::Iterator::difference_type, std::ptrdiff_t>);
static_assert(std::is_same_v<CircularArc::Iterator::value_type, CoordinateXY>);
static_assert(
    std::is_same_v<CircularArc::Iterator::pointer, const CoordinateXY*>);
static_assert(
    std::is_same_v<CircularArc::Iterator::reference, const CoordinateXY&>);

struct AngleTestCase {
  std::string_view name;
  double p0x;
  double p0y;
  double p1x;
  double p1y;
  double p2x;
  double p2y;
  int expected_orientation;
  double expected_theta0;
  double expected_theta2;
  double expected_angle;
  double expected_radius;
  double tol;
};

struct ContainsAngleTestCase {
  std::string_view name;
  double p0x;
  double p0y;
  double p1x;
  double p1y;
  double p2x;
  double p2y;
  double theta;
  bool expected;
};

TEST(CircularArcTest, ConstructorStoresReferencesAndLazyAccessorsCache) {
  const CoordinateXY q0(8.0, -4.0);
  const CoordinateXY q1(6.0, 0.0);
  const CoordinateXY q2(0.0, -8.0);
  const CircularArc arc(q0, q1, q2);

  EXPECT_EQ(&arc.p0, &q0);
  EXPECT_EQ(&arc.p1, &q1);
  EXPECT_EQ(&arc.p2, &q2);

  // First call computes; second call exercises cached path.
  EXPECT_EQ(arc.orientation(), +Orientation::COUNTERCLOCKWISE);
  EXPECT_EQ(arc.orientation(), +Orientation::COUNTERCLOCKWISE);

  const CoordinateXY& center1 = arc.getCenter();
  const CoordinateXY& center2 = arc.getCenter();
  EXPECT_EQ(&center1, &center2);
  EXPECT_NEAR(center1.x, 3.0, 1e-12);
  EXPECT_NEAR(center1.y, -4.0, 1e-12);

  EXPECT_NEAR(arc.getRadius(), 5.0, 1e-12);
  EXPECT_NEAR(arc.getRadius(), 5.0, 1e-12);

  // Calling getRadius() before getCenter() lazily computes center too.
  const CircularArc arc_radius_first(q0, q1, q2);
  EXPECT_NEAR(arc_radius_first.getRadius(), 5.0, 1e-12);
  EXPECT_NEAR(arc_radius_first.getCenter().x, 3.0, 1e-12);
  EXPECT_NEAR(arc_radius_first.getCenter().y, -4.0, 1e-12);
}

TEST(CircularArcTest, OrientationDetectsCcwCwCollinearAndThrowsOnNonFinite) {
  {
    const CoordinateXY p0(1.0, 0.0);
    const CoordinateXY p1(0.0, 1.0);
    const CoordinateXY p2(-1.0, 0.0);
    const CircularArc ccw(p0, p1, p2);
    EXPECT_EQ(ccw.orientation(), +Orientation::COUNTERCLOCKWISE);
  }
  {
    const CoordinateXY p0(-1.0, 0.0);
    const CoordinateXY p1(0.0, 1.0);
    const CoordinateXY p2(1.0, 0.0);
    const CircularArc cw(p0, p1, p2);
    EXPECT_EQ(cw.orientation(), +Orientation::CLOCKWISE);
  }
  {
    const CoordinateXY p0(0.0, 0.0);
    const CoordinateXY p1(2.0, 4.0);
    const CoordinateXY p2(4.0, 8.0);
    const CircularArc collinear(p0, p1, p2);
    EXPECT_EQ(collinear.orientation(), +Orientation::COLLINEAR);
  }
  {
    const CoordinateXY p0(1.0, 0.0);
    const CoordinateXY p1(-1.0, 0.0);
    const CoordinateXY p2(1.0, 0.0);
    const CircularArc closed(p0, p1, p2);
    EXPECT_EQ(closed.orientation(), +Orientation::COLLINEAR);
  }
  {
    const CoordinateXY p0(0.0, 0.0);
    const CoordinateXY p1(1.0, 0.0);
    const CoordinateXY p2_nan(DoubleNotANumber, 0.0);
    const CircularArc nan_arc(p0, p1, p2_nan);
    EXPECT_THROW(nan_arc.orientation(), IllegalArgumentException);

    const CoordinateXY p2_inf(DoubleInfinity, 0.0);
    const CircularArc inf_arc(p0, p1, p2_inf);
    EXPECT_THROW(inf_arc.orientation(), IllegalArgumentException);
  }
}

TEST(CircularArcTest, CircleAndLinearClassification) {
  // General partial arc.
  {
    const CoordinateXY p0(5.0, 0.0);
    const CoordinateXY p1(4.0, 3.0);
    const CoordinateXY p2(3.0, 4.0);
    const CircularArc arc(p0, p1, p2);
    EXPECT_FALSE(arc.isCircle());
    EXPECT_FALSE(arc.isLinear());
    EXPECT_NEAR(arc.getCenter().x, 0.0, 1e-12);
    EXPECT_NEAR(arc.getCenter().y, 0.0, 1e-12);
    EXPECT_NEAR(arc.getRadius(), 5.0, 1e-12);
  }

  // Closed circle (p0 == p2, antipodal p1).
  {
    const CoordinateXY p0(5.0, 1.0);
    const CoordinateXY p1(-1.0, 1.0);
    const CoordinateXY p2(5.0, 1.0);
    const CircularArc circle(p0, p1, p2);
    EXPECT_TRUE(circle.isCircle());
    EXPECT_FALSE(circle.isLinear());
    EXPECT_DOUBLE_EQ(circle.getCenter().x, 2.0);
    EXPECT_DOUBLE_EQ(circle.getCenter().y, 1.0);
    EXPECT_DOUBLE_EQ(circle.getRadius(), 3.0);
  }

  // Degenerate zero-length single point.
  {
    const CoordinateXY p(2.0, -3.0);
    const CircularArc single_point(p, p, p);
    EXPECT_TRUE(single_point.isCircle());
    EXPECT_FALSE(single_point.isLinear());
    EXPECT_DOUBLE_EQ(single_point.getCenter().x, 2.0);
    EXPECT_DOUBLE_EQ(single_point.getCenter().y, -3.0);
    EXPECT_DOUBLE_EQ(single_point.getRadius(), 0.0);
  }

  // Collinear straight segment.
  {
    const CoordinateXY p0(0.0, 0.0);
    const CoordinateXY p1(1.0, 2.0);
    const CoordinateXY p2(3.0, 6.0);
    const CircularArc line(p0, p1, p2);
    EXPECT_FALSE(line.isCircle());
    EXPECT_TRUE(line.isLinear());
    EXPECT_TRUE(std::isnan(line.getCenter().x));
    EXPECT_TRUE(std::isnan(line.getCenter().y));
    EXPECT_TRUE(std::isnan(line.getRadius()));
  }

  // Repeated midpoint/endpoint (p0 == p1 != p2) is linear.
  {
    const CoordinateXY p0(1.0, 1.0);
    const CoordinateXY p1(1.0, 1.0);
    const CoordinateXY p2(4.0, 5.0);
    const CircularArc repeated(p0, p1, p2);
    EXPECT_FALSE(repeated.isCircle());
    EXPECT_TRUE(repeated.isLinear());
    EXPECT_TRUE(std::isnan(repeated.getRadius()));
  }
}

TEST(CircularArcTest, ThetaAndGetAngleCoverAllOrientationsAndWrapAround) {
  constexpr double kPi = MATH_PI;
  const double kPythHalf = 2.0 * std::atan2(4.0, 3.0);
  const auto kCases = std::to_array<AngleTestCase>({
      {
          .name = "ccw_quarter_no_wrap",
          .p0x = 2.0,
          .p0y = 0.0,
          .p1x = std::numbers::sqrt2,
          .p1y = std::numbers::sqrt2,
          .p2x = 0.0,
          .p2y = 2.0,
          .expected_orientation = +Orientation::COUNTERCLOCKWISE,
          .expected_theta0 = 0.0,
          .expected_theta2 = kPi / 2.0,
          .expected_angle = kPi / 2.0,
          .expected_radius = 2.0,
          .tol = 1e-12,
      },
      {
          .name = "cw_quarter_no_wrap",
          .p0x = 0.0,
          .p0y = 2.0,
          .p1x = std::numbers::sqrt2,
          .p1y = std::numbers::sqrt2,
          .p2x = 2.0,
          .p2y = 0.0,
          .expected_orientation = +Orientation::CLOCKWISE,
          .expected_theta0 = kPi / 2.0,
          .expected_theta2 = 0.0,
          .expected_angle = kPi / 2.0,
          .expected_radius = 2.0,
          .tol = 1e-12,
      },
      {
          .name = "ccw_cross_pi_branch_cut",
          .p0x = -3.0,
          .p0y = 4.0,
          .p1x = -5.0,
          .p1y = 0.0,
          .p2x = -3.0,
          .p2y = -4.0,
          .expected_orientation = +Orientation::COUNTERCLOCKWISE,
          .expected_theta0 = std::atan2(4.0, -3.0),
          .expected_theta2 = std::atan2(-4.0, -3.0),
          .expected_angle = kPythHalf,
          .expected_radius = 5.0,
          .tol = 1e-12,
      },
      {
          .name = "cw_cross_pi_branch_cut",
          .p0x = -3.0,
          .p0y = -4.0,
          .p1x = -5.0,
          .p1y = 0.0,
          .p2x = -3.0,
          .p2y = 4.0,
          .expected_orientation = +Orientation::CLOCKWISE,
          .expected_theta0 = std::atan2(-4.0, -3.0),
          .expected_theta2 = std::atan2(4.0, -3.0),
          .expected_angle = kPythHalf,
          .expected_radius = 5.0,
          .tol = 1e-12,
      },
      {
          .name = "ccw_major_three_quarters",
          .p0x = 0.0,
          .p0y = 3.0,
          .p1x = -3.0,
          .p1y = 0.0,
          .p2x = 3.0,
          .p2y = 0.0,
          .expected_orientation = +Orientation::COUNTERCLOCKWISE,
          .expected_theta0 = kPi / 2.0,
          .expected_theta2 = 0.0,
          .expected_angle = 1.5 * kPi,
          .expected_radius = 3.0,
          .tol = 1e-12,
      },
      {
          .name = "cw_major_three_quarters",
          .p0x = 3.0,
          .p0y = 0.0,
          .p1x = -3.0,
          .p1y = 0.0,
          .p2x = 0.0,
          .p2y = 3.0,
          .expected_orientation = +Orientation::CLOCKWISE,
          .expected_theta0 = 0.0,
          .expected_theta2 = kPi / 2.0,
          .expected_angle = 1.5 * kPi,
          .expected_radius = 3.0,
          .tol = 1e-12,
      },
      {
          .name = "ccw_semicircle_shifted_center",
          .p0x = 15.0,
          .p0y = -20.0,
          .p1x = 10.0,
          .p1y = -15.0,
          .p2x = 5.0,
          .p2y = -20.0,
          .expected_orientation = +Orientation::COUNTERCLOCKWISE,
          .expected_theta0 = 0.0,
          .expected_theta2 = kPi,
          .expected_angle = kPi,
          .expected_radius = 5.0,
          .tol = 1e-12,
      },
      {
          .name = "closed_circle",
          .p0x = 4.0,
          .p0y = 1.0,
          .p1x = -2.0,
          .p1y = 1.0,
          .p2x = 4.0,
          .p2y = 1.0,
          .expected_orientation = +Orientation::COLLINEAR,
          .expected_theta0 = 0.0,
          .expected_theta2 = 0.0,
          .expected_angle = 2.0 * kPi,
          .expected_radius = 3.0,
          .tol = 1e-12,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY p0(tc.p0x, tc.p0y);
    const CoordinateXY p1(tc.p1x, tc.p1y);
    const CoordinateXY p2(tc.p2x, tc.p2y);
    const CircularArc arc(p0, p1, p2);
    EXPECT_EQ(arc.orientation(), tc.expected_orientation);
    EXPECT_NEAR(arc.theta0(), tc.expected_theta0, tc.tol);
    EXPECT_NEAR(arc.theta2(), tc.expected_theta2, tc.tol);
    EXPECT_NEAR(arc.getAngle(), tc.expected_angle, tc.tol);
    EXPECT_NEAR(arc.getRadius(), tc.expected_radius, tc.tol);
  }

  // Linear arc produces NaN angles.
  const CoordinateXY l0(0.0, 0.0);
  const CoordinateXY l1(1.0, 1.0);
  const CoordinateXY l2(2.0, 2.0);
  const CircularArc linear(l0, l1, l2);
  EXPECT_TRUE(std::isnan(linear.theta0()));
  EXPECT_TRUE(std::isnan(linear.theta2()));
  EXPECT_TRUE(std::isnan(linear.getAngle()));
}

TEST(CircularArcTest, LengthAndAreaForLinearPartialClosedAndZeroLengthArcs) {
  // Linear collinear arc: chord length, zero enclosed area.
  {
    const CoordinateXY p0(1.0, 2.0);
    const CoordinateXY p1(4.0, 6.0);
    const CoordinateXY p2(7.0, 10.0);
    const CircularArc linear(p0, p1, p2);
    EXPECT_DOUBLE_EQ(linear.getLength(), 10.0);
    EXPECT_DOUBLE_EQ(linear.getArea(), 0.0);
  }

  // Quarter circle, radius 2.
  {
    const CoordinateXY p0(2.0, 0.0);
    const CoordinateXY p1(std::numbers::sqrt2, std::numbers::sqrt2);
    const CoordinateXY p2(0.0, 2.0);
    const CircularArc quarter(p0, p1, p2);
    EXPECT_NEAR(quarter.getLength(), MATH_PI, 1e-12);
    EXPECT_NEAR(quarter.getArea(), 2.0 * (MATH_PI / 2.0 - 1.0), 1e-12);
  }

  // Semicircle, radius 5.
  {
    const CoordinateXY p0(-5.0, 0.0);
    const CoordinateXY p1(0.0, -5.0);
    const CoordinateXY p2(5.0, 0.0);
    const CircularArc semi(p0, p1, p2);
    EXPECT_NEAR(semi.getLength(), 5.0 * MATH_PI, 1e-12);
    EXPECT_NEAR(semi.getArea(), 0.5 * 25.0 * MATH_PI, 1e-12);
  }

  // Major 3/4 circle, radius 1.
  {
    const CoordinateXY p0(0.0, 1.0);
    const CoordinateXY p1(-1.0, 0.0);
    const CoordinateXY p2(1.0, 0.0);
    const CircularArc major(p0, p1, p2);
    EXPECT_NEAR(major.getLength(), 1.5 * MATH_PI, 1e-12);
    EXPECT_NEAR(major.getArea(), 0.5 * (1.5 * MATH_PI + 1.0), 1e-12);
  }

  // Closed circle, radius 3.
  {
    const CoordinateXY p0(3.0, 0.0);
    const CoordinateXY p1(-3.0, 0.0);
    const CoordinateXY p2(3.0, 0.0);
    const CircularArc circle(p0, p1, p2);
    EXPECT_NEAR(circle.getLength(), 6.0 * MATH_PI, 1e-12);
    EXPECT_NEAR(circle.getArea(), 9.0 * MATH_PI, 1e-12);
  }

  // Collapsed zero-radius point.
  {
    const CoordinateXY p(4.0, -2.0);
    const CircularArc point(p, p, p);
    EXPECT_DOUBLE_EQ(point.getLength(), 0.0);
    EXPECT_DOUBLE_EQ(point.getArea(), 0.0);
  }
}

TEST(CircularArcTest, ContainsAngleAndContainsPointOnCircle) {
  constexpr double kPi = MATH_PI;
  const auto kCases = std::to_array<ContainsAngleTestCase>({
      // CCW minor arc from 0 to atan2(4, 3) (~0.9273 rad), t2 < 0 branch true.
      {
          .name = "ccw_minor_at_theta0",
          .p0x = 5.0,
          .p0y = 0.0,
          .p1x = 4.0,
          .p1y = 3.0,
          .p2x = 3.0,
          .p2y = 4.0,
          .theta = 0.0,
          .expected = true,
      },
      {
          .name = "ccw_minor_at_theta2",
          .p0x = 5.0,
          .p0y = 0.0,
          .p1x = 4.0,
          .p1y = 3.0,
          .p2x = 3.0,
          .p2y = 4.0,
          .theta = std::atan2(4.0, 3.0),
          .expected = true,
      },
      {
          .name = "ccw_minor_interior",
          .p0x = 5.0,
          .p0y = 0.0,
          .p1x = 4.0,
          .p1y = 3.0,
          .p2x = 3.0,
          .p2y = 4.0,
          .theta = 0.5,
          .expected = true,
      },
      {
          .name = "ccw_minor_outside_positive",
          .p0x = 5.0,
          .p0y = 0.0,
          .p1x = 4.0,
          .p1y = 3.0,
          .p2x = 3.0,
          .p2y = 4.0,
          .theta = 2.0,
          .expected = false,
      },
      {
          .name = "ccw_minor_outside_negative",
          .p0x = 5.0,
          .p0y = 0.0,
          .p1x = 4.0,
          .p1y = 3.0,
          .p2x = 3.0,
          .p2y = 4.0,
          .theta = -0.5,
          .expected = false,
      },
      // CCW arc crossing +-pi branch cut, t2 < 0 branch false.
      {
          .name = "ccw_wrap_on_arc_pos",
          .p0x = -3.0,
          .p0y = 4.0,
          .p1x = -5.0,
          .p1y = 0.0,
          .p2x = -3.0,
          .p2y = -4.0,
          .theta = 0.9 * kPi,
          .expected = true,
      },
      {
          .name = "ccw_wrap_on_arc_neg",
          .p0x = -3.0,
          .p0y = 4.0,
          .p1x = -5.0,
          .p1y = 0.0,
          .p2x = -3.0,
          .p2y = -4.0,
          .theta = -0.9 * kPi,
          .expected = true,
      },
      {
          .name = "ccw_wrap_outside",
          .p0x = -3.0,
          .p0y = 4.0,
          .p1x = -5.0,
          .p1y = 0.0,
          .p2x = -3.0,
          .p2y = -4.0,
          .theta = 0.0,
          .expected = false,
      },
      // CW minor arc from atan2(4, 3) to 0, t2 < 0 branch true.
      {
          .name = "cw_minor_interior",
          .p0x = 3.0,
          .p0y = 4.0,
          .p1x = 4.0,
          .p1y = 3.0,
          .p2x = 5.0,
          .p2y = 0.0,
          .theta = 0.5,
          .expected = true,
      },
      {
          .name = "cw_minor_outside_positive",
          .p0x = 3.0,
          .p0y = 4.0,
          .p1x = 4.0,
          .p1y = 3.0,
          .p2x = 5.0,
          .p2y = 0.0,
          .theta = 1.5,
          .expected = false,
      },
      {
          .name = "cw_minor_outside_negative",
          .p0x = 3.0,
          .p0y = 4.0,
          .p1x = 4.0,
          .p1y = 3.0,
          .p2x = 5.0,
          .p2y = 0.0,
          .theta = -0.5,
          .expected = false,
      },
      // CW arc crossing +-pi branch cut, t2 < 0 branch false.
      {
          .name = "cw_wrap_on_arc_neg",
          .p0x = -3.0,
          .p0y = -4.0,
          .p1x = -5.0,
          .p1y = 0.0,
          .p2x = -3.0,
          .p2y = 4.0,
          .theta = -0.9 * kPi,
          .expected = true,
      },
      {
          .name = "cw_wrap_on_arc_pos",
          .p0x = -3.0,
          .p0y = -4.0,
          .p1x = -5.0,
          .p1y = 0.0,
          .p2x = -3.0,
          .p2y = 4.0,
          .theta = 0.9 * kPi,
          .expected = true,
      },
      {
          .name = "cw_wrap_outside",
          .p0x = -3.0,
          .p0y = -4.0,
          .p1x = -5.0,
          .p1y = 0.0,
          .p2x = -3.0,
          .p2y = 4.0,
          .theta = 0.0,
          .expected = false,
      },
      // Closed circle contains every finite angle.
      {
          .name = "closed_circle_any_angle",
          .p0x = 5.0,
          .p0y = 0.0,
          .p1x = -5.0,
          .p1y = 0.0,
          .p2x = 5.0,
          .p2y = 0.0,
          .theta = -1.23,
          .expected = true,
      },
      // NaN angle returns false.
      {
          .name = "nan_angle_false",
          .p0x = 5.0,
          .p0y = 0.0,
          .p1x = 4.0,
          .p1y = 3.0,
          .p2x = 3.0,
          .p2y = 4.0,
          .theta = DoubleNotANumber,
          .expected = false,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY p0(tc.p0x, tc.p0y);
    const CoordinateXY p1(tc.p1x, tc.p1y);
    const CoordinateXY p2(tc.p2x, tc.p2y);
    const CircularArc arc(p0, p1, p2);
    EXPECT_EQ(arc.containsAngle(tc.theta), tc.expected);
  }

  // Shifted center containsPointOnCircle test.
  const CoordinateXY s0(15.0, -5.0);
  const CoordinateXY s1(10.0, 0.0);
  const CoordinateXY s2(5.0, -5.0);
  const CircularArc shifted(s0, s1, s2);
  EXPECT_TRUE(shifted.containsPointOnCircle(CoordinateXY(13.0, -1.0)));
  EXPECT_TRUE(shifted.containsPointOnCircle(CoordinateXY(7.0, -1.0)));
  EXPECT_FALSE(shifted.containsPointOnCircle(CoordinateXY(13.0, -9.0)));
  EXPECT_FALSE(shifted.containsPointOnCircle(CoordinateXY(10.0, -10.0)));
}

TEST(CircularArcTest,
     ContainsPointChecksEndpointsDistanceCirclePredicateAndArc) {
  // CCW upper semicircle centered at (0, 0) with radius 5.
  const CoordinateXY p0(5.0, 0.0);
  const CoordinateXY p1(0.0, 5.0);
  const CoordinateXY p2(-5.0, 0.0);
  CircularArc ccw_arc(p0, p1, p2);

  // Exact defining vertices.
  EXPECT_TRUE(ccw_arc.containsPoint(p0));
  EXPECT_TRUE(ccw_arc.containsPoint(p1));
  EXPECT_TRUE(ccw_arc.containsPoint(p2));

  // Exact integer Pythagorean points on the upper arc vs lower complement.
  EXPECT_TRUE(ccw_arc.containsPoint(CoordinateXY(4.0, 3.0)));
  EXPECT_TRUE(ccw_arc.containsPoint(CoordinateXY(3.0, 4.0)));
  EXPECT_TRUE(ccw_arc.containsPoint(CoordinateXY(-3.0, 4.0)));
  EXPECT_TRUE(ccw_arc.containsPoint(CoordinateXY(-4.0, 3.0)));
  EXPECT_FALSE(ccw_arc.containsPoint(CoordinateXY(3.0, -4.0)));
  EXPECT_FALSE(ccw_arc.containsPoint(CoordinateXY(-3.0, -4.0)));
  EXPECT_FALSE(ccw_arc.containsPoint(CoordinateXY(0.0, -5.0)));

  // Far from circle (fails dist > 1e-8).
  EXPECT_FALSE(ccw_arc.containsPoint(CoordinateXY(0.0, 0.0)));
  EXPECT_FALSE(ccw_arc.containsPoint(CoordinateXY(4.0, 4.0)));
  EXPECT_FALSE(ccw_arc.containsPoint(CoordinateXY(3.0, 4.0 + 1e-6)));

  // Within 1e-8 radial tolerance, but off circle in exact TrianglePredicate.
  EXPECT_FALSE(ccw_arc.containsPoint(CoordinateXY(3.0, 4.0 + 1e-10)));
  EXPECT_FALSE(ccw_arc.containsPoint(CoordinateXY(3.0, 4.0 - 1e-10)));

  // Clockwise arc with shifted center (10, -20) and radius 5.
  const CoordinateXY cp0(15.0, -20.0);
  const CoordinateXY cp1(10.0, -25.0);
  const CoordinateXY cp2(5.0, -20.0);
  CircularArc cw_arc(cp0, cp1, cp2);
  EXPECT_TRUE(cw_arc.containsPoint(CoordinateXY(13.0, -24.0)));
  EXPECT_TRUE(cw_arc.containsPoint(CoordinateXY(7.0, -24.0)));
  EXPECT_FALSE(cw_arc.containsPoint(CoordinateXY(13.0, -16.0)));
  EXPECT_FALSE(cw_arc.containsPoint(CoordinateXY(13.0, -24.0 + 1e-10)));

  // Closed circle contains points anywhere on its circumference.
  const CoordinateXY fp0(5.0, 0.0);
  const CoordinateXY fp1(-5.0, 0.0);
  const CoordinateXY fp2(5.0, 0.0);
  CircularArc full_circle(fp0, fp1, fp2);
  EXPECT_TRUE(full_circle.containsPoint(CoordinateXY(3.0, 4.0)));
  EXPECT_TRUE(full_circle.containsPoint(CoordinateXY(3.0, -4.0)));
  EXPECT_FALSE(full_circle.containsPoint(CoordinateXY(3.0, 4.0 + 1e-6)));

  // Collinear arc still returns true for its three defining vertices.
  const CoordinateXY l0(0.0, 0.0);
  const CoordinateXY l1(1.0, 1.0);
  const CoordinateXY l2(2.0, 2.0);
  CircularArc linear(l0, l1, l2);
  EXPECT_TRUE(linear.containsPoint(l0));
  EXPECT_TRUE(linear.containsPoint(l1));
  EXPECT_TRUE(linear.containsPoint(l2));
}

TEST(CircularArcTest,
     IsUpwardAtPointCoversClockwiseAndCounterclockwiseQuadrants) {
  // Circle centered at (10, -5) with radius 5.
  // CW major arc spanning SW, NW, NE, SE quadrants: (14, -2) -> (10, -10) ->
  // (13, -1).
  {
    const CoordinateXY p0(14.0, -2.0);
    const CoordinateXY p1(10.0, -10.0);
    const CoordinateXY p2(13.0, -1.0);
    const CircularArc cw(p0, p1, p2);
    ASSERT_EQ(cw.orientation(), +Orientation::CLOCKWISE);
    EXPECT_TRUE(cw.isUpwardAtPoint(CoordinateXY(7.0, -9.0)));    // SW
    EXPECT_TRUE(cw.isUpwardAtPoint(CoordinateXY(7.0, -1.0)));    // NW
    EXPECT_FALSE(cw.isUpwardAtPoint(CoordinateXY(13.0, -1.0)));  // NE
    EXPECT_FALSE(cw.isUpwardAtPoint(CoordinateXY(13.0, -9.0)));  // SE
  }

  // CCW major arc spanning SE, NE, NW, SW quadrants: (13, -1) -> (10, -10) ->
  // (14, -2).
  {
    const CoordinateXY p0(13.0, -1.0);
    const CoordinateXY p1(10.0, -10.0);
    const CoordinateXY p2(14.0, -2.0);
    const CircularArc ccw(p0, p1, p2);
    ASSERT_EQ(ccw.orientation(), +Orientation::COUNTERCLOCKWISE);
    EXPECT_TRUE(ccw.isUpwardAtPoint(CoordinateXY(13.0, -9.0)));  // SE
    EXPECT_TRUE(ccw.isUpwardAtPoint(CoordinateXY(13.0, -1.0)));  // NE
    EXPECT_FALSE(ccw.isUpwardAtPoint(CoordinateXY(7.0, -1.0)));  // NW
    EXPECT_FALSE(ccw.isUpwardAtPoint(CoordinateXY(7.0, -9.0)));  // SW
  }

  // Degenerate arc where q equals center throws IllegalArgumentException.
  {
    const CoordinateXY c(1.0, 2.0);
    const CircularArc collapsed(c, c, c);
    EXPECT_THROW(collapsed.isUpwardAtPoint(c), IllegalArgumentException);
  }
}

TEST(CircularArcTest, IteratorTraversesThreeVerticesInOrder) {
  const CoordinateXY p0(1.0, 2.0);
  const CoordinateXY p1(3.0, 4.0);
  const CoordinateXY p2(5.0, 2.0);
  const CircularArc arc(p0, p1, p2);

  EXPECT_EQ(std::distance(arc.begin(), arc.end()), 3);

  std::vector<const CoordinateXY*> ptrs;
  for (const CoordinateXY& pt : arc) {
    ptrs.push_back(&pt);
  }
  ASSERT_EQ(ptrs.size(), 3u);
  EXPECT_EQ(ptrs[0], &p0);
  EXPECT_EQ(ptrs[1], &p1);
  EXPECT_EQ(ptrs[2], &p2);

  CircularArc::Iterator it = arc.begin();
  EXPECT_TRUE(it == arc.begin());
  EXPECT_TRUE(it != arc.end());
  EXPECT_EQ(&(*it), &p0);

  CircularArc::Iterator post = it++;
  EXPECT_EQ(&(*post), &p0);
  EXPECT_EQ(&(*it), &p1);
  EXPECT_TRUE(post != it);

  CircularArc::Iterator& pre_ref = ++it;
  EXPECT_EQ(&pre_ref, &it);
  EXPECT_EQ(&(*it), &p2);

  ++it;
  EXPECT_TRUE(it == arc.end());
  EXPECT_FALSE(it != arc.end());
}

}  // namespace
}  // namespace geos_google
