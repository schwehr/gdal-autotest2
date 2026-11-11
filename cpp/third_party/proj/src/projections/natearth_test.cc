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
// Unit tests for the Natural Earth (natearth) projection
// (proj/src/projections/natearth.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/natearth.cpp

#include <array>
#include <cmath>
#include <limits>
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

// Polynomial constants from natearth.cpp.
constexpr double kA0 = 0.8707;
constexpr double kA1 = -0.131979;
constexpr double kA2 = -0.013791;
constexpr double kA3 = 0.003971;
constexpr double kA4 = -0.001529;
constexpr double kB0 = 1.007226;
constexpr double kB1 = 0.015085;
constexpr double kB2 = -0.044475;
constexpr double kB3 = 0.028874;
constexpr double kB4 = -0.005916;
constexpr double kMaxY = 0.8707 * 0.52 * kPi;

constexpr PJ_XY ExpectedUnitNatEarthXY(double lam, double phi) {
  const double phi2 = phi * phi;
  const double phi4 = phi2 * phi2;
  const double x =
      lam *
      (kA0 + phi2 * (kA1 + phi2 * (kA2 + phi4 * phi2 * (kA3 + phi2 * kA4))));
  const double y =
      phi * (kB0 + phi2 * (kB1 + phi4 * (kB2 + kB3 * phi2 + kB4 * phi4)));
  return {.x = x, .y = y};
}

TEST(NatearthTest, MetadataAndForcesSphericalEccentricityZero) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=natearth +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("natearth", info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", info.description);
  EXPECT_NE(std::string_view(pj->descr).find("Natural Earth"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, pj->es);
}

TEST(NatearthTest, UnitSphereForwardAndInverseMatchPolynomialAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=natearth +R=1"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 0.0},
      {.lon_deg = 0.0, .lat_deg = 60.0},
      {.lon_deg = -120.0, .lat_deg = 35.0},
      {.lon_deg = 150.0, .lat_deg = -45.0},
      {.lon_deg = -180.0, .lat_deg = 90.0},
      {.lon_deg = 180.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);
    const PJ_XY expected = ExpectedUnitNatEarthXY(lam, phi);

    // 1. Forward matches exact polynomial evaluation.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected.x, fwd.xy.x, 1e-12);
    EXPECT_NEAR(expected.y, fwd.xy.y, 1e-12);

    // 2. Symmetry across prime meridian and equator.
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

TEST(NatearthTest, InverseClampsYBeyondMaxYAndHandlesNonConvergence) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=natearth +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. xy.y > MAX_Y clamps to +MAX_Y (~ +pi/2).
  const PJ_COORD north_clamped =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.3, kMaxY + 0.5, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kHalfPi, north_clamped.lp.phi, 1e-4);
  EXPECT_GT(north_clamped.lp.lam, 0.3);

  // 2. xy.y < -MAX_Y clamps to -MAX_Y (~ -pi/2).
  const PJ_COORD south_clamped =
      proj_trans(pj.get(), PJ_INV, proj_coord(-0.3, -kMaxY - 0.5, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kHalfPi, south_clamped.lp.phi, 1e-4);
  EXPECT_LT(south_clamped.lp.lam, -0.3);

  // 3. NaN y bypasses the < / > clamps, runs MAX_ITER = 100 iterations without
  //    satisfying fabs(tol) < EPS, and triggers i == 0 ->
  //    PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN. Call pj_inv (not
  //    proj_trans, which short-circuits NaN coordinates before P->inv).
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const PJ_LP nan_inv = pj_inv({.x = 0.0, .y = nan}, pj.get());
  EXPECT_EQ(HUGE_VAL, nan_inv.lam);
  EXPECT_EQ(HUGE_VAL, nan_inv.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
}

TEST(NatearthTest, Classic2dEntryPointsArrayTransformAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=natearth +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP lp = {.lam = DegToRad(35.0), .phi = DegToRad(-25.0)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, inv_lp.lam, 1e-10);
  EXPECT_NEAR(lp.phi, inv_lp.phi, 1e-10);

  auto coords = std::to_array<PJ_COORD>({
      proj_coord(DegToRad(-80.0), DegToRad(20.0), 0.0, 0.0),
      proj_coord(DegToRad(110.0), DegToRad(-50.0), 0.0, 0.0),
  });
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(-80.0), coords[0].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(20.0), coords[0].lp.phi, 1e-10);
  EXPECT_NEAR(DegToRad(110.0), coords[1].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(-50.0), coords[1].lp.phi, 1e-10);

  // At the origin (0, 0), dx/dlam = A0 = 0.8707 and dy/dphi = B0 = 1.007226.
  const PJ_FACTORS eq_factors =
      proj_factors(pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kB0, eq_factors.meridional_scale, 1e-6);
  EXPECT_NEAR(kA0, eq_factors.parallel_scale, 1e-6);
}

}  // namespace
}  // namespace proj
