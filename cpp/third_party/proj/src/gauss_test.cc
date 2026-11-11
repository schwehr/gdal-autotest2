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
// Unit tests for Gauss conformal projection of ellipsoid onto sphere
// initialization, forward/inverse transforms, and failure branches
// (proj/src/gauss.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/gauss.cpp

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
constexpr double kDegToRad = kPi / 180.0;
constexpr double kWgs84F = 1.0 / 298.257223563;
constexpr double kWgs84E = 0.0818191908426215;

TEST(GaussTest, SphericalInitReducesToIdentityAndRoundTrips) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  double chi = 0.0;
  double rc = 0.0;
  const double phi0 = 45.0 * kDegToRad;
  ScopedMalloc<void> en(pj_gauss_ini(0.0, phi0, &chi, &rc));
  ASSERT_NE(nullptr, en.get());
  EXPECT_DOUBLE_EQ(phi0, chi);
  EXPECT_DOUBLE_EQ(1.0, rc);

  const PJ_LP in_lp = {.lam = 15.0 * kDegToRad, .phi = -30.0 * kDegToRad};
  const PJ_LP fwd = pj_gauss(ctx.get(), in_lp, en.get());
  EXPECT_NEAR(in_lp.lam, fwd.lam, 1e-14);
  EXPECT_NEAR(in_lp.phi, fwd.phi, 1e-14);

  const PJ_LP inv = pj_inv_gauss(ctx.get(), fwd, en.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));
  EXPECT_NEAR(in_lp.lam, inv.lam, 1e-14);
  EXPECT_NEAR(in_lp.phi, inv.phi, 1e-14);
}

TEST(GaussTest, EllipsoidalRoundTripAcrossOriginsAndLatitudes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Mid-latitude origin (Amersfoort ~52.156N) and equatorial origin.
  const double origins[] = {0.0, 52.15616055555555 * kDegToRad,
                            -35.0 * kDegToRad, kHalfPi};
  for (const double phi0 : origins) {
    double chi = 0.0;
    double rc = 0.0;
    ScopedMalloc<void> en(pj_gauss_ini(kWgs84E, phi0, &chi, &rc));
    ASSERT_NE(nullptr, en.get()) << "phi0=" << phi0;
    EXPECT_GT(rc, 0.99);
    EXPECT_LE(std::abs(chi), kHalfPi + 1e-12);

    // At phi = phi0, conformal latitude maps directly to chi.
    const PJ_LP at_origin = {.lam = 0.0, .phi = phi0};
    const PJ_LP fwd_origin = pj_gauss(ctx.get(), at_origin, en.get());
    EXPECT_NEAR(0.0, fwd_origin.lam, 1e-14);
    EXPECT_NEAR(chi, fwd_origin.phi, 1e-12);

    constexpr PJ_LP kPoints[] = {
        {.lam = 0.0, .phi = 0.0},
        {.lam = 10.0 * kDegToRad, .phi = 45.0 * kDegToRad},
        {.lam = -120.0 * kDegToRad, .phi = -60.0 * kDegToRad},
        {.lam = kPi, .phi = 89.0 * kDegToRad},
        {.lam = -kPi, .phi = -89.0 * kDegToRad},
    };
    for (const PJ_LP& pt : kPoints) {
      const PJ_LP slp = pj_gauss(ctx.get(), pt, en.get());
      const PJ_LP elp = pj_inv_gauss(ctx.get(), slp, en.get());
      EXPECT_EQ(0, proj_context_errno(ctx.get()));
      EXPECT_NEAR(pt.lam, elp.lam, 1e-13) << "phi0=" << phi0;
      EXPECT_NEAR(pt.phi, elp.phi, 1e-13) << "phi0=" << phi0;
    }
  }
}

TEST(GaussTest, SouthPoleOriginBranchAndExtremeEccentricityUnderflow) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. South pole phi0 = -pi/2 hits (.5 * phi0 + M_FORTPI < 1e-10) branch.
  double chi = 0.0;
  double rc = 0.0;
  ScopedMalloc<void> south_en(pj_gauss_ini(kWgs84E, -kHalfPi, &chi, &rc));
  ASSERT_NE(nullptr, south_en.get());
  EXPECT_DOUBLE_EQ(-kHalfPi, chi);
  const double expected_rc = 1.0 / std::sqrt(1.0 - kWgs84F * (2.0 - kWgs84F));
  EXPECT_NEAR(expected_rc, rc, 1e-14);

  const PJ_LP near_south = {.lam = 0.25, .phi = -80.0 * kDegToRad};
  const PJ_LP slp = pj_gauss(ctx.get(), near_south, south_en.get());
  const PJ_LP elp = pj_inv_gauss(ctx.get(), slp, south_en.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));
  EXPECT_NEAR(near_south.lam, elp.lam, 1e-13);
  EXPECT_NEAR(near_south.phi, elp.phi, 1e-13);

  // 2. Extreme eccentricity e < 1 with large phi0 underflows srat_val to 0.0,
  // triggering free(en) + nullptr return cleanly.
  double fail_chi = 0.0;
  double fail_rc = 0.0;
  EXPECT_EQ(nullptr, pj_gauss_ini(0.99999999, 0.5, &fail_chi, &fail_rc));
}

TEST(GaussTest, InvGaussReportsDomainErrorWhenConvergenceFails) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  double chi = 0.0;
  double rc = 0.0;
  ScopedMalloc<void> en(pj_gauss_ini(kWgs84E, 45.0 * kDegToRad, &chi, &rc));
  ASSERT_NE(nullptr, en.get());

  // NaN input prevents |elp.phi - slp.phi| < DEL_TOL, exhausting all MAX_ITER
  // iterations and setting PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  const PJ_LP nan_slp = {.lam = 0.0,
                         .phi = std::numeric_limits<double>::quiet_NaN()};
  const PJ_LP bad = pj_inv_gauss(ctx.get(), nan_slp, en.get());
  EXPECT_TRUE(std::isnan(bad.phi));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_context_errno(ctx.get()));
}

TEST(GaussTest, StereaProjectionEndToEndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj sterea(proj_create(
      ctx.get(),
      "+proj=sterea +lat_0=52.15616055555555 +lon_0=5.38763888888889 "
      "+k=0.9999079 +x_0=155000 +y_0=463000 +ellps=bessel"));
  ASSERT_NE(nullptr, sterea.get());

  const PJ_COORD amersfoort = proj_coord(5.38763888888889 * kDegToRad,
                                         52.15616055555555 * kDegToRad, 0, 0);
  const PJ_COORD fwd = proj_trans(sterea.get(), PJ_FWD, amersfoort);
  EXPECT_EQ(0, proj_errno(sterea.get()));
  EXPECT_NEAR(155000.0, fwd.xy.x, 1e-4);
  EXPECT_NEAR(463000.0, fwd.xy.y, 1e-4);

  const PJ_COORD pt = proj_coord(4.9 * kDegToRad, 52.37 * kDegToRad, 0, 0);
  const PJ_COORD pt_xy = proj_trans(sterea.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(sterea.get()));
  const PJ_COORD pt_rt = proj_trans(sterea.get(), PJ_INV, pt_xy);
  EXPECT_EQ(0, proj_errno(sterea.get()));
  EXPECT_NEAR(pt.lp.lam, pt_rt.lp.lam, 1e-11);
  EXPECT_NEAR(pt.lp.phi, pt_rt.lp.phi, 1e-11);
}

}  // namespace
}  // namespace proj
