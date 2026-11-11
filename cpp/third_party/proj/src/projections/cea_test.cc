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
// Unit tests for the Equal Area Cylindrical (cea) projection
// (proj/src/projections/cea.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/cea.cpp

#include <array>
#include <cmath>
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

TEST(CeaTest, InvalidLatTsGreaterThan90DegreesFailsWithIllegalArgValue) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get());

  for (const char* defn :
       {"+proj=cea +lat_ts=95", "+proj=cea +lat_ts=-100 +ellps=WGS84"}) {
    ScopedPj pj(proj_create(ctx.get(), defn));
    EXPECT_EQ(nullptr, pj.get()) << defn;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
  }
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for lat_ts"));
}

TEST(CeaTest, EllipsoidalDefaultAndStandardParallelRoundTripAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  for (const char* defn :
       {"+proj=cea +ellps=WGS84", "+proj=cea +lat_ts=30 +ellps=WGS84"}) {
    ScopedPj pj(proj_create(ctx.get(), defn));
    ASSERT_NE(nullptr, pj.get()) << defn;
    EXPECT_EQ(1, pj_has_inverse(pj.get()));

    struct CoordCase {
      double lon_deg;
      double lat_deg;
    };
    constexpr CoordCase kCases[] = {
        {.lon_deg = 0.0, .lat_deg = 0.0},
        {.lon_deg = 15.0, .lat_deg = 30.0},
        {.lon_deg = -122.4, .lat_deg = 37.8},
        {.lon_deg = 140.0, .lat_deg = -45.0},
        {.lon_deg = -60.0, .lat_deg = 85.0},
    };

    for (const auto& tc : kCases) {
      const double lam = DegToRad(tc.lon_deg);
      const double phi = DegToRad(tc.lat_deg);

      // 1. Forward ellipsoidal projection.
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get())) << defn;
      EXPECT_FALSE(std::isinf(fwd.xy.x));
      EXPECT_FALSE(std::isinf(fwd.xy.y));

      // 2. Inverse ellipsoidal projection.
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << defn;
      EXPECT_NEAR(lam, inv.lp.lam, 1e-11) << defn;
      EXPECT_NEAR(phi, inv.lp.phi, 1e-11) << defn;

      // 3. Equal-area property: areal_scale == 1.0 everywhere on the ellipsoid.
      const PJ_FACTORS factors =
          proj_factors(pj.get(), proj_coord(lam, phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(1.0, factors.areal_scale, 1e-6);
    }
  }
}

TEST(CeaTest, SphericalForwardInversePoleClampingAndOutsideDomain) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=cea +lat_ts=30 +R=1"));
  ASSERT_NE(nullptr, pj.get());
  const double k0 = std::cos(DegToRad(30.0));
  EXPECT_NEAR(k0, pj->k0, 1e-12);

  // 1. Standard forward and inverse on unit sphere: x = k0 * lam, y =
  // sin(phi)/k0.
  const double lam = DegToRad(40.0);
  const double phi = DegToRad(30.0);
  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(k0 * lam, fwd.xy.x, 1e-12);
  EXPECT_NEAR(0.5 / k0, fwd.xy.y, 1e-12);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, inv.lp.lam, 1e-12);
  EXPECT_NEAR(phi, inv.lp.phi, 1e-12);

  // 2. Near-pole clamping in cea_s_inverse when 1.0 <= |y * k0| <= 1.0 + 1e-10.
  const double y_north_pole = (1.0 + 0.5e-10) / k0;
  const PJ_COORD north_inv = proj_trans(
      pj.get(), PJ_INV, proj_coord(k0 * 0.5, y_north_pole, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.5, north_inv.lp.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, north_inv.lp.phi, 1e-12);

  const double y_south_pole = -(1.0 + 0.5e-10) / k0;
  const PJ_COORD south_inv = proj_trans(
      pj.get(), PJ_INV, proj_coord(k0 * 0.5, y_south_pole, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.5, south_inv.lp.lam, 1e-12);
  EXPECT_NEAR(-kHalfPi, south_inv.lp.phi, 1e-12);

  // 3. Out-of-domain |y * k0| > 1.0 + 1e-10 sets OUTSIDE_PROJECTION_DOMAIN.
  const PJ_COORD out_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 2.0 / k0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, out_inv.lp.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(CeaTest, DestructorNullGuards) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=cea +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  ASSERT_NE(nullptr, pj->destructor);

  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));

  PJ* raw = pj_new();
  ASSERT_NE(nullptr, raw);
  raw->ctx = ctx.get();
  EXPECT_EQ(nullptr, raw->opaque);
  EXPECT_EQ(nullptr, pj->destructor(raw, 0));
}

}  // namespace
}  // namespace proj
