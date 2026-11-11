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
// Unit tests for legacy `+proj=geocent` conversion stub and its integration
// with PROJ cartesian/3D transforms (proj/src/conversions/geocent.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/conversions/geocent.cpp

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

TEST(GeocentTest, SetupFlagsAndZeroedFalseOffsets) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. +proj=geocent requires no ellipsoid (need_ellps = 0, defaults to GRS80)
  //    and explicitly resets x0/y0 to 0.0 even when +x_0/+y_0 are provided.
  ScopedPj pj(proj_create(ctx.get(), "+proj=geocent +x_0=500 +y_0=600"));
  ASSERT_NE(nullptr, pj.get());

  EXPECT_EQ(1, pj->is_geocent);
  EXPECT_DOUBLE_EQ(0.0, pj->x0);
  EXPECT_DOUBLE_EQ(0.0, pj->y0);
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj_left(pj.get()));
  EXPECT_EQ(PJ_IO_UNITS_CARTESIAN, pj_right(pj.get()));
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
}

TEST(GeocentTest, EllipsoidalAndSpherical3DRoundTripMatchesCart) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj geocent_wgs84(proj_create(ctx.get(), "+proj=geocent +ellps=WGS84"));
  ScopedPj cart_wgs84(proj_create(ctx.get(), "+proj=cart +ellps=WGS84"));
  ASSERT_NE(nullptr, geocent_wgs84.get());
  ASSERT_NE(nullptr, cart_wgs84.get());

  struct Case3D {
    double lon_rad;
    double lat_rad;
    double height_m;
  };
  constexpr Case3D kCases[] = {
      {.lon_rad = 0.0, .lat_rad = 0.0, .height_m = 0.0},
      {.lon_rad = kHalfPi, .lat_rad = 0.0, .height_m = 250.0},
      {.lon_rad = DegToRad(12.0),
       .lat_rad = DegToRad(55.0),
       .height_m = 123.45},
      {.lon_rad = DegToRad(-122.0),
       .lat_rad = DegToRad(-33.5),
       .height_m = 2500.0},
      {.lon_rad = 0.0, .lat_rad = kHalfPi, .height_m = 100.0},
      {.lon_rad = 0.0, .lat_rad = -kHalfPi, .height_m = 50.0},
  };

  for (const auto& tc : kCases) {
    const PJ_COORD input =
        proj_coord(tc.lon_rad, tc.lat_rad, tc.height_m, 2026.25);

    // 1. Forward output matches +proj=cart and preserves t.
    const PJ_COORD fwd = proj_trans(geocent_wgs84.get(), PJ_FWD, input);
    const PJ_COORD ref = proj_trans(cart_wgs84.get(), PJ_FWD, input);
    EXPECT_EQ(0, proj_errno(geocent_wgs84.get()));
    EXPECT_EQ(0, proj_errno(cart_wgs84.get()));
    EXPECT_NEAR(ref.xyz.x, fwd.xyz.x, 1e-7);
    EXPECT_NEAR(ref.xyz.y, fwd.xyz.y, 1e-7);
    EXPECT_NEAR(ref.xyz.z, fwd.xyz.z, 1e-7);
    EXPECT_DOUBLE_EQ(2026.25, fwd.xyzt.t);

    // 2. Inverse recovers geodetic (lon, lat, h) and preserves t.
    const PJ_COORD inv = proj_trans(geocent_wgs84.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(geocent_wgs84.get()));
    EXPECT_NEAR(tc.lat_rad, inv.lpz.phi, 1e-12);
    EXPECT_NEAR(tc.height_m, inv.lpz.z, 1e-5);
    EXPECT_DOUBLE_EQ(2026.25, inv.xyzt.t);
    if (std::abs(std::abs(tc.lat_rad) - kHalfPi) > 1e-9) {
      EXPECT_NEAR(tc.lon_rad, inv.lpz.lam, 1e-12);
    }
  }

  // 3. Spherical branch (+R=6371000) + linear unit scaling (+units=km).
  constexpr double kRadiusM = 6371000.0;
  ScopedPj geocent_km(
      proj_create(ctx.get(), "+proj=geocent +R=6371000 +units=km"));
  ASSERT_NE(nullptr, geocent_km.get());
  const PJ_COORD sph_fwd = proj_trans(geocent_km.get(), PJ_FWD,
                                      proj_coord(0.0, kPi / 4.0, 1000.0, 0.0));
  EXPECT_EQ(0, proj_errno(geocent_km.get()));
  const double expected_km =
      ((kRadiusM + 1000.0) * std::cos(kPi / 4.0)) / 1000.0;
  EXPECT_NEAR(expected_km, sph_fwd.xyz.x, 1e-9);
  EXPECT_NEAR(0.0, sph_fwd.xyz.y, 1e-12);
  EXPECT_NEAR(expected_km, sph_fwd.xyz.z, 1e-9);

  const PJ_COORD sph_inv = proj_trans(geocent_km.get(), PJ_INV, sph_fwd);
  EXPECT_EQ(0, proj_errno(geocent_km.get()));
  EXPECT_NEAR(0.0, sph_inv.lpz.lam, 1e-12);
  EXPECT_NEAR(kPi / 4.0, sph_inv.lpz.phi, 1e-12);
  EXPECT_NEAR(1000.0, sph_inv.lpz.z, 1e-6);
}

TEST(GeocentTest, Classic2DEntryPointsBatchAndCrsToCrs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=geocent +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Classic 2D pj_fwd (z=0) and pj_inv (Z=0 -> phi=0).
  const PJ_LP in_lp = {.lam = DegToRad(10.0), .phi = DegToRad(48.0)};
  const PJ_XY xy = pj_fwd(in_lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_COORD ref3d =
      proj_trans(pj.get(), PJ_FWD, proj_coord(in_lp.lam, in_lp.phi, 0.0, 0.0));
  EXPECT_NEAR(ref3d.xyz.x, xy.x, 1e-8);
  EXPECT_NEAR(ref3d.xyz.y, xy.y, 1e-8);

  const PJ_LP out_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_lp.lam, out_lp.lam, 1e-12);
  EXPECT_NEAR(0.0, out_lp.phi, 1e-12);

  // 2. Batch proj_trans_array and proj_trans_generic.
  std::array<PJ_COORD, 2> arr = {
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(DegToRad(90.0), 0.0, 500.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, arr.size(), arr.data()));
  EXPECT_NEAR(6378137.0, arr[0].xyz.x, 1e-6);
  EXPECT_NEAR(6378637.0, arr[1].xyz.y, 1e-6);
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, arr.size(), arr.data()));
  EXPECT_NEAR(0.0, arr[0].lpz.lam, 1e-12);
  EXPECT_NEAR(DegToRad(90.0), arr[1].lpz.lam, 1e-12);
  EXPECT_NEAR(500.0, arr[1].lpz.z, 1e-6);

  std::array<double, 2> x = {0.0, kHalfPi};
  std::array<double, 2> y = {0.0, 0.0};
  std::array<double, 2> z = {0.0, 0.0};
  EXPECT_EQ(2u, proj_trans_generic(pj.get(), PJ_FWD, x.data(), sizeof(double),
                                   x.size(), y.data(), sizeof(double), y.size(),
                                   z.data(), sizeof(double), z.size(), nullptr,
                                   0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(6378137.0, x[0], 1e-6);
  EXPECT_NEAR(6378137.0, y[1], 1e-6);

  // 3. +type=crs yields a GeocentricCRS usable with proj_create_crs_to_crs.
  ScopedPj geocent_crs(
      proj_create(ctx.get(), "+proj=geocent +datum=WGS84 +type=crs"));
  ASSERT_NE(nullptr, geocent_crs.get());
  EXPECT_EQ(PJ_TYPE_GEOCENTRIC_CRS, proj_get_type(geocent_crs.get()));

  ScopedPj crs_to_crs(proj_create_crs_to_crs(
      ctx.get(), "EPSG:4326", "+proj=geocent +datum=WGS84 +type=crs", nullptr));
  ASSERT_NE(nullptr, crs_to_crs.get());
  // EPSG:4326 takes (lat, lon) in degrees.
  const PJ_COORD crs_fwd =
      proj_trans(crs_to_crs.get(), PJ_FWD, proj_coord(0.0, 90.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(crs_to_crs.get()));
  EXPECT_NEAR(0.0, crs_fwd.xyz.x, 1e-6);
  EXPECT_NEAR(6378137.0, crs_fwd.xyz.y, 1e-6);
  EXPECT_NEAR(0.0, crs_fwd.xyz.z, 1e-6);
}

TEST(GeocentTest, ProjFactorsAndErrorEdgeCases) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Because geocent sets P->fwd = (lam, phi), proj_factors computes
  //    unit-identity derivatives (dx/dlam = 1, dy/dphi = 1) on
  //    sphere/ellipsoid.
  ScopedPj sph(proj_create(ctx.get(), "+proj=geocent +R=6371000"));
  ASSERT_NE(nullptr, sph.get());
  const PJ_FACTORS sph_fac =
      proj_factors(sph.get(), proj_coord(0.0, DegToRad(60.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_NEAR(1.0, sph_fac.dx_dlam, 1e-10);
  EXPECT_NEAR(0.0, sph_fac.dx_dphi, 1e-10);
  EXPECT_NEAR(0.0, sph_fac.dy_dlam, 1e-10);
  EXPECT_NEAR(1.0, sph_fac.dy_dphi, 1e-10);
  EXPECT_NEAR(1.0, sph_fac.meridional_scale, 1e-9);
  EXPECT_NEAR(2.0, sph_fac.parallel_scale, 1e-9);
  EXPECT_NEAR(2.0, sph_fac.areal_scale, 1e-9);

  // 2. Domain errors on forward angular inputs and HUGE_VAL on inverse.
  ScopedPj wgs84(proj_create(ctx.get(), "+proj=geocent +ellps=WGS84"));
  ASSERT_NE(nullptr, wgs84.get());

  const PJ_COORD bad_lat = proj_trans(
      wgs84.get(), PJ_FWD, proj_coord(0.0, kHalfPi + 1e-4, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(bad_lat.xyz.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(wgs84.get()));
  proj_errno_reset(wgs84.get());

  const PJ_COORD bad_lon =
      proj_trans(wgs84.get(), PJ_FWD, proj_coord(11.0, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(bad_lon.xyz.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(wgs84.get()));
  proj_errno_reset(wgs84.get());

  const PJ_COORD huge_inv =
      proj_trans(wgs84.get(), PJ_INV, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(huge_inv.lpz.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(wgs84.get()));
  proj_errno_reset(wgs84.get());

  // 3. NaN propagates cleanly without setting errno.
  const PJ_COORD nan_out = proj_trans(
      wgs84.get(), PJ_FWD,
      proj_coord(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.xyz.x));
  EXPECT_EQ(0, proj_errno(wgs84.get()));
}

}  // namespace
}  // namespace proj
