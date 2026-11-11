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
// Unit tests for inverse isometric latitude solver pj_sinhpsi2tanphi() and
// pj_phi2() (proj/src/phi2.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/phi2.cpp

#include <algorithm>
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
constexpr double kWgs84InvF = 298.257223563;
constexpr double kWgs84F = 1.0 / kWgs84InvF;
constexpr double kWgs84ESq = kWgs84F * (2.0 - kWgs84F);

double Wgs84Eccentricity() { return std::sqrt(kWgs84ESq); }

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Phi2Test, SinhPsi2TanPhiInvertsConformalTangentAcrossAllRanges) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  const double wgs84_e = Wgs84Eccentricity();

  // 1. Sphere (e == 0): tan(phi) equals tan(chi) = tau'.
  for (const double taup : {0.0, -0.0, 0.5, -1.5, 70.0, 71.0, -100.0}) {
    EXPECT_NEAR(taup, pj_sinhpsi2tanphi(ctx.get(), taup, 0.0), 1e-15);
    EXPECT_EQ(0, proj_context_errno(ctx.get()));
  }

  // 2. WGS84 ellipsoid from equator through mid-latitudes to |tau'| > 70.
  constexpr double kLatDegs[] = {0.0,   1e-7, 15.0, -33.5, 45.0,
                                 -65.0, 88.0, 89.5, -89.8, 89.99};
  for (const double lat_deg : kLatDegs) {
    const double phi = DegToRad(lat_deg);
    const double tau = std::tan(phi);
    const double tau1 = std::hypot(1.0, tau);
    const double sig = std::sinh(wgs84_e * std::atanh(wgs84_e * tau / tau1));
    const double taup = std::hypot(1.0, sig) * tau - sig * tau1;

    const double recovered = pj_sinhpsi2tanphi(ctx.get(), taup, wgs84_e);
    EXPECT_EQ(0, proj_context_errno(ctx.get())) << lat_deg;
    EXPECT_NEAR(tau, recovered, 1e-13 * std::max(1.0, std::abs(tau)))
        << lat_deg;
  }

  // Boundary around |taup| == 70 triggers both initial-guess branches.
  const double at_70 = pj_sinhpsi2tanphi(ctx.get(), 70.0, wgs84_e);
  const double above_70 = pj_sinhpsi2tanphi(ctx.get(), 70.1, wgs84_e);
  const double neg_above_70 = pj_sinhpsi2tanphi(ctx.get(), -100.0, wgs84_e);
  EXPECT_EQ(0, proj_context_errno(ctx.get()));
  EXPECT_GT(above_70, at_70);
  EXPECT_LT(neg_above_70, -100.0);
}

TEST(Phi2Test, SinhPsi2TanPhiHandlesDomainLimitsInfNanAndNonConvergence) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  const double wgs84_e = Wgs84Eccentricity();

  // 1. Large argument exceeding tmax = 2 / sqrt(eps) returns asymptotic value
  // immediately without entering Newton iterations.
  const double scale = std::exp(wgs84_e * std::atanh(wgs84_e));
  EXPECT_NEAR(1e9 * scale, pj_sinhpsi2tanphi(ctx.get(), 1e9, wgs84_e), 1e-4);
  EXPECT_NEAR(-1e9 * scale, pj_sinhpsi2tanphi(ctx.get(), -1e9, wgs84_e), 1e-4);
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  // 2. +/-Inf and NaN propagate without setting errno.
  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(kInf, pj_sinhpsi2tanphi(ctx.get(), kInf, wgs84_e));
  EXPECT_EQ(-kInf, pj_sinhpsi2tanphi(ctx.get(), -kInf, wgs84_e));
  EXPECT_TRUE(std::isnan(pj_sinhpsi2tanphi(ctx.get(), kNan, wgs84_e)));
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  // 3. Degenerate eccentricity e = 1 hits !(fabs(tau) < tmax) early return.
  EXPECT_EQ(kInf, pj_sinhpsi2tanphi(ctx.get(), 1.0, 1.0));
  EXPECT_TRUE(std::isnan(pj_sinhpsi2tanphi(ctx.get(), 0.0, 1.0)));
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  // 4. Extreme eccentricity (e = 0.9999) fails to converge within 5 Newton
  // iterations and sets PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE.
  const double unconverged = pj_sinhpsi2tanphi(ctx.get(), 0.1, 0.9999);
  EXPECT_TRUE(std::isfinite(unconverged));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);
}

TEST(Phi2Test, Phi2RoundTripsTsfnForSphereAndEllipsoid) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  const double wgs84_e = Wgs84Eccentricity();

  constexpr double kLatDegs[] = {0.0, 15.0, -30.0, 45.0, -60.0, 85.0, -89.5};
  for (const double e : {0.0, wgs84_e}) {
    for (const double lat_deg : kLatDegs) {
      const double phi = DegToRad(lat_deg);
      const double ts = pj_tsfn(phi, std::sin(phi), e);
      const double recovered = pj_phi2(ctx.get(), ts, e);
      EXPECT_EQ(0, proj_context_errno(ctx.get()));
      EXPECT_NEAR(phi, recovered, 1e-12) << "e=" << e << " lat=" << lat_deg;
    }
  }
}

TEST(Phi2Test, Phi2HandlesPolesNegativeTsAndNan) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  const double wgs84_e = Wgs84Eccentricity();

  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

  EXPECT_DOUBLE_EQ(0.0, pj_phi2(ctx.get(), 1.0, wgs84_e));
  EXPECT_DOUBLE_EQ(0.0, pj_phi2(ctx.get(), -1.0, wgs84_e));
  EXPECT_NEAR(kHalfPi, pj_phi2(ctx.get(), 0.0, wgs84_e), 1e-15);
  EXPECT_NEAR(-kHalfPi, pj_phi2(ctx.get(), kInf, wgs84_e), 1e-15);
  EXPECT_TRUE(std::isnan(pj_phi2(ctx.get(), kNan, wgs84_e)));
  EXPECT_EQ(0, proj_context_errno(ctx.get()));
}

TEST(Phi2Test, LccAndMercInverseRoundTripViaPhi2) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // LCC inverse calls pj_phi2(); Mercator inverse calls pj_sinhpsi2tanphi().
  for (const char* def : {"+proj=lcc +lat_1=33 +lat_2=45 +ellps=WGS84",
                          "+proj=merc +ellps=WGS84"}) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;

    const PJ_COORD lp = proj_coord(DegToRad(-95.0), DegToRad(39.0), 0.0, 0.0);
    const PJ_COORD xy = proj_trans(pj.get(), PJ_FWD, lp);
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, xy);
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    EXPECT_NEAR(lp.lp.lam, inv.lp.lam, 1e-12) << def;
    EXPECT_NEAR(lp.lp.phi, inv.lp.phi, 1e-12) << def;
  }
}

}  // namespace
}  // namespace proj
