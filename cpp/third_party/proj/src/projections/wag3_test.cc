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
// Unit tests for the Wagner III pseudocylindrical projection
// (proj/src/projections/wag3.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/wag3.cpp

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
constexpr double kTwoThird = 2.0 / 3.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Wag3Test, MetadataAndDefaultLatTsForwardInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  // Default lat_ts is 0, so C_x = cos(0) / cos(0) = 1.0.
  ScopedPj pj(proj_create(ctx.get(), "+proj=wag3 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("wag3", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Wagner III"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 90.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 30.0},
      {.lon_deg = -60.0, .lat_deg = 60.0},
      {.lon_deg = 120.0, .lat_deg = -45.0},
      {.lon_deg = -180.0, .lat_deg = 90.0},
      {.lon_deg = 180.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);

    // 1. Forward: x = lam * cos((2/3) * phi), y = phi.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam * std::cos(kTwoThird * phi), fwd.xy.x, 1e-12);
    EXPECT_NEAR(phi, fwd.xy.y, 1e-12);

    // 2. Inverse round-trip.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-12);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-12);
  }
}

TEST(Wag3Test, CustomLatTsScalesHorizontalCoordinateAndPreservesTrueScale) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=wag3 +lat_ts=30 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const double ts = DegToRad(30.0);
  const double c_x = std::cos(ts) / std::cos(kTwoThird * ts);
  const double lam = DegToRad(40.0);
  const double phi = DegToRad(-50.0);

  // 1. Forward and inverse with custom lat_ts.
  const PJ_XY xy = pj_fwd({.lam = lam, .phi = phi}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(c_x * lam * std::cos(kTwoThird * phi), xy.x, 1e-12);
  EXPECT_NEAR(phi, xy.y, 1e-12);

  const PJ_LP lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, lp.lam, 1e-12);
  EXPECT_NEAR(phi, lp.phi, 1e-12);

  // 2. At phi = +-lat_ts, parallel scale k = C_x * cos((2/3)*ts) / cos(ts) == 1
  //    and meridional scale along central meridian is 1.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, ts, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-6);
  EXPECT_NEAR(1.0, factors.meridional_scale, 1e-6);

  // 3. At the poles (phi = +-pi/2), cos((2/3)*(+-pi/2)) = 0.5, so the pole line
  //    is half the length of the equator.
  const PJ_COORD eq =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, 0.0, 0.0, 0.0));
  const PJ_COORD pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, kHalfPi, 0.0, 0.0));
  EXPECT_NEAR(0.5 * eq.xy.x, pole.xy.x, 1e-12);
}

}  // namespace
}  // namespace proj
