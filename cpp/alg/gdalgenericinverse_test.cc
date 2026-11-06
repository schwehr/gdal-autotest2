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
// Tests generic inverse 2D coordinate transformation
// (alg/gdalgenericinverse.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalgenericinverse.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalgenericinverse.h

#include "third_party/gdal/alg/gdalgenericinverse.h"

#include <cmath>

#include "testing/base/public/gunit.h"

namespace autotest2 {
namespace {

// Forward linear transform:
//   xOut = 2.0 * xIn + 3.0 * yIn
//   yOut = 4.0 * xIn - yIn
bool LinearForward(double xIn, double yIn, double& xOut, double& yOut,
                   void* /* pUserData */) {
  xOut = 2.0 * xIn + 3.0 * yIn;
  yOut = 4.0 * xIn - yIn;
  return true;
}

// Forward transform that fails on demand based on a counter in pUserData.
struct FailControl {
  int call_count = 0;
  int fail_at_call = -1;
};

bool FailingForward(double xIn, double yIn, double& xOut, double& yOut,
                    void* pUserData) {
  auto* ctrl = static_cast<FailControl*>(pUserData);
  ctrl->call_count++;
  if (ctrl->call_count == ctrl->fail_at_call) {
    return false;
  }
  xOut = xIn;
  yOut = yIn;
  return true;
}

// Forward transform that produces zero derivatives (singular Jacobian det ==
// 0).
bool ZeroDerivativeForward(double /* xIn */, double /* yIn */, double& xOut,
                           double& yOut, void* /* pUserData */) {
  xOut = 42.0;
  yOut = 42.0;
  return true;
}

// Forward transform designed not to converge within 15 iterations.
bool NonConvergingForward(double xIn, double yIn, double& xOut, double& yOut,
                          void* /* pUserData */) {
  // Push the output far away and non-linearly so Newton-Raphson
  // oscillates/diverges
  xOut = std::sin(xIn * 100.0) * 1000.0;
  yOut = std::cos(yIn * 100.0) * 1000.0;
  return true;
}

TEST(GdalGenericInverseTest, LinearTransformConvergenceDefaultTolerance) {
  // Invert (13.0, 5.0) which corresponds to (2.0, 3.0) since:
  //   2*(2) + 3*(3) = 13
  //   4*(2) - (3) = 5
  double x_out = 0.0;
  double y_out = 0.0;
  const bool success = GDALGenericInverse2D(
      13.0, 5.0, /*guessedXOut=*/0.0, /*guessedYOut=*/0.0, LinearForward,
      /*pfnForwardTranformerUserData=*/nullptr, x_out, y_out,
      /*computeJacobianMatrixOnlyAtFirstIter=*/false,
      /*toleranceOnInputCoordinates=*/0.0,
      /*toleranceOnOutputCoordinates=*/0.0);
  EXPECT_TRUE(success);
  EXPECT_NEAR(2.0, x_out, 1e-8);
  EXPECT_NEAR(3.0, y_out, 1e-8);
}

TEST(GdalGenericInverseTest, JacobianOnlyAtFirstIterAndExplicitInputTolerance) {
  double x_out = 0.0;
  double y_out = 0.0;
  const bool success = GDALGenericInverse2D(
      13.0, 5.0, /*guessedXOut=*/1.0, /*guessedYOut=*/1.0, LinearForward,
      /*pfnForwardTranformerUserData=*/nullptr, x_out, y_out,
      /*computeJacobianMatrixOnlyAtFirstIter=*/true,
      /*toleranceOnInputCoordinates=*/1e-6,
      /*toleranceOnOutputCoordinates=*/0.0);
  EXPECT_TRUE(success);
  EXPECT_NEAR(2.0, x_out, 1e-5);
  EXPECT_NEAR(3.0, y_out, 1e-5);
}

TEST(GdalGenericInverseTest, OutputToleranceTrigger) {
  double x_out = 0.0;
  double y_out = 0.0;
  const bool success = GDALGenericInverse2D(
      13.0, 5.0, /*guessedXOut=*/1.999, /*guessedYOut=*/2.999, LinearForward,
      /*pfnForwardTranformerUserData=*/nullptr, x_out, y_out,
      /*computeJacobianMatrixOnlyAtFirstIter=*/false,
      /*toleranceOnInputCoordinates=*/1e-12,
      /*toleranceOnOutputCoordinates=*/0.1);
  EXPECT_TRUE(success);
  EXPECT_NEAR(2.0, x_out, 0.1);
  EXPECT_NEAR(3.0, y_out, 0.1);
}

TEST(GdalGenericInverseTest, ForwardTransformerFailsFirstCall) {
  FailControl ctrl;
  ctrl.fail_at_call = 1;
  double x_out = 0.0;
  double y_out = 0.0;
  EXPECT_FALSE(GDALGenericInverse2D(
      1.0, 1.0, 0.0, 0.0, FailingForward, &ctrl, x_out, y_out,
      /*computeJacobianMatrixOnlyAtFirstIter=*/false, 0.0, 0.0));
}

TEST(GdalGenericInverseTest, ForwardTransformerFailsJacobianXStep) {
  FailControl ctrl;
  ctrl.fail_at_call = 2;  // Call 1 succeeds (point evaluation), Call 2 fails
  double x_out = 0.0;
  double y_out = 0.0;
  EXPECT_FALSE(GDALGenericInverse2D(
      1.0, 1.0, 0.0, 0.0, FailingForward, &ctrl, x_out, y_out,
      /*computeJacobianMatrixOnlyAtFirstIter=*/false, 0.0, 0.0));
}

TEST(GdalGenericInverseTest, ForwardTransformerFailsJacobianYStep) {
  FailControl ctrl;
  ctrl.fail_at_call = 3;  // Call 1 & 2 succeed, Call 3 fails
  double x_out = 0.0;
  double y_out = 0.0;
  EXPECT_FALSE(GDALGenericInverse2D(
      1.0, 1.0, 0.0, 0.0, FailingForward, &ctrl, x_out, y_out,
      /*computeJacobianMatrixOnlyAtFirstIter=*/false, 0.0, 0.0));
}

TEST(GdalGenericInverseTest, SingularJacobianDetZero) {
  double x_out = 0.0;
  double y_out = 0.0;
  EXPECT_FALSE(GDALGenericInverse2D(
      1.0, 1.0, 0.0, 0.0, ZeroDerivativeForward, nullptr, x_out, y_out,
      /*computeJacobianMatrixOnlyAtFirstIter=*/false, 0.0, 0.0));
}

TEST(GdalGenericInverseTest, ExceedsMaxIterationsWithoutConvergence) {
  double x_out = 0.0;
  double y_out = 0.0;
  EXPECT_FALSE(GDALGenericInverse2D(
      500.0, 500.0, 0.0, 0.0, NonConvergingForward, nullptr, x_out, y_out,
      /*computeJacobianMatrixOnlyAtFirstIter=*/false, 1e-12, 0.0));
}

}  // namespace
}  // namespace autotest2
