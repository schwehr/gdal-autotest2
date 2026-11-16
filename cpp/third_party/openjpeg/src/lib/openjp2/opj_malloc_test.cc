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
//
// Tests internal OpenJPEG memory allocation and alignment helpers
// (src/lib/openjp2/opj_malloc.c).
//
// Note for google3: OPJ_HAVE_POSIX_MEMALIGN is enabled in opj_config_private.h,
// so the memalign, Windows _aligned_malloc, and generic offset-pointer fallback
// branches are compiled out.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/opj_malloc.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/opj_malloc.h

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/openjpeg2/google/util/opj_internal.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::Each;
using ::testing::ElementsAreArray;
using ::testing::Eq;

constexpr auto kAllocSizes =
    std::to_array<size_t>({1, 7, 15, 16, 17, 31, 32, 33, 64, 255, 4096});

struct OpjFreeDeleter {
  void operator()(void* ptr) const { opj_free(ptr); }
};
using OpjPtr = std::unique_ptr<void, OpjFreeDeleter>;

using OpjAlignedVoidPtr = std::unique_ptr<void, OpjAlignedFreeDeleter>;

bool IsAligned(const void* ptr, size_t alignment) {
  return (reinterpret_cast<std::uintptr_t>(ptr) & (alignment - 1u)) == 0u;
}

std::span<uint8_t> ByteSpan(void* ptr, size_t size) {
  return {static_cast<uint8_t*>(ptr), size};
}

std::span<const uint8_t> ConstByteSpan(const void* ptr, size_t size) {
  return {static_cast<const uint8_t*>(ptr), size};
}

std::vector<uint8_t> MakePattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> pattern(size);
  for (size_t i = 0; i < size; ++i) {
    pattern[i] = static_cast<uint8_t>((i * 37u + seed + 1u) & 0xFFu);
  }
  return pattern;
}

void FillPattern(void* ptr, size_t size, uint8_t seed) {
  const std::vector<uint8_t> pattern = MakePattern(size, seed);
  std::copy(pattern.begin(), pattern.end(), ByteSpan(ptr, size).begin());
}

void VerifyWritablePattern(void* ptr, size_t size, uint8_t seed) {
  FillPattern(ptr, size, seed);
  EXPECT_THAT(ConstByteSpan(ptr, size),
              ElementsAreArray(MakePattern(size, seed)));
}

template <typename PtrT, typename ReallocFn>
bool ReallocInPlace(PtrT& ptr, size_t new_size, ReallocFn realloc_fn) {
  void* next = realloc_fn(ptr.get(), new_size);
  if (next == nullptr) {
    return false;
  }
  ptr.release();
  ptr.reset(next);
  return true;
}

template <typename AllocFn>
void VerifyAlignedMalloc(AllocFn alloc_fn, size_t alignment) {
  OpjAlignedVoidPtr zero_ptr(alloc_fn(0));
  EXPECT_EQ(nullptr, zero_ptr.get());

  // Hold all allocations alive concurrently to inspect distinct heap slots.
  std::vector<OpjAlignedVoidPtr> live;
  live.reserve(kAllocSizes.size());
  for (const size_t size : kAllocSizes) {
    SCOPED_TRACE(::testing::Message() << "size=" << size);
    OpjAlignedVoidPtr ptr(alloc_fn(size));
    ASSERT_NE(nullptr, ptr.get());
    EXPECT_TRUE(IsAligned(ptr.get(), alignment));
    VerifyWritablePattern(ptr.get(), size, static_cast<uint8_t>(size & 0xFFu));
    live.push_back(std::move(ptr));
  }
}

template <typename ReallocFn>
void VerifyAlignedReallocFromNullptr(ReallocFn realloc_fn, size_t alignment,
                                     std::span<const size_t> sizes,
                                     size_t copies) {
  std::vector<OpjAlignedVoidPtr> live;
  live.reserve(sizes.size() * copies);
  for (size_t copy = 0; copy < copies; ++copy) {
    for (const size_t size : sizes) {
      SCOPED_TRACE(::testing::Message()
                   << "copy=" << copy << ", size=" << size);
      OpjAlignedVoidPtr ptr(realloc_fn(nullptr, size));
      ASSERT_NE(nullptr, ptr.get());
      EXPECT_TRUE(IsAligned(ptr.get(), alignment));
      const uint8_t seed = static_cast<uint8_t>((size + copy) & 0xFFu);
      VerifyWritablePattern(ptr.get(), size, seed);
      live.push_back(std::move(ptr));
    }
  }
}

template <typename ReallocFn>
void VerifyAlignedResizeSequence(OpjAlignedVoidPtr ptr, size_t initial_size,
                                 size_t alignment,
                                 std::span<const size_t> steps,
                                 ReallocFn realloc_fn) {
  size_t current_size = initial_size;
  uint8_t seed = 5;
  ASSERT_NE(nullptr, ptr.get());
  ASSERT_TRUE(IsAligned(ptr.get(), alignment));
  FillPattern(ptr.get(), current_size, seed);

  for (const size_t next_size : steps) {
    SCOPED_TRACE(::testing::Message() << "next_size=" << next_size);
    ASSERT_TRUE(ReallocInPlace(ptr, next_size, realloc_fn));
    EXPECT_TRUE(IsAligned(ptr.get(), alignment));
    const size_t preserved = std::min(current_size, next_size);
    EXPECT_THAT(ConstByteSpan(ptr.get(), preserved),
                ElementsAreArray(MakePattern(preserved, seed)));
    seed = static_cast<uint8_t>(seed + 11u);
    FillPattern(ptr.get(), next_size, seed);
    current_size = next_size;
  }
}

TEST(OpjMallocTest, MallocZeroReturnsNullAndPositiveSizesAreWritable) {
  OpjPtr zero_ptr(opj_malloc(0));
  EXPECT_EQ(nullptr, zero_ptr.get());

  std::vector<OpjPtr> live;
  live.reserve(kAllocSizes.size());
  for (const size_t size : kAllocSizes) {
    SCOPED_TRACE(::testing::Message() << "size=" << size);
    OpjPtr ptr(opj_malloc(size));
    ASSERT_NE(nullptr, ptr.get());
    VerifyWritablePattern(ptr.get(), size, static_cast<uint8_t>(size & 0xFFu));
    live.push_back(std::move(ptr));
  }
}

TEST(OpjMallocTest, FreeAcceptsNullAndAllocatedPointers) {
  opj_free(nullptr);

  std::array<OpjPtr, 3> ptrs = {
      OpjPtr(opj_malloc(64)),
      OpjPtr(opj_calloc(8, 8)),
      OpjPtr(opj_realloc(nullptr, 48)),
  };
  for (OpjPtr& p : ptrs) {
    ASSERT_NE(nullptr, p.get());
    FillPattern(p.get(), 48, 1);
    opj_free(p.release());
  }
}

TEST(OpjMallocTest, CallocZeroArgumentsReturnNullAndValidAreZeroed) {
  struct CallocCase {
    size_t num;
    size_t size;
  };
  constexpr auto kZeroCases = std::to_array<CallocCase>({
      {.num = 0, .size = 0},
      {.num = 0, .size = 16},
      {.num = 16, .size = 0},
  });
  for (const CallocCase& tc : kZeroCases) {
    SCOPED_TRACE(::testing::Message()
                 << "num=" << tc.num << ", size=" << tc.size);
    OpjPtr ptr(opj_calloc(tc.num, tc.size));
    EXPECT_EQ(nullptr, ptr.get());
  }

  constexpr auto kValidCases = std::to_array<CallocCase>({
      {.num = 1, .size = 1},
      {.num = 1, .size = 37},
      {.num = 16, .size = 4},
      {.num = 64, .size = 16},
      {.num = 257, .size = 3},
  });
  for (const CallocCase& tc : kValidCases) {
    SCOPED_TRACE(::testing::Message()
                 << "num=" << tc.num << ", size=" << tc.size);
    const size_t total = tc.num * tc.size;
    OpjPtr ptr(opj_calloc(tc.num, tc.size));
    ASSERT_NE(nullptr, ptr.get());
    EXPECT_THAT(ConstByteSpan(ptr.get(), total), Each(Eq(uint8_t{0})));
    VerifyWritablePattern(ptr.get(), total, 7);
  }
}

TEST(OpjMallocTest, ReallocNullptrHandlesZeroAndPositiveSizes) {
  // Note: opj_realloc(non_null, 0) returns nullptr without freeing ptr, so
  // zero-size behavior is tested with nullptr to stay leak- and upgrade-safe.
  OpjPtr zero_ptr(opj_realloc(nullptr, 0));
  EXPECT_EQ(nullptr, zero_ptr.get());

  for (const size_t size : kAllocSizes) {
    SCOPED_TRACE(::testing::Message() << "size=" << size);
    OpjPtr ptr(opj_realloc(nullptr, size));
    ASSERT_NE(nullptr, ptr.get());
    VerifyWritablePattern(ptr.get(), size, static_cast<uint8_t>(size & 0xFFu));
  }
}

TEST(OpjMallocTest, ReallocGrowsAndShrinksPreservingPrefix) {
  size_t current_size = 17;
  uint8_t seed = 3;
  OpjPtr ptr(opj_malloc(current_size));
  ASSERT_NE(nullptr, ptr.get());
  FillPattern(ptr.get(), current_size, seed);

  constexpr auto kResizeSteps = std::to_array<size_t>({64, 4096, 128, 5});
  for (const size_t next_size : kResizeSteps) {
    SCOPED_TRACE(::testing::Message() << "next_size=" << next_size);
    ASSERT_TRUE(ReallocInPlace(ptr, next_size, &opj_realloc));
    const size_t preserved = std::min(current_size, next_size);
    EXPECT_THAT(ConstByteSpan(ptr.get(), preserved),
                ElementsAreArray(MakePattern(preserved, seed)));
    seed = static_cast<uint8_t>(seed + 9u);
    FillPattern(ptr.get(), next_size, seed);
    current_size = next_size;
  }
}

TEST(OpjMallocTest, AlignedMallocZeroReturnsNullAndSizesAre16ByteAligned) {
  VerifyAlignedMalloc(&opj_aligned_malloc, 16);
}

TEST(OpjMallocTest, AlignedFreeAcceptsNullAndAlignedPointers) {
  opj_aligned_free(nullptr);

  std::array<OpjAlignedVoidPtr, 4> ptrs = {
      OpjAlignedVoidPtr(opj_aligned_malloc(64)),
      OpjAlignedVoidPtr(opj_aligned_realloc(nullptr, 48)),
      OpjAlignedVoidPtr(opj_aligned_32_malloc(64)),
      OpjAlignedVoidPtr(opj_aligned_32_realloc(nullptr, 48)),
  };
  for (OpjAlignedVoidPtr& p : ptrs) {
    ASSERT_NE(nullptr, p.get());
    FillPattern(p.get(), 48, 2);
    opj_aligned_free(p.release());
  }
}

// Wait: best-effort depending on underlying allocator size-class alignment
// (tcmalloc vs ASan/MSan/TSan interceptors).
TEST(OpjMallocTest, AlignedReallocHandlesNullptrZeroGrowthAndShrinkage) {
  OpjAlignedVoidPtr zero_ptr(opj_aligned_realloc(nullptr, 0));
  EXPECT_EQ(nullptr, zero_ptr.get());

  // Hold small realloc(nullptr, size) allocations concurrently so
  // 8-byte-aligned slots can also exercise the 16-byte alignment fallback path.
  constexpr auto kNullptrSizes =
      std::to_array<size_t>({1, 3, 7, 8, 16, 24, 48, 256});
  VerifyAlignedReallocFromNullptr(&opj_aligned_realloc, 16, kNullptrSizes, 4);

  constexpr auto kResizeSteps =
      std::to_array<size_t>({40, 256, 4096, 128, 7, 1});
  VerifyAlignedResizeSequence(OpjAlignedVoidPtr(opj_aligned_malloc(13)), 13, 16,
                              kResizeSteps, &opj_aligned_realloc);
}

TEST(OpjMallocTest, Aligned32MallocZeroReturnsNullAndSizesAre32ByteAligned) {
  VerifyAlignedMalloc(&opj_aligned_32_malloc, 32);
}

TEST(OpjMallocTest, Aligned32ReallocHandlesNullptrZeroAndPowerOfTwoResize) {
  OpjAlignedVoidPtr zero_ptr(opj_aligned_32_realloc(nullptr, 0));
  EXPECT_EQ(nullptr, zero_ptr.get());

  constexpr auto kPowerOfTwoSteps =
      std::to_array<size_t>({64, 256, 4096, 128, 32});
  VerifyAlignedResizeSequence(OpjAlignedVoidPtr(opj_aligned_32_malloc(32)), 32,
                              32, kPowerOfTwoSteps, &opj_aligned_32_realloc);
}

// Realloc alignment fallback is best-effort across underlying allocators.
TEST(OpjMallocTest, Aligned32ReallocPreservesDataAcrossUnalignedSizes) {
  // Hold multiple live allocations across tiny, odd, and 16-byte-multiple
  // (non-32-byte-multiple) size classes so realloc() encounters both 32-byte
  // aligned fast-path slots and 16-byte-aligned fallback copy slots.
  constexpr size_t kBatchCount = 16;
  size_t current_size = 16;
  std::vector<OpjAlignedVoidPtr> batch;
  batch.reserve(kBatchCount);
  for (size_t i = 0; i < kBatchCount; ++i) {
    OpjAlignedVoidPtr ptr(opj_aligned_32_malloc(current_size));
    ASSERT_NE(nullptr, ptr.get());
    ASSERT_TRUE(IsAligned(ptr.get(), 32));
    FillPattern(ptr.get(), current_size, static_cast<uint8_t>(i));
    batch.push_back(std::move(ptr));
  }

  constexpr auto kOddAnd16ByteSizes =
      std::to_array<size_t>({48, 80, 112, 24, 16, 7, 1, 48});
  for (const size_t next_size : kOddAnd16ByteSizes) {
    const size_t preserved = std::min(current_size, next_size);
    for (size_t i = 0; i < kBatchCount; ++i) {
      SCOPED_TRACE(::testing::Message()
                   << "next_size=" << next_size << ", slot=" << i);
      ASSERT_TRUE(ReallocInPlace(batch[i], next_size, &opj_aligned_32_realloc));
      EXPECT_TRUE(IsAligned(batch[i].get(), 32));
      const uint8_t seed = static_cast<uint8_t>(i);
      EXPECT_THAT(ConstByteSpan(batch[i].get(), preserved),
                  ElementsAreArray(MakePattern(preserved, seed)));
      // Initialize all next_size bytes for MSan before the next resize step.
      FillPattern(batch[i].get(), next_size, seed);
    }
    current_size = next_size;
  }

  constexpr auto kNullptrOddSizes =
      std::to_array<size_t>({1, 7, 8, 16, 24, 48, 80});
  VerifyAlignedReallocFromNullptr(&opj_aligned_32_realloc, 32, kNullptrOddSizes,
                                  4);
}

// TODO(schwehr): Cover OOM allocation failure branches in opj_malloc.c
// (posix_memalign() != 0 and opj_aligned_alloc_n() == nullptr) when running
// with allocator_may_return_null=1 or dedicated fault injection.

}  // namespace
}  // namespace openjpeg2
