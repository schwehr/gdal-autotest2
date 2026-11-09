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
// Tests fractal heap indirect block lifecycle: pinning/unpinning, reference
// counting, root creation/doubling/halving/reverting, row allocation, child
// indirect block creation/attach/detach/deletion, size calculation, parent info
// lookup, real-space vs tmp-space allocation, and cache-error paths
// (third_party/hdf5/src/H5HFiblock.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFiblock.c

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t) inside internal fractal heap and file structs.
#ifndef NDEBUG
#define H5HFIBLOCK_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5HFpkg.h pulls in H5B2private.h and H5Fpkg.h pulls in H5Bprivate.h, both of
// which use `try` as a C parameter name, colliding with the C++ keyword and
// Google3's `-fno-exceptions` macro.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_param_try
extern "C" {
#define H5F_FRIEND
#define H5HF_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5HFpkg.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#include "third_party/hdf5/src/H5Zprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HFIBLOCK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HFIBLOCK_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

// RAII wrapper for pushing/popping an HDF5 API context and metadata tag.
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

H5F_t* GetFilePtr(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5HF_create_t MakeIblockHeapParams(unsigned width = 2,
                                   size_t start_block_size = 64,
                                   size_t max_direct_size = 64,
                                   unsigned max_index = 14,
                                   unsigned start_root_rows = 1,
                                   uint32_t max_man_size = 44) {
  H5HF_create_t cparam{};
  cparam.managed.width = width;
  cparam.managed.start_block_size = start_block_size;
  cparam.managed.max_direct_size = max_direct_size;
  cparam.managed.max_index = max_index;
  cparam.managed.start_root_rows = start_root_rows;
  cparam.checksum_dblocks = true;
  cparam.max_man_size = max_man_size;
  cparam.id_len = 0;
  return cparam;
}

std::vector<uint8_t> MakePattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>((seed + i * 11) & 0xffU);
  }
  return data;
}

void RunMultiLevelDoubleHalveRevert(bool filtered, bool flush_before_shrink,
                                    bool use_tmp_space) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  const bool orig_tmp_space = f->shared->use_tmp_space;
  f->shared->use_tmp_space = use_tmp_space;
  auto restore_tmp_space =
      absl::MakeCleanup([&]() { f->shared->use_tmp_space = orig_tmp_space; });

  // max_direct_rows = 3 (rows 0,1 = 64B, row 2 = 128B); row 3..7 are indirect.
  // Growing root nrows 1 -> 2 -> 4 -> 8 hits both `old_nrows < max_direct_rows`
  // and `old_nrows >= max_direct_rows` in root_double, and halving 8 -> 4 -> 2
  // hits both `child_iblocks` realloc and free paths in root_halve.
  // max_man_size=100 ensures 80B objects are stored as managed objects in
  // row 2 (128B blocks), driving root growth and shrinkage predictably.
  H5HF_create_t cparam = MakeIblockHeapParams(
      /*width=*/2, /*start_block_size=*/64, /*max_direct_size=*/128,
      /*max_index=*/14, /*start_root_rows=*/1, /*max_man_size=*/100);
  auto reset_pline = absl::MakeCleanup([&]() {
    if (cparam.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
    }
  });
  if (filtered) {
    unsigned level = 6;
    ASSERT_GE(
        H5Z_append(&cparam.pline, H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL, 1,
                   &level),
        0);
  }

  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
  H5HF_hdr_t* hdr = fh->hdr;

  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);

  // 1. Insert 4 small (40B) objects (rows 0-1) + 2 medium (80B) objects
  //    (row 2) + enough 40B objects to reach row 4 (nrows = 8).
  //    Total blocks before row 4: 4*64 + 2*128 + 2*256 = 1024B = 16 small
  //    equivalent; specifically 4 + 2 + 8 + 1 = 15 objects reaches row 4.
  std::vector<std::vector<uint8_t>> ids;
  for (int i = 0; i < 4; ++i) {
    ids.emplace_back(id_len);
    const std::vector<uint8_t> obj =
        MakePattern(40, static_cast<uint8_t>(i));
    ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), ids.back().data()), 0);
  }
  if (flush_before_shrink) {
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  }
  for (int i = 0; i < 2; ++i) {
    ids.emplace_back(id_len);
    const std::vector<uint8_t> obj =
        MakePattern(80, static_cast<uint8_t>(20 + i));
    ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), ids.back().data()), 0);
  }
  for (int i = 0; i < 9; ++i) {
    ids.emplace_back(id_len);
    const std::vector<uint8_t> obj =
        MakePattern(40, static_cast<uint8_t>(40 + i));
    ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), ids.back().data()), 0);
  }
  ASSERT_GE(hdr->man_dtable.curr_root_rows, 8U);

  // 2. Check H5HF_size() across 3-level indirect block tree.
  hsize_t heap_size = 0;
  EXPECT_GE(H5HF_size(fh, &heap_size), 0);
  EXPECT_GT(heap_size, 0U);

  if (flush_before_shrink) {
    // Move root & child iblocks to real file space before halving/detaching.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  }

  // 3. Remove row 4 and row 3 objects while keeping row 2 (ids[4]) + row 0-1:
  //    triggers root_halve 8 -> 4 (reallocating child_iblocks).
  for (size_t i = 6; i < ids.size(); ++i) {
    ASSERT_GE(H5HF_remove(fh, ids[i].data()), 0);
  }
  EXPECT_EQ(hdr->man_dtable.curr_root_rows, 4U);

  // 4. Remove row 2 objects (ids[4], ids[5]) while keeping row 0 & row 1:
  //    triggers root_halve 4 -> 2 (freeing child_iblocks).
  ASSERT_GE(H5HF_remove(fh, ids[4].data()), 0);
  ASSERT_GE(H5HF_remove(fh, ids[5].data()), 0);
  EXPECT_EQ(hdr->man_dtable.curr_root_rows, 2U);

  // 5. Remove row 1 & row 0 col 1 while keeping entry 0 (ids[0]):
  //    triggers H5HF__man_iblock_root_revert() back to root direct block!
  ASSERT_GE(H5HF_remove(fh, ids[3].data()), 0);
  ASSERT_GE(H5HF_remove(fh, ids[2].data()), 0);
  ASSERT_GE(H5HF_remove(fh, ids[1].data()), 0);
  EXPECT_EQ(hdr->man_dtable.curr_root_rows, 0U);

  // Verify entry 0 survived revert intact, then remove it to empty the heap.
  std::vector<uint8_t> read0(40, 0);
  ASSERT_GE(H5HF_read(fh, ids[0].data(), read0.data()), 0);
  EXPECT_EQ(read0, MakePattern(40, 0));
  ASSERT_GE(H5HF_remove(fh, ids[0].data()), 0);
  EXPECT_EQ(hdr->man_dtable.table_addr, HADDR_UNDEF);
}

TEST(H5HFiblockTest, UnfilteredAndFilteredGrowHalveRevertAndRealSpacePaths) {
  RunMultiLevelDoubleHalveRevert(/*filtered=*/false,
                                 /*flush_before_shrink=*/false,
                                 /*use_tmp_space=*/true);
  RunMultiLevelDoubleHalveRevert(/*filtered=*/true,
                                 /*flush_before_shrink=*/false,
                                 /*use_tmp_space=*/true);
  RunMultiLevelDoubleHalveRevert(/*filtered=*/false,
                                 /*flush_before_shrink=*/true,
                                 /*use_tmp_space=*/true);
  RunMultiLevelDoubleHalveRevert(/*filtered=*/true,
                                 /*flush_before_shrink=*/true,
                                 /*use_tmp_space=*/false);
}

TEST(H5HFiblockTest, RootCreateSkipFullRootAndEmptyRootIblockDetach) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  // 1. Removing entry 0 BEFORE entry 1 skips root_revert, and removing entry 1
  //    drops root nchildren to 0 -> calls H5HF__hdr_empty() from detach.
  //    (Note: upstream H5HF__man_iblock_root_create indexes row_block_off[nrows]
  //    out-of-bounds when start_root_rows == 0 triggers max_root_rows, so we
  //    use start_root_rows=1 to reach empty-root detach cleanly under ASan.)
  {
    const H5HF_create_t cparam = MakeIblockHeapParams(
        /*width=*/2, /*start_block_size=*/64, /*max_direct_size=*/128,
        /*max_index=*/12, /*start_root_rows=*/1, /*max_man_size=*/100);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    size_t id_len = 0;
    ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
    std::vector<uint8_t> id0(id_len);
    std::vector<uint8_t> id1(id_len);
    const std::vector<uint8_t> obj = MakePattern(40, 0x05U);
    ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), id0.data()), 0);
    ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), id1.data()), 0);
    EXPECT_GE(fh->hdr->man_dtable.curr_root_rows, 1U);

    // Removing entry 0 BEFORE entry 1 skips root_revert, and removing entry 1
    // drops root nchildren to 0 -> calls H5HF__hdr_empty() from detach.
    ASSERT_GE(H5HF_remove(fh, id0.data()), 0);
    ASSERT_GE(H5HF_remove(fh, id1.data()), 0);
    EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 0U);
    EXPECT_EQ(fh->hdr->man_dtable.table_addr, HADDR_UNDEF);
  }

  // 2. Empty heap receiving an object needing > start_block_size calls
  //    root_create with have_direct_block=false and min_dblock_size >
  //    start_block_size.
  {
    const H5HF_create_t cparam = MakeIblockHeapParams(
        /*width=*/2, /*start_block_size=*/64, /*max_direct_size=*/256,
        /*max_index=*/14, /*start_root_rows=*/1, /*max_man_size=*/200);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    size_t id_len = 0;
    ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
    std::vector<uint8_t> id_big(id_len);
    const std::vector<uint8_t> obj_big = MakePattern(180, 0x31U);
    ASSERT_GE(H5HF_insert(fh, obj_big.size(), obj_big.data(), id_big.data()),
              0);
    EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 4U);
  }

  // 3. root_double with skip_direct_rows=true: fill rows 0-1 (nrows=2 <
  //    max_direct_rows=4), then insert 180B object (min_dblock_size=256 >
  //    next_size=128).
  {
    const H5HF_create_t cparam = MakeIblockHeapParams(
        /*width=*/2, /*start_block_size=*/64, /*max_direct_size=*/256,
        /*max_index=*/14, /*start_root_rows=*/1, /*max_man_size=*/200);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    size_t id_len = 0;
    ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
    std::vector<uint8_t> id(id_len);
    const std::vector<uint8_t> obj_small = MakePattern(40, 0x41U);
    for (int i = 0; i < 4; ++i) {
      ASSERT_GE(H5HF_insert(fh, obj_small.size(), obj_small.data(), id.data()),
                0);
    }
    EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 2U);

    const std::vector<uint8_t> obj_big = MakePattern(180, 0x42U);
    ASSERT_GE(H5HF_insert(fh, obj_big.size(), obj_big.data(), id.data()), 0);
    EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 4U);
  }
}

TEST(H5HFiblockTest, ChildIblockTakeOwnershipDetachAndAllocRowPaths) {
  haddr_t heap_addr = HADDR_UNDEF;
  std::vector<uint8_t> row4_obj_id;
  std::vector<uint8_t> image;

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    // max_direct_rows = 3 (rows 0,1 = 64B, row 2 = 128B).
    const H5HF_create_t cparam = MakeIblockHeapParams(
        /*width=*/2, /*start_block_size=*/64, /*max_direct_size=*/128,
        /*max_index=*/14, /*start_root_rows=*/1, /*max_man_size=*/100);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() {
      if (fh != nullptr) {
        H5HF_close(fh);
      }
    });
    ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);

    size_t id_len = 0;
    ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
    std::vector<uint8_t> id(id_len);

    // Fill rows 0-1 (4 x 40B) and row 2 (2 x 80B) so next_row is 3.
    for (int i = 0; i < 4; ++i) {
      const std::vector<uint8_t> obj = MakePattern(40, static_cast<uint8_t>(i));
      ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), id.data()), 0);
    }
    for (int i = 0; i < 2; ++i) {
      const std::vector<uint8_t> obj =
          MakePattern(80, static_cast<uint8_t>(10 + i));
      ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), id.data()), 0);
    }

    // Inserting two 80B objects at next_row=3 skips row 3, creates child iblock
    // at row 4 col 0 (child_nrows=3), skips child rows 0-1 (holding refcount >
    // 1 on child iblock), and fills child entries 4 and 5. Then inserting an
    // 80B anchor at row 4 col 1 keeps root->nrows=8 and man_size covering row 4
    // col 0 when row 4 col 0 is emptied.
    std::vector<uint8_t> r4c0_e4_id(id_len);
    std::vector<uint8_t> r4c0_e5_id(id_len);
    std::vector<uint8_t> r4c1_anchor_id(id_len);
    const std::vector<uint8_t> obj_80 = MakePattern(80, 0x77U);
    ASSERT_GE(H5HF_insert(fh, obj_80.size(), obj_80.data(), r4c0_e4_id.data()),
              0);
    ASSERT_GE(H5HF_insert(fh, obj_80.size(), obj_80.data(), r4c0_e5_id.data()),
              0);
    ASSERT_GE(
        H5HF_insert(fh, obj_80.size(), obj_80.data(), r4c1_anchor_id.data()),
        0);

    // Flush so the child iblock at row 4 col 0 is in real file space, then remove
    // both objects in row 4 col 0: triggers del_iblock->rc > 0
    // (took_ownership=true, H5MF_xfree on real space, removed_from_cache=true)
    // and subsequent H5HF__man_iblock_dest() when rc drops to 0.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5HF_remove(fh, r4c0_e4_id.data()), 0);
    ASSERT_GE(H5HF_remove(fh, r4c0_e5_id.data()), 0);

    // Re-insert one 80B object: allocates into row 4 col 0 (while row 4 col 1
    // still keeps man_size >= 2048), leaving row 3 / row 4 skipped sections
    // valid on reopen.
    row4_obj_id.resize(id_len);
    ASSERT_GE(
        H5HF_insert(fh, obj_80.size(), obj_80.data(), row4_obj_id.data()), 0);
    EXPECT_GE(H5HF_close(fh), 0);
    fh = nullptr;

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  // Reopen and insert an 80B object into the serialized 128B row section in
  // row 4 col 0 (whose child indirect block is already allocated on disk) ->
  // hits H5HF__man_iblock_alloc_row() revival path, then delete the whole
  // heap -> hits H5HF__man_iblock_delete() on real-space multi-level tree.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("iblock_reopen.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5F_t* f2 = GetFilePtr(reopened.get());
  ASSERT_NE(f2, nullptr);

  H5HF_t* fh2 = H5HF_open(f2, heap_addr);
  ASSERT_NE(fh2, nullptr);
  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh2, &id_len), 0);
  std::vector<uint8_t> alloc_row_id(id_len);
  const std::vector<uint8_t> alloc_row_obj = MakePattern(80, 0x99U);
  ASSERT_GE(H5HF_insert(fh2, alloc_row_obj.size(), alloc_row_obj.data(),
                        alloc_row_id.data()),
            0);
  ASSERT_GE(H5HF_close(fh2), 0);

  EXPECT_GE(H5HF_delete(f2, heap_addr), 0);
}

TEST(H5HFiblockTest, FilteredMultiLevelDeleteFreesAllBlocks) {
  haddr_t heap_addr = HADDR_UNDEF;
  std::vector<uint8_t> image;

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_create_t cparam = MakeIblockHeapParams(
        /*width=*/2, /*start_block_size=*/64, /*max_direct_size=*/64,
        /*max_index=*/14, /*start_root_rows=*/1, /*max_man_size=*/44);
    unsigned level = 6;
    ASSERT_GE(
        H5Z_append(&cparam.pline, H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL, 1,
                   &level),
        0);
    auto reset_pline =
        absl::MakeCleanup([&]() { H5O_msg_reset(H5O_PLINE_ID, &cparam.pline); });

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() {
      if (fh != nullptr) {
        H5HF_close(fh);
      }
    });
    ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);

    size_t id_len = 0;
    ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
    std::vector<uint8_t> id(id_len);
    for (int i = 0; i < 18; ++i) {
      const std::vector<uint8_t> obj = MakePattern(40, static_cast<uint8_t>(i));
      ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), id.data()), 0);
    }

    EXPECT_GE(H5HF_close(fh), 0);
    fh = nullptr;

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(
      H5Fopen("iblock_filt_del.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5F_t* f2 = GetFilePtr(reopened.get());
  ASSERT_NE(f2, nullptr);

  EXPECT_GE(H5HF_delete(f2, heap_addr), 0);
}

TEST(H5HFiblockTest, DirectInternalProtectPinDetachParentInfoAndErrorPaths) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    const H5HF_create_t cparam = MakeIblockHeapParams(
        /*width=*/2, /*start_block_size=*/64, /*max_direct_size=*/64,
        /*max_index=*/14, /*start_root_rows=*/1, /*max_man_size=*/44);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() {
      if (fh != nullptr) {
        H5HF_close(fh);
      }
    });
    H5HF_hdr_t* hdr = fh->hdr;

    size_t id_len = 0;
    ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
    std::vector<uint8_t> id(id_len);
    // Populate 18 blocks so row 2..4 child/grandchild indirect blocks exist.
    for (int i = 0; i < 18; ++i) {
      const std::vector<uint8_t> obj =
          MakePattern(40, static_cast<uint8_t>(i));
      ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), id.data()), 0);
    }

    // 1. H5HF__man_iblock_parent_info() for 1-level child (offset 256 -> root
    //    entry 4) and 2-level grandchild (offset 1024 + 256 = 1280 -> child
    //    at block_off=1024, entry 4).
    hsize_t par_block_off = 999;
    unsigned par_entry = 999;
    ASSERT_GE(
        H5HF__man_iblock_parent_info(hdr, /*block_off=*/256, &par_block_off,
                                     &par_entry),
        0);
    EXPECT_EQ(par_block_off, 0U);
    EXPECT_EQ(par_entry, 4U);

    ASSERT_GE(
        H5HF__man_iblock_parent_info(hdr, /*block_off=*/1280, &par_block_off,
                                     &par_entry),
        0);
    EXPECT_EQ(par_block_off, 1024U);
    EXPECT_EQ(par_entry, 4U);

    // 2. Protect root iblock with must_protect=false while pinned (did_protect=
    //    false) and with must_protect=true (did_protect=true, while already
    //    pinned).
    bool did_protect = true;
    H5HF_indirect_t* root_pinned = H5HF__man_iblock_protect(
        hdr, hdr->man_dtable.table_addr, hdr->man_dtable.curr_root_rows,
        nullptr, 0, /*must_protect=*/false, H5AC__READ_ONLY_FLAG, &did_protect);
    ASSERT_NE(root_pinned, nullptr);
    EXPECT_FALSE(did_protect);
    EXPECT_GE(H5HF__man_iblock_unprotect(root_pinned, H5AC__NO_FLAGS_SET,
                                         did_protect),
              0);

    did_protect = false;
    H5HF_indirect_t* root_prot = H5HF__man_iblock_protect(
        hdr, hdr->man_dtable.table_addr, hdr->man_dtable.curr_root_rows,
        nullptr, 0, /*must_protect=*/true, H5AC__NO_FLAGS_SET, &did_protect);
    ASSERT_NE(root_prot, nullptr);
    EXPECT_TRUE(did_protect);

    // 3. Protect child iblock at entry 4 via pinned parent vs cache protect,
    //    plus H5HF__man_iblock_entry_addr().
    haddr_t child_addr = HADDR_UNDEF;
    ASSERT_GE(H5HF__man_iblock_entry_addr(root_prot, 4, &child_addr), 0);
    EXPECT_NE(child_addr, HADDR_UNDEF);

    bool child_did_protect = false;
    H5HF_indirect_t* child_iblock = H5HF__man_iblock_protect(
        hdr, child_addr, /*iblock_nrows=*/1, root_prot, /*par_entry=*/4,
        /*must_protect=*/false, H5AC__READ_ONLY_FLAG, &child_did_protect);
    ASSERT_NE(child_iblock, nullptr);
    // Pin child_iblock, then verify must_protect=false returns pinned pointer.
    ASSERT_GE(H5HF__iblock_incr(child_iblock), 0);
    bool pinned_did_protect = true;
    H5HF_indirect_t* child_from_pin = H5HF__man_iblock_protect(
        hdr, child_addr, /*iblock_nrows=*/1, root_prot, /*par_entry=*/4,
        /*must_protect=*/false, H5AC__READ_ONLY_FLAG, &pinned_did_protect);
    EXPECT_EQ(child_from_pin, child_iblock);
    EXPECT_FALSE(pinned_did_protect);
    EXPECT_GE(H5HF__man_iblock_unprotect(child_from_pin, H5AC__NO_FLAGS_SET,
                                         pinned_did_protect),
              0);
    ASSERT_GE(H5HF__iblock_decr(child_iblock), 0);
    EXPECT_GE(H5HF__man_iblock_unprotect(child_iblock, H5AC__NO_FLAGS_SET,
                                         child_did_protect),
              0);

    // 4. Unprotect root_prot before flushing, then create a temporary child
    //    iblock at an unused indirect slot (row 4 col 1, entry 9), attach two
    //    entries (entry 2, then entry 0), flush to real file space, and detach
    //    entry 2 (max_child gap scan) then entry 0 (del_iblock->rc == 0 path).
    ASSERT_GE(H5HF__man_iblock_unprotect(root_prot, H5AC__NO_FLAGS_SET,
                                         did_protect),
              0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    bool pin_did_protect = true;
    H5HF_indirect_t* root_access = H5HF__man_iblock_protect(
        hdr, hdr->man_dtable.table_addr, hdr->man_dtable.curr_root_rows,
        nullptr, 0, /*must_protect=*/false, H5AC__NO_FLAGS_SET,
        &pin_did_protect);
    ASSERT_NE(root_access, nullptr);
    EXPECT_FALSE(pin_did_protect);

    haddr_t tmp_child_addr = HADDR_UNDEF;
    ASSERT_GE(H5HF__man_iblock_create(hdr, root_access, /*par_entry=*/9,
                                      /*nrows=*/3, /*max_rows=*/3,
                                      &tmp_child_addr),
              0);
    bool tmp_prot = false;
    H5HF_indirect_t* tmp_iblock = H5HF__man_iblock_protect(
        hdr, tmp_child_addr, /*iblock_nrows=*/3, root_access, /*par_entry=*/9,
        /*must_protect=*/true, H5AC__NO_FLAGS_SET, &tmp_prot);
    ASSERT_NE(tmp_iblock, nullptr);
    ASSERT_TRUE(tmp_prot);
    // Unprotect immediately so tmp_iblock stays in cache with rc==0, !pinned,
    // !protected to test cache guard error paths without leaks.
    ASSERT_GE(
        H5HF__man_iblock_unprotect(tmp_iblock, H5AC__NO_FLAGS_SET, tmp_prot),
        0);

    const haddr_t dummy_dblock_0 = H5MF_alloc(f, H5FD_MEM_FHEAP_DBLOCK, 64);
    const haddr_t dummy_dblock_2 = H5MF_alloc(f, H5FD_MEM_FHEAP_DBLOCK, 64);
    ASSERT_NE(dummy_dblock_0, HADDR_UNDEF);
    ASSERT_NE(dummy_dblock_2, HADDR_UNDEF);

    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5HF__iblock_incr(tmp_iblock), 0);
      EXPECT_LT(H5HF__man_iblock_attach(tmp_iblock, 0, dummy_dblock_0), 0);
      EXPECT_LT(H5HF__iblock_dirty(tmp_iblock), 0);
      EXPECT_LT(
          H5HF__man_iblock_unprotect(tmp_iblock, H5AC__NO_FLAGS_SET, true), 0);

      // rc==1 while unpinned -> decr fails H5AC_unpin_entry.
      root_access->child_iblocks[5] = tmp_iblock;
      tmp_iblock->rc = 1;
      EXPECT_LT(H5HF__iblock_decr(tmp_iblock), 0);
      EXPECT_EQ(tmp_iblock->rc, 0U);
      EXPECT_EQ(root_access->child_iblocks[5], nullptr);

      // rc==1 while unpinned & unprotected -> attach fails at dirty, and
      // H5HF__man_iblock_create(parent=tmp_iblock) fails at attach.
      tmp_iblock->rc = 1;
      EXPECT_LT(H5HF__man_iblock_attach(tmp_iblock, 0, dummy_dblock_0), 0);
      tmp_iblock->ents[0].addr = HADDR_UNDEF;
      tmp_iblock->nchildren = 0;
      tmp_iblock->max_child = 0;
      tmp_iblock->rc = 1;

      const haddr_t saved_tmp = f->shared->tmp_addr;
      haddr_t failed_child_addr = HADDR_UNDEF;
      EXPECT_LT(H5HF__man_iblock_create(hdr, tmp_iblock, /*par_entry=*/0,
                                        /*nrows=*/1, /*max_rows=*/1,
                                        &failed_child_addr),
                0);
      tmp_iblock->ents[0].addr = HADDR_UNDEF;
      tmp_iblock->nchildren = 0;
      tmp_iblock->max_child = 0;
      tmp_iblock->rc = 0;
      f->shared->tmp_addr = saved_tmp;
    }

    // Protect & pin tmp_iblock to test detach failure while protected,
    // unprotected/unpinned, and protected-but-unpinned.
    tmp_prot = false;
    tmp_iblock = H5HF__man_iblock_protect(
        hdr, tmp_child_addr, /*iblock_nrows=*/3, root_access, /*par_entry=*/9,
        /*must_protect=*/true, H5AC__NO_FLAGS_SET, &tmp_prot);
    ASSERT_NE(tmp_iblock, nullptr);
    ASSERT_GE(H5HF__iblock_incr(tmp_iblock), 0);
    ASSERT_GE(
        H5HF__man_iblock_attach(tmp_iblock, /*entry=*/0, dummy_dblock_0), 0);
    {
      ScopedErrorSilence silence;
      // Detaching the last entry tries H5HF__man_iblock_protect(..., true) on
      // an already-protected block and fails cleanly.
      EXPECT_LT(H5HF__man_iblock_detach(tmp_iblock, /*entry=*/0), 0);
      tmp_iblock->ents[0].addr = dummy_dblock_0;
      tmp_iblock->nchildren = 1;
      tmp_iblock->max_child = 0;
    }
    ASSERT_GE(
        H5HF__man_iblock_attach(tmp_iblock, /*entry=*/2, dummy_dblock_2), 0);
    // Release pins so tmp_iblock is unprotected & unpinned, then test detach
    // error paths while keeping cache consistency.
    ASSERT_GE(
        H5HF__man_iblock_unprotect(tmp_iblock, H5AC__NO_FLAGS_SET, tmp_prot),
        0);
    ASSERT_GE(H5HF__iblock_decr(tmp_iblock), 0);
    ASSERT_GE(H5HF__iblock_decr(tmp_iblock), 0);
    ASSERT_GE(H5HF__iblock_decr(tmp_iblock), 0);
    {
      ScopedErrorSilence silence;
      tmp_iblock->rc = 1;
      EXPECT_LT(H5HF__man_iblock_detach(tmp_iblock, /*entry=*/2), 0);
      tmp_iblock->ents[2].addr = dummy_dblock_2;
      tmp_iblock->nchildren = 2;
      tmp_iblock->max_child = 2;
      tmp_iblock->rc = 0;

      tmp_prot = false;
      tmp_iblock = H5HF__man_iblock_protect(
          hdr, tmp_child_addr, /*iblock_nrows=*/3, root_access, /*par_entry=*/9,
          /*must_protect=*/true, H5AC__NO_FLAGS_SET, &tmp_prot);
      ASSERT_NE(tmp_iblock, nullptr);
      tmp_iblock->rc = 1;
      root_access->child_iblocks[5] = tmp_iblock;
      EXPECT_LT(H5HF__man_iblock_detach(tmp_iblock, /*entry=*/2), 0);
      tmp_iblock->ents[2].addr = dummy_dblock_2;
      tmp_iblock->nchildren = 2;
      tmp_iblock->max_child = 2;
      EXPECT_EQ(tmp_iblock->rc, 0U);
      EXPECT_EQ(root_access->child_iblocks[5], nullptr);
    }
    // Restore pin state matching the 2 attached dummy entries, then flush.
    ASSERT_GE(H5HF__iblock_incr(tmp_iblock), 0);
    tmp_iblock->rc = 2;
    EXPECT_GE(
        H5HF__man_iblock_unprotect(tmp_iblock, H5AC__NO_FLAGS_SET, tmp_prot),
        0);

    // Flush so tmp_iblock moves to real file space before detaching to 0.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    // Re-acquire pinned tmp_iblock and detach its entries cleanly.
    pin_did_protect = true;
    root_access = H5HF__man_iblock_protect(
        hdr, hdr->man_dtable.table_addr, hdr->man_dtable.curr_root_rows,
        nullptr, 0, /*must_protect=*/false, H5AC__NO_FLAGS_SET,
        &pin_did_protect);
    ASSERT_NE(root_access, nullptr);
    EXPECT_FALSE(pin_did_protect);
    tmp_prot = true;
    tmp_iblock = H5HF__man_iblock_protect(
        hdr, root_access->ents[9].addr, /*iblock_nrows=*/3, root_access,
        /*par_entry=*/9, /*must_protect=*/false, H5AC__NO_FLAGS_SET,
        &tmp_prot);
    ASSERT_NE(tmp_iblock, nullptr);
    EXPECT_FALSE(tmp_prot);
    ASSERT_GE(H5HF__man_iblock_detach(tmp_iblock, /*entry=*/2), 0);
    EXPECT_EQ(tmp_iblock->max_child, 0U);
    // Detaching the last entry destroys tmp_iblock with rc == 0 and detaches
    // entry 9 from root_access.
    ASSERT_GE(H5HF__man_iblock_detach(tmp_iblock, /*entry=*/0), 0);
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_FHEAP_DBLOCK, dummy_dblock_0, 64), 0);
    EXPECT_GE(H5MF_xfree(f, H5FD_MEM_FHEAP_DBLOCK, dummy_dblock_2, 64), 0);

    // 5. Error paths for invalid indirect block address.
    {
      ScopedErrorSilence silence;
      const haddr_t bad_addr = hdr->heap_addr + 1;
      bool bad_did_protect = false;
      EXPECT_EQ(H5HF__man_iblock_protect(hdr, bad_addr, /*iblock_nrows=*/1,
                                         nullptr, 0, /*must_protect=*/false,
                                         H5AC__READ_ONLY_FLAG, &bad_did_protect),
                nullptr);
      hsize_t bad_size = 0;
      EXPECT_LT(
          H5HF__man_iblock_size(f, hdr, bad_addr, /*nrows=*/1, nullptr, 0,
                                &bad_size),
          0);
      EXPECT_LT(
          H5HF__man_iblock_delete(hdr, bad_addr, /*iblock_nrows=*/1, nullptr,
                                  0),
          0);
    }

    EXPECT_GE(H5HF_close(fh), 0);
    fh = nullptr;
  }
  EXPECT_GE(H5Fclose(file.release()), 0);
}

TEST(H5HFiblockTest, RootCreateDoubleHalveAndCorruptDeleteErrorPaths) {
  // 1. Test root_create/iblock_create cache-insert collision and
  //    root_double / root_halve allocation/move/xfree error paths.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = GetFilePtr(file.get());
      ASSERT_NE(f, nullptr);

      const H5HF_create_t cparam = MakeIblockHeapParams(
          /*width=*/2, /*start_block_size=*/64, /*max_direct_size=*/512,
          /*max_index=*/14, /*start_root_rows=*/2, /*max_man_size=*/450);
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      auto close_fh = absl::MakeCleanup([&]() {
        if (fh != nullptr) {
          H5HF_close(fh);
        }
      });
      H5HF_hdr_t* hdr = fh->hdr;

      size_t id_len = 0;
      ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
      std::vector<uint8_t> id0(id_len);
      const std::vector<uint8_t> obj40 = MakePattern(40, 0x11U);
      ASSERT_GE(H5HF_insert(fh, obj40.size(), obj40.data(), id0.data()), 0);
      ASSERT_EQ(hdr->man_dtable.curr_root_rows, 0U);

      // Force H5HF__man_iblock_create() inside root_create() to allocate the
      // already-cached root direct block tmp address -> H5AC_insert_entry
      // fails cleanly and H5HF__man_iblock_dest() releases the block.
      {
        ScopedErrorSilence silence;
        const haddr_t saved_tmp = f->shared->tmp_addr;
        const hsize_t create_sz = H5HF_MAN_INDIRECT_SIZE(hdr, 2);
        f->shared->tmp_addr = hdr->man_dtable.table_addr + create_sz;
        EXPECT_LT(H5HF__man_iblock_root_create(hdr, 64), 0);
        f->shared->tmp_addr = saved_tmp;
      }

      // Fill rows 0..1 (entries 0..3: four 64B blocks) so root_iblock
      // (nrows=2, in temp space) is ready to double on the next insert.
      const std::vector<uint8_t> obj100 = MakePattern(100, 0x22U);
      std::vector<uint8_t> id1(id_len), id2(id_len), id3(id_len);
      ASSERT_GE(H5HF_insert(fh, obj40.size(), obj40.data(), id1.data()), 0);
      ASSERT_GE(H5HF_insert(fh, obj40.size(), obj40.data(), id2.data()), 0);
      ASSERT_GE(H5HF_insert(fh, obj40.size(), obj40.data(), id3.data()), 0);
      ASSERT_EQ(hdr->man_dtable.curr_root_rows, 2U);

      bool did_protect = true;
      H5HF_indirect_t* root_iblock = H5HF__man_iblock_protect(
          hdr, hdr->man_dtable.table_addr, hdr->man_dtable.curr_root_rows,
          nullptr, 0, /*must_protect=*/false, H5AC__NO_FLAGS_SET, &did_protect);
      ASSERT_NE(root_iblock, nullptr);
      ASSERT_FALSE(did_protect);

      const haddr_t dblock3_addr = root_iblock->ents[3].addr;
      ASSERT_NE(dblock3_addr, HADDR_UNDEF);
      const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      ASSERT_NE(eoa, HADDR_UNDEF);
      const unsigned old_nrows = root_iblock->nrows;
      const size_t old_size = root_iblock->size;
      const hsize_t double_sz = H5HF_MAN_INDIRECT_SIZE(hdr, 4);
      ASSERT_LE(dblock3_addr + double_sz, root_iblock->addr);

      // root_double tmp allocation failure (H5MF_alloc_tmp), real allocation
      // failure (H5MF_alloc), and cache move collision (H5AC_move_entry).
      {
        ScopedErrorSilence silence;
        std::vector<uint8_t> tmp_id(id_len);

        const haddr_t saved_tmp = f->shared->tmp_addr;
        f->shared->tmp_addr = eoa + 8;
        EXPECT_LT(H5HF_insert(fh, obj100.size(), obj100.data(), tmp_id.data()),
                  0);
        root_iblock->nrows = old_nrows;
        root_iblock->size = old_size;
        f->shared->tmp_addr = saved_tmp;

        const bool saved_use_tmp = f->shared->use_tmp_space;
        const H5F_blk_aggr_t saved_aggr = f->shared->meta_aggr;
        f->shared->use_tmp_space = false;
        f->shared->meta_aggr.addr = 0;
        f->shared->meta_aggr.size = 0;
        if (f->shared->meta_aggr.alloc_size <= eoa + 4096) {
          f->shared->meta_aggr.alloc_size = eoa + 4096;
        }
        f->shared->tmp_addr = eoa + 8;
        EXPECT_LT(H5HF_insert(fh, obj100.size(), obj100.data(), tmp_id.data()),
                  0);
        root_iblock->nrows = old_nrows;
        root_iblock->size = old_size;
        f->shared->use_tmp_space = saved_use_tmp;
        f->shared->meta_aggr = saved_aggr;
        f->shared->tmp_addr = saved_tmp;

        f->shared->tmp_addr = dblock3_addr + double_sz;
        EXPECT_LT(H5HF_insert(fh, obj100.size(), obj100.data(), tmp_id.data()),
                  0);
        EXPECT_GE(H5AC_resize_entry(root_iblock, old_size), 0);
        root_iblock->nrows = old_nrows;
        root_iblock->size = old_size;
        f->shared->tmp_addr = saved_tmp;
      }

      // Now double root_iblock normally to nrows=4 (entry 4 = 128B allocated).
      std::vector<uint8_t> id4(id_len);
      ASSERT_GE(H5HF_insert(fh, obj100.size(), obj100.data(), id4.data()), 0);
      ASSERT_EQ(hdr->man_dtable.curr_root_rows, 4U);
      did_protect = true;
      root_iblock = H5HF__man_iblock_protect(
          hdr, hdr->man_dtable.table_addr, hdr->man_dtable.curr_root_rows,
          nullptr, 0, /*must_protect=*/false, H5AC__NO_FLAGS_SET, &did_protect);
      ASSERT_NE(root_iblock, nullptr);
      ASSERT_FALSE(did_protect);
      const haddr_t dblock4_addr = root_iblock->ents[4].addr;
      ASSERT_NE(dblock4_addr, HADDR_UNDEF);

      // Attach a dummy entry at slot 7 so detaching slot 7 triggers
      // H5HF__man_iblock_root_halve(), and inject tmp error paths first.
      const haddr_t dummy7 = H5MF_alloc(f, H5FD_MEM_FHEAP_DBLOCK, 64);
      ASSERT_NE(dummy7, HADDR_UNDEF);
      const haddr_t eoa_after = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      ASSERT_NE(eoa_after, HADDR_UNDEF);
      ASSERT_GE(H5HF__man_iblock_attach(root_iblock, /*entry=*/7, dummy7), 0);

      {
        ScopedErrorSilence silence;
        const unsigned h_nrows = root_iblock->nrows;
        const size_t h_size = root_iblock->size;
        const haddr_t saved_tmp = f->shared->tmp_addr;

        // 1) H5MF_alloc_tmp failure in root_halve
        f->shared->tmp_addr = eoa_after + 8;
        EXPECT_LT(H5HF__man_iblock_detach(root_iblock, 7), 0);
        root_iblock->nrows = h_nrows;
        root_iblock->size = h_size;
        f->shared->tmp_addr = saved_tmp;
        ASSERT_GE(H5HF__iblock_decr(root_iblock), 0);
        ASSERT_GE(H5HF__man_iblock_attach(root_iblock, 7, dummy7), 0);

        // 2) H5MF_alloc failure in root_halve
        const bool saved_use_tmp = f->shared->use_tmp_space;
        const H5F_blk_aggr_t saved_aggr = f->shared->meta_aggr;
        f->shared->use_tmp_space = false;
        f->shared->meta_aggr.addr = 0;
        f->shared->meta_aggr.size = 0;
        if (f->shared->meta_aggr.alloc_size <= eoa_after + 4096) {
          f->shared->meta_aggr.alloc_size = eoa_after + 4096;
        }
        f->shared->tmp_addr = eoa_after + 8;
        EXPECT_LT(H5HF__man_iblock_detach(root_iblock, 7), 0);
        root_iblock->nrows = h_nrows;
        root_iblock->size = h_size;
        f->shared->use_tmp_space = saved_use_tmp;
        f->shared->meta_aggr = saved_aggr;
        f->shared->tmp_addr = saved_tmp;
        ASSERT_GE(H5HF__iblock_decr(root_iblock), 0);
        ASSERT_GE(H5HF__man_iblock_attach(root_iblock, 7, dummy7), 0);

        // 3) H5AC_move_entry collision in root_halve
        ASSERT_LE(dblock4_addr + h_size, root_iblock->addr);
        f->shared->tmp_addr = dblock4_addr + h_size;
        EXPECT_LT(H5HF__man_iblock_detach(root_iblock, 7), 0);
        root_iblock->nrows = h_nrows;
        root_iblock->size = h_size;
        f->shared->tmp_addr = saved_tmp;
        ASSERT_GE(H5HF__iblock_decr(root_iblock), 0);
        ASSERT_GE(H5HF__man_iblock_attach(root_iblock, 7, dummy7), 0);
      }

      // Flush so root_iblock is in real file space, then trigger H5MF_xfree
      // failure inside root_halve() and clean up dummy7.
      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
      did_protect = true;
      root_iblock = H5HF__man_iblock_protect(
          hdr, hdr->man_dtable.table_addr, hdr->man_dtable.curr_root_rows,
          nullptr, 0, /*must_protect=*/false, H5AC__NO_FLAGS_SET, &did_protect);
      ASSERT_NE(root_iblock, nullptr);

      const H5FD_mem_t iblock_mem_type =
          (f->shared->fs_type_map[H5FD_MEM_FHEAP_IBLOCK] == H5FD_MEM_DEFAULT)
              ? H5FD_MEM_FHEAP_IBLOCK
              : f->shared->fs_type_map[H5FD_MEM_FHEAP_IBLOCK];
      const auto iblock_fs_type = static_cast<H5F_mem_page_t>(iblock_mem_type);
      if (f->shared->fs_man[iblock_fs_type] == nullptr &&
          f->shared->fs_addr[iblock_fs_type] == HADDR_UNDEF) {
        ScopedErrorSilence silence;
        f->shared->fs_addr[iblock_fs_type] = hdr->heap_addr;
        EXPECT_LT(H5HF__man_iblock_detach(root_iblock, 7), 0);
        f->shared->fs_addr[iblock_fs_type] = HADDR_UNDEF;
        ASSERT_GE(H5HF__iblock_decr(root_iblock), 0);
        ASSERT_GE(H5HF__man_iblock_attach(root_iblock, 7, dummy7), 0);
      }
      ASSERT_GE(H5HF__man_iblock_detach(root_iblock, 7), 0);

      // Fill remaining row 2..3 entries (5=128B, 6=256B, 7=256B), flush so
      // root_iblock (nrows=4) is in real space, and trigger H5MF_xfree failure
      // in root_double().
      std::vector<uint8_t> id5(id_len), id6(id_len), id7(id_len);
      const std::vector<uint8_t> obj200 = MakePattern(200, 0x33U);
      const std::vector<uint8_t> obj400 = MakePattern(400, 0x55U);
      ASSERT_GE(H5HF_insert(fh, obj100.size(), obj100.data(), id5.data()), 0);
      ASSERT_GE(H5HF_insert(fh, obj200.size(), obj200.data(), id6.data()), 0);
      ASSERT_GE(H5HF_insert(fh, obj200.size(), obj200.data(), id7.data()), 0);
      ASSERT_EQ(hdr->man_dtable.curr_root_rows, 4U);
      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

      if (f->shared->fs_man[iblock_fs_type] == nullptr &&
          f->shared->fs_addr[iblock_fs_type] == HADDR_UNDEF) {
        ScopedErrorSilence silence;
        std::vector<uint8_t> id8(id_len);
        f->shared->fs_addr[iblock_fs_type] = hdr->heap_addr;
        EXPECT_LT(H5HF_insert(fh, obj400.size(), obj400.data(), id8.data()), 0);
        f->shared->fs_addr[iblock_fs_type] = HADDR_UNDEF;
      }

      EXPECT_GE(H5MF_xfree(f, H5FD_MEM_FHEAP_DBLOCK, dummy7, 64), 0);
      EXPECT_GE(H5HF_close(fh), 0);
      fh = nullptr;
    }
    EXPECT_GE(H5Fclose(file.release()), 0);
  }

  // 2. Build a 2-level heap image so we can test:
  //    (a) H5HF__man_dblock_delete() failure in H5HF__man_iblock_delete(), and
  //    (b) corrupt child "FHIB" failure in H5HF_size() and H5HF_delete().
  haddr_t heap_addr = HADDR_UNDEF;
  haddr_t child_iblock_addr = HADDR_UNDEF;
  std::vector<uint8_t> base_image;
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = GetFilePtr(file.get());
      ASSERT_NE(f, nullptr);

      const H5HF_create_t cparam = MakeIblockHeapParams(
          /*width=*/2, /*start_block_size=*/64, /*max_direct_size=*/64,
          /*max_index=*/14, /*start_root_rows=*/1, /*max_man_size=*/44);
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);

      size_t id_len = 0;
      ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
      std::vector<uint8_t> id(id_len);
      for (int i = 0; i < 6; ++i) {
        const std::vector<uint8_t> obj =
            MakePattern(40, static_cast<uint8_t>(i + 0x10));
        ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), id.data()), 0);
      }

      ASSERT_GE(H5HF_close(fh), 0);
      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

      fh = H5HF_open(f, heap_addr);
      ASSERT_NE(fh, nullptr);
      H5HF_hdr_t* hdr = fh->hdr;
      bool root_prot_flag = false;
      H5HF_indirect_t* root_iblock = H5HF__man_iblock_protect(
          hdr, hdr->man_dtable.table_addr, hdr->man_dtable.curr_root_rows,
          nullptr, 0, /*must_protect=*/false, H5AC__READ_ONLY_FLAG,
          &root_prot_flag);
      ASSERT_NE(root_iblock, nullptr);
      ASSERT_GE(
          H5HF__man_iblock_entry_addr(root_iblock, 4, &child_iblock_addr), 0);
      ASSERT_NE(child_iblock_addr, HADDR_UNDEF);
      ASSERT_GE(
          H5HF__man_iblock_unprotect(root_iblock, H5AC__NO_FLAGS_SET,
                                     root_prot_flag),
          0);
      ASSERT_GE(H5HF_close(fh), 0);

      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
      const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
      ASSERT_GT(image_size, 0);
      base_image.resize(static_cast<size_t>(image_size));
      ASSERT_EQ(
          H5Fget_file_image(file.get(), base_image.data(), base_image.size()),
          image_size);
    }
    EXPECT_GE(H5Fclose(file.release()), 0);
  }

  // (a) Fail H5MF_xfree() inside H5HF__man_dblock_delete() called by
  //     H5HF__man_iblock_delete(), then restore and delete cleanly.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_file_image(fapl.get(), base_image.data(), base_image.size()), 0);
    ScopedFile reopened(
        H5Fopen("iblock_delete_xfree_fail.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(reopened.valid());

    {
      ScopedApiContext ctx2;
      ASSERT_TRUE(ctx2.valid());
      H5F_t* f2 = GetFilePtr(reopened.get());
      ASSERT_NE(f2, nullptr);

      // Pin newly loaded root_iblock (rc == 0, unpinned) with
      // root_iblock_flags == 0 to cover root_iblock initialization in
      // H5HF__iblock_pin(), then unprotect, unpin, and close before delete.
      {
        H5HF_t* fh_pin = H5HF_open(f2, heap_addr);
        ASSERT_NE(fh_pin, nullptr);
        H5HF_hdr_t* hdr_pin = fh_pin->hdr;
        bool pin_prot = false;
        H5HF_indirect_t* root_prot = H5HF__man_iblock_protect(
            hdr_pin, hdr_pin->man_dtable.table_addr,
            hdr_pin->man_dtable.curr_root_rows, nullptr, 0,
            /*must_protect=*/true, H5AC__NO_FLAGS_SET, &pin_prot);
        ASSERT_NE(root_prot, nullptr);
        EXPECT_TRUE(pin_prot);
        EXPECT_EQ(root_prot->rc, 0U);
        const unsigned saved_root_flags = hdr_pin->root_iblock_flags;
        H5HF_indirect_t* const saved_root_ptr = hdr_pin->root_iblock;
        hdr_pin->root_iblock_flags = 0;
        hdr_pin->root_iblock = nullptr;
        ASSERT_GE(H5HF__iblock_incr(root_prot), 0);
        EXPECT_EQ(hdr_pin->root_iblock, saved_root_ptr);
        hdr_pin->root_iblock_flags |= saved_root_flags;
        EXPECT_GE(H5HF__man_iblock_unprotect(root_prot, H5AC__NO_FLAGS_SET,
                                             pin_prot),
                  0);
        ASSERT_GE(H5HF__iblock_decr(root_prot), 0);
        EXPECT_GE(H5HF_close(fh_pin), 0);
      }

      const H5FD_mem_t dblock_mem_type =
          (f2->shared->fs_type_map[H5FD_MEM_FHEAP_DBLOCK] == H5FD_MEM_DEFAULT)
              ? H5FD_MEM_FHEAP_DBLOCK
              : f2->shared->fs_type_map[H5FD_MEM_FHEAP_DBLOCK];
      const auto dblock_fs_type =
          static_cast<H5F_mem_page_t>(dblock_mem_type);
      if (f2->shared->fs_man[dblock_fs_type] == nullptr &&
          f2->shared->fs_addr[dblock_fs_type] == HADDR_UNDEF) {
        ScopedErrorSilence silence;
        f2->shared->fs_addr[dblock_fs_type] = heap_addr;
        EXPECT_LT(H5HF_delete(f2, heap_addr), 0);
        f2->shared->fs_addr[dblock_fs_type] = HADDR_UNDEF;
      }
      EXPECT_GE(H5HF_delete(f2, heap_addr), 0);
    }
    EXPECT_GE(H5Fclose(reopened.release()), 0);
  }

  // (b) Corrupt child indirect block "FHIB" and exercise H5HF_size() and
  //     H5HF_delete() recursive child failure paths cleanly.
  {
    std::vector<uint8_t> corrupt_image = base_image;
    ASSERT_LE(child_iblock_addr + 4, corrupt_image.size());
    ASSERT_EQ(std::memcmp(corrupt_image.data() + child_iblock_addr, "FHIB", 4),
              0);
    corrupt_image[child_iblock_addr] = 'X';

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_file_image(fapl.get(), corrupt_image.data(),
                          corrupt_image.size()),
        0);
    ScopedFile reopened(
        H5Fopen("iblock_corrupt_child.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(reopened.valid());

    {
      ScopedApiContext ctx2;
      ASSERT_TRUE(ctx2.valid());
      H5F_t* f2 = GetFilePtr(reopened.get());
      ASSERT_NE(f2, nullptr);

      H5HF_t* fh2 = H5HF_open(f2, heap_addr);
      ASSERT_NE(fh2, nullptr);
      {
        ScopedErrorSilence silence;
        hsize_t heap_size = 0;
        EXPECT_LT(H5HF_size(fh2, &heap_size), 0);
      }
      EXPECT_GE(H5HF_close(fh2), 0);

      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5HF_delete(f2, heap_addr), 0);
      }
    }
    EXPECT_GE(H5Fclose(reopened.release()), 0);
  }
}

}  // namespace
}  // namespace hdf5
