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
// Unit tests for the California Cooperative Oceanic Fisheries Investigations
// (CalCOFI) Line/Station coordinate system (proj/src/projections/calcofi.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/calcofi.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

C_NAMESPACE PJ* pj_calcofi(PJ*);

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

// Reference point O: Line 80, Station 60 at (-121.15 deg, 34.15 deg).
constexpr double kPtOLonDeg = -121.15;
constexpr double kPtOLatDeg = 34.15;
constexpr double kPtOLine = 80.0;
constexpr double kPtOStation = 60.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(CalcofiTest, EllipsoidalReferencePointAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  // By convention CalCOFI uses Clarke 1866.
  ScopedPj pj(proj_create(ctx.get(), "+proj=calcofi +ellps=clrk66"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_STREQ("calcofi", proj_pj_info(pj.get()).id);

  // 1. Reference point O (-121.15, 34.15) maps to (Line 80, Station 60).
  const PJ_COORD pt_o =
      proj_coord(DegToRad(kPtOLonDeg), DegToRad(kPtOLatDeg), 0.0, 0.0);
  const PJ_COORD fwd_o = proj_trans(pj.get(), PJ_FWD, pt_o);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kPtOLine, fwd_o.xy.x, 1e-7);
  EXPECT_NEAR(kPtOStation, fwd_o.xy.y, 1e-7);

  const PJ_COORD inv_o = proj_trans(pj.get(), PJ_INV, fwd_o);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(pt_o.lp.lam, inv_o.lp.lam, 1e-9);
  EXPECT_NEAR(pt_o.lp.phi, inv_o.lp.phi, 1e-9);

  // 2. Round-trip across California Current stations.
  struct StationCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr StationCase kCases[] = {
      {.lon_deg = -121.15, .lat_deg = 34.15},
      {.lon_deg = -117.25, .lat_deg = 32.70},
      {.lon_deg = -122.00, .lat_deg = 36.80},
      {.lon_deg = -124.50, .lat_deg = 38.00},
      {.lon_deg = -118.50, .lat_deg = 30.00},
  };

  for (const auto& tc : kCases) {
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), -50.0, 2026.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lpzt.lam, inv.lpzt.lam, 1e-7);
    EXPECT_NEAR(in.lpzt.phi, inv.lpzt.phi, 1e-7);
    EXPECT_NEAR(in.lpzt.z, inv.lpzt.z, 1e-9);
    EXPECT_NEAR(in.lpzt.t, inv.lpzt.t, 1e-9);
  }
}

TEST(CalcofiTest, SphericalReferencePointAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=calcofi +R=6370997"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0.0, pj->es);

  // 1. Reference point O on sphere also maps to (Line 80, Station 60).
  const PJ_COORD pt_o =
      proj_coord(DegToRad(kPtOLonDeg), DegToRad(kPtOLatDeg), 0.0, 0.0);
  const PJ_COORD fwd_o = proj_trans(pj.get(), PJ_FWD, pt_o);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kPtOLine, fwd_o.xy.x, 1e-7);
  EXPECT_NEAR(kPtOStation, fwd_o.xy.y, 1e-7);

  // 2. Round-trip array of points on the spherical branch.
  PJ_COORD coords[3] = {
      pt_o,
      proj_coord(DegToRad(-120.0), DegToRad(33.0), 0.0, 0.0),
      proj_coord(DegToRad(-123.5), DegToRad(35.5), 0.0, 0.0),
  };
  const PJ_COORD orig[3] = {coords[0], coords[1], coords[2]};

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 3, coords));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 3, coords));
  for (int i = 0; i < 3; ++i) {
    EXPECT_NEAR(orig[i].lp.lam, coords[i].lp.lam, 1e-7);
    EXPECT_NEAR(orig[i].lp.phi, coords[i].lp.phi, 1e-7);
  }
}

TEST(CalcofiTest, PolesFailWithOutsideProjectionDomainEllipsoidalAndSpherical) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  for (const char* def :
       {"+proj=calcofi +ellps=clrk66", "+proj=calcofi +R=6370997"}) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get());

    // Both North Pole (+pi/2) and South Pole (-pi/2) satisfy
    // `fabs(fabs(lp.phi) - M_HALFPI) <= EPS10`.
    for (const double pole_phi : {kHalfPi, -kHalfPi}) {
      proj_errno_reset(pj.get());
      (void)pj_fwd({.lam = DegToRad(kPtOLonDeg), .phi = pole_phi}, pj.get());
      EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
                proj_errno(pj.get()));
    }
  }
}

TEST(CalcofiTest, IgnoresCustomOriginParametersAndDirectConstructor) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj_default(proj_create(ctx.get(), "+proj=calcofi +ellps=clrk66"));
  ScopedPj pj_custom(proj_create(
      ctx.get(),
      "+proj=calcofi +ellps=clrk66 +lon_0=-100 +x_0=5000 +y_0=9000"));
  ASSERT_NE(nullptr, pj_default.get());
  ASSERT_NE(nullptr, pj_custom.get());

  // CalCOFI explicitly zeroes lam0, x0, y0 and sets a = 1, over = 1 so
  // coordinates remain anchored to reference point O.
  EXPECT_EQ(0.0, pj_custom->lam0);
  EXPECT_EQ(0.0, pj_custom->x0);
  EXPECT_EQ(0.0, pj_custom->y0);
  EXPECT_EQ(1.0, pj_custom->a);
  EXPECT_EQ(1, pj_custom->over);

  const PJ_COORD pt = proj_coord(DegToRad(-120.5), DegToRad(33.8), 0.0, 0.0);
  const PJ_COORD fwd_default = proj_trans(pj_default.get(), PJ_FWD, pt);
  const PJ_COORD fwd_custom = proj_trans(pj_custom.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(pj_default.get()));
  EXPECT_EQ(0, proj_errno(pj_custom.get()));
  EXPECT_NEAR(fwd_default.xy.x, fwd_custom.xy.x, 1e-12);
  EXPECT_NEAR(fwd_default.xy.y, fwd_custom.xy.y, 1e-12);

  const PJ_COORD inv_default =
      proj_trans(pj_default.get(), PJ_INV, fwd_default);
  const PJ_COORD inv_custom = proj_trans(pj_custom.get(), PJ_INV, fwd_custom);
  EXPECT_EQ(0, proj_errno(pj_default.get()));
  EXPECT_EQ(0, proj_errno(pj_custom.get()));
  EXPECT_NEAR(inv_default.lp.lam, inv_custom.lp.lam, 1e-12);
  EXPECT_NEAR(inv_default.lp.phi, inv_custom.lp.phi, 1e-12);

  // Direct call to pj_calcofi(nullptr) exercises OPERATION macro allocation.
  ScopedPj raw(pj_calcofi(nullptr));
  ASSERT_NE(nullptr, raw.get());
  EXPECT_STREQ("calcofi", raw->short_name);
}

}  // namespace
}  // namespace proj
