// Tests for GDAL's VSI virtual file system.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cplstringlist.cpp
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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

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
  char* str = strdup("check free");
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

  ASSERT_FALSE(list.IsSorted());
  list.Sort();
  ASSERT_TRUE(list.IsSorted());
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
  EXPECT_STREQ("a", list[0]);
  EXPECT_STREQ("b", list[static_cast<size_t>(1)]);

  const CPLStringList& const_list = list;
  EXPECT_EQ(nullptr, const_list[-1]);
  EXPECT_EQ(nullptr, const_list[2]);
  EXPECT_STREQ("a", const_list[0]);
  EXPECT_STREQ("b", const_list[static_cast<size_t>(1)]);
}

TEST(CplStringList, ConstructorsAndAssignment) {
  // Constructor from char** with ownership.
  char** raw = CSLAddString(nullptr, "one");
  raw = CSLAddString(raw, "two");
  CPLStringList owned(raw, TRUE);
  EXPECT_EQ(2, owned.Count());
  EXPECT_STREQ("one", owned[0]);

  // Constructor from CSLConstList (copies).
  CSLConstList const_raw = owned.List();
  CPLStringList copied_from_const(const_raw);
  EXPECT_EQ(2, copied_from_const.Count());
  EXPECT_STREQ("two", copied_from_const[1]);

  // Constructor from std::vector<std::string>.
  CPLStringList empty_vec_list(std::vector<std::string>{});
  EXPECT_TRUE(empty_vec_list.empty());
  std::vector<std::string> vec = {"alpha", "beta", "gamma"};
  CPLStringList vec_list(vec);
  EXPECT_EQ(3, vec_list.Count());
  EXPECT_STREQ("beta", vec_list[1]);

  // Constructor from std::initializer_list<const char *>.
  CPLStringList init_list({"first", "second"});
  EXPECT_EQ(2, init_list.Count());
  EXPECT_STREQ("first", init_list[0]);
  EXPECT_STREQ("second", init_list[1]);

  // Copy constructor and copy assignment.
  CPLStringList copy_constructed(init_list);
  EXPECT_EQ(2, copy_constructed.Count());
  EXPECT_STREQ("first", copy_constructed[0]);

  // Self copy assignment.
  copy_constructed = *(&copy_constructed);
  EXPECT_EQ(2, copy_constructed.Count());

  // Move constructor and move assignment.
  CPLStringList move_constructed(std::move(copy_constructed));
  EXPECT_EQ(2, move_constructed.Count());
  EXPECT_STREQ("second", move_constructed[1]);

  CPLStringList move_assigned;
  move_assigned = std::move(move_constructed);
  EXPECT_EQ(2, move_assigned.Count());
  // Self move assignment.
  move_assigned = std::move(*(&move_assigned));
  EXPECT_EQ(2, move_assigned.Count());

  // operator=(CSLConstList) and operator=(char**).
  CPLStringList const_assigned;
  const_assigned = static_cast<CSLConstList>(vec_list.List());
  EXPECT_EQ(3, const_assigned.Count());
  // Self CSLConstList assignment.
  const_assigned = static_cast<CSLConstList>(const_assigned.List());
  EXPECT_EQ(3, const_assigned.Count());

  CPLStringList raw_assigned;
  raw_assigned = CSLDuplicate(vec_list.List());
  EXPECT_EQ(3, raw_assigned.Count());

  // BoundToConstList & MakeOurOwnCopy via mutation.
  const CPLStringList bound = CPLStringList::BoundToConstList(vec_list.List());
  EXPECT_STREQ("alpha", bound[0]);
  EXPECT_EQ(3, bound.Count());

  CPLStringList unowned(vec_list.List(), FALSE);
  // operator[] when nCount == -1 on const and non-const.
  const CPLStringList& const_unowned = unowned;
  EXPECT_STREQ("alpha", const_unowned[0]);

  CPLStringList unowned2(vec_list.List(), FALSE);
  EXPECT_STREQ("beta", unowned2[1]);
  // Trigger MakeOurOwnCopy by modifying unowned2.
  unowned2.AddString(std::string("delta"));
  EXPECT_EQ(4, unowned2.Count());
  EXPECT_EQ(3, vec_list.Count());

  // Assign with empty list (*papszList == nullptr).
  char* empty_array[1] = {nullptr};
  CPLStringList empty_assigned(empty_array, FALSE);
  EXPECT_EQ(0, empty_assigned.Count());

  // StealList.
  char** stolen = unowned2.StealList();
  EXPECT_EQ(0, unowned2.Count());
  EXPECT_EQ(4, CSLCount(stolen));
  CSLDestroy(stolen);
}

TEST(CplStringList, ReallocAndEdgeCases) {
  CPLStringList list;
  // Add > 25 strings to trigger VSI_REALLOC_VERBOSE in EnsureAllocation.
  for (int i = 0; i < 35; ++i) {
    list.AddString(std::string("item_") + std::to_string(i));
  }
  EXPECT_EQ(35, list.Count());

  // AddStringDirectly when nCount == -1.
  char** raw = CSLAddString(nullptr, "x");
  CPLStringList uncounted(raw, TRUE);
  uncounted.AddStringDirectly(CPLStrdup("y"));
  EXPECT_EQ(2, uncounted.Count());

  // InsertStringDirectly when nCount == -1.
  char** raw2 = CSLAddString(nullptr, "x");
  CPLStringList uncounted2(raw2, TRUE);
  uncounted2.InsertStringDirectly(0, CPLStrdup("w"));
  EXPECT_EQ(2, uncounted2.Count());
  EXPECT_STREQ("w", uncounted2[0]);

  // InsertStringDirectly out of bounds reports a CPLError, leaves the list
  // unchanged and does not take ownership of the string.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    char* not_inserted = CPLStrdup("bad");
    uncounted2.InsertStringDirectly(-1, not_inserted);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(2, uncounted2.Count());
    CPLFree(not_inserted);
  }

  // AddNameValue with nullptr key or value.
  int before = uncounted2.Count();
  uncounted2.AddNameValue(nullptr, "val");
  uncounted2.AddNameValue("key", nullptr);
  EXPECT_EQ(before, uncounted2.Count());

  // FetchNameValueDef.
  uncounted2.AddNameValue("mykey", "myval");
  EXPECT_STREQ("myval", uncounted2.FetchNameValueDef("mykey", "default"));
  EXPECT_STREQ("default", uncounted2.FetchNameValueDef("missing", "default"));
}

TEST(CplStringList, SortAndCompareKeyValueCases) {
  CPLStringList list;
  // Include lowercase keys, prefix keys (e.g. "ab=1" vs "abc=2"), and keys
  // without '='.
  list.AddString("abc=2");
  list.AddString("ab=1");
  list.AddString("ab=0");
  list.AddString("plain_b");
  list.AddString("plain_a");
  list.AddString("Zebra:10");
  list.Sort();
  EXPECT_TRUE(list.IsSorted());

  EXPECT_STREQ("10", list.FetchNameValue("zebra"));
  EXPECT_NE(-1, list.FindName("ab"));
  EXPECT_NE(-1, list.FindName("abc"));
  EXPECT_EQ(-1, list.FindName("a"));
  EXPECT_EQ(-1, list.FindName("abcd"));
  EXPECT_EQ(-1, list.FindName("zzz"));
}

TEST(CplStringList, Iterators) {
  CPLStringList list;
  list.AddString("flag_only");
  list.AddNameValue("k1", "v1");
  list.AddNameValue("k2", "v2");
  list.AddString("trailing_flag");

  // Test CSLIterator & CSLIteratorWrapper.
  std::vector<std::string> items;
  for (const char* s : cpl::Iterate(list)) {
    items.push_back(s);
  }
  ASSERT_EQ(4, items.size());

  auto wrapper = cpl::Iterate(list);
  auto it1 = wrapper.begin();
  auto it2 = wrapper.begin();
  auto end1 = wrapper.end();
  auto end2 = wrapper.end();
  EXPECT_TRUE(it1 == it2);
  EXPECT_TRUE(end1 == end2);
  EXPECT_FALSE(it1 == end1);
  EXPECT_FALSE(end1 == it1);

  // Test CSLNameValueIterator with bReturnNullKeyIfNotNameValue = false.
  std::vector<std::pair<std::string, std::string>> kv_pairs;
  for (const auto& [k, v] : cpl::IterateNameValue(list, false)) {
    kv_pairs.emplace_back(k, v);
  }
  ASSERT_EQ(2, kv_pairs.size());
  EXPECT_EQ("k1", kv_pairs[0].first);
  EXPECT_EQ("v1", kv_pairs[0].second);
  EXPECT_EQ("k2", kv_pairs[1].first);
  EXPECT_EQ("v2", kv_pairs[1].second);

  // Test CSLNameValueIterator with bReturnNullKeyIfNotNameValue = true.
  std::vector<std::pair<std::string, std::string>> all_pairs;
  for (const auto& [k, v] : cpl::IterateNameValue(list, true)) {
    all_pairs.emplace_back(k ? k : "<null>", v);
  }
  ASSERT_EQ(4, all_pairs.size());
  EXPECT_EQ("<null>", all_pairs[0].first);
  EXPECT_EQ("flag_only", all_pairs[0].second);
  EXPECT_EQ("<null>", all_pairs[3].first);
  EXPECT_EQ("trailing_flag", all_pairs[3].second);
}

}  // namespace
