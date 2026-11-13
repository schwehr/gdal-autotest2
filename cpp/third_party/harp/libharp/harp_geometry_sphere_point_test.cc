// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Unit tests for libharp spherical point operations
// (libharp/harp-geometry-sphere-point.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-geometry-sphere-point.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-geometry.h

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = std::numbers::pi / 2.0;

TEST(HarpGeometrySpherePointTest, EqualComparesInCartesianSpace) {
  constexpr harp_spherical_point kOrigin = {.lon = 0.0, .lat = 0.0};
  constexpr harp_spherical_point kOriginTwoPi = {.lon = 2.0 * kPi, .lat = 0.0};
  constexpr harp_spherical_point kNorthPoleA = {.lon = 0.0, .lat = kHalfPi};
  constexpr harp_spherical_point kNorthPoleB = {.lon = 1.75, .lat = kHalfPi};
  constexpr harp_spherical_point kOther = {.lon = 0.5, .lat = 0.25};

  EXPECT_EQ(1, harp_spherical_point_equal(&kOrigin, &kOrigin));
  EXPECT_EQ(1, harp_spherical_point_equal(&kOrigin, &kOriginTwoPi));
  // All longitudes at the North Pole represent the same 3D point.
  EXPECT_EQ(1, harp_spherical_point_equal(&kNorthPoleA, &kNorthPoleB));
  EXPECT_EQ(0, harp_spherical_point_equal(&kOrigin, &kOther));
}

struct PointCheckCase {
  harp_spherical_point input;
  double expected_lat;
  double expected_lon;
};

constexpr PointCheckCase kPointCheckCases[] = {
    // Negative longitude wraps into [0, 2*pi).
    {.input = {.lon = -kHalfPi, .lat = 0.25},
     .expected_lat = 0.25,
     .expected_lon = 1.5 * kPi},
    // Longitude >= 2*pi wraps into [0, 2*pi).
    {.input = {.lon = 2.5 * kPi, .lat = 0.25},
     .expected_lat = 0.25,
     .expected_lon = kHalfPi},
    // Latitude > pi wraps into [-pi, pi] and then flips across South Pole.
    {.input = {.lon = 0.5, .lat = 1.25 * kPi},
     .expected_lat = -0.25 * kPi,
     .expected_lon = 0.5 + kPi},
    // Latitude > pi/2 with lon < pi flips lat and shifts lon by +pi.
    {.input = {.lon = 0.25 * kPi, .lat = 0.75 * kPi},
     .expected_lat = 0.25 * kPi,
     .expected_lon = 1.25 * kPi},
    // Latitude > pi/2 with lon >= pi flips lat and shifts lon by -pi.
    {.input = {.lon = 1.5 * kPi, .lat = 0.75 * kPi},
     .expected_lat = 0.25 * kPi,
     .expected_lon = 0.5 * kPi},
    // Latitude < -pi/2 with lon < pi flips lat and shifts lon by +pi.
    {.input = {.lon = 0.25 * kPi, .lat = -0.75 * kPi},
     .expected_lat = -0.25 * kPi,
     .expected_lon = 1.25 * kPi},
    // Latitude < -pi/2 with lon >= pi flips lat and shifts lon by -pi.
    {.input = {.lon = 1.5 * kPi, .lat = -0.75 * kPi},
     .expected_lat = -0.25 * kPi,
     .expected_lon = 0.5 * kPi},
    // Negative input wrapping onto +pi/2 restores -pi/2 (South Pole).
    {.input = {.lon = 0.5, .lat = -1.5 * kPi},
     .expected_lat = -kHalfPi,
     .expected_lon = 0.5},
    // Longitude within HARP_GEOMETRY_EPSILON of 2*pi snaps to 0.0.
    {.input = {.lon = 2.0 * kPi - 0.5e-10, .lat = 0.2},
     .expected_lat = 0.2,
     .expected_lon = 0.0},
    // Near-zero lon and lat snap to exact 0.0.
    {.input = {.lon = 0.5e-10, .lat = -0.5e-10},
     .expected_lat = 0.0,
     .expected_lon = 0.0},
};

TEST(HarpGeometrySpherePointTest, CheckNormalizesOutOfRangeAndNearZeroAngles) {
  for (const auto& tc : kPointCheckCases) {
    harp_spherical_point point = tc.input;
    harp_spherical_point_check(&point);
    EXPECT_NEAR(tc.expected_lat, point.lat, 1e-12);
    EXPECT_NEAR(tc.expected_lon, point.lon, 1e-12);
  }
}

TEST(HarpGeometrySpherePointTest, Vector3dConversionsAndPoleBranches) {
  constexpr harp_spherical_point kEquatorPrime = {.lon = 0.0, .lat = 0.0};
  constexpr harp_spherical_point kEquatorEast = {.lon = kHalfPi, .lat = 0.0};
  constexpr harp_spherical_point kNorthPole = {.lon = 0.0, .lat = kHalfPi};
  constexpr harp_spherical_point kSouthPole = {.lon = 0.0, .lat = -kHalfPi};

  harp_vector3d v{};
  harp_vector3d_from_spherical_point(&v, &kEquatorPrime);
  EXPECT_NEAR(1.0, v.x, 1e-15);
  EXPECT_NEAR(0.0, v.y, 1e-15);
  EXPECT_NEAR(0.0, v.z, 1e-15);

  harp_vector3d_from_spherical_point(&v, &kEquatorEast);
  EXPECT_NEAR(0.0, v.x, 1e-15);
  EXPECT_NEAR(1.0, v.y, 1e-15);
  EXPECT_NEAR(0.0, v.z, 1e-15);

  harp_vector3d_from_spherical_point(&v, &kNorthPole);
  EXPECT_NEAR(0.0, v.x, 1e-15);
  EXPECT_NEAR(0.0, v.y, 1e-15);
  EXPECT_NEAR(1.0, v.z, 1e-15);

  harp_vector3d_from_spherical_point(&v, &kSouthPole);
  EXPECT_NEAR(0.0, v.x, 1e-15);
  EXPECT_NEAR(0.0, v.y, 1e-15);
  EXPECT_NEAR(-1.0, v.z, 1e-15);

  // Spherical point from vector3d: rho == 0 branches (z == 0, z > 0, z < 0,
  // NaN).
  harp_spherical_point p{};
  constexpr harp_vector3d kZeroVec = {0.0, 0.0, 0.0};
  harp_spherical_point_from_vector3d(&p, &kZeroVec);
  EXPECT_DOUBLE_EQ(0.0, p.lat);
  EXPECT_DOUBLE_EQ(0.0, p.lon);

  constexpr harp_vector3d kPosZ = {0.0, 0.0, 2.5};
  harp_spherical_point_from_vector3d(&p, &kPosZ);
  EXPECT_DOUBLE_EQ(kHalfPi, p.lat);

  constexpr harp_vector3d kNegZ = {0.0, 0.0, -2.5};
  harp_spherical_point_from_vector3d(&p, &kNegZ);
  EXPECT_DOUBLE_EQ(-kHalfPi, p.lat);

  const harp_vector3d nan_z = {0.0, 0.0, harp_nan()};
  harp_spherical_point_from_vector3d(&p, &nan_z);
  EXPECT_TRUE(std::isnan(p.lat));

  // General vector (rho > 0): (1, 1, sqrt(2)) -> lat = pi/4, lon = pi/4.
  const harp_vector3d diag = {1.0, 1.0, std::sqrt(2.0)};
  harp_spherical_point_from_vector3d(&p, &diag);
  EXPECT_NEAR(0.25 * kPi, p.lat, 1e-15);
  EXPECT_NEAR(0.25 * kPi, p.lon, 1e-15);
}

TEST(HarpGeometrySpherePointTest, RadDegRoundTripAndSurfaceDistances) {
  harp_spherical_point p = {.lon = -90.0, .lat = 45.0};
  harp_spherical_point_rad_from_deg(&p);
  EXPECT_NEAR(0.25 * kPi, p.lat, 1e-12);
  EXPECT_NEAR(-kHalfPi, p.lon, 1e-12);

  harp_spherical_point_deg_from_rad(&p);
  EXPECT_NEAR(45.0, p.lat, 1e-12);
  EXPECT_NEAR(-90.0, p.lon, 1e-12);

  // Unit-sphere distances: identical, near-identical (FPzero), orthogonal,
  // and antipodal.
  constexpr harp_spherical_point kOrigin = {.lon = 0.0, .lat = 0.0};
  constexpr harp_spherical_point kNearOrigin = {.lon = 1e-12, .lat = 0.0};
  constexpr harp_spherical_point kQuarter = {.lon = kHalfPi, .lat = 0.0};
  constexpr harp_spherical_point kAntipode = {.lon = kPi, .lat = 0.0};

  EXPECT_DOUBLE_EQ(0.0, harp_spherical_point_distance(&kOrigin, &kOrigin));
  EXPECT_DOUBLE_EQ(0.0, harp_spherical_point_distance(&kOrigin, &kNearOrigin));
  EXPECT_NEAR(kHalfPi, harp_spherical_point_distance(&kOrigin, &kQuarter),
              1e-15);
  EXPECT_NEAR(kPi, harp_spherical_point_distance(&kOrigin, &kAntipode), 1e-15);

  // Public API distance in meters on WGS84 sphere.
  double distance = -1.0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_geometry_get_point_distance(0.0, 0.0, 0.0, 0.0, &distance));
  EXPECT_DOUBLE_EQ(0.0, distance);

  ASSERT_EQ(HARP_SUCCESS,
            harp_geometry_get_point_distance(0.0, 0.0, 0.0, 90.0, &distance));
  EXPECT_NEAR(kHalfPi * CONST_EARTH_RADIUS_WGS84_SPHERE, distance, 1e-6);

  ASSERT_EQ(HARP_SUCCESS,
            harp_geometry_get_point_distance(-90.0, 0.0, 90.0, 0.0, &distance));
  EXPECT_NEAR(kPi * CONST_EARTH_RADIUS_WGS84_SPHERE, distance, 1e-6);
}

}  // namespace
}  // namespace stcorp_harp
