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
// Unit tests for the Eckert II pseudocylindrical projection
// (proj/src/projections/eck2.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/eck2.cpp

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
constexpr double kFxc = 0.46065886596178063902;
constexpr double kFyc = 1.44720250911653531871;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Eck2Test, MetadataAndRoundTripAllQuadrants) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=eck2 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("eck2", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Eckert II"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 60.0, .lat_deg = 0.0},
      {.lon_deg = 30.0, .lat_deg = 45.0},
      {.lon_deg = -75.0, .lat_deg = 60.0},
      {.lon_deg = 110.0, .lat_deg = -35.0},
      {.lon_deg = -150.0, .lat_deg = -70.0},
      {.lon_deg = 45.0, .lat_deg = 90.0},
      {.lon_deg = -45.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);

    // 1. Forward projection.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));

    const double s = std::sqrt(4.0 - 3.0 * std::sin(std::abs(phi)));
    const double expected_x = kFxc * lam * s;
    const double expected_y = (phi < 0.0 ? -1.0 : 1.0) * kFyc * (2.0 - s);
    EXPECT_NEAR(expected_x, fwd.xy.x, 1e-12);
    EXPECT_NEAR(expected_y, fwd.xy.y, 1e-12);

    // 2. Inverse projection round-trip.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-11);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-11);
  }
}

TEST(Eck2Test, InversePoleClampingAndOutsideProjectionDomainBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=eck2 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Slightly beyond |y| = FYC so that s = (4 - u^2) / 3 = 1 + 1e-8 is in
  //    [1.0, ONEEPS] (ONEEPS = 1.0000001), clamping lp.phi to +M_HALFPI (for
  //    y > 0) and -M_HALFPI (for y < 0).
  const double u_pos = std::sqrt(1.0 - 3.0e-8);
  const double y_clamped = kFyc * (2.0 - u_pos);
  const PJ_LP inv_north = pj_inv({.x = 0.0, .y = y_clamped}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kHalfPi, inv_north.phi, 1e-12);

  const PJ_LP inv_south = pj_inv({.x = 0.0, .y = -y_clamped}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kHalfPi, inv_south.phi, 1e-12);

  // 2. Choose u = 2 - |y| / FYC = -sqrt(7.00000015) so that
  //    s = (4 - u^2) / 3 = -1.00000005 is in [-ONEEPS, -1.0], exercising the
  //    negative side of `lp.phi < 0. ? -M_HALFPI : M_HALFPI`.
  const double y_neg_s = kFyc * (2.0 + std::sqrt(7.00000015));
  const PJ_LP inv_neg_s = pj_inv({.x = 0.0, .y = y_neg_s}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kHalfPi, inv_neg_s.phi, 1e-12);

  // 3. Choose y = 2 * FYC so u = 0 and s = 4/3 > ONEEPS, triggering
  //    PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  ScopedLogSuppressor suppressor(ctx.get());
  const PJ_COORD inv_outside =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 2.0 * kFyc, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv_outside.xyzt.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(Eck2Test, FactorsConfirmEqualAreaScale) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=eck2 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(20.0), DegToRad(-30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-5);
}

}  // namespace
}  // namespace proj
