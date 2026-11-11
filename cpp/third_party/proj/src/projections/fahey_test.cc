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
// Unit tests for the Fahey pseudocylindrical projection
// (proj/src/projections/fahey.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/fahey.cpp

#include <cmath>
#include <numbers>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;
constexpr double kFaheyXScale = 0.819152;
constexpr double kFaheyYScale = 1.819152;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(FaheyTest, MetadataAndForcedSphericalSetup) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_ellps(proj_create(ctx.get(), "+proj=fahey +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_ellps.get());
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj_ellps.get());
  EXPECT_STREQ("fahey", info.id);
  EXPECT_NE(std::string_view(pj_ellps->descr).find("Fahey"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj_ellps.get()));

  // PJ_PROJECTION(fahey) forces P->es = 0.0, so an ellipsoidal setup with
  // WGS84 semimajor axis must match a pure sphere of the same radius.
  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=fahey +a=6378137.0"));
  ASSERT_NE(nullptr, pj_sphere.get());

  const PJ_COORD in = proj_coord(DegToRad(35.0), DegToRad(-42.0), 0.0, 0.0);
  const PJ_COORD out_ellps = proj_trans(pj_ellps.get(), PJ_FWD, in);
  const PJ_COORD out_sphere = proj_trans(pj_sphere.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(out_sphere.xy.x, out_ellps.xy.x, 1e-9);
  EXPECT_NEAR(out_sphere.xy.y, out_ellps.xy.y, 1e-9);
}

TEST(FaheyTest, ForwardAnalyticalFormulaOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=fahey +R=1"));
  ASSERT_NE(nullptr, pj.get());

  struct AngleCase {
    double lon_rad;
    double lat_rad;
  };
  constexpr AngleCase kCases[] = {
      {.lon_rad = 0.0, .lat_rad = 0.0},
      {.lon_rad = DegToRad(60.0), .lat_rad = 0.0},
      {.lon_rad = 0.0, .lat_rad = DegToRad(45.0)},
      {.lon_rad = DegToRad(45.0), .lat_rad = DegToRad(30.0)},
      {.lon_rad = DegToRad(-120.0), .lat_rad = DegToRad(60.0)},
      {.lon_rad = DegToRad(90.0), .lat_rad = DegToRad(-40.0)},
      {.lon_rad = DegToRad(-75.0), .lat_rad = DegToRad(-25.0)},
      // At the poles, tan(0.5 * +-pi/2) = +-1, so x collapses to 0.
      {.lon_rad = DegToRad(135.0), .lat_rad = kHalfPi},
      {.lon_rad = DegToRad(-135.0), .lat_rad = -kHalfPi},
  };

  for (const auto& tc : kCases) {
    const double t = std::tan(0.5 * tc.lat_rad);
    const double expected_x =
        kFaheyXScale * tc.lon_rad * std::sqrt(std::max(0.0, 1.0 - t * t));
    const double expected_y = kFaheyYScale * t;

    const PJ_COORD out =
        proj_trans(pj.get(), PJ_FWD, proj_coord(tc.lon_rad, tc.lat_rad, 0, 0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected_x, out.xy.x, 1e-12);
    EXPECT_NEAR(expected_y, out.xy.y, 1e-12);
  }
}

TEST(FaheyTest, ForwardAndInverseRoundTripAcrossQuadrants) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=fahey +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 15.0, .lat_deg = 52.0},
      {.lon_deg = -77.0, .lat_deg = 38.9},
      {.lon_deg = 151.2, .lat_deg = -33.86},
      {.lon_deg = -58.4, .lat_deg = -34.6},
      {.lon_deg = 179.5, .lat_deg = 85.0},
  };

  for (const auto& tc : kCases) {
    const PJ_LP in_lp = {.lam = DegToRad(tc.lon_deg),
                         .phi = DegToRad(tc.lat_deg)};
    // 1. Classic 2D pj_fwd / pj_inv round-trip.
    const PJ_XY xy = pj_fwd(in_lp, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_LP out_lp = pj_inv(xy, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in_lp.lam, out_lp.lam, 1e-12);
    EXPECT_NEAR(in_lp.phi, out_lp.phi, 1e-12);

    // 2. 4D proj_trans round-trip.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(in_lp.lam, in_lp.phi, 0, 0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in_lp.lam, inv.lp.lam, 1e-12);
    EXPECT_NEAR(in_lp.phi, inv.lp.phi, 1e-12);
  }
}

TEST(FaheyTest, InverseNearPoleToleranceClampsLongitudeToZero) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=fahey +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. At the exact north and south poles (|y / 1.819152| == 1), 1 - t^2 == 0,
  // which is < TOL (1e-6), so fahey_s_inverse sets lam = 0 regardless of x.
  const PJ_COORD north_pole_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.42, kFaheyYScale, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, north_pole_inv.lp.lam);
  EXPECT_NEAR(kHalfPi, north_pole_inv.lp.phi, 1e-12);

  const PJ_COORD south_pole_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(-0.42, -kFaheyYScale, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, south_pole_inv.lp.lam);
  EXPECT_NEAR(-kHalfPi, south_pole_inv.lp.phi, 1e-12);

  // 2. Slightly inside the TOL (1e-6) band: 1 - t^2 = 0.5e-6 < 1e-6 -> lam = 0.
  const double t_inside = std::sqrt(1.0 - 0.5e-6);
  const PJ_COORD inside_tol_inv = proj_trans(
      pj.get(), PJ_INV, proj_coord(0.1, kFaheyYScale * t_inside, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, inside_tol_inv.lp.lam);

  // 3. Slightly outside the TOL (1e-6) band: 1 - t^2 = 2.0e-6 >= 1e-6 ->
  // non-zero longitude is recovered via xy.x / (0.819152 * sqrt(1 - t^2)).
  constexpr double kExpectedLon = 0.35;
  const double u_outside = 2.0e-6;
  const double t_outside = std::sqrt(1.0 - u_outside);
  const double x_outside = kFaheyXScale * kExpectedLon * std::sqrt(u_outside);
  const PJ_COORD outside_tol_inv =
      proj_trans(pj.get(), PJ_INV,
                 proj_coord(x_outside, kFaheyYScale * t_outside, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kExpectedLon, outside_tol_inv.lp.lam, 1e-9);
}

TEST(FaheyTest, BatchTransformFactorsAndDomainErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(
      ctx.get(), "+proj=fahey +R=6371000 +lon_0=10 +x_0=50000 +y_0=-25000"));
  ASSERT_NE(nullptr, pj.get());

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(10.0), 0.0, 0.0, 0.0),
      proj_coord(DegToRad(25.0), DegToRad(35.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  EXPECT_NEAR(50000.0, coords[0].xy.x, 1e-6);
  EXPECT_NEAR(-25000.0, coords[0].xy.y, 1e-6);

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(DegToRad(10.0), coords[0].lp.lam, 1e-12);
  EXPECT_NEAR(0.0, coords[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(25.0), coords[1].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(35.0), coords[1].lp.phi, 1e-12);

  // Verify distortion factors at an interior point.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(DegToRad(20.0), DegToRad(30.0), 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(factors.meridional_scale, 0.0);
  EXPECT_GT(factors.parallel_scale, 0.0);
  EXPECT_GT(factors.areal_scale, 0.0);

  // Latitude beyond pi/2 is rejected by fwd_prepare.
  ScopedLogSuppressor suppress_logs(ctx.get());
  const PJ_COORD bad =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, kHalfPi + 0.1, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
}

}  // namespace
}  // namespace proj
