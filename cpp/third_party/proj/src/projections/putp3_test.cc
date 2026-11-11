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
// Unit tests for the Putnins P3 and Putnins P3' pseudocylindrical projections
// (proj/src/projections/putp3.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/putp3.cpp

#include <array>
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
constexpr double kC = 0.79788456;
constexpr double kRpisq = 0.1013211836;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Putp3Test, MetadataAndSetupForPutp3AndPutp3p) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj3(proj_create(ctx.get(), "+proj=putp3 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj3.get());
  EXPECT_EQ(0, proj_errno(pj3.get()));
  EXPECT_EQ(0.0, pj3->es);
  const PJ_PROJ_INFO info3 = proj_pj_info(pj3.get());
  EXPECT_STREQ("putp3", info3.id);
  EXPECT_NE(std::string_view(pj3->descr).find("Putnins P3"),
            std::string_view::npos);
  EXPECT_EQ(1, info3.has_inverse);

  ScopedPj pj3p(proj_create(ctx.get(), "+proj=putp3p +ellps=WGS84"));
  ASSERT_NE(nullptr, pj3p.get());
  EXPECT_EQ(0, proj_errno(pj3p.get()));
  EXPECT_EQ(0.0, pj3p->es);
  const PJ_PROJ_INFO info3p = proj_pj_info(pj3p.get());
  EXPECT_STREQ("putp3p", info3p.id);
  EXPECT_NE(std::string_view(pj3p->descr).find("Putnins P3'"),
            std::string_view::npos);
  EXPECT_EQ(1, info3p.has_inverse);
}

TEST(Putp3Test, ForwardFormulaPointedPolesInPutp3VsFlatPolesInPutp3p) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj3(proj_create(ctx.get(), "+proj=putp3 +R=1"));
  ASSERT_NE(nullptr, pj3.get());
  ScopedPj pj3p(proj_create(ctx.get(), "+proj=putp3p +R=1"));
  ASSERT_NE(nullptr, pj3p.get());

  constexpr double kLon = DegToRad(60.0);
  constexpr double kLat = DegToRad(45.0);

  // 1. Putnins P3 uses A = 4 * RPISQ:
  //    x = C * lam * (1 - 4 * RPISQ * phi^2), y = C * phi.
  const PJ_COORD fwd3 =
      proj_trans(pj3.get(), PJ_FWD, proj_coord(kLon, kLat, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj3.get()));
  EXPECT_NEAR(kC * kLon * (1.0 - 4.0 * kRpisq * kLat * kLat), fwd3.xy.x, 1e-12);
  EXPECT_NEAR(kC * kLat, fwd3.xy.y, 1e-12);

  // 2. Putnins P3' uses A = 2 * RPISQ:
  //    x = C * lam * (1 - 2 * RPISQ * phi^2), y = C * phi.
  const PJ_COORD fwd3p =
      proj_trans(pj3p.get(), PJ_FWD, proj_coord(kLon, kLat, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj3p.get()));
  EXPECT_NEAR(kC * kLon * (1.0 - 2.0 * kRpisq * kLat * kLat), fwd3p.xy.x,
              1e-12);
  EXPECT_NEAR(kC * kLat, fwd3p.xy.y, 1e-12);

  // 3. At the North Pole (phi = pi/2), Putnins P3 converges to x ~ 0 (pointed
  //    pole), while Putnins P3' has x ~ 0.5 * C * lam (flat pole).
  const PJ_COORD pole3 =
      proj_trans(pj3.get(), PJ_FWD, proj_coord(kLon, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj3.get()));
  EXPECT_NEAR(0.0, pole3.xy.x, 1e-9);
  EXPECT_NEAR(kC * kHalfPi, pole3.xy.y, 1e-12);

  const PJ_COORD pole3p =
      proj_trans(pj3p.get(), PJ_FWD, proj_coord(kLon, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj3p.get()));
  EXPECT_NEAR(0.5 * kC * kLon, pole3p.xy.x, 1e-9);
  EXPECT_NEAR(kC * kHalfPi, pole3p.xy.y, 1e-12);
}

TEST(Putp3Test, RoundTripForwardAndInverseForBothVariants) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr const char* kVariants[] = {
      "+proj=putp3 +R=6371000",
      "+proj=putp3p +R=6371000",
  };
  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 25.0, .lat_deg = 15.0},
      {.lon_deg = -110.0, .lat_deg = 50.0},
      {.lon_deg = 140.0, .lat_deg = -35.0},
      {.lon_deg = -175.0, .lat_deg = -75.0},
  };

  for (const char* def : kVariants) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get());

    for (const auto& tc : kCases) {
      const double lam = DegToRad(tc.lon_deg);
      const double phi = DegToRad(tc.lat_deg);

      // 1. Forward transform.
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 50.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get()));

      // 2. Inverse transform recovers input radians.
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(lam, inv.lp.lam, 1e-12);
      EXPECT_NEAR(phi, inv.lp.phi, 1e-12);
      EXPECT_NEAR(50.0, inv.lpz.z, 1e-12);
    }
  }
}

TEST(Putp3Test, Classic2DFwdInvArrayAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=putp3p +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP lp = {.lam = DegToRad(40.0), .phi = DegToRad(-30.0)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, inv_lp.lam, 1e-12);
  EXPECT_NEAR(lp.phi, inv_lp.phi, 1e-12);

  std::array<PJ_COORD, 2> batch = {
      proj_coord(DegToRad(-60.0), DegToRad(30.0), 0.0, 0.0),
      proj_coord(DegToRad(90.0), DegToRad(-60.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, batch.size(), batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, batch.size(), batch.data()));
  EXPECT_NEAR(DegToRad(-60.0), batch[0].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(30.0), batch[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(90.0), batch[1].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-60.0), batch[1].lp.phi, 1e-12);

  // At origin (0, 0), dx/dlam = C and dy/dphi = C, so areal_scale = C^2.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kC, factors.meridional_scale, 1e-5);
  EXPECT_NEAR(kC, factors.parallel_scale, 1e-5);
  EXPECT_NEAR(kC * kC, factors.areal_scale, 1e-5);
}

}  // namespace
}  // namespace proj
