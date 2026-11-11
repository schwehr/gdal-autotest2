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

// Unit tests for third_party/geos/src/algorithm/PolygonNodeTopology.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/PolygonNodeTopology.cpp

#include "third_party/geos/include/geos/algorithm/PolygonNodeTopology.h"

#include <array>
#include <cmath>
#include <limits>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::algorithm::PolygonNodeTopology;
using geos::geom::CoordinateXY;
using geos::util::IllegalArgumentException;

struct CompareAngleCase {
  std::string_view name;
  CoordinateXY origin;
  CoordinateXY p;
  CoordinateXY q;
  int expected_sign;
};

TEST(PolygonNodeTopologyTest, CompareAngleCases) {
  const auto kCases = std::to_array<CompareAngleCase>({
      // Cross-quadrant ordering: NE(0) < NW(1) < SW(2) < SE(3).
      {.name = "ne_less_than_nw",
       .origin = {0.0, 0.0},
       .p = {1.0, 1.0},
       .q = {-1.0, 1.0},
       .expected_sign = -1},
      {.name = "nw_less_than_sw",
       .origin = {0.0, 0.0},
       .p = {-1.0, 1.0},
       .q = {-1.0, -1.0},
       .expected_sign = -1},
      {.name = "sw_less_than_se",
       .origin = {0.0, 0.0},
       .p = {-1.0, -1.0},
       .q = {1.0, -1.0},
       .expected_sign = -1},
      {.name = "se_greater_than_ne",
       .origin = {0.0, 0.0},
       .p = {1.0, -1.0},
       .q = {1.0, 0.0},
       .expected_sign = 1},
      {.name = "shifted_origin_sw_less_than_se",
       .origin = {100.0, -200.0},
       .p = {99.0, -201.0},
       .q = {101.0, -201.0},
       .expected_sign = -1},
      // Axes belong to NE (+X, +Y), NW (-X), SE (-Y).
      {.name = "pos_x_less_than_ne_diag",
       .origin = {0.0, 0.0},
       .p = {1.0, 0.0},
       .q = {1.0, 1.0},
       .expected_sign = -1},
      {.name = "pos_y_greater_than_ne_diag",
       .origin = {0.0, 0.0},
       .p = {0.0, 1.0},
       .q = {1.0, 1.0},
       .expected_sign = 1},
      {.name = "pos_y_less_than_nw_diag",
       .origin = {0.0, 0.0},
       .p = {0.0, 1.0},
       .q = {-1.0, 1.0},
       .expected_sign = -1},
      {.name = "neg_x_greater_than_nw_diag",
       .origin = {0.0, 0.0},
       .p = {-1.0, 0.0},
       .q = {-1.0, 1.0},
       .expected_sign = 1},
      {.name = "neg_x_less_than_sw_diag",
       .origin = {0.0, 0.0},
       .p = {-1.0, 0.0},
       .q = {-1.0, -1.0},
       .expected_sign = -1},
      {.name = "neg_y_greater_than_sw_diag",
       .origin = {0.0, 0.0},
       .p = {0.0, -1.0},
       .q = {-1.0, -1.0},
       .expected_sign = 1},
      {.name = "neg_y_less_than_se_diag",
       .origin = {0.0, 0.0},
       .p = {0.0, -1.0},
       .q = {1.0, -1.0},
       .expected_sign = -1},
      // Same-quadrant ordering across all four quadrants.
      {.name = "same_quadrant_ne_ccw",
       .origin = {0.0, 0.0},
       .p = {1.0, 2.0},
       .q = {2.0, 1.0},
       .expected_sign = 1},
      {.name = "same_quadrant_nw_cw",
       .origin = {0.0, 0.0},
       .p = {-1.0, 2.0},
       .q = {-2.0, 1.0},
       .expected_sign = -1},
      {.name = "same_quadrant_sw_ccw",
       .origin = {0.0, 0.0},
       .p = {-1.0, -2.0},
       .q = {-2.0, -1.0},
       .expected_sign = 1},
      {.name = "same_quadrant_se_cw",
       .origin = {0.0, 0.0},
       .p = {1.0, -2.0},
       .q = {2.0, -1.0},
       .expected_sign = -1},
      {.name = "nearly_collinear_ne_dd_precision",
       .origin = {0.0, 0.0},
       .p = {static_cast<double>(1ULL << 45), 1.0},
       .q = {static_cast<double>(1ULL << 46), 2.0 + std::ldexp(1.0, -51)},
       .expected_sign = -1},
      // Collinear vectors with different lengths and shifted origin.
      {.name = "collinear_same_ray_different_lengths",
       .origin = {10.0, -5.0},
       .p = {12.0, -1.0},
       .q = {16.0, 7.0},
       .expected_sign = 0},
      {.name = "collinear_same_ray_sw",
       .origin = {0.0, 0.0},
       .p = {-2.0, -3.0},
       .q = {-4.0, -6.0},
       .expected_sign = 0},
      {.name = "identical_endpoints",
       .origin = {-3.0, 4.0},
       .p = {0.0, 8.0},
       .q = {0.0, 8.0},
       .expected_sign = 0},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(PolygonNodeTopology::compareAngle(&tc.origin, &tc.p, &tc.q),
              tc.expected_sign);
    EXPECT_EQ(PolygonNodeTopology::compareAngle(&tc.origin, &tc.q, &tc.p),
              -tc.expected_sign);
  }
}

TEST(PolygonNodeTopologyTest, CompareAngleThrowsOnInvalidInputs) {
  const CoordinateXY origin{5.0, 5.0};
  const CoordinateXY other{6.0, 7.0};
  EXPECT_THROW(PolygonNodeTopology::compareAngle(&origin, &origin, &other),
               IllegalArgumentException);
  EXPECT_THROW(PolygonNodeTopology::compareAngle(&origin, &other, &origin),
               IllegalArgumentException);
  EXPECT_THROW(PolygonNodeTopology::compareAngle(&origin, &origin, &origin),
               IllegalArgumentException);

  const CoordinateXY zero{0.0, 0.0};
  const CoordinateXY q_ne{1.0, 1.0};
  const CoordinateXY p_inf{1.0, std::numeric_limits<double>::infinity()};
  EXPECT_THROW(PolygonNodeTopology::compareAngle(&zero, &p_inf, &q_ne),
               IllegalArgumentException);

  const CoordinateXY q_se{1.0, -1.0};
  const CoordinateXY p_nan{1.0, std::numeric_limits<double>::quiet_NaN()};
  EXPECT_THROW(PolygonNodeTopology::compareAngle(&zero, &p_nan, &q_se),
               IllegalArgumentException);
}

struct CrossingCase {
  std::string_view name;
  CoordinateXY node;
  CoordinateXY a0;
  CoordinateXY a1;
  CoordinateXY b0;
  CoordinateXY b1;
  bool expected;
};

TEST(PolygonNodeTopologyTest, IsCrossingCases) {
  const auto kCases = std::to_array<CrossingCase>({
      // Orthogonal crossing across quadrants, both A orientations.
      {.name = "orthogonal_crossing_a_ccw",
       .node = {0.0, 0.0},
       .a0 = {1.0, 0.0},
       .a1 = {-1.0, 0.0},
       .b0 = {0.0, -1.0},
       .b1 = {0.0, 1.0},
       .expected = true},
      {.name = "orthogonal_crossing_a_swapped",
       .node = {0.0, 0.0},
       .a0 = {-1.0, 0.0},
       .a1 = {1.0, 0.0},
       .b0 = {0.0, 1.0},
       .b1 = {0.0, -1.0},
       .expected = true},
      // Non-crossing: both B segments stay on the same side of A.
      {.name = "non_crossing_both_between",
       .node = {0.0, 0.0},
       .a0 = {1.0, 0.0},
       .a1 = {-1.0, 0.0},
       .b0 = {1.0, 1.0},
       .b1 = {-1.0, 1.0},
       .expected = false},
      {.name = "non_crossing_both_outside",
       .node = {0.0, 0.0},
       .a0 = {-1.0, 0.0},
       .a1 = {1.0, 0.0},
       .b0 = {-1.0, -1.0},
       .b1 = {1.0, -1.0},
       .expected = false},
      // Same-quadrant wedges.
      {.name = "same_quadrant_ne_crossing",
       .node = {0.0, 0.0},
       .a0 = {3.0, 1.0},
       .a1 = {1.0, 3.0},
       .b0 = {2.0, 2.0},
       .b1 = {4.0, 1.0},
       .expected = true},
      {.name = "same_quadrant_sw_crossing",
       .node = {0.0, 0.0},
       .a0 = {-3.0, -1.0},
       .a1 = {-1.0, -3.0},
       .b0 = {-2.0, -2.0},
       .b1 = {-4.0, -1.0},
       .expected = true},
      {.name = "same_quadrant_ne_both_inside",
       .node = {0.0, 0.0},
       .a0 = {1.0, 3.0},
       .a1 = {3.0, 1.0},
       .b0 = {2.0, 2.0},
       .b1 = {3.0, 2.0},
       .expected = false},
      {.name = "same_quadrant_se_both_inside",
       .node = {0.0, 0.0},
       .a0 = {1.0, -3.0},
       .a1 = {3.0, -1.0},
       .b0 = {2.0, -2.0},
       .b1 = {2.0, -3.0},
       .expected = false},
      {.name = "same_quadrant_outside_both_sides_of_wedge",
       .node = {0.0, 0.0},
       .a0 = {3.0, 1.0},
       .a1 = {1.0, 3.0},
       .b0 = {4.0, 1.0},
       .b1 = {1.0, 4.0},
       .expected = false},
      // Wedge straddling the positive X axis (SE to NE).
      {.name = "wrap_pos_x_crossing",
       .node = {10.0, 20.0},
       .a0 = {11.0, 19.0},
       .a1 = {11.0, 21.0},
       .b0 = {12.0, 20.0},
       .b1 = {9.0, 20.0},
       .expected = true},
      {.name = "wrap_pos_x_both_outside_arc",
       .node = {10.0, 20.0},
       .a0 = {11.0, 19.0},
       .a1 = {11.0, 21.0},
       .b0 = {12.0, 19.5},
       .b1 = {12.0, 20.5},
       .expected = false},
      // Collinear A segments enclose zero arc.
      {.name = "collinear_a_segments_do_not_cross",
       .node = {0.0, 0.0},
       .a0 = {1.0, 1.0},
       .a1 = {2.0, 2.0},
       .b0 = {1.0, 0.0},
       .b1 = {0.0, 1.0},
       .expected = false},
      // Collinear endpoints with either A segment do not count as crossing.
      {.name = "b0_collinear_with_a_lo",
       .node = {0.0, 0.0},
       .a0 = {1.0, 0.0},
       .a1 = {-1.0, 0.0},
       .b0 = {2.0, 0.0},
       .b1 = {0.0, 1.0},
       .expected = false},
      {.name = "b0_collinear_with_a_hi",
       .node = {0.0, 0.0},
       .a0 = {1.0, 0.0},
       .a1 = {-1.0, 0.0},
       .b0 = {-2.0, 0.0},
       .b1 = {0.0, -1.0},
       .expected = false},
      {.name = "b1_collinear_with_a_lo",
       .node = {0.0, 0.0},
       .a0 = {1.0, 0.0},
       .a1 = {-1.0, 0.0},
       .b0 = {0.0, 1.0},
       .b1 = {2.0, 0.0},
       .expected = false},
      {.name = "b1_collinear_with_a_hi",
       .node = {0.0, 0.0},
       .a0 = {1.0, 0.0},
       .a1 = {-1.0, 0.0},
       .b0 = {0.0, -1.0},
       .b1 = {-2.0, 0.0},
       .expected = false},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(PolygonNodeTopology::isCrossing(&tc.node, &tc.a0, &tc.a1, &tc.b0,
                                              &tc.b1),
              tc.expected);
    EXPECT_EQ(PolygonNodeTopology::isCrossing(&tc.node, &tc.a1, &tc.a0, &tc.b0,
                                              &tc.b1),
              tc.expected);
    EXPECT_EQ(PolygonNodeTopology::isCrossing(&tc.node, &tc.a0, &tc.a1, &tc.b1,
                                              &tc.b0),
              tc.expected);
    EXPECT_EQ(PolygonNodeTopology::isCrossing(&tc.node, &tc.a1, &tc.a0, &tc.b1,
                                              &tc.b0),
              tc.expected);
  }
}

TEST(PolygonNodeTopologyTest, IsCrossingThrowsOnZeroLengthVector) {
  const CoordinateXY node{0.0, 0.0};
  const CoordinateXY a0{1.0, 0.0};
  const CoordinateXY a1{-1.0, 0.0};
  const CoordinateXY b0{0.0, 1.0};
  const CoordinateXY b1{0.0, -1.0};

  EXPECT_THROW(PolygonNodeTopology::isCrossing(&node, &node, &a1, &b0, &b1),
               IllegalArgumentException);
  EXPECT_THROW(PolygonNodeTopology::isCrossing(&node, &a0, &node, &b0, &b1),
               IllegalArgumentException);
  EXPECT_THROW(PolygonNodeTopology::isCrossing(&node, &a0, &a1, &node, &b1),
               IllegalArgumentException);
  EXPECT_THROW(PolygonNodeTopology::isCrossing(&node, &a0, &a1, &b0, &node),
               IllegalArgumentException);
}

struct InteriorSegmentCase {
  std::string_view name;
  CoordinateXY node;
  CoordinateXY a0;
  CoordinateXY a1;
  CoordinateXY b;
  bool expected;
};

TEST(PolygonNodeTopologyTest, IsInteriorSegmentCases) {
  const auto kCases = std::to_array<InteriorSegmentCase>({
      // CW square corners: (0,0) -> (0,10) -> (10,10) -> (10,0) -> (0,0).
      // Ring interior is on the right of a0 -> node -> a1.
      {.name = "cw_shell_sw_corner_inside",
       .node = {0.0, 0.0},
       .a0 = {10.0, 0.0},
       .a1 = {0.0, 10.0},
       .b = {5.0, 5.0},
       .expected = true},
      {.name = "cw_shell_sw_corner_outside_nw",
       .node = {0.0, 0.0},
       .a0 = {10.0, 0.0},
       .a1 = {0.0, 10.0},
       .b = {-5.0, 5.0},
       .expected = false},
      {.name = "cw_shell_sw_corner_outside_se",
       .node = {0.0, 0.0},
       .a0 = {10.0, 0.0},
       .a1 = {0.0, 10.0},
       .b = {5.0, -5.0},
       .expected = false},
      {.name = "cw_shell_nw_corner_inside",
       .node = {0.0, 10.0},
       .a0 = {0.0, 0.0},
       .a1 = {10.0, 10.0},
       .b = {5.0, 5.0},
       .expected = true},
      {.name = "cw_shell_nw_corner_outside",
       .node = {0.0, 10.0},
       .a0 = {0.0, 0.0},
       .a1 = {10.0, 10.0},
       .b = {-5.0, 15.0},
       .expected = false},
      {.name = "cw_shell_ne_corner_inside",
       .node = {10.0, 10.0},
       .a0 = {0.0, 10.0},
       .a1 = {10.0, 0.0},
       .b = {5.0, 5.0},
       .expected = true},
      {.name = "cw_shell_ne_corner_outside",
       .node = {10.0, 10.0},
       .a0 = {0.0, 10.0},
       .a1 = {10.0, 0.0},
       .b = {15.0, 15.0},
       .expected = false},
      {.name = "cw_shell_se_corner_inside",
       .node = {10.0, 0.0},
       .a0 = {10.0, 10.0},
       .a1 = {0.0, 0.0},
       .b = {5.0, 5.0},
       .expected = true},
      {.name = "cw_shell_se_corner_outside",
       .node = {10.0, 0.0},
       .a0 = {10.0, 10.0},
       .a1 = {0.0, 0.0},
       .b = {15.0, -5.0},
       .expected = false},
      // Concave (reflex) 270-degree CW shell corner around (10,10).
      {.name = "concave_cw_shell_reflex_corner_interior_nw",
       .node = {10.0, 10.0},
       .a0 = {20.0, 10.0},
       .a1 = {10.0, 0.0},
       .b = {5.0, 15.0},
       .expected = true},
      {.name = "concave_cw_shell_reflex_corner_interior_sw",
       .node = {10.0, 10.0},
       .a0 = {20.0, 10.0},
       .a1 = {10.0, 0.0},
       .b = {5.0, 5.0},
       .expected = true},
      {.name = "concave_cw_shell_reflex_corner_exterior_se",
       .node = {10.0, 10.0},
       .a0 = {20.0, 10.0},
       .a1 = {10.0, 0.0},
       .b = {15.0, 5.0},
       .expected = false},
      // Reversed (CCW) corner: right side is the reflex exterior (angle(a0) >
      // angle(a1)).
      {.name = "ccw_hole_corner_inside_hole_is_exterior",
       .node = {0.0, 0.0},
       .a0 = {0.0, 10.0},
       .a1 = {10.0, 0.0},
       .b = {5.0, 5.0},
       .expected = false},
      {.name = "ccw_hole_corner_outside_hole_sw_is_interior",
       .node = {0.0, 0.0},
       .a0 = {0.0, 10.0},
       .a1 = {10.0, 0.0},
       .b = {-5.0, -5.0},
       .expected = true},
      // Same-quadrant narrow wedges (both orientations).
      {.name = "same_quadrant_ne_wedge_interior",
       .node = {0.0, 0.0},
       .a0 = {3.0, 1.0},
       .a1 = {1.0, 3.0},
       .b = {2.0, 2.0},
       .expected = true},
      {.name = "same_quadrant_ne_wedge_exterior_before_a0",
       .node = {0.0, 0.0},
       .a0 = {3.0, 1.0},
       .a1 = {1.0, 3.0},
       .b = {4.0, 1.0},
       .expected = false},
      {.name = "same_quadrant_ne_wedge_exterior_after_a1",
       .node = {0.0, 0.0},
       .a0 = {3.0, 1.0},
       .a1 = {1.0, 3.0},
       .b = {1.0, 4.0},
       .expected = false},
      {.name = "same_quadrant_sw_reversed_inside_wedge_is_exterior",
       .node = {0.0, 0.0},
       .a0 = {-1.0, -3.0},
       .a1 = {-3.0, -1.0},
       .b = {-2.0, -2.0},
       .expected = false},
      {.name = "same_quadrant_sw_reversed_outside_wedge_is_interior",
       .node = {0.0, 0.0},
       .a0 = {-1.0, -3.0},
       .a1 = {-3.0, -1.0},
       .b = {1.0, 1.0},
       .expected = true},
      // Wedge wrapping across the positive X axis: (1,-1) -> (0,0) -> (1,1).
      {.name = "wrap_pos_x_wedge_on_pos_x_interior",
       .node = {0.0, 0.0},
       .a0 = {1.0, -1.0},
       .a1 = {1.0, 1.0},
       .b = {1.0, 0.0},
       .expected = true},
      {.name = "wrap_pos_x_wedge_on_neg_x_exterior",
       .node = {0.0, 0.0},
       .a0 = {1.0, -1.0},
       .a1 = {1.0, 1.0},
       .b = {-1.0, 0.0},
       .expected = false},
      {.name = "wrap_pos_x_wedge_reverse_on_pos_x_exterior",
       .node = {0.0, 0.0},
       .a0 = {1.0, 1.0},
       .a1 = {1.0, -1.0},
       .b = {1.0, 0.0},
       .expected = false},
      {.name = "wrap_pos_x_wedge_reverse_on_neg_x_interior",
       .node = {0.0, 0.0},
       .a0 = {1.0, 1.0},
       .a1 = {1.0, -1.0},
       .b = {-1.0, 0.0},
       .expected = true},
      // Zero-angle spike encloses empty interior.
      {.name = "collinear_corner_segments_empty_interior",
       .node = {0.0, 0.0},
       .a0 = {1.0, 1.0},
       .a1 = {2.0, 2.0},
       .b = {0.0, 1.0},
       .expected = false},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(
        PolygonNodeTopology::isInteriorSegment(&tc.node, &tc.a0, &tc.a1, &tc.b),
        tc.expected);
  }
}

TEST(PolygonNodeTopologyTest, IsInteriorSegmentThrowsOnZeroLengthVector) {
  const CoordinateXY node{0.0, 0.0};
  const CoordinateXY a0{1.0, 0.0};
  const CoordinateXY a1{0.0, 1.0};
  const CoordinateXY b{1.0, 1.0};

  EXPECT_THROW(PolygonNodeTopology::isInteriorSegment(&node, &node, &a1, &b),
               IllegalArgumentException);
  EXPECT_THROW(PolygonNodeTopology::isInteriorSegment(&node, &a0, &node, &b),
               IllegalArgumentException);
  EXPECT_THROW(PolygonNodeTopology::isInteriorSegment(&node, &a0, &a1, &node),
               IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
