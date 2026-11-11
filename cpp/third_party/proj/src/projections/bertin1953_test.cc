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
// Unit tests for the Bertin 1953 (bertin1953) forward-only world projection
// (proj/src/projections/bertin1953.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/bertin1953.cpp

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

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(Bertin1953Test, SetupForcesFixedObliquePoleAndNoInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=bertin1953 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(0, pj_has_inverse(pj.get()));
  EXPECT_EQ(nullptr, pj->inv);
  EXPECT_NE(nullptr, pj->fwd);
  EXPECT_EQ(0.0, pj->es);
  EXPECT_EQ(0.0, pj->lam0);
  EXPECT_NEAR(DegToRad(-42.0), pj->phi0, 1e-12);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("bertin1953", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Bertin 1953"),
            std::string_view::npos);
  EXPECT_EQ(0, info.has_inverse);

  // Inverse transformation fails cleanly because bertin1953 is forward-only.
  const PJ_COORD inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inv.lp.lam));
  EXPECT_NE(0, proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(Bertin1953Test, ObliqueCenterMapsToOriginAndAllAdjustmentBranchesExecute) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=bertin1953 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. At (lon = +16.5 deg, lat = +42 deg), the rotation by lam += -16.5 deg
  // and phi0 = -42 deg maps the point to rotated (lam = 0, phi = 0), yielding
  // exact (0, 0) where neither xy.y < 0 nor xy.y > 0 post-adjustment triggers.
  const PJ_XY center =
      pj_fwd({.lam = DegToRad(16.5), .phi = DegToRad(42.0)}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, center.x, 1e-12);
  EXPECT_NEAR(0.0, center.y, 1e-12);

  // 2. Point in rotated northern hemisphere with non-zero rotated lam and phi:
  // exercises `if (xy.y > 0.)` post-projection adjustment (`xy.y *= 1 + ...`)
  // without triggering `lp.lam + lp.phi < -fu`.
  const PJ_XY north_pos =
      pj_fwd({.lam = DegToRad(60.0), .phi = DegToRad(60.0)}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(north_pos.x, 0.0);
  EXPECT_GT(north_pos.y, 0.0);

  // 3. Point in rotated southern hemisphere with `lp.lam + lp.phi >= -1.4`:
  // exercises `if (xy.y < 0.)` (`xy.x *= 1 + d`) without pre-adjustment.
  const PJ_XY south_mild =
      pj_fwd({.lam = DegToRad(30.0), .phi = DegToRad(10.0)}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(south_mild.x, 0.0);
  EXPECT_LT(south_mild.y, 0.0);

  // 4. Point in rotated southwest quadrant where `lp.lam + lp.phi < -1.4`
  // (`-fu`): exercises the pre-projection adjustment block
  // `if (lp.lam + lp.phi < -fu)` as well as `if (xy.y < 0.)`.
  const PJ_XY pre_adj =
      pj_fwd({.lam = DegToRad(-73.5), .phi = DegToRad(-60.0)}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_LT(pre_adj.x, 0.0);
  EXPECT_LT(pre_adj.y, 0.0);

  // 5. Verify batch forward transform and distortion factors.
  std::array<PJ_COORD, 3> coords = {
      proj_coord(DegToRad(2.3522), DegToRad(48.8566), 0.0, 0.0),    // Paris
      proj_coord(DegToRad(-73.5), DegToRad(-60.0), 0.0, 0.0),       // Pre-adj
      proj_coord(DegToRad(139.6917), DegToRad(35.6895), 0.0, 0.0),  // Tokyo
  };
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  for (const auto& c : coords) {
    EXPECT_FALSE(std::isinf(c.xy.x));
    EXPECT_FALSE(std::isinf(c.xy.y));
  }

  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(2.3522), DegToRad(48.8566), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(factors.meridional_scale, 0.0);
  EXPECT_GT(factors.parallel_scale, 0.0);
}

}  // namespace
}  // namespace proj
