// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for third_party/geos/include/geos/index/strtree/Interval.h and
// third_party/geos/src/index/strtree/Interval.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/index/strtree/Interval.h
//   https://github.com/libgeos/geos/blob/main/src/index/strtree/Interval.cpp

#include "third_party/geos/include/geos/index/strtree/Interval.h"

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::index::strtree::Interval;

TEST(IntervalTest, ConstructAndAccessors) {
  struct AccessorCase {
    std::string_view name;
    double min_val;
    double max_val;
    double expected_width;
    double expected_centre;
  };

  constexpr auto kCases = std::to_array<AccessorCase>({
      {
          .name = "positive_span",
          .min_val = 1.0,
          .max_val = 5.0,
          .expected_width = 4.0,
          .expected_centre = 3.0,
      },
      {
          .name = "fractional_span",
          .min_val = 1.25,
          .max_val = 3.75,
          .expected_width = 2.5,
          .expected_centre = 2.5,
      },
      {
          .name = "point_interval",
          .min_val = 4.5,
          .max_val = 4.5,
          .expected_width = 0.0,
          .expected_centre = 4.5,
      },
      {
          .name = "zero_point",
          .min_val = 0.0,
          .max_val = 0.0,
          .expected_width = 0.0,
          .expected_centre = 0.0,
      },
      {
          .name = "negative_span",
          .min_val = -8.0,
          .max_val = -2.0,
          .expected_width = 6.0,
          .expected_centre = -5.0,
      },
      {
          .name = "crossing_zero",
          .min_val = -3.0,
          .max_val = 5.0,
          .expected_width = 8.0,
          .expected_centre = 1.0,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    const Interval interval(tc.min_val, tc.max_val);
    EXPECT_DOUBLE_EQ(interval.getMin(), tc.min_val);
    EXPECT_DOUBLE_EQ(interval.getMax(), tc.max_val);
    EXPECT_DOUBLE_EQ(interval.getWidth(), tc.expected_width);
    EXPECT_DOUBLE_EQ(interval.getCentre(), tc.expected_centre);
  }
}

TEST(IntervalTest, CopyConstructionAndAssignment) {
  const Interval original(-2.5, 7.5);
  const Interval copied(original);
  EXPECT_TRUE(copied.equals(&original));
  EXPECT_DOUBLE_EQ(copied.getMin(), -2.5);
  EXPECT_DOUBLE_EQ(copied.getMax(), 7.5);

  Interval assigned(0.0, 1.0);
  assigned = original;
  EXPECT_TRUE(assigned.equals(&original));
  EXPECT_DOUBLE_EQ(assigned.getMin(), -2.5);
  EXPECT_DOUBLE_EQ(assigned.getMax(), 7.5);
}

TEST(IntervalTest, InfiniteAndNanBounds) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

  const Interval neg_inf_to_zero(-kInf, 0.0);
  EXPECT_DOUBLE_EQ(neg_inf_to_zero.getMin(), -kInf);
  EXPECT_DOUBLE_EQ(neg_inf_to_zero.getMax(), 0.0);
  EXPECT_DOUBLE_EQ(neg_inf_to_zero.getWidth(), kInf);
  EXPECT_DOUBLE_EQ(neg_inf_to_zero.getCentre(), -kInf);

  const Interval zero_to_pos_inf(0.0, kInf);
  EXPECT_DOUBLE_EQ(zero_to_pos_inf.getMin(), 0.0);
  EXPECT_DOUBLE_EQ(zero_to_pos_inf.getMax(), kInf);
  EXPECT_DOUBLE_EQ(zero_to_pos_inf.getWidth(), kInf);
  EXPECT_DOUBLE_EQ(zero_to_pos_inf.getCentre(), kInf);

  const Interval all_real(-kInf, kInf);
  EXPECT_DOUBLE_EQ(all_real.getWidth(), kInf);
  EXPECT_TRUE(std::isnan(all_real.getCentre()));

  const Interval nan_min(kNan, 1.0);
  EXPECT_TRUE(std::isnan(nan_min.getMin()));
  EXPECT_DOUBLE_EQ(nan_min.getMax(), 1.0);
  EXPECT_TRUE(std::isnan(nan_min.getWidth()));
  EXPECT_TRUE(std::isnan(nan_min.getCentre()));

  const Interval nan_max(1.0, kNan);
  EXPECT_DOUBLE_EQ(nan_max.getMin(), 1.0);
  EXPECT_TRUE(std::isnan(nan_max.getMax()));
  EXPECT_TRUE(std::isnan(nan_max.getWidth()));
  EXPECT_TRUE(std::isnan(nan_max.getCentre()));

  const Interval both_nan(kNan, kNan);
  EXPECT_TRUE(std::isnan(both_nan.getMin()));
  EXPECT_TRUE(std::isnan(both_nan.getMax()));
  EXPECT_TRUE(std::isnan(both_nan.getWidth()));
  EXPECT_TRUE(std::isnan(both_nan.getCentre()));
}

TEST(IntervalTest, ExpandToInclude) {
  struct ExpandCase {
    std::string_view name;
    double a_min;
    double a_max;
    double b_min;
    double b_max;
    double expected_min;
    double expected_max;
  };

  constexpr auto kCases = std::to_array<ExpandCase>({
      {
          .name = "disjoint_right",
          .a_min = 1.0,
          .a_max = 3.0,
          .b_min = 5.0,
          .b_max = 8.0,
          .expected_min = 1.0,
          .expected_max = 8.0,
      },
      {
          .name = "disjoint_left",
          .a_min = 5.0,
          .a_max = 8.0,
          .b_min = 1.0,
          .b_max = 3.0,
          .expected_min = 1.0,
          .expected_max = 8.0,
      },
      {
          .name = "overlapping_right",
          .a_min = 1.0,
          .a_max = 5.0,
          .b_min = 3.0,
          .b_max = 8.0,
          .expected_min = 1.0,
          .expected_max = 8.0,
      },
      {
          .name = "overlapping_left",
          .a_min = 3.0,
          .a_max = 8.0,
          .b_min = 1.0,
          .b_max = 5.0,
          .expected_min = 1.0,
          .expected_max = 8.0,
      },
      {
          .name = "other_contained_in_target",
          .a_min = 1.0,
          .a_max = 10.0,
          .b_min = 3.0,
          .b_max = 7.0,
          .expected_min = 1.0,
          .expected_max = 10.0,
      },
      {
          .name = "target_contained_in_other",
          .a_min = 3.0,
          .a_max = 7.0,
          .b_min = 1.0,
          .b_max = 10.0,
          .expected_min = 1.0,
          .expected_max = 10.0,
      },
      {
          .name = "identical_intervals",
          .a_min = 2.0,
          .a_max = 6.0,
          .b_min = 2.0,
          .b_max = 6.0,
          .expected_min = 2.0,
          .expected_max = 6.0,
      },
      {
          .name = "two_points",
          .a_min = 2.0,
          .a_max = 2.0,
          .b_min = 5.0,
          .b_max = 5.0,
          .expected_min = 2.0,
          .expected_max = 5.0,
      },
      {
          .name = "negative_intervals",
          .a_min = -10.0,
          .a_max = -5.0,
          .b_min = -7.0,
          .b_max = -1.0,
          .expected_min = -10.0,
          .expected_max = -1.0,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    Interval target(tc.a_min, tc.a_max);
    const Interval other(tc.b_min, tc.b_max);
    Interval* returned = target.expandToInclude(&other);
    EXPECT_EQ(returned, &target);
    EXPECT_DOUBLE_EQ(target.getMin(), tc.expected_min);
    EXPECT_DOUBLE_EQ(target.getMax(), tc.expected_max);
  }

  // Self-expansion leaves bounds unchanged and chained calls mutate in place.
  Interval chain(4.0, 5.0);
  const Interval left(1.0, 2.0);
  const Interval right(7.0, 9.0);
  EXPECT_EQ(chain.expandToInclude(&chain), &chain);
  EXPECT_DOUBLE_EQ(chain.getMin(), 4.0);
  EXPECT_DOUBLE_EQ(chain.getMax(), 5.0);

  EXPECT_EQ(chain.expandToInclude(&left)->expandToInclude(&right), &chain);
  EXPECT_DOUBLE_EQ(chain.getMin(), 1.0);
  EXPECT_DOUBLE_EQ(chain.getMax(), 9.0);
}

TEST(IntervalTest, Intersects) {
  constexpr double kInf = std::numeric_limits<double>::infinity();

  struct IntersectsCase {
    std::string_view name;
    double a_min;
    double a_max;
    double b_min;
    double b_max;
    bool expected;
  };

  constexpr auto kCases = std::to_array<IntersectsCase>({
      {
          .name = "disjoint",
          .a_min = 1.0,
          .a_max = 4.0,
          .b_min = 5.0,
          .b_max = 9.0,
          .expected = false,
      },
      {
          .name = "touching_endpoint",
          .a_min = 1.0,
          .a_max = 4.0,
          .b_min = 4.0,
          .b_max = 9.0,
          .expected = true,
      },
      {
          .name = "partial_overlap",
          .a_min = 1.0,
          .a_max = 5.0,
          .b_min = 3.0,
          .b_max = 9.0,
          .expected = true,
      },
      {
          .name = "strict_containment",
          .a_min = 1.0,
          .a_max = 10.0,
          .b_min = 3.0,
          .b_max = 7.0,
          .expected = true,
      },
      {
          .name = "identical",
          .a_min = 2.0,
          .a_max = 6.0,
          .b_min = 2.0,
          .b_max = 6.0,
          .expected = true,
      },
      {
          .name = "point_inside",
          .a_min = 1.0,
          .a_max = 10.0,
          .b_min = 5.0,
          .b_max = 5.0,
          .expected = true,
      },
      {
          .name = "point_at_min_endpoint",
          .a_min = 1.0,
          .a_max = 10.0,
          .b_min = 1.0,
          .b_max = 1.0,
          .expected = true,
      },
      {
          .name = "point_at_max_endpoint",
          .a_min = 1.0,
          .a_max = 10.0,
          .b_min = 10.0,
          .b_max = 10.0,
          .expected = true,
      },
      {
          .name = "point_disjoint",
          .a_min = 1.0,
          .a_max = 10.0,
          .b_min = 11.0,
          .b_max = 11.0,
          .expected = false,
      },
      {
          .name = "equal_points",
          .a_min = 4.0,
          .a_max = 4.0,
          .b_min = 4.0,
          .b_max = 4.0,
          .expected = true,
      },
      {
          .name = "distinct_points",
          .a_min = 4.0,
          .a_max = 4.0,
          .b_min = 5.0,
          .b_max = 5.0,
          .expected = false,
      },
      {
          .name = "negative_overlapping",
          .a_min = -10.0,
          .a_max = -3.0,
          .b_min = -5.0,
          .b_max = -1.0,
          .expected = true,
      },
      {
          .name = "negative_disjoint",
          .a_min = -10.0,
          .a_max = -6.0,
          .b_min = -5.0,
          .b_max = -1.0,
          .expected = false,
      },
      {
          .name = "infinite_touching",
          .a_min = -kInf,
          .a_max = 0.0,
          .b_min = 0.0,
          .b_max = kInf,
          .expected = true,
      },
      {
          .name = "infinite_disjoint",
          .a_min = -kInf,
          .a_max = -1.0,
          .b_min = 1.0,
          .b_max = kInf,
          .expected = false,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    const Interval a(tc.a_min, tc.a_max);
    const Interval b(tc.b_min, tc.b_max);
    EXPECT_EQ(a.intersects(&b), tc.expected);
    EXPECT_EQ(b.intersects(&a), tc.expected);
    EXPECT_TRUE(a.intersects(&a));
  }

  // Because intersects() computes !(other->imin > imax || other->imax < imin),
  // any NaN bound makes both relational comparisons false, returning true.
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  const Interval finite(1.0, 2.0);
  const Interval nan_interval(kNan, kNan);
  EXPECT_TRUE(finite.intersects(&nan_interval));
  EXPECT_TRUE(nan_interval.intersects(&finite));
}

TEST(IntervalTest, Equals) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

  struct EqualsCase {
    std::string_view name;
    double a_min;
    double a_max;
    double b_min;
    double b_max;
    bool expected;
  };

  constexpr auto kCases = std::to_array<EqualsCase>({
      {
          .name = "identical",
          .a_min = 1.0,
          .a_max = 5.0,
          .b_min = 1.0,
          .b_max = 5.0,
          .expected = true,
      },
      {
          .name = "signed_zeros",
          .a_min = -0.0,
          .a_max = 0.0,
          .b_min = 0.0,
          .b_max = -0.0,
          .expected = true,
      },
      {
          .name = "identical_points",
          .a_min = 3.0,
          .a_max = 3.0,
          .b_min = 3.0,
          .b_max = 3.0,
          .expected = true,
      },
      {
          .name = "same_min_different_max",
          .a_min = 1.0,
          .a_max = 5.0,
          .b_min = 1.0,
          .b_max = 6.0,
          .expected = false,
      },
      {
          .name = "different_min_same_max",
          .a_min = 1.0,
          .a_max = 5.0,
          .b_min = 2.0,
          .b_max = 5.0,
          .expected = false,
      },
      {
          .name = "both_different",
          .a_min = 1.0,
          .a_max = 5.0,
          .b_min = 2.0,
          .b_max = 6.0,
          .expected = false,
      },
      {
          .name = "infinite_equal",
          .a_min = -kInf,
          .a_max = kInf,
          .b_min = -kInf,
          .b_max = kInf,
          .expected = true,
      },
      {
          .name = "infinite_different",
          .a_min = -kInf,
          .a_max = 0.0,
          .b_min = -kInf,
          .b_max = kInf,
          .expected = false,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    const Interval a(tc.a_min, tc.a_max);
    const Interval b(tc.b_min, tc.b_max);
    EXPECT_EQ(a.equals(&b), tc.expected);
    EXPECT_EQ(b.equals(&a), tc.expected);
    EXPECT_TRUE(a.equals(&a));
  }

  const Interval nan_min(kNan, 1.0);
  const Interval nan_max(1.0, kNan);
  EXPECT_FALSE(nan_min.equals(&nan_min));
  EXPECT_FALSE(nan_max.equals(&nan_max));
}

}  // namespace
}  // namespace geos_google
