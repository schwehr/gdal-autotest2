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

// Unit tests for third_party/geos/src/operation/valid/PolygonRingSelfNode.cpp.

#include "third_party/geos/include/geos/operation/valid/PolygonRingSelfNode.h"

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::operation::valid::PolygonRingSelfNode;
using geos::util::IllegalArgumentException;

TEST(PolygonRingSelfNodeTest, GetCoordinatePreservesNodePoint) {
  CoordinateXY node_pt(3.5, -7.25);
  const CoordinateXY e00(10.0, -7.25);
  const CoordinateXY e01(3.5, 5.0);
  const CoordinateXY e10(-2.0, -12.0);
  const CoordinateXY e11(1.0, -12.0);

  PolygonRingSelfNode node(node_pt, &e00, &e01, &e10, &e11);
  // Mutating the caller's CoordinateXY must not alter the stored node point.
  node_pt.x = 999.0;
  node_pt.y = -999.0;

  const CoordinateXY* stored = node.getCoordinate();
  ASSERT_NE(stored, nullptr);
  EXPECT_DOUBLE_EQ(stored->x, 3.5);
  EXPECT_DOUBLE_EQ(stored->y, -7.25);
  EXPECT_EQ(node.getCoordinate(), stored);

  // e11 is unused by isExterior; passing nullptr must remain safe.
  PolygonRingSelfNode node_null_e11(CoordinateXY(0.0, 0.0), &e00, &e01, &e10,
                                    nullptr);
  EXPECT_TRUE(node_null_e11.getCoordinate()->equals2D(CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(node_null_e11.isExterior(/*isInteriorOnRight=*/true));
}

TEST(PolygonRingSelfNodeTest, IsExteriorAcuteRightHandCorner) {
  // Corner e00 -> node -> e01 turns from +X to +Y: the right-hand sector
  // (looking along e00 -> node -> e01) is Quadrant I (0..90 deg).
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY e00(10.0, 0.0);
  const CoordinateXY e01(0.0, 10.0);
  const CoordinateXY e11_unused(0.0, -10.0);

  // Segment inside Quadrant I lies in the right-hand sector.
  const CoordinateXY e10_in_q1(5.0, 5.0);
  PolygonRingSelfNode inside_right_wedge(node_pt, &e00, &e01, &e10_in_q1,
                                         &e11_unused);
  EXPECT_FALSE(inside_right_wedge.isExterior(/*isInteriorOnRight=*/true));
  EXPECT_TRUE(inside_right_wedge.isExterior(/*isInteriorOnRight=*/false));

  // Segments in Quadrants II, III, IV lie outside the right-hand sector.
  const CoordinateXY outside_pts[] = {
      {-5.0, 5.0},
      {-5.0, -5.0},
      {5.0, -5.0},
  };
  for (const CoordinateXY& e10_out : outside_pts) {
    PolygonRingSelfNode outside_right_wedge(node_pt, &e00, &e01, &e10_out,
                                            &e11_unused);
    EXPECT_TRUE(outside_right_wedge.isExterior(/*isInteriorOnRight=*/true));
    EXPECT_FALSE(outside_right_wedge.isExterior(/*isInteriorOnRight=*/false));
  }
}

TEST(PolygonRingSelfNodeTest, IsExteriorReflexRightHandCorner) {
  // Reversing the corner to e00(0,10) -> node(0,0) -> e01(10,0) makes the
  // right-hand sector the 270-degree reflex arc covering Quadrants II, III, IV.
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY e00(0.0, 10.0);
  const CoordinateXY e01(10.0, 0.0);
  const CoordinateXY e11_unused(-10.0, 0.0);

  // Segment in Quadrant I is now outside the right-hand sector.
  const CoordinateXY e10_q1(5.0, 5.0);
  PolygonRingSelfNode q1_node(node_pt, &e00, &e01, &e10_q1, &e11_unused);
  EXPECT_TRUE(q1_node.isExterior(/*isInteriorOnRight=*/true));
  EXPECT_FALSE(q1_node.isExterior(/*isInteriorOnRight=*/false));

  // Segments in Quadrants II, III, IV are inside the right-hand sector.
  const CoordinateXY reflex_pts[] = {
      {-5.0, 5.0},
      {-5.0, -5.0},
      {5.0, -5.0},
  };
  for (const CoordinateXY& e10_in : reflex_pts) {
    PolygonRingSelfNode in_reflex(node_pt, &e00, &e01, &e10_in, &e11_unused);
    EXPECT_FALSE(in_reflex.isExterior(/*isInteriorOnRight=*/true));
    EXPECT_TRUE(in_reflex.isExterior(/*isInteriorOnRight=*/false));
  }
}

TEST(PolygonRingSelfNodeTest, IsExteriorSymmetricSelfTouchRoles) {
  // Inverted shell touching itself at (5, 0) to enclose a hole:
  // (10, 0) -> (5, 0) -> (8, 5) and (2, 5) -> (5, 0) -> (0, 0).
  const CoordinateXY pinch_pt(5.0, 0.0);
  const CoordinateXY e00(10.0, 0.0);
  const CoordinateXY e01(8.0, 5.0);
  const CoordinateXY e10(2.0, 5.0);
  const CoordinateXY e11(0.0, 0.0);

  // Both corners and all symmetrical edge choices agree that the touch is
  // exterior when interior is on the right (CW shell / CCW hole) and interior
  // when interior is on the left (CCW shell / CW hole).
  PolygonRingSelfNode from_c0_e10(pinch_pt, &e00, &e01, &e10, &e11);
  PolygonRingSelfNode from_c0_e11(pinch_pt, &e00, &e01, &e11, &e10);
  PolygonRingSelfNode from_c1_e00(pinch_pt, &e10, &e11, &e00, &e01);
  PolygonRingSelfNode from_c1_e01(pinch_pt, &e10, &e11, &e01, &e00);

  for (const PolygonRingSelfNode& node :
       {from_c0_e10, from_c0_e11, from_c1_e00, from_c1_e01}) {
    EXPECT_TRUE(node.isExterior(/*isInteriorOnRight=*/true));
    EXPECT_FALSE(node.isExterior(/*isInteriorOnRight=*/false));
  }

  // Bow-tie CW shell self-touching at (5, 5):
  // (0, 10) -> (5, 5) -> (10, 10) and (10, 0) -> (5, 5) -> (0, 0).
  const CoordinateXY bowtie_pt(5.0, 5.0);
  const CoordinateXY b00(0.0, 10.0);
  const CoordinateXY b01(10.0, 10.0);
  const CoordinateXY b10(10.0, 0.0);
  const CoordinateXY b11(0.0, 0.0);

  PolygonRingSelfNode bowtie_c0(bowtie_pt, &b00, &b01, &b10, &b11);
  PolygonRingSelfNode bowtie_c1(bowtie_pt, &b10, &b11, &b00, &b01);

  EXPECT_FALSE(bowtie_c0.isExterior(/*isInteriorOnRight=*/true));
  EXPECT_FALSE(bowtie_c1.isExterior(/*isInteriorOnRight=*/true));
  EXPECT_TRUE(bowtie_c0.isExterior(/*isInteriorOnRight=*/false));
  EXPECT_TRUE(bowtie_c1.isExterior(/*isInteriorOnRight=*/false));
}

TEST(PolygonRingSelfNodeTest, IsExteriorTranslatedAndNegativeCoordinates) {
  const CoordinateXY node_pt(-25.5, 13.75);
  const CoordinateXY e00(-20.0, 11.25);  // SE relative to node
  const CoordinateXY e01(-22.0, 18.5);   // NE relative to node
  const CoordinateXY e10_inside(-19.0, 14.0);
  const CoordinateXY e10_outside(-30.0, 13.5);
  const CoordinateXY e11(-31.0, 10.0);

  PolygonRingSelfNode inside(node_pt, &e00, &e01, &e10_inside, &e11);
  EXPECT_FALSE(inside.isExterior(/*isInteriorOnRight=*/true));
  EXPECT_TRUE(inside.isExterior(/*isInteriorOnRight=*/false));

  PolygonRingSelfNode outside(node_pt, &e00, &e01, &e10_outside, &e11);
  EXPECT_TRUE(outside.isExterior(/*isInteriorOnRight=*/true));
  EXPECT_FALSE(outside.isExterior(/*isInteriorOnRight=*/false));
}

TEST(PolygonRingSelfNodeTest, DegenerateZeroLengthAndNaNThrow) {
  const CoordinateXY node_pt(1.0, 2.0);
  const CoordinateXY e00(3.0, 2.0);
  const CoordinateXY e01(1.0, 4.0);
  const CoordinateXY e10(-1.0, 0.0);

  // Zero-length segment to node_pt causes Quadrant::quadrant to throw.
  PolygonRingSelfNode zero_corner(node_pt, &node_pt, &e01, &e10, nullptr);
  EXPECT_THROW(zero_corner.isExterior(true), IllegalArgumentException);

  PolygonRingSelfNode zero_test_seg(node_pt, &e00, &e01, &node_pt, nullptr);
  EXPECT_THROW(zero_test_seg.isExterior(false), IllegalArgumentException);

  // NaN coordinate sharing quadrant NW with e01 triggers Orientation::index
  // non-finite input check.
  const CoordinateXY nan_pt(geos::DoubleNotANumber, 5.0);
  const CoordinateXY nw_pt(-1.0, 5.0);
  PolygonRingSelfNode nan_corner(node_pt, &nan_pt, &nw_pt, &e10, nullptr);
  EXPECT_THROW(nan_corner.isExterior(true), IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
