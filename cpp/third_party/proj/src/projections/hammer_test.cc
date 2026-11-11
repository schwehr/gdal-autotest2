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
// Unit tests for the Hammer & Eckert-Greifendorff (hammer) projection
// (proj/src/projections/hammer.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/hammer.cpp

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

TEST(HammerTest, DefaultAndCustomWMParametersRoundTripAndPreserveEqualArea) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Default Hammer projection (W = 0.5, M = 1.0).
  ScopedPj pj_default(proj_create(ctx.get(), "+proj=hammer +R=1"));
  ASSERT_NE(nullptr, pj_default.get());
  EXPECT_EQ(0, proj_errno(pj_default.get()));
  EXPECT_EQ(1, pj_has_inverse(pj_default.get()));
  EXPECT_EQ(0.0, pj_default->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj_default.get());
  EXPECT_STREQ("hammer", info.id);
  EXPECT_NE(
      std::string_view(pj_default->descr).find("Hammer & Eckert-Greifendorff"),
      std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);

  // 2. Custom +W parameter (Eckert-Greifendorff W = 0.25, default M = 1.0).
  ScopedPj pj_custom_w(proj_create(ctx.get(), "+proj=hammer +W=0.25 +R=1"));
  ASSERT_NE(nullptr, pj_custom_w.get());
  EXPECT_EQ(0, proj_errno(pj_custom_w.get()));

  struct SampleCoord {
    double lon_deg;
    double lat_deg;
  };
  constexpr SampleCoord kCoords[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 30.0, .lat_deg = 45.0},
      {.lon_deg = -120.0, .lat_deg = 60.0},
      {.lon_deg = 95.0, .lat_deg = -35.0},
      {.lon_deg = -170.0, .lat_deg = -75.0},
  };

  for (PJ* pj : {pj_default.get(), pj_custom_w.get()}) {
    for (const auto& sc : kCoords) {
      const double lam = DegToRad(sc.lon_deg);
      const double phi = DegToRad(sc.lat_deg);
      const PJ_COORD fwd =
          proj_trans(pj, PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj));
      EXPECT_FALSE(std::isinf(fwd.xy.x));
      EXPECT_FALSE(std::isinf(fwd.xy.y));

      const PJ_COORD inv = proj_trans(pj, PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj));
      EXPECT_NEAR(lam, inv.lp.lam, 1e-11);
      EXPECT_NEAR(phi, inv.lp.phi, 1e-11);
    }

    // Equal-area check (areal_scale == 1.0).
    const PJ_FACTORS factors =
        proj_factors(pj, proj_coord(DegToRad(25.0), DegToRad(40.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj));
    EXPECT_NEAR(1.0, factors.areal_scale, 1e-5);
  }

  // 3. Custom +M parameter scales forward x by M and y by 1/M while preserving
  // unit areal scale.
  constexpr double kCustomM = 2.0;
  ScopedPj pj_custom_m(
      proj_create(ctx.get(), "+proj=hammer +W=0.25 +M=2.0 +R=1"));
  ASSERT_NE(nullptr, pj_custom_m.get());
  EXPECT_EQ(0, proj_errno(pj_custom_m.get()));

  const PJ_COORD sample_lp =
      proj_coord(DegToRad(30.0), DegToRad(45.0), 0.0, 0.0);
  const PJ_COORD fwd_m1 = proj_trans(pj_custom_w.get(), PJ_FWD, sample_lp);
  const PJ_COORD fwd_m2 = proj_trans(pj_custom_m.get(), PJ_FWD, sample_lp);
  EXPECT_EQ(0, proj_errno(pj_custom_m.get()));
  EXPECT_NEAR(kCustomM * fwd_m1.xy.x, fwd_m2.xy.x, 1e-12);
  EXPECT_NEAR(fwd_m1.xy.y / kCustomM, fwd_m2.xy.y, 1e-12);

  const PJ_FACTORS factors_m2 = proj_factors(pj_custom_m.get(), sample_lp);
  EXPECT_EQ(0, proj_errno(pj_custom_m.get()));
  EXPECT_NEAR(1.0, factors_m2.areal_scale, 1e-5);
}

TEST(HammerTest, ZeroWOrZeroMParameterFailsWithIllegalArgValue) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);

  // 1. +W=0 triggers `if (Q->w <= 0.)`.
  ScopedPj pj_zero_w(proj_create(ctx.get(), "+proj=hammer +W=0"));
  EXPECT_EQ(nullptr, pj_zero_w.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for W: it should be > 0"));

  // 2. +M=0 triggers `if (Q->m <= 0.)`.
  logs.Clear();
  ScopedPj pj_zero_m(proj_create(ctx.get(), "+proj=hammer +M=0"));
  EXPECT_EQ(nullptr, pj_zero_m.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for M: it should be > 0"));
}

TEST(HammerTest, ForwardAntipodalDenominatorZeroAndInverseBoundaryFailCleanly) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  // 1. Forward singularity `denom == 0.0`: when phi = 0 (cosphi = 1) and
  // W * lam = pi (cos(W * lam) = -1), `1 + cosphi * cos(W * lam) == 0.0`.
  // With +W=1, lam = pi hits denom == 0.0 directly.
  ScopedPj pj_w1(proj_create(ctx.get(), "+proj=hammer +W=1 +R=1"));
  ASSERT_NE(nullptr, pj_w1.get());

  const PJ_XY fwd_err = pj_fwd({.lam = kPi, .phi = 0.0}, pj_w1.get());
  EXPECT_TRUE(std::isinf(fwd_err.x));
  EXPECT_TRUE(std::isinf(fwd_err.y));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_w1.get()));
  proj_errno_reset(pj_w1.get());

  // 2. Inverse boundary singularity `fabs(2. * z * z - 1.) < EPS`:
  // When x = 0 and y = sqrt(2), z = sqrt(1 - 0.25 * 2) = sqrt(0.5), so
  // 2 * z^2 - 1 == 0.0 (< 1e-10).
  ScopedPj pj_default(proj_create(ctx.get(), "+proj=hammer +R=1"));
  ASSERT_NE(nullptr, pj_default.get());

  const PJ_LP inv_err =
      pj_inv({.x = 0.0, .y = std::sqrt(2.0)}, pj_default.get());
  EXPECT_EQ(HUGE_VAL, inv_err.lam);
  EXPECT_EQ(HUGE_VAL, inv_err.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_default.get()));
  proj_errno_reset(pj_default.get());
}

}  // namespace
}  // namespace proj
