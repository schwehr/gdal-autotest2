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
// Unit tests for numerical partial derivatives in pj_deriv (proj/src/deriv.cpp)
// and integration through proj_factors (proj/src/factors.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/deriv.cpp

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

TEST(DerivTest, NullFwdFunctionReturnsFailure) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // +proj=noop uses fwd4d and leaves legacy 2D fwd pointer null.
  ScopedPj noop(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_NE(nullptr, noop.get());
  ASSERT_EQ(nullptr, noop->fwd);

  DERIVS der = {.x_l = 0.0, .x_p = 0.0, .y_l = 0.0, .y_p = 0.0};
  EXPECT_EQ(1, pj_deriv({.lam = 0.0, .phi = 0.0}, 1e-5, noop.get(), &der));
}

TEST(DerivTest, MercatorSphereAndEllipsoidMatchAnalyticalJacobian) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Spherical Mercator (R=1): x = lam, y = asinh(tan(phi)),
  //    dy/dphi = sec(phi).
  ScopedPj merc_sph(proj_create(ctx.get(), "+proj=merc +R=1"));
  ASSERT_NE(nullptr, merc_sph.get());
  ASSERT_NE(nullptr, merc_sph->fwd);

  const PJ_LP lp_sph = {.lam = 0.25, .phi = kPi / 4.0};
  DERIVS der_sph{};
  EXPECT_EQ(0, pj_deriv(lp_sph, 1e-6, merc_sph.get(), &der_sph));
  EXPECT_NEAR(1.0, der_sph.x_l, 1e-10);
  EXPECT_NEAR(0.0, der_sph.x_p, 1e-10);
  EXPECT_NEAR(0.0, der_sph.y_l, 1e-10);
  EXPECT_NEAR(1.0 / std::cos(lp_sph.phi), der_sph.y_p, 1e-8);

  // 2. Ellipsoidal Mercator:
  //    dy/dphi = (1 - e^2) / (cos(phi) * (1 - e^2 * sin^2(phi))).
  ScopedPj merc_ell(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, merc_ell.get());
  ASSERT_NE(nullptr, merc_ell->fwd);

  const PJ_LP lp_ell = {.lam = -0.4, .phi = 0.6};
  DERIVS der_ell{};
  EXPECT_EQ(0, pj_deriv(lp_ell, 1e-6, merc_ell.get(), &der_ell));
  const double sinphi = std::sin(lp_ell.phi);
  const double expected_y_p =
      merc_ell->one_es /
      (std::cos(lp_ell.phi) * (1.0 - merc_ell->es * sinphi * sinphi));
  EXPECT_NEAR(1.0, der_ell.x_l, 1e-10);
  EXPECT_NEAR(0.0, der_ell.x_p, 1e-10);
  EXPECT_NEAR(0.0, der_ell.y_l, 1e-10);
  EXPECT_NEAR(expected_y_p, der_ell.y_p, 1e-8);
}

TEST(DerivTest, ObliqueStereographicNonZeroOffDiagonalsAndCauchyRiemann) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj stere(proj_create(ctx.get(), "+proj=stere +R=1 +lat_0=45 +k_0=1"));
  ASSERT_NE(nullptr, stere.get());
  ASSERT_NE(nullptr, stere->fwd);

  const PJ_LP lp = {.lam = 0.3, .phi = 0.5};
  DERIVS der{};
  EXPECT_EQ(0, pj_deriv(lp, 1e-6, stere.get(), &der));

  // Verify analytical Jacobian on unit-sphere oblique stereographic
  // (phi0=pi/4):
  //   x = 2*cosphi*sinlam / D
  //   y = 2*(cosphi0*sinphi - sinphi0*cosphi*coslam) / D
  constexpr double phi0 = kPi / 4.0;
  const double sinphi0 = std::sin(phi0);
  const double cosphi0 = std::cos(phi0);
  const double sinphi = std::sin(lp.phi);
  const double cosphi = std::cos(lp.phi);
  const double sinlam = std::sin(lp.lam);
  const double coslam = std::cos(lp.lam);
  const double denom = 1.0 + sinphi0 * sinphi + cosphi0 * cosphi * coslam;
  const double d_lam = -cosphi0 * cosphi * sinlam;
  const double d_phi = sinphi0 * cosphi - cosphi0 * sinphi * coslam;

  const double num_x = 2.0 * cosphi * sinlam;
  const double num_x_lam = 2.0 * cosphi * coslam;
  const double num_x_phi = -2.0 * sinphi * sinlam;

  const double num_y = 2.0 * (cosphi0 * sinphi - sinphi0 * cosphi * coslam);
  const double num_y_lam = 2.0 * sinphi0 * cosphi * sinlam;
  const double num_y_phi = 2.0 * (cosphi0 * cosphi + sinphi0 * sinphi * coslam);

  const double denom2 = denom * denom;
  const double expected_x_l = (num_x_lam * denom - num_x * d_lam) / denom2;
  const double expected_x_p = (num_x_phi * denom - num_x * d_phi) / denom2;
  const double expected_y_l = (num_y_lam * denom - num_y * d_lam) / denom2;
  const double expected_y_p = (num_y_phi * denom - num_y * d_phi) / denom2;

  EXPECT_GT(std::abs(expected_x_l), 0.1);
  EXPECT_GT(std::abs(expected_x_p), 0.1);
  EXPECT_GT(std::abs(expected_y_l), 0.1);
  EXPECT_GT(std::abs(expected_y_p), 0.1);

  EXPECT_NEAR(expected_x_l, der.x_l, 1e-8);
  EXPECT_NEAR(expected_x_p, der.x_p, 1e-8);
  EXPECT_NEAR(expected_y_l, der.y_l, 1e-8);
  EXPECT_NEAR(expected_y_p, der.y_p, 1e-8);

  // Conformal Cauchy-Riemann check: dx/dlam = cos(phi)*dy/dphi,
  // dy/dlam = -cos(phi)*dx/dphi.
  EXPECT_NEAR(cosphi * der.y_p, der.x_l, 1e-8);
  EXPECT_NEAR(-cosphi * der.x_p, der.y_l, 1e-8);
}

TEST(DerivTest, LatitudePolarOvershootAtCornerOneAndTwoReturnsFailure) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +R=1"));
  ASSERT_NE(nullptr, merc.get());
  DERIVS der{};

  // Corner 1 (phi + h) overshoots +pi/2.
  EXPECT_EQ(
      1, pj_deriv({.lam = 0.0, .phi = kHalfPi - 1e-6}, 1e-5, merc.get(), &der));

  // Corner 1 (-pi/2 + 9e-6) stays in bounds, then Corner 2 (phi - h) overshoots
  // -pi/2.
  EXPECT_EQ(1, pj_deriv({.lam = 0.0, .phi = -kHalfPi + 1e-6}, 1e-5, merc.get(),
                        &der));
}

TEST(DerivTest, ForwardDomainErrorAtEachOfFourCornersReturnsFailure) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // On unit-sphere orthographic, Q->fwd returns xy.x = HUGE_VAL whenever the
  // visibility dot product sin(phi0)*sin(phi) + cos(phi0)*cos(phi)*cos(lam) is
  // < -1e-10. Choosing lat_0 = +/-45 deg near lam = +/-pi/2 isolates each of
  // the 4 evaluation corners (+h,+h), (+h,-h), (-h,-h), (-h,+h) independently.
  struct CornerCase {
    const char* proj_str;
    PJ_LP lp;
    double h;
  };
  constexpr CornerCase kCases[] = {
      // Corner 1 (+h, +h) fails immediately on equatorial horizon.
      {.proj_str = "+proj=ortho +R=1 +lat_0=0",
       .lp = {.lam = kHalfPi, .phi = 0.0},
       .h = 0.01},
      // Corner 1 passes, Corner 2 (+h, -h) fails.
      {.proj_str = "+proj=ortho +R=1 +lat_0=45",
       .lp = {.lam = kHalfPi, .phi = 0.01},
       .h = 0.02},
      // Corners 1 & 2 pass, Corner 3 (-h, -h) fails.
      {.proj_str = "+proj=ortho +R=1 +lat_0=45",
       .lp = {.lam = -kHalfPi, .phi = 0.01},
       .h = 0.02},
      // Corners 1, 2 & 3 pass, Corner 4 (-h, +h) fails.
      {.proj_str = "+proj=ortho +R=1 +lat_0=-45",
       .lp = {.lam = -kHalfPi, .phi = -0.01},
       .h = 0.02},
  };

  for (const auto& tc : kCases) {
    ScopedPj ortho(proj_create(ctx.get(), tc.proj_str));
    ASSERT_NE(nullptr, ortho.get()) << tc.proj_str;
    ASSERT_NE(nullptr, ortho->fwd) << tc.proj_str;

    DERIVS der{};
    EXPECT_EQ(1, pj_deriv(tc.lp, tc.h, ortho.get(), &der)) << tc.proj_str;
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(ortho.get()));
    proj_errno_reset(ortho.get());
  }
}

TEST(DerivTest, ProjFactorsIntegrationForConversionAndProjectedCrs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj utm32(proj_create(ctx.get(), "EPSG:32632"));
  ASSERT_NE(nullptr, utm32.get());

  // proj_factors takes (lon, lat) in radians even for projected CRS objects.
  const PJ_COORD copenhagen =
      proj_coord(proj_torad(12.0), proj_torad(55.0), 0.0, 0.0);
  const PJ_FACTORS fac = proj_factors(utm32.get(), copenhagen);
  EXPECT_EQ(0, proj_errno(utm32.get()));
  EXPECT_NEAR(1.0, fac.meridional_scale, 1e-3);
  EXPECT_NEAR(1.0, fac.parallel_scale, 1e-3);
  EXPECT_NEAR(0.0, fac.angular_distortion, 1e-6);
  EXPECT_GT(std::abs(fac.dy_dphi), 0.1);
}

}  // namespace
}  // namespace proj
