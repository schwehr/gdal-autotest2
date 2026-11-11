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
// Unit tests for the Mercator and Web Mercator projections
// (proj/src/projections/merc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/merc.cpp

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

TEST(MercTest, InvalidLatTsAtOrBeyondPolesFailsWithIllegalArgValue) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr const char* kInvalidDefs[] = {
      "+proj=merc +lat_ts=90",
      "+proj=merc +lat_ts=-90",
      "+proj=merc +lat_ts=95",
      "+proj=merc +R=1 +lat_ts=90",
  };

  for (const char* def : kInvalidDefs) {
    ScopedLogCapture log_capture(ctx.get());
    proj_context_errno_set(ctx.get(), 0);

    ScopedPj pj(proj_create(ctx.get(), def));
    EXPECT_EQ(nullptr, pj.get()) << def;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << def;
    EXPECT_TRUE(log_capture.ContainsMessage("Invalid value for lat_ts")) << def;
  }
}

TEST(MercTest, EllipsoidalForwardInverseLatTsAndConformalFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Default ellipsoidal Mercator (is_phits == 0, k0 = 1.0).
  ScopedPj merc_ell(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, merc_ell.get());
  EXPECT_EQ(1, pj_has_inverse(merc_ell.get()));

  const PJ_PROJ_INFO info = proj_pj_info(merc_ell.get());
  EXPECT_STREQ("merc", info.id);
  EXPECT_NE(std::string_view(merc_ell->descr).find("Mercator"),
            std::string_view::npos);

  // 2. Ellipsoidal Mercator with standard parallel lat_ts (is_phits != 0).
  ScopedPj merc_lat_ts(
      proj_create(ctx.get(), "+proj=merc +ellps=WGS84 +lat_ts=-45"));
  ASSERT_NE(nullptr, merc_lat_ts.get());
  const double phi_ts = DegToRad(45.0);
  const double expected_k0 =
      pj_msfn(std::sin(phi_ts), std::cos(phi_ts), merc_lat_ts->es);
  EXPECT_NEAR(expected_k0, merc_lat_ts->k0, 1e-12);

  struct LonLatCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr LonLatCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 12.5, .lat_deg = 55.67},
      {.lon_deg = -122.4, .lat_deg = 37.8},
      {.lon_deg = 151.2, .lat_deg = -33.86},
      {.lon_deg = -179.9, .lat_deg = -80.0},
  };

  PJ* const kProjections[] = {merc_ell.get(), merc_lat_ts.get()};
  for (PJ* pj : kProjections) {
    for (const auto& tc : kCases) {
      const double lon_rad = DegToRad(tc.lon_deg);
      const double lat_rad = DegToRad(tc.lat_deg);

      const PJ_COORD fwd =
          proj_trans(pj, PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj));
      EXPECT_FALSE(std::isinf(fwd.xy.x));
      EXPECT_FALSE(std::isinf(fwd.xy.y));

      const PJ_COORD inv = proj_trans(pj, PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj));
      EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-12);
      EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-12);
    }
  }

  // 3. At +/-lat_ts, parallel and meridional scale factors equal 1.0, and
  // angular distortion is 0 (conformal projection).
  const PJ_FACTORS factors = proj_factors(
      merc_lat_ts.get(), proj_coord(DegToRad(10.0), -phi_ts, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc_lat_ts.get()));
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-7);
  EXPECT_NEAR(1.0, factors.meridional_scale, 1e-7);
  EXPECT_NEAR(0.0, factors.angular_distortion, 1e-7);
}

TEST(MercTest, SphericalForwardAndInverseWithAndWithoutLatTs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Unit sphere without lat_ts (k0 = 1.0).
  ScopedPj merc_sph(proj_create(ctx.get(), "+proj=merc +R=1"));
  ASSERT_NE(nullptr, merc_sph.get());
  EXPECT_DOUBLE_EQ(1.0, merc_sph->k0);

  // 2. Unit sphere with lat_ts = 60 deg (k0 = cos(60 deg) = 0.5).
  ScopedPj merc_sph_ts(proj_create(ctx.get(), "+proj=merc +R=1 +lat_ts=60"));
  ASSERT_NE(nullptr, merc_sph_ts.get());
  EXPECT_NEAR(0.5, merc_sph_ts->k0, 1e-12);

  const double lam = DegToRad(30.0);
  const double phi = DegToRad(45.0);

  const PJ_COORD fwd_unit =
      proj_trans(merc_sph.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc_sph.get()));
  EXPECT_NEAR(lam, fwd_unit.xy.x, 1e-12);
  EXPECT_NEAR(std::asinh(std::tan(phi)), fwd_unit.xy.y, 1e-12);

  const PJ_COORD fwd_ts =
      proj_trans(merc_sph_ts.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc_sph_ts.get()));
  EXPECT_NEAR(0.5 * fwd_unit.xy.x, fwd_ts.xy.x, 1e-12);
  EXPECT_NEAR(0.5 * fwd_unit.xy.y, fwd_ts.xy.y, 1e-12);

  const PJ_COORD inv_ts = proj_trans(merc_sph_ts.get(), PJ_INV, fwd_ts);
  EXPECT_EQ(0, proj_errno(merc_sph_ts.get()));
  EXPECT_NEAR(lam, inv_ts.lp.lam, 1e-12);
  EXPECT_NEAR(phi, inv_ts.lp.phi, 1e-12);
}

TEST(MercTest, WebMercatorForcesUnitK0AndUsesSphericalFormulas) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Even when +k_0=0.5 and +ellps=WGS84 are supplied, +proj=webmerc overrides
  // P->k0 = 1.0 and uses spherical Mercator equations with WGS84 semi-major a.
  ScopedPj webmerc(
      proj_create(ctx.get(), "+proj=webmerc +ellps=WGS84 +k_0=0.5"));
  ASSERT_NE(nullptr, webmerc.get());
  EXPECT_DOUBLE_EQ(1.0, webmerc->k0);

  const PJ_PROJ_INFO info = proj_pj_info(webmerc.get());
  EXPECT_STREQ("webmerc", info.id);
  EXPECT_NE(
      std::string_view(webmerc->descr).find("Web Mercator / Pseudo Mercator"),
      std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);

  constexpr double kWgs84A = 6378137.0;
  const double lam = DegToRad(-74.006);
  const double phi = DegToRad(40.7128);

  const PJ_COORD fwd =
      proj_trans(webmerc.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(webmerc.get()));
  EXPECT_NEAR(kWgs84A * lam, fwd.xy.x, 1e-6);
  EXPECT_NEAR(kWgs84A * std::asinh(std::tan(phi)), fwd.xy.y, 1e-6);

  const PJ_COORD inv = proj_trans(webmerc.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(webmerc.get()));
  EXPECT_NEAR(lam, inv.lp.lam, 1e-12);
  EXPECT_NEAR(phi, inv.lp.phi, 1e-12);
}

}  // namespace
}  // namespace proj
