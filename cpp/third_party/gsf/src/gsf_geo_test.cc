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
// Tests geographic position and offset helpers (src/gsf_geo.c).
// No platform #ifdefs exist in src/gsf_geo.c; all lines compile on Linux.
//
// See also:
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf_geo.c
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf.h

#include "third_party/gsf/src/gsf.h"

#include <cmath>
#include <numbers>
#include <string_view>
#include <type_traits>

#include "testing/base/public/gunit.h"

namespace gsf {
namespace {

static_assert(sizeof(GSF_POSITION) == 24);
static_assert(sizeof(GSF_POSITION_OFFSETS) == 24);
static_assert(std::is_standard_layout_v<GSF_POSITION>);
static_assert(std::is_trivially_copyable_v<GSF_POSITION>);
static_assert(std::is_standard_layout_v<GSF_POSITION_OFFSETS>);
static_assert(std::is_trivially_copyable_v<GSF_POSITION_OFFSETS>);

// WGS-84 semi-major and semi-minor axes (meters) matching gp_info[WGS_1984]
// in src/gsf_geo.c.
constexpr double kWgs84SemiMajorMeters = 6378137.0;
constexpr double kWgs84SemiMinorMeters = 6356752.314;

constexpr double DegToRad(double deg) {
  return deg * (std::numbers::pi / 180.0);
}

constexpr double RadToDeg(double rad) {
  return rad * (180.0 / std::numbers::pi);
}

struct EllipsoidRadii {
  double parallel_radius_m;    // N(phi) * cos(phi), meters per radian of lon.
  double meridional_radius_m;  // M(phi), meters per radian of lat.
};

// Computes WGS-84 principal radii independently via eccentricity e^2 rather
// than the reduced-latitude parameterization in gsf_geo.c.
EllipsoidRadii ComputeWgs84Radii(double lat_deg) {
  const double phi = DegToRad(lat_deg);
  const double b_over_a = kWgs84SemiMinorMeters / kWgs84SemiMajorMeters;
  const double e2 = 1.0 - b_over_a * b_over_a;
  const double sin_phi = std::sin(phi);
  const double denom_sq = 1.0 - e2 * sin_phi * sin_phi;
  const double denom = std::sqrt(denom_sq);
  return {
      .parallel_radius_m = (kWgs84SemiMajorMeters / denom) * std::cos(phi),
      .meridional_radius_m =
          kWgs84SemiMajorMeters * (1.0 - e2) / (denom * denom_sq),
  };
}

// Analytical single-step Euler destination in the ship frame (+x forward,
// +y starboard, +z down, +hdg clockwise from north).
GSF_POSITION ExpectedSingleStepDestination(GSF_POSITION gp,
                                           GSF_POSITION_OFFSETS offsets,
                                           double hdg_deg) {
  const EllipsoidRadii radii = ComputeWgs84Radii(gp.lat);
  const double hdg_rad = DegToRad(hdg_deg);
  const double east_m =
      offsets.x * std::sin(hdg_rad) + offsets.y * std::cos(hdg_rad);
  const double north_m =
      offsets.x * std::cos(hdg_rad) - offsets.y * std::sin(hdg_rad);
  return {
      .lon = gp.lon + RadToDeg(east_m / radii.parallel_radius_m),
      .lat = gp.lat + RadToDeg(north_m / radii.meridional_radius_m),
      .z = gp.z + offsets.z,
  };
}

// Analytical single-step inverse offsets for |dlon| <= PI. Negates the
// displacement to match gsfGetPositionOffsets's upstream sign convention and
// pins its initial iz = gp_from.z accumulation quirk.
GSF_POSITION_OFFSETS ExpectedSingleStepOffsets(GSF_POSITION gp_from,
                                               GSF_POSITION gp_to,
                                               double hdg_deg) {
  const EllipsoidRadii radii = ComputeWgs84Radii(gp_from.lat);
  const double east_m =
      radii.parallel_radius_m * DegToRad(gp_to.lon - gp_from.lon);
  const double north_m =
      radii.meridional_radius_m * DegToRad(gp_to.lat - gp_from.lat);
  const double hdg_rad = DegToRad(hdg_deg);
  return {
      .x = -(north_m * std::cos(hdg_rad) + east_m * std::sin(hdg_rad)),
      .y = -(east_m * std::cos(hdg_rad) - north_m * std::sin(hdg_rad)),
      .z = gp_from.z - (gp_to.z - gp_from.z),
  };
}

// Immediately copies *ptr by value; gsfGetPositionDestination returns a pointer
// to a function-local static struct that must never be freed.
GSF_POSITION CopyDestination(GSF_POSITION gp, GSF_POSITION_OFFSETS offsets,
                             double hdg_deg, double dist_step) {
  GSF_POSITION* pos =
      gsfGetPositionDestination(gp, offsets, hdg_deg, dist_step);
  EXPECT_NE(pos, nullptr);
  return pos != nullptr ? *pos : GSF_POSITION{};
}

// Immediately copies *ptr by value; gsfGetPositionOffsets returns a pointer to
// a function-local static struct that must never be freed.
GSF_POSITION_OFFSETS CopyOffsets(GSF_POSITION gp_from, GSF_POSITION gp_to,
                                 double hdg_deg, double dist_step) {
  GSF_POSITION_OFFSETS* off =
      gsfGetPositionOffsets(gp_from, gp_to, hdg_deg, dist_step);
  EXPECT_NE(off, nullptr);
  return off != nullptr ? *off : GSF_POSITION_OFFSETS{};
}

TEST(GsfGeoTest, DestinationZeroOffsetsClampsIterationsToOne) {
  // Zero offsets yield iter == 0 from floor(0 / dist_step), exercising the
  // |x| <= |y| branch and the iter <= 0 clamp to 1 without dividing by zero.
  constexpr GSF_POSITION kStart{.lon = -122.4194, .lat = 37.7749, .z = 42.5};
  constexpr GSF_POSITION_OFFSETS kZero{.x = 0.0, .y = 0.0, .z = 0.0};

  const GSF_POSITION dest = CopyDestination(kStart, kZero, 123.0, 50.0);
  EXPECT_NEAR(kStart.lon, dest.lon, 1e-12);
  EXPECT_NEAR(kStart.lat, dest.lat, 1e-12);
  EXPECT_NEAR(kStart.z, dest.z, 1e-12);
}

TEST(GsfGeoTest, DestinationStepLargerThanOffsetsClampsIterationsToOne) {
  struct TestCase {
    std::string_view name;
    GSF_POSITION_OFFSETS offsets;
    double hdg_deg;
  };
  // All cases have max(|x|, |y|) < dist_step (100 m), hitting floor(...) == 0
  // -> iter = 1 on both sides of |offsets.x| > |offsets.y|.
  constexpr TestCase kCases[] = {
      {
          .name = "x_dominates_positive_z",
          .offsets = {.x = 50.0, .y = -20.0, .z = 12.5},
          .hdg_deg = 30.0,
      },
      {
          .name = "y_dominates_negative_z",
          .offsets = {.x = -20.0, .y = 50.0, .z = -7.5},
          .hdg_deg = 160.0,
      },
      {
          .name = "equal_magnitudes_hit_else_branch",
          .offsets = {.x = -30.0, .y = 30.0, .z = 0.0},
          .hdg_deg = 270.0,
      },
  };
  constexpr GSF_POSITION kStart{.lon = -70.5, .lat = 41.25, .z = 10.0};

  for (const TestCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const GSF_POSITION actual =
        CopyDestination(kStart, tc.offsets, tc.hdg_deg, 100.0);
    const GSF_POSITION expected =
        ExpectedSingleStepDestination(kStart, tc.offsets, tc.hdg_deg);
    EXPECT_NEAR(expected.lon, actual.lon, 1e-10);
    EXPECT_NEAR(expected.lat, actual.lat, 1e-10);
    EXPECT_NEAR(expected.z, actual.z, 1e-12);
  }
}

TEST(GsfGeoTest, DestinationCardinalAndDiagonalHeadings) {
  struct TestCase {
    std::string_view name;
    double hdg_deg;
    GSF_POSITION_OFFSETS offsets;
    double dist_step;
    int expected_dlon_sign;
    int expected_dlat_sign;
  };
  constexpr TestCase kCases[] = {
      {
          .name = "north_forward_only_exact_iter_one",
          .hdg_deg = 0.0,
          .offsets = {.x = 100.0, .y = 0.0, .z = 5.0},
          .dist_step = 100.0,  // floor(100 / 100) == 1 skips iter <= 0 clamp.
          .expected_dlon_sign = 0,
          .expected_dlat_sign = 1,
      },
      {
          .name = "north_starboard_goes_east",
          .hdg_deg = 0.0,
          .offsets = {.x = 0.0, .y = 100.0, .z = -5.0},
          .dist_step = 100.0,
          .expected_dlon_sign = 1,
          .expected_dlat_sign = 0,
      },
      {
          .name = "east_forward_goes_east",
          .hdg_deg = 90.0,
          .offsets = {.x = 150.0, .y = 0.0, .z = 3.25},
          .dist_step = 1000.0,
          .expected_dlon_sign = 1,
          .expected_dlat_sign = 0,
      },
      {
          .name = "east_starboard_goes_south",
          .hdg_deg = 90.0,
          .offsets = {.x = 0.0, .y = 150.0, .z = -3.25},
          .dist_step = 1000.0,
          .expected_dlon_sign = 0,
          .expected_dlat_sign = -1,
      },
      {
          .name = "south_forward_goes_south",
          .hdg_deg = 180.0,
          .offsets = {.x = 200.0, .y = 0.0, .z = 0.0},
          .dist_step = 1000.0,
          .expected_dlon_sign = 0,
          .expected_dlat_sign = -1,
      },
      {
          .name = "west_forward_goes_west",
          .hdg_deg = 270.0,
          .offsets = {.x = 200.0, .y = 0.0, .z = 11.0},
          .dist_step = 1000.0,
          .expected_dlon_sign = -1,
          .expected_dlat_sign = 0,
      },
      {
          .name = "negative_heading_matches_west",
          .hdg_deg = -90.0,
          .offsets = {.x = 200.0, .y = -50.0, .z = -11.0},
          .dist_step = 1000.0,
          .expected_dlon_sign = -1,
          .expected_dlat_sign = -1,
      },
      {
          .name = "diagonal_northeast_combined_offsets",
          .hdg_deg = 45.0,
          .offsets = {.x = 120.0, .y = -40.0, .z = 2.5},
          .dist_step = 1000.0,
          .expected_dlon_sign = 1,
          .expected_dlat_sign = 1,
      },
      {
          .name = "diagonal_northwest_combined_offsets",
          .hdg_deg = 315.0,
          .offsets = {.x = 80.0, .y = 160.0, .z = -2.5},
          .dist_step = 1000.0,
          .expected_dlon_sign = 1,
          .expected_dlat_sign = 1,
      },
  };
  constexpr GSF_POSITION kStart{.lon = -45.0, .lat = 35.0, .z = -20.0};

  for (const TestCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const GSF_POSITION actual =
        CopyDestination(kStart, tc.offsets, tc.hdg_deg, tc.dist_step);
    const GSF_POSITION expected =
        ExpectedSingleStepDestination(kStart, tc.offsets, tc.hdg_deg);
    EXPECT_NEAR(expected.lon, actual.lon, 1e-10);
    EXPECT_NEAR(expected.lat, actual.lat, 1e-10);
    EXPECT_NEAR(expected.z, actual.z, 1e-12);

    if (tc.expected_dlon_sign > 0) {
      EXPECT_GT(actual.lon, kStart.lon);
    } else if (tc.expected_dlon_sign < 0) {
      EXPECT_LT(actual.lon, kStart.lon);
    } else {
      EXPECT_NEAR(kStart.lon, actual.lon, 1e-12);
    }

    if (tc.expected_dlat_sign > 0) {
      EXPECT_GT(actual.lat, kStart.lat);
    } else if (tc.expected_dlat_sign < 0) {
      EXPECT_LT(actual.lat, kStart.lat);
    } else {
      EXPECT_NEAR(kStart.lat, actual.lat, 1e-12);
    }
  }
}

TEST(GsfGeoTest, DestinationAcrossLatitudesMatchesEllipsoidCurvature) {
  struct TestCase {
    std::string_view name;
    double lat_deg;
    GSF_POSITION_OFFSETS offsets;
  };
  // Avoid exact +-90 deg poles because metric() divides by cos(phi).
  constexpr TestCase kCases[] = {
      {.name = "equator_x_dom",
       .lat_deg = 0.0,
       .offsets = {.x = 200.0, .y = 50.0, .z = 10.0}},
      {.name = "low_lat_y_dom",
       .lat_deg = 15.0,
       .offsets = {.x = 50.0, .y = 200.0, .z = -10.0}},
      {.name = "southern_mid_equal_xy",
       .lat_deg = -45.0,
       .offsets = {.x = 150.0, .y = -150.0, .z = 4.0}},
      {.name = "high_north_80",
       .lat_deg = 80.0,
       .offsets = {.x = 200.0, .y = 50.0, .z = 0.0}},
      {.name = "high_south_80",
       .lat_deg = -80.0,
       .offsets = {.x = 50.0, .y = -200.0, .z = -4.0}},
      {.name = "polar_north_85",
       .lat_deg = 85.0,
       .offsets = {.x = 100.0, .y = 80.0, .z = 1.0}},
      {.name = "polar_south_85",
       .lat_deg = -85.0,
       .offsets = {.x = -80.0, .y = 100.0, .z = -1.0}},
  };

  for (const TestCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const GSF_POSITION start{.lon = 12.5, .lat = tc.lat_deg, .z = 0.0};
    const GSF_POSITION actual = CopyDestination(start, tc.offsets, 60.0, 500.0);
    const GSF_POSITION expected =
        ExpectedSingleStepDestination(start, tc.offsets, 60.0);
    EXPECT_NEAR(expected.lon, actual.lon, 1e-10);
    EXPECT_NEAR(expected.lat, actual.lat, 1e-10);
    EXPECT_NEAR(expected.z, actual.z, 1e-12);
  }

  // Parallel radius shrinks with cos(lat), so an identical eastward step must
  // span much more longitude at 80 deg N than at the equator.
  constexpr GSF_POSITION kEq{.lon = 0.0, .lat = 0.0, .z = 0.0};
  constexpr GSF_POSITION kHigh{.lon = 0.0, .lat = 80.0, .z = 0.0};
  constexpr GSF_POSITION_OFFSETS kEast100m{.x = 100.0, .y = 0.0, .z = 0.0};
  const GSF_POSITION eq_dest = CopyDestination(kEq, kEast100m, 90.0, 500.0);
  const GSF_POSITION high_dest = CopyDestination(kHigh, kEast100m, 90.0, 500.0);
  EXPECT_GT(high_dest.lon, eq_dest.lon * 5.0);
}

TEST(GsfGeoTest, DestinationMultiStepUpdatesMetricAlongPath) {
  // 100 km northeast from 45 deg N: multi-step (iter == 1000) recomputes
  // metric(ilat, ...) as latitude rises, shifting longitude by ~0.005 deg
  // compared to a single Euler step (iter == 1).
  constexpr GSF_POSITION kStart{.lon = 0.0, .lat = 45.0, .z = 10.0};
  constexpr GSF_POSITION_OFFSETS kForward100km{
      .x = 100000.0, .y = 0.0, .z = -50.0};
  const GSF_POSITION single =
      CopyDestination(kStart, kForward100km, 45.0, 200000.0);
  const GSF_POSITION multi =
      CopyDestination(kStart, kForward100km, 45.0, 100.0);

  EXPECT_NEAR(-40.0, single.z, 1e-9);
  EXPECT_NEAR(-40.0, multi.z, 1e-9);
  EXPECT_GT(multi.lon, single.lon);
  EXPECT_GT(std::abs(multi.lon - single.lon), 1e-3);

  // Also exercise iter > 1 via the |offsets.y| >= |offsets.x| branch.
  constexpr GSF_POSITION kSouthStart{.lon = 10.0, .lat = -35.0, .z = 5.0};
  constexpr GSF_POSITION_OFFSETS kPort50km{
      .x = 1000.0, .y = -50000.0, .z = 35.5};
  const GSF_POSITION south_dest =
      CopyDestination(kSouthStart, kPort50km, 0.0, 500.0);
  const GSF_POSITION approx =
      ExpectedSingleStepDestination(kSouthStart, kPort50km, 0.0);
  EXPECT_NEAR(approx.lon, south_dest.lon, 1e-4);
  EXPECT_NEAR(approx.lat, south_dest.lat, 1e-6);
  EXPECT_NEAR(40.5, south_dest.z, 1e-9);
}

TEST(GsfGeoTest, DestinationReturnsStaticBufferOverwrittenOnNextCall) {
  constexpr GSF_POSITION kOriginA{.lon = 1.0, .lat = 2.0, .z = 3.0};
  constexpr GSF_POSITION kOriginB{.lon = -120.0, .lat = -30.0, .z = 100.0};

  GSF_POSITION* ptr_a = gsfGetPositionDestination(
      kOriginA, {.x = 100.0, .y = 0.0, .z = 5.0}, 0.0, 50.0);
  ASSERT_NE(ptr_a, nullptr);
  const GSF_POSITION copy_a = *ptr_a;

  GSF_POSITION* ptr_b = gsfGetPositionDestination(
      kOriginB, {.x = 0.0, .y = 200.0, .z = -10.0}, 90.0, 50.0);
  ASSERT_NE(ptr_b, nullptr);
  const GSF_POSITION copy_b = *ptr_b;

  // Same static storage address is reused and overwritten; never free() it.
  EXPECT_EQ(ptr_a, ptr_b);
  EXPECT_NEAR(8.0, copy_a.z, 1e-12);
  EXPECT_NEAR(90.0, copy_b.z, 1e-12);
  EXPECT_NEAR(copy_b.lon, ptr_a->lon, 1e-12);
  EXPECT_NEAR(copy_b.lat, ptr_a->lat, 1e-12);
  EXPECT_NE(copy_a.lat, ptr_a->lat);
}

TEST(GsfGeoTest, OffsetsDirectionalBranchesAndAxisDominance) {
  struct TestCase {
    std::string_view name;
    GSF_POSITION gp_from;
    GSF_POSITION gp_to;
    double hdg_deg;
  };
  // Use dist_step = 100 km so small angular steps execute iter == 1 and match
  // the analytical single-step inverse to sub-micrometer precision across all
  // lat/lon comparison and lon_diff vs lat_diff branches.
  constexpr TestCase kCases[] = {
      {
          .name = "lat_gt_lon_gt_lon_diff_dominates_hdg_90",
          .gp_from = {.lon = 10.0, .lat = 20.0, .z = 0.0},
          .gp_to = {.lon = 10.5, .lat = 20.1, .z = 12.0},
          .hdg_deg = 90.0,
      },
      {
          .name = "lat_lt_lon_lt_lat_diff_dominates_hdg_180",
          .gp_from = {.lon = 10.1, .lat = 20.5, .z = 0.0},
          .gp_to = {.lon = 10.0, .lat = 20.0, .z = -8.0},
          .hdg_deg = 180.0,
      },
      {
          .name = "lat_eq_lon_lt_southern_hemisphere_hdg_270",
          .gp_from = {.lon = 120.0, .lat = -35.0, .z = 0.0},
          .gp_to = {.lon = 119.8, .lat = -35.0, .z = 3.0},
          .hdg_deg = 270.0,
      },
      {
          .name = "lat_lt_lon_eq_high_north_hdg_45",
          .gp_from = {.lon = -45.0, .lat = 60.0, .z = 0.0},
          .gp_to = {.lon = -45.0, .lat = 59.8, .z = -4.5},
          .hdg_deg = 45.0,
      },
      {
          // gp_to.lat == 0.0 makes cos(gp_to.lat) == 1.0, so equal angular
          // deltas produce lon_diff == lat_diff (hits else branch).
          .name = "equal_lon_diff_and_lat_diff_at_equator_hdg_0",
          .gp_from = {.lon = 0.0, .lat = -0.01, .z = 0.0},
          .gp_to = {.lon = 0.01, .lat = 0.0, .z = 10.0},
          .hdg_deg = 0.0,
      },
      {
          .name = "high_south_lat_lon_diff_dominates",
          .gp_from = {.lon = 15.0, .lat = -80.0, .z = 0.0},
          .gp_to = {.lon = 15.5, .lat = -80.0, .z = 2.0},
          .hdg_deg = 0.0,
      },
  };

  for (const TestCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const GSF_POSITION_OFFSETS actual =
        CopyOffsets(tc.gp_from, tc.gp_to, tc.hdg_deg, 100000.0);
    const GSF_POSITION_OFFSETS expected =
        ExpectedSingleStepOffsets(tc.gp_from, tc.gp_to, tc.hdg_deg);
    EXPECT_NEAR(expected.x, actual.x, 1e-6);
    EXPECT_NEAR(expected.y, actual.y, 1e-6);
    EXPECT_NEAR(expected.z, actual.z, 1e-12);
  }
}

TEST(GsfGeoTest, OffsetsMultiStepAcrossLatitudes) {
  struct TestCase {
    std::string_view name;
    GSF_POSITION gp_from;
    GSF_POSITION gp_to;
    double hdg_deg;
  };
  constexpr TestCase kCases[] = {
      {.name = "equatorial_lon_dom",
       .gp_from = {.lon = 0.0, .lat = 0.0, .z = 0.0},
       .gp_to = {.lon = 0.015, .lat = 0.01, .z = -5.0},
       .hdg_deg = 0.0},
      {.name = "mid_north_lon_dom",
       .gp_from = {.lon = -70.0, .lat = 45.0, .z = 0.0},
       .gp_to = {.lon = -69.98, .lat = 44.99, .z = 5.0},
       .hdg_deg = 45.0},
      {.name = "high_south_lat_dom",
       .gp_from = {.lon = 120.0, .lat = -60.0, .z = 0.0},
       .gp_to = {.lon = 119.99, .lat = -60.015, .z = -5.0},
       .hdg_deg = 180.0},
      {.name = "high_north_lat_dom",
       .gp_from = {.lon = 10.0, .lat = 80.0, .z = 0.0},
       .gp_to = {.lon = 10.02, .lat = 80.01, .z = 5.0},
       .hdg_deg = 270.0},
  };

  for (const TestCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const GSF_POSITION_OFFSETS actual =
        CopyOffsets(tc.gp_from, tc.gp_to, tc.hdg_deg, 25.0);
    const GSF_POSITION_OFFSETS approx =
        ExpectedSingleStepOffsets(tc.gp_from, tc.gp_to, tc.hdg_deg);
    EXPECT_TRUE(std::isfinite(actual.x));
    EXPECT_TRUE(std::isfinite(actual.y));
    EXPECT_TRUE(std::isfinite(actual.z));
    EXPECT_NEAR(approx.x, actual.x, 1.0);
    EXPECT_NEAR(approx.y, actual.y, 1.0);
    EXPECT_NEAR(approx.z, actual.z, 1e-9);
  }
}

TEST(GsfGeoTest, OffsetsNonZeroFromZPinsInitialAccumulationQuirk) {
  // Upstream gsfGetPositionOffsets initializes iz = gp_from.z instead of 0.0
  // before accumulating -doz, so offsets.z == 2 * gp_from.z - gp_to.z.
  constexpr GSF_POSITION kFromA{.lon = -70.0, .lat = 40.0, .z = 100.0};
  constexpr GSF_POSITION kToA{.lon = -70.01, .lat = 40.01, .z = 125.0};
  const GSF_POSITION_OFFSETS off_a = CopyOffsets(kFromA, kToA, 0.0, 50.0);
  EXPECT_NEAR(75.0, off_a.z, 1e-9);

  constexpr GSF_POSITION kFromB{.lon = 20.0, .lat = -10.0, .z = -30.0};
  constexpr GSF_POSITION kToB{.lon = 20.01, .lat = -10.0, .z = 10.0};
  const GSF_POSITION_OFFSETS off_b = CopyOffsets(kFromB, kToB, 90.0, 50000.0);
  EXPECT_NEAR(-70.0, off_b.z, 1e-12);
}

TEST(GsfGeoTest, OffsetsDatelineCrossingBothDirections) {
  const double eq_step_02_m =
      ComputeWgs84Radii(0.0).parallel_radius_m * DegToRad(0.2);
  const double lat60_step_02_m =
      ComputeWgs84Radii(60.0).parallel_radius_m * DegToRad(0.2);

  // Westward crossing (-179.9 -> +179.9): dlon > PI and lon_diff > PI.
  // Note upstream computes dlon = ((2*PI) - dlon) / iter (> 0), matching a
  // +0.2 deg step in sign and magnitude rather than -0.2 deg.
  const GSF_POSITION_OFFSETS cross_dlon_gt_pi =
      CopyOffsets({.lon = -179.9, .lat = 0.0, .z = 0.0},
                  {.lon = 179.9, .lat = 0.0, .z = 0.0}, 0.0, 100.0);
  const GSF_POSITION_OFFSETS equiv_east_02 =
      CopyOffsets({.lon = 179.7, .lat = 0.0, .z = 0.0},
                  {.lon = 179.9, .lat = 0.0, .z = 0.0}, 0.0, 100.0);
  EXPECT_NEAR(0.0, cross_dlon_gt_pi.x, 1e-9);
  EXPECT_NEAR(-eq_step_02_m, cross_dlon_gt_pi.y, 1e-6);
  EXPECT_NEAR(equiv_east_02.y, cross_dlon_gt_pi.y, 1e-7);

  // Eastward crossing (+179.9 -> -179.9): dlon < -PI and lon_diff > PI.
  // Upstream computes dlon = -(dlon + 2*PI) / iter (< 0), matching -0.2 deg.
  const GSF_POSITION_OFFSETS cross_dlon_lt_neg_pi =
      CopyOffsets({.lon = 179.9, .lat = 0.0, .z = 0.0},
                  {.lon = -179.9, .lat = 0.0, .z = 0.0}, 0.0, 100.0);
  const GSF_POSITION_OFFSETS equiv_west_02 =
      CopyOffsets({.lon = 179.9, .lat = 0.0, .z = 0.0},
                  {.lon = 179.7, .lat = 0.0, .z = 0.0}, 0.0, 100.0);
  EXPECT_NEAR(0.0, cross_dlon_lt_neg_pi.x, 1e-9);
  EXPECT_NEAR(eq_step_02_m, cross_dlon_lt_neg_pi.y, 1e-6);
  EXPECT_NEAR(equiv_west_02.y, cross_dlon_lt_neg_pi.y, 1e-7);

  // Dateline crossing at 60 deg N with heading 90 deg (east).
  const GSF_POSITION_OFFSETS cross_60n_gt =
      CopyOffsets({.lon = -179.9, .lat = 60.0, .z = 0.0},
                  {.lon = 179.9, .lat = 60.0, .z = 0.0}, 90.0, 100.0);
  const GSF_POSITION_OFFSETS cross_60n_lt =
      CopyOffsets({.lon = 179.9, .lat = 60.0, .z = 0.0},
                  {.lon = -179.9, .lat = 60.0, .z = 0.0}, 90.0, 100.0);
  EXPECT_NEAR(-lat60_step_02_m, cross_60n_gt.x, 1e-6);
  EXPECT_NEAR(lat60_step_02_m, cross_60n_lt.x, 1e-6);

  // Dateline crossing combined with lat_diff > lon_diff in both directions.
  const GSF_POSITION_OFFSETS cross_lat_w =
      CopyOffsets({.lon = -179.9, .lat = 40.0, .z = 0.0},
                  {.lon = 179.9, .lat = 40.5, .z = 10.0}, 0.0, 100.0);
  const GSF_POSITION_OFFSETS ref_lat_w =
      CopyOffsets({.lon = 179.7, .lat = 40.0, .z = 0.0},
                  {.lon = 179.9, .lat = 40.5, .z = 10.0}, 0.0, 100.0);
  EXPECT_NEAR(ref_lat_w.x, cross_lat_w.x, 1e-5);
  EXPECT_NEAR(ref_lat_w.y, cross_lat_w.y, 1e-5);
  EXPECT_NEAR(ref_lat_w.z, cross_lat_w.z, 1e-9);

  const GSF_POSITION_OFFSETS cross_lat_e =
      CopyOffsets({.lon = 179.9, .lat = 40.5, .z = 0.0},
                  {.lon = -179.9, .lat = 40.0, .z = -10.0}, 90.0, 100.0);
  const GSF_POSITION_OFFSETS ref_lat_e =
      CopyOffsets({.lon = 179.9, .lat = 40.5, .z = 0.0},
                  {.lon = 179.7, .lat = 40.0, .z = -10.0}, 90.0, 100.0);
  EXPECT_NEAR(ref_lat_e.x, cross_lat_e.x, 1e-5);
  EXPECT_NEAR(ref_lat_e.y, cross_lat_e.y, 1e-5);
  EXPECT_NEAR(ref_lat_e.z, cross_lat_e.z, 1e-9);
}

TEST(GsfGeoTest, OffsetsAndDestinationRoundTripConsistency) {
  constexpr double kLatitudes[] = {0.0, 45.0, -60.0, 80.0};
  constexpr double kHeadings[] = {0.0, 45.0, 90.0, 180.0, 270.0};
  constexpr GSF_POSITION_OFFSETS kInputOffsets{
      .x = 5000.0, .y = -3000.0, .z = 15.0};
  constexpr double kStepMeters = 50.0;

  for (const double lat : kLatitudes) {
    for (const double hdg : kHeadings) {
      SCOPED_TRACE(lat);
      SCOPED_TRACE(hdg);
      // gp_from.z must be 0.0 so gsfGetPositionOffsets's iz = gp_from.z
      // initialization yields -offsets_in.z cleanly on round-trip.
      const GSF_POSITION gp_from{.lon = 10.0, .lat = lat, .z = 0.0};

      // 1. Project forward from gp_from to gp_to using multi-step integration.
      const GSF_POSITION gp_to =
          CopyDestination(gp_from, kInputOffsets, hdg, kStepMeters);

      // 2. Recover offsets from gp_from to gp_to; upstream returns -offsets.
      const GSF_POSITION_OFFSETS offsets_back =
          CopyOffsets(gp_from, gp_to, hdg, kStepMeters);
      EXPECT_NEAR(-kInputOffsets.x, offsets_back.x, 0.02);
      EXPECT_NEAR(-kInputOffsets.y, offsets_back.y, 0.02);
      EXPECT_NEAR(-kInputOffsets.z, offsets_back.z, 1e-9);

      // 3. Apply recovered offsets starting at gp_to to return to gp_from.
      const GSF_POSITION gp_roundtrip =
          CopyDestination(gp_to, offsets_back, hdg, kStepMeters);
      EXPECT_NEAR(gp_from.lon, gp_roundtrip.lon, 2e-5);
      EXPECT_NEAR(gp_from.lat, gp_roundtrip.lat, 1e-6);
      EXPECT_NEAR(gp_from.z, gp_roundtrip.z, 1e-9);
    }
  }
}

TEST(GsfGeoTest, OffsetsReturnsStaticBufferOverwrittenOnNextCall) {
  GSF_POSITION_OFFSETS* ptr_a =
      gsfGetPositionOffsets({.lon = 0.0, .lat = 0.0, .z = 0.0},
                            {.lon = 0.01, .lat = 0.0, .z = 12.0}, 0.0, 50.0);
  ASSERT_NE(ptr_a, nullptr);
  const GSF_POSITION_OFFSETS copy_a = *ptr_a;

  GSF_POSITION_OFFSETS* ptr_b =
      gsfGetPositionOffsets({.lon = 10.0, .lat = 20.0, .z = 0.0},
                            {.lon = 10.0, .lat = 20.02, .z = -4.0}, 0.0, 50.0);
  ASSERT_NE(ptr_b, nullptr);
  const GSF_POSITION_OFFSETS copy_b = *ptr_b;

  // Same static storage address is reused and overwritten; never free() it.
  EXPECT_EQ(ptr_a, ptr_b);
  EXPECT_NEAR(-12.0, copy_a.z, 1e-9);
  EXPECT_NEAR(4.0, copy_b.z, 1e-9);
  EXPECT_NEAR(copy_b.x, ptr_a->x, 1e-9);
  EXPECT_NEAR(copy_b.y, ptr_a->y, 1e-9);
  EXPECT_NE(copy_a.x, ptr_a->x);
}

// TODO(schwehr): src/gsf_geo.c is at 100% line coverage; consider adding +/-90
// deg polar singularity and non-positive dist_step edge-case tests.

}  // namespace
}  // namespace gsf
