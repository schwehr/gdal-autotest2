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
// Unit tests for Lambert Conformal Conic (lcc) projection
// (proj/src/projections/lcc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/lcc.cpp

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

TEST(LccTest, ParameterValidationRejectsSymmetricPolarAndDegenerateParallels) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  constexpr const char* kInvalidDefs[] = {
      // Default lat_1 = 0, lat_2 = 0 -> |lat_1 + lat_2| < EPS10.
      "+proj=lcc +ellps=WGS84",
      // Symmetric standard parallels lat_1 = -lat_2 -> |lat_1 + lat_2| == 0.
      "+proj=lcc +lat_1=30 +lat_2=-30 +ellps=WGS84",
      // |lat_1| >= 90 deg.
      "+proj=lcc +lat_1=90 +lat_2=45 +ellps=WGS84",
      "+proj=lcc +lat_1=-90 +lat_2=-45 +ellps=WGS84",
      // |lat_2| >= 90 deg.
      "+proj=lcc +lat_1=45 +lat_2=90 +ellps=WGS84",
      "+proj=lcc +lat_1=-45 +lat_2=-90 +ellps=WGS84",
      // Extreme eccentricity on secant ellipsoid makes m1 == m2 (Q->n == 0).
      "+proj=lcc +lat_1=30 +lat_2=60 +a=1 +es=0.99999999999999999",
      // Spherical secant with tiny lat_2 rounds cos(phi2) to 1 (Q->n == 0).
      "+proj=lcc +a=1 +lat_2=0.0000001",
  };

  for (const char* def : kInvalidDefs) {
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj pj(proj_create(ctx.get(), def));
    EXPECT_EQ(nullptr, pj.get()) << def;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << def;
  }
  EXPECT_TRUE(capture.ContainsMessage("Invalid value for"));
}

TEST(LccTest, EllipsoidalAndSpherical1SPAnd2SPForwardInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr const char* kDefs[] = {
      // 2SP Northern hemisphere ellipsoidal.
      "+proj=lcc +lat_1=33 +lat_2=45 +lat_0=39 +lon_0=-96 +ellps=WGS84",
      // 1SP Northern hemisphere ellipsoidal (lat_2 and lat_0 omitted ->
      // phi2 = phi0 = phi1).
      "+proj=lcc +lat_1=42 +k_0=0.9996 +ellps=WGS84",
      // 1SP with explicit lat_0 != lat_1.
      "+proj=lcc +lat_1=42 +lat_0=40 +ellps=WGS84",
      // 2SP Southern hemisphere ellipsoidal (exercises Q->n < 0 in inverse).
      "+proj=lcc +lat_1=-18 +lat_2=-36 +lat_0=-27 +lon_0=135 +ellps=WGS84",
      // 2SP Northern hemisphere spherical.
      "+proj=lcc +lat_1=33 +lat_2=45 +lat_0=39 +R=6371000",
      // 1SP Southern hemisphere spherical.
      "+proj=lcc +lat_1=-35 +R=6371000",
      // Polar origin lat_0=90 on ellipsoid and sphere (exercises rho0 = 0).
      "+proj=lcc +lat_1=33 +lat_2=45 +lat_0=90 +ellps=WGS84",
      "+proj=lcc +lat_1=33 +lat_2=45 +lat_0=90 +R=6371000",
  };

  for (const char* def : kDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    EXPECT_EQ(1, pj_has_inverse(pj.get()));

    const PJ_COORD in = proj_coord(DegToRad(10.0), DegToRad(35.0), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    EXPECT_FALSE(std::isinf(fwd.xy.x)) << def;
    EXPECT_FALSE(std::isinf(fwd.xy.y)) << def;

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-10) << def;
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-10) << def;
  }
}

TEST(LccTest, ApexPoleAndOppositePoleAndInversePhi2FailureBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppressor(ctx.get());

  // 1. Northern cone (Q->n > 0): North Pole projects to apex (rho == 0) and
  // inverse at apex returns (0, +pi/2); South Pole is outside domain.
  ScopedPj pj_north(
      proj_create(ctx.get(), "+proj=lcc +lat_1=33 +lat_2=45 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_north.get());

  const PJ_COORD np_fwd =
      proj_trans(pj_north.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_north.get()));
  const PJ_COORD np_inv = proj_trans(pj_north.get(), PJ_INV, np_fwd);
  EXPECT_EQ(0, proj_errno(pj_north.get()));
  EXPECT_NEAR(0.0, np_inv.lp.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, np_inv.lp.phi, 1e-12);

  const PJ_COORD sp_fwd =
      proj_trans(pj_north.get(), PJ_FWD, proj_coord(0.0, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, sp_fwd.xy.x);
  EXPECT_EQ(HUGE_VAL, sp_fwd.xy.y);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_north.get()));
  proj_errno_reset(pj_north.get());

  // 1b. Exact apex inverse (rho == 0) when +lat_0=90 / +lat_0=-90 makes
  // Q->rho0 == 0.0, covering lines 72-73 for both Q->n > 0 and Q->n < 0.
  ScopedPj pj_north_apex(proj_create(
      ctx.get(), "+proj=lcc +lat_1=33 +lat_2=45 +lat_0=90 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_north_apex.get());
  const PJ_LP north_apex_inv =
      pj_inv({.x = 0.0, .y = 0.0}, pj_north_apex.get());
  EXPECT_EQ(0, proj_errno(pj_north_apex.get()));
  EXPECT_DOUBLE_EQ(0.0, north_apex_inv.lam);
  EXPECT_NEAR(kHalfPi, north_apex_inv.phi, 1e-12);

  ScopedPj pj_south_apex(proj_create(
      ctx.get(), "+proj=lcc +lat_1=-33 +lat_2=-45 +lat_0=-90 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_south_apex.get());
  const PJ_LP south_apex_inv =
      pj_inv({.x = 0.0, .y = 0.0}, pj_south_apex.get());
  EXPECT_EQ(0, proj_errno(pj_south_apex.get()));
  EXPECT_DOUBLE_EQ(0.0, south_apex_inv.lam);
  EXPECT_NEAR(-kHalfPi, south_apex_inv.phi, 1e-12);

  // 2. Southern cone (Q->n < 0): South Pole projects to apex (rho == 0) and
  // inverse at apex returns (0, -pi/2); North Pole is outside domain.
  ScopedPj pj_south(
      proj_create(ctx.get(), "+proj=lcc +lat_1=-33 +lat_2=-45 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_south.get());

  const PJ_COORD sp_ok =
      proj_trans(pj_south.get(), PJ_FWD, proj_coord(0.0, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_south.get()));
  const PJ_COORD sp_inv = proj_trans(pj_south.get(), PJ_INV, sp_ok);
  EXPECT_EQ(0, proj_errno(pj_south.get()));
  EXPECT_NEAR(0.0, sp_inv.lp.lam, 1e-12);
  EXPECT_NEAR(-kHalfPi, sp_inv.lp.phi, 1e-12);

  // 3. NaN input in ellipsoidal lcc_e_inverse propagates NaN through pj_phi2
  // without setting errno (since pj_phi2 returns atan(...) which is NaN, not
  // HUGE_VAL), while HUGE_VAL input is rejected with
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  ScopedPj pj_high_e(
      proj_create(ctx.get(), "+proj=lcc +lat_1=45 +a=1 +es=0.9999"));
  ASSERT_NE(nullptr, pj_high_e.get());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const PJ_LP nan_inv = pj_inv({.x = nan, .y = 1.0}, pj_high_e.get());
  EXPECT_TRUE(std::isnan(nan_inv.lam));
  EXPECT_TRUE(std::isnan(nan_inv.phi));
  EXPECT_EQ(0, proj_errno(pj_high_e.get()));

  const PJ_LP huge_inv = pj_inv({.x = HUGE_VAL, .y = 1.0}, pj_high_e.get());
  EXPECT_EQ(HUGE_VAL, huge_inv.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_high_e.get()));
  proj_errno_reset(pj_high_e.get());
}

}  // namespace
}  // namespace proj
