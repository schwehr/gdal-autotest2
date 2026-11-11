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

// Unit tests for third_party/geos/include/geos/operation/grid/Matrix.h.
//
// google3 notes:
// - Matrix<T>::release() is intentionally not exercised because m_data is a
//   std::shared_ptr<T[]>, which has no release() member in C++20.
// - MATRIX_CHECK_BOUNDS is not defined in Matrix_test.cc to preserve ODR
//   compatibility with //third_party/geos (GridIntersection/FloodFill).
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/operation/grid/Matrix.h

#include "third_party/geos/include/geos/operation/grid/Matrix.h"

#include <cstddef>
#include <iterator>
#include <memory>
#include <numeric>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::operation::grid::Matrix;

static_assert(std::is_same_v<Matrix<double>::value_type, double>);
static_assert(std::is_same_v<Matrix<int>::value_type, int>);
static_assert(std::is_move_constructible_v<Matrix<double>>);
static_assert(std::is_nothrow_move_constructible_v<Matrix<double>>);
static_assert(!std::is_copy_constructible_v<Matrix<double>>);
static_assert(!std::is_copy_assignable_v<Matrix<double>>);
static_assert(!std::is_move_assignable_v<Matrix<double>>);
static_assert(
    !std::is_convertible_v<std::vector<std::vector<int>>, Matrix<int>>);

TEST(MatrixTest, ZeroInitializedConstructorAllocatesAndClearsElements) {
  const Matrix<double> mat(3, 4);
  EXPECT_EQ(mat.getNumRows(), 3U);
  EXPECT_EQ(mat.getNumCols(), 4U);
  ASSERT_NE(mat.data(), nullptr);

  for (size_t i = 0; i < mat.getNumRows(); ++i) {
    for (size_t j = 0; j < mat.getNumCols(); ++j) {
      EXPECT_DOUBLE_EQ(mat(i, j), 0.0);
    }
  }
  EXPECT_EQ(std::distance(mat.begin(), mat.end()), 12);
}

TEST(MatrixTest, ZeroDimensionZeroInitializedMatricesHaveNullBuffer) {
  const Matrix<int> zero_both(0, 0);
  EXPECT_EQ(zero_both.getNumRows(), 0U);
  EXPECT_EQ(zero_both.getNumCols(), 0U);
  EXPECT_EQ(zero_both.data(), nullptr);
  EXPECT_EQ(zero_both.begin(), zero_both.end());

  const Matrix<int> zero_rows(0, 4);
  EXPECT_EQ(zero_rows.getNumRows(), 0U);
  EXPECT_EQ(zero_rows.getNumCols(), 4U);
  EXPECT_EQ(zero_rows.data(), nullptr);
  EXPECT_EQ(zero_rows.begin(), zero_rows.end());

  const Matrix<int> zero_cols(3, 0);
  EXPECT_EQ(zero_cols.getNumRows(), 3U);
  EXPECT_EQ(zero_cols.getNumCols(), 0U);
  EXPECT_EQ(zero_cols.data(), nullptr);
  EXPECT_EQ(zero_cols.begin(), zero_cols.end());
}

TEST(MatrixTest, FillValueConstructorPopulatesAllCells) {
  const Matrix<float> mat(2, 3, -2.5f);
  EXPECT_EQ(mat.getNumRows(), 2U);
  EXPECT_EQ(mat.getNumCols(), 3U);
  ASSERT_NE(mat.data(), nullptr);

  for (size_t i = 0; i < mat.getNumRows(); ++i) {
    for (size_t j = 0; j < mat.getNumCols(); ++j) {
      EXPECT_FLOAT_EQ(mat(i, j), -2.5f);
    }
  }

  const Matrix<int> zero_both(0, 0, 42);
  EXPECT_EQ(zero_both.getNumRows(), 0U);
  EXPECT_EQ(zero_both.getNumCols(), 0U);
  EXPECT_EQ(zero_both.data(), nullptr);
  EXPECT_EQ(zero_both.begin(), zero_both.end());

  const Matrix<int> zero_rows(0, 3, 42);
  EXPECT_EQ(zero_rows.getNumRows(), 0U);
  EXPECT_EQ(zero_rows.getNumCols(), 3U);
  EXPECT_EQ(zero_rows.data(), nullptr);
  EXPECT_EQ(zero_rows.begin(), zero_rows.end());

  const Matrix<int> zero_cols(3, 0, 42);
  EXPECT_EQ(zero_cols.getNumRows(), 3U);
  EXPECT_EQ(zero_cols.getNumCols(), 0U);
  EXPECT_EQ(zero_cols.data(), nullptr);
  EXPECT_EQ(zero_cols.begin(), zero_cols.end());
}

TEST(MatrixTest, SharedPointerConstructorSharesUnderlyingBuffer) {
  std::shared_ptr<int[]> buffer(new int[6]{10, 20, 30, 40, 50, 60});
  Matrix<int> mat(2, 3, buffer);

  EXPECT_EQ(mat.getNumRows(), 2U);
  EXPECT_EQ(mat.getNumCols(), 3U);
  EXPECT_EQ(mat.data(), buffer.get());
  EXPECT_EQ(buffer.use_count(), 2);

  EXPECT_EQ(mat(0, 0), 10);
  EXPECT_EQ(mat(0, 1), 20);
  EXPECT_EQ(mat(0, 2), 30);
  EXPECT_EQ(mat(1, 0), 40);
  EXPECT_EQ(mat(1, 1), 50);
  EXPECT_EQ(mat(1, 2), 60);

  mat(1, 1) = 99;
  EXPECT_EQ(buffer[4], 99);

  buffer[0] = -5;
  EXPECT_EQ(mat(0, 0), -5);

  const Matrix<int> empty_shared(0, 2, nullptr);
  EXPECT_EQ(empty_shared.getNumRows(), 0U);
  EXPECT_EQ(empty_shared.getNumCols(), 2U);
  EXPECT_EQ(empty_shared.data(), nullptr);
  EXPECT_EQ(empty_shared.begin(), empty_shared.end());
}

TEST(MatrixTest, NestedVectorConstructorCopiesRowMajorData) {
  const std::vector<std::vector<int>> input = {
      {1, 2, 3},
      {4, 5, 6},
  };
  Matrix<int> mat(input);

  EXPECT_EQ(mat.getNumRows(), 2U);
  EXPECT_EQ(mat.getNumCols(), 3U);
  EXPECT_EQ(mat(0, 0), 1);
  EXPECT_EQ(mat(0, 1), 2);
  EXPECT_EQ(mat(0, 2), 3);
  EXPECT_EQ(mat(1, 0), 4);
  EXPECT_EQ(mat(1, 1), 5);
  EXPECT_EQ(mat(1, 2), 6);

  // Storage is owned independently of the input vector.
  mat(0, 0) = 999;
  EXPECT_EQ(input[0][0], 1);

  const Matrix<double> single(std::vector<std::vector<double>>{{3.25}});
  EXPECT_EQ(single.getNumRows(), 1U);
  EXPECT_EQ(single.getNumCols(), 1U);
  EXPECT_DOUBLE_EQ(single(0, 0), 3.25);

  const Matrix<int> zero_cols(std::vector<std::vector<int>>{{}});
  EXPECT_EQ(zero_cols.getNumRows(), 1U);
  EXPECT_EQ(zero_cols.getNumCols(), 0U);
  EXPECT_NE(zero_cols.data(), nullptr);
  EXPECT_EQ(zero_cols.begin(), zero_cols.end());
}

TEST(MatrixTest, MoveConstructorTransfersBufferOwnership) {
  Matrix<int> src(std::vector<std::vector<int>>{
      {7, 8},
      {9, 10},
      {11, 12},
  });
  const int* original_ptr = src.data();
  ASSERT_NE(original_ptr, nullptr);

  const Matrix<int> dst(std::move(src));
  EXPECT_EQ(src.data(), nullptr);
  EXPECT_EQ(dst.getNumRows(), 3U);
  EXPECT_EQ(dst.getNumCols(), 2U);
  EXPECT_EQ(dst.data(), original_ptr);

  EXPECT_EQ(dst(0, 0), 7);
  EXPECT_EQ(dst(0, 1), 8);
  EXPECT_EQ(dst(1, 0), 9);
  EXPECT_EQ(dst(1, 1), 10);
  EXPECT_EQ(dst(2, 0), 11);
  EXPECT_EQ(dst(2, 1), 12);
}

TEST(MatrixTest, ElementRowAndDataAccessorsUseRowMajorLayout) {
  Matrix<double> mat(2, 3);
  const Matrix<double>& const_mat = mat;

  EXPECT_EQ(mat.data(), const_mat.data());

  mat(0, 0) = 1.0;
  mat(0, 2) = 3.0;
  mat(1, 1) = 5.0;
  mat.row(0)[1] = 2.0;
  mat.row(1)[0] = 4.0;
  mat.data()[5] = 6.0;

  EXPECT_EQ(mat.row(0), mat.data());
  EXPECT_EQ(mat.row(1), mat.data() + 3);
  EXPECT_EQ(&mat(1, 2), mat.data() + 5);
  EXPECT_EQ(&const_mat(1, 1), const_mat.data() + 4);

  const std::vector<double> expected = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  for (size_t idx = 0; idx < expected.size(); ++idx) {
    EXPECT_DOUBLE_EQ(const_mat.data()[idx], expected[idx]);
    EXPECT_DOUBLE_EQ(const_mat(idx / 3, idx % 3), expected[idx]);
  }
}

TEST(MatrixTest, IteratorsSpanAllElementsInRowMajorOrder) {
  const Matrix<int> mat(std::vector<std::vector<int>>{
      {1, 2, 3},
      {4, 5, 6},
  });

  EXPECT_EQ(mat.begin(), mat.data());
  EXPECT_EQ(mat.end(), mat.data() + 6);
  EXPECT_EQ(std::distance(mat.begin(), mat.end()), 6);
  EXPECT_EQ(std::accumulate(mat.begin(), mat.end(), 0), 21);

  std::vector<int> iterated;
  for (const int value : mat) {
    iterated.push_back(value);
  }
  EXPECT_EQ(iterated, (std::vector<int>{1, 2, 3, 4, 5, 6}));

  const Matrix<int> empty(0, 0);
  int empty_iterations = 0;
  for (const int value : empty) {
    (void)value;
    ++empty_iterations;
  }
  EXPECT_EQ(empty_iterations, 0);
}

TEST(MatrixTest, IncrementAccumulatesCellValues) {
  Matrix<double> mat(2, 2);
  mat.increment(0, 1, 2.5);
  mat.increment(0, 1, -0.75);
  mat.increment(1, 0, 0.0);
  mat.increment(1, 1, 4.25);

  EXPECT_DOUBLE_EQ(mat(0, 0), 0.0);
  EXPECT_DOUBLE_EQ(mat(0, 1), 1.75);
  EXPECT_DOUBLE_EQ(mat(1, 0), 0.0);
  EXPECT_DOUBLE_EQ(mat(1, 1), 4.25);
}

TEST(MatrixTest, EqualityAndInequalityOperatorsCheckDimensionsAndBytes) {
  const Matrix<int> base(2, 3, 5);
  Matrix<int> manual(2, 3);
  for (size_t i = 0; i < 2; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      manual(i, j) = 5;
    }
  }

  const Matrix<int>& base_ref = base;
  EXPECT_TRUE(base == base_ref);
  EXPECT_FALSE(base != base_ref);
  EXPECT_TRUE(base == manual);
  EXPECT_FALSE(base != manual);

  const Matrix<int> diff_rows(3, 3, 5);
  EXPECT_FALSE(base == diff_rows);
  EXPECT_TRUE(base != diff_rows);

  const Matrix<int> diff_cols(2, 2, 5);
  EXPECT_FALSE(base == diff_cols);
  EXPECT_TRUE(base != diff_cols);

  // Differ only in the last element to ensure all elements are compared.
  manual(1, 2) = 6;
  EXPECT_FALSE(base == manual);
  EXPECT_TRUE(base != manual);

  // Zero-column non-null matrices compare equal without passing nullptr to
  // memcmp.
  const Matrix<int> empty_cols_a(std::vector<std::vector<int>>{{}});
  const Matrix<int> empty_cols_b(std::vector<std::vector<int>>{{}});
  EXPECT_TRUE(empty_cols_a == empty_cols_b);
  EXPECT_FALSE(empty_cols_a != empty_cols_b);
}

TEST(MatrixTest, StreamInsertionFormatsZeroAndNonZeroCells) {
  const Matrix<double> double_mat(std::vector<std::vector<double>>{
      {0.0, 1.5},
      {-2.25, 0.0},
  });
  std::ostringstream double_oss;
  std::ostream& returned = (double_oss << double_mat);
  EXPECT_EQ(&returned, &double_oss);
  EXPECT_EQ(double_oss.str(),
            "             1.500000 \n"
            " -2.250000            \n");

  const Matrix<int> int_mat(std::vector<std::vector<int>>{
      {0, 42},
      {-7, 0},
  });
  std::ostringstream int_oss;
  int_oss << int_mat;
  EXPECT_EQ(int_oss.str(),
            "                   42 \n"
            "        -7            \n");

  const Matrix<double> empty_mat(0, 0);
  std::ostringstream empty_oss;
  empty_oss << empty_mat;
  EXPECT_EQ(empty_oss.str(), "");

  const Matrix<int> single_one(1, 1, 1);
  const Matrix<int> single_zero(1, 1, 0);
  std::ostringstream chained_oss;
  chained_oss << single_one << single_zero;
  EXPECT_EQ(chained_oss.str(),
            "         1 \n"
            "           \n");
}

TEST(MatrixTest, SupportsMoveOnlyElementTypes) {
  Matrix<std::unique_ptr<int>> mat(2, 2);
  EXPECT_EQ(mat(0, 0), nullptr);
  EXPECT_EQ(mat(0, 1), nullptr);
  EXPECT_EQ(mat(1, 0), nullptr);
  EXPECT_EQ(mat(1, 1), nullptr);

  mat(0, 1) = std::make_unique<int>(123);
  mat(1, 0) = std::make_unique<int>(-45);

  const Matrix<std::unique_ptr<int>> moved(std::move(mat));
  EXPECT_EQ(mat.data(), nullptr);
  ASSERT_NE(moved(0, 1), nullptr);
  ASSERT_NE(moved(1, 0), nullptr);
  EXPECT_EQ(*moved(0, 1), 123);
  EXPECT_EQ(*moved(1, 0), -45);
  EXPECT_EQ(moved(0, 0), nullptr);
  EXPECT_EQ(moved(1, 1), nullptr);
}

}  // namespace
}  // namespace geos_google
