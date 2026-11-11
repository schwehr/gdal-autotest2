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
// Unit tests for the Miller Cylindrical projection
// (proj/src/projections/mill.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/mill.cpp

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
constexpr double kQuarterPi = kPi / 4.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(MillTest, MetadataAndForcedSphericalSetup) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_ellps(proj_create(ctx.get(), "+proj=mill +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_ellps.get());
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj_ellps.get());
  EXPECT_STREQ("mill", info.id);
  EXPECT_NE(std::string_view(pj_ellps->descr).find("Miller Cylindrical"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj_ellps.get()));

  // PJ_PROJECTION(mill) sets P->es = 0.0, so an ellipsoidal setup matches a
  // sphere with the same semimajor axis.
  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=mill +a=6378137.0"));
  ASSERT_NE(nullptr, pj_sphere.get());

  const PJ_COORD in = proj_coord(DegToRad(35.0), DegToRad(-45.0), 0.0, 0.0);
  const PJ_COORD out_ellps = proj_trans(pj_ellps.get(), PJ_FWD, in);
  const PJ_COORD out_sphere = proj_trans(pj_sphere.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(out_sphere.xy.x, out_ellps.xy.x, 1e-9);
  EXPECT_NEAR(out_sphere.xy.y, out_ellps.xy.y, 1e-9);
}

TEST(MillTest, ForwardAnalyticalFormulaAndFinitePolesOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=mill +R=1"));
  ASSERT_NE(nullptr, pj.get());

  struct AngleCase {
    double lon_rad;
    double lat_rad;
  };
  constexpr AngleCase kCases[] = {
      {.lon_rad = 0.0, .lat_rad = 0.0},
      {.lon_rad = DegToRad(90.0), .lat_rad = 0.0},
      {.lon_rad = 0.0, .lat_rad = DegToRad(45.0)},
      {.lon_rad = DegToRad(45.0), .lat_rad = DegToRad(30.0)},
      {.lon_rad = DegToRad(-120.0), .lat_rad = DegToRad(60.0)},
      {.lon_rad = DegToRad(90.0), .lat_rad = DegToRad(-40.0)},
      {.lon_rad = DegToRad(-75.0), .lat_rad = DegToRad(-25.0)},
      // Unlike Mercator, Miller Cylindrical scales latitude by 0.8 before
      // Mercator's log(tan(pi/4 + x/2)), so poles (+-pi/2) project to finite y.
      {.lon_rad = kPi, .lat_rad = kHalfPi},
      {.lon_rad = -kPi, .lat_rad = -kHalfPi},
  };

  for (const auto& tc : kCases) {
    const double expected_x = tc.lon_rad;
    const double expected_y =
        1.25 * std::log(std::tan(kQuarterPi + 0.4 * tc.lat_rad));

    const PJ_COORD out =
        proj_trans(pj.get(), PJ_FWD, proj_coord(tc.lon_rad, tc.lat_rad, 0, 0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected_x, out.xy.x, 1e-12);
    EXPECT_NEAR(expected_y, out.xy.y, 1e-12);
  }
}

TEST(MillTest, ForwardAndInverseRoundTripAcrossQuadrantsAndPoles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=mill +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 18.0, .lat_deg = 59.3},
      {.lon_deg = -122.4, .lat_deg = 37.8},
      {.lon_deg = 144.9, .lat_deg = -37.8},
      {.lon_deg = -43.2, .lat_deg = -22.9},
      {.lon_deg = 175.0, .lat_deg = 90.0},
      {.lon_deg = -175.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const PJ_LP in_lp = {.lam = DegToRad(tc.lon_deg),
                         .phi = DegToRad(tc.lat_deg)};
    // 1. Classic 2D pj_fwd / pj_inv round-trip.
    const PJ_XY xy = pj_fwd(in_lp, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_LP out_lp = pj_inv(xy, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in_lp.lam, out_lp.lam, 1e-12);
    EXPECT_NEAR(in_lp.phi, out_lp.phi, 1e-12);

    // 2. 4D proj_trans round-trip.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(in_lp.lam, in_lp.phi, 0, 0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in_lp.lam, inv.lp.lam, 1e-12);
    EXPECT_NEAR(in_lp.phi, inv.lp.phi, 1e-12);
  }
}

TEST(MillTest, BatchTransformAndAnalyticalScaleFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(
      ctx.get(), "+proj=mill +R=6371000 +lon_0=20 +x_0=15000 +y_0=-30000"));
  ASSERT_NE(nullptr, pj.get());

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(20.0), 0.0, 0.0, 0.0),
      proj_coord(DegToRad(50.0), DegToRad(40.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  EXPECT_NEAR(15000.0, coords[0].xy.x, 1e-6);
  EXPECT_NEAR(-30000.0, coords[0].xy.y, 1e-6);

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(DegToRad(20.0), coords[0].lp.lam, 1e-12);
  EXPECT_NEAR(0.0, coords[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(50.0), coords[1].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(40.0), coords[1].lp.phi, 1e-12);

  // On a sphere, Miller Cylindrical has parallel scale k = sec(phi) and
  // meridional scale h = sec(0.8 * phi).
  const double phi = DegToRad(50.0);
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(DegToRad(20.0), phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0 / std::cos(phi), factors.parallel_scale, 1e-5);
  EXPECT_NEAR(1.0 / std::cos(0.8 * phi), factors.meridional_scale, 1e-5);

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
