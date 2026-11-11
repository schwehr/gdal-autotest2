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
// Unit tests for the van der Grinten (I) world projection
// (proj/src/projections/vandg.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/vandg.cpp

#include <cmath>
#include <limits>
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

TEST(VandgTest, ForwardAndInverseSpecialAxesPolesAndQuadrantsRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=vandg +R=6371000"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  // 1. Origin (0, 0) exercises |phi| <= TOL in forward and |y| < TOL, |x| <=
  // TOL in inverse.
  const PJ_COORD origin_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, origin_fwd.xy.x);
  EXPECT_DOUBLE_EQ(0.0, origin_fwd.xy.y);

  const PJ_COORD origin_inv = proj_trans(pj.get(), PJ_INV, origin_fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, origin_inv.lp.lam);
  EXPECT_DOUBLE_EQ(0.0, origin_inv.lp.phi);

  // 2. Equator (phi = 0, lam != 0), prime meridian (lam = 0, phi != 0), and
  // all four general quadrants round-trip accurately.
  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 45.0, .lat_deg = 0.0},
      {.lon_deg = -90.0, .lat_deg = 0.0},
      {.lon_deg = 0.0, .lat_deg = 45.0},
      {.lon_deg = 0.0, .lat_deg = -45.0},
      {.lon_deg = 60.0, .lat_deg = 30.0},
      {.lon_deg = -60.0, .lat_deg = 30.0},
      {.lon_deg = 60.0, .lat_deg = -30.0},
      {.lon_deg = -60.0, .lat_deg = -30.0},
      {.lon_deg = 175.0, .lat_deg = 75.0},
      {.lon_deg = -175.0, .lat_deg = -75.0},
  };

  for (const auto& tc : kCases) {
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-9);
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-9);
  }

  // 3. North and South Poles (|p2 - 1| < TOL) with non-zero longitude map to
  // x = 0, y = +-pi * R.
  const PJ_COORD npole = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(50.0), kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, npole.xy.x);
  EXPECT_NEAR(kPi * 6371000.0, npole.xy.y, 1e-3);

  const PJ_COORD spole = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(50.0), -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, spole.xy.x);
  EXPECT_NEAR(-kPi * 6371000.0, spole.xy.y, 1e-3);
}

TEST(VandgTest, OverFlagWithLongitudeBeyond180RoundTripsViaRGreaterThanPiSq) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=vandg +R=1 +over"));
  ASSERT_NE(nullptr, pj.get());

  // With +over and |lam| > pi, vandg_s_forward sets sign = -1 and projects
  // outside the r = pi^2 circle; vandg_s_inverse takes the `if (r > PISQ)`
  // branch (`d = M_TWOPI - d`) to recover the original coordinates.
  const PJ_LP in = {.lam = DegToRad(210.0), .phi = DegToRad(35.0)};
  const PJ_XY fwd = pj_fwd(in, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(fwd.x * fwd.x + fwd.y * fwd.y, kPi * kPi);

  const PJ_LP inv = pj_inv(fwd, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.lam, inv.lam, 1e-10);
  EXPECT_NEAR(in.phi, inv.phi, 1e-10);
}

TEST(VandgTest, ForwardAndInverseDomainErrorAndClampingBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=vandg +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Latitude slightly above pi/2 within TOL (1.0 < p2 <= 1.0 + 1e-10) is
  // clamped via `if (p2 > 1.) p2 = 1.`. Call `pj->fwd` directly so
  // `fwd_prepare`'s PJ_EPS_LAT (1e-12) pre-check does not intercept it first.
  const PJ_XY clamped =
      pj->fwd({.lam = 0.5, .phi = kHalfPi * (1.0 + 0.5e-10)}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, clamped.x);
  EXPECT_NEAR(kPi, clamped.y, 1e-9);

  // 2. Latitude exceeding pi/2 beyond TOL triggers `(p2 - TOL) > 1.` error
  // inside vandg_s_forward when called directly via `pj->fwd`.
  proj_errno_reset(pj.get());
  (void)pj->fwd({.lam = 0.0, .phi = kHalfPi + 1e-4}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));

  // 3. Extremely large y in vandg_s_inverse causes |al * m| < 1e-16 and
  // returns PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  proj_errno_reset(pj.get());
  const PJ_LP huge_y_inv = pj_inv({.x = 0.0, .y = 1e60}, pj.get());
  EXPECT_EQ(HUGE_VAL, huge_y_inv.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));

  // 4. NaN x with finite non-zero y exercises the `(t - TOL) <= 1.` false
  // branch in vandg_s_inverse.
  proj_errno_reset(pj.get());
  (void)pj_inv({.x = std::numeric_limits<double>::quiet_NaN(), .y = 1.0},
               pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));

  // 5. Verify proj_factors on van der Grinten.
  proj_errno_reset(pj.get());
  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(factors.areal_scale, 0.0);
}

}  // namespace
}  // namespace proj
