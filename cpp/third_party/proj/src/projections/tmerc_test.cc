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
// Unit tests for Transverse Mercator (tmerc), Extended Transverse Mercator
// (etmerc), and Universal Transverse Mercator (utm) projections
// (proj/src/projections/tmerc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/tmerc.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(TmercTest, EllipsoidalExactAndDefaultRoundTripAndAlgorithmEquivalence) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Without +algo/+approx, tmerc calls pj_load_ini() and defaults to
  // Poder/Engsager (matching +algo=poder_engsager and +proj=etmerc).
  ScopedPj tmerc_default(proj_create(ctx.get(), "+proj=tmerc +ellps=WGS84"));
  ScopedPj tmerc_exact(
      proj_create(ctx.get(), "+proj=tmerc +algo=poder_engsager +ellps=WGS84"));
  ScopedPj etmerc(proj_create(ctx.get(), "+proj=etmerc +ellps=WGS84"));
  ASSERT_NE(nullptr, tmerc_default.get());
  ASSERT_NE(nullptr, tmerc_exact.get());
  ASSERT_NE(nullptr, etmerc.get());

  EXPECT_EQ(1, proj_pj_info(tmerc_default.get()).has_inverse);
  EXPECT_EQ(1, pj_has_inverse(etmerc.get()));
  EXPECT_EQ(1, proj_angular_input(etmerc.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(etmerc.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(etmerc.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(etmerc.get(), PJ_INV));

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 3.0, .lat_deg = 55.0},
      {.lon_deg = -15.0, .lat_deg = 40.0},
      {.lon_deg = 25.0, .lat_deg = -33.5},
      {.lon_deg = -60.0, .lat_deg = -50.0},
      {.lon_deg = 80.0, .lat_deg = 70.0},
      {.lon_deg = -80.0, .lat_deg = -70.0},
      // |dlon| > 90 deg at high latitude succeeds under Poder/Engsager (unlike
      // Evenden/Snyder).
      {.lon_deg = 100.0, .lat_deg = 70.0},
      {.lon_deg = 0.0, .lat_deg = 90.0},
      {.lon_deg = 0.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const PJ_COORD lp =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);

    // 1. Forward transforms across default, explicit exact, and etmerc.
    const PJ_COORD fwd_default = proj_trans(tmerc_default.get(), PJ_FWD, lp);
    const PJ_COORD fwd_exact = proj_trans(tmerc_exact.get(), PJ_FWD, lp);
    const PJ_COORD fwd_etmerc = proj_trans(etmerc.get(), PJ_FWD, lp);
    EXPECT_EQ(0, proj_errno(tmerc_default.get()));
    EXPECT_EQ(0, proj_errno(tmerc_exact.get()));
    EXPECT_EQ(0, proj_errno(etmerc.get()));
    EXPECT_NEAR(fwd_exact.xy.x, fwd_default.xy.x, 1e-9);
    EXPECT_NEAR(fwd_exact.xy.y, fwd_default.xy.y, 1e-9);
    EXPECT_NEAR(fwd_exact.xy.x, fwd_etmerc.xy.x, 1e-9);
    EXPECT_NEAR(fwd_exact.xy.y, fwd_etmerc.xy.y, 1e-9);

    // 2. Inverse round-trip.
    const PJ_COORD inv = proj_trans(etmerc.get(), PJ_INV, fwd_etmerc);
    EXPECT_EQ(0, proj_errno(etmerc.get()));
    EXPECT_NEAR(lp.lp.phi, inv.lp.phi, 1e-11);
    if (std::abs(tc.lat_deg) < 90.0) {
      EXPECT_NEAR(lp.lp.lam, inv.lp.lam, 1e-11);
    }
  }

  // Verify WGS84 quarter meridian at the poles (k0 = 1).
  const PJ_COORD north_pole =
      proj_trans(etmerc.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(etmerc.get()));
  EXPECT_NEAR(0.0, north_pole.xy.x, 1e-9);
  EXPECT_NEAR(10001965.7293, north_pole.xy.y, 1e-3);
}

TEST(TmercTest, NonZeroOriginFalseOffsetsAndScaleFactorRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  constexpr char kDef[] =
      "+proj=tmerc +ellps=WGS84 +lon_0=9 +lat_0=52 +k_0=0.9996 "
      "+x_0=500000 +y_0=100000";
  ScopedPj pj(proj_create(ctx.get(), kDef));
  ASSERT_NE(nullptr, pj.get());

  // 1. Origin (lon_0, lat_0) maps exactly to false easting/northing.
  const PJ_COORD origin = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(9.0), DegToRad(52.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(500000.0, origin.xy.x, 1e-6);
  EXPECT_NEAR(100000.0, origin.xy.y, 1e-6);

  // 2. Off-origin round-trip in both directions from central meridian.
  constexpr double kLons[] = {12.0, 5.0};
  constexpr double kLats[] = {55.0, 48.0};
  for (size_t i = 0; i < 2; ++i) {
    const PJ_COORD in =
        proj_coord(DegToRad(kLons[i]), DegToRad(kLats[i]), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-11);
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-11);
  }
}

TEST(TmercTest, EvendenSnyderApproxEllipsoidalForwardInverseAndPoles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj approx_flag(proj_create(
      ctx.get(),
      "+proj=tmerc +approx +ellps=WGS84 +lat_0=10 +k_0=0.9996 +x_0=1000 "
      "+y_0=2000"));
  ScopedPj approx_algo(proj_create(
      ctx.get(),
      "+proj=tmerc +algo=evenden_snyder +ellps=WGS84 +lat_0=10 +k_0=0.9996 "
      "+x_0=1000 +y_0=2000"));
  ScopedPj exact_ref(proj_create(
      ctx.get(),
      "+proj=tmerc +algo=poder_engsager +ellps=WGS84 +lat_0=10 +k_0=0.9996 "
      "+x_0=1000 +y_0=2000"));
  ASSERT_NE(nullptr, approx_flag.get());
  ASSERT_NE(nullptr, approx_algo.get());
  ASSERT_NE(nullptr, exact_ref.get());

  // 1. Mid-latitude points agree closely with Poder/Engsager and round-trip.
  struct ApproxCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr ApproxCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 10.0},
      {.lon_deg = 2.0, .lat_deg = 45.0},
      {.lon_deg = -2.0, .lat_deg = -30.0},
      // Exact poles exercise |cos(phi)| <= 1e-10 branch in approx_e_fwd().
      {.lon_deg = 0.0, .lat_deg = 90.0},
      {.lon_deg = 0.0, .lat_deg = -90.0},
  };
  for (const auto& tc : kCases) {
    const PJ_COORD lp =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD fwd_flag = proj_trans(approx_flag.get(), PJ_FWD, lp);
    const PJ_COORD fwd_algo = proj_trans(approx_algo.get(), PJ_FWD, lp);
    const PJ_COORD fwd_exact = proj_trans(exact_ref.get(), PJ_FWD, lp);
    EXPECT_EQ(0, proj_errno(approx_flag.get()));
    EXPECT_EQ(0, proj_errno(approx_algo.get()));
    EXPECT_NEAR(fwd_flag.xy.x, fwd_algo.xy.x, 1e-9);
    EXPECT_NEAR(fwd_flag.xy.y, fwd_algo.xy.y, 1e-9);
    EXPECT_NEAR(fwd_exact.xy.x, fwd_flag.xy.x, 1e-3);
    EXPECT_NEAR(fwd_exact.xy.y, fwd_flag.xy.y, 1e-3);

    const PJ_COORD inv = proj_trans(approx_algo.get(), PJ_INV, fwd_algo);
    EXPECT_EQ(0, proj_errno(approx_algo.get()));
    EXPECT_NEAR(lp.lp.phi, inv.lp.phi, 1e-11);
    if (std::abs(tc.lat_deg) < 90.0) {
      EXPECT_NEAR(lp.lp.lam, inv.lp.lam, 1e-11);
    }
  }

  // 2. Inverse northings past the quarter meridian clamp to +/-pi/2.
  const PJ_COORD clamp_north = proj_trans(approx_algo.get(), PJ_INV,
                                          proj_coord(1000.0, 2.0e7, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(approx_algo.get()));
  EXPECT_NEAR(0.0, clamp_north.lp.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, clamp_north.lp.phi, 1e-12);

  const PJ_COORD clamp_south = proj_trans(approx_algo.get(), PJ_INV,
                                          proj_coord(1000.0, -2.0e7, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(approx_algo.get()));
  EXPECT_NEAR(0.0, clamp_south.lp.lam, 1e-12);
  EXPECT_NEAR(-kHalfPi, clamp_south.lp.phi, 1e-12);

  // 3. Forward longitudes beyond +/-90 deg from central meridian fail.
  for (double bad_lon_deg : {91.0, -91.0}) {
    const PJ_COORD bad =
        proj_trans(approx_algo.get(), PJ_FWD,
                   proj_coord(DegToRad(bad_lon_deg), 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(bad.xy.x));
    EXPECT_TRUE(std::isinf(bad.xy.y));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(approx_algo.get()));
    proj_errno_reset(approx_algo.get());
  }
}

TEST(TmercTest, SphericalTransverseMercatorForwardInverseAndSingularities) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // Sphere (es == 0) always selects spherical Evenden/Snyder even if +algo is
  // explicitly set to poder_engsager.
  ScopedPj sph(proj_create(ctx.get(), "+proj=tmerc +R=6371000"));
  ScopedPj sph_override(
      proj_create(ctx.get(), "+proj=tmerc +R=6371000 +algo=poder_engsager"));
  ASSERT_NE(nullptr, sph.get());
  ASSERT_NE(nullptr, sph_override.get());

  // 1. Verify against closed-form spherical Transverse Mercator equations.
  struct SphericalCase {
    double lon_deg;
    double lat_deg;
    double expected_x;
    double expected_y;
  };
  constexpr SphericalCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0, .expected_x = 0.0, .expected_y = 0.0},
      {.lon_deg = 10.0,
       .lat_deg = 45.0,
       .expected_x = 786248.494465,
       .expected_y = 5052536.076938},
      {.lon_deg = -25.0,
       .lat_deg = -30.0,
       .expected_x = -2445154.170744,
       .expected_y = -3613677.760779},
      {.lon_deg = 30.0,
       .lat_deg = 0.0,
       .expected_x = 3499629.445552,
       .expected_y = 0.0},
      // Equator with |lon| > 90 deg maps y to pi * R so inverse round-trips.
      {.lon_deg = 120.0,
       .lat_deg = 0.0,
       .expected_x = 8390338.761308,
       .expected_y = 20015086.796021},
      {.lon_deg = -120.0,
       .lat_deg = 0.0,
       .expected_x = -8390338.761308,
       .expected_y = 20015086.796021},
  };

  for (const auto& tc : kCases) {
    const PJ_COORD lp =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(sph.get(), PJ_FWD, lp);
    const PJ_COORD fwd_ovr = proj_trans(sph_override.get(), PJ_FWD, lp);
    EXPECT_EQ(0, proj_errno(sph.get()));
    EXPECT_EQ(0, proj_errno(sph_override.get()));
    EXPECT_NEAR(tc.expected_x, fwd.xy.x, 1e-4);
    EXPECT_NEAR(tc.expected_y, fwd.xy.y, 1e-4);
    EXPECT_NEAR(fwd.xy.x, fwd_ovr.xy.x, 1e-9);
    EXPECT_NEAR(fwd.xy.y, fwd_ovr.xy.y, 1e-9);

    const PJ_COORD inv = proj_trans(sph.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(sph.get()));
    EXPECT_NEAR(lp.lp.lam, inv.lp.lam, 1e-11);
    EXPECT_NEAR(lp.lp.phi, inv.lp.phi, 1e-11);
  }

  // 2. Inverse at exact north pole (g == 0, h == cos(pi/2) == 0).
  const PJ_COORD pole_inv = proj_trans(
      sph.get(), PJ_INV, proj_coord(0.0, kHalfPi * 6371000.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_NEAR(0.0, pole_inv.lp.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, pole_inv.lp.phi, 1e-12);

  // 3. Near-equator point where cosphi != 1.0 and cos(y) can round to 1.0.
  const PJ_COORD near_eq = proj_trans(
      sph.get(), PJ_FWD, proj_coord(0.029845130209103034, 1.5e-8, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_LT(std::abs(near_eq.xy.y), 1.0);

  // 4. Non-zero lat_0, k_0, and y_0 round-trip across both hemispheres.
  ScopedPj sph_lat0(proj_create(
      ctx.get(), "+proj=tmerc +R=6371000 +lat_0=15 +k_0=0.9996 +y_0=1000000"));
  ASSERT_NE(nullptr, sph_lat0.get());
  for (const double lat_deg : {35.0, -20.0}) {
    const double lon_deg = lat_deg > 0.0 ? 5.0 : -5.0;
    const PJ_COORD lp = proj_coord(DegToRad(lon_deg), DegToRad(lat_deg), 0, 0);
    const PJ_COORD fwd = proj_trans(sph_lat0.get(), PJ_FWD, lp);
    EXPECT_EQ(0, proj_errno(sph_lat0.get()));
    const PJ_COORD inv = proj_trans(sph_lat0.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(sph_lat0.get()));
    EXPECT_NEAR(lp.lp.lam, inv.lp.lam, 1e-11);
    EXPECT_NEAR(lp.lp.phi, inv.lp.phi, 1e-11);
  }

  // 5. Singularity on forward at (lon = +/-90 deg, lat = 0) where |b| == 1.
  for (double bad_lon_deg : {90.0, -90.0}) {
    const PJ_COORD bad_fwd = proj_trans(
        sph.get(), PJ_FWD, proj_coord(DegToRad(bad_lon_deg), 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(bad_fwd.xy.x));
    EXPECT_TRUE(std::isinf(bad_fwd.xy.y));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(sph.get()));
    proj_errno_reset(sph.get());
  }

  // 6. Inverse underflow when exp(x / R) == 0.
  const PJ_COORD bad_inv =
      proj_trans(sph.get(), PJ_INV, proj_coord(-1.0e10, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(bad_inv.lp.lam));
  EXPECT_TRUE(std::isinf(bad_inv.lp.phi));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(sph.get()));
  proj_errno_reset(sph.get());
}

TEST(TmercTest, ExactDomainSingularitiesReturnHugeValAndErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=etmerc +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // Forward domain cutoff: |Ce| > 2.623395162778 near (lon = +/-90 deg, lat=0).
  for (double lon_deg : {88.0, -88.0}) {
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(DegToRad(lon_deg), 0.0, 0, 0));
    EXPECT_TRUE(std::isinf(fwd.xy.x));
    EXPECT_TRUE(std::isinf(fwd.xy.y));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }

  // Inverse domain cutoff: |Ce| = |x / Qn| > 2.623395162778 (~1.67e7 m).
  for (double bad_x : {2.0e7, -2.0e7}) {
    const PJ_COORD inv =
        proj_trans(pj.get(), PJ_INV, proj_coord(bad_x, 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(inv.lp.lam));
    EXPECT_TRUE(std::isinf(inv.lp.phi));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }

  // Verify error state is clean and subsequent transforms succeed.
  const PJ_COORD ok = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(3.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_FALSE(std::isinf(ok.xy.x));
}

TEST(TmercTest, AutoAlgorithmSelectionAndFallbackConditions) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj_auto(
      proj_create(ctx.get(), "+proj=tmerc +algo=auto +ellps=WGS84"));
  ScopedPj pj_approx(
      proj_create(ctx.get(), "+proj=tmerc +algo=evenden_snyder +ellps=WGS84"));
  ScopedPj pj_exact(
      proj_create(ctx.get(), "+proj=tmerc +algo=poder_engsager +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_auto.get());
  ASSERT_NE(nullptr, pj_approx.get());
  ASSERT_NE(nullptr, pj_exact.get());

  // 1. Within +/-3 deg of central meridian, auto dispatches to Evenden/Snyder.
  const PJ_COORD near_lp = proj_coord(DegToRad(1.0), DegToRad(45.0), 0.0, 0.0);
  const PJ_COORD auto_near = proj_trans(pj_auto.get(), PJ_FWD, near_lp);
  const PJ_COORD approx_near = proj_trans(pj_approx.get(), PJ_FWD, near_lp);
  EXPECT_EQ(0, proj_errno(pj_auto.get()));
  EXPECT_NEAR(approx_near.xy.x, auto_near.xy.x, 1e-9);
  EXPECT_NEAR(approx_near.xy.y, auto_near.xy.y, 1e-9);
  const PJ_COORD auto_near_inv = proj_trans(pj_auto.get(), PJ_INV, auto_near);
  EXPECT_EQ(0, proj_errno(pj_auto.get()));
  EXPECT_NEAR(near_lp.lp.lam, auto_near_inv.lp.lam, 1e-11);
  EXPECT_NEAR(near_lp.lp.phi, auto_near_inv.lp.phi, 1e-11);

  // 2. Beyond +/-3 deg of central meridian, auto dispatches to Poder/Engsager.
  const PJ_COORD far_lp = proj_coord(DegToRad(5.0), DegToRad(45.0), 0.0, 0.0);
  const PJ_COORD auto_far = proj_trans(pj_auto.get(), PJ_FWD, far_lp);
  const PJ_COORD exact_far = proj_trans(pj_exact.get(), PJ_FWD, far_lp);
  EXPECT_EQ(0, proj_errno(pj_auto.get()));
  EXPECT_NEAR(exact_far.xy.x, auto_far.xy.x, 1e-9);
  EXPECT_NEAR(exact_far.xy.y, auto_far.xy.y, 1e-9);
  const PJ_COORD auto_far_inv = proj_trans(pj_auto.get(), PJ_INV, auto_far);
  EXPECT_EQ(0, proj_errno(pj_auto.get()));
  EXPECT_NEAR(far_lp.lp.lam, auto_far_inv.lp.lam, 1e-11);
  EXPECT_NEAR(far_lp.lp.phi, auto_far_inv.lp.phi, 1e-11);

  // 3. +algo=auto falls back to PODER_ENGSAGER when phi0 != 0, |k0 - 1| > 0.01,
  //    or es > 0.1 (rf < ~200). Test at (100 deg, 70 deg) where approx fails.
  struct FallbackCase {
    const char* auto_def;
    const char* exact_def;
  };
  constexpr FallbackCase kFallbacks[] = {
      {.auto_def = "+proj=tmerc +algo=auto +ellps=WGS84 +lat_0=10",
       .exact_def = "+proj=tmerc +algo=poder_engsager +ellps=WGS84 +lat_0=10"},
      {.auto_def = "+proj=tmerc +algo=auto +ellps=WGS84 +k_0=0.95",
       .exact_def = "+proj=tmerc +algo=poder_engsager +ellps=WGS84 +k_0=0.95"},
      {.auto_def = "+proj=tmerc +algo=auto +a=6378137 +rf=50",
       .exact_def = "+proj=tmerc +algo=poder_engsager +a=6378137 +rf=50"},
  };
  const PJ_COORD high_lon =
      proj_coord(DegToRad(100.0), DegToRad(70.0), 0.0, 0.0);
  for (const auto& fb : kFallbacks) {
    ScopedPj a(proj_create(ctx.get(), fb.auto_def));
    ScopedPj e(proj_create(ctx.get(), fb.exact_def));
    ASSERT_NE(nullptr, a.get());
    ASSERT_NE(nullptr, e.get());

    const PJ_COORD fa = proj_trans(a.get(), PJ_FWD, high_lon);
    const PJ_COORD fe = proj_trans(e.get(), PJ_FWD, high_lon);
    EXPECT_EQ(0, proj_errno(a.get())) << fb.auto_def;
    EXPECT_EQ(0, proj_errno(e.get())) << fb.exact_def;
    EXPECT_NEAR(fe.xy.x, fa.xy.x, 1e-9);
    EXPECT_NEAR(fe.xy.y, fa.xy.y, 1e-9);
  }
}

TEST(TmercTest, ProjIniDefaultTmercAlgoOverrideViaInMemoryFs) {
  constexpr const char* kSearchPaths[] = {"/mem"};

  // 1. proj.ini sets tmerc_default_algo = evenden_snyder -> rejects |lon| > 90.
  {
    InMemoryFileSystem fs;
    fs.AddTextFile("/mem/proj.ini", "tmerc_default_algo = evenden_snyder\n");
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ScopedLogSuppressor suppress(ctx.get());

    ASSERT_TRUE(fs.Install(ctx.get()));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
    proj_context_set_search_paths(ctx.get(), 1, kSearchPaths);

    ScopedPj pj(proj_create(ctx.get(), "+proj=tmerc +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_TRUE(ctx->iniFileLoaded);
    EXPECT_EQ(TMercAlgo::EVENDEN_SNYDER, ctx->defaultTmercAlgo);

    const PJ_COORD out =
        proj_trans(pj.get(), PJ_FWD, proj_coord(DegToRad(95.0), 0.0, 0, 0));
    EXPECT_TRUE(std::isinf(out.xy.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
  }

  // 2. proj.ini sets tmerc_default_algo = auto.
  {
    InMemoryFileSystem fs;
    fs.AddTextFile("/mem/proj.ini", "tmerc_default_algo = auto\n");
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ScopedLogSuppressor suppress(ctx.get());

    ASSERT_TRUE(fs.Install(ctx.get()));
    proj_context_set_file_finder(ctx.get(), nullptr, nullptr);
    proj_context_set_search_paths(ctx.get(), 1, kSearchPaths);

    ScopedPj pj(proj_create(ctx.get(), "+proj=tmerc +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_TRUE(ctx->iniFileLoaded);
    EXPECT_EQ(TMercAlgo::AUTO, ctx->defaultTmercAlgo);
  }
}

TEST(TmercTest, UtmZonesHemispheresInferredZoneAndRoundTrips) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj utm32n(proj_create(ctx.get(), "+proj=utm +zone=32 +ellps=WGS84"));
  ScopedPj utm32s(
      proj_create(ctx.get(), "+proj=utm +zone=32 +south +ellps=WGS84"));
  ScopedPj utm32_approx(
      proj_create(ctx.get(), "+proj=utm +zone=32 +approx +ellps=WGS84"));
  ScopedPj utm32_auto(
      proj_create(ctx.get(), "+proj=utm +zone=32 +algo=auto +ellps=WGS84"));
  ASSERT_NE(nullptr, utm32n.get());
  ASSERT_NE(nullptr, utm32s.get());
  ASSERT_NE(nullptr, utm32_approx.get());
  ASSERT_NE(nullptr, utm32_auto.get());

  // 1. Equator on central meridian (9 E): (500000, 0) N / (500000, 10000000) S.
  const PJ_COORD cm_eq = proj_coord(DegToRad(9.0), 0.0, 0.0, 0.0);
  const PJ_COORD fwd_cm_n = proj_trans(utm32n.get(), PJ_FWD, cm_eq);
  const PJ_COORD fwd_cm_s = proj_trans(utm32s.get(), PJ_FWD, cm_eq);
  EXPECT_EQ(0, proj_errno(utm32n.get()));
  EXPECT_EQ(0, proj_errno(utm32s.get()));
  EXPECT_NEAR(500000.0, fwd_cm_n.xy.x, 1e-6);
  EXPECT_NEAR(0.0, fwd_cm_n.xy.y, 1e-6);
  EXPECT_NEAR(500000.0, fwd_cm_s.xy.x, 1e-6);
  EXPECT_NEAR(10000000.0, fwd_cm_s.xy.y, 1e-6);

  // 2. Reference coordinates at (12 E, +/-55) verified against Kruger series.
  const PJ_COORD pt_n = proj_coord(DegToRad(12.0), DegToRad(55.0), 0.0, 0.0);
  const PJ_COORD fwd_n = proj_trans(utm32n.get(), PJ_FWD, pt_n);
  EXPECT_EQ(0, proj_errno(utm32n.get()));
  EXPECT_NEAR(691875.6321, fwd_n.xy.x, 1e-3);
  EXPECT_NEAR(6098907.8250, fwd_n.xy.y, 1e-3);
  const PJ_COORD inv_n = proj_trans(utm32n.get(), PJ_INV, fwd_n);
  EXPECT_EQ(0, proj_errno(utm32n.get()));
  EXPECT_NEAR(pt_n.lp.lam, inv_n.lp.lam, 1e-11);
  EXPECT_NEAR(pt_n.lp.phi, inv_n.lp.phi, 1e-11);

  const PJ_COORD pt_s = proj_coord(DegToRad(12.0), DegToRad(-55.0), 0.0, 0.0);
  const PJ_COORD fwd_s = proj_trans(utm32s.get(), PJ_FWD, pt_s);
  EXPECT_EQ(0, proj_errno(utm32s.get()));
  EXPECT_NEAR(691875.6321, fwd_s.xy.x, 1e-3);
  EXPECT_NEAR(3901092.1749, fwd_s.xy.y, 1e-3);
  const PJ_COORD inv_s = proj_trans(utm32s.get(), PJ_INV, fwd_s);
  EXPECT_EQ(0, proj_errno(utm32s.get()));
  EXPECT_NEAR(pt_s.lp.lam, inv_s.lp.lam, 1e-11);
  EXPECT_NEAR(pt_s.lp.phi, inv_s.lp.phi, 1e-11);

  // Also check +approx and +algo=auto variants on (12 E, 55 N).
  const PJ_COORD fwd_approx = proj_trans(utm32_approx.get(), PJ_FWD, pt_n);
  const PJ_COORD fwd_auto = proj_trans(utm32_auto.get(), PJ_FWD, pt_n);
  EXPECT_EQ(0, proj_errno(utm32_approx.get()));
  EXPECT_EQ(0, proj_errno(utm32_auto.get()));
  EXPECT_NEAR(fwd_n.xy.x, fwd_approx.xy.x, 1e-3);
  EXPECT_NEAR(fwd_n.xy.y, fwd_approx.xy.y, 1e-3);
  EXPECT_NEAR(fwd_approx.xy.x, fwd_auto.xy.x, 1e-6);
  EXPECT_NEAR(fwd_approx.xy.y, fwd_auto.xy.y, 1e-6);

  // 3. Inferred zone from +lon_0, including -180/+180 zone clamping branches.
  ScopedPj inferred_32(
      proj_create(ctx.get(), "+proj=utm +lon_0=9 +ellps=WGS84"));
  ASSERT_NE(nullptr, inferred_32.get());
  const PJ_COORD fwd_inf32 = proj_trans(inferred_32.get(), PJ_FWD, pt_n);
  EXPECT_EQ(0, proj_errno(inferred_32.get()));
  EXPECT_NEAR(fwd_n.xy.x, fwd_inf32.xy.x, 1e-9);
  EXPECT_NEAR(fwd_n.xy.y, fwd_inf32.xy.y, 1e-9);

  // adjlon() preserves magnitudes < pi + 1e-12, so -180.00000000001 deg yields
  // floor < 0 (clamped to zone 1, lon_0 = -177) and +180.00000000001 deg yields
  // floor >= 60 (clamped to zone 60, lon_0 = +177).
  ScopedPj utm_z1(proj_create(ctx.get(), "+proj=utm +zone=1 +ellps=WGS84"));
  ScopedPj utm_z1_clamp(
      proj_create(ctx.get(), "+proj=utm +lon_0=-180.00000000001 +ellps=WGS84"));
  ScopedPj utm_z60(proj_create(ctx.get(), "+proj=utm +zone=60 +ellps=WGS84"));
  ScopedPj utm_z60_clamp(
      proj_create(ctx.get(), "+proj=utm +lon_0=180.00000000001 +ellps=WGS84"));
  ASSERT_NE(nullptr, utm_z1.get());
  ASSERT_NE(nullptr, utm_z1_clamp.get());
  ASSERT_NE(nullptr, utm_z60.get());
  ASSERT_NE(nullptr, utm_z60_clamp.get());

  const PJ_COORD pt_z1 = proj_coord(DegToRad(-177.0), DegToRad(20.0), 0, 0);
  const PJ_COORD f_z1 = proj_trans(utm_z1.get(), PJ_FWD, pt_z1);
  const PJ_COORD f_z1c = proj_trans(utm_z1_clamp.get(), PJ_FWD, pt_z1);
  EXPECT_EQ(0, proj_errno(utm_z1.get()));
  EXPECT_EQ(0, proj_errno(utm_z1_clamp.get()));
  EXPECT_NEAR(f_z1.xy.x, f_z1c.xy.x, 1e-9);
  EXPECT_NEAR(f_z1.xy.y, f_z1c.xy.y, 1e-9);

  const PJ_COORD pt_z60 = proj_coord(DegToRad(177.0), DegToRad(20.0), 0, 0);
  const PJ_COORD f_z60 = proj_trans(utm_z60.get(), PJ_FWD, pt_z60);
  const PJ_COORD f_z60c = proj_trans(utm_z60_clamp.get(), PJ_FWD, pt_z60);
  EXPECT_EQ(0, proj_errno(utm_z60.get()));
  EXPECT_EQ(0, proj_errno(utm_z60_clamp.get()));
  EXPECT_NEAR(f_z60.xy.x, f_z60c.xy.x, 1e-9);
  EXPECT_NEAR(f_z60.xy.y, f_z60c.xy.y, 1e-9);
}

TEST(TmercTest, ParameterValidationFailuresAndLogMessages) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  struct InvalidCase {
    const char* def;
    int expected_errno;
    const char* expected_log;
  };
  constexpr InvalidCase kCases[] = {
      {.def = "+proj=tmerc +algo=nonexistent +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for algo"},
      {.def = "+proj=etmerc +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for eccentricity"},
      {.def = "+proj=utm +zone=32 +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for eccentricity"},
      {.def = "+proj=utm +lon_0=60000 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for lon_0"},
      {.def = "+proj=utm +lon_0=-60000 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for lon_0"},
      {.def = "+proj=utm +zone=0 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for zone"},
      {.def = "+proj=utm +zone=-1 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for zone"},
      {.def = "+proj=utm +zone=61 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for zone"},
      {.def = "+proj=utm +zone=32 +algo=bad +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for algo"},
  };

  for (const auto& tc : kCases) {
    capture.Clear();
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    EXPECT_EQ(nullptr, pj.get()) << tc.def;
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get())) << tc.def;
    EXPECT_TRUE(capture.ContainsMessage(tc.expected_log)) << tc.def;
  }

  // Empty string and nullptr inputs fail gracefully.
  ScopedPj empty_pj(proj_create(ctx.get(), ""));
  EXPECT_EQ(nullptr, empty_pj.get());
  EXPECT_NE(0, proj_context_errno(ctx.get()));

  ScopedPj null_pj(proj_create(ctx.get(), nullptr));
  EXPECT_EQ(nullptr, null_pj.get());
  EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_context_errno(ctx.get()));
}

TEST(TmercTest, ProjFactorsConformalScaleAndConvergence) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj utm(proj_create(ctx.get(), "+proj=utm +zone=32 +ellps=WGS84"));
  ScopedPj approx(proj_create(ctx.get(), "+proj=tmerc +approx +ellps=WGS84"));
  ScopedPj sph(proj_create(ctx.get(), "+proj=tmerc +R=6371000"));
  ASSERT_NE(nullptr, utm.get());
  ASSERT_NE(nullptr, approx.get());
  ASSERT_NE(nullptr, sph.get());

  // 1. On UTM central meridian at equator: h = k = 0.9996, conv = 0.
  const PJ_FACTORS f_cm =
      proj_factors(utm.get(), proj_coord(DegToRad(9.0), 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(utm.get()));
  EXPECT_NEAR(0.9996, f_cm.meridional_scale, 1e-8);
  EXPECT_NEAR(0.9996, f_cm.parallel_scale, 1e-8);
  EXPECT_NEAR(0.9996 * 0.9996, f_cm.areal_scale, 1e-8);
  EXPECT_NEAR(0.0, f_cm.angular_distortion, 1e-8);
  EXPECT_NEAR(kHalfPi, f_cm.meridian_parallel_angle, 1e-8);
  EXPECT_NEAR(0.0, f_cm.meridian_convergence, 1e-8);

  // 2. Off central meridian (12 E, 55 N): conformal scale > k0, non-zero conv.
  const PJ_FACTORS f_off = proj_factors(
      utm.get(), proj_coord(DegToRad(12.0), DegToRad(55.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(utm.get()));
  EXPECT_GT(f_off.parallel_scale, 0.9996);
  EXPECT_NEAR(f_off.meridional_scale, f_off.parallel_scale, 1e-7);
  EXPECT_NEAR(f_off.tissot_semimajor, f_off.tissot_semiminor, 1e-7);
  EXPECT_NEAR(0.0, f_off.angular_distortion, 1e-7);
  EXPECT_GT(std::abs(f_off.meridian_convergence), 0.01);

  // 3. Approx and spherical variants are also conformal.
  const PJ_COORD pt = proj_coord(DegToRad(5.0), DegToRad(40.0), 0.0, 0.0);
  for (PJ* pj : {approx.get(), sph.get()}) {
    const PJ_FACTORS f = proj_factors(pj, pt);
    EXPECT_EQ(0, proj_errno(pj));
    EXPECT_NEAR(f.meridional_scale, f.parallel_scale, 1e-7);
    EXPECT_NEAR(0.0, f.angular_distortion, 1e-7);
    EXPECT_NEAR(kHalfPi, f.meridian_parallel_angle, 1e-7);
  }

  // Null PJ* returns zeroed factors safely.
  const PJ_FACTORS f_null = proj_factors(nullptr, pt);
  EXPECT_DOUBLE_EQ(0.0, f_null.meridional_scale);
  EXPECT_DOUBLE_EQ(0.0, f_null.parallel_scale);
}

TEST(TmercTest, Transform3D4DArrayGenericAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=utm +zone=32 +ellps=WGS84"));
  ScopedPj approx(proj_create(ctx.get(), "+proj=tmerc +approx +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  ASSERT_NE(nullptr, approx.get());

  // 1. 3D/4D coordinates preserve z and t across FWD and INV.
  const PJ_COORD in4d =
      proj_coord(DegToRad(12.0), DegToRad(55.0), 123.45, 2026.5);
  const PJ_COORD fwd4d = proj_trans(pj.get(), PJ_FWD, in4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(123.45, fwd4d.xyzt.z);
  EXPECT_DOUBLE_EQ(2026.5, fwd4d.xyzt.t);
  const PJ_COORD inv4d = proj_trans(pj.get(), PJ_INV, fwd4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in4d.xyzt.x, inv4d.xyzt.x, 1e-11);
  EXPECT_NEAR(in4d.xyzt.y, inv4d.xyzt.y, 1e-11);
  EXPECT_DOUBLE_EQ(123.45, inv4d.xyzt.z);
  EXPECT_DOUBLE_EQ(2026.5, inv4d.xyzt.t);

  // 2. PJ_IDENT and nullptr PJ* pass coordinates through unchanged.
  const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, in4d);
  EXPECT_DOUBLE_EQ(in4d.xyzt.x, ident.xyzt.x);
  EXPECT_DOUBLE_EQ(in4d.xyzt.y, ident.xyzt.y);
  const PJ_COORD null_tr = proj_trans(nullptr, PJ_FWD, in4d);
  EXPECT_DOUBLE_EQ(in4d.xyzt.x, null_tr.xyzt.x);

  // 3. proj_trans_array batch round-trip.
  std::array<PJ_COORD, 3> batch = {
      proj_coord(DegToRad(9.0), DegToRad(0.0), 10.0, 2020.0),
      proj_coord(DegToRad(10.5), DegToRad(48.2), 20.0, 2021.0),
      proj_coord(DegToRad(7.5), DegToRad(51.1), 30.0, 2022.0),
  };
  const auto original_batch = batch;
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, batch.size(), batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, batch.size(), batch.data()));
  for (size_t i = 0; i < batch.size(); ++i) {
    EXPECT_NEAR(original_batch[i].lpzt.lam, batch[i].lpzt.lam, 1e-11);
    EXPECT_NEAR(original_batch[i].lpzt.phi, batch[i].lpzt.phi, 1e-11);
    EXPECT_DOUBLE_EQ(original_batch[i].lpzt.z, batch[i].lpzt.z);
    EXPECT_DOUBLE_EQ(original_batch[i].lpzt.t, batch[i].lpzt.t);
  }

  // Mixed-validity batch reports domain error while transforming valid items.
  std::array<PJ_COORD, 2> mixed = {
      proj_coord(DegToRad(1.0), DegToRad(45.0), 0.0, 0.0),
      proj_coord(DegToRad(95.0), DegToRad(0.0), 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_trans_array(approx.get(), PJ_FWD, mixed.size(), mixed.data()));
  EXPECT_FALSE(std::isinf(mixed[0].xy.x));
  EXPECT_TRUE(std::isinf(mixed[1].xy.x));
  proj_errno_reset(approx.get());

  // 4. proj_trans_generic on separate coordinate arrays.
  std::array<double, 2> x = {DegToRad(8.0), DegToRad(11.0)};
  std::array<double, 2> y = {DegToRad(47.0), DegToRad(53.0)};
  std::array<double, 2> z = {100.0, 200.0};
  const auto orig_x = x;
  const auto orig_y = y;
  ASSERT_EQ(2u, proj_trans_generic(pj.get(), PJ_FWD, x.data(), sizeof(double),
                                   x.size(), y.data(), sizeof(double), y.size(),
                                   z.data(), sizeof(double), z.size(), nullptr,
                                   0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  ASSERT_EQ(2u, proj_trans_generic(pj.get(), PJ_INV, x.data(), sizeof(double),
                                   x.size(), y.data(), sizeof(double), y.size(),
                                   z.data(), sizeof(double), z.size(), nullptr,
                                   0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  for (size_t i = 0; i < 2; ++i) {
    EXPECT_NEAR(orig_x[i], x[i], 1e-11);
    EXPECT_NEAR(orig_y[i], y[i], 1e-11);
  }

  // 5. NaN and +/-Inf edge inputs.
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const double kInf = std::numeric_limits<double>::infinity();
  const PJ_COORD nan_out =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kNaN, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.xy.x));
  EXPECT_TRUE(std::isnan(nan_out.xy.y));

  const PJ_COORD err_in = proj_coord_error();
  const PJ_COORD err_out = proj_trans(pj.get(), PJ_FWD, err_in);
  EXPECT_TRUE(std::isinf(err_out.xy.x));
  proj_errno_reset(pj.get());

  const PJ_COORD inf_lon =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kInf, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_lon.xy.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  const PJ_COORD neg_inf_lat =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, -kInf, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(neg_inf_lat.xy.y));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

}  // namespace
}  // namespace proj
