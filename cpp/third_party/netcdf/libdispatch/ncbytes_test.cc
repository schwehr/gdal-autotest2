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

// Unit tests for //third_party/netcdf/libdispatch/ncbytes.c.
//
// Note: ncbytes.c hardcodes `#define NCBYTESDEBUG 1`, which aborts inside
// ncbytesfail() on precondition violations and has an upstream off-by-one loop
// bound in ncbytesprepend() when length == alloc. Tests exercise all normal,
// boundary, growth, ownership-transfer, and non-extendible paths safely without
// invoking abort() or out-of-bounds writes.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/ncbytes.c

#include <array>
#include <cstring>
#include <memory>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
#include "third_party/netcdf/include/ncbytes.h"
int ncbytesprepend(NCbytes* bb, char elem);
}  // extern "C"

namespace netcdf {
namespace {

struct NcBytesDeleter {
  void operator()(NCbytes* bb) const { ncbytesfree(bb); }
};
using ScopedNcBytes = std::unique_ptr<NCbytes, NcBytesDeleter>;

TEST(NcbytesTest, NewAndFreeLifecycle) {
  // ncbytesfree(nullptr) is a safe no-op.
  ncbytesfree(nullptr);

  ScopedNcBytes bb(ncbytesnew());
  ASSERT_NE(nullptr, bb.get());
  EXPECT_EQ(0UL, bb->alloc);
  EXPECT_EQ(0UL, bb->length);
  EXPECT_EQ(nullptr, bb->content);
  EXPECT_EQ(1, bb->extendible);

  EXPECT_EQ(0UL, ncbyteslength(bb.get()));
  EXPECT_EQ(0UL, ncbytesalloc(bb.get()));
  EXPECT_EQ(0, ncbytesavail(bb.get(), 1UL));
  EXPECT_EQ(0UL, ncbyteslength(static_cast<NCbytes*>(nullptr)));
  EXPECT_EQ(0UL, ncbytesalloc(static_cast<NCbytes*>(nullptr)));
  EXPECT_EQ(0, ncbytesavail(static_cast<NCbytes*>(nullptr), 1UL));
  EXPECT_EQ(0UL, ncbytesclear(static_cast<NCbytes*>(nullptr)));
}

TEST(NcbytesTest, SetAllocHandlesDefaultGrowthEarlyReturnAndPreservesData) {
  ScopedNcBytes bb(ncbytesnew());
  ASSERT_NE(nullptr, bb.get());

  // sz == 0 on fresh buffer allocates DEFAULTALLOC (1024).
  EXPECT_EQ(1, ncbytessetalloc(bb.get(), 0));
  EXPECT_EQ(1024UL, bb->alloc);
  EXPECT_EQ(1, ncbytesavail(bb.get(), 1UL));
  ASSERT_NE(nullptr, bb->content);

  // Duplicating an empty allocated buffer returns an empty NUL-terminated
  // string.
  EXPECT_EQ(1, ncbytesfill(bb.get(), 'x'));
  ScopedCString empty_dup = MakeScopedCString(ncbytesdup(bb.get()));
  ASSERT_NE(nullptr, empty_dup.get());
  EXPECT_STREQ("", empty_dup.get());

  // Requesting <= current alloc is a no-op returning TRUE.
  EXPECT_EQ(1, ncbytessetalloc(bb.get(), 512));
  EXPECT_EQ(1024UL, bb->alloc);

  // Populate data before growing so memcpy + free(old content) path runs.
  EXPECT_EQ(1, ncbytesappendn(bb.get(), "hello", 5));
  EXPECT_EQ(5UL, bb->length);

  // sz == 0 on allocated buffer doubles capacity (1024 -> 2048).
  EXPECT_EQ(1, ncbytessetalloc(bb.get(), 0));
  EXPECT_EQ(2048UL, bb->alloc);
  EXPECT_EQ(5UL, bb->length);
  EXPECT_EQ("hello", std::string_view(bb->content, bb->length));

  // Explicit larger size reallocates to exact requested capacity.
  EXPECT_EQ(1, ncbytesextend(bb.get(), 100UL));
  EXPECT_EQ(2148UL, bb->alloc);
  EXPECT_EQ("hello", std::string_view(bb->content, bb->length));
}

TEST(NcbytesTest, SetLengthAndFillManageLengthAndContents) {
  ScopedNcBytes bb(ncbytesnew());
  ASSERT_NE(nullptr, bb.get());

  // Growing past initial zero alloc allocates and sets length.
  EXPECT_EQ(1, ncbytessetlength(bb.get(), 8));
  EXPECT_EQ(8UL, bb->length);
  EXPECT_GE(bb->alloc, 8UL);

  EXPECT_EQ(1, ncbytesfill(bb.get(), 'Z'));
  EXPECT_EQ("ZZZZZZZZ", std::string_view(bb->content, bb->length));

  // Shrinking length preserves allocated capacity.
  const unsigned long saved_alloc = bb->alloc;
  EXPECT_EQ(1, ncbytessetlength(bb.get(), 3));
  EXPECT_EQ(3UL, bb->length);
  EXPECT_EQ(saved_alloc, bb->alloc);

  // Growing within existing capacity does not reallocate.
  EXPECT_EQ(1, ncbytessetlength(bb.get(), 6));
  EXPECT_EQ(6UL, bb->length);
  EXPECT_EQ(saved_alloc, bb->alloc);

  // Growing beyond existing capacity reallocates.
  EXPECT_EQ(1, ncbytessetlength(bb.get(), 32));
  EXPECT_EQ(32UL, bb->length);
  EXPECT_GE(bb->alloc, 32UL);

  ncbytesclear(bb.get());
  EXPECT_EQ(0UL, bb->length);
}

TEST(NcbytesTest, GetAndSetRespectBoundsAndNullBuffer) {
  EXPECT_EQ(-1, ncbytesget(nullptr, 0));

  ScopedNcBytes bb(ncbytesnew());
  ASSERT_NE(nullptr, bb.get());
  EXPECT_EQ(-1, ncbytesget(bb.get(), 0));

  ASSERT_EQ(1, ncbytessetlength(bb.get(), 4));
  ASSERT_EQ(1, ncbytesfill(bb.get(), 'a'));
  EXPECT_EQ(1, ncbytesset(bb.get(), 0, 'X'));
  EXPECT_EQ(1, ncbytesset(bb.get(), 3, 'Y'));

  // 7-bit ASCII values avoid signed/unsigned char differences across x86/ARM.
  EXPECT_EQ('X', ncbytesget(bb.get(), 0));
  EXPECT_EQ('a', ncbytesget(bb.get(), 1));
  EXPECT_EQ('Y', ncbytesget(bb.get(), 3));
  EXPECT_EQ(-1, ncbytesget(bb.get(), 4));
  EXPECT_EQ(-1, ncbytesget(bb.get(), 999));
}

TEST(NcbytesTest, AppendAppendnCatAndNullTerminateWorkTogether) {
  ScopedNcBytes bb(ncbytesnew());
  ASSERT_NE(nullptr, bb.get());

  EXPECT_EQ(1, ncbytesappend(bb.get(), 'A'));
  EXPECT_EQ(1UL, bb->length);

  EXPECT_EQ(1, ncbytesappendn(bb.get(), "BCDE", 4));
  EXPECT_EQ(5UL, bb->length);

  // n == 0 uses strlen() on the input C-string.
  EXPECT_EQ(1, ncbytesappendn(bb.get(), "FG", 0));
  EXPECT_EQ(7UL, bb->length);

  // ncbytescat(bb, nullptr) is a safe no-op returning 1.
  EXPECT_EQ(1, ncbytescat(bb.get(), nullptr));
  EXPECT_EQ(7UL, bb->length);

  EXPECT_EQ(1, ncbytescat(bb.get(), ""));
  EXPECT_EQ(7UL, bb->length);

  EXPECT_EQ(1, ncbytescat(bb.get(), "HIJ"));
  EXPECT_EQ(10UL, bb->length);
  EXPECT_EQ("ABCDEFGHIJ", std::string_view(bb->content, bb->length));

  // Append a raw byte and explicitly NUL-terminate without incrementing length.
  EXPECT_EQ(1, ncbytesappend(bb.get(), 'K'));
  EXPECT_EQ(11UL, bb->length);
  EXPECT_EQ(1, ncbytesnull(bb.get()));
  EXPECT_EQ(11UL, bb->length);
  EXPECT_STREQ("ABCDEFGHIJK", bb->content);
}

TEST(NcbytesTest, DupAndExtractReturnOwnedCopies) {
  ScopedNcBytes bb(ncbytesnew());
  ASSERT_NE(nullptr, bb.get());

  ASSERT_EQ(1, ncbytesappendn(bb.get(), "xy", 2));
  ScopedCString dup = MakeScopedCString(ncbytesdup(bb.get()));
  ASSERT_NE(nullptr, dup.get());
  EXPECT_STREQ("xy", dup.get());
  EXPECT_EQ(2UL, bb->length);

  ASSERT_EQ(1, ncbytescat(bb.get(), "-tail"));
  ScopedCString extracted = MakeScopedCString(ncbytesextract(bb.get()));
  ASSERT_NE(nullptr, extracted.get());
  EXPECT_STREQ("xy-tail", extracted.get());
  EXPECT_EQ(0UL, bb->alloc);
  EXPECT_EQ(0UL, bb->length);
  EXPECT_EQ(nullptr, bb->content);
}

TEST(NcbytesTest, SetContentsAndPrependOperateSafelyOnCallerBuffer) {
  // Zero-initialized backing arrays outlive `bb` and exceed `alloc` to avoid
  // the upstream +1 shift index in ncbytesprepend() when length < alloc.
  std::array<char, 16> backing{};
  std::memcpy(backing.data(), "bcd", 3);
  std::array<char, 8> second_backing{};

  ScopedNcBytes bb(ncbytesnew());
  ASSERT_NE(nullptr, bb.get());

  // Seed heap content first so ncbytessetcontents exercises freeing prior buf.
  ASSERT_EQ(1, ncbytescat(bb.get(), "heap_buffer"));
  ASSERT_NE(nullptr, bb->content);

  EXPECT_EQ(1, ncbytessetcontents(bb.get(), backing.data(), 8, 3));
  EXPECT_EQ(0, bb->extendible);
  EXPECT_EQ(8UL, bb->alloc);
  EXPECT_EQ(3UL, bb->length);
  EXPECT_EQ(backing.data(), bb->content);

  // Prepend into caller-owned non-extendible buffer with spare alloc.
  EXPECT_EQ(1, ncbytesprepend(bb.get(), 'a'));
  EXPECT_EQ(4UL, bb->length);
  EXPECT_EQ("abcd", std::string_view(bb->content, bb->length));

  // Prepend on length == 0 skips loop interior after shifting zeroed slots.
  ncbytesclear(bb.get());
  EXPECT_EQ(1, ncbytesprepend(bb.get(), '!'));
  EXPECT_EQ(1UL, bb->length);
  EXPECT_EQ('!', bb->content[0]);

  // Calling ncbytessetcontents on an already non-extendible buffer does not
  // free the previous caller buffer.
  EXPECT_EQ(1, ncbytessetcontents(bb.get(), second_backing.data(), 4, 0));
  EXPECT_EQ(second_backing.data(), bb->content);
  EXPECT_EQ(0, bb->extendible);
}

TEST(NcbytesTest, RemoveDeletesFromFrontMiddleAndTail) {
  ScopedNcBytes bb(ncbytesnew());
  ASSERT_NE(nullptr, bb.get());

  ASSERT_EQ(1, ncbytescat(bb.get(), "ABCDE"));
  EXPECT_EQ(5UL, bb->length);

  // Remove index 0 (memmove shifts 4 bytes).
  EXPECT_EQ(1, ncbytesremove(bb.get(), 0));
  EXPECT_EQ(4UL, bb->length);
  EXPECT_EQ("BCDE", std::string_view(bb->content, bb->length));

  // Remove middle index 1 (memmove shifts 2 bytes).
  EXPECT_EQ(1, ncbytesremove(bb.get(), 1));
  EXPECT_EQ(3UL, bb->length);
  EXPECT_EQ("BDE", std::string_view(bb->content, bb->length));

  // Remove tail index 2 (skips memmove).
  EXPECT_EQ(1, ncbytesremove(bb.get(), 2));
  EXPECT_EQ(2UL, bb->length);
  EXPECT_EQ("BD", std::string_view(bb->content, bb->length));

  // Remove remaining elements down to empty.
  EXPECT_EQ(1, ncbytesremove(bb.get(), 1));
  EXPECT_EQ(1, ncbytesremove(bb.get(), 0));
  EXPECT_EQ(0UL, bb->length);
}

}  // namespace
}  // namespace netcdf
