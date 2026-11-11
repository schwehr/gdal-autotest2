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
// Unit tests for the McBryde-Thomas Flat-Pole Sine (No. 2) projection
// (proj/src/projections/mbt_fps.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/mbt_fps.cpp

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
constexpr double kCx = 0.22248;
constexpr double kCy = 1.44492;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(MbtFpsTest, MetadataAndForcesSphericalMode) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=mbt_fps +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("mbt_fps", info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", info.description);
  EXPECT_STREQ("McBryde-Thomas Flat-Pole Sine (No. 2)\n\tCyl, Sph", pj->descr);
  EXPECT_EQ(1, info.has_inverse);
}

TEST(MbtFpsTest, ForwardEquatorFlatPolesAndIterationExhaustion) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=mbt_fps +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Equator (phi = 0 => theta = 0, t = 0):
  //    x = C_x * lam * (1 + 3) = 4 * C_x * lam, y = 0.
  constexpr double kLon = DegToRad(45.0);
  const PJ_COORD eq =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLon, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(4.0 * kCx * kLon, eq.xy.x, 1e-12);
  EXPECT_NEAR(0.0, eq.xy.y, 1e-12);

  // 2. Flat poles (phi = +-pi/2): x is 1/4 of the equatorial length
  //    (C_x * lam), up to ~3.1e-5 because C1, C2, C3 in mbt_fps.cpp are
  //    5-decimal-place constants so the solved theta at phi = +-pi/2 differs
  //    from +-pi/2 by ~6e-5.
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLon, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kCx * kLon, north_pole.xy.x, 1e-4);
  EXPECT_GT(north_pole.xy.y, 1.3);

  const PJ_COORD south_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(-kLon, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kCx * kLon, south_pole.xy.x, 1e-4);
  EXPECT_LT(south_pole.xy.y, -1.3);

  // 3. Direct pj->fwd with a large initial phi (1e6 rad) bypasses fwd_prepare's
  //    |phi| <= pi/2 check and exercises the Newton loop running all
  //    MAX_ITER = 10 iterations without early break.
  ASSERT_NE(nullptr, pj->fwd);
  const PJ_XY exhausted = pj->fwd({.lam = 0.1, .phi = 1e6}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_FALSE(std::isnan(exhausted.x));
  EXPECT_FALSE(std::isnan(exhausted.y));
}

TEST(MbtFpsTest, ForwardAndInverseRoundTripAcrossLatitudes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=mbt_fps +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 15.0, .lat_deg = 25.0},
      {.lon_deg = -75.0, .lat_deg = 45.0},
      {.lon_deg = 135.0, .lat_deg = -60.0},
      {.lon_deg = -170.0, .lat_deg = -80.0},
      {.lon_deg = 60.0, .lat_deg = 90.0},
      {.lon_deg = -60.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);

    // 1. Forward projection.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 10.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));

    // 2. Inverse projection recovers (lam, phi).
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-6);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-6);
    EXPECT_NEAR(10.0, inv.lpz.z, 1e-12);
  }
}

TEST(MbtFpsTest, InverseOutOfDomainArrayAndDistortionFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=mbt_fps +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. |y| > C_y triggers domain error in first aasin(ctx, xy.y / C_y).
  const PJ_COORD out_y =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 2.0, 0.0, 0.0));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  EXPECT_EQ(HUGE_VAL, out_y.lp.lam);
  proj_errno_reset(pj.get());

  // 2. Batch transform and equal-area distortion factors.
  std::array<PJ_COORD, 2> coords = {
      proj_coord(DegToRad(30.0), DegToRad(20.0), 0.0, 0.0),
      proj_coord(DegToRad(-90.0), DegToRad(-50.0), 0.0, 0.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(30.0), coords[0].lp.lam, 1e-6);
  EXPECT_NEAR(DegToRad(20.0), coords[0].lp.phi, 1e-6);
  EXPECT_NEAR(DegToRad(-90.0), coords[1].lp.lam, 1e-6);
  EXPECT_NEAR(DegToRad(-50.0), coords[1].lp.phi, 1e-6);

  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(15.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-4);
}

}  // namespace
}  // namespace proj
