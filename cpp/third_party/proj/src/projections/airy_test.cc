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
// Unit tests for the Airy minimum-error azimuthal projection
// (proj/src/projections/airy.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/airy.cpp

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

TEST(AiryTest, EquatorialModeForwardCenterHemisphereCutAndNoCutAntipode) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  // 1. Default equatorial mode (lat_0=0, lat_b=0, no_cut=0).
  ScopedPj eq_pj(proj_create(ctx.get(), "+proj=airy +R=6371000"));
  ASSERT_NE(nullptr, eq_pj.get());
  // Airy is forward-only (no inverse).
  EXPECT_EQ(0, pj_has_inverse(eq_pj.get()));

  // Center of projection (lon=0, lat=0) exercises |1 - cosz| <= EPS (Krho = 0.5
  // - Cb) and maps to (0, 0).
  const PJ_COORD origin =
      proj_trans(eq_pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(eq_pj.get()));
  EXPECT_DOUBLE_EQ(0.0, origin.xy.x);
  EXPECT_DOUBLE_EQ(0.0, origin.xy.y);

  // Off-center point in visible hemisphere (cosz > 0).
  const PJ_COORD pt =
      proj_trans(eq_pj.get(), PJ_FWD,
                 proj_coord(DegToRad(30.0), DegToRad(20.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(eq_pj.get()));
  EXPECT_GT(pt.xy.x, 0.0);
  EXPECT_GT(pt.xy.y, 0.0);

  // Opposite hemisphere point (lon=120 deg, lat=0 -> cosz = -0.5 < -EPS) fails
  // when +no_cut is not set.
  const PJ_COORD cut_err = proj_trans(
      eq_pj.get(), PJ_FWD, proj_coord(DegToRad(120.0), 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, cut_err.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(eq_pj.get()));

  // 2. With +no_cut and +lat_b=90 (beta = 0 -> Cb = -0.5), lon=120 deg
  // succeeds, while the exact antipode (lon=180 deg, lat=0 -> cosz = -1, t = 0)
  // triggers the `t == 0` domain error branch.
  ScopedPj nocut_pj(
      proj_create(ctx.get(), "+proj=airy +R=6371000 +no_cut +lat_b=90"));
  ASSERT_NE(nullptr, nocut_pj.get());

  const PJ_COORD back_hemisphere =
      proj_trans(nocut_pj.get(), PJ_FWD,
                 proj_coord(DegToRad(120.0), DegToRad(10.0), 0, 0));
  EXPECT_EQ(0, proj_errno(nocut_pj.get()));
  EXPECT_GT(back_hemisphere.xy.x, 0.0);
  EXPECT_GT(back_hemisphere.xy.y, 0.0);

  proj_errno_reset(nocut_pj.get());
  (void)pj_fwd({.lam = kPi, .phi = 0.0}, nocut_pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(nocut_pj.get()));
}

TEST(AiryTest, ObliqueModeForwardCenterOffCenterAndHemisphereCut) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  ScopedPj obliq_pj(
      proj_create(ctx.get(), "+proj=airy +lat_0=45 +lat_b=30 +R=6371000"));
  ASSERT_NE(nullptr, obliq_pj.get());

  // 1. Projection center (lon=0, lat=45) maps to (0, 0) via |1 - cosz| <= EPS.
  const PJ_COORD center = proj_trans(obliq_pj.get(), PJ_FWD,
                                     proj_coord(0.0, DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(obliq_pj.get()));
  EXPECT_NEAR(0.0, center.xy.x, 1e-9);
  EXPECT_NEAR(0.0, center.xy.y, 1e-9);

  // 2. General oblique point exercises oblique cosz and xy.y formula.
  const PJ_COORD general = proj_trans(
      obliq_pj.get(), PJ_FWD, proj_coord(DegToRad(20.0), DegToRad(55.0), 0, 0));
  EXPECT_EQ(0, proj_errno(obliq_pj.get()));
  EXPECT_GT(general.xy.x, 0.0);
  EXPECT_GT(general.xy.y, 0.0);

  // 3. Point in opposite oblique hemisphere triggers cosz < -EPS cut error.
  const PJ_COORD antipodal = proj_trans(
      obliq_pj.get(), PJ_FWD, proj_coord(kPi, DegToRad(-45.0), 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, antipodal.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(obliq_pj.get()));
}

TEST(AiryTest, NorthAndSouthPolarModesPoleSingularityAndHemisphereCut) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  // 1. North Polar mode (+lat_0=90).
  ScopedPj npole_pj(proj_create(ctx.get(), "+proj=airy +lat_0=90 +R=6371000"));
  ASSERT_NE(nullptr, npole_pj.get());

  // Exact North Pole (lp.phi <= EPS after |pi/2 - phi|) maps to (0, 0).
  const PJ_COORD npole_center =
      proj_trans(npole_pj.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(npole_pj.get()));
  EXPECT_DOUBLE_EQ(0.0, npole_center.xy.x);
  EXPECT_DOUBLE_EQ(0.0, npole_center.xy.y);

  // Point at lon=0, lat=45 deg in N_POLE has x=0 and y < 0 (xy.y = -xy.y).
  const PJ_COORD npole_pt = proj_trans(
      npole_pj.get(), PJ_FWD, proj_coord(0.0, DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(npole_pj.get()));
  EXPECT_NEAR(0.0, npole_pt.xy.x, 1e-9);
  EXPECT_LT(npole_pt.xy.y, 0.0);

  // Southern latitude in N_POLE without +no_cut triggers (lp.phi - EPS) >
  // M_HALFPI domain error.
  const PJ_COORD npole_cut = proj_trans(
      npole_pj.get(), PJ_FWD, proj_coord(0.0, DegToRad(-10.0), 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, npole_cut.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(npole_pj.get()));

  // 2. South Polar mode (+lat_0=-90) with +no_cut.
  ScopedPj spole_pj(
      proj_create(ctx.get(), "+proj=airy +lat_0=-90 +no_cut +R=6371000"));
  ASSERT_NE(nullptr, spole_pj.get());

  // Exact South Pole maps to (0, 0).
  const PJ_COORD spole_center =
      proj_trans(spole_pj.get(), PJ_FWD, proj_coord(0.0, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(spole_pj.get()));
  EXPECT_DOUBLE_EQ(0.0, spole_center.xy.x);
  EXPECT_DOUBLE_EQ(0.0, spole_center.xy.y);

  // Point at lon=0, lat=-45 deg in S_POLE has x=0 and y > 0.
  const PJ_COORD spole_pt = proj_trans(
      spole_pj.get(), PJ_FWD, proj_coord(0.0, DegToRad(-45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(spole_pj.get()));
  EXPECT_NEAR(0.0, spole_pt.xy.x, 1e-9);
  EXPECT_GT(spole_pt.xy.y, 0.0);

  // Northern hemisphere point succeeds when +no_cut is enabled.
  const PJ_COORD spole_nocut = proj_trans(
      spole_pj.get(), PJ_FWD, proj_coord(0.0, DegToRad(20.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(spole_pj.get()));
  EXPECT_GT(spole_nocut.xy.y, spole_pt.xy.y);

  // 3. Verify proj_factors on Airy.
  const PJ_FACTORS factors = proj_factors(
      spole_pj.get(), proj_coord(DegToRad(15.0), DegToRad(-60.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(spole_pj.get()));
  EXPECT_GT(factors.meridional_scale, 0.0);
  EXPECT_GT(factors.parallel_scale, 0.0);
}

}  // namespace
}  // namespace proj
