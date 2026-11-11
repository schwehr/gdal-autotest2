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
// third_party/geos/src/operation/linemerge/LineMergeDirectedEdge.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/linemerge/LineMergeDirectedEdge.cpp

#include "third_party/geos/include/geos/operation/linemerge/LineMergeDirectedEdge.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Quadrant.h"
#include "third_party/geos/include/geos/operation/linemerge/LineMergeEdge.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdgeStar.h"
#include "third_party/geos/include/geos/planargraph/Node.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Quadrant;
using geos::operation::linemerge::LineMergeDirectedEdge;
using geos::operation::linemerge::LineMergeEdge;
using geos::planargraph::Node;
using geos::util::IllegalArgumentException;

struct EdgePair {
  LineMergeDirectedEdge* forward;
  LineMergeDirectedEdge* reverse;
  LineMergeEdge* edge;
};

class GraphBuilder {
 public:
  explicit GraphBuilder(const GeometryFactory& factory) : factory_(factory) {}

  Node* AddNode(double x, double y) {
    nodes_.push_back(std::make_unique<Node>(Coordinate(x, y)));
    return nodes_.back().get();
  }

  EdgePair AddEdge(Node* from, Node* to,
                   std::initializer_list<CoordinateXY> coords) {
    lines_.push_back(factory_.createLineString(CoordinateSequence(coords)));
    const LineString* line = lines_.back().get();
    const CoordinateSequence* seq = line->getCoordinatesRO();

    dir_edges_.push_back(std::make_unique<LineMergeDirectedEdge>(
        from, to, Coordinate(seq->getX(1), seq->getY(1)), true));
    LineMergeDirectedEdge* fwd = dir_edges_.back().get();

    const std::size_t prev_idx = seq->size() - 2;
    dir_edges_.push_back(std::make_unique<LineMergeDirectedEdge>(
        to, from, Coordinate(seq->getX(prev_idx), seq->getY(prev_idx)), false));
    LineMergeDirectedEdge* rev = dir_edges_.back().get();

    edges_.push_back(std::make_unique<LineMergeEdge>(line));
    LineMergeEdge* edge = edges_.back().get();
    edge->setDirectedEdges(fwd, rev);
    return {.forward = fwd, .reverse = rev, .edge = edge};
  }

 private:
  const GeometryFactory& factory_;
  std::vector<std::unique_ptr<LineString>> lines_;
  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<std::unique_ptr<LineMergeDirectedEdge>> dir_edges_;
  std::vector<std::unique_ptr<LineMergeEdge>> edges_;
};

TEST(LineMergeDirectedEdgeTest,
     ConstructsWithNodesDirectionPtAndEdgeDirection) {
  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(3.0, 4.0));

  LineMergeDirectedEdge fwd(&n0, &n1, Coordinate(1.0, 1.0), true);
  EXPECT_EQ(fwd.getFromNode(), &n0);
  EXPECT_EQ(fwd.getToNode(), &n1);
  EXPECT_TRUE(fwd.getCoordinate().equals2D(CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(fwd.getDirectionPt().equals2D(CoordinateXY(1.0, 1.0)));
  EXPECT_TRUE(fwd.getEdgeDirection());
  EXPECT_EQ(fwd.getQuadrant(), int{Quadrant::NE});
  EXPECT_NEAR(fwd.getAngle(), std::numbers::pi / 4.0, 1e-12);

  LineMergeDirectedEdge rev(&n1, &n0, Coordinate(2.0, 3.0), false);
  EXPECT_EQ(rev.getFromNode(), &n1);
  EXPECT_EQ(rev.getToNode(), &n0);
  EXPECT_FALSE(rev.getEdgeDirection());
  EXPECT_EQ(rev.getQuadrant(), int{Quadrant::SW});
}

TEST(LineMergeDirectedEdgeTest, ThrowsWhenDirectionPtMatchesFromCoordinate) {
  Node n0(Coordinate(1.0, 2.0));
  Node n1(Coordinate(3.0, 4.0));

  EXPECT_THROW(LineMergeDirectedEdge(&n0, &n1, Coordinate(1.0, 2.0), true),
               IllegalArgumentException);
}

TEST(LineMergeDirectedEdgeTest, GetNextReturnsNullWhenToNodeDegreeIsZero) {
  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(1.0, 0.0));
  LineMergeDirectedEdge de(&n0, &n1, Coordinate(1.0, 0.0), true);

  ASSERT_EQ(n1.getDegree(), 0u);
  EXPECT_EQ(de.getNext(), nullptr);
  EXPECT_EQ(de.getNext(true), nullptr);
}

TEST(LineMergeDirectedEdgeTest, GetNextReturnsNullWhenToNodeDegreeIsOne) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphBuilder graph(*factory);

  Node* n0 = graph.AddNode(0.0, 0.0);
  Node* n1 = graph.AddNode(1.0, 0.0);
  EdgePair e = graph.AddEdge(n0, n1, {{0.0, 0.0}, {1.0, 0.0}});

  ASSERT_EQ(n1->getDegree(), 1u);
  ASSERT_EQ(n0->getDegree(), 1u);
  EXPECT_EQ(e.forward->getNext(), nullptr);
  EXPECT_EQ(e.forward->getNext(true), nullptr);
  EXPECT_EQ(e.reverse->getNext(), nullptr);
  EXPECT_EQ(e.reverse->getNext(true), nullptr);
}

TEST(LineMergeDirectedEdgeTest,
     GetNextReturnsNullAtDegreeThreeAndFourJunctions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphBuilder graph(*factory);

  Node* n0 = graph.AddNode(0.0, 0.0);
  Node* n1 = graph.AddNode(1.0, 0.0);
  Node* n2 = graph.AddNode(2.0, 0.0);
  Node* n3 = graph.AddNode(1.0, 1.0);
  Node* n4 = graph.AddNode(1.0, -1.0);

  EdgePair e0 = graph.AddEdge(n0, n1, {{0.0, 0.0}, {1.0, 0.0}});
  graph.AddEdge(n1, n2, {{1.0, 0.0}, {2.0, 0.0}});
  graph.AddEdge(n1, n3, {{1.0, 0.0}, {1.0, 1.0}});

  ASSERT_EQ(n1->getDegree(), 3u);
  EXPECT_EQ(e0.forward->getNext(false), nullptr);
  EXPECT_EQ(e0.forward->getNext(true), nullptr);

  graph.AddEdge(n1, n4, {{1.0, 0.0}, {1.0, -1.0}});
  ASSERT_EQ(n1->getDegree(), 4u);
  EXPECT_EQ(e0.forward->getNext(false), nullptr);
  EXPECT_EQ(e0.forward->getNext(true), nullptr);
}

TEST(LineMergeDirectedEdgeTest,
     GetNextHandlesBothStarPositionsAndDirectionChecks) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Case 1: e0: n0 -> n1, e1: n1 -> n2.
  // At n1, e1.forward points NE (+1, 0) and e0.reverse points NW (-1, 0),
  // so star edges are [e1.forward, e0.reverse].
  {
    GraphBuilder graph(*factory);
    Node* n0 = graph.AddNode(0.0, 0.0);
    Node* n1 = graph.AddNode(1.0, 0.0);
    Node* n2 = graph.AddNode(2.0, 0.0);

    EdgePair e0 = graph.AddEdge(n0, n1, {{0.0, 0.0}, {1.0, 0.0}});
    EdgePair e1 = graph.AddEdge(n1, n2, {{1.0, 0.0}, {2.0, 0.0}});

    ASSERT_EQ(n1->getDegree(), 2u);
    const auto& star_edges = n1->getOutEdges()->getEdges();
    ASSERT_EQ(star_edges[0], e1.forward);
    ASSERT_EQ(star_edges[1], e0.reverse);

    // sym == star_edges[1]; nextedge == e1.forward (edgeDirection == true).
    EXPECT_EQ(e0.forward->getNext(), e1.forward);
    EXPECT_EQ(e0.forward->getNext(false), e1.forward);
    EXPECT_EQ(e0.forward->getNext(true), e1.forward);

    // sym == star_edges[0]; nextedge == e0.reverse (edgeDirection == false).
    EXPECT_EQ(e1.reverse->getNext(), e0.reverse);
    EXPECT_EQ(e1.reverse->getNext(false), e0.reverse);
    EXPECT_EQ(e1.reverse->getNext(true), nullptr);
  }

  // Case 2: e0: n1 -> n0, e1: n2 -> n1.
  // At n1, e1.reverse points NE (+1, 0) and e0.forward points NW (-1, 0),
  // so star edges are [e1.reverse, e0.forward].
  {
    GraphBuilder graph(*factory);
    Node* n0 = graph.AddNode(0.0, 0.0);
    Node* n1 = graph.AddNode(1.0, 0.0);
    Node* n2 = graph.AddNode(2.0, 0.0);

    EdgePair e0 = graph.AddEdge(n1, n0, {{1.0, 0.0}, {0.0, 0.0}});
    EdgePair e1 = graph.AddEdge(n2, n1, {{2.0, 0.0}, {1.0, 0.0}});

    ASSERT_EQ(n1->getDegree(), 2u);
    const auto& star_edges = n1->getOutEdges()->getEdges();
    ASSERT_EQ(star_edges[0], e1.reverse);
    ASSERT_EQ(star_edges[1], e0.forward);

    // sym == star_edges[0]; nextedge == e0.forward (edgeDirection == true).
    EXPECT_EQ(e1.forward->getNext(false), e0.forward);
    EXPECT_EQ(e1.forward->getNext(true), e0.forward);

    // sym == star_edges[1]; nextedge == e1.reverse (edgeDirection == false).
    EXPECT_EQ(e0.reverse->getNext(false), e1.reverse);
    EXPECT_EQ(e0.reverse->getNext(true), nullptr);
  }
}

TEST(LineMergeDirectedEdgeTest,
     GetNextOnSelfLoopReturnsSelfOrNullDependingOnDirection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphBuilder graph(*factory);

  Node* n0 = graph.AddNode(0.0, 0.0);
  EdgePair loop =
      graph.AddEdge(n0, n0, {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}});

  ASSERT_EQ(n0->getDegree(), 2u);
  EXPECT_EQ(loop.forward->getNext(false), loop.forward);
  EXPECT_EQ(loop.forward->getNext(true), loop.forward);
  EXPECT_EQ(loop.reverse->getNext(false), loop.reverse);
  EXPECT_EQ(loop.reverse->getNext(true), nullptr);
}

TEST(LineMergeDirectedEdgeTest, FollowsMultiEdgeChainUntilEndpoint) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphBuilder graph(*factory);

  Node* n0 = graph.AddNode(0.0, 0.0);
  Node* n1 = graph.AddNode(1.0, 0.0);
  Node* n2 = graph.AddNode(2.0, 0.0);
  Node* n3 = graph.AddNode(3.0, 0.0);

  EdgePair e0 = graph.AddEdge(n0, n1, {{0.0, 0.0}, {1.0, 0.0}});
  EdgePair e1 = graph.AddEdge(n2, n1, {{2.0, 0.0}, {1.0, 0.0}});
  EdgePair e2 = graph.AddEdge(n2, n3, {{2.0, 0.0}, {3.0, 0.0}});

  std::vector<LineMergeDirectedEdge*> undirected_walk;
  for (LineMergeDirectedEdge* cur = e0.forward; cur != nullptr;
       cur = cur->getNext(false)) {
    undirected_walk.push_back(cur);
  }
  ASSERT_EQ(undirected_walk.size(), 3u);
  EXPECT_EQ(undirected_walk[0], e0.forward);
  EXPECT_EQ(undirected_walk[1], e1.reverse);
  EXPECT_EQ(undirected_walk[2], e2.forward);

  EXPECT_EQ(e0.forward->getNext(true), nullptr);
}

}  // namespace
}  // namespace geos_google
