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
// Unit tests for the Nell-Hammer pseudocylindrical projection
// (proj/src/projections/nell_h.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/nell_h.cpp

#include <cmath>
#include <numbers>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(NellHTest, MetadataAndForwardInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nell_h +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("nell_h", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Nell-Hammer"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 0.0},
      {.lon_deg = 30.0, .lat_deg = 25.0},
      {.lon_deg = -60.0, .lat_deg = 50.0},
      {.lon_deg = 120.0, .lat_deg = -40.0},
      {.lon_deg = -150.0, .lat_deg = -75.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);

    // 1. Forward: x = 0.5 * lam * (1 + cos(phi)), y = 2 * (phi - tan(phi/2)).
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.5 * lam * (1.0 + std::cos(phi)), fwd.xy.x, 1e-12);
    EXPECT_NEAR(2.0 * (phi - std::tan(0.5 * phi)), fwd.xy.y, 1e-12);

    // 2. Inverse: Newton-Raphson converges within NITER = 9 iterations.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-7);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-7);
  }
}

TEST(NellHTest, InversePolesExhaustIterationLimitAndSnapToHalfPi) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nell_h +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // At the North and South Poles (phi = +-pi/2), the derivative of
  // phi - tan(phi/2) vanishes (1 - 0.5 / cos^2(pi/4) == 0). Newton-Raphson
  // halves the residual each step and exhausts all NITER = 9 iterations
  // (|V| ~= 0.0015 > 1e-7), entering the `if (!i)` fallback that sets
  // lp.phi = +-M_HALFPI and lp.lam = 2 * xy.x.
  const double lam = DegToRad(50.0);

  // 1. North Pole (p > 0 -> +M_HALFPI).
  const PJ_COORD north_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.5 * lam, north_fwd.xy.x, 1e-12);
  EXPECT_NEAR(kPi - 2.0, north_fwd.xy.y, 1e-12);

  const PJ_COORD north_inv = proj_trans(pj.get(), PJ_INV, north_fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, north_inv.lp.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, north_inv.lp.phi, 1e-12);

  // 2. South Pole (p < 0 -> -M_HALFPI).
  const PJ_COORD south_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-0.5 * lam, south_fwd.xy.x, 1e-12);
  EXPECT_NEAR(-(kPi - 2.0), south_fwd.xy.y, 1e-12);

  const PJ_COORD south_inv = proj_trans(pj.get(), PJ_INV, south_fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-lam, south_inv.lp.lam, 1e-12);
  EXPECT_NEAR(-kHalfPi, south_inv.lp.phi, 1e-12);
}

TEST(NellHTest, FactorsVerifyEqualAreaScale) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nell_h +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(15.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-5);
}

}  // namespace
}  // namespace proj
