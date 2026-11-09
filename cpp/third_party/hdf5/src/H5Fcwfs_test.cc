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
// Tests global heap collection cache ("CWFS": Collections With Free Space)
// management including insertion, eviction, search, extension, LRU advance,
// and removal (third_party/hdf5/src/H5Fcwfs.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Fcwfs.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug/stats fields in H5AC_info_t
// (the first member of H5HG_heap_t) and related cache structs.
#ifndef NDEBUG
#define H5FCWFS_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#define H5HG_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#pragma push_macro("try")
#undef try
#define try h5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "third_party/hdf5/src/H5HGpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5FCWFS_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FCWFS_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

constexpr unsigned kMaxCwfs = 16;

struct SyntheticHeap {
  H5HG_heap_t heap{};
  H5HG_obj_t obj0{};

  void Init(haddr_t addr, size_t total_size, size_t free_size) {
    heap = {};
    obj0 = {};
    heap.addr = addr;
    heap.size = total_size;
    obj0.size = free_size;
    heap.obj = &obj0;
  }
};

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

H5F_t* GetFileStruct(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

void ClearCwfsEntries(H5F_t* f) {
  if (!f || !f->shared || !f->shared->cwfs) return;
  while (f->shared->ncwfs > 0) {
    H5F_cwfs_remove_heap(f->shared, f->shared->cwfs[0]);
  }
}

TEST(H5FcwfsTest, AddAllocatesPrependsAndEvictsRightmostSmallerWhenFull) {
  ScopedFile file = CreateInMemoryFile("cwfs_add.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFileStruct(file.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(f->shared, nullptr);
  EXPECT_EQ(f->shared->cwfs, nullptr);
  EXPECT_EQ(f->shared->ncwfs, 0u);

  std::array<SyntheticHeap, kMaxCwfs + 2> heaps{};
  absl::Cleanup cleanup = [&] { ClearCwfsEntries(f); };

  // Prepend 16 heaps so final slots [0..15] have h15 at index 0 and h0 at 15.
  // Give h5 (which lands at slot 10) a smaller free size than slots [11..15]
  // to verify that H5F_cwfs_add scans from right to left and evicts slot 10.
  for (unsigned i = 0; i < kMaxCwfs; ++i) {
    const size_t free_sz = (i == 5) ? 256u : 1000u + static_cast<size_t>(i) * 16u;
    heaps[i].Init(0x1000 + i * 0x100, 4096, free_sz);
    ASSERT_GE(H5F_cwfs_add(f, &heaps[i].heap), 0);
  }
  ASSERT_EQ(f->shared->ncwfs, kMaxCwfs);
  EXPECT_EQ(f->shared->cwfs[0], &heaps[15].heap);
  EXPECT_EQ(f->shared->cwfs[10], &heaps[5].heap);
  EXPECT_EQ(f->shared->cwfs[15], &heaps[0].heap);

  // Adding a heap with free space <= every entry leaves the list unchanged.
  heaps[16].Init(0x5000, 4096, 128);
  ASSERT_GE(H5F_cwfs_add(f, &heaps[16].heap), 0);
  EXPECT_EQ(f->shared->ncwfs, kMaxCwfs);
  EXPECT_EQ(f->shared->cwfs[0], &heaps[15].heap);
  EXPECT_EQ(f->shared->cwfs[10], &heaps[5].heap);

  // Adding a heap with 512 free bytes replaces slot 10 (&heaps[5], free 256),
  // shifts slots [0..9] to [1..10], and places heaps[17] at slot 0.
  heaps[17].Init(0x6000, 4096, 512);
  ASSERT_GE(H5F_cwfs_add(f, &heaps[17].heap), 0);
  EXPECT_EQ(f->shared->ncwfs, kMaxCwfs);
  EXPECT_EQ(f->shared->cwfs[0], &heaps[17].heap);
  EXPECT_EQ(f->shared->cwfs[1], &heaps[15].heap);
  EXPECT_EQ(f->shared->cwfs[10], &heaps[6].heap);
  EXPECT_EQ(f->shared->cwfs[11], &heaps[4].heap);
  EXPECT_EQ(f->shared->cwfs[15], &heaps[0].heap);
}

TEST(H5FcwfsTest, FindFreeHeapSearchesSwapsAndRespectsMaxSize) {
  ScopedFile file = CreateInMemoryFile("cwfs_find_synth.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFileStruct(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Empty CWFS list leaves addr unchanged.
  haddr_t addr = HADDR_UNDEF;
  EXPECT_GE(H5F_cwfs_find_free_heap(f, 64, &addr), 0);
  EXPECT_EQ(addr, HADDR_UNDEF);

  SyntheticHeap small_heap;
  SyntheticHeap large_heap;
  small_heap.Init(0x2000, 4096, 128);
  large_heap.Init(0x3000, 4096, 512);
  absl::Cleanup cleanup = [&] { ClearCwfsEntries(f); };

  ASSERT_GE(H5F_cwfs_add(f, &large_heap.heap), 0);
  ASSERT_GE(H5F_cwfs_add(f, &small_heap.heap), 0);
  ASSERT_EQ(f->shared->cwfs[0], &small_heap.heap);
  ASSERT_EQ(f->shared->cwfs[1], &large_heap.heap);

  // Hit at index 0 does not reorder the list.
  addr = HADDR_UNDEF;
  ASSERT_GE(H5F_cwfs_find_free_heap(f, 64, &addr), 0);
  EXPECT_EQ(addr, 0x2000u);
  EXPECT_EQ(f->shared->cwfs[0], &small_heap.heap);

  // Hit at index 1 swaps it forward into slot 0.
  addr = HADDR_UNDEF;
  ASSERT_GE(H5F_cwfs_find_free_heap(f, 256, &addr), 0);
  EXPECT_EQ(addr, 0x3000u);
  EXPECT_EQ(f->shared->cwfs[0], &large_heap.heap);
  EXPECT_EQ(f->shared->cwfs[1], &small_heap.heap);

  // Requesting more than H5HG_MAXSIZE skips extension and leaves addr unset.
  addr = HADDR_UNDEF;
  ASSERT_GE(H5F_cwfs_find_free_heap(f, H5HG_MAXSIZE + 1024u, &addr), 0);
  EXPECT_EQ(addr, HADDR_UNDEF);
}

TEST(H5FcwfsTest, FindFreeHeapExtendsRealCollectionsAtFrontAndSecondSlot) {
  ScopedFile file = CreateInMemoryFile("cwfs_extend_real.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFileStruct(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Create collection A (4096 bytes) at EOA with ~200 free bytes left.
  std::vector<uint8_t> data_small(3800, 0xAB);
  H5HG_t obj1{};
  ASSERT_GE(H5HG_insert(f, data_small.size(), data_small.data(), &obj1), 0);
  ASSERT_EQ(f->shared->ncwfs, 1u);

  // 2. Insert a 10 KiB object; H5F_cwfs_find_free_heap extends collection A at
  // EOA in place because size + new_need <= H5HG_MAXSIZE.
  std::vector<uint8_t> data_med(10000, 0xCD);
  H5HG_t obj2{};
  ASSERT_GE(H5HG_insert(f, data_med.size(), data_med.data(), &obj2), 0);
  EXPECT_EQ(obj2.addr, obj1.addr);

  // 3. Insert an object > H5HG_MAXSIZE (70 KiB) so collection A cannot extend
  // to fit it; a second collection B (> 65 KiB) is allocated at EOA, placing
  // collection A behind EOA (non-extensible) and CWFS as [B, A].
  std::vector<uint8_t> data_huge(70000, 0xEF);
  H5HG_t obj3{};
  ASSERT_GE(H5HG_insert(f, data_huge.size(), data_huge.data(), &obj3), 0);
  EXPECT_NE(obj3.addr, obj1.addr);

  // 4. Insert a 3 KiB object: B has no space and size > H5HG_MAXSIZE, A cannot
  // extend (not at EOA), so collection C (4096 bytes) is allocated at EOA.
  std::vector<uint8_t> data_tail(3000, 0x11);
  H5HG_t obj4{};
  ASSERT_GE(H5HG_insert(f, data_tail.size(), data_tail.data(), &obj4), 0);
  ASSERT_EQ(f->shared->ncwfs, 3u);

  // Promote B ahead of C by moving C to index 1 so cwfs[0] fails to extend
  // (size > H5HG_MAXSIZE) and cwfs[1] (collection C at EOA) extends and swaps
  // back to slot 0!
  H5HG_heap_t* heap_c = f->shared->cwfs[0];
  H5HG_heap_t* heap_b = f->shared->cwfs[1];
  ASSERT_GE(H5F_cwfs_advance_heap(f, heap_b, false), 0);
  ASSERT_EQ(f->shared->cwfs[0], heap_b);
  ASSERT_EQ(f->shared->cwfs[1], heap_c);

  // Move non-EOA heap_a ahead of heap_c as well to exercise was_extended ==
  // false on an eligible-sized heap before reaching heap_c at index 2.
  H5HG_heap_t* heap_a = f->shared->cwfs[2];
  ASSERT_GE(H5F_cwfs_advance_heap(f, heap_a, false), 0);
  ASSERT_GE(H5F_cwfs_advance_heap(f, heap_a, false), 0);
  ASSERT_EQ(f->shared->cwfs[0], heap_a);
  ASSERT_EQ(f->shared->cwfs[1], heap_b);
  ASSERT_EQ(f->shared->cwfs[2], heap_c);

  haddr_t extended_addr = HADDR_UNDEF;
  ASSERT_GE(H5F_cwfs_find_free_heap(f, 8000, &extended_addr), 0);
  EXPECT_EQ(extended_addr, H5HG_get_addr(heap_c));
  // Found at index 2, so heap_c swaps one step forward to index 1.
  EXPECT_EQ(f->shared->cwfs[1], heap_c);
  EXPECT_EQ(f->shared->cwfs[2], heap_b);

  // Verify stored global heap objects remain readable after extension.
  size_t read_size = 0;
  std::vector<uint8_t> read_back(data_med.size(), 0);
  ASSERT_NE(H5HG_read(f, &obj2, read_back.data(), &read_size), nullptr);
  EXPECT_EQ(read_size, data_med.size());
  EXPECT_EQ(read_back, data_med);
}

TEST(H5FcwfsTest, AdvanceAndRemoveHeapHandleAllPositions) {
  ScopedFile file = CreateInMemoryFile("cwfs_adv_rm.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFileStruct(file.get());
  ASSERT_NE(f, nullptr);

  std::array<SyntheticHeap, kMaxCwfs + 1> heaps{};
  for (unsigned i = 0; i < heaps.size(); ++i) {
    heaps[i].Init(0x8000 + i * 0x100, 4096, 1024);
  }
  absl::Cleanup cleanup = [&] { ClearCwfsEntries(f); };

  // Initialize CWFS array with heaps[0] and heaps[1] so cwfs is allocated
  // (order: [h1, h0]).
  ASSERT_GE(H5F_cwfs_add(f, &heaps[0].heap), 0);
  ASSERT_GE(H5F_cwfs_add(f, &heaps[1].heap), 0);
  ASSERT_EQ(f->shared->cwfs[0], &heaps[1].heap);
  ASSERT_EQ(f->shared->cwfs[1], &heaps[0].heap);

  // Advance slot 0: stays at slot 0.
  ASSERT_GE(H5F_cwfs_advance_heap(f, &heaps[1].heap, false), 0);
  EXPECT_EQ(f->shared->cwfs[0], &heaps[1].heap);

  // Advance slot 1: swaps with slot 0 -> [h0, h1].
  ASSERT_GE(H5F_cwfs_advance_heap(f, &heaps[0].heap, false), 0);
  EXPECT_EQ(f->shared->cwfs[0], &heaps[0].heap);
  EXPECT_EQ(f->shared->cwfs[1], &heaps[1].heap);

  // Advance absent heap with add_heap == false: ignored.
  ASSERT_GE(H5F_cwfs_advance_heap(f, &heaps[2].heap, false), 0);
  EXPECT_EQ(f->shared->ncwfs, 2u);

  // Advance absent heaps with add_heap == true up to capacity 16.
  for (unsigned i = 2; i < kMaxCwfs; ++i) {
    ASSERT_GE(H5F_cwfs_advance_heap(f, &heaps[i].heap, true), 0);
    EXPECT_EQ(f->shared->ncwfs, i + 1);
    EXPECT_EQ(f->shared->cwfs[i], &heaps[i].heap);
  }

  // Advancing an absent heap with add_heap == true when full (ncwfs == 16)
  // overwrites the last slot (index 15).
  ASSERT_GE(H5F_cwfs_advance_heap(f, &heaps[16].heap, true), 0);
  EXPECT_EQ(f->shared->ncwfs, kMaxCwfs);
  EXPECT_EQ(f->shared->cwfs[15], &heaps[16].heap);

  // Removing an absent heap is a safe no-op.
  ASSERT_GE(H5F_cwfs_remove_heap(f->shared, &heaps[15].heap), 0);
  EXPECT_EQ(f->shared->ncwfs, kMaxCwfs);

  // Remove middle, first, and last entries cleanly.
  ASSERT_GE(H5F_cwfs_remove_heap(f->shared, &heaps[5].heap), 0);
  EXPECT_EQ(f->shared->ncwfs, 15u);
  ASSERT_GE(H5F_cwfs_remove_heap(f->shared, &heaps[0].heap), 0);
  EXPECT_EQ(f->shared->ncwfs, 14u);
  EXPECT_EQ(f->shared->cwfs[0], &heaps[1].heap);
  ASSERT_GE(H5F_cwfs_remove_heap(f->shared, &heaps[16].heap), 0);
  EXPECT_EQ(f->shared->ncwfs, 13u);

  ClearCwfsEntries(f);
  EXPECT_EQ(f->shared->ncwfs, 0u);
}

}  // namespace
}  // namespace hdf5
