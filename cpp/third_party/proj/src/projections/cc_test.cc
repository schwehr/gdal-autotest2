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
// Unit tests for the Central Cylindrical perspective projection
// (proj/src/projections/cc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/cc.cpp

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

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(CcTest, MetadataAndForcedSphericalSetup) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_ellps(proj_create(ctx.get(), "+proj=cc +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_ellps.get());
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj_ellps.get());
  EXPECT_STREQ("cc", info.id);
  EXPECT_NE(std::string_view(pj_ellps->descr).find("Central Cylindrical"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj_ellps.get()));

  // PJ_PROJECTION(cc) sets P->es = 0.0, so an ellipsoidal setup matches a
  // sphere with the same semimajor axis.
  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=cc +a=6378137.0"));
  ASSERT_NE(nullptr, pj_sphere.get());

  const PJ_COORD in = proj_coord(DegToRad(25.0), DegToRad(-35.0), 0.0, 0.0);
  const PJ_COORD out_ellps = proj_trans(pj_ellps.get(), PJ_FWD, in);
  const PJ_COORD out_sphere = proj_trans(pj_sphere.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(out_sphere.xy.x, out_ellps.xy.x, 1e-9);
  EXPECT_NEAR(out_sphere.xy.y, out_ellps.xy.y, 1e-9);
}

TEST(CcTest, ForwardAnalyticalFormulaAndInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj_unit(proj_create(ctx.get(), "+proj=cc +R=1"));
  ASSERT_NE(nullptr, pj_unit.get());
  ScopedPj pj_wgs84(proj_create(ctx.get(), "+proj=cc +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_wgs84.get());

  struct AngleCase {
    double lon_rad;
    double lat_rad;
  };
  constexpr AngleCase kCases[] = {
      {.lon_rad = 0.0, .lat_rad = 0.0},
      {.lon_rad = DegToRad(60.0), .lat_rad = 0.0},
      {.lon_rad = 0.0, .lat_rad = kPi / 4.0},
      {.lon_rad = DegToRad(45.0), .lat_rad = DegToRad(30.0)},
      {.lon_rad = DegToRad(-120.0), .lat_rad = DegToRad(60.0)},
      {.lon_rad = DegToRad(90.0), .lat_rad = DegToRad(-45.0)},
      {.lon_rad = DegToRad(-75.0), .lat_rad = DegToRad(-70.0)},
  };

  for (const auto& tc : kCases) {
    // 1. Unit sphere analytical check: x = lam, y = tan(phi).
    const PJ_COORD unit_fwd = proj_trans(
        pj_unit.get(), PJ_FWD, proj_coord(tc.lon_rad, tc.lat_rad, 0, 0));
    EXPECT_EQ(0, proj_errno(pj_unit.get()));
    EXPECT_NEAR(tc.lon_rad, unit_fwd.xy.x, 1e-12);
    EXPECT_NEAR(std::tan(tc.lat_rad), unit_fwd.xy.y, 1e-12);

    const PJ_COORD unit_inv = proj_trans(pj_unit.get(), PJ_INV, unit_fwd);
    EXPECT_EQ(0, proj_errno(pj_unit.get()));
    EXPECT_NEAR(tc.lon_rad, unit_inv.lp.lam, 1e-12);
    EXPECT_NEAR(tc.lat_rad, unit_inv.lp.phi, 1e-12);

    // 2. Classic 2D pj_fwd / pj_inv on WGS84.
    const PJ_LP in_lp = {.lam = tc.lon_rad, .phi = tc.lat_rad};
    const PJ_XY xy = pj_fwd(in_lp, pj_wgs84.get());
    EXPECT_EQ(0, proj_errno(pj_wgs84.get()));
    const PJ_LP out_lp = pj_inv(xy, pj_wgs84.get());
    EXPECT_EQ(0, proj_errno(pj_wgs84.get()));
    EXPECT_NEAR(tc.lon_rad, out_lp.lam, 1e-12);
    EXPECT_NEAR(tc.lat_rad, out_lp.phi, 1e-12);
  }
}

TEST(CcTest, ForwardPoleSingularityWithinEps10ReturnsOutsideProjectionDomain) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=cc +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // cc_s_forward rejects latitudes where ||phi| - pi/2| <= EPS10 (1e-10) with
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  constexpr double kSingularLats[] = {
      kHalfPi,
      -kHalfPi,
      kHalfPi - 0.5e-10,
      -kHalfPi + 0.5e-10,
  };
  for (const double lat : kSingularLats) {
    const PJ_COORD out =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.25, lat, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, out.xy.x);
    EXPECT_EQ(HUGE_VAL, out.xy.y);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
  }

  // Just outside the EPS10 threshold (|phi| = pi/2 - 2e-10), forward succeeds.
  const double near_pole_lat = kHalfPi - 2.0e-10;
  const PJ_COORD valid =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.25, near_pole_lat, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.25, valid.xy.x, 1e-12);
  EXPECT_NEAR(std::tan(near_pole_lat), valid.xy.y, 1e-2);
}

TEST(CcTest, BatchTransformAndAnalyticalScaleFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(
      ctx.get(), "+proj=cc +R=6371000 +lon_0=15 +x_0=20000 +y_0=-10000"));
  ASSERT_NE(nullptr, pj.get());

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(15.0), 0.0, 0.0, 0.0),
      proj_coord(DegToRad(45.0), kPi / 4.0, 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  EXPECT_NEAR(20000.0, coords[0].xy.x, 1e-6);
  EXPECT_NEAR(-10000.0, coords[0].xy.y, 1e-6);

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(DegToRad(15.0), coords[0].lp.lam, 1e-12);
  EXPECT_NEAR(0.0, coords[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(45.0), coords[1].lp.lam, 1e-12);
  EXPECT_NEAR(kPi / 4.0, coords[1].lp.phi, 1e-12);

  // At phi = 45 deg on a sphere, Central Cylindrical has parallel scale
  // k = sec(45 deg) = sqrt(2) and meridional scale h = sec^2(45 deg) = 2.0.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(DegToRad(15.0), kPi / 4.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(std::sqrt(2.0), factors.parallel_scale, 1e-5);
  EXPECT_NEAR(2.0, factors.meridional_scale, 1e-5);
  EXPECT_NEAR(2.0 * std::sqrt(2.0), factors.areal_scale, 1e-5);
}

}  // namespace
}  // namespace proj
