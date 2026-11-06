// Copyright 2014 Google Inc. All Rights Reserved.
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
// Tests for GDAL's CPLString class that is derived from STL string.
// Yes, there is also a cpl_string.cc tested in cpl_string_test.cc.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cplstring.cpp

#include <cstdarg>
#include <cstddef>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

TEST(CPLString, ConstructFromStdString) {
  ASSERT_STREQ("abc", CPLString(std::string("abc")).c_str());
}

TEST(CPLString, ConstructFromConstCharStar) {
  ASSERT_STREQ("abc", CPLString("abc").c_str());
}

TEST(CPLString, ConstructWithCount) {
  ASSERT_STREQ("a", CPLString("abc", 1).c_str());
}

// Tests formatting of doubles to a CPLString.
TEST(CPLString, FormatC) {
  CPLString str;
  str.FormatC(0);
  EXPECT_EQ("0", str);
  str.FormatC(1);
  EXPECT_EQ("01", str);
  str.FormatC(-1);
  EXPECT_EQ("01-1", str);
  str.Clear();
  EXPECT_EQ("", str);

  str.FormatC(0, "%0.2f");
  EXPECT_EQ("0.00", str);
  str.Clear();

  EXPECT_EQ("+1.01", str.FormatC(1.012, "%+0.2f"));
  str.Clear();
}

// Tests white space stripping.
TEST(CPLString, Trim) {
  struct TrimCase {
    const char* input;
    const char* expected;
  };
  constexpr TrimCase kCases[] = {
      {.input = "", .expected = ""},
      {.input = " ", .expected = ""},
      {.input = "a ", .expected = "a"},
      {.input = " b", .expected = "b"},
      {.input = " c ", .expected = "c"},
      {.input = " d e ", .expected = "d e"},
      {.input = "\t\n\rfoo", .expected = "foo"},
      {.input = "bar\t\n\r", .expected = "bar"},
  };
  for (const auto& tc : kCases) {
    CPLString str(tc.input);
    EXPECT_EQ(tc.expected, str.Trim()) << tc.input;
  }
}

// Tests case insensitive searching.
TEST(CPLString, ifind) {
  const CPLString str("abc123 Foo BAR");
  struct FindCase {
    const char* needle;
    size_t pos;
    size_t expected;
  };
  constexpr FindCase kCases[] = {
      {.needle = "", .pos = 0, .expected = std::string::npos},
      {.needle = "z", .pos = 0, .expected = std::string::npos},
      {.needle = "3", .pos = 10, .expected = std::string::npos},
      {.needle = "a", .pos = 100, .expected = std::string::npos},
      {.needle = " ", .pos = 0, .expected = 6},
      {.needle = "3", .pos = 2, .expected = 5},
      {.needle = "bC123", .pos = 0, .expected = 1},
      {.needle = "abc", .pos = 0, .expected = 0},
      {.needle = "AbC", .pos = 0, .expected = 0},
      {.needle = "ABC", .pos = 0, .expected = 0},
      {.needle = "foo", .pos = 0, .expected = 7},
      {.needle = "Foo", .pos = 0, .expected = 7},
      {.needle = "FOO", .pos = 0, .expected = 7},
  };
  for (const auto& tc : kCases) {
    EXPECT_EQ(tc.expected, str.ifind(tc.needle, tc.pos)) << tc.needle;
  }

  EXPECT_EQ(11u, str.ifind(std::string("bar"), 1));
  EXPECT_EQ(11u, str.ifind(std::string("Bar"), 2));
  EXPECT_EQ(11u, str.ifind(std::string("BAR"), 3));
}

// Tests converting a string to upper and lower case.
TEST(CPLString, ToupperAndTolower) {
  EXPECT_EQ("", CPLString("").toupper());
  EXPECT_EQ("\n\r\t\b ", CPLString("\n\r\t\b ").toupper());
  EXPECT_EQ("1290-=!@#$%^&*()_+`~\"[]\\{}|;':,./<>?AZAZ",
            CPLString("1290-=!@#$%^&*()_+`~\"[]\\{}|;':,./<>?azAZ").toupper());

  EXPECT_EQ("", CPLString("").tolower());
  EXPECT_EQ("\n\r\t\b ", CPLString("\n\r\t\b ").tolower());
  EXPECT_EQ("1290-=!@#$%^&*()_+`~\"[]\\{}|;':,./<>?azaz",
            CPLString("1290-=!@#$%^&*()_+`~\"[]\\{}|;':,./<>?azAZ").tolower());
}

// Tests replacing all instances of a sub-string.
TEST(CPLString, replaceAllStrToStr) {
  struct ReplaceCase {
    const char* input;
    const char* before;
    const char* after;
    const char* expected;
  };
  constexpr ReplaceCase kCases[] = {
      {.input = "", .before = "", .after = "", .expected = ""},
      {.input = "", .before = "", .after = "a", .expected = ""},
      {.input = "foo", .before = "f", .after = "b", .expected = "boo"},
      {.input = "foo", .before = "o", .after = "*", .expected = "f**"},
      {.input = "foo", .before = "-", .after = "*", .expected = "foo"},
      {.input = "foofoo", .before = "f", .after = "b", .expected = "booboo"},
      {.input = "foofoo", .before = "foo", .after = "", .expected = ""},
      {.input = "foofoo",
       .before = "foo",
       .after = "bar",
       .expected = "barbar"},
      {.input = "foobar",
       .before = "foo",
       .after = "foo",
       .expected = "foobar"},
  };
  for (const auto& tc : kCases) {
    CPLString str(tc.input);
    str.replaceAll(tc.before, tc.after);
    EXPECT_EQ(tc.expected, str);
  }
}

TEST(CPLString, replaceAllCharToStr) {
  struct ReplaceCase {
    const char* input;
    char before;
    const char* after;
    const char* expected;
  };
  constexpr ReplaceCase kCases[] = {
      {.input = "", .before = 'a', .after = "", .expected = ""},
      {.input = "", .before = 'a', .after = "b", .expected = ""},
      {.input = "a", .before = 'a', .after = "b", .expected = "b"},
      {.input = "a", .before = 'a', .after = "", .expected = ""},
      {.input = "aba", .before = 'a', .after = "", .expected = "b"},
      {.input = "aba", .before = 'a', .after = "123", .expected = "123b123"},
  };
  for (const auto& tc : kCases) {
    CPLString str(tc.input);
    str.replaceAll(tc.before, tc.after);
    EXPECT_EQ(tc.expected, str);
  }
}

TEST(CPLString, replaceAllStrToChar) {
  struct ReplaceCase {
    const char* input;
    const char* before;
    char after;
    const char* expected;
  };
  constexpr ReplaceCase kCases[] = {
      {.input = "", .before = "a", .after = 'b', .expected = ""},
      {.input = "", .before = "", .after = 'b', .expected = ""},
      {.input = "a", .before = "a", .after = 'b', .expected = "b"},
      {.input = "aca", .before = "a", .after = 'b', .expected = "bcb"},
      {.input = "aca", .before = "ca", .after = 'b', .expected = "ab"},
      {.input = "aca", .before = "", .after = 'b', .expected = "aca"},
  };
  for (const auto& tc : kCases) {
    CPLString str(tc.input);
    str.replaceAll(tc.before, tc.after);
    EXPECT_EQ(tc.expected, str);
  }
}

TEST(CPLString, replaceAllCharToChar) {
  struct ReplaceCase {
    const char* input;
    char before;
    char after;
    const char* expected;
  };
  constexpr ReplaceCase kCases[] = {
      {.input = "", .before = 'a', .after = 'b', .expected = ""},
      {.input = "a", .before = 'a', .after = 'b', .expected = "b"},
      {.input = "c", .before = 'a', .after = 'b', .expected = "c"},
      {.input = "xyzzy", .before = 'y', .after = '!', .expected = "x!zz!"},
  };
  for (const auto& tc : kCases) {
    CPLString str(tc.input);
    str.replaceAll(tc.before, tc.after);
    EXPECT_EQ(tc.expected, str);
  }
}

TEST(CPLString, replaceAllChaining) {
  CPLString str("abc123!@#$");
  EXPECT_EQ("-123!@#-", str.replaceAll("abc", '$').replaceAll("$", "-"));
}

TEST(CPLString, EndsWith) {
  const CPLString empty("");
  EXPECT_TRUE(empty.endsWith(empty));
  EXPECT_TRUE(empty.endsWith(""));
  EXPECT_FALSE(empty.endsWith("a"));
  EXPECT_FALSE(empty.endsWith(" "));

  const CPLString str("abc123");
  EXPECT_TRUE(str.endsWith(str));
  EXPECT_TRUE(str.endsWith(""));
  EXPECT_TRUE(str.endsWith("abc123"));
  EXPECT_TRUE(str.endsWith("3"));
  EXPECT_FALSE(str.endsWith("a"));
  EXPECT_FALSE(str.endsWith("abc12"));
  EXPECT_FALSE(str.endsWith(" abc123"));
}

// Tests parsing of URL key-value pairs.
TEST(CPLString, UrlGetValue) {
  static constexpr char kUrl1[] =
      "https://user:pass@example.com/key?foo=narwal&bar=0&baz=1.23#end";
  static constexpr char kUrl2[] =
      "/vsizip//vsicurl/ftp://example.com/key?a=b&1=2#tail";
  struct UrlCase {
    const char* url;
    const char* key;
    const char* expected;
  };
  constexpr UrlCase kCases[] = {
      {.url = "", .key = "", .expected = ""},
      {.url = "", .key = "foo", .expected = ""},
      {.url = kUrl1, .key = "foo", .expected = "narwal"},
      {.url = kUrl1, .key = "bar", .expected = "0"},
      // Note that it does include the ending fragment.
      {.url = kUrl1, .key = "baz", .expected = "1.23#end"},
      {.url = kUrl2, .key = "a", .expected = "b"},
      {.url = kUrl2, .key = "1", .expected = "2#tail"},
      {.url = kUrl2, .key = "2", .expected = ""},
      {.url = kUrl2, .key = "?", .expected = ""},
      {.url = kUrl2, .key = "&", .expected = ""},
  };
  for (const auto& tc : kCases) {
    EXPECT_EQ(tc.expected, CPLURLGetValue(tc.url, tc.key)) << tc.key;
  }
}

// Tests constructing URL key-value pairs.
TEST(CPLString, UrlAddKvp) {
  CPLString str = CPLURLAddKVP("http://example.org", "a", "b");
  EXPECT_EQ("http://example.org?a=b", str);
  str = CPLURLAddKVP(str.c_str(), "a", nullptr);
  EXPECT_EQ("http://example.org?", str);
  str = CPLURLAddKVP(str.c_str(), "a", "b");
  str = CPLURLAddKVP(str.c_str(), "c", "1");
  EXPECT_EQ("http://example.org?a=b&c=1", str);
  str = CPLURLAddKVP(str.c_str(), "a", nullptr);
  EXPECT_EQ("http://example.org?c=1", str);

  // Bad behavior goes unpunished.
  str = CPLURLAddKVP(str.c_str(), "?", "?");
  EXPECT_EQ("http://example.org?c=1&?=?", str);
  str = CPLURLAddKVP(str.c_str(), "&", "=");
  EXPECT_EQ("http://example.org?c=1&?=?&==", str);

  str = CPLURLAddKVP("foo", "bar", "baz");
  EXPECT_EQ("foo?bar=baz", str);

  // Updating an existing KVP with a non-null value.
  str = CPLURLAddKVP("http://example.org?a=b&c=1", "a", "new_b");
  EXPECT_EQ("http://example.org?a=new_b&c=1", str);
  str = CPLURLAddKVP("http://example.org?a=b&c=1", "c", "new_c");
  EXPECT_EQ("http://example.org?a=b&c=new_c", str);
}

CPLString CallVPrintf(CPL_FORMAT_STRING(const char* fmt), ...) {
  va_list args;
  va_start(args, fmt);
  CPLString str;
  str.vPrintf(fmt, args);
  va_end(args);
  return str;
}

CPLString CallCPLOvPrintf(CPL_FORMAT_STRING(const char* fmt), ...) {
  va_list args;
  va_start(args, fmt);
  CPLString str = CPLOvPrintf(fmt, args);
  va_end(args);
  return str;
}

TEST(CPLString, PrintfAndVPrintf) {
  CPLString str;
  EXPECT_EQ("hello 42", str.Printf("hello %d", 42));
  EXPECT_EQ("hello 42", str);

  // Test medium buffer (> 500 bytes, < 2000 bytes).
  const std::string medium(1000, 'x');
  EXPECT_EQ(medium, str.Printf("%s", medium.c_str()));

  // Test large buffer (>= 2000 bytes, triggers while loop realloc).
  const std::string large(10000, 'y');
  EXPECT_EQ(large, str.Printf("%s", large.c_str()));

  // Direct vPrintf and CPLOPrintf / CPLOvPrintf.
  EXPECT_EQ("test 123", CallVPrintf("test %d", 123));
  EXPECT_EQ("oprintf 456", CPLOPrintf("oprintf %d", 456));
  EXPECT_EQ("ovprintf 789", CallCPLOvPrintf("ovprintf %d", 789));
}

TEST(CPLString, Recode) {
  CPLString str("hello");
  // Same encoding (including default/nullptr -> UTF-8).
  EXPECT_EQ("hello", str.Recode(nullptr, nullptr));
  EXPECT_EQ("hello", str.Recode(CPL_ENC_UTF8, CPL_ENC_UTF8));

  // Valid recoding from ISO-8859-1 to UTF-8.
  CPLString latin1("\xE9");  // 'é' in ISO-8859-1
  latin1.Recode(CPL_ENC_ISO8859_1, CPL_ENC_UTF8);
  EXPECT_EQ("\xC3\xA9", latin1);
}

TEST(CPLString, URLEncode) {
  CPLString str("https://example.com/path with spaces/100%20ok%GG%?a=1&b=2");
  EXPECT_EQ("https://example.com/path%20with%20spaces/100%20ok%25GG%25?a=1&b=2",
            str.URLEncode());
}

TEST(CPLString, QuotedSQLIdentifier) {
  EXPECT_EQ("", CPLQuotedSQLIdentifier(nullptr));
  EXPECT_EQ("\"my_table\"", CPLQuotedSQLIdentifier("my_table"));
  EXPECT_EQ("\"my\"\"table\"", CPLQuotedSQLIdentifier("my\"table"));
}

}  // namespace
}  // namespace autotest2
