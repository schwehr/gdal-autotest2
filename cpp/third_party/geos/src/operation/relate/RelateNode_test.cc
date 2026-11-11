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

#include "third_party/geos/include/geos/operation/relate/RelateNode.h"

#include <array>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/GeometryGraph.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relate/EdgeEndBuilder.h"
#include "third_party/geos/include/geos/operation/relate/EdgeEndBundleStar.h"
#include "third_party/geos/include/geos/operation/relate/RelateNodeFactory.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Dimension;
using geos::geom::GeometryFactory;
using geos::geom::IntersectionMatrix;
using geos::geom::Location;
using geos::geom::Position;
using geos::geomgraph::EdgeEnd;
using geos::geomgraph::GeometryGraph;
using geos::geomgraph::Label;
using geos::geomgraph::Node;
using geos::io::WKTReader;
using geos::operation::relate::EdgeEndBuilder;
using geos::operation::relate::EdgeEndBundleStar;
using geos::operation::relate::RelateNode;
using geos::operation::relate::RelateNodeFactory;

TEST(RelateNodeTest, ConstructsAndDestroysThroughBasePointer) {
  const Coordinate coord(3.0, 4.0, 5.0);
  std::unique_ptr<Node> base_ptr =
      std::make_unique<RelateNode>(coord, new EdgeEndBundleStar());

  auto* relate_node = dynamic_cast<RelateNode*>(base_ptr.get());
  ASSERT_NE(relate_node, nullptr);
  EXPECT_TRUE(relate_node->getCoordinate().equals3D(coord));
  EXPECT_NE(dynamic_cast<EdgeEndBundleStar*>(relate_node->getEdges()), nullptr);
  EXPECT_FALSE(relate_node->isIsolated());

  relate_node->setLabel(0, Location::BOUNDARY);
  EXPECT_TRUE(relate_node->isIsolated());

  relate_node->setLabel(1, Location::INTERIOR);
  EXPECT_FALSE(relate_node->isIsolated());
}

TEST(RelateNodeTest, UpdateIMCallsComputeIMForAllValidLocationPairs) {
  constexpr std::array<Location, 3> kLocations = {
      Location::INTERIOR,
      Location::BOUNDARY,
      Location::EXTERIOR,
  };

  for (Location loc0 : kLocations) {
    for (Location loc1 : kLocations) {
      RelateNode node(Coordinate(1.0, 2.0), new EdgeEndBundleStar());
      node.setLabel(0, loc0);
      node.setLabel(1, loc1);

      IntersectionMatrix im;
      node.updateIM(im);

      for (Location row : kLocations) {
        for (Location col : kLocations) {
          const int expected =
              (row == loc0 && col == loc1) ? +Dimension::P : +Dimension::False;
          EXPECT_EQ(im.get(row, col), expected);
        }
      }
    }
  }
}

TEST(RelateNodeTest, ComputeIMDoesNotDowngradeHigherDimensions) {
  RelateNode node(Coordinate(0.0, 0.0), new EdgeEndBundleStar());
  node.setLabel(0, Location::INTERIOR);
  node.setLabel(1, Location::BOUNDARY);

  IntersectionMatrix im("F1FF0FFF2");
  node.updateIM(im);
  EXPECT_EQ(im.get(Location::INTERIOR, Location::BOUNDARY), +Dimension::L);

  im.set(Location::INTERIOR, Location::BOUNDARY, +Dimension::A);
  node.updateIM(im);
  EXPECT_EQ(im.get(Location::INTERIOR, Location::BOUNDARY), +Dimension::A);
}

TEST(RelateNodeTest, UpdateIMFromEdgesWithEmptyStarLeavesMatrixUnchanged) {
  RelateNode node(Coordinate(0.0, 0.0), new EdgeEndBundleStar());
  IntersectionMatrix im;
  node.updateIMFromEdges(im);
  EXPECT_EQ(im.toString(), "FFFFFFFFF");
}

TEST(RelateNodeTest, UpdateIMFromEdgesAppliesLineAndAreaLabels) {
  {
    RelateNode node(Coordinate(0.0, 0.0), new EdgeEndBundleStar());
    Label line_label(Location::NONE);
    line_label.setLocation(0, Location::INTERIOR);
    line_label.setLocation(1, Location::EXTERIOR);

    node.add(new EdgeEnd(nullptr, Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
                         line_label));

    IntersectionMatrix im;
    node.updateIMFromEdges(im);
    EXPECT_EQ(im.toString(), "FF1FFFFFF");
  }

  {
    RelateNode node(Coordinate(0.0, 0.0), new EdgeEndBundleStar());
    Label area_label(Location::BOUNDARY, Location::INTERIOR,
                     Location::EXTERIOR);
    area_label.setLocation(1, Position::LEFT, Location::EXTERIOR);
    area_label.setLocation(1, Position::RIGHT, Location::INTERIOR);

    node.add(new EdgeEnd(nullptr, Coordinate(0.0, 0.0), Coordinate(0.0, 1.0),
                         area_label));

    IntersectionMatrix im;
    node.updateIMFromEdges(im);
    EXPECT_EQ(im.toString(), "FF2F1F2FF");
  }
}

TEST(RelateNodeTest, AddEdgeEndWithMismatchedOriginThrowsIllegalArgument) {
  RelateNode node(Coordinate(0.0, 0.0), new EdgeEndBundleStar());
  auto bad_edge_end = std::make_unique<EdgeEnd>(nullptr, Coordinate(1.0, 1.0),
                                                Coordinate(2.0, 2.0),
                                                Label(0, Location::INTERIOR));
  EXPECT_THROW(node.add(bad_edge_end.get()),
               geos::util::IllegalArgumentException);
}

TEST(RelateNodeTest, IntegrationWithGeometryGraphAndEdgeEndBuilder) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  auto poly = reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto line = reader.read("LINESTRING (0 0, 10 10)");

  std::vector<std::unique_ptr<GeometryGraph>> graphs;
  graphs.push_back(std::make_unique<GeometryGraph>(0, poly.get()));
  graphs.push_back(std::make_unique<GeometryGraph>(1, line.get()));

  std::unique_ptr<Node> owned_node(
      RelateNodeFactory::instance().createNode(Coordinate(0.0, 0.0)));
  auto* relate_node = dynamic_cast<RelateNode*>(owned_node.get());
  ASSERT_NE(relate_node, nullptr);

  EdgeEndBuilder builder;
  auto ee0 = builder.computeEdgeEnds(graphs[0]->getEdges());
  for (auto& ee : ee0) {
    if (ee->getCoordinate().equals2D(Coordinate(0.0, 0.0))) {
      relate_node->add(ee.release());
    }
  }
  auto ee1 = builder.computeEdgeEnds(graphs[1]->getEdges());
  for (auto& ee : ee1) {
    if (ee->getCoordinate().equals2D(Coordinate(0.0, 0.0))) {
      relate_node->add(ee.release());
    }
  }

  relate_node->setLabel(0, Location::BOUNDARY);
  relate_node->setLabel(1, Location::BOUNDARY);
  relate_node->getEdges()->computeLabelling(graphs);

  IntersectionMatrix im;
  relate_node->updateIM(im);
  relate_node->updateIMFromEdges(im);

  // At (0, 0), line boundary touches polygon boundary (BB=0); line interior
  // enters polygon interior (II=1); polygon boundary edges lie in line exterior
  // (BE=1); polygon interior/exterior around node give IE=2, EE=2.
  EXPECT_EQ(im.get(Location::BOUNDARY, Location::BOUNDARY), +Dimension::P);
  EXPECT_EQ(im.get(Location::INTERIOR, Location::INTERIOR), +Dimension::L);
  EXPECT_EQ(im.get(Location::BOUNDARY, Location::EXTERIOR), +Dimension::L);
  EXPECT_EQ(im.get(Location::INTERIOR, Location::EXTERIOR), +Dimension::A);
  EXPECT_EQ(im.get(Location::EXTERIOR, Location::EXTERIOR), +Dimension::A);
}

}  // namespace
}  // namespace geos_google
