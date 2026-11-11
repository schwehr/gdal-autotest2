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

// Unit tests for third_party/geos/src/index/bintree/Key.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/bintree/Key.cpp

#include "third_party/geos/include/geos/index/bintree/Key.h"

#include <array>
#include <cmath>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/index/bintree/Interval.h"

namespace geos_google {
namespace {

using geos::index::bintree::Interval;
using geos::index::bintree::Key;

struct ComputeLevelCase {
  std::string_view name;
  double min;
  double max;
  int expected_level;
};

constexpr auto kComputeLevelCases = std::to_array<ComputeLevelCase>({
    {
        .name = "zero_width_at_origin",
        .min = 0.0,
        .max = 0.0,
        .expected_level = 0,
    },
    {
        .name = "zero_width_nonzero",
        .min = 5.5,
        .max = 5.5,
        .expected_level = 0,
    },
    {.name = "quarter_width", .min = 0.0, .max = 0.25, .expected_level = -1},
    {.name = "half_width", .min = 1.0, .max = 1.5, .expected_level = 0},
    {
        .name = "three_quarters_width",
        .min = 1.0,
        .max = 1.75,
        .expected_level = 0,
    },
    {.name = "unit_width", .min = 0.0, .max = 1.0, .expected_level = 1},
    {.name = "width_one_and_half", .min = 2.0, .max = 3.5, .expected_level = 1},
    {.name = "width_two", .min = 0.0, .max = 2.0, .expected_level = 2},
    {
        .name = "width_three_negative",
        .min = -4.0,
        .max = -1.0,
        .expected_level = 2,
    },
    {
        .name = "width_eight_negative",
        .min = -10.0,
        .max = -2.0,
        .expected_level = 4,
    },
    {.name = "width_1024", .min = 0.0, .max = 1024.0, .expected_level = 11},
});

TEST(KeyTest, ComputeLevelCoversZeroSubUnitAndMultiUnitWidths) {
  for (const auto& tc : kComputeLevelCases) {
    SCOPED_TRACE(tc.name);
    Interval interval(tc.min, tc.max);
    EXPECT_EQ(Key::computeLevel(&interval), tc.expected_level);
  }
}

struct KeyCase {
  std::string_view name;
  double input_min;
  double input_max;
  double expected_pt;
  int expected_level;
  double expected_min;
  double expected_max;
};

constexpr auto kKeyCases = std::to_array<KeyCase>({
    {
        .name = "aligned_unit",
        .input_min = 0.0,
        .input_max = 1.0,
        .expected_pt = 0.0,
        .expected_level = 1,
        .expected_min = 0.0,
        .expected_max = 2.0,
    },
    {
        .name = "aligned_sub_unit",
        .input_min = 0.0,
        .input_max = 0.25,
        .expected_pt = 0.0,
        .expected_level = -1,
        .expected_min = 0.0,
        .expected_max = 0.5,
    },
    {
        .name = "shifted_sub_unit",
        .input_min = 5.25,
        .input_max = 5.5,
        .expected_pt = 5.0,
        .expected_level = -1,
        .expected_min = 5.0,
        .expected_max = 5.5,
    },
    {
        .name = "cross_boundary_single_expansion",
        .input_min = 1.5,
        .input_max = 2.5,
        .expected_pt = 0.0,
        .expected_level = 2,
        .expected_min = 0.0,
        .expected_max = 4.0,
    },
    {
        .name = "cross_boundary_multi_expansion",
        .input_min = 3.5,
        .input_max = 4.5,
        .expected_pt = 0.0,
        .expected_level = 3,
        .expected_min = 0.0,
        .expected_max = 8.0,
    },
    {
        .name = "tiny_straddling_power_of_two",
        .input_min = 3.875,
        .input_max = 4.125,
        .expected_pt = 0.0,
        .expected_level = 3,
        .expected_min = 0.0,
        .expected_max = 8.0,
    },
    {
        .name = "negative_aligned",
        .input_min = -3.5,
        .input_max = -1.5,
        .expected_pt = -4.0,
        .expected_level = 2,
        .expected_min = -4.0,
        .expected_max = 0.0,
    },
    {
        .name = "negative_cross_boundary",
        .input_min = -4.5,
        .input_max = -3.5,
        .expected_pt = -8.0,
        .expected_level = 3,
        .expected_min = -8.0,
        .expected_max = 0.0,
    },
    {
        .name = "zero_width_origin",
        .input_min = 0.0,
        .input_max = 0.0,
        .expected_pt = 0.0,
        .expected_level = 0,
        .expected_min = 0.0,
        .expected_max = 1.0,
    },
    {
        .name = "zero_width_positive",
        .input_min = 3.25,
        .input_max = 3.25,
        .expected_pt = 3.0,
        .expected_level = 0,
        .expected_min = 3.0,
        .expected_max = 4.0,
    },
    {
        .name = "zero_width_negative",
        .input_min = -2.25,
        .input_max = -2.25,
        .expected_pt = -3.0,
        .expected_level = 0,
        .expected_min = -3.0,
        .expected_max = -2.0,
    },
    {
        .name = "large_interval",
        .input_min = 1000.0,
        .input_max = 2000.0,
        .expected_pt = 0.0,
        .expected_level = 11,
        .expected_min = 0.0,
        .expected_max = 2048.0,
    },
});

TEST(KeyTest, ConstructsAndComputesCoveringPowerOfTwoIntervals) {
  for (const auto& tc : kKeyCases) {
    SCOPED_TRACE(tc.name);
    Interval input(tc.input_min, tc.input_max);
    Key key(&input);

    EXPECT_DOUBLE_EQ(key.getPoint(), tc.expected_pt);
    EXPECT_EQ(key.getLevel(), tc.expected_level);

    Interval* key_interval = key.getInterval();
    ASSERT_NE(key_interval, nullptr);
    EXPECT_DOUBLE_EQ(key_interval->getMin(), tc.expected_min);
    EXPECT_DOUBLE_EQ(key_interval->getMax(), tc.expected_max);
    EXPECT_DOUBLE_EQ(key_interval->getWidth(), std::exp2(key.getLevel()));
    EXPECT_TRUE(key_interval->contains(&input));
  }
}

TEST(KeyTest, ComputeKeyReplacesExistingIntervalAndCopiesInput) {
  Interval first(0.0, 1.0);
  Key key(&first);

  // Mutating the caller's Interval must not affect Key's owned Interval.
  first.init(100.0, 200.0);
  ASSERT_NE(key.getInterval(), nullptr);
  EXPECT_DOUBLE_EQ(key.getInterval()->getMin(), 0.0);
  EXPECT_DOUBLE_EQ(key.getInterval()->getMax(), 2.0);

  Interval second(10.0, 11.0);
  key.computeKey(&second);
  EXPECT_DOUBLE_EQ(key.getPoint(), 10.0);
  EXPECT_EQ(key.getLevel(), 1);
  ASSERT_NE(key.getInterval(), nullptr);
  EXPECT_DOUBLE_EQ(key.getInterval()->getMin(), 10.0);
  EXPECT_DOUBLE_EQ(key.getInterval()->getMax(), 12.0);
  EXPECT_TRUE(key.getInterval()->contains(&second));

  Interval third(-5.0, -3.5);
  key.computeKey(&third);
  EXPECT_DOUBLE_EQ(key.getPoint(), -8.0);
  EXPECT_EQ(key.getLevel(), 3);
  ASSERT_NE(key.getInterval(), nullptr);
  EXPECT_DOUBLE_EQ(key.getInterval()->getMin(), -8.0);
  EXPECT_DOUBLE_EQ(key.getInterval()->getMax(), 0.0);
  EXPECT_TRUE(key.getInterval()->contains(&third));
}

}  // namespace
}  // namespace geos_google
