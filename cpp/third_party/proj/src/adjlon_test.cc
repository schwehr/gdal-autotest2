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
// Unit tests for longitude normalization to [-pi, pi] (proj/src/adjlon.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/adjlon.cpp

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
constexpr double kSphereRadiusM = 6378137.0;

TEST(AdjlonTest, ReturnsUnchangedWithinPiPlusTolerance) {
  // adjlon() intentionally allows a 1e-12 rad overshoot past +-pi to avoid
  // spurious sign flips right on the antimeridian.
  constexpr double kCases[] = {
      0.0, -0.0, 1.0,         -1.0,           kPi / 2.0,     -kPi / 2.0,
      kPi, -kPi, kPi + 5e-13, -(kPi + 5e-13), kPi + 9.9e-13, -(kPi + 9.9e-13),
  };

  for (const double input : kCases) {
    const double actual = adjlon(input);
    EXPECT_DOUBLE_EQ(input, actual) << "input=" << input;
  }

  // Signed zero passes straight through without arithmetic alteration.
  EXPECT_FALSE(std::signbit(adjlon(0.0)));
  EXPECT_TRUE(std::signbit(adjlon(-0.0)));
}

TEST(AdjlonTest, WrapsLongitudesBeyondToleranceToMinusPiToPi) {
  struct WrapCase {
    double input_rad;
    double expected_rad;
    double tol_rad;
  };
  constexpr WrapCase kCases[] = {
      // Just beyond the pi + 1e-12 antimeridian overshoot threshold.
      {
          .input_rad = kPi + 2e-12,
          .expected_rad = -kPi + 2e-12,
          .tol_rad = 1e-15,
      },
      {
          .input_rad = -(kPi + 2e-12),
          .expected_rad = kPi - 2e-12,
          .tol_rad = 1e-15,
      },
      // Half- and full-revolution angles on both sides of zero.
      {.input_rad = 1.5 * kPi, .expected_rad = -0.5 * kPi, .tol_rad = 1e-14},
      {.input_rad = -1.5 * kPi, .expected_rad = 0.5 * kPi, .tol_rad = 1e-14},
      {.input_rad = 2.0 * kPi, .expected_rad = 0.0, .tol_rad = 1e-14},
      {.input_rad = -2.0 * kPi, .expected_rad = 0.0, .tol_rad = 1e-14},
      {.input_rad = 2.5 * kPi, .expected_rad = 0.5 * kPi, .tol_rad = 1e-14},
      {.input_rad = -2.5 * kPi, .expected_rad = -0.5 * kPi, .tol_rad = 1e-14},
      {.input_rad = 3.0 * kPi, .expected_rad = -kPi, .tol_rad = 1e-14},
      {.input_rad = -3.0 * kPi, .expected_rad = -kPi, .tol_rad = 1e-14},
      {.input_rad = 3.25 * kPi, .expected_rad = -0.75 * kPi, .tol_rad = 1e-14},
      {.input_rad = -3.25 * kPi, .expected_rad = 0.75 * kPi, .tol_rad = 1e-14},
      // Multiple-revolution wrapping.
      {.input_rad = 10.0 * kPi + 0.25, .expected_rad = 0.25, .tol_rad = 1e-12},
      {
          .input_rad = -10.0 * kPi - 0.25,
          .expected_rad = -0.25,
          .tol_rad = 1e-12,
      },
      {
          .input_rad = 100.0 * kPi - 0.75,
          .expected_rad = -0.75,
          .tol_rad = 1e-12,
      },
      {
          .input_rad = -100.0 * kPi + 0.75,
          .expected_rad = 0.75,
          .tol_rad = 1e-12,
      },
  };

  for (const auto& tc : kCases) {
    const double actual = adjlon(tc.input_rad);
    EXPECT_NEAR(tc.expected_rad, actual, tc.tol_rad)
        << "input_rad=" << tc.input_rad;
    EXPECT_GE(actual, -kPi - 1e-15);
    EXPECT_LE(actual, kPi + 1e-15);
  }
}

TEST(AdjlonTest, NonFiniteInputsReturnNan) {
  constexpr double kQnan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();

  EXPECT_TRUE(std::isnan(adjlon(kQnan)));
  EXPECT_TRUE(std::isnan(adjlon(kInf)));
  EXPECT_TRUE(std::isnan(adjlon(-kInf)));
}

TEST(AdjlonTest, ProjTransHonorsOverAndLonWrapViaAdjlon) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Without +over, forward Mercator wraps 1.5*pi rad via adjlon to -0.5*pi.
  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +R=6378137"));
  ASSERT_NE(nullptr, merc.get());
  const PJ_COORD wrapped =
      proj_trans(merc.get(), PJ_FWD, proj_coord(1.5 * kPi, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_NEAR(-kSphereRadiusM * 0.5 * kPi, wrapped.xy.x, 1e-6);
  EXPECT_NEAR(0.0, wrapped.xy.y, 1e-9);

  // 2. With +over, forward Mercator bypasses adjlon and keeps 1.5*pi.
  ScopedPj merc_over(proj_create(ctx.get(), "+proj=merc +R=6378137 +over"));
  ASSERT_NE(nullptr, merc_over.get());
  const PJ_COORD unwrapped =
      proj_trans(merc_over.get(), PJ_FWD, proj_coord(1.5 * kPi, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc_over.get()));
  EXPECT_NEAR(kSphereRadiusM * 1.5 * kPi, unwrapped.xy.x, 1e-6);
  EXPECT_NEAR(0.0, unwrapped.xy.y, 1e-9);

  // 3. Inverse Mercator with +lon_0=90 wraps 0.75*pi + 0.5*pi = 1.25*pi to
  //    -0.75*pi via adjlon unless +over is specified.
  ScopedPj merc_lon0(proj_create(ctx.get(), "+proj=merc +R=6378137 +lon_0=90"));
  ASSERT_NE(nullptr, merc_lon0.get());
  const PJ_COORD inv_wrapped =
      proj_trans(merc_lon0.get(), PJ_INV,
                 proj_coord(kSphereRadiusM * 0.75 * kPi, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc_lon0.get()));
  EXPECT_NEAR(-0.75 * kPi, inv_wrapped.lp.lam, 1e-12);
  EXPECT_NEAR(0.0, inv_wrapped.lp.phi, 1e-12);

  // 4. +lon_wrap=180 centers output longitudes around pi rad via
  //    long_wrap_center + adjlon(lam - long_wrap_center).
  ScopedPj wrap_180(
      proj_create(ctx.get(), "+proj=longlat +R=6378137 +lon_wrap=180"));
  ASSERT_NE(nullptr, wrap_180.get());
  const PJ_COORD shifted =
      proj_trans(wrap_180.get(), PJ_FWD, proj_coord(-0.5 * kPi, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(wrap_180.get()));
  EXPECT_NEAR(1.5 * kPi, shifted.lp.lam, 1e-12);
  EXPECT_NEAR(0.0, shifted.lp.phi, 1e-12);
}

}  // namespace
}  // namespace proj
