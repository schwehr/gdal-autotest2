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
// Unit tests for libharp internal CSV element parsers (libharp/harp-csv.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-csv.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-csv.h

#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::HasSubstr;

TEST(HarpCsvTest, ParseDoubleSuccessAndCursorAdvancement) {
  std::string line = "1.5,  -2.25e2, 42";
  char* cursor = line.data();
  double value = 0.0;

  ASSERT_EQ(HARP_SUCCESS, harp_csv_parse_double(&cursor, &value));
  EXPECT_DOUBLE_EQ(1.5, value);

  ASSERT_EQ(HARP_SUCCESS, harp_csv_parse_double(&cursor, &value));
  EXPECT_DOUBLE_EQ(-225.0, value);

  ASSERT_EQ(HARP_SUCCESS, harp_csv_parse_double(&cursor, &value));
  EXPECT_DOUBLE_EQ(42.0, value);
  EXPECT_EQ('\0', *cursor);
}

TEST(HarpCsvTest, ParseDoubleInvalidInputSetsErrorAndNan) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  struct BadDoubleCase {
    std::string_view input;
  };
  constexpr BadDoubleCase kBadCases[] = {
      {.input = "abc,1.0"},
      {.input = ",1.0"},
      {.input = "   "},
      {.input = ""},
  };

  for (const auto& tc : kBadCases) {
    harp_set_error(HARP_SUCCESS, nullptr);
    std::string buf(tc.input);
    char* cursor = buf.data();
    double value = 123.0;
    EXPECT_EQ(-1, harp_csv_parse_double(&cursor, &value));
    EXPECT_EQ(HARP_ERROR_INVALID_FORMAT, harp_errno);
    EXPECT_EQ(1, harp_isnan(value));
    EXPECT_THAT(harp_errno_to_string(HARP_ERROR_INVALID_FORMAT),
                HasSubstr("could not parse floating point value"));
  }
}

TEST(HarpCsvTest, ParseLongSuccessAndCursorAdvancement) {
  std::string line = "0,   -17, 987654321";
  char* cursor = line.data();
  long value = -1;

  ASSERT_EQ(HARP_SUCCESS, harp_csv_parse_long(&cursor, &value));
  EXPECT_EQ(0L, value);

  ASSERT_EQ(HARP_SUCCESS, harp_csv_parse_long(&cursor, &value));
  EXPECT_EQ(-17L, value);

  ASSERT_EQ(HARP_SUCCESS, harp_csv_parse_long(&cursor, &value));
  EXPECT_EQ(987654321L, value);
  EXPECT_EQ('\0', *cursor);
}

TEST(HarpCsvTest, ParseLongInvalidInputSetsError) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  struct BadLongCase {
    std::string_view input;
  };
  constexpr BadLongCase kBadCases[] = {
      {.input = "xyz,10"},
      {.input = ",10"},
      {.input = "   "},
      {.input = ""},
  };

  for (const auto& tc : kBadCases) {
    harp_set_error(HARP_SUCCESS, nullptr);
    std::string buf(tc.input);
    char* cursor = buf.data();
    long value = 99L;
    EXPECT_EQ(-1, harp_csv_parse_long(&cursor, &value));
    EXPECT_EQ(HARP_ERROR_INVALID_FORMAT, harp_errno);
    EXPECT_EQ(0L, value);
    EXPECT_THAT(harp_errno_to_string(HARP_ERROR_INVALID_FORMAT),
                HasSubstr("could not parse long value"));
  }
}

TEST(HarpCsvTest, ParseStringUnquotedAndQuotedSuccess) {
  // Quoted elements require a trailing comma in harp_csv_parse_string; unquoted
  // elements accept either a comma or end-of-string and trim leading spaces.
  std::string line = "\"hello, world\",  alpha,,beta";
  char* cursor = line.data();
  char* value = nullptr;

  ASSERT_EQ(HARP_SUCCESS, harp_csv_parse_string(&cursor, &value));
  EXPECT_STREQ("hello, world", value);

  ASSERT_EQ(HARP_SUCCESS, harp_csv_parse_string(&cursor, &value));
  EXPECT_STREQ("alpha", value);

  ASSERT_EQ(HARP_SUCCESS, harp_csv_parse_string(&cursor, &value));
  EXPECT_STREQ("", value);

  ASSERT_EQ(HARP_SUCCESS, harp_csv_parse_string(&cursor, &value));
  EXPECT_STREQ("beta", value);
  EXPECT_EQ('\0', *cursor);
}

TEST(HarpCsvTest, ParseStringQuotedErrors) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  struct BadStringCase {
    std::string_view input;
    std::string_view expected_msg;
  };
  constexpr BadStringCase kBadCases[] = {
      {.input = "\"unterminated", .expected_msg = "missing closing '\"'"},
      {.input = "\"abc\"x,next",
       .expected_msg = "trailing characters after closing '\"'"},
      // A quoted element at end-of-line without a trailing comma also triggers
      // the trailing-character check because cursor[len + 1] is '\0' != ','.
      {.input = "\"abc\"",
       .expected_msg = "trailing characters after closing '\"'"},
  };

  for (const auto& tc : kBadCases) {
    harp_set_error(HARP_SUCCESS, nullptr);
    std::string buf(tc.input);
    char* cursor = buf.data();
    char* value = nullptr;
    EXPECT_EQ(-1, harp_csv_parse_string(&cursor, &value));
    EXPECT_EQ(HARP_ERROR_INVALID_FORMAT, harp_errno);
    EXPECT_THAT(harp_errno_to_string(HARP_ERROR_INVALID_FORMAT),
                HasSubstr(tc.expected_msg));
  }
}

TEST(HarpCsvTest, ParseVariableNameAndUnitSuccess) {
  std::string line =
      "  temperature [K]  , O3_column_number_density, pressure  [hPa]";
  char* cursor = line.data();
  char* name = nullptr;
  char* unit = nullptr;

  ASSERT_EQ(HARP_SUCCESS,
            harp_csv_parse_variable_name_and_unit(&cursor, &name, &unit));
  EXPECT_STREQ("temperature", name);
  ASSERT_NE(nullptr, unit);
  EXPECT_STREQ("K", unit);

  ASSERT_EQ(HARP_SUCCESS,
            harp_csv_parse_variable_name_and_unit(&cursor, &name, &unit));
  EXPECT_STREQ("O3_column_number_density", name);
  EXPECT_EQ(nullptr, unit);

  ASSERT_EQ(HARP_SUCCESS,
            harp_csv_parse_variable_name_and_unit(&cursor, &name, &unit));
  EXPECT_STREQ("pressure", name);
  ASSERT_NE(nullptr, unit);
  EXPECT_STREQ("hPa", unit);
  EXPECT_EQ('\0', *cursor);
}

TEST(HarpCsvTest, ParseVariableNameAndUnitInvalidInputs) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  struct BadVarCase {
    std::string_view input;
    std::string_view expected_msg;
  };
  constexpr BadVarCase kBadCases[] = {
      {.input = "123_bad", .expected_msg = "is not an identifier"},
      {.input = "bad-name [K]", .expected_msg = "is not an identifier"},
      {.input = "", .expected_msg = "is not an identifier"},
      {.input = "temp K", .expected_msg = "invalid unit"},
      {.input = "temp ", .expected_msg = "invalid unit"},
      {.input = "temp [K", .expected_msg = "invalid unit"},
      {.input = "temp [K] extra",
       .expected_msg = "invalid trailing characters"},
  };

  for (const auto& tc : kBadCases) {
    harp_set_error(HARP_SUCCESS, nullptr);
    std::string buf(tc.input);
    char* cursor = buf.data();
    char* name = nullptr;
    char* unit = nullptr;
    EXPECT_EQ(-1, harp_csv_parse_variable_name_and_unit(&cursor, &name, &unit));
    EXPECT_EQ(HARP_ERROR_INVALID_FORMAT, harp_errno);
    EXPECT_THAT(harp_errno_to_string(HARP_ERROR_INVALID_FORMAT),
                HasSubstr(tc.expected_msg));
  }
}

}  // namespace
}  // namespace stcorp_harp
