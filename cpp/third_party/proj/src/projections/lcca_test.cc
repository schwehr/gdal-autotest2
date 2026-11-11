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
// Unit tests for the Lambert Conformal Conic Alternative projection
// (proj/src/projections/lcca.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/lcca.cpp

#include <cmath>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

C_NAMESPACE PJ* pj_lcca(PJ*);

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(LccaTest, MissingOrZeroLat0FailsWithIllegalArgValue) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get());

  // 1. Default lat_0 is 0, which is rejected by lcca.
  ScopedPj missing_lat0(proj_create(ctx.get(), "+proj=lcca +ellps=WGS84"));
  EXPECT_EQ(nullptr, missing_lat0.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(
      logs.ContainsMessage("Invalid value for lat_0: it should be different"));

  // 2. Explicit +lat_0=0 is likewise rejected.
  proj_context_errno_set(ctx.get(), 0);
  logs.Clear();
  ScopedPj zero_lat0(
      proj_create(ctx.get(), "+proj=lcca +lat_0=0 +ellps=WGS84"));
  EXPECT_EQ(nullptr, zero_lat0.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
}

TEST(LccaTest, EllipsoidalForwardInverseRoundTripNorthernAndSouthern) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj_north(proj_create(
      ctx.get(),
      "+proj=lcca +lat_0=35 +lon_0=5 +k_0=0.9996 +x_0=500000 +y_0=300000 "
      "+ellps=clrk80"));
  ASSERT_NE(nullptr, pj_north.get());
  EXPECT_EQ(1, pj_has_inverse(pj_north.get()));
  EXPECT_STREQ("lcca", proj_pj_info(pj_north.get()).id);

  // 1. Origin (lon_0, lat_0) maps to false origin (x_0, y_0).
  const PJ_COORD origin = proj_coord(DegToRad(5.0), DegToRad(35.0), 0.0, 0.0);
  const PJ_COORD origin_fwd = proj_trans(pj_north.get(), PJ_FWD, origin);
  EXPECT_EQ(0, proj_errno(pj_north.get()));
  EXPECT_NEAR(500000.0, origin_fwd.xy.x, 1e-6);
  EXPECT_NEAR(300000.0, origin_fwd.xy.y, 1e-6);

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kNorthCases[] = {
      {.lon_deg = 5.0, .lat_deg = 35.0},  {.lon_deg = 3.0, .lat_deg = 33.5},
      {.lon_deg = 8.5, .lat_deg = 36.8},  {.lon_deg = 0.0, .lat_deg = 31.0},
      {.lon_deg = 10.0, .lat_deg = 38.0},
  };

  for (const auto& tc : kNorthCases) {
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 100.0, 2026.0);
    const PJ_COORD fwd = proj_trans(pj_north.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj_north.get()));
    const PJ_COORD inv = proj_trans(pj_north.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj_north.get()));
    EXPECT_NEAR(in.lpzt.lam, inv.lpzt.lam, 1e-10);
    EXPECT_NEAR(in.lpzt.phi, inv.lpzt.phi, 1e-10);
    EXPECT_NEAR(in.lpzt.z, inv.lpzt.z, 1e-9);
    EXPECT_NEAR(in.lpzt.t, inv.lpzt.t, 1e-9);
  }

  // 2. Low-latitude northern cone (lat_0 = 15 deg) round-trips coordinates
  // extending across the equator into the southern hemisphere (where S < 0 and
  // dr < 0 while r = r0 - dr > 0).
  ScopedPj pj_low_lat(
      proj_create(ctx.get(), "+proj=lcca +lat_0=15 +lon_0=20 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_low_lat.get());
  const PJ_COORD south_in =
      proj_coord(DegToRad(22.5), DegToRad(-5.0), 0.0, 0.0);
  const PJ_COORD south_fwd = proj_trans(pj_low_lat.get(), PJ_FWD, south_in);
  EXPECT_EQ(0, proj_errno(pj_low_lat.get()));
  const PJ_COORD south_inv = proj_trans(pj_low_lat.get(), PJ_INV, south_fwd);
  EXPECT_EQ(0, proj_errno(pj_low_lat.get()));
  EXPECT_NEAR(south_in.lp.lam, south_inv.lp.lam, 1e-10);
  EXPECT_NEAR(south_in.lp.phi, south_inv.lp.phi, 1e-10);

  // 3. Negative lat_0 is accepted by the constructor and maps (lon_0, lat_0)
  // to the projection origin (0, 0) in the forward direction.
  ScopedPj pj_south(
      proj_create(ctx.get(), "+proj=lcca +lat_0=-32 +lon_0=20 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_south.get());
  const PJ_COORD south_origin_fwd =
      proj_trans(pj_south.get(), PJ_FWD,
                 proj_coord(DegToRad(20.0), DegToRad(-32.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_south.get()));
  EXPECT_NEAR(0.0, south_origin_fwd.xy.x, 1e-9);
  EXPECT_NEAR(0.0, south_origin_fwd.xy.y, 1e-9);
}

TEST(LccaTest, SphericalRoundTripSymmetryTransArrayAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=lcca +lat_0=45 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  // East-west symmetry across central meridian.
  const PJ_COORD east = proj_coord(DegToRad(4.0), DegToRad(46.0), 0.0, 0.0);
  const PJ_COORD west = proj_coord(DegToRad(-4.0), DegToRad(46.0), 0.0, 0.0);
  const PJ_COORD fwd_east = proj_trans(pj.get(), PJ_FWD, east);
  const PJ_COORD fwd_west = proj_trans(pj.get(), PJ_FWD, west);
  EXPECT_NEAR(-fwd_east.xy.x, fwd_west.xy.x, 1e-6);
  EXPECT_NEAR(fwd_east.xy.y, fwd_west.xy.y, 1e-6);

  PJ_COORD arr[2] = {east, west};
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, arr));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, arr));
  EXPECT_NEAR(east.lp.lam, arr[0].lp.lam, 1e-10);
  EXPECT_NEAR(east.lp.phi, arr[0].lp.phi, 1e-10);
  EXPECT_NEAR(west.lp.lam, arr[1].lp.lam, 1e-10);
  EXPECT_NEAR(west.lp.phi, arr[1].lp.phi, 1e-10);

  // At the standard parallel (lat_0 = 45 deg), both scale factors are 1.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-6);
}

TEST(LccaTest, InverseNonConvergenceSetsOutsideProjectionDomainError) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=lcca +lat_0=35 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // A huge normalized y (-1e20) causes the 10-iteration cubic Newton-Raphson
  // solver for S in lcca_e_inverse to exhaust MAX_ITER (!i), setting
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  (void)pj_inv({.x = 0.0, .y = -1e20}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));

  proj_errno_reset(pj.get());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  (void)pj_inv({.x = nan, .y = nan}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
}

TEST(LccaTest, ConstructorAndDestructorEdgeCases) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=lcca +lat_0=35 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  ASSERT_NE(nullptr, pj->destructor);

  // 1. Destructor with nullptr PJ* returns nullptr.
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));

  // 2. Direct pj_lcca(nullptr) allocates a skeleton PJ with null opaque;
  // passing it to pj_lcca_destructor exercises the nullptr == P->opaque branch.
  ScopedPj raw(pj_lcca(nullptr));
  ASSERT_NE(nullptr, raw.get());
  EXPECT_STREQ("lcca", raw->short_name);
  EXPECT_EQ(nullptr, raw->opaque);
  EXPECT_EQ(nullptr, pj->destructor(raw.release(), PROJ_ERR_OTHER));
}

}  // namespace
}  // namespace proj
