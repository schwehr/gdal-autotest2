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

#include "third_party/geos/include/geos/geomgraph/index/SimpleMCSweepLineIntersector.h"

#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersection.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersectionList.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/index/EdgeSetIntersector.h"
#include "third_party/geos/include/geos/geomgraph/index/SegmentIntersector.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"

namespace geos_google {
namespace {

using ::geos::algorithm::LineIntersector;
using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geomgraph::Edge;
using ::geos::geomgraph::Node;
using ::geos::geomgraph::index::EdgeSetIntersector;
using ::geos::geomgraph::index::SegmentIntersector;
using ::geos::geomgraph::index::SimpleMCSweepLineIntersector;
using ::geos::util::CurrentThreadInterrupt;
using ::geos::util::GEOSException;

static_assert(!std::is_copy_constructible_v<SimpleMCSweepLineIntersector>);
static_assert(!std::is_copy_assignable_v<SimpleMCSweepLineIntersector>);

class InspectableMCSweepLineIntersector final
    : public SimpleMCSweepLineIntersector {
 public:
  int numOverlaps() const { return nOverlaps; }
};

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(seq.release());
}

std::vector<Coordinate> CollectIntersections(const Edge& edge) {
  std::vector<Coordinate> pts;
  for (const auto& ei : edge.getEdgeIntersectionList()) {
    pts.push_back(ei.getCoordinate());
  }
  return pts;
}

TEST(SimpleMCSweepLineIntersectorTest, EmptySingleSet) {
  std::vector<Edge*> edges;

  for (bool test_all : {false, true}) {
    SimpleMCSweepLineIntersector intersector;
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    intersector.computeIntersections(&edges, &si, test_all);
    EXPECT_EQ(si.numTests, 0);
    EXPECT_FALSE(si.hasIntersection());
    EXPECT_FALSE(si.hasProperIntersection());
  }
}

TEST(SimpleMCSweepLineIntersectorTest,
     SingleSelfIntersectingEdgeRespectsTestAllSegments) {
  // Multi-segment monotone chains: NE chain (0,0)-(4,4)-(6,6), SE chain
  // (6,6)-(6,0), NW chain (6,0)-(4,2)-(0,6). Chain 0 crosses chain 2 at (3,3).
  {
    auto edge = MakeEdge({{0, 0}, {4, 4}, {6, 6}, {6, 0}, {4, 2}, {0, 6}});
    std::vector<Edge*> edges = {edge.get()};
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    SimpleMCSweepLineIntersector intersector;

    intersector.computeIntersections(&edges, &si, /*testAllSegments=*/false);
    EXPECT_EQ(si.numTests, 0);
    EXPECT_FALSE(si.hasIntersection());
    EXPECT_TRUE(edge->getEdgeIntersectionList().isEmpty());
    EXPECT_TRUE(edge->isIsolated());
  }

  {
    auto edge = MakeEdge({{0, 0}, {4, 4}, {6, 6}, {6, 0}, {4, 2}, {0, 6}});
    std::vector<Edge*> edges = {edge.get()};
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    SimpleMCSweepLineIntersector intersector;

    intersector.computeIntersections(&edges, &si, /*testAllSegments=*/true);
    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_TRUE(si.hasProperInteriorIntersection());
    EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(3, 3)));
    EXPECT_TRUE(edge->getEdgeIntersectionList().isIntersection({3, 3}));
    EXPECT_FALSE(edge->isIsolated());
  }
}

TEST(SimpleMCSweepLineIntersectorTest,
     SingleSetDisjointXRangesPrunesComparisons) {
  auto e0 = MakeEdge({{-10, 0}, {-8, 4}});
  auto e1 = MakeEdge({{0, 0}, {2, 4}});
  auto e2 = MakeEdge({{10, 0}, {12, 4}});
  std::vector<Edge*> edges = {e0.get(), e1.get(), e2.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  SimpleMCSweepLineIntersector intersector;

  intersector.computeIntersections(&edges, &si, /*testAllSegments=*/true);
  EXPECT_EQ(si.numTests, 0);
  EXPECT_FALSE(si.hasIntersection());
}

TEST(SimpleMCSweepLineIntersectorTest,
     SingleSetNestedXIntervalsExercisesDeleteEventsInSweepRange) {
  auto long_edge = MakeEdge({{0, 0}, {10, 0}});
  auto short_crossing = MakeEdge({{2, -2}, {6, 2}});
  auto short_disjoint = MakeEdge({{3, 5}, {5, 6}});
  std::vector<Edge*> edges = {long_edge.get(), short_crossing.get(),
                              short_disjoint.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  SimpleMCSweepLineIntersector intersector;

  intersector.computeIntersections(&edges, &si, /*testAllSegments=*/false);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(4, 0)));
  EXPECT_TRUE(long_edge->getEdgeIntersectionList().isIntersection({4, 0}));
  EXPECT_TRUE(short_crossing->getEdgeIntersectionList().isIntersection({4, 0}));
  EXPECT_TRUE(short_disjoint->getEdgeIntersectionList().isEmpty());
}

TEST(SimpleMCSweepLineIntersectorTest,
     SingleSetSharedXBoundariesAndVerticalSegments) {
  auto left = MakeEdge({{0, 0}, {5, 5}});
  auto right = MakeEdge({{5, 5}, {10, 0}});
  auto vert_long = MakeEdge({{5, -2}, {5, 8}});
  auto vert_short = MakeEdge({{5, 1}, {5, 4}});
  std::vector<Edge*> edges = {left.get(), right.get(), vert_long.get(),
                              vert_short.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  SimpleMCSweepLineIntersector intersector;

  intersector.computeIntersections(&edges, &si, /*testAllSegments=*/false);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_FALSE(si.hasProperIntersection());
  EXPECT_TRUE(left->getEdgeIntersectionList().isIntersection({5, 5}));
  EXPECT_TRUE(right->getEdgeIntersectionList().isIntersection({5, 5}));
  EXPECT_TRUE(vert_long->getEdgeIntersectionList().isIntersection({5, 5}));
  EXPECT_TRUE(vert_long->getEdgeIntersectionList().isIntersection({5, 1}));
  EXPECT_TRUE(vert_long->getEdgeIntersectionList().isIntersection({5, 4}));
  EXPECT_TRUE(vert_short->getEdgeIntersectionList().isIntersection({5, 1}));
  EXPECT_TRUE(vert_short->getEdgeIntersectionList().isIntersection({5, 4}));
}

TEST(SimpleMCSweepLineIntersectorTest,
     SingleSetMonotoneChainsBinarySearchAndReversedDirection) {
  // Single NE monotone chain of 4 segments vs single NW monotone chain of 4
  // segments ordered right-to-left. They intersect at (3, 3), leaving the
  // upper half of `forward` ([8, 32] x [8, 32]) disjoint from `reversed`
  // ([0, 6] x [0, 6]) so MonotoneChainEdge binary search prunes those halves.
  auto forward = MakeEdge({{0, 0}, {4, 4}, {8, 8}, {20, 20}, {32, 32}});
  auto reversed = MakeEdge({{6, 0}, {5, 1}, {2, 4}, {1, 5}, {0, 6}});
  std::vector<Edge*> edges = {forward.get(), reversed.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  InspectableMCSweepLineIntersector intersector;

  intersector.computeIntersections(&edges, &si, /*testAllSegments=*/false);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(3, 3)));
  EXPECT_EQ(intersector.numOverlaps(), 1);
  // MonotoneChainEdge binary search prunes non-overlapping sub-chains:
  // 8 of 16 segment pairs are tested instead of all 16.
  EXPECT_LT(si.numTests, 16);
  EXPECT_EQ(si.numTests, 8);
  EXPECT_TRUE(forward->getEdgeIntersectionList().isIntersection({3, 3}));
  EXPECT_TRUE(reversed->getEdgeIntersectionList().isIntersection({3, 3}));
}

TEST(SimpleMCSweepLineIntersectorTest, TwoEdgeSetsEmptyCases) {
  auto edge = MakeEdge({{0, 0}, {4, 4}});
  std::vector<Edge*> empty_edges;
  std::vector<Edge*> non_empty = {edge.get()};

  for (auto [lhs, rhs] :
       std::vector<std::pair<std::vector<Edge*>*, std::vector<Edge*>*>>{
           {&empty_edges, &empty_edges},
           {&empty_edges, &non_empty},
           {&non_empty, &empty_edges}}) {
    InspectableMCSweepLineIntersector intersector;
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    intersector.computeIntersections(lhs, rhs, &si);
    EXPECT_EQ(intersector.numOverlaps(), 0);
    EXPECT_EQ(si.numTests, 0);
    EXPECT_FALSE(si.hasIntersection());
  }
}

TEST(SimpleMCSweepLineIntersectorTest,
     TwoEdgeSetsIgnoresIntraSetIntersectionsAndFindsCrossSet) {
  // Disjoint X-ranges: neither intra-set crossings nor cross-set pairs run.
  {
    auto a0 = MakeEdge({{0, 0}, {4, 4}});
    auto a1 = MakeEdge({{0, 4}, {4, 0}});  // Intra-set crossing at (2, 2)
    auto b0 = MakeEdge({{10, 0}, {14, 4}});
    auto b1 = MakeEdge({{10, 4}, {14, 0}});  // Intra-set crossing at (12, 2)
    std::vector<Edge*> edges0 = {a0.get(), a1.get()};
    std::vector<Edge*> edges1 = {b0.get(), b1.get()};

    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    InspectableMCSweepLineIntersector intersector;

    intersector.computeIntersections(&edges0, &edges1, &si);
    EXPECT_EQ(intersector.numOverlaps(), 0);
    EXPECT_EQ(si.numTests, 0);
    EXPECT_FALSE(si.hasIntersection());
    EXPECT_TRUE(a0->getEdgeIntersectionList().isEmpty());
    EXPECT_TRUE(a1->getEdgeIntersectionList().isEmpty());
    EXPECT_TRUE(b0->getEdgeIntersectionList().isEmpty());
    EXPECT_TRUE(b1->getEdgeIntersectionList().isEmpty());
  }

  // Overlapping X-ranges, disjoint Y-ranges: the X-sweep checks only the 4
  // cross-set single-segment pairs and skips both intra-set crossings.
  {
    auto a0 = MakeEdge({{0, 0}, {4, 4}});
    auto a1 = MakeEdge({{0, 4}, {4, 0}});  // Intra-set crossing at (2, 2)
    auto b0 = MakeEdge({{0, 10}, {4, 14}});
    auto b1 = MakeEdge({{0, 14}, {4, 10}});  // Intra-set crossing at (2, 12)
    std::vector<Edge*> edges0 = {a0.get(), a1.get()};
    std::vector<Edge*> edges1 = {b0.get(), b1.get()};

    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    InspectableMCSweepLineIntersector intersector;

    intersector.computeIntersections(&edges0, &edges1, &si);
    EXPECT_EQ(intersector.numOverlaps(), 4);
    EXPECT_EQ(si.numTests, 4);
    EXPECT_FALSE(si.hasIntersection());
    EXPECT_TRUE(a0->getEdgeIntersectionList().isEmpty());
    EXPECT_TRUE(a1->getEdgeIntersectionList().isEmpty());
    EXPECT_TRUE(b0->getEdgeIntersectionList().isEmpty());
    EXPECT_TRUE(b1->getEdgeIntersectionList().isEmpty());
  }

  {
    auto a0 = MakeEdge({{0, 0}, {4, 4}});
    auto a1 = MakeEdge({{0, 4}, {4, 0}});  // Intra-set crossing at (2, 2)
    auto b0 = MakeEdge({{1, 0}, {1, 4}});  // Crosses a0 at (1,1), a1 at (1,3)
    std::vector<Edge*> edges0 = {a0.get(), a1.get()};
    std::vector<Edge*> edges1 = {b0.get()};

    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    InspectableMCSweepLineIntersector intersector;

    intersector.computeIntersections(&edges0, &edges1, &si);
    EXPECT_EQ(intersector.numOverlaps(), 2);
    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_TRUE(a0->getEdgeIntersectionList().isIntersection({1, 1}));
    EXPECT_FALSE(a0->getEdgeIntersectionList().isIntersection({2, 2}));
    EXPECT_TRUE(a1->getEdgeIntersectionList().isIntersection({1, 3}));
    EXPECT_FALSE(a1->getEdgeIntersectionList().isIntersection({2, 2}));

    std::vector<Coordinate> b_pts = CollectIntersections(*b0);
    ASSERT_EQ(b_pts.size(), 2);
    EXPECT_TRUE(b_pts[0].equals2D(Coordinate(1, 1)));
    EXPECT_TRUE(b_pts[1].equals2D(Coordinate(1, 3)));
  }
}

TEST(SimpleMCSweepLineIntersectorTest,
     TwoEdgeSetsCollinearEndpointBoundaryNodeAndPolymorphic) {
  std::unique_ptr<EdgeSetIntersector> esi =
      std::make_unique<SimpleMCSweepLineIntersector>();

  auto a0 = MakeEdge({{0, 0}, {4, 0}});
  auto b0 = MakeEdge({{2, 0}, {6, 0}});   // Collinear overlap [(2,0), (4,0)]
  auto b1 = MakeEdge({{1, -2}, {1, 2}});  // Crosses a0 at boundary node (1, 0)
  Node bdy_node({1, 0}, nullptr);
  std::vector<Node*> bdy_nodes0 = {&bdy_node};
  std::vector<Edge*> edges0 = {a0.get()};
  std::vector<Edge*> edges1 = {b0.get(), b1.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/false,
                        /*newRecordIsolated=*/true);
  si.setBoundaryNodes(&bdy_nodes0, nullptr);

  esi->computeIntersections(&edges0, &edges1, &si);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_FALSE(si.hasProperInteriorIntersection());
  EXPECT_TRUE(a0->getEdgeIntersectionList().isIntersection({1, 0}));
  EXPECT_TRUE(a0->getEdgeIntersectionList().isIntersection({2, 0}));
  EXPECT_TRUE(a0->getEdgeIntersectionList().isIntersection({4, 0}));
  EXPECT_TRUE(b0->getEdgeIntersectionList().isIntersection({2, 0}));
  EXPECT_TRUE(b0->getEdgeIntersectionList().isIntersection({4, 0}));
  EXPECT_TRUE(b1->getEdgeIntersectionList().isIntersection({1, 0}));
}

TEST(SimpleMCSweepLineIntersectorTest, InterruptionDuringSweep) {
  struct InterruptState {
    int count = 0;
    int trigger_at = 0;
  };
  auto cb = [](void* userdata) -> int {
    auto* state = static_cast<InterruptState*>(userdata);
    ++state->count;
    return state->count >= state->trigger_at ? 1 : 0;
  };

  for (int trigger_at : {1, 3, 7}) {
    InterruptState state{0, trigger_at};
    auto* prev = CurrentThreadInterrupt::registerCallback(cb, &state);
    absl::Cleanup restore = [prev] {
      CurrentThreadInterrupt::registerCallback(prev, nullptr);
    };

    auto e0 = MakeEdge({{0, 0}, {4, 4}});
    auto e1 = MakeEdge({{0, 4}, {4, 0}});
    std::vector<Edge*> edges = {e0.get(), e1.get()};
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    SimpleMCSweepLineIntersector intersector;

    EXPECT_THROW(intersector.computeIntersections(&edges, &si,
                                                  /*testAllSegments=*/false),
                 GEOSException);
  }
}

TEST(SimpleMCSweepLineIntersectorTest, ClosedBowtieRespectsTestAllSegments) {
  {
    auto bowtie = MakeEdge({{0, 0}, {4, 4}, {4, 0}, {0, 4}, {0, 0}});
    std::vector<Edge*> edges = {bowtie.get()};
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    InspectableMCSweepLineIntersector intersector;

    intersector.computeIntersections(&edges, &si, /*testAllSegments=*/false);
    EXPECT_EQ(intersector.numOverlaps(), 0);
    EXPECT_EQ(si.numTests, 0);
    EXPECT_FALSE(si.hasIntersection());
    EXPECT_TRUE(bowtie->getEdgeIntersectionList().isEmpty());
    EXPECT_TRUE(bowtie->isIsolated());
  }

  {
    auto bowtie = MakeEdge({{0, 0}, {4, 4}, {4, 0}, {0, 4}, {0, 0}});
    std::vector<Edge*> edges = {bowtie.get()};
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    InspectableMCSweepLineIntersector intersector;

    intersector.computeIntersections(&edges, &si, /*testAllSegments=*/true);
    EXPECT_GT(intersector.numOverlaps(), 0);
    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_TRUE(si.hasProperInteriorIntersection());
    EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(2, 2)));
    EXPECT_TRUE(bowtie->getEdgeIntersectionList().isIntersection({2, 2}));
    EXPECT_FALSE(bowtie->isIsolated());
  }
}

}  // namespace
}  // namespace geos_google
