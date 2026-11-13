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
// Unit tests for libharp geographic utility functions
// (libharp/harp-geometry-util.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-geometry-util.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-geometry.h

#include <array>
#include <cmath>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

struct GeographicAverageCase {
  double lat_p;
  double lon_p;
  double lat_q;
  double lon_q;
  double expected_lat;
  double expected_lon;
};

constexpr GeographicAverageCase kAverageCases[] = {
    // Identical point.
    {.lat_p = 15.0,
     .lon_p = 30.0,
     .lat_q = 15.0,
     .lon_q = 30.0,
     .expected_lat = 15.0,
     .expected_lon = 30.0},
    // Along equator.
    {.lat_p = 0.0,
     .lon_p = 0.0,
     .lat_q = 0.0,
     .lon_q = 90.0,
     .expected_lat = 0.0,
     .expected_lon = 45.0},
    // Along prime meridian.
    {.lat_p = 0.0,
     .lon_p = 0.0,
     .lat_q = 60.0,
     .lon_q = 0.0,
     .expected_lat = 30.0,
     .expected_lon = 0.0},
    // Across the antimeridian: midpoint lies at +/-180 deg.
    {.lat_p = 0.0,
     .lon_p = 170.0,
     .lat_q = 0.0,
     .lon_q = -170.0,
     .expected_lat = 0.0,
     .expected_lon = 180.0},
    // Symmetric points straddling the equator.
    {.lat_p = -30.0,
     .lon_p = 20.0,
     .lat_q = 30.0,
     .lon_q = 20.0,
     .expected_lat = 0.0,
     .expected_lon = 20.0},
};

TEST(HarpGeometryUtilTest, GeographicAverageMidpointOnGreatCircle) {
  for (const auto& tc : kAverageCases) {
    double lat_u = -999.0;
    double lon_u = -999.0;
    harp_geographic_average(tc.lat_p, tc.lon_p, tc.lat_q, tc.lon_q, &lat_u,
                            &lon_u);
    EXPECT_NEAR(tc.expected_lat, lat_u, 1e-10);
    // Treat +180 and -180 longitudes equivalently at the antimeridian.
    if (std::abs(tc.expected_lon) == 180.0) {
      EXPECT_NEAR(180.0, std::abs(lon_u), 1e-10);
    } else {
      EXPECT_NEAR(tc.expected_lon, lon_u, 1e-10);
    }
  }
}

TEST(HarpGeometryUtilTest, GeographicIntersectionFindsCrossingAndCoincident) {
  double lat_u = -999.0;
  double lon_u = -999.0;

  // Equator intersected with prime meridian meets at (0, 0).
  harp_geographic_intersection(0.0, -10.0, 0.0, 10.0, -10.0, 0.0, 10.0, 0.0,
                               &lat_u, &lon_u);
  EXPECT_NEAR(0.0, lat_u, 1e-10);
  EXPECT_NEAR(0.0, lon_u, 1e-10);

  // Diagonal great circles crossing at (0, 45).
  harp_geographic_intersection(-10.0, 35.0, 10.0, 55.0, -10.0, 55.0, 10.0, 35.0,
                               &lat_u, &lon_u);
  EXPECT_NEAR(0.0, lat_u, 1e-10);
  EXPECT_NEAR(45.0, lon_u, 1e-10);

  // Coincident great circles (both along equator) yield norm_u == 0 -> NaN.
  harp_geographic_intersection(0.0, 0.0, 0.0, 10.0, 0.0, 20.0, 0.0, 30.0,
                               &lat_u, &lon_u);
  EXPECT_TRUE(std::isnan(lat_u));
  EXPECT_TRUE(std::isnan(lon_u));

  // Identical endpoints on first pair also yield norm_u == 0 -> NaN.
  harp_geographic_intersection(10.0, 20.0, 10.0, 20.0, 0.0, 0.0, 10.0, 0.0,
                               &lat_u, &lon_u);
  EXPECT_TRUE(std::isnan(lat_u));
  EXPECT_TRUE(std::isnan(lon_u));
}

TEST(HarpGeometryUtilTest, GeographicExtrapolationReflectsAcrossPivot) {
  double lat_u = -999.0;
  double lon_u = -999.0;

  // Extrapolating q=(0, 0) through p=(0, 10) yields u=(0, 20).
  harp_geographic_extrapolation(0.0, 10.0, 0.0, 0.0, &lat_u, &lon_u);
  EXPECT_NEAR(0.0, lat_u, 1e-10);
  EXPECT_NEAR(20.0, lon_u, 1e-10);

  // Extrapolating along meridian: q=(0, 0) through p=(30, 0) yields u=(60, 0).
  harp_geographic_extrapolation(30.0, 0.0, 0.0, 0.0, &lat_u, &lon_u);
  EXPECT_NEAR(60.0, lat_u, 1e-10);
  EXPECT_NEAR(0.0, lon_u, 1e-10);

  // Average of q and extrapolated u recovers pivot p.
  harp_geographic_extrapolation(25.0, 40.0, 15.0, 20.0, &lat_u, &lon_u);
  double mid_lat = -999.0;
  double mid_lon = -999.0;
  harp_geographic_average(15.0, 20.0, lat_u, lon_u, &mid_lat, &mid_lon);
  EXPECT_NEAR(25.0, mid_lat, 1e-10);
  EXPECT_NEAR(40.0, mid_lon, 1e-10);
}

TEST(HarpGeometryUtilTest, GeographicCenterFromBoundsValidAndInvalid) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // 4-vertex square symmetric around (0, 0).
  constexpr std::array<double, 4> kSquareLat = {-5.0, -5.0, 5.0, 5.0};
  constexpr std::array<double, 4> kSquareLon = {-5.0, 5.0, 5.0, -5.0};
  double center_lat = -999.0;
  double center_lon = -999.0;
  ASSERT_EQ(HARP_SUCCESS, harp_geographic_center_from_bounds(
                              4, kSquareLat.data(), kSquareLon.data(),
                              &center_lat, &center_lon));
  EXPECT_NEAR(0.0, center_lat, 1e-10);
  EXPECT_NEAR(0.0, center_lon, 1e-10);

  // 2-vertex bounding box [10, 20] x [30, 40] -> center near (15, 35).
  constexpr std::array<double, 2> kBoxLat = {10.0, 20.0};
  constexpr std::array<double, 2> kBoxLon = {30.0, 40.0};
  ASSERT_EQ(HARP_SUCCESS,
            harp_geographic_center_from_bounds(
                2, kBoxLat.data(), kBoxLon.data(), &center_lat, &center_lon));
  EXPECT_NEAR(15.0, center_lat, 0.1);
  EXPECT_NEAR(35.0, center_lon, 0.1);

  // Degenerate 2-vertex bounding box with equal latitudes triggers polygon
  // error.
  constexpr std::array<double, 2> kDegenerateLat = {10.0, 10.0};
  EXPECT_EQ(-1, harp_geographic_center_from_bounds(2, kDegenerateLat.data(),
                                                   kBoxLon.data(), &center_lat,
                                                   &center_lon));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
}

TEST(HarpGeometryUtilTest, GetGridCornerCoordinatesCoversInteriorAndBoundary) {
  // 3x3 regular grid of cell centers at lat/lon in {-1, 0, 1} deg so all
  // boundary branches (i==0, 0<i<num_x, i==num_x, j==0, 0<j<num_y, j==num_y)
  // are exercised.
  constexpr long kNumX = 3;
  constexpr long kNumY = 3;
  std::array<double, kNumX * kNumY> lat{};
  std::array<double, kNumX * kNumY> lon{};
  for (long i = 0; i < kNumX; ++i) {
    for (long j = 0; j < kNumY; ++j) {
      lat[i * kNumY + j] = static_cast<double>(i - 1);
      lon[i * kNumY + j] = static_cast<double>(j - 1);
    }
  }

  std::array<double, (kNumX + 1) * (kNumY + 1)> lat_edge{};
  std::array<double, (kNumX + 1) * (kNumY + 1)> lon_edge{};
  lat_edge.fill(-999.0);
  lon_edge.fill(-999.0);

  harp_get_grid_corner_coordinates(kNumX, kNumY, lon.data(), lat.data(),
                                   lon_edge.data(), lat_edge.data());

  for (long i = 0; i <= kNumX; ++i) {
    for (long j = 0; j <= kNumY; ++j) {
      const double expected_lat = static_cast<double>(i) - 1.5;
      // harp_geographic_center_from_bounds normalizes longitudes to [0, 360).
      double expected_lon = static_cast<double>(j) - 1.5;
      if (expected_lon < 0.0) {
        expected_lon += 360.0;
      }
      EXPECT_NEAR(expected_lat, lat_edge[i * (kNumY + 1) + j], 0.05);
      EXPECT_NEAR(expected_lon, lon_edge[i * (kNumY + 1) + j], 0.05);
    }
  }
}

}  // namespace
}  // namespace stcorp_harp
