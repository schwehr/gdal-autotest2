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

// Unit tests for third_party/geos/src/operation/linemerge/LineMergeGraph.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/linemerge/LineMergeGraph.cpp

#include "third_party/geos/include/geos/operation/linemerge/LineMergeGraph.h"

#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/operation/linemerge/LineMergeDirectedEdge.h"
#include "third_party/geos/include/geos/operation/linemerge/LineMergeEdge.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdge.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdgeStar.h"
#include "third_party/geos/include/geos/planargraph/Edge.h"
#include "third_party/geos/include/geos/planargraph/Node.h"
#include "third_party/geos/include/geos/planargraph/PlanarGraph.h"
#include "third_party/geos/include/geos/util.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::operation::linemerge::LineMergeDirectedEdge;
using geos::operation::linemerge::LineMergeEdge;
using geos::operation::linemerge::LineMergeGraph;
using geos::planargraph::DirectedEdge;
using geos::planargraph::Edge;
using geos::planargraph::Node;
using geos::planargraph::PlanarGraph;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

std::unique_ptr<LineString> MakeLine(const GeometryFactory& factory,
                                     std::initializer_list<Coordinate> coords) {
  return factory.createLineString(CoordinateSequence(coords));
}

void ExpectEmptyGraph(LineMergeGraph& graph) {
  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_TRUE(nodes.empty());
  EXPECT_TRUE(graph.getEdges()->empty());
  EXPECT_EQ(std::distance(graph.dirEdgeBegin(), graph.dirEdgeEnd()), 0);
}

TEST(LineMergeGraphTest, EmptyGraphDestructionAndQueries) {
  LineMergeGraph graph;
  ExpectEmptyGraph(graph);
  EXPECT_EQ(graph.findNode(Coordinate(0.0, 0.0)), nullptr);
}

TEST(LineMergeGraphTest, EmptyLineStringIsIgnored) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> empty_line = factory->createLineString();

  LineMergeGraph graph;
  graph.addEdge(empty_line.get());

  ExpectEmptyGraph(graph);
}

TEST(LineMergeGraphTest, AllEqualCoordinatesLineStringIsIgnored) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> two_same =
      MakeLine(*factory, {Coordinate(5.0, 5.0), Coordinate(5.0, 5.0)});
  std::unique_ptr<LineString> three_same = MakeLine(
      *factory,
      {Coordinate(2.0, 3.0), Coordinate(2.0, 3.0), Coordinate(2.0, 3.0)});
  // RepeatedPointRemover compares 2D equality, so differing Z alone collapses.
  std::unique_ptr<LineString> same_xy_different_z = MakeLine(
      *factory, {Coordinate(4.0, 4.0, 1.0), Coordinate(4.0, 4.0, 9.0)});

  LineMergeGraph graph;
  graph.addEdge(two_same.get());
  graph.addEdge(three_same.get());
  graph.addEdge(same_xy_different_z.get());

  ExpectEmptyGraph(graph);
  EXPECT_EQ(graph.findNode(Coordinate(5.0, 5.0)), nullptr);
  EXPECT_EQ(graph.findNode(Coordinate(2.0, 3.0)), nullptr);
  EXPECT_EQ(graph.findNode(Coordinate(4.0, 4.0)), nullptr);
}

TEST(LineMergeGraphTest, SingleTwoPointEdgePopulatesNodesAndDirectedEdges) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> line =
      MakeLine(*factory, {Coordinate(0.0, 0.0), Coordinate(3.0, 4.0)});

  LineMergeGraph graph;
  graph.addEdge(line.get());

  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_THAT(nodes, SizeIs(2));
  ASSERT_THAT(*graph.getEdges(), SizeIs(1));
  EXPECT_EQ(std::distance(graph.dirEdgeBegin(), graph.dirEdgeEnd()), 2);

  Node* start_node = graph.findNode(Coordinate(0.0, 0.0));
  Node* end_node = graph.findNode(Coordinate(3.0, 4.0));
  ASSERT_NE(start_node, nullptr);
  ASSERT_NE(end_node, nullptr);
  EXPECT_NE(start_node, end_node);
  EXPECT_EQ(start_node->getDegree(), 1u);
  EXPECT_EQ(end_node->getDegree(), 1u);

  Edge* raw_edge = (*graph.getEdges())[0];
  auto* edge = geos::detail::down_cast<LineMergeEdge*>(raw_edge);
  EXPECT_EQ(edge->getLine(), line.get());

  auto* de0 =
      geos::detail::down_cast<LineMergeDirectedEdge*>(edge->getDirEdge(0));
  auto* de1 =
      geos::detail::down_cast<LineMergeDirectedEdge*>(edge->getDirEdge(1));
  ASSERT_NE(de0, nullptr);
  ASSERT_NE(de1, nullptr);

  EXPECT_EQ(de0->getFromNode(), start_node);
  EXPECT_EQ(de0->getToNode(), end_node);
  EXPECT_TRUE(de0->getEdgeDirection());
  EXPECT_TRUE(de0->getDirectionPt().equals2D(Coordinate(3.0, 4.0)));
  EXPECT_EQ(de0->getSym(), de1);
  EXPECT_EQ(de0->getEdge(), edge);

  EXPECT_EQ(de1->getFromNode(), end_node);
  EXPECT_EQ(de1->getToNode(), start_node);
  EXPECT_FALSE(de1->getEdgeDirection());
  EXPECT_TRUE(de1->getDirectionPt().equals2D(Coordinate(0.0, 0.0)));
  EXPECT_EQ(de1->getSym(), de0);
  EXPECT_EQ(de1->getEdge(), edge);
}

TEST(LineMergeGraphTest, RepeatedConsecutiveVerticesAreRemovedForDirectionPts) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> line =
      MakeLine(*factory, {Coordinate(0.0, 0.0), Coordinate(0.0, 0.0),
                          Coordinate(1.0, 0.0), Coordinate(1.0, 0.0),
                          Coordinate(2.0, 1.0), Coordinate(3.0, 3.0),
                          Coordinate(3.0, 3.0)});

  LineMergeGraph graph;
  graph.addEdge(line.get());

  ASSERT_THAT(*graph.getEdges(), SizeIs(1));
  Edge* edge = (*graph.getEdges())[0];
  auto* de0 =
      geos::detail::down_cast<LineMergeDirectedEdge*>(edge->getDirEdge(0));
  auto* de1 =
      geos::detail::down_cast<LineMergeDirectedEdge*>(edge->getDirEdge(1));

  // After removing repeated consecutive points, sequence is:
  // (0,0) -> (1,0) -> (2,1) -> (3,3).
  EXPECT_EQ(de0->getFromNode(), graph.findNode(Coordinate(0.0, 0.0)));
  EXPECT_EQ(de0->getToNode(), graph.findNode(Coordinate(3.0, 3.0)));
  EXPECT_TRUE(de0->getDirectionPt().equals2D(Coordinate(1.0, 0.0)));

  EXPECT_EQ(de1->getFromNode(), graph.findNode(Coordinate(3.0, 3.0)));
  EXPECT_EQ(de1->getToNode(), graph.findNode(Coordinate(0.0, 0.0)));
  EXPECT_TRUE(de1->getDirectionPt().equals2D(Coordinate(2.0, 1.0)));
}

TEST(LineMergeGraphTest, SharedEndpointsReuseExistingNodesAndTrackDegree) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> l1 =
      MakeLine(*factory, {Coordinate(0.0, 1.0), Coordinate(1.0, 1.0)});
  std::unique_ptr<LineString> l2 =
      MakeLine(*factory, {Coordinate(1.0, 1.0), Coordinate(2.0, 1.0)});
  std::unique_ptr<LineString> l3 =
      MakeLine(*factory, {Coordinate(1.0, 1.0), Coordinate(1.0, 2.0)});

  LineMergeGraph graph;
  graph.addEdge(l1.get());
  graph.addEdge(l2.get());
  graph.addEdge(l3.get());

  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_THAT(nodes, SizeIs(4));
  EXPECT_THAT(*graph.getEdges(), SizeIs(3));
  EXPECT_EQ(std::distance(graph.dirEdgeBegin(), graph.dirEdgeEnd()), 6);

  Node* center = graph.findNode(Coordinate(1.0, 1.0));
  ASSERT_NE(center, nullptr);
  EXPECT_EQ(center->getDegree(), 3u);

  std::vector<Node*> deg1_nodes;
  graph.findNodesOfDegree(1, deg1_nodes);
  EXPECT_THAT(deg1_nodes, SizeIs(3));

  std::vector<Node*> deg3_nodes;
  graph.findNodesOfDegree(3, deg3_nodes);
  ASSERT_THAT(deg3_nodes, SizeIs(1));
  EXPECT_EQ(deg3_nodes[0], center);
}

TEST(LineMergeGraphTest, ClosedLoopReusesStartNodeAsEndNode) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> loop =
      MakeLine(*factory, {Coordinate(0.0, 0.0), Coordinate(2.0, 0.0),
                          Coordinate(2.0, 2.0), Coordinate(0.0, 0.0)});
  std::unique_ptr<LinearRing> ring =
      factory->createLinearRing(CoordinateSequence(
          {Coordinate(10.0, 0.0), Coordinate(12.0, 0.0), Coordinate(12.0, 2.0),
           Coordinate(10.0, 2.0), Coordinate(10.0, 0.0)}));

  LineMergeGraph graph;
  graph.addEdge(loop.get());
  graph.addEdge(ring.get());

  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_THAT(nodes, SizeIs(2));
  ASSERT_THAT(*graph.getEdges(), SizeIs(2));
  EXPECT_EQ(std::distance(graph.dirEdgeBegin(), graph.dirEdgeEnd()), 4);

  Node* loop_node = graph.findNode(Coordinate(0.0, 0.0));
  ASSERT_NE(loop_node, nullptr);
  EXPECT_EQ(loop_node->getDegree(), 2u);

  Edge* loop_edge = (*graph.getEdges())[0];
  auto* de0 =
      geos::detail::down_cast<LineMergeDirectedEdge*>(loop_edge->getDirEdge(0));
  auto* de1 =
      geos::detail::down_cast<LineMergeDirectedEdge*>(loop_edge->getDirEdge(1));
  EXPECT_EQ(de0->getFromNode(), loop_node);
  EXPECT_EQ(de0->getToNode(), loop_node);
  EXPECT_TRUE(de0->getDirectionPt().equals2D(Coordinate(2.0, 0.0)));
  EXPECT_EQ(de1->getFromNode(), loop_node);
  EXPECT_EQ(de1->getToNode(), loop_node);
  EXPECT_TRUE(de1->getDirectionPt().equals2D(Coordinate(2.0, 2.0)));

  Node* ring_node = graph.findNode(Coordinate(10.0, 0.0));
  ASSERT_NE(ring_node, nullptr);
  EXPECT_EQ(ring_node->getDegree(), 2u);
}

TEST(LineMergeGraphTest, ParallelEdgesShareSameEndpointNodes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> upper = MakeLine(
      *factory,
      {Coordinate(0.0, 0.0), Coordinate(1.0, 1.0), Coordinate(2.0, 0.0)});
  std::unique_ptr<LineString> lower = MakeLine(
      *factory,
      {Coordinate(0.0, 0.0), Coordinate(1.0, -1.0), Coordinate(2.0, 0.0)});

  LineMergeGraph graph;
  graph.addEdge(upper.get());
  graph.addEdge(lower.get());

  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_THAT(nodes, SizeIs(2));
  ASSERT_THAT(*graph.getEdges(), SizeIs(2));
  EXPECT_EQ(std::distance(graph.dirEdgeBegin(), graph.dirEdgeEnd()), 4);

  Node* left = graph.findNode(Coordinate(0.0, 0.0));
  Node* right = graph.findNode(Coordinate(2.0, 0.0));
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(left->getDegree(), 2u);
  EXPECT_EQ(right->getDegree(), 2u);

  Edge* upper_edge = (*graph.getEdges())[0];
  Edge* lower_edge = (*graph.getEdges())[1];
  EXPECT_EQ(upper_edge->getOppositeNode(left), right);
  EXPECT_EQ(upper_edge->getOppositeNode(right), left);
  EXPECT_EQ(lower_edge->getOppositeNode(left), right);
  EXPECT_EQ(lower_edge->getOppositeNode(right), left);

  std::vector<Edge*> left_edges;
  DirectedEdge::toEdges(left->getOutEdges()->getEdges(), left_edges);
  EXPECT_THAT(left_edges, UnorderedElementsAre(upper_edge, lower_edge));

  std::vector<Edge*> right_edges;
  DirectedEdge::toEdges(right->getOutEdges()->getEdges(), right_edges);
  EXPECT_THAT(right_edges, UnorderedElementsAre(upper_edge, lower_edge));
}

TEST(LineMergeGraphTest, EndpointsWithDifferentZShareNodeBy2DCoordinate) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> l1 = MakeLine(
      *factory, {Coordinate(0.0, 0.0, 10.0), Coordinate(1.0, 0.0, 20.0)});
  std::unique_ptr<LineString> l2 = MakeLine(
      *factory, {Coordinate(1.0, 0.0, 99.0), Coordinate(2.0, 0.0, 30.0)});

  LineMergeGraph graph;
  graph.addEdge(l1.get());
  graph.addEdge(l2.get());

  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_THAT(nodes, SizeIs(3));
  Node* middle = graph.findNode(Coordinate(1.0, 0.0));
  ASSERT_NE(middle, nullptr);
  EXPECT_EQ(middle->getDegree(), 2u);
  EXPECT_DOUBLE_EQ(middle->getCoordinate().z, 20.0);
}

TEST(LineMergeGraphTest, EndNodeReusedAndLoopAttachedToExistingNode) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> l1 =
      MakeLine(*factory, {Coordinate(0.0, 0.0), Coordinate(1.0, 0.0)});
  std::unique_ptr<LineString> l2 =
      MakeLine(*factory, {Coordinate(2.0, 0.0), Coordinate(1.0, 0.0)});
  std::unique_ptr<LineString> loop =
      MakeLine(*factory, {Coordinate(1.0, 0.0), Coordinate(1.0, 1.0),
                          Coordinate(2.0, 1.0), Coordinate(1.0, 0.0)});

  LineMergeGraph graph;
  graph.addEdge(l1.get());
  graph.addEdge(l2.get());
  graph.addEdge(loop.get());

  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_THAT(nodes, SizeIs(3));
  EXPECT_THAT(*graph.getEdges(), SizeIs(3));
  EXPECT_EQ(std::distance(graph.dirEdgeBegin(), graph.dirEdgeEnd()), 6);

  Node* n0 = graph.findNode(Coordinate(0.0, 0.0));
  Node* n1 = graph.findNode(Coordinate(1.0, 0.0));
  Node* n2 = graph.findNode(Coordinate(2.0, 0.0));
  ASSERT_NE(n0, nullptr);
  ASSERT_NE(n1, nullptr);
  ASSERT_NE(n2, nullptr);
  EXPECT_EQ(n0->getDegree(), 1u);
  EXPECT_EQ(n1->getDegree(), 4u);
  EXPECT_EQ(n2->getDegree(), 1u);
}

TEST(LineMergeGraphTest, DegreeTwoConnectionWiresLineMergeDirectedEdgeNext) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> l1 =
      MakeLine(*factory, {Coordinate(0.0, 0.0), Coordinate(1.0, 0.0)});
  std::unique_ptr<LineString> l2 =
      MakeLine(*factory, {Coordinate(1.0, 0.0), Coordinate(2.0, 0.0)});

  LineMergeGraph graph;
  graph.addEdge(l1.get());
  graph.addEdge(l2.get());

  ASSERT_THAT(*graph.getEdges(), SizeIs(2));
  Edge* e1 = (*graph.getEdges())[0];
  Edge* e2 = (*graph.getEdges())[1];
  auto* e1_fwd =
      geos::detail::down_cast<LineMergeDirectedEdge*>(e1->getDirEdge(0));
  auto* e2_fwd =
      geos::detail::down_cast<LineMergeDirectedEdge*>(e2->getDirEdge(0));
  ASSERT_NE(e1_fwd, nullptr);
  ASSERT_NE(e2_fwd, nullptr);
  EXPECT_EQ(e1_fwd->getNext(), e2_fwd);
  EXPECT_EQ(e1_fwd->getNext(/*checkDirection=*/true), e2_fwd);
  EXPECT_EQ(e2_fwd->getNext(), nullptr);
}

TEST(LineMergeGraphTest, DeleteThroughPlanarGraphBasePointer) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> line =
      MakeLine(*factory, {Coordinate(0.0, 0.0), Coordinate(1.0, 1.0)});

  std::unique_ptr<PlanarGraph> empty_base = std::make_unique<LineMergeGraph>();
  EXPECT_TRUE(empty_base->getEdges()->empty());
  empty_base.reset();

  auto derived = std::make_unique<LineMergeGraph>();
  derived->addEdge(line.get());
  std::unique_ptr<PlanarGraph> populated_base = std::move(derived);
  EXPECT_THAT(*populated_base->getEdges(), SizeIs(1));
  EXPECT_NE(populated_base->findNode(Coordinate(0.0, 0.0)), nullptr);
  populated_base.reset();
}

}  // namespace
}  // namespace geos_google
