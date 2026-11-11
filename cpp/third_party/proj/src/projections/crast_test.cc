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
// Unit tests for the Craster Parabolic (Putnins P4) pseudocylindrical
// projection (proj/src/projections/crast.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/crast.cpp

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
constexpr double kXm = 0.97720502380583984317;
constexpr double kRxm = 1.02332670794648848847;
constexpr double kYm = 3.06998012383946546542;
constexpr double kRym = 0.32573500793527994772;
constexpr double kThird = 0.333333333333333333;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(CrastTest, MetadataAndForcedSphericalSetup) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_ellps(proj_create(ctx.get(), "+proj=crast +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_ellps.get());
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj_ellps.get());
  EXPECT_STREQ("crast", info.id);
  EXPECT_NE(
      std::string_view(pj_ellps->descr).find("Craster Parabolic (Putnins P4)"),
      std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj_ellps.get()));

  // PJ_PROJECTION(crast) sets P->es = 0.0, so an ellipsoidal definition uses
  // the spherical equations scaled by the semimajor axis.
  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=crast +a=6378137.0"));
  ASSERT_NE(nullptr, pj_sphere.get());

  const PJ_COORD in = proj_coord(DegToRad(-65.0), DegToRad(48.0), 0.0, 0.0);
  const PJ_COORD out_ellps = proj_trans(pj_ellps.get(), PJ_FWD, in);
  const PJ_COORD out_sphere = proj_trans(pj_sphere.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(out_sphere.xy.x, out_ellps.xy.x, 1e-9);
  EXPECT_NEAR(out_sphere.xy.y, out_ellps.xy.y, 1e-9);
}

TEST(CrastTest, ForwardAnalyticalFormulaAndPointedPolesOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=crast +R=1"));
  ASSERT_NE(nullptr, pj.get());

  struct AngleCase {
    double lon_rad;
    double lat_rad;
  };
  constexpr AngleCase kCases[] = {
      {.lon_rad = 0.0, .lat_rad = 0.0},
      {.lon_rad = DegToRad(90.0), .lat_rad = 0.0},
      {.lon_rad = 0.0, .lat_rad = DegToRad(60.0)},
      {.lon_rad = DegToRad(45.0), .lat_rad = DegToRad(30.0)},
      {.lon_rad = DegToRad(-120.0), .lat_rad = DegToRad(45.0)},
      {.lon_rad = DegToRad(75.0), .lat_rad = DegToRad(-50.0)},
      {.lon_rad = DegToRad(-150.0), .lat_rad = DegToRad(-35.0)},
      // At the poles (+-pi/2), 2 * cos(+-pi/3) - 1 == 0, so meridians meet at
      // a point (x = 0, y = +-0.5 * YM).
      {.lon_rad = DegToRad(120.0), .lat_rad = kHalfPi},
      {.lon_rad = DegToRad(-120.0), .lat_rad = -kHalfPi},
  };

  for (const auto& tc : kCases) {
    const double phi_third = tc.lat_rad * kThird;
    const double expected_x =
        kXm * tc.lon_rad * (2.0 * std::cos(2.0 * phi_third) - 1.0);
    const double expected_y = kYm * std::sin(phi_third);

    const PJ_COORD out =
        proj_trans(pj.get(), PJ_FWD, proj_coord(tc.lon_rad, tc.lat_rad, 0, 0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected_x, out.xy.x, 1e-12);
    EXPECT_NEAR(expected_y, out.xy.y, 1e-12);
  }
}

TEST(CrastTest, ForwardAndInverseRoundTripAcrossQuadrants) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj_unit(proj_create(ctx.get(), "+proj=crast +R=1"));
  ASSERT_NE(nullptr, pj_unit.get());
  ScopedPj pj_wgs84(proj_create(ctx.get(), "+proj=crast +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_wgs84.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 20.0, .lat_deg = 40.0},
      {.lon_deg = -110.0, .lat_deg = 55.0},
      {.lon_deg = 140.0, .lat_deg = -25.0},
      {.lon_deg = -45.0, .lat_deg = -65.0},
      {.lon_deg = 179.0, .lat_deg = 80.0},
  };

  for (const auto& tc : kCases) {
    const PJ_LP in_lp = {.lam = DegToRad(tc.lon_deg),
                         .phi = DegToRad(tc.lat_deg)};

    // 1. Unit-sphere direct formula check for crast_s_inverse.
    const PJ_XY unit_xy = pj_fwd(in_lp, pj_unit.get());
    EXPECT_EQ(0, proj_errno(pj_unit.get()));
    const double expected_phi = 3.0 * std::asin(unit_xy.y * kRym);
    const double expected_lam =
        unit_xy.x * kRxm / (2.0 * std::cos(2.0 * expected_phi * kThird) - 1.0);
    const PJ_LP unit_lp = pj_inv(unit_xy, pj_unit.get());
    EXPECT_EQ(0, proj_errno(pj_unit.get()));
    EXPECT_NEAR(expected_lam, unit_lp.lam, 1e-12);
    EXPECT_NEAR(expected_phi, unit_lp.phi, 1e-12);

    // 2. WGS84 4D proj_trans round-trip.
    const PJ_COORD fwd = proj_trans(pj_wgs84.get(), PJ_FWD,
                                    proj_coord(in_lp.lam, in_lp.phi, 0, 0));
    EXPECT_EQ(0, proj_errno(pj_wgs84.get()));
    const PJ_COORD inv = proj_trans(pj_wgs84.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj_wgs84.get()));
    EXPECT_NEAR(in_lp.lam, inv.lp.lam, 1e-12);
    EXPECT_NEAR(in_lp.phi, inv.lp.phi, 1e-12);
  }
}

TEST(CrastTest, EqualAreaFactorsAndBatchTransform) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(
      ctx.get(), "+proj=crast +R=6371000 +lon_0=-20 +x_0=10000 +y_0=20000"));
  ASSERT_NE(nullptr, pj.get());

  // Craster Parabolic is an equal-area projection on the sphere
  // (areal_scale=1).
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(DegToRad(15.0), DegToRad(35.0), 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-5);
  EXPECT_GT(factors.meridional_scale, 0.0);
  EXPECT_GT(factors.parallel_scale, 0.0);

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(-20.0), 0.0, 0.0, 0.0),
      proj_coord(DegToRad(10.0), DegToRad(-30.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  EXPECT_NEAR(10000.0, coords[0].xy.x, 1e-6);
  EXPECT_NEAR(20000.0, coords[0].xy.y, 1e-6);

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(DegToRad(-20.0), coords[0].lp.lam, 1e-12);
  EXPECT_NEAR(0.0, coords[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(10.0), coords[1].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-30.0), coords[1].lp.phi, 1e-12);

  // Latitude > pi/2 is rejected by fwd_prepare.
  ScopedLogSuppressor suppress_logs(ctx.get());
  const PJ_COORD bad =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, kHalfPi + 0.1, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
}

}  // namespace
}  // namespace proj
