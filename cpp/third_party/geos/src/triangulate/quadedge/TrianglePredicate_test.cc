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

// Unit tests for
// third_party/geos/src/triangulate/quadedge/TrianglePredicate.cpp.
//
// Notes:
// - `TrianglePredicate::triArea` is private with no friends or internal
// callers,
//   so only the three public `isInCircle*` predicates are reachable here.
// - In upstream `TrianglePredicate.cpp`, `isInCircleRobust` takes parameters in
//   `(q, p, r, t)` order whereas `isInCircleNonRobust` takes `(p, q, r, t)`
//   with the same determinant sign mapping, so `isInCircleNonRobust` returns
//   inverted `INTERIOR`/`EXTERIOR` locations relative to `isInCircleRobust` and
//   `isInCircleNormalized` for the same vertex ordering.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/triangulate/quadedge/TrianglePredicate.cpp

#include "third_party/geos/include/geos/triangulate/quadedge/TrianglePredicate.h"

#include <array>
#include <limits>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Location.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Location;
using geos::triangulate::quadedge::TrianglePredicate;

struct InCircleCase {
  std::string_view name;
  double ax;
  double ay;
  double bx;
  double by;
  double cx;
  double cy;
  double px;
  double py;
  Location expected_normalized;
  Location expected_robust;
  Location expected_non_robust;
};

constexpr auto kInCircleCases = std::to_array<InCircleCase>({
    // Unit right triangle (0,0)-(1,0)-(0,1), CCW:
    // circumcircle is (x - 0.5)^2 + (y - 0.5)^2 = 0.5.
    {
        .name = "ccw_right_tri_center_inside",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 1.0,
        .by = 0.0,
        .cx = 0.0,
        .cy = 1.0,
        .px = 0.5,
        .py = 0.5,
        .expected_normalized = Location::INTERIOR,
        .expected_robust = Location::INTERIOR,
        .expected_non_robust = Location::EXTERIOR,
    },
    {
        .name = "ccw_right_tri_leg_midpoint_inside",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 1.0,
        .by = 0.0,
        .cx = 0.0,
        .cy = 1.0,
        .px = 0.5,
        .py = 0.0,
        .expected_normalized = Location::INTERIOR,
        .expected_robust = Location::INTERIOR,
        .expected_non_robust = Location::EXTERIOR,
    },
    {
        .name = "ccw_right_tri_fourth_corner_on_circle",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 1.0,
        .by = 0.0,
        .cx = 0.0,
        .cy = 1.0,
        .px = 1.0,
        .py = 1.0,
        .expected_normalized = Location::BOUNDARY,
        .expected_robust = Location::BOUNDARY,
        .expected_non_robust = Location::BOUNDARY,
    },
    {
        .name = "ccw_right_tri_vertex_a_on_circle",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 1.0,
        .by = 0.0,
        .cx = 0.0,
        .cy = 1.0,
        .px = 0.0,
        .py = 0.0,
        .expected_normalized = Location::BOUNDARY,
        .expected_robust = Location::BOUNDARY,
        .expected_non_robust = Location::BOUNDARY,
    },
    {
        .name = "ccw_right_tri_outside_positive",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 1.0,
        .by = 0.0,
        .cx = 0.0,
        .cy = 1.0,
        .px = 2.0,
        .py = 2.0,
        .expected_normalized = Location::EXTERIOR,
        .expected_robust = Location::EXTERIOR,
        .expected_non_robust = Location::INTERIOR,
    },
    {
        .name = "ccw_right_tri_outside_negative",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 1.0,
        .by = 0.0,
        .cx = 0.0,
        .cy = 1.0,
        .px = -0.5,
        .py = -0.5,
        .expected_normalized = Location::EXTERIOR,
        .expected_robust = Location::EXTERIOR,
        .expected_non_robust = Location::INTERIOR,
    },
    // CW orientation reverses interior/exterior while preserving boundary.
    {
        .name = "cw_right_tri_center",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 0.0,
        .by = 1.0,
        .cx = 1.0,
        .cy = 0.0,
        .px = 0.5,
        .py = 0.5,
        .expected_normalized = Location::EXTERIOR,
        .expected_robust = Location::EXTERIOR,
        .expected_non_robust = Location::INTERIOR,
    },
    {
        .name = "cw_right_tri_on_circle",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 0.0,
        .by = 1.0,
        .cx = 1.0,
        .cy = 0.0,
        .px = 1.0,
        .py = 1.0,
        .expected_normalized = Location::BOUNDARY,
        .expected_robust = Location::BOUNDARY,
        .expected_non_robust = Location::BOUNDARY,
    },
    {
        .name = "cw_right_tri_outside",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 0.0,
        .by = 1.0,
        .cx = 1.0,
        .cy = 0.0,
        .px = 2.0,
        .py = 2.0,
        .expected_normalized = Location::INTERIOR,
        .expected_robust = Location::INTERIOR,
        .expected_non_robust = Location::EXTERIOR,
    },
    // Circle of radius 5 centered at origin: (5,0), (0,5), (-3,-4) (CCW).
    {
        .name = "radius5_origin_inside",
        .ax = 5.0,
        .ay = 0.0,
        .bx = 0.0,
        .by = 5.0,
        .cx = -3.0,
        .cy = -4.0,
        .px = -2.0,
        .py = 4.0,
        .expected_normalized = Location::INTERIOR,
        .expected_robust = Location::INTERIOR,
        .expected_non_robust = Location::EXTERIOR,
    },
    {
        .name = "radius5_pythagorean_triple_on_circle",
        .ax = 5.0,
        .ay = 0.0,
        .bx = 0.0,
        .by = 5.0,
        .cx = -3.0,
        .cy = -4.0,
        .px = 3.0,
        .py = -4.0,
        .expected_normalized = Location::BOUNDARY,
        .expected_robust = Location::BOUNDARY,
        .expected_non_robust = Location::BOUNDARY,
    },
    {
        .name = "radius5_just_outside",
        .ax = 5.0,
        .ay = 0.0,
        .bx = 0.0,
        .by = 5.0,
        .cx = -3.0,
        .cy = -4.0,
        .px = 5.0,
        .py = 1.0,
        .expected_normalized = Location::EXTERIOR,
        .expected_robust = Location::EXTERIOR,
        .expected_non_robust = Location::INTERIOR,
    },
    // Translated by (-100, +250).
    {
        .name = "translated_inside",
        .ax = -95.0,
        .ay = 250.0,
        .bx = -100.0,
        .by = 255.0,
        .cx = -103.0,
        .cy = 246.0,
        .px = -100.0,
        .py = 250.0,
        .expected_normalized = Location::INTERIOR,
        .expected_robust = Location::INTERIOR,
        .expected_non_robust = Location::EXTERIOR,
    },
    {
        .name = "translated_on_circle",
        .ax = -95.0,
        .ay = 250.0,
        .bx = -100.0,
        .by = 255.0,
        .cx = -103.0,
        .cy = 246.0,
        .px = -97.0,
        .py = 254.0,
        .expected_normalized = Location::BOUNDARY,
        .expected_robust = Location::BOUNDARY,
        .expected_non_robust = Location::BOUNDARY,
    },
    {
        .name = "translated_outside",
        .ax = -95.0,
        .ay = 250.0,
        .bx = -100.0,
        .by = 255.0,
        .cx = -103.0,
        .cy = 246.0,
        .px = -94.0,
        .py = 250.0,
        .expected_normalized = Location::EXTERIOR,
        .expected_robust = Location::EXTERIOR,
        .expected_non_robust = Location::INTERIOR,
    },
    // Degenerate triangles: collinear and duplicate vertices.
    {
        .name = "collinear_point_on_line",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 1.0,
        .by = 0.0,
        .cx = 2.0,
        .cy = 0.0,
        .px = 3.0,
        .py = 0.0,
        .expected_normalized = Location::BOUNDARY,
        .expected_robust = Location::BOUNDARY,
        .expected_non_robust = Location::BOUNDARY,
    },
    {
        .name = "collinear_point_off_line",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 1.0,
        .by = 0.0,
        .cx = 2.0,
        .cy = 0.0,
        .px = 1.0,
        .py = 1.0,
        .expected_normalized = Location::INTERIOR,
        .expected_robust = Location::INTERIOR,
        .expected_non_robust = Location::EXTERIOR,
    },
    {
        .name = "duplicate_ab_vertices",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 0.0,
        .by = 0.0,
        .cx = 1.0,
        .cy = 0.0,
        .px = 0.5,
        .py = 0.5,
        .expected_normalized = Location::BOUNDARY,
        .expected_robust = Location::BOUNDARY,
        .expected_non_robust = Location::BOUNDARY,
    },
    {
        .name = "duplicate_bc_vertices",
        .ax = 0.0,
        .ay = 0.0,
        .bx = 1.0,
        .by = 0.0,
        .cx = 1.0,
        .cy = 0.0,
        .px = 0.5,
        .py = 0.5,
        .expected_normalized = Location::BOUNDARY,
        .expected_robust = Location::BOUNDARY,
        .expected_non_robust = Location::BOUNDARY,
    },
    {
        .name = "all_vertices_coincident",
        .ax = 1.0,
        .ay = 1.0,
        .bx = 1.0,
        .by = 1.0,
        .cx = 1.0,
        .cy = 1.0,
        .px = 2.0,
        .py = 3.0,
        .expected_normalized = Location::BOUNDARY,
        .expected_robust = Location::BOUNDARY,
        .expected_non_robust = Location::BOUNDARY,
    },
});

TEST(TrianglePredicateTest, AllPredicatesMatchExpectedLocations) {
  for (const auto& tc : kInCircleCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY a(tc.ax, tc.ay);
    const CoordinateXY b(tc.bx, tc.by);
    const CoordinateXY c(tc.cx, tc.cy);
    const CoordinateXY p(tc.px, tc.py);

    EXPECT_EQ(TrianglePredicate::isInCircleNormalized(a, b, c, p),
              tc.expected_normalized);
    EXPECT_EQ(TrianglePredicate::isInCircleRobust(a, b, c, p),
              tc.expected_robust);
    EXPECT_EQ(TrianglePredicate::isInCircleNonRobust(a, b, c, p),
              tc.expected_non_robust);
  }
}

TEST(TrianglePredicateTest, RobustDefaultsNearlyCocircularToBoundary) {
  const CoordinateXY a(0.0, 0.0);
  const CoordinateXY b(1.0, 0.0);
  const CoordinateXY c(0.0, 1.0);
  constexpr double kDelta = 2.0 * std::numeric_limits<double>::epsilon();

  // Slightly outside the unit-right-triangle circumcircle, within `deterror`.
  const CoordinateXY near_outside(1.0, 1.0 + kDelta);
  EXPECT_EQ(TrianglePredicate::isInCircleRobust(a, b, c, near_outside),
            Location::BOUNDARY);
  EXPECT_EQ(TrianglePredicate::isInCircleNormalized(a, b, c, near_outside),
            Location::EXTERIOR);
  EXPECT_EQ(TrianglePredicate::isInCircleNonRobust(a, b, c, near_outside),
            Location::INTERIOR);

  // Slightly inside the circumcircle, within `deterror`.
  const CoordinateXY near_inside(1.0, 1.0 - kDelta);
  EXPECT_EQ(TrianglePredicate::isInCircleRobust(a, b, c, near_inside),
            Location::BOUNDARY);
  EXPECT_EQ(TrianglePredicate::isInCircleNormalized(a, b, c, near_inside),
            Location::INTERIOR);
  EXPECT_EQ(TrianglePredicate::isInCircleNonRobust(a, b, c, near_inside),
            Location::EXTERIOR);
}

TEST(TrianglePredicateTest, LargeTranslationPreservesAccuracy) {
  constexpr double kOffset = 1e8;
  const CoordinateXY a(kOffset, kOffset);
  const CoordinateXY b(kOffset + 1.0, kOffset);
  const CoordinateXY c(kOffset, kOffset + 1.0);

  const CoordinateXY inside(kOffset + 0.5, kOffset + 0.5);
  const CoordinateXY on_circle(kOffset + 1.0, kOffset + 1.0);
  const CoordinateXY outside(kOffset + 2.0, kOffset + 2.0);

  EXPECT_EQ(TrianglePredicate::isInCircleNormalized(a, b, c, inside),
            Location::INTERIOR);
  EXPECT_EQ(TrianglePredicate::isInCircleNormalized(a, b, c, on_circle),
            Location::BOUNDARY);
  EXPECT_EQ(TrianglePredicate::isInCircleNormalized(a, b, c, outside),
            Location::EXTERIOR);

  EXPECT_EQ(TrianglePredicate::isInCircleRobust(a, b, c, inside),
            Location::INTERIOR);
  EXPECT_EQ(TrianglePredicate::isInCircleRobust(a, b, c, on_circle),
            Location::BOUNDARY);
  EXPECT_EQ(TrianglePredicate::isInCircleRobust(a, b, c, outside),
            Location::EXTERIOR);

  EXPECT_EQ(TrianglePredicate::isInCircleNonRobust(a, b, c, inside),
            Location::EXTERIOR);
  EXPECT_EQ(TrianglePredicate::isInCircleNonRobust(a, b, c, on_circle),
            Location::BOUNDARY);
  EXPECT_EQ(TrianglePredicate::isInCircleNonRobust(a, b, c, outside),
            Location::INTERIOR);
}

TEST(TrianglePredicateTest, NonFiniteCoordinatesDoNotCrash) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const CoordinateXY a(0.0, 0.0);
  const CoordinateXY b(1.0, 0.0);
  const CoordinateXY c(0.0, 1.0);

  const CoordinateXY nan_pt(nan, 0.0);
  EXPECT_EQ(TrianglePredicate::isInCircleNonRobust(a, b, c, nan_pt),
            Location::BOUNDARY);
  EXPECT_EQ(TrianglePredicate::isInCircleNormalized(a, b, c, nan_pt),
            Location::INTERIOR);
  EXPECT_EQ(TrianglePredicate::isInCircleRobust(a, b, c, nan_pt),
            Location::BOUNDARY);

  const CoordinateXY inf_pt(inf, 0.0);
  EXPECT_EQ(TrianglePredicate::isInCircleNonRobust(a, b, c, inf_pt),
            Location::BOUNDARY);
  EXPECT_EQ(TrianglePredicate::isInCircleNormalized(a, b, c, inf_pt),
            Location::INTERIOR);
  EXPECT_EQ(TrianglePredicate::isInCircleRobust(a, b, c, inf_pt),
            Location::BOUNDARY);
}

}  // namespace
}  // namespace geos_google
