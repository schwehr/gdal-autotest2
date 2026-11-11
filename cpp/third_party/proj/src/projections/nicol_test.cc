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
// Unit tests for the Nicolosi Globular projection
// (proj/src/projections/nicol.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/nicol.cpp

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

TEST(NicolTest, MetadataAndNonInvertibility) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nicol +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("nicol", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Nicolosi Globular"),
            std::string_view::npos);
  EXPECT_EQ(0, info.has_inverse);
  EXPECT_EQ(0, pj_has_inverse(pj.get()));

  ScopedLogSuppressor suppressor(ctx.get());
  const PJ_COORD inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.3, 0.4, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv.xyzt.x);
  EXPECT_NE(0, proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(NicolTest, SpecialBranchesCentralMeridianEquatorBoundingCircleAndPoles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nicol +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const double lam = DegToRad(40.0);
  const double phi = DegToRad(35.0);

  // 1. Branch 1: |lam| < 1e-10 (central meridian) -> x = 0, y = phi.
  const PJ_COORD central =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, central.xy.x, 1e-12);
  EXPECT_NEAR(phi, central.xy.y, 1e-12);

  // 2. Branch 2: |phi| < 1e-10 with |lam| >= 1e-10 (equator) -> x = lam, y = 0.
  const PJ_COORD equator =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lam, equator.xy.x, 1e-12);
  EXPECT_NEAR(0.0, equator.xy.y, 1e-12);

  // 3. Branch 3: ||lam| - pi/2| < 1e-10 with |phi| >= 1e-10 (90-degree
  //    bounding circle meridians) -> x = lam * cos(phi), y = (pi/2) * sin(phi).
  const PJ_COORD circle_east =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kHalfPi, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kHalfPi * std::cos(phi), circle_east.xy.x, 1e-12);
  EXPECT_NEAR(kHalfPi * std::sin(phi), circle_east.xy.y, 1e-12);

  const PJ_COORD circle_west =
      proj_trans(pj.get(), PJ_FWD, proj_coord(-kHalfPi, -phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kHalfPi * std::cos(phi), circle_west.xy.x, 1e-12);
  EXPECT_NEAR(-kHalfPi * std::sin(phi), circle_west.xy.y, 1e-12);

  // 4. Branch 4: ||phi| - pi/2| < 1e-10 with |lam| != 0 and |lam| != pi/2
  //    (North and South Poles) -> x = 0, y = phi.
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lam, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, north_pole.xy.x, 1e-12);
  EXPECT_NEAR(kHalfPi, north_pole.xy.y, 1e-12);

  const PJ_COORD south_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, south_pole.xy.x, 1e-12);
  EXPECT_NEAR(-kHalfPi, south_pole.xy.y, 1e-12);
}

TEST(NicolTest, GeneralCircularArcFormulaAndFourQuadrantSymmetry) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=nicol +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // Test both inside (|lam| < pi/2) and outside (|lam| > pi/2) the hemisphere
  // across all four quadrants to cover `lp.lam < 0.` and `lp.phi < 0.`
  // branches.
  constexpr double kLonDegs[] = {45.0, 120.0};
  const double phi = DegToRad(30.0);

  for (const double lon_deg : kLonDegs) {
    const double lam = DegToRad(lon_deg);

    const PJ_COORD ne =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    const PJ_COORD nw =
        proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, phi, 0.0, 0.0));
    const PJ_COORD se =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, -phi, 0.0, 0.0));
    const PJ_COORD sw =
        proj_trans(pj.get(), PJ_FWD, proj_coord(-lam, -phi, 0.0, 0.0));

    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_GT(ne.xy.x, 0.0);
    EXPECT_GT(ne.xy.y, 0.0);

    EXPECT_NEAR(-ne.xy.x, nw.xy.x, 1e-12);
    EXPECT_NEAR(ne.xy.y, nw.xy.y, 1e-12);

    EXPECT_NEAR(ne.xy.x, se.xy.x, 1e-12);
    EXPECT_NEAR(-ne.xy.y, se.xy.y, 1e-12);

    EXPECT_NEAR(-ne.xy.x, sw.xy.x, 1e-12);
    EXPECT_NEAR(-ne.xy.y, sw.xy.y, 1e-12);
  }

  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(30.0), DegToRad(25.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(factors.meridional_scale, 0.0);
  EXPECT_GT(factors.parallel_scale, 0.0);
}

}  // namespace
}  // namespace proj
