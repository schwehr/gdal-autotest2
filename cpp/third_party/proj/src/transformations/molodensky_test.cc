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
// Unit tests for standard and abridged Molodensky coordinate transformations
// (proj/src/transformations/molodensky.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/molodensky.cpp

#include <cmath>
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

TEST(MolodenskyTest, StandardMolodenskyAnalyticalMatchAnd3D4DRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // International 1924 -> WGS84 approximate Molodensky shift parameters.
  ScopedPj pj(
      proj_create(ctx.get(),
                  "+proj=molodensky +ellps=intl +dx=-84 +dy=-107 +dz=-120 "
                  "+da=-251 +df=-0.000014192702"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj->left);
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj->right);

  // 1. Verify exact analytical standard Molodensky formula at mid-latitude.
  const double lam = DegToRad(15.0);
  const double phi = DegToRad(50.0);
  const double h = 200.0;
  const double a = pj->a;
  const double es = pj->es;
  const double f = pj->f;
  constexpr double kDx = -84.0;
  constexpr double kDy = -107.0;
  constexpr double kDz = -120.0;
  constexpr double kDa = -251.0;
  constexpr double kDf = -0.000014192702;

  const double slam = std::sin(lam);
  const double clam = std::cos(lam);
  const double sphi = std::sin(phi);
  const double cphi = std::cos(phi);
  const double rho = (a * (1.0 - es)) / std::pow(1.0 - es * sphi * sphi, 1.5);
  const double nu = a / std::sqrt(1.0 - es * sphi * sphi);

  const double expected_dphi =
      ((-kDx * sphi * clam) - (kDy * sphi * slam) + (kDz * cphi) +
       ((nu * es * sphi * cphi * kDa) / a) +
       (sphi * cphi * (rho / (1.0 - f) + nu * (1.0 - f)) * kDf)) /
      (rho + h);
  const double expected_dlam = (-kDx * slam + kDy * clam) / ((nu + h) * cphi);
  const double expected_dh = kDx * cphi * clam + kDy * cphi * slam +
                             kDz * sphi - (a / nu) * kDa +
                             nu * (1.0 - f) * sphi * sphi * kDf;

  const PJ_COORD in = proj_coord(lam, phi, h, 2025.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam + expected_dlam, fwd.lpz.lam, 1e-15);
  EXPECT_NEAR(phi + expected_dphi, fwd.lpz.phi, 1e-15);
  EXPECT_NEAR(h + expected_dh, fwd.lpz.z, 1e-12);
  EXPECT_DOUBLE_EQ(2025.0, fwd.xyzt.t);

  // 2. Verify 4D inverse and 3D entry points (pj_fwd3d / pj_inv3d) Across
  // equator (phi == 0 branch in RM), mid-latitudes, and poles (fabs(phi) ==
  // M_PI_2 branch in RM).
  struct CoordCase {
    double lon_rad;
    double lat_rad;
    double height_m;
  };
  constexpr CoordCase kCases[] = {
      // Equator exercises `if (phi == 0) return a * (1 - es);` in RM.
      {.lon_rad = 0.0, .lat_rad = 0.0, .height_m = 0.0},
      {.lon_rad = DegToRad(12.0), .lat_rad = DegToRad(55.0), .height_m = 150.0},
      {.lon_rad = DegToRad(-75.0),
       .lat_rad = DegToRad(-33.0),
       .height_m = 500.0},
  };

  for (const auto& tc : kCases) {
    const PJ_COORD pt = proj_coord(tc.lon_rad, tc.lat_rad, tc.height_m, 0.0);
    const PJ_COORD f4d = proj_trans(pj.get(), PJ_FWD, pt);
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD i4d = proj_trans(pj.get(), PJ_INV, f4d);
    EXPECT_EQ(0, proj_errno(pj.get()));
    // Molodensky reverse evaluates the differential at the shifted coordinate,
    // so round-trip agrees to second order in (dphi, dlam).
    EXPECT_NEAR(tc.lon_rad, i4d.lpz.lam, 1e-8);
    EXPECT_NEAR(tc.lat_rad, i4d.lpz.phi, 1e-8);
    EXPECT_NEAR(tc.height_m, i4d.lpz.z, 1e-2);

    const PJ_XYZ f3d = pj_fwd3d(pt.lpz, pj.get());
    EXPECT_NEAR(f4d.xyz.x, f3d.x, 1e-15);
    EXPECT_NEAR(f4d.xyz.y, f3d.y, 1e-15);
    EXPECT_NEAR(f4d.xyz.z, f3d.z, 1e-15);

    const PJ_LPZ i3d = pj_inv3d(f3d, pj.get());
    EXPECT_NEAR(i4d.lpz.lam, i3d.lam, 1e-15);
    EXPECT_NEAR(i4d.lpz.phi, i3d.phi, 1e-15);
    EXPECT_NEAR(i4d.lpz.z, i3d.z, 1e-15);
  }

  // 3. Exact poles (phi = +-pi/2) with dx = dy = 0 exercise `if (fabs(phi) ==
  // M_PI_2) return a / sqrt(1 - es);` in RM while keeping dlam = 0.
  ScopedPj pj_axial(proj_create(
      ctx.get(),
      "+proj=molodensky +ellps=WGS84 +dx=0 +dy=0 +dz=10 +da=-5 +df=0"));
  ASSERT_NE(nullptr, pj_axial.get());
  for (const double pole_phi : {kHalfPi, -kHalfPi}) {
    const PJ_COORD pole_in = proj_coord(0.0, pole_phi, 100.0, 0.0);
    const PJ_COORD pole_fwd = proj_trans(pj_axial.get(), PJ_FWD, pole_in);
    EXPECT_EQ(0, proj_errno(pj_axial.get()));
    EXPECT_NEAR(0.0, pole_fwd.lpz.lam, 1e-15);
    EXPECT_FALSE(std::isnan(pole_fwd.lpz.phi));
    const PJ_COORD pole_inv = proj_trans(pj_axial.get(), PJ_INV, pole_fwd);
    EXPECT_EQ(0, proj_errno(pj_axial.get()));
    EXPECT_NEAR(pole_phi, pole_inv.lpz.phi, 1e-9);
    EXPECT_NEAR(100.0, pole_inv.lpz.z, 1e-3);
  }
}

TEST(MolodenskyTest, AbridgedMolodenskyAnalyticalMatchAndHeightIndependence) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=molodensky +abridged +ellps=intl +dx=-84 +dy=-107 +dz=-120 "
      "+da=-251 +df=-0.000014192702"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Verify exact analytical abridged Molodensky formula.
  const double lam = DegToRad(15.0);
  const double phi = DegToRad(50.0);
  const double h = 200.0;
  const double a = pj->a;
  const double es = pj->es;
  const double f = pj->f;
  constexpr double kDx = -84.0;
  constexpr double kDy = -107.0;
  constexpr double kDz = -120.0;
  constexpr double kDa = -251.0;
  constexpr double kDf = -0.000014192702;

  const double slam = std::sin(lam);
  const double clam = std::cos(lam);
  const double sphi = std::sin(phi);
  const double cphi = std::cos(phi);
  const double rho = (a * (1.0 - es)) / std::pow(1.0 - es * sphi * sphi, 1.5);
  const double nu = a / std::sqrt(1.0 - es * sphi * sphi);
  const double adffda = a * kDf + f * kDa;

  const double expected_dphi = (-kDx * sphi * clam - kDy * sphi * slam +
                                kDz * cphi + adffda * std::sin(2.0 * phi)) /
                               rho;
  const double expected_dlam = (-kDx * slam + kDy * clam) / (nu * cphi);
  const double expected_dh = kDx * cphi * clam + kDy * cphi * slam +
                             kDz * sphi - kDa + adffda * sphi * sphi;

  const PJ_COORD in_h200 = proj_coord(lam, phi, h, 0.0);
  const PJ_COORD fwd_h200 = proj_trans(pj.get(), PJ_FWD, in_h200);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam + expected_dlam, fwd_h200.lpz.lam, 1e-15);
  EXPECT_NEAR(phi + expected_dphi, fwd_h200.lpz.phi, 1e-15);
  EXPECT_NEAR(h + expected_dh, fwd_h200.lpz.z, 1e-12);

  // 2. Unlike standard Molodensky, abridged angular and height shifts do not
  // depend on the input ellipsoidal height z.
  const PJ_COORD in_h0 = proj_coord(lam, phi, 0.0, 0.0);
  const PJ_COORD fwd_h0 = proj_trans(pj.get(), PJ_FWD, in_h0);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(fwd_h0.lpz.lam, fwd_h200.lpz.lam);
  EXPECT_DOUBLE_EQ(fwd_h0.lpz.phi, fwd_h200.lpz.phi);
  EXPECT_NEAR(fwd_h0.lpz.z + h, fwd_h200.lpz.z, 1e-12);

  // 3. Round-trip via 4D inverse.
  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd_h200);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, inv.lpz.lam, 1e-8);
  EXPECT_NEAR(phi, inv.lpz.phi, 1e-8);
  EXPECT_NEAR(h, inv.lpz.z, 1e-2);
}

TEST(MolodenskyTest, SphericalEarthRadiusOfCurvatureFastPath) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // +R=6371000 sets P->es = 0, exercising `if (es == 0) return a;` in both RN
  // and RM.
  constexpr double kRadius = 6371000.0;
  ScopedPj pj_std(proj_create(
      ctx.get(),
      "+proj=molodensky +R=6371000 +dx=10 +dy=20 +dz=30 +da=5 +df=0"));
  ASSERT_NE(nullptr, pj_std.get());
  EXPECT_DOUBLE_EQ(0.0, pj_std->es);

  ScopedPj pj_abr(proj_create(
      ctx.get(),
      "+proj=molodensky +abridged +R=6371000 +dx=10 +dy=20 +dz=30 +da=5 "
      "+df=0"));
  ASSERT_NE(nullptr, pj_abr.get());

  // At (lam=0, phi=0, z=0) on a sphere with df=0:
  // dphi = dz / R, dlam = dy / R, dh = dx - da.
  const PJ_COORD origin = proj_coord(0.0, 0.0, 0.0, 0.0);
  const PJ_COORD fwd_std = proj_trans(pj_std.get(), PJ_FWD, origin);
  const PJ_COORD fwd_abr = proj_trans(pj_abr.get(), PJ_FWD, origin);
  EXPECT_EQ(0, proj_errno(pj_std.get()));
  EXPECT_EQ(0, proj_errno(pj_abr.get()));

  EXPECT_NEAR(20.0 / kRadius, fwd_std.lpz.lam, 1e-15);
  EXPECT_NEAR(30.0 / kRadius, fwd_std.lpz.phi, 1e-15);
  EXPECT_NEAR(5.0, fwd_std.lpz.z, 1e-12);

  EXPECT_DOUBLE_EQ(fwd_std.lpz.lam, fwd_abr.lpz.lam);
  EXPECT_DOUBLE_EQ(fwd_std.lpz.phi, fwd_abr.lpz.phi);
  EXPECT_DOUBLE_EQ(fwd_std.lpz.z, fwd_abr.lpz.z);

  const PJ_COORD inv_std = proj_trans(pj_std.get(), PJ_INV, fwd_std);
  const PJ_COORD inv_abr = proj_trans(pj_abr.get(), PJ_INV, fwd_abr);
  // Reverse Molodensky evaluates dh at the shifted (lam, phi), leaving a
  // second-order residual -(dy^2 + dz^2) / R ≈ -2.04e-4 m.
  EXPECT_NEAR(0.0, inv_std.lpz.lam, 1e-10);
  EXPECT_NEAR(0.0, inv_std.lpz.phi, 1e-10);
  EXPECT_NEAR(0.0, inv_std.lpz.z, 1e-3);
  EXPECT_NEAR(0.0, inv_abr.lpz.lam, 1e-10);
  EXPECT_NEAR(0.0, inv_abr.lpz.phi, 1e-10);
  EXPECT_NEAR(0.0, inv_abr.lpz.z, 1e-3);
}

TEST(MolodenskyTest, Classic2DForwardAndReverseEntryPoints) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=molodensky +ellps=WGS84 +dx=15 +dy=-25 +dz=35 +da=-10 +df=1e-6"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP in_lp = {.lam = DegToRad(10.0), .phi = DegToRad(48.0)};
  const PJ_XY xy = pj_fwd(in_lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  // Verify 2D pj_fwd matches 3D forward at height z = 0.
  const PJ_COORD fwd3d =
      proj_trans(pj.get(), PJ_FWD, proj_coord(in_lp.lam, in_lp.phi, 0.0, 0.0));
  EXPECT_DOUBLE_EQ(fwd3d.xy.x, xy.x);
  EXPECT_DOUBLE_EQ(fwd3d.xy.y, xy.y);

  // Verify 2D pj_inv matches 3D inverse with z = 0 and approximately recovers
  // in_lp.
  const PJ_LP out_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_COORD inv3d =
      proj_trans(pj.get(), PJ_INV, proj_coord(xy.x, xy.y, 0.0, 0.0));
  EXPECT_DOUBLE_EQ(inv3d.lp.lam, out_lp.lam);
  EXPECT_DOUBLE_EQ(inv3d.lp.phi, out_lp.phi);
  EXPECT_NEAR(in_lp.lam, out_lp.lam, 1e-9);
  EXPECT_NEAR(in_lp.phi, out_lp.phi, 1e-9);
}

TEST(MolodenskyTest, SingularityZeroDenominatorReturnsOutsideProjectionDomain) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());

  // 1. Standard mode: dphi_denom == 0.0 when lpz.z == -RM(a, es, phi).
  // On a sphere of radius R = 6371000, RM = 6371000 everywhere, so z = -6371000
  // makes dphi_denom = 0.0.
  ScopedPj pj_sphere(proj_create(
      ctx.get(),
      "+proj=molodensky +R=6371000 +dx=10 +dy=20 +dz=30 +da=0 +df=0"));
  ASSERT_NE(nullptr, pj_sphere.get());

  const PJ_COORD bad_dphi = proj_coord(0.0, 0.0, -6371000.0, 0.0);
  const PJ_COORD fwd_dphi = proj_trans(pj_sphere.get(), PJ_FWD, bad_dphi);
  EXPECT_EQ(HUGE_VAL, fwd_dphi.xyz.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_sphere.get()));
  proj_errno_reset(pj_sphere.get());

  const PJ_COORD inv_dphi = proj_trans(pj_sphere.get(), PJ_INV, bad_dphi);
  EXPECT_EQ(HUGE_VAL, inv_dphi.lpz.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_sphere.get()));
  proj_errno_reset(pj_sphere.get());

  // 2. Standard mode: dlam_denom == 0.0 while dphi_denom != 0.0.
  // On an ellipsoid (es != 0) at the equator (phi = 0), RM = a * (1 - es) while
  // RN = a. Setting lpz.z = -a makes dphi_denom = -a * es != 0 and
  // dlam_denom = (a - a) * cos(0) = 0.0.
  ScopedPj pj_ellps(proj_create(
      ctx.get(),
      "+proj=molodensky +ellps=WGS84 +dx=10 +dy=20 +dz=30 +da=0 +df=0"));
  ASSERT_NE(nullptr, pj_ellps.get());

  const PJ_COORD bad_dlam = proj_coord(0.0, 0.0, -pj_ellps->a, 0.0);
  const PJ_COORD fwd_dlam = proj_trans(pj_ellps.get(), PJ_FWD, bad_dlam);
  EXPECT_EQ(HUGE_VAL, fwd_dlam.xyz.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_ellps.get()));
  proj_errno_reset(pj_ellps.get());

  const PJ_COORD inv_dlam = proj_trans(pj_ellps.get(), PJ_INV, bad_dlam);
  EXPECT_EQ(HUGE_VAL, inv_dlam.lpz.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_ellps.get()));
  proj_errno_reset(pj_ellps.get());

  // 3. Abridged mode: dlam_denom == 0.0 (`RN(P->a, P->es, phi) * cphi == 0.0`).
  // With a subnormal-borderline radius P->a = 1e-310 and es = 0 at phi = pi/2,
  // RM(a, 0, pi/2) = 1e-310 != 0 while RN * cos(pi/2) = 1e-310 * 6.12e-17
  // underflows to 0.0 in IEEE-754 double.
  ScopedPj pj_abr(proj_create(
      ctx.get(),
      "+proj=molodensky +abridged +R=6371000 +dx=0 +dy=0 +dz=0 +da=0 +df=0"));
  ASSERT_NE(nullptr, pj_abr.get());
  pj_abr->a = 1e-310;

  const PJ_COORD pole = proj_coord(0.0, kHalfPi, 0.0, 0.0);
  const PJ_COORD fwd_abr_err = proj_trans(pj_abr.get(), PJ_FWD, pole);
  EXPECT_EQ(HUGE_VAL, fwd_abr_err.xyz.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_abr.get()));
  proj_errno_reset(pj_abr.get());

  const PJ_COORD inv_abr_err = proj_trans(pj_abr.get(), PJ_INV, pole);
  EXPECT_EQ(HUGE_VAL, inv_abr_err.lpz.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_abr.get()));
  proj_errno_reset(pj_abr.get());
}

TEST(MolodenskyTest, MissingRequiredParametersFailWithMissingArgAndLogMessage) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  struct MissingArgCase {
    const char* definition;
    const char* expected_log;
  };
  constexpr MissingArgCase kCases[] = {
      {.definition = "+proj=molodensky", .expected_log = "missing dx"},
      {.definition = "+proj=molodensky +dx=1", .expected_log = "missing dy"},
      {.definition = "+proj=molodensky +dx=1 +dy=2",
       .expected_log = "missing dz"},
      {.definition = "+proj=molodensky +dx=1 +dy=2 +dz=3",
       .expected_log = "missing da"},
      {.definition = "+proj=molodensky +dx=1 +dy=2 +dz=3 +da=4",
       .expected_log = "missing df"},
  };

  for (const auto& tc : kCases) {
    capture.Clear();
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj pj(proj_create(ctx.get(), tc.definition));
    EXPECT_EQ(nullptr, pj.get()) << tc.definition;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()))
        << tc.definition;
    EXPECT_TRUE(capture.ContainsMessage(tc.expected_log)) << tc.definition;
  }
}

}  // namespace
}  // namespace proj
