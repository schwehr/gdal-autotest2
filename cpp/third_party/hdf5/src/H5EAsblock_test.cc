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
// Tests extensible array super block allocation, unpaged and paged layouts,
// creation, cache protection/unprotection, SWMR proxy attachment, deletion, and
// error cleanup paths (third_party/hdf5/src/H5EAsblock.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5EAsblock.c

#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5C_cache_entry_t
// (first member of H5EA_hdr_t / H5EA_iblock_t / H5EA_sblock_t). Define NDEBUG
// around internal headers so C++ struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5EASBLOCK_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5EA_FRIEND
#define H5EA_TESTING
#define H5F_FRIEND
#pragma push_macro("try")
#undef try
#define try h5_try_param
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5EApkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5EASBLOCK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5EASBLOCK_TEST_RESTORE_UNDEF_NDEBUG
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

class ScopedEa {
 public:
  ScopedEa() = default;
  explicit ScopedEa(H5EA_t* ea) : ea_(ea) {}
  ~ScopedEa() { reset(); }

  ScopedEa(const ScopedEa&) = delete;
  ScopedEa& operator=(const ScopedEa&) = delete;

  void reset(H5EA_t* next = nullptr) {
    if (ea_ != nullptr) {
      H5EA_close(ea_);
    }
    ea_ = next;
  }

  H5EA_t* get() const { return ea_; }

 private:
  H5EA_t* ea_ = nullptr;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5EA_create_t MakeTestCreateParams() {
  H5EA_create_t cparam{};
  cparam.cls = H5EA_CLS_TEST;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_nelmts_bits = 10;
  cparam.idx_blk_elmts = 4;
  cparam.data_blk_min_elmts = 4;
  cparam.sup_blk_min_data_ptrs = 4;
  cparam.max_dblk_page_nelmts_bits = 4;  // 16 elements/page.
  return cparam;
}

ScopedFile CreateSwmrTempFile(const TempFile& tmp_file) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST) <
      0) {
    return ScopedFile();
  }
  ScopedFile file(
      H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  if (!file.valid()) {
    return ScopedFile();
  }
  if (H5Fstart_swmr_write(file.get()) < 0) {
    return ScopedFile();
  }
  return file;
}

TEST(H5EAsblockTest, AllocCreateProtectUnprotectAndDestUnpagedAndPaged) {
  ScopedFile file = CreateInMemoryFile("h5easblock_variants.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5EA_create_t cparam = MakeTestCreateParams();
  haddr_t hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
  ASSERT_TRUE(H5_addr_defined(hdr_addr));
  H5EA_hdr_t* hdr = H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);

  bool stats_changed = false;
  hdr->idx_blk_addr = H5EA__iblock_create(hdr, &stats_changed);
  ASSERT_TRUE(H5_addr_defined(hdr->idx_blk_addr));
  H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(iblock, nullptr);

  // sblk_idx = 4 -> unpaged (dblk_nelmts=16 <= dblk_page_nelmts=16).
  // sblk_idx = 5 -> paged (dblk_nelmts=32 > dblk_page_nelmts=16, 2 pages).
  H5EA_sblock_t* scratch_unpaged = H5EA__sblock_alloc(hdr, iblock, 4);
  ASSERT_NE(scratch_unpaged, nullptr);
  EXPECT_EQ(scratch_unpaged->dblk_npages, 0u);
  EXPECT_EQ(scratch_unpaged->page_init, nullptr);
  EXPECT_GE(H5EA__sblock_dest(scratch_unpaged), 0);

  H5EA_sblock_t* scratch_paged = H5EA__sblock_alloc(hdr, iblock, 5);
  ASSERT_NE(scratch_paged, nullptr);
  EXPECT_EQ(scratch_paged->dblk_npages, 2u);
  EXPECT_NE(scratch_paged->page_init, nullptr);
  EXPECT_GE(H5EA__sblock_dest(scratch_paged), 0);

  // Create empty unpaged sblock (sblk_idx = 4) and delete it directly.
  haddr_t sb4_addr = H5EA__sblock_create(hdr, iblock, &stats_changed, 4);
  ASSERT_TRUE(H5_addr_defined(sb4_addr));
  H5EA_sblock_t* sb4 =
      H5EA__sblock_protect(hdr, iblock, sb4_addr, 4, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(sb4, nullptr);
  EXPECT_EQ(sb4->dblk_npages, 0u);
  EXPECT_GE(H5EA__sblock_unprotect(sb4, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5EA__sblock_delete(hdr, iblock, sb4_addr, 4), 0);

  // Create paged sblock (sblk_idx = 5), protect read-write, and delete directly.
  haddr_t sb5_addr = H5EA__sblock_create(hdr, iblock, &stats_changed, 5);
  ASSERT_TRUE(H5_addr_defined(sb5_addr));
  H5EA_sblock_t* sb5 =
      H5EA__sblock_protect(hdr, iblock, sb5_addr, 5, H5AC__NO_FLAGS_SET);
  ASSERT_NE(sb5, nullptr);
  EXPECT_EQ(sb5->dblk_npages, 2u);
  EXPECT_NE(sb5->page_init, nullptr);
  EXPECT_GE(H5EA__sblock_unprotect(sb5, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5EA__sblock_delete(hdr, iblock, sb5_addr, 5), 0);

  ASSERT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5EA__hdr_delete(hdr), 0);
}

TEST(H5EAsblockTest, PopulatedPagedAndSwmrReloadDeleteSuperBlocks) {
  // 1. Populated EA covering partially populated unpaged + paged superblocks.
  {
    ScopedFile file = CreateInMemoryFile("h5easblock_populated.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    const H5EA_create_t cparam = MakeTestCreateParams();
    ScopedEa ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);

    // Index 64 -> sblk 4 (unpaged 16), 128 -> sblk 5 page 0, 146 -> sblk 5 page 1.
    for (hsize_t idx : {hsize_t{64}, hsize_t{128}, hsize_t{146}}) {
      uint64_t val = idx * 3;
      ASSERT_GE(H5EA_set(ea.get(), idx, &val), 0);
    }

    haddr_t ea_addr = HADDR_UNDEF;
    ASSERT_GE(H5EA_get_addr(ea.get(), &ea_addr), 0);
    ea.reset();

    EXPECT_GE(H5EA_delete(f, ea_addr, nullptr), 0);
  }

  // 2. Write non-SWMR file containing superblocks, reopen under SWMR_WRITE so
  // H5EA__sblock_protect attaches top_proxy, and also create/delete under SWMR.
  TempFile tmp_file("h5easblock_swmr.h5");
  haddr_t saved_addr = HADDR_UNDEF;
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile file(H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    const H5EA_create_t cparam = MakeTestCreateParams();
    ScopedEa ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);
    uint64_t v_unpaged = 640;
    uint64_t v_paged = 1280;
    ASSERT_GE(H5EA_set(ea.get(), 64, &v_unpaged), 0);
    ASSERT_GE(H5EA_set(ea.get(), 128, &v_paged), 0);
    ASSERT_GE(H5EA_get_addr(ea.get(), &saved_addr), 0);
  }

  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile file(H5Fopen(tmp_file.c_str(),
                            H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ScopedEa ea(H5EA_open(f, saved_addr, nullptr));
    ASSERT_NE(ea.get(), nullptr);
    uint64_t out = 0;
    ASSERT_GE(H5EA_get(ea.get(), 128, &out), 0);
    EXPECT_EQ(out, 1280u);
    ea.reset();

    // Direct SWMR create + delete of superblock.
    const H5EA_create_t cparam = MakeTestCreateParams();
    ScopedEa ea2(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea2.get(), nullptr);
    uint64_t v2 = 9001;
    ASSERT_GE(H5EA_set(ea2.get(), 128, &v2), 0);
    haddr_t ea2_addr = HADDR_UNDEF;
    ASSERT_GE(H5EA_get_addr(ea2.get(), &ea2_addr), 0);
    ea2.reset();

    EXPECT_GE(H5EA_delete(f, ea2_addr, nullptr), 0);
  }

  // 3. Direct H5EA__sblock_create / H5EA__sblock_delete and top_proxy failure
  // cleanup in H5EA__sblock_protect in a file transitioned via start_swmr_write.
  {
    TempFile swmr_start_file("h5easblock_swmr_start.h5");
    ScopedFile file = CreateSwmrTempFile(swmr_start_file);
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    const H5EA_create_t cparam = MakeTestCreateParams();
    haddr_t hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr));
    H5EA_hdr_t* hdr =
        H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    bool stats_changed = false;
    hdr->idx_blk_addr = H5EA__iblock_create(hdr, &stats_changed);
    ASSERT_TRUE(H5_addr_defined(hdr->idx_blk_addr));
    H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(iblock, nullptr);

    haddr_t sb_addr = H5EA__sblock_create(hdr, iblock, &stats_changed, 4);
    ASSERT_TRUE(H5_addr_defined(sb_addr));

    // Exercise H5EA__sblock_protect failure when top_proxy child attach fails.
    H5EA_sblock_t* sb =
        H5EA__sblock_protect(hdr, iblock, sb_addr, 4, H5AC__NO_FLAGS_SET);
    ASSERT_NE(sb, nullptr);
    ASSERT_NE(sb->top_proxy, nullptr);
    ASSERT_GE(H5AC_proxy_entry_remove_child(hdr->top_proxy, sb), 0);
    sb->top_proxy = nullptr;
    ASSERT_GE(H5EA__sblock_unprotect(sb, H5AC__NO_FLAGS_SET), 0);

    ASSERT_GE(H5AC_proxy_entry_remove_child(hdr->top_proxy, iblock), 0);
    iblock->top_proxy = nullptr;
    ASSERT_GE(H5AC_proxy_entry_remove_child(hdr->top_proxy, hdr), 0);
    hdr->top_proxy->addr = HADDR_UNDEF;

    {
      const haddr_t orig_tmp = f->shared->tmp_addr;
      const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      ASSERT_TRUE(H5_addr_defined(eoa));
      auto restore_tmp =
          absl::MakeCleanup([&] { f->shared->tmp_addr = orig_tmp; });
      f->shared->tmp_addr = eoa + 1;

      ScopedErrorSilence silence;
      EXPECT_EQ(
          H5EA__sblock_protect(hdr, iblock, sb_addr, 4, H5AC__NO_FLAGS_SET),
          nullptr);
    }

    ASSERT_GE(H5AC_proxy_entry_dest(hdr->top_proxy), 0);
    hdr->top_proxy = nullptr;
    EXPECT_GE(H5EA__sblock_delete(hdr, iblock, sb_addr, 4), 0);

    ASSERT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
    EXPECT_GE(H5EA__hdr_delete(hdr), 0);
  }
}

TEST(H5EAsblockTest, ErrorAndCleanupPathsCoverAllocCreateProtectAndDelete) {
  ScopedFile file = CreateInMemoryFile("h5easblock_err.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5EA_create_t cparam = MakeTestCreateParams();

  // 1. Failing H5EA__hdr_incr inside H5EA__sblock_alloc and H5EA__sblock_create
  // when hdr is unpinned and unprotected.
  haddr_t hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
  ASSERT_TRUE(H5_addr_defined(hdr_addr));
  H5EA_hdr_t* hdr = H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  ASSERT_GE(H5EA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);
  {
    bool sc = false;
    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__sblock_alloc(hdr, nullptr, 4), nullptr);
    EXPECT_EQ(H5EA__sblock_create(hdr, nullptr, &sc, 4), HADDR_UNDEF);
  }

  hdr = H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  bool stats_changed = false;
  hdr->idx_blk_addr = H5EA__iblock_create(hdr, &stats_changed);
  ASSERT_TRUE(H5_addr_defined(hdr->idx_blk_addr));
  H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(iblock, nullptr);

  // 2. Failing H5MF_alloc inside H5EA__sblock_create.
  {
    const hsize_t orig_aggr = f->shared->meta_aggr.size;
    const haddr_t orig_tmp = f->shared->tmp_addr;
    const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
    ASSERT_TRUE(H5_addr_defined(eoa));
    auto restore_space = absl::MakeCleanup([&] {
      f->shared->meta_aggr.size = orig_aggr;
      f->shared->tmp_addr = orig_tmp;
    });
    f->shared->meta_aggr.size = 0;
    f->shared->tmp_addr = eoa;

    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__sblock_create(hdr, iblock, &stats_changed, 4), HADDR_UNDEF);
  }

  // 3. Failing H5AC_insert_entry inside H5EA__sblock_create via read-only flag.
  {
    const unsigned orig_flags = f->shared->flags;
    auto restore_flags =
        absl::MakeCleanup([&] { f->shared->flags = orig_flags; });
    f->shared->flags &= ~static_cast<unsigned>(H5F_ACC_RDWR);

    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__sblock_create(hdr, iblock, &stats_changed, 5), HADDR_UNDEF);
  }

  // 4. Failing H5EA__sblock_protect and H5EA__sblock_delete on zeroed address.
  haddr_t bad_addr = H5MF_alloc(f, H5FD_MEM_EARRAY_SBLOCK, 64);
  ASSERT_TRUE(H5_addr_defined(bad_addr));
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__sblock_protect(hdr, iblock, bad_addr, 4,
                                   H5AC__READ_ONLY_FLAG),
              nullptr);
    EXPECT_LT(H5EA__sblock_delete(hdr, iblock, bad_addr, 4), 0);
  }

  // 5. Failing H5EA__sblock_unprotect when read-only entry dirtied, plus
  // failing H5EA__dblock_delete inside H5EA__sblock_delete on corrupt dblk addr.
  haddr_t sb_addr = H5EA__sblock_create(hdr, iblock, &stats_changed, 4);
  ASSERT_TRUE(H5_addr_defined(sb_addr));
  H5EA_sblock_t* sb =
      H5EA__sblock_protect(hdr, iblock, sb_addr, 4, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(sb, nullptr);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5EA__sblock_unprotect(sb, H5AC__DIRTIED_FLAG), 0);
  }
  ASSERT_GE(H5EA__sblock_unprotect(sb, H5AC__NO_FLAGS_SET), 0);

  sb = H5EA__sblock_protect(hdr, iblock, sb_addr, 4, H5AC__NO_FLAGS_SET);
  ASSERT_NE(sb, nullptr);
  sb->dblk_addrs[0] = bad_addr;
  ASSERT_GE(H5EA__sblock_unprotect(sb, H5AC__NO_FLAGS_SET), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5EA__sblock_delete(hdr, iblock, sb_addr, 4), 0);
  }

  ASSERT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5EA__hdr_delete(hdr), 0);
}

}  // namespace
}  // namespace hdf5
