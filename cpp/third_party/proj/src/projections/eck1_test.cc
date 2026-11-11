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
// Unit tests for the Eckert I pseudocylindrical projection
// (proj/src/projections/eck1.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/eck1.cpp

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
constexpr double kFc = 0.92131773192356127802;
constexpr double kRp = 0.31830988618379067154;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Eck1Test, MetadataAndForcedSphericalSetup) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_ellps(proj_create(ctx.get(), "+proj=eck1 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_ellps.get());
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj_ellps.get());
  EXPECT_STREQ("eck1", info.id);
  EXPECT_NE(std::string_view(pj_ellps->descr).find("Eckert I"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj_ellps.get()));

  // PJ_PROJECTION(eck1) sets P->es = 0.0, so an ellipsoidal setup matches a
  // sphere with the same semimajor axis.
  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=eck1 +a=6378137.0"));
  ASSERT_NE(nullptr, pj_sphere.get());

  const PJ_COORD in = proj_coord(DegToRad(50.0), DegToRad(-30.0), 0.0, 0.0);
  const PJ_COORD out_ellps = proj_trans(pj_ellps.get(), PJ_FWD, in);
  const PJ_COORD out_sphere = proj_trans(pj_sphere.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(out_sphere.xy.x, out_ellps.xy.x, 1e-9);
  EXPECT_NEAR(out_sphere.xy.y, out_ellps.xy.y, 1e-9);
}

TEST(Eck1Test, ForwardAnalyticalFormulaAndPoleHalfLengthOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=eck1 +R=1"));
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
      {.lon_rad = kPi, .lat_rad = kHalfPi},
      {.lon_rad = -kPi, .lat_rad = -kHalfPi},
  };

  for (const auto& tc : kCases) {
    const double expected_x =
        kFc * tc.lon_rad * (1.0 - kRp * std::abs(tc.lat_rad));
    const double expected_y = kFc * tc.lat_rad;

    const PJ_COORD out =
        proj_trans(pj.get(), PJ_FWD, proj_coord(tc.lon_rad, tc.lat_rad, 0, 0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected_x, out.xy.x, 1e-12);
    EXPECT_NEAR(expected_y, out.xy.y, 1e-12);
  }

  // On Eckert I, the pole lines (phi = +-pi/2) have half the length of the
  // equator (phi = 0) because 1 - (1/pi)*(pi/2) == 0.5.
  const PJ_COORD eq = proj_trans(pj.get(), PJ_FWD, proj_coord(1.0, 0.0, 0, 0));
  const PJ_COORD np =
      proj_trans(pj.get(), PJ_FWD, proj_coord(1.0, kHalfPi, 0, 0));
  const PJ_COORD sp =
      proj_trans(pj.get(), PJ_FWD, proj_coord(1.0, -kHalfPi, 0, 0));
  EXPECT_NEAR(0.5 * eq.xy.x, np.xy.x, 1e-12);
  EXPECT_NEAR(0.5 * eq.xy.x, sp.xy.x, 1e-12);
}

TEST(Eck1Test, ForwardAndInverseRoundTripAcrossQuadrantsAndPoles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=eck1 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 12.5, .lat_deg = 55.6},
      {.lon_deg = -95.0, .lat_deg = 42.0},
      {.lon_deg = 135.0, .lat_deg = -35.0},
      {.lon_deg = -70.0, .lat_deg = -50.0},
      // Because the pole is a line of half equatorial length, even +-90 deg
      // round-trips longitude and latitude cleanly in Eckert I.
      {.lon_deg = 120.0, .lat_deg = 90.0},
      {.lon_deg = -120.0, .lat_deg = -90.0},
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

TEST(Eck1Test, BatchTransformFactorsAndDomainErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(
      ctx.get(), "+proj=eck1 +R=6371000 +lon_0=-10 +x_0=30000 +y_0=40000"));
  ASSERT_NE(nullptr, pj.get());

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(-10.0), 0.0, 0.0, 0.0),
      proj_coord(DegToRad(30.0), DegToRad(-25.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  EXPECT_NEAR(30000.0, coords[0].xy.x, 1e-6);
  EXPECT_NEAR(40000.0, coords[0].xy.y, 1e-6);

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(DegToRad(-10.0), coords[0].lp.lam, 1e-12);
  EXPECT_NEAR(0.0, coords[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(30.0), coords[1].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-25.0), coords[1].lp.phi, 1e-12);

  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(DegToRad(20.0), DegToRad(30.0), 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(factors.meridional_scale, 0.0);
  EXPECT_GT(factors.parallel_scale, 0.0);
  EXPECT_GT(factors.areal_scale, 0.0);

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
