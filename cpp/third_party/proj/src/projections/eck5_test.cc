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
// Unit tests for the Eckert V pseudocylindrical projection
// (proj/src/projections/eck5.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/eck5.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kXf = 0.44101277172455148219;
constexpr double kYf = 0.88202554344910296438;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Eck5Test, ClosedFormForwardAndInverseOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=eck5 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  EXPECT_EQ(0.0, pj->es);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("eck5", info.id);

  // On unit sphere:
  //   x = XF * (1 + cos(phi)) * lam
  //   y = YF * phi
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
    const double expected_x = kXf * (1.0 + std::cos(phi)) * lam;
    const double expected_y = kYf * phi;

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

TEST(Eck5Test, EllipsoidForcedToSphereClassic2DBatchAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=eck5 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0.0, pj->es);

  const PJ_LP lp = {.lam = DegToRad(40.0), .phi = DegToRad(-25.0)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, inv_lp.lam, 1e-12);
  EXPECT_NEAR(lp.phi, inv_lp.phi, 1e-12);

  PJ_COORD batch[2] = {
      proj_coord(DegToRad(15.0), DegToRad(35.0), 0.0, 0.0),
      proj_coord(DegToRad(-95.0), DegToRad(-50.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, batch));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, batch));
  EXPECT_NEAR(DegToRad(15.0), batch[0].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(35.0), batch[0].lp.phi, 1e-12);
  EXPECT_NEAR(DegToRad(-95.0), batch[1].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-50.0), batch[1].lp.phi, 1e-12);

  // At the equator (phi = 0), parallel scale is 2 * XF = YF and meridional
  // scale on the prime meridian is YF.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kYf, factors.parallel_scale, 1e-7);
  EXPECT_NEAR(kYf, factors.meridional_scale, 1e-7);
}

}  // namespace
}  // namespace proj
