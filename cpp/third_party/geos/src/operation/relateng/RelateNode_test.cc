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

// Unit tests for third_party/geos/src/operation/relateng/RelateNode.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/relateng/RelateNode.cpp

#include "third_party/geos/include/geos/operation/relateng/RelateNode.h"

#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/operation/relateng/NodeSection.h"
#include "third_party/geos/include/geos/operation/relateng/RelateEdge.h"
#include "third_party/geos/include/geos/operation/relateng/RelateGeometry.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Dimension;
using geos::geom::Location;
using geos::geom::Position;
using geos::operation::relateng::NodeSection;
using geos::operation::relateng::RelateEdge;
using geos::operation::relateng::RelateGeometry;
using geos::operation::relateng::RelateNode;

constexpr bool kGeomA = RelateGeometry::GEOM_A;
constexpr bool kGeomB = RelateGeometry::GEOM_B;
constexpr int kDimFalse = Dimension::False;
constexpr int kDimP = Dimension::P;
constexpr int kDimL = Dimension::L;
constexpr int kDimA = Dimension::A;
constexpr int kPosLeft = Position::LEFT;
constexpr int kPosOn = Position::ON;
constexpr int kPosRight = Position::RIGHT;

NodeSection MakeSection(bool is_a, int dim, const CoordinateXY* v0,
                        const CoordinateXY& node_pt, const CoordinateXY* v1) {
  return NodeSection(is_a, dim, /*id=*/0, /*ringId=*/0, /*poly=*/nullptr,
                     /*isNodeAtVertex=*/true, v0, node_pt, v1);
}

std::unique_ptr<NodeSection> MakeLineSectionPtr(bool is_a,
                                                const CoordinateXY* v0,
                                                const CoordinateXY& node_pt,
                                                const CoordinateXY* v1) {
  return std::make_unique<NodeSection>(is_a, kDimL, /*id=*/0, /*ringId=*/0,
                                       /*poly=*/nullptr,
                                       /*isNodeAtVertex=*/true, v0, node_pt,
                                       v1);
}

void ExpectEdgeLocations(const RelateEdge& edge, bool is_a, Location left,
                         Location on, Location right) {
  SCOPED_TRACE(is_a ? "Geometry A" : "Geometry B");
  EXPECT_EQ(edge.location(is_a, kPosLeft), left);
  EXPECT_EQ(edge.location(is_a, kPosOn), on);
  EXPECT_EQ(edge.location(is_a, kPosRight), right);
}

void ExpectEdgeMatchesRay(const RelateEdge& edge, const CoordinateXY& ray_pt) {
  EXPECT_EQ(edge.compareToEdge(&ray_pt), 0);
}

TEST(RelateNodeTest, EmptyNodeAccessorsFormattingAndFinish) {
  static_assert(!std::is_copy_constructible_v<RelateNode>);
  static_assert(!std::is_copy_assignable_v<RelateNode>);

  const CoordinateXY node_pt(1.0, 2.0);
  RelateNode node(&node_pt);

  EXPECT_EQ(node.getCoordinate(), &node_pt);
  EXPECT_TRUE(node.getCoordinate()->equals2D(CoordinateXY(1.0, 2.0)));
  EXPECT_TRUE(node.getEdges().empty());
  EXPECT_FALSE(node.hasExteriorEdge(kGeomA));
  EXPECT_FALSE(node.hasExteriorEdge(kGeomB));

  EXPECT_EQ(node.toString(), "Node[POINT (1 2 )]:\n");
  std::ostringstream oss;
  oss << node;
  EXPECT_EQ(oss.str(), "Node[POINT (1 2 )]:\n");

  node.finish(/*isAreaInteriorA=*/true, /*isAreaInteriorB=*/true);
  EXPECT_TRUE(node.getEdges().empty());
  EXPECT_FALSE(node.hasExteriorEdge(kGeomA));
  EXPECT_FALSE(node.hasExteriorEdge(kGeomB));
}

TEST(RelateNodeTest, AddEdgesOverloadsAndNonEdgeDimensions) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY east(1.0, 0.0);
  const CoordinateXY north(0.0, 1.0);
  const CoordinateXY west(-1.0, 0.0);
  const CoordinateXY south(0.0, -1.0);

  RelateNode node(&node_pt);

  std::vector<const NodeSection*> empty_raw;
  node.addEdges(empty_raw);
  EXPECT_TRUE(node.getEdges().empty());

  std::vector<std::unique_ptr<NodeSection>> empty_owned;
  node.addEdges(empty_owned);
  EXPECT_TRUE(node.getEdges().empty());

  const NodeSection point_ns =
      MakeSection(kGeomA, kDimP, &east, node_pt, &north);
  const NodeSection false_ns =
      MakeSection(kGeomB, kDimFalse, &west, node_pt, &south);
  node.addEdges(&point_ns);
  node.addEdges(&false_ns);
  EXPECT_TRUE(node.getEdges().empty());

  const NodeSection raw_a = MakeSection(kGeomA, kDimL, &east, node_pt, nullptr);
  const NodeSection raw_b =
      MakeSection(kGeomB, kDimL, nullptr, node_pt, &north);
  std::vector<const NodeSection*> raw_list{&raw_a, &raw_b};
  node.addEdges(raw_list);
  ASSERT_EQ(node.getEdges().size(), 2u);
  ExpectEdgeMatchesRay(*node.getEdges()[0], east);
  ExpectEdgeMatchesRay(*node.getEdges()[1], north);

  std::vector<std::unique_ptr<NodeSection>> owned_list;
  owned_list.push_back(MakeLineSectionPtr(kGeomA, &west, node_pt, nullptr));
  owned_list.push_back(MakeLineSectionPtr(kGeomB, nullptr, node_pt, &south));
  node.addEdges(owned_list);

  const auto& edges = node.getEdges();
  ASSERT_EQ(edges.size(), 4u);
  ExpectEdgeMatchesRay(*edges[0], east);
  ExpectEdgeMatchesRay(*edges[1], north);
  ExpectEdgeMatchesRay(*edges[2], west);
  ExpectEdgeMatchesRay(*edges[3], south);

  ExpectEdgeLocations(*edges[0], kGeomA, Location::EXTERIOR, Location::INTERIOR,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[0], kGeomB, Location::NONE, Location::NONE,
                      Location::NONE);
  ExpectEdgeLocations(*edges[1], kGeomA, Location::NONE, Location::NONE,
                      Location::NONE);
  ExpectEdgeLocations(*edges[1], kGeomB, Location::EXTERIOR, Location::INTERIOR,
                      Location::EXTERIOR);
}

TEST(RelateNodeTest, AddLineEdgesSkipsNullAndZeroLengthAndSortsCcw) {
  const CoordinateXY node_pt(10.0, 20.0);
  const CoordinateXY same_pt(10.0, 20.0);
  const CoordinateXY east(15.0, 20.0);
  const CoordinateXY ne(12.0, 22.0);
  const CoordinateXY north(10.0, 25.0);
  const CoordinateXY nw(8.0, 22.0);
  const CoordinateXY west(5.0, 20.0);
  const CoordinateXY sw(8.0, 18.0);
  const CoordinateXY south(10.0, 15.0);
  const CoordinateXY se(12.0, 18.0);

  RelateNode node(&node_pt);

  // Null and zero-length vertices must be ignored.
  const NodeSection both_null =
      MakeSection(kGeomA, kDimL, nullptr, node_pt, nullptr);
  const NodeSection both_zero =
      MakeSection(kGeomA, kDimL, &same_pt, node_pt, &same_pt);
  node.addEdges(&both_null);
  node.addEdges(&both_zero);
  EXPECT_TRUE(node.getEdges().empty());

  // Add one null endpoint and one valid endpoint in SE, then NW.
  const NodeSection se_only = MakeSection(kGeomA, kDimL, nullptr, node_pt, &se);
  const NodeSection nw_only = MakeSection(kGeomA, kDimL, &nw, node_pt, nullptr);
  node.addEdges(&se_only);
  node.addEdges(&nw_only);
  ASSERT_EQ(node.getEdges().size(), 2u);
  ExpectEdgeMatchesRay(*node.getEdges()[0], nw);
  ExpectEdgeMatchesRay(*node.getEdges()[1], se);

  // Zero-length vertex paired with valid vertices: inserts at index 0 (ne) and
  // in the middle (sw).
  const NodeSection ne_and_zero =
      MakeSection(kGeomA, kDimL, &same_pt, node_pt, &ne);
  const NodeSection zero_and_sw =
      MakeSection(kGeomA, kDimL, &sw, node_pt, &same_pt);
  node.addEdges(&ne_and_zero);
  node.addEdges(&zero_and_sw);

  // Insert remaining rays out of angular order: east at index 0, south, west,
  // north in the middle.
  const NodeSection ew_line = MakeSection(kGeomA, kDimL, &west, node_pt, &east);
  const NodeSection ns_line =
      MakeSection(kGeomA, kDimL, &south, node_pt, &north);
  node.addEdges(&ew_line);
  node.addEdges(&ns_line);

  const auto& edges = node.getEdges();
  ASSERT_EQ(edges.size(), 8u);
  ExpectEdgeMatchesRay(*edges[0], east);
  ExpectEdgeMatchesRay(*edges[1], ne);
  ExpectEdgeMatchesRay(*edges[2], north);
  ExpectEdgeMatchesRay(*edges[3], nw);
  ExpectEdgeMatchesRay(*edges[4], west);
  ExpectEdgeMatchesRay(*edges[5], sw);
  ExpectEdgeMatchesRay(*edges[6], south);
  ExpectEdgeMatchesRay(*edges[7], se);

  for (const auto& edge : edges) {
    ExpectEdgeLocations(*edge, kGeomA, Location::EXTERIOR, Location::INTERIOR,
                        Location::EXTERIOR);
    ExpectEdgeLocations(*edge, kGeomB, Location::NONE, Location::NONE,
                        Location::NONE);
  }
}

TEST(RelateNodeTest, AddEdgesMergesCollinearRaysWithinAndAcrossGeometries) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY east_near(2.0, 0.0);
  const CoordinateXY east_mid(5.0, 0.0);
  const CoordinateXY east_far(10.0, 0.0);
  const CoordinateXY north_near(0.0, 3.0);
  const CoordinateXY north_far(0.0, 10.0);
  const CoordinateXY west(-10.0, 0.0);
  const CoordinateXY south(0.0, -10.0);

  // Same-geometry line+line and cross-geometry line+line along East.
  {
    RelateNode node(&node_pt);
    const NodeSection a_line1 =
        MakeSection(kGeomA, kDimL, nullptr, node_pt, &east_near);
    const NodeSection a_line2 =
        MakeSection(kGeomA, kDimL, nullptr, node_pt, &east_mid);
    const NodeSection b_line =
        MakeSection(kGeomB, kDimL, nullptr, node_pt, &east_far);
    node.addEdges(&a_line1);
    node.addEdges(&a_line2);
    node.addEdges(&b_line);

    ASSERT_EQ(node.getEdges().size(), 1u);
    ExpectEdgeLocations(*node.getEdges()[0], kGeomA, Location::EXTERIOR,
                        Location::INTERIOR, Location::EXTERIOR);
    ExpectEdgeLocations(*node.getEdges()[0], kGeomB, Location::EXTERIOR,
                        Location::INTERIOR, Location::EXTERIOR);
  }

  // Line + Area upgrades A on North, while Area + Line keeps B area on East.
  {
    RelateNode node(&node_pt);
    const NodeSection a_line =
        MakeSection(kGeomA, kDimL, nullptr, node_pt, &north_near);
    const NodeSection a_area =
        MakeSection(kGeomA, kDimA, &north_far, node_pt, &west);
    const NodeSection b_area =
        MakeSection(kGeomB, kDimA, &east_near, node_pt, &north_far);
    const NodeSection b_line =
        MakeSection(kGeomB, kDimL, nullptr, node_pt, &east_far);
    node.addEdges(&a_line);
    node.addEdges(&a_area);
    node.addEdges(&b_area);
    node.addEdges(&b_line);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 3u);
    ExpectEdgeMatchesRay(*edges[0], east_near);
    ExpectEdgeMatchesRay(*edges[1], north_near);
    ExpectEdgeMatchesRay(*edges[2], west);

    ExpectEdgeLocations(*edges[0], kGeomB, Location::INTERIOR,
                        Location::BOUNDARY, Location::EXTERIOR);
    ExpectEdgeLocations(*edges[1], kGeomA, Location::INTERIOR,
                        Location::BOUNDARY, Location::EXTERIOR);
    ExpectEdgeLocations(*edges[1], kGeomB, Location::EXTERIOR,
                        Location::BOUNDARY, Location::INTERIOR);
    ExpectEdgeLocations(*edges[2], kGeomA, Location::EXTERIOR,
                        Location::BOUNDARY, Location::INTERIOR);
  }

  // Two adjacent Area A corners sharing East promote East to INTERIOR via
  // merge + updateIfAreaNext.
  {
    RelateNode node(&node_pt);
    const NodeSection q1 =
        MakeSection(kGeomA, kDimA, &east_far, node_pt, &north_far);
    const NodeSection q4 =
        MakeSection(kGeomA, kDimA, &south, node_pt, &east_near);
    node.addEdges(&q1);
    node.addEdges(&q4);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 3u);
    ExpectEdgeMatchesRay(*edges[0], east_far);
    ExpectEdgeMatchesRay(*edges[1], north_far);
    ExpectEdgeMatchesRay(*edges[2], south);

    ExpectEdgeLocations(*edges[0], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);
    ExpectEdgeLocations(*edges[1], kGeomA, Location::EXTERIOR,
                        Location::BOUNDARY, Location::INTERIOR);
    ExpectEdgeLocations(*edges[2], kGeomA, Location::INTERIOR,
                        Location::BOUNDARY, Location::EXTERIOR);
    EXPECT_TRUE(node.hasExteriorEdge(kGeomA));
  }
}

TEST(RelateNodeTest, UpdateEdgesInAreaMarksInteriorEdgesWithoutAndWithWrap) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY east(10.0, 0.0);
  const CoordinateXY ene(10.0, 2.0);
  const CoordinateXY ne(5.0, 5.0);
  const CoordinateXY nne(2.0, 10.0);
  const CoordinateXY north(0.0, 10.0);
  const CoordinateXY nw(-5.0, 5.0);
  const CoordinateXY sw(-5.0, -5.0);
  const CoordinateXY se(5.0, -5.0);

  // Non-wrapping sector [east, north] marks ene, ne, nne interior for A.
  {
    RelateNode node(&node_pt);
    const NodeSection b_line_1 = MakeSection(kGeomB, kDimL, &sw, node_pt, &ene);
    const NodeSection b_line_2 =
        MakeSection(kGeomB, kDimL, nullptr, node_pt, &ne);
    const NodeSection b_line_3 =
        MakeSection(kGeomB, kDimL, nullptr, node_pt, &nne);
    const NodeSection a_corner =
        MakeSection(kGeomA, kDimA, &east, node_pt, &north);
    node.addEdges(&b_line_1);
    node.addEdges(&b_line_2);
    node.addEdges(&b_line_3);
    node.addEdges(&a_corner);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 6u);
    ExpectEdgeMatchesRay(*edges[0], east);
    ExpectEdgeMatchesRay(*edges[1], ene);
    ExpectEdgeMatchesRay(*edges[2], ne);
    ExpectEdgeMatchesRay(*edges[3], nne);
    ExpectEdgeMatchesRay(*edges[4], north);
    ExpectEdgeMatchesRay(*edges[5], sw);

    // Before finish: interior rays already marked, outside ray still NONE.
    ExpectEdgeLocations(*edges[0], kGeomA, Location::INTERIOR,
                        Location::BOUNDARY, Location::EXTERIOR);
    ExpectEdgeLocations(*edges[1], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);
    ExpectEdgeLocations(*edges[2], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);
    ExpectEdgeLocations(*edges[3], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);
    ExpectEdgeLocations(*edges[4], kGeomA, Location::EXTERIOR,
                        Location::BOUNDARY, Location::INTERIOR);
    ExpectEdgeLocations(*edges[5], kGeomA, Location::NONE, Location::NONE,
                        Location::NONE);
    EXPECT_TRUE(node.hasExteriorEdge(kGeomA));

    node.finish(/*isAreaInteriorA=*/false, /*isAreaInteriorB=*/false);
    ExpectEdgeLocations(*edges[5], kGeomA, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);
    ExpectEdgeLocations(*edges[0], kGeomB, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);
    ExpectEdgeLocations(*edges[4], kGeomB, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);
  }

  // Wrapping sector [north, east] (90..360/0 deg) marks nw, sw, se interior
  // while leaving ne outside until finish().
  {
    RelateNode node(&node_pt);
    const NodeSection b_diag_1 = MakeSection(kGeomB, kDimL, &ne, node_pt, &sw);
    const NodeSection b_diag_2 = MakeSection(kGeomB, kDimL, &nw, node_pt, &se);
    const NodeSection a_reflex =
        MakeSection(kGeomA, kDimA, &north, node_pt, &east);
    node.addEdges(&b_diag_1);
    node.addEdges(&b_diag_2);
    node.addEdges(&a_reflex);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 6u);
    ExpectEdgeMatchesRay(*edges[0], east);
    ExpectEdgeMatchesRay(*edges[1], ne);
    ExpectEdgeMatchesRay(*edges[2], north);
    ExpectEdgeMatchesRay(*edges[3], nw);
    ExpectEdgeMatchesRay(*edges[4], sw);
    ExpectEdgeMatchesRay(*edges[5], se);

    ExpectEdgeLocations(*edges[1], kGeomA, Location::NONE, Location::NONE,
                        Location::NONE);
    ExpectEdgeLocations(*edges[3], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);
    ExpectEdgeLocations(*edges[4], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);
    ExpectEdgeLocations(*edges[5], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);

    node.finish(/*isAreaInteriorA=*/false, /*isAreaInteriorB=*/false);
    ExpectEdgeLocations(*edges[1], kGeomA, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);
    // B propagates from startIndex=1 (ne) across index 5 -> 0 onto east.
    ExpectEdgeLocations(*edges[0], kGeomB, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);
    ExpectEdgeLocations(*edges[2], kGeomB, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);
  }
}

TEST(RelateNodeTest, OverlappingAreaSectionsUpdateViaPrevAndNextWithWrap) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY east(10.0, 0.0);
  const CoordinateXY ne(10.0, 10.0);
  const CoordinateXY north(0.0, 10.0);
  const CoordinateXY nw(-10.0, 10.0);
  const CoordinateXY west(-10.0, 0.0);
  const CoordinateXY sw(-10.0, -10.0);
  const CoordinateXY south(0.0, -10.0);
  const CoordinateXY se(10.0, -10.0);

  // Nested corner [east, ne] inside [south, north]:
  // east (index 0) wraps prevIndex to south; ne (index 1) checks next north.
  {
    RelateNode node(&node_pt);
    const NodeSection outer =
        MakeSection(kGeomA, kDimA, &south, node_pt, &north);
    const NodeSection inner = MakeSection(kGeomA, kDimA, &east, node_pt, &ne);
    node.addEdges(&outer);
    node.addEdges(&inner);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 4u);
    ExpectEdgeMatchesRay(*edges[0], east);
    ExpectEdgeMatchesRay(*edges[1], ne);
    ExpectEdgeMatchesRay(*edges[2], north);
    ExpectEdgeMatchesRay(*edges[3], south);

    ExpectEdgeLocations(*edges[0], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);
    ExpectEdgeLocations(*edges[1], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);
    ExpectEdgeLocations(*edges[2], kGeomA, Location::EXTERIOR,
                        Location::BOUNDARY, Location::INTERIOR);
    ExpectEdgeLocations(*edges[3], kGeomA, Location::INTERIOR,
                        Location::BOUNDARY, Location::EXTERIOR);
  }

  // Nested corner [sw, se] inside [west, east]:
  // sw (index 2) checks prev west; se (index 3 == size-1) wraps nextIndex to 0.
  {
    RelateNode node(&node_pt);
    const NodeSection outer = MakeSection(kGeomA, kDimA, &west, node_pt, &east);
    const NodeSection inner = MakeSection(kGeomA, kDimA, &sw, node_pt, &se);
    node.addEdges(&outer);
    node.addEdges(&inner);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 4u);
    ExpectEdgeMatchesRay(*edges[0], east);
    ExpectEdgeMatchesRay(*edges[1], west);
    ExpectEdgeMatchesRay(*edges[2], sw);
    ExpectEdgeMatchesRay(*edges[3], se);

    ExpectEdgeLocations(*edges[0], kGeomA, Location::EXTERIOR,
                        Location::BOUNDARY, Location::INTERIOR);
    ExpectEdgeLocations(*edges[1], kGeomA, Location::INTERIOR,
                        Location::BOUNDARY, Location::EXTERIOR);
    ExpectEdgeLocations(*edges[2], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);
    ExpectEdgeLocations(*edges[3], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);
  }

  // Asymmetric overlap on Geometry B: left-only overlap (prev=INTERIOR only).
  {
    RelateNode node(&node_pt);
    const NodeSection b1 = MakeSection(kGeomB, kDimA, &east, node_pt, &north);
    const NodeSection b2 = MakeSection(kGeomB, kDimA, &ne, node_pt, &nw);
    node.addEdges(&b1);
    node.addEdges(&b2);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 4u);
    ExpectEdgeLocations(*edges[0], kGeomB, Location::INTERIOR,
                        Location::BOUNDARY, Location::EXTERIOR);  // east
    ExpectEdgeLocations(*edges[1], kGeomB, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);  // ne
    ExpectEdgeLocations(*edges[2], kGeomB, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);  // north
    ExpectEdgeLocations(*edges[3], kGeomB, Location::EXTERIOR,
                        Location::BOUNDARY, Location::INTERIOR);  // nw
  }

  // Asymmetric overlap on Geometry B: right-only overlap (next=INTERIOR only).
  {
    RelateNode node(&node_pt);
    const NodeSection b1 = MakeSection(kGeomB, kDimA, &north, node_pt, &west);
    const NodeSection b2 = MakeSection(kGeomB, kDimA, &ne, node_pt, &nw);
    node.addEdges(&b1);
    node.addEdges(&b2);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 4u);
    ExpectEdgeLocations(*edges[0], kGeomB, Location::INTERIOR,
                        Location::BOUNDARY, Location::EXTERIOR);  // ne
    ExpectEdgeLocations(*edges[1], kGeomB, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);  // north
    ExpectEdgeLocations(*edges[2], kGeomB, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);  // nw
    ExpectEdgeLocations(*edges[3], kGeomB, Location::EXTERIOR,
                        Location::BOUNDARY, Location::INTERIOR);  // west
  }
}

TEST(RelateNodeTest, FinishHandlesAllAreaInteriorCombinationsAndPropagation) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY east(10.0, 0.0);
  const CoordinateXY ne(10.0, 10.0);
  const CoordinateXY north(0.0, 10.0);
  const CoordinateXY nw(-10.0, 10.0);
  const CoordinateXY west(-10.0, 0.0);
  const CoordinateXY sw(-10.0, -10.0);
  const CoordinateXY south(0.0, -10.0);
  const CoordinateXY se(10.0, -10.0);

  // finish(true, true) marks both A and B interior on every edge.
  {
    RelateNode node(&node_pt);
    const NodeSection a_line =
        MakeSection(kGeomA, kDimL, &west, node_pt, &east);
    const NodeSection b_line =
        MakeSection(kGeomB, kDimL, &south, node_pt, &north);
    node.addEdges(&a_line);
    node.addEdges(&b_line);
    node.finish(/*isAreaInteriorA=*/true, /*isAreaInteriorB=*/true);

    for (const auto& edge : node.getEdges()) {
      ExpectEdgeLocations(*edge, kGeomA, Location::INTERIOR, Location::INTERIOR,
                          Location::INTERIOR);
      ExpectEdgeLocations(*edge, kGeomB, Location::INTERIOR, Location::INTERIOR,
                          Location::INTERIOR);
    }
    EXPECT_FALSE(node.hasExteriorEdge(kGeomA));
    EXPECT_FALSE(node.hasExteriorEdge(kGeomB));
  }

  // finish(true, false): A is area-interior, B propagates from B's area corner.
  {
    RelateNode node(&node_pt);
    const NodeSection b_area =
        MakeSection(kGeomB, kDimA, &east, node_pt, &north);
    const NodeSection a_line = MakeSection(kGeomA, kDimL, &sw, node_pt, &ne);
    node.addEdges(&b_area);
    node.addEdges(&a_line);
    node.finish(/*isAreaInteriorA=*/true, /*isAreaInteriorB=*/false);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 4u);
    for (const auto& edge : edges) {
      ExpectEdgeLocations(*edge, kGeomA, Location::INTERIOR, Location::INTERIOR,
                          Location::INTERIOR);
    }
    ExpectEdgeLocations(*edges[1], kGeomB, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);  // ne
    ExpectEdgeLocations(*edges[3], kGeomB, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);  // sw
    EXPECT_FALSE(node.hasExteriorEdge(kGeomA));
    EXPECT_TRUE(node.hasExteriorEdge(kGeomB));
  }

  // finish(false, true): B is area-interior, A propagates from A's area corner.
  {
    RelateNode node(&node_pt);
    const NodeSection a_area =
        MakeSection(kGeomA, kDimA, &east, node_pt, &north);
    const NodeSection b_line = MakeSection(kGeomB, kDimL, &sw, node_pt, &ne);
    node.addEdges(&a_area);
    node.addEdges(&b_line);
    node.finish(/*isAreaInteriorA=*/false, /*isAreaInteriorB=*/true);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 4u);
    for (const auto& edge : edges) {
      ExpectEdgeLocations(*edge, kGeomB, Location::INTERIOR, Location::INTERIOR,
                          Location::INTERIOR);
    }
    ExpectEdgeLocations(*edges[1], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);  // ne
    ExpectEdgeLocations(*edges[3], kGeomA, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);  // sw
    EXPECT_TRUE(node.hasExteriorEdge(kGeomA));
    EXPECT_FALSE(node.hasExteriorEdge(kGeomB));
  }

  // Single-edge node with finish(false, false): propagation loop runs 0 times.
  {
    RelateNode node(&node_pt);
    const NodeSection a_ray =
        MakeSection(kGeomA, kDimL, nullptr, node_pt, &east);
    const NodeSection b_ray =
        MakeSection(kGeomB, kDimL, nullptr, node_pt, &east);
    node.addEdges(&a_ray);
    node.addEdges(&b_ray);
    node.finish(/*isAreaInteriorA=*/false, /*isAreaInteriorB=*/false);

    ASSERT_EQ(node.getEdges().size(), 1u);
    ExpectEdgeLocations(*node.getEdges()[0], kGeomA, Location::EXTERIOR,
                        Location::INTERIOR, Location::EXTERIOR);
    ExpectEdgeLocations(*node.getEdges()[0], kGeomB, Location::EXTERIOR,
                        Location::INTERIOR, Location::EXTERIOR);
  }

  // Alternating multi-corner area A with B lines added after A:
  // propagateSideLocations alternates between INTERIOR (QI, QIII) and EXTERIOR
  // (QII, QIV).
  {
    RelateNode node(&node_pt);
    const NodeSection a_q1 = MakeSection(kGeomA, kDimA, &east, node_pt, &north);
    const NodeSection a_q3 = MakeSection(kGeomA, kDimA, &west, node_pt, &south);
    const NodeSection b_diag_1 = MakeSection(kGeomB, kDimL, &sw, node_pt, &ne);
    const NodeSection b_diag_2 = MakeSection(kGeomB, kDimL, &se, node_pt, &nw);
    node.addEdges(&a_q1);
    node.addEdges(&a_q3);
    node.addEdges(&b_diag_1);
    node.addEdges(&b_diag_2);
    node.finish(/*isAreaInteriorA=*/false, /*isAreaInteriorB=*/false);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 8u);
    // 0: east, 1: ne, 2: north, 3: nw, 4: west, 5: sw, 6: south, 7: se
    ExpectEdgeLocations(*edges[1], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);
    ExpectEdgeLocations(*edges[3], kGeomA, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);
    ExpectEdgeLocations(*edges[5], kGeomA, Location::INTERIOR,
                        Location::INTERIOR, Location::INTERIOR);
    ExpectEdgeLocations(*edges[7], kGeomA, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);

    ExpectEdgeLocations(*edges[0], kGeomB, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);
    ExpectEdgeLocations(*edges[2], kGeomB, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);
    ExpectEdgeLocations(*edges[4], kGeomB, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);
    ExpectEdgeLocations(*edges[6], kGeomB, Location::EXTERIOR,
                        Location::EXTERIOR, Location::EXTERIOR);
  }

  // Full 360-degree coverage of A via two touching half-planes leaves no
  // exterior edges for A even with finish(false, false).
  {
    RelateNode node(&node_pt);
    const NodeSection top = MakeSection(kGeomA, kDimA, &east, node_pt, &west);
    const NodeSection bottom =
        MakeSection(kGeomA, kDimA, &west, node_pt, &east);
    const NodeSection b_line = MakeSection(kGeomB, kDimL, &sw, node_pt, &ne);
    node.addEdges(&top);
    node.addEdges(&bottom);
    node.addEdges(&b_line);
    node.finish(/*isAreaInteriorA=*/false, /*isAreaInteriorB=*/false);

    for (const auto& edge : node.getEdges()) {
      ExpectEdgeLocations(*edge, kGeomA, Location::INTERIOR, Location::INTERIOR,
                          Location::INTERIOR);
    }
    EXPECT_FALSE(node.hasExteriorEdge(kGeomA));
    EXPECT_TRUE(node.hasExteriorEdge(kGeomB));
  }
}

TEST(RelateNodeTest, HasExteriorEdgeChecksNoneLeftRightAndInterior) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY east(1.0, 0.0);
  const CoordinateXY north(0.0, 1.0);

  // Edges unknown for A -> no exterior edge for A, line B has LEFT=EXTERIOR.
  RelateNode node(&node_pt);
  const NodeSection b_line = MakeSection(kGeomB, kDimL, &east, node_pt, &north);
  node.addEdges(&b_line);
  EXPECT_FALSE(node.hasExteriorEdge(kGeomA));
  EXPECT_TRUE(node.hasExteriorEdge(kGeomB));

  // Area A entering edge at index 0 has LEFT=INTERIOR, RIGHT=EXTERIOR.
  RelateNode area_node(&node_pt);
  const NodeSection a_area = MakeSection(kGeomA, kDimA, &east, node_pt, &north);
  area_node.addEdges(&a_area);
  ASSERT_EQ(area_node.getEdges().size(), 2u);
  ExpectEdgeLocations(*area_node.getEdges()[0], kGeomA, Location::INTERIOR,
                      Location::BOUNDARY, Location::EXTERIOR);
  EXPECT_TRUE(area_node.hasExteriorEdge(kGeomA));

  area_node.finish(/*isAreaInteriorA=*/true, /*isAreaInteriorB=*/true);
  EXPECT_FALSE(area_node.hasExteriorEdge(kGeomA));
}

TEST(RelateNodeTest, ToStringAndStreamOperatorFormatNodeAndEdges) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY east(1.0, 0.0);
  const CoordinateXY north(0.0, 1.0);
  const CoordinateXY west(-1.0, 0.0);

  RelateNode node(&node_pt);
  const NodeSection a_line = MakeSection(kGeomA, kDimL, &west, node_pt, &east);
  const NodeSection b_area = MakeSection(kGeomB, kDimA, &east, node_pt, &north);
  node.addEdges(&a_line);
  node.addEdges(&b_area);
  node.finish(/*isAreaInteriorA=*/false, /*isAreaInteriorB=*/false);

  const std::string expected =
      "Node[POINT (0 0 )]:\n"
      "LINESTRING (0 0, 1 0) - A:eie/B:ibe\n"
      "LINESTRING (0 0, 0 1) - A:eee/B:ebi\n"
      "LINESTRING (0 0, -1 0) - A:eie/B:eee\n";
  EXPECT_EQ(node.toString(), expected);

  std::ostringstream oss;
  oss << node;
  EXPECT_EQ(oss.str(), expected);
}

}  // namespace
}  // namespace geos_google
