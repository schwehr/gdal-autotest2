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
// Unit tests for Guyou, Peirce Quincuncial, Adams Hemisphere in a Square,
// and Adams World in a Square I & II projections
// (proj/src/projections/adams.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/adams.cpp

#include <array>
#include <cmath>
#include <cstddef>
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
// Complete elliptic integral of the first kind K(1/sqrt(2)) and sqrt(2) * K.
constexpr double kEllipK = 1.8540746773013719;
constexpr double kSqrt2EllipK = kEllipK * std::numbers::sqrt2;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(AdamsTest, AllFiveProjectionsSetupMetadataAndSphereForcing) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  struct MetaCase {
    const char* def;
    int expected_has_inverse;
  };
  constexpr MetaCase kCases[] = {
      {.def = "+proj=guyou +R=1", .expected_has_inverse = 0},
      {.def = "+proj=adams_hemi +R=1", .expected_has_inverse = 0},
      {.def = "+proj=adams_ws1 +R=1", .expected_has_inverse = 0},
      {.def = "+proj=adams_ws2 +R=1", .expected_has_inverse = 1},
      {.def = "+proj=peirce_q +R=1", .expected_has_inverse = 1},
      {.def = "+proj=peirce_q +shape=diamond +R=1", .expected_has_inverse = 1},
      {.def = "+proj=peirce_q +shape=square +R=1", .expected_has_inverse = 1},
      {.def = "+proj=peirce_q +shape=nhemisphere +R=1",
       .expected_has_inverse = 0},
      {.def = "+proj=peirce_q +shape=shemisphere +R=1",
       .expected_has_inverse = 0},
      {.def = "+proj=peirce_q +shape=horizontal +R=1",
       .expected_has_inverse = 0},
      {.def = "+proj=peirce_q +shape=vertical +R=1", .expected_has_inverse = 0},
  };

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;
    EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD)) << tc.def;
    EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD)) << tc.def;
    EXPECT_EQ(tc.expected_has_inverse, proj_pj_info(pj.get()).has_inverse)
        << tc.def;
    EXPECT_EQ(tc.expected_has_inverse, pj_has_inverse(pj.get())) << tc.def;

    if (tc.expected_has_inverse == 0) {
      const PJ_COORD inv =
          proj_trans(pj.get(), PJ_INV, proj_coord(0.1, 0.1, 0.0, 0.0));
      EXPECT_TRUE(std::isinf(inv.lp.lam)) << tc.def;
      EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(pj.get())) << tc.def;
      proj_errno_reset(pj.get());
    }
  }

  // Verify pj_adams_setup forces P->es = 0 even when +ellps=WGS84 is passed.
  ScopedPj wgs84_ws2(proj_create(ctx.get(), "+proj=adams_ws2 +ellps=WGS84"));
  ScopedPj sph_ws2(proj_create(ctx.get(), "+proj=adams_ws2 +R=6378137"));
  ASSERT_NE(nullptr, wgs84_ws2.get());
  ASSERT_NE(nullptr, sph_ws2.get());
  EXPECT_DOUBLE_EQ(0.0, wgs84_ws2->es);

  const PJ_COORD pt = proj_coord(DegToRad(30.0), DegToRad(40.0), 0.0, 0.0);
  const PJ_COORD f_wgs = proj_trans(wgs84_ws2.get(), PJ_FWD, pt);
  const PJ_COORD f_sph = proj_trans(sph_ws2.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(wgs84_ws2.get()));
  EXPECT_NEAR(f_sph.xy.x, f_wgs.xy.x, 1e-6);
  EXPECT_NEAR(f_sph.xy.y, f_wgs.xy.y, 1e-6);
}

TEST(AdamsTest, GuyouForwardReferenceSymmetryPolesAndDomainErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=guyou +R=1"));
  ScopedPj pj_r(proj_create(ctx.get(), "+proj=guyou +R=6371000"));
  ASSERT_NE(nullptr, pj.get());
  ASSERT_NE(nullptr, pj_r.get());

  struct GuyouCase {
    double lon_deg;
    double lat_deg;
    double expected_x;
    double expected_y;
  };
  constexpr GuyouCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0, .expected_x = 0.0, .expected_y = 0.0},
      {.lon_deg = 0.0,
       .lat_deg = 90.0,
       .expected_x = 0.0,
       .expected_y = 1.85407},
      {.lon_deg = 0.0,
       .lat_deg = -90.0,
       .expected_x = 0.0,
       .expected_y = -1.85407},
      {.lon_deg = 90.0,
       .lat_deg = 0.0,
       .expected_x = kEllipK,
       .expected_y = 0.0},
      {.lon_deg = -90.0,
       .lat_deg = 0.0,
       .expected_x = -kEllipK,
       .expected_y = 0.0},
      {.lon_deg = 30.0,
       .lat_deg = 40.0,
       .expected_x = 0.4588444004,
       .expected_y = 0.7761693733},
      {.lon_deg = -30.0,
       .lat_deg = 40.0,
       .expected_x = -0.4588444004,
       .expected_y = 0.7761693733},
      {.lon_deg = 30.0,
       .lat_deg = -40.0,
       .expected_x = 0.4588444004,
       .expected_y = -0.7761693733},
      {.lon_deg = -30.0,
       .lat_deg = -40.0,
       .expected_x = -0.4588444004,
       .expected_y = -0.7761693733},
  };

  for (const auto& tc : kCases) {
    const PJ_COORD lp =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, lp);
    const PJ_COORD fwd_r = proj_trans(pj_r.get(), PJ_FWD, lp);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_EQ(0, proj_errno(pj_r.get()));
    EXPECT_NEAR(tc.expected_x, fwd.xy.x, 1e-6);
    EXPECT_NEAR(tc.expected_y, fwd.xy.y, 1e-6);
    EXPECT_NEAR(fwd.xy.x * 6371000.0, fwd_r.xy.x, 1e-3);
    EXPECT_NEAR(fwd.xy.y * 6371000.0, fwd_r.xy.y, 1e-3);
  }

  // Guyou is restricted to |lon| <= 90 deg + TOL.
  for (double bad_lon_deg : {95.0, -95.0}) {
    const PJ_COORD bad = proj_trans(
        pj.get(), PJ_FWD, proj_coord(DegToRad(bad_lon_deg), 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(bad.xy.x));
    EXPECT_TRUE(std::isinf(bad.xy.y));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }
}

TEST(AdamsTest, AdamsHemisphereForwardSymmetryPolesAndDomainErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=adams_hemi +R=1"));
  ASSERT_NE(nullptr, pj.get());

  struct HemiCase {
    double lon_deg;
    double lat_deg;
    double expected_x;
    double expected_y;
  };
  constexpr HemiCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0, .expected_x = 0.0, .expected_y = 0.0},
      {.lon_deg = 0.0,
       .lat_deg = 90.0,
       .expected_x = 0.0,
       .expected_y = kSqrt2EllipK},
      {.lon_deg = 0.0,
       .lat_deg = -90.0,
       .expected_x = 0.0,
       .expected_y = -kSqrt2EllipK},
      {.lon_deg = 90.0,
       .lat_deg = 0.0,
       .expected_x = kSqrt2EllipK,
       .expected_y = 0.0},
      {.lon_deg = 30.0,
       .lat_deg = 40.0,
       .expected_x = 0.4620817607,
       .expected_y = 0.7695529552},
      {.lon_deg = -30.0,
       .lat_deg = 40.0,
       .expected_x = -0.4620817607,
       .expected_y = 0.7695529552},
      {.lon_deg = 30.0,
       .lat_deg = -40.0,
       .expected_x = 0.4620817607,
       .expected_y = -0.7695529552},
      {.lon_deg = -30.0,
       .lat_deg = -40.0,
       .expected_x = -0.4620817607,
       .expected_y = -0.7695529552},
  };

  for (const auto& tc : kCases) {
    const PJ_COORD fwd = proj_trans(
        pj.get(), PJ_FWD,
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(tc.expected_x, fwd.xy.x, 1e-6);
    EXPECT_NEAR(tc.expected_y, fwd.xy.y, 1e-6);
  }

  for (double bad_lon_deg : {95.0, -95.0}) {
    const PJ_COORD bad = proj_trans(
        pj.get(), PJ_FWD, proj_coord(DegToRad(bad_lon_deg), 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(bad.xy.x));
    EXPECT_TRUE(std::isinf(bad.xy.y));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }
}

TEST(AdamsTest, AdamsWorldInASquare1ForwardQuadrantsAndExtremes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=adams_ws1 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  struct Ws1Case {
    double lon_deg;
    double lat_deg;
    double expected_x;
    double expected_y;
  };
  constexpr Ws1Case kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0, .expected_x = 0.0, .expected_y = 0.0},
      {.lon_deg = 180.0,
       .lat_deg = 0.0,
       .expected_x = kEllipK,
       .expected_y = 0.0},
      {.lon_deg = -180.0,
       .lat_deg = 0.0,
       .expected_x = -kEllipK,
       .expected_y = 0.0},
      {.lon_deg = 0.0,
       .lat_deg = 90.0,
       .expected_x = 0.0,
       .expected_y = kEllipK},
      {.lon_deg = 0.0,
       .lat_deg = -90.0,
       .expected_x = 0.0,
       .expected_y = -kEllipK},
      {.lon_deg = 30.0,
       .lat_deg = 40.0,
       .expected_x = 0.2537704337,
       .expected_y = 0.3833176635},
      {.lon_deg = -120.0,
       .lat_deg = -60.0,
       .expected_x = -1.0280567895,
       .expected_y = -0.8260178384},
  };

  for (const auto& tc : kCases) {
    const PJ_COORD fwd = proj_trans(
        pj.get(), PJ_FWD,
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(tc.expected_x, fwd.xy.x, 1e-6);
    EXPECT_NEAR(tc.expected_y, fwd.xy.y, 1e-6);
  }
}

TEST(AdamsTest, AdamsWorldInASquare2ForwardAndGenericInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=adams_ws2 +R=1"));
  ScopedPj pj_r(proj_create(ctx.get(), "+proj=adams_ws2 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());
  ASSERT_NE(nullptr, pj_r.get());

  // 1. Verify forward reference points including rotated square corners.
  const PJ_COORD fwd_30_40 = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(30.0), DegToRad(40.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.2538255575, fwd_30_40.xy.x, 1e-6);
  EXPECT_NEAR(0.3830672864, fwd_30_40.xy.y, 1e-6);

  const PJ_COORD fwd_antimer =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kPi, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kSqrt2EllipK, fwd_antimer.xy.x, 1e-6);
  EXPECT_NEAR(0.0, fwd_antimer.xy.y, 1e-6);

  // 2. Round-trip across all four quadrants, equator, and south pole (where
  //    |lp.phi| >= pi/2 sets initial lam = 0 in adams_inverse()).
  struct RoundTripCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr RoundTripCase kRoundTrips[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 30.0},
      {.lon_deg = -45.0, .lat_deg = 30.0},
      {.lon_deg = 120.0, .lat_deg = -40.0},
      {.lon_deg = -120.0, .lat_deg = -40.0},
      {.lon_deg = 0.0, .lat_deg = 89.0},
      {.lon_deg = 0.0, .lat_deg = -89.0},
      {.lon_deg = 0.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kRoundTrips) {
    const PJ_COORD lp =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj_r.get(), PJ_FWD, lp);
    EXPECT_EQ(0, proj_errno(pj_r.get()));
    const PJ_COORD inv = proj_trans(pj_r.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj_r.get()));
    EXPECT_NEAR(lp.lp.phi, inv.lp.phi, 1e-8);
    if (std::abs(tc.lat_deg) < 90.0) {
      EXPECT_NEAR(lp.lp.lam, inv.lp.lam, 1e-8);
    }
  }

  // 3. Out-of-domain inverse point fails to converge in pj_generic_inverse_2d.
  const PJ_COORD bad_inv = proj_trans(
      pj_r.get(), PJ_INV, proj_coord(10.0 * 6371000.0, 10.0 * 6371000.0, 0, 0));
  EXPECT_TRUE(std::isinf(bad_inv.lp.lam));
  EXPECT_TRUE(std::isinf(bad_inv.lp.phi));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_r.get()));
  proj_errno_reset(pj_r.get());
}

TEST(AdamsTest, PeirceQuincuncialSquareForwardAndAllInverseHeuristicBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=peirce_q +shape=square +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. North pole maps to (0, 0) and hits exact origin shortcut on inverse.
  const PJ_COORD np =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, np.xy.x, 1e-12);
  EXPECT_NEAR(0.0, np.xy.y, 1e-12);
  const PJ_COORD np_inv = proj_trans(pj.get(), PJ_INV, proj_coord(0, 0, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, np_inv.lp.lam);
  EXPECT_DOUBLE_EQ(kHalfPi, np_inv.lp.phi);

  // 2. Direct inverse on exact x == 0, |y| < 2.622057580396 branch -> (-45,45).
  const PJ_COORD b1_lt =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, -0.8308962723, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(DegToRad(-45.0), b1_lt.lp.lam, 1e-8);
  EXPECT_NEAR(DegToRad(45.0), b1_lt.lp.phi, 1e-8);

  // 3. Round-trip table covering all remaining heuristic branches of
  //    peirce_q_square_inverse() plus all 5 southern longitude fold-out bands.
  struct SquareCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr SquareCase kCases[] = {
      // (-45, 0) -> (0, -2.6220575803961212): x==0, |y| >= 2.622057580396.
      {.lon_deg = -45.0, .lat_deg = 0.0},
      // Positive/negative axis branches.
      {.lon_deg = 45.0, .lat_deg = 45.0},
      {.lon_deg = -135.0, .lat_deg = 45.0},
      {.lon_deg = 135.0, .lat_deg = 45.0},
      // Interior northern quadrants.
      {.lon_deg = 20.0, .lat_deg = 30.0},
      {.lon_deg = 110.0, .lat_deg = 30.0},
      {.lon_deg = 170.0, .lat_deg = 30.0},
      {.lon_deg = -160.0, .lat_deg = 30.0},
      {.lon_deg = -110.0, .lat_deg = 30.0},
      // All 5 southern hemisphere fold-out longitude bands.
      {.lon_deg = -170.0, .lat_deg = -45.0},  // lam < -0.75 * pi
      {.lon_deg = -90.0, .lat_deg = -45.0},   // [-0.75 * pi, -0.25 * pi)
      {.lon_deg = 0.0, .lat_deg = -45.0},     // [-0.25 * pi, 0.25 * pi)
      {.lon_deg = 90.0, .lat_deg = -45.0},    // [0.25 * pi, 0.75 * pi)
      {.lon_deg = 170.0, .lat_deg = -45.0},   // >= 0.75 * pi
  };

  for (const auto& tc : kCases) {
    const PJ_COORD lp =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, lp);
    EXPECT_EQ(0, proj_errno(pj.get()))
        << "fwd (" << tc.lon_deg << ", " << tc.lat_deg << ")";
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()))
        << "inv (" << tc.lon_deg << ", " << tc.lat_deg << ")";
    EXPECT_NEAR(lp.lp.lam, inv.lp.lam, 1e-8);
    EXPECT_NEAR(lp.lp.phi, inv.lp.phi, 1e-8);
  }

  // 4. Out-of-domain inverse coordinate outside square returns error.
  const PJ_COORD bad_inv = proj_trans(pj.get(), PJ_INV, proj_coord(5, 5, 0, 0));
  EXPECT_TRUE(std::isinf(bad_inv.lp.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(AdamsTest, PeirceQuincuncialDiamondDefaultAndInverseBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj_default(proj_create(ctx.get(), "+proj=peirce_q +R=1"));
  ScopedPj pj_diamond(
      proj_create(ctx.get(), "+proj=peirce_q +shape=diamond +R=1"));
  ASSERT_NE(nullptr, pj_default.get());
  ASSERT_NE(nullptr, pj_diamond.get());

  // 1. Default shape matches explicit +shape=diamond and handles north pole.
  const PJ_COORD np =
      proj_trans(pj_default.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_default.get()));
  EXPECT_NEAR(0.0, np.xy.x, 1e-12);
  EXPECT_NEAR(0.0, np.xy.y, 1e-12);
  const PJ_COORD np_inv =
      proj_trans(pj_diamond.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_diamond.get()));
  EXPECT_DOUBLE_EQ(0.0, np_inv.lp.lam);
  EXPECT_DOUBLE_EQ(kHalfPi, np_inv.lp.phi);

  // 2. Convergent round-trips hitting all quadrant/axis and southern triangle
  //    branches in peirce_q_diamond_inverse().
  struct DiamondCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr DiamondCase kRoundTrips[] = {
      {.lon_deg = 90.0, .lat_deg = 0.0},
      {.lon_deg = 0.0, .lat_deg = 45.0},
      {.lon_deg = 45.0, .lat_deg = 30.0},
      {.lon_deg = 135.0, .lat_deg = 30.0},
      {.lon_deg = -135.0, .lat_deg = 30.0},
      {.lon_deg = -45.0, .lat_deg = 30.0},
      // Southern hemisphere points (|x| or |y| > 1.855 -> phi0 = -pi/4).
      {.lon_deg = -150.0, .lat_deg = -30.0},
      {.lon_deg = -90.0, .lat_deg = -45.0},
      {.lon_deg = -30.0, .lat_deg = -30.0},
      {.lon_deg = 0.0, .lat_deg = -45.0},
      {.lon_deg = 30.0, .lat_deg = -30.0},
      {.lon_deg = 90.0, .lat_deg = -45.0},
      {.lon_deg = 120.0, .lat_deg = -30.0},
  };
  for (const auto& tc : kRoundTrips) {
    const PJ_COORD lp =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD f_def = proj_trans(pj_default.get(), PJ_FWD, lp);
    const PJ_COORD f_dia = proj_trans(pj_diamond.get(), PJ_FWD, lp);
    EXPECT_EQ(0, proj_errno(pj_default.get()));
    EXPECT_EQ(0, proj_errno(pj_diamond.get()));
    EXPECT_NEAR(f_dia.xy.x, f_def.xy.x, 1e-12);
    EXPECT_NEAR(f_dia.xy.y, f_def.xy.y, 1e-12);

    const PJ_COORD inv = proj_trans(pj_diamond.get(), PJ_INV, f_dia);
    EXPECT_EQ(0, proj_errno(pj_diamond.get()))
        << "inv (" << tc.lon_deg << ", " << tc.lat_deg << ")";
    EXPECT_NEAR(lp.lp.lam, inv.lp.lam, 1e-8);
    EXPECT_NEAR(lp.lp.phi, inv.lp.phi, 1e-8);
  }

  // Forward check for >= 0.75 * pi southern segment in diamond mode.
  const PJ_COORD s_fwd =
      proj_trans(pj_diamond.get(), PJ_FWD,
                 proj_coord(DegToRad(170.0), DegToRad(-45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_diamond.get()));
  EXPECT_NEAR(0.1420162622, s_fwd.xy.x, 1e-6);
  EXPECT_NEAR(2.8938751567, s_fwd.xy.y, 1e-6);

  // Out-of-domain inverse coordinate fails to converge.
  const PJ_COORD bad_inv =
      proj_trans(pj_diamond.get(), PJ_INV, proj_coord(5.0, 5.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(bad_inv.lp.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_diamond.get()));
  proj_errno_reset(pj_diamond.get());
}

TEST(AdamsTest, PeirceQuincuncialHemispheresAndScrolledRectangles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. nhemisphere rejects southern latitudes (< -TOL).
  ScopedPj nhemi(
      proj_create(ctx.get(), "+proj=peirce_q +shape=nhemisphere +R=1"));
  ASSERT_NE(nullptr, nhemi.get());
  const PJ_COORD n_ok = proj_trans(
      nhemi.get(), PJ_FWD, proj_coord(DegToRad(45), DegToRad(45), 0, 0));
  EXPECT_EQ(0, proj_errno(nhemi.get()));
  EXPECT_FALSE(std::isinf(n_ok.xy.x));
  const PJ_COORD n_bad =
      proj_trans(nhemi.get(), PJ_FWD, proj_coord(0.0, DegToRad(-10), 0, 0));
  EXPECT_TRUE(std::isinf(n_bad.xy.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(nhemi.get()));
  proj_errno_reset(nhemi.get());

  // 2. shemisphere requires phi <= -TOL (even phi == 0 is rejected).
  ScopedPj shemi(
      proj_create(ctx.get(), "+proj=peirce_q +shape=shemisphere +R=1"));
  ASSERT_NE(nullptr, shemi.get());
  const PJ_COORD s_ok = proj_trans(
      shemi.get(), PJ_FWD, proj_coord(DegToRad(-45), DegToRad(-45), 0, 0));
  EXPECT_EQ(0, proj_errno(shemi.get()));
  EXPECT_FALSE(std::isinf(s_ok.xy.x));
  for (double bad_lat_deg : {0.0, 10.0}) {
    const PJ_COORD s_bad = proj_trans(
        shemi.get(), PJ_FWD, proj_coord(0.0, DegToRad(bad_lat_deg), 0, 0));
    EXPECT_TRUE(std::isinf(s_bad.xy.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(shemi.get()));
    proj_errno_reset(shemi.get());
  }

  // 3. horizontal with and without +scrollx (both positive and negative wraps).
  ScopedPj horiz(
      proj_create(ctx.get(), "+proj=peirce_q +shape=horizontal +R=1"));
  ScopedPj horiz_pos(proj_create(
      ctx.get(), "+proj=peirce_q +shape=horizontal +scrollx=0.5 +R=1"));
  ScopedPj horiz_neg(proj_create(
      ctx.get(), "+proj=peirce_q +shape=horizontal +scrollx=-0.5 +R=1"));
  ASSERT_NE(nullptr, horiz.get());
  ASSERT_NE(nullptr, horiz_pos.get());
  ASSERT_NE(nullptr, horiz_neg.get());

  const PJ_COORD h_south = proj_coord(DegToRad(-90.0), DegToRad(-45.0), 0, 0);
  const PJ_COORD h_s0 = proj_trans(horiz.get(), PJ_FWD, h_south);
  const PJ_COORD h_sp = proj_trans(horiz_pos.get(), PJ_FWD, h_south);
  EXPECT_EQ(0, proj_errno(horiz.get()));
  EXPECT_EQ(0, proj_errno(horiz_pos.get()));
  // (-90, -45) wraps across +2*kEllipK so net shift is -2*kEllipK.
  EXPECT_NEAR(h_s0.xy.x - 2.0 * kEllipK, h_sp.xy.x, 1e-6);

  const PJ_COORD h_north = proj_coord(0.0, DegToRad(45.0), 0, 0);
  const PJ_COORD h_np = proj_trans(horiz_pos.get(), PJ_FWD, h_north);
  const PJ_COORD h_nn = proj_trans(horiz_neg.get(), PJ_FWD, h_north);
  EXPECT_EQ(0, proj_errno(horiz_pos.get()));
  EXPECT_EQ(0, proj_errno(horiz_neg.get()));
  // Half-width scroll (+0.5 unwrapped vs -0.5 wrapped) lands at same x.
  EXPECT_NEAR(h_np.xy.x, h_nn.xy.x, 1e-6);

  // 4. vertical with and without +scrolly (both positive and negative wraps).
  ScopedPj vert(proj_create(ctx.get(), "+proj=peirce_q +shape=vertical +R=1"));
  ScopedPj vert_pos(proj_create(
      ctx.get(), "+proj=peirce_q +shape=vertical +scrolly=0.5 +R=1"));
  ScopedPj vert_neg(proj_create(
      ctx.get(), "+proj=peirce_q +shape=vertical +scrolly=-0.5 +R=1"));
  ASSERT_NE(nullptr, vert.get());
  ASSERT_NE(nullptr, vert_pos.get());
  ASSERT_NE(nullptr, vert_neg.get());

  const PJ_COORD v_south = proj_coord(0.0, DegToRad(-45.0), 0, 0);
  const PJ_COORD v_s0 = proj_trans(vert.get(), PJ_FWD, v_south);
  const PJ_COORD v_sp = proj_trans(vert_pos.get(), PJ_FWD, v_south);
  EXPECT_EQ(0, proj_errno(vert.get()));
  EXPECT_EQ(0, proj_errno(vert_pos.get()));
  EXPECT_NEAR(v_s0.xy.y - 2.0 * kEllipK, v_sp.xy.y, 1e-6);

  const PJ_COORD v_north = proj_coord(DegToRad(90.0), DegToRad(45.0), 0, 0);
  const PJ_COORD v_np = proj_trans(vert_pos.get(), PJ_FWD, v_north);
  const PJ_COORD v_nn = proj_trans(vert_neg.get(), PJ_FWD, v_north);
  EXPECT_EQ(0, proj_errno(vert_pos.get()));
  EXPECT_EQ(0, proj_errno(vert_neg.get()));
  EXPECT_NEAR(v_np.xy.y, v_nn.xy.y, 1e-6);
}

TEST(AdamsTest, ParameterValidationFailuresAndLogMessages) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  struct InvalidCase {
    const char* def;
    int expected_errno;
    const char* expected_log;
  };
  constexpr InvalidCase kCases[] = {
      {.def = "+proj=peirce_q +shape=triangle +R=1",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "peirce_q: invalid value for 'shape' parameter"},
      {.def = "+proj=peirce_q +shape=horizontal +scrollx=1.5 +R=1",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for scrollx"},
      {.def = "+proj=peirce_q +shape=horizontal +scrollx=-1.5 +R=1",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for scrollx"},
      {.def = "+proj=peirce_q +shape=vertical +scrolly=1.5 +R=1",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for scrolly"},
      {.def = "+proj=peirce_q +shape=vertical +scrolly=-1.5 +R=1",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for scrolly"},
  };

  for (const auto& tc : kCases) {
    capture.Clear();
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    EXPECT_EQ(nullptr, pj.get()) << tc.def;
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get())) << tc.def;
    EXPECT_TRUE(capture.ContainsMessage(tc.expected_log)) << tc.def;
  }

  ScopedPj empty_pj(proj_create(ctx.get(), ""));
  EXPECT_EQ(nullptr, empty_pj.get());
  EXPECT_NE(0, proj_context_errno(ctx.get()));

  ScopedPj null_pj(proj_create(ctx.get(), nullptr));
  EXPECT_EQ(nullptr, null_pj.get());
  EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_context_errno(ctx.get()));
}

TEST(AdamsTest, ProjFactorsConformalityCheck) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr const char* kDefs[] = {
      "+proj=guyou +R=1",
      "+proj=adams_hemi +R=1",
      "+proj=adams_ws1 +R=1",
      "+proj=adams_ws2 +R=1",
      "+proj=peirce_q +shape=square +R=1",
  };
  const PJ_COORD pt = proj_coord(DegToRad(15.0), DegToRad(25.0), 0.0, 0.0);

  for (const char* def : kDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    const PJ_FACTORS f = proj_factors(pj.get(), pt);
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    // Chebyshev ell_int_5 precision (~1e-7) limits derivative accuracy.
    EXPECT_NEAR(f.meridional_scale, f.parallel_scale, 1e-5) << def;
    EXPECT_NEAR(f.tissot_semimajor, f.tissot_semiminor, 1e-5) << def;
    EXPECT_NEAR(0.0, f.angular_distortion, 1e-5) << def;
    EXPECT_NEAR(kHalfPi, f.meridian_parallel_angle, 1e-5) << def;
  }
}

TEST(AdamsTest, Transform3D4DArrayGenericAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj ws2(proj_create(ctx.get(), "+proj=adams_ws2 +R=6371000"));
  ScopedPj pq_sq(proj_create(ctx.get(), "+proj=peirce_q +shape=square +R=1"));
  ScopedPj guyou(proj_create(ctx.get(), "+proj=guyou +R=1"));
  ASSERT_NE(nullptr, ws2.get());
  ASSERT_NE(nullptr, pq_sq.get());
  ASSERT_NE(nullptr, guyou.get());

  // 1. 3D/4D z and t pass through FWD and INV unchanged.
  const PJ_COORD in4d =
      proj_coord(DegToRad(35.0), DegToRad(25.0), 456.75, 2026.25);
  const PJ_COORD fwd4d = proj_trans(ws2.get(), PJ_FWD, in4d);
  EXPECT_EQ(0, proj_errno(ws2.get()));
  EXPECT_DOUBLE_EQ(456.75, fwd4d.xyzt.z);
  EXPECT_DOUBLE_EQ(2026.25, fwd4d.xyzt.t);
  const PJ_COORD inv4d = proj_trans(ws2.get(), PJ_INV, fwd4d);
  EXPECT_EQ(0, proj_errno(ws2.get()));
  EXPECT_NEAR(in4d.lpzt.lam, inv4d.lpzt.lam, 1e-8);
  EXPECT_NEAR(in4d.lpzt.phi, inv4d.lpzt.phi, 1e-8);
  EXPECT_DOUBLE_EQ(456.75, inv4d.lpzt.z);
  EXPECT_DOUBLE_EQ(2026.25, inv4d.lpzt.t);

  // 2. PJ_IDENT and nullptr PJ* pass coordinates through unchanged.
  const PJ_COORD ident = proj_trans(ws2.get(), PJ_IDENT, in4d);
  EXPECT_DOUBLE_EQ(in4d.xyzt.x, ident.xyzt.x);
  const PJ_COORD null_tr = proj_trans(nullptr, PJ_FWD, in4d);
  EXPECT_DOUBLE_EQ(in4d.xyzt.x, null_tr.xyzt.x);

  // 3. proj_trans_array round-trip and mixed-validity error handling.
  std::array<PJ_COORD, 3> batch = {
      proj_coord(DegToRad(15.0), DegToRad(20.0), 1.0, 2020.0),
      proj_coord(DegToRad(-40.0), DegToRad(35.0), 2.0, 2021.0),
      proj_coord(DegToRad(60.0), DegToRad(-30.0), 3.0, 2022.0),
  };
  const auto orig_batch = batch;
  ASSERT_EQ(0, proj_trans_array(ws2.get(), PJ_FWD, batch.size(), batch.data()));
  ASSERT_EQ(0, proj_trans_array(ws2.get(), PJ_INV, batch.size(), batch.data()));
  for (size_t i = 0; i < batch.size(); ++i) {
    EXPECT_NEAR(orig_batch[i].lpzt.lam, batch[i].lpzt.lam, 1e-8);
    EXPECT_NEAR(orig_batch[i].lpzt.phi, batch[i].lpzt.phi, 1e-8);
  }

  std::array<PJ_COORD, 2> mixed = {
      proj_coord(DegToRad(15.0), DegToRad(20.0), 0.0, 0.0),
      proj_coord(DegToRad(100.0), DegToRad(0.0), 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_trans_array(guyou.get(), PJ_FWD, mixed.size(), mixed.data()));
  EXPECT_FALSE(std::isinf(mixed[0].xy.x));
  EXPECT_TRUE(std::isinf(mixed[1].xy.x));
  proj_errno_reset(guyou.get());

  // 4. proj_trans_generic round-trip on peirce_q square.
  std::array<double, 2> x = {DegToRad(20.0), DegToRad(-110.0)};
  std::array<double, 2> y = {DegToRad(30.0), DegToRad(-45.0)};
  const auto orig_x = x;
  const auto orig_y = y;
  ASSERT_EQ(2u,
            proj_trans_generic(pq_sq.get(), PJ_FWD, x.data(), sizeof(double),
                               x.size(), y.data(), sizeof(double), y.size(),
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pq_sq.get()));
  ASSERT_EQ(2u,
            proj_trans_generic(pq_sq.get(), PJ_INV, x.data(), sizeof(double),
                               x.size(), y.data(), sizeof(double), y.size(),
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pq_sq.get()));
  for (size_t i = 0; i < 2; ++i) {
    EXPECT_NEAR(orig_x[i], x[i], 1e-8);
    EXPECT_NEAR(orig_y[i], y[i], 1e-8);
  }

  // 5. NaN and +/-Inf edge inputs.
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const double kInf = std::numeric_limits<double>::infinity();
  const PJ_COORD nan_out =
      proj_trans(ws2.get(), PJ_FWD, proj_coord(kNaN, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.xy.x));
  EXPECT_TRUE(std::isnan(nan_out.xy.y));

  const PJ_COORD err_out = proj_trans(ws2.get(), PJ_FWD, proj_coord_error());
  EXPECT_TRUE(std::isinf(err_out.xy.x));
  proj_errno_reset(ws2.get());

  const PJ_COORD inf_lon =
      proj_trans(ws2.get(), PJ_FWD, proj_coord(kInf, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_lon.xy.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(ws2.get()));
  proj_errno_reset(ws2.get());

  const PJ_COORD neg_inf_lat =
      proj_trans(ws2.get(), PJ_FWD, proj_coord(0.0, -kInf, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(neg_inf_lat.xy.y));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(ws2.get()));
  proj_errno_reset(ws2.get());
}

}  // namespace
}  // namespace proj
