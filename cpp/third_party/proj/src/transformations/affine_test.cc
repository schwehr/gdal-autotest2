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
// Unit tests for 2D/3D/4D affine and geographic offset transformations
// (+proj=affine and +proj=geogoffset) in proj/src/transformations/affine.cpp.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/affine.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kArcSecToRad = kDegToRad / 3600.0;

TEST(AffineTest, DefaultIdentityLeaves2D3DAnd4DCoordinatesUnchanged) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=affine"));
  ASSERT_NE(nullptr, pj.get());

  // 1. 4D forward and inverse via proj_trans.
  const PJ_COORD in4d = proj_coord(12.5, -34.0, 56.75, 2026.5);
  const PJ_COORD fwd4d = proj_trans(pj.get(), PJ_FWD, in4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(in4d.xyzt.x, fwd4d.xyzt.x);
  EXPECT_DOUBLE_EQ(in4d.xyzt.y, fwd4d.xyzt.y);
  EXPECT_DOUBLE_EQ(in4d.xyzt.z, fwd4d.xyzt.z);
  EXPECT_DOUBLE_EQ(in4d.xyzt.t, fwd4d.xyzt.t);

  const PJ_COORD inv4d = proj_trans(pj.get(), PJ_INV, fwd4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(in4d.xyzt.x, inv4d.xyzt.x);
  EXPECT_DOUBLE_EQ(in4d.xyzt.y, inv4d.xyzt.y);
  EXPECT_DOUBLE_EQ(in4d.xyzt.z, inv4d.xyzt.z);
  EXPECT_DOUBLE_EQ(in4d.xyzt.t, inv4d.xyzt.t);

  // 2. 3D forward and inverse entry points (forward_3d / reverse_3d).
  const PJ_LPZ in3d = {.lam = 1.0, .phi = 2.0, .z = 3.0};
  const PJ_XYZ fwd3d = pj_fwd3d(in3d, pj.get());
  EXPECT_DOUBLE_EQ(1.0, fwd3d.x);
  EXPECT_DOUBLE_EQ(2.0, fwd3d.y);
  EXPECT_DOUBLE_EQ(3.0, fwd3d.z);

  const PJ_LPZ inv3d = pj_inv3d(fwd3d, pj.get());
  EXPECT_DOUBLE_EQ(1.0, inv3d.lam);
  EXPECT_DOUBLE_EQ(2.0, inv3d.phi);
  EXPECT_DOUBLE_EQ(3.0, inv3d.z);

  // 3. 2D forward and inverse entry points (forward_2d / reverse_2d).
  const PJ_LP in2d = {.lam = 4.0, .phi = -5.0};
  const PJ_XY fwd2d = pj_fwd(in2d, pj.get());
  EXPECT_DOUBLE_EQ(4.0, fwd2d.x);
  EXPECT_DOUBLE_EQ(-5.0, fwd2d.y);

  const PJ_LP inv2d = pj_inv(fwd2d, pj.get());
  EXPECT_DOUBLE_EQ(4.0, inv2d.lam);
  EXPECT_DOUBLE_EQ(-5.0, inv2d.phi);
}

TEST(AffineTest, Full3x3MatrixOffsetsAndTimeScaleRoundTripAcrossDimensions) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=affine +xoff=10 +yoff=-20 +zoff=30 +toff=1.5 "
                          "+s11=2.0 +s12=0.5 +s13=-0.25 "
                          "+s21=-0.5 +s22=3.0 +s23=0.75 "
                          "+s31=0.25 +s32=-0.5 +s33=4.0 +tscale=2.5"));
  ASSERT_NE(nullptr, pj.get());

  // 1. 4D forward and inverse.
  const double x = 4.0;
  const double y = -2.0;
  const double z = 8.0;
  const double t = 10.0;
  const PJ_COORD fwd4d = proj_trans(pj.get(), PJ_FWD, proj_coord(x, y, z, t));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(10.0 + 2.0 * x + 0.5 * y - 0.25 * z, fwd4d.xyzt.x, 1e-12);
  EXPECT_NEAR(-20.0 - 0.5 * x + 3.0 * y + 0.75 * z, fwd4d.xyzt.y, 1e-12);
  EXPECT_NEAR(30.0 + 0.25 * x - 0.5 * y + 4.0 * z, fwd4d.xyzt.z, 1e-12);
  EXPECT_NEAR(1.5 + 2.5 * t, fwd4d.xyzt.t, 1e-12);

  const PJ_COORD inv4d = proj_trans(pj.get(), PJ_INV, fwd4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(x, inv4d.xyzt.x, 1e-12);
  EXPECT_NEAR(y, inv4d.xyzt.y, 1e-12);
  EXPECT_NEAR(z, inv4d.xyzt.z, 1e-12);
  EXPECT_NEAR(t, inv4d.xyzt.t, 1e-12);

  // 2. 3D forward and inverse via pj_fwd3d / pj_inv3d.
  const PJ_LPZ in3d = {.lam = x, .phi = y, .z = z};
  const PJ_XYZ fwd3d = pj_fwd3d(in3d, pj.get());
  EXPECT_NEAR(fwd4d.xyz.x, fwd3d.x, 1e-12);
  EXPECT_NEAR(fwd4d.xyz.y, fwd3d.y, 1e-12);
  EXPECT_NEAR(fwd4d.xyz.z, fwd3d.z, 1e-12);

  const PJ_LPZ inv3d = pj_inv3d(fwd3d, pj.get());
  EXPECT_NEAR(x, inv3d.lam, 1e-12);
  EXPECT_NEAR(y, inv3d.phi, 1e-12);
  EXPECT_NEAR(z, inv3d.z, 1e-12);

  // 3. Pure 2D affine (zoff=0, s13=s23=s31=s32=0) via pj_fwd / pj_inv.
  ScopedPj pj2d(proj_create(
      ctx.get(), "+proj=affine +xoff=5 +yoff=-3 +s11=2 +s12=1 +s21=-1 +s22=3"));
  ASSERT_NE(nullptr, pj2d.get());
  const PJ_LP in2d = {.lam = 3.0, .phi = 4.0};
  const PJ_XY fwd2d = pj_fwd(in2d, pj2d.get());
  EXPECT_NEAR(5.0 + 2.0 * 3.0 + 1.0 * 4.0, fwd2d.x, 1e-12);
  EXPECT_NEAR(-3.0 - 1.0 * 3.0 + 3.0 * 4.0, fwd2d.y, 1e-12);

  const PJ_LP inv2d = pj_inv(fwd2d, pj2d.get());
  EXPECT_NEAR(in2d.lam, inv2d.lam, 1e-12);
  EXPECT_NEAR(in2d.phi, inv2d.phi, 1e-12);
}

TEST(AffineTest, SingularMatrixAndZeroTimeScaleDisableInverseAndLogDebug) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Singular spatial matrix (det == 0.0) with PJ_LOG_DEBUG logs
  //    "matrix non invertible" and clears inv/inv3d/inv4d.
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_DEBUG);
    ScopedPj pj_singular(proj_create(ctx.get(), "+proj=affine +s11=0"));
    ASSERT_NE(nullptr, pj_singular.get());
    EXPECT_TRUE(logs.ContainsMessage("matrix non invertible"));
    EXPECT_EQ(nullptr, pj_singular->inv);
    EXPECT_EQ(nullptr, pj_singular->inv3d);
    EXPECT_EQ(nullptr, pj_singular->inv4d);

    const PJ_COORD fwd =
        proj_trans(pj_singular.get(), PJ_FWD, proj_coord(5.0, 6.0, 7.0, 8.0));
    EXPECT_EQ(0, proj_errno(pj_singular.get()));
    EXPECT_DOUBLE_EQ(0.0, fwd.xyzt.x);
    EXPECT_DOUBLE_EQ(6.0, fwd.xyzt.y);

    const PJ_COORD inv = proj_trans(pj_singular.get(), PJ_INV, fwd);
    EXPECT_TRUE(std::isinf(inv.xyzt.x));
    EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(pj_singular.get()));
  }

  // 2. Non-singular spatial matrix with +tscale=0 and PJ_LOG_NONE also disables
  //    inverse without logging.
  {
    ScopedLogSuppressor silence(ctx.get());
    ScopedPj pj_zero_t(
        proj_create(ctx.get(), "+proj=affine +toff=2000 +tscale=0"));
    ASSERT_NE(nullptr, pj_zero_t.get());
    EXPECT_EQ(nullptr, pj_zero_t->inv);
    EXPECT_EQ(nullptr, pj_zero_t->inv3d);
    EXPECT_EQ(nullptr, pj_zero_t->inv4d);

    const PJ_COORD fwd =
        proj_trans(pj_zero_t.get(), PJ_FWD, proj_coord(1.0, 2.0, 3.0, 1995.0));
    EXPECT_EQ(0, proj_errno(pj_zero_t.get()));
    EXPECT_DOUBLE_EQ(2000.0, fwd.xyzt.t);

    const PJ_COORD inv = proj_trans(pj_zero_t.get(), PJ_INV, fwd);
    EXPECT_TRUE(std::isinf(inv.xyzt.x));
    EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(pj_zero_t.get()));
  }
}

TEST(AffineTest, GeogoffsetAppliesArcsecondAngularAndMeterVerticalOffsets) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(
      proj_create(ctx.get(), "+proj=geogoffset +dlon=36 +dlat=-72 +dh=15.25"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj->left);
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj->right);

  const double lon0 = 10.0 * kDegToRad;
  const double lat0 = 50.0 * kDegToRad;
  const double h0 = 100.0;
  const double t0 = 2025.0;

  // 1. Forward 4D adds dlon (in rad), dlat (in rad), and dh (in m).
  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lon0, lat0, h0, t0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lon0 + 36.0 * kArcSecToRad, fwd.lpzt.lam, 1e-14);
  EXPECT_NEAR(lat0 - 72.0 * kArcSecToRad, fwd.lpzt.phi, 1e-14);
  EXPECT_NEAR(h0 + 15.25, fwd.lpzt.z, 1e-12);
  EXPECT_DOUBLE_EQ(t0, fwd.lpzt.t);

  // 2. Inverse 4D round-trips back to the original geodetic coordinate.
  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lon0, inv.lpzt.lam, 1e-14);
  EXPECT_NEAR(lat0, inv.lpzt.phi, 1e-14);
  EXPECT_NEAR(h0, inv.lpzt.z, 1e-12);
  EXPECT_DOUBLE_EQ(t0, inv.lpzt.t);

  // 3. 2D entry points (pj_fwd / pj_inv) shift (lam, phi) and round-trip.
  const PJ_LP lp_in = {.lam = lon0, .phi = lat0};
  const PJ_XY xy_fwd = pj_fwd(lp_in, pj.get());
  EXPECT_NEAR(lon0 + 36.0 * kArcSecToRad, xy_fwd.x, 1e-14);
  EXPECT_NEAR(lat0 - 72.0 * kArcSecToRad, xy_fwd.y, 1e-14);

  const PJ_LP lp_inv = pj_inv(xy_fwd, pj.get());
  EXPECT_NEAR(lon0, lp_inv.lam, 1e-14);
  EXPECT_NEAR(lat0, lp_inv.phi, 1e-14);
}

}  // namespace
}  // namespace proj
