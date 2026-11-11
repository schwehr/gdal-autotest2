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
// Unit tests for Bipolar conic of western hemisphere (bipc) projection
// (proj/src/projections/bipc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/bipc.cpp

#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;
constexpr double kLamB = -0.34894976726250681539;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(BipcTest,
     SkewedAndNoskewForwardInverseRoundTripBothConesAndTransitionZone) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  for (const char* def :
       {"+proj=bipc +R=6371000", "+proj=bipc +R=6371000 +ns"}) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    EXPECT_EQ(1, pj_has_inverse(pj.get()));

    struct CoordCase {
      const char* label;
      double lon_rad;
      double lat_rad;
      double expected_inv_lon_rad;
      double expected_inv_lat_rad;
    };
    constexpr CoordCase kCases[] = {
        // Northern cone (tag == 0, |t| >= al): North America.
        {.label = "NorthConeInterior",
         .lon_rad = DegToRad(-95.0),
         .lat_rad = DegToRad(40.0),
         .expected_inv_lon_rad = DegToRad(-95.0),
         .expected_inv_lat_rad = DegToRad(40.0)},
        // Southern cone (tag == 1, |t| >= al): South America.
        {.label = "SouthConeInterior",
         .lon_rad = DegToRad(-60.0),
         .lat_rad = DegToRad(-20.0),
         .expected_inv_lon_rad = DegToRad(-60.0),
         .expected_inv_lat_rad = DegToRad(-20.0)},
        // Northern cone transition blend zone (tag == 0, |t| < al,
        // triggers r /= cos(al - t) in bipc_s_forward and fAz < al in inverse;
        // forward uses pow(0.5*(R104-z), n) while inverse uses
        // pow(tan(0.5*(R104-z)), n)).
        {.label = "NorthConeBlendZone",
         .lon_rad = -1.1,
         .lat_rad = 0.46,
         .expected_inv_lon_rad = -1.1349129094250823,
         .expected_inv_lat_rad = 0.43321717189833497},
        // Southern cone transition blend zone (tag == 1, |t| < al,
        // triggers r /= cos(al + t) in bipc_s_forward and fAz < al in inverse).
        {.label = "SouthConeBlendZone",
         .lon_rad = -1.1,
         .lat_rad = 0.25,
         .expected_inv_lon_rad = -1.0972445441752718,
         .expected_inv_lat_rad = 0.2519025445736876},
    };

    for (const auto& tc : kCases) {
      // 1. Forward projection.
      const PJ_COORD in = proj_coord(tc.lon_rad, tc.lat_rad, 0.0, 0.0);
      const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj.get())) << def << " " << tc.label;
      EXPECT_FALSE(std::isinf(fwd.xy.x)) << def << " " << tc.label;
      EXPECT_FALSE(std::isinf(fwd.xy.y)) << def << " " << tc.label;

      // 2. Inverse projection round-trip.
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << def << " " << tc.label;
      EXPECT_NEAR(tc.expected_inv_lon_rad, inv.lp.lam, 1e-6)
          << def << " " << tc.label;
      EXPECT_NEAR(tc.expected_inv_lat_rad, inv.lp.phi, 1e-6)
          << def << " " << tc.label;
    }
  }
}

TEST(BipcTest, ForwardGeographicPolesAndOutsideDomainBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppressor(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=bipc +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Exact North Pole (lp.phi == +pi/2) hits fabs(fabs(lp.phi) - M_HALFPI) <
  // EPS10 with Az = 0, tphi = HUGE_VAL, tag = 0.
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_FALSE(std::isinf(north_pole.xy.x));
  EXPECT_FALSE(std::isinf(north_pole.xy.y));

  // 2. Exact South Pole (lp.phi == -pi/2) hits fabs(fabs(lp.phi) - M_HALFPI) <
  // EPS10 with Az = M_PI, tphi = HUGE_VAL, tag = 1 (and skips Az recomputation
  // since tphi == HUGE_VAL).
  const PJ_COORD south_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_FALSE(std::isinf(south_pole.xy.x));
  EXPECT_FALSE(std::isinf(south_pole.xy.y));

  // 3. Point with spherical distance z > R104 (104 deg) from Pole B makes
  // al = 0.5 * (R104 - z) < 0 and sets
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  const PJ_COORD far_pt =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLamB + 2.8, -0.5, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, far_pt.xy.x);
  EXPECT_EQ(HUGE_VAL, far_pt.xy.y);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(BipcTest, InverseNonConvergenceSetsOutsideProjectionDomainErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppressor(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=bipc +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // Passing NaN directly to pj_inv runs all NITER = 10 iterations in
  // bipc_s_inverse and hits the !i error return.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const PJ_LP lp = pj_inv({.x = nan, .y = nan}, pj.get());
  (void)lp;
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // Directed IEEE-754 rounding modes exercise the 1-ULP clamping branches
  // (1.0 < fabs(z) <= ONEEPS) in both tag==1 (Az > Azba) and tag==0
  // (Az <= Azba) cones as well as the z < 0.0 domain rejection after clamping
  // z to -1.0.
  const int saved_round = std::fegetround();
  absl::Cleanup restore_round = [saved_round] { std::fesetround(saved_round); };
  constexpr double kC20 = 0.93969262078590838411;
  constexpr double kS20 = -0.34202014332566873287;
  constexpr double kR110 = 1.91986217719376253360;

  if (std::fesetround(FE_UPWARD) == 0) {
    const PJ_XY up_tag1 = pj_fwd(
        {.lam = -kR110, .phi = std::atan2(kS20, kC20) - 1e-12}, pj.get());
    EXPECT_FALSE(std::isnan(up_tag1.x));
    proj_errno_reset(pj.get());

    const PJ_XY up_tag0 = pj_fwd(
        {.lam = kLamB, .phi = 0.25 * std::numbers::pi + 1e-12}, pj.get());
    EXPECT_FALSE(std::isnan(up_tag0.x));
    proj_errno_reset(pj.get());
  }
  if (std::fesetround(FE_DOWNWARD) == 0) {
    const PJ_XY down_tag0 = pj_fwd(
        {.lam = kLamB + std::numbers::pi, .phi = -0.25 * std::numbers::pi},
        pj.get());
    (void)down_tag0;
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }
}

// TODO(schwehr): The remaining uncovered branches in bipc.cpp (lines 136-137,
// 195-196, 200-202, 232-233, 241, 249, 255) are defensive >ONEEPS acos/asin
// domain guards and the pj_calloc failure guard in PROJ(bipc).

}  // namespace
}  // namespace proj
