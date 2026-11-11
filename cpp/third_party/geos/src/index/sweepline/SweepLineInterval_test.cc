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

// Unit tests for third_party/geos/src/index/sweepline/SweepLineInterval.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/sweepline/SweepLineInterval.cpp

#include "third_party/geos/include/geos/index/sweepline/SweepLineInterval.h"

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::index::sweepline::SweepLineInterval;

struct BoundCase {
  std::string_view name;
  double input_min;
  double input_max;
  double expected_min;
  double expected_max;
};

constexpr auto kBoundCases = std::to_array<BoundCase>({
    {
        .name = "ordered_positive",
        .input_min = 1.0,
        .input_max = 5.0,
        .expected_min = 1.0,
        .expected_max = 5.0,
    },
    {
        .name = "reversed_positive",
        .input_min = 5.0,
        .input_max = 1.0,
        .expected_min = 1.0,
        .expected_max = 5.0,
    },
    {
        .name = "ordered_crossing_zero",
        .input_min = -1.25,
        .input_max = 3.75,
        .expected_min = -1.25,
        .expected_max = 3.75,
    },
    {
        .name = "reversed_crossing_zero",
        .input_min = 3.75,
        .input_max = -1.25,
        .expected_min = -1.25,
        .expected_max = 3.75,
    },
    {
        .name = "ordered_negative",
        .input_min = -10.0,
        .input_max = -2.5,
        .expected_min = -10.0,
        .expected_max = -2.5,
    },
    {
        .name = "reversed_negative",
        .input_min = -2.5,
        .input_max = -10.0,
        .expected_min = -10.0,
        .expected_max = -2.5,
    },
    {
        .name = "point_positive",
        .input_min = 4.5,
        .input_max = 4.5,
        .expected_min = 4.5,
        .expected_max = 4.5,
    },
    {
        .name = "point_zero",
        .input_min = 0.0,
        .input_max = 0.0,
        .expected_min = 0.0,
        .expected_max = 0.0,
    },
    {
        .name = "point_negative",
        .input_min = -7.0,
        .input_max = -7.0,
        .expected_min = -7.0,
        .expected_max = -7.0,
    },
    {
        .name = "large_magnitude_reversed",
        .input_min = 1e100,
        .input_max = -1e100,
        .expected_min = -1e100,
        .expected_max = 1e100,
    },
    {
        .name = "tiny_positive_ordered",
        .input_min = 1e-300,
        .input_max = 2e-300,
        .expected_min = 1e-300,
        .expected_max = 2e-300,
    },
});

TEST(SweepLineIntervalTest, NormalizesBoundsAndDefaultsItemToNull) {
  for (const auto& tc : kBoundCases) {
    SCOPED_TRACE(std::string(tc.name));
    SweepLineInterval interval(tc.input_min, tc.input_max);
    EXPECT_DOUBLE_EQ(interval.getMin(), tc.expected_min);
    EXPECT_DOUBLE_EQ(interval.getMax(), tc.expected_max);
    EXPECT_EQ(interval.getItem(), nullptr);
  }
}

TEST(SweepLineIntervalTest, HandlesAdjacentUlpAndSignedZeroBounds) {
  const double next = std::nextafter(1.0, 2.0);
  SweepLineInterval ordered(1.0, next);
  EXPECT_DOUBLE_EQ(ordered.getMin(), 1.0);
  EXPECT_DOUBLE_EQ(ordered.getMax(), next);
  EXPECT_LT(ordered.getMin(), ordered.getMax());

  SweepLineInterval reversed(next, 1.0);
  EXPECT_DOUBLE_EQ(reversed.getMin(), 1.0);
  EXPECT_DOUBLE_EQ(reversed.getMax(), next);
  EXPECT_LT(reversed.getMin(), reversed.getMax());

  SweepLineInterval signed_zero_lr(-0.0, 0.0);
  EXPECT_DOUBLE_EQ(signed_zero_lr.getMin(), 0.0);
  EXPECT_DOUBLE_EQ(signed_zero_lr.getMax(), 0.0);

  SweepLineInterval signed_zero_rl(0.0, -0.0);
  EXPECT_DOUBLE_EQ(signed_zero_rl.getMin(), 0.0);
  EXPECT_DOUBLE_EQ(signed_zero_rl.getMax(), 0.0);
}

TEST(SweepLineIntervalTest, PreservesExplicitNullAndNonNullItemPointers) {
  SweepLineInterval explicit_null(1.0, 2.0, nullptr);
  EXPECT_EQ(explicit_null.getItem(), nullptr);

  int payload = 42;
  SweepLineInterval with_int(3.0, 7.0, &payload);
  EXPECT_DOUBLE_EQ(with_int.getMin(), 3.0);
  EXPECT_DOUBLE_EQ(with_int.getMax(), 7.0);
  ASSERT_EQ(with_int.getItem(), &payload);
  *static_cast<int*>(with_int.getItem()) = 99;
  EXPECT_EQ(payload, 99);

  struct Tag {
    int id;
  } tag{.id = 7};
  SweepLineInterval reversed_with_tag(9.0, 3.0, &tag);
  EXPECT_DOUBLE_EQ(reversed_with_tag.getMin(), 3.0);
  EXPECT_DOUBLE_EQ(reversed_with_tag.getMax(), 9.0);
  ASSERT_EQ(reversed_with_tag.getItem(), &tag);
  EXPECT_EQ(static_cast<const Tag*>(reversed_with_tag.getItem())->id, 7);
}

TEST(SweepLineIntervalTest, CopyConstructionAndAssignment) {
  int payload = 123;
  SweepLineInterval original(8.5, 2.5, &payload);

  SweepLineInterval copied(original);
  EXPECT_DOUBLE_EQ(copied.getMin(), 2.5);
  EXPECT_DOUBLE_EQ(copied.getMax(), 8.5);
  EXPECT_EQ(copied.getItem(), &payload);

  SweepLineInterval assigned(0.0, 1.0);
  assigned = original;
  EXPECT_DOUBLE_EQ(assigned.getMin(), 2.5);
  EXPECT_DOUBLE_EQ(assigned.getMax(), 8.5);
  EXPECT_EQ(assigned.getItem(), &payload);
}

TEST(SweepLineIntervalTest, HandlesInfiniteAndNanBounds) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

  SweepLineInterval full_inf(-kInf, kInf);
  EXPECT_DOUBLE_EQ(full_inf.getMin(), -kInf);
  EXPECT_DOUBLE_EQ(full_inf.getMax(), kInf);

  SweepLineInterval reversed_inf(kInf, -kInf);
  EXPECT_DOUBLE_EQ(reversed_inf.getMin(), -kInf);
  EXPECT_DOUBLE_EQ(reversed_inf.getMax(), kInf);

  SweepLineInterval neg_inf_finite(-10.0, -kInf);
  EXPECT_DOUBLE_EQ(neg_inf_finite.getMin(), -kInf);
  EXPECT_DOUBLE_EQ(neg_inf_finite.getMax(), -10.0);

  SweepLineInterval pos_inf_finite(kInf, 10.0);
  EXPECT_DOUBLE_EQ(pos_inf_finite.getMin(), 10.0);
  EXPECT_DOUBLE_EQ(pos_inf_finite.getMax(), kInf);

  SweepLineInterval same_pos_inf(kInf, kInf);
  EXPECT_DOUBLE_EQ(same_pos_inf.getMin(), kInf);
  EXPECT_DOUBLE_EQ(same_pos_inf.getMax(), kInf);

  SweepLineInterval same_neg_inf(-kInf, -kInf);
  EXPECT_DOUBLE_EQ(same_neg_inf.getMin(), -kInf);
  EXPECT_DOUBLE_EQ(same_neg_inf.getMax(), -kInf);

  // Ordered comparisons against NaN evaluate false in both ternary conditions,
  // assigning the non-NaN bound to min when it is newMax, or to max when it is
  // newMin.
  SweepLineInterval nan_first(kNan, 5.0);
  EXPECT_DOUBLE_EQ(nan_first.getMin(), 5.0);
  EXPECT_TRUE(std::isnan(nan_first.getMax()));

  SweepLineInterval nan_second(5.0, kNan);
  EXPECT_TRUE(std::isnan(nan_second.getMin()));
  EXPECT_DOUBLE_EQ(nan_second.getMax(), 5.0);

  SweepLineInterval both_nan(kNan, kNan);
  EXPECT_TRUE(std::isnan(both_nan.getMin()));
  EXPECT_TRUE(std::isnan(both_nan.getMax()));
}

}  // namespace
}  // namespace geos_google
