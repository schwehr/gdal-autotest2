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
// Unit tests for libharp 3D vector geometry operations
// (libharp/harp-geometry-vector3d.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-geometry-vector3d.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-geometry.h

#include <cmath>

#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

struct VectorEqualCase {
  harp_vector3d a;
  harp_vector3d b;
  int expected;
};

constexpr VectorEqualCase kEqualCases[] = {
    {.a = {1.0, -2.0, 3.5}, .b = {1.0, -2.0, 3.5}, .expected = 1},
    // Differences below HARP_GEOMETRY_EPSILON (1e-10) compare equal.
    {.a = {1.0, 2.0, 3.0},
     .b = {1.0 + 0.5e-10, 2.0 - 0.5e-10, 3.0 + 0.5e-10},
     .expected = 1},
    // Differences above HARP_GEOMETRY_EPSILON on each coordinate compare
    // unequal.
    {.a = {1.0, 2.0, 3.0}, .b = {1.0 + 2.0e-10, 2.0, 3.0}, .expected = 0},
    {.a = {1.0, 2.0, 3.0}, .b = {1.0, 2.0 - 2.0e-10, 3.0}, .expected = 0},
    {.a = {1.0, 2.0, 3.0}, .b = {1.0, 2.0, 3.0 + 2.0e-10}, .expected = 0},
};

TEST(HarpGeometryVector3dTest, EqualRespectsEpsilonTolerancePerCoordinate) {
  for (const auto& tc : kEqualCases) {
    EXPECT_EQ(tc.expected, harp_vector3d_equal(&tc.a, &tc.b));
  }
}

struct DotProductCase {
  harp_vector3d a;
  harp_vector3d b;
  double expected;
};

constexpr DotProductCase kDotProductCases[] = {
    {.a = {0.0, 0.0, 0.0}, .b = {1.0, 2.0, 3.0}, .expected = 0.0},
    {.a = {1.0, 0.0, 0.0}, .b = {0.0, 1.0, 0.0}, .expected = 0.0},
    {.a = {1.0, 2.0, 3.0}, .b = {4.0, -5.0, 6.0}, .expected = 12.0},
    {.a = {-2.5, 3.0, -1.5}, .b = {2.0, -4.0, -2.0}, .expected = -14.0},
};

TEST(HarpGeometryVector3dTest, DotProductComputesScalarProduct) {
  for (const auto& tc : kDotProductCases) {
    EXPECT_DOUBLE_EQ(tc.expected, harp_vector3d_dotproduct(&tc.a, &tc.b));
  }
}

TEST(HarpGeometryVector3dTest, CrossProductBasisAndGeneralVectors) {
  constexpr harp_vector3d kX = {1.0, 0.0, 0.0};
  constexpr harp_vector3d kY = {0.0, 1.0, 0.0};
  constexpr harp_vector3d kZ = {0.0, 0.0, 1.0};
  harp_vector3d out{};

  harp_vector3d_crossproduct(&out, &kX, &kY);
  EXPECT_EQ(1, harp_vector3d_equal(&out, &kZ));

  harp_vector3d_crossproduct(&out, &kY, &kZ);
  EXPECT_EQ(1, harp_vector3d_equal(&out, &kX));

  harp_vector3d_crossproduct(&out, &kZ, &kX);
  EXPECT_EQ(1, harp_vector3d_equal(&out, &kY));

  // Anti-commutativity: y x x = -z.
  constexpr harp_vector3d kNegZ = {0.0, 0.0, -1.0};
  harp_vector3d_crossproduct(&out, &kY, &kX);
  EXPECT_EQ(1, harp_vector3d_equal(&out, &kNegZ));

  // Parallel vectors produce the zero vector.
  constexpr harp_vector3d kA = {2.0, -3.0, 4.0};
  constexpr harp_vector3d kTwoA = {4.0, -6.0, 8.0};
  constexpr harp_vector3d kZero = {0.0, 0.0, 0.0};
  harp_vector3d_crossproduct(&out, &kA, &kTwoA);
  EXPECT_EQ(1, harp_vector3d_equal(&out, &kZero));

  // General vectors: (1, 2, 3) x (4, 5, 6) = (-3, 6, -3), orthogonal to both.
  constexpr harp_vector3d kB = {4.0, 5.0, 6.0};
  constexpr harp_vector3d kC = {1.0, 2.0, 3.0};
  constexpr harp_vector3d kExpected = {-3.0, 6.0, -3.0};
  harp_vector3d_crossproduct(&out, &kC, &kB);
  EXPECT_EQ(1, harp_vector3d_equal(&out, &kExpected));
  EXPECT_DOUBLE_EQ(0.0, harp_vector3d_dotproduct(&out, &kC));
  EXPECT_DOUBLE_EQ(0.0, harp_vector3d_dotproduct(&out, &kB));
}

TEST(HarpGeometryVector3dTest, NormComputesEuclideanLength) {
  constexpr harp_vector3d kZero = {0.0, 0.0, 0.0};
  constexpr harp_vector3d kUnit = {0.0, -1.0, 0.0};
  constexpr harp_vector3d kPythagorean = {3.0, -4.0, 12.0};
  constexpr harp_vector3d kEqualAxes = {1.0, 1.0, 1.0};

  EXPECT_DOUBLE_EQ(0.0, harp_vector3d_norm(&kZero));
  EXPECT_DOUBLE_EQ(1.0, harp_vector3d_norm(&kUnit));
  EXPECT_DOUBLE_EQ(13.0, harp_vector3d_norm(&kPythagorean));
  EXPECT_NEAR(std::sqrt(3.0), harp_vector3d_norm(&kEqualAxes), 1e-15);
}

}  // namespace
}  // namespace stcorp_harp
