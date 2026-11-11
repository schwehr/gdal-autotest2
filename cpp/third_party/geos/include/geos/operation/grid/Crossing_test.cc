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

// Unit tests for third_party/geos/include/geos/operation/grid/Crossing.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/operation/grid/Crossing.h

#include "third_party/geos/include/geos/operation/grid/Crossing.h"

#include <array>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/operation/grid/Side.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateXY;
using geos::operation::grid::Crossing;
using geos::operation::grid::Side;

static_assert(!std::is_default_constructible_v<Crossing>);
static_assert(std::is_constructible_v<Crossing, Side, double, double>);
static_assert(std::is_constructible_v<Crossing, Side, const CoordinateXY&>);
static_assert(std::is_copy_constructible_v<Crossing>);
static_assert(std::is_copy_assignable_v<Crossing>);
static_assert(std::is_move_constructible_v<Crossing>);
static_assert(std::is_move_assignable_v<Crossing>);

struct CrossingCase {
  Side side;
  double x;
  double y;
};

constexpr auto kCrossingCases = std::to_array<CrossingCase>({
    {.side = Side::NONE, .x = 0.0, .y = 0.0},
    {.side = Side::LEFT, .x = -12.5, .y = 3.25},
    {.side = Side::RIGHT, .x = 42.75, .y = -8.5},
    {.side = Side::TOP, .x = 1.0e-9, .y = 1.0e9},
    {.side = Side::BOTTOM, .x = -1.0e9, .y = -1.0e-9},
});

// Side.h declares operator<<(std::ostream&, const Side&) without a definition,
// so compare integer representations in EXPECT_EQ to avoid GoogleTest linking
// against the undefined ostream printer.
void ExpectSideEq(Side actual, Side expected) {
  EXPECT_TRUE(actual == expected);
  EXPECT_EQ(static_cast<int>(actual), static_cast<int>(expected));
}

TEST(CrossingTest, ConstructsFromSideAndXYCoordinates) {
  for (const CrossingCase& tc : kCrossingCases) {
    const Crossing crossing(tc.side, tc.x, tc.y);
    ExpectSideEq(crossing.getSide(), tc.side);
    EXPECT_DOUBLE_EQ(crossing.getCoord().x, tc.x);
    EXPECT_DOUBLE_EQ(crossing.getCoord().y, tc.y);
    EXPECT_TRUE(crossing.getCoord().equals2D(CoordinateXY(tc.x, tc.y)));
  }
}

TEST(CrossingTest, ConstructsFromSideAndCoordinateXY) {
  for (const CrossingCase& tc : kCrossingCases) {
    CoordinateXY source(tc.x, tc.y);
    const Crossing crossing(tc.side, source);
    ExpectSideEq(crossing.getSide(), tc.side);
    EXPECT_EQ(crossing.getCoord(), source);
    EXPECT_DOUBLE_EQ(crossing.getCoord().x, tc.x);
    EXPECT_DOUBLE_EQ(crossing.getCoord().y, tc.y);
    EXPECT_NE(&crossing.getCoord(), &source);

    // Mutating the source coordinate does not alter the stored copy.
    source.x += 100.0;
    source.y -= 100.0;
    EXPECT_DOUBLE_EQ(crossing.getCoord().x, tc.x);
    EXPECT_DOUBLE_EQ(crossing.getCoord().y, tc.y);
  }
}

TEST(CrossingTest, ConstructsFromCoordinateSlice) {
  const Coordinate coord_xyz(7.5, -2.25, 99.0);
  const Crossing crossing(Side::TOP, coord_xyz);

  ExpectSideEq(crossing.getSide(), Side::TOP);
  EXPECT_DOUBLE_EQ(crossing.getCoord().x, 7.5);
  EXPECT_DOUBLE_EQ(crossing.getCoord().y, -2.25);
}

TEST(CrossingTest, AccessorsReturnConstReferencesToMembers) {
  const Crossing crossing(Side::RIGHT, 1.5, 2.5);

  static_assert(std::is_same_v<decltype(crossing.getSide()), const Side&>);
  static_assert(
      std::is_same_v<decltype(crossing.getCoord()), const CoordinateXY&>);

  const Side* side_ptr_a = &crossing.getSide();
  const Side* side_ptr_b = &crossing.getSide();
  EXPECT_EQ(side_ptr_a, side_ptr_b);

  const CoordinateXY* coord_ptr_a = &crossing.getCoord();
  const CoordinateXY* coord_ptr_b = &crossing.getCoord();
  EXPECT_EQ(coord_ptr_a, coord_ptr_b);

  const Crossing copy(crossing);
  EXPECT_NE(&copy.getSide(), side_ptr_a);
  EXPECT_NE(&copy.getCoord(), coord_ptr_a);
}

TEST(CrossingTest, PreservesNonFiniteAndExtremeCoordinates) {
  const Crossing nan_xy(Side::NONE, DoubleNotANumber, DoubleNotANumber);
  ExpectSideEq(nan_xy.getSide(), Side::NONE);
  EXPECT_TRUE(std::isnan(nan_xy.getCoord().x));
  EXPECT_TRUE(std::isnan(nan_xy.getCoord().y));
  EXPECT_TRUE(nan_xy.getCoord().isNull());

  const Crossing null_coord_crossing(Side::LEFT, CoordinateXY::getNull());
  ExpectSideEq(null_coord_crossing.getSide(), Side::LEFT);
  EXPECT_TRUE(null_coord_crossing.getCoord().isNull());

  constexpr double kInf = std::numeric_limits<double>::infinity();
  const Crossing inf_crossing(Side::BOTTOM, kInf, -kInf);
  ExpectSideEq(inf_crossing.getSide(), Side::BOTTOM);
  EXPECT_TRUE(std::isinf(inf_crossing.getCoord().x));
  EXPECT_GT(inf_crossing.getCoord().x, 0.0);
  EXPECT_TRUE(std::isinf(inf_crossing.getCoord().y));
  EXPECT_LT(inf_crossing.getCoord().y, 0.0);

  const Crossing neg_zero_crossing(Side::TOP, -0.0,
                                   std::numeric_limits<double>::lowest());
  EXPECT_DOUBLE_EQ(neg_zero_crossing.getCoord().x, 0.0);
  EXPECT_TRUE(std::signbit(neg_zero_crossing.getCoord().x));
  EXPECT_DOUBLE_EQ(neg_zero_crossing.getCoord().y,
                   std::numeric_limits<double>::lowest());
}

TEST(CrossingTest, SupportsCopyMoveAndContainerStorage) {
  const Crossing original(Side::LEFT, 3.0, 4.0);
  const Crossing copied(original);
  ExpectSideEq(copied.getSide(), Side::LEFT);
  EXPECT_EQ(copied.getCoord(), CoordinateXY(3.0, 4.0));

  Crossing assigned(Side::NONE, 0.0, 0.0);
  assigned = original;
  ExpectSideEq(assigned.getSide(), Side::LEFT);
  EXPECT_EQ(assigned.getCoord(), CoordinateXY(3.0, 4.0));

  Crossing move_source(Side::BOTTOM, -5.0, 6.0);
  const Crossing moved(std::move(move_source));
  ExpectSideEq(moved.getSide(), Side::BOTTOM);
  EXPECT_EQ(moved.getCoord(), CoordinateXY(-5.0, 6.0));

  Crossing move_target(Side::NONE, 0.0, 0.0);
  move_target = Crossing(Side::RIGHT, 8.0, 9.0);
  ExpectSideEq(move_target.getSide(), Side::RIGHT);
  EXPECT_EQ(move_target.getCoord(), CoordinateXY(8.0, 9.0));

  std::vector<Crossing> crossings;
  crossings.emplace_back(Side::TOP, 1.0, 2.0);
  crossings.emplace_back(Side::BOTTOM, CoordinateXY(3.0, 4.0));
  crossings.push_back(original);

  ASSERT_EQ(crossings.size(), 3U);
  ExpectSideEq(crossings[0].getSide(), Side::TOP);
  EXPECT_EQ(crossings[0].getCoord(), CoordinateXY(1.0, 2.0));
  ExpectSideEq(crossings[1].getSide(), Side::BOTTOM);
  EXPECT_EQ(crossings[1].getCoord(), CoordinateXY(3.0, 4.0));
  ExpectSideEq(crossings[2].getSide(), Side::LEFT);
  EXPECT_EQ(crossings[2].getCoord(), CoordinateXY(3.0, 4.0));
}

}  // namespace
}  // namespace geos_google
