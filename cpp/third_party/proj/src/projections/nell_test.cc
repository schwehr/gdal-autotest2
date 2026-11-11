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
// Unit tests for the Nell pseudocylindrical projection
// (proj/src/projections/nell.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/nell.cpp

#include <cmath>
#include <limits>
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

TEST(NellTest, SetupForcesSphereAndRegistersForwardAndInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nell +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  EXPECT_EQ(0.0, pj->es);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("nell", info.id);
}

TEST(NellTest, ForwardEquatorPrimeMeridianAndSymmetryOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nell +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. At the origin (0, 0), theta = 0 and x = 0, y = 0.
  const PJ_COORD origin =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, origin.xy.x, 1e-12);
  EXPECT_NEAR(0.0, origin.xy.y, 1e-12);

  // 2. Along the equator (phi = 0), theta = 0 so x = 0.5 * lam * (1 + 1) = lam.
  const double lam = DegToRad(40.0);
  const PJ_COORD eq =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, eq.xy.x, 1e-12);
  EXPECT_NEAR(0.0, eq.xy.y, 1e-12);

  // 3. Verify quadrant symmetry and intermediate latitude Newton convergence
  // where y = theta satisfies theta + sin(theta) = 2 * sin(phi) and
  // x = 0.5 * lam * (1 + cos(theta)).
  const double phi = DegToRad(35.0);
  const PJ_COORD ne =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  const PJ_COORD nw =
      proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, phi, 0.0, 0.0));
  const PJ_COORD se =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, -phi, 0.0, 0.0));
  const PJ_COORD sw =
      proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, -phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));

  const double theta = ne.xy.y;
  EXPECT_NEAR(2.0 * std::sin(phi), theta + std::sin(theta), 1e-7);
  EXPECT_NEAR(0.5 * lam * (1.0 + std::cos(theta)), ne.xy.x, 1e-12);

  EXPECT_NEAR(-ne.xy.x, nw.xy.x, 1e-12);
  EXPECT_NEAR(ne.xy.y, nw.xy.y, 1e-12);
  EXPECT_NEAR(ne.xy.x, se.xy.x, 1e-12);
  EXPECT_NEAR(-ne.xy.y, se.xy.y, 1e-12);
  EXPECT_NEAR(-ne.xy.x, sw.xy.x, 1e-12);
  EXPECT_NEAR(-ne.xy.y, sw.xy.y, 1e-12);
}

TEST(NellTest, ForwardPolesExhaustMaxIterationsCleanly) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nell +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // At phi = +-pi/2, y = theta satisfies theta + sin(theta) = 2 * sin(phi) =
  // +-2 (theta ≈ +-1.1060601577062719 rad) and x = 0.5 * lam * (1 +
  // cos(theta)).
  const double lam = DegToRad(30.0);
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.1060601577062719, north_pole.xy.y, 1e-7);
  EXPECT_NEAR(2.0, north_pole.xy.y + std::sin(north_pole.xy.y), 1e-7);
  EXPECT_NEAR(0.5 * lam * (1.0 + std::cos(north_pole.xy.y)), north_pole.xy.x,
              1e-12);

  const PJ_COORD south_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-north_pole.xy.x, south_pole.xy.x, 1e-12);
  EXPECT_NEAR(-north_pole.xy.y, south_pole.xy.y, 1e-12);

  // Calling pj->fwd directly with NaN bypasses fwd_prepare and runs all
  // MAX_ITER = 10 iterations of the Newton loop before exiting via i == 0.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const PJ_XY nan_xy = pj->fwd({.lam = 0.0, .phi = nan}, pj.get());
  EXPECT_TRUE(std::isnan(nan_xy.x));
  EXPECT_TRUE(std::isnan(nan_xy.y));
}

TEST(NellTest, ForwardAndInverseRoundTripAcrossLatitudes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nell +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 15.0, .lat_deg = 20.0},
      {.lon_deg = -75.0, .lat_deg = 45.0},
      {.lon_deg = 120.0, .lat_deg = -60.0},
      {.lon_deg = -170.0, .lat_deg = -80.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);

    // 1. Forward projection.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(fwd.xy.x));

    // 2. Inverse projection.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-7);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-7);
  }
}

TEST(NellTest, InverseOutOfDomainYTriggersProjectionDomainError) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nell +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // For y = 4.0 on the unit sphere, 0.5 * (y + sin(y)) > 1.0, causing aasin()
  // to set PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  const PJ_COORD inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.5, 4.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inv.lp.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(NellTest, Classic2DEntryPointsBatchTransformAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nell +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP lp = {.lam = DegToRad(25.0), .phi = DegToRad(40.0)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, inv_lp.lam, 1e-7);
  EXPECT_NEAR(lp.phi, inv_lp.phi, 1e-7);

  PJ_COORD batch[2] = {
      proj_coord(DegToRad(10.0), DegToRad(15.0), 0.0, 0.0),
      proj_coord(DegToRad(-40.0), DegToRad(-30.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, batch));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, batch));
  EXPECT_NEAR(DegToRad(10.0), batch[0].lp.lam, 1e-7);
  EXPECT_NEAR(DegToRad(15.0), batch[0].lp.phi, 1e-7);
  EXPECT_NEAR(DegToRad(-40.0), batch[1].lp.lam, 1e-7);
  EXPECT_NEAR(DegToRad(-30.0), batch[1].lp.phi, 1e-7);

  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-6);
  EXPECT_GT(factors.meridional_scale, 0.0);
}

}  // namespace
}  // namespace proj
