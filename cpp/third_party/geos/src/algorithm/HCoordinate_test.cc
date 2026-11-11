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

// Unit tests for third_party/geos/src/algorithm/HCoordinate.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for NotRepresentableException.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/HCoordinate.cpp

#include "third_party/geos/include/geos/algorithm/HCoordinate.h"

#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/NotRepresentableException.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::algorithm::HCoordinate;
using geos::algorithm::NotRepresentableException;
using geos::geom::Coordinate;

struct LinePairCase {
  std::string_view name;
  double p1x;
  double p1y;
  double p2x;
  double p2y;
  double q1x;
  double q1y;
  double q2x;
  double q2y;
  double expected_x;
  double expected_y;
};

TEST(HCoordinateTest, DefaultConstructorInitializesToOrigin) {
  const HCoordinate hc;
  EXPECT_DOUBLE_EQ(hc.x, 0.0);
  EXPECT_DOUBLE_EQ(hc.y, 0.0);
  EXPECT_DOUBLE_EQ(hc.w, 1.0);
  EXPECT_DOUBLE_EQ(hc.getX(), 0.0);
  EXPECT_DOUBLE_EQ(hc.getY(), 0.0);

  Coordinate coord(12.0, 34.0, 56.0);
  hc.getCoordinate(coord);
  EXPECT_DOUBLE_EQ(coord.x, 0.0);
  EXPECT_DOUBLE_EQ(coord.y, 0.0);
  EXPECT_TRUE(std::isnan(coord.z));
}

TEST(HCoordinateTest, ThreeDoubleConstructorAndProjectiveScaling) {
  const HCoordinate positive_w(3.0, -6.0, 2.0);
  EXPECT_DOUBLE_EQ(positive_w.x, 3.0);
  EXPECT_DOUBLE_EQ(positive_w.y, -6.0);
  EXPECT_DOUBLE_EQ(positive_w.w, 2.0);
  EXPECT_DOUBLE_EQ(positive_w.getX(), 1.5);
  EXPECT_DOUBLE_EQ(positive_w.getY(), -3.0);

  // Negating all homogeneous coordinates represents the same Euclidean point.
  const HCoordinate negative_w(-3.0, 6.0, -2.0);
  EXPECT_DOUBLE_EQ(negative_w.getX(), 1.5);
  EXPECT_DOUBLE_EQ(negative_w.getY(), -3.0);

  Coordinate c1;
  Coordinate c2;
  positive_w.getCoordinate(c1);
  negative_w.getCoordinate(c2);
  EXPECT_DOUBLE_EQ(c1.x, c2.x);
  EXPECT_DOUBLE_EQ(c1.y, c2.y);

  // A point at infinity (w == 0) is constructible, but cannot be dehomogenized.
  const HCoordinate ideal_point(1.0, 2.0, 0.0);
  EXPECT_DOUBLE_EQ(ideal_point.x, 1.0);
  EXPECT_DOUBLE_EQ(ideal_point.y, 2.0);
  EXPECT_DOUBLE_EQ(ideal_point.w, 0.0);
  EXPECT_THROW(ideal_point.getX(), NotRepresentableException);
  EXPECT_THROW(ideal_point.getY(), NotRepresentableException);
  Coordinate out;
  EXPECT_THROW(ideal_point.getCoordinate(out), NotRepresentableException);
}

TEST(HCoordinateTest, CoordinateConstructorSetsUnitWeight) {
  const Coordinate p(4.5, -7.25, 99.0);
  const HCoordinate hc(p);
  EXPECT_DOUBLE_EQ(hc.x, 4.5);
  EXPECT_DOUBLE_EQ(hc.y, -7.25);
  EXPECT_DOUBLE_EQ(hc.w, 1.0);
  EXPECT_DOUBLE_EQ(hc.getX(), 4.5);
  EXPECT_DOUBLE_EQ(hc.getY(), -7.25);
}

TEST(HCoordinateTest, TwoCoordinateConstructorBuildsHomogeneousLine) {
  const Coordinate p1(1.0, 2.0);
  const Coordinate p2(4.0, 6.0);
  const HCoordinate line(p1, p2);

  // Line equation: x * X + y * Y + w = 0
  EXPECT_DOUBLE_EQ(line.x, p1.y - p2.y);
  EXPECT_DOUBLE_EQ(line.y, p2.x - p1.x);
  EXPECT_DOUBLE_EQ(line.w, p1.x * p2.y - p2.x * p1.y);
  EXPECT_DOUBLE_EQ(line.x * p1.x + line.y * p1.y + line.w, 0.0);
  EXPECT_DOUBLE_EQ(line.x * p2.x + line.y * p2.y + line.w, 0.0);
}

TEST(HCoordinateTest, TwoHCoordinateConstructorComputesCrossProduct) {
  const HCoordinate a(1.0, 2.0, 3.0);
  const HCoordinate b(4.0, 5.0, 6.0);
  const HCoordinate cross(a, b);

  EXPECT_DOUBLE_EQ(cross.x, 2.0 * 6.0 - 5.0 * 3.0);
  EXPECT_DOUBLE_EQ(cross.y, 4.0 * 3.0 - 1.0 * 6.0);
  EXPECT_DOUBLE_EQ(cross.w, 1.0 * 5.0 - 4.0 * 2.0);
  EXPECT_DOUBLE_EQ(cross.getX(), 1.0);
  EXPECT_DOUBLE_EQ(cross.getY(), -2.0);

  // Intersecting two homogeneous lines yields their intersection point.
  const HCoordinate line1(Coordinate(0.0, 0.0), Coordinate(4.0, 4.0));
  const HCoordinate line2(Coordinate(0.0, 4.0), Coordinate(4.0, 0.0));
  const HCoordinate pt(line1, line2);
  EXPECT_DOUBLE_EQ(pt.getX(), 2.0);
  EXPECT_DOUBLE_EQ(pt.getY(), 2.0);

  // Joining two non-unit-weight points and intersecting with a second line.
  const HCoordinate hpt1(2.0, 0.0, 2.0);  // Euclidean (1, 0)
  const HCoordinate hpt2(0.0, 6.0, 3.0);  // Euclidean (0, 2)
  const HCoordinate joined_line(hpt1, hpt2);
  const HCoordinate diag_line(Coordinate(0.0, 0.0), Coordinate(3.0, 6.0));
  const HCoordinate inter(joined_line, diag_line);
  Coordinate out;
  inter.getCoordinate(out);
  EXPECT_DOUBLE_EQ(out.x, 0.5);
  EXPECT_DOUBLE_EQ(out.y, 1.0);
}

TEST(HCoordinateTest, FourCoordinateConstructorComputesIntersection) {
  const HCoordinate inter(Coordinate(0.0, 0.0), Coordinate(4.0, 2.0),
                          Coordinate(0.0, 2.0), Coordinate(4.0, 0.0));
  Coordinate out;
  inter.getCoordinate(out);
  EXPECT_DOUBLE_EQ(out.x, 2.0);
  EXPECT_DOUBLE_EQ(out.y, 1.0);
  EXPECT_TRUE(std::isnan(out.z));

  // Parallel lines produce w == 0 without throwing during construction.
  const HCoordinate parallel(Coordinate(0.0, 0.0), Coordinate(2.0, 0.0),
                             Coordinate(0.0, 1.0), Coordinate(2.0, 1.0));
  EXPECT_DOUBLE_EQ(parallel.w, 0.0);
  EXPECT_THROW(parallel.getX(), NotRepresentableException);
  EXPECT_THROW(parallel.getY(), NotRepresentableException);
  EXPECT_THROW(parallel.getCoordinate(out), NotRepresentableException);
}

TEST(HCoordinateTest, StaticIntersectionValidCases) {
  constexpr auto kCases = std::to_array<LinePairCase>({
      {
          .name = "orthogonal_cross",
          .p1x = 0.0,
          .p1y = 1.0,
          .p2x = 2.0,
          .p2y = 1.0,
          .q1x = 1.0,
          .q1y = 0.0,
          .q2x = 1.0,
          .q2y = 2.0,
          .expected_x = 1.0,
          .expected_y = 1.0,
      },
      {
          .name = "fractional_diagonal",
          .p1x = 0.0,
          .p1y = 0.0,
          .p2x = 3.0,
          .p2y = 3.0,
          .q1x = 0.0,
          .q1y = 1.0,
          .q2x = 3.0,
          .q2y = 0.0,
          .expected_x = 0.75,
          .expected_y = 0.75,
      },
      {
          .name = "intersection_outside_segments",
          .p1x = 0.0,
          .p1y = 0.0,
          .p2x = 1.0,
          .p2y = 0.0,
          .q1x = 3.0,
          .q1y = 1.0,
          .q2x = 3.0,
          .q2y = 4.0,
          .expected_x = 3.0,
          .expected_y = 0.0,
      },
      {
          .name = "shared_endpoint",
          .p1x = 1.0,
          .p1y = 2.0,
          .p2x = 5.0,
          .p2y = 4.0,
          .q1x = 1.0,
          .q1y = 2.0,
          .q2x = -2.0,
          .q2y = 7.0,
          .expected_x = 1.0,
          .expected_y = 2.0,
      },
      {
          .name = "negative_coordinates",
          .p1x = -10.0,
          .p1y = -4.0,
          .p2x = -2.0,
          .p2y = 4.0,
          .q1x = -10.0,
          .q1y = 4.0,
          .q2x = -2.0,
          .q2y = -4.0,
          .expected_x = -6.0,
          .expected_y = 0.0,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    Coordinate result(0.0, 0.0, 42.0);
    HCoordinate::intersection(
        Coordinate(tc.p1x, tc.p1y), Coordinate(tc.p2x, tc.p2y),
        Coordinate(tc.q1x, tc.q1y), Coordinate(tc.q2x, tc.q2y), result);
    EXPECT_DOUBLE_EQ(result.x, tc.expected_x);
    EXPECT_DOUBLE_EQ(result.y, tc.expected_y);
    EXPECT_TRUE(std::isnan(result.z));
  }
}

TEST(HCoordinateTest, StaticIntersectionThrowsNotRepresentable) {
  Coordinate result;

  // Parallel horizontal lines (w == 0, x != 0 => xInt is +-inf).
  EXPECT_THROW(HCoordinate::intersection(
                   Coordinate(0.0, 1.0), Coordinate(2.0, 1.0),
                   Coordinate(0.0, 2.0), Coordinate(2.0, 2.0), result),
               NotRepresentableException);

  // Parallel vertical lines (w == 0, x == 0, y != 0 => xInt is NaN).
  EXPECT_THROW(HCoordinate::intersection(
                   Coordinate(1.0, 0.0), Coordinate(1.0, 2.0),
                   Coordinate(2.0, 0.0), Coordinate(2.0, 2.0), result),
               NotRepresentableException);

  // Parallel diagonal lines.
  EXPECT_THROW(HCoordinate::intersection(
                   Coordinate(0.0, 0.0), Coordinate(2.0, 2.0),
                   Coordinate(1.0, 0.0), Coordinate(3.0, 2.0), result),
               NotRepresentableException);

  // Coincident lines (x == 0, y == 0, w == 0 => 0/0 = NaN).
  EXPECT_THROW(HCoordinate::intersection(
                   Coordinate(0.0, 0.0), Coordinate(3.0, 3.0),
                   Coordinate(1.0, 1.0), Coordinate(5.0, 5.0), result),
               NotRepresentableException);

  // Degenerate zero-length segment.
  EXPECT_THROW(HCoordinate::intersection(
                   Coordinate(2.0, 2.0), Coordinate(2.0, 2.0),
                   Coordinate(0.0, 0.0), Coordinate(4.0, 4.0), result),
               NotRepresentableException);

  // Finite xInt with overflowing non-finite yInt exercises the second operand
  // of `(!std::isfinite(xInt)) || (!std::isfinite(yInt))`.
  EXPECT_THROW(HCoordinate::intersection(
                   Coordinate(0.0, 0.0), Coordinate(1.0, 1e308),
                   Coordinate(3.0, 0.0), Coordinate(4.0, 1.0), result),
               NotRepresentableException);

  // NaN input coordinate.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(HCoordinate::intersection(
                   Coordinate(nan, 0.0), Coordinate(1.0, 1.0),
                   Coordinate(0.0, 1.0), Coordinate(1.0, 0.0), result),
               NotRepresentableException);
}

TEST(HCoordinateTest, GetXGetYAndGetCoordinateThrowOnNonFiniteQuotients) {
  Coordinate out;

  // x/w = inf, y/w = NaN.
  const HCoordinate inf_x(5.0, 0.0, 0.0);
  EXPECT_THROW(inf_x.getX(), NotRepresentableException);
  EXPECT_THROW(inf_x.getY(), NotRepresentableException);
  EXPECT_THROW(inf_x.getCoordinate(out), NotRepresentableException);

  // x/w = NaN, y/w = inf.
  const HCoordinate inf_y(0.0, 5.0, 0.0);
  EXPECT_THROW(inf_y.getX(), NotRepresentableException);
  EXPECT_THROW(inf_y.getY(), NotRepresentableException);
  EXPECT_THROW(inf_y.getCoordinate(out), NotRepresentableException);

  // Overflow only in x.
  const HCoordinate overflow_x(1e308, 1.0, 1e-10);
  EXPECT_THROW(overflow_x.getX(), NotRepresentableException);
  EXPECT_DOUBLE_EQ(overflow_x.getY(), 1e10);
  EXPECT_THROW(overflow_x.getCoordinate(out), NotRepresentableException);

  // Overflow only in y (getX succeeds inside getCoordinate, getY throws).
  const HCoordinate overflow_y(1.0, 1e308, 1e-10);
  EXPECT_DOUBLE_EQ(overflow_y.getX(), 1e10);
  EXPECT_THROW(overflow_y.getY(), NotRepresentableException);
  EXPECT_THROW(overflow_y.getCoordinate(out), NotRepresentableException);

  // Non-finite input ordinates or weights.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(HCoordinate(nan, 1.0, 1.0).getX(), NotRepresentableException);
  EXPECT_THROW(HCoordinate(1.0, nan, 1.0).getY(), NotRepresentableException);
  EXPECT_THROW(HCoordinate(1.0, 1.0, nan).getCoordinate(out),
               NotRepresentableException);

  // Exception derives from GEOSException and carries a non-empty message.
  try {
    (void)inf_x.getX();
    FAIL() << "Expected NotRepresentableException";
  } catch (const NotRepresentableException& e) {
    EXPECT_FALSE(std::string(e.what()).empty());
  }
}

TEST(HCoordinateTest, StreamInsertionFormatsComponents) {
  {
    std::ostringstream oss;
    oss << HCoordinate();
    EXPECT_EQ(oss.str(), "(0, 0) [w: 1]");
  }
  {
    std::ostringstream oss;
    oss << HCoordinate(1.5, -2.25, 0.5);
    EXPECT_EQ(oss.str(), "(1.5, -2.25) [w: 0.5]");
  }
}

}  // namespace
}  // namespace geos_google
