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
// Unit tests for forward coordinate preparation, invocation dispatch,
// finalization, and error propagation (proj/src/fwd.cpp). Note: in google3
// default non-GCE builds, networking is disabled and proj.db is embedded.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/fwd.cpp

#include <cmath>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(FwdTest, DispatchOrderAndMissingForwardPointersAcross2d3d4d) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. P->fwd only (+proj=merc +R=6371000): pj_fwd uses P->fwd directly while
  //    pj_fwd3d and pj_fwd4d fall back to P->fwd, preserving Z and T.
  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +R=6371000"));
  ASSERT_NE(nullptr, merc.get());
  ASSERT_NE(nullptr, merc->fwd);
  EXPECT_EQ(nullptr, merc->fwd3d);
  EXPECT_EQ(nullptr, merc->fwd4d);

  const double lon = DegToRad(10.0);
  const double lat = DegToRad(20.0);
  const PJ_XY merc_2d = pj_fwd(proj_coord(lon, lat, 0.0, 0.0).lp, merc.get());
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_NEAR(6371000.0 * lon, merc_2d.x, 1e-6);

  const PJ_XYZ merc_3d =
      pj_fwd3d(proj_coord(lon, lat, 123.0, 0.0).lpz, merc.get());
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_NEAR(merc_2d.x, merc_3d.x, 1e-9);
  EXPECT_NEAR(merc_2d.y, merc_3d.y, 1e-9);
  EXPECT_NEAR(123.0, merc_3d.z, 1e-12);

  PJ_COORD merc_4d = proj_coord(lon, lat, 123.0, 2020.5);
  EXPECT_TRUE(pj_fwd4d(merc_4d, merc.get()));
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_NEAR(merc_2d.x, merc_4d.xyz.x, 1e-9);
  EXPECT_NEAR(merc_2d.y, merc_4d.xyz.y, 1e-9);
  EXPECT_NEAR(123.0, merc_4d.xyz.z, 1e-12);
  EXPECT_NEAR(2020.5, merc_4d.xyzt.t, 1e-12);

  // 2. P->fwd3d + P->fwd without P->fwd4d (+proj=cart): pj_fwd3d invokes
  //    P->fwd3d and pj_fwd4d falls back to P->fwd3d.
  ScopedPj cart(proj_create(ctx.get(), "+proj=cart +ellps=WGS84"));
  ASSERT_NE(nullptr, cart.get());
  ASSERT_NE(nullptr, cart->fwd3d);
  EXPECT_EQ(nullptr, cart->fwd4d);

  const PJ_XYZ cart_3d =
      pj_fwd3d(proj_coord(0.0, 0.0, 100.0, 0.0).lpz, cart.get());
  EXPECT_EQ(0, proj_errno(cart.get()));
  EXPECT_NEAR(6378237.0, cart_3d.x, 1e-6);

  PJ_COORD cart_4d = proj_coord(0.0, 0.0, 100.0, 2021.0);
  EXPECT_TRUE(pj_fwd4d(cart_4d, cart.get()));
  EXPECT_EQ(0, proj_errno(cart.get()));
  EXPECT_NEAR(6378237.0, cart_4d.xyz.x, 1e-6);
  EXPECT_NEAR(2021.0, cart_4d.xyzt.t, 1e-12);

  // 3. P->fwd3d without P->fwd (+proj=helmert 3D): pj_fwd falls back to
  //    P->fwd3d.
  ScopedPj helmert(proj_create(ctx.get(), "+proj=helmert +x=10 +y=20 +z=30"));
  ASSERT_NE(nullptr, helmert.get());
  EXPECT_EQ(nullptr, helmert->fwd);
  ASSERT_NE(nullptr, helmert->fwd3d);

  const PJ_XY helmert_2d =
      pj_fwd(proj_coord(1.0, 2.0, 0.0, 0.0).lp, helmert.get());
  EXPECT_EQ(0, proj_errno(helmert.get()));
  EXPECT_NEAR(11.0, helmert_2d.x, 1e-12);
  EXPECT_NEAR(22.0, helmert_2d.y, 1e-12);

  // 4. P->fwd4d only (+proj=noop): pj_fwd and pj_fwd3d fall back to P->fwd4d,
  //    while pj_fwd4d invokes P->fwd4d directly.
  ScopedPj noop(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_NE(nullptr, noop.get());
  EXPECT_EQ(nullptr, noop->fwd);
  EXPECT_EQ(nullptr, noop->fwd3d);
  ASSERT_NE(nullptr, noop->fwd4d);

  const PJ_XY noop_2d = pj_fwd(proj_coord(3.0, 4.0, 0.0, 0.0).lp, noop.get());
  EXPECT_EQ(0, proj_errno(noop.get()));
  EXPECT_NEAR(3.0, noop_2d.x, 1e-12);
  EXPECT_NEAR(4.0, noop_2d.y, 1e-12);

  const PJ_XYZ noop_3d =
      pj_fwd3d(proj_coord(3.0, 4.0, 5.0, 0.0).lpz, noop.get());
  EXPECT_EQ(0, proj_errno(noop.get()));
  EXPECT_NEAR(5.0, noop_3d.z, 1e-12);

  PJ_COORD noop_4d = proj_coord(3.0, 4.0, 5.0, 6.0);
  EXPECT_TRUE(pj_fwd4d(noop_4d, noop.get()));
  EXPECT_EQ(0, proj_errno(noop.get()));
  EXPECT_NEAR(6.0, noop_4d.xyzt.t, 1e-12);

  // 5. CRS object without forward function pointers fails with
  //    PROJ_ERR_OTHER_NO_INVERSE_OP across pj_fwd, pj_fwd3d, and pj_fwd4d.
  ScopedPj crs(proj_create(ctx.get(), "EPSG:4326"));
  ASSERT_NE(nullptr, crs.get());
  EXPECT_EQ(nullptr, crs->fwd);
  EXPECT_EQ(nullptr, crs->fwd3d);
  EXPECT_EQ(nullptr, crs->fwd4d);

  const PJ_XY err_2d = pj_fwd(proj_coord(0.0, 0.0, 0.0, 0.0).lp, crs.get());
  EXPECT_TRUE(std::isinf(err_2d.x));
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(crs.get()));
  proj_errno_reset(crs.get());

  const PJ_XYZ err_3d = pj_fwd3d(proj_coord(0.0, 0.0, 0.0, 0.0).lpz, crs.get());
  EXPECT_TRUE(std::isinf(err_3d.x));
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(crs.get()));
  proj_errno_reset(crs.get());

  PJ_COORD err_4d = proj_coord(0.0, 0.0, 0.0, 0.0);
  EXPECT_FALSE(pj_fwd4d(err_4d, crs.get()));
  EXPECT_TRUE(std::isinf(err_4d.xyzt.x));
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(crs.get()));
}

TEST(FwdTest, SkipFwdPrepareAndFinalizeBypassPreparationAcross2d3d4d) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Values > 10 would trigger invalid-longitude/latitude errors in fwd_prepare
  // if skip_fwd_prepare were not set on axisswap.
  ScopedPj swap_2d(proj_create(ctx.get(), "+proj=axisswap +order=-1,2"));
  ASSERT_NE(nullptr, swap_2d.get());
  EXPECT_EQ(1, swap_2d->skip_fwd_prepare);
  EXPECT_EQ(1, swap_2d->skip_fwd_finalize);
  const PJ_XY r2 = pj_fwd(proj_coord(12.0, 20.0, 0.0, 0.0).lp, swap_2d.get());
  EXPECT_EQ(0, proj_errno(swap_2d.get()));
  EXPECT_NEAR(-12.0, r2.x, 1e-12);
  EXPECT_NEAR(20.0, r2.y, 1e-12);

  ScopedPj swap_3d(proj_create(ctx.get(), "+proj=axisswap +order=2,1,3"));
  ASSERT_NE(nullptr, swap_3d.get());
  EXPECT_EQ(1, swap_3d->skip_fwd_prepare);
  EXPECT_EQ(1, swap_3d->skip_fwd_finalize);
  const PJ_XYZ r3 =
      pj_fwd3d(proj_coord(12.0, 20.0, 30.0, 0.0).lpz, swap_3d.get());
  EXPECT_EQ(0, proj_errno(swap_3d.get()));
  EXPECT_NEAR(20.0, r3.x, 1e-12);
  EXPECT_NEAR(12.0, r3.y, 1e-12);
  EXPECT_NEAR(30.0, r3.z, 1e-12);

  ScopedPj swap_4d(proj_create(ctx.get(), "+proj=axisswap +order=2,1"));
  ASSERT_NE(nullptr, swap_4d.get());
  EXPECT_EQ(1, swap_4d->skip_fwd_prepare);
  EXPECT_EQ(1, swap_4d->skip_fwd_finalize);
  PJ_COORD c4 = proj_coord(12.0, 20.0, 30.0, 40.0);
  EXPECT_TRUE(pj_fwd4d(c4, swap_4d.get()));
  EXPECT_EQ(0, proj_errno(swap_4d.get()));
  EXPECT_NEAR(20.0, c4.xyzt.x, 1e-12);
  EXPECT_NEAR(12.0, c4.xyzt.y, 1e-12);
  EXPECT_NEAR(30.0, c4.xyzt.z, 1e-12);
  EXPECT_NEAR(40.0, c4.xyzt.t, 1e-12);
}

TEST(FwdTest, FwdPrepareAngularDomainClampingAndLongitudeWrapping) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj ll(proj_create(ctx.get(), "+proj=latlong +ellps=WGS84"));
  ASSERT_NE(nullptr, ll.get());
  ScopedPj ll_over(proj_create(ctx.get(), "+proj=latlong +ellps=WGS84 +over"));
  ASSERT_NE(nullptr, ll_over.get());

  // 1. HUGE_VAL in phi or lam when |phi| > pi/2 short-circuits without errno.
  const PJ_XY huge_phi =
      pj_fwd(proj_coord(0.0, HUGE_VAL, 0.0, 0.0).lp, ll.get());
  EXPECT_TRUE(std::isinf(huge_phi.x));
  EXPECT_EQ(0, proj_errno(ll.get()));

  const PJ_XY huge_lam_hi_phi =
      pj_fwd(proj_coord(HUGE_VAL, 2.0, 0.0, 0.0).lp, ll.get());
  EXPECT_TRUE(std::isinf(huge_lam_hi_phi.x));
  EXPECT_EQ(0, proj_errno(ll.get()));

  // 2. North latitude just above +pi/2 within PJ_EPS_LAT clamps to +pi/2.
  const PJ_XY clamped_pole =
      pj_fwd(proj_coord(0.1, kHalfPi + 5e-13, 0.0, 0.0).lp, ll.get());
  EXPECT_EQ(0, proj_errno(ll.get()));
  EXPECT_NEAR(0.1, clamped_pole.x, 1e-12);
  EXPECT_NEAR(kHalfPi, clamped_pole.y, 1e-15);

  // 3. Latitude beyond +-pi/2 + PJ_EPS_LAT fails with INVALID_COORD.
  for (const double bad_lat : {kHalfPi + 0.01, -kHalfPi - 0.01,
                               -std::numeric_limits<double>::infinity()}) {
    const PJ_XY bad = pj_fwd(proj_coord(0.0, bad_lat, 0.0, 0.0).lp, ll.get());
    EXPECT_TRUE(std::isinf(bad.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(ll.get()));
    proj_errno_reset(ll.get());
  }

  // 4. |lon| > 10 rad (including +-Inf) fails with INVALID_COORD.
  for (const double bad_lon :
       {10.5, -10.5, HUGE_VAL, -std::numeric_limits<double>::infinity()}) {
    const PJ_XY bad = pj_fwd(proj_coord(bad_lon, 0.0, 0.0, 0.0).lp, ll.get());
    EXPECT_TRUE(std::isinf(bad.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(ll.get()));
    proj_errno_reset(ll.get());
  }

  // 5. pi < |lon| <= 10 rad wraps via adjlon by default, preserved with +over.
  const PJ_XY wrapped = pj_fwd(proj_coord(4.0, 0.0, 0.0, 0.0).lp, ll.get());
  EXPECT_EQ(0, proj_errno(ll.get()));
  EXPECT_NEAR(4.0 - 2.0 * kPi, wrapped.x, 1e-12);

  const PJ_XY preserved =
      pj_fwd(proj_coord(4.0, 0.0, 0.0, 0.0).lp, ll_over.get());
  EXPECT_EQ(0, proj_errno(ll_over.get()));
  EXPECT_NEAR(4.0, preserved.x, 1e-12);

  // 6. HUGE_VAL in v[2] aborts angular 3D transformation without errno.
  const PJ_XYZ huge_z =
      pj_fwd3d(proj_coord(0.0, 0.0, HUGE_VAL, 0.0).lpz, ll.get());
  EXPECT_TRUE(std::isinf(huge_z.x));
  EXPECT_EQ(0, proj_errno(ll.get()));

  // 7. Post-greenwich and post-lon_0 subtraction triggers second adjlon wrap
  //    when P->over == 0 vs stays unwrapped when +over is active.
  ScopedPj merc_shift(
      proj_create(ctx.get(), "+proj=merc +R=6371000 +pm=paris +lon_0=150"));
  ASSERT_NE(nullptr, merc_shift.get());
  ScopedPj merc_shift_over(proj_create(
      ctx.get(), "+proj=merc +R=6371000 +pm=paris +lon_0=150 +over"));
  ASSERT_NE(nullptr, merc_shift_over.get());

  const PJ_XY shifted_wrapped =
      pj_fwd(proj_coord(DegToRad(-100.0), 0.0, 0.0, 0.0).lp, merc_shift.get());
  const PJ_XY shifted_unwrapped = pj_fwd(
      proj_coord(DegToRad(-100.0), 0.0, 0.0, 0.0).lp, merc_shift_over.get());
  EXPECT_EQ(0, proj_errno(merc_shift.get()));
  EXPECT_EQ(0, proj_errno(merc_shift_over.get()));
  EXPECT_GT(shifted_wrapped.x, 0.0);
  EXPECT_LT(shifted_unwrapped.x, 0.0);
}

TEST(FwdTest, FwdPrepareNonRadiansInputsAndCartesianHelmert) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. HUGE_VAL in v[0], v[1], or v[2] short-circuits non-radians preparation
  //    and triggers HUGE_VAL exit guards in pj_fwd, pj_fwd3d, and pj_fwd4d.
  ScopedPj noop(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_NE(nullptr, noop.get());

  const PJ_XY err_v0 =
      pj_fwd(proj_coord(HUGE_VAL, 1.0, 0.0, 0.0).lp, noop.get());
  EXPECT_TRUE(std::isinf(err_v0.x));

  const PJ_XY err_v1 =
      pj_fwd(proj_coord(1.0, HUGE_VAL, 0.0, 0.0).lp, noop.get());
  EXPECT_TRUE(std::isinf(err_v1.x));

  const PJ_XYZ err_3d_v2 =
      pj_fwd3d(proj_coord(1.0, 2.0, HUGE_VAL, 0.0).lpz, noop.get());
  EXPECT_TRUE(std::isinf(err_3d_v2.x));

  PJ_COORD err_4d_v2 = proj_coord(1.0, 2.0, HUGE_VAL, 0.0);
  EXPECT_FALSE(pj_fwd4d(err_4d_v2, noop.get()));
  EXPECT_TRUE(std::isinf(err_4d_v2.xyzt.x));

  // 2. INPUT_UNITS == PJ_IO_UNITS_CARTESIAN && P->helmert applies P->helmert
  //    in PJ_INV before invoking topocentric forward.
  ScopedPj topo(
      proj_create(ctx.get(),
                  "+proj=topocentric +lon_0=0 +lat_0=0 +h_0=0 +ellps=WGS84 "
                  "+towgs84=100,200,300"));
  ASSERT_NE(nullptr, topo.get());
  ASSERT_EQ(PJ_IO_UNITS_CARTESIAN, topo->left);
  ASSERT_NE(nullptr, topo->helmert);

  PJ_COORD c = proj_coord(6378137.0, 0.0, 0.0, 0.0);
  EXPECT_TRUE(pj_fwd4d(c, topo.get()));
  EXPECT_EQ(0, proj_errno(topo.get()));
  EXPECT_NEAR(-200.0, c.xyz.x, 1e-6);
  EXPECT_NEAR(-300.0, c.xyz.y, 1e-6);
  EXPECT_NEAR(-100.0, c.xyz.z, 1e-6);
}

TEST(FwdTest, FwdPrepareGeocHgridshiftHelmertCartAndVgridshift) {
  absl::Cleanup cleanup_globals = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  InMemoryFileSystem fs;
  constexpr float kGeoidShifts[] = {15.0f, 15.0f, 15.0f, 15.0f};
  fs.AddFile("/mem/geoid.gtx",
             BuildGtxGrid(-10.0, -10.0, 20.0, 20.0, 2, 2, kGeoidShifts));
  // CTable2 stores (lon_shift_west_pos_rad, lat_shift_rad) over [-0.2, 0.2].
  constexpr float kHShifts[] = {
      0.01f, 0.02f, 0.01f, 0.02f, 0.01f, 0.02f, 0.01f, 0.02f,
  };
  fs.AddFile("/mem/shift.ct2",
             BuildCTable2Grid(-0.2, -0.2, 0.4, 0.4, 2, 2, kHShifts));
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 1. P->geoc converts input geocentric latitude to geodetic latitude.
  ScopedPj geoc(proj_create(ctx.get(), "+proj=latlong +ellps=WGS84 +geoc"));
  ASSERT_NE(nullptr, geoc.get());
  const double lat45 = DegToRad(45.0);
  const PJ_XY geoc_out =
      pj_fwd(proj_coord(0.0, lat45, 0.0, 0.0).lp, geoc.get());
  EXPECT_EQ(0, proj_errno(geoc.get()));
  EXPECT_GT(geoc_out.y, lat45);

  // 2. P->hgridshift applies PJ_INV on in-bounds coordinates and aborts with
  //    HUGE_VAL on out-of-grid coordinates across 2D, 3D, and 4D entry points.
  ScopedPj hgrid(proj_create(
      ctx.get(), "+proj=latlong +ellps=WGS84 +nadgrids=/mem/shift.ct2"));
  ASSERT_NE(nullptr, hgrid.get());
  const PJ_XY hgrid_in = pj_fwd(proj_coord(0.0, 0.0, 0.0, 0.0).lp, hgrid.get());
  EXPECT_EQ(0, proj_errno(hgrid.get()));
  EXPECT_NEAR(0.01, hgrid_in.x, 1e-6);
  EXPECT_NEAR(-0.02, hgrid_in.y, 1e-6);

  const PJ_LP oob_lp = proj_coord(lat45, lat45, 0.0, 0.0).lp;
  EXPECT_TRUE(std::isinf(pj_fwd(oob_lp, hgrid.get()).x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(hgrid.get()));
  proj_errno_reset(hgrid.get());

  EXPECT_TRUE(std::isinf(
      pj_fwd3d(proj_coord(lat45, lat45, 0.0, 0.0).lpz, hgrid.get()).x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(hgrid.get()));
  proj_errno_reset(hgrid.get());

  PJ_COORD oob_4d = proj_coord(lat45, lat45, 0.0, 0.0);
  EXPECT_FALSE(pj_fwd4d(oob_4d, hgrid.get()));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(hgrid.get()));

  // 3. P->cart_wgs84 -> optional P->helmert -> P->cart path (with and without
  //    P->helmert via non-WGS84 ellipsoid).
  ScopedPj with_helmert(
      proj_create(ctx.get(), "+proj=latlong +ellps=intl +towgs84=100,200,300"));
  ASSERT_NE(nullptr, with_helmert.get());
  ASSERT_NE(nullptr, with_helmert->helmert);
  const PJ_XYZ shift_xyz =
      pj_fwd3d(proj_coord(0.0, lat45, 0.0, 0.0).lpz, with_helmert.get());
  EXPECT_EQ(0, proj_errno(with_helmert.get()));
  EXPECT_FALSE(std::isinf(shift_xyz.x));
  EXPECT_NE(lat45, shift_xyz.y);

  ScopedPj without_helmert(
      proj_create(ctx.get(), "+proj=latlong +ellps=intl +towgs84=0,0,0"));
  ASSERT_NE(nullptr, without_helmert.get());
  EXPECT_EQ(nullptr, without_helmert->helmert);
  ASSERT_NE(nullptr, without_helmert->cart_wgs84);
  ASSERT_NE(nullptr, without_helmert->cart);
  const PJ_XYZ ellps_only =
      pj_fwd3d(proj_coord(0.0, lat45, 0.0, 0.0).lpz, without_helmert.get());
  EXPECT_EQ(0, proj_errno(without_helmert.get()));
  EXPECT_FALSE(std::isinf(ellps_only.x));

  // 4. P->vgridshift subtracts geoid height via PJ_FWD during fwd_prepare
  //    (converting geometric/ellipsoidal height to orthometric height).
  ScopedPj vgrid(proj_create(
      ctx.get(), "+proj=latlong +ellps=WGS84 +geoidgrids=/mem/geoid.gtx"));
  ASSERT_NE(nullptr, vgrid.get());
  const PJ_XYZ vgrid_out =
      pj_fwd3d(proj_coord(0.0, 0.0, 100.0, 0.0).lpz, vgrid.get());
  EXPECT_EQ(0, proj_errno(vgrid.get()));
  EXPECT_NEAR(85.0, vgrid_out.z, 1e-4);
}

TEST(FwdTest, FwdFinalizeCoversAllOutputUnitsAndAxisswap) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. PJ_IO_UNITS_CARTESIAN with P->is_geocent == 1 vs == 0 and linear units.
  ScopedPj geocent(
      proj_create(ctx.get(), "+proj=geocent +ellps=WGS84 +units=km"));
  ASSERT_NE(nullptr, geocent.get());
  EXPECT_EQ(PJ_IO_UNITS_CARTESIAN, geocent->right);
  const PJ_XYZ geocent_xyz =
      pj_fwd3d(proj_coord(0.0, 0.0, 0.0, 0.0).lpz, geocent.get());
  EXPECT_EQ(0, proj_errno(geocent.get()));
  EXPECT_NEAR(6378.137, geocent_xyz.x, 1e-6);

  ScopedPj cart_km(proj_create(ctx.get(), "+proj=cart +ellps=WGS84 +units=km"));
  ASSERT_NE(nullptr, cart_km.get());
  EXPECT_EQ(PJ_IO_UNITS_CARTESIAN, cart_km->right);
  EXPECT_EQ(0, cart_km->is_geocent);
  const PJ_XYZ cart_xyz =
      pj_fwd3d(proj_coord(0.0, 0.0, 0.0, 0.0).lpz, cart_km.get());
  EXPECT_EQ(0, proj_errno(cart_km.get()));
  EXPECT_NEAR(6378.137, cart_xyz.x, 1e-6);

  // 2. PJ_IO_UNITS_CLASSIC (falling through into PJ_IO_UNITS_PROJECTED) on both
  //    spherical and ellipsoidal Mercator with false offsets and units.
  for (const char* def : {
           "+proj=merc +R=6371000 +x_0=1000 +y_0=2000 +z_0=50 +units=km "
           "+vunits=m",
           "+proj=merc +ellps=WGS84 +x_0=1000 +y_0=2000 +z_0=50 +units=km "
           "+vunits=m",
       }) {
    ScopedPj merc(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, merc.get());
    EXPECT_EQ(PJ_IO_UNITS_CLASSIC, merc->right);
    const PJ_XYZ out =
        pj_fwd3d(proj_coord(DegToRad(1.0), 0.0, 10.0, 0.0).lpz, merc.get());
    EXPECT_EQ(0, proj_errno(merc.get()));
    EXPECT_GT(out.x, 110.0);
    EXPECT_NEAR(2.0, out.y, 1e-6);
    EXPECT_NEAR(60.0, out.z, 1e-9);
  }

  // 3. PJ_IO_UNITS_PROJECTED directly (+proj=helmert +theta=0).
  ScopedPj helmert_proj(
      proj_create(ctx.get(),
                  "+proj=helmert +x=10 +y=20 +theta=0 +x_0=100 +y_0=200 +z_0=5 "
                  "+units=km +vunits=m"));
  ASSERT_NE(nullptr, helmert_proj.get());
  EXPECT_EQ(PJ_IO_UNITS_PROJECTED, helmert_proj->right);
  const PJ_XYZ hp_out =
      pj_fwd3d(proj_coord(1000.0, 2000.0, 10.0, 0.0).lpz, helmert_proj.get());
  EXPECT_EQ(0, proj_errno(helmert_proj.get()));
  EXPECT_NEAR(1.11, hp_out.x, 1e-9);
  EXPECT_NEAR(2.22, hp_out.y, 1e-9);
  EXPECT_NEAR(15.0, hp_out.z, 1e-9);

  // 4. PJ_IO_UNITS_DEGREES (unitconvert sets skip_fwd_prepare=1, finalize=0).
  ScopedPj rad_to_deg(
      proj_create(ctx.get(), "+proj=unitconvert +xy_in=rad +xy_out=deg"));
  ASSERT_NE(nullptr, rad_to_deg.get());
  EXPECT_EQ(PJ_IO_UNITS_DEGREES, rad_to_deg->right);
  EXPECT_EQ(0, rad_to_deg->skip_fwd_finalize);
  const PJ_XY deg_out =
      pj_fwd(proj_coord(kPi, kHalfPi / 2.0, 0.0, 0.0).lp, rad_to_deg.get());
  EXPECT_EQ(0, proj_errno(rad_to_deg.get()));
  EXPECT_NEAR(180.0, deg_out.x, 1e-9);
  EXPECT_NEAR(45.0, deg_out.y, 1e-9);

  // 5. PJ_IO_UNITS_RADIANS (+proj=latlong) applies vfr_meter/z0 and lon_wrap.
  ScopedPj ll_wrap(proj_create(
      ctx.get(),
      "+proj=latlong +ellps=WGS84 +z_0=10 +vunits=km +lon_wrap=180"));
  ASSERT_NE(nullptr, ll_wrap.get());
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, ll_wrap->right);
  const PJ_XYZ ll_out =
      pj_fwd3d(proj_coord(DegToRad(-90.0), DegToRad(10.0), 990.0, 0.0).lpz,
               ll_wrap.get());
  EXPECT_EQ(0, proj_errno(ll_wrap.get()));
  EXPECT_NEAR(DegToRad(270.0), ll_out.x, 1e-12);
  EXPECT_NEAR(DegToRad(10.0), ll_out.y, 1e-12);
  EXPECT_NEAR(1.0, ll_out.z, 1e-9);

  // 6. P->axisswap runs at the end of fwd_finalize (+axis=neu swaps E/N).
  ScopedPj merc_neu(
      proj_create(ctx.get(), "+proj=merc +ellps=WGS84 +axis=neu"));
  ASSERT_NE(nullptr, merc_neu.get());
  ASSERT_NE(nullptr, merc_neu->axisswap);
  const PJ_XY neu_out = pj_fwd(
      proj_coord(DegToRad(10.0), DegToRad(20.0), 0.0, 0.0).lp, merc_neu.get());
  EXPECT_EQ(0, proj_errno(merc_neu.get()));
  EXPECT_GT(neu_out.x, 2000000.0);  // northing in x
  EXPECT_LT(neu_out.y, 1200000.0);  // easting in y
}

TEST(FwdTest, PostTransformErrorsFiniteCoordWithErrnoAndErrnoPreservation) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Back-hemisphere ortho triggers HUGE_VAL post-transform exit across
  //    pj_fwd, pj_fwd3d, and pj_fwd4d on both sphere and ellipsoid.
  for (const char* def :
       {"+proj=ortho +R=6371000", "+proj=ortho +ellps=WGS84"}) {
    ScopedPj ortho(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, ortho.get());

    const PJ_LP back_lp = proj_coord(kPi, 0.0, 0.0, 0.0).lp;
    EXPECT_TRUE(std::isinf(pj_fwd(back_lp, ortho.get()).x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(ortho.get()));
    proj_errno_reset(ortho.get());

    EXPECT_TRUE(std::isinf(
        pj_fwd3d(proj_coord(kPi, 0.0, 0.0, 0.0).lpz, ortho.get()).x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(ortho.get()));
    proj_errno_reset(ortho.get());

    PJ_COORD back_4d = proj_coord(kPi, 0.0, 0.0, 0.0);
    EXPECT_FALSE(pj_fwd4d(back_4d, ortho.get()));
    EXPECT_TRUE(std::isinf(back_4d.xyzt.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(ortho.get()));
  }

  // 2. Transverse Central Cylindrical at (lon=pi/2, lat=0) sets errno while
  //    returning {0, 0} (!= HUGE_VAL), exercising error_or_coord's
  //    last_errno check (pj_fwd, pj_fwd3d) and pj_fwd4d's tail errno check.
  ScopedPj tcc(proj_create(ctx.get(), "+proj=tcc +R=6371000"));
  ASSERT_NE(nullptr, tcc.get());

  EXPECT_TRUE(
      std::isinf(pj_fwd(proj_coord(kHalfPi, 0.0, 0.0, 0.0).lp, tcc.get()).x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(tcc.get()));
  proj_errno_reset(tcc.get());

  EXPECT_TRUE(std::isinf(
      pj_fwd3d(proj_coord(kHalfPi, 0.0, 0.0, 0.0).lpz, tcc.get()).x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(tcc.get()));
  proj_errno_reset(tcc.get());

  PJ_COORD tcc_4d = proj_coord(kHalfPi, 0.0, 0.0, 0.0);
  EXPECT_FALSE(pj_fwd4d(tcc_4d, tcc.get()));
  EXPECT_TRUE(std::isinf(tcc_4d.xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(tcc.get()));

  // 3. Prior non-zero last_errno is preserved on successful pj_fwd, pj_fwd3d,
  //    and pj_fwd4d calls.
  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, merc.get());
  proj_errno_set(merc.get(), PROJ_ERR_OTHER_API_MISUSE);
  const PJ_XY p2 = pj_fwd(proj_coord(0.0, 0.0, 0.0, 0.0).lp, merc.get());
  EXPECT_NEAR(0.0, p2.x, 1e-12);
  EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_errno(merc.get()));

  const PJ_XYZ p3 = pj_fwd3d(proj_coord(0.0, 0.0, 7.0, 0.0).lpz, merc.get());
  EXPECT_NEAR(7.0, p3.z, 1e-12);
  EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_errno(merc.get()));

  PJ_COORD p4 = proj_coord(0.0, 0.0, 7.0, 8.0);
  EXPECT_TRUE(pj_fwd4d(p4, merc.get()));
  EXPECT_NEAR(8.0, p4.xyzt.t, 1e-12);
  EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_errno(merc.get()));
  proj_errno_reset(merc.get());

  // 4. NaN inputs, proj_trans_array (success and failure), and proj_factors.
  const double nan_val = std::numeric_limits<double>::quiet_NaN();
  const PJ_XY direct_nan =
      pj_fwd(proj_coord(nan_val, 0.0, 0.0, 0.0).lp, merc.get());
  EXPECT_TRUE(std::isnan(direct_nan.x));
  EXPECT_EQ(0, proj_errno(merc.get()));

  const PJ_COORD trans_nan =
      proj_trans(merc.get(), PJ_FWD, proj_coord(0.0, nan_val, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(trans_nan.xyzt.x));
  EXPECT_TRUE(std::isnan(trans_nan.xyzt.y));

  PJ_COORD batch[] = {
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(DegToRad(10.0), DegToRad(20.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(merc.get(), PJ_FWD, 2, batch));
  EXPECT_NEAR(0.0, batch[0].xy.x, 1e-12);
  EXPECT_GT(batch[1].xy.x, 1000000.0);

  PJ_COORD bad_batch[] = {
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(0.0, kHalfPi + 0.1, 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD,
            proj_trans_array(merc.get(), PJ_FWD, 2, bad_batch));
  EXPECT_NEAR(0.0, bad_batch[0].xy.x, 1e-12);
  EXPECT_TRUE(std::isinf(bad_batch[1].xy.x));
  proj_errno_reset(merc.get());

  ScopedPj merc_sphere(proj_create(ctx.get(), "+proj=merc +R=6371000"));
  ASSERT_NE(nullptr, merc_sphere.get());
  const PJ_FACTORS f_sphere = proj_factors(
      merc_sphere.get(), proj_coord(0.0, DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc_sphere.get()));
  EXPECT_NEAR(std::sqrt(2.0), f_sphere.meridional_scale, 1e-9);
  EXPECT_NEAR(std::sqrt(2.0), f_sphere.parallel_scale, 1e-9);

  const PJ_FACTORS f_ellps =
      proj_factors(merc.get(), proj_coord(0.0, DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_NEAR(f_ellps.meridional_scale, f_ellps.parallel_scale, 1e-9);
}

// TODO(schwehr): Remaining uncovered lines in
// third_party/proj4/proj/src/fwd.cpp are unreachable branches: (1) line 67
// (`coo.lp.phi = -M_HALFPI`) due to the upstream typo on line 61 (`coo.lp.phi -
// M_HALFPI < -PJ_EPS_LAT` is always true when `coo.lp.phi < -M_HALFPI`), (2)
// line 108 (`coo.lp.lam == HUGE_VAL` after `P->cart` `PJ_INV`, which computes
// finite `atan2` on finite cartesian intermediates), and (3) false branch of
// line 178 (`coo.lpz.lam != HUGE_VAL` inside `fwd_finalize`, where `HUGE_VAL ==
// coo.v[0]` has already returned).

}  // namespace
}  // namespace proj
