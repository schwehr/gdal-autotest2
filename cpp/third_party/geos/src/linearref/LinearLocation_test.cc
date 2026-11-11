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

#include "third_party/geos/include/geos/linearref/LinearLocation.h"

#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineSegment;
using geos::io::WKTReader;
using geos::linearref::LinearLocation;
using geos::util::IllegalArgumentException;

class LinearLocationTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};

  std::unique_ptr<Geometry> ReadWKT(const std::string& wkt) const {
    return reader_.read(wkt);
  }
};

TEST_F(LinearLocationTest, ConstructorsAndGetters) {
  LinearLocation default_loc;
  EXPECT_EQ(default_loc.getComponentIndex(), 0u);
  EXPECT_EQ(default_loc.getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(default_loc.getSegmentFraction(), 0.0);

  // Two-argument constructor keeps segmentFraction unnormalized.
  LinearLocation seg_loc(2, 0.5);
  EXPECT_EQ(seg_loc.getComponentIndex(), 0u);
  EXPECT_EQ(seg_loc.getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(seg_loc.getSegmentFraction(), 0.5);

  LinearLocation neg_unnorm(1, -0.5);
  EXPECT_EQ(neg_unnorm.getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(neg_unnorm.getSegmentFraction(), -0.5);

  LinearLocation one_unnorm(1, 1.0);
  EXPECT_EQ(one_unnorm.getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(one_unnorm.getSegmentFraction(), 1.0);

  LinearLocation gt_one_unnorm(1, 1.75);
  EXPECT_EQ(gt_one_unnorm.getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(gt_one_unnorm.getSegmentFraction(), 1.75);

  // Three-argument constructor calls normalize().
  LinearLocation in_range(1, 2, 0.25);
  EXPECT_EQ(in_range.getComponentIndex(), 1u);
  EXPECT_EQ(in_range.getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(in_range.getSegmentFraction(), 0.25);

  LinearLocation clamped_low(1, 2, -0.5);
  EXPECT_EQ(clamped_low.getComponentIndex(), 1u);
  EXPECT_EQ(clamped_low.getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(clamped_low.getSegmentFraction(), 0.0);

  LinearLocation exact_one(1, 2, 1.0);
  EXPECT_EQ(exact_one.getComponentIndex(), 1u);
  EXPECT_EQ(exact_one.getSegmentIndex(), 3u);
  EXPECT_DOUBLE_EQ(exact_one.getSegmentFraction(), 0.0);

  LinearLocation clamped_high(1, 2, 1.5);
  EXPECT_EQ(clamped_high.getComponentIndex(), 1u);
  EXPECT_EQ(clamped_high.getSegmentIndex(), 3u);
  EXPECT_DOUBLE_EQ(clamped_high.getSegmentFraction(), 0.0);
}

TEST_F(LinearLocationTest, PointAlongSegmentByFraction) {
  const Coordinate p0(0.0, 10.0, 100.0);
  const Coordinate p1(20.0, 50.0, 200.0);

  Coordinate c_neg = LinearLocation::pointAlongSegmentByFraction(p0, p1, -0.25);
  EXPECT_TRUE(c_neg.equals3D(p0));

  Coordinate c_zero = LinearLocation::pointAlongSegmentByFraction(p0, p1, 0.0);
  EXPECT_TRUE(c_zero.equals3D(p0));

  Coordinate c_one = LinearLocation::pointAlongSegmentByFraction(p0, p1, 1.0);
  EXPECT_TRUE(c_one.equals3D(p1));

  Coordinate c_gt_one =
      LinearLocation::pointAlongSegmentByFraction(p0, p1, 1.5);
  EXPECT_TRUE(c_gt_one.equals3D(p1));

  Coordinate c_quarter =
      LinearLocation::pointAlongSegmentByFraction(p0, p1, 0.25);
  EXPECT_DOUBLE_EQ(c_quarter.x, 5.0);
  EXPECT_DOUBLE_EQ(c_quarter.y, 20.0);
  EXPECT_DOUBLE_EQ(c_quarter.z, 125.0);

  // 2D points (NaN Z) produce NaN Z.
  const Coordinate p2d_0(1.0, 2.0);
  const Coordinate p2d_1(5.0, 10.0);
  Coordinate c_2d =
      LinearLocation::pointAlongSegmentByFraction(p2d_0, p2d_1, 0.5);
  EXPECT_DOUBLE_EQ(c_2d.x, 3.0);
  EXPECT_DOUBLE_EQ(c_2d.y, 6.0);
  EXPECT_TRUE(std::isnan(c_2d.z));

  // Mixed Z (one NaN) also produces NaN Z.
  Coordinate c_mixed =
      LinearLocation::pointAlongSegmentByFraction(p0, p2d_1, 0.5);
  EXPECT_DOUBLE_EQ(c_mixed.x, 2.5);
  EXPECT_DOUBLE_EQ(c_mixed.y, 10.0);
  EXPECT_TRUE(std::isnan(c_mixed.z));

  Coordinate c_mixed_rev =
      LinearLocation::pointAlongSegmentByFraction(p2d_0, p1, 0.5);
  EXPECT_DOUBLE_EQ(c_mixed_rev.x, 10.5);
  EXPECT_DOUBLE_EQ(c_mixed_rev.y, 26.0);
  EXPECT_TRUE(std::isnan(c_mixed_rev.z));

  Coordinate c_2d_zero =
      LinearLocation::pointAlongSegmentByFraction(p2d_0, p1, 0.0);
  EXPECT_DOUBLE_EQ(c_2d_zero.x, 1.0);
  EXPECT_DOUBLE_EQ(c_2d_zero.y, 2.0);
  EXPECT_TRUE(std::isnan(c_2d_zero.z));

  Coordinate c_2d_one =
      LinearLocation::pointAlongSegmentByFraction(p0, p2d_1, 1.0);
  EXPECT_DOUBLE_EQ(c_2d_one.x, 5.0);
  EXPECT_DOUBLE_EQ(c_2d_one.y, 10.0);
  EXPECT_TRUE(std::isnan(c_2d_one.z));
}

TEST_F(LinearLocationTest, SetToEndAndGetEndLocation) {
  auto line = ReadWKT("LINESTRING (0 0, 10 0, 10 10)");
  LinearLocation end = LinearLocation::getEndLocation(line.get());
  EXPECT_EQ(end.getComponentIndex(), 0u);
  EXPECT_EQ(end.getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(end.getSegmentFraction(), 1.0);

  LinearLocation loc(0, 0, 0.5);
  loc.setToEnd(line.get());
  EXPECT_EQ(loc.compareTo(end), 0);

  auto mls = ReadWKT("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0, 40 0, 50 0))");
  LinearLocation mls_end = LinearLocation::getEndLocation(mls.get());
  EXPECT_EQ(mls_end.getComponentIndex(), 1u);
  EXPECT_EQ(mls_end.getSegmentIndex(), 3u);
  EXPECT_DOUBLE_EQ(mls_end.getSegmentFraction(), 1.0);

  auto ring = ReadWKT("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  LinearLocation ring_end = LinearLocation::getEndLocation(ring.get());
  EXPECT_EQ(ring_end.getComponentIndex(), 0u);
  EXPECT_EQ(ring_end.getSegmentIndex(), 4u);
  EXPECT_DOUBLE_EQ(ring_end.getSegmentFraction(), 1.0);

  auto empty_mls = ReadWKT("MULTILINESTRING EMPTY");
  LinearLocation empty_loc(2, 3, 0.5);
  empty_loc.setToEnd(empty_mls.get());
  EXPECT_EQ(empty_loc.getComponentIndex(), 0u);
  EXPECT_EQ(empty_loc.getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(empty_loc.getSegmentFraction(), 0.0);

  auto empty_gc = ReadWKT("GEOMETRYCOLLECTION EMPTY");
  LinearLocation gc_end = LinearLocation::getEndLocation(empty_gc.get());
  EXPECT_EQ(gc_end.getComponentIndex(), 0u);
  EXPECT_EQ(gc_end.getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(gc_end.getSegmentFraction(), 0.0);

  auto pt = ReadWKT("POINT (1 2)");
  EXPECT_THROW(LinearLocation::getEndLocation(pt.get()),
               IllegalArgumentException);

  auto poly = ReadWKT("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  EXPECT_THROW(loc.setToEnd(poly.get()), IllegalArgumentException);

  std::vector<std::unique_ptr<Geometry>> gc_line_last;
  gc_line_last.push_back(ReadWKT("POINT (1 2)"));
  gc_line_last.push_back(ReadWKT("LINESTRING (0 0, 5 0, 10 0)"));
  auto gc_ok = factory_->createGeometryCollection(std::move(gc_line_last));
  LinearLocation gc_ok_end = LinearLocation::getEndLocation(gc_ok.get());
  EXPECT_EQ(gc_ok_end.getComponentIndex(), 1u);
  EXPECT_EQ(gc_ok_end.getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(gc_ok_end.getSegmentFraction(), 1.0);

  std::vector<std::unique_ptr<Geometry>> gc_pt_last;
  gc_pt_last.push_back(ReadWKT("LINESTRING (0 0, 5 0)"));
  gc_pt_last.push_back(ReadWKT("POINT (1 2)"));
  auto gc_bad = factory_->createGeometryCollection(std::move(gc_pt_last));
  EXPECT_THROW(LinearLocation::getEndLocation(gc_bad.get()),
               IllegalArgumentException);
}

TEST_F(LinearLocationTest, Clamp) {
  auto mls = ReadWKT("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0, 40 0))");

  // componentIndex >= numGeometries -> setToEnd
  LinearLocation comp_oob(2, 0, 0.3);
  comp_oob.clamp(mls.get());
  EXPECT_EQ(comp_oob.getComponentIndex(), 1u);
  EXPECT_EQ(comp_oob.getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(comp_oob.getSegmentFraction(), 1.0);

  // segmentIndex >= total points (5) -> clamp to last vertex of component 0.
  LinearLocation seg_oob(0, 5, 0.4);
  seg_oob.clamp(mls.get());
  EXPECT_EQ(seg_oob.getComponentIndex(), 0u);
  EXPECT_EQ(seg_oob.getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(seg_oob.getSegmentFraction(), 1.0);

  // Valid in-range location remains unchanged.
  LinearLocation valid_loc(1, 1, 0.4);
  valid_loc.clamp(mls.get());
  EXPECT_EQ(valid_loc.getComponentIndex(), 1u);
  EXPECT_EQ(valid_loc.getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(valid_loc.getSegmentFraction(), 0.4);

  auto line = ReadWKT("LINESTRING (0 0, 5 0, 10 0)");
  LinearLocation line_comp_oob(1, 0, 0.5);
  line_comp_oob.clamp(line.get());
  EXPECT_EQ(line_comp_oob.getComponentIndex(), 0u);
  EXPECT_EQ(line_comp_oob.getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(line_comp_oob.getSegmentFraction(), 1.0);

  LinearLocation line_seg_oob(0, 3, 0.2);
  line_seg_oob.clamp(line.get());
  EXPECT_EQ(line_seg_oob.getComponentIndex(), 0u);
  EXPECT_EQ(line_seg_oob.getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(line_seg_oob.getSegmentFraction(), 1.0);

  // Empty MultiLineString clamps to (0, 0, 0).
  auto empty_mls = ReadWKT("MULTILINESTRING EMPTY");
  LinearLocation empty_clamp(0, 1, 0.5);
  empty_clamp.clamp(empty_mls.get());
  EXPECT_EQ(empty_clamp.getComponentIndex(), 0u);
  EXPECT_EQ(empty_clamp.getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(empty_clamp.getSegmentFraction(), 0.0);

  // Non-LineString geometry throws when segmentIndex >= getNumPoints().
  auto pt = ReadWKT("POINT (1 2)");
  LinearLocation pt_loc(0, 1, 0.5);
  EXPECT_THROW(pt_loc.clamp(pt.get()), IllegalArgumentException);

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(ReadWKT("POINT (1 2)"));
  auto gc = factory_->createGeometryCollection(std::move(geoms));
  LinearLocation gc_loc(0, 2, 0.5);
  EXPECT_THROW(gc_loc.clamp(gc.get()), IllegalArgumentException);
}

TEST_F(LinearLocationTest, SnapToVertex) {
  auto line = ReadWKT("LINESTRING (0 0, 10 0, 10 20)");

  // Locations already at or outside [0, 1] are untouched.
  LinearLocation at_zero(0, 0, 0.0);
  at_zero.snapToVertex(line.get(), 5.0);
  EXPECT_DOUBLE_EQ(at_zero.getSegmentFraction(), 0.0);

  LinearLocation neg_frac(0, -0.2);
  neg_frac.snapToVertex(line.get(), 5.0);
  EXPECT_DOUBLE_EQ(neg_frac.getSegmentFraction(), -0.2);

  LinearLocation at_one(0, 1.0);
  at_one.snapToVertex(line.get(), 5.0);
  EXPECT_DOUBLE_EQ(at_one.getSegmentFraction(), 1.0);

  LinearLocation gt_one(0, 1.4);
  gt_one.snapToVertex(line.get(), 5.0);
  EXPECT_DOUBLE_EQ(gt_one.getSegmentFraction(), 1.4);

  // Snap to start of segment (lenToStart = 1.0 < 2.0).
  LinearLocation near_start(0, 0, 0.1);
  near_start.snapToVertex(line.get(), 2.0);
  EXPECT_DOUBLE_EQ(near_start.getSegmentFraction(), 0.0);

  // Exact midpoint tie (lenToStart == lenToEnd == 5.0 < 6.0) snaps to 0.0.
  LinearLocation midpoint(0, 0, 0.5);
  midpoint.snapToVertex(line.get(), 6.0);
  EXPECT_DOUBLE_EQ(midpoint.getSegmentFraction(), 0.0);

  // Snap to end of segment 1 (len 20, lenToEnd = 1.0 < 2.0).
  LinearLocation near_end(0, 1, 0.95);
  near_end.snapToVertex(line.get(), 2.0);
  EXPECT_DOUBLE_EQ(near_end.getSegmentFraction(), 1.0);

  // Strict '<' comparison: distance == minDistance does not snap.
  LinearLocation exact_thresh(0, 0, 0.5);
  exact_thresh.snapToVertex(line.get(), 5.0);
  EXPECT_DOUBLE_EQ(exact_thresh.getSegmentFraction(), 0.5);

  // Outside tolerance at both ends: remains unchanged.
  LinearLocation middle(0, 0, 0.3);
  middle.snapToVertex(line.get(), 2.0);
  EXPECT_DOUBLE_EQ(middle.getSegmentFraction(), 0.3);

  LinearLocation far_from_end(0, 0, 0.7);
  far_from_end.snapToVertex(line.get(), 1.0);
  EXPECT_DOUBLE_EQ(far_from_end.getSegmentFraction(), 0.7);

  // Snapping on second component of MultiLineString.
  auto mls = ReadWKT("MULTILINESTRING ((0 0, 10 0), (0 0, 0 10))");
  LinearLocation mls_near_end(1, 0, 0.9);
  mls_near_end.snapToVertex(mls.get(), 1.5);
  EXPECT_DOUBLE_EQ(mls_near_end.getSegmentFraction(), 1.0);

  auto pt = ReadWKT("POINT (0 0)");
  LinearLocation pt_loc(0, 0, 0.5);
  EXPECT_THROW(pt_loc.snapToVertex(pt.get(), 1.0), IllegalArgumentException);
}

TEST_F(LinearLocationTest, GetSegmentLength) {
  auto line = ReadWKT("LINESTRING (0 0, 3 4, 3 14)");
  EXPECT_DOUBLE_EQ(LinearLocation(0, 0, 0.5).getSegmentLength(line.get()), 5.0);
  EXPECT_DOUBLE_EQ(LinearLocation(0, 1, 0.2).getSegmentLength(line.get()),
                   10.0);
  // At or past final vertex uses last segment.
  EXPECT_DOUBLE_EQ(LinearLocation(0, 2, 0.0).getSegmentLength(line.get()),
                   10.0);
  EXPECT_DOUBLE_EQ(LinearLocation(0, 5, 0.0).getSegmentLength(line.get()),
                   10.0);

  auto mls = ReadWKT("MULTILINESTRING ((0 0, 1 0), (0 0, 0 7))");
  EXPECT_DOUBLE_EQ(LinearLocation(0, 0, 0.0).getSegmentLength(mls.get()), 1.0);
  EXPECT_DOUBLE_EQ(LinearLocation(1, 0, 0.5).getSegmentLength(mls.get()), 7.0);

  auto pt = ReadWKT("POINT (0 0)");
  EXPECT_THROW(LinearLocation(0, 0, 0.0).getSegmentLength(pt.get()),
               IllegalArgumentException);

  auto poly = ReadWKT("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  EXPECT_THROW(LinearLocation(0, 0, 0.0).getSegmentLength(poly.get()),
               IllegalArgumentException);
}

TEST_F(LinearLocationTest, IsVertex) {
  EXPECT_TRUE(LinearLocation(0, 0, 0.0).isVertex());
  EXPECT_TRUE(LinearLocation(0, -0.1).isVertex());
  EXPECT_TRUE(LinearLocation(0, 1.0).isVertex());
  EXPECT_TRUE(LinearLocation(0, 1.2).isVertex());
  EXPECT_FALSE(LinearLocation(0, 0, 0.0001).isVertex());
  EXPECT_FALSE(LinearLocation(0, 0, 0.5).isVertex());
  EXPECT_FALSE(LinearLocation(0, 0, 0.9999).isVertex());
}

TEST_F(LinearLocationTest, GetCoordinate) {
  auto empty_line = ReadWKT("LINESTRING EMPTY");
  Coordinate empty_c =
      LinearLocation(0, 0, 0.0).getCoordinate(empty_line.get());
  EXPECT_TRUE(empty_c.isNull());

  auto empty_mls = ReadWKT("MULTILINESTRING EMPTY");
  Coordinate empty_mls_c =
      LinearLocation(0, 0, 0.0).getCoordinate(empty_mls.get());
  EXPECT_TRUE(empty_mls_c.isNull());

  auto pt = ReadWKT("POINT (1 2)");
  EXPECT_THROW(LinearLocation(0, 0, 0.0).getCoordinate(pt.get()),
               IllegalArgumentException);

  auto poly = ReadWKT("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  EXPECT_THROW(LinearLocation(0, 0, 0.5).getCoordinate(poly.get()),
               IllegalArgumentException);

  auto line_3d = ReadWKT("LINESTRING Z (0 0 10, 10 0 20, 10 10 30)");
  Coordinate start_c = LinearLocation(0, 0, 0.0).getCoordinate(line_3d.get());
  EXPECT_TRUE(start_c.equals3D(Coordinate(0.0, 0.0, 10.0)));

  Coordinate mid_c = LinearLocation(0, 0, 0.5).getCoordinate(line_3d.get());
  EXPECT_TRUE(mid_c.equals3D(Coordinate(5.0, 0.0, 15.0)));

  Coordinate seg1_c = LinearLocation(0, 1, 0.25).getCoordinate(line_3d.get());
  EXPECT_TRUE(seg1_c.equals3D(Coordinate(10.0, 2.5, 22.5)));

  Coordinate last_vertex_c =
      LinearLocation(0, 2, 0.0).getCoordinate(line_3d.get());
  EXPECT_TRUE(last_vertex_c.equals3D(Coordinate(10.0, 10.0, 30.0)));

  Coordinate end_c = LinearLocation::getEndLocation(line_3d.get())
                         .getCoordinate(line_3d.get());
  EXPECT_TRUE(end_c.equals3D(Coordinate(10.0, 10.0, 30.0)));

  // 2D line returns NaN Z.
  auto line_2d = ReadWKT("LINESTRING (0 0, 4 0)");
  Coordinate c_2d = LinearLocation(0, 0, 0.5).getCoordinate(line_2d.get());
  EXPECT_DOUBLE_EQ(c_2d.x, 2.0);
  EXPECT_DOUBLE_EQ(c_2d.y, 0.0);
  EXPECT_TRUE(std::isnan(c_2d.z));

  // MultiLineString second component.
  auto mls = ReadWKT("MULTILINESTRING ((0 0, 10 0), (20 10, 20 30))");
  Coordinate mls_c = LinearLocation(1, 0, 0.25).getCoordinate(mls.get());
  EXPECT_DOUBLE_EQ(mls_c.x, 20.0);
  EXPECT_DOUBLE_EQ(mls_c.y, 15.0);
}

TEST_F(LinearLocationTest, GetSegment) {
  auto pt = ReadWKT("POINT (1 2)");
  EXPECT_THROW(LinearLocation(0, 0, 0.0).getSegment(pt.get()),
               IllegalArgumentException);

  auto line = ReadWKT("LINESTRING (0 0, 10 0, 10 20)");
  std::unique_ptr<LineSegment> seg0 =
      LinearLocation(0, 0, 0.5).getSegment(line.get());
  ASSERT_NE(seg0, nullptr);
  EXPECT_EQ(seg0->compareTo(LineSegment(0.0, 0.0, 10.0, 0.0)), 0);

  std::unique_ptr<LineSegment> seg1 =
      LinearLocation(0, 1, 0.2).getSegment(line.get());
  ASSERT_NE(seg1, nullptr);
  EXPECT_EQ(seg1->compareTo(LineSegment(10.0, 0.0, 10.0, 20.0)), 0);

  // Endpoint returns last segment.
  std::unique_ptr<LineSegment> end_seg =
      LinearLocation::getEndLocation(line.get()).getSegment(line.get());
  ASSERT_NE(end_seg, nullptr);
  EXPECT_EQ(end_seg->compareTo(LineSegment(10.0, 0.0, 10.0, 20.0)), 0);
  EXPECT_TRUE(end_seg->p0.equals2D(Coordinate(10.0, 0.0)));
  EXPECT_TRUE(end_seg->p1.equals2D(Coordinate(10.0, 20.0)));

  auto two_pt = ReadWKT("LINESTRING (1 2, 4 6)");
  std::unique_ptr<LineSegment> two_pt_end =
      LinearLocation::getEndLocation(two_pt.get()).getSegment(two_pt.get());
  ASSERT_NE(two_pt_end, nullptr);
  EXPECT_TRUE(two_pt_end->p0.equals2D(Coordinate(1.0, 2.0)));
  EXPECT_TRUE(two_pt_end->p1.equals2D(Coordinate(4.0, 6.0)));

  auto mls = ReadWKT("MULTILINESTRING ((0 0, 1 0), (5 5, 8 9))");
  std::unique_ptr<LineSegment> mls_seg =
      LinearLocation(1, 0, 0.5).getSegment(mls.get());
  ASSERT_NE(mls_seg, nullptr);
  EXPECT_EQ(mls_seg->compareTo(LineSegment(5.0, 5.0, 8.0, 9.0)), 0);
}

TEST_F(LinearLocationTest, IsValid) {
  auto line = ReadWKT("LINESTRING (0 0, 10 0, 10 10)");
  EXPECT_TRUE(LinearLocation(0, 0, 0.0).isValid(line.get()));
  EXPECT_TRUE(LinearLocation(0, 1, 0.5).isValid(line.get()));
  // (0, 2, 1.0) normalizes to (0, 3, 0.0), where segmentIndex == numPoints and
  // segmentFraction == 0.0.
  EXPECT_TRUE(LinearLocation(0, 2, 1.0).isValid(line.get()));
  // Unnormalized endpoint (0, 2, 1.0) is also valid.
  EXPECT_TRUE(LinearLocation(2, 1.0).isValid(line.get()));

  // Out-of-bounds component index.
  EXPECT_FALSE(LinearLocation(1, 0, 0.0).isValid(line.get()));
  // segmentIndex > numPoints (3).
  EXPECT_FALSE(LinearLocation(0, 4, 0.0).isValid(line.get()));
  // segmentIndex == numPoints with non-zero fraction.
  EXPECT_FALSE(LinearLocation(3, 0.5).isValid(line.get()));
  // Unnormalized fraction < 0.0 or > 1.0.
  EXPECT_FALSE(LinearLocation(0, -0.1).isValid(line.get()));
  EXPECT_FALSE(LinearLocation(0, 1.5).isValid(line.get()));

  auto empty_mls = ReadWKT("MULTILINESTRING EMPTY");
  EXPECT_FALSE(LinearLocation(0, 0, 0.0).isValid(empty_mls.get()));

  auto pt = ReadWKT("POINT (1 2)");
  EXPECT_THROW(LinearLocation(0, 0, 0.0).isValid(pt.get()),
               IllegalArgumentException);
}

TEST_F(LinearLocationTest, CompareToAndCompareLocationValues) {
  const LinearLocation base(1, 2, 0.5);

  // compareTo: all 7 branches.
  EXPECT_EQ(base.compareTo(LinearLocation(2, 0, 0.0)), -1);
  EXPECT_EQ(base.compareTo(LinearLocation(0, 9, 0.9)), 1);
  EXPECT_EQ(base.compareTo(LinearLocation(1, 3, 0.0)), -1);
  EXPECT_EQ(base.compareTo(LinearLocation(1, 1, 0.9)), 1);
  EXPECT_EQ(base.compareTo(LinearLocation(1, 2, 0.75)), -1);
  EXPECT_EQ(base.compareTo(LinearLocation(1, 2, 0.25)), 1);
  EXPECT_EQ(base.compareTo(LinearLocation(1, 2, 0.5)), 0);

  // Member compareLocationValues: all 7 branches.
  EXPECT_EQ(base.compareLocationValues(2, 0, 0.0), -1);
  EXPECT_EQ(base.compareLocationValues(0, 9, 0.9), 1);
  EXPECT_EQ(base.compareLocationValues(1, 3, 0.0), -1);
  EXPECT_EQ(base.compareLocationValues(1, 1, 0.9), 1);
  EXPECT_EQ(base.compareLocationValues(1, 2, 0.75), -1);
  EXPECT_EQ(base.compareLocationValues(1, 2, 0.25), 1);
  EXPECT_EQ(base.compareLocationValues(1, 2, 0.5), 0);

  // Static compareLocationValues: all 7 branches.
  EXPECT_EQ(LinearLocation::compareLocationValues(1, 2, 0.5, 2, 0, 0.0), -1);
  EXPECT_EQ(LinearLocation::compareLocationValues(1, 2, 0.5, 0, 9, 0.9), 1);
  EXPECT_EQ(LinearLocation::compareLocationValues(1, 2, 0.5, 1, 3, 0.0), -1);
  EXPECT_EQ(LinearLocation::compareLocationValues(1, 2, 0.5, 1, 1, 0.9), 1);
  EXPECT_EQ(LinearLocation::compareLocationValues(1, 2, 0.5, 1, 2, 0.75), -1);
  EXPECT_EQ(LinearLocation::compareLocationValues(1, 2, 0.5, 1, 2, 0.25), 1);
  EXPECT_EQ(LinearLocation::compareLocationValues(1, 2, 0.5, 1, 2, 0.5), 0);
}

TEST_F(LinearLocationTest, IsOnSameSegment) {
  // Different components.
  EXPECT_FALSE(
      LinearLocation(0, 1, 0.5).isOnSameSegment(LinearLocation(1, 1, 0.5)));

  // Same component and segment.
  EXPECT_TRUE(
      LinearLocation(0, 1, 0.2).isOnSameSegment(LinearLocation(0, 1, 0.8)));

  // Next segment start (frac == 0.0) shares segment with previous segment.
  EXPECT_TRUE(
      LinearLocation(0, 1, 0.5).isOnSameSegment(LinearLocation(0, 2, 0.0)));
  EXPECT_FALSE(
      LinearLocation(0, 1, 0.5).isOnSameSegment(LinearLocation(0, 2, 0.1)));

  // Symmetric check when this is at next segment start.
  EXPECT_TRUE(
      LinearLocation(0, 2, 0.0).isOnSameSegment(LinearLocation(0, 1, 0.5)));
  EXPECT_FALSE(
      LinearLocation(0, 2, 0.1).isOnSameSegment(LinearLocation(0, 1, 0.5)));

  // Segments separated by >= 2.
  EXPECT_FALSE(
      LinearLocation(0, 0, 0.0).isOnSameSegment(LinearLocation(0, 2, 0.0)));
  EXPECT_FALSE(
      LinearLocation(0, 2, 0.0).isOnSameSegment(LinearLocation(0, 0, 0.0)));
}

TEST_F(LinearLocationTest, IsEndpoint) {
  auto line = ReadWKT("LINESTRING (0 0, 10 0, 20 0)");
  EXPECT_FALSE(LinearLocation(0, 0, 0.0).isEndpoint(*line));
  EXPECT_FALSE(LinearLocation(0, 1, 0.5).isEndpoint(*line));
  EXPECT_TRUE(LinearLocation(0, 1, 1.0).isEndpoint(*line));
  EXPECT_TRUE(LinearLocation(0, 2, 0.0).isEndpoint(*line));
  EXPECT_TRUE(LinearLocation(2, 1.0).isEndpoint(*line));
  EXPECT_TRUE(LinearLocation(0, 2, 1.0).isEndpoint(*line));
  EXPECT_TRUE(LinearLocation::getEndLocation(line.get()).isEndpoint(*line));

  auto mls = ReadWKT("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0, 40 0))");
  EXPECT_TRUE(LinearLocation(0, 1, 0.0).isEndpoint(*mls));
  EXPECT_FALSE(LinearLocation(1, 0, 0.5).isEndpoint(*mls));
  EXPECT_TRUE(LinearLocation(1, 2, 0.0).isEndpoint(*mls));

  auto pt = ReadWKT("POINT (0 0)");
  EXPECT_THROW(LinearLocation(0, 0, 0.0).isEndpoint(*pt),
               IllegalArgumentException);

  auto poly = ReadWKT("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  EXPECT_THROW(LinearLocation(0, 0, 0.0).isEndpoint(*poly),
               IllegalArgumentException);
}

TEST_F(LinearLocationTest, StreamOperator) {
  {
    std::ostringstream oss;
    oss << LinearLocation();
    EXPECT_EQ(oss.str(), "LinearLoc[0, 0, 0]");
  }
  {
    std::ostringstream oss;
    oss << LinearLocation(1, 3, 0.25);
    EXPECT_EQ(oss.str(), "LinearLoc[1, 3, 0.25]");
  }
}

}  // namespace
}  // namespace geos_google
