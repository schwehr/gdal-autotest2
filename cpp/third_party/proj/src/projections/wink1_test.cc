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
// Unit tests for the Winkel I pseudocylindrical projection
// (proj/src/projections/wink1.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/wink1.cpp

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

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Wink1Test, DefaultLatTsZeroMatchesSinusoidalEquirectangularAverage) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=wink1 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  EXPECT_EQ(0.0, pj->es);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("wink1", info.id);

  // When lat_ts is omitted, cosphi1 = cos(0) = 1.0, so:
  //   x = 0.5 * lam * (1 + cos(phi))
  //   y = phi
  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},   {.lon_deg = 30.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 60.0}, {.lon_deg = -120.0, .lat_deg = -45.0},
      {.lon_deg = 90.0, .lat_deg = 90.0}, {.lon_deg = -90.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);
    const double expected_x = 0.5 * lam * (1.0 + std::cos(phi));
    const double expected_y = phi;

    // 1. Forward: geodetic (lam, phi) -> projected (x, y).
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected_x, fwd.xy.x, 1e-12);
    EXPECT_NEAR(expected_y, fwd.xy.y, 1e-12);

    // 2. Inverse: projected (x, y) -> geodetic (lam, phi).
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-12);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-12);
  }
}

TEST(Wink1Test, CustomLatTsScalesParallelLengthAndRoundTrips) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=wink1 +lat_ts=50 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const double cosphi1 = std::cos(DegToRad(50.0));
  const double lam = DegToRad(35.0);
  const double phi = DegToRad(-25.0);

  // 1. Verify forward formula with custom cosphi1 = cos(50 deg).
  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.5 * lam * (cosphi1 + std::cos(phi)), fwd.xy.x, 1e-12);
  EXPECT_NEAR(phi, fwd.xy.y, 1e-12);

  // 2. Verify inverse at both mid-latitudes and the poles (where cos(phi) = 0,
  // but cosphi1 > 0 keeps the pole line of finite non-zero length).
  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, inv.lp.lam, 1e-12);
  EXPECT_NEAR(phi, inv.lp.phi, 1e-12);

  const PJ_COORD pole_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.5 * lam * cosphi1, pole_fwd.xy.x, 1e-12);
  EXPECT_NEAR(kHalfPi, pole_fwd.xy.y, 1e-12);

  const PJ_COORD pole_inv = proj_trans(pj.get(), PJ_INV, pole_fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, pole_inv.lp.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, pole_inv.lp.phi, 1e-12);
}

TEST(Wink1Test, EllipsoidForcedToSphereClassic2DAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=wink1 +lat_ts=30 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0.0, pj->es);

  const PJ_LP lp = {.lam = DegToRad(-60.0), .phi = DegToRad(30.0)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, inv_lp.lam, 1e-12);
  EXPECT_NEAR(lp.phi, inv_lp.phi, 1e-12);

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(15.0), DegToRad(30.0), 0.0, 0.0),
      proj_coord(DegToRad(-45.0), DegToRad(-30.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(DegToRad(15.0), coords[0].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(30.0), coords[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(-45.0), coords[1].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-30.0), coords[1].lp.phi, 1e-12);

  // At phi = lat_ts = 30 deg, parallel scale k = 0.5 * (cos(30) + cos(30)) /
  // cos(30) = 1.0 (true scale along standard parallels +-lat_ts).
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-7);
}

}  // namespace
}  // namespace proj
