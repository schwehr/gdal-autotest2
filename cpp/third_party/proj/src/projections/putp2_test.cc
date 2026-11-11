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
// Unit tests for the Putnins P2 pseudocylindrical projection
// (proj/src/projections/putp2.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/putp2.cpp

#include <array>
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
constexpr double kCx = 1.89490;
constexpr double kCy = 1.71848;
constexpr double kPiDiv3 = 1.0471975511965977;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Putp2Test, MetadataAndForcesSphericalMode) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=putp2 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("putp2", info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", info.description);
  EXPECT_STREQ("Putnins P2\n\tPCyl, Sph", pj->descr);
  EXPECT_EQ(1, info.has_inverse);
}

TEST(Putp2Test, ForwardEquatorAndPoleIterationExhaustionClampBranch) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=putp2 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Equator (phi = 0 => theta = 0): x = C_x * lam * (1 - 0.5) = 0.5 * C_x *
  // lam,
  //    y = 0.
  constexpr double kLon = DegToRad(60.0);
  const PJ_COORD eq =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLon, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.5 * kCx * kLon, eq.xy.x, 1e-12);
  EXPECT_NEAR(0.0, eq.xy.y, 1e-12);

  // 2. At the poles (phi = +-pi/2), the derivative 1 +
  // cos(theta)*(cos(theta)-1)
  //    - sin^2(theta) vanishes at theta = +-pi/3, so Newton's method converges
  //    only linearly and exhausts NITER = 10 iterations (`!i`), clamping theta
  //    to +-PI_DIV_3 where cos(+-pi/3) = 0.5 and x = 0 (pointed poles).
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLon, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, north_pole.xy.x, 1e-12);
  EXPECT_NEAR(kCy * std::sin(kPiDiv3), north_pole.xy.y, 1e-12);

  const PJ_COORD south_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLon, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, south_pole.xy.x, 1e-12);
  EXPECT_NEAR(-kCy * std::sin(kPiDiv3), south_pole.xy.y, 1e-12);
}

TEST(Putp2Test, ForwardAndInverseRoundTripAcrossLatitudes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=putp2 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 15.0, .lat_deg = 20.0},
      {.lon_deg = -75.0, .lat_deg = 45.0},
      {.lon_deg = 130.0, .lat_deg = -35.0},
      {.lon_deg = -165.0, .lat_deg = -70.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);

    // 1. Forward projection.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 25.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));

    // 2. Inverse projection recovers (lam, phi).
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-10);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-10);
    EXPECT_NEAR(25.0, inv.lpz.z, 1e-12);
  }
}

TEST(Putp2Test, InverseOutOfDomainArrayAndDistortionFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=putp2 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. |y| > C_y triggers domain error in first aasin(ctx, xy.y / C_y).
  const PJ_COORD out_y =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 2.0, 0.0, 0.0));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  EXPECT_EQ(HUGE_VAL, out_y.lp.lam);
  proj_errno_reset(pj.get());

  // 2. At the pole y = +-C_y * sin(pi/3), theta = +-pi/3 maximizes
  //    |theta + sin(theta)*(cos(theta) - 1)| / C_p slightly above 1.0 (within
  //    ONE_TOL), so the second aasin() clamps phi to +-pi/2 without error.
  const double pole_y = kCy * std::sin(kPiDiv3);
  const PJ_COORD inv_north_pole =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, pole_y, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, inv_north_pole.lp.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, inv_north_pole.lp.phi, 1e-7);

  const PJ_COORD inv_south_pole =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, -pole_y, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, inv_south_pole.lp.lam, 1e-12);
  EXPECT_NEAR(-kHalfPi, inv_south_pole.lp.phi, 1e-7);

  // 3. Batch transform and equal-area distortion factors.
  std::array<PJ_COORD, 2> coords = {
      proj_coord(DegToRad(30.0), DegToRad(25.0), 0.0, 0.0),
      proj_coord(DegToRad(-60.0), DegToRad(-40.0), 0.0, 0.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(30.0), coords[0].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(25.0), coords[0].lp.phi, 1e-10);
  EXPECT_NEAR(DegToRad(-60.0), coords[1].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(-40.0), coords[1].lp.phi, 1e-10);

  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(15.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-4);
}

}  // namespace
}  // namespace proj
