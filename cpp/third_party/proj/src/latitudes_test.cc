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
// Unit tests for PROJ auxiliary latitude helpers: conformal latitude, authalic
// latitude (small-n series and large-n Newton paths), Karney auxiliary latitude
// series coefficients/Clenshaw evaluation, and rectifying radius
// (proj/src/latitudes.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/latitudes.cpp

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

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
constexpr double kWgs84N = kWgs84F / (2.0 - kWgs84F);

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(LatitudesTest, ConformalLatitudeSphereAndEllipsoidRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Spherical branch (P->e == 0.0) returns the input latitude unchanged.
  ScopedPj sphere(proj_create(ctx.get(), "+proj=merc +R=6378137"));
  ASSERT_NE(nullptr, sphere.get());
  ASSERT_DOUBLE_EQ(0.0, sphere->e);

  EXPECT_DOUBLE_EQ(0.0, pj_conformal_lat(0.0, sphere.get()));
  EXPECT_TRUE(std::signbit(pj_conformal_lat(-0.0, sphere.get())));
  EXPECT_DOUBLE_EQ(DegToRad(37.5),
                   pj_conformal_lat(DegToRad(37.5), sphere.get()));
  EXPECT_DOUBLE_EQ(DegToRad(-62.25),
                   pj_conformal_lat_inverse(DegToRad(-62.25), sphere.get()));

  // 2. Ellipsoidal branch matches Snyder (1987) eq. (3-1), Karney auxlat
  //    series, and round-trips via pj_conformal_lat_inverse().
  ScopedPj wgs84(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, wgs84.get());
  ASSERT_GT(wgs84->e, 0.0);

  constexpr int kOrder = static_cast<int>(AuxLat::ORDER);
  std::array<double, kOrder> phi_to_chi{};
  pj_auxlat_coeffs(wgs84->n, AuxLat::GEOGRAPHIC, AuxLat::CONFORMAL,
                   phi_to_chi.data());

  constexpr double kLatitudesDeg[] = {
      -89.9, -75.0, -45.0, -15.0, 0.0, 15.0, 45.0, 75.0, 89.9,
  };
  for (const double lat_deg : kLatitudesDeg) {
    const double phi = DegToRad(lat_deg);
    const double chi = pj_conformal_lat(phi, wgs84.get());
    const double snyder_chi =
        2.0 * std::atan(std::tan(kPi / 4.0 + phi / 2.0) *
                        std::pow((1.0 - wgs84->e * std::sin(phi)) /
                                     (1.0 + wgs84->e * std::sin(phi)),
                                 wgs84->e / 2.0)) -
        kHalfPi;
    EXPECT_NEAR(snyder_chi, chi, 1e-14) << "lat_deg=" << lat_deg;
    EXPECT_NEAR(pj_auxlat_convert(phi, phi_to_chi.data()), chi, 1e-14)
        << "lat_deg=" << lat_deg;

    // Odd symmetry and equator/interior magnitude ordering.
    EXPECT_NEAR(-chi, pj_conformal_lat(-phi, wgs84.get()), 1e-15);
    if (lat_deg != 0.0) {
      EXPECT_LT(std::abs(chi), std::abs(phi)) << "lat_deg=" << lat_deg;
    } else {
      EXPECT_DOUBLE_EQ(0.0, chi);
    }

    const double phi_back = pj_conformal_lat_inverse(chi, wgs84.get());
    EXPECT_EQ(0, proj_context_errno(ctx.get()));
    EXPECT_NEAR(phi, phi_back, 1e-13) << "lat_deg=" << lat_deg;
  }

  // Exact poles map to +-pi/2 and round-trip cleanly.
  EXPECT_NEAR(kHalfPi, pj_conformal_lat(kHalfPi, wgs84.get()), 1e-12);
  EXPECT_NEAR(-kHalfPi, pj_conformal_lat(-kHalfPi, wgs84.get()), 1e-12);
  EXPECT_NEAR(kHalfPi, pj_conformal_lat_inverse(kHalfPi, wgs84.get()), 1e-12);
  EXPECT_NEAR(-kHalfPi, pj_conformal_lat_inverse(-kHalfPi, wgs84.get()), 1e-12);
}

TEST(LatitudesTest, AuthalicLatQHandlesSphereEllipsoidAndSingularity) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Sphere (e == 0) and near-sphere (e = 1e-8 < 1e-7 threshold) return
  // 2*sin.
  ScopedPj sphere(proj_create(ctx.get(), "+proj=merc +R=6378137"));
  ScopedPj near_sphere(
      proj_create(ctx.get(), "+proj=merc +a=6378137 +es=1e-16"));
  ASSERT_NE(nullptr, sphere.get());
  ASSERT_NE(nullptr, near_sphere.get());
  ASSERT_LT(near_sphere->e, 1e-7);

  constexpr double kSinValues[] = {-1.0, -0.5, 0.0, 0.25, 0.5, 1.0};
  for (const double s : kSinValues) {
    EXPECT_DOUBLE_EQ(2.0 * s, pj_authalic_lat_q(s, sphere.get()));
    EXPECT_DOUBLE_EQ(2.0 * s, pj_authalic_lat_q(s, near_sphere.get()));
  }

  // 2. Ellipsoid (e >= 1e-7) matches Snyder (1987) eq. (3-12).
  ScopedPj wgs84(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, wgs84.get());
  for (const double s : kSinValues) {
    const double q = pj_authalic_lat_q(s, wgs84.get());
    const double e_s = wgs84->e * s;
    const double expected_q =
        wgs84->one_es *
        (s / (1.0 - e_s * e_s) +
         (0.5 / wgs84->e) * std::log((1.0 + e_s) / (1.0 - e_s)));
    EXPECT_NEAR(expected_q, q, 1e-14) << "s=" << s;
    EXPECT_NEAR(-q, pj_authalic_lat_q(-s, wgs84.get()), 1e-15);
  }
  const double qp = pj_authalic_lat_q(1.0, wgs84.get());
  EXPECT_GT(qp, 1.9);
  EXPECT_LT(qp, 2.0);

  // 3. Zero-division guard returns HUGE_VAL when 1 - (e * sinphi)^2 == 0.
  ScopedPj singular(pj_new());
  ASSERT_NE(nullptr, singular.get());
  singular->ctx = ctx.get();
  singular->e = 0.5;
  singular->es = 0.25;
  singular->one_es = 0.75;
  EXPECT_EQ(HUGE_VAL, pj_authalic_lat_q(2.0, singular.get()));
  EXPECT_EQ(HUGE_VAL, pj_authalic_lat_q(-2.0, singular.get()));
}

TEST(LatitudesTest, AuthalicLatSeriesAndLargeFlatteningNewtonBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Small flattening (|n| < 0.01): WGS84 uses the 2*ORDER series array.
  ScopedPj wgs84(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, wgs84.get());
  ASSERT_LT(std::abs(wgs84->n), 0.01);

  ScopedMalloc<double> apa_wgs84(pj_authalic_lat_compute_coeffs(wgs84->n));
  ASSERT_NE(nullptr, apa_wgs84.get());
  const double qp_wgs84 = pj_authalic_lat_q(1.0, wgs84.get());

  constexpr double kLatitudesDeg[] = {
      -89.9, -60.0, -30.0, 0.0, 30.0, 60.0, 89.9,
  };
  for (const double lat_deg : kLatitudesDeg) {
    const double phi = DegToRad(lat_deg);
    const double beta = pj_authalic_lat(phi, std::sin(phi), std::cos(phi),
                                        apa_wgs84.get(), wgs84.get(), qp_wgs84);
    const double analytic_beta =
        std::asin(pj_authalic_lat_q(std::sin(phi), wgs84.get()) / qp_wgs84);
    EXPECT_NEAR(analytic_beta, beta, 1e-13) << "lat_deg=" << lat_deg;
    EXPECT_NEAR(
        phi,
        pj_authalic_lat_inverse(beta, apa_wgs84.get(), wgs84.get(), qp_wgs84),
        1e-13)
        << "lat_deg=" << lat_deg;
  }

  // Exact poles round-trip cleanly via series path.
  EXPECT_NEAR(kHalfPi,
              pj_authalic_lat_inverse(
                  pj_authalic_lat(kHalfPi, 1.0, 0.0, apa_wgs84.get(),
                                  wgs84.get(), qp_wgs84),
                  apa_wgs84.get(), wgs84.get(), qp_wgs84),
              1e-12);

  // Sphere (n == 0) returns identical latitudes.
  ScopedPj sphere(proj_create(ctx.get(), "+proj=merc +R=6378137"));
  ASSERT_NE(nullptr, sphere.get());
  ScopedMalloc<double> apa_sphere(pj_authalic_lat_compute_coeffs(sphere->n));
  ASSERT_NE(nullptr, apa_sphere.get());
  const double phi45 = DegToRad(45.0);
  EXPECT_DOUBLE_EQ(phi45,
                   pj_authalic_lat(phi45, std::sin(phi45), std::cos(phi45),
                                   apa_sphere.get(), sphere.get(), 2.0));
  EXPECT_DOUBLE_EQ(phi45, pj_authalic_lat_inverse(phi45, apa_sphere.get(),
                                                  sphere.get(), 2.0));

  // 2. Exact boundary |n| == 0.01 and negative |n| >= 0.01 take the
  // single-block
  //    large-n branch, while negative |n| < 0.01 takes the 2*ORDER branch.
  ScopedMalloc<double> apa_boundary(pj_authalic_lat_compute_coeffs(0.01));
  ASSERT_NE(nullptr, apa_boundary.get());
  ScopedMalloc<double> apa_prolate_large(pj_authalic_lat_compute_coeffs(-0.02));
  ASSERT_NE(nullptr, apa_prolate_large.get());
  ScopedMalloc<double> apa_prolate_small(
      pj_authalic_lat_compute_coeffs(-0.005));
  ASSERT_NE(nullptr, apa_prolate_small.get());

  // 3. Large flattening (|n| >= 0.01): es = 0.5 gives n ~= 0.1716, exercising
  //    exact q/qp forward and Newton iteration in pj_authalic_lat_inverse().
  ScopedPj high_flat(proj_create(ctx.get(), "+proj=merc +a=1 +es=0.5"));
  ASSERT_NE(nullptr, high_flat.get());
  ASSERT_GE(std::abs(high_flat->n), 0.01);

  ScopedMalloc<double> apa_flat(pj_authalic_lat_compute_coeffs(high_flat->n));
  ASSERT_NE(nullptr, apa_flat.get());
  const double qp_flat = pj_authalic_lat_q(1.0, high_flat.get());

  constexpr double kHighFlatDeg[] = {-80.0, -45.0, -15.0, 0.0,
                                     15.0,  45.0,  80.0};
  for (const double lat_deg : kHighFlatDeg) {
    const double phi = DegToRad(lat_deg);
    const double beta =
        pj_authalic_lat(phi, std::sin(phi), std::cos(phi), apa_flat.get(),
                        high_flat.get(), qp_flat);
    EXPECT_NEAR(
        phi,
        pj_authalic_lat_inverse(beta, apa_flat.get(), high_flat.get(), qp_flat),
        1e-12)
        << "lat_deg=" << lat_deg;
  }

  // Ratio > 1 and < -1 clamping branches when rounding causes |q/qp| > 1.
  const double slightly_small_qp = qp_flat * (1.0 - 1e-15);
  EXPECT_NEAR(kHalfPi,
              pj_authalic_lat(kHalfPi, 1.0, 0.0, apa_flat.get(),
                              high_flat.get(), slightly_small_qp),
              1e-15);
  EXPECT_NEAR(-kHalfPi,
              pj_authalic_lat(-kHalfPi, -1.0, 0.0, apa_flat.get(),
                              high_flat.get(), slightly_small_qp),
              1e-15);
}

TEST(LatitudesTest, EqualAreaProjectionsUseConsistentAuthalicQuantities) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj cea(proj_create(ctx.get(), "+proj=cea +ellps=WGS84"));
  ASSERT_NE(nullptr, cea.get());

  const PJ_COORD pt = proj_coord(DegToRad(18.5), DegToRad(-33.8), 0.0, 0.0);
  const PJ_COORD fwd = proj_trans(cea.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(cea.get()));
  EXPECT_NEAR(0.5 * kWgs84A * pj_authalic_lat_q(std::sin(pt.lp.phi), cea.get()),
              fwd.xy.y, 1e-7);

  const PJ_COORD inv = proj_trans(cea.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(cea.get()));
  EXPECT_NEAR(pt.lp.lam, inv.lp.lam, 1e-12);
  EXPECT_NEAR(pt.lp.phi, inv.lp.phi, 1e-12);

  // Equal-area projection satisfies areal scale factor s = h * k = 1.
  const PJ_FACTORS factors = proj_factors(cea.get(), pt);
  EXPECT_EQ(0, proj_errno(cea.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-10);
}

TEST(LatitudesTest, AuxlatCoeffsSupportsAllFourPairsAndThrowsOnInvalid) {
  constexpr int kOrder = static_cast<int>(AuxLat::ORDER);
  std::array<double, kOrder> F{};

  // 1. Out-of-range enum indices throw std::out_of_range.
  EXPECT_THROW(pj_auxlat_coeffs(kWgs84N, static_cast<AuxLat>(-1),
                                AuxLat::GEOGRAPHIC, F.data()),
               std::out_of_range);
  EXPECT_THROW(
      pj_auxlat_coeffs(kWgs84N, AuxLat::NUMBER, AuxLat::GEOGRAPHIC, F.data()),
      std::out_of_range);
  EXPECT_THROW(pj_auxlat_coeffs(kWgs84N, AuxLat::GEOGRAPHIC,
                                static_cast<AuxLat>(-1), F.data()),
               std::out_of_range);
  EXPECT_THROW(
      pj_auxlat_coeffs(kWgs84N, AuxLat::GEOGRAPHIC, AuxLat::NUMBER, F.data()),
      std::out_of_range);

  // 2. Valid enum values for ungenerated matrices throw std::out_of_range.
  struct UnsupportedPair {
    AuxLat in;
    AuxLat out;
  };
  constexpr UnsupportedPair kUnsupported[] = {
      {.in = AuxLat::GEOGRAPHIC, .out = AuxLat::GEOGRAPHIC},
      {.in = AuxLat::PARAMETRIC, .out = AuxLat::GEOGRAPHIC},
      {.in = AuxLat::GEOGRAPHIC, .out = AuxLat::GEOCENTRIC},
      {.in = AuxLat::CONFORMAL, .out = AuxLat::AUTHALIC},
      {.in = AuxLat::AUTHALIC, .out = AuxLat::RECTIFYING},
  };
  for (const auto& pair : kUnsupported) {
    EXPECT_THROW(pj_auxlat_coeffs(kWgs84N, pair.in, pair.out, F.data()),
                 std::out_of_range);
  }

  // 3. Spherical n == 0 yields all-zero Fourier coefficients.
  pj_auxlat_coeffs(0.0, AuxLat::GEOGRAPHIC, AuxLat::RECTIFYING, F.data());
  for (const double c : F) {
    EXPECT_DOUBLE_EQ(0.0, c);
  }

  // 4. All 4 supported bidirectional pairs round-trip cleanly on WGS84,
  //    exercising both the even-power branch (<= RECTIFYING) and full branch.
  constexpr AuxLat kSupportedFromGeo[] = {
      AuxLat::RECTIFYING,
      AuxLat::CONFORMAL,
      AuxLat::AUTHALIC,
  };
  constexpr double kTestLatsDeg[] = {-75.0, -30.0, 0.0, 30.0, 75.0};

  for (const AuxLat target : kSupportedFromGeo) {
    std::array<double, kOrder> fwd{};
    std::array<double, kOrder> inv{};
    pj_auxlat_coeffs(kWgs84N, AuxLat::GEOGRAPHIC, target, fwd.data());
    pj_auxlat_coeffs(kWgs84N, target, AuxLat::GEOGRAPHIC, inv.data());
    for (const double lat_deg : kTestLatsDeg) {
      const double phi = DegToRad(lat_deg);
      const double aux = pj_auxlat_convert(phi, fwd.data());
      EXPECT_NEAR(phi, pj_auxlat_convert(aux, inv.data()), 1e-14);
    }
  }

  // CONFORMAL <-> RECTIFYING round-trip and transitive agreement with phi->mu.
  std::array<double, kOrder> phi_to_chi{};
  std::array<double, kOrder> phi_to_mu{};
  std::array<double, kOrder> chi_to_mu{};
  std::array<double, kOrder> mu_to_chi{};
  pj_auxlat_coeffs(kWgs84N, AuxLat::GEOGRAPHIC, AuxLat::CONFORMAL,
                   phi_to_chi.data());
  pj_auxlat_coeffs(kWgs84N, AuxLat::GEOGRAPHIC, AuxLat::RECTIFYING,
                   phi_to_mu.data());
  pj_auxlat_coeffs(kWgs84N, AuxLat::CONFORMAL, AuxLat::RECTIFYING,
                   chi_to_mu.data());
  pj_auxlat_coeffs(kWgs84N, AuxLat::RECTIFYING, AuxLat::CONFORMAL,
                   mu_to_chi.data());

  for (const double lat_deg : kTestLatsDeg) {
    const double phi = DegToRad(lat_deg);
    const double chi = pj_auxlat_convert(phi, phi_to_chi.data());
    const double mu_direct = pj_auxlat_convert(phi, phi_to_mu.data());
    const double mu_via_chi = pj_auxlat_convert(chi, chi_to_mu.data());
    EXPECT_NEAR(mu_direct, mu_via_chi, 1e-14);
    EXPECT_NEAR(chi, pj_auxlat_convert(mu_via_chi, mu_to_chi.data()), 1e-14);
  }
}

TEST(LatitudesTest, AuxlatConvertAllThreeOverloadsAndPolarPrecision) {
  constexpr int kOrder = static_cast<int>(AuxLat::ORDER);
  std::array<double, kOrder> F{};
  pj_auxlat_coeffs(kWgs84N, AuxLat::GEOGRAPHIC, AuxLat::CONFORMAL, F.data());

  const double phi = DegToRad(51.5);
  const double sphi = std::sin(phi);
  const double cphi = std::cos(phi);

  const double chi_angle = pj_auxlat_convert(phi, F.data());
  const double chi_trig = pj_auxlat_convert(phi, sphi, cphi, F.data());
  EXPECT_DOUBLE_EQ(chi_angle, chi_trig);

  double schi = 0.0;
  double cchi = 0.0;
  pj_auxlat_convert(sphi, cphi, schi, cchi, F.data());
  EXPECT_NEAR(1.0, schi * schi + cchi * cchi, 1e-15);
  EXPECT_NEAR(chi_angle, std::atan2(schi, cchi), 1e-14);

  // Near the north pole, the sine/cosine overload preserves small cos(chi)
  // with full relative precision instead of losing digits in pi/2 - phi.
  const double polar_cphi = 1e-10;
  const double polar_sphi = std::sqrt(1.0 - polar_cphi * polar_cphi);
  double polar_schi = 0.0;
  double polar_cchi = 0.0;
  pj_auxlat_convert(polar_sphi, polar_cphi, polar_schi, polar_cchi, F.data());
  EXPECT_GT(polar_cchi, 0.0);
  EXPECT_LT(polar_cchi, 2e-10);
  EXPECT_NEAR(1.0, polar_schi * polar_schi + polar_cchi * polar_cchi, 1e-15);
}

TEST(LatitudesTest, PolyvalClenshawAndRectifyingRadius) {
  // 1. pj_polyval handles N < 0, N == 0, and general Horner evaluation.
  EXPECT_DOUBLE_EQ(0.0, pj_polyval(3.0, nullptr, -1));
  constexpr std::array<double, 4> kPoly = {1.0, -2.0, 3.0, 4.0};
  EXPECT_DOUBLE_EQ(1.0, pj_polyval(5.0, kPoly.data(), 0));
  // 1 - 2*2 + 3*4 + 4*8 = 41.
  EXPECT_DOUBLE_EQ(41.0, pj_polyval(2.0, kPoly.data(), 3));

  // 2. pj_clenshaw returns 0 for K <= 0 and matches direct sine summation.
  constexpr std::array<double, 4> kFourier = {0.1, -0.02, 0.003, -0.0004};
  EXPECT_DOUBLE_EQ(0.0, pj_clenshaw(0.6, 0.8, kFourier.data(), 0));
  EXPECT_DOUBLE_EQ(0.0, pj_clenshaw(0.6, 0.8, kFourier.data(), -2));

  for (const double zeta : {0.0, kPi / 7.0, kPi / 4.0, kHalfPi}) {
    const double s = std::sin(zeta);
    const double c = std::cos(zeta);
    for (const int K : {1, 2, 4}) {
      double expected = 0.0;
      for (int k = 0; k < K; ++k) {
        expected += kFourier[k] * std::sin((2 * k + 2) * zeta);
      }
      EXPECT_NEAR(expected, pj_clenshaw(s, c, kFourier.data(), K), 1e-15)
          << "zeta=" << zeta << " K=" << K;
    }
  }

  // 3. pj_rectifying_radius handles sphere (n=0), prolate (n<0), oblate,
  //    and matches pj_enfn()[0] and the WGS84 quarter-meridian length.
  EXPECT_DOUBLE_EQ(1.0, pj_rectifying_radius(0.0));

  // At n = 0.5: (1 + 0.25/4 + 0.0625/64 + 0.015625/256) / 1.5.
  const double expected_half =
      (1.0 + 0.25 / 4.0 + 0.0625 / 64.0 + 0.015625 / 256.0) / 1.5;
  EXPECT_NEAR(expected_half, pj_rectifying_radius(0.5), 1e-15);

  const double expected_prolate =
      (1.0 + 0.01 / 4.0 + 0.0001 / 64.0 + 0.000001 / 256.0) / 0.9;
  EXPECT_NEAR(expected_prolate, pj_rectifying_radius(-0.1), 1e-15);

  ScopedMalloc<double> en(pj_enfn(kWgs84N));
  ASSERT_NE(nullptr, en.get());
  const double rect_wgs84 = pj_rectifying_radius(kWgs84N);
  EXPECT_DOUBLE_EQ(en.get()[0], rect_wgs84);

  // WGS84 quarter meridian = (pi/2) * a * rectifying_radius ~= 10001965.7293 m.
  const double quarter_meridian_m = kHalfPi * kWgs84A * rect_wgs84;
  EXPECT_NEAR(10001965.729312728, quarter_meridian_m, 1e-7);
}

TEST(LatitudesTest, NonFiniteInputsPropagateNanCleanly) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj wgs84(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ScopedPj sphere(proj_create(ctx.get(), "+proj=merc +R=6378137"));
  ASSERT_NE(nullptr, wgs84.get());
  ASSERT_NE(nullptr, sphere.get());

  constexpr double kQnan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();

  EXPECT_TRUE(std::isnan(pj_conformal_lat(kQnan, sphere.get())));
  EXPECT_TRUE(std::isnan(pj_conformal_lat(kQnan, wgs84.get())));
  EXPECT_TRUE(std::isnan(pj_conformal_lat_inverse(kQnan, sphere.get())));
  EXPECT_TRUE(std::isnan(pj_conformal_lat_inverse(kQnan, wgs84.get())));
  EXPECT_TRUE(std::isnan(pj_conformal_lat_inverse(kInf, wgs84.get())));
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  EXPECT_TRUE(std::isnan(pj_authalic_lat_q(kQnan, sphere.get())));
  EXPECT_TRUE(std::isnan(pj_authalic_lat_q(kQnan, wgs84.get())));
  EXPECT_TRUE(std::isnan(pj_rectifying_radius(kQnan)));

  constexpr std::array<double, 2> kCoeffs = {1.0, 2.0};
  EXPECT_TRUE(std::isnan(pj_polyval(kQnan, kCoeffs.data(), 1)));
  EXPECT_TRUE(std::isnan(pj_clenshaw(kQnan, 1.0, kCoeffs.data(), 2)));
}

}  // namespace
}  // namespace proj
