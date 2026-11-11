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
// Unit tests for the Oblique Mercator projection
// (proj/src/projections/omerc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/omerc.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
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
constexpr double kTolRad = 1e-10;
constexpr double kTolMeters = 1e-3;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

void ExpectRoundTrip(PJ* pj, double lon_deg, double lat_deg, double tol_rad) {
  const PJ_COORD input =
      proj_coord(DegToRad(lon_deg), DegToRad(lat_deg), 0.0, 0.0);
  // 1. Forward: geographic (lon, lat in radians) -> projected (x, y in meters).
  const PJ_COORD fwd = proj_trans(pj, PJ_FWD, input);
  EXPECT_EQ(0, proj_errno(pj)) << "lon=" << lon_deg << " lat=" << lat_deg;
  EXPECT_FALSE(std::isinf(fwd.xy.x)) << "lon=" << lon_deg << " lat=" << lat_deg;
  EXPECT_FALSE(std::isinf(fwd.xy.y)) << "lon=" << lon_deg << " lat=" << lat_deg;

  // 2. Inverse: projected (x, y in meters) -> geographic (lon, lat in radians).
  const PJ_COORD inv = proj_trans(pj, PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj)) << "lon=" << lon_deg << " lat=" << lat_deg;
  EXPECT_NEAR(input.lp.lam, inv.lp.lam, tol_rad)
      << "lon=" << lon_deg << " lat=" << lat_deg;
  EXPECT_NEAR(input.lp.phi, inv.lp.phi, tol_rad)
      << "lon=" << lon_deg << " lat=" << lat_deg;
}

TEST(OmercTest, CreateValidationErrorsAndIgnoredLon0Logging) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  {
    ScopedLogSuppressor suppress(ctx.get());
    struct ErrorCase {
      const char* name;
      const char* proj_def;
    };
    constexpr ErrorCase kInvalidDefs[] = {
        // Two-point mode illegal parameter checks.
        {
            .name = "two_point_lat_1_plus_90",
            .proj_def =
                "+proj=omerc +lat_0=45 +lon_1=0 +lat_1=90 +lon_2=20 +lat_2=50 "
                "+ellps=WGS84",
        },
        {
            .name = "two_point_lat_1_minus_90",
            .proj_def =
                "+proj=omerc +lat_0=45 +lon_1=0 +lat_1=-90 +lon_2=20 +lat_2=50 "
                "+ellps=WGS84",
        },
        {
            .name = "two_point_lat_2_plus_90",
            .proj_def =
                "+proj=omerc +lat_0=45 +lon_1=0 +lat_1=30 +lon_2=20 +lat_2=90 "
                "+ellps=WGS84",
        },
        {
            .name = "two_point_lat_2_minus_90",
            .proj_def =
                "+proj=omerc +lat_0=45 +lon_1=0 +lat_1=30 +lon_2=20 +lat_2=-90 "
                "+ellps=WGS84",
        },
        {
            .name = "two_point_equal_lat_1_lat_2",
            .proj_def =
                "+proj=omerc +lat_0=45 +lon_1=0 +lat_1=45 +lon_2=20 +lat_2=45 "
                "+ellps=WGS84",
        },
        {
            .name = "two_point_lat_1_zero",
            .proj_def =
                "+proj=omerc +lat_0=45 +lon_1=0 +lat_1=0 +lon_2=20 +lat_2=45 "
                "+ellps=WGS84",
        },
        {
            .name = "two_point_lat_0_plus_90",
            .proj_def =
                "+proj=omerc +lat_0=90 +lon_1=0 +lat_1=30 +lon_2=20 +lat_2=50 "
                "+ellps=WGS84",
        },
        {
            .name = "two_point_lat_0_minus_90",
            .proj_def =
                "+proj=omerc +lat_0=-90 +lon_1=0 +lat_1=30 +lon_2=20 +lat_2=50 "
                "+ellps=WGS84",
        },
        // Azimuth/gamma mode illegal parameter checks.
        {
            // With !alp && gam on a sphere, |gamma| must be <= 90 - |lat_0|.
            .name = "gamma_exceeds_max_for_lat_0",
            .proj_def = "+proj=omerc +lat_0=45 +lonc=10 +gamma=60 +R=6371000",
        },
        {
            .name = "alpha_lat_0_plus_90",
            .proj_def = "+proj=omerc +lat_0=90 +lonc=10 +alpha=30 +ellps=WGS84",
        },
        {
            .name = "alpha_lat_0_minus_90",
            .proj_def =
                "+proj=omerc +lat_0=-90 +lonc=10 +alpha=30 +ellps=WGS84",
        },
    };

    for (const auto& tc : kInvalidDefs) {
      ScopedPj bad(proj_create(ctx.get(), tc.proj_def));
      EXPECT_EQ(nullptr, bad.get()) << tc.name;
      EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
                proj_context_errno(ctx.get()))
          << tc.name;
    }
  }

  // Non-zero +lon_0 (with non-zero low 32 bits in radians) logs a trace notice.
  ScopedLogCapture capture(ctx.get(), PJ_LOG_TRACE);
  ScopedPj ignored_lon0(proj_create(
      ctx.get(),
      "+proj=omerc +lat_0=45 +lonc=10 +alpha=30 +lon_0=12.3 +ellps=WGS84"));
  ASSERT_NE(nullptr, ignored_lon0.get());
  EXPECT_TRUE(capture.ContainsMessage("lon_0 is ignored."));
}

TEST(OmercTest, AzimuthGammaOffsetAndRotationModes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Standard WGS84 azimuth mode (+alpha without +gamma).
  ScopedPj alpha_wgs84(proj_create(
      ctx.get(), "+proj=omerc +lat_0=45 +lonc=10 +alpha=30 +ellps=WGS84"));
  ASSERT_NE(nullptr, alpha_wgs84.get());
  EXPECT_EQ(1, proj_pj_info(alpha_wgs84.get()).has_inverse);

  struct RefCase {
    double lon_deg;
    double lat_deg;
    double expected_x;
    double expected_y;
  };
  constexpr RefCase kRefCases[] = {
      {
          .lon_deg = 10.0,
          .lat_deg = 45.0,
          .expected_x = 0.0,
          .expected_y = 0.0,
      },
      {
          .lon_deg = 12.0,
          .lat_deg = 46.0,
          .expected_x = 154915.9456857568,
          .expected_y = 113076.2020115474,
      },
      {
          .lon_deg = -5.0,
          .lat_deg = -20.0,
          .expected_x = -2350905.7441995614,
          .expected_y = -7052825.0835567322,
      },
  };
  for (const auto& tc : kRefCases) {
    const PJ_COORD fwd = proj_trans(
        alpha_wgs84.get(), PJ_FWD,
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(alpha_wgs84.get()));
    EXPECT_NEAR(tc.expected_x, fwd.xy.x, kTolMeters);
    EXPECT_NEAR(tc.expected_y, fwd.xy.y, kTolMeters);
    ExpectRoundTrip(alpha_wgs84.get(), tc.lon_deg, tc.lat_deg, kTolRad);
  }

  // Classic 2D pj_fwd / pj_inv entry points.
  const PJ_XY xy2d =
      pj_fwd({.lam = DegToRad(12.0), .phi = DegToRad(46.0)}, alpha_wgs84.get());
  EXPECT_EQ(0, proj_errno(alpha_wgs84.get()));
  EXPECT_NEAR(154915.9456857568, xy2d.x, kTolMeters);
  const PJ_LP lp2d = pj_inv(xy2d, alpha_wgs84.get());
  EXPECT_EQ(0, proj_errno(alpha_wgs84.get()));
  EXPECT_NEAR(DegToRad(12.0), lp2d.lam, kTolRad);
  EXPECT_NEAR(DegToRad(46.0), lp2d.phi, kTolRad);

  // 2. Combined +alpha and +gamma (skew vs rectified grid angle differ).
  ScopedPj alpha_and_gamma(
      proj_create(ctx.get(),
                  "+proj=omerc +lat_0=4 +lonc=102.25 +alpha=323.0257964666666 "
                  "+gamma=323.1301023611111 +k_0=0.99984 +ellps=WGS84"));
  ASSERT_NE(nullptr, alpha_and_gamma.get());
  const PJ_COORD rso_fwd =
      proj_trans(alpha_and_gamma.get(), PJ_FWD,
                 proj_coord(DegToRad(101.6869), DegToRad(3.1390), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(alpha_and_gamma.get()));
  EXPECT_NEAR(-62757.425152, rso_fwd.xy.x, kTolMeters);
  EXPECT_NEAR(-95064.514121, rso_fwd.xy.y, kTolMeters);
  ExpectRoundTrip(alpha_and_gamma.get(), 101.6869, 3.1390, kTolRad);

  // 3. Valid +gamma only (!alp && gam).
  ScopedPj gamma_only(proj_create(
      ctx.get(), "+proj=omerc +lat_0=45 +lonc=10 +gamma=25 +ellps=WGS84"));
  ASSERT_NE(nullptr, gamma_only.get());
  ExpectRoundTrip(gamma_only.get(), 12.0, 47.0, kTolRad);
  ExpectRoundTrip(gamma_only.get(), 7.0, 42.0, kTolRad);

  // 4. +no_off, +no_uoff, and +no_rot branches.
  ScopedPj no_off(proj_create(
      ctx.get(),
      "+proj=omerc +lat_0=45 +lonc=10 +alpha=30 +no_off +ellps=WGS84"));
  ScopedPj no_uoff(
      proj_create(ctx.get(),
                  "+proj=omerc +lat_0=45 +lonc=10 +alpha=30 +no_uoff +no_rot "
                  "+ellps=WGS84"));
  ASSERT_NE(nullptr, no_off.get());
  ASSERT_NE(nullptr, no_uoff.get());

  const std::string_view no_off_def(proj_pj_info(no_off.get()).definition);
  const std::string_view no_uoff_def(proj_pj_info(no_uoff.get()).definition);
  EXPECT_NE(std::string_view::npos, no_off_def.find("no_off")) << no_off_def;
  EXPECT_NE(std::string_view::npos, no_uoff_def.find("no_uoff")) << no_uoff_def;

  // With +no_off, u_0 is 0 so the projection center shifts by a*u_0 (~5.46e6m).
  const PJ_COORD no_off_center =
      proj_trans(no_off.get(), PJ_FWD,
                 proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(no_off.get()));
  EXPECT_NEAR(5461181.740, std::hypot(no_off_center.xy.x, no_off_center.xy.y),
              kTolMeters);

  ExpectRoundTrip(no_off.get(), 12.0, 46.0, kTolRad);
  ExpectRoundTrip(no_uoff.get(), 12.0, 46.0, kTolRad);
}

TEST(OmercTest, TwoPointModeNormalAndAntimeridianWrap) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Standard two-point WGS84 definition.
  ScopedPj two_pt(proj_create(
      ctx.get(),
      "+proj=omerc +lat_0=45 +lon_1=-5 +lat_1=40 +lon_2=25 +lat_2=55 "
      "+ellps=WGS84"));
  ASSERT_NE(nullptr, two_pt.get());

  const PJ_COORD fwd =
      proj_trans(two_pt.get(), PJ_FWD,
                 proj_coord(DegToRad(10.0), DegToRad(47.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(two_pt.get()));
  EXPECT_NEAR(620815.235388, fwd.xy.x, kTolMeters);
  EXPECT_NEAR(254466.091545, fwd.xy.y, kTolMeters);
  ExpectRoundTrip(two_pt.get(), 10.0, 47.0, kTolRad);
  ExpectRoundTrip(two_pt.get(), -5.0, 40.0, kTolRad);
  ExpectRoundTrip(two_pt.get(), 25.0, 55.0, kTolRad);

  // 2. Longitude difference < -pi and > +pi wrap branches across antimeridian.
  ScopedPj wrap_neg(proj_create(
      ctx.get(),
      "+proj=omerc +lat_0=40 +lon_1=-120 +lat_1=30 +lon_2=120 +lat_2=50 "
      "+R=6371000"));
  ScopedPj wrap_pos(proj_create(
      ctx.get(),
      "+proj=omerc +lat_0=40 +lon_1=120 +lat_1=30 +lon_2=-120 +lat_2=50 "
      "+R=6371000"));
  ASSERT_NE(nullptr, wrap_neg.get());
  ASSERT_NE(nullptr, wrap_pos.get());

  ExpectRoundTrip(wrap_neg.get(), 175.0, 40.0, kTolRad);
  ExpectRoundTrip(wrap_neg.get(), -175.0, 42.0, kTolRad);
  ExpectRoundTrip(wrap_pos.get(), 175.0, 40.0, kTolRad);
  ExpectRoundTrip(wrap_pos.get(), -175.0, 42.0, kTolRad);
}

TEST(OmercTest, EquatorialSouthernNearZeroAndSphericalBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Equatorial origin (|P->phi0| <= EPS -> else branch in setup).
  ScopedPj equatorial(proj_create(
      ctx.get(), "+proj=omerc +lat_0=0 +lonc=0 +alpha=30 +R=6371000"));
  ASSERT_NE(nullptr, equatorial.get());
  const PJ_COORD eq_center =
      proj_trans(equatorial.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(equatorial.get()));
  EXPECT_NEAR(0.0, eq_center.xy.x, 1e-6);
  EXPECT_NEAR(0.0, eq_center.xy.y, 1e-6);
  ExpectRoundTrip(equatorial.get(), 15.0, 10.0, kTolRad);
  ExpectRoundTrip(equatorial.get(), -20.0, -15.0, kTolRad);

  // 2. Southern hemisphere origin (P->phi0 < 0 -> negative F and u_0).
  ScopedPj southern(proj_create(
      ctx.get(), "+proj=omerc +lat_0=-45 +lonc=10 +alpha=30 +ellps=WGS84"));
  ASSERT_NE(nullptr, southern.get());
  const PJ_COORD south_center =
      proj_trans(southern.get(), PJ_FWD,
                 proj_coord(DegToRad(10.0), DegToRad(-45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(southern.get()));
  EXPECT_NEAR(0.0, south_center.xy.x, 1e-6);
  EXPECT_NEAR(0.0, south_center.xy.y, 1e-6);
  ExpectRoundTrip(southern.get(), 12.0, -43.0, kTolRad);
  ExpectRoundTrip(southern.get(), -5.0, -20.0, kTolRad);

  // 3. Tiny |phi0| > EPS (1.745e-9 > 1e-10) where spherical D^2 - 1 rounds to
  //    zero, hitting the (F = D*D - 1.) <= 0. clamping branch.
  ScopedPj tiny_lat0(proj_create(
      ctx.get(), "+proj=omerc +lat_0=1e-7 +lonc=0 +alpha=30 +R=6371000"));
  ASSERT_NE(nullptr, tiny_lat0.get());
  ExpectRoundTrip(tiny_lat0.get(), 0.0, 0.0, kTolRad);
  ExpectRoundTrip(tiny_lat0.get(), 5.0, 5.0, kTolRad);
}

TEST(OmercTest, PolesCosNearZeroAndDomainSingularities) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. North and south poles hit the |phi| == M_HALFPI branch in forward and
  //    the | |Up| - 1 | < EPS branch in inverse.
  ScopedPj wgs84(proj_create(
      ctx.get(), "+proj=omerc +lat_0=45 +lonc=10 +alpha=30 +ellps=WGS84"));
  ASSERT_NE(nullptr, wgs84.get());

  for (const double pole_phi : {kHalfPi, -kHalfPi}) {
    const PJ_COORD fwd_pole =
        proj_trans(wgs84.get(), PJ_FWD, proj_coord(0.0, pole_phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(wgs84.get())) << pole_phi;
    EXPECT_FALSE(std::isinf(fwd_pole.xy.x)) << pole_phi;
    EXPECT_FALSE(std::isinf(fwd_pole.xy.y)) << pole_phi;

    const PJ_COORD inv_pole = proj_trans(wgs84.get(), PJ_INV, fwd_pole);
    EXPECT_EQ(0, proj_errno(wgs84.get())) << pole_phi;
    EXPECT_NEAR(pole_phi, inv_pole.lp.phi, kTolRad) << pole_phi;
    EXPECT_TRUE(std::isfinite(inv_pole.lp.lam)) << pole_phi;
  }

  // 2. With +lat_0=0 +lonc=0 +alpha=0 on a sphere (B=1, lam0=0):
  //    - (lon=+-90 deg, lat=0) makes |U| == 1 -> forward domain error.
  //    - (lon=+90, lat=+30) and (-90, -30) give cos(B*lam) ~ 0 -> u = A*lam.
  ScopedPj sphere_axis(proj_create(
      ctx.get(), "+proj=omerc +lat_0=0 +lonc=0 +alpha=0 +R=6371000"));
  ASSERT_NE(nullptr, sphere_axis.get());

  ExpectRoundTrip(sphere_axis.get(), 90.0, 30.0, kTolRad);
  ExpectRoundTrip(sphere_axis.get(), -90.0, -30.0, kTolRad);

  for (const double singular_lon : {90.0, -90.0}) {
    const PJ_COORD singular =
        proj_trans(sphere_axis.get(), PJ_FWD,
                   proj_coord(DegToRad(singular_lon), 0.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, singular.xy.x) << singular_lon;
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(sphere_axis.get()))
        << singular_lon;
    proj_errno_reset(sphere_axis.get());
  }

  // 3. Inverse Qp == 0 underflow for huge positive v (both rotated & +no_rot).
  ScopedPj sphere_norot(proj_create(
      ctx.get(), "+proj=omerc +lat_0=0 +lonc=0 +alpha=0 +no_rot +R=6371000"));
  ASSERT_NE(nullptr, sphere_norot.get());

  const PJ_COORD underflow_rot =
      proj_trans(sphere_axis.get(), PJ_INV, proj_coord(1e10, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, underflow_rot.lp.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(sphere_axis.get()));
  proj_errno_reset(sphere_axis.get());

  const PJ_COORD underflow_norot =
      proj_trans(sphere_norot.get(), PJ_INV, proj_coord(0.0, 1e10, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, underflow_norot.lp.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(sphere_norot.get()));
}

TEST(OmercTest, ScaleFactorsBatchArrayGeneric4DAndEdgeCoords) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Conformality, scale k_0, and meridian convergence via proj_factors().
  constexpr double kK0 = 0.9996;
  ScopedPj rotated(proj_create(
      ctx.get(),
      "+proj=omerc +lat_0=45 +lonc=10 +alpha=30 +k_0=0.9996 +ellps=WGS84"));
  ScopedPj unrotated(proj_create(
      ctx.get(),
      "+proj=omerc +lat_0=45 +lonc=10 +alpha=30 +no_rot +R=6371000"));
  ASSERT_NE(nullptr, rotated.get());
  ASSERT_NE(nullptr, unrotated.get());

  const PJ_COORD center = proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0);
  const PJ_FACTORS fac_rot = proj_factors(rotated.get(), center);
  EXPECT_EQ(0, proj_errno(rotated.get()));
  EXPECT_NEAR(kK0, fac_rot.meridional_scale, 1e-6);
  EXPECT_NEAR(kK0, fac_rot.parallel_scale, 1e-6);
  EXPECT_NEAR(kK0 * kK0, fac_rot.areal_scale, 1e-6);
  EXPECT_NEAR(0.0, fac_rot.angular_distortion, 1e-6);
  EXPECT_NEAR(kHalfPi, fac_rot.meridian_parallel_angle, 1e-6);
  EXPECT_NEAR(0.0, fac_rot.meridian_convergence, 1e-7);

  const PJ_FACTORS fac_norot = proj_factors(unrotated.get(), center);
  // In +no_rot mode, (u, v) is left-handed so Jacobian det < 0 sets 2050, while
  // meridional and parallel scales are still computed as 1.0.
  proj_errno_reset(unrotated.get());
  EXPECT_NEAR(1.0, fac_norot.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, fac_norot.parallel_scale, 1e-6);
  EXPECT_NEAR(-(kHalfPi + DegToRad(30.0)), fac_norot.meridian_convergence,
              1e-7);

  // 2. Batch transform via proj_trans_array.
  std::array<PJ_COORD, 3> coords = {
      proj_coord(DegToRad(8.0), DegToRad(44.0), 100.0, 2020.0),
      proj_coord(DegToRad(10.0), DegToRad(45.0), 250.0, 2021.0),
      proj_coord(DegToRad(13.5), DegToRad(47.25), -25.0, 2022.0),
  };
  const std::array<PJ_COORD, 3> originals = coords;
  ASSERT_EQ(
      0, proj_trans_array(rotated.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(
      0, proj_trans_array(rotated.get(), PJ_INV, coords.size(), coords.data()));
  for (std::size_t i = 0; i < coords.size(); ++i) {
    EXPECT_NEAR(originals[i].xyzt.x, coords[i].xyzt.x, kTolRad) << "idx=" << i;
    EXPECT_NEAR(originals[i].xyzt.y, coords[i].xyzt.y, kTolRad) << "idx=" << i;
    EXPECT_DOUBLE_EQ(originals[i].xyzt.z, coords[i].xyzt.z) << "idx=" << i;
    EXPECT_DOUBLE_EQ(originals[i].xyzt.t, coords[i].xyzt.t) << "idx=" << i;
  }

  // 3. Strided 4D proj_trans_generic preserving z and t.
  double x = DegToRad(11.0);
  double y = DegToRad(46.0);
  double z = 512.25;
  double t = 2026.5;
  EXPECT_EQ(
      1u, proj_trans_generic(rotated.get(), PJ_FWD, &x, sizeof(x), 1, &y,
                             sizeof(y), 1, &z, sizeof(z), 1, &t, sizeof(t), 1));
  EXPECT_EQ(0, proj_errno(rotated.get()));
  EXPECT_EQ(
      1u, proj_trans_generic(rotated.get(), PJ_INV, &x, sizeof(x), 1, &y,
                             sizeof(y), 1, &z, sizeof(z), 1, &t, sizeof(t), 1));
  EXPECT_EQ(0, proj_errno(rotated.get()));
  EXPECT_NEAR(DegToRad(11.0), x, kTolRad);
  EXPECT_NEAR(DegToRad(46.0), y, kTolRad);
  EXPECT_DOUBLE_EQ(512.25, z);
  EXPECT_DOUBLE_EQ(2026.5, t);

  // 4. PJ_IDENT, nullptr PJ*, NaN, +Inf, and out-of-range latitude.
  const PJ_COORD ident_in = proj_coord(0.25, 0.5, 1.0, 2.0);
  const PJ_COORD ident_out = proj_trans(rotated.get(), PJ_IDENT, ident_in);
  EXPECT_DOUBLE_EQ(ident_in.xyzt.x, ident_out.xyzt.x);
  EXPECT_DOUBLE_EQ(ident_in.xyzt.y, ident_out.xyzt.y);
  const PJ_COORD null_out = proj_trans(nullptr, PJ_FWD, ident_in);
  EXPECT_DOUBLE_EQ(ident_in.xyzt.x, null_out.xyzt.x);

  const PJ_COORD nan_out = proj_trans(
      rotated.get(), PJ_FWD,
      proj_coord(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.xy.x));

  const PJ_COORD inf_out = proj_trans(
      rotated.get(), PJ_FWD,
      proj_coord(std::numeric_limits<double>::infinity(), 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inf_out.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(rotated.get()));
  proj_errno_reset(rotated.get());

  const PJ_COORD bad_lat = proj_trans(
      rotated.get(), PJ_FWD, proj_coord(0.0, DegToRad(95.0), 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_lat.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(rotated.get()));
}

// TODO(schwehr): Cover remaining defensive/unreachable branches in
// proj/src/projections/omerc.cpp if fault injection or extreme eccentricities
// are tested:
// - omerc_e_inverse() lines 112-113 (pj_phi2 == HUGE_VAL; reachable only for
//   unbounded/non-convergent inputs already excluded by |Up| < 1 - EPS)
// - PJ_PROJECTION(omerc) line 130 (calloc OOM)
// - PJ_PROJECTION(omerc) lines 258-260 and 272-274 (exact floating-point
//   cancellation p == 0 or denom == 0 in 2-point setup for extreme es -> 1)

}  // namespace
}  // namespace proj
