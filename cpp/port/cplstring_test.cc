// Tests for GDAL's CPLString class that is derived from STL string.
// Yes, there is also a cpl_string.cc tested in cpl_string_test.cc.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cplstring.cpp
//
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

#include <cstdarg>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_string.h"

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
  CPLString str("");
  EXPECT_EQ("", str.Trim());
  str = " ";
  EXPECT_EQ("", str.Trim());

  str = "a ";
  EXPECT_EQ("a", str.Trim());
  str = " b";
  EXPECT_EQ("b", str.Trim());
  str = " c ";
  EXPECT_EQ("c", str.Trim());
  str = " d e ";
  EXPECT_EQ("d e", str.Trim());

  str = "\t\n\rfoo";
  EXPECT_EQ("foo", str.Trim());
  str = "bar\t\n\r";
  EXPECT_EQ("bar", str.Trim());
}

// Tests case insensitive searching.
TEST(CPLString, ifind) {
  CPLString str("abc123 Foo BAR");
  EXPECT_EQ(std::string::npos, str.ifind("", 0));
  EXPECT_EQ(std::string::npos, str.ifind("z", 0));
  EXPECT_EQ(std::string::npos, str.ifind("3", 10));
  EXPECT_EQ(6, str.ifind(" ", 0));

  EXPECT_EQ(5, str.ifind("3", 2));
  EXPECT_EQ(1, str.ifind("bC123", 0));

  EXPECT_EQ(0, str.ifind("abc", 0));
  EXPECT_EQ(0, str.ifind("AbC", 0));
  EXPECT_EQ(0, str.ifind("ABC", 0));

  EXPECT_EQ(7, str.ifind("foo", 0));
  EXPECT_EQ(7, str.ifind("Foo", 0));
  EXPECT_EQ(7, str.ifind("FOO", 0));

  EXPECT_EQ(11, str.ifind(std::string("bar"), 1));
  EXPECT_EQ(11, str.ifind(std::string("Bar"), 2));
  EXPECT_EQ(11, str.ifind(std::string("BAR"), 3));
}

// Tests converting a string to upper case.
TEST(CPLString, toupper) {
  CPLString str("");
  EXPECT_EQ("", str.toupper());
  str = "\n\r\t\b ";
  EXPECT_EQ("\n\r\t\b ", str.toupper());
  str = "1290-=!@#$%^&*()_+`~\"[]\\{}|;':,./<>?azAZ";
  EXPECT_EQ("1290-=!@#$%^&*()_+`~\"[]\\{}|;':,./<>?AZAZ", str.toupper());
}

// Tests converting a string to lower case.
TEST(CPLString, tolower) {
  CPLString str("");
  EXPECT_EQ("", str.tolower());
  str = "\n\r\t\b ";
  EXPECT_EQ("\n\r\t\b ", str.tolower());
  str = "1290-=!@#$%^&*()_+`~\"[]\\{}|;':,./<>?azAZ";
  EXPECT_EQ("1290-=!@#$%^&*()_+`~\"[]\\{}|;':,./<>?azaz", str.tolower());
}

// Tests replacing all instances of a sub-string.
TEST(CPLString, replaceAllStrToStr) {
  CPLString str("");
  str.replaceAll("", "");
  EXPECT_EQ("", str);

  str = "";
  str.replaceAll("", "a");
  EXPECT_EQ("", str);

  str = "foo";
  str.replaceAll("f", "b");
  EXPECT_EQ("boo", str);

  str = "foo";
  str.replaceAll("o", "*");
  EXPECT_EQ("f**", str);

  str = "foo";
  str.replaceAll("-", "*");
  EXPECT_EQ("foo", str);

  str = "foofoo";
  str.replaceAll("f", "b");
  EXPECT_EQ("booboo", str);

  str = "foofoo";
  str.replaceAll("foo", "");
  EXPECT_EQ("", str);

  str = "foofoo";
  str.replaceAll("foo", "bar");
  EXPECT_EQ("barbar", str);

  str = "foobar";
  str.replaceAll("foo", "foo");
  EXPECT_EQ("foobar", str);
}

TEST(CPLString, replaceAllCharToStr) {
  CPLString str("");
  str.replaceAll('a', "");
  EXPECT_EQ("", str);

  str = "";
  str.replaceAll('a', "b");
  EXPECT_EQ("", str);

  str = "a";
  str.replaceAll('a', "b");
  EXPECT_EQ("b", str);

  str = "a";
  str.replaceAll('a', "");
  EXPECT_EQ("", str);

  str = "aba";
  str.replaceAll('a', "");
  EXPECT_EQ("b", str);

  str = "aba";
  str.replaceAll('a', "123");
  EXPECT_EQ("123b123", str);
}

TEST(CPLString, replaceAllStrToChar) {
  CPLString str("");
  str.replaceAll("a", 'b');
  EXPECT_EQ("", str);

  str = "";
  str.replaceAll("", 'b');
  EXPECT_EQ("", str);

  str = "a";
  str.replaceAll("a", 'b');
  EXPECT_EQ("b", str);

  str = "aca";
  str.replaceAll("a", 'b');
  EXPECT_EQ("bcb", str);

  str = "aca";
  str.replaceAll("ca", 'b');
  EXPECT_EQ("ab", str);

  str = "aca";
  str.replaceAll("", 'b');
  EXPECT_EQ("aca", str);
}

TEST(CPLString, replaceAllCharToChar) {
  CPLString str("");
  str.replaceAll('a', 'b');
  EXPECT_EQ("", str);

  str = "a";
  str.replaceAll('a', 'b');
  EXPECT_EQ("b", str);

  str = "c";
  str.replaceAll('a', 'b');
  EXPECT_EQ("c", str);

  str = "xyzzy";
  str.replaceAll('y', '!');
  EXPECT_EQ("x!zz!", str);
}

TEST(CPLString, replaceAllChaining) {
  CPLString str("abc123!@#$");
  EXPECT_EQ("-123!@#-", str.replaceAll("abc", '$').replaceAll("$", "-"));
}

TEST(CPLString, EndsWithEmpty) {
  CPLString str("");
  EXPECT_TRUE(str.endsWith(str));
  EXPECT_TRUE(str.endsWith(""));
  EXPECT_FALSE(str.endsWith("a"));
  EXPECT_FALSE(str.endsWith(" "));
}

TEST(CPLString, EndsWith) {
  CPLString str("abc123");
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
  CPLString str = CPLURLGetValue("", "");
  EXPECT_EQ("", str);
  str = CPLURLGetValue("", "foo");
  EXPECT_EQ("", str);
  std::string url(
      "https://user:pass@example.com/key?foo=narwal&bar=0&baz=1.23#end");
  str = CPLURLGetValue(url.c_str(), "foo");
  EXPECT_EQ("narwal", str);
  str = CPLURLGetValue(url.c_str(), "bar");
  EXPECT_EQ("0", str);
  // Note that it does include the ending fragment.
  str = CPLURLGetValue(url.c_str(), "baz");
  EXPECT_EQ("1.23#end", str);

  std::string url2("/vsizip//vsicurl/ftp://example.com/key?a=b&1=2#tail");
  str = CPLURLGetValue(url2.c_str(), "a");
  EXPECT_EQ("b", str);
  str = CPLURLGetValue(url2.c_str(), "1");
  EXPECT_EQ("2#tail", str);
  str = CPLURLGetValue(url2.c_str(), "2");
  EXPECT_EQ("", str);
  str = CPLURLGetValue(url2.c_str(), "?");
  EXPECT_EQ("", str);
  str = CPLURLGetValue(url2.c_str(), "&");
  EXPECT_EQ("", str);
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

namespace {
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
}  // namespace

TEST(CPLString, PrintfAndVPrintf) {
  CPLString str;
  EXPECT_EQ("hello 42", str.Printf("hello %d", 42));
  EXPECT_EQ("hello 42", str);

  // Test medium buffer (> 500 bytes, < 2000 bytes).
  std::string medium(1000, 'x');
  EXPECT_EQ(medium, str.Printf("%s", medium.c_str()));

  // Test large buffer (>= 2000 bytes, triggers while loop realloc).
  std::string large(10000, 'y');
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

TEST(CPLString, IfindOutOfBoundsPos) {
  CPLString str("abcdef");
  EXPECT_EQ(std::string::npos, str.ifind("a", 100));
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
