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

#include "third_party/geos/include/geos/index/quadtree/IntervalSize.h"

#include <cmath>
#include <limits>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using ::geos::index::quadtree::IntervalSize;

TEST(IntervalSizeTest, MinBinaryExponentValue) {
  EXPECT_EQ(+IntervalSize::MIN_BINARY_EXPONENT, -50);
}

TEST(IntervalSizeTest, ZeroWidthEqualEndpoints) {
  EXPECT_TRUE(IntervalSize::isZeroWidth(0.0, 0.0));
  EXPECT_TRUE(IntervalSize::isZeroWidth(-0.0, 0.0));
  EXPECT_TRUE(IntervalSize::isZeroWidth(1.0, 1.0));
  EXPECT_TRUE(IntervalSize::isZeroWidth(-42.5, -42.5));
  EXPECT_TRUE(IntervalSize::isZeroWidth(1e100, 1e100));
  EXPECT_TRUE(IntervalSize::isZeroWidth(-1e100, -1e100));
  EXPECT_TRUE(
      IntervalSize::isZeroWidth(std::numeric_limits<double>::denorm_min(),
                                std::numeric_limits<double>::denorm_min()));
}

TEST(IntervalSizeTest, ExactBoundaryAtUnitScale) {
  // At maxAbs == 1.0, scaledInterval == width.
  // frexp(2^-50) sets level = -49; after level -= 1, level == -50.
  EXPECT_TRUE(IntervalSize::isZeroWidth(1.0 - std::ldexp(1.0, -50), 1.0));
  EXPECT_TRUE(IntervalSize::isZeroWidth(1.0 - std::ldexp(1.5, -50), 1.0));
  EXPECT_TRUE(IntervalSize::isZeroWidth(1.0 - std::ldexp(1.0, -51), 1.0));

  // frexp(2^-49) sets level = -48; after level -= 1, level == -49 > -50.
  EXPECT_FALSE(IntervalSize::isZeroWidth(1.0 - std::ldexp(1.0, -49), 1.0));

  // When upper endpoint is > 1.0, maxAbs > 1.0 scales down the interval.
  // 2^-49 / (1 + 2^-49) < 2^-49 => level == -50 => zero width.
  EXPECT_TRUE(IntervalSize::isZeroWidth(1.0, 1.0 + std::ldexp(1.0, -49)));
  // 1.5 * 2^-49 / (1 + 1.5 * 2^-49) > 2^-49 => level == -49 => non-zero width.
  EXPECT_FALSE(IntervalSize::isZeroWidth(1.0, 1.0 + std::ldexp(1.5, -49)));
}

TEST(IntervalSizeTest, NegativeIntervalsUseMaxAbs) {
  EXPECT_TRUE(IntervalSize::isZeroWidth(-1.0, -1.0 + std::ldexp(1.0, -50)));
  EXPECT_FALSE(IntervalSize::isZeroWidth(-1.0, -1.0 + std::ldexp(1.0, -49)));
  EXPECT_TRUE(IntervalSize::isZeroWidth(-1.0 - std::ldexp(1.0, -50), -1.0));
  EXPECT_FALSE(IntervalSize::isZeroWidth(-1.0 - std::ldexp(1.5, -49), -1.0));
}

TEST(IntervalSizeTest, IntervalsAtOrSpanningZeroAreNonZeroWidth) {
  // Relative to maxAbs, intervals starting at or spanning 0 have scaled
  // interval >= 1.0 (level >= 0), even for tiny absolute widths.
  EXPECT_FALSE(IntervalSize::isZeroWidth(0.0, 1.0));
  EXPECT_FALSE(IntervalSize::isZeroWidth(0.0, std::ldexp(1.0, -100)));
  EXPECT_FALSE(IntervalSize::isZeroWidth(-std::ldexp(1.0, -100), 0.0));
  EXPECT_FALSE(
      IntervalSize::isZeroWidth(0.0, std::numeric_limits<double>::min()));
  EXPECT_FALSE(IntervalSize::isZeroWidth(
      0.0, std::numeric_limits<double>::denorm_min()));
  EXPECT_FALSE(IntervalSize::isZeroWidth(-1e-100, 1e-100));
  EXPECT_FALSE(IntervalSize::isZeroWidth(-1e-300, 1e-300));
  EXPECT_FALSE(IntervalSize::isZeroWidth(-10.0, 10.0));
}

TEST(IntervalSizeTest, LargeAndSmallMagnitudeIntervals) {
  const double large_base = std::ldexp(1.0, 50);
  EXPECT_TRUE(IntervalSize::isZeroWidth(large_base - 1.0, large_base));
  const double mid_base = std::ldexp(1.0, 49);
  EXPECT_FALSE(IntervalSize::isZeroWidth(mid_base - 1.0, mid_base));

  const double small_base = std::ldexp(1.0, -100);
  EXPECT_TRUE(IntervalSize::isZeroWidth(small_base - std::ldexp(1.0, -150),
                                        small_base));
  EXPECT_FALSE(IntervalSize::isZeroWidth(small_base - std::ldexp(1.0, -149),
                                         small_base));

  EXPECT_FALSE(IntervalSize::isZeroWidth(1e7, 1e7 + 1e-6));
  EXPECT_TRUE(IntervalSize::isZeroWidth(1e7, 1e7 + 1e-9));
}

}  // namespace
}  // namespace geos_google
