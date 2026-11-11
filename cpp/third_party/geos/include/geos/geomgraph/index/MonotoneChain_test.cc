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

// Unit tests for third_party/geos/include/geos/geomgraph/index/MonotoneChain.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/geomgraph/index/MonotoneChain.h

#include "third_party/geos/include/geos/geomgraph/index/MonotoneChain.h"

#include <initializer_list>
#include <memory>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersectionList.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/index/MonotoneChainEdge.h"
#include "third_party/geos/include/geos/geomgraph/index/SegmentIntersector.h"
#include "third_party/geos/include/geos/geomgraph/index/SweepLineEventObj.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geomgraph::Edge;
using geos::geomgraph::Node;
using geos::geomgraph::index::MonotoneChain;
using geos::geomgraph::index::MonotoneChainEdge;
using geos::geomgraph::index::SegmentIntersector;
using geos::geomgraph::index::SweepLineEventOBJ;

static_assert(std::is_base_of_v<SweepLineEventOBJ, MonotoneChain>);
static_assert(std::has_virtual_destructor_v<MonotoneChain>);
static_assert(!std::is_default_constructible_v<MonotoneChain>);
static_assert(!std::is_copy_constructible_v<MonotoneChain>);
static_assert(!std::is_copy_assignable_v<MonotoneChain>);
static_assert(!std::is_move_constructible_v<MonotoneChain>);
static_assert(!std::is_move_assignable_v<MonotoneChain>);

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(seq.release());
}

TEST(MonotoneChainTest, SupportsPolymorphicDestructionViaBasePointer) {
  auto edge = MakeEdge({{0.0, 0.0}, {1.0, 1.0}});
  MonotoneChainEdge* mce = edge->getMonotoneChainEdge();
  ASSERT_NE(mce, nullptr);

  std::unique_ptr<SweepLineEventOBJ> poly =
      std::make_unique<MonotoneChain>(mce, 0);
  EXPECT_NE(dynamic_cast<MonotoneChain*>(poly.get()), nullptr);
  poly.reset();

  std::unique_ptr<MonotoneChain> direct =
      std::make_unique<MonotoneChain>(mce, 0);
  EXPECT_NE(direct.get(), nullptr);
  direct.reset();

  MonotoneChain stack_chain(mce, 0);
  EXPECT_EQ(mce->getStartIndexes().size(), 2u);
}

TEST(MonotoneChainTest, ComputeIntersectionsPrunesAndFindsProperIntersection) {
  // Single monotone chain of 4 segments in the NE quadrant.
  auto edge_a =
      MakeEdge({{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}, {3.0, 3.0}, {4.0, 4.0}});
  // Single-segment chain intersecting only segment 0 of edge_a at (0.5, 0.5).
  auto edge_b = MakeEdge({{0.2, 0.8}, {0.8, 0.2}});

  MonotoneChainEdge* mce_a = edge_a->getMonotoneChainEdge();
  MonotoneChainEdge* mce_b = edge_b->getMonotoneChainEdge();
  ASSERT_EQ(mce_a->getStartIndexes().size(), 2u);
  ASSERT_EQ(mce_b->getStartIndexes().size(), 2u);

  MonotoneChain mc_a(mce_a, 0);
  MonotoneChain mc_b(mce_b, 0);

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/false);
  mc_a.computeIntersections(&mc_b, &si);

  // Binary search prunes the [2, 4] half-chain (segments 2 and 3), while
  // [0, 2] splits directly into 1x1 leaves [0, 1] and [1, 2] before overlaps
  // is rechecked, so only 2 of the 4 segments are tested.
  EXPECT_EQ(si.numTests, 2);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(si.hasProperInteriorIntersection());
  EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(0.5, 0.5)));
  EXPECT_TRUE(
      edge_a->getEdgeIntersectionList().isIntersection(Coordinate(0.5, 0.5)));
  EXPECT_TRUE(
      edge_b->getEdgeIntersectionList().isIntersection(Coordinate(0.5, 0.5)));
}

TEST(MonotoneChainTest,
     ComputeIntersectionsSplitsBothMultiSegmentChainsAndFindsCrossing) {
  // Both chains have 3 segments in single monotone quadrants (NE and SE).
  auto edge_ne = MakeEdge({{0.0, 0.0}, {1.0, 1.0}, {3.0, 3.0}, {4.0, 4.0}});
  auto edge_se = MakeEdge({{0.0, 4.0}, {1.0, 3.0}, {3.0, 1.0}, {4.0, 0.0}});

  MonotoneChainEdge* mce_ne = edge_ne->getMonotoneChainEdge();
  MonotoneChainEdge* mce_se = edge_se->getMonotoneChainEdge();
  ASSERT_EQ(mce_ne->getStartIndexes().size(), 2u);
  ASSERT_EQ(mce_se->getStartIndexes().size(), 2u);

  MonotoneChain mc_ne(mce_ne, 0);
  MonotoneChain mc_se(mce_se, 0);

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/false);
  mc_ne.computeIntersections(&mc_se, &si);

  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(2.0, 2.0)));
  EXPECT_TRUE(
      edge_ne->getEdgeIntersectionList().isIntersection(Coordinate(2.0, 2.0)));
  EXPECT_TRUE(
      edge_se->getEdgeIntersectionList().isIntersection(Coordinate(2.0, 2.0)));
}

TEST(MonotoneChainTest, ComputeIntersectionsHandlesNonProperEndpointTouch) {
  auto edge_a = MakeEdge({{0.0, 0.0}, {4.0, 0.0}});
  auto edge_b = MakeEdge({{2.0, 0.0}, {2.0, 3.0}});

  MonotoneChain mc_a(edge_a->getMonotoneChainEdge(), 0);
  MonotoneChain mc_b(edge_b->getMonotoneChainEdge(), 0);

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/false);
  mc_a.computeIntersections(&mc_b, &si);

  EXPECT_TRUE(si.hasIntersection());
  EXPECT_FALSE(si.hasProperIntersection());
  EXPECT_FALSE(si.hasProperInteriorIntersection());
  EXPECT_TRUE(
      edge_a->getEdgeIntersectionList().isIntersection(Coordinate(2.0, 0.0)));
  EXPECT_TRUE(
      edge_b->getEdgeIntersectionList().isIntersection(Coordinate(2.0, 0.0)));
}

TEST(MonotoneChainTest, ComputeIntersectionsHandlesDisjointAndParallelChains) {
  auto edge_a = MakeEdge({{0.0, 0.0}, {2.0, 2.0}, {4.0, 4.0}});
  auto edge_disjoint = MakeEdge({{5.0, 5.0}, {6.0, 6.0}});
  auto edge_parallel = MakeEdge({{0.0, 1.0}, {2.0, 3.0}, {4.0, 5.0}});

  MonotoneChain mc_a(edge_a->getMonotoneChainEdge(), 0);
  MonotoneChain mc_disjoint(edge_disjoint->getMonotoneChainEdge(), 0);
  MonotoneChain mc_parallel(edge_parallel->getMonotoneChainEdge(), 0);

  LineIntersector li1;
  SegmentIntersector si_disjoint(&li1, true, false);
  mc_a.computeIntersections(&mc_disjoint, &si_disjoint);
  EXPECT_EQ(si_disjoint.numTests, 0);
  EXPECT_FALSE(si_disjoint.hasIntersection());

  LineIntersector li2;
  SegmentIntersector si_parallel(&li2, true, false);
  mc_a.computeIntersections(&mc_parallel, &si_parallel);
  EXPECT_GT(si_parallel.numTests, 0);
  EXPECT_FALSE(si_parallel.hasIntersection());
}

TEST(MonotoneChainTest, ComputeIntersectionsRespectsSelectedChainIndex) {
  // Chain 0 (NE): (0,0)->(2,2)->(4,4); Chain 1 (SE): (4,4)->(6,2)->(8,0).
  auto edge =
      MakeEdge({{0.0, 0.0}, {2.0, 2.0}, {4.0, 4.0}, {6.0, 2.0}, {8.0, 0.0}});
  MonotoneChainEdge* mce = edge->getMonotoneChainEdge();
  ASSERT_EQ(mce->getStartIndexes().size(), 3u);
  EXPECT_EQ(mce->getStartIndexes()[0], 0u);
  EXPECT_EQ(mce->getStartIndexes()[1], 2u);
  EXPECT_EQ(mce->getStartIndexes()[2], 4u);

  // Probe intersects only chain 1 at vertex (6, 2).
  auto probe = MakeEdge({{5.0, 1.0}, {7.0, 3.0}});
  MonotoneChain chain0(mce, 0);
  MonotoneChain chain1(mce, 1);
  MonotoneChain chain_probe(probe->getMonotoneChainEdge(), 0);

  LineIntersector li0;
  SegmentIntersector si0(&li0, true, false);
  chain0.computeIntersections(&chain_probe, &si0);
  EXPECT_EQ(si0.numTests, 0);
  EXPECT_FALSE(si0.hasIntersection());
  EXPECT_TRUE(edge->getEdgeIntersectionList().isEmpty());

  LineIntersector li1;
  SegmentIntersector si1(&li1, true, false);
  chain1.computeIntersections(&chain_probe, &si1);
  EXPECT_TRUE(si1.hasIntersection());
  EXPECT_TRUE(
      edge->getEdgeIntersectionList().isIntersection(Coordinate(6.0, 2.0)));
  EXPECT_TRUE(
      probe->getEdgeIntersectionList().isIntersection(Coordinate(6.0, 2.0)));
}

TEST(MonotoneChainTest,
     ComputeIntersectionsFindsSelfIntersectionBetweenDistinctChains) {
  // Chain 0 (NE): (0,0)->(4,4), Chain 1 (SE): (4,4)->(4,0), Chain 2 (NW):
  // (4,0)->(0,4).
  auto self_crossing =
      MakeEdge({{0.0, 0.0}, {4.0, 4.0}, {4.0, 0.0}, {0.0, 4.0}});
  MonotoneChainEdge* mce = self_crossing->getMonotoneChainEdge();
  ASSERT_EQ(mce->getStartIndexes().size(), 4u);

  MonotoneChain chain0(mce, 0);
  MonotoneChain chain2(mce, 2);

  // Comparing a single-segment chain with itself skips identical segment pairs.
  LineIntersector li_same;
  SegmentIntersector si_same(&li_same, true, false);
  chain0.computeIntersections(&chain0, &si_same);
  EXPECT_EQ(si_same.numTests, 0);
  EXPECT_FALSE(si_same.hasIntersection());

  LineIntersector li_cross;
  SegmentIntersector si_cross(&li_cross, true, false);
  chain0.computeIntersections(&chain2, &si_cross);
  EXPECT_TRUE(si_cross.hasIntersection());
  EXPECT_TRUE(si_cross.hasProperIntersection());
  EXPECT_TRUE(
      si_cross.getProperIntersectionPoint().equals2D(Coordinate(2.0, 2.0)));
  EXPECT_TRUE(self_crossing->getEdgeIntersectionList().isIntersection(
      Coordinate(2.0, 2.0)));
}

TEST(MonotoneChainTest,
     ComputeIntersectionsHonorsIncludeProperAndRecordIsolatedFlags) {
  auto edge_a = MakeEdge({{0.0, 0.0}, {4.0, 4.0}});
  auto edge_b = MakeEdge({{0.0, 4.0}, {4.0, 0.0}});
  edge_a->setIsolated(true);
  edge_b->setIsolated(true);

  MonotoneChain mc_a(edge_a->getMonotoneChainEdge(), 0);
  MonotoneChain mc_b(edge_b->getMonotoneChainEdge(), 0);

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/false,
                        /*newRecordIsolated=*/true);
  mc_a.computeIntersections(&mc_b, &si);

  EXPECT_FALSE(edge_a->isIsolated());
  EXPECT_FALSE(edge_b->isIsolated());
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(edge_a->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(edge_b->getEdgeIntersectionList().isEmpty());
}

TEST(MonotoneChainTest, ComputeIntersectionsRecordsCollinearOverlapEndpoints) {
  auto edge_a = MakeEdge({{0.0, 0.0}, {2.0, 0.0}, {4.0, 0.0}});
  auto edge_b = MakeEdge({{1.0, 0.0}, {3.0, 0.0}});

  MonotoneChain mc_a(edge_a->getMonotoneChainEdge(), 0);
  MonotoneChain mc_b(edge_b->getMonotoneChainEdge(), 0);

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/false);
  mc_a.computeIntersections(&mc_b, &si);

  EXPECT_TRUE(si.hasIntersection());
  EXPECT_FALSE(si.hasProperIntersection());
  EXPECT_FALSE(si.hasProperInteriorIntersection());
  EXPECT_TRUE(
      edge_a->getEdgeIntersectionList().isIntersection(Coordinate(1.0, 0.0)));
  EXPECT_TRUE(
      edge_a->getEdgeIntersectionList().isIntersection(Coordinate(2.0, 0.0)));
  EXPECT_TRUE(
      edge_a->getEdgeIntersectionList().isIntersection(Coordinate(3.0, 0.0)));
  EXPECT_TRUE(
      edge_b->getEdgeIntersectionList().isIntersection(Coordinate(1.0, 0.0)));
  EXPECT_TRUE(
      edge_b->getEdgeIntersectionList().isIntersection(Coordinate(2.0, 0.0)));
  EXPECT_TRUE(
      edge_b->getEdgeIntersectionList().isIntersection(Coordinate(3.0, 0.0)));
}

TEST(MonotoneChainTest,
     ComputeIntersectionsRespectsBoundaryNodesOnProperCrossing) {
  auto edge_a = MakeEdge({{0.0, 0.0}, {4.0, 4.0}});
  auto edge_b = MakeEdge({{0.0, 4.0}, {4.0, 0.0}});
  Node bdy_node(Coordinate(2.0, 2.0), nullptr);
  std::vector<Node*> bdy0{&bdy_node};

  MonotoneChain mc_a(edge_a->getMonotoneChainEdge(), 0);
  MonotoneChain mc_b(edge_b->getMonotoneChainEdge(), 0);

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/false,
                        /*newRecordIsolated=*/false);
  si.setBoundaryNodes(&bdy0, nullptr);
  mc_a.computeIntersections(&mc_b, &si);

  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_FALSE(si.hasProperInteriorIntersection());
  EXPECT_TRUE(
      edge_a->getEdgeIntersectionList().isIntersection(Coordinate(2.0, 2.0)));
  EXPECT_TRUE(
      edge_b->getEdgeIntersectionList().isIntersection(Coordinate(2.0, 2.0)));
}

}  // namespace
}  // namespace geos_google
