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
// Unit tests for unit-ellipsoid meridian distance initialization, forward
// calculation, and inverse convergence/error paths (proj/src/proj_mdist.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/proj_mdist.cpp

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
constexpr double kWgs84A = 6378137.0;
constexpr double kWgs84InvF = 298.257223563;
constexpr double kWgs84F = 1.0 / kWgs84InvF;
constexpr double kWgs84Es = kWgs84F * (2.0 - kWgs84F);

TEST(ProjMdistTest, SphericalCaseReducesToIdentityAndRoundTrips) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedMalloc<void> en(proj_mdist_ini(0.0));
  ASSERT_NE(nullptr, en.get());

  constexpr double kLatitudes[] = {-kHalfPi,  -kPi / 3.0, -kPi / 6.0, 0.0,
                                   kPi / 6.0, kPi / 4.0,  kPi / 3.0,  kHalfPi};
  for (const double phi : kLatitudes) {
    const double m = proj_mdist(phi, std::sin(phi), std::cos(phi), en.get());
    EXPECT_DOUBLE_EQ(phi, m) << "phi=" << phi;

    const double phi_inv = proj_inv_mdist(ctx.get(), m, en.get());
    EXPECT_EQ(0, proj_context_errno(ctx.get()));
    EXPECT_NEAR(phi, phi_inv, 1e-14) << "phi=" << phi;
  }
}

TEST(ProjMdistTest, Wgs84AndBesselEllipsoidsMatchMeridianArcAndInvertCleanly) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. WGS84 unit meridian distance at North Pole scales to ~10,001,965.729 m.
  ScopedMalloc<void> wgs84_en(proj_mdist_ini(kWgs84Es));
  ASSERT_NE(nullptr, wgs84_en.get());
  const double quarter_meridian =
      kWgs84A * proj_mdist(kHalfPi, 1.0, 0.0, wgs84_en.get());
  EXPECT_NEAR(10001965.7293, quarter_meridian, 1e-4);

  // Equator has zero meridian distance and odd symmetry: M(-phi) == -M(phi).
  EXPECT_DOUBLE_EQ(0.0, proj_mdist(0.0, 0.0, 1.0, wgs84_en.get()));

  constexpr double kLatitudes[] = {-kHalfPi,   -kPi / 3.0, -kPi / 4.0,
                                   -kPi / 6.0, 0.0,        kPi / 6.0,
                                   kPi / 4.0,  kPi / 3.0,  kHalfPi};
  for (const double phi : kLatitudes) {
    const double m_pos =
        proj_mdist(phi, std::sin(phi), std::cos(phi), wgs84_en.get());
    const double m_neg =
        proj_mdist(-phi, std::sin(-phi), std::cos(-phi), wgs84_en.get());
    EXPECT_DOUBLE_EQ(-m_pos, m_neg) << "phi=" << phi;

    const double phi_inv = proj_inv_mdist(ctx.get(), m_pos, wgs84_en.get());
    EXPECT_EQ(0, proj_context_errno(ctx.get()));
    EXPECT_NEAR(phi, phi_inv, 1e-13) << "phi=" << phi;
  }

  // 2. Bessel 1841 ellipsoid (f = 1 / 299.1528128).
  constexpr double kBesselF = 1.0 / 299.1528128;
  constexpr double kBesselEs = kBesselF * (2.0 - kBesselF);
  ScopedMalloc<void> bessel_en(proj_mdist_ini(kBesselEs));
  ASSERT_NE(nullptr, bessel_en.get());
  const double m_45 = proj_mdist(kPi / 4.0, std::sin(kPi / 4.0),
                                 std::cos(kPi / 4.0), bessel_en.get());
  EXPECT_NEAR(kPi / 4.0, proj_inv_mdist(ctx.get(), m_45, bessel_en.get()),
              1e-13);
}

TEST(ProjMdistTest, HighEccentricityExhaustsInitSeriesLoopWithoutEarlyBreak) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // es = 0.95 ensures E[i] remains large through i = 19, completing all
  // MAX_ITER = 20 loop steps instead of breaking early on Es == El.
  ScopedMalloc<void> high_es_en(proj_mdist_ini(0.95));
  ASSERT_NE(nullptr, high_es_en.get());

  const double phi = 0.35;
  const double m =
      proj_mdist(phi, std::sin(phi), std::cos(phi), high_es_en.get());
  EXPECT_TRUE(std::isfinite(m));
  EXPECT_GT(m, 0.0);
  EXPECT_LT(m, phi);

  const double phi_inv = proj_inv_mdist(ctx.get(), m, high_es_en.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));
  EXPECT_NEAR(phi, phi_inv, 1e-10);
}

TEST(ProjMdistTest, InverseReportsDomainErrorWhenConvergenceFails) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedMalloc<void> en(proj_mdist_ini(kWgs84Es));
  ASSERT_NE(nullptr, en.get());

  // Non-finite distance never satisfies |t| < TOL, exercising the MAX_ITER
  // exhaustion error branch cleanly without undefined division by zero.
  const double nan_phi = proj_inv_mdist(
      ctx.get(), std::numeric_limits<double>::quiet_NaN(), en.get());
  EXPECT_TRUE(std::isnan(nan_phi));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_context_errno(ctx.get()));
}

TEST(ProjMdistTest, RoussProjectionEndToEndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj rouss(
      proj_create(ctx.get(), "+proj=rouss +ellps=WGS84 +lat_0=45 +lon_0=2"));
  ASSERT_NE(nullptr, rouss.get());

  const PJ_COORD origin_pt =
      proj_coord(2.5 * (kPi / 180.0), 45.5 * (kPi / 180.0), 0.0, 0.0);
  const PJ_COORD fwd = proj_trans(rouss.get(), PJ_FWD, origin_pt);
  EXPECT_EQ(0, proj_errno(rouss.get()));
  EXPECT_TRUE(std::isfinite(fwd.xy.x));
  EXPECT_TRUE(std::isfinite(fwd.xy.y));

  const PJ_COORD inv = proj_trans(rouss.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(rouss.get()));
  EXPECT_NEAR(origin_pt.lp.lam, inv.lp.lam, 1e-10);
  EXPECT_NEAR(origin_pt.lp.phi, inv.lp.phi, 1e-10);
}

}  // namespace
}  // namespace proj
