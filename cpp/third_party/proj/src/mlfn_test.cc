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
// Unit tests for ellipsoidal meridional-distance setup, forward, and inverse
// functions pj_enfn(), pj_mlfn(), and pj_inv_mlfn() (proj/src/mlfn.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/mlfn.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/geodesic.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;
constexpr double kWgs84A = 6378137.0;
constexpr double kWgs84InvF = 298.257223563;
constexpr double kWgs84F = 1.0 / kWgs84InvF;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

// Converts first flattening f to third flattening n = f / (2 - f).
constexpr double ThirdFlatteningFromF(double f) { return f / (2.0 - f); }

TEST(MlfnTest, SphericalThirdFlatteningYieldsIdentityConversion) {
  ScopedMalloc<double> en(pj_enfn(0.0));
  ASSERT_NE(nullptr, en.get());

  // 1. Rectifying radius multiplier en[0] == 1 and all Fourier coeffs == 0.
  EXPECT_DOUBLE_EQ(1.0, en.get()[0]);
  constexpr int kNumCoeffs = 2 * static_cast<int>(AuxLat::ORDER);
  for (int i = 1; i <= kNumCoeffs; ++i) {
    EXPECT_DOUBLE_EQ(0.0, en.get()[i]) << "i=" << i;
  }

  // 2. Forward and inverse reduce identically to phi in radians.
  for (const double lat_deg : {-90.0, -45.0, -0.0, 0.0, 30.0, 60.0, 90.0}) {
    const double phi = DegToRad(lat_deg);
    const double m = pj_mlfn(phi, std::sin(phi), std::cos(phi), en.get());
    EXPECT_DOUBLE_EQ(phi, m) << "lat_deg=" << lat_deg;
    EXPECT_DOUBLE_EQ(phi, pj_inv_mlfn(m, en.get())) << "lat_deg=" << lat_deg;
  }
}

TEST(MlfnTest, Wgs84MatchesGeodesicMeridianDistanceAndRoundTrips) {
  const double n = ThirdFlatteningFromF(kWgs84F);
  ScopedMalloc<double> en(pj_enfn(n));
  ASSERT_NE(nullptr, en.get());

  struct geod_geodesic geod{};
  geod_init(&geod, kWgs84A, kWgs84F);

  constexpr double kLatDegs[] = {-90.0, -75.0, -60.0, -45.0, -30.0,
                                 -1e-6, 0.0,   1e-6,  15.0,  30.0,
                                 45.0,  60.0,  75.0,  89.5,  90.0};

  for (const double lat_deg : kLatDegs) {
    const double phi = DegToRad(lat_deg);
    const double sphi = std::sin(phi);
    const double cphi = std::cos(phi);

    // 1. Forward dimensionless meridional distance m = s / a.
    const double m = pj_mlfn(phi, sphi, cphi, en.get());

    // Compare a * m against geodesic arc length from equator along meridian 0.
    double s12 = 0.0;
    geod_inverse(&geod, 0.0, 0.0, std::abs(lat_deg), 0.0, &s12, nullptr,
                 nullptr);
    const double signed_s12 = lat_deg < 0.0 ? -s12 : s12;
    EXPECT_NEAR(signed_s12, kWgs84A * m, 1e-6) << "lat_deg=" << lat_deg;

    // 2. Inverse recovers geographic latitude phi to full double precision.
    const double phi_inv = pj_inv_mlfn(m, en.get());
    EXPECT_NEAR(phi, phi_inv, 1e-13) << "lat_deg=" << lat_deg;
  }

  // Verify quarter meridian at North Pole (~10,001,965.729 m on WGS84).
  const double quarter_meridian =
      kWgs84A * pj_mlfn(kHalfPi, 1.0, 0.0, en.get());
  EXPECT_NEAR(10001965.7293, quarter_meridian, 1e-3);
}

TEST(MlfnTest, AccuracyAtFlatteningLimitProlateEllipsoidAndOddSymmetry) {
  // Exercise |f| = 1/150 (documented double-precision boundary) and a prolate
  // ellipsoid (f < 0 => n < 0).
  for (const double f : {1.0 / 150.0, -1.0 / 200.0}) {
    const double n = ThirdFlatteningFromF(f);
    ScopedMalloc<double> en(pj_enfn(n));
    ASSERT_NE(nullptr, en.get()) << "f=" << f;

    if (f > 0.0) {
      // Oblate ellipsoid has b < a, so rectifying radius < a (en[0] < 1).
      EXPECT_LT(en.get()[0], 1.0);
    } else {
      // Prolate ellipsoid has b > a, so rectifying radius > a (en[0] > 1).
      EXPECT_GT(en.get()[0], 1.0);
    }

    struct geod_geodesic geod{};
    geod_init(&geod, kWgs84A, f);

    for (const double lat_deg : {5.0, 27.5, 45.0, 68.0, 89.9}) {
      const double phi = DegToRad(lat_deg);
      const double sphi = std::sin(phi);
      const double cphi = std::cos(phi);

      const double m_pos = pj_mlfn(phi, sphi, cphi, en.get());
      const double m_neg = pj_mlfn(-phi, -sphi, cphi, en.get());
      EXPECT_NEAR(-m_pos, m_neg, 1e-15) << "f=" << f << " lat=" << lat_deg;

      double s12 = 0.0;
      geod_inverse(&geod, 0.0, 0.0, lat_deg, 0.0, &s12, nullptr, nullptr);
      EXPECT_NEAR(s12, kWgs84A * m_pos, 1e-5)
          << "f=" << f << " lat=" << lat_deg;

      const double inv_pos = pj_inv_mlfn(m_pos, en.get());
      const double inv_neg = pj_inv_mlfn(m_neg, en.get());
      EXPECT_NEAR(phi, inv_pos, 1e-13) << "f=" << f << " lat=" << lat_deg;
      EXPECT_NEAR(-inv_pos, inv_neg, 1e-15) << "f=" << f << " lat=" << lat_deg;
    }
  }
}

TEST(MlfnTest, CassiniEllipsoidalProjectionRoundTripsViaMlfn) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj cass(
      proj_create(ctx.get(), "+proj=cass +ellps=WGS84 +lon_0=10 +lat_0=50"));
  ASSERT_NE(nullptr, cass.get());

  // 1. Forward Cassini evaluates meridian arc distance via pj_mlfn().
  const PJ_COORD in_lp = proj_coord(DegToRad(11.5), DegToRad(52.3), 0.0, 0.0);
  const PJ_COORD xy = proj_trans(cass.get(), PJ_FWD, in_lp);
  EXPECT_EQ(0, proj_errno(cass.get()));
  EXPECT_FALSE(std::isinf(xy.xy.x));

  // 2. Inverse Cassini starts from footpoint latitude via pj_inv_mlfn().
  const PJ_COORD roundtrip = proj_trans(cass.get(), PJ_INV, xy);
  EXPECT_EQ(0, proj_errno(cass.get()));
  EXPECT_NEAR(in_lp.lp.lam, roundtrip.lp.lam, 1e-11);
  EXPECT_NEAR(in_lp.lp.phi, roundtrip.lp.phi, 1e-11);
}

// TODO(schwehr): pj_enfn() line 17 OOM guard (`if (nullptr == en) return
// nullptr;`) is left uncovered in unit tests without custom malloc failure
// injection.

}  // namespace
}  // namespace proj
