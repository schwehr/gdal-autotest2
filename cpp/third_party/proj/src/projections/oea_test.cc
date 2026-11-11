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
// Unit tests for the Oblated Equal Area projection
// (proj/src/projections/oea.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/oea.cpp

#include <cmath>
#include <numbers>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(OeaTest, InvalidOrMissingNAndMParametersFailWithIllegalArgValue) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct InvalidParamCase {
    const char* proj_str;
    std::string_view expected_log_substr;
  };
  constexpr InvalidParamCase kInvalidCases[] = {
      // Missing n defaults to 0.0 <= 0.
      {.proj_str = "+proj=oea +m=1",
       .expected_log_substr = "Invalid value for n: it should be > 0"},
      {.proj_str = "+proj=oea +n=0 +m=1",
       .expected_log_substr = "Invalid value for n: it should be > 0"},
      {.proj_str = "+proj=oea +n=-1.5 +m=1",
       .expected_log_substr = "Invalid value for n: it should be > 0"},
      // Valid n > 0, missing or non-positive m.
      {.proj_str = "+proj=oea +n=1",
       .expected_log_substr = "Invalid value for m: it should be > 0"},
      {.proj_str = "+proj=oea +n=1 +m=0",
       .expected_log_substr = "Invalid value for m: it should be > 0"},
      {.proj_str = "+proj=oea +n=1 +m=-2.0",
       .expected_log_substr = "Invalid value for m: it should be > 0"},
  };

  for (const auto& tc : kInvalidCases) {
    ScopedLogCapture log_capture(ctx.get());
    proj_context_errno_set(ctx.get(), 0);

    ScopedPj pj(proj_create(ctx.get(), tc.proj_str));
    EXPECT_EQ(nullptr, pj.get()) << tc.proj_str;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << tc.proj_str;
    EXPECT_TRUE(
        log_capture.ContainsMessage(std::string(tc.expected_log_substr)))
        << tc.proj_str;
  }
}

TEST(OeaTest, ForwardAndInverseRoundTripAndEqualAreaProperty) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(), "+proj=oea +n=1.5 +m=2.0 +lat_0=30 +lon_0=10 +theta=25 +R=1"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("oea", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Oblated Equal Area"),
            std::string_view::npos);

  // 1. The projection center (lon_0 = 10d, lat_0 = 30d) maps to (0, 0).
  const PJ_COORD center_fwd = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(10.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, center_fwd.xy.x, 1e-12);
  EXPECT_NEAR(0.0, center_fwd.xy.y, 1e-12);

  // 2. Round-trip across all quadrants around the oblique center.
  struct LonLatCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr LonLatCase kCases[] = {
      {.lon_deg = 10.0, .lat_deg = 30.0},  {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 25.0, .lat_deg = 45.0},  {.lon_deg = -20.0, .lat_deg = 15.0},
      {.lon_deg = 40.0, .lat_deg = -10.0}, {.lon_deg = -35.0, .lat_deg = 60.0},
  };

  for (const auto& tc : kCases) {
    const double lon_rad = DegToRad(tc.lon_deg);
    const double lat_rad = DegToRad(tc.lat_deg);

    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(fwd.xy.x));
    EXPECT_FALSE(std::isinf(fwd.xy.y));

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-11);
    EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-11);
  }

  // 3. Verify equal-area distortion factor (areal_scale == 1.0).
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(DegToRad(20.0), DegToRad(35.0), 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-5);
}

TEST(OeaTest, InverseOutsideProjectionDomainSetsError) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=oea +n=1.0 +m=1.0 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // |xy.y * rn| > 1.000001 triggers aasin domain error in oea_s_inverse.
  const PJ_COORD inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 2.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv.lp.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

}  // namespace
}  // namespace proj
