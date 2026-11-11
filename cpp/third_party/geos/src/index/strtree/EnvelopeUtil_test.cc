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

// Unit tests for third_party/geos/src/index/strtree/EnvelopeUtil.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/strtree/EnvelopeUtil.cpp

#include "third_party/geos/include/geos/index/strtree/EnvelopeUtil.h"

#include <array>
#include <cmath>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Envelope.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::index::strtree::EnvelopeUtil;

struct EnvSpec {
  double x1;
  double x2;
  double y1;
  double y2;
};

struct MaxDistanceCase {
  std::string_view name;
  EnvSpec env1;
  EnvSpec env2;
  double expected;
};

constexpr auto kMaxDistanceCases = std::to_array<MaxDistanceCase>({
    {
        .name = "identical_origin_points",
        .env1 = {.x1 = 0.0, .x2 = 0.0, .y1 = 0.0, .y2 = 0.0},
        .env2 = {.x1 = 0.0, .x2 = 0.0, .y1 = 0.0, .y2 = 0.0},
        .expected = 0.0,
    },
    {
        .name = "identical_nonzero_points",
        .env1 = {.x1 = 2.5, .x2 = 2.5, .y1 = -3.5, .y2 = -3.5},
        .env2 = {.x1 = 2.5, .x2 = 2.5, .y1 = -3.5, .y2 = -3.5},
        .expected = 0.0,
    },
    {
        .name = "distinct_points_3_4_5",
        .env1 = {.x1 = 0.0, .x2 = 0.0, .y1 = 0.0, .y2 = 0.0},
        .env2 = {.x1 = 3.0, .x2 = 3.0, .y1 = 4.0, .y2 = 4.0},
        .expected = 5.0,
    },
    {
        .name = "identical_boxes",
        .env1 = {.x1 = 1.0, .x2 = 4.0, .y1 = 2.0, .y2 = 6.0},
        .env2 = {.x1 = 1.0, .x2 = 4.0, .y1 = 2.0, .y2 = 6.0},
        .expected = 5.0,
    },
    {
        .name = "nested_envelopes",
        .env1 = {.x1 = 0.0, .x2 = 6.0, .y1 = 0.0, .y2 = 8.0},
        .env2 = {.x1 = 2.0, .x2 = 4.0, .y1 = 3.0, .y2 = 5.0},
        .expected = 10.0,
    },
    {
        .name = "disjoint_5_12_13",
        .env1 = {.x1 = 1.0, .x2 = 3.0, .y1 = 2.0, .y2 = 5.0},
        .env2 = {.x1 = 4.0, .x2 = 6.0, .y1 = 10.0, .y2 = 14.0},
        .expected = 13.0,
    },
    {
        .name = "overlapping_cross_6_8_10",
        .env1 = {.x1 = -3.0, .x2 = 1.0, .y1 = -1.0, .y2 = 2.0},
        .env2 = {.x1 = -1.0, .x2 = 5.0, .y1 = -4.0, .y2 = 0.0},
        .expected = 10.0,
    },
    {
        .name = "negative_coordinates_8_15_17",
        .env1 = {.x1 = -10.0, .x2 = -8.0, .y1 = -20.0, .y2 = -15.0},
        .env2 = {.x1 = -5.0, .x2 = -2.0, .y1 = -7.0, .y2 = -5.0},
        .expected = 17.0,
    },
    {
        .name = "degenerate_horizontal_segments",
        .env1 = {.x1 = -5.0, .x2 = -2.0, .y1 = 3.0, .y2 = 3.0},
        .env2 = {.x1 = -1.0, .x2 = 3.0, .y1 = 3.0, .y2 = 3.0},
        .expected = 8.0,
    },
    {
        .name = "degenerate_vertical_segments",
        .env1 = {.x1 = 2.0, .x2 = 2.0, .y1 = -5.0, .y2 = -1.0},
        .env2 = {.x1 = 2.0, .x2 = 2.0, .y1 = 0.0, .y2 = 7.0},
        .expected = 12.0,
    },
    {
        .name = "unsorted_constructor_extents",
        .env1 = {.x1 = 4.0, .x2 = 1.0, .y1 = 6.0, .y2 = 2.0},
        .env2 = {.x1 = 1.0, .x2 = 1.0, .y1 = 2.0, .y2 = 2.0},
        .expected = 5.0,
    },
});

TEST(EnvelopeUtilTest, ComputesMaximumDistanceAcrossCases) {
  for (const auto& tc : kMaxDistanceCases) {
    SCOPED_TRACE(tc.name);
    Envelope env1(tc.env1.x1, tc.env1.x2, tc.env1.y1, tc.env1.y2);
    Envelope env2(tc.env2.x1, tc.env2.x2, tc.env2.y1, tc.env2.y2);

    EXPECT_DOUBLE_EQ(EnvelopeUtil::maximumDistance(&env1, &env2), tc.expected);
    EXPECT_DOUBLE_EQ(EnvelopeUtil::maximumDistance(&env2, &env1), tc.expected);
  }
}

TEST(EnvelopeUtilTest, ComputesNonIntegerDiagonalAndSamePointer) {
  Envelope env1(0.5, 1.25, -1.5, 2.5);
  Envelope env2(2.0, 3.75, 0.25, 4.0);

  const double expected = std::hypot(3.75 - 0.5, 4.0 - (-1.5));
  EXPECT_DOUBLE_EQ(EnvelopeUtil::maximumDistance(&env1, &env2), expected);
  EXPECT_DOUBLE_EQ(EnvelopeUtil::maximumDistance(&env1, &env1),
                   env1.getDiameter());
}

TEST(EnvelopeUtilTest, HandlesLargeFiniteCoordinates) {
  const double scale = std::ldexp(1.0, 300);
  Envelope env1(0.0, 1.0 * scale, 0.0, 2.0 * scale);
  Envelope env2(2.0 * scale, 3.0 * scale, 3.0 * scale, 4.0 * scale);

  EXPECT_DOUBLE_EQ(EnvelopeUtil::maximumDistance(&env1, &env2), 5.0 * scale);
}

TEST(EnvelopeUtilTest, HandlesInfiniteBounds) {
  Envelope env_pos_inf(0.0, geos::DoubleInfinity, 0.0, 1.0);
  Envelope env_neg_inf(geos::DoubleNegInfinity, 0.0, 0.0, 1.0);
  Envelope env_unit(0.0, 1.0, 0.0, 1.0);

  const double pos_inf_dist =
      EnvelopeUtil::maximumDistance(&env_pos_inf, &env_unit);
  EXPECT_TRUE(std::isinf(pos_inf_dist));
  EXPECT_GT(pos_inf_dist, 0.0);

  const double neg_inf_dist =
      EnvelopeUtil::maximumDistance(&env_neg_inf, &env_unit);
  EXPECT_TRUE(std::isinf(neg_inf_dist));
  EXPECT_GT(neg_inf_dist, 0.0);
}

}  // namespace
}  // namespace geos_google
