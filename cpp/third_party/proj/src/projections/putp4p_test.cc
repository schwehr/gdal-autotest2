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
// Unit tests for the Putnins P4' and Werenskiold I projections
// (proj/src/projections/putp4p.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/putp4p.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Putp4pTest, Putp4pForwardAndInverseAnalyticalAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=putp4p +R=1"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  constexpr double kCx = 0.874038744;
  constexpr double kCy = 3.883251825;

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 30.0, .lat_deg = 45.0},
      {.lon_deg = -60.0, .lat_deg = 35.0},
      {.lon_deg = 90.0, .lat_deg = -50.0},
      {.lon_deg = -120.0, .lat_deg = -75.0},
  };

  for (const auto& tc : kCases) {
    const PJ_LP in = {.lam = DegToRad(tc.lon_deg), .phi = DegToRad(tc.lat_deg)};
    const PJ_XY xy = pj_fwd(in, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));

    const double theta = std::asin(0.883883476 * std::sin(in.phi));
    const double expected_x =
        kCx * in.lam * std::cos(theta) / std::cos(theta / 3.0);
    const double expected_y = kCy * std::sin(theta / 3.0);
    EXPECT_NEAR(expected_x, xy.x, 1e-9);
    EXPECT_NEAR(expected_y, xy.y, 1e-9);

    const PJ_LP out = pj_inv(xy, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    // Note: 0.883883476 * 1.13137085 = 1.000000000376646, so round-trip
    // latitude is accurate to ~1e-8 rad.
    EXPECT_NEAR(in.lam, out.lam, 1e-7);
    EXPECT_NEAR(in.phi, out.phi, 1e-7);
  }
}

TEST(Putp4pTest, WerenForwardAndInverseAnalyticalAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=weren +R=1"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  constexpr double kCx = 1.0;
  constexpr double kCy = 4.442882938;

  const PJ_LP in = {.lam = DegToRad(50.0), .phi = DegToRad(-40.0)};
  const PJ_XY xy = pj_fwd(in, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const double theta = std::asin(0.883883476 * std::sin(in.phi));
  const double expected_x =
      kCx * in.lam * std::cos(theta) / std::cos(theta / 3.0);
  const double expected_y = kCy * std::sin(theta / 3.0);
  EXPECT_NEAR(expected_x, xy.x, 1e-9);
  EXPECT_NEAR(expected_y, xy.y, 1e-9);

  const PJ_LP out = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.lam, out.lam, 1e-7);
  EXPECT_NEAR(in.phi, out.phi, 1e-7);
}

TEST(Putp4pTest, InverseOutOfDomainReportsOutsideProjectionDomain) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_log(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=putp4p +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. |xy.y| > C_y triggers domain error in the first aasin(xy.y / C_y).
  const PJ_COORD inv1 =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 5.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inv1.xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 2. |xy.y| <= C_y but |1.13137085 * sin(3 * asin(xy.y / C_y))| > 1 triggers
  // domain error in the second aasin call.
  const PJ_COORD inv2 =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 2.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inv2.xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(Putp4pTest, EqualAreaFactorsAndTransArray) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj putp4p(proj_create(ctx.get(), "+proj=putp4p +R=6371000"));
  ASSERT_NE(nullptr, putp4p.get());

  // Putnins P4' is an equal-area pseudocylindrical projection (areal_scale ==
  // 1).
  const PJ_FACTORS factors = proj_factors(
      putp4p.get(), proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(putp4p.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-6);

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(15.0), DegToRad(25.0), 0.0, 0.0),
      proj_coord(DegToRad(-45.0), DegToRad(-35.0), 0.0, 0.0),
  };
  const PJ_COORD orig0 = coords[0];
  const PJ_COORD orig1 = coords[1];

  ASSERT_EQ(0, proj_trans_array(putp4p.get(), PJ_FWD, 2, coords));
  ASSERT_EQ(0, proj_trans_array(putp4p.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(orig0.lp.lam, coords[0].lp.lam, 1e-7);
  EXPECT_NEAR(orig0.lp.phi, coords[0].lp.phi, 1e-7);
  EXPECT_NEAR(orig1.lp.lam, coords[1].lp.lam, 1e-7);
  EXPECT_NEAR(orig1.lp.phi, coords[1].lp.phi, 1e-7);
}

}  // namespace
}  // namespace proj
