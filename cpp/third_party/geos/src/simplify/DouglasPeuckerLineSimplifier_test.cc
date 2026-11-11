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

#include "third_party/geos/include/geos/simplify/DouglasPeuckerLineSimplifier.h"

#include <initializer_list>
#include <limits>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::simplify::DouglasPeuckerLineSimplifier;

CoordinateSequence MakeXYSeq(std::initializer_list<CoordinateXY> coords) {
  CoordinateSequence seq(0, false, false);
  seq.reserve(coords.size());
  for (const CoordinateXY& coord : coords) {
    seq.add(coord);
  }
  return seq;
}

CoordinateSequence MakeXYZSeq(std::initializer_list<Coordinate> coords) {
  CoordinateSequence seq(0, true, false);
  seq.reserve(coords.size());
  for (const Coordinate& coord : coords) {
    seq.add(coord);
  }
  return seq;
}

TEST(DouglasPeuckerLineSimplifierTest, EmptySequenceReturnsEmpty) {
  CoordinateSequence empty(0, false, false);

  auto res_preserve = DouglasPeuckerLineSimplifier::simplify(empty, 1.0, true);
  ASSERT_NE(res_preserve, nullptr);
  EXPECT_TRUE(res_preserve->isEmpty());
  EXPECT_EQ(res_preserve->size(), 0u);

  auto res_no_preserve =
      DouglasPeuckerLineSimplifier::simplify(empty, 1.0, false);
  ASSERT_NE(res_no_preserve, nullptr);
  EXPECT_TRUE(res_no_preserve->isEmpty());
}

TEST(DouglasPeuckerLineSimplifierTest, SingleAndTwoPointSequencesUnchanged) {
  CoordinateSequence single = MakeXYSeq({{3.0, 4.0}});
  auto res_single = DouglasPeuckerLineSimplifier::simplify(single, 10.0, true);
  ASSERT_EQ(res_single->size(), 1u);
  EXPECT_EQ(res_single->getAt<CoordinateXY>(0), CoordinateXY(3.0, 4.0));

  CoordinateSequence pair = MakeXYSeq({{0.0, 0.0}, {10.0, 0.0}});
  auto res_pair = DouglasPeuckerLineSimplifier::simplify(pair, 100.0, false);
  ASSERT_EQ(res_pair->size(), 2u);
  EXPECT_EQ(res_pair->getAt<CoordinateXY>(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(res_pair->getAt<CoordinateXY>(1), CoordinateXY(10.0, 0.0));
}

TEST(DouglasPeuckerLineSimplifierTest, NaNToleranceThrows) {
  CoordinateSequence seq = MakeXYSeq({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  const double nan = std::numeric_limits<double>::quiet_NaN();

  EXPECT_THROW(DouglasPeuckerLineSimplifier::simplify(seq, nan, true),
               geos::util::IllegalArgumentException);

  DouglasPeuckerLineSimplifier simp(seq);
  EXPECT_THROW(simp.setDistanceTolerance(nan),
               geos::util::IllegalArgumentException);
}

TEST(DouglasPeuckerLineSimplifierTest, NegativeTolerancePreservesAllVertices) {
  // Collinear interior point has distance 0.0 > -1.0, so nothing is dropped.
  CoordinateSequence seq = MakeXYSeq({{0.0, 0.0}, {5.0, 0.0}, {10.0, 0.0}});
  auto res = DouglasPeuckerLineSimplifier::simplify(seq, -1.0, true);
  EXPECT_TRUE(CoordinateSequence::equals(&seq, res.get()));
}

TEST(DouglasPeuckerLineSimplifierTest, ZeroToleranceDropsOnlyCollinearPoints) {
  CoordinateSequence seq = MakeXYSeq({
      {0.0, 0.0},
      {2.0, 0.0},
      {5.0, 0.0},
      {5.0, 3.0},
      {5.0, 6.0},
      {10.0, 6.0},
  });
  CoordinateSequence expected =
      MakeXYSeq({{0.0, 0.0}, {5.0, 0.0}, {5.0, 6.0}, {10.0, 6.0}});

  auto res = DouglasPeuckerLineSimplifier::simplify(seq, 0.0, true);
  EXPECT_TRUE(CoordinateSequence::equals(&expected, res.get()));
}

TEST(DouglasPeuckerLineSimplifierTest, ToleranceBoundaryIsInclusive) {
  CoordinateSequence seq = MakeXYSeq({{0.0, 0.0}, {5.0, 1.0}, {10.0, 0.0}});

  auto exact = DouglasPeuckerLineSimplifier::simplify(seq, 1.0, true);
  ASSERT_EQ(exact->size(), 2u);
  EXPECT_EQ(exact->getAt<CoordinateXY>(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(exact->getAt<CoordinateXY>(1), CoordinateXY(10.0, 0.0));

  auto tighter = DouglasPeuckerLineSimplifier::simplify(seq, 0.9999, true);
  EXPECT_TRUE(CoordinateSequence::equals(&seq, tighter.get()));
}

TEST(DouglasPeuckerLineSimplifierTest, RecursiveSplittingKeepsMultiplePeaks) {
  CoordinateSequence seq = MakeXYSeq({
      {0.0, 0.0},
      {2.0, 1.5},
      {4.0, 0.2},
      {6.0, 5.0},
      {8.0, -2.0},
      {10.0, 0.0},
  });
  // Peak at (6, 5) splits section into (0..3) and (3..5).
  // Within (0..3), (4, 0.2) exceeds 1.5 from (0,0)-(6,5), then (2, 1.5) is
  // ~1.398 <= 1.5 from (0,0)-(4,0.2) and gets dropped.
  // Within (3..5), (8, -2) exceeds 1.5 and stays.
  CoordinateSequence expected = MakeXYSeq({
      {0.0, 0.0},
      {4.0, 0.2},
      {6.0, 5.0},
      {8.0, -2.0},
      {10.0, 0.0},
  });

  auto res = DouglasPeuckerLineSimplifier::simplify(seq, 1.5, true);
  EXPECT_TRUE(CoordinateSequence::equals(&expected, res.get()));
}

TEST(DouglasPeuckerLineSimplifierTest, InstanceSettersAndRepeatedSimplify) {
  CoordinateSequence seq = MakeXYSeq({
      {0.0, 0.0},
      {2.0, 1.5},
      {4.0, 0.2},
      {6.0, 5.0},
      {8.0, -2.0},
      {10.0, 0.0},
  });

  DouglasPeuckerLineSimplifier simp(seq);
  simp.setPreserveClosedEndpoint(true);
  simp.setDistanceTolerance(1.5);
  auto res1 = simp.simplify();
  EXPECT_EQ(res1->size(), 5u);

  simp.setDistanceTolerance(10.0);
  auto res2 = simp.simplify();
  ASSERT_EQ(res2->size(), 2u);
  EXPECT_EQ(res2->getAt<CoordinateXY>(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(res2->getAt<CoordinateXY>(1), CoordinateXY(10.0, 0.0));

  CoordinateSequence ring = MakeXYSeq({
      {5.0, 0.0},
      {10.0, 0.0},
      {10.0, 10.0},
      {0.0, 10.0},
      {0.0, 0.0},
      {5.0, 0.0},
  });
  DouglasPeuckerLineSimplifier ring_simp(ring);
  ring_simp.setDistanceTolerance(0.0);
  ring_simp.setPreserveClosedEndpoint(true);
  EXPECT_EQ(ring_simp.simplify()->size(), 6u);
  ring_simp.setPreserveClosedEndpoint(false);
  EXPECT_EQ(ring_simp.simplify()->size(), 5u);
}

TEST(DouglasPeuckerLineSimplifierTest,
     RingEndpointSimplifiedOnlyWhenPreserveClosedEndpointIsFalse) {
  CoordinateSequence ring = MakeXYSeq({
      {5.0, 0.0},
      {8.0, 0.0},
      {10.0, 0.0},
      {10.0, 5.0},
      {10.0, 10.0},
      {0.0, 10.0},
      {0.0, 0.0},
      {2.0, 0.0},
      {5.0, 0.0},
  });

  auto simplified_ring =
      DouglasPeuckerLineSimplifier::simplify(ring, 0.0, false);
  CoordinateSequence expected_simplified = MakeXYSeq({
      {10.0, 0.0},
      {10.0, 10.0},
      {0.0, 10.0},
      {0.0, 0.0},
      {10.0, 0.0},
  });
  EXPECT_TRUE(simplified_ring->isRing());
  EXPECT_TRUE(
      CoordinateSequence::equals(&expected_simplified, simplified_ring.get()));

  auto preserved_ring = DouglasPeuckerLineSimplifier::simplify(ring, 0.0, true);
  CoordinateSequence expected_preserved = MakeXYSeq({
      {5.0, 0.0},
      {10.0, 0.0},
      {10.0, 10.0},
      {0.0, 10.0},
      {0.0, 0.0},
      {5.0, 0.0},
  });
  EXPECT_TRUE(preserved_ring->isRing());
  EXPECT_TRUE(
      CoordinateSequence::equals(&expected_preserved, preserved_ring.get()));

  // Nearly collinear closed endpoint within or equal to tolerance is removed.
  CoordinateSequence slightly_bent_ring = MakeXYSeq({
      {4.0, 0.5},
      {8.0, 0.0},
      {8.0, 8.0},
      {0.0, 8.0},
      {0.0, 0.0},
      {4.0, 0.5},
  });
  CoordinateSequence expected_bent_simplified = MakeXYSeq({
      {8.0, 0.0},
      {8.0, 8.0},
      {0.0, 8.0},
      {0.0, 0.0},
      {8.0, 0.0},
  });
  auto simplified_bent =
      DouglasPeuckerLineSimplifier::simplify(slightly_bent_ring, 0.5, false);
  EXPECT_TRUE(CoordinateSequence::equals(&expected_bent_simplified,
                                         simplified_bent.get()));

  auto kept_bent =
      DouglasPeuckerLineSimplifier::simplify(slightly_bent_ring, 0.4999, false);
  EXPECT_TRUE(CoordinateSequence::equals(&slightly_bent_ring, kept_bent.get()));
}

TEST(DouglasPeuckerLineSimplifierTest,
     RingEndpointKeptWhenCornerOrNonRingOrCollapsed) {
  // Corner endpoint exceeds tolerance of segment connecting neighbors.
  CoordinateSequence square = MakeXYSeq({
      {0.0, 0.0},
      {10.0, 0.0},
      {10.0, 10.0},
      {0.0, 10.0},
      {0.0, 0.0},
  });
  auto res_square = DouglasPeuckerLineSimplifier::simplify(square, 1.0, false);
  EXPECT_TRUE(CoordinateSequence::equals(&square, res_square.get()));

  // Non-closed sequence with > 3 kept points and collinear start does not trim
  // its start point even with preserveClosedEndpoint = false.
  CoordinateSequence open_almost_ring = MakeXYSeq({
      {5.0, 0.0},
      {10.0, 0.0},
      {10.0, 10.0},
      {0.0, 10.0},
      {0.0, 0.0},
      {5.0, 0.5},
  });
  auto res_open =
      DouglasPeuckerLineSimplifier::simplify(open_almost_ring, 0.1, false);
  EXPECT_TRUE(CoordinateSequence::equals(&open_almost_ring, res_open.get()));

  // Closed 3-point sequence has size < 4 so isRing() is false.
  CoordinateSequence three_closed_kept =
      MakeXYSeq({{0.0, 0.0}, {5.0, 0.1}, {0.0, 0.0}});
  auto res_three_kept =
      DouglasPeuckerLineSimplifier::simplify(three_closed_kept, 1.0, false);
  EXPECT_TRUE(
      CoordinateSequence::equals(&three_closed_kept, res_three_kept.get()));
  EXPECT_FALSE(res_three_kept->isRing());

  CoordinateSequence three_closed_collapsed =
      MakeXYSeq({{0.0, 0.0}, {0.1, 0.0}, {0.0, 0.0}});
  auto res_three_collapsed = DouglasPeuckerLineSimplifier::simplify(
      three_closed_collapsed, 1.0, false);
  ASSERT_EQ(res_three_collapsed->size(), 2u);
  EXPECT_FALSE(res_three_collapsed->isRing());

  // Ring collapsing to 2 points stops before ring-endpoint removal.
  CoordinateSequence flat_ring =
      MakeXYSeq({{0.0, 0.0}, {5.0, 0.0}, {10.0, 0.0}, {0.0, 0.0}});
  auto res_flat =
      DouglasPeuckerLineSimplifier::simplify(flat_ring, 10.0, false);
  ASSERT_EQ(res_flat->size(), 2u);
  EXPECT_EQ(res_flat->getAt<CoordinateXY>(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(res_flat->getAt<CoordinateXY>(1), CoordinateXY(0.0, 0.0));

  // Ring collapsing to 3 points (<= LinearRing::MINIMUM_VALID_SIZE) stops too.
  CoordinateSequence triangle_collapse =
      MakeXYSeq({{0.0, 0.0}, {5.0, 5.0}, {10.0, 0.0}, {0.0, 0.0}});
  auto res_tri =
      DouglasPeuckerLineSimplifier::simplify(triangle_collapse, 6.0, false);
  ASSERT_EQ(res_tri->size(), 3u);
  EXPECT_EQ(res_tri->getAt<CoordinateXY>(1), CoordinateXY(10.0, 0.0));

  // Ring with 4 kept points (> LinearRing::MINIMUM_VALID_SIZE) trims closed
  // endpoint down to 3 points when preserveClosedEndpoint is false.
  CoordinateSequence collinear_four_ring =
      MakeXYSeq({{5.0, 0.0}, {10.0, 0.0}, {0.0, 0.0}, {5.0, 0.0}});
  auto res_four_trimmed =
      DouglasPeuckerLineSimplifier::simplify(collinear_four_ring, 0.5, false);
  CoordinateSequence expected_three =
      MakeXYSeq({{10.0, 0.0}, {0.0, 0.0}, {10.0, 0.0}});
  EXPECT_TRUE(
      CoordinateSequence::equals(&expected_three, res_four_trimmed.get()));
  auto res_four_kept =
      DouglasPeuckerLineSimplifier::simplify(collinear_four_ring, 0.5, true);
  EXPECT_TRUE(
      CoordinateSequence::equals(&collinear_four_ring, res_four_kept.get()));
}

TEST(DouglasPeuckerLineSimplifierTest, InfiniteToleranceCollapsesToEndpoints) {
  const double inf = std::numeric_limits<double>::infinity();

  CoordinateSequence polyline =
      MakeXYSeq({{0.0, 0.0}, {3.0, 4.0}, {7.0, -2.0}, {10.0, 0.0}});
  auto res_poly = DouglasPeuckerLineSimplifier::simplify(polyline, inf, true);
  ASSERT_EQ(res_poly->size(), 2u);
  EXPECT_EQ(res_poly->getAt<CoordinateXY>(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(res_poly->getAt<CoordinateXY>(1), CoordinateXY(10.0, 0.0));

  CoordinateSequence ring = MakeXYSeq({
      {0.0, 0.0},
      {10.0, 0.0},
      {10.0, 10.0},
      {0.0, 10.0},
      {0.0, 0.0},
  });
  auto res_ring = DouglasPeuckerLineSimplifier::simplify(ring, inf, false);
  ASSERT_EQ(res_ring->size(), 2u);
  EXPECT_EQ(res_ring->getAt<CoordinateXY>(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(res_ring->getAt<CoordinateXY>(1), CoordinateXY(0.0, 0.0));
}

TEST(DouglasPeuckerLineSimplifierTest, PreservesZCoordinatesOnKeptPoints) {
  CoordinateSequence seq3d = MakeXYZSeq({
      Coordinate(0.0, 0.0, 100.0),
      Coordinate(5.0, 0.1, 200.0),
      Coordinate(10.0, 0.0, 300.0),
  });

  auto res = DouglasPeuckerLineSimplifier::simplify(seq3d, 0.5, true);
  ASSERT_EQ(res->size(), 2u);
  EXPECT_TRUE(res->hasZ());
  EXPECT_TRUE(res->getAt(0).equals3D(Coordinate(0.0, 0.0, 100.0)));
  EXPECT_TRUE(res->getAt(1).equals3D(Coordinate(10.0, 0.0, 300.0)));

  CoordinateSequence ring3d = MakeXYZSeq({
      Coordinate(5.0, 0.0, 10.0),
      Coordinate(10.0, 0.0, 20.0),
      Coordinate(10.0, 10.0, 30.0),
      Coordinate(0.0, 10.0, 40.0),
      Coordinate(0.0, 0.0, 50.0),
      Coordinate(5.0, 0.0, 10.0),
  });
  auto res_ring = DouglasPeuckerLineSimplifier::simplify(ring3d, 0.0, false);
  ASSERT_EQ(res_ring->size(), 5u);
  EXPECT_TRUE(res_ring->hasZ());
  EXPECT_TRUE(res_ring->getAt(0).equals3D(Coordinate(10.0, 0.0, 20.0)));
  EXPECT_TRUE(res_ring->getAt(4).equals3D(Coordinate(10.0, 0.0, 20.0)));
}

}  // namespace
}  // namespace geos_google
