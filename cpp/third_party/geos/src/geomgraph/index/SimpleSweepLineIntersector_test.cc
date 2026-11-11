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

#include "third_party/geos/include/geos/geomgraph/index/SimpleSweepLineIntersector.h"

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersection.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersectionList.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/index/EdgeSetIntersector.h"
#include "third_party/geos/include/geos/geomgraph/index/SegmentIntersector.h"
#include "third_party/geos/include/geos/geomgraph/index/SweepLineEvent.h"
#include "third_party/geos/include/geos/geomgraph/index/SweepLineEventObj.h"

namespace geos_google {
namespace {

using ::geos::algorithm::LineIntersector;
using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geomgraph::Edge;
using ::geos::geomgraph::Node;
using ::geos::geomgraph::index::EdgeSetIntersector;
using ::geos::geomgraph::index::SegmentIntersector;
using ::geos::geomgraph::index::SimpleSweepLineIntersector;
using ::geos::geomgraph::index::SweepLineEvent;
using ::geos::geomgraph::index::SweepLineEventOBJ;

// Upstream SimpleSweepLineIntersector::~SimpleSweepLineIntersector() deletes
// each SweepLineEvent* in `events`, but SweepLineEvent::~SweepLineEvent() is
// defaulted and never frees the SweepLineSegment allocated in add(). Reclaim
// those SweepLineEventOBJ instances on teardown via standard C++ explicit
// instantiation member access so HeapLeakChecker passes cleanly.
template <typename Tag, typename Tag::Type Member>
struct PrivateMemberRobber {
  friend typename Tag::Type AccessPrivateMember(Tag) { return Member; }
};

struct SimpleSweepLineEventsTag {
  using Type = std::vector<SweepLineEvent*> SimpleSweepLineIntersector::*;
  friend Type AccessPrivateMember(SimpleSweepLineEventsTag);
};

template struct PrivateMemberRobber<SimpleSweepLineEventsTag,
                                    &SimpleSweepLineIntersector::events>;

void ReclaimSweepLineObjects(SimpleSweepLineIntersector& intersector) {
  auto& events = intersector.*AccessPrivateMember(SimpleSweepLineEventsTag{});
  for (SweepLineEvent* ev : events) {
    if (ev != nullptr && ev->isInsert()) {
      SweepLineEventOBJ* obj = ev->getObject();
      delete obj;
    }
  }
}

class ScopedSimpleSweepLineIntersector {
 public:
  ScopedSimpleSweepLineIntersector() = default;
  ScopedSimpleSweepLineIntersector(const ScopedSimpleSweepLineIntersector&) =
      delete;
  ScopedSimpleSweepLineIntersector& operator=(
      const ScopedSimpleSweepLineIntersector&) = delete;

  ~ScopedSimpleSweepLineIntersector() { ReclaimSweepLineObjects(intersector_); }

  SimpleSweepLineIntersector* operator->() { return &intersector_; }

 private:
  SimpleSweepLineIntersector intersector_;
};

struct ReclaimingEdgeSetDeleter {
  SimpleSweepLineIntersector* concrete = nullptr;

  void operator()(EdgeSetIntersector* ptr) const {
    if (concrete != nullptr) {
      ReclaimSweepLineObjects(*concrete);
    }
    delete ptr;
  }
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

TEST(SimpleSweepLineIntersectorTest, EmptySingleSet) {
  std::vector<Edge*> edges;

  for (bool test_all : {false, true}) {
    ScopedSimpleSweepLineIntersector intersector;
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    intersector->computeIntersections(&edges, &si, test_all);
    EXPECT_EQ(si.numTests, 0);
    EXPECT_FALSE(si.hasIntersection());
    EXPECT_FALSE(si.hasProperIntersection());
  }
}

TEST(SimpleSweepLineIntersectorTest,
     SingleSelfIntersectingEdgeRespectsTestAllSegments) {
  {
    auto edge = MakeEdge({{0, 0}, {4, 4}, {4, 0}, {0, 4}});
    std::vector<Edge*> edges = {edge.get()};
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    ScopedSimpleSweepLineIntersector intersector;

    intersector->computeIntersections(&edges, &si, /*testAllSegments=*/false);
    EXPECT_EQ(si.numTests, 0);
    EXPECT_FALSE(si.hasIntersection());
    EXPECT_TRUE(edge->getEdgeIntersectionList().isEmpty());
    EXPECT_TRUE(edge->isIsolated());
  }

  {
    auto edge = MakeEdge({{0, 0}, {4, 4}, {4, 0}, {0, 4}});
    std::vector<Edge*> edges = {edge.get()};
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    ScopedSimpleSweepLineIntersector intersector;

    intersector->computeIntersections(&edges, &si, /*testAllSegments=*/true);
    EXPECT_GT(si.numTests, 0);
    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_TRUE(si.hasProperInteriorIntersection());
    EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(2, 2)));
    EXPECT_TRUE(edge->getEdgeIntersectionList().isIntersection({2, 2}));
    EXPECT_FALSE(edge->isIsolated());
  }
}

TEST(SimpleSweepLineIntersectorTest,
     SingleSetDisjointXRangesPrunesComparisons) {
  auto e0 = MakeEdge({{-10, 0}, {-8, 4}});
  auto e1 = MakeEdge({{0, 0}, {2, 4}});
  auto e2 = MakeEdge({{10, 0}, {12, 4}});
  std::vector<Edge*> edges = {e0.get(), e1.get(), e2.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  ScopedSimpleSweepLineIntersector intersector;

  intersector->computeIntersections(&edges, &si, /*testAllSegments=*/true);
  EXPECT_EQ(si.numTests, 0);
  EXPECT_FALSE(si.hasIntersection());
}

TEST(SimpleSweepLineIntersectorTest,
     SingleSetNestedXIntervalsExercisesDeleteEventsInSweepRange) {
  auto long_edge = MakeEdge({{0, 0}, {10, 0}});
  auto short_crossing = MakeEdge({{2, -2}, {6, 2}});
  auto short_disjoint = MakeEdge({{3, 5}, {5, 6}});
  std::vector<Edge*> edges = {long_edge.get(), short_crossing.get(),
                              short_disjoint.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  ScopedSimpleSweepLineIntersector intersector;

  intersector->computeIntersections(&edges, &si, /*testAllSegments=*/false);
  // Distinct X intervals yield deterministic sweep pairs:
  // long vs short_crossing, long vs short_disjoint, short_crossing vs
  // short_disjoint.
  EXPECT_EQ(si.numTests, 3);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(4, 0)));
  EXPECT_TRUE(long_edge->getEdgeIntersectionList().isIntersection({4, 0}));
  EXPECT_TRUE(short_crossing->getEdgeIntersectionList().isIntersection({4, 0}));
  EXPECT_TRUE(short_disjoint->getEdgeIntersectionList().isEmpty());
}

TEST(SimpleSweepLineIntersectorTest,
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
  ScopedSimpleSweepLineIntersector intersector;

  intersector->computeIntersections(&edges, &si, /*testAllSegments=*/false);
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

TEST(SimpleSweepLineIntersectorTest, SingleSetReversedDirectionSegments) {
  auto e0 = MakeEdge({{10, 0}, {0, 10}});
  auto e1 = MakeEdge({{10, 10}, {0, 0}});
  std::vector<Edge*> edges = {e0.get(), e1.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  ScopedSimpleSweepLineIntersector intersector;

  intersector->computeIntersections(&edges, &si, /*testAllSegments=*/false);
  EXPECT_EQ(si.numTests, 1);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(5, 5)));
  EXPECT_TRUE(e0->getEdgeIntersectionList().isIntersection({5, 5}));
  EXPECT_TRUE(e1->getEdgeIntersectionList().isIntersection({5, 5}));
}

TEST(SimpleSweepLineIntersectorTest, TwoEdgeSetsEmptyCases) {
  auto edge = MakeEdge({{0, 0}, {4, 4}});
  std::vector<Edge*> empty_edges;
  std::vector<Edge*> non_empty = {edge.get()};

  for (auto [lhs, rhs] :
       std::vector<std::pair<std::vector<Edge*>*, std::vector<Edge*>*>>{
           {&empty_edges, &empty_edges},
           {&empty_edges, &non_empty},
           {&non_empty, &empty_edges}}) {
    ScopedSimpleSweepLineIntersector intersector;
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    intersector->computeIntersections(lhs, rhs, &si);
    EXPECT_EQ(si.numTests, 0);
    EXPECT_FALSE(si.hasIntersection());
  }
}

TEST(SimpleSweepLineIntersectorTest,
     TwoEdgeSetsIgnoresIntraSetIntersectionsAndFindsCrossSet) {
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
    ScopedSimpleSweepLineIntersector intersector;

    intersector->computeIntersections(&edges0, &edges1, &si);
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
    ScopedSimpleSweepLineIntersector intersector;

    intersector->computeIntersections(&edges0, &edges1, &si);
    EXPECT_EQ(si.numTests, 2);
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

TEST(SimpleSweepLineIntersectorTest,
     TwoEdgeSetsCollinearEndpointBoundaryNodeAndPolymorphic) {
  auto raw = std::make_unique<SimpleSweepLineIntersector>();
  SimpleSweepLineIntersector* concrete = raw.get();
  std::unique_ptr<EdgeSetIntersector, ReclaimingEdgeSetDeleter> esi(
      raw.release(), ReclaimingEdgeSetDeleter{concrete});

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
  EXPECT_EQ(si.numTests, 2);
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

TEST(SimpleSweepLineIntersectorTest,
     MultiSegmentPolylinesAndSelfIntersectionInTwoSet) {
  {
    auto e0 = MakeEdge({{0, 0}, {2, 4}, {4, 0}, {6, 4}});
    auto e1 = MakeEdge({{0, 2}, {6, 2}});
    auto e2 = MakeEdge({{1, 10}, {3, 12}, {5, 10}});
    std::vector<Edge*> edges = {e0.get(), e1.get(), e2.get()};

    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    ScopedSimpleSweepLineIntersector intersector;

    intersector->computeIntersections(&edges, &si, /*testAllSegments=*/false);
    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_TRUE(e0->getEdgeIntersectionList().isIntersection({1, 2}));
    EXPECT_TRUE(e0->getEdgeIntersectionList().isIntersection({3, 2}));
    EXPECT_TRUE(e0->getEdgeIntersectionList().isIntersection({5, 2}));
    EXPECT_EQ(CollectIntersections(*e1).size(), 3u);
    EXPECT_TRUE(e1->getEdgeIntersectionList().isIntersection({1, 2}));
    EXPECT_TRUE(e1->getEdgeIntersectionList().isIntersection({3, 2}));
    EXPECT_TRUE(e1->getEdgeIntersectionList().isIntersection({5, 2}));
    EXPECT_TRUE(e2->getEdgeIntersectionList().isEmpty());
  }

  {
    auto a0 = MakeEdge({{0, 0}, {4, 4}, {4, 0}, {0, 4}});
    auto b0 = MakeEdge({{1, 0}, {1, 4}, {3, 4}, {3, 0}});
    std::vector<Edge*> edges0 = {a0.get()};
    std::vector<Edge*> edges1 = {b0.get()};

    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    ScopedSimpleSweepLineIntersector intersector;

    intersector->computeIntersections(&edges0, &edges1, &si);
    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_FALSE(a0->getEdgeIntersectionList().isIntersection({2, 2}));
    EXPECT_TRUE(a0->getEdgeIntersectionList().isIntersection({1, 1}));
    EXPECT_TRUE(a0->getEdgeIntersectionList().isIntersection({1, 3}));
    EXPECT_TRUE(a0->getEdgeIntersectionList().isIntersection({3, 1}));
    EXPECT_TRUE(a0->getEdgeIntersectionList().isIntersection({3, 3}));
    EXPECT_EQ(CollectIntersections(*b0).size(), 4u);
  }
}

TEST(SimpleSweepLineIntersectorTest, LargeScaleDisjointAndUnsortedEdges) {
  auto e0 = MakeEdge({{-1000000.0, 0.0}, {-999990.0, 10.0}});
  auto e1 = MakeEdge({{5.0, 0.0}, {5.0, 10.0}});
  auto e2 = MakeEdge({{0.0, 5.0}, {10.0, 5.0}});
  auto e3 = MakeEdge({{1000000.0, 0.0}, {1000010.0, 10.0}});
  std::vector<Edge*> edges = {e3.get(), e1.get(), e2.get(), e0.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  ScopedSimpleSweepLineIntersector intersector;

  intersector->computeIntersections(&edges, &si, /*testAllSegments=*/false);
  EXPECT_EQ(si.numTests, 1);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(5, 5)));
  EXPECT_TRUE(e0->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(e1->getEdgeIntersectionList().isIntersection({5, 5}));
  EXPECT_TRUE(e2->getEdgeIntersectionList().isIntersection({5, 5}));
  EXPECT_TRUE(e3->getEdgeIntersectionList().isEmpty());
}

TEST(SimpleSweepLineIntersectorTest,
     TwoEdgeSetsWithIdenticalVectorPointerSkipsAllComparisons) {
  auto e0 = MakeEdge({{0, 0}, {4, 4}});
  auto e1 = MakeEdge({{0, 4}, {4, 0}});
  std::vector<Edge*> edges = {e0.get(), e1.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  ScopedSimpleSweepLineIntersector intersector;

  intersector->computeIntersections(&edges, &edges, &si);
  EXPECT_EQ(si.numTests, 0);
  EXPECT_FALSE(si.hasIntersection());
  EXPECT_TRUE(e0->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(e1->getEdgeIntersectionList().isEmpty());
}

}  // namespace
}  // namespace geos_google
