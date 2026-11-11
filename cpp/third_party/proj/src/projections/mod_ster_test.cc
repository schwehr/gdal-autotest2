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
// Unit tests for the Modified Stereographic projections: mil_os (Miller
// Oblated Stereographic), lee_os (Lee Oblated Stereographic), gs48 (48 U.S.),
// alsk (Alaska), and gs50 (50 U.S.) (proj/src/projections/mod_ster.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/mod_ster.cpp

#include <array>
#include <cmath>
#include <limits>
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

TEST(ModSterTest, MetadataHardcodedOriginsAndEllipsoidOverrides) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Spherical-only variants force P->es = 0 even with +ellps=WGS84, and
  // override user +lon_0/+lat_0 with their built-in projection origins.
  struct SphericalVariant {
    const char* id;
    const char* proj_str;
    double expected_lam0_deg;
    double expected_phi0_deg;
  };
  constexpr SphericalVariant kSphericalVariants[] = {
      {.id = "mil_os",
       .proj_str = "+proj=mil_os +ellps=WGS84",
       .expected_lam0_deg = 20.0,
       .expected_phi0_deg = 18.0},
      {.id = "lee_os",
       .proj_str = "+proj=lee_os +ellps=WGS84",
       .expected_lam0_deg = -165.0,
       .expected_phi0_deg = -10.0},
      {.id = "gs48",
       .proj_str = "+proj=gs48 +R=6370997",
       .expected_lam0_deg = -96.0,
       .expected_phi0_deg = 39.0},
  };

  for (const auto& v : kSphericalVariants) {
    ScopedPj pj(proj_create(ctx.get(), v.proj_str));
    ASSERT_NE(nullptr, pj.get()) << v.id;
    const PJ_PROJ_INFO info = proj_pj_info(pj.get());
    EXPECT_STREQ(v.id, info.id);
    EXPECT_EQ(1, info.has_inverse);
    EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD));
    EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD));
    EXPECT_EQ(0.0, pj->es) << v.id;
    EXPECT_NEAR(DegToRad(v.expected_lam0_deg), pj->lam0, 1e-12) << v.id;
    EXPECT_NEAR(DegToRad(v.expected_phi0_deg), pj->phi0, 1e-12) << v.id;
    EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));
  }

  // gs48 specifically sets P->a = 6370997.
  ScopedPj gs48_def(proj_create(ctx.get(), "+proj=gs48"));
  ASSERT_NE(nullptr, gs48_def.get());
  EXPECT_EQ(6370997.0, gs48_def->a);
  EXPECT_EQ(0.0, gs48_def->es);

  // 2. alsk and gs50 support both Clarke 1866 ellipsoidal (es != 0 ->
  // a=6378206.4, es=0.00676866) and spherical (es == 0 -> a=6370997) modes.
  ScopedPj alsk_ell(proj_create(ctx.get(), "+proj=alsk +ellps=clrk66"));
  ScopedPj alsk_sph(proj_create(ctx.get(), "+proj=alsk +R=6370997"));
  ScopedPj gs50_ell(proj_create(ctx.get(), "+proj=gs50 +ellps=clrk66"));
  ScopedPj gs50_sph(proj_create(ctx.get(), "+proj=gs50 +R=6370997"));
  ASSERT_NE(nullptr, alsk_ell.get());
  ASSERT_NE(nullptr, alsk_sph.get());
  ASSERT_NE(nullptr, gs50_ell.get());
  ASSERT_NE(nullptr, gs50_sph.get());

  EXPECT_EQ(6378206.4, alsk_ell->a);
  EXPECT_DOUBLE_EQ(0.00676866, alsk_ell->es);
  EXPECT_EQ(6370997.0, alsk_sph->a);
  EXPECT_EQ(0.0, alsk_sph->es);

  EXPECT_EQ(6378206.4, gs50_ell->a);
  EXPECT_DOUBLE_EQ(0.00676866, gs50_ell->es);
  EXPECT_EQ(6370997.0, gs50_sph->a);
  EXPECT_EQ(0.0, gs50_sph->es);
}

TEST(ModSterTest, OriginExactZeroShortcutAndRoundTripAllFiveProjections) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Use matching +R=6370997 / +ellps=clrk66 so core P->ra matches the
  // projection's overridden P->a for exact meter round-trips.
  struct TestVariant {
    const char* proj_str;
    double origin_lon_deg;
    double origin_lat_deg;
    double test_lon_deg;
    double test_lat_deg;
  };
  constexpr TestVariant kVariants[] = {
      {.proj_str = "+proj=mil_os +R=6371000",
       .origin_lon_deg = 20.0,
       .origin_lat_deg = 18.0,
       .test_lon_deg = 5.0,
       .test_lat_deg = 32.0},
      {.proj_str = "+proj=lee_os +R=6371000",
       .origin_lon_deg = -165.0,
       .origin_lat_deg = -10.0,
       .test_lon_deg = 175.0,
       .test_lat_deg = -35.0},
      {.proj_str = "+proj=gs48 +R=6370997",
       .origin_lon_deg = -96.0,
       .origin_lat_deg = 39.0,
       .test_lon_deg = -80.0,
       .test_lat_deg = 35.0},
      {.proj_str = "+proj=alsk +R=6370997",
       .origin_lon_deg = -152.0,
       .origin_lat_deg = 64.0,
       .test_lon_deg = -140.0,
       .test_lat_deg = 60.0},
      {.proj_str = "+proj=alsk +ellps=clrk66",
       .origin_lon_deg = -152.0,
       .origin_lat_deg = 64.0,
       .test_lon_deg = -165.0,
       .test_lat_deg = 56.0},
      {.proj_str = "+proj=gs50 +R=6370997",
       .origin_lon_deg = -120.0,
       .origin_lat_deg = 45.0,
       .test_lon_deg = -105.0,
       .test_lat_deg = 40.0},
      {.proj_str = "+proj=gs50 +ellps=clrk66",
       .origin_lon_deg = -120.0,
       .origin_lat_deg = 45.0,
       .test_lon_deg = -150.0,
       .test_lat_deg = 62.0},
  };

  for (const auto& v : kVariants) {
    ScopedPj pj(proj_create(ctx.get(), v.proj_str));
    ASSERT_NE(nullptr, pj.get()) << v.proj_str;

    // 1. Exact projection origin (lam0, phi0) maps to (0, 0), and inverse at
    // (0, 0) exercises the `fabs(rh) <= EPSLN` shortcut returning (lam0, phi0).
    const PJ_COORD fwd_origin =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(v.origin_lon_deg),
                              DegToRad(v.origin_lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << v.proj_str;
    EXPECT_NEAR(0.0, fwd_origin.xy.x, 1e-8) << v.proj_str;
    EXPECT_NEAR(0.0, fwd_origin.xy.y, 1e-8) << v.proj_str;

    const PJ_COORD inv_origin =
        proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << v.proj_str;
    EXPECT_NEAR(DegToRad(v.origin_lon_deg), inv_origin.lp.lam, 1e-12)
        << v.proj_str;
    EXPECT_NEAR(DegToRad(v.origin_lat_deg), inv_origin.lp.phi, 1e-12)
        << v.proj_str;

    // 2. Round-trip off-origin regional coordinate.
    const double lon_rad = DegToRad(v.test_lon_deg);
    const double lat_rad = DegToRad(v.test_lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << v.proj_str;
    EXPECT_FALSE(std::isinf(fwd.xy.x)) << v.proj_str;
    EXPECT_FALSE(std::isinf(fwd.xy.y)) << v.proj_str;

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << v.proj_str;
    EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-10) << v.proj_str;
    EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-10) << v.proj_str;
  }

  // 3. Ellipsoidal vs spherical coefficients (ABe vs ABs) differ distinctly
  // away from origin, and WGS84 input also enters the ellipsoidal branch.
  ScopedPj gs50_wgs84(proj_create(ctx.get(), "+proj=gs50 +ellps=WGS84"));
  ScopedPj gs50_sph(proj_create(ctx.get(), "+proj=gs50 +R=6370997"));
  ASSERT_NE(nullptr, gs50_wgs84.get());
  ASSERT_NE(nullptr, gs50_sph.get());
  EXPECT_DOUBLE_EQ(0.00676866, gs50_wgs84->es);
  const PJ_COORD sample =
      proj_coord(DegToRad(-155.0), DegToRad(20.0), 0.0, 0.0);
  const PJ_COORD out_ell = proj_trans(gs50_wgs84.get(), PJ_FWD, sample);
  const PJ_COORD out_sph = proj_trans(gs50_sph.get(), PJ_FWD, sample);
  EXPECT_EQ(0, proj_errno(gs50_wgs84.get()));
  EXPECT_EQ(0, proj_errno(gs50_sph.get()));
  EXPECT_GT(
      std::hypot(out_ell.xy.x - out_sph.xy.x, out_ell.xy.y - out_sph.xy.y),
      1000.0);
}

TEST(ModSterTest, PolesAntipodeSingularityAndInverseNonConvergence) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. North Pole round-trips cleanly on ellipsoidal alsk, while South Pole is
  // outside the regional modified stereographic domain.
  ScopedPj alsk(proj_create(ctx.get(), "+proj=alsk +ellps=clrk66"));
  ASSERT_NE(nullptr, alsk.get());
  {
    const PJ_COORD fwd_np =
        proj_trans(alsk.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(alsk.get()));
    const PJ_COORD inv_np = proj_trans(alsk.get(), PJ_INV, fwd_np);
    EXPECT_EQ(0, proj_errno(alsk.get()));
    EXPECT_NEAR(kHalfPi, inv_np.lp.phi, 1e-9);

    const PJ_COORD fwd_sp =
        proj_trans(alsk.get(), PJ_FWD, proj_coord(0.0, -kHalfPi, 0.0, 0.0));
    const PJ_COORD inv_sp = proj_trans(alsk.get(), PJ_INV, fwd_sp);
    EXPECT_TRUE(std::isinf(inv_sp.lp.phi));
    proj_errno_reset(alsk.get());
  }

  // 2. Antipodal point of gs50 origin (lam = -120 + 180 = +60 deg, phi = -45
  // deg) hits denom == 0 (or ~1e-16 under FMA contraction) in forward
  // transform.
  ScopedPj gs50_sph(proj_create(ctx.get(), "+proj=gs50 +R=6370997"));
  ASSERT_NE(nullptr, gs50_sph.get());
  const PJ_COORD antipode =
      proj_trans(gs50_sph.get(), PJ_FWD,
                 proj_coord(DegToRad(60.0), DegToRad(-45.0), 0.0, 0.0));
  if (std::isinf(antipode.xy.x)) {
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(gs50_sph.get()));
  } else {
    EXPECT_GT(std::hypot(antipode.xy.x, antipode.xy.y), 1e20);
  }
  proj_errno_reset(gs50_sph.get());

  // 3. Large out-of-domain coordinates (1e15, 0) m prevent complex polynomial
  // Newton iteration from converging in 20 steps (nn == 0 -> HUGE_VAL) across
  // all 5 projection degrees (n = 2, 4, 5, 9).
  constexpr const char* kAllProjections[] = {
      "+proj=mil_os +R=6371000",  "+proj=lee_os +R=6371000",
      "+proj=gs48 +R=6370997",    "+proj=alsk +ellps=clrk66",
      "+proj=gs50 +ellps=clrk66",
  };
  for (const char* def : kAllProjections) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    const PJ_COORD inv_fail =
        proj_trans(pj.get(), PJ_INV, proj_coord(1e15, 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(inv_fail.lp.lam)) << def;
    EXPECT_TRUE(std::isinf(inv_fail.lp.phi)) << def;
    proj_errno_reset(pj.get());
  }
}

TEST(ModSterTest, FactorsLinearUnitsBatchAndSpecialValues) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Complex polynomial conformal mapping preserves conformality
  // (meridional_scale == parallel_scale, zero angular distortion).
  ScopedPj gs50(proj_create(ctx.get(), "+proj=gs50 +ellps=clrk66"));
  ASSERT_NE(nullptr, gs50.get());
  const PJ_FACTORS fac = proj_factors(
      gs50.get(), proj_coord(DegToRad(-110.0), DegToRad(40.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(gs50.get()));
  EXPECT_NEAR(fac.meridional_scale, fac.parallel_scale, 1e-6);
  EXPECT_NEAR(0.0, fac.angular_distortion, 1e-6);

  // 2. False easting/northing, +units=km, and 4D (z, t) pass-through.
  // Note: +units=km without +vunits=... sets both horizontal fr_meter and
  // vertical vfr_meter to 0.001, so fwd_km.xyzt.z is scaled to km (0.8765) and
  // inv_km.lpzt.z is scaled back to meters (876.5).
  ScopedPj alsk_km(proj_create(
      ctx.get(), "+proj=alsk +ellps=clrk66 +x_0=500000 +y_0=200000 +units=km"));
  ASSERT_NE(nullptr, alsk_km.get());
  const PJ_COORD pt_4d =
      proj_coord(DegToRad(-148.0), DegToRad(62.0), 876.5, 2026.5);
  const PJ_COORD fwd_km = proj_trans(alsk_km.get(), PJ_FWD, pt_4d);
  EXPECT_EQ(0, proj_errno(alsk_km.get()));
  EXPECT_NEAR(0.8765, fwd_km.xyzt.z, 1e-12);
  EXPECT_NEAR(2026.5, fwd_km.xyzt.t, 1e-12);
  const PJ_COORD inv_km = proj_trans(alsk_km.get(), PJ_INV, fwd_km);
  EXPECT_EQ(0, proj_errno(alsk_km.get()));
  EXPECT_NEAR(pt_4d.lpzt.lam, inv_km.lpzt.lam, 1e-10);
  EXPECT_NEAR(pt_4d.lpzt.phi, inv_km.lpzt.phi, 1e-10);
  EXPECT_NEAR(876.5, inv_km.lpzt.z, 1e-12);
  EXPECT_NEAR(2026.5, inv_km.lpzt.t, 1e-12);

  // 3. Batch transform via proj_trans_array (0 == success) and
  // proj_trans_generic.
  std::array<PJ_COORD, 2> batch = {
      proj_coord(DegToRad(-120.0), DegToRad(45.0), 0.0, 0.0),
      proj_coord(DegToRad(-100.0), DegToRad(35.0), 0.0, 0.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(gs50.get(), PJ_FWD, batch.size(), batch.data()));
  ASSERT_EQ(0,
            proj_trans_array(gs50.get(), PJ_INV, batch.size(), batch.data()));
  EXPECT_NEAR(DegToRad(-120.0), batch[0].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(45.0), batch[0].lp.phi, 1e-10);
  EXPECT_NEAR(DegToRad(-100.0), batch[1].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(35.0), batch[1].lp.phi, 1e-10);

  ScopedPj mil_os(proj_create(ctx.get(), "+proj=mil_os +R=6371000"));
  ASSERT_NE(nullptr, mil_os.get());
  std::array<double, 2> gx = {DegToRad(15.0), DegToRad(25.0)};
  std::array<double, 2> gy = {DegToRad(10.0), DegToRad(25.0)};
  EXPECT_EQ(2u,
            proj_trans_generic(mil_os.get(), PJ_FWD, gx.data(), sizeof(double),
                               gx.size(), gy.data(), sizeof(double), gy.size(),
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(mil_os.get()));
  EXPECT_EQ(2u,
            proj_trans_generic(mil_os.get(), PJ_INV, gx.data(), sizeof(double),
                               gx.size(), gy.data(), sizeof(double), gy.size(),
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(mil_os.get()));
  EXPECT_NEAR(DegToRad(15.0), gx[0], 1e-10);
  EXPECT_NEAR(DegToRad(10.0), gy[0], 1e-10);
  EXPECT_NEAR(DegToRad(25.0), gx[1], 1e-10);
  EXPECT_NEAR(DegToRad(25.0), gy[1], 1e-10);

  // 4. NaN and HUGE_VAL edge inputs.
  ScopedLogSuppressor suppress(ctx.get());
  const double kNan = std::numeric_limits<double>::quiet_NaN();
  const PJ_COORD nan_res =
      proj_trans(gs50.get(), PJ_FWD, proj_coord(kNan, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_res.xy.x));
  EXPECT_TRUE(std::isnan(nan_res.xy.y));

  const PJ_COORD inf_res =
      proj_trans(gs50.get(), PJ_FWD, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_res.xy.x));
  proj_errno_reset(gs50.get());
}

// TODO(schwehr): Unreachable lines in proj/src/projections/mod_ster.cpp without
// fault injection or upstream code changes:
// - OOM null checks on calloc in PJ_PROJECTION(mil_os), PJ_PROJECTION(lee_os),
//   PJ_PROJECTION(gs48), PJ_PROJECTION(alsk), and PJ_PROJECTION(gs50) (lines
//   137-138, 157-158, 177-178, 205-206, 241-242).

}  // namespace
}  // namespace proj
