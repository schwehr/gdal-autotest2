// Tests for GDAL's C String List (CSL) API.
// Yes, there is also a cplstring.cc tested in cplstring_test.cc.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cplstring.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// WARNING: The CSL API uses realloc.  Function calls that might
// modify the string list return a new pointer to the list that
// callers must use.  It is the callers responsibility to free the CSL.

#include "port/cpl_string.h"

#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

#include "gunit.h"

using std::unique_ptr;

namespace autotest2 {
namespace {

TEST(CslStringTest, CslNullptr) {
  // Tests CSLCount and CSLAddString.
  ASSERT_EQ(0, CSLCount(nullptr));

  char **string_list = CSLAddString(nullptr, nullptr);
  ASSERT_EQ(string_list, nullptr);

  CSLDestroy(string_list);  // Safe to destroy a nullptr.
}

TEST(CslStringTest, BasicCsl) {
  // Tests CSLCount and CSLAddString.
  char **string_list = CSLAddString(nullptr, "abc");
  ASSERT_NE(string_list, nullptr);
  ASSERT_EQ(1, CSLCount(string_list));

  // Passing a nullptr guarentees that realloc is not called and the
  // pointer returned will be the same as passed in by the caller.
  char **string_list2 = CSLAddString(string_list, nullptr);
  ASSERT_EQ(string_list, string_list2);
  ASSERT_EQ(1, CSLCount(string_list2));

  // Possible realloc.  Should not use string_list after this call.
  string_list2 = CSLAddString(string_list2, "123");
  ASSERT_EQ(2, CSLCount(string_list2));

  // string_list is no longer relevant, so do not free.
  CSLDestroy(string_list2);
}

TEST(CslStringTest, GetField) {
  ASSERT_STREQ("", CSLGetField(nullptr, 0));
  char **string_list = CSLAddString(nullptr, "0");

  ASSERT_STREQ("", CSLGetField(string_list, -1));
  ASSERT_STREQ("0", CSLGetField(string_list, 0));
  ASSERT_STREQ("", CSLGetField(string_list, 1));

  string_list = CSLAddString(string_list, "1");
  ASSERT_STREQ("0", CSLGetField(string_list, 0));
  ASSERT_STREQ("1", CSLGetField(string_list, 1));
  ASSERT_STREQ("", CSLGetField(string_list, 2));

  CSLDestroy(string_list);
}

TEST(CslStringTest, Duplicate) {
  EXPECT_EQ(nullptr, CSLDuplicate(nullptr));
  char **string_list = CSLAddString(nullptr, "1");
  string_list = CSLAddString(string_list, "");
  string_list = CSLAddString(string_list, "\n");
  string_list = CSLAddString(string_list, "2");
  char **string_list_dup = CSLDuplicate(string_list);
  ASSERT_NE(nullptr, string_list_dup);
  ASSERT_EQ(4, CSLCount(string_list_dup));
  ASSERT_EQ(CSLCount(string_list), CSLCount(string_list_dup));
  for (int i = 0; i < CSLCount(string_list); ++i) {
    EXPECT_STREQ(CSLGetField(string_list, i), CSLGetField(string_list_dup, i));
  }
  CSLDestroy(string_list);
  // Access the last record after the delete for heap check.
  EXPECT_STREQ("2", CSLGetField(string_list_dup, 3));
  CSLDestroy(string_list_dup);
}

TEST(CslStringTest, Merge) {
  EXPECT_EQ(nullptr, CSLMerge(nullptr, nullptr));

  // Merge a nullptr into a list of one element.
  char **string_list1 = CSLAddString(nullptr, "1");
  char **string_list2 = CSLMerge(string_list1, nullptr);
  EXPECT_EQ(string_list1, string_list2);
  CSLDestroy(string_list1);
}

TEST(CslStringTest, MergeIfOrigIsNull) {
  char **string_list1 = CSLAddString(nullptr, "1");
  char **string_list2 = CSLMerge(nullptr, string_list1);
  EXPECT_NE(string_list1, string_list2);
  EXPECT_EQ(1, CSLCount(string_list2));
  EXPECT_STREQ("1", CSLGetField(string_list2, 0));
  CSLDestroy(string_list1);
  CSLDestroy(string_list2);
}

TEST(CslStringTest, MergeSameKey) {
  // The 1 key in string_list2 will overwrite the original.
  char **string_list1 = CSLAddString(nullptr, "1=a");
  char **string_list2 = CSLAddString(nullptr, "1=b");
  EXPECT_EQ(string_list1, CSLMerge(string_list1, string_list2));
  EXPECT_EQ(1, CSLCount(string_list1));
  EXPECT_STREQ("1=b", CSLGetField(string_list1, 0));
  CSLDestroy(string_list1);
  CSLDestroy(string_list2);
}

TEST(CslStringTest, MergeSameKeyMultiple) {
  char **string_list1 = CSLAddString(nullptr, "a=1");
  string_list1 = CSLAddString(string_list1, "c=3");

  char **string_list2 = CSLAddString(nullptr, "b=2");
  // This key will overwrite c=3.
  string_list2 = CSLAddString(string_list2, "c=4");

  string_list1 = CSLMerge(string_list1, string_list2);

  EXPECT_EQ(3, CSLCount(string_list1));
  EXPECT_STREQ("a=1", CSLGetField(string_list1, 0));
  EXPECT_STREQ("c=4", CSLGetField(string_list1, 1));
  EXPECT_STREQ("b=2", CSLGetField(string_list1, 2));

  CSLDestroy(string_list1);
  CSLDestroy(string_list2);
}

TEST(CslStringTest, SaveAndLoad2) {
  char **string_list = CSLAddString(nullptr, "abc");
  string_list = CSLAddString(string_list, "1234567890");
  char buf[] = "/tmp/gdal_cpl_string_XXXXXX";
  const char *filename = mktemp(buf);
  ASSERT_NE(nullptr, filename);
  EXPECT_EQ(2, CSLSave(string_list, filename));
  CSLDestroy(string_list);
  string_list = nullptr;

  const int unlimited_lines = -1;
  const int unlimited_columns = -1;
  char **no_options = nullptr;  // GDAL always ignores the options.
  string_list =
      CSLLoad2(filename, unlimited_lines, unlimited_columns, no_options);
  EXPECT_EQ(2, CSLCount(string_list));
  EXPECT_STREQ("abc", string_list[0]);
  EXPECT_STREQ("1234567890", string_list[1]);
  CSLDestroy(string_list);
  string_list = nullptr;

  // Request only 1 line be read.
  const int read_only_one_line = 1;
  string_list =
      CSLLoad2(filename, read_only_one_line, unlimited_columns, no_options);
  EXPECT_EQ(1, CSLCount(string_list));
  EXPECT_STREQ("abc", string_list[0]);
  CSLDestroy(string_list);
  string_list = nullptr;

  // Request only 1 line be read with 4 characters max.
  // Max columns includes the newline.
  const int max_columns_4 = 4;
  string_list =
      CSLLoad2(filename, read_only_one_line, max_columns_4, no_options);
  EXPECT_EQ(1, CSLCount(string_list));
  EXPECT_STREQ("abc", string_list[0]);
  CSLDestroy(string_list);
  string_list = nullptr;

  // Giving more lines than exist in the file is not an error.
  const int read_up_to_10_lines = 10;
  string_list =
      CSLLoad2(filename, read_up_to_10_lines, unlimited_columns, no_options);
  EXPECT_EQ(2, CSLCount(string_list));
  CSLDestroy(string_list);
  string_list = nullptr;

  // Request no more than 3 character per column be read.
  // If any line before the number of lines is up exceeds the max columns, the
  // whole load fails and returns nothing. The file has more than 3 characters
  // in the first line (3 characters and the new line), so it returns nullptr.
  const int max_columns_3 = 3;
  string_list =
      CSLLoad2(filename, read_only_one_line, max_columns_3, no_options);
  EXPECT_EQ(nullptr, string_list);
}

TEST(CslStringTest, SaveAndLoad) {
  char **string_list = CSLAddString(nullptr, "abc");
  // Put in some new lines to show that the writer will not count them in its
  // return, but the reader will return those as separate lines.
  string_list = CSLAddString(string_list, "1234567890\n\r\n");
  char buf[] = "/tmp/gdal_cpl_string_XXXXXX";
  const char *filename = mktemp(buf);
  ASSERT_NE(nullptr, filename);
  EXPECT_EQ(2, CSLSave(string_list, filename));
  CSLDestroy(string_list);
  string_list = nullptr;

  // CSLLoad is just CSLLoad2 with unlimited lines and columns.
  string_list = CSLLoad(filename);
  // Get 4 rather than 2 because of the newlines snuck in above.
  EXPECT_EQ(4, CSLCount(string_list));
  EXPECT_STREQ("abc", string_list[0]);
  EXPECT_STREQ("1234567890", string_list[1]);
  EXPECT_STREQ("", string_list[2]);
  EXPECT_STREQ("", string_list[3]);
  CSLDestroy(string_list);
}

// TODO(schwehr): And test the rest of the functions in csl_string.cpp.
//   CSLPrint
//   CSLInsertStrings
//   CSLInsertString
//   CSLRemoveStrings
//   CSLFindString
//   CSLFindStringCaseSensitive
//   CSLPartialFindString

TEST(CslStringTest, TestCSLTokenizeString) {
  unique_ptr<char *, std::function<void(char **)>> result(
      CSLTokenizeString(nullptr), CSLDestroy);
  ASSERT_NE(nullptr, result);
  EXPECT_EQ(nullptr, result.get()[0]);

  result.reset(CSLTokenizeString(""));
  ASSERT_EQ(0, CSLCount(result.get()));
  EXPECT_EQ(nullptr, result.get()[0]);

  result.reset(CSLTokenizeString("a"));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);

  result.reset(CSLTokenizeString("a b"));
  ASSERT_EQ(2, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);
  EXPECT_STREQ("b", result.get()[1]);

  result.reset(CSLTokenizeString("\" a b \""));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ(" a b ", result.get()[0]);

  result.reset(CSLTokenizeString("aa \" \" b"));
  ASSERT_EQ(3, CSLCount(result.get()));
  EXPECT_STREQ("aa", result.get()[0]);
  EXPECT_STREQ(" ", result.get()[1]);
  EXPECT_STREQ("b", result.get()[2]);
}

//   CSLTokenizeStringComplex - Deprecated.  Will not test.

TEST(CslStringTest, TestCSLTokenizeString2) {
  constexpr int CSLT_NOFLAGS = 0x0000;  // TODO(schwher): Move to cpl_string.h.

  unique_ptr<char *, std::function<void(char **)>> result(
      CSLTokenizeString2(nullptr, nullptr, CSLT_NOFLAGS), CSLDestroy);
  ASSERT_NE(nullptr, result);
  EXPECT_EQ(nullptr, result.get()[0]);

  result.reset(CSLTokenizeString2("", " ", CSLT_NOFLAGS));
  ASSERT_EQ(0, CSLCount(result.get()));
  EXPECT_EQ(nullptr, result.get()[0]);

  result.reset(CSLTokenizeString2("a", " ", CSLT_NOFLAGS));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);

  result.reset(CSLTokenizeString2("a b", " ", CSLT_NOFLAGS));
  ASSERT_EQ(2, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);
  EXPECT_STREQ("b", result.get()[1]);

  result.reset(CSLTokenizeString2("a b", ",", CSLT_NOFLAGS));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("a b", result.get()[0]);

  // Make sure double quote works as a delimiter.
  result.reset(CSLTokenizeString2("a\"b", "\"", CSLT_NOFLAGS));
  ASSERT_EQ(2, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);
  EXPECT_STREQ("b", result.get()[1]);

  // Multiple characters for delimiters.
  result.reset(CSLTokenizeString2("a,b c", " ,", CSLT_NOFLAGS));
  ASSERT_EQ(3, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);
  EXPECT_STREQ("b", result.get()[1]);
  EXPECT_STREQ("c", result.get()[2]);

  // Check CSLT_ALLOWEMPTYTOKENS.
  result.reset(CSLTokenizeString2("", " ", CSLT_ALLOWEMPTYTOKENS));
  ASSERT_EQ(0, CSLCount(result.get()));

  result.reset(CSLTokenizeString2(" ", " ", CSLT_ALLOWEMPTYTOKENS));
  ASSERT_EQ(2, CSLCount(result.get()));
  EXPECT_STREQ("", result.get()[0]);
  EXPECT_STREQ("", result.get()[1]);

  result.reset(CSLTokenizeString2("abcd", "bc", CSLT_ALLOWEMPTYTOKENS));
  ASSERT_EQ(3, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);
  EXPECT_STREQ("", result.get()[1]);
  EXPECT_STREQ("d", result.get()[2]);

  // Check CSLT_STRIPLEADSPACES.
  result.reset(CSLTokenizeString2("", " ", CSLT_STRIPLEADSPACES));
  ASSERT_EQ(0, CSLCount(result.get()));

  result.reset(CSLTokenizeString2(" \t    ", " ", CSLT_STRIPLEADSPACES));
  ASSERT_EQ(0, CSLCount(result.get()));

  result.reset(CSLTokenizeString2(" \t    a b ", " ", CSLT_STRIPLEADSPACES));
  ASSERT_EQ(2, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);
  EXPECT_STREQ("b", result.get()[1]);

  // Strip leading does not work in this case.
  result.reset(CSLTokenizeString2(
      "  a  b ", " ", CSLT_STRIPLEADSPACES | CSLT_ALLOWEMPTYTOKENS));
  ASSERT_EQ(6, CSLCount(result.get()));
  EXPECT_STREQ("", result.get()[0]);
  EXPECT_STREQ("", result.get()[1]);
  EXPECT_STREQ("a", result.get()[2]);
  EXPECT_STREQ("", result.get()[3]);
  EXPECT_STREQ("b", result.get()[4]);
  EXPECT_STREQ("", result.get()[5]);

  // Check CSLT_STRIPENDSPACES.
  result.reset(CSLTokenizeString2("", " ", CSLT_STRIPENDSPACES));
  ASSERT_EQ(0, CSLCount(result.get()));

  result.reset(CSLTokenizeString2(" \t ", ",", CSLT_STRIPENDSPACES));
  ASSERT_EQ(0, CSLCount(result.get()));

  result.reset(CSLTokenizeString2("a \t ", ",", CSLT_STRIPENDSPACES));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);

  result.reset(CSLTokenizeString2("a \t ", " ", CSLT_STRIPENDSPACES));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);

  result.reset(CSLTokenizeString2("a \t ", " ",
                                  CSLT_STRIPENDSPACES | CSLT_ALLOWEMPTYTOKENS));
  ASSERT_EQ(3, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);
  EXPECT_STREQ("", result.get()[1]);
  EXPECT_STREQ("", result.get()[2]);

  result.reset(CSLTokenizeString2(" a ", " ",
                                  CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);

  result.reset(CSLTokenizeString2(" a ", " ", CSLT_STRIPLEADSPACES |
                                              CSLT_STRIPENDSPACES |
                                              CSLT_ALLOWEMPTYTOKENS));
  ASSERT_EQ(3, CSLCount(result.get()));
  EXPECT_STREQ("", result.get()[0]);
  EXPECT_STREQ("a", result.get()[1]);
  EXPECT_STREQ("", result.get()[2]);

  // CSLT_HONOURSTRINGS
  result.reset(CSLTokenizeString2("\"a\"", " ", CSLT_NOFLAGS));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\"a\"", result.get()[0]);

  result.reset(CSLTokenizeString2("\"a\"", " ", CSLT_HONOURSTRINGS));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);

  result.reset(CSLTokenizeString2("\"a\"", "\"", CSLT_HONOURSTRINGS));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);

  result.reset(CSLTokenizeString2("a,\"b,c\",d", ",", CSLT_HONOURSTRINGS));
  ASSERT_EQ(3, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);
  EXPECT_STREQ("b,c", result.get()[1]);
  EXPECT_STREQ("d", result.get()[2]);

  result.reset(CSLTokenizeString2("a,b\",c,\"d,e", ",", CSLT_HONOURSTRINGS));
  ASSERT_EQ(3, CSLCount(result.get()));
  EXPECT_STREQ("a", result.get()[0]);
  EXPECT_STREQ("b,c,d", result.get()[1]);
  EXPECT_STREQ("e", result.get()[2]);

  result.reset(CSLTokenizeString2("\"\"", " ", CSLT_HONOURSTRINGS));
  ASSERT_EQ(0, CSLCount(result.get()));

  result.reset(CSLTokenizeString2("\"\"", " ",
                                  CSLT_HONOURSTRINGS | CSLT_ALLOWEMPTYTOKENS));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("", result.get()[0]);

  // CSLT_PRESERVEQUOTES
  result.reset(CSLTokenizeString2("\"\"", " ", CSLT_PRESERVEQUOTES));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\"\"", result.get()[0]);

  result.reset(CSLTokenizeString2("\"\"", "\"", CSLT_PRESERVEQUOTES));
  ASSERT_EQ(0, CSLCount(result.get()));

  result.reset(CSLTokenizeString2("\"\"", "\"",
                                  CSLT_PRESERVEQUOTES | CSLT_ALLOWEMPTYTOKENS));
  ASSERT_EQ(3, CSLCount(result.get()));
  EXPECT_STREQ("", result.get()[0]);
  EXPECT_STREQ("", result.get()[1]);
  EXPECT_STREQ("", result.get()[2]);

  // CSLT_PRESERVEESCAPES does not appear to work.
  result.reset(CSLTokenizeString2("\b", " ", CSLT_NOFLAGS));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\b", result.get()[0]);

  result.reset(CSLTokenizeString2("\b", " ", CSLT_PRESERVEESCAPES));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\b", result.get()[0]);

  result.reset(CSLTokenizeString2("\\", " ", CSLT_NOFLAGS));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\\", result.get()[0]);

  result.reset(CSLTokenizeString2("\\", " ", CSLT_PRESERVEESCAPES));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\\", result.get()[0]);

  result.reset(CSLTokenizeString2("\\\\", " ", CSLT_NOFLAGS));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\\\\", result.get()[0]);

  result.reset(CSLTokenizeString2("\\\\", " ", CSLT_PRESERVEESCAPES));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\\\\", result.get()[0]);

  result.reset(CSLTokenizeString2("\\b", " ", CSLT_NOFLAGS));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\\b", result.get()[0]);

  result.reset(CSLTokenizeString2("\\b", " ", CSLT_PRESERVEESCAPES));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\\b", result.get()[0]);

  result.reset(CSLTokenizeString2("\\\"", " ", CSLT_NOFLAGS));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\\\"", result.get()[0]);

  result.reset(CSLTokenizeString2("\\\"", " ", CSLT_PRESERVEESCAPES));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\\\"", result.get()[0]);

  result.reset(CSLTokenizeString2("\"", " ", CSLT_NOFLAGS));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\"", result.get()[0]);

  result.reset(CSLTokenizeString2("\"", " ", CSLT_PRESERVEESCAPES));
  ASSERT_EQ(1, CSLCount(result.get()));
  EXPECT_STREQ("\"", result.get()[0]);
}

//   CPLSPrintf
//   CSLAppendPrintf
//   CPLVASPrintf
//   CPLvsnprintf
//   CPLsnprintf
//   CPLsprintf
//   CPLsscanf

TEST(CslStringTest, TestBoolean) {
  // nullptr not allowed.

  EXPECT_EQ(FALSE, CSLTestBoolean("NO"));
  EXPECT_EQ(FALSE, CSLTestBoolean("FALSE"));
  EXPECT_EQ(FALSE, CSLTestBoolean("OFF"));
  EXPECT_EQ(FALSE, CSLTestBoolean("0"));

  // Anything else will be true.
  EXPECT_EQ(TRUE, CSLTestBoolean(""));
  EXPECT_EQ(TRUE, CSLTestBoolean("YES"));
  EXPECT_EQ(TRUE, CSLTestBoolean("TRUE"));
  EXPECT_EQ(TRUE, CSLTestBoolean("ON"));
  EXPECT_EQ(TRUE, CSLTestBoolean("1"));
  EXPECT_EQ(TRUE, CSLTestBoolean("xyzzy"));
}

// TODO(schwehr): And test the rest of the functions in csl_string.cpp.
//   CSLFetchBoolean
//   CSLFetchNameValueDef
//   CSLFetchNameValue
//   CSLFindName
//   CPLParseNameValue
//   CSLFetchNameValueMultiple
//   CSLAddNameValue
//   CSLSetNameValue
//   CSLSetNameValueSeparator

TEST(CplStringTest, EscapingBackslash) {
  char *result = CPLEscapeString(nullptr, 0, CPLES_BackslashQuotable);
  EXPECT_STREQ("", result);
  CPLFree(result);

  // CPLUnescapeString cannot handle nullptr with 0.

  char nul[1] ={'\0'};
  result = CPLEscapeString(nul, 1, CPLES_BackslashQuotable);
  EXPECT_STREQ("\\0", result);
  CPLFree(result);

  int length = 0;
  result = CPLUnescapeString("", &length, CPLES_BackslashQuotable);
  EXPECT_EQ(0, length);  // NUL not included.
  EXPECT_STREQ("", result);
  CPLFree(result);

  char lots[] = "\n\"\\";
  result = CPLEscapeString(lots, -1, CPLES_BackslashQuotable);
  EXPECT_STREQ("\\n\\\"\\\\", result);
  CPLFree(result);

  result = CPLUnescapeString("\\n\\\"\\\\", &length, CPLES_BackslashQuotable);
  EXPECT_EQ(3, length);  // NUL not included.
  EXPECT_STREQ(lots, result);
  CPLFree(result);

  char no_escape[256];
  for (int i=0; i < 256; ++i) {
    no_escape[i] = static_cast<char>(i);
  }
  no_escape[0] = 'z';  // \0
  no_escape[10] = 'z';  // \n
  no_escape[34] = 'z';  // "
  no_escape[92] = 'z';  // backslash

  result = CPLEscapeString(no_escape, 256, CPLES_BackslashQuotable);
  for (int i=0; i < 256; ++i) {
    EXPECT_EQ(no_escape[i], result[i]);
  }
  CPLFree(result);

  no_escape[255] = '\0';
  result = CPLUnescapeString(no_escape, &length, CPLES_BackslashQuotable);
  EXPECT_LE(255, length);
  for (int i=0; i < 256; ++i) {
    EXPECT_EQ(no_escape[i], result[i]);
  }
  CPLFree(result);
}

TEST(CplStringTest, EscapeXml) {
  // Warning: The forward and reverse escaping are not symmetric.

  char *result = CPLEscapeString(nullptr, 0, CPLES_XML);
  EXPECT_STREQ("", result);
  CPLFree(result);

  // CPLUnescapeString cannot handle nullptr with 0.

  result = CPLEscapeString("<>&\"", -1, CPLES_XML);
  EXPECT_STREQ("&lt;&gt;&amp;&quot;", result);
  CPLFree(result);

  int length = 0;
  result = CPLUnescapeString("&lt;&gt;&amp;&quot;&apos;", &length, CPLES_XML);
  EXPECT_EQ(5, length);  // NUL not included.
  EXPECT_STREQ("<>&\"'", result);
  CPLFree(result);

  // TODO(schwehr): Explore more of the XML escaping and unescaping.
}

TEST(CplStringTest, EscapeUrl) {
  char *result = CPLEscapeString(nullptr, 0, CPLES_URL);
  EXPECT_STREQ("", result);
  CPLFree(result);

  // CPLUnescapeString cannot handle nullptr with 0.

  result = CPLEscapeString("", -1, CPLES_URL);
  EXPECT_STREQ("", result);
  CPLFree(result);

  int length = 0;
  result = CPLUnescapeString("", &length, CPLES_URL);
  EXPECT_EQ(0, length);
  EXPECT_STREQ("", result);
  CPLFree(result);

  result = CPLEscapeString("azAZ190._", -1, CPLES_URL);
  EXPECT_STREQ("azAZ190._", result);
  CPLFree(result);

  result = CPLUnescapeString("azAZ190._", &length, CPLES_URL);
  EXPECT_EQ(9, length);  // NUL not included.
  EXPECT_STREQ("azAZ190._", result);
  CPLFree(result);

  // Randomly try things that need encoding.
  result = CPLEscapeString("<>?,/;':\"", -1, CPLES_URL);
  EXPECT_STREQ("%3C%3E%3F,%2F%3B'%3A\"", result);
  CPLFree(result);

  result = CPLUnescapeString("%3C%3E%3F%2C%2F%3B%27%3A%22", &length, CPLES_URL);
  EXPECT_EQ(9, length);  // NUL not included.
  EXPECT_STREQ("<>?,/;':\"", result);
  CPLFree(result);

  result = CPLEscapeString("`~!@#$%^&*()+", -1, CPLES_URL);
  EXPECT_STREQ("%60%7E!%40%23$%25%5E%26*()+", result);
  CPLFree(result);

  result = CPLUnescapeString(
      "%60%7E%21%40%23%24%25%5E%26%2A%28%29%2B", &length, CPLES_URL);
  EXPECT_EQ(13, length);  // NUL not included.
  EXPECT_STREQ("`~!@#$%^&*()+", result);
  CPLFree(result);

  result = CPLEscapeString(" -=[]{}\\|\n\r\t", -1, CPLES_URL);
  EXPECT_STREQ("%20-%3D%5B%5D%7B%7D%5C%7C%0A%0D%09", result);
  CPLFree(result);

  result = CPLUnescapeString(
      "%20%2D%3D%5B%5D%7B%7D%5C%7C%0A%0D%09", &length, CPLES_URL);
  EXPECT_EQ(12, length);  // NUL not included.
  EXPECT_STREQ(" -=[]{}\\|\n\r\t", result);
  CPLFree(result);
}

TEST(CplStringTest, EscapeSql) {
  char *result = CPLEscapeString(nullptr, 0, CPLES_SQL);
  EXPECT_STREQ("", result);
  CPLFree(result);

  // CPLUnescapeString cannot handle nullptr even with 0.

  result = CPLEscapeString("", -1, CPLES_SQL);
  EXPECT_STREQ("", result);
  CPLFree(result);

  int length = 0;
  result = CPLUnescapeString("", &length, CPLES_SQL);
  EXPECT_EQ(0, length);
  EXPECT_STREQ("", result);
  CPLFree(result);

  result = CPLEscapeString("'; DROP TABLE ?;'", -1, CPLES_SQL);
  EXPECT_STREQ("''; DROP TABLE ?;''", result);
  CPLFree(result);

  result = CPLUnescapeString("''; DROP TABLE ?;''", &length, CPLES_SQL);
  EXPECT_EQ(17, length);
  EXPECT_STREQ("'; DROP TABLE ?;'", result);
  CPLFree(result);

  char no_escape[256];
  for (int i=0; i < 256; ++i) {
    no_escape[i] = static_cast<char>(i);
  }
  no_escape['\0'] = 'z';
  no_escape['\''] = 'z';
  no_escape[255] = '\0';

  result = CPLEscapeString(no_escape, 256, CPLES_SQL);
  for (int i=0; i < 256; ++i) {
    EXPECT_EQ(no_escape[i], result[i]);
  }
  CPLFree(result);

  result = CPLUnescapeString(no_escape, &length, CPLES_SQL);
  EXPECT_LE(255, length);
  // TODO(schwehr):  What is in 256 to 262?
  for (int i=0; i < 256; ++i) {
    EXPECT_EQ(no_escape[i], result[i]);
  }
  CPLFree(result);
}

TEST(CplStringTest, EscapeCsv) {
  // nullptr crashes this mode unike the others.

  // CPLUnescapeString doues not support CPLES_CSV.

  char *result = CPLEscapeString("", -1, CPLES_CSV);
  EXPECT_STREQ("", result);
  CPLFree(result);

  result = CPLEscapeString("1", -1, CPLES_CSV);
  EXPECT_STREQ("1", result);
  CPLFree(result);

  result = CPLEscapeString(",", -1, CPLES_CSV);
  EXPECT_STREQ("\",\"", result);
  CPLFree(result);

  result = CPLEscapeString("\"", -1, CPLES_CSV);
  EXPECT_STREQ("\"\"\"\"", result);
  CPLFree(result);

  result = CPLEscapeString(";", -1, CPLES_CSV);
  EXPECT_STREQ("\";\"", result);
  CPLFree(result);

  result = CPLEscapeString("\t", -1, CPLES_CSV);
  EXPECT_STREQ("\"\t\"", result);
  CPLFree(result);

  result = CPLEscapeString("\n", -1, CPLES_CSV);
  EXPECT_STREQ("\"\n\"", result);
  CPLFree(result);

  result = CPLEscapeString("\r", -1, CPLES_CSV);
  EXPECT_STREQ("\"\r\"", result);
  CPLFree(result);

  result = CPLEscapeString("1,2", -1, CPLES_CSV);
  EXPECT_STREQ("\"1,2\"", result);
  CPLFree(result);

  result = CPLEscapeString("1,3\t\r\n\t", -1, CPLES_CSV);
  EXPECT_STREQ("\"1,3\t\r\n\t\"", result);
  CPLFree(result);

  result = CPLEscapeString("azAZ091<>?:'][{}|~!`", -1, CPLES_CSV);
  EXPECT_STREQ("azAZ091<>?:'][{}|~!`", result);
  CPLFree(result);

  result = CPLEscapeString("@#$%^&*()_+-=\\", -1, CPLES_CSV);
  EXPECT_STREQ("@#$%^&*()_+-=\\", result);
  CPLFree(result);
}


TEST(CplStringNumberTest, TestBinaryToHex) {
  char *result = CPLBinaryToHex(0, nullptr);
  EXPECT_STREQ("", result);
  CPLFree(result);

  GByte data_a[1] = {0};
  result = CPLBinaryToHex(0, data_a);
  EXPECT_STREQ("", result);
  CPLFree(result);

  GByte data_b[1] = {0};
  result = CPLBinaryToHex(1, &data_b[0]);
  EXPECT_STREQ("00", result);
  CPLFree(result);

  GByte data_c[1] = {11};
  result = CPLBinaryToHex(1, &data_c[0]);
  EXPECT_STREQ("0B", result);
  CPLFree(result);

  GByte data_d[1] = {254};
  result = CPLBinaryToHex(1, &data_d[0]);
  EXPECT_STREQ("FE", result);
  CPLFree(result);

  GByte data_e[2] = {15, 1};
  result = CPLBinaryToHex(2, &data_e[0]);
  EXPECT_STREQ("0F01", result);
  CPLFree(result);

  GByte data_f[2] = {240, 160};
  result = CPLBinaryToHex(2, &data_f[0]);
  EXPECT_STREQ("F0A0", result);
  CPLFree(result);
}

TEST(CplStringNumberTest, TestHexToBinary) {
  // The behavior of CPLHexToBinary is a bit surprising.
  // This documents the existing behavior and does not imply that
  // this is the behavior we want.
  // It does not work correctly for odd numbers of characters.
  //
  // TODO(schwehr): Provide GDAL with a more robust replacement.

  int len = 0;
  GByte *data = CPLHexToBinary("0", &len);
  ASSERT_EQ(0, len);
  EXPECT_NE(nullptr, data);
  CPLFree(data);

  data = CPLHexToBinary("1", &len);
  ASSERT_EQ(0, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(0, data[0]);
  CPLFree(data);

  data = CPLHexToBinary("F", &len);
  ASSERT_EQ(0, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(0, data[0]);
  CPLFree(data);

  data = CPLHexToBinary("FF", &len);
  ASSERT_EQ(1, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(255, data[0]);
  CPLFree(data);

  data = CPLHexToBinary("ff", &len);
  ASSERT_EQ(1, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(255, data[0]);
  CPLFree(data);

  data = CPLHexToBinary("0F", &len);
  ASSERT_EQ(1, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(15, data[0]);
  CPLFree(data);

  // The 0x is treated as two zero digits.
  data = CPLHexToBinary("0xF", &len);
  ASSERT_EQ(1, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(0, data[0]);
  CPLFree(data);

  data = CPLHexToBinary("0x0F", &len);
  ASSERT_EQ(2, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(0, data[0]);
  CPLFree(data);

  // Improper handling of odd number of digits.
  data = CPLHexToBinary("FFF", &len);
  ASSERT_EQ(1, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(255, data[0]);
  CPLFree(data);

  data = CPLHexToBinary("FFFFFFFF", &len);
  ASSERT_EQ(4, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(255, data[0]);
  EXPECT_EQ(255, data[1]);
  EXPECT_EQ(255, data[2]);
  EXPECT_EQ(255, data[3]);
  CPLFree(data);

  // All other characters are 0 in the LUT.

  // The capital letter o.
  data = CPLHexToBinary("OO", &len);
  ASSERT_EQ(1, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(0, data[0]);
  CPLFree(data);

  data = CPLHexToBinary("xx", &len);
  ASSERT_EQ(1, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(0, data[0]);
  CPLFree(data);

  data = CPLHexToBinary("zz", &len);
  ASSERT_EQ(1, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(0, data[0]);
  CPLFree(data);

  data = CPLHexToBinary("--", &len);
  ASSERT_EQ(1, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(0, data[0]);
  CPLFree(data);

  data = CPLHexToBinary("++", &len);
  ASSERT_EQ(1, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(0, data[0]);
  CPLFree(data);

  data = CPLHexToBinary("..", &len);
  ASSERT_EQ(1, len);
  EXPECT_NE(nullptr, data);
  EXPECT_EQ(0, data[0]);
  CPLFree(data);
}

TEST(CplStringNumberTest, TestGetValueType) {
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType(nullptr));

  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType(""));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("a"));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("B"));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("_"));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("<"));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType(">"));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("123abc"));

  EXPECT_EQ(CPL_VALUE_INTEGER, CPLGetValueType("-"));
  EXPECT_EQ(CPL_VALUE_INTEGER, CPLGetValueType("+"));
  EXPECT_EQ(CPL_VALUE_INTEGER, CPLGetValueType("0"));
  EXPECT_EQ(CPL_VALUE_INTEGER, CPLGetValueType("0 "));
  EXPECT_EQ(CPL_VALUE_INTEGER, CPLGetValueType("-1"));
  EXPECT_EQ(CPL_VALUE_INTEGER, CPLGetValueType("2"));
  EXPECT_EQ(CPL_VALUE_INTEGER, CPLGetValueType("+2"));
  EXPECT_EQ(CPL_VALUE_INTEGER, CPLGetValueType("9999999999999999"));

  EXPECT_EQ(CPL_VALUE_REAL, CPLGetValueType("."));
  EXPECT_EQ(CPL_VALUE_REAL, CPLGetValueType(".1"));
  EXPECT_EQ(CPL_VALUE_REAL, CPLGetValueType("0."));
  EXPECT_EQ(CPL_VALUE_REAL, CPLGetValueType("-1."));
  EXPECT_EQ(CPL_VALUE_REAL, CPLGetValueType("-.1"));
  EXPECT_EQ(CPL_VALUE_REAL, CPLGetValueType("1e3"));
  EXPECT_EQ(CPL_VALUE_REAL, CPLGetValueType("1e3 "));
  EXPECT_EQ(CPL_VALUE_REAL, CPLGetValueType("2.e4"));
  EXPECT_EQ(CPL_VALUE_REAL, CPLGetValueType("-3e5"));
  EXPECT_EQ(CPL_VALUE_REAL, CPLGetValueType("1e-5"));
  EXPECT_EQ(CPL_VALUE_REAL, CPLGetValueType("5e+9"));
}

TEST(CplStringTest, TestStrlcpy) {
  char dst[5] = "1234";
  EXPECT_EQ(0, CPLStrlcpy(dst, "", 0));
  EXPECT_STREQ("1234", dst);

  char dst2[5] = "1234";
  EXPECT_EQ(0, CPLStrlcpy(dst2, "", sizeof(dst2)));
  EXPECT_STREQ("", dst2);

  char dst3[5] = "1234";
  EXPECT_EQ(0, CPLStrlcpy(dst3, "", 0));
  EXPECT_STREQ("1234", dst3);

  char dst4[5] = "1234";
  EXPECT_EQ(2, CPLStrlcpy(dst4, "12", 1));
  EXPECT_STREQ("", dst4);

  char dst5[5] = "1234";
  EXPECT_EQ(2, CPLStrlcpy(dst5, "12", 2));
  EXPECT_STREQ("1", dst5);

  char dst6[5] = "1234";
  EXPECT_EQ(2, CPLStrlcpy(dst6, "12", sizeof(dst5)));
  EXPECT_STREQ("12", dst6);

  char dst7[5] = "abcd";
  EXPECT_EQ(7, CPLStrlcpy(dst7, "1234567", sizeof(dst6)));
  EXPECT_STREQ("1234", dst7);
}

TEST(CplStringTest, TestStrlcat) {
  // BSD style string concatenation.
  // The return is the size the call tried to create.

  char dst[5] = "";
  EXPECT_EQ(0, CPLStrlcat(dst, "", 0));
  EXPECT_STREQ("", dst);

  char dst2[5] = "";
  EXPECT_EQ(1, CPLStrlcat(dst2, "1", 0));
  EXPECT_STREQ("", dst2);

  char dst3[5] = "";
  EXPECT_EQ(8, CPLStrlcat(dst3, "12345678", sizeof(dst3)));
  EXPECT_STREQ("1234", dst3);

  char dst4[5] = "abc";
  EXPECT_EQ(7, CPLStrlcat(dst4, "defg", sizeof(dst4)));
  EXPECT_STREQ("abcd", dst4);

  char dst5[15] = "abcdefg";
  EXPECT_EQ(9, CPLStrlcat(dst5, "defg", 5));
  EXPECT_STREQ("abcdefg", dst5);
}

TEST(CplStringTest, TestStrnlen) {
  // nullptr not allowed.

  EXPECT_EQ(0, CPLStrnlen("", 0));
  EXPECT_EQ(0, CPLStrnlen("", 1));

  EXPECT_EQ(0, CPLStrnlen(" ", 0));
  EXPECT_EQ(1, CPLStrnlen(" ", 1));

  EXPECT_EQ(5, CPLStrnlen("0123456789", 5));
  EXPECT_EQ(10, CPLStrnlen("0123456789", 12));
}

}  // namespace
}  // namespace autotest2
