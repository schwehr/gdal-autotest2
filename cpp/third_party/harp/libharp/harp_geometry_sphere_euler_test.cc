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
// Unit tests for libharp spherical Euler transformation operations
// (libharp/harp-geometry-sphere-euler.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-geometry-sphere-euler.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-geometry.h

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = std::numbers::pi / 2.0;

TEST(HarpGeometrySphereEulerTest, SetToZxzAndEqualChecksRotationEquivalence) {
  harp_euler_transformation t1{};
  harp_euler_transformation_set_to_zxz(&t1);
  EXPECT_EQ('Z', t1.phi_axis);
  EXPECT_EQ('X', t1.theta_axis);
  EXPECT_EQ('Z', t1.psi_axis);
  t1.phi = 0.0;
  t1.theta = 0.0;
  t1.psi = 0.0;

  // Equivalent identity rotation using phi + psi == 0 with theta == 0.
  harp_euler_transformation t2{};
  harp_euler_transformation_set_to_zxz(&t2);
  t2.phi = 0.7;
  t2.theta = 0.0;
  t2.psi = -0.7;
  EXPECT_EQ(1, harp_euler_transformation_equal(&t1, &t2));

  // Rotation around X keeps (lat=0, lon=0) fixed while moving (lat=0,
  // lon=pi/2), exercising the second point comparison in
  // harp_euler_transformation_equal.
  harp_euler_transformation rx{};
  rx.phi_axis = 'X';
  rx.theta_axis = 'Z';
  rx.psi_axis = 'Z';
  rx.phi = kHalfPi;
  rx.theta = 0.0;
  rx.psi = 0.0;
  EXPECT_EQ(0, harp_euler_transformation_equal(&t1, &rx));

  // Different rotation moving (0, 0) fails on the first point comparison.
  t2.theta = 0.5;
  EXPECT_EQ(0, harp_euler_transformation_equal(&t1, &t2));
}

TEST(HarpGeometrySphereEulerTest, ApplyEulerRotationsForAllAxesAndInvert) {
  constexpr harp_spherical_point kOrigin = {.lon = 0.0, .lat = 0.0};
  harp_spherical_point out{};

  // Rotation around Z by +pi/2 maps (0, 0) -> (0, pi/2).
  harp_euler_transformation rz{};
  rz.phi_axis = 'Z';
  rz.theta_axis = 'X';
  rz.psi_axis = 'Z';
  rz.phi = kHalfPi;
  rz.theta = 0.0;
  rz.psi = 0.0;
  harp_spherical_point_apply_euler_transformation(&out, &kOrigin, &rz);
  EXPECT_NEAR(0.0, out.lat, 1e-12);
  EXPECT_NEAR(kHalfPi, out.lon, 1e-12);

  // Rotation around Y by -pi/2 maps (0, 0) = (1, 0, 0) -> (0, 0, 1) = North
  // Pole.
  harp_euler_transformation ry{};
  ry.phi_axis = 'X';
  ry.theta_axis = 'Y';
  ry.psi_axis = 'Z';
  ry.phi = 0.0;
  ry.theta = -kHalfPi;
  ry.psi = 0.0;
  harp_spherical_point_apply_euler_transformation(&out, &kOrigin, &ry);
  EXPECT_NEAR(kHalfPi, out.lat, 1e-12);

  // Rotation around X by +pi/2 maps (0, pi/2) = (0, 1, 0) -> North Pole.
  constexpr harp_spherical_point kEast = {.lon = kHalfPi, .lat = 0.0};
  harp_euler_transformation rx{};
  rx.phi_axis = 'X';
  rx.theta_axis = 'Y';
  rx.psi_axis = 'Z';
  rx.phi = kHalfPi;
  rx.theta = 0.0;
  rx.psi = 0.0;
  harp_spherical_point_apply_euler_transformation(&out, &kEast, &rx);
  EXPECT_NEAR(kHalfPi, out.lat, 1e-12);

  // General XYZ transformation and its inverse restore the input point.
  harp_euler_transformation xyz{};
  xyz.phi_axis = 'X';
  xyz.theta_axis = 'Y';
  xyz.psi_axis = 'Z';
  xyz.phi = 0.3;
  xyz.theta = -0.4;
  xyz.psi = 0.8;

  harp_euler_transformation inv = xyz;
  harp_euler_transformation_invert(&inv);
  EXPECT_EQ('Z', inv.phi_axis);
  EXPECT_EQ('Y', inv.theta_axis);
  EXPECT_EQ('X', inv.psi_axis);

  constexpr harp_spherical_point kSample = {.lon = 1.1, .lat = 0.35};
  harp_spherical_point rotated{};
  harp_spherical_point restored{};
  harp_spherical_point_apply_euler_transformation(&rotated, &kSample, &xyz);
  harp_spherical_point_apply_euler_transformation(&restored, &rotated, &inv);
  EXPECT_EQ(1, harp_spherical_point_equal(&kSample, &restored));
}

#if !defined(MEMORY_SANITIZER)
TEST(HarpGeometrySphereEulerTest, InvalidEulerAxisSetsError) {
  // harp_spherical_point_apply_euler_transformation ignores the -1 return of
  // vector3d_apply_euler_transformation and passes an uninitialized vectorout
  // to harp_spherical_point_from_vector3d, so guard this negative test under
  // MSan.
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_euler_transformation invalid{};
  invalid.phi_axis = 'Q';
  invalid.theta_axis = 'X';
  invalid.psi_axis = 'Z';
  invalid.phi = 0.5;
  invalid.theta = 0.0;
  invalid.psi = 0.0;

  constexpr harp_spherical_point kOrigin = {.lon = 0.0, .lat = 0.0};
  harp_spherical_point out{};
  harp_spherical_point_apply_euler_transformation(&out, &kOrigin, &invalid);
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
}
#endif

TEST(HarpGeometrySphereEulerTest,
     TransformToZxzAndFromSphericalVectorComposeAndAlignPoints) {
  // Compose two non-ZXZ transformations into an equivalent ZXZ transformation.
  harp_euler_transformation tin{};
  tin.phi_axis = 'X';
  tin.theta_axis = 'Y';
  tin.psi_axis = 'Z';
  tin.phi = 0.25;
  tin.theta = -0.35;
  tin.psi = 0.5;

  harp_euler_transformation tnext{};
  tnext.phi_axis = 'Y';
  tnext.theta_axis = 'Z';
  tnext.psi_axis = 'X';
  tnext.phi = -0.2;
  tnext.theta = 0.4;
  tnext.psi = -0.15;

  harp_euler_transformation composed{};
  harp_euler_transformation_transform_to_zxz_euler_transformation(&composed,
                                                                  &tin, &tnext);
  EXPECT_EQ('Z', composed.phi_axis);
  EXPECT_EQ('X', composed.theta_axis);
  EXPECT_EQ('Z', composed.psi_axis);

  constexpr harp_spherical_point kProbe = {.lon = 0.9, .lat = -0.3};
  harp_spherical_point step1{};
  harp_spherical_point step2{};
  harp_spherical_point direct{};
  harp_spherical_point_apply_euler_transformation(&step1, &kProbe, &tin);
  harp_spherical_point_apply_euler_transformation(&step2, &step1, &tnext);
  harp_spherical_point_apply_euler_transformation(&direct, &kProbe, &composed);
  EXPECT_EQ(1, harp_spherical_point_equal(&step2, &direct));

  // Identical begin/end spherical vector yields identity transformation.
  constexpr harp_spherical_point kBegin = {.lon = 0.6, .lat = 0.4};
  harp_euler_transformation vec_trans{};
  harp_euler_transformation_set_to_zxz(&vec_trans);
  harp_euler_transformation_from_spherical_vector(&vec_trans, &kBegin, &kBegin);
  EXPECT_DOUBLE_EQ(0.0, vec_trans.phi);
  EXPECT_DOUBLE_EQ(0.0, vec_trans.theta);
  EXPECT_DOUBLE_EQ(0.0, vec_trans.psi);

  // Distinct begin/end spherical vector produces ZXZ transformation mapping
  // (0, 0) -> begin and (0, dist) -> end.
  constexpr harp_spherical_point kEnd = {.lon = 1.2, .lat = -0.2};
  harp_euler_transformation_from_spherical_vector(&vec_trans, &kBegin, &kEnd);
  EXPECT_EQ('Z', vec_trans.phi_axis);
  EXPECT_EQ('X', vec_trans.theta_axis);
  EXPECT_EQ('Z', vec_trans.psi_axis);

  constexpr harp_spherical_point kOrigin = {.lon = 0.0, .lat = 0.0};
  const harp_spherical_point kArcEnd = {
      .lon = harp_spherical_point_distance(&kBegin, &kEnd), .lat = 0.0};
  harp_spherical_point mapped_begin{};
  harp_spherical_point mapped_end{};
  harp_spherical_point_apply_euler_transformation(&mapped_begin, &kOrigin,
                                                  &vec_trans);
  harp_spherical_point_apply_euler_transformation(&mapped_end, &kArcEnd,
                                                  &vec_trans);
  EXPECT_EQ(1, harp_spherical_point_equal(&kBegin, &mapped_begin));
  EXPECT_EQ(1, harp_spherical_point_equal(&kEnd, &mapped_end));
}

}  // namespace
}  // namespace stcorp_harp
