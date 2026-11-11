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

// Unit tests for third_party/geos/src/geom/Coordinate.cpp and
// third_party/geos/include/geos/geom/Coordinate.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/Coordinate.cpp

#include "third_party/geos/include/geos/geom/Coordinate.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"

namespace geos_google {
namespace {

using geos::DoubleInfinity;
using geos::DoubleMax;
using geos::DoubleNegInfinity;
using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateLessThan;
using geos::geom::CoordinateType;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Ordinate;

// Compile-time invariants for enums, explicit constructors, and common_type.
static_assert(
    std::is_same_v<std::underlying_type_t<CoordinateType>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<Ordinate>, std::uint8_t>);

static_assert(!std::is_convertible_v<CoordinateXY, Coordinate>);
static_assert(!std::is_convertible_v<CoordinateXY, CoordinateXYM>);
static_assert(!std::is_convertible_v<CoordinateXY, CoordinateXYZM>);
static_assert(!std::is_convertible_v<Coordinate, CoordinateXYZM>);
static_assert(!std::is_convertible_v<CoordinateXYM, CoordinateXYZM>);

static_assert(std::is_constructible_v<Coordinate, CoordinateXY>);
static_assert(std::is_constructible_v<CoordinateXYM, CoordinateXY>);
static_assert(std::is_constructible_v<CoordinateXYZM, CoordinateXY>);
static_assert(std::is_constructible_v<CoordinateXYZM, Coordinate>);
static_assert(std::is_constructible_v<CoordinateXYZM, CoordinateXYM>);

static_assert(std::is_same_v<std::common_type_t<CoordinateXY, CoordinateXY>,
                             CoordinateXY>);
static_assert(
    std::is_same_v<std::common_type_t<CoordinateXY, Coordinate>, Coordinate>);
static_assert(std::is_same_v<std::common_type_t<CoordinateXY, CoordinateXYM>,
                             CoordinateXYM>);
static_assert(std::is_same_v<std::common_type_t<CoordinateXY, CoordinateXYZM>,
                             CoordinateXYZM>);

static_assert(
    std::is_same_v<std::common_type_t<Coordinate, CoordinateXY>, Coordinate>);
static_assert(
    std::is_same_v<std::common_type_t<Coordinate, Coordinate>, Coordinate>);
static_assert(std::is_same_v<std::common_type_t<Coordinate, CoordinateXYM>,
                             CoordinateXYZM>);
static_assert(std::is_same_v<std::common_type_t<Coordinate, CoordinateXYZM>,
                             CoordinateXYZM>);

static_assert(std::is_same_v<std::common_type_t<CoordinateXYM, CoordinateXY>,
                             CoordinateXYM>);
static_assert(std::is_same_v<std::common_type_t<CoordinateXYM, Coordinate>,
                             CoordinateXYZM>);
static_assert(std::is_same_v<std::common_type_t<CoordinateXYM, CoordinateXYM>,
                             CoordinateXYM>);
static_assert(std::is_same_v<std::common_type_t<CoordinateXYM, CoordinateXYZM>,
                             CoordinateXYZM>);

static_assert(std::is_same_v<std::common_type_t<CoordinateXYZM, CoordinateXY>,
                             CoordinateXYZM>);
static_assert(std::is_same_v<std::common_type_t<CoordinateXYZM, Coordinate>,
                             CoordinateXYZM>);
static_assert(std::is_same_v<std::common_type_t<CoordinateXYZM, CoordinateXYM>,
                             CoordinateXYZM>);
static_assert(std::is_same_v<std::common_type_t<CoordinateXYZM, CoordinateXYZM>,
                             CoordinateXYZM>);

// Exact 16-digit binary64 integer whose decimal expansion differs between
// default stream precision (6) and toString() precision (17).
constexpr double kHighPrecisionExact = 1000000000000001.0;

TEST(CoordinateTypeTest, StreamOperatorFormatsAllValues) {
  struct TypeCase {
    CoordinateType type;
    std::string_view expected;
  };
  constexpr auto kCases = std::to_array<TypeCase>({
      {.type = CoordinateType::XY, .expected = "XY"},
      {.type = CoordinateType::XYZ, .expected = "XYZ"},
      {.type = CoordinateType::XYM, .expected = "XYM"},
      {.type = CoordinateType::XYZM, .expected = "XYZM"},
  });

  for (const auto& tc : kCases) {
    std::ostringstream oss;
    oss << tc.type;
    EXPECT_EQ(oss.str(), tc.expected);
  }

  std::ostringstream chained;
  chained << CoordinateType::XY << "/" << CoordinateType::XYZM;
  EXPECT_EQ(chained.str(), "XY/XYZM");
}

TEST(CoordinateXYTest, ConstructionAndOrdinateAccessors) {
  const CoordinateXY default_coord;
  EXPECT_DOUBLE_EQ(default_coord.x, 0.0);
  EXPECT_DOUBLE_EQ(default_coord.y, 0.0);
  EXPECT_DOUBLE_EQ(default_coord.get<Ordinate::X>(), 0.0);
  EXPECT_DOUBLE_EQ(default_coord.get<Ordinate::Y>(), 0.0);
  EXPECT_TRUE(std::isnan(default_coord.get<Ordinate::Z>()));
  EXPECT_TRUE(std::isnan(default_coord.get<Ordinate::M>()));
  EXPECT_TRUE(default_coord.isValid());
  EXPECT_FALSE(default_coord.isNull());

  const CoordinateXY coord(3.25, -7.5);
  EXPECT_DOUBLE_EQ(coord.x, 3.25);
  EXPECT_DOUBLE_EQ(coord.y, -7.5);
  EXPECT_DOUBLE_EQ(coord.get<Ordinate::X>(), 3.25);
  EXPECT_DOUBLE_EQ(coord.get<Ordinate::Y>(), -7.5);
  EXPECT_TRUE(std::isnan(coord.get<Ordinate::Z>()));
  EXPECT_TRUE(std::isnan(coord.get<Ordinate::M>()));
}

TEST(CoordinateXYTest, GetNullIsNullAndSetNull) {
  const CoordinateXY& null_coord = CoordinateXY::getNull();
  EXPECT_EQ(&null_coord, &CoordinateXY::getNull());
  EXPECT_TRUE(null_coord.isNull());
  EXPECT_FALSE(null_coord.isValid());
  EXPECT_TRUE(std::isnan(null_coord.x));
  EXPECT_TRUE(std::isnan(null_coord.y));

  CoordinateXY only_x_nan(DoubleNotANumber, 1.0);
  EXPECT_FALSE(only_x_nan.isNull());
  EXPECT_FALSE(only_x_nan.isValid());

  CoordinateXY only_y_nan(1.0, DoubleNotANumber);
  EXPECT_FALSE(only_y_nan.isNull());
  EXPECT_FALSE(only_y_nan.isValid());

  CoordinateXY coord(10.0, 20.0);
  EXPECT_FALSE(coord.isNull());
  coord.setNull();
  EXPECT_TRUE(coord.isNull());
  EXPECT_FALSE(coord.isValid());
  EXPECT_TRUE(std::isnan(coord.x));
  EXPECT_TRUE(std::isnan(coord.y));
}

TEST(CoordinateXYTest, IsValidHandlesFiniteInfiniteAndNan) {
  struct ValidityCase {
    std::string_view name;
    double x;
    double y;
    bool expected_valid;
  };
  constexpr auto kCases = std::to_array<ValidityCase>({
      {.name = "origin", .x = 0.0, .y = 0.0, .expected_valid = true},
      {.name = "negative_zero", .x = -0.0, .y = -0.0, .expected_valid = true},
      {
          .name = "extreme_finite",
          .x = std::numeric_limits<double>::lowest(),
          .y = DoubleMax,
          .expected_valid = true,
      },
      {.name = "nan_x",
       .x = DoubleNotANumber,
       .y = 1.0,
       .expected_valid = false},
      {.name = "nan_y",
       .x = 1.0,
       .y = DoubleNotANumber,
       .expected_valid = false},
      {.name = "both_nan",
       .x = DoubleNotANumber,
       .y = DoubleNotANumber,
       .expected_valid = false},
      {.name = "pos_inf_x",
       .x = DoubleInfinity,
       .y = 0.0,
       .expected_valid = false},
      {.name = "neg_inf_x",
       .x = DoubleNegInfinity,
       .y = 0.0,
       .expected_valid = false},
      {.name = "pos_inf_y",
       .x = 0.0,
       .y = DoubleInfinity,
       .expected_valid = false},
      {.name = "neg_inf_y",
       .x = 0.0,
       .y = DoubleNegInfinity,
       .expected_valid = false},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY coord(tc.x, tc.y);
    EXPECT_EQ(coord.isValid(), tc.expected_valid);
  }
}

TEST(CoordinateXYTest, EqualityAndEquals2D) {
  const CoordinateXY a(1.5, -2.5);
  const CoordinateXY a_copy(1.5, -2.5);
  const CoordinateXY neg_zero(-0.0, 0.0);
  const CoordinateXY pos_zero(0.0, -0.0);
  const CoordinateXY diff_x(2.5, -2.5);
  const CoordinateXY diff_y(1.5, 2.5);
  const Coordinate a_3d(1.5, -2.5, 99.0);

  EXPECT_TRUE(a == a_copy);
  EXPECT_FALSE(a != a_copy);
  EXPECT_TRUE(a.equals2D(a_copy));
  EXPECT_TRUE(a.equals(a_copy));

  EXPECT_TRUE(neg_zero == pos_zero);
  EXPECT_TRUE(a == a_3d);
  EXPECT_TRUE(a.equals2D(a_3d));

  EXPECT_FALSE(a == diff_x);
  EXPECT_TRUE(a != diff_x);
  EXPECT_FALSE(a.equals2D(diff_x));
  EXPECT_FALSE(a.equals(diff_x));

  EXPECT_FALSE(a == diff_y);
  EXPECT_TRUE(a != diff_y);
  EXPECT_FALSE(a.equals2D(diff_y));

  // IEEE 754 NaN equality is false in 2D comparison.
  EXPECT_FALSE(CoordinateXY::getNull() == CoordinateXY::getNull());
  EXPECT_TRUE(CoordinateXY::getNull() != CoordinateXY::getNull());
}

TEST(CoordinateXYTest, Equals2DWithTolerance) {
  struct ToleranceCase {
    std::string_view name;
    double x1;
    double y1;
    double x2;
    double y2;
    double tolerance;
    bool expected;
  };
  constexpr auto kCases = std::to_array<ToleranceCase>({
      {
          .name = "exact_match_zero_tol",
          .x1 = 1.0,
          .y1 = 2.0,
          .x2 = 1.0,
          .y2 = 2.0,
          .tolerance = 0.0,
          .expected = true,
      },
      {
          .name = "strictly_within_tolerance",
          .x1 = 1.0,
          .y1 = 2.0,
          .x2 = 1.25,
          .y2 = 1.75,
          .tolerance = 0.5,
          .expected = true,
      },
      {
          .name = "exact_tolerance_boundary",
          .x1 = -1.0,
          .y1 = 2.0,
          .x2 = -1.5,
          .y2 = 2.5,
          .tolerance = 0.5,
          .expected = true,
      },
      {
          .name = "x_exceeds_tolerance",
          .x1 = 1.0,
          .y1 = 2.0,
          .x2 = 1.6,
          .y2 = 2.0,
          .tolerance = 0.5,
          .expected = false,
      },
      {
          .name = "y_exceeds_tolerance",
          .x1 = 1.0,
          .y1 = 2.0,
          .x2 = 1.2,
          .y2 = 2.6,
          .tolerance = 0.5,
          .expected = false,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY c1(tc.x1, tc.y1);
    const CoordinateXY c2(tc.x2, tc.y2);
    EXPECT_EQ(c1.equals2D(c2, tc.tolerance), tc.expected);
    EXPECT_EQ(c2.equals2D(c1, tc.tolerance), tc.expected);
  }
}

TEST(CoordinateXYTest, CompareToAndLessThan) {
  struct CompareCase {
    std::string_view name;
    double x1;
    double y1;
    double x2;
    double y2;
    int expected;
  };
  constexpr auto kCases = std::to_array<CompareCase>({
      {.name = "x_less_y_greater",
       .x1 = 1.0,
       .y1 = 100.0,
       .x2 = 2.0,
       .y2 = -5.0,
       .expected = -1},
      {.name = "x_greater_y_less",
       .x1 = 2.0,
       .y1 = -5.0,
       .x2 = 1.0,
       .y2 = 100.0,
       .expected = 1},
      {.name = "x_equal_y_less",
       .x1 = 1.0,
       .y1 = 2.0,
       .x2 = 1.0,
       .y2 = 3.0,
       .expected = -1},
      {.name = "x_equal_y_greater",
       .x1 = 1.0,
       .y1 = 3.0,
       .x2 = 1.0,
       .y2 = 2.0,
       .expected = 1},
      {.name = "equal",
       .x1 = 1.0,
       .y1 = 2.0,
       .x2 = 1.0,
       .y2 = 2.0,
       .expected = 0},
  });

  const CoordinateLessThan less_than;
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY c1(tc.x1, tc.y1);
    const CoordinateXY c2(tc.x2, tc.y2);
    EXPECT_EQ(c1.compareTo(c2), tc.expected);
    EXPECT_EQ(c1 < c2, tc.expected < 0);
    EXPECT_EQ(less_than(c1, c2), tc.expected < 0);
    EXPECT_EQ(less_than(&c1, &c2), tc.expected < 0);
  }

  const Coordinate c3d_low(1.0, 2.0, 50.0);
  const Coordinate c3d_high(1.0, 2.0, -50.0);
  EXPECT_EQ(c3d_low.compareTo(c3d_high), 0);
  EXPECT_FALSE(c3d_low < c3d_high);
  EXPECT_FALSE(c3d_high < c3d_low);
}

TEST(CoordinateXYTest, DistanceAndDistanceSquared) {
  struct DistanceCase {
    std::string_view name;
    double x1;
    double y1;
    double x2;
    double y2;
    double expected_dist;
    double expected_dist_sq;
  };
  constexpr auto kCases = std::to_array<DistanceCase>({
      {
          .name = "identical",
          .x1 = 2.5,
          .y1 = -3.5,
          .x2 = 2.5,
          .y2 = -3.5,
          .expected_dist = 0.0,
          .expected_dist_sq = 0.0,
      },
      {
          .name = "3_4_5_triangle",
          .x1 = -1.0,
          .y1 = -2.0,
          .x2 = 2.0,
          .y2 = 2.0,
          .expected_dist = 5.0,
          .expected_dist_sq = 25.0,
      },
      {
          .name = "axis_aligned",
          .x1 = 0.0,
          .y1 = 1.5,
          .x2 = 0.0,
          .y2 = -6.0,
          .expected_dist = 7.5,
          .expected_dist_sq = 56.25,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY a(tc.x1, tc.y1);
    const CoordinateXY b(tc.x2, tc.y2);
    EXPECT_DOUBLE_EQ(a.distance(b), tc.expected_dist);
    EXPECT_DOUBLE_EQ(b.distance(a), tc.expected_dist);
    EXPECT_DOUBLE_EQ(a.distanceSquared(b), tc.expected_dist_sq);
    EXPECT_DOUBLE_EQ(b.distanceSquared(a), tc.expected_dist_sq);
  }

  // Distance on CoordinateXY ignores any Z ordinate on derived types.
  const Coordinate p3d_a(0.0, 0.0, 100.0);
  const Coordinate p3d_b(3.0, 4.0, -100.0);
  EXPECT_DOUBLE_EQ(p3d_a.distance(p3d_b), 5.0);
  EXPECT_DOUBLE_EQ(p3d_a.distanceSquared(p3d_b), 25.0);
}

TEST(CoordinateXYTest, ToStringAndStreamOutput) {
  const CoordinateXY simple(1.5, -2.25);
  EXPECT_EQ(simple.toString(), "1.5 -2.25");

  const CoordinateXY high_prec(kHighPrecisionExact, -kHighPrecisionExact);
  EXPECT_EQ(high_prec.toString(), "1000000000000001 -1000000000000001");

  std::ostringstream oss;
  oss << simple << "; " << CoordinateXY(3.0, 4.0);
  EXPECT_EQ(oss.str(), "1.5 -2.25; 3 4");
}

TEST(CoordinateXYTest, HashCodeAndUnorderedSet) {
  const CoordinateXY::HashCode hasher;
  const Coordinate a(1.5, -2.5, 10.0);
  const Coordinate a_diff_z(1.5, -2.5, 20.0);
  const Coordinate swapped(-2.5, 1.5, 10.0);

  EXPECT_EQ(hasher(a), hasher(a_diff_z));
  EXPECT_NE(hasher(a), hasher(swapped));

  CoordinateXY::UnorderedSet coords;
  coords.insert(a);
  coords.insert(a_diff_z);
  coords.insert(swapped);
  EXPECT_EQ(coords.size(), 2u);
  EXPECT_TRUE(coords.contains(Coordinate(1.5, -2.5, 0.0)));
  EXPECT_TRUE(coords.contains(Coordinate(-2.5, 1.5, 0.0)));
}

TEST(CoordinateTest, ConstructorsAndOrdinateAccessors) {
  const Coordinate default_coord;
  EXPECT_DOUBLE_EQ(default_coord.x, 0.0);
  EXPECT_DOUBLE_EQ(default_coord.y, 0.0);
  EXPECT_TRUE(std::isnan(default_coord.z));
  EXPECT_TRUE(std::isnan(default_coord.get<Ordinate::Z>()));
  EXPECT_TRUE(std::isnan(default_coord.get<Ordinate::M>()));
  EXPECT_TRUE(default_coord.isValid());

  const Coordinate two_arg(1.0, 2.0);
  EXPECT_DOUBLE_EQ(two_arg.x, 1.0);
  EXPECT_DOUBLE_EQ(two_arg.y, 2.0);
  EXPECT_TRUE(std::isnan(two_arg.z));

  const Coordinate three_arg(1.0, 2.0, 3.5);
  EXPECT_DOUBLE_EQ(three_arg.get<Ordinate::X>(), 1.0);
  EXPECT_DOUBLE_EQ(three_arg.get<Ordinate::Y>(), 2.0);
  EXPECT_DOUBLE_EQ(three_arg.get<Ordinate::Z>(), 3.5);
  EXPECT_TRUE(std::isnan(three_arg.get<Ordinate::M>()));

  const Coordinate from_xy(CoordinateXY(4.5, -5.5));
  EXPECT_DOUBLE_EQ(from_xy.x, 4.5);
  EXPECT_DOUBLE_EQ(from_xy.y, -5.5);
  EXPECT_TRUE(std::isnan(from_xy.z));
}

TEST(CoordinateTest, GetNullIsNullAndSetNull) {
  const Coordinate& null_coord = Coordinate::getNull();
  EXPECT_EQ(&null_coord, &Coordinate::getNull());
  EXPECT_TRUE(null_coord.isNull());
  EXPECT_FALSE(null_coord.isValid());
  EXPECT_TRUE(std::isnan(null_coord.x));
  EXPECT_TRUE(std::isnan(null_coord.y));
  EXPECT_TRUE(std::isnan(null_coord.z));

  const Coordinate nan_xy_finite_z(DoubleNotANumber, DoubleNotANumber, 5.0);
  EXPECT_TRUE(nan_xy_finite_z.CoordinateXY::isNull());
  EXPECT_FALSE(nan_xy_finite_z.isNull());

  Coordinate coord(1.0, 2.0, 3.0);
  EXPECT_FALSE(coord.isNull());
  coord.setNull();
  EXPECT_TRUE(coord.isNull());
  EXPECT_TRUE(std::isnan(coord.x));
  EXPECT_TRUE(std::isnan(coord.y));
  EXPECT_TRUE(std::isnan(coord.z));
}

TEST(CoordinateTest, Equals3D) {
  const Coordinate c123(1.0, 2.0, 3.0);
  const Coordinate c123_dup(1.0, 2.0, 3.0);
  const Coordinate c124(1.0, 2.0, 4.0);
  const Coordinate c923(9.0, 2.0, 3.0);
  const Coordinate c193(1.0, 9.0, 3.0);
  const Coordinate c12_nan(1.0, 2.0);
  const Coordinate c12_nan_dup(1.0, 2.0, DoubleNotANumber);

  EXPECT_TRUE(c123.equals3D(c123_dup));
  EXPECT_FALSE(c123.equals3D(c124));
  EXPECT_FALSE(c123.equals3D(c923));
  EXPECT_FALSE(c123.equals3D(c193));
  EXPECT_FALSE(c123.equals3D(c12_nan));
  EXPECT_FALSE(c12_nan.equals3D(c123));
  EXPECT_TRUE(c12_nan.equals3D(c12_nan_dup));
}

TEST(CoordinateTest, AssignmentFromCoordinateAndCoordinateXY) {
  Coordinate coord(1.0, 2.0, 3.0);
  const Coordinate other(4.0, 5.0, 6.0);
  coord = other;
  EXPECT_TRUE(coord.equals3D(other));

  coord = CoordinateXY(7.0, 8.0);
  EXPECT_DOUBLE_EQ(coord.x, 7.0);
  EXPECT_DOUBLE_EQ(coord.y, 8.0);
  EXPECT_TRUE(std::isnan(coord.z));
}

TEST(CoordinateTest, ToStringAndStreamOutput) {
  const Coordinate with_z(1.5, -2.25, 3.75);
  EXPECT_EQ(with_z.toString(), "1.5 -2.25 3.75");

  const Coordinate without_z(1.5, -2.25);
  EXPECT_EQ(without_z.toString(), "1.5 -2.25");

  const Coordinate high_prec(1.0, 2.0, kHighPrecisionExact);
  EXPECT_EQ(high_prec.toString(), "1 2 1000000000000001");

  std::ostringstream oss;
  oss << with_z << " | " << without_z;
  EXPECT_EQ(oss.str(), "1.5 -2.25 3.75 | 1.5 -2.25");
}

TEST(CoordinateTest, ContainerTypedefsAndLessThan) {
  const Coordinate a(1.0, 2.0, 10.0);
  const Coordinate b(1.0, 3.0, 20.0);
  const Coordinate c(2.0, 1.0, 30.0);
  const Coordinate a_dup(1.0, 2.0, 99.0);

  const Coordinate::ConstSet ptr_set = {&c, &a, &b, &a_dup};
  ASSERT_EQ(ptr_set.size(), 3u);
  auto it = ptr_set.begin();
  EXPECT_TRUE((*it++)->equals3D(a));
  EXPECT_TRUE((*it++)->equals3D(b));
  EXPECT_TRUE((*it++)->equals3D(c));

  const Coordinate::ConstXYSet xy_ptr_set = {&c, &a, &b, &a_dup};
  EXPECT_EQ(xy_ptr_set.size(), 3u);

  Coordinate::ConstIntMap coord_map;
  coord_map[&b] = 2;
  coord_map[&a] = 1;
  coord_map[&c] = 3;
  EXPECT_EQ(coord_map.at(&a_dup), 1);

  Coordinate::ConstVect ptr_vect = {&a, &b, &c};
  EXPECT_EQ(ptr_vect.size(), 3u);

  Coordinate::ConstStack ptr_stack;
  ptr_stack.push(&a);
  ptr_stack.push(&c);
  EXPECT_EQ(ptr_stack.top(), &c);

  Coordinate::Vect coords = {a, b, c};
  EXPECT_EQ(coords.size(), 3u);
}

TEST(CoordinateXYMTest, ConstructorsAndOrdinateAccessors) {
  const CoordinateXYM default_coord;
  EXPECT_DOUBLE_EQ(default_coord.x, 0.0);
  EXPECT_DOUBLE_EQ(default_coord.y, 0.0);
  EXPECT_TRUE(std::isnan(default_coord.m));
  EXPECT_TRUE(std::isnan(default_coord.get<Ordinate::Z>()));
  EXPECT_TRUE(std::isnan(default_coord.get<Ordinate::M>()));

  const CoordinateXYM from_xy(CoordinateXY(3.5, 4.5));
  EXPECT_DOUBLE_EQ(from_xy.x, 3.5);
  EXPECT_DOUBLE_EQ(from_xy.y, 4.5);
  EXPECT_TRUE(std::isnan(from_xy.m));

  const CoordinateXYM full(1.0, 2.0, 5.5);
  EXPECT_DOUBLE_EQ(full.get<Ordinate::X>(), 1.0);
  EXPECT_DOUBLE_EQ(full.get<Ordinate::Y>(), 2.0);
  EXPECT_TRUE(std::isnan(full.get<Ordinate::Z>()));
  EXPECT_DOUBLE_EQ(full.get<Ordinate::M>(), 5.5);
}

TEST(CoordinateXYMTest, GetNullIsNullAndSetNull) {
  const CoordinateXYM& null_coord = CoordinateXYM::getNull();
  EXPECT_EQ(&null_coord, &CoordinateXYM::getNull());
  EXPECT_TRUE(null_coord.isNull());
  EXPECT_FALSE(null_coord.isValid());
  EXPECT_TRUE(std::isnan(null_coord.x));
  EXPECT_TRUE(std::isnan(null_coord.y));
  EXPECT_TRUE(std::isnan(null_coord.m));

  const CoordinateXYM nan_xy_finite_m(DoubleNotANumber, DoubleNotANumber, 2.0);
  EXPECT_TRUE(nan_xy_finite_m.CoordinateXY::isNull());
  EXPECT_FALSE(nan_xy_finite_m.isNull());

  CoordinateXYM coord(1.0, 2.0, 3.0);
  coord.setNull();
  EXPECT_TRUE(coord.isNull());
  EXPECT_TRUE(std::isnan(coord.x));
  EXPECT_TRUE(std::isnan(coord.y));
  EXPECT_TRUE(std::isnan(coord.m));
}

TEST(CoordinateXYMTest, Equals3DAndAssignments) {
  const CoordinateXYM a(1.0, 2.0, 4.0);
  const CoordinateXYM a_dup(1.0, 2.0, 4.0);
  const CoordinateXYM diff_m(1.0, 2.0, 5.0);
  const CoordinateXYM diff_x(9.0, 2.0, 4.0);
  const CoordinateXYM diff_y(1.0, 9.0, 4.0);
  const CoordinateXYM nan_m(CoordinateXY(1.0, 2.0));
  const CoordinateXYM nan_m_dup(1.0, 2.0, DoubleNotANumber);

  EXPECT_TRUE(a.equals3D(a_dup));
  EXPECT_FALSE(a.equals3D(diff_m));
  EXPECT_FALSE(a.equals3D(diff_x));
  EXPECT_FALSE(a.equals3D(diff_y));
  EXPECT_FALSE(a.equals3D(nan_m));
  EXPECT_FALSE(nan_m.equals3D(a));
  EXPECT_TRUE(nan_m.equals3D(nan_m_dup));

  CoordinateXYM target(0.0, 0.0, 9.0);
  target = CoordinateXYZM(7.0, 8.0, 99.0, 10.5);
  EXPECT_DOUBLE_EQ(target.x, 7.0);
  EXPECT_DOUBLE_EQ(target.y, 8.0);
  EXPECT_DOUBLE_EQ(target.m, 10.5);

  target = CoordinateXY(11.0, 12.0);
  EXPECT_DOUBLE_EQ(target.x, 11.0);
  EXPECT_DOUBLE_EQ(target.y, 12.0);
  EXPECT_TRUE(std::isnan(target.m));
}

TEST(CoordinateXYMTest, ToStringAndStreamOutput) {
  const CoordinateXYM with_m(1.5, 2.5, 4.25);
  EXPECT_EQ(with_m.toString(), "1.5 2.5 4.25");

  const CoordinateXYM without_m(CoordinateXY(1.5, 2.5));
  EXPECT_EQ(without_m.toString(), "1.5 2.5");

  const CoordinateXYM high_prec(1.0, 2.0, kHighPrecisionExact);
  EXPECT_EQ(high_prec.toString(), "1 2 1000000000000001");

  std::ostringstream oss;
  oss << with_m << " | " << without_m;
  EXPECT_EQ(oss.str(), "1.5 2.5 4.25 | 1.5 2.5");
}

TEST(CoordinateXYZMTest, ConstructorsAndOrdinateAccessors) {
  const CoordinateXYZM default_coord;
  EXPECT_DOUBLE_EQ(default_coord.x, 0.0);
  EXPECT_DOUBLE_EQ(default_coord.y, 0.0);
  EXPECT_TRUE(std::isnan(default_coord.z));
  EXPECT_TRUE(std::isnan(default_coord.m));

  const CoordinateXYZM from_xy(CoordinateXY(1.0, 2.0));
  EXPECT_DOUBLE_EQ(from_xy.x, 1.0);
  EXPECT_DOUBLE_EQ(from_xy.y, 2.0);
  EXPECT_TRUE(std::isnan(from_xy.z));
  EXPECT_TRUE(std::isnan(from_xy.m));

  const CoordinateXYZM from_xyz(Coordinate(1.0, 2.0, 3.0));
  EXPECT_DOUBLE_EQ(from_xyz.x, 1.0);
  EXPECT_DOUBLE_EQ(from_xyz.y, 2.0);
  EXPECT_DOUBLE_EQ(from_xyz.z, 3.0);
  EXPECT_TRUE(std::isnan(from_xyz.m));

  const CoordinateXYZM from_xym(CoordinateXYM(1.0, 2.0, 4.0));
  EXPECT_DOUBLE_EQ(from_xym.x, 1.0);
  EXPECT_DOUBLE_EQ(from_xym.y, 2.0);
  EXPECT_TRUE(std::isnan(from_xym.z));
  EXPECT_DOUBLE_EQ(from_xym.m, 4.0);

  const CoordinateXYZM full(1.0, 2.0, 3.0, 4.0);
  EXPECT_DOUBLE_EQ(full.get<Ordinate::X>(), 1.0);
  EXPECT_DOUBLE_EQ(full.get<Ordinate::Y>(), 2.0);
  EXPECT_DOUBLE_EQ(full.get<Ordinate::Z>(), 3.0);
  EXPECT_DOUBLE_EQ(full.get<Ordinate::M>(), 4.0);
}

TEST(CoordinateXYZMTest, GetNullIsNullAndSetNull) {
  const CoordinateXYZM& null_coord = CoordinateXYZM::getNull();
  EXPECT_EQ(&null_coord, &CoordinateXYZM::getNull());
  EXPECT_TRUE(null_coord.isNull());
  EXPECT_FALSE(null_coord.isValid());
  EXPECT_TRUE(std::isnan(null_coord.x));
  EXPECT_TRUE(std::isnan(null_coord.y));
  EXPECT_TRUE(std::isnan(null_coord.z));
  EXPECT_TRUE(std::isnan(null_coord.m));

  const CoordinateXYZM only_z_finite(DoubleNotANumber, DoubleNotANumber, 3.0,
                                     DoubleNotANumber);
  EXPECT_FALSE(only_z_finite.isNull());

  const CoordinateXYZM only_m_finite(DoubleNotANumber, DoubleNotANumber,
                                     DoubleNotANumber, 4.0);
  EXPECT_TRUE(only_m_finite.Coordinate::isNull());
  EXPECT_FALSE(only_m_finite.isNull());

  CoordinateXYZM coord(1.0, 2.0, 3.0, 4.0);
  coord.setNull();
  EXPECT_TRUE(coord.isNull());
  EXPECT_TRUE(std::isnan(coord.x));
  EXPECT_TRUE(std::isnan(coord.y));
  EXPECT_TRUE(std::isnan(coord.z));
  EXPECT_TRUE(std::isnan(coord.m));
}

TEST(CoordinateXYZMTest, Equals4DAndAssignments) {
  const CoordinateXYZM base(1.0, 2.0, 3.0, 4.0);
  const CoordinateXYZM base_dup(1.0, 2.0, 3.0, 4.0);
  const CoordinateXYZM diff_x(9.0, 2.0, 3.0, 4.0);
  const CoordinateXYZM diff_y(1.0, 9.0, 3.0, 4.0);
  const CoordinateXYZM diff_z(1.0, 2.0, 9.0, 4.0);
  const CoordinateXYZM diff_m(1.0, 2.0, 3.0, 9.0);
  const CoordinateXYZM nan_z(1.0, 2.0, DoubleNotANumber, 4.0);
  const CoordinateXYZM nan_z_dup(1.0, 2.0, DoubleNotANumber, 4.0);
  const CoordinateXYZM nan_m(1.0, 2.0, 3.0, DoubleNotANumber);
  const CoordinateXYZM nan_zm(1.0, 2.0, DoubleNotANumber, DoubleNotANumber);
  const CoordinateXYZM nan_zm_dup(1.0, 2.0, DoubleNotANumber, DoubleNotANumber);

  EXPECT_TRUE(base.equals4D(base_dup));
  EXPECT_FALSE(base.equals4D(diff_x));
  EXPECT_FALSE(base.equals4D(diff_y));
  EXPECT_FALSE(base.equals4D(diff_z));
  EXPECT_FALSE(base.equals4D(diff_m));
  EXPECT_FALSE(base.equals4D(nan_z));
  EXPECT_FALSE(nan_z.equals4D(base));
  EXPECT_FALSE(base.equals4D(nan_m));
  EXPECT_FALSE(nan_m.equals4D(base));
  EXPECT_TRUE(nan_z.equals4D(nan_z_dup));
  EXPECT_TRUE(nan_zm.equals4D(nan_zm_dup));

  CoordinateXYZM target(1.0, 2.0, 3.0, 4.0);
  target = CoordinateXY(5.0, 6.0);
  EXPECT_DOUBLE_EQ(target.x, 5.0);
  EXPECT_DOUBLE_EQ(target.y, 6.0);
  EXPECT_TRUE(std::isnan(target.z));
  EXPECT_TRUE(std::isnan(target.m));

  target = CoordinateXYZM(1.0, 2.0, 3.0, 4.0);
  target = Coordinate(7.0, 8.0, 9.0);
  EXPECT_DOUBLE_EQ(target.x, 7.0);
  EXPECT_DOUBLE_EQ(target.y, 8.0);
  EXPECT_DOUBLE_EQ(target.z, 9.0);
  EXPECT_TRUE(std::isnan(target.m));

  target = CoordinateXYZM(1.0, 2.0, 3.0, 4.0);
  target = CoordinateXYM(10.0, 11.0, 12.0);
  EXPECT_DOUBLE_EQ(target.x, 10.0);
  EXPECT_DOUBLE_EQ(target.y, 11.0);
  EXPECT_TRUE(std::isnan(target.z));
  EXPECT_DOUBLE_EQ(target.m, 12.0);
}

TEST(CoordinateXYZMTest, ToStringAndStreamOutputAllNanCombinations) {
  const CoordinateXYZM full(1.5, 2.5, 3.5, 4.5);
  EXPECT_EQ(full.toString(), "1.5 2.5 3.5 4.5");

  const CoordinateXYZM z_only(1.5, 2.5, 3.5, DoubleNotANumber);
  EXPECT_EQ(z_only.toString(), "1.5 2.5 3.5");

  const CoordinateXYZM m_only(1.5, 2.5, DoubleNotANumber, 4.5);
  std::ostringstream expected_m_only;
  expected_m_only << std::setprecision(17) << 1.5 << " " << 2.5 << " "
                  << DoubleNotANumber << " " << 4.5;
  EXPECT_EQ(m_only.toString(), expected_m_only.str());

  const CoordinateXYZM xy_only(1.5, 2.5, DoubleNotANumber, DoubleNotANumber);
  EXPECT_EQ(xy_only.toString(), "1.5 2.5");

  const CoordinateXYZM high_prec(1.0, 2.0, 3.0, kHighPrecisionExact);
  EXPECT_EQ(high_prec.toString(), "1 2 3 1000000000000001");

  std::ostringstream oss;
  oss << full << " | " << z_only << " | " << xy_only;
  EXPECT_EQ(oss.str(), "1.5 2.5 3.5 4.5 | 1.5 2.5 3.5 | 1.5 2.5");
}

}  // namespace
}  // namespace geos_google
