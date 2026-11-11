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
// Unit tests for PROJ pipeline orchestration and the push/pop stack operators
// (proj/src/pipeline.cpp). Tests use pj_create_internal() alongside
// proj_create()/proj_create_argv() where needed because PROJStringParser
// intercepts several malformed syntax forms and overrides PJ_LOG_TRACE prior
// to invoking pipeline initialization.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/pipeline.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(PipelineTest, DatumShiftCreatesViaStringAndArgvAndTransforms4D) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Construct a 3-step datum-shift pipeline via proj_create.
  ScopedPj pipe(proj_create(ctx.get(),
                            "+proj=pipeline +ellps=WGS84 +step +proj=cart "
                            "+step +proj=helmert +x=100 +y=-50 +z=25 "
                            "+step +proj=cart +inv"));
  ASSERT_NE(nullptr, pipe.get());
  EXPECT_STREQ("pipeline", proj_pj_info(pipe.get()).id);
  EXPECT_EQ(1, proj_pj_info(pipe.get()).has_inverse);

  // 2. Construct the same pipeline via proj_create_argv using mutable buffers.
  char arg0[] = "+proj=pipeline";
  char arg1[] = "+ellps=WGS84";
  char arg2[] = "+step";
  char arg3[] = "+proj=cart";
  char arg4[] = "+step";
  char arg5[] = "+proj=helmert";
  char arg6[] = "+x=100";
  char arg7[] = "+y=-50";
  char arg8[] = "+z=25";
  char arg9[] = "+step";
  char arg10[] = "+proj=cart";
  char arg11[] = "+inv";
  std::array<char*, 12> argv = {arg0, arg1, arg2, arg3, arg4,  arg5,
                                arg6, arg7, arg8, arg9, arg10, arg11};
  ScopedPj pipe_argv(
      proj_create_argv(ctx.get(), static_cast<int>(argv.size()), argv.data()));
  ASSERT_NE(nullptr, pipe_argv.get());

  // 3. Verify round-trip via proj_trans / proj_roundtrip in radians (lon, lat).
  PJ_COORD pt = proj_coord(DegToRad(12.5), DegToRad(55.7), 42.0, 2020.0);
  const PJ_COORD shifted = proj_trans(pipe.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(pipe.get()));
  EXPECT_NE(pt.lpz.phi, shifted.lpz.phi);

  const double roundtrip_err = proj_roundtrip(pipe_argv.get(), PJ_FWD, 2, &pt);
  EXPECT_NEAR(0.0, roundtrip_err, 1e-7);

  // 4. Batch-transform via proj_trans_array.
  std::array<PJ_COORD, 2> coords = {
      proj_coord(DegToRad(10.0), DegToRad(48.0), 100.0, 0.0),
      proj_coord(DegToRad(-73.9), DegToRad(40.7), 15.0, 0.0),
  };
  const std::array<PJ_COORD, 2> original = coords;
  ASSERT_EQ(0,
            proj_trans_array(pipe.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pipe.get(), PJ_INV, coords.size(), coords.data()));
  for (size_t i = 0; i < coords.size(); ++i) {
    EXPECT_NEAR(original[i].lpz.lam, coords[i].lpz.lam, 1e-11);
    EXPECT_NEAR(original[i].lpz.phi, coords[i].lpz.phi, 1e-11);
    EXPECT_NEAR(original[i].lpz.z, coords[i].lpz.z, 1e-6);
  }

  // 5. Transform separate strided coordinate buffers via proj_trans_generic.
  std::array<double, 2> xs = {DegToRad(2.3), DegToRad(13.4)};
  std::array<double, 2> ys = {DegToRad(48.8), DegToRad(52.5)};
  std::array<double, 2> zs = {50.0, 75.0};
  std::array<double, 2> ts = {2015.0, 2022.0};
  ASSERT_EQ(2u,
            proj_trans_generic(pipe.get(), PJ_FWD, xs.data(), sizeof(double),
                               xs.size(), ys.data(), sizeof(double), ys.size(),
                               zs.data(), sizeof(double), zs.size(), ts.data(),
                               sizeof(double), ts.size()));
  EXPECT_EQ(0, proj_errno(pipe.get()));
  ASSERT_EQ(2u,
            proj_trans_generic(pipe.get(), PJ_INV, xs.data(), sizeof(double),
                               xs.size(), ys.data(), sizeof(double), ys.size(),
                               zs.data(), sizeof(double), zs.size(), ts.data(),
                               sizeof(double), ts.size()));
  EXPECT_EQ(0, proj_errno(pipe.get()));
  EXPECT_NEAR(DegToRad(2.3), xs[0], 1e-11);
  EXPECT_NEAR(DegToRad(52.5), ys[1], 1e-11);
  EXPECT_NEAR(50.0, zs[0], 1e-6);
}

TEST(PipelineTest, FourDimThreeDimTwoDimOmitInvertedAndEarlyBreak) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Construct a 4-step pipeline (>=4 steps exercises Step(Step&&) vector
  // reallocation) with +omit_inv, +omit_fwd, and +inv steps.
  ScopedPj affine_pipe(proj_create(
      ctx.get(),
      "+proj=pipeline "
      "+step +proj=affine +xoff=10 +yoff=20 +zoff=30 +toff=4 +omit_inv "
      "+step +proj=affine +xoff=100 +yoff=200 +zoff=300 +toff=400 +omit_fwd "
      "+step +proj=affine +xoff=1 +yoff=2 +zoff=3 +toff=0.5 +inv "
      "+step +proj=affine +xoff=5 +yoff=7 +zoff=9 +toff=2"));
  ASSERT_NE(nullptr, affine_pipe.get());

  // Forward skips step 1 (+omit_fwd) and inverts step 2 (+inv):
  // delta = (+10 - 1 + 5, +20 - 2 + 7, +30 - 3 + 9, +4 - 0.5 + 2)
  //       = (+14, +25, +36, +5.5).
  const PJ_COORD fwd4d =
      proj_trans(affine_pipe.get(), PJ_FWD, proj_coord(1.0, 2.0, 3.0, 4.0));
  EXPECT_EQ(0, proj_errno(affine_pipe.get()));
  EXPECT_NEAR(15.0, fwd4d.xyzt.x, 1e-12);
  EXPECT_NEAR(27.0, fwd4d.xyzt.y, 1e-12);
  EXPECT_NEAR(39.0, fwd4d.xyzt.z, 1e-12);
  EXPECT_NEAR(9.5, fwd4d.xyzt.t, 1e-12);

  // Inverse runs steps 3->2(+inv runs fwd)->1->0(skipped via +omit_inv):
  // delta = (-5 + 1 - 100, -7 + 2 - 200, -9 + 3 - 300, -2 + 0.5 - 400)
  //       = (-104, -205, -306, -401.5).
  const PJ_COORD inv4d =
      proj_trans(affine_pipe.get(), PJ_INV, proj_coord(1.0, 2.0, 3.0, 4.0));
  EXPECT_EQ(0, proj_errno(affine_pipe.get()));
  EXPECT_NEAR(-103.0, inv4d.xyzt.x, 1e-12);
  EXPECT_NEAR(-203.0, inv4d.xyzt.y, 1e-12);
  EXPECT_NEAR(-303.0, inv4d.xyzt.z, 1e-12);
  EXPECT_NEAR(-397.5, inv4d.xyzt.t, 1e-12);

  // 2. Exercise 3D forward and inverse entry points directly.
  const PJ_XYZ fwd3d = pj_fwd3d({1.0, 2.0, 3.0}, affine_pipe.get());
  EXPECT_EQ(0, proj_errno(affine_pipe.get()));
  EXPECT_NEAR(15.0, fwd3d.x, 1e-12);
  EXPECT_NEAR(27.0, fwd3d.y, 1e-12);
  EXPECT_NEAR(39.0, fwd3d.z, 1e-12);

  const PJ_LPZ inv3d = pj_inv3d({1.0, 2.0, 3.0}, affine_pipe.get());
  EXPECT_EQ(0, proj_errno(affine_pipe.get()));
  EXPECT_NEAR(-103.0, inv3d.lam, 1e-12);
  EXPECT_NEAR(-203.0, inv3d.phi, 1e-12);
  EXPECT_NEAR(-303.0, inv3d.z, 1e-12);

  // 3. Exercise 2D forward and inverse entry points directly.
  const PJ_XY fwd2d = pj_fwd({1.0, 2.0}, affine_pipe.get());
  EXPECT_EQ(0, proj_errno(affine_pipe.get()));
  EXPECT_NEAR(15.0, fwd2d.x, 1e-12);
  EXPECT_NEAR(27.0, fwd2d.y, 1e-12);

  const PJ_LP inv2d = pj_inv({1.0, 2.0}, affine_pipe.get());
  EXPECT_EQ(0, proj_errno(affine_pipe.get()));
  EXPECT_NEAR(-103.0, inv2d.lam, 1e-12);
  EXPECT_NEAR(-203.0, inv2d.phi, 1e-12);

  // 4. Exercise early break on HUGE_VAL in forward and inverse for 4D, 3D, 2D.
  // In PJ_FWD: step 1 (ortho at antimeridian lon=pi) emits HUGE_VAL before
  // trailing noop runs. In PJ_INV: step 1 (ortho at x=1e8 m, off-disk) emits
  // HUGE_VAL and breaks before step 0 (set v_1=12345) can overwrite x.
  ScopedLogSuppressor suppress(ctx.get());
  ScopedPj break_pipe(proj_create(ctx.get(),
                                  "+proj=pipeline "
                                  "+step +proj=set +v_1=12345 +omit_fwd "
                                  "+step +proj=ortho +R=6371000 "
                                  "+step +proj=noop"));
  ASSERT_NE(nullptr, break_pipe.get());

  const PJ_COORD fwd4_err =
      proj_trans(break_pipe.get(), PJ_FWD, proj_coord(kPi, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(fwd4_err.xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(break_pipe.get()));
  proj_errno_reset(break_pipe.get());

  const PJ_XYZ fwd3_err = pj_fwd3d({kPi, 0.0, 0.0}, break_pipe.get());
  EXPECT_TRUE(std::isinf(fwd3_err.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(break_pipe.get()));
  proj_errno_reset(break_pipe.get());

  const PJ_XY fwd2_err = pj_fwd({kPi, 0.0}, break_pipe.get());
  EXPECT_TRUE(std::isinf(fwd2_err.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(break_pipe.get()));
  proj_errno_reset(break_pipe.get());

  const PJ_COORD inv4_err =
      proj_trans(break_pipe.get(), PJ_INV, proj_coord(1.0e8, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inv4_err.xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(break_pipe.get()));
  proj_errno_reset(break_pipe.get());

  const PJ_LPZ inv3_err = pj_inv3d({1.0e8, 0.0, 0.0}, break_pipe.get());
  EXPECT_TRUE(std::isinf(inv3_err.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(break_pipe.get()));
  proj_errno_reset(break_pipe.get());

  const PJ_LP inv2_err = pj_inv({1.0e8, 0.0}, break_pipe.get());
  EXPECT_TRUE(std::isinf(inv2_err.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(break_pipe.get()));
  proj_errno_reset(break_pipe.get());
}

TEST(PipelineTest, PushAndPopCoverAllDimensionsEmptyStackAndStandalone) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Push and pop all four coordinate channels in both directions.
  ScopedPj push_all(
      proj_create(ctx.get(),
                  "+proj=pipeline "
                  "+step +proj=push +v_1 +v_2 +v_3 +v_4 "
                  "+step +proj=set +v_1=91 +v_2=92 +v_3=93 +v_4=94 "
                  "+step +proj=pop +v_1 +v_2 +v_3 +v_4"));
  ASSERT_NE(nullptr, push_all.get());

  const PJ_COORD fwd =
      proj_trans(push_all.get(), PJ_FWD, proj_coord(1.5, 2.5, 3.5, 2021.0));
  EXPECT_EQ(0, proj_errno(push_all.get()));
  EXPECT_DOUBLE_EQ(1.5, fwd.v[0]);
  EXPECT_DOUBLE_EQ(2.5, fwd.v[1]);
  EXPECT_DOUBLE_EQ(3.5, fwd.v[2]);
  EXPECT_DOUBLE_EQ(2021.0, fwd.v[3]);

  // In PJ_INV, pop runs first as push, then set overwrites, then push pops.
  const PJ_COORD inv =
      proj_trans(push_all.get(), PJ_INV, proj_coord(-4.0, -5.0, -6.0, 1999.0));
  EXPECT_EQ(0, proj_errno(push_all.get()));
  EXPECT_DOUBLE_EQ(-4.0, inv.v[0]);
  EXPECT_DOUBLE_EQ(-5.0, inv.v[1]);
  EXPECT_DOUBLE_EQ(-6.0, inv.v[2]);
  EXPECT_DOUBLE_EQ(1999.0, inv.v[3]);

  // 2. Selective v_1-only push/pop leaves v_2, v_3, v_4 overwritten by set.
  ScopedPj push_v1(
      proj_create(ctx.get(),
                  "+proj=pipeline "
                  "+step +proj=push +v_1 "
                  "+step +proj=set +v_1=99 +v_2=88 +v_3=77 +v_4=66 "
                  "+step +proj=pop +v_1"));
  ASSERT_NE(nullptr, push_v1.get());
  const PJ_COORD selective =
      proj_trans(push_v1.get(), PJ_FWD, proj_coord(10.0, 20.0, 30.0, 40.0));
  EXPECT_EQ(0, proj_errno(push_v1.get()));
  EXPECT_DOUBLE_EQ(10.0, selective.v[0]);
  EXPECT_DOUBLE_EQ(88.0, selective.v[1]);
  EXPECT_DOUBLE_EQ(77.0, selective.v[2]);
  EXPECT_DOUBLE_EQ(66.0, selective.v[3]);

  // 3. Pop on empty stack inside a 1-step pipeline leaves all coordinates
  // untouched (pj_create_internal keeps single-step pipeline intact).
  ScopedPj empty_pop(pj_create_internal(
      ctx.get(), "+proj=pipeline +step +proj=pop +v_1 +v_2 +v_3 +v_4"));
  ASSERT_NE(nullptr, empty_pop.get());
  const PJ_COORD untouched =
      proj_trans(empty_pop.get(), PJ_FWD, proj_coord(7.0, 8.0, 9.0, 10.0));
  EXPECT_EQ(0, proj_errno(empty_pop.get()));
  EXPECT_DOUBLE_EQ(7.0, untouched.v[0]);
  EXPECT_DOUBLE_EQ(8.0, untouched.v[1]);
  EXPECT_DOUBLE_EQ(9.0, untouched.v[2]);
  EXPECT_DOUBLE_EQ(10.0, untouched.v[3]);

  // 4. Standalone push/pop outside a pipeline have P->parent == nullptr and
  // act as no-ops cleanly.
  ScopedPj standalone_push(proj_create(ctx.get(), "+proj=push +v_1 +v_2"));
  ScopedPj standalone_pop(proj_create(ctx.get(), "+proj=pop +v_1 +v_2"));
  ASSERT_NE(nullptr, standalone_push.get());
  ASSERT_NE(nullptr, standalone_pop.get());

  const PJ_COORD push_noop =
      proj_trans(standalone_push.get(), PJ_FWD, proj_coord(3.0, 4.0, 0.0, 0.0));
  const PJ_COORD pop_noop =
      proj_trans(standalone_pop.get(), PJ_FWD, proj_coord(3.0, 4.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(standalone_push.get()));
  EXPECT_EQ(0, proj_errno(standalone_pop.get()));
  EXPECT_DOUBLE_EQ(3.0, push_noop.v[0]);
  EXPECT_DOUBLE_EQ(4.0, pop_noop.v[1]);
}

TEST(PipelineTest, IoUnitPropagationAndOneSidedWhateverCompatibility) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Backward propagation from merc -> preceding noop steps become RADIANS.
  ScopedPj backward_prop(
      proj_create(ctx.get(),
                  "+proj=pipeline +step +proj=noop +step +proj=noop "
                  "+step +proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, backward_prop.get());
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj_left(backward_prop.get()));
  EXPECT_EQ(PJ_IO_UNITS_PROJECTED, pj_right(backward_prop.get()));

  // 2. Forward propagation from merc -> trailing noop steps become PROJECTED.
  ScopedPj forward_prop(
      proj_create(ctx.get(),
                  "+proj=pipeline +step +proj=merc +ellps=WGS84 "
                  "+step +proj=noop +step +proj=noop"));
  ASSERT_NE(nullptr, forward_prop.get());
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj_left(forward_prop.get()));
  EXPECT_EQ(PJ_IO_UNITS_PROJECTED, pj_right(forward_prop.get()));

  // 3. Neighbor with left != right (unitconvert deg->rad) propagates DEGREES
  // left and RADIANS right to bounding noop steps.
  ScopedPj unit_prop(
      proj_create(ctx.get(),
                  "+proj=pipeline +step +proj=noop "
                  "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
                  "+step +proj=noop"));
  ASSERT_NE(nullptr, unit_prop.get());
  EXPECT_EQ(PJ_IO_UNITS_DEGREES, pj_left(unit_prop.get()));
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj_right(unit_prop.get()));
  const PJ_COORD converted =
      proj_trans(unit_prop.get(), PJ_FWD, proj_coord(180.0, 45.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(unit_prop.get()));
  EXPECT_NEAR(kPi, converted.lp.lam, 1e-12);
  EXPECT_NEAR(kPi / 4.0, converted.lp.phi, 1e-12);

  // 4. All-WHATEVER steps remain PJ_IO_UNITS_WHATEVER on both sides.
  ScopedPj whatever_only(proj_create(
      ctx.get(), "+proj=pipeline +step +proj=noop +step +proj=set +v_1=5"));
  ASSERT_NE(nullptr, whatever_only.get());
  EXPECT_EQ(PJ_IO_UNITS_WHATEVER, pj_left(whatever_only.get()));
  EXPECT_EQ(PJ_IO_UNITS_WHATEVER, pj_right(whatever_only.get()));

  // 5. Step with non-WHATEVER output followed by step whose input stays
  // WHATEVER (right is fixed to DEGREES so propagation leaves left as WHATEVER)
  // triggers next_step_input == PJ_IO_UNITS_WHATEVER compatibility check.
  ScopedPj one_sided_whatever(pj_create_internal(
      ctx.get(),
      "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad "
      "+step +proj=unitconvert +xy_out=deg"));
  ASSERT_NE(nullptr, one_sided_whatever.get());
  EXPECT_EQ(PJ_IO_UNITS_DEGREES, pj_left(one_sided_whatever.get()));
  EXPECT_EQ(PJ_IO_UNITS_DEGREES, pj_right(one_sided_whatever.get()));
}

TEST(PipelineTest, SetEllipsoidDefaultGrs80VsExplicitAndAssignContext) {
  ScopedContext ctx1 = MakeContext();
  ScopedContext ctx2 = MakeContext();
  ASSERT_NE(nullptr, ctx1.get());
  ASSERT_NE(nullptr, ctx2.get());

  // 1. Pipeline without global ellipsoid defaults to GRS80 and initializes
  // geod.
  ScopedPj default_ellps(
      pj_create_internal(ctx1.get(), "+proj=pipeline +step +proj=noop"));
  ASSERT_NE(nullptr, default_ellps.get());
  EXPECT_DOUBLE_EQ(6378137.0, default_ellps->a);
  EXPECT_NEAR(1.0 / 298.257222101, default_ellps->f, 1e-10);
  const double eq_one_deg_m =
      proj_lp_dist(default_ellps.get(), proj_coord(0.0, 0.0, 0.0, 0.0),
                   proj_coord(0.0, DegToRad(1.0), 0.0, 0.0));
  EXPECT_NEAR(110574.3885, eq_one_deg_m, 1e-2);

  // 2. Explicit global ellipsoid is retained on pipeline and passed to steps.
  ScopedPj explicit_ellps(pj_create_internal(
      ctx1.get(), "+proj=pipeline +ellps=clrk66 +step +proj=merc"));
  ASSERT_NE(nullptr, explicit_ellps.get());
  EXPECT_DOUBLE_EQ(6378206.4, explicit_ellps->a);
  const PJ_COORD merc_out =
      proj_trans(explicit_ellps.get(), PJ_FWD, proj_coord(1.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(explicit_ellps.get()));
  EXPECT_NEAR(6378206.4, merc_out.xy.x, 1e-6);

  // 3. Reassigning context updates both pipeline and all child steps.
  ScopedPj multi(pj_create_internal(
      ctx1.get(),
      "+proj=pipeline +step +proj=ortho +R=6371000 +step +proj=noop"));
  ASSERT_NE(nullptr, multi.get());
  proj_assign_context(multi.get(), ctx2.get());
  EXPECT_EQ(ctx2.get(), pj_get_ctx(multi.get()));

  ScopedLogSuppressor suppress_ctx2(ctx2.get());
  proj_errno_reset(multi.get());
  const PJ_COORD err_pt =
      proj_trans(multi.get(), PJ_FWD, proj_coord(kPi, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(err_pt.xyzt.x));
  EXPECT_EQ(0, proj_context_errno(ctx1.get()));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_context_errno(ctx2.get()));
}

TEST(PipelineTest, InverseCapabilityHonorsOmitInvAndOmitFwd) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. tcc lacks an inverse -> entire pipeline becomes forward-only.
  ScopedPj no_inv(pj_create_internal(
      ctx.get(), "+proj=pipeline +step +proj=tcc +R=6371000"));
  ASSERT_NE(nullptr, no_inv.get());
  EXPECT_EQ(0, proj_pj_info(no_inv.get()).has_inverse);
  EXPECT_EQ(nullptr, no_inv->inv);
  EXPECT_EQ(nullptr, no_inv->inv3d);
  EXPECT_EQ(nullptr, no_inv->inv4d);

  const PJ_COORD inv_fail =
      proj_trans(no_inv.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inv_fail.xyzt.x));
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(no_inv.get()));

  // 2. Adding +omit_inv skips the non-invertible step on inverse, keeping it.
  ScopedPj omit_inv(pj_create_internal(
      ctx.get(), "+proj=pipeline +step +proj=tcc +R=6371000 +omit_inv"));
  ASSERT_NE(nullptr, omit_inv.get());
  EXPECT_EQ(1, proj_pj_info(omit_inv.get()).has_inverse);
  const PJ_COORD inv_ok =
      proj_trans(omit_inv.get(), PJ_INV, proj_coord(0.5, 0.25, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(omit_inv.get()));
  EXPECT_DOUBLE_EQ(0.5, inv_ok.xyzt.x);

  // 3. Inverted tcc with +omit_fwd skips the missing inv in PJ_FWD while
  // allowing PJ_INV to execute forward tcc.
  ScopedPj omit_fwd(pj_create_internal(
      ctx.get(), "+proj=pipeline +step +proj=tcc +inv +R=6371000 +omit_fwd"));
  ASSERT_NE(nullptr, omit_fwd.get());
  EXPECT_EQ(1, proj_pj_info(omit_fwd.get()).has_inverse);
  const PJ_COORD fwd_skipped =
      proj_trans(omit_fwd.get(), PJ_FWD, proj_coord(0.2, 0.3, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(omit_fwd.get()));
  EXPECT_DOUBLE_EQ(0.2, fwd_skipped.xyzt.x);

  const PJ_COORD inv_runs_tcc_fwd =
      proj_trans(omit_fwd.get(), PJ_INV, proj_coord(0.1, 0.2, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(omit_fwd.get()));
  EXPECT_NE(0.1, inv_runs_tcc_fwd.xy.x);
}

TEST(PipelineTest, GlobalPlusLocalInvToggleCancelsAndTraceLogsSteps) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedLogCapture trace(ctx.get(), PJ_LOG_TRACE);
  ScopedPj double_inv(pj_create_internal(
      ctx.get(), "+proj=pipeline +inv +step +proj=merc +inv +R=6371000"));
  ASSERT_NE(nullptr, double_inv.get());

  EXPECT_TRUE(trace.ContainsMessage("Building arg list for step no. 0"));
  EXPECT_TRUE(trace.ContainsMessage("1 steps built. Determining i/o"));

  // Global +inv combined with step +inv toggles inverted twice (0 -> 1 -> 0),
  // leaving the step running forward Mercator.
  const PJ_COORD out =
      proj_trans(double_inv.get(), PJ_FWD, proj_coord(1.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(double_inv.get()));
  EXPECT_NEAR(6371000.0, out.xy.x, 1e-6);
  EXPECT_NEAR(0.0, out.xy.y, 1e-9);
}

TEST(PipelineTest, SyntaxAndValidationErrorsReturnExpectedErrnos) {
  struct ErrorCase {
    std::string_view def;
    int expected_errno;
    std::string_view expected_log;
  };
  constexpr ErrorCase kErrorCases[] = {
      {
          .def = "+step +proj=pipeline +proj=noop",
          .expected_errno = PROJ_ERR_INVALID_OP_WRONG_SYNTAX,
          .expected_log = "+step before +proj=pipeline",
      },
      {
          .def = "+proj=pipeline +proj=merc +step +proj=noop",
          .expected_errno = PROJ_ERR_INVALID_OP_WRONG_SYNTAX,
          .expected_log = "proj= operator before first step",
      },
      {
          .def = "+proj=pipeline +o_proj=merc +step +proj=noop",
          .expected_errno = PROJ_ERR_INVALID_OP_WRONG_SYNTAX,
          .expected_log = "o_proj= operator before first step",
      },
      {
          .def = "+proj=pipeline +ellps=WGS84",
          .expected_errno = PROJ_ERR_INVALID_OP_WRONG_SYNTAX,
          .expected_log = "",
      },
      {
          .def = "+proj=pipeline +step +proj=nonexistent_proj",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
          .expected_log = "Bad step definition",
      },
      {
          .def = "+proj=pipeline +step +proj=tcc +inv +R=6371000",
          .expected_errno = PROJ_ERR_OTHER_NO_INVERSE_OP,
          .expected_log = "Inverse operation for tcc is not available",
      },
      {
          .def = "+proj=pipeline +step +proj=merc +ellps=WGS84 "
                 "+step +proj=merc +ellps=WGS84",
          .expected_errno = PROJ_ERR_INVALID_OP_WRONG_SYNTAX,
          .expected_log = "Mismatched units between step 1 and 2",
      },
  };

  for (const auto& tc : kErrorCases) {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ScopedLogCapture errors(ctx.get(), PJ_LOG_ERROR);

    const std::string def_str(tc.def);
    ScopedPj bad(pj_create_internal(ctx.get(), def_str.c_str()));
    EXPECT_EQ(nullptr, bad.get()) << tc.def;
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get())) << tc.def;
    if (!tc.expected_log.empty()) {
      EXPECT_TRUE(errors.ContainsMessage(std::string(tc.expected_log)))
          << tc.def;
    }
  }

  // Also confirm public proj_create propagates pipeline unit mismatch cleanly.
  ScopedContext pub_ctx = MakeContext();
  ASSERT_NE(nullptr, pub_ctx.get());
  ScopedLogSuppressor suppress(pub_ctx.get());
  ScopedPj bad_pub(proj_create(pub_ctx.get(),
                               "+proj=pipeline +step +proj=merc +ellps=WGS84 "
                               "+step +proj=merc +ellps=WGS84"));
  EXPECT_EQ(nullptr, bad_pub.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_WRONG_SYNTAX,
            proj_context_errno(pub_ctx.get()));
}

TEST(PipelineTest, InitExpansionNestsChildPipelinesAndEnforcesLimits) {
  InMemoryFileSystem fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ASSERT_TRUE(fs.Install(ctx.get()));
  // get_init caches parsed sections process-wide; clear before leaving scope.
  absl::Cleanup clear_init = [] { pj_clear_initcache(); };

  fs.AddTextFile("/mem/pipe_init.txt",
                 "<child_shift>\n"
                 "+proj=pipeline +step +proj=affine +xoff=10 +yoff=-5\n\n"
                 "<nested_unwrapped>\n"
                 "+proj=pipeline +step +proj=pipeline +step +proj=noop\n\n"
                 "<self_loop>\n"
                 "+proj=pipeline +step +init=/mem/pipe_init.txt:self_loop\n\n");

  // 1. Child pipeline wrapped in +init executes cleanly as a pipeline step.
  ScopedPj valid_nested(pj_create_internal(
      ctx.get(),
      "+proj=pipeline +step +init=/mem/pipe_init.txt:child_shift "
      "+step +proj=affine +xoff=2"));
  ASSERT_NE(nullptr, valid_nested.get());
  const PJ_COORD out =
      proj_trans(valid_nested.get(), PJ_FWD, proj_coord(1.0, 2.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(valid_nested.get()));
  EXPECT_DOUBLE_EQ(13.0, out.xy.x);
  EXPECT_DOUBLE_EQ(-3.0, out.xy.y);

  // 2. Unwrapped nested proj=pipeline inside expanded +init is rejected.
  ScopedLogCapture errors(ctx.get(), PJ_LOG_ERROR);
  ScopedPj unwrapped(pj_create_internal(
      ctx.get(), "+init=/mem/pipe_init.txt:nested_unwrapped"));
  EXPECT_EQ(nullptr, unwrapped.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_WRONG_SYNTAX, proj_context_errno(ctx.get()));
  EXPECT_TRUE(errors.ContainsMessage("Nesting only allowed"));

  // 3. Self-referential +init hits pipelineInitRecursiongCounter == 5 and
  // invokes destructor while P->opaque == nullptr, unwinding cleanly.
  errors.Clear();
  ScopedPj recursive(pj_create_internal(
      ctx.get(), "+proj=pipeline +step +init=/mem/pipe_init.txt:self_loop"));
  EXPECT_EQ(nullptr, recursive.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_WRONG_SYNTAX, proj_context_errno(ctx.get()));
  EXPECT_TRUE(errors.ContainsMessage("too deep recursion"));
  EXPECT_EQ(0, ctx->pipelineInitRecursiongCounter);
}

// TODO(schwehr): Defensive OOM checks (lines 300, 403, 409, 414, 693),
// destructor(nullptr) (line 265), i_pipeline == -1 (line 458) guarded upstream
// by init.cpp, err_to_report == 0 fallback (line 506), and Q->fwd missing on a
// non-inverted built-in operator (lines 551-554) are unreachable without mock
// allocator or custom PJ injection.

}  // namespace
}  // namespace proj
