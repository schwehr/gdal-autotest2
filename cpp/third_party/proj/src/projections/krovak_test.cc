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
// Unit tests for Krovak and Modified Krovak projections
// (proj/src/projections/krovak.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/krovak.cpp

#include <array>
#include <cmath>
#include <cstddef>
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

TEST(KrovakTest, SetupDefaultsCustomParametersAndIllegalLat0) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Default +proj=krovak populates Bessel parameters and Krovak defaults.
  {
    ScopedPj pj(proj_create(ctx.get(), "+proj=krovak"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(6377397.155, pj->a);
    EXPECT_DOUBLE_EQ(0.006674372230614, pj->es);
    EXPECT_DOUBLE_EQ(0.863937979737193, pj->phi0);
    EXPECT_DOUBLE_EQ(0.7417649320975901 - 0.308341501185665, pj->lam0);
    EXPECT_DOUBLE_EQ(0.9999, pj->k0);

    const PJ_PROJ_INFO info = proj_pj_info(pj.get());
    EXPECT_STREQ("krovak", info.id);
    EXPECT_EQ(1, info.has_inverse);
    EXPECT_EQ(1, pj_has_inverse(pj.get()));
    EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD));
    EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD));

    const PJ_COORD fwd = proj_trans(
        pj.get(), PJ_FWD, proj_coord(DegToRad(14.4), DegToRad(50.1), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(-744215.096543, fwd.xy.x, 1e-4);
    EXPECT_NEAR(-1041500.076857, fwd.xy.y, 1e-4);
  }

  // 2. Default +proj=mod_krovak sets modified flag and applies correction.
  {
    ScopedPj pj(proj_create(ctx.get(), "+proj=mod_krovak"));
    ASSERT_NE(nullptr, pj.get());
    const PJ_PROJ_INFO info = proj_pj_info(pj.get());
    EXPECT_STREQ("mod_krovak", info.id);

    const PJ_COORD fwd = proj_trans(
        pj.get(), PJ_FWD, proj_coord(DegToRad(14.4), DegToRad(50.1), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(-744215.121415, fwd.xy.x, 1e-4);
    EXPECT_NEAR(-1041500.050573, fwd.xy.y, 1e-4);
  }

  // 3. Explicit +lat_0, +lon_0, +k_0, and +x_0/+y_0 overrides.
  {
    ScopedPj custom(proj_create(ctx.get(),
                                "+proj=krovak +lat_0=50 +lon_0=25 +k_0=0.9998 "
                                "+x_0=500000 +y_0=500000 +ellps=bessel"));
    ASSERT_NE(nullptr, custom.get());
    EXPECT_DOUBLE_EQ(0.9998, custom->k0);

    const double lon = DegToRad(14.4);
    const double lat = DegToRad(50.1);
    const PJ_COORD fwd =
        proj_trans(custom.get(), PJ_FWD, proj_coord(lon, lat, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(custom.get()));
    EXPECT_NEAR(-1255948.452553, fwd.xy.x, 1e-4);
    EXPECT_NEAR(-1539639.750795, fwd.xy.y, 1e-4);

    const PJ_COORD inv = proj_trans(custom.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(custom.get()));
    EXPECT_NEAR(lon, inv.lp.lam, 1e-12);
    EXPECT_NEAR(lat, inv.lp.phi, 1e-12);
  }

  // 4. Explicit +k alias also suppresses default k0 = 0.9999.
  {
    ScopedPj with_k(
        proj_create(ctx.get(), "+proj=krovak +k=0.9998 +ellps=bessel"));
    ASSERT_NE(nullptr, with_k.get());
    EXPECT_DOUBLE_EQ(0.9998, with_k->k0);
  }

  // 5. +lat_0=-90 makes tan(phi0/2 + pi/4) == 0.0 and triggers setup error.
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    for (const char* def :
         {"+proj=krovak +lat_0=-90", "+proj=mod_krovak +lat_0=-90"}) {
      logs.Clear();
      ScopedPj bad(proj_create(ctx.get(), def));
      EXPECT_EQ(nullptr, bad.get()) << def;
      EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
                proj_context_errno(ctx.get()))
          << def;
      EXPECT_TRUE(logs.ContainsMessage("Invalid value for lat_0")) << def;
    }
  }
}

TEST(KrovakTest, ForwardAndInverseRoundTripDefaultAndCzechModes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj east_north(proj_create(ctx.get(), "+proj=krovak +ellps=bessel"));
  ScopedPj west_south(
      proj_create(ctx.get(), "+proj=krovak +czech +ellps=bessel"));
  ASSERT_NE(nullptr, east_north.get());
  ASSERT_NE(nullptr, west_south.get());

  struct PtCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr PtCase kCases[] = {
      {.lon_deg = 14.4, .lat_deg = 50.1},
      {.lon_deg = 16.61, .lat_deg = 49.19},
      {.lon_deg = 17.11, .lat_deg = 48.15},
      {.lon_deg = 22.0, .lat_deg = 48.0},
      {.lon_deg = 15.0, .lat_deg = 46.0},
      {.lon_deg = 15.0, .lat_deg = 52.0},
  };

  for (const auto& tc : kCases) {
    const double lon = DegToRad(tc.lon_deg);
    const double lat = DegToRad(tc.lat_deg);
    const PJ_COORD fwd_en = proj_trans(east_north.get(), PJ_FWD,
                                       proj_coord(lon, lat, 321.5, 2026.0));
    const PJ_COORD fwd_ws = proj_trans(west_south.get(), PJ_FWD,
                                       proj_coord(lon, lat, 321.5, 2026.0));
    EXPECT_EQ(0, proj_errno(east_north.get()));
    EXPECT_EQ(0, proj_errno(west_south.get()));

    // With x0 = y0 = 0, East/North coordinates are negated West/South coords.
    EXPECT_NEAR(-fwd_ws.xyzt.x, fwd_en.xyzt.x, 1e-8);
    EXPECT_NEAR(-fwd_ws.xyzt.y, fwd_en.xyzt.y, 1e-8);
    EXPECT_DOUBLE_EQ(321.5, fwd_en.xyzt.z);
    EXPECT_DOUBLE_EQ(2026.0, fwd_en.xyzt.t);

    const PJ_COORD inv_en = proj_trans(east_north.get(), PJ_INV, fwd_en);
    const PJ_COORD inv_ws = proj_trans(west_south.get(), PJ_INV, fwd_ws);
    EXPECT_EQ(0, proj_errno(east_north.get()));
    EXPECT_EQ(0, proj_errno(west_south.get()));
    EXPECT_NEAR(lon, inv_en.lpzt.lam, 1e-11);
    EXPECT_NEAR(lat, inv_en.lpzt.phi, 1e-11);
    EXPECT_NEAR(lon, inv_ws.lpzt.lam, 1e-11);
    EXPECT_NEAR(lat, inv_ws.lpzt.phi, 1e-11);
    EXPECT_DOUBLE_EQ(321.5, inv_en.lpzt.z);
    EXPECT_DOUBLE_EQ(2026.0, inv_ws.lpzt.t);
  }

  // Asymmetric false origin round-trips accurately in +czech mode.
  ScopedPj czech_offset(proj_create(
      ctx.get(), "+proj=krovak +czech +x_0=100000 +y_0=200000 +ellps=bessel"));
  ASSERT_NE(nullptr, czech_offset.get());
  const PJ_COORD fwd_off =
      proj_trans(czech_offset.get(), PJ_FWD,
                 proj_coord(DegToRad(14.4), DegToRad(50.1), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(czech_offset.get()));
  EXPECT_NEAR(844215.096543, fwd_off.xy.x, 1e-4);
  EXPECT_NEAR(1241500.076857, fwd_off.xy.y, 1e-4);
  const PJ_COORD inv_off = proj_trans(czech_offset.get(), PJ_INV, fwd_off);
  EXPECT_EQ(0, proj_errno(czech_offset.get()));
  EXPECT_NEAR(DegToRad(14.4), inv_off.lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(50.1), inv_off.lp.phi, 1e-12);
}

TEST(KrovakTest, ModifiedKrovakPolynomialCorrectionAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj mod_en(proj_create(ctx.get(), "+proj=mod_krovak +ellps=bessel"));
  ScopedPj mod_ws(
      proj_create(ctx.get(), "+proj=mod_krovak +czech +ellps=bessel"));
  ScopedPj reg_ws(proj_create(ctx.get(), "+proj=krovak +czech +ellps=bessel"));
  ASSERT_NE(nullptr, mod_en.get());
  ASSERT_NE(nullptr, mod_ws.get());
  ASSERT_NE(nullptr, reg_ws.get());

  // Verify exact constant offset (C2, C1) at reduced coordinates (Xr=0, Yr=0),
  // corresponding to Czech Westing = Y0 = 654000, Southing = X0 = 1089000.
  {
    const PJ_COORD x0y0_pt =
        proj_coord(0.2745628946662816, 0.868825066112171, 0.0, 0.0);
    const PJ_COORD reg_fwd = proj_trans(reg_ws.get(), PJ_FWD, x0y0_pt);
    const PJ_COORD mod_fwd = proj_trans(mod_ws.get(), PJ_FWD, x0y0_pt);
    EXPECT_EQ(0, proj_errno(reg_ws.get()));
    EXPECT_EQ(0, proj_errno(mod_ws.get()));
    EXPECT_NEAR(654000.0, reg_fwd.xy.x, 1e-6);
    EXPECT_NEAR(1089000.0, reg_fwd.xy.y, 1e-6);
    EXPECT_NEAR(0.02515965696, reg_fwd.xy.x - mod_fwd.xy.x, 1e-8);
    EXPECT_NEAR(0.02946529277, reg_fwd.xy.y - mod_fwd.xy.y, 1e-8);
  }

  // Round-trip across both regional and off-center coordinates.
  // Modified Krovak inverse evaluates the polynomial at reduced coordinates
  // without iteration, leaving ~1e-10 rad (~0.6 mm) residual far off-center.
  constexpr double kCoordsDeg[][2] = {
      {14.4, 50.1}, {18.1, 49.8}, {21.25, 48.72}, {10.0, 45.0}, {28.0, 52.0}};
  for (const auto& pt : kCoordsDeg) {
    const double lon = DegToRad(pt[0]);
    const double lat = DegToRad(pt[1]);
    for (PJ* pj : {mod_en.get(), mod_ws.get()}) {
      const PJ_COORD fwd =
          proj_trans(pj, PJ_FWD, proj_coord(lon, lat, 10.0, 2025.0));
      EXPECT_EQ(0, proj_errno(pj));
      const PJ_COORD inv = proj_trans(pj, PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj));
      EXPECT_NEAR(lon, inv.lpzt.lam, 1e-9);
      EXPECT_NEAR(lat, inv.lpzt.phi, 1e-9);
      EXPECT_DOUBLE_EQ(10.0, inv.lpzt.z);
      EXPECT_DOUBLE_EQ(2025.0, inv.lpzt.t);
    }
  }
}

TEST(KrovakTest, ObliquePoleSingularityAndDomainErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Oblique cone pole: lam = lam0, phi_pole -> s = pi/2, cos_s < 1e-12.
  {
    ScopedPj reg_en(proj_create(ctx.get(), "+proj=krovak +ellps=bessel"));
    ScopedPj reg_ws(
        proj_create(ctx.get(), "+proj=krovak +czech +ellps=bessel"));
    ASSERT_NE(nullptr, reg_en.get());
    ASSERT_NE(nullptr, reg_ws.get());

    const double lon_pole = reg_en->lam0;
    constexpr double kPhiPoleRad = 1.0429668480103937;
    const PJ_COORD pole_fwd = proj_trans(
        reg_en.get(), PJ_FWD, proj_coord(lon_pole, kPhiPoleRad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(reg_en.get()));
    EXPECT_DOUBLE_EQ(0.0, pole_fwd.xy.x);
    EXPECT_DOUBLE_EQ(0.0, pole_fwd.xy.y);

    // Inverse of exact origin (0, 0) exercises rho == 0.0 branch -> s = pi/2.
    for (PJ* pj : {reg_en.get(), reg_ws.get()}) {
      const PJ_COORD origin_inv =
          proj_trans(pj, PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj));
      EXPECT_NEAR(lon_pole, origin_inv.lp.lam, 1e-11);
      EXPECT_NEAR(kPhiPoleRad, origin_inv.lp.phi, 1e-11);
    }
  }

  // 2. Overflow in Modified Krovak fourth-power polynomial produces NaN and
  //    exhausts MAX_ITER (i == 0), setting OUTSIDE_PROJECTION_DOMAIN.
  {
    ScopedPj mod(proj_create(ctx.get(), "+proj=mod_krovak +ellps=bessel"));
    ASSERT_NE(nullptr, mod.get());
    const PJ_COORD overflow_inv =
        proj_trans(mod.get(), PJ_INV, proj_coord(1e80, 1e80, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, overflow_inv.lpzt.lam);
    EXPECT_EQ(HUGE_VAL, overflow_inv.lpzt.phi);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(mod.get()));

    proj_errno_reset(mod.get());
    EXPECT_EQ(0, proj_errno(mod.get()));
    const PJ_COORD valid =
        proj_trans(mod.get(), PJ_FWD,
                   proj_coord(DegToRad(14.4), DegToRad(50.1), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(mod.get()));
    EXPECT_TRUE(std::isfinite(valid.xy.x));
  }

  // 3. Invalid inputs (NaN, HUGE_VAL, out-of-range latitude).
  {
    ScopedLogSuppressor suppress_logs(ctx.get());
    ScopedPj pj(proj_create(ctx.get(), "+proj=krovak +ellps=bessel"));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD nan_coord = proj_trans(
        pj.get(), PJ_FWD,
        proj_coord(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isnan(nan_coord.xyzt.x));

    const PJ_COORD inf_coord =
        proj_trans(pj.get(), PJ_FWD, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, inf_coord.xyzt.x);

    const PJ_COORD bad_lat =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, kHalfPi + 0.1, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, bad_lat.xyzt.x);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
    proj_errno_reset(pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
  }
}

TEST(KrovakTest, FactorsBatchArraysClassicEntryPointsAndRoundTripApi) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj east_north(proj_create(ctx.get(), "+proj=krovak +ellps=bessel"));
  ScopedPj west_south(
      proj_create(ctx.get(), "+proj=krovak +czech +ellps=bessel"));
  ASSERT_NE(nullptr, east_north.get());
  ASSERT_NE(nullptr, west_south.get());

  // 1. Conformality and convergence factors at Prague (14.4E, 50.1N).
  const PJ_COORD prague = proj_coord(DegToRad(14.4), DegToRad(50.1), 0.0, 0.0);
  const PJ_FACTORS fac_en = proj_factors(east_north.get(), prague);
  EXPECT_EQ(0, proj_errno(east_north.get()));
  EXPECT_NEAR(0.9999039818, fac_en.meridional_scale, 1e-8);
  EXPECT_NEAR(0.9999039818, fac_en.parallel_scale, 1e-8);
  EXPECT_NEAR(0.9998079728, fac_en.areal_scale, 1e-8);
  EXPECT_LT(fac_en.angular_distortion, 1e-9);
  EXPECT_NEAR(-0.13699908, fac_en.meridian_convergence, 1e-6);

  const PJ_FACTORS fac_ws = proj_factors(west_south.get(), prague);
  EXPECT_EQ(0, proj_errno(west_south.get()));
  EXPECT_NEAR(fac_en.meridian_convergence + kPi, fac_ws.meridian_convergence,
              1e-6);

  // 2. Classic 2D entry points pj_fwd / pj_inv.
  const PJ_XY xy = pj_fwd(prague.lp, east_north.get());
  EXPECT_EQ(0, proj_errno(east_north.get()));
  const PJ_LP lp = pj_inv(xy, east_north.get());
  EXPECT_EQ(0, proj_errno(east_north.get()));
  EXPECT_NEAR(prague.lp.lam, lp.lam, 1e-12);
  EXPECT_NEAR(prague.lp.phi, lp.phi, 1e-12);

  // 3. Batch array API (all-valid and mixed invalid coordinate error tracking).
  std::array<PJ_COORD, 2> pts = {
      proj_coord(DegToRad(14.4), DegToRad(50.1), 0.0, 0.0),
      proj_coord(DegToRad(16.61), DegToRad(49.19), 200.0, 1.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(east_north.get(), PJ_FWD, pts.size(), pts.data()));
  EXPECT_NEAR(-744215.096543, pts[0].xy.x, 1e-4);
  ASSERT_EQ(0,
            proj_trans_array(east_north.get(), PJ_INV, pts.size(), pts.data()));
  EXPECT_NEAR(DegToRad(14.4), pts[0].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(49.19), pts[1].lp.phi, 1e-12);
  EXPECT_DOUBLE_EQ(200.0, pts[1].lpz.z);

  {
    ScopedLogSuppressor suppress_logs(ctx.get());
    std::array<PJ_COORD, 2> mixed = {
        proj_coord(DegToRad(14.4), DegToRad(50.1), 0.0, 0.0),
        proj_coord(0.0, kHalfPi + 0.1, 0.0, 0.0),
    };
    EXPECT_EQ(
        PROJ_ERR_COORD_TRANSFM_INVALID_COORD,
        proj_trans_array(east_north.get(), PJ_FWD, mixed.size(), mixed.data()));
    EXPECT_NEAR(-744215.096543, mixed[0].xy.x, 1e-4);
    EXPECT_EQ(HUGE_VAL, mixed[1].xy.x);
    proj_errno_reset(east_north.get());
    EXPECT_EQ(0, proj_errno(east_north.get()));
  }

  // 4. Generic strided batch transformation.
  std::array<double, 2> x = {DegToRad(14.4), DegToRad(16.61)};
  std::array<double, 2> y = {DegToRad(50.1), DegToRad(49.19)};
  const std::size_t done = proj_trans_generic(
      west_south.get(), PJ_FWD, x.data(), sizeof(double), x.size(), y.data(),
      sizeof(double), y.size(), nullptr, 0, 0, nullptr, 0, 0);
  EXPECT_EQ(2U, done);
  EXPECT_EQ(0, proj_errno(west_south.get()));
  EXPECT_NEAR(744215.096543, x[0], 1e-4);

  // 5. Sub-micrometer round-trip accuracy via proj_roundtrip.
  PJ_COORD rt_pt = prague;
  const double rt_dist_m = proj_roundtrip(east_north.get(), PJ_FWD, 5, &rt_pt);
  EXPECT_EQ(0, proj_errno(east_north.get()));
  EXPECT_LT(rt_dist_m, 1e-6);
}

}  // namespace
}  // namespace proj
