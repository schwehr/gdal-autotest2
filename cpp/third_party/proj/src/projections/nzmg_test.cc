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
// Unit tests for the New Zealand Map Grid (nzmg) projection
// (proj/src/projections/nzmg.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/nzmg.cpp

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

// Fixed New Zealand Map Grid parameters forced by PJ_PROJECTION(nzmg).
constexpr double kNzmgOriginLonDeg = 173.0;
constexpr double kNzmgOriginLatDeg = -41.0;
constexpr double kNzmgFalseEasting = 2510000.0;
constexpr double kNzmgFalseNorthing = 6023150.0;
constexpr double kInternationalSemiMajor = 6378388.0;

TEST(NzmgTest, MetadataAndForcedFixedEarthParameters) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Supplying custom +lon_0, +lat_0, +x_0, +y_0, or +a should be overridden by
  // PJ_PROJECTION(nzmg), which forces the International 1924 major axis and
  // official NZMG origin/false coordinates.
  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=nzmg +lon_0=0 +lat_0=0 +x_0=0 +y_0=0 +a=6371000 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("nzmg", info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", info.description);
  EXPECT_NE(std::string_view(pj->descr).find("New Zealand Map Grid"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  EXPECT_DOUBLE_EQ(kInternationalSemiMajor, pj->a);
  EXPECT_DOUBLE_EQ(1.0 / kInternationalSemiMajor, pj->ra);
  EXPECT_NEAR(DegToRad(kNzmgOriginLonDeg), pj->lam0, 1e-12);
  EXPECT_NEAR(DegToRad(kNzmgOriginLatDeg), pj->phi0, 1e-12);
  EXPECT_DOUBLE_EQ(kNzmgFalseEasting, pj->x0);
  EXPECT_DOUBLE_EQ(kNzmgFalseNorthing, pj->y0);

  // At the NZMG origin (173E, 41S), forward projection yields exact false
  // easting and false northing.
  const PJ_COORD origin_fwd =
      proj_trans(pj.get(), PJ_FWD,
                 proj_coord(DegToRad(kNzmgOriginLonDeg),
                            DegToRad(kNzmgOriginLatDeg), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kNzmgFalseEasting, origin_fwd.xy.x, 1e-6);
  EXPECT_NEAR(kNzmgFalseNorthing, origin_fwd.xy.y, 1e-6);
}

TEST(NzmgTest, ForwardAndInverseRoundTripAcrossNewZealand) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nzmg"));
  ASSERT_NE(nullptr, pj.get());

  struct CityCase {
    std::string_view name;
    double lon_deg;
    double lat_deg;
  };
  constexpr CityCase kCities[] = {
      {.name = "Origin", .lon_deg = 173.0, .lat_deg = -41.0},
      {.name = "Auckland", .lon_deg = 174.7633, .lat_deg = -36.8485},
      {.name = "Wellington", .lon_deg = 174.7762, .lat_deg = -41.2865},
      {.name = "Christchurch", .lon_deg = 172.6362, .lat_deg = -43.5321},
      {.name = "Dunedin", .lon_deg = 170.5028, .lat_deg = -45.8788},
      {.name = "Invercargill", .lon_deg = 168.3538, .lat_deg = -46.4132},
      {.name = "Gisborne", .lon_deg = 178.0176, .lat_deg = -38.6623},
  };

  for (const auto& city : kCities) {
    const double lon_rad = DegToRad(city.lon_deg);
    const double lat_rad = DegToRad(city.lat_deg);

    // 1. Forward projection (lon, lat in radians) -> (E, N in meters).
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD,
                                    proj_coord(lon_rad, lat_rad, 15.0, 2026.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << city.name;
    EXPECT_TRUE(std::isfinite(fwd.xy.x)) << city.name;
    EXPECT_TRUE(std::isfinite(fwd.xy.y)) << city.name;
    EXPECT_DOUBLE_EQ(15.0, fwd.xyz.z);
    EXPECT_DOUBLE_EQ(2026.0, fwd.xyzt.t);

    // Easting/northing sign relative to origin (173E, -41S).
    if (city.lon_deg > kNzmgOriginLonDeg + 0.5) {
      EXPECT_GT(fwd.xy.x, kNzmgFalseEasting) << city.name;
    } else if (city.lon_deg < kNzmgOriginLonDeg - 0.5) {
      EXPECT_LT(fwd.xy.x, kNzmgFalseEasting) << city.name;
    }
    if (city.lat_deg > kNzmgOriginLatDeg + 0.5) {
      EXPECT_GT(fwd.xy.y, kNzmgFalseNorthing) << city.name;
    } else if (city.lat_deg < kNzmgOriginLatDeg - 0.5) {
      EXPECT_LT(fwd.xy.y, kNzmgFalseNorthing) << city.name;
    }

    // 2. Inverse projection (E, N in meters) -> (lon, lat in radians).
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << city.name;
    // The 9-term tpsi and 8-term tphi power series approximations round-trip
    // to within ~1e-8 radians across New Zealand.
    EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-7) << city.name;
    EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-7) << city.name;
  }
}

TEST(NzmgTest, Classic2dEntryPointsArrayTransformAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nzmg"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP lp = {.lam = DegToRad(174.7762), .phi = DegToRad(-41.2865)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(2658777.236615, xy.x, 1e-3);
  EXPECT_NEAR(5989819.780160, xy.y, 1e-3);

  const PJ_LP lp_back = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, lp_back.lam, 1e-7);
  EXPECT_NEAR(lp.phi, lp_back.phi, 1e-7);

  // Batch array transformation.
  auto coords = std::to_array<PJ_COORD>({
      proj_coord(DegToRad(173.0), DegToRad(-41.0), 0.0, 0.0),
      proj_coord(DegToRad(174.7633), DegToRad(-36.8485), 0.0, 0.0),
  });
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(173.0), coords[0].lp.lam, 1e-9);
  EXPECT_NEAR(DegToRad(-41.0), coords[0].lp.phi, 1e-9);
  EXPECT_NEAR(DegToRad(174.7633), coords[1].lp.lam, 1e-7);
  EXPECT_NEAR(DegToRad(-36.8485), coords[1].lp.phi, 1e-7);

  // Distortion factors at the NZMG origin: conformal scale is near 1.
  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(173.0), DegToRad(-41.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.meridional_scale, 1e-2);
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-2);
  EXPECT_NEAR(kPi / 2.0, factors.meridian_parallel_angle, 1e-3);
}

TEST(NzmgTest, InverseNonConvergenceReturnsHugeValAndSetsDomainErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nzmg"));
  ASSERT_NE(nullptr, pj.get());

  // For the degree-6 complex polynomial in nzmg_e_inverse, a starting point of
  // |p| ~ 1.5e4 (x = 1e11 m, y = 1e11 m) stays completely finite across all 20
  // Newton-Raphson steps (each step scales |p| by ~5/6), exhausting nn == 0
  // and taking the `else lp.lam = lp.phi = HUGE_VAL;` branch without setting
  // P->ctx->last_errno (pj_inv4d returns early on HUGE_VAL == coo.v[0] before
  // inv_finalize).
  const PJ_COORD out =
      proj_trans(pj.get(), PJ_INV, proj_coord(1e11, 1e11, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, out.lp.lam);
  EXPECT_EQ(HUGE_VAL, out.lp.phi);
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_LP raw_out = pj->inv({.x = 1e4, .y = 1e4}, pj.get());
  EXPECT_EQ(HUGE_VAL, raw_out.lam);
  EXPECT_EQ(HUGE_VAL, raw_out.phi);
  EXPECT_EQ(0, proj_errno(pj.get()));
}

}  // namespace
}  // namespace proj
