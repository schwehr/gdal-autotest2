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
// Unit tests for the Hatano Asymmetrical Equal Area (hatano) projection
// (proj/src/projections/hatano.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/hatano.cpp

#include <array>
#include <cmath>
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

TEST(HatanoTest, AsymmetricalNorthSouthForwardAndInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=hatano +R=1"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("hatano", info.id);
  EXPECT_STREQ("Hatano Asymmetrical Equal Area\n\tPCyl, Sph", pj->descr);
  EXPECT_EQ(1, info.has_inverse);

  // 1. Origin (0, 0) maps to (0, 0), and along equator x = 0.85 * lam.
  const PJ_XY eq = pj_fwd({.lam = 1.0, .phi = 0.0}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.85, eq.x, 1e-12);
  EXPECT_NEAR(0.0, eq.y, 1e-12);

  // 2. Verify north/south asymmetry: +45 deg and -45 deg produce distinct
  // |y| and |x| values because CN/FYCN != CSz/FYCS.
  const PJ_XY north_45 =
      pj_fwd({.lam = DegToRad(30.0), .phi = DegToRad(45.0)}, pj.get());
  const PJ_XY south_45 =
      pj_fwd({.lam = DegToRad(30.0), .phi = DegToRad(-45.0)}, pj.get());
  EXPECT_GT(north_45.y, 0.0);
  EXPECT_LT(south_45.y, 0.0);
  EXPECT_GT(std::abs(north_45.y) - std::abs(south_45.y), 5e-3);
  EXPECT_GT(south_45.x - north_45.x, 1e-2);

  // 3. Round-trip coordinates across both hemispheres and poles.
  struct TestCoord {
    double lon_deg;
    double lat_deg;
  };
  constexpr TestCoord kCoords[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 30.0},
      {.lon_deg = -110.0, .lat_deg = 65.0},
      {.lon_deg = 80.0, .lat_deg = -25.0},
      {.lon_deg = -150.0, .lat_deg = -70.0},
      {.lon_deg = 0.0, .lat_deg = 90.0},
      {.lon_deg = 0.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCoords) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-6);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-6);
  }

  // 4. Verify equal-area property (areal_scale == 1.0) via proj_factors.
  const PJ_FACTORS factors_n = proj_factors(
      pj.get(), proj_coord(DegToRad(20.0), DegToRad(40.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors_n.areal_scale, 1e-4);

  const PJ_FACTORS factors_s = proj_factors(
      pj.get(), proj_coord(DegToRad(-20.0), DegToRad(-40.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors_s.areal_scale, 1e-4);
}

TEST(HatanoTest, InverseClampingAndOutOfDomainBranchesForThAndPhi) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=hatano +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. |th| > ONETOL (1.000001): xy.y = 2.0 -> th = 2.0 * RYCN = 1.137 >
  // ONETOL.
  const PJ_LP inv_th_over = pj_inv({.x = 0.0, .y = 2.0}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  EXPECT_TRUE(std::isinf(inv_th_over.lam));
  EXPECT_TRUE(std::isinf(inv_th_over.phi));
  proj_errno_reset(pj.get());

  // 2. 1.0 < |th| <= ONETOL (clamps th to +-M_HALFPI, which then exceeds
  // ONETOL on the second check for lp.phi):
  // FYCN = 1.75859 (1 / RYCN) and FYCS = 1.93052 (1 / RYCS).
  const PJ_LP inv_th_clamp_pos =
      pj_inv({.x = 0.0, .y = 1.75859 * 1.0000005}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  EXPECT_TRUE(std::isinf(inv_th_clamp_pos.lam));
  EXPECT_TRUE(std::isinf(inv_th_clamp_pos.phi));
  proj_errno_reset(pj.get());

  const PJ_LP inv_th_clamp_neg =
      pj_inv({.x = 0.0, .y = -1.93052 * 1.0000005}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  EXPECT_TRUE(std::isinf(inv_th_clamp_neg.lam));
  EXPECT_TRUE(std::isinf(inv_th_clamp_neg.phi));
  proj_errno_reset(pj.get());

  // 3. |th| < 1.0, but |lp.phi| > ONETOL (e.g. xy.y = 1.6 exceeds north pole
  // y ≈ 1.4885 while remaining below FYCN = 1.75859).
  const PJ_LP inv_phi_over = pj_inv({.x = 0.0, .y = 1.6}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  EXPECT_TRUE(std::isinf(inv_phi_over.lam));
  EXPECT_TRUE(std::isinf(inv_phi_over.phi));
  proj_errno_reset(pj.get());

  // 4. |th| < 1.0 and 1.0 < |lp.phi| <= ONETOL (1.000001), exercising the
  // `lp.phi = lp.phi > 0. ? M_HALFPI : -M_HALFPI` clamping branch at both
  // poles. Adding 1e-6 to the exact pole y shifts un-asin'd lp.phi from 1.0 to
  // ~1.0000004 (inside (1.0, 1.000001]).
  const PJ_XY north_pole_xy = pj_fwd({.lam = 0.0, .phi = kHalfPi}, pj.get());
  const PJ_LP north_pole_clamped =
      pj_inv({.x = 0.0, .y = north_pole_xy.y + 1e-6}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kHalfPi, north_pole_clamped.phi, 1e-12);

  const PJ_XY south_pole_xy = pj_fwd({.lam = 0.0, .phi = -kHalfPi}, pj.get());
  const PJ_LP south_pole_clamped =
      pj_inv({.x = 0.0, .y = south_pole_xy.y - 1e-6}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kHalfPi, south_pole_clamped.phi, 1e-12);
}

}  // namespace
}  // namespace proj
