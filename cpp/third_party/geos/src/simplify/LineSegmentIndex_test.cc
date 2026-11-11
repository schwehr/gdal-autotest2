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

#include "third_party/geos/include/geos/simplify/LineSegmentIndex.h"

#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/simplify/TaggedLineSegment.h"
#include "third_party/geos/include/geos/simplify/TaggedLineString.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::LineSegment;
using geos::simplify::LineSegmentIndex;
using geos::simplify::TaggedLineSegment;
using geos::simplify::TaggedLineString;
using testing::IsEmpty;
using testing::UnorderedElementsAre;

TEST(LineSegmentIndexTest, QueryEmptyIndexReturnsEmpty) {
  LineSegmentIndex index;
  const LineSegment query_seg(0.0, 0.0, 10.0, 10.0);
  const LineSegment point_seg(5.0, 5.0, 5.0, 5.0);

  EXPECT_THAT(index.query(&query_seg), IsEmpty());
  EXPECT_THAT(index.query(&point_seg), IsEmpty());
}

TEST(LineSegmentIndexTest, AddSingleSegmentAndQueryOverlapsAndDisjoint) {
  LineSegmentIndex index;
  const LineSegment seg(0.0, 0.0, 10.0, 10.0);
  index.add(&seg);

  const LineSegment identical(0.0, 0.0, 10.0, 10.0);
  const LineSegment reversed(10.0, 10.0, 0.0, 0.0);
  const LineSegment crossing(0.0, 10.0, 10.0, 0.0);
  const LineSegment touching_corner(10.0, 10.0, 20.0, 20.0);
  const LineSegment touching_edge(5.0, 10.0, 5.0, 20.0);
  const LineSegment interior_point(5.0, 5.0, 5.0, 5.0);

  EXPECT_THAT(index.query(&identical), UnorderedElementsAre(&seg));
  EXPECT_THAT(index.query(&reversed), UnorderedElementsAre(&seg));
  EXPECT_THAT(index.query(&crossing), UnorderedElementsAre(&seg));
  EXPECT_THAT(index.query(&touching_corner), UnorderedElementsAre(&seg));
  EXPECT_THAT(index.query(&touching_edge), UnorderedElementsAre(&seg));
  EXPECT_THAT(index.query(&interior_point), UnorderedElementsAre(&seg));

  const LineSegment disjoint_right(10.5, 0.0, 20.0, 10.0);
  const LineSegment disjoint_above(0.0, 10.5, 10.0, 20.0);
  EXPECT_THAT(index.query(&disjoint_right), IsEmpty());
  EXPECT_THAT(index.query(&disjoint_above), IsEmpty());
}

TEST(LineSegmentIndexTest,
     QueryUsesEnvelopeIntersectionRatherThanGeometryIntersection) {
  LineSegmentIndex index;
  const LineSegment diagonal(0.0, 0.0, 10.0, 10.0);
  index.add(&diagonal);

  // Geometrically disjoint from (0,0)-(10,10), but bounding boxes overlap.
  const LineSegment corner_seg(0.0, 9.0, 1.0, 10.0);
  EXPECT_THAT(index.query(&corner_seg), UnorderedElementsAre(&diagonal));
}

TEST(LineSegmentIndexTest, QueryFiltersNonIntersectingCandidatesFromQuadtree) {
  LineSegmentIndex index;
  // Crosses both axes so Quadtree stores it in the root node, which is visited
  // for every query regardless of query bounds.
  const LineSegment root_seg(-5.0, -5.0, 5.0, 5.0);
  const LineSegment quad_a(1.0, 1.0, 2.0, 2.0);
  const LineSegment quad_b(10.0, 10.0, 11.0, 11.0);
  const LineSegment spanning(1.5, 1.5, 10.5, 10.5);

  index.add(&root_seg);
  index.add(&quad_a);
  index.add(&quad_b);
  index.add(&spanning);

  const LineSegment query_high(10.0, 10.0, 12.0, 12.0);
  EXPECT_THAT(index.query(&query_high),
              UnorderedElementsAre(&quad_b, &spanning));

  const LineSegment query_far(20.0, 20.0, 25.0, 25.0);
  EXPECT_THAT(index.query(&query_far), IsEmpty());

  const LineSegment query_negative(-5.0, -5.0, -4.0, -4.0);
  EXPECT_THAT(index.query(&query_negative), UnorderedElementsAre(&root_seg));
}

TEST(LineSegmentIndexTest,
     AddAndRemoveHorizontalVerticalAndZeroLengthSegments) {
  LineSegmentIndex index;
  const LineSegment horiz(1.0, 5.0, 8.0, 5.0);
  const LineSegment vert(4.0, 1.0, 4.0, 9.0);
  const LineSegment point_seg(20.0, 20.0, 20.0, 20.0);

  index.add(&horiz);
  index.add(&vert);
  index.add(&point_seg);

  const LineSegment cross_hv(3.0, 4.0, 5.0, 6.0);
  EXPECT_THAT(index.query(&cross_hv), UnorderedElementsAre(&horiz, &vert));

  const LineSegment near_horiz(1.0, 5.1, 8.0, 5.1);
  EXPECT_THAT(index.query(&near_horiz), UnorderedElementsAre(&vert));

  const LineSegment hit_point(19.0, 19.0, 21.0, 21.0);
  EXPECT_THAT(index.query(&hit_point), UnorderedElementsAre(&point_seg));

  index.remove(&horiz);
  index.remove(&point_seg);

  EXPECT_THAT(index.query(&cross_hv), UnorderedElementsAre(&vert));
  EXPECT_THAT(index.query(&hit_point), IsEmpty());

  index.remove(&vert);
  EXPECT_THAT(index.query(&cross_hv), IsEmpty());
}

TEST(LineSegmentIndexTest, RemoveRequiresMatchingPointerIdentity) {
  LineSegmentIndex index;
  const LineSegment seg_a(1.0, 1.0, 4.0, 4.0);
  const LineSegment seg_b(1.0, 1.0, 4.0, 4.0);
  const LineSegment seg_c(3.0, 3.0, 6.0, 6.0);
  const LineSegment unindexed_same_coords(1.0, 1.0, 4.0, 4.0);
  const LineSegment unindexed_far(50.0, 50.0, 51.0, 51.0);

  index.add(&seg_a);
  index.add(&seg_b);
  index.add(&seg_c);

  const LineSegment query_mid(2.0, 2.0, 3.5, 3.5);
  EXPECT_THAT(index.query(&query_mid),
              UnorderedElementsAre(&seg_a, &seg_b, &seg_c));

  // Removing a distinct LineSegment instance at the same coordinates must not
  // remove seg_a or seg_b.
  index.remove(&unindexed_same_coords);
  index.remove(&unindexed_far);
  EXPECT_THAT(index.query(&query_mid),
              UnorderedElementsAre(&seg_a, &seg_b, &seg_c));

  index.remove(&seg_a);
  EXPECT_THAT(index.query(&query_mid), UnorderedElementsAre(&seg_b, &seg_c));

  // Removing an already-removed segment is a safe no-op.
  index.remove(&seg_a);
  EXPECT_THAT(index.query(&query_mid), UnorderedElementsAre(&seg_b, &seg_c));

  index.remove(&seg_b);
  index.remove(&seg_c);
  EXPECT_THAT(index.query(&query_mid), IsEmpty());
}

TEST(LineSegmentIndexTest, AddTaggedLineStringIndexesAllSegments) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence coords{
      Coordinate{0.0, 0.0},
      Coordinate{10.0, 0.0},
      Coordinate{10.0, 10.0},
      Coordinate{20.0, 10.0},
  };
  auto line = factory->createLineString(coords);
  TaggedLineString tagged_line(line.get(), 2, false);

  LineSegmentIndex index;
  index.add(tagged_line);

  const LineSegment query_corner(9.0, -1.0, 11.0, 1.0);
  auto hits = index.query(&query_corner);
  EXPECT_THAT(hits, UnorderedElementsAre(tagged_line.getSegment(0),
                                         tagged_line.getSegment(1)));

  const TaggedLineSegment* first_tagged = tagged_line.getSegment(0);
  EXPECT_EQ(first_tagged->getParent(), line.get());
  EXPECT_EQ(first_tagged->getIndex(), 0u);

  const LineSegment query_all(-1.0, -1.0, 21.0, 11.0);
  EXPECT_THAT(
      index.query(&query_all),
      UnorderedElementsAre(tagged_line.getSegment(0), tagged_line.getSegment(1),
                           tagged_line.getSegment(2)));

  index.remove(tagged_line.getSegment(1));
  EXPECT_THAT(index.query(&query_corner),
              UnorderedElementsAre(tagged_line.getSegment(0)));
}

TEST(LineSegmentIndexTest, AddEmptyTaggedLineStringIsNoOp) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto empty_line = factory->createLineString();
  TaggedLineString tagged_empty(empty_line.get(), 2, false);

  LineSegmentIndex index;
  index.add(tagged_empty);

  const LineSegment wide_query(-100.0, -100.0, 100.0, 100.0);
  EXPECT_THAT(index.query(&wide_query), IsEmpty());
}

TEST(LineSegmentIndexTest, NonFiniteCoordinatesThrowOnAdd) {
  LineSegmentIndex index;
  const LineSegment valid_seg(0.0, 0.0, 5.0, 5.0);
  index.add(&valid_seg);

  const LineSegment nan_seg(0.0, 0.0, geos::DoubleNotANumber, 1.0);
  const LineSegment inf_seg(0.0, 0.0, geos::DoubleInfinity, 1.0);

  EXPECT_THROW(index.add(&nan_seg), geos::util::IllegalArgumentException);
  EXPECT_THROW(index.add(&inf_seg), geos::util::IllegalArgumentException);
  EXPECT_THAT(index.query(&inf_seg), UnorderedElementsAre(&valid_seg));
}

}  // namespace
}  // namespace geos_google
