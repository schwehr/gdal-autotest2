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
// Unit tests for the Bacon Globular, Apian Globular I, and Ortelius Oval
// projections (proj/src/projections/bacon.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/bacon.cpp

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
constexpr double kHalfPiSq = 2.46740110027233965467;
constexpr double kEps = 1e-10;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(BaconTest, MetadataAndForwardOnlySetupForBaconApianAndOrtel) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct ProjMetaCase {
    const char* def;
    const char* expected_id;
    const char* expected_desc;
  };
  constexpr ProjMetaCase kMetaCases[] = {
      {.def = "+proj=bacon +ellps=WGS84",
       .expected_id = "bacon",
       .expected_desc = "Bacon Globular"},
      {.def = "+proj=apian +ellps=WGS84",
       .expected_id = "apian",
       .expected_desc = "Apian Globular I"},
      {.def = "+proj=ortel +ellps=WGS84",
       .expected_id = "ortel",
       .expected_desc = "Ortelius Oval"},
  };

  for (const auto& tc : kMetaCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_EQ(0, pj_has_inverse(pj.get()));
    EXPECT_EQ(0.0, pj->es);

    const PJ_PROJ_INFO info = proj_pj_info(pj.get());
    EXPECT_STREQ(tc.expected_id, info.id);
    EXPECT_NE(std::string_view(pj->descr).find(tc.expected_desc),
              std::string_view::npos);
    EXPECT_EQ(0, info.has_inverse);
  }
}

TEST(BaconTest, BaconGlobularForwardPrimeMeridianAndHemispheres) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=bacon +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const double phi = DegToRad(30.0);
  const double expected_y = kHalfPi * std::sin(phi);

  // 1. |lam| < EPS branch sets x = 0.0 and y = (pi/2) * sin(phi).
  const PJ_COORD pm_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, pm_fwd.xy.x);
  EXPECT_NEAR(expected_y, pm_fwd.xy.y, 1e-12);

  // 2. Positive and negative longitudes (including |lam| >= pi/2) use the
  // circular arc formula with y = (pi/2) * sin(phi).
  constexpr double kBaconLamsDeg[] = {45.0, -45.0, 120.0, -120.0};
  for (const double lam_deg : kBaconLamsDeg) {
    const double lam = DegToRad(lam_deg);
    const double ax = std::abs(lam);
    const double f = 0.5 * (kHalfPiSq / ax + ax);
    const double expected_x =
        std::copysign(ax - f + std::sqrt(f * f - expected_y * expected_y), lam);

    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected_x, fwd.xy.x, 1e-12);
    EXPECT_NEAR(expected_y, fwd.xy.y, 1e-12);
  }
}

TEST(BaconTest, ApianGlobularForwardLinearLatitudeAndCircularMeridians) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=apian +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const double phi = DegToRad(-40.0);

  // 1. Near-zero longitude (|lam| < EPS) sets x = 0.0 and y = phi.
  const PJ_COORD tiny_lam =
      proj_trans(pj.get(), PJ_FWD, proj_coord(1e-12, phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, tiny_lam.xy.x);
  EXPECT_NEAR(phi, tiny_lam.xy.y, 1e-12);

  // 2. Non-zero positive and negative longitudes use y = phi and circular arcs
  // even when |lam| >= pi/2 (since Q->ortl == 0).
  constexpr double kApianLamsDeg[] = {60.0, -60.0, 110.0, -110.0};
  for (const double lam_deg : kApianLamsDeg) {
    const double lam = DegToRad(lam_deg);
    const double ax = std::abs(lam);
    const double f = 0.5 * (kHalfPiSq / ax + ax);
    const double expected_x =
        std::copysign(ax - f + std::sqrt(f * f - phi * phi), lam);

    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected_x, fwd.xy.x, 1e-12);
    EXPECT_NEAR(phi, fwd.xy.y, 1e-12);
  }
}

TEST(BaconTest, OrteliusOvalInnerAndOuterHemisphereFormulas) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj ortel(proj_create(ctx.get(), "+proj=ortel +R=1"));
  ScopedPj apian(proj_create(ctx.get(), "+proj=apian +R=1"));
  ASSERT_NE(nullptr, ortel.get());
  ASSERT_NE(nullptr, apian.get());

  const double phi = DegToRad(35.0);

  // 1. Inside the central hemisphere (|lam| < pi/2), Ortelius Oval is identical
  // to Apian Globular I.
  constexpr double kInnerLamsDeg[] = {0.0, 40.0, -40.0, 89.0, -89.0};
  for (const double lam_deg : kInnerLamsDeg) {
    const double lam = DegToRad(lam_deg);
    const PJ_COORD o_fwd =
        proj_trans(ortel.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    const PJ_COORD a_fwd =
        proj_trans(apian.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(ortel.get()));
    EXPECT_NEAR(a_fwd.xy.x, o_fwd.xy.x, 1e-12);
    EXPECT_NEAR(a_fwd.xy.y, o_fwd.xy.y, 1e-12);
  }

  // 2. Outside the central hemisphere (|lam| >= pi/2), Ortelius Oval takes the
  // Q->ortl && ax >= M_HALFPI branch:
  // |x| = sqrt((pi/2)^2 - phi^2 + EPS) + |lam| - pi/2, y = phi.
  constexpr double kOuterLamsDeg[] = {90.0,   -90.0, 135.0,
                                      -135.0, 180.0, -180.0};
  for (const double lam_deg : kOuterLamsDeg) {
    const double lam = DegToRad(lam_deg);
    const double ax = std::abs(lam);
    const double expected_x = std::copysign(
        std::sqrt(kHalfPiSq - phi * phi + kEps) + ax - kHalfPi, lam);

    const PJ_COORD o_fwd =
        proj_trans(ortel.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(ortel.get()));
    EXPECT_NEAR(expected_x, o_fwd.xy.x, 1e-12);
    EXPECT_NEAR(phi, o_fwd.xy.y, 1e-12);
  }
}

}  // namespace
}  // namespace proj
