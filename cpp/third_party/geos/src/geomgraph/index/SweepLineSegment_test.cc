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

#include "third_party/geos/include/geos/geomgraph/index/SweepLineSegment.h"

#include <cstddef>
#include <initializer_list>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersectionList.h"
#include "third_party/geos/include/geos/geomgraph/index/SegmentIntersector.h"
#include "third_party/geos/include/geos/geomgraph/index/SweepLineEvent.h"
#include "third_party/geos/include/geos/geomgraph/index/SweepLineEventObj.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geomgraph::Edge;
using geos::geomgraph::index::SegmentIntersector;
using geos::geomgraph::index::SweepLineEvent;
using geos::geomgraph::index::SweepLineEventOBJ;
using geos::geomgraph::index::SweepLineSegment;

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(seq.release());
}

TEST(SweepLineSegmentTest, MinAndMaxXIncreasingSegment) {
  auto edge = MakeEdge({Coordinate(1.5, 2.0), Coordinate(7.25, -3.0)});
  SweepLineSegment seg(edge.get(), 0);

  EXPECT_DOUBLE_EQ(seg.getMinX(), 1.5);
  EXPECT_DOUBLE_EQ(seg.getMaxX(), 7.25);
}

TEST(SweepLineSegmentTest, MinAndMaxXDecreasingSegment) {
  auto edge = MakeEdge({Coordinate(8.5, 1.0), Coordinate(-3.25, 4.0)});
  SweepLineSegment seg(edge.get(), 0);

  EXPECT_DOUBLE_EQ(seg.getMinX(), -3.25);
  EXPECT_DOUBLE_EQ(seg.getMaxX(), 8.5);
}

TEST(SweepLineSegmentTest, MinAndMaxXVerticalZeroLengthAndNegative) {
  auto vertical = MakeEdge({Coordinate(4.0, -5.0), Coordinate(4.0, 12.0)});
  SweepLineSegment seg_v(vertical.get(), 0);
  EXPECT_DOUBLE_EQ(seg_v.getMinX(), 4.0);
  EXPECT_DOUBLE_EQ(seg_v.getMaxX(), 4.0);

  auto zero_len = MakeEdge({Coordinate(-6.5, 2.0), Coordinate(-6.5, 2.0)});
  SweepLineSegment seg_z(zero_len.get(), 0);
  EXPECT_DOUBLE_EQ(seg_z.getMinX(), -6.5);
  EXPECT_DOUBLE_EQ(seg_z.getMaxX(), -6.5);

  auto negative = MakeEdge({Coordinate(-10.0, 0.0), Coordinate(-2.5, 1.0)});
  SweepLineSegment seg_n(negative.get(), 0);
  EXPECT_DOUBLE_EQ(seg_n.getMinX(), -10.0);
  EXPECT_DOUBLE_EQ(seg_n.getMaxX(), -2.5);
}

TEST(SweepLineSegmentTest, SelectsCorrectSegmentByPtIndexInPolyline) {
  auto edge = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(10.0, 5.0),
      Coordinate(3.0, 8.0),
      Coordinate(-4.0, 2.0),
  });

  SweepLineSegment seg0(edge.get(), 0);
  SweepLineSegment seg1(edge.get(), 1);
  SweepLineSegment seg2(edge.get(), 2);

  EXPECT_DOUBLE_EQ(seg0.getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(seg0.getMaxX(), 10.0);

  EXPECT_DOUBLE_EQ(seg1.getMinX(), 3.0);
  EXPECT_DOUBLE_EQ(seg1.getMaxX(), 10.0);

  EXPECT_DOUBLE_EQ(seg2.getMinX(), -4.0);
  EXPECT_DOUBLE_EQ(seg2.getMaxX(), 3.0);
}

TEST(SweepLineSegmentTest,
     ComputeIntersectionsProperCrossingIncludeProperTrue) {
  auto edge0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(10.0, 10.0)});
  auto edge1 = MakeEdge({Coordinate(0.0, 10.0), Coordinate(10.0, 0.0)});

  SweepLineSegment seg0(edge0.get(), 0);
  SweepLineSegment seg1(edge1.get(), 0);

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);

  seg0.computeIntersections(&seg1, &si);

  EXPECT_EQ(si.numTests, 1);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(si.hasProperInteriorIntersection());
  EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(5.0, 5.0)));

  EXPECT_FALSE(edge0->getEdgeIntersectionList().isEmpty());
  EXPECT_FALSE(edge1->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(
      edge0->getEdgeIntersectionList().isIntersection(Coordinate(5.0, 5.0)));
  EXPECT_TRUE(
      edge1->getEdgeIntersectionList().isIntersection(Coordinate(5.0, 5.0)));
  EXPECT_FALSE(edge0->isIsolated());
  EXPECT_FALSE(edge1->isIsolated());
}

TEST(SweepLineSegmentTest,
     ComputeIntersectionsProperCrossingIncludeProperFalse) {
  auto edge0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(10.0, 10.0)});
  auto edge1 = MakeEdge({Coordinate(0.0, 10.0), Coordinate(10.0, 0.0)});

  SweepLineSegment seg0(edge0.get(), 0);
  SweepLineSegment seg1(edge1.get(), 0);

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/false,
                        /*newRecordIsolated=*/false);

  seg0.computeIntersections(&seg1, &si);

  EXPECT_EQ(si.numTests, 1);
  EXPECT_TRUE(si.hasIntersection());
  EXPECT_TRUE(si.hasProperIntersection());
  EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(5.0, 5.0)));
  EXPECT_TRUE(edge0->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(edge1->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(edge0->isIsolated());
  EXPECT_TRUE(edge1->isIsolated());
}

TEST(SweepLineSegmentTest, ComputeIntersectionsEndpointAndCollinear) {
  // T-junction endpoint intersection is recorded even with includeProper=false.
  auto edge0 = MakeEdge({Coordinate(0.0, 5.0), Coordinate(10.0, 5.0)});
  auto edge1 = MakeEdge({Coordinate(5.0, 0.0), Coordinate(5.0, 5.0)});

  SweepLineSegment seg0(edge0.get(), 0);
  SweepLineSegment seg1(edge1.get(), 0);

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/false,
                        /*newRecordIsolated=*/true);

  seg0.computeIntersections(&seg1, &si);

  EXPECT_TRUE(si.hasIntersection());
  EXPECT_FALSE(si.hasProperIntersection());
  EXPECT_TRUE(
      edge0->getEdgeIntersectionList().isIntersection(Coordinate(5.0, 5.0)));
  EXPECT_TRUE(
      edge1->getEdgeIntersectionList().isIntersection(Coordinate(5.0, 5.0)));

  // Collinear overlap across non-zero segment indices.
  auto edge2 = MakeEdge({
      Coordinate(-5.0, 1.0),
      Coordinate(0.0, 0.0),
      Coordinate(6.0, 0.0),
  });
  auto edge3 = MakeEdge({
      Coordinate(4.0, 0.0),
      Coordinate(10.0, 0.0),
  });

  SweepLineSegment seg2(edge2.get(), 1);
  SweepLineSegment seg3(edge3.get(), 0);
  SegmentIntersector si_collinear(&li, /*newIncludeProper=*/false,
                                  /*newRecordIsolated=*/false);

  seg2.computeIntersections(&seg3, &si_collinear);

  EXPECT_TRUE(si_collinear.hasIntersection());
  EXPECT_FALSE(si_collinear.hasProperIntersection());
  EXPECT_TRUE(
      edge2->getEdgeIntersectionList().isIntersection(Coordinate(4.0, 0.0)));
  EXPECT_TRUE(
      edge2->getEdgeIntersectionList().isIntersection(Coordinate(6.0, 0.0)));
  EXPECT_TRUE(
      edge3->getEdgeIntersectionList().isIntersection(Coordinate(4.0, 0.0)));
  EXPECT_TRUE(
      edge3->getEdgeIntersectionList().isIntersection(Coordinate(6.0, 0.0)));
}

TEST(SweepLineSegmentTest, ComputeIntersectionsDisjointAndSelfSegments) {
  auto edge0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(5.0, 0.0)});
  auto edge1 = MakeEdge({Coordinate(0.0, 2.0), Coordinate(5.0, 2.0)});

  SweepLineSegment seg0(edge0.get(), 0);
  SweepLineSegment seg1(edge1.get(), 0);

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/true);

  // Disjoint segments.
  seg0.computeIntersections(&seg1, &si);
  EXPECT_EQ(si.numTests, 1);
  EXPECT_FALSE(si.hasIntersection());
  EXPECT_TRUE(edge0->getEdgeIntersectionList().isEmpty());
  EXPECT_TRUE(edge1->getEdgeIntersectionList().isEmpty());

  // Identical segment on the same edge is skipped by SegmentIntersector.
  SegmentIntersector si_same(&li, true, true);
  seg0.computeIntersections(&seg0, &si_same);
  EXPECT_EQ(si_same.numTests, 0);
  EXPECT_FALSE(si_same.hasIntersection());

  // Bowtie polyline: adjacent segments have a trivial endpoint intersection,
  // while non-adjacent segments 0 and 2 cross at (5, 5).
  auto self_edge = MakeEdge({
      Coordinate(0.0, 0.0),
      Coordinate(10.0, 10.0),
      Coordinate(10.0, 0.0),
      Coordinate(0.0, 10.0),
  });
  SweepLineSegment s0(self_edge.get(), 0);
  SweepLineSegment s1(self_edge.get(), 1);
  SweepLineSegment s2(self_edge.get(), 2);

  SegmentIntersector si_adj(&li, true, true);
  s0.computeIntersections(&s1, &si_adj);
  EXPECT_EQ(si_adj.numTests, 1);
  EXPECT_FALSE(si_adj.hasIntersection());

  SegmentIntersector si_self(&li, true, true);
  s0.computeIntersections(&s2, &si_self);
  EXPECT_EQ(si_self.numTests, 1);
  EXPECT_TRUE(si_self.hasIntersection());
  EXPECT_TRUE(si_self.hasProperIntersection());
  EXPECT_TRUE(
      si_self.getProperIntersectionPoint().equals2D(Coordinate(5.0, 5.0)));
}

TEST(SweepLineSegmentTest, IntegratesWithSweepLineEventPolymorphically) {
  auto edge = MakeEdge({Coordinate(9.0, 1.0), Coordinate(2.0, 4.0)});
  std::unique_ptr<SweepLineEventOBJ> obj =
      std::make_unique<SweepLineSegment>(edge.get(), 0);
  auto* seg = dynamic_cast<SweepLineSegment*>(obj.get());
  ASSERT_NE(seg, nullptr);

  SweepLineEvent insert_ev(edge.get(), seg->getMinX(), nullptr, seg);
  SweepLineEvent delete_ev(edge.get(), seg->getMaxX(), &insert_ev, seg);

  EXPECT_EQ(insert_ev.getObject(), obj.get());
  EXPECT_EQ(delete_ev.getObject(), obj.get());
  EXPECT_LT(insert_ev.compareTo(&delete_ev), 0);
  EXPECT_DOUBLE_EQ(seg->getMinX(), 2.0);
  EXPECT_DOUBLE_EQ(seg->getMaxX(), 9.0);
}

}  // namespace
}  // namespace geos_google
