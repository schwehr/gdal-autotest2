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
// Unit tests for the Wagner II pseudocylindrical projection
// (proj/src/projections/wag2.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/wag2.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kCx = 0.92483;
constexpr double kCy = 1.38725;
constexpr double kCp1 = 0.88022;
constexpr double kCp2 = 0.88550;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Wag2Test, ClosedFormForwardAndInverseOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=wag2 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  EXPECT_EQ(0.0, pj->es);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("wag2", info.id);

  // On unit sphere:
  //   psi = asin(C_p1 * sin(C_p2 * phi))
  //   x = C_x * lam * cos(psi)
  //   y = C_y * psi
  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 0.0},
      {.lon_deg = 0.0, .lat_deg = 45.0},
      {.lon_deg = 30.0, .lat_deg = 60.0},
      {.lon_deg = -120.0, .lat_deg = -45.0},
      {.lon_deg = 180.0, .lat_deg = 90.0},
      {.lon_deg = -180.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);
    const double psi = std::asin(kCp1 * std::sin(kCp2 * phi));
    const double expected_x = kCx * lam * std::cos(psi);
    const double expected_y = kCy * psi;

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

TEST(Wag2Test, InverseOutOfDomainYTriggersProjectionDomainError) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=wag2 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // At y = kCy * (pi / 2), sin(y / kCy) / kCp1 = 1 / 0.88022 > 1.0, which
  // exceeds aasin's domain and sets
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV,
                                  proj_coord(0.0, kCy * (kPi / 2.0), 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inv.lp.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(Wag2Test, EllipsoidForcedToSphereClassic2DBatchAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=wag2 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0.0, pj->es);

  const PJ_LP lp = {.lam = DegToRad(-50.0), .phi = DegToRad(35.0)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, inv_lp.lam, 1e-12);
  EXPECT_NEAR(lp.phi, inv_lp.phi, 1e-12);

  PJ_COORD batch[2] = {
      proj_coord(DegToRad(20.0), DegToRad(40.0), 0.0, 0.0),
      proj_coord(DegToRad(-80.0), DegToRad(-55.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, batch));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, batch));
  EXPECT_NEAR(DegToRad(20.0), batch[0].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(40.0), batch[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(-80.0), batch[1].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-55.0), batch[1].lp.phi, 1e-12);

  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kCx, factors.parallel_scale, 1e-6);
  EXPECT_GT(factors.meridional_scale, 0.0);
}

}  // namespace
}  // namespace proj
