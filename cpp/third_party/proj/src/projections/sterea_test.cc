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
// Unit tests for the Oblique Stereographic Alternative (sterea) projection
// (proj/src/projections/sterea.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/sterea.cpp

#include <array>
#include <cmath>
#include <numbers>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(StereaTest, MetadataAndDutchRdNewAmersfoortReferencePoint) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Dutch RD New (EPSG:28992) uses +proj=sterea centered on Amersfoort
  // (52.15616055555555N, 5.38763888888889E) with Bessel 1841 ellipsoid.
  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=sterea +lat_0=52.15616055555555 +lon_0=5.38763888888889 "
      "+k=0.9999079 +x_0=155000 +y_0=463000 +ellps=bessel"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("sterea", info.id);
  EXPECT_NE(
      std::string_view(pj->descr).find("Oblique Stereographic Alternative"),
      std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  // 1. At the projection center (Amersfoort), forward projection yields exact
  //    false easting (155000 m) and false northing (463000 m), and inverse
  //    hits the rho == 0.0 branch in sterea_e_inverse (lp.phi = Q->phic0,
  //    lp.lam = 0).
  const double lon0 = DegToRad(5.38763888888889);
  const double lat0 = DegToRad(52.15616055555555);
  const PJ_COORD center_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lon0, lat0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(155000.0, center_fwd.xy.x, 1e-6);
  EXPECT_NEAR(463000.0, center_fwd.xy.y, 1e-6);

  const PJ_COORD center_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(155000.0, 463000.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lon0, center_inv.lp.lam, 1e-12);
  EXPECT_NEAR(lat0, center_inv.lp.phi, 1e-12);

  // 2. Non-origin points across the Netherlands (rho != 0.0 branch in inverse).
  struct PointCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr PointCase kPoints[] = {
      {.lon_deg = 4.8952, .lat_deg = 52.3702},  // Amsterdam
      {.lon_deg = 4.4777, .lat_deg = 51.9244},  // Rotterdam
      {.lon_deg = 6.5665, .lat_deg = 53.2194},  // Groningen
      {.lon_deg = 5.6910, .lat_deg = 50.8514},  // Maastricht
  };

  for (const auto& pt : kPoints) {
    const double lon = DegToRad(pt.lon_deg);
    const double lat = DegToRad(pt.lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon, lat, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lon, inv.lp.lam, 1e-11);
    EXPECT_NEAR(lat, inv.lp.phi, 1e-11);
  }
}

TEST(StereaTest, SphericalRoundTripAndAntipodalSingularityDenomZero) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());

  // On a unit sphere (+R=1) with origin at (0, 0), Q->phic0 = 0, Q->sinc0 = 0,
  // Q->cosc0 = 1, Q->R2 = 2.
  ScopedPj pj(
      proj_create(ctx.get(), "+proj=sterea +lat_0=0 +lon_0=0 +k=1 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Normal spherical point (45E, 30N) round-trips accurately.
  const PJ_COORD fwd = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(45.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(DegToRad(45.0), inv.lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(30.0), inv.lp.phi, 1e-12);

  // 2. At the antipode of the projection center (lon = pi, lat = 0),
  //    sinc = 0, cosc = 1, cosl = cos(pi) = -1, so
  //    denom = 1 + 0 + 1 * 1 * (-1) == 0.0 exactly, hitting the
  //    denom == 0.0 error branch in sterea_e_forward.
  const PJ_COORD antipode_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kPi, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, antipode_fwd.xy.x);
  EXPECT_EQ(HUGE_VAL, antipode_fwd.xy.y);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(StereaTest, DestructorNullGuardsAndGaussInitFailureBranch) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());

  // 1. Verify custom destructor handles nullptr safely.
  ScopedPj pj(proj_create(ctx.get(), "+proj=sterea +lat_0=45 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  ASSERT_NE(nullptr, pj->destructor);
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));

  // 2. When es is extremely close to 1 (+es=0.9999999999999999) and lat_0=45,
  //    pj_gauss_ini's srat_val underflows to 0.0 and returns nullptr,
  //    exercising the `if (nullptr == Q->en)` cleanup path in
  //    PJ_PROJECTION(sterea).
  ScopedPj fail_pj(proj_create(
      ctx.get(), "+proj=sterea +lat_0=45 +a=1 +es=0.9999999999999999"));
  EXPECT_EQ(nullptr, fail_pj.get());
  EXPECT_EQ(PROJ_ERR_OTHER, proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);
}

TEST(StereaTest, Classic2dEntryPointsArrayTransformAndConformalFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(
      ctx.get(), "+proj=sterea +lat_0=52 +lon_0=20 +k=0.9998 +ellps=GRS80"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP lp = {.lam = DegToRad(21.0), .phi = DegToRad(52.23)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, inv_lp.lam, 1e-11);
  EXPECT_NEAR(lp.phi, inv_lp.phi, 1e-11);

  auto coords = std::to_array<PJ_COORD>({
      proj_coord(DegToRad(20.0), DegToRad(52.0), 0.0, 0.0),
      proj_coord(DegToRad(19.0), DegToRad(50.5), 0.0, 0.0),
  });
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(20.0), coords[0].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(52.0), coords[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(19.0), coords[1].lp.lam, 1e-11);
  EXPECT_NEAR(DegToRad(50.5), coords[1].lp.phi, 1e-11);

  // sterea is conformal via the Gauss conformal sphere: at the origin,
  // meridional_scale == parallel_scale == k0 (0.9998) and angular_distortion ==
  // 0.
  const PJ_FACTORS origin_factors = proj_factors(
      pj.get(), proj_coord(DegToRad(20.0), DegToRad(52.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.9998, origin_factors.meridional_scale, 1e-6);
  EXPECT_NEAR(0.9998, origin_factors.parallel_scale, 1e-6);
  EXPECT_NEAR(0.0, origin_factors.angular_distortion, 1e-6);
}

}  // namespace
}  // namespace proj
