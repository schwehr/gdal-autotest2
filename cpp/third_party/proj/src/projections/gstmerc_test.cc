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
// Unit tests for the Gauss-Schreiber Transverse Mercator (Gauss-Laborde
// Reunion) projection (proj/src/projections/gstmerc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/gstmerc.cpp

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

TEST(GstmercTest, ReunionEllipsoidalOriginAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Gauss-Laborde Reunion configuration (Piton des Neiges origin).
  ScopedPj pj(
      proj_create(ctx.get(),
                  "+proj=gstmerc +ellps=intl +lat_0=-21.11666666666667 "
                  "+lon_0=55.53333333333333 +k_0=1.0 +x_0=160000 +y_0=50000"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  // 1. Origin (lon_0, lat_0) maps directly to false easting/northing (x_0,
  // y_0).
  const PJ_COORD origin_in = proj_coord(DegToRad(55.53333333333333),
                                        DegToRad(-21.11666666666667), 0.0, 0.0);
  const PJ_COORD origin_fwd = proj_trans(pj.get(), PJ_FWD, origin_in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(160000.0, origin_fwd.xy.x, 1e-6);
  EXPECT_NEAR(50000.0, origin_fwd.xy.y, 1e-6);

  // 2. Round-trip points in all four quadrants around the projection center.
  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 55.53333333333333, .lat_deg = -21.11666666666667},
      {.lon_deg = 55.8, .lat_deg = -20.9},
      {.lon_deg = 55.2, .lat_deg = -20.9},
      {.lon_deg = 55.8, .lat_deg = -21.4},
      {.lon_deg = 55.2, .lat_deg = -21.4},
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
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-11);
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-11);
  }
}

TEST(GstmercTest, SphericalEquatorialMatchesStandardTransverseMercator) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // On a sphere (es = 0, e = 0) with lat_0 = 0, n1 = 1, phic = 0, c = 0,
  // YS = 0, n2 = k0 * R.
  constexpr double kRadius = 6371000.0;
  constexpr double kScale = 0.9996;
  ScopedPj pj(proj_create(
      ctx.get(), "+proj=gstmerc +R=6371000 +lat_0=0 +lon_0=0 +k_0=0.9996"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP lp = {.lam = DegToRad(5.0), .phi = DegToRad(30.0)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  // Analytical spherical Transverse Mercator formula:
  // sinLs1 = cos(phi) * sin(lam), x = k0 * R * atanh(sinLs1),
  // y = k0 * R * atan(tan(phi) / cos(lam)).
  const double sin_ls1 = std::cos(lp.phi) * std::sin(lp.lam);
  const double expected_x = kScale * kRadius * std::atanh(sin_ls1);
  const double expected_y =
      kScale * kRadius * std::atan(std::tan(lp.phi) / std::cos(lp.lam));
  EXPECT_NEAR(expected_x, xy.x, 1e-5);
  EXPECT_NEAR(expected_y, xy.y, 1e-5);

  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, inv_lp.lam, 1e-12);
  EXPECT_NEAR(lp.phi, inv_lp.phi, 1e-12);
}

TEST(GstmercTest, ConformalityFactorsAndTransArray) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(
      ctx.get(), "+proj=gstmerc +ellps=WGS84 +lat_0=45 +lon_0=10 +k_0=0.9996"));
  ASSERT_NE(nullptr, pj.get());

  // Gauss-Schreiber Transverse Mercator is strictly conformal: h == k and
  // angular_distortion == 0.
  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(12.0), DegToRad(46.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(factors.meridional_scale, factors.parallel_scale, 1e-7);
  EXPECT_NEAR(0.0, factors.angular_distortion, 1e-7);
  EXPECT_NEAR(kPi / 2.0, factors.meridian_parallel_angle, 1e-7);

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(11.0), DegToRad(44.0), 0.0, 0.0),
      proj_coord(DegToRad(9.0), DegToRad(46.5), 0.0, 0.0),
  };
  const PJ_COORD orig0 = coords[0];
  const PJ_COORD orig1 = coords[1];

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(orig0.lp.lam, coords[0].lp.lam, 1e-11);
  EXPECT_NEAR(orig0.lp.phi, coords[0].lp.phi, 1e-11);
  EXPECT_NEAR(orig1.lp.lam, coords[1].lp.lam, 1e-11);
  EXPECT_NEAR(orig1.lp.phi, coords[1].lp.phi, 1e-11);
}

}  // namespace
}  // namespace proj
