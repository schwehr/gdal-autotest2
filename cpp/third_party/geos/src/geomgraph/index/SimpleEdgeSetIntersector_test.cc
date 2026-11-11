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

#include "third_party/geos/include/geos/geomgraph/index/SimpleEdgeSetIntersector.h"

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

namespace geos_google {
namespace {

using ::geos::algorithm::LineIntersector;
using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geomgraph::Edge;
using ::geos::geomgraph::Node;
using ::geos::geomgraph::index::EdgeSetIntersector;
using ::geos::geomgraph::index::SegmentIntersector;
using ::geos::geomgraph::index::SimpleEdgeSetIntersector;

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

TEST(SimpleEdgeSetIntersectorTest, EmptySingleSet) {
  std::vector<Edge*> edges;
  SimpleEdgeSetIntersector intersector;

  {
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    intersector.computeIntersections(&edges, &si, /*testAllSegments=*/false);
    EXPECT_EQ(si.numTests, 0);
    EXPECT_FALSE(si.hasIntersection());
    EXPECT_FALSE(si.hasProperIntersection());
  }

  {
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    intersector.computeIntersections(&edges, &si, /*testAllSegments=*/true);
    EXPECT_EQ(si.numTests, 0);
    EXPECT_FALSE(si.hasIntersection());
    EXPECT_FALSE(si.hasProperIntersection());
  }
}

TEST(SimpleEdgeSetIntersectorTest,
     SingleSelfIntersectingEdgeRespectsTestAllSegments) {
  {
    auto edge = MakeEdge({{0, 0}, {4, 4}, {4, 0}, {0, 4}});
    std::vector<Edge*> edges = {edge.get()};
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    SimpleEdgeSetIntersector intersector;

    intersector.computeIntersections(&edges, &si, /*testAllSegments=*/false);
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
    SimpleEdgeSetIntersector intersector;

    intersector.computeIntersections(&edges, &si, /*testAllSegments=*/true);
    // 3 segments tested against 3 segments minus 3 identical (seg, seg) pairs.
    EXPECT_EQ(si.numTests, 6);
    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_TRUE(si.hasProperInteriorIntersection());
    EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(2, 2)));
    EXPECT_TRUE(edge->getEdgeIntersectionList().isIntersection({2, 2}));
    EXPECT_FALSE(edge->isIsolated());
  }
}

TEST(SimpleEdgeSetIntersectorTest,
     SingleSetMultipleEdgesExcludesSelfWhenFlagFalse) {
  auto self_crossing = MakeEdge({{0, 0}, {4, 4}, {4, 0}, {0, 4}});
  auto disjoint = MakeEdge({{10, 0}, {10, 4}});
  std::vector<Edge*> edges = {self_crossing.get(), disjoint.get()};

  {
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    SimpleEdgeSetIntersector intersector;

    intersector.computeIntersections(&edges, &si, /*testAllSegments=*/false);
    // (self_crossing, disjoint) and (disjoint, self_crossing): 2 * (3 * 1) = 6.
    EXPECT_EQ(si.numTests, 6);
    EXPECT_FALSE(si.hasIntersection());
    EXPECT_TRUE(self_crossing->getEdgeIntersectionList().isEmpty());
    EXPECT_TRUE(disjoint->getEdgeIntersectionList().isEmpty());
  }

  {
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    SimpleEdgeSetIntersector intersector;

    intersector.computeIntersections(&edges, &si, /*testAllSegments=*/true);
    // 4 total segments -> 4 * 4 - 4 self-segment pairs = 12 tests.
    EXPECT_EQ(si.numTests, 12);
    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(2, 2)));
    EXPECT_TRUE(
        self_crossing->getEdgeIntersectionList().isIntersection({2, 2}));
    EXPECT_TRUE(disjoint->getEdgeIntersectionList().isEmpty());
  }
}

TEST(SimpleEdgeSetIntersectorTest,
     SingleSetMutualIntersectionAndDuplicateEdgePointer) {
  auto e0 = MakeEdge({{-4, -4}, {0, 0}});
  auto e1 = MakeEdge({{-4, 0}, {0, -4}});
  std::vector<Edge*> edges = {e0.get(), e1.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  SimpleEdgeSetIntersector intersector;

  intersector.computeIntersections(&edges, &si, /*testAllSegments=*/false);
  EXPECT_EQ(si.numTests, 2);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(-2, -2)));
  EXPECT_TRUE(e0->getEdgeIntersectionList().isIntersection({-2, -2}));
  EXPECT_TRUE(e1->getEdgeIntersectionList().isIntersection({-2, -2}));
  EXPECT_FALSE(e0->isIsolated());
  EXPECT_FALSE(e1->isIsolated());

  // Duplicate pointer in vector still satisfies edge0 == edge1.
  auto e2 = MakeEdge({{0, 0}, {2, 2}, {2, 0}, {0, 2}});
  std::vector<Edge*> dup_edges = {e2.get(), e2.get()};
  LineIntersector li2;
  SegmentIntersector si2(&li2, /*newIncludeProper=*/true,
                         /*newRecordIsolated=*/true);
  intersector.computeIntersections(&dup_edges, &si2, /*testAllSegments=*/false);
  EXPECT_EQ(si2.numTests, 0);
  EXPECT_FALSE(si2.hasIntersection());
}

TEST(SimpleEdgeSetIntersectorTest, TwoEdgeSetsEmptyCases) {
  auto edge = MakeEdge({{0, 0}, {4, 4}});
  std::vector<Edge*> empty_edges;
  std::vector<Edge*> non_empty = {edge.get()};
  SimpleEdgeSetIntersector intersector;

  for (auto [lhs, rhs] :
       std::vector<std::pair<std::vector<Edge*>*, std::vector<Edge*>*>>{
           {&empty_edges, &empty_edges},
           {&empty_edges, &non_empty},
           {&non_empty, &empty_edges}}) {
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);
    intersector.computeIntersections(lhs, rhs, &si);
    EXPECT_EQ(si.numTests, 0);
    EXPECT_FALSE(si.hasIntersection());
  }
}

TEST(SimpleEdgeSetIntersectorTest,
     TwoEdgeSetsBruteForceTestsDisjointEnvelopes) {
  auto e0 = MakeEdge({{0, 0}, {1, 1}, {2, 0}});
  auto e1 = MakeEdge({{100, 0}, {101, 1}, {102, 0}});
  std::vector<Edge*> edges0 = {e0.get()};
  std::vector<Edge*> edges1 = {e1.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  SimpleEdgeSetIntersector intersector;

  intersector.computeIntersections(&edges0, &edges1, &si);
  // Brute force checks every segment pair regardless of envelope separation.
  EXPECT_EQ(si.numTests, 4);
  EXPECT_FALSE(si.hasIntersection());
  EXPECT_TRUE(e0->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(e1->getEdgeIntersectionList().isEmpty());
}

TEST(SimpleEdgeSetIntersectorTest, TwoEdgeSetsIgnoresIntraSetIntersections) {
  auto a0 = MakeEdge({{0, 0}, {4, 4}});
  auto a1 = MakeEdge({{0, 4}, {4, 0}});  // Crosses a0 at (2, 2)
  auto b0 = MakeEdge({{20, 0}, {24, 4}});
  auto b1 = MakeEdge({{20, 4}, {24, 0}});  // Crosses b0 at (22, 2)
  std::vector<Edge*> edges0 = {a0.get(), a1.get()};
  std::vector<Edge*> edges1 = {b0.get(), b1.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  SimpleEdgeSetIntersector intersector;

  intersector.computeIntersections(&edges0, &edges1, &si);
  EXPECT_EQ(si.numTests, 4);
  EXPECT_FALSE(si.hasIntersection());
  EXPECT_TRUE(a0->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(a1->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(b0->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(b1->getEdgeIntersectionList().isEmpty());
}

TEST(SimpleEdgeSetIntersectorTest,
     TwoEdgeSetsFindsProperEndpointAndCollinearIntersections) {
  auto a_cross = MakeEdge({{0, 0}, {4, 4}});
  auto a_collinear = MakeEdge({{10, 0}, {14, 0}});
  auto b_cross = MakeEdge({{0, 4}, {4, 0}});
  auto b_touch = MakeEdge({{4, 4}, {6, 4}});
  auto b_collinear = MakeEdge({{12, 0}, {16, 0}});
  std::vector<Edge*> edges0 = {a_cross.get(), a_collinear.get()};
  std::vector<Edge*> edges1 = {b_cross.get(), b_touch.get(), b_collinear.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);
  SimpleEdgeSetIntersector intersector;

  intersector.computeIntersections(&edges0, &edges1, &si);
  EXPECT_EQ(si.numTests, 6);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(2, 2)));

  std::vector<Coordinate> cross_pts = CollectIntersections(*a_cross);
  ASSERT_EQ(cross_pts.size(), 2);
  EXPECT_TRUE(cross_pts[0].equals2D(Coordinate(2, 2)));
  EXPECT_TRUE(cross_pts[1].equals2D(Coordinate(4, 4)));
  EXPECT_TRUE(b_cross->getEdgeIntersectionList().isIntersection({2, 2}));
  EXPECT_TRUE(b_touch->getEdgeIntersectionList().isIntersection({4, 4}));

  std::vector<Coordinate> col_a_pts = CollectIntersections(*a_collinear);
  ASSERT_EQ(col_a_pts.size(), 2);
  EXPECT_TRUE(col_a_pts[0].equals2D(Coordinate(12, 0)));
  EXPECT_TRUE(col_a_pts[1].equals2D(Coordinate(14, 0)));
  EXPECT_TRUE(b_collinear->getEdgeIntersectionList().isIntersection({12, 0}));
  EXPECT_TRUE(b_collinear->getEdgeIntersectionList().isIntersection({14, 0}));
}

TEST(SimpleEdgeSetIntersectorTest, PolymorphicReuseAndBoundaryNodeHandling) {
  std::unique_ptr<EdgeSetIntersector> esi =
      std::make_unique<SimpleEdgeSetIntersector>();

  auto e0 = MakeEdge({{0, 0}, {4, 0}});
  auto e1 = MakeEdge({{2, -2}, {2, 2}});
  Node bdy_node({2, 0}, nullptr);
  std::vector<Node*> bdy_nodes0 = {&bdy_node};
  std::vector<Edge*> edges0 = {e0.get()};
  std::vector<Edge*> edges1 = {e1.get()};

  {
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/false,
                          /*newRecordIsolated=*/true);
    si.setBoundaryNodes(&bdy_nodes0, nullptr);
    esi->computeIntersections(&edges0, &edges1, &si);
    EXPECT_EQ(si.numTests, 1);
    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_FALSE(si.hasProperInteriorIntersection());
    // Boundary node causes SegmentIntersector to record it despite
    // includeProper=false.
    EXPECT_TRUE(e0->getEdgeIntersectionList().isIntersection({2, 0}));
    EXPECT_TRUE(e1->getEdgeIntersectionList().isIntersection({2, 0}));
  }

  // Reusing the same intersector on disjoint edges leaves no stale state.
  auto d0 = MakeEdge({{0, 0}, {1, 0}});
  auto d1 = MakeEdge({{0, 5}, {1, 5}});
  std::vector<Edge*> disjoint0 = {d0.get()};
  std::vector<Edge*> disjoint1 = {d1.get()};
  {
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/false);
    esi->computeIntersections(&disjoint0, &disjoint1, &si);
    EXPECT_EQ(si.numTests, 1);
    EXPECT_FALSE(si.hasIntersection());
    EXPECT_TRUE(d0->isIsolated());
    EXPECT_TRUE(d1->isIsolated());
  }
}

}  // namespace
}  // namespace geos_google
