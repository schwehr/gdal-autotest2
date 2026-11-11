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
// Unit tests for the Denoyer Semi-Elliptical pseudocylindrical projection
// (proj/src/projections/denoy.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/denoy.cpp

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

constexpr double kC0 = 0.95;
constexpr double kC1 = -0.08333333333333333333;
constexpr double kC3 = 0.00166666666666666666;
constexpr double kD1 = 0.9;
constexpr double kD5 = 0.03;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(DenoyTest, MetadataForcedSphericalAndNoInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_ellps(proj_create(ctx.get(), "+proj=denoy +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_ellps.get());
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj_ellps.get());
  EXPECT_STREQ("denoy", info.id);
  EXPECT_NE(std::string_view(pj_ellps->descr).find("Denoyer Semi-Elliptical"),
            std::string_view::npos);
  EXPECT_EQ(0, info.has_inverse);
  EXPECT_EQ(0, pj_has_inverse(pj_ellps.get()));

  // PJ_PROJECTION(denoy) sets P->es = 0.0, forcing spherical computation.
  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=denoy +a=6378137.0"));
  ASSERT_NE(nullptr, pj_sphere.get());

  const PJ_COORD in = proj_coord(DegToRad(-55.0), DegToRad(40.0), 0.0, 0.0);
  const PJ_COORD out_ellps = proj_trans(pj_ellps.get(), PJ_FWD, in);
  const PJ_COORD out_sphere = proj_trans(pj_sphere.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(out_sphere.xy.x, out_ellps.xy.x, 1e-9);
  EXPECT_NEAR(out_sphere.xy.y, out_ellps.xy.y, 1e-9);

  // Denoyer Semi-Elliptical has no inverse operation (P->inv == nullptr).
  ScopedLogSuppressor suppress_logs(ctx.get());
  const PJ_COORD inv = proj_trans(pj_ellps.get(), PJ_INV, out_ellps);
  EXPECT_EQ(HUGE_VAL, inv.lp.lam);
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(pj_ellps.get()));
  proj_errno_reset(pj_ellps.get());

  const PJ_LP inv2d = pj_inv(out_ellps.xy, pj_ellps.get());
  EXPECT_EQ(HUGE_VAL, inv2d.lam);
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(pj_ellps.get()));
  proj_errno_reset(pj_ellps.get());
}

TEST(DenoyTest, ForwardAnalyticalFormulaOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=denoy +R=1"));
  ASSERT_NE(nullptr, pj.get());

  struct AngleCase {
    double lon_rad;
    double lat_rad;
  };
  constexpr AngleCase kCases[] = {
      {.lon_rad = 0.0, .lat_rad = 0.0},
      // Equator (phi == 0): cos(0) == 1, so x = lam and y = 0.
      {.lon_rad = DegToRad(75.0), .lat_rad = 0.0},
      {.lon_rad = DegToRad(-75.0), .lat_rad = 0.0},
      // Prime meridian (lam == 0): x = 0 and y = phi.
      {.lon_rad = 0.0, .lat_rad = DegToRad(50.0)},
      // General interior and boundary points across all four quadrants.
      {.lon_rad = DegToRad(45.0), .lat_rad = DegToRad(30.0)},
      {.lon_rad = DegToRad(-120.0), .lat_rad = DegToRad(60.0)},
      {.lon_rad = DegToRad(90.0), .lat_rad = DegToRad(-40.0)},
      {.lon_rad = DegToRad(-75.0), .lat_rad = DegToRad(-25.0)},
      {.lon_rad = kPi, .lat_rad = kHalfPi},
      {.lon_rad = -kPi, .lat_rad = -kHalfPi},
  };

  for (const auto& tc : kCases) {
    const double abs_lam = std::abs(tc.lon_rad);
    const double phi = tc.lat_rad;
    const double phi4 = phi * phi * phi * phi;
    const double expected_x =
        tc.lon_rad *
        std::cos((kC0 + abs_lam * (kC1 + abs_lam * abs_lam * kC3)) *
                 (phi * (kD1 + kD5 * phi4)));
    const double expected_y = phi;

    const PJ_COORD out =
        proj_trans(pj.get(), PJ_FWD, proj_coord(tc.lon_rad, tc.lat_rad, 0, 0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected_x, out.xy.x, 1e-12);
    EXPECT_NEAR(expected_y, out.xy.y, 1e-12);
  }
}

TEST(DenoyTest, QuadrantSymmetryAndBatchTransform) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  constexpr double kRadius = 6371000.0;
  ScopedPj pj(proj_create(ctx.get(), "+proj=denoy +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  const double lam = DegToRad(65.0);
  const double phi = DegToRad(40.0);
  const PJ_XY ne = pj_fwd({.lam = lam, .phi = phi}, pj.get());
  const PJ_XY nw = pj_fwd({.lam = -lam, .phi = phi}, pj.get());
  const PJ_XY se = pj_fwd({.lam = lam, .phi = -phi}, pj.get());
  const PJ_XY sw = pj_fwd({.lam = -lam, .phi = -phi}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  // x is odd in lam and even in phi (since cos(...) is even in phi);
  // y = R * phi is independent of lam and odd in phi.
  EXPECT_NEAR(-ne.x, nw.x, 1e-9);
  EXPECT_NEAR(ne.x, se.x, 1e-9);
  EXPECT_NEAR(-ne.x, sw.x, 1e-9);
  EXPECT_NEAR(kRadius * phi, ne.y, 1e-9);
  EXPECT_NEAR(kRadius * phi, nw.y, 1e-9);
  EXPECT_NEAR(-kRadius * phi, se.y, 1e-9);
  EXPECT_NEAR(-kRadius * phi, sw.y, 1e-9);

  PJ_COORD coords[2] = {
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(lam, phi, 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  EXPECT_DOUBLE_EQ(0.0, coords[0].xy.x);
  EXPECT_DOUBLE_EQ(0.0, coords[0].xy.y);
  EXPECT_NEAR(ne.x, coords[1].xy.x, 1e-9);
  EXPECT_NEAR(ne.y, coords[1].xy.y, 1e-9);

  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(lam, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(factors.meridional_scale, 0.0);
  EXPECT_GT(factors.parallel_scale, 0.0);
  EXPECT_GT(factors.areal_scale, 0.0);
}

}  // namespace
}  // namespace proj
