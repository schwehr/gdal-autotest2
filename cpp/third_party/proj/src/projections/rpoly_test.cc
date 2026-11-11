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
// Unit tests for the Rectangular Polyconic projection
// (proj/src/projections/rpoly.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/rpoly.cpp

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

TEST(RpolyTest, MetadataAndNonInvertibility) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=rpoly +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("rpoly", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Rectangular Polyconic"),
            std::string_view::npos);
  EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD));
  EXPECT_EQ(0, info.has_inverse);
  EXPECT_EQ(0, pj_has_inverse(pj.get()));

  ScopedLogSuppressor suppressor(ctx.get());
  const PJ_COORD inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.1, 0.2, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv.xyzt.x);
  EXPECT_NE(0, proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(RpolyTest, ForwardDefaultModeZeroLatTsEquatorAndNonZeroLat0) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  // Without +lat_ts (or |lat_ts| <= 1e-9), Q->mode is 0 and fa = 0.5 * lam.
  // With +lat_0=10, P->phi0 = DegToRad(10).
  ScopedPj pj(proj_create(ctx.get(), "+proj=rpoly +lat_0=10 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const double phi0 = DegToRad(10.0);
  const double lam = DegToRad(30.0);

  // 1. Equator (|phi| < 1e-9) hits the near-equator branch:
  //    x = fa + fa = lam, y = -phi0.
  const PJ_COORD eq =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, eq.xy.x, 1e-12);
  EXPECT_NEAR(-phi0, eq.xy.y, 1e-12);

  // 2. Non-zero latitude (|phi| >= 1e-9) hits the general polyconic branch:
  //    cot_phi = 1 / tan(phi), fa = 2 * atan(0.5 * lam * sin(phi)),
  //    x = sin(fa) * cot_phi, y = phi - phi0 + (1 - cos(fa)) * cot_phi.
  const double phi = DegToRad(40.0);
  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));

  const double cot_phi = 1.0 / std::tan(phi);
  const double fa = 2.0 * std::atan(0.5 * lam * std::sin(phi));
  const double expected_x = std::sin(fa) * cot_phi;
  const double expected_y = phi - phi0 + (1.0 - std::cos(fa)) * cot_phi;
  EXPECT_NEAR(expected_x, fwd.xy.x, 1e-12);
  EXPECT_NEAR(expected_y, fwd.xy.y, 1e-12);

  // 3. Symmetry across the central meridian (lam -> -lam).
  const PJ_COORD fwd_west =
      proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-expected_x, fwd_west.xy.x, 1e-12);
  EXPECT_NEAR(expected_y, fwd_west.xy.y, 1e-12);
}

TEST(RpolyTest, ForwardNonZeroLatTsModeOneEquatorAndGeneralLatitude) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  // With |lat_ts| > 1e-9, Q->mode is 1:
  //   fxb = 0.5 * sin(|lat_ts|), fxa = 0.5 / fxb, fa = tan(lam * fxb) * fxa.
  ScopedPj pj_pos(
      proj_create(ctx.get(), "+proj=rpoly +lat_ts=30 +lat_0=15 +R=1"));
  ScopedPj pj_neg(
      proj_create(ctx.get(), "+proj=rpoly +lat_ts=-30 +lat_0=15 +R=1"));
  ASSERT_NE(nullptr, pj_pos.get());
  ASSERT_NE(nullptr, pj_neg.get());

  const double phi1 = DegToRad(30.0);
  const double phi0 = DegToRad(15.0);
  const double fxb = 0.5 * std::sin(phi1);
  const double fxa = 0.5 / fxb;
  const double lam = DegToRad(24.0);

  // 1. Equator (|phi| < 1e-9) with Q->mode == 1: x = 2 * fa, y = -phi0.
  const double fa_eq = std::tan(lam * fxb) * fxa;
  const PJ_COORD eq_pos =
      proj_trans(pj_pos.get(), PJ_FWD, proj_coord(lam, 0.0, 0.0, 0.0));
  const PJ_COORD eq_neg =
      proj_trans(pj_neg.get(), PJ_FWD, proj_coord(lam, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_pos.get()));
  EXPECT_EQ(0, proj_errno(pj_neg.get()));
  EXPECT_NEAR(2.0 * fa_eq, eq_pos.xy.x, 1e-12);
  EXPECT_NEAR(-phi0, eq_pos.xy.y, 1e-12);
  EXPECT_NEAR(eq_pos.xy.x, eq_neg.xy.x, 1e-12);
  EXPECT_NEAR(eq_pos.xy.y, eq_neg.xy.y, 1e-12);

  // 2. Non-zero latitude with Q->mode == 1.
  const double phi = DegToRad(-35.0);
  const PJ_COORD fwd_pos =
      proj_trans(pj_pos.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  const PJ_COORD fwd_neg =
      proj_trans(pj_neg.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_pos.get()));
  EXPECT_EQ(0, proj_errno(pj_neg.get()));

  const double cot_phi = 1.0 / std::tan(phi);
  const double fa = 2.0 * std::atan(fa_eq * std::sin(phi));
  const double expected_x = std::sin(fa) * cot_phi;
  const double expected_y = phi - phi0 + (1.0 - std::cos(fa)) * cot_phi;
  EXPECT_NEAR(expected_x, fwd_pos.xy.x, 1e-12);
  EXPECT_NEAR(expected_y, fwd_pos.xy.y, 1e-12);
  // Q->phi1 uses fabs(lat_ts), so +lat_ts=30 and +lat_ts=-30 match.
  EXPECT_NEAR(fwd_pos.xy.x, fwd_neg.xy.x, 1e-12);
  EXPECT_NEAR(fwd_pos.xy.y, fwd_neg.xy.y, 1e-12);
}

TEST(RpolyTest, EllipsoidForcesSphericalAndArrayFactorsWork) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  // rpoly sets P->es = 0, using only the semimajor axis a.
  ScopedPj pj_ellps(proj_create(ctx.get(), "+proj=rpoly +ellps=WGS84"));
  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=rpoly +R=6378137"));
  ASSERT_NE(nullptr, pj_ellps.get());
  ASSERT_NE(nullptr, pj_sphere.get());

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(15.0), DegToRad(45.0), 0.0, 0.0),
      proj_coord(DegToRad(-20.0), kHalfPi, 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj_ellps.get(), PJ_FWD, 2, coords));

  const PJ_XY xy0 =
      pj_fwd({.lam = DegToRad(15.0), .phi = DegToRad(45.0)}, pj_sphere.get());
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(xy0.x, coords[0].xy.x, 1e-6);
  EXPECT_NEAR(xy0.y, coords[0].xy.y, 1e-6);

  const PJ_FACTORS factors = proj_factors(
      pj_ellps.get(), proj_coord(DegToRad(10.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));
  EXPECT_GT(factors.meridional_scale, 0.0);
  EXPECT_GT(factors.parallel_scale, 0.0);
}

}  // namespace
}  // namespace proj
