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
// Unit tests for the Times cylindrical projection
// (proj/src/projections/times.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/times.cpp

#include <array>
#include <cmath>
#include <numbers>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;
constexpr double kQuarterPi = kPi / 4.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(TimesTest, MetadataAndForcesSphericalModeOnEllipsoid) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=times +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("times", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Times"), std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
}

TEST(TimesTest, ForwardMatchesClosedFormFormulaOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=times +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Origin maps to (0, 0).
  const PJ_COORD origin =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, origin.xy.x, 1e-12);
  EXPECT_NEAR(0.0, origin.xy.y, 1e-12);

  // 2. Equator (phi = 0, T = 0, S = 0): x = 0.74482 * lam, y = 0.
  constexpr double kLam = DegToRad(30.0);
  const PJ_COORD eq =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLam, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.74482 * kLam, eq.xy.x, 1e-12);
  EXPECT_NEAR(0.0, eq.xy.y, 1e-12);

  // 3. General coordinate (lon = 45 deg, lat = 60 deg) matches Snyder p. 213:
  //    T = tan(phi / 2), S = sin(pi/4 * T),
  //    x = lam * (0.74482 - 0.34588 * S^2), y = 1.70711 * T.
  constexpr double kLon = DegToRad(45.0);
  constexpr double kLat = DegToRad(60.0);
  const double t = std::tan(kLat / 2.0);
  const double s = std::sin(kQuarterPi * t);
  const double expected_x = kLon * (0.74482 - 0.34588 * s * s);
  const double expected_y = 1.70711 * t;

  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLon, kLat, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(expected_x, fwd.xy.x, 1e-12);
  EXPECT_NEAR(expected_y, fwd.xy.y, 1e-12);

  // 4. North and South Poles (phi = +-pi/2 => T = +-1, S = +-sin(pi/4)).
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLon, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kLon * (0.74482 - 0.34588 * 0.5), north_pole.xy.x, 1e-12);
  EXPECT_NEAR(1.70711, north_pole.xy.y, 1e-12);

  const PJ_COORD south_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(-kLon, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kLon * (0.74482 - 0.34588 * 0.5), south_pole.xy.x, 1e-12);
  EXPECT_NEAR(-1.70711, south_pole.xy.y, 1e-12);
}

TEST(TimesTest, ForwardAndInverseRoundTripAcrossLatitudesAndQuadrants) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=times +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 15.0, .lat_deg = 30.0},
      {.lon_deg = -75.0, .lat_deg = 45.0},
      {.lon_deg = 120.0, .lat_deg = -60.0},
      {.lon_deg = -179.0, .lat_deg = -85.0},
      {.lon_deg = 90.0, .lat_deg = 90.0},
      {.lon_deg = -90.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);

    // 1. Forward projection.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 100.0, 2026.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(fwd.xy.x));
    EXPECT_FALSE(std::isinf(fwd.xy.y));

    // 2. Inverse projection recovers (lon, lat) and preserves (z, t).
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-12);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-12);
    EXPECT_NEAR(100.0, inv.lpzt.z, 1e-12);
    EXPECT_NEAR(2026.0, inv.lpzt.t, 1e-12);
  }
}

TEST(TimesTest, Classic2DFwdInvArrayAndDistortionFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=times +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP lp = {.lam = DegToRad(-25.0), .phi = DegToRad(40.0)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP roundtrip = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, roundtrip.lam, 1e-12);
  EXPECT_NEAR(lp.phi, roundtrip.phi, 1e-12);

  std::array<PJ_COORD, 2> coords = {
      proj_coord(DegToRad(10.0), DegToRad(20.0), 0.0, 0.0),
      proj_coord(DegToRad(-50.0), DegToRad(-35.0), 0.0, 0.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(10.0), coords[0].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(20.0), coords[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(-50.0), coords[1].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-35.0), coords[1].lp.phi, 1e-12);

  // At the origin (0, 0): dx/dlam = 0.74482, dy/dphi = 1.70711 / 2 = 0.853555.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.70711 / 2.0, factors.meridional_scale, 1e-5);
  EXPECT_NEAR(0.74482, factors.parallel_scale, 1e-5);
  EXPECT_NEAR((1.70711 / 2.0) * 0.74482, factors.areal_scale, 1e-5);
}

}  // namespace
}  // namespace proj
