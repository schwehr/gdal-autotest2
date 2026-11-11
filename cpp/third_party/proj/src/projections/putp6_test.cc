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
// Unit tests for the Putnins P6 (putp6) and Putnins P6' (putp6p)
// pseudocylindrical projections (proj/src/projections/putp6.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/putp6.cpp

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

TEST(Putp6Test, Putp6AndPutp6pForwardAndInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 30.0, .lat_deg = 15.0},
      {.lon_deg = -75.0, .lat_deg = 45.0},
      {.lon_deg = 120.0, .lat_deg = -60.0},
      {.lon_deg = -180.0, .lat_deg = 80.0},
      {.lon_deg = 180.0, .lat_deg = -85.0},
      // Exact poles (converge to theta = +-sqrt(3), y = +-C_y * sqrt(3)).
      {.lon_deg = 0.0, .lat_deg = 90.0},
      {.lon_deg = 0.0, .lat_deg = -90.0},
  };

  for (const char* defn :
       {"+proj=putp6 +ellps=WGS84", "+proj=putp6p +ellps=WGS84"}) {
    ScopedPj pj(proj_create(ctx.get(), defn));
    ASSERT_NE(nullptr, pj.get()) << defn;
    EXPECT_EQ(1, pj_has_inverse(pj.get()));
    EXPECT_DOUBLE_EQ(0.0, pj->es);

    for (const auto& tc : kCases) {
      const double lam = DegToRad(tc.lon_deg);
      const double phi = DegToRad(tc.lat_deg);

      // 1. Forward projection.
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get())) << defn;
      EXPECT_FALSE(std::isinf(fwd.xy.x));
      EXPECT_FALSE(std::isinf(fwd.xy.y));

      // 2. Inverse projection. At the exact poles (|lat| == 90 deg), asin(v)
      // has infinite slope 1/sqrt(1 - v^2), so a 1-2 ULP rounding in v < 1.0
      // yields sqrt(2 * ULP) ~ 2.6e-8 rad.
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << defn;
      const double phi_tol = std::abs(tc.lat_deg) >= 89.9 ? 1e-7 : 1e-9;
      EXPECT_NEAR(phi, inv.lp.phi, phi_tol) << defn;
      if (std::abs(tc.lat_deg) < 89.9) {
        EXPECT_NEAR(lam, inv.lp.lam, 1e-9) << defn;
      }
    }

    // 3. Verify proj_factors at a mid-latitude point.
    const PJ_FACTORS factors = proj_factors(
        pj.get(), proj_coord(DegToRad(20.0), DegToRad(35.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_GT(factors.areal_scale, 0.5);
  }
}

TEST(Putp6Test, ForwardNonConvergenceFallbackAssignsSignedConPole) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=putp6 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // At the pole sin(phi) = +-1, the target root is theta = +-sqrt(3) where the
  // derivative A - 2*sqrt(1 + theta^2) = 4 - 4 = 0 (a double root, so Newton's
  // method only halves the error per step). Passing phi = +-2.5*pi to P->fwd
  // directly starts at theta_0 = +-8.66 while keeping sin(phi) = +-1, so 10
  // iterations cannot reach EPS = 1e-10 and `!i` triggers for both p > 0 and
  // p < 0.
  constexpr double kConPole = 1.732050807568877;
  constexpr double kCy = 0.91910;

  const PJ_XY pos_pole = pj->fwd({.lam = 1.0, .phi = 2.5 * kPi}, pj.get());
  // sqrt_1_plus_phi2 is set to 2, and Q->D == 2 for putp6, so x == 0.
  EXPECT_DOUBLE_EQ(0.0, pos_pole.x);
  EXPECT_NEAR(kCy * kConPole, pos_pole.y, 1e-12);

  const PJ_XY neg_pole = pj->fwd({.lam = 1.0, .phi = -2.5 * kPi}, pj.get());
  EXPECT_DOUBLE_EQ(0.0, neg_pole.x);
  EXPECT_NEAR(-kCy * kConPole, neg_pole.y, 1e-12);
}

TEST(Putp6Test, InverseOutsideProjectionDomainSetsErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  for (const char* defn : {"+proj=putp6 +R=1", "+proj=putp6p +R=1"}) {
    ScopedPj pj(proj_create(ctx.get(), defn));
    ASSERT_NE(nullptr, pj.get());

    // Large |y| causes |((A - r) * phi - log(phi + r)) / B| > 1 in aasin().
    const PJ_COORD inv =
        proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 10.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, inv.lp.lam);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }
}

}  // namespace
}  // namespace proj
