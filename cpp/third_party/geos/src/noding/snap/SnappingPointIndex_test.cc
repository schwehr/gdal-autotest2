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

#include "third_party/geos/include/geos/noding/snap/SnappingPointIndex.h"

#include <cmath>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::noding::snap::SnappingPointIndex;

TEST(SnappingPointIndexTest, InsertsFirstPointAndSnapsExactDuplicate) {
  SnappingPointIndex index(0.1);
  const Coordinate input(1.5, 2.5, 3.5);

  const Coordinate& first = index.snap(input);
  EXPECT_NE(&first, &input);
  EXPECT_TRUE(first.equals3D(input));

  const Coordinate duplicate_xy(1.5, 2.5, 9.0);
  const Coordinate& second = index.snap(duplicate_xy);
  EXPECT_EQ(&second, &first);
  EXPECT_DOUBLE_EQ(second.x, 1.5);
  EXPECT_DOUBLE_EQ(second.y, 2.5);
  EXPECT_DOUBLE_EQ(second.z, 3.5);
}

TEST(SnappingPointIndexTest, SnapsWithinAndAtToleranceBoundaryOnly) {
  SnappingPointIndex index(1.0);
  const Coordinate& base = index.snap(Coordinate(10.0, 20.0));

  // Euclidean distance 0.5 <= 1.0 snaps to base.
  const Coordinate& within = index.snap(Coordinate(10.3, 20.4));
  EXPECT_EQ(&within, &base);
  EXPECT_DOUBLE_EQ(within.x, 10.0);
  EXPECT_DOUBLE_EQ(within.y, 20.0);

  // Euclidean distance exactly 1.0 <= 1.0 snaps to base.
  const Coordinate& on_boundary = index.snap(Coordinate(11.0, 20.0));
  EXPECT_EQ(&on_boundary, &base);

  // Euclidean distance strictly greater than 1.0 inserts a new snap point.
  const Coordinate& outside = index.snap(Coordinate(11.0001, 20.0));
  EXPECT_NE(&outside, &base);
  EXPECT_DOUBLE_EQ(outside.x, 11.0001);
  EXPECT_DOUBLE_EQ(outside.y, 20.0);
}

TEST(SnappingPointIndexTest, ZeroToleranceSnapsOnlyExact2DMatches) {
  SnappingPointIndex index(0.0);

  const Coordinate& first = index.snap(Coordinate(1.0, 2.0, 10.0));
  const Coordinate& near_pt = index.snap(Coordinate(1.0 + 1e-12, 2.0, 20.0));
  EXPECT_NE(&near_pt, &first);
  EXPECT_DOUBLE_EQ(near_pt.x, 1.0 + 1e-12);
  EXPECT_DOUBLE_EQ(near_pt.z, 20.0);

  const Coordinate& exact_xy = index.snap(Coordinate(1.0, 2.0, 30.0));
  EXPECT_EQ(&exact_xy, &first);
  EXPECT_DOUBLE_EQ(exact_xy.z, 10.0);
}

TEST(SnappingPointIndexTest, SnapsToClosestCandidateAndBreaksTiesByOrder) {
  SnappingPointIndex index(2.0);
  const Coordinate& left = index.snap(Coordinate(0.0, 0.0));
  const Coordinate& right = index.snap(Coordinate(3.0, 0.0));
  EXPECT_NE(&left, &right);

  // Closer to (3, 0) (dist 1.2) than to (0, 0) (dist 1.8).
  const Coordinate& snapped_right = index.snap(Coordinate(1.8, 0.0));
  EXPECT_EQ(&snapped_right, &right);

  // Equidistant candidates (dist 1.5 to both) break tie toward lesser coords.
  SnappingPointIndex tie_index(2.0);
  const Coordinate& higher = tie_index.snap(Coordinate(13.0, 0.0));
  const Coordinate& lower = tie_index.snap(Coordinate(10.0, 0.0));
  const Coordinate& tied = tie_index.snap(Coordinate(11.5, 0.0));
  EXPECT_NE(&higher, &lower);
  EXPECT_EQ(&tied, &lower);
}

TEST(SnappingPointIndexTest, DoesNotChainSnapThroughSnappedQueryPoints) {
  SnappingPointIndex index(1.0);
  const Coordinate& origin = index.snap(Coordinate(0.0, 0.0));

  // Snaps to origin; (0.8, 0.0) is not stored as a separate snap target.
  const Coordinate& intermediate = index.snap(Coordinate(0.8, 0.0));
  EXPECT_EQ(&intermediate, &origin);

  // Distance to (0.8, 0.0) is 0.7, but distance to origin is 1.5 > 1.0.
  const Coordinate& third = index.snap(Coordinate(1.5, 0.0));
  EXPECT_NE(&third, &origin);
  EXPECT_DOUBLE_EQ(third.x, 1.5);
  EXPECT_DOUBLE_EQ(third.y, 0.0);
}

TEST(SnappingPointIndexTest, PreservesMissingZAsNaN) {
  SnappingPointIndex index(0.5);
  const Coordinate& first = index.snap(Coordinate(5.0, 6.0));
  EXPECT_TRUE(std::isnan(first.z));

  const Coordinate& snapped = index.snap(Coordinate(5.1, 6.1, 42.0));
  EXPECT_EQ(&snapped, &first);
  EXPECT_TRUE(std::isnan(snapped.z));
}

}  // namespace
}  // namespace geos_google
