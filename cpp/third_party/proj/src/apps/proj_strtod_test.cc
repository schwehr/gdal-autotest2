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
// Unit tests for PROJ's locale-independent, underscore-accepting string-to-
// double parser (proj/src/apps/proj_strtod.cpp). Note: the upstream `#ifdef
// TEST` standalone harness in proj_strtod.cpp is not compiled into :proj4.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/apps/proj_strtod.cpp

#include "third_party/proj4/proj/src/apps/proj_strtod.h"

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <span>  // NOLINT(build/c++20)
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

namespace proj {
namespace {

struct DoubleParseCase {
  const char* input;
  double expected;
  double tol;
  std::string_view expected_tail;
};

struct ZeroCase {
  const char* input;
  bool expected_signbit;
  std::string_view expected_tail;
};

struct ErrorCase {
  const char* input;
  int expected_errno;
  std::string_view expected_tail;
};

template <typename CaseT>
constexpr bool AllTailsMatchInputs(std::span<const CaseT> cases) {
  if (cases.empty()) {
    return false;
  }
  for (const auto& tc : cases) {
    if (!std::string_view(tc.input).ends_with(tc.expected_tail)) {
      return false;
    }
  }
  return true;
}

void ExpectEndptrTail(const char* input, const char* endptr,
                      std::string_view expected_tail) {
  ASSERT_NE(nullptr, endptr) << "input=\"" << input << "\"";
  const std::string_view input_view(input);
  ASSERT_GE(input_view.size(), expected_tail.size())
      << "input=\"" << input << "\"";
  const auto offset =
      static_cast<std::ptrdiff_t>(input_view.size() - expected_tail.size());
  EXPECT_EQ(input + offset, endptr) << "input=\"" << input << "\"";
  EXPECT_EQ(expected_tail, endptr) << "input=\"" << input << "\"";
}

TEST(ProjStrtodTest, NullInputReturnsHugeValAndSetsEfault) {
  absl::Cleanup reset_errno = [] { errno = 0; };

  char sentinel = 'x';
  char* endptr = &sentinel;
  errno = 0;
  const double val = proj_strtod(nullptr, &endptr);
  EXPECT_EQ(EFAULT, errno);
  EXPECT_EQ(HUGE_VAL, val);
  EXPECT_EQ(nullptr, endptr);

  errno = 0;
  EXPECT_EQ(HUGE_VAL, proj_strtod(nullptr, nullptr));
  EXPECT_EQ(EFAULT, errno);

  errno = 0;
  EXPECT_EQ(HUGE_VAL, proj_atof(nullptr));  // NOLINT(runtime/deprecated_fn)
  EXPECT_EQ(EFAULT, errno);
}

TEST(ProjStrtodTest, EmptyWhitespaceAndNonNumericReturnZeroAtInputStart) {
  absl::Cleanup reset_errno = [] { errno = 0; };

  // Non-numeric prefixes, stray signs, "inf", and signed "NaN" (NaN check runs
  // before sign handling) must all leave endptr pointing to the start of `str`.
  constexpr const char* kInputs[] = {
      "",     " ",   "   \t\n\r\v\f", "abcde", "   abcde", "na",   "e1",
      "  E5", "inf", "-inf",          "+Inf",  "-NaN",     "+nan", "+",
      "-",    "  +", "  -",           "+ ",    "- ",       " + ",  " - ",
      "+/-",  "-+",  "+abc",          "-e5",
  };
  static_assert(std::size(kInputs) > 0);

  for (const char* input : kInputs) {
    char* endptr = nullptr;
    errno = 0;
    const double val = proj_strtod(input, &endptr);
    EXPECT_EQ(0, errno) << "input=\"" << input << "\"";
    EXPECT_DOUBLE_EQ(0.0, val) << "input=\"" << input << "\"";
    EXPECT_FALSE(std::signbit(val)) << "input=\"" << input << "\"";
    EXPECT_EQ(input, endptr) << "input=\"" << input << "\"";
  }
}

TEST(ProjStrtodTest, CaseInsensitiveNanAdvancesEndptrPastPrefix) {
  absl::Cleanup reset_errno = [] { errno = 0; };

  struct NanCase {
    const char* input;
    std::string_view expected_tail;
  };
  constexpr NanCase kCases[] = {
      {.input = "NaN", .expected_tail = ""},
      {.input = "nan", .expected_tail = ""},
      {.input = "nAn", .expected_tail = ""},
      {.input = "NaN123", .expected_tail = "123"},
      {.input = "  NAN_tail", .expected_tail = "_tail"},
      {.input = "\tnaN ", .expected_tail = " "},
  };
  static_assert(AllTailsMatchInputs<NanCase>(kCases));

  for (const auto& tc : kCases) {
    char* endptr = nullptr;
    errno = 0;
    const double val = proj_strtod(tc.input, &endptr);
    EXPECT_EQ(0, errno) << "input=\"" << tc.input << "\"";
    EXPECT_TRUE(std::isnan(val)) << "input=\"" << tc.input << "\"";
    ExpectEndptrTail(tc.input, endptr, tc.expected_tail);
  }
}

TEST(ProjStrtodTest, ZeroFormsPreserveSignAndAdvanceEndptr) {
  absl::Cleanup reset_errno = [] { errno = 0; };

  constexpr ZeroCase kCases[] = {
      // Terminated after prefix zeros/underscores (non-decimal early return).
      {.input = "0", .expected_signbit = false, .expected_tail = ""},
      {.input = "0000", .expected_signbit = false, .expected_tail = ""},
      {.input = "_0_", .expected_signbit = false, .expected_tail = ""},
      {.input = "__", .expected_signbit = false, .expected_tail = ""},
      {.input = "+0", .expected_signbit = false, .expected_tail = ""},
      {.input = "-0", .expected_signbit = true, .expected_tail = ""},
      {.input = "-000_0", .expected_signbit = true, .expected_tail = ""},
      {.input = "-__", .expected_signbit = true, .expected_tail = ""},
      {.input = "0 66", .expected_signbit = false, .expected_tail = " 66"},
      {.input = "-0 66", .expected_signbit = true, .expected_tail = " 66"},
      {.input = "0_ ", .expected_signbit = false, .expected_tail = " "},
      {.input = "0_abc", .expected_signbit = false, .expected_tail = "abc"},
      {.input = "-0+5", .expected_signbit = true, .expected_tail = "+5"},
      {.input = "  +00_xyz", .expected_signbit = false, .expected_tail = "xyz"},
      // Decimal zero forms exiting via the zero-integral fractional path.
      {.input = "0.", .expected_signbit = false, .expected_tail = ""},
      {.input = "-0.", .expected_signbit = true, .expected_tail = ""},
      {.input = ".", .expected_signbit = false, .expected_tail = ""},
      {.input = "-.", .expected_signbit = true, .expected_tail = ""},
      {.input = "+.", .expected_signbit = false, .expected_tail = ""},
      {.input = "0.0", .expected_signbit = false, .expected_tail = ""},
      {.input = "-0.0", .expected_signbit = true, .expected_tail = ""},
      {.input = "0._0_0", .expected_signbit = false, .expected_tail = ""},
      {.input = ".000", .expected_signbit = false, .expected_tail = ""},
      {.input = "-.000", .expected_signbit = true, .expected_tail = ""},
      {.input = "0.xyz", .expected_signbit = false, .expected_tail = "xyz"},
      {.input = "-0.xyz", .expected_signbit = true, .expected_tail = "xyz"},
      {.input = "0.0___rest",
       .expected_signbit = false,
       .expected_tail = "rest"},
      {.input = "-0.0___rest",
       .expected_signbit = true,
       .expected_tail = "rest"},
      {.input = "._", .expected_signbit = false, .expected_tail = ""},
  };
  static_assert(AllTailsMatchInputs<ZeroCase>(kCases));

  for (const auto& tc : kCases) {
    char* endptr = nullptr;
    errno = 0;
    const double val = proj_strtod(tc.input, &endptr);
    EXPECT_EQ(0, errno) << "input=\"" << tc.input << "\"";
    EXPECT_DOUBLE_EQ(0.0, val) << "input=\"" << tc.input << "\"";
    EXPECT_EQ(tc.expected_signbit, std::signbit(val))
        << "input=\"" << tc.input << "\"";
    ExpectEndptrTail(tc.input, endptr, tc.expected_tail);
  }
}

TEST(ProjStrtodTest, ValidIntegersDecimalsAndUnderscores) {
  absl::Cleanup reset_errno = [] { errno = 0; };

  constexpr DoubleParseCase kCases[] = {
      {.input = "1", .expected = 1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "-1", .expected = -1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "+42", .expected = 42.0, .tol = 0.0, .expected_tail = ""},
      {.input = "  _123", .expected = 123.0, .tol = 0.0, .expected_tail = ""},
      {.input = "\t\n\r\v\f -3.5_0rest",
       .expected = -3.5,
       .tol = 1e-15,
       .expected_tail = "rest"},
      {.input = "6_098_907",
       .expected = 6098907.0,
       .tol = 0.0,
       .expected_tail = ""},
      {.input = "1______", .expected = 1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "-1_000_000_000_000",
       .expected = -1e12,
       .tol = 0.0,
       .expected_tail = ""},
      // Dot with no digits or non-numeric trailing characters.
      {.input = "1.", .expected = 1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "-1.", .expected = -1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "1.xyz", .expected = 1.0, .tol = 0.0, .expected_tail = "xyz"},
      {.input = "-2.abc", .expected = -2.0, .tol = 0.0, .expected_tail = "abc"},
      {.input = "1..2", .expected = 1.0, .tol = 0.0, .expected_tail = ".2"},
      // Comma is never treated as a decimal separator (LC_NUMERIC=C semantics).
      {.input = "1,5", .expected = 1.0, .tol = 0.0, .expected_tail = ",5"},
      {.input = "1.+2", .expected = 1.0, .tol = 0.0, .expected_tail = "+2"},
      {.input = "123abc",
       .expected = 123.0,
       .tol = 0.0,
       .expected_tail = "abc"},
      {.input = "-123abc",
       .expected = -123.0,
       .tol = 0.0,
       .expected_tail = "abc"},
      // Zero-tailed fractions restore `integral_part` instead of scaling.
      {.input = "1.0", .expected = 1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "-1.0", .expected = -1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "1._", .expected = 1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "1._5", .expected = 1.5, .tol = 1e-15, .expected_tail = ""},
      {.input = "4321.00000000000000000000",
       .expected = 4321.0,
       .tol = 0.0,
       .expected_tail = ""},
      {.input = "-4321.00000000000000000000",
       .expected = -4321.0,
       .tol = 0.0,
       .expected_tail = ""},
      {.input = "1.0___", .expected = 1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "-1.0___", .expected = -1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "1_.0", .expected = 1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "1.__0", .expected = 1.0, .tol = 0.0, .expected_tail = ""},
      // Underscores in the fraction do not shift decimal place values.
      {.input = "1.__5", .expected = 1.5, .tol = 1e-15, .expected_tail = ""},
      {.input = "1.0_5", .expected = 1.05, .tol = 1e-15, .expected_tail = ""},
      {.input = "+_0_._5", .expected = 0.5, .tol = 1e-15, .expected_tail = ""},
      {.input = "-.5", .expected = -0.5, .tol = 1e-15, .expected_tail = ""},
      {.input = ".25", .expected = 0.25, .tol = 1e-15, .expected_tail = ""},
      {.input = "0.00002",
       .expected = 0.00002,
       .tol = 1e-18,
       .expected_tail = ""},
      {.input = "-0.00002",
       .expected = -0.00002,
       .tol = 1e-18,
       .expected_tail = ""},
      {.input = "0._0_0_5",
       .expected = 0.005,
       .tol = 1e-16,
       .expected_tail = ""},
      {.input = "6_098_907.825_0",
       .expected = 6098907.825,
       .tol = 1e-9,
       .expected_tail = ""},
      {.input = "__123_456_789_._10_11_12",
       .expected = 123456789.101112,
       .tol = 1e-7,
       .expected_tail = ""},
      // Truncates fractional accumulation past 17 total digits to avoid loss.
      {.input = "1.234567890123456789012345",
       .expected = 1.23456789012345678,
       .tol = 1e-15,
       .expected_tail = ""},
      // Leading fractional zeros preserve precision down to the pow() branch.
      {.input = "0.00000000000000000001",
       .expected = 1e-20,
       .tol = 1e-34,
       .expected_tail = ""},
      {.input =
           "0.0000000000000000000000000000000000000000000000000000000000000"
           "000000000002",
       .expected = 2e-73,
       .tol = 1e-86,
       .expected_tail = ""},
  };
  static_assert(AllTailsMatchInputs<DoubleParseCase>(kCases));

  for (const auto& tc : kCases) {
    char* endptr = nullptr;
    errno = 0;
    const double val = proj_strtod(tc.input, &endptr);
    EXPECT_EQ(0, errno) << "input=\"" << tc.input << "\"";
    EXPECT_NEAR(tc.expected, val, tc.tol) << "input=\"" << tc.input << "\"";
    ExpectEndptrTail(tc.input, endptr, tc.expected_tail);
  }
}

TEST(ProjStrtodTest, ScientificNotationAndPowThresholdBranches) {
  absl::Cleanup reset_errno = [] { errno = 0; };

  constexpr DoubleParseCase kCases[] = {
      {.input = "1e2", .expected = 100.0, .tol = 0.0, .expected_tail = ""},
      {.input = "1E2", .expected = 100.0, .tol = 0.0, .expected_tail = ""},
      {.input = "1.e2", .expected = 100.0, .tol = 0.0, .expected_tail = ""},
      // Net exponent == 0 hits fast return without scaling.
      {.input = "1.5e1", .expected = 15.0, .tol = 0.0, .expected_tail = ""},
      {.input = "42e0", .expected = 42.0, .tol = 0.0, .expected_tail = ""},
      {.input = "-4321.0000e0",
       .expected = -4321.0,
       .tol = 0.0,
       .expected_tail = ""},
      {.input = "0.0001e4", .expected = 1.0, .tol = 0.0, .expected_tail = ""},
      // Signed exponent with no digits parses exponent 0.
      {.input = "1e+", .expected = 1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "1e-", .expected = 1.0, .tol = 0.0, .expected_tail = ""},
      {.input = "2.5e+3", .expected = 2500.0, .tol = 0.0, .expected_tail = ""},
      {.input = "2.5e-3",
       .expected = 0.0025,
       .tol = 1e-17,
       .expected_tail = ""},
      {.input = "1___e__2__",
       .expected = 100.0,
       .tol = 0.0,
       .expected_tail = ""},
      {.input = "1e_+_2", .expected = 100.0, .tol = 0.0, .expected_tail = ""},
      {.input = "-1e__-_2__rest",
       .expected = -0.01,
       .tol = 1e-16,
       .expected_tail = "rest"},
      {.input = "1e1_2", .expected = 1e12, .tol = 0.0, .expected_tail = ""},
      {.input = "1e-1_0", .expected = 1e-10, .tol = 1e-24, .expected_tail = ""},
      {.input = "-0.00001e-2",
       .expected = -1e-7,
       .tol = 1e-21,
       .expected_tail = ""},
      {.input = "-0.00001e2",
       .expected = -1e-3,
       .tol = 1e-17,
       .expected_tail = ""},
      // Stray 'e'/'E' stops before the exponent marker.
      {.input = "100e", .expected = 100.0, .tol = 0.0, .expected_tail = "e"},
      {.input = "100E", .expected = 100.0, .tol = 0.0, .expected_tail = "E"},
      {.input = "100elephants",
       .expected = 100.0,
       .tol = 0.0,
       .expected_tail = "elephants"},
      {.input = "100Exyz",
       .expected = 100.0,
       .tol = 0.0,
       .expected_tail = "Exyz"},
      // Threshold around abs(exponent) == 20 (loop vs std::pow).
      {.input = "1e19", .expected = 1e19, .tol = 1e5, .expected_tail = ""},
      {.input = "1e-19", .expected = 1e-19, .tol = 1e-33, .expected_tail = ""},
      {.input = "1e20", .expected = 1e20, .tol = 1e6, .expected_tail = ""},
      {.input = "-1e20", .expected = -1e20, .tol = 1e6, .expected_tail = ""},
      {.input = "1e-20", .expected = 1e-20, .tol = 1e-34, .expected_tail = ""},
      {.input = "-1e-20",
       .expected = -1e-20,
       .tol = 1e-34,
       .expected_tail = ""},
      {.input = "3.14e100",
       .expected = 3.14e100,
       .tol = 1e86,
       .expected_tail = ""},
      {.input = "3.14e-100",
       .expected = 3.14e-100,
       .tol = 1e-114,
       .expected_tail = ""},
      {.input = "1e308", .expected = 1e308, .tol = 1e294, .expected_tail = ""},
      {.input = "1e-300",
       .expected = 1e-300,
       .tol = 1e-314,
       .expected_tail = ""},
  };
  static_assert(AllTailsMatchInputs<DoubleParseCase>(kCases));

  for (const auto& tc : kCases) {
    char* endptr = nullptr;
    errno = 0;
    const double val = proj_strtod(tc.input, &endptr);
    EXPECT_EQ(0, errno) << "input=\"" << tc.input << "\"";
    EXPECT_TRUE(std::isfinite(val)) << "input=\"" << tc.input << "\"";
    EXPECT_NEAR(tc.expected, val, tc.tol) << "input=\"" << tc.input << "\"";
    ExpectEndptrTail(tc.input, endptr, tc.expected_tail);
  }

  // Exponents at DBL_MIN_EXP (-1021) / DBL_MAX_EXP (1024) pass the pre-pow
  // range guard and underflow/overflow inside pow().
  {
    constexpr char kUnderflow[] = "1e-1021";
    char* endptr = nullptr;
    errno = 0;
    const double val = proj_strtod(kUnderflow, &endptr);
    EXPECT_DOUBLE_EQ(0.0, val);
    EXPECT_FALSE(std::signbit(val));
    ExpectEndptrTail(kUnderflow, endptr, "");
  }
  {
    constexpr char kNegUnderflow[] = "-1e-1021";
    char* endptr = nullptr;
    errno = 0;
    const double val = proj_strtod(kNegUnderflow, &endptr);
    EXPECT_DOUBLE_EQ(0.0, val);
    EXPECT_TRUE(std::signbit(val));
    ExpectEndptrTail(kNegUnderflow, endptr, "");
  }
  {
    constexpr char kOverflow[] = "1e1024";
    char* endptr = nullptr;
    errno = 0;
    const double val = proj_strtod(kOverflow, &endptr);
    EXPECT_EQ(HUGE_VAL, val);
    ExpectEndptrTail(kOverflow, endptr, "");
  }
}

TEST(ProjStrtodTest, InvalidDigitlessAndExponentErrorsReturnHugeVal) {
  absl::Cleanup reset_errno = [] { errno = 0; };

  constexpr ErrorCase kCases[] = {
      // Zero-digit mantissas reaching the exponent / sign check set EINVAL.
      {.input = "0e5", .expected_errno = EINVAL, .expected_tail = "e5"},
      {.input = "0E-2", .expected_errno = EINVAL, .expected_tail = "E-2"},
      {.input = "-0e3", .expected_errno = EINVAL, .expected_tail = "e3"},
      {.input = ".e5", .expected_errno = EINVAL, .expected_tail = "e5"},
      {.input = ".+", .expected_errno = EINVAL, .expected_tail = "+"},
      {.input = ".-", .expected_errno = EINVAL, .expected_tail = "-"},
      {.input = "._e5", .expected_errno = EINVAL, .expected_tail = "e5"},
      {.input = "0.e5", .expected_errno = EINVAL, .expected_tail = "e5"},
      {.input = "0.0e5", .expected_errno = EINVAL, .expected_tail = "e5"},
      // Underscore(s) after 'e'/'E' followed by non-sign, non-digit fail.
      {.input = "1e_", .expected_errno = 0, .expected_tail = ""},
      {.input = "1e_abc", .expected_errno = 0, .expected_tail = "abc"},
      {.input = "2.5E__xyz", .expected_errno = 0, .expected_tail = "xyz"},
      // Exponents outside [DBL_MIN_EXP, DBL_MAX_EXP] set ERANGE.
      {.input = "1e1025", .expected_errno = ERANGE, .expected_tail = ""},
      {.input = "1e9999", .expected_errno = ERANGE, .expected_tail = ""},
      {.input = "-1e9999", .expected_errno = ERANGE, .expected_tail = ""},
      {.input = "1e-1022", .expected_errno = ERANGE, .expected_tail = ""},
      {.input = "1.5e1026", .expected_errno = ERANGE, .expected_tail = ""},
      {.input = "0.0001e-1018", .expected_errno = ERANGE, .expected_tail = ""},
      {.input = "1e-9999", .expected_errno = ERANGE, .expected_tail = ""},
  };
  static_assert(AllTailsMatchInputs<ErrorCase>(kCases));

  for (const auto& tc : kCases) {
    char* endptr = nullptr;
    errno = 0;
    const double val = proj_strtod(tc.input, &endptr);
    EXPECT_EQ(tc.expected_errno, errno) << "input=\"" << tc.input << "\"";
    EXPECT_EQ(HUGE_VAL, val) << "input=\"" << tc.input << "\"";
    ExpectEndptrTail(tc.input, endptr, tc.expected_tail);
  }
}

TEST(ProjStrtodTest, ProjAtofMatchesProjStrtodAcrossValidAndErrorInputs) {
  absl::Cleanup reset_errno = [] { errno = 0; };

  struct AtofCase {
    const char* input;
    double expected;
    double tol;
    int expected_errno;
  };
  constexpr AtofCase kCases[] = {
      {.input = "", .expected = 0.0, .tol = 0.0, .expected_errno = 0},
      {.input = "   ", .expected = 0.0, .tol = 0.0, .expected_errno = 0},
      {.input = "abc", .expected = 0.0, .tol = 0.0, .expected_errno = 0},
      {.input = "+", .expected = 0.0, .tol = 0.0, .expected_errno = 0},
      {.input = "0", .expected = 0.0, .tol = 0.0, .expected_errno = 0},
      {.input = "42.5m", .expected = 42.5, .tol = 1e-14, .expected_errno = 0},
      {.input = "6_098_907.825_0",
       .expected = 6098907.825,
       .tol = 1e-9,
       .expected_errno = 0},
      {.input = "-1.5e-3",
       .expected = -0.0015,
       .tol = 1e-17,
       .expected_errno = 0},
      {.input = "100elephants",
       .expected = 100.0,
       .tol = 0.0,
       .expected_errno = 0},
      {.input = "0.00000000000000000001",
       .expected = 1e-20,
       .tol = 1e-34,
       .expected_errno = 0},
      {.input = "0e5",
       .expected = HUGE_VAL,
       .tol = 0.0,
       .expected_errno = EINVAL},
      {.input = "1e_", .expected = HUGE_VAL, .tol = 0.0, .expected_errno = 0},
      {.input = "1e9999",
       .expected = HUGE_VAL,
       .tol = 0.0,
       .expected_errno = ERANGE},
  };
  static_assert(std::size(kCases) > 0);

  for (const auto& tc : kCases) {
    errno = 0;
    const double val = proj_atof(tc.input);  // NOLINT(runtime/deprecated_fn)
    EXPECT_EQ(tc.expected_errno, errno) << "input=\"" << tc.input << "\"";
    if (std::isinf(tc.expected)) {
      EXPECT_EQ(tc.expected, val) << "input=\"" << tc.input << "\"";
    } else {
      EXPECT_NEAR(tc.expected, val, tc.tol) << "input=\"" << tc.input << "\"";
    }
  }

  errno = 0;
  EXPECT_TRUE(std::isnan(proj_atof("NaN")));  // NOLINT(runtime/deprecated_fn)
  EXPECT_EQ(0, errno);

  errno = 0;
  const double neg_zero = proj_atof("-0");  // NOLINT(runtime/deprecated_fn)
  EXPECT_DOUBLE_EQ(0.0, neg_zero);
  EXPECT_TRUE(std::signbit(neg_zero));
}

TEST(ProjStrtodTest, NullEndptrIsSafeAcrossAllBranches) {
  absl::Cleanup reset_errno = [] { errno = 0; };

  constexpr const char* kInputs[] = {
      "",    "   ", "NaN", "abc", "+",   "0",
      "123", "1.",  "0e5", "1e_", "1e2", "1e9999",
  };
  static_assert(std::size(kInputs) > 0);

  for (const char* input : kInputs) {
    errno = 0;
    const double via_null_endptr = proj_strtod(input, nullptr);
    const int errno_null_endptr = errno;

    char* endptr = nullptr;
    errno = 0;
    const double via_endptr = proj_strtod(input, &endptr);
    const int errno_endptr = errno;

    EXPECT_EQ(errno_endptr, errno_null_endptr) << "input=\"" << input << "\"";
    if (std::isnan(via_endptr)) {
      EXPECT_TRUE(std::isnan(via_null_endptr)) << "input=\"" << input << "\"";
    } else {
      EXPECT_EQ(via_endptr, via_null_endptr) << "input=\"" << input << "\"";
      EXPECT_EQ(std::signbit(via_endptr), std::signbit(via_null_endptr))
          << "input=\"" << input << "\"";
    }
  }
}

// Note: lines 161-163 of proj/src/apps/proj_strtod.cpp (the fallback in
// `switch (*p)`'s `default:` branch) are unreachable because line 142 already
// restricts `*p` to `"0123456789+-._"` and line 128 excludes `'\0'`. Lines
// 331-455 (`#ifdef TEST`) are not compiled into `:proj4`.

}  // namespace
}  // namespace proj
