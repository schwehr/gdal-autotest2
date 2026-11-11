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

// Unit tests for third_party/geos/src/algorithm/LineIntersector.cpp and
// third_party/geos/include/geos/algorithm/LineIntersector.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/LineIntersector.cpp

#include "third_party/geos/include/geos/algorithm/LineIntersector.h"

#include <cmath>
#include <cstddef>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::PrecisionModel;
using ::testing::HasSubstr;
using ::testing::Not;

constexpr std::size_t kNoIntersection =
    static_cast<std::size_t>(LineIntersector::NO_INTERSECTION);
constexpr std::size_t kPointIntersection =
    static_cast<std::size_t>(LineIntersector::POINT_INTERSECTION);
constexpr std::size_t kCollinearIntersection =
    static_cast<std::size_t>(LineIntersector::COLLINEAR_INTERSECTION);

TEST(LineIntersectorTest, DefaultStateBeforeComputation) {
  LineIntersector li;
  EXPECT_FALSE(li.hasIntersection());
  EXPECT_EQ(li.getIntersectionNum(), kNoIntersection);
  EXPECT_FALSE(li.isProper());
  EXPECT_FALSE(li.isInteriorIntersection());
  EXPECT_FALSE(li.isInteriorIntersection(0));
  EXPECT_FALSE(li.isInteriorIntersection(1));
  EXPECT_FALSE(li.isIntersection(Coordinate(0.0, 0.0)));
  EXPECT_EQ(li.getEndpoint(0, 0), nullptr);
  EXPECT_EQ(li.getEndpoint(0, 1), nullptr);
  EXPECT_EQ(li.getEndpoint(1, 0), nullptr);
  EXPECT_EQ(li.getEndpoint(1, 1), nullptr);
  EXPECT_EQ(li.toString(), "<none>_<none> <none>_<none> : ");
}

TEST(LineIntersectorTest, IsSameSignAndNonZero) {
  EXPECT_TRUE(LineIntersector::isSameSignAndNonZero(1.0, 2.5));
  EXPECT_TRUE(LineIntersector::isSameSignAndNonZero(1e-12, 1e12));
  EXPECT_TRUE(LineIntersector::isSameSignAndNonZero(-1.0, -2.5));
  EXPECT_TRUE(LineIntersector::isSameSignAndNonZero(-1e-12, -1e12));

  EXPECT_FALSE(LineIntersector::isSameSignAndNonZero(1.0, -1.0));
  EXPECT_FALSE(LineIntersector::isSameSignAndNonZero(-1.0, 1.0));
  EXPECT_FALSE(LineIntersector::isSameSignAndNonZero(0.0, 0.0));
  EXPECT_FALSE(LineIntersector::isSameSignAndNonZero(-0.0, -0.0));
  EXPECT_FALSE(LineIntersector::isSameSignAndNonZero(0.0, 5.0));
  EXPECT_FALSE(LineIntersector::isSameSignAndNonZero(5.0, 0.0));
  EXPECT_FALSE(LineIntersector::isSameSignAndNonZero(0.0, -5.0));
  EXPECT_FALSE(LineIntersector::isSameSignAndNonZero(-5.0, 0.0));
}

TEST(LineIntersectorTest, ComputeEdgeDistanceAllBranches) {
  const CoordinateXY p0(1.0, 2.0);
  const CoordinateXY horiz_p1(11.0, 5.0);  // dx = 10 > dy = 3
  const CoordinateXY vert_p1(4.0, 12.0);   // dx = 3 <= dy = 10
  const CoordinateXY diag_p1(6.0, 7.0);    // dx = 5 == dy = 5

  // p == p0 returns 0.
  EXPECT_DOUBLE_EQ(LineIntersector::computeEdgeDistance(p0, p0, horiz_p1), 0.0);
  EXPECT_DOUBLE_EQ(LineIntersector::computeEdgeDistance(p0, p0, vert_p1), 0.0);

  // p == p1 picks max(dx, dy).
  EXPECT_DOUBLE_EQ(LineIntersector::computeEdgeDistance(horiz_p1, p0, horiz_p1),
                   10.0);
  EXPECT_DOUBLE_EQ(LineIntersector::computeEdgeDistance(vert_p1, p0, vert_p1),
                   10.0);
  EXPECT_DOUBLE_EQ(LineIntersector::computeEdgeDistance(diag_p1, p0, diag_p1),
                   5.0);

  // Interior points along dominant axis.
  EXPECT_DOUBLE_EQ(LineIntersector::computeEdgeDistance(CoordinateXY(4.0, 2.9),
                                                        p0, horiz_p1),
                   3.0);
  EXPECT_DOUBLE_EQ(
      LineIntersector::computeEdgeDistance(CoordinateXY(2.5, 7.0), p0, vert_p1),
      5.0);

  // Non-endpoint whose dominant-axis delta from p0 is 0 falls back to max(pdx,
  // pdy) so non-endpoints never produce edge distance 0.
  EXPECT_DOUBLE_EQ(LineIntersector::computeEdgeDistance(CoordinateXY(1.0, 2.5),
                                                        p0, horiz_p1),
                   0.5);
  EXPECT_DOUBLE_EQ(
      LineIntersector::computeEdgeDistance(CoordinateXY(1.5, 2.0), p0, vert_p1),
      0.5);
}

TEST(LineIntersectorTest, NoIntersectionAllRejectionPaths) {
  LineIntersector li;

  // Disjoint bounding boxes (fast envelope rejection).
  {
    const CoordinateXY p1(0.0, 0.0), p2(1.0, 1.0);
    const CoordinateXY q1(5.0, 5.0), q2(6.0, 6.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_FALSE(li.hasIntersection());
    EXPECT_EQ(li.getIntersectionNum(), kNoIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_FALSE(li.isInteriorIntersection());
    EXPECT_EQ(li.getEndpoint(0, 0), &p1);
    EXPECT_EQ(li.getEndpoint(0, 1), &p2);
    EXPECT_EQ(li.getEndpoint(1, 0), &q1);
    EXPECT_EQ(li.getEndpoint(1, 1), &q2);
    EXPECT_THAT(li.toString(), HasSubstr(" : "));
    EXPECT_THAT(li.toString(), Not(HasSubstr("proper")));
    EXPECT_THAT(li.toString(), Not(HasSubstr("endpoint")));
    EXPECT_THAT(li.toString(), Not(HasSubstr("collinear")));
  }

  // Overlapping envelopes, Q strictly on positive (left) side of P.
  {
    const CoordinateXY p1(0.0, 0.0), p2(10.0, 0.0);
    const CoordinateXY q1(2.0, 1.0), q2(8.0, 4.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_FALSE(li.hasIntersection());
    EXPECT_EQ(li.getIntersectionNum(), kNoIntersection);
  }

  // Overlapping envelopes, Q strictly on negative (right) side of P.
  {
    const CoordinateXY p1(0.0, 0.0), p2(10.0, 0.0);
    const CoordinateXY q1(2.0, -1.0), q2(8.0, -4.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_FALSE(li.hasIntersection());
    EXPECT_EQ(li.getIntersectionNum(), kNoIntersection);
  }

  // Overlapping envelopes, Q straddles infinite line of P, P on positive side
  // of Q.
  {
    const CoordinateXY p1(6.0, 0.0), p2(10.0, 0.0);
    const CoordinateXY q1(0.0, -4.0), q2(8.0, 4.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_FALSE(li.hasIntersection());
    EXPECT_EQ(li.getIntersectionNum(), kNoIntersection);
  }

  // Overlapping envelopes, Q straddles infinite line of P, P on negative side
  // of Q.
  {
    const CoordinateXY p1(0.0, 0.0), p2(2.0, 0.0);
    const CoordinateXY q1(0.0, -4.0), q2(8.0, 4.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_FALSE(li.hasIntersection());
    EXPECT_EQ(li.getIntersectionNum(), kNoIntersection);
  }

  // Collinear non-overlapping segments.
  {
    const CoordinateXY p1(0.0, 0.0), p2(2.0, 0.0);
    const CoordinateXY q1(4.0, 0.0), q2(6.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_FALSE(li.hasIntersection());
    EXPECT_EQ(li.getIntersectionNum(), kNoIntersection);
  }
}

TEST(LineIntersectorTest, ProperCrossingIntersection) {
  LineIntersector li;
  const CoordinateXY p1(0.0, 0.0), p2(10.0, 10.0);
  const CoordinateXY q1(0.0, 10.0), q2(10.0, 0.0);
  li.computeIntersection(p1, p2, q1, q2);

  EXPECT_TRUE(li.hasIntersection());
  EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
  EXPECT_TRUE(li.isProper());
  EXPECT_TRUE(li.isInteriorIntersection());
  EXPECT_TRUE(li.isInteriorIntersection(0));
  EXPECT_TRUE(li.isInteriorIntersection(1));
  EXPECT_TRUE(li.isIntersection(Coordinate(5.0, 5.0)));
  EXPECT_FALSE(li.isIntersection(Coordinate(0.0, 0.0)));

  const CoordinateXYZM& pt = li.getIntersection(0);
  EXPECT_DOUBLE_EQ(pt.x, 5.0);
  EXPECT_DOUBLE_EQ(pt.y, 5.0);
  EXPECT_TRUE(std::isnan(pt.z));
  EXPECT_TRUE(std::isnan(pt.m));

  EXPECT_DOUBLE_EQ(li.getEdgeDistance(0, 0), 5.0);
  EXPECT_DOUBLE_EQ(li.getEdgeDistance(1, 0), 5.0);
  EXPECT_THAT(li.toString(), HasSubstr("proper"));
  EXPECT_THAT(li.toString(), Not(HasSubstr("endpoint")));
  EXPECT_THAT(li.toString(), Not(HasSubstr("collinear")));
}

TEST(LineIntersectorTest, SharedEndpointIntersectionsAllPairs) {
  LineIntersector li;

  // p1 == q1
  {
    const CoordinateXY p1(2.0, 3.0), p2(6.0, 3.0);
    const CoordinateXYZM q1(2.0, 3.0, 11.0, 22.0), q2(2.0, 7.0, 15.0, 26.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_TRUE(li.hasIntersection());
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_FALSE(li.isInteriorIntersection());
    EXPECT_FALSE(li.isInteriorIntersection(0));
    EXPECT_FALSE(li.isInteriorIntersection(1));
    EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 2.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 3.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).z, 11.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).m, 22.0);
    EXPECT_THAT(li.toString(), HasSubstr("endpoint"));
    EXPECT_THAT(li.toString(), Not(HasSubstr("proper")));
    EXPECT_THAT(li.toString(), Not(HasSubstr("collinear")));
  }

  // p1 == q2 (p has Z/M, q has NaN Z/M -> preserves p's Z/M).
  {
    const CoordinateXYZM p1(2.0, 3.0, 7.0, 14.0), p2(6.0, 3.0, 9.0, 18.0);
    const CoordinateXY q1(2.0, 7.0), q2(2.0, 3.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_FALSE(li.isInteriorIntersection());
    EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 2.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 3.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).z, 7.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).m, 14.0);
  }

  // p2 == q1
  {
    const Coordinate p1(0.0, 0.0, 1.0), p2(4.0, 2.0, 5.0);
    const Coordinate q1(4.0, 2.0, 99.0), q2(4.0, 8.0, 100.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_FALSE(li.isInteriorIntersection());
    EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 4.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 2.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).z, 5.0);
    EXPECT_TRUE(std::isnan(li.getIntersection(0).m));
  }

  // p2 == q2
  {
    const CoordinateXY p1(0.0, 0.0), p2(4.0, 2.0);
    const CoordinateXY q1(4.0, 8.0), q2(4.0, 2.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_FALSE(li.isInteriorIntersection());
    EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 4.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 2.0);
  }
}

TEST(LineIntersectorTest, EndpointOnOtherInteriorAllFourCases) {
  LineIntersector li;

  // Pq1 == 0: q1 lies on interior of P, q1 lacks Z/M -> interpolates from P.
  {
    const CoordinateXYZM p1(0.0, 0.0, 10.0, 100.0), p2(10.0, 0.0, 30.0, 200.0);
    const CoordinateXY q1(2.5, 0.0), q2(2.5, 5.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_TRUE(li.isInteriorIntersection());
    EXPECT_TRUE(li.isInteriorIntersection(0));
    EXPECT_FALSE(li.isInteriorIntersection(1));
    EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 2.5);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 0.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).z, 15.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).m, 125.0);
    EXPECT_THAT(li.toString(), HasSubstr("endpoint"));
  }

  // Pq2 == 0: q2 lies on interior of P and carries its own Z/M.
  {
    const CoordinateXYZM p1(0.0, 0.0, 10.0, 100.0), p2(10.0, 0.0, 30.0, 200.0);
    const CoordinateXYZM q1(5.0, 5.0, 1.0, 2.0), q2(5.0, 0.0, 77.0, 88.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_TRUE(li.isInteriorIntersection(0));
    EXPECT_FALSE(li.isInteriorIntersection(1));
    EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 5.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 0.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).z, 77.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).m, 88.0);
  }

  // Qp1 == 0: p1 lies on interior of Q.
  {
    const CoordinateXY p1(4.0, 5.0), p2(8.0, 5.0);
    const CoordinateXYZM q1(4.0, 0.0, 0.0, 10.0), q2(4.0, 10.0, 40.0, 50.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_FALSE(li.isInteriorIntersection(0));
    EXPECT_TRUE(li.isInteriorIntersection(1));
    EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 4.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 5.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).z, 20.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).m, 30.0);
  }

  // Qp2 == 0: p2 lies on interior of Q.
  {
    const CoordinateXY p1(0.0, 5.0), p2(4.0, 5.0);
    const Coordinate q1(4.0, 0.0, 0.0), q2(4.0, 10.0, 20.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_FALSE(li.isInteriorIntersection(0));
    EXPECT_TRUE(li.isInteriorIntersection(1));
    EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 4.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 5.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).z, 10.0);
    EXPECT_TRUE(std::isnan(li.getIntersection(0).m));
  }
}

TEST(LineIntersectorTest, CollinearContainedAndOrderingAlongSegment) {
  LineIntersector li;

  // Q strictly inside P, opposite direction: exercises both branches of
  // computeIntLineIndex (dist0 > dist1 for P vs dist0 <= dist1 for Q).
  {
    const CoordinateXY p1(0.0, 0.0), p2(10.0, 0.0);
    const CoordinateXY q1(8.0, 0.0), q2(2.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);

    EXPECT_TRUE(li.hasIntersection());
    EXPECT_EQ(li.getIntersectionNum(), kCollinearIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_TRUE(li.isInteriorIntersection());
    EXPECT_TRUE(li.isInteriorIntersection(0));
    EXPECT_FALSE(li.isInteriorIntersection(1));
    EXPECT_TRUE(li.isIntersection(Coordinate(8.0, 0.0)));
    EXPECT_TRUE(li.isIntersection(Coordinate(2.0, 0.0)));
    EXPECT_FALSE(li.isIntersection(Coordinate(5.0, 0.0)));
    EXPECT_THAT(li.toString(), HasSubstr("collinear"));

    // Along P (0->10): (8,0) has distance 8, (2,0) has distance 2.
    EXPECT_EQ(li.getIndexAlongSegment(0, 0), 0u);
    EXPECT_EQ(li.getIndexAlongSegment(0, 1), 1u);
    EXPECT_TRUE(
        li.getIntersectionAlongSegment(0, 0).equals2D(Coordinate(8.0, 0.0)));
    EXPECT_TRUE(
        li.getIntersectionAlongSegment(0, 1).equals2D(Coordinate(2.0, 0.0)));
    EXPECT_DOUBLE_EQ(li.getEdgeDistance(0, 0), 8.0);
    EXPECT_DOUBLE_EQ(li.getEdgeDistance(0, 1), 2.0);

    // Along Q (8->2): (8,0) has distance 0, (2,0) has distance 6.
    EXPECT_EQ(li.getIndexAlongSegment(1, 0), 1u);
    EXPECT_EQ(li.getIndexAlongSegment(1, 1), 0u);
    EXPECT_TRUE(
        li.getIntersectionAlongSegment(1, 0).equals2D(Coordinate(2.0, 0.0)));
    EXPECT_TRUE(
        li.getIntersectionAlongSegment(1, 1).equals2D(Coordinate(8.0, 0.0)));
    EXPECT_DOUBLE_EQ(li.getEdgeDistance(1, 0), 0.0);
    EXPECT_DOUBLE_EQ(li.getEdgeDistance(1, 1), 6.0);
  }

  // Q strictly inside P, same direction.
  {
    const CoordinateXY p1(0.0, 0.0), p2(10.0, 0.0);
    const CoordinateXY q1(2.0, 0.0), q2(8.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kCollinearIntersection);
    EXPECT_EQ(li.getIndexAlongSegment(0, 0), 1u);
    EXPECT_EQ(li.getIndexAlongSegment(0, 1), 0u);
    EXPECT_TRUE(
        li.getIntersectionAlongSegment(0, 0).equals2D(Coordinate(8.0, 0.0)));
    EXPECT_TRUE(
        li.getIntersectionAlongSegment(0, 1).equals2D(Coordinate(2.0, 0.0)));
  }

  // Identical segments: neither intersection point is interior to either
  // segment.
  {
    const CoordinateXY p1(0.0, 0.0), p2(0.0, 10.0);
    const CoordinateXY q1(0.0, 0.0), q2(0.0, 10.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kCollinearIntersection);
    EXPECT_FALSE(li.isInteriorIntersection());
    EXPECT_DOUBLE_EQ(li.getEdgeDistance(0, 0), 0.0);
    EXPECT_DOUBLE_EQ(li.getEdgeDistance(0, 1), 10.0);
  }

  // P strictly inside Q.
  {
    const CoordinateXY p1(3.0, 3.0), p2(7.0, 7.0);
    const CoordinateXY q1(0.0, 0.0), q2(10.0, 10.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kCollinearIntersection);
    EXPECT_FALSE(li.isInteriorIntersection(0));
    EXPECT_TRUE(li.isInteriorIntersection(1));
  }
}

TEST(LineIntersectorTest, CollinearPartialOverlapsAndSinglePointTouches) {
  LineIntersector li;

  // Partial overlap: q1inP && p1inQ -> COLLINEAR_INTERSECTION.
  {
    const CoordinateXY p1(0.0, 0.0), p2(4.0, 0.0);
    const CoordinateXY q1(2.0, 0.0), q2(-2.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kCollinearIntersection);
    EXPECT_TRUE(li.isIntersection(Coordinate(2.0, 0.0)));
    EXPECT_TRUE(li.isIntersection(Coordinate(0.0, 0.0)));
  }

  // Collinear touch at q1 == p1 -> POINT_INTERSECTION.
  {
    const CoordinateXY p1(0.0, 0.0), p2(4.0, 0.0);
    const CoordinateXY q1(0.0, 0.0), q2(-4.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_FALSE(li.isInteriorIntersection());
    EXPECT_TRUE(li.isIntersection(Coordinate(0.0, 0.0)));
    EXPECT_THAT(li.toString(), HasSubstr("endpoint"));
    EXPECT_THAT(li.toString(), Not(HasSubstr("collinear")));
  }

  // Partial overlap: q1inP && p2inQ -> COLLINEAR_INTERSECTION.
  {
    const CoordinateXY p1(0.0, 0.0), p2(4.0, 0.0);
    const CoordinateXY q1(2.0, 0.0), q2(6.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kCollinearIntersection);
    EXPECT_TRUE(li.isIntersection(Coordinate(2.0, 0.0)));
    EXPECT_TRUE(li.isIntersection(Coordinate(4.0, 0.0)));
  }

  // Collinear touch at q1 == p2 -> POINT_INTERSECTION.
  {
    const CoordinateXY p1(0.0, 0.0), p2(4.0, 0.0);
    const CoordinateXY q1(4.0, 0.0), q2(8.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_TRUE(li.isIntersection(Coordinate(4.0, 0.0)));
  }

  // Partial overlap: q2inP && p1inQ -> COLLINEAR_INTERSECTION.
  {
    const CoordinateXY p1(0.0, 0.0), p2(4.0, 0.0);
    const CoordinateXY q1(-2.0, 0.0), q2(2.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kCollinearIntersection);
    EXPECT_TRUE(li.isIntersection(Coordinate(2.0, 0.0)));
    EXPECT_TRUE(li.isIntersection(Coordinate(0.0, 0.0)));
  }

  // Collinear touch at q2 == p1 -> POINT_INTERSECTION.
  {
    const CoordinateXY p1(0.0, 0.0), p2(4.0, 0.0);
    const CoordinateXY q1(-4.0, 0.0), q2(0.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_TRUE(li.isIntersection(Coordinate(0.0, 0.0)));
  }

  // Partial overlap: q2inP && p2inQ -> COLLINEAR_INTERSECTION.
  {
    const CoordinateXY p1(0.0, 0.0), p2(4.0, 0.0);
    const CoordinateXY q1(6.0, 0.0), q2(2.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kCollinearIntersection);
    EXPECT_TRUE(li.isIntersection(Coordinate(2.0, 0.0)));
    EXPECT_TRUE(li.isIntersection(Coordinate(4.0, 0.0)));
  }

  // Collinear touch at q2 == p2 -> POINT_INTERSECTION.
  {
    const CoordinateXY p1(0.0, 0.0), p2(4.0, 0.0);
    const CoordinateXY q1(8.0, 0.0), q2(4.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_FALSE(li.isProper());
    EXPECT_TRUE(li.isIntersection(Coordinate(4.0, 0.0)));
  }
}

TEST(LineIntersectorTest, CoordinateZMInterpolation) {
  LineIntersector li;

  // Proper intersection between XYZ and XYZM averages interpolated Z and uses
  // XYZM's M.
  {
    const Coordinate p1(0.0, 0.0, 0.0), p2(10.0, 10.0, 20.0);
    const CoordinateXYZM q1(0.0, 10.0, 100.0, 10.0), q2(10.0, 0.0, 120.0, 30.0);
    li.computeIntersection(p1, p2, q1, q2);
    ASSERT_EQ(li.getIntersectionNum(), kPointIntersection);
    const CoordinateXYZM& pt = li.getIntersection(0);
    EXPECT_DOUBLE_EQ(pt.x, 5.0);
    EXPECT_DOUBLE_EQ(pt.y, 5.0);
    // P Z at midpoint = 10, Q Z at midpoint = 110 -> average = 60.
    EXPECT_DOUBLE_EQ(pt.z, 60.0);
    EXPECT_DOUBLE_EQ(pt.m, 20.0);
  }

  // Collinear intersection with XY inside XYZM interpolates Z/M from XYZM.
  {
    const CoordinateXYZM p1(0.0, 0.0, 10.0, 100.0), p2(10.0, 0.0, 30.0, 200.0);
    const CoordinateXY q1(2.5, 0.0), q2(7.5, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    ASSERT_EQ(li.getIntersectionNum(), kCollinearIntersection);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).z, 15.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).m, 125.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(1).z, 25.0);
    EXPECT_DOUBLE_EQ(li.getIntersection(1).m, 175.0);
  }
}

TEST(LineIntersectorTest, PrecisionModelRoundsProperIntersection) {
  const CoordinateXY p1(0.0, 0.0), p2(4.0, 8.0);
  const CoordinateXY q1(0.0, 4.0), q2(4.0, 0.0);
  // Exact intersection is (4/3, 8/3) = (1.3333..., 2.6666...).

  PrecisionModel pm_fixed_unit(PrecisionModel::FIXED);
  LineIntersector li(&pm_fixed_unit);
  li.computeIntersection(p1, p2, q1, q2);
  ASSERT_EQ(li.getIntersectionNum(), kPointIntersection);
  EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 1.0);
  EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 3.0);

  PrecisionModel pm_eighths(8.0);
  li.setPrecisionModel(&pm_eighths);
  li.computeIntersection(p1, p2, q1, q2);
  EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 11.0 / 8.0);
  EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 21.0 / 8.0);

  PrecisionModel pm_tenths(10.0);
  li.setPrecisionModel(&pm_tenths);
  li.computeIntersection(p1, p2, q1, q2);
  EXPECT_NEAR(li.getIntersection(0).x, 1.3, 1e-12);
  EXPECT_NEAR(li.getIntersection(0).y, 2.7, 1e-12);

  li.setPrecisionModel(nullptr);
  li.computeIntersection(p1, p2, q1, q2);
  EXPECT_NEAR(li.getIntersection(0).x, 4.0 / 3.0, 1e-12);
  EXPECT_NEAR(li.getIntersection(0).y, 8.0 / 3.0, 1e-12);
}

TEST(LineIntersectorTest, CoordinateSequenceOverloadWithOffsetsAndDimensions) {
  LineIntersector li;

  const CoordinateSequence seq_xy{
      CoordinateXY(-10.0, -10.0),
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 10.0),
  };
  const CoordinateSequence seq_xyz{
      Coordinate(0.0, 100.0, 0.0),
      Coordinate(1.0, 100.0, 0.0),
      Coordinate(0.0, 10.0, 10.0),
      Coordinate(10.0, 0.0, 30.0),
  };
  const CoordinateSequence seq_xym{
      CoordinateXYM(0.0, 10.0, 100.0),
      CoordinateXYM(10.0, 0.0, 200.0),
  };
  const CoordinateSequence seq_xyzm{
      CoordinateXYZM(0.0, 0.0, 2.0, 4.0),
      CoordinateXYZM(10.0, 10.0, 22.0, 44.0),
  };

  // Segment 1 of seq_xy with segment 2 of seq_xyz.
  li.computeIntersection(seq_xy, 1, seq_xyz, 2);
  ASSERT_EQ(li.getIntersectionNum(), kPointIntersection);
  EXPECT_TRUE(li.isProper());
  EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 5.0);
  EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 5.0);
  EXPECT_DOUBLE_EQ(li.getIntersection(0).z, 20.0);
  EXPECT_TRUE(std::isnan(li.getIntersection(0).m));

  // Segment 1 of seq_xy with segment 0 of seq_xym.
  li.computeIntersection(seq_xy, 1, seq_xym, 0);
  ASSERT_EQ(li.getIntersectionNum(), kPointIntersection);
  EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 5.0);
  EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 5.0);
  EXPECT_TRUE(std::isnan(li.getIntersection(0).z));
  EXPECT_DOUBLE_EQ(li.getIntersection(0).m, 150.0);

  // Segment 0 of seq_xyzm with segment 0 of seq_xym.
  li.computeIntersection(seq_xyzm, 0, seq_xym, 0);
  ASSERT_EQ(li.getIntersectionNum(), kPointIntersection);
  EXPECT_DOUBLE_EQ(li.getIntersection(0).x, 5.0);
  EXPECT_DOUBLE_EQ(li.getIntersection(0).y, 5.0);
  EXPECT_DOUBLE_EQ(li.getIntersection(0).z, 12.0);
  EXPECT_DOUBLE_EQ(li.getIntersection(0).m, 87.0);
}

TEST(LineIntersectorTest, ExtremeScaleOverflowFallsBackToNearestEndpoint) {
  // At 1e155, Coordinate differences (~1e142) remain finite in Orientation and
  // Distance, while p1.x * p2.y overflows IEEE-754 double inside
  // CGAlgorithmsDD::intersection, exercising nearestEndpoint fallback for all 4
  // endpoints and both homogeneous/heterogeneous coordinate type branches.
  constexpr double kBase = 1.0e155;
  constexpr double kDelta = 1.0e142;
  LineIntersector li;

  // p1 is nearest (same coordinate types).
  {
    const CoordinateXY p1(kBase + 4.9 * kDelta, kBase);
    const CoordinateXY p2(kBase + 10.0 * kDelta, kBase);
    const CoordinateXY q1(kBase + 5.0 * kDelta, kBase - 10.0 * kDelta);
    const CoordinateXY q2(kBase + 5.0 * kDelta, kBase + 10.0 * kDelta);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_TRUE(li.isProper());
    EXPECT_DOUBLE_EQ(li.getIntersection(0).x, p1.x);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).y, p1.y);
  }

  // p2 is nearest (mixed coordinate types: C1 != C2, winner on P).
  {
    const Coordinate p1(kBase, kBase, 10.0);
    const Coordinate p2(kBase + 5.1 * kDelta, kBase, 20.0);
    const CoordinateXYZM q1(kBase + 5.0 * kDelta, kBase - 10.0 * kDelta, 30.0,
                            1.0);
    const CoordinateXYZM q2(kBase + 5.0 * kDelta, kBase + 10.0 * kDelta, 50.0,
                            3.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_TRUE(li.isProper());
    EXPECT_DOUBLE_EQ(li.getIntersection(0).x, p2.x);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).y, p2.y);
  }

  // q1 is nearest (same coordinate types).
  {
    const CoordinateXY p1(kBase, kBase);
    const CoordinateXY p2(kBase + 10.0 * kDelta, kBase);
    const CoordinateXY q1(kBase + 5.0 * kDelta, kBase - 0.1 * kDelta);
    const CoordinateXY q2(kBase + 5.0 * kDelta, kBase + 10.0 * kDelta);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_TRUE(li.isProper());
    EXPECT_DOUBLE_EQ(li.getIntersection(0).x, q1.x);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).y, q1.y);
  }

  // q2 is nearest (mixed coordinate types: C1 != C2, winner on Q).
  {
    const Coordinate p1(kBase, kBase, 10.0);
    const Coordinate p2(kBase + 10.0 * kDelta, kBase, 20.0);
    const CoordinateXYZM q1(kBase + 5.0 * kDelta, kBase - 10.0 * kDelta, 30.0,
                            1.0);
    const CoordinateXYZM q2(kBase + 5.0 * kDelta, kBase + 0.1 * kDelta, 50.0,
                            3.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
    EXPECT_TRUE(li.isProper());
    EXPECT_DOUBLE_EQ(li.getIntersection(0).x, q2.x);
    EXPECT_DOUBLE_EQ(li.getIntersection(0).y, q2.y);
  }
}

TEST(LineIntersectorTest, DegenerateZeroLengthSegments) {
  LineIntersector li;

  // Identical zero-length segments.
  {
    const CoordinateXY p1(2.0, 3.0), p2(2.0, 3.0);
    const CoordinateXY q1(2.0, 3.0), q2(2.0, 3.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_TRUE(li.hasIntersection());
    EXPECT_EQ(li.getIntersectionNum(), kCollinearIntersection);
    EXPECT_TRUE(li.getIntersection(0).equals2D(p1));
  }

  // Zero-length segment lying on interior of non-degenerate segment.
  {
    const CoordinateXY p1(2.0, 0.0), p2(2.0, 0.0);
    const CoordinateXY q1(0.0, 0.0), q2(4.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_TRUE(li.hasIntersection());
    EXPECT_EQ(li.getIntersectionNum(), kCollinearIntersection);
    EXPECT_TRUE(li.getIntersection(0).equals2D(p1));
  }

  // Zero-length segment disjoint from segment.
  {
    const CoordinateXY p1(2.0, 1.0), p2(2.0, 1.0);
    const CoordinateXY q1(0.0, 0.0), q2(4.0, 0.0);
    li.computeIntersection(p1, p2, q1, q2);
    EXPECT_FALSE(li.hasIntersection());
    EXPECT_EQ(li.getIntersectionNum(), kNoIntersection);
  }
}

TEST(LineIntersectorTest, StateResetsAcrossSequentialCalls) {
  LineIntersector li;

  const CoordinateXY col_p1(0.0, 0.0), col_p2(10.0, 0.0);
  const CoordinateXY col_q1(2.0, 0.0), col_q2(8.0, 0.0);
  li.computeIntersection(col_p1, col_p2, col_q1, col_q2);
  ASSERT_EQ(li.getIntersectionNum(), kCollinearIntersection);

  const CoordinateXY pt_p1(0.0, 0.0), pt_p2(4.0, 4.0);
  const CoordinateXY pt_q1(0.0, 4.0), pt_q2(4.0, 0.0);
  li.computeIntersection(pt_p1, pt_p2, pt_q1, pt_q2);
  EXPECT_TRUE(li.hasIntersection());
  EXPECT_EQ(li.getIntersectionNum(), kPointIntersection);
  EXPECT_TRUE(li.isProper());
  EXPECT_TRUE(li.isIntersection(Coordinate(2.0, 2.0)));
  EXPECT_FALSE(li.isIntersection(Coordinate(8.0, 0.0)));
  EXPECT_THAT(li.toString(), Not(HasSubstr("collinear")));

  const CoordinateXY dis_p1(0.0, 0.0), dis_p2(1.0, 0.0);
  const CoordinateXY dis_q1(0.0, 5.0), dis_q2(1.0, 5.0);
  li.computeIntersection(dis_p1, dis_p2, dis_q1, dis_q2);
  EXPECT_FALSE(li.hasIntersection());
  EXPECT_EQ(li.getIntersectionNum(), kNoIntersection);
  EXPECT_FALSE(li.isProper());
  EXPECT_FALSE(li.isInteriorIntersection());
  EXPECT_FALSE(li.isIntersection(Coordinate(2.0, 2.0)));
  EXPECT_THAT(li.toString(), Not(HasSubstr("proper")));
}

}  // namespace
}  // namespace geos_google
