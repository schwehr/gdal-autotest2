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
// Unit tests for the Winkel II pseudocylindrical projection
// (proj/src/projections/wink2.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/wink2.cpp

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
constexpr double kHalfPi = kPi / 2.0;
constexpr double kQuarterPi = kPi / 4.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Wink2Test, MetadataAndDefaultAndCustomLat1Setup) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_default(proj_create(ctx.get(), "+proj=wink2 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_default.get());
  EXPECT_EQ(0, proj_errno(pj_default.get()));
  EXPECT_EQ(0.0, pj_default->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj_default.get());
  EXPECT_STREQ("wink2", info.id);
  EXPECT_NE(std::string_view(pj_default->descr).find("Winkel II"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);

  // At the equator (phi = 0), x = 0.5 * lam * (1 + cos(lat_1)).
  ScopedPj pj_r1_lat0(proj_create(ctx.get(), "+proj=wink2 +R=1"));
  ASSERT_NE(nullptr, pj_r1_lat0.get());
  ScopedPj pj_r1_lat60(proj_create(ctx.get(), "+proj=wink2 +R=1 +lat_1=60"));
  ASSERT_NE(nullptr, pj_r1_lat60.get());

  constexpr double kLon = DegToRad(40.0);
  const PJ_COORD eq0 =
      proj_trans(pj_r1_lat0.get(), PJ_FWD, proj_coord(kLon, 0.0, 0.0, 0.0));
  const PJ_COORD eq60 =
      proj_trans(pj_r1_lat60.get(), PJ_FWD, proj_coord(kLon, 0.0, 0.0, 0.0));
  EXPECT_NEAR(kLon, eq0.xy.x, 1e-12);
  EXPECT_NEAR(0.5 * kLon * (1.0 + 0.5), eq60.xy.x, 1e-12);
  EXPECT_NEAR(0.0, eq0.xy.y, 1e-12);
  EXPECT_NEAR(0.0, eq60.xy.y, 1e-12);
}

TEST(Wink2Test, ForwardPoleIterationExhaustionBranchForNorthAndSouthPoles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=wink2 +R=1 +lat_1=60"));
  ASSERT_NE(nullptr, pj.get());

  constexpr double kLon = DegToRad(90.0);

  // At phi = +-pi/2, the root of phi + sin(phi) = +-pi is a double root where
  // 1 + cos(phi) -> 0, so Newton's method converges linearly and exhausts all
  // MAX_ITER = 10 iterations (i == 0), exercising the `if (!i)` fallback
  // `lp.phi = (lp.phi < 0.) ? -M_HALFPI : M_HALFPI`.
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLon, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  // At lp.phi = +pi/2: cos(pi/2) = 0, sin(pi/2) = 1, xy.y_init = 1, so:
  // x = 0.5 * lam * (0 + cos(60 deg)) = 0.25 * lam, y = (pi/4) * (1 + 1) =
  // pi/2.
  EXPECT_NEAR(0.25 * kLon, north_pole.xy.x, 1e-12);
  EXPECT_NEAR(2.0 * kQuarterPi, north_pole.xy.y, 1e-12);

  const PJ_COORD south_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLon, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.25 * kLon, south_pole.xy.x, 1e-12);
  EXPECT_NEAR(-2.0 * kQuarterPi, south_pole.xy.y, 1e-12);
}

TEST(Wink2Test, ForwardAndGenericInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(
      proj_create(ctx.get(), "+proj=wink2 +lat_1=50.459776 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 15.0, .lat_deg = 25.0},
      {.lon_deg = -75.0, .lat_deg = 45.0},
      {.lon_deg = 120.0, .lat_deg = -35.0},
      {.lon_deg = -160.0, .lat_deg = -65.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);

    // 1. Forward projection (converges in < 10 iterations, hitting `else`).
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));

    // 2. Inverse via pj_generic_inverse_2d recovers (lam, phi).
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-9);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-9);
  }
}

TEST(Wink2Test, InverseOutOfDomainArrayAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=wink2 +R=1 +lat_1=40"));
  ASSERT_NE(nullptr, pj.get());

  // Out-of-domain y (> pi/2) fails generic 2D inverse.
  const PJ_COORD bad_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 5.0, 0.0, 0.0));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  EXPECT_EQ(HUGE_VAL, bad_inv.lp.lam);
  proj_errno_reset(pj.get());

  std::array<PJ_COORD, 2> coords = {
      proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0),
      proj_coord(DegToRad(-45.0), DegToRad(-20.0), 0.0, 0.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(20.0), coords[0].lp.lam, 1e-9);
  EXPECT_NEAR(DegToRad(30.0), coords[0].lp.phi, 1e-9);
  EXPECT_NEAR(DegToRad(-45.0), coords[1].lp.lam, 1e-9);
  EXPECT_NEAR(DegToRad(-20.0), coords[1].lp.phi, 1e-9);

  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(10.0), DegToRad(20.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(factors.meridional_scale, 0.0);
  EXPECT_GT(factors.parallel_scale, 0.0);
  EXPECT_GT(factors.areal_scale, 0.0);
}

}  // namespace
}  // namespace proj
