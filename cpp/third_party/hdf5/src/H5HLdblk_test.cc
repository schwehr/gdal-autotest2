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
// Tests internal local heap data block creation, destruction, and reallocation
// routines (third_party/hdf5/src/H5HLdblk.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HLdblk.c

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
#define H5HLDBLK_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try h5_try_param
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
#ifdef H5HLDBLK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HLDBLK_TEST_RESTORE_UNDEF_NDEBUG
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

// Reallocates both the on-disk datablock via H5HL__dblk_realloc and the
// companion in-memory image + single freelist node so subsequent cache flushes
// always read a consistent image.
herr_t ReallocHeapAndImage(H5F_t* f, H5HL_t* heap, size_t new_size) {
  const size_t old_size = heap->dblk_size;
  if (H5HL__dblk_realloc(f, heap, new_size) < 0) {
    return -1;
  }
  if (new_size > old_size) {
    uint8_t* grown = H5FL_BLK_REALLOC(lheap_chunk, heap->dblk_image, new_size);
    if (grown == nullptr) {
      return -1;
    }
    std::memset(grown + old_size, 0, new_size - old_size);
    heap->dblk_image = grown;
  }
  if (heap->freelist != nullptr) {
    heap->freelist->offset = 0;
    heap->freelist->size = new_size;
  }
  return 0;
}

TEST(H5HLdblkTest, DblkNewAndDestLifecycleAndUnlinkedBranch) {
  ASSERT_GE(H5open(), 0);

  // 1. Sole reference: H5HL__dblk_dest decrements rc to 0 and frees heap.
  {
    H5HL_t* heap = H5HL__new(/*sizeof_size=*/8, /*sizeof_addr=*/8,
                             /*prfx_size=*/32);
    ASSERT_NE(heap, nullptr);
    H5HL_dblk_t* dblk = H5HL__dblk_new(heap);
    ASSERT_NE(dblk, nullptr);
    EXPECT_EQ(dblk->heap, heap);
    EXPECT_EQ(heap->dblk, dblk);
    EXPECT_EQ(heap->rc, 1u);
    EXPECT_GE(H5HL__dblk_dest(dblk), 0);
  }

  // 2. Extra reference keeps heap alive after dblk destruction.
  {
    H5HL_t* heap = H5HL__new(/*sizeof_size=*/8, /*sizeof_addr=*/8,
                             /*prfx_size=*/32);
    ASSERT_NE(heap, nullptr);
    ASSERT_GE(H5HL__inc_rc(heap), 0);
    absl::Cleanup dec_heap = [&] {
      if (heap != nullptr) {
        H5HL__dec_rc(heap);
      }
    };

    H5HL_dblk_t* dblk = H5HL__dblk_new(heap);
    ASSERT_NE(dblk, nullptr);
    EXPECT_EQ(heap->rc, 2u);
    ASSERT_GE(H5HL__dblk_dest(dblk), 0);
    EXPECT_EQ(heap->dblk, nullptr);
    EXPECT_EQ(heap->rc, 1u);

    ASSERT_GE(H5HL__dec_rc(heap), 0);
    heap = nullptr;
  }

  // 3. Unlinked dblk (`dblk->heap == nullptr`) frees cleanly.
  {
    H5HL_t* heap = H5HL__new(/*sizeof_size=*/8, /*sizeof_addr=*/8,
                             /*prfx_size=*/32);
    ASSERT_NE(heap, nullptr);
    ASSERT_GE(H5HL__inc_rc(heap), 0);
    absl::Cleanup dec_heap = [&] {
      if (heap != nullptr) {
        H5HL__dec_rc(heap);
      }
    };

    H5HL_dblk_t* dblk = H5HL__dblk_new(heap);
    ASSERT_NE(dblk, nullptr);
    heap->dblk = nullptr;
    dblk->heap = nullptr;
    ASSERT_GE(H5HL__dec_rc(heap), 0);
    EXPECT_GE(H5HL__dblk_dest(dblk), 0);

    ASSERT_GE(H5HL__dec_rc(heap), 0);
    heap = nullptr;
  }
}

TEST(H5HLdblkTest, ReallocSameAddressAndSplitMoveBranches) {
  ScopedFile file = CreateInMemoryFile("h5hldblk_realloc.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  haddr_t addr1 = HADDR_UNDEF;
  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    // 1. Create heap1 at EOA (`single_cache_obj == true`) and shrink/realloc in
    // place so H5MF_xfree + H5MF_alloc reuse the exact same `old_addr`.
    ASSERT_GE(H5HL_create(f, /*size_hint=*/128, &addr1), 0);
    H5HL_t* heap1 = H5HL_protect(f, addr1, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap1, nullptr);
    absl::Cleanup unprot_heap1 = [&] {
      if (heap1 != nullptr) {
        H5HL_unprotect(heap1);
      }
    };
    ASSERT_TRUE(heap1->single_cache_obj);
    const haddr_t orig_dblk_addr = heap1->dblk_addr;
    ASSERT_GE(ReallocHeapAndImage(f, heap1, 64), 0);
    EXPECT_EQ(heap1->dblk_addr, orig_dblk_addr);
    EXPECT_TRUE(heap1->single_cache_obj);
    EXPECT_EQ(heap1->dblk_size, 64u);

    // 2. Allocate heap2 immediately after heap1 so heap1's dblk is no longer at
    // EOA. Reallocating heap1 moves dblk (`old_addr != new_addr`) and detaches
    // it from the prefix (`single_cache_obj == false`).
    haddr_t addr2 = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/128, &addr2), 0);
    ASSERT_GE(ReallocHeapAndImage(f, heap1, 256), 0);
    EXPECT_NE(heap1->dblk_addr, orig_dblk_addr);
    EXPECT_FALSE(heap1->single_cache_obj);
    EXPECT_NE(heap1->dblk, nullptr);
    EXPECT_EQ(heap1->dblk_size, 256u);

    // 3. Now heap1's detached dblk sits at EOA (`single_cache_obj == false`).
    // Shrinking returns space at the exact same address (`old_addr ==
    // new_addr` with `!single_cache_obj`).
    const haddr_t detached_addr = heap1->dblk_addr;
    ASSERT_GE(ReallocHeapAndImage(f, heap1, 128), 0);
    EXPECT_EQ(heap1->dblk_addr, detached_addr);
    EXPECT_FALSE(heap1->single_cache_obj);
    EXPECT_EQ(heap1->dblk_size, 128u);

    // 4. Allocate heap3 after detached_addr so heap1's detached dblk moves
    // again (`old_addr != new_addr` with `!single_cache_obj` ->
    // H5AC_move_entry).
    haddr_t addr3 = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/128, &addr3), 0);
    ASSERT_GE(ReallocHeapAndImage(f, heap1, 512), 0);
    EXPECT_NE(heap1->dblk_addr, detached_addr);
    EXPECT_FALSE(heap1->single_cache_obj);
    EXPECT_EQ(heap1->dblk_size, 512u);

    // Resize dblk_image to match final 512 size is already done; also mark
    // heap dirty by inserting an item before unprotecting.
    size_t item_off = 0;
    constexpr char kItem[] = "detached_dblk_item";
    ASSERT_GE(H5HL_insert(f, heap1, sizeof(kItem), kItem, &item_off), 0);
    ASSERT_GE(H5HL_unprotect(heap1), 0);
    heap1 = nullptr;
  }

  // Flush and reopen from file image to verify persisted split heap metadata.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
            image_size);
  file.reset();

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(
      H5Fopen("h5hldblk_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(reopened.valid());
  H5F_t* rf = GetH5File(reopened.get());
  ASSERT_NE(rf, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  size_t queried_size = 0;
  ASSERT_GE(H5HL_get_size(rf, addr1, &queried_size), 0);
  EXPECT_EQ(queried_size, 512u);
}

TEST(H5HLdblkTest, ReallocErrorPathsRestoreOldAddressAndSize) {
  // 1. H5MF_xfree failure when dblk_addr points into temporary file space.
  {
    ScopedFile file = CreateInMemoryFile("h5hldblk_err_xfree.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/64, &addr), 0);
    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot_heap = [&] { H5HL_unprotect(heap); };

    const haddr_t orig_addr = heap->dblk_addr;
    const size_t orig_size = heap->dblk_size;
    const haddr_t tmp_addr = H5MF_alloc_tmp(f, orig_size);
    ASSERT_TRUE(H5_addr_defined(tmp_addr));

    {
      ScopedErrorSilence silence;
      heap->dblk_addr = tmp_addr;
      EXPECT_LT(H5HL__dblk_realloc(f, heap, 256), 0);
      EXPECT_EQ(heap->dblk_addr, tmp_addr);
      EXPECT_EQ(heap->dblk_size, orig_size);
      heap->dblk_addr = orig_addr;
    }
  }

  // 2. H5MF_alloc failure when requesting allocation crossing into
  // temporary file space (`f->shared->tmp_addr`), after flushing so the clean
  // heap entry is not rewritten at file close.
  {
    ScopedFile alloc_file = CreateInMemoryFile("h5hldblk_err_alloc.h5");
    ASSERT_TRUE(alloc_file.valid());
    H5F_t* sf = GetH5File(alloc_file.get());
    ASSERT_NE(sf, nullptr);

    haddr_t addr = HADDR_UNDEF;
    {
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      ASSERT_GE(H5HL_create(sf, /*size_hint=*/64, &addr), 0);
    }
    ASSERT_GE(H5Fflush(alloc_file.get(), H5F_SCOPE_GLOBAL), 0);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5HL_t* heap = H5HL_protect(sf, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprot_heap = [&] { H5HL_unprotect(heap); };

    const haddr_t orig_addr = heap->dblk_addr;
    const size_t orig_size = heap->dblk_size;
    const haddr_t eoa = H5F_get_eoa(sf, H5FD_MEM_LHEAP);
    ASSERT_TRUE(H5_addr_defined(eoa));
    const haddr_t orig_tmp_addr = sf->shared->tmp_addr;
    absl::Cleanup restore_tmp_addr = [&] {
      sf->shared->tmp_addr = orig_tmp_addr;
    };
    sf->shared->tmp_addr = eoa;

    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5HL__dblk_realloc(sf, heap, 256), 0);
      EXPECT_EQ(heap->dblk_addr, orig_addr);
      EXPECT_EQ(heap->dblk_size, orig_size);
    }
  }

  // 3. Cache resize and move failure branches in H5HL__dblk_realloc:
  //    - old_addr == new_addr with single_cache_obj (resize prfx fails)
  //    - old_addr != new_addr with single_cache_obj (resize prfx fails)
  //    - old_addr == new_addr with !single_cache_obj (resize dblk fails)
  //    - old_addr != new_addr with !single_cache_obj (resize dblk & move fail)
  {
    ScopedFile cfile = CreateInMemoryFile("h5hldblk_err_cache.h5");
    ASSERT_TRUE(cfile.valid());
    H5F_t* cf = GetH5File(cfile.get());
    ASSERT_NE(cf, nullptr);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    // (a) old_addr == new_addr, single_cache_obj == true: unprotect clean
    // prfx while holding rc so H5C_resize_entry rejects unpinned/unprotected.
    haddr_t s_addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(cf, /*size_hint=*/128, &s_addr), 0);
    H5HL_t* s_heap = H5HL_protect(cf, s_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(s_heap, nullptr);
    ASSERT_GE(H5HL__inc_rc(s_heap), 0);
    ASSERT_GE(H5HL_unprotect(s_heap), 0);
    {
      absl::Cleanup dec_s = [&] { H5HL__dec_rc(s_heap); };
      ScopedErrorSilence silence;
      const haddr_t orig_addr = s_heap->dblk_addr;
      EXPECT_LT(H5HL__dblk_realloc(cf, s_heap, 128), 0);
      EXPECT_EQ(s_heap->dblk_addr, orig_addr);
      EXPECT_EQ(s_heap->dblk_size, 128u);
    }

    // (b) Block s_heap with a blocker so old_addr != new_addr, still
    // single_cache_obj == true: resize prfx fails after H5HL__dblk_new.
    haddr_t blocker_s = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(cf, /*size_hint=*/32, &blocker_s), 0);
    s_heap = H5HL_protect(cf, s_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(s_heap, nullptr);
    ASSERT_GE(H5HL__inc_rc(s_heap), 0);
    ASSERT_GE(H5HL_unprotect(s_heap), 0);
    {
      absl::Cleanup dec_s = [&] {
        if (s_heap->dblk != nullptr) {
          H5HL__dblk_dest(s_heap->dblk);
        }
        H5HL__dec_rc(s_heap);
      };
      ScopedErrorSilence silence;
      const haddr_t orig_addr = s_heap->dblk_addr;
      EXPECT_LT(H5HL__dblk_realloc(cf, s_heap, 256), 0);
      EXPECT_EQ(s_heap->dblk_addr, orig_addr);
      EXPECT_EQ(s_heap->dblk_size, 128u);
      EXPECT_TRUE(s_heap->single_cache_obj);
    }

    // (c) Create split heap whose detached dblk sits at EOA
    // (old_addr == new_addr, !single_cache_obj): unpin/unprotect dblk with rc
    // held so H5AC_resize_entry(heap->dblk) fails.
    haddr_t split_addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(cf, /*size_hint=*/64, &split_addr), 0);
    haddr_t split_blk = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(cf, /*size_hint=*/32, &split_blk), 0);
    H5HL_t* d_heap = H5HL_protect(cf, split_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(d_heap, nullptr);
    ASSERT_GE(ReallocHeapAndImage(cf, d_heap, 256), 0);
    ASSERT_FALSE(d_heap->single_cache_obj);
    ASSERT_GE(H5HL__inc_rc(d_heap), 0);
    ASSERT_GE(H5HL_unprotect(d_heap), 0);
    {
      absl::Cleanup dec_d = [&] { H5HL__dec_rc(d_heap); };
      ScopedErrorSilence silence;
      const haddr_t orig_addr = d_heap->dblk_addr;
      EXPECT_LT(H5HL__dblk_realloc(cf, d_heap, 256), 0);
      EXPECT_EQ(d_heap->dblk_addr, orig_addr);
      EXPECT_EQ(d_heap->dblk_size, 256u);
    }

    // (d) Block d_heap with a large heap (size_hint=256 > any prior hole)
    // so it allocates at EOA, old_addr != new_addr, !single_cache_obj, and
    // resize dblk fails when unpinned/unprotected.
    haddr_t blocker_d = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(cf, /*size_hint=*/256, &blocker_d), 0);
    d_heap = H5HL_protect(cf, split_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(d_heap, nullptr);
    ASSERT_GE(H5HL__inc_rc(d_heap), 0);
    ASSERT_GE(H5HL_unprotect(d_heap), 0);
    {
      absl::Cleanup dec_d = [&] { H5HL__dec_rc(d_heap); };
      ScopedErrorSilence silence;
      const haddr_t orig_addr = d_heap->dblk_addr;
      EXPECT_LT(H5HL__dblk_realloc(cf, d_heap, 512), 0);
      EXPECT_EQ(d_heap->dblk_addr, orig_addr);
      EXPECT_EQ(d_heap->dblk_size, 256u);
    }
  }

  // (e) On a fresh blocked split heap with dblk protected, temporarily mark
  // dblk read-only in cache so H5AC_resize_entry succeeds and H5AC_move_entry
  // fails, then restore the read-only flag and original cache entry size.
  {
    ScopedFile e_file = CreateInMemoryFile("h5hldblk_err_move.h5");
    ASSERT_TRUE(e_file.valid());
    H5F_t* const ef = GetH5File(e_file.get());
    ASSERT_NE(ef, nullptr);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    haddr_t e_addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(ef, /*size_hint=*/64, &e_addr), 0);
    haddr_t e_blk1 = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(ef, /*size_hint=*/32, &e_blk1), 0);
    H5HL_t* e_heap = H5HL_protect(ef, e_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(e_heap, nullptr);
    absl::Cleanup unprot_e = [&] { H5HL_unprotect(e_heap); };
    ASSERT_GE(ReallocHeapAndImage(ef, e_heap, 256), 0);
    ASSERT_FALSE(e_heap->single_cache_obj);
    // Use size_hint=128 (> initial 64-byte hole) so e_blk2 allocates at EOA
    // immediately after e_heap->dblk, forcing H5HL__dblk_realloc(512) to move.
    haddr_t e_blk2 = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(ef, /*size_hint=*/128, &e_blk2), 0);
    {
      ScopedErrorSilence silence;
      const haddr_t orig_addr = e_heap->dblk_addr;
      const size_t orig_size = e_heap->dblk_size;
      e_heap->dblk->cache_info.is_read_only = true;
      absl::Cleanup restore_ro = [&] {
        e_heap->dblk->cache_info.is_read_only = false;
        H5AC_resize_entry(e_heap->dblk, orig_size);
      };
      EXPECT_LT(H5HL__dblk_realloc(ef, e_heap, 512), 0);
      EXPECT_EQ(e_heap->dblk_addr, orig_addr);
      EXPECT_EQ(e_heap->dblk_size, orig_size);
    }
  }
}

}  // namespace
}  // namespace hdf5
