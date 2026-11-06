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
// Tests GDALLinearSystemSolve and GDALMatrix (alg/gdallinearsystem.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdallinearsystem.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/gdallinearsystem.h

#include "third_party/gdal/alg/gdallinearsystem.h"

#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(GdalLinearSystemTest, MatrixBasicOperations) {
  GDALMatrix mat_default;
  EXPECT_EQ(0, mat_default.getNumRows());
  EXPECT_EQ(0, mat_default.getNumCols());

  GDALMatrix mat(2, 3);
  EXPECT_EQ(2, mat.getNumRows());
  EXPECT_EQ(3, mat.getNumCols());
  EXPECT_NE(nullptr, mat.data());

  const GDALMatrix& cmat = mat;
  EXPECT_NE(nullptr, cmat.data());

  mat(0, 0) = 1.0;
  mat(1, 0) = 2.0;
  mat(0, 1) = 3.0;
  mat(1, 1) = 4.0;
  mat(0, 2) = 5.0;
  mat(1, 2) = 6.0;

  EXPECT_DOUBLE_EQ(1.0, cmat(0, 0));
  EXPECT_DOUBLE_EQ(2.0, cmat(1, 0));
  EXPECT_DOUBLE_EQ(3.0, cmat(0, 1));
  EXPECT_DOUBLE_EQ(4.0, cmat(1, 1));
  EXPECT_DOUBLE_EQ(5.0, cmat(0, 2));
  EXPECT_DOUBLE_EQ(6.0, cmat(1, 2));

  mat.resize(1, 2);
  EXPECT_EQ(1, mat.getNumRows());
  EXPECT_EQ(2, mat.getNumCols());
  EXPECT_DOUBLE_EQ(0.0, mat(0, 0));
  EXPECT_DOUBLE_EQ(0.0, mat(0, 1));
}

TEST(GdalLinearSystemTest, Solve1x1System) {
  GDALMatrix a(1, 1);
  GDALMatrix rhs(1, 1);
  GDALMatrix x(1, 1);

  a(0, 0) = 4.0;
  rhs(0, 0) = 12.0;

  EXPECT_TRUE(GDALLinearSystemSolve(a, rhs, x));
  EXPECT_DOUBLE_EQ(3.0, x(0, 0));
}

TEST(GdalLinearSystemTest, Solve2x2SystemWithRowPivoting) {
  // Matrix A:
  // [ 0  2 ]
  // [ 3  1 ]
  // Needs row swap since A(0,0) == 0.
  // RHS:
  // [ 4 ]
  // [ 5 ]
  // Solution:
  // 2*x1 = 4 => x1 = 2
  // 3*x0 + x1 = 5 => 3*x0 + 2 = 5 => x0 = 1
  GDALMatrix a(2, 2);
  GDALMatrix rhs(2, 1);
  GDALMatrix x(2, 1);

  a(0, 0) = 0.0;
  a(0, 1) = 2.0;
  a(1, 0) = 3.0;
  a(1, 1) = 1.0;

  rhs(0, 0) = 4.0;
  rhs(1, 0) = 5.0;

  EXPECT_TRUE(GDALLinearSystemSolve(a, rhs, x));
  EXPECT_DOUBLE_EQ(1.0, x(0, 0));
  EXPECT_DOUBLE_EQ(2.0, x(1, 0));
}

TEST(GdalLinearSystemTest, Solve3x3MultiColumnRHS) {
  // A:
  // [ 1  2  3 ]
  // [ 0  1  4 ]
  // [ 5  6  0 ]
  // Column 1 RHS: A * [1, 1, 1]^T = [6, 5, 11]^T
  // Column 2 RHS: A * [2, 0, 1]^T = [5, 4, 10]^T
  GDALMatrix a(3, 3);
  a(0, 0) = 1.0;
  a(0, 1) = 2.0;
  a(0, 2) = 3.0;
  a(1, 0) = 0.0;
  a(1, 1) = 1.0;
  a(1, 2) = 4.0;
  a(2, 0) = 5.0;
  a(2, 1) = 6.0;
  a(2, 2) = 0.0;

  GDALMatrix rhs(3, 2);
  rhs(0, 0) = 6.0;
  rhs(0, 1) = 5.0;
  rhs(1, 0) = 5.0;
  rhs(1, 1) = 4.0;
  rhs(2, 0) = 11.0;
  rhs(2, 1) = 10.0;

  GDALMatrix x(3, 2);
  EXPECT_TRUE(GDALLinearSystemSolve(a, rhs, x));

  EXPECT_NEAR(1.0, x(0, 0), 1e-12);
  EXPECT_NEAR(1.0, x(1, 0), 1e-12);
  EXPECT_NEAR(1.0, x(2, 0), 1e-12);

  EXPECT_NEAR(2.0, x(0, 1), 1e-12);
  EXPECT_NEAR(0.0, x(1, 1), 1e-12);
  EXPECT_NEAR(1.0, x(2, 1), 1e-12);
}

TEST(GdalLinearSystemTest, SingularMatrixFails) {
  // Singular matrix with det == 0:
  // Matrix with a zero column (not invertible, pivot is 0):
  // [ 0  1 ]
  // [ 0  2 ]
  GDALMatrix a(2, 2);
  a(0, 0) = 0.0;
  a(0, 1) = 1.0;
  a(1, 0) = 0.0;
  a(1, 1) = 2.0;

  GDALMatrix rhs(2, 1);
  rhs(0, 0) = 3.0;
  rhs(1, 0) = 6.0;

  GDALMatrix x(2, 1);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(GDALLinearSystemSolve(a, rhs, x));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("matrix not invertible"));
  CPLErrorReset();
}

TEST(GdalLinearSystemTest, NegativeDimensionCatchesExceptionAndReturnsFalse) {
  // (-1)*(-1) == 1 and (-1)*0 == 0 construct valid GDALMatrix vectors and
  // satisfy the dimension-equality asserts, while std::vector<int> perm(m)
  // with m == -1 throws std::length_error inside solve(), exercising the
  // catch (const std::exception&) error path in GDALLinearSystemSolve.
  GDALMatrix a(-1, -1);
  GDALMatrix rhs(-1, 0);
  GDALMatrix x(-1, 0);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(GDALLinearSystemSolve(a, rhs, x));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("GDALLinearSystemSolve:"));
  CPLErrorReset();
}

// TODO: b/335317901 - Line 42 (eps < 0) in solve() is unreachable via
// GDALLinearSystemSolve (solve is called with eps = 0).

}  // namespace
}  // namespace autotest2
