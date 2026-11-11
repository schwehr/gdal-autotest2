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
// Unit tests for the Urmaev Flat-Polar Sinusoidal (urmfps) and Wagner I /
// Kavrayskiy VI (wag1) projections (proj/src/projections/urmfps.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/urmfps.cpp

#include <cmath>
#include <numbers>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;
constexpr double kCx = 0.8773826753;
constexpr double kCy = 1.139753528477;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(UrmfpsTest, MissingOrOutOfRangeParameterNFails) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Missing +n fails with PROJ_ERR_INVALID_OP_MISSING_ARG.
  {
    ScopedLogCapture log_capture(ctx.get());
    proj_context_errno_set(ctx.get(), 0);

    ScopedPj pj(proj_create(ctx.get(), "+proj=urmfps"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
    EXPECT_TRUE(log_capture.ContainsMessage("Missing parameter n."));
  }

  // 2. Out-of-range +n (<= 0 or > 1) fails with
  // PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE.
  constexpr const char* kOutOfRangeDefs[] = {
      "+proj=urmfps +n=0",
      "+proj=urmfps +n=-0.5",
      "+proj=urmfps +n=1.0001",
      "+proj=urmfps +n=2.0",
  };
  for (const char* def : kOutOfRangeDefs) {
    ScopedLogCapture log_capture(ctx.get());
    proj_context_errno_set(ctx.get(), 0);

    ScopedPj pj(proj_create(ctx.get(), def));
    EXPECT_EQ(nullptr, pj.get()) << def;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << def;
    EXPECT_TRUE(log_capture.ContainsMessage(
        "Invalid value for n: it should be in ]0,1] range."))
        << def;
  }
}

TEST(UrmfpsTest, UrmfpsForwardAndInverseAnalyticalValuesAndBoundaryNOne) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Test n = 0.8 and the upper boundary n = 1.0.
  constexpr double kNValues[] = {0.8, 1.0};
  for (const double n : kNValues) {
    const std::string def =
        n == 1.0 ? "+proj=urmfps +n=1.0 +R=1" : "+proj=urmfps +n=0.8 +R=1";
    ScopedPj pj(proj_create(ctx.get(), def.c_str()));
    ASSERT_NE(nullptr, pj.get()) << def;
    EXPECT_EQ(1, pj_has_inverse(pj.get()));
    EXPECT_EQ(0.0, pj->es);

    const PJ_PROJ_INFO info = proj_pj_info(pj.get());
    EXPECT_STREQ("urmfps", info.id);
    EXPECT_NE(std::string_view(pj->descr).find("Urmaev Flat-Polar Sinusoidal"),
              std::string_view::npos);

    struct LonLatCase {
      double lon_deg;
      double lat_deg;
    };
    constexpr LonLatCase kCases[] = {
        {.lon_deg = 0.0, .lat_deg = 0.0},
        {.lon_deg = 30.0, .lat_deg = 45.0},
        {.lon_deg = -120.0, .lat_deg = -60.0},
        {.lon_deg = 180.0, .lat_deg = 85.0},
        {.lon_deg = -90.0, .lat_deg = -90.0},
    };

    for (const auto& tc : kCases) {
      const double lam = DegToRad(tc.lon_deg);
      const double phi = DegToRad(tc.lat_deg);
      const double psi = std::asin(n * std::sin(phi));
      const double expected_x = kCx * lam * std::cos(psi);
      const double expected_y = (kCy / n) * psi;

      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(expected_x, fwd.xy.x, 1e-12);
      EXPECT_NEAR(expected_y, fwd.xy.y, 1e-12);

      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      if (std::abs(std::cos(psi)) > 1e-9) {
        EXPECT_NEAR(lam, inv.lp.lam, 1e-11);
      }
      EXPECT_NEAR(phi, inv.lp.phi, 1e-11);
    }
  }
}

TEST(UrmfpsTest, WagnerIMatchesUrmfpsWithSqrt3Over2AndInverseDomainError) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj wag1(proj_create(ctx.get(), "+proj=wag1 +R=1"));
  ScopedPj urmfps_wag1(proj_create(
      ctx.get(), "+proj=urmfps +n=0.8660254037844386467637231707 +R=1"));
  ASSERT_NE(nullptr, wag1.get());
  ASSERT_NE(nullptr, urmfps_wag1.get());

  const PJ_PROJ_INFO info = proj_pj_info(wag1.get());
  EXPECT_STREQ("wag1", info.id);
  EXPECT_NE(std::string_view(wag1->descr).find("Wagner I (Kavrayskiy VI)"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);

  // 1. Verify wag1 and urmfps(n = sqrt(3)/2) produce identical coordinates.
  constexpr double kLatsDeg[] = {-90.0, -50.0, 0.0, 35.0, 90.0};
  for (const double lat_deg : kLatsDeg) {
    const double lam = DegToRad(75.0);
    const double phi = DegToRad(lat_deg);

    const PJ_COORD w_fwd =
        proj_trans(wag1.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    const PJ_COORD u_fwd =
        proj_trans(urmfps_wag1.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(wag1.get()));
    EXPECT_NEAR(u_fwd.xy.x, w_fwd.xy.x, 1e-12);
    EXPECT_NEAR(u_fwd.xy.y, w_fwd.xy.y, 1e-12);

    const PJ_COORD w_inv = proj_trans(wag1.get(), PJ_INV, w_fwd);
    EXPECT_EQ(0, proj_errno(wag1.get()));
    EXPECT_NEAR(lam, w_inv.lp.lam, 1e-11);
    EXPECT_NEAR(phi, w_inv.lp.phi, 1e-11);
  }

  // 2. Equal-area scale factor check (areal_scale == 1.0).
  const PJ_FACTORS factors = proj_factors(
      wag1.get(), proj_coord(DegToRad(20.0), DegToRad(40.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(wag1.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-5);

  // 3. Inverse Y coordinate where |sin(y / C_y) / n| > 1.000001 fails in aasin.
  const double y_outside = (kCy / (std::numbers::sqrt3 / 2.0)) * kHalfPi;
  const PJ_COORD inv_bad =
      proj_trans(wag1.get(), PJ_INV, proj_coord(0.0, y_outside, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv_bad.lp.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(wag1.get()));
  proj_errno_reset(wag1.get());
}

}  // namespace
}  // namespace proj
