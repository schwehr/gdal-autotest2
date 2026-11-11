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

// Unit tests for third_party/geos/src/deps/ryu/d2s.c.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/deps/ryu/d2s.c

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "ryu/ryu.h"
#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

std::string FormatFixed(double value, uint32_t precision) {
  std::array<char, 64> buf{};
  const int len = geos_d2sfixed_buffered_n(value, precision, buf.data());
  EXPECT_GT(len, 0);
  EXPECT_LT(len, static_cast<int>(buf.size()));
  return std::string(buf.data(), static_cast<size_t>(len));
}

std::string FormatExp(double value, uint32_t precision) {
  std::array<char, 64> buf{};
  const int len = geos_d2sexp_buffered_n(value, precision, buf.data());
  EXPECT_GT(len, 0);
  EXPECT_LT(len, static_cast<int>(buf.size()));
  return std::string(buf.data(), static_cast<size_t>(len));
}

struct FormatCase {
  std::string_view name;
  double value;
  uint32_t precision;
  std::string_view expected;
};

constexpr auto kFixedCases = std::to_array<FormatCase>({
    {.name = "pos_zero", .value = 0.0, .precision = 6, .expected = "0"},
    {.name = "neg_zero", .value = -0.0, .precision = 6, .expected = "0"},
    {.name = "pos_inf",
     .value = std::numeric_limits<double>::infinity(),
     .precision = 6,
     .expected = "Infinity"},
    {.name = "neg_inf",
     .value = -std::numeric_limits<double>::infinity(),
     .precision = 6,
     .expected = "-Infinity"},
    {.name = "nan",
     .value = std::numeric_limits<double>::quiet_NaN(),
     .precision = 6,
     .expected = "NaN"},
    {.name = "small_int", .value = 42.0, .precision = 6, .expected = "42"},
    {.name = "small_int_trailing_zeros",
     .value = 120000.0,
     .precision = 6,
     .expected = "120000"},
    {.name = "neg_small_int", .value = -7.0, .precision = 6, .expected = "-7"},
    {.name = "large_int_64bit",
     .value = 12345678901234.0,
     .precision = 6,
     .expected = "12345678901234"},
    {.name = "exact_half", .value = 0.5, .precision = 6, .expected = "0.5"},
    {.name = "leading_decimal_zeros",
     .value = 123.0001,
     .precision = 6,
     .expected = "123.0001"},
    {.name = "sub_unit_leading_zeros",
     .value = -0.0000456,
     .precision = 10,
     .expected = "-0.0000456"},
    {.name = "trim_to_zero", .value = 0.00123, .precision = 1, .expected = "0"},
    {.name = "neg_trim_to_zero_no_minus",
     .value = -0.00123,
     .precision = 1,
     .expected = "0"},
    {.name = "trim_round_up", .value = 1.26, .precision = 1, .expected = "1.3"},
    {.name = "trim_round_down",
     .value = 1.24,
     .precision = 1,
     .expected = "1.2"},
    {.name = "trim_round_carry_to_int",
     .value = 1.999,
     .precision = 2,
     .expected = "2"},
    {.name = "trim_strip_trailing_zero",
     .value = 1.201,
     .precision = 2,
     .expected = "1.2"},
    {.name = "high_precision_many_digits",
     .value = 123456789.1234567,
     .precision = 16,
     .expected = "123456789.1234567"},
    {.name = "round_half_even_down",
     .value = 1.25,
     .precision = 1,
     .expected = "1.2"},
    {.name = "round_half_even_up",
     .value = 1.75,
     .precision = 1,
     .expected = "1.8"},
});

TEST(D2sTest, FixedFormatting) {
  for (const auto& tc : kFixedCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(FormatFixed(tc.value, tc.precision), tc.expected);
  }
}

TEST(D2sTest, LargeBinaryPowersAndSubnormalsFixed) {
  // Exercises d2d with e2 >= 0 (shortest round-trip decimal representation).
  EXPECT_EQ(FormatFixed(std::ldexp(1.0, 54), 6), "18014398509481984");
  EXPECT_EQ(FormatFixed(std::ldexp(1.0, 60), 6), "1152921504606847000");
  EXPECT_EQ(FormatFixed(std::ldexp(1.0, 80), 6), "1208925819614629200000000");
  // Negative e2 trailing zero branches (e.g. exact binary fractions) &
  // subnormal.
  EXPECT_EQ(FormatFixed(std::ldexp(1.0, -4), 10), "0.0625");
  EXPECT_EQ(FormatFixed(std::ldexp(1.0, -10), 15), "0.0009765625");
  EXPECT_EQ(FormatFixed(0.008, 6), "0.008");

  const std::string sub =
      FormatFixed(std::numeric_limits<double>::denorm_min(), 16);
  EXPECT_EQ(sub, "0");
}

constexpr auto kExpCases = std::to_array<FormatCase>({
    {.name = "pos_zero", .value = 0.0, .precision = 6, .expected = "0"},
    {.name = "neg_zero", .value = -0.0, .precision = 6, .expected = "0"},
    {.name = "pos_inf",
     .value = std::numeric_limits<double>::infinity(),
     .precision = 6,
     .expected = "Infinity"},
    {.name = "neg_inf",
     .value = -std::numeric_limits<double>::infinity(),
     .precision = 6,
     .expected = "-Infinity"},
    {.name = "nan",
     .value = std::numeric_limits<double>::quiet_NaN(),
     .precision = 6,
     .expected = "NaN"},
    {.name = "unit", .value = 1.0, .precision = 6, .expected = "1e+0"},
    {.name = "small_int_trailing_zeros",
     .value = 120000.0,
     .precision = 6,
     .expected = "1.2e+5"},
    {.name = "exp_two_digits_pos",
     .value = 1.5e25,
     .precision = 6,
     .expected = "1.5e+25"},
    {.name = "exp_three_digits_pos",
     .value = -2.75e150,
     .precision = 6,
     .expected = "-2.75e+150"},
    {.name = "exp_single_digit_neg",
     .value = 0.25,
     .precision = 6,
     .expected = "2.5e-1"},
    {.name = "exp_two_digits_neg",
     .value = 1.25e-42,
     .precision = 6,
     .expected = "1.25e-42"},
    {.name = "exp_three_digits_neg",
     .value = 5.5e-200,
     .precision = 6,
     .expected = "5.5e-200"},
});

TEST(D2sTest, ScientificFormatting) {
  for (const auto& tc : kExpCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(FormatExp(tc.value, tc.precision), tc.expected);
  }
}

TEST(D2sTest, SubnormalAndExtremeExponents) {
  const std::string sub =
      FormatExp(std::numeric_limits<double>::denorm_min(), 16);
  EXPECT_EQ(sub, "5e-324");

  const std::string max_dbl = FormatExp(std::numeric_limits<double>::max(), 16);
  EXPECT_EQ(max_dbl.substr(max_dbl.size() - 5), "e+308");

  const std::string min_norm =
      FormatExp(std::numeric_limits<double>::min(), 16);
  EXPECT_EQ(min_norm.substr(min_norm.size() - 5), "e-308");

  // Exercise many values across exponent decades to trigger rare d2d branches.
  for (int exp = -100; exp <= 100; ++exp) {
    const double base = std::ldexp(1.0, exp);
    const std::string out = FormatExp(base * 1.25, 12);
    EXPECT_FALSE(out.empty());
  }
}

}  // namespace
}  // namespace geos_google
