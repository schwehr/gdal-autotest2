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
// Unit tests for the Compact Miller (comill) cylindrical projection
// (proj/src/projections/comill.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/comill.cpp

#include <array>
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

TEST(ComillTest, ForwardPolynomialAndInverseRoundTripAcrossLatitudes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=comill +R=1"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("comill", info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", info.description);
  EXPECT_STREQ("Compact Miller\n\tCyl, Sph", pj->descr);
  EXPECT_EQ(1, info.has_inverse);

  // 1. Verify exact polynomial formula:
  //    x = lam, y = phi * (K1 + phi^2 * (K2 + K3 * phi^2))
  //    with K1 = 0.9902, K2 = 0.1604, K3 = -0.03054.
  constexpr double kK1 = 0.9902;
  constexpr double kK2 = 0.1604;
  constexpr double kK3 = -0.03054;
  const double lam = DegToRad(40.0);
  const double phi = DegToRad(50.0);
  const double phi2 = phi * phi;
  const double expected_y = phi * (kK1 + phi2 * (kK2 + kK3 * phi2));

  const PJ_XY xy = pj_fwd({.lam = lam, .phi = phi}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, xy.x, 1e-15);
  EXPECT_NEAR(expected_y, xy.y, 1e-15);

  // 2. Round-trip from south pole to north pole.
  constexpr double kLatsDeg[] = {-90.0, -60.0, -30.0, 0.0, 30.0, 60.0, 90.0};
  for (const double lat_deg : kLatsDeg) {
    const double test_lam = DegToRad(-115.0);
    const double test_phi = DegToRad(lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(test_lam, test_phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(test_lam, inv.lp.lam, 1e-11);
    EXPECT_NEAR(test_phi, inv.lp.phi, 1e-11);
  }

  // 3. Batch transform and distortion factors.
  std::array<PJ_COORD, 2> batch = {
      proj_coord(DegToRad(15.0), DegToRad(25.0), 0.0, 0.0),
      proj_coord(DegToRad(-85.0), DegToRad(-45.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, batch.size(), batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, batch.size(), batch.data()));
  EXPECT_NEAR(DegToRad(-85.0), batch[1].lp.lam, 1e-11);
  EXPECT_NEAR(DegToRad(-45.0), batch[1].lp.phi, 1e-11);

  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(15.0), DegToRad(25.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(factors.meridional_scale, 0.0);
  EXPECT_GT(factors.parallel_scale, 0.0);
}

TEST(ComillTest, InverseClampsNorthingBeyondMaxYAndReportsNaNNonConvergence) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=comill +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. xy.y > MAX_Y (0.6000207669862655 * pi ≈ 1.88502) clamps to +MAX_Y,
  // which inverts to +pi/2.
  const PJ_LP north_clamped = pj_inv({.x = 0.5, .y = 3.0}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.5, north_clamped.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, north_clamped.phi, 1e-11);

  // 2. xy.y < -MAX_Y clamps to -MAX_Y, which inverts to -pi/2.
  const PJ_LP south_clamped = pj_inv({.x = -0.5, .y = -3.0}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-0.5, south_clamped.lam, 1e-12);
  EXPECT_NEAR(-kHalfPi, south_clamped.phi, 1e-11);

  // 3. NaN xy.y bypasses both < and > comparisons and exhausts all MAX_ITER=100
  // Newton-Raphson iterations (`i == 0`), setting
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN on P->ctx.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const PJ_LP raw_nan_inv = pj->inv({.x = 0.25, .y = nan}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_context_errno(ctx.get()));
  EXPECT_NEAR(0.25, raw_nan_inv.lam, 1e-12);
  EXPECT_TRUE(std::isnan(raw_nan_inv.phi));
  proj_errno_reset(pj.get());

  const PJ_LP nan_inv = pj_inv({.x = 0.25, .y = nan}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  EXPECT_EQ(HUGE_VAL, nan_inv.lam);
  EXPECT_EQ(HUGE_VAL, nan_inv.phi);
  proj_errno_reset(pj.get());
}

}  // namespace
}  // namespace proj
