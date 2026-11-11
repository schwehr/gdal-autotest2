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
// Unit tests for the Swiss Oblique Mercator (somerc) projection
// (proj/src/projections/somerc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/somerc.cpp

#include <array>
#include <cmath>
#include <limits>
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

TEST(SomercTest, Ch1903Lv03OriginAndSwissCitiesForwardInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Official CH1903 / LV03 parameters centered at the Old Bern Observatory.
  constexpr char kCh1903Lv03[] =
      "+proj=somerc +lat_0=46.95240555555556 +lon_0=7.439583333333333 "
      "+k_0=1 +x_0=600000 +y_0=200000 +ellps=bessel";
  ScopedPj pj(proj_create(ctx.get(), kCh1903Lv03));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("somerc", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Swiss. Obl. Mercator"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);

  // 1. Projection center at Bern maps to false easting/northing (600000,
  // 200000).
  const double bern_lon = DegToRad(7.439583333333333);
  const double bern_lat = DegToRad(46.95240555555556);
  const PJ_COORD bern_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(bern_lon, bern_lat, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(600000.0, bern_fwd.xy.x, 1e-4);
  EXPECT_NEAR(200000.0, bern_fwd.xy.y, 1e-4);

  // 2. Round-trip multiple locations across Switzerland (Zurich, Geneva,
  // Lugano, Davos).
  struct City {
    double lon_deg;
    double lat_deg;
  };
  constexpr City kCities[] = {
      {.lon_deg = 8.5417, .lat_deg = 47.3769},  // Zurich
      {.lon_deg = 6.1432, .lat_deg = 46.2044},  // Geneva
      {.lon_deg = 8.9511, .lat_deg = 46.0037},  // Lugano
      {.lon_deg = 9.8355, .lat_deg = 46.8027},  // Davos
  };

  for (const auto& city : kCities) {
    const double lam = DegToRad(city.lon_deg);
    const double phi = DegToRad(city.lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(fwd.xy.x));
    EXPECT_FALSE(std::isinf(fwd.xy.y));

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-10);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-10);
  }

  // 3. Verify scale factors at projection origin (conformal: h == k == 1).
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(bern_lon, bern_lat, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.meridional_scale, 1e-7);
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-7);
  EXPECT_NEAR(0.0, factors.angular_distortion, 1e-7);
}

TEST(SomercTest, SphericalFallbackAndBatchTransformRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(
      proj_create(ctx.get(), "+proj=somerc +lat_0=45 +lon_0=10 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  std::array<PJ_COORD, 2> coords = {
      proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0),
      proj_coord(DegToRad(12.5), DegToRad(47.0), 0.0, 0.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  EXPECT_NEAR(0.0, coords[0].xy.x, 1e-6);
  EXPECT_NEAR(0.0, coords[0].xy.y, 1e-6);

  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(12.5), coords[1].lp.lam, 1e-11);
  EXPECT_NEAR(DegToRad(47.0), coords[1].lp.phi, 1e-11);
}

TEST(SomercTest,
     InverseNonConvergenceAfterSixIterationsSetsOutsideDomainErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  // 1. High-eccentricity ellipsoid where Newton-Raphson in somerc_e_inverse
  // starts near phip = 0 (at xy.y = -0.00563) and overshoots -pi/2 on the
  // first step due to large rone_es, failing to converge within NITER=6
  // iterations and returning raw (0.0, 0.0) from pj->inv.
  ScopedPj pj_high_e(
      proj_create(ctx.get(), "+proj=somerc +lat_0=45 +a=1 +e=0.999"));
  ASSERT_NE(nullptr, pj_high_e.get());
  ASSERT_NE(nullptr, pj_high_e->inv);
  const PJ_LP lp_high_e =
      pj_high_e->inv({.x = 0.0, .y = -0.00563}, pj_high_e.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_high_e.get()));
  EXPECT_EQ(0.0, lp_high_e.lam);
  EXPECT_EQ(0.0, lp_high_e.phi);
  proj_errno_reset(pj_high_e.get());

  // 2. NaN input to pj_inv also exhausts all NITER=6 iterations (`i == 0`),
  // sets PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN, and error_or_coord
  // in pj_inv maps the return value to HUGE_VAL.
  ScopedPj pj_wgs84(
      proj_create(ctx.get(), "+proj=somerc +lat_0=46.95 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_wgs84.get());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const PJ_LP lp_nan = pj_inv({.x = nan, .y = 0.0}, pj_wgs84.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_wgs84.get()));
  EXPECT_EQ(HUGE_VAL, lp_nan.lam);
  EXPECT_EQ(HUGE_VAL, lp_nan.phi);
  proj_errno_reset(pj_wgs84.get());
}

}  // namespace
}  // namespace proj
