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
// Unit tests for the Tobler-Mercator cylindrical projection
// (proj/src/projections/tobmerc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/tobmerc.cpp

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

TEST(TobmercTest, ClosedFormForwardAndInverseOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=tobmerc +R=1"));
  ASSERT_NE(nullptr, pj.get());

  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("tobmerc", info.id);

  // On unit sphere with k0 = 1:
  //   x = lam * cos^2(phi)
  //   y = asinh(tan(phi))
  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 0.0},
      {.lon_deg = 30.0, .lat_deg = 45.0},
      {.lon_deg = -60.0, .lat_deg = 60.0},
      {.lon_deg = 120.0, .lat_deg = -30.0},
      {.lon_deg = -150.0, .lat_deg = -75.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);
    const double cosphi = std::cos(phi);
    const double expected_x = lam * cosphi * cosphi;
    const double expected_y = std::asinh(std::tan(phi));

    // 1. Forward: (lam, phi) -> (x, y).
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected_x, fwd.xy.x, 1e-12);
    EXPECT_NEAR(expected_y, fwd.xy.y, 1e-12);

    // 2. Inverse: (x, y) -> (lam, phi).
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-12);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-12);
  }
}

TEST(TobmercTest, ForwardAtPolesSetsOutsideProjectionDomainError) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=tobmerc +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // fabs(lp.phi) >= M_HALFPI inside tobmerc_s_forward triggers
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  constexpr double kPoleLatitudes[] = {kHalfPi, -kHalfPi};
  for (const double phi : kPoleLatitudes) {
    const PJ_COORD out =
        proj_trans(pj.get(), PJ_FWD, proj_coord(DegToRad(10.0), phi, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(out.xy.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());

    const PJ_XY xy = pj_fwd({.lam = DegToRad(10.0), .phi = phi}, pj.get());
    EXPECT_TRUE(std::isinf(xy.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }

  // Direct call to pj->fwd with |phi| > pi/2 also hits the >= M_HALFPI guard.
  const PJ_XY over_pole =
      pj->fwd({.lam = DegToRad(10.0), .phi = kHalfPi + 0.1}, pj.get());
  EXPECT_EQ(0.0, over_pole.x);
  EXPECT_EQ(0.0, over_pole.y);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(TobmercTest, CustomScaleFactorK0BatchTransformAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  constexpr double kScale = 0.9996;
  ScopedPj pj(proj_create(ctx.get(), "+proj=tobmerc +R=1 +k_0=0.9996"));
  ASSERT_NE(nullptr, pj.get());

  const double lam = DegToRad(20.0);
  const double phi = DegToRad(50.0);
  const double cosphi = std::cos(phi);

  const PJ_LP lp = {.lam = lam, .phi = phi};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kScale * lam * cosphi * cosphi, xy.x, 1e-12);
  EXPECT_NEAR(kScale * std::asinh(std::tan(phi)), xy.y, 1e-12);

  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, inv_lp.lam, 1e-12);
  EXPECT_NEAR(phi, inv_lp.phi, 1e-12);

  PJ_COORD batch[2] = {
      proj_coord(DegToRad(12.0), DegToRad(34.0), 0.0, 0.0),
      proj_coord(DegToRad(-45.0), DegToRad(-23.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, batch));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, batch));
  EXPECT_NEAR(DegToRad(12.0), batch[0].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(34.0), batch[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(-45.0), batch[1].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-23.0), batch[1].lp.phi, 1e-12);

  // For k0 = 1 on unit sphere, meridional scale h = 1/cos(phi) and parallel
  // scale k = cos(phi), so areal scale s = h * k = 1.0 (equal-area). For custom
  // k0, areal scale is k0^2.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kScale * kScale, factors.areal_scale, 1e-6);
}

}  // namespace
}  // namespace proj
