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

#include "third_party/geos/include/geos/geomgraph/index/SegmentIntersector.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersection.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersectionList.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geomgraph::Edge;
using geos::geomgraph::Node;
using geos::geomgraph::index::SegmentIntersector;
using testing::ElementsAre;

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(seq.release());
}

std::vector<Coordinate> CollectIntersectionCoords(const Edge& edge) {
  std::vector<Coordinate> coords;
  for (const auto& ei : edge.getEdgeIntersectionList()) {
    coords.push_back(ei.getCoordinate());
  }
  return coords;
}

TEST(SegmentIntersectorTest, IsAdjacentSegmentsChecksIndexDifferenceOfOne) {
  EXPECT_TRUE(SegmentIntersector::isAdjacentSegments(0, 1));
  EXPECT_TRUE(SegmentIntersector::isAdjacentSegments(1, 0));
  EXPECT_TRUE(SegmentIntersector::isAdjacentSegments(4, 5));
  EXPECT_TRUE(SegmentIntersector::isAdjacentSegments(5, 4));

  EXPECT_FALSE(SegmentIntersector::isAdjacentSegments(0, 0));
  EXPECT_FALSE(SegmentIntersector::isAdjacentSegments(0, 2));
  EXPECT_FALSE(SegmentIntersector::isAdjacentSegments(2, 0));
}

TEST(SegmentIntersectorTest, InitialStateAndIsDoneDefaults) {
  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);

  EXPECT_EQ(si.numTests, 0);
  EXPECT_FALSE(si.hasIntersection());
  EXPECT_FALSE(si.hasProperIntersection());
  EXPECT_FALSE(si.hasProperInteriorIntersection());
  EXPECT_FALSE(si.getIsDone());
}

TEST(SegmentIntersectorTest, SameEdgeSameSegmentReturnsEarlyWithoutTesting) {
  auto edge = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 4.0)});
  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);

  si.addIntersections(edge.get(), 0, edge.get(), 0);

  EXPECT_EQ(si.numTests, 0);
  EXPECT_FALSE(si.hasIntersection());
  EXPECT_TRUE(edge->isIsolated());
  EXPECT_TRUE(edge->getEdgeIntersectionList().isEmpty());
}

TEST(SegmentIntersectorTest,
     DisjointSegmentsIncrementTestsWithoutRecordingIntersections) {
  auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(2.0, 0.0)});
  auto e1 = MakeEdge({Coordinate(0.0, 3.0), Coordinate(2.0, 3.0)});
  auto e_multi = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(1.0, 0.0),
      Coordinate(2.0, 2.0),
      Coordinate(3.0, 2.0),
  });

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);

  si.addIntersections(e0.get(), 0, e1.get(), 0);
  si.addIntersections(e_multi.get(), 0, e_multi.get(), 2);

  EXPECT_EQ(si.numTests, 2);
  EXPECT_FALSE(si.hasIntersection());
  EXPECT_FALSE(si.hasProperIntersection());
  EXPECT_FALSE(si.hasProperInteriorIntersection());
  EXPECT_TRUE(e0->isIsolated());
  EXPECT_TRUE(e1->isIsolated());
  EXPECT_TRUE(e_multi->isIsolated());
  EXPECT_TRUE(e0->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(e1->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(e_multi->getEdgeIntersectionList().isEmpty());
}

TEST(SegmentIntersectorTest,
     RecordIsolatedControlsWhetherIntersectingEdgesAreCleared) {
  {
    auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 4.0)});
    auto e1 = MakeEdge({Coordinate(0.0, 4.0), Coordinate(4.0, 0.0)});
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/true);

    si.addIntersections(e0.get(), 0, e1.get(), 0);

    EXPECT_FALSE(e0->isIsolated());
    EXPECT_FALSE(e1->isIsolated());
  }

  {
    auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 4.0)});
    auto e1 = MakeEdge({Coordinate(0.0, 4.0), Coordinate(4.0, 0.0)});
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/false);

    si.addIntersections(e0.get(), 0, e1.get(), 0);

    EXPECT_TRUE(e0->isIsolated());
    EXPECT_TRUE(e1->isIsolated());
  }
}

TEST(SegmentIntersectorTest, TrivialAndNonTrivialSelfIntersectionsOnSameEdge) {
  // Adjacent segments on an open edge meeting only at their shared vertex.
  auto open_edge = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(2.0, 0.0),
      Coordinate(4.0, 2.0),
  });
  LineIntersector li_open;
  SegmentIntersector si_open(&li_open, /*newIncludeProper=*/true,
                             /*newRecordIsolated=*/true);

  si_open.addIntersections(open_edge.get(), 0, open_edge.get(), 1);
  si_open.addIntersections(open_edge.get(), 1, open_edge.get(), 0);

  EXPECT_EQ(si_open.numTests, 2);
  EXPECT_FALSE(open_edge->isIsolated());
  EXPECT_FALSE(si_open.hasIntersection());
  EXPECT_FALSE(si_open.hasProperIntersection());
  EXPECT_TRUE(open_edge->getEdgeIntersectionList().isEmpty());

  // Adjacent collinear overlapping segments on the same edge yield 2
  // intersection points, so the overlap is non-trivial.
  auto folded = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(4.0, 0.0),
      Coordinate(2.0, 0.0),
  });
  LineIntersector li_folded;
  SegmentIntersector si_folded(&li_folded, /*newIncludeProper=*/false,
                               /*newRecordIsolated=*/false);

  si_folded.addIntersections(folded.get(), 0, folded.get(), 1);

  EXPECT_EQ(si_folded.numTests, 1);
  EXPECT_TRUE(si_folded.hasIntersection());
  EXPECT_FALSE(si_folded.hasProperIntersection());
  EXPECT_THAT(CollectIntersectionCoords(*folded),
              ElementsAre(Coordinate(2.0, 0.0), Coordinate(4.0, 0.0),
                          Coordinate(2.0, 0.0)));

  // Non-adjacent crossing segments on an open edge are non-trivial.
  auto self_cross = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(4.0, 0.0),
      Coordinate(4.0, 2.0),
      Coordinate(0.0, -2.0),
  });
  LineIntersector li_cross;
  SegmentIntersector si_cross(&li_cross, /*newIncludeProper=*/true,
                              /*newRecordIsolated=*/false);

  si_cross.addIntersections(self_cross.get(), 0, self_cross.get(), 2);

  EXPECT_TRUE(si_cross.hasIntersection());
  EXPECT_TRUE(si_cross.hasProperIntersection());
  EXPECT_TRUE(si_cross.hasProperInteriorIntersection());
  EXPECT_EQ(si_cross.getProperIntersectionPoint(), Coordinate(2.0, 0.0));

  // Closed edge: adjacent segments 1 & 2 are trivial; closure check on
  // segments 0 & 2 runs with both segment orderings.
  auto closed_ring = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(4.0, 0.0),
      Coordinate(0.0, 4.0),
      Coordinate(0.0, 0.0),
  });
  ASSERT_TRUE(closed_ring->isClosed());

  LineIntersector li_adj_closed;
  SegmentIntersector si_adj_closed(&li_adj_closed, /*newIncludeProper=*/true,
                                   /*newRecordIsolated=*/false);
  si_adj_closed.addIntersections(closed_ring.get(), 1, closed_ring.get(), 2);
  EXPECT_FALSE(si_adj_closed.hasIntersection());

  LineIntersector li_closed_ends;
  SegmentIntersector si_closed_ends(&li_closed_ends, /*newIncludeProper=*/false,
                                    /*newRecordIsolated=*/false);
  si_closed_ends.addIntersections(closed_ring.get(), 0, closed_ring.get(), 2);
  si_closed_ends.addIntersections(closed_ring.get(), 2, closed_ring.get(), 0);
  EXPECT_EQ(si_closed_ends.numTests, 2);
  EXPECT_TRUE(si_closed_ends.hasIntersection());
  EXPECT_FALSE(si_closed_ends.hasProperIntersection());
  EXPECT_FALSE(closed_ring->getEdgeIntersectionList().isEmpty());
}

TEST(SegmentIntersectorTest, ProperIntersectionRespectsIncludeProperFlag) {
  {
    auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 4.0)});
    auto e1 = MakeEdge({Coordinate(0.0, 4.0), Coordinate(4.0, 0.0)});
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/false,
                          /*newRecordIsolated=*/false);

    si.addIntersections(e0.get(), 0, e1.get(), 0);

    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_TRUE(si.hasProperInteriorIntersection());
    EXPECT_EQ(si.getProperIntersectionPoint(), Coordinate(2.0, 2.0));
    EXPECT_TRUE(e0->getEdgeIntersectionList().isEmpty());
    EXPECT_TRUE(e1->getEdgeIntersectionList().isEmpty());
  }

  {
    auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 4.0)});
    auto e1 = MakeEdge({Coordinate(0.0, 4.0), Coordinate(4.0, 0.0)});
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/false);

    si.addIntersections(e0.get(), 0, e1.get(), 0);

    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_TRUE(si.hasProperInteriorIntersection());
    EXPECT_EQ(si.getProperIntersectionPoint(), Coordinate(2.0, 2.0));
    EXPECT_THAT(CollectIntersectionCoords(*e0),
                ElementsAre(Coordinate(2.0, 2.0)));
    EXPECT_THAT(CollectIntersectionCoords(*e1),
                ElementsAre(Coordinate(2.0, 2.0)));
  }
}

TEST(SegmentIntersectorTest,
     NonProperIntersectionsRecordedEvenWhenIncludeProperIsFalse) {
  // T-junction at endpoint of e1.
  {
    auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 0.0)});
    auto e1 = MakeEdge({Coordinate(2.0, 0.0), Coordinate(2.0, 3.0)});
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/false,
                          /*newRecordIsolated=*/true);

    si.addIntersections(e0.get(), 0, e1.get(), 0);

    EXPECT_TRUE(si.hasIntersection());
    EXPECT_FALSE(si.hasProperIntersection());
    EXPECT_FALSE(si.hasProperInteriorIntersection());
    EXPECT_THAT(CollectIntersectionCoords(*e0),
                ElementsAre(Coordinate(2.0, 0.0)));
    EXPECT_THAT(CollectIntersectionCoords(*e1),
                ElementsAre(Coordinate(2.0, 0.0)));
  }

  // Collinear overlap of two distinct edges.
  {
    auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 0.0)});
    auto e1 = MakeEdge({Coordinate(1.0, 0.0), Coordinate(3.0, 0.0)});
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/false,
                          /*newRecordIsolated=*/true);

    si.addIntersections(e0.get(), 0, e1.get(), 0);

    EXPECT_TRUE(si.hasIntersection());
    EXPECT_FALSE(si.hasProperIntersection());
    EXPECT_FALSE(si.hasProperInteriorIntersection());
    EXPECT_THAT(CollectIntersectionCoords(*e0),
                ElementsAre(Coordinate(1.0, 0.0), Coordinate(3.0, 0.0)));
    EXPECT_THAT(CollectIntersectionCoords(*e1),
                ElementsAre(Coordinate(1.0, 0.0), Coordinate(3.0, 0.0)));
  }
}

TEST(SegmentIntersectorTest,
     BoundaryNodesClassifyProperBoundaryVsInteriorIntersections) {
  Node non_matching_node(Coordinate(99.0, 99.0), nullptr);
  Node matching_node(Coordinate(2.0, 2.0), nullptr);

  // Non-matching node in bdyNodes[0], null bdyNodes[1]: treated as proper
  // interior intersection and omitted when includeProper is false.
  {
    auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 4.0)});
    auto e1 = MakeEdge({Coordinate(0.0, 4.0), Coordinate(4.0, 0.0)});
    std::vector<Node*> bdy0 = {&non_matching_node};
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/false,
                          /*newRecordIsolated=*/false);
    si.setBoundaryNodes(&bdy0, nullptr);

    si.addIntersections(e0.get(), 0, e1.get(), 0);

    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_TRUE(si.hasProperInteriorIntersection());
    EXPECT_TRUE(e0->getEdgeIntersectionList().isEmpty());
    EXPECT_TRUE(e1->getEdgeIntersectionList().isEmpty());
  }

  // Matching node in bdyNodes[0]: recorded even when includeProper is false,
  // and hasProperInteriorIntersection remains false.
  {
    auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 4.0)});
    auto e1 = MakeEdge({Coordinate(0.0, 4.0), Coordinate(4.0, 0.0)});
    std::vector<Node*> bdy0 = {&non_matching_node, &matching_node};
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/false,
                          /*newRecordIsolated=*/false);
    si.setBoundaryNodes(&bdy0, nullptr);

    si.addIntersections(e0.get(), 0, e1.get(), 0);

    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_FALSE(si.hasProperInteriorIntersection());
    EXPECT_EQ(si.getProperIntersectionPoint(), Coordinate(2.0, 2.0));
    EXPECT_THAT(CollectIntersectionCoords(*e0),
                ElementsAre(Coordinate(2.0, 2.0)));
    EXPECT_THAT(CollectIntersectionCoords(*e1),
                ElementsAre(Coordinate(2.0, 2.0)));
  }

  // Matching node in bdyNodes[1] after non-matching bdyNodes[0].
  {
    auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 4.0)});
    auto e1 = MakeEdge({Coordinate(0.0, 4.0), Coordinate(4.0, 0.0)});
    std::vector<Node*> bdy0 = {&non_matching_node};
    std::vector<Node*> bdy1 = {&matching_node};
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/false,
                          /*newRecordIsolated=*/false);
    si.setBoundaryNodes(&bdy0, &bdy1);

    si.addIntersections(e0.get(), 0, e1.get(), 0);

    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_FALSE(si.hasProperInteriorIntersection());
    EXPECT_THAT(CollectIntersectionCoords(*e0),
                ElementsAre(Coordinate(2.0, 2.0)));
    EXPECT_THAT(CollectIntersectionCoords(*e1),
                ElementsAre(Coordinate(2.0, 2.0)));
  }
}

TEST(SegmentIntersectorTest, FlagsAccumulateAcrossMultipleIntersectionTests) {
  auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 4.0)});
  auto e1 = MakeEdge({Coordinate(0.0, 4.0), Coordinate(4.0, 0.0)});
  auto disjoint = MakeEdge({Coordinate(20.0, 20.0), Coordinate(25.0, 25.0)});
  auto e2 = MakeEdge({Coordinate(8.0, 8.0), Coordinate(12.0, 12.0)});
  auto e3 = MakeEdge({Coordinate(8.0, 12.0), Coordinate(12.0, 8.0)});

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);

  si.addIntersections(e0.get(), 0, e1.get(), 0);
  EXPECT_EQ(si.numTests, 1);
  EXPECT_EQ(si.getProperIntersectionPoint(), Coordinate(2.0, 2.0));

  si.addIntersections(e0.get(), 0, disjoint.get(), 0);
  EXPECT_EQ(si.numTests, 2);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(si.hasProperInteriorIntersection());
  EXPECT_EQ(si.getProperIntersectionPoint(), Coordinate(2.0, 2.0));

  si.addIntersections(e2.get(), 0, e3.get(), 0);
  EXPECT_EQ(si.numTests, 3);
  EXPECT_EQ(si.getProperIntersectionPoint(), Coordinate(10.0, 10.0));
}

}  // namespace
}  // namespace geos_google
