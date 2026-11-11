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
// Unit tests for the Lagrange conformal polyconic projection
// (proj/src/projections/lagrng.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/lagrng.cpp

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

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(LagrngTest, InvalidWOrLat1AtPolesFailsWithIllegalArgValue) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get());

  // 1. W <= 0 is rejected (`Invalid value for W: it should be > 0`).
  for (const char* defn : {"+proj=lagrng +W=0", "+proj=lagrng +W=-1.5"}) {
    ScopedPj pj(proj_create(ctx.get(), defn));
    EXPECT_EQ(nullptr, pj.get()) << defn;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
  }
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for W"));

  // 2. |lat_1| == 90 deg is rejected (`|lat_1| should be < 90°`).
  logs.Clear();
  for (const char* defn :
       {"+proj=lagrng +lat_1=90", "+proj=lagrng +lat_1=-90"}) {
    ScopedPj pj(proj_create(ctx.get(), defn));
    EXPECT_EQ(nullptr, pj.get()) << defn;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
  }
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for lat_1"));
}

TEST(LagrngTest, DefaultAndCustomWAndLat1RoundTripAndPoles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  for (const char* defn :
       {"+proj=lagrng +R=1", "+proj=lagrng +W=1.5 +lat_1=20 +R=1"}) {
    ScopedPj pj(proj_create(ctx.get(), defn));
    ASSERT_NE(nullptr, pj.get()) << defn;
    EXPECT_EQ(1, pj_has_inverse(pj.get()));
    EXPECT_DOUBLE_EQ(0.0, pj->es);

    struct CoordCase {
      double lon_deg;
      double lat_deg;
    };
    constexpr CoordCase kCases[] = {
        {.lon_deg = 0.0, .lat_deg = 0.0},
        {.lon_deg = 30.0, .lat_deg = 25.0},
        {.lon_deg = -60.0, .lat_deg = -40.0},
        {.lon_deg = 120.0, .lat_deg = 65.0},
        {.lon_deg = -150.0, .lat_deg = -15.0},
    };

    for (const auto& tc : kCases) {
      const double lam = DegToRad(tc.lon_deg);
      const double phi = DegToRad(tc.lat_deg);

      // 1. Forward projection.
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get())) << defn;
      EXPECT_FALSE(std::isinf(fwd.xy.x));
      EXPECT_FALSE(std::isinf(fwd.xy.y));

      // 2. Inverse projection.
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << defn;
      EXPECT_NEAR(lam, inv.lp.lam, 1e-11) << defn;
      EXPECT_NEAR(phi, inv.lp.phi, 1e-11) << defn;
    }

    // 3. Exact North Pole (+pi/2 -> (0, 2)) and South Pole (-pi/2 -> (0, -2))
    // in both forward (`fabs(fabs(sin_phi) - 1) < TOL`) and inverse
    // (`fabs(fabs(xy.y) - 2.) < TOL`).
    const PJ_COORD north_fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.5, kHalfPi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(0.0, north_fwd.xy.x);
    EXPECT_DOUBLE_EQ(2.0, north_fwd.xy.y);

    const PJ_COORD north_inv = proj_trans(pj.get(), PJ_INV, north_fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(0.0, north_inv.lp.lam);
    EXPECT_NEAR(kHalfPi, north_inv.lp.phi, 1e-12);

    const PJ_COORD south_fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(-0.5, -kHalfPi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(0.0, south_fwd.xy.x);
    EXPECT_DOUBLE_EQ(-2.0, south_fwd.xy.y);

    const PJ_COORD south_inv = proj_trans(pj.get(), PJ_INV, south_fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(0.0, south_inv.lp.lam);
    EXPECT_NEAR(-kHalfPi, south_inv.lp.phi, 1e-12);

    // 4. Conformal property: angular_distortion == 0 and meridional == parallel
    // scale away from singularities.
    const PJ_FACTORS factors = proj_factors(
        pj.get(), proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(factors.meridional_scale, factors.parallel_scale, 1e-5);
    EXPECT_NEAR(0.0, factors.angular_distortion, 1e-5);
  }
}

TEST(LagrngTest,
     ForwardAndInverseSingularitiesSetOutsideProjectionDomainErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  // With W = 1 and lat_1 = 0, at phi = 0 and lam = pi we have v = 1 and
  // cos(lam * rw) = cos(pi) = -1, so c = 0.5 * (v + 1/v) + cos(lam) = 0 < TOL,
  // triggering `if (c < TOL)` in lagrng_s_forward.
  ScopedPj pj(proj_create(ctx.get(), "+proj=lagrng +W=1 +lat_1=0 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_COORD bad_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kPi, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_fwd.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // In lagrng_s_inverse, c = (2 + y) * (2 - y) - x^2 = 4 - x^2 - y^2.
  // At (x, y) = (2, 0), |y| != 2 and c == 0 < TOL, triggering `if (fabs(c) <
  // TOL)`.
  const PJ_COORD bad_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(2.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_inv.lp.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

}  // namespace
}  // namespace proj
