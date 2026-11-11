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
// Unit tests for the Goode Homolosine pseudocylindrical projection
// (proj/src/projections/goode.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/goode.cpp

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
constexpr double kYCor = 0.05280;
constexpr double kPhiLim = 0.71093078197902358062;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(GoodeTest, MetadataSphericalSetupAndDestructorNullGuards) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=goode +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("goode", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Goode Homolosine"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);

  // Verify the null-pointer and null-opaque guards in goode_destructor.
  ASSERT_NE(nullptr, pj->destructor);
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));
  PJ* bare_pj = pj_new();
  ASSERT_NE(nullptr, bare_pj);
  EXPECT_EQ(nullptr, pj->destructor(bare_pj, 0));
}

TEST(GoodeTest, ForwardAndInverseMatchSinusoidalAndShiftedMollweideZones) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj goode(proj_create(ctx.get(), "+proj=goode +R=1"));
  ScopedPj sinu(proj_create(ctx.get(), "+proj=sinu +R=1"));
  ScopedPj moll(proj_create(ctx.get(), "+proj=moll +R=1"));
  ASSERT_NE(nullptr, goode.get());
  ASSERT_NE(nullptr, sinu.get());
  ASSERT_NE(nullptr, moll.get());

  // 1. Low-latitude zone (|phi| <= PHI_LIM) delegates directly to Sinusoidal.
  constexpr double kLowLatRads[] = {
      0.0, DegToRad(20.0), DegToRad(-20.0), kPhiLim, -kPhiLim,
  };
  for (const double phi : kLowLatRads) {
    const double lam = DegToRad(35.0);
    const PJ_COORD g_fwd =
        proj_trans(goode.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    const PJ_COORD s_fwd =
        proj_trans(sinu.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(goode.get()));
    EXPECT_NEAR(s_fwd.xy.x, g_fwd.xy.x, 1e-12);
    EXPECT_NEAR(s_fwd.xy.y, g_fwd.xy.y, 1e-12);

    const PJ_COORD g_inv = proj_trans(goode.get(), PJ_INV, g_fwd);
    EXPECT_EQ(0, proj_errno(goode.get()));
    EXPECT_NEAR(lam, g_inv.lp.lam, 1e-12);
    EXPECT_NEAR(phi, g_inv.lp.phi, 1e-12);
  }

  // 2. High-latitude zones (|phi| > PHI_LIM) in both northern (phi > 0) and
  // southern (phi < 0) hemispheres delegate to Mollweide shifted by -+Y_COR.
  constexpr double kHighLatRads[] = {
      kPhiLim + 1e-4, -(kPhiLim + 1e-4), DegToRad(50.0), DegToRad(-50.0),
      DegToRad(75.0), DegToRad(-75.0),   kHalfPi,        -kHalfPi,
  };
  for (const double phi : kHighLatRads) {
    const double lam = DegToRad(-60.0);
    const PJ_COORD g_fwd =
        proj_trans(goode.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    const PJ_COORD m_fwd =
        proj_trans(moll.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(goode.get()));
    const double expected_y = m_fwd.xy.y - (phi >= 0.0 ? kYCor : -kYCor);
    EXPECT_NEAR(m_fwd.xy.x, g_fwd.xy.x, 1e-12);
    EXPECT_NEAR(expected_y, g_fwd.xy.y, 1e-12);

    const PJ_COORD g_inv = proj_trans(goode.get(), PJ_INV, g_fwd);
    EXPECT_EQ(0, proj_errno(goode.get()));
    if (std::abs(std::abs(phi) - kHalfPi) > 1e-9) {
      EXPECT_NEAR(lam, g_inv.lp.lam, 1e-6);
    }
    EXPECT_NEAR(phi, g_inv.lp.phi, 1e-6);
  }
}

TEST(GoodeTest, EqualAreaFactorsAndInverseOutOfDomainErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=goode +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Both sinusoidal and Mollweide lobes are equal-area (areal_scale == 1.0).
  const PJ_FACTORS f_low =
      proj_factors(pj.get(), proj_coord(DegToRad(15.0), DegToRad(25.0), 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, f_low.areal_scale, 1e-5);

  const PJ_FACTORS f_high =
      proj_factors(pj.get(), proj_coord(DegToRad(15.0), DegToRad(60.0), 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, f_high.areal_scale, 1e-5);

  // 2. Out-of-domain Y in the Mollweide zone (|y| > sqrt(2) * pi / 2) fails.
  const PJ_COORD inv_bad =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 3.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv_bad.lp.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

}  // namespace
}  // namespace proj
