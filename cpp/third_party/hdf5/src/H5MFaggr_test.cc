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
// Tests metadata and small-raw-data block aggregation allocation, EOA extension,
// section/aggregator absorption, queries, reset/free, and EOA shrinking
// (third_party/hdf5/src/H5MFaggr.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5MFaggr.c

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"
#include "third_party/hdf5/src/H5FDdevelop.h"

// `:hdf5` is compiled with -DNDEBUG, omitting `#ifndef NDEBUG` fields from
// H5C_cache_entry_t (`H5AC_info_t`). Define NDEBUG around both the prelude and
// internal headers so struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5MFAGGR_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#define H5MF_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDprivate.h"
#pragma push_macro("try")
#undef try
#define try h5f_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5MFpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5MFAGGR_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5MFAGGR_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

// Pushes an HDF5 API context and freespace metadata tag for direct internal calls.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dxpl_id = H5P_DEFAULT,
                            haddr_t tag = H5AC__FREESPACE_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (dxpl_id != H5P_DEFAULT) {
        H5CX_set_dxpl(dxpl_id);
      }
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

// RAII owner for H5MF_free_section_t* created via H5MF__sect_new.
class ScopedSection {
 public:
  explicit ScopedSection(H5MF_free_section_t* sect = nullptr) : sect_(sect) {}
  ~ScopedSection() { reset(); }

  ScopedSection(const ScopedSection&) = delete;
  ScopedSection& operator=(const ScopedSection&) = delete;

  void reset(H5MF_free_section_t* next = nullptr) {
    if (sect_ != nullptr) {
      H5MF__sect_free(&sect_->sect_info);
    }
    sect_ = next;
  }

  H5MF_free_section_t* get() const { return sect_; }

 private:
  H5MF_free_section_t* sect_ = nullptr;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

ScopedFile CreateCoreFile(H5F_fspace_strategy_t strategy, bool persist,
                          hsize_t threshold, hsize_t alignment,
                          hsize_t meta_block_size, hsize_t sdata_block_size) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (!fcpl.valid()) return ScopedFile();
  if (H5Pset_file_space_strategy(fcpl.get(), strategy, persist, 1) < 0) {
    return ScopedFile();
  }

  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) return ScopedFile();
  if (H5Pset_alignment(fapl.get(), threshold, alignment) < 0) {
    return ScopedFile();
  }
  if (H5Pset_meta_block_size(fapl.get(), meta_block_size) < 0) {
    return ScopedFile();
  }
  if (H5Pset_small_data_block_size(fapl.get(), sdata_block_size) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate("aggr_test.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
}

TEST(H5MFaggrTest, VfdAllocDispatchesMetaAndRawAndExtendsAtEoa) {
  ScopedFile file = CreateCoreFile(H5F_FSPACE_STRATEGY_FSM_AGGR,
                                   /*persist=*/false, /*threshold=*/1,
                                   /*alignment=*/1, /*meta_block_size=*/512,
                                   /*sdata_block_size=*/512);
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Query both aggregators (including nullptr outputs).
  haddr_t meta_addr = HADDR_UNDEF;
  hsize_t meta_size = 0;
  ASSERT_GE(H5MF__aggr_query(f, &f->shared->meta_aggr, &meta_addr, &meta_size), 0);
  EXPECT_NE(meta_addr, HADDR_UNDEF);
  EXPECT_GT(meta_size, 0u);
  EXPECT_GE(H5MF__aggr_query(f, &f->shared->sdata_aggr, nullptr, nullptr), 0);

  // Suballocate metadata inside existing meta_aggr.
  const haddr_t meta1 = H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 32);
  EXPECT_EQ(meta1, meta_addr);
  EXPECT_EQ(f->shared->meta_aggr.addr, meta_addr + 32);
  EXPECT_EQ(f->shared->meta_aggr.size, meta_size - 32);

  // Allocate raw (DRAW) and global heap (GHEAP) from empty sdata_aggr at EOA.
  const haddr_t draw1 = H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 64);
  EXPECT_NE(draw1, HADDR_UNDEF);
  EXPECT_EQ(f->shared->sdata_aggr.size, 512u - 64u);

  const haddr_t gheap1 = H5MF_aggr_vfd_alloc(f, H5FD_MEM_GHEAP, 64);
  EXPECT_EQ(gheap1, draw1 + 64);
  EXPECT_EQ(f->shared->sdata_aggr.size, 512u - 128u);

  // Small-enough-for-aggr request (> remaining sdata_aggr->size, < alloc_size)
  // extends sdata_aggr at EOA via H5F__try_extend.
  const haddr_t sdata_cur = f->shared->sdata_aggr.addr;
  const haddr_t draw2 = H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 450);
  EXPECT_EQ(draw2, sdata_cur);
  EXPECT_EQ(f->shared->sdata_aggr.tot_size, 1024u);

  // Large request (>= alloc_size) when sdata_aggr is at EOA extends sdata_aggr
  // in place without shrinking sdata_aggr->size.
  const hsize_t size_before_large = f->shared->sdata_aggr.size;
  const haddr_t draw3 = H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 600);
  EXPECT_NE(draw3, HADDR_UNDEF);
  EXPECT_EQ(f->shared->sdata_aggr.size, size_before_large);
}

TEST(H5MFaggrTest, FoldsEoaFragmentWhenUnalignedAndFreesOtherAggr) {
  ScopedFile file = CreateCoreFile(H5F_FSPACE_STRATEGY_FSM_AGGR,
                                   /*persist=*/false, /*threshold=*/128,
                                   /*alignment=*/64, /*meta_block_size=*/512,
                                   /*sdata_block_size=*/512);
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Drain meta_aggr down to <= 16 bytes using small (< threshold) chunks so no
  // alignment overflow triggers an extension.
  while (f->shared->meta_aggr.size > 16) {
    const hsize_t step = f->shared->meta_aggr.size - 16;
    const hsize_t chunk = step < 64 ? step : 64;
    ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, chunk), HADDR_UNDEF);
  }
  ASSERT_LE(f->shared->meta_aggr.size, 16u);

  // Allocate large raw block >= threshold & >= alloc_size with non-multiple size
  // so EOA becomes unaligned (mod 64 != 0) and meta_aggr is no longer at EOA.
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 529), HADDR_UNDEF);
  const haddr_t eoa_unaligned = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
  ASSERT_NE(eoa_unaligned, HADDR_UNDEF);
  ASSERT_NE(eoa_unaligned % 64, 0u);
  ASSERT_NE(f->shared->meta_aggr.addr + f->shared->meta_aggr.size, eoa_unaligned);

  // Requesting 32 bytes (< threshold 128 => alignment == 0 in H5MF__aggr_alloc,
  // while H5F__alloc allocates alloc_size == 512 >= 128) generates an EOA
  // alignment fragment that folds directly into the new meta_aggr block.
  const haddr_t meta_folded = H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 32);
  EXPECT_EQ(meta_folded, eoa_unaligned);
  EXPECT_GT(f->shared->meta_aggr.tot_size, 512u);

  // Now set up sdata_aggr at EOA with multiple blocks (tot_size - size >=
  // alloc_size) and an unaligned sdata_aggr.addr so freeing sdata_aggr exposes
  // an unaligned EOA for both size < alloc_size and size >= alloc_size paths.
  auto setup_overfull_sdata_at_eoa = [&]() {
    ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 64), HADDR_UNDEF);
    // Force sdata_aggr to extend at EOA with a request > remaining size (< 512).
    const hsize_t req1 = f->shared->sdata_aggr.size + 16;
    ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, req1), HADDR_UNDEF);
    // Advance sdata_aggr.addr off the 64-byte boundary while keeping
    // (tot_size - size) >= alloc_size and keeping sdata_aggr at EOA.
    ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 17), HADDR_UNDEF);
    const haddr_t cur_eoa = H5F_get_eoa(f, H5FD_MEM_DRAW);
    ASSERT_EQ(f->shared->sdata_aggr.addr + f->shared->sdata_aggr.size, cur_eoa);
    ASSERT_NE(f->shared->sdata_aggr.addr % 64, 0u);
    ASSERT_GE(f->shared->sdata_aggr.tot_size - f->shared->sdata_aggr.size,
              f->shared->sdata_aggr.alloc_size);
  };

  // Case 1: size < meta_aggr.alloc_size triggers freeing sdata_aggr at EOA
  // and leaves an aligned block plus an EOA fragment freed via H5MF_xfree.
  setup_overfull_sdata_at_eoa();
  while (f->shared->meta_aggr.size > 16) {
    const hsize_t step = f->shared->meta_aggr.size - 16;
    const hsize_t chunk = step < 64 ? step : 64;
    ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, chunk), HADDR_UNDEF);
  }
  const haddr_t aligned_meta = H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 200);
  EXPECT_EQ(aligned_meta % 64, 0u);
  EXPECT_EQ(f->shared->sdata_aggr.size, 0u);

  // Case 2: size >= meta_aggr.alloc_size also releases sdata_aggr at EOA and
  // frees the resulting EOA alignment fragment.
  setup_overfull_sdata_at_eoa();
  const haddr_t large_aligned_meta = H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 600);
  EXPECT_EQ(large_aligned_meta % 64, 0u);
  EXPECT_EQ(f->shared->sdata_aggr.size, 0u);
}

TEST(H5MFaggrTest, AlignmentFragmentInSuballocAndEoaExtension) {
  ScopedFile file = CreateCoreFile(H5F_FSPACE_STRATEGY_FSM_AGGR,
                                   /*persist=*/false, /*threshold=*/64,
                                   /*alignment=*/256, /*meta_block_size=*/512,
                                   /*sdata_block_size=*/512);
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Allocate aligned sdata block then misalign sdata_aggr->addr by 16 bytes.
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 64), HADDR_UNDEF);
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 16), HADDR_UNDEF);
  ASSERT_NE(f->shared->sdata_aggr.addr % 256, 0u);

  // Suballocation inside existing sdata_aggr with aggr_frag_size > 0
  // (128 + 240 = 368 <= 432 remaining).
  const haddr_t sub_aligned = H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 128);
  EXPECT_EQ(sub_aligned % 256, 0u);

  // Reset sdata_aggr onto a fresh aligned 512-byte block, then consume 200
  // (>= threshold) + 60 (< threshold) bytes so addr % 256 == 4 (aggr_frag_size
  // == 252 > ext_size - 350 == 162) and remaining size == 252.
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 600), HADDR_UNDEF);
  while (f->shared->sdata_aggr.size > 0) {
    const hsize_t chunk = f->shared->sdata_aggr.size < 32
                              ? f->shared->sdata_aggr.size
                              : 32;
    ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, chunk), HADDR_UNDEF);
  }
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 200), HADDR_UNDEF);
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 60), HADDR_UNDEF);
  ASSERT_EQ(f->shared->sdata_aggr.addr % 256, 4u);
  ASSERT_LT(f->shared->sdata_aggr.size, 350u);
  const haddr_t ext_small_aligned = H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 350);
  EXPECT_EQ(ext_small_aligned % 256, 0u);

  // Misalign sdata_aggr again, then allocate size >= alloc_size at EOA with
  // aggr_frag_size > 0.
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 17), HADDR_UNDEF);
  ASSERT_NE(f->shared->sdata_aggr.addr % 256, 0u);
  const haddr_t ext_large_aligned = H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 600);
  EXPECT_EQ(ext_large_aligned % 256, 0u);
}

TEST(H5MFaggrTest, TmpAddrOverlapErrorsAndInactiveStrategyBypass) {
  // 1) tmp_addr overlap checks in both size >= alloc_size and size < alloc_size.
  {
    ScopedFile file = CreateCoreFile(H5F_FSPACE_STRATEGY_FSM_AGGR,
                                     /*persist=*/true, /*threshold=*/64,
                                     /*alignment=*/64, /*meta_block_size=*/512,
                                     /*sdata_block_size=*/512);
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    const haddr_t saved_tmp = f->shared->tmp_addr;
    absl::Cleanup restore_tmp = [&] { f->shared->tmp_addr = saved_tmp; };

    {
      // Drain meta_aggr below 64 bytes so a request < alloc_size (512)
      // overflows it and reaches line 269.
      while (f->shared->meta_aggr.size > 64) {
        const hsize_t step = f->shared->meta_aggr.size - 64;
        const hsize_t chunk = step < 32 ? step : 32;
        ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, chunk), HADDR_UNDEF);
      }
      ScopedErrorSilence silence;
      f->shared->tmp_addr =
          f->shared->meta_aggr.addr + f->shared->meta_aggr.size + 8;
      // Case size >= alloc_size:
      EXPECT_EQ(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 600), HADDR_UNDEF);
      // Case size < alloc_size and (size + aggr_frag_size) > aggr->size:
      const hsize_t too_big_for_leftover = f->shared->meta_aggr.size + 32;
      ASSERT_LT(too_big_for_leftover, f->shared->meta_aggr.alloc_size);
      f->shared->tmp_addr =
          f->shared->meta_aggr.addr + f->shared->meta_aggr.size + 16;
      EXPECT_EQ(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, too_big_for_leftover),
                HADDR_UNDEF);
      // Trigger H5MF_aggr_vfd_alloc(DRAW) error return cleanly too:
      ASSERT_EQ(f->shared->sdata_aggr.size, 0u);
      f->shared->tmp_addr = f->shared->sdata_aggr.addr + 16;
      EXPECT_EQ(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 100), HADDR_UNDEF);
    }
    f->shared->tmp_addr = saved_tmp;

    // Closing + persistent FSM bypasses aggregator into direct H5F__alloc.
    const bool saved_closing = f->shared->closing;
    f->shared->closing = true;
    absl::Cleanup restore_closing = [&] { f->shared->closing = saved_closing; };
    const haddr_t direct_closing = H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 64);
    EXPECT_NE(direct_closing, HADDR_UNDEF);
  }

  // 2) H5F_FSPACE_STRATEGY_NONE bypasses aggregators and frees EOA fragments.
  {
    ScopedFile file = CreateCoreFile(H5F_FSPACE_STRATEGY_NONE,
                                     /*persist=*/false, /*threshold=*/64,
                                     /*alignment=*/64, /*meta_block_size=*/512,
                                     /*sdata_block_size=*/512);
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    const haddr_t odd = H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 17);
    EXPECT_NE(odd, HADDR_UNDEF);
    const haddr_t aligned = H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 128);
    EXPECT_NE(aligned, HADDR_UNDEF);
    EXPECT_EQ(aligned % 64, 0u);
    EXPECT_EQ(f->shared->meta_aggr.size, 0u);
    EXPECT_EQ(f->shared->sdata_aggr.size, 0u);
  }
}

TEST(H5MFaggrTest, TryExtendEoaAndNonEoaAndInactive) {
  ScopedFile file = CreateCoreFile(H5F_FSPACE_STRATEGY_FSM_AGGR,
                                   /*persist=*/false, /*threshold=*/1,
                                   /*alignment=*/1, /*meta_block_size=*/1000,
                                   /*sdata_block_size=*/512);
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Make sure meta_aggr is at EOA with >= 500 bytes remaining.
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 64), HADDR_UNDEF);
  ASSERT_GE(f->shared->meta_aggr.size, 500u);
  const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
  ASSERT_EQ(f->shared->meta_aggr.addr + f->shared->meta_aggr.size, eoa);

  // Non-adjoining blk_end fails to extend.
  EXPECT_EQ(H5MF__aggr_try_extend(f, &f->shared->meta_aggr, H5FD_MEM_DEFAULT,
                                  f->shared->meta_aggr.addr - 1, 16),
            0);

  // Below 10% threshold extends directly into aggregator without growing tot_size.
  const hsize_t tot_before = f->shared->meta_aggr.tot_size;
  const hsize_t size_before = f->shared->meta_aggr.size;
  EXPECT_EQ(H5MF__aggr_try_extend(f, &f->shared->meta_aggr, H5FD_MEM_DEFAULT,
                                  f->shared->meta_aggr.addr, 16),
            1);
  EXPECT_EQ(f->shared->meta_aggr.tot_size, tot_before);
  EXPECT_EQ(f->shared->meta_aggr.size, size_before - 16);

  // Above 10% threshold and < alloc_size bubbles up by alloc_size (1000).
  const hsize_t size_before_bubble = f->shared->meta_aggr.size;
  EXPECT_EQ(H5MF__aggr_try_extend(f, &f->shared->meta_aggr, H5FD_MEM_DEFAULT,
                                  f->shared->meta_aggr.addr, 400),
            1);
  EXPECT_EQ(f->shared->meta_aggr.tot_size, tot_before + 1000);
  EXPECT_EQ(f->shared->meta_aggr.size, size_before_bubble + 1000 - 400);

  // Above alloc_size bubbles up by extra_requested (1200).
  const hsize_t size_before_big = f->shared->meta_aggr.size;
  EXPECT_EQ(H5MF__aggr_try_extend(f, &f->shared->meta_aggr, H5FD_MEM_DEFAULT,
                                  f->shared->meta_aggr.addr, 1200),
            1);
  EXPECT_EQ(f->shared->meta_aggr.tot_size, tot_before + 2200);
  EXPECT_EQ(f->shared->meta_aggr.size, size_before_big);

  // Reset aggregators so meta_aggr holds a single fresh block (tot_size - size
  // < alloc_size), preventing H5MF_aggr_vfd_alloc(DRAW) from freeing meta_aggr
  // when moving EOA past meta_aggr.
  ASSERT_GE(H5MF_free_aggrs(f), 0);
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 64), HADDR_UNDEF);
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 64), HADDR_UNDEF);
  ASSERT_GT(f->shared->meta_aggr.size, 32u);
  ASSERT_NE(f->shared->meta_aggr.addr + f->shared->meta_aggr.size,
            H5F_get_eoa(f, H5FD_MEM_DEFAULT));

  // Non-EOA meta_aggr succeeds when size >= extra_requested, fails otherwise.
  EXPECT_EQ(H5MF__aggr_try_extend(f, &f->shared->meta_aggr, H5FD_MEM_DEFAULT,
                                  f->shared->meta_aggr.addr, 32),
            1);
  EXPECT_EQ(H5MF__aggr_try_extend(f, &f->shared->meta_aggr, H5FD_MEM_DEFAULT,
                                  f->shared->meta_aggr.addr,
                                  f->shared->meta_aggr.size + 10),
            0);

  // Inactive feature flag disables H5MF__aggr_try_extend.
  const unsigned long saved_flags = f->shared->feature_flags;
  absl::Cleanup restore_flags = [&] { f->shared->feature_flags = saved_flags; };
  f->shared->feature_flags &= ~H5FD_FEAT_AGGREGATE_METADATA;
  EXPECT_EQ(H5MF__aggr_try_extend(f, &f->shared->meta_aggr, H5FD_MEM_DEFAULT,
                                  f->shared->meta_aggr.addr, 8),
            0);
}

TEST(H5MFaggrTest, CanAbsorbAndAbsorbFrontAndBack) {
  ScopedFile file = CreateCoreFile(H5F_FSPACE_STRATEGY_FSM_AGGR,
                                   /*persist=*/false, /*threshold=*/1,
                                   /*alignment=*/1, /*meta_block_size=*/512,
                                   /*sdata_block_size=*/512);
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  H5F_blk_aggr_t aggr = f->shared->meta_aggr;
  aggr.alloc_size = 512;
  aggr.addr = 0x2000;
  aggr.size = 128;
  aggr.tot_size = 200;

  H5MF_shrink_type_t shrink = H5MF_SHRINK_EOA;

  // Non-adjoining section returns false.
  ScopedSection disjoint(H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, 0x1000, 64));
  ASSERT_NE(disjoint.get(), nullptr);
  EXPECT_EQ(H5MF__aggr_can_absorb(f, &aggr, disjoint.get(), &shrink), 0);

  // Adjoining front with sum < alloc_size => AGGR_ABSORB_SECT.
  ScopedSection small_front(
      H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, 0x2000 - 64, 64));
  ASSERT_NE(small_front.get(), nullptr);
  EXPECT_EQ(H5MF__aggr_can_absorb(f, &aggr, small_front.get(), &shrink), 1);
  EXPECT_EQ(shrink, H5MF_SHRINK_AGGR_ABSORB_SECT);
  EXPECT_GE(H5MF__aggr_absorb(f, &aggr, small_front.get(), true), 0);
  EXPECT_EQ(aggr.addr, 0x2000u - 64u);
  EXPECT_EQ(aggr.size, 192u);
  EXPECT_EQ(aggr.tot_size, 136u);

  // Adjoining back with sum < alloc_size => aggr absorbs section onto end.
  ScopedSection small_back(
      H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, aggr.addr + aggr.size, 64));
  ASSERT_NE(small_back.get(), nullptr);
  EXPECT_EQ(H5MF__aggr_can_absorb(f, &aggr, small_back.get(), &shrink), 1);
  EXPECT_EQ(shrink, H5MF_SHRINK_AGGR_ABSORB_SECT);
  EXPECT_GE(H5MF__aggr_absorb(f, &aggr, small_back.get(), true), 0);
  EXPECT_EQ(aggr.size, 256u);

  // Adjoining front with sum >= alloc_size and allow_sect_absorb = false clamps
  // tot_size and absorbs onto front of aggr.
  ScopedSection forced_front(
      H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, aggr.addr - 300, 300));
  ASSERT_NE(forced_front.get(), nullptr);
  EXPECT_GE(H5MF__aggr_absorb(f, &aggr, forced_front.get(), false), 0);
  EXPECT_EQ(aggr.size, 556u);
  EXPECT_EQ(aggr.tot_size, 0u);

  // Adjoining front with sum >= alloc_size and allow_sect_absorb = true:
  // section absorbs aggr onto end and aggr resets.
  H5F_blk_aggr_t saved = aggr;
  ScopedSection big_front(
      H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, saved.addr - 512, 512));
  ASSERT_NE(big_front.get(), nullptr);
  EXPECT_EQ(H5MF__aggr_can_absorb(f, &saved, big_front.get(), &shrink), 1);
  EXPECT_EQ(shrink, H5MF_SHRINK_SECT_ABSORB_AGGR);
  EXPECT_GE(H5MF__aggr_absorb(f, &saved, big_front.get(), true), 0);
  EXPECT_EQ(big_front.get()->sect_info.size, 512u + 556u);
  EXPECT_EQ(saved.size, 0u);
  EXPECT_EQ(saved.addr, 0u);

  // Adjoining back with sum >= alloc_size and allow_sect_absorb = true:
  // section absorbs aggr onto front and aggr resets.
  saved = aggr;
  ScopedSection big_back(
      H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, saved.addr + saved.size, 512));
  ASSERT_NE(big_back.get(), nullptr);
  EXPECT_EQ(H5MF__aggr_can_absorb(f, &saved, big_back.get(), &shrink), 1);
  EXPECT_EQ(shrink, H5MF_SHRINK_SECT_ABSORB_AGGR);
  EXPECT_GE(H5MF__aggr_absorb(f, &saved, big_back.get(), true), 0);
  EXPECT_EQ(big_back.get()->sect_info.addr, aggr.addr);
  EXPECT_EQ(big_back.get()->sect_info.size, 512u + 556u);
  EXPECT_EQ(saved.size, 0u);

  // Inactive feature flag prevents absorption.
  const unsigned long saved_flags = f->shared->feature_flags;
  absl::Cleanup restore_flags = [&] { f->shared->feature_flags = saved_flags; };
  f->shared->feature_flags &= ~H5FD_FEAT_AGGREGATE_METADATA;
  EXPECT_EQ(H5MF__aggr_can_absorb(f, &aggr, small_front.get(), &shrink), 0);
}

TEST(H5MFaggrTest, FreeAggrsOrderingAndTryShrinkEoaAndClose) {
  ScopedFile file = CreateCoreFile(H5F_FSPACE_STRATEGY_FSM_AGGR,
                                   /*persist=*/false, /*threshold=*/1,
                                   /*alignment=*/1, /*meta_block_size=*/512,
                                   /*sdata_block_size=*/512);
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Initially meta_aggr may be at EOA; try shrinking before allocating sdata.
  EXPECT_GE(H5MF__aggrs_try_shrink_eoa(f), 0);

  // Allocate meta then sdata so ma_addr < sda_addr; free_aggrs releases sdata
  // first, then meta.
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 32), HADDR_UNDEF);
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 32), HADDR_UNDEF);
  ASSERT_LT(f->shared->meta_aggr.addr, f->shared->sdata_aggr.addr);
  EXPECT_GE(H5MF_free_aggrs(f), 0);
  EXPECT_EQ(f->shared->meta_aggr.size, 0u);
  EXPECT_EQ(f->shared->sdata_aggr.size, 0u);

  // Calling free_aggrs / try_shrink_eoa when both are empty is a safe no-op.
  EXPECT_GE(H5MF_free_aggrs(f), 0);
  EXPECT_EQ(H5MF__aggrs_try_shrink_eoa(f), 0);

  // Allocate sdata then meta so sda_addr < ma_addr (hits ma_addr >= sda_addr).
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 32), HADDR_UNDEF);
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 32), HADDR_UNDEF);
  ASSERT_GT(f->shared->meta_aggr.addr, f->shared->sdata_aggr.addr);

  // H5MF__aggrs_try_shrink_eoa shrinks meta_aggr at EOA (leaving sdata_aggr).
  const haddr_t eoa_before_shrink = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
  EXPECT_EQ(H5MF__aggrs_try_shrink_eoa(f), 1);
  EXPECT_LT(H5F_get_eoa(f, H5FD_MEM_DEFAULT), eoa_before_shrink);
  EXPECT_EQ(f->shared->meta_aggr.size, 0u);

  // Reset remaining sdata_aggr when meta_aggr was inactive/missing address.
  {
    const unsigned long saved_flags = f->shared->feature_flags;
    absl::Cleanup restore_flags = [&] { f->shared->feature_flags = saved_flags; };
    f->shared->feature_flags &= ~H5FD_FEAT_AGGREGATE_METADATA;
    EXPECT_GE(H5MF_free_aggrs(f), 0);
    EXPECT_EQ(f->shared->sdata_aggr.size, 0u);
  }

  // Put sdata_aggr at EOA and verify try_shrink_eoa shrinks sdata_aggr too.
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 64), HADDR_UNDEF);
  const haddr_t eoa_before_sdata_shrink = H5F_get_eoa(f, H5FD_MEM_DRAW);
  EXPECT_EQ(H5MF__aggrs_try_shrink_eoa(f), 1);
  EXPECT_LT(H5F_get_eoa(f, H5FD_MEM_DRAW), eoa_before_sdata_shrink);

  // Public dataset creation/write + close exercises H5MF_free_aggrs organically.
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "scalar", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  const int32_t value = 42;
  EXPECT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &value),
            0);
  dset.reset();
  space.reset();
  EXPECT_GE(H5Fclose(file.release()), 0);
}

// Minimal in-memory VFD with fault injection hooks for covering driver error
// paths inside H5MFaggr.c.
struct FaultVfdControls {
  bool fail_get_eoa = false;
  int fail_get_eoa_after = -1;  // When >= 0, fail after N successful calls.
  bool fail_set_eoa = false;
  bool fail_alloc = false;
  bool fail_free = false;
};

FaultVfdControls* g_fault_controls = nullptr;

struct FaultMemFile {
  H5FD_t pub{};
  haddr_t eoa = 0;
  std::vector<uint8_t> buf;
};

H5FD_t* FaultVfdOpen(const char* /*name*/, unsigned /*flags*/,
                     hid_t /*fapl_id*/, haddr_t maxaddr) {
  auto* file = new FaultMemFile();
  file->pub.maxaddr = maxaddr;
  return &file->pub;
}

herr_t FaultVfdClose(H5FD_t* f) {
  delete reinterpret_cast<FaultMemFile*>(f);
  return SUCCEED;
}

int FaultVfdCmp(const H5FD_t* f1, const H5FD_t* f2) {
  if (f1 < f2) return -1;
  if (f1 > f2) return 1;
  return 0;
}

herr_t FaultVfdQuery(const H5FD_t* /*f*/, unsigned long* flags) {
  if (flags != nullptr) {
    *flags = H5FD_FEAT_AGGREGATE_METADATA | H5FD_FEAT_AGGREGATE_SMALLDATA;
  }
  return SUCCEED;
}

haddr_t FaultVfdAlloc(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                      hsize_t size) {
  if (g_fault_controls != nullptr && g_fault_controls->fail_alloc) {
    return HADDR_UNDEF;
  }
  auto* file = reinterpret_cast<FaultMemFile*>(f);
  const haddr_t addr = file->eoa;
  file->eoa += size;
  if (file->eoa > file->buf.size()) {
    file->buf.resize(static_cast<size_t>(file->eoa), 0);
  }
  return addr;
}

herr_t FaultVfdFree(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                    haddr_t addr, hsize_t size) {
  if (g_fault_controls != nullptr && g_fault_controls->fail_free) {
    return FAIL;
  }
  auto* file = reinterpret_cast<FaultMemFile*>(f);
  if (addr + size == file->eoa) {
    file->eoa = addr;
  }
  return SUCCEED;
}

haddr_t FaultVfdGetEoa(const H5FD_t* f, H5FD_mem_t /*type*/) {
  if (g_fault_controls != nullptr) {
    if (g_fault_controls->fail_get_eoa) {
      return HADDR_UNDEF;
    }
    if (g_fault_controls->fail_get_eoa_after >= 0) {
      if (g_fault_controls->fail_get_eoa_after == 0) {
        return HADDR_UNDEF;
      }
      --g_fault_controls->fail_get_eoa_after;
    }
  }
  const auto* file = reinterpret_cast<const FaultMemFile*>(f);
  return file->eoa;
}

herr_t FaultVfdSetEoa(H5FD_t* f, H5FD_mem_t /*type*/, haddr_t addr) {
  if (g_fault_controls != nullptr && g_fault_controls->fail_set_eoa) {
    return FAIL;
  }
  auto* file = reinterpret_cast<FaultMemFile*>(f);
  file->eoa = addr;
  if (file->eoa > file->buf.size()) {
    file->buf.resize(static_cast<size_t>(file->eoa), 0);
  }
  return SUCCEED;
}

haddr_t FaultVfdGetEof(const H5FD_t* f, H5FD_mem_t /*type*/) {
  const auto* file = reinterpret_cast<const FaultMemFile*>(f);
  return static_cast<haddr_t>(file->buf.size());
}

herr_t FaultVfdRead(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                    haddr_t addr, size_t size, void* out) {
  const auto* file = reinterpret_cast<const FaultMemFile*>(f);
  std::memset(out, 0, size);
  if (addr < file->buf.size()) {
    const size_t avail = static_cast<size_t>(file->buf.size() - addr);
    const size_t n = std::min(avail, size);
    std::memcpy(out, file->buf.data() + addr, n);
  }
  return SUCCEED;
}

herr_t FaultVfdWrite(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                     haddr_t addr, size_t size, const void* in) {
  auto* file = reinterpret_cast<FaultMemFile*>(f);
  if (addr + size > file->buf.size()) {
    file->buf.resize(static_cast<size_t>(addr + size), 0);
  }
  if (size > 0) {
    std::memcpy(file->buf.data() + addr, in, size);
  }
  return SUCCEED;
}

TEST(H5MFaggrTest, FaultInjectedVfdCoversDriverErrorPaths) {
  FaultVfdControls controls;
  g_fault_controls = &controls;
  absl::Cleanup clear_controls = [] { g_fault_controls = nullptr; };

  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = static_cast<H5FD_class_value_t>(512);
  cls.name = "fault_mem_aggr_test";
  cls.maxaddr = (HADDR_MAX / 2);
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.open = FaultVfdOpen;
  cls.close = FaultVfdClose;
  cls.cmp = FaultVfdCmp;
  cls.query = FaultVfdQuery;
  cls.alloc = FaultVfdAlloc;
  cls.free = FaultVfdFree;
  cls.get_eoa = FaultVfdGetEoa;
  cls.set_eoa = FaultVfdSetEoa;
  cls.get_eof = FaultVfdGetEof;
  cls.read = FaultVfdRead;
  cls.write = FaultVfdWrite;
  constexpr H5FD_mem_t kFlMap[H5FD_MEM_NTYPES] = H5FD_FLMAP_DICHOTOMY;
  std::memcpy(cls.fl_map, kFlMap, sizeof(kFlMap));

  const hid_t drv_id = H5FDregister(&cls);
  ASSERT_GE(drv_id, 0);
  absl::Cleanup unreg_drv = [drv_id] { H5FDunregister(drv_id); };

  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_FSM_AGGR,
                                       false, 1),
            0);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_driver(fapl.get(), drv_id, nullptr), 0);
  ASSERT_GE(H5Pset_meta_block_size(fapl.get(), 512), 0);
  ASSERT_GE(H5Pset_small_data_block_size(fapl.get(), 512), 0);

  ScopedFile file(
      H5Fcreate("fault_aggr.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  // Ensure fault flags are reset before `file` closes.
  absl::Cleanup reset_faults = [&controls] { controls = FaultVfdControls{}; };

  // 1) fail_get_eoa: H5MF_aggr_vfd_alloc, H5MF__aggr_try_extend,
  // H5MF__aggrs_try_shrink_eoa (both first and second checks).
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 64), HADDR_UNDEF);
  controls.fail_get_eoa = true;
  EXPECT_EQ(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 32), HADDR_UNDEF);
  EXPECT_LT(H5MF__aggr_try_extend(f, &f->shared->meta_aggr, H5FD_MEM_DEFAULT,
                                  f->shared->meta_aggr.addr, 16),
            0);
  EXPECT_LT(H5MF__aggrs_try_shrink_eoa(f), 0);
  controls.fail_get_eoa = false;

  controls.fail_get_eoa_after = 1;
  EXPECT_LT(H5MF__aggrs_try_shrink_eoa(f), 0);
  controls.fail_get_eoa_after = -1;

  // 2) fail_set_eoa (H5F__try_extend failure):
  // Ensure meta_aggr is at EOA, then fail extension for >= alloc_size,
  // < alloc_size overflow, and H5MF__aggr_try_extend (>10% bubble up).
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 64), HADDR_UNDEF);
  controls.fail_set_eoa = true;
  EXPECT_EQ(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 600), HADDR_UNDEF);
  const hsize_t overflow_req = f->shared->meta_aggr.size + 16;
  ASSERT_LT(overflow_req, f->shared->meta_aggr.alloc_size);
  EXPECT_EQ(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, overflow_req), HADDR_UNDEF);
  EXPECT_LT(H5MF__aggr_try_extend(f, &f->shared->meta_aggr, H5FD_MEM_DEFAULT,
                                  f->shared->meta_aggr.addr,
                                  f->shared->meta_aggr.size),
            0);
  controls.fail_set_eoa = false;

  // 3) fail_free:
  // a) H5MF__aggrs_try_shrink_eoa fails shrinking meta_aggr at EOA.
  controls.fail_free = true;
  EXPECT_LT(H5MF__aggrs_try_shrink_eoa(f), 0);
  controls.fail_free = false;

  // b) H5MF_free_aggrs fails resetting first_aggr (meta_aggr at EOA) when
  // fail_free = true; then succeeds on first_aggr and fails on second_aggr.
  controls.fail_free = true;
  EXPECT_LT(H5MF_free_aggrs(f), 0);
  controls.fail_free = false;
  EXPECT_GE(H5MF_free_aggrs(f), 0);

  // Set up sdata_aggr empty/lower and meta_aggr hitting tmp_addr on xfree so
  // second_aggr reset fails in H5MF_free_aggrs.
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 32), HADDR_UNDEF);
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 32), HADDR_UNDEF);
  {
    const haddr_t saved_tmp = f->shared->tmp_addr;
    absl::Cleanup restore_tmp = [&] { f->shared->tmp_addr = saved_tmp; };
    f->shared->tmp_addr = f->shared->meta_aggr.addr;
    // Free sdata_aggr first (above meta_aggr), then fail on second_aggr (meta).
    EXPECT_LT(H5MF_free_aggrs(f), 0);
  }
  EXPECT_GE(H5MF_free_aggrs(f), 0);

  // c) H5MF__aggrs_try_shrink_eoa fails shrinking sdata_aggr at EOA.
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 32), HADDR_UNDEF);
  controls.fail_free = true;
  EXPECT_LT(H5MF__aggrs_try_shrink_eoa(f), 0);
  controls.fail_free = false;

  // d) Releasing other_aggr (sdata_aggr with tot_size - size >= alloc_size)
  // fails with fail_free = true in both size >= alloc_size and < alloc_size.
  const hsize_t req_extend = f->shared->sdata_aggr.size + 16;
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, req_extend), HADDR_UNDEF);
  ASSERT_GE(f->shared->sdata_aggr.tot_size - f->shared->sdata_aggr.size,
            f->shared->sdata_aggr.alloc_size);
  ASSERT_EQ(f->shared->meta_aggr.size, 0u);
  controls.fail_free = true;
  EXPECT_EQ(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 600), HADDR_UNDEF);
  EXPECT_EQ(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 100), HADDR_UNDEF);
  controls.fail_free = false;
  EXPECT_GE(H5MF_free_aggrs(f), 0);

  // 4) fail_alloc: H5F__alloc fails for >= alloc_size, < alloc_size, and
  // inactive aggregator strategy.
  controls.fail_alloc = true;
  EXPECT_EQ(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 600), HADDR_UNDEF);
  EXPECT_EQ(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 100), HADDR_UNDEF);
  {
    const H5F_fspace_strategy_t saved_strat = f->shared->fs_strategy;
    f->shared->fs_strategy = H5F_FSPACE_STRATEGY_NONE;
    absl::Cleanup restore_strat = [&] { f->shared->fs_strategy = saved_strat; };
    EXPECT_EQ(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 64), HADDR_UNDEF);
  }
  controls.fail_alloc = false;
}

}  // namespace
}  // namespace hdf5
