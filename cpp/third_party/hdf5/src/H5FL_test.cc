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
// Tests HDF5 internal free-list managers—regular, block priority-queue,
// array/base-array, sequence, and factory allocators, garbage collection,
// memory limits, and package termination—along with public free-list APIs
// (third_party/hdf5/src/H5FL.c).
//
// Google3 build notes:
//   - H5_NO_FREE_LISTS and H5_USING_MEMCHECKER are disabled, so all free-list
//     pools and garbage collectors in H5FL.c are active.
//   - :hdf5 is compiled with -DNDEBUG, which elides internal assert() checks.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FL.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, so wrap internal headers with NDEBUG=1
// to match `:hdf5` struct layouts, then restore NDEBUG for test code.
#ifndef NDEBUG
#define H5FL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#include "third_party/hdf5/src/H5FLprivate.h"
H5FL_EXTERN(H5FL_fac_head_t);
}  // extern "C"
#ifdef H5FL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::Each;

// Default free-list limits defined in H5FL.c.
constexpr int kDefaultRegGlbLim = 1 * 1024 * 1024;
constexpr int kDefaultRegLstLim = 64 * 1024;
constexpr int kDefaultArrGlbLim = 4 * 1024 * 1024;
constexpr int kDefaultArrLstLim = 4 * 64 * 1024;
constexpr int kDefaultBlkGlbLim = 16 * 1024 * 1024;
constexpr int kDefaultBlkLstLim = 1024 * 1024;
constexpr int kDefaultFacGlbLim = 16 * 1024 * 1024;
constexpr int kDefaultFacLstLim = 1024 * 1024;

void RestoreDefaultFreeListLimits() {
  H5FL_set_free_list_limits(kDefaultRegGlbLim, kDefaultRegLstLim,
                            kDefaultArrGlbLim, kDefaultArrLstLim,
                            kDefaultBlkGlbLim, kDefaultBlkLstLim,
                            kDefaultFacGlbLim, kDefaultFacLstLim);
}

struct TinyRegItem {
  uint8_t value;
};
static_assert(sizeof(TinyRegItem) < sizeof(H5FL_reg_node_t));

struct LargeRegItem {
  uint64_t a;
  uint64_t b;
  uint64_t c;
  uint64_t d;
};
static_assert(sizeof(LargeRegItem) == 32);

struct ArrElem {
  uint32_t id;
  uint32_t tag;
};
static_assert(sizeof(ArrElem) == 8);

struct BaseHeader {
  uint64_t magic;
  uint32_t count;
  uint32_t flags;
};
static_assert(sizeof(BaseHeader) == 16);

struct BaseArrElem {
  uint32_t v;
};

struct SeqElem {
  uint64_t key;
  uint64_t val;
};
static_assert(sizeof(SeqElem) == 16);

TEST(H5FLTest, PublicFreeListApisAndChunkedDatasetLifecycle) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_limits = [] { RestoreDefaultFreeListLimits(); };

  // 1. Query current sizes, including nullptr outputs.
  size_t reg_size = 0;
  size_t arr_size = 0;
  size_t blk_size = 0;
  size_t fac_size = 0;
  ASSERT_GE(H5get_free_list_sizes(&reg_size, &arr_size, &blk_size, &fac_size),
            0);
  EXPECT_GE(H5get_free_list_sizes(nullptr, nullptr, nullptr, nullptr), 0);

  // 2. Exercise public H5set_free_list_limits with -1 (no limit) and explicit
  //    bounds.
  EXPECT_GE(H5set_free_list_limits(-1, -1, -1, -1, -1, -1), 0);
  EXPECT_GE(H5set_free_list_limits(4096, 1024, 4096, 1024, 4096, 1024), 0);

  // 3. Drive HDF5 public operations that allocate from reg/arr/blk/fac pools
  //    (in-memory file, chunked dataset with hyperslab write, attribute).
  {
    ScopedFile file = CreateInMemoryFile("h5fl_public.h5");
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 2> kDims = {16, 16};
    constexpr std::array<hsize_t, 2> kChunks = {4, 4};
    ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunks.data()), 0);

    ScopedDataset dset(H5Dcreate2(file, "chunked", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    std::vector<int32_t> data(16 * 16, 42);
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       data.data()),
              0);

    ScopedSpace attr_space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(attr_space.valid());
    ScopedAttr attr(H5Acreate2(dset, "tag", H5T_NATIVE_INT32, attr_space,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    int32_t tag_val = 7;
    ASSERT_GE(H5Awrite(attr, H5T_NATIVE_INT32, &tag_val), 0);
  }

  ASSERT_GE(H5get_free_list_sizes(&reg_size, &arr_size, &blk_size, &fac_size),
            0);
  EXPECT_GE(H5garbage_collect(), 0);
}

TEST(H5FLTest, InternalSetFreeListLimitsSupportsSentinelsAndExplicitValues) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_limits = [] { RestoreDefaultFreeListLimits(); };

  EXPECT_GE(H5FL_set_free_list_limits(-1, -1, -1, -1, -1, -1, -1, -1), 0);
  EXPECT_GE(H5FL_set_free_list_limits(0, 0, 0, 0, 0, 0, 0, 0), 0);
  EXPECT_GE(
      H5FL_set_free_list_limits(2048, 512, 2048, 512, 2048, 512, 2048, 512), 0);
}

TEST(H5FLTest, RegMallocCallocFreeAndReuse) {
  ASSERT_GE(H5open(), 0);
  H5FL_DEFINE_STATIC(LargeRegItem);
  auto* head = &H5FL_REG_NAME(LargeRegItem);

  LargeRegItem* p1 = nullptr;
  LargeRegItem* p2 = nullptr;
  absl::Cleanup cleanup = [&] {
    if (p1 != nullptr) H5FL_reg_free(head, p1);
    if (p2 != nullptr) H5FL_reg_free(head, p2);
    H5FL_garbage_coll();
  };

  p1 = H5FL_MALLOC(LargeRegItem);
  ASSERT_NE(p1, nullptr);
  EXPECT_TRUE(head->init);
  EXPECT_EQ(head->size, sizeof(LargeRegItem));
  EXPECT_EQ(head->allocated, 1u);
  EXPECT_EQ(head->onlist, 0u);
  *p1 = {0x1111, 0x2222, 0x3333, 0x4444};

  p2 = H5FL_CALLOC(LargeRegItem);
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(head->allocated, 2u);
  EXPECT_EQ(p2->a, 0u);
  EXPECT_EQ(p2->b, 0u);
  EXPECT_EQ(p2->c, 0u);
  EXPECT_EQ(p2->d, 0u);
  *p2 = {0xaaaa, 0xbbbb, 0xcccc, 0xdddd};

  // Freeing returns nullptr and places node on free list for LIFO reuse.
  EXPECT_EQ(H5FL_FREE(LargeRegItem, p1), nullptr);
  p1 = nullptr;
  EXPECT_EQ(head->onlist, 1u);

  p1 = H5FL_CALLOC(LargeRegItem);
  ASSERT_NE(p1, nullptr);
  EXPECT_EQ(head->onlist, 0u);
  EXPECT_EQ(head->allocated, 2u);
  EXPECT_EQ(p1->a, 0u);
  EXPECT_EQ(p1->b, 0u);
  EXPECT_EQ(p1->c, 0u);
  EXPECT_EQ(p1->d, 0u);

  EXPECT_EQ(H5FL_FREE(LargeRegItem, p2), nullptr);
  p2 = nullptr;
  p2 = H5FL_MALLOC(LargeRegItem);
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(head->onlist, 0u);
  EXPECT_EQ(head->allocated, 2u);
}

TEST(H5FLTest, RegSmallTypeExpandsBlockSizeToFitListNode) {
  ASSERT_GE(H5open(), 0);
  H5FL_DEFINE_STATIC(TinyRegItem);
  auto* head = &H5FL_REG_NAME(TinyRegItem);

  TinyRegItem* p = nullptr;
  absl::Cleanup cleanup = [&] {
    if (p != nullptr) H5FL_reg_free(head, p);
    H5FL_garbage_coll();
  };

  p = static_cast<TinyRegItem*>(H5FL_reg_malloc(head));
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(head->init);
  EXPECT_GE(head->size, sizeof(H5FL_reg_node_t));
  p->value = 0x7f;

  EXPECT_EQ(H5FL_reg_free(head, p), nullptr);
  p = nullptr;
  EXPECT_EQ(head->onlist, 1u);

  p = static_cast<TinyRegItem*>(H5FL_reg_calloc(head));
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->value, 0u);
}

TEST(H5FLTest, RegListAndGlobalLimitsTriggerGarbageCollection) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_limits = [] { RestoreDefaultFreeListLimits(); };

  static H5FL_reg_head_t head_a = {false, 0, 0, "reg_lim_a", 32, nullptr};
  static H5FL_reg_head_t head_b = {false, 0, 0, "reg_lim_b", 32, nullptr};

  std::vector<void*> alloc_a;
  std::vector<void*> alloc_b;
  absl::Cleanup cleanup = [&] {
    for (void* p : alloc_a) H5FL_reg_free(&head_a, p);
    for (void* p : alloc_b) H5FL_reg_free(&head_b, p);
    H5FL_garbage_coll();
  };

  // 1. Exceed per-list limit (48 bytes) with large global limit: 2nd 32-byte
  //    free reaches 64 > 48 and triggers H5FL__reg_gc_list(head_a).
  ASSERT_GE(
      H5FL_set_free_list_limits(4096, 48, 4096, 4096, 4096, 4096, 4096, 4096),
      0);
  for (int i = 0; i < 2; ++i) {
    void* ptr = H5FL_reg_malloc(&head_a);
    ASSERT_NE(ptr, nullptr);
    alloc_a.push_back(ptr);
  }
  while (!alloc_a.empty()) {
    void* ptr = alloc_a.back();
    alloc_a.pop_back();
    H5FL_reg_free(&head_a, ptr);
  }
  EXPECT_EQ(head_a.onlist, 0u);
  EXPECT_EQ(head_a.allocated, 0u);

  // 2. Exceed global limit (48 bytes) across two lists while each list stays
  //    below per-list limit (1024 bytes): triggers H5FL__reg_gc().
  ASSERT_GE(
      H5FL_set_free_list_limits(48, 1024, 4096, 4096, 4096, 4096, 4096, 4096),
      0);
  void* pa = H5FL_reg_malloc(&head_a);
  void* pb = H5FL_reg_malloc(&head_b);
  ASSERT_NE(pa, nullptr);
  ASSERT_NE(pb, nullptr);
  alloc_a.push_back(pa);
  alloc_b.push_back(pb);

  H5FL_reg_free(&head_a, alloc_a.back());
  alloc_a.pop_back();
  EXPECT_EQ(head_a.onlist, 1u);

  H5FL_reg_free(&head_b, alloc_b.back());
  alloc_b.pop_back();
  EXPECT_EQ(head_a.onlist, 0u);
  EXPECT_EQ(head_b.onlist, 0u);
}

TEST(H5FLTest, BlkMallocCallocAvailAndPriorityQueueReordering) {
  ASSERT_GE(H5open(), 0);
  H5FL_BLK_DEFINE_STATIC(pq_test);
  auto* head = &H5FL_BLK_NAME(pq_test);

  std::vector<void*> live;
  absl::Cleanup cleanup = [&] {
    for (void* p : live) H5FL_blk_free(head, p);
    H5FL_garbage_coll();
  };

  // Uninitialized queue reports no free blocks available.
  EXPECT_EQ(0, H5FL_BLK_AVAIL(pq_test, 32));

  // Allocate sizes 32, 64, 128 so PQ order becomes [128 -> 64 -> 32].
  uint8_t* b32 = H5FL_BLK_MALLOC(pq_test, 32);
  ASSERT_NE(b32, nullptr);
  live.push_back(b32);
  uint8_t* b64_a = H5FL_BLK_MALLOC(pq_test, 64);
  ASSERT_NE(b64_a, nullptr);
  live.push_back(b64_a);
  uint8_t* b128 = H5FL_BLK_MALLOC(pq_test, 128);
  ASSERT_NE(b128, nullptr);
  live.push_back(b128);

  // Node exists for 64, but list is empty -> avail is still false; allocating a
  // second 64-byte block hits the existing-empty-node branch in
  // H5FL_blk_malloc.
  EXPECT_EQ(0, H5FL_blk_free_block_avail(head, 64));
  uint8_t* b64_b = H5FL_BLK_MALLOC(pq_test, 64);
  ASSERT_NE(b64_b, nullptr);
  live.push_back(b64_b);
  std::memset(b32, 0xaa, 32);
  std::memset(b64_a, 0xbb, 64);
  std::memset(b64_b, 0xcc, 64);
  std::memset(b128, 0xdd, 128);

  // Free all blocks so each PQ node holds a cached block.
  while (!live.empty()) {
    void* p = live.back();
    live.pop_back();
    EXPECT_EQ(H5FL_BLK_FREE(pq_test, p), nullptr);
  }
  EXPECT_EQ(head->onlist, 4u);
  EXPECT_EQ(head->list_mem, 32u + 64u + 64u + 128u);

  // Querying avail walks the PQ and promotes matching nodes to the head:
  //   - tail node (32), middle node (64), head node (128), missing size (256).
  EXPECT_GT(H5FL_blk_free_block_avail(head, 32), 0);
  EXPECT_EQ(head->head->size, 32u);
  EXPECT_GT(H5FL_blk_free_block_avail(head, 64), 0);
  EXPECT_EQ(head->head->size, 64u);
  EXPECT_GT(H5FL_blk_free_block_avail(head, 64), 0);
  EXPECT_GT(H5FL_blk_free_block_avail(head, 128), 0);
  EXPECT_EQ(head->head->size, 128u);
  EXPECT_EQ(0, H5FL_blk_free_block_avail(head, 256));

  // Reusing via H5FL_BLK_CALLOC zeroes out the previously filled block.
  uint8_t* zero64 = H5FL_BLK_CALLOC(pq_test, 64);
  ASSERT_NE(zero64, nullptr);
  live.push_back(zero64);
  std::vector<uint8_t> buf(zero64, zero64 + 64);
  EXPECT_THAT(buf, Each(0));
}

TEST(H5FLTest, BlkReallocNullSameGrowAndShrink) {
  ASSERT_GE(H5open(), 0);
  static H5FL_blk_head_t head = {false, 0, 0, 0, "blk_realloc", nullptr};

  void* blk = nullptr;
  absl::Cleanup cleanup = [&] {
    if (blk != nullptr) H5FL_blk_free(&head, blk);
    H5FL_garbage_coll();
  };

  // 1. realloc(nullptr, 16) allocates a fresh block.
  blk = H5FL_blk_realloc(&head, nullptr, 16);
  ASSERT_NE(blk, nullptr);
  auto* bytes = static_cast<uint8_t*>(blk);
  for (uint8_t i = 0; i < 16; ++i) {
    bytes[i] = static_cast<uint8_t>(i + 1);
  }

  // 2. realloc with identical size returns the exact same pointer.
  void* same = H5FL_blk_realloc(&head, blk, 16);
  EXPECT_EQ(same, blk);
  blk = same;

  // 3. Grow to 48 bytes and verify first 16 bytes preserved.
  blk = H5FL_blk_realloc(&head, blk, 48);
  ASSERT_NE(blk, nullptr);
  bytes = static_cast<uint8_t*>(blk);
  for (uint8_t i = 0; i < 16; ++i) {
    EXPECT_EQ(bytes[i], static_cast<uint8_t>(i + 1));
  }
  std::memset(bytes + 16, 0x55, 32);

  // 4. Shrink to 8 bytes and verify first 8 bytes preserved.
  blk = H5FL_blk_realloc(&head, blk, 8);
  ASSERT_NE(blk, nullptr);
  bytes = static_cast<uint8_t*>(blk);
  for (uint8_t i = 0; i < 8; ++i) {
    EXPECT_EQ(bytes[i], static_cast<uint8_t>(i + 1));
  }
}

TEST(H5FLTest, BlkGcListUnlinksHeadMiddleTailAndLimitsTriggerGc) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_limits = [] { RestoreDefaultFreeListLimits(); };

  static H5FL_blk_head_t head_a = {false, 0, 0, 0, "blk_pq_a", nullptr};
  static H5FL_blk_head_t head_b = {false, 0, 0, 0, "blk_pq_b", nullptr};

  std::vector<void*> live_a;
  std::vector<void*> live_b;
  absl::Cleanup cleanup = [&] {
    for (void* p : live_a) H5FL_blk_free(&head_a, p);
    for (void* p : live_b) H5FL_blk_free(&head_b, p);
    H5FL_garbage_coll();
  };

  // 1. Build PQ with 5 sizes: [80 -> 64 -> 48 -> 32 -> 16] (head=80, tail=16).
  void* p16 = H5FL_blk_malloc(&head_a, 16);
  void* p32 = H5FL_blk_malloc(&head_a, 32);
  void* p48 = H5FL_blk_malloc(&head_a, 48);
  void* p64 = H5FL_blk_malloc(&head_a, 64);
  void* p80 = H5FL_blk_malloc(&head_a, 80);
  ASSERT_NE(p16, nullptr);
  ASSERT_NE(p32, nullptr);
  ASSERT_NE(p48, nullptr);
  ASSERT_NE(p64, nullptr);
  ASSERT_NE(p80, nullptr);
  live_a = {p32, p64};

  // Free tail (16), middle (48), and head (80) while keeping 32 and 64 active.
  // H5FL__blk_gc_list removes head, middle, and tail PQ nodes while keeping
  // the two active nodes linked.
  H5FL_blk_free(&head_a, p16);
  H5FL_blk_free(&head_a, p48);
  H5FL_blk_free(&head_a, p80);
  ASSERT_GE(H5FL_garbage_coll(), 0);
  EXPECT_EQ(head_a.onlist, 0u);
  EXPECT_EQ(head_a.allocated, 2u);
  ASSERT_NE(head_a.head, nullptr);
  EXPECT_EQ(head_a.head->size, 64u);
  ASSERT_NE(head_a.head->next, nullptr);
  EXPECT_EQ(head_a.head->next->size, 32u);
  EXPECT_EQ(head_a.head->next->next, nullptr);

  // Free remaining blocks so GC removes all PQ nodes.
  for (void* p : live_a) H5FL_blk_free(&head_a, p);
  live_a.clear();
  ASSERT_GE(H5FL_garbage_coll(), 0);
  EXPECT_EQ(head_a.head, nullptr);

  // 2. Exceed per-list limit (48) on H5FL_blk_free -> triggers
  // H5FL__blk_gc_list.
  ASSERT_GE(
      H5FL_set_free_list_limits(4096, 4096, 4096, 4096, 4096, 48, 4096, 4096),
      0);
  void* x1 = H5FL_blk_malloc(&head_a, 32);
  void* x2 = H5FL_blk_malloc(&head_a, 32);
  ASSERT_NE(x1, nullptr);
  ASSERT_NE(x2, nullptr);
  H5FL_blk_free(&head_a, x1);
  EXPECT_EQ(head_a.onlist, 1u);
  H5FL_blk_free(&head_a, x2);
  EXPECT_EQ(head_a.onlist, 0u);
  EXPECT_EQ(head_a.list_mem, 0u);

  // 3. Exceed global block limit (48) across head_a + head_b while each list
  //    stays <= per-list limit (1024) -> triggers H5FL__blk_gc.
  ASSERT_GE(
      H5FL_set_free_list_limits(4096, 4096, 4096, 4096, 48, 1024, 4096, 4096),
      0);
  void* ba = H5FL_blk_malloc(&head_a, 32);
  void* bb = H5FL_blk_malloc(&head_b, 32);
  ASSERT_NE(ba, nullptr);
  ASSERT_NE(bb, nullptr);
  live_a.push_back(ba);
  live_b.push_back(bb);

  H5FL_blk_free(&head_a, live_a.back());
  live_a.pop_back();
  EXPECT_EQ(head_a.onlist, 1u);

  H5FL_blk_free(&head_b, live_b.back());
  live_b.pop_back();
  EXPECT_EQ(head_a.onlist, 0u);
  EXPECT_EQ(head_b.onlist, 0u);
}

TEST(H5FLTest, ArrMallocCallocFreeNullBaseArrayAndReuse) {
  ASSERT_GE(H5open(), 0);
  H5FL_ARR_DEFINE_STATIC(ArrElem, 6);
  H5FL_BARR_DEFINE_STATIC(BaseHeader, BaseArrElem, 4);
  auto* arr_head = &H5FL_ARR_NAME(ArrElem);
  auto* barr_head = &H5FL_ARR_NAME(BaseArrElem);

  ArrElem* a1 = nullptr;
  ArrElem* a2 = nullptr;
  void* ba = nullptr;
  absl::Cleanup cleanup = [&] {
    if (a1 != nullptr) H5FL_arr_free(arr_head, a1);
    if (a2 != nullptr) H5FL_arr_free(arr_head, a2);
    if (ba != nullptr) H5FL_arr_free(barr_head, ba);
    H5FL_garbage_coll();
  };

  // Freeing nullptr is an explicit no-op returning nullptr.
  EXPECT_EQ(H5FL_ARR_FREE(ArrElem, nullptr), nullptr);

  a1 = static_cast<ArrElem*>(H5FL_ARR_MALLOC(ArrElem, 3));
  ASSERT_NE(a1, nullptr);
  EXPECT_TRUE(arr_head->init);
  EXPECT_EQ(arr_head->allocated, 1u);
  a1[0] = {10, 20};
  a1[1] = {30, 40};
  a1[2] = {50, 60};

  // Free and reallocate with H5FL_ARR_CALLOC to verify reuse and zeroing.
  EXPECT_EQ(H5FL_ARR_FREE(ArrElem, a1), nullptr);
  a1 = nullptr;
  EXPECT_EQ(arr_head->list_arr[3].onlist, 1u);

  a2 = static_cast<ArrElem*>(H5FL_ARR_CALLOC(ArrElem, 3));
  ASSERT_NE(a2, nullptr);
  EXPECT_EQ(arr_head->list_arr[3].onlist, 0u);
  EXPECT_EQ(arr_head->allocated, 1u);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(a2[i].id, 0u);
    EXPECT_EQ(a2[i].tag, 0u);
  }

  // Base + array free list allocates sizeof(BaseHeader) + elem * sizeof(elem).
  ba = H5FL_ARR_CALLOC(BaseArrElem, 4);
  ASSERT_NE(ba, nullptr);
  auto* hdr = static_cast<BaseHeader*>(ba);
  EXPECT_EQ(hdr->magic, 0u);
  EXPECT_EQ(hdr->count, 0u);
  hdr->magic = 0xdeadbeefULL;
  hdr->count = 4;
  auto* elems = reinterpret_cast<BaseArrElem*>(static_cast<uint8_t*>(ba) +
                                               sizeof(BaseHeader));
  elems[3].v = 99;
  EXPECT_EQ(elems[3].v, 99u);
}

TEST(H5FLTest, ArrReallocNullSameGrowShrinkAndLimits) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_limits = [] { RestoreDefaultFreeListLimits(); };

  H5FL_ARR_DEFINE_STATIC(ArrElem, 8);
  static H5FL_arr_head_t arr_b = {false,           0,      0, "arr_b", 5, 0,
                                  sizeof(ArrElem), nullptr};
  auto* arr_a = &H5FL_ARR_NAME(ArrElem);

  ArrElem* cur = nullptr;
  void* other = nullptr;
  absl::Cleanup cleanup = [&] {
    if (cur != nullptr) H5FL_arr_free(arr_a, cur);
    if (other != nullptr) H5FL_arr_free(&arr_b, other);
    H5FL_garbage_coll();
  };

  // 1. realloc(nullptr, 2) allocates fresh array.
  cur = static_cast<ArrElem*>(H5FL_ARR_REALLOC(ArrElem, nullptr, 2));
  ASSERT_NE(cur, nullptr);
  cur[0] = {1, 10};
  cur[1] = {2, 20};

  // 2. realloc with same element count returns exact same pointer.
  void* same = H5FL_ARR_REALLOC(ArrElem, cur, 2);
  EXPECT_EQ(same, cur);

  // 3. Grow to 5 elements (preserves first 2).
  cur = static_cast<ArrElem*>(H5FL_ARR_REALLOC(ArrElem, cur, 5));
  ASSERT_NE(cur, nullptr);
  EXPECT_EQ(cur[0].id, 1u);
  EXPECT_EQ(cur[1].tag, 20u);
  cur[2] = {3, 30};

  // 4. Shrink to 1 element (preserves element 0).
  cur = static_cast<ArrElem*>(H5FL_ARR_REALLOC(ArrElem, cur, 1));
  ASSERT_NE(cur, nullptr);
  EXPECT_EQ(cur[0].id, 1u);
  EXPECT_EQ(cur[0].tag, 10u);

  H5FL_arr_free(arr_a, cur);
  cur = nullptr;
  ASSERT_GE(H5FL_garbage_coll(), 0);

  // 5. Exceed per-list array limit (24 bytes): freeing 2 x 16-byte arrays = 32.
  ASSERT_GE(
      H5FL_set_free_list_limits(4096, 4096, 4096, 24, 4096, 4096, 4096, 4096),
      0);
  void* x1 = H5FL_arr_malloc(arr_a, 2);
  void* x2 = H5FL_arr_malloc(arr_a, 2);
  ASSERT_NE(x1, nullptr);
  ASSERT_NE(x2, nullptr);
  H5FL_arr_free(arr_a, x1);
  EXPECT_EQ(arr_a->list_arr[2].onlist, 1u);
  H5FL_arr_free(arr_a, x2);
  EXPECT_EQ(arr_a->list_arr[2].onlist, 0u);
  EXPECT_EQ(arr_a->list_mem, 0u);

  // 6. Exceed global array limit (24 bytes) across arr_a and arr_b.
  ASSERT_GE(
      H5FL_set_free_list_limits(4096, 4096, 24, 1024, 4096, 4096, 4096, 4096),
      0);
  cur = static_cast<ArrElem*>(H5FL_arr_malloc(arr_a, 2));
  other = H5FL_arr_malloc(&arr_b, 2);
  ASSERT_NE(cur, nullptr);
  ASSERT_NE(other, nullptr);

  H5FL_arr_free(arr_a, cur);
  cur = nullptr;
  EXPECT_EQ(arr_a->list_arr[2].onlist, 1u);

  H5FL_arr_free(&arr_b, other);
  other = nullptr;
  EXPECT_EQ(arr_a->list_arr[2].onlist, 0u);
  EXPECT_EQ(arr_b.list_arr[2].onlist, 0u);
}

TEST(H5FLTest, SeqMallocCallocReallocAndFree) {
  ASSERT_GE(H5open(), 0);
  H5FL_SEQ_DEFINE_STATIC(SeqElem);

  SeqElem* seq = nullptr;
  SeqElem* zero_seq = nullptr;
  absl::Cleanup cleanup = [&] {
    if (seq != nullptr) H5FL_SEQ_FREE(SeqElem, seq);
    if (zero_seq != nullptr) H5FL_SEQ_FREE(SeqElem, zero_seq);
    H5FL_garbage_coll();
  };

  seq = H5FL_SEQ_MALLOC(SeqElem, 2);
  ASSERT_NE(seq, nullptr);
  seq[0] = {10, 100};
  seq[1] = {20, 200};

  zero_seq = H5FL_SEQ_CALLOC(SeqElem, 3);
  ASSERT_NE(zero_seq, nullptr);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(zero_seq[i].key, 0u);
    EXPECT_EQ(zero_seq[i].val, 0u);
  }
  EXPECT_EQ(H5FL_SEQ_FREE(SeqElem, zero_seq), nullptr);
  zero_seq = nullptr;

  // Same-size realloc keeps pointer; grow preserves elements; shrink truncates.
  SeqElem* same = H5FL_SEQ_REALLOC(SeqElem, seq, 2);
  EXPECT_EQ(same, seq);

  seq = H5FL_SEQ_REALLOC(SeqElem, seq, 4);
  ASSERT_NE(seq, nullptr);
  EXPECT_EQ(seq[0].key, 10u);
  EXPECT_EQ(seq[1].val, 200u);

  seq = H5FL_SEQ_REALLOC(SeqElem, seq, 1);
  ASSERT_NE(seq, nullptr);
  EXPECT_EQ(seq[0].key, 10u);
  EXPECT_EQ(seq[0].val, 100u);
}

TEST(H5FLTest, FacInitSmallSizeMallocCallocErrorOnBusyTermAndUnlinkOrders) {
  ASSERT_GE(H5open(), 0);

  H5FL_fac_head_t* fac1 = H5FL_fac_init(1);  // Clamped to sizeof(node_t).
  H5FL_fac_head_t* fac2 = H5FL_fac_init(32);
  H5FL_fac_head_t* fac3 = H5FL_fac_init(64);
  ASSERT_NE(fac1, nullptr);
  ASSERT_NE(fac2, nullptr);
  ASSERT_NE(fac3, nullptr);
  EXPECT_GE(fac1->size, sizeof(void*));
  EXPECT_TRUE(fac1->init);

  void* blk1 = nullptr;
  void* blk2 = nullptr;
  absl::Cleanup cleanup = [&] {
    if (blk1 != nullptr && fac1 != nullptr) H5FL_fac_free(fac1, blk1);
    if (blk2 != nullptr && fac2 != nullptr) H5FL_fac_free(fac2, blk2);
    if (fac2 != nullptr) H5FL_fac_term(fac2);
    if (fac1 != nullptr) H5FL_fac_term(fac1);
    if (fac3 != nullptr) H5FL_fac_term(fac3);
    H5FL_garbage_coll();
  };

  blk1 = H5FL_FAC_MALLOC(fac1);
  ASSERT_NE(blk1, nullptr);
  std::memset(blk1, 0x5a, fac1->size);
  EXPECT_EQ(H5FL_FAC_FREE(fac1, blk1), nullptr);
  blk1 = nullptr;

  blk1 = H5FL_FAC_CALLOC(fac1);
  ASSERT_NE(blk1, nullptr);
  std::vector<uint8_t> zbuf(static_cast<uint8_t*>(blk1),
                            static_cast<uint8_t*>(blk1) + fac1->size);
  EXPECT_THAT(zbuf, Each(0));
  EXPECT_EQ(H5FL_FAC_FREE(fac1, blk1), nullptr);
  blk1 = nullptr;

  // Attempting to terminate a factory with outstanding blocks must fail.
  blk2 = H5FL_FAC_MALLOC(fac2);
  ASSERT_NE(blk2, nullptr);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FL_fac_term(fac2), 0);
  }
  EXPECT_EQ(H5FL_FAC_FREE(fac2, blk2), nullptr);
  blk2 = nullptr;

  // GC list currently [fac3 -> fac2 -> fac1]. Terminate in order:
  //   1. fac2 (middle: prev_gc != null, next != null)
  //   2. fac1 (tail: prev_gc != null, next == null)
  //   3. fac3 (head: prev_gc == null, next == null)
  EXPECT_GE(H5FL_fac_term(fac2), 0);
  fac2 = nullptr;
  EXPECT_GE(H5FL_fac_term(fac1), 0);
  fac1 = nullptr;
  EXPECT_GE(H5FL_fac_term(fac3), 0);
  fac3 = nullptr;

  // Also test terminating head when next != null ([fac_b -> fac_a]).
  H5FL_fac_head_t* fac_a = H5FL_fac_init(24);
  H5FL_fac_head_t* fac_b = H5FL_fac_init(24);
  ASSERT_NE(fac_a, nullptr);
  ASSERT_NE(fac_b, nullptr);
  absl::Cleanup cleanup_ab = [&] {
    if (fac_b != nullptr) H5FL_fac_term(fac_b);
    if (fac_a != nullptr) H5FL_fac_term(fac_a);
  };
  EXPECT_GE(H5FL_fac_term(fac_b), 0);
  fac_b = nullptr;
  EXPECT_GE(H5FL_fac_term(fac_a), 0);
  fac_a = nullptr;
}

TEST(H5FLTest, FacListAndGlobalLimitsTriggerGarbageCollection) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_limits = [] { RestoreDefaultFreeListLimits(); };

  H5FL_fac_head_t* fac_a = H5FL_fac_init(32);
  H5FL_fac_head_t* fac_b = H5FL_fac_init(32);
  ASSERT_NE(fac_a, nullptr);
  ASSERT_NE(fac_b, nullptr);

  void* pa = nullptr;
  void* pb = nullptr;
  absl::Cleanup cleanup = [&] {
    if (pa != nullptr) H5FL_fac_free(fac_a, pa);
    if (pb != nullptr) H5FL_fac_free(fac_b, pb);
    H5FL_fac_term(fac_a);
    H5FL_fac_term(fac_b);
    H5FL_garbage_coll();
  };

  // 1. Per-list limit (48 bytes): freeing 2 x 32-byte blocks hits list GC.
  ASSERT_GE(
      H5FL_set_free_list_limits(4096, 4096, 4096, 4096, 4096, 4096, 4096, 48),
      0);
  void* x1 = H5FL_fac_malloc(fac_a);
  void* x2 = H5FL_fac_malloc(fac_a);
  ASSERT_NE(x1, nullptr);
  ASSERT_NE(x2, nullptr);
  H5FL_fac_free(fac_a, x1);
  EXPECT_EQ(fac_a->onlist, 1u);
  H5FL_fac_free(fac_a, x2);
  EXPECT_EQ(fac_a->onlist, 0u);
  EXPECT_EQ(fac_a->allocated, 0u);

  // 2. Global limit (48 bytes) across fac_a and fac_b while per-list is 1024.
  ASSERT_GE(
      H5FL_set_free_list_limits(4096, 4096, 4096, 4096, 4096, 4096, 48, 1024),
      0);
  pa = H5FL_fac_malloc(fac_a);
  pb = H5FL_fac_malloc(fac_b);
  ASSERT_NE(pa, nullptr);
  ASSERT_NE(pb, nullptr);

  H5FL_fac_free(fac_a, pa);
  pa = nullptr;
  EXPECT_EQ(fac_a->onlist, 1u);

  H5FL_fac_free(fac_b, pb);
  pb = nullptr;
  EXPECT_EQ(fac_a->onlist, 0u);
  EXPECT_EQ(fac_b->onlist, 0u);
}

TEST(H5FLTest, GetFreeListSizesReportsActiveAndIdleLists) {
  ASSERT_GE(H5open(), 0);

  static H5FL_reg_head_t reg_head = {false, 0, 0, "sz_reg", 64, nullptr};
  static H5FL_arr_head_t arr_active = {false, 0, 0,  "sz_arr_act",
                                       4,     0, 16, nullptr};
  static H5FL_arr_head_t arr_idle = {false, 0, 0,  "sz_arr_idle",
                                     4,     0, 16, nullptr};
  static H5FL_blk_head_t blk_head = {false, 0, 0, 0, "sz_blk", nullptr};

  H5FL_fac_head_t* fac_head = H5FL_fac_init(128);
  ASSERT_NE(fac_head, nullptr);

  void* reg_p = nullptr;
  void* arr_p = nullptr;
  void* idle_p = nullptr;
  void* blk_p = nullptr;
  void* fac_p = nullptr;
  absl::Cleanup cleanup = [&] {
    if (reg_p != nullptr) H5FL_reg_free(&reg_head, reg_p);
    if (arr_p != nullptr) H5FL_arr_free(&arr_active, arr_p);
    if (idle_p != nullptr) H5FL_arr_free(&arr_idle, idle_p);
    if (blk_p != nullptr) H5FL_blk_free(&blk_head, blk_p);
    if (fac_p != nullptr) H5FL_fac_free(fac_head, fac_p);
    H5FL_fac_term(fac_head);
    H5FL_garbage_coll();
  };

  ASSERT_GE(H5FL_garbage_coll(), 0);
  size_t reg0 = 0;
  size_t arr0 = 0;
  size_t blk0 = 0;
  size_t fac0 = 0;
  ASSERT_GE(H5FL_get_free_list_sizes(&reg0, &arr0, &blk0, &fac0), 0);

  reg_p = H5FL_reg_malloc(&reg_head);
  arr_p = H5FL_arr_malloc(&arr_active, 2);
  idle_p = H5FL_arr_malloc(&arr_idle, 1);
  blk_p = H5FL_blk_malloc(&blk_head, 96);
  fac_p = H5FL_fac_malloc(fac_head);
  ASSERT_NE(reg_p, nullptr);
  ASSERT_NE(arr_p, nullptr);
  ASSERT_NE(idle_p, nullptr);
  ASSERT_NE(blk_p, nullptr);
  ASSERT_NE(fac_p, nullptr);

  // Free and GC arr_idle so it remains on H5FL_arr_gc_head with allocated==0.
  H5FL_arr_free(&arr_idle, idle_p);
  idle_p = nullptr;
  ASSERT_GE(H5FL_garbage_coll(), 0);
  EXPECT_EQ(arr_idle.allocated, 0u);

  size_t reg1 = 0;
  size_t arr1 = 0;
  size_t blk1 = 0;
  size_t fac1 = 0;
  ASSERT_GE(H5FL_get_free_list_sizes(&reg1, &arr1, &blk1, &fac1), 0);
  EXPECT_GE(reg1, reg0 + 64);
  EXPECT_GE(arr1, arr0 + 32);
  EXPECT_GE(blk1, blk0 + 96);
  EXPECT_GE(fac1, fac0 + 128);
}

TEST(H5FLTest, TermPackageHandlesEmptyOutstandingAndFacCleanupStates) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_lib = [] { (void)H5open(); };

  // Close the full HDF5 library first so H5SL and all other callers of H5FL
  // terminate cleanly before we inspect H5FL_term_package() directly.
  ASSERT_GE(H5close(), 0);

  {
    // 1. Package already uninitialized -> returns 0 immediately.
    EXPECT_EQ(0, H5FL_term_package());

    static H5FL_reg_head_t term_reg = {false, 0, 0, "term_reg", 32, nullptr};
    static H5FL_arr_head_t term_arr = {false, 0, 0,  "term_arr",
                                       4,     0, 16, nullptr};
    static H5FL_blk_head_t term_blk = {false, 0, 0, 0, "term_blk", nullptr};

    void* reg_p = nullptr;
    void* arr_p = nullptr;
    void* blk_p = nullptr;
    H5FL_fac_head_t* fac = H5FL_fac_init(24);
    ASSERT_NE(fac, nullptr);
    bool fac_gc_unlinked = false;
    absl::Cleanup cleanup = [&] {
      if (reg_p != nullptr) H5FL_reg_free(&term_reg, reg_p);
      if (arr_p != nullptr) H5FL_arr_free(&term_arr, arr_p);
      if (blk_p != nullptr) H5FL_blk_free(&term_blk, blk_p);
      if (fac != nullptr) {
        if (fac_gc_unlinked) {
          H5FL_FREE(H5FL_fac_head_t, fac);
        } else {
          H5FL_fac_term(fac);
        }
      }
      H5FL_term_package();
    };

    // Seed one cached block onto fac so H5FL_garbage_coll() inside
    // H5FL_term_package() sweeps it before H5FL__fac_term_all() unlinks fac.
    void* fac_tmp = H5FL_fac_malloc(fac);
    ASSERT_NE(fac_tmp, nullptr);
    H5FL_fac_free(fac, fac_tmp);

    // Keep one outstanding allocation in reg, arr, and blk.
    reg_p = H5FL_reg_malloc(&term_reg);
    arr_p = H5FL_arr_malloc(&term_arr, 1);
    blk_p = H5FL_blk_malloc(&term_blk, 40);
    ASSERT_NE(reg_p, nullptr);
    ASSERT_NE(arr_p, nullptr);
    ASSERT_NE(blk_p, nullptr);

    // 2. First H5FL_term_package(): unlinks the idle factory via
    //    H5FL__fac_term_all() and retains reg/arr/blk because allocated > 0.
    const int pending = H5FL_term_package();
    fac_gc_unlinked = true;
    EXPECT_GT(pending, 0);
    EXPECT_FALSE(fac->init);

    // 3. Free all outstanding allocations and factory head, then verify
    //    H5FL_term_package() returns 0 and resets all remaining lists.
    H5FL_reg_free(&term_reg, reg_p);
    reg_p = nullptr;
    H5FL_arr_free(&term_arr, arr_p);
    arr_p = nullptr;
    H5FL_blk_free(&term_blk, blk_p);
    blk_p = nullptr;
    H5FL_FREE(H5FL_fac_head_t, fac);
    fac = nullptr;

    EXPECT_EQ(0, H5FL_term_package());
    EXPECT_FALSE(term_reg.init);
    EXPECT_FALSE(term_arr.init);
    EXPECT_FALSE(term_blk.init);
    EXPECT_EQ(0, H5FL_term_package());
  }

  ASSERT_GE(H5open(), 0);
}

}  // namespace
}  // namespace hdf5
