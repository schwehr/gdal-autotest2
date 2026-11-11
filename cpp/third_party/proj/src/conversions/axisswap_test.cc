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
// Unit tests for coordinate axis reordering and sign flipping
// (proj/src/conversions/axisswap.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/conversions/axisswap.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

TEST(AxisswapTest, Order2DSwapXYFastPathArgvAndMetadata) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. +order=2,1 triggers the dedicated swap_xy_4d fast path for both
  // forward and inverse while preserving z and t.
  ScopedPj pj(proj_create(ctx.get(), "+proj=axisswap +order=2,1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("axisswap", info.id);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0, proj_angular_input(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD));

  const PJ_COORD input = proj_coord(10.0, -20.0, 30.0, 2026.5);
  const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, input);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(10.0, ident.xyzt.x, 1e-15);
  EXPECT_NEAR(-20.0, ident.xyzt.y, 1e-15);
  EXPECT_NEAR(30.0, ident.xyzt.z, 1e-15);
  EXPECT_NEAR(2026.5, ident.xyzt.t, 1e-15);

  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, input);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-20.0, fwd.xyzt.x, 1e-15);
  EXPECT_NEAR(10.0, fwd.xyzt.y, 1e-15);
  EXPECT_NEAR(30.0, fwd.xyzt.z, 1e-15);
  EXPECT_NEAR(2026.5, fwd.xyzt.t, 1e-15);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(10.0, inv.xyzt.x, 1e-15);
  EXPECT_NEAR(-20.0, inv.xyzt.y, 1e-15);
  EXPECT_NEAR(30.0, inv.xyzt.z, 1e-15);
  EXPECT_NEAR(2026.5, inv.xyzt.t, 1e-15);

  // 2. Verify proj_create_argv with mutable buffers.
  char arg0[] = "+proj=axisswap";
  char arg1[] = "+order=2,1";
  char* argv[] = {arg0, arg1};
  ScopedPj argv_pj(proj_create_argv(ctx.get(), 2, argv));
  ASSERT_NE(nullptr, argv_pj.get());
  const PJ_COORD argv_fwd = proj_trans(argv_pj.get(), PJ_FWD, input);
  EXPECT_EQ(0, proj_errno(argv_pj.get()));
  EXPECT_NEAR(-20.0, argv_fwd.xyzt.x, 1e-15);
  EXPECT_NEAR(10.0, argv_fwd.xyzt.y, 1e-15);
}

TEST(AxisswapTest, Order2DGeneralPermutationsAndSignFlips) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct Case2D {
    const char* def;
    double in_x;
    double in_y;
    double expected_x;
    double expected_y;
  };
  constexpr Case2D kCases[] = {
      {.def = "+proj=axisswap +order=1,2",
       .in_x = 11.0,
       .in_y = 22.0,
       .expected_x = 11.0,
       .expected_y = 22.0},
      // Non-self-inverse 90-degree rotations verify fwd vs inv distinction.
      {.def = "+proj=axisswap +order=-2,1",
       .in_x = 11.0,
       .in_y = 22.0,
       .expected_x = -22.0,
       .expected_y = 11.0},
      {.def = "+proj=axisswap +order=2,-1",
       .in_x = 11.0,
       .in_y = 22.0,
       .expected_x = 22.0,
       .expected_y = -11.0},
      {.def = "+proj=axisswap +order=-1,2",
       .in_x = 11.0,
       .in_y = 22.0,
       .expected_x = -11.0,
       .expected_y = 22.0},
      {.def = "+proj=axisswap +order=1,-2",
       .in_x = 11.0,
       .in_y = 22.0,
       .expected_x = 11.0,
       .expected_y = -22.0},
      {.def = "+proj=axisswap +order=-2,-1",
       .in_x = 11.0,
       .in_y = 22.0,
       .expected_x = -22.0,
       .expected_y = -11.0},
  };

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    // 1. 4D API falls back to 2D fwd/inv, preserving z and t.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(tc.in_x, tc.in_y, 33.0, 44.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_x, fwd.xyzt.x, 1e-15) << tc.def;
    EXPECT_NEAR(tc.expected_y, fwd.xyzt.y, 1e-15) << tc.def;
    EXPECT_NEAR(33.0, fwd.xyzt.z, 1e-15) << tc.def;
    EXPECT_NEAR(44.0, fwd.xyzt.t, 1e-15) << tc.def;

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.in_x, inv.xyzt.x, 1e-15) << tc.def;
    EXPECT_NEAR(tc.in_y, inv.xyzt.y, 1e-15) << tc.def;
    EXPECT_NEAR(33.0, inv.xyzt.z, 1e-15) << tc.def;
    EXPECT_NEAR(44.0, inv.xyzt.t, 1e-15) << tc.def;

    // 2. Direct internal 2D entry points pj_fwd / pj_inv.
    const PJ_XY xy = pj_fwd({.lam = tc.in_x, .phi = tc.in_y}, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_x, xy.x, 1e-15) << tc.def;
    EXPECT_NEAR(tc.expected_y, xy.y, 1e-15) << tc.def;

    const PJ_LP lp = pj_inv(xy, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.in_x, lp.lam, 1e-15) << tc.def;
    EXPECT_NEAR(tc.in_y, lp.phi, 1e-15) << tc.def;
  }
}

TEST(AxisswapTest, Order3DPermutationsAndSignFlips) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct Case3D {
    const char* def;
    double expected_x;
    double expected_y;
    double expected_z;
  };
  constexpr Case3D kCases[] = {
      // Cyclic 3D permutation is not self-inverse.
      {.def = "+proj=axisswap +order=2,3,1",
       .expected_x = 20.0,
       .expected_y = 30.0,
       .expected_z = 10.0},
      {.def = "+proj=axisswap +order=-3,1,-2",
       .expected_x = -30.0,
       .expected_y = 10.0,
       .expected_z = -20.0},
      {.def = "+proj=axisswap +order=1,2,-3",
       .expected_x = 10.0,
       .expected_y = 20.0,
       .expected_z = -30.0},
  };

  constexpr double kX = 10.0;
  constexpr double kY = 20.0;
  constexpr double kZ = 30.0;
  constexpr double kT = 40.0;

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    // 1. 4D API falls back to 3D fwd/inv, leaving t untouched.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(kX, kY, kZ, kT));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_x, fwd.xyzt.x, 1e-15) << tc.def;
    EXPECT_NEAR(tc.expected_y, fwd.xyzt.y, 1e-15) << tc.def;
    EXPECT_NEAR(tc.expected_z, fwd.xyzt.z, 1e-15) << tc.def;
    EXPECT_NEAR(kT, fwd.xyzt.t, 1e-15) << tc.def;

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(kX, inv.xyzt.x, 1e-15) << tc.def;
    EXPECT_NEAR(kY, inv.xyzt.y, 1e-15) << tc.def;
    EXPECT_NEAR(kZ, inv.xyzt.z, 1e-15) << tc.def;
    EXPECT_NEAR(kT, inv.xyzt.t, 1e-15) << tc.def;

    // 2. Direct internal 3D entry points pj_fwd3d / pj_inv3d.
    const PJ_XYZ xyz = pj_fwd3d({.lam = kX, .phi = kY, .z = kZ}, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_x, xyz.x, 1e-15) << tc.def;
    EXPECT_NEAR(tc.expected_y, xyz.y, 1e-15) << tc.def;
    EXPECT_NEAR(tc.expected_z, xyz.z, 1e-15) << tc.def;

    const PJ_LPZ lpz = pj_inv3d(xyz, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(kX, lpz.lam, 1e-15) << tc.def;
    EXPECT_NEAR(kY, lpz.phi, 1e-15) << tc.def;
    EXPECT_NEAR(kZ, lpz.z, 1e-15) << tc.def;
  }
}

TEST(AxisswapTest, Order4DPermutationsAndSignFlips) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct Case4D {
    const char* def;
    double expected_x;
    double expected_y;
    double expected_z;
    double expected_t;
  };
  constexpr Case4D kCases[] = {
      {.def = "+proj=axisswap +order=4,3,2,1",
       .expected_x = 40.0,
       .expected_y = 30.0,
       .expected_z = 20.0,
       .expected_t = 10.0},
      // Non-self-inverse 4-cycle verifies distinct forward and inverse maps.
      {.def = "+proj=axisswap +order=2,3,4,-1",
       .expected_x = 20.0,
       .expected_y = 30.0,
       .expected_z = 40.0,
       .expected_t = -10.0},
      {.def = "+proj=axisswap +order=-1,-2,-3,-4",
       .expected_x = -10.0,
       .expected_y = -20.0,
       .expected_z = -30.0,
       .expected_t = -40.0},
      {.def = "+proj=axisswap +order=3,-4,1,-2",
       .expected_x = 30.0,
       .expected_y = -40.0,
       .expected_z = 10.0,
       .expected_t = -20.0},
      // Fifth and further elements are ignored once all 4 slots are filled.
      {.def = "+proj=axisswap +order=1,2,3,4,1",
       .expected_x = 10.0,
       .expected_y = 20.0,
       .expected_z = 30.0,
       .expected_t = 40.0},
  };

  const PJ_COORD input = proj_coord(10.0, 20.0, 30.0, 40.0);
  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, input);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_x, fwd.xyzt.x, 1e-15) << tc.def;
    EXPECT_NEAR(tc.expected_y, fwd.xyzt.y, 1e-15) << tc.def;
    EXPECT_NEAR(tc.expected_z, fwd.xyzt.z, 1e-15) << tc.def;
    EXPECT_NEAR(tc.expected_t, fwd.xyzt.t, 1e-15) << tc.def;

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(input.xyzt.x, inv.xyzt.x, 1e-15) << tc.def;
    EXPECT_NEAR(input.xyzt.y, inv.xyzt.y, 1e-15) << tc.def;
    EXPECT_NEAR(input.xyzt.z, inv.xyzt.z, 1e-15) << tc.def;
    EXPECT_NEAR(input.xyzt.t, inv.xyzt.t, 1e-15) << tc.def;
  }
}

TEST(AxisswapTest, AxisParameterCoversAllSixOrientations) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct AxisCase {
    const char* def;
    double expected_x;
    double expected_y;
    double expected_z;
  };
  constexpr AxisCase kCases[] = {
      {.def = "+proj=axisswap +axis=neu",
       .expected_x = 20.0,
       .expected_y = 10.0,
       .expected_z = 30.0},
      {.def = "+proj=axisswap +axis=wsd",
       .expected_x = -10.0,
       .expected_y = -20.0,
       .expected_z = -30.0},
      // Non-self-inverse axis triple testing d, e, n together.
      {.def = "+proj=axisswap +axis=den",
       .expected_x = -30.0,
       .expected_y = 10.0,
       .expected_z = 20.0},
  };

  const PJ_COORD input = proj_coord(10.0, 20.0, 30.0, 50.0);
  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, input);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_x, fwd.xyzt.x, 1e-15) << tc.def;
    EXPECT_NEAR(tc.expected_y, fwd.xyzt.y, 1e-15) << tc.def;
    EXPECT_NEAR(tc.expected_z, fwd.xyzt.z, 1e-15) << tc.def;
    EXPECT_NEAR(50.0, fwd.xyzt.t, 1e-15) << tc.def;

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(10.0, inv.xyzt.x, 1e-15) << tc.def;
    EXPECT_NEAR(20.0, inv.xyzt.y, 1e-15) << tc.def;
    EXPECT_NEAR(30.0, inv.xyzt.z, 1e-15) << tc.def;
    EXPECT_NEAR(50.0, inv.xyzt.t, 1e-15) << tc.def;
  }
}

TEST(AxisswapTest, AngularUnitsPipelinesEllipsoidsAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. +angularunits sets PJ_IO_UNITS_RADIANS on both left and right sides.
  ScopedPj angular_pj(
      proj_create(ctx.get(), "+proj=axisswap +order=2,1 +angularunits"));
  ASSERT_NE(nullptr, angular_pj.get());
  EXPECT_EQ(1, proj_angular_input(angular_pj.get(), PJ_FWD));
  EXPECT_EQ(1, proj_angular_output(angular_pj.get(), PJ_FWD));
  EXPECT_EQ(1, proj_angular_input(angular_pj.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(angular_pj.get(), PJ_INV));

  // skip_fwd_prepare is set, so values > pi are swapped directly without
  // invalid-latitude rejection.
  const PJ_COORD ang_fwd =
      proj_trans(angular_pj.get(), PJ_FWD, proj_coord(5.0, 2.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(angular_pj.get()));
  EXPECT_NEAR(2.0, ang_fwd.xyzt.x, 1e-15);
  EXPECT_NEAR(5.0, ang_fwd.xyzt.y, 1e-15);

  // 2. Spherical (+R=...) vs ellipsoidal (+ellps=WGS84) in pipelines and
  // +axis=neu cs2cs emulation.
  ScopedPj sphere_pipe(
      proj_create(ctx.get(),
                  "+proj=pipeline +step +proj=axisswap +order=2,1 "
                  "+angularunits +step +proj=merc +R=6371000"));
  ScopedPj ellps_pipe(
      proj_create(ctx.get(),
                  "+proj=pipeline +step +proj=axisswap +order=2,1 "
                  "+angularunits +step +proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, sphere_pipe.get());
  ASSERT_NE(nullptr, ellps_pipe.get());

  const PJ_COORD lat_lon_rad =
      proj_coord(kPi / 4.0, 10.0 * kPi / 180.0, 0.0, 0.0);
  const PJ_COORD sphere_xy = proj_trans(sphere_pipe.get(), PJ_FWD, lat_lon_rad);
  const PJ_COORD ellps_xy = proj_trans(ellps_pipe.get(), PJ_FWD, lat_lon_rad);
  EXPECT_EQ(0, proj_errno(sphere_pipe.get()));
  EXPECT_EQ(0, proj_errno(ellps_pipe.get()));
  EXPECT_GT(std::abs(sphere_xy.xyzt.y - ellps_xy.xyzt.y), 1000.0);

  ScopedPj merc_neu(
      proj_create(ctx.get(), "+proj=merc +ellps=WGS84 +axis=neu"));
  ASSERT_NE(nullptr, merc_neu.get());
  const PJ_COORD neu_out = proj_trans(
      merc_neu.get(), PJ_FWD, proj_coord(10.0 * kPi / 180.0, kPi / 4.0, 0, 0));
  EXPECT_EQ(0, proj_errno(merc_neu.get()));
  EXPECT_NEAR(ellps_xy.xyzt.y, neu_out.xyzt.x, 1e-6);
  EXPECT_NEAR(ellps_xy.xyzt.x, neu_out.xyzt.y, 1e-6);

  // 3. proj_factors succeeds on 2D axisswap (P->fwd is populated), and fails on
  // +order=2,1 (which uses swap_xy_4d with P->fwd == nullptr).
  ScopedPj rot_2d(
      proj_create(ctx.get(), "+proj=axisswap +order=-2,1 +ellps=WGS84"));
  ASSERT_NE(nullptr, rot_2d.get());
  const PJ_FACTORS fac =
      proj_factors(rot_2d.get(), proj_coord(0.1, 0.2, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(rot_2d.get()));
  EXPECT_NEAR(0.0, fac.dx_dlam, 1e-9);
  EXPECT_NEAR(-1.0, fac.dx_dphi, 1e-9);
  EXPECT_NEAR(1.0, fac.dy_dlam, 1e-9);
  EXPECT_NEAR(0.0, fac.dy_dphi, 1e-9);

  ScopedLogSuppressor suppress(ctx.get());
  ScopedPj fast_2d(
      proj_create(ctx.get(), "+proj=axisswap +order=2,1 +R=6371000"));
  ASSERT_NE(nullptr, fast_2d.get());
  proj_factors(fast_2d.get(), proj_coord(0.1, 0.2, 0.0, 0.0));
  EXPECT_NE(0, proj_errno(fast_2d.get()));
  proj_errno_reset(fast_2d.get());
}

TEST(AxisswapTest, BatchTransformsAndRoundtrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Batch proj_trans_array on 4D axisswap.
  ScopedPj pj_4d(proj_create(ctx.get(), "+proj=axisswap +order=2,3,4,-1"));
  ASSERT_NE(nullptr, pj_4d.get());
  std::array<PJ_COORD, 2> coords = {
      proj_coord(1.0, 2.0, 3.0, 4.0),
      proj_coord(-5.0, 6.0, -7.0, 8.0),
  };
  const std::array<PJ_COORD, 2> orig = coords;
  ASSERT_EQ(
      0, proj_trans_array(pj_4d.get(), PJ_FWD, coords.size(), coords.data()));
  EXPECT_NEAR(2.0, coords[0].xyzt.x, 1e-15);
  EXPECT_NEAR(3.0, coords[0].xyzt.y, 1e-15);
  EXPECT_NEAR(4.0, coords[0].xyzt.z, 1e-15);
  EXPECT_NEAR(-1.0, coords[0].xyzt.t, 1e-15);

  ASSERT_EQ(
      0, proj_trans_array(pj_4d.get(), PJ_INV, coords.size(), coords.data()));
  for (size_t i = 0; i < coords.size(); ++i) {
    EXPECT_NEAR(orig[i].xyzt.x, coords[i].xyzt.x, 1e-15);
    EXPECT_NEAR(orig[i].xyzt.y, coords[i].xyzt.y, 1e-15);
    EXPECT_NEAR(orig[i].xyzt.z, coords[i].xyzt.z, 1e-15);
    EXPECT_NEAR(orig[i].xyzt.t, coords[i].xyzt.t, 1e-15);
  }

  // 2. Strided proj_trans_generic on 2D axisswap.
  ScopedPj pj_2d(proj_create(ctx.get(), "+proj=axisswap +order=-2,1"));
  ASSERT_NE(nullptr, pj_2d.get());
  struct XYItem {
    double x;
    double y;
    int id;
  };
  std::array<XYItem, 2> items = {
      XYItem{.x = 3.0, .y = 7.0, .id = 1},
      XYItem{.x = -4.0, .y = 9.0, .id = 2},
  };
  EXPECT_EQ(2u,
            proj_trans_generic(pj_2d.get(), PJ_FWD, &items[0].x, sizeof(XYItem),
                               items.size(), &items[0].y, sizeof(XYItem),
                               items.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj_2d.get()));
  EXPECT_NEAR(-7.0, items[0].x, 1e-15);
  EXPECT_NEAR(3.0, items[0].y, 1e-15);
  EXPECT_NEAR(-9.0, items[1].x, 1e-15);
  EXPECT_NEAR(-4.0, items[1].y, 1e-15);

  // 3. Exact round-trip over multiple iterations.
  PJ_COORD rt = proj_coord(1.0, 2.0, 3.0, 4.0);
  EXPECT_NEAR(0.0, proj_roundtrip(pj_4d.get(), PJ_FWD, 8, &rt), 1e-15);
  EXPECT_EQ(0, proj_errno(pj_4d.get()));
}

TEST(AxisswapTest, ParameterValidationErrorsCoverAllBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct ErrorCase {
    const char* def;
    int expected_errno;
    const char* expected_log;
  };
  constexpr ErrorCase kErrorCases[] = {
      // Missing or mutually exclusive +order and +axis.
      {.def = "+proj=axisswap",
       .expected_errno = PROJ_ERR_INVALID_OP_MUTUALLY_EXCLUSIVE_ARGS,
       .expected_log = "must provide EITHER 'order' OR 'axis' parameter."},
      {.def = "+proj=axisswap +order=2,1 +axis=neu",
       .expected_errno = PROJ_ERR_INVALID_OP_MUTUALLY_EXCLUSIVE_ARGS,
       .expected_log = "must provide EITHER 'order' OR 'axis' parameter."},
      // Disallowed characters in +order.
      {.def = "+proj=axisswap +order=2,1,5",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "unknown axis '5'"},
      {.def = "+proj=axisswap +order=a,b",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "unknown axis 'a'"},
      // Characters valid in "1234-," but producing out-of-range axis index.
      {.def = "+proj=axisswap +order=12,3",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "invalid axis"},
      {.def = "+proj=axisswap +order=-,2",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "invalid axis"},
      {.def = "+proj=axisswap +order=,1",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "invalid axis"},
      // Duplicate axis specifications (with +order or +axis).
      {.def = "+proj=axisswap +order=1,1",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "duplicate axes specified"},
      {.def = "+proj=axisswap +order=1,-1",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "duplicate axes specified"},
      {.def = "+proj=axisswap +order=2,3,2",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "duplicate axes specified"},
      {.def = "+proj=axisswap +order=1,2,3,1",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "duplicate axes specified"},
      {.def = "+proj=axisswap +axis=ewu",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "duplicate axes specified"},
      {.def = "+proj=axisswap +axis=nnu",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "duplicate axes specified"},
      // Incomplete / invalid dimensionality or high-index axes in 2D/3D setups.
      {.def = "+proj=axisswap +order",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "bad axis order"},
      {.def = "+proj=axisswap +order=1",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "bad axis order"},
      {.def = "+proj=axisswap +order=1,3",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "bad axis order"},
      {.def = "+proj=axisswap +order=3,1",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "bad axis order"},
      {.def = "+proj=axisswap +order=2,3",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "bad axis order"},
      {.def = "+proj=axisswap +order=1,2,4",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "bad axis order"},
      {.def = "+proj=axisswap +order=1,4,2",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "bad axis order"},
      {.def = "+proj=axisswap +order=4,1,2",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "bad axis order"},
      // Invalid +axis orientation rejected during init before setup.
      {.def = "+proj=axisswap +axis=xyz",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for axis"},
  };

  for (const auto& tc : kErrorCases) {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj bad(proj_create(ctx.get(), tc.def));
    EXPECT_EQ(nullptr, bad.get()) << tc.def;
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get())) << tc.def;
    EXPECT_TRUE(capture.ContainsMessage(tc.expected_log))
        << tc.def << " missing log: " << tc.expected_log;
  }
}

TEST(AxisswapTest, EdgeInputsNullEmptyNanInfAndHugeVal) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Null and empty strings fail cleanly.
  EXPECT_EQ(nullptr, proj_create(ctx.get(), nullptr));
  EXPECT_NE(0, proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);

  EXPECT_EQ(nullptr, proj_create(ctx.get(), ""));
  EXPECT_NE(0, proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);

  // 2. HUGE_VAL, NaN, and -Inf handling.
  ScopedPj pj(proj_create(ctx.get(), "+proj=axisswap +order=2,1"));
  ASSERT_NE(nullptr, pj.get());

  // Direct fwd4d call moves HUGE_VAL in x to y without triggering fwd's
  // coo.v[0] == HUGE_VAL sentinel, whereas proj_trans rejects HUGE_VAL in x
  // up front in trans.cpp.
  ASSERT_NE(nullptr, pj->fwd4d);
  PJ_COORD direct_huge_x = proj_coord(HUGE_VAL, 2.0, 3.0, 4.0);
  pj->fwd4d(direct_huge_x, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(2.0, direct_huge_x.xyzt.x, 1e-15);
  EXPECT_TRUE(std::isinf(direct_huge_x.xyzt.y));

  const PJ_COORD huge_x_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(HUGE_VAL, 2.0, 3.0, 4.0));
  EXPECT_TRUE(std::isinf(huge_x_fwd.xyzt.x));
  EXPECT_TRUE(std::isinf(huge_x_fwd.xyzt.y));

  const PJ_COORD huge_y_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(1.0, HUGE_VAL, 3.0, 4.0));
  EXPECT_TRUE(std::isinf(huge_y_fwd.xyzt.x));
  EXPECT_TRUE(std::isinf(huge_y_fwd.xyzt.z));

  const PJ_COORD huge_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(1.0, HUGE_VAL, 3.0, 4.0));
  EXPECT_TRUE(std::isinf(huge_inv.xyzt.x));
  EXPECT_TRUE(std::isinf(huge_inv.xyzt.z));

  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const PJ_COORD nan_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(1.0, kNaN, 3.0, 4.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_TRUE(std::isnan(nan_fwd.xyzt.x));
  EXPECT_TRUE(std::isnan(nan_fwd.xyzt.y));
  EXPECT_TRUE(std::isnan(nan_fwd.xyzt.z));
  EXPECT_TRUE(std::isnan(nan_fwd.xyzt.t));

  // -INFINITY is distinct from +HUGE_VAL and swaps without erroring.
  const double kNegInf = -std::numeric_limits<double>::infinity();
  const PJ_COORD neg_inf_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(5.0, kNegInf, 3.0, 4.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(kNegInf, neg_inf_fwd.xyzt.x);
  EXPECT_NEAR(5.0, neg_inf_fwd.xyzt.y, 1e-15);
}

}  // namespace
}  // namespace proj
