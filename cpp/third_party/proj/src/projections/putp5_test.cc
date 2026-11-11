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
// Unit tests for the Putnins P5 and Putnins P5' projections
// (proj/src/projections/putp5.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/putp5.cpp

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

TEST(Putp5Test, Putp5ForwardAndInverseAnalyticalAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=putp5 +R=1"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  constexpr double kC = 1.01346;
  constexpr double kD = 1.2158542;
  constexpr double kA = 2.0;
  constexpr double kB = 1.0;

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

    const double expected_x =
        kC * in.lam * (kA - kB * std::sqrt(1.0 + kD * in.phi * in.phi));
    const double expected_y = kC * in.phi;
    EXPECT_NEAR(expected_x, xy.x, 1e-12);
    EXPECT_NEAR(expected_y, xy.y, 1e-12);

    const PJ_LP out = pj_inv(xy, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lam, out.lam, 1e-12);
    EXPECT_NEAR(in.phi, out.phi, 1e-12);
  }
}

TEST(Putp5Test, Putp5pForwardAndInverseAnalyticalAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=putp5p +R=1"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  constexpr double kC = 1.01346;
  constexpr double kD = 1.2158542;
  constexpr double kA = 1.5;
  constexpr double kB = 0.5;

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 60.0},
      {.lon_deg = -75.0, .lat_deg = 25.0},
      {.lon_deg = 110.0, .lat_deg = -40.0},
      {.lon_deg = -150.0, .lat_deg = -80.0},
  };

  for (const auto& tc : kCases) {
    const PJ_LP in = {.lam = DegToRad(tc.lon_deg), .phi = DegToRad(tc.lat_deg)};
    const PJ_XY xy = pj_fwd(in, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));

    const double expected_x =
        kC * in.lam * (kA - kB * std::sqrt(1.0 + kD * in.phi * in.phi));
    const double expected_y = kC * in.phi;
    EXPECT_NEAR(expected_x, xy.x, 1e-12);
    EXPECT_NEAR(expected_y, xy.y, 1e-12);

    const PJ_LP out = pj_inv(xy, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lam, out.lam, 1e-12);
    EXPECT_NEAR(in.phi, out.phi, 1e-12);
  }
}

TEST(Putp5Test, PoleGeometryPointInPutp5VersusLineInPutp5p) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj putp5(proj_create(ctx.get(), "+proj=putp5 +R=1"));
  ASSERT_NE(nullptr, putp5.get());
  ScopedPj putp5p(proj_create(ctx.get(), "+proj=putp5p +R=1"));
  ASSERT_NE(nullptr, putp5p.get());

  // At |phi| = pi/2, sqrt(1 + D * (pi/2)^2) ~= 2.
  // For Putnins P5 (A=2, B=1), A - B*2 ~= 0 (pole collapses to a point).
  // For Putnins P5' (A=1.5, B=0.5), A - B*2 ~= 0.5 (pole is half the equator).
  const PJ_XY pole_p5 = pj_fwd({.lam = kPi, .phi = kHalfPi}, putp5.get());
  EXPECT_EQ(0, proj_errno(putp5.get()));
  EXPECT_NEAR(0.0, pole_p5.x, 1e-6);

  const PJ_XY eq_p5p = pj_fwd({.lam = kPi, .phi = 0.0}, putp5p.get());
  const PJ_XY pole_p5p = pj_fwd({.lam = kPi, .phi = kHalfPi}, putp5p.get());
  EXPECT_EQ(0, proj_errno(putp5p.get()));
  EXPECT_NEAR(0.5 * eq_p5p.x, pole_p5p.x, 1e-6);
}

TEST(Putp5Test, TransArrayAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=putp5p +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(15.0), DegToRad(25.0), 0.0, 0.0),
      proj_coord(DegToRad(-45.0), DegToRad(-35.0), 0.0, 0.0),
  };
  const PJ_COORD orig0 = coords[0];
  const PJ_COORD orig1 = coords[1];

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(orig0.lp.lam, coords[0].lp.lam, 1e-12);
  EXPECT_NEAR(orig0.lp.phi, coords[0].lp.phi, 1e-12);
  EXPECT_NEAR(orig1.lp.lam, coords[1].lp.lam, 1e-12);
  EXPECT_NEAR(orig1.lp.phi, coords[1].lp.phi, 1e-12);

  // Meridians are equally spaced along y = C * phi, so meridional scale is C
  // along the central meridian.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.01346, factors.meridional_scale, 1e-5);
  EXPECT_GT(factors.parallel_scale, 0.0);
}

}  // namespace
}  // namespace proj
