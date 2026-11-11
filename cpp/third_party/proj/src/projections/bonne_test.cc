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
// Unit tests for the Bonne (and Werner when lat_1=90) pseudoconical equal-area
// projection (proj/src/projections/bonne.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/bonne.cpp

#include <cmath>
#include <cstdlib>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(BonneTest, InvalidLat1ZeroOrMissingRejectsCreation) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get());

  // 1. Missing +lat_1 defaults to 0, failing |lat_1| >= 1e-10 check.
  ScopedPj missing_lat1(proj_create(ctx.get(), "+proj=bonne +ellps=WGS84"));
  EXPECT_EQ(nullptr, missing_lat1.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for lat_1"));

  // 2. Explicit +lat_1=0 is also rejected.
  ScopedPj zero_lat1(proj_create(ctx.get(), "+proj=bonne +lat_1=0 +R=6371000"));
  EXPECT_EQ(nullptr, zero_lat1.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
}

TEST(BonneTest, EllipsoidalForwardAndInverseNorthernAndSouthernLat1RoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  for (const char* proj_str : {
           "+proj=bonne +lat_1=45 +ellps=WGS84",
           "+proj=bonne +lat_1=-45 +ellps=WGS84",
       }) {
    ScopedPj pj(proj_create(ctx.get(), proj_str));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(1, pj_has_inverse(pj.get()));

    struct CoordCase {
      double lon_deg;
      double lat_deg;
    };
    constexpr CoordCase kCases[] = {
        {.lon_deg = 0.0, .lat_deg = 0.0},
        {.lon_deg = 15.0, .lat_deg = 45.0},
        {.lon_deg = -25.0, .lat_deg = 30.0},
        {.lon_deg = 20.0, .lat_deg = -35.0},
        {.lon_deg = -40.0, .lat_deg = -60.0},
    };

    for (const auto& tc : kCases) {
      // 1. Forward projection.
      const PJ_COORD in =
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 100.0, 2026.0);
      const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_FALSE(std::isinf(fwd.xy.x));
      EXPECT_FALSE(std::isinf(fwd.xy.y));
      EXPECT_DOUBLE_EQ(100.0, fwd.xyzt.z);
      EXPECT_DOUBLE_EQ(2026.0, fwd.xyzt.t);

      // 2. Inverse projection round-trip.
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-10);
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-10);
    }
  }
}

TEST(BonneTest, EllipsoidalWernerPoleZeroRadiusAndInverseDomainBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  // 1. With lat_1=90 (Werner projection) on WGS84, am1 == 0 and at the North
  // Pole (phi = pi/2) rh == 0, exercising the |rh| <= EPS10 branch in
  // bonne_e_forward (xy = {0, 0}).
  ScopedPj werner_ellps(
      proj_create(ctx.get(), "+proj=bonne +lat_1=90 +ellps=WGS84"));
  ASSERT_NE(nullptr, werner_ellps.get());
  const PJ_LP north_pole = {.lam = DegToRad(30.0), .phi = kHalfPi};
  const PJ_XY pole_xy = pj_fwd(north_pole, werner_ellps.get());
  EXPECT_EQ(0, proj_errno(werner_ellps.get()));
  EXPECT_DOUBLE_EQ(0.0, pole_xy.x);
  EXPECT_DOUBLE_EQ(0.0, pole_xy.y);

  // 2. For lat_1=45 on WGS84, find the projected North Pole and perturb y by
  // +5e-11 * a so normalized rh decreases by 5e-11 and pj_inv_mlfn returns
  // abs_phi in [M_HALFPI, M_HALFPI + EPS10], exercising the `abs_phi - M_HALFPI
  // <= EPS10` pole-clamping branch (lp.lam = 0).
  ScopedPj pj45(proj_create(ctx.get(), "+proj=bonne +lat_1=45 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj45.get());
  const PJ_XY pole45_xy = pj_fwd({.lam = 0.0, .phi = kHalfPi}, pj45.get());
  EXPECT_EQ(0, proj_errno(pj45.get()));

  const PJ_LP near_pole_inv =
      pj_inv({.x = 0.0, .y = pole45_xy.y + 5e-11 * pj45->a}, pj45.get());
  EXPECT_EQ(0, proj_errno(pj45.get()));
  EXPECT_DOUBLE_EQ(0.0, near_pole_inv.lam);
  EXPECT_NEAR(kHalfPi, near_pole_inv.phi, 1e-9);

  // 3. Large y near the cone apex (y = 1.0 * a in meters -> 1.0 in normalized
  // units) makes |lp.phi| > M_HALFPI + EPS10, triggering
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN in bonne_e_inverse.
  proj_errno_reset(pj45.get());
  const PJ_LP out_of_domain_e =
      pj_inv({.x = 0.0, .y = 1.0 * pj45->a}, pj45.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj45.get()));
  EXPECT_TRUE(std::isinf(out_of_domain_e.lam));
}

TEST(BonneTest, SphericalForwardAndInverseStandardAndWernerPoleBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  // 1. Standard spherical Bonne with positive and negative lat_1 (cphi1 =
  // cot(phi1)).
  for (const char* proj_str : {
           "+proj=bonne +lat_1=45 +R=6371000",
           "+proj=bonne +lat_1=-45 +R=6371000",
       }) {
    ScopedPj pj(proj_create(ctx.get(), proj_str));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD in = proj_coord(DegToRad(25.0), DegToRad(35.0), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-12);
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-12);
  }

  // 2. Spherical Werner (lat_1=90 -> |phi1| + EPS10 >= M_HALFPI -> cphi1 = 0).
  ScopedPj werner_sph(
      proj_create(ctx.get(), "+proj=bonne +lat_1=90 +R=6371000"));
  ASSERT_NE(nullptr, werner_sph.get());

  // At North Pole (phi = pi/2), rh == 0 -> |rh| <= EPS10 branch in
  // bonne_s_forward returns (0, 0).
  const PJ_XY pole_fwd =
      pj_fwd({.lam = DegToRad(40.0), .phi = kHalfPi}, werner_sph.get());
  EXPECT_EQ(0, proj_errno(werner_sph.get()));
  EXPECT_DOUBLE_EQ(0.0, pole_fwd.x);
  EXPECT_DOUBLE_EQ(0.0, pole_fwd.y);

  // Inverse at (0, 0) yields rh == 0, lp.phi == pi/2 -> M_HALFPI - abs_phi <=
  // EPS10 sets lp.lam = 0.
  const PJ_LP pole_inv = pj_inv({.x = 0.0, .y = 0.0}, werner_sph.get());
  EXPECT_EQ(0, proj_errno(werner_sph.get()));
  EXPECT_DOUBLE_EQ(0.0, pole_inv.lam);
  EXPECT_NEAR(kHalfPi, pole_inv.phi, 1e-12);

  // 3. Out-of-domain coordinate in bonne_s_inverse (|lp.phi| > M_HALFPI when
  // normalized rh > pi).
  proj_errno_reset(werner_sph.get());
  const PJ_LP out_of_domain_s = pj_inv(
      {.x = 10.0 * werner_sph->a, .y = 10.0 * werner_sph->a}, werner_sph.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(werner_sph.get()));
  EXPECT_TRUE(std::isinf(out_of_domain_s.lam));
}

TEST(BonneTest, FactorsEqualAreaAndDestructorNullBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=bonne +lat_1=45 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  // Bonne is an equal-area projection, so areal_scale is 1.0.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(DegToRad(10.0), DegToRad(45.0), 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-6);
  EXPECT_NEAR(1.0, factors.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-6);

  // Exercise destructor null-pointer and null-opaque defensive branches.
  ASSERT_NE(nullptr, pj->destructor);
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));
  std::free(pj->opaque);
  pj->opaque = nullptr;
}

}  // namespace
}  // namespace proj
