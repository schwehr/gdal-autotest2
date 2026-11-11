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

#include "third_party/geos/include/geos/geomgraph/index/MonotoneChainEdge.h"

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
#include "third_party/geos/include/geos/geomgraph/index/SegmentIntersector.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geomgraph::Edge;
using geos::geomgraph::index::MonotoneChainEdge;
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

TEST(MonotoneChainEdgeTest, CoordinatesAndStartIndexesReflectUnderlyingEdge) {
  auto edge = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(2.0, 1.0),
      Coordinate(4.0, 3.0),
      Coordinate(2.0, 5.0),
      Coordinate(0.0, 6.0),
  });

  MonotoneChainEdge mce(edge.get());

  EXPECT_EQ(mce.getCoordinates(), edge->getCoordinates());
  EXPECT_THAT(mce.getStartIndexes(), ElementsAre(0, 2, 4));

  MonotoneChainEdge* edge_mce = edge->getMonotoneChainEdge();
  ASSERT_NE(edge_mce, nullptr);
  EXPECT_EQ(edge_mce->getCoordinates(), edge->getCoordinates());
  EXPECT_THAT(edge_mce->getStartIndexes(), ElementsAre(0, 2, 4));
}

TEST(MonotoneChainEdgeTest, GetMinXAndMaxXCoverAscendingDescendingAndVertical) {
  // Chain 0 (0..2): NE ascending x (1 -> 7)
  // Chain 1 (2..4): SW descending x (7 -> -2)
  // Chain 2 (4..6): SE vertical x (-2 -> -2)
  auto edge = MakeEdge({
      Coordinate(1.0, 0.0),
      Coordinate(3.0, 1.0),
      Coordinate(7.0, 4.0),
      Coordinate(4.0, 2.0),
      Coordinate(-2.0, 0.0),
      Coordinate(-2.0, -3.0),
      Coordinate(-2.0, -5.0),
  });
  MonotoneChainEdge mce(edge.get());
  ASSERT_THAT(mce.getStartIndexes(), ElementsAre(0, 2, 4, 6));

  EXPECT_DOUBLE_EQ(mce.getMinX(0), 1.0);
  EXPECT_DOUBLE_EQ(mce.getMaxX(0), 7.0);

  EXPECT_DOUBLE_EQ(mce.getMinX(1), -2.0);
  EXPECT_DOUBLE_EQ(mce.getMaxX(1), 7.0);

  EXPECT_DOUBLE_EQ(mce.getMinX(2), -2.0);
  EXPECT_DOUBLE_EQ(mce.getMaxX(2), -2.0);
}

TEST(MonotoneChainEdgeTest,
     ComputeIntersectsSingleSegmentIntersectingAndDisjoint) {
  auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 4.0)});
  auto e1 = MakeEdge({Coordinate(0.0, 4.0), Coordinate(4.0, 0.0)});
  auto e2 = MakeEdge({Coordinate(10.0, 10.0), Coordinate(12.0, 12.0)});

  MonotoneChainEdge mce0(e0.get());
  MonotoneChainEdge mce1(e1.get());
  MonotoneChainEdge mce2(e2.get());

  LineIntersector li_hit;
  SegmentIntersector si_hit(&li_hit, /*newIncludeProper=*/true,
                            /*newRecordIsolated=*/true);
  mce0.computeIntersects(mce1, si_hit);

  EXPECT_EQ(si_hit.numTests, 1);
  EXPECT_TRUE(si_hit.hasIntersection());
  EXPECT_TRUE(si_hit.hasProperIntersection());
  EXPECT_EQ(si_hit.getProperIntersectionPoint(), Coordinate(2.0, 2.0));
  EXPECT_THAT(CollectIntersectionCoords(*e0),
              ElementsAre(Coordinate(2.0, 2.0)));
  EXPECT_THAT(CollectIntersectionCoords(*e1),
              ElementsAre(Coordinate(2.0, 2.0)));

  LineIntersector li_miss;
  SegmentIntersector si_miss(&li_miss, /*newIncludeProper=*/true,
                             /*newRecordIsolated=*/true);
  mce0.computeIntersects(mce2, si_miss);

  EXPECT_EQ(si_miss.numTests, 1);
  EXPECT_FALSE(si_miss.hasIntersection());
  EXPECT_TRUE(e2->getEdgeIntersectionList().isEmpty());
}

TEST(MonotoneChainEdgeTest, ComputeIntersectsPrunesDisjointMultiSegmentChains) {
  auto e0 = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(1.0, 1.0),
      Coordinate(2.0, 2.0),
      Coordinate(3.0, 3.0),
      Coordinate(4.0, 4.0),
  });
  auto e1 = MakeEdge({
      Coordinate(10.0, 10.0),
      Coordinate(11.0, 11.0),
      Coordinate(12.0, 12.0),
      Coordinate(13.0, 13.0),
      Coordinate(14.0, 14.0),
  });

  MonotoneChainEdge mce0(e0.get());
  MonotoneChainEdge mce1(e1.get());
  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/false);

  mce0.computeIntersects(mce1, si);

  EXPECT_EQ(si.numTests, 0);
  EXPECT_FALSE(si.hasIntersection());
  EXPECT_TRUE(e0->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(e1->getEdgeIntersectionList().isEmpty());
}

TEST(MonotoneChainEdgeTest,
     ComputeIntersectsPrunesSubchainsWithinOverlappingMultiSegmentChains) {
  // Two strictly separated 8-segment monotone curves whose full envelopes
  // overlap ([0,8]x[0,12.8] vs [0,8]x[1,13.8]).
  auto e0 = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(1.0, 0.1),
      Coordinate(2.0, 0.2),
      Coordinate(3.0, 0.4),
      Coordinate(4.0, 0.8),
      Coordinate(5.0, 1.6),
      Coordinate(6.0, 3.2),
      Coordinate(7.0, 6.4),
      Coordinate(8.0, 12.8),
  });
  auto e1 = MakeEdge({
      Coordinate(0.0, 1.0),
      Coordinate(1.0, 1.1),
      Coordinate(2.0, 1.2),
      Coordinate(3.0, 1.4),
      Coordinate(4.0, 1.8),
      Coordinate(5.0, 2.6),
      Coordinate(6.0, 4.2),
      Coordinate(7.0, 7.4),
      Coordinate(8.0, 13.8),
  });

  MonotoneChainEdge mce0(e0.get());
  MonotoneChainEdge mce1(e1.get());
  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/false);

  mce0.computeIntersects(mce1, si);

  EXPECT_FALSE(si.hasIntersection());
  EXPECT_GT(si.numTests, 0);
  EXPECT_LT(si.numTests, 64);
}

TEST(MonotoneChainEdgeTest,
     ComputeIntersectsFindsProperIntersectionBetweenMultiSegmentChains) {
  auto e0 = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(2.0, 2.0),
      Coordinate(4.0, 4.0),
      Coordinate(6.0, 6.0),
      Coordinate(8.0, 8.0),
      Coordinate(10.0, 10.0),
  });
  auto e1 = MakeEdge({
      Coordinate(0.0, 10.0),
      Coordinate(2.0, 8.0),
      Coordinate(4.0, 6.0),
      Coordinate(6.0, 4.0),
      Coordinate(8.0, 2.0),
      Coordinate(10.0, 0.0),
  });

  MonotoneChainEdge mce0(e0.get());
  MonotoneChainEdge mce1(e1.get());
  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);

  mce0.computeIntersects(mce1, si);

  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_EQ(si.getProperIntersectionPoint(), Coordinate(5.0, 5.0));
  EXPECT_LT(si.numTests, 25);
  EXPECT_THAT(CollectIntersectionCoords(*e0),
              ElementsAre(Coordinate(5.0, 5.0)));
  EXPECT_THAT(CollectIntersectionCoords(*e1),
              ElementsAre(Coordinate(5.0, 5.0)));
}

TEST(MonotoneChainEdgeTest, ComputeIntersectsHandlesAsymmetricChainLengths) {
  // Single-segment chain vs 5-segment chain and vice versa exercise cases where
  // start0 == mid0 or start1 == mid1 during binary subdivision.
  {
    auto single = MakeEdge({Coordinate(0.0, 5.0), Coordinate(10.0, 5.0)});
    auto multi = MakeEdge({
        Coordinate(0.0, 0.0),
        Coordinate(2.0, 2.0),
        Coordinate(4.0, 4.0),
        Coordinate(6.0, 6.0),
        Coordinate(8.0, 8.0),
        Coordinate(10.0, 10.0),
    });

    MonotoneChainEdge mce_single(single.get());
    MonotoneChainEdge mce_multi(multi.get());
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/false);

    mce_single.computeIntersects(mce_multi, si);

    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_EQ(si.getProperIntersectionPoint(), Coordinate(5.0, 5.0));
    EXPECT_THAT(CollectIntersectionCoords(*single),
                ElementsAre(Coordinate(5.0, 5.0)));
    EXPECT_THAT(CollectIntersectionCoords(*multi),
                ElementsAre(Coordinate(5.0, 5.0)));
  }

  {
    auto multi = MakeEdge({
        Coordinate(0.0, 0.0),
        Coordinate(2.0, 2.0),
        Coordinate(4.0, 4.0),
        Coordinate(6.0, 6.0),
        Coordinate(8.0, 8.0),
        Coordinate(10.0, 10.0),
    });
    auto single = MakeEdge({Coordinate(0.0, 5.0), Coordinate(10.0, 5.0)});

    MonotoneChainEdge mce_multi(multi.get());
    MonotoneChainEdge mce_single(single.get());
    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/false);

    mce_multi.computeIntersects(mce_single, si);

    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_EQ(si.getProperIntersectionPoint(), Coordinate(5.0, 5.0));
    EXPECT_THAT(CollectIntersectionCoords(*multi),
                ElementsAre(Coordinate(5.0, 5.0)));
    EXPECT_THAT(CollectIntersectionCoords(*single),
                ElementsAre(Coordinate(5.0, 5.0)));
  }
}

TEST(MonotoneChainEdgeTest,
     ComputeIntersectsFindsMultipleCrossingsAcrossChainsAndSelfIntersections) {
  auto sawtooth = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(2.0, 4.0),
      Coordinate(4.0, 0.0),
      Coordinate(6.0, 4.0),
      Coordinate(8.0, 0.0),
  });
  auto baseline = MakeEdge({
      Coordinate(0.0, 2.0),
      Coordinate(4.0, 2.0),
      Coordinate(8.0, 2.0),
  });

  MonotoneChainEdge mce_saw(sawtooth.get());
  MonotoneChainEdge mce_line(baseline.get());
  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);

  mce_saw.computeIntersects(mce_line, si);

  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_THAT(CollectIntersectionCoords(*sawtooth),
              ElementsAre(Coordinate(1.0, 2.0), Coordinate(3.0, 2.0),
                          Coordinate(5.0, 2.0), Coordinate(7.0, 2.0)));
  EXPECT_THAT(CollectIntersectionCoords(*baseline),
              ElementsAre(Coordinate(1.0, 2.0), Coordinate(3.0, 2.0),
                          Coordinate(5.0, 2.0), Coordinate(7.0, 2.0)));

  // Self-intersection on a bowtie edge across non-adjacent chains.
  auto bowtie = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(4.0, 4.0),
      Coordinate(4.0, 0.0),
      Coordinate(0.0, 4.0),
  });
  MonotoneChainEdge mce_bowtie(bowtie.get());
  LineIntersector li_self;
  SegmentIntersector si_self(&li_self, /*newIncludeProper=*/true,
                             /*newRecordIsolated=*/false);

  mce_bowtie.computeIntersects(mce_bowtie, si_self);

  EXPECT_TRUE(si_self.hasIntersection());
  EXPECT_TRUE(si_self.hasProperIntersection());
  EXPECT_EQ(si_self.getProperIntersectionPoint(), Coordinate(2.0, 2.0));
  EXPECT_THAT(CollectIntersectionCoords(*bowtie),
              ElementsAre(Coordinate(2.0, 2.0), Coordinate(2.0, 2.0)));
}

TEST(MonotoneChainEdgeTest, ComputeIntersectsForChainTargetsSpecificChainPair) {
  auto e0 = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(1.0, 1.0),
      Coordinate(4.0, 4.0),
      Coordinate(6.0, 2.0),
      Coordinate(8.0, 0.0),
  });
  auto e1 = MakeEdge({
      Coordinate(0.0, 4.0),
      Coordinate(3.0, 1.0),
      Coordinate(5.0, -1.0),
      Coordinate(6.0, 0.0),
      Coordinate(7.0, 1.0),
  });

  MonotoneChainEdge mce0(e0.get());
  MonotoneChainEdge mce1(e1.get());
  ASSERT_THAT(mce0.getStartIndexes(), ElementsAre(0, 2, 4));
  ASSERT_THAT(mce1.getStartIndexes(), ElementsAre(0, 2, 4));

  // Chain 0 of e0 ([0,4]x[0,4]) vs chain 1 of e1 ([5,7]x[-1,1]): disjoint.
  LineIntersector li_disjoint;
  SegmentIntersector si_disjoint(&li_disjoint, /*newIncludeProper=*/true,
                                 /*newRecordIsolated=*/false);
  mce0.computeIntersectsForChain(0, mce1, 1, si_disjoint);
  EXPECT_EQ(si_disjoint.numTests, 0);
  EXPECT_FALSE(si_disjoint.hasIntersection());

  // Chain 0 of e0 vs chain 0 of e1: proper intersection at (2, 2).
  LineIntersector li_hit;
  SegmentIntersector si_hit(&li_hit, /*newIncludeProper=*/true,
                            /*newRecordIsolated=*/false);
  mce0.computeIntersectsForChain(0, mce1, 0, si_hit);
  EXPECT_TRUE(si_hit.hasIntersection());
  EXPECT_TRUE(si_hit.hasProperIntersection());
  EXPECT_EQ(si_hit.getProperIntersectionPoint(), Coordinate(2.0, 2.0));
  EXPECT_THAT(CollectIntersectionCoords(*e0),
              ElementsAre(Coordinate(2.0, 2.0)));
  EXPECT_THAT(CollectIntersectionCoords(*e1),
              ElementsAre(Coordinate(2.0, 2.0)));
}

}  // namespace
}  // namespace geos_google
