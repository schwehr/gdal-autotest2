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
// Unit tests for the Eckert III (eck3), Putnins P1 (putp1), Wagner VI (wag6),
// and Kavrayskiy VII (kav7) pseudocylindrical projections
// (proj/src/projections/eck3.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/eck3.cpp

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

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

struct VariantSpec {
  const char* proj_str;
  double cx;
  double cy;
  double a;
  double b;
};

constexpr VariantSpec kVariants[] = {
    {
        .proj_str = "+proj=eck3 +R=1",
        .cx = 0.42223820031577120149,
        .cy = 0.84447640063154240298,
        .a = 1.0,
        .b = 0.4052847345693510857755,
    },
    {
        .proj_str = "+proj=kav7 +R=1",
        .cx = 0.8660254037844,
        .cy = 1.0,
        .a = 0.0,
        .b = 0.30396355092701331433,
    },
    {
        .proj_str = "+proj=wag6 +R=1",
        .cx = 1.0,
        .cy = 1.0,
        .a = 0.0,
        .b = 0.30396355092701331433,
    },
    {
        .proj_str = "+proj=putp1 +R=1",
        .cx = 1.89490,
        .cy = 0.94745,
        .a = -0.5,
        .b = 0.30396355092701331433,
    },
};

TEST(Eck3Test, AllFourProjectionsForwardFormulaAndInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct TestPoint {
    double lon_deg;
    double lat_deg;
  };
  constexpr TestPoint kPoints[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 30.0},
      {.lon_deg = -120.0, .lat_deg = -60.0},
      {.lon_deg = 180.0, .lat_deg = 89.0},
      {.lon_deg = -180.0, .lat_deg = -89.0},
      {.lon_deg = 90.0, .lat_deg = 75.0},
  };

  for (const auto& variant : kVariants) {
    ScopedPj pj(proj_create(ctx.get(), variant.proj_str));
    ASSERT_NE(nullptr, pj.get()) << variant.proj_str;
    EXPECT_EQ(1, pj_has_inverse(pj.get()));

    for (const auto& pt : kPoints) {
      const double lam = DegToRad(pt.lon_deg);
      const double phi = DegToRad(pt.lat_deg);

      // 1. Forward projection matches analytic formula on unit sphere.
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get()));

      const double expected_y = variant.cy * phi;
      const double expected_x =
          variant.cx * lam *
          (variant.a + std::sqrt(1.0 - variant.b * phi * phi));
      EXPECT_NEAR(expected_x, fwd.xy.x, 1e-12) << variant.proj_str;
      EXPECT_NEAR(expected_y, fwd.xy.y, 1e-12) << variant.proj_str;

      // 2. Inverse projection recovers (lam, phi).
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(lam, inv.lp.lam, 1e-12) << variant.proj_str;
      EXPECT_NEAR(phi, inv.lp.phi, 1e-12) << variant.proj_str;
    }

    // 3. Verify scale factors at a mid-latitude point:
    //    dx/dphi = -C_x * lam * B * phi / sqrt(1 - B * phi^2)
    //    dy/dphi = C_y
    //    h = hypot(dx/dphi, dy/dphi)
    //    k = C_x * (A + sqrt(1 - B * phi^2)) / cos(phi)
    const double factor_lam = DegToRad(15.0);
    const double factor_phi = DegToRad(40.0);
    const PJ_FACTORS factors =
        proj_factors(pj.get(), proj_coord(factor_lam, factor_phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    const double root = std::sqrt(1.0 - variant.b * factor_phi * factor_phi);
    const double dx_dphi =
        -variant.cx * factor_lam * variant.b * factor_phi / root;
    const double expected_h = std::hypot(dx_dphi, variant.cy);
    const double expected_k =
        variant.cx * (variant.a + root) / std::cos(factor_phi);
    EXPECT_NEAR(expected_h, factors.meridional_scale, 1e-5) << variant.proj_str;
    EXPECT_NEAR(expected_k, factors.parallel_scale, 1e-5) << variant.proj_str;
  }
}

TEST(Eck3Test, InverseZeroDenominatorSetsHugeValWhenAsqrtClampsToZero) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  // For wag6 and kav7, Q->A == 0.0 and Q->B = 3 / pi^2 (~0.30396355).
  // When |y / C_y| >= pi / sqrt(3) (~1.8138), 1 - B * phi^2 <= 0 so
  // asqrt(...) == 0 and denominator == 0.0, hitting `if (denominator == 0.0)
  // lp.lam = HUGE_VAL;`.
  for (const char* defn : {"+proj=wag6 +R=1", "+proj=kav7 +R=1"}) {
    ScopedPj pj(proj_create(ctx.get(), defn));
    ASSERT_NE(nullptr, pj.get());

    const PJ_LP raw_inv = pj->inv({.x = 1.0, .y = 2.0}, pj.get());
    EXPECT_EQ(HUGE_VAL, raw_inv.lam);
    EXPECT_NEAR(2.0, raw_inv.phi, 1e-12);

    const PJ_LP wrapped_inv = pj_inv({.x = 1.0, .y = 2.0}, pj.get());
    EXPECT_EQ(HUGE_VAL, wrapped_inv.lam);
    EXPECT_EQ(HUGE_VAL, wrapped_inv.phi);
  }

  // Also exercise asqrt clamping in forward when |phi| > 1/sqrt(B) via raw fwd.
  ScopedPj pj_eck3(proj_create(ctx.get(), "+proj=eck3 +R=1"));
  ASSERT_NE(nullptr, pj_eck3.get());
  const PJ_XY raw_fwd = pj_eck3->fwd({.lam = 1.0, .phi = 2.0}, pj_eck3.get());
  EXPECT_NEAR(0.42223820031577120149, raw_fwd.x, 1e-12);
  EXPECT_NEAR(2.0 * 0.84447640063154240298, raw_fwd.y, 1e-12);
}

TEST(Eck3Test, EllipsoidDefinitionsForceSphericalEccentricityZero) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=eck3 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_DOUBLE_EQ(0.0, pj->es);

  auto coords = std::to_array<PJ_COORD>({
      proj_coord(DegToRad(10.0), DegToRad(20.0), 10.0, 2026.0),
      proj_coord(DegToRad(-45.0), DegToRad(60.0), 20.0, 2026.0),
  });
  const auto orig = coords;

  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  for (size_t i = 0; i < coords.size(); ++i) {
    EXPECT_NEAR(orig[i].lp.lam, coords[i].lp.lam, 1e-12);
    EXPECT_NEAR(orig[i].lp.phi, coords[i].lp.phi, 1e-12);
  }
}

}  // namespace
}  // namespace proj
