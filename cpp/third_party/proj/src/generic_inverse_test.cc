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
// Unit tests for numerical 2D Newton-Raphson inverse solver
// (proj/src/generic_inverse.cpp) and inverse-using projections.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/generic_inverse.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;
constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(GenericInverseTest, ImmediateReturnSignQuadrantsAndStepClamping) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj eqc(proj_create(ctx.get(), "+proj=eqc +R=1"));
  ASSERT_NE(nullptr, eqc.get());

  // 1. Exact initial guess returns immediately at iteration 0.
  const PJ_LP exact = pj_generic_inverse_2d({.x = 0.25, .y = -0.35}, eqc.get(),
                                            {.lam = 0.25, .phi = -0.35}, 1e-12);
  EXPECT_EQ(0, proj_errno(eqc.get()));
  EXPECT_DOUBLE_EQ(0.25, exact.lam);
  EXPECT_DOUBLE_EQ(-0.35, exact.phi);

  // 2. Converge across all initial-guess sign quadrants (<= 0 vs > 0 for both
  // lam and phi) and both +0.3 / -0.3 Newton step clamps.
  struct QuadrantCase {
    PJ_LP initial;
    PJ_XY target;
  };
  constexpr QuadrantCase kCases[] = {
      {.initial = {.lam = 0.0, .phi = 0.0}, .target = {.x = 1.0, .y = 1.0}},
      {.initial = {.lam = 0.5, .phi = 0.0}, .target = {.x = 0.5, .y = 0.5}},
      {.initial = {.lam = 0.0, .phi = 0.0}, .target = {.x = -1.0, .y = -1.0}},
      {.initial = {.lam = 0.2, .phi = 0.2}, .target = {.x = -0.9, .y = -0.7}},
      {.initial = {.lam = -0.2, .phi = 0.2}, .target = {.x = 0.8, .y = -0.6}},
      {.initial = {.lam = 0.2, .phi = -0.2}, .target = {.x = -0.8, .y = 0.6}},
  };
  for (const auto& tc : kCases) {
    const PJ_LP res =
        pj_generic_inverse_2d(tc.target, eqc.get(), tc.initial, 1e-12);
    EXPECT_EQ(0, proj_errno(eqc.get()));
    EXPECT_NEAR(tc.target.x, res.lam, 1e-12);
    EXPECT_NEAR(tc.target.y, res.phi, 1e-12);
  }
}

TEST(GenericInverseTest,
     SkipsJacobianRecomputationCloseToSolutionAndHandlesZeroDet) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=eqc +R=1"));
  ASSERT_NE(nullptr, pj.get());

  auto orig_fwd = pj->fwd;
  absl::Cleanup restore_fwd = [&] { pj->fwd = orig_fwd; };

  // 1. Weakly quadratic fwd(lam, phi) = (lam + 0.05 * lam^2, phi) from (0, 0)
  // to target (1.05, 0.5) [exact root (1.0, 0.5)]:
  // - iters 0..2 take -0.3 clamped steps,
  // - iter 4 shrinks residual from ~5e-4 (> 1e-6) to ~1e-8 (<= 1e-6),
  // - iter 5 enters the i > 0 residual <= 1e-6 branch skipping Jacobian
  //   recomputation while residual exceeds 1e-12,
  // - iter 6 converges within 1e-12.
  pj->fwd = [](PJ_LP lp, PJ*) -> PJ_XY {
    return {.x = lp.lam + 0.05 * lp.lam * lp.lam, .y = lp.phi};
  };
  const PJ_LP nonlinear = pj_generic_inverse_2d(
      {.x = 1.05, .y = 0.5}, pj.get(), {.lam = 0.0, .phi = 0.0}, 1e-12);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, nonlinear.lam, 1e-12);
  EXPECT_NEAR(0.5, nonlinear.phi, 1e-12);

  // 2. Rank-deficient fwd(lam, phi) = (lam, 0) has det == 0; solver exhausts
  // all 15 iterations without updating deriv_* and reports domain error.
  pj->fwd = [](PJ_LP lp, PJ*) -> PJ_XY { return {.x = lp.lam, .y = 0.0}; };
  const PJ_LP singular = pj_generic_inverse_2d({.x = 0.5, .y = 0.5}, pj.get(),
                                               {.lam = 0.0, .phi = 0.0}, 1e-10);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, singular.lam);
  EXPECT_DOUBLE_EQ(0.0, singular.phi);
  proj_errno_reset(pj.get());
}

TEST(GenericInverseTest, ClampsDomainBoundsAndSetsErrnoOnNonConvergence) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj eqc(proj_create(ctx.get(), "+proj=eqc +R=1"));
  ASSERT_NE(nullptr, eqc.get());

  struct BoundCase {
    PJ_XY target;
    PJ_LP initial;
    double expected_lam;
    double expected_phi;
  };
  constexpr BoundCase kBoundCases[] = {
      {.target = {.x = -kPi - 0.5, .y = 0.0},
       .initial = {.lam = -kPi + 0.1, .phi = 0.0},
       .expected_lam = -kPi,
       .expected_phi = 0.0},
      {.target = {.x = kPi + 0.5, .y = 0.0},
       .initial = {.lam = kPi - 0.1, .phi = 0.0},
       .expected_lam = kPi,
       .expected_phi = 0.0},
      {.target = {.x = 0.0, .y = -kHalfPi - 0.5},
       .initial = {.lam = 0.0, .phi = -kHalfPi + 0.1},
       .expected_lam = 0.0,
       .expected_phi = -kHalfPi},
      {.target = {.x = 0.0, .y = kHalfPi + 0.5},
       .initial = {.lam = 0.0, .phi = kHalfPi - 0.1},
       .expected_lam = 0.0,
       .expected_phi = kHalfPi},
  };

  for (const auto& tc : kBoundCases) {
    const PJ_LP res =
        pj_generic_inverse_2d(tc.target, eqc.get(), tc.initial, 1e-12);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(eqc.get()));
    EXPECT_NEAR(tc.expected_lam, res.lam, 1e-12);
    EXPECT_NEAR(tc.expected_phi, res.phi, 1e-12);
    proj_errno_reset(eqc.get());
  }
}

TEST(GenericInverseTest, RealProjectionInversesRoundTripCleanly) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr const char* kDefs[] = {
      "+proj=wink2 +R=1 +lat_1=50d28",     "+proj=adams_ws2 +R=1",
      "+proj=peirce_q +shape=square +R=1", "+proj=peirce_q +shape=diamond +R=1",
      "+proj=cass +ellps=WGS84",
  };
  constexpr PJ_LP kTestCoords[] = {
      {.lam = DegToRad(15.0), .phi = DegToRad(25.0)},
      {.lam = DegToRad(-40.0), .phi = DegToRad(-18.0)},
  };

  for (const char* def : kDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    for (const PJ_LP& lp : kTestCoords) {
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lp.lam, lp.phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      EXPECT_NEAR(lp.lam, inv.lp.lam, 1e-8) << def;
      EXPECT_NEAR(lp.phi, inv.lp.phi, 1e-8) << def;
    }
  }
}

}  // namespace
}  // namespace proj
