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

#include "third_party/geos/include/geos/operation/buffer/SubgraphDepthLocater.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/PlanarGraph.h"
#include "third_party/geos/include/geos/operation/buffer/BufferNodeFactory.h"
#include "third_party/geos/include/geos/operation/buffer/BufferSubgraph.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Location;
using geos::geomgraph::Edge;
using geos::geomgraph::Label;
using geos::geomgraph::Node;
using geos::geomgraph::PlanarGraph;
using geos::operation::buffer::BufferNodeFactory;
using geos::operation::buffer::BufferSubgraph;
using geos::operation::buffer::SubgraphDepthLocater;

struct SubgraphArena {
  std::vector<std::unique_ptr<PlanarGraph>> graphs;
  std::vector<std::unique_ptr<BufferSubgraph>> owned_subgraphs;

  BufferSubgraph* AddSubgraph(
      const std::vector<std::vector<Coordinate>>& edge_coords,
      int depth_delta = -1, int outside_depth = 0) {
    auto graph = std::make_unique<PlanarGraph>(BufferNodeFactory::instance());
    std::vector<Edge*> raw_edges;
    raw_edges.reserve(edge_coords.size());

    const Label lbl(0, Location::BOUNDARY,
                    depth_delta < 0 ? Location::EXTERIOR : Location::INTERIOR,
                    depth_delta < 0 ? Location::INTERIOR : Location::EXTERIOR);

    for (const auto& coords : edge_coords) {
      auto seq = std::make_unique<CoordinateSequence>();
      seq->reserve(coords.size());
      for (const Coordinate& c : coords) {
        seq->add(c);
      }
      Edge* edge = new Edge(seq.release(), lbl);
      edge->setDepthDelta(depth_delta);
      raw_edges.push_back(edge);
    }

    graph->addEdges(raw_edges);
    std::vector<Node*> nodes;
    graph->getNodes(nodes);
    EXPECT_FALSE(nodes.empty());

    auto subgraph = std::make_unique<BufferSubgraph>();
    subgraph->create(nodes.front());
    subgraph->computeDepth(outside_depth);
    BufferSubgraph* result = subgraph.get();

    graphs.push_back(std::move(graph));
    owned_subgraphs.push_back(std::move(subgraph));
    return result;
  }
};

TEST(SubgraphDepthLocaterTest, EmptySubgraphsReturnsZero) {
  std::vector<BufferSubgraph*> subgraphs;
  SubgraphDepthLocater locater(&subgraphs);
  EXPECT_EQ(locater.getDepth(Coordinate(0.0, 0.0)), 0);
}

TEST(SubgraphDepthLocaterTest, EnvelopeBoundsAndExteriorStabbing) {
  SubgraphArena arena;
  BufferSubgraph* sq = arena.AddSubgraph({
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
       Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
  });
  std::vector<BufferSubgraph*> subgraphs{sq};
  SubgraphDepthLocater locater(&subgraphs);

  // Outside subgraph envelope (minY, maxY, maxX checks).
  EXPECT_EQ(locater.getDepth(Coordinate(5.0, -1.0)), 0);
  EXPECT_EQ(locater.getDepth(Coordinate(5.0, 11.0)), 0);
  EXPECT_EQ(locater.getDepth(Coordinate(11.0, 5.0)), 0);

  // Left of polygon: stabs upward left edge (leftDepth=0) and downward right
  // edge (leftDepth=1); leftmost segment wins.
  EXPECT_EQ(locater.getDepth(Coordinate(-5.0, 5.0)), 0);

  // Inside polygon.
  EXPECT_EQ(locater.getDepth(Coordinate(5.0, 5.0)), 1);
}

TEST(SubgraphDepthLocaterTest, NestedShellHoleAndIslandFromSingleGraph) {
  // Single PlanarGraph with three disconnected concentric rings, built in
  // descending rightmost-coordinate order matching BufferBuilder.
  PlanarGraph graph(BufferNodeFactory::instance());
  const Label cw_lbl(0, Location::BOUNDARY, Location::EXTERIOR,
                     Location::INTERIOR);
  auto add_ring = [&](const std::vector<Coordinate>& coords, int depth_delta) {
    auto seq = std::make_unique<CoordinateSequence>();
    seq->reserve(coords.size());
    for (const Coordinate& c : coords) {
      seq->add(c);
    }
    Edge* edge = new Edge(seq.release(), cw_lbl);
    edge->setDepthDelta(depth_delta);
    return edge;
  };

  // Outer shell [0,20]x[0,20], hole [4,16]x[4,16] (CCW), island [8,12]x[8,12].
  std::vector<Edge*> edges{
      add_ring(
          {Coordinate(0.0, 0.0), Coordinate(0.0, 20.0), Coordinate(20.0, 20.0),
           Coordinate(20.0, 0.0), Coordinate(0.0, 0.0)},
          -1),
      add_ring(
          {Coordinate(4.0, 4.0), Coordinate(16.0, 4.0), Coordinate(16.0, 16.0),
           Coordinate(4.0, 16.0), Coordinate(4.0, 4.0)},
          -1),
      add_ring(
          {Coordinate(8.0, 8.0), Coordinate(8.0, 12.0), Coordinate(12.0, 12.0),
           Coordinate(12.0, 8.0), Coordinate(8.0, 8.0)},
          -1),
  };
  graph.addEdges(edges);

  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  std::vector<std::unique_ptr<BufferSubgraph>> owned;
  std::vector<BufferSubgraph*> processed;
  for (Node* n : nodes) {
    if (!n->isVisited()) {
      auto bsg = std::make_unique<BufferSubgraph>();
      bsg->create(n);
      processed.push_back(bsg.get());
      owned.push_back(std::move(bsg));
    }
  }
  ASSERT_EQ(processed.size(), 3u);
  std::sort(processed.begin(), processed.end(),
            geos::operation::buffer::BufferSubgraphGT);

  std::vector<BufferSubgraph*> active;
  for (BufferSubgraph* bsg : processed) {
    SubgraphDepthLocater loc(&active);
    const int outside_depth = loc.getDepth(*bsg->getRightmostCoordinate());
    bsg->computeDepth(outside_depth);
    active.push_back(bsg);
  }

  SubgraphDepthLocater final_loc(&active);
  EXPECT_EQ(final_loc.getDepth(Coordinate(2.0, 10.0)), 1);
  EXPECT_EQ(final_loc.getDepth(Coordinate(6.0, 10.0)), 0);
  EXPECT_EQ(final_loc.getDepth(Coordinate(10.0, 10.0)), 1);
}

TEST(SubgraphDepthLocaterTest, MultiEdgeSubgraphFiltersEdgeEnvelopes) {
  SubgraphArena arena;
  // CW hexagon formed by 6 separate edges so edge envelopes are smaller than
  // the subgraph envelope [0,10]x[0,10].
  BufferSubgraph* hex = arena.AddSubgraph({
      {Coordinate(0.0, 5.0), Coordinate(2.0, 10.0)},
      {Coordinate(2.0, 10.0), Coordinate(8.0, 10.0)},
      {Coordinate(8.0, 10.0), Coordinate(10.0, 5.0)},
      {Coordinate(10.0, 5.0), Coordinate(8.0, 0.0)},
      {Coordinate(8.0, 0.0), Coordinate(2.0, 0.0)},
      {Coordinate(2.0, 0.0), Coordinate(0.0, 5.0)},
  });
  std::vector<BufferSubgraph*> subgraphs{hex};
  SubgraphDepthLocater locater(&subgraphs);

  // Ray at (5,5) skips top, bottom, and left edges by edge envelope.
  EXPECT_EQ(locater.getDepth(Coordinate(5.0, 5.0)), 1);
  // Outside upper-left corner, within subgraph bbox.
  EXPECT_EQ(locater.getDepth(Coordinate(1.0, 8.0)), 0);
  // Outside upper-right corner, right of upper-right edge.
  EXPECT_EQ(locater.getDepth(Coordinate(9.0, 8.0)), 0);
}

TEST(SubgraphDepthLocaterTest, NonConvexSingleEdgeSegmentFilters) {
  SubgraphArena arena;
  // CW C-shaped polygon in a single edge covering all segment filters.
  BufferSubgraph* c_shape = arena.AddSubgraph({
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
       Coordinate(10.0, 8.0), Coordinate(4.0, 8.0), Coordinate(4.0, 2.0),
       Coordinate(10.0, 2.0), Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
  });
  std::vector<BufferSubgraph*> subgraphs{c_shape};
  SubgraphDepthLocater locater(&subgraphs);

  EXPECT_EQ(locater.getDepth(Coordinate(2.0, 5.0)), 1);
  EXPECT_EQ(locater.getDepth(Coordinate(6.0, 5.0)), 0);
  EXPECT_EQ(locater.getDepth(Coordinate(8.0, 9.0)), 1);
  EXPECT_EQ(locater.getDepth(Coordinate(8.0, 1.0)), 1);
  EXPECT_EQ(locater.getDepth(Coordinate(-1.0, 5.0)), 0);
}

TEST(SubgraphDepthLocaterTest, SlantedTriangleSkipsRayRightOfSegment) {
  SubgraphArena arena;
  BufferSubgraph* tri = arena.AddSubgraph({
      {Coordinate(0.0, 0.0), Coordinate(5.0, 10.0), Coordinate(10.0, 0.0),
       Coordinate(0.0, 0.0)},
  });
  std::vector<BufferSubgraph*> subgraphs{tri};
  SubgraphDepthLocater locater(&subgraphs);

  // (4,2) is to the right of upward segment (0,0)->(5,10), so segment 0 is
  // skipped and segment 1 (10,0)->(5,10) gives interior depth 1.
  EXPECT_EQ(locater.getDepth(Coordinate(4.0, 2.0)), 1);
  // (9,8) is right of segment 1 -> no segments stabbed -> 0.
  EXPECT_EQ(locater.getDepth(Coordinate(9.0, 8.0)), 0);
}

TEST(SubgraphDepthLocaterTest, DoubleDepthDeltaReportedInside) {
  SubgraphArena arena;
  BufferSubgraph* dbl = arena.AddSubgraph(
      {{Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
        Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)}},
      /*depth_delta=*/-2, /*outside_depth=*/0);
  std::vector<BufferSubgraph*> subgraphs{dbl};
  SubgraphDepthLocater locater(&subgraphs);

  EXPECT_EQ(locater.getDepth(Coordinate(5.0, 5.0)), 2);
}

TEST(SubgraphDepthLocaterTest, DepthSegmentCompareDisjointXBothOrders) {
  SubgraphArena arena;
  BufferSubgraph* left_sq = arena.AddSubgraph(
      {{Coordinate(2.0, 0.0), Coordinate(2.0, 10.0), Coordinate(4.0, 10.0),
        Coordinate(4.0, 0.0), Coordinate(2.0, 0.0)}},
      -1, 10);
  BufferSubgraph* right_sq = arena.AddSubgraph(
      {{Coordinate(8.0, 0.0), Coordinate(8.0, 10.0), Coordinate(12.0, 10.0),
        Coordinate(12.0, 0.0), Coordinate(8.0, 0.0)}},
      -1, 20);

  {
    std::vector<BufferSubgraph*> ab{left_sq, right_sq};
    SubgraphDepthLocater loc(&ab);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 5.0)), 10);
  }
  {
    std::vector<BufferSubgraph*> ba{right_sq, left_sq};
    SubgraphDepthLocater loc(&ba);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 5.0)), 10);
  }
}

TEST(SubgraphDepthLocaterTest, DepthSegmentCompareByP0AndOtherP0InYExtent) {
  SubgraphArena arena;
  BufferSubgraph* a =
      arena.AddSubgraph({{Coordinate(4.0, 0.0), Coordinate(10.0, 10.0),
                          Coordinate(20.0, 0.0), Coordinate(4.0, 0.0)}},
                        -1, 10);
  // Left edge (9,2)->(7,8) overlaps X [4,10] and lies right of (4,0)->(10,10).
  BufferSubgraph* b =
      arena.AddSubgraph({{Coordinate(9.0, 2.0), Coordinate(7.0, 8.0),
                          Coordinate(22.0, 2.0), Coordinate(9.0, 2.0)}},
                        -1, 20);
  // Left edge (5,4)->(7,9) overlaps X [4,10] and lies left of (4,0)->(10,10).
  BufferSubgraph* c =
      arena.AddSubgraph({{Coordinate(5.0, 4.0), Coordinate(7.0, 9.0),
                          Coordinate(22.0, 4.0), Coordinate(5.0, 4.0)}},
                        -1, 30);

  {
    std::vector<BufferSubgraph*> ab{a, b};
    SubgraphDepthLocater loc(&ab);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 5.0)), 10);
  }
  {
    std::vector<BufferSubgraph*> ba{b, a};
    SubgraphDepthLocater loc(&ba);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 5.0)), 10);
  }
  {
    std::vector<BufferSubgraph*> ac{a, c};
    SubgraphDepthLocater loc(&ac);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 5.0)), 30);
  }
  {
    std::vector<BufferSubgraph*> ca{c, a};
    SubgraphDepthLocater loc(&ca);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 5.0)), 30);
  }
}

TEST(SubgraphDepthLocaterTest, DepthSegmentCompareByP1InYExtent) {
  SubgraphArena arena;
  // A: (6,4)->(10,8) vs B: (6,0)->(6,6) -> comp00=0 (0<4), comp01=-1 (LEFT).
  BufferSubgraph* a =
      arena.AddSubgraph({{Coordinate(6.0, 4.0), Coordinate(10.0, 8.0),
                          Coordinate(20.0, 4.0), Coordinate(6.0, 4.0)}},
                        -1, 10);
  BufferSubgraph* b =
      arena.AddSubgraph({{Coordinate(6.0, 0.0), Coordinate(6.0, 6.0),
                          Coordinate(20.0, 0.0), Coordinate(6.0, 0.0)}},
                        -1, 20);
  {
    std::vector<BufferSubgraph*> ab{a, b};
    SubgraphDepthLocater loc(&ab);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 5.0)), 20);
  }
  {
    std::vector<BufferSubgraph*> ba{b, a};
    SubgraphDepthLocater loc(&ba);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 5.0)), 20);
  }

  // C: (4,4)->(10,8) vs D: (9,0)->(9,6) -> comp00=0 (0<4), comp01=+1 (RIGHT).
  BufferSubgraph* c =
      arena.AddSubgraph({{Coordinate(4.0, 4.0), Coordinate(10.0, 8.0),
                          Coordinate(20.0, 4.0), Coordinate(4.0, 4.0)}},
                        -1, 10);
  BufferSubgraph* d =
      arena.AddSubgraph({{Coordinate(9.0, 0.0), Coordinate(9.0, 6.0),
                          Coordinate(20.0, 0.0), Coordinate(9.0, 0.0)}},
                        -1, 20);
  {
    std::vector<BufferSubgraph*> cd{c, d};
    SubgraphDepthLocater loc(&cd);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 5.0)), 10);
  }
  {
    std::vector<BufferSubgraph*> dc{d, c};
    SubgraphDepthLocater loc(&dc);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 5.0)), 10);
  }
}

TEST(SubgraphDepthLocaterTest, DepthSegmentCompareByOtherP1InYExtent) {
  SubgraphArena arena;
  // Shared bottom vertex (5,0): A=(5,0)->(7,6), B=(5,0)->(6,10) -> comp11=-1.
  BufferSubgraph* a =
      arena.AddSubgraph({{Coordinate(5.0, 0.0), Coordinate(7.0, 6.0),
                          Coordinate(20.0, 0.0), Coordinate(5.0, 0.0)}},
                        -1, 10);
  BufferSubgraph* b =
      arena.AddSubgraph({{Coordinate(5.0, 0.0), Coordinate(6.0, 10.0),
                          Coordinate(25.0, 0.0), Coordinate(5.0, 0.0)}},
                        -1, 20);
  {
    std::vector<BufferSubgraph*> ab{a, b};
    SubgraphDepthLocater loc(&ab);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 3.0)), 20);
  }
  {
    std::vector<BufferSubgraph*> ba{b, a};
    SubgraphDepthLocater loc(&ba);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 3.0)), 20);
  }

  // Shared bottom vertex (5,0): C=(5,0)->(6,6), D=(5,0)->(7,10) -> comp11=+1.
  BufferSubgraph* c =
      arena.AddSubgraph({{Coordinate(5.0, 0.0), Coordinate(6.0, 6.0),
                          Coordinate(20.0, 0.0), Coordinate(5.0, 0.0)}},
                        -1, 10);
  BufferSubgraph* d =
      arena.AddSubgraph({{Coordinate(5.0, 0.0), Coordinate(7.0, 10.0),
                          Coordinate(25.0, 0.0), Coordinate(5.0, 0.0)}},
                        -1, 20);
  {
    std::vector<BufferSubgraph*> cd{c, d};
    SubgraphDepthLocater loc(&cd);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 3.0)), 10);
  }
  {
    std::vector<BufferSubgraph*> dc{d, c};
    SubgraphDepthLocater loc(&dc);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 3.0)), 10);
  }
}

TEST(SubgraphDepthLocaterTest, DepthSegmentCompareTouchingAtVertexUsesMaxY) {
  SubgraphArena arena;
  BufferSubgraph* lower =
      arena.AddSubgraph({{Coordinate(5.0, 0.0), Coordinate(5.0, 5.0),
                          Coordinate(15.0, 0.0), Coordinate(5.0, 0.0)}},
                        -1, 10);
  BufferSubgraph* upper =
      arena.AddSubgraph({{Coordinate(5.0, 5.0), Coordinate(5.0, 10.0),
                          Coordinate(15.0, 5.0), Coordinate(5.0, 5.0)}},
                        -1, 20);

  // Ray through shared vertex (5,5) hits both vertical segments; maxY breaks
  // the tie in both directions.
  {
    std::vector<BufferSubgraph*> lu{lower, upper};
    SubgraphDepthLocater loc(&lu);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 5.0)), 10);
  }
  {
    std::vector<BufferSubgraph*> ul{upper, lower};
    SubgraphDepthLocater loc(&ul);
    EXPECT_EQ(loc.getDepth(Coordinate(0.0, 5.0)), 10);
  }
}

TEST(SubgraphDepthLocaterTest,
     DepthSegmentCompareIdenticalSegmentsReturnsZero) {
  SubgraphArena arena;
  BufferSubgraph* s1 =
      arena.AddSubgraph({{Coordinate(5.0, 0.0), Coordinate(5.0, 10.0),
                          Coordinate(15.0, 0.0), Coordinate(5.0, 0.0)}},
                        -1, 10);
  BufferSubgraph* s2 =
      arena.AddSubgraph({{Coordinate(5.0, 0.0), Coordinate(5.0, 10.0),
                          Coordinate(15.0, 0.0), Coordinate(5.0, 0.0)}},
                        -1, 10);

  std::vector<BufferSubgraph*> subgraphs{s1, s2};
  SubgraphDepthLocater locater(&subgraphs);
  EXPECT_EQ(locater.getDepth(Coordinate(0.0, 5.0)), 10);
}

}  // namespace
}  // namespace geos_google
