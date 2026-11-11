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
// Unit tests for the spherical and ellipsoidal Geostationary Satellite View
// projection (proj/src/projections/geos.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/geos.cpp

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
constexpr double kSatHeightM = 35785831.0;
constexpr double kRoundTripTolRad = 1e-11;
constexpr double kMetricTolM = 1e-4;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(GeosTest, InitializationInvalidArgsAndMetadata) {
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
      // Missing or non-positive +h fails radius_g_1 <= 0.
      {.def = "+proj=geos +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for h"},
      {.def = "+proj=geos +h=0 +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for h"},
      {.def = "+proj=geos +h=-1000 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for h"},
      // Extreme +h exceeds radius_g_1 > 1e10 limit.
      {.def = "+proj=geos +h=1e18 +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for h"},
      // Invalid +sweep values (wrong letter, multi-char, empty).
      {.def = "+proj=geos +h=35785831 +sweep=z +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for sweep"},
      {.def = "+proj=geos +h=35785831 +sweep=xy +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for sweep"},
      {.def = "+proj=geos +h=35785831 +sweep= +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for sweep"},
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

  // Mutable buffers for proj_create_argv (never const_cast string literals).
  char arg0[] = "proj=geos";
  char arg1[] = "h=35785831";
  char arg2[] = "sweep=y";
  char arg3[] = "ellps=WGS84";
  std::array<char*, 4> argv = {arg0, arg1, arg2, arg3};
  ScopedPj pj(
      proj_create_argv(ctx.get(), static_cast<int>(argv.size()), argv.data()));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("geos", info.id);
  EXPECT_THAT(pj->descr, HasSubstr("Geostationary Satellite View"));
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(pj.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(pj.get(), PJ_INV));
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));
}

TEST(GeosTest, SphericalAnalyticalForwardInverseAndSweepModes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_default(
      proj_create(ctx.get(), "+proj=geos +h=35785831 +R=6371000"));
  ScopedPj pj_sweep_y(
      proj_create(ctx.get(), "+proj=geos +h=35785831 +sweep=y +R=6371000"));
  ScopedPj pj_sweep_x(
      proj_create(ctx.get(), "+proj=geos +h=35785831 +sweep=x +R=6371000"));
  ASSERT_NE(nullptr, pj_default.get());
  ASSERT_NE(nullptr, pj_sweep_y.get());
  ASSERT_NE(nullptr, pj_sweep_x.get());
  EXPECT_EQ(0.0, pj_default->es);

  // 1. Sub-satellite origin maps to (0, 0).
  const PJ_COORD origin_y =
      proj_trans(pj_sweep_y.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_sweep_y.get()));
  EXPECT_DOUBLE_EQ(0.0, origin_y.xy.x);
  EXPECT_DOUBLE_EQ(0.0, origin_y.xy.y);

  // 2. Analytical verification at (lon=20 deg, lat=30 deg) for sweep=y vs x.
  const double lam = DegToRad(20.0);
  const double phi = DegToRad(30.0);
  const double vx = std::cos(phi) * std::cos(lam);
  const double vy = std::cos(phi) * std::sin(lam);
  const double vz = std::sin(phi);
  const double tmp = (1.0 + kSatHeightM / kSphereRadiusM) - vx;

  const PJ_COORD fwd_default =
      proj_trans(pj_default.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  const PJ_COORD fwd_y =
      proj_trans(pj_sweep_y.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  const PJ_COORD fwd_x =
      proj_trans(pj_sweep_x.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_default.get()));
  EXPECT_EQ(0, proj_errno(pj_sweep_y.get()));
  EXPECT_EQ(0, proj_errno(pj_sweep_x.get()));

  // Omitting +sweep defaults to sweep=y.
  EXPECT_DOUBLE_EQ(fwd_y.xy.x, fwd_default.xy.x);
  EXPECT_DOUBLE_EQ(fwd_y.xy.y, fwd_default.xy.y);

  const double expected_y_mode_x = kSatHeightM * std::atan(vy / tmp);
  const double expected_y_mode_y =
      kSatHeightM * std::atan(vz / std::hypot(vy, tmp));
  EXPECT_NEAR(expected_y_mode_x, fwd_y.xy.x, kMetricTolM);
  EXPECT_NEAR(expected_y_mode_y, fwd_y.xy.y, kMetricTolM);

  const double expected_x_mode_x =
      kSatHeightM * std::atan(vy / std::hypot(vz, tmp));
  const double expected_x_mode_y = kSatHeightM * std::atan(vz / tmp);
  EXPECT_NEAR(expected_x_mode_x, fwd_x.xy.x, kMetricTolM);
  EXPECT_NEAR(expected_x_mode_y, fwd_x.xy.y, kMetricTolM);

  // 3. Round-trip multiple coordinates on both sweep=y and sweep=x.
  struct LonLatDeg {
    double lon_deg;
    double lat_deg;
  };
  constexpr LonLatDeg kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},     {.lon_deg = 20.0, .lat_deg = 30.0},
      {.lon_deg = -35.0, .lat_deg = -25.0}, {.lon_deg = 50.0, .lat_deg = 10.0},
      {.lon_deg = -15.0, .lat_deg = 60.0},
  };
  for (PJ* pj : {pj_sweep_y.get(), pj_sweep_x.get()}) {
    for (const auto& tc : kCases) {
      const double in_lam = DegToRad(tc.lon_deg);
      const double in_phi = DegToRad(tc.lat_deg);
      const PJ_COORD fwd =
          proj_trans(pj, PJ_FWD, proj_coord(in_lam, in_phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj));
      const PJ_COORD inv = proj_trans(pj, PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj));
      EXPECT_NEAR(in_lam, inv.lp.lam, kRoundTripTolRad);
      EXPECT_NEAR(in_phi, inv.lp.phi, kRoundTripTolRad);
    }

    // Off-disk coordinate triggers det < 0 in geos_s_inverse.
    const PJ_COORD off_disk =
        proj_trans(pj, PJ_INV, proj_coord(1.0e7, 1.0e7, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(off_disk.lp.lam));
    EXPECT_TRUE(std::isinf(off_disk.lp.phi));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN, proj_errno(pj));
    proj_errno_reset(pj);
  }
}

TEST(GeosTest, EllipsoidalRoundTripVisibilityAndSweepModes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_default(
      proj_create(ctx.get(), "+proj=geos +h=35785831 +ellps=WGS84"));
  ScopedPj pj_sweep_y(proj_create(
      ctx.get(), "+proj=geos +h=35785831 +sweep=y +lon_0=-75 +ellps=WGS84"));
  ScopedPj pj_sweep_x(proj_create(
      ctx.get(), "+proj=geos +h=35785831 +sweep=x +lon_0=-75 +ellps=WGS84"));
  ScopedPj pj_sphere(
      proj_create(ctx.get(), "+proj=geos +h=35785831 +R=6378137"));
  ASSERT_NE(nullptr, pj_default.get());
  ASSERT_NE(nullptr, pj_sweep_y.get());
  ASSERT_NE(nullptr, pj_sweep_x.get());
  ASSERT_NE(nullptr, pj_sphere.get());

  // 1. Verify ellipsoidal geocentric flattening differs from sphere at 30N.
  const PJ_COORD ell_pt =
      proj_trans(pj_default.get(), PJ_FWD,
                 proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0));
  const PJ_COORD sph_pt =
      proj_trans(pj_sphere.get(), PJ_FWD,
                 proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_default.get()));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_GT(std::abs(ell_pt.xy.y - sph_pt.xy.y), 1000.0);

  // 2. Round-trip visible points with lon_0=-75 on both sweep=y and sweep=x.
  struct LonLatDeg {
    double lon_deg;
    double lat_deg;
  };
  constexpr LonLatDeg kVisibleCases[] = {
      {.lon_deg = -75.0, .lat_deg = 0.0},
      {.lon_deg = -60.0, .lat_deg = 25.0},
      {.lon_deg = -115.0, .lat_deg = 35.0},
      {.lon_deg = -45.0, .lat_deg = -45.0},
      {.lon_deg = -130.0, .lat_deg = -10.0},
  };
  for (PJ* pj : {pj_sweep_y.get(), pj_sweep_x.get()}) {
    for (const auto& tc : kVisibleCases) {
      const double in_lam = DegToRad(tc.lon_deg);
      const double in_phi = DegToRad(tc.lat_deg);
      const PJ_COORD fwd =
          proj_trans(pj, PJ_FWD, proj_coord(in_lam, in_phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj));
      const PJ_COORD inv = proj_trans(pj, PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj));
      EXPECT_NEAR(in_lam, inv.lp.lam, kRoundTripTolRad);
      EXPECT_NEAR(in_phi, inv.lp.phi, kRoundTripTolRad);
    }

    // 3. Points past the Earth limb or on the far side fail visibility check.
    constexpr LonLatDeg kHiddenCases[] = {
        {.lon_deg = 25.0, .lat_deg = 0.0},  // 100 deg east of lon_0 (far side)
        {.lon_deg = 7.0, .lat_deg = 0.0},   // 82 deg east (just past limb)
        {.lon_deg = -75.0, .lat_deg = 90.0},  // exact north pole (beyond limb)
    };
    for (const auto& tc : kHiddenCases) {
      const PJ_COORD hidden = proj_trans(
          pj, PJ_FWD,
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0));
      EXPECT_TRUE(std::isinf(hidden.xy.x));
      EXPECT_TRUE(std::isinf(hidden.xy.y));
      EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
                proj_errno(pj));
      proj_errno_reset(pj);
    }

    // 4. Off-disk inverse triggers det < 0 in geos_e_inverse.
    const PJ_COORD off_disk =
        proj_trans(pj, PJ_INV, proj_coord(-8.0e6, 8.0e6, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(off_disk.lp.lam));
    EXPECT_TRUE(std::isinf(off_disk.lp.phi));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN, proj_errno(pj));
    proj_errno_reset(pj);
  }
}

TEST(GeosTest, Transforms3D4DBatchGenericAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=geos +h=35785831 +x_0=100000 +y_0=-200000 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. 4D proj_trans preserves z and t; PJ_IDENT is no-op.
  const PJ_COORD in_4d =
      proj_coord(DegToRad(12.0), DegToRad(40.0), 250.5, 2026.5);
  const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, in_4d);
  EXPECT_DOUBLE_EQ(in_4d.v[0], ident.v[0]);
  EXPECT_DOUBLE_EQ(in_4d.v[1], ident.v[1]);
  EXPECT_DOUBLE_EQ(in_4d.v[2], ident.v[2]);
  EXPECT_DOUBLE_EQ(in_4d.v[3], ident.v[3]);

  const PJ_COORD fwd_4d = proj_trans(pj.get(), PJ_FWD, in_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(250.5, fwd_4d.xyzt.z);
  EXPECT_DOUBLE_EQ(2026.5, fwd_4d.xyzt.t);

  const PJ_COORD inv_4d = proj_trans(pj.get(), PJ_INV, fwd_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpzt.lam, inv_4d.lpzt.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpzt.phi, inv_4d.lpzt.phi, kRoundTripTolRad);
  EXPECT_DOUBLE_EQ(250.5, inv_4d.lpzt.z);
  EXPECT_DOUBLE_EQ(2026.5, inv_4d.lpzt.t);

  // 2. Classic 2D and 3D helpers agree with proj_trans.
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
  EXPECT_DOUBLE_EQ(250.5, xyz3d.z);
  const PJ_LPZ lpz3d = pj_inv3d(xyz3d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpz.lam, lpz3d.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpz.phi, lpz3d.phi, kRoundTripTolRad);
  EXPECT_DOUBLE_EQ(250.5, lpz3d.z);

  // 3. Batch proj_trans_array: all valid returns 0; mixed returns domain error.
  std::array<PJ_COORD, 2> valid_batch = {
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(DegToRad(-25.0), DegToRad(15.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, valid_batch.size(),
                                valid_batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, valid_batch.size(),
                                valid_batch.data()));
  EXPECT_NEAR(0.0, valid_batch[0].lp.lam, kRoundTripTolRad);
  EXPECT_NEAR(0.0, valid_batch[0].lp.phi, kRoundTripTolRad);

  std::array<PJ_COORD, 2> mixed_batch = {
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(DegToRad(100.0), 0.0, 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_trans_array(pj.get(), PJ_FWD, mixed_batch.size(),
                             mixed_batch.data()));
  EXPECT_FALSE(std::isinf(mixed_batch[0].xy.x));
  EXPECT_TRUE(std::isinf(mixed_batch[1].xy.x));
  proj_errno_reset(pj.get());

  // 4. proj_trans_generic with strided arrays.
  std::array<double, 2> xs = {0.0, DegToRad(10.0)};
  std::array<double, 2> ys = {0.0, DegToRad(20.0)};
  EXPECT_EQ(2u, proj_trans_generic(pj.get(), PJ_FWD, xs.data(), sizeof(double),
                                   xs.size(), ys.data(), sizeof(double),
                                   ys.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(100000.0, xs[0], kMetricTolM);
  EXPECT_NEAR(-200000.0, ys[0], kMetricTolM);

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

TEST(GeosTest, ScaleFactorsSphericalAndEllipsoidal) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj sph(proj_create(ctx.get(), "+proj=geos +h=35785831 +R=6371000"));
  ScopedPj ell(proj_create(ctx.get(), "+proj=geos +h=35785831 +ellps=WGS84"));
  ASSERT_NE(nullptr, sph.get());
  ASSERT_NE(nullptr, ell.get());

  // 1. At sub-satellite point (0, 0), both scales equal 1 and distortion is 0.
  const PJ_FACTORS sph_center =
      proj_factors(sph.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_NEAR(1.0, sph_center.meridional_scale, 1e-7);
  EXPECT_NEAR(1.0, sph_center.parallel_scale, 1e-7);
  EXPECT_NEAR(1.0, sph_center.areal_scale, 1e-7);
  EXPECT_NEAR(0.0, sph_center.angular_distortion, 1e-7);
  EXPECT_NEAR(kHalfPi, sph_center.meridian_parallel_angle, 1e-7);

  const PJ_FACTORS ell_center =
      proj_factors(ell.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ell.get()));
  EXPECT_NEAR(1.0, ell_center.meridional_scale, 1e-7);
  EXPECT_NEAR(1.0, ell_center.parallel_scale, 1e-7);
  EXPECT_NEAR(1.0, ell_center.areal_scale, 1e-7);

  // 2. Off-center visible point has positive areal scale and distortion > 0.
  const PJ_FACTORS ell_off = proj_factors(
      ell.get(), proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ell.get()));
  EXPECT_GT(ell_off.areal_scale, 0.0);
  EXPECT_LT(ell_off.areal_scale, 1.0);
  EXPECT_GT(ell_off.angular_distortion, 0.0);

  // 3. nullptr PJ returns zero-initialized factors.
  const PJ_FACTORS null_fac =
      proj_factors(nullptr, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_DOUBLE_EQ(0.0, null_fac.meridional_scale);
  EXPECT_DOUBLE_EQ(0.0, null_fac.areal_scale);
}

// TODO(schwehr): Only the calloc OOM guard in PJ_PROJECTION(geos) remains
// uncovered without allocator fault injection.

}  // namespace
}  // namespace proj
