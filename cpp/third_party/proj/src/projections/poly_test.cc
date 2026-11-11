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
// Unit tests for Polyconic (American) projection
// (proj/src/projections/poly.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/poly.cpp

#include <cmath>
#include <limits>
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

TEST(PolyTest, EllipsoidalForwardInverseRoundTripEquatorNonZeroAndPoles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(
      proj_create(ctx.get(), "+proj=poly +ellps=WGS84 +lat_0=30 +lon_0=-96"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      // Equator (fabs(lp.phi) <= TOL in poly_e_forward and fabs(xy.y) <= TOL in
      // poly_e_inverse).
      {.lon_deg = -90.0, .lat_deg = 0.0},
      // Mid-latitude Northern and Southern hemispheres.
      {.lon_deg = -85.0, .lat_deg = 38.0},
      {.lon_deg = -105.0, .lat_deg = -22.5},
  };

  for (const auto& tc : kCases) {
    // 1. Forward ellipsoidal Polyconic.
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(fwd.xy.x));
    EXPECT_FALSE(std::isinf(fwd.xy.y));

    // 2. Inverse ellipsoidal Polyconic.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-10);
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-10);
  }

  // 3. Exact North Pole exercises fabs(cp) <= TOL (ms = 0) in poly_e_forward.
  const PJ_COORD pole_fwd = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(-80.0), kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, pole_fwd.xy.x, 1e-9);
}

TEST(PolyTest, SphericalForwardInverseRoundTripEquatorAndNonEquatorial) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=poly +R=6371000 +lat_0=20"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      // Equator exercises fabs(lp.phi) <= TOL in poly_s_forward and
      // fabs(P->phi0 + xy.y) <= TOL in poly_s_inverse.
      {.lon_deg = 15.0, .lat_deg = 0.0},
      // Non-equatorial points in both hemispheres.
      {.lon_deg = 12.0, .lat_deg = 35.0},
      {.lon_deg = -18.0, .lat_deg = -25.0},
  };

  for (const auto& tc : kCases) {
    // 1. Forward spherical Polyconic.
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(fwd.xy.x));
    EXPECT_FALSE(std::isinf(fwd.xy.y));

    // 2. Inverse spherical Polyconic.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-9);
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-9);
  }
}

TEST(PolyTest, InverseFailureBranchesAndDestructorNullGuards) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppressor(ctx.get());

  // 1. Ellipsoidal inverse on unit semimajor axis (+a=1 +es=0.00669438):
  // xy.y == pi/2 (with lat_0=0 -> ml0=0) starts the iteration at lp.phi = pi/2
  // where fabs(cos(lp.phi)) < ITOL (1e-12).
  ScopedPj pj_ell(
      proj_create(ctx.get(), "+proj=poly +a=1 +es=0.00669438 +lat_0=0"));
  ASSERT_NE(nullptr, pj_ell.get());
  const PJ_LP cp_fail = pj_inv({.x = 0.1, .y = kHalfPi}, pj_ell.get());
  EXPECT_EQ(HUGE_VAL, cp_fail.lam);
  EXPECT_EQ(HUGE_VAL, cp_fail.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_ell.get()));
  proj_errno_reset(pj_ell.get());

  // 2. Ellipsoidal inverse non-convergence after I_ITER = 20 iterations:
  // xy.x = NaN with xy.y = 1.0 passes fabs(xy.y) > TOL and first-step
  // fabs(cos(1.0)) >= ITOL, then propagates NaN through all 20 Newton-Raphson
  // steps until !i sets OUTSIDE_PROJECTION_DOMAIN.
  const double kNan = std::numeric_limits<double>::quiet_NaN();
  const PJ_LP iter_fail = pj_inv({.x = kNan, .y = 1.0}, pj_ell.get());
  EXPECT_EQ(HUGE_VAL, iter_fail.lam);
  EXPECT_EQ(HUGE_VAL, iter_fail.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_ell.get()));
  proj_errno_reset(pj_ell.get());

  // 3. Spherical inverse non-convergence after N_ITER = 10 iterations.
  ScopedPj pj_sph(proj_create(ctx.get(), "+proj=poly +R=1 +lat_0=0"));
  ASSERT_NE(nullptr, pj_sph.get());
  (void)pj_inv({.x = 5.0, .y = 1.0}, pj_sph.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_sph.get()));
  proj_errno_reset(pj_sph.get());

  // 4. Destructor null P and null P->opaque guards.
  const PJ_DESTRUCTOR dtor = pj_ell->destructor;
  ASSERT_NE(nullptr, dtor);
  EXPECT_EQ(nullptr, dtor(nullptr, 0));

  PJ* bare_pj = pj_new();
  ASSERT_NE(nullptr, bare_pj);
  bare_pj->ctx = ctx.get();
  EXPECT_EQ(nullptr, dtor(bare_pj, 0));
}

}  // namespace
}  // namespace proj
