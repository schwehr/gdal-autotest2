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
// Unit tests for the Eckert IV pseudocylindrical projection
// (proj/src/projections/eck4.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/eck4.cpp

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
constexpr double kCx = 0.42223820031577120149;
constexpr double kCy = 1.32650042817700232218;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Eck4Test, MetadataAndSphericalEnforcement) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=eck4 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("eck4", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Eckert IV"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
}

TEST(Eck4Test, ForwardAnalyticalValuesAndPoleIterationExhaustionBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=eck4 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. At the equator (phi = 0), theta = 0, so x = 2 * C_x * lam and y = 0.
  const double lam = DegToRad(45.0);
  const PJ_COORD eq_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(2.0 * kCx * lam, eq_fwd.xy.x, 1e-12);
  EXPECT_NEAR(0.0, eq_fwd.xy.y, 1e-12);

  // 2. At the north and south poles (phi = +-pi/2), the derivative in Newton's
  // method vanishes at theta = +-pi/2 (double root), exhausting all NITER = 6
  // iterations (i == 0) and taking the (!i) pole branch for both signs of
  // theta: x = C_x * lam, y = +-C_y.
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kCx * lam, north_pole.xy.x, 1e-12);
  EXPECT_NEAR(kCy, north_pole.xy.y, 1e-12);

  const PJ_COORD south_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kCx * lam, south_pole.xy.x, 1e-12);
  EXPECT_NEAR(-kCy, south_pole.xy.y, 1e-12);
}

TEST(Eck4Test, ForwardAndInverseRoundTripAcrossGlobe) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=eck4 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  struct LonLatCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr LonLatCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 15.0, .lat_deg = 30.0},
      {.lon_deg = -75.0, .lat_deg = -45.0},
      {.lon_deg = 120.0, .lat_deg = 60.0},
      {.lon_deg = -160.0, .lat_deg = 80.0},
      {.lon_deg = 180.0, .lat_deg = -85.0},
      {.lon_deg = 45.0, .lat_deg = 90.0},
      {.lon_deg = -45.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lon_rad = DegToRad(tc.lon_deg);
    const double lat_rad = DegToRad(tc.lat_deg);

    // 1. Forward projection.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(fwd.xy.x));
    EXPECT_FALSE(std::isinf(fwd.xy.y));

    // 2. Inverse projection back to (lon, lat).
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-6);
    EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-6);
  }

  // Verify equal-area property (areal_scale == 1.0) at a mid-latitude point.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(DegToRad(20.0), DegToRad(40.0), 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-5);
}

TEST(Eck4Test, InverseNearPoleThresholdOverFlagAndDomainBoundaries) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=eck4 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Near-pole branch: 0 <= 1 - |sin_theta| <= 1e-12 for both y > 0 and y <
  // 0.
  const double x_in = kCx * 0.5;
  const PJ_COORD inv_north = proj_trans(
      pj.get(), PJ_INV, proj_coord(x_in, kCy * (1.0 - 5e-13), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.5, inv_north.lp.lam, 1e-9);
  EXPECT_NEAR(kHalfPi, inv_north.lp.phi, 1e-12);

  const PJ_COORD inv_south = proj_trans(
      pj.get(), PJ_INV, proj_coord(-x_in, -kCy * (1.0 - 5e-13), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-0.5, inv_south.lp.lam, 1e-9);
  EXPECT_NEAR(-kHalfPi, inv_south.lp.phi, 1e-12);

  // 2. Longitude slightly beyond +-pi within 1e-10 tolerance (without +over)
  // is clamped to +-pi.
  const double x_pos_clamp = 2.0 * kCx * (kPi + 5e-11);
  const PJ_COORD inv_pos_clamp =
      proj_trans(pj.get(), PJ_INV, proj_coord(x_pos_clamp, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(kPi, inv_pos_clamp.lp.lam);

  const PJ_COORD inv_neg_clamp =
      proj_trans(pj.get(), PJ_INV, proj_coord(-x_pos_clamp, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(-kPi, inv_neg_clamp.lp.lam);

  // 3. Longitude exceeding pi by more than 1e-10 (without +over) fails with
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  const double x_outside = 2.0 * kCx * (kPi + 0.1);
  const PJ_COORD inv_outside =
      proj_trans(pj.get(), PJ_INV, proj_coord(x_outside, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv_outside.lp.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 4. Y coordinate beyond +-C_y (|sin_theta| > 1 + 1e-7) fails in aasin.
  const PJ_COORD inv_y_outside =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, kCy * 1.1, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv_y_outside.lp.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 5. With +over enabled, longitudes beyond +-pi are returned unclamped.
  ScopedPj pj_over(proj_create(ctx.get(), "+proj=eck4 +R=1 +over"));
  ASSERT_NE(nullptr, pj_over.get());
  const PJ_COORD inv_over =
      proj_trans(pj_over.get(), PJ_INV, proj_coord(x_outside, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_over.get()));
  EXPECT_NEAR(kPi + 0.1, inv_over.lp.lam, 1e-12);
}

}  // namespace
}  // namespace proj
