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

// Unit tests for third_party/geos/src/algorithm/CircularArcs.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/CircularArcs.cpp

#include "third_party/geos/include/geos/algorithm/CircularArcs.h"

#include <array>
#include <cmath>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"

namespace geos_google {
namespace {

using geos::algorithm::CircularArcs;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;

struct EnvelopeCase {
  std::string_view name;
  double p0x;
  double p0y;
  double p1x;
  double p1y;
  double p2x;
  double p2y;
  double minx;
  double maxx;
  double miny;
  double maxy;
  double tol;
};

TEST(CircularArcsTest, GetCenterGeneralAndClosedCircle) {
  // Unit circle centered at (0, 0).
  const CoordinateXY c1 = CircularArcs::getCenter(
      CoordinateXY(1.0, 0.0), CoordinateXY(0.0, 1.0), CoordinateXY(-1.0, 0.0));
  EXPECT_DOUBLE_EQ(c1.x, 0.0);
  EXPECT_DOUBLE_EQ(c1.y, 0.0);

  // Shifted circle of radius 5 centered at (3, -4).
  const CoordinateXY p0(8.0, -4.0);
  const CoordinateXY p1(6.0, 0.0);
  const CoordinateXY p2(0.0, -8.0);
  const CoordinateXY c2 = CircularArcs::getCenter(p0, p1, p2);
  EXPECT_NEAR(c2.x, 3.0, 1e-12);
  EXPECT_NEAR(c2.y, -4.0, 1e-12);
  EXPECT_NEAR(c2.distance(p0), 5.0, 1e-12);
  EXPECT_NEAR(c2.distance(p1), 5.0, 1e-12);
  EXPECT_NEAR(c2.distance(p2), 5.0, 1e-12);

  // Closed circle (p0 == p2, p1 antipodal): center is midpoint of p0 and p1.
  const CoordinateXY closed = CircularArcs::getCenter(
      CoordinateXY(2.0, 3.0), CoordinateXY(6.0, 7.0), CoordinateXY(2.0, 3.0));
  EXPECT_DOUBLE_EQ(closed.x, 4.0);
  EXPECT_DOUBLE_EQ(closed.y, 5.0);

  // Zero-length collapsed arc returns the repeated point.
  const CoordinateXY point_arc =
      CircularArcs::getCenter(CoordinateXY(1.5, -2.5), CoordinateXY(1.5, -2.5),
                              CoordinateXY(1.5, -2.5));
  EXPECT_DOUBLE_EQ(point_arc.x, 1.5);
  EXPECT_DOUBLE_EQ(point_arc.y, -2.5);
}

TEST(CircularArcsTest, GetCenterCollinearAndRepeatedEndpointReturnNaN) {
  const CoordinateXY collinear = CircularArcs::getCenter(
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 2.0));
  EXPECT_TRUE(std::isnan(collinear.x));
  EXPECT_TRUE(std::isnan(collinear.y));

  const CoordinateXY repeated_start = CircularArcs::getCenter(
      CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0));
  EXPECT_TRUE(std::isnan(repeated_start.x));
  EXPECT_TRUE(std::isnan(repeated_start.y));
}

TEST(CircularArcsTest, ExpandEnvelopeDegenerateAndCollinear) {
  Envelope zero_len;
  CircularArcs::expandEnvelope(zero_len, CoordinateXY(2.0, -3.0),
                               CoordinateXY(2.0, -3.0),
                               CoordinateXY(2.0, -3.0));
  EXPECT_DOUBLE_EQ(zero_len.getMinX(), 2.0);
  EXPECT_DOUBLE_EQ(zero_len.getMaxX(), 2.0);
  EXPECT_DOUBLE_EQ(zero_len.getMinY(), -3.0);
  EXPECT_DOUBLE_EQ(zero_len.getMaxY(), -3.0);

  Envelope collinear;
  CircularArcs::expandEnvelope(collinear, CoordinateXY(0.0, 0.0),
                               CoordinateXY(1.0, 2.0), CoordinateXY(2.0, 4.0));
  EXPECT_DOUBLE_EQ(collinear.getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(collinear.getMaxX(), 2.0);
  EXPECT_DOUBLE_EQ(collinear.getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(collinear.getMaxY(), 4.0);
}

TEST(CircularArcsTest, ExpandEnvelopeSameQuadrantMinorMajorAndClosedCircle) {
  constexpr auto kCases = std::to_array<EnvelopeCase>({
      // Radius 5 centered at (0,0), minor arc completely inside NE quadrant.
      {.name = "minor_arc_single_quadrant_ccw",
       .p0x = 5.0,
       .p0y = 0.0,
       .p1x = 4.0,
       .p1y = 3.0,
       .p2x = 3.0,
       .p2y = 4.0,
       .minx = 3.0,
       .maxx = 5.0,
       .miny = 0.0,
       .maxy = 4.0,
       .tol = 1e-12},
      {.name = "minor_arc_single_quadrant_cw",
       .p0x = 3.0,
       .p0y = 4.0,
       .p1x = 4.0,
       .p1y = 3.0,
       .p2x = 5.0,
       .p2y = 0.0,
       .minx = 3.0,
       .maxx = 5.0,
       .miny = 0.0,
       .maxy = 4.0,
       .tol = 1e-12},
      // Major arc starting and ending in NE quadrant wraps all 4 extrema.
      {.name = "major_arc_same_quadrant_ccw",
       .p0x = 3.0,
       .p0y = 4.0,
       .p1x = -5.0,
       .p1y = 0.0,
       .p2x = 4.0,
       .p2y = 3.0,
       .minx = -5.0,
       .maxx = 5.0,
       .miny = -5.0,
       .maxy = 5.0,
       .tol = 1e-12},
      {.name = "major_arc_same_quadrant_cw",
       .p0x = 4.0,
       .p0y = 3.0,
       .p1x = -5.0,
       .p1y = 0.0,
       .p2x = 3.0,
       .p2y = 4.0,
       .minx = -5.0,
       .maxx = 5.0,
       .miny = -5.0,
       .maxy = 5.0,
       .tol = 1e-12},
      // Closed circle centered at (2, 1) with radius 3.
      {.name = "closed_circle",
       .p0x = 5.0,
       .p0y = 1.0,
       .p1x = -1.0,
       .p1y = 1.0,
       .p2x = 5.0,
       .p2y = 1.0,
       .minx = -1.0,
       .maxx = 5.0,
       .miny = -2.0,
       .maxy = 4.0,
       .tol = 1e-12},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    Envelope env;
    CircularArcs::expandEnvelope(env, CoordinateXY(tc.p0x, tc.p0y),
                                 CoordinateXY(tc.p1x, tc.p1y),
                                 CoordinateXY(tc.p2x, tc.p2y));
    EXPECT_NEAR(env.getMinX(), tc.minx, tc.tol);
    EXPECT_NEAR(env.getMaxX(), tc.maxx, tc.tol);
    EXPECT_NEAR(env.getMinY(), tc.miny, tc.tol);
    EXPECT_NEAR(env.getMaxY(), tc.maxy, tc.tol);
  }
}

TEST(CircularArcsTest, ExpandEnvelopeCrossingAllFourAxesCcwAndCw) {
  // Radius 5 circle centered at (0, 0) using exact Pythagorean-triple points
  // that stay strictly interior to each quadrant so axis extrema govern bounds.
  constexpr auto kCases = std::to_array<EnvelopeCase>({
      // Crosses +Y axis (NE -> NW).
      {.name = "cross_north_axis_ccw",
       .p0x = 4.0,
       .p0y = 3.0,
       .p1x = 3.0,
       .p1y = 4.0,
       .p2x = -4.0,
       .p2y = 3.0,
       .minx = -4.0,
       .maxx = 4.0,
       .miny = 3.0,
       .maxy = 5.0,
       .tol = 1e-12},
      // Crosses -X axis (NW -> SW).
      {.name = "cross_west_axis_ccw",
       .p0x = -3.0,
       .p0y = 4.0,
       .p1x = -4.0,
       .p1y = 3.0,
       .p2x = -3.0,
       .p2y = -4.0,
       .minx = -5.0,
       .maxx = -3.0,
       .miny = -4.0,
       .maxy = 4.0,
       .tol = 1e-12},
      // Crosses -Y axis (SW -> SE).
      {.name = "cross_south_axis_ccw",
       .p0x = -4.0,
       .p0y = -3.0,
       .p1x = -3.0,
       .p1y = -4.0,
       .p2x = 4.0,
       .p2y = -3.0,
       .minx = -4.0,
       .maxx = 4.0,
       .miny = -5.0,
       .maxy = -3.0,
       .tol = 1e-12},
      // Crosses +X axis (SE -> NE).
      {.name = "cross_east_axis_ccw",
       .p0x = 3.0,
       .p0y = -4.0,
       .p1x = 4.0,
       .p1y = -3.0,
       .p2x = 3.0,
       .p2y = 4.0,
       .minx = 3.0,
       .maxx = 5.0,
       .miny = -4.0,
       .maxy = 4.0,
       .tol = 1e-12},
      // Clockwise multi-quadrant arc crossing +X, -Y, and -X axes (NE -> NW
      // CW).
      {.name = "three_axis_arc_cw",
       .p0x = 4.0,
       .p0y = 3.0,
       .p1x = 3.0,
       .p1y = -4.0,
       .p2x = -4.0,
       .p2y = 3.0,
       .minx = -5.0,
       .maxx = 5.0,
       .miny = -5.0,
       .maxy = 3.0,
       .tol = 1e-12},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    Envelope env;
    CircularArcs::expandEnvelope(env, CoordinateXY(tc.p0x, tc.p0y),
                                 CoordinateXY(tc.p1x, tc.p1y),
                                 CoordinateXY(tc.p2x, tc.p2y));
    EXPECT_NEAR(env.getMinX(), tc.minx, tc.tol);
    EXPECT_NEAR(env.getMaxX(), tc.maxx, tc.tol);
    EXPECT_NEAR(env.getMinY(), tc.miny, tc.tol);
    EXPECT_NEAR(env.getMaxY(), tc.maxy, tc.tol);
  }
}

TEST(CircularArcsTest, ExpandEnvelopePreservesExistingBounds) {
  Envelope env(10.0, 12.0, -1.0, 2.0);
  CircularArcs::expandEnvelope(env, CoordinateXY(4.0, 3.0),
                               CoordinateXY(3.0, 4.0), CoordinateXY(-4.0, 3.0));
  EXPECT_NEAR(env.getMinX(), -4.0, 1e-12);
  EXPECT_NEAR(env.getMaxX(), 12.0, 1e-12);
  EXPECT_NEAR(env.getMinY(), -1.0, 1e-12);
  EXPECT_NEAR(env.getMaxY(), 5.0, 1e-12);
}

}  // namespace
}  // namespace geos_google
