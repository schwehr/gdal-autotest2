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
// Unit tests for the Natural Earth II (natearth2) projection
// (proj/src/projections/natearth2.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/natearth2.cpp

#include <array>
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

// Polynomial constants from natearth2.cpp.
constexpr double kA0 = 0.84719;
constexpr double kA1 = -0.13063;
constexpr double kA2 = -0.04515;
constexpr double kA3 = 0.05494;
constexpr double kA4 = -0.02326;
constexpr double kA5 = 0.00331;
constexpr double kB0 = 1.01183;
constexpr double kB1 = -0.02625;
constexpr double kB2 = 0.01926;
constexpr double kB3 = -0.00396;
constexpr double kMaxY = 0.84719 * 0.535117535153096 * kPi;

constexpr PJ_XY ExpectedUnitNatEarth2XY(double lam, double phi) {
  const double phi2 = phi * phi;
  const double phi4 = phi2 * phi2;
  const double phi6 = phi2 * phi4;
  const double x =
      lam * (kA0 + kA1 * phi2 +
             phi6 * phi6 * (kA2 + kA3 * phi2 + kA4 * phi4 + kA5 * phi6));
  const double y = phi * (kB0 + phi4 * phi4 * (kB1 + kB2 * phi2 + kB3 * phi4));
  return {.x = x, .y = y};
}

TEST(Natearth2Test, MetadataAndForcesSphericalEccentricityZero) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=natearth2 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("natearth2", info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", info.description);
  EXPECT_STREQ("Natural Earth 2\n\tPCyl, Sph", pj->descr);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, pj->es);
}

TEST(Natearth2Test, UnitSphereForwardAndInverseMatchPolynomialAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=natearth2 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 60.0, .lat_deg = 0.0},
      {.lon_deg = 0.0, .lat_deg = 45.0},
      {.lon_deg = -100.0, .lat_deg = 30.0},
      {.lon_deg = 135.0, .lat_deg = -65.0},
      {.lon_deg = -180.0, .lat_deg = 90.0},
      {.lon_deg = 180.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);
    const PJ_XY expected = ExpectedUnitNatEarth2XY(lam, phi);

    // 1. Forward matches exact polynomial evaluation.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected.x, fwd.xy.x, 1e-12);
    EXPECT_NEAR(expected.y, fwd.xy.y, 1e-12);

    // 2. Odd symmetry across origin.
    const PJ_COORD sym_fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, -phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(-fwd.xy.x, sym_fwd.xy.x, 1e-12);
    EXPECT_NEAR(-fwd.xy.y, sym_fwd.xy.y, 1e-12);

    // 3. Inverse round-trip.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-10);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-10);
  }
}

TEST(Natearth2Test, InverseClampsYBeyondMaxYAndHandlesNonConvergence) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=natearth2 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. xy.y > MAX_Y clamps to +MAX_Y (~ +pi/2).
  const PJ_COORD north_clamped =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.25, kMaxY + 0.5, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kHalfPi, north_clamped.lp.phi, 1e-5);
  EXPECT_GT(north_clamped.lp.lam, 0.25);

  // 2. xy.y < -MAX_Y clamps to -MAX_Y (~ -pi/2).
  const PJ_COORD south_clamped =
      proj_trans(pj.get(), PJ_INV, proj_coord(-0.25, -kMaxY - 0.5, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kHalfPi, south_clamped.lp.phi, 1e-5);
  EXPECT_LT(south_clamped.lp.lam, -0.25);

  // 3. Calling pj_inv with NaN y bypasses proj_trans's pj_coord_has_nans
  //    short-circuit and the < / > clamps, exhausts MAX_ITER = 100 in
  //    natearth2_s_inverse, and triggers i == 0 ->
  //    PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN (with error_or_coord
  //    returning HUGE_VAL).
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const PJ_LP nan_inv = pj_inv({.x = 0.0, .y = nan}, pj.get());
  EXPECT_EQ(HUGE_VAL, nan_inv.lam);
  EXPECT_EQ(HUGE_VAL, nan_inv.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_COORD nan_trans =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, nan, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_trans.lp.lam));
  EXPECT_TRUE(std::isnan(nan_trans.lp.phi));
  EXPECT_EQ(0, proj_errno(pj.get()));
}

TEST(Natearth2Test, Classic2dEntryPointsArrayTransformAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=natearth2 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP lp = {.lam = DegToRad(-40.0), .phi = DegToRad(50.0)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, inv_lp.lam, 1e-10);
  EXPECT_NEAR(lp.phi, inv_lp.phi, 1e-10);

  auto coords = std::to_array<PJ_COORD>({
      proj_coord(DegToRad(-70.0), DegToRad(15.0), 0.0, 0.0),
      proj_coord(DegToRad(95.0), DegToRad(-55.0), 0.0, 0.0),
  });
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(-70.0), coords[0].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(15.0), coords[0].lp.phi, 1e-10);
  EXPECT_NEAR(DegToRad(95.0), coords[1].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(-55.0), coords[1].lp.phi, 1e-10);

  // At the origin (0, 0), dx/dlam = A0 = 0.84719 and dy/dphi = B0 = 1.01183.
  const PJ_FACTORS eq_factors =
      proj_factors(pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kB0, eq_factors.meridional_scale, 1e-6);
  EXPECT_NEAR(kA0, eq_factors.parallel_scale, 1e-6);
}

}  // namespace
}  // namespace proj
