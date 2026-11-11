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
// Unit tests for the spherical and ellipsoidal Orthographic projection
// (proj/src/projections/ortho.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/ortho.cpp

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
constexpr double kWgs84MajorM = 6378137.0;
constexpr double kWgs84MinorM = 6356752.314245179;

// ortho_e_inverse stops Newton-Raphson when |dphi| < 1e-12 && |dlam| < 1e-12;
// 1e-10 rad gives clean cross-arch margin for both closed-form and iterative
// round-trips while keeping sub-millimeter precision on Earth.
constexpr double kRoundTripTolRad = 1e-10;
constexpr double kMetricTolM = 1e-4;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(OrthoTest, InitializationInvalidDefinitionsAndMetadata) {
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
      {.def = "+proj=ortho +lat_0=95 +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
      {.def = "+proj=ortho +k_0=0 +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
      {.def = "+proj=ortho +R=0",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
      // Reversed DMS unit suffixes fail inside dmstor_ctx when parsing +alpha.
      {.def = "+proj=ortho +alpha=10d20r +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
  };
  for (const auto& tc : kInvalidCases) {
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj bad(proj_create(ctx.get(), tc.def));
    EXPECT_EQ(nullptr, bad.get());
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get()));
  }

  // Mutable buffers for proj_create_argv (never const_cast string literals).
  char arg0[] = "proj=ortho";
  char arg1[] = "lat_0=45";
  char arg2[] = "alpha=30";
  char arg3[] = "ellps=WGS84";
  std::array<char*, 4> argv = {arg0, arg1, arg2, arg3};
  ScopedPj pj(
      proj_create_argv(ctx.get(), static_cast<int>(argv.size()), argv.data()));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("ortho", info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", info.description);
  EXPECT_THAT(pj->descr, HasSubstr("Orthographic"));
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(pj.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(pj.get(), PJ_INV));
}

TEST(OrthoTest, SphericalAnalyticalForwardAndInverseAllModes) {
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
      // EQUIT: center origin hits |rh| <= EPS10 branch in ortho_s_inverse.
      {.def = "+proj=ortho +lat_0=0 +lon_0=0 +R=6371000",
       .lon_deg = 0.0,
       .lat_deg = 0.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0},
      {.def = "+proj=ortho +lat_0=0 +lon_0=0 +R=6371000",
       .lon_deg = 30.0,
       .lat_deg = 45.0,
       .expected_x_m = 2252488.651470,
       .expected_y_m = 4504977.302939},
      // OBLIQ: center origin and general off-center point.
      {.def = "+proj=ortho +lat_0=45 +lon_0=10 +R=6371000",
       .lon_deg = 10.0,
       .lat_deg = 45.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0},
      {.def = "+proj=ortho +lat_0=45 +lon_0=10 +R=6371000",
       .lon_deg = 25.0,
       .lat_deg = 55.0,
       .expected_x_m = 945790.912857,
       .expected_y_m = 1194358.461116},
      // OBLIQ with azimuth rotation +alpha, scale +k_0, and false offsets.
      {.def = "+proj=ortho +lat_0=45 +lon_0=10 +k_0=0.98 +alpha=30 "
              "+x_0=500000 +y_0=200000 +R=6371000",
       .lon_deg = -5.0,
       .lat_deg = 30.0,
       .expected_x_m = 30871.111884,
       .expected_y_m = -1786365.273744},
      // N_POLE: pole origin and 60N point.
      {.def = "+proj=ortho +lat_0=90 +lon_0=0 +R=6371000",
       .lon_deg = 0.0,
       .lat_deg = 90.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0},
      {.def = "+proj=ortho +lat_0=90 +lon_0=0 +R=6371000",
       .lon_deg = 30.0,
       .lat_deg = 60.0,
       .expected_x_m = 1592750.000000,
       .expected_y_m = -2758723.923755},
      // S_POLE: pole origin and 45S point.
      {.def = "+proj=ortho +lat_0=-90 +lon_0=0 +R=6371000",
       .lon_deg = 0.0,
       .lat_deg = -90.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0},
      {.def = "+proj=ortho +lat_0=-90 +lon_0=0 +R=6371000",
       .lon_deg = -60.0,
       .lat_deg = -45.0,
       .expected_x_m = -3901424.787818,
       .expected_y_m = 2252488.651470},
  };

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    const double lon_rad = DegToRad(tc.lon_deg);
    const double lat_rad = DegToRad(tc.lat_deg);

    // 1. Verify forward projection matches analytical spherical coordinates.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_x_m, fwd.xy.x, kMetricTolM) << tc.def;
    EXPECT_NEAR(tc.expected_y_m, fwd.xy.y, kMetricTolM) << tc.def;

    // 2. Round-trip back via inverse projection.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(lat_rad, inv.lp.phi, kRoundTripTolRad) << tc.def;
    if (std::abs(std::abs(tc.lat_deg) - 90.0) > 1e-6) {
      EXPECT_NEAR(lon_rad, inv.lp.lam, kRoundTripTolRad) << tc.def;
    }
  }
}

TEST(OrthoTest, SphericalInverseLimbClampingAndSpecialBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj equit(proj_create(ctx.get(), "+proj=ortho +lat_0=0 +R=6371000"));
  ASSERT_NE(nullptr, equit.get());

  // 1. 1.0 < sinc <= 1.0 + EPS10 clamps sinc to 1.0 without error.
  const PJ_COORD clamped =
      proj_trans(equit.get(), PJ_INV,
                 proj_coord(kSphereRadiusM * (1.0 + 1e-12), 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(equit.get()));
  EXPECT_NEAR(kHalfPi, clamped.lp.lam, 1e-12);
  EXPECT_NEAR(0.0, clamped.lp.phi, 1e-12);

  // 2. Exact limb points in EQUIT exercise |lp.phi| >= 1.0 clamping and the
  //    xy.y == 0.0 longitude branches (xy.x == 0, < 0, > 0).
  struct LimbCase {
    double x_m;
    double y_m;
    double expected_lon_rad;
    double expected_lat_rad;
  };
  constexpr LimbCase kLimbCases[] = {
      {.x_m = 0.0,
       .y_m = kSphereRadiusM,
       .expected_lon_rad = 0.0,
       .expected_lat_rad = kHalfPi},
      {.x_m = 0.0,
       .y_m = -kSphereRadiusM,
       .expected_lon_rad = 0.0,
       .expected_lat_rad = -kHalfPi},
      {.x_m = -kSphereRadiusM,
       .y_m = 0.0,
       .expected_lon_rad = -kHalfPi,
       .expected_lat_rad = 0.0},
      {.x_m = kSphereRadiusM,
       .y_m = 0.0,
       .expected_lon_rad = kHalfPi,
       .expected_lat_rad = 0.0},
  };
  for (const auto& tc : kLimbCases) {
    const PJ_COORD inv =
        proj_trans(equit.get(), PJ_INV, proj_coord(tc.x_m, tc.y_m, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(equit.get()));
    EXPECT_NEAR(tc.expected_lon_rad, inv.lp.lam, 1e-12);
    EXPECT_NEAR(tc.expected_lat_rad, inv.lp.phi, 1e-12);
  }

  // 3. OBLIQ North Pole (0, R*cos(phi0)) exercises |lp.phi| >= 1.0 clamping.
  ScopedPj obliq(proj_create(ctx.get(), "+proj=ortho +lat_0=45 +R=6371000"));
  ASSERT_NE(nullptr, obliq.get());
  const PJ_COORD pole_inv = proj_trans(
      obliq.get(), PJ_INV,
      proj_coord(0.0, kSphereRadiusM * std::cos(DegToRad(45.0)), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(obliq.get()));
  // asin(1 - 1ulp) differs from pi/2 by O(sqrt(eps)) ~ 1.5e-8 across CPUs.
  EXPECT_NEAR(kHalfPi, pole_inv.lp.phi, 1e-7);
}

TEST(OrthoTest, SphericalDomainErrorsAndTraceLogging) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_TRACE);

  struct DomainErrorCase {
    const char* def;
    double lon_deg;
    double lat_deg;
  };
  constexpr DomainErrorCase kBackHemisphere[] = {
      {.def = "+proj=ortho +lat_0=0 +R=6371000",
       .lon_deg = 100.0,
       .lat_deg = 0.0},
      {.def = "+proj=ortho +lat_0=45 +R=6371000",
       .lon_deg = 180.0,
       .lat_deg = -60.0},
      {.def = "+proj=ortho +lat_0=90 +R=6371000",
       .lon_deg = 0.0,
       .lat_deg = -10.0},
      {.def = "+proj=ortho +lat_0=-90 +R=6371000",
       .lon_deg = 0.0,
       .lat_deg = 10.0},
  };

  for (const auto& tc : kBackHemisphere) {
    capture.Clear();
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
    EXPECT_TRUE(capture.ContainsMessage("unprojected hemisphere")) << tc.def;
  }

  // Inverse point strictly outside disk (rh > 1 + EPS10) fails and recovers
  // after proj_errno_reset.
  ScopedPj pj(proj_create(ctx.get(), "+proj=ortho +lat_0=0 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());
  const PJ_COORD inv_err = proj_trans(
      pj.get(), PJ_INV, proj_coord(1.1 * kSphereRadiusM, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inv_err.lp.lam));
  EXPECT_TRUE(std::isinf(inv_err.lp.phi));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));

  proj_errno_reset(pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_COORD ok =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, ok.lp.lam, 1e-12);
  EXPECT_NEAR(0.0, ok.lp.phi, 1e-12);
}

TEST(OrthoTest, EllipsoidalForwardAndInverseAllModesAndAlpha) {
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
      // EQUIT: interior, exact equator (xy.y == 0), both poles (sinphi2 > 1),
      // and both east/west equatorial limbs (|sinlam| ~ 1).
      {.def = "+proj=ortho +lat_0=0 +ellps=WGS84",
       .lon_deg = 20.0,
       .lat_deg = 35.0,
       .expected_x_m = 1788911.337540,
       .expected_y_m = 3637866.909378,
       .check_xy = true},
      {.def = "+proj=ortho +lat_0=0 +ellps=WGS84",
       .lon_deg = -25.0,
       .lat_deg = -40.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0,
       .check_xy = false},
      {.def = "+proj=ortho +lat_0=0 +ellps=WGS84",
       .lon_deg = 15.0,
       .lat_deg = 0.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0,
       .check_xy = false},
      {.def = "+proj=ortho +lat_0=0 +ellps=WGS84",
       .lon_deg = 0.0,
       .lat_deg = 90.0,
       .expected_x_m = 0.0,
       .expected_y_m = kWgs84MinorM,
       .check_xy = true},
      {.def = "+proj=ortho +lat_0=0 +ellps=WGS84",
       .lon_deg = 0.0,
       .lat_deg = -90.0,
       .expected_x_m = 0.0,
       .expected_y_m = -kWgs84MinorM,
       .check_xy = true},
      {.def = "+proj=ortho +lat_0=0 +ellps=WGS84",
       .lon_deg = 90.0,
       .lat_deg = 0.0,
       .expected_x_m = kWgs84MajorM,
       .expected_y_m = 0.0,
       .check_xy = true},
      {.def = "+proj=ortho +lat_0=0 +ellps=WGS84",
       .lon_deg = -90.0,
       .lat_deg = 0.0,
       .expected_x_m = -kWgs84MajorM,
       .expected_y_m = 0.0,
       .check_xy = true},
      // N_POLE and S_POLE: interior, pole, and equatorial limb (rh2 == 1).
      {.def = "+proj=ortho +lat_0=90 +ellps=WGS84",
       .lon_deg = 30.0,
       .lat_deg = 65.0,
       .expected_x_m = 1351479.413001,
       .expected_y_m = -2340831.008702,
       .check_xy = true},
      {.def = "+proj=ortho +lat_0=90 +ellps=WGS84",
       .lon_deg = 45.0,
       .lat_deg = 0.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0,
       .check_xy = false},
      {.def = "+proj=ortho +lat_0=-90 +ellps=WGS84",
       .lon_deg = -40.0,
       .lat_deg = -70.0,
       .expected_x_m = -1406372.783847,
       .expected_y_m = 1676049.817677,
       .check_xy = true},
      {.def = "+proj=ortho +lat_0=-90 +ellps=WGS84",
       .lon_deg = -60.0,
       .lat_deg = 0.0,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0,
       .check_xy = false},
      // OBLIQ with +alpha, +k_0, +x_0, +y_0.
      {.def = "+proj=ortho +lat_0=45 +lon_0=10 +k_0=0.9996 +alpha=30 "
              "+x_0=500000 +y_0=200000 +ellps=WGS84",
       .lon_deg = 12.0,
       .lat_deg = 55.0,
       .expected_x_m = 56883.663089,
       .expected_y_m = 1223322.176466,
       .check_xy = true},
      // OBLIQ pole-crossing cases: initial spherical guess lands on the
      // wrong side of the pole, so Newton steps cross +pi/2 and -pi/2 and
      // fold longitude by pi.
      {.def = "+proj=ortho +lat_0=45 +ellps=WGS84",
       .lon_deg = 179.0,
       .lat_deg = 89.95,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0,
       .check_xy = false},
      {.def = "+proj=ortho +lat_0=-45 +ellps=WGS84",
       .lon_deg = 179.0,
       .lat_deg = -89.95,
       .expected_x_m = 0.0,
       .expected_y_m = 0.0,
       .check_xy = false},
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
}

TEST(OrthoTest, EllipsoidalDomainErrorsAndNewtonNonConvergence) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Forward unprojected hemisphere check on ellipsoid (OBLIQ & EQUIT).
  {
    ScopedPj pj(proj_create(ctx.get(), "+proj=ortho +lat_0=45 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());
    const PJ_COORD err =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(180.0), DegToRad(-50.0), 0.0, 0.0));
    EXPECT_TRUE(std::isinf(err.xy.x));
    EXPECT_TRUE(std::isinf(err.xy.y));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
  }

  // 2. Inverse outer boundary errors across N_POLE, S_POLE, EQUIT, OBLIQ.
  struct InvErrorCase {
    const char* def;
    double x_m;
    double y_m;
  };
  constexpr InvErrorCase kInvErrors[] = {
      {.def = "+proj=ortho +lat_0=90 +ellps=WGS84",
       .x_m = 1.1 * kWgs84MajorM,
       .y_m = 0.0},
      {.def = "+proj=ortho +lat_0=-90 +ellps=WGS84",
       .x_m = 0.0,
       .y_m = -1.1 * kWgs84MajorM},
      {.def = "+proj=ortho +lat_0=0 +ellps=WGS84",
       .x_m = 0.0,
       .y_m = 1.1 * kWgs84MajorM},
      {.def = "+proj=ortho +lat_0=45 +ellps=WGS84",
       .x_m = 1.5 * kWgs84MajorM,
       .y_m = 0.0},
      // At lat_0=45 on WGS84, minimum attainable normalized yp is ~ -0.99497,
      // while (0.03, -0.996) passes the recentered unit-disk test
      // (x^2 + y_rec^2 ~ 0.9963 <= 1) and exhausts all 20 Newton iterations.
      {.def = "+proj=ortho +lat_0=45 +ellps=WGS84",
       .x_m = 0.03 * kWgs84MajorM,
       .y_m = -0.996 * kWgs84MajorM},
  };
  for (const auto& tc : kInvErrors) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;
    const PJ_COORD err =
        proj_trans(pj.get(), PJ_INV, proj_coord(tc.x_m, tc.y_m, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(err.lp.lam)) << tc.def;
    EXPECT_TRUE(std::isinf(err.lp.phi)) << tc.def;
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()))
        << tc.def;
  }
}

TEST(OrthoTest, Transforms3D4DBatchGenericAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=ortho +lat_0=45 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. 4D proj_trans preserves z and t across round-trip; PJ_IDENT is no-op.
  const PJ_COORD in_4d =
      proj_coord(DegToRad(15.0), DegToRad(50.0), 420.5, 2025.25);
  const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, in_4d);
  EXPECT_DOUBLE_EQ(in_4d.v[0], ident.v[0]);
  EXPECT_DOUBLE_EQ(in_4d.v[1], ident.v[1]);
  EXPECT_DOUBLE_EQ(in_4d.v[2], ident.v[2]);
  EXPECT_DOUBLE_EQ(in_4d.v[3], ident.v[3]);

  const PJ_COORD fwd_4d = proj_trans(pj.get(), PJ_FWD, in_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(420.5, fwd_4d.xyzt.z);
  EXPECT_DOUBLE_EQ(2025.25, fwd_4d.xyzt.t);

  const PJ_COORD inv_4d = proj_trans(pj.get(), PJ_INV, fwd_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpzt.lam, inv_4d.lpzt.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpzt.phi, inv_4d.lpzt.phi, kRoundTripTolRad);
  EXPECT_DOUBLE_EQ(420.5, inv_4d.lpzt.z);
  EXPECT_DOUBLE_EQ(2025.25, inv_4d.lpzt.t);

  // 2. Classic 2D and 3D internal helpers agree with proj_trans.
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
  EXPECT_DOUBLE_EQ(420.5, xyz3d.z);
  const PJ_LPZ lpz3d = pj_inv3d(xyz3d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpz.lam, lpz3d.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpz.phi, lpz3d.phi, kRoundTripTolRad);
  EXPECT_DOUBLE_EQ(420.5, lpz3d.z);

  // 3. Batch proj_trans_array: all valid returns 0; mixed returns domain error.
  std::array<PJ_COORD, 2> valid_batch = {
      proj_coord(0.0, DegToRad(45.0), 0.0, 0.0),
      proj_coord(DegToRad(10.0), DegToRad(50.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, valid_batch.size(),
                                valid_batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, valid_batch.size(),
                                valid_batch.data()));
  EXPECT_NEAR(0.0, valid_batch[0].lp.lam, kRoundTripTolRad);
  EXPECT_NEAR(DegToRad(45.0), valid_batch[0].lp.phi, kRoundTripTolRad);

  std::array<PJ_COORD, 2> mixed_batch = {
      proj_coord(0.0, DegToRad(45.0), 0.0, 0.0),
      proj_coord(DegToRad(180.0), DegToRad(-60.0), 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_trans_array(pj.get(), PJ_FWD, mixed_batch.size(),
                             mixed_batch.data()));
  EXPECT_FALSE(std::isinf(mixed_batch[0].xy.x));
  EXPECT_TRUE(std::isinf(mixed_batch[1].xy.x));
  proj_errno_reset(pj.get());

  // 4. proj_trans_generic with strided buffers.
  std::array<double, 2> xs = {0.0, DegToRad(12.0)};
  std::array<double, 2> ys = {DegToRad(45.0), DegToRad(52.0)};
  EXPECT_EQ(2u, proj_trans_generic(pj.get(), PJ_FWD, xs.data(), sizeof(double),
                                   xs.size(), ys.data(), sizeof(double),
                                   ys.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, xs[0], kMetricTolM);
  EXPECT_NEAR(0.0, ys[0], kMetricTolM);

  // 5. Edge inputs: nullptr PJ, NaN, Inf, and out-of-range latitude.
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

  const PJ_COORD neg_inf_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(-HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(neg_inf_inv.lp.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
}

TEST(OrthoTest, ScaleFactorsSphericalAndEllipsoidal) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Spherical center has unit scale and zero angular distortion; moving
  //    along the central meridian scales radially by cos(phi).
  ScopedPj sph(proj_create(ctx.get(), "+proj=ortho +lat_0=0 +R=6371000"));
  ASSERT_NE(nullptr, sph.get());

  const PJ_FACTORS sph_center =
      proj_factors(sph.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_NEAR(1.0, sph_center.meridional_scale, 1e-8);
  EXPECT_NEAR(1.0, sph_center.parallel_scale, 1e-8);
  EXPECT_NEAR(1.0, sph_center.areal_scale, 1e-8);
  EXPECT_NEAR(0.0, sph_center.angular_distortion, 1e-8);
  EXPECT_NEAR(kHalfPi, sph_center.meridian_parallel_angle, 1e-8);

  const double lat30 = DegToRad(30.0);
  const PJ_FACTORS sph_30n =
      proj_factors(sph.get(), proj_coord(0.0, lat30, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_NEAR(std::cos(lat30), sph_30n.meridional_scale, 1e-7);
  EXPECT_NEAR(1.0, sph_30n.parallel_scale, 1e-7);
  EXPECT_NEAR(std::cos(lat30), sph_30n.areal_scale, 1e-7);
  EXPECT_NEAR(kHalfPi, sph_30n.meridian_parallel_angle, 1e-7);

  // 2. Ellipsoidal projection center respects +k_0 even with +alpha rotation.
  ScopedPj ell(proj_create(
      ctx.get(), "+proj=ortho +lat_0=45 +k_0=0.95 +alpha=30 +ellps=WGS84"));
  ASSERT_NE(nullptr, ell.get());
  const PJ_FACTORS ell_center =
      proj_factors(ell.get(), proj_coord(0.0, DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ell.get()));
  EXPECT_NEAR(0.95, ell_center.meridional_scale, 1e-7);
  EXPECT_NEAR(0.95, ell_center.parallel_scale, 1e-7);
  EXPECT_NEAR(0.95 * 0.95, ell_center.areal_scale, 1e-7);
  EXPECT_NEAR(0.0, ell_center.angular_distortion, 1e-7);

  // 3. nullptr PJ safely returns zero-initialized factors.
  const PJ_FACTORS null_fac =
      proj_factors(nullptr, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_DOUBLE_EQ(0.0, null_fac.meridional_scale);
  EXPECT_DOUBLE_EQ(0.0, null_fac.parallel_scale);
}

// TODO(schwehr): Only the calloc OOM guard in PJ_PROJECTION(ortho) remains
// uncovered without allocator fault injection.

}  // namespace
}  // namespace proj
