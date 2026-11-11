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

// Unit tests for //third_party/netcdf/ncdump/list.c.
//
// Covers creation, capacity resizing, stack operations, indexed insertion and
// removal, value lookup/removal, deduplication, cloning, and deep freeing.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/list.c

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/ncdump_globals_stub.h"

namespace netcdf {
namespace {

struct ListDeleter {
  void operator()(List* l) const { listfree(l); }
};
using ScopedList = std::unique_ptr<List, ListDeleter>;

struct ListAllDeleter {
  void operator()(List* l) const { listfreeall(l); }
};
using ScopedListAll = std::unique_ptr<List, ListAllDeleter>;

struct FreeDeleter {
  void operator()(void* p) const { std::free(p); }
};
using ScopedVoidArray = std::unique_ptr<void*, FreeDeleter>;

TEST(ListTest, NullAndEmptyListGuardsBehaveSafely) {
  ncdump_stub_reset_globals();
  absl::Cleanup cleanup = [] { ncdump_stub_reset_globals(); };

  int dummy = 42;
  EXPECT_EQ(1, listnull(nullptr));
  EXPECT_EQ(0, listnull(&dummy));

  EXPECT_EQ(1, listfree(nullptr));
  EXPECT_EQ(1, listfreeall(nullptr));
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
  EXPECT_EQ(nullptr, listcontents(static_cast<List*>(nullptr)));
  EXPECT_EQ(0, listlength(static_cast<List*>(nullptr)));

  ScopedList empty(listnew());
  ASSERT_NE(nullptr, empty.get());
  EXPECT_EQ(0, listlength(empty.get()));
  EXPECT_EQ(nullptr, listcontents(empty.get()));
  EXPECT_EQ(nullptr, listget(empty.get(), 0));
  EXPECT_EQ(0, listset(empty.get(), 0, &dummy));
  EXPECT_EQ(nullptr, listpop(empty.get()));
  EXPECT_EQ(nullptr, listtop(empty.get()));
  EXPECT_EQ(nullptr, listremove(empty.get(), 0));
  EXPECT_EQ(0, listcontains(empty.get(), &dummy));
  EXPECT_EQ(0, listelemremove(empty.get(), &dummy));
  EXPECT_EQ(1, listunique(empty.get()));
  // Releasing via explicit listfree verifies cleanup when content == nullptr.
  EXPECT_EQ(1, listfree(empty.release()));
}

TEST(ListTest, PushPopTopGetSetAndAutoGrowth) {
  ncdump_stub_reset_globals();
  absl::Cleanup cleanup = [] { ncdump_stub_reset_globals(); };

  std::array<int, 25> vals{};
  for (int i = 0; i < static_cast<int>(vals.size()); ++i) {
    vals[static_cast<size_t>(i)] = i;
  }

  ScopedList list(listnew());
  ASSERT_NE(nullptr, list.get());

  // Pushing 20 items triggers initial DEFAULTALLOC (16) and doubling to 32.
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(1, listpush(list.get(), &vals[static_cast<size_t>(i)]));
  }
  EXPECT_EQ(20, listlength(list.get()));
  EXPECT_GE(list->alloc, 20UL);
  EXPECT_NE(nullptr, listcontents(list.get()));
  EXPECT_EQ(&vals[19], listtop(list.get()));
  EXPECT_EQ(&vals[0], listget(list.get(), 0));
  EXPECT_EQ(nullptr, listget(list.get(), 20));

  EXPECT_EQ(0, listset(list.get(), 20, &vals[20]));
  EXPECT_EQ(1, listset(list.get(), 5, &vals[24]));
  EXPECT_EQ(&vals[24], listget(list.get(), 5));

  EXPECT_EQ(1, listcontains(list.get(), &vals[24]));
  EXPECT_EQ(0, listcontains(list.get(), &vals[23]));

  EXPECT_EQ(&vals[19], listpop(list.get()));
  EXPECT_EQ(&vals[18], listpop(list.get()));
  EXPECT_EQ(18, listlength(list.get()));
}

TEST(ListTest, SetAllocSetLengthExtendAndClear) {
  ncdump_stub_reset_globals();
  absl::Cleanup cleanup = [] { ncdump_stub_reset_globals(); };

  ScopedList list(listnew());
  ASSERT_NE(nullptr, list.get());

  EXPECT_EQ(1, listsetalloc(list.get(), 4));
  EXPECT_EQ(4UL, list->alloc);
  // Requesting smaller/equal capacity is a no-op returning TRUE.
  EXPECT_EQ(1, listsetalloc(list.get(), 2));
  EXPECT_EQ(4UL, list->alloc);
  // Growing while length == 0 frees prior content buffer cleanly.
  EXPECT_EQ(1, listsetalloc(list.get(), 8));
  EXPECT_EQ(8UL, list->alloc);

  EXPECT_EQ(1, listsetlength(list.get(), 3));
  EXPECT_EQ(3, listlength(list.get()));
  // Setting length above current allocation expands capacity automatically.
  EXPECT_EQ(1, listsetlength(list.get(), 12));
  EXPECT_EQ(12, listlength(list.get()));
  EXPECT_GE(list->alloc, 12UL);

  const unsigned long prev_alloc = list->alloc;
  EXPECT_EQ(1, listextend(list.get(), 4));
  EXPECT_GE(list->alloc, prev_alloc + 4);

  EXPECT_EQ(1, listclear(list.get()));
  EXPECT_EQ(0, listlength(list.get()));
}

TEST(ListTest, InsertRemoveElemRemoveAndUnique) {
  ncdump_stub_reset_globals();
  absl::Cleanup cleanup = [] { ncdump_stub_reset_globals(); };

  std::array<int, 6> v{};

  ScopedList list(listnew());
  ASSERT_NE(nullptr, list.get());

  // Out-of-bounds insertion fails.
  EXPECT_EQ(0, listinsert(list.get(), 1, &v[0]));
  // Insert at 0, front, and tail.
  EXPECT_EQ(1, listinsert(list.get(), 0, &v[1]));
  EXPECT_EQ(1, listinsert(list.get(), 0, &v[0]));
  EXPECT_EQ(1, listinsert(list.get(), 2, &v[2]));
  ASSERT_EQ(3, listlength(list.get()));
  EXPECT_EQ(&v[0], listget(list.get(), 0));
  EXPECT_EQ(&v[1], listget(list.get(), 1));
  EXPECT_EQ(&v[2], listget(list.get(), 2));

  // Verify listinsert expands a full 1-slot buffer (length > 0 path).
  ScopedList tight(listnew());
  ASSERT_NE(nullptr, tight.get());
  ASSERT_EQ(1, listsetalloc(tight.get(), 1));
  ASSERT_EQ(1, listpush(tight.get(), &v[0]));
  ASSERT_EQ(1, listinsert(tight.get(), 0, &v[1]));
  EXPECT_EQ(2, listlength(tight.get()));
  EXPECT_EQ(&v[1], listget(tight.get(), 0));
  EXPECT_EQ(&v[0], listget(tight.get(), 1));

  // Out-of-bounds and middle removal.
  EXPECT_EQ(nullptr, listremove(list.get(), 3));
  EXPECT_EQ(&v[1], listremove(list.get(), 1));
  ASSERT_EQ(2, listlength(list.get()));
  EXPECT_EQ(&v[0], listget(list.get(), 0));
  EXPECT_EQ(&v[2], listget(list.get(), 1));

  // Add duplicates and exercise listelemremove + listunique.
  ASSERT_EQ(1, listpush(list.get(), &v[3]));
  ASSERT_EQ(1, listpush(list.get(), &v[0]));
  ASSERT_EQ(1, listpush(list.get(), &v[2]));
  // Current contents: [v0, v2, v3, v0, v2].
  EXPECT_EQ(0, listelemremove(list.get(), &v[5]));
  EXPECT_EQ(1, listelemremove(list.get(), &v[0]));
  // First v0 removed; remaining: [v2, v3, v0, v2].
  EXPECT_EQ(1, listcontains(list.get(), &v[0]));

  ASSERT_EQ(1, listpush(list.get(), &v[3]));
  // Current contents: [v2, v3, v0, v2, v3].
  EXPECT_EQ(1, listunique(list.get()));
  ASSERT_EQ(3, listlength(list.get()));
  const std::array<void*, 3> unique_view = {
      listget(list.get(), 0), listget(list.get(), 1), listget(list.get(), 2)};
  EXPECT_THAT(unique_view, ::testing::ElementsAre(&v[2], &v[3], &v[0]));
}

TEST(ListTest, DupCloneAndFreeAll) {
  ncdump_stub_reset_globals();
  absl::Cleanup cleanup = [] { ncdump_stub_reset_globals(); };

  std::array<int, 3> v{};
  ScopedList list(listnew());
  ASSERT_NE(nullptr, list.get());
  ASSERT_EQ(1, listpush(list.get(), &v[0]));
  ASSERT_EQ(1, listpush(list.get(), &v[1]));

  ScopedVoidArray dup(listdup(list.get()));
  ASSERT_NE(nullptr, dup.get());
  EXPECT_EQ(&v[0], dup.get()[0]);
  EXPECT_EQ(&v[1], dup.get()[1]);
  EXPECT_EQ(nullptr, dup.get()[2]);

  ScopedList clone(listclone(list.get()));
  ASSERT_NE(nullptr, clone.get());
  EXPECT_EQ(2, listlength(clone.get()));
  EXPECT_EQ(&v[0], listget(clone.get(), 0));
  EXPECT_EQ(&v[1], listget(clone.get(), 1));

  ScopedListAll owned(listnew());
  ASSERT_NE(nullptr, owned.get());
  char* first = static_cast<char*>(std::malloc(6));
  char* second = static_cast<char*>(std::malloc(5));
  ASSERT_NE(nullptr, first);
  ASSERT_NE(nullptr, second);
  std::memcpy(first, "alpha", 6);
  std::memcpy(second, "beta", 5);
  ASSERT_EQ(1, listpush(owned.get(), first));
  ASSERT_EQ(1, listpush(owned.get(), nullptr));
  ASSERT_EQ(1, listpush(owned.get(), second));
  EXPECT_EQ(1, listfreeall(owned.release()));
}

}  // namespace
}  // namespace netcdf
