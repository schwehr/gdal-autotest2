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
// Unit tests for the Patterson Cylindrical projection
// (proj/src/projections/patterson.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/patterson.cpp

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

// Polynomial constants from patterson.cpp.
constexpr double kK1 = 1.0148;
constexpr double kK2 = 0.23185;
constexpr double kK3 = -0.14499;
constexpr double kK4 = 0.02406;
constexpr double kMaxY = 1.790857183;

constexpr double ExpectedUnitPattersonY(double phi) {
  const double phi2 = phi * phi;
  return phi * (kK1 + phi2 * phi2 * (kK2 + phi2 * (kK3 + kK4 * phi2)));
}

TEST(PattersonTest, MetadataAndForcesSphericalEccentricityZero) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Even when created with an ellipsoidal definition (+ellps=WGS84),
  // PJ_PROJECTION(patterson) forces P->es = 0.
  ScopedPj pj(proj_create(ctx.get(), "+proj=patterson +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("patterson", info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", info.description);
  EXPECT_STREQ("Patterson Cylindrical\n\tCyl", pj->descr);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, pj->es);
}

TEST(PattersonTest, UnitSphereForwardMatchesAnalyticalPolynomialAndSymmetry) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=patterson +R=1"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 30.0, .lat_deg = 15.0},
      {.lon_deg = -75.0, .lat_deg = 45.0},
      {.lon_deg = 120.0, .lat_deg = -60.0},
      {.lon_deg = -180.0, .lat_deg = 90.0},
      {.lon_deg = 180.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);

    // 1. Forward on R=1 matches exact polynomial formula.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, fwd.xy.x, 1e-12);
    EXPECT_NEAR(ExpectedUnitPattersonY(phi), fwd.xy.y, 1e-12);

    // 2. Odd symmetry in both longitude and latitude.
    const PJ_COORD neg_fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, -phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(-fwd.xy.x, neg_fwd.xy.x, 1e-12);
    EXPECT_NEAR(-fwd.xy.y, neg_fwd.xy.y, 1e-12);

    // 3. Inverse round-trips to input (lam, phi). At +-90 deg, fwd.xy.y
    // (~1.79085718329) slightly exceeds MAX_Y (1.790857183) and is clamped in
    // patterson_s_inverse, shifting the recovered pole latitude by ~2.97e-10.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-10);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-9);
  }
}

TEST(PattersonTest, InverseClampsYBeyondPoleMaxYToNorthAndSouthPoles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=patterson +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. xy.y > MAX_Y (1.790857183) clamps target y to +MAX_Y and converges to
  //    +pi/2.
  const PJ_COORD north_clamped =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.5, 2.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.5, north_clamped.lp.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, north_clamped.lp.phi, 1e-7);

  // 2. xy.y < -MAX_Y (-1.790857183) clamps target y to -MAX_Y and converges to
  //    -pi/2.
  const PJ_COORD south_clamped =
      proj_trans(pj.get(), PJ_INV, proj_coord(-0.5, -2.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-0.5, south_clamped.lp.lam, 1e-12);
  EXPECT_NEAR(-kHalfPi, south_clamped.lp.phi, 1e-7);
}

TEST(PattersonTest, InverseNewtonRaphsonNonConvergenceSetsOutsideDomainErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=patterson +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // In patterson_s_inverse, yc = xy.y is assigned BEFORE xy.y is clamped to
  // MAX_Y. Starting at xy.y = 1e8 keeps all 100 Newton-Raphson iterations
  // completely finite (each step on the degree-9 polynomial scales yc by 8/9,
  // leaving yc ~ 766 after 100 iterations), exhausting i == 0 and setting
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  const PJ_COORD non_conv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 1e8, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, non_conv.lp.lam);
  EXPECT_EQ(HUGE_VAL, non_conv.lp.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // Also verify NaN y input via pj_inv exhausts MAX_ITER and sets the domain
  // error (proj_trans short-circuits NaN coordinates before invoking P->inv).
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const PJ_LP nan_inv = pj_inv({.x = 0.0, .y = nan}, pj.get());
  EXPECT_EQ(HUGE_VAL, nan_inv.lam);
  EXPECT_EQ(HUGE_VAL, nan_inv.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
  EXPECT_DOUBLE_EQ(kMaxY, 1.790857183);
}

TEST(PattersonTest, Classic2dArrayTransformAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=patterson +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP lp = {.lam = DegToRad(25.0), .phi = DegToRad(-35.0)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, inv_lp.lam, 1e-10);
  EXPECT_NEAR(lp.phi, inv_lp.phi, 1e-10);

  auto coords = std::to_array<PJ_COORD>({
      proj_coord(DegToRad(-120.0), DegToRad(40.0), 0.0, 0.0),
      proj_coord(DegToRad(60.0), DegToRad(-20.0), 0.0, 0.0),
  });
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(-120.0), coords[0].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(40.0), coords[0].lp.phi, 1e-10);
  EXPECT_NEAR(DegToRad(60.0), coords[1].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(-20.0), coords[1].lp.phi, 1e-10);

  // At the equator, dy/dphi = K1 = 1.0148 and dx/dlam = 1.0.
  const PJ_FACTORS eq_factors =
      proj_factors(pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kK1, eq_factors.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, eq_factors.parallel_scale, 1e-6);
}

}  // namespace
}  // namespace proj
