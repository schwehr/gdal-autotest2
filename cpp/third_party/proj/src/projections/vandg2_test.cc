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
// Unit tests for the van der Grinten II and III projections
// (proj/src/projections/vandg2.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/vandg2.cpp

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

TEST(Vandg2Test, CreationPropertiesAndNoInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj vandg2(proj_create(ctx.get(), "+proj=vandg2 +R=6371000"));
  ASSERT_NE(nullptr, vandg2.get());
  EXPECT_EQ(0, pj_has_inverse(vandg2.get()));
  EXPECT_EQ(nullptr, vandg2->inv);

  ScopedPj vandg3(proj_create(ctx.get(), "+proj=vandg3 +ellps=WGS84"));
  ASSERT_NE(nullptr, vandg3.get());
  EXPECT_EQ(0, pj_has_inverse(vandg3.get()));
  EXPECT_EQ(nullptr, vandg3->inv);
  // vandg3 forces spherical eccentricity es = 0 even when an ellipsoid is set.
  EXPECT_EQ(0.0, vandg3->es);

  // Attempting an inverse transform fails cleanly because no inverse exists.
  ScopedLogSuppressor suppress_log(ctx.get());
  const PJ_COORD inv =
      proj_trans(vandg2.get(), PJ_INV, proj_coord(1000.0, 2000.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inv.xyzt.x));
  EXPECT_NE(0, proj_errno(vandg2.get()));
  proj_errno_reset(vandg2.get());
}

TEST(Vandg2Test, CentralMeridianSmallLambdaBranchPositiveAndNegativePhi) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj vandg2(proj_create(ctx.get(), "+proj=vandg2 +R=1"));
  ASSERT_NE(nullptr, vandg2.get());
  ScopedPj vandg3(proj_create(ctx.get(), "+proj=vandg3 +R=1"));
  ASSERT_NE(nullptr, vandg3.get());

  // 1. Origin (lam = 0, phi = 0) takes the |lam| < 1e-10 branch with phi >= 0.
  const PJ_XY origin2 = pj_fwd({.lam = 0.0, .phi = 0.0}, vandg2.get());
  EXPECT_EQ(0, proj_errno(vandg2.get()));
  EXPECT_NEAR(0.0, origin2.x, 1e-12);
  EXPECT_NEAR(0.0, origin2.y, 1e-12);

  // 2. Central meridian (|lam| < 1e-10) with positive and negative latitudes.
  const double phi = DegToRad(45.0);
  const double bt = std::abs((2.0 / kPi) * phi);
  const double ct = std::sqrt(1.0 - bt * bt);
  const double expected_y = kPi * bt / (1.0 + ct);

  const PJ_XY north2 = pj_fwd({.lam = 1e-12, .phi = phi}, vandg2.get());
  EXPECT_EQ(0, proj_errno(vandg2.get()));
  EXPECT_NEAR(0.0, north2.x, 1e-12);
  EXPECT_NEAR(expected_y, north2.y, 1e-12);

  const PJ_XY south2 = pj_fwd({.lam = -1e-12, .phi = -phi}, vandg2.get());
  EXPECT_EQ(0, proj_errno(vandg2.get()));
  EXPECT_NEAR(0.0, south2.x, 1e-12);
  EXPECT_NEAR(-expected_y, south2.y, 1e-12);

  // 3. Verify vandg3 shares the exact same central-meridian formula.
  const PJ_XY north3 = pj_fwd({.lam = 0.0, .phi = phi}, vandg3.get());
  EXPECT_EQ(0, proj_errno(vandg3.get()));
  EXPECT_NEAR(0.0, north3.x, 1e-12);
  EXPECT_NEAR(expected_y, north3.y, 1e-12);

  const PJ_XY south3 = pj_fwd({.lam = 0.0, .phi = -phi}, vandg3.get());
  EXPECT_EQ(0, proj_errno(vandg3.get()));
  EXPECT_NEAR(0.0, south3.x, 1e-12);
  EXPECT_NEAR(-expected_y, south3.y, 1e-12);
}

TEST(Vandg2Test, ForwardVandg2AllQuadrantsAndAnalyticalValues) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=vandg2 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const double lam = DegToRad(60.0);
  const double phi = DegToRad(30.0);
  const double bt = (2.0 / kPi) * phi;
  const double ct = std::sqrt(1.0 - bt * bt);
  const double at = 0.5 * std::abs(kPi / lam - lam / kPi);
  const double x1 = (ct * std::sqrt(1.0 + at * at) - at * ct * ct) /
                    (1.0 + at * at * bt * bt);
  const double expected_x = kPi * x1;
  const double expected_y = kPi * std::sqrt(1.0 - x1 * (x1 + 2.0 * at) + 1e-10);

  struct QuadrantCase {
    double lam_sign;
    double phi_sign;
  };
  constexpr QuadrantCase kQuadrants[] = {
      {.lam_sign = 1.0, .phi_sign = 1.0},
      {.lam_sign = -1.0, .phi_sign = 1.0},
      {.lam_sign = 1.0, .phi_sign = -1.0},
      {.lam_sign = -1.0, .phi_sign = -1.0},
  };

  for (const auto& q : kQuadrants) {
    const PJ_XY xy =
        pj_fwd({.lam = q.lam_sign * lam, .phi = q.phi_sign * phi}, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(q.lam_sign * expected_x, xy.x, 1e-12);
    EXPECT_NEAR(q.phi_sign * expected_y, xy.y, 1e-12);
  }
}

TEST(Vandg2Test, ForwardVandg3AllQuadrantsAndAnalyticalValues) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=vandg3 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const double lam = DegToRad(75.0);
  const double phi = DegToRad(40.0);
  const double bt = (2.0 / kPi) * phi;
  const double ct = std::sqrt(1.0 - bt * bt);
  const double at = 0.5 * std::abs(kPi / lam - lam / kPi);
  const double x1 = bt / (1.0 + ct);
  const double expected_x = kPi * (std::sqrt(at * at + 1.0 - x1 * x1) - at);
  const double expected_y = kPi * x1;

  struct QuadrantCase {
    double lam_sign;
    double phi_sign;
  };
  constexpr QuadrantCase kQuadrants[] = {
      {.lam_sign = 1.0, .phi_sign = 1.0},
      {.lam_sign = -1.0, .phi_sign = 1.0},
      {.lam_sign = 1.0, .phi_sign = -1.0},
      {.lam_sign = -1.0, .phi_sign = -1.0},
  };

  for (const auto& q : kQuadrants) {
    const PJ_XY xy =
        pj_fwd({.lam = q.lam_sign * lam, .phi = q.phi_sign * phi}, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(q.lam_sign * expected_x, xy.x, 1e-12);
    EXPECT_NEAR(q.phi_sign * expected_y, xy.y, 1e-12);
  }
}

TEST(Vandg2Test, PoleAndBeyondPoleClampingCtNegativeBranch) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj vandg2(proj_create(ctx.get(), "+proj=vandg2 +R=1"));
  ASSERT_NE(nullptr, vandg2.get());
  ScopedPj vandg3(proj_create(ctx.get(), "+proj=vandg3 +R=1"));
  ASSERT_NE(nullptr, vandg3.get());

  // 1. Exact poles (phi = +-pi/2) give bt = 1 and ct = 0 via pj_fwd, collapsing
  // Easting to 0 and Northing to +-pi.
  const PJ_XY pole2 =
      pj_fwd({.lam = DegToRad(30.0), .phi = kHalfPi}, vandg2.get());
  EXPECT_EQ(0, proj_errno(vandg2.get()));
  EXPECT_NEAR(0.0, pole2.x, 1e-12);
  EXPECT_NEAR(kPi, pole2.y, 1e-5);

  const PJ_XY pole3 =
      pj_fwd({.lam = DegToRad(30.0), .phi = -kHalfPi}, vandg3.get());
  EXPECT_EQ(0, proj_errno(vandg3.get()));
  EXPECT_NEAR(0.0, pole3.x, 1e-12);
  EXPECT_NEAR(-kPi, pole3.y, 1e-11);

  // 2. Invoke P->fwd directly with |phi| > pi/2 so bt > 1 and ct = 1 - bt^2 <
  // 0, exercising the `if (ct < 0.) ct = 0.` clamping branch without
  // fwd_prepare's latitude check.
  ASSERT_NE(nullptr, vandg2->fwd);
  const PJ_XY direct_over_pole2 =
      vandg2->fwd({.lam = 0.0, .phi = kHalfPi + 0.05}, vandg2.get());
  EXPECT_NEAR(0.0, direct_over_pole2.x, 1e-12);
  EXPECT_NEAR(2.0 * (kHalfPi + 0.05), direct_over_pole2.y, 1e-12);

  ASSERT_NE(nullptr, vandg3->fwd);
  const PJ_XY direct_over_pole3 =
      vandg3->fwd({.lam = 0.0, .phi = -(kHalfPi + 0.05)}, vandg3.get());
  EXPECT_NEAR(0.0, direct_over_pole3.x, 1e-12);
  EXPECT_NEAR(-2.0 * (kHalfPi + 0.05), direct_over_pole3.y, 1e-12);
}

TEST(Vandg2Test, TransArrayAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=vandg2 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(15.0), DegToRad(25.0), 0.0, 0.0),
      proj_coord(DegToRad(-45.0), DegToRad(-35.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  EXPECT_GT(coords[0].xy.x, 0.0);
  EXPECT_GT(coords[0].xy.y, 0.0);
  EXPECT_LT(coords[1].xy.x, 0.0);
  EXPECT_LT(coords[1].xy.y, 0.0);

  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(factors.meridional_scale, 0.0);
  EXPECT_GT(factors.parallel_scale, 0.0);
  EXPECT_GT(factors.areal_scale, 0.0);
}

}  // namespace
}  // namespace proj
