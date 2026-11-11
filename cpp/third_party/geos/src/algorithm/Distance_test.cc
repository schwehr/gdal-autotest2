// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/algorithm/Distance.h"

#include <cmath>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::algorithm::Distance;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;

TEST(DistanceTest, PointToSegmentDegenerateZeroLength) {
  const CoordinateXY a(3.0, 4.0);
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(a, a, a), 0.0);
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(0.0, 0.0), a, a), 5.0);
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(6.0, 8.0), a, a), 5.0);
}

TEST(DistanceTest, PointToSegmentProjectionsBeforeInteriorAndAfter) {
  const CoordinateXY a(0.0, 0.0);
  const CoordinateXY b(10.0, 0.0);

  // Behind A (r < 0).
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(-3.0, 4.0), a, b),
                   5.0);
  // At A (r == 0).
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(0.0, 0.0), a, b), 0.0);
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(0.0, 5.0), a, b), 5.0);

  // Interior to AB (0 < r < 1).
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(4.0, 0.0), a, b), 0.0);
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(4.0, 7.0), a, b), 7.0);
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(4.0, -7.0), a, b),
                   7.0);

  // At B (r == 1).
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(10.0, 0.0), a, b),
                   0.0);
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(10.0, -6.0), a, b),
                   6.0);

  // Beyond B (r > 1).
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(13.0, 4.0), a, b),
                   5.0);
}

TEST(DistanceTest, PointToSegmentDiagonalAndSymmetry) {
  const CoordinateXY a(0.0, 0.0);
  const CoordinateXY b(3.0, 4.0);

  // (0, 5) projects into interior of (0,0)-(3,4) at r = 0.8 with distance 3.
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(0.0, 5.0), a, b), 3.0);
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(0.0, 5.0), b, a), 3.0);

  // Midpoint lies on segment.
  EXPECT_DOUBLE_EQ(Distance::pointToSegment(CoordinateXY(1.5, 2.0), a, b), 0.0);
}

TEST(DistanceTest, PointToLinePerpendicular) {
  const CoordinateXY a(0.0, 0.0);
  const CoordinateXY b(3.0, 4.0);

  // On the segment and on its infinite extensions.
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicular(CoordinateXY(1.5, 2.0), a, b), 0.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicular(CoordinateXY(-3.0, -4.0), a, b), 0.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicular(CoordinateXY(6.0, 8.0), a, b), 0.0);

  // Off the line on both sides, including outside segment projection range.
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicular(CoordinateXY(0.0, 5.0), a, b), 3.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicular(CoordinateXY(0.0, -5.0), a, b), 3.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicular(CoordinateXY(3.0, -1.0), a, b), 3.0);

  // Horizontal and vertical lines.
  EXPECT_DOUBLE_EQ(Distance::pointToLinePerpendicular(CoordinateXY(-10.0, 6.0),
                                                      CoordinateXY(0.0, 2.0),
                                                      CoordinateXY(5.0, 2.0)),
                   4.0);
  EXPECT_DOUBLE_EQ(Distance::pointToLinePerpendicular(CoordinateXY(-3.0, 20.0),
                                                      CoordinateXY(2.0, 0.0),
                                                      CoordinateXY(2.0, 5.0)),
                   5.0);
}

TEST(DistanceTest, PointToLinePerpendicularSigned) {
  const CoordinateXY a(0.0, 0.0);
  const CoordinateXY b(10.0, 0.0);

  // Left of directed segment A->B is negative; right is positive; on line is 0.
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicularSigned(CoordinateXY(5.0, 3.0), a, b),
      -3.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicularSigned(CoordinateXY(5.0, -3.0), a, b),
      3.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicularSigned(CoordinateXY(5.0, 0.0), a, b),
      0.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicularSigned(CoordinateXY(-5.0, 4.0), a, b),
      -4.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicularSigned(CoordinateXY(15.0, -4.0), a, b),
      4.0);

  // Reversing segment direction flips sign.
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicularSigned(CoordinateXY(5.0, 3.0), b, a),
      3.0);

  // Vertical and diagonal lines match unsigned perpendicular distance in abs.
  const CoordinateXY va(2.0, 0.0);
  const CoordinateXY vb(2.0, 5.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicularSigned(CoordinateXY(0.0, 3.0), va, vb),
      -2.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicularSigned(CoordinateXY(5.0, 3.0), va, vb),
      3.0);

  const CoordinateXY da(0.0, 0.0);
  const CoordinateXY db(3.0, 4.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicularSigned(CoordinateXY(0.0, 5.0), da, db),
      -3.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToLinePerpendicularSigned(CoordinateXY(3.0, -1.0), da, db),
      3.0);
  EXPECT_DOUBLE_EQ(
      std::fabs(Distance::pointToLinePerpendicularSigned(CoordinateXY(0.0, 5.0),
                                                         da, db)),
      Distance::pointToLinePerpendicular(CoordinateXY(0.0, 5.0), da, db));
}

TEST(DistanceTest, SegmentToSegmentDegenerateEndpoints) {
  // First segment degenerate (A == B).
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(2.0, 3.0), CoordinateXY(2.0, 3.0),
                       CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0)),
                   3.0);
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(2.0, 0.0), CoordinateXY(2.0, 0.0),
                       CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0)),
                   0.0);

  // Second segment degenerate (C == D).
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0),
                       CoordinateXY(2.0, -5.0), CoordinateXY(2.0, -5.0)),
                   5.0);

  // Both segments degenerate (point-to-point).
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 0.0),
                       CoordinateXY(3.0, 4.0), CoordinateXY(3.0, 4.0)),
                   5.0);
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(1.0, 2.0), CoordinateXY(1.0, 2.0),
                       CoordinateXY(1.0, 2.0), CoordinateXY(1.0, 2.0)),
                   0.0);
}

TEST(DistanceTest, SegmentToSegmentIntersectingAndTouching) {
  // Proper crossing intersection.
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 4.0),
                       CoordinateXY(0.0, 4.0), CoordinateXY(4.0, 0.0)),
                   0.0);

  // T-junction at endpoint.
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0),
                       CoordinateXY(2.0, 0.0), CoordinateXY(2.0, 5.0)),
                   0.0);

  // V-junction sharing an endpoint.
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 2.0),
                       CoordinateXY(2.0, 2.0), CoordinateXY(4.0, 0.0)),
                   0.0);
}

TEST(DistanceTest, SegmentToSegmentParallelAndCollinear) {
  // Parallel non-collinear segments with overlapping bounding boxes.
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
                       CoordinateXY(2.0, 3.0), CoordinateXY(8.0, 3.0)),
                   3.0);

  // Slanted parallel segments.
  EXPECT_NEAR(Distance::segmentToSegment(
                  CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 4.0),
                  CoordinateXY(1.0, 0.0), CoordinateXY(5.0, 4.0)),
              1.0 / std::sqrt(2.0), 1e-12);

  // Collinear overlapping segments.
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
                       CoordinateXY(3.0, 0.0), CoordinateXY(7.0, 0.0)),
                   0.0);
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 4.0),
                       CoordinateXY(2.0, 2.0), CoordinateXY(6.0, 6.0)),
                   0.0);

  // Collinear endpoint-touching segments.
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 5.0),
                       CoordinateXY(5.0, 5.0), CoordinateXY(10.0, 10.0)),
                   0.0);
}

TEST(DistanceTest, SegmentToSegmentDisjointEnvelopesAndOverlappingNonCrossing) {
  // Disjoint envelopes.
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0),
                       CoordinateXY(4.0, 5.0), CoordinateXY(6.0, 7.0)),
                   5.0);

  // Overlapping envelopes where infinite-line intersection has r < 0.
  EXPECT_DOUBLE_EQ(
      Distance::segmentToSegment(
          CoordinateXY(2.0, 0.0), CoordinateXY(10.0, 0.0),
          CoordinateXY(0.0, -1.0), CoordinateXY(3.0, 5.0)),
      Distance::pointToSegment(CoordinateXY(2.0, 0.0), CoordinateXY(0.0, -1.0),
                               CoordinateXY(3.0, 5.0)));

  // Overlapping envelopes where infinite-line intersection has r > 1.
  EXPECT_DOUBLE_EQ(
      Distance::segmentToSegment(CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0),
                                 CoordinateXY(3.0, -1.0),
                                 CoordinateXY(8.0, 1.0)),
      Distance::pointToSegment(CoordinateXY(4.0, 0.0), CoordinateXY(3.0, -1.0),
                               CoordinateXY(8.0, 1.0)));

  // Overlapping envelopes with 0 <= r <= 1 and s < 0.
  EXPECT_NEAR(Distance::segmentToSegment(
                  CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 10.0),
                  CoordinateXY(5.0, 1.0), CoordinateXY(6.0, 0.0)),
              2.0 * std::sqrt(2.0), 1e-12);

  // Overlapping envelopes with 0 <= r <= 1 and s > 1 (C and D swapped).
  EXPECT_NEAR(Distance::segmentToSegment(
                  CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 10.0),
                  CoordinateXY(6.0, 0.0), CoordinateXY(5.0, 1.0)),
              2.0 * std::sqrt(2.0), 1e-12);

  // Minimum realized at each of A, B, C, and D in turn.
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(2.0, 1.0), CoordinateXY(10.0, 10.0),
                       CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0)),
                   1.0);
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(10.0, 10.0), CoordinateXY(2.0, 1.0),
                       CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0)),
                   1.0);
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0),
                       CoordinateXY(2.0, 1.0), CoordinateXY(10.0, 10.0)),
                   1.0);
  EXPECT_DOUBLE_EQ(Distance::segmentToSegment(
                       CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0),
                       CoordinateXY(10.0, 10.0), CoordinateXY(2.0, 1.0)),
                   1.0);
}

TEST(DistanceTest, PointToSegmentStringEmptyThrowsIllegalArgumentException) {
  CoordinateSequence empty;
  EXPECT_THROW(
      { Distance::pointToSegmentString(CoordinateXY(0.0, 0.0), &empty); },
      geos::util::IllegalArgumentException);
}

TEST(DistanceTest, PointToSegmentStringSingleVertex) {
  CoordinateSequence single({Coordinate(3.0, 4.0)});
  EXPECT_DOUBLE_EQ(
      Distance::pointToSegmentString(CoordinateXY(3.0, 4.0), &single), 0.0);
  EXPECT_DOUBLE_EQ(
      Distance::pointToSegmentString(CoordinateXY(0.0, 0.0), &single), 5.0);
}

TEST(DistanceTest, PointToSegmentStringMultiSegmentPolyline) {
  CoordinateSequence poly({
      Coordinate(0.0, 0.0),
      Coordinate(10.0, 0.0),
      Coordinate(10.0, 10.0),
      Coordinate(0.0, 10.0),
  });

  // Closest to initial vertex (0,0): later segments never beat minDistance.
  EXPECT_DOUBLE_EQ(
      Distance::pointToSegmentString(CoordinateXY(-3.0, -4.0), &poly), 5.0);

  // Closest to first segment interior.
  EXPECT_DOUBLE_EQ(
      Distance::pointToSegmentString(CoordinateXY(5.0, 2.0), &poly), 2.0);

  // Closest to middle segment (updates minDistance during loop).
  EXPECT_DOUBLE_EQ(
      Distance::pointToSegmentString(CoordinateXY(13.0, 5.0), &poly), 3.0);

  // Closest to final segment (updates minDistance on last iteration).
  EXPECT_DOUBLE_EQ(
      Distance::pointToSegmentString(CoordinateXY(4.0, 12.0), &poly), 2.0);

  // Point lying directly on a segment.
  EXPECT_DOUBLE_EQ(
      Distance::pointToSegmentString(CoordinateXY(10.0, 4.5), &poly), 0.0);

  // Polyline with repeated consecutive vertices.
  CoordinateSequence with_repeats({
      Coordinate(0.0, 0.0),
      Coordinate(0.0, 0.0),
      Coordinate(4.0, 0.0),
      Coordinate(4.0, 0.0),
      Coordinate(4.0, 3.0),
  });
  EXPECT_DOUBLE_EQ(
      Distance::pointToSegmentString(CoordinateXY(7.0, 1.5), &with_repeats),
      3.0);
}

}  // namespace
}  // namespace geos_google
