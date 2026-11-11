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

#include "third_party/geos/include/geos/noding/NodingIntersectionFinder.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/MCIndexSegmentSetMutualIntersector.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::noding::BasicSegmentString;
using geos::noding::MCIndexSegmentSetMutualIntersector;
using geos::noding::NodingIntersectionFinder;
using geos::noding::SegmentString;

struct OwnedString {
  std::unique_ptr<CoordinateSequence> seq;
  std::unique_ptr<BasicSegmentString> ss;

  explicit OwnedString(std::initializer_list<CoordinateXY> coords)
      : seq(std::make_unique<CoordinateSequence>(coords)),
        ss(std::make_unique<BasicSegmentString>(seq.get(), nullptr)) {}

  SegmentString* get() { return ss.get(); }
};

TEST(NodingIntersectionFinderTest, InitialState) {
  LineIntersector li;
  NodingIntersectionFinder finder(li);

  EXPECT_FALSE(finder.hasIntersection());
  EXPECT_FALSE(finder.isDone());
  EXPECT_TRUE(finder.getInteriorIntersection().isNull());
  EXPECT_EQ(finder.count(), 0u);
  EXPECT_TRUE(finder.getIntersectionSegments().empty());
}

TEST(NodingIntersectionFinderTest, SameSegmentShortCircuits) {
  OwnedString ss({{0.0, 0.0}, {10.0, 0.0}});
  LineIntersector li;
  NodingIntersectionFinder finder(li);

  finder.processIntersections(ss.get(), 0, ss.get(), 0);

  EXPECT_FALSE(finder.hasIntersection());
  EXPECT_FALSE(finder.isDone());
  EXPECT_EQ(finder.count(), 0u);
  EXPECT_TRUE(finder.getIntersectionSegments().empty());
}

TEST(NodingIntersectionFinderTest,
     ProperInteriorIntersectionAndDefaultSingleStop) {
  OwnedString a0({{0.0, 0.0}, {10.0, 10.0}});
  OwnedString a1({{0.0, 10.0}, {10.0, 0.0}});
  OwnedString b0({{20.0, 0.0}, {30.0, 10.0}});
  OwnedString b1({{20.0, 10.0}, {30.0, 0.0}});

  LineIntersector li;
  NodingIntersectionFinder finder(li);

  finder.processIntersections(a0.get(), 0, a1.get(), 0);
  EXPECT_TRUE(finder.hasIntersection());
  EXPECT_TRUE(finder.isDone());
  EXPECT_EQ(finder.count(), 1u);
  EXPECT_TRUE(finder.getInteriorIntersection().equals2D({5.0, 5.0}));

  const std::vector<Coordinate>& segs = finder.getIntersectionSegments();
  ASSERT_EQ(segs.size(), 4u);
  EXPECT_TRUE(segs[0].equals2D({0.0, 0.0}));
  EXPECT_TRUE(segs[1].equals2D({10.0, 10.0}));
  EXPECT_TRUE(segs[2].equals2D({0.0, 10.0}));
  EXPECT_TRUE(segs[3].equals2D({10.0, 0.0}));

  // With default findAllIntersections=false, further pairs are ignored.
  finder.processIntersections(b0.get(), 0, b1.get(), 0);
  EXPECT_EQ(finder.count(), 1u);
  EXPECT_EQ(finder.getIntersectionSegments().size(), 4u);
  EXPECT_TRUE(finder.getInteriorIntersection().equals2D({5.0, 5.0}));
}

TEST(NodingIntersectionFinderTest, FindAllIntersectionsRecordsAll) {
  OwnedString a0({{0.0, 0.0}, {10.0, 10.0}});
  OwnedString a1({{0.0, 10.0}, {10.0, 0.0}});
  OwnedString b0({{20.0, 0.0}, {30.0, 10.0}});
  OwnedString b1({{20.0, 10.0}, {30.0, 0.0}});

  LineIntersector li;
  NodingIntersectionFinder finder(li);
  finder.setFindAllIntersections(true);

  finder.processIntersections(a0.get(), 0, a1.get(), 0);
  finder.processIntersections(b0.get(), 0, b1.get(), 0);

  EXPECT_TRUE(finder.hasIntersection());
  EXPECT_TRUE(finder.isDone());
  EXPECT_EQ(finder.count(), 2u);
  EXPECT_TRUE(finder.getInteriorIntersection().equals2D({25.0, 5.0}));

  const std::vector<Coordinate>& segs = finder.getIntersectionSegments();
  ASSERT_EQ(segs.size(), 8u);
  EXPECT_TRUE(segs[4].equals2D({20.0, 0.0}));
  EXPECT_TRUE(segs[5].equals2D({30.0, 10.0}));
  EXPECT_TRUE(segs[6].equals2D({20.0, 10.0}));
  EXPECT_TRUE(segs[7].equals2D({30.0, 0.0}));
}

TEST(NodingIntersectionFinderTest,
     TJunctionAndCollinearOverlapDetectedWhileDisjointIgnored) {
  // Disjoint segments.
  {
    OwnedString e0({{0.0, 0.0}, {2.0, 0.0}});
    OwnedString e1({{5.0, 5.0}, {8.0, 5.0}});
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.processIntersections(e0.get(), 0, e1.get(), 0);
    EXPECT_FALSE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 0u);
  }

  // T-junction: endpoint (5, 0) lies in interior of (0, 0)->(10, 0).
  {
    OwnedString e0({{0.0, 0.0}, {10.0, 0.0}});
    OwnedString e1({{5.0, 0.0}, {5.0, 5.0}});
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.processIntersections(e0.get(), 0, e1.get(), 0);
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 1u);
    EXPECT_TRUE(finder.getInteriorIntersection().equals2D({5.0, 0.0}));
  }

  // Collinear partial overlap.
  {
    OwnedString e0({{0.0, 0.0}, {6.0, 0.0}});
    OwnedString e1({{4.0, 0.0}, {10.0, 0.0}});
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.processIntersections(e0.get(), 0, e1.get(), 0);
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 1u);
  }
}

TEST(NodingIntersectionFinderTest, EndpointOnlyMeetingsAreNotReported) {
  OwnedString a({{0.0, 0.0}, {5.0, 0.0}, {10.0, 0.0}});
  OwnedString b_start_at_a_start({{0.0, 0.0}, {0.0, 5.0}, {0.0, 10.0}});
  OwnedString b_end_at_a_start({{0.0, 10.0}, {0.0, 5.0}, {0.0, 0.0}});
  OwnedString b_start_at_a_end({{10.0, 0.0}, {10.0, 5.0}, {10.0, 10.0}});
  OwnedString b_end_at_a_end({{10.0, 10.0}, {10.0, 5.0}, {10.0, 0.0}});

  LineIntersector li;
  NodingIntersectionFinder finder(li);
  finder.setFindAllIntersections(true);

  // p00 == p10 (both endpoints)
  finder.processIntersections(a.get(), 0, b_start_at_a_start.get(), 0);
  // p00 == p11 (both endpoints)
  finder.processIntersections(a.get(), 0, b_end_at_a_start.get(), 1);
  // p01 == p10 (both endpoints)
  finder.processIntersections(a.get(), 1, b_start_at_a_end.get(), 0);
  // p01 == p11 (both endpoints)
  finder.processIntersections(a.get(), 1, b_end_at_a_end.get(), 1);

  EXPECT_FALSE(finder.hasIntersection());
  EXPECT_FALSE(finder.isDone());
  EXPECT_EQ(finder.count(), 0u);
}

TEST(NodingIntersectionFinderTest,
     InteriorVertexIntersectionsDetectedAcrossAllVertexPairs) {
  // (0, 0) is an interior vertex of e0.
  OwnedString e0({{-5.0, 5.0}, {0.0, 0.0}, {5.0, 5.0}});
  OwnedString from_origin({{0.0, 0.0}, {0.0, -5.0}});
  OwnedString to_origin({{0.0, -5.0}, {0.0, 0.0}});

  // 1. p00 == p10 (e0 seg 1 start, from_origin seg 0 start)
  {
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.processIntersections(e0.get(), 1, from_origin.get(), 0);
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 1u);
    EXPECT_TRUE(finder.getInteriorIntersection().equals2D({0.0, 0.0}));
  }

  // 2. p00 == p11 (e0 seg 1 start, to_origin seg 0 end)
  {
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.processIntersections(e0.get(), 1, to_origin.get(), 0);
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 1u);
    EXPECT_TRUE(finder.getInteriorIntersection().equals2D({0.0, 0.0}));
  }

  // 3. p01 == p10 (e0 seg 0 end, from_origin seg 0 start)
  {
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.processIntersections(e0.get(), 0, from_origin.get(), 0);
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 1u);
    EXPECT_TRUE(finder.getInteriorIntersection().equals2D({0.0, 0.0}));
  }

  // 4. p01 == p11 (e0 seg 0 end, to_origin seg 0 end)
  {
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.processIntersections(e0.get(), 0, to_origin.get(), 0);
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 1u);
    EXPECT_TRUE(finder.getInteriorIntersection().equals2D({0.0, 0.0}));
  }
}

TEST(NodingIntersectionFinderTest,
     SelfIntersectionAdjacentIgnoredNonAdjacentDetectedAndClosedRingValid) {
  // Self-touching polyline at interior vertex (5, 5):
  // seg 0: (0,0)-(5,5), seg 1: (5,5)-(10,0),
  // seg 2: (10,0)-(5,5), seg 3: (5,5)-(0,10).
  OwnedString self_touch(
      {{0.0, 0.0}, {5.0, 5.0}, {10.0, 0.0}, {5.0, 5.0}, {0.0, 10.0}});

  // Adjacent segments sharing interior vertex (5, 5) are valid.
  {
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.processIntersections(self_touch.get(), 0, self_touch.get(), 1);
    EXPECT_FALSE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 0u);
  }

  // Non-adjacent segments sharing interior vertex (5, 5) are flagged.
  {
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.processIntersections(self_touch.get(), 0, self_touch.get(), 3);
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 1u);
    EXPECT_TRUE(finder.getInteriorIntersection().equals2D({5.0, 5.0}));
  }

  // Reverse segment index order exercises std::max/std::min symmetry.
  {
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.processIntersections(self_touch.get(), 3, self_touch.get(), 0);
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 1u);
    EXPECT_TRUE(finder.getInteriorIntersection().equals2D({5.0, 5.0}));
  }

  // Closed ring meeting only at its start/end vertex is valid noding.
  OwnedString closed_ring({{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 0.0}});
  {
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.processIntersections(closed_ring.get(), 0, closed_ring.get(), 2);
    EXPECT_FALSE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 0u);
  }
}

TEST(NodingIntersectionFinderTest, CheckEndSegmentsOnlyFiltering) {
  // 5-vertex (4-segment) strings: end segments are 0 and 3; middle are 1 and 2.
  OwnedString e0(
      {{0.0, 0.0}, {2.0, 0.0}, {8.0, 10.0}, {10.0, 10.0}, {12.0, 10.0}});
  OwnedString e1(
      {{0.0, 10.0}, {2.0, 10.0}, {8.0, 0.0}, {10.0, 0.0}, {12.0, 0.0}});

  // When checking end segments only, middle-vs-middle crossing (seg 1 vs seg 1)
  // is skipped.
  {
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.setCheckEndSegmentsOnly(true);
    finder.processIntersections(e0.get(), 1, e1.get(), 1);
    EXPECT_FALSE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 0u);
  }

  // When checkEndSegmentsOnly is disabled, the middle-vs-middle crossing is
  // found.
  {
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.setCheckEndSegmentsOnly(false);
    finder.processIntersections(e0.get(), 1, e1.get(), 1);
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_EQ(finder.count(), 1u);
    EXPECT_TRUE(finder.getInteriorIntersection().equals2D({5.0, 5.0}));
  }

  // If either e0 (seg 0 or seg 3) or e1 (seg 0 or seg 3) is an end segment,
  // checkEndSegmentsOnly=true still tests and reports the intersection.
  OwnedString cross_mid(
      {{0.0, 5.0}, {3.0, 5.0}, {7.0, 5.0}, {10.0, 5.0}, {12.0, 5.0}});
  OwnedString cross_vertical({{5.0, 0.0}, {5.0, 10.0}});  // 1 segment (end=0)
  OwnedString cross_last_end(
      {{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}, {5.0, 0.0}, {5.0, 10.0}});  // seg 3

  // e0 is end segment (seg 0), e1 is middle segment (seg 1).
  {
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.setCheckEndSegmentsOnly(true);
    finder.processIntersections(cross_vertical.get(), 0, cross_mid.get(), 1);
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_TRUE(finder.getInteriorIntersection().equals2D({5.0, 5.0}));
  }

  // e0 is middle segment (seg 1), e1 is last end segment (seg 3).
  {
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    finder.setCheckEndSegmentsOnly(true);
    finder.processIntersections(cross_mid.get(), 1, cross_last_end.get(), 3);
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_TRUE(finder.getInteriorIntersection().equals2D({5.0, 5.0}));
  }
}

TEST(NodingIntersectionFinderTest,
     WorksWithMCIndexSegmentSetMutualIntersector) {
  OwnedString base_disjoint({{0.0, 0.0}, {2.0, 0.0}});
  OwnedString base_cross({{0.0, 10.0}, {10.0, 10.0}});
  OwnedString query({{5.0, 5.0}, {5.0, 15.0}});

  SegmentString::ConstVect base_segs{base_disjoint.get(), base_cross.get()};
  SegmentString::ConstVect query_segs{query.get()};

  LineIntersector li;
  NodingIntersectionFinder finder(li);
  MCIndexSegmentSetMutualIntersector intersector;
  intersector.setSegmentIntersector(&finder);
  intersector.setBaseSegments(&base_segs);
  intersector.process(&query_segs);

  EXPECT_TRUE(finder.hasIntersection());
  EXPECT_TRUE(finder.isDone());
  EXPECT_EQ(finder.count(), 1u);
  EXPECT_TRUE(finder.getInteriorIntersection().equals2D({5.0, 10.0}));
}

}  // namespace
}  // namespace geos_google
