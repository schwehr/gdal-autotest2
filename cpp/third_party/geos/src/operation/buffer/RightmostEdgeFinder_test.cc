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

#include "third_party/geos/include/geos/operation/buffer/RightmostEdgeFinder.h"

#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdge.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/PlanarGraph.h"
#include "third_party/geos/include/geos/operation/buffer/BufferNodeFactory.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geomgraph::DirectedEdge;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeEnd;
using geos::geomgraph::PlanarGraph;
using geos::operation::buffer::BufferNodeFactory;
using geos::operation::buffer::RightmostEdgeFinder;
using geos::util::TopologyException;

struct GraphFixture {
  PlanarGraph graph{BufferNodeFactory::instance()};

  std::vector<DirectedEdge*> AddEdges(
      const std::vector<std::vector<Coordinate>>& edge_coords) {
    std::vector<Edge*> edges;
    edges.reserve(edge_coords.size());
    for (const auto& coords : edge_coords) {
      auto seq = std::make_unique<CoordinateSequence>();
      seq->reserve(coords.size());
      for (const Coordinate& c : coords) {
        seq->add(c);
      }
      edges.push_back(new Edge(seq.release()));
    }
    graph.addEdges(edges);
    std::vector<DirectedEdge*> dir_edges;
    for (EdgeEnd* ee : *graph.getEdgeEnds()) {
      dir_edges.push_back(dynamic_cast<DirectedEdge*>(ee));
    }
    return dir_edges;
  }
};

TEST(RightmostEdgeFinderTest, InitialStateBeforeFindEdge) {
  RightmostEdgeFinder finder;
  EXPECT_EQ(finder.getEdge(), nullptr);
  EXPECT_TRUE(finder.getCoordinate().isNull());
}

TEST(RightmostEdgeFinderTest, ThrowsOnEmptyDirectedEdgeList) {
  RightmostEdgeFinder finder;
  std::vector<DirectedEdge*> empty_edges;
  EXPECT_THROW(finder.findEdge(&empty_edges), TopologyException);
}

TEST(RightmostEdgeFinderTest, ThrowsWhenOnlyBackwardEdgesPresent) {
  GraphFixture fixture;
  std::vector<DirectedEdge*> all_edges = fixture.AddEdges({
      {Coordinate(0.0, 0.0), Coordinate(5.0, 10.0), Coordinate(10.0, 0.0),
       Coordinate(0.0, 0.0)},
  });

  std::vector<DirectedEdge*> backward_only;
  for (DirectedEdge* de : all_edges) {
    if (!de->isForward()) {
      backward_only.push_back(de);
    }
  }
  ASSERT_FALSE(backward_only.empty());

  RightmostEdgeFinder finder;
  EXPECT_THROW(finder.findEdge(&backward_only), TopologyException);
}

TEST(RightmostEdgeFinderTest, InteriorVertexCCWRingKeepsForwardUpwardEdge) {
  GraphFixture fixture;
  std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
      {Coordinate(0.0, 0.0), Coordinate(10.0, 0.0), Coordinate(10.0, 10.0),
       Coordinate(0.0, 10.0), Coordinate(0.0, 0.0)},
  });

  RightmostEdgeFinder finder;
  finder.findEdge(&dir_edges);

  EXPECT_EQ(finder.getCoordinate(), Coordinate(10.0, 0.0));
  ASSERT_NE(finder.getEdge(), nullptr);
  EXPECT_TRUE(finder.getEdge()->isForward());
}

TEST(RightmostEdgeFinderTest, InteriorVertexCWRingFlipsDownwardEdgeToSym) {
  GraphFixture fixture;
  std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
       Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
  });

  RightmostEdgeFinder finder;
  finder.findEdge(&dir_edges);

  EXPECT_EQ(finder.getCoordinate(), Coordinate(10.0, 10.0));
  ASSERT_NE(finder.getEdge(), nullptr);
  EXPECT_FALSE(finder.getEdge()->isForward());
}

TEST(RightmostEdgeFinderTest, InteriorVertexBothSegmentsBelowCCWUsesPrev) {
  GraphFixture fixture;
  // At minCoord=(10,10): pPrev=(5,0), pNext=(0,0) -> CCW, so usePrev=true.
  // Previous segment (5,0)->(10,10) goes upward -> Position::RIGHT.
  std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
      {Coordinate(0.0, 0.0), Coordinate(5.0, 0.0), Coordinate(10.0, 10.0),
       Coordinate(0.0, 0.0)},
  });

  RightmostEdgeFinder finder;
  finder.findEdge(&dir_edges);

  EXPECT_EQ(finder.getCoordinate(), Coordinate(10.0, 10.0));
  ASSERT_NE(finder.getEdge(), nullptr);
  EXPECT_TRUE(finder.getEdge()->isForward());
}

TEST(RightmostEdgeFinderTest, InteriorVertexBothSegmentsBelowCWKeepsCurrent) {
  GraphFixture fixture;
  // At minCoord=(10,10): pPrev=(0,0), pNext=(5,0) -> CW, so usePrev=false.
  // Current segment (10,10)->(5,0) goes downward -> Position::LEFT -> sym.
  std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
      {Coordinate(0.0, 0.0), Coordinate(10.0, 10.0), Coordinate(5.0, 0.0),
       Coordinate(0.0, 0.0)},
  });

  RightmostEdgeFinder finder;
  finder.findEdge(&dir_edges);

  EXPECT_EQ(finder.getCoordinate(), Coordinate(10.0, 10.0));
  ASSERT_NE(finder.getEdge(), nullptr);
  EXPECT_FALSE(finder.getEdge()->isForward());
}

TEST(RightmostEdgeFinderTest, InteriorVertexBothSegmentsAboveCWUsesPrev) {
  GraphFixture fixture;
  // At minCoord=(10,0): pPrev=(5,10), pNext=(0,10) -> CW, so usePrev=true.
  // Previous segment (5,10)->(10,0) goes downward -> Position::LEFT -> sym.
  std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
      {Coordinate(0.0, 10.0), Coordinate(5.0, 10.0), Coordinate(10.0, 0.0),
       Coordinate(0.0, 10.0)},
  });

  RightmostEdgeFinder finder;
  finder.findEdge(&dir_edges);

  EXPECT_EQ(finder.getCoordinate(), Coordinate(10.0, 0.0));
  ASSERT_NE(finder.getEdge(), nullptr);
  EXPECT_FALSE(finder.getEdge()->isForward());
}

TEST(RightmostEdgeFinderTest, InteriorVertexBothSegmentsAboveCCWKeepsCurrent) {
  GraphFixture fixture;
  // At minCoord=(10,0): pPrev=(0,10), pNext=(5,10) -> CCW, so usePrev=false.
  // Current segment (10,0)->(5,10) goes upward -> Position::RIGHT -> forward.
  std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
      {Coordinate(0.0, 10.0), Coordinate(10.0, 0.0), Coordinate(5.0, 10.0),
       Coordinate(0.0, 10.0)},
  });

  RightmostEdgeFinder finder;
  finder.findEdge(&dir_edges);

  EXPECT_EQ(finder.getCoordinate(), Coordinate(10.0, 0.0));
  ASSERT_NE(finder.getEdge(), nullptr);
  EXPECT_TRUE(finder.getEdge()->isForward());
}

TEST(RightmostEdgeFinderTest,
     InteriorVertexHorizontalNextFallsBackToPrevSegment) {
  GraphFixture fixture;
  // Segment 1 (10,10)->(5,10) is horizontal; side falls back to segment 0
  // (0,0)->(10,10) which goes upward -> Position::RIGHT.
  std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
      {Coordinate(0.0, 0.0), Coordinate(10.0, 10.0), Coordinate(5.0, 10.0),
       Coordinate(0.0, 0.0)},
  });

  RightmostEdgeFinder finder;
  finder.findEdge(&dir_edges);

  EXPECT_EQ(finder.getCoordinate(), Coordinate(10.0, 10.0));
  ASSERT_NE(finder.getEdge(), nullptr);
  EXPECT_TRUE(finder.getEdge()->isForward());
}

TEST(RightmostEdgeFinderTest, NodeWithForwardRightmostStarEdge) {
  GraphFixture fixture;
  // Multi-edge CCW square: E1 starts at rightmost node (10,0) and goes up.
  std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
      {Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)},
      {Coordinate(10.0, 0.0), Coordinate(10.0, 10.0)},
      {Coordinate(10.0, 10.0), Coordinate(0.0, 10.0)},
      {Coordinate(0.0, 10.0), Coordinate(0.0, 0.0)},
  });

  RightmostEdgeFinder finder;
  finder.findEdge(&dir_edges);

  EXPECT_EQ(finder.getCoordinate(), Coordinate(10.0, 0.0));
  ASSERT_NE(finder.getEdge(), nullptr);
  EXPECT_TRUE(finder.getEdge()->isForward());
  EXPECT_EQ(finder.getEdge()->getCoordinate(), Coordinate(10.0, 0.0));
  EXPECT_EQ(finder.getEdge()->getDirectedCoordinate(), Coordinate(10.0, 10.0));
}

TEST(RightmostEdgeFinderTest, NodeWithForwardDownwardStarEdgeFlipsToSym) {
  GraphFixture fixture;
  // Multi-edge CW trapezoid: E2 starts at (12,10) going down to (8,0).
  std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0)},
      {Coordinate(0.0, 10.0), Coordinate(12.0, 10.0)},
      {Coordinate(12.0, 10.0), Coordinate(8.0, 0.0)},
      {Coordinate(8.0, 0.0), Coordinate(0.0, 0.0)},
  });

  RightmostEdgeFinder finder;
  finder.findEdge(&dir_edges);

  EXPECT_EQ(finder.getCoordinate(), Coordinate(12.0, 10.0));
  ASSERT_NE(finder.getEdge(), nullptr);
  EXPECT_FALSE(finder.getEdge()->isForward());
  EXPECT_EQ(finder.getEdge()->getCoordinate(), Coordinate(8.0, 0.0));
  EXPECT_EQ(finder.getEdge()->getDirectedCoordinate(), Coordinate(12.0, 10.0));
}

TEST(RightmostEdgeFinderTest, NodeWithBackwardRightmostStarEdgeUpward) {
  GraphFixture fixture;
  // At node (12,10): E2 goes horizontally (12,10)->(0,10) (dy=0), while
  // E1's backward DirectedEdge goes (12,10)->(8,0) (dy=-10).
  // Star selects backward edge; RightmostEdgeFinder switches to forward E1
  // with minIndex = size - 1, then finds Position::RIGHT on segment 0.
  std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
      {Coordinate(0.0, 0.0), Coordinate(8.0, 0.0)},
      {Coordinate(8.0, 0.0), Coordinate(12.0, 10.0)},
      {Coordinate(12.0, 10.0), Coordinate(0.0, 10.0)},
      {Coordinate(0.0, 10.0), Coordinate(0.0, 0.0)},
  });

  RightmostEdgeFinder finder;
  finder.findEdge(&dir_edges);

  EXPECT_EQ(finder.getCoordinate(), Coordinate(12.0, 10.0));
  ASSERT_NE(finder.getEdge(), nullptr);
  EXPECT_TRUE(finder.getEdge()->isForward());
  EXPECT_EQ(finder.getEdge()->getCoordinate(), Coordinate(8.0, 0.0));
  EXPECT_EQ(finder.getEdge()->getDirectedCoordinate(), Coordinate(12.0, 10.0));
}

TEST(RightmostEdgeFinderTest, NodeWithBackwardRightmostStarEdgeDownwardFlips) {
  GraphFixture fixture;
  // At node (12,0): E2 goes horizontally (12,0)->(0,0) (NW, dy=0), while
  // E1's backward edge goes (12,0)->(8,10) (NW, smaller angle than pi).
  // Star selects backward E1; RightmostEdgeFinder flips to forward E1
  // (8,10)->(12,0) (downward -> Position::LEFT), then orientedDe flips back
  // to E1->getSym().
  std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
      {Coordinate(0.0, 10.0), Coordinate(8.0, 10.0)},
      {Coordinate(8.0, 10.0), Coordinate(12.0, 0.0)},
      {Coordinate(12.0, 0.0), Coordinate(0.0, 0.0)},
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0)},
  });

  RightmostEdgeFinder finder;
  finder.findEdge(&dir_edges);

  EXPECT_EQ(finder.getCoordinate(), Coordinate(12.0, 0.0));
  ASSERT_NE(finder.getEdge(), nullptr);
  EXPECT_FALSE(finder.getEdge()->isForward());
  EXPECT_EQ(finder.getEdge()->getCoordinate(), Coordinate(12.0, 0.0));
  EXPECT_EQ(finder.getEdge()->getDirectedCoordinate(), Coordinate(8.0, 10.0));
}

TEST(RightmostEdgeFinderTest, SelectsRightmostAmongMultipleSubgraphs) {
  GraphFixture fixture;
  std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
      {Coordinate(0.0, 0.0), Coordinate(5.0, 0.0), Coordinate(5.0, 5.0),
       Coordinate(0.0, 5.0), Coordinate(0.0, 0.0)},
      {Coordinate(10.0, 0.0), Coordinate(18.0, 0.0), Coordinate(18.0, 4.0),
       Coordinate(10.0, 4.0), Coordinate(10.0, 0.0)},
      {Coordinate(6.0, 0.0), Coordinate(12.0, 0.0), Coordinate(12.0, 3.0),
       Coordinate(6.0, 3.0), Coordinate(6.0, 0.0)},
  });

  RightmostEdgeFinder finder;
  finder.findEdge(&dir_edges);

  EXPECT_EQ(finder.getCoordinate(), Coordinate(18.0, 0.0));
  ASSERT_NE(finder.getEdge(), nullptr);
  EXPECT_TRUE(finder.getEdge()->isForward());
}

TEST(RightmostEdgeFinderTest, DegenerateHorizontalEdgeUsesFallback) {
  {
    GraphFixture fixture;
    std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
        {Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)},
    });

    RightmostEdgeFinder finder;
    finder.findEdge(&dir_edges);

    EXPECT_EQ(finder.getCoordinate(), Coordinate(0.0, 0.0));
    ASSERT_NE(finder.getEdge(), nullptr);
    EXPECT_TRUE(finder.getEdge()->isForward());
  }
  {
    GraphFixture fixture;
    std::vector<DirectedEdge*> dir_edges = fixture.AddEdges({
        {Coordinate(0.0, 0.0), Coordinate(10.0, 0.0), Coordinate(5.0, 0.0)},
    });

    RightmostEdgeFinder finder;
    finder.findEdge(&dir_edges);

    EXPECT_EQ(finder.getCoordinate(), Coordinate(10.0, 0.0));
    ASSERT_NE(finder.getEdge(), nullptr);
    EXPECT_TRUE(finder.getEdge()->isForward());
  }
}

}  // namespace
}  // namespace geos_google
