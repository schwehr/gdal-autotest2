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

// Unit tests for third_party/geos/src/algorithm/Length.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/Length.cpp

#include "third_party/geos/include/geos/algorithm/Length.h"

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"

namespace geos_google {
namespace {

using geos::algorithm::Length;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;

struct Point2D {
  double x;
  double y;
};

struct LengthCase {
  std::string_view name;
  std::span<const Point2D> points;
  double expected_length;
};

constexpr std::array<Point2D, 0> kEmptyPts{};
constexpr std::array<Point2D, 1> kSinglePt{{{3.5, -2.5}}};
constexpr std::array<Point2D, 2> kRepeatedPt{{{5.0, -3.0}, {5.0, -3.0}}};
constexpr std::array<Point2D, 2> kHorizontalPts{{{-2.0, 4.0}, {6.0, 4.0}}};
constexpr std::array<Point2D, 2> kVerticalPts{{{1.0, -5.0}, {1.0, 7.0}}};
constexpr std::array<Point2D, 2> kPythagorean345{{{0.0, 0.0}, {3.0, 4.0}}};
constexpr std::array<Point2D, 2> kPythagorean51213{{{-1.0, -2.0}, {4.0, 10.0}}};
constexpr std::array<Point2D, 3> kLShapePts{
    {{0.0, 0.0}, {3.0, 0.0}, {3.0, 4.0}}};
constexpr std::array<Point2D, 5> kUnitSquareRing{
    {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}, {0.0, 0.0}}};
constexpr std::array<Point2D, 3> kBacktrackPts{
    {{0.0, 0.0}, {3.0, 4.0}, {0.0, 0.0}}};
constexpr std::array<Point2D, 3> kFractionalNegativePts{
    {{0.5, -1.5}, {2.0, 0.5}, {-1.0, 4.5}}};

constexpr std::array<LengthCase, 11> kExactCases{{
    {.name = "empty", .points = kEmptyPts, .expected_length = 0.0},
    {.name = "single_point", .points = kSinglePt, .expected_length = 0.0},
    {.name = "repeated_point", .points = kRepeatedPt, .expected_length = 0.0},
    {.name = "horizontal", .points = kHorizontalPts, .expected_length = 8.0},
    {.name = "vertical", .points = kVerticalPts, .expected_length = 12.0},
    {.name = "triangle_3_4_5",
     .points = kPythagorean345,
     .expected_length = 5.0},
    {.name = "triangle_5_12_13",
     .points = kPythagorean51213,
     .expected_length = 13.0},
    {.name = "l_shape", .points = kLShapePts, .expected_length = 7.0},
    {.name = "unit_square_ring",
     .points = kUnitSquareRing,
     .expected_length = 4.0},
    {.name = "self_retracing_path",
     .points = kBacktrackPts,
     .expected_length = 10.0},
    {.name = "fractional_negative",
     .points = kFractionalNegativePts,
     .expected_length = 7.5},
}};

std::vector<CoordinateXY> MakeVector(std::span<const Point2D> pts) {
  std::vector<CoordinateXY> coords;
  coords.reserve(pts.size());
  for (const Point2D& pt : pts) {
    coords.emplace_back(pt.x, pt.y);
  }
  return coords;
}

CoordinateSequence MakeSequence(std::span<const Point2D> pts) {
  CoordinateSequence seq = CoordinateSequence::XY(0);
  seq.reserve(pts.size());
  for (const Point2D& pt : pts) {
    seq.add(pt.x, pt.y);
  }
  return seq;
}

TEST(LengthTest, ExactPlanarLengthsBothOverloads) {
  for (const LengthCase& tc : kExactCases) {
    SCOPED_TRACE(tc.name);
    const std::vector<CoordinateXY> vec = MakeVector(tc.points);
    const CoordinateSequence seq = MakeSequence(tc.points);
    EXPECT_DOUBLE_EQ(Length::ofLine(vec), tc.expected_length);
    EXPECT_DOUBLE_EQ(Length::ofLine(&seq), tc.expected_length);
  }
}

TEST(LengthTest, EmptyAndSinglePointAcrossCoordinateDimensions) {
  const CoordinateSequence empty_xyz = CoordinateSequence::XYZ(0);
  const CoordinateSequence empty_xym = CoordinateSequence::XYM(0);
  const CoordinateSequence empty_xyzm = CoordinateSequence::XYZM(0);
  EXPECT_DOUBLE_EQ(Length::ofLine(&empty_xyz), 0.0);
  EXPECT_DOUBLE_EQ(Length::ofLine(&empty_xym), 0.0);
  EXPECT_DOUBLE_EQ(Length::ofLine(&empty_xyzm), 0.0);

  const CoordinateSequence single_xyz{Coordinate(1.0, 2.0, 99.0)};
  const CoordinateSequence single_xym{CoordinateXYM(1.0, 2.0, 42.0)};
  const CoordinateSequence single_xyzm{CoordinateXYZM(1.0, 2.0, 99.0, 42.0)};
  EXPECT_DOUBLE_EQ(Length::ofLine(&single_xyz), 0.0);
  EXPECT_DOUBLE_EQ(Length::ofLine(&single_xym), 0.0);
  EXPECT_DOUBLE_EQ(Length::ofLine(&single_xyzm), 0.0);
}

TEST(LengthTest, IrrationalDiagonalSegmentsAccumulate) {
  const std::vector<CoordinateXY> vec{
      {0.0, 0.0},
      {1.0, 1.0},
      {2.0, 0.0},
      {3.0, 1.0},
  };
  const CoordinateSequence seq{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 1.0),
      CoordinateXY(2.0, 0.0),
      CoordinateXY(3.0, 1.0),
  };
  const double expected = 3.0 * std::numbers::sqrt2;
  EXPECT_NEAR(Length::ofLine(vec), expected, 1e-12);
  EXPECT_NEAR(Length::ofLine(&seq), expected, 1e-12);
}

TEST(LengthTest, IgnoresZAndMOrdinates) {
  const CoordinateSequence xyz_seq{
      Coordinate(0.0, 0.0, -500.0),
      Coordinate(3.0, 4.0, 1000.0),
      Coordinate(3.0, 16.0, 2000.0),
  };
  const CoordinateSequence xym_seq{
      CoordinateXYM(0.0, 0.0, 10.0),
      CoordinateXYM(3.0, 4.0, 250.0),
      CoordinateXYM(3.0, 16.0, -75.0),
  };
  const CoordinateSequence xyzm_seq{
      CoordinateXYZM(0.0, 0.0, 100.0, 10.0),
      CoordinateXYZM(3.0, 4.0, -200.0, 250.0),
      CoordinateXYZM(3.0, 16.0, 300.0, -75.0),
  };

  EXPECT_DOUBLE_EQ(Length::ofLine(&xyz_seq), 17.0);
  EXPECT_DOUBLE_EQ(Length::ofLine(&xym_seq), 17.0);
  EXPECT_DOUBLE_EQ(Length::ofLine(&xyzm_seq), 17.0);
}

TEST(LengthTest, NanCoordinatesProduceNan) {
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<CoordinateXY> vec{{0.0, 0.0}, {kNan, 1.0}};
  const CoordinateSequence seq{CoordinateXY(0.0, 0.0), CoordinateXY(1.0, kNan)};

  EXPECT_TRUE(std::isnan(Length::ofLine(vec)));
  EXPECT_TRUE(std::isnan(Length::ofLine(&seq)));
}

TEST(LengthTest, LargeOffsetSegmentsPreservePrecision) {
  constexpr double kOffset = 1.0e8;
  const std::vector<CoordinateXY> vec{
      {kOffset, kOffset},
      {kOffset + 3.0, kOffset + 4.0},
      {kOffset + 3.0, kOffset + 16.0},
  };
  const CoordinateSequence seq{
      CoordinateXY(kOffset, kOffset),
      CoordinateXY(kOffset + 3.0, kOffset + 4.0),
      CoordinateXY(kOffset + 3.0, kOffset + 16.0),
  };
  EXPECT_NEAR(Length::ofLine(vec), 17.0, 1e-7);
  EXPECT_NEAR(Length::ofLine(&seq), 17.0, 1e-7);
}

}  // namespace
}  // namespace geos_google
