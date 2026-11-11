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

#include "third_party/geos/include/geos/operation/grid/PerimeterDistance.h"

#include <limits>
#include <stdexcept>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateXY;
using ::geos::geom::Envelope;
using ::geos::operation::grid::PerimeterDistance;

TEST(PerimeterDistanceTest, RawCoordinatesAllSidesAndCorners) {
  // Use a non-square box with negative bounds to avoid width/height symmetry.
  constexpr double kXMin = -2.0;
  constexpr double kYMin = -1.0;
  constexpr double kXMax = 4.0;  // width = 6
  constexpr double kYMax = 3.0;  // height = 4, perimeter = 20

  // Left edge (0 to height = 4)
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(kXMin, kYMin, kXMax,
                                                           kYMax, -2.0, -1.0),
                   0.0);
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(kXMin, kYMin, kXMax,
                                                           kYMax, -2.0, 1.5),
                   2.5);
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(kXMin, kYMin, kXMax,
                                                           kYMax, -2.0, 3.0),
                   4.0);

  // Top edge (4 to 10)
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(kXMin, kYMin, kXMax,
                                                           kYMax, 1.5, 3.0),
                   7.5);
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(kXMin, kYMin, kXMax,
                                                           kYMax, 4.0, 3.0),
                   10.0);

  // Right edge (10 to 14)
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(kXMin, kYMin, kXMax,
                                                           kYMax, 4.0, 0.5),
                   12.5);
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(kXMin, kYMin, kXMax,
                                                           kYMax, 4.0, -1.0),
                   14.0);

  // Bottom edge (14 to 20)
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(kXMin, kYMin, kXMax,
                                                           kYMax, 0.0, -1.0),
                   18.0);
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(kXMin, kYMin, kXMax,
                                                           kYMax, -1.5, -1.0),
                   19.5);
}

TEST(PerimeterDistanceTest, EnvelopeOverloadMatchesCoordinates) {
  const Envelope env(22.0, 10.0, 11.0, 5.0);  // width = 12, height = 6

  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(10.0, 5.0)),
      0.0);
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(10.0, 8.0)),
      3.0);
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(10.0, 11.0)),
      6.0);
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(15.0, 11.0)),
      11.0);
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(22.0, 11.0)),
      18.0);
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(22.0, 7.0)),
      22.0);
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(22.0, 5.0)),
      24.0);
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(14.0, 5.0)),
      32.0);
}

TEST(PerimeterDistanceTest, DegenerateEnvelopes) {
  const Envelope point_env(CoordinateXY(3.0, 7.0));
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(
                       point_env, CoordinateXY(3.0, 7.0)),
                   0.0);

  const Envelope vertical_env(2.0, 2.0, 0.0, 5.0);
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(
                       vertical_env, CoordinateXY(2.0, 0.0)),
                   0.0);
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(
                       vertical_env, CoordinateXY(2.0, 3.0)),
                   3.0);
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(
                       vertical_env, CoordinateXY(2.0, 5.0)),
                   5.0);

  const Envelope horizontal_env(0.0, 5.0, 2.0, 2.0);
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(
                       horizontal_env, CoordinateXY(0.0, 2.0)),
                   0.0);
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(
                       horizontal_env, CoordinateXY(3.0, 2.0)),
                   3.0);
  EXPECT_DOUBLE_EQ(PerimeterDistance::getPerimeterDistance(
                       horizontal_env, CoordinateXY(5.0, 2.0)),
                   5.0);
}

TEST(PerimeterDistanceTest, ThrowsWhenPointNotOnBoundary) {
  const Envelope env(-2.0, 4.0, -1.0, 3.0);

  // Strictly interior
  EXPECT_THROW(
      PerimeterDistance::getPerimeterDistance(-2.0, -1.0, 4.0, 3.0, 1.0, 1.0),
      std::runtime_error);
  EXPECT_THROW(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(1.0, 1.0)),
      std::runtime_error);

  // Outside each side
  EXPECT_THROW(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(-3.0, 1.0)),
      std::runtime_error);
  EXPECT_THROW(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(5.0, 1.0)),
      std::runtime_error);
  EXPECT_THROW(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(1.0, -2.0)),
      std::runtime_error);
  EXPECT_THROW(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(1.0, 4.0)),
      std::runtime_error);

  // NaN coordinates do not compare equal to any boundary.
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(kNaN, 1.0)),
      std::runtime_error);
  EXPECT_THROW(
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(1.0, kNaN)),
      std::runtime_error);
}

TEST(PerimeterDistanceTest, GetPerimeterDistanceCCWBasicAndWrapAround) {
  constexpr double kPerimeter = 20.0;

  // measure2 < measure1: direct subtraction
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistanceCCW(12.0, 4.0, kPerimeter), 8.0);
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistanceCCW(15.0, 0.0, kPerimeter), 15.0);

  // measure2 == measure1: zero distance
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistanceCCW(7.5, 7.5, kPerimeter), 0.0);
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistanceCCW(0.0, 0.0, kPerimeter), 0.0);

  // measure2 > measure1: wraps around perimeter origin
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistanceCCW(3.0, 17.0, kPerimeter), 6.0);
  EXPECT_DOUBLE_EQ(
      PerimeterDistance::getPerimeterDistanceCCW(0.0, 15.0, kPerimeter), 5.0);
}

TEST(PerimeterDistanceTest, PerimeterDistanceCCWConsistencyOnBox) {
  const Envelope env(0.0, 6.0, 0.0, 4.0);
  const double perimeter = env.getPerimeter();
  ASSERT_DOUBLE_EQ(perimeter, 20.0);

  const double d_left =
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(0.0, 2.0));
  const double d_top =
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(3.0, 4.0));
  const double d_right =
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(6.0, 1.0));
  const double d_bottom =
      PerimeterDistance::getPerimeterDistance(env, CoordinateXY(2.0, 0.0));

  const double top_to_left =
      PerimeterDistance::getPerimeterDistanceCCW(d_top, d_left, perimeter);
  const double left_to_bottom =
      PerimeterDistance::getPerimeterDistanceCCW(d_left, d_bottom, perimeter);
  const double bottom_to_right =
      PerimeterDistance::getPerimeterDistanceCCW(d_bottom, d_right, perimeter);
  const double right_to_top =
      PerimeterDistance::getPerimeterDistanceCCW(d_right, d_top, perimeter);

  EXPECT_DOUBLE_EQ(top_to_left, 5.0);
  EXPECT_DOUBLE_EQ(left_to_bottom, 4.0);
  EXPECT_DOUBLE_EQ(bottom_to_right, 5.0);
  EXPECT_DOUBLE_EQ(right_to_top, 6.0);
  EXPECT_DOUBLE_EQ(
      top_to_left + left_to_bottom + bottom_to_right + right_to_top, perimeter);
}

}  // namespace
}  // namespace geos_google
