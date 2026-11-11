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

// Unit tests for third_party/geos/src/coverage/InvalidSegmentDetector.cpp and
// third_party/geos/include/geos/coverage/InvalidSegmentDetector.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/coverage/InvalidSegmentDetector.cpp

#include "third_party/geos/include/geos/coverage/InvalidSegmentDetector.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/coverage/CoverageRing.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/noding/MCIndexSegmentSetMutualIntersector.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using geos::coverage::CoverageRing;
using geos::coverage::InvalidSegmentDetector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::noding::MCIndexSegmentSetMutualIntersector;
using geos::noding::SegmentString;

struct OwnedCoverageRing {
  std::unique_ptr<CoordinateSequence> seq;
  std::unique_ptr<CoverageRing> ring;
};

OwnedCoverageRing MakeRing(const std::vector<Coordinate>& coords,
                           bool interior_on_right) {
  auto seq = std::make_unique<CoordinateSequence>();
  for (const Coordinate& coord : coords) {
    seq->add(coord);
  }
  auto ring = std::make_unique<CoverageRing>(seq.get(), interior_on_right);
  return {.seq = std::move(seq), .ring = std::move(ring)};
}

// CW unit box [0,10]x[0,10]; interior is on the right of each directed segment.
std::vector<Coordinate> CwBoxCoords() {
  return {
      Coordinate(0.0, 0.0),  Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
      Coordinate(10.0, 0.0), Coordinate(0.0, 0.0),
  };
}

// CCW unit box [0,10]x[0,10]; interior is on the left of each directed segment.
std::vector<Coordinate> CcwBoxCoords() {
  return {
      Coordinate(0.0, 0.0),  Coordinate(10.0, 0.0), Coordinate(10.0, 10.0),
      Coordinate(0.0, 10.0), Coordinate(0.0, 0.0),
  };
}

TEST(InvalidSegmentDetectorTest, IsDoneAlwaysFalse) {
  InvalidSegmentDetector default_detector;
  EXPECT_FALSE(default_detector.isDone());

  InvalidSegmentDetector tol_detector(0.5);
  EXPECT_FALSE(tol_detector.isDone());
}

TEST(InvalidSegmentDetectorTest, SkipsTargetWithKnownStatus) {
  OwnedCoverageRing adj = MakeRing(
      {Coordinate(-5.0, 5.0), Coordinate(5.0, 5.0), Coordinate(5.0, 15.0),
       Coordinate(-5.0, 15.0), Coordinate(-5.0, 5.0)},
      /*interior_on_right=*/true);
  OwnedCoverageRing target_matched =
      MakeRing(CwBoxCoords(), /*interior_on_right=*/true);
  target_matched.ring->markMatched(0);

  InvalidSegmentDetector detector(0.0);
  detector.processIntersections(adj.ring.get(), 0, target_matched.ring.get(),
                                0);
  EXPECT_FALSE(target_matched.ring->isInvalid(0));

  OwnedCoverageRing target_already_invalid =
      MakeRing(CwBoxCoords(), /*interior_on_right=*/true);
  target_already_invalid.ring->markInvalid(0);
  detector.processIntersections(adj.ring.get(), 0,
                                target_already_invalid.ring.get(), 0);
  EXPECT_TRUE(target_already_invalid.ring->isInvalid(0));

  OwnedCoverageRing target_unknown =
      MakeRing(CwBoxCoords(), /*interior_on_right=*/true);
  detector.processIntersections(adj.ring.get(), 0, target_unknown.ring.get(),
                                0);
  EXPECT_TRUE(target_unknown.ring->isInvalid(0));
}

TEST(InvalidSegmentDetectorTest, SkipsZeroLengthSegments) {
  // Zero-length target segment lying directly on an adjacent segment.
  OwnedCoverageRing adj =
      MakeRing({Coordinate(0.0, 5.0), Coordinate(10.0, 5.0),
                Coordinate(10.0, 10.0), Coordinate(0.0, 5.0)},
               /*interior_on_right=*/true);
  OwnedCoverageRing zero_target =
      MakeRing({Coordinate(5.0, 5.0), Coordinate(5.0, 5.0),
                Coordinate(5.0, 8.0), Coordinate(5.0, 5.0)},
               /*interior_on_right=*/true);

  InvalidSegmentDetector detector(0.0);
  detector.processIntersections(adj.ring.get(), 0, zero_target.ring.get(), 0);
  EXPECT_FALSE(zero_target.ring->isInvalid(0));

  // Zero-length adjacent segment lying on a non-zero target segment.
  OwnedCoverageRing zero_adj =
      MakeRing({Coordinate(5.0, 5.0), Coordinate(5.0, 5.0),
                Coordinate(5.0, 8.0), Coordinate(5.0, 5.0)},
               /*interior_on_right=*/true);
  OwnedCoverageRing normal_target =
      MakeRing({Coordinate(0.0, 5.0), Coordinate(10.0, 5.0),
                Coordinate(10.0, 10.0), Coordinate(0.0, 5.0)},
               /*interior_on_right=*/true);
  detector.processIntersections(zero_adj.ring.get(), 0,
                                normal_target.ring.get(), 0);
  EXPECT_FALSE(normal_target.ring->isInvalid(0));
}

TEST(InvalidSegmentDetectorTest, SkipsEqualSegmentsRegardlessOfDirection) {
  OwnedCoverageRing adj = MakeRing(
      {Coordinate(0.0, 0.0), Coordinate(10.0, 0.0), Coordinate(10.0, 10.0),
       Coordinate(0.0, 10.0), Coordinate(0.0, 0.0)},
      /*interior_on_right=*/false);
  OwnedCoverageRing same_dir = MakeRing(
      {Coordinate(0.0, 0.0), Coordinate(10.0, 0.0), Coordinate(10.0, -5.0),
       Coordinate(0.0, -5.0), Coordinate(0.0, 0.0)},
      /*interior_on_right=*/true);
  OwnedCoverageRing rev_dir = MakeRing(
      {Coordinate(10.0, 0.0), Coordinate(0.0, 0.0), Coordinate(0.0, -5.0),
       Coordinate(10.0, -5.0), Coordinate(10.0, 0.0)},
      /*interior_on_right=*/false);

  InvalidSegmentDetector detector(1.0);
  detector.processIntersections(adj.ring.get(), 0, same_dir.ring.get(), 0);
  EXPECT_FALSE(same_dir.ring->isInvalid(0));

  detector.processIntersections(adj.ring.get(), 0, rev_dir.ring.get(), 0);
  EXPECT_FALSE(rev_dir.ring->isInvalid(0));
}

TEST(InvalidSegmentDetectorTest, DetectsNonEqualCollinearOverlaps) {
  OwnedCoverageRing adj = MakeRing(
      {Coordinate(0.0, 0.0), Coordinate(10.0, 0.0), Coordinate(10.0, 10.0),
       Coordinate(0.0, 10.0), Coordinate(0.0, 0.0)},
      /*interior_on_right=*/false);

  InvalidSegmentDetector detector(0.0);

  // Shares adj0 only (partial overlap).
  OwnedCoverageRing partial = MakeRing(
      {Coordinate(0.0, 0.0), Coordinate(6.0, 0.0), Coordinate(6.0, -4.0),
       Coordinate(0.0, -4.0), Coordinate(0.0, 0.0)},
      /*interior_on_right=*/true);
  detector.processIntersections(adj.ring.get(), 0, partial.ring.get(), 0);
  EXPECT_TRUE(partial.ring->isInvalid(0));

  // Shares adj1 only in reverse (partial overlap).
  OwnedCoverageRing rev_partial = MakeRing(
      {Coordinate(10.0, 0.0), Coordinate(4.0, 0.0), Coordinate(4.0, -4.0),
       Coordinate(10.0, -4.0), Coordinate(10.0, 0.0)},
      /*interior_on_right=*/false);
  detector.processIntersections(adj.ring.get(), 0, rev_partial.ring.get(), 0);
  EXPECT_TRUE(rev_partial.ring->isInvalid(0));

  // Target strictly contained inside adjacent segment.
  OwnedCoverageRing contained = MakeRing(
      {Coordinate(2.0, 0.0), Coordinate(8.0, 0.0), Coordinate(8.0, -4.0),
       Coordinate(2.0, -4.0), Coordinate(2.0, 0.0)},
      /*interior_on_right=*/true);
  detector.processIntersections(adj.ring.get(), 0, contained.ring.get(), 0);
  EXPECT_TRUE(contained.ring->isInvalid(0));

  // Target strictly contains adjacent segment.
  OwnedCoverageRing containing = MakeRing(
      {Coordinate(-2.0, 0.0), Coordinate(12.0, 0.0), Coordinate(12.0, -4.0),
       Coordinate(-2.0, -4.0), Coordinate(-2.0, 0.0)},
      /*interior_on_right=*/true);
  detector.processIntersections(adj.ring.get(), 0, containing.ring.get(), 0);
  EXPECT_TRUE(containing.ring->isInvalid(0));
}

TEST(InvalidSegmentDetectorTest, DetectsCrossingAndInteriorPointIntersections) {
  OwnedCoverageRing adj = MakeRing(CwBoxCoords(), /*interior_on_right=*/true);
  InvalidSegmentDetector detector(0.0);

  // Disjoint segment -> valid.
  OwnedCoverageRing disjoint =
      MakeRing({Coordinate(20.0, 0.0), Coordinate(20.0, 10.0),
                Coordinate(30.0, 10.0), Coordinate(20.0, 0.0)},
               /*interior_on_right=*/true);
  detector.processIntersections(adj.ring.get(), 0, disjoint.ring.get(), 0);
  EXPECT_FALSE(disjoint.ring->isInvalid(0));

  // Proper crossing of adj[0] = (0,0)->(0,10).
  OwnedCoverageRing crossing =
      MakeRing({Coordinate(-5.0, 5.0), Coordinate(5.0, 5.0),
                Coordinate(5.0, 15.0), Coordinate(-5.0, 5.0)},
               /*interior_on_right=*/true);
  detector.processIntersections(adj.ring.get(), 0, crossing.ring.get(), 0);
  EXPECT_TRUE(crossing.ring->isInvalid(0));

  // T-junction: target endpoint lies in interior of adj[0].
  OwnedCoverageRing t_tgt_endpoint =
      MakeRing({Coordinate(0.0, 5.0), Coordinate(-5.0, 5.0),
                Coordinate(-5.0, 8.0), Coordinate(0.0, 5.0)},
               /*interior_on_right=*/true);
  detector.processIntersections(adj.ring.get(), 0, t_tgt_endpoint.ring.get(),
                                0);
  EXPECT_TRUE(t_tgt_endpoint.ring->isInvalid(0));

  // T-junction: adj endpoint (0,0) lies in interior of target segment.
  OwnedCoverageRing t_adj_endpoint =
      MakeRing({Coordinate(0.0, -5.0), Coordinate(0.0, 5.0),
                Coordinate(-5.0, 5.0), Coordinate(0.0, -5.0)},
               /*interior_on_right=*/false);
  detector.processIntersections(adj.ring.get(), 0, t_adj_endpoint.ring.get(),
                                0);
  EXPECT_TRUE(t_adj_endpoint.ring->isInvalid(0));
}

TEST(InvalidSegmentDetectorTest,
     EndpointIntersectionChecksInteriorForBothOrientations) {
  OwnedCoverageRing adj_cw =
      MakeRing(CwBoxCoords(), /*interior_on_right=*/true);
  OwnedCoverageRing adj_ccw =
      MakeRing(CcwBoxCoords(), /*interior_on_right=*/false);
  InvalidSegmentDetector detector(0.0);

  // Target touches corner (0,0) via tgt0 or tgt1, going into interior (5,5)
  // vs exterior (-5,5).
  struct CornerTest {
    Coordinate tgt0;
    Coordinate tgt1;
    bool expected_invalid;
  };
  const std::vector<CornerTest> corner_cases = {
      {Coordinate(0.0, 0.0), Coordinate(5.0, 5.0), true},
      {Coordinate(5.0, 5.0), Coordinate(0.0, 0.0), true},
      {Coordinate(0.0, 0.0), Coordinate(-5.0, 5.0), false},
      {Coordinate(-5.0, 5.0), Coordinate(0.0, 0.0), false},
  };

  for (const CornerTest& tc : corner_cases) {
    OwnedCoverageRing tgt_for_cw =
        MakeRing({tc.tgt0, tc.tgt1, Coordinate(-5.0, -5.0), tc.tgt0},
                 /*interior_on_right=*/true);
    detector.processIntersections(adj_cw.ring.get(), 0, tgt_for_cw.ring.get(),
                                  0);
    EXPECT_EQ(tgt_for_cw.ring->isInvalid(0), tc.expected_invalid);

    OwnedCoverageRing tgt_for_ccw =
        MakeRing({tc.tgt0, tc.tgt1, Coordinate(-5.0, -5.0), tc.tgt0},
                 /*interior_on_right=*/true);
    detector.processIntersections(adj_ccw.ring.get(), 0, tgt_for_ccw.ring.get(),
                                  0);
    EXPECT_EQ(tgt_for_ccw.ring->isInvalid(0), tc.expected_invalid);
  }

  // Opposite corner (10,10) exercises angle wrap-around in PolygonNodeTopology.
  OwnedCoverageRing opp_interior =
      MakeRing({Coordinate(10.0, 10.0), Coordinate(5.0, 5.0),
                Coordinate(5.0, 12.0), Coordinate(10.0, 10.0)},
               /*interior_on_right=*/true);
  detector.processIntersections(adj_cw.ring.get(), 1, opp_interior.ring.get(),
                                0);
  EXPECT_TRUE(opp_interior.ring->isInvalid(0));

  OwnedCoverageRing opp_exterior =
      MakeRing({Coordinate(10.0, 10.0), Coordinate(15.0, 15.0),
                Coordinate(15.0, 10.0), Coordinate(10.0, 10.0)},
               /*interior_on_right=*/true);
  detector.processIntersections(adj_cw.ring.get(), 1, opp_exterior.ring.get(),
                                0);
  EXPECT_FALSE(opp_exterior.ring->isInvalid(0));
}

TEST(InvalidSegmentDetectorTest,
     EndpointMatchingAdjacentCornerSegmentsAreValid) {
  // At vertex (0,0) of CW box, segment 0 is (0,0)->(0,10) and segment 3 is
  // (10,0)->(0,0).
  OwnedCoverageRing adj_cw =
      MakeRing(CwBoxCoords(), /*interior_on_right=*/true);
  InvalidSegmentDetector detector(0.0);

  // Target matches adjPrev=(10,0) at corner (0,0) while tested against adj[0].
  OwnedCoverageRing tgt_matches_prev =
      MakeRing({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                Coordinate(5.0, -5.0), Coordinate(0.0, 0.0)},
               /*interior_on_right=*/false);
  detector.processIntersections(adj_cw.ring.get(), 0,
                                tgt_matches_prev.ring.get(), 0);
  EXPECT_FALSE(tgt_matches_prev.ring->isInvalid(0));

  // Target matches adjNext=(0,10) at corner (0,0) while tested against adj[3].
  OwnedCoverageRing tgt_matches_next =
      MakeRing({Coordinate(0.0, 0.0), Coordinate(0.0, 10.0),
                Coordinate(-5.0, 5.0), Coordinate(0.0, 0.0)},
               /*interior_on_right=*/false);
  detector.processIntersections(adj_cw.ring.get(), 3,
                                tgt_matches_next.ring.get(), 0);
  EXPECT_FALSE(tgt_matches_next.ring->isInvalid(0));
}

TEST(InvalidSegmentDetectorTest, NearlyParallelDetectionAllBranches) {
  // With distanceTol == 0, close parallel segments are not flagged.
  {
    OwnedCoverageRing adj =
        MakeRing({Coordinate(0.0, 0.2), Coordinate(10.0, 0.2),
                  Coordinate(10.0, 5.0), Coordinate(0.0, 0.2)},
                 /*interior_on_right=*/false);
    OwnedCoverageRing tgt =
        MakeRing({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                  Coordinate(10.0, -5.0), Coordinate(0.0, 0.0)},
                 /*interior_on_right=*/true);
    InvalidSegmentDetector zero_tol(0.0);
    zero_tol.processIntersections(adj.ring.get(), 0, tgt.ring.get(), 0);
    EXPECT_FALSE(tgt.ring->isInvalid(0));
  }

  InvalidSegmentDetector detector(1.0);

  // 1) line0.project(line1) == false (adj lies completely past target end).
  {
    OwnedCoverageRing adj =
        MakeRing({Coordinate(12.0, 0.2), Coordinate(20.0, 0.2),
                  Coordinate(20.0, 5.0), Coordinate(12.0, 0.2)},
                 /*interior_on_right=*/false);
    OwnedCoverageRing tgt =
        MakeRing({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                  Coordinate(10.0, -5.0), Coordinate(0.0, 0.0)},
                 /*interior_on_right=*/true);
    detector.processIntersections(adj.ring.get(), 0, tgt.ring.get(), 0);
    EXPECT_FALSE(tgt.ring->isInvalid(0));
  }

  // 2) line0.project(line1) == true, line1.project(line0) == false.
  {
    OwnedCoverageRing adj =
        MakeRing({Coordinate(2.0, 10.0), Coordinate(3.0, 5.0),
                  Coordinate(6.0, 10.0), Coordinate(2.0, 10.0)},
                 /*interior_on_right=*/false);
    OwnedCoverageRing tgt =
        MakeRing({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                  Coordinate(10.0, -5.0), Coordinate(0.0, 0.0)},
                 /*interior_on_right=*/true);
    detector.processIntersections(adj.ring.get(), 0, tgt.ring.get(), 0);
    EXPECT_FALSE(tgt.ring->isInvalid(0));
  }

  // 3) proj0.getLength() <= distanceTol (tiny parallel overlap).
  {
    OwnedCoverageRing adj =
        MakeRing({Coordinate(9.5, 0.2), Coordinate(20.0, 0.2),
                  Coordinate(20.0, 5.0), Coordinate(9.5, 0.2)},
                 /*interior_on_right=*/false);
    OwnedCoverageRing tgt =
        MakeRing({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                  Coordinate(10.0, -5.0), Coordinate(0.0, 0.0)},
                 /*interior_on_right=*/true);
    detector.processIntersections(adj.ring.get(), 0, tgt.ring.get(), 0);
    EXPECT_FALSE(tgt.ring->isInvalid(0));
  }

  // 4) proj0.getLength() > distanceTol, proj1.getLength() <= distanceTol.
  {
    InvalidSegmentDetector tol2(2.0);
    OwnedCoverageRing adj =
        MakeRing({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                  Coordinate(10.0, -5.0), Coordinate(0.0, 0.0)},
                 /*interior_on_right=*/true);
    OwnedCoverageRing tgt =
        MakeRing({Coordinate(2.0, 0.1), Coordinate(3.5, 5.0),
                  Coordinate(0.0, 5.0), Coordinate(2.0, 0.1)},
                 /*interior_on_right=*/true);
    tol2.processIntersections(adj.ring.get(), 0, tgt.ring.get(), 0);
    EXPECT_FALSE(tgt.ring->isInvalid(0));
  }

  // 5) Same-direction parallel segments within tolerance -> invalid.
  {
    OwnedCoverageRing adj =
        MakeRing({Coordinate(0.0, 0.5), Coordinate(10.0, 0.5),
                  Coordinate(10.0, 5.0), Coordinate(0.0, 0.5)},
                 /*interior_on_right=*/false);
    OwnedCoverageRing tgt =
        MakeRing({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                  Coordinate(10.0, -5.0), Coordinate(0.0, 0.0)},
                 /*interior_on_right=*/true);
    detector.processIntersections(adj.ring.get(), 0, tgt.ring.get(), 0);
    EXPECT_TRUE(tgt.ring->isInvalid(0));
  }

  // 6) Opposite-direction parallel segments within tolerance (reverses proj1)
  // -> invalid.
  {
    OwnedCoverageRing adj =
        MakeRing({Coordinate(10.0, 0.5), Coordinate(0.0, 0.5),
                  Coordinate(0.0, 5.0), Coordinate(10.0, 0.5)},
                 /*interior_on_right=*/true);
    OwnedCoverageRing tgt =
        MakeRing({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                  Coordinate(10.0, -5.0), Coordinate(0.0, 0.0)},
                 /*interior_on_right=*/true);
    detector.processIntersections(adj.ring.get(), 0, tgt.ring.get(), 0);
    EXPECT_TRUE(tgt.ring->isInvalid(0));
  }

  // 7) Parallel segments farther apart than distanceTol -> valid.
  {
    OwnedCoverageRing adj =
        MakeRing({Coordinate(0.0, 1.5), Coordinate(10.0, 1.5),
                  Coordinate(10.0, 5.0), Coordinate(0.0, 1.5)},
                 /*interior_on_right=*/false);
    OwnedCoverageRing tgt =
        MakeRing({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                  Coordinate(10.0, -5.0), Coordinate(0.0, 0.0)},
                 /*interior_on_right=*/true);
    detector.processIntersections(adj.ring.get(), 0, tgt.ring.get(), 0);
    EXPECT_FALSE(tgt.ring->isInvalid(0));
  }

  // 8) Diverging wedge: one projected endpoint within tol, other outside tol ->
  // valid.
  {
    OwnedCoverageRing adj =
        MakeRing({Coordinate(0.0, 0.2), Coordinate(10.0, 3.0),
                  Coordinate(0.0, 5.0), Coordinate(0.0, 0.2)},
                 /*interior_on_right=*/false);
    OwnedCoverageRing tgt =
        MakeRing({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                  Coordinate(10.0, -5.0), Coordinate(0.0, 0.0)},
                 /*interior_on_right=*/true);
    detector.processIntersections(adj.ring.get(), 0, tgt.ring.get(), 0);
    EXPECT_FALSE(tgt.ring->isInvalid(0));
  }
}

TEST(InvalidSegmentDetectorTest, MutualIntersectorWithLinearRingCoverage) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Polygon with a hole and an overlapping adjacent polygon.
  auto target_poly = reader.read<Polygon>(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 8, 8 8, 8 2, 2 2))");
  auto adj_valid =
      reader.read<Polygon>("POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))");
  auto adj_narrow_gap =
      reader.read<Polygon>("POLYGON ((0 -0.2, 10 -0.2, 10 -5, 0 -5, 0 -0.2))");

  CoverageRing target_shell(target_poly->getExteriorRing(), /*isShell=*/true);
  CoverageRing target_hole(target_poly->getInteriorRingN(0), /*isShell=*/false);
  CoverageRing adj_shell_valid(adj_valid->getExteriorRing(), /*isShell=*/true);
  CoverageRing adj_shell_gap(adj_narrow_gap->getExteriorRing(),
                             /*isShell=*/true);

  std::vector<const SegmentString*> base_rings = {&target_shell, &target_hole};
  std::vector<const SegmentString*> adj_rings = {&adj_shell_valid};

  // Without gap polygon, shared edge (10,0)-(10,10) and hole are all valid.
  InvalidSegmentDetector strict_detector(0.0);
  MCIndexSegmentSetMutualIntersector valid_intersector(0.0);
  valid_intersector.setBaseSegments(&base_rings);
  valid_intersector.setSegmentIntersector(&strict_detector);
  valid_intersector.process(&adj_rings);
  EXPECT_FALSE(target_shell.hasInvalid());
  EXPECT_FALSE(target_hole.hasInvalid());

  // With gap tolerance 0.5 and adjacent polygon 0.2 away from bottom edge
  // (0,0)-(10,0), segment 0 of target_shell is marked invalid.
  InvalidSegmentDetector gap_detector(0.5);
  MCIndexSegmentSetMutualIntersector gap_intersector(0.5);
  gap_intersector.setBaseSegments(&base_rings);
  gap_intersector.setSegmentIntersector(&gap_detector);
  std::vector<const SegmentString*> gap_adj_rings = {&adj_shell_gap};
  gap_intersector.process(&gap_adj_rings);
  EXPECT_TRUE(target_shell.isInvalid(0));
  EXPECT_FALSE(target_shell.isInvalid(1));
  EXPECT_FALSE(target_shell.isInvalid(2));
  EXPECT_FALSE(target_shell.isInvalid(3));
  EXPECT_FALSE(target_hole.hasInvalid());
}

}  // namespace
}  // namespace geos_google
