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

// Unit tests for third_party/geos/src/index/bintree/Interval.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/bintree/Interval.cpp

#include "third_party/geos/include/geos/index/bintree/Interval.h"

#include <array>
#include <limits>
#include <string_view>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::index::bintree::Interval;

struct BoundsCase {
  std::string_view name;
  double input_min;
  double input_max;
  double expected_min;
  double expected_max;
  double expected_width;
};

struct ExpandCase {
  std::string_view name;
  double other_min;
  double other_max;
  double expected_min;
  double expected_max;
};

struct RelationCase {
  std::string_view name;
  double other_min;
  double other_max;
  bool expected;
};

constexpr auto kBoundsCases = std::to_array<BoundsCase>({
    {
        .name = "ordered_positive",
        .input_min = 2.5,
        .input_max = 7.5,
        .expected_min = 2.5,
        .expected_max = 7.5,
        .expected_width = 5.0,
    },
    {
        .name = "reversed_positive",
        .input_min = 7.5,
        .input_max = 2.5,
        .expected_min = 2.5,
        .expected_max = 7.5,
        .expected_width = 5.0,
    },
    {
        .name = "spanning_origin",
        .input_min = -4.0,
        .input_max = 6.0,
        .expected_min = -4.0,
        .expected_max = 6.0,
        .expected_width = 10.0,
    },
    {
        .name = "reversed_negative",
        .input_min = -1.5,
        .input_max = -8.5,
        .expected_min = -8.5,
        .expected_max = -1.5,
        .expected_width = 7.0,
    },
    {
        .name = "zero_width",
        .input_min = 3.25,
        .input_max = 3.25,
        .expected_min = 3.25,
        .expected_max = 3.25,
        .expected_width = 0.0,
    },
});

constexpr auto kExpandCases = std::to_array<ExpandCase>({
    {
        .name = "contained_leaves_unchanged",
        .other_min = 3.0,
        .other_max = 4.0,
        .expected_min = 2.0,
        .expected_max = 5.0,
    },
    {
        .name = "identical_leaves_unchanged",
        .other_min = 2.0,
        .other_max = 5.0,
        .expected_min = 2.0,
        .expected_max = 5.0,
    },
    {
        .name = "extends_min_only",
        .other_min = -1.0,
        .other_max = 4.0,
        .expected_min = -1.0,
        .expected_max = 5.0,
    },
    {
        .name = "extends_max_only",
        .other_min = 3.0,
        .other_max = 9.0,
        .expected_min = 2.0,
        .expected_max = 9.0,
    },
    {
        .name = "extends_both_bounds",
        .other_min = 0.0,
        .other_max = 10.0,
        .expected_min = 0.0,
        .expected_max = 10.0,
    },
    {
        .name = "disjoint_left",
        .other_min = -5.0,
        .other_max = -2.0,
        .expected_min = -5.0,
        .expected_max = 5.0,
    },
    {
        .name = "disjoint_right",
        .other_min = 8.0,
        .other_max = 12.0,
        .expected_min = 2.0,
        .expected_max = 12.0,
    },
    {
        .name = "point_outside_right",
        .other_min = 7.0,
        .other_max = 7.0,
        .expected_min = 2.0,
        .expected_max = 7.0,
    },
});

constexpr auto kOverlapsCases = std::to_array<RelationCase>({
    {.name = "disjoint_left",
     .other_min = -2.0,
     .other_max = 1.5,
     .expected = false},
    {.name = "disjoint_right",
     .other_min = 6.5,
     .other_max = 10.0,
     .expected = false},
    {.name = "touching_min",
     .other_min = 0.0,
     .other_max = 2.0,
     .expected = true},
    {.name = "touching_max",
     .other_min = 6.0,
     .other_max = 8.0,
     .expected = true},
    {.name = "partial_left",
     .other_min = 0.0,
     .other_max = 4.0,
     .expected = true},
    {.name = "partial_right",
     .other_min = 4.0,
     .other_max = 8.0,
     .expected = true},
    {.name = "contained", .other_min = 3.0, .other_max = 5.0, .expected = true},
    {.name = "containing",
     .other_min = 0.0,
     .other_max = 10.0,
     .expected = true},
    {.name = "identical", .other_min = 2.0, .other_max = 6.0, .expected = true},
    {.name = "point_on_min",
     .other_min = 2.0,
     .other_max = 2.0,
     .expected = true},
    {.name = "point_on_max",
     .other_min = 6.0,
     .other_max = 6.0,
     .expected = true},
    {.name = "point_interior",
     .other_min = 4.0,
     .other_max = 4.0,
     .expected = true},
    {.name = "point_outside_left",
     .other_min = 1.999,
     .other_max = 1.999,
     .expected = false},
    {.name = "point_outside_right",
     .other_min = 6.001,
     .other_max = 6.001,
     .expected = false},
});

constexpr auto kContainsCases = std::to_array<RelationCase>({
    {.name = "identical", .other_min = 2.0, .other_max = 6.0, .expected = true},
    {.name = "strict_interior",
     .other_min = 3.0,
     .other_max = 5.0,
     .expected = true},
    {.name = "shared_min",
     .other_min = 2.0,
     .other_max = 5.0,
     .expected = true},
    {.name = "shared_max",
     .other_min = 3.0,
     .other_max = 6.0,
     .expected = true},
    {.name = "point_at_min",
     .other_min = 2.0,
     .other_max = 2.0,
     .expected = true},
    {.name = "point_at_max",
     .other_min = 6.0,
     .other_max = 6.0,
     .expected = true},
    {.name = "point_interior",
     .other_min = 4.0,
     .other_max = 4.0,
     .expected = true},
    {.name = "extends_left",
     .other_min = 1.0,
     .other_max = 5.0,
     .expected = false},
    {.name = "extends_right",
     .other_min = 3.0,
     .other_max = 7.0,
     .expected = false},
    {.name = "strictly_larger",
     .other_min = 1.0,
     .other_max = 7.0,
     .expected = false},
    {.name = "touching_left_outside",
     .other_min = 0.0,
     .other_max = 2.0,
     .expected = false},
    {.name = "touching_right_outside",
     .other_min = 6.0,
     .other_max = 8.0,
     .expected = false},
    {.name = "disjoint_left",
     .other_min = -2.0,
     .other_max = 0.0,
     .expected = false},
    {.name = "disjoint_right",
     .other_min = 8.0,
     .other_max = 10.0,
     .expected = false},
});

TEST(IntervalTest, DefaultConstructorInitializesOriginPoint) {
  const Interval interval;
  EXPECT_DOUBLE_EQ(interval.min, 0.0);
  EXPECT_DOUBLE_EQ(interval.max, 0.0);
  EXPECT_DOUBLE_EQ(interval.getMin(), 0.0);
  EXPECT_DOUBLE_EQ(interval.getMax(), 0.0);
  EXPECT_DOUBLE_EQ(interval.getWidth(), 0.0);
}

TEST(IntervalTest, TwoArgConstructorOrdersEndpointsAndComputesWidth) {
  for (const auto& tc : kBoundsCases) {
    SCOPED_TRACE(tc.name);
    const Interval interval(tc.input_min, tc.input_max);
    EXPECT_DOUBLE_EQ(interval.getMin(), tc.expected_min);
    EXPECT_DOUBLE_EQ(interval.getMax(), tc.expected_max);
    EXPECT_DOUBLE_EQ(interval.getWidth(), tc.expected_width);
  }
}

TEST(IntervalTest, PointerConstructorCopiesAndDetachesFromSource) {
  Interval source(10.0, -2.0);
  const Interval copy(&source);
  source.init(100.0, 200.0);

  EXPECT_DOUBLE_EQ(copy.getMin(), -2.0);
  EXPECT_DOUBLE_EQ(copy.getMax(), 10.0);
  EXPECT_DOUBLE_EQ(copy.getWidth(), 12.0);
}

TEST(IntervalTest, InitOverwritesAndNormalizesEndpoints) {
  Interval interval(1.0, 5.0);
  for (const auto& tc : kBoundsCases) {
    SCOPED_TRACE(tc.name);
    interval.init(tc.input_min, tc.input_max);
    EXPECT_DOUBLE_EQ(interval.getMin(), tc.expected_min);
    EXPECT_DOUBLE_EQ(interval.getMax(), tc.expected_max);
    EXPECT_DOUBLE_EQ(interval.getWidth(), tc.expected_width);
  }
}

TEST(IntervalTest, ExpandToIncludeUpdatesOnlyExtendedBounds) {
  for (const auto& tc : kExpandCases) {
    SCOPED_TRACE(tc.name);
    Interval interval(2.0, 5.0);
    Interval other(tc.other_min, tc.other_max);
    interval.expandToInclude(&other);
    EXPECT_DOUBLE_EQ(interval.getMin(), tc.expected_min);
    EXPECT_DOUBLE_EQ(interval.getMax(), tc.expected_max);
  }

  Interval self_interval(2.0, 5.0);
  self_interval.expandToInclude(&self_interval);
  EXPECT_DOUBLE_EQ(self_interval.getMin(), 2.0);
  EXPECT_DOUBLE_EQ(self_interval.getMax(), 5.0);
}

TEST(IntervalTest, OverlapsHandlesDisjointTouchingAndContainedCases) {
  const Interval base(2.0, 6.0);
  for (const auto& tc : kOverlapsCases) {
    SCOPED_TRACE(tc.name);
    const Interval other(tc.other_min, tc.other_max);
    EXPECT_EQ(base.overlaps(&other), tc.expected);
    EXPECT_EQ(base.overlaps(tc.other_min, tc.other_max), tc.expected);
  }
}

TEST(IntervalTest, ContainsIntervalAndBoundsOverloads) {
  const Interval base(2.0, 6.0);
  for (const auto& tc : kContainsCases) {
    SCOPED_TRACE(tc.name);
    const Interval other(tc.other_min, tc.other_max);
    EXPECT_EQ(base.contains(&other), tc.expected);
    EXPECT_EQ(base.contains(tc.other_min, tc.other_max), tc.expected);
  }
}

TEST(IntervalTest, ContainsPointIsClosedAndRejectsNonFiniteOutside) {
  const Interval base(2.0, 6.0);
  EXPECT_FALSE(base.contains(1.99));
  EXPECT_TRUE(base.contains(2.0));
  EXPECT_TRUE(base.contains(4.0));
  EXPECT_TRUE(base.contains(6.0));
  EXPECT_FALSE(base.contains(6.01));
  EXPECT_FALSE(base.contains(std::numeric_limits<double>::quiet_NaN()));
  EXPECT_FALSE(base.contains(-std::numeric_limits<double>::infinity()));
  EXPECT_FALSE(base.contains(std::numeric_limits<double>::infinity()));

  const Interval point(3.5, 3.5);
  EXPECT_TRUE(point.contains(3.5));
  EXPECT_FALSE(point.contains(3.49));
  EXPECT_FALSE(point.contains(3.51));
}

}  // namespace
}  // namespace geos_google
