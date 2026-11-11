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

// Unit tests for third_party/geos/src/algorithm/Intersection.cpp and
// third_party/geos/include/geos/algorithm/Intersection.h.

#include "third_party/geos/include/geos/algorithm/Intersection.h"

#include <cmath>
#include <limits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::algorithm::Intersection;
using geos::geom::CoordinateXY;
using geos::util::IllegalArgumentException;

TEST(IntersectionTest, OrthogonalAndDiagonalLinesIntersect) {
  CoordinateXY ortho = Intersection::intersection({0.0, 5.0}, {10.0, 5.0},
                                                  {5.0, 0.0}, {5.0, 10.0});
  EXPECT_FALSE(ortho.isNull());
  EXPECT_DOUBLE_EQ(ortho.x, 5.0);
  EXPECT_DOUBLE_EQ(ortho.y, 5.0);

  CoordinateXY diag = Intersection::intersection({0.0, 0.0}, {2.0, 2.0},
                                                 {0.0, 2.0}, {2.0, 0.0});
  EXPECT_FALSE(diag.isNull());
  EXPECT_DOUBLE_EQ(diag.x, 1.0);
  EXPECT_DOUBLE_EQ(diag.y, 1.0);

  // Fractional intersection: y = 2x and x + 3y = 3 -> (3/7, 6/7).
  CoordinateXY frac = Intersection::intersection({0.0, 0.0}, {1.0, 2.0},
                                                 {0.0, 1.0}, {3.0, 0.0});
  EXPECT_FALSE(frac.isNull());
  EXPECT_NEAR(frac.x, 3.0 / 7.0, 1e-15);
  EXPECT_NEAR(frac.y, 6.0 / 7.0, 1e-15);
}

TEST(IntersectionTest, InfiniteLineExtensionAndNegativeCoordinates) {
  // Segments do not overlap, but infinite lines intersect at (5, 0).
  CoordinateXY ext = Intersection::intersection({0.0, 0.0}, {1.0, 0.0},
                                                {5.0, 2.0}, {5.0, 4.0});
  EXPECT_FALSE(ext.isNull());
  EXPECT_DOUBLE_EQ(ext.x, 5.0);
  EXPECT_DOUBLE_EQ(ext.y, 0.0);

  CoordinateXY neg = Intersection::intersection({-4.0, -2.0}, {-1.0, 4.0},
                                                {-5.0, 0.0}, {0.0, 0.0});
  EXPECT_FALSE(neg.isNull());
  EXPECT_DOUBLE_EQ(neg.x, -3.0);
  EXPECT_DOUBLE_EQ(neg.y, 0.0);
}

TEST(IntersectionTest, NearlyParallelLinesComputeAccurateIntersection) {
  CoordinateXY pt = Intersection::intersection({0.0, 0.0}, {1e8, 1.0},
                                               {0.0, 2.0}, {1e8, 1.0});
  EXPECT_FALSE(pt.isNull());
  EXPECT_DOUBLE_EQ(pt.x, 1e8);
  EXPECT_DOUBLE_EQ(pt.y, 1.0);
}

TEST(IntersectionTest, ParallelCollinearAndDegenerateLinesReturnNull) {
  // Parallel horizontal lines.
  EXPECT_TRUE(Intersection::intersection({0.0, 0.0}, {10.0, 0.0}, {0.0, 1.0},
                                         {10.0, 1.0})
                  .isNull());

  // Parallel diagonal lines.
  EXPECT_TRUE(
      Intersection::intersection({0.0, 0.0}, {2.0, 4.0}, {1.0, 0.0}, {3.0, 4.0})
          .isNull());

  // Collinear lines.
  EXPECT_TRUE(Intersection::intersection({0.0, 0.0}, {5.0, 5.0}, {6.0, 6.0},
                                         {10.0, 10.0})
                  .isNull());

  // Zero-length degenerate line.
  EXPECT_TRUE(
      Intersection::intersection({1.0, 2.0}, {1.0, 2.0}, {0.0, 0.0}, {4.0, 4.0})
          .isNull());
  EXPECT_TRUE(
      Intersection::intersection({0.0, 0.0}, {4.0, 4.0}, {2.0, 2.0}, {2.0, 2.0})
          .isNull());
}

TEST(IntersectionTest, LineSegmentEndpointOnLineReturnsEndpoint) {
  // seg1 lies on the line (between line1 and line2).
  CoordinateXY s1_on = Intersection::intersectionLineSegment(
      {0.0, 0.0}, {10.0, 0.0}, {3.0, 0.0}, {3.0, 5.0});
  EXPECT_EQ(s1_on, CoordinateXY(3.0, 0.0));

  // seg1 lies on the extension of the line.
  CoordinateXY s1_ext = Intersection::intersectionLineSegment(
      {0.0, 0.0}, {10.0, 0.0}, {15.0, 0.0}, {15.0, -4.0});
  EXPECT_EQ(s1_ext, CoordinateXY(15.0, 0.0));

  // Segment collinear with the line returns seg1.
  CoordinateXY collinear = Intersection::intersectionLineSegment(
      {0.0, 0.0}, {10.0, 0.0}, {2.0, 0.0}, {8.0, 0.0});
  EXPECT_EQ(collinear, CoordinateXY(2.0, 0.0));

  // seg2 lies on the line while seg1 is off the line (both sides).
  CoordinateXY s2_left = Intersection::intersectionLineSegment(
      {0.0, 0.0}, {10.0, 0.0}, {4.0, 5.0}, {7.0, 0.0});
  EXPECT_EQ(s2_left, CoordinateXY(7.0, 0.0));

  CoordinateXY s2_right = Intersection::intersectionLineSegment(
      {0.0, 0.0}, {10.0, 0.0}, {4.0, -5.0}, {-2.0, 0.0});
  EXPECT_EQ(s2_right, CoordinateXY(-2.0, 0.0));
}

TEST(IntersectionTest, LineSegmentBothEndpointsOnSameSideReturnsNull) {
  // Both strictly left of line1 -> line2.
  EXPECT_TRUE(Intersection::intersectionLineSegment({0.0, 0.0}, {10.0, 0.0},
                                                    {2.0, 1.0}, {8.0, 5.0})
                  .isNull());

  // Both strictly right of line1 -> line2.
  EXPECT_TRUE(Intersection::intersectionLineSegment({0.0, 0.0}, {10.0, 0.0},
                                                    {2.0, -1.0}, {8.0, -5.0})
                  .isNull());

  // Zero-length segment off the line.
  EXPECT_TRUE(Intersection::intersectionLineSegment({0.0, 0.0}, {10.0, 0.0},
                                                    {5.0, 2.0}, {5.0, 2.0})
                  .isNull());
}

TEST(IntersectionTest, LineSegmentCrossingComputesIntersection) {
  // Crossing from left to right.
  CoordinateXY l_to_r = Intersection::intersectionLineSegment(
      {0.0, 0.0}, {10.0, 0.0}, {4.0, 3.0}, {4.0, -1.0});
  EXPECT_FALSE(l_to_r.isNull());
  EXPECT_DOUBLE_EQ(l_to_r.x, 4.0);
  EXPECT_DOUBLE_EQ(l_to_r.y, 0.0);

  // Crossing from right to left along line extension.
  CoordinateXY r_to_l = Intersection::intersectionLineSegment(
      {0.0, 0.0}, {10.0, 0.0}, {20.0, -2.0}, {24.0, 2.0});
  EXPECT_FALSE(r_to_l.isNull());
  EXPECT_DOUBLE_EQ(r_to_l.x, 22.0);
  EXPECT_DOUBLE_EQ(r_to_l.y, 0.0);

  // Diagonal line and diagonal crossing segment.
  CoordinateXY diag = Intersection::intersectionLineSegment(
      {0.0, 0.0}, {10.0, 10.0}, {0.0, 6.0}, {6.0, 0.0});
  EXPECT_FALSE(diag.isNull());
  EXPECT_DOUBLE_EQ(diag.x, 3.0);
  EXPECT_DOUBLE_EQ(diag.y, 3.0);
}

TEST(IntersectionTest,
     LineSegmentFallbackSelectsClosestEndpointOnIntersectionOverflow) {
  // Choose powers of 2 where Orientation::index and Distance remain finite and
  // exact, while the degree-3 numerator in CGAlgorithmsDD::intersection
  // overflows IEEE-754 double (~2^1080 > DBL_MAX) and returns null.
  const double x = std::ldexp(1.0, 380);
  const double d = std::ldexp(1.0, 350);
  const CoordinateXY line1(x, 0.0);
  const CoordinateXY line2(x, d);
  const CoordinateXY near_pt(x - d, 0.5 * d);
  const CoordinateXY far_pt(x + 2.0 * d, 0.5 * d);
  const CoordinateXY equal_pt(x + d, 0.5 * d);

  ASSERT_TRUE(
      Intersection::intersection(line1, line2, near_pt, far_pt).isNull());

  // dist1 < dist2 -> returns seg1.
  EXPECT_EQ(
      Intersection::intersectionLineSegment(line1, line2, near_pt, far_pt),
      near_pt);

  // dist1 > dist2 -> returns seg2.
  EXPECT_EQ(
      Intersection::intersectionLineSegment(line1, line2, far_pt, near_pt),
      near_pt);

  // dist1 == dist2 -> returns seg2.
  EXPECT_EQ(
      Intersection::intersectionLineSegment(line1, line2, near_pt, equal_pt),
      equal_pt);
}

TEST(IntersectionTest, NonFiniteSegmentCoordinateThrowsIllegalArgument) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(Intersection::intersectionLineSegment({0.0, 0.0}, {1.0, 0.0},
                                                     {nan, 1.0}, {1.0, -1.0}),
               IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
