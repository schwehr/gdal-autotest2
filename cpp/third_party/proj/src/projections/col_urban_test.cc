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
// Unit tests for the Colombia Urban projection
// (proj/src/projections/col_urban.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/col_urban.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(ColUrbanTest, IogpBogotaReferenceForwardAndInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // IOGP Guidance Note 7-2 Colombia Urban example configuration (Bogota):
  // GRS80 ellipsoid, lat_0 = 4d40'49.75"N, lon_0 = 74d08'47.73"W, h_0 = 2550m,
  // false easting = 92334.879m, false northing = 109320.965m.
  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=col_urban +ellps=GRS80 +lat_0=4.680486111111111 "
      "+lon_0=-74.14659166666667 +h_0=2550 +x_0=92334.879 +y_0=109320.965"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  // 1. Projection origin maps exactly to (x_0, y_0).
  const PJ_COORD origin_in = proj_coord(DegToRad(-74.14659166666667),
                                        DegToRad(4.680486111111111), 0.0, 0.0);
  const PJ_COORD origin_fwd = proj_trans(pj.get(), PJ_FWD, origin_in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(92334.879, origin_fwd.xy.x, 1e-6);
  EXPECT_NEAR(109320.965, origin_fwd.xy.y, 1e-6);

  // 2. Round-trip multiple urban-extent offsets around Bogota.
  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = -74.14659166666667, .lat_deg = 4.680486111111111},
      {.lon_deg = -74.05, .lat_deg = 4.60},
      {.lon_deg = -74.25, .lat_deg = 4.75},
      {.lon_deg = -74.00, .lat_deg = 4.80},
      {.lon_deg = -74.30, .lat_deg = 4.50},
  };

  for (const auto& tc : kCases) {
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(fwd.xy.x));
    EXPECT_FALSE(std::isinf(fwd.xy.y));

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-9);
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-9);
  }
}

TEST(ColUrbanTest, AnalyticalFormulaVerificationWithUnitSphereAndEllipsoid) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Verify the normalized projection equations in pj_fwd and pj_inv on an
  // ellipsoid with explicit h_0 and lat_0.
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=col_urban +ellps=WGS84 +lat_0=10 +h_0=1500"));
  ASSERT_NE(nullptr, pj.get());

  const double a = pj->a;
  const double es = pj->es;
  const double phi0 = DegToRad(10.0);
  const double h0 = 1500.0 / a;
  const double sinphi0 = std::sin(phi0);
  const double nu0 = 1.0 / std::sqrt(1.0 - es * sinphi0 * sinphi0);
  const double A = 1.0 + h0 / nu0;
  const double rho0 = (1.0 - es) / std::pow(1.0 - es * sinphi0 * sinphi0, 1.5);
  const double B = std::tan(phi0) / (2.0 * rho0 * nu0);
  const double C = 1.0 + h0;
  const double D = rho0 * (1.0 + h0 / (1.0 - es));

  const PJ_LP lp = {.lam = DegToRad(0.25), .phi = DegToRad(10.2)};
  const double cosphi = std::cos(lp.phi);
  const double sinphi = std::sin(lp.phi);
  const double nu = 1.0 / std::sqrt(1.0 - es * sinphi * sinphi);
  const double lam_nu_cosphi = lp.lam * nu * cosphi;
  const double expected_norm_x = A * lam_nu_cosphi;
  const double sinphi_m = std::sin(0.5 * (lp.phi + phi0));
  const double rho_m =
      (1.0 - es) / std::pow(1.0 - es * sinphi_m * sinphi_m, 1.5);
  const double G = 1.0 + h0 / rho_m;
  const double expected_norm_y =
      G * rho0 * ((lp.phi - phi0) + B * lam_nu_cosphi * lam_nu_cosphi);

  // 1. Forward via pj_fwd scales normalized xy by semi-major axis a.
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(a * expected_norm_x, xy.x, 1e-6);
  EXPECT_NEAR(a * expected_norm_y, xy.y, 1e-6);

  // 2. Inverse via pj_inv matches the closed-form inverse formula.
  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const double expected_inv_phi =
      phi0 + expected_norm_y / D -
      B * (expected_norm_x / C) * (expected_norm_x / C);
  const double sin_inv_phi = std::sin(expected_inv_phi);
  const double nu_inv = 1.0 / std::sqrt(1.0 - es * sin_inv_phi * sin_inv_phi);
  const double expected_inv_lam =
      expected_norm_x / (C * nu_inv * std::cos(expected_inv_phi));
  EXPECT_NEAR(expected_inv_phi, inv_lp.phi, 1e-12);
  EXPECT_NEAR(expected_inv_lam, inv_lp.lam, 1e-12);
}

TEST(ColUrbanTest, DefaultZeroHeightAndSphericalRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Omitting +h_0 defaults h_0 to 0; +R=6371000 sets es = 0.
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=col_urban +R=6371000 +lat_0=-25 +lon_0=15"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0.0, pj->es);

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(15.1), DegToRad(-24.9), 0.0, 0.0),
      proj_coord(DegToRad(14.85), DegToRad(-25.15), 0.0, 0.0),
  };
  const PJ_COORD orig0 = coords[0];
  const PJ_COORD orig1 = coords[1];

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(orig0.lp.lam, coords[0].lp.lam, 1e-9);
  EXPECT_NEAR(orig0.lp.phi, coords[0].lp.phi, 1e-9);
  EXPECT_NEAR(orig1.lp.lam, coords[1].lp.lam, 1e-9);
  EXPECT_NEAR(orig1.lp.phi, coords[1].lp.phi, 1e-9);

  const PJ_FACTORS factors = proj_factors(pj.get(), orig0);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.meridional_scale, 1e-3);
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-3);
}

}  // namespace
}  // namespace proj
