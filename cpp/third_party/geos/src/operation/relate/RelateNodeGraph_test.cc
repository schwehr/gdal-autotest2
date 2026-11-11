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

// Unit tests for third_party/geos/src/operation/relate/RelateNodeGraph.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD.
// - Each test allocates its own GeometryFactory::create() for isolation.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/relate/RelateNodeGraph.cpp

#include "third_party/geos/include/geos/operation/relate/RelateNodeGraph.h"

#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEndStar.h"
#include "third_party/geos/include/geos/geomgraph/GeometryGraph.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/NodeMap.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relate/EdgeEndBuilder.h"
#include "third_party/geos/include/geos/operation/relate/EdgeEndBundle.h"
#include "third_party/geos/include/geos/operation/relate/RelateNode.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geomgraph::EdgeEnd;
using geos::geomgraph::GeometryGraph;
using geos::geomgraph::NodeMap;
using geos::io::WKTReader;
using geos::operation::relate::EdgeEndBuilder;
using geos::operation::relate::EdgeEndBundle;
using geos::operation::relate::RelateNode;
using geos::operation::relate::RelateNodeGraph;

RelateNode* FindRelateNode(RelateNodeGraph& graph, double x, double y) {
  Coordinate target(x, y);
  NodeMap::container& node_map = graph.getNodeMap();
  auto it = node_map.find(&target);
  if (it == node_map.end()) {
    return nullptr;
  }
  return dynamic_cast<RelateNode*>(it->second.get());
}

TEST(RelateNodeGraphTest, DefaultConstructorStartsEmpty) {
  auto graph = std::make_unique<RelateNodeGraph>();
  EXPECT_TRUE(graph->getNodeMap().empty());
  graph.reset();
}

TEST(RelateNodeGraphTest, BuildOnEmptyGeometriesProducesNoNodes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr std::array<const char*, 4> kEmptyWkts = {
      "POINT EMPTY",
      "LINESTRING EMPTY",
      "POLYGON EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
  };
  for (const char* wkt : kEmptyWkts) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr geom = reader.read(wkt);
    GeometryGraph geom_graph(0, geom.get());
    RelateNodeGraph relate_graph;

    relate_graph.build(&geom_graph);
    EXPECT_TRUE(relate_graph.getNodeMap().empty());
  }
}

TEST(RelateNodeGraphTest, BuildOnPointAndMultiPointAddsIsolatedInteriorNodes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr geom = reader.read("MULTIPOINT ((1 2), (3 4), (1 2))");

  GeometryGraph geom_graph(0, geom.get());
  RelateNodeGraph relate_graph;
  relate_graph.build(&geom_graph);

  EXPECT_EQ(relate_graph.getNodeMap().size(), 2);

  constexpr std::array<std::pair<double, double>, 2> kExpectedCoords = {{
      {1.0, 2.0},
      {3.0, 4.0},
  }};
  for (const auto& [x, y] : kExpectedCoords) {
    RelateNode* node = FindRelateNode(relate_graph, x, y);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->getLabel().getLocation(0), Location::INTERIOR);
    EXPECT_EQ(node->getLabel().getLocation(1), Location::NONE);
    EXPECT_TRUE(node->isIsolated());
    EXPECT_EQ(node->getEdges()->getDegree(), 0);
  }
}

TEST(RelateNodeGraphTest,
     BuildOnOpenLineStringAddsBoundaryEndpointsAndEdgeEnds) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr line = reader.read("LINESTRING (0 0, 5 5, 10 0)");

  GeometryGraph geom_graph(0, line.get());
  RelateNodeGraph relate_graph;
  relate_graph.build(&geom_graph);

  EXPECT_EQ(relate_graph.getNodeMap().size(), 2);

  RelateNode* start = FindRelateNode(relate_graph, 0, 0);
  ASSERT_NE(start, nullptr);
  EXPECT_EQ(start->getLabel().getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(start->getEdges()->getDegree(), 1);

  RelateNode* end = FindRelateNode(relate_graph, 10, 0);
  ASSERT_NE(end, nullptr);
  EXPECT_EQ(end->getLabel().getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(end->getEdges()->getDegree(), 1);

  EXPECT_EQ(FindRelateNode(relate_graph, 5, 5), nullptr);
}

TEST(RelateNodeGraphTest, BuildOnClosedLineStringRespectsBoundaryNodeRule) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr ring = reader.read("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");

  {
    GeometryGraph mod2_graph(0, ring.get(),
                             BoundaryNodeRule::getBoundaryOGCSFS());
    RelateNodeGraph relate_graph;
    relate_graph.build(&mod2_graph);

    ASSERT_EQ(relate_graph.getNodeMap().size(), 1);
    RelateNode* seam = FindRelateNode(relate_graph, 0, 0);
    ASSERT_NE(seam, nullptr);
    EXPECT_EQ(seam->getLabel().getLocation(0), Location::INTERIOR);
    EXPECT_EQ(seam->getEdges()->getDegree(), 2);
  }

  {
    GeometryGraph ep_graph(0, ring.get(),
                           BoundaryNodeRule::getBoundaryEndPoint());
    RelateNodeGraph relate_graph;
    relate_graph.build(&ep_graph);

    ASSERT_EQ(relate_graph.getNodeMap().size(), 1);
    RelateNode* seam = FindRelateNode(relate_graph, 0, 0);
    ASSERT_NE(seam, nullptr);
    EXPECT_EQ(seam->getLabel().getLocation(0), Location::BOUNDARY);
    EXPECT_EQ(seam->getEdges()->getDegree(), 2);
  }
}

TEST(RelateNodeGraphTest, BuildOnSelfCrossingLineStringNodesIntersection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr bowtie = reader.read("LINESTRING (0 0, 10 10, 10 0, 0 10)");

  GeometryGraph geom_graph(0, bowtie.get());
  LineIntersector li;
  geom_graph.computeSelfNodes(&li, false);

  RelateNodeGraph relate_graph;
  relate_graph.build(&geom_graph);

  // Endpoints (0, 0) and (0, 10) plus self-intersection at (5, 5).
  EXPECT_EQ(relate_graph.getNodeMap().size(), 3);

  RelateNode* crossing = FindRelateNode(relate_graph, 5, 5);
  ASSERT_NE(crossing, nullptr);
  EXPECT_EQ(crossing->getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(crossing->getEdges()->getDegree(), 4);

  RelateNode* start = FindRelateNode(relate_graph, 0, 0);
  ASSERT_NE(start, nullptr);
  EXPECT_EQ(start->getLabel().getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(start->getEdges()->getDegree(), 1);

  RelateNode* end = FindRelateNode(relate_graph, 0, 10);
  ASSERT_NE(end, nullptr);
  EXPECT_EQ(end->getLabel().getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(end->getEdges()->getDegree(), 1);
}

TEST(RelateNodeGraphTest,
     BuildOverridesIntersectionLabelWithGeometryGraphLabel) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  // (5, 0) is an interior intersection along the first line, but a degree-1
  // endpoint of the second line, so its GeometryGraph label is BOUNDARY.
  Geometry::Ptr t_junction =
      reader.read("MULTILINESTRING ((0 0, 10 0), (5 0, 5 5))");

  GeometryGraph geom_graph(0, t_junction.get());
  LineIntersector li;
  geom_graph.computeSelfNodes(&li, false);

  RelateNodeGraph relate_graph;
  relate_graph.computeIntersectionNodes(&geom_graph, 0);
  RelateNode* before_copy = FindRelateNode(relate_graph, 5, 0);
  ASSERT_NE(before_copy, nullptr);
  EXPECT_EQ(before_copy->getLabel().getLocation(0), Location::INTERIOR);

  relate_graph.copyNodesAndLabels(&geom_graph, 0);
  RelateNode* after_copy = FindRelateNode(relate_graph, 5, 0);
  ASSERT_NE(after_copy, nullptr);
  EXPECT_EQ(after_copy->getLabel().getLocation(0), Location::BOUNDARY);

  // Full build inserts EdgeEnds for all 3 rays leaving (5, 0).
  RelateNodeGraph built_graph;
  built_graph.build(&geom_graph);
  RelateNode* built_junction = FindRelateNode(built_graph, 5, 0);
  ASSERT_NE(built_junction, nullptr);
  EXPECT_EQ(built_junction->getLabel().getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(built_junction->getEdges()->getDegree(), 3);
}

TEST(RelateNodeGraphTest, ComputeIntersectionNodesHandlesBoundaryEdgesAndFlip) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  // Shell and hole meet at vertex (5, 0). Use (3, 3) as hole start so (5, 0)
  // appears once in the shell intersection list and once in the hole's.
  Geometry::Ptr poly = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (3 3, 5 0, 7 3, 3 3))");

  GeometryGraph geom_graph(0, poly.get());
  LineIntersector li;
  geom_graph.computeSelfNodes(&li, false);

  RelateNodeGraph relate_graph;
  relate_graph.computeIntersectionNodes(&geom_graph, 0);

  // Two boundary edge intersections at (5, 0) toggle NONE -> BOUNDARY ->
  // INTERIOR via setLabelBoundary().
  RelateNode* touch_node = FindRelateNode(relate_graph, 5, 0);
  ASSERT_NE(touch_node, nullptr);
  EXPECT_EQ(touch_node->getLabel().getLocation(0), Location::INTERIOR);

  // copyNodesAndLabels restores the parent GeometryGraph BOUNDARY label.
  relate_graph.copyNodesAndLabels(&geom_graph, 0);
  EXPECT_EQ(touch_node->getLabel().getLocation(0), Location::BOUNDARY);

  // Full build attaches 4 EdgeEnds around (5, 0) (2 from shell, 2 from hole).
  RelateNodeGraph full_graph;
  full_graph.build(&geom_graph);
  RelateNode* full_touch = FindRelateNode(full_graph, 5, 0);
  ASSERT_NE(full_touch, nullptr);
  EXPECT_EQ(full_touch->getLabel().getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(full_touch->getEdges()->getDegree(), 4);
}

TEST(RelateNodeGraphTest,
     ComputeIntersectionNodesPreservesExistingLabelAtArgIndex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr t_junction =
      reader.read("MULTILINESTRING ((0 0, 10 0), (5 0, 5 5))");

  GeometryGraph graph1(1, t_junction.get());
  LineIntersector li;
  graph1.computeSelfNodes(&li, false);

  RelateNodeGraph relate_graph;
  // Copy BOUNDARY label at (5, 0) for argIndex = 1 first.
  relate_graph.copyNodesAndLabels(&graph1, 1);
  RelateNode* junction = FindRelateNode(relate_graph, 5, 0);
  ASSERT_NE(junction, nullptr);
  EXPECT_EQ(junction->getLabel().getLocation(1), Location::BOUNDARY);

  // Running computeIntersectionNodes afterward sees !isNull(1) and keeps
  // BOUNDARY.
  relate_graph.computeIntersectionNodes(&graph1, 1);
  EXPECT_EQ(junction->getLabel().getLocation(1), Location::BOUNDARY);
  EXPECT_EQ(junction->getLabel().getLocation(0), Location::NONE);
}

TEST(RelateNodeGraphTest, ComputesAndCopiesLabelsForBothArgIndices) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr line = reader.read("LINESTRING (0 5, 10 5)");
  Geometry::Ptr poly = reader.read("POLYGON ((2 0, 8 0, 8 10, 2 10, 2 0))");

  GeometryGraph graph0(0, line.get());
  GeometryGraph graph1(1, poly.get());
  LineIntersector li;
  graph0.computeEdgeIntersections(&graph1, &li, true);

  RelateNodeGraph relate_graph;
  relate_graph.computeIntersectionNodes(&graph0, 0);
  relate_graph.computeIntersectionNodes(&graph1, 1);
  relate_graph.copyNodesAndLabels(&graph0, 0);
  relate_graph.copyNodesAndLabels(&graph1, 1);

  RelateNode* enter = FindRelateNode(relate_graph, 2, 5);
  ASSERT_NE(enter, nullptr);
  EXPECT_EQ(enter->getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(enter->getLabel().getLocation(1), Location::BOUNDARY);
  EXPECT_FALSE(enter->isIsolated());

  RelateNode* exit_pt = FindRelateNode(relate_graph, 8, 5);
  ASSERT_NE(exit_pt, nullptr);
  EXPECT_EQ(exit_pt->getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(exit_pt->getLabel().getLocation(1), Location::BOUNDARY);

  RelateNode* line_start = FindRelateNode(relate_graph, 0, 5);
  ASSERT_NE(line_start, nullptr);
  EXPECT_EQ(line_start->getLabel().getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(line_start->getLabel().getLocation(1), Location::NONE);

  RelateNode* poly_vertex = FindRelateNode(relate_graph, 2, 0);
  ASSERT_NE(poly_vertex, nullptr);
  EXPECT_EQ(poly_vertex->getLabel().getLocation(0), Location::NONE);
  EXPECT_EQ(poly_vertex->getLabel().getLocation(1), Location::BOUNDARY);
}

TEST(RelateNodeGraphTest, CopyNodesAndLabelsOverridesBoundaryWithInterior) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr poly = reader.read("POLYGON ((2 0, 8 0, 8 10, 2 10, 2 0))");
  Geometry::Ptr line = reader.read("LINESTRING (0 5, 10 5)");
  // Two segments sharing (2, 5) produce an INTERIOR node at (2, 5) under Mod-2.
  Geometry::Ptr mls = reader.read("MULTILINESTRING ((0 5, 2 5), (2 5, 5 5))");

  GeometryGraph poly_graph(0, poly.get());
  GeometryGraph line_graph(1, line.get());
  LineIntersector li;
  poly_graph.computeEdgeIntersections(&line_graph, &li, true);

  RelateNodeGraph relate_graph;
  relate_graph.computeIntersectionNodes(&poly_graph, 0);
  RelateNode* node = FindRelateNode(relate_graph, 2, 5);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->getLabel().getLocation(0), Location::BOUNDARY);

  GeometryGraph mls_graph(0, mls.get());
  relate_graph.copyNodesAndLabels(&mls_graph, 0);
  EXPECT_EQ(node->getLabel().getLocation(0), Location::INTERIOR);
}

TEST(RelateNodeGraphTest, InsertEdgeEndsHandlesEmptyAndParallelBundles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr overlapping =
      reader.read("MULTILINESTRING ((0 0, 10 0), (0 0, 5 0))");

  GeometryGraph geom_graph(0, overlapping.get());
  RelateNodeGraph relate_graph;

  std::vector<std::unique_ptr<EdgeEnd>> empty_list;
  relate_graph.insertEdgeEnds(empty_list);
  EXPECT_TRUE(relate_graph.getNodeMap().empty());

  EdgeEndBuilder ee_builder;
  std::vector<std::unique_ptr<EdgeEnd>> edge_ends =
      ee_builder.computeEdgeEnds(geom_graph.getEdges());
  ASSERT_EQ(edge_ends.size(), 4);

  relate_graph.insertEdgeEnds(edge_ends);
  EXPECT_EQ(relate_graph.getNodeMap().size(), 3);

  RelateNode* origin = FindRelateNode(relate_graph, 0, 0);
  ASSERT_NE(origin, nullptr);
  // Labels were not copied via copyNodesAndLabels, so node label remains NONE.
  EXPECT_TRUE(origin->getLabel().isNull());
  // Both EdgeEnds from (0, 0) along +x share one EdgeEndBundle.
  EXPECT_EQ(origin->getEdges()->getDegree(), 1);
  auto* bundle = dynamic_cast<EdgeEndBundle*>(*origin->getEdges()->begin());
  ASSERT_NE(bundle, nullptr);
  EXPECT_EQ(bundle->getEdgeEnds().size(), 2);

  EXPECT_NE(FindRelateNode(relate_graph, 5, 0), nullptr);
  EXPECT_NE(FindRelateNode(relate_graph, 10, 0), nullptr);
}

}  // namespace
}  // namespace geos_google
