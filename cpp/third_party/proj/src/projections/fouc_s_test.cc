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
// Unit tests for the Foucaut Sinusoidal projection
// (proj/src/projections/fouc_s.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/fouc_s.cpp

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

TEST(FoucSTest, InvalidNOutsideUnitIntervalFailsCreation) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_log(ctx.get());

  // Both n < 0 and n > 1 must be rejected with
  // PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE.
  constexpr const char* kInvalidDefs[] = {
      "+proj=fouc_s +n=-0.01",
      "+proj=fouc_s +n=-2.0",
      "+proj=fouc_s +n=1.01",
      "+proj=fouc_s +n=5.0",
  };

  for (const char* def : kInvalidDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
    proj_context_errno_set(ctx.get(), 0);
  }
}

TEST(FoucSTest, ForwardAndInverseWithNonZeroNRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=fouc_s +n=0.5 +R=1"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0.0, pj->es);

  constexpr double kN = 0.5;
  constexpr double kN1 = 0.5;

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 30.0, .lat_deg = 45.0},
      {.lon_deg = -60.0, .lat_deg = 35.0},
      {.lon_deg = 90.0, .lat_deg = -50.0},
      {.lon_deg = -120.0, .lat_deg = -70.0},
  };

  for (const auto& tc : kCases) {
    const PJ_LP in = {.lam = DegToRad(tc.lon_deg), .phi = DegToRad(tc.lat_deg)};
    const PJ_XY xy = pj_fwd(in, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));

    const double t = std::cos(in.phi);
    const double expected_x = in.lam * t / (kN + kN1 * t);
    const double expected_y = kN * in.phi + kN1 * std::sin(in.phi);
    EXPECT_NEAR(expected_x, xy.x, 1e-12);
    EXPECT_NEAR(expected_y, xy.y, 1e-12);

    const PJ_LP out = pj_inv(xy, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lam, out.lam, 1e-7);
    EXPECT_NEAR(in.phi, out.phi, 1e-7);
  }
}

TEST(FoucSTest, ZeroNBranchUsesDirectArcsinInInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // When n == 0 (either omitted or +n=0), fouc_s reduces to the cylindrical
  // equal-area / Lambert projection (x = lam, y = sin(phi)) and
  // fouc_s_s_inverse takes the `else lp.phi = aasin(P->ctx, xy.y);` branch.
  ScopedPj pj(proj_create(ctx.get(), "+proj=fouc_s +n=0 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP in = {.lam = DegToRad(40.0), .phi = DegToRad(-30.0)};
  const PJ_XY xy = pj_fwd(in, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.lam, xy.x, 1e-12);
  EXPECT_NEAR(std::sin(in.phi), xy.y, 1e-12);

  const PJ_LP out = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.lam, out.lam, 1e-12);
  EXPECT_NEAR(in.phi, out.phi, 1e-12);
}

TEST(FoucSTest, InverseMaxIterFallbackClampsToPositiveAndNegativeHalfPi) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=fouc_s +n=0.8 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // With n = 0.8 (n1 = 0.2), the derivative 0.8 + 0.2 * cos(phi) is in
  // [0.6, 1.0]. For an extreme |xy.y| = 1e20, Newton-Raphson shrinks the
  // residual by a bounded factor per step and cannot reach LOOP_TOL (1e-7)
  // within MAX_ITER (10) steps, exercising `if (!i) lp.phi = xy.y < 0. ?
  // -M_HALFPI : M_HALFPI;` for both positive and negative xy.y.
  const PJ_LP pos_clamped = pj_inv({.x = 0.0, .y = 1e20}, pj.get());
  EXPECT_NEAR(kHalfPi, pos_clamped.phi, 1e-12);
  EXPECT_NEAR(0.0, pos_clamped.lam, 1e-12);

  const PJ_LP neg_clamped = pj_inv({.x = 0.0, .y = -1e20}, pj.get());
  EXPECT_NEAR(-kHalfPi, neg_clamped.phi, 1e-12);
  EXPECT_NEAR(0.0, neg_clamped.lam, 1e-12);
}

TEST(FoucSTest, EqualAreaFactorsAndTransArrayWithNOne) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // At the upper boundary n = 1, Foucaut Sinusoidal reduces to the classic
  // Sinusoidal projection (x = lam * cos(phi), y = phi). It is equal-area for
  // any n in [0, 1] (areal_scale == 1).
  ScopedPj pj(proj_create(ctx.get(), "+proj=fouc_s +n=1 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(25.0), DegToRad(40.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-6);

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(15.0), DegToRad(25.0), 0.0, 0.0),
      proj_coord(DegToRad(-45.0), DegToRad(-35.0), 0.0, 0.0),
  };
  const PJ_COORD orig0 = coords[0];
  const PJ_COORD orig1 = coords[1];

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(orig0.lp.lam, coords[0].lp.lam, 1e-7);
  EXPECT_NEAR(orig0.lp.phi, coords[0].lp.phi, 1e-7);
  EXPECT_NEAR(orig1.lp.lam, coords[1].lp.lam, 1e-7);
  EXPECT_NEAR(orig1.lp.phi, coords[1].lp.phi, 1e-7);
}

}  // namespace
}  // namespace proj
