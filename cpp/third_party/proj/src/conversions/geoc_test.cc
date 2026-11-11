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
// Unit tests for geodetic <-> geocentric latitude conversion and
// pj_geocentric_latitude() (proj/src/conversions/geoc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/conversions/geoc.cpp

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

TEST(GeocTest, EllipsoidalForwardInverseRoundTripAndAnalyticalValues) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=geoc +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj_left(pj.get()));
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj_right(pj.get()));
  EXPECT_EQ(1, pj->is_latlong);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  // 1. Equator: geocentric latitude equals geodetic latitude (0 rad).
  const PJ_COORD eq = proj_trans(pj.get(), PJ_FWD,
                                 proj_coord(DegToRad(25.0), 0.0, 12.5, 2026.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(DegToRad(25.0), eq.lpzt.lam);
  EXPECT_DOUBLE_EQ(0.0, eq.lpzt.phi);
  EXPECT_DOUBLE_EQ(12.5, eq.lpzt.z);
  EXPECT_DOUBLE_EQ(2026.0, eq.lpzt.t);

  // 2. Closed-form check at +/-45 deg where tan(phi) = +/-1, so
  //    psi = atan(+/- (1 - e^2)) with WGS84 f = 1 / 298.257223563.
  constexpr double kWgs84F = 1.0 / 298.257223563;
  const double one_minus_es = (1.0 - kWgs84F) * (1.0 - kWgs84F);
  const double expected_45 = std::atan(one_minus_es);

  const PJ_COORD fwd_45 =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, kPi / 4.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(expected_45, fwd_45.lp.phi, 1e-13);
  EXPECT_LT(fwd_45.lp.phi, kPi / 4.0);

  // 3. Round-trip across hemispheres and latitudes near (but outside) pole cap.
  constexpr double kLatitudesDeg[] = {-89.9, -60.0, -45.0, -15.0, 15.0,
                                      30.0,  45.0,  60.0,  89.9};
  for (const double lat_deg : kLatitudesDeg) {
    const double lon = DegToRad(-123.45);
    const double lat = DegToRad(lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon, lat, 321.0, 1999.5));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(lon, fwd.lpzt.lam);
    EXPECT_DOUBLE_EQ(321.0, fwd.lpzt.z);
    EXPECT_DOUBLE_EQ(1999.5, fwd.lpzt.t);

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lon, inv.lpzt.lam, 1e-13);
    EXPECT_NEAR(lat, inv.lpzt.phi, 1e-13);
    EXPECT_DOUBLE_EQ(321.0, inv.lpzt.z);
    EXPECT_DOUBLE_EQ(1999.5, inv.lpzt.t);
  }
}

TEST(GeocTest, NearPoleLimitCopiesLatitudeDirectly) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=geoc +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Inside the +/- (pi/2 - 1e-9) boundary, tangent conversion runs.
  const double inside_lat = kHalfPi - 1e-5;
  const PJ_COORD inside_fwd = pj_geocentric_latitude(
      pj.get(), PJ_FWD, proj_coord(0.3, inside_lat, 0.0, 0.0));
  EXPECT_NE(inside_lat, inside_fwd.lp.phi);
  EXPECT_LT(inside_fwd.lp.phi, inside_lat);
  const PJ_COORD inside_inv =
      pj_geocentric_latitude(pj.get(), PJ_INV, inside_fwd);
  EXPECT_NEAR(inside_lat, inside_inv.lp.phi, 1e-13);

  // 2. Outside the +/- (pi/2 - 1e-9) cap (both direct helper and proj_trans),
  //    the input latitude is returned verbatim to avoid tan(pi/2) blowup.
  constexpr double kPolarLats[] = {kHalfPi - 5e-10, kHalfPi, -(kHalfPi - 5e-10),
                                   -kHalfPi};
  for (const double lat : kPolarLats) {
    const PJ_COORD direct_fwd = pj_geocentric_latitude(
        pj.get(), PJ_FWD, proj_coord(0.5, lat, 10.0, 20.0));
    const PJ_COORD direct_inv = pj_geocentric_latitude(
        pj.get(), PJ_INV, proj_coord(0.5, lat, 10.0, 20.0));
    EXPECT_DOUBLE_EQ(lat, direct_fwd.lpzt.phi);
    EXPECT_DOUBLE_EQ(lat, direct_inv.lpzt.phi);

    const PJ_COORD trans_fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.5, lat, 10.0, 20.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(lat, trans_fwd.lpzt.phi);

    const PJ_COORD trans_inv =
        proj_trans(pj.get(), PJ_INV, proj_coord(0.5, lat, 10.0, 20.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(lat, trans_inv.lpzt.phi);
  }
}

TEST(GeocTest, SphericalEllipsoidIsExactIdentity) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=geoc +R=6371000"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_DOUBLE_EQ(0.0, pj->es);

  constexpr double kLatitudesDeg[] = {-60.0, -30.0, 0.0, 45.0, 80.0};
  for (const double lat_deg : kLatitudesDeg) {
    const PJ_COORD in =
        proj_coord(DegToRad(12.0), DegToRad(lat_deg), 50.0, 2025.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, in);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(in.lpzt.phi, fwd.lpzt.phi);
    EXPECT_DOUBLE_EQ(in.lpzt.phi, inv.lpzt.phi);
  }
}

TEST(GeocTest, GeocModifierOnProjectionMatchesExplicitGeocConversion) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj geoc(proj_create(ctx.get(), "+proj=geoc +ellps=WGS84"));
  ScopedPj merc_geog(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ScopedPj merc_geoc(proj_create(ctx.get(), "+proj=merc +ellps=WGS84 +geoc"));
  ASSERT_NE(nullptr, geoc.get());
  ASSERT_NE(nullptr, merc_geog.get());
  ASSERT_NE(nullptr, merc_geoc.get());

  // Geographic (lon, lat) -> Mercator via +proj=merc must match converting
  // geographic -> geocentric latitude first and projecting with +geoc.
  const PJ_COORD geog_pt = proj_coord(DegToRad(15.0), DegToRad(50.0), 0.0, 0.0);
  const PJ_COORD geoc_pt = proj_trans(geoc.get(), PJ_FWD, geog_pt);
  EXPECT_EQ(0, proj_errno(geoc.get()));

  const PJ_COORD xy_direct = proj_trans(merc_geog.get(), PJ_FWD, geog_pt);
  const PJ_COORD xy_via_geoc = proj_trans(merc_geoc.get(), PJ_FWD, geoc_pt);
  EXPECT_EQ(0, proj_errno(merc_geog.get()));
  EXPECT_EQ(0, proj_errno(merc_geoc.get()));
  EXPECT_NEAR(xy_direct.xy.x, xy_via_geoc.xy.x, 1e-6);
  EXPECT_NEAR(xy_direct.xy.y, xy_via_geoc.xy.y, 1e-6);

  const PJ_COORD back_geoc = proj_trans(merc_geoc.get(), PJ_INV, xy_via_geoc);
  EXPECT_EQ(0, proj_errno(merc_geoc.get()));
  EXPECT_NEAR(geoc_pt.lp.phi, back_geoc.lp.phi, 1e-12);
}

TEST(GeocTest, BatchGenericLegacyAndErrorPaths) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=geoc +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Batch proj_trans_array round-trip.
  std::array<PJ_COORD, 2> arr = {
      proj_coord(DegToRad(10.0), DegToRad(20.0), 0.0, 0.0),
      proj_coord(DegToRad(-40.0), DegToRad(-55.0), 100.0, 2022.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, arr.size(), arr.data()));
  EXPECT_LT(arr[0].lp.phi, DegToRad(20.0));
  EXPECT_GT(arr[1].lp.phi, DegToRad(-55.0));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, arr.size(), arr.data()));
  EXPECT_NEAR(DegToRad(20.0), arr[0].lp.phi, 1e-13);
  EXPECT_NEAR(DegToRad(-55.0), arr[1].lp.phi, 1e-13);

  // 2. proj_trans_generic and classic 2D pj_fwd / pj_inv.
  std::array<double, 2> lams = {0.1, -0.2};
  std::array<double, 2> phis = {DegToRad(35.0), DegToRad(-35.0)};
  EXPECT_EQ(2u,
            proj_trans_generic(pj.get(), PJ_FWD, lams.data(), sizeof(double),
                               lams.size(), phis.data(), sizeof(double),
                               phis.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-phis[0], phis[1], 1e-14);

  const PJ_LP in_lp = {.lam = DegToRad(12.0), .phi = DegToRad(48.0)};
  const PJ_XY fwd_xy = pj_fwd(in_lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP inv_lp = pj_inv(fwd_xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_lp.lam, inv_lp.lam, 1e-13);
  EXPECT_NEAR(in_lp.phi, inv_lp.phi, 1e-13);

  // 3. Domain errors in fwd_prepare set INVALID_COORD + HUGE_VAL.
  const PJ_COORD bad_lat =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, kHalfPi + 1e-4, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(bad_lat.lp.phi));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  const PJ_COORD bad_lon =
      proj_trans(pj.get(), PJ_FWD, proj_coord(11.0, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(bad_lon.lp.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 4. NaN propagates cleanly and proj_factors rejects fwd4d-only operation.
  const PJ_COORD nan_out = proj_trans(
      pj.get(), PJ_FWD,
      proj_coord(0.0, std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.lp.phi));
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_FACTORS fac = proj_factors(pj.get(), proj_coord(0.0, 0.5, 0.0, 0.0));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, fac.meridional_scale);
  proj_errno_reset(pj.get());
}

}  // namespace
}  // namespace proj
