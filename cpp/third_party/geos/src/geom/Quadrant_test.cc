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

#include "third_party/geos/include/geos/geom/Quadrant.h"

#include <array>
#include <cstddef>
#include <limits>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateXY;
using ::geos::geom::Quadrant;
using ::geos::util::IllegalArgumentException;

TEST(QuadrantTest, QuadrantConstants) {
  EXPECT_EQ(+Quadrant::NE, 0);
  EXPECT_EQ(+Quadrant::NW, 1);
  EXPECT_EQ(+Quadrant::SW, 2);
  EXPECT_EQ(+Quadrant::SE, 3);
}

TEST(QuadrantTest, QuadrantFromDisplacements) {
  // Strictly inside each quadrant.
  EXPECT_EQ(Quadrant::quadrant(1.0, 1.0), +Quadrant::NE);
  EXPECT_EQ(Quadrant::quadrant(-1.0, 1.0), +Quadrant::NW);
  EXPECT_EQ(Quadrant::quadrant(-1.0, -1.0), +Quadrant::SW);
  EXPECT_EQ(Quadrant::quadrant(1.0, -1.0), +Quadrant::SE);

  // Along axes (dx == 0 or dy == 0, but not both).
  EXPECT_EQ(Quadrant::quadrant(5.0, 0.0), +Quadrant::NE);
  EXPECT_EQ(Quadrant::quadrant(0.0, 5.0), +Quadrant::NE);
  EXPECT_EQ(Quadrant::quadrant(-5.0, 0.0), +Quadrant::NW);
  EXPECT_EQ(Quadrant::quadrant(0.0, -5.0), +Quadrant::SE);

  // Signed negative zero checks.
  EXPECT_EQ(Quadrant::quadrant(1.0, -0.0), +Quadrant::NE);
  EXPECT_EQ(Quadrant::quadrant(-0.0, 1.0), +Quadrant::NE);
  EXPECT_EQ(Quadrant::quadrant(-1.0, -0.0), +Quadrant::NW);
  EXPECT_EQ(Quadrant::quadrant(-0.0, -1.0), +Quadrant::SE);

  // Infinite displacements.
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(Quadrant::quadrant(inf, inf), +Quadrant::NE);
  EXPECT_EQ(Quadrant::quadrant(-inf, inf), +Quadrant::NW);
  EXPECT_EQ(Quadrant::quadrant(-inf, -inf), +Quadrant::SW);
  EXPECT_EQ(Quadrant::quadrant(inf, -inf), +Quadrant::SE);

  // Zero displacement throws IllegalArgumentException.
  try {
    Quadrant::quadrant(0.0, 0.0);
    FAIL() << "Expected IllegalArgumentException for (0.0, 0.0)";
  } catch (const IllegalArgumentException& e) {
    EXPECT_NE(std::string(e.what()).find("Cannot compute the quadrant"),
              std::string::npos);
  }

  EXPECT_THROW(Quadrant::quadrant(-0.0, -0.0), IllegalArgumentException);
}

TEST(QuadrantTest, QuadrantFromCoordinates) {
  const CoordinateXY origin(10.0, -20.0);

  // Strictly inside each quadrant relative to origin.
  EXPECT_EQ(Quadrant::quadrant(origin, CoordinateXY(11.0, -19.0)),
            +Quadrant::NE);
  EXPECT_EQ(Quadrant::quadrant(origin, CoordinateXY(9.0, -19.0)),
            +Quadrant::NW);
  EXPECT_EQ(Quadrant::quadrant(origin, CoordinateXY(9.0, -21.0)),
            +Quadrant::SW);
  EXPECT_EQ(Quadrant::quadrant(origin, CoordinateXY(11.0, -21.0)),
            +Quadrant::SE);

  // Axis-aligned segments.
  EXPECT_EQ(Quadrant::quadrant(origin, CoordinateXY(15.0, -20.0)),
            +Quadrant::NE);
  EXPECT_EQ(Quadrant::quadrant(origin, CoordinateXY(10.0, -15.0)),
            +Quadrant::NE);
  EXPECT_EQ(Quadrant::quadrant(origin, CoordinateXY(5.0, -20.0)),
            +Quadrant::NW);
  EXPECT_EQ(Quadrant::quadrant(origin, CoordinateXY(10.0, -25.0)),
            +Quadrant::SE);

  // Identical coordinates throw IllegalArgumentException.
  try {
    Quadrant::quadrant(origin, CoordinateXY(10.0, -20.0));
    FAIL() << "Expected IllegalArgumentException for identical coordinates";
  } catch (const IllegalArgumentException& e) {
    EXPECT_NE(std::string(e.what()).find("Cannot compute the quadrant"),
              std::string::npos);
  }
}

TEST(QuadrantTest, PseudoAngleValuesAndMonotonicity) {
  const CoordinateXY center(0.0, 0.0);
  EXPECT_DOUBLE_EQ(Quadrant::pseudoAngle(center, CoordinateXY(1.0, 0.0)), 0.0);
  EXPECT_DOUBLE_EQ(Quadrant::pseudoAngle(center, CoordinateXY(1.0, 1.0)), 0.5);
  EXPECT_DOUBLE_EQ(Quadrant::pseudoAngle(center, CoordinateXY(0.0, 1.0)), 1.0);
  EXPECT_DOUBLE_EQ(Quadrant::pseudoAngle(center, CoordinateXY(-1.0, 1.0)), 1.5);
  EXPECT_DOUBLE_EQ(Quadrant::pseudoAngle(center, CoordinateXY(-1.0, 0.0)), 2.0);
  EXPECT_DOUBLE_EQ(Quadrant::pseudoAngle(center, CoordinateXY(-1.0, -1.0)),
                   2.5);
  EXPECT_DOUBLE_EQ(Quadrant::pseudoAngle(center, CoordinateXY(0.0, -1.0)), 3.0);
  EXPECT_DOUBLE_EQ(Quadrant::pseudoAngle(center, CoordinateXY(1.0, -1.0)), 3.5);

  // Verify translation invariance and counterclockwise monotonicity around
  // a non-origin center.
  const CoordinateXY shifted_center(10.0, -5.0);
  const std::array<CoordinateXY, 12> ccw_points = {{
      CoordinateXY(12.0, -5.0),
      CoordinateXY(12.0, -4.0),
      CoordinateXY(11.0, -3.0),
      CoordinateXY(10.0, -3.0),
      CoordinateXY(9.0, -3.0),
      CoordinateXY(8.0, -4.0),
      CoordinateXY(8.0, -5.0),
      CoordinateXY(8.0, -6.0),
      CoordinateXY(9.0, -7.0),
      CoordinateXY(10.0, -7.0),
      CoordinateXY(11.0, -7.0),
      CoordinateXY(12.0, -6.0),
  }};

  double prev = Quadrant::pseudoAngle(shifted_center, ccw_points[0]);
  EXPECT_DOUBLE_EQ(prev, 0.0);
  for (std::size_t i = 1; i < ccw_points.size(); ++i) {
    const double cur = Quadrant::pseudoAngle(shifted_center, ccw_points[i]);
    EXPECT_GT(cur, prev) << "at index " << i;
    EXPECT_LT(cur, 4.0) << "at index " << i;
    prev = cur;
  }

  // Just below the positive x-axis wraps just below 4.0.
  const double near_wrap =
      Quadrant::pseudoAngle(center, CoordinateXY(1.0, -1e-6));
  EXPECT_GT(near_wrap, 3.999);
  EXPECT_LT(near_wrap, 4.0);
}

TEST(QuadrantTest, IsOppositeAllPairs) {
  constexpr bool kExpected[4][4] = {
      {false, false, true, false},
      {false, false, false, true},
      {true, false, false, false},
      {false, true, false, false},
  };
  for (int q1 = 0; q1 < 4; ++q1) {
    for (int q2 = 0; q2 < 4; ++q2) {
      EXPECT_EQ(Quadrant::isOpposite(q1, q2), kExpected[q1][q2])
          << "q1=" << q1 << ", q2=" << q2;
    }
  }
}

TEST(QuadrantTest, CommonHalfPlaneAllPairs) {
  constexpr int kExpected[4][4] = {
      // q2: NE(0), NW(1), SW(2), SE(3)
      {+Quadrant::NE, +Quadrant::NE, -1, +Quadrant::SE},  // q1 = NE(0)
      {+Quadrant::NE, +Quadrant::NW, +Quadrant::NW, -1},  // q1 = NW(1)
      {-1, +Quadrant::NW, +Quadrant::SW, +Quadrant::SW},  // q1 = SW(2)
      {+Quadrant::SE, -1, +Quadrant::SW, +Quadrant::SE},  // q1 = SE(3)
  };
  for (int q1 = 0; q1 < 4; ++q1) {
    for (int q2 = 0; q2 < 4; ++q2) {
      EXPECT_EQ(Quadrant::commonHalfPlane(q1, q2), kExpected[q1][q2])
          << "q1=" << q1 << ", q2=" << q2;
    }
  }
}

TEST(QuadrantTest, IsInHalfPlaneAllPairs) {
  // Note: for halfPlane == SE, GEOS checks quad == SE || quad == SW.
  constexpr bool kExpected[4][4] = {
      // halfPlane: NE(0), NW(1), SW(2), SE(3)
      {true, false, false, false},  // quad = NE(0)
      {true, true, false, false},   // quad = NW(1)
      {false, true, true, true},    // quad = SW(2)
      {false, false, true, true},   // quad = SE(3)
  };
  for (int q = 0; q < 4; ++q) {
    for (int hp = 0; hp < 4; ++hp) {
      EXPECT_EQ(Quadrant::isInHalfPlane(q, hp), kExpected[q][hp])
          << "quad=" << q << ", halfPlane=" << hp;
    }
  }

  EXPECT_FALSE(Quadrant::isInHalfPlane(-1, +Quadrant::SE));
  EXPECT_FALSE(Quadrant::isInHalfPlane(4, +Quadrant::SE));
  EXPECT_FALSE(Quadrant::isInHalfPlane(-1, +Quadrant::NE));
  EXPECT_FALSE(Quadrant::isInHalfPlane(4, +Quadrant::NE));
}

TEST(QuadrantTest, IsNorthernAllQuadrants) {
  EXPECT_TRUE(Quadrant::isNorthern(+Quadrant::NE));
  EXPECT_TRUE(Quadrant::isNorthern(+Quadrant::NW));
  EXPECT_FALSE(Quadrant::isNorthern(+Quadrant::SW));
  EXPECT_FALSE(Quadrant::isNorthern(+Quadrant::SE));
  EXPECT_FALSE(Quadrant::isNorthern(-1));
  EXPECT_FALSE(Quadrant::isNorthern(4));
}

}  // namespace
}  // namespace geos_google
