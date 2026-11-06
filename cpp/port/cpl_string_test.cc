// Copyright 2014 Google LLC
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
// Tests for GDAL's C String List (CSL) API.
// Yes, there is also a cplstring.cc tested in cplstring_test.cc.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_string.h
//
// WARNING: The CSL API uses realloc. Function calls that might
// modify the string list return a new pointer to the list that
// callers must use. It is the callers responsibility to free the CSL.

#include "third_party/gdal/port/cpl_string.h"

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

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
  static constexpr char kFilename[] =
      "/vsimem/gdal_cpl_string_save_and_load2.txt";
  absl::Cleanup remove_file = [] { VSIUnlink(kFilename); };

  {
    char** raw = CSLAddString(nullptr, "abc");
    StringListPtr to_save(CSLAddString(raw, "1234567890"), CSLDestroy);
    EXPECT_EQ(2, CSLSave(to_save.get(), kFilename));
  }

  constexpr int kUnlimitedLines = -1;
  constexpr int kUnlimitedColumns = -1;
  char** no_options = nullptr;  // GDAL always ignores the options.
  {
    StringListPtr string_list(
        CSLLoad2(kFilename, kUnlimitedLines, kUnlimitedColumns, no_options),
        CSLDestroy);
    ASSERT_EQ(2, CSLCount(string_list.get()));
    EXPECT_STREQ("abc", string_list.get()[0]);
    EXPECT_STREQ("1234567890", string_list.get()[1]);
  }

  // Request only 1 line be read.
  constexpr int kReadOnlyOneLine = 1;
  {
    StringListPtr string_list(
        CSLLoad2(kFilename, kReadOnlyOneLine, kUnlimitedColumns, no_options),
        CSLDestroy);
    ASSERT_EQ(1, CSLCount(string_list.get()));
    EXPECT_STREQ("abc", string_list.get()[0]);
  }

  // Request only 1 line be read with 4 characters max.
  // Max columns includes the newline.
  constexpr int kMaxColumns4 = 4;
  {
    StringListPtr string_list(
        CSLLoad2(kFilename, kReadOnlyOneLine, kMaxColumns4, no_options),
        CSLDestroy);
    ASSERT_EQ(1, CSLCount(string_list.get()));
    EXPECT_STREQ("abc", string_list.get()[0]);
  }

  // Giving more lines than exist in the file is not an error.
  constexpr int kReadUpTo10Lines = 10;
  {
    StringListPtr string_list(
        CSLLoad2(kFilename, kReadUpTo10Lines, kUnlimitedColumns, no_options),
        CSLDestroy);
    EXPECT_EQ(2, CSLCount(string_list.get()));
  }

  // Request no more than 3 characters per column be read.
  // If any line before the number of lines is up exceeds the max columns, the
  // whole load fails and returns nothing. The file has more than 3 characters
  // in the first line (3 characters and the new line), so it returns nullptr.
  constexpr int kMaxColumns3 = 3;
  EXPECT_EQ(nullptr,
            CSLLoad2(kFilename, kReadOnlyOneLine, kMaxColumns3, no_options));
}

TEST(CslStringTest, SaveAndLoad) {
  static constexpr char kFilename[] =
      "/vsimem/gdal_cpl_string_save_and_load.txt";
  absl::Cleanup remove_file = [] { VSIUnlink(kFilename); };

  {
    char** raw = CSLAddString(nullptr, "abc");
    // Put in some new lines to show that the writer will not count them in its
    // return, but the reader will return those as separate lines.
    StringListPtr to_save(CSLAddString(raw, "1234567890\n\r\n"), CSLDestroy);
    EXPECT_EQ(2, CSLSave(to_save.get(), kFilename));
  }

  // CSLLoad is just CSLLoad2 with unlimited lines and columns.
  StringListPtr string_list(CSLLoad(kFilename), CSLDestroy);
  // Get 4 rather than 2 because of the newlines snuck in above.
  ASSERT_EQ(4, CSLCount(string_list.get()));
  EXPECT_STREQ("abc", string_list.get()[0]);
  EXPECT_STREQ("1234567890", string_list.get()[1]);
  EXPECT_STREQ("", string_list.get()[2]);
  EXPECT_STREQ("", string_list.get()[3]);
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
  StringListPtr result(CSLTokenizeString(nullptr), CSLDestroy);
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
  StringListPtr result(CSLTokenizeString2(nullptr, nullptr, CSLT_NOFLAGS),
                       CSLDestroy);
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
  constexpr const char* kEscapeInputs[] = {"\b",  "\\",   "\\\\",
                                           "\\b", "\\\"", "\""};
  for (const char* input : kEscapeInputs) {
    result.reset(CSLTokenizeString2(input, " ", CSLT_NOFLAGS));
    ASSERT_EQ(1, CSLCount(result.get()));
    EXPECT_STREQ(input, result.get()[0]);

    result.reset(CSLTokenizeString2(input, " ", CSLT_PRESERVEESCAPES));
    ASSERT_EQ(1, CSLCount(result.get()));
    EXPECT_STREQ(input, result.get()[0]);
  }
}

TEST(CslStringTest, TestCSLTokenizeString2_b63363493) {
  StringListPtr result(CSLTokenizeString2("(:)", ",:()", CSLT_NOFLAGS),
                       CSLDestroy);
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
  struct TestCase {
    const char* input;
    bool expected;
  };
  constexpr TestCase kCases[] = {
      {.input = "NO", .expected = false},
      {.input = "FALSE", .expected = false},
      {.input = "OFF", .expected = false},
      {.input = "0", .expected = false},
      // Anything else will be true.
      {.input = "", .expected = true},
      {.input = "YES", .expected = true},
      {.input = "TRUE", .expected = true},
      {.input = "ON", .expected = true},
      {.input = "1", .expected = true},
      {.input = "xyzzy", .expected = true},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_EQ(tc.expected, static_cast<bool>(CSLTestBoolean(tc.input)));
  }
}

TEST(CplStringTest, EscapingBackslash) {
  std::unique_ptr<char, CplFreeDeleter> result(
      CPLEscapeString(nullptr, 0, CPLES_BackslashQuotable));
  EXPECT_STREQ("", result.get());

  // CPLUnescapeString cannot handle nullptr with 0.

  char nul[1] = {'\0'};
  result.reset(CPLEscapeString(nul, 1, CPLES_BackslashQuotable));
  EXPECT_STREQ("\\0", result.get());

  int length = 0;
  result.reset(CPLUnescapeString("", &length, CPLES_BackslashQuotable));
  EXPECT_EQ(0, length);  // NUL not included.
  EXPECT_STREQ("", result.get());

  char lots[] = "\n\"\\";
  result.reset(CPLEscapeString(lots, -1, CPLES_BackslashQuotable));
  EXPECT_STREQ("\\n\\\"\\\\", result.get());

  result.reset(
      CPLUnescapeString("\\n\\\"\\\\", &length, CPLES_BackslashQuotable));
  EXPECT_EQ(3, length);  // NUL not included.
  EXPECT_STREQ(lots, result.get());

  char no_escape[256];
  for (int i = 0; i < 256; ++i) {
    no_escape[i] = static_cast<char>(i);
  }
  no_escape[0] = 'z';   // \0
  no_escape[10] = 'z';  // \n
  no_escape[34] = 'z';  // "
  no_escape[92] = 'z';  // backslash

  result.reset(CPLEscapeString(no_escape, 256, CPLES_BackslashQuotable));
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(no_escape[i], result.get()[i]);
  }

  no_escape[255] = '\0';
  result.reset(CPLUnescapeString(no_escape, &length, CPLES_BackslashQuotable));
  EXPECT_LE(255, length);
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(no_escape[i], result.get()[i]);
  }
}

TEST(CplStringTest, EscapeXml) {
  // Warning: The forward and reverse escaping are not symmetric.
  std::unique_ptr<char, CplFreeDeleter> result(
      CPLEscapeString(nullptr, 0, CPLES_XML));
  EXPECT_STREQ("", result.get());

  // CPLUnescapeString cannot handle nullptr with 0.

  result.reset(CPLEscapeString("<>&\"", -1, CPLES_XML));
  EXPECT_STREQ("&lt;&gt;&amp;&quot;", result.get());

  int length = 0;
  result.reset(
      CPLUnescapeString("&lt;&gt;&amp;&quot;&apos;", &length, CPLES_XML));
  EXPECT_EQ(5, length);  // NUL not included.
  EXPECT_STREQ("<>&\"'", result.get());
}

TEST(CplStringTest, EscapeUrl) {
  std::unique_ptr<char, CplFreeDeleter> result(
      CPLEscapeString(nullptr, 0, CPLES_URL));
  EXPECT_STREQ("", result.get());

  // CPLUnescapeString cannot handle nullptr with 0.

  struct TestCase {
    const char* raw;
    const char* escaped;
    const char* unescape_input;
    int expected_len;
  };
  constexpr TestCase kCases[] = {
      {.raw = "", .escaped = "", .unescape_input = "", .expected_len = 0},
      {.raw = "azAZ190._",
       .escaped = "azAZ190._",
       .unescape_input = "azAZ190._",
       .expected_len = 9},
      {.raw = "<>?,/;':\"",
       .escaped = "%3C%3E%3F,%2F%3B'%3A%22",
       .unescape_input = "%3C%3E%3F%2C%2F%3B%27%3A%22",
       .expected_len = 9},
      {.raw = "`~!@#$%^&*()+",
       .escaped = "%60%7E!%40%23$%25%5E%26*()+",
       .unescape_input = "%60%7E%21%40%23%24%25%5E%26%2A%28%29%2B",
       .expected_len = 13},
      {.raw = " -=[]{}\\|\n\r\t",
       .escaped = "%20-%3D%5B%5D%7B%7D%5C%7C%0A%0D%09",
       .unescape_input = "%20%2D%3D%5B%5D%7B%7D%5C%7C%0A%0D%09",
       .expected_len = 12},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.raw);
    result.reset(CPLEscapeString(tc.raw, -1, CPLES_URL));
    EXPECT_STREQ(tc.escaped, result.get());

    int length = 0;
    result.reset(CPLUnescapeString(tc.unescape_input, &length, CPLES_URL));
    EXPECT_EQ(tc.expected_len, length);  // NUL not included.
    EXPECT_STREQ(tc.raw, result.get());
  }
}

TEST(CplStringTest, EscapeSql) {
  std::unique_ptr<char, CplFreeDeleter> result(
      CPLEscapeString(nullptr, 0, CPLES_SQL));
  EXPECT_STREQ("", result.get());

  // CPLUnescapeString cannot handle nullptr even with 0.

  result.reset(CPLEscapeString("", -1, CPLES_SQL));
  EXPECT_STREQ("", result.get());

  int length = 0;
  result.reset(CPLUnescapeString("", &length, CPLES_SQL));
  EXPECT_EQ(0, length);
  EXPECT_STREQ("", result.get());

  result.reset(CPLEscapeString("'; DROP TABLE ?;'", -1, CPLES_SQL));
  EXPECT_STREQ("''; DROP TABLE ?;''", result.get());

  result.reset(CPLUnescapeString("''; DROP TABLE ?;''", &length, CPLES_SQL));
  EXPECT_EQ(17, length);
  EXPECT_STREQ("'; DROP TABLE ?;'", result.get());

  char no_escape[256];
  for (int i = 0; i < 256; ++i) {
    no_escape[i] = static_cast<char>(i);
  }
  no_escape['\0'] = 'z';
  no_escape['\''] = 'z';
  no_escape[255] = '\0';

  result.reset(CPLEscapeString(no_escape, 256, CPLES_SQL));
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(no_escape[i], result.get()[i]);
  }

  result.reset(CPLUnescapeString(no_escape, &length, CPLES_SQL));
  EXPECT_LE(255, length);
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(no_escape[i], result.get()[i]);
  }
}

TEST(CplStringTest, EscapeCsv) {
  // nullptr crashes this mode unlike the others.
  // CPLUnescapeString does not support CPLES_CSV.
  struct TestCase {
    const char* input;
    std::string_view expected;
  };
  constexpr TestCase kCases[] = {
      {.input = "", .expected = ""},
      {.input = "1", .expected = "1"},
      {.input = ",", .expected = "\",\""},
      {.input = "\"", .expected = "\"\"\"\""},
      {.input = ";", .expected = "\";\""},
      {.input = "\t", .expected = "\"\t\""},
      {.input = "\n", .expected = "\"\n\""},
      {.input = "\r", .expected = "\"\r\""},
      {.input = "1,2", .expected = "\"1,2\""},
      {.input = "1,3\t\r\n\t", .expected = "\"1,3\t\r\n\t\""},
      {.input = "azAZ091<>?:'][{}|~!`", .expected = "azAZ091<>?:'][{}|~!`"},
      {.input = "@#$%^&*()_+-=\\", .expected = "@#$%^&*()_+-=\\"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    std::unique_ptr<char, CplFreeDeleter> result(
        CPLEscapeString(tc.input, -1, CPLES_CSV));
    ASSERT_NE(nullptr, result);
    EXPECT_EQ(tc.expected, result.get());
  }
}

TEST(CplStringNumberTest, TestBinaryToHex) {
  std::unique_ptr<char, CplFreeDeleter> result(CPLBinaryToHex(0, nullptr));
  EXPECT_STREQ("", result.get());

  constexpr GByte kDataA[1] = {0};
  result.reset(CPLBinaryToHex(0, kDataA));
  EXPECT_STREQ("", result.get());

  struct TestCase {
    std::span<const GByte> data;
    std::string_view expected;
  };
  static constexpr auto kDataB = std::to_array<GByte>({0});
  static constexpr auto kDataC = std::to_array<GByte>({11});
  static constexpr auto kDataD = std::to_array<GByte>({254});
  static constexpr auto kDataE = std::to_array<GByte>({15, 1});
  static constexpr auto kDataF = std::to_array<GByte>({240, 160});
  constexpr TestCase kCases[] = {
      {.data = kDataB, .expected = "00"},
      {.data = kDataC, .expected = "0B"},
      {.data = kDataD, .expected = "FE"},
      {.data = kDataE, .expected = "0F01"},
      {.data = kDataF, .expected = "F0A0"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.expected);
    result.reset(
        CPLBinaryToHex(static_cast<int>(tc.data.size()), tc.data.data()));
    ASSERT_NE(nullptr, result);
    EXPECT_EQ(tc.expected, result.get());
  }
}

TEST(CplStringNumberTest, TestHexToBinary) {
  // The behavior of CPLHexToBinary is a bit surprising.
  // This documents the existing behavior and does not imply that
  // this is the behavior we want.
  // It does not work correctly for odd numbers of characters.
  int len = 0;
  std::unique_ptr<GByte, CplFreeDeleter> data(CPLHexToBinary("0", &len));
  ASSERT_EQ(0, len);
  EXPECT_NE(nullptr, data);

  struct TestCase {
    const char* input;
    int expected_len;
    GByte expected_first_byte;
  };
  // Note: CPLHexToBinary always allocates (nLen / 2 + 1) bytes and writes a
  // trailing NUL at pabyWKB[i] after decoding, so data[0] is valid (and 0)
  // even when expected_len == 0.
  constexpr TestCase kCases[] = {
      {.input = "1", .expected_len = 0, .expected_first_byte = 0},
      {.input = "F", .expected_len = 0, .expected_first_byte = 0},
      {.input = "FF", .expected_len = 1, .expected_first_byte = 255},
      {.input = "ff", .expected_len = 1, .expected_first_byte = 255},
      {.input = "0F", .expected_len = 1, .expected_first_byte = 15},
      // The 0x is treated as two zero digits.
      {.input = "0xF", .expected_len = 1, .expected_first_byte = 0},
      {.input = "0x0F", .expected_len = 2, .expected_first_byte = 0},
      // Improper handling of odd number of digits.
      {.input = "FFF", .expected_len = 1, .expected_first_byte = 255},
      // All other characters are 0 in the LUT.
      {.input = "OO", .expected_len = 1, .expected_first_byte = 0},
      {.input = "xx", .expected_len = 1, .expected_first_byte = 0},
      {.input = "zz", .expected_len = 1, .expected_first_byte = 0},
      {.input = "--", .expected_len = 1, .expected_first_byte = 0},
      {.input = "++", .expected_len = 1, .expected_first_byte = 0},
      {.input = "..", .expected_len = 1, .expected_first_byte = 0},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    data.reset(CPLHexToBinary(tc.input, &len));
    ASSERT_EQ(tc.expected_len, len);
    ASSERT_NE(nullptr, data);
    EXPECT_EQ(tc.expected_first_byte, data.get()[0]);
  }

  data.reset(CPLHexToBinary("FFFFFFFF", &len));
  ASSERT_EQ(4, len);
  ASSERT_NE(nullptr, data);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(255, data.get()[i]);
  }
}

TEST(CplStringNumberTest, TestGetValueType) {
  EXPECT_EQ(CPL_VALUE_STRING, CPLGetValueType(nullptr));

  struct TestCase {
    const char* input;
    CPLValueType expected;
  };
  constexpr TestCase kCases[] = {
      {.input = "", .expected = CPL_VALUE_STRING},
      {.input = "a", .expected = CPL_VALUE_STRING},
      {.input = "B", .expected = CPL_VALUE_STRING},
      {.input = "_", .expected = CPL_VALUE_STRING},
      {.input = "<", .expected = CPL_VALUE_STRING},
      {.input = ">", .expected = CPL_VALUE_STRING},
      {.input = "123abc", .expected = CPL_VALUE_STRING},
      {.input = "-", .expected = CPL_VALUE_INTEGER},
      {.input = "+", .expected = CPL_VALUE_INTEGER},
      {.input = "0", .expected = CPL_VALUE_INTEGER},
      {.input = "0 ", .expected = CPL_VALUE_STRING},
      {.input = "01", .expected = CPL_VALUE_STRING},
      {.input = "-1", .expected = CPL_VALUE_INTEGER},
      {.input = "2", .expected = CPL_VALUE_INTEGER},
      {.input = "+2", .expected = CPL_VALUE_INTEGER},
      {.input = "9999999999999999", .expected = CPL_VALUE_INTEGER},
      {.input = ".", .expected = CPL_VALUE_REAL},
      {.input = ".1", .expected = CPL_VALUE_REAL},
      {.input = "0.", .expected = CPL_VALUE_REAL},
      {.input = "-1.", .expected = CPL_VALUE_REAL},
      {.input = "-.1", .expected = CPL_VALUE_REAL},
      {.input = "1e3", .expected = CPL_VALUE_REAL},
      {.input = "1e3 ", .expected = CPL_VALUE_REAL},
      {.input = "2.e4", .expected = CPL_VALUE_REAL},
      {.input = "-3e5", .expected = CPL_VALUE_REAL},
      {.input = "1e-5", .expected = CPL_VALUE_REAL},
      {.input = "5e+9", .expected = CPL_VALUE_REAL},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_EQ(tc.expected, CPLGetValueType(tc.input));
  }
}

TEST(CplStringTest, TestStrlcpy) {
  struct TestCase {
    const char* src;
    size_t size;
    size_t expected_ret;
    std::string_view expected_dst;
  };
  constexpr TestCase kCases[] = {
      {.src = "", .size = 0, .expected_ret = 0, .expected_dst = "1234"},
      {.src = "", .size = 5, .expected_ret = 0, .expected_dst = ""},
      {.src = "12", .size = 1, .expected_ret = 2, .expected_dst = ""},
      {.src = "12", .size = 2, .expected_ret = 2, .expected_dst = "1"},
      {.src = "12", .size = 5, .expected_ret = 2, .expected_dst = "12"},
      {.src = "1234567", .size = 5, .expected_ret = 7, .expected_dst = "1234"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.src);
    char dst[5] = "1234";
    EXPECT_EQ(tc.expected_ret, CPLStrlcpy(dst, tc.src, tc.size));
    EXPECT_EQ(tc.expected_dst, dst);
  }
}

TEST(CplStringTest, TestStrlcat) {
  // BSD style string concatenation.
  // The return is the size the call tried to create.
  struct TestCase {
    const char* initial;
    const char* src;
    size_t size;
    size_t expected_ret;
    std::string_view expected_dst;
  };
  constexpr TestCase kCases[] = {
      {.initial = "",
       .src = "",
       .size = 0,
       .expected_ret = 0,
       .expected_dst = ""},
      {.initial = "",
       .src = "1",
       .size = 0,
       .expected_ret = 1,
       .expected_dst = ""},
      {.initial = "",
       .src = "12345678",
       .size = 5,
       .expected_ret = 8,
       .expected_dst = "1234"},
      {.initial = "abc",
       .src = "defg",
       .size = 5,
       .expected_ret = 7,
       .expected_dst = "abcd"},
      {.initial = "abcdefg",
       .src = "defg",
       .size = 5,
       .expected_ret = 9,
       .expected_dst = "abcdefg"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.initial);
    char dst[15] = {};
    CPLStrlcpy(dst, tc.initial, sizeof(dst));
    EXPECT_EQ(tc.expected_ret, CPLStrlcat(dst, tc.src, tc.size));
    EXPECT_EQ(tc.expected_dst, dst);
  }
}

TEST(CplStringTest, TestStrnlen) {
  // nullptr not allowed.
  struct TestCase {
    const char* str;
    size_t maxlen;
    size_t expected;
  };
  constexpr TestCase kCases[] = {
      {.str = "", .maxlen = 0, .expected = 0},
      {.str = "", .maxlen = 1, .expected = 0},
      {.str = " ", .maxlen = 0, .expected = 0},
      {.str = " ", .maxlen = 1, .expected = 1},
      {.str = "0123456789", .maxlen = 5, .expected = 5},
      {.str = "0123456789", .maxlen = 12, .expected = 10},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.str);
    EXPECT_EQ(tc.expected, CPLStrnlen(tc.str, tc.maxlen));
  }
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
  static constexpr char kManyLinesFile[] = "/vsimem/csl_many_lines.txt";
  absl::Cleanup unlink_many_lines = [] { VSIUnlink(kManyLinesFile); };
  {
    VSILFILE* fp = VSIFOpenL(kManyLinesFile, "wb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp] { VSIFCloseL(fp); };
    for (int i = 0; i < 25; ++i) {
      VSIFPrintfL(fp, "line_%d\n", i);
    }
  }
  StringListPtr loaded(CSLLoad(kManyLinesFile), CSLDestroy);
  ASSERT_EQ(25, CSLCount(loaded.get()));

  // CSLSave edge cases.
  EXPECT_EQ(0, CSLSave(nullptr, "/vsimem/empty_save.txt"));
  const std::string missing_dir_file =
      std::string(::testing::TempDir()) + "/no_such_dir/save.txt";
  EXPECT_EQ(0, CSLSave(loaded.get(), missing_dir_file.c_str()));

  // CSLPrint to nullptr (stdout) with nullptr list, and to a temporary FILE*.
  EXPECT_EQ(0, CSLPrint(nullptr, nullptr));
  const std::string temp_file =
      std::string(::testing::TempDir()) + "/csl_print.txt";
  {
    absl::Cleanup unlink_temp = [&temp_file] { VSIUnlink(temp_file.c_str()); };
    FILE* fp_out = fopen(temp_file.c_str(), "wb");
    ASSERT_NE(nullptr, fp_out);
    absl::Cleanup close_out = [fp_out] { fclose(fp_out); };
    EXPECT_EQ(25, CSLPrint(loaded.get(), fp_out));
  }

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
  std::unique_ptr<char, CplFreeDeleter> esc(
      CPLEscapeString("\xEF\xBB\xBF\x01'\"<>&a", -1, CPLES_XML));
  EXPECT_STREQ("&#xFEFF;'&quot;&lt;&gt;&amp;a", esc.get());
  esc.reset(
      CPLEscapeString("\xEF\xBB\xBF\x01'\"<>&a", -1, CPLES_XML_BUT_QUOTES));
  EXPECT_STREQ("&#xFEFF;'\"&lt;&gt;&amp;a", esc.get());

  // Hex and decimal numeric entities are decoded; an unterminated entity
  // (&#65 without ';') stops the decoding.
  std::unique_ptr<char, CplFreeDeleter> unesc(CPLUnescapeString(
      "&#x41;&#x6a;&#x3F;&#65;&#65&bogus;normal", &len, CPLES_XML));
  EXPECT_STREQ("Aj?A", unesc.get());
  EXPECT_EQ(4, len);

  // An unknown named entity stops the decoding.
  unesc.reset(CPLUnescapeString("ok&bogus;normal", &len, CPLES_XML));
  EXPECT_STREQ("ok", unesc.get());
  EXPECT_EQ(2, len);

  // BackslashQuotable with \0 (an embedded NUL in the output) and a trailing
  // backslash (ignored).
  unesc.reset(CPLUnescapeString("a\\0b\\nc\\", &len, CPLES_BackslashQuotable));
  ASSERT_EQ(5, len);
  EXPECT_EQ(0, memcmp("a\0b\nc", unesc.get(), 5));

  // URL with lowercase/uppercase hex, '+', and invalid %ZZ (decoded as 0).
  unesc.reset(CPLUnescapeString("a+b%2a%2A%a0%A0%ZZ", &len, CPLES_URL));
  ASSERT_EQ(8, len);
  EXPECT_EQ(0, memcmp("a b**\xa0\xa0\0", unesc.get(), 8));

  // SQLI.
  esc.reset(CPLEscapeString("a\"b'c", -1, CPLES_SQLI));
  EXPECT_STREQ("a\"\"b'c", esc.get());
  unesc.reset(CPLUnescapeString("a\"\"b", &len, CPLES_SQLI));
  EXPECT_STREQ("a\"b", unesc.get());

  // CSV with CPLES_CSV_FORCE_QUOTING.
  esc.reset(CPLEscapeString(" leading_space", -1, CPLES_CSV));
  EXPECT_STREQ(" leading_space", esc.get());

  esc.reset(CPLEscapeString("", -1, CPLES_CSV_FORCE_QUOTING));
  EXPECT_STREQ("\"\"", esc.get());

  esc.reset(CPLEscapeString("plain", -1, CPLES_CSV_FORCE_QUOTING));
  EXPECT_STREQ("\"plain\"", esc.get());

  esc.reset(CPLEscapeString("with\"quote", -1, CPLES_CSV_FORCE_QUOTING));
  EXPECT_STREQ("\"with\"\"quote\"", esc.get());

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
  StringListPtr cmd(
      CSLParseCommandLine(
          "prog \"arg1 with \\\" quote\" 'arg2 with \\' quote' \\\\ plain"),
      CSLDestroy);
  EXPECT_GE(CSLCount(cmd.get()), 4);

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
