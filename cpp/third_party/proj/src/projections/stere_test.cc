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
// Unit tests for Stereographic (+proj=stere) and Universal Polar Stereographic
// (+proj=ups) projections (proj/src/projections/stere.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/stere.cpp

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

// stere_e_inverse terminates fixed-point iteration at |phi_l - lp.phi| < 1e-10
// (CONV = 1e-10); 1e-9 rad accommodates that stopping threshold plus cross-arch
// FMA differences across x86_64 and arm.
constexpr double kRoundTripTolRad = 1e-9;
constexpr double kMetricTolM = 1e-4;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(StereTest, InitializationInvalidDefinitionsAndUpsSphereRejection) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Invalid proj strings and parameters under log suppression.
  {
    ScopedLogSuppressor suppress(ctx.get());
    struct InvalidCase {
      const char* def;
      int expected_errno;
    };
    constexpr InvalidCase kInvalidCases[] = {
        {.def = nullptr, .expected_errno = PROJ_ERR_OTHER_API_MISUSE},
        {.def = "", .expected_errno = PROJ_ERR_INVALID_OP_WRONG_SYNTAX},
        {.def = "+proj=stere +lat_0=95 +R=6371000",
         .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
        {.def = "+proj=stere +k_0=0 +R=6371000",
         .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
        {.def = "+proj=stere +R=0",
         .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
        {.def = "+proj=ups +south=maybe +ellps=WGS84",
         .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
    };
    for (const auto& tc : kInvalidCases) {
      proj_context_errno_set(ctx.get(), 0);
      ScopedPj bad(proj_create(ctx.get(), tc.def));
      EXPECT_EQ(nullptr, bad.get());
      EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get()));
    }
  }

  // 2. +proj=ups rejects spherical earths (es == 0) with explicit error log.
  {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj bad_ups(proj_create(ctx.get(), "+proj=ups +R=6371000"));
    EXPECT_EQ(nullptr, bad_ups.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
    EXPECT_TRUE(
        capture.ContainsMessage("only ellipsoidal formulation supported"));
  }

  // 3. Mutable argv creation and metadata on stere and ups.
  char s0[] = "proj=stere";
  char s1[] = "lat_0=90";
  char s2[] = "lat_ts=71";
  char s3[] = "ellps=WGS84";
  std::array<char*, 4> stere_argv = {s0, s1, s2, s3};
  ScopedPj stere_pj(proj_create_argv(
      ctx.get(), static_cast<int>(stere_argv.size()), stere_argv.data()));
  ASSERT_NE(nullptr, stere_pj.get());
  const PJ_PROJ_INFO stere_info = proj_pj_info(stere_pj.get());
  EXPECT_STREQ("stere", stere_info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", stere_info.description);
  EXPECT_THAT(stere_pj->descr, HasSubstr("Stereographic"));
  EXPECT_EQ(1, stere_info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(stere_pj.get()));
  EXPECT_EQ(1, proj_angular_input(stere_pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(stere_pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(stere_pj.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(stere_pj.get(), PJ_INV));

  ScopedPj ups_pj(proj_create(ctx.get(), "+proj=ups +ellps=WGS84"));
  ASSERT_NE(nullptr, ups_pj.get());
  EXPECT_STREQ("ups", proj_pj_info(ups_pj.get()).id);
  EXPECT_THAT(ups_pj->descr, HasSubstr("Universal Polar Stereographic"));
}

TEST(StereTest, SphericalAnalyticalForwardAndInverseAllModes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct SphericalCase {
    const char* def;
    double lon_deg;
    double lat_deg;
    double expected_x_m;
    double expected_y_m;
  };
  constexpr SphericalCase kCases[] = {
      // EQUIT: origin hits |rh| <= EPS10 in stere_s_inverse.
      {.def = "+proj=stere +lat_0=0 +lon_0=0 +R=6371000",
       .lon_deg = 0.0,
       .lat_deg = 0.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0},
      {.def = "+proj=stere +lat_0=0 +lon_0=0 +R=6371000",
       .lon_deg = 30.0,
       .lat_deg = 45.0,
       .expected_x_m = 2794005.406695,
       .expected_y_m = 5588010.813389},
      // OBLIQ: origin and off-center with +k_0, +x_0, +y_0.
      {.def = "+proj=stere +lat_0=45 +lon_0=10 +k_0=0.9996 "
              "+x_0=500000 +y_0=1000000 +R=6371000",
       .lon_deg = 10.0,
       .lat_deg = 45.0,
       .expected_x_m = 500000.0,
       .expected_y_m = 1000000.0},
      {.def = "+proj=stere +lat_0=45 +lon_0=10 +k_0=0.9996 "
              "+x_0=500000 +y_0=1000000 +R=6371000",
       .lon_deg = 25.0,
       .lat_deg = 55.0,
       .expected_x_m = 1459328.638407,
       .expected_y_m = 2211454.096986},
      // N_POLE: pole origin, default lat_ts=90, and secant lat_ts=60.
      {.def = "+proj=stere +lat_0=90 +R=6371000",
       .lon_deg = 0.0,
       .lat_deg = 90.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0},
      {.def = "+proj=stere +lat_0=90 +R=6371000",
       .lon_deg = 45.0,
       .lat_deg = 70.0,
       .expected_x_m = 1588698.091975,
       .expected_y_m = -1588698.091975},
      {.def = "+proj=stere +lat_0=90 +lat_ts=60 +R=6371000",
       .lon_deg = 30.0,
       .lat_deg = 60.0,
       .expected_x_m = 1592750.000000,
       .expected_y_m = -2758723.923755},
      // S_POLE: pole origin, default lat_ts=90, and negative lat_ts=-71.
      {.def = "+proj=stere +lat_0=-90 +R=6371000",
       .lon_deg = 0.0,
       .lat_deg = -90.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0},
      {.def = "+proj=stere +lat_0=-90 +R=6371000",
       .lon_deg = -60.0,
       .lat_deg = -75.0,
       .expected_x_m = -1452771.578866,
       .expected_y_m = 838758.062129},
      {.def = "+proj=stere +lat_0=-90 +lat_ts=-71 +R=6371000",
       .lon_deg = 20.0,
       .lat_deg = -71.0,
       .expected_x_m = 709416.372700,
       .expected_y_m = 1949105.464983},
  };

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    const double lon_rad = DegToRad(tc.lon_deg);
    const double lat_rad = DegToRad(tc.lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_x_m, fwd.xy.x, kMetricTolM) << tc.def;
    EXPECT_NEAR(tc.expected_y_m, fwd.xy.y, kMetricTolM) << tc.def;

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(lat_rad, inv.lp.phi, kRoundTripTolRad) << tc.def;
    if (std::abs(std::abs(tc.lat_deg) - 90.0) > 1e-6) {
      EXPECT_NEAR(lon_rad, inv.lp.lam, kRoundTripTolRad) << tc.def;
    }
  }
}

TEST(StereTest, SphericalDomainErrorsAtAntipodalPoints) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  struct AntipodeCase {
    const char* def;
    double lon_deg;
    double lat_deg;
  };
  constexpr AntipodeCase kAntipodes[] = {
      {.def = "+proj=stere +lat_0=0 +R=6371000",
       .lon_deg = 180.0,
       .lat_deg = 0.0},
      {.def = "+proj=stere +lat_0=45 +R=6371000",
       .lon_deg = 180.0,
       .lat_deg = -45.0},
      {.def = "+proj=stere +lat_0=90 +R=6371000",
       .lon_deg = 0.0,
       .lat_deg = -90.0},
      {.def = "+proj=stere +lat_0=-90 +R=6371000",
       .lon_deg = 0.0,
       .lat_deg = 90.0},
  };

  for (const auto& tc : kAntipodes) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    const PJ_COORD err = proj_trans(
        pj.get(), PJ_FWD,
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0));
    EXPECT_TRUE(std::isinf(err.xy.x)) << tc.def;
    EXPECT_TRUE(std::isinf(err.xy.y)) << tc.def;
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()))
        << tc.def;

    // proj_errno_reset restores clean subsequent transforms.
    proj_errno_reset(pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD ok =
        proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(ok.lp.lam));
  }
}

TEST(StereTest, EllipsoidalForwardAndInverseAllModesAndUps) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct EllipsoidalCase {
    const char* def;
    double lon_deg;
    double lat_deg;
    double expected_x_m;
    double expected_y_m;
    bool check_xy;
  };
  constexpr EllipsoidalCase kCases[] = {
      // EQUIT: center origin (rho == 0) and off-center point.
      {.def = "+proj=stere +lat_0=0 +ellps=WGS84",
       .lon_deg = 0.0,
       .lat_deg = 0.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0,
       .check_xy = true},
      {.def = "+proj=stere +lat_0=0 +ellps=WGS84",
       .lon_deg = 25.0,
       .lat_deg = -35.0,
       .expected_x_m = 2537672.912752,
       .expected_y_m = -4176361.930175,
       .check_xy = true},
      // OBLIQ: center origin (rho == 0 with sinX1 != 0) and off-center point.
      {.def = "+proj=stere +lat_0=52 +lon_0=5 +k_0=0.9999079 "
              "+x_0=155000 +y_0=463000 +ellps=WGS84",
       .lon_deg = 5.0,
       .lat_deg = 52.0,
       .expected_x_m = 155000.0,
       .expected_y_m = 463000.0,
       .check_xy = true},
      {.def = "+proj=stere +lat_0=52 +lon_0=5 +k_0=0.9999079 "
              "+x_0=155000 +y_0=463000 +ellps=WGS84",
       .lon_deg = 6.0,
       .lat_deg = 53.0,
       .expected_x_m = 222138.475526,
       .expected_y_m = 574736.256255,
       .check_xy = true},
      // N_POLE: exact pole (|phi - pi/2| < 1e-15) and secant lat_ts=71.
      {.def = "+proj=stere +lat_0=90 +ellps=WGS84",
       .lon_deg = 0.0,
       .lat_deg = 90.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0,
       .check_xy = true},
      {.def = "+proj=stere +lat_0=90 +lat_ts=71 +ellps=WGS84",
       .lon_deg = -45.0,
       .lat_deg = 75.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0,
       .check_xy = false},
      // S_POLE: exact pole and secant lat_ts=-71.
      {.def = "+proj=stere +lat_0=-90 +ellps=WGS84",
       .lon_deg = 0.0,
       .lat_deg = -90.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0,
       .check_xy = true},
      {.def = "+proj=stere +lat_0=-90 +lat_ts=-71 +ellps=WGS84",
       .lon_deg = 120.0,
       .lat_deg = -78.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0,
       .check_xy = false},
      // Universal Polar Stereographic (+proj=ups): North and South.
      {.def = "+proj=ups +ellps=WGS84",
       .lon_deg = 0.0,
       .lat_deg = 90.0,
       .expected_x_m = 2000000.0,
       .expected_y_m = 2000000.0,
       .check_xy = true},
      {.def = "+proj=ups +ellps=WGS84",
       .lon_deg = 15.0,
       .lat_deg = 85.0,
       .expected_x_m = 2143762.951633,
       .expected_y_m = 1463469.360260,
       .check_xy = true},
      {.def = "+proj=ups +south +ellps=WGS84",
       .lon_deg = 0.0,
       .lat_deg = -90.0,
       .expected_x_m = 2000000.0,
       .expected_y_m = 2000000.0,
       .check_xy = true},
      {.def = "+proj=ups +south +ellps=WGS84",
       .lon_deg = -70.0,
       .lat_deg = -82.0,
       .expected_x_m = 1164070.138663,
       .expected_y_m = 2304253.587461,
       .check_xy = true},
  };

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    const double lon_rad = DegToRad(tc.lon_deg);
    const double lat_rad = DegToRad(tc.lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    if (tc.check_xy) {
      EXPECT_NEAR(tc.expected_x_m, fwd.xy.x, kMetricTolM) << tc.def;
      EXPECT_NEAR(tc.expected_y_m, fwd.xy.y, kMetricTolM) << tc.def;
    }

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(lat_rad, inv.lp.phi, kRoundTripTolRad) << tc.def;
    if (std::abs(std::abs(tc.lat_deg) - 90.0) > 1e-6) {
      EXPECT_NEAR(lon_rad, inv.lp.lam, kRoundTripTolRad) << tc.def;
    }
  }

  // Direct inverse at exact origin (0, 0) in OBLIQ guarantees rho == 0.0
  // regardless of false-offset FMA rounding.
  ScopedPj obliq_origin(
      proj_create(ctx.get(), "+proj=stere +lat_0=52 +lon_0=5 +ellps=WGS84"));
  ASSERT_NE(nullptr, obliq_origin.get());
  const PJ_COORD origin_inv =
      proj_trans(obliq_origin.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(obliq_origin.get()));
  EXPECT_NEAR(DegToRad(5.0), origin_inv.lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(52.0), origin_inv.lp.phi, 1e-12);
}

TEST(StereTest, EllipsoidalSingularitiesAndNonConvergence) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. OBLIQ denom == 0 branch: with tiny |phi0| = 1e-7 deg (> EPS10 rad),
  //    cos(X1) rounds to 1.0 and |sin(X1)^2| < 1e-17, making
  //    1 + sinX1*(-sinX1) - cosX1^2 == 0.0 across both x86_64 and arm.
  {
    ScopedPj obliq(
        proj_create(ctx.get(), "+proj=stere +lat_0=1e-7 +ellps=WGS84"));
    ASSERT_NE(nullptr, obliq.get());
    const PJ_COORD err = proj_trans(obliq.get(), PJ_FWD,
                                    proj_coord(kPi, DegToRad(-1e-7), 0.0, 0.0));
    EXPECT_TRUE(std::isinf(err.xy.x));
    EXPECT_TRUE(std::isinf(err.xy.y));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(obliq.get()));
  }

  // 2. EQUIT 1 + cosX * coslam == 0 branch sets xy.y = HUGE_VAL.
  {
    ScopedPj equit(proj_create(ctx.get(), "+proj=stere +lat_0=0 +ellps=WGS84"));
    ASSERT_NE(nullptr, equit.get());
    const PJ_COORD antipode =
        proj_trans(equit.get(), PJ_FWD, proj_coord(kPi, 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(antipode.xy.y));
  }

  // 3. stere_e_inverse non-convergence: fixed-point iteration with NITER = 8
  //    converges like O(e^(2*8)), so an eccentricity squared of es = 0.5 fails
  //    to reach CONV = 1e-10 in 8 steps and emits OUTSIDE_PROJECTION_DOMAIN.
  struct NonConvCase {
    const char* def;
    double x_m;
    double y_m;
  };
  constexpr NonConvCase kNonConvCases[] = {
      {.def = "+proj=stere +lat_0=0 +a=1000000 +es=0.5",
       .x_m = 0.0,
       .y_m = 500000.0},
      {.def = "+proj=stere +lat_0=90 +a=1000000 +es=0.5",
       .x_m = 1000000.0,
       .y_m = 0.0},
  };
  for (const auto& tc : kNonConvCases) {
    ScopedPj high_ecc(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, high_ecc.get()) << tc.def;
    const PJ_COORD err = proj_trans(high_ecc.get(), PJ_INV,
                                    proj_coord(tc.x_m, tc.y_m, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(err.lp.lam)) << tc.def;
    EXPECT_TRUE(std::isinf(err.lp.phi)) << tc.def;
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(high_ecc.get()))
        << tc.def;
  }
}

TEST(StereTest, Transforms3D4DBatchGenericAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=stere +lat_0=45 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  // 1. 4D proj_trans preserves z and t across round-trip; PJ_IDENT is no-op.
  const PJ_COORD in_4d =
      proj_coord(DegToRad(15.0), DegToRad(50.0), 312.25, 2026.5);
  const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, in_4d);
  EXPECT_DOUBLE_EQ(in_4d.v[0], ident.v[0]);
  EXPECT_DOUBLE_EQ(in_4d.v[1], ident.v[1]);
  EXPECT_DOUBLE_EQ(in_4d.v[2], ident.v[2]);
  EXPECT_DOUBLE_EQ(in_4d.v[3], ident.v[3]);

  const PJ_COORD fwd_4d = proj_trans(pj.get(), PJ_FWD, in_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(312.25, fwd_4d.xyzt.z);
  EXPECT_DOUBLE_EQ(2026.5, fwd_4d.xyzt.t);

  const PJ_COORD inv_4d = proj_trans(pj.get(), PJ_INV, fwd_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpzt.lam, inv_4d.lpzt.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpzt.phi, inv_4d.lpzt.phi, kRoundTripTolRad);
  EXPECT_DOUBLE_EQ(312.25, inv_4d.lpzt.z);
  EXPECT_DOUBLE_EQ(2026.5, inv_4d.lpzt.t);

  // 2. Classic 2D and 3D internal wrappers agree with proj_trans.
  const PJ_XY xy2d = pj_fwd(in_4d.lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(fwd_4d.xy.x, xy2d.x, 1e-6);
  EXPECT_NEAR(fwd_4d.xy.y, xy2d.y, 1e-6);
  const PJ_LP lp2d = pj_inv(xy2d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lp.lam, lp2d.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lp.phi, lp2d.phi, kRoundTripTolRad);

  const PJ_XYZ xyz3d = pj_fwd3d(in_4d.lpz, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(312.25, xyz3d.z);
  const PJ_LPZ lpz3d = pj_inv3d(xyz3d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpz.lam, lpz3d.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpz.phi, lpz3d.phi, kRoundTripTolRad);
  EXPECT_DOUBLE_EQ(312.25, lpz3d.z);

  // 3. Batch proj_trans_array: valid batch succeeds with 0; mixed fails.
  std::array<PJ_COORD, 2> valid_batch = {
      proj_coord(0.0, DegToRad(45.0), 0.0, 0.0),
      proj_coord(DegToRad(20.0), DegToRad(55.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, valid_batch.size(),
                                valid_batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, valid_batch.size(),
                                valid_batch.data()));
  EXPECT_NEAR(0.0, valid_batch[0].lp.lam, kRoundTripTolRad);
  EXPECT_NEAR(DegToRad(45.0), valid_batch[0].lp.phi, kRoundTripTolRad);

  std::array<PJ_COORD, 2> mixed_batch = {
      proj_coord(0.0, DegToRad(45.0), 0.0, 0.0),
      proj_coord(kPi, DegToRad(-45.0), 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_trans_array(pj.get(), PJ_FWD, mixed_batch.size(),
                             mixed_batch.data()));
  EXPECT_FALSE(std::isinf(mixed_batch[0].xy.x));
  EXPECT_TRUE(std::isinf(mixed_batch[1].xy.x));
  proj_errno_reset(pj.get());

  // 4. proj_trans_generic on separate coordinate arrays.
  std::array<double, 2> xs = {0.0, DegToRad(10.0)};
  std::array<double, 2> ys = {DegToRad(45.0), DegToRad(50.0)};
  EXPECT_EQ(2u, proj_trans_generic(pj.get(), PJ_FWD, xs.data(), sizeof(double),
                                   xs.size(), ys.data(), sizeof(double),
                                   ys.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, xs[0], kMetricTolM);
  EXPECT_NEAR(0.0, ys[0], kMetricTolM);

  // 5. Edge inputs: nullptr PJ, NaN, Inf, and invalid latitude.
  const PJ_COORD null_out = proj_trans(nullptr, PJ_FWD, in_4d);
  EXPECT_DOUBLE_EQ(in_4d.v[0], null_out.v[0]);

  const double qnan = std::numeric_limits<double>::quiet_NaN();
  const PJ_COORD nan_out =
      proj_trans(pj.get(), PJ_FWD, proj_coord(qnan, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.v[0]));
  EXPECT_TRUE(std::isnan(nan_out.v[1]));
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_COORD inf_phi =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, HUGE_VAL, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_phi.xy.x));

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

TEST(StereTest, ScaleFactorsConformalityAndTrueScaleLatitude) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Spherical stereographic is conformal (h == k, omega == 0) everywhere and
  //    has scale k0 * 2 / (1 + cos(phi) * cos(lam)) in equatorial aspect.
  ScopedPj sph_eq(
      proj_create(ctx.get(), "+proj=stere +lat_0=0 +k_0=0.994 +R=6371000"));
  ASSERT_NE(nullptr, sph_eq.get());
  const double lon30 = DegToRad(30.0);
  const double lat40 = DegToRad(40.0);
  const double expected_k =
      0.994 * 2.0 / (1.0 + std::cos(lat40) * std::cos(lon30));
  const PJ_FACTORS sph_fac =
      proj_factors(sph_eq.get(), proj_coord(lon30, lat40, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph_eq.get()));
  EXPECT_NEAR(expected_k, sph_fac.meridional_scale, 1e-7);
  EXPECT_NEAR(expected_k, sph_fac.parallel_scale, 1e-7);
  EXPECT_NEAR(expected_k * expected_k, sph_fac.areal_scale, 1e-7);
  EXPECT_NEAR(0.0, sph_fac.angular_distortion, 1e-7);
  EXPECT_NEAR(kHalfPi, sph_fac.meridian_parallel_angle, 1e-7);

  // 2. Spherical polar stereographic with +lat_ts=60 has unit scale at 60N.
  ScopedPj sph_ts(
      proj_create(ctx.get(), "+proj=stere +lat_0=90 +lat_ts=60 +R=6371000"));
  ASSERT_NE(nullptr, sph_ts.get());
  const PJ_FACTORS sph_ts_fac =
      proj_factors(sph_ts.get(), proj_coord(0.0, DegToRad(60.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph_ts.get()));
  EXPECT_NEAR(1.0, sph_ts_fac.meridional_scale, 1e-7);
  EXPECT_NEAR(1.0, sph_ts_fac.parallel_scale, 1e-7);
  EXPECT_NEAR(0.0, sph_ts_fac.angular_distortion, 1e-7);

  // 3. Ellipsoidal polar stereographic with +lat_ts=71 has unit scale at 71N.
  ScopedPj ell_ts(
      proj_create(ctx.get(), "+proj=stere +lat_0=90 +lat_ts=71 +ellps=WGS84"));
  ASSERT_NE(nullptr, ell_ts.get());
  const PJ_FACTORS ell_ts_fac =
      proj_factors(ell_ts.get(), proj_coord(0.0, DegToRad(71.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ell_ts.get()));
  EXPECT_NEAR(1.0, ell_ts_fac.meridional_scale, 1e-7);
  EXPECT_NEAR(1.0, ell_ts_fac.parallel_scale, 1e-7);
  EXPECT_NEAR(0.0, ell_ts_fac.angular_distortion, 1e-7);

  // 4. UPS near North Pole is conformal with pole scale ~0.994.
  ScopedPj ups(proj_create(ctx.get(), "+proj=ups +ellps=WGS84"));
  ASSERT_NE(nullptr, ups.get());
  const PJ_FACTORS ups_fac =
      proj_factors(ups.get(), proj_coord(0.0, DegToRad(89.9), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ups.get()));
  EXPECT_NEAR(0.994, ups_fac.meridional_scale, 1e-5);
  EXPECT_NEAR(0.994, ups_fac.parallel_scale, 1e-5);
  EXPECT_NEAR(0.0, ups_fac.angular_distortion, 1e-6);

  // 5. nullptr PJ returns zero-initialized PJ_FACTORS.
  const PJ_FACTORS null_fac =
      proj_factors(nullptr, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_DOUBLE_EQ(0.0, null_fac.meridional_scale);
  EXPECT_DOUBLE_EQ(0.0, null_fac.parallel_scale);
}

// TODO(schwehr): Only the calloc OOM guards in PJ_PROJECTION(stere) and
// PJ_PROJECTION(ups) remain uncovered without allocator fault injection.

}  // namespace
}  // namespace proj
