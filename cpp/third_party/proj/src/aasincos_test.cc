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
// Unit tests for domain-clamping inverse trig/math helpers aasin(), aacos(),
// asqrt(), and aatan2() (proj/src/aasincos.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/aasincos.cpp

#include <cmath>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

TEST(AasincosTest, AasinInDomainExactBoundsToleranceAndOutOfDomain) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Interior values match std::asin without setting errno.
  for (const double v :
       {-0.99, -0.5, -0.0, 0.0, 0.5, std::nextafter(1.0, 0.0)}) {
    const double actual = aasin(ctx.get(), v);
    EXPECT_EQ(0, proj_context_errno(ctx.get())) << "v=" << v;
    EXPECT_NEAR(std::asin(v), actual, 1e-15) << "v=" << v;
  }

  // 2. Exact boundary and overshoot within ONE_TOL (1 + 1e-14) clamp to +-pi/2
  // cleanly without setting an error.
  struct ClampCase {
    double input;
    double expected;
    int expected_errno;
  };
  constexpr ClampCase kBoundaryCases[] = {
      {.input = 1.0, .expected = kHalfPi, .expected_errno = 0},
      {.input = -1.0, .expected = -kHalfPi, .expected_errno = 0},
      {.input = 1.0 + 5e-15, .expected = kHalfPi, .expected_errno = 0},
      {.input = -(1.0 + 5e-15), .expected = -kHalfPi, .expected_errno = 0},
      // Exceeding ONE_TOL sets
      // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
      {.input = 1.0 + 2e-14,
       .expected = kHalfPi,
       .expected_errno = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN},
      {.input = -(1.0 + 2e-14),
       .expected = -kHalfPi,
       .expected_errno = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN},
      {.input = 2.5,
       .expected = kHalfPi,
       .expected_errno = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN},
      {.input = -2.5,
       .expected = -kHalfPi,
       .expected_errno = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN},
      {.input = kInf,
       .expected = kHalfPi,
       .expected_errno = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN},
      {.input = -kInf,
       .expected = -kHalfPi,
       .expected_errno = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN},
  };

  for (const auto& tc : kBoundaryCases) {
    proj_context_errno_set(ctx.get(), 0);
    const double actual = aasin(ctx.get(), tc.input);
    EXPECT_NEAR(tc.expected, actual, 1e-15) << "input=" << tc.input;
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get()))
        << "input=" << tc.input;
  }

  // 3. NaN bypasses the >= 1.0 branch and returns NaN with errno unchanged.
  proj_context_errno_set(ctx.get(), 0);
  EXPECT_TRUE(std::isnan(aasin(ctx.get(), kNan)));
  EXPECT_EQ(0, proj_context_errno(ctx.get()));
}

TEST(AasincosTest, AacosInDomainExactBoundsToleranceAndOutOfDomain) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Interior values match std::acos without setting errno.
  for (const double v : {std::nextafter(-1.0, 0.0), -0.5, -0.0, 0.0, 0.5,
                         std::nextafter(1.0, 0.0)}) {
    const double actual = aacos(ctx.get(), v);
    EXPECT_EQ(0, proj_context_errno(ctx.get())) << "v=" << v;
    EXPECT_NEAR(std::acos(v), actual, 1e-15) << "v=" << v;
  }

  // 2. Exact boundary, tolerance clamp, and out-of-domain error behavior.
  struct ClampCase {
    double input;
    double expected;
    int expected_errno;
  };
  constexpr ClampCase kBoundaryCases[] = {
      {.input = 1.0, .expected = 0.0, .expected_errno = 0},
      {.input = -1.0, .expected = kPi, .expected_errno = 0},
      {.input = 1.0 + 5e-15, .expected = 0.0, .expected_errno = 0},
      {.input = -(1.0 + 5e-15), .expected = kPi, .expected_errno = 0},
      {.input = 1.0 + 2e-14,
       .expected = 0.0,
       .expected_errno = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN},
      {.input = -(1.0 + 2e-14),
       .expected = kPi,
       .expected_errno = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN},
      {.input = 4.0,
       .expected = 0.0,
       .expected_errno = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN},
      {.input = -4.0,
       .expected = kPi,
       .expected_errno = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN},
      {.input = kInf,
       .expected = 0.0,
       .expected_errno = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN},
      {.input = -kInf,
       .expected = kPi,
       .expected_errno = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN},
  };

  for (const auto& tc : kBoundaryCases) {
    proj_context_errno_set(ctx.get(), 0);
    const double actual = aacos(ctx.get(), tc.input);
    EXPECT_NEAR(tc.expected, actual, 1e-15) << "input=" << tc.input;
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get()))
        << "input=" << tc.input;
  }

  // 3. NaN propagates cleanly without setting errno.
  proj_context_errno_set(ctx.get(), 0);
  EXPECT_TRUE(std::isnan(aacos(ctx.get(), kNan)));
  EXPECT_EQ(0, proj_context_errno(ctx.get()));
}

TEST(AasincosTest, AsqrtClampsNonPositiveAndPreservesPositiveInfNan) {
  for (const double v : {-kInf, -100.0, -1e-300, -0.0, 0.0}) {
    const double actual = asqrt(v);
    EXPECT_DOUBLE_EQ(0.0, actual) << "v=" << v;
    EXPECT_FALSE(std::signbit(actual)) << "v=" << v;
  }

  for (const double v : {1e-100, 0.25, 2.0, 9.0, 1e12}) {
    EXPECT_NEAR(std::sqrt(v), asqrt(v), 1e-15 * std::sqrt(v)) << "v=" << v;
  }

  EXPECT_EQ(kInf, asqrt(kInf));
  EXPECT_TRUE(std::isnan(asqrt(kNan)));
}

TEST(AasincosTest, Aatan2SuppressesNearZeroBothAndHandlesAxesQuadrantsNan) {
  struct Atan2Case {
    double n;
    double d;
    double expected;
  };
  constexpr Atan2Case kCases[] = {
      // Both |n| < 1e-50 and |d| < 1e-50 collapse to exact +0.0 (even for
      // -0.0).
      {.n = 0.0, .d = 0.0, .expected = 0.0},
      {.n = -0.0, .d = -0.0, .expected = 0.0},
      {.n = 1e-60, .d = -1e-60, .expected = 0.0},
      {.n = -1e-100, .d = 1e-100, .expected = 0.0},
      // Only one operand < 1e-50 falls through to std::atan2().
      {.n = 0.0, .d = 1.0, .expected = 0.0},
      {.n = 0.0, .d = -1.0, .expected = kPi},
      {.n = 0.0, .d = -1e-49, .expected = kPi},
      {.n = -0.0, .d = -1e-49, .expected = -kPi},
      {.n = 1e-60, .d = -1.0, .expected = kPi},
      {.n = -1e-60, .d = -1.0, .expected = -kPi},
      {.n = 1.0, .d = 0.0, .expected = kHalfPi},
      {.n = -1.0, .d = 1e-60, .expected = -kHalfPi},
      {.n = 1e-49, .d = 0.0, .expected = kHalfPi},
      // Standard four quadrants.
      {.n = 1.0, .d = 1.0, .expected = kPi / 4.0},
      {.n = 1.0, .d = -1.0, .expected = 3.0 * kPi / 4.0},
      {.n = -1.0, .d = -1.0, .expected = -3.0 * kPi / 4.0},
      {.n = -1.0, .d = 1.0, .expected = -kPi / 4.0},
  };

  for (const auto& tc : kCases) {
    const double actual = aatan2(tc.n, tc.d);
    EXPECT_NEAR(tc.expected, actual, 1e-15) << "n=" << tc.n << " d=" << tc.d;
    if (std::abs(tc.n) < 1e-50 && std::abs(tc.d) < 1e-50) {
      EXPECT_FALSE(std::signbit(actual));
    }
  }

  EXPECT_TRUE(std::isnan(aatan2(kNan, 1.0)));
  EXPECT_TRUE(std::isnan(aatan2(1.0, kNan)));
  EXPECT_TRUE(std::isnan(aatan2(kNan, kNan)));
}

TEST(AasincosTest, OutOfDomainProjectionInverseReportsAasinError) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj wag2(proj_create(ctx.get(), "+proj=wag2 +R=6371000"));
  ASSERT_NE(nullptr, wag2.get());

  // For wag2 inverse, phi' = y / (R * 1.38725), then
  // aasin(sin(phi') / 0.88022). Setting phi' = 2.0 rad makes
  // |sin(phi') / 0.88022| > 1.033 > ONE_TOL.
  const PJ_COORD out_of_domain =
      proj_coord(0.0, 2.0 * 1.38725 * 6371000.0, 0.0, 0.0);
  const PJ_COORD res = proj_trans(wag2.get(), PJ_INV, out_of_domain);
  EXPECT_TRUE(std::isinf(res.lp.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(wag2.get()));
  proj_errno_reset(wag2.get());
}

}  // namespace
}  // namespace proj
