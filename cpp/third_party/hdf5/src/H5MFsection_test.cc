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
// Tests file free-space section allocation, deserialization, splitting, and
// simple/small/large section class callbacks
// (third_party/hdf5/src/H5MFsection.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5MFsection.c

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"
#include "third_party/hdf5/src/H5FDdevelop.h"

// `:hdf5` is compiled with -DNDEBUG, omitting `#ifndef NDEBUG` fields from
// H5C_cache_entry_t (`H5AC_info_t`). Define NDEBUG around both the prelude and
// internal headers so struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5MFSECTION_TEST_RESTORE_UNDEF_NDEBUG 1
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
#include "third_party/hdf5/src/H5FSprivate.h"
#include "third_party/hdf5/src/H5MFpkg.h"
#include "third_party/hdf5/src/H5PBprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5MFSECTION_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5MFSECTION_TEST_RESTORE_UNDEF_NDEBUG
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

// RAII owner for H5FS_section_info_t* / H5MF_free_section_t*.
class ScopedSection {
 public:
  explicit ScopedSection(H5FS_section_info_t* sect = nullptr) : sect_(sect) {}
  explicit ScopedSection(H5MF_free_section_t* sect)
      : sect_(sect ? &sect->sect_info : nullptr) {}
  ~ScopedSection() { reset(); }

  ScopedSection(const ScopedSection&) = delete;
  ScopedSection& operator=(const ScopedSection&) = delete;

  void reset(H5FS_section_info_t* next = nullptr) {
    if (sect_ != nullptr) {
      H5MF__sect_free(sect_);
    }
    sect_ = next;
  }

  H5FS_section_info_t** slot() { return &sect_; }
  H5FS_section_info_t* get() const { return sect_; }
  H5FS_section_info_t* release() {
    H5FS_section_info_t* out = sect_;
    sect_ = nullptr;
    return out;
  }

 private:
  H5FS_section_info_t* sect_ = nullptr;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

ScopedFile CreateNonPagedCoreFile(hsize_t meta_block_size = 512,
                                  hsize_t sdata_block_size = 512) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (!fcpl.valid()) return ScopedFile();
  if (H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_FSM_AGGR,
                                 /*persist=*/false, 1) < 0) {
    return ScopedFile();
  }
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) return ScopedFile();
  if (H5Pset_meta_block_size(fapl.get(), meta_block_size) < 0) {
    return ScopedFile();
  }
  if (H5Pset_small_data_block_size(fapl.get(), sdata_block_size) < 0) {
    return ScopedFile();
  }
  return ScopedFile(
      H5Fcreate("sect_simple.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
}

ScopedFile CreatePagedCoreFile(const char* name, hsize_t page_size,
                               size_t page_buf_size) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (!fcpl.valid()) return ScopedFile();
  if (H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE,
                                 /*persist=*/false, 1) < 0) {
    return ScopedFile();
  }
  if (H5Pset_file_space_page_size(fcpl.get(), page_size) < 0) {
    return ScopedFile();
  }
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) return ScopedFile();
  if (page_buf_size > 0 &&
      H5Pset_page_buffer_size(fapl.get(), page_buf_size, 50, 50) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(name, H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
}

TEST(H5MFsectionTest, NewFreeDeserializeValidAndSplitAcrossAllClasses) {
  EXPECT_EQ(H5MF_FSPACE_SECT_CLS_SIMPLE[0].type,
            static_cast<unsigned>(H5MF_FSPACE_SECT_SIMPLE));
  EXPECT_EQ(H5MF_FSPACE_SECT_CLS_SMALL[0].type,
            static_cast<unsigned>(H5MF_FSPACE_SECT_SMALL));
  EXPECT_EQ(H5MF_FSPACE_SECT_CLS_LARGE[0].type,
            static_cast<unsigned>(H5MF_FSPACE_SECT_LARGE));

  const H5FS_section_class_t* classes[] = {
      &H5MF_FSPACE_SECT_CLS_SIMPLE[0],
      &H5MF_FSPACE_SECT_CLS_SMALL[0],
      &H5MF_FSPACE_SECT_CLS_LARGE[0],
  };

  for (const H5FS_section_class_t* cls : classes) {
    unsigned des_flags = 0;
    ASSERT_NE(cls->deserialize, nullptr);
    ScopedSection sect(
        cls->deserialize(cls, nullptr, 0x1000, 256, &des_flags));
    ASSERT_NE(sect.get(), nullptr);
    EXPECT_EQ(sect.get()->addr, 0x1000u);
    EXPECT_EQ(sect.get()->size, 256u);
    EXPECT_EQ(sect.get()->type, cls->type);
    EXPECT_EQ(sect.get()->state, H5FS_SECT_LIVE);

    ASSERT_NE(cls->valid, nullptr);
    EXPECT_GE(cls->valid(cls, sect.get()), 0);

    ASSERT_NE(cls->split, nullptr);
    ScopedSection frag(cls->split(sect.get(), 64));
    ASSERT_NE(frag.get(), nullptr);
    EXPECT_EQ(frag.get()->addr, 0x1000u);
    EXPECT_EQ(frag.get()->size, 64u);
    EXPECT_EQ(sect.get()->addr, 0x1040u);
    EXPECT_EQ(sect.get()->size, 192u);
  }
}

TEST(H5MFsectionTest, SimpleCallbacksMergeAndShrink) {
  ScopedFile file = CreateNonPagedCoreFile(/*meta_block_size=*/512,
                                           /*sdata_block_size=*/512);
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  const H5FS_section_class_t* cls = &H5MF_FSPACE_SECT_CLS_SIMPLE[0];
  H5MF_sect_ud_t udata{};
  udata.f = f;
  udata.alloc_type = H5FD_MEM_SUPER;

  // can_merge & merge
  ScopedSection s1(H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, 0x1000, 64));
  ScopedSection s2(H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, 0x1040, 32));
  ScopedSection s_gap(H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, 0x1100, 32));
  ASSERT_NE(s1.get(), nullptr);
  ASSERT_NE(s2.get(), nullptr);
  ASSERT_NE(s_gap.get(), nullptr);

  EXPECT_EQ(cls->can_merge(s1.get(), s_gap.get(), &udata), 0);
  EXPECT_EQ(cls->can_merge(s1.get(), s2.get(), &udata), 1);
  ASSERT_GE(cls->merge(s1.slot(), s2.release(), &udata), 0);
  EXPECT_EQ(s1.get()->size, 96u);

  // 1) can_shrink + shrink at EOA
  const haddr_t alloc_at_eoa = H5F__alloc(f, H5FD_MEM_DRAW, 128, nullptr, nullptr);
  ASSERT_NE(alloc_at_eoa, HADDR_UNDEF);
  const haddr_t eoa_before = H5F_get_eoa(f, H5FD_MEM_DRAW);
  ScopedSection at_eoa(
      H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, alloc_at_eoa, 128));
  ASSERT_NE(at_eoa.get(), nullptr);
  udata.alloc_type = H5FD_MEM_DRAW;
  EXPECT_EQ(cls->can_shrink(at_eoa.get(), &udata), 1);
  EXPECT_EQ(udata.shrink, H5MF_SHRINK_EOA);
  EXPECT_GE(cls->shrink(at_eoa.slot(), &udata), 0);
  EXPECT_EQ(at_eoa.get(), nullptr);
  EXPECT_EQ(H5F_get_eoa(f, H5FD_MEM_DRAW), eoa_before - 128);

  // 2) Non-EOA with allow_eoa_shrink_only == true
  udata.allow_eoa_shrink_only = true;
  EXPECT_EQ(cls->can_shrink(s1.get(), &udata), 0);
  udata.allow_eoa_shrink_only = false;

  // 3) Merge with metadata aggregator (both AGGR_ABSORB_SECT and
  // SECT_ABSORB_AGGR).
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_SUPER, 64), HADDR_UNDEF);
  ASSERT_GT(f->shared->meta_aggr.size, 0u);
  const haddr_t meta_aggr_addr = f->shared->meta_aggr.addr;
  udata.alloc_type = H5FD_MEM_SUPER;
  udata.allow_sect_absorb = true;

  // Non-adjoining metadata section fails to shrink/absorb.
  ScopedSection non_adj_meta(
      H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, 0x80, 16));
  ASSERT_NE(non_adj_meta.get(), nullptr);
  EXPECT_EQ(cls->can_shrink(non_adj_meta.get(), &udata), 0);

  // Small section adjoining front of meta_aggr => H5MF_SHRINK_AGGR_ABSORB_SECT.
  ScopedSection small_meta(
      H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, meta_aggr_addr - 32, 32));
  ASSERT_NE(small_meta.get(), nullptr);
  EXPECT_EQ(cls->can_shrink(small_meta.get(), &udata), 1);
  EXPECT_EQ(udata.shrink, H5MF_SHRINK_AGGR_ABSORB_SECT);
  EXPECT_EQ(udata.aggr, &f->shared->meta_aggr);
  EXPECT_GE(cls->shrink(small_meta.slot(), &udata), 0);
  EXPECT_EQ(small_meta.get(), nullptr);

  // Large section adjoining front of meta_aggr => H5MF_SHRINK_SECT_ABSORB_AGGR
  // leaves *sect non-null!
  const haddr_t cur_meta_addr = f->shared->meta_aggr.addr;
  ScopedSection big_meta(
      H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, cur_meta_addr - 512, 512));
  ASSERT_NE(big_meta.get(), nullptr);
  EXPECT_EQ(cls->can_shrink(big_meta.get(), &udata), 1);
  EXPECT_EQ(udata.shrink, H5MF_SHRINK_SECT_ABSORB_AGGR);
  EXPECT_GE(cls->shrink(big_meta.slot(), &udata), 0);
  EXPECT_NE(big_meta.get(), nullptr);

  // 4) Merge with small raw data aggregator + non-adjoining raw section.
  ASSERT_NE(H5MF_aggr_vfd_alloc(f, H5FD_MEM_DRAW, 64), HADDR_UNDEF);
  ASSERT_GT(f->shared->sdata_aggr.size, 0u);
  const haddr_t sdata_addr = f->shared->sdata_aggr.addr;
  udata.alloc_type = H5FD_MEM_DRAW;

  ScopedSection non_adj_raw(H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, 0x80, 16));
  ASSERT_NE(non_adj_raw.get(), nullptr);
  EXPECT_EQ(cls->can_shrink(non_adj_raw.get(), &udata), 0);

  ScopedSection small_raw(
      H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, sdata_addr - 32, 32));
  ASSERT_NE(small_raw.get(), nullptr);
  EXPECT_EQ(cls->can_shrink(small_raw.get(), &udata), 1);
  EXPECT_EQ(udata.aggr, &f->shared->sdata_aggr);
  EXPECT_GE(cls->shrink(small_raw.slot(), &udata), 0);
  EXPECT_EQ(small_raw.get(), nullptr);
}

TEST(H5MFsectionTest, SmallCallbacksAddCanMergeAndMergeWithAndWithoutPageBuf) {
  constexpr hsize_t kPageSize = 512;
  ScopedFile file =
      CreatePagedCoreFile("sect_paged_pb.h5", kPageSize, /*page_buf_size=*/4096);
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(f->shared->page_buf, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  const H5FS_section_class_t* cls = &H5MF_FSPACE_SECT_CLS_SMALL[0];
  H5MF_sect_ud_t udata{};
  udata.f = f;

  // small_add: early return for DRAW / GHEAP.
  {
    ScopedSection draw_sect(H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, 0x400, 64));
    ASSERT_NE(draw_sect.get(), nullptr);
    unsigned flags = H5FS_ADD_RETURNED_SPACE;
    udata.alloc_type = H5FD_MEM_DRAW;
    EXPECT_GE(cls->add(draw_sect.slot(), &flags, &udata), 0);
    EXPECT_EQ(draw_sect.get()->size, 64u);
    udata.alloc_type = H5FD_MEM_GHEAP;
    EXPECT_GE(cls->add(draw_sect.slot(), &flags, &udata), 0);
  }

  // small_add: fs_page_size == 0 triggers H5E_BADVALUE error.
  {
    const hsize_t saved_psize = f->shared->fs_page_size;
    f->shared->fs_page_size = 0;
    absl::Cleanup restore_psize = [&] { f->shared->fs_page_size = saved_psize; };
    ScopedErrorSilence silence;
    ScopedSection bad_sect(H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, 0x400, 64));
    ASSERT_NE(bad_sect.get(), nullptr);
    unsigned flags = H5FS_ADD_RETURNED_SPACE;
    udata.alloc_type = H5FD_MEM_SUPER;
    EXPECT_LT(cls->add(bad_sect.slot(), &flags, &udata), 0);
  }

  // small_add: page-end drop, page-end adjust, and unchanged cases.
  {
    const size_t saved_thres = f->shared->pgend_meta_thres;
    f->shared->pgend_meta_thres = 64;
    absl::Cleanup restore_thres = [&] {
      f->shared->pgend_meta_thres = saved_thres;
    };
    udata.alloc_type = H5FD_MEM_SUPER;

    // 1) At page end with size <= thres and H5FS_ADD_RETURNED_SPACE => dropped.
    ScopedSection pgend_drop(
        H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, kPageSize + 448, 64));
    ASSERT_NE(pgend_drop.get(), nullptr);
    unsigned flags = H5FS_ADD_RETURNED_SPACE;
    EXPECT_GE(cls->add(pgend_drop.slot(), &flags, &udata), 0);
    EXPECT_EQ(pgend_drop.get(), nullptr);
    EXPECT_EQ(flags & H5FS_ADD_RETURNED_SPACE, 0u);
    EXPECT_NE(flags & H5FS_PAGE_END_NO_ADD, 0u);

    // 2) At page end WITHOUT H5FS_ADD_RETURNED_SPACE => kept unchanged.
    ScopedSection pgend_keep(
        H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, kPageSize + 448, 64));
    ASSERT_NE(pgend_keep.get(), nullptr);
    flags = 0;
    EXPECT_GE(cls->add(pgend_keep.slot(), &flags, &udata), 0);
    ASSERT_NE(pgend_keep.get(), nullptr);
    EXPECT_EQ(pgend_keep.get()->size, 64u);

    // 3) Near page end (prem == 52 <= 64) => grows size to reach page end.
    ScopedSection pgend_adj(
        H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, kPageSize + 200, 260));
    ASSERT_NE(pgend_adj.get(), nullptr);
    flags = H5FS_ADD_RETURNED_SPACE;
    EXPECT_GE(cls->add(pgend_adj.slot(), &flags, &udata), 0);
    EXPECT_EQ(pgend_adj.get()->size, 312u);

    // 4) Far from page end (prem > 64) => unchanged.
    ScopedSection normal_small(
        H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, kPageSize + 64, 100));
    ASSERT_NE(normal_small.get(), nullptr);
    EXPECT_GE(cls->add(normal_small.slot(), &flags, &udata), 0);
    EXPECT_EQ(normal_small.get()->size, 100u);
  }

  // small_can_merge: non-adjoining, same page (true), cross-page boundary (false).
  {
    ScopedSection s1(H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, kPageSize, 256));
    ScopedSection s2(
        H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, kPageSize + 256, 256));
    ScopedSection s3(
        H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, 2 * kPageSize, 256));
    ASSERT_NE(s1.get(), nullptr);
    ASSERT_NE(s2.get(), nullptr);
    ASSERT_NE(s3.get(), nullptr);

    EXPECT_EQ(cls->can_merge(s1.get(), s3.get(), &udata), 0);
    EXPECT_EQ(cls->can_merge(s1.get(), s2.get(), &udata), 1);
    EXPECT_EQ(cls->can_merge(s2.get(), s3.get(), &udata), 0);
  }

  // small_merge: sub-page merge and full-page merge (with page_buf != nullptr
  // for SUPER and DRAW, and via real H5MF_alloc + H5MF_xfree).
  {
    const haddr_t page1 = H5MF_alloc(f, H5FD_MEM_SUPER, 100);
    ASSERT_NE(page1, HADDR_UNDEF);
    const haddr_t page1_b = H5MF_alloc(f, H5FD_MEM_SUPER, 100);
    EXPECT_EQ(page1_b, page1 + 100);
    // Free [page1 + 100, 100) then [page1, 100): first merges with remaining
    // 312 bytes (< 512), second reaches full 512-byte page and removes from PB!
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_SUPER, page1_b, 100), 0);
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_SUPER, page1, 100), 0);

    // Same full-page merge for DRAW (bypasses H5PB_remove_entry).
    const haddr_t draw_page = H5MF_alloc(f, H5FD_MEM_DRAW, 256);
    ASSERT_NE(draw_page, HADDR_UNDEF);
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, draw_page, 256), 0);

    // Partial merge directly (< fs_page_size).
    ScopedSection p1(H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, kPageSize, 100));
    ScopedSection p2(
        H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, kPageSize + 100, 100));
    ASSERT_NE(p1.get(), nullptr);
    ASSERT_NE(p2.get(), nullptr);
    udata.alloc_type = H5FD_MEM_SUPER;
    EXPECT_GE(cls->merge(p1.slot(), p2.release(), &udata), 0);
    ASSERT_NE(p1.get(), nullptr);
    EXPECT_EQ(p1.get()->size, 200u);

    // Direct full-page merge on metadata page hits H5PB_remove_entry directly.
    const haddr_t full_meta_page = H5MF_alloc(f, H5FD_MEM_SUPER, kPageSize);
    ASSERT_NE(full_meta_page, HADDR_UNDEF);
    ASSERT_GE(H5PB_add_new_page(f->shared, H5FD_MEM_SUPER, full_meta_page), 0);
    ScopedSection h1(
        H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, full_meta_page, 256));
    ScopedSection h2(
        H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, full_meta_page + 256, 256));
    ASSERT_NE(h1.get(), nullptr);
    ASSERT_NE(h2.get(), nullptr);
    udata.alloc_type = H5FD_MEM_SUPER;
    EXPECT_GE(cls->merge(h1.slot(), h2.release(), &udata), 0);
    EXPECT_EQ(h1.get(), nullptr);

    // Direct full-page merge at >= tmp_addr triggers H5MF_xfree error return.
    ScopedErrorSilence silence;
    ScopedSection bad1(
        H5MF__sect_new(H5MF_FSPACE_SECT_SMALL, f->shared->tmp_addr, 256));
    ScopedSection bad2(H5MF__sect_new(H5MF_FSPACE_SECT_SMALL,
                                      f->shared->tmp_addr + 256, 256));
    ASSERT_NE(bad1.get(), nullptr);
    ASSERT_NE(bad2.get(), nullptr);
    H5MF_free_section_t* raw_bad2 =
        reinterpret_cast<H5MF_free_section_t*>(bad2.release());
    EXPECT_LT(cls->merge(bad1.slot(), &raw_bad2->sect_info, &udata), 0);
    H5MF__sect_free(&raw_bad2->sect_info);
  }

  // Also test full-page merge with page_buf == nullptr.
  {
    ScopedFile no_pb_file =
        CreatePagedCoreFile("sect_paged_nopb.h5", kPageSize, /*page_buf_size=*/0);
    ASSERT_TRUE(no_pb_file.valid());
    H5F_t* f_nopb = UnwrapFile(no_pb_file.get());
    ASSERT_NE(f_nopb, nullptr);
    EXPECT_EQ(f_nopb->shared->page_buf, nullptr);

    ScopedApiContext nopb_ctx;
    ASSERT_TRUE(nopb_ctx.valid());
    const haddr_t meta_page = H5MF_alloc(f_nopb, H5FD_MEM_SUPER, 256);
    ASSERT_NE(meta_page, HADDR_UNDEF);
    EXPECT_GE(H5MF_xfree(f_nopb, H5FD_MEM_SUPER, meta_page, 256), 0);
  }
}

TEST(H5MFsectionTest, LargeCallbacksCanMergeMergeCanShrinkAndShrink) {
  constexpr hsize_t kPageSize = 512;
  ScopedFile file =
      CreatePagedCoreFile("sect_large.h5", kPageSize, /*page_buf_size=*/0);
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  const H5FS_section_class_t* cls = &H5MF_FSPACE_SECT_CLS_LARGE[0];
  H5MF_sect_ud_t udata{};
  udata.f = f;
  udata.alloc_type = H5FD_MEM_DRAW;

  // can_merge & merge
  {
    ScopedSection l1(
        H5MF__sect_new(H5MF_FSPACE_SECT_LARGE, kPageSize, kPageSize));
    ScopedSection l2(
        H5MF__sect_new(H5MF_FSPACE_SECT_LARGE, 2 * kPageSize, kPageSize));
    ScopedSection l_gap(
        H5MF__sect_new(H5MF_FSPACE_SECT_LARGE, 4 * kPageSize, kPageSize));
    ASSERT_NE(l1.get(), nullptr);
    ASSERT_NE(l2.get(), nullptr);
    ASSERT_NE(l_gap.get(), nullptr);

    EXPECT_EQ(cls->can_merge(l1.get(), l_gap.get(), &udata), 0);
    EXPECT_EQ(cls->can_merge(l1.get(), l2.get(), &udata), 1);
    EXPECT_GE(cls->merge(l1.slot(), l2.release(), &udata), 0);
    EXPECT_EQ(l1.get()->size, 2 * kPageSize);
  }

  // Allocate two full pages at EOA, test can_shrink & page-aligned shrink.
  {
    const haddr_t blk = H5MF_alloc(f, H5FD_MEM_DRAW, 2 * kPageSize);
    ASSERT_NE(blk, HADDR_UNDEF);
    ASSERT_EQ(blk % kPageSize, 0u);
    const haddr_t eoa_before = H5F_get_eoa(f, H5FD_MEM_DRAW);
    ASSERT_EQ(blk + 2 * kPageSize, eoa_before);

    ScopedSection not_at_eoa(
        H5MF__sect_new(H5MF_FSPACE_SECT_LARGE, blk, kPageSize));
    EXPECT_EQ(cls->can_shrink(not_at_eoa.get(), &udata), 0);

    ScopedSection too_small_at_eoa(H5MF__sect_new(
        H5MF_FSPACE_SECT_LARGE, eoa_before - 256, 256));
    EXPECT_EQ(cls->can_shrink(too_small_at_eoa.get(), &udata), 0);

    ScopedSection aligned_eoa(
        H5MF__sect_new(H5MF_FSPACE_SECT_LARGE, blk, 2 * kPageSize));
    EXPECT_EQ(cls->can_shrink(aligned_eoa.get(), &udata), 1);
    EXPECT_EQ(udata.shrink, H5MF_SHRINK_EOA);
    EXPECT_GE(cls->shrink(aligned_eoa.slot(), &udata), 0);
    EXPECT_EQ(aligned_eoa.get(), nullptr);
    EXPECT_EQ(H5F_get_eoa(f, H5FD_MEM_DRAW), blk);
  }

  // Misaligned large section at EOA retains partial page fragment (frag_size).
  {
    const haddr_t blk = H5MF_alloc(f, H5FD_MEM_DRAW, 2 * kPageSize);
    ASSERT_NE(blk, HADDR_UNDEF);
    const haddr_t eoa_before = H5F_get_eoa(f, H5FD_MEM_DRAW);
    ASSERT_EQ(blk + 2 * kPageSize, eoa_before);

    // Start 100 bytes past page boundary: frag_size = 512 - 100 = 412;
    // frees the trailing 512-byte full page at EOA, retains 412-byte section.
    ScopedSection mis_eoa(H5MF__sect_new(H5MF_FSPACE_SECT_LARGE, blk + 100,
                                         2 * kPageSize - 100));
    ASSERT_NE(mis_eoa.get(), nullptr);
    EXPECT_EQ(cls->can_shrink(mis_eoa.get(), &udata), 1);
    EXPECT_EQ(udata.shrink, H5MF_SHRINK_EOA);
    EXPECT_GE(cls->shrink(mis_eoa.slot(), &udata), 0);
    ASSERT_NE(mis_eoa.get(), nullptr);
    EXPECT_EQ(mis_eoa.get()->size, kPageSize - 100);
    EXPECT_EQ(H5F_get_eoa(f, H5FD_MEM_DRAW), blk + kPageSize);
  }
}

TEST(H5MFsectionTest, PersistentFsmReopenAndAlignedAllocExercisesDeserializeAndSplit) {
  TempFile temp("persistent_fsm.h5");
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_FSM_AGGR,
                                       /*persist=*/true, 1),
            0);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_alignment(fapl.get(), /*threshold=*/64, /*alignment=*/64),
            0);

  // Create persistent file with non-EOA unaligned free section so FSM is saved.
  {
    ScopedFile file(
        H5Fcreate(temp.c_str(), H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    const haddr_t a1 = H5MF_alloc(f, H5FD_MEM_DRAW, 17);
    const haddr_t a2 = H5MF_alloc(f, H5FD_MEM_DRAW, 256);
    const haddr_t anchor = H5MF_alloc(f, H5FD_MEM_DRAW, 256);
    ASSERT_NE(a1, HADDR_UNDEF);
    ASSERT_NE(a2, HADDR_UNDEF);
    ASSERT_NE(anchor, HADDR_UNDEF);
    // Free contiguous odd-offset + 256-byte region before anchor so reopening
    // deserializes it and subsequent aligned alloc splits off a fragment.
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, a1, 17), 0);
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_DRAW, a2, 256), 0);
  }

  {
    ScopedFile reopened(H5Fopen(temp.c_str(), H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(reopened.valid());
    H5F_t* f = UnwrapFile(reopened.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    // Requesting >= threshold (64) triggers FSM open -> H5MF__sect_deserialize
    // and section alignment split -> H5MF__sect_split.
    const haddr_t aligned = H5MF_alloc(f, H5FD_MEM_DRAW, 128);
    EXPECT_NE(aligned, HADDR_UNDEF);
    EXPECT_EQ(aligned % 64, 0u);
  }
}

struct SectionFaultControls {
  bool fail_get_eoa = false;
  bool fail_free = false;
};

SectionFaultControls* g_sect_faults = nullptr;

struct SectionFaultFile {
  H5FD_t pub{};
  haddr_t eoa = 0;
  std::vector<uint8_t> buf;
};

H5FD_t* SectFaultOpen(const char* /*name*/, unsigned /*flags*/,
                      hid_t /*fapl_id*/, haddr_t maxaddr) {
  auto* file = new SectionFaultFile();
  file->pub.maxaddr = maxaddr;
  return &file->pub;
}

herr_t SectFaultClose(H5FD_t* f) {
  delete reinterpret_cast<SectionFaultFile*>(f);
  return SUCCEED;
}

int SectFaultCmp(const H5FD_t* f1, const H5FD_t* f2) {
  return f1 < f2 ? -1 : (f1 > f2 ? 1 : 0);
}

herr_t SectFaultQuery(const H5FD_t* /*f*/, unsigned long* flags) {
  if (flags != nullptr) {
    *flags = H5FD_FEAT_AGGREGATE_METADATA | H5FD_FEAT_AGGREGATE_SMALLDATA;
  }
  return SUCCEED;
}

haddr_t SectFaultGetEoa(const H5FD_t* f, H5FD_mem_t /*type*/) {
  if (g_sect_faults != nullptr && g_sect_faults->fail_get_eoa) {
    return HADDR_UNDEF;
  }
  return reinterpret_cast<const SectionFaultFile*>(f)->eoa;
}

herr_t SectFaultSetEoa(H5FD_t* f, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* file = reinterpret_cast<SectionFaultFile*>(f);
  file->eoa = addr;
  if (file->eoa > file->buf.size()) {
    file->buf.resize(static_cast<size_t>(file->eoa), 0);
  }
  return SUCCEED;
}

haddr_t SectFaultGetEof(const H5FD_t* f, H5FD_mem_t /*type*/) {
  return static_cast<haddr_t>(
      reinterpret_cast<const SectionFaultFile*>(f)->buf.size());
}

herr_t SectFaultFree(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                     haddr_t addr, hsize_t size) {
  if (g_sect_faults != nullptr && g_sect_faults->fail_free) {
    return FAIL;
  }
  auto* file = reinterpret_cast<SectionFaultFile*>(f);
  if (addr + size == file->eoa) {
    file->eoa = addr;
  }
  return SUCCEED;
}

herr_t SectFaultRead(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                     haddr_t addr, size_t size, void* out) {
  const auto* file = reinterpret_cast<const SectionFaultFile*>(f);
  std::memset(out, 0, size);
  if (addr < file->buf.size()) {
    const size_t n = std::min<size_t>(file->buf.size() - addr, size);
    std::memcpy(out, file->buf.data() + addr, n);
  }
  return SUCCEED;
}

herr_t SectFaultWrite(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                      haddr_t addr, size_t size, const void* in) {
  auto* file = reinterpret_cast<SectionFaultFile*>(f);
  if (addr + size > file->buf.size()) {
    file->buf.resize(static_cast<size_t>(addr + size), 0);
  }
  if (size > 0) {
    std::memcpy(file->buf.data() + addr, in, size);
  }
  return SUCCEED;
}

TEST(H5MFsectionTest, DriverErrorPathsForSimpleAndLargeShrink) {
  SectionFaultControls controls;
  g_sect_faults = &controls;
  absl::Cleanup clear_controls = [] { g_sect_faults = nullptr; };

  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = static_cast<H5FD_class_value_t>(513);
  cls.name = "fault_mem_sect_test";
  cls.maxaddr = (HADDR_MAX / 2);
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.open = SectFaultOpen;
  cls.close = SectFaultClose;
  cls.cmp = SectFaultCmp;
  cls.query = SectFaultQuery;
  cls.free = SectFaultFree;
  cls.get_eoa = SectFaultGetEoa;
  cls.set_eoa = SectFaultSetEoa;
  cls.get_eof = SectFaultGetEof;
  cls.read = SectFaultRead;
  cls.write = SectFaultWrite;
  constexpr H5FD_mem_t kFlMap[H5FD_MEM_NTYPES] = H5FD_FLMAP_DICHOTOMY;
  std::memcpy(cls.fl_map, kFlMap, sizeof(kFlMap));

  const hid_t drv_id = H5FDregister(&cls);
  ASSERT_GE(drv_id, 0);
  absl::Cleanup unreg_drv = [drv_id] { H5FDunregister(drv_id); };

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_driver(fapl.get(), drv_id, nullptr), 0);

  // Non-paged file for simple_can_shrink / simple_shrink error paths.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_FSM_AGGR,
                                         false, 1),
              0);
    ScopedFile file(
        H5Fcreate("sect_fault_simple.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;
    absl::Cleanup reset_faults = [&controls] {
      controls = SectionFaultControls{};
    };

    H5MF_sect_ud_t udata{};
    udata.f = f;
    udata.alloc_type = H5FD_MEM_DRAW;
    udata.shrink = H5MF_SHRINK_EOA;

    ScopedSection s(H5MF__sect_new(H5MF_FSPACE_SECT_SIMPLE, 0x1000, 128));
    ASSERT_NE(s.get(), nullptr);

    controls.fail_get_eoa = true;
    EXPECT_LT(H5MF_FSPACE_SECT_CLS_SIMPLE[0].can_shrink(s.get(), &udata), 0);
    controls.fail_get_eoa = false;

    controls.fail_free = true;
    EXPECT_LT(H5MF_FSPACE_SECT_CLS_SIMPLE[0].shrink(s.slot(), &udata), 0);
    controls.fail_free = false;
  }

  // Paged file for large_can_shrink / large_shrink error paths.
  {
    constexpr hsize_t kPageSize = 512;
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE,
                                         false, 1),
              0);
    ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), kPageSize), 0);
    ScopedFile file(
        H5Fcreate("sect_fault_large.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;
    absl::Cleanup reset_faults = [&controls] {
      controls = SectionFaultControls{};
    };

    H5MF_sect_ud_t udata{};
    udata.f = f;
    udata.alloc_type = H5FD_MEM_DRAW;
    udata.shrink = H5MF_SHRINK_EOA;

    ScopedSection l(H5MF__sect_new(H5MF_FSPACE_SECT_LARGE, kPageSize, kPageSize));
    ASSERT_NE(l.get(), nullptr);

    controls.fail_get_eoa = true;
    EXPECT_LT(H5MF_FSPACE_SECT_CLS_LARGE[0].can_shrink(l.get(), &udata), 0);
    controls.fail_get_eoa = false;

    controls.fail_free = true;
    EXPECT_LT(H5MF_FSPACE_SECT_CLS_LARGE[0].shrink(l.slot(), &udata), 0);
    controls.fail_free = false;
  }
}

}  // namespace
}  // namespace hdf5
