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

// Unit tests for //third_party/netcdf/ncgen/list.c.
//
// Exercises List creation, capacity growth, indexing, insertion, removal,
// deduplication, cloning, and null-pointer boundary handling.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/list.c

#include <array>
#include <cstddef>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"

namespace netcdf {
namespace {

TEST(ListTest, NullAndEmptyListGuardsReturnSafeDefaults) {
  int dummy = 42;

  EXPECT_EQ(1, listnull(nullptr));
  EXPECT_EQ(0, listnull(&dummy));
  EXPECT_EQ(1, listfree(nullptr));
  EXPECT_EQ(0, listsetalloc(nullptr, 4));
  EXPECT_EQ(0, listsetlength(nullptr, 4));
  EXPECT_EQ(nullptr, listget(nullptr, 0));
  EXPECT_EQ(0, listset(nullptr, 0, &dummy));
  EXPECT_EQ(0, listinsert(nullptr, 0, &dummy));
  EXPECT_EQ(0, listpush(nullptr, &dummy));
  EXPECT_EQ(nullptr, listpop(nullptr));
  EXPECT_EQ(nullptr, listtop(nullptr));
  EXPECT_EQ(nullptr, listremove(nullptr, 0));
  EXPECT_EQ(0, listcontains(nullptr, &dummy));
  EXPECT_EQ(0, listelemremove(nullptr, &dummy));
  EXPECT_EQ(1, listunique(nullptr));
  EXPECT_EQ(0UL, listlength(static_cast<List*>(nullptr)));
  EXPECT_EQ(nullptr, listcontents(static_cast<List*>(nullptr)));

  List* empty = listnew();
  ASSERT_NE(nullptr, empty);
  const absl::Cleanup free_empty([&]() { listfree(empty); });
  EXPECT_EQ(0UL, listlength(empty));
  EXPECT_EQ(nullptr, listcontents(empty));
  EXPECT_EQ(nullptr, listget(empty, 0));
  EXPECT_EQ(nullptr, listpop(empty));
  EXPECT_EQ(nullptr, listtop(empty));
  EXPECT_EQ(nullptr, listremove(empty, 0));
  EXPECT_EQ(0, listelemremove(empty, &dummy));
  EXPECT_EQ(1, listunique(empty));
  EXPECT_EQ(0, listcontains(empty, &dummy));
}

TEST(ListTest, SetAllocSetLengthAndClearPreserveElements) {
  int a = 1;
  int b = 2;
  int c = 3;

  List* l = listnew();
  ASSERT_NE(nullptr, l);
  const absl::Cleanup free_l([&]() { listfree(l); });

  // sz == 0 when length == 0 allocates DEFAULTALLOC (16).
  EXPECT_EQ(1, listsetalloc(l, 0));
  EXPECT_EQ(16UL, l->alloc);

  // Requesting smaller or equal capacity keeps existing buffer.
  EXPECT_EQ(1, listsetalloc(l, 8));
  EXPECT_EQ(16UL, l->alloc);

  // Growing when length == 0 skips copying old content.
  EXPECT_EQ(1, listsetalloc(l, 20));
  EXPECT_EQ(20UL, l->alloc);

  EXPECT_EQ(1, listsetlength(l, 2));
  EXPECT_EQ(2UL, listlength(l));
  EXPECT_EQ(1, listset(l, 0, &a));
  EXPECT_EQ(1, listset(l, 1, &b));
  EXPECT_EQ(0, listset(l, 2, &c));

  // Growing past alloc when length > 0 preserves existing elements.
  EXPECT_EQ(1, listsetlength(l, 24));
  EXPECT_GE(l->alloc, 24UL);
  EXPECT_EQ(24UL, listlength(l));
  EXPECT_EQ(&a, listget(l, 0));
  EXPECT_EQ(&b, listget(l, 1));
  EXPECT_EQ(nullptr, listget(l, 24));

  // Verify macro helpers listextend, listcontents, and listclear.
  EXPECT_NE(nullptr, listcontents(l));
  EXPECT_EQ(1, listextend(l, 8));
  EXPECT_GE(l->alloc, 32UL);
  EXPECT_EQ(1, listclear(l));
  EXPECT_EQ(0UL, listlength(l));
}

TEST(ListTest, PushPopTopInsertAndRemoveMaintainOrder) {
  std::array<int, 20> items{};
  for (size_t i = 0; i < items.size(); ++i) {
    items[i] = static_cast<int>(i);
  }

  List* l = listnew();
  ASSERT_NE(nullptr, l);
  const absl::Cleanup free_l([&]() { listfree(l); });

  // Push > DEFAULTALLOC (16) elements to trigger automatic doubling.
  for (int& item : items) {
    EXPECT_EQ(1, listpush(l, &item));
  }
  EXPECT_EQ(items.size(), listlength(l));
  EXPECT_GE(l->alloc, 32UL);

  EXPECT_EQ(&items[19], listtop(l));
  EXPECT_EQ(items.size(), listlength(l));
  EXPECT_EQ(&items[19], listpop(l));
  EXPECT_EQ(items.size() - 1, listlength(l));

  // Out-of-bounds insert/remove return failure/nullptr.
  int extra_front = 100;
  int extra_tail = 200;
  EXPECT_EQ(0, listinsert(l, 999, &extra_front));
  EXPECT_EQ(nullptr, listremove(l, 999));

  // Insert at front (shifts everything up) and at tail (no shift).
  EXPECT_EQ(1, listinsert(l, 0, &extra_front));
  EXPECT_EQ(1, listinsert(l, listlength(l), &extra_tail));
  EXPECT_EQ(&extra_front, listget(l, 0));
  EXPECT_EQ(&items[0], listget(l, 1));
  EXPECT_EQ(&extra_tail, listget(l, listlength(l) - 1));

  // Remove from front and end.
  EXPECT_EQ(&extra_front, listremove(l, 0));
  EXPECT_EQ(&extra_tail, listremove(l, listlength(l) - 1));
  EXPECT_EQ(&items[0], listget(l, 0));
  EXPECT_EQ(&items[18], listget(l, listlength(l) - 1));

  // Inserting into a freshly allocated list allocates content on demand.
  List* fresh = listnew();
  ASSERT_NE(nullptr, fresh);
  const absl::Cleanup free_fresh([&]() { listfree(fresh); });
  EXPECT_EQ(1, listinsert(fresh, 0, &extra_front));
  EXPECT_EQ(1UL, listlength(fresh));
  EXPECT_EQ(&extra_front, listget(fresh, 0));
}

TEST(ListTest, ContainsElemRemoveUniqueDupAndClone) {
  int a = 1;
  int b = 2;
  int c = 3;
  int d = 4;
  int absent = 99;

  List* l = listnew();
  ASSERT_NE(nullptr, l);
  const absl::Cleanup free_l([&]() { listfree(l); });

  ASSERT_EQ(1, listpush(l, &a));
  ASSERT_EQ(1, listpush(l, &b));
  ASSERT_EQ(1, listpush(l, &a));
  ASSERT_EQ(1, listpush(l, &c));
  ASSERT_EQ(1, listpush(l, &b));
  ASSERT_EQ(1, listpush(l, &d));

  EXPECT_EQ(1, listcontains(l, &c));
  EXPECT_EQ(0, listcontains(l, &absent));

  EXPECT_EQ(0, listelemremove(l, &absent));
  // Removes only the first occurrence of &a.
  EXPECT_EQ(1, listelemremove(l, &a));
  ASSERT_EQ(5UL, listlength(l));
  EXPECT_EQ(&b, listget(l, 0));
  EXPECT_EQ(&a, listget(l, 1));
  EXPECT_EQ(&c, listget(l, 2));
  EXPECT_EQ(&b, listget(l, 3));
  EXPECT_EQ(&d, listget(l, 4));

  // Deduplicate removes the second &b while preserving order.
  EXPECT_EQ(1, listunique(l));
  ASSERT_EQ(4UL, listlength(l));
  EXPECT_EQ(&b, listget(l, 0));
  EXPECT_EQ(&a, listget(l, 1));
  EXPECT_EQ(&c, listget(l, 2));
  EXPECT_EQ(&d, listget(l, 3));

  // Duplicate raw NULL-terminated array and verify independent clone.
  void** dup = listdup(l);
  ASSERT_NE(nullptr, dup);
  const absl::Cleanup free_dup([&]() { efree(dup); });
  EXPECT_EQ(&b, dup[0]);
  EXPECT_EQ(&a, dup[1]);
  EXPECT_EQ(&c, dup[2]);
  EXPECT_EQ(&d, dup[3]);
  EXPECT_EQ(nullptr, dup[4]);

  List* clone = listclone(l);
  ASSERT_NE(nullptr, clone);
  const absl::Cleanup free_clone([&]() { listfree(clone); });
  ASSERT_EQ(4UL, listlength(clone));
  EXPECT_EQ(&b, listget(clone, 0));
  EXPECT_EQ(&a, listget(clone, 1));
  EXPECT_EQ(&c, listget(clone, 2));
  EXPECT_EQ(&d, listget(clone, 3));

  EXPECT_EQ(&d, listpop(l));
  EXPECT_EQ(3UL, listlength(l));
  EXPECT_EQ(4UL, listlength(clone));
}

}  // namespace
}  // namespace netcdf
