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

// Unit tests for
// third_party/geos/src/noding/FastSegmentSetIntersectionFinder.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/noding/FastSegmentSetIntersectionFinder.cpp

#include "third_party/geos/include/geos/noding/FastSegmentSetIntersectionFinder.h"

#include <array>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentIntersectionDetector.h"
#include "third_party/geos/include/geos/noding/SegmentSetMutualIntersector.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::noding::BasicSegmentString;
using geos::noding::FastSegmentSetIntersectionFinder;
using geos::noding::SegmentIntersectionDetector;
using geos::noding::SegmentSetMutualIntersector;
using geos::noding::SegmentString;

// Owns CoordinateSequence and BasicSegmentString storage while exposing raw
// SegmentString pointers in a SegmentString::ConstVect.
struct OwnedSegmentSet {
  std::vector<std::unique_ptr<CoordinateSequence>> sequences;
  std::vector<std::unique_ptr<BasicSegmentString>> strings;
  SegmentString::ConstVect ptrs;

  void Add(const std::vector<CoordinateXY>& coords) {
    auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
    seq->reserve(coords.size());
    for (const CoordinateXY& c : coords) {
      seq->add(c);
    }
    auto ss = std::make_unique<BasicSegmentString>(seq.get(), nullptr);
    ptrs.push_back(ss.get());
    sequences.push_back(std::move(seq));
    strings.push_back(std::move(ss));
  }
};

struct Point2D {
  double x;
  double y;
};

struct SegmentPairCase {
  std::string_view name;
  Point2D base_start;
  Point2D base_end;
  Point2D query_start;
  Point2D query_end;
  bool expected_intersects;
};

constexpr auto kSegmentPairCases = std::to_array<SegmentPairCase>({
    {
        .name = "proper_crossing",
        .base_start = {0.0, 0.0},
        .base_end = {10.0, 10.0},
        .query_start = {0.0, 10.0},
        .query_end = {10.0, 0.0},
        .expected_intersects = true,
    },
    {
        .name = "t_junction_endpoint_on_interior",
        .base_start = {0.0, 0.0},
        .base_end = {10.0, 0.0},
        .query_start = {5.0, 0.0},
        .query_end = {5.0, 5.0},
        .expected_intersects = true,
    },
    {
        .name = "shared_endpoint",
        .base_start = {0.0, 0.0},
        .base_end = {5.0, 0.0},
        .query_start = {5.0, 0.0},
        .query_end = {10.0, 0.0},
        .expected_intersects = true,
    },
    {
        .name = "collinear_overlap",
        .base_start = {0.0, 0.0},
        .base_end = {10.0, 0.0},
        .query_start = {5.0, 0.0},
        .query_end = {15.0, 0.0},
        .expected_intersects = true,
    },
    {
        .name = "collinear_disjoint",
        .base_start = {0.0, 0.0},
        .base_end = {4.0, 0.0},
        .query_start = {6.0, 0.0},
        .query_end = {10.0, 0.0},
        .expected_intersects = false,
    },
    {
        .name = "parallel_disjoint",
        .base_start = {0.0, 0.0},
        .base_end = {10.0, 0.0},
        .query_start = {0.0, 1.0},
        .query_end = {10.0, 1.0},
        .expected_intersects = false,
    },
    {
        .name = "overlapping_envelopes_disjoint_segments",
        .base_start = {0.0, 0.0},
        .base_end = {10.0, 10.0},
        .query_start = {0.0, 8.0},
        .query_end = {2.0, 10.0},
        .expected_intersects = false,
    },
    {
        .name = "far_disjoint",
        .base_start = {0.0, 0.0},
        .base_end = {1.0, 1.0},
        .query_start = {100.0, 100.0},
        .query_end = {101.0, 101.0},
        .expected_intersects = false,
    },
    {
        .name = "identical_segments",
        .base_start = {0.0, 0.0},
        .base_end = {10.0, 10.0},
        .query_start = {0.0, 0.0},
        .query_end = {10.0, 10.0},
        .expected_intersects = true,
    },
    {
        .name = "collinear_contained",
        .base_start = {0.0, 0.0},
        .base_end = {10.0, 0.0},
        .query_start = {2.0, 0.0},
        .query_end = {8.0, 0.0},
        .expected_intersects = true,
    },
    {
        .name = "degenerate_point_segment_on_line",
        .base_start = {0.0, 0.0},
        .base_end = {10.0, 0.0},
        .query_start = {5.0, 0.0},
        .query_end = {5.0, 0.0},
        .expected_intersects = true,
    },
    {
        .name = "degenerate_point_segment_off_line",
        .base_start = {0.0, 0.0},
        .base_end = {10.0, 0.0},
        .query_start = {5.0, 1.0},
        .query_end = {5.0, 1.0},
        .expected_intersects = false,
    },
});

TEST(FastSegmentSetIntersectionFinderTest,
     GetSegmentSetIntersectorReturnsNonNull) {
  OwnedSegmentSet base;
  base.Add({{0.0, 0.0}, {10.0, 0.0}});

  FastSegmentSetIntersectionFinder finder(&base.ptrs);
  const FastSegmentSetIntersectionFinder& const_finder = finder;
  const SegmentSetMutualIntersector* intersector =
      const_finder.getSegmentSetIntersector();
  EXPECT_NE(intersector, nullptr);
  EXPECT_EQ(finder.getSegmentSetIntersector(), intersector);
}

TEST(FastSegmentSetIntersectionFinderTest, IntersectsSegmentPairCases) {
  for (const SegmentPairCase& tc : kSegmentPairCases) {
    SCOPED_TRACE(tc.name);
    OwnedSegmentSet base;
    base.Add({
        {tc.base_start.x, tc.base_start.y},
        {tc.base_end.x, tc.base_end.y},
    });
    OwnedSegmentSet query;
    query.Add({
        {tc.query_start.x, tc.query_start.y},
        {tc.query_end.x, tc.query_end.y},
    });

    FastSegmentSetIntersectionFinder finder(&base.ptrs);
    EXPECT_EQ(finder.intersects(&query.ptrs), tc.expected_intersects);
  }
}

TEST(FastSegmentSetIntersectionFinderTest,
     EmptySetsAndEmptyStringsDoNotIntersect) {
  OwnedSegmentSet empty_set;
  OwnedSegmentSet non_empty;
  non_empty.Add({{0.0, 0.0}, {10.0, 10.0}});

  {
    FastSegmentSetIntersectionFinder finder(&empty_set.ptrs);
    EXPECT_FALSE(finder.intersects(&non_empty.ptrs));
    EXPECT_FALSE(finder.intersects(&empty_set.ptrs));
  }

  {
    FastSegmentSetIntersectionFinder finder(&non_empty.ptrs);
    EXPECT_FALSE(finder.intersects(&empty_set.ptrs));
  }

  OwnedSegmentSet empty_coords_base;
  empty_coords_base.Add({});
  OwnedSegmentSet empty_coords_query;
  empty_coords_query.Add({});

  {
    FastSegmentSetIntersectionFinder finder(&empty_coords_base.ptrs);
    EXPECT_FALSE(finder.intersects(&non_empty.ptrs));
    EXPECT_FALSE(finder.intersects(&empty_coords_query.ptrs));
  }

  {
    FastSegmentSetIntersectionFinder finder(&non_empty.ptrs);
    EXPECT_FALSE(finder.intersects(&empty_coords_query.ptrs));
  }

  OwnedSegmentSet single_vertex_base;
  single_vertex_base.Add({{5.0, 0.0}});
  OwnedSegmentSet single_vertex_query;
  single_vertex_query.Add({{5.0, 0.0}});
  OwnedSegmentSet horizontal;
  horizontal.Add({{0.0, 0.0}, {10.0, 0.0}});

  {
    FastSegmentSetIntersectionFinder finder(&single_vertex_base.ptrs);
    EXPECT_FALSE(finder.intersects(&horizontal.ptrs));
    EXPECT_FALSE(finder.intersects(&single_vertex_query.ptrs));
  }

  {
    FastSegmentSetIntersectionFinder finder(&horizontal.ptrs);
    EXPECT_FALSE(finder.intersects(&single_vertex_query.ptrs));
  }
}

TEST(FastSegmentSetIntersectionFinderTest, MultiSegmentNonMonotoneChains) {
  OwnedSegmentSet base;
  base.Add({
      {0.0, 0.0},
      {5.0, 5.0},
      {10.0, 0.0},
      {15.0, 5.0},
      {20.0, 0.0},
  });

  FastSegmentSetIntersectionFinder finder(&base.ptrs);

  OwnedSegmentSet intersecting_query;
  intersecting_query.Add({{11.0, 0.0}, {14.0, 5.0}});
  EXPECT_TRUE(finder.intersects(&intersecting_query.ptrs));

  // First segment stays above the valley; second crosses (10,0)->(15,5) at
  // (12,2).
  OwnedSegmentSet multi_seg_intersecting_query;
  multi_seg_intersecting_query.Add({{8.0, 5.0}, {10.0, 2.0}, {13.0, 2.0}});
  EXPECT_TRUE(finder.intersects(&multi_seg_intersecting_query.ptrs));

  // Envelope overlaps (0,0)->(5,5) and (5,5)->(10,0), but stays strictly below
  // y = 4 across x in [4, 6].
  OwnedSegmentSet disjoint_under_peak_query;
  disjoint_under_peak_query.Add(
      {{4.0, 0.0}, {4.0, 3.0}, {6.0, 3.0}, {6.0, 0.0}});
  EXPECT_FALSE(finder.intersects(&disjoint_under_peak_query.ptrs));

  // Envelope overlaps (5,5)->(10,0) and (10,0)->(15,5), but stays strictly
  // above y = 2 across x in [8, 12].
  OwnedSegmentSet disjoint_valley_query;
  disjoint_valley_query.Add({{8.0, 5.0}, {8.0, 3.0}, {12.0, 3.0}, {12.0, 5.0}});
  EXPECT_FALSE(finder.intersects(&disjoint_valley_query.ptrs));
}

TEST(FastSegmentSetIntersectionFinderTest, MultiSegmentMonotoneChain) {
  OwnedSegmentSet base;
  base.Add({
      {0.0, 0.0},
      {2.0, 1.0},
      {4.0, 3.0},
      {6.0, 6.0},
      {8.0, 10.0},
  });

  FastSegmentSetIntersectionFinder finder(&base.ptrs);

  // Crosses (2,1)->(4,3) at (3.5, 2.5).
  OwnedSegmentSet intersecting_query;
  intersecting_query.Add({{2.0, 4.0}, {5.0, 1.0}});
  EXPECT_TRUE(finder.intersects(&intersecting_query.ptrs));

  // Envelope [1,7] x [0,2] overlaps the monotone chain envelope, but stays
  // strictly below the chain.
  OwnedSegmentSet disjoint_query;
  disjoint_query.Add({{1.0, 0.0}, {7.0, 0.0}, {7.0, 2.0}});
  EXPECT_FALSE(finder.intersects(&disjoint_query.ptrs));
}

TEST(FastSegmentSetIntersectionFinderTest,
     RepeatedQueriesReuseIndexedBaseSegments) {
  OwnedSegmentSet base;
  base.Add({{0.0, 0.0}, {10.0, 0.0}});
  base.Add({{0.0, 10.0}, {10.0, 10.0}});

  FastSegmentSetIntersectionFinder finder(&base.ptrs);

  OwnedSegmentSet q_between;
  q_between.Add({{2.0, 2.0}, {8.0, 8.0}});
  EXPECT_FALSE(finder.intersects(&q_between.ptrs));

  OwnedSegmentSet q_lower;
  q_lower.Add({{5.0, -1.0}, {5.0, 1.0}});
  EXPECT_TRUE(finder.intersects(&q_lower.ptrs));

  OwnedSegmentSet q_far;
  q_far.Add({{20.0, 0.0}, {30.0, 0.0}});
  EXPECT_FALSE(finder.intersects(&q_far.ptrs));

  OwnedSegmentSet q_upper;
  q_upper.Add({{5.0, 9.0}, {5.0, 11.0}});
  EXPECT_TRUE(finder.intersects(&q_upper.ptrs));
}

TEST(FastSegmentSetIntersectionFinderTest,
     IntersectsWithCustomSegmentIntersectionDetector) {
  OwnedSegmentSet base;
  base.Add({{0.0, 0.0}, {10.0, 0.0}});
  base.Add({{0.0, 10.0}, {10.0, 10.0}});

  FastSegmentSetIntersectionFinder finder(&base.ptrs);
  LineIntersector li;

  // Endpoint touch only: intersects() is true, hasProperIntersection() is
  // false.
  {
    OwnedSegmentSet query;
    query.Add({{0.0, 0.0}, {0.0, 5.0}});
    SegmentIntersectionDetector detector(&li);
    detector.setFindProper(true);

    EXPECT_TRUE(finder.intersects(&query.ptrs, &detector));
    EXPECT_TRUE(detector.hasIntersection());
    EXPECT_FALSE(detector.hasProperIntersection());
    EXPECT_TRUE(detector.hasNonProperIntersection());
    ASSERT_NE(detector.getIntersection(), nullptr);
    EXPECT_TRUE(detector.getIntersection()->equals2D(CoordinateXY(0.0, 0.0)));
    ASSERT_NE(detector.getIntersectionSegments(), nullptr);
    EXPECT_EQ(detector.getIntersectionSegments()->size(), 4u);
  }

  // Both endpoint touch and proper crossing: detector with setFindProper(true)
  // continues past the touch and records the proper intersection at (5, 0).
  {
    OwnedSegmentSet query;
    query.Add({{0.0, 0.0}, {0.0, 5.0}});
    query.Add({{5.0, -5.0}, {5.0, 5.0}});
    SegmentIntersectionDetector detector(&li);
    detector.setFindProper(true);

    EXPECT_TRUE(finder.intersects(&query.ptrs, &detector));
    EXPECT_TRUE(detector.hasIntersection());
    EXPECT_TRUE(detector.hasProperIntersection());
    ASSERT_NE(detector.getIntersection(), nullptr);
    EXPECT_TRUE(detector.getIntersection()->equals2D(CoordinateXY(5.0, 0.0)));
  }

  // With setFindAllIntersectionTypes(true), finding a proper crossing first
  // does not stop early before a subsequent non-proper endpoint touch is found.
  {
    OwnedSegmentSet query;
    query.Add({{5.0, -5.0}, {5.0, 5.0}});
    query.Add({{0.0, 0.0}, {0.0, 5.0}});
    SegmentIntersectionDetector detector(&li);
    detector.setFindAllIntersectionTypes(true);

    EXPECT_TRUE(finder.intersects(&query.ptrs, &detector));
    EXPECT_TRUE(detector.hasIntersection());
    EXPECT_TRUE(detector.hasProperIntersection());
    EXPECT_TRUE(detector.hasNonProperIntersection());
  }

  // Disjoint query leaves detector state clear.
  {
    OwnedSegmentSet query;
    query.Add({{20.0, 0.0}, {20.0, 5.0}});
    SegmentIntersectionDetector detector(&li);

    EXPECT_FALSE(finder.intersects(&query.ptrs, &detector));
    EXPECT_FALSE(detector.hasIntersection());
    EXPECT_FALSE(detector.hasProperIntersection());
    EXPECT_FALSE(detector.hasNonProperIntersection());
    EXPECT_EQ(detector.getIntersection(), nullptr);
    EXPECT_EQ(detector.getIntersectionSegments(), nullptr);
  }
}

TEST(FastSegmentSetIntersectionFinderTest,
     SameAndCopiedSegmentStringIntersection) {
  OwnedSegmentSet single;
  single.Add({{0.0, 0.0}, {10.0, 10.0}});
  FastSegmentSetIntersectionFinder single_finder(&single.ptrs);

  // Testing a single-segment SegmentString against the identical pointer skips
  // self-segment comparison (e0 == e1 && segIndex0 == segIndex1).
  EXPECT_FALSE(single_finder.intersects(&single.ptrs));

  // A distinct SegmentString instance with identical coordinates intersects.
  OwnedSegmentSet single_copy;
  single_copy.Add({{0.0, 0.0}, {10.0, 10.0}});
  EXPECT_TRUE(single_finder.intersects(&single_copy.ptrs));

  // A self-crossing multi-segment string intersects itself at distinct segment
  // indices.
  OwnedSegmentSet bowtie;
  bowtie.Add({{0.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {10.0, 0.0}});
  FastSegmentSetIntersectionFinder bowtie_finder(&bowtie.ptrs);
  EXPECT_TRUE(bowtie_finder.intersects(&bowtie.ptrs));
}

}  // namespace
}  // namespace geos_google
