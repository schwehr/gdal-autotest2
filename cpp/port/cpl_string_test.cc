// Tests for GDAL's C String List (CSL) API.
// Yes, there is also a cplstring.cc tested in cplstring_test.cc.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_string.h
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

// WARNING: The CSL API uses realloc.  Function calls that might
// modify the string list return a new pointer to the list that
// callers must use.  It is the callers responsibility to free the CSL.

#include "third_party/gdal/port/cpl_string.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

using std::unique_ptr;

namespace autotest2 {
namespace {

typedef unique_ptr<char*, std::function<void(char**)>> StringListPtr;

constexpr int CSLT_NOFLAGS = 0x0000;  // TODO(schwehr): Move to cpl_string.h.

TEST(CslStringTest, CslNullptr) {
  // Tests CSLCount and CSLAddString.
  ASSERT_EQ(0, CSLCount(nullptr));

  char** string_list = CSLAddString(nullptr, nullptr);
  ASSERT_EQ(string_list, nullptr);

  CSLDestroy(string_list);  // Safe to destroy a nullptr.
}

TEST(CslStringTest, BasicCsl) {
  // Tests CSLCount and CSLAddString.
  char** string_list = CSLAddString(nullptr, "abc");
  ASSERT_NE(string_list, nullptr);
  ASSERT_EQ(1, CSLCount(string_list));

  // Passing a nullptr guarantees that realloc is not called and the
  // pointer returned will be the same as passed in by the caller.
  char** string_list2 = CSLAddString(string_list, nullptr);
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
  char** string_list = CSLAddString(nullptr, "0");

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
  char** string_list = CSLAddString(nullptr, "1");
  string_list = CSLAddString(string_list, "");
  string_list = CSLAddString(string_list, "\n");
  string_list = CSLAddString(string_list, "2");
  char** string_list_dup = CSLDuplicate(string_list);
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
  char** string_list1 = CSLAddString(nullptr, "1");
  char** string_list2 = CSLMerge(string_list1, nullptr);
  EXPECT_EQ(string_list1, string_list2);
  CSLDestroy(string_list1);
}

TEST(CslStringTest, MergeIfOrigIsNull) {
  char** string_list1 = CSLAddString(nullptr, "1");
  char** string_list2 = CSLMerge(nullptr, string_list1);
  EXPECT_NE(string_list1, string_list2);
  EXPECT_EQ(1, CSLCount(string_list2));
  EXPECT_STREQ("1", CSLGetField(string_list2, 0));
  CSLDestroy(string_list1);
  CSLDestroy(string_list2);
}

TEST(CslStringTest, MergeSameKey) {
  // The 1 key in string_list2 will overwrite the original.
  char** string_list1 = CSLAddString(nullptr, "1=a");
  char** string_list2 = CSLAddString(nullptr, "1=b");
  EXPECT_EQ(string_list1, CSLMerge(string_list1, string_list2));
  EXPECT_EQ(1, CSLCount(string_list1));
  EXPECT_STREQ("1=b", CSLGetField(string_list1, 0));
  CSLDestroy(string_list1);
  CSLDestroy(string_list2);
}

TEST(CslStringTest, MergeSameKeyMultiple) {
  char** string_list1 = CSLAddString(nullptr, "a=1");
  string_list1 = CSLAddString(string_list1, "c=3");

  char** string_list2 = CSLAddString(nullptr, "b=2");
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
  char** string_list = CSLAddString(nullptr, "abc");
  string_list = CSLAddString(string_list, "1234567890");
  char buf[] = "/tmp/gdal_cpl_string_XXXXXX";
  const char* filename = mktemp(buf);
  ASSERT_NE(nullptr, filename);
  EXPECT_EQ(2, CSLSave(string_list, filename));
  CSLDestroy(string_list);
  string_list = nullptr;

  const int unlimited_lines = -1;
  const int unlimited_columns = -1;
  char** no_options = nullptr;  // GDAL always ignores the options.
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
  char** string_list = CSLAddString(nullptr, "abc");
  // Put in some new lines to show that the writer will not count them in its
  // return, but the reader will return those as separate lines.
  string_list = CSLAddString(string_list, "1234567890\n\r\n");
  char buf[] = "/tmp/gdal_cpl_string_XXXXXX";
  const char* filename = mktemp(buf);
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

TEST(CslStringTest, TestCSLFindString) {
  EXPECT_EQ(-1, CSLFindString(nullptr, ""));
  EXPECT_EQ(-1, CSLFindString(nullptr, "abc"));

  // Test list with one empty string.
  {
    StringListPtr string_list(CSLAddString(nullptr, ""), CSLDestroy);

    EXPECT_EQ(0, CSLFindString(string_list.get(), ""));
    EXPECT_EQ(-1, CSLFindString(string_list.get(), "a"));
  }

  // Test list with one "a" string.
  {
    StringListPtr string_list(CSLAddString(nullptr, "a"), CSLDestroy);

    EXPECT_EQ(-1, CSLFindString(string_list.get(), ""));
    EXPECT_EQ(0, CSLFindString(string_list.get(), "a"));
    EXPECT_EQ(0, CSLFindString(string_list.get(), "A"));
    EXPECT_EQ(-1, CSLFindString(string_list.get(), "b"));
  }

  // Test finding first instance of a string that is lower case.
  {
    StringListPtr string_list(CSLAddString(nullptr, "a"), CSLDestroy);
    string_list.reset(CSLAddString(string_list.release(), "foo"));
    string_list.reset(CSLAddString(string_list.release(), "Foo"));

    EXPECT_EQ(-1, CSLFindString(string_list.get(), ""));
    EXPECT_EQ(1, CSLFindString(string_list.get(), "foo"));
    EXPECT_EQ(1, CSLFindString(string_list.get(), "Foo"));
  }

  // Test finding first instance of a string that is upper case.
  {
    StringListPtr string_list(CSLAddString(nullptr, "a"), CSLDestroy);
    string_list.reset(CSLAddString(string_list.release(), "Bar"));
    string_list.reset(CSLAddString(string_list.release(), "bar"));

    EXPECT_EQ(-1, CSLFindString(string_list.get(), ""));
    EXPECT_EQ(1, CSLFindString(string_list.get(), "bar"));
    EXPECT_EQ(1, CSLFindString(string_list.get(), "Bar"));
  }

  // Test that partials do not match.
  {
    StringListPtr string_list(CSLAddString(nullptr, "FooBarBaz"), CSLDestroy);
    EXPECT_EQ(-1, CSLFindString(string_list.get(), "Bar"));
  }
}

TEST(CslStringTest, TestCSLFindStringCaseSensitive) {
  EXPECT_EQ(-1, CSLFindStringCaseSensitive(nullptr, ""));
  EXPECT_EQ(-1, CSLFindStringCaseSensitive(nullptr, "abc"));

  {
    StringListPtr string_list(CSLAddString(nullptr, ""), CSLDestroy);

    EXPECT_EQ(0, CSLFindStringCaseSensitive(string_list.get(), ""));
    EXPECT_EQ(-1, CSLFindStringCaseSensitive(string_list.get(), "a"));
  }

  {
    StringListPtr string_list(CSLAddString(nullptr, "a"), CSLDestroy);

    EXPECT_EQ(-1, CSLFindStringCaseSensitive(string_list.get(), ""));
    EXPECT_EQ(0, CSLFindStringCaseSensitive(string_list.get(), "a"));
    EXPECT_EQ(-1, CSLFindStringCaseSensitive(string_list.get(), "A"));
    EXPECT_EQ(-1, CSLFindStringCaseSensitive(string_list.get(), "b"));
  }

  {
    StringListPtr string_list(CSLAddString(nullptr, "a"), CSLDestroy);
    string_list.reset(CSLAddString(string_list.release(), "foo"));
    string_list.reset(CSLAddString(string_list.release(), "Foo"));

    EXPECT_EQ(-1, CSLFindStringCaseSensitive(string_list.get(), "b"));
    EXPECT_EQ(1, CSLFindStringCaseSensitive(string_list.get(), "foo"));
    EXPECT_EQ(2, CSLFindStringCaseSensitive(string_list.get(), "Foo"));
  }

  {
    StringListPtr string_list(CSLAddString(nullptr, "a"), CSLDestroy);
    string_list.reset(CSLAddString(string_list.release(), "Bar"));
    string_list.reset(CSLAddString(string_list.release(), "bar"));

    EXPECT_EQ(2, CSLFindStringCaseSensitive(string_list.get(), "bar"));
    EXPECT_EQ(1, CSLFindStringCaseSensitive(string_list.get(), "Bar"));
  }

  {
    StringListPtr string_list(CSLAddString(nullptr, "FooBarBaz"), CSLDestroy);
    EXPECT_EQ(-1, CSLFindStringCaseSensitive(string_list.get(), "Bar"));
  }
}

TEST(CslStringTest, TestCSLPartialFindString) {
  EXPECT_EQ(-1, CSLPartialFindString(nullptr, ""));
  EXPECT_EQ(-1, CSLPartialFindString(nullptr, "abc"));

  {
    StringListPtr string_list(CSLAddString(nullptr, ""), CSLDestroy);

    EXPECT_EQ(0, CSLPartialFindString(string_list.get(), ""));
    EXPECT_EQ(-1, CSLPartialFindString(string_list.get(), "a"));
  }

  {
    StringListPtr string_list(CSLAddString(nullptr, "a"), CSLDestroy);

    EXPECT_EQ(0, CSLPartialFindString(string_list.get(), ""));
    EXPECT_EQ(0, CSLPartialFindString(string_list.get(), "a"));
    EXPECT_EQ(-1, CSLPartialFindString(string_list.get(), "A"));
    EXPECT_EQ(-1, CSLPartialFindString(string_list.get(), "b"));
  }

  {
    StringListPtr string_list(CSLAddString(nullptr, "a"), CSLDestroy);
    string_list.reset(CSLAddString(string_list.release(), "foo"));
    string_list.reset(CSLAddString(string_list.release(), "Foo"));

    EXPECT_EQ(0, CSLPartialFindString(string_list.get(), ""));
    EXPECT_EQ(1, CSLPartialFindString(string_list.get(), "foo"));
    EXPECT_EQ(2, CSLPartialFindString(string_list.get(), "Foo"));
  }

  {
    StringListPtr string_list(CSLAddString(nullptr, "a"), CSLDestroy);
    string_list.reset(CSLAddString(string_list.release(), "Bar"));
    string_list.reset(CSLAddString(string_list.release(), "bar"));

    EXPECT_EQ(2, CSLPartialFindString(string_list.get(), "bar"));
    EXPECT_EQ(1, CSLPartialFindString(string_list.get(), "Bar"));
  }

  {
    StringListPtr string_list(CSLAddString(nullptr, "a"), CSLDestroy);
    string_list.reset(CSLAddString(string_list.release(), "foobar"));
    string_list.reset(CSLAddString(string_list.release(), "FooBar"));

    EXPECT_EQ(1, CSLPartialFindString(string_list.get(), "oba"));
    EXPECT_EQ(2, CSLPartialFindString(string_list.get(), "oBa"));
  }
}

TEST(CslStringTest, TestCSLTokenizeString) {
  unique_ptr<char*, std::function<void(char**)>> result(
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
  unique_ptr<char*, std::function<void(char**)>> result(
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

  result.reset(CSLTokenizeString2(
      " a ", " ",
      CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES | CSLT_ALLOWEMPTYTOKENS));
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

TEST(CslStringTest, TestCSLTokenizeString2_b63363493) {
  unique_ptr<char*, std::function<void(char**)>> result(
      CSLTokenizeString2("(:)", ",:()", CSLT_NOFLAGS), CSLDestroy);
  ASSERT_NE(nullptr, result);
  EXPECT_EQ(nullptr, result.get()[0]);
}

//   CPLSPrintf
//   CSLAppendPrintf
//   CPLVASPrintf
//   CPLvsnprintf

TEST(CplSnprintfTest, CanSpecifyPrecisionWhenPrinting) {
  char buf[32];

  EXPECT_EQ(CPLsnprintf(buf, sizeof(buf), "a%.*fb", 1, 2.12), 5);
  EXPECT_STREQ(buf, "a2.1b");
}

TEST(CplSnprintfTest, LengthCanBeLimitedBySizeParameter) {
  char buf[8];

  EXPECT_EQ(CPLsnprintf(buf, sizeof(buf), "123%s456", "9999"), 10);
  EXPECT_STREQ(buf, "1239999");
}

TEST(CplSnprintfTest, LengthLimitZero) {
  char buf[0];

  EXPECT_EQ(CPLsnprintf(buf, sizeof(buf), "123456"), 6);
  EXPECT_STREQ(buf, "");
}

//   CPLsprintf

TEST(CplSscanfTest, CanScanFloatingPointUsingPercentLf) {
  double actual = 0;

  ASSERT_EQ(CPLsscanf("foo 123.4567 bar", "foo %lf bar", &actual), 1);
  EXPECT_DOUBLE_EQ(actual, 123.4567);
}

TEST(CplSscanfTest, CanScanMoreThanOneFloatingPoint) {
  double first = 0;
  double second = 0;

  ASSERT_EQ(CPLsscanf("1 2", "%lf %lf", &first, &second), 2);
  EXPECT_DOUBLE_EQ(first, 1.0);
  EXPECT_DOUBLE_EQ(second, 2.0);
}

TEST(CplSscanfTest, CanScanAndSpaceInInputTreatedTheSame) {
  {
    double first = 0;
    double second = 0;

    ASSERT_EQ(CPLsscanf("1 2", "%lf\t%lf", &first, &second), 2);
    EXPECT_DOUBLE_EQ(first, 1.0);
    EXPECT_DOUBLE_EQ(second, 2.0);
  }

  {
    double first = 0;
    double second = 0;

    ASSERT_EQ(CPLsscanf("1\t2", "%lf %lf", &first, &second), 2);
    EXPECT_DOUBLE_EQ(first, 1.0);
    EXPECT_DOUBLE_EQ(second, 2.0);
  }
}

TEST(CplSscanfTest, CanScanAndNumberOfSpacesDontMatter) {
  {
    double first = 0;
    double second = 0;

    ASSERT_EQ(CPLsscanf("1 2", "%lf      %lf", &first, &second), 2);
    EXPECT_DOUBLE_EQ(first, 1.0);
    EXPECT_DOUBLE_EQ(second, 2.0);
  }

  {
    double first = 0;
    double second = 0;

    ASSERT_EQ(CPLsscanf("1        2", "%lf %lf", &first, &second), 2);
    EXPECT_DOUBLE_EQ(first, 1.0);
    EXPECT_DOUBLE_EQ(second, 2.0);
  }
}

TEST(CplSscanfTest, CanScanWithMoreFormatSpecifiersThanInput) {
  double first = 0;
  double second = 0;
  double third = 0;

  ASSERT_EQ(CPLsscanf("1 2", "%lf %lf %lf", &first, &second, &third), 2);
  EXPECT_DOUBLE_EQ(first, 1.0);
  EXPECT_DOUBLE_EQ(second, 2.0);
  EXPECT_DOUBLE_EQ(third, 0.0);
}

TEST(CplSscanfTest, ReportsErrorForUnexpectedFormatSpecifier) {
  WithQuietHandler silence_errors;

  double first = 0;
  int second = 0;

  EXPECT_EQ(CPLsscanf("1 2", "%lf %d", &first, &second), 1);

  EXPECT_STREQ(CPLGetLastErrorMsg(),
               "Format %lf %d not supported by CPLsscanf()");
  EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
  EXPECT_EQ(CPLGetLastErrorNo(), CPLE_NotSupported);

  EXPECT_DOUBLE_EQ(first, 1.0);
  EXPECT_EQ(second, 0);
}

TEST(CslStringTest, TestBoolean) {
  // nullptr not allowed.

  EXPECT_FALSE(CSLTestBoolean("NO"));
  EXPECT_FALSE(CSLTestBoolean("FALSE"));
  EXPECT_FALSE(CSLTestBoolean("OFF"));
  EXPECT_FALSE(CSLTestBoolean("0"));

  // Anything else will be true.
  EXPECT_TRUE(CSLTestBoolean(""));
  EXPECT_TRUE(CSLTestBoolean("YES"));
  EXPECT_TRUE(CSLTestBoolean("TRUE"));
  EXPECT_TRUE(CSLTestBoolean("ON"));
  EXPECT_TRUE(CSLTestBoolean("1"));
  EXPECT_TRUE(CSLTestBoolean("xyzzy"));
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
  char* result = CPLEscapeString(nullptr, 0, CPLES_BackslashQuotable);
  EXPECT_STREQ("", result);
  CPLFree(result);

  // CPLUnescapeString cannot handle nullptr with 0.

  char nul[1] = {'\0'};
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
  for (int i = 0; i < 256; ++i) {
    no_escape[i] = static_cast<char>(i);
  }
  no_escape[0] = 'z';   // \0
  no_escape[10] = 'z';  // \n
  no_escape[34] = 'z';  // "
  no_escape[92] = 'z';  // backslash

  result = CPLEscapeString(no_escape, 256, CPLES_BackslashQuotable);
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(no_escape[i], result[i]);
  }
  CPLFree(result);

  no_escape[255] = '\0';
  result = CPLUnescapeString(no_escape, &length, CPLES_BackslashQuotable);
  EXPECT_LE(255, length);
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(no_escape[i], result[i]);
  }
  CPLFree(result);
}

TEST(CplStringTest, EscapeXml) {
  // Warning: The forward and reverse escaping are not symmetric.

  char* result = CPLEscapeString(nullptr, 0, CPLES_XML);
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
  char* result = CPLEscapeString(nullptr, 0, CPLES_URL);
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
  EXPECT_STREQ("%3C%3E%3F,%2F%3B'%3A%22", result);
  CPLFree(result);

  result = CPLUnescapeString("%3C%3E%3F%2C%2F%3B%27%3A%22", &length, CPLES_URL);
  EXPECT_EQ(9, length);  // NUL not included.
  EXPECT_STREQ("<>?,/;':\"", result);
  CPLFree(result);

  result = CPLEscapeString("`~!@#$%^&*()+", -1, CPLES_URL);
  EXPECT_STREQ("%60%7E!%40%23$%25%5E%26*()+", result);
  CPLFree(result);

  result = CPLUnescapeString("%60%7E%21%40%23%24%25%5E%26%2A%28%29%2B", &length,
                             CPLES_URL);
  EXPECT_EQ(13, length);  // NUL not included.
  EXPECT_STREQ("`~!@#$%^&*()+", result);
  CPLFree(result);

  result = CPLEscapeString(" -=[]{}\\|\n\r\t", -1, CPLES_URL);
  EXPECT_STREQ("%20-%3D%5B%5D%7B%7D%5C%7C%0A%0D%09", result);
  CPLFree(result);

  result = CPLUnescapeString("%20%2D%3D%5B%5D%7B%7D%5C%7C%0A%0D%09", &length,
                             CPLES_URL);
  EXPECT_EQ(12, length);  // NUL not included.
  EXPECT_STREQ(" -=[]{}\\|\n\r\t", result);
  CPLFree(result);
}

TEST(CplStringTest, EscapeSql) {
  char* result = CPLEscapeString(nullptr, 0, CPLES_SQL);
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
  for (int i = 0; i < 256; ++i) {
    no_escape[i] = static_cast<char>(i);
  }
  no_escape['\0'] = 'z';
  no_escape['\''] = 'z';
  no_escape[255] = '\0';

  result = CPLEscapeString(no_escape, 256, CPLES_SQL);
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(no_escape[i], result[i]);
  }
  CPLFree(result);

  result = CPLUnescapeString(no_escape, &length, CPLES_SQL);
  EXPECT_LE(255, length);
  // TODO(schwehr):  What is in 256 to 262?
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(no_escape[i], result[i]);
  }
  CPLFree(result);
}

TEST(CplStringTest, EscapeCsv) {
  // nullptr crashes this mode unlike the others.

  // CPLUnescapeString does not support CPLES_CSV.

  char* result = CPLEscapeString("", -1, CPLES_CSV);
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
  char* result = CPLBinaryToHex(0, nullptr);
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
  GByte* data = CPLHexToBinary("0", &len);
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
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("0 "));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("01"));
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

// CPLCharUniquePtr is only defined when GDAL_COMPILATION is defined.
TEST(CplCharUniquePtrTest, CplCharUniquePtrBasicOperations) {
  CPLCharUniquePtr ptr;

  EXPECT_EQ(nullptr, ptr.get());

  // Test that resetting will invoke custom deleter
  ptr.reset(CPLStrdup("foo"));
  EXPECT_STREQ("foo", ptr.get());
}

int CallCPLVASPrintf(char** buf, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int ret = CPLVASPrintf(buf, fmt, args);
  va_end(args);
  return ret;
}

// Hide format string from -Wformat so we can test GDAL-specific modifiers like
// %I64d and unknown specifiers in CPLvsnprintf.
int DynamicCPLsnprintf(char* buf, size_t size, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int ret = CPLvsnprintf(buf, size, fmt, args);
  va_end(args);
  return ret;
}

TEST(CslStringTest, LoadSavePrintInsertAndRemove) {
  WithQuietHandler quiet;

  // CSLLoad2 on non-existent file (with and without error emission).
  EXPECT_EQ(nullptr, CSLLoad2("/vsimem/no_such_csl_file.txt", -1, -1, nullptr));
  const char* no_err_opts[] = {"EMIT_ERROR_IF_CANNOT_OPEN_FILE=FALSE", nullptr};
  EXPECT_EQ(nullptr,
            CSLLoad2("/vsimem/no_such_csl_file.txt", -1, -1, no_err_opts));

  // CSLLoad with > 16 lines to trigger line array reallocation.
  const char* kManyLinesFile = "/vsimem/csl_many_lines.txt";
  {
    VSILFILE* fp = VSIFOpenL(kManyLinesFile, "wb");
    ASSERT_NE(nullptr, fp);
    for (int i = 0; i < 25; ++i) {
      VSIFPrintfL(fp, "line_%d\n", i);
    }
    VSIFCloseL(fp);
  }
  char** loaded = CSLLoad(kManyLinesFile);
  ASSERT_EQ(25, CSLCount(loaded));

  // CSLSave edge cases.
  EXPECT_EQ(0, CSLSave(nullptr, "/vsimem/empty_save.txt"));
  const std::string missing_dir_file =
      std::string(::testing::TempDir()) + "/no_such_dir/save.txt";
  EXPECT_EQ(0, CSLSave(loaded, missing_dir_file.c_str()));

  // CSLPrint to nullptr (stdout) with nullptr list, and to a temporary FILE*.
  EXPECT_EQ(0, CSLPrint(nullptr, nullptr));
  const std::string temp_file =
      std::string(::testing::TempDir()) + "/csl_print.txt";
  FILE* fp_out = fopen(temp_file.c_str(), "wb");
  ASSERT_NE(nullptr, fp_out);
  EXPECT_EQ(25, CSLPrint(loaded, fp_out));
  fclose(fp_out);
  VSIUnlink(temp_file.c_str());
  VSIUnlink(kManyLinesFile);
  CSLDestroy(loaded);

  // CSLInsertStrings and CSLInsertString.
  char* to_insert[] = {const_cast<char*>("ins1"), const_cast<char*>("ins2"),
                       nullptr};
  EXPECT_EQ(nullptr, CSLInsertStrings(nullptr, 0, nullptr));
  char** list = CSLInsertStrings(nullptr, 0, to_insert);
  ASSERT_EQ(2, CSLCount(list));
  list = CSLInsertStrings(list, 0, to_insert);
  ASSERT_EQ(4, CSLCount(list));
  list = CSLInsertStrings(list, 2, to_insert);
  ASSERT_EQ(6, CSLCount(list));
  list = CSLInsertStrings(list, -1, to_insert);
  ASSERT_EQ(8, CSLCount(list));
  list = CSLInsertStrings(list, 100, to_insert);
  ASSERT_EQ(10, CSLCount(list));

  list = CSLInsertString(list, 1, nullptr);
  ASSERT_EQ(10, CSLCount(list));
  list = CSLInsertString(list, 1, "single_ins");
  ASSERT_EQ(11, CSLCount(list));

  // CSLRemoveStrings.
  // TODO: b/335317901 - Upstream CSLRemoveStrings omits ++ppszDst in the
  // ppapszRetStrings == nullptr free loop, so it frees the first removed
  // string nNumToRemove times and leaks the rest. Only nNumToRemove == 1 (or
  // removing everything) is used with a nullptr out-list here until that is
  // fixed upstream in port/cpl_string.cpp.
  EXPECT_EQ(nullptr, CSLRemoveStrings(nullptr, 0, 1, nullptr));
  list = CSLRemoveStrings(list, 0, 0, nullptr);
  list = CSLRemoveStrings(list, 1, 1, nullptr);
  ASSERT_EQ(10, CSLCount(list));

  char** removed = nullptr;
  list = CSLRemoveStrings(list, 1, 2, &removed);
  ASSERT_EQ(8, CSLCount(list));
  ASSERT_EQ(2, CSLCount(removed));
  CSLDestroy(removed);

  // Remove all remaining strings.
  list = CSLRemoveStrings(list, 0, 100, nullptr);
  EXPECT_EQ(nullptr, list);
}

TEST(CslStringTest, TokenizeComplexAndEscapesInQuotes) {
  char** tok1 = CSLTokenizeStringComplex("a \"b c\"  d", " ", TRUE, TRUE);
  EXPECT_EQ(4, CSLCount(tok1));
  CSLDestroy(tok1);

  // Token > 10 chars + CSLT_HONOURSTRINGS | CSLT_PRESERVEQUOTES /
  // CSLT_PRESERVEESCAPES.
  char** tok2 = CSLTokenizeString2(
      "long_token_0123456789abcdef \"quoted \\\" and \\\\ text\"", " ",
      CSLT_HONOURSTRINGS | CSLT_PRESERVEQUOTES);
  ASSERT_EQ(2, CSLCount(tok2));
  CSLDestroy(tok2);

  char** tok3 = CSLTokenizeString2("\"quoted \\\" and \\\\ text\"", " ",
                                   CSLT_HONOURSTRINGS | CSLT_PRESERVEESCAPES);
  ASSERT_EQ(1, CSLCount(tok3));
  CSLDestroy(tok3);

  char** tok4 = CSLTokenizeString2("  hello  ,  world  ", ",",
                                   CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES);
  ASSERT_EQ(2, CSLCount(tok4));
  EXPECT_STREQ("hello", tok4[0]);
  EXPECT_STREQ("world", tok4[1]);
  CSLDestroy(tok4);
}

TEST(CplSnprintfTest, AllFormatSpecifiersAndPrintfHelpers) {
  WithQuietHandler quiet;

  // CPLSPrintf ring buffer wrap (>10 calls) and >8000 char truncation error.
  for (int i = 0; i < 15; ++i) {
    EXPECT_NE(nullptr, CPLSPrintf("ring_%d", i));
  }
  std::string huge_str(8100, 'Z');
  EXPECT_NE(nullptr, CPLSPrintf("%s", huge_str.c_str()));

  // CSLAppendPrintf.
  char** list = CSLAppendPrintf(nullptr, "first_%d", 1);
  list = CSLAppendPrintf(list, "second_%d", 2);
  ASSERT_EQ(2, CSLCount(list));
  CSLDestroy(list);

  // CPLVASPrintf.
  char* vas_buf = nullptr;
  EXPECT_GE(CallCPLVASPrintf(&vas_buf, "hello %d %s", 42, "world"), 0);
  EXPECT_STREQ("hello 42 world", vas_buf);
  CPLFree(vas_buf);
  EXPECT_GE(CallCPLVASPrintf(nullptr, "size_only %d", 42), 0);

  // CPLsprintf and CPLprintf.
  char sbuf[128] = {};
  EXPECT_GT(CPLsprintf(sbuf, "sp_%d", 99), 0);
  EXPECT_STREQ("sp_99", sbuf);

  EXPECT_GT(CPLprintf("short_print %d\n", 1), 0);
  std::string print_5k(4200, 'P');
  EXPECT_GT(CPLprintf("%s\n", print_5k.c_str()), 0);

  // CPLvsnprintf all specifiers and modifiers.
  char buf[512] = {};
  int ret = DynamicCPLsnprintf(buf, sizeof(buf), "%% %d %i %c %hd %ld %lld %zd",
                               1, -2, 'A', static_cast<short>(3), 4L, 5LL,
                               static_cast<size_t>(7));
  EXPECT_STREQ("% 1 -2 A 3 4 5 7", buf);
  EXPECT_EQ(static_cast<int>(strlen(buf)), ret);
  ret = DynamicCPLsnprintf(buf, sizeof(buf), "%o %u %x %X %ho %lu %llx %zu", 8U,
                           9U, 10U, 11U, static_cast<unsigned short>(12), 13UL,
                           14ULL, static_cast<size_t>(16));
  EXPECT_STREQ("10 9 a B 14 13 e 16", buf);
  EXPECT_EQ(static_cast<int>(strlen(buf)), ret);
  ret = DynamicCPLsnprintf(
      buf, sizeof(buf), "%-+ #010.2f %e %E %F %g %G %a %A %Lf", 1.25, 2.5, 2.5,
      3.5, 4.5, 4.5, 1.5, 1.5, static_cast<long double>(6.5));
  EXPECT_STREQ(
      "+1.25      2.500000e+00 2.500000E+00 3.500000 4.5 4.5 0x1.8p+0 "
      "0X1.8P+0 6.500000",
      buf);
  EXPECT_EQ(static_cast<int>(strlen(buf)), ret);

  // The Microsoft I64 modifier is accepted and consumes a 64-bit argument, but
  // the format is forwarded unchanged to the native snprintf, which (on glibc)
  // interprets it as the 'I' flag plus a width of 64. Only check the digits.
  EXPECT_GT(
      DynamicCPLsnprintf(buf, sizeof(buf), "%I64d", static_cast<GIntBig>(6)),
      0);
  EXPECT_NE(std::string::npos, std::string(buf).find('6'));
  EXPECT_GT(
      DynamicCPLsnprintf(buf, sizeof(buf), "%I64u", static_cast<GUIntBig>(15)),
      0);
  EXPECT_NE(std::string::npos, std::string(buf).find("15"));

  // %p output is implementation defined but non-empty for a non-null pointer.
  EXPECT_GT(DynamicCPLsnprintf(buf, sizeof(buf), "%p", buf), 0);
  EXPECT_NE('\0', buf[0]);

  // Positional '$' and unknown length modifiers fall back to vsnprintf.
  EXPECT_EQ(2, DynamicCPLsnprintf(buf, sizeof(buf), "%1$d", 42));
  EXPECT_STREQ("42", buf);
  EXPECT_EQ(3, DynamicCPLsnprintf(buf, sizeof(buf), "%jd %ju",
                                  static_cast<intmax_t>(1),
                                  static_cast<uintmax_t>(2)));
  EXPECT_STREQ("1 2", buf);

  // Truncation in %.*f, %%, and literal char falls back to vsnprintf, which
  // returns the untruncated length.
  char tiny[3] = {};
  EXPECT_EQ(8, DynamicCPLsnprintf(tiny, sizeof(tiny), "%.*f", 4, 123.456));
  EXPECT_STREQ("12", tiny);
  EXPECT_EQ(3, DynamicCPLsnprintf(tiny, sizeof(tiny), "ab%%"));
  EXPECT_STREQ("ab", tiny);
  EXPECT_EQ(4, DynamicCPLsnprintf(tiny, sizeof(tiny), "abcd"));
  EXPECT_STREQ("ab", tiny);

  // CPLsscanf additional branches.
  double d = 0.0;
  EXPECT_EQ(0, CPLsscanf("foo 1.0", "bar %lf", &d));
  EXPECT_EQ(0, CPLsscanf("not_a_num", "%lf", &d));
  EXPECT_EQ(1, CPLsscanf("1.0", "%lf trailing", &d));
}

TEST(CslStringTest, NameValueAndMemorySizeFunctions) {
  WithQuietHandler quiet;

  EXPECT_TRUE(CPLTestBoolean("YES"));
  EXPECT_FALSE(CPLTestBoolean("NO"));

  EXPECT_FALSE(CPLFetchBool(nullptr, "KEY", false));
  EXPECT_TRUE(CPLFetchBool(nullptr, "KEY", true));
  EXPECT_EQ(1, CSLFetchBoolean(nullptr, "KEY", 1));

  EXPECT_EQ(nullptr, CSLFetchNameValue(nullptr, "KEY"));
  EXPECT_EQ(nullptr, CSLFetchNameValue(nullptr, nullptr));
  EXPECT_STREQ("DEF", CSLFetchNameValueDef(nullptr, "KEY", "DEF"));
  EXPECT_EQ(-1, CSLFindName(nullptr, "KEY"));
  EXPECT_EQ(nullptr, CSLFetchNameValueMultiple(nullptr, "KEY"));

  // CSLAddNameValue and CSLSetNameValue.
  EXPECT_EQ(nullptr, CSLAddNameValue(nullptr, nullptr, "v"));
  EXPECT_EQ(nullptr, CSLAddNameValue(nullptr, "k", nullptr));

  char** list = CSLAddNameValue(nullptr, "K1", "V1");
  list = CSLAddNameValue(list, "K1", "V1_SECOND");
  list = CSLAddString(list, "K2:V2");
  list = CSLAddString(list, "K3 = V3");
  list = CSLAddString(list, "NO_SEP_ENTRY");

  EXPECT_STREQ("V1", CSLFetchNameValue(list, "K1"));
  EXPECT_STREQ("V1", CSLFetchNameValueDef(list, "K1", "DEF"));
  EXPECT_STREQ("DEF", CSLFetchNameValueDef(list, "MISSING", "DEF"));
  EXPECT_STREQ("V2", CSLFetchNameValue(list, "K2"));
  EXPECT_EQ(nullptr, CSLFetchNameValue(list, "K3"));
  EXPECT_TRUE(CPLFetchBool(list, "K1", false));

  char** multi = CSLFetchNameValueMultiple(list, "K1");
  ASSERT_EQ(2, CSLCount(multi));
  CSLDestroy(multi);

  // Update existing keys (preserving separator and whitespace).
  list = CSLSetNameValue(list, nullptr, "ignored");
  list = CSLSetNameValue(list, "K2  ", "V2_NEW");
  list = CSLSetNameValue(list, "K3", "V3_NEW");
  EXPECT_STREQ("V3_NEW", CSLFetchNameValue(list, "K3"));
  list = CSLSetNameValue(list, "K4", "V4");
  EXPECT_STREQ("V4", CSLFetchNameValue(list, "K4"));

  // Remove existing and non-existing keys with pszValue == nullptr.
  list = CSLSetNameValue(list, "K4", nullptr);
  list = CSLSetNameValue(list, "NON_EXISTING", nullptr);
  EXPECT_EQ(nullptr, CSLFetchNameValue(list, "K4"));

  CSLSetNameValueSeparator(list, ": ");
  EXPECT_STREQ(" V1", CSLFetchNameValue(list, "K1"));
  CSLDestroy(list);

  // CPLParseNameValue and CPLParseNameValueSep.
  char* key = nullptr;
  EXPECT_EQ(nullptr, CPLParseNameValue("no_separator", &key));
  EXPECT_STREQ("val", CPLParseNameValue("my_key  = val", &key));
  EXPECT_STREQ("my_key", key);
  CPLFree(key);
  EXPECT_STREQ("val2", CPLParseNameValue("my_key2:val2", nullptr));

  // CPLParseMemorySize.
  GIntBig bytes = 0;
  bool unit_spec = false;
  EXPECT_EQ(CE_Failure, CPLParseMemorySize("", &bytes, &unit_spec));
  EXPECT_EQ(CE_Failure, CPLParseMemorySize("   ", &bytes, &unit_spec));
  EXPECT_EQ(CE_Failure, CPLParseMemorySize("abc", &bytes, &unit_spec));
  EXPECT_EQ(CE_Failure, CPLParseMemorySize("-10", &bytes, &unit_spec));
  EXPECT_EQ(CE_Failure, CPLParseMemorySize("150%", &bytes, &unit_spec));
  EXPECT_EQ(CE_Failure, CPLParseMemorySize("1e30GB", &bytes, &unit_spec));
  EXPECT_EQ(CE_Failure, CPLParseMemorySize("10TB", &bytes, &unit_spec));
  EXPECT_EQ(CE_Failure, CPLParseMemorySize("10MBextra", &bytes, &unit_spec));

  EXPECT_EQ(CE_None, CPLParseMemorySize(" 25% ", &bytes, &unit_spec));
  EXPECT_TRUE(unit_spec);
  EXPECT_GT(bytes, 0);

  EXPECT_EQ(CE_None, CPLParseMemorySize("100", &bytes, &unit_spec));
  EXPECT_FALSE(unit_spec);
  EXPECT_EQ(100, bytes);

  const char* valid_sizes[] = {"10K",  "10k",  "10KB", "10kb", "10M", "10m",
                               "10MB", "10mb", "1G",   "1g",   "1GB", "1gb  "};
  for (const char* s : valid_sizes) {
    EXPECT_EQ(CE_None, CPLParseMemorySize(s, &bytes, &unit_spec)) << s;
    EXPECT_TRUE(unit_spec);
  }
}

TEST(CplStringTest, EscapeUnescapeCommandLineAndSqlComments) {
  int len = 0;

  // XML and XML_BUT_QUOTES with BOM, control chars, single quotes, numeric
  // entities, and unknown entities. The BOM is escaped as &#xFEFF; and the
  // unrepresentable control character is dropped.
  char* esc = CPLEscapeString("\xEF\xBB\xBF\x01'\"<>&a", -1, CPLES_XML);
  EXPECT_STREQ("&#xFEFF;'&quot;&lt;&gt;&amp;a", esc);
  CPLFree(esc);
  esc = CPLEscapeString("\xEF\xBB\xBF\x01'\"<>&a", -1, CPLES_XML_BUT_QUOTES);
  EXPECT_STREQ("&#xFEFF;'\"&lt;&gt;&amp;a", esc);
  CPLFree(esc);

  // Hex and decimal numeric entities are decoded; an unterminated entity
  // (&#65 without ';') stops the decoding.
  char* unesc = CPLUnescapeString("&#x41;&#x6a;&#x3F;&#65;&#65&bogus;normal",
                                  &len, CPLES_XML);
  EXPECT_STREQ("Aj?A", unesc);
  EXPECT_EQ(4, len);
  CPLFree(unesc);

  // An unknown named entity stops the decoding.
  unesc = CPLUnescapeString("ok&bogus;normal", &len, CPLES_XML);
  EXPECT_STREQ("ok", unesc);
  EXPECT_EQ(2, len);
  CPLFree(unesc);

  // BackslashQuotable with \0 (an embedded NUL in the output) and a trailing
  // backslash (ignored).
  unesc = CPLUnescapeString("a\\0b\\nc\\", &len, CPLES_BackslashQuotable);
  ASSERT_EQ(5, len);
  EXPECT_EQ(0, memcmp("a\0b\nc", unesc, 5));
  CPLFree(unesc);

  // URL with lowercase/uppercase hex, '+', and invalid %ZZ (decoded as 0).
  unesc = CPLUnescapeString("a+b%2a%2A%a0%A0%ZZ", &len, CPLES_URL);
  ASSERT_EQ(8, len);
  EXPECT_EQ(0, memcmp("a b**\xa0\xa0\0", unesc, 8));
  CPLFree(unesc);

  // SQLI.
  esc = CPLEscapeString("a\"b'c", -1, CPLES_SQLI);
  EXPECT_STREQ("a\"\"b'c", esc);
  CPLFree(esc);
  unesc = CPLUnescapeString("a\"\"b", &len, CPLES_SQLI);
  EXPECT_STREQ("a\"b", unesc);
  CPLFree(unesc);

  // CSV with CPLES_CSV_FORCE_QUOTING.
  esc = CPLEscapeString(" leading_space", -1, CPLES_CSV);
  EXPECT_STREQ(" leading_space", esc);
  CPLFree(esc);

  esc = CPLEscapeString("", -1, CPLES_CSV_FORCE_QUOTING);
  EXPECT_STREQ("\"\"", esc);
  CPLFree(esc);

  esc = CPLEscapeString("plain", -1, CPLES_CSV_FORCE_QUOTING);
  EXPECT_STREQ("\"plain\"", esc);
  CPLFree(esc);

  esc = CPLEscapeString("with\"quote", -1, CPLES_CSV_FORCE_QUOTING);
  EXPECT_STREQ("\"with\"\"quote\"", esc);
  CPLFree(esc);

  // CPLGetValueType additional branches.
  EXPECT_EQ(CPL_VALUE_INTEGER, CPLGetValueType("   42"));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("42 43"));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("1-2"));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("1.2.3"));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("e5"));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("1e"));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("1e2e3"));
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType("1e5000"));

  // CSLParseCommandLine.
  char** cmd = CSLParseCommandLine(
      "prog \"arg1 with \\\" quote\" 'arg2 with \\' quote' \\\\ plain");
  EXPECT_GE(CSLCount(cmd), 4);
  CSLDestroy(cmd);

  // CPLToupper and CPLTolower.
  EXPECT_EQ('A', CPLToupper('a'));
  EXPECT_EQ('a', CPLTolower('A'));

  // CPLRemoveSQLComments.
  std::string sql =
      "SELECT 'it''s -- not a comment', \"col\"\"--name\" -- real comment\n"
      "FROM my_table; -- end comment";
  std::string stripped = CPLRemoveSQLComments(sql);
  EXPECT_EQ(std::string::npos, stripped.find("real comment"));
  EXPECT_NE(std::string::npos, stripped.find("not a comment"));
  EXPECT_NE(std::string::npos, stripped.find("FROM my_table;"));
}

}  // namespace
}  // namespace autotest2
