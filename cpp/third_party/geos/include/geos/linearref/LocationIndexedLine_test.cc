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

#include "third_party/geos/include/geos/linearref/LocationIndexedLine.h"

#include <cmath>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/linearref/LinearLocation.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::linearref::LinearLocation;
using geos::linearref::LocationIndexedLine;
using geos::util::IllegalArgumentException;
using geos::util::IllegalStateException;

class LocationIndexedLineTest : public ::testing::Test {
 protected:
  LocationIndexedLineTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(LocationIndexedLineTest, ConstructWithLinearGeometriesAndEndpoints) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0, 10 10)");
  LocationIndexedLine indexed(line.get());

  LinearLocation start = indexed.getStartIndex();
  EXPECT_EQ(start.getComponentIndex(), 0u);
  EXPECT_EQ(start.getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(start.getSegmentFraction(), 0.0);
  EXPECT_TRUE(indexed.isValidIndex(start));

  LinearLocation end = indexed.getEndIndex();
  EXPECT_EQ(end.getComponentIndex(), 0u);
  EXPECT_EQ(end.getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(end.getSegmentFraction(), 1.0);
  EXPECT_TRUE(indexed.isValidIndex(end));

  auto multi = ReadWkt("MULTILINESTRING ((0 0, 5 0), (10 0, 10 5, 20 5))");
  LocationIndexedLine indexed_multi(multi.get());
  LinearLocation multi_end = indexed_multi.getEndIndex();
  EXPECT_EQ(multi_end.getComponentIndex(), 1u);
  EXPECT_EQ(multi_end.getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(multi_end.getSegmentFraction(), 1.0);
  EXPECT_TRUE(indexed_multi.isValidIndex(indexed_multi.getStartIndex()));
  EXPECT_TRUE(indexed_multi.isValidIndex(multi_end));

  auto empty_line = ReadWkt("LINESTRING EMPTY");
  LocationIndexedLine indexed_empty(empty_line.get());
  EXPECT_TRUE(
      indexed_empty.extractPoint(indexed_empty.getStartIndex()).isNull());

  auto empty_multi = ReadWkt("MULTILINESTRING EMPTY");
  LocationIndexedLine indexed_empty_multi(empty_multi.get());
  EXPECT_EQ(indexed_empty_multi.getEndIndex().getComponentIndex(), 0u);
  EXPECT_EQ(indexed_empty_multi.getEndIndex().getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(indexed_empty_multi.getEndIndex().getSegmentFraction(), 0.0);
  EXPECT_FALSE(
      indexed_empty_multi.isValidIndex(indexed_empty_multi.getStartIndex()));

  auto ring = ReadWkt("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  LocationIndexedLine indexed_ring(ring.get());
  Coordinate ring_mid = indexed_ring.extractPoint(LinearLocation(0, 2, 0.5));
  EXPECT_DOUBLE_EQ(ring_mid.x, 5.0);
  EXPECT_DOUBLE_EQ(ring_mid.y, 10.0);
}

TEST_F(LocationIndexedLineTest, ConstructWithNonLinealGeometryThrows) {
  auto point = ReadWkt("POINT (1 2)");
  EXPECT_THROW((LocationIndexedLine(point.get())), IllegalArgumentException);

  auto multipoint = ReadWkt("MULTIPOINT ((0 0), (1 1))");
  EXPECT_THROW((LocationIndexedLine(multipoint.get())),
               IllegalArgumentException);

  auto polygon = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  EXPECT_THROW((LocationIndexedLine(polygon.get())), IllegalArgumentException);

  auto mixed =
      ReadWkt("GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (0 0, 1 1))");
  EXPECT_THROW((LocationIndexedLine(mixed.get())), IllegalArgumentException);
}

TEST_F(LocationIndexedLineTest,
       ExtractPointVerticesFractionsAndZInterpolation) {
  auto line2d = ReadWkt("LINESTRING (0 0, 10 0, 10 20)");
  LocationIndexedLine indexed2d(line2d.get());

  Coordinate start_pt = indexed2d.extractPoint(indexed2d.getStartIndex());
  EXPECT_DOUBLE_EQ(start_pt.x, 0.0);
  EXPECT_DOUBLE_EQ(start_pt.y, 0.0);
  EXPECT_TRUE(std::isnan(start_pt.z));

  Coordinate mid_seg0 = indexed2d.extractPoint(LinearLocation(0, 0, 0.25));
  EXPECT_DOUBLE_EQ(mid_seg0.x, 2.5);
  EXPECT_DOUBLE_EQ(mid_seg0.y, 0.0);
  EXPECT_TRUE(std::isnan(mid_seg0.z));

  Coordinate vertex1 = indexed2d.extractPoint(LinearLocation(0, 1, 0.0));
  EXPECT_DOUBLE_EQ(vertex1.x, 10.0);
  EXPECT_DOUBLE_EQ(vertex1.y, 0.0);

  Coordinate mid_seg1 = indexed2d.extractPoint(LinearLocation(0, 1, 0.5));
  EXPECT_DOUBLE_EQ(mid_seg1.x, 10.0);
  EXPECT_DOUBLE_EQ(mid_seg1.y, 10.0);

  Coordinate end_pt = indexed2d.extractPoint(indexed2d.getEndIndex());
  EXPECT_DOUBLE_EQ(end_pt.x, 10.0);
  EXPECT_DOUBLE_EQ(end_pt.y, 20.0);

  auto line3d = ReadWkt("LINESTRING Z (0 0 10, 10 0 30)");
  LocationIndexedLine indexed3d(line3d.get());
  Coordinate pt3d = indexed3d.extractPoint(LinearLocation(0, 0, 0.5));
  EXPECT_DOUBLE_EQ(pt3d.x, 5.0);
  EXPECT_DOUBLE_EQ(pt3d.y, 0.0);
  EXPECT_DOUBLE_EQ(pt3d.z, 20.0);

  auto multi = ReadWkt("MULTILINESTRING ((0 0, 10 0), (20 5, 30 5))");
  LocationIndexedLine indexed_multi(multi.get());
  Coordinate comp1_mid = indexed_multi.extractPoint(LinearLocation(1, 0, 0.4));
  EXPECT_DOUBLE_EQ(comp1_mid.x, 24.0);
  EXPECT_DOUBLE_EQ(comp1_mid.y, 5.0);
}

TEST_F(LocationIndexedLineTest, ExtractPointWithOffset) {
  auto line = ReadWkt("LINESTRING (0 0, 3 4)");
  LocationIndexedLine indexed(line.get());

  // Zero offset stays on the segment.
  Coordinate on_line = indexed.extractPoint(LinearLocation(0, 0, 0.5), 0.0);
  EXPECT_DOUBLE_EQ(on_line.x, 1.5);
  EXPECT_DOUBLE_EQ(on_line.y, 2.0);

  // Segment direction is (3, 4) with length 5; +5 offsets left by (-4, 3).
  Coordinate left_start = indexed.extractPoint(LinearLocation(0, 0, 0.0), 5.0);
  EXPECT_DOUBLE_EQ(left_start.x, -4.0);
  EXPECT_DOUBLE_EQ(left_start.y, 3.0);

  // Negative offset goes to the right by (4, -3).
  Coordinate right_start =
      indexed.extractPoint(LinearLocation(0, 0, 0.0), -5.0);
  EXPECT_DOUBLE_EQ(right_start.x, 4.0);
  EXPECT_DOUBLE_EQ(right_start.y, -3.0);

  // End index offsets from the end vertex along the last segment.
  Coordinate left_end = indexed.extractPoint(indexed.getEndIndex(), 5.0);
  EXPECT_DOUBLE_EQ(left_end.x, -1.0);
  EXPECT_DOUBLE_EQ(left_end.y, 7.0);

  // MultiLineString second component offset.
  auto multi = ReadWkt("MULTILINESTRING ((0 0, 1 0), (10 0, 10 10))");
  LocationIndexedLine indexed_multi(multi.get());
  Coordinate comp1_left =
      indexed_multi.extractPoint(LinearLocation(1, 0, 0.5), 2.0);
  EXPECT_DOUBLE_EQ(comp1_left.x, 8.0);
  EXPECT_DOUBLE_EQ(comp1_left.y, 5.0);

  // Zero-length segment fails when offset is non-zero, succeeds when 0.
  auto degenerate = ReadWkt("LINESTRING (1 2, 1 2)");
  LocationIndexedLine indexed_deg(degenerate.get());
  Coordinate deg_zero =
      indexed_deg.extractPoint(LinearLocation(0, 0, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(deg_zero.x, 1.0);
  EXPECT_DOUBLE_EQ(deg_zero.y, 2.0);
  EXPECT_THROW(indexed_deg.extractPoint(LinearLocation(0, 0, 0.0), 1.0),
               IllegalStateException);
}

TEST_F(LocationIndexedLineTest, ExtractLineForwardReversedAndMultiLineString) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0, 10 10, 20 10)");
  LocationIndexedLine indexed(line.get());

  LinearLocation start(0, 0, 0.5);  // (5, 0)
  LinearLocation end(0, 2, 0.5);    // (15, 10)

  auto forward = indexed.extractLine(start, end);
  auto expected_forward = ReadWkt("LINESTRING (5 0, 10 0, 10 10, 15 10)");
  EXPECT_TRUE(forward->equalsExact(expected_forward.get()));

  auto reversed = indexed.extractLine(end, start);
  auto expected_reversed = ReadWkt("LINESTRING (15 10, 10 10, 10 0, 5 0)");
  EXPECT_TRUE(reversed->equalsExact(expected_reversed.get()));

  auto point_subline = indexed.extractLine(start, start);
  auto expected_zero_len = ReadWkt("LINESTRING (5 0, 5 0)");
  EXPECT_TRUE(point_subline->equalsExact(expected_zero_len.get()));

  auto full_line =
      indexed.extractLine(indexed.getStartIndex(), indexed.getEndIndex());
  EXPECT_TRUE(full_line->equalsExact(line.get())) << full_line->toText();

  auto vertex_seg =
      indexed.extractLine(LinearLocation(0, 1, 0.0), LinearLocation(0, 2, 0.0));
  auto expected_vertex_seg = ReadWkt("LINESTRING (10 0, 10 10)");
  EXPECT_TRUE(vertex_seg->equalsExact(expected_vertex_seg.get()))
      << vertex_seg->toText();

  auto multi = ReadWkt("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0))");
  LocationIndexedLine indexed_multi(multi.get());
  auto multi_sub = indexed_multi.extractLine(LinearLocation(0, 0, 0.5),
                                             LinearLocation(1, 0, 0.5));
  auto expected_multi_sub =
      ReadWkt("MULTILINESTRING ((5 0, 10 0), (20 0, 25 0))");
  EXPECT_TRUE(multi_sub->equalsExact(expected_multi_sub.get()));

  auto multi_rev = indexed_multi.extractLine(LinearLocation(1, 0, 0.5),
                                             LinearLocation(0, 0, 0.5));
  // ExtractLineByLocation::reverse delegates to MultiLineString::reverseImpl,
  // which reverses each component's coordinates while preserving component
  // order.
  auto expected_multi_rev =
      ReadWkt("MULTILINESTRING ((10 0, 5 0), (25 0, 20 0))");
  EXPECT_TRUE(multi_rev->equalsExact(expected_multi_rev.get()))
      << multi_rev->toText();

  auto single_comp_fwd = indexed_multi.extractLine(LinearLocation(1, 0, 0.2),
                                                   LinearLocation(1, 0, 0.8));
  auto expected_single_comp_fwd = ReadWkt("LINESTRING (22 0, 28 0)");
  EXPECT_TRUE(single_comp_fwd->equalsExact(expected_single_comp_fwd.get()))
      << single_comp_fwd->toText();

  auto single_comp_rev = indexed_multi.extractLine(LinearLocation(1, 0, 0.8),
                                                   LinearLocation(1, 0, 0.2));
  auto expected_single_comp_rev = ReadWkt("LINESTRING (28 0, 22 0)");
  EXPECT_TRUE(single_comp_rev->equalsExact(expected_single_comp_rev.get()))
      << single_comp_rev->toText();
}

TEST_F(LocationIndexedLineTest, IndexOfAndProject) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0, 10 10)");
  LocationIndexedLine indexed(line.get());

  LinearLocation loc_start = indexed.indexOf(Coordinate(0.0, 0.0));
  EXPECT_EQ(loc_start.compareTo(LinearLocation(0, 0, 0.0)), 0);

  LinearLocation loc_mid = indexed.indexOf(Coordinate(4.0, 0.0));
  EXPECT_EQ(loc_mid.getComponentIndex(), 0u);
  EXPECT_EQ(loc_mid.getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(loc_mid.getSegmentFraction(), 0.4);

  LinearLocation loc_seg1 = indexed.indexOf(Coordinate(10.0, 7.0));
  EXPECT_EQ(loc_seg1.getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(loc_seg1.getSegmentFraction(), 0.7);

  // Off-line projection onto nearest segment.
  LinearLocation projected = indexed.project(Coordinate(3.0, 2.0));
  EXPECT_EQ(projected.getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(projected.getSegmentFraction(), 0.3);

  // Equidistant point (5, 5) to (5, 0) on seg0 and (10, 5) on seg1 returns the
  // earliest location along the line.
  LinearLocation tie = indexed.project(Coordinate(5.0, 5.0));
  EXPECT_EQ(tie.getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(tie.getSegmentFraction(), 0.5);

  auto multi = ReadWkt("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0))");
  LocationIndexedLine indexed_multi(multi.get());
  LinearLocation multi_proj = indexed_multi.project(Coordinate(26.0, 3.0));
  EXPECT_EQ(multi_proj.getComponentIndex(), 1u);
  EXPECT_EQ(multi_proj.getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(multi_proj.getSegmentFraction(), 0.6);
}

TEST_F(LocationIndexedLineTest, IndexOfAfterDisambiguatesAndHandlesBoundaries) {
  // Path passes through (5, 0) twice: along seg 0 and at vertex 3.
  auto line = ReadWkt("LINESTRING (0 0, 10 0, 10 10, 5 0, 10 -10)");
  LocationIndexedLine indexed(line.get());

  LinearLocation first = indexed.indexOf(Coordinate(5.0, 0.0));
  EXPECT_EQ(first.getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(first.getSegmentFraction(), 0.5);

  LinearLocation second = indexed.indexOfAfter(Coordinate(5.0, 0.0), first);
  EXPECT_GT(second.compareTo(first), 0);
  Coordinate extracted = indexed.extractPoint(second);
  EXPECT_DOUBLE_EQ(extracted.x, 5.0);
  EXPECT_DOUBLE_EQ(extracted.y, 0.0);

  // When minIndex is at or past the end, getEndIndex() is returned.
  LinearLocation at_end =
      indexed.indexOfAfter(Coordinate(5.0, 0.0), indexed.getEndIndex());
  EXPECT_EQ(at_end.compareTo(indexed.getEndIndex()), 0);

  // When no point on the single segment exists after minIndex, throws.
  auto single_seg = ReadWkt("LINESTRING (0 0, 10 0)");
  LocationIndexedLine indexed_single(single_seg.get());
  EXPECT_THROW(indexed_single.indexOfAfter(Coordinate(2.0, 0.0),
                                           LinearLocation(0, 0, 0.8)),
               IllegalArgumentException);
}

TEST_F(LocationIndexedLineTest, IndicesOfSubLine) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0, 10 10, 20 10)");
  LocationIndexedLine indexed(line.get());

  auto subline = ReadWkt("LINESTRING (3 0, 10 0, 10 10, 14 10)");
  std::unique_ptr<LinearLocation[]> locs(indexed.indicesOf(subline.get()));
  ASSERT_NE(locs, nullptr);
  EXPECT_EQ(locs[0].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(locs[0].getSegmentFraction(), 0.3);
  EXPECT_EQ(locs[1].getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(locs[1].getSegmentFraction(), 0.4);

  auto round_trip = indexed.extractLine(locs[0], locs[1]);
  EXPECT_TRUE(round_trip->equalsExact(subline.get()));

  auto zero_len_sub = ReadWkt("LINESTRING (10 4, 10 4)");
  std::unique_ptr<LinearLocation[]> zero_locs(
      indexed.indicesOf(zero_len_sub.get()));
  EXPECT_EQ(zero_locs[0].compareTo(zero_locs[1]), 0);
  EXPECT_EQ(zero_locs[0].getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(zero_locs[0].getSegmentFraction(), 0.4);

  auto point = ReadWkt("POINT (5 0)");
  EXPECT_THROW(
      ([&] {
        std::unique_ptr<LinearLocation[]> bad(indexed.indicesOf(point.get()));
      }()),
      IllegalArgumentException);

  auto multi = ReadWkt("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0))");
  LocationIndexedLine indexed_multi(multi.get());
  auto multi_sub = ReadWkt("MULTILINESTRING ((4 0, 10 0), (20 0, 27 0))");
  std::unique_ptr<LinearLocation[]> multi_locs(
      indexed_multi.indicesOf(multi_sub.get()));
  ASSERT_NE(multi_locs, nullptr);
  EXPECT_EQ(multi_locs[0].getComponentIndex(), 0u);
  EXPECT_DOUBLE_EQ(multi_locs[0].getSegmentFraction(), 0.4);
  EXPECT_EQ(multi_locs[1].getComponentIndex(), 1u);
  EXPECT_DOUBLE_EQ(multi_locs[1].getSegmentFraction(), 0.7);
}

TEST_F(LocationIndexedLineTest, IsValidIndexAndClampIndex) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0, 10 10)");
  LocationIndexedLine indexed(line.get());

  EXPECT_TRUE(indexed.isValidIndex(LinearLocation(0, 0, 0.0)));
  EXPECT_TRUE(indexed.isValidIndex(LinearLocation(0, 1, 0.5)));
  EXPECT_TRUE(indexed.isValidIndex(LinearLocation(0, 2, 1.0)));
  EXPECT_TRUE(indexed.isValidIndex(indexed.getEndIndex()));

  // Invalid component index, segment index, and unnormalized fractions.
  EXPECT_FALSE(indexed.isValidIndex(LinearLocation(1, 0, 0.0)));
  EXPECT_FALSE(indexed.isValidIndex(LinearLocation(0, 9, 0.0)));
  EXPECT_FALSE(indexed.isValidIndex(LinearLocation(0, 3, 0.5)));
  EXPECT_FALSE(indexed.isValidIndex(LinearLocation(3, 0.5)));
  EXPECT_FALSE(indexed.isValidIndex(LinearLocation(0, -0.5)));
  EXPECT_FALSE(indexed.isValidIndex(LinearLocation(0, 1.5)));

  LinearLocation valid_loc(0, 1, 0.25);
  LinearLocation clamped_valid = indexed.clampIndex(valid_loc);
  EXPECT_EQ(clamped_valid.compareTo(valid_loc), 0);

  LinearLocation clamped_comp = indexed.clampIndex(LinearLocation(5, 0, 0.0));
  EXPECT_TRUE(indexed.isValidIndex(clamped_comp));
  EXPECT_EQ(clamped_comp.compareTo(indexed.getEndIndex()), 0);

  LinearLocation clamped_seg = indexed.clampIndex(LinearLocation(0, 42, 0.0));
  EXPECT_TRUE(indexed.isValidIndex(clamped_seg));
  EXPECT_EQ(clamped_seg.compareTo(indexed.getEndIndex()), 0);
  Coordinate end_pt = indexed.extractPoint(clamped_seg);
  EXPECT_DOUBLE_EQ(end_pt.x, 10.0);
  EXPECT_DOUBLE_EQ(end_pt.y, 10.0);
}

}  // namespace
}  // namespace geos_google
