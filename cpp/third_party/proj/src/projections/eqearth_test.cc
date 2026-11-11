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
// Unit tests for Equal Earth (eqearth) projection
// (proj/src/projections/eqearth.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/eqearth.cpp

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

TEST(EqearthTest, EllipsoidalAndSphericalForwardInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  for (const char* def :
       {"+proj=eqearth +ellps=WGS84", "+proj=eqearth +R=6371000"}) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    EXPECT_EQ(1, pj_has_inverse(pj.get()));

    struct CoordCase {
      double lon_rad;
      double lat_rad;
      double lat_tol;
    };
    constexpr CoordCase kCases[] = {
        {.lon_rad = 0.0, .lat_rad = 0.0, .lat_tol = 1e-12},
        {.lon_rad = DegToRad(45.0), .lat_rad = DegToRad(30.0), .lat_tol = 1e-9},
        {.lon_rad = DegToRad(-120.0),
         .lat_rad = DegToRad(-60.0),
         .lat_tol = 1e-9},
        // At +-pi/2, eqearth.cpp clamps y/rqda to MAX_Y = 1.3173627591574
        // (13 decimal places), shifting asin(sin(yc)/M) by ~1.15e-7 rad.
        {.lon_rad = 0.0, .lat_rad = kHalfPi, .lat_tol = 2e-7},
        {.lon_rad = 0.0, .lat_rad = -kHalfPi, .lat_tol = 2e-7},
    };

    for (const auto& tc : kCases) {
      // 1. Forward Equal Earth projection.
      const PJ_COORD in = proj_coord(tc.lon_rad, tc.lat_rad, 25.0, 2026.0);
      const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      EXPECT_FALSE(std::isinf(fwd.xy.x)) << def;
      EXPECT_FALSE(std::isinf(fwd.xy.y)) << def;

      // 2. Inverse Equal Earth projection.
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      EXPECT_NEAR(tc.lon_rad, inv.lp.lam, 1e-9) << def;
      EXPECT_NEAR(tc.lat_rad, inv.lp.phi, tc.lat_tol) << def;
    }

    // 3. Equal-area property check via proj_factors at mid-latitude.
    const PJ_FACTORS factors = proj_factors(
        pj.get(), proj_coord(DegToRad(20.0), DegToRad(40.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    EXPECT_NEAR(1.0, factors.areal_scale, 1e-2) << def;
  }
}

TEST(EqearthTest,
     InverseClampsExceedingMaxYAndHandlesNonConvergenceAndDestructor) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppressor(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=eqearth +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. xy.y > MAX_Y and xy.y < -MAX_Y are clamped to +-MAX_Y (1.3173627591574,
  // which corresponds to +-pi/2 within ~1.15e-7 rad due to MAX_Y truncation).
  const PJ_LP north_clamped = pj_inv({.x = 0.0, .y = 5.0}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, north_clamped.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, north_clamped.phi, 2e-7);

  const PJ_LP south_clamped = pj_inv({.x = 0.0, .y = -5.0}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, south_clamped.lam, 1e-12);
  EXPECT_NEAR(-kHalfPi, south_clamped.phi, 2e-7);

  // 2. NaN y bypasses MAX_Y comparisons and exhausts MAX_ITER = 12 in
  // eqearth_e_inverse, setting
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  (void)pj_inv({.x = 0.0, .y = nan}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_context_errno(ctx.get()));
  proj_errno_reset(pj.get());

  // 3. Destructor null P and null P->opaque guards.
  const PJ_DESTRUCTOR dtor = pj->destructor;
  ASSERT_NE(nullptr, dtor);
  EXPECT_EQ(nullptr, dtor(nullptr, 0));

  PJ* bare_pj = pj_new();
  ASSERT_NE(nullptr, bare_pj);
  bare_pj->ctx = ctx.get();
  EXPECT_EQ(nullptr, dtor(bare_pj, 0));
}

}  // namespace
}  // namespace proj
