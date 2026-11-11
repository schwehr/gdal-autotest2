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
// Unit tests for the Loximuthal projection (proj/src/projections/loxim.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/loxim.cpp

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
constexpr double kQuarterPi = kPi / 4.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(LoximTest, InvalidLat1AtOrBeyondPolesFailsCreation) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_log(ctx.get());

  constexpr const char* kInvalidDefs[] = {
      "+proj=loxim +lat_1=90",
      "+proj=loxim +lat_1=-90",
      "+proj=loxim +lat_1=95",
      "+proj=loxim +lat_1=-100",
  };

  for (const char* def : kInvalidDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
    proj_context_errno_set(ctx.get(), 0);
  }
}

TEST(LoximTest, StandardParallelSmallDeltaYBranchForwardAndInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Use unit sphere (+R=1) to verify exact analytical values when
  // |lp.phi - phi1| < 1e-8 (the `fabs(xy.y) < EPS` branch in both fwd and inv).
  ScopedPj pj(proj_create(ctx.get(), "+proj=loxim +lat_1=30 +R=1"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0.0, pj->es);

  const double phi1 = DegToRad(30.0);
  const double lam = DegToRad(45.0);

  // 1. Exact standard parallel (xy.y == 0).
  const PJ_XY fwd_exact = pj_fwd({.lam = lam, .phi = phi1}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam * std::cos(phi1), fwd_exact.x, 1e-12);
  EXPECT_NEAR(0.0, fwd_exact.y, 1e-12);

  const PJ_LP inv_exact = pj_inv(fwd_exact, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, inv_exact.lam, 1e-12);
  EXPECT_NEAR(phi1, inv_exact.phi, 1e-12);

  // 2. Slightly perturbed within EPS (1e-8) still takes the fast linear branch.
  const PJ_XY fwd_eps = pj_fwd({.lam = -lam, .phi = phi1 + 1e-10}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-lam * std::cos(phi1), fwd_eps.x, 1e-12);
  EXPECT_NEAR(1e-10, fwd_eps.y, 1e-15);

  const PJ_LP inv_eps = pj_inv(fwd_eps, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-lam, inv_eps.lam, 1e-12);
  EXPECT_NEAR(phi1 + 1e-10, inv_eps.phi, 1e-12);
}

TEST(LoximTest, GeneralForwardAndInverseRoundTripAllQuadrants) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=loxim +lat_1=20 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const double phi1 = DegToRad(20.0);
  const double tanphi1 = std::tan(kQuarterPi + 0.5 * phi1);

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 40.0, .lat_deg = 55.0},
      {.lon_deg = -60.0, .lat_deg = 50.0},
      {.lon_deg = 75.0, .lat_deg = -35.0},
      {.lon_deg = -110.0, .lat_deg = -45.0},
      {.lon_deg = 0.0, .lat_deg = 0.0},
  };

  for (const auto& tc : kCases) {
    const PJ_LP in = {.lam = DegToRad(tc.lon_deg), .phi = DegToRad(tc.lat_deg)};
    const PJ_XY xy = pj_fwd(in, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));

    const double expected_y = in.phi - phi1;
    const double expected_x =
        in.lam * expected_y /
        std::log(std::tan(kQuarterPi + 0.5 * in.phi) / tanphi1);
    EXPECT_NEAR(expected_x, xy.x, 1e-12);
    EXPECT_NEAR(expected_y, xy.y, 1e-12);

    const PJ_LP out = pj_inv(xy, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lam, out.lam, 1e-12);
    EXPECT_NEAR(in.phi, out.phi, 1e-12);
  }
}

TEST(LoximTest, NorthAndSouthPoleSingularityBranchesForwardAndInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=loxim +lat_1=15 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const double phi1 = DegToRad(15.0);

  // 1. South pole (lp.phi = -pi/2) makes M_FORTPI + 0.5 * lp.phi == 0,
  // hitting `fabs(xy.x) < EPS` in loxim_s_forward and `fabs(lp.lam) < EPS` in
  // loxim_s_inverse.
  const PJ_XY south_pole_xy =
      pj_fwd({.lam = DegToRad(50.0), .phi = -kHalfPi}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, south_pole_xy.x, 1e-12);
  EXPECT_NEAR(-kHalfPi - phi1, south_pole_xy.y, 1e-12);

  const PJ_LP south_pole_lp =
      pj_inv({.x = 1.23, .y = -kHalfPi - phi1}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, south_pole_lp.lam, 1e-12);
  EXPECT_NEAR(-kHalfPi, south_pole_lp.phi, 1e-12);

  // 2. North pole (lp.phi = +pi/2) makes M_FORTPI + 0.5 * lp.phi == +pi/2,
  // hitting `fabs(fabs(...) - M_HALFPI) < EPS` in both forward and inverse.
  const PJ_XY north_pole_xy =
      pj_fwd({.lam = DegToRad(-50.0), .phi = kHalfPi}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, north_pole_xy.x, 1e-12);
  EXPECT_NEAR(kHalfPi - phi1, north_pole_xy.y, 1e-12);

  const PJ_LP north_pole_lp =
      pj_inv({.x = -1.23, .y = kHalfPi - phi1}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, north_pole_lp.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, north_pole_lp.phi, 1e-12);
}

TEST(LoximTest, TransArrayAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=loxim +lat_1=40 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(25.0), DegToRad(50.0), 0.0, 0.0),
      proj_coord(DegToRad(-30.0), DegToRad(10.0), 0.0, 0.0),
  };
  const PJ_COORD orig0 = coords[0];
  const PJ_COORD orig1 = coords[1];

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(orig0.lp.lam, coords[0].lp.lam, 1e-12);
  EXPECT_NEAR(orig0.lp.phi, coords[0].lp.phi, 1e-12);
  EXPECT_NEAR(orig1.lp.lam, coords[1].lp.lam, 1e-12);
  EXPECT_NEAR(orig1.lp.phi, coords[1].lp.phi, 1e-12);

  // Off the central meridian, dy/dphi = 1 while dx/dphi != 0, so
  // meridional_scale = hypot(dx/dphi, dy/dphi) > 1.0.
  const PJ_FACTORS factors = proj_factors(pj.get(), orig0);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.dy_dphi, 1e-6);
  EXPECT_NEAR(std::hypot(factors.dx_dphi, 1.0), factors.meridional_scale, 1e-6);
  EXPECT_GT(factors.meridional_scale, 1.0);
  EXPECT_GT(factors.parallel_scale, 0.0);

  // Along the central meridian (lam = 0), dx/dphi = 0, so meridional_scale is
  // 1.0; along the standard parallel (phi = lat_1 = 40 deg), parallel_scale is
  // 1.0.
  const PJ_FACTORS cm_factors =
      proj_factors(pj.get(), proj_coord(0.0, DegToRad(40.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, cm_factors.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, cm_factors.parallel_scale, 1e-6);
}

}  // namespace
}  // namespace proj
