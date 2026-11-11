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

// Unit tests for third_party/geos/src/algorithm/Angle.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/Angle.cpp

#include "third_party/geos/include/geos/algorithm/Angle.h"

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::MATH_PI;
using geos::algorithm::Angle;
using geos::algorithm::Orientation;
using geos::geom::Coordinate;
using geos::geom::CoordinateXY;

constexpr double kTol = 1e-12;

static_assert(Angle::COUNTERCLOCKWISE == Orientation::COUNTERCLOCKWISE);
static_assert(Angle::CLOCKWISE == Orientation::CLOCKWISE);
static_assert(Angle::NONE == Orientation::COLLINEAR);
static_assert(Angle::PI_TIMES_2 == 2.0 * MATH_PI);
static_assert(Angle::PI_OVER_2 == MATH_PI / 2.0);
static_assert(Angle::PI_OVER_4 == MATH_PI / 4.0);

TEST(AngleTest, ConstantsMatchExpectedValues) {
  EXPECT_EQ(+Angle::COUNTERCLOCKWISE, 1);
  EXPECT_EQ(+Angle::CLOCKWISE, -1);
  EXPECT_EQ(+Angle::NONE, 0);
  EXPECT_DOUBLE_EQ(+Angle::PI_TIMES_2, 2.0 * std::numbers::pi);
  EXPECT_DOUBLE_EQ(+Angle::PI_OVER_2, std::numbers::pi / 2.0);
  EXPECT_DOUBLE_EQ(+Angle::PI_OVER_4, std::numbers::pi / 4.0);
}

TEST(AngleTest, ToDegreesAndToRadiansConversions) {
  struct ConversionCase {
    std::string_view name;
    double degrees;
    double radians;
  };

  constexpr auto kCases = std::to_array<ConversionCase>({
      {.name = "zero", .degrees = 0.0, .radians = 0.0},
      {.name = "45_deg", .degrees = 45.0, .radians = Angle::PI_OVER_4},
      {.name = "90_deg", .degrees = 90.0, .radians = Angle::PI_OVER_2},
      {.name = "180_deg", .degrees = 180.0, .radians = MATH_PI},
      {.name = "270_deg", .degrees = 270.0, .radians = 3.0 * Angle::PI_OVER_2},
      {.name = "360_deg", .degrees = 360.0, .radians = Angle::PI_TIMES_2},
      {.name = "neg_45_deg", .degrees = -45.0, .radians = -Angle::PI_OVER_4},
      {.name = "neg_90_deg", .degrees = -90.0, .radians = -Angle::PI_OVER_2},
      {.name = "neg_180_deg", .degrees = -180.0, .radians = -MATH_PI},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_DOUBLE_EQ(Angle::toDegrees(c.radians), c.degrees);
    EXPECT_DOUBLE_EQ(Angle::toRadians(c.degrees), c.radians);
    EXPECT_DOUBLE_EQ(Angle::toDegrees(Angle::toRadians(c.degrees)), c.degrees);
  }

  for (const double deg : {1.0, 30.0, 135.0, -123.45, 720.0}) {
    EXPECT_NEAR(Angle::toDegrees(Angle::toRadians(deg)), deg, kTol);
  }

  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(std::isnan(Angle::toDegrees(nan)));
  EXPECT_TRUE(std::isnan(Angle::toRadians(nan)));
}

TEST(AngleTest, AngleFromOriginAndBetweenTwoPoints) {
  struct VectorAngleCase {
    std::string_view name;
    double x;
    double y;
    double expected;
  };

  constexpr auto kCases = std::to_array<VectorAngleCase>({
      {.name = "zero_vector", .x = 0.0, .y = 0.0, .expected = 0.0},
      {.name = "pos_x", .x = 3.0, .y = 0.0, .expected = 0.0},
      {.name = "quad_1_diag", .x = 2.0, .y = 2.0, .expected = Angle::PI_OVER_4},
      {.name = "pos_y", .x = 0.0, .y = 5.0, .expected = Angle::PI_OVER_2},
      {.name = "quad_2_diag",
       .x = -2.0,
       .y = 2.0,
       .expected = 3.0 * Angle::PI_OVER_4},
      {.name = "neg_x", .x = -4.0, .y = 0.0, .expected = MATH_PI},
      {.name = "quad_3_diag",
       .x = -2.0,
       .y = -2.0,
       .expected = -3.0 * Angle::PI_OVER_4},
      {.name = "neg_y", .x = 0.0, .y = -7.0, .expected = -Angle::PI_OVER_2},
      {.name = "quad_4_diag",
       .x = 2.0,
       .y = -2.0,
       .expected = -Angle::PI_OVER_4},
  });

  const CoordinateXY origin(10.0, -5.0);
  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    const CoordinateXY vec(c.x, c.y);
    EXPECT_DOUBLE_EQ(Angle::angle(vec), c.expected);

    const CoordinateXY shifted(origin.x + c.x, origin.y + c.y);
    EXPECT_DOUBLE_EQ(Angle::angle(origin, shifted), c.expected);
  }

  // Coordinate (3D) implicitly converts to CoordinateXY and ignores Z.
  const Coordinate p0(1.0, 2.0, 100.0);
  const Coordinate p1(1.0, 5.0, -50.0);
  EXPECT_DOUBLE_EQ(Angle::angle(p0, p1), Angle::PI_OVER_2);
}

TEST(AngleTest, IsAcuteAndIsObtuse) {
  struct TripletCase {
    std::string_view name;
    CoordinateXY p0;
    CoordinateXY p1;
    CoordinateXY p2;
    bool expected_acute;
    bool expected_obtuse;
  };

  const auto kCases = std::to_array<TripletCase>({
      {
          .name = "zero_deg_same_ray",
          .p0 = {1.0, 0.0},
          .p1 = {0.0, 0.0},
          .p2 = {2.0, 0.0},
          .expected_acute = true,
          .expected_obtuse = false,
      },
      {
          .name = "acute_45_deg",
          .p0 = {1.0, 0.0},
          .p1 = {0.0, 0.0},
          .p2 = {1.0, 1.0},
          .expected_acute = true,
          .expected_obtuse = false,
      },
      {
          .name = "acute_shifted_base",
          .p0 = {5.0, 4.0},
          .p1 = {3.0, 4.0},
          .p2 = {4.0, 6.0},
          .expected_acute = true,
          .expected_obtuse = false,
      },
      {
          .name = "right_angle_90_deg",
          .p0 = {1.0, 0.0},
          .p1 = {0.0, 0.0},
          .p2 = {0.0, 1.0},
          .expected_acute = false,
          .expected_obtuse = false,
      },
      {
          .name = "obtuse_135_deg",
          .p0 = {1.0, 0.0},
          .p1 = {0.0, 0.0},
          .p2 = {-1.0, 1.0},
          .expected_acute = false,
          .expected_obtuse = true,
      },
      {
          .name = "straight_180_deg",
          .p0 = {1.0, 0.0},
          .p1 = {0.0, 0.0},
          .p2 = {-1.0, 0.0},
          .expected_acute = false,
          .expected_obtuse = true,
      },
      {
          .name = "degenerate_zero_vector",
          .p0 = {2.0, 3.0},
          .p1 = {2.0, 3.0},
          .p2 = {4.0, 3.0},
          .expected_acute = false,
          .expected_obtuse = false,
      },
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(Angle::isAcute(c.p0, c.p1, c.p2), c.expected_acute);
    EXPECT_EQ(Angle::isAcute(c.p2, c.p1, c.p0), c.expected_acute);
    EXPECT_EQ(Angle::isObtuse(c.p0, c.p1, c.p2), c.expected_obtuse);
    EXPECT_EQ(Angle::isObtuse(c.p2, c.p1, c.p0), c.expected_obtuse);
  }
}

TEST(AngleTest, AngleBetweenUnoriented) {
  struct BetweenCase {
    std::string_view name;
    CoordinateXY tip1;
    CoordinateXY tail;
    CoordinateXY tip2;
    double expected;
  };

  const auto kCases = std::to_array<BetweenCase>({
      {
          .name = "same_direction",
          .tip1 = {3.0, 0.0},
          .tail = {0.0, 0.0},
          .tip2 = {5.0, 0.0},
          .expected = 0.0,
      },
      {
          .name = "right_angle",
          .tip1 = {1.0, 0.0},
          .tail = {0.0, 0.0},
          .tip2 = {0.0, 1.0},
          .expected = Angle::PI_OVER_2,
      },
      {
          .name = "opposite_directions",
          .tip1 = {1.0, 0.0},
          .tail = {0.0, 0.0},
          .tip2 = {-1.0, 0.0},
          .expected = MATH_PI,
      },
      {
          .name = "across_pi_discontinuity",
          .tip1 = {-1.0, 1.0},
          .tail = {0.0, 0.0},
          .tip2 = {-1.0, -1.0},
          .expected = Angle::PI_OVER_2,
      },
      {
          .name = "shifted_tail_45_deg",
          .tip1 = {3.0, 3.0},
          .tail = {2.0, 3.0},
          .tip2 = {4.0, 5.0},
          .expected = Angle::PI_OVER_4,
      },
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_DOUBLE_EQ(Angle::angleBetween(c.tip1, c.tail, c.tip2), c.expected);
    EXPECT_DOUBLE_EQ(Angle::angleBetween(c.tip2, c.tail, c.tip1), c.expected);
  }
}

TEST(AngleTest, AngleBetweenOrientedCoversAllWrappingBranches) {
  struct OrientedCase {
    std::string_view name;
    CoordinateXY tip1;
    CoordinateXY tail;
    CoordinateXY tip2;
    double expected;
  };

  const auto kCases = std::to_array<OrientedCase>({
      {
          .name = "zero_angle",
          .tip1 = {1.0, 0.0},
          .tail = {0.0, 0.0},
          .tip2 = {2.0, 0.0},
          .expected = 0.0,
      },
      {
          .name = "ccw_90_deg",
          .tip1 = {1.0, 0.0},
          .tail = {0.0, 0.0},
          .tip2 = {0.0, 1.0},
          .expected = Angle::PI_OVER_2,
      },
      {
          .name = "cw_90_deg",
          .tip1 = {0.0, 1.0},
          .tail = {0.0, 0.0},
          .tip2 = {1.0, 0.0},
          .expected = -Angle::PI_OVER_2,
      },
      {
          // a1 = 3*PI/4, a2 = -3*PI/4 -> angDel = -3*PI/2 <= -PI -> wraps +2*PI
          .name = "ccw_across_neg_x_axis",
          .tip1 = {-1.0, 1.0},
          .tail = {0.0, 0.0},
          .tip2 = {-1.0, -1.0},
          .expected = Angle::PI_OVER_2,
      },
      {
          // a1 = -3*PI/4, a2 = 3*PI/4 -> angDel = +3*PI/2 > PI -> wraps -2*PI
          .name = "cw_across_neg_x_axis",
          .tip1 = {-1.0, -1.0},
          .tail = {0.0, 0.0},
          .tip2 = {-1.0, 1.0},
          .expected = -Angle::PI_OVER_2,
      },
      {
          // a1 = +PI/2, a2 = -PI/2 -> angDel = -PI <= -PI -> normalized to +PI
          .name = "opposite_north_to_south_wraps_to_pi",
          .tip1 = {0.0, 1.0},
          .tail = {0.0, 0.0},
          .tip2 = {0.0, -1.0},
          .expected = MATH_PI,
      },
      {
          // a1 = -PI/2, a2 = +PI/2 -> angDel = +PI (already in (-Pi, Pi])
          .name = "opposite_south_to_north_is_pi",
          .tip1 = {0.0, -1.0},
          .tail = {0.0, 0.0},
          .tip2 = {0.0, 1.0},
          .expected = MATH_PI,
      },
      {
          .name = "shifted_tail",
          .tip1 = {6.0, 5.0},
          .tail = {5.0, 5.0},
          .tip2 = {5.0, 4.0},
          .expected = -Angle::PI_OVER_2,
      },
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_DOUBLE_EQ(Angle::angleBetweenOriented(c.tip1, c.tail, c.tip2),
                     c.expected);
  }
}

TEST(AngleTest, BisectorHandlesGeneralAndCollinearInputs) {
  struct BisectorCase {
    std::string_view name;
    CoordinateXY tip1;
    CoordinateXY tail;
    CoordinateXY tip2;
    double expected;
  };

  const auto kCases = std::to_array<BisectorCase>({
      {
          .name = "first_quadrant_ccw",
          .tip1 = {1.0, 0.0},
          .tail = {0.0, 0.0},
          .tip2 = {0.0, 1.0},
          .expected = Angle::PI_OVER_4,
      },
      {
          .name = "first_quadrant_cw",
          .tip1 = {0.0, 1.0},
          .tail = {0.0, 0.0},
          .tip2 = {1.0, 0.0},
          .expected = Angle::PI_OVER_4,
      },
      {
          .name = "across_neg_x_axis_ccw",
          .tip1 = {-1.0, 1.0},
          .tail = {0.0, 0.0},
          .tip2 = {-1.0, -1.0},
          .expected = MATH_PI,
      },
      {
          .name = "across_neg_x_axis_cw",
          .tip1 = {-1.0, -1.0},
          .tail = {0.0, 0.0},
          .tip2 = {-1.0, 1.0},
          .expected = MATH_PI,
      },
      {
          .name = "collinear_same_pos_x",
          .tip1 = {1.0, 0.0},
          .tail = {0.0, 0.0},
          .tip2 = {3.0, 0.0},
          .expected = 0.0,
      },
      {
          .name = "collinear_same_neg_x",
          .tip1 = {-1.0, 0.0},
          .tail = {0.0, 0.0},
          .tip2 = {-2.0, 0.0},
          .expected = MATH_PI,
      },
      {
          .name = "collinear_opposite_east_west",
          .tip1 = {1.0, 0.0},
          .tail = {0.0, 0.0},
          .tip2 = {-1.0, 0.0},
          .expected = Angle::PI_OVER_2,
      },
      {
          .name = "collinear_opposite_west_east",
          .tip1 = {-1.0, 0.0},
          .tail = {0.0, 0.0},
          .tip2 = {1.0, 0.0},
          .expected = -Angle::PI_OVER_2,
      },
      {
          .name = "shifted_tail",
          .tip1 = {11.0, 20.0},
          .tail = {10.0, 20.0},
          .tip2 = {10.0, 19.0},
          .expected = -Angle::PI_OVER_4,
      },
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_DOUBLE_EQ(Angle::bisector(c.tip1, c.tail, c.tip2), c.expected);
  }
}

TEST(AngleTest, InteriorAngleForCwRingVertices) {
  struct InteriorCase {
    std::string_view name;
    CoordinateXY p0;
    CoordinateXY p1;
    CoordinateXY p2;
    double expected;
  };

  const auto kCases = std::to_array<InteriorCase>({
      {
          // CW corner: (0,0) -> (0,1) -> (1,1) has 90 deg interior angle.
          .name = "cw_90_deg_corner",
          .p0 = {0.0, 0.0},
          .p1 = {0.0, 1.0},
          .p2 = {1.0, 1.0},
          .expected = Angle::PI_OVER_2,
      },
      {
          // Reflex corner on CW ring: 270 deg interior angle.
          .name = "cw_270_deg_reflex_corner",
          .p0 = {2.0, 1.0},
          .p1 = {1.0, 1.0},
          .p2 = {1.0, 0.0},
          .expected = 3.0 * Angle::PI_OVER_2,
      },
      {
          .name = "straight_180_deg",
          .p0 = {0.0, 0.0},
          .p1 = {1.0, 0.0},
          .p2 = {2.0, 0.0},
          .expected = MATH_PI,
      },
      {
          .name = "zero_deg_foldback",
          .p0 = {1.0, 0.0},
          .p1 = {0.0, 0.0},
          .p2 = {1.0, 0.0},
          .expected = 0.0,
      },
      {
          .name = "cw_45_deg_corner",
          .p0 = {0.0, 0.0},
          .p1 = {0.0, 1.0},
          .p2 = {1.0, 0.0},
          .expected = Angle::PI_OVER_4,
      },
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_DOUBLE_EQ(Angle::interiorAngle(c.p0, c.p1, c.p2), c.expected);
  }
}

TEST(AngleTest, GetTurnDeterminesShortestRotationDirection) {
  struct TurnCase {
    std::string_view name;
    double ang1;
    double ang2;
    int expected;
  };

  constexpr auto kCases = std::to_array<TurnCase>({
      {
          .name = "zero_to_pi_over_2_ccw",
          .ang1 = 0.0,
          .ang2 = Angle::PI_OVER_2,
          .expected = +Angle::COUNTERCLOCKWISE,
      },
      {
          .name = "across_pi_ccw",
          .ang1 = 3.0 * Angle::PI_OVER_4,
          .ang2 = -3.0 * Angle::PI_OVER_4,
          .expected = +Angle::COUNTERCLOCKWISE,
      },
      {
          .name = "pi_over_2_to_zero_cw",
          .ang1 = Angle::PI_OVER_2,
          .ang2 = 0.0,
          .expected = +Angle::CLOCKWISE,
      },
      {
          .name = "across_pi_cw",
          .ang1 = -3.0 * Angle::PI_OVER_4,
          .ang2 = 3.0 * Angle::PI_OVER_4,
          .expected = +Angle::CLOCKWISE,
      },
      {
          .name = "equal_angles_none",
          .ang1 = Angle::PI_OVER_4,
          .ang2 = Angle::PI_OVER_4,
          .expected = +Angle::NONE,
      },
      {
          .name = "zero_angles_none",
          .ang1 = 0.0,
          .ang2 = 0.0,
          .expected = +Angle::NONE,
      },
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(Angle::getTurn(c.ang1, c.ang2), c.expected);
  }
}

TEST(AngleTest, NormalizeMapsIntoHalfOpenMinusPiToPi) {
  struct NormalizeCase {
    std::string_view name;
    double input;
    double expected;
  };

  constexpr auto kCases = std::to_array<NormalizeCase>({
      {.name = "zero", .input = 0.0, .expected = 0.0},
      {.name = "pi_over_2",
       .input = Angle::PI_OVER_2,
       .expected = Angle::PI_OVER_2},
      {.name = "plus_pi_preserved", .input = MATH_PI, .expected = MATH_PI},
      {.name = "minus_pi_maps_to_plus_pi",
       .input = -MATH_PI,
       .expected = MATH_PI},
      {.name = "two_pi_maps_to_zero",
       .input = Angle::PI_TIMES_2,
       .expected = 0.0},
      {.name = "neg_two_pi_maps_to_zero",
       .input = -Angle::PI_TIMES_2,
       .expected = 0.0},
      {.name = "three_pi", .input = 3.0 * MATH_PI, .expected = MATH_PI},
      {.name = "neg_three_pi", .input = -3.0 * MATH_PI, .expected = MATH_PI},
      {.name = "five_pi_over_2",
       .input = 5.0 * Angle::PI_OVER_2,
       .expected = Angle::PI_OVER_2},
      {.name = "neg_seven_pi_over_2",
       .input = -7.0 * Angle::PI_OVER_2,
       .expected = Angle::PI_OVER_2},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    const double actual = Angle::normalize(c.input);
    EXPECT_GT(actual, -MATH_PI);
    EXPECT_LE(actual, MATH_PI);
    EXPECT_NEAR(actual, c.expected, kTol);
  }
  EXPECT_DOUBLE_EQ(Angle::normalize(0.0), 0.0);
  EXPECT_DOUBLE_EQ(Angle::normalize(MATH_PI), MATH_PI);
  EXPECT_DOUBLE_EQ(Angle::normalize(-MATH_PI), MATH_PI);
}

TEST(AngleTest, NormalizePositiveMapsIntoZeroToTwoPi) {
  struct NormalizePositiveCase {
    std::string_view name;
    double input;
    double expected;
  };

  constexpr auto kCases = std::to_array<NormalizePositiveCase>({
      {.name = "zero", .input = 0.0, .expected = 0.0},
      {.name = "neg_zero", .input = -0.0, .expected = 0.0},
      // Tiny negative angle rounds up to PI_TIMES_2 after addition, hitting the
      // round-off clamp `if (angle >= PI_TIMES_2) angle = 0.0;`.
      {.name = "tiny_negative_rounds_to_zero",
       .input = -1e-20,
       .expected = 0.0},
      {.name = "neg_pi_over_2",
       .input = -Angle::PI_OVER_2,
       .expected = 3.0 * Angle::PI_OVER_2},
      {.name = "neg_pi", .input = -MATH_PI, .expected = MATH_PI},
      {.name = "neg_2_pi", .input = -Angle::PI_TIMES_2, .expected = 0.0},
      {.name = "neg_3_pi", .input = -3.0 * MATH_PI, .expected = MATH_PI},
      {.name = "neg_4_pi", .input = -4.0 * MATH_PI, .expected = 0.0},
      {.name = "pos_pi", .input = MATH_PI, .expected = MATH_PI},
      {.name = "pos_2_pi", .input = Angle::PI_TIMES_2, .expected = 0.0},
      {.name = "pos_3_pi", .input = 3.0 * MATH_PI, .expected = MATH_PI},
      {.name = "pos_4_pi", .input = 4.0 * MATH_PI, .expected = 0.0},
      {.name = "pos_5_pi_over_2",
       .input = 5.0 * Angle::PI_OVER_2,
       .expected = Angle::PI_OVER_2},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    const double actual = Angle::normalizePositive(c.input);
    EXPECT_GE(actual, 0.0);
    EXPECT_LT(actual, +Angle::PI_TIMES_2);
    EXPECT_NEAR(actual, c.expected, kTol);
  }
  EXPECT_DOUBLE_EQ(Angle::normalizePositive(-1e-20), 0.0);
  EXPECT_DOUBLE_EQ(Angle::normalizePositive(Angle::PI_TIMES_2), 0.0);
  EXPECT_DOUBLE_EQ(Angle::normalizePositive(-Angle::PI_TIMES_2), 0.0);
}

TEST(AngleTest, DiffComputesSmallestUnorientedAngle) {
  struct DiffCase {
    std::string_view name;
    double ang1;
    double ang2;
    double expected;
  };

  constexpr auto kCases = std::to_array<DiffCase>({
      {.name = "equal", .ang1 = 0.5, .ang2 = 0.5, .expected = 0.0},
      {.name = "first_smaller",
       .ang1 = 0.0,
       .ang2 = Angle::PI_OVER_2,
       .expected = Angle::PI_OVER_2},
      {.name = "first_larger",
       .ang1 = Angle::PI_OVER_2,
       .ang2 = 0.0,
       .expected = Angle::PI_OVER_2},
      {.name = "opposite",
       .ang1 = -Angle::PI_OVER_2,
       .ang2 = Angle::PI_OVER_2,
       .expected = MATH_PI},
      {
          .name = "wraps_above_pi_first_smaller",
          .ang1 = -3.0 * Angle::PI_OVER_4,
          .ang2 = 3.0 * Angle::PI_OVER_4,
          .expected = Angle::PI_OVER_2,
      },
      {
          .name = "wraps_above_pi_first_larger",
          .ang1 = 3.0 * Angle::PI_OVER_4,
          .ang2 = -3.0 * Angle::PI_OVER_4,
          .expected = Angle::PI_OVER_2,
      },
      {
          .name = "minus_pi_and_plus_pi",
          .ang1 = -MATH_PI,
          .ang2 = MATH_PI,
          .expected = 0.0,
      },
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    const double actual = Angle::diff(c.ang1, c.ang2);
    EXPECT_GE(actual, 0.0);
    EXPECT_LE(actual, MATH_PI);
    EXPECT_DOUBLE_EQ(actual, c.expected);
  }
}

TEST(AngleTest, SinCosSnapZerosNearZeroValues) {
  struct SnapCase {
    std::string_view name;
    double angle;
    double expected_sin;
    double expected_cos;
    bool exact;
  };

  constexpr auto kCases = std::to_array<SnapCase>({
      {.name = "zero",
       .angle = 0.0,
       .expected_sin = 0.0,
       .expected_cos = 1.0,
       .exact = true},
      {.name = "pi_over_2",
       .angle = Angle::PI_OVER_2,
       .expected_sin = 1.0,
       .expected_cos = 0.0,
       .exact = true},
      {.name = "pi",
       .angle = MATH_PI,
       .expected_sin = 0.0,
       .expected_cos = -1.0,
       .exact = true},
      {.name = "neg_pi_over_2",
       .angle = -Angle::PI_OVER_2,
       .expected_sin = -1.0,
       .expected_cos = 0.0,
       .exact = true},
      {.name = "two_pi",
       .angle = Angle::PI_TIMES_2,
       .expected_sin = 0.0,
       .expected_cos = 1.0,
       .exact = true},
      {.name = "pi_over_4",
       .angle = Angle::PI_OVER_4,
       .expected_sin = std::numbers::sqrt2 / 2.0,
       .expected_cos = std::numbers::sqrt2 / 2.0,
       .exact = false},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    double s = 99.0;
    double cs = 99.0;
    Angle::sinCosSnap(c.angle, s, cs);
    if (c.exact) {
      EXPECT_EQ(s, c.expected_sin);
      EXPECT_EQ(cs, c.expected_cos);
    } else {
      EXPECT_NEAR(s, c.expected_sin, kTol);
      EXPECT_NEAR(cs, c.expected_cos, kTol);
    }
  }
}

TEST(AngleTest, ProjectComputesEndpointAtAngleAndDistance) {
  const CoordinateXY base(1.0, 2.0);

  const CoordinateXY east = Angle::project(base, 0.0, 5.0);
  EXPECT_DOUBLE_EQ(east.x, 6.0);
  EXPECT_DOUBLE_EQ(east.y, 2.0);

  const CoordinateXY north = Angle::project(base, Angle::PI_OVER_2, 3.0);
  EXPECT_EQ(north.x, 1.0);
  EXPECT_DOUBLE_EQ(north.y, 5.0);

  const CoordinateXY west = Angle::project(base, MATH_PI, 4.0);
  EXPECT_DOUBLE_EQ(west.x, -3.0);
  EXPECT_EQ(west.y, 2.0);

  const CoordinateXY south = Angle::project(base, -Angle::PI_OVER_2, 2.5);
  EXPECT_EQ(south.x, 1.0);
  EXPECT_DOUBLE_EQ(south.y, -0.5);

  const CoordinateXY zero_dist = Angle::project(base, Angle::PI_OVER_4, 0.0);
  EXPECT_DOUBLE_EQ(zero_dist.x, 1.0);
  EXPECT_DOUBLE_EQ(zero_dist.y, 2.0);

  const CoordinateXY diag = Angle::project(
      CoordinateXY(0.0, 0.0), Angle::PI_OVER_4, std::numbers::sqrt2);
  EXPECT_NEAR(diag.x, 1.0, kTol);
  EXPECT_NEAR(diag.y, 1.0, kTol);

  const CoordinateXY reversed = Angle::project(base, 0.0, -3.0);
  EXPECT_DOUBLE_EQ(reversed.x, -2.0);
  EXPECT_DOUBLE_EQ(reversed.y, 2.0);
}

}  // namespace
}  // namespace geos_google
