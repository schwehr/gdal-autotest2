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
// Unit tests for the Roussilhe Stereographic projection
// (proj/src/projections/rouss.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/rouss.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

C_NAMESPACE PJ* pj_rouss(PJ*);

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(RoussTest,
     EllipsoidalForwardInverseRoundTripNorthernSouthernAndEquatorial) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=rouss +lat_0=52 +lon_0=21 +k_0=0.9999 +x_0=500000 +y_0=600000 "
      "+ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_STREQ("rouss", proj_pj_info(pj.get()).id);

  // 1. Center (lon_0, lat_0) maps to false origin (x_0, y_0).
  const PJ_COORD center = proj_coord(DegToRad(21.0), DegToRad(52.0), 0.0, 0.0);
  const PJ_COORD center_fwd = proj_trans(pj.get(), PJ_FWD, center);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(500000.0, center_fwd.xy.x, 1e-6);
  EXPECT_NEAR(600000.0, center_fwd.xy.y, 1e-6);

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 21.0, .lat_deg = 52.0}, {.lon_deg = 19.0, .lat_deg = 50.5},
      {.lon_deg = 23.5, .lat_deg = 53.8}, {.lon_deg = 17.5, .lat_deg = 54.0},
      {.lon_deg = 24.0, .lat_deg = 49.5},
  };

  for (const auto& tc : kCases) {
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 250.0, 2026.5);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lpzt.lam, inv.lpzt.lam, 1e-8);
    EXPECT_NEAR(in.lpzt.phi, inv.lpzt.phi, 1e-8);
    EXPECT_NEAR(in.lpzt.z, inv.lpzt.z, 1e-9);
    EXPECT_NEAR(in.lpzt.t, inv.lpzt.t, 1e-9);
  }

  // 2. Southern hemisphere (lat_0 < 0) and equatorial (lat_0 = 0) setups.
  for (const char* def : {"+proj=rouss +lat_0=-35 +lon_0=140 +ellps=GRS80",
                          "+proj=rouss +lat_0=0 +lon_0=0 +ellps=intl"}) {
    ScopedPj p(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, p.get());
    const PJ_LP in_lp = {.lam = p->lam0 + DegToRad(2.0),
                         .phi = p->phi0 + DegToRad(1.5)};
    const PJ_XY xy = pj_fwd(in_lp, p.get());
    EXPECT_EQ(0, proj_errno(p.get()));
    const PJ_LP out_lp = pj_inv(xy, p.get());
    EXPECT_EQ(0, proj_errno(p.get()));
    EXPECT_NEAR(in_lp.lam, out_lp.lam, 1e-8);
    EXPECT_NEAR(in_lp.phi, out_lp.phi, 1e-8);
  }
}

TEST(RoussTest, SphericalSymmetryTransArrayAndConformalFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=rouss +lat_0=45 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_COORD east = proj_coord(DegToRad(3.0), DegToRad(46.0), 0.0, 0.0);
  const PJ_COORD west = proj_coord(DegToRad(-3.0), DegToRad(46.0), 0.0, 0.0);
  const PJ_COORD fwd_east = proj_trans(pj.get(), PJ_FWD, east);
  const PJ_COORD fwd_west = proj_trans(pj.get(), PJ_FWD, west);
  EXPECT_NEAR(-fwd_east.xy.x, fwd_west.xy.x, 1e-6);
  EXPECT_NEAR(fwd_east.xy.y, fwd_west.xy.y, 1e-6);

  PJ_COORD arr[2] = {east, west};
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, arr));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, arr));
  EXPECT_NEAR(east.lp.lam, arr[0].lp.lam, 1e-9);
  EXPECT_NEAR(east.lp.phi, arr[0].lp.phi, 1e-9);
  EXPECT_NEAR(west.lp.lam, arr[1].lp.lam, 1e-9);
  EXPECT_NEAR(west.lp.phi, arr[1].lp.phi, 1e-9);

  // At the center of stereographic projection, scale factors equal k0 (1.0)
  // and angular distortion is 0.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-6);
  EXPECT_NEAR(0.0, factors.angular_distortion, 1e-6);
}

TEST(RoussTest, ConstructorAndDestructorEdgeCases) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=rouss +lat_0=52 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  ASSERT_NE(nullptr, pj->destructor);

  // 1. Destructor with nullptr PJ* returns nullptr.
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));

  // 2. Direct pj_rouss(nullptr) allocates a skeleton PJ with null opaque;
  // passing it to pj_rouss_destructor exercises the nullptr == P->opaque path.
  PJ* raw = pj_rouss(nullptr);
  ASSERT_NE(nullptr, raw);
  EXPECT_STREQ("rouss", raw->short_name);
  EXPECT_EQ(nullptr, raw->opaque);
  EXPECT_EQ(nullptr, pj->destructor(raw, PROJ_ERR_OTHER));
}

}  // namespace
}  // namespace proj
