// Tests for GDAL's VSI virtual file system.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cplstringlist.cpp
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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <string>

#include "gunit.h"
#include "port/cpl_string.h"

namespace {

// Tests the CPLStringList class basic operations.
TEST(CplStringList, Basic) {
  CPLStringList list;
  ASSERT_EQ(0, list.size());
  ASSERT_EQ(0, list.Count());
  ASSERT_EQ(-1, list.FindString("zero"));
  list.AddString("zero");
  ASSERT_EQ(1, list.size());
  ASSERT_EQ(1, list.Count());
  ASSERT_EQ(0, list.FindString("zero"));

  list.Clear();
  ASSERT_EQ(0, list.size());
  list.AddString("zero");
  list.AddStringDirectly(strdup("two"));

  // Use a locally managed string to check for double free.
  std::unique_ptr<std::string> unique_str(new std::string("three"));
  list.AddString(unique_str->c_str());
  ASSERT_EQ(2, list.FindString("three"));
  ASSERT_EQ(2, list.PartialFindString("re"));

  list.InsertString(1, "one");
  ASSERT_EQ(1, list.FindString("one"));

  list.InsertString(0, "new zero");
  ASSERT_EQ(0, list.FindString("new zero"));

  list.InsertStringDirectly(1, strdup("new one"));
  ASSERT_EQ(1, list.FindString("new one"));

  // This test must me last.
  // Make sure memory is not double freed.
  char *str = strdup("check free");
  list.AddString(str);
  free(str);
}

// Tests the name=value operations.
TEST(CplStringList, NameAndSort) {
  CPLStringList list;

  list.AddNameValue("FILENAME_0", "a");
  ASSERT_EQ(0, list.FindString("FILENAME_0=a"));

  // TODO(schwehr): Test multiple calls to AddNameValue with the same name.

  list.SetNameValue("FILENAME_1", "b");
  ASSERT_EQ(1, list.FindString("FILENAME_1=b"));

  list.SetNameValue("FILENAME_1", "c");
  ASSERT_STREQ("c", list.FetchNameValue("FILENAME_1"));
  ASSERT_EQ(1, list.FindString("FILENAME_1=c"));

  // TODO(schwehr): Make sure there is only one.

  list.AddNameValue("FILENAME_8", "d");
  ASSERT_EQ(2, list.FindString("FILENAME_8=d"));

  // Not sorted, so expect 7 to be after 8.
  list.AddNameValue("FILENAME_7", "e");
  ASSERT_EQ(3, list.FindString("FILENAME_7=e"));

  ASSERT_EQ(false, bool(list.IsSorted()));
  list.Sort();
  ASSERT_EQ(true, bool(list.IsSorted()));
  ASSERT_EQ(2, list.FindString("FILENAME_7=e"));

  // Now that the list is sorted, add and set now insert as sorted.
  list.AddNameValue("FILENAME_6", "f");
  ASSERT_EQ(2, list.FindString("FILENAME_6=f"));

  list.SetNameValue("FILENAME_5", "g");
  ASSERT_EQ(2, list.FindString("FILENAME_5=g"));

  // Delete an entry.
  list.SetNameValue("FILENAME_5", nullptr);
  ASSERT_EQ(2, list.FindString("FILENAME_6=f"));
}

TEST(CplStringList, Boolean) {
  CPLStringList list;

  ASSERT_TRUE(list.FetchBoolean("does_not_exist", true));
  ASSERT_FALSE(list.FetchBoolean("does_not_exist", false));

  list.AddString("a");
  // This contradicts the comments for FetchBoolean.
  ASSERT_TRUE(list.FetchBoolean("a", true));
  ASSERT_FALSE(list.FetchBoolean("a", false));

  // Only these 4 values are false.
  list.SetNameValue("b", "NO");
  ASSERT_FALSE(list.FetchBoolean("b", true));
  list.SetNameValue("c", "FALSE");
  ASSERT_FALSE(list.FetchBoolean("c", true));
  list.SetNameValue("d", "OFF");
  ASSERT_FALSE(list.FetchBoolean("d", true));
  list.SetNameValue("e", "0");
  ASSERT_FALSE(list.FetchBoolean("e", true));

  // But case does not matter.
  list.SetNameValue("b", "no");
  ASSERT_FALSE(list.FetchBoolean("b", true));
  list.SetNameValue("c", "fAlse");
  ASSERT_FALSE(list.FetchBoolean("c", true));
  list.SetNameValue("d", "Off");
  ASSERT_FALSE(list.FetchBoolean("d", true));

  list.SetNameValue("f", "YES");
  ASSERT_TRUE(list.FetchBoolean("f", false));
  list.SetNameValue("g", "TRUE");
  ASSERT_TRUE(list.FetchBoolean("g", false));
  list.SetNameValue("h", "ON");
  ASSERT_TRUE(list.FetchBoolean("h", false));
  list.SetNameValue("i", "1");
  ASSERT_TRUE(list.FetchBoolean("i", false));
  list.SetNameValue("i", "-1");
  ASSERT_TRUE(list.FetchBoolean("i", false));

  list.SetNameValue("j", "");
  ASSERT_TRUE(list.FetchBoolean("j", false));
  list.SetNameValue("j", " ");
  ASSERT_TRUE(list.FetchBoolean("j", false));

  list.AddString("k=");
  ASSERT_TRUE(list.FetchBoolean("k", false));
}

// Tests looking up by position index.
TEST(CplStringList, ArrayOp) {
  CPLStringList list;
  ASSERT_EQ(nullptr, list[0]);
  ASSERT_EQ(nullptr, list[static_cast<size_t>(0)]);

  list.AddString("a");
  list.AddString("b");
  ASSERT_EQ(nullptr, list[-1]);
  ASSERT_EQ(nullptr, list[2]);
  ASSERT_EQ(nullptr, list[static_cast<size_t>(2)]);
}

// TODO(schwehr): Test the rest of cplstringlist.

}  // namespace
