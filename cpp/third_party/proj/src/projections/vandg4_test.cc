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
// Unit tests for the van der Grinten IV projection
// (proj/src/projections/vandg4.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/vandg4.cpp

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

TEST(Vandg4Test, MetadataAndNonInvertibility) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=vandg4 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("vandg4", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("van der Grinten IV"),
            std::string_view::npos);
  EXPECT_EQ(0, info.has_inverse);
  EXPECT_EQ(0, pj_has_inverse(pj.get()));

  ScopedLogSuppressor suppressor(ctx.get());
  const PJ_COORD inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.2, 0.3, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv.xyzt.x);
  EXPECT_NE(0, proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(Vandg4Test, EquatorCentralMeridianAndPoleSpecialCases) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=vandg4 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Equator (|phi| < 1e-10): x = lam, y = 0.
  const double lam = DegToRad(60.0);
  const PJ_COORD eq =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, eq.xy.x, 1e-12);
  EXPECT_NEAR(0.0, eq.xy.y, 1e-12);

  // 2. Central meridian (|lam| < 1e-10, |phi| >= 1e-10): x = 0, y = phi.
  const double phi = DegToRad(35.0);
  const PJ_COORD mer =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, mer.xy.x, 1e-12);
  EXPECT_NEAR(phi, mer.xy.y, 1e-12);

  // 3. North and South Poles (||phi| - pi/2| < 1e-10): x = 0, y = phi.
  const PJ_COORD north =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, north.xy.x, 1e-12);
  EXPECT_NEAR(kHalfPi, north.xy.y, 1e-12);

  const PJ_COORD south =
      proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, south.xy.x, 1e-12);
  EXPECT_NEAR(-kHalfPi, south.xy.y, 1e-12);
}

TEST(Vandg4Test, GeneralForwardInnerOuterLongitudeAndFourQuadrantSymmetry) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=vandg4 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // Test longitudes inside (|lam| < pi/2 -> dt = -dt), on (|lam| == pi/2 ->
  // dt = 0), and outside (|lam| > pi/2 -> dt > 0) the 90-degree meridian
  // circle.
  constexpr double kLonDegs[] = {45.0, 90.0, 135.0};
  const double phi = DegToRad(40.0);

  for (const double lon_deg : kLonDegs) {
    const double lam = DegToRad(lon_deg);

    // 1. First quadrant (+lam, +phi).
    const PJ_COORD ne =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_GT(ne.xy.x, 0.0);
    EXPECT_GT(ne.xy.y, 0.0);

    // 2. Second quadrant (-lam, +phi) exercises `if (lp.lam < 0.) xy.x =
    // -xy.x`.
    const PJ_COORD nw =
        proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(-ne.xy.x, nw.xy.x, 1e-12);
    EXPECT_NEAR(ne.xy.y, nw.xy.y, 1e-12);

    // 3. Fourth quadrant (+lam, -phi) exercises `if (lp.phi < 0.) xy.y =
    // -xy.y`.
    const PJ_COORD se =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, -phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(ne.xy.x, se.xy.x, 1e-12);
    EXPECT_NEAR(-ne.xy.y, se.xy.y, 1e-12);

    // 4. Third quadrant (-lam, -phi) exercises both sign-flip branches.
    const PJ_COORD sw =
        proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, -phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(-ne.xy.x, sw.xy.x, 1e-12);
    EXPECT_NEAR(-ne.xy.y, sw.xy.y, 1e-12);
  }

  // At |lam| = pi/2, dt == 0, so (x / (pi/2))^2 + (y / (pi/2))^2 == 1.
  const PJ_XY on_circle = pj_fwd({.lam = kHalfPi, .phi = phi}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kHalfPi * kHalfPi,
              on_circle.x * on_circle.x + on_circle.y * on_circle.y, 1e-12);
}

}  // namespace
}  // namespace proj
