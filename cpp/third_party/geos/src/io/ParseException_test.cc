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

// Unit tests for third_party/geos/src/io/ParseException.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for throw/catch assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/io/ParseException.cpp

#include "third_party/geos/include/geos/io/ParseException.h"

#include <array>
#include <exception>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::io::ParseException;
using geos::util::GEOSException;
using ::testing::AllOf;
using ::testing::EndsWith;
using ::testing::StartsWith;

static_assert(std::is_base_of_v<GEOSException, ParseException>);
static_assert(std::is_base_of_v<std::runtime_error, ParseException>);
static_assert(std::is_base_of_v<std::exception, ParseException>);
static_assert(std::is_copy_constructible_v<ParseException>);
static_assert(std::is_copy_assignable_v<ParseException>);
static_assert(std::is_nothrow_destructible_v<ParseException>);

struct MessageCase {
  std::string_view name;
  std::string_view msg;
  std::string_view expected_what;
};

constexpr auto kMessageCases = std::to_array<MessageCase>({
    {
        .name = "simple_message",
        .msg = "unexpected token",
        .expected_what = "ParseException: unexpected token",
    },
    {
        .name = "empty_message",
        .msg = "",
        .expected_what = "ParseException: ",
    },
    {
        .name = "multiline_and_quoted",
        .msg = "line 1\ncolumn '2'",
        .expected_what = "ParseException: line 1\ncolumn '2'",
    },
});

struct TokenCase {
  std::string_view name;
  std::string_view msg;
  std::string_view var;
  std::string_view expected_what;
};

constexpr auto kTokenCases = std::to_array<TokenCase>({
    {
        .name = "normal_token",
        .msg = "Unknown type",
        .var = "POLYGONZ",
        .expected_what = "ParseException: Unknown type: 'POLYGONZ'",
    },
    {
        .name = "empty_token",
        .msg = "Expected number",
        .var = "",
        .expected_what = "ParseException: Expected number: ''",
    },
    {
        .name = "empty_message",
        .msg = "",
        .var = "EOF",
        .expected_what = "ParseException: : 'EOF'",
    },
    {
        .name = "both_empty",
        .msg = "",
        .var = "",
        .expected_what = "ParseException: : ''",
    },
    {
        .name = "embedded_single_quote",
        .msg = "Unexpected char",
        .var = "'",
        .expected_what = "ParseException: Unexpected char: '''",
    },
});

struct DoubleCase {
  std::string_view name;
  std::string_view msg;
  double num;
  std::string_view expected_what;
};

constexpr auto kDoubleCases = std::to_array<DoubleCase>({
    {
        .name = "zero",
        .msg = "Bad ordinate",
        .num = 0.0,
        .expected_what = "ParseException: Bad ordinate: '0'",
    },
    {
        .name = "positive_integer",
        .msg = "Unexpected dimension",
        .num = 42.0,
        .expected_what = "ParseException: Unexpected dimension: '42'",
    },
    {
        .name = "negative_decimal",
        .msg = "Negative value",
        .num = -7.5,
        .expected_what = "ParseException: Negative value: '-7.5'",
    },
    {
        .name = "fractional_decimal",
        .msg = "Fractional value",
        .num = 3.125,
        .expected_what = "ParseException: Fractional value: '3.125'",
    },
    {
        .name = "six_digit_integer",
        .msg = "Six digit integer",
        .num = 123456.0,
        .expected_what = "ParseException: Six digit integer: '123456'",
    },
    {
        .name = "large_scientific",
        .msg = "Large scientific",
        .num = 1.5e10,
        .expected_what = "ParseException: Large scientific: '1.5e+10'",
    },
    {
        .name = "small_scientific",
        .msg = "Small scientific",
        .num = 1.25e-5,
        .expected_what = "ParseException: Small scientific: '1.25e-05'",
    },
    {
        .name = "empty_message",
        .msg = "",
        .num = 1.5,
        .expected_what = "ParseException: : '1.5'",
    },
});

TEST(ParseExceptionTest, DefaultConstructsWithPrefix) {
  const ParseException ex;
  EXPECT_STREQ(ex.what(), "ParseException: ");
}

TEST(ParseExceptionTest, MessageConstructorFormatsWhat) {
  for (const MessageCase& tc : kMessageCases) {
    SCOPED_TRACE(tc.name);
    const ParseException ex{std::string(tc.msg)};
    EXPECT_EQ(std::string_view(ex.what()), tc.expected_what);
  }

  const std::string long_msg(256, 'x');
  const ParseException long_ex(long_msg);
  EXPECT_EQ(long_ex.what(), "ParseException: " + long_msg);
}

TEST(ParseExceptionTest, StringTokenConstructorQuotesVariable) {
  for (const TokenCase& tc : kTokenCases) {
    SCOPED_TRACE(tc.name);
    const ParseException ex{std::string(tc.msg), std::string(tc.var)};
    EXPECT_EQ(std::string_view(ex.what()), tc.expected_what);
  }
}

TEST(ParseExceptionTest, DoubleConstructorFormatsFiniteValues) {
  for (const DoubleCase& tc : kDoubleCases) {
    SCOPED_TRACE(tc.name);
    const ParseException ex{std::string(tc.msg), tc.num};
    EXPECT_EQ(std::string_view(ex.what()), tc.expected_what);
  }
}

TEST(ParseExceptionTest, DoubleConstructorHandlesExtremeAndNonFiniteValues) {
  const std::array<double, 7> edge_values = {
      std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::min(),
      -0.0,
  };

  for (double value : edge_values) {
    std::ostringstream oss;
    oss << value;
    const ParseException ex("value", value);
    EXPECT_THAT(std::string_view(ex.what()),
                AllOf(StartsWith("ParseException: value: '"), EndsWith("'")));
    EXPECT_EQ(ex.what(), "ParseException: value: '" + oss.str() + "'");
  }
}

TEST(ParseExceptionTest, ThrowsAndCaughtAsBaseExceptionTypes) {
  EXPECT_THROW([] { throw ParseException(); }(), ParseException);
  EXPECT_THROW([] { throw ParseException("bad wkt"); }(), ParseException);
  EXPECT_THROW([] { throw ParseException("bad token", "FOO"); }(),
               ParseException);
  EXPECT_THROW([] { throw ParseException("bad value", 12.5); }(),
               ParseException);

  try {
    throw ParseException("bad wkt", "POINT(");
  } catch (const GEOSException& ex) {
    EXPECT_STREQ(ex.what(), "ParseException: bad wkt: 'POINT('");
  } catch (...) {
    FAIL() << "Expected catch as GEOSException";
  }

  try {
    throw ParseException("bad ordinate", 9.5);
  } catch (const std::runtime_error& ex) {
    EXPECT_STREQ(ex.what(), "ParseException: bad ordinate: '9.5'");
  } catch (...) {
    FAIL() << "Expected catch as std::runtime_error";
  }

  try {
    throw ParseException("generic error");
  } catch (const std::exception& ex) {
    EXPECT_STREQ(ex.what(), "ParseException: generic error");
  } catch (...) {
    FAIL() << "Expected catch as std::exception";
  }
}

TEST(ParseExceptionTest, RethrowPreservesDynamicTypeAndMessage) {
  try {
    try {
      throw ParseException("nested", "TOKEN");
    } catch (const std::exception&) {
      throw;
    }
  } catch (const ParseException& ex) {
    EXPECT_STREQ(ex.what(), "ParseException: nested: 'TOKEN'");
  } catch (...) {
    FAIL() << "Expected rethrown ParseException";
  }
}

TEST(ParseExceptionTest, CopyConstructionAndAssignmentPreserveMessage) {
  const ParseException original("msg", "var");
  const ParseException copied(original);
  EXPECT_STREQ(copied.what(), "ParseException: msg: 'var'");

  ParseException assigned;
  EXPECT_STREQ(assigned.what(), "ParseException: ");
  assigned = original;
  EXPECT_STREQ(assigned.what(), "ParseException: msg: 'var'");
}

TEST(ParseExceptionTest, PolymorphicDestructionThroughBasePointer) {
  std::unique_ptr<GEOSException> geos_err =
      std::make_unique<ParseException>("poly", "tok");
  EXPECT_STREQ(geos_err->what(), "ParseException: poly: 'tok'");

  std::unique_ptr<std::exception> std_err =
      std::make_unique<ParseException>("poly", 2.5);
  EXPECT_STREQ(std_err->what(), "ParseException: poly: '2.5'");

  std::unique_ptr<ParseException> exact_err =
      std::make_unique<ParseException>();
  EXPECT_STREQ(exact_err->what(), "ParseException: ");
}

}  // namespace
}  // namespace geos_google
