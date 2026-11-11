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
// Unit tests for General Sinusoidal Series (gn_sinu), Sinusoidal (sinu),
// Eckert VI (eck6), and McBryde-Thomas Flat-Polar Sinusoidal (mbtfps)
// projections (proj/src/projections/gn_sinu.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/gn_sinu.cpp

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

TEST(GnSinuTest, ParameterValidationRejectsMissingOrIllegalMAndN) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  // 1. Missing parameter n.
  {
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj pj(proj_create(ctx.get(), "+proj=gn_sinu +m=1"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
    EXPECT_TRUE(capture.ContainsMessage("Missing parameter n."));
  }

  // 2. Missing parameter m.
  {
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj pj(proj_create(ctx.get(), "+proj=gn_sinu +n=1"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
    EXPECT_TRUE(capture.ContainsMessage("Missing parameter m."));
  }

  // 3. Non-positive n (n <= 0).
  for (const char* def :
       {"+proj=gn_sinu +n=0 +m=1", "+proj=gn_sinu +n=-0.5 +m=1"}) {
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj pj(proj_create(ctx.get(), def));
    EXPECT_EQ(nullptr, pj.get()) << def;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << def;
  }
  EXPECT_TRUE(capture.ContainsMessage("Invalid value for n"));

  // 4. Negative m (m < 0).
  {
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj pj(proj_create(ctx.get(), "+proj=gn_sinu +n=1 +m=-0.25"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
    EXPECT_TRUE(capture.ContainsMessage("Invalid value for m"));
  }
}

TEST(GnSinuTest, EllipsoidalSinusoidalForwardInversePolesAndOutOfDomain) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppressor(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=sinu +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 25.0, .lat_deg = 48.5},
      {.lon_deg = -110.0, .lat_deg = -33.2},
  };

  for (const auto& tc : kCases) {
    // 1. Forward ellipsoidal sinusoidal.
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));

    // 2. Inverse ellipsoidal sinusoidal (|phi| < pi/2 branch).
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-11);
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-11);
  }

  // 3. Northing slightly past the North Pole (by 1e-4 m, ~1.57e-11 rad < EPS10)
  // ensures pj_inv_mlfn returns phi >= M_HALFPI and triggers the
  // `(s - EPS10) < M_HALFPI` branch where lp.lam is clamped to 0.
  const PJ_COORD pole_fwd = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(45.0), kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, pole_fwd.xy.x, 1e-6);

  const PJ_COORD pole_inv = proj_trans(
      pj.get(), PJ_INV, proj_coord(1.0, pole_fwd.xy.y + 1e-4, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, pole_inv.lp.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, pole_inv.lp.phi, 1e-9);

  // 4. Northing beyond the pole meridional distance triggers
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN in gn_sinu_e_inverse.
  const PJ_COORD beyond_pole = proj_trans(
      pj.get(), PJ_INV, proj_coord(0.0, 2.0 * pole_fwd.xy.y, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, beyond_pole.lp.lam);
  EXPECT_EQ(HUGE_VAL, beyond_pole.lp.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(GnSinuTest, SphericalSinusoidalEck6MbtfpsAndGeneralSeriesRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr const char* kDefs[] = {
      // Spherical sinu: m == 0, n == 1.
      "+proj=sinu +R=6371000",
      // Eckert VI: m == 1, n == 1 + pi/2.
      "+proj=eck6 +R=6371000",
      // McBryde-Thomas Flat-Polar Sinusoidal: m == 0.5, n == 1.785398...
      "+proj=mbtfps +R=6371000",
      // General Sinusoidal Series with m == 0 and n != 1 (exercises the
      // Q->m == 0 && Q->n != 1 branch in both forward and inverse).
      "+proj=gn_sinu +R=6371000 +m=0 +n=0.8",
      // General Sinusoidal Series with m > 0 and n > 0.
      "+proj=gn_sinu +R=6371000 +m=2 +n=3",
  };

  for (const char* def : kDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;

    const PJ_COORD in =
        proj_coord(DegToRad(30.0), DegToRad(40.0), 10.0, 2026.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    EXPECT_FALSE(std::isinf(fwd.xy.x)) << def;
    EXPECT_FALSE(std::isinf(fwd.xy.y)) << def;

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-6) << def;
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-6) << def;
  }
}

TEST(GnSinuTest, SphericalForwardNonConvergenceAndDestructorNullBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppressor(ctx.get());

  // 1. With m > 0 and huge n (or NaN phi via pj_fwd), the 8-step Newton loop in
  // gn_sinu_s_forward exhausts MAX_ITER and sets
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  ScopedPj pj(proj_create(ctx.get(), "+proj=gn_sinu +R=1 +m=1 +n=1e9"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_XY non_conv = pj_fwd({.lam = 0.2, .phi = 0.5}, pj.get());
  (void)non_conv;
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 2. Exercise null P and null P->opaque branches in pj_gn_sinu_destructor.
  const PJ_DESTRUCTOR dtor = pj->destructor;
  ASSERT_NE(nullptr, dtor);
  EXPECT_EQ(nullptr, dtor(nullptr, 0));

  PJ* bare_pj = pj_new();
  ASSERT_NE(nullptr, bare_pj);
  bare_pj->ctx = ctx.get();
  EXPECT_EQ(nullptr, dtor(bare_pj, 0));
}

}  // namespace
}  // namespace proj
