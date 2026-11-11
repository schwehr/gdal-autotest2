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
// Unit tests for the Aitoff (aitoff) and Winkel Tripel (wintri) projections
// (proj/src/projections/aitoff.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/aitoff.cpp

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(AitoffTest, MetadataForcedSphericalAndInvalidWintriLat1) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Both aitoff and wintri force P->es = 0 even when +ellps=WGS84 is passed.
  ScopedPj aitoff_wgs84(proj_create(ctx.get(), "+proj=aitoff +ellps=WGS84"));
  ScopedPj aitoff_sph(proj_create(ctx.get(), "+proj=aitoff +R=6378137"));
  ASSERT_NE(nullptr, aitoff_wgs84.get());
  ASSERT_NE(nullptr, aitoff_sph.get());
  EXPECT_STREQ("aitoff", proj_pj_info(aitoff_wgs84.get()).id);
  EXPECT_EQ(1, proj_pj_info(aitoff_wgs84.get()).has_inverse);
  EXPECT_EQ(1, proj_angular_input(aitoff_wgs84.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(aitoff_wgs84.get(), PJ_FWD));
  EXPECT_EQ(0.0, aitoff_wgs84->es);

  ScopedPj wintri_default(proj_create(ctx.get(), "+proj=wintri +ellps=WGS84"));
  ASSERT_NE(nullptr, wintri_default.get());
  EXPECT_STREQ("wintri", proj_pj_info(wintri_default.get()).id);
  EXPECT_EQ(1, proj_pj_info(wintri_default.get()).has_inverse);
  EXPECT_EQ(0.0, wintri_default->es);

  const PJ_COORD pt = proj_coord(DegToRad(45.0), DegToRad(30.0), 0.0, 0.0);
  const PJ_COORD fwd_wgs84 = proj_trans(aitoff_wgs84.get(), PJ_FWD, pt);
  const PJ_COORD fwd_sph = proj_trans(aitoff_sph.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(aitoff_wgs84.get()));
  EXPECT_EQ(0, proj_errno(aitoff_sph.get()));
  EXPECT_NEAR(fwd_sph.xy.x, fwd_wgs84.xy.x, 1e-9);
  EXPECT_NEAR(fwd_sph.xy.y, fwd_wgs84.xy.y, 1e-9);

  // 2. Upstream aitoff.cpp checks `cos(lat_1) == 0.` for |lat_1| == 90 deg,
  // which never triggers in IEEE-754 double (cos(pi/2) ~= 6.12e-17 != 0.0).
  // Pin that creation succeeds with near-zero cosphi1.
  ScopedPj wintri_90(proj_create(ctx.get(), "+proj=wintri +R=1 +lat_1=90"));
  ASSERT_NE(nullptr, wintri_90.get());
  const PJ_COORD aitoff_pt =
      proj_trans(aitoff_sph.get(), PJ_FWD,
                 proj_coord(DegToRad(60.0), DegToRad(30.0), 0.0, 0.0));
  const PJ_COORD w90_pt =
      proj_trans(wintri_90.get(), PJ_FWD,
                 proj_coord(DegToRad(60.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(aitoff_sph.get()));
  EXPECT_EQ(0, proj_errno(wintri_90.get()));
  EXPECT_NEAR(0.5 * (aitoff_pt.xy.x / 6378137.0), w90_pt.xy.x, 1e-12);
}

TEST(AitoffTest, AitoffAnalyticalPointsRoundTripAndOverDoubleCover) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=aitoff +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Origin exercises sin_alpha == 0 forward branch and alpha == 0 inverse.
  const PJ_COORD origin_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, origin_fwd.xy.x, 1e-15);
  EXPECT_NEAR(0.0, origin_fwd.xy.y, 1e-15);
  const PJ_COORD origin_inv = proj_trans(pj.get(), PJ_INV, origin_fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, origin_inv.lp.lam, 1e-15);
  EXPECT_NEAR(0.0, origin_inv.lp.phi, 1e-15);

  // 2. Equator extreme (+/-180 deg) -> (+/-pi, 0), Poles -> (0, +/-pi/2).
  const PJ_COORD east_eq =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kPi, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kPi, east_eq.xy.x, 1e-12);
  EXPECT_NEAR(0.0, east_eq.xy.y, 1e-12);

  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, north_pole.xy.x, 1e-12);
  EXPECT_NEAR(kHalfPi, north_pole.xy.y, 1e-12);
  const PJ_COORD pole_inv = proj_trans(pj.get(), PJ_INV, north_pole);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kHalfPi, pole_inv.lp.phi, 1e-12);

  // 3. Round-trip across all four quadrants + near-origin relative accuracy.
  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 1e-9, .lat_deg = -1e-9},
      {.lon_deg = 45.0, .lat_deg = 30.0},
      {.lon_deg = -120.0, .lat_deg = 60.0},
      {.lon_deg = 165.0, .lat_deg = -75.0},
      {.lon_deg = -175.0, .lat_deg = -85.0},
      {.lon_deg = 0.0, .lat_deg = -90.0},
  };
  for (const auto& tc : kCases) {
    const double lon_rad = DegToRad(tc.lon_deg);
    const double lat_rad = DegToRad(tc.lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-12) << "lat=" << tc.lat_deg;
    if (std::abs(tc.lat_deg) < 89.9) {
      EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-12) << "lon=" << tc.lon_deg;
    }
  }

  // 4. Closed-form inverse double-covers the ellipse x^2 + 4*y^2 <= (2*pi)^2
  // in +over mode (e.g. lon = 240 deg).
  ScopedPj pj_over(proj_create(ctx.get(), "+proj=aitoff +R=1 +over"));
  ASSERT_NE(nullptr, pj_over.get());
  const double over_lon = DegToRad(240.0);
  const double over_lat = DegToRad(20.0);
  const PJ_COORD over_fwd = proj_trans(
      pj_over.get(), PJ_FWD, proj_coord(over_lon, over_lat, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_over.get()));
  const PJ_COORD over_inv = proj_trans(pj_over.get(), PJ_INV, over_fwd);
  EXPECT_EQ(0, proj_errno(pj_over.get()));
  EXPECT_NEAR(over_lon, over_inv.lp.lam, 1e-12);
  EXPECT_NEAR(over_lat, over_inv.lp.phi, 1e-12);
}

TEST(AitoffTest, AitoffInverseOutsideEllipseReturnsError) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=aitoff +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // Closed-form inverse domain is the ellipse (x/2)^2 + y^2 <= pi^2.
  struct OutsideCase {
    double x;
    double y;
  };
  constexpr OutsideCase kOutside[] = {
      {.x = 6.5, .y = 0.0},  {.x = -6.5, .y = 0.0}, {.x = 0.0, .y = 3.2},
      {.x = 0.0, .y = -3.2}, {.x = 4.5, .y = 2.5},
  };

  for (const auto& tc : kOutside) {
    const PJ_COORD inv =
        proj_trans(pj.get(), PJ_INV, proj_coord(tc.x, tc.y, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(inv.lp.lam)) << "x=" << tc.x << " y=" << tc.y;
    EXPECT_TRUE(std::isinf(inv.lp.phi)) << "x=" << tc.x << " y=" << tc.y;
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()))
        << "x=" << tc.x << " y=" << tc.y;
    proj_errno_reset(pj.get());
  }
}

TEST(AitoffTest, WintriLat1RelationshipRoundTripAndInverseEdges) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj aitoff(proj_create(ctx.get(), "+proj=aitoff +R=1"));
  ScopedPj wintri_def(proj_create(ctx.get(), "+proj=wintri +R=1"));
  ScopedPj wintri_eq(proj_create(ctx.get(), "+proj=wintri +R=1 +lat_1=0"));
  ScopedPj wintri_60(proj_create(ctx.get(), "+proj=wintri +R=1 +lat_1=60"));
  ASSERT_NE(nullptr, aitoff.get());
  ASSERT_NE(nullptr, wintri_def.get());
  ASSERT_NE(nullptr, wintri_eq.get());
  ASSERT_NE(nullptr, wintri_60.get());

  // 1. Verify Winkel Tripel is the exact mean of Aitoff and Equirectangular
  // with standard parallel lat_1 (default cosphi1 = 2/pi).
  const double test_lon = DegToRad(60.0);
  const double test_lat = DegToRad(35.0);
  const PJ_COORD pt = proj_coord(test_lon, test_lat, 0.0, 0.0);
  const PJ_COORD a_xy = proj_trans(aitoff.get(), PJ_FWD, pt);
  const PJ_COORD w_def = proj_trans(wintri_def.get(), PJ_FWD, pt);
  const PJ_COORD w_eq = proj_trans(wintri_eq.get(), PJ_FWD, pt);
  const PJ_COORD w_60 = proj_trans(wintri_60.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(aitoff.get()));
  EXPECT_EQ(0, proj_errno(wintri_def.get()));
  EXPECT_EQ(0, proj_errno(wintri_eq.get()));
  EXPECT_EQ(0, proj_errno(wintri_60.get()));
  EXPECT_NEAR(0.5 * (a_xy.xy.x + test_lon * (2.0 / kPi)), w_def.xy.x, 1e-12);
  EXPECT_NEAR(0.5 * (a_xy.xy.y + test_lat), w_def.xy.y, 1e-12);
  EXPECT_NEAR(0.5 * (a_xy.xy.x + test_lon * 1.0), w_eq.xy.x, 1e-12);
  EXPECT_NEAR(0.5 * (a_xy.xy.x + test_lon * 0.5), w_60.xy.x, 1e-12);

  // 2. Round-trip Winkel Tripel across origin, near-zero shortcut, quadrants,
  // high latitudes, and poles for both default and custom +lat_1.
  struct WintriCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr WintriCase kWintriCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 30.0},
      {.lon_deg = -135.0, .lat_deg = 55.0},
      {.lon_deg = 175.0, .lat_deg = -75.0},
      {.lon_deg = -60.0, .lat_deg = -89.0},
      {.lon_deg = 0.0, .lat_deg = 90.0},
      {.lon_deg = 0.0, .lat_deg = -90.0},
  };
  for (PJ* p : {wintri_def.get(), wintri_60.get()}) {
    for (const auto& tc : kWintriCases) {
      const double lon_rad = DegToRad(tc.lon_deg);
      const double lat_rad = DegToRad(tc.lat_deg);
      const PJ_COORD fwd =
          proj_trans(p, PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(p));
      const PJ_COORD inv = proj_trans(p, PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(p));
      EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-10) << "lat=" << tc.lat_deg;
      if (std::abs(tc.lat_deg) < 89.5) {
        EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-10) << "lon=" << tc.lon_deg;
      }
    }
  }

  // 3. Near-zero inverse shortcut (|x| < 1e-12 && |y| < 1e-12) returns (0, 0).
  const PJ_COORD tiny_inv =
      proj_trans(wintri_def.get(), PJ_INV, proj_coord(1e-13, -1e-13, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(wintri_def.get()));
  EXPECT_EQ(0.0, tiny_inv.lp.lam);
  EXPECT_EQ(0.0, tiny_inv.lp.phi);

  // 4. Newton singularity branch (denom == 0): at (0, 1e-10), cos(1e-10) rounds
  // to 1.0 in IEEE-754 double, so C = 1 - D^2 == 0 and denom == 0 triggers
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  ScopedLogSuppressor suppress(ctx.get());
  const PJ_COORD sing_inv =
      proj_trans(wintri_def.get(), PJ_INV, proj_coord(0.0, 1e-10, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(sing_inv.lp.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(wintri_def.get()));
  proj_errno_reset(wintri_def.get());

  // 5. Beyond +/-pi/2 y-extents exercise the Newton polar symmetry reflection
  // branches (lp.phi > pi/2 and lp.phi < -pi/2).
  const PJ_COORD above_pole =
      proj_trans(wintri_def.get(), PJ_INV, proj_coord(0.0, 1.6, 0.0, 0.0));
  EXPECT_LE(std::abs(above_pole.lp.phi), kHalfPi);
  proj_errno_reset(wintri_def.get());

  const PJ_COORD below_pole =
      proj_trans(wintri_def.get(), PJ_INV, proj_coord(0.0, -1.6, 0.0, 0.0));
  EXPECT_LE(std::abs(below_pole.lp.phi), kHalfPi);
  proj_errno_reset(wintri_def.get());
}

TEST(AitoffTest, FactorsLinearUnitsBatchAndSpecialValues) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj aitoff(proj_create(ctx.get(), "+proj=aitoff +R=6371000"));
  ScopedPj wintri(proj_create(ctx.get(), "+proj=wintri +R=6371000"));
  ASSERT_NE(nullptr, aitoff.get());
  ASSERT_NE(nullptr, wintri.get());

  // 1. Scale factors at the origin equal 1.0 in both projections (with
  // wintri x-scale = 0.5*(1 + 2/pi) along equator and 1.0 along meridian).
  const PJ_FACTORS a_fac =
      proj_factors(aitoff.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(aitoff.get()));
  EXPECT_NEAR(1.0, a_fac.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, a_fac.parallel_scale, 1e-6);

  const PJ_FACTORS w_fac = proj_factors(
      wintri.get(), proj_coord(DegToRad(30.0), DegToRad(40.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(wintri.get()));
  EXPECT_GT(w_fac.meridional_scale, 0.0);
  EXPECT_GT(w_fac.parallel_scale, 0.0);
  EXPECT_GT(w_fac.areal_scale, 0.0);

  // 2. +lon_0, +x_0, +y_0, +units=km, and 4D (z, t) pass-through.
  ScopedPj wintri_km(proj_create(
      ctx.get(),
      "+proj=wintri +R=6371000 +lon_0=10 +x_0=50000 +y_0=100000 +units=km"));
  ASSERT_NE(nullptr, wintri_km.get());
  const PJ_COORD pt_4d =
      proj_coord(DegToRad(25.0), DegToRad(-15.0), 321.5, 2026.75);
  const PJ_COORD fwd_km = proj_trans(wintri_km.get(), PJ_FWD, pt_4d);
  EXPECT_EQ(0, proj_errno(wintri_km.get()));
  EXPECT_NEAR(0.3215, fwd_km.xyzt.z, 1e-12);
  EXPECT_NEAR(2026.75, fwd_km.xyzt.t, 1e-12);
  const PJ_COORD inv_km = proj_trans(wintri_km.get(), PJ_INV, fwd_km);
  EXPECT_EQ(0, proj_errno(wintri_km.get()));
  EXPECT_NEAR(pt_4d.lpzt.lam, inv_km.lpzt.lam, 1e-10);
  EXPECT_NEAR(pt_4d.lpzt.phi, inv_km.lpzt.phi, 1e-10);
  EXPECT_NEAR(321.5, inv_km.lpzt.z, 1e-12);
  EXPECT_NEAR(2026.75, inv_km.lpzt.t, 1e-12);

  // 3. Batch transforms via proj_trans_array and proj_trans_generic.
  std::array<PJ_COORD, 2> batch = {
      proj_coord(DegToRad(-40.0), DegToRad(20.0), 0.0, 0.0),
      proj_coord(DegToRad(70.0), DegToRad(-50.0), 0.0, 0.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(aitoff.get(), PJ_FWD, batch.size(), batch.data()));
  ASSERT_EQ(0,
            proj_trans_array(aitoff.get(), PJ_INV, batch.size(), batch.data()));
  EXPECT_NEAR(DegToRad(-40.0), batch[0].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(20.0), batch[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(70.0), batch[1].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-50.0), batch[1].lp.phi, 1e-12);

  std::array<double, 2> gx = {DegToRad(15.0), DegToRad(-85.0)};
  std::array<double, 2> gy = {DegToRad(25.0), DegToRad(-45.0)};
  EXPECT_EQ(2u,
            proj_trans_generic(wintri.get(), PJ_FWD, gx.data(), sizeof(double),
                               gx.size(), gy.data(), sizeof(double), gy.size(),
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(wintri.get()));
  EXPECT_EQ(2u,
            proj_trans_generic(wintri.get(), PJ_INV, gx.data(), sizeof(double),
                               gx.size(), gy.data(), sizeof(double), gy.size(),
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(wintri.get()));
  EXPECT_NEAR(DegToRad(15.0), gx[0], 1e-10);
  EXPECT_NEAR(DegToRad(25.0), gy[0], 1e-10);
  EXPECT_NEAR(DegToRad(-85.0), gx[1], 1e-10);
  EXPECT_NEAR(DegToRad(-45.0), gy[1], 1e-10);

  // 4. NaN and HUGE_VAL edge inputs.
  const double kNan = std::numeric_limits<double>::quiet_NaN();
  const PJ_COORD nan_res =
      proj_trans(aitoff.get(), PJ_FWD, proj_coord(kNan, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_res.xy.x));
  EXPECT_TRUE(std::isnan(nan_res.xy.y));

  const PJ_COORD inf_res =
      proj_trans(wintri.get(), PJ_FWD, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_res.xy.x));
}

// TODO(schwehr): Unreachable lines in proj/src/projections/aitoff.cpp without
// fault injection or upstream code changes:
// - OOM null checks on calloc in PJ_PROJECTION(aitoff) and
//   PJ_PROJECTION(wintri) (lines 255-256, 266-267).
// - Lines 273-276 (`cos(lat_1) == 0.`): IEEE-754 double `cos(90 * DEG_TO_RAD)`
//   evaluates to ~6.12e-17, never exact 0.0.
// - Lines 235-237 (`iter == MAXITER && round == MAXROUND`): post-increment on
//   `iter++ < MAXITER` and `round++ < MAXROUND` leaves iter==11 / round==21 on
//   loop exhaustion, so the condition does not fire on non-convergence.

}  // namespace
}  // namespace proj
