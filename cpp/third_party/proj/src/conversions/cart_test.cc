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
// Unit tests for geodetic <-> geocentric Cartesian conversions
// (proj/src/conversions/cart.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/conversions/cart.cpp

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

TEST(CartTest, Ellipsoidal3DRoundTripEquatorPrimeMeridianPolesAndHeight) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=cart +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  struct Case3D {
    double lon_rad;
    double lat_rad;
    double height_m;
  };
  constexpr Case3D kCases[] = {
      {.lon_rad = 0.0, .lat_rad = 0.0, .height_m = 0.0},
      {.lon_rad = DegToRad(12.0),
       .lat_rad = DegToRad(55.0),
       .height_m = 123.45},
      {.lon_rad = DegToRad(-122.0),
       .lat_rad = DegToRad(-33.5),
       .height_m = 2500.0},
      // Exact north and south poles trigger the near-pole geocentric_radius
      // branch.
      {.lon_rad = 0.0, .lat_rad = kHalfPi, .height_m = 100.0},
      {.lon_rad = 0.0, .lat_rad = -kHalfPi, .height_m = 50.0},
      // Near-pole latitude (> 89.99994 deg -> cosphi < 1e-6) avoids N/cosphi
      // blowup.
      {.lon_rad = DegToRad(45.0),
       .lat_rad = DegToRad(89.99999),
       .height_m = 42.0},
  };

  for (const auto& tc : kCases) {
    // 1. Forward: geodetic (lon, lat in rad, height in m) -> (X, Y, Z in m).
    const PJ_COORD fwd = proj_trans(
        pj.get(), PJ_FWD, proj_coord(tc.lon_rad, tc.lat_rad, tc.height_m, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(fwd.xyz.x));

    // 2. Inverse: (X, Y, Z) -> geodetic (lon, lat, height).
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(tc.lat_rad, inv.lpz.phi, 1e-12);
    EXPECT_NEAR(tc.height_m, inv.lpz.z, 1e-5);
    if (std::abs(std::abs(tc.lat_rad) - kHalfPi) > 1e-9) {
      EXPECT_NEAR(tc.lon_rad, inv.lpz.lam, 1e-12);
    }
  }
}

TEST(CartTest, SphericalEllipsoidMatchesExactSphereRadiusBranch) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  constexpr double kRadius = 6371000.0;
  ScopedPj pj(proj_create(ctx.get(), "+proj=cart +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Verify es == 0 fast path in normal_radius_of_curvature at pole & 45 deg.
  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, kPi / 4.0, 1000.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  const double expected = (kRadius + 1000.0) * std::cos(kPi / 4.0);
  EXPECT_NEAR(expected, fwd.xyz.x, 1e-6);
  EXPECT_NEAR(0.0, fwd.xyz.y, 1e-9);
  EXPECT_NEAR(expected, fwd.xyz.z, 1e-6);

  // 2. Round-trip back via 3D inverse.
  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, inv.lpz.lam, 1e-12);
  EXPECT_NEAR(kPi / 4.0, inv.lpz.phi, 1e-12);
  EXPECT_NEAR(1000.0, inv.lpz.z, 1e-6);
}

TEST(CartTest, InverseOriginAndNearOriginZClampingBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=cart +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // Exact origin (0, 0, 0) exercises norm == 0, norm_phi == 0, and x_phi <= 0.
  const PJ_COORD origin_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kHalfPi, origin_inv.lpz.phi, 1e-12);

  // Small negative Z with X=Y=0 exercises x_phi <= 0 with cart.z < 0 (-pi/2).
  const PJ_COORD neg_z_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 0.0, -1.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kHalfPi, neg_z_inv.lpz.phi, 1e-12);
}

TEST(CartTest, Classic2DFwdAndInvEntryPointsRoundTripZeroHeight) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=cart +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP in_lp = {.lam = DegToRad(10.0), .phi = DegToRad(48.0)};
  const PJ_XY xy = pj_fwd(in_lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_FALSE(std::isinf(xy.x));

  // Verify classic 2D fwd sets z=0 (geodetic height=0 on WGS84 at 48N).
  const PJ_COORD fwd3d =
      proj_trans(pj.get(), PJ_FWD, proj_coord(in_lp.lam, in_lp.phi, 0.0, 0.0));
  EXPECT_NEAR(fwd3d.xyz.x, xy.x, 1e-9);
  EXPECT_NEAR(fwd3d.xyz.y, xy.y, 1e-9);

  // Classic 2D inv sets Z=0, returning phi=0 (equatorial plane) and exact lam.
  const PJ_LP out_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_lp.lam, out_lp.lam, 1e-12);
  EXPECT_NEAR(0.0, out_lp.phi, 1e-12);
}

}  // namespace
}  // namespace proj
