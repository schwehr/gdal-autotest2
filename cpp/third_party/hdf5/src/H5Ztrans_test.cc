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
// Tests algebraic data transform expression lexing, recursive-descent parsing,
// constant folding, parse-tree cloning, evaluation across native integer and
// floating-point types, and dataset I/O / DXPL integration
// (third_party/hdf5/src/H5Ztrans.c).
//
// Google3 Linux builds compile C with -funsigned-char (CHAR_MIN == 0), so
// H5T_NATIVE_CHAR aliases H5T_NATIVE_UCHAR and the signed-char #else branch in
// H5Ztrans.c is compiled out.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ztrans.c

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer)
#define H5ZTRANS_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define H5ZTRANS_TEST_HAS_OTHER_SAN 1
#endif
#endif
#if !defined(H5ZTRANS_TEST_HAS_ASAN_OR_LSAN) &&                   \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__))
#define H5ZTRANS_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if !defined(H5ZTRANS_TEST_HAS_OTHER_SAN) &&                 \
    (defined(MEMORY_SANITIZER) || defined(THREAD_SANITIZER) || \
     defined(__SANITIZE_THREAD__))
#define H5ZTRANS_TEST_HAS_OTHER_SAN 1
#endif

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields in internal
// headers pulled by `hdf5_internal_prelude.h`. Define NDEBUG around the
// internal headers so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5ZTRANS_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Zprivate.h"
}  // extern "C"
#ifdef H5ZTRANS_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ZTRANS_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::FloatNear;

// Move-only RAII guard for internal H5Z_data_xform_t* handles.
class ScopedXform {
 public:
  explicit ScopedXform(H5Z_data_xform_t* xform = nullptr) : xform_(xform) {}
  ~ScopedXform() { reset(); }

  ScopedXform(const ScopedXform&) = delete;
  ScopedXform& operator=(const ScopedXform&) = delete;

  ScopedXform(ScopedXform&& other) noexcept
      : xform_(std::exchange(other.xform_, nullptr)) {}
  ScopedXform& operator=(ScopedXform&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.xform_, nullptr));
    }
    return *this;
  }

  void reset(H5Z_data_xform_t* next = nullptr) {
    if (xform_ != nullptr) {
      H5Z_xform_destroy(xform_);
    }
    xform_ = next;
  }

  H5Z_data_xform_t* get() const { return xform_; }
  bool valid() const { return xform_ != nullptr; }

 private:
  H5Z_data_xform_t* xform_ = nullptr;
};

const H5T_t* LookupNativeType(hid_t type_id) {
  return static_cast<const H5T_t*>(H5I_object(type_id));
}

template <typename T>
void ExpectElementNear(T actual, double expected) {
  if constexpr (std::is_same_v<T, float>) {
    EXPECT_THAT(actual, FloatNear(static_cast<float>(expected), 1e-4f));
  } else if constexpr (std::is_floating_point_v<T>) {
    EXPECT_THAT(static_cast<double>(actual), DoubleNear(expected, 1e-9));
  } else {
    EXPECT_EQ(actual, static_cast<T>(expected));
  }
}

template <typename T>
void EvalAndCheck(const char* expr, hid_t type_id,
                  const std::vector<T>& initial,
                  const std::vector<double>& expected) {
  ASSERT_EQ(initial.size(), expected.size());
  ScopedXform xform(H5Z_xform_create(expr));
  ASSERT_TRUE(xform.valid()) << "expr=" << expr;
  const H5T_t* h5_type = LookupNativeType(type_id);
  ASSERT_NE(h5_type, nullptr);

  std::vector<T> buf(initial.size());
  // Zero-initialize full storage before writing so x86_64 MSan never reads
  // uninitialized padding bytes in 80-bit `long double`.
  std::memset(buf.data(), 0, buf.size() * sizeof(T));
  for (size_t i = 0; i < initial.size(); ++i) {
    buf[i] = initial[i];
  }

  ASSERT_GE(H5Z_xform_eval(xform.get(), buf.data(), buf.size(), h5_type), 0)
      << "expr=" << expr;
  for (size_t i = 0; i < buf.size(); ++i) {
    ExpectElementNear(buf[i], expected[i]);
  }
}

template <typename T>
void VerifyAllOperationsForType(hid_t type_id, bool is_signed) {
  auto make_vec = [](std::initializer_list<int> vals) {
    std::vector<T> v(vals.size());
    std::memset(v.data(), 0, v.size() * sizeof(T));
    size_t idx = 0;
    for (int val : vals) {
      v[idx++] = static_cast<T>(val);
    }
    return v;
  };

  const std::vector<T> pos = make_vec({4, 6, 8});

  // 1. Trivial constant integer and float transforms (H5Z_XFORM_DO_OP5).
  EvalAndCheck<T>("7", type_id, pos, {7, 7, 7});
  EvalAndCheck<T>("2.5", type_id, pos,
                  {static_cast<double>(static_cast<T>(2.5)),
                   static_cast<double>(static_cast<T>(2.5)),
                   static_cast<double>(static_cast<T>(2.5))});

  // 2. Symbol on left-hand side with integer and float constants.
  EvalAndCheck<T>("x + 3", type_id, pos, {7, 9, 11});
  EvalAndCheck<T>("x + 2.5", type_id, pos,
                  {static_cast<double>(static_cast<T>(6.5)),
                   static_cast<double>(static_cast<T>(8.5)),
                   static_cast<double>(static_cast<T>(10.5))});
  EvalAndCheck<T>("x - 1", type_id, pos, {3, 5, 7});
  EvalAndCheck<T>("x - 1.5", type_id, pos,
                  {static_cast<double>(static_cast<T>(2.5)),
                   static_cast<double>(static_cast<T>(4.5)),
                   static_cast<double>(static_cast<T>(6.5))});
  EvalAndCheck<T>("x * 2", type_id, pos, {8, 12, 16});
  EvalAndCheck<T>("x * 1.5", type_id, pos, {6, 9, 12});
  EvalAndCheck<T>("x / 2", type_id, pos, {2, 3, 4});
  EvalAndCheck<T>("x / 2.0", type_id, pos, {2, 3, 4});

  // 3. Symbol on right-hand side with integer and float constants.
  EvalAndCheck<T>("3 + x", type_id, pos, {7, 9, 11});
  EvalAndCheck<T>("2.5 + x", type_id, pos,
                  {static_cast<double>(static_cast<T>(6.5)),
                   static_cast<double>(static_cast<T>(8.5)),
                   static_cast<double>(static_cast<T>(10.5))});
  EvalAndCheck<T>("20 - x", type_id, pos, {16, 14, 12});
  EvalAndCheck<T>("20.5 - x", type_id, pos,
                  {static_cast<double>(static_cast<T>(16.5)),
                   static_cast<double>(static_cast<T>(14.5)),
                   static_cast<double>(static_cast<T>(12.5))});
  EvalAndCheck<T>("3 * x", type_id, pos, {12, 18, 24});
  EvalAndCheck<T>("1.5 * x", type_id, pos, {6, 9, 12});
  EvalAndCheck<T>("48 / x", type_id, pos, {12, 8, 6});
  EvalAndCheck<T>("48.0 / x", type_id, pos, {12, 8, 6});

  // 4. Unary '+' (all types) and unary '-' (signed types to avoid unsigned UB).
  EvalAndCheck<T>("+x", type_id, pos, {4, 6, 8});
  if (is_signed) {
    EvalAndCheck<T>("-x", type_id, pos, {-4, -6, -8});
  }

  // 5. Polynomial / multi-symbol transforms (allocates temporary array copies).
  EvalAndCheck<T>("x + x", type_id, pos, {8, 12, 16});
  EvalAndCheck<T>("(x + 2) - x", type_id, pos, {2, 2, 2});
  EvalAndCheck<T>("x * x", type_id, pos, {16, 36, 64});
  EvalAndCheck<T>("(x * 2) / x", type_id, pos, {2, 2, 2});
  EvalAndCheck<T>("x * x + x * (1 + 1)", type_id, pos, {24, 48, 80});
}

TEST(H5ZtransTest, CreateCopyNoopExtractAndDestroy) {
  ASSERT_GE(H5open(), 0);

  // Destroying or copying a null transform is a safe no-op.
  EXPECT_GE(H5Z_xform_destroy(nullptr), 0);
  EXPECT_TRUE(H5Z_xform_noop(nullptr));
  H5Z_data_xform_t* null_copy = nullptr;
  EXPECT_GE(H5Z_xform_copy(&null_copy), 0);
  EXPECT_EQ(null_copy, nullptr);

  // Only a bare single-character symbol expression is treated as a no-op.
  struct NoopCase {
    const char* expr;
    bool expected_noop;
  };
  constexpr auto kNoopCases = std::to_array<NoopCase>({
      {.expr = "x", .expected_noop = true},
      {.expr = "y", .expected_noop = true},
      {.expr = "5", .expected_noop = false},
      {.expr = " x ", .expected_noop = false},
      {.expr = "x + 1", .expected_noop = false},
  });
  for (const auto& tc : kNoopCases) {
    ScopedXform xform(H5Z_xform_create(tc.expr));
    ASSERT_TRUE(xform.valid()) << tc.expr;
    EXPECT_EQ(H5Z_xform_noop(xform.get()), tc.expected_noop) << tc.expr;
    EXPECT_STREQ(H5Z_xform_extract_xform_str(xform.get()), tc.expr);
  }

  // Copy parse trees covering integer, float, symbol, unary, and all 4 ops.
  // Avoid scientific-notation 'e' in H5Z_xform_copy because upstream
  // H5Z_xform_copy counts 'e' as an isalpha variable mismatch and leaks.
  struct CopyCase {
    const char* expr;
    std::array<double, 3> input;
    std::array<double, 3> expected;
  };
  constexpr auto kCopyCases = std::to_array<CopyCase>({
      {.expr = "42", .input = {1.0, 2.0, 3.0}, .expected = {42.0, 42.0, 42.0}},
      {.expr = "3.5", .input = {1.0, 2.0, 3.0}, .expected = {3.5, 3.5, 3.5}},
      {.expr = "+x", .input = {1.0, -2.0, 3.0}, .expected = {1.0, -2.0, 3.0}},
      {.expr = "-x", .input = {1.0, -2.0, 3.0}, .expected = {-1.0, 2.0, -3.0}},
      {.expr = "(x * 2 + 3.5) / (4 - x)",
       .input = {0.0, 2.0, 3.0},
       .expected = {3.5 / 4.0, 7.5 / 2.0, 9.5 / 1.0}},
  });

  const H5T_t* double_type = LookupNativeType(H5T_NATIVE_DOUBLE);
  ASSERT_NE(double_type, nullptr);

  for (const auto& tc : kCopyCases) {
    ScopedXform orig(H5Z_xform_create(tc.expr));
    ASSERT_TRUE(orig.valid()) << tc.expr;

    H5Z_data_xform_t* raw_copy = orig.get();
    ASSERT_GE(H5Z_xform_copy(&raw_copy), 0) << tc.expr;
    ScopedXform copy(raw_copy);
    EXPECT_NE(copy.get(), orig.get());
    EXPECT_STREQ(H5Z_xform_extract_xform_str(copy.get()), tc.expr);

    // Destroy original before evaluating copy to confirm deep ownership.
    orig.reset();

    std::array<double, 3> data = tc.input;
    ASSERT_GE(H5Z_xform_eval(copy.get(), data.data(), data.size(), double_type),
              0);
    for (size_t i = 0; i < data.size(); ++i) {
      EXPECT_THAT(data[i], DoubleNear(tc.expected[i], 1e-12)) << tc.expr;
    }
  }
}

TEST(H5ZtransTest, ParseValidExpressionsAndConstantFolding) {
  ASSERT_GE(H5open(), 0);
  const H5T_t* double_type = LookupNativeType(H5T_NATIVE_DOUBLE);
  ASSERT_NE(double_type, nullptr);

  struct FoldCase {
    const char* expr;
    double expected;
  };
  constexpr auto kFoldCases = std::to_array<FoldCase>({
      // Literals and float lexing variants.
      {.expr = "0", .expected = 0.0},
      {.expr = "42", .expected = 42.0},
      {.expr = "3.25", .expected = 3.25},
      {.expr = ".5", .expected = 0.5},
      {.expr = "5.", .expected = 5.0},
      {.expr = ".", .expected = 0.0},
      {.expr = "1e2", .expected = 100.0},
      {.expr = "1.5e+2", .expected = 150.0},
      {.expr = "250E-2", .expected = 2.5},
      {.expr = ".5e2", .expected = 50.0},
      {.expr = "5.E-1", .expected = 0.5},
      {.expr = " \t\n 7 + 3 \r ", .expected = 10.0},
      // Unary folding.
      {.expr = "+5", .expected = 5.0},
      {.expr = "-5", .expected = -5.0},
      {.expr = "+3.5", .expected = 3.5},
      {.expr = "-3.5", .expected = -3.5},
      // Binary int/float combinations for +, -, *, /.
      {.expr = "2 + 3", .expected = 5.0},
      {.expr = "2 + 3.5", .expected = 5.5},
      {.expr = "2.5 + 3", .expected = 5.5},
      {.expr = "2.5 + 3.5", .expected = 6.0},
      {.expr = "10 - 3", .expected = 7.0},
      {.expr = "10 - 3.5", .expected = 6.5},
      {.expr = "10.5 - 3", .expected = 7.5},
      {.expr = "10.5 - 3.5", .expected = 7.0},
      {.expr = "3 * 4", .expected = 12.0},
      {.expr = "3 * 2.5", .expected = 7.5},
      {.expr = "2.5 * 3", .expected = 7.5},
      {.expr = "2.5 * 1.5", .expected = 3.75},
      {.expr = "9 / 2", .expected = 4.0},
      {.expr = "9 / 2.0", .expected = 4.5},
      {.expr = "9.0 / 2", .expected = 4.5},
      {.expr = "9.0 / 2.0", .expected = 4.5},
      // Multi-level reduction on lchild and rchild.
      {.expr = "(2 + 3) * 4", .expected = 20.0},
      {.expr = "4 * (2 + 3)", .expected = 20.0},
      {.expr = "(2 + 3) * (4 + 1)", .expected = 25.0},
      {.expr = "(8 + 4) / 3", .expected = 4.0},
      {.expr = "24 / (2 + 4)", .expected = 4.0},
      {.expr = "(2 * 3) + 4", .expected = 10.0},
      {.expr = "4 + (2 * 3)", .expected = 10.0},
      {.expr = "(2 * 3) - 4", .expected = 2.0},
      {.expr = "10 - (2 * 3)", .expected = 4.0},
      {.expr = "-5 + 8", .expected = 3.0},
      {.expr = "8 + -5", .expected = 3.0},
      {.expr = "2 + -5", .expected = -3.0},
      // Precedence and left-associativity.
      {.expr = "10 - 3 - 2", .expected = 5.0},
      {.expr = "2 + 3 * 4", .expected = 14.0},
      {.expr = "24 / 4 / 2", .expected = 3.0},
  });

  for (const auto& tc : kFoldCases) {
    ScopedXform xform(H5Z_xform_create(tc.expr));
    ASSERT_TRUE(xform.valid()) << tc.expr;
    EXPECT_FALSE(H5Z_xform_noop(xform.get())) << tc.expr;

    std::array<double, 2> values = {99.0, -99.0};
    ASSERT_GE(
        H5Z_xform_eval(xform.get(), values.data(), values.size(), double_type),
        0)
        << tc.expr;
    EXPECT_THAT(values[0], DoubleNear(tc.expected, 1e-12)) << tc.expr;
    EXPECT_THAT(values[1], DoubleNear(tc.expected, 1e-12)) << tc.expr;
  }

  // Alphanumeric single-letter symbol ("x1"), leading/trailing 'e' symbols, and
  // scientific notation mixed with a symbol.
  EvalAndCheck<double>("x1 + 2", H5T_NATIVE_DOUBLE, {1.0, 2.0}, {3.0, 4.0});
  EvalAndCheck<double>("e + 1", H5T_NATIVE_DOUBLE, {1.0, 2.0}, {2.0, 3.0});
  EvalAndCheck<double>("1 + e", H5T_NATIVE_DOUBLE, {1.0, 2.0}, {2.0, 3.0});
  EvalAndCheck<double>("x + 1e2", H5T_NATIVE_DOUBLE, {1.0, 2.0},
                       {101.0, 102.0});
  EvalAndCheck<double>("x * 2.5e-1", H5T_NATIVE_DOUBLE, {4.0, 8.0}, {1.0, 2.0});
  EvalAndCheck<double>("x + .5E+1", H5T_NATIVE_DOUBLE, {1.0, 2.0}, {6.0, 7.0});

  // Sub-tree constant folding alongside symbols (lchild and rchild).
  EvalAndCheck<double>("x * (2 + 3)", H5T_NATIVE_DOUBLE, {2.0, 3.0},
                       {10.0, 15.0});
  EvalAndCheck<double>("(2 + 3) * x", H5T_NATIVE_DOUBLE, {2.0, 3.0},
                       {10.0, 15.0});
  EvalAndCheck<double>("x + (2 * 3)", H5T_NATIVE_DOUBLE, {2.0, 3.0},
                       {8.0, 9.0});
  EvalAndCheck<double>("(2 * 3) + x", H5T_NATIVE_DOUBLE, {2.0, 3.0},
                       {8.0, 9.0});
}

TEST(H5ZtransTest, ParseInvalidExpressionsFailCleanly) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  constexpr auto kInvalidExprs = std::to_array<const char*>({
      "",
      "   ",
      // Malformed floats.
      "1e",
      "1e+",
      "1e-",
      "1E",
      ".e",
      "1.2.3",
      "1.2x",
      "1e2x",
      "1e2.3",
      "1.e2.",
      // Unknown characters.
      "@",
      "x @ 1",
      "x % 2",
      "x ^ 2",
      // Missing/invalid operands.
      "x +",
      "x -",
      "x *",
      "x /",
      "x + )",
      "x * )",
      "x / )",
      "x + * 2",
      "* 2",
      "/ 2",
      "* x",
      "/ x",
      // Parenthesis errors.
      "(",
      ")",
      "()",
      "(x",
      "(x + 1",
      "x + )",
      "((x + 1)",
      // Trailing unexpected tokens (exercises unget in H5Z__parse_term).
      "x 2",
      "x 2.5",
      "x y",
      "x (1)",
      "2 3",
      "x + 1 2",
      "x @",
      "x 1e+",
      // Unary +/- errors.
      "+",
      "-",
      "+*",
      "-*",
      "+)",
      "-)",
      "+(x + 1)",
      "-(x + 1)",
      "++x",
      "--x",
      "+-x",
      "-+x",
      // Multi-letter identifiers trigger variable-count sanity check.
      "xy",
      "var + 1",
      "x + var",
      "1e2 + xy",
      "1 + 2e",
  });

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());

  for (const char* expr : kInvalidExprs) {
    EXPECT_EQ(H5Z_xform_create(expr), nullptr) << expr;
    EXPECT_LT(H5Pset_data_transform(dxpl.get(), expr), 0) << expr;
  }
}

TEST(H5ZtransTest, EvaluateAllNativeTypesTrivialAndArithmetic) {
  ASSERT_GE(H5open(), 0);

  VerifyAllOperationsForType<char>(H5T_NATIVE_CHAR, /*is_signed=*/false);
  VerifyAllOperationsForType<unsigned char>(H5T_NATIVE_UCHAR,
                                            /*is_signed=*/false);
  VerifyAllOperationsForType<signed char>(H5T_NATIVE_SCHAR,
                                          /*is_signed=*/true);
  VerifyAllOperationsForType<short>(H5T_NATIVE_SHORT, /*is_signed=*/true);
  VerifyAllOperationsForType<unsigned short>(H5T_NATIVE_USHORT,
                                             /*is_signed=*/false);
  VerifyAllOperationsForType<int>(H5T_NATIVE_INT, /*is_signed=*/true);
  VerifyAllOperationsForType<unsigned int>(H5T_NATIVE_UINT,
                                           /*is_signed=*/false);
  VerifyAllOperationsForType<long>(H5T_NATIVE_LONG, /*is_signed=*/true);
  VerifyAllOperationsForType<unsigned long>(H5T_NATIVE_ULONG,
                                            /*is_signed=*/false);
  VerifyAllOperationsForType<long long>(H5T_NATIVE_LLONG, /*is_signed=*/true);
  VerifyAllOperationsForType<unsigned long long>(H5T_NATIVE_ULLONG,
                                                 /*is_signed=*/false);
  VerifyAllOperationsForType<float>(H5T_NATIVE_FLOAT, /*is_signed=*/true);
  VerifyAllOperationsForType<double>(H5T_NATIVE_DOUBLE, /*is_signed=*/true);
  VerifyAllOperationsForType<long double>(H5T_NATIVE_LDOUBLE,
                                          /*is_signed=*/true);
}

TEST(H5ZtransTest, EvaluateUnsupportedTypesFailCleanly) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  ScopedXform xform(H5Z_xform_create("x + 1"));
  ASSERT_TRUE(xform.valid());
  ScopedXform trivial(H5Z_xform_create("7"));
  ASSERT_TRUE(trivial.valid());

  // Non-native byte order.
  ScopedType swapped_int(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(swapped_int.valid());
  const H5T_order_t native_order = H5Tget_order(H5T_NATIVE_INT);
  ASSERT_GE(H5Tset_order(swapped_int.get(),
                         native_order == H5T_ORDER_LE ? H5T_ORDER_BE
                                                      : H5T_ORDER_LE),
            0);

  // Compound type.
  struct Pair {
    int a;
    double b;
  };
  ScopedType compound_type(H5Tcreate(H5T_COMPOUND, sizeof(Pair)));
  ASSERT_TRUE(compound_type.valid());
  ASSERT_GE(H5Tinsert(compound_type.get(), "a", offsetof(Pair, a),
                      H5T_NATIVE_INT),
            0);
  ASSERT_GE(H5Tinsert(compound_type.get(), "b", offsetof(Pair, b),
                      H5T_NATIVE_DOUBLE),
            0);

  std::array<uint8_t, 64> raw_buf{};
  const std::array<hid_t, 5> unsupported_ids = {
      H5T_C_S1, H5T_NATIVE_B32, H5T_NATIVE_FLOAT16, swapped_int.get(),
      compound_type.get(),
  };

  for (hid_t tid : unsupported_ids) {
    const H5T_t* h5_type = LookupNativeType(tid);
    ASSERT_NE(h5_type, nullptr);
    EXPECT_LT(H5Z_xform_eval(xform.get(), raw_buf.data(), 1, h5_type), 0);
    EXPECT_LT(H5Z_xform_eval(trivial.get(), raw_buf.data(), 1, h5_type), 0);
  }
}

TEST(H5ZtransTest, PublicDatasetWriteReadAndDxplProperties) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // 1. Validate DXPL set/get/replace/copy/equal behavior.
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());

  {
    ScopedErrorSilence silence;
    std::array<char, 32> out{};
    EXPECT_LT(H5Pget_data_transform(dxpl.get(), out.data(), out.size()), 0);
    EXPECT_LT(H5Pset_data_transform(dxpl.get(), nullptr), 0);
    EXPECT_LT(H5Pset_data_transform(H5I_INVALID_HID, "x + 1"), 0);
  }

  constexpr char kWriteExpr[] = "2 * x + 1";
  ASSERT_GE(H5Pset_data_transform(dxpl.get(), kWriteExpr), 0);

  const ssize_t query_len = H5Pget_data_transform(dxpl.get(), nullptr, 0);
  ASSERT_EQ(query_len, static_cast<ssize_t>(std::strlen(kWriteExpr)));

  std::array<char, 4> short_buf{};
  EXPECT_EQ(H5Pget_data_transform(dxpl.get(), short_buf.data(), short_buf.size()),
            query_len);
  EXPECT_STREQ(short_buf.data(), "2 *");

  std::string full_buf(static_cast<size_t>(query_len) + 1, '\0');
  EXPECT_EQ(H5Pget_data_transform(dxpl.get(), full_buf.data(), full_buf.size()),
            query_len);
  EXPECT_STREQ(full_buf.c_str(), kWriteExpr);

  ScopedPlist dxpl_copy(H5Pcopy(dxpl.get()));
  ASSERT_TRUE(dxpl_copy.valid());
  EXPECT_GT(H5Pequal(dxpl.get(), dxpl_copy.get()), 0);

  // Replace existing expression on dxpl_copy and confirm inequality.
  constexpr char kReadExpr[] = "(x - 1) / 2";
  ASSERT_GE(H5Pset_data_transform(dxpl_copy.get(), kReadExpr), 0);
  EXPECT_EQ(H5Pequal(dxpl.get(), dxpl_copy.get()), 0);

  // 2. Write contiguous int32 dataset with "2 * x + 1".
  constexpr std::array<hsize_t, 1> kDims = {6};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset contig_dset(H5Dcreate2(file.get(), "contig", H5T_NATIVE_INT,
                                       space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
  ASSERT_TRUE(contig_dset.valid());

  constexpr std::array<int, 6> kInputInts = {0, 1, 2, 3, 4, 5};
  ASSERT_GE(H5Dwrite(contig_dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                     dxpl.get(), kInputInts.data()),
            0);

  // Reading with default DXPL returns transformed stored values.
  std::array<int, 6> stored_ints{};
  ASSERT_GE(H5Dread(contig_dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, stored_ints.data()),
            0);
  EXPECT_THAT(stored_ints, ElementsAre(1, 3, 5, 7, 9, 11));

  // Reading with inverse transform recovers original values.
  std::array<int, 6> roundtrip_ints{};
  ASSERT_GE(H5Dread(contig_dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                    dxpl_copy.get(), roundtrip_ints.data()),
            0);
  EXPECT_THAT(roundtrip_ints, ElementsAre(0, 1, 2, 3, 4, 5));

  // Reading with trivial "x" no-op transform matches stored values.
  ScopedPlist noop_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(noop_dxpl.valid());
  ASSERT_GE(H5Pset_data_transform(noop_dxpl.get(), "x"), 0);
  std::array<int, 6> noop_ints{};
  ASSERT_GE(H5Dread(contig_dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                    noop_dxpl.get(), noop_ints.data()),
            0);
  EXPECT_THAT(noop_ints, ElementsAre(1, 3, 5, 7, 9, 11));

  // 3. Write chunked double dataset with polynomial transform "x * x + x".
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  constexpr std::array<hsize_t, 1> kChunkDims = {3};
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, kChunkDims.data()), 0);

  ScopedDataset chunked_dset(H5Dcreate2(file.get(), "chunked",
                                        H5T_NATIVE_DOUBLE, space.get(),
                                        H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(chunked_dset.valid());

  ScopedPlist poly_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(poly_dxpl.valid());
  ASSERT_GE(H5Pset_data_transform(poly_dxpl.get(), "x * x + x"), 0);

  constexpr std::array<double, 6> kInputDoubles = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  ASSERT_GE(H5Dwrite(chunked_dset.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                     poly_dxpl.get(), kInputDoubles.data()),
            0);

  std::array<double, 6> stored_doubles{};
  ASSERT_GE(H5Dread(chunked_dset.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, stored_doubles.data()),
            0);
  EXPECT_THAT(stored_doubles, ElementsAre(2.0, 6.0, 12.0, 20.0, 30.0, 42.0));

  // Partial hyperslab read with transform only modifies selected region.
  constexpr std::array<hsize_t, 1> kStart = {2};
  constexpr std::array<hsize_t, 1> kCount = {2};
  ScopedSpace mem_space(H5Screate_simple(1, kCount.data(), nullptr));
  ASSERT_TRUE(mem_space.valid());
  ScopedSpace file_space(H5Dget_space(chunked_dset.get()));
  ASSERT_TRUE(file_space.valid());
  ASSERT_GE(H5Sselect_hyperslab(file_space.get(), H5S_SELECT_SET,
                                kStart.data(), nullptr, kCount.data(), nullptr),
            0);

  ScopedPlist read_scale_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(read_scale_dxpl.valid());
  ASSERT_GE(H5Pset_data_transform(read_scale_dxpl.get(), "x / 2.0"), 0);
  std::array<double, 2> slice_out{};
  ASSERT_GE(H5Dread(chunked_dset.get(), H5T_NATIVE_DOUBLE, mem_space.get(),
                    file_space.get(), read_scale_dxpl.get(), slice_out.data()),
            0);
  EXPECT_THAT(slice_out, ElementsAre(6.0, 10.0));

  // 4. Attempting a data transform on a string dataset fails cleanly.
  ScopedType str_type(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str_type.valid());
  ASSERT_GE(H5Tset_size(str_type.get(), 4), 0);
  constexpr std::array<hsize_t, 1> kOneDim = {1};
  ScopedSpace one_space(H5Screate_simple(1, kOneDim.data(), nullptr));
  ASSERT_TRUE(one_space.valid());
  ScopedDataset str_dset(H5Dcreate2(file.get(), "str", str_type.get(),
                                    one_space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
  ASSERT_TRUE(str_dset.valid());
  {
    ScopedErrorSilence silence;
    std::array<char, 4> str_data = {'a', 'b', 'c', '\0'};
    EXPECT_LT(H5Dwrite(str_dset.get(), str_type.get(), H5S_ALL, H5S_ALL,
                       dxpl.get(), str_data.data()),
              0);
  }
}

#if !defined(H5ZTRANS_TEST_HAS_ASAN_OR_LSAN) && \
    !defined(H5ZTRANS_TEST_HAS_OTHER_SAN)
TEST(H5ZtransTest, CopyMismatchAndOomAllocationFailuresReturnErrors) {
  const std::string prev_style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  absl::Cleanup restore_style = [&] {
    GTEST_FLAG_SET(death_test_style, prev_style);
  };

  const auto run_oom_checks = []() {
    if (H5open() < 0) std::exit(1);
    ScopedErrorSilence silence;

    // Upstream H5Z_xform_create skips scientific-notation 'e'/'E' when counting
    // variables, while H5Z_xform_copy counts every isalpha() character and hits
    // a variable-count mismatch. Run inside the isolated child process because
    // H5Z_xform_copy's error path leaks dat_val_pointers and ptr_dat_val (32 B).
    {
      ScopedXform sci_xform(H5Z_xform_create("1e2 + x"));
      if (!sci_xform.valid()) std::exit(12);
      H5Z_data_xform_t* sci_copy = sci_xform.get();
      if (H5Z_xform_copy(&sci_copy) >= 0) std::exit(13);
      if (sci_copy != sci_xform.get()) std::exit(14);
    }

    // Warm small size classes so error reporting and small structs succeed
    // while large spans are exhausted via RLIMIT_AS.
    constexpr std::array<size_t, 8> kWarmSizes = {16,  32,  64,   128,
                                                  256, 512, 1024, 2048};
    for (size_t sz : kWarmSizes) {
      void* p = std::malloc(sz);
      std::free(p);
    }

    // Build test inputs before tightening RLIMIT_AS.
    // Note: H5Z_xform_create calls strlen(expr) inside a per-character loop
    // (O(N^2)), so keep any successfully created string <= 136 KiB.
    // 1. Huge expression (16 MiB) for H5MM_xstrdup OOM in H5Z_xform_create
    //    (fails at xstrdup before reaching the O(N^2) loop).
    std::string huge_space_expr(16ULL << 20, ' ');
    huge_space_expr += 'x';

    // 2. ~260 KiB space-padded expression for H5MM_xstrdup OOM in H5Z_xform_copy.
    std::string medium_space_expr(260ULL << 10, ' ');
    medium_space_expr += 'x';
    ScopedXform medium_space_xform(H5Z_xform_create(medium_space_expr.c_str()));
    if (!medium_space_xform.valid()) std::exit(2);
    medium_space_expr.clear();
    medium_space_expr.shrink_to_fit();

    // 3. Single long identifier (~200K chars) for ptr_dat_val OOM (~1.6 MiB)
    //    in H5Z_xform_create (zero parse-tree recursion).
    std::string many_alpha_expr(200000, 'x');

    // 4. Two-symbol transform and 16 MiB array buffer for partial-alloc OOM in
    //    H5Z_xform_eval (first ptr_dat_val succeeds, second fails and frees #0).
    ScopedXform two_x_xform(H5Z_xform_create("x + x"));
    if (!two_x_xform.valid()) std::exit(4);
    std::vector<uint8_t> large_buf(16ULL << 20, 1);
    const H5T_t* u8_type = LookupNativeType(H5T_NATIVE_UCHAR);
    if (u8_type == nullptr) std::exit(5);

    // Pre-reserve exact sacrificial spans to free just before two-step allocs.
    void* reserve_create = std::malloc(256ULL << 10);
    void* reserve_eval = std::malloc(16ULL << 20);
    if (!reserve_create || !reserve_eval) std::exit(6);

    std::ifstream statm("/proc/self/statm");
    unsigned long vm_pages = 0;
    if (!(statm >> vm_pages)) std::exit(7);
    const long page_sz = ::sysconf(_SC_PAGESIZE);
    if (page_sz <= 0) std::exit(8);
    const rlim_t vm_bytes =
        static_cast<rlim_t>(vm_pages) * static_cast<rlim_t>(page_sz);

    struct rlimit old_lim{};
    if (::getrlimit(RLIMIT_AS, &old_lim) != 0) std::exit(9);
    struct rlimit tight_lim = old_lim;
    tight_lim.rlim_cur = std::min<rlim_t>(
        old_lim.rlim_max, vm_bytes + static_cast<rlim_t>(2ULL << 20));
    if (::setrlimit(RLIMIT_AS, &tight_lim) != 0) std::exit(10);

    std::vector<void*> drain_raw;
    std::vector<uint8_t*> drain_u8;
    drain_raw.reserve(4096);
    drain_u8.reserve(4096);
    constexpr std::array<size_t, 6> kDrainSizes = {
        16ULL << 20, 4ULL << 20, 2ULL << 20,
        1032ULL << 10, 512ULL << 10, 256ULL << 10};
    for (size_t sz : kDrainSizes) {
      while (drain_raw.size() < 4096) {
        void* p = std::malloc(sz);
        if (p == nullptr) break;
        drain_raw.push_back(p);
      }
      while (drain_u8.size() < 4096) {
        auto* p = static_cast<uint8_t*>(std::malloc(sz));
        if (p == nullptr) break;
        drain_u8.push_back(p);
      }
    }

    // Check 1: H5Z_xform_create H5MM_xstrdup failure (16 MiB).
    H5Z_data_xform_t* created_huge = H5Z_xform_create(huge_space_expr.c_str());

    // Check 2: H5Z_xform_copy H5MM_xstrdup failure (~260 KiB), before freeing
    // reserve_create so no >= 256 KiB span exists in the heap.
    H5Z_data_xform_t* med_copy = medium_space_xform.get();
    const herr_t copy_med_rc = H5Z_xform_copy(&med_copy);

    // Check 3: H5Z_xform_create ptr_dat_val calloc failure after xstrdup ok.
    std::free(reserve_create);
    H5Z_data_xform_t* created_many = H5Z_xform_create(many_alpha_expr.c_str());

    // Check 4: H5Z_xform_eval partial temp-array alloc failure & cleanup.
    std::free(reserve_eval);
    const herr_t eval_oom_rc = H5Z_xform_eval(
        two_x_xform.get(), large_buf.data(), large_buf.size(), u8_type);

    ::setrlimit(RLIMIT_AS, &old_lim);
    for (void* p : drain_raw) std::free(p);
    for (uint8_t* p : drain_u8) std::free(p);

    if (created_huge != nullptr || created_many != nullptr ||
        copy_med_rc >= 0 || eval_oom_rc >= 0) {
      std::exit(11);
    }
    std::exit(0);
  };

  EXPECT_EXIT(run_oom_checks(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
