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
// Unit tests for the Mollweide (moll), Wagner IV (wag4), and Wagner V (wag5)
// pseudocylindrical projections (proj/src/projections/moll.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/moll.cpp

#include <array>
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

TEST(MollTest, MetadataAndSphericalSetupForMollWag4AndWag5) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct VariantSpec {
    const char* proj_str;
    std::string_view expected_id;
    std::string_view expected_desc;
  };
  constexpr VariantSpec kVariants[] = {
      {
          .proj_str = "+proj=moll +ellps=WGS84",
          .expected_id = "moll",
          .expected_desc = "Mollweide\n\tPCyl, Sph",
      },
      {
          .proj_str = "+proj=wag4 +ellps=WGS84",
          .expected_id = "wag4",
          .expected_desc = "Wagner IV\n\tPCyl, Sph",
      },
      {
          .proj_str = "+proj=wag5 +ellps=WGS84",
          .expected_id = "wag5",
          .expected_desc = "Wagner V\n\tPCyl, Sph",
      },
  };

  for (const auto& v : kVariants) {
    ScopedPj pj(proj_create(ctx.get(), v.proj_str));
    ASSERT_NE(nullptr, pj.get()) << v.expected_id;
    EXPECT_EQ(0, proj_errno(pj.get())) << v.expected_id;

    const PJ_PROJ_INFO info = proj_pj_info(pj.get());
    EXPECT_EQ(v.expected_id, info.id);
    EXPECT_STREQ("PROJ-based coordinate operation", info.description);
    EXPECT_EQ(v.expected_desc, pj->descr);
    EXPECT_EQ(1, info.has_inverse);
    EXPECT_EQ(1, pj_has_inverse(pj.get()));
    EXPECT_DOUBLE_EQ(0.0, pj->es);
  }
}

TEST(MollTest, MollweideForwardPolesExhaustMaxIterAndClampToHalfPi) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=moll +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // On unit-sphere Mollweide, C_x = 2*sqrt(2)/pi, C_y = sqrt(2), C_p = pi.
  const double expected_cx = 2.0 * std::sqrt(2.0) / kPi;
  const double expected_cy = std::sqrt(2.0);

  // 1. Equator: theta = 0, x = C_x * lam, y = 0.
  const PJ_COORD eq_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(1.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(expected_cx, eq_fwd.xy.x, 1e-12);
  EXPECT_NEAR(0.0, eq_fwd.xy.y, 1e-12);

  // 2. Exact poles (+pi/2 and -pi/2) via proj_trans: theta + sin(theta) = +-pi
  //    has a triple root at theta = +-pi, so Newton-Raphson takes 29 steps
  //    before (theta + sin(theta) - k) rounds to 0 in IEEE 754 double at
  //    theta ~= +-(pi - 1.249e-5).
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(1.0, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, north_pole.xy.x, 1e-5);
  EXPECT_NEAR(expected_cy, north_pole.xy.y, 1e-10);

  const PJ_COORD south_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(1.0, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, south_pole.xy.x, 1e-5);
  EXPECT_NEAR(-expected_cy, south_pole.xy.y, 1e-10);

  // 3. Exhaust MAX_ITER (i == 0) to cover both branches of
  //    `if (!i) lp.phi = (lp.phi < 0.) ? -M_HALFPI : M_HALFPI;` in
  //    moll_s_forward, both via proj_trans at +-(kHalfPi - 1.5e-8) and via
  //    direct pj->fwd at +-(kPi - 1e-5) where |V| stays > 45.0.
  const PJ_COORD near_np =
      proj_trans(pj.get(), PJ_FWD, proj_coord(1.0, kHalfPi - 1.5e-8, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, near_np.xy.x, 1e-12);
  EXPECT_NEAR(expected_cy, near_np.xy.y, 1e-12);

  const PJ_COORD near_sp = proj_trans(
      pj.get(), PJ_FWD, proj_coord(1.0, -kHalfPi + 1.5e-8, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, near_sp.xy.x, 1e-12);
  EXPECT_NEAR(-expected_cy, near_sp.xy.y, 1e-12);

  const PJ_XY clamp_pos = pj->fwd({.lam = 1.0, .phi = kPi - 1e-5}, pj.get());
  EXPECT_NEAR(0.0, clamp_pos.x, 1e-12);
  EXPECT_NEAR(expected_cy, clamp_pos.y, 1e-12);

  const PJ_XY clamp_neg = pj->fwd({.lam = 1.0, .phi = -kPi + 1e-5}, pj.get());
  EXPECT_NEAR(0.0, clamp_neg.x, 1e-12);
  EXPECT_NEAR(-expected_cy, clamp_neg.y, 1e-12);
}

TEST(MollTest, ForwardAndInverseRoundTripAcrossMollWag4AndWag5) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr const char* kDefs[] = {
      "+proj=moll +R=1",
      "+proj=wag4 +R=1",
      "+proj=wag5 +R=1",
  };
  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 30.0, .lat_deg = 15.0},
      {.lon_deg = -75.0, .lat_deg = 45.0},
      {.lon_deg = 120.0, .lat_deg = -60.0},
      {.lon_deg = -170.0, .lat_deg = -80.0},
  };

  for (const char* def : kDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;

    for (const auto& tc : kCases) {
      const double lam = DegToRad(tc.lon_deg);
      const double phi = DegToRad(tc.lat_deg);

      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      EXPECT_TRUE(std::isfinite(fwd.xy.x)) << def;
      EXPECT_TRUE(std::isfinite(fwd.xy.y)) << def;

      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      EXPECT_NEAR(lam, inv.lp.lam, 1e-6) << def;
      EXPECT_NEAR(phi, inv.lp.phi, 1e-6) << def;
    }
  }
}

TEST(MollTest, InverseOutsideBoundingEllipseReturnsHugeValAndSetsErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=moll +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. At y = 0 on unit-sphere Mollweide, |x| >= C_x * pi = 2*sqrt(2) (~2.8284)
  // gives |lp.lam| >= pi, hitting the `else { lp.lam = lp.phi = HUGE_VAL; }`
  // branch in moll_s_inverse (which returns HUGE_VAL without setting errno).
  const PJ_COORD out_east =
      proj_trans(pj.get(), PJ_INV, proj_coord(3.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, out_east.lp.lam);
  EXPECT_EQ(HUGE_VAL, out_east.lp.phi);
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_COORD out_west =
      proj_trans(pj.get(), PJ_INV, proj_coord(-3.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, out_west.lp.lam);
  EXPECT_EQ(HUGE_VAL, out_west.lp.phi);
  EXPECT_EQ(0, proj_errno(pj.get()));

  // 2. When |y| > C_y = sqrt(2), aasin(ctx, y / C_y) sets
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN and returns HUGE_VAL.
  const PJ_COORD out_north =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 2.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, out_north.lp.lam);
  EXPECT_EQ(HUGE_VAL, out_north.lp.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(MollTest, Classic2dEntryPointsArrayTransformAndEqualAreaFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj moll(proj_create(ctx.get(), "+proj=moll +R=1"));
  ScopedPj wag4(proj_create(ctx.get(), "+proj=wag4 +R=1"));
  ASSERT_NE(nullptr, moll.get());
  ASSERT_NE(nullptr, wag4.get());

  const PJ_LP lp = {.lam = DegToRad(40.0), .phi = DegToRad(35.0)};
  const PJ_XY xy = pj_fwd(lp, moll.get());
  EXPECT_EQ(0, proj_errno(moll.get()));
  const PJ_LP inv_lp = pj_inv(xy, moll.get());
  EXPECT_EQ(0, proj_errno(moll.get()));
  EXPECT_NEAR(lp.lam, inv_lp.lam, 1e-6);
  EXPECT_NEAR(lp.phi, inv_lp.phi, 1e-6);

  auto coords = std::to_array<PJ_COORD>({
      proj_coord(DegToRad(-50.0), DegToRad(25.0), 0.0, 0.0),
      proj_coord(DegToRad(80.0), DegToRad(-40.0), 0.0, 0.0),
  });
  ASSERT_EQ(0,
            proj_trans_array(wag4.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(wag4.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(-50.0), coords[0].lp.lam, 1e-6);
  EXPECT_NEAR(DegToRad(25.0), coords[0].lp.phi, 1e-6);
  EXPECT_NEAR(DegToRad(80.0), coords[1].lp.lam, 1e-6);
  EXPECT_NEAR(DegToRad(-40.0), coords[1].lp.phi, 1e-6);

  // Both Mollweide and Wagner IV are equal-area projections (areal_scale == 1).
  const PJ_FACTORS moll_factors = proj_factors(
      moll.get(), proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(moll.get()));
  EXPECT_NEAR(1.0, moll_factors.areal_scale, 1e-5);

  const PJ_FACTORS wag4_factors = proj_factors(
      wag4.get(), proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(wag4.get()));
  EXPECT_NEAR(1.0, wag4_factors.areal_scale, 1e-5);
}

}  // namespace
}  // namespace proj
