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
// Unit tests for the McBride-Thomas Flat-Polar Parabolic projection
// (proj/src/projections/mbtfpp.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/mbtfpp.cpp

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
constexpr double kCsy = 0.95257934441568037152;
constexpr double kFxc = 0.92582009977255146156;
constexpr double kFyc = 3.40168025708304504493;
constexpr double kC23 = 0.66666666666666666666;
constexpr double kC13 = 0.33333333333333333333;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(MbtfppTest, MetadataAndForcesSphericalMode) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=mbtfpp +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("mbtfpp", info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", info.description);
  EXPECT_STREQ("McBride-Thomas Flat-Polar Parabolic\n\tCyl, Sph", pj->descr);
  EXPECT_EQ(1, info.has_inverse);
}

TEST(MbtfppTest, ForwardFormulaAndFlatPoleGeometryOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=mbtfpp +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Equator (phi = 0): x = FXC * lam, y = 0.
  constexpr double kLon = DegToRad(45.0);
  const PJ_COORD eq =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLon, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kFxc * kLon, eq.xy.x, 1e-12);
  EXPECT_NEAR(0.0, eq.xy.y, 1e-12);

  // 2. General latitude (phi = 30 deg) matches closed-form equations.
  constexpr double kLat = DegToRad(30.0);
  const double theta = std::asin(kCsy * std::sin(kLat));
  const double expected_x = kFxc * kLon * (2.0 * std::cos(kC23 * theta) - 1.0);
  const double expected_y = kFyc * std::sin(kC13 * theta);

  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLon, kLat, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(expected_x, fwd.xy.x, 1e-12);
  EXPECT_NEAR(expected_y, fwd.xy.y, 1e-12);

  // 3. Flat poles: at phi = +-pi/2 with lam != 0, x is non-zero (1/3 of
  //    equator length).
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kLon, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR((kFxc * kLon) / 3.0, north_pole.xy.x, 1e-12);
  EXPECT_GT(north_pole.xy.y, 1.38);
}

TEST(MbtfppTest, ForwardAndInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=mbtfpp +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 20.0, .lat_deg = 15.0},
      {.lon_deg = -65.0, .lat_deg = 45.0},
      {.lon_deg = 135.0, .lat_deg = -60.0},
      {.lon_deg = -170.0, .lat_deg = -85.0},
      {.lon_deg = 45.0, .lat_deg = 90.0},
      {.lon_deg = -45.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);

    // 1. Forward projection.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));

    // 2. Inverse projection.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-11);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-11);
  }
}

TEST(MbtfppTest, InverseClampingAndOutOfDomainBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=mbtfpp +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. First domain check: |xy.y / FYC| > ONEEPS (1.0000001) fails immediately
  //    with PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  const PJ_COORD far_out =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 1.1 * kFyc, 0.0, 0.0));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  EXPECT_EQ(HUGE_VAL, far_out.lp.lam);
  proj_errno_reset(pj.get());

  // 2. First clamping branch: 1.0 <= |xy.y / FYC| <= ONEEPS (1.0000001) clamps
  //    the intermediate angle to +-M_HALFPI before multiplying by 3.0, which
  //    then sets sin(3 * +-pi/2) / CSy = -+1 / CSy (|val| > ONEEPS) and
  //    triggers the second domain error check. Call pj->inv directly so
  //    inv_finalize does not overwrite the intermediate out.phi with HUGE_VAL.
  ASSERT_NE(nullptr, pj->inv);
  for (const double sign : {1.0, -1.0}) {
    const PJ_LP out =
        pj->inv({.x = 0.0, .y = sign * kFyc * 1.00000005}, pj.get());
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    EXPECT_NEAR(-sign / kCsy, out.phi, 1e-12);
    proj_errno_reset(pj.get());
  }

  // 3. Second clamping branch: |xy.y / FYC| < 1.0, but
  //    1.0 <= |sin(3 * asin(xy.y / FYC)) / CSy| <= ONEEPS (1.0000001) clamps
  //    the final latitude to +M_HALFPI (for y > 0) and -M_HALFPI (for y < 0).
  const double clamped_sin_3t = 1.00000005 * kCsy;
  const double y_near_pole = kFyc * std::sin(std::asin(clamped_sin_3t) / 3.0);

  const PJ_COORD north_clamped =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.2, y_near_pole, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kHalfPi, north_clamped.lp.phi, 1e-12);

  const PJ_COORD south_clamped =
      proj_trans(pj.get(), PJ_INV, proj_coord(-0.2, -y_near_pole, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kHalfPi, south_clamped.lp.phi, 1e-12);

  // 4. Second domain error check via |xy.y / FYC| < 1.0 with
  //    |sin(3 * asin(xy.y / FYC)) / CSy| > ONEEPS (e.g. y = 1.5 on R=1).
  const PJ_COORD beyond_pole =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 1.5, 0.0, 0.0));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  EXPECT_EQ(HUGE_VAL, beyond_pole.lp.lam);
  proj_errno_reset(pj.get());
}

TEST(MbtfppTest, TransArrayAndDistortionFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=mbtfpp +R=1"));
  ASSERT_NE(nullptr, pj.get());

  std::array<PJ_COORD, 2> coords = {
      proj_coord(DegToRad(30.0), DegToRad(25.0), 0.0, 0.0),
      proj_coord(DegToRad(-90.0), DegToRad(-50.0), 0.0, 0.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(30.0), coords[0].lp.lam, 1e-11);
  EXPECT_NEAR(DegToRad(25.0), coords[0].lp.phi, 1e-11);
  EXPECT_NEAR(DegToRad(-90.0), coords[1].lp.lam, 1e-11);
  EXPECT_NEAR(DegToRad(-50.0), coords[1].lp.phi, 1e-11);

  // Equal-area projection: areal_scale is 1.0 across the sphere.
  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(20.0), DegToRad(35.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-4);
}

}  // namespace
}  // namespace proj
