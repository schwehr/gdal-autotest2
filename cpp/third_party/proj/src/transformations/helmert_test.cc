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
// Unit tests for 3(6)-, 4(8)-, and 7(14)-parameter Helmert and
// Molodensky-Badekas transformations (proj/src/transformations/helmert.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/helmert.cpp

#include <cmath>
#include <numbers>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kArcsecToRad = (kPi / 180.0) / 3600.0;

TEST(HelmertTest, ThreeParameterTranslationOnlyFastPathRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  // With no rotations and s=0, Q->no_rotation == 1 and Q->scale == 0, which
  // bypasses convention requirements and uses the translation-only fast path.
  ScopedPj pj(
      proj_create(ctx.get(), "+proj=helmert +x=10.5 +y=-20.25 +z=30.75"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Forward 4D transformation via proj_trans.
  const PJ_COORD in = proj_coord(1000.0, 2000.0, 3000.0, 0.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1010.5, fwd.xyz.x, 1e-12);
  EXPECT_NEAR(1979.75, fwd.xyz.y, 1e-12);
  EXPECT_NEAR(3030.75, fwd.xyz.z, 1e-12);

  // 2. Inverse 4D transformation via proj_trans.
  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.xyz.x, inv.xyz.x, 1e-12);
  EXPECT_NEAR(in.xyz.y, inv.xyz.y, 1e-12);
  EXPECT_NEAR(in.xyz.z, inv.xyz.z, 1e-12);

  // 3. Direct 3D entry points pj_fwd3d and pj_inv3d.
  const PJ_XYZ fwd3d = pj_fwd3d(in.lpz, pj.get());
  EXPECT_NEAR(1010.5, fwd3d.x, 1e-12);
  EXPECT_NEAR(1979.75, fwd3d.y, 1e-12);
  EXPECT_NEAR(3030.75, fwd3d.z, 1e-12);

  const PJ_LPZ inv3d = pj_inv3d(fwd3d, pj.get());
  EXPECT_NEAR(in.lpz.lam, inv3d.lam, 1e-12);
  EXPECT_NEAR(in.lpz.phi, inv3d.phi, 1e-12);
  EXPECT_NEAR(in.lpz.z, inv3d.z, 1e-12);
}

TEST(HelmertTest, ScaleWithoutRotationUsesMatrixPathWithoutConvention) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  // Q->no_rotation == 1, but Q->scale != 0 skips the translation-only fast path
  // and applies (1 + s * 1e-6) with the identity rotation matrix.
  ScopedPj pj(proj_create(ctx.get(), "+proj=helmert +x=10 +y=20 +z=30 +s=100"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_COORD in = proj_coord(10000.0, -20000.0, 30000.0, 0.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  constexpr double kScale = 1.0 + 100.0 * 1e-6;
  EXPECT_NEAR(10000.0 * kScale + 10.0, fwd.xyz.x, 1e-9);
  EXPECT_NEAR(-20000.0 * kScale + 20.0, fwd.xyz.y, 1e-9);
  EXPECT_NEAR(30000.0 * kScale + 30.0, fwd.xyz.z, 1e-9);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.xyz.x, inv.xyz.x, 1e-9);
  EXPECT_NEAR(in.xyz.y, inv.xyz.y, 1e-9);
  EXPECT_NEAR(in.xyz.z, inv.xyz.z, 1e-9);
}

TEST(HelmertTest, SevenParameterConventionsAndExactVsApproximateRotation) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Small-angle approximate rotation with position_vector vs
  // coordinate_frame: negating rotation angles flips between conventions.
  ScopedPj pj_pos_approx(proj_create(
      ctx.get(),
      "+proj=helmert +x=100 +y=-50 +z=25 +rx=1.2 +ry=-2.4 +rz=3.6 +s=1.5 "
      "+convention=position_vector"));
  ASSERT_NE(nullptr, pj_pos_approx.get());

  ScopedPj pj_cf_approx(proj_create(
      ctx.get(),
      "+proj=helmert +x=100 +y=-50 +z=25 +rx=-1.2 +ry=2.4 +rz=-3.6 +s=1.5 "
      "+convention=coordinate_frame"));
  ASSERT_NE(nullptr, pj_cf_approx.get());

  const PJ_COORD pt = proj_coord(3500000.0, 1200000.0, 5000000.0, 0.0);
  const PJ_COORD fwd_pos = proj_trans(pj_pos_approx.get(), PJ_FWD, pt);
  const PJ_COORD fwd_cf = proj_trans(pj_cf_approx.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(pj_pos_approx.get()));
  EXPECT_EQ(0, proj_errno(pj_cf_approx.get()));
  EXPECT_NEAR(fwd_pos.xyz.x, fwd_cf.xyz.x, 1e-9);
  EXPECT_NEAR(fwd_pos.xyz.y, fwd_cf.xyz.y, 1e-9);
  EXPECT_NEAR(fwd_pos.xyz.z, fwd_cf.xyz.z, 1e-9);

  // Verify approximate rotation formula for coordinate_frame explicitly.
  ScopedPj pj_cf_pos_angles(proj_create(
      ctx.get(),
      "+proj=helmert +x=100 +y=-50 +z=25 +rx=1.2 +ry=-2.4 +rz=3.6 +s=1.5 "
      "+convention=coordinate_frame"));
  ASSERT_NE(nullptr, pj_cf_pos_angles.get());
  const PJ_COORD fwd_cf_pos = proj_trans(pj_cf_pos_angles.get(), PJ_FWD, pt);
  const double f = 1.2 * kArcsecToRad;
  const double t = -2.4 * kArcsecToRad;
  const double p = 3.6 * kArcsecToRad;
  const double scale = 1.0 + 1.5e-6;
  EXPECT_NEAR(scale * (pt.xyz.x + p * pt.xyz.y - t * pt.xyz.z) + 100.0,
              fwd_cf_pos.xyz.x, 1e-8);
  EXPECT_NEAR(scale * (-p * pt.xyz.x + pt.xyz.y + f * pt.xyz.z) - 50.0,
              fwd_cf_pos.xyz.y, 1e-8);
  EXPECT_NEAR(scale * (t * pt.xyz.x - f * pt.xyz.y + pt.xyz.z) + 25.0,
              fwd_cf_pos.xyz.z, 1e-8);

  // 2. Exact rotation matrix (+exact) is strictly orthogonal, so 3D inverse via
  // matrix transpose round-trips to machine precision.
  ScopedPj pj_exact_pos(proj_create(
      ctx.get(),
      "+proj=helmert +x=100 +y=-50 +z=25 +rx=120 +ry=-80 +rz=45 +s=2.0 "
      "+exact +convention=position_vector"));
  ASSERT_NE(nullptr, pj_exact_pos.get());

  const PJ_COORD fwd_exact = proj_trans(pj_exact_pos.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(pj_exact_pos.get()));
  const PJ_COORD inv_exact = proj_trans(pj_exact_pos.get(), PJ_INV, fwd_exact);
  EXPECT_EQ(0, proj_errno(pj_exact_pos.get()));
  EXPECT_NEAR(pt.xyz.x, inv_exact.xyz.x, 1e-7);
  EXPECT_NEAR(pt.xyz.y, inv_exact.xyz.y, 1e-7);
  EXPECT_NEAR(pt.xyz.z, inv_exact.xyz.z, 1e-7);

  ScopedPj pj_exact_cf(proj_create(
      ctx.get(),
      "+proj=helmert +x=100 +y=-50 +z=25 +rx=120 +ry=-80 +rz=45 +s=2.0 "
      "+exact +convention=coordinate_frame"));
  ASSERT_NE(nullptr, pj_exact_cf.get());
  const PJ_COORD fwd_exact_cf = proj_trans(pj_exact_cf.get(), PJ_FWD, pt);
  const PJ_COORD inv_exact_cf =
      proj_trans(pj_exact_cf.get(), PJ_INV, fwd_exact_cf);
  EXPECT_NEAR(pt.xyz.x, inv_exact_cf.xyz.x, 1e-7);
  EXPECT_NEAR(pt.xyz.y, inv_exact_cf.xyz.y, 1e-7);
  EXPECT_NEAR(pt.xyz.z, inv_exact_cf.xyz.z, 1e-7);
}

TEST(HelmertTest, Towgs84ThreeAndSevenParameterExpansion) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. 3-parameter towgs84 leaves datum_params[6] == 0 -> Q->scale_0 = 0.
  //    Set skip_fwd_prepare = 1 so fwd_prepare does not apply the cs2cs
  //    emulation P->helmert inverse shift before helmert_forward_4d runs.
  ScopedPj pj3(proj_create(ctx.get(), "+proj=helmert +towgs84=15,-25,35"));
  ASSERT_NE(nullptr, pj3.get());
  pj3->skip_fwd_prepare = 1;
  const PJ_COORD in = proj_coord(100.0, 200.0, 300.0, 0.0);
  const PJ_COORD fwd3 = proj_trans(pj3.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj3.get()));
  EXPECT_NEAR(115.0, fwd3.xyz.x, 1e-12);
  EXPECT_NEAR(175.0, fwd3.xyz.y, 1e-12);
  EXPECT_NEAR(335.0, fwd3.xyz.z, 1e-12);

  // 2. 7-parameter towgs84 converts datum_params[6] back to ppm scale_0.
  ScopedPj pj7(proj_create(
      ctx.get(),
      "+proj=helmert +towgs84=10,20,30,1,2,3,4 +convention=position_vector"));
  ASSERT_NE(nullptr, pj7.get());
  pj7->skip_fwd_prepare = 1;
  ScopedPj pj_equiv(
      proj_create(ctx.get(),
                  "+proj=helmert +x=10 +y=20 +z=30 +rx=1 +ry=2 +rz=3 +s=4 "
                  "+convention=position_vector"));
  ASSERT_NE(nullptr, pj_equiv.get());

  const PJ_COORD fwd7 = proj_trans(pj7.get(), PJ_FWD, in);
  const PJ_COORD fwd_eq = proj_trans(pj_equiv.get(), PJ_FWD, in);
  EXPECT_NEAR(fwd_eq.xyz.x, fwd7.xyz.x, 1e-11);
  EXPECT_NEAR(fwd_eq.xyz.y, fwd7.xyz.y, 1e-11);
  EXPECT_NEAR(fwd_eq.xyz.z, fwd7.xyz.z, 1e-11);
}

TEST(HelmertTest, FourParameter2DAndKinematicEightParameterTransform) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. 4-parameter 2D Helmert with default s=1.0 when +s is omitted, and
  // theta = 324000 arcsec = 90 degrees (pi/2 rad).
  ScopedPj pj_default_s(
      proj_create(ctx.get(), "+proj=helmert +x=10 +y=20 +theta=324000"));
  ASSERT_NE(nullptr, pj_default_s.get());

  // Verify 2D pj_fwd and pj_inv entry points:
  // cr = cos(pi/2)*1 = 0, sr = sin(pi/2)*1 = 1
  // x' = sr * y + x0 = y + 10; y' = -sr * x + y0 = -x + 20.
  const PJ_LP lp_in = {.lam = 50.0, .phi = 80.0};
  const PJ_XY xy_out = pj_fwd(lp_in, pj_default_s.get());
  EXPECT_EQ(0, proj_errno(pj_default_s.get()));
  EXPECT_NEAR(90.0, xy_out.x, 1e-9);
  EXPECT_NEAR(-30.0, xy_out.y, 1e-9);

  const PJ_LP lp_back = pj_inv(xy_out, pj_default_s.get());
  EXPECT_EQ(0, proj_errno(pj_default_s.get()));
  EXPECT_NEAR(lp_in.lam, lp_back.lam, 1e-9);
  EXPECT_NEAR(lp_in.phi, lp_back.phi, 1e-9);

  // 2. 4-parameter / 8-parameter kinematic mode via 4D proj_trans (exercises
  // Q->fourparam branch in helmert_forward_3d and helmert_reverse_3d,
  // preserving z while evolving theta and s with time).
  ScopedPj pj_kin4(proj_create(
      ctx.get(),
      "+proj=helmert +x=10 +y=20 +theta=0 +dtheta=32400 +s=2.0 +ds=0.5 "
      "+t_epoch=2000.0"));
  ASSERT_NE(nullptr, pj_kin4.get());

  // At t = 2010.0 (dt = 10), theta = 324000 arcsec = 90 deg, scale = 2.0 + 5.0
  // = 7.0.
  const PJ_COORD c_in = proj_coord(5.0, 8.0, 42.0, 2010.0);
  const PJ_COORD c_fwd = proj_trans(pj_kin4.get(), PJ_FWD, c_in);
  EXPECT_EQ(0, proj_errno(pj_kin4.get()));
  EXPECT_NEAR(7.0 * 8.0 + 10.0, c_fwd.xyz.x, 1e-8);
  EXPECT_NEAR(-7.0 * 5.0 + 20.0, c_fwd.xyz.y, 1e-8);
  EXPECT_NEAR(42.0, c_fwd.xyz.z, 1e-12);

  const PJ_COORD c_inv = proj_trans(pj_kin4.get(), PJ_INV, c_fwd);
  EXPECT_EQ(0, proj_errno(pj_kin4.get()));
  EXPECT_NEAR(c_in.xyz.x, c_inv.xyz.x, 1e-9);
  EXPECT_NEAR(c_in.xyz.y, c_inv.xyz.y, 1e-9);
  EXPECT_NEAR(c_in.xyz.z, c_inv.xyz.z, 1e-12);
}

TEST(HelmertTest, FourteenParameterKinematicTimeEvolutionAndEpochFallback) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_kin(proj_create(
      ctx.get(),
      "+proj=helmert +x=10 +y=20 +z=30 +rx=1 +ry=2 +rz=3 +s=1.0 "
      "+dx=2.0 +dy=-1.0 +dz=0.5 +drx=0.1 +dry=0.2 +drz=0.3 +ds=0.25 "
      "+t_epoch=2000.0 +exact +convention=position_vector"));
  ASSERT_NE(nullptr, pj_kin.get());

  // Static Helmert equivalent at t = 2010.0 (dt = 10):
  // x=30, y=10, z=35, rx=2, ry=4, rz=6, s=3.5.
  ScopedPj pj_static_2010(
      proj_create(ctx.get(),
                  "+proj=helmert +x=30 +y=10 +z=35 +rx=2 +ry=4 +rz=6 +s=3.5 "
                  "+exact +convention=position_vector"));
  ASSERT_NE(nullptr, pj_static_2010.get());

  // Static Helmert equivalent at t_epoch = 2000.0 (dt = 0):
  ScopedPj pj_static_2000(
      proj_create(ctx.get(),
                  "+proj=helmert +x=10 +y=20 +z=30 +rx=1 +ry=2 +rz=3 +s=1.0 "
                  "+exact +convention=position_vector"));
  ASSERT_NE(nullptr, pj_static_2000.get());

  // 1. Transform at t = 2010.0 (triggers t_obs != Q->t_obs rebuild).
  const PJ_COORD pt_2010 = proj_coord(4000000.0, 1000000.0, 4800000.0, 2010.0);
  const PJ_COORD fwd_2010 = proj_trans(pj_kin.get(), PJ_FWD, pt_2010);
  const PJ_COORD expected_2010 =
      proj_trans(pj_static_2010.get(), PJ_FWD, pt_2010);
  EXPECT_NEAR(expected_2010.xyz.x, fwd_2010.xyz.x, 1e-8);
  EXPECT_NEAR(expected_2010.xyz.y, fwd_2010.xyz.y, 1e-8);
  EXPECT_NEAR(expected_2010.xyz.z, fwd_2010.xyz.z, 1e-8);

  // 2. Second call at same t = 2010.0 exercises cached rotation matrix branch
  // (t_obs == Q->t_obs).
  const PJ_COORD fwd_2010_cached = proj_trans(pj_kin.get(), PJ_FWD, pt_2010);
  EXPECT_NEAR(fwd_2010.xyz.x, fwd_2010_cached.xyz.x, 1e-12);

  // 3. Transform with t = HUGE_VAL falls back to Q->t_epoch (2000.0).
  const PJ_COORD pt_huge =
      proj_coord(4000000.0, 1000000.0, 4800000.0, HUGE_VAL);
  const PJ_COORD fwd_huge = proj_trans(pj_kin.get(), PJ_FWD, pt_huge);
  const PJ_COORD expected_2000 =
      proj_trans(pj_static_2000.get(), PJ_FWD, pt_huge);
  EXPECT_NEAR(expected_2000.xyz.x, fwd_huge.xyz.x, 1e-8);
  EXPECT_NEAR(expected_2000.xyz.y, fwd_huge.xyz.y, 1e-8);
  EXPECT_NEAR(expected_2000.xyz.z, fwd_huge.xyz.z, 1e-8);

  // 4. Inverse at t = 2010.0 (triggers t_obs != Q->t_obs in helmert_reverse_4d)
  // and a second inverse at t = 2010.0 (cached) followed by t = HUGE_VAL.
  const PJ_COORD inv_2010 = proj_trans(pj_kin.get(), PJ_INV, fwd_2010);
  EXPECT_NEAR(pt_2010.xyz.x, inv_2010.xyz.x, 1e-7);
  EXPECT_NEAR(pt_2010.xyz.y, inv_2010.xyz.y, 1e-7);
  EXPECT_NEAR(pt_2010.xyz.z, inv_2010.xyz.z, 1e-7);

  const PJ_COORD inv_2010_cached = proj_trans(pj_kin.get(), PJ_INV, fwd_2010);
  EXPECT_NEAR(inv_2010.xyz.x, inv_2010_cached.xyz.x, 1e-12);

  PJ_COORD fwd_huge_with_t = fwd_huge;
  fwd_huge_with_t.xyzt.t = HUGE_VAL;
  const PJ_COORD inv_huge = proj_trans(pj_kin.get(), PJ_INV, fwd_huge_with_t);
  EXPECT_NEAR(pt_huge.xyz.x, inv_huge.xyz.x, 1e-7);
  EXPECT_NEAR(pt_huge.xyz.y, inv_huge.xyz.y, 1e-7);
  EXPECT_NEAR(pt_huge.xyz.z, inv_huge.xyz.z, 1e-7);
}

TEST(HelmertTest, MolodenskyBadekasReferencePointAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=molobadekas +x=12.5 +y=-8.25 +z=4.75 +rx=10 +ry=-20 +rz=30 +s=5.0 "
      "+px=3000000 +py=1000000 +pz=5000000 +exact "
      "+convention=position_vector"));
  ASSERT_NE(nullptr, pj.get());

  // 1. At the exact reference point (px, py, pz), X = Y = Z = 0, so rotation
  // and scale contribute 0 and only the translation (x, y, z) is added.
  const PJ_COORD ref_pt = proj_coord(3000000.0, 1000000.0, 5000000.0, 0.0);
  const PJ_COORD fwd_ref = proj_trans(pj.get(), PJ_FWD, ref_pt);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(3000012.5, fwd_ref.xyz.x, 1e-9);
  EXPECT_NEAR(999991.75, fwd_ref.xyz.y, 1e-9);
  EXPECT_NEAR(5000004.75, fwd_ref.xyz.z, 1e-9);

  // 2. Off-reference point round-trips through 3D forward and reverse.
  const PJ_COORD off_pt = proj_coord(3010000.0, 1020000.0, 4990000.0, 0.0);
  const PJ_COORD fwd_off = proj_trans(pj.get(), PJ_FWD, off_pt);
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_COORD inv_off = proj_trans(pj.get(), PJ_INV, fwd_off);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(off_pt.xyz.x, inv_off.xyz.x, 1e-7);
  EXPECT_NEAR(off_pt.xyz.y, inv_off.xyz.y, 1e-7);
  EXPECT_NEAR(off_pt.xyz.z, inv_off.xyz.z, 1e-7);
}

TEST(HelmertTest, ParameterValidationErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());

  struct ErrorCase {
    const char* definition;
    int expected_err;
  };
  constexpr ErrorCase kCases[] = {
      // Rotational term without +convention=...
      {.definition = "+proj=helmert +rx=1.0",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // Rotational rate term without +convention=...
      {.definition = "+proj=helmert +drx=0.5",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // Invalid +convention value.
      {.definition = "+proj=helmert +rx=1.0 +convention=unknown_convention",
       .expected_err = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
      // +towgs84 combined with +convention=coordinate_frame is rejected.
      {.definition =
           "+proj=helmert +towgs84=1,2,3,4,5,6,7 +convention=coordinate_frame",
       .expected_err = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
      // Obsolete +transpose parameter is rejected.
      {.definition = "+proj=helmert +x=1 +transpose",
       .expected_err = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
      // Scale <= -1.0e6 makes (1 + s*1e-6) <= 0 and is rejected.
      {.definition = "+proj=helmert +x=1 +s=-1000000",
       .expected_err = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
      // 4-parameter +theta with +s=0 is rejected (division by zero in reverse).
      {.definition = "+proj=helmert +x=1 +theta=10 +s=0",
       .expected_err = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
      // Molodensky-Badekas always requires +convention=...
      {.definition = "+proj=molobadekas +x=1",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      {.definition = "+proj=molobadekas +x=1 +convention=invalid",
       .expected_err = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
  };

  for (const auto& tc : kCases) {
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj pj(proj_create(ctx.get(), tc.definition));
    EXPECT_EQ(nullptr, pj.get()) << tc.definition;
    EXPECT_EQ(tc.expected_err, proj_context_errno(ctx.get())) << tc.definition;
  }
}

TEST(HelmertTest, TraceLoggingCoversAllHelmertAndMolobadekasBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_TRACE);

  // 1. Helmert without rotation (no_rotation == 1).
  ScopedPj pj_no_rot(proj_create(ctx.get(), "+proj=helmert +x=1 +y=2 +z=3"));
  ASSERT_NE(nullptr, pj_no_rot.get());
  EXPECT_TRUE(capture.ContainsMessage("Helmert parameters:"));
  EXPECT_TRUE(capture.ContainsMessage("Rotation Matrix:"));

  // 2. Helmert with position_vector and time update.
  capture.Clear();
  ScopedPj pj_pos(proj_create(ctx.get(),
                              "+proj=helmert +x=1 +rx=1 +drx=0.1 +t_epoch=2000 "
                              "+convention=position_vector"));
  ASSERT_NE(nullptr, pj_pos.get());
  EXPECT_TRUE(capture.ContainsMessage("convention=position_vector"));
  const PJ_COORD c = proj_coord(100.0, 200.0, 300.0, 2005.0);
  (void)proj_trans(pj_pos.get(), PJ_FWD, c);
  EXPECT_TRUE(capture.ContainsMessage(
      "Transformation parameters for observation t_obs=2005"));

  // 3. Helmert with coordinate_frame.
  capture.Clear();
  ScopedPj pj_cf(proj_create(
      ctx.get(), "+proj=helmert +x=1 +rx=1 +convention=coordinate_frame"));
  ASSERT_NE(nullptr, pj_cf.get());
  EXPECT_TRUE(capture.ContainsMessage("convention=coordinate_frame"));

  // 4. Molodensky-Badekas with both position_vector and coordinate_frame.
  capture.Clear();
  ScopedPj mb_pos(proj_create(
      ctx.get(),
      "+proj=molobadekas +x=1 +rx=1 +px=10 +convention=position_vector"));
  ASSERT_NE(nullptr, mb_pos.get());
  EXPECT_TRUE(capture.ContainsMessage("Molodensky-Badekas parameters:"));
  EXPECT_TRUE(capture.ContainsMessage("convention=position_vector"));

  capture.Clear();
  ScopedPj mb_cf(proj_create(
      ctx.get(),
      "+proj=molobadekas +x=1 +rx=1 +px=10 +convention=coordinate_frame"));
  ASSERT_NE(nullptr, mb_cf.get());
  EXPECT_TRUE(capture.ContainsMessage("convention=coordinate_frame"));
}

}  // namespace
}  // namespace proj
