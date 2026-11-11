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
// Unit tests for the McBryde-Thomas Flat-Polar Quartic projection
// (proj/src/projections/mbtfpq.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/mbtfpq.cpp

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
constexpr double kFyc = 1.87475828462269495505;
constexpr double kRyc = 0.53340209679417701685;
constexpr double kFxc = 0.31245971410378249250;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(MbtfpqTest, MetadataAndSphericalEnforcement) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=mbtfpq +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("mbtfpq", info.id);
  EXPECT_NE(
      std::string_view(pj->descr).find("McBryde-Thomas Flat-Polar Quartic"),
      std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
}

TEST(MbtfpqTest, ForwardAndInverseRoundTripAndFlatPolarGeometry) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=mbtfpq +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. At the equator (phi = 0), theta = 0, so x = 3 * FXC * lam and y = 0.
  const double lam = DegToRad(60.0);
  const PJ_COORD eq_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(3.0 * kFxc * lam, eq_fwd.xy.x, 1e-12);
  EXPECT_NEAR(0.0, eq_fwd.xy.y, 1e-12);

  // 2. At the poles (phi = +-pi/2), theta = +-pi/2, so sin(theta/2) =
  // +-sqrt(2)/2 and cos(theta) = 0, giving flat-polar line x = FXC * lam, y =
  // +-FYC * sqrt(2)/2.
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kFxc * lam, north_pole.xy.x, 1e-7);
  EXPECT_NEAR(kFyc * (std::numbers::sqrt2 / 2.0), north_pole.xy.y, 1e-7);

  // 3. Round-trip across all quadrants and poles.
  struct LonLatCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr LonLatCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 25.0, .lat_deg = 35.0},
      {.lon_deg = -110.0, .lat_deg = -50.0},
      {.lon_deg = 170.0, .lat_deg = 75.0},
      {.lon_deg = -180.0, .lat_deg = -85.0},
      {.lon_deg = 45.0, .lat_deg = 90.0},
      {.lon_deg = -45.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lon_rad = DegToRad(tc.lon_deg);
    const double lat_rad = DegToRad(tc.lat_deg);

    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-6);
    EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-6);
  }

  // 4. Verify equal-area scale factor (areal_scale == 1.0).
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(DegToRad(30.0), DegToRad(40.0), 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-5);
}

TEST(MbtfpqTest, InverseBoundaryClampingAndOutOfDomainBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=mbtfpq +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. First domain check on |RYC * xy.y|:
  //    a) |RYC * xy.y| > ONETOL (1.000001) fails with
  //    OUTSIDE_PROJECTION_DOMAIN.
  const PJ_COORD outer_over =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 2.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, outer_over.lp.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  //    b) 1.0 < RYC * xy.y <= ONETOL (positive and negative) exercises the
  //       clamping branches t = +-1, lp.phi = +-M_PI.
  const double y_outer_tol = 1.0000005 / kRyc;
  const PJ_COORD outer_pos_clamp =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, y_outer_tol, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_FALSE(std::isnan(outer_pos_clamp.lp.phi));

  const PJ_COORD outer_neg_clamp =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, -y_outer_tol, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_FALSE(std::isnan(outer_neg_clamp.lp.phi));

  // 2. Second domain check on |RC * (t + sin(2 * asin(t)))| when |t| <= 1:
  //    At t = sqrt(2)/2 (~0.70710678), RC * (t + sin(2*asin(t))) == 1.0.
  //    a) t = +-0.7071072 gives 1.0 < |RC * (t + sin(lp.phi))| <= ONETOL,
  //       clamping lp.phi to +-M_HALFPI.
  const double y_inner_tol = 0.7071072 / kRyc;
  const PJ_COORD inner_pos_clamp =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, y_inner_tol, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(kHalfPi, inner_pos_clamp.lp.phi);

  const PJ_COORD inner_neg_clamp =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, -y_inner_tol, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(-kHalfPi, inner_neg_clamp.lp.phi);

  //    b) t = 0.8 gives |t| <= 1.0 on the first check, but
  //       RC * (0.8 + sin(2*asin(0.8))) = RC * 1.76 = 1.031 > ONETOL on the
  //       second check, failing with OUTSIDE_PROJECTION_DOMAIN.
  const double y_inner_over = 0.8 / kRyc;
  const PJ_COORD inner_over =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, y_inner_over, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inner_over.lp.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

}  // namespace
}  // namespace proj
