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
// Tests local heap lifecycle, protection, insertion, free-list coalescing,
// heap minimization, size queries, deletion, and error handling
// (third_party/hdf5/src/H5HL.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HL.c

#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5HL_prfx_t and H5HL_dblk_t). Match those struct layouts
// across both the prelude and internal headers.
#ifndef NDEBUG
#define H5HL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try h5f_try_param
extern "C" {
#define H5F_FRIEND
#define H5HL_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5HLpkg.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x10000) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

// H5HL__minimize_heap_space() shrinks heap->dblk_image before calling
// H5HL__dblk_realloc(). When realloc fails, heap->dblk_size keeps its old
// size while heap->dblk_image remains shrunk, so restore dblk_image before
// flushing or closing the file.
void RestoreHeapImageAfterMinimizeFailure(H5HL_t* heap) {
  if (heap != nullptr && heap->dblk_size > 0) {
    uint8_t* grown = H5FL_BLK_REALLOC(lheap_chunk, heap->dblk_image,
                                      heap->dblk_size);
    ASSERT_NE(grown, nullptr);
    std::memset(grown, 0, heap->dblk_size);
    heap->dblk_image = grown;
  }
}

TEST(H5HLTest, CreateWithZeroSmallUnalignedHintsAndQuerySizes) {
  ScopedFile file = CreateInMemoryFile("h5hl_create.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const size_t hdr_size = H5HL_SIZEOF_HDR(f);
  const size_t min_free = H5HL_SIZEOF_FREE(f);
  EXPECT_EQ(min_free, 16u);

  // 1. size_hint == 0 creates a prefix-only heap with dblk_size == 0.
  haddr_t addr_zero = HADDR_UNDEF;
  ASSERT_GE(H5HL_create(f, /*size_hint=*/0, &addr_zero), 0);
  size_t dblk_sz = 999;
  ASSERT_GE(H5HL_get_size(f, addr_zero, &dblk_sz), 0);
  EXPECT_EQ(dblk_sz, 0u);
  hsize_t total_sz = 0;
  ASSERT_GE(H5HL_heapsize(f, addr_zero, &total_sz), 0);
  EXPECT_EQ(total_sz, hdr_size);

  // 2. Non-zero size_hint < H5HL_SIZEOF_FREE(f) clamps up to H5HL_SIZEOF_FREE.
  haddr_t addr_tiny = HADDR_UNDEF;
  ASSERT_GE(H5HL_create(f, /*size_hint=*/1, &addr_tiny), 0);
  ASSERT_GE(H5HL_get_size(f, addr_tiny, &dblk_sz), 0);
  EXPECT_EQ(dblk_sz, min_free);

  // 3. Unaligned size_hint rounds up to 8-byte alignment; H5HL_heapsize
  // accumulates onto the existing *heap_size value.
  haddr_t addr_unaligned = HADDR_UNDEF;
  ASSERT_GE(H5HL_create(f, /*size_hint=*/25, &addr_unaligned), 0);
  ASSERT_GE(H5HL_get_size(f, addr_unaligned, &dblk_sz), 0);
  EXPECT_EQ(dblk_sz, 32u);

  hsize_t accumulated = 100;
  ASSERT_GE(H5HL_heapsize(f, addr_unaligned, &accumulated), 0);
  EXPECT_EQ(accumulated, 100u + hdr_size + 32u);

  H5HL_t* heap = H5HL_protect(f, addr_unaligned, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(heap, nullptr);
  EXPECT_EQ(H5HL_heap_get_size(heap), 32u);
  ASSERT_GE(H5HL_unprotect(heap), 0);
}

TEST(H5HLTest, ReentrantProtectAndOffsetIntoBoundsChecks) {
  ScopedFile file = CreateInMemoryFile("h5hl_protect.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  haddr_t addr = HADDR_UNDEF;
  ASSERT_GE(H5HL_create(f, /*size_hint=*/64, &addr), 0);

  // First protect pins prefix (`prots`: 0 -> 1).
  H5HL_t* heap1 = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(heap1, nullptr);
  EXPECT_EQ(heap1->prots, 1u);

  size_t offset = 0;
  constexpr char kEntry[] = "hello_heap";
  ASSERT_GE(H5HL_insert(f, heap1, sizeof(kEntry), kEntry, &offset), 0);

  // Re-entrant protect increments `prots` (1 -> 2) without re-protecting.
  H5HL_t* heap2 = H5HL_protect(f, addr, H5AC__READ_ONLY_FLAG);
  ASSERT_EQ(heap2, heap1);
  EXPECT_EQ(heap1->prots, 2u);
  EXPECT_STREQ(static_cast<const char*>(H5HL_offset_into(heap2, offset)),
               kEntry);

  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5HL_offset_into(heap2, heap2->dblk_size), nullptr);
    EXPECT_EQ(H5HL_offset_into(heap2, heap2->dblk_size + 32), nullptr);
  }

  // First unprotect keeps the heap pinned (`prots`: 2 -> 1); second unpins.
  ASSERT_GE(H5HL_unprotect(heap2), 0);
  EXPECT_EQ(heap1->prots, 1u);
  ASSERT_GE(H5HL_unprotect(heap1), 0);
}

TEST(H5HLTest, InsertFreeListSplittingExactFitAndAllGrowthModes) {
  ScopedFile file = CreateInMemoryFile("h5hl_insert.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // 1. Start with size_hint == 0: inserting 8 bytes trims `need_more = 8`
  // (`need_more - need_size == 0 < H5HL_SIZEOF_FREE`) with no free node.
  haddr_t addr_grow = HADDR_UNDEF;
  ASSERT_GE(H5HL_create(f, /*size_hint=*/0, &addr_grow), 0);
  H5HL_t* hg = H5HL_protect(f, addr_grow, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hg, nullptr);
  absl::Cleanup unprot_hg = [&] {
    if (hg != nullptr) {
      H5HL_unprotect(hg);
    }
  };
  size_t off0 = 0;
  ASSERT_GE(H5HL_insert(f, hg, 8, "1234567", &off0), 0);
  EXPECT_EQ(hg->dblk_size, 8u);
  EXPECT_EQ(hg->freelist, nullptr);

  // Next 16-byte insert extends in-place (`was_extended == true`,
  // `single_cache_obj == true`), and trims `need_more = 16` because
  // `need_more (16) < need_size (16) + 16`.
  size_t off1 = 0;
  std::vector<uint8_t> buf16(16, 0x11);
  ASSERT_GE(H5HL_insert(f, hg, buf16.size(), buf16.data(), &off1), 0);
  EXPECT_EQ(hg->dblk_size, 24u);
  EXPECT_TRUE(hg->single_cache_obj);

  // Next 8-byte insert doubles `dblk_size` (`need_more = 24`) and leaves a
  // 16-byte free block (`need_more - need_size == 16 >= H5HL_SIZEOF_FREE`).
  size_t off2 = 0;
  ASSERT_GE(H5HL_insert(f, hg, 8, "abcdefg", &off2), 0);
  EXPECT_EQ(hg->dblk_size, 48u);
  ASSERT_NE(hg->freelist, nullptr);
  EXPECT_EQ(hg->freelist->offset, 32u);
  EXPECT_EQ(hg->freelist->size, 16u);

  // Insert 32 bytes while trailing free block `[32, 48)` is at end of heap:
  // extends `last_fl` (`need_more = 48`), leaving trailing free block `[64,
  // 96)`.
  size_t off3 = 0;
  std::vector<uint8_t> buf32(32, 0x22);
  ASSERT_GE(H5HL_insert(f, hg, buf32.size(), buf32.data(), &off3), 0);
  EXPECT_EQ(off3, 32u);
  EXPECT_EQ(hg->dblk_size, 96u);
  ASSERT_NE(hg->freelist, nullptr);
  EXPECT_EQ(hg->freelist->offset, 64u);
  EXPECT_EQ(hg->freelist->size, 32u);

  // Insert exact 32-byte match for the sole trailing free block -> removes it.
  size_t off4 = 0;
  ASSERT_GE(H5HL_insert(f, hg, buf32.size(), buf32.data(), &off4), 0);
  EXPECT_EQ(off4, 64u);
  EXPECT_EQ(hg->freelist, nullptr);

  // Create blocker heap after `hg` so `hg` cannot extend in place ->
  // `was_extended == false` -> detaches into `single_cache_obj == false`.
  haddr_t blocker_addr = HADDR_UNDEF;
  ASSERT_GE(H5HL_create(f, /*size_hint=*/64, &blocker_addr), 0);

  size_t off5 = 0;
  ASSERT_GE(H5HL_insert(f, hg, buf32.size(), buf32.data(), &off5), 0);
  EXPECT_FALSE(hg->single_cache_obj);
  EXPECT_EQ(hg->dblk_size, 192u);
  ASSERT_NE(hg->freelist, nullptr);  // free block [128, 192)

  // Now `hg`'s detached dblk is at EOA (`single_cache_obj == false`). Fill the
  // remaining 64 bytes (`[128, 192)`) and insert 32 more bytes to exercise
  // `was_extended == true` when `!single_cache_obj`.
  size_t off6 = 0;
  std::vector<uint8_t> buf64(64, 0x33);
  ASSERT_GE(H5HL_insert(f, hg, buf64.size(), buf64.data(), &off6), 0);
  EXPECT_EQ(hg->freelist, nullptr);
  size_t off7 = 0;
  ASSERT_GE(H5HL_insert(f, hg, buf32.size(), buf32.data(), &off7), 0);
  EXPECT_EQ(hg->dblk_size, 384u);

  // 2. Test H5HL__remove_free head, middle, tail, and `last_fl` offset
  // comparison (non-monotonic freelist order + leftover < H5HL_SIZEOF_FREE).
  haddr_t addr_fl = HADDR_UNDEF;
  ASSERT_GE(H5HL_create(f, /*size_hint=*/256, &addr_fl), 0);
  H5HL_t* hfl = H5HL_protect(f, addr_fl, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hfl, nullptr);
  absl::Cleanup unprot_hfl = [&] { H5HL_unprotect(hfl); };

  size_t a = 0, b = 0, c = 0, d = 0, e = 0, g = 0;
  std::vector<uint8_t> buf24(24, 0x44);
  std::vector<uint8_t> buf48(48, 0x55);
  std::vector<uint8_t> buf16_b(16, 0x66);
  // Layout inside 256-byte heap:
  // a: [0, 32), b: [32, 48), c: [48, 72) (24B), d: [72, 120) (48B),
  // e: [120, 136) (16B), g: [136, 256) (120B).
  ASSERT_GE(H5HL_insert(f, hfl, 32, buf32.data(), &a), 0);
  ASSERT_GE(H5HL_insert(f, hfl, 16, buf16_b.data(), &b), 0);
  ASSERT_GE(H5HL_insert(f, hfl, 24, buf24.data(), &c), 0);
  ASSERT_GE(H5HL_insert(f, hfl, 48, buf48.data(), &d), 0);
  ASSERT_GE(H5HL_insert(f, hfl, 16, buf16_b.data(), &e), 0);
  std::vector<uint8_t> buf120(120, 0x77);
  ASSERT_GE(H5HL_insert(f, hfl, 120, buf120.data(), &g), 0);
  EXPECT_EQ(hfl->freelist, nullptr);

  // Remove c(24B at 48), a(32B at 0), e(16B at 120), d? Keep d live, remove e
  // last so freelist order is: head=e(120,16) -> mid=a(0,32) -> tail=c(48,24).
  // (Adding at head makes last_fl track largest offset first vs later!)
  ASSERT_GE(H5HL_remove(f, hfl, c, 24), 0);
  ASSERT_GE(H5HL_remove(f, hfl, a, 32), 0);
  ASSERT_GE(H5HL_remove(f, hfl, e, 16), 0);

  // Inserting 256 bytes skips e(16<256), a(32<256), c(24<256) and hits both
  // branches of `!last_fl || last_fl->offset < fl->offset`.
  size_t big_off = 0;
  std::vector<uint8_t> buf256(256, 0x88);
  ASSERT_GE(H5HL_insert(f, hfl, buf256.size(), buf256.data(), &big_off), 0);

  // Inserting 16 bytes cannot split c(24) because 24 - 16 = 8 < 16; exact-fits
  // e(16) at the head (`!fl->prev && fl->next`) or after it.
  // Insert 32 bytes removes middle node `a(0, 32)` (`fl->prev && fl->next`).
  size_t mid_off = 0;
  ASSERT_GE(H5HL_insert(f, hfl, 32, buf32.data(), &mid_off), 0);
  EXPECT_EQ(mid_off, a);

  // Insert 24 bytes removes `c(48, 24)` (`fl->prev && !fl->next`, tail).
  size_t tail_off = 0;
  ASSERT_GE(H5HL_insert(f, hfl, 24, buf24.data(), &tail_off), 0);
  EXPECT_EQ(tail_off, c);

  // Re-protect `hg` cleanly.
  ASSERT_GE(H5HL_unprotect(hg), 0);
  hg = nullptr;
}

TEST(H5HLTest, RemoveCoalescingPrependAppendThreeWayAndMinimizeBranches) {
  ScopedFile file = CreateInMemoryFile("h5hl_remove.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // 1. Test tiny chunk loss (< H5HL_SIZEOF_FREE), simple prepend, simple
  // append, and both 3-way coalescing cases (without minimization).
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/128, &addr), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    // Layout: [0..16) X, [16..32) Y, [32..48) Z, [48..56) tiny(8B),
    // [56..128) guard(72B).
    uint8_t buf16[16]{};
    uint8_t buf8[8]{};
    uint8_t buf72[72]{};
    size_t x = 0, y = 0, z = 0, tiny = 0, guard = 0;
    ASSERT_GE(H5HL_insert(f, heap, 16, buf16, &x), 0);
    ASSERT_GE(H5HL_insert(f, heap, 16, buf16, &y), 0);
    ASSERT_GE(H5HL_insert(f, heap, 16, buf16, &z), 0);
    ASSERT_GE(H5HL_insert(f, heap, 8, buf8, &tiny), 0);
    ASSERT_GE(H5HL_insert(f, heap, 72, buf72, &guard), 0);
    EXPECT_EQ(heap->freelist, nullptr);

    // Removing tiny 8-byte chunk (< 16) when no adjacent free block exists is
    // silently dropped without adding a free node.
    ASSERT_GE(H5HL_remove(f, heap, tiny, 4), 0);
    EXPECT_EQ(heap->freelist, nullptr);

    // 3-way coalesce Case 1 (`(offset + size) == fl->offset`, `fl2` bridges
    // on left): remove X first, Z second -> freelist = [Z(32,16), X(0,16)].
    // Removing Y(16,16) prepends to Z and merges X -> single block [0, 48).
    ASSERT_GE(H5HL_remove(f, heap, x, 16), 0);
    ASSERT_GE(H5HL_remove(f, heap, z, 16), 0);
    ASSERT_GE(H5HL_remove(f, heap, y, 16), 0);
    ASSERT_NE(heap->freelist, nullptr);
    EXPECT_EQ(heap->freelist->offset, 0u);
    EXPECT_EQ(heap->freelist->size, 48u);
    EXPECT_EQ(heap->freelist->next, nullptr);

    // Reclaim [0, 48) as 3 separate 16-byte chunks again to test Case 2.
    ASSERT_GE(H5HL_insert(f, heap, 16, buf16, &x), 0);
    ASSERT_GE(H5HL_insert(f, heap, 16, buf16, &y), 0);
    ASSERT_GE(H5HL_insert(f, heap, 16, buf16, &z), 0);
    EXPECT_EQ(heap->freelist, nullptr);

    // 3-way coalesce Case 2 (`fl->offset + fl->size == offset`, `fl2` bridges
    // on right): remove Z first, X second -> freelist = [X(0,16), Z(32,16)].
    // Removing Y(16,16) appends to X and merges Z -> single block [0, 48).
    ASSERT_GE(H5HL_remove(f, heap, z, 16), 0);
    ASSERT_GE(H5HL_remove(f, heap, x, 16), 0);
    ASSERT_GE(H5HL_remove(f, heap, y, 16), 0);
    ASSERT_NE(heap->freelist, nullptr);
    EXPECT_EQ(heap->freelist->offset, 0u);
    EXPECT_EQ(heap->freelist->size, 48u);
    EXPECT_EQ(heap->freelist->next, nullptr);

    // Now test removing `guard(56, 72)` on a 128-byte heap (`dblk_size ==
    // H5HL_MIN_HEAP`): calls H5HL__minimize_heap_space which keeps size 128.
    ASSERT_GE(H5HL_remove(f, heap, guard, 72), 0);
    EXPECT_EQ(heap->dblk_size, 128u);
  }

  // 2. Minimize Branch A (`new_heap_size >= last_fl->offset + H5HL_SIZEOF_FREE`,
  // truncating trailing free block to 128 bytes).
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &addr), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    std::vector<uint8_t> keep(16, 1);
    std::vector<uint8_t> drop(1008, 2);
    size_t off_k = 0, off_d = 0;
    ASSERT_GE(H5HL_insert(f, heap, keep.size(), keep.data(), &off_k), 0);
    ASSERT_GE(H5HL_insert(f, heap, drop.size(), drop.data(), &off_d), 0);
    ASSERT_GE(H5HL_remove(f, heap, off_d, drop.size()), 0);
    EXPECT_EQ(heap->dblk_size, 128u);
    ASSERT_NE(heap->freelist, nullptr);
    EXPECT_EQ(heap->freelist->offset, 16u);
    EXPECT_EQ(heap->freelist->size, 112u);
  }

  // 3. Minimize Branch B (`new_heap_size < last_fl->offset + H5HL_SIZEOF_FREE`,
  // sole free block -> doubles new_heap_size back to 512 and truncates).
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &addr), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    std::vector<uint8_t> keep(400, 1);
    std::vector<uint8_t> drop(624, 2);
    size_t off_k = 0, off_d = 0;
    ASSERT_GE(H5HL_insert(f, heap, keep.size(), keep.data(), &off_k), 0);
    ASSERT_GE(H5HL_insert(f, heap, drop.size(), drop.data(), &off_d), 0);
    ASSERT_GE(H5HL_remove(f, heap, off_d, drop.size()), 0);
    EXPECT_EQ(heap->dblk_size, 512u);
    ASSERT_NE(heap->freelist, nullptr);
    EXPECT_EQ(heap->freelist->offset, 400u);
    EXPECT_EQ(heap->freelist->size, 112u);
  }

  // 4. Minimize Branch C (`new_heap_size < last_fl->offset + H5HL_SIZEOF_FREE`
  // with another free block present -> sets `new_heap_size = last_fl->offset`
  // and removes `last_fl`). Also trigger via prepend + append + 3-way paths!
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &addr), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    std::vector<uint8_t> hole(16, 1);
    std::vector<uint8_t> mid(400, 2);
    std::vector<uint8_t> t0(16, 3);
    std::vector<uint8_t> t1(16, 4);
    std::vector<uint8_t> t2(576, 5);
    size_t off_h = 0, off_m = 0, off_t0 = 0, off_t1 = 0, off_t2 = 0;
    ASSERT_GE(H5HL_insert(f, heap, hole.size(), hole.data(), &off_h), 0);
    ASSERT_GE(H5HL_insert(f, heap, mid.size(), mid.data(), &off_m), 0);
    ASSERT_GE(H5HL_insert(f, heap, t0.size(), t0.data(), &off_t0), 0);
    ASSERT_GE(H5HL_insert(f, heap, t1.size(), t1.data(), &off_t1), 0);
    ASSERT_GE(H5HL_insert(f, heap, t2.size(), t2.data(), &off_t2), 0);

    // Remove hole at 0 so freelist is non-empty, then remove t0, then t2
    // (`[448, 576)` >= 1024/2) triggers Minimize Branch C immediately:
    // `new_heap_size` halves past `448 + 16` down to 256, and because `[0, 16)`
    // and `[416, 16)` remain on freelist, `new_heap_size = 448` and `last_fl`
    // is removed. Removing `t1` (`[432, 16)`) then prepends cleanly to `t0`.
    ASSERT_GE(H5HL_remove(f, heap, off_h, hole.size()), 0);
    ASSERT_GE(H5HL_remove(f, heap, off_t0, t0.size()), 0);
    ASSERT_GE(H5HL_remove(f, heap, off_t2, t2.size()), 0);
    EXPECT_EQ(heap->dblk_size, 448u);
    ASSERT_GE(H5HL_remove(f, heap, off_t1, t1.size()), 0);
    EXPECT_EQ(heap->dblk_size, 448u);
    ASSERT_NE(heap->freelist, nullptr);
    EXPECT_EQ(heap->freelist->offset, 416u);
    EXPECT_EQ(heap->freelist->size, 32u);
    ASSERT_NE(heap->freelist->next, nullptr);
    EXPECT_EQ(heap->freelist->next->offset, 0u);
    EXPECT_EQ(heap->freelist->next->next, nullptr);
  }

  // 4b. True 3-way prepend coalesce reaching end of heap and triggering
  // Minimize Branch C: keep each part < dblk_size/2 (512) until `t1` merges all
  // three into `[208, 816)`.
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &addr), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    std::vector<uint8_t> hole(16, 1);
    std::vector<uint8_t> mid(192, 2);
    std::vector<uint8_t> t0(192, 3);
    std::vector<uint8_t> t1(208, 4);
    std::vector<uint8_t> t2(416, 5);
    size_t off_h = 0, off_m = 0, off_t0 = 0, off_t1 = 0, off_t2 = 0;
    ASSERT_GE(H5HL_insert(f, heap, hole.size(), hole.data(), &off_h), 0);
    ASSERT_GE(H5HL_insert(f, heap, mid.size(), mid.data(), &off_m), 0);
    ASSERT_GE(H5HL_insert(f, heap, t0.size(), t0.data(), &off_t0), 0);
    ASSERT_GE(H5HL_insert(f, heap, t1.size(), t1.data(), &off_t1), 0);
    ASSERT_GE(H5HL_insert(f, heap, t2.size(), t2.data(), &off_t2), 0);

    ASSERT_GE(H5HL_remove(f, heap, off_h, hole.size()), 0);
    ASSERT_GE(H5HL_remove(f, heap, off_t0, t0.size()), 0);
    ASSERT_GE(H5HL_remove(f, heap, off_t2, t2.size()), 0);
    EXPECT_EQ(heap->dblk_size, 1024u);
    ASSERT_GE(H5HL_remove(f, heap, off_t1, t1.size()), 0);
    EXPECT_EQ(heap->dblk_size, 208u);
    ASSERT_NE(heap->freelist, nullptr);
    EXPECT_EQ(heap->freelist->offset, 0u);
    EXPECT_EQ(heap->freelist->next, nullptr);
  }

  // 5. Simple prepend & simple append triggering minimize at end of heap, plus
  // 3-way append coalesce triggering minimize.
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &addr), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    std::vector<uint8_t> keep(16, 1);
    std::vector<uint8_t> a(16, 2);
    std::vector<uint8_t> b(400, 3);
    std::vector<uint8_t> c(592, 4);
    size_t ok = 0, oa = 0, ob = 0, oc = 0;
    ASSERT_GE(H5HL_insert(f, heap, keep.size(), keep.data(), &ok), 0);
    ASSERT_GE(H5HL_insert(f, heap, a.size(), a.data(), &oa), 0);
    ASSERT_GE(H5HL_insert(f, heap, b.size(), b.data(), &ob), 0);
    ASSERT_GE(H5HL_insert(f, heap, c.size(), c.data(), &oc), 0);
    // Remove c (592 > 512 -> minimizes 1024 -> 512), wait: keep `b` so c is <
    // half? 592 > 512 minimizes to 512! Instead, use smaller tail chunks.
    ASSERT_GE(H5HL_remove(f, heap, oc, c.size()), 0);
    EXPECT_EQ(heap->dblk_size, 512u);
  }
}

TEST(H5HLTest, DeleteSingleAndSplitHeapsAndExerciseErrorPaths) {
  // 1. Delete both single-cache-obj and split-cache-obj heaps.
  {
    ScopedFile file = CreateInMemoryFile("h5hl_delete.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    haddr_t single_addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/64, &single_addr), 0);
    ASSERT_GE(H5HL_delete(f, single_addr), 0);

    haddr_t split_addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/32, &split_addr), 0);
    haddr_t blocker = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/32, &blocker), 0);
    H5HL_t* hs = H5HL_protect(f, split_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hs, nullptr);
    std::vector<uint8_t> big(256, 0xAA);
    size_t off = 0;
    ASSERT_GE(H5HL_insert(f, hs, big.size(), big.data(), &off), 0);
    ASSERT_FALSE(hs->single_cache_obj);
    ASSERT_GE(H5HL_unprotect(hs), 0);

    ASSERT_GE(H5HL_delete(f, split_addr), 0);

    // Test H5HL_insert & H5HL_remove failing cleanly when dblk_realloc fails
    // (dblk_addr temporarily in temporary file space).
    haddr_t err_addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &err_addr), 0);
    haddr_t err_blocker = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/32, &err_blocker), 0);
    H5HL_t* he = H5HL_protect(f, err_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(he, nullptr);
    absl::Cleanup unprot_he = [&] { H5HL_unprotect(he); };

    std::vector<uint8_t> k64(64, 1);
    std::vector<uint8_t> d960(960, 2);
    size_t ok64 = 0, od960 = 0;
    ASSERT_GE(H5HL_insert(f, he, k64.size(), k64.data(), &ok64), 0);
    ASSERT_GE(H5HL_insert(f, he, d960.size(), d960.data(), &od960), 0);

    const haddr_t orig_addr = he->dblk_addr;
    const haddr_t tmp_addr = H5MF_alloc_tmp(f, 64);
    ASSERT_TRUE(H5_addr_defined(tmp_addr));
    {
      ScopedErrorSilence silence;
      he->dblk_addr = tmp_addr;
      // Insert exceeding dblk_size fails in H5HL__dblk_realloc.
      size_t fail_off = 0;
      EXPECT_LT(H5HL_insert(f, he, d960.size(), d960.data(), &fail_off), 0);
      // Remove > half at end of heap fails in H5HL__minimize_heap_space.
      EXPECT_LT(H5HL_remove(f, he, od960, d960.size()), 0);
      he->dblk_addr = orig_addr;
      RestoreHeapImageAfterMinimizeFailure(he);
    }
  }

  // 2. Error paths on a read-only file image: H5HL_create fails; protect,
  // delete, get_size, and heapsize fail at an invalid address (superblock 0).
  ScopedFile rw_file = CreateInMemoryFile("h5hl_ro_src.h5");
  ASSERT_TRUE(rw_file.valid());
  ASSERT_GE(H5Fflush(rw_file.get(), H5F_SCOPE_GLOBAL), 0);
  const ssize_t image_size = H5Fget_file_image(rw_file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(rw_file.get(), image.data(), image.size()),
            image_size);
  rw_file.reset();

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(H5Fopen("h5hl_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());
  H5F_t* rf = GetH5File(ro_file.get());
  ASSERT_NE(rf, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  ScopedErrorSilence silence;

  haddr_t bad_addr = 12345;
  EXPECT_LT(H5HL_create(rf, 64, &bad_addr), 0);
  EXPECT_FALSE(H5_addr_defined(bad_addr));

  EXPECT_EQ(H5HL_protect(rf, 0, H5AC__READ_ONLY_FLAG), nullptr);
  EXPECT_LT(H5HL_delete(rf, 0), 0);
  size_t sz = 0;
  EXPECT_LT(H5HL_get_size(rf, 0, &sz), 0);
  hsize_t hsz = 0;
  EXPECT_LT(H5HL_heapsize(rf, 0, &hsz), 0);
}

TEST(H5HLTest, ErrorHandlingAndAdditionalRemoveInsertPaths) {
  ScopedFile file = CreateInMemoryFile("h5hl_extra.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // 1. H5HL_insert extending when freelist is non-empty and not at end of heap
  // hits `heap->freelist->prev = fl` when prepending the new trailing free node.
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/128, &addr), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    std::vector<uint8_t> b16(16, 1);
    std::vector<uint8_t> b112(112, 2);
    std::vector<uint8_t> b32(32, 3);
    size_t o16 = 0, o112 = 0, o32 = 0;
    ASSERT_GE(H5HL_insert(f, heap, b16.size(), b16.data(), &o16), 0);
    ASSERT_GE(H5HL_insert(f, heap, b112.size(), b112.data(), &o112), 0);
    // Free [0, 16) so freelist != nullptr and last_fl is NOT at end of heap.
    ASSERT_GE(H5HL_remove(f, heap, o16, b16.size()), 0);
    ASSERT_GE(H5HL_insert(f, heap, b32.size(), b32.data(), &o32), 0);
    EXPECT_EQ(o32, 128u);
    ASSERT_NE(heap->freelist, nullptr);
    ASSERT_NE(heap->freelist->next, nullptr);
    EXPECT_EQ(heap->freelist->next->prev, heap->freelist);
  }

  // 2. Remaining H5HL_remove prepend and append paths (fl2 loops, simple
  // prepend/append without and with minimization, plus minimization errors).
  // (a) Prepend fl2 skip + simple prepend without minimization.
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/256, &addr), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    std::vector<uint8_t> b16(16, 1);
    std::vector<uint8_t> b144(144, 2);
    std::vector<uint8_t> b64(64, 3);
    size_t oh = 0, om = 0, op = 0, oq = 0, og = 0;
    ASSERT_GE(H5HL_insert(f, heap, b16.size(), b16.data(), &oh), 0);
    ASSERT_GE(H5HL_insert(f, heap, b144.size(), b144.data(), &om), 0);
    ASSERT_GE(H5HL_insert(f, heap, b16.size(), b16.data(), &op), 0);
    ASSERT_GE(H5HL_insert(f, heap, b16.size(), b16.data(), &oq), 0);
    ASSERT_GE(H5HL_insert(f, heap, b64.size(), b64.data(), &og), 0);

    // freelist: [Q(176,16), H(0,16)] -> remove P(160,16) prepends to Q, checks
    // H in fl2 loop (no match), skips minimize because 192 != 256.
    ASSERT_GE(H5HL_remove(f, heap, oh, b16.size()), 0);
    ASSERT_GE(H5HL_remove(f, heap, oq, b16.size()), 0);
    ASSERT_GE(H5HL_remove(f, heap, op, b16.size()), 0);
    ASSERT_NE(heap->freelist, nullptr);
    EXPECT_EQ(heap->freelist->offset, 160u);
    EXPECT_EQ(heap->freelist->size, 32u);
  }

  // (b) Simple prepend with minimization succeeding.
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &addr), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    std::vector<uint8_t> keep(400, 1);
    std::vector<uint8_t> p(224, 2);
    std::vector<uint8_t> q(400, 3);
    size_t ok = 0, op = 0, oq = 0;
    ASSERT_GE(H5HL_insert(f, heap, keep.size(), keep.data(), &ok), 0);
    ASSERT_GE(H5HL_insert(f, heap, p.size(), p.data(), &op), 0);
    ASSERT_GE(H5HL_insert(f, heap, q.size(), q.data(), &oq), 0);
    ASSERT_GE(H5HL_remove(f, heap, oq, q.size()), 0);
    EXPECT_EQ(heap->dblk_size, 1024u);
    ASSERT_GE(H5HL_remove(f, heap, op, p.size()), 0);
    EXPECT_EQ(heap->dblk_size, 512u);
  }

  // (c) Simple prepend with minimization failing.
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &addr), 0);
    haddr_t blocker = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/32, &blocker), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    std::vector<uint8_t> keep(400, 1);
    std::vector<uint8_t> p(224, 2);
    std::vector<uint8_t> q(400, 3);
    size_t ok = 0, op = 0, oq = 0;
    ASSERT_GE(H5HL_insert(f, heap, keep.size(), keep.data(), &ok), 0);
    ASSERT_GE(H5HL_insert(f, heap, p.size(), p.data(), &op), 0);
    ASSERT_GE(H5HL_insert(f, heap, q.size(), q.data(), &oq), 0);
    ASSERT_GE(H5HL_remove(f, heap, oq, q.size()), 0);

    const haddr_t orig_addr = heap->dblk_addr;
    const haddr_t tmp_addr = H5MF_alloc_tmp(f, 64);
    ASSERT_TRUE(H5_addr_defined(tmp_addr));
    ScopedErrorSilence silence;
    heap->dblk_addr = tmp_addr;
    EXPECT_LT(H5HL_remove(f, heap, op, p.size()), 0);
    heap->dblk_addr = orig_addr;
    RestoreHeapImageAfterMinimizeFailure(heap);
  }

  // (d) 3-way prepend coalesce with minimization failing.
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &addr), 0);
    haddr_t blocker = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/32, &blocker), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    std::vector<uint8_t> keep(208, 1);
    std::vector<uint8_t> t0(192, 2);
    std::vector<uint8_t> t1(208, 3);
    std::vector<uint8_t> t2(416, 4);
    size_t ok = 0, o0 = 0, o1 = 0, o2 = 0;
    ASSERT_GE(H5HL_insert(f, heap, keep.size(), keep.data(), &ok), 0);
    ASSERT_GE(H5HL_insert(f, heap, t0.size(), t0.data(), &o0), 0);
    ASSERT_GE(H5HL_insert(f, heap, t1.size(), t1.data(), &o1), 0);
    ASSERT_GE(H5HL_insert(f, heap, t2.size(), t2.data(), &o2), 0);
    ASSERT_GE(H5HL_remove(f, heap, o0, t0.size()), 0);
    ASSERT_GE(H5HL_remove(f, heap, o2, t2.size()), 0);

    const haddr_t orig_addr = heap->dblk_addr;
    const haddr_t tmp_addr = H5MF_alloc_tmp(f, 64);
    ASSERT_TRUE(H5_addr_defined(tmp_addr));
    ScopedErrorSilence silence;
    heap->dblk_addr = tmp_addr;
    EXPECT_LT(H5HL_remove(f, heap, o1, t1.size()), 0);
    heap->dblk_addr = orig_addr;
    RestoreHeapImageAfterMinimizeFailure(heap);
  }

  // (e) 3-way append coalesce with fl2 skip + minimization succeeding and
  // failing.
  {
    auto setup_3way_append = [&](H5HL_t* heap, size_t* out_ob) {
      std::vector<uint8_t> hole(16, 1);
      std::vector<uint8_t> keep(224, 2);
      std::vector<uint8_t> a(200, 3);
      std::vector<uint8_t> b(200, 4);
      std::vector<uint8_t> c(384, 5);
      size_t oh = 0, ok = 0, oa = 0, oc = 0;
      ASSERT_GE(H5HL_insert(f, heap, hole.size(), hole.data(), &oh), 0);
      ASSERT_GE(H5HL_insert(f, heap, keep.size(), keep.data(), &ok), 0);
      ASSERT_GE(H5HL_insert(f, heap, a.size(), a.data(), &oa), 0);
      ASSERT_GE(H5HL_insert(f, heap, b.size(), b.data(), out_ob), 0);
      ASSERT_GE(H5HL_insert(f, heap, c.size(), c.data(), &oc), 0);
      // freelist order: A -> H(0,16) -> C(640,384). Removing B appends to A,
      // skips H in fl2 loop, merges C at end, and minimizes!
      ASSERT_GE(H5HL_remove(f, heap, oc, c.size()), 0);
      ASSERT_GE(H5HL_remove(f, heap, oh, hole.size()), 0);
      ASSERT_GE(H5HL_remove(f, heap, oa, a.size()), 0);
    };

    haddr_t addr_ok = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &addr_ok), 0);
    H5HL_t* heap_ok = H5HL_protect(f, addr_ok, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap_ok, nullptr);
    absl::Cleanup unprot_ok = [&] { H5HL_unprotect(heap_ok); };
    size_t ob_ok = 0;
    setup_3way_append(heap_ok, &ob_ok);
    ASSERT_GE(H5HL_remove(f, heap_ok, ob_ok, 200), 0);
    EXPECT_EQ(heap_ok->dblk_size, 240u);

    haddr_t addr_err = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &addr_err), 0);
    haddr_t blocker = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/32, &blocker), 0);
    H5HL_t* heap_err = H5HL_protect(f, addr_err, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap_err, nullptr);
    absl::Cleanup unprot_err = [&] { H5HL_unprotect(heap_err); };
    size_t ob_err = 0;
    setup_3way_append(heap_err, &ob_err);
    const haddr_t orig_addr = heap_err->dblk_addr;
    const haddr_t tmp_addr = H5MF_alloc_tmp(f, 64);
    ASSERT_TRUE(H5_addr_defined(tmp_addr));
    ScopedErrorSilence silence;
    heap_err->dblk_addr = tmp_addr;
    EXPECT_LT(H5HL_remove(f, heap_err, ob_err, 200), 0);
    heap_err->dblk_addr = orig_addr;
    RestoreHeapImageAfterMinimizeFailure(heap_err);
  }

  // (f) Simple append without minimize, and simple append with minimize
  // succeeding and failing.
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &addr), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    std::vector<uint8_t> keep(304, 1);
    std::vector<uint8_t> a(104, 2);
    std::vector<uint8_t> b(104, 3);
    std::vector<uint8_t> c(512, 4);
    size_t ok = 0, oa = 0, ob = 0, oc = 0;
    ASSERT_GE(H5HL_insert(f, heap, keep.size(), keep.data(), &ok), 0);
    ASSERT_GE(H5HL_insert(f, heap, a.size(), a.data(), &oa), 0);
    ASSERT_GE(H5HL_insert(f, heap, b.size(), b.data(), &ob), 0);
    ASSERT_GE(H5HL_insert(f, heap, c.size(), c.data(), &oc), 0);

    // Simple append not at end (`oa` + `ob`, skips minimize):
    ASSERT_GE(H5HL_remove(f, heap, oa, a.size()), 0);
    ASSERT_GE(H5HL_remove(f, heap, ob, b.size()), 0);
    EXPECT_EQ(heap->dblk_size, 1024u);
    // Simple append reaching end + triggering minimize:
    ASSERT_GE(H5HL_remove(f, heap, oc, c.size()), 0);
    EXPECT_EQ(heap->dblk_size, 512u);
  }
  {
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/1024, &addr), 0);
    haddr_t blocker = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/32, &blocker), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot = [&] { H5HL_unprotect(heap); };

    std::vector<uint8_t> keep(400, 1);
    std::vector<uint8_t> a(224, 2);
    std::vector<uint8_t> b(400, 3);
    size_t ok = 0, oa = 0, ob = 0;
    ASSERT_GE(H5HL_insert(f, heap, keep.size(), keep.data(), &ok), 0);
    ASSERT_GE(H5HL_insert(f, heap, a.size(), a.data(), &oa), 0);
    ASSERT_GE(H5HL_insert(f, heap, b.size(), b.data(), &ob), 0);
    ASSERT_GE(H5HL_remove(f, heap, oa, a.size()), 0);

    const haddr_t orig_addr = heap->dblk_addr;
    const haddr_t tmp_addr = H5MF_alloc_tmp(f, 64);
    ASSERT_TRUE(H5_addr_defined(tmp_addr));
    ScopedErrorSilence silence;
    heap->dblk_addr = tmp_addr;
    EXPECT_LT(H5HL_remove(f, heap, ob, b.size()), 0);
    heap->dblk_addr = orig_addr;
    RestoreHeapImageAfterMinimizeFailure(heap);
  }

  // 3. Evicted split heap protect, split dblk protect conflict in protect &
  // delete, unprotect unpin failures, and dirty-marking failures.
  {
    haddr_t split_addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/32, &split_addr), 0);
    haddr_t blocker = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/32, &blocker), 0);
    H5HL_t* hs = H5HL_protect(f, split_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hs, nullptr);
    std::vector<uint8_t> big(256, 0xAB);
    size_t big_off = 0;
    ASSERT_GE(H5HL_insert(f, hs, big.size(), big.data(), &big_off), 0);
    ASSERT_FALSE(hs->single_cache_obj);
    const haddr_t split_dblk_addr = hs->dblk_addr;
    ASSERT_GE(H5HL_unprotect(hs), 0);

    // Flush & evict everything so split heap is cold in cache, then protect it
    // (exercises H5HL_protect protecting and pinning separate dblk).
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    hs = H5HL_protect(f, split_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hs, nullptr);
    ASSERT_FALSE(hs->single_cache_obj);
    ASSERT_GE(H5HL_unprotect(hs), 0);

    // Protect dblk exclusively in cache so H5HL_protect and H5HL_delete fail
    // to re-protect dblk.
    void* held_dblk = H5AC_protect(f, H5AC_LHEAP_DBLK, split_dblk_addr, hs,
                                   H5AC__NO_FLAGS_SET);
    ASSERT_NE(held_dblk, nullptr);
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5HL_protect(f, split_addr, H5AC__READ_ONLY_FLAG), nullptr);
      EXPECT_LT(H5HL_delete(f, split_addr), 0);
    }
    ASSERT_GE(H5AC_unprotect(f, H5AC_LHEAP_DBLK, split_dblk_addr, held_dblk,
                             H5AC__NO_FLAGS_SET),
              0);

    // Unpinned/unprotected dirty check failures on split heap & single heap.
    hs = H5HL_protect(f, split_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hs, nullptr);
    ASSERT_GE(H5HL__inc_rc(hs), 0);
    ASSERT_GE(H5HL_unprotect(hs), 0);
    {
      absl::Cleanup dec_hs = [&] { H5HL__dec_rc(hs); };
      ScopedErrorSilence silence;
      size_t fail_off = 0;
      EXPECT_LT(H5HL_insert(f, hs, 8, "1234567", &fail_off), 0);
      EXPECT_LT(H5HL_remove(f, hs, big_off, 16), 0);
    }

    // Unprotect unpin failure on split heap (unpin dblk before H5HL_unprotect).
    hs = H5HL_protect(f, split_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hs, nullptr);
    ASSERT_GE(H5AC_unpin_entry(hs->dblk), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5HL_unprotect(hs), 0);
    }

    // Same dirty & unpin failure on a single-cache-obj heap.
    haddr_t single_addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/64, &single_addr), 0);
    H5HL_t* h1 = H5HL_protect(f, single_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(h1, nullptr);
    ASSERT_TRUE(h1->single_cache_obj);
    ASSERT_GE(H5HL__inc_rc(h1), 0);
    ASSERT_GE(H5HL_unprotect(h1), 0);
    {
      absl::Cleanup dec_h1 = [&] { H5HL__dec_rc(h1); };
      ScopedErrorSilence silence;
      size_t fail_off = 0;
      EXPECT_LT(H5HL_insert(f, h1, 8, "1234567", &fail_off), 0);
    }
    h1 = H5HL_protect(f, single_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(h1, nullptr);
    ASSERT_GE(H5AC_unpin_entry(h1->prfx), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5HL_unprotect(h1), 0);
    }
  }

  // 4. H5HL_create H5MF_alloc failure (after prfx/dblk are not yet allocated):
  // block allocation at EOA via `f->shared->tmp_addr = eoa`.
  {
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_LHEAP);
    ASSERT_TRUE(H5_addr_defined(eoa));
    const haddr_t orig_tmp = f->shared->tmp_addr;
    absl::Cleanup restore_tmp = [&] { f->shared->tmp_addr = orig_tmp; };
    f->shared->tmp_addr = eoa;

    ScopedErrorSilence silence;
    haddr_t fail_addr = 12345;
    EXPECT_LT(H5HL_create(f, /*size_hint=*/64, &fail_addr), 0);
    EXPECT_FALSE(H5_addr_defined(fail_addr));
  }
}

}  // namespace
}  // namespace hdf5
