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

#include "third_party/geos/include/geos/operation/buffer/BufferSubgraph.h"

#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdge.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdgeStar.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/PlanarGraph.h"
#include "third_party/geos/include/geos/operation/buffer/BufferNodeFactory.h"
#include "third_party/geos/include/geos/operation/buffer/PolygonBuilder.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::geom::Position;
using geos::geomgraph::DirectedEdge;
using geos::geomgraph::DirectedEdgeStar;
using geos::geomgraph::Edge;
using geos::geomgraph::Label;
using geos::geomgraph::Node;
using geos::geomgraph::PlanarGraph;
using geos::operation::buffer::BufferNodeFactory;
using geos::operation::buffer::BufferSubgraph;
using geos::operation::buffer::BufferSubgraphGT;
using geos::operation::buffer::PolygonBuilder;
using geos::util::TopologyException;

Edge* MakeEdge(std::initializer_list<Coordinate> coords, const Label& label,
               int depth_delta) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  auto edge = std::make_unique<Edge>(seq.release(), label);
  edge->setDepthDelta(depth_delta);
  return edge.release();
}

Node* FindNode(PlanarGraph& graph, const Coordinate& coord) {
  Coordinate mutable_coord = coord;
  return graph.find(mutable_coord);
}

TEST(BufferSubgraphTest, EmptySubgraphStateEnvelopeCachingAndStreamOutput) {
  BufferSubgraph without_envelope;
  EXPECT_EQ(without_envelope.getRightmostCoordinate(), nullptr);
  EXPECT_TRUE(without_envelope.getNodes()->empty());
  EXPECT_TRUE(without_envelope.getDirectedEdges()->empty());

  BufferSubgraph empty;
  Envelope* env1 = empty.getEnvelope();
  ASSERT_NE(env1, nullptr);
  EXPECT_TRUE(env1->isNull());

  // Second getEnvelope() call returns cached Envelope pointer.
  Envelope* env2 = empty.getEnvelope();
  EXPECT_EQ(env2, env1);

  std::ostringstream oss;
  oss << empty;
  const std::string text = oss.str();
  EXPECT_NE(text.find("BufferSubgraph["), std::string::npos);
  EXPECT_NE(text.find("0 nodes, 0 directed edges"), std::string::npos);
}

TEST(BufferSubgraphTest, CreateThrowsOnIsolatedNodeWithoutForwardEdges) {
  PlanarGraph graph(BufferNodeFactory::instance());
  Node* isolated = graph.addNode(Coordinate(3.0, 4.0));
  ASSERT_NE(isolated, nullptr);

  BufferSubgraph subgraph;
  EXPECT_THROW(subgraph.create(isolated), TopologyException);
  EXPECT_EQ(subgraph.getNodes()->size(), 1u);
  EXPECT_TRUE(subgraph.getDirectedEdges()->empty());
}

TEST(BufferSubgraphTest, SingleClosedCwRingDepthComputationResultAndEnvelope) {
  PlanarGraph graph(BufferNodeFactory::instance());
  const Label cw_shell_label(Location::BOUNDARY, Location::EXTERIOR,
                             Location::INTERIOR);
  Edge* edge = MakeEdge(
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
       Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
      cw_shell_label, -1);
  graph.addEdges({edge});

  Node* start_node = FindNode(graph, Coordinate(0.0, 0.0));
  ASSERT_NE(start_node, nullptr);

  BufferSubgraph subgraph;
  subgraph.create(start_node);

  ASSERT_NE(subgraph.getRightmostCoordinate(), nullptr);
  EXPECT_EQ(*subgraph.getRightmostCoordinate(), Coordinate(10.0, 10.0));
  EXPECT_EQ(subgraph.getNodes()->size(), 1u);
  EXPECT_EQ(subgraph.getDirectedEdges()->size(), 2u);

  subgraph.computeDepth(0);
  subgraph.findResultEdges();

  const auto* dir_edges = subgraph.getDirectedEdges();
  ASSERT_EQ(dir_edges->size(), 2u);
  for (DirectedEdge* de : *dir_edges) {
    if (de->isForward()) {
      EXPECT_EQ(de->getDepth(Position::LEFT), 0);
      EXPECT_EQ(de->getDepth(Position::RIGHT), 1);
      EXPECT_TRUE(de->isInResult());
    } else {
      EXPECT_EQ(de->getDepth(Position::LEFT), 1);
      EXPECT_EQ(de->getDepth(Position::RIGHT), 0);
      EXPECT_FALSE(de->isInResult());
    }
  }

  Envelope* env = subgraph.getEnvelope();
  ASSERT_NE(env, nullptr);
  EXPECT_FALSE(env->isNull());
  EXPECT_DOUBLE_EQ(env->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 10.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 10.0);
  EXPECT_EQ(subgraph.getEnvelope(), env);

  std::ostringstream oss;
  oss << subgraph;
  const std::string printed = oss.str();
  EXPECT_NE(printed.find("1 nodes, 2 directed edges"), std::string::npos);
  EXPECT_NE(printed.find("Node 0:"), std::string::npos);
  EXPECT_NE(printed.find("DirEdge 0:"), std::string::npos);
  EXPECT_NE(printed.find("DirEdge 1:"), std::string::npos);
}

TEST(BufferSubgraphTest,
     MultiNodeCyclicGraphTraversesBreadthFirstAndBuildsPoly) {
  PlanarGraph graph(BufferNodeFactory::instance());
  const Label cw_label(Location::BOUNDARY, Location::EXTERIOR,
                       Location::INTERIOR);
  // Two arcs meeting at (0,5) and (10,5) exercise both node-star rightmost
  // edge lookup and breadth-first duplicate adjacent-node suppression.
  Edge* upper_arc = MakeEdge(
      {Coordinate(0.0, 5.0), Coordinate(5.0, 10.0), Coordinate(10.0, 5.0)},
      cw_label, -1);
  Edge* lower_arc = MakeEdge(
      {Coordinate(10.0, 5.0), Coordinate(5.0, 0.0), Coordinate(0.0, 5.0)},
      cw_label, -1);
  graph.addEdges({upper_arc, lower_arc});

  Node* left_node = FindNode(graph, Coordinate(0.0, 5.0));
  ASSERT_NE(left_node, nullptr);

  BufferSubgraph subgraph;
  subgraph.create(left_node);
  ASSERT_NE(subgraph.getRightmostCoordinate(), nullptr);
  EXPECT_DOUBLE_EQ(subgraph.getRightmostCoordinate()->x, 10.0);

  subgraph.computeDepth(0);
  subgraph.findResultEdges();

  int in_result_count = 0;
  for (DirectedEdge* de : *subgraph.getDirectedEdges()) {
    if (de->isForward()) {
      EXPECT_EQ(de->getDepth(Position::LEFT), 0);
      EXPECT_EQ(de->getDepth(Position::RIGHT), 1);
      EXPECT_TRUE(de->isInResult());
      ++in_result_count;
    } else {
      EXPECT_EQ(de->getDepth(Position::LEFT), 1);
      EXPECT_EQ(de->getDepth(Position::RIGHT), 0);
      EXPECT_FALSE(de->isInResult());
    }
  }
  EXPECT_GE(in_result_count, 2);

  GeometryFactory::Ptr factory = GeometryFactory::create();
  PolygonBuilder poly_builder(factory.get());
  poly_builder.add(subgraph.getDirectedEdges(), subgraph.getNodes());
  std::vector<std::unique_ptr<Geometry>> polys = poly_builder.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 50.0);
}

TEST(BufferSubgraphTest, DegreeFourPinchNodeSkipsUnvisitedEdgesBeforeVisited) {
  PlanarGraph graph(BufferNodeFactory::instance());
  const Label cw_label(Location::BOUNDARY, Location::EXTERIOR,
                       Location::INTERIOR);
  // Two loops touching at (5,5). The upper loop occupies quadrants 0-1 while
  // the lower-right loop occupies quadrant 3, so computeNodeDepth(center)
  // scans past unvisited edges in CCW star order before reaching startEdge.
  Edge* upper_loop = MakeEdge({Coordinate(5.0, 5.0), Coordinate(0.0, 10.0),
                               Coordinate(6.0, 10.0), Coordinate(5.0, 5.0)},
                              cw_label, -1);
  Edge* right_loop = MakeEdge({Coordinate(5.0, 5.0), Coordinate(10.0, 4.0),
                               Coordinate(10.0, 0.0), Coordinate(5.0, 5.0)},
                              cw_label, -1);
  graph.addEdges({upper_loop, right_loop});

  Node* center = FindNode(graph, Coordinate(5.0, 5.0));
  ASSERT_NE(center, nullptr);

  BufferSubgraph subgraph;
  subgraph.create(center);
  ASSERT_NE(subgraph.getRightmostCoordinate(), nullptr);
  EXPECT_EQ(*subgraph.getRightmostCoordinate(), Coordinate(10.0, 4.0));

  subgraph.computeDepth(0);
  subgraph.findResultEdges();

  GeometryFactory::Ptr factory = GeometryFactory::create();
  PolygonBuilder poly_builder(factory.get());
  poly_builder.add(subgraph.getDirectedEdges(), subgraph.getNodes());
  std::vector<std::unique_ptr<Geometry>> polys = poly_builder.getPolygons();
  ASSERT_EQ(polys.size(), 2u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea() + polys[1]->getArea(), 25.0);
}

TEST(BufferSubgraphTest, DisjointSubgraphsCompareToGTAndNestedPolygonWithHole) {
  PlanarGraph graph(BufferNodeFactory::instance());
  const Label cw_label(Location::BOUNDARY, Location::EXTERIOR,
                       Location::INTERIOR);
  Edge* outer_edge = MakeEdge(
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
       Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
      cw_label, -1);
  // Inner CCW hole boundary: interior of polygon is on RHS, exterior on LHS.
  Edge* hole_edge = MakeEdge(
      {Coordinate(2.0, 2.0), Coordinate(8.0, 2.0), Coordinate(8.0, 8.0),
       Coordinate(2.0, 8.0), Coordinate(2.0, 2.0)},
      cw_label, -1);
  // Third component sharing x=10 with outer_edge to test compareTo() == 0.
  Edge* peer_edge = MakeEdge(
      {Coordinate(5.0, 20.0), Coordinate(5.0, 30.0), Coordinate(10.0, 30.0),
       Coordinate(10.0, 20.0), Coordinate(5.0, 20.0)},
      cw_label, -1);
  graph.addEdges({outer_edge, hole_edge, peer_edge});

  std::vector<Node*> all_nodes;
  graph.getNodes(all_nodes);
  std::vector<std::unique_ptr<BufferSubgraph>> subgraphs;
  for (Node* n : all_nodes) {
    if (!n->isVisited()) {
      auto sg = std::make_unique<BufferSubgraph>();
      sg->create(n);
      subgraphs.push_back(std::move(sg));
    }
  }
  ASSERT_EQ(subgraphs.size(), 3u);

  BufferSubgraph* outer_sg = nullptr;
  BufferSubgraph* hole_sg = nullptr;
  BufferSubgraph* peer_sg = nullptr;
  for (const auto& sg : subgraphs) {
    const Envelope* env = sg->getEnvelope();
    if (env->getMinY() == 0.0) {
      outer_sg = sg.get();
    } else if (env->getMinY() == 2.0) {
      hole_sg = sg.get();
    } else {
      peer_sg = sg.get();
    }
  }
  ASSERT_NE(outer_sg, nullptr);
  ASSERT_NE(hole_sg, nullptr);
  ASSERT_NE(peer_sg, nullptr);

  EXPECT_EQ(outer_sg->compareTo(hole_sg), 1);
  EXPECT_EQ(hole_sg->compareTo(outer_sg), -1);
  EXPECT_EQ(outer_sg->compareTo(peer_sg), 0);
  EXPECT_EQ(outer_sg->compareTo(outer_sg), 0);

  EXPECT_TRUE(BufferSubgraphGT(outer_sg, hole_sg));
  EXPECT_FALSE(BufferSubgraphGT(hole_sg, outer_sg));
  EXPECT_FALSE(BufferSubgraphGT(outer_sg, peer_sg));

  outer_sg->computeDepth(0);
  outer_sg->findResultEdges();

  hole_sg->computeDepth(1);
  hole_sg->findResultEdges();

  GeometryFactory::Ptr factory = GeometryFactory::create();
  PolygonBuilder poly_builder(factory.get());
  poly_builder.add(outer_sg->getDirectedEdges(), outer_sg->getNodes());
  poly_builder.add(hole_sg->getDirectedEdges(), hole_sg->getNodes());
  std::vector<std::unique_ptr<Geometry>> polys = poly_builder.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  const auto* poly = dynamic_cast<const Polygon*>(polys[0].get());
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 64.0);
}

TEST(BufferSubgraphTest, FindResultEdgesFiltersByDepthBoundsAndInteriorArea) {
  // Case 1: outsideDepth = 1 gives RIGHT = 2, LEFT = 1 -> excluded (LEFT > 0).
  {
    PlanarGraph graph(BufferNodeFactory::instance());
    const Label cw_label(Location::BOUNDARY, Location::EXTERIOR,
                         Location::INTERIOR);
    Edge* edge = MakeEdge(
        {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
         Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
        cw_label, -1);
    graph.addEdges({edge});
    BufferSubgraph sg;
    sg.create(FindNode(graph, Coordinate(0.0, 0.0)));
    sg.computeDepth(1);
    sg.findResultEdges();
    for (DirectedEdge* de : *sg.getDirectedEdges()) {
      EXPECT_FALSE(de->isInResult());
    }
  }

  // Case 2: outsideDepth = -1 gives RIGHT = 0, LEFT = -1 -> excluded (RIGHT <
  // 1).
  {
    PlanarGraph graph(BufferNodeFactory::instance());
    const Label cw_label(Location::BOUNDARY, Location::EXTERIOR,
                         Location::INTERIOR);
    Edge* edge = MakeEdge(
        {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
         Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
        cw_label, -1);
    graph.addEdges({edge});
    BufferSubgraph sg;
    sg.create(FindNode(graph, Coordinate(0.0, 0.0)));
    sg.computeDepth(-1);
    sg.findResultEdges();
    for (DirectedEdge* de : *sg.getDirectedEdges()) {
      EXPECT_FALSE(de->isInResult());
    }
  }

  // Case 3: outsideDepth = -1 with depthDelta = -2 gives RIGHT = 1, LEFT = -1
  // -> forward edge selected.
  {
    PlanarGraph graph(BufferNodeFactory::instance());
    const Label cw_label(Location::BOUNDARY, Location::EXTERIOR,
                         Location::INTERIOR);
    Edge* edge = MakeEdge(
        {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
         Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
        cw_label, -2);
    graph.addEdges({edge});
    BufferSubgraph sg;
    sg.create(FindNode(graph, Coordinate(0.0, 0.0)));
    sg.computeDepth(-1);
    sg.findResultEdges();
    for (DirectedEdge* de : *sg.getDirectedEdges()) {
      EXPECT_EQ(de->isInResult(), de->isForward());
    }
  }

  // Case 4: interior area edge (both sides INTERIOR for both geoms) is excluded
  // even when RIGHT >= 1 and LEFT <= 0.
  {
    PlanarGraph graph(BufferNodeFactory::instance());
    const Label interior_area_label(Location::BOUNDARY, Location::INTERIOR,
                                    Location::INTERIOR);
    Edge* edge = MakeEdge(
        {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
         Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
        interior_area_label, -1);
    graph.addEdges({edge});
    BufferSubgraph sg;
    sg.create(FindNode(graph, Coordinate(0.0, 0.0)));
    sg.computeDepth(0);
    sg.findResultEdges();
    for (DirectedEdge* de : *sg.getDirectedEdges()) {
      EXPECT_TRUE(de->isInteriorAreaEdge());
      EXPECT_FALSE(de->isInResult());
    }
  }
}

TEST(BufferSubgraphTest, ComputeDepthThrowsOnDepthMismatch) {
  PlanarGraph graph(BufferNodeFactory::instance());
  const Label cw_label(Location::BOUNDARY, Location::EXTERIOR,
                       Location::INTERIOR);
  // Non-closed single edge leaves unbalanced depthDelta at the endpoint node.
  Edge* edge =
      MakeEdge({Coordinate(0.0, 0.0), Coordinate(10.0, 10.0)}, cw_label, -1);
  graph.addEdges({edge});

  BufferSubgraph sg;
  sg.create(FindNode(graph, Coordinate(0.0, 0.0)));
  EXPECT_THROW(sg.computeDepth(0), TopologyException);
}

TEST(BufferSubgraphTest, ComputeNodeDepthThrowsWhenNeighborHasNoVisitedEdge) {
  const Label interior_label(Location::BOUNDARY, Location::INTERIOR,
                             Location::INTERIOR);
  auto seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          Coordinate(20.0, 0.0), Coordinate(10.0, 5.0)});
  auto edge = std::make_unique<Edge>(seq.release(), interior_label);
  edge->setDepthDelta(0);

  auto n0 =
      std::make_unique<Node>(Coordinate(20.0, 0.0), new DirectedEdgeStar());
  auto n1 =
      std::make_unique<Node>(Coordinate(10.0, 5.0), new DirectedEdgeStar());

  auto de_fwd = std::make_unique<DirectedEdge>(edge.get(), true);
  auto de_bwd = std::make_unique<DirectedEdge>(edge.get(), false);
  de_fwd->setSym(de_bwd.get());
  de_bwd->setSym(de_fwd.get());

  // Add de_fwd to n0's star, but only set de_bwd's node pointer to n1 without
  // registering de_bwd in n1's star so computeNodeDepth(n1) finds no startEdge.
  n0->add(de_fwd.get());
  de_bwd->setNode(n1.get());

  BufferSubgraph sg;
  sg.create(n0.get());
  EXPECT_THROW(sg.computeDepth(0), TopologyException);
}

}  // namespace
}  // namespace geos_google
