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

// Unit tests for //third_party/netcdf/libdispatch/nclist.c.
//
// Exercises dynamic pointer list creation, resizing, element mutation,
// searching, deduplication, shallow/deep cloning, ownership transfer, and
// internal memmovex shifting. Note that nclistfail() calls abort() under
// NCLISTDEBUG=1 in upstream nclist.c, so non-fatal null/empty API contracts
// are verified directly.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/nclist.c

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
#include "third_party/netcdf/include/nclist.h"
}  // extern "C"

namespace netcdf {
namespace {

struct NcListDeleter {
  void operator()(NClist* list) const { nclistfree(list); }
};

struct NcListFreeAllDeleter {
  void operator()(NClist* list) const { nclistfreeall(list); }
};

struct CFreeDeleter {
  void operator()(void* ptr) const { std::free(ptr); }
};

using ScopedNcList = std::unique_ptr<NClist, NcListDeleter>;
using ScopedNcListAll = std::unique_ptr<NClist, NcListFreeAllDeleter>;
using ScopedMalloc = std::unique_ptr<void, CFreeDeleter>;

char* DupCString(std::string_view s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out != nullptr) {
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
  }
  return out;
}

TEST(NclistTest, IsNullDetectsNullAndNonNull) {
  int value = 42;
  EXPECT_EQ(1, nclistisnull(nullptr));
  EXPECT_EQ(0, nclistisnull(&value));
}

TEST(NclistTest, EmptyAndNullSafeOperations) {
  int dummy = 7;
  EXPECT_EQ(nullptr, nclistcontents(static_cast<NClist*>(nullptr)));
  EXPECT_EQ(0U, nclistlength(static_cast<NClist*>(nullptr)));
  EXPECT_EQ(0, nclistcontains(nullptr, &dummy));
  EXPECT_EQ(0, nclistmatch(nullptr, "alpha", 1));
  EXPECT_EQ(nullptr, nclistclone(nullptr, 0));
  EXPECT_EQ(nullptr, nclistclone(nullptr, 1));
  EXPECT_EQ(nullptr, nclistextract(nullptr));
  EXPECT_EQ(1, nclistclearall(nullptr));
  EXPECT_EQ(1, nclistfree(nullptr));
  EXPECT_EQ(1, nclistfreeall(nullptr));

  ScopedNcList list(nclistnew());
  ASSERT_NE(nullptr, list.get());
  EXPECT_EQ(0U, list->alloc);
  EXPECT_EQ(0U, nclistlength(list.get()));
  EXPECT_EQ(nullptr, nclistcontents(list.get()));
  EXPECT_EQ(1, list->extendible);

  EXPECT_EQ(nullptr, nclistget(list.get(), 0));
  EXPECT_EQ(nullptr, nclistpop(list.get()));
  EXPECT_EQ(nullptr, nclisttop(list.get()));
  EXPECT_EQ(nullptr, nclistremove(list.get(), 0));
  EXPECT_EQ(0, nclistcontains(list.get(), &dummy));
  EXPECT_EQ(0, nclistmatch(list.get(), "alpha", 0));
  EXPECT_EQ(0, nclistelemremove(list.get(), &dummy));
  EXPECT_EQ(1, nclistunique(list.get()));
  EXPECT_EQ(1, nclistnull(list.get()));
}

TEST(NclistTest, PushPopTopResizeAndExtract) {
  ScopedNcList list(nclistnew());
  ASSERT_NE(nullptr, list.get());

  std::array<int, 40> storage{};
  for (size_t i = 0; i < storage.size(); ++i) {
    storage[i] = static_cast<int>(i * 3);
    ASSERT_EQ(1, nclistpush(list.get(), &storage[i]));
    EXPECT_EQ(&storage[i], nclisttop(list.get()));
  }
  EXPECT_EQ(storage.size(), nclistlength(list.get()));
  EXPECT_GE(list->alloc, storage.size());
  for (size_t i = 0; i < storage.size(); ++i) {
    EXPECT_EQ(&storage[i], nclistget(list.get(), i));
  }

  // Requesting an allocation smaller than current alloc is a no-op.
  const size_t prev_alloc = list->alloc;
  EXPECT_EQ(1, nclistsetalloc(list.get(), 4));
  EXPECT_EQ(prev_alloc, list->alloc);

  // Extending alloc preserves existing elements and length.
  EXPECT_EQ(1, nclistextend(list.get(), 20));
  EXPECT_GE(list->alloc, prev_alloc + 20);
  EXPECT_EQ(storage.size(), nclistlength(list.get()));
  EXPECT_EQ(&storage[0], nclistget(list.get(), 0));
  EXPECT_EQ(&storage[39], nclistget(list.get(), 39));

  // Pop in LIFO order down to empty.
  for (size_t i = storage.size(); i > 0; --i) {
    EXPECT_EQ(&storage[i - 1], nclistpop(list.get()));
  }
  EXPECT_EQ(0U, nclistlength(list.get()));
  EXPECT_EQ(nullptr, nclistpop(list.get()));
  EXPECT_EQ(nullptr, nclisttop(list.get()));

  // Re-push and extract internal array ownership.
  ASSERT_EQ(1, nclistpush(list.get(), &storage[5]));
  ScopedMalloc extracted(nclistextract(list.get()));
  ASSERT_NE(nullptr, extracted.get());
  EXPECT_EQ(&storage[5], static_cast<void**>(extracted.get())[0]);
  EXPECT_EQ(0U, nclistlength(list.get()));
  EXPECT_EQ(0U, list->alloc);
  EXPECT_EQ(nullptr, list->content);
}

TEST(NclistTest, SetAndSetLengthZeroExtendAndShrink) {
  ScopedNcList list(nclistnew());
  ASSERT_NE(nullptr, list.get());

  int a = 1;
  int b = 2;
  int c = 3;

  EXPECT_EQ(1, nclistsetlength(list.get(), 5));
  EXPECT_EQ(5U, nclistlength(list.get()));
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(nullptr, nclistget(list.get(), i));
  }

  // Overwriting an existing slot keeps length unchanged.
  EXPECT_EQ(1, nclistset(list.get(), 2, &a));
  EXPECT_EQ(5U, nclistlength(list.get()));
  EXPECT_EQ(&a, nclistget(list.get(), 2));

  // Setting past current length and alloc grows and zero-fills the gap.
  EXPECT_EQ(1, nclistset(list.get(), 20, &b));
  EXPECT_EQ(21U, nclistlength(list.get()));
  EXPECT_EQ(&b, nclistget(list.get(), 20));
  for (size_t i = 3; i < 20; ++i) {
    EXPECT_EQ(nullptr, nclistget(list.get(), i));
  }

  EXPECT_EQ(1, nclistset(list.get(), 2, &c));
  EXPECT_EQ(&c, nclistget(list.get(), 2));
  EXPECT_EQ(nullptr, nclistget(list.get(), 21));

  // Shrinking then re-expanding clears newly exposed slots.
  EXPECT_EQ(1, nclistsetlength(list.get(), 3));
  EXPECT_EQ(3U, nclistlength(list.get()));
  EXPECT_EQ(1, nclistsetlength(list.get(), 6));
  EXPECT_EQ(6U, nclistlength(list.get()));
  EXPECT_EQ(&c, nclistget(list.get(), 2));
  EXPECT_EQ(nullptr, nclistget(list.get(), 3));
  EXPECT_EQ(nullptr, nclistget(list.get(), 5));

  EXPECT_EQ(1, nclistclear(list.get()));
  EXPECT_EQ(0U, nclistlength(list.get()));
}

TEST(NclistTest, InsertAndRemoveShiftElements) {
  ScopedNcList list(nclistnew());
  ASSERT_NE(nullptr, list.get());

  int a = 10;
  int b = 20;
  int c = 30;
  int d = 40;

  // Insert into empty list at index 0.
  ASSERT_EQ(1, nclistinsert(list.get(), 0, &a));
  // Insert at head (shifts existing elements right via memmovex d > s).
  ASSERT_EQ(1, nclistinsert(list.get(), 0, &b));
  // Insert into the middle.
  ASSERT_EQ(1, nclistinsert(list.get(), 1, &c));
  // Insert at tail (zero-byte memmovex).
  ASSERT_EQ(1, nclistinsert(list.get(), 3, &d));

  ASSERT_EQ(4U, nclistlength(list.get()));
  EXPECT_THAT(std::vector<void*>(list->content, list->content + list->length),
              ::testing::ElementsAre(&b, &c, &a, &d));

  // Out-of-bounds remove returns nullptr.
  EXPECT_EQ(nullptr, nclistremove(list.get(), 4));

  // Remove middle element (shifts left via memmovex d < s).
  EXPECT_EQ(&c, nclistremove(list.get(), 1));
  // Remove tail element (zero-byte memmovex).
  EXPECT_EQ(&d, nclistremove(list.get(), 2));

  ASSERT_EQ(2U, nclistlength(list.get()));
  EXPECT_THAT(std::vector<void*>(list->content, list->content + list->length),
              ::testing::ElementsAre(&b, &a));
}

TEST(NclistTest, ContainsMatchElemRemoveAndUnique) {
  ScopedNcList list(nclistnew());
  ASSERT_NE(nullptr, list.get());

  char s1[] = "Alpha";
  char s2[] = "Beta";
  char s3[] = "Gamma";
  char s4[] = "Delta";

  ASSERT_EQ(1, nclistpush(list.get(), s1));
  ASSERT_EQ(1, nclistpush(list.get(), s2));
  ASSERT_EQ(1, nclistpush(list.get(), s1));
  ASSERT_EQ(1, nclistpush(list.get(), s3));
  ASSERT_EQ(1, nclistpush(list.get(), s2));

  EXPECT_EQ(1, nclistcontains(list.get(), s2));
  EXPECT_EQ(0, nclistcontains(list.get(), s4));

  // Case-sensitive and case-insensitive string matching.
  EXPECT_EQ(1, nclistmatch(list.get(), "Alpha", 1));
  EXPECT_EQ(0, nclistmatch(list.get(), "alpha", 1));
  EXPECT_EQ(1, nclistmatch(list.get(), "alpha", 0));
  EXPECT_EQ(1, nclistmatch(list.get(), "BETA", 0));
  EXPECT_EQ(0, nclistmatch(list.get(), "Epsilon", 0));

  // nclistelemremove only removes the first matching pointer.
  EXPECT_EQ(0, nclistelemremove(list.get(), s4));
  EXPECT_EQ(1, nclistelemremove(list.get(), s1));
  EXPECT_THAT(std::vector<void*>(list->content, list->content + list->length),
              ::testing::ElementsAre(s2, s1, s3, s2));

  // nclistunique removes remaining duplicates while preserving order.
  EXPECT_EQ(1, nclistunique(list.get()));
  EXPECT_THAT(std::vector<void*>(list->content, list->content + list->length),
              ::testing::ElementsAre(s2, s1, s3));
}

TEST(NclistTest, CloneShallowAndDeepAndNullTerminator) {
  ScopedNcListAll empty(nclistnew());
  ASSERT_NE(nullptr, empty.get());
  ScopedNcList empty_clone(nclistclone(empty.get(), 0));
  ASSERT_NE(nullptr, empty_clone.get());
  EXPECT_EQ(0U, nclistlength(empty_clone.get()));
  EXPECT_EQ(nullptr, empty_clone->content[0]);

  ScopedNcListAll list(nclistnew());
  ASSERT_NE(nullptr, list.get());
  ASSERT_EQ(1, nclistpush(list.get(), DupCString("one")));
  ASSERT_EQ(1, nclistpush(list.get(), DupCString("two")));
  ASSERT_EQ(1, nclistpush(list.get(), DupCString("three")));

  // Shallow clone shares string pointers and sets trailing NULL.
  ScopedNcList shallow(nclistclone(list.get(), 0));
  ASSERT_NE(nullptr, shallow.get());
  ASSERT_EQ(3U, nclistlength(shallow.get()));
  EXPECT_EQ(nclistget(list.get(), 0), nclistget(shallow.get(), 0));
  EXPECT_EQ(nclistget(list.get(), 2), nclistget(shallow.get(), 2));
  EXPECT_EQ(nullptr, shallow->content[3]);

  // Deep clone duplicates each string independently.
  ScopedNcListAll deep(nclistclone(list.get(), 1));
  ASSERT_NE(nullptr, deep.get());
  ASSERT_EQ(3U, nclistlength(deep.get()));
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_NE(nclistget(list.get(), i), nclistget(deep.get(), i));
    EXPECT_STREQ(static_cast<const char*>(nclistget(list.get(), i)),
                 static_cast<const char*>(nclistget(deep.get(), i)));
  }
  EXPECT_EQ(nullptr, deep->content[3]);

  // nclistnull appends an uncounted trailing NULL pointer.
  EXPECT_EQ(1, nclistnull(list.get()));
  EXPECT_EQ(3U, nclistlength(list.get()));
  EXPECT_EQ(nullptr, nclistcontents(list.get())[3]);
}

TEST(NclistTest, ClearAllAndSetContentsOwnership) {
  {
    ScopedNcListAll list(nclistnew());
    ASSERT_NE(nullptr, list.get());
    ASSERT_EQ(1, nclistpush(list.get(), std::malloc(8)));
    ASSERT_EQ(1, nclistpush(list.get(), nullptr));
    ASSERT_EQ(1, nclistpush(list.get(), std::malloc(16)));

    EXPECT_EQ(1, nclistclearall(list.get()));
    EXPECT_EQ(0U, nclistlength(list.get()));
  }

  // Replacing an extendible buffer frees the old buffer and clears extendible.
  ScopedNcList list(nclistnew());
  ASSERT_NE(nullptr, list.get());
  ASSERT_EQ(1, nclistsetalloc(list.get(), 4));

  int x = 1;
  int y = 2;
  std::array<void*, 2> external_buf1 = {&x, &y};
  std::array<void*, 1> external_buf2 = {&y};
  EXPECT_EQ(1, nclistsetcontents(list.get(), external_buf1.data(),
                                 external_buf1.size(), external_buf1.size()));
  EXPECT_EQ(0, list->extendible);
  EXPECT_EQ(2U, nclistlength(list.get()));
  EXPECT_EQ(&x, nclistget(list.get(), 0));

  // Replacing a non-extendible buffer does not free external_buf1.
  EXPECT_EQ(1, nclistsetcontents(list.get(), external_buf2.data(),
                                 external_buf2.size(), external_buf2.size()));
  EXPECT_EQ(1U, nclistlength(list.get()));
  EXPECT_EQ(&y, nclistget(list.get(), 0));
  // ScopedNcList calls nclistfree(), which must NOT free external_buf2.
}

}  // namespace
}  // namespace netcdf
