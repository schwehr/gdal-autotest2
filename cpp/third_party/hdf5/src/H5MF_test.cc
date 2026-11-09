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
// Tests file memory management, free-space manager lifecycles, block
// aggregators, paged allocation, and free-space settling
// (third_party/hdf5/src/H5MF.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5MF.c

// `:hdf5` is built with -DNDEBUG, which omits #ifndef NDEBUG fields from
// H5C_cache_entry_t (H5AC_info_t) inside H5FS_t, H5FS_sinfo_t, and H5F_super_t.
// Define NDEBUG around hdf5_internal_prelude.h and internal headers so struct
// layouts match `:hdf5`. H5Fpkg.h also uses `try` as a parameter name.
// clang-format off
#ifndef NDEBUG
#define H5MF_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try hdf5_try_param
extern "C" {
#define H5F_FRIEND
#define H5FS_FRIEND
#define H5MF_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5FSpkg.h"
#include "third_party/hdf5/src/H5MFpkg.h"
#include "third_party/hdf5/src/H5PBprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5MF_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5MF_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  ScopedApiContext() {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(H5AC__FREESPACE_TAG, &prev_tag_);
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

 private:
  H5CX_node_t node_ = {};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

haddr_t FailingGetEoa(const H5FD_t* /*file*/, H5FD_mem_t /*type*/) {
  return HADDR_UNDEF;
}

herr_t FailingSetEoa(H5FD_t* /*file*/, H5FD_mem_t /*type*/, haddr_t /*addr*/) {
  return -1;
}

herr_t FailingWrite(H5FD_t* /*file*/, H5FD_mem_t /*type*/, hid_t /*dxpl*/,
                    haddr_t /*addr*/, size_t /*size*/, const void* /*buffer*/) {
  return -1;
}

ScopedPlist MakeStrategyFcpl(H5F_fspace_strategy_t strategy, bool persist,
                             hsize_t threshold, hsize_t page_size = 4096) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (!fcpl.valid()) return ScopedPlist();
  if (H5Pset_file_space_strategy(fcpl.get(), strategy, persist, threshold) <
      0) {
    fcpl.reset();
    return fcpl;
  }
  if (strategy == H5F_FSPACE_STRATEGY_PAGE &&
      H5Pset_file_space_page_size(fcpl.get(), page_size) < 0) {
    fcpl.reset();
  }
  return fcpl;
}

ScopedPlist MakeFaplWithV110AndCore(bool disable_aggr = false) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) return ScopedPlist();
  if (H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST) <
      0) {
    fapl.reset();
    return fapl;
  }
  if (disable_aggr) {
    if (H5Pset_meta_block_size(fapl.get(), 0) < 0 ||
        H5Pset_small_data_block_size(fapl.get(), 0) < 0) {
      fapl.reset();
    }
  }
  return fapl;
}

TEST(H5MFTest, InitMergeFlagsMapsAllTypeConfigurations) {
  ScopedFile file = CreateInMemoryFile("mf_merge_flags.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  H5F_shared_t* f_sh = f->shared;

  std::array<H5FD_mem_t, H5FD_MEM_NTYPES> saved_map{};
  std::array<unsigned, H5FD_MEM_NTYPES> saved_merge{};
  std::memcpy(saved_map.data(), f_sh->fs_type_map, sizeof(saved_map));
  std::memcpy(saved_merge.data(), f_sh->fs_aggr_merge, sizeof(saved_merge));
  absl::Cleanup restore = [&]() {
    std::memcpy(f_sh->fs_type_map, saved_map.data(), sizeof(saved_map));
    std::memcpy(f_sh->fs_aggr_merge, saved_merge.data(), sizeof(saved_merge));
  };

  // 1. All H5FD_MEM_DEFAULT -> SEPARATE, raw data merging allowed.
  std::array<H5FD_mem_t, H5FD_MEM_NTYPES> map_all_default = {
      H5FD_MEM_DEFAULT, H5FD_MEM_DEFAULT, H5FD_MEM_DEFAULT, H5FD_MEM_DEFAULT,
      H5FD_MEM_DEFAULT, H5FD_MEM_DEFAULT, H5FD_MEM_DEFAULT,
  };
  std::memcpy(f_sh->fs_type_map, map_all_default.data(),
              sizeof(map_all_default));
  ASSERT_GE(H5MF_init_merge_flags(f_sh), 0);
  EXPECT_EQ(f_sh->fs_aggr_merge[H5FD_MEM_SUPER], 0U);
  EXPECT_EQ(f_sh->fs_aggr_merge[H5FD_MEM_DRAW], H5F_FS_MERGE_RAWDATA);
  EXPECT_EQ(f_sh->fs_aggr_merge[H5FD_MEM_GHEAP], H5F_FS_MERGE_RAWDATA);

  // 2. All H5FD_MEM_SUPER -> TOGETHER, metadata & raw data merge across all.
  std::array<H5FD_mem_t, H5FD_MEM_NTYPES> map_all_super = {
      H5FD_MEM_SUPER, H5FD_MEM_SUPER, H5FD_MEM_SUPER, H5FD_MEM_SUPER,
      H5FD_MEM_SUPER, H5FD_MEM_SUPER, H5FD_MEM_SUPER,
  };
  std::memcpy(f_sh->fs_type_map, map_all_super.data(), sizeof(map_all_super));
  ASSERT_GE(H5MF_init_merge_flags(f_sh), 0);
  for (unsigned m : f_sh->fs_aggr_merge) {
    EXPECT_NE(m & H5F_FS_MERGE_METADATA, 0U);
    EXPECT_NE(m & H5F_FS_MERGE_RAWDATA, 0U);
  }

  // 3. Dichotomy: all metadata -> SUPER, DRAW & GHEAP -> DRAW.
  std::array<H5FD_mem_t, H5FD_MEM_NTYPES> map_dichotomy = {
      H5FD_MEM_SUPER, H5FD_MEM_SUPER, H5FD_MEM_SUPER, H5FD_MEM_DRAW,
      H5FD_MEM_DRAW,  H5FD_MEM_SUPER, H5FD_MEM_SUPER,
  };
  std::memcpy(f_sh->fs_type_map, map_dichotomy.data(), sizeof(map_dichotomy));
  ASSERT_GE(H5MF_init_merge_flags(f_sh), 0);
  EXPECT_NE(f_sh->fs_aggr_merge[H5FD_MEM_SUPER] & H5F_FS_MERGE_METADATA, 0U);
  EXPECT_EQ(f_sh->fs_aggr_merge[H5FD_MEM_SUPER] & H5F_FS_MERGE_RAWDATA, 0U);
  EXPECT_NE(f_sh->fs_aggr_merge[H5FD_MEM_OHDR] & H5F_FS_MERGE_METADATA, 0U);
  EXPECT_EQ(f_sh->fs_aggr_merge[H5FD_MEM_OHDR] & H5F_FS_MERGE_RAWDATA, 0U);
  EXPECT_EQ(f_sh->fs_aggr_merge[H5FD_MEM_DRAW], H5F_FS_MERGE_RAWDATA);
  EXPECT_EQ(f_sh->fs_aggr_merge[H5FD_MEM_GHEAP], H5F_FS_MERGE_RAWDATA);

  // 4. DRAW == SUPER with distinct BTREE -> SEPARATE and raw merge disabled.
  std::array<H5FD_mem_t, H5FD_MEM_NTYPES> map_draw_eq_super = {
      H5FD_MEM_DEFAULT, H5FD_MEM_SUPER, H5FD_MEM_BTREE, H5FD_MEM_SUPER,
      H5FD_MEM_DRAW,    H5FD_MEM_SUPER, H5FD_MEM_SUPER,
  };
  std::memcpy(f_sh->fs_type_map, map_draw_eq_super.data(),
              sizeof(map_draw_eq_super));
  ASSERT_GE(H5MF_init_merge_flags(f_sh), 0);
  EXPECT_EQ(f_sh->fs_aggr_merge[H5FD_MEM_SUPER], 0U);
  EXPECT_EQ(f_sh->fs_aggr_merge[H5FD_MEM_DRAW], 0U);

  // 5. Mixed metadata (BTREE != SUPER) with DRAW == DRAW -> SEPARATE + RAWDATA.
  std::array<H5FD_mem_t, H5FD_MEM_NTYPES> map_mixed_meta = {
      H5FD_MEM_DEFAULT, H5FD_MEM_SUPER, H5FD_MEM_BTREE, H5FD_MEM_DRAW,
      H5FD_MEM_DRAW,    H5FD_MEM_LHEAP, H5FD_MEM_OHDR,
  };
  std::memcpy(f_sh->fs_type_map, map_mixed_meta.data(), sizeof(map_mixed_meta));
  ASSERT_GE(H5MF_init_merge_flags(f_sh), 0);
  EXPECT_EQ(f_sh->fs_aggr_merge[H5FD_MEM_BTREE], 0U);
  EXPECT_EQ(f_sh->fs_aggr_merge[H5FD_MEM_DRAW], H5F_FS_MERGE_RAWDATA);
  EXPECT_EQ(f_sh->fs_aggr_merge[H5FD_MEM_GHEAP], H5F_FS_MERGE_RAWDATA);
}

TEST(H5MFTest, AllocToFsTypeCoversPagedNonPagedAndMultiMap) {
  // 1. Non-paged aggregation mapping with explicit and default entries.
  {
    ScopedFile file = CreateInMemoryFile("mf_fs_type_nonpaged.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    H5F_shared_t* f_sh = f->shared;

    H5F_mem_page_t fs_type = H5F_MEM_PAGE_NTYPES;
    H5MF__alloc_to_fs_type(f_sh, H5FD_MEM_OHDR, 64, &fs_type);
    EXPECT_EQ(fs_type, H5F_MEM_PAGE_SUPER);

    H5MF__alloc_to_fs_type(f_sh, H5FD_MEM_DRAW, 64, &fs_type);
    EXPECT_EQ(fs_type, H5F_MEM_PAGE_DRAW);

    const H5FD_mem_t saved_btree = f_sh->fs_type_map[H5FD_MEM_BTREE];
    absl::Cleanup restore = [&]() {
      f_sh->fs_type_map[H5FD_MEM_BTREE] = saved_btree;
    };
    f_sh->fs_type_map[H5FD_MEM_BTREE] = H5FD_MEM_DEFAULT;
    H5MF__alloc_to_fs_type(f_sh, H5FD_MEM_BTREE, 64, &fs_type);
    EXPECT_EQ(fs_type, H5F_MEM_PAGE_BTREE);
  }

  // 2. Paged aggregation with contiguous VFD vs H5FD_FEAT_PAGED_AGGR VFD.
  {
    ScopedPlist fcpl =
        MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, false, 1, 512);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_fs_type_paged.h5", H5F_ACC_TRUNC, fcpl.get(),
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    H5F_shared_t* f_sh = f->shared;

    H5F_mem_page_t fs_type = H5F_MEM_PAGE_NTYPES;
    // Small request maps to small paged FSM.
    H5MF__alloc_to_fs_type(f_sh, H5FD_MEM_OHDR, 256, &fs_type);
    EXPECT_EQ(fs_type, H5F_MEM_PAGE_META);
    H5MF__alloc_to_fs_type(f_sh, H5FD_MEM_DRAW, 256, &fs_type);
    EXPECT_EQ(fs_type, H5F_MEM_PAGE_DRAW);

    // Large request on contiguous VFD maps to H5F_MEM_PAGE_GENERIC.
    H5MF__alloc_to_fs_type(f_sh, H5FD_MEM_DRAW, 512, &fs_type);
    EXPECT_EQ(fs_type, H5F_MEM_PAGE_GENERIC);

    // Temporarily emulate non-contiguous VFD with H5FD_FEAT_PAGED_AGGR.
    const unsigned long saved_lf_flags = f_sh->lf->feature_flags;
    const unsigned long saved_sh_flags = f_sh->feature_flags;
    const H5FD_mem_t saved_lheap = f_sh->fs_type_map[H5FD_MEM_LHEAP];
    absl::Cleanup restore = [&]() {
      f_sh->lf->feature_flags = saved_lf_flags;
      f_sh->feature_flags = saved_sh_flags;
      f_sh->fs_type_map[H5FD_MEM_LHEAP] = saved_lheap;
    };
    f_sh->lf->feature_flags |= H5FD_FEAT_PAGED_AGGR;
    f_sh->feature_flags |= H5FD_FEAT_PAGED_AGGR;

    // Explicit non-default mapping -> mapped type + (NTYPES - 1).
    H5MF__alloc_to_fs_type(f_sh, H5FD_MEM_DRAW, 512, &fs_type);
    EXPECT_EQ(fs_type, H5F_MEM_PAGE_LARGE_DRAW);

    // H5FD_MEM_DEFAULT entry -> alloc_type + (NTYPES - 1).
    f_sh->fs_type_map[H5FD_MEM_LHEAP] = H5FD_MEM_DEFAULT;
    H5MF__alloc_to_fs_type(f_sh, H5FD_MEM_LHEAP, 512, &fs_type);
    EXPECT_EQ(fs_type, H5F_MEM_PAGE_LARGE_LHEAP);
  }
}

TEST(H5MFTest, StartAddFindAndDeleteSectionsLifecycle) {
  // 1. Non-paged FSM section creation, split, exact-match reuse, and deletion.
  {
    ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_FSM_AGGR, false, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore(/*disable_aggr=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_sect_nonpaged.h5", H5F_ACC_TRUNC, fcpl.get(),
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    // Allocate real blocks so section offsets stay within valid EOA/non-EOA
    // bounds below EOA (avoiding EOA shrink during add_sect).
    const haddr_t blk_draw = H5MF_alloc(f, H5FD_MEM_DRAW, 256);
    const haddr_t blk_meta = H5MF_alloc(f, H5FD_MEM_SUPER, 256);
    const haddr_t blk_tail = H5MF_alloc(f, H5FD_MEM_DRAW, 64);
    ASSERT_NE(blk_draw, HADDR_UNDEF);
    ASSERT_NE(blk_meta, HADDR_UNDEF);
    ASSERT_NE(blk_tail, HADDR_UNDEF);

    // Start both RDFSM (DRAW) and self-referential MDFSM (SUPER).
    ASSERT_GE(H5MF__start_fstype(f, H5F_MEM_PAGE_DRAW), 0);
    ASSERT_GE(H5MF__start_fstype(f, H5F_MEM_PAGE_SUPER), 0);

    // Empty search returns 0.
    haddr_t found_addr = HADDR_UNDEF;
    EXPECT_EQ(H5MF__find_sect(f, H5FD_MEM_DRAW, 64,
                              f->shared->fs_man[H5FD_MEM_DRAW], &found_addr),
              0);

    // Add 256B DRAW section, split via 100B find, then exact-match 156B find.
    H5MF_free_section_t* sect_draw =
        H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, blk_draw, 256);
    ASSERT_NE(sect_draw, nullptr);
    bool merged_or_shrunk = false;
    ASSERT_GE(H5MF__add_sect(f, H5FD_MEM_DRAW, f->shared->fs_man[H5FD_MEM_DRAW],
                             sect_draw, &merged_or_shrunk),
              0);
    EXPECT_FALSE(merged_or_shrunk);

    EXPECT_GT(H5MF__find_sect(f, H5FD_MEM_DRAW, 100,
                              f->shared->fs_man[H5FD_MEM_DRAW], &found_addr),
              0);
    EXPECT_EQ(found_addr, blk_draw);

    EXPECT_GT(H5MF__find_sect(f, H5FD_MEM_DRAW, 156,
                              f->shared->fs_man[H5FD_MEM_DRAW], &found_addr),
              0);
    EXPECT_EQ(found_addr, blk_draw + 100);
    EXPECT_EQ(H5MF__find_sect(f, H5FD_MEM_DRAW, 1,
                              f->shared->fs_man[H5FD_MEM_DRAW], &found_addr),
              0);

    // Add self-referential MDFSM section and reuse it.
    H5MF_free_section_t* sect_meta =
        H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, blk_meta, 256);
    ASSERT_NE(sect_meta, nullptr);
    ASSERT_GE(
        H5MF__add_sect(f, H5FD_MEM_SUPER, f->shared->fs_man[H5FD_MEM_SUPER],
                       sect_meta, &merged_or_shrunk),
        0);
    EXPECT_GT(H5MF__find_sect(f, H5FD_MEM_SUPER, 256,
                              f->shared->fs_man[H5FD_MEM_SUPER], &found_addr),
              0);
    EXPECT_EQ(found_addr, blk_meta);
  }

  // 2. Paged FSM start covers GENERIC (page alignment) and small DRAW.
  {
    ScopedPlist fcpl =
        MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, false, 1, 512);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("mf_sect_paged.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    ASSERT_GE(H5MF__start_fstype(f, H5F_MEM_PAGE_GENERIC), 0);
    ASSERT_GE(H5MF__start_fstype(f, H5F_MEM_PAGE_DRAW), 0);
    EXPECT_NE(f->shared->fs_man[H5F_MEM_PAGE_GENERIC], nullptr);
    EXPECT_NE(f->shared->fs_man[H5F_MEM_PAGE_DRAW], nullptr);
  }
}

TEST(H5MFTest, AllocTmpAllocatesSafelyAndDetectsOverlapAndEoaErrors) {
  ScopedFile file = CreateInMemoryFile("mf_alloc_tmp.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;

  const haddr_t orig_tmp = f->shared->tmp_addr;
  absl::Cleanup restore_tmp = [&]() { f->shared->tmp_addr = orig_tmp; };

  const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
  ASSERT_NE(eoa, HADDR_UNDEF);

  const haddr_t t1 = H5MF_alloc_tmp(f, 128);
  const haddr_t t2 = H5MF_alloc_tmp(f, 256);
  ASSERT_NE(t1, HADDR_UNDEF);
  ASSERT_NE(t2, HADDR_UNDEF);
  EXPECT_EQ(t1 - 256, t2);
  EXPECT_GT(t2, eoa);

  ScopedErrorSilence silence;
  // Attempting to free a temporary address via H5MF_xfree fails cleanly.
  EXPECT_LT(H5MF_xfree(f, H5FD_MEM_SUPER, t2, 256), 0);

  // Requesting a temporary allocation overlapping EOA fails.
  EXPECT_EQ(H5MF_alloc_tmp(f, f->shared->tmp_addr - eoa), HADDR_UNDEF);

  // Driver get_eoa failure in H5MF_alloc_tmp fails cleanly.
  const H5FD_class_t* orig_cls = f->shared->lf->cls;
  H5FD_class_t bad_cls = *orig_cls;
  bad_cls.get_eoa = FailingGetEoa;
  absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
  f->shared->lf->cls = &bad_cls;
  EXPECT_EQ(H5MF_alloc_tmp(f, 64), HADDR_UNDEF);
}

TEST(H5MFTest, XfreeHandlesNoopShrinkDropMergeAndStrategies) {
  // 1. Threshold drop, delete-state drop, EOA shrink, start FSM, merge & fail.
  {
    ScopedPlist fcpl =
        MakeStrategyFcpl(H5F_FSPACE_STRATEGY_FSM_AGGR, false, /*threshold=*/64);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore(/*disable_aggr=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("mf_xfree_fsm.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    // Early no-op checks.
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, HADDR_UNDEF, 64), 0);
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, 128, 0), 0);

    // Allocate 5 blocks at EOA: [a(64)][b(32)][c(128)][d(32)][e(64)].
    const haddr_t a = H5MF_alloc(f, H5FD_MEM_DRAW, 64);
    const haddr_t b = H5MF_alloc(f, H5FD_MEM_DRAW, 32);
    const haddr_t c = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    const haddr_t d = H5MF_alloc(f, H5FD_MEM_DRAW, 32);
    const haddr_t e = H5MF_alloc(f, H5FD_MEM_DRAW, 64);
    ASSERT_NE(a, HADDR_UNDEF);
    ASSERT_NE(b, HADDR_UNDEF);
    ASSERT_NE(c, HADDR_UNDEF);
    ASSERT_NE(d, HADDR_UNDEF);
    ASSERT_NE(e, HADDR_UNDEF);

    // Freeing 'e' at EOA shrinks EOA without starting DRAW FSM.
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, e, 64), 0);
    EXPECT_EQ(f->shared->fs_man[H5FD_MEM_DRAW], nullptr);

    // Freeing 'b' (32B < threshold=64B) while FSM is closed drops it on floor.
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, b, 32), 0);
    EXPECT_EQ(f->shared->fs_man[H5FD_MEM_DRAW], nullptr);

    // Freeing metadata 'm' while fs_state == DELETING drops it cleanly.
    const haddr_t m1 = H5MF_alloc(f, H5FD_MEM_OHDR, 128);
    const haddr_t m2 = H5MF_alloc(f, H5FD_MEM_OHDR, 128);
    ASSERT_NE(m1, HADDR_UNDEF);
    ASSERT_NE(m2, HADDR_UNDEF);
    {
      const H5F_fs_state_t orig_state = f->shared->fs_state[H5FD_MEM_SUPER];
      absl::Cleanup restore_state = [&]() {
        f->shared->fs_state[H5FD_MEM_SUPER] = orig_state;
      };
      f->shared->fs_state[H5FD_MEM_SUPER] = H5F_FS_STATE_DELETING;
      EXPECT_GE(H5MF_xfree(f, H5FD_MEM_OHDR, m1, 128), 0);
      EXPECT_EQ(f->shared->fs_man[H5FD_MEM_SUPER], nullptr);
    }

    // Freeing 'c' (128B >= threshold=64B) starts DRAW FSM and inserts section.
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, c, 128), 0);
    ASSERT_NE(f->shared->fs_man[H5FD_MEM_DRAW], nullptr);

    // Now DRAW FSM is open:
    // - Freeing non-adjacent 'a' (64B >= 64) stays separate.
    // - Freeing adjacent 'b' (32B < 64, adjoins a and c!) merges via
    //   H5FS_sect_try_merge (merged == true).
    // - Freeing non-adjacent 16B (< 64) fails merge (merged == false -> frees
    //   section node cleanly in done:).
    const haddr_t f_tail = H5MF_alloc(f, H5FD_MEM_DRAW, 16);
    const haddr_t f_keep = H5MF_alloc(f, H5FD_MEM_DRAW, 32);
    ASSERT_NE(f_tail, HADDR_UNDEF);
    ASSERT_NE(f_keep, HADDR_UNDEF);

    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, b, 32), 0);
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, f_tail, 16), 0);

    size_t sect_cnt = 0;
    ASSERT_GE(H5MF_get_free_sections(f, H5FD_MEM_DRAW, 0, nullptr, &sect_cnt),
              0);
    EXPECT_GE(sect_cnt, 1U);

    // Also free metadata m1 now to exercise H5F__accum_free path in H5MF_xfree.
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_OHDR, m1, 128), 0);
  }

  // 2. Non-FSM strategies (AGGR and NONE) drop non-EOA frees cleanly.
  for (H5F_fspace_strategy_t strat :
       {H5F_FSPACE_STRATEGY_AGGR, H5F_FSPACE_STRATEGY_NONE}) {
    ScopedPlist fcpl = MakeStrategyFcpl(strat, false, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore(/*disable_aggr=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("mf_xfree_no_fsm.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    const haddr_t b1 = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    const haddr_t b2 = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    ASSERT_NE(b1, HADDR_UNDEF);
    ASSERT_NE(b2, HADDR_UNDEF);
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, b1, 128), 0);
    EXPECT_EQ(f->shared->fs_man[H5FD_MEM_DRAW], nullptr);
  }
}

TEST(H5MFTest, TryExtendCoversEoaAggrFsmAndPagedBoundaries) {
  // 1. Non-paged FSM_AGGR: EOA extend (incl GHEAP), aggr extend, FSM extend.
  {
    ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_FSM_AGGR, false, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("mf_extend_aggr.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    // Small raw data allocation creates sdata_aggr; extending into sdata_aggr
    // succeeds via H5MF__aggr_try_extend.
    const haddr_t g1 = H5MF_alloc(f, H5FD_MEM_GHEAP, 64);
    ASSERT_NE(g1, HADDR_UNDEF);
    EXPECT_GT(H5MF_try_extend(f, H5FD_MEM_GHEAP, g1, 64, 64), 0);

    // Large allocation at EOA bypasses aggregator and extends at EOA directly.
    const haddr_t big = H5MF_alloc(f, H5FD_MEM_GHEAP, 4096);
    ASSERT_NE(big, HADDR_UNDEF);
    EXPECT_GT(H5MF_try_extend(f, H5FD_MEM_GHEAP, big, 4096, 256), 0);

    // Adjacent FSM section extension: allocate b1, b2, b3, free b2, extend b1.
    const haddr_t b1 = H5MF_alloc(f, H5FD_MEM_DRAW, 4096);
    const haddr_t b2 = H5MF_alloc(f, H5FD_MEM_DRAW, 4096);
    const haddr_t b3 = H5MF_alloc(f, H5FD_MEM_DRAW, 4096);
    ASSERT_NE(b1, HADDR_UNDEF);
    ASSERT_NE(b2, HADDR_UNDEF);
    ASSERT_NE(b3, HADDR_UNDEF);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, b2, 4096), 0);
    EXPECT_GT(H5MF_try_extend(f, H5FD_MEM_DRAW, b1, 4096, 1024), 0);
    // Extending b1 past available section returns 0.
    EXPECT_EQ(H5MF_try_extend(f, H5FD_MEM_DRAW, b1 + 1024, 4096, 4096), 0);
  }

  // 2. Paged aggregation: page boundary block, pgend_meta_thres, large frag.
  {
    ScopedPlist fcpl =
        MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, false, 1, 512);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("mf_extend_paged.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    // Allocate two small DRAW blocks sharing a 512B page, free second, extend.
    const haddr_t s1 = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    const haddr_t s2 = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    ASSERT_NE(s1, HADDR_UNDEF);
    ASSERT_NE(s2, HADDR_UNDEF);
    // Crossing page boundary is rejected (returns 0).
    EXPECT_EQ(H5MF_try_extend(f, H5FD_MEM_DRAW, s1, 128, 512), 0);
    // Extending into adjacent small free space on same page succeeds.
    EXPECT_GT(H5MF_try_extend(f, H5FD_MEM_DRAW, s2, 128, 64), 0);

    // Small metadata extending into pgend_meta_thres at page end.
    const haddr_t m1 = H5MF_alloc(f, H5FD_MEM_OHDR, 480);
    ASSERT_NE(m1, HADDR_UNDEF);
    // Consume the remaining 32B from small META FSM so no free section exists.
    const haddr_t m_rem = H5MF_alloc(f, H5FD_MEM_OHDR, 32);
    ASSERT_NE(m_rem, HADDR_UNDEF);
    EXPECT_EQ(H5MF_try_extend(f, H5FD_MEM_OHDR, m1, 480, 16), 0);
    {
      const size_t orig_thres = f->shared->pgend_meta_thres;
      absl::Cleanup restore_thres = [&]() {
        f->shared->pgend_meta_thres = orig_thres;
      };
      f->shared->pgend_meta_thres = 64;
      EXPECT_GT(H5MF_try_extend(f, H5FD_MEM_OHDR, m1, 480, 16), 0);
    }

    // Large block at EOA extended by unaligned extra_requested -> adds frag.
    const haddr_t l1 = H5MF_alloc(f, H5FD_MEM_DRAW, 1024);
    ASSERT_NE(l1, HADDR_UNDEF);
    EXPECT_GT(H5MF_try_extend(f, H5FD_MEM_DRAW, l1, 1024, 100), 0);
    EXPECT_NE(f->shared->fs_man[H5F_MEM_PAGE_GENERIC], nullptr);

    // Large block get_eoa failure path in H5MF_try_extend.
    ScopedErrorSilence silence;
    const H5FD_class_t* orig_cls = f->shared->lf->cls;
    H5FD_class_t bad_cls = *orig_cls;
    bad_cls.get_eoa = FailingGetEoa;
    absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
    f->shared->lf->cls = &bad_cls;
    EXPECT_LT(H5MF_try_extend(f, H5FD_MEM_DRAW, l1, 1024, 64), 0);
  }
}

TEST(H5MFTest, TryShrinkCoversSimpleSmallLargeAndDriverErrors) {
  // 1. Non-paged SIMPLE shrinkable vs non-shrinkable + FailingGetEoa.
  {
    ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_FSM_AGGR, false, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore(/*disable_aggr=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_shrink_simple.h5", H5F_ACC_TRUNC, fcpl.get(),
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    const haddr_t b1 = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    const haddr_t b2 = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    ASSERT_NE(b1, HADDR_UNDEF);
    ASSERT_NE(b2, HADDR_UNDEF);
    EXPECT_EQ(H5MF_try_shrink(f, H5FD_MEM_DRAW, b1, 128), 0);
    EXPECT_GT(H5MF_try_shrink(f, H5FD_MEM_DRAW, b2, 128), 0);

    ScopedErrorSilence silence;
    const H5FD_class_t* orig_cls = f->shared->lf->cls;
    H5FD_class_t bad_cls = *orig_cls;
    bad_cls.get_eoa = FailingGetEoa;
    absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
    f->shared->lf->cls = &bad_cls;
    EXPECT_LT(H5MF_try_shrink(f, H5FD_MEM_DRAW, b1, 128), 0);
  }

  // 2. Paged SMALL (no can_shrink -> 0) and LARGE (EOA shrink -> > 0).
  {
    ScopedPlist fcpl =
        MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, false, 1, 512);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("mf_shrink_paged.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    const haddr_t s1 = H5MF_alloc(f, H5FD_MEM_DRAW, 64);
    const haddr_t l1 = H5MF_alloc(f, H5FD_MEM_DRAW, 1024);
    const haddr_t l2 = H5MF_alloc(f, H5FD_MEM_DRAW, 512);
    ASSERT_NE(s1, HADDR_UNDEF);
    ASSERT_NE(l1, HADDR_UNDEF);
    ASSERT_NE(l2, HADDR_UNDEF);

    EXPECT_EQ(H5MF_try_shrink(f, H5FD_MEM_DRAW, s1, 64), 0);
    EXPECT_EQ(H5MF_try_shrink(f, H5FD_MEM_DRAW, l1, 1024), 0);
    EXPECT_GT(H5MF_try_shrink(f, H5FD_MEM_DRAW, l2, 512), 0);
  }
}

TEST(H5MFTest, AllocPagefsCoversSmallLargePageBufferAndErrors) {
  ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, false, 1, 512);
  ASSERT_TRUE(fcpl.valid());
  ScopedPlist fapl = MakeFaplWithV110AndCore();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_page_buffer_size(fapl.get(), 512 * 8, 50, 0), 0);

  ScopedFile file(
      H5Fcreate("mf_alloc_pagefs.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;

  // 1. Aligned vs unaligned large allocations (frag == 0 vs frag > 0).
  const haddr_t lg_aligned = H5MF_alloc(f, H5FD_MEM_DRAW, 1024);
  const haddr_t lg_unaligned = H5MF_alloc(f, H5FD_MEM_DRAW, 700);
  ASSERT_NE(lg_aligned, HADDR_UNDEF);
  ASSERT_NE(lg_unaligned, HADDR_UNDEF);
  EXPECT_NE(f->shared->fs_man[H5F_MEM_PAGE_GENERIC], nullptr);

  // 2. Small allocations hit H5PB_add_new_page, then freeing two halves merges
  //    to a full page and invokes H5MF_xfree.
  const haddr_t sm_meta = H5MF_alloc(f, H5FD_MEM_OHDR, 128);
  const haddr_t sm_draw_a = H5MF_alloc(f, H5FD_MEM_DRAW, 256);
  const haddr_t sm_draw_b = H5MF_alloc(f, H5FD_MEM_DRAW, 256);
  ASSERT_NE(sm_meta, HADDR_UNDEF);
  ASSERT_NE(sm_draw_a, HADDR_UNDEF);
  ASSERT_NE(sm_draw_b, HADDR_UNDEF);
  EXPECT_EQ(sm_draw_a + 256, sm_draw_b);
  EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, sm_draw_a, 256), 0);
  EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, sm_draw_b, 256), 0);

  // 3. Temporarily emulate H5FD_FEAT_PAGED_AGGR + H5FD_FLMAP_DEFAULT to
  // exercise
  //    all small and large switch cases in H5MF__alloc_pagefs.
  {
    const unsigned long orig_lf_flags = f->shared->lf->feature_flags;
    const unsigned long orig_sh_flags = f->shared->feature_flags;
    std::array<H5FD_mem_t, H5FD_MEM_NTYPES> orig_map{};
    std::memcpy(orig_map.data(), f->shared->fs_type_map, sizeof(orig_map));
    absl::Cleanup cleanup_multi = [&]() {
      H5MF_try_close(f);
      f->shared->lf->feature_flags = orig_lf_flags;
      f->shared->feature_flags = orig_sh_flags;
      std::memcpy(f->shared->fs_type_map, orig_map.data(), sizeof(orig_map));
    };

    f->shared->lf->feature_flags |= H5FD_FEAT_PAGED_AGGR;
    f->shared->feature_flags |= H5FD_FEAT_PAGED_AGGR;
    std::array<H5FD_mem_t, H5FD_MEM_NTYPES> default_map = {
        H5FD_MEM_DEFAULT, H5FD_MEM_DEFAULT, H5FD_MEM_DEFAULT, H5FD_MEM_DEFAULT,
        H5FD_MEM_DEFAULT, H5FD_MEM_DEFAULT, H5FD_MEM_DEFAULT,
    };
    std::memcpy(f->shared->fs_type_map, default_map.data(),
                sizeof(default_map));

    for (H5FD_mem_t mtype : {H5FD_MEM_BTREE, H5FD_MEM_DRAW, H5FD_MEM_GHEAP,
                             H5FD_MEM_LHEAP, H5FD_MEM_OHDR}) {
      EXPECT_NE(H5MF_alloc(f, mtype, 700), HADDR_UNDEF);
      EXPECT_NE(H5MF_alloc(f, mtype, 128), HADDR_UNDEF);
    }

    // Unrecognized H5F_MEM_PAGE_DEFAULT case returns HADDR_UNDEF cleanly.
    ScopedErrorSilence silence;
    EXPECT_EQ(H5MF_alloc(f, H5FD_MEM_DEFAULT, 128), HADDR_UNDEF);
  }

  // 4. Error paths in H5MF__alloc_pagefs: FailingSetEoa (H5F__alloc) &
  //    FailingGetEoa.
  ScopedErrorSilence silence;
  const H5FD_class_t* orig_cls = f->shared->lf->cls;
  H5FD_class_t bad_cls = *orig_cls;
  absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
  f->shared->lf->cls = &bad_cls;

  bad_cls.set_eoa = FailingSetEoa;
  EXPECT_EQ(H5MF_alloc(f, H5FD_MEM_DRAW, 1024), HADDR_UNDEF);

  bad_cls.get_eoa = FailingGetEoa;
  EXPECT_EQ(H5MF_alloc(f, H5FD_MEM_DRAW, 1024), HADDR_UNDEF);
}

TEST(H5MFTest, PersistentFsmLifecycleAndReopenNonPaged) {
  TempFile temp("mf_persist_nonpaged.h5");
  ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_FSM_AGGR, true, 1);
  ASSERT_TRUE(fcpl.valid());
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);
  ASSERT_GE(H5Pset_meta_block_size(fapl.get(), 0), 0);
  ASSERT_GE(H5Pset_small_data_block_size(fapl.get(), 0), 0);

  // 1. Create file, populate & free both RDFSM (DRAW) and MDFSM (SUPER), close.
  {
    ScopedFile file(
        H5Fcreate(temp.c_str(), H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    const haddr_t d1 = H5MF_alloc(f, H5FD_MEM_DRAW, 256);
    const haddr_t d2 = H5MF_alloc(f, H5FD_MEM_DRAW, 256);
    const haddr_t d3 = H5MF_alloc(f, H5FD_MEM_DRAW, 256);
    const haddr_t d4 = H5MF_alloc(f, H5FD_MEM_DRAW, 256);
    const haddr_t m1 = H5MF_alloc(f, H5FD_MEM_OHDR, 1024);
    const haddr_t mg1 = H5MF_alloc(f, H5FD_MEM_OHDR, 64);
    const haddr_t m3 = H5MF_alloc(f, H5FD_MEM_OHDR, 1024);
    const haddr_t mg2 = H5MF_alloc(f, H5FD_MEM_OHDR, 64);
    const haddr_t m2 = H5MF_alloc(f, H5FD_MEM_OHDR, 256);
    const haddr_t tail = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    ASSERT_NE(d1, HADDR_UNDEF);
    ASSERT_NE(d2, HADDR_UNDEF);
    ASSERT_NE(d3, HADDR_UNDEF);
    ASSERT_NE(d4, HADDR_UNDEF);
    ASSERT_NE(m1, HADDR_UNDEF);
    ASSERT_NE(mg1, HADDR_UNDEF);
    ASSERT_NE(m3, HADDR_UNDEF);
    ASSERT_NE(mg2, HADDR_UNDEF);
    ASSERT_NE(m2, HADDR_UNDEF);
    ASSERT_NE(tail, HADDR_UNDEF);

    // Free separated interior sections so both RDFSM and MDFSM persist on
    // close, plus m2 right before tail so close cascades shrink_eoa onto m2.
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, d1, 256), 0);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, d3, 256), 0);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_OHDR, m1, 1024), 0);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_OHDR, m3, 1024), 0);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_OHDR, m2, 256), 0);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, tail, 128), 0);
  }

  // 2. Reopen RW, inspect lazy-open freespace & sections, test lazy alloc/ext.
  {
    ScopedFile file(H5Fopen(temp.c_str(), H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    // Initially fs_man is null until accessed while fs_addr is defined.
    EXPECT_EQ(f->shared->fs_man[H5FD_MEM_DRAW], nullptr);
    EXPECT_NE(f->shared->fs_addr[H5FD_MEM_DRAW], HADDR_UNDEF);

    // H5Fget_freespace & H5Fget_info2 open & close closed FSMs.
    const hssize_t fs_bytes = H5Fget_freespace(file.get());
    EXPECT_GE(fs_bytes, 256);
    H5F_info2_t finfo{};
    ASSERT_GE(H5Fget_info2(file.get(), &finfo), 0);
    EXPECT_EQ(finfo.free.tot_space, static_cast<hsize_t>(fs_bytes));

    // Query free sections: count-only, partial, and full.
    const ssize_t total_sects =
        H5Fget_free_sections(file.get(), H5FD_MEM_DEFAULT, 0, nullptr);
    ASSERT_GE(total_sects, 2);
    std::vector<H5F_sect_info_t> partial(1);
    EXPECT_EQ(
        H5Fget_free_sections(file.get(), H5FD_MEM_DEFAULT, 1, partial.data()),
        total_sects);
    std::vector<H5F_sect_info_t> full(static_cast<size_t>(total_sects));
    EXPECT_EQ(H5Fget_free_sections(file.get(), H5FD_MEM_DEFAULT, full.size(),
                                   full.data()),
              total_sects);
    EXPECT_GE(H5Fget_free_sections(file.get(), H5FD_MEM_DRAW, full.size(),
                                   full.data()),
              1);

    // Null output pointers in H5MF_get_freespace succeed cleanly.
    {
      ScopedApiContext ctx;
      EXPECT_GE(H5MF_get_freespace(f, nullptr, nullptr), 0);
      // Lazy-open via H5MF_try_extend into existing DRAW free section.
      EXPECT_EQ(f->shared->fs_man[H5FD_MEM_DRAW], nullptr);
      EXPECT_EQ(H5MF_try_extend(f, H5FD_MEM_DRAW, 1, 1, 1), 0);
      EXPECT_NE(f->shared->fs_man[H5FD_MEM_DRAW], nullptr);

      // Close FSM manually and verify H5MF_alloc lazy-opens fs_addr[SUPER].
      if (!f->shared->fs_man[H5FD_MEM_SUPER] &&
          H5_addr_defined(f->shared->fs_addr[H5FD_MEM_SUPER])) {
        EXPECT_NE(H5MF_alloc(f, H5FD_MEM_OHDR, 64), HADDR_UNDEF);
      }
    }

    // Downgrade format via H5Fformat_convert -> invokes H5MF_try_close.
    EXPECT_GE(H5Fformat_convert(file.get()), 0);
    EXPECT_FALSE(f->shared->fs_persist);
  }
}

TEST(H5MFTest, PersistentAndNonPersistentPagedLifecycleAndReopen) {
  TempFile temp_persist("mf_persist_paged.h5");
  ScopedPlist fcpl_persist =
      MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, true, 1, 512);
  ASSERT_TRUE(fcpl_persist.valid());
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);
  ASSERT_GE(H5Pset_page_buffer_size(fapl.get(), 512 * 8, 50, 0), 0);

  // 1. Persistent PAGE strategy: populate small & large FSMs, flush & close.
  {
    ScopedFile file(H5Fcreate(temp_persist.c_str(), H5F_ACC_TRUNC,
                              fcpl_persist.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    const haddr_t sm_meta = H5MF_alloc(f, H5FD_MEM_OHDR, 128);
    const haddr_t sm_draw = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    const haddr_t lg1 = H5MF_alloc(f, H5FD_MEM_DRAW, 1024);
    const haddr_t lg2 = H5MF_alloc(f, H5FD_MEM_DRAW, 1024);
    ASSERT_NE(sm_meta, HADDR_UNDEF);
    ASSERT_NE(sm_draw, HADDR_UNDEF);
    ASSERT_NE(lg1, HADDR_UNDEF);
    ASSERT_NE(lg2, HADDR_UNDEF);
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, lg1, 1024), 0);
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_OHDR, sm_meta, 64), 0);
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, sm_draw, 64), 0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  }

  // 2. Reopen RW, exercise H5MF_get_freespace, H5MF_get_free_sections,
  //    re-settle on flush, and H5MF_try_close for paged aggregation.
  {
    ScopedFile file(H5Fopen(temp_persist.c_str(), H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    EXPECT_GT(H5Fget_freespace(file.get()), 0);

    const ssize_t n_all =
        H5Fget_free_sections(file.get(), H5FD_MEM_DEFAULT, 0, nullptr);
    ASSERT_GE(n_all, 1);
    std::vector<H5F_sect_info_t> sects(static_cast<size_t>(n_all));
    EXPECT_EQ(H5Fget_free_sections(file.get(), H5FD_MEM_DEFAULT, sects.size(),
                                   sects.data()),
              n_all);
    EXPECT_GE(H5Fget_free_sections(file.get(), H5FD_MEM_SUPER, 0, nullptr), 0);
    EXPECT_GE(H5Fget_free_sections(file.get(), H5FD_MEM_DRAW, 0, nullptr), 0);

    // Modify file so settle runs again on close, reopening closed FSMs in pass.
    {
      ScopedApiContext ctx;
      const haddr_t more = H5MF_alloc(f, H5FD_MEM_DRAW, 64);
      ASSERT_NE(more, HADDR_UNDEF);
    }
  }

  // 2b. Reopen RW and downgrade PAGE format via H5Fformat_convert -> invokes
  //     H5MF_try_close(f) for paged aggregation cleanly.
  {
    ScopedFile file(H5Fopen(temp_persist.c_str(), H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    EXPECT_GE(H5Fformat_convert(file.get()), 0);
    EXPECT_FALSE(f->shared->fs_persist);
  }

  // 3. Non-persistent PAGE strategy closing (deletes active FSMs).
  {
    ScopedPlist fcpl_np =
        MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, false, 1, 512);
    ASSERT_TRUE(fcpl_np.valid());
    ScopedPlist fapl_core = MakeFaplWithV110AndCore();
    ASSERT_TRUE(fapl_core.valid());
    ScopedFile file(H5Fcreate("mf_nonpersist_paged.h5", H5F_ACC_TRUNC,
                              fcpl_np.get(), fapl_core.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;
    const haddr_t lg1 = H5MF_alloc(f, H5FD_MEM_DRAW, 700);
    const haddr_t sm1 = H5MF_alloc(f, H5FD_MEM_DRAW, 64);
    ASSERT_NE(lg1, HADDR_UNDEF);
    ASSERT_NE(sm1, HADDR_UNDEF);
  }

  // 4. Earliest superblock (< v2) FSM_AGGR closing exercises non-persistent
  //    super_vers < 2 path in H5MF__close_aggrfs.
  {
    ScopedFile file = CreateInMemoryFile("mf_earliest_v0.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;
    const haddr_t b1 = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    const haddr_t b2 = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    ASSERT_NE(b1, HADDR_UNDEF);
    ASSERT_NE(b2, HADDR_UNDEF);
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, b1, 128), 0);
  }
}

TEST(H5MFTest, MultiAndSplitVfdUseSeparateAndDichotomyMaps) {
  TempDir dir("mf_multi_split");

  // 1. Split VFD (dichotomy map via public API).
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_split(fapl.get(), "-m.h5", H5P_DEFAULT, "-r.h5",
                                H5P_DEFAULT),
              0);
    const std::string base = dir.child("split_test").string();
    ScopedFile file(
        H5Fcreate(base.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedGroup grp(
        H5Gcreate2(file.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    EXPECT_GE(H5Fget_freespace(file.get()), 0);
  }

  // 2. Multi VFD with default separate member maps (H5FD_FLMAP_DEFAULT).
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_fapl_multi(fapl.get(), nullptr, nullptr, nullptr, nullptr, true),
        0);
    const std::string base = dir.child("multi_base").string();
    ScopedFile file(
        H5Fcreate(base.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(
        H5Gcreate2(file.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    EXPECT_GE(H5Fget_freespace(file.get()), 0);
  }
}

TEST(H5MFTest, SettleHandlesBypassDistinctHdrSinfoAndEmptyRawFsm) {
  // 1. Non-persistent bypass and null_fsm_addr bypass in H5MF_settle_*.
  {
    ScopedFile file = CreateInMemoryFile("mf_settle_bypass.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    bool settled = false;
    ASSERT_GE(H5MF_settle_raw_data_fsm(f, &settled), 0);
    EXPECT_FALSE(settled);
    ASSERT_GE(H5MF_settle_meta_data_fsm(f, &settled), 0);
    EXPECT_FALSE(settled);
  }

  // 2. Persistent FSM_AGGR with null_fsm_addr=true skips settling; then with
  //    separate OHDR/LHEAP mapping and empty DRAW FSM exercises distinct
  //    sm_hdr_fspace != sm_sinfo_fspace and empty RDFSM branch.
  {
    ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_FSM_AGGR, true, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore(/*disable_aggr=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_settle_distinct.h5", H5F_ACC_TRUNC,
                              fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    const bool orig_null_fsm = f->shared->null_fsm_addr;
    std::array<H5FD_mem_t, H5FD_MEM_NTYPES> orig_map{};
    std::memcpy(orig_map.data(), f->shared->fs_type_map, sizeof(orig_map));
    absl::Cleanup restore = [&]() {
      f->shared->null_fsm_addr = orig_null_fsm;
      H5MF_try_close(f);
      f->shared->fs_persist = false;
      std::memcpy(f->shared->fs_type_map, orig_map.data(), sizeof(orig_map));
    };

    f->shared->null_fsm_addr = true;
    bool settled = false;
    ASSERT_GE(H5MF_settle_raw_data_fsm(f, &settled), 0);
    EXPECT_FALSE(settled);
    ASSERT_GE(H5MF_settle_meta_data_fsm(f, &settled), 0);
    EXPECT_FALSE(settled);
    f->shared->null_fsm_addr = false;

    // Give OHDR (FSPACE_HDR) and LHEAP (FSPACE_SINFO) distinct FSMs, start both
    // plus an empty DRAW FSM so empty RDFSM and distinct sinfo paths execute.
    f->shared->fs_type_map[H5FD_MEM_OHDR] = H5FD_MEM_OHDR;
    f->shared->fs_type_map[H5FD_MEM_LHEAP] = H5FD_MEM_LHEAP;
    const haddr_t h_blk = H5MF_alloc(f, H5FD_MEM_OHDR, 64);
    const haddr_t l_blk = H5MF_alloc(f, H5FD_MEM_LHEAP, 64);
    const haddr_t guard = H5MF_alloc(f, H5FD_MEM_DRAW, 64);
    ASSERT_NE(h_blk, HADDR_UNDEF);
    ASSERT_NE(l_blk, HADDR_UNDEF);
    ASSERT_NE(guard, HADDR_UNDEF);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_OHDR, h_blk, 64), 0);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_LHEAP, l_blk, 64), 0);
    ASSERT_GE(H5MF__start_fstype(f, H5F_MEM_PAGE_DRAW), 0);

    bool raw_settled = false;
    bool meta_settled = false;
    ASSERT_GE(H5MF_settle_raw_data_fsm(f, &raw_settled), 0);
    EXPECT_TRUE(raw_settled);
    ASSERT_GE(H5MF_settle_meta_data_fsm(f, &meta_settled), 0);
    EXPECT_TRUE(meta_settled);
  }
}

TEST(H5MFTest, CorruptFsmAddressesAndVfdFailuresReturnErrorsCleanly) {
  ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_FSM_AGGR, false, 1);
  ASSERT_TRUE(fcpl.valid());
  ScopedPlist fapl = MakeFaplWithV110AndCore(/*disable_aggr=*/true);
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(
      H5Fcreate("mf_errors.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ScopedErrorSilence silence;

  const haddr_t blk = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
  const haddr_t guard = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
  ASSERT_NE(blk, HADDR_UNDEF);
  ASSERT_NE(guard, HADDR_UNDEF);

  // 1. Corrupt fs_addr[DRAW] to unmapped file address -> H5FS_open fails
  // cleanly.
  {
    const haddr_t orig_fs_addr = f->shared->fs_addr[H5FD_MEM_DRAW];
    const H5F_fs_state_t orig_state = f->shared->fs_state[H5FD_MEM_DRAW];
    absl::Cleanup restore_fs = [&]() {
      f->shared->fs_addr[H5FD_MEM_DRAW] = orig_fs_addr;
      f->shared->fs_state[H5FD_MEM_DRAW] = orig_state;
    };
    const haddr_t bad_addr = H5F_get_eoa(f, H5FD_MEM_DRAW) + 4096;
    f->shared->fs_addr[H5FD_MEM_DRAW] = bad_addr;
    f->shared->fs_state[H5FD_MEM_DRAW] = H5F_FS_STATE_CLOSED;

    EXPECT_LT(H5MF__open_fstype(f, H5F_MEM_PAGE_DRAW), 0);
    EXPECT_LT(H5MF__start_fstype(f, H5F_MEM_PAGE_DRAW), 0);
    EXPECT_EQ(H5MF_alloc(f, H5FD_MEM_DRAW, 64), HADDR_UNDEF);
    EXPECT_LT(H5MF_xfree(f, H5FD_MEM_DRAW, blk, 128), 0);
    EXPECT_LT(H5MF_try_extend(f, H5FD_MEM_DRAW, blk, 128, 64), 0);
    hsize_t tot = 0;
    hsize_t meta = 0;
    EXPECT_LT(H5MF_get_freespace(f, &tot, &meta), 0);
    size_t scnt = 0;
    EXPECT_LT(H5MF_get_free_sections(f, H5FD_MEM_DRAW, 0, nullptr, &scnt), 0);
    EXPECT_LT(H5MF_close(f), 0);
  }

  // 2. Failing get_eoa / set_eoa inside H5MF_get_freespace & H5MF_try_close.
  {
    const H5FD_class_t* orig_cls = f->shared->lf->cls;
    H5FD_class_t bad_cls = *orig_cls;
    bad_cls.get_eoa = FailingGetEoa;
    bad_cls.set_eoa = FailingSetEoa;
    absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
    f->shared->lf->cls = &bad_cls;

    hsize_t tot = 0;
    EXPECT_LT(H5MF_get_freespace(f, &tot, nullptr), 0);
  }
}

TEST(H5MFTest, CloseShrinkEoaPagedCascadeAndErrorPaths) {
  // 1. Paged EOA cascade on close: freeing trailing small page puts a prior
  //    GENERIC section at EOA so H5MF__close_shrink_eoa sets eoa_shrank = true.
  {
    ScopedPlist fcpl =
        MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, false, 1, 512);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_close_paged_cascade.h5", H5F_ACC_TRUNC,
                              fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    {
      ScopedApiContext ctx;
      const haddr_t lg1 = H5MF_alloc(f, H5FD_MEM_DRAW, 512);
      const haddr_t sm1 = H5MF_alloc(f, H5FD_MEM_DRAW, 256);
      const haddr_t sm2 = H5MF_alloc(f, H5FD_MEM_DRAW, 256);
      ASSERT_NE(lg1, HADDR_UNDEF);
      ASSERT_NE(sm1, HADDR_UNDEF);
      ASSERT_NE(sm2, HADDR_UNDEF);
      EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, lg1, 512), 0);
      EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, sm1, 256), 0);
      EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, sm2, 256), 0);
    }
    EXPECT_GE(H5Fclose(file.release()), 0);
  }

  // 2. Paged H5MF_close error via FailingGetEoa in H5MF__close_shrink_eoa.
  {
    ScopedPlist fcpl =
        MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, false, 1, 512);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_close_paged_err.h5", H5F_ACC_TRUNC,
                              fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    const haddr_t lg1 = H5MF_alloc(f, H5FD_MEM_DRAW, 512);
    const haddr_t lg2 = H5MF_alloc(f, H5FD_MEM_DRAW, 512);
    ASSERT_NE(lg1, HADDR_UNDEF);
    ASSERT_NE(lg2, HADDR_UNDEF);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, lg1, 512), 0);

    ScopedErrorSilence silence;
    const H5FD_class_t* orig_cls = f->shared->lf->cls;
    H5FD_class_t bad_cls = *orig_cls;
    bad_cls.get_eoa = FailingGetEoa;
    absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
    f->shared->lf->cls = &bad_cls;

    EXPECT_LT(H5MF_close(f), 0);
  }

  // 3. Non-paged H5MF_close error via active meta_aggr in H5MF_free_aggrs.
  {
    ScopedFile file = CreateInMemoryFile("mf_close_aggr_err.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    ASSERT_NE(H5MF_alloc(f, H5FD_MEM_OHDR, 64), HADDR_UNDEF);
    ASSERT_GT(f->shared->meta_aggr.size, 0U);

    ScopedErrorSilence silence;
    const H5FD_class_t* orig_cls = f->shared->lf->cls;
    H5FD_class_t bad_cls = *orig_cls;
    bad_cls.get_eoa = FailingGetEoa;
    absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
    f->shared->lf->cls = &bad_cls;

    EXPECT_LT(H5MF_close(f), 0);
  }

  // 4. Non-paged H5MF_close error in H5MF__close_shrink_eoa with open FSM.
  {
    ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_FSM_AGGR, false, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore(/*disable_aggr=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_close_shrink_err.h5", H5F_ACC_TRUNC,
                              fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    const haddr_t d1 = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    const haddr_t d2 = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    ASSERT_NE(d1, HADDR_UNDEF);
    ASSERT_NE(d2, HADDR_UNDEF);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, d1, 128), 0);

    ScopedErrorSilence silence;
    const H5FD_class_t* orig_cls = f->shared->lf->cls;
    H5FD_class_t bad_cls = *orig_cls;
    bad_cls.get_eoa = FailingGetEoa;
    absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
    f->shared->lf->cls = &bad_cls;

    EXPECT_LT(H5MF_close(f), 0);
  }
}

TEST(H5MFTest, DriverErrorsInAllocXfreeTryExtendAndTryShrink) {
  // 1. Non-paged H5MF_alloc / H5MF_xfree / H5MF_try_extend / H5MF_try_shrink.
  {
    ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_FSM_AGGR, false, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore(/*disable_aggr=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_nonpaged_drv_err.h5", H5F_ACC_TRUNC,
                              fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    const haddr_t blk = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    const haddr_t meta_blk = H5MF_alloc(f, H5FD_MEM_OHDR, 128);
    ASSERT_NE(blk, HADDR_UNDEF);
    ASSERT_NE(meta_blk, HADDR_UNDEF);
    // Write metadata to activate dirty metadata accumulator at meta_blk.
    std::array<uint8_t, 128> mdata{};
    mdata.fill(0x5a);
    ASSERT_GE(
        H5F_block_write(f, H5FD_MEM_OHDR, meta_blk, mdata.size(), mdata.data()),
        0);

    ScopedErrorSilence silence;
    const H5FD_class_t* orig_cls = f->shared->lf->cls;
    H5FD_class_t bad_cls = *orig_cls;
    absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
    f->shared->lf->cls = &bad_cls;

    // set_eoa failure triggers H5MF_aggr_vfd_alloc & sect_cls->shrink errors.
    bad_cls.set_eoa = FailingSetEoa;
    EXPECT_EQ(H5MF_alloc(f, H5FD_MEM_DRAW, 128), HADDR_UNDEF);
    const haddr_t tail = H5MF_alloc(f, H5FD_MEM_OHDR, 64);
    if (tail == HADDR_UNDEF) {
      // Allocate tail using orig_cls, then fail shrink using FailingSetEoa.
      f->shared->lf->cls = orig_cls;
      const haddr_t real_tail = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
      ASSERT_NE(real_tail, HADDR_UNDEF);
      f->shared->lf->cls = &bad_cls;
      EXPECT_LT(H5MF_try_shrink(f, H5FD_MEM_DRAW, real_tail, 128), 0);
    }

    // write failure inside dirty metadata accumulator fails H5MF_xfree.
    if ((f->shared->feature_flags & H5FD_FEAT_ACCUMULATE_METADATA) &&
        f->shared->accum.dirty && H5_addr_defined(f->shared->accum.loc) &&
        f->shared->accum.dirty_len >= 64) {
      bad_cls.write = FailingWrite;
      const haddr_t hole =
          f->shared->accum.loc + f->shared->accum.dirty_off + 16;
      EXPECT_LT(H5MF_xfree(f, H5FD_MEM_OHDR, hole, 16), 0);
      bad_cls.write = orig_cls->write;
    }

    // get_eoa failure triggers H5MF_xfree try_shrink & H5F__try_extend errors.
    bad_cls.set_eoa = orig_cls->set_eoa;
    bad_cls.get_eoa = FailingGetEoa;
    EXPECT_LT(H5MF_xfree(f, H5FD_MEM_DRAW, blk, 128), 0);
    EXPECT_LT(H5MF_try_extend(f, H5FD_MEM_DRAW, blk, 128, 64), 0);
  }

  // 2. Aggregator extension failure in H5MF_try_extend (H5MF__aggr_try_extend).
  {
    ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_FSM_AGGR, false, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_aggr_extend_err.h5", H5F_ACC_TRUNC,
                              fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    const haddr_t g1 = H5MF_alloc(f, H5FD_MEM_GHEAP, 64);
    ASSERT_NE(g1, HADDR_UNDEF);
    ASSERT_GT(f->shared->sdata_aggr.size, 0U);

    ScopedErrorSilence silence;
    const H5FD_class_t* orig_cls = f->shared->lf->cls;
    H5FD_class_t bad_cls = *orig_cls;
    bad_cls.set_eoa = FailingSetEoa;
    absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
    f->shared->lf->cls = &bad_cls;

    // g1 sits before sdata_aggr; extra > 0.1 * aggr.size tries H5F__try_extend
    // on sdata_aggr at EOA and fails cleanly in H5MF__aggr_try_extend.
    EXPECT_LT(H5MF_try_extend(f, H5FD_MEM_GHEAP, g1, 64, 512), 0);
  }

  // 3. Paged H5MF_try_extend large-fragment H5MF__start_fstype failure.
  {
    ScopedPlist fcpl =
        MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, false, 1, 512);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_paged_extend_start_err.h5", H5F_ACC_TRUNC,
                              fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    const haddr_t lg1 = H5MF_alloc(f, H5FD_MEM_DRAW, 512);
    ASSERT_NE(lg1, HADDR_UNDEF);
    ASSERT_EQ(f->shared->fs_man[H5F_MEM_PAGE_GENERIC], nullptr);

    ScopedErrorSilence silence;
    const haddr_t orig_addr = f->shared->fs_addr[H5F_MEM_PAGE_GENERIC];
    const H5F_fs_state_t orig_st = f->shared->fs_state[H5F_MEM_PAGE_GENERIC];
    absl::Cleanup restore = [&]() {
      f->shared->fs_addr[H5F_MEM_PAGE_GENERIC] = orig_addr;
      f->shared->fs_state[H5F_MEM_PAGE_GENERIC] = orig_st;
    };
    f->shared->fs_addr[H5F_MEM_PAGE_GENERIC] =
        H5F_get_eoa(f, H5FD_MEM_DRAW) + 4096;
    f->shared->fs_state[H5F_MEM_PAGE_GENERIC] = H5F_FS_STATE_CLOSED;
    EXPECT_LT(H5MF_try_extend(f, H5FD_MEM_DRAW, lg1, 512, 100), 0);
  }
}

TEST(H5MFTest, SettleMetaAndRawFsmErrorsAndContinueAllocLoops) {
  // 1. Non-paged persistent settle_raw / settle_meta error branches.
  {
    ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_FSM_AGGR, true, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore(/*disable_aggr=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_settle_err_nonpaged.h5", H5F_ACC_TRUNC,
                              fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;
    ScopedErrorSilence silence;

    bool settled = false;
    // Empty file + FailingGetEoa -> settle_meta hits final H5FD_get_eoa error.
    {
      const H5FD_class_t* orig_cls = f->shared->lf->cls;
      H5FD_class_t bad_cls = *orig_cls;
      bad_cls.get_eoa = FailingGetEoa;
      absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
      f->shared->lf->cls = &bad_cls;
      EXPECT_LT(H5MF_settle_meta_data_fsm(f, &settled), 0);
    }

    // Defined fs_addr for small hdr / sinfo triggers H5E_BADVALUE in
    // settle_meta.
    {
      std::array<H5FD_mem_t, H5FD_MEM_NTYPES> orig_map{};
      std::memcpy(orig_map.data(), f->shared->fs_type_map, sizeof(orig_map));
      const haddr_t orig_ohdr = f->shared->fs_addr[H5FD_MEM_OHDR];
      const haddr_t orig_lh = f->shared->fs_addr[H5FD_MEM_LHEAP];
      absl::Cleanup restore = [&]() {
        f->shared->fs_addr[H5FD_MEM_OHDR] = orig_ohdr;
        f->shared->fs_addr[H5FD_MEM_LHEAP] = orig_lh;
        std::memcpy(f->shared->fs_type_map, orig_map.data(), sizeof(orig_map));
      };
      f->shared->fs_type_map[H5FD_MEM_OHDR] = H5FD_MEM_OHDR;
      f->shared->fs_type_map[H5FD_MEM_LHEAP] = H5FD_MEM_LHEAP;
      f->shared->fs_addr[H5FD_MEM_OHDR] = 1024;
      EXPECT_LT(H5MF_settle_meta_data_fsm(f, &settled), 0);
      f->shared->fs_addr[H5FD_MEM_OHDR] = HADDR_UNDEF;
      f->shared->fs_addr[H5FD_MEM_LHEAP] = 1024;
      EXPECT_LT(H5MF_settle_meta_data_fsm(f, &settled), 0);
    }

    // Corrupt fs_addr[DRAW] with closed fs_man -> settle_raw hits open_fstype
    // err.
    {
      const haddr_t orig_addr = f->shared->fs_addr[H5FD_MEM_DRAW];
      absl::Cleanup restore = [&]() {
        f->shared->fs_addr[H5FD_MEM_DRAW] = orig_addr;
      };
      f->shared->fs_addr[H5FD_MEM_DRAW] = H5F_get_eoa(f, H5FD_MEM_DRAW) + 4096;
      EXPECT_LT(H5MF_settle_raw_data_fsm(f, &settled), 0);
    }

    // Active DRAW section (non-EOA) -> FailingGetEoa hits close_shrink_eoa err;
    // FailingSetEoa hits H5FS_alloc_hdr error in settle_raw_data_fsm.
    const haddr_t d1 = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    const haddr_t d2 = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    ASSERT_NE(d1, HADDR_UNDEF);
    ASSERT_NE(d2, HADDR_UNDEF);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, d1, 128), 0);
    {
      const H5FD_class_t* orig_cls = f->shared->lf->cls;
      H5FD_class_t bad_cls = *orig_cls;
      absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
      f->shared->lf->cls = &bad_cls;

      bad_cls.get_eoa = FailingGetEoa;
      EXPECT_LT(H5MF_settle_raw_data_fsm(f, &settled), 0);
      EXPECT_LT(H5MF_settle_meta_data_fsm(f, &settled), 0);

      bad_cls.get_eoa = orig_cls->get_eoa;
      bad_cls.set_eoa = FailingSetEoa;
      EXPECT_LT(H5MF_settle_raw_data_fsm(f, &settled), 0);
    }
  }

  // 2. Active meta_aggr error in settle_raw_data_fsm / settle_meta_data_fsm.
  {
    ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_FSM_AGGR, true, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore(/*disable_aggr=*/false);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_settle_aggr_err.h5", H5F_ACC_TRUNC,
                              fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;
    ASSERT_NE(H5MF_alloc(f, H5FD_MEM_OHDR, 64), HADDR_UNDEF);
    ASSERT_GT(f->shared->meta_aggr.size, 0U);

    ScopedErrorSilence silence;
    const H5FD_class_t* orig_cls = f->shared->lf->cls;
    H5FD_class_t bad_cls = *orig_cls;
    bad_cls.get_eoa = FailingGetEoa;
    absl::Cleanup restore_cls = [&]() { f->shared->lf->cls = orig_cls; };
    f->shared->lf->cls = &bad_cls;

    bool settled = false;
    EXPECT_LT(H5MF_settle_raw_data_fsm(f, &settled), 0);
    EXPECT_LT(H5MF_settle_meta_data_fsm(f, &settled), 0);
  }

  // 3. Persistent PAGE H5MF__continue_alloc_fsm retry on sm_hdr_fspace.
  {
    ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, true, 1, 512);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_settle_continue_sm_hdr.h5", H5F_ACC_TRUNC,
                              fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    // Drain any existing small META section so we control exact leftover size.
    size_t nsects = 0;
    ASSERT_GE(H5MF_get_free_sections(f, H5FD_MEM_OHDR, 0, nullptr, &nsects), 0);
    if (nsects > 0) {
      std::vector<H5F_sect_info_t> sects(nsects);
      ASSERT_GE(H5MF_get_free_sections(f, H5FD_MEM_OHDR, nsects, sects.data(),
                                       &nsects),
                0);
      for (const H5F_sect_info_t& s : sects) {
        if (s.size < 512) {
          ASSERT_NE(H5MF_alloc(f, H5FD_MEM_OHDR, s.size), HADDR_UNDEF);
        }
      }
    }

    // Allocate from a fresh 512B META page leaving exactly H5FS_HEADER_SIZE + 8
    // free bytes: hdr consumes H5FS_HEADER_SIZE leaving 8B, then sinfo alloc
    // needs another page -> increases sect_size -> triggers retry loop!
    const hsize_t hdr_sz = H5FS_HEADER_SIZE(f);
    const hsize_t leftover = hdr_sz + 8;
    ASSERT_LT(leftover, 512U);
    ASSERT_NE(H5MF_alloc(f, H5FD_MEM_OHDR, 512 - leftover), HADDR_UNDEF);

    bool raw_settled = false;
    bool meta_settled = false;
    ASSERT_GE(H5MF_settle_raw_data_fsm(f, &raw_settled), 0);
    EXPECT_TRUE(raw_settled);
    ASSERT_GE(H5MF_settle_meta_data_fsm(f, &meta_settled), 0);
    EXPECT_TRUE(meta_settled);
  }

  // 4. Persistent PAGE with H5FD_FEAT_PAGED_AGGR & distinct OHDR/LHEAP:
  //    covers lg_fshdr/lg_fssinfo fs_addr errors and lg_sinfo settle.
  {
    ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, true, 1, 512);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_settle_paged_large_distinct.h5",
                              H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    f->shared->lf->feature_flags |= H5FD_FEAT_PAGED_AGGR;
    f->shared->feature_flags |= H5FD_FEAT_PAGED_AGGR;
    f->shared->fs_type_map[H5FD_MEM_OHDR] = H5FD_MEM_OHDR;
    f->shared->fs_type_map[H5FD_MEM_LHEAP] = H5FD_MEM_LHEAP;

    // Exercise lg_fshdr / lg_fssinfo BADVALUE error guards first.
    {
      ScopedErrorSilence silence;
      bool settled = false;
      f->shared->fs_addr[H5F_MEM_PAGE_LARGE_OHDR] = 512;
      EXPECT_LT(H5MF_settle_meta_data_fsm(f, &settled), 0);
      f->shared->fs_addr[H5F_MEM_PAGE_LARGE_OHDR] = HADDR_UNDEF;
      f->shared->fs_addr[H5F_MEM_PAGE_LARGE_LHEAP] = 512;
      EXPECT_LT(H5MF_settle_meta_data_fsm(f, &settled), 0);
      f->shared->fs_addr[H5F_MEM_PAGE_LARGE_LHEAP] = HADDR_UNDEF;
    }

    const haddr_t lg_oh = H5MF_alloc(f, H5FD_MEM_OHDR, 512);
    const haddr_t lg_lh = H5MF_alloc(f, H5FD_MEM_LHEAP, 512);
    const haddr_t sm_oh1 = H5MF_alloc(f, H5FD_MEM_OHDR, 256);
    const haddr_t sm_lh1 = H5MF_alloc(f, H5FD_MEM_LHEAP, 256);
    const haddr_t guard = H5MF_alloc(f, H5FD_MEM_DRAW, 512);
    ASSERT_NE(lg_oh, HADDR_UNDEF);
    ASSERT_NE(lg_lh, HADDR_UNDEF);
    ASSERT_NE(sm_oh1, HADDR_UNDEF);
    ASSERT_NE(sm_lh1, HADDR_UNDEF);
    ASSERT_NE(guard, HADDR_UNDEF);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_OHDR, lg_oh, 512), 0);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_LHEAP, lg_lh, 512), 0);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_OHDR, sm_oh1, 256), 0);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_LHEAP, sm_lh1, 256), 0);

    bool raw_settled = false;
    bool meta_settled = false;
    ASSERT_GE(H5MF_settle_raw_data_fsm(f, &raw_settled), 0);
    EXPECT_TRUE(raw_settled);
    ASSERT_GE(H5MF_settle_meta_data_fsm(f, &meta_settled), 0);
    EXPECT_TRUE(meta_settled);
  }

  // 5. Persistent PAGE with distinct OHDR/LHEAP where small LHEAP leaves
  //    sinfo non-null after pass 1 so H5MF__continue_alloc_fsm checks the
  //    distinct sm_sinfo_fspace branch.
  {
    ScopedPlist fcpl = MakeStrategyFcpl(H5F_FSPACE_STRATEGY_PAGE, true, 1, 512);
    ASSERT_TRUE(fcpl.valid());
    ScopedPlist fapl = MakeFaplWithV110AndCore();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("mf_settle_continue_sm_sinfo.h5", H5F_ACC_TRUNC,
                              fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;

    f->shared->fs_type_map[H5FD_MEM_OHDR] = H5FD_MEM_OHDR;
    f->shared->fs_type_map[H5FD_MEM_LHEAP] = H5FD_MEM_LHEAP;

    const haddr_t sm_oh1 = H5MF_alloc(f, H5FD_MEM_OHDR, 256);
    const haddr_t sm_lh_keep = H5MF_alloc(f, H5FD_MEM_LHEAP, 512 - 8);
    const haddr_t guard = H5MF_alloc(f, H5FD_MEM_DRAW, 512);
    ASSERT_NE(sm_oh1, HADDR_UNDEF);
    ASSERT_NE(sm_lh_keep, HADDR_UNDEF);
    ASSERT_NE(guard, HADDR_UNDEF);
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_OHDR, sm_oh1, 256), 0);

    bool raw_settled = false;
    bool meta_settled = false;
    ASSERT_GE(H5MF_settle_raw_data_fsm(f, &raw_settled), 0);
    EXPECT_TRUE(raw_settled);
    ASSERT_GE(H5MF_settle_meta_data_fsm(f, &meta_settled), 0);
    EXPECT_TRUE(meta_settled);

    // Replenish LHEAP (guarded before `guard`) so H5Fclose() cleanly re-settles
    // LHEAP in one pass inside H5MF_settle_meta_data_fsm with no leak.
    ASSERT_GE(H5MF_xfree(f, H5FD_MEM_LHEAP, sm_lh_keep, 512 - 8), 0);
  }
}

}  // namespace
}  // namespace hdf5
