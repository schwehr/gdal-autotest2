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
// Unit tests for PROJ inverse operation preparation, dispatch, and
// finalization (proj/src/inv.cpp). Uses an in-memory PROJ_FILE_API for
// grid-shift branches without external testdata or disk I/O.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/inv.cpp

#include <array>
#include <cmath>
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
constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(InvTest, RejectsHugeValInputsAndNormalizesMissingHelmertTime) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, merc.get());

  // 1. HUGE_VAL in x, y, or z immediately triggers domain error in inv_prepare.
  EXPECT_EQ(HUGE_VAL, pj_inv({HUGE_VAL, 0.0}, merc.get()).lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(merc.get()));
  proj_errno_reset(merc.get());

  EXPECT_EQ(HUGE_VAL, pj_inv({0.0, HUGE_VAL}, merc.get()).lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(merc.get()));
  proj_errno_reset(merc.get());

  EXPECT_EQ(HUGE_VAL, pj_inv3d({0.0, 0.0, HUGE_VAL}, merc.get()).lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(merc.get()));
  proj_errno_reset(merc.get());

  PJ_COORD err4d = {{HUGE_VAL, 0.0, 0.0, 0.0}};
  EXPECT_FALSE(pj_inv4d(err4d, merc.get()));
  EXPECT_EQ(HUGE_VAL, err4d.v[0]);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(merc.get()));
  proj_errno_reset(merc.get());

  // 2. When P->helmert is present, missing time (HUGE_VAL in v[3]) is coerced
  // to 0.0 in inv_prepare rather than failing inside the Helmert step.
  ScopedPj helmert_proj(
      proj_create(ctx.get(), "+proj=merc +ellps=intl +towgs84=10,20,30"));
  ASSERT_NE(nullptr, helmert_proj.get());
  PJ_COORD coo = {{1000.0, 2000.0, 0.0, HUGE_VAL}};
  EXPECT_TRUE(pj_inv4d(coo, helmert_proj.get()));
  EXPECT_EQ(0, proj_errno(helmert_proj.get()));
  EXPECT_DOUBLE_EQ(0.0, coo.xyzt.t);
}

TEST(InvTest, PrepareHandlesAxisSwapAndAllInputUnitModes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. P->axisswap + PJ_IO_UNITS_CLASSIC (+to_meter, +x_0, +y_0, * P->ra).
  ScopedPj neu_merc(
      proj_create(ctx.get(),
                  "+proj=merc +R=6371000 +x_0=100 +y_0=200 +to_meter=1000 "
                  "+axis=neu"));
  ASSERT_NE(nullptr, neu_merc.get());
  const PJ_COORD in_rad = proj_coord(DegToRad(8.0), DegToRad(15.0), 0.0, 0.0);
  const PJ_COORD fwd_neu = proj_trans(neu_merc.get(), PJ_FWD, in_rad);
  EXPECT_EQ(0, proj_errno(neu_merc.get()));
  PJ_COORD inv_neu = fwd_neu;
  EXPECT_TRUE(pj_inv4d(inv_neu, neu_merc.get()));
  EXPECT_EQ(0, proj_errno(neu_merc.get()));
  EXPECT_NEAR(in_rad.lp.lam, inv_neu.lp.lam, 1e-12);
  EXPECT_NEAR(in_rad.lp.phi, inv_neu.lp.phi, 1e-12);

  // 2. PJ_IO_UNITS_WHATEVER (noop).
  ScopedPj noop(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_NE(nullptr, noop.get());
  const PJ_LPZ noop_out = pj_inv3d({1.25, -2.5, 3.75}, noop.get());
  EXPECT_EQ(0, proj_errno(noop.get()));
  EXPECT_DOUBLE_EQ(1.25, noop_out.lam);
  EXPECT_DOUBLE_EQ(-2.5, noop_out.phi);
  EXPECT_DOUBLE_EQ(3.75, noop_out.z);

  // 3. PJ_IO_UNITS_DEGREES (unitconvert sets skip_inv_prepare = 1 by default;
  // also exercise inv_prepare's DEGREES case directly).
  ScopedPj unitconv(
      proj_create(ctx.get(), "+proj=unitconvert +xy_in=rad +xy_out=deg"));
  ASSERT_NE(nullptr, unitconv.get());
  PJ_COORD deg_in = proj_coord(15.0, -30.0, 0.0, 0.0);
  EXPECT_TRUE(pj_inv4d(deg_in, unitconv.get()));
  EXPECT_EQ(0, proj_errno(unitconv.get()));
  EXPECT_NEAR(DegToRad(15.0), deg_in.lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-30.0), deg_in.lp.phi, 1e-12);

  unitconv->skip_inv_prepare = 0;
  PJ_COORD deg_in_prep = proj_coord(15.0, -30.0, 0.0, 0.0);
  EXPECT_TRUE(pj_inv4d(deg_in_prep, unitconv.get()));
  EXPECT_EQ(0, proj_errno(unitconv.get()));
  EXPECT_NEAR(DegToRad(15.0), deg_in_prep.lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-30.0), deg_in_prep.lp.phi, 1e-12);

  // 4. PJ_IO_UNITS_CARTESIAN with is_geocent == 0 (cart) and is_geocent == 1
  // (geocent), both scaled in kilometers (+to_meter=1000).
  const PJ_COORD geo_pt =
      proj_coord(DegToRad(12.0), DegToRad(55.0), 250.0, 0.0);
  ScopedPj cart_km(
      proj_create(ctx.get(), "+proj=cart +ellps=WGS84 +to_meter=1000"));
  ASSERT_NE(nullptr, cart_km.get());
  const PJ_COORD cart_fwd = proj_trans(cart_km.get(), PJ_FWD, geo_pt);
  EXPECT_EQ(0, proj_errno(cart_km.get()));
  const PJ_LPZ cart_inv = pj_inv3d(cart_fwd.xyz, cart_km.get());
  EXPECT_EQ(0, proj_errno(cart_km.get()));
  EXPECT_NEAR(geo_pt.lpz.lam, cart_inv.lam, 1e-12);
  EXPECT_NEAR(geo_pt.lpz.phi, cart_inv.phi, 1e-12);
  EXPECT_NEAR(geo_pt.lpz.z, cart_inv.z, 1e-5);

  ScopedPj geocent_km(
      proj_create(ctx.get(), "+proj=geocent +ellps=WGS84 +to_meter=1000"));
  ASSERT_NE(nullptr, geocent_km.get());
  const PJ_COORD geocent_fwd = proj_trans(geocent_km.get(), PJ_FWD, geo_pt);
  EXPECT_EQ(0, proj_errno(geocent_km.get()));
  const PJ_LPZ geocent_inv = pj_inv3d(geocent_fwd.xyz, geocent_km.get());
  EXPECT_EQ(0, proj_errno(geocent_km.get()));
  EXPECT_NEAR(geo_pt.lpz.lam, geocent_inv.lam, 1e-12);
  EXPECT_NEAR(geo_pt.lpz.phi, geocent_inv.phi, 1e-12);
  EXPECT_NEAR(geo_pt.lpz.z, geocent_inv.z, 1e-5);

  // 5. PJ_IO_UNITS_PROJECTED (helmert with +theta uses projected left/right
  // units and must skip P->ra scaling).
  ScopedPj helmert_proj2d(proj_create(
      ctx.get(),
      "+proj=helmert +x=10 +y=20 +theta=0 +convention=position_vector "
      "+x_0=100 +y_0=200 +z_0=5 +to_meter=1000 +vto_meter=10"));
  ASSERT_NE(nullptr, helmert_proj2d.get());
  const PJ_COORD h_in = proj_coord(500.0, -300.0, 20.0, 0.0);
  const PJ_COORD h_fwd = proj_trans(helmert_proj2d.get(), PJ_FWD, h_in);
  EXPECT_EQ(0, proj_errno(helmert_proj2d.get()));
  PJ_COORD h_inv = h_fwd;
  EXPECT_TRUE(pj_inv4d(h_inv, helmert_proj2d.get()));
  EXPECT_EQ(0, proj_errno(helmert_proj2d.get()));
  EXPECT_NEAR(h_in.xyz.x, h_inv.xyz.x, 1e-9);
  EXPECT_NEAR(h_in.xyz.y, h_inv.xyz.y, 1e-9);
  EXPECT_NEAR(h_in.xyz.z, h_inv.xyz.z, 1e-9);

  // 6. PJ_IO_UNITS_RADIANS (+vto_meter and +z_0 on vertical offset).
  ScopedPj voff(
      proj_create(ctx.get(),
                  "+proj=vertoffset +ellps=WGS84 +lat_0=45 +lon_0=10 +dh=5 "
                  "+z_0=20 +vto_meter=2"));
  ASSERT_NE(nullptr, voff.get());
  const PJ_COORD v_in = proj_coord(DegToRad(10.0), DegToRad(45.0), 100.0, 0.0);
  const PJ_COORD v_fwd = proj_trans(voff.get(), PJ_FWD, v_in);
  EXPECT_EQ(0, proj_errno(voff.get()));
  const PJ_LPZ v_inv = pj_inv3d(v_fwd.xyz, voff.get());
  EXPECT_EQ(0, proj_errno(voff.get()));
  EXPECT_NEAR(v_in.lpz.lam, v_inv.lam, 1e-12);
  EXPECT_NEAR(v_in.lpz.phi, v_inv.phi, 1e-12);
  EXPECT_NEAR(v_in.lpz.z, v_inv.z, 1e-6);
}

TEST(InvTest, FinalizePrimeMeridianOverFlagGeocAndDatumShifts) {
  InMemoryFileSystem fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  absl::Cleanup clear_caches = [] { proj_cleanup(); };

  // 1. Central meridian + prime meridian (+lon_0 + +pm=paris).
  ScopedPj pm_merc(
      proj_create(ctx.get(), "+proj=merc +R=6371000 +lon_0=10 +pm=paris"));
  ASSERT_NE(nullptr, pm_merc.get());
  const PJ_LP pm_lp = pj_inv({0.0, 0.0}, pm_merc.get());
  EXPECT_EQ(0, proj_errno(pm_merc.get()));
  const double expected_paris_plus_10_rad =
      DegToRad(10.0 + 2.0 + 20.0 / 60.0 + 14.025 / 3600.0);
  EXPECT_NEAR(expected_paris_plus_10_rad, pm_lp.lam, 1e-12);

  // 2. P->over == 0 wraps via adjlon() while P->over == 1 skips wrapping.
  ScopedPj wrap_merc(
      proj_create(ctx.get(), "+proj=merc +R=6371000 +lon_0=170"));
  ScopedPj over_merc(
      proj_create(ctx.get(), "+proj=merc +R=6371000 +lon_0=170 +over"));
  ASSERT_NE(nullptr, wrap_merc.get());
  ASSERT_NE(nullptr, over_merc.get());
  const double x_30_east_of_cm = 6371000.0 * DegToRad(30.0);
  const PJ_LP wrapped = pj_inv({x_30_east_of_cm, 0.0}, wrap_merc.get());
  const PJ_LP unwrapped = pj_inv({x_30_east_of_cm, 0.0}, over_merc.get());
  EXPECT_EQ(0, proj_errno(wrap_merc.get()));
  EXPECT_EQ(0, proj_errno(over_merc.get()));
  EXPECT_NEAR(DegToRad(-160.0), wrapped.lam, 1e-12);
  EXPECT_NEAR(DegToRad(200.0), unwrapped.lam, 1e-12);

  // 3. P->geoc geocentric latitude flag round-trips geocentric inputs.
  ScopedPj geoc_merc(proj_create(ctx.get(), "+proj=merc +ellps=WGS84 +geoc"));
  ASSERT_NE(nullptr, geoc_merc.get());
  const PJ_COORD geoc_in = proj_coord(DegToRad(15.0), DegToRad(45.0), 0.0, 0.0);
  const PJ_COORD geoc_fwd = proj_trans(geoc_merc.get(), PJ_FWD, geoc_in);
  EXPECT_EQ(0, proj_errno(geoc_merc.get()));
  const PJ_LP geoc_inv = pj_inv(geoc_fwd.xy, geoc_merc.get());
  EXPECT_EQ(0, proj_errno(geoc_merc.get()));
  EXPECT_NEAR(geoc_in.lp.lam, geoc_inv.lam, 1e-12);
  EXPECT_NEAR(geoc_in.lp.phi, geoc_inv.phi, 1e-12);

  // 4. Non-zero +towgs84 (P->helmert != nullptr) and null +towgs84 on non-WGS84
  // ellipsoid (P->helmert == nullptr, P->cart_wgs84 != nullptr && P->cart !=
  // nullptr).
  ScopedPj helmert_nz(
      proj_create(ctx.get(), "+proj=merc +ellps=intl +towgs84=100,200,300"));
  ScopedPj helmert_zero(
      proj_create(ctx.get(), "+proj=merc +ellps=intl +towgs84=0,0,0"));
  ASSERT_NE(nullptr, helmert_nz.get());
  ASSERT_NE(nullptr, helmert_zero.get());
  EXPECT_NE(nullptr, helmert_nz->helmert);
  EXPECT_EQ(nullptr, helmert_zero->helmert);
  EXPECT_NE(nullptr, helmert_zero->cart);
  EXPECT_NE(nullptr, helmert_zero->cart_wgs84);

  const PJ_COORD datum_in =
      proj_coord(DegToRad(10.0), DegToRad(50.0), 50.0, 0.0);
  for (PJ* pj : {helmert_nz.get(), helmert_zero.get()}) {
    const PJ_COORD fwd = proj_trans(pj, PJ_FWD, datum_in);
    EXPECT_EQ(0, proj_errno(pj));
    PJ_COORD inv = fwd;
    EXPECT_TRUE(pj_inv4d(inv, pj));
    EXPECT_EQ(0, proj_errno(pj));
    EXPECT_NEAR(datum_in.lpz.lam, inv.lpz.lam, 1e-10);
    EXPECT_NEAR(datum_in.lpz.phi, inv.lpz.phi, 1e-10);
    EXPECT_NEAR(datum_in.lpz.z, inv.lpz.z, 1e-4);
  }

  // 5. P->vgridshift and P->hgridshift using synthetic in-memory grids, both
  // inside grid bounds (round-trip) and outside bounds (early error return).
  ASSERT_TRUE(fs.Install(ctx.get()));
  constexpr std::array<float, 9> kVShifts = {12.5f, 12.5f, 12.5f, 12.5f, 12.5f,
                                             12.5f, 12.5f, 12.5f, 12.5f};
  fs.AddFile("/mem/inv_vgrid.gtx",
             BuildGtxGrid(/*west_deg=*/5.0, /*south_deg=*/45.0,
                          /*res_x_deg=*/1.0, /*res_y_deg=*/1.0, /*rows=*/3,
                          /*cols=*/3, kVShifts));
  constexpr std::array<float, 18> kHShifts = {
      0.0001f, 0.0002f, 0.0001f, 0.0002f, 0.0001f, 0.0002f,
      0.0001f, 0.0002f, 0.0001f, 0.0002f, 0.0001f, 0.0002f,
      0.0001f, 0.0002f, 0.0001f, 0.0002f, 0.0001f, 0.0002f};
  fs.AddFile("/mem/inv_hgrid.ct2",
             BuildCTable2Grid(/*west_rad=*/DegToRad(5.0),
                              /*south_rad=*/DegToRad(45.0),
                              /*res_x_rad=*/DegToRad(1.0),
                              /*res_y_rad=*/DegToRad(1.0), /*cols=*/3,
                              /*rows=*/3, kHShifts));

  ScopedPj vgrid_merc(proj_create(ctx.get(),
                                  "+proj=merc +ellps=WGS84 "
                                  "+geoidgrids=/mem/inv_vgrid.gtx"));
  ScopedPj hgrid_merc(proj_create(ctx.get(),
                                  "+proj=merc +ellps=WGS84 "
                                  "+nadgrids=/mem/inv_hgrid.ct2"));
  ASSERT_NE(nullptr, vgrid_merc.get());
  ASSERT_NE(nullptr, hgrid_merc.get());

  const PJ_COORD inside_grid =
      proj_coord(DegToRad(6.0), DegToRad(46.0), 100.0, 0.0);
  const PJ_COORD v_fwd = proj_trans(vgrid_merc.get(), PJ_FWD, inside_grid);
  EXPECT_EQ(0, proj_errno(vgrid_merc.get()));
  const PJ_LPZ v_inv = pj_inv3d(v_fwd.xyz, vgrid_merc.get());
  EXPECT_EQ(0, proj_errno(vgrid_merc.get()));
  EXPECT_NEAR(inside_grid.lpz.lam, v_inv.lam, 1e-12);
  EXPECT_NEAR(inside_grid.lpz.phi, v_inv.phi, 1e-12);
  EXPECT_NEAR(inside_grid.lpz.z, v_inv.z, 1e-5);

  const PJ_COORD h_fwd = proj_trans(hgrid_merc.get(), PJ_FWD, inside_grid);
  EXPECT_EQ(0, proj_errno(hgrid_merc.get()));
  const PJ_LP h_inv = pj_inv(h_fwd.xy, hgrid_merc.get());
  EXPECT_EQ(0, proj_errno(hgrid_merc.get()));
  EXPECT_NEAR(inside_grid.lp.lam, h_inv.lam, 1e-10);
  EXPECT_NEAR(inside_grid.lp.phi, h_inv.phi, 1e-10);

  ScopedLogSuppressor suppress(ctx.get());
  const double outside_x = 6378137.0 * DegToRad(40.0);
  const PJ_LPZ v_out = pj_inv3d({outside_x, 0.0, 0.0}, vgrid_merc.get());
  EXPECT_EQ(HUGE_VAL, v_out.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(vgrid_merc.get()));
  proj_errno_reset(vgrid_merc.get());

  // Also check pj_inv4d error handling when inv_finalize triggers a grid error.
  PJ_COORD v_out_4d = proj_coord(outside_x, 0.0, 0.0, 0.0);
  EXPECT_FALSE(pj_inv4d(v_out_4d, vgrid_merc.get()));
  EXPECT_EQ(HUGE_VAL, v_out_4d.v[0]);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(vgrid_merc.get()));
  proj_errno_reset(vgrid_merc.get());

  const PJ_LP h_out = pj_inv({outside_x, 0.0}, hgrid_merc.get());
  EXPECT_EQ(HUGE_VAL, h_out.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(hgrid_merc.get()));
  proj_errno_reset(hgrid_merc.get());
}

TEST(InvTest, DispatchDimensionalTiersAndErrorHandling) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. P->inv only (+proj=merc): tests 2D direct, 3D->2D fallback, 4D->2D.
  ScopedPj only_2d(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, only_2d.get());
  const PJ_LP p2 = pj_inv({0.0, 0.0}, only_2d.get());
  const PJ_LPZ p3 = pj_inv3d({0.0, 0.0, 12.0}, only_2d.get());
  PJ_COORD p4 = proj_coord(0.0, 0.0, 12.0, 2020.0);
  EXPECT_TRUE(pj_inv4d(p4, only_2d.get()));
  EXPECT_EQ(0, proj_errno(only_2d.get()));
  EXPECT_DOUBLE_EQ(0.0, p2.lam);
  EXPECT_DOUBLE_EQ(12.0, p3.z);
  EXPECT_DOUBLE_EQ(2020.0, p4.xyzt.t);

  // 2. P->inv3d only (+proj=vertoffset): tests 2D->3D fallback, 3D direct,
  // 4D->3D fallback.
  ScopedPj only_3d(
      proj_create(ctx.get(), "+proj=vertoffset +ellps=WGS84 +dh=5"));
  ASSERT_NE(nullptr, only_3d.get());
  const PJ_LP v2 = pj_inv({0.5, -0.25}, only_3d.get());
  EXPECT_NEAR(0.5, v2.lam, 1e-12);
  EXPECT_NEAR(-0.25, v2.phi, 1e-12);
  EXPECT_NEAR(5.0, pj_inv3d({0.0, 0.0, 10.0}, only_3d.get()).z, 1e-6);
  PJ_COORD v4 = proj_coord(0.0, 0.0, 10.0, 0.0);
  EXPECT_TRUE(pj_inv4d(v4, only_3d.get()));
  EXPECT_NEAR(5.0, v4.lpz.z, 1e-6);

  // 3. P->inv4d only (+proj=geoc): tests 2D->4D and 3D->4D fallbacks + 4D.
  ScopedPj only_4d(proj_create(ctx.get(), "+proj=geoc +ellps=WGS84"));
  ASSERT_NE(nullptr, only_4d.get());
  const double phi = DegToRad(30.0);
  EXPECT_NEAR(pj_inv({0.0, phi}, only_4d.get()).phi,
              pj_inv3d({0.0, phi, 0.0}, only_4d.get()).phi, 1e-12);
  PJ_COORD g4 = proj_coord(0.0, phi, 0.0, 0.0);
  EXPECT_TRUE(pj_inv4d(g4, only_4d.get()));
  EXPECT_EQ(0, proj_errno(only_4d.get()));

  // 4. Non-invertible projection (+proj=august): all three report NO_INVERSE.
  ScopedPj no_inv(proj_create(ctx.get(), "+proj=august +R=6371000"));
  ASSERT_NE(nullptr, no_inv.get());
  EXPECT_EQ(HUGE_VAL, pj_inv({0.0, 0.0}, no_inv.get()).lam);
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(no_inv.get()));
  proj_errno_reset(no_inv.get());

  EXPECT_EQ(HUGE_VAL, pj_inv3d({0.0, 0.0, 0.0}, no_inv.get()).lam);
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(no_inv.get()));
  proj_errno_reset(no_inv.get());

  PJ_COORD no_inv4 = {{0.0, 0.0, 0.0, 0.0}};
  EXPECT_FALSE(pj_inv4d(no_inv4, no_inv.get()));
  EXPECT_EQ(HUGE_VAL, no_inv4.v[0]);
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(no_inv.get()));
  proj_errno_reset(no_inv.get());

  // 5. Operator inverse returning HUGE_VAL in 2D, 3D, and 4D (also exercises
  // skip_inv_prepare and skip_inv_finalize via pipeline).
  ScopedPj ortho(proj_create(ctx.get(), "+proj=ortho +R=6371000"));
  ASSERT_NE(nullptr, ortho.get());
  EXPECT_EQ(HUGE_VAL, pj_inv({1e8, 1e8}, ortho.get()).lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(ortho.get()));
  proj_errno_reset(ortho.get());

  ScopedPj pipe_ortho(
      proj_create(ctx.get(), "+proj=pipeline +step +proj=ortho +R=6371000"));
  ASSERT_NE(nullptr, pipe_ortho.get());
  EXPECT_EQ(HUGE_VAL, pj_inv3d({1e8, 1e8, 0.0}, pipe_ortho.get()).lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pipe_ortho.get()));
  proj_errno_reset(pipe_ortho.get());

  PJ_COORD pipe_err4 = {{1e8, 1e8, 0.0, 0.0}};
  EXPECT_FALSE(pj_inv4d(pipe_err4, pipe_ortho.get()));
  EXPECT_EQ(HUGE_VAL, pipe_err4.v[0]);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pipe_ortho.get()));
  proj_errno_reset(pipe_ortho.get());

  // Also verify pipeline round-trip with skip_inv_prepare / skip_inv_finalize
  // for 2D/3D/4D succeeds.
  ScopedPj pipe_merc(
      proj_create(ctx.get(), "+proj=pipeline +step +proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, pipe_merc.get());
  EXPECT_DOUBLE_EQ(0.0, pj_inv({0.0, 0.0}, pipe_merc.get()).lam);
  EXPECT_DOUBLE_EQ(0.0, pj_inv3d({0.0, 0.0, 5.0}, pipe_merc.get()).lam);
  PJ_COORD pm4 = proj_coord(0.0, 0.0, 5.0, 2020.0);
  EXPECT_TRUE(pj_inv4d(pm4, pipe_merc.get()));
  EXPECT_EQ(0, proj_errno(pipe_merc.get()));
  EXPECT_DOUBLE_EQ(0.0, pm4.lp.lam);

  // 6. Pre-existing errno is preserved across successful pj_inv/3d/4d calls.
  proj_errno_set(only_2d.get(), PROJ_ERR_OTHER_API_MISUSE);
  (void)pj_inv({0.0, 0.0}, only_2d.get());
  EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_errno(only_2d.get()));
  (void)pj_inv3d({0.0, 0.0, 0.0}, only_2d.get());
  EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_errno(only_2d.get()));
  PJ_COORD ok4 = proj_coord(0.0, 0.0, 0.0, 0.0);
  EXPECT_TRUE(pj_inv4d(ok4, only_2d.get()));
  EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_errno(only_2d.get()));
  proj_errno_reset(only_2d.get());
}

}  // namespace
}  // namespace proj

// Coverage notes (proj/src/inv.cpp):
// - Line 49-50 (`HUGE_VAL == coo.v[2] && P->helmert` in inv_prepare) is
//   shadowed by line 40 (`coo.v[2] == HUGE_VAL` returns earlier).
// - Line 104-105 (`coo.xyz.x == HUGE_VAL` in inv_finalize) is shadowed because
//   pj_inv/pj_inv3d/pj_inv4d return before inv_finalize when coo.v[0] ==
//   HUGE_VAL.
