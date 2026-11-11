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

// Unit tests for third_party/geos/src/noding/SegmentIntersectionDetector.cpp
// and third_party/geos/include/geos/noding/SegmentIntersectionDetector.h.

#include "third_party/geos/include/geos/noding/SegmentIntersectionDetector.h"

#include <cstddef>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/MCIndexNoder.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/noding/SimpleNoder.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::PrecisionModel;
using geos::noding::BasicSegmentString;
using geos::noding::MCIndexNoder;
using geos::noding::SegmentIntersectionDetector;
using geos::noding::SegmentString;
using geos::noding::SimpleNoder;

void ExpectSegmentsEqual(const CoordinateSequence* segs,
                         const CoordinateXY& p00, const CoordinateXY& p01,
                         const CoordinateXY& p10, const CoordinateXY& p11) {
  ASSERT_NE(segs, nullptr);
  ASSERT_EQ(segs->size(), 4u);
  EXPECT_TRUE(segs->getAt<CoordinateXY>(0).equals2D(p00));
  EXPECT_TRUE(segs->getAt<CoordinateXY>(1).equals2D(p01));
  EXPECT_TRUE(segs->getAt<CoordinateXY>(2).equals2D(p10));
  EXPECT_TRUE(segs->getAt<CoordinateXY>(3).equals2D(p11));
}

TEST(SegmentIntersectionDetectorTest, InitialStateIsEmptyAndNotDone) {
  LineIntersector li;
  SegmentIntersectionDetector detector(&li);

  EXPECT_FALSE(detector.hasIntersection());
  EXPECT_FALSE(detector.hasProperIntersection());
  EXPECT_FALSE(detector.hasNonProperIntersection());
  EXPECT_EQ(detector.getIntersection(), nullptr);
  EXPECT_EQ(detector.getIntersectionSegments(), nullptr);
  EXPECT_FALSE(detector.isDone());

  detector.setFindProper(true);
  EXPECT_FALSE(detector.isDone());

  detector.setFindAllIntersectionTypes(true);
  EXPECT_FALSE(detector.isDone());
}

TEST(SegmentIntersectionDetectorTest, SkipsIdenticalSegmentAndSameIndex) {
  LineIntersector li;
  SegmentIntersectionDetector detector(&li);

  CoordinateSequence seq({CoordinateXY{0.0, 0.0}, CoordinateXY{2.0, 2.0}});
  BasicSegmentString ss(&seq, nullptr);

  detector.processIntersections(&ss, 0, &ss, 0);

  EXPECT_FALSE(detector.hasIntersection());
  EXPECT_FALSE(detector.hasProperIntersection());
  EXPECT_FALSE(detector.hasNonProperIntersection());
  EXPECT_EQ(detector.getIntersection(), nullptr);
  EXPECT_EQ(detector.getIntersectionSegments(), nullptr);
  EXPECT_FALSE(detector.isDone());
}

TEST(SegmentIntersectionDetectorTest,
     SameSegmentStringDifferentIndicesChecked) {
  LineIntersector li;
  SegmentIntersectionDetector detector(&li);
  detector.setFindAllIntersectionTypes(true);

  // Self-intersecting polyline: seg 0 crosses seg 2 properly at (1, 1);
  // seg 0 shares vertex (2, 2) with seg 1 non-properly.
  CoordinateSequence seq({CoordinateXY{0.0, 0.0}, CoordinateXY{2.0, 2.0},
                          CoordinateXY{2.0, 0.0}, CoordinateXY{0.0, 2.0}});
  BasicSegmentString ss(&seq, nullptr);

  detector.processIntersections(&ss, 0, &ss, 1);
  EXPECT_TRUE(detector.hasIntersection());
  EXPECT_TRUE(detector.hasNonProperIntersection());
  EXPECT_FALSE(detector.hasProperIntersection());
  EXPECT_FALSE(detector.isDone());
  ASSERT_NE(detector.getIntersection(), nullptr);
  EXPECT_TRUE(detector.getIntersection()->equals2D(CoordinateXY{2.0, 2.0}));

  detector.processIntersections(&ss, 0, &ss, 2);
  EXPECT_TRUE(detector.hasProperIntersection());
  EXPECT_TRUE(detector.isDone());
  ASSERT_NE(detector.getIntersection(), nullptr);
  EXPECT_TRUE(detector.getIntersection()->equals2D(CoordinateXY{1.0, 1.0}));
  ExpectSegmentsEqual(detector.getIntersectionSegments(), {0.0, 0.0},
                      {2.0, 2.0}, {2.0, 0.0}, {0.0, 2.0});
}

TEST(SegmentIntersectionDetectorTest, DisjointSegmentsDoNotRecordIntersection) {
  LineIntersector li;
  SegmentIntersectionDetector detector(&li);

  CoordinateSequence seq0({CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0}});
  CoordinateSequence seq1({CoordinateXY{2.0, 2.0}, CoordinateXY{3.0, 3.0}});
  BasicSegmentString ss0(&seq0, nullptr);
  BasicSegmentString ss1(&seq1, nullptr);

  detector.processIntersections(&ss0, 0, &ss1, 0);

  EXPECT_FALSE(detector.hasIntersection());
  EXPECT_FALSE(detector.hasProperIntersection());
  EXPECT_FALSE(detector.hasNonProperIntersection());
  EXPECT_EQ(detector.getIntersection(), nullptr);
  EXPECT_EQ(detector.getIntersectionSegments(), nullptr);
  EXPECT_FALSE(detector.isDone());
}

TEST(SegmentIntersectionDetectorTest, DetectsProperIntersectionAndIsDone) {
  LineIntersector li;
  SegmentIntersectionDetector detector(&li);

  CoordinateSequence seq0({CoordinateXY{0.0, 0.0}, CoordinateXY{4.0, 4.0}});
  CoordinateSequence seq1({CoordinateXY{0.0, 4.0}, CoordinateXY{4.0, 0.0}});
  BasicSegmentString ss0(&seq0, nullptr);
  BasicSegmentString ss1(&seq1, nullptr);

  detector.processIntersections(&ss0, 0, &ss1, 0);

  EXPECT_TRUE(detector.hasIntersection());
  EXPECT_TRUE(detector.hasProperIntersection());
  EXPECT_FALSE(detector.hasNonProperIntersection());
  EXPECT_TRUE(detector.isDone());
  ASSERT_NE(detector.getIntersection(), nullptr);
  EXPECT_TRUE(detector.getIntersection()->equals2D(CoordinateXY{2.0, 2.0}));
  ExpectSegmentsEqual(detector.getIntersectionSegments(), {0.0, 0.0},
                      {4.0, 4.0}, {0.0, 4.0}, {4.0, 0.0});
}

TEST(SegmentIntersectionDetectorTest,
     DetectsNonProperEndpointTJunctionCollinear) {
  // Shared endpoint.
  {
    LineIntersector li;
    SegmentIntersectionDetector detector(&li);
    CoordinateSequence seq0({CoordinateXY{0.0, 0.0}, CoordinateXY{2.0, 0.0}});
    CoordinateSequence seq1({CoordinateXY{2.0, 0.0}, CoordinateXY{2.0, 3.0}});
    BasicSegmentString ss0(&seq0, nullptr);
    BasicSegmentString ss1(&seq1, nullptr);

    detector.processIntersections(&ss0, 0, &ss1, 0);
    EXPECT_TRUE(detector.hasIntersection());
    EXPECT_FALSE(detector.hasProperIntersection());
    EXPECT_TRUE(detector.hasNonProperIntersection());
    EXPECT_TRUE(detector.isDone());
    ASSERT_NE(detector.getIntersection(), nullptr);
    EXPECT_TRUE(detector.getIntersection()->equals2D(CoordinateXY{2.0, 0.0}));
  }

  // T-junction: endpoint lies in interior of other segment.
  {
    LineIntersector li;
    SegmentIntersectionDetector detector(&li);
    CoordinateSequence seq0({CoordinateXY{0.0, 0.0}, CoordinateXY{4.0, 0.0}});
    CoordinateSequence seq1({CoordinateXY{2.0, 0.0}, CoordinateXY{2.0, 3.0}});
    BasicSegmentString ss0(&seq0, nullptr);
    BasicSegmentString ss1(&seq1, nullptr);

    detector.processIntersections(&ss0, 0, &ss1, 0);
    EXPECT_TRUE(detector.hasIntersection());
    EXPECT_FALSE(detector.hasProperIntersection());
    EXPECT_TRUE(detector.hasNonProperIntersection());
    EXPECT_TRUE(detector.isDone());
    ASSERT_NE(detector.getIntersection(), nullptr);
    EXPECT_TRUE(detector.getIntersection()->equals2D(CoordinateXY{2.0, 0.0}));
  }

  // Collinear overlap.
  {
    LineIntersector li;
    SegmentIntersectionDetector detector(&li);
    CoordinateSequence seq0({CoordinateXY{0.0, 0.0}, CoordinateXY{4.0, 0.0}});
    CoordinateSequence seq1({CoordinateXY{2.0, 0.0}, CoordinateXY{6.0, 0.0}});
    BasicSegmentString ss0(&seq0, nullptr);
    BasicSegmentString ss1(&seq1, nullptr);

    detector.processIntersections(&ss0, 0, &ss1, 0);
    EXPECT_TRUE(detector.hasIntersection());
    EXPECT_FALSE(detector.hasProperIntersection());
    EXPECT_TRUE(detector.hasNonProperIntersection());
    EXPECT_TRUE(detector.isDone());
    ASSERT_NE(detector.getIntersection(), nullptr);
    EXPECT_TRUE(detector.getIntersection()->equals2D(CoordinateXY{2.0, 0.0}));
    ExpectSegmentsEqual(detector.getIntersectionSegments(), {0.0, 0.0},
                        {4.0, 0.0}, {2.0, 0.0}, {6.0, 0.0});
  }
}

TEST(SegmentIntersectionDetectorTest,
     FindProperKeepsFirstNonProperUntilProper) {
  LineIntersector li;
  SegmentIntersectionDetector detector(&li);
  detector.setFindProper(true);

  // 1) Non-proper intersection when no intersection was recorded yet:
  // saved as fallback, but isDone() remains false.
  CoordinateSequence seq_np0a({CoordinateXY{0.0, 0.0}, CoordinateXY{4.0, 0.0}});
  CoordinateSequence seq_np0b({CoordinateXY{2.0, 0.0}, CoordinateXY{2.0, 3.0}});
  BasicSegmentString ss_np0a(&seq_np0a, nullptr);
  BasicSegmentString ss_np0b(&seq_np0b, nullptr);

  detector.processIntersections(&ss_np0a, 0, &ss_np0b, 0);
  EXPECT_TRUE(detector.hasIntersection());
  EXPECT_FALSE(detector.hasProperIntersection());
  EXPECT_TRUE(detector.hasNonProperIntersection());
  EXPECT_FALSE(detector.isDone());
  ASSERT_NE(detector.getIntersection(), nullptr);
  EXPECT_TRUE(detector.getIntersection()->equals2D(CoordinateXY{2.0, 0.0}));
  ExpectSegmentsEqual(detector.getIntersectionSegments(), {0.0, 0.0},
                      {4.0, 0.0}, {2.0, 0.0}, {2.0, 3.0});

  // 2) Second non-proper intersection does NOT overwrite recorded segments
  // because intPt is already non-null and saveLocation == false.
  CoordinateSequence seq_np1a(
      {CoordinateXY{10.0, 0.0}, CoordinateXY{12.0, 0.0}});
  CoordinateSequence seq_np1b(
      {CoordinateXY{12.0, 0.0}, CoordinateXY{14.0, 0.0}});
  BasicSegmentString ss_np1a(&seq_np1a, nullptr);
  BasicSegmentString ss_np1b(&seq_np1b, nullptr);

  detector.processIntersections(&ss_np1a, 0, &ss_np1b, 0);
  EXPECT_FALSE(detector.hasProperIntersection());
  EXPECT_FALSE(detector.isDone());
  ExpectSegmentsEqual(detector.getIntersectionSegments(), {0.0, 0.0},
                      {4.0, 0.0}, {2.0, 0.0}, {2.0, 3.0});

  // 3) Proper intersection replaces previously saved segments and completes.
  CoordinateSequence seq_pa({CoordinateXY{0.0, 10.0}, CoordinateXY{4.0, 14.0}});
  CoordinateSequence seq_pb({CoordinateXY{0.0, 14.0}, CoordinateXY{4.0, 10.0}});
  BasicSegmentString ss_pa(&seq_pa, nullptr);
  BasicSegmentString ss_pb(&seq_pb, nullptr);

  detector.processIntersections(&ss_pa, 0, &ss_pb, 0);
  EXPECT_TRUE(detector.hasIntersection());
  EXPECT_TRUE(detector.hasProperIntersection());
  EXPECT_TRUE(detector.hasNonProperIntersection());
  EXPECT_TRUE(detector.isDone());
  ASSERT_NE(detector.getIntersection(), nullptr);
  EXPECT_TRUE(detector.getIntersection()->equals2D(CoordinateXY{2.0, 12.0}));
  ExpectSegmentsEqual(detector.getIntersectionSegments(), {0.0, 10.0},
                      {4.0, 14.0}, {0.0, 14.0}, {4.0, 10.0});
}

TEST(SegmentIntersectionDetectorTest, FindAllTypesStopsOnlyAfterBothTypes) {
  LineIntersector li;
  SegmentIntersectionDetector detector(&li);
  detector.setFindAllIntersectionTypes(true);

  CoordinateSequence seq_pa({CoordinateXY{0.0, 0.0}, CoordinateXY{2.0, 2.0}});
  CoordinateSequence seq_pb({CoordinateXY{0.0, 2.0}, CoordinateXY{2.0, 0.0}});
  BasicSegmentString ss_pa(&seq_pa, nullptr);
  BasicSegmentString ss_pb(&seq_pb, nullptr);

  detector.processIntersections(&ss_pa, 0, &ss_pb, 0);
  EXPECT_TRUE(detector.hasIntersection());
  EXPECT_TRUE(detector.hasProperIntersection());
  EXPECT_FALSE(detector.hasNonProperIntersection());
  EXPECT_FALSE(detector.isDone());

  CoordinateSequence seq_na({CoordinateXY{5.0, 0.0}, CoordinateXY{6.0, 0.0}});
  CoordinateSequence seq_nb({CoordinateXY{6.0, 0.0}, CoordinateXY{7.0, 1.0}});
  BasicSegmentString ss_na(&seq_na, nullptr);
  BasicSegmentString ss_nb(&seq_nb, nullptr);

  detector.processIntersections(&ss_na, 0, &ss_nb, 0);
  EXPECT_TRUE(detector.hasProperIntersection());
  EXPECT_TRUE(detector.hasNonProperIntersection());
  EXPECT_TRUE(detector.isDone());
}

TEST(SegmentIntersectionDetectorTest, IntegratesWithMCIndexAndSimpleNoder) {
  PrecisionModel pm(10.0);
  LineIntersector li(&pm);
  SegmentIntersectionDetector mc_detector(&li);
  mc_detector.setFindProper(true);

  CoordinateSequence s0({CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 0.0}});
  CoordinateSequence s1({CoordinateXY{1.0, 0.0}, CoordinateXY{2.0, 0.0}});
  CoordinateSequence s2({CoordinateXY{10.0, 10.0}, CoordinateXY{12.0, 12.0}});
  CoordinateSequence s3({CoordinateXY{10.0, 12.0}, CoordinateXY{12.0, 10.0}});
  BasicSegmentString bs0(&s0, nullptr);
  BasicSegmentString bs1(&s1, nullptr);
  BasicSegmentString bs2(&s2, nullptr);
  BasicSegmentString bs3(&s3, nullptr);
  std::vector<SegmentString*> segs = {&bs0, &bs1, &bs2, &bs3};

  MCIndexNoder mc_noder(&mc_detector);
  mc_noder.computeNodes(&segs);
  EXPECT_TRUE(mc_detector.hasIntersection());
  EXPECT_TRUE(mc_detector.hasProperIntersection());
  EXPECT_TRUE(mc_detector.isDone());
  ASSERT_NE(mc_detector.getIntersection(), nullptr);
  EXPECT_TRUE(
      mc_detector.getIntersection()->equals2D(CoordinateXY{11.0, 11.0}));

  // Verify SimpleNoder driving detector when only non-proper intersection
  // exists.
  LineIntersector li_simple;
  SegmentIntersectionDetector simple_detector(&li_simple);
  simple_detector.setFindProper(true);
  std::vector<SegmentString*> non_proper_segs = {&bs0, &bs1};
  SimpleNoder simple_noder(&simple_detector);
  simple_noder.computeNodes(&non_proper_segs);
  EXPECT_TRUE(simple_detector.hasIntersection());
  EXPECT_FALSE(simple_detector.hasProperIntersection());
  EXPECT_TRUE(simple_detector.hasNonProperIntersection());
  EXPECT_FALSE(simple_detector.isDone());
  ASSERT_NE(simple_detector.getIntersection(), nullptr);
  EXPECT_TRUE(
      simple_detector.getIntersection()->equals2D(CoordinateXY{1.0, 0.0}));
}

}  // namespace
}  // namespace geos_google
