// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Unit tests for libharp spherical line geometry operations
// (libharp/harp-geometry-sphere-line.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-geometry-sphere-line.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-geometry.h

#include <cmath>
#include <cstdint>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = std::numbers::pi / 2.0;

harp_spherical_line MakeLine(const harp_spherical_point& begin,
                             const harp_spherical_point& end) {
  harp_spherical_line line{};
  harp_spherical_line_from_spherical_points(&line, &begin, &end);
  return line;
}

TEST(HarpGeometrySphereLineTest, BeginEndAndInverseEulerTransformation) {
  constexpr harp_spherical_point kBegin = {.lon = 0.4, .lat = 0.25};
  constexpr harp_spherical_point kEnd = {.lon = 1.2, .lat = -0.3};
  const harp_spherical_line line = MakeLine(kBegin, kEnd);

  harp_spherical_point out_begin{};
  harp_spherical_point out_end{};
  harp_spherical_line_begin(&out_begin, &line);
  harp_spherical_line_end(&out_end, &line);
  EXPECT_EQ(1, harp_spherical_point_equal(&kBegin, &out_begin));
  EXPECT_EQ(1, harp_spherical_point_equal(&kEnd, &out_end));

  // Inverse Euler transform maps begin back to (0, 0) and end to (length, 0).
  harp_euler_transformation inv{};
  harp_inverse_euler_transformation_from_spherical_line(&inv, &line);
  harp_spherical_point rotated_begin{};
  harp_spherical_point rotated_end{};
  harp_spherical_point_apply_euler_transformation(&rotated_begin, &kBegin,
                                                  &inv);
  harp_spherical_point_apply_euler_transformation(&rotated_end, &kEnd, &inv);
  EXPECT_NEAR(0.0, rotated_begin.lat, 1e-12);
  EXPECT_NEAR(0.0, rotated_begin.lon, 1e-12);
  EXPECT_NEAR(0.0, rotated_end.lat, 1e-12);
  EXPECT_NEAR(line.length, rotated_end.lon, 1e-12);
}

TEST(HarpGeometrySphereLineTest, FromSphericalPointsZeroLengthAndMeridian) {
  // Zero-length line preserves position via special phi/theta/psi branch.
  constexpr harp_spherical_point kPoint = {.lon = 0.75, .lat = 0.4};
  const harp_spherical_line zero_line = MakeLine(kPoint, kPoint);
  EXPECT_DOUBLE_EQ(0.0, zero_line.length);
  EXPECT_DOUBLE_EQ(kHalfPi, zero_line.phi);
  EXPECT_DOUBLE_EQ(kPoint.lat, zero_line.theta);
  EXPECT_DOUBLE_EQ(kPoint.lon - kHalfPi, zero_line.psi);

  harp_spherical_point begin{};
  harp_spherical_point end{};
  harp_spherical_line_begin(&begin, &zero_line);
  harp_spherical_line_end(&end, &zero_line);
  EXPECT_EQ(1, harp_spherical_point_equal(&kPoint, &begin));
  EXPECT_EQ(1, harp_spherical_point_equal(&kPoint, &end));

  // Antipodal points sharing the same longitude trigger the meridian branch
  // (length == pi and equal lon).
  constexpr harp_spherical_point kNorthPole = {.lon = 0.5, .lat = kHalfPi};
  constexpr harp_spherical_point kSouthPole = {.lon = 0.5, .lat = -kHalfPi};
  const harp_spherical_line meridian = MakeLine(kNorthPole, kSouthPole);
  EXPECT_NEAR(kPi, meridian.length, 1e-12);
}

TEST(HarpGeometrySphereLineTest, PointIsAtSphericalLineBranches) {
  constexpr harp_spherical_point kBegin = {.lon = 0.0, .lat = 0.0};
  constexpr harp_spherical_point kEnd = {.lon = 1.0, .lat = 0.0};
  const harp_spherical_line eq_arc = MakeLine(kBegin, kEnd);

  constexpr harp_spherical_point kMid = {.lon = 0.5, .lat = 0.0};
  constexpr harp_spherical_point kBeyondEnd = {.lon = 1.5, .lat = 0.0};
  constexpr harp_spherical_point kBeforeBegin = {.lon = -0.2, .lat = 0.0};
  constexpr harp_spherical_point kOffEquator = {.lon = 0.5, .lat = 0.3};

  EXPECT_EQ(1, harp_spherical_point_is_at_spherical_line(&kBegin, &eq_arc));
  EXPECT_EQ(1, harp_spherical_point_is_at_spherical_line(&kMid, &eq_arc));
  EXPECT_EQ(1, harp_spherical_point_is_at_spherical_line(&kEnd, &eq_arc));
  EXPECT_EQ(0, harp_spherical_point_is_at_spherical_line(&kBeyondEnd, &eq_arc));
  EXPECT_EQ(0,
            harp_spherical_point_is_at_spherical_line(&kBeforeBegin, &eq_arc));
  EXPECT_EQ(0,
            harp_spherical_point_is_at_spherical_line(&kOffEquator, &eq_arc));
}

TEST(HarpGeometrySphereLineTest, IntersectsCoversCrossingConnectedCollinear) {
  // Crossing lines on front hemisphere (i1 inside both).
  constexpr harp_spherical_point kA1 = {.lon = 0.0, .lat = -0.3};
  constexpr harp_spherical_point kA2 = {.lon = 0.0, .lat = 0.3};
  constexpr harp_spherical_point kB1 = {.lon = -0.3, .lat = 0.0};
  constexpr harp_spherical_point kB2 = {.lon = 0.3, .lat = 0.0};
  EXPECT_EQ(1, harp_spherical_line_intersects(&kA1, &kA2, &kB1, &kB2));

  // Crossing lines on back hemisphere where antipodal i2 is inside both.
  constexpr harp_spherical_point kC1 = {.lon = kPi, .lat = -0.3};
  constexpr harp_spherical_point kC2 = {.lon = kPi, .lat = 0.3};
  constexpr harp_spherical_point kD1 = {.lon = kPi - 0.3, .lat = 0.0};
  constexpr harp_spherical_point kD2 = {.lon = kPi + 0.3, .lat = 0.0};
  EXPECT_EQ(1, harp_spherical_line_intersects(&kC1, &kC2, &kD1, &kD2));

  // Connected lines sharing any endpoint return 0.
  constexpr harp_spherical_point kP0 = {.lon = 0.0, .lat = 0.0};
  constexpr harp_spherical_point kP1 = {.lon = 0.5, .lat = 0.0};
  constexpr harp_spherical_point kP2 = {.lon = 0.0, .lat = 0.5};
  EXPECT_EQ(0, harp_spherical_line_intersects(&kP0, &kP1, &kP0, &kP2));
  EXPECT_EQ(0, harp_spherical_line_intersects(&kP0, &kP1, &kP2, &kP0));
  EXPECT_EQ(0, harp_spherical_line_intersects(&kP1, &kP0, &kP0, &kP2));
  EXPECT_EQ(0, harp_spherical_line_intersects(&kP1, &kP0, &kP2, &kP0));

  // Coplanar / identical great circles (norm == 0.0) return 1.
  constexpr harp_spherical_point kEq1 = {.lon = 0.0, .lat = 0.0};
  constexpr harp_spherical_point kEq2 = {.lon = 0.4, .lat = 0.0};
  constexpr harp_spherical_point kEq3 = {.lon = 1.0, .lat = 0.0};
  constexpr harp_spherical_point kEq4 = {.lon = 1.4, .lat = 0.0};
  EXPECT_EQ(1, harp_spherical_line_intersects(&kEq1, &kEq2, &kEq3, &kEq4));

  // Separate non-intersecting lines on distinct great circles return 0.
  constexpr harp_spherical_point kSep1 = {.lon = 1.0, .lat = 0.4};
  constexpr harp_spherical_point kSep2 = {.lon = 1.4, .lat = 0.4};
  EXPECT_EQ(0, harp_spherical_line_intersects(&kEq1, &kEq2, &kSep1, &kSep2));
}

TEST(HarpGeometrySphereLineTest, LineLineRelationshipEquatorialAndGeneral) {
  constexpr harp_spherical_point kLon0 = {.lon = 0.0, .lat = 0.0};
  constexpr harp_spherical_point kLon1 = {.lon = 1.0, .lat = 0.0};
  constexpr harp_spherical_point kLon02 = {.lon = 0.2, .lat = 0.0};
  constexpr harp_spherical_point kLon08 = {.lon = 0.8, .lat = 0.0};
  constexpr harp_spherical_point kLon15 = {.lon = 1.5, .lat = 0.0};
  constexpr harp_spherical_point kLon20 = {.lon = 2.0, .lat = 0.0};
  constexpr harp_spherical_point kLonNeg05 = {.lon = -0.5, .lat = 0.0};

  const harp_spherical_line l_0_1 = MakeLine(kLon0, kLon1);
  const harp_spherical_line l_1_0 = MakeLine(kLon1, kLon0);
  const harp_spherical_line l_sub = MakeLine(kLon02, kLon08);
  const harp_spherical_line l_1_2 = MakeLine(kLon1, kLon20);
  const harp_spherical_line l_neg05_0 = MakeLine(kLonNeg05, kLon0);
  const harp_spherical_line l_05_15 =
      MakeLine({.lon = 0.5, .lat = 0.0}, kLon15);
  const harp_spherical_line l_neg05_05 =
      MakeLine(kLonNeg05, {.lon = 0.5, .lat = 0.0});
  const harp_spherical_line l_15_20 = MakeLine(kLon15, kLon20);

  // Equal, reversed-equal, contained/contains, connected, overlapping, separate
  // on the same great circle.
  EXPECT_EQ(HARP_GEOMETRY_LINE_EQUAL,
            harp_spherical_line_spherical_line_relationship(&l_0_1, &l_0_1));
  EXPECT_EQ(HARP_GEOMETRY_LINE_CONTAINS,
            harp_spherical_line_spherical_line_relationship(&l_0_1, &l_1_0));
  EXPECT_EQ(HARP_GEOMETRY_LINE_CONTAINS,
            harp_spherical_line_spherical_line_relationship(&l_0_1, &l_sub));
  EXPECT_EQ(HARP_GEOMETRY_LINE_CONTAINED,
            harp_spherical_line_spherical_line_relationship(&l_sub, &l_0_1));
  EXPECT_EQ(HARP_GEOMETRY_LINE_CONNECTED,
            harp_spherical_line_spherical_line_relationship(&l_0_1, &l_1_2));
  EXPECT_EQ(
      HARP_GEOMETRY_LINE_CONNECTED,
      harp_spherical_line_spherical_line_relationship(&l_0_1, &l_neg05_0));
  EXPECT_EQ(HARP_GEOMETRY_LINE_OVERLAP,
            harp_spherical_line_spherical_line_relationship(&l_0_1, &l_05_15));
  EXPECT_EQ(
      HARP_GEOMETRY_LINE_OVERLAP,
      harp_spherical_line_spherical_line_relationship(&l_0_1, &l_neg05_05));
  EXPECT_EQ(HARP_GEOMETRY_LINE_SEPARATE,
            harp_spherical_line_spherical_line_relationship(&l_0_1, &l_15_20));

  // Full 2*pi great circle lines compare equal regardless of phi.
  harp_spherical_line full1 = {
      .phi = 0.2, .theta = 0.5, .psi = 0.3, .length = 2.0 * kPi};
  harp_spherical_line full2 = {
      .phi = 1.4, .theta = 0.5, .psi = 0.3, .length = 2.0 * kPi};
  EXPECT_EQ(HARP_GEOMETRY_LINE_EQUAL,
            harp_spherical_line_spherical_line_relationship(&full1, &full2));

  // Two zero-length lines are separate; NaN-length line with distinct
  // orientation is separate.
  const harp_spherical_line pt_a = MakeLine(kLon0, kLon0);
  const harp_spherical_line pt_b = MakeLine(kLon1, kLon1);
  EXPECT_EQ(HARP_GEOMETRY_LINE_SEPARATE,
            harp_spherical_line_spherical_line_relationship(&pt_a, &pt_b));
  harp_spherical_line nan_line = l_15_20;
  nan_line.length = harp_nan();
  EXPECT_EQ(HARP_GEOMETRY_LINE_SEPARATE,
            harp_spherical_line_spherical_line_relationship(&nan_line, &l_0_1));

  // Non-collinear lines: connected at all 4 endpoint combinations.
  const harp_spherical_line cross_at_0 =
      MakeLine(kLon0, {.lon = 0.0, .lat = 0.5});
  const harp_spherical_line cross_at_0_rev =
      MakeLine({.lon = 0.0, .lat = 0.5}, kLon0);
  const harp_spherical_line cross_at_1 =
      MakeLine(kLon1, {.lon = 1.0, .lat = 0.5});
  const harp_spherical_line cross_at_1_rev =
      MakeLine({.lon = 1.0, .lat = 0.5}, kLon1);
  EXPECT_EQ(
      HARP_GEOMETRY_LINE_CONNECTED,
      harp_spherical_line_spherical_line_relationship(&l_0_1, &cross_at_0));
  EXPECT_EQ(
      HARP_GEOMETRY_LINE_CONNECTED,
      harp_spherical_line_spherical_line_relationship(&l_0_1, &cross_at_0_rev));
  EXPECT_EQ(
      HARP_GEOMETRY_LINE_CONNECTED,
      harp_spherical_line_spherical_line_relationship(&l_0_1, &cross_at_1));
  EXPECT_EQ(
      HARP_GEOMETRY_LINE_CONNECTED,
      harp_spherical_line_spherical_line_relationship(&l_0_1, &cross_at_1_rev));

  // Non-collinear crossing (both descending and ascending) and separate.
  // Also test a long line (length > pi/2) to cover the v[0][1].x <= 0.0 branch.
  const harp_spherical_line long_eq = MakeLine(kLon0, {.lon = 2.2, .lat = 0.0});
  const harp_spherical_line cross_desc =
      MakeLine({.lon = 0.5, .lat = 0.4}, {.lon = 0.5, .lat = -0.4});
  const harp_spherical_line cross_asc =
      MakeLine({.lon = 0.5, .lat = -0.4}, {.lon = 0.5, .lat = 0.4});
  const harp_spherical_line north_only =
      MakeLine({.lon = 0.5, .lat = 0.2}, {.lon = 0.5, .lat = 0.6});
  const harp_spherical_line cross_outside =
      MakeLine({.lon = 2.5, .lat = 0.4}, {.lon = 2.5, .lat = -0.4});

  EXPECT_EQ(
      HARP_GEOMETRY_LINE_CROSS,
      harp_spherical_line_spherical_line_relationship(&long_eq, &cross_desc));
  EXPECT_EQ(
      HARP_GEOMETRY_LINE_CROSS,
      harp_spherical_line_spherical_line_relationship(&l_0_1, &cross_asc));
  EXPECT_EQ(
      HARP_GEOMETRY_LINE_SEPARATE,
      harp_spherical_line_spherical_line_relationship(&l_0_1, &north_only));
  EXPECT_EQ(
      HARP_GEOMETRY_LINE_SEPARATE,
      harp_spherical_line_spherical_line_relationship(&l_0_1, &cross_outside));
}

TEST(HarpGeometrySphereLineTest, IntersectionPointPolygonSegmentAndDistance) {
  // Intersection of equator segment and meridian segment at (lat=0, lon=0.5).
  const harp_spherical_line eq =
      MakeLine({.lon = 0.0, .lat = 0.0}, {.lon = 1.0, .lat = 0.0});
  const harp_spherical_line mer =
      MakeLine({.lon = 0.5, .lat = -0.4}, {.lon = 0.5, .lat = 0.4});
  harp_spherical_point intersection{};
  harp_spherical_line_spherical_line_intersection_point(&eq, &mer,
                                                        &intersection);
  EXPECT_NEAR(0.0, intersection.lat, 1e-12);
  EXPECT_NEAR(0.5, intersection.lon, 1e-12);

  // Coincident great circles return NaN intersection point.
  const harp_spherical_line eq2 =
      MakeLine({.lon = 1.2, .lat = 0.0}, {.lon = 1.8, .lat = 0.0});
  harp_spherical_line_spherical_line_intersection_point(&eq, &eq2,
                                                        &intersection);
  EXPECT_TRUE(std::isnan(intersection.lat));
  EXPECT_TRUE(std::isnan(intersection.lon));

  // Polygon segment extraction (valid indices including wrap-around and OOB).
  harp_spherical_polygon* raw_poly = nullptr;
  ASSERT_EQ(0, harp_spherical_polygon_new(3, &raw_poly));
  ScopedHarpSphericalPolygon poly(raw_poly);
  poly->point[0] = {.lon = 0.0, .lat = 0.0};
  poly->point[1] = {.lon = 0.5, .lat = 0.0};
  poly->point[2] = {.lon = 0.0, .lat = 0.5};

  harp_spherical_line seg{};
  EXPECT_EQ(-1, harp_spherical_polygon_get_segment(&seg, poly.get(), -1));
  EXPECT_EQ(-1, harp_spherical_polygon_get_segment(&seg, poly.get(), 3));
  ASSERT_EQ(0, harp_spherical_polygon_get_segment(&seg, poly.get(), 0));
  harp_spherical_point seg_begin{};
  harp_spherical_point seg_end{};
  harp_spherical_line_begin(&seg_begin, &seg);
  harp_spherical_line_end(&seg_end, &seg);
  EXPECT_EQ(1, harp_spherical_point_equal(&poly->point[0], &seg_begin));
  EXPECT_EQ(1, harp_spherical_point_equal(&poly->point[1], &seg_end));

  ASSERT_EQ(0, harp_spherical_polygon_get_segment(&seg, poly.get(), 2));
  harp_spherical_line_begin(&seg_begin, &seg);
  harp_spherical_line_end(&seg_end, &seg);
  EXPECT_EQ(1, harp_spherical_point_equal(&poly->point[2], &seg_begin));
  EXPECT_EQ(1, harp_spherical_point_equal(&poly->point[0], &seg_end));

  // 3D point-to-line distance measures Euclidean distance to the 3D chord
  // through the line endpoints: 0 at an endpoint, 1 - cos(0.5) at midpoint
  // (0.5, 0) of [0, 1], and hypot(cos(0.5), 1) from North Pole to [-0.5, 0.5].
  constexpr harp_spherical_point kAtBegin = {.lon = 0.0, .lat = 0.0};
  EXPECT_NEAR(0.0, harp_spherical_line_spherical_point_distance(&eq, &kAtBegin),
              1e-12);
  constexpr harp_spherical_point kOnArcMid = {.lon = 0.5, .lat = 0.0};
  EXPECT_NEAR(1.0 - std::cos(0.5),
              harp_spherical_line_spherical_point_distance(&eq, &kOnArcMid),
              1e-12);
  constexpr harp_spherical_point kNorthPole = {.lon = 0.0, .lat = kHalfPi};
  const harp_spherical_line q_eq =
      MakeLine({.lon = -0.5, .lat = 0.0}, {.lon = 0.5, .lat = 0.0});
  EXPECT_NEAR(std::hypot(std::cos(0.5), 1.0),
              harp_spherical_line_spherical_point_distance(&q_eq, &kNorthPole),
              1e-12);

  // Degenerate zero-length line yields NaN distance.
  const harp_spherical_line zero = MakeLine(kOnArcMid, kOnArcMid);
  EXPECT_TRUE(std::isnan(
      harp_spherical_line_spherical_point_distance(&zero, &kNorthPole)));
}

}  // namespace
}  // namespace stcorp_harp
