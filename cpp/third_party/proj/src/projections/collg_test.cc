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
// Unit tests for the Collignon pseudocylindrical projection
// (proj/src/projections/collg.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/collg.cpp

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
constexpr double kFxc = 1.12837916709551257390;  // 2 / sqrt(pi)
constexpr double kFyc = 1.77245385090551602729;  // sqrt(pi)

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(CollgTest, MetadataAndRoundTripAcrossHemispheres) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=collg +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("collg", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Collignon"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 0.0},
      {.lon_deg = -90.0, .lat_deg = 30.0},
      {.lon_deg = 120.0, .lat_deg = 60.0},
      {.lon_deg = -135.0, .lat_deg = -45.0},
      {.lon_deg = 60.0, .lat_deg = -80.0},
      {.lon_deg = 15.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);

    // 1. Forward projection.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));

    const double s = std::sqrt(1.0 - std::sin(phi));
    EXPECT_NEAR(kFxc * lam * s, fwd.xy.x, 1e-12);
    EXPECT_NEAR(kFyc * (1.0 - s), fwd.xy.y, 1e-12);

    // 2. Inverse projection round-trip.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-11);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-11);
  }
}

TEST(CollgTest, NorthPoleForwardAndInverseCollapseToApex) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=collg +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. At phi = +pi/2, 1 - sin(phi) == 0 triggers the `if (xy.y <= 0.)` branch
  //    in collg_s_forward, collapsing all longitudes to (0, FYC).
  const PJ_COORD north_fwd = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(75.0), kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, north_fwd.xy.x, 1e-12);
  EXPECT_NEAR(kFyc, north_fwd.xy.y, 1e-12);

  // 2. At y = FYC, collg_s_inverse computes lp.phi = 1 - 0 = 1.0, hitting the
  //    `else` clamping branch (`lp.phi = M_HALFPI`) and `if (lp.lam <= 0.)
  //    lp.lam = 0.`.
  const PJ_COORD north_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.5, kFyc, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, north_inv.lp.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, north_inv.lp.phi, 1e-12);
}

TEST(CollgTest, InverseNearSouthPoleClampingAndOutsideDomainError) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=collg +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Choose u = y / FYC - 1 = -sqrt(2.00000005) so that
  //    1 - u^2 = -1.00000005 is in [-ONEEPS, -1.0] (with ONEEPS = 1.0000001).
  //    This exercises the `lp.phi < 0. ? -M_HALFPI : M_HALFPI` negative branch
  //    followed by `lp.lam = xy.x / (FXC * sqrt(2))`.
  const double y_near_south_pole = kFyc * (1.0 - std::sqrt(2.00000005));
  const double x_in = kFxc * 0.5 * std::sqrt(2.0);
  const PJ_LP inv_clamped =
      pj_inv({.x = x_in, .y = y_near_south_pole}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kHalfPi, inv_clamped.phi, 1e-12);
  EXPECT_NEAR(0.5, inv_clamped.lam, 1e-12);

  // 2. Choose y = -FYC so u = -2 and |1 - u^2| = 3 > ONEEPS, triggering
  //    PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  ScopedLogSuppressor suppressor(ctx.get());
  const PJ_COORD inv_outside =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, -kFyc, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv_outside.xyzt.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(CollgTest, FactorsVerifyEqualAreaPropertyOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=collg +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // Collignon is an equal-area pseudocylindrical projection (areal_scale == 1).
  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(25.0), DegToRad(35.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-5);
}

}  // namespace
}  // namespace proj
