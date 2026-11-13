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
// Unit tests for libharp spherical polygon geometry operations
// (libharp/harp-geometry-sphere-polygon.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-geometry-sphere-polygon.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-geometry.h

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::HasSubstr;

ScopedHarpSphericalPolygon MakePolygonDeg(std::vector<double> lats,
                                          std::vector<double> lons,
                                          int check_polygon = 1) {
  harp_spherical_polygon* raw = nullptr;
  int rc = harp_spherical_polygon_from_latitude_longitude_bounds(
      0, static_cast<long>(lats.size()), lats.data(), lons.data(),
      check_polygon, &raw);
  if (rc != 0) {
    return nullptr;
  }
  return ScopedHarpSphericalPolygon(raw);
}

harp_spherical_point PointDeg(double lat_deg, double lon_deg) {
  harp_spherical_point p = {.lon = lon_deg, .lat = lat_deg};
  harp_spherical_point_rad_from_deg(&p);
  harp_spherical_point_check(&p);
  return p;
}

TEST(HarpGeometrySpherePolygonTest, FromBoundsNewDeleteAndValidationErrors) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // 1. 2-vertex bounding box expands to 4 corners.
  double box_lats[2] = {10.0, 20.0};
  double box_lons[2] = {30.0, 40.0};
  harp_spherical_polygon* raw_box = nullptr;
  ASSERT_EQ(0, harp_spherical_polygon_from_latitude_longitude_bounds(
                   0, 2, box_lats, box_lons, 1, &raw_box));
  ScopedHarpSphericalPolygon box(raw_box);
  EXPECT_EQ(4, box->numberofpoints);

  // 2. Degenerate 2-vertex bounding box (equal lat or equal lon) fails.
  double deg_lats[2] = {10.0, 10.0};
  EXPECT_EQ(-1, harp_spherical_polygon_from_latitude_longitude_bounds(
                    0, 2, deg_lats, box_lons, 1, &raw_box));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  double deg_lons[2] = {30.0, 30.0};
  EXPECT_EQ(-1, harp_spherical_polygon_from_latitude_longitude_bounds(
                    0, 2, box_lats, deg_lons, 1, &raw_box));

  // 3. Closed polygon discards duplicate closing vertex; measurement_id > 0
  //    selects the second slice.
  double multi_lats[10] = {0, 0, 10, 10, 0, 1, 1, 5, 5, 1};
  double multi_lons[10] = {0, 10, 10, 0, 0, 20, 25, 25, 20, 20};
  harp_spherical_polygon* raw_closed = nullptr;
  ASSERT_EQ(0, harp_spherical_polygon_from_latitude_longitude_bounds(
                   1, 5, multi_lats, multi_lons, 1, &raw_closed));
  ScopedHarpSphericalPolygon closed(raw_closed);
  EXPECT_EQ(4, closed->numberofpoints);
  EXPECT_NEAR(1.0 * CONST_DEG2RAD, closed->point[0].lat, 1e-12);
  EXPECT_NEAR(20.0 * CONST_DEG2RAD, closed->point[0].lon, 1e-12);

  // 4. Single-point input has begin == end so num_points drops to 0 and fails.
  double one_lat[1] = {5.0};
  double one_lon[1] = {5.0};
  EXPECT_EQ(-1, harp_spherical_polygon_from_latitude_longitude_bounds(
                    0, 1, one_lat, one_lon, 1, &raw_closed));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // 5. Self-intersecting bowtie polygon fails check.
  std::vector<double> bowtie_lat = {0.0, 10.0, 10.0, 0.0};
  std::vector<double> bowtie_lon = {0.0, 10.0, 0.0, 10.0};
  EXPECT_EQ(-1,
            harp_spherical_polygon_from_latitude_longitude_bounds(
                0, 4, bowtie_lat.data(), bowtie_lon.data(), 1, &raw_closed));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_THAT(harp_errno_to_string(HARP_ERROR_INVALID_ARGUMENT),
              HasSubstr("overlap"));

  // 6. Non-crossing polygon extending > 90 deg from its centroid fails
  //    hemisphere check (line segments don't overlap, FPle(point.lat, 0.0)).
  std::vector<double> huge_lat = {-30.0, 5.0, 5.0, 5.0};
  std::vector<double> huge_lon = {0.0, 90.0, 180.0, 270.0};
  harp_spherical_polygon* raw_huge = nullptr;
  EXPECT_EQ(-1, harp_spherical_polygon_from_latitude_longitude_bounds(
                    0, 4, huge_lat.data(), huge_lon.data(), 1, &raw_huge));
  ScopedHarpSphericalPolygon huge_guard(raw_huge);
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // 7. Ultra-thin [0, 1e-12] x [0, 0.5] sliver yields near-zero centroid
  //    vector (norm2 > 0, all components within FPzero) -> "polygon too large".
  harp_spherical_polygon* raw_zero_center = nullptr;
  ASSERT_EQ(0, harp_spherical_polygon_new(4, &raw_zero_center));
  ScopedHarpSphericalPolygon zero_center(raw_zero_center);
  zero_center->point[0] = {.lon = 0.0, .lat = 0.0};
  zero_center->point[1] = {.lon = 0.5, .lat = 0.0};
  zero_center->point[2] = {.lon = 0.5, .lat = 1e-12};
  zero_center->point[3] = {.lon = 0.0, .lat = 1e-12};
  EXPECT_EQ(-1, harp_spherical_polygon_check(zero_center.get()));
  EXPECT_THAT(harp_errno_to_string(HARP_ERROR_INVALID_ARGUMENT),
              HasSubstr("too large"));

  // 8. Check with check_polygon == 0 skips validation; deleting nullptr is
  // safe.
  ScopedHarpSphericalPolygon unchecked =
      MakePolygonDeg(bowtie_lat, bowtie_lon, /*check_polygon=*/0);
  ASSERT_NE(nullptr, unchecked);
  harp_spherical_polygon_delete(nullptr);
}

TEST(HarpGeometrySpherePolygonTest,
     CentreHandlesWindingObtuseEdgesAndSmallPolys) {
  // Degenerate polygon (< 3 points) falls back to (1, 0, 0).
  harp_spherical_polygon* raw_two = nullptr;
  ASSERT_EQ(0, harp_spherical_polygon_new(2, &raw_two));
  ScopedHarpSphericalPolygon two_pts(raw_two);
  two_pts->point[0] = {.lon = 0.0, .lat = 0.0};
  two_pts->point[1] = {.lon = 0.5, .lat = 0.5};
  harp_vector3d centre{};
  ASSERT_EQ(0, harp_spherical_polygon_centre(&centre, two_pts.get()));
  EXPECT_DOUBLE_EQ(1.0, centre.x);
  EXPECT_DOUBLE_EQ(0.0, centre.y);
  EXPECT_DOUBLE_EQ(0.0, centre.z);

  // CCW and CW triangles should produce the same positive centroid vector.
  ScopedHarpSphericalPolygon ccw =
      MakePolygonDeg({0.0, 0.0, 10.0}, {0.0, 10.0, 5.0});
  ScopedHarpSphericalPolygon cw =
      MakePolygonDeg({0.0, 10.0, 0.0}, {0.0, 5.0, 10.0});
  ASSERT_NE(nullptr, ccw);
  ASSERT_NE(nullptr, cw);
  harp_vector3d c_ccw{};
  harp_vector3d c_cw{};
  ASSERT_EQ(0, harp_spherical_polygon_centre(&c_ccw, ccw.get()));
  ASSERT_EQ(0, harp_spherical_polygon_centre(&c_cw, cw.get()));
  EXPECT_NEAR(c_ccw.x, c_cw.x, 1e-12);
  EXPECT_NEAR(c_ccw.y, c_cw.y, 1e-12);
  EXPECT_NEAR(c_ccw.z, c_cw.z, 1e-12);

  // Large spherical triangle with an edge spanning > 90 deg (dotab < 0 branch).
  ScopedHarpSphericalPolygon obtuse =
      MakePolygonDeg({10.0, 10.0, 80.0}, {0.0, 110.0, 55.0});
  ASSERT_NE(nullptr, obtuse);
  ASSERT_EQ(0, harp_spherical_polygon_centre(&centre, obtuse.get()));
  EXPECT_GT(centre.z, 0.0);
}

TEST(HarpGeometrySpherePolygonTest,
     ContainsPointCoversBoundsEdgesEquatorAndPoles) {
  // Empty polygon contains nothing.
  harp_spherical_polygon* raw_empty = nullptr;
  ASSERT_EQ(0, harp_spherical_polygon_new(0, &raw_empty));
  ScopedHarpSphericalPolygon empty(raw_empty);
  const harp_spherical_point origin = PointDeg(0.0, 0.0);
  EXPECT_EQ(0, harp_spherical_polygon_contains_point(empty.get(), &origin));

  // Square around equator/prime meridian.
  ScopedHarpSphericalPolygon sq =
      MakePolygonDeg({0.0, 0.0, 10.0, 10.0}, {0.0, 10.0, 10.0, 0.0});
  ASSERT_NE(nullptr, sq);

  // On vertex, on edge, strictly inside, outside bounds, and inside bounds
  // near a corner.
  const harp_spherical_point vertex = PointDeg(0.0, 0.0);
  const harp_spherical_point on_edge = PointDeg(0.0, 5.0);
  const harp_spherical_point inside = PointDeg(5.0, 5.0);
  const harp_spherical_point outside_far = PointDeg(30.0, 30.0);
  // Point on equator shares lat=0 with two vertices, triggering the on_equator
  // randomized rotation loop inside contains_point.
  const harp_spherical_point equator_inside = PointDeg(0.5, 5.0);
  const harp_spherical_point equator_outside = PointDeg(0.0, 12.0);
  // Diamond straddling the equator: (0, 5) is strictly inside and lies on the
  // same great circle as vertices (0, 0) and (0, 10), triggering the
  // on_equator randomized rotation loop inside contains_point.
  ScopedHarpSphericalPolygon diamond =
      MakePolygonDeg({-5.0, 0.0, 5.0, 0.0}, {5.0, 10.0, 5.0, 0.0});
  ASSERT_NE(nullptr, diamond);
  const harp_spherical_point diamond_center = PointDeg(0.0, 5.0);
  EXPECT_EQ(
      1, harp_spherical_polygon_contains_point(diamond.get(), &diamond_center));

  // Triangle to test inside-bounding-box-but-outside-polygon.
  ScopedHarpSphericalPolygon tri =
      MakePolygonDeg({0.0, 0.0, 10.0}, {0.0, 10.0, 0.0});
  ASSERT_NE(nullptr, tri);
  const harp_spherical_point in_bbox_outside_tri = PointDeg(9.0, 9.0);

  EXPECT_EQ(1, harp_spherical_polygon_contains_point(sq.get(), &vertex));
  EXPECT_EQ(1, harp_spherical_polygon_contains_point(sq.get(), &on_edge));
  EXPECT_EQ(1, harp_spherical_polygon_contains_point(sq.get(), &inside));
  EXPECT_EQ(1,
            harp_spherical_polygon_contains_point(sq.get(), &equator_inside));
  EXPECT_EQ(0, harp_spherical_polygon_contains_point(sq.get(), &outside_far));
  EXPECT_EQ(0,
            harp_spherical_polygon_contains_point(sq.get(), &equator_outside));
  EXPECT_EQ(0, harp_spherical_polygon_contains_point(tri.get(),
                                                     &in_bbox_outside_tri));

  // Dateline-crossing polygon and point wrapping.
  ScopedHarpSphericalPolygon dateline = MakePolygonDeg(
      {-10.0, -10.0, 10.0, 10.0}, {170.0, -170.0, -170.0, 170.0});
  ASSERT_NE(nullptr, dateline);
  const harp_spherical_point at_179 = PointDeg(0.0, 179.0);
  const harp_spherical_point at_neg179 = PointDeg(0.0, -179.0);
  const harp_spherical_point at_prime = PointDeg(0.0, 0.0);
  EXPECT_EQ(1, harp_spherical_polygon_contains_point(dateline.get(), &at_179));
  EXPECT_EQ(1,
            harp_spherical_polygon_contains_point(dateline.get(), &at_neg179));
  EXPECT_EQ(0,
            harp_spherical_polygon_contains_point(dateline.get(), &at_prime));

  // North-pole and South-pole covering polygons (longitude range == 2*pi).
  ScopedHarpSphericalPolygon north_cap =
      MakePolygonDeg({70.0, 70.0, 70.0, 70.0}, {-135.0, -45.0, 45.0, 135.0});
  ScopedHarpSphericalPolygon south_cap = MakePolygonDeg(
      {-70.0, -70.0, -70.0, -70.0}, {135.0, 45.0, -45.0, -135.0});
  ASSERT_NE(nullptr, north_cap);
  ASSERT_NE(nullptr, south_cap);
  const harp_spherical_point north_pole = PointDeg(90.0, 0.0);
  const harp_spherical_point south_pole = PointDeg(-90.0, 0.0);
  EXPECT_EQ(
      1, harp_spherical_polygon_contains_point(north_cap.get(), &north_pole));
  EXPECT_EQ(
      0, harp_spherical_polygon_contains_point(north_cap.get(), &south_pole));
  EXPECT_EQ(
      1, harp_spherical_polygon_contains_point(south_cap.get(), &south_pole));
  EXPECT_EQ(
      0, harp_spherical_polygon_contains_point(south_cap.get(), &north_pole));
}

TEST(HarpGeometrySpherePolygonTest, PolygonLineRelationshipsCoverAllCases) {
  ScopedHarpSphericalPolygon sq =
      MakePolygonDeg({0.0, 0.0, 10.0, 10.0}, {0.0, 10.0, 10.0, 0.0});
  ASSERT_NE(nullptr, sq);

  auto line_deg = [](double lat1, double lon1, double lat2, double lon2) {
    harp_spherical_line l{};
    harp_spherical_point p1 = PointDeg(lat1, lon1);
    harp_spherical_point p2 = PointDeg(lat2, lon2);
    harp_spherical_line_from_spherical_points(&l, &p1, &p2);
    return l;
  };

  // Strictly inside.
  const harp_spherical_line inside = line_deg(2.0, 2.0, 8.0, 8.0);
  EXPECT_EQ(
      HARP_GEOMETRY_LINE_POLY_CONTAINED,
      harp_spherical_polygon_spherical_line_relationship(sq.get(), &inside));

  // Equal to one polygon edge -> SEPARATE per HARP convention.
  const harp_spherical_line equal_edge = line_deg(0.0, 0.0, 0.0, 10.0);
  EXPECT_EQ(HARP_GEOMETRY_LINE_POLY_SEPARATE,
            harp_spherical_polygon_spherical_line_relationship(sq.get(),
                                                               &equal_edge));

  // Overlapping collinear edge.
  const harp_spherical_line overlap_edge = line_deg(0.0, -5.0, 0.0, 5.0);
  EXPECT_EQ(HARP_GEOMETRY_LINE_POLY_OVERLAP,
            harp_spherical_polygon_spherical_line_relationship(sq.get(),
                                                               &overlap_edge));

  // Crossing an edge with both endpoints outside (one inside, one outside).
  const harp_spherical_line cross_thru = line_deg(5.0, -5.0, 5.0, 5.0);
  EXPECT_EQ(HARP_GEOMETRY_LINE_POLY_OVERLAP,
            harp_spherical_polygon_spherical_line_relationship(sq.get(),
                                                               &cross_thru));

  // Crossing an edge with one endpoint on the edge and the other outside.
  const harp_spherical_line from_edge_out = line_deg(5.0, 0.0, 5.0, -5.0);
  EXPECT_EQ(HARP_GEOMETRY_LINE_POLY_OVERLAP,
            harp_spherical_polygon_spherical_line_relationship(sq.get(),
                                                               &from_edge_out));

  // Touching edge/vertex from outside -> SEPARATE (covers !p1 && p2 and p1 &&
  // !p2 branches).
  const harp_spherical_line touch_from_out = line_deg(-5.0, -5.0, 0.0, 0.0);
  const harp_spherical_line touch_to_out = line_deg(0.0, 0.0, -5.0, -5.0);
  EXPECT_EQ(HARP_GEOMETRY_LINE_POLY_SEPARATE,
            harp_spherical_polygon_spherical_line_relationship(
                sq.get(), &touch_from_out));
  EXPECT_EQ(HARP_GEOMETRY_LINE_POLY_SEPARATE,
            harp_spherical_polygon_spherical_line_relationship(sq.get(),
                                                               &touch_to_out));

  // Line from an edge point going strictly inside -> CONTAINED.
  const harp_spherical_line edge_to_inside = line_deg(5.0, 0.0, 5.0, 5.0);
  EXPECT_EQ(HARP_GEOMETRY_LINE_POLY_CONTAINED,
            harp_spherical_polygon_spherical_line_relationship(
                sq.get(), &edge_to_inside));

  // Completely disjoint line.
  const harp_spherical_line disjoint = line_deg(20.0, 20.0, 25.0, 25.0);
  EXPECT_EQ(
      HARP_GEOMETRY_LINE_POLY_SEPARATE,
      harp_spherical_polygon_spherical_line_relationship(sq.get(), &disjoint));
}

TEST(HarpGeometrySpherePolygonTest,
     PolygonRelationshipsOverlapFractionAndPublicApis) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  ScopedHarpSphericalPolygon outer =
      MakePolygonDeg({0.0, 0.0, 20.0, 20.0}, {0.0, 20.0, 20.0, 0.0});
  ScopedHarpSphericalPolygon inner =
      MakePolygonDeg({5.0, 5.0, 15.0, 15.0}, {5.0, 15.0, 15.0, 5.0});
  // Opposite winding shifted overlap (exercises ascending and descending
  // vertex addition loops in overlapping_fraction).
  ScopedHarpSphericalPolygon shifted_ccw =
      MakePolygonDeg({10.0, 10.0, 30.0, 30.0}, {10.0, 30.0, 30.0, 10.0});
  ScopedHarpSphericalPolygon shifted_cw =
      MakePolygonDeg({10.0, 30.0, 30.0, 10.0}, {10.0, 10.0, 30.0, 30.0});
  ScopedHarpSphericalPolygon disjoint_far =
      MakePolygonDeg({40.0, 40.0, 50.0, 50.0}, {40.0, 50.0, 50.0, 40.0});
  // Two disjoint triangles whose bounding boxes overlap (recheck == 1 path).
  ScopedHarpSphericalPolygon tri_lower =
      MakePolygonDeg({0.0, 0.0, 10.0}, {0.0, 10.0, 0.0});
  ScopedHarpSphericalPolygon tri_upper =
      MakePolygonDeg({6.0, 12.0, 12.0}, {6.0, 6.0, 12.0});

  ASSERT_NE(nullptr, outer);
  ASSERT_NE(nullptr, inner);
  ASSERT_NE(nullptr, shifted_ccw);
  ASSERT_NE(nullptr, shifted_cw);
  ASSERT_NE(nullptr, disjoint_far);
  ASSERT_NE(nullptr, tri_lower);
  ASSERT_NE(nullptr, tri_upper);

  EXPECT_EQ(HARP_GEOMETRY_POLY_CONTAINS,
            harp_spherical_polygon_spherical_polygon_relationship(
                outer.get(), inner.get(), 0));
  EXPECT_EQ(HARP_GEOMETRY_POLY_CONTAINED,
            harp_spherical_polygon_spherical_polygon_relationship(
                inner.get(), outer.get(), 0));
  EXPECT_EQ(HARP_GEOMETRY_POLY_OVERLAP,
            harp_spherical_polygon_spherical_polygon_relationship(
                outer.get(), shifted_ccw.get(), 0));
  EXPECT_EQ(HARP_GEOMETRY_POLY_SEPARATE,
            harp_spherical_polygon_spherical_polygon_relationship(
                outer.get(), disjoint_far.get(), 0));
  EXPECT_EQ(HARP_GEOMETRY_POLY_SEPARATE,
            harp_spherical_polygon_spherical_polygon_relationship(
                tri_lower.get(), tri_upper.get(), 0));

  // Overlapping and overlapping fraction checks.
  int is_overlapping = -1;
  double fraction = -1.0;
  ASSERT_EQ(0, harp_spherical_polygon_overlapping_fraction(
                   outer.get(), inner.get(), &is_overlapping, &fraction));
  EXPECT_EQ(1, is_overlapping);
  EXPECT_DOUBLE_EQ(1.0, fraction);

  ASSERT_EQ(0,
            harp_spherical_polygon_overlapping_fraction(
                outer.get(), disjoint_far.get(), &is_overlapping, &fraction));
  EXPECT_EQ(0, is_overlapping);
  EXPECT_DOUBLE_EQ(0.0, fraction);

  ASSERT_EQ(0, harp_spherical_polygon_overlapping_fraction(
                   outer.get(), shifted_ccw.get(), &is_overlapping, &fraction));
  EXPECT_EQ(1, is_overlapping);
  EXPECT_GT(fraction, 0.1);
  EXPECT_LT(fraction, 0.5);

  double fraction_cw = -1.0;
  ASSERT_EQ(0,
            harp_spherical_polygon_overlapping_fraction(
                outer.get(), shifted_cw.get(), &is_overlapping, &fraction_cw));
  EXPECT_EQ(1, is_overlapping);
  EXPECT_NEAR(fraction, fraction_cw, 1e-6);

  // Collinear edge overlap: share the lat=0 great-circle edge.
  ScopedHarpSphericalPolygon right_edge =
      MakePolygonDeg({0.0, 0.0, 20.0, 20.0}, {10.0, 30.0, 30.0, 10.0});
  ASSERT_NE(nullptr, right_edge);
  ASSERT_EQ(0, harp_spherical_polygon_overlapping_fraction(
                   outer.get(), right_edge.get(), &is_overlapping, &fraction));
  EXPECT_EQ(1, is_overlapping);
  EXPECT_NEAR(0.5, fraction, 0.05);

  // Public APIs: harp_geometry_has_point_in_area,
  // harp_geometry_has_area_overlap, harp_geometry_get_area.
  std::array<double, 4> lat_a = {0.0, 0.0, 10.0, 10.0};
  std::array<double, 4> lon_a = {0.0, 10.0, 10.0, 0.0};
  std::array<double, 4> lat_b = {5.0, 5.0, 15.0, 15.0};
  std::array<double, 4> lon_b = {5.0, 15.0, 15.0, 5.0};
  std::array<double, 4> bad_lat = {0.0, 10.0, 10.0, 0.0};
  std::array<double, 4> bad_lon = {0.0, 10.0, 0.0, 10.0};

  int in_area = -1;
  ASSERT_EQ(HARP_SUCCESS,
            harp_geometry_has_point_in_area(5.0, 5.0, 4, lat_a.data(),
                                            lon_a.data(), &in_area));
  EXPECT_EQ(1, in_area);
  ASSERT_EQ(HARP_SUCCESS,
            harp_geometry_has_point_in_area(25.0, 25.0, 4, lat_a.data(),
                                            lon_a.data(), &in_area));
  EXPECT_EQ(0, in_area);
  EXPECT_EQ(-1, harp_geometry_has_point_in_area(5.0, 5.0, 4, bad_lat.data(),
                                                bad_lon.data(), &in_area));

  int has_overlap = -1;
  double pub_fraction = -1.0;
  ASSERT_EQ(HARP_SUCCESS, harp_geometry_has_area_overlap(
                              4, lat_a.data(), lon_a.data(), 4, lat_b.data(),
                              lon_b.data(), &has_overlap, &pub_fraction));
  EXPECT_EQ(1, has_overlap);
  EXPECT_GT(pub_fraction, 0.2);
  EXPECT_LT(pub_fraction, 0.3);

  ASSERT_EQ(HARP_SUCCESS, harp_geometry_has_area_overlap(
                              4, lat_a.data(), lon_a.data(), 4, lat_b.data(),
                              lon_b.data(), &has_overlap, nullptr));
  EXPECT_EQ(1, has_overlap);

  EXPECT_EQ(-1, harp_geometry_has_area_overlap(
                    4, bad_lat.data(), bad_lon.data(), 4, lat_b.data(),
                    lon_b.data(), &has_overlap, nullptr));
  EXPECT_EQ(-1, harp_geometry_has_area_overlap(4, lat_a.data(), lon_a.data(), 4,
                                               bad_lat.data(), bad_lon.data(),
                                               &has_overlap, nullptr));

  // Area of one octant of the WGS84 sphere (0..90 deg lon, 0..90 deg lat
  // approximated by square -> test 10x10 deg and antimeridian-wrapping square).
  double area = -1.0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_geometry_get_area(4, lat_a.data(), lon_a.data(), &area));
  EXPECT_GT(area, 1.0e12);
  EXPECT_LT(area, 1.3e12);

  // Prime-meridian-crossing polygon surface area exercises lonC +/- 2*pi
  // wrapping in spherical_polygon_get_surface_area.
  std::array<double, 4> pm_lat = {-5.0, -5.0, 5.0, 5.0};
  std::array<double, 4> pm_lon = {355.0, 5.0, 5.0, 355.0};
  double pm_area = -1.0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_geometry_get_area(4, pm_lat.data(), pm_lon.data(), &pm_area));
  EXPECT_NEAR(area, pm_area, 5e10);

  EXPECT_EQ(-1,
            harp_geometry_get_area(4, bad_lat.data(), bad_lon.data(), &area));
}

}  // namespace
}  // namespace stcorp_harp
