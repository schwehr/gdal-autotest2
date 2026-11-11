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

#include "third_party/geos/include/geos/operation/valid/PolygonIntersectionAnalyzer.h"

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/MCIndexNoder.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/operation/valid/PolygonRing.h"
#include "third_party/geos/include/geos/operation/valid/TopologyValidationError.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::noding::BasicSegmentString;
using geos::noding::MCIndexNoder;
using geos::noding::SegmentString;
using geos::operation::valid::PolygonIntersectionAnalyzer;
using geos::operation::valid::PolygonRing;
using geos::operation::valid::TopologyValidationError;

class PolygonIntersectionAnalyzerTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();

  std::unique_ptr<LinearRing> MakeRing(
      std::initializer_list<CoordinateXY> coords) const {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    return factory_->createLinearRing(std::move(seq));
  }

  static CoordinateSequence* MutableCoords(const LinearRing* ring) {
    return const_cast<CoordinateSequence*>(ring->getCoordinatesRO());
  }
};

TEST_F(PolygonIntersectionAnalyzerTest, InitialState) {
  for (bool allow_inverted : {false, true}) {
    PolygonIntersectionAnalyzer analyzer(allow_inverted);
    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_FALSE(analyzer.hasDoubleTouch());
    EXPECT_FALSE(analyzer.isDone());
    EXPECT_EQ(analyzer.getInvalidCode(),
              int{TopologyValidationError::oNoInvalidIntersection});
    EXPECT_TRUE(analyzer.getInvalidLocation().isNull());
    EXPECT_TRUE(analyzer.getDoubleTouchLocation().isNull());
  }
}

TEST_F(PolygonIntersectionAnalyzerTest,
       SameSegmentAndDisjointSegmentsAreValid) {
  auto ring0 = MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
  auto ring1 = MakeRing({{20, 20}, {30, 20}, {30, 30}, {20, 30}, {20, 20}});
  BasicSegmentString ss0(MutableCoords(ring0.get()), nullptr);
  BasicSegmentString ss1(MutableCoords(ring1.get()), nullptr);

  PolygonIntersectionAnalyzer analyzer(false);

  // Same SegmentString and same segment index -> early return.
  analyzer.processIntersections(&ss0, 0, &ss0, 0);
  EXPECT_FALSE(analyzer.isInvalid());
  EXPECT_FALSE(analyzer.hasDoubleTouch());
  EXPECT_FALSE(analyzer.isDone());

  // Opposite disjoint segments on the same ring.
  analyzer.processIntersections(&ss0, 0, &ss0, 2);
  EXPECT_FALSE(analyzer.isInvalid());
  EXPECT_FALSE(analyzer.isDone());

  // Disjoint segments on different rings.
  analyzer.processIntersections(&ss0, 0, &ss1, 0);
  EXPECT_FALSE(analyzer.isInvalid());
  EXPECT_FALSE(analyzer.isDone());
}

TEST_F(PolygonIntersectionAnalyzerTest, AdjacentSegmentsInSameRingAreValid) {
  auto ring = MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
  BasicSegmentString ss(MutableCoords(ring.get()), nullptr);

  PolygonIntersectionAnalyzer analyzer(false);

  // delta <= 1 with segIndex0 < segIndex1 and segIndex0 > segIndex1.
  analyzer.processIntersections(&ss, 0, &ss, 1);
  EXPECT_FALSE(analyzer.isInvalid());
  analyzer.processIntersections(&ss, 2, &ss, 1);
  EXPECT_FALSE(analyzer.isInvalid());

  // Wraparound adjacency: segIndex 0 and segIndex (size() - 2 == 3).
  analyzer.processIntersections(&ss, 0, &ss, 3);
  EXPECT_FALSE(analyzer.isInvalid());
  analyzer.processIntersections(&ss, 3, &ss, 0);
  EXPECT_FALSE(analyzer.isInvalid());
  EXPECT_FALSE(analyzer.isDone());
}

TEST_F(PolygonIntersectionAnalyzerTest, ProperCrossingReportsSelfIntersection) {
  // Same ring (bowtie): segment 0 (0,0)-(10,10) crosses segment 2
  // (10,0)-(0,10).
  {
    auto bowtie = MakeRing({{0, 0}, {10, 10}, {10, 0}, {0, 10}, {0, 0}});
    BasicSegmentString ss(MutableCoords(bowtie.get()), nullptr);
    PolygonIntersectionAnalyzer analyzer(false);

    analyzer.processIntersections(&ss, 0, &ss, 2);
    EXPECT_TRUE(analyzer.isInvalid());
    EXPECT_TRUE(analyzer.isDone());
    EXPECT_FALSE(analyzer.hasDoubleTouch());
    EXPECT_EQ(analyzer.getInvalidCode(),
              int{TopologyValidationError::eSelfIntersection});
    EXPECT_EQ(analyzer.getInvalidLocation(), CoordinateXY(5, 5));
  }

  // Different rings crossing properly.
  {
    auto ring0 = MakeRing({{0, 0}, {10, 10}, {10, 0}, {0, 0}});
    auto ring1 = MakeRing({{0, 10}, {10, 0}, {0, 0}, {0, 10}});
    BasicSegmentString ss0(MutableCoords(ring0.get()), nullptr);
    BasicSegmentString ss1(MutableCoords(ring1.get()), nullptr);
    PolygonIntersectionAnalyzer analyzer(false);

    analyzer.processIntersections(&ss0, 0, &ss1, 0);
    EXPECT_TRUE(analyzer.isInvalid());
    EXPECT_TRUE(analyzer.isDone());
    EXPECT_EQ(analyzer.getInvalidCode(),
              int{TopologyValidationError::eSelfIntersection});
    EXPECT_EQ(analyzer.getInvalidLocation(), CoordinateXY(5, 5));
  }
}

TEST_F(PolygonIntersectionAnalyzerTest,
       CollinearOverlapReportsSelfIntersection) {
  // Different rings with collinear overlapping segments.
  {
    auto ring0 = MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
    auto ring1 = MakeRing({{2, 0}, {8, 0}, {8, -5}, {2, -5}, {2, 0}});
    BasicSegmentString ss0(MutableCoords(ring0.get()), nullptr);
    BasicSegmentString ss1(MutableCoords(ring1.get()), nullptr);
    PolygonIntersectionAnalyzer analyzer(false);

    analyzer.processIntersections(&ss0, 0, &ss1, 0);
    EXPECT_TRUE(analyzer.isInvalid());
    EXPECT_TRUE(analyzer.isDone());
    EXPECT_EQ(analyzer.getInvalidCode(),
              int{TopologyValidationError::eSelfIntersection});
    EXPECT_EQ(analyzer.getInvalidLocation(), CoordinateXY(2, 0));
  }

  // Adjacent collinear spike segments on the same ring.
  // LineIntersector puts q1 = (10, 0) first since both endpoints of segment 1
  // lie within segment 0.
  {
    auto spike = MakeRing({{0, 0}, {10, 0}, {5, 0}, {10, 10}, {0, 10}, {0, 0}});
    BasicSegmentString ss(MutableCoords(spike.get()), nullptr);
    PolygonIntersectionAnalyzer analyzer(false);

    analyzer.processIntersections(&ss, 0, &ss, 1);
    EXPECT_TRUE(analyzer.isInvalid());
    EXPECT_TRUE(analyzer.isDone());
    EXPECT_EQ(analyzer.getInvalidCode(),
              int{TopologyValidationError::eSelfIntersection});
    EXPECT_EQ(analyzer.getInvalidLocation(), CoordinateXY(10, 0));
  }
}

TEST_F(PolygonIntersectionAnalyzerTest,
       SameRingVertexTouchInvalidWhenInvertedRingDisallowed) {
  auto pinch =
      MakeRing({{0, 0}, {10, 0}, {5, 5}, {10, 10}, {0, 10}, {5, 5}, {0, 0}});
  BasicSegmentString ss(MutableCoords(pinch.get()), nullptr);
  PolygonIntersectionAnalyzer analyzer(false);

  // Segments 2 and 5 are non-adjacent (delta = 3 < 5) and touch at (5, 5).
  analyzer.processIntersections(&ss, 2, &ss, 5);
  EXPECT_TRUE(analyzer.isInvalid());
  EXPECT_TRUE(analyzer.isDone());
  EXPECT_EQ(analyzer.getInvalidCode(),
            int{TopologyValidationError::eRingSelfIntersection});
  EXPECT_EQ(analyzer.getInvalidLocation(), CoordinateXY(5, 5));

  // A subsequent non-invalid call must preserve the recorded error.
  analyzer.processIntersections(&ss, 0, &ss, 1);
  EXPECT_TRUE(analyzer.isInvalid());
  EXPECT_EQ(analyzer.getInvalidCode(),
            int{TopologyValidationError::eRingSelfIntersection});
  EXPECT_EQ(analyzer.getInvalidLocation(), CoordinateXY(5, 5));
}

TEST_F(PolygonIntersectionAnalyzerTest,
       EndpointSkipOptimizationDefersCrossingUntilStartVertex) {
  auto ring0 = MakeRing({{0, 0}, {5, 5}, {10, 10}, {10, 0}, {0, 0}});
  auto ring1 = MakeRing({{0, 5}, {5, 5}, {10, 5}, {5, 8}, {0, 5}});
  BasicSegmentString ss0(MutableCoords(ring0.get()), nullptr);
  BasicSegmentString ss1(MutableCoords(ring1.get()), nullptr);

  // intPt == p01 && intPt == p11 -> skipped.
  {
    PolygonIntersectionAnalyzer analyzer(false);
    analyzer.processIntersections(&ss0, 0, &ss1, 0);
    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_FALSE(analyzer.isDone());
  }

  // intPt == p01 (not p11) -> skipped.
  {
    PolygonIntersectionAnalyzer analyzer(false);
    analyzer.processIntersections(&ss0, 0, &ss1, 1);
    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_FALSE(analyzer.isDone());
  }

  // intPt == p11 (not p01) -> skipped.
  {
    PolygonIntersectionAnalyzer analyzer(false);
    analyzer.processIntersections(&ss0, 1, &ss1, 0);
    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_FALSE(analyzer.isDone());
  }

  // intPt == p00 && intPt == p10 -> evaluated and found crossing.
  {
    PolygonIntersectionAnalyzer analyzer(false);
    analyzer.processIntersections(&ss0, 1, &ss1, 1);
    EXPECT_TRUE(analyzer.isInvalid());
    EXPECT_TRUE(analyzer.isDone());
    EXPECT_EQ(analyzer.getInvalidCode(),
              int{TopologyValidationError::eSelfIntersection});
    EXPECT_EQ(analyzer.getInvalidLocation(), CoordinateXY(5, 5));
  }
}

TEST_F(PolygonIntersectionAnalyzerTest,
       VertexAtSegIndexZeroUsesPrevCoordinateWraparound) {
  auto ring0 = MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});

  // Crossing at index 0 vertex (0, 0) for both rings.
  {
    auto ring1 = MakeRing({{0, 0}, {5, 5}, {-5, 5}, {-5, -5}, {0, 0}});
    BasicSegmentString ss0(MutableCoords(ring0.get()), nullptr);
    BasicSegmentString ss1(MutableCoords(ring1.get()), nullptr);
    PolygonIntersectionAnalyzer analyzer(false);

    analyzer.processIntersections(&ss0, 0, &ss1, 0);
    EXPECT_TRUE(analyzer.isInvalid());
    EXPECT_EQ(analyzer.getInvalidCode(),
              int{TopologyValidationError::eSelfIntersection});
    EXPECT_EQ(analyzer.getInvalidLocation(), CoordinateXY(0, 0));
  }

  // Non-crossing touch at index 0 vertex (0, 0) for both rings.
  {
    auto ring1 = MakeRing({{0, 0}, {-5, 0}, {-5, -5}, {0, -5}, {0, 0}});
    BasicSegmentString ss0(MutableCoords(ring0.get()), nullptr);
    BasicSegmentString ss1(MutableCoords(ring1.get()), nullptr);
    PolygonIntersectionAnalyzer analyzer(false);

    analyzer.processIntersections(&ss0, 0, &ss1, 0);
    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_FALSE(analyzer.hasDoubleTouch());
    EXPECT_FALSE(analyzer.isDone());
  }
}

TEST_F(PolygonIntersectionAnalyzerTest,
       VertexOnSegmentInteriorCrossingAndTouching) {
  auto base = MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});

  // intPt interior to ss0 segment 0 and equals ss1 p10: crossing.
  {
    auto crossing = MakeRing({{5, 0}, {5, 5}, {0, 5}, {5, -5}, {5, 0}});
    BasicSegmentString ss0(MutableCoords(base.get()), nullptr);
    BasicSegmentString ss1(MutableCoords(crossing.get()), nullptr);
    PolygonIntersectionAnalyzer analyzer(false);

    analyzer.processIntersections(&ss0, 0, &ss1, 0);
    EXPECT_TRUE(analyzer.isInvalid());
    EXPECT_EQ(analyzer.getInvalidCode(),
              int{TopologyValidationError::eSelfIntersection});
    EXPECT_EQ(analyzer.getInvalidLocation(), CoordinateXY(5, 0));
  }

  // intPt interior to ss0 segment 0 and equals ss1 p10: non-crossing touch.
  {
    auto tangent = MakeRing({{5, 0}, {8, 5}, {2, 5}, {5, 0}});
    BasicSegmentString ss0(MutableCoords(base.get()), nullptr);
    BasicSegmentString ss1(MutableCoords(tangent.get()), nullptr);
    PolygonIntersectionAnalyzer analyzer(false);

    analyzer.processIntersections(&ss0, 0, &ss1, 0);
    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_FALSE(analyzer.isDone());
  }

  // intPt equals ss0 p00 and interior to ss1 segment 0: crossing.
  {
    auto crossing = MakeRing({{5, 0}, {5, 5}, {0, 5}, {5, -5}, {5, 0}});
    BasicSegmentString ss0(MutableCoords(crossing.get()), nullptr);
    BasicSegmentString ss1(MutableCoords(base.get()), nullptr);
    PolygonIntersectionAnalyzer analyzer(false);

    analyzer.processIntersections(&ss0, 0, &ss1, 0);
    EXPECT_TRUE(analyzer.isInvalid());
    EXPECT_EQ(analyzer.getInvalidCode(),
              int{TopologyValidationError::eSelfIntersection});
    EXPECT_EQ(analyzer.getInvalidLocation(), CoordinateXY(5, 0));
  }
}

TEST_F(PolygonIntersectionAnalyzerTest,
       SelfTouchWithInvertedRingValidThrowsIfMissingPolygonRingData) {
  auto ring = MakeRing({{0, 0},
                        {0, 10},
                        {5, 10},
                        {2, 4},
                        {8, 4},
                        {5, 10},
                        {10, 10},
                        {10, 0},
                        {0, 0}});
  BasicSegmentString ss(MutableCoords(ring.get()), nullptr);
  PolygonIntersectionAnalyzer analyzer(true);

  EXPECT_THROW(analyzer.processIntersections(&ss, 2, &ss, 5),
               geos::util::IllegalStateException);
  EXPECT_FALSE(analyzer.isInvalid());
  EXPECT_FALSE(analyzer.isDone());
}

TEST_F(PolygonIntersectionAnalyzerTest,
       SelfTouchWithInvertedRingValidRecordsSelfNodes) {
  // Valid exterior self-touch: CW shell with CCW hole loop touching at (5, 10).
  {
    auto ring = MakeRing({{0, 0},
                          {0, 10},
                          {5, 10},
                          {2, 4},
                          {8, 4},
                          {5, 10},
                          {10, 10},
                          {10, 0},
                          {0, 0}});
    PolygonRing poly_ring(ring.get());
    BasicSegmentString ss(MutableCoords(ring.get()), &poly_ring);
    PolygonIntersectionAnalyzer analyzer(true);

    analyzer.processIntersections(&ss, 2, &ss, 5);
    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_FALSE(analyzer.hasDoubleTouch());
    EXPECT_FALSE(analyzer.isDone());
    EXPECT_EQ(poly_ring.findInteriorSelfNode(), nullptr);
  }

  // Invalid interior self-touch (bowtie-like touch at (5, 5)).
  {
    auto ring =
        MakeRing({{0, 0}, {10, 0}, {5, 5}, {10, 10}, {0, 10}, {5, 5}, {0, 0}});
    PolygonRing poly_ring(ring.get());
    BasicSegmentString ss(MutableCoords(ring.get()), &poly_ring);
    PolygonIntersectionAnalyzer analyzer(true);

    analyzer.processIntersections(&ss, 2, &ss, 5);
    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_FALSE(analyzer.hasDoubleTouch());
    EXPECT_FALSE(analyzer.isDone());
    const CoordinateXY* interior_node = poly_ring.findInteriorSelfNode();
    ASSERT_NE(interior_node, nullptr);
    EXPECT_EQ(*interior_node, CoordinateXY(5, 5));
  }

  // Two distinct self-touches on the same SegmentString must not trigger
  // hasDoubleTouch() (which applies only across different SegmentStrings).
  {
    auto ring = MakeRing({{0, 0},
                          {0, 10},
                          {5, 10},
                          {3, 7},
                          {7, 7},
                          {5, 10},
                          {10, 10},
                          {10, 0},
                          {5, 0},
                          {7, 3},
                          {3, 3},
                          {5, 0},
                          {0, 0}});
    PolygonRing poly_ring(ring.get());
    BasicSegmentString ss(MutableCoords(ring.get()), &poly_ring);
    PolygonIntersectionAnalyzer analyzer(true);

    analyzer.processIntersections(&ss, 2, &ss, 5);
    EXPECT_FALSE(analyzer.hasDoubleTouch());
    analyzer.processIntersections(&ss, 8, &ss, 11);
    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_FALSE(analyzer.hasDoubleTouch());
    EXPECT_FALSE(analyzer.isDone());
    EXPECT_EQ(poly_ring.findInteriorSelfNode(), nullptr);
  }
}

TEST_F(PolygonIntersectionAnalyzerTest,
       DoubleTouchBetweenRingsInSamePolygonDetected) {
  auto shell = MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
  auto hole = MakeRing({{5, 0}, {8, 5}, {5, 10}, {2, 5}, {5, 0}});

  PolygonRing shell_pr(shell.get());
  PolygonRing hole_pr(hole.get(), 0, &shell_pr);
  BasicSegmentString shell_ss(MutableCoords(shell.get()), &shell_pr);
  BasicSegmentString hole_ss(MutableCoords(hole.get()), &hole_pr);

  PolygonIntersectionAnalyzer analyzer(false);

  // First touch at (5, 0).
  analyzer.processIntersections(&shell_ss, 0, &hole_ss, 0);
  EXPECT_FALSE(analyzer.isInvalid());
  EXPECT_FALSE(analyzer.hasDoubleTouch());
  EXPECT_FALSE(analyzer.isDone());

  // Re-visiting the exact same touch point (5, 0) is not a double touch.
  analyzer.processIntersections(&shell_ss, 0, &hole_ss, 0);
  EXPECT_FALSE(analyzer.isInvalid());
  EXPECT_FALSE(analyzer.hasDoubleTouch());
  EXPECT_FALSE(analyzer.isDone());

  // Second touch at distinct point (5, 10) triggers double touch.
  analyzer.processIntersections(&shell_ss, 2, &hole_ss, 2);
  EXPECT_FALSE(analyzer.isInvalid());
  EXPECT_TRUE(analyzer.hasDoubleTouch());
  EXPECT_TRUE(analyzer.isDone());
  EXPECT_EQ(analyzer.getDoubleTouchLocation(), CoordinateXY(5, 10));
}

TEST_F(PolygonIntersectionAnalyzerTest,
       MultipleTouchesAcrossDifferentPolygonsOrNullDataNotDoubleTouch) {
  auto shell0 = MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
  auto shell1 = MakeRing({{5, 0}, {8, 5}, {5, 10}, {2, 5}, {5, 0}});

  // Different PolygonRing shells -> not same polygon.
  {
    PolygonRing pr0(shell0.get());
    PolygonRing pr1(shell1.get());
    BasicSegmentString ss0(MutableCoords(shell0.get()), &pr0);
    BasicSegmentString ss1(MutableCoords(shell1.get()), &pr1);
    PolygonIntersectionAnalyzer analyzer(false);

    analyzer.processIntersections(&ss0, 0, &ss1, 0);
    analyzer.processIntersections(&ss0, 2, &ss1, 2);
    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_FALSE(analyzer.hasDoubleTouch());
    EXPECT_FALSE(analyzer.isDone());
  }

  // Missing PolygonRing data -> ignored by addDoubleTouch.
  {
    BasicSegmentString ss0(MutableCoords(shell0.get()), nullptr);
    BasicSegmentString ss1(MutableCoords(shell1.get()), nullptr);
    PolygonIntersectionAnalyzer analyzer(false);

    analyzer.processIntersections(&ss0, 0, &ss1, 0);
    analyzer.processIntersections(&ss0, 2, &ss1, 2);
    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_FALSE(analyzer.hasDoubleTouch());
    EXPECT_FALSE(analyzer.isDone());
  }
}

TEST_F(PolygonIntersectionAnalyzerTest,
       CollinearSingleEndpointTouchAcrossRingsIsValid) {
  auto ring0 = MakeRing({{5, 0}, {0, 0}, {0, 5}, {5, 0}});
  auto ring1 = MakeRing({{5, 0}, {10, 0}, {10, 5}, {5, 0}});
  BasicSegmentString ss0(MutableCoords(ring0.get()), nullptr);
  BasicSegmentString ss1(MutableCoords(ring1.get()), nullptr);
  PolygonIntersectionAnalyzer analyzer(false);

  analyzer.processIntersections(&ss0, 0, &ss1, 0);
  EXPECT_FALSE(analyzer.isInvalid());
  EXPECT_FALSE(analyzer.hasDoubleTouch());
  EXPECT_FALSE(analyzer.isDone());
}

TEST_F(PolygonIntersectionAnalyzerTest,
       SameRingVertexTouchesSegmentInteriorWhenInvertedRingValid) {
  auto ring =
      MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {5, 0}, {0, 5}, {0, 0}});
  PolygonRing poly_ring(ring.get());
  BasicSegmentString ss(MutableCoords(ring.get()), &poly_ring);
  PolygonIntersectionAnalyzer analyzer(true);

  // Segment 4 starts at (5, 0) on the interior of segment 0 (0,0)-(10,0).
  analyzer.processIntersections(&ss, 0, &ss, 4);
  EXPECT_FALSE(analyzer.isInvalid());
  EXPECT_FALSE(analyzer.hasDoubleTouch());
  EXPECT_FALSE(analyzer.isDone());
  const CoordinateXY* interior_node = poly_ring.findInteriorSelfNode();
  ASSERT_NE(interior_node, nullptr);
  EXPECT_EQ(*interior_node, CoordinateXY(5, 0));
}

TEST_F(PolygonIntersectionAnalyzerTest, IntegrationWithMCIndexNoder) {
  // Bowtie ring detected via MCIndexNoder.
  {
    auto bowtie = MakeRing({{0, 0}, {10, 10}, {10, 0}, {0, 10}, {0, 0}});
    BasicSegmentString ss(MutableCoords(bowtie.get()), nullptr);
    std::vector<SegmentString*> seg_strings = {&ss};

    PolygonIntersectionAnalyzer analyzer(false);
    MCIndexNoder noder(&analyzer);
    noder.computeNodes(&seg_strings);

    EXPECT_TRUE(analyzer.isInvalid());
    EXPECT_TRUE(analyzer.isDone());
    EXPECT_EQ(analyzer.getInvalidCode(),
              int{TopologyValidationError::eSelfIntersection});
    EXPECT_EQ(analyzer.getInvalidLocation(), CoordinateXY(5, 5));
  }

  // Double-touching hole detected via MCIndexNoder.
  {
    auto shell = MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
    auto hole = MakeRing({{5, 0}, {8, 5}, {5, 10}, {2, 5}, {5, 0}});
    PolygonRing shell_pr(shell.get());
    PolygonRing hole_pr(hole.get(), 0, &shell_pr);
    BasicSegmentString shell_ss(MutableCoords(shell.get()), &shell_pr);
    BasicSegmentString hole_ss(MutableCoords(hole.get()), &hole_pr);
    std::vector<SegmentString*> seg_strings = {&shell_ss, &hole_ss};

    PolygonIntersectionAnalyzer analyzer(false);
    MCIndexNoder noder(&analyzer);
    noder.computeNodes(&seg_strings);

    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_TRUE(analyzer.hasDoubleTouch());
    EXPECT_TRUE(analyzer.isDone());
    const CoordinateXY& touch_loc = analyzer.getDoubleTouchLocation();
    EXPECT_TRUE(touch_loc == CoordinateXY(5, 0) ||
                touch_loc == CoordinateXY(5, 10));
  }

  // Valid inverted-ring self-touch processed via MCIndexNoder.
  {
    auto ring = MakeRing({{0, 0},
                          {0, 10},
                          {5, 10},
                          {2, 4},
                          {8, 4},
                          {5, 10},
                          {10, 10},
                          {10, 0},
                          {0, 0}});
    PolygonRing poly_ring(ring.get());
    BasicSegmentString ss(MutableCoords(ring.get()), &poly_ring);
    std::vector<SegmentString*> seg_strings = {&ss};

    PolygonIntersectionAnalyzer analyzer(true);
    MCIndexNoder noder(&analyzer);
    noder.computeNodes(&seg_strings);

    EXPECT_FALSE(analyzer.isInvalid());
    EXPECT_FALSE(analyzer.hasDoubleTouch());
    EXPECT_FALSE(analyzer.isDone());
    EXPECT_EQ(poly_ring.findInteriorSelfNode(), nullptr);
  }
}

}  // namespace
}  // namespace geos_google
