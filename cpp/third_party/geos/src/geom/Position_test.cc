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

// Unit tests for third_party/geos/src/geom/Position.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/Position.cpp

#include "third_party/geos/include/geos/geom/Position.h"

#include <array>
#include <limits>
#include <string_view>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::geom::Position;

static_assert(Position::ON == 0);
static_assert(Position::LEFT == 1);
static_assert(Position::RIGHT == 2);
static_assert(Position::ON != Position::LEFT);
static_assert(Position::LEFT != Position::RIGHT);
static_assert(Position::ON != Position::RIGHT);

struct OppositeCase {
  std::string_view name;
  int input;
  int expected;
};

constexpr auto kOppositeCases = std::to_array<OppositeCase>({
    {
        .name = "left_to_right",
        .input = Position::LEFT,
        .expected = Position::RIGHT,
    },
    {
        .name = "right_to_left",
        .input = Position::RIGHT,
        .expected = Position::LEFT,
    },
    {
        .name = "on_unchanged",
        .input = Position::ON,
        .expected = Position::ON,
    },
    {
        .name = "negative_unchanged",
        .input = -1,
        .expected = -1,
    },
    {
        .name = "arbitrary_positive_unchanged",
        .input = 3,
        .expected = 3,
    },
    {
        .name = "int_min_unchanged",
        .input = std::numeric_limits<int>::min(),
        .expected = std::numeric_limits<int>::min(),
    },
    {
        .name = "int_max_unchanged",
        .input = std::numeric_limits<int>::max(),
        .expected = std::numeric_limits<int>::max(),
    },
});

TEST(PositionTest, OppositeMapsPositionsCorrectly) {
  for (const auto& tc : kOppositeCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(Position::opposite(tc.input), tc.expected);
  }
}

TEST(PositionTest, OppositeIsAnInvolution) {
  for (const auto& tc : kOppositeCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(Position::opposite(Position::opposite(tc.input)), tc.input);
  }
}

}  // namespace
}  // namespace geos_google
