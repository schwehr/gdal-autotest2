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

// Unit tests for //third_party/netcdf/ncgen/bytebuffer.c.
//
// Exercises allocation growth, fixed-buffer guards, element access, string
// concatenation, insertions, head/tail queue operations, duplication, and
// buffer extraction.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/bytebuffer.c

#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"

extern "C" {
extern int bbdebug;
int bbHeadpop(Bytebuffer* bb, char* pelem);
int bbTailpop(Bytebuffer* bb, char* pelem);
int bbHeadpeek(Bytebuffer* bb, char* pelem);
int bbTailpeek(Bytebuffer* bb, char* pelem);
}  // extern "C"

namespace netcdf {
namespace {

struct BbDeleter {
  void operator()(Bytebuffer* bb) const { bbFree(bb); }
};
using ScopedBb = std::unique_ptr<Bytebuffer, BbDeleter>;

class ScopedBbDebug {
 public:
  explicit ScopedBbDebug(int value) : prev_(bbdebug) { bbdebug = value; }
  ~ScopedBbDebug() { bbdebug = prev_; }

  ScopedBbDebug(const ScopedBbDebug&) = delete;
  ScopedBbDebug& operator=(const ScopedBbDebug&) = delete;

 private:
  int prev_;
};

void CleanupNcgenGlobals() {
  if (filldatalist != nullptr) {
    filldatalist->readonly = 0;
    freedatalist(filldatalist);
    filldatalist = nullptr;
  }
  if (codebuffer != nullptr) {
    bbFree(codebuffer);
    codebuffer = nullptr;
  }
  if (stmt != nullptr) {
    bbFree(stmt);
    stmt = nullptr;
  }
}

class BytebufferTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CleanupNcgenGlobals();
    ncgen_shim_reset_globals();
  }

  void TearDown() override { CleanupNcgenGlobals(); }
};

TEST_F(BytebufferTest, NewFreeAndMacrosHandleNullEmptyAndPopulated) {
  bbFree(nullptr);

  Bytebuffer* null_bb = nullptr;
  EXPECT_EQ(0U, bbLength(null_bb));
  EXPECT_EQ(0U, bbAlloc(null_bb));
  EXPECT_STREQ("", bbContents(null_bb));
  EXPECT_EQ(0U, bbNeed(null_bb, 1));
  EXPECT_EQ(0U, bbAvail(null_bb));
  bbClear(null_bb);

  ScopedBb empty(bbNew());
  ASSERT_NE(empty.get(), nullptr);
  EXPECT_EQ(0U, bbLength(empty.get()));
  EXPECT_EQ(0U, bbAlloc(empty.get()));
  EXPECT_EQ(0, empty->nonextendible);
  EXPECT_STREQ("", bbContents(empty.get()));
  EXPECT_EQ(0U, bbAvail(empty.get()));
  // Freeing a buffer whose content was never allocated skips efree(content).
  empty.reset();

  ScopedBb populated(bbNew());
  ASSERT_NE(populated.get(), nullptr);
  EXPECT_EQ(1, bbAppend(populated.get(), 'a'));
  EXPECT_EQ(1U, bbLength(populated.get()));
  EXPECT_GT(bbAlloc(populated.get()), 1U);
  EXPECT_NE(0U, bbNeed(populated.get(), 1));
  EXPECT_EQ(bbAlloc(populated.get()) - 1U, bbAvail(populated.get()));
  bbClear(populated.get());
  EXPECT_EQ(0U, bbLength(populated.get()));
}

TEST_F(BytebufferTest, SetallocAndSetlengthGrowShrinkAndHonorNonextendible) {
  ScopedBbDebug debug_guard(0);

  EXPECT_EQ(0, bbSetalloc(nullptr, 16));
  EXPECT_EQ(0, bbSetlength(nullptr, 16));

  ScopedBb bb(bbNew());
  ASSERT_NE(bb.get(), nullptr);

  // Initial zero-size bbSetalloc allocates DEFAULTALLOC (1024).
  EXPECT_EQ(1, bbSetalloc(bb.get(), 0));
  EXPECT_EQ(1024U, bbAlloc(bb.get()));
  // Requesting <= existing alloc is a no-op returning TRUE.
  EXPECT_EQ(1, bbSetalloc(bb.get(), 512));
  EXPECT_EQ(1024U, bbAlloc(bb.get()));

  EXPECT_EQ(1, bbAppend(bb.get(), 'x'));
  EXPECT_EQ(1, bbAppend(bb.get(), 'y'));
  // Zero-size bbSetalloc on non-empty buffer doubles alloc and preserves data.
  EXPECT_EQ(1, bbSetalloc(bb.get(), 0));
  EXPECT_EQ(2048U, bbAlloc(bb.get()));
  EXPECT_EQ('x', bbGet(bb.get(), 0));
  EXPECT_EQ('y', bbGet(bb.get(), 1));

  // bbSetlength grows when needed and shrinks without reallocating.
  EXPECT_EQ(1, bbSetlength(bb.get(), 5));
  EXPECT_EQ(5U, bbLength(bb.get()));
  EXPECT_EQ(1, bbSetlength(bb.get(), 2));
  EXPECT_EQ(2U, bbLength(bb.get()));
  EXPECT_EQ(1, bbSetlength(bb.get(), 4096));
  EXPECT_EQ(4096U, bbLength(bb.get()));
  EXPECT_EQ(4096U, bbAlloc(bb.get()));

  // Replacing content on an extendible buffer frees its prior allocation.
  std::array<char, 4> fixed_a{};
  std::array<char, 8> fixed_b{};
  EXPECT_EQ(0, bbSetcontents(nullptr, fixed_a.data(),
                             static_cast<unsigned int>(fixed_a.size())));
  EXPECT_EQ(1, bbSetcontents(bb.get(), fixed_a.data(),
                             static_cast<unsigned int>(fixed_a.size())));
  EXPECT_EQ(1, bb->nonextendible);
  EXPECT_EQ(4U, bbAlloc(bb.get()));
  EXPECT_EQ(0U, bbLength(bb.get()));

  // Nonextendible buffers reject explicit larger allocations and inserts.
  EXPECT_EQ(0, bbSetalloc(bb.get(), 16));
  EXPECT_EQ(0, bbSetlength(bb.get(), 16));
  EXPECT_EQ(0, bbInsertn(bb.get(), 0, "abcde", 5));

  // Calling bbSetcontents again while nonextendible does not efree fixed_a.
  EXPECT_EQ(1, bbSetcontents(bb.get(), fixed_b.data(),
                             static_cast<unsigned int>(fixed_b.size())));
  EXPECT_EQ(8U, bbAlloc(bb.get()));

  // Extract external pointer before bbFree so caller-owned storage is not
  // freed.
  EXPECT_EQ(fixed_b.data(), bbExtract(bb.get()));
}

TEST_F(BytebufferTest, FillGetSetAndNullHandleValidAndInvalidIndices) {
  ScopedBbDebug debug_guard(0);

  EXPECT_EQ(0, bbFill(nullptr, 'a'));
  EXPECT_EQ(-1, bbGet(nullptr, 0));
  EXPECT_EQ(0, bbSet(nullptr, 0, 'a'));

  ScopedBb bb(bbNew());
  ASSERT_NE(bb.get(), nullptr);

  // Operations on zero-length buffer.
  EXPECT_EQ(1, bbFill(bb.get(), 'z'));
  EXPECT_EQ(-1, bbGet(bb.get(), 0));
  EXPECT_EQ(0, bbSet(bb.get(), 0, 'z'));

  ASSERT_EQ(1, bbSetlength(bb.get(), 4));
  EXPECT_EQ(1, bbFill(bb.get(), 'k'));
  EXPECT_EQ('k', bbGet(bb.get(), 0));
  EXPECT_EQ('k', bbGet(bb.get(), 3));
  EXPECT_EQ(-1, bbGet(bb.get(), 4));

  EXPECT_EQ(1, bbSet(bb.get(), 2, 'm'));
  EXPECT_EQ('m', bbGet(bb.get(), 2));
  EXPECT_EQ(1, bbNull(bb.get()));
  EXPECT_EQ(4U, bbLength(bb.get()));
  EXPECT_STREQ("kkmk", bbContents(bb.get()));
}

TEST_F(BytebufferTest, AppendAppendnCatAndCatbufHandleGrowthAndEmbeddedNuls) {
  ScopedBbDebug debug_guard(0);

  ScopedBb bb(bbNew());
  ASSERT_NE(bb.get(), nullptr);

  EXPECT_EQ(0, bbAppend(nullptr, 'a'));
  EXPECT_EQ(0, bbAppendn(nullptr, "a", 1));
  EXPECT_EQ(0, bbAppendn(bb.get(), nullptr, 1));

  // Append past DEFAULTALLOC via bbAppend to exercise growth inside bbAppend.
  for (unsigned int i = 0; i < 1030; ++i) {
    ASSERT_EQ(1, bbAppend(bb.get(), static_cast<char>('0' + (i % 10))));
  }
  EXPECT_EQ(1030U, bbLength(bb.get()));
  EXPECT_EQ(2048U, bbAlloc(bb.get()));
  EXPECT_EQ('0', bbGet(bb.get(), 0));
  EXPECT_EQ('9', bbGet(bb.get(), 1029));

  // Clear and test bbAppendn with n==0 (strlen) and n>0 with embedded NUL.
  bbClear(bb.get());
  EXPECT_EQ(1, bbAppendn(bb.get(), "hello", 0));
  EXPECT_EQ(5U, bbLength(bb.get()));
  const char kBinarySuffix[] = {'\0', 'w', 'o', 'r', 'l', 'd'};
  EXPECT_EQ(1, bbAppendn(bb.get(), kBinarySuffix, sizeof(kBinarySuffix)));
  EXPECT_EQ(11U, bbLength(bb.get()));
  EXPECT_EQ(std::string_view("hello\0world", 11),
            std::string_view(bbContents(bb.get()), bbLength(bb.get())));

  // Trigger growth inside bbAppendn past current 2048-byte allocation.
  const std::string large_chunk(2050, 'q');
  EXPECT_EQ(1, bbAppendn(bb.get(), large_chunk.data(), large_chunk.size()));
  EXPECT_EQ(11U + 2050U, bbLength(bb.get()));

  // Test bbCat (empty and non-empty) and bbCatbuf (null, empty, non-empty).
  bbClear(bb.get());
  EXPECT_EQ(1, bbCat(bb.get(), ""));
  EXPECT_EQ(0U, bbLength(bb.get()));
  EXPECT_EQ(1, bbCat(bb.get(), "alpha"));
  EXPECT_STREQ("alpha", bbContents(bb.get()));

  ScopedBb empty_src(bbNew());
  ASSERT_NE(empty_src.get(), nullptr);
  EXPECT_EQ(1, bbCatbuf(bb.get(), nullptr));
  EXPECT_EQ(1, bbCatbuf(bb.get(), empty_src.get()));
  EXPECT_STREQ("alpha", bbContents(bb.get()));

  ScopedBb suffix(bbNew());
  ASSERT_NE(suffix.get(), nullptr);
  EXPECT_EQ(1, bbCat(suffix.get(), "_beta"));
  EXPECT_EQ(1, bbCatbuf(bb.get(), suffix.get()));
  EXPECT_EQ(10U, bbLength(bb.get()));
  EXPECT_STREQ("alpha_beta", bbContents(bb.get()));
}

TEST_F(BytebufferTest, InsertPopPeekDupAndExtractManageContents) {
  ScopedBbDebug debug_guard(0);

  EXPECT_EQ(0, bbInsertn(nullptr, 0, "a", 1));

  ScopedBb bb(bbNew());
  ASSERT_NE(bb.get(), nullptr);

  // Inserting into unallocated buffer invokes bbExtend.
  EXPECT_EQ(1, bbInsert(bb.get(), 0, 'a'));
  EXPECT_EQ(1U, bbLength(bb.get()));
  // Append at end and insert before last character.
  EXPECT_EQ(1, bbInsert(bb.get(), 1, 'b'));
  EXPECT_EQ(1, bbInsert(bb.get(), 1, 'X'));
  EXPECT_EQ(1, bbNull(bb.get()));
  EXPECT_STREQ("aXb", bbContents(bb.get()));

  // Peek and pop error cases.
  char ch = '\0';
  EXPECT_EQ(0, bbHeadpop(nullptr, &ch));
  EXPECT_EQ(0, bbTailpop(nullptr, &ch));
  EXPECT_EQ(0, bbHeadpeek(nullptr, &ch));
  EXPECT_EQ(0, bbTailpeek(nullptr, &ch));

  ScopedBb empty(bbNew());
  ASSERT_NE(empty.get(), nullptr);
  EXPECT_EQ(0, bbHeadpop(empty.get(), &ch));
  EXPECT_EQ(0, bbTailpop(empty.get(), &ch));
  EXPECT_EQ(0, bbHeadpeek(empty.get(), &ch));
  EXPECT_EQ(0, bbTailpeek(empty.get(), &ch));

  // Valid peek/pop sequence on "aXb".
  EXPECT_EQ(1, bbHeadpeek(bb.get(), &ch));
  EXPECT_EQ('a', ch);
  EXPECT_EQ(1, bbTailpeek(bb.get(), &ch));
  EXPECT_EQ('b', ch);
  EXPECT_EQ(3U, bbLength(bb.get()));

  EXPECT_EQ(1, bbHeadpop(bb.get(), &ch));
  EXPECT_EQ('a', ch);
  EXPECT_EQ(2U, bbLength(bb.get()));

  EXPECT_EQ(1, bbTailpop(bb.get(), &ch));
  EXPECT_EQ('b', ch);
  EXPECT_EQ(1U, bbLength(bb.get()));

  EXPECT_EQ(1, bbHeadpop(bb.get(), &ch));
  EXPECT_EQ('X', ch);
  EXPECT_EQ(0U, bbLength(bb.get()));
  EXPECT_EQ(0, bbHeadpop(bb.get(), &ch));

  // bbDup returns a NUL-terminated copy owned by the caller.
  EXPECT_EQ(1, bbCat(bb.get(), "dup_me"));
  std::unique_ptr<char, decltype(&std::free)> dup(bbDup(bb.get()), &std::free);
  ASSERT_NE(dup.get(), nullptr);
  EXPECT_STREQ("dup_me", dup.get());

  // bbExtract transfers ownership and resets the Bytebuffer.
  EXPECT_EQ(nullptr, bbExtract(nullptr));
  EXPECT_EQ(nullptr, bbExtract(empty.get()));

  std::unique_ptr<char, decltype(&std::free)> extracted(bbExtract(bb.get()),
                                                        &std::free);
  ASSERT_NE(extracted.get(), nullptr);
  EXPECT_STREQ("dup_me", extracted.get());
  EXPECT_EQ(0U, bbLength(bb.get()));
  EXPECT_EQ(0U, bbAlloc(bb.get()));
  EXPECT_EQ(nullptr, bb->content);
}

}  // namespace
}  // namespace netcdf
