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
// Unit tests for Snyder's conformal small-t function pj_tsfn()
// (proj/src/tsfn.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/tsfn.cpp

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
constexpr double kQuarterPi = kPi / 4.0;
constexpr double kWgs84InvF = 298.257223563;
constexpr double kWgs84F = 1.0 / kWgs84InvF;
constexpr double kWgs84Es = kWgs84F * (2.0 - kWgs84F);

double Wgs84Eccentricity() { return std::sqrt(kWgs84Es); }

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

// Direct Snyder (1987) eq. (15-9) oracle for |phi| < pi/2.
double SnyderTsfnOracle(double phi, double sinphi, double e) {
  const double base = (1.0 - e * sinphi) / (1.0 + e * sinphi);
  return std::tan(kQuarterPi - 0.5 * phi) / std::pow(base, 0.5 * e);
}

TEST(TsfnTest, EquatorZeroPoleAndBothSinphiSignBranches) {
  const double wgs84_e = Wgs84Eccentricity();

  for (const double e : {0.0, wgs84_e, 0.5}) {
    // 1. Equator (sinphi = 0 and -0) exercises the sinphi <= 0 branch -> 1.0.
    EXPECT_DOUBLE_EQ(1.0, pj_tsfn(0.0, 0.0, e)) << "e=" << e;
    EXPECT_DOUBLE_EQ(1.0, pj_tsfn(-0.0, -0.0, e)) << "e=" << e;

    // 2. North Pole (sinphi = 1 > 0) uses cosphi / (1 + sinphi), avoiding
    // division by zero and returning ~0 cleanly.
    const double north_pole = pj_tsfn(kHalfPi, 1.0, e);
    EXPECT_TRUE(std::isfinite(north_pole)) << "e=" << e;
    EXPECT_GE(north_pole, 0.0) << "e=" << e;
    EXPECT_NEAR(0.0, north_pole, 1e-15) << "e=" << e;
  }

  // 3. Negative latitudes (sinphi <= 0) and positive latitudes (sinphi > 0)
  // match Snyder's formula across mid- and high-latitude ranges.
  constexpr double kLatDegs[] = {-85.0, -60.0, -30.0, -1e-4,
                                 1e-4,  30.0,  60.0,  85.0};
  for (const double e : {0.0, wgs84_e, 0.5}) {
    for (const double lat_deg : kLatDegs) {
      const double phi = DegToRad(lat_deg);
      const double sphi = std::sin(phi);
      const double actual = pj_tsfn(phi, sphi, e);
      const double expected = SnyderTsfnOracle(phi, sphi, e);
      EXPECT_NEAR(expected, actual, 1e-13 * std::max(1.0, expected))
          << "e=" << e << " lat=" << lat_deg;
    }
  }
}

TEST(TsfnTest, SphereHalfAngleTangentReciprocalSymmetryAndMonotonicity) {
  const double wgs84_e = Wgs84Eccentricity();

  // 1. Spherical eccentricity (e = 0) reduces identically to tan(pi/4 - phi/2).
  constexpr double kLatDegs[] = {-80.0, -45.0, -15.0, 0.0, 15.0, 45.0, 80.0};
  double prev_ts = std::numeric_limits<double>::infinity();
  for (const double lat_deg : kLatDegs) {
    const double phi = DegToRad(lat_deg);
    const double ts_sphere = pj_tsfn(phi, std::sin(phi), 0.0);
    const double expected = std::tan(kQuarterPi - 0.5 * phi);
    EXPECT_NEAR(expected, ts_sphere, 1e-13 * std::max(1.0, expected))
        << "lat=" << lat_deg;
    // ts(phi) = exp(-psi(phi)) decreases strictly as phi increases.
    EXPECT_LT(ts_sphere, prev_ts) << "lat=" << lat_deg;
    prev_ts = ts_sphere;
  }

  // 2. Antisymmetry of isometric latitude psi(-phi) = -psi(phi) gives
  // ts(-phi) * ts(phi) == 1.
  for (const double e : {0.0, wgs84_e, 0.5}) {
    for (const double lat_deg : {5.0, 35.0, 65.0, 89.5}) {
      const double phi = DegToRad(lat_deg);
      const double sphi = std::sin(phi);
      const double ts_pos = pj_tsfn(phi, sphi, e);
      const double ts_neg = pj_tsfn(-phi, -sphi, e);
      EXPECT_NEAR(1.0, ts_pos * ts_neg, 1e-13)
          << "e=" << e << " lat=" << lat_deg;
    }
  }
}

TEST(TsfnTest, RoundTripsWithPhi2AcrossEccentricities) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  const double wgs84_e = Wgs84Eccentricity();

  constexpr double kLatDegs[] = {-89.5, -60.0, -30.0, -1.0, 0.0,
                                 1.0,   30.0,  60.0,  89.5};
  for (const double e : {0.0, wgs84_e, 0.5}) {
    for (const double lat_deg : kLatDegs) {
      const double phi = DegToRad(lat_deg);
      const double ts = pj_tsfn(phi, std::sin(phi), e);
      const double phi_back = pj_phi2(ctx.get(), ts, e);
      EXPECT_EQ(0, proj_context_errno(ctx.get()));
      EXPECT_NEAR(phi, phi_back, 1e-12) << "e=" << e << " lat=" << lat_deg;
    }
  }
}

TEST(TsfnTest, LccAndStereographicProjectionsRoundTripNorthernAndSouthern) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // LCC and Polar Stereographic forward transforms use pj_tsfn() across
  // Northern (sinphi > 0) and Southern (sinphi <= 0) latitudes.
  constexpr const char* kDefs[] = {
      "+proj=lcc +lat_1=33 +lat_2=45 +lon_0=-95 +ellps=WGS84",
      "+proj=stere +lat_0=90 +lon_0=0 +ellps=WGS84",
  };

  for (const char* def : kDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;

    for (const double lat_deg : {-20.0, 0.0, 39.0, 65.0}) {
      const PJ_COORD in_lp =
          proj_coord(DegToRad(-90.0), DegToRad(lat_deg), 0.0, 0.0);
      const PJ_COORD xy = proj_trans(pj.get(), PJ_FWD, in_lp);
      EXPECT_EQ(0, proj_errno(pj.get())) << def << " lat=" << lat_deg;
      const PJ_COORD roundtrip = proj_trans(pj.get(), PJ_INV, xy);
      EXPECT_EQ(0, proj_errno(pj.get())) << def << " lat=" << lat_deg;
      EXPECT_NEAR(in_lp.lp.lam, roundtrip.lp.lam, 1e-12)
          << def << " lat=" << lat_deg;
      EXPECT_NEAR(in_lp.lp.phi, roundtrip.lp.phi, 1e-12)
          << def << " lat=" << lat_deg;
    }
  }
}

}  // namespace
}  // namespace proj
