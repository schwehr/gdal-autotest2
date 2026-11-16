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
// Tests internal LUP matrix inversion routines (src/lib/openjp2/invert.c).
//
// Caveats:
// - kTolerance (1e-5f) covers rounding differences between x86 SSE2 and
//   --cpu=arm scalar/FMA paths with >=10x headroom on small matrices.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/invert.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/invert.h

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"

// Include internal OpenJPEG headers after C++ stdlib and GoogleTest to avoid
// macro/poison collisions.
#include "third_party/openjpeg2/google/util/opj_internal.h"

namespace openjpeg2 {
namespace {

constexpr float kTolerance = 1e-5f;
constexpr float kOutputSentinel = -999.0f;

struct MatrixCase {
  std::string_view name;
  uint32_t n;
  std::span<const float> matrix;
  std::span<const float> expected_inverse;
};

struct SingularCase {
  std::string_view name;
  uint32_t n;
  std::span<const float> matrix;
};

// 1x1 invertible matrices.
constexpr auto kMat1x1One = std::to_array<float>({1.0f});
constexpr auto kMat1x1Four = std::to_array<float>({4.0f});
constexpr auto kMat1x1FourInv = std::to_array<float>({0.25f});
constexpr auto kMat1x1Neg = std::to_array<float>({-2.5f});
constexpr auto kMat1x1NegInv = std::to_array<float>({-0.4f});
constexpr auto kMat1x1Eighth = std::to_array<float>({0.125f});
constexpr auto kMat1x1EighthInv = std::to_array<float>({8.0f});

// Matrices requiring no row-pivot swaps (k2 == k at every elimination step).
constexpr auto kMat2x2NoSwap = std::to_array<float>({
    4.0f,
    1.0f,
    2.0f,
    3.0f,
});
constexpr auto kMat2x2NoSwapInv = std::to_array<float>({
    0.3f,
    -0.1f,
    -0.2f,
    0.4f,
});

constexpr auto kMat2x2NoSwapNegPivot = std::to_array<float>({
    -4.0f,
    2.0f,
    1.0f,
    -3.0f,
});
constexpr auto kMat2x2NoSwapNegPivotInv = std::to_array<float>({
    -0.3f,
    -0.2f,
    -0.1f,
    -0.4f,
});

constexpr auto kMat3x3NoSwap = std::to_array<float>({
    4.0f,
    0.0f,
    2.0f,
    2.0f,
    4.0f,
    1.0f,
    -1.0f,
    1.0f,
    2.0f,
});
constexpr auto kMat3x3NoSwapInv = std::to_array<float>({
    0.175f,
    0.05f,
    -0.2f,
    -0.125f,
    0.25f,
    0.0f,
    0.15f,
    -0.1f,
    0.4f,
});

// Strictly diagonally dominant 4x4 matrix with negative diagonal pivots at
// k = 0 and k = 2 so k2 == k at every step.
constexpr auto kMat4x4NoSwap = std::to_array<float>({
    -4.0f,
    1.0f,
    0.0f,
    1.0f,
    1.0f,
    4.0f,
    -1.0f,
    0.0f,
    0.0f,
    -1.0f,
    -4.0f,
    1.0f,
    1.0f,
    0.0f,
    1.0f,
    4.0f,
});

// Matrices requiring row-pivot swaps (k2 != k) including negative pivots.
constexpr auto kMat2x2SwapZeroDiag = std::to_array<float>({
    0.0f,
    2.0f,
    -4.0f,
    1.0f,
});
constexpr auto kMat2x2SwapZeroDiagInv = std::to_array<float>({
    0.125f,
    -0.25f,
    0.5f,
    0.0f,
});

constexpr auto kMat2x2SwapNegPivot = std::to_array<float>({
    1.0f,
    2.0f,
    -4.0f,
    4.0f,
});
constexpr auto kMat2x2SwapNegPivotInv = std::to_array<float>({
    1.0f / 3.0f,
    -1.0f / 6.0f,
    1.0f / 3.0f,
    1.0f / 12.0f,
});

// Swaps row 0<->1 at k = 0 (pivot -4) and row 1<->2 at k = 1 (pivot 5.5).
constexpr auto kMat3x3SwapMultiStep = std::to_array<float>({
    1.0f,
    -2.0f,
    3.0f,
    -4.0f,
    1.0f,
    2.0f,
    2.0f,
    5.0f,
    -1.0f,
});

// JPEG 2000 Part 1 irreversible ICT color transform matrix (swaps at k = 0, 1).
constexpr auto kMat3x3Jpeg2kIct = std::to_array<float>({
    0.299f,
    0.587f,
    0.114f,
    -0.16875f,
    -0.33126f,
    0.5f,
    0.5f,
    -0.41869f,
    -0.08131f,
});

// Signed cyclic permutation matrix: swaps at every step k = 0, 1, 2.
constexpr auto kMat4x4SignedCyclicPerm = std::to_array<float>({
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    -1.0f,
    0.0f,
    0.0f,
    0.0f,
});
constexpr auto kMat4x4SignedCyclicPermInv = std::to_array<float>({
    0.0f,
    0.0f,
    0.0f,
    -1.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
});

// Dense 4x4 matrix requiring row swaps at k = 0 (pivot -4), k = 1 (pivot -5.5),
// and k = 2.
constexpr auto kMat4x4SwapDense = std::to_array<float>({
    1.0f,
    2.0f,
    -1.0f,
    0.0f,
    0.0f,
    1.0f,
    2.0f,
    -1.0f,
    -4.0f,
    1.0f,
    0.0f,
    2.0f,
    2.0f,
    -5.0f,
    1.0f,
    3.0f,
});

// Dense 5x5 matrix exercising larger stride updates with swaps and negative
// pivots.
constexpr auto kMat5x5SwapDense = std::to_array<float>({
    0.0f, 2.0f, 0.0f, 1.0f, 0.0f,  -3.0f, 1.0f, 0.0f,  0.0f,
    1.0f, 1.0f, 0.0f, 4.0f, -1.0f, 0.0f,  0.0f, -5.0f, 1.0f,
    2.0f, 0.0f, 1.0f, 1.0f, 0.0f,  -2.0f, 3.0f,
});

// Singular matrices triggering p == 0.0 at some k < nb_compo - 1.
constexpr auto kSing2x2AllZero = std::to_array<float>({
    0.0f,
    0.0f,
    0.0f,
    0.0f,
});
constexpr auto kSing2x2ZeroCol0 = std::to_array<float>({
    0.0f,
    1.0f,
    0.0f,
    2.0f,
});
constexpr auto kSing2x2NegZeroCol0 = std::to_array<float>({
    -0.0f,
    1.0f,
    -0.0f,
    2.0f,
});
constexpr auto kSing3x3ZeroCol0 = std::to_array<float>({
    0.0f,
    1.0f,
    2.0f,
    0.0f,
    3.0f,
    4.0f,
    0.0f,
    5.0f,
    6.0f,
});
constexpr auto kSing3x3ZeroCol1 = std::to_array<float>({
    2.0f,
    0.0f,
    1.0f,
    1.0f,
    0.0f,
    3.0f,
    -1.0f,
    0.0f,
    4.0f,
});
constexpr auto kSing3x3DependentCols = std::to_array<float>({
    2.0f,
    4.0f,
    1.0f,
    1.0f,
    2.0f,
    3.0f,
    -2.0f,
    -4.0f,
    5.0f,
});
constexpr auto kSing4x4ZeroCol0 = std::to_array<float>({
    0.0f,
    1.0f,
    2.0f,
    3.0f,
    0.0f,
    4.0f,
    5.0f,
    6.0f,
    0.0f,
    7.0f,
    8.0f,
    9.0f,
    0.0f,
    1.0f,
    0.0f,
    1.0f,
});
constexpr auto kSing4x4ZeroCol2 = std::to_array<float>({
    1.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    2.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    3.0f,
    0.0f,
    0.0f,
    0.0f,
    4.0f,
});
constexpr auto kSing4x4DependentCols = std::to_array<float>({
    2.0f,
    0.0f,
    2.0f,
    1.0f,
    1.0f,
    2.0f,
    3.0f,
    0.0f,
    0.0f,
    1.0f,
    1.0f,
    4.0f,
    -1.0f,
    1.0f,
    0.0f,
    2.0f,
});

std::vector<float> MakeIdentity(uint32_t n) {
  std::vector<float> matrix(static_cast<std::size_t>(n) * n, 0.0f);
  for (uint32_t i = 0; i < n; ++i) {
    matrix[static_cast<std::size_t>(i) * n + i] = 1.0f;
  }
  return matrix;
}

void ExpectMatricesNear(std::span<const float> expected,
                        std::span<const float> actual, uint32_t n,
                        float tolerance = kTolerance) {
  const std::size_t size = static_cast<std::size_t>(n) * n;
  ASSERT_EQ(size, expected.size());
  ASSERT_EQ(size, actual.size());
  for (uint32_t i = 0; i < n; ++i) {
    for (uint32_t j = 0; j < n; ++j) {
      const std::size_t idx = static_cast<std::size_t>(i) * n + j;
      EXPECT_NEAR(expected[idx], actual[idx], tolerance)
          << "element mismatch at (" << i << ", " << j << ")";
    }
  }
}

void ExpectMatrixProductIsIdentity(std::span<const float> a,
                                   std::span<const float> b, uint32_t n,
                                   float tolerance = kTolerance) {
  const std::size_t size = static_cast<std::size_t>(n) * n;
  ASSERT_EQ(size, a.size());
  ASSERT_EQ(size, b.size());
  for (uint32_t i = 0; i < n; ++i) {
    for (uint32_t j = 0; j < n; ++j) {
      float sum = 0.0f;
      for (uint32_t k = 0; k < n; ++k) {
        sum += a[static_cast<std::size_t>(i) * n + k] *
               b[static_cast<std::size_t>(k) * n + j];
      }
      const float expected = (i == j) ? 1.0f : 0.0f;
      EXPECT_NEAR(expected, sum, tolerance)
          << "product mismatch at (" << i << ", " << j << ")";
    }
  }
}

void VerifyInversion(std::span<const float> original, uint32_t n,
                     std::span<const float> expected_inverse = {}) {
  const std::size_t size = static_cast<std::size_t>(n) * n;
  ASSERT_EQ(size, original.size());

  // opj_matrix_inversion_f mutates pSrcMatrix in place into L/U factors, so
  // preserve `original` to verify A_orig * A_inv == I.
  std::vector<float> src_work(original.begin(), original.end());
  std::vector<float> actual(size, kOutputSentinel);

  ASSERT_EQ(OPJ_TRUE,
            opj_matrix_inversion_f(src_work.data(), actual.data(), n));

  ExpectMatrixProductIsIdentity(original, actual, n);
  ExpectMatrixProductIsIdentity(actual, original, n);
  if (!expected_inverse.empty()) {
    ExpectMatricesNear(expected_inverse, actual, n);
  }
}

TEST(InvertTest, ZeroDimensionReturnsFalse) {
  // With nb_compo == 0, opj_malloc(0) returns nullptr and
  // opj_matrix_inversion_f exits before dereferencing either matrix pointer.
  EXPECT_EQ(OPJ_FALSE, opj_matrix_inversion_f(nullptr, nullptr, 0));

  std::array<float, 1> src = {42.0f};
  std::array<float, 1> dst = {kOutputSentinel};
  EXPECT_EQ(OPJ_FALSE, opj_matrix_inversion_f(src.data(), dst.data(), 0));
  EXPECT_FLOAT_EQ(42.0f, src[0]);
  EXPECT_FLOAT_EQ(kOutputSentinel, dst[0]);
}

TEST(InvertTest, IdentityMatricesInvertToIdentity) {
  for (uint32_t n : {1U, 2U, 3U, 4U}) {
    SCOPED_TRACE(n);
    const std::vector<float> identity = MakeIdentity(n);
    VerifyInversion(identity, n, identity);
  }
}

TEST(InvertTest, OneByOneNonZeroMatricesInvertToReciprocal) {
  // Note: 1x1 [0.0f] is intentionally omitted because opj_lupDecompose only
  // checks pivots for k < nb_compo - 1, so nb_compo == 1 would bypass the zero
  // check and divide by 0.0f in opj_lupSolve.
  constexpr MatrixCase kCases[] = {
      {.name = "unit",
       .n = 1,
       .matrix = kMat1x1One,
       .expected_inverse = kMat1x1One},
      {.name = "four",
       .n = 1,
       .matrix = kMat1x1Four,
       .expected_inverse = kMat1x1FourInv},
      {.name = "negative",
       .n = 1,
       .matrix = kMat1x1Neg,
       .expected_inverse = kMat1x1NegInv},
      {.name = "eighth",
       .n = 1,
       .matrix = kMat1x1Eighth,
       .expected_inverse = kMat1x1EighthInv},
  };

  for (const MatrixCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    VerifyInversion(tc.matrix, tc.n, tc.expected_inverse);
  }
}

TEST(InvertTest, MatricesWithoutRowSwapsInvertCorrectly) {
  constexpr MatrixCase kCases[] = {
      {.name = "2x2_positive_pivots",
       .n = 2,
       .matrix = kMat2x2NoSwap,
       .expected_inverse = kMat2x2NoSwapInv},
      {.name = "2x2_negative_pivots",
       .n = 2,
       .matrix = kMat2x2NoSwapNegPivot,
       .expected_inverse = kMat2x2NoSwapNegPivotInv},
      {.name = "3x3_no_swap",
       .n = 3,
       .matrix = kMat3x3NoSwap,
       .expected_inverse = kMat3x3NoSwapInv},
      {.name = "4x4_diagonally_dominant_negative_pivots",
       .n = 4,
       .matrix = kMat4x4NoSwap,
       .expected_inverse = {}},
  };

  for (const MatrixCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    VerifyInversion(tc.matrix, tc.n, tc.expected_inverse);
  }
}

TEST(InvertTest, MatricesRequiringRowPivotSwapsInvertCorrectly) {
  constexpr MatrixCase kCases[] = {
      {.name = "2x2_zero_diag_negative_pivot",
       .n = 2,
       .matrix = kMat2x2SwapZeroDiag,
       .expected_inverse = kMat2x2SwapZeroDiagInv},
      {.name = "2x2_nonzero_diag_negative_pivot",
       .n = 2,
       .matrix = kMat2x2SwapNegPivot,
       .expected_inverse = kMat2x2SwapNegPivotInv},
      {.name = "3x3_multi_step_swaps",
       .n = 3,
       .matrix = kMat3x3SwapMultiStep,
       .expected_inverse = {}},
      {.name = "3x3_jpeg2000_ict",
       .n = 3,
       .matrix = kMat3x3Jpeg2kIct,
       .expected_inverse = {}},
      {.name = "4x4_signed_cyclic_permutation",
       .n = 4,
       .matrix = kMat4x4SignedCyclicPerm,
       .expected_inverse = kMat4x4SignedCyclicPermInv},
      {.name = "4x4_dense_all_step_swaps",
       .n = 4,
       .matrix = kMat4x4SwapDense,
       .expected_inverse = {}},
      {.name = "5x5_dense_swaps",
       .n = 5,
       .matrix = kMat5x5SwapDense,
       .expected_inverse = {}},
  };

  for (const MatrixCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    VerifyInversion(tc.matrix, tc.n, tc.expected_inverse);
  }
}

TEST(InvertTest, DoubleInversionRecoversOriginalMatrix) {
  constexpr MatrixCase kCases[] = {
      {.name = "3x3_multi_step_swaps",
       .n = 3,
       .matrix = kMat3x3SwapMultiStep,
       .expected_inverse = {}},
      {.name = "4x4_dense_swaps",
       .n = 4,
       .matrix = kMat4x4SwapDense,
       .expected_inverse = {}},
  };

  for (const MatrixCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const std::size_t size = static_cast<std::size_t>(tc.n) * tc.n;
    std::vector<float> src1(tc.matrix.begin(), tc.matrix.end());
    std::vector<float> inv(size, kOutputSentinel);
    ASSERT_EQ(OPJ_TRUE, opj_matrix_inversion_f(src1.data(), inv.data(), tc.n));

    std::vector<float> src2(inv.begin(), inv.end());
    std::vector<float> inv_inv(size, kOutputSentinel);
    ASSERT_EQ(OPJ_TRUE,
              opj_matrix_inversion_f(src2.data(), inv_inv.data(), tc.n));
    ExpectMatricesNear(tc.matrix, inv_inv, tc.n);
  }
}

TEST(InvertTest, SingularMatricesReturnFalse) {
  // Each singular matrix becomes zero in column k < nb_compo - 1 so
  // opj_lupDecompose returns OPJ_FALSE before opj_lupSolve divides by u.
  constexpr SingularCase kCases[] = {
      {.name = "2x2_all_zero", .n = 2, .matrix = kSing2x2AllZero},
      {.name = "2x2_zero_col0", .n = 2, .matrix = kSing2x2ZeroCol0},
      {.name = "2x2_neg_zero_col0", .n = 2, .matrix = kSing2x2NegZeroCol0},
      {.name = "3x3_zero_col0", .n = 3, .matrix = kSing3x3ZeroCol0},
      {.name = "3x3_zero_col1", .n = 3, .matrix = kSing3x3ZeroCol1},
      {.name = "3x3_dependent_cols", .n = 3, .matrix = kSing3x3DependentCols},
      {.name = "4x4_zero_col0", .n = 4, .matrix = kSing4x4ZeroCol0},
      {.name = "4x4_zero_col2", .n = 4, .matrix = kSing4x4ZeroCol2},
      {.name = "4x4_dependent_cols", .n = 4, .matrix = kSing4x4DependentCols},
  };

  for (const SingularCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const std::size_t size = static_cast<std::size_t>(tc.n) * tc.n;
    ASSERT_EQ(size, tc.matrix.size());
    std::vector<float> src(tc.matrix.begin(), tc.matrix.end());
    std::vector<float> dst(size, kOutputSentinel);
    EXPECT_EQ(OPJ_FALSE, opj_matrix_inversion_f(src.data(), dst.data(), tc.n));
  }
}

}  // namespace
}  // namespace openjpeg2
