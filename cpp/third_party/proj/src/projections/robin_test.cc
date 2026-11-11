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
// Unit tests for the Robinson pseudocylindrical projection
// (proj/src/projections/robin.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/robin.cpp

#include <cmath>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

C_NAMESPACE PJ* pj_robin(PJ*);

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;
constexpr double kFxc = 0.8487;
constexpr double kFyc = 1.3523;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(RobinTest, ForwardAndInverseRoundTripAcrossAllTableIntervals) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  // Robinson is spherical; passing an ellipsoid forces P->es = 0.
  ScopedPj pj(proj_create(ctx.get(), "+proj=robin +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0.0, pj->es);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_STREQ("robin", proj_pj_info(pj.get()).id);

  // Origin (0, 0) maps to (0, 0).
  const PJ_COORD origin_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, origin_fwd.xy.x, 1e-6);
  EXPECT_NEAR(0.0, origin_fwd.xy.y, 1e-6);

  // Step through all 5-degree table intervals in both hemispheres (-88 to +88
  // deg), including latitudes like 4.5 deg (normalized y ~ 0.056) where the
  // initial bucket index `i = lround(floor(lp.phi * 18))` overshoots Y[i].c0
  // (0.062) and triggers `--i` in the table-interval search loop.
  for (int lat_deg = -88; lat_deg <= 88; lat_deg += 4) {
    for (const double lon_deg : {-150.0, -45.0, 0.0, 60.0, 175.0}) {
      const PJ_COORD in =
          proj_coord(DegToRad(lon_deg), DegToRad(lat_deg), 0.0, 0.0);
      const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj.get()));
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-6);
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-6);
    }
  }
}

TEST(RobinTest, PolesAndNearPolePathologicInverseBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=robin +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Forward at exact North and South poles (i == NODES == 18).
  const PJ_LP north_pole = {.lam = DegToRad(40.0), .phi = kHalfPi};
  const PJ_XY fwd_north = pj_fwd(north_pole, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kFyc, fwd_north.y, 1e-6);

  const PJ_LP south_pole = {.lam = DegToRad(-40.0), .phi = -kHalfPi};
  const PJ_XY fwd_south = pj_fwd(south_pole, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kFyc, fwd_south.y, 1e-6);

  // 2. Forward slightly past 90 deg clamps `i >= NODES` to `NODES`. Call
  // pj->fwd directly so fwd_prepare in fwd.cpp does not reject |phi| > pi/2.
  const PJ_XY fwd_over = pj->fwd({.lam = 0.2, .phi = kHalfPi + 0.05}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(fwd_over.y, kFyc);

  // 3. Inverse in the `1.0 <= fabs(xy.y / FYC) <= ONEEPS (1.000001)` branch
  // for both positive and negative y.
  const PJ_LP inv_north =
      pj_inv({.x = fwd_north.x, .y = kFyc * 1.0000005}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kHalfPi, inv_north.phi, 1e-12);
  EXPECT_NEAR(north_pole.lam, inv_north.lam, 1e-6);

  const PJ_LP inv_south =
      pj_inv({.x = fwd_south.x, .y = -kFyc * 1.0000005}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kHalfPi, inv_south.phi, 1e-12);
  EXPECT_NEAR(south_pole.lam, inv_south.lam, 1e-6);
}

TEST(RobinTest, ForwardAndInverseDomainErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=robin +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const double nan = std::numeric_limits<double>::quiet_NaN();

  // 1. Forward with NaN phi sets i = -1 < 0 and reports domain error.
  (void)pj_fwd({.lam = 0.0, .phi = nan}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 2. Inverse with |xy.y / FYC| > ONEEPS (1.000001) reports domain error.
  (void)pj_inv({.x = 0.0, .y = kFyc * 1.01}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 3. Inverse with NaN y enters the general branch with i = -1 < 0 and
  // reports domain error.
  (void)pj_inv({.x = 0.0, .y = nan}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 4. Inverse with |lp.lam| > M_PI sets domain error and returns HUGE_VAL.
  const PJ_LP inv_over_lon = pj_inv({.x = kFxc * 4.0, .y = 0.0}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  EXPECT_EQ(HUGE_VAL, inv_over_lon.lam);
  EXPECT_EQ(HUGE_VAL, inv_over_lon.phi);
}

TEST(RobinTest, TransArrayFactorsAndDirectConstructor) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=robin +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(15.0), DegToRad(30.0), 0.0, 0.0),
      proj_coord(DegToRad(-75.0), DegToRad(-40.0), 0.0, 0.0),
  };
  const PJ_COORD orig[2] = {coords[0], coords[1]};

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  for (int i = 0; i < 2; ++i) {
    EXPECT_NEAR(orig[i].lp.lam, coords[i].lp.lam, 1e-6);
    EXPECT_NEAR(orig[i].lp.phi, coords[i].lp.phi, 1e-6);
  }

  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kFxc, factors.parallel_scale, 1e-4);

  // Direct call to pj_robin(nullptr) exercises the OPERATION macro allocation.
  ScopedPj raw(pj_robin(nullptr));
  ASSERT_NE(nullptr, raw.get());
  EXPECT_STREQ("robin", raw->short_name);
}

}  // namespace
}  // namespace proj
