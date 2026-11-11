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
// Unit tests for the Oblique Cylindrical Equal Area (ocea) projection
// (proj/src/projections/ocea.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/ocea.cpp

#include <array>
#include <cmath>
#include <format>  // NOLINT(build/c++20)
#include <numbers>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(OceaTest, OnePointAndAzimuthModeRoundTripAndScaleFactor) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=ocea +lat_0=30 +lonc=15 +alpha=25 +k_0=0.95 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  // Compute P->lam0 so we can test points in the principal hemisphere
  // (|lon - lam0| < pi/2, cos(lp.lam) > 0) for round-trip and in the opposite
  // hemisphere (cos(lp.lam) < 0) for the `if (t < 0.) xy.x += M_PI;` branch.
  const double lam0 = pj->lam0;

  struct OffsetCase {
    double dlam_deg;
    double lat_deg;
  };
  constexpr OffsetCase kCases[] = {
      {.dlam_deg = 0.0, .lat_deg = 0.0},
      {.dlam_deg = 20.0, .lat_deg = 30.0},
      {.dlam_deg = -35.0, .lat_deg = -20.0},
      {.dlam_deg = 60.0, .lat_deg = 15.0},
  };

  for (const auto& tc : kCases) {
    const double lon_rad = adjlon(lam0 + DegToRad(tc.dlam_deg));
    const double lat_rad = DegToRad(tc.lat_deg);

    // 1. Forward projection.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(fwd.xy.x));
    EXPECT_FALSE(std::isinf(fwd.xy.y));

    // 2. Inverse projection.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-11);
    EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-11);
  }

  // Exercise the `if (t < 0.) xy.x += M_PI;` branch in ocea_s_forward by
  // choosing a longitude 120 deg away from lam0 so cos(lp.lam) = -0.5 < 0.
  const double far_lon = adjlon(lam0 + DegToRad(120.0));
  const PJ_COORD far_fwd = proj_trans(
      pj.get(), PJ_FWD, proj_coord(far_lon, DegToRad(10.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(far_fwd.xy.x, 0.0);
}

TEST(OceaTest, TwoPointModeGeneralNonZeroLat1RoundTripAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=ocea +lat_1=20 +lon_1=10 +lat_2=50 +lon_2=60 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  const double lam0 = pj->lam0;
  const PJ_COORD in =
      proj_coord(adjlon(lam0 + DegToRad(15.0)), DegToRad(25.0), 50.0, 2026.0);

  // 1. Forward and inverse round-trip.
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.lpz.lam, inv.lpz.lam, 1e-11);
  EXPECT_NEAR(in.lpz.phi, inv.lpz.phi, 1e-11);
  EXPECT_NEAR(in.lpz.z, inv.lpz.z, 1e-9);
  EXPECT_DOUBLE_EQ(in.xyzt.t, inv.xyzt.t);

  // 2. Equal-area property: areal_scale == 1.0 on the sphere.
  const PJ_FACTORS factors = proj_factors(pj.get(), in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-6);
}

TEST(OceaTest, TwoPointModeSpecialBranchesLon1MinusHalfPiAndTanPhi1Zero) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Exact `lam_1 == -M_HALFPI` branch (`if (lam_1 == -M_HALFPI) lam_p =
  // -lam_p`). Pass -M_HALFPI with the 'r' (radians) suffix so dmstor returns
  // exact -M_HALFPI.
  const std::string proj_lon1_neg_halfpi = std::format(
      "+proj=ocea +lat_1=30 +lon_1=-{:.17g}r +lat_2=45 +lon_2=20 +R=1",
      kHalfPi);
  ScopedPj pj_neg90(proj_create(ctx.get(), proj_lon1_neg_halfpi.c_str()));
  ASSERT_NE(nullptr, pj_neg90.get());

  const PJ_LP lp_in = {.lam = pj_neg90->lam0 + 0.2, .phi = 0.3};
  const PJ_XY xy = pj_fwd(lp_in, pj_neg90.get());
  EXPECT_EQ(0, proj_errno(pj_neg90.get()));
  const PJ_LP lp_out = pj_inv(xy, pj_neg90.get());
  EXPECT_EQ(0, proj_errno(pj_neg90.get()));
  EXPECT_NEAR(lp_in.lam, lp_out.lam, 1e-12);
  EXPECT_NEAR(lp_in.phi, lp_out.phi, 1e-12);

  // 2. `tan_phi_1 == 0.0` with `cos(lam_p - lam_1) >= 0.0` -> `phi_p =
  // -M_HALFPI`: When lat_1 = 0 and lat_2 = 0, lam_p = atan2(0, 0) = 0; with
  // lon_1 = 0, cos(lam_p - lam_1) = cos(0) = +1 >= 0.
  ScopedPj pj_tan0_pos(proj_create(
      ctx.get(), "+proj=ocea +lat_1=0 +lon_1=0 +lat_2=0 +lon_2=45 +R=1"));
  ASSERT_NE(nullptr, pj_tan0_pos.get());
  const PJ_COORD c_pos =
      proj_trans(pj_tan0_pos.get(), PJ_FWD,
                 proj_coord(pj_tan0_pos->lam0 + 0.1, 0.2, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_tan0_pos.get()));
  const PJ_COORD c_pos_inv = proj_trans(pj_tan0_pos.get(), PJ_INV, c_pos);
  EXPECT_EQ(0, proj_errno(pj_tan0_pos.get()));
  EXPECT_NEAR(pj_tan0_pos->lam0 + 0.1, c_pos_inv.lp.lam, 1e-12);
  EXPECT_NEAR(0.2, c_pos_inv.lp.phi, 1e-12);

  // 3. `tan_phi_1 == 0.0` with `cos(lam_p - lam_1) < 0.0` -> `phi_p =
  // +M_HALFPI`: When lat_1 = 0 and lat_2 = 0, lam_p = 0; with lon_1 = 180 deg
  // (pi), cos(lam_p - lam_1) = cos(-pi) = -1 < 0.
  ScopedPj pj_tan0_neg(proj_create(
      ctx.get(), "+proj=ocea +lat_1=0 +lon_1=180 +lat_2=0 +lon_2=45 +R=1"));
  ASSERT_NE(nullptr, pj_tan0_neg.get());
  const PJ_COORD c_neg =
      proj_trans(pj_tan0_neg.get(), PJ_FWD,
                 proj_coord(pj_tan0_neg->lam0 + 0.1, 0.2, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_tan0_neg.get()));
  const PJ_COORD c_neg_inv = proj_trans(pj_tan0_neg.get(), PJ_INV, c_neg);
  EXPECT_EQ(0, proj_errno(pj_tan0_neg.get()));
  EXPECT_NEAR(pj_tan0_neg->lam0 + 0.1, c_neg_inv.lp.lam, 1e-12);
  EXPECT_NEAR(0.2, c_neg_inv.lp.phi, 1e-12);
}

}  // namespace
}  // namespace proj
