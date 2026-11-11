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

// Unit tests for third_party/geos/include/geos/algorithm/Interpolate.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/algorithm/Interpolate.h

#include "third_party/geos/include/geos/algorithm/Interpolate.h"

#include <cmath>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::algorithm::Interpolate;
using geos::geom::Coordinate;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;

TEST(InterpolateTest, ZInterpolateSinglePairNaNEndpoints) {
  const CoordinateXY p(2.5, 0.0);

  // Both endpoints lack Z (CoordinateXY, default Coordinate, CoordinateXYM).
  EXPECT_TRUE(std::isnan(Interpolate::zInterpolate(p, CoordinateXY(0.0, 0.0),
                                                   CoordinateXY(10.0, 0.0))));
  EXPECT_TRUE(std::isnan(Interpolate::zInterpolate(p, Coordinate(0.0, 0.0),
                                                   Coordinate(10.0, 0.0))));
  EXPECT_TRUE(std::isnan(Interpolate::zInterpolate(
      p, CoordinateXYM(0.0, 0.0, 1.0), CoordinateXYM(10.0, 0.0, 2.0))));

  // First endpoint Z is NaN: returns second endpoint Z directly.
  EXPECT_DOUBLE_EQ(
      Interpolate::zInterpolate(p, Coordinate(0.0, 0.0, DoubleNotANumber),
                                Coordinate(10.0, 0.0, 42.5)),
      42.5);
  EXPECT_DOUBLE_EQ(Interpolate::zInterpolate(
                       p, CoordinateXYZM(0.0, 0.0, DoubleNotANumber, 1.0),
                       CoordinateXYZM(10.0, 0.0, -13.25, 2.0)),
                   -13.25);

  // Second endpoint Z is NaN: returns first endpoint Z directly.
  EXPECT_DOUBLE_EQ(
      Interpolate::zInterpolate(p, Coordinate(0.0, 0.0, 17.25),
                                Coordinate(10.0, 0.0, DoubleNotANumber)),
      17.25);
  EXPECT_DOUBLE_EQ(Interpolate::zInterpolate(
                       p, CoordinateXYZM(0.0, 0.0, 9.5, 1.0),
                       CoordinateXYZM(10.0, 0.0, DoubleNotANumber, 2.0)),
                   9.5);
}

TEST(InterpolateTest, ZInterpolateSinglePairEndpointAndConstantMatches) {
  const Coordinate p1(1.0, 2.0, 10.0);
  const Coordinate p2(5.0, 6.0, 30.0);

  // Matches p1 in 2D (even when p carries its own distinct Z, which slices).
  EXPECT_DOUBLE_EQ(Interpolate::zInterpolate(CoordinateXY(1.0, 2.0), p1, p2),
                   10.0);
  EXPECT_DOUBLE_EQ(
      Interpolate::zInterpolate(Coordinate(1.0, 2.0, 999.0), p1, p2), 10.0);

  // Matches p2 in 2D.
  EXPECT_DOUBLE_EQ(Interpolate::zInterpolate(CoordinateXY(5.0, 6.0), p1, p2),
                   30.0);

  // Zero-length segment when p matches both endpoints returns p1.z safely.
  EXPECT_DOUBLE_EQ(Interpolate::zInterpolate(CoordinateXY(1.0, 2.0),
                                             Coordinate(1.0, 2.0, 11.0),
                                             Coordinate(1.0, 2.0, 22.0)),
                   11.0);

  // Equal Z at both endpoints (dz == 0.0) returns constant Z directly.
  EXPECT_DOUBLE_EQ(Interpolate::zInterpolate(CoordinateXY(3.0, 3.0),
                                             Coordinate(0.0, 0.0, -7.5),
                                             Coordinate(10.0, 10.0, -7.5)),
                   -7.5);
}

TEST(InterpolateTest, ZInterpolateSinglePairLinearAlongSegments) {
  struct ZCase {
    std::string_view name;
    CoordinateXY p;
    Coordinate p1;
    Coordinate p2;
    double expected_z;
  };

  const ZCase kCases[] = {
      {
          .name = "horizontal_ascending_quarter",
          .p = CoordinateXY(2.0, 0.0),
          .p1 = Coordinate(0.0, 0.0, 10.0),
          .p2 = Coordinate(8.0, 0.0, 26.0),
          .expected_z = 14.0,
      },
      {
          .name = "horizontal_descending_three_quarters",
          .p = CoordinateXY(6.0, 0.0),
          .p1 = Coordinate(0.0, 0.0, 26.0),
          .p2 = Coordinate(8.0, 0.0, 10.0),
          .expected_z = 14.0,
      },
      {
          .name = "vertical_quarter",
          .p = CoordinateXY(3.0, -2.0),
          .p1 = Coordinate(3.0, -4.0, 0.0),
          .p2 = Coordinate(3.0, 4.0, 80.0),
          .expected_z = 20.0,
      },
      {
          .name = "diagonal_3_4_5_midpoint",
          .p = CoordinateXY(2.5, 4.0),
          .p1 = Coordinate(1.0, 2.0, -4.0),
          .p2 = Coordinate(4.0, 6.0, 16.0),
          .expected_z = 6.0,
      },
      {
          .name = "non_collinear_distance_ratio",
          .p = CoordinateXY(0.0, 2.0),
          .p1 = Coordinate(0.0, 0.0, 0.0),
          .p2 = Coordinate(4.0, 0.0, 10.0),
          .expected_z = 5.0,
      },
  };

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_DOUBLE_EQ(Interpolate::zInterpolate(tc.p, tc.p1, tc.p2),
                     tc.expected_z);
  }

  // Also verify CoordinateXYZM ignores M and interpolates Z.
  EXPECT_DOUBLE_EQ(
      Interpolate::zInterpolate(CoordinateXY(1.0, 0.0),
                                CoordinateXYZM(0.0, 0.0, 100.0, 1.0),
                                CoordinateXYZM(4.0, 0.0, 120.0, 9.0)),
      105.0);
}

TEST(InterpolateTest, MInterpolateSinglePairAllBranches) {
  const CoordinateXY p(2.0, 0.0);

  // Types without M return NaN.
  EXPECT_TRUE(std::isnan(Interpolate::mInterpolate(p, CoordinateXY(0.0, 0.0),
                                                   CoordinateXY(8.0, 0.0))));
  EXPECT_TRUE(std::isnan(Interpolate::mInterpolate(p, Coordinate(0.0, 0.0, 5.0),
                                                   Coordinate(8.0, 0.0, 9.0))));
  EXPECT_TRUE(std::isnan(
      Interpolate::mInterpolate(p, CoordinateXYM(CoordinateXY(0.0, 0.0)),
                                CoordinateXYM(CoordinateXY(8.0, 0.0)))));

  // One NaN endpoint returns the other endpoint's M.
  EXPECT_DOUBLE_EQ(
      Interpolate::mInterpolate(p, CoordinateXYM(0.0, 0.0, DoubleNotANumber),
                                CoordinateXYM(8.0, 0.0, 12.5)),
      12.5);
  EXPECT_DOUBLE_EQ(
      Interpolate::mInterpolate(p, CoordinateXYM(0.0, 0.0, 8.25),
                                CoordinateXYM(8.0, 0.0, DoubleNotANumber)),
      8.25);

  // Endpoint 2D matches and equal M (dm == 0.0).
  const CoordinateXYM m1(1.0, 2.0, 100.0);
  const CoordinateXYM m2(5.0, 6.0, 140.0);
  EXPECT_DOUBLE_EQ(Interpolate::mInterpolate(CoordinateXY(1.0, 2.0), m1, m2),
                   100.0);
  EXPECT_DOUBLE_EQ(Interpolate::mInterpolate(CoordinateXY(5.0, 6.0), m1, m2),
                   140.0);
  EXPECT_DOUBLE_EQ(Interpolate::mInterpolate(CoordinateXY(3.0, 4.0),
                                             CoordinateXYM(1.0, 2.0, 55.5),
                                             CoordinateXYM(5.0, 6.0, 55.5)),
                   55.5);

  // Linear interpolation for CoordinateXYM and CoordinateXYZM.
  EXPECT_DOUBLE_EQ(Interpolate::mInterpolate(CoordinateXY(0.0, 2.0),
                                             CoordinateXYM(0.0, 0.0, 100.0),
                                             CoordinateXYM(0.0, 8.0, 132.0)),
                   108.0);
  EXPECT_DOUBLE_EQ(
      Interpolate::mInterpolate(CoordinateXY(1.5, 2.0),
                                CoordinateXYZM(0.0, 0.0, 500.0, 20.0),
                                CoordinateXYZM(3.0, 4.0, 900.0, 10.0)),
      15.0);
}

TEST(InterpolateTest, ZInterpolateTwoPairsAveragesAndHandlesNaN) {
  const CoordinateXY p(2.0, 0.0);
  const Coordinate p1(0.0, 0.0, 10.0);
  const Coordinate p2(4.0, 0.0, 20.0);  // zp = 15.0
  const CoordinateXYZM q1(2.0, -4.0, 20.0, 100.0);
  const CoordinateXYZM q2(2.0, 4.0, 30.0, 200.0);  // zq = 25.0
  const CoordinateXY xy1(0.0, 0.0);
  const CoordinateXY xy2(4.0, 0.0);
  const CoordinateXYM xym1(2.0, -4.0, 1.0);
  const CoordinateXYM xym2(2.0, 4.0, 2.0);

  // Both pairs produce finite Z (supports distinct coordinate types C1, C2).
  EXPECT_DOUBLE_EQ(Interpolate::zInterpolate(p, p1, p2, q1, q2), 20.0);

  // First pair yields NaN -> returns second pair's interpolated Z.
  EXPECT_DOUBLE_EQ(Interpolate::zInterpolate(p, xy1, xy2, q1, q2), 25.0);

  // Second pair yields NaN -> returns first pair's interpolated Z.
  EXPECT_DOUBLE_EQ(Interpolate::zInterpolate(p, p1, p2, xym1, xym2), 15.0);

  // Both pairs yield NaN -> returns NaN.
  EXPECT_TRUE(std::isnan(Interpolate::zInterpolate(p, xy1, xy2, xym1, xym2)));

  // Partial NaN in one pair collapses to the non-NaN endpoint before averaging.
  EXPECT_DOUBLE_EQ(
      Interpolate::zInterpolate(p, Coordinate(0.0, 0.0, DoubleNotANumber),
                                Coordinate(4.0, 0.0, 15.0), q1, q2),
      20.0);
}

TEST(InterpolateTest, MInterpolateTwoPairsAveragesAndHandlesNaN) {
  const CoordinateXY p(2.0, 0.0);
  const CoordinateXYM p1(0.0, 0.0, 100.0);
  const CoordinateXYM p2(4.0, 0.0, 120.0);  // mp = 110.0
  const CoordinateXYZM q1(2.0, -2.0, 1.0, 200.0);
  const CoordinateXYZM q2(2.0, 2.0, 9.0, 240.0);  // mq = 220.0
  const Coordinate z1(0.0, 0.0, 10.0);
  const Coordinate z2(4.0, 0.0, 20.0);
  const CoordinateXY xy1(2.0, -2.0);
  const CoordinateXY xy2(2.0, 2.0);

  // Both pairs produce finite M (mixed C1 = CoordinateXYM, C2 =
  // CoordinateXYZM).
  EXPECT_DOUBLE_EQ(Interpolate::mInterpolate(p, p1, p2, q1, q2), 165.0);

  // First pair lacks M -> returns second pair's interpolated M.
  EXPECT_DOUBLE_EQ(Interpolate::mInterpolate(p, z1, z2, q1, q2), 220.0);

  // Second pair lacks M -> returns first pair's interpolated M.
  EXPECT_DOUBLE_EQ(Interpolate::mInterpolate(p, p1, p2, xy1, xy2), 110.0);

  // Both pairs lack M -> returns NaN.
  EXPECT_TRUE(std::isnan(Interpolate::mInterpolate(p, z1, z2, xy1, xy2)));
}

TEST(InterpolateTest, ZGetAndMGetPreferFirstNonNaN) {
  // zGet across all combinations of finite and NaN Z.
  EXPECT_DOUBLE_EQ(Interpolate::zGet(Coordinate(1.0, 2.0, 5.0),
                                     CoordinateXYZM(1.0, 2.0, 9.0, 3.0)),
                   5.0);
  EXPECT_DOUBLE_EQ(
      Interpolate::zGet(Coordinate(1.0, 2.0, 5.0), CoordinateXY(1.0, 2.0)),
      5.0);
  EXPECT_DOUBLE_EQ(Interpolate::zGet(CoordinateXYM(1.0, 2.0, 7.0),
                                     Coordinate(1.0, 2.0, 11.0)),
                   11.0);
  EXPECT_DOUBLE_EQ(Interpolate::zGet(Coordinate(1.0, 2.0),
                                     CoordinateXYZM(1.0, 2.0, 12.0, 4.0)),
                   12.0);
  EXPECT_TRUE(std::isnan(
      Interpolate::zGet(CoordinateXY(1.0, 2.0), CoordinateXYM(1.0, 2.0, 7.0))));

  // mGet across all combinations of finite and NaN M.
  EXPECT_DOUBLE_EQ(Interpolate::mGet(CoordinateXYM(1.0, 2.0, 7.0),
                                     CoordinateXYZM(1.0, 2.0, 9.0, 13.0)),
                   7.0);
  EXPECT_DOUBLE_EQ(Interpolate::mGet(CoordinateXYM(1.0, 2.0, 7.0),
                                     Coordinate(1.0, 2.0, 5.0)),
                   7.0);
  EXPECT_DOUBLE_EQ(Interpolate::mGet(Coordinate(1.0, 2.0, 5.0),
                                     CoordinateXYZM(1.0, 2.0, 9.0, 13.0)),
                   13.0);
  EXPECT_DOUBLE_EQ(
      Interpolate::mGet(CoordinateXY(1.0, 2.0), CoordinateXYM(1.0, 2.0, 14.0)),
      14.0);
  EXPECT_TRUE(std::isnan(
      Interpolate::mGet(CoordinateXY(1.0, 2.0), Coordinate(1.0, 2.0, 5.0))));
}

TEST(InterpolateTest, ZGetOrInterpolateAndMGetOrInterpolate) {
  const Coordinate z_p1(0.0, 0.0, 10.0);
  const Coordinate z_p2(4.0, 0.0, 20.0);

  // Existing finite Z is returned verbatim without interpolating.
  EXPECT_DOUBLE_EQ(
      Interpolate::zGetOrInterpolate(Coordinate(1.0, 0.0, 99.0), z_p1, z_p2),
      99.0);
  EXPECT_DOUBLE_EQ(Interpolate::zGetOrInterpolate(
                       CoordinateXYZM(1.0, 0.0, 99.0, 4.0),
                       CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0)),
                   99.0);

  // Missing Z triggers interpolation from p1-p2.
  EXPECT_DOUBLE_EQ(
      Interpolate::zGetOrInterpolate(CoordinateXY(1.0, 0.0), z_p1, z_p2), 12.5);
  EXPECT_DOUBLE_EQ(
      Interpolate::zGetOrInterpolate(Coordinate(1.0, 0.0), z_p1, z_p2), 12.5);
  EXPECT_DOUBLE_EQ(
      Interpolate::zGetOrInterpolate(CoordinateXYM(1.0, 0.0, 55.0), z_p1, z_p2),
      12.5);

  // Missing Z everywhere returns NaN.
  EXPECT_TRUE(std::isnan(Interpolate::zGetOrInterpolate(
      CoordinateXY(1.0, 0.0), CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0))));

  const CoordinateXYM m_p1(0.0, 0.0, 100.0);
  const CoordinateXYM m_p2(4.0, 0.0, 140.0);

  // Existing finite M is returned verbatim without interpolating.
  EXPECT_DOUBLE_EQ(
      Interpolate::mGetOrInterpolate(CoordinateXYM(1.0, 0.0, 88.0), m_p1, m_p2),
      88.0);
  EXPECT_DOUBLE_EQ(Interpolate::mGetOrInterpolate(
                       CoordinateXYZM(1.0, 0.0, 5.0, 88.0),
                       Coordinate(0.0, 0.0, 1.0), Coordinate(4.0, 0.0, 2.0)),
                   88.0);

  // Missing M triggers interpolation from p1-p2.
  EXPECT_DOUBLE_EQ(
      Interpolate::mGetOrInterpolate(CoordinateXY(1.0, 0.0), m_p1, m_p2),
      110.0);
  EXPECT_DOUBLE_EQ(
      Interpolate::mGetOrInterpolate(Coordinate(1.0, 0.0, 5.0), m_p1, m_p2),
      110.0);
  EXPECT_DOUBLE_EQ(Interpolate::mGetOrInterpolate(
                       CoordinateXYZM(Coordinate(1.0, 0.0, 5.0)), m_p1, m_p2),
                   110.0);

  // Missing M everywhere returns NaN.
  EXPECT_TRUE(std::isnan(Interpolate::mGetOrInterpolate(
      CoordinateXY(1.0, 0.0), Coordinate(0.0, 0.0, 1.0),
      Coordinate(4.0, 0.0, 2.0))));
}

}  // namespace
}  // namespace geos_google
