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

#include "third_party/geos/include/geos/operation/relateng/NodeSections.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relateng/NodeSection.h"
#include "third_party/geos/include/geos/operation/relateng/RelateEdge.h"
#include "third_party/geos/include/geos/operation/relateng/RelateNode.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::geom::Position;
using geos::io::WKTReader;
using geos::operation::relateng::NodeSection;
using geos::operation::relateng::NodeSections;
using geos::operation::relateng::RelateEdge;
using geos::operation::relateng::RelateNode;

constexpr int kDimL = Dimension::L;
constexpr int kDimA = Dimension::A;
constexpr int kPosOn = Position::ON;
constexpr int kPosLeft = Position::LEFT;
constexpr int kPosRight = Position::RIGHT;

void ExpectEdgeLocations(const RelateEdge& edge, bool is_a, Location left,
                         Location on, Location right) {
  EXPECT_EQ(edge.location(is_a, kPosLeft), left);
  EXPECT_EQ(edge.location(is_a, kPosOn), on);
  EXPECT_EQ(edge.location(is_a, kPosRight), right);
}

class NodeSectionsTest : public ::testing::Test {
 protected:
  NodeSectionsTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Polygon> ReadPoly(const std::string& wkt) const {
    return reader_.read<Polygon>(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(NodeSectionsTest, EmptyNodeSectionsPropertiesAndCreateNode) {
  const CoordinateXY pt(1.0, 2.0);
  NodeSections nss(&pt);

  EXPECT_EQ(nss.getCoordinate(), &pt);
  EXPECT_FALSE(nss.hasInteractionAB());
  EXPECT_EQ(nss.getPolygonal(true), nullptr);
  EXPECT_EQ(nss.getPolygonal(false), nullptr);

  std::unique_ptr<RelateNode> node = nss.createNode();
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->getCoordinate(), &pt);
  EXPECT_TRUE(node->getEdges().empty());
}

TEST_F(NodeSectionsTest, HasInteractionABAllCombinations) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY v0(1.0, 0.0);
  const CoordinateXY v1(-1.0, 0.0);

  {
    NodeSections only_a(&node_pt);
    only_a.addNodeSection(
        new NodeSection(true, kDimL, 0, 0, nullptr, true, &v0, node_pt, &v1));
    only_a.addNodeSection(
        new NodeSection(true, kDimL, 1, 0, nullptr, true, &v1, node_pt, &v0));
    EXPECT_FALSE(only_a.hasInteractionAB());
  }
  {
    NodeSections only_b(&node_pt);
    only_b.addNodeSection(
        new NodeSection(false, kDimL, 0, 0, nullptr, true, &v0, node_pt, &v1));
    only_b.addNodeSection(
        new NodeSection(false, kDimL, 1, 0, nullptr, true, &v1, node_pt, &v0));
    EXPECT_FALSE(only_b.hasInteractionAB());
  }
  {
    NodeSections a_then_b(&node_pt);
    a_then_b.addNodeSection(
        new NodeSection(true, kDimL, 0, 0, nullptr, true, &v0, node_pt, &v1));
    a_then_b.addNodeSection(
        new NodeSection(false, kDimL, 0, 0, nullptr, true, &v0, node_pt, &v1));
    EXPECT_TRUE(a_then_b.hasInteractionAB());
  }
  {
    NodeSections b_then_a(&node_pt);
    b_then_a.addNodeSection(
        new NodeSection(false, kDimL, 0, 0, nullptr, true, &v0, node_pt, &v1));
    b_then_a.addNodeSection(
        new NodeSection(true, kDimL, 0, 0, nullptr, true, &v0, node_pt, &v1));
    EXPECT_TRUE(b_then_a.hasInteractionAB());
  }
}

TEST_F(NodeSectionsTest, GetPolygonalSkipsWrongGeomAndNullPolygons) {
  std::unique_ptr<Polygon> poly_a =
      ReadPoly("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::unique_ptr<Polygon> poly_b =
      ReadPoly("POLYGON ((0 0, -10 0, -10 10, 0 10, 0 0))");

  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY va0(10.0, 0.0);
  const CoordinateXY va1(0.0, 10.0);
  const CoordinateXY vb0(0.0, 10.0);
  const CoordinateXY vb1(-10.0, 0.0);

  NodeSections nss(&node_pt);
  // Null polygon on A and non-null polygon on B.
  nss.addNodeSection(
      new NodeSection(true, kDimL, -1, 0, nullptr, true, &va0, node_pt, &va1));
  nss.addNodeSection(new NodeSection(false, kDimA, 0, 0, poly_b.get(), true,
                                     &vb0, node_pt, &vb1));
  EXPECT_EQ(nss.getPolygonal(true), nullptr);
  EXPECT_EQ(nss.getPolygonal(false),
            static_cast<const Geometry*>(poly_b.get()));

  // Subsequent area section on A is found after skipping null line section.
  nss.addNodeSection(new NodeSection(true, kDimA, 0, 0, poly_a.get(), true,
                                     &va0, node_pt, &va1));
  EXPECT_EQ(nss.getPolygonal(true), static_cast<const Geometry*>(poly_a.get()));
  EXPECT_EQ(nss.getPolygonal(false),
            static_cast<const Geometry*>(poly_b.get()));
}

TEST_F(NodeSectionsTest, CreateNodeLineSectionsIncludingSameIdLines) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY east(5.0, 0.0);
  const CoordinateXY north(0.0, 5.0);
  const CoordinateXY west(-5.0, 0.0);
  const CoordinateXY south(0.0, -5.0);

  NodeSections nss(&node_pt);
  // Two line sections sharing (isA=true, id=0) must not trigger polygon
  // conversion because ns->isArea() is false.
  nss.addNodeSection(new NodeSection(true, kDimL, 0, 0, nullptr, true, nullptr,
                                     node_pt, &east));
  nss.addNodeSection(new NodeSection(true, kDimL, 0, 0, nullptr, false, &north,
                                     node_pt, &south));
  nss.addNodeSection(new NodeSection(false, kDimL, 0, 0, nullptr, false, &east,
                                     node_pt, &west));

  EXPECT_TRUE(nss.hasInteractionAB());
  EXPECT_EQ(nss.getPolygonal(true), nullptr);
  EXPECT_EQ(nss.getPolygonal(false), nullptr);

  std::unique_ptr<RelateNode> node = nss.createNode();
  ASSERT_NE(node, nullptr);
  const auto& edges = node->getEdges();
  ASSERT_EQ(edges.size(), 4u);

  // Before finish: east (index 0) is shared by A and B; north/south are A-only;
  // west is B-only.
  ExpectEdgeLocations(*edges[0], true, Location::EXTERIOR, Location::INTERIOR,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[0], false, Location::EXTERIOR, Location::INTERIOR,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[1], true, Location::EXTERIOR, Location::INTERIOR,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[1], false, Location::NONE, Location::NONE,
                      Location::NONE);

  node->finish(false, false);
  ExpectEdgeLocations(*edges[1], false, Location::EXTERIOR, Location::EXTERIOR,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[2], true, Location::EXTERIOR, Location::EXTERIOR,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[2], false, Location::EXTERIOR, Location::INTERIOR,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[3], true, Location::EXTERIOR, Location::INTERIOR,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[3], false, Location::EXTERIOR, Location::EXTERIOR,
                      Location::EXTERIOR);
}

TEST_F(NodeSectionsTest, CreateNodeUnsortedSinglePolygonSectionsAcrossIds) {
  std::unique_ptr<Polygon> poly_a0 =
      ReadPoly("POLYGON ((0 0, 10 1, 10 10, 1 10, 0 0))");
  std::unique_ptr<Polygon> poly_a1 =
      ReadPoly("POLYGON ((0 0, -1 -10, -10 -10, -10 -1, 0 0))");
  std::unique_ptr<Polygon> poly_b0 =
      ReadPoly("POLYGON ((0 0, -10 1, -10 10, 0 10, 0 0))");

  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY a0_in(10.0, 1.0);
  const CoordinateXY a0_out(1.0, 10.0);
  const CoordinateXY a1_in(-10.0, -1.0);
  const CoordinateXY a1_out(-1.0, -10.0);
  const CoordinateXY b0_in(0.0, 10.0);
  const CoordinateXY b0_out(-10.0, 1.0);
  const CoordinateXY a_line_dir(1.0, -10.0);

  // Add sections out of order: B area, A id=1 area, A id=0 area, A line.
  // After sorting: A line, A id=0 area, A id=1 area, B id=0 area.
  // Tests hasMultiplePolygonSections returning false both on !isSamePolygon
  // and at the final index.
  NodeSections nss(&node_pt);
  nss.addNodeSection(new NodeSection(false, kDimA, 0, 0, poly_b0.get(), true,
                                     &b0_in, node_pt, &b0_out));
  nss.addNodeSection(new NodeSection(true, kDimA, 1, 0, poly_a1.get(), true,
                                     &a1_in, node_pt, &a1_out));
  nss.addNodeSection(new NodeSection(true, kDimA, 0, 0, poly_a0.get(), true,
                                     &a0_in, node_pt, &a0_out));
  nss.addNodeSection(new NodeSection(true, kDimL, -1, 0, nullptr, true,
                                     &a_line_dir, node_pt, nullptr));

  std::unique_ptr<RelateNode> node = nss.createNode();
  ASSERT_NE(node, nullptr);
  const auto& edges = node->getEdges();
  ASSERT_EQ(edges.size(), 7u);

  node->finish(false, false);
  // CCW edge order:
  // 0: (10,1) A0 in, 1: (1,10) A0 out, 2: (0,10) B0 in, 3: (-10,1) B0 out,
  // 4: (-10,-1) A1 in, 5: (-1,-10) A1 out, 6: (1,-10) A line.
  ExpectEdgeLocations(*edges[0], true, Location::INTERIOR, Location::BOUNDARY,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[1], true, Location::EXTERIOR, Location::BOUNDARY,
                      Location::INTERIOR);
  ExpectEdgeLocations(*edges[2], false, Location::INTERIOR, Location::BOUNDARY,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[3], false, Location::EXTERIOR, Location::BOUNDARY,
                      Location::INTERIOR);
  ExpectEdgeLocations(*edges[4], true, Location::INTERIOR, Location::BOUNDARY,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[5], true, Location::EXTERIOR, Location::BOUNDARY,
                      Location::INTERIOR);
  ExpectEdgeLocations(*edges[6], true, Location::EXTERIOR, Location::INTERIOR,
                      Location::EXTERIOR);
}

TEST_F(NodeSectionsTest, CreateNodeConvertsShellAndHoleOfSamePolygonAtEnd) {
  std::unique_ptr<Polygon> poly_a =
      ReadPoly("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (0 0, 5 2, 2 5, 0 0))");

  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY shell_in(10.0, 0.0);
  const CoordinateXY shell_out(0.0, 10.0);
  const CoordinateXY hole_in(2.0, 5.0);
  const CoordinateXY hole_out(5.0, 2.0);

  // Add hole before shell so prepareSections orders shell (ring 0) first.
  NodeSections nss(&node_pt);
  nss.addNodeSection(new NodeSection(true, kDimA, 0, 1, poly_a.get(), true,
                                     &hole_in, node_pt, &hole_out));
  nss.addNodeSection(new NodeSection(true, kDimA, 0, 0, poly_a.get(), true,
                                     &shell_in, node_pt, &shell_out));

  std::unique_ptr<RelateNode> node = nss.createNode();
  ASSERT_NE(node, nullptr);
  const auto& edges = node->getEdges();
  ASSERT_EQ(edges.size(), 4u);

  // Converted maximal-ring corners enclose [0 deg, 21.8 deg] and [68.2 deg, 90
  // deg], leaving the hole [21.8 deg, 68.2 deg] as EXTERIOR for A.
  ExpectEdgeLocations(*edges[0], true, Location::INTERIOR, Location::BOUNDARY,
                      Location::EXTERIOR);  // (10, 0)
  ExpectEdgeLocations(*edges[1], true, Location::EXTERIOR, Location::BOUNDARY,
                      Location::INTERIOR);  // (5, 2)
  ExpectEdgeLocations(*edges[2], true, Location::INTERIOR, Location::BOUNDARY,
                      Location::EXTERIOR);  // (2, 5)
  ExpectEdgeLocations(*edges[3], true, Location::EXTERIOR, Location::BOUNDARY,
                      Location::INTERIOR);  // (0, 10)
}

TEST_F(NodeSectionsTest, CreateNodeConvertsMultipleHolesFollowedByOtherGeom) {
  std::unique_ptr<Polygon> poly_a = ReadPoly(
      "POLYGON ((-10 -10, -10 10, 10 10, 10 -10, -10 -10), "
      "(0 0, 5 2, 2 5, 0 0), (0 0, -5 -2, -2 -5, 0 0))");
  std::unique_ptr<Polygon> poly_b =
      ReadPoly("POLYGON ((0 0, -10 0, -10 10, 0 10, 0 0))");

  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY h1_in(2.0, 5.0);
  const CoordinateXY h1_out(5.0, 2.0);
  const CoordinateXY h2_in(-2.0, -5.0);
  const CoordinateXY h2_out(-5.0, -2.0);
  const CoordinateXY b_in(0.0, 10.0);
  const CoordinateXY b_out(-10.0, 0.0);
  const CoordinateXY b_line_dir(0.0, -10.0);

  // Two holes from polygon A (id=0) followed by B sections so
  // collectPolygonSections stops at !isSamePolygon before sections.size().
  NodeSections nss(&node_pt);
  nss.addNodeSection(new NodeSection(false, kDimA, 0, 0, poly_b.get(), true,
                                     &b_in, node_pt, &b_out));
  nss.addNodeSection(new NodeSection(true, kDimA, 0, 2, poly_a.get(), true,
                                     &h2_in, node_pt, &h2_out));
  nss.addNodeSection(new NodeSection(false, kDimL, -1, 0, nullptr, true,
                                     &b_line_dir, node_pt, nullptr));
  nss.addNodeSection(new NodeSection(true, kDimA, 0, 1, poly_a.get(), true,
                                     &h1_in, node_pt, &h1_out));

  EXPECT_TRUE(nss.hasInteractionAB());
  EXPECT_EQ(nss.getPolygonal(true), static_cast<const Geometry*>(poly_a.get()));
  EXPECT_EQ(nss.getPolygonal(false),
            static_cast<const Geometry*>(poly_b.get()));

  std::unique_ptr<RelateNode> node = nss.createNode();
  ASSERT_NE(node, nullptr);
  const auto& edges = node->getEdges();
  ASSERT_EQ(edges.size(), 7u);

  node->finish(false, false);
  // CCW edge order:
  // 0: (5, 2) h1 out, 1: (2, 5) h1 in, 2: (0, 10) B in, 3: (-10, 0) B out,
  // 4: (-5, -2) h2 out, 5: (-2, -5) h2 in, 6: (0, -10) B line.
  ExpectEdgeLocations(*edges[0], true, Location::EXTERIOR, Location::BOUNDARY,
                      Location::INTERIOR);
  ExpectEdgeLocations(*edges[0], false, Location::EXTERIOR, Location::EXTERIOR,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[1], true, Location::INTERIOR, Location::BOUNDARY,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[2], true, Location::INTERIOR, Location::INTERIOR,
                      Location::INTERIOR);
  ExpectEdgeLocations(*edges[2], false, Location::INTERIOR, Location::BOUNDARY,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[3], true, Location::INTERIOR, Location::INTERIOR,
                      Location::INTERIOR);
  ExpectEdgeLocations(*edges[3], false, Location::EXTERIOR, Location::BOUNDARY,
                      Location::INTERIOR);
  ExpectEdgeLocations(*edges[4], true, Location::EXTERIOR, Location::BOUNDARY,
                      Location::INTERIOR);
  ExpectEdgeLocations(*edges[5], true, Location::INTERIOR, Location::BOUNDARY,
                      Location::EXTERIOR);
  ExpectEdgeLocations(*edges[6], true, Location::INTERIOR, Location::INTERIOR,
                      Location::INTERIOR);
  ExpectEdgeLocations(*edges[6], false, Location::EXTERIOR, Location::INTERIOR,
                      Location::EXTERIOR);
  EXPECT_TRUE(node->hasExteriorEdge(true));
  EXPECT_TRUE(node->hasExteriorEdge(false));
}

}  // namespace
}  // namespace geos_google
