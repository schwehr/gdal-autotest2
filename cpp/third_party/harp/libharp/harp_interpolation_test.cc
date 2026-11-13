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
// Unit tests for libharp 1D/2D interpolation and interval bounds functions
// (libharp/harp-interpolation.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-interpolation.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-internal.h

#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;

TEST(HarpInterpolationTest, FindIndexAscendingAndDescending) {
  constexpr auto kAsc = std::to_array<double>({10.0, 20.0, 30.0, 40.0, 50.0});
  constexpr long kAscLen = static_cast<long>(kAsc.size());

  struct FindCase {
    double target;
    long initial_guess;
    long expected_index;
  };
  constexpr FindCase kAscCases[] = {
      {.target = 5.0, .initial_guess = -1, .expected_index = -1},
      {.target = 5.0, .initial_guess = 0, .expected_index = -1},
      {.target = 5.0, .initial_guess = 4, .expected_index = -1},
      {.target = 10.0, .initial_guess = 3, .expected_index = 0},
      {.target = 25.0, .initial_guess = 0, .expected_index = 1},
      {.target = 25.0, .initial_guess = 4, .expected_index = 1},
      {.target = 30.0, .initial_guess = 10, .expected_index = 2},
      {.target = 45.0, .initial_guess = 1, .expected_index = 3},
      {.target = 50.0, .initial_guess = 0, .expected_index = 4},
      {.target = 60.0, .initial_guess = 4, .expected_index = 5},
      {.target = 60.0, .initial_guess = 0, .expected_index = 5},
      {.target = 60.0, .initial_guess = -5, .expected_index = 5},
  };
  for (const auto& tc : kAscCases) {
    long idx = tc.initial_guess;
    harp_interpolate_find_index(kAscLen, kAsc.data(), tc.target, &idx);
    EXPECT_EQ(tc.expected_index, idx);
  }

  constexpr auto kDesc = std::to_array<double>({50.0, 40.0, 30.0, 20.0, 10.0});
  constexpr long kDescLen = static_cast<long>(kDesc.size());
  constexpr FindCase kDescCases[] = {
      {.target = 60.0, .initial_guess = 0, .expected_index = -1},
      {.target = 60.0, .initial_guess = 4, .expected_index = -1},
      {.target = 50.0, .initial_guess = 3, .expected_index = 0},
      {.target = 35.0, .initial_guess = 0, .expected_index = 1},
      {.target = 35.0, .initial_guess = 4, .expected_index = 1},
      {.target = 20.0, .initial_guess = -1, .expected_index = 3},
      {.target = 10.0, .initial_guess = 0, .expected_index = 4},
      {.target = 5.0, .initial_guess = 4, .expected_index = 5},
      {.target = 5.0, .initial_guess = 1, .expected_index = 5},
  };
  for (const auto& tc : kDescCases) {
    long idx = tc.initial_guess;
    harp_interpolate_find_index(kDescLen, kDesc.data(), tc.target, &idx);
    EXPECT_EQ(tc.expected_index, idx);
  }
}

TEST(HarpInterpolationTest, CubicAndBicubicSplineInterpolation) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // Linear function y = 2*x + 1 is reproduced exactly by natural cubic spline.
  constexpr auto kX = std::to_array<double>({0.0, 1.0, 2.0, 4.0, 5.0});
  constexpr auto kY = std::to_array<double>({1.0, 3.0, 5.0, 9.0, 11.0});
  double yp = 0.0;
  ASSERT_EQ(HARP_SUCCESS, harp_cubic_spline_interpolation(kX.data(), kY.data(),
                                                          kX.size(), 2.5, &yp));
  EXPECT_NEAR(6.0, yp, 1e-12);
  ASSERT_EQ(HARP_SUCCESS, harp_cubic_spline_interpolation(kX.data(), kY.data(),
                                                          kX.size(), 0.5, &yp));
  EXPECT_NEAR(2.0, yp, 1e-12);

  // Duplicate x coordinates (n == 2) trigger "xx must be distinct" error.
  constexpr auto kDupX = std::to_array<double>({1.0, 1.0});
  constexpr auto kDupY = std::to_array<double>({1.0, 2.0});
  EXPECT_EQ(-1, harp_cubic_spline_interpolation(kDupX.data(), kDupY.data(),
                                                kDupX.size(), 1.0, &yp));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Bicubic spline on bilinear f(x, y) = x + 2*y.
  constexpr auto kXx = std::to_array<double>({0.0, 1.0, 2.0, 3.0});
  constexpr auto kYy = std::to_array<double>({0.0, 2.0, 4.0, 6.0});
  constexpr std::array<std::array<double, 4>, 4> kZRows = {{
      {0.0, 4.0, 8.0, 12.0},
      {1.0, 5.0, 9.0, 13.0},
      {2.0, 6.0, 10.0, 14.0},
      {3.0, 7.0, 11.0, 15.0},
  }};
  const double* zz_ptrs[] = {
      kZRows[0].data(),
      kZRows[1].data(),
      kZRows[2].data(),
      kZRows[3].data(),
  };
  double zp = 0.0;
  ASSERT_EQ(HARP_SUCCESS, harp_bicubic_spline_interpolation(
                              kXx.data(), kYy.data(), zz_ptrs, kXx.size(),
                              kYy.size(), 1.5, 3.0, &zp));
  EXPECT_NEAR(7.5, zp, 1e-12);

  // Duplicate yy (n == 2, m == 2) triggers row-spline failure and cleanup.
  constexpr auto kXx2 = std::to_array<double>({0.0, 1.0});
  constexpr auto kYy2 = std::to_array<double>({0.0, 2.0});
  const double* zz_ptrs2[] = {kZRows[0].data(), kZRows[1].data()};
  EXPECT_EQ(-1, harp_bicubic_spline_interpolation(kXx2.data(), kDupX.data(),
                                                  zz_ptrs2, kXx2.size(),
                                                  kDupX.size(), 0.5, 1.0, &zp));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Duplicate xx (m == 2, n == 2) triggers column-spline failure and cleanup.
  EXPECT_EQ(-1, harp_bicubic_spline_interpolation(kDupX.data(), kYy2.data(),
                                                  zz_ptrs2, kDupX.size(),
                                                  kYy2.size(), 1.0, 1.0, &zp));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
}

TEST(HarpInterpolationTest, LinearValueAndArrayAllOutOfBoundsModes) {
  constexpr auto kGridAsc = std::to_array<double>({1.0, 2.0, 4.0});
  constexpr auto kValsAsc = std::to_array<double>({10.0, 20.0, 40.0});
  constexpr auto kTargets =
      std::to_array<double>({0.0, 1.0, 1.5, 3.0, 4.0, 6.0});

  // Single-value linear checks across out_of_bound_flag 0, 1, 2.
  double v = 0.0;
  harp_interpolate_value_linear(kGridAsc.size(), kGridAsc.data(),
                                kValsAsc.data(), 0.0, 0, &v);
  EXPECT_EQ(1, harp_isnan(v));
  harp_interpolate_value_linear(kGridAsc.size(), kGridAsc.data(),
                                kValsAsc.data(), 6.0, 0, &v);
  EXPECT_EQ(1, harp_isnan(v));

  harp_interpolate_value_linear(kGridAsc.size(), kGridAsc.data(),
                                kValsAsc.data(), 0.0, 1, &v);
  EXPECT_DOUBLE_EQ(10.0, v);
  harp_interpolate_value_linear(kGridAsc.size(), kGridAsc.data(),
                                kValsAsc.data(), 6.0, 1, &v);
  EXPECT_DOUBLE_EQ(40.0, v);

  harp_interpolate_value_linear(kGridAsc.size(), kGridAsc.data(),
                                kValsAsc.data(), 0.0, 2, &v);
  EXPECT_DOUBLE_EQ(0.0, v);
  harp_interpolate_value_linear(kGridAsc.size(), kGridAsc.data(),
                                kValsAsc.data(), 6.0, 2, &v);
  EXPECT_DOUBLE_EQ(60.0, v);

  std::vector<double> out(kTargets.size(), -999.0);
  harp_interpolate_array_linear(kGridAsc.size(), kGridAsc.data(),
                                kValsAsc.data(), kTargets.size(),
                                kTargets.data(), 2, out.data());
  EXPECT_THAT(out, ElementsAre(0.0, 10.0, 15.0, 30.0, 40.0, 60.0));

  // Descending grid and targets.
  constexpr auto kGridDesc = std::to_array<double>({4.0, 2.0, 1.0});
  constexpr auto kValsDesc = std::to_array<double>({40.0, 20.0, 10.0});
  constexpr auto kTargetsDesc =
      std::to_array<double>({6.0, 4.0, 3.0, 1.5, 1.0, 0.0});
  harp_interpolate_array_linear(kGridDesc.size(), kGridDesc.data(),
                                kValsDesc.data(), kTargetsDesc.size(),
                                kTargetsDesc.data(), 1, out.data());
  EXPECT_THAT(out, ElementsAre(40.0, 40.0, 30.0, 15.0, 10.0, 10.0));
}

TEST(HarpInterpolationTest, LogLinearValueAndArrayAllOutOfBoundsModes) {
  constexpr auto kGrid = std::to_array<double>({1.0, 2.0, 8.0});
  const auto kVals = std::to_array<double>({0.0, std::log(2.0), std::log(8.0)});

  double v = 0.0;
  // Below range: 0 -> NaN, 1 -> edge, 2 -> loglinear extrap.
  harp_interpolate_value_loglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                   0.5, 0, &v);
  EXPECT_EQ(1, harp_isnan(v));
  harp_interpolate_value_loglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                   0.5, 1, &v);
  EXPECT_DOUBLE_EQ(0.0, v);
  harp_interpolate_value_loglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                   0.5, 2, &v);
  EXPECT_NEAR(std::log(0.5), v, 1e-12);

  // Above range: 0 -> NaN, 1 -> edge, 2 -> loglinear extrap.
  harp_interpolate_value_loglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                   16.0, 0, &v);
  EXPECT_EQ(1, harp_isnan(v));
  harp_interpolate_value_loglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                   16.0, 1, &v);
  EXPECT_DOUBLE_EQ(std::log(8.0), v);
  harp_interpolate_value_loglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                   16.0, 2, &v);
  EXPECT_NEAR(std::log(16.0), v, 1e-12);

  constexpr auto kTargets = std::to_array<double>({0.5, 1.0, 4.0, 8.0, 16.0});
  std::vector<double> out(kTargets.size(), -999.0);
  harp_interpolate_array_loglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                   kTargets.size(), kTargets.data(), 2,
                                   out.data());
  EXPECT_NEAR(std::log(0.5), out[0], 1e-12);
  EXPECT_DOUBLE_EQ(0.0, out[1]);
  EXPECT_NEAR(std::log(4.0), out[2], 1e-12);
  EXPECT_DOUBLE_EQ(std::log(8.0), out[3]);
  EXPECT_NEAR(std::log(16.0), out[4], 1e-12);
}

TEST(HarpInterpolationTest, LogLogLinearValueAndArrayAllOutOfBoundsModes) {
  // Power law f(x) = 3.0 * x^(-2.0) is exact under log/log linear
  // interpolation.
  constexpr auto kGrid = std::to_array<double>({1.0, 2.0, 4.0});
  constexpr auto kVals = std::to_array<double>({3.0, 0.75, 0.1875});

  double v = 0.0;
  harp_interpolate_value_logloglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                      0.5, 0, &v);
  EXPECT_EQ(1, harp_isnan(v));
  harp_interpolate_value_logloglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                      0.5, 1, &v);
  EXPECT_DOUBLE_EQ(3.0, v);
  harp_interpolate_value_logloglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                      0.5, 2, &v);
  EXPECT_NEAR(12.0, v, 1e-11);

  harp_interpolate_value_logloglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                      8.0, 0, &v);
  EXPECT_EQ(1, harp_isnan(v));
  harp_interpolate_value_logloglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                      8.0, 1, &v);
  EXPECT_DOUBLE_EQ(0.1875, v);
  harp_interpolate_value_logloglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                      8.0, 2, &v);
  EXPECT_NEAR(3.0 / 64.0, v, 1e-12);

  constexpr auto kTargets = std::to_array<double>({0.5, 1.0, 3.0, 4.0, 8.0});
  std::vector<double> out(kTargets.size(), -999.0);
  harp_interpolate_array_logloglinear(kGrid.size(), kGrid.data(), kVals.data(),
                                      kTargets.size(), kTargets.data(), 2,
                                      out.data());
  EXPECT_NEAR(12.0, out[0], 1e-11);
  EXPECT_DOUBLE_EQ(3.0, out[1]);
  EXPECT_NEAR(3.0 / 9.0, out[2], 1e-12);
  EXPECT_DOUBLE_EQ(0.1875, out[3]);
  EXPECT_NEAR(3.0 / 64.0, out[4], 1e-12);
}

TEST(HarpInterpolationTest, IntervalInterpolateArrayLinear) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  // Source intervals: [0, 10]=100, [10, 20]=NaN, [20, 30]=300.
  constexpr auto kSrcBounds =
      std::to_array<double>({0.0, 10.0, 10.0, 20.0, 20.0, 30.0});
  const auto kSrcVals = std::to_array<double>({100.0, nan, 300.0});

  // Target intervals:
  // - [-5, 0]: non-overlapping -> NaN
  // - [5, 25]: overlaps 0.5 of [0,10] + NaN [10,20] + 0.5 of [20,30] = 200
  // - [12, 18]: overlaps only NaN interval -> NaN
  // - reversed bounds [30, 25]: overlaps 0.5 of [20,30] = 150
  constexpr auto kTgtBounds =
      std::to_array<double>({-5.0, 0.0, 5.0, 25.0, 12.0, 18.0, 30.0, 25.0});
  std::vector<double> out(4, -999.0);
  harp_interval_interpolate_array_linear(3, kSrcBounds.data(), kSrcVals.data(),
                                         4, kTgtBounds.data(), out.data());
  EXPECT_EQ(1, harp_isnan(out[0]));
  EXPECT_DOUBLE_EQ(200.0, out[1]);
  EXPECT_EQ(1, harp_isnan(out[2]));
  EXPECT_DOUBLE_EQ(150.0, out[3]);

  // Reversed source interval bounds [10, 0] and [20, 10].
  constexpr auto kRevSrcBounds = std::to_array<double>({10.0, 0.0, 20.0, 10.0});
  constexpr auto kRevSrcVals = std::to_array<double>({50.0, 100.0});
  constexpr auto kTgt = std::to_array<double>({5.0, 15.0});
  std::vector<double> out_rev(1, -999.0);
  harp_interval_interpolate_array_linear(2, kRevSrcBounds.data(),
                                         kRevSrcVals.data(), 1, kTgt.data(),
                                         out_rev.data());
  EXPECT_DOUBLE_EQ(75.0, out_rev[0]);
}

TEST(HarpInterpolationTest, BoundsFromMidpointsLinearAndLoglinear) {
  const double nan = std::numeric_limits<double>::quiet_NaN();

  // 1. Empty / all-NaN input leaves bounds untouched or NaN-filled.
  const auto kAllNan = std::to_array<double>({nan, nan});
  std::vector<double> bounds(4, -999.0);
  harp_bounds_from_midpoints_linear(0, kAllNan.data(), 0, bounds.data());
  harp_bounds_from_midpoints_linear(2, kAllNan.data(), 0, bounds.data());
  EXPECT_EQ(1, harp_isnan(bounds[0]));
  EXPECT_EQ(1, harp_isnan(bounds[3]));

  bounds.assign(4, -999.0);
  harp_bounds_from_midpoints_loglinear(0, kAllNan.data(), 0, bounds.data());
  harp_bounds_from_midpoints_loglinear(2, kAllNan.data(), 0, bounds.data());
  EXPECT_EQ(1, harp_isnan(bounds[0]));
  EXPECT_EQ(1, harp_isnan(bounds[3]));

  // 2. Single valid midpoint (with trailing NaN).
  const auto kSingleWithNan = std::to_array<double>({5.0, nan});
  harp_bounds_from_midpoints_linear(2, kSingleWithNan.data(), 1, bounds.data());
  EXPECT_DOUBLE_EQ(5.0, bounds[0]);
  EXPECT_DOUBLE_EQ(5.0, bounds[1]);
  EXPECT_EQ(1, harp_isnan(bounds[2]));
  EXPECT_EQ(1, harp_isnan(bounds[3]));

  harp_bounds_from_midpoints_loglinear(2, kSingleWithNan.data(), 1,
                                       bounds.data());
  EXPECT_DOUBLE_EQ(5.0, bounds[0]);
  EXPECT_DOUBLE_EQ(5.0, bounds[1]);
  EXPECT_EQ(1, harp_isnan(bounds[2]));
  EXPECT_EQ(1, harp_isnan(bounds[3]));

  // 3. Multi-point linear bounds with extrapolate=0 and extrapolate=1.
  constexpr auto kLinMid = std::to_array<double>({1.0, 3.0, 7.0});
  std::vector<double> lin_bounds(6, -999.0);
  harp_bounds_from_midpoints_linear(3, kLinMid.data(), 0, lin_bounds.data());
  EXPECT_THAT(lin_bounds, ElementsAre(1.0, 2.0, 2.0, 5.0, 5.0, 7.0));

  harp_bounds_from_midpoints_linear(3, kLinMid.data(), 1, lin_bounds.data());
  EXPECT_THAT(lin_bounds, ElementsAre(0.0, 2.0, 2.0, 5.0, 5.0, 9.0));

  // 4. Multi-point loglinear bounds with extrapolate=0 and extrapolate=1.
  constexpr auto kLogMid = std::to_array<double>({1.0, 4.0, 16.0});
  std::vector<double> log_bounds(6, -999.0);
  harp_bounds_from_midpoints_loglinear(3, kLogMid.data(), 0, log_bounds.data());
  EXPECT_NEAR(1.0, log_bounds[0], 1e-12);
  EXPECT_NEAR(2.0, log_bounds[1], 1e-12);
  EXPECT_NEAR(2.0, log_bounds[2], 1e-12);
  EXPECT_NEAR(8.0, log_bounds[3], 1e-12);
  EXPECT_NEAR(8.0, log_bounds[4], 1e-12);
  EXPECT_NEAR(16.0, log_bounds[5], 1e-12);

  harp_bounds_from_midpoints_loglinear(3, kLogMid.data(), 1, log_bounds.data());
  EXPECT_NEAR(0.5, log_bounds[0], 1e-12);
  EXPECT_NEAR(2.0, log_bounds[1], 1e-12);
  EXPECT_NEAR(2.0, log_bounds[2], 1e-12);
  EXPECT_NEAR(8.0, log_bounds[3], 1e-12);
  EXPECT_NEAR(8.0, log_bounds[4], 1e-12);
  EXPECT_NEAR(32.0, log_bounds[5], 1e-12);
}

}  // namespace
}  // namespace stcorp_harp
