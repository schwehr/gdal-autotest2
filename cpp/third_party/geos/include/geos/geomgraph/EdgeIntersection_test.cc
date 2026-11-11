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

// Unit tests for third_party/geos/include/geos/geomgraph/EdgeIntersection.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/geomgraph/EdgeIntersection.h

#include "third_party/geos/include/geos/geomgraph/EdgeIntersection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geomgraph::EdgeIntersection;
using geos::geomgraph::EdgeIntersectionLessThen;

static_assert(std::is_final_v<EdgeIntersection>);
static_assert(!std::is_default_constructible_v<EdgeIntersection>);
static_assert(std::is_copy_constructible_v<EdgeIntersection>);
static_assert(std::is_copy_assignable_v<EdgeIntersection>);
static_assert(std::is_move_constructible_v<EdgeIntersection>);
static_assert(std::is_move_assignable_v<EdgeIntersection>);

TEST(EdgeIntersectionTest, ConstructsAndExposesMembersAndAccessors) {
  const Coordinate coord2d(1.5, 2.5);
  const EdgeIntersection ei2d(coord2d, 3, 0.75);

  EXPECT_DOUBLE_EQ(ei2d.coord.x, 1.5);
  EXPECT_DOUBLE_EQ(ei2d.coord.y, 2.5);
  EXPECT_TRUE(std::isnan(ei2d.coord.z));
  EXPECT_EQ(ei2d.segmentIndex, 3u);
  EXPECT_DOUBLE_EQ(ei2d.dist, 0.75);

  EXPECT_EQ(&ei2d.getCoordinate(), &ei2d.coord);
  EXPECT_TRUE(ei2d.getCoordinate().equals2D(coord2d));
  EXPECT_EQ(ei2d.getSegmentIndex(), 3u);
  EXPECT_DOUBLE_EQ(ei2d.getDistance(), 0.75);

  const Coordinate coord3d(4.0, -5.0, 9.5);
  const EdgeIntersection ei3d(coord3d, 0, 0.0);
  EXPECT_TRUE(ei3d.getCoordinate().equals3D(coord3d));

  EdgeIntersection copied(ei3d);
  EXPECT_TRUE(copied.getCoordinate().equals3D(coord3d));
  EXPECT_EQ(copied.getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(copied.getDistance(), 0.0);

  copied = ei2d;
  EXPECT_TRUE(copied.getCoordinate().equals2D(coord2d));
  EXPECT_TRUE(std::isnan(copied.getCoordinate().z));
  EXPECT_EQ(copied.getSegmentIndex(), 3u);
  EXPECT_DOUBLE_EQ(copied.getDistance(), 0.75);
}

TEST(EdgeIntersectionTest, IsEndPointHandlesStartEndAndInteriorCases) {
  struct EndPointCase {
    std::size_t segment_index;
    double dist;
    std::size_t max_segment_index;
    bool expected;
  };

  constexpr auto kCases = std::to_array<EndPointCase>({
      {.segment_index = 0,
       .dist = 0.0,
       .max_segment_index = 0,
       .expected = true},
      {.segment_index = 0,
       .dist = 0.0,
       .max_segment_index = 4,
       .expected = true},
      {.segment_index = 0,
       .dist = -0.0,
       .max_segment_index = 4,
       .expected = true},
      {.segment_index = 0,
       .dist = 0.25,
       .max_segment_index = 4,
       .expected = false},
      {.segment_index = 0,
       .dist = 0.25,
       .max_segment_index = 0,
       .expected = true},
      {.segment_index = 1,
       .dist = 0.0,
       .max_segment_index = 4,
       .expected = false},
      {.segment_index = 2,
       .dist = 1.5,
       .max_segment_index = 4,
       .expected = false},
      {.segment_index = 3,
       .dist = 0.99,
       .max_segment_index = 4,
       .expected = false},
      {.segment_index = 4,
       .dist = 0.0,
       .max_segment_index = 4,
       .expected = true},
      {.segment_index = 4,
       .dist = 0.5,
       .max_segment_index = 4,
       .expected = true},
      {.segment_index = 5,
       .dist = 0.0,
       .max_segment_index = 4,
       .expected = false},
  });

  const Coordinate pt(10.0, 20.0);
  for (std::size_t i = 0; i < kCases.size(); ++i) {
    const auto& tc = kCases[i];
    SCOPED_TRACE(i);
    const EdgeIntersection ei(pt, tc.segment_index, tc.dist);
    EXPECT_EQ(ei.isEndPoint(tc.max_segment_index), tc.expected);
  }
}

TEST(EdgeIntersectionTest, EqualityComparesSegmentIndexAndDistanceOnly) {
  const EdgeIntersection base(Coordinate(1.0, 2.0, 3.0), 2, 1.25);
  const EdgeIntersection same_values(Coordinate(1.0, 2.0, 3.0), 2, 1.25);
  const EdgeIntersection different_coord(Coordinate(99.0, -42.0, 7.0), 2, 1.25);
  const EdgeIntersection different_segment(Coordinate(1.0, 2.0, 3.0), 3, 1.25);
  const EdgeIntersection different_dist(Coordinate(1.0, 2.0, 3.0), 2, 1.5);

  EXPECT_TRUE(base == same_values);
  EXPECT_FALSE(base != same_values);

  // Coordinate is intentionally ignored by operator== to stay consistent with
  // <.
  EXPECT_TRUE(base == different_coord);
  EXPECT_FALSE(base != different_coord);

  EXPECT_FALSE(base == different_segment);
  EXPECT_TRUE(base != different_segment);

  EXPECT_FALSE(base == different_dist);
  EXPECT_TRUE(base != different_dist);

  const EdgeIntersection pos_zero(Coordinate(0.0, 0.0), 0, 0.0);
  const EdgeIntersection neg_zero(Coordinate(0.0, 0.0), 0, -0.0);
  EXPECT_EQ(pos_zero, neg_zero);

  const EdgeIntersection nan_dist(Coordinate(0.0, 0.0), 1, DoubleNotANumber);
  EXPECT_FALSE(nan_dist == nan_dist);
  EXPECT_TRUE(nan_dist != nan_dist);
}

TEST(EdgeIntersectionTest, LessThanImplementsStrictWeakOrdering) {
  const EdgeIntersection seg1_large_dist(Coordinate(5.0, 5.0), 1, 100.0);
  const EdgeIntersection seg2_zero_dist(Coordinate(0.0, 0.0), 2, 0.0);
  const EdgeIntersection seg2_mid_dist(Coordinate(1.0, 1.0), 2, 0.5);
  const EdgeIntersection seg2_mid_alt_coord(Coordinate(9.0, 9.0), 2, 0.5);
  const EdgeIntersection seg2_high_dist(Coordinate(2.0, 2.0), 2, 1.5);
  const EdgeIntersection seg_max(Coordinate(3.0, 3.0),
                                 std::numeric_limits<std::size_t>::max(), 0.0);

  // Primary ordering by segmentIndex.
  EXPECT_TRUE(seg1_large_dist < seg2_zero_dist);
  EXPECT_FALSE(seg2_zero_dist < seg1_large_dist);
  EXPECT_TRUE(seg2_high_dist < seg_max);
  EXPECT_FALSE(seg_max < seg2_high_dist);

  // Secondary ordering by dist when segmentIndex matches.
  EXPECT_TRUE(seg2_zero_dist < seg2_mid_dist);
  EXPECT_FALSE(seg2_mid_dist < seg2_zero_dist);
  EXPECT_TRUE(seg2_mid_dist < seg2_high_dist);
  EXPECT_FALSE(seg2_high_dist < seg2_mid_dist);

  // Equivalence when segmentIndex and dist match (even with differing coords).
  EXPECT_FALSE(seg2_mid_dist < seg2_mid_dist);
  EXPECT_FALSE(seg2_mid_dist < seg2_mid_alt_coord);
  EXPECT_FALSE(seg2_mid_alt_coord < seg2_mid_dist);

  // NaN distance produces false in both directions.
  const EdgeIntersection nan_dist(Coordinate(0.0, 0.0), 2, DoubleNotANumber);
  EXPECT_FALSE(nan_dist < seg2_mid_dist);
  EXPECT_FALSE(seg2_mid_dist < nan_dist);

  std::vector<EdgeIntersection> items = {
      seg_max, seg2_high_dist, seg2_zero_dist, seg1_large_dist, seg2_mid_dist,
  };
  std::sort(items.begin(), items.end());
  EXPECT_TRUE(std::is_sorted(items.begin(), items.end()));
  EXPECT_EQ(items[0], seg1_large_dist);
  EXPECT_EQ(items[1], seg2_zero_dist);
  EXPECT_EQ(items[2], seg2_mid_dist);
  EXPECT_EQ(items[3], seg2_high_dist);
  EXPECT_EQ(items[4], seg_max);
}

TEST(EdgeIntersectionTest, EdgeIntersectionLessThenSupportsValuesAndPointers) {
  const EdgeIntersection a(Coordinate(0.0, 0.0), 0, 0.5);
  const EdgeIntersection b(Coordinate(1.0, 1.0), 1, 0.1);
  const EdgeIntersection c(Coordinate(2.0, 2.0), 1, 0.4);
  const EdgeIntersection c_equiv(Coordinate(9.0, 9.0), 1, 0.4);

  const EdgeIntersectionLessThen less;

  // Value overload.
  EXPECT_TRUE(less(a, b));
  EXPECT_FALSE(less(b, a));
  EXPECT_TRUE(less(b, c));
  EXPECT_FALSE(less(c, b));
  EXPECT_FALSE(less(c, c_equiv));
  EXPECT_FALSE(less(c_equiv, c));

  // Pointer overload.
  EXPECT_TRUE(less(&a, &b));
  EXPECT_FALSE(less(&b, &a));
  EXPECT_TRUE(less(&b, &c));
  EXPECT_FALSE(less(&c, &b));
  EXPECT_FALSE(less(&c, &c_equiv));
  EXPECT_FALSE(less(&c_equiv, &c));

  std::set<EdgeIntersection, EdgeIntersectionLessThen> value_set;
  EXPECT_TRUE(value_set.insert(c).second);
  EXPECT_FALSE(value_set.insert(c_equiv).second);
  EXPECT_TRUE(value_set.insert(a).second);
  EXPECT_TRUE(value_set.insert(b).second);
  ASSERT_EQ(value_set.size(), 3u);
  auto it = value_set.begin();
  EXPECT_EQ(*it++, a);
  EXPECT_EQ(*it++, b);
  EXPECT_EQ(*it++, c);

  std::set<const EdgeIntersection*, EdgeIntersectionLessThen> ptr_set;
  EXPECT_TRUE(ptr_set.insert(&c).second);
  EXPECT_FALSE(ptr_set.insert(&c_equiv).second);
  EXPECT_TRUE(ptr_set.insert(&a).second);
  EXPECT_TRUE(ptr_set.insert(&b).second);
  ASSERT_EQ(ptr_set.size(), 3u);
  auto pit = ptr_set.begin();
  EXPECT_EQ(*pit++, &a);
  EXPECT_EQ(*pit++, &b);
  EXPECT_EQ(*pit++, &c);
}

TEST(EdgeIntersectionTest, StreamInsertionFormatsCoordinateSegmentAndDistance) {
  const EdgeIntersection ei2d(Coordinate(1.5, 2.5), 3, 0.75);
  std::ostringstream oss2d;
  oss2d << ei2d;
  EXPECT_EQ(oss2d.str(), "1.5 2.5 seg # = 3 dist = 0.75");

  const EdgeIntersection ei3d(Coordinate(1.5, 2.5, 9.0), 4, 1.25);
  std::ostringstream oss3d;
  oss3d << ei3d;
  EXPECT_EQ(oss3d.str(), "1.5 2.5 9 seg # = 4 dist = 1.25");

  std::ostringstream chained;
  chained << ei2d << " | " << ei3d;
  EXPECT_EQ(chained.str(),
            "1.5 2.5 seg # = 3 dist = 0.75 | 1.5 2.5 9 seg # = 4 dist = 1.25");
}

}  // namespace
}  // namespace geos_google
