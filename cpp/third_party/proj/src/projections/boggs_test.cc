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
// Unit tests for the Boggs Eumorphic pseudocylindrical projection
// (proj/src/projections/boggs.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/boggs.cpp

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
constexpr double kSqrt2 = std::numbers::sqrt2;
constexpr double kFxc = 2.00276;
constexpr double kFxc2 = 1.11072;
constexpr double kFyc = 0.49931;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(BoggsTest, SetupForcesSphereAndHasNoInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=boggs +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  EXPECT_EQ(0.0, pj->es);
  EXPECT_EQ(0, pj_has_inverse(pj.get()));
  EXPECT_EQ(nullptr, pj->inv);
  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("boggs", info.id);
  EXPECT_EQ(0, info.has_inverse);

  // Calling inverse on a forward-only projection returns HUGE_VAL and sets
  // PROJ_ERR_OTHER_NO_INVERSE_OP.
  const PJ_COORD inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(1000.0, 1000.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inv.lp.lam));
  EXPECT_NE(0, proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(BoggsTest, ForwardEquatorAndPoleGuardBranchesOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=boggs +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. At the origin (0, 0), x = 0 and y = 0.
  const PJ_COORD origin =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, origin.xy.x, 1e-12);
  EXPECT_NEAR(0.0, origin.xy.y, 1e-12);

  // 2. Along the equator (phi = 0), theta = 0 so:
  //    x = FXC * lam / (1 + FXC2), y = 0.
  const double lam = DegToRad(45.0);
  const PJ_COORD eq =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kFxc * lam / (1.0 + kFxc2), eq.xy.x, 1e-12);
  EXPECT_NEAR(0.0, eq.xy.y, 1e-12);

  // 3. Exact north and south poles trigger the fabs(fabs(phi) - M_HALFPI) < EPS
  // guard branch where xy.x = 0 and theta remains +-pi/2 (unhalved before
  // computing xy.y = FYC * (phi + sqrt(2) * sin(theta))).
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(0.0, north_pole.xy.x);
  EXPECT_NEAR(kFyc * (kHalfPi + kSqrt2), north_pole.xy.y, 1e-12);

  const PJ_COORD south_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(0.0, south_pole.xy.x);
  EXPECT_NEAR(-kFyc * (kHalfPi + kSqrt2), south_pole.xy.y, 1e-12);
}

TEST(BoggsTest, ForwardMidLatitudesAndNearPoleNewtonIterationAndSymmetry) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=boggs +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Mid-latitude point exercises the Newton solver for Mollweide auxiliary
  // angle 2*theta + sin(2*theta) = pi * sin(phi), then halves theta and applies
  // the Boggs arithmetic-harmonic mean formulas.
  const double lam = DegToRad(60.0);
  const double phi = DegToRad(40.0);
  const PJ_COORD ne =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  const PJ_COORD nw =
      proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, phi, 0.0, 0.0));
  const PJ_COORD se =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, -phi, 0.0, 0.0));
  const PJ_COORD sw =
      proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, -phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));

  // Recover sin(theta) from y = FYC * (phi + sqrt(2) * sin(theta)) and verify
  // that 2*theta satisfies Mollweide's equation 2*theta + sin(2*theta) =
  // pi * sin(phi).
  const double sin_theta = (ne.xy.y / kFyc - phi) / kSqrt2;
  const double theta = std::asin(sin_theta);
  EXPECT_NEAR(kPi * std::sin(phi), 2.0 * theta + std::sin(2.0 * theta), 1e-7);
  EXPECT_NEAR(kFxc * lam / (1.0 / std::cos(phi) + kFxc2 / std::cos(theta)),
              ne.xy.x, 1e-9);

  // Verify four-quadrant symmetry.
  EXPECT_NEAR(-ne.xy.x, nw.xy.x, 1e-12);
  EXPECT_NEAR(ne.xy.y, nw.xy.y, 1e-12);
  EXPECT_NEAR(ne.xy.x, se.xy.x, 1e-12);
  EXPECT_NEAR(-ne.xy.y, se.xy.y, 1e-12);
  EXPECT_NEAR(-ne.xy.x, sw.xy.x, 1e-12);
  EXPECT_NEAR(-ne.xy.y, sw.xy.y, 1e-12);

  // 2. Latitude just outside the pole guard (pi/2 - 1e-6 >= EPS = 1e-7)
  // exercises the high-iteration Newton path near theta = pi/2.
  const PJ_LP near_pole_lp = {.lam = lam, .phi = kHalfPi - 1e-6};
  const PJ_XY near_pole_xy = pj_fwd(near_pole_lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(near_pole_xy.x, 0.0);
  EXPECT_NEAR(kFyc * (kHalfPi + kSqrt2), near_pole_xy.y, 1e-3);

  // 3. Verify batch forward transform and equal-area scale factor (s == 1).
  PJ_COORD batch[2] = {
      proj_coord(DegToRad(15.0), DegToRad(25.0), 0.0, 0.0),
      proj_coord(DegToRad(-30.0), DegToRad(-50.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, batch));
  EXPECT_GT(batch[0].xy.x, 0.0);
  EXPECT_GT(batch[0].xy.y, 0.0);
  EXPECT_LT(batch[1].xy.x, 0.0);
  EXPECT_LT(batch[1].xy.y, 0.0);

  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-4);
}

}  // namespace
}  // namespace proj
