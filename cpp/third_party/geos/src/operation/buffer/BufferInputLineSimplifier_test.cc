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

// Unit tests for geos::operation::buffer::BufferInputLineSimplifier.
// See also:
// https://github.com/libgeos/geos/blob/main/src/operation/buffer/BufferInputLineSimplifier.cpp

#include "third_party/geos/include/geos/operation/buffer/BufferInputLineSimplifier.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::operation::buffer::BufferInputLineSimplifier;

TEST(BufferInputLineSimplifierTest, EmptyAndShortSequencesUnchanged) {
  CoordinateSequence empty = CoordinateSequence::XY(0);
  auto empty_res = BufferInputLineSimplifier::simplify(empty, 1.0);
  ASSERT_NE(empty_res, nullptr);
  EXPECT_TRUE(empty_res->isEmpty());
  EXPECT_EQ(empty_res->size(), 0U);

  CoordinateSequence one{CoordinateXY(1.0, 2.0)};
  auto one_res = BufferInputLineSimplifier::simplify(one, 1.0);
  ASSERT_EQ(one_res->size(), 1U);
  EXPECT_DOUBLE_EQ(one_res->getAt<CoordinateXY>(0).x, 1.0);
  EXPECT_DOUBLE_EQ(one_res->getAt<CoordinateXY>(0).y, 2.0);

  CoordinateSequence two{CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0)};
  auto two_res = BufferInputLineSimplifier::simplify(two, 10.0);
  EXPECT_TRUE(two_res->equalsIdentical(two));

  // A 3-point sequence never enters the simplification loop because index
  // starts at 1, so lastIndex >= 3 stops immediately.
  CoordinateSequence three{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(0.5, -0.05),
      CoordinateXY(1.0, 0.0),
  };
  auto three_res = BufferInputLineSimplifier::simplify(three, 10.0);
  EXPECT_TRUE(three_res->equalsIdentical(three));
}

TEST(BufferInputLineSimplifierTest,
     DeletesShallowCounterclockwiseConcavityWithPositiveTolerance) {
  // Endpoints (0, 0) and index 1 are never deleted; triple (1, 2, 3) makes a
  // CCW turn with p1.distance(p2) < 2.0 and pointToSegment(p2, p1, p3) = 0.2.
  CoordinateSequence input{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(2.0, -0.2),
      CoordinateXY(3.0, 0.0),
  };
  auto result = BufferInputLineSimplifier::simplify(input, 2.0);

  CoordinateSequence expected{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(3.0, 0.0),
  };
  EXPECT_TRUE(result->equalsIdentical(expected));
}

TEST(BufferInputLineSimplifierTest,
     KeepsClockwiseTurnWithPositiveToleranceAndDeletesWithNegativeTolerance) {
  // Triple (1, 2, 3) turns CW; positive tolerance keeps it, negative
  // tolerance switches orientation to CLOCKWISE and deletes it.
  CoordinateSequence cw_turn{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(2.0, 0.2),
      CoordinateXY(3.0, 0.0),
  };

  auto kept = BufferInputLineSimplifier::simplify(cw_turn, 2.0);
  EXPECT_TRUE(kept->equalsIdentical(cw_turn));

  BufferInputLineSimplifier simplifier(cw_turn);
  auto deleted = simplifier.simplify(-2.0);
  CoordinateSequence expected{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(3.0, 0.0),
  };
  EXPECT_TRUE(deleted->equalsIdentical(expected));

  // CCW turn is preserved when tolerance is negative.
  CoordinateSequence ccw_turn{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(2.0, -0.2),
      CoordinateXY(3.0, 0.0),
  };
  auto ccw_kept = BufferInputLineSimplifier::simplify(ccw_turn, -2.0);
  EXPECT_TRUE(ccw_kept->equalsIdentical(ccw_turn));
}

TEST(BufferInputLineSimplifierTest, ZeroAndBoundaryTolerancesPreserveVertices) {
  // At triple (1, 2, 3): pointToSegment(p2, p1, p3) = 0.5 and
  // p1.distance(p2) = sqrt(0.34) ~= 0.5831.
  CoordinateSequence input{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(0.5, 0.0),
      CoordinateXY(0.8, -0.5),
      CoordinateXY(1.1, 0.0),
  };

  // Zero tolerance hits distanceTol <= 0.0 in isShallow.
  auto zero_res = BufferInputLineSimplifier::simplify(input, 0.0);
  EXPECT_TRUE(zero_res->equalsIdentical(input));

  // Strict inequality dist < distanceTol keeps exact boundary point (0.5).
  auto equal_res = BufferInputLineSimplifier::simplify(input, 0.5);
  EXPECT_TRUE(equal_res->equalsIdentical(input));

  // Deep concavity exceeds tolerance.
  auto small_res = BufferInputLineSimplifier::simplify(input, 0.25);
  EXPECT_TRUE(small_res->equalsIdentical(input));

  // Above p1.distance(p2) and pointToSegment allows deletion.
  auto above_res = BufferInputLineSimplifier::simplify(input, 0.6);
  CoordinateSequence expected{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(0.5, 0.0),
      CoordinateXY(1.1, 0.0),
  };
  EXPECT_TRUE(above_res->equalsIdentical(expected));
}

TEST(BufferInputLineSimplifierTest,
     RejectsLongShallowSegmentViaIsShallowSampledAndKeepsCollinear) {
  // Triple (1, 2, 3) is CCW and pointToSegment(p2, p1, p3) = 0.1 < 1.0,
  // yet p1.distance(p2) > 10.0 so isShallowSampled returns false at i == i0.
  CoordinateSequence long_shallow{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(11.0, -0.1),
      CoordinateXY(21.0, 0.0),
  };
  auto long_res = BufferInputLineSimplifier::simplify(long_shallow, 1.0);
  EXPECT_TRUE(long_res->equalsIdentical(long_shallow));

  // Collinear middle vertices are not strictly concave and remain untouched.
  CoordinateSequence collinear{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(1.2, 0.0),
      CoordinateXY(1.5, 0.0),
  };
  auto collinear_res = BufferInputLineSimplifier::simplify(collinear, 10.0);
  EXPECT_TRUE(collinear_res->equalsIdentical(collinear));
}

TEST(BufferInputLineSimplifierTest,
     MultiPassCascadingDeletionCoversLargeSamplingStride) {
  // Construct 40 vertices on a smooth CCW arc within diameter < 6.1 so every
  // vertex 2..38 deletes across passes, exercising findNextNonDeletedIndex
  // skipping deleted nodes and (i2 - i0) / 10 >= 1 sampling stride.
  constexpr std::size_t kCount = 40;
  CoordinateSequence arc = CoordinateSequence::XY(kCount);
  for (std::size_t i = 0; i < kCount; ++i) {
    const double x = -3.0 + (6.0 * static_cast<double>(i)) / (kCount - 1);
    const double y = 10.0 - std::sqrt(100.0 - x * x);
    arc.setAt(CoordinateXY(x, y), i);
  }

  auto result = BufferInputLineSimplifier::simplify(arc, 10.0);
  ASSERT_EQ(result->size(), 3U);
  EXPECT_DOUBLE_EQ(result->getAt<CoordinateXY>(0).x,
                   arc.getAt<CoordinateXY>(0).x);
  EXPECT_DOUBLE_EQ(result->getAt<CoordinateXY>(1).x,
                   arc.getAt<CoordinateXY>(1).x);
  EXPECT_DOUBLE_EQ(result->getAt<CoordinateXY>(2).x,
                   arc.getAt<CoordinateXY>(kCount - 1).x);
}

TEST(BufferInputLineSimplifierTest,
     CollapsesDuplicateVerticesAndPreservesZCoordinates) {
  CoordinateSequence input{
      Coordinate(0.0, 0.0, 10.0), Coordinate(0.0, 0.0, 15.0),
      Coordinate(1.0, 0.0, 20.0), Coordinate(1.5, -0.1, 25.0),
      Coordinate(2.0, 0.0, 30.0), Coordinate(2.0, 0.0, 35.0),
  };

  auto result = BufferInputLineSimplifier::simplify(input, 2.0);
  ASSERT_EQ(result->size(), 3U);
  EXPECT_DOUBLE_EQ(result->getAt(0).x, 0.0);
  EXPECT_DOUBLE_EQ(result->getAt(0).z, 10.0);
  EXPECT_DOUBLE_EQ(result->getAt(1).x, 1.0);
  EXPECT_DOUBLE_EQ(result->getAt(1).z, 20.0);
  EXPECT_DOUBLE_EQ(result->getAt(2).x, 2.0);
  EXPECT_DOUBLE_EQ(result->getAt(2).z, 30.0);
}

TEST(BufferInputLineSimplifierTest,
     AlternatingConcavitiesSimplifyOnlySelectedSideAndPreserveEndpoints) {
  // Zigzag with a shallow CCW concavity at index 2 and a shallow CW concavity
  // at index 4. Spacing to index 3 is > 1.0 so vertex 3 is never deleted in
  // pass 2. Positive tolerance deletes only index 2; negative tolerance
  // deletes only index 4.
  CoordinateSequence zigzag{
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0), CoordinateXY(1.5, -0.1),
      CoordinateXY(3.0, 0.0), CoordinateXY(3.5, 0.1), CoordinateXY(5.0, 0.0),
  };

  auto pos_res = BufferInputLineSimplifier::simplify(zigzag, 1.0);
  CoordinateSequence expected_pos{
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0), CoordinateXY(3.0, 0.0),
      CoordinateXY(3.5, 0.1), CoordinateXY(5.0, 0.0),
  };
  EXPECT_TRUE(pos_res->equalsIdentical(expected_pos));

  auto neg_res = BufferInputLineSimplifier::simplify(zigzag, -1.0);
  CoordinateSequence expected_neg{
      CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0), CoordinateXY(1.5, -0.1),
      CoordinateXY(3.0, 0.0), CoordinateXY(5.0, 0.0),
  };
  EXPECT_TRUE(neg_res->equalsIdentical(expected_neg));
}

TEST(BufferInputLineSimplifierTest, ThrowsOnNonFiniteEndpointCoordinate) {
  // CGAlgorithmsDD::orientationIndex validates the 3rd argument (p2 in
  // isConcave(p0, p1, p2)), which corresponds to index 3 on the first triple.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  CoordinateSequence bad_nan_p2{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(2.0, 0.0),
      CoordinateXY(3.0, nan),
  };
  EXPECT_THROW(BufferInputLineSimplifier::simplify(bad_nan_p2, 1.0),
               geos::util::IllegalArgumentException);

  CoordinateSequence bad_inf_p2{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(2.0, 0.0),
      CoordinateXY(inf, 0.0),
  };
  EXPECT_THROW(BufferInputLineSimplifier::simplify(bad_inf_p2, 1.0),
               geos::util::IllegalArgumentException);

  // When NaN is only at p1 (index 2) in a 4-point sequence, orientationIndex
  // falls back to DD arithmetic and yields STRAIGHT (0), so the sequence is
  // left unmodified without throwing.
  CoordinateSequence nan_mid{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(2.0, nan),
      CoordinateXY(3.0, 0.0),
  };
  auto kept = BufferInputLineSimplifier::simplify(nan_mid, 1.0);
  ASSERT_EQ(kept->size(), 4U);
  EXPECT_TRUE(std::isnan(kept->getAt<CoordinateXY>(2).y));
}

}  // namespace
}  // namespace geos_google
