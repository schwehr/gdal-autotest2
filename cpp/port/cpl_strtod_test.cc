// Copyright 2015 Google LLC
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
// Tests for GDAL's C strtod API:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_strtod.cpp

#include <cmath>
#include <cstring>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

TEST(CplStrtodTest, CPLAtofDelim) {
  EXPECT_DOUBLE_EQ(-1.2, CPLAtofDelim("-1=2", '='));
}

TEST(CplStrtodTest, CPLAtof) { EXPECT_DOUBLE_EQ(-1.2, CPLAtof("-1.2")); }

TEST(CplStrtodTest, AtofM) {
  // Test conversion to double using multi-lingual / locale processing.
  struct TestCase {
    const char* input;
    double expected;
  };
  constexpr TestCase kCases[] = {
      {.input = "0", .expected = 0.0},
      {.input = "6", .expected = 6.0},
      {.input = "1.", .expected = 1.0},
      {.input = "1.2", .expected = 1.2},
      {.input = "+1.3", .expected = 1.3},
      {.input = "-2", .expected = -2.0},
      {.input = "-3,", .expected = -3.0},
      {.input = "-4,5", .expected = -4.5},
      {.input = "1,", .expected = 1.0},
      {.input = "1,2", .expected = 1.2},
      {.input = "+1,3", .expected = 1.3},
      {.input = "1.23456789123456789", .expected = 1.23456789123456789},
      // String with >= 50 characters before any '.' or ',' or '\0'.
      {.input = "                                                  1.5",
       .expected = 1.5},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_EQ(tc.expected, CPLAtofM(tc.input));
  }
}

TEST(CplStrtodTest, StrtodM) {
  struct TestCase {
    const char* input;
    double expected;
    const char* expected_tail;
  };
  constexpr TestCase kCases[] = {
      {.input = "1.25abc", .expected = 1.25, .expected_tail = "abc"},
      {.input = "1,25xyz", .expected = 1.25, .expected_tail = "xyz"},
      {.input = "42", .expected = 42.0, .expected_tail = ""},
      // String with >= 50 characters before any '.' or ',' or '\0'.
      {.input = "                                                  2.5tail",
       .expected = 2.5,
       .expected_tail = "tail"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    char* end = nullptr;
    EXPECT_EQ(tc.expected, CPLStrtodM(tc.input, &end));
    ASSERT_NE(nullptr, end);
    EXPECT_STREQ(tc.expected_tail, end);
  }
}

TEST(CplStrtodTest, StrtodDelim) {
  struct DelimCase {
    const char* input;
    char delim;
    double expected;
  };
  constexpr DelimCase kDelimCases[] = {
      {.input = "0", .delim = '.', .expected = 0.0},
      {.input = "1", .delim = '.', .expected = 1.0},
      {.input = "2z3", .delim = 'z', .expected = 2.3},
      // Insane, but works.
      {.input = "405", .delim = '0', .expected = 4.5},
      {.input = "1.23456789123456789",
       .delim = '.',
       .expected = 1.23456789123456789},
  };
  for (const auto& tc : kDelimCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_EQ(tc.expected, CPLStrtodDelim(tc.input, nullptr, tc.delim));
  }

  char str[] = "12.34abc";
  char* end = nullptr;
  EXPECT_EQ(12.34, CPLStrtodDelim(str, &end, '.'));
  EXPECT_EQ('a', *end);

  // Leading spaces and locale decimal point in input when using custom point.
  end = nullptr;
  EXPECT_EQ(1.25, CPLStrtodDelim("   1,25.99", &end, ','));
  ASSERT_NE(nullptr, end);
  EXPECT_STREQ(".99", end);

  constexpr const char* kNanStrings[] = {
      "1.#QNAN", "-1.#QNAN",       "-1.#QNAN0000000", "1.#QNAN0000000",
      "-1.#IND", "-1.#IND0000000", "1.#SNAN",         "nan",
  };
  for (const char* s : kNanStrings) {
    SCOPED_TRACE(s);
    EXPECT_TRUE(std::isnan(CPLStrtodDelim(s, nullptr, '.')));
    end = nullptr;
    EXPECT_TRUE(std::isnan(CPLStrtodDelim(s, &end, '.')));
    EXPECT_EQ(s + std::strlen(s), end);
  }

  struct InfCase {
    const char* input;
    bool negative;
  };
  constexpr InfCase kInfCases[] = {
      {.input = "inf", .negative = false},
      {.input = "-inf", .negative = true},
      {.input = "1.#INF", .negative = false},
  };
  for (const auto& tc : kInfCases) {
    SCOPED_TRACE(tc.input);
    const double val_null = CPLStrtodDelim(tc.input, nullptr, '.');
    EXPECT_TRUE(std::isinf(val_null));
    EXPECT_EQ(tc.negative, val_null < 0.0);

    end = nullptr;
    const double val_end = CPLStrtodDelim(tc.input, &end, '.');
    EXPECT_TRUE(std::isinf(val_end));
    EXPECT_EQ(tc.negative, val_end < 0.0);
    EXPECT_EQ(tc.input + std::strlen(tc.input), end);
  }
}

TEST(CplStrtodTest, Strtod) {
  // CPLStrtod is StrtoDelim with '.' as the delimiter.
  struct TestCase {
    const char* input;
    double expected;
  };
  constexpr TestCase kCases[] = {
      {.input = "0", .expected = 0.0},
      {.input = "-1.0", .expected = -1.0},
      {.input = "2.3", .expected = 2.3},
      {.input = "-123456789.123456789123456789",
       .expected = -123456789.123456789123456789},
      {.input = "-9:8", .expected = -9.0},
      {.input = "-8,7", .expected = -8.0},
      {.input = "bad wolf", .expected = 0.0},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_EQ(tc.expected, CPLStrtod(tc.input, nullptr));
  }
}

TEST(CplStrtodTest, CPLStrtofDelim) {
  struct TestCase {
    const char* input;
    char delim;
    float expected;
  };
  constexpr TestCase kCases[] = {
      {.input = "0", .delim = '.', .expected = 0.0f},
      {.input = "1.2", .delim = '.', .expected = 1.2f},
      {.input = "-3a4", .delim = 'a', .expected = -3.4f},
      {.input = "-3", .delim = '-', .expected = 0.3f},
      {.input = "5+6", .delim = '+', .expected = 5.6f},
      {.input = "789", .delim = '8', .expected = 7.9f},
      {.input = "2 9", .delim = ' ', .expected = 2.9f},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_FLOAT_EQ(tc.expected, CPLStrtofDelim(tc.input, nullptr, tc.delim));
  }

  const float value = CPLStrtof("-123456789.123456789123456789", nullptr);
  EXPECT_EQ(-123456789.123456789123456789f, value);

  char* end = nullptr;
  EXPECT_FLOAT_EQ(3.5f, CPLStrtofDelim("3,5rest", &end, ','));
  ASSERT_NE(nullptr, end);
  EXPECT_STREQ("rest", end);

  end = nullptr;
  EXPECT_FLOAT_EQ(4.25f, CPLStrtof("4.25end", &end));
  ASSERT_NE(nullptr, end);
  EXPECT_STREQ("end", end);
}

}  // namespace
}  // namespace autotest2
