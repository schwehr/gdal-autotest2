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
// Unit tests for ISO-19111 internal string, locale-independent numeric parsing,
// sqlite3_snprintf-backed formatting, and decimal-year rounding helpers
// (proj/src/iso19111/internal.cpp). Note: on Linux/google3, toString uses the
// non-_WIN32 sqlite3_snprintf path; tests calling c_locale_stod(const
// std::string&) require -fexceptions, and iso19111/BUILD sets -DFROM_PROJ_CPP
// and linkstatic = True for internal symbols.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/internal.cpp

#include "third_party/proj4/proj/include/proj/internal/internal.hpp"

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

namespace proj {
namespace {

namespace pj_internal = ::osgeo::proj::internal;

using ::testing::ElementsAre;

TEST(InternalTest, ReplaceAllHandlesEmptyNoMatchMultipleAndExpandingPatterns) {
  struct ReplaceCase {
    std::string_view input;
    std::string_view before;
    std::string_view after;
    std::string_view expected;
  };
  constexpr ReplaceCase kCases[] = {
      {.input = "unchanged",
       .before = "",
       .after = "x",
       .expected = "unchanged"},
      {.input = "", .before = "a", .after = "b", .expected = ""},
      {.input = "hello world",
       .before = "xyz",
       .after = "abc",
       .expected = "hello world"},
      {.input = "EPSG:4326",
       .before = ":",
       .after = "/",
       .expected = "EPSG/4326"},
      {.input = "aaaa", .before = "aa", .after = "b", .expected = "bb"},
      // Verify search resumes after the replacement so overlapping needles do
      // not loop infinitely.
      {.input = "aba", .before = "a", .after = "aa", .expected = "aabaa"},
      {.input = "foo-bar-baz",
       .before = "-",
       .after = "",
       .expected = "foobarbaz"},
  };

  for (const auto& tc : kCases) {
    EXPECT_EQ(tc.expected, pj_internal::replaceAll(std::string(tc.input),
                                                   std::string(tc.before),
                                                   std::string(tc.after)))
        << "input=\"" << tc.input << "\", before=\"" << tc.before << "\"";
  }
}

TEST(InternalTest, CiEqualHandlesAllThreeOverloadsAndLengthMismatches) {
  struct EqualCase {
    const char* lhs;
    const char* rhs;
    bool expected;
  };
  constexpr EqualCase kCases[] = {
      {.lhs = "", .rhs = "", .expected = true},
      {.lhs = "EPSG", .rhs = "EPSG", .expected = true},
      {.lhs = "Transverse_Mercator",
       .rhs = "transverse_mercator",
       .expected = true},
      {.lhs = "EPSG:4326", .rhs = "epsg:4326", .expected = true},
      {.lhs = "WGS", .rhs = "WGS84", .expected = false},
      {.lhs = "WGS84", .rhs = "WGS", .expected = false},
      {.lhs = "WGS84", .rhs = "WGS85", .expected = false},
      {.lhs = "abc", .rhs = "abd", .expected = false},
  };

  for (const auto& tc : kCases) {
    const std::string lhs_str(tc.lhs);
    const std::string rhs_str(tc.rhs);
    EXPECT_EQ(tc.expected, pj_internal::ci_equal(lhs_str, rhs_str))
        << tc.lhs << " vs " << tc.rhs;
    EXPECT_EQ(tc.expected, pj_internal::ci_equal(lhs_str, tc.rhs))
        << tc.lhs << " vs " << tc.rhs;
    EXPECT_EQ(tc.expected, pj_internal::ci_equal(tc.lhs, tc.rhs))
        << tc.lhs << " vs " << tc.rhs;
  }
}

TEST(InternalTest, CiLessProvidesStrictWeakCaseInsensitiveOrder) {
  struct LessCase {
    std::string_view lhs;
    std::string_view rhs;
    bool expected;
  };
  constexpr LessCase kCases[] = {
      {.lhs = "", .rhs = "", .expected = false},
      {.lhs = "", .rhs = "a", .expected = true},
      {.lhs = "a", .rhs = "", .expected = false},
      {.lhs = "abc", .rhs = "DEF", .expected = true},
      {.lhs = "ABC", .rhs = "def", .expected = true},
      {.lhs = "def", .rhs = "ABC", .expected = false},
      {.lhs = "AbC", .rhs = "aBc", .expected = false},
      {.lhs = "aBc", .rhs = "AbC", .expected = false},
      {.lhs = "proj", .rhs = "PROJ4", .expected = true},
      {.lhs = "PROJ4", .rhs = "proj", .expected = false},
  };

  for (const auto& tc : kCases) {
    EXPECT_EQ(tc.expected,
              pj_internal::ci_less(std::string(tc.lhs), std::string(tc.rhs)))
        << tc.lhs << " < " << tc.rhs;
  }
}

TEST(InternalTest, ToLowerAndToUpperConvertOnlyAsciiLetters) {
  struct CasePair {
    std::string_view input;
    std::string_view expected_lower;
    std::string_view expected_upper;
  };
  constexpr CasePair kCases[] = {
      {.input = "", .expected_lower = "", .expected_upper = ""},
      {.input = "Epsg:4326_wgs84",
       .expected_lower = "epsg:4326_wgs84",
       .expected_upper = "EPSG:4326_WGS84"},
      // Cover characters immediately adjacent to 'A'..'Z' and 'a'..'z' in
      // ASCII ('@', '[', '`', '{').
      {.input = "@AZ[`az{09",
       .expected_lower = "@az[`az{09",
       .expected_upper = "@AZ[`AZ{09"},
      {.input = "\xC3\xA9",
       .expected_lower = "\xC3\xA9",
       .expected_upper = "\xC3\xA9"},
  };

  for (const auto& tc : kCases) {
    const std::string input(tc.input);
    EXPECT_EQ(tc.expected_lower, pj_internal::tolower(input)) << tc.input;
    EXPECT_EQ(tc.expected_upper, pj_internal::toupper(input)) << tc.input;
  }
}

TEST(InternalTest, StripQuotesRemovesOnlyMatchingOuterDoubleQuotes) {
  struct QuoteCase {
    std::string_view input;
    std::string_view expected;
  };
  constexpr QuoteCase kCases[] = {
      {.input = "", .expected = ""},
      {.input = "\"", .expected = "\""},
      {.input = "a", .expected = "a"},
      {.input = "\"\"", .expected = ""},
      {.input = "\"WGS 84\"", .expected = "WGS 84"},
      {.input = "\"a\"b\"", .expected = "a\"b"},
      {.input = "\"unclosed", .expected = "\"unclosed"},
      {.input = "unopened\"", .expected = "unopened\""},
      {.input = "'single'", .expected = "'single'"},
      {.input = "plain", .expected = "plain"},
  };

  for (const auto& tc : kCases) {
    EXPECT_EQ(tc.expected, pj_internal::stripQuotes(std::string(tc.input)))
        << tc.input;
  }
}

TEST(InternalTest, CiFindHandlesCStringAndOffsetOverloads) {
  struct FindCase {
    std::string_view haystack;
    const char* needle;
    std::size_t expected;
  };
  constexpr FindCase kCstrCases[] = {
      {.haystack = "PROJCS[\"WGS84\"]", .needle = "projcs", .expected = 0},
      {.haystack = "AUTHORITY[\"EPSG\",\"4326\"]",
       .needle = "epsg",
       .expected = 11},
      {.haystack = "EPSG:4326", .needle = "4326", .expected = 5},
      {.haystack = "EPSG:4326", .needle = "ogc", .expected = std::string::npos},
      {.haystack = "abc", .needle = "abcd", .expected = std::string::npos},
      {.haystack = "abc", .needle = "", .expected = 0},
      {.haystack = "", .needle = "", .expected = 0},
      {.haystack = "", .needle = "a", .expected = std::string::npos},
  };

  for (const auto& tc : kCstrCases) {
    EXPECT_EQ(tc.expected,
              pj_internal::ci_find(std::string(tc.haystack), tc.needle))
        << "haystack=\"" << tc.haystack << "\", needle=\"" << tc.needle << "\"";
  }

  const std::string multi = "Foo bar FOO";
  EXPECT_EQ(0u, pj_internal::ci_find(multi, std::string("foo")));
  EXPECT_EQ(8u, pj_internal::ci_find(multi, std::string("foo"), 1));
  EXPECT_EQ(8u, pj_internal::ci_find(multi, std::string("foo"), 8));
  EXPECT_EQ(std::string::npos,
            pj_internal::ci_find(multi, std::string("foo"), 9));
  EXPECT_EQ(std::string::npos,
            pj_internal::ci_find(multi, std::string("foo"), multi.size()));
  EXPECT_EQ(2u, pj_internal::ci_find(std::string("abcd"), std::string(""), 2));
}

TEST(InternalTest, StartsWithCiStartsWithAndEndsWithCheckLengthsAndCase) {
  struct PrefixSuffixCase {
    const char* text;
    const char* pattern;
    bool expected;
  };
  constexpr PrefixSuffixCase kCiPrefixCases[] = {
      {.text = "", .pattern = "", .expected = true},
      {.text = "EPSG:4326", .pattern = "", .expected = true},
      {.text = "EPSG:4326", .pattern = "epsg", .expected = true},
      {.text = "epsg", .pattern = "EPSG", .expected = true},
      {.text = "EP", .pattern = "EPSG", .expected = false},
      {.text = "OGC:CRS84", .pattern = "EPSG", .expected = false},
  };

  for (const auto& tc : kCiPrefixCases) {
    EXPECT_EQ(tc.expected, pj_internal::ci_starts_with(tc.text, tc.pattern))
        << tc.text << " starts_with " << tc.pattern;
    EXPECT_EQ(tc.expected, pj_internal::ci_starts_with(std::string(tc.text),
                                                       std::string(tc.pattern)))
        << tc.text << " starts_with " << tc.pattern;
  }

  constexpr PrefixSuffixCase kEndsWithCases[] = {
      {.text = "", .pattern = "", .expected = true},
      {.text = "grid.tif", .pattern = "", .expected = true},
      {.text = "grid.tif", .pattern = ".tif", .expected = true},
      {.text = "grid.tif", .pattern = "grid.tif", .expected = true},
      {.text = "grid.tif", .pattern = ".TIF", .expected = false},
      {.text = "tif", .pattern = ".tif", .expected = false},
      {.text = "grid.gtx", .pattern = ".tif", .expected = false},
  };

  for (const auto& tc : kEndsWithCases) {
    EXPECT_EQ(tc.expected, pj_internal::ends_with(std::string(tc.text),
                                                  std::string(tc.pattern)))
        << tc.text << " ends_with " << tc.pattern;
  }

  // Exercise inline starts_with helpers declared in internal.hpp.
  EXPECT_TRUE(
      pj_internal::starts_with(std::string("EPSG:4326"), std::string("EPSG")));
  EXPECT_FALSE(
      pj_internal::starts_with(std::string("EPSG:4326"), std::string("epsg")));
  EXPECT_FALSE(
      pj_internal::starts_with(std::string("EP"), std::string("EPSG")));
  EXPECT_TRUE(pj_internal::starts_with(std::string("EPSG:4326"), "EPSG"));
  EXPECT_FALSE(pj_internal::starts_with(std::string("EPSG:4326"), "epsg"));
  EXPECT_FALSE(pj_internal::starts_with(std::string("EP"), "EPSG"));
}

TEST(InternalTest, CLocaleStodParsesFastPathClassicFallbackAndRejectsInvalid) {
  struct ValidDoubleCase {
    std::string_view text;
    double expected;
  };
  constexpr ValidDoubleCase kValidCases[] = {
      // Fast path: 0 < size < 15 with optional sign, digits, and decimal point.
      {.text = "0", .expected = 0.0},
      {.text = "-0", .expected = 0.0},
      {.text = "42", .expected = 42.0},
      {.text = "+42", .expected = 42.0},
      {.text = "-42", .expected = -42.0},
      {.text = "3.14159265", .expected = 3.14159265},
      {.text = "+0.25", .expected = 0.25},
      {.text = "-123.4567", .expected = -123.4567},
      {.text = ".5", .expected = 0.5},
      {.text = "5.", .expected = 5.0},
      {.text = "-.125", .expected = -0.125},
      {.text = "99999999999999", .expected = 99999999999999.0},
      // Slow std::istringstream classic-locale path: exponent or size >= 15.
      {.text = "1.5e2", .expected = 150.0},
      {.text = "-2.5E-3", .expected = -0.0025},
      {.text = "1234567890.1234", .expected = 1234567890.1234},
      {.text = "0.1234567890123456", .expected = 0.1234567890123456},
  };

  for (const auto& tc : kValidCases) {
    bool success = false;
    const std::string input(tc.text);
    const double parsed = pj_internal::c_locale_stod(input, success);
    EXPECT_TRUE(success) << tc.text;
    EXPECT_DOUBLE_EQ(tc.expected, parsed) << tc.text;
    EXPECT_DOUBLE_EQ(tc.expected, pj_internal::c_locale_stod(input)) << tc.text;
  }

  constexpr std::string_view kInvalidInputs[] = {
      "", "abc", "12.34abc", "12.34 ", "1,5", "123456789012345xyz", "1e500",
  };

  for (const std::string_view text : kInvalidInputs) {
    bool success = true;
    const std::string input(text);
    const double parsed = pj_internal::c_locale_stod(input, success);
    EXPECT_FALSE(success) << text;
    EXPECT_DOUBLE_EQ(0.0, parsed) << text;
    EXPECT_THROW((void)pj_internal::c_locale_stod(input), std::invalid_argument)
        << text;
  }
}

TEST(InternalTest, SplitHandlesSingleCharAndMultiCharSeparators) {
  EXPECT_THAT(pj_internal::split("", ','), ElementsAre(""));
  EXPECT_THAT(pj_internal::split("single", ','), ElementsAre("single"));
  EXPECT_THAT(pj_internal::split("a,b,c", ','), ElementsAre("a", "b", "c"));
  EXPECT_THAT(pj_internal::split(",a,,b,", ','),
              ElementsAre("", "a", "", "b", ""));

  const std::string sep = "::";
  EXPECT_THAT(pj_internal::split("", sep), ElementsAre(""));
  EXPECT_THAT(pj_internal::split("a:b", sep), ElementsAre("a:b"));
  EXPECT_THAT(pj_internal::split("a::b::c", sep), ElementsAre("a", "b", "c"));
  EXPECT_THAT(pj_internal::split("::a::::b::", sep),
              ElementsAre("", "a", "", "b", ""));
}

TEST(InternalTest, ToStringFormatsIntegersDoublesAndNineCarryWorkaround) {
  EXPECT_EQ("0", pj_internal::toString(0));
  EXPECT_EQ("42", pj_internal::toString(42));
  EXPECT_EQ("-4326", pj_internal::toString(-4326));
  EXPECT_EQ("2147483647",
            pj_internal::toString(std::numeric_limits<int>::max()));
  EXPECT_EQ("-2147483648",
            pj_internal::toString(std::numeric_limits<int>::min()));

  EXPECT_EQ("0", pj_internal::toString(0.0));
  EXPECT_EQ("0", pj_internal::toString(-0.0));
  EXPECT_EQ("42.5", pj_internal::toString(42.5));
  EXPECT_EQ("-123.75", pj_internal::toString(-123.75));

  // At precision == 15, values containing "9999999999" are re-formatted at
  // %.14g to collapse floating-point representation noise.
  EXPECT_EQ("1.3", pj_internal::toString(1.29999999999999));
  EXPECT_EQ("-1.3", pj_internal::toString(-1.29999999999999));
  EXPECT_EQ("2", pj_internal::toString(1.99999999999999));

  // Other precisions do not trigger the precision == 15 fallback.
  EXPECT_EQ("1.29999999999999", pj_internal::toString(1.29999999999999, 16));
  EXPECT_EQ("123.5", pj_internal::toString(123.456, 4));
  EXPECT_EQ("1e-05", pj_internal::toString(1e-5, 6));
  EXPECT_EQ("1.5e+20", pj_internal::toString(1.5e20));

  EXPECT_EQ("Inf",
            pj_internal::toString(std::numeric_limits<double>::infinity()));
  EXPECT_EQ("-Inf",
            pj_internal::toString(-std::numeric_limits<double>::infinity()));
  EXPECT_EQ("NaN",
            pj_internal::toString(std::numeric_limits<double>::quiet_NaN()));
}

TEST(InternalTest, ConcatJoinGetRoundedEpochAndDownCastBehaveAsSpecified) {
  EXPECT_EQ("EPSG:4326", pj_internal::concat("EPSG:", std::string("4326")));
  EXPECT_EQ("", pj_internal::concat("", std::string("")));
  EXPECT_EQ("[EPSG:4326]",
            pj_internal::concat("[", std::string("EPSG:4326"), "]"));
  EXPECT_EQ("body", pj_internal::concat("", std::string("body"), ""));

  EXPECT_EQ("", pj_internal::join({}, ", "));
  EXPECT_EQ("only", pj_internal::join({"only"}, ", "));
  EXPECT_EQ("a, b, c", pj_internal::join({"a", "b", "c"}, ", "));
  EXPECT_EQ(":mid:", pj_internal::join({"", "mid", ""}, ":"));
  EXPECT_EQ("xy", pj_internal::join({"x", "y"}, ""));

  // Values within 1e-3 of a thousandth of a year snap to exact thousandths.
  EXPECT_DOUBLE_EQ(2015.0, pj_internal::getRoundedEpochInDecimalYear(2015.0));
  EXPECT_DOUBLE_EQ(2010.0,
                   pj_internal::getRoundedEpochInDecimalYear(2010.0 + 5e-7));
  EXPECT_DOUBLE_EQ(2010.0,
                   pj_internal::getRoundedEpochInDecimalYear(2010.0 - 5e-7));
  EXPECT_DOUBLE_EQ(2018.375,
                   pj_internal::getRoundedEpochInDecimalYear(2018.375 + 4e-7));
  EXPECT_DOUBLE_EQ(-2000.5,
                   pj_internal::getRoundedEpochInDecimalYear(-2000.5 + 5e-7));

  // Values further than 1e-3 thousandths from a grid point remain unchanged.
  EXPECT_DOUBLE_EQ(2010.1234,
                   pj_internal::getRoundedEpochInDecimalYear(2010.1234));
  const double unrounded = 2010.0 + 2e-6;
  EXPECT_DOUBLE_EQ(unrounded,
                   pj_internal::getRoundedEpochInDecimalYear(unrounded));

  struct Base {
    virtual ~Base() = default;
  };
  struct Derived final : Base {
    int marker = 7;
  };
  EXPECT_EQ(nullptr,
            pj_internal::down_cast<Derived*>(static_cast<Base*>(nullptr)));
  Derived derived;
  Base* base = &derived;
  Derived* cast_back = pj_internal::down_cast<Derived*>(base);
  ASSERT_NE(nullptr, cast_back);
  EXPECT_EQ(7, cast_back->marker);
}

}  // namespace
}  // namespace proj
