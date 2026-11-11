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
// Unit tests for Albers Equal Area (aea) and Lambert Equal Area Conic (leac)
// projections (proj/src/projections/aea.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/aea.cpp

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
constexpr double kRoundTripTolRad = 1e-11;
constexpr double kMetricTolM = 1e-4;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(AeaTest, InitializationValidationAndMetadataAeaAndLeac) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  struct InvalidCase {
    const char* def;
    int expected_errno;
    const char* expected_log;
  };
  constexpr InvalidCase kInvalidCases[] = {
      {.def = nullptr,
       .expected_errno = PROJ_ERR_OTHER_API_MISUSE,
       .expected_log = nullptr},
      {.def = "",
       .expected_errno = PROJ_ERR_INVALID_OP_WRONG_SYNTAX,
       .expected_log = nullptr},
      {.def = "+proj=aea +lat_1=95 +lat_2=30 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for lat_1"},
      {.def = "+proj=aea +lat_1=30 +lat_2=-95 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for lat_2"},
      {.def = "+proj=leac +lat_1=95 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for lat_2"},
      {.def = "+proj=aea +lat_1=-30 +lat_2=30 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "|lat_1 + lat_2| should be > 0"},
      // Omitting lat_1/lat_2 leaves both 0, failing |lat_1 + lat_2| < EPS10.
      {.def = "+proj=aea +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "|lat_1 + lat_2| should be > 0"},
      // leac without +south sets phi1=90; lat_1=-90 sets phi2=-90 -> sum 0.
      {.def = "+proj=leac +lat_1=-90 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "|lat_1 + lat_2| should be > 0"},
      // phi1=0 and phi2=3.49e-10 rad (>= EPS10) gives m1 == m2 == 1.0 while
      // ml2 != ml1, triggering Q->n == 0 ("Invalid value for eccentricity").
      {.def = "+proj=aea +lat_1=0 +lat_2=0.00000002 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for eccentricity"},
  };

  for (const auto& tc : kInvalidCases) {
    capture.Clear();
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj bad(proj_create(ctx.get(), tc.def));
    EXPECT_EQ(nullptr, bad.get());
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get()));
    if (tc.expected_log != nullptr) {
      EXPECT_TRUE(capture.ContainsMessage(tc.expected_log)) << tc.def;
    }
  }

  // |phi1 - phi2| = 1.745e-9 >= EPS10 (secant), yet sin(phi1) == sin(90)
  // == 1.0 in binary64, triggering ml2 == ml1 -> destructor(P, 0).
  // Call pj_create_internal directly because PROJStringParser calls
  // proj_errno_string(0) == nullptr when pj_create_internal fails with errno 0.
  proj_context_errno_set(ctx.get(), 0);
  ScopedPj ml_eq(pj_create_internal(
      ctx.get(), "+proj=aea +lat_1=89.9999999 +lat_2=90 +ellps=WGS84"));
  EXPECT_EQ(nullptr, ml_eq.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  // Mutable buffers for proj_create_argv (never const_cast string literals).
  char aea0[] = "proj=aea";
  char aea1[] = "lat_1=29.5";
  char aea2[] = "lat_2=45.5";
  char aea3[] = "ellps=WGS84";
  std::array<char*, 4> aea_argv = {aea0, aea1, aea2, aea3};
  ScopedPj pj_aea(proj_create_argv(ctx.get(), static_cast<int>(aea_argv.size()),
                                   aea_argv.data()));
  ASSERT_NE(nullptr, pj_aea.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  const PJ_PROJ_INFO aea_info = proj_pj_info(pj_aea.get());
  EXPECT_STREQ("aea", aea_info.id);
  EXPECT_NE(nullptr, aea_info.description);
  EXPECT_THAT(pj_aea->descr, HasSubstr("Albers Equal Area"));
  EXPECT_EQ(1, aea_info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj_aea.get()));
  EXPECT_EQ(1, proj_angular_input(pj_aea.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj_aea.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(pj_aea.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(pj_aea.get(), PJ_INV));
  EXPECT_EQ(nullptr, pj_aea->destructor(nullptr, 0));

  char leac0[] = "proj=leac";
  char leac1[] = "lat_1=45";
  char leac2[] = "south";
  char leac3[] = "ellps=WGS84";
  std::array<char*, 4> leac_argv = {leac0, leac1, leac2, leac3};
  ScopedPj pj_leac(proj_create_argv(
      ctx.get(), static_cast<int>(leac_argv.size()), leac_argv.data()));
  ASSERT_NE(nullptr, pj_leac.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  const PJ_PROJ_INFO leac_info = proj_pj_info(pj_leac.get());
  EXPECT_STREQ("leac", leac_info.id);
  EXPECT_NE(nullptr, leac_info.description);
  EXPECT_THAT(pj_leac->descr, HasSubstr("Lambert Equal Area Conic"));
  EXPECT_EQ(1, leac_info.has_inverse);
}

TEST(AeaTest, EllipsoidalSecantTangentSouthernPolesAndLeacEquivalence) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct LonLatDeg {
    double lon_deg;
    double lat_deg;
  };

  // 1. Secant WGS84 Albers including exact north and south poles.
  {
    ScopedPj pj(proj_create(
        ctx.get(),
        "+proj=aea +lat_1=29.5 +lat_2=45.5 +lat_0=23 +lon_0=-96 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD origin =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(-96.0), DegToRad(23.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, origin.xy.x, kMetricTolM);
    EXPECT_NEAR(0.0, origin.xy.y, kMetricTolM);

    constexpr LonLatDeg kCases[] = {
        {.lon_deg = -75.0, .lat_deg = 40.0},
        {.lon_deg = -120.0, .lat_deg = 35.0},
        {.lon_deg = -96.0, .lat_deg = 0.0},
        {.lon_deg = -10.0, .lat_deg = -30.0},
        {.lon_deg = -96.0, .lat_deg = 90.0},
        {.lon_deg = -96.0, .lat_deg = -90.0},
    };
    for (const auto& tc : kCases) {
      const PJ_COORD in =
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
      const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj.get()));
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, kRoundTripTolRad);
      if (std::abs(std::abs(in.lp.phi) - kHalfPi) > 1e-9) {
        EXPECT_NEAR(in.lp.lam, inv.lp.lam, kRoundTripTolRad);
      }
    }
  }

  // 2. Tangent cone (lat_1 == lat_2) and southern cone (Q->n < 0).
  {
    ScopedPj pj_tan(
        proj_create(ctx.get(), "+proj=aea +lat_1=45 +lat_2=45 +ellps=WGS84"));
    ScopedPj pj_south(proj_create(
        ctx.get(),
        "+proj=aea +lat_1=-45 +lat_2=-15 +lat_0=-30 +lon_0=135 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_tan.get());
    ASSERT_NE(nullptr, pj_south.get());

    const PJ_COORD tan_in =
        proj_coord(DegToRad(20.0), DegToRad(50.0), 0.0, 0.0);
    const PJ_COORD tan_inv = proj_trans(
        pj_tan.get(), PJ_INV, proj_trans(pj_tan.get(), PJ_FWD, tan_in));
    EXPECT_EQ(0, proj_errno(pj_tan.get()));
    EXPECT_NEAR(tan_in.lp.lam, tan_inv.lp.lam, kRoundTripTolRad);
    EXPECT_NEAR(tan_in.lp.phi, tan_inv.lp.phi, kRoundTripTolRad);

    constexpr LonLatDeg kSouthCases[] = {
        {.lon_deg = 140.0, .lat_deg = -35.0},
        {.lon_deg = 120.0, .lat_deg = -20.0},
        {.lon_deg = 150.0, .lat_deg = -60.0},
    };
    for (const auto& tc : kSouthCases) {
      const PJ_COORD in =
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
      const PJ_COORD inv = proj_trans(pj_south.get(), PJ_INV,
                                      proj_trans(pj_south.get(), PJ_FWD, in));
      EXPECT_EQ(0, proj_errno(pj_south.get()));
      EXPECT_NEAR(in.lp.lam, inv.lp.lam, kRoundTripTolRad);
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, kRoundTripTolRad);
    }
  }

  // 3. leac equivalence to aea with one standard parallel at +-90 and exact
  //    cone-apex inverse (Q->rho == 0.0) for both northern and southern cones.
  {
    ScopedPj leac_n(
        proj_create(ctx.get(), "+proj=leac +lat_1=45 +lat_0=90 +ellps=WGS84"));
    ScopedPj aea_n(proj_create(
        ctx.get(), "+proj=aea +lat_1=90 +lat_2=45 +lat_0=90 +ellps=WGS84"));
    ScopedPj leac_s(proj_create(
        ctx.get(), "+proj=leac +south +lat_1=-45 +lat_0=-90 +ellps=WGS84"));
    ScopedPj aea_s(proj_create(
        ctx.get(), "+proj=aea +lat_1=-90 +lat_2=-45 +lat_0=-90 +ellps=WGS84"));
    ASSERT_NE(nullptr, leac_n.get());
    ASSERT_NE(nullptr, aea_n.get());
    ASSERT_NE(nullptr, leac_s.get());
    ASSERT_NE(nullptr, aea_s.get());

    const PJ_COORD pt_n = proj_coord(DegToRad(20.0), DegToRad(50.0), 0.0, 0.0);
    const PJ_COORD fwd_leac_n = proj_trans(leac_n.get(), PJ_FWD, pt_n);
    const PJ_COORD fwd_aea_n = proj_trans(aea_n.get(), PJ_FWD, pt_n);
    EXPECT_NEAR(fwd_aea_n.xy.x, fwd_leac_n.xy.x, kMetricTolM);
    EXPECT_NEAR(fwd_aea_n.xy.y, fwd_leac_n.xy.y, kMetricTolM);

    const PJ_COORD pt_s =
        proj_coord(DegToRad(-30.0), DegToRad(-55.0), 0.0, 0.0);
    const PJ_COORD fwd_leac_s = proj_trans(leac_s.get(), PJ_FWD, pt_s);
    const PJ_COORD fwd_aea_s = proj_trans(aea_s.get(), PJ_FWD, pt_s);
    EXPECT_NEAR(fwd_aea_s.xy.x, fwd_leac_s.xy.x, kMetricTolM);
    EXPECT_NEAR(fwd_aea_s.xy.y, fwd_leac_s.xy.y, kMetricTolM);

    // At lat_0 = +-90, Q->rho0 == 0, so inverse of (0, 0) hits Q->rho == 0.
    const PJ_COORD apex_n =
        proj_trans(leac_n.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(leac_n.get()));
    EXPECT_DOUBLE_EQ(0.0, apex_n.lp.lam);
    EXPECT_NEAR(kHalfPi, apex_n.lp.phi, 1e-12);

    const PJ_COORD apex_s =
        proj_trans(leac_s.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(leac_s.get()));
    EXPECT_DOUBLE_EQ(0.0, apex_s.lp.lam);
    EXPECT_NEAR(-kHalfPi, apex_s.lp.phi, 1e-12);
  }
}

TEST(AeaTest, SphericalAnalyticalInverseClampingAndDomainErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Spherical secant analytical verification and round-trip.
  ScopedPj sph(proj_create(
      ctx.get(),
      "+proj=aea +lat_1=29.5 +lat_2=45.5 +lat_0=23 +lon_0=-96 +R=6371000"));
  ASSERT_NE(nullptr, sph.get());
  EXPECT_EQ(0.0, sph->es);

  const double phi1 = DegToRad(29.5);
  const double phi2 = DegToRad(45.5);
  const double phi0 = DegToRad(23.0);
  const double n = 0.5 * (std::sin(phi1) + std::sin(phi2));
  const double c = std::cos(phi1) * std::cos(phi1) + 2.0 * n * std::sin(phi1);
  const double rho0 = std::sqrt(c - 2.0 * n * std::sin(phi0)) / n;

  const double test_lam = DegToRad(-66.0);
  const double test_phi = DegToRad(37.5);
  const double rho = std::sqrt(c - 2.0 * n * std::sin(test_phi)) / n;
  const double theta = n * DegToRad(30.0);
  const PJ_COORD sph_fwd =
      proj_trans(sph.get(), PJ_FWD, proj_coord(test_lam, test_phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_NEAR(kSphereRadiusM * rho * std::sin(theta), sph_fwd.xy.x,
              kMetricTolM);
  EXPECT_NEAR(kSphereRadiusM * (rho0 - rho * std::cos(theta)), sph_fwd.xy.y,
              kMetricTolM);

  const PJ_COORD sph_inv = proj_trans(sph.get(), PJ_INV, sph_fwd);
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_NEAR(test_lam, sph_inv.lp.lam, kRoundTripTolRad);
  EXPECT_NEAR(test_phi, sph_inv.lp.phi, kRoundTripTolRad);

  // 2. Spherical inverse clamping when |qs_div_2| > 1 (both poles).
  const PJ_COORD inv_south_clamp =
      proj_trans(sph.get(), PJ_INV, proj_coord(3.0e7, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_NEAR(-kHalfPi, inv_south_clamp.lp.phi, 1e-12);

  // Point slightly off cone apex (rho = 1000 / R > 0) gives qs_div_2 ~ c/n2
  // > 1.
  const PJ_COORD inv_north_clamp =
      proj_trans(sph.get(), PJ_INV,
                 proj_coord(0.0, kSphereRadiusM * rho0 - 1000.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_NEAR(kHalfPi, inv_north_clamp.lp.phi, 1e-12);

  // 3. Spherical tangent and southern cones.
  ScopedPj sph_tan(
      proj_create(ctx.get(), "+proj=aea +lat_1=35 +lat_2=35 +R=6371000"));
  ScopedPj sph_south(
      proj_create(ctx.get(), "+proj=aea +lat_1=-45 +lat_2=-15 +R=6371000"));
  ASSERT_NE(nullptr, sph_tan.get());
  ASSERT_NE(nullptr, sph_south.get());

  const PJ_COORD in_tan = proj_coord(DegToRad(15.0), DegToRad(40.0), 0.0, 0.0);
  const PJ_COORD out_tan = proj_trans(
      sph_tan.get(), PJ_INV, proj_trans(sph_tan.get(), PJ_FWD, in_tan));
  EXPECT_EQ(0, proj_errno(sph_tan.get()));
  EXPECT_NEAR(in_tan.lp.lam, out_tan.lp.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_tan.lp.phi, out_tan.lp.phi, kRoundTripTolRad);

  const PJ_COORD in_s = proj_coord(DegToRad(-25.0), DegToRad(-30.0), 0.0, 0.0);
  const PJ_COORD out_s = proj_trans(sph_south.get(), PJ_INV,
                                    proj_trans(sph_south.get(), PJ_FWD, in_s));
  EXPECT_EQ(0, proj_errno(sph_south.get()));
  EXPECT_NEAR(in_s.lp.lam, out_s.lp.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_s.lp.phi, out_s.lp.phi, kRoundTripTolRad);

  // 4. Ellipsoidal inverse domain error when |qs| > 2.
  ScopedPj ell(proj_create(
      ctx.get(),
      "+proj=aea +lat_1=29.5 +lat_2=45.5 +lat_0=23 +lon_0=-96 +ellps=WGS84"));
  ASSERT_NE(nullptr, ell.get());
  const PJ_COORD ell_err =
      proj_trans(ell.get(), PJ_INV, proj_coord(3.0e7, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(ell_err.lp.lam));
  EXPECT_TRUE(std::isinf(ell_err.lp.phi));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(ell.get()));
}

TEST(AeaTest, ScaleFactorsEqualAreaPropertyAnd4DBatchEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=aea +lat_1=29.5 +lat_2=45.5 +lat_0=23 +lon_0=-96 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Equal-area property: areal_scale == 1, h * k == 1, theta_prime == pi/2;
  //    at standard parallel lat_1=29.5, h == k == 1 and omega == 0.
  const PJ_FACTORS fac_std = proj_factors(
      pj.get(), proj_coord(DegToRad(-80.0), DegToRad(29.5), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, fac_std.meridional_scale, 1e-7);
  EXPECT_NEAR(1.0, fac_std.parallel_scale, 1e-7);
  EXPECT_NEAR(1.0, fac_std.areal_scale, 1e-7);
  EXPECT_NEAR(0.0, fac_std.angular_distortion, 1e-7);
  EXPECT_NEAR(kHalfPi, fac_std.meridian_parallel_angle, 1e-7);

  const PJ_FACTORS fac_mid = proj_factors(
      pj.get(), proj_coord(DegToRad(-85.0), DegToRad(37.5), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, fac_mid.areal_scale, 1e-7);
  EXPECT_NEAR(1.0, fac_mid.meridional_scale * fac_mid.parallel_scale, 1e-7);
  EXPECT_NEAR(kHalfPi, fac_mid.meridian_parallel_angle, 1e-7);

  const PJ_FACTORS null_fac =
      proj_factors(nullptr, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_DOUBLE_EQ(0.0, null_fac.areal_scale);

  // 2. 4D proj_trans preserves z and t; PJ_IDENT is no-op.
  const PJ_COORD in_4d =
      proj_coord(DegToRad(-82.0), DegToRad(35.0), 512.25, 2024.75);
  const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, in_4d);
  EXPECT_DOUBLE_EQ(in_4d.v[0], ident.v[0]);
  EXPECT_DOUBLE_EQ(in_4d.v[1], ident.v[1]);
  EXPECT_DOUBLE_EQ(in_4d.v[2], ident.v[2]);
  EXPECT_DOUBLE_EQ(in_4d.v[3], ident.v[3]);

  const PJ_COORD fwd_4d = proj_trans(pj.get(), PJ_FWD, in_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(512.25, fwd_4d.xyzt.z);
  EXPECT_DOUBLE_EQ(2024.75, fwd_4d.xyzt.t);

  const PJ_COORD inv_4d = proj_trans(pj.get(), PJ_INV, fwd_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpzt.lam, inv_4d.lpzt.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpzt.phi, inv_4d.lpzt.phi, kRoundTripTolRad);
  EXPECT_DOUBLE_EQ(512.25, inv_4d.lpzt.z);
  EXPECT_DOUBLE_EQ(2024.75, inv_4d.lpzt.t);

  // 3. Classic 2D and 3D helpers agree with proj_trans.
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
  EXPECT_DOUBLE_EQ(512.25, xyz3d.z);
  const PJ_LPZ lpz3d = pj_inv3d(xyz3d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpz.lam, lpz3d.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpz.phi, lpz3d.phi, kRoundTripTolRad);
  EXPECT_DOUBLE_EQ(512.25, lpz3d.z);

  // 4. Batch proj_trans_array and strided proj_trans_generic.
  std::array<PJ_COORD, 2> valid_batch = {
      proj_coord(DegToRad(-96.0), DegToRad(23.0), 0.0, 0.0),
      proj_coord(DegToRad(-85.0), DegToRad(38.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, valid_batch.size(),
                                valid_batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, valid_batch.size(),
                                valid_batch.data()));
  EXPECT_NEAR(DegToRad(-96.0), valid_batch[0].lp.lam, kRoundTripTolRad);
  EXPECT_NEAR(DegToRad(23.0), valid_batch[0].lp.phi, kRoundTripTolRad);

  std::array<PJ_COORD, 2> mixed_inv_batch = {
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(3.0e7, 0.0, 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_trans_array(pj.get(), PJ_INV, mixed_inv_batch.size(),
                             mixed_inv_batch.data()));
  EXPECT_FALSE(std::isinf(mixed_inv_batch[0].lp.lam));
  EXPECT_TRUE(std::isinf(mixed_inv_batch[1].lp.lam));
  proj_errno_reset(pj.get());

  std::array<double, 2> xs = {DegToRad(-96.0), DegToRad(-90.0)};
  std::array<double, 2> ys = {DegToRad(23.0), DegToRad(35.0)};
  EXPECT_EQ(2u, proj_trans_generic(pj.get(), PJ_FWD, xs.data(), sizeof(double),
                                   xs.size(), ys.data(), sizeof(double),
                                   ys.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, xs[0], kMetricTolM);
  EXPECT_NEAR(0.0, ys[0], kMetricTolM);

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

// TODO(schwehr): Allocator OOM guards, P->opaque == nullptr destructor guard,
// aea_e_forward Q->rho < 0 rounding guard, and the legacy lp.phi == HUGE_VAL
// check after pj_authalic_lat_inverse remain uncovered.

}  // namespace
}  // namespace proj
