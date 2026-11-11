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

// Unit tests for third_party/geos/src/geom/LineSegment.cpp and
// third_party/geos/include/geos/geom/LineSegment.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/LineSegment.cpp

#include "third_party/geos/include/geos/geom/LineSegment.h"

#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"

namespace geos_google {
namespace {

using geos::algorithm::Orientation;
using geos::geom::Coordinate;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LineSegment;
using geos::geom::LineString;
using geos::util::IllegalStateException;

TEST(LineSegmentTest, ConstructorsSetCoordinatesAndIndexing) {
  LineSegment default_seg;
  EXPECT_DOUBLE_EQ(default_seg.p0.x, 0.0);
  EXPECT_DOUBLE_EQ(default_seg.p0.y, 0.0);
  EXPECT_TRUE(std::isnan(default_seg.p0.z));
  EXPECT_DOUBLE_EQ(default_seg.p1.x, 0.0);
  EXPECT_DOUBLE_EQ(default_seg.p1.y, 0.0);
  EXPECT_TRUE(std::isnan(default_seg.p1.z));

  LineSegment xy_seg(1.0, 2.0, 4.0, 6.0);
  EXPECT_DOUBLE_EQ(xy_seg.p0.x, 1.0);
  EXPECT_DOUBLE_EQ(xy_seg.p0.y, 2.0);
  EXPECT_TRUE(std::isnan(xy_seg.p0.z));
  EXPECT_DOUBLE_EQ(xy_seg.p1.x, 4.0);
  EXPECT_DOUBLE_EQ(xy_seg.p1.y, 6.0);
  EXPECT_TRUE(std::isnan(xy_seg.p1.z));

  const Coordinate c0(10.0, 20.0, 30.0);
  const Coordinate c1(40.0, 50.0, 60.0);
  LineSegment xyz_seg(c0, c1);
  EXPECT_TRUE(xyz_seg[0].equals3D(c0));
  EXPECT_TRUE(xyz_seg[1].equals3D(c1));

  const LineSegment& const_ref = xyz_seg;
  EXPECT_TRUE(const_ref[0].equals3D(c0));
  EXPECT_TRUE(const_ref[1].equals3D(c1));

  xyz_seg[0] = Coordinate(-1.0, -2.0, -3.0);
  xyz_seg[1] = Coordinate(7.0, 8.0, 9.0);
  EXPECT_TRUE(xyz_seg.p0.equals3D(Coordinate(-1.0, -2.0, -3.0)));
  EXPECT_TRUE(xyz_seg.p1.equals3D(Coordinate(7.0, 8.0, 9.0)));

  default_seg.setCoordinates(Coordinate(2.0, 3.0, 4.0),
                             Coordinate(5.0, 6.0, 7.0));
  EXPECT_TRUE(default_seg.p0.equals3D(Coordinate(2.0, 3.0, 4.0)));
  EXPECT_TRUE(default_seg.p1.equals3D(Coordinate(5.0, 6.0, 7.0)));

  xy_seg.setCoordinates(default_seg);
  EXPECT_TRUE(xy_seg.p0.equals3D(Coordinate(2.0, 3.0, 4.0)));
  EXPECT_TRUE(xy_seg.p1.equals3D(Coordinate(5.0, 6.0, 7.0)));
}

TEST(LineSegmentTest, MemberStreamInsertionFormatsCoordinates) {
  LineSegment seg(1.5, 2.5, 3.5, 4.5);
  std::ostringstream oss;
  seg << oss;
  EXPECT_EQ(oss.str(), "LINESEGMENT(1.5 2.5,3.5 4.5)");
}

TEST(LineSegmentTest, BoundsLengthAxisAlignmentMidPointAndAngle) {
  const LineSegment forward(1.0, 2.0, 4.0, 6.0);
  EXPECT_DOUBLE_EQ(forward.minX(), 1.0);
  EXPECT_DOUBLE_EQ(forward.maxX(), 4.0);
  EXPECT_DOUBLE_EQ(forward.minY(), 2.0);
  EXPECT_DOUBLE_EQ(forward.maxY(), 6.0);
  EXPECT_DOUBLE_EQ(forward.getLength(), 5.0);
  EXPECT_FALSE(forward.isHorizontal());
  EXPECT_FALSE(forward.isVertical());

  const LineSegment backward(4.0, 6.0, 1.0, 2.0);
  EXPECT_DOUBLE_EQ(backward.minX(), 1.0);
  EXPECT_DOUBLE_EQ(backward.maxX(), 4.0);
  EXPECT_DOUBLE_EQ(backward.minY(), 2.0);
  EXPECT_DOUBLE_EQ(backward.maxY(), 6.0);

  const LineSegment with_z(Coordinate(0.0, 0.0, 100.0),
                           Coordinate(3.0, 4.0, 500.0));
  EXPECT_DOUBLE_EQ(with_z.getLength(), 5.0);

  const LineSegment zero(3.0, 3.0, 3.0, 3.0);
  EXPECT_DOUBLE_EQ(zero.getLength(), 0.0);
  EXPECT_TRUE(zero.isHorizontal());
  EXPECT_TRUE(zero.isVertical());

  const LineSegment horiz(0.0, 5.0, 10.0, 5.0);
  EXPECT_TRUE(horiz.isHorizontal());
  EXPECT_FALSE(horiz.isVertical());
  EXPECT_DOUBLE_EQ(horiz.angle(), 0.0);

  const LineSegment vert(5.0, 0.0, 5.0, 10.0);
  EXPECT_FALSE(vert.isHorizontal());
  EXPECT_TRUE(vert.isVertical());
  EXPECT_NEAR(vert.angle(), std::numbers::pi / 2.0, 1e-12);

  EXPECT_NEAR(LineSegment(0.0, 0.0, 1.0, 1.0).angle(), std::numbers::pi / 4.0,
              1e-12);
  EXPECT_NEAR(LineSegment(0.0, 0.0, -1.0, 0.0).angle(), std::numbers::pi,
              1e-12);
  EXPECT_NEAR(LineSegment(0.0, 0.0, 0.0, -1.0).angle(), -std::numbers::pi / 2.0,
              1e-12);

  const CoordinateXY mid = forward.midPoint();
  EXPECT_DOUBLE_EQ(mid.x, 2.5);
  EXPECT_DOUBLE_EQ(mid.y, 4.0);

  const CoordinateXY static_mid =
      LineSegment::midPoint(CoordinateXY(-2.0, 4.0), CoordinateXY(6.0, -8.0));
  EXPECT_DOUBLE_EQ(static_mid.x, 2.0);
  EXPECT_DOUBLE_EQ(static_mid.y, -2.0);
}

TEST(LineSegmentTest, ReverseNormalizeEqualsTopoOrderingAndHash) {
  LineSegment seg(Coordinate(1.0, 2.0, 3.0), Coordinate(4.0, 5.0, 6.0));
  seg.reverse();
  EXPECT_TRUE(seg.p0.equals3D(Coordinate(4.0, 5.0, 6.0)));
  EXPECT_TRUE(seg.p1.equals3D(Coordinate(1.0, 2.0, 3.0)));

  LineSegment norm_x(5.0, 1.0, 2.0, 9.0);
  norm_x.normalize();
  EXPECT_EQ(norm_x.p0, CoordinateXY(2.0, 9.0));
  EXPECT_EQ(norm_x.p1, CoordinateXY(5.0, 1.0));

  LineSegment norm_y(3.0, 8.0, 3.0, 2.0);
  norm_y.normalize();
  EXPECT_EQ(norm_y.p0, CoordinateXY(3.0, 2.0));
  EXPECT_EQ(norm_y.p1, CoordinateXY(3.0, 8.0));

  LineSegment already_norm(1.0, 2.0, 3.0, 4.0);
  already_norm.normalize();
  EXPECT_EQ(already_norm.p0, CoordinateXY(1.0, 2.0));
  EXPECT_EQ(already_norm.p1, CoordinateXY(3.0, 4.0));

  const LineSegment base(Coordinate(0.0, 0.0, 1.0), Coordinate(2.0, 2.0, 2.0));
  const LineSegment same_xy(Coordinate(0.0, 0.0, 9.0),
                            Coordinate(2.0, 2.0, 8.0));
  const LineSegment reversed(2.0, 2.0, 0.0, 0.0);
  const LineSegment different(0.0, 0.0, 2.0, 3.0);
  EXPECT_TRUE(base.equalsTopo(same_xy));
  EXPECT_TRUE(base.equalsTopo(reversed));
  EXPECT_FALSE(base.equalsTopo(different));

  const LineSegment s_a(0.0, 0.0, 1.0, 1.0);
  const LineSegment s_b(0.0, 0.0, 1.0, 1.0);
  const LineSegment s_c(0.0, 0.0, 2.0, 0.0);
  const LineSegment s_d(1.0, 0.0, 2.0, 0.0);
  EXPECT_EQ(s_a.compareTo(s_b), 0);
  EXPECT_TRUE(s_a == s_b);
  EXPECT_LT(s_a.compareTo(s_c), 0);
  EXPECT_GT(s_c.compareTo(s_a), 0);
  EXPECT_LT(s_c.compareTo(s_d), 0);
  EXPECT_GT(s_d.compareTo(s_c), 0);
  EXPECT_TRUE(s_a < s_c);
  EXPECT_TRUE(s_c > s_a);
  EXPECT_FALSE(s_a < s_b);
  EXPECT_FALSE(s_a > s_b);

  LineSegment::HashCode hasher;
  EXPECT_EQ(hasher(s_a), hasher(s_b));
  EXPECT_EQ(hasher(&s_a), hasher(&s_b));

  LineSegment::UnorderedSet seg_set;
  seg_set.insert(s_a);
  seg_set.insert(s_b);
  seg_set.insert(s_c);
  EXPECT_EQ(seg_set.size(), 2u);
}

TEST(LineSegmentTest, OrientationIndexPointAndSegment) {
  const LineSegment base(0.0, 0.0, 10.0, 0.0);

  EXPECT_EQ(base.orientationIndex(CoordinateXY(5.0, 3.0)), Orientation::LEFT);
  EXPECT_EQ(base.orientationIndex(CoordinateXY(5.0, -3.0)), Orientation::RIGHT);
  EXPECT_EQ(base.orientationIndex(CoordinateXY(15.0, 0.0)),
            Orientation::COLLINEAR);

  EXPECT_EQ(base.orientationIndex(Coordinate(5.0, 3.0, 99.0)),
            Orientation::LEFT);
  EXPECT_EQ(base.orientationIndex(Coordinate(5.0, -3.0, 99.0)),
            Orientation::RIGHT);
  EXPECT_EQ(base.orientationIndex(Coordinate(-5.0, 0.0, 99.0)),
            Orientation::COLLINEAR);

  const LineSegment both_left(1.0, 1.0, 9.0, 2.0);
  const LineSegment left_touch_p0(1.0, 0.0, 5.0, 4.0);
  const LineSegment left_touch_p1(5.0, 4.0, 9.0, 0.0);
  const LineSegment collinear(2.0, 0.0, 12.0, 0.0);
  const LineSegment both_right(1.0, -1.0, 9.0, -2.0);
  const LineSegment right_touch_p0(1.0, 0.0, 5.0, -4.0);
  const LineSegment right_touch_p1(5.0, -4.0, 9.0, 0.0);
  const LineSegment cross_lr(5.0, 3.0, 5.0, -3.0);
  const LineSegment cross_rl(5.0, -3.0, 5.0, 3.0);

  EXPECT_EQ(base.orientationIndex(both_left), 1);
  EXPECT_EQ(base.orientationIndex(&both_left), 1);
  EXPECT_EQ(base.orientationIndex(left_touch_p0), 1);
  EXPECT_EQ(base.orientationIndex(left_touch_p1), 1);
  EXPECT_EQ(base.orientationIndex(collinear), 0);
  EXPECT_EQ(base.orientationIndex(both_right), -1);
  EXPECT_EQ(base.orientationIndex(right_touch_p0), -1);
  EXPECT_EQ(base.orientationIndex(right_touch_p1), -1);
  EXPECT_EQ(base.orientationIndex(cross_lr), 0);
  EXPECT_EQ(base.orientationIndex(cross_rl), 0);
}

TEST(LineSegmentTest, ProjectionFactorAndSegmentFraction) {
  struct ProjectionCase {
    std::string_view name;
    double x0;
    double y0;
    double x1;
    double y1;
    double px;
    double py;
    double expected_factor;
    double expected_fraction;
  };

  constexpr auto kCases = std::to_array<ProjectionCase>({
      {
          .name = "exact_p0",
          .x0 = 0.0,
          .y0 = 0.0,
          .x1 = 10.0,
          .y1 = 0.0,
          .px = 0.0,
          .py = 0.0,
          .expected_factor = 0.0,
          .expected_fraction = 0.0,
      },
      {
          .name = "exact_p1",
          .x0 = 0.0,
          .y0 = 0.0,
          .x1 = 10.0,
          .y1 = 0.0,
          .px = 10.0,
          .py = 0.0,
          .expected_factor = 1.0,
          .expected_fraction = 1.0,
      },
      {
          .name = "degenerate_segment",
          .x0 = 2.0,
          .y0 = 3.0,
          .x1 = 2.0,
          .y1 = 3.0,
          .px = 5.0,
          .py = 7.0,
          .expected_factor = 0.0,
          .expected_fraction = 0.0,
      },
      {
          .name = "perpendicular_through_p0",
          .x0 = 0.0,
          .y0 = 0.0,
          .x1 = 10.0,
          .y1 = 0.0,
          .px = 0.0,
          .py = 5.0,
          .expected_factor = 0.0,
          .expected_fraction = 0.0,
      },
      {
          .name = "perpendicular_through_p1",
          .x0 = 0.0,
          .y0 = 0.0,
          .x1 = 10.0,
          .y1 = 0.0,
          .px = 10.0,
          .py = -4.0,
          .expected_factor = 1.0,
          .expected_fraction = 1.0,
      },
      {
          .name = "midpoint",
          .x0 = 0.0,
          .y0 = 0.0,
          .x1 = 10.0,
          .y1 = 0.0,
          .px = 5.0,
          .py = 7.0,
          .expected_factor = 0.5,
          .expected_fraction = 0.5,
      },
      {
          .name = "before_p0",
          .x0 = 0.0,
          .y0 = 0.0,
          .x1 = 10.0,
          .y1 = 0.0,
          .px = -5.0,
          .py = 3.0,
          .expected_factor = -0.5,
          .expected_fraction = 0.0,
      },
      {
          .name = "beyond_p1",
          .x0 = 0.0,
          .y0 = 0.0,
          .x1 = 10.0,
          .y1 = 0.0,
          .px = 15.0,
          .py = -2.0,
          .expected_factor = 1.5,
          .expected_fraction = 1.0,
      },
      {
          .name = "diagonal_segment",
          .x0 = 0.0,
          .y0 = 0.0,
          .x1 = 3.0,
          .y1 = 4.0,
          .px = 4.0,
          .py = 3.0,
          .expected_factor = 0.96,
          .expected_fraction = 0.96,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const LineSegment seg(tc.x0, tc.y0, tc.x1, tc.y1);
    const CoordinateXY p(tc.px, tc.py);
    EXPECT_DOUBLE_EQ(seg.projectionFactor(p), tc.expected_factor);
    EXPECT_DOUBLE_EQ(seg.segmentFraction(p), tc.expected_fraction);
  }
}

TEST(LineSegmentTest, ProjectPointOverloads) {
  const LineSegment seg(0.0, 0.0, 10.0, 10.0);

  Coordinate out;
  seg.project(Coordinate(0.0, 0.0, 42.0), out);
  EXPECT_TRUE(out.equals3D(Coordinate(0.0, 0.0, 42.0)));

  seg.project(Coordinate(10.0, 10.0, 99.0), out);
  EXPECT_TRUE(out.equals3D(Coordinate(10.0, 10.0, 99.0)));

  seg.project(Coordinate(0.0, 10.0, 7.0), out);
  EXPECT_DOUBLE_EQ(out.x, 5.0);
  EXPECT_DOUBLE_EQ(out.y, 5.0);
  EXPECT_TRUE(std::isnan(out.z));

  seg.project(Coordinate(-4.0, 0.0, 1.0), out);
  EXPECT_DOUBLE_EQ(out.x, -2.0);
  EXPECT_DOUBLE_EQ(out.y, -2.0);
  EXPECT_TRUE(std::isnan(out.z));

  seg.project(Coordinate(14.0, 10.0, 1.0), out);
  EXPECT_DOUBLE_EQ(out.x, 12.0);
  EXPECT_DOUBLE_EQ(out.y, 12.0);

  EXPECT_EQ(seg.project(CoordinateXY(0.0, 0.0)), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(seg.project(CoordinateXY(10.0, 10.0)), CoordinateXY(10.0, 10.0));
  EXPECT_EQ(seg.project(CoordinateXY(0.0, 10.0)), CoordinateXY(5.0, 5.0));
  EXPECT_EQ(seg.project(CoordinateXY(-4.0, 0.0)), CoordinateXY(-2.0, -2.0));
  EXPECT_EQ(seg.project(CoordinateXY(14.0, 10.0)), CoordinateXY(12.0, 12.0));

  const LineSegment zero(3.0, 4.0, 3.0, 4.0);
  zero.project(Coordinate(10.0, 20.0, 5.0), out);
  EXPECT_EQ(out, CoordinateXY(3.0, 4.0));
  EXPECT_EQ(zero.project(CoordinateXY(10.0, 20.0)), CoordinateXY(3.0, 4.0));
}

TEST(LineSegmentTest, ProjectLineSegmentCoversAllClampingAndDisjointPaths) {
  struct SegmentProjectCase {
    std::string_view name;
    double sx0;
    double sy0;
    double sx1;
    double sy1;
    bool expected_overlap;
    double ex0;
    double ey0;
    double ex1;
    double ey1;
  };

  constexpr auto kCases = std::to_array<SegmentProjectCase>({
      {
          .name = "entirely_after_p1",
          .sx0 = 12.0,
          .sy0 = 2.0,
          .sx1 = 15.0,
          .sy1 = 2.0,
          .expected_overlap = false,
      },
      {
          .name = "starts_at_p1_and_goes_after",
          .sx0 = 10.0,
          .sy0 = 2.0,
          .sx1 = 14.0,
          .sy1 = 2.0,
          .expected_overlap = false,
      },
      {
          .name = "perpendicular_at_p1",
          .sx0 = 10.0,
          .sy0 = -3.0,
          .sx1 = 10.0,
          .sy1 = 3.0,
          .expected_overlap = false,
      },
      {
          .name = "entirely_before_p0",
          .sx0 = -5.0,
          .sy0 = 1.0,
          .sx1 = -2.0,
          .sy1 = 1.0,
          .expected_overlap = false,
      },
      {
          .name = "ends_at_p0_and_goes_before",
          .sx0 = -4.0,
          .sy0 = 1.0,
          .sx1 = 0.0,
          .sy1 = 1.0,
          .expected_overlap = false,
      },
      {
          .name = "perpendicular_at_p0",
          .sx0 = 0.0,
          .sy0 = -3.0,
          .sx1 = 0.0,
          .sy1 = 3.0,
          .expected_overlap = false,
      },
      {
          .name = "strictly_interior",
          .sx0 = 2.0,
          .sy0 = 5.0,
          .sx1 = 8.0,
          .sy1 = -3.0,
          .expected_overlap = true,
          .ex0 = 2.0,
          .ey0 = 0.0,
          .ex1 = 8.0,
          .ey1 = 0.0,
      },
      {
          .name = "spans_full_segment",
          .sx0 = -5.0,
          .sy0 = 3.0,
          .sx1 = 15.0,
          .sy1 = 3.0,
          .expected_overlap = true,
          .ex0 = 0.0,
          .ey0 = 0.0,
          .ex1 = 10.0,
          .ey1 = 0.0,
      },
      {
          .name = "reversed_spans_full_segment",
          .sx0 = 15.0,
          .sy0 = 3.0,
          .sx1 = -5.0,
          .sy1 = 3.0,
          .expected_overlap = true,
          .ex0 = 10.0,
          .ey0 = 0.0,
          .ex1 = 0.0,
          .ey1 = 0.0,
      },
      {
          .name = "clamps_start_below_zero",
          .sx0 = -3.0,
          .sy0 = 2.0,
          .sx1 = 4.0,
          .sy1 = 2.0,
          .expected_overlap = true,
          .ex0 = 0.0,
          .ey0 = 0.0,
          .ex1 = 4.0,
          .ey1 = 0.0,
      },
      {
          .name = "clamps_end_above_one",
          .sx0 = 4.0,
          .sy0 = 2.0,
          .sx1 = 13.0,
          .sy1 = 2.0,
          .expected_overlap = true,
          .ex0 = 4.0,
          .ey0 = 0.0,
          .ex1 = 10.0,
          .ey1 = 0.0,
      },
      {
          .name = "exact_factor_one_on_p0",
          .sx0 = 10.0,
          .sy0 = 4.0,
          .sx1 = 2.5,
          .sy1 = 4.0,
          .expected_overlap = true,
          .ex0 = 10.0,
          .ey0 = 0.0,
          .ex1 = 2.5,
          .ey1 = 0.0,
      },
      {
          .name = "exact_factor_one_on_p1",
          .sx0 = 2.5,
          .sy0 = 4.0,
          .sx1 = 10.0,
          .sy1 = 4.0,
          .expected_overlap = true,
          .ex0 = 2.5,
          .ey0 = 0.0,
          .ex1 = 10.0,
          .ey1 = 0.0,
      },
      {
          .name = "perpendicular_interior_collapses_to_point",
          .sx0 = 3.0,
          .sy0 = -4.0,
          .sx1 = 3.0,
          .sy1 = 4.0,
          .expected_overlap = true,
          .ex0 = 3.0,
          .ey0 = 0.0,
          .ex1 = 3.0,
          .ey1 = 0.0,
      },
  });

  const LineSegment base(0.0, 0.0, 10.0, 0.0);
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const LineSegment input(tc.sx0, tc.sy0, tc.sx1, tc.sy1);
    LineSegment ret(99.0, 99.0, 99.0, 99.0);
    const bool has_overlap = base.project(input, ret);
    EXPECT_EQ(has_overlap, tc.expected_overlap);
    if (tc.expected_overlap) {
      EXPECT_EQ(ret.p0, CoordinateXY(tc.ex0, tc.ey0));
      EXPECT_EQ(ret.p1, CoordinateXY(tc.ex1, tc.ey1));
    }
  }
}

TEST(LineSegmentTest, DistanceAndClosestPoint) {
  const LineSegment base(0.0, 0.0, 10.0, 0.0);

  EXPECT_DOUBLE_EQ(base.distance(CoordinateXY(4.0, 3.0)), 3.0);
  EXPECT_DOUBLE_EQ(base.distance(CoordinateXY(-3.0, 4.0)), 5.0);
  EXPECT_DOUBLE_EQ(base.distance(LineSegment(2.0, 2.0, 8.0, 5.0)), 2.0);
  EXPECT_DOUBLE_EQ(base.distance(LineSegment(5.0, -1.0, 5.0, 1.0)), 0.0);

  EXPECT_DOUBLE_EQ(base.distancePerpendicular(CoordinateXY(20.0, -7.0)), 7.0);
  EXPECT_DOUBLE_EQ(base.distancePerpendicularOriented(CoordinateXY(20.0, 7.0)),
                   7.0);
  EXPECT_DOUBLE_EQ(base.distancePerpendicularOriented(CoordinateXY(20.0, -7.0)),
                   -7.0);
  EXPECT_DOUBLE_EQ(base.distancePerpendicularOriented(CoordinateXY(20.0, 0.0)),
                   0.0);

  const LineSegment zero(1.0, 1.0, 1.0, 1.0);
  EXPECT_DOUBLE_EQ(zero.distancePerpendicular(CoordinateXY(4.0, 5.0)), 5.0);
  EXPECT_DOUBLE_EQ(zero.distancePerpendicularOriented(CoordinateXY(4.0, 5.0)),
                   5.0);

  CoordinateXY closest;
  base.closestPoint(CoordinateXY(4.0, 7.0), closest);
  EXPECT_EQ(closest, CoordinateXY(4.0, 0.0));

  base.closestPoint(CoordinateXY(-3.0, 4.0), closest);
  EXPECT_EQ(closest, CoordinateXY(0.0, 0.0));

  base.closestPoint(CoordinateXY(13.0, -4.0), closest);
  EXPECT_EQ(closest, CoordinateXY(10.0, 0.0));

  zero.closestPoint(CoordinateXY(4.0, 5.0), closest);
  EXPECT_EQ(closest, CoordinateXY(1.0, 1.0));
}

TEST(LineSegmentTest, ClosestPointsCoversIntersectionAndAllEndpointWinners) {
  {
    LineSegment a(0.0, 0.0, 10.0, 10.0);
    const LineSegment b(0.0, 10.0, 10.0, 0.0);
    const auto pts = a.closestPoints(b);
    EXPECT_EQ(pts[0], CoordinateXY(5.0, 5.0));
    EXPECT_EQ(pts[1], CoordinateXY(5.0, 5.0));
    EXPECT_EQ(a.closestPoints(&b), pts);
  }

  {
    LineSegment a(0.0, 0.0, 4.0, 0.0);
    const LineSegment b(2.0, 0.0, 6.0, 0.0);
    const auto pts = a.closestPoints(b);
    EXPECT_EQ(pts[0], pts[1]);
    EXPECT_DOUBLE_EQ(pts[0].y, 0.0);
    EXPECT_GE(pts[0].x, 2.0);
    EXPECT_LE(pts[0].x, 4.0);
  }

  {
    // Case 1: line.p0 is closest to interior of this.
    LineSegment a(0.0, 0.0, 10.0, 0.0);
    const LineSegment b(5.0, 1.0, 5.0, 5.0);
    const auto pts = a.closestPoints(b);
    EXPECT_EQ(pts[0], CoordinateXY(5.0, 0.0));
    EXPECT_EQ(pts[1], CoordinateXY(5.0, 1.0));
  }

  {
    // Case 2: line.p1 is closest to interior of this (close01 beats close00).
    LineSegment a(0.0, 0.0, 10.0, 0.0);
    const LineSegment b(5.0, 5.0, 5.0, 1.0);
    const auto pts = a.closestPoints(b);
    EXPECT_EQ(pts[0], CoordinateXY(5.0, 0.0));
    EXPECT_EQ(pts[1], CoordinateXY(5.0, 1.0));
  }

  {
    // Case 3: this->p0 is closest to interior of line (close10 beats both).
    LineSegment a(5.0, 1.0, 5.0, 5.0);
    const LineSegment b(0.0, 0.0, 10.0, 0.0);
    const auto pts = a.closestPoints(b);
    EXPECT_EQ(pts[0], CoordinateXY(5.0, 1.0));
    EXPECT_EQ(pts[1], CoordinateXY(5.0, 0.0));
  }

  {
    // Case 4: this->p1 is closest to interior of line (close11 strictly wins:
    // dist(close00)=sqrt(26), dist(close01)=sqrt(26), dist(close10)=10,
    // dist(close11)=1).
    LineSegment a(5.0, 10.0, 5.0, 1.0);
    const LineSegment b(0.0, 0.0, 10.0, 0.0);
    const auto pts = a.closestPoints(b);
    EXPECT_EQ(pts[0], CoordinateXY(5.0, 1.0));
    EXPECT_EQ(pts[1], CoordinateXY(5.0, 0.0));
  }
}

TEST(LineSegmentTest, IntersectionAndLineIntersection) {
  const LineSegment diag1(0.0, 0.0, 4.0, 4.0);
  const LineSegment diag2(0.0, 4.0, 4.0, 0.0);
  EXPECT_EQ(diag1.intersection(diag2), CoordinateXY(2.0, 2.0));
  EXPECT_EQ(diag1.lineIntersection(diag2), CoordinateXY(2.0, 2.0));

  const LineSegment t_stem(2.0, 0.0, 2.0, 2.0);
  const LineSegment t_top(0.0, 2.0, 4.0, 2.0);
  EXPECT_EQ(t_stem.intersection(t_top), CoordinateXY(2.0, 2.0));

  const LineSegment overlap_a(0.0, 0.0, 4.0, 0.0);
  const LineSegment overlap_b(2.0, 0.0, 6.0, 0.0);
  const Coordinate overlap_pt = overlap_a.intersection(overlap_b);
  EXPECT_FALSE(overlap_pt.isNull());
  EXPECT_DOUBLE_EQ(overlap_pt.y, 0.0);
  EXPECT_GE(overlap_pt.x, 2.0);
  EXPECT_LE(overlap_pt.x, 4.0);
  EXPECT_TRUE(overlap_a.lineIntersection(overlap_b).isNull());

  const LineSegment horiz(0.0, 0.0, 1.0, 0.0);
  const LineSegment vert_disjoint(5.0, 1.0, 5.0, 3.0);
  EXPECT_TRUE(horiz.intersection(vert_disjoint).isNull());
  EXPECT_EQ(horiz.lineIntersection(vert_disjoint), CoordinateXY(5.0, 0.0));

  const LineSegment parallel(0.0, 2.0, 5.0, 2.0);
  EXPECT_TRUE(horiz.intersection(parallel).isNull());
  EXPECT_TRUE(horiz.lineIntersection(parallel).isNull());
}

TEST(LineSegmentTest, PointAlongPointAlongOffsetAndOffsetSegment) {
  const LineSegment seg3d(Coordinate(0.0, 0.0, 10.0),
                          Coordinate(10.0, 0.0, 20.0));
  Coordinate pt;

  seg3d.pointAlong(0.0, pt);
  EXPECT_TRUE(pt.equals3D(Coordinate(0.0, 0.0, 10.0)));

  seg3d.pointAlong(0.5, pt);
  EXPECT_TRUE(pt.equals3D(Coordinate(5.0, 0.0, 15.0)));

  seg3d.pointAlong(1.0, pt);
  EXPECT_TRUE(pt.equals3D(Coordinate(10.0, 0.0, 20.0)));

  seg3d.pointAlong(-0.5, pt);
  EXPECT_TRUE(pt.equals3D(Coordinate(-5.0, 0.0, 5.0)));

  seg3d.pointAlong(1.5, pt);
  EXPECT_TRUE(pt.equals3D(Coordinate(15.0, 0.0, 25.0)));

  LineSegment horiz(0.0, 0.0, 10.0, 0.0);
  horiz.pointAlongOffset(0.5, 2.0, pt);
  EXPECT_EQ(pt, CoordinateXY(5.0, 2.0));
  EXPECT_TRUE(std::isnan(pt.z));

  horiz.pointAlongOffset(0.5, -3.0, pt);
  EXPECT_EQ(pt, CoordinateXY(5.0, -3.0));

  horiz.pointAlongOffset(0.25, 0.0, pt);
  EXPECT_EQ(pt, CoordinateXY(2.5, 0.0));

  const LineSegment diag(0.0, 0.0, 3.0, 4.0);
  diag.pointAlongOffset(0.0, 5.0, pt);
  EXPECT_DOUBLE_EQ(pt.x, -4.0);
  EXPECT_DOUBLE_EQ(pt.y, 3.0);

  const LineSegment off_left = horiz.offset(2.0);
  EXPECT_EQ(off_left.p0, CoordinateXY(0.0, 2.0));
  EXPECT_EQ(off_left.p1, CoordinateXY(10.0, 2.0));

  const LineSegment off_right = horiz.offset(-2.0);
  EXPECT_EQ(off_right.p0, CoordinateXY(0.0, -2.0));
  EXPECT_EQ(off_right.p1, CoordinateXY(10.0, -2.0));

  const LineSegment off_zero = horiz.offset(0.0);
  EXPECT_EQ(off_zero, horiz);

  LineSegment zero_len(3.0, 4.0, 3.0, 4.0);
  zero_len.pointAlongOffset(0.5, 0.0, pt);
  EXPECT_EQ(pt, CoordinateXY(3.0, 4.0));
  EXPECT_THROW(zero_len.pointAlongOffset(0.5, 1.0, pt), IllegalStateException);
  EXPECT_THROW(zero_len.offset(1.0), IllegalStateException);
}

TEST(LineSegmentTest, ToGeometryCreatesTwoPointLineString) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  const LineSegment seg2d(1.0, 2.0, 4.0, 6.0);
  std::unique_ptr<LineString> line2d = seg2d.toGeometry(*factory);
  ASSERT_NE(line2d, nullptr);
  EXPECT_EQ(line2d->getFactory(), factory.get());
  EXPECT_EQ(line2d->getNumPoints(), 2u);
  EXPECT_EQ(line2d->getCoordinateN(0), CoordinateXY(1.0, 2.0));
  EXPECT_EQ(line2d->getCoordinateN(1), CoordinateXY(4.0, 6.0));
  EXPECT_DOUBLE_EQ(line2d->getLength(), 5.0);

  const LineSegment seg3d(Coordinate(1.0, 2.0, 7.0), Coordinate(4.0, 6.0, 9.0));
  std::unique_ptr<LineString> line3d = seg3d.toGeometry(*factory);
  ASSERT_NE(line3d, nullptr);
  EXPECT_TRUE(line3d->getCoordinateN(0).equals3D(Coordinate(1.0, 2.0, 7.0)));
  EXPECT_TRUE(line3d->getCoordinateN(1).equals3D(Coordinate(4.0, 6.0, 9.0)));
}

}  // namespace
}  // namespace geos_google
