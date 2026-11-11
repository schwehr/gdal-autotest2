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
// Unit tests for Snyder's small m parallel-radius helper pj_msfn()
// (proj/src/msfn.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/msfn.cpp

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
constexpr double kWgs84InvF = 298.257223563;
constexpr double kWgs84F = 1.0 / kWgs84InvF;
constexpr double kWgs84Es = kWgs84F * (2.0 - kWgs84F);

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(MsfnTest, EquatorAndPolesAcrossEccentricities) {
  constexpr double kEccentricitiesSq[] = {0.0, kWgs84Es, 0.08, -0.01, 0.99};
  for (const double es : kEccentricitiesSq) {
    // At the equator (sinphi = 0, cosphi = 1), m = 1 / sqrt(1) = 1.
    EXPECT_DOUBLE_EQ(1.0, pj_msfn(0.0, 1.0, es)) << "es=" << es;

    // At either pole (sinphi = +-1, cosphi = 0) with es < 1, m = 0.
    EXPECT_DOUBLE_EQ(0.0, pj_msfn(1.0, 0.0, es)) << "es=" << es;
    EXPECT_DOUBLE_EQ(0.0, pj_msfn(-1.0, 0.0, es)) << "es=" << es;
  }

  // Negative zero cosphi preserves sign bit when numerator divides by > 0.
  EXPECT_TRUE(std::signbit(pj_msfn(1.0, -0.0, kWgs84Es)));
}

TEST(MsfnTest, SphereMatchesCosPhiAndEllipsoidMatchesSnyderFormula) {
  constexpr double kLatitudesDeg[] = {-75.0, -45.0, -30.0, 0.0, 15.0,
                                      30.0,  45.0,  60.0,  75.0};

  for (const double lat_deg : kLatitudesDeg) {
    const double phi = DegToRad(lat_deg);
    const double sphi = std::sin(phi);
    const double cphi = std::cos(phi);

    // 1. Spherical branch (es = 0) reduces identically to cos(phi).
    const double m_sphere = pj_msfn(sphi, cphi, 0.0);
    EXPECT_NEAR(cphi, m_sphere, 1e-15) << "lat_deg=" << lat_deg;

    // 2. Ellipsoidal branch matches Snyder (1987) eq. 14-15:
    //    m(phi) = cos(phi) / sqrt(1 - e^2 * sin^2(phi)).
    const double m_ellps = pj_msfn(sphi, cphi, kWgs84Es);
    const double expected_ellps =
        cphi / std::sqrt(1.0 - kWgs84Es * sphi * sphi);
    EXPECT_NEAR(expected_ellps, m_ellps, 1e-15) << "lat_deg=" << lat_deg;

    // For an oblate ellipsoid (es > 0) away from the equator/poles,
    // m_ellps > cos(phi).
    if (lat_deg != 0.0) {
      EXPECT_GT(m_ellps, m_sphere) << "lat_deg=" << lat_deg;
    }
  }
}

TEST(MsfnTest, ExactRationalOblateAndProlateCases) {
  struct RationalCase {
    double sinphi;
    double cosphi;
    double es;
    double expected;
  };
  constexpr RationalCase kCases[] = {
      // Sphere: sin=0.6, cos=0.8, es=0 -> 0.8.
      {.sinphi = 0.6, .cosphi = 0.8, .es = 0.0, .expected = 0.8},
      // Degenerate es=1 away from poles: sqrt(1 - 0.36) = 0.8 -> 0.8/0.8 = 1.0.
      {.sinphi = 0.6, .cosphi = 0.8, .es = 1.0, .expected = 1.0},
      // Oblate: sin=0.8, cos=0.6, es=9/16 -> sqrt(1 - 0.36) = 0.8 -> 0.75.
      {.sinphi = 0.8, .cosphi = 0.6, .es = 0.5625, .expected = 0.75},
      // Prolate (es < 0): sin=0.6, cos=0.8, es=-25/16 ->
      // sqrt(25/16) = 1.25 -> 0.8 / 1.25 = 0.64.
      {.sinphi = 0.6, .cosphi = 0.8, .es = -1.5625, .expected = 0.64},
  };

  for (const auto& tc : kCases) {
    EXPECT_NEAR(tc.expected, pj_msfn(tc.sinphi, tc.cosphi, tc.es), 1e-15);
  }
}

TEST(MsfnTest, SymmetryInLatitudeAndCosineSign) {
  const double phi = DegToRad(37.5);
  const double sphi = std::sin(phi);
  const double cphi = std::cos(phi);

  EXPECT_DOUBLE_EQ(pj_msfn(sphi, cphi, kWgs84Es),
                   pj_msfn(-sphi, cphi, kWgs84Es));
  EXPECT_DOUBLE_EQ(-pj_msfn(sphi, cphi, kWgs84Es),
                   pj_msfn(sphi, -cphi, kWgs84Es));
}

TEST(MsfnTest, SingularitiesAndNonFiniteInputs) {
  constexpr double kQnan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();

  // Zero denominator: 1 - es * sinphi^2 == 0 with non-zero cosphi -> +-Inf.
  const double pos_inf = pj_msfn(1.0, 1.0, 1.0);
  EXPECT_TRUE(std::isinf(pos_inf));
  EXPECT_GT(pos_inf, 0.0);

  const double neg_inf = pj_msfn(1.0, -1.0, 1.0);
  EXPECT_TRUE(std::isinf(neg_inf));
  EXPECT_LT(neg_inf, 0.0);

  // Indeterminate 0 / 0 at pole when es == 1 -> NaN.
  EXPECT_TRUE(std::isnan(pj_msfn(1.0, 0.0, 1.0)));

  // Negative radicand: 1 - 2.0 * 1.0^2 = -1.0 -> sqrt(-1) = NaN.
  EXPECT_TRUE(std::isnan(pj_msfn(1.0, 0.5, 2.0)));

  // NaN propagation across every argument.
  EXPECT_TRUE(std::isnan(pj_msfn(kQnan, 0.5, kWgs84Es)));
  EXPECT_TRUE(std::isnan(pj_msfn(0.5, kQnan, kWgs84Es)));
  EXPECT_TRUE(std::isnan(pj_msfn(0.5, 0.5, kQnan)));

  // Infinite cosphi with valid denominator scales to +Inf.
  const double inf_cos = pj_msfn(0.0, kInf, kWgs84Es);
  EXPECT_TRUE(std::isinf(inf_cos));
  EXPECT_GT(inf_cos, 0.0);
}

TEST(MsfnTest, ConsistentWithMercatorScaleAtStandardParallel) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  const double lat_ts = DegToRad(45.0);
  const double sin_ts = std::sin(lat_ts);
  const double cos_ts = std::cos(lat_ts);

  // 1. Ellipsoidal Mercator with +lat_ts=45 sets
  //    P->k0 = pj_msfn(sin_ts, cos_ts, es), making parallel scale equal to 1 at
  //    lat_ts and k0 at the equator.
  ScopedPj merc_ellps(
      proj_create(ctx.get(), "+proj=merc +lat_ts=45 +ellps=WGS84"));
  ASSERT_NE(nullptr, merc_ellps.get());

  const double expected_k0 = pj_msfn(sin_ts, cos_ts, merc_ellps->es);
  EXPECT_NEAR(expected_k0, merc_ellps->k0, 1e-15);

  const PJ_FACTORS factors_at_ts =
      proj_factors(merc_ellps.get(), proj_coord(0.0, lat_ts, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc_ellps.get()));
  EXPECT_NEAR(1.0, factors_at_ts.parallel_scale, 1e-12);

  const PJ_FACTORS factors_at_eq =
      proj_factors(merc_ellps.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc_ellps.get()));
  EXPECT_NEAR(expected_k0, factors_at_eq.parallel_scale, 1e-12);

  // 2. Spherical Mercator with +lat_ts=45 sets
  //    P->k0 = cos(lat_ts) = pj_msfn(..., 0).
  ScopedPj merc_sphere(
      proj_create(ctx.get(), "+proj=merc +lat_ts=45 +R=6378137"));
  ASSERT_NE(nullptr, merc_sphere.get());
  EXPECT_DOUBLE_EQ(0.0, merc_sphere->es);
  EXPECT_NEAR(pj_msfn(sin_ts, cos_ts, 0.0), merc_sphere->k0, 1e-15);

  const PJ_FACTORS sphere_eq_factors =
      proj_factors(merc_sphere.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc_sphere.get()));
  EXPECT_NEAR(cos_ts, sphere_eq_factors.parallel_scale, 1e-12);
}

}  // namespace
}  // namespace proj
