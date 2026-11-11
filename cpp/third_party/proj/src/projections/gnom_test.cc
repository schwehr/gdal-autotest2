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
// Unit tests for the spherical and ellipsoidal Gnomonic projection
// (proj/src/projections/gnom.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/gnom.cpp

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::HasSubstr;

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;
constexpr double kSphereRadiusM = 6371000.0;
constexpr double kRoundTripTolSphRad = 1e-11;
// Karney's ellipsoidal gnomonic inverse converges within 0.01 * sqrt(eps)
// (~1.5e-10 rad).
constexpr double kRoundTripTolEllRad = 1e-9;
constexpr double kMetricTolM = 1e-4;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(GnomTest, InitializationInvalidArgsAndMetadata) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  struct InvalidCase {
    const char* def;
    int expected_errno;
  };
  constexpr InvalidCase kInvalidCases[] = {
      {.def = nullptr, .expected_errno = PROJ_ERR_OTHER_API_MISUSE},
      {.def = "", .expected_errno = PROJ_ERR_INVALID_OP_WRONG_SYNTAX},
      {.def = "+proj=gnom +R=0",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
      {.def = "+proj=gnom +lat_0=95 +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
  };
  for (const auto& tc : kInvalidCases) {
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj bad(proj_create(ctx.get(), tc.def));
    EXPECT_EQ(nullptr, bad.get());
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get()));
  }

  // Mutable buffers for proj_create_argv (never const_cast string literals).
  char arg0[] = "proj=gnom";
  char arg1[] = "lat_0=45";
  char arg2[] = "lon_0=10";
  char arg3[] = "ellps=WGS84";
  std::array<char*, 4> argv = {arg0, arg1, arg2, arg3};
  ScopedPj pj(
      proj_create_argv(ctx.get(), static_cast<int>(argv.size()), argv.data()));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("gnom", info.id);
  EXPECT_THAT(pj->descr, HasSubstr("Gnomonic"));
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(pj.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(pj.get(), PJ_INV));
  // gnom clears P->es = 0 after initializing Q->g.
  EXPECT_EQ(0.0, pj->es);
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));
}

TEST(GnomTest, SphericalAllFourModesAnalyticalRoundTripAndHorizonErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct LonLatDeg {
    double lon_deg;
    double lat_deg;
  };

  // 1. Equatorial mode (EQUIT).
  {
    ScopedPj pj(proj_create(ctx.get(), "+proj=gnom +lat_0=0 +R=6371000"));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD origin =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(0.0, origin.xy.x);
    EXPECT_DOUBLE_EQ(0.0, origin.xy.y);

    // Inverse near origin exercises |rh| <= EPS10 branch.
    const PJ_COORD inv_origin =
        proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(0.0, inv_origin.lp.lam);
    EXPECT_DOUBLE_EQ(0.0, inv_origin.lp.phi);

    // Analytical equatorial formula: x = R*tan(lam), y = R*tan(phi)/cos(lam).
    const double lam30 = DegToRad(30.0);
    const double phi45 = DegToRad(45.0);
    const PJ_COORD fwd_30_45 =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam30, phi45, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(kSphereRadiusM * std::tan(lam30), fwd_30_45.xy.x, kMetricTolM);
    EXPECT_NEAR(kSphereRadiusM * std::tan(phi45) / std::cos(lam30),
                fwd_30_45.xy.y, kMetricTolM);

    // Extreme y values in EQUIT inverse clamp |sin(phi)| >= 1 to +-pi/2.
    const PJ_COORD clamp_n = proj_trans(
        pj.get(), PJ_INV, proj_coord(0.0, 1.0e9 * kSphereRadiusM, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(kHalfPi, clamp_n.lp.phi, 1e-12);
    const PJ_COORD clamp_s = proj_trans(
        pj.get(), PJ_INV, proj_coord(0.0, -1.0e9 * kSphereRadiusM, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(-kHalfPi, clamp_s.lp.phi, 1e-12);

    constexpr LonLatDeg kEquitCases[] = {
        {.lon_deg = 15.0, .lat_deg = 20.0},
        {.lon_deg = -45.0, .lat_deg = -30.0},
        {.lon_deg = 60.0, .lat_deg = 50.0},
        {.lon_deg = 0.0, .lat_deg = 75.0},
    };
    for (const auto& tc : kEquitCases) {
      const PJ_COORD in =
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
      const PJ_COORD inv =
          proj_trans(pj.get(), PJ_INV, proj_trans(pj.get(), PJ_FWD, in));
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(in.lp.lam, inv.lp.lam, kRoundTripTolSphRad);
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, kRoundTripTolSphRad);
    }

    // Horizon (90 deg) and far hemisphere fail with cos(c) <= EPS10.
    for (const double bad_lon_deg : {90.0, 120.0}) {
      const PJ_COORD err = proj_trans(
          pj.get(), PJ_FWD, proj_coord(DegToRad(bad_lon_deg), 0.0, 0.0, 0.0));
      EXPECT_TRUE(std::isinf(err.xy.x));
      EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
                proj_errno(pj.get()));
      proj_errno_reset(pj.get());
    }
  }

  // 2. Oblique mode (OBLIQ).
  {
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=gnom +lat_0=45 +lon_0=10 +R=6371000"));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD inv_origin =
        proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(DegToRad(10.0), inv_origin.lp.lam, 1e-12);
    EXPECT_NEAR(DegToRad(45.0), inv_origin.lp.phi, 1e-12);

    constexpr LonLatDeg kObliqCases[] = {
        {.lon_deg = 25.0, .lat_deg = 55.0},
        {.lon_deg = -10.0, .lat_deg = 20.0},
        {.lon_deg = 40.0, .lat_deg = 70.0},
        {.lon_deg = 10.0, .lat_deg = 89.0},
    };
    for (const auto& tc : kObliqCases) {
      const PJ_COORD in =
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
      const PJ_COORD inv =
          proj_trans(pj.get(), PJ_INV, proj_trans(pj.get(), PJ_FWD, in));
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(in.lp.lam, inv.lp.lam, kRoundTripTolSphRad);
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, kRoundTripTolSphRad);
    }

    // North pole maps to (0, R * cot(45)) = (0, R) and inverts back to 90N.
    const PJ_COORD fwd_pole = proj_trans(
        pj.get(), PJ_FWD, proj_coord(DegToRad(10.0), kHalfPi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, fwd_pole.xy.x, kMetricTolM);
    EXPECT_NEAR(kSphereRadiusM, fwd_pole.xy.y, kMetricTolM);
    const PJ_COORD inv_pole = proj_trans(pj.get(), PJ_INV, fwd_pole);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(kHalfPi, inv_pole.lp.phi, 1e-7);

    // Horizon (10E, 45S is 90 deg away) and antipode fail.
    const PJ_COORD horizon_err =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(10.0), DegToRad(-45.0), 0.0, 0.0));
    EXPECT_TRUE(std::isinf(horizon_err.xy.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }

  // 3. North Pole (N_POLE) and South Pole (S_POLE) modes.
  {
    ScopedPj pj_n(proj_create(ctx.get(), "+proj=gnom +lat_0=90 +R=6371000"));
    ScopedPj pj_s(proj_create(ctx.get(), "+proj=gnom +lat_0=-90 +R=6371000"));
    ASSERT_NE(nullptr, pj_n.get());
    ASSERT_NE(nullptr, pj_s.get());

    const PJ_COORD inv_n0 =
        proj_trans(pj_n.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_NEAR(kHalfPi, inv_n0.lp.phi, 1e-12);
    const PJ_COORD inv_s0 =
        proj_trans(pj_s.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_NEAR(-kHalfPi, inv_s0.lp.phi, 1e-12);

    // Analytical polar check: at 60N/60S and 30E, rho = R * tan(30 deg).
    const double rho = kSphereRadiusM * std::tan(DegToRad(30.0));
    const PJ_COORD fwd_n =
        proj_trans(pj_n.get(), PJ_FWD,
                   proj_coord(DegToRad(30.0), DegToRad(60.0), 0.0, 0.0));
    EXPECT_NEAR(rho * std::sin(DegToRad(30.0)), fwd_n.xy.x, kMetricTolM);
    EXPECT_NEAR(-rho * std::cos(DegToRad(30.0)), fwd_n.xy.y, kMetricTolM);
    const PJ_COORD inv_n = proj_trans(pj_n.get(), PJ_INV, fwd_n);
    EXPECT_NEAR(DegToRad(30.0), inv_n.lp.lam, kRoundTripTolSphRad);
    EXPECT_NEAR(DegToRad(60.0), inv_n.lp.phi, kRoundTripTolSphRad);

    const PJ_COORD fwd_s =
        proj_trans(pj_s.get(), PJ_FWD,
                   proj_coord(DegToRad(30.0), DegToRad(-60.0), 0.0, 0.0));
    EXPECT_NEAR(rho * std::sin(DegToRad(30.0)), fwd_s.xy.x, kMetricTolM);
    EXPECT_NEAR(rho * std::cos(DegToRad(30.0)), fwd_s.xy.y, kMetricTolM);
    const PJ_COORD inv_s = proj_trans(pj_s.get(), PJ_INV, fwd_s);
    EXPECT_NEAR(DegToRad(30.0), inv_s.lp.lam, kRoundTripTolSphRad);
    EXPECT_NEAR(DegToRad(-60.0), inv_s.lp.phi, kRoundTripTolSphRad);

    // Equator lies on the horizon of both polar projections -> domain error.
    const PJ_COORD eq_n =
        proj_trans(pj_n.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(eq_n.xy.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj_n.get()));

    const PJ_COORD eq_s =
        proj_trans(pj_s.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(eq_s.xy.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj_s.get()));
  }
}

TEST(GnomTest, EllipsoidalGeodesicForwardInverseBothRadiusRegimesAndHorizon) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj ell_obliq(
      proj_create(ctx.get(), "+proj=gnom +lat_0=45 +lon_0=10 +ellps=WGS84"));
  ScopedPj sph_obliq(
      proj_create(ctx.get(), "+proj=gnom +lat_0=45 +lon_0=10 +R=6378137"));
  ScopedPj ell_equit(
      proj_create(ctx.get(), "+proj=gnom +lat_0=0 +ellps=WGS84"));
  ScopedPj ell_pole(
      proj_create(ctx.get(), "+proj=gnom +lat_0=90 +ellps=WGS84"));
  ASSERT_NE(nullptr, ell_obliq.get());
  ASSERT_NE(nullptr, sph_obliq.get());
  ASSERT_NE(nullptr, ell_equit.get());
  ASSERT_NE(nullptr, ell_pole.get());

  // 1. Verify WGS84 geodesic gnomonic differs from sphere with same major axis.
  const PJ_COORD test_pt = proj_coord(DegToRad(35.0), DegToRad(55.0), 0.0, 0.0);
  const PJ_COORD fwd_ell = proj_trans(ell_obliq.get(), PJ_FWD, test_pt);
  const PJ_COORD fwd_sph = proj_trans(sph_obliq.get(), PJ_FWD, test_pt);
  EXPECT_EQ(0, proj_errno(ell_obliq.get()));
  EXPECT_GT(
      std::hypot(fwd_ell.xy.x - fwd_sph.xy.x, fwd_ell.xy.y - fwd_sph.xy.y),
      1000.0);

  // 2. Round-trip center (rho == 0), inner disk (rho <= 1, little=true), and
  //    outer disk (rho > 1, little=false -> >45 deg angular distance).
  struct Case {
    double lon_deg;
    double lat_deg;
    bool expect_rho_gt_one;
  };
  constexpr Case kObliqCases[] = {
      {.lon_deg = 10.0, .lat_deg = 45.0, .expect_rho_gt_one = false},
      {.lon_deg = 20.0, .lat_deg = 50.0, .expect_rho_gt_one = false},
      // ~68.8 deg geodesic distance from (10E, 45N) -> rho = tan(68.8) > 1.
      {.lon_deg = 80.0, .lat_deg = 10.0, .expect_rho_gt_one = true},
  };
  for (const auto& tc : kObliqCases) {
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(ell_obliq.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(ell_obliq.get()));
    if (tc.expect_rho_gt_one) {
      EXPECT_GT(std::hypot(fwd.xy.x, fwd.xy.y), 6378137.0);
    }
    const PJ_COORD inv = proj_trans(ell_obliq.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(ell_obliq.get()));
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, kRoundTripTolEllRad);
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, kRoundTripTolEllRad);
  }

  // Equatorial and polar WGS84 round-trips (including rho > 1 on equator).
  const PJ_COORD eq_outer =
      proj_coord(DegToRad(60.0), DegToRad(15.0), 0.0, 0.0);
  const PJ_COORD eq_fwd = proj_trans(ell_equit.get(), PJ_FWD, eq_outer);
  EXPECT_GT(std::hypot(eq_fwd.xy.x, eq_fwd.xy.y), 6378137.0);
  const PJ_COORD eq_inv = proj_trans(ell_equit.get(), PJ_INV, eq_fwd);
  EXPECT_EQ(0, proj_errno(ell_equit.get()));
  EXPECT_NEAR(eq_outer.lp.lam, eq_inv.lp.lam, kRoundTripTolEllRad);
  EXPECT_NEAR(eq_outer.lp.phi, eq_inv.lp.phi, kRoundTripTolEllRad);

  const PJ_COORD pole_pt =
      proj_coord(DegToRad(-40.0), DegToRad(65.0), 0.0, 0.0);
  const PJ_COORD pole_inv = proj_trans(
      ell_pole.get(), PJ_INV, proj_trans(ell_pole.get(), PJ_FWD, pole_pt));
  EXPECT_EQ(0, proj_errno(ell_pole.get()));
  EXPECT_NEAR(pole_pt.lp.lam, pole_inv.lp.lam, kRoundTripTolEllRad);
  EXPECT_NEAR(pole_pt.lp.phi, pole_inv.lp.phi, kRoundTripTolEllRad);

  // 3. Points beyond horizon fail reduced geodesic scale M <= 0 check.
  const PJ_COORD far_eq = proj_trans(
      ell_equit.get(), PJ_FWD, proj_coord(DegToRad(130.0), 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(far_eq.xy.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(ell_equit.get()));

  const PJ_COORD far_obliq =
      proj_trans(ell_obliq.get(), PJ_FWD,
                 proj_coord(DegToRad(-170.0), DegToRad(-30.0), 0.0, 0.0));
  EXPECT_TRUE(std::isinf(far_obliq.xy.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(ell_obliq.get()));
}

TEST(GnomTest, ScaleFactorsTransforms3D4DBatchGenericAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Scale factors on spherical equatorial gnomonic: at center scales equal
  //    1; along central meridian at lat=30N, h = sec^2(30) and k = sec(30).
  ScopedPj sph(proj_create(ctx.get(), "+proj=gnom +lat_0=0 +R=6371000"));
  ASSERT_NE(nullptr, sph.get());

  const PJ_FACTORS center_fac =
      proj_factors(sph.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_NEAR(1.0, center_fac.meridional_scale, 1e-7);
  EXPECT_NEAR(1.0, center_fac.parallel_scale, 1e-7);
  EXPECT_NEAR(1.0, center_fac.areal_scale, 1e-7);
  EXPECT_NEAR(0.0, center_fac.angular_distortion, 1e-7);
  EXPECT_NEAR(kHalfPi, center_fac.meridian_parallel_angle, 1e-7);

  const double sec30 = 1.0 / std::cos(DegToRad(30.0));
  const PJ_FACTORS fac_30n =
      proj_factors(sph.get(), proj_coord(0.0, DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_NEAR(sec30 * sec30, fac_30n.meridional_scale, 1e-7);
  EXPECT_NEAR(sec30, fac_30n.parallel_scale, 1e-7);
  EXPECT_NEAR(sec30 * sec30 * sec30, fac_30n.areal_scale, 1e-7);

  const PJ_FACTORS null_fac =
      proj_factors(nullptr, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_DOUBLE_EQ(0.0, null_fac.meridional_scale);

  // 2. 4D proj_trans preserves z and t; PJ_IDENT is no-op.
  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=gnom +lat_0=45 +lon_0=10 +x_0=100000 +y_0=-50000 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  const PJ_FACTORS ell_fac = proj_factors(
      pj.get(), proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(ell_fac.areal_scale, 0.9);

  const PJ_COORD in_4d =
      proj_coord(DegToRad(22.0), DegToRad(52.0), 777.25, 2025.5);
  const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, in_4d);
  EXPECT_DOUBLE_EQ(in_4d.v[0], ident.v[0]);
  EXPECT_DOUBLE_EQ(in_4d.v[1], ident.v[1]);
  EXPECT_DOUBLE_EQ(in_4d.v[2], ident.v[2]);
  EXPECT_DOUBLE_EQ(in_4d.v[3], ident.v[3]);

  const PJ_COORD fwd_4d = proj_trans(pj.get(), PJ_FWD, in_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(777.25, fwd_4d.xyzt.z);
  EXPECT_DOUBLE_EQ(2025.5, fwd_4d.xyzt.t);

  const PJ_COORD inv_4d = proj_trans(pj.get(), PJ_INV, fwd_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpzt.lam, inv_4d.lpzt.lam, kRoundTripTolEllRad);
  EXPECT_NEAR(in_4d.lpzt.phi, inv_4d.lpzt.phi, kRoundTripTolEllRad);
  EXPECT_DOUBLE_EQ(777.25, inv_4d.lpzt.z);
  EXPECT_DOUBLE_EQ(2025.5, inv_4d.lpzt.t);

  // 3. Classic 2D and 3D helpers agree with proj_trans.
  const PJ_XY xy2d = pj_fwd(in_4d.lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(fwd_4d.xy.x, xy2d.x, 1e-6);
  EXPECT_NEAR(fwd_4d.xy.y, xy2d.y, 1e-6);
  const PJ_LP lp2d = pj_inv(xy2d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lp.lam, lp2d.lam, kRoundTripTolEllRad);
  EXPECT_NEAR(in_4d.lp.phi, lp2d.phi, kRoundTripTolEllRad);

  const PJ_XYZ xyz3d = pj_fwd3d(in_4d.lpz, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(777.25, xyz3d.z);
  const PJ_LPZ lpz3d = pj_inv3d(xyz3d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpz.lam, lpz3d.lam, kRoundTripTolEllRad);
  EXPECT_NEAR(in_4d.lpz.phi, lpz3d.phi, kRoundTripTolEllRad);
  EXPECT_DOUBLE_EQ(777.25, lpz3d.z);

  // 4. Batch proj_trans_array and strided proj_trans_generic.
  std::array<PJ_COORD, 2> valid_batch = {
      proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0),
      proj_coord(DegToRad(20.0), DegToRad(50.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, valid_batch.size(),
                                valid_batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, valid_batch.size(),
                                valid_batch.data()));
  EXPECT_NEAR(DegToRad(10.0), valid_batch[0].lp.lam, kRoundTripTolEllRad);
  EXPECT_NEAR(DegToRad(45.0), valid_batch[0].lp.phi, kRoundTripTolEllRad);

  std::array<PJ_COORD, 2> mixed_batch = {
      proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0),
      proj_coord(DegToRad(-170.0), DegToRad(-30.0), 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_trans_array(pj.get(), PJ_FWD, mixed_batch.size(),
                             mixed_batch.data()));
  EXPECT_FALSE(std::isinf(mixed_batch[0].xy.x));
  EXPECT_TRUE(std::isinf(mixed_batch[1].xy.x));
  proj_errno_reset(pj.get());

  std::array<double, 2> xs = {DegToRad(10.0), DegToRad(15.0)};
  std::array<double, 2> ys = {DegToRad(45.0), DegToRad(48.0)};
  EXPECT_EQ(2u, proj_trans_generic(pj.get(), PJ_FWD, xs.data(), sizeof(double),
                                   xs.size(), ys.data(), sizeof(double),
                                   ys.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(100000.0, xs[0], kMetricTolM);
  EXPECT_NEAR(-50000.0, ys[0], kMetricTolM);

  // 5. Edge inputs: nullptr PJ, NaN, Inf, out-of-range latitude.
  const PJ_COORD null_out = proj_trans(nullptr, PJ_FWD, in_4d);
  EXPECT_DOUBLE_EQ(in_4d.v[0], null_out.v[0]);

  const double qnan = std::numeric_limits<double>::quiet_NaN();
  const PJ_COORD nan_out =
      proj_trans(pj.get(), PJ_FWD, proj_coord(qnan, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.v[0]));
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_COORD inf_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, HUGE_VAL, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_fwd.xy.x));

  const PJ_COORD bad_lat =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 2.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(bad_lat.xy.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  const PJ_COORD inf_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_inv.lp.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
}

// TODO(schwehr): Allocator OOM guard in PJ_PROJECTION(gnom), the spherical
// OBLIQ |sin(phi)| >= 1 pole clamp, and the 10-iteration Newton non-convergence
// branch in gnom_e_inverse remain uncovered.

}  // namespace
}  // namespace proj
