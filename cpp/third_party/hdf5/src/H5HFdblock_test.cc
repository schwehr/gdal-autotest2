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
// Tests fractal heap direct block allocation, creation, protection, multi-level
// location, destruction, and deletion for filtered/unfiltered and temporary vs
// real file-space paths (third_party/hdf5/src/H5HFdblock.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFdblock.c

#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t) inside internal fractal heap structs.
#ifndef NDEBUG
#define H5HFDBLOCK_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5HFpkg.h pulls in H5B2private.h, which uses `try` as a C parameter name,
// colliding with both the C++ keyword and Google3's `-fno-exceptions` macro.
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
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#include "third_party/hdf5/src/H5Zprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HFDBLOCK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HFDBLOCK_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

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

H5HF_create_t MakeHeapParams(size_t start_block_size = 64,
                             size_t max_direct_size = 256,
                             unsigned width = 2, unsigned max_index = 16,
                             unsigned start_root_rows = 1,
                             uint32_t max_man_size = 200,
                             bool checksum_dblocks = true) {
  H5HF_create_t cparam{};
  cparam.managed.width = width;
  cparam.managed.start_block_size = start_block_size;
  cparam.managed.max_direct_size = max_direct_size;
  cparam.managed.max_index = max_index;
  cparam.managed.start_root_rows = start_root_rows;
  cparam.checksum_dblocks = checksum_dblocks;
  cparam.max_man_size = max_man_size;
  cparam.id_len = 0;
  return cparam;
}

std::vector<uint8_t> MakeData(size_t len, uint8_t seed) {
  std::vector<uint8_t> buf(len);
  for (size_t i = 0; i < len; ++i) {
    buf[i] = static_cast<uint8_t>((seed + i * 7) & 0xffU);
  }
  return buf;
}

TEST(H5HFdblockTest, NewCreateProtectDestroyRootAndChildFilteredAndUnfiltered) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  // Test both unfiltered and DEFLATE-filtered heaps through:
  // - root dblock create (tmp space AND real space by toggling use_tmp_space)
  // - min_dblock_size *= 2 overhead bump
  // - root dblock destroy via last object removal -> H5HF__hdr_empty
  // - non-root dblock creation across rows -> highest and non-highest destroy
  for (bool filtered : {false, true}) {
    H5HF_create_t cparam = MakeHeapParams();
    absl::Cleanup reset_pline = [&] {
      if (cparam.pline.nused > 0) {
        H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
      }
    };
    if (filtered) {
      unsigned int cd_vals[1] = {1};
      ASSERT_GE(H5Z_append(&cparam.pline, H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL,
                           1, cd_vals),
                0);
    }

    // 1. Real file space allocation (use_tmp_space = false) on root dblock
    //    followed by removing its sole object so destroy hits !H5F_IS_TMP_ADDR
    //    with parent_removed == NULL and curr_root_rows == 0.
    {
      const bool saved_tmp = f->shared->use_tmp_space;
      f->shared->use_tmp_space = false;
      absl::Cleanup restore_tmp = [&] {
        f->shared->use_tmp_space = saved_tmp;
      };

      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      absl::Cleanup close_fh = [&] { H5HF_close(fh); };

      std::vector<uint8_t> id(fh->hdr->id_len, 0);
      const std::vector<uint8_t> obj = MakeData(24, 0x10U);
      ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), id.data()), 0);
      EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 0U);
      EXPECT_FALSE(H5F_IS_TMP_ADDR(f, fh->hdr->man_dtable.table_addr));

      std::vector<uint8_t> got(obj.size(), 0);
      EXPECT_GE(H5HF_read(fh, id.data(), got.data()), 0);
      EXPECT_THAT(got, ElementsAreArray(obj));

      // Removing the only object destroys the root direct block via shrink.
      EXPECT_GE(H5HF_remove(fh, id.data()), 0);
      EXPECT_EQ(fh->hdr->man_dtable.table_addr, HADDR_UNDEF);
    }

    // 2. Request < start_block_size needing overhead bump (52 + 20 > 64 -> 128)
    //    and request >= start_block_size needing overhead bump (120 + 20 > 128
    //    -> 256), plus destroying non-highest & highest non-root direct blocks.
    {
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      absl::Cleanup close_fh = [&] { H5HF_close(fh); };

      const std::vector<uint8_t> small_obj = MakeData(24, 0x20U);
      const std::vector<uint8_t> bump_small = MakeData(52, 0x30U);
      const std::vector<uint8_t> bump_med = MakeData(120, 0x40U);

      std::vector<uint8_t> id0(fh->hdr->id_len, 0);
      std::vector<uint8_t> id1(fh->hdr->id_len, 0);
      std::vector<uint8_t> id2(fh->hdr->id_len, 0);
      std::vector<uint8_t> id3(fh->hdr->id_len, 0);

      ASSERT_GE(H5HF_insert(fh, small_obj.size(), small_obj.data(), id0.data()),
                0);
      ASSERT_GE(
          H5HF_insert(fh, bump_small.size(), bump_small.data(), id1.data()), 0);
      ASSERT_GE(H5HF_insert(fh, bump_med.size(), bump_med.data(), id2.data()),
                0);
      ASSERT_GE(H5HF_insert(fh, bump_med.size(), bump_med.data(), id3.data()),
                0);
      EXPECT_GT(fh->hdr->man_dtable.curr_root_rows, 0U);

      // Destroy non-highest direct block id1 (hits curr_root_rows > 0 without
      // reverse_iter), then highest direct block id3 (hits reverse_iter).
      EXPECT_GE(H5HF_remove(fh, id1.data()), 0);
      EXPECT_GE(H5HF_remove(fh, id3.data()), 0);
      // Destroy id2 and finally id0 -> reverts/empties cleanly.
      EXPECT_GE(H5HF_remove(fh, id2.data()), 0);
      EXPECT_GE(H5HF_remove(fh, id0.data()), 0);
    }
  }
  // Also exercise parent_removed == true in H5HF__man_dblock_destroy when
  // removing the sole direct block in a child indirect block while the root
  // indirect block still has other children.
  {
    const H5HF_create_t cparam = MakeHeapParams(
        /*start_block_size=*/64, /*max_direct_size=*/256, /*width=*/2,
        /*max_index=*/16, /*start_root_rows=*/2, /*max_man_size=*/200);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    absl::Cleanup close_fh = [&] { H5HF_close(fh); };

    std::vector<uint8_t> id(fh->hdr->id_len, 0);
    for (int i = 0; i < 4; ++i) {
      ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, i).data(), id.data()), 0);
    }
    for (int i = 0; i < 2; ++i) {
      ASSERT_GE(H5HF_insert(fh, 100, MakeData(100, 10 + i).data(), id.data()),
                0);
    }
    for (int i = 0; i < 2; ++i) {
      ASSERT_GE(H5HF_insert(fh, 200, MakeData(200, 20 + i).data(), id.data()),
                0);
    }
    // Creates child indirect block at (row 4, col 0) with a single direct block.
    std::vector<uint8_t> child_id(fh->hdr->id_len, 0);
    ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, 30).data(), child_id.data()), 0);
    EXPECT_GE(H5HF_remove(fh, child_id.data()), 0);
  }
  EXPECT_GE(H5Fclose(file.release()), 0);
}

TEST(H5HFdblockTest, MultiLevelLocateCreatesMissingChildIblockAndFailsBadAddr) {
  std::vector<uint8_t> image;
  haddr_t reopened_heap_addr = HADDR_UNDEF;

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    const H5HF_create_t cparam = MakeHeapParams(
        /*start_block_size=*/64, /*max_direct_size=*/256, /*width=*/2,
        /*max_index=*/16, /*start_root_rows=*/1, /*max_man_size=*/200);

    // 1. Multi-level locate on an existing child indirect block at row 5
    //    (width=2, start_block_size=64, max_direct_size=256 => max_direct_rows
    //    = 4; inserting 200B objects skips row 4 and enters row 5!).
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    reopened_heap_addr = fh->hdr->heap_addr;

    std::vector<std::vector<uint8_t>> ids;
    const std::vector<uint8_t> blk_filler = MakeData(200, 0x55U);
    for (int i = 0; i < 6; ++i) {
      std::vector<uint8_t> id(fh->hdr->id_len, 0);
      ASSERT_GE(
          H5HF_insert(fh, blk_filler.size(), blk_filler.data(), id.data()), 0);
      ids.push_back(id);
    }
    ASSERT_GT(fh->hdr->man_dtable.curr_root_rows, 5U);

    // Read objects spanning multiple levels (exercises while (row >= max_direct)
    // in H5HF__man_dblock_locate).
    std::vector<uint8_t> got(blk_filler.size(), 0);
    for (const auto& id : ids) {
      EXPECT_GE(H5HF_read(fh, id.data(), got.data()), 0);
      EXPECT_THAT(got, ElementsAreArray(blk_filler));
    }

    // Call H5HF__man_dblock_locate directly with ret_entry == nullptr on row 5
    // (where the child indirect block already exists).
    H5HF_hdr_t* hdr = fh->hdr;
    {
      H5HF_indirect_t* located_iblock = nullptr;
      bool did_protect = false;
      const hsize_t row5_off = hdr->man_dtable.row_block_off[5];
      ASSERT_GE(H5HF__man_dblock_locate(hdr, row5_off, &located_iblock, nullptr,
                                        &did_protect, H5AC__READ_ONLY_FLAG),
                0);
      EXPECT_NE(located_iblock, nullptr);
      EXPECT_GE(H5HF__man_iblock_unprotect(located_iblock, H5AC__NO_FLAGS_SET,
                                           did_protect),
                0);
    }

    // Negative protect test: flush heap header to disk and attempt to protect
    // its address as H5AC_FHEAP_DBLOCK (fails checksum/magic check cleanly).
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5HF__man_dblock_protect(hdr, hdr->heap_addr, 64, nullptr, 0,
                                         H5AC__READ_ONLY_FLAG),
                nullptr);
    }
    EXPECT_GE(H5HF_close(fh), 0);

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
    EXPECT_GE(H5Fclose(file.release()), 0);
  }

  // Reopen file image so free-space row sections covering skipped row 4 are
  // serialized; inserting a 40B object revives the row section and calls
  // H5HF__man_dblock_locate with !H5_addr_defined(iblock_addr) to allocate the
  // missing child indirect block at row 4 and immediately populate a direct
  // block inside it.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(
      H5Fopen("dblock_locate_reopen.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5HF_t* fh = H5HF_open(GetFilePtr(reopened.get()), reopened_heap_addr);
  ASSERT_NE(fh, nullptr);
  const std::vector<uint8_t> small_obj = MakeData(40, 0x7aU);
  std::vector<uint8_t> small_id(fh->hdr->id_len, 0);
  ASSERT_GE(
      H5HF_insert(fh, small_obj.size(), small_obj.data(), small_id.data()), 0);
  std::vector<uint8_t> got_small(small_obj.size(), 0);
  EXPECT_GE(H5HF_read(fh, small_id.data(), got_small.data()), 0);
  EXPECT_THAT(got_small, ElementsAreArray(small_obj));
  EXPECT_GE(H5HF_close(fh), 0);
  EXPECT_GE(H5Fclose(reopened.release()), 0);
}

TEST(H5HFdblockTest, DeleteCachedTmpRealAndUncachedAfterReopen) {
  haddr_t uncached_heap_addr = HADDR_UNDEF;
  std::vector<uint8_t> image;

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    const H5HF_create_t cparam = MakeHeapParams();

    // 1. Delete heap immediately without flush: blocks are in cache & at tmp
    //    addresses (dblock_status & IN_CACHE, H5F_IS_TMP_ADDR true).
    {
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      haddr_t addr = fh->hdr->heap_addr;
      std::vector<uint8_t> id(fh->hdr->id_len, 0);
      const std::vector<uint8_t> obj = MakeData(24, 0x66U);
      ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), id.data()), 0);
      EXPECT_GE(H5HF_close(fh), 0);
      EXPECT_GE(H5HF_delete(f, addr), 0);
    }

    // 2. Create heap, flush so blocks remain in cache at REAL file addresses,
    //    then delete (IN_CACHE && !H5F_IS_TMP_ADDR -> expunge + H5MF_xfree).
    {
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      haddr_t addr = fh->hdr->heap_addr;
      std::vector<uint8_t> id(fh->hdr->id_len, 0);
      const std::vector<uint8_t> obj = MakeData(24, 0x77U);
      ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), id.data()), 0);
      EXPECT_GE(H5HF_close(fh), 0);
      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
      EXPECT_GE(H5HF_delete(f, addr), 0);
    }

    // 3. Create heap with root + child dblocks, flush, and save image so on
    //    reopen direct blocks are NOT in cache.
    {
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      uncached_heap_addr = fh->hdr->heap_addr;
      std::vector<uint8_t> id1(fh->hdr->id_len, 0);
      std::vector<uint8_t> id2(fh->hdr->id_len, 0);
      const std::vector<uint8_t> o1 = MakeData(24, 0x88U);
      const std::vector<uint8_t> o2 = MakeData(100, 0x99U);
      ASSERT_GE(H5HF_insert(fh, o1.size(), o1.data(), id1.data()), 0);
      ASSERT_GE(H5HF_insert(fh, o2.size(), o2.data(), id2.data()), 0);
      EXPECT_GE(H5HF_close(fh), 0);
    }

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  // Reopen image and delete uncached heap -> H5HF__man_dblock_delete takes
  // !(dblock_status & H5AC_ES__IN_CACHE) && !H5F_IS_TMP_ADDR!
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("dblock_reopen.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  EXPECT_GE(H5HF_delete(GetFilePtr(reopened.get()), uncached_heap_addr), 0);
}

TEST(H5HFdblockTest, LocateMissingChildIblockAndAllocationDestroyErrors) {
  // 1. Missing child indirect block at skipped row 4 with pinned root iblock:
  //    test allocation failure (550), protect failure (559), clean missing
  //    child creation (549, 553, 554), plus reopened root protect failure (528).
  std::vector<uint8_t> image;
  haddr_t reopen_addr = HADDR_UNDEF;
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    const H5HF_create_t cparam = MakeHeapParams(
        /*start_block_size=*/64, /*max_direct_size=*/256, /*width=*/2,
        /*max_index=*/16, /*start_root_rows=*/2, /*max_man_size=*/200);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    reopen_addr = fh->hdr->heap_addr;

    std::vector<uint8_t> id(fh->hdr->id_len, 0);
    // Fill root rows 0..3 completely, then insert 200B -> skips row 4 and
    // enters row 5, keeping root_iblock pinned with ents[8].addr == HADDR_UNDEF.
    for (int i = 0; i < 4; ++i) {
      ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, i).data(), id.data()), 0);
    }
    for (int i = 0; i < 2; ++i) {
      ASSERT_GE(H5HF_insert(fh, 100, MakeData(100, 10 + i).data(), id.data()),
                0);
    }
    for (int i = 0; i < 2; ++i) {
      ASSERT_GE(H5HF_insert(fh, 200, MakeData(200, 20 + i).data(), id.data()),
                0);
    }
    ASSERT_GE(H5HF_insert(fh, 200, MakeData(200, 30).data(), id.data()), 0);

    H5HF_hdr_t* hdr = fh->hdr;
    ASSERT_EQ(hdr->root_iblock_flags, H5HF_ROOT_IBLOCK_PINNED);
    ASSERT_NE(hdr->root_iblock, nullptr);
    ASSERT_EQ(hdr->root_iblock->ents[8].addr, HADDR_UNDEF);
    const hsize_t row4_off = hdr->man_dtable.row_block_off[4];

    {
      ScopedErrorSilence silence;
      const unsigned saved_nchildren = hdr->root_iblock->nchildren;
      const unsigned saved_max_child = hdr->root_iblock->max_child;
      const unsigned saved_flags = f->shared->flags;
      f->shared->flags &= ~H5F_ACC_RDWR;
      absl::Cleanup restore_state = [&] {
        f->shared->flags = saved_flags;
        hdr->root_iblock->ents[8].addr = HADDR_UNDEF;
        hdr->root_iblock->nchildren = saved_nchildren;
        hdr->root_iblock->max_child = saved_max_child;
      };

      H5HF_indirect_t* loc_ib = nullptr;
      bool did_protect = false;
      EXPECT_LT(H5HF__man_dblock_locate(hdr, row4_off, &loc_ib, nullptr,
                                        &did_protect, H5AC__READ_ONLY_FLAG),
                0);
    }

    {
      ScopedErrorSilence silence;
      hdr->root_iblock->ents[8].addr = 0x2000000;
      absl::Cleanup restore_ent = [&] {
        hdr->root_iblock->ents[8].addr = HADDR_UNDEF;
      };

      H5HF_indirect_t* loc_ib = nullptr;
      bool did_protect = false;
      EXPECT_LT(H5HF__man_dblock_locate(hdr, row4_off, &loc_ib, nullptr,
                                        &did_protect, H5AC__READ_ONLY_FLAG),
                0);
    }

    // Now allow missing child iblock at (4,0) to be created cleanly (549, 553,
    // 554) with H5AC__NO_FLAGS_SET.
    {
      H5HF_indirect_t* loc_ib = nullptr;
      unsigned entry = 0;
      bool did_protect = false;
      ASSERT_GE(H5HF__man_dblock_locate(hdr, row4_off, &loc_ib, &entry,
                                        &did_protect, H5AC__NO_FLAGS_SET),
                0);
      ASSERT_NE(loc_ib, nullptr);
      EXPECT_GE(H5HF__man_iblock_unprotect(loc_ib, H5AC__NO_FLAGS_SET,
                                           did_protect),
                0);
    }

    EXPECT_GE(H5HF_close(fh), 0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedFile reopened(
        H5Fopen("dblock_root_err.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(reopened.valid());

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5HF_t* fh = H5HF_open(GetFilePtr(reopened.get()), reopen_addr);
    ASSERT_NE(fh, nullptr);
    absl::Cleanup close_fh = [&] { H5HF_close(fh); };

    ScopedErrorSilence silence;
    const haddr_t saved_table = fh->hdr->man_dtable.table_addr;
    fh->hdr->man_dtable.table_addr = 0x2000000;
    absl::Cleanup restore_table = [&] {
      fh->hdr->man_dtable.table_addr = saved_table;
    };
    H5HF_indirect_t* loc_ib = nullptr;
    bool did_protect = false;
    EXPECT_LT(H5HF__man_dblock_locate(fh->hdr, 0, &loc_ib, nullptr,
                                      &did_protect, H5AC__READ_ONLY_FLAG),
              0);
  }

  // 2. Direct block creation & new errors: tmp_addr exhaustion on empty,
  //    real-space allocation failure, root_create failure, iblock_new failure,
  //    min_dblock_size > next_size, and destroy flush-dep failure.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    // (a) Empty heap tmp_addr failure -> dblock_create (142, 191, 192), new (352).
    {
      const H5HF_create_t cparam = MakeHeapParams();
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      absl::Cleanup close_fh = [&] { H5HF_close(fh); };

      ScopedErrorSilence silence;
      const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      const haddr_t saved_tmp = f->shared->tmp_addr;
      f->shared->tmp_addr = eoa + 16;
      absl::Cleanup restore_tmp = [&] { f->shared->tmp_addr = saved_tmp; };

      H5HF_free_section_t* sec = nullptr;
      EXPECT_LT(H5HF__man_dblock_new(fh->hdr, 24, &sec), 0);
      EXPECT_EQ(sec, nullptr);
    }

    // (b) Real-space H5MF_alloc failure (147) when use_tmp_space == false.
    {
      const H5HF_create_t cparam = MakeHeapParams(
          /*start_block_size=*/4096, /*max_direct_size=*/8192, /*width=*/2,
          /*max_index=*/20, /*start_root_rows=*/1, /*max_man_size=*/4096);
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      absl::Cleanup close_fh = [&] { H5HF_close(fh); };

      ScopedErrorSilence silence;
      const bool saved_use_tmp = f->shared->use_tmp_space;
      const haddr_t saved_tmp = f->shared->tmp_addr;
      f->shared->use_tmp_space = false;
      f->shared->tmp_addr = H5F_get_eoa(f, H5FD_MEM_DEFAULT) + 16;
      absl::Cleanup restore_f = [&] {
        f->shared->use_tmp_space = saved_use_tmp;
        f->shared->tmp_addr = saved_tmp;
      };

      H5HF_free_section_t* sec = nullptr;
      EXPECT_LT(H5HF__man_dblock_new(fh->hdr, 24, &sec), 0);
      EXPECT_EQ(sec, nullptr);
    }

    // (c) Root direct block -> H5HF__hdr_update_iter failure (377) on
    //     exhausted tmp_addr.
    {
      const H5HF_create_t cparam = MakeHeapParams();
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      absl::Cleanup close_fh = [&] { H5HF_close(fh); };

      std::vector<uint8_t> id(fh->hdr->id_len, 0);
      ASSERT_GE(H5HF_insert(fh, 24, MakeData(24, 1).data(), id.data()), 0);
      ASSERT_EQ(fh->hdr->man_dtable.curr_root_rows, 0U);

      ScopedErrorSilence silence;
      const haddr_t saved_tmp = f->shared->tmp_addr;
      f->shared->tmp_addr = H5F_get_eoa(f, H5FD_MEM_DEFAULT) + 16;
      absl::Cleanup restore_tmp = [&] { f->shared->tmp_addr = saved_tmp; };

      H5HF_free_section_t* sec = nullptr;
      EXPECT_LT(H5HF__man_dblock_new(fh->hdr, 200, &sec), 0);
    }

    // (d) Indirect-root heap: test min_dblock_size > next_size (387-391),
    //     dblock_create failure inside else branch (399), and destroy flush
    //     dependency failure (278).
    {
      const H5HF_create_t cparam = MakeHeapParams(
          /*start_block_size=*/64, /*max_direct_size=*/256, /*width=*/2,
          /*max_index=*/16, /*start_root_rows=*/4, /*max_man_size=*/200);
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      absl::Cleanup close_fh = [&] { H5HF_close(fh); };

      std::vector<uint8_t> id(fh->hdr->id_len, 0);
      for (int i = 0; i < 4; ++i) {
        ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, i).data(), id.data()), 0);
      }
      ASSERT_GE(H5HF_insert(fh, 100, MakeData(100, 9).data(), id.data()), 0);

      // Force next_row == 2 with size 64 while requesting 100B (min 128).
      {
        ScopedErrorSilence silence;
        ASSERT_GE(fh->hdr->man_dtable.row_block_size[2], 128ULL);
        const hsize_t saved_rbs = fh->hdr->man_dtable.row_block_size[2];
        // Undo last insert? Wait: after 4x40 + 100, next_row is 2, col is 1.
        // Force row 2 block size < min_dblock_size(128) cleanly.
        fh->hdr->man_dtable.row_block_size[2] = 64;
        absl::Cleanup restore_rbs = [&] {
          fh->hdr->man_dtable.row_block_size[2] = saved_rbs;
        };
        H5HF_free_section_t* sec = nullptr;
        EXPECT_LT(H5HF__man_dblock_new(fh->hdr, 100, &sec), 0);
      }

      // Line 399: dblock_new succeeds in update_iter & inc_iter, fails in
      // dblock_create on exhausted tmp_addr; restore iterator immediately.
      {
        ScopedErrorSilence silence;
        const hsize_t saved_off = fh->hdr->man_iter_off;
        const unsigned saved_entry = fh->hdr->next_block.curr->entry;
        const haddr_t saved_tmp = f->shared->tmp_addr;
        f->shared->tmp_addr = H5F_get_eoa(f, H5FD_MEM_DEFAULT) + 16;
        absl::Cleanup restore_state = [&] {
          f->shared->tmp_addr = saved_tmp;
          fh->hdr->man_iter_off = saved_off;
          H5HF__man_iter_set_entry(fh->hdr, &fh->hdr->next_block, saved_entry);
        };
        H5HF_free_section_t* sec = nullptr;
        EXPECT_LT(H5HF__man_dblock_new(fh->hdr, 100, &sec), 0);
      }

      // Line 278: protect non-highest direct block at (0,0), set wrong
      // fd_parent so H5AC_destroy_flush_dependency fails cleanly.
      {
        H5HF_indirect_t* par_ib = nullptr;
        unsigned par_ent = 0;
        bool did_protect = false;
        ASSERT_GE(H5HF__man_dblock_locate(fh->hdr, 0, &par_ib, &par_ent,
                                          &did_protect, H5AC__READ_ONLY_FLAG),
                  0);
        const haddr_t daddr0 = par_ib->ents[par_ent].addr;
        H5HF_direct_t* dblock = H5HF__man_dblock_protect(
            fh->hdr, daddr0, 64, par_ib, par_ent, H5AC__NO_FLAGS_SET);
        ASSERT_NE(dblock, nullptr);
        ASSERT_GE(H5HF__man_iblock_unprotect(par_ib, H5AC__NO_FLAGS_SET,
                                             did_protect),
                  0);

        ScopedErrorSilence silence;
        void* const saved_fd_parent = dblock->fd_parent;
        const hsize_t saved_alloc = fh->hdr->man_alloc_size;
        dblock->fd_parent = fh->hdr;
        absl::Cleanup restore_db = [&] {
          fh->hdr->man_alloc_size = saved_alloc;
          dblock->fd_parent = saved_fd_parent;
        };
        EXPECT_LT(H5HF__man_dblock_destroy(fh->hdr, dblock, daddr0, nullptr),
                  0);
      }
    }
  }

  // 3. Lines 173, 179: ret_sec_node == nullptr + duplicate cache address.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    const H5HF_create_t cparam = MakeHeapParams();
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    absl::Cleanup close_fh = [&] { H5HF_close(fh); };

    // Fill 64B root direct block completely (0 bytes remaining in fspace).
    const size_t avail = fh->hdr->man_dtable.row_tot_dblock_free[0];
    ASSERT_GT(avail, 0U);
    ASSERT_LE(avail, fh->hdr->max_man_size);
    std::vector<uint8_t> id(fh->hdr->id_len, 0);
    ASSERT_GE(
        H5HF_insert(fh, avail, MakeData(avail, 0x33U).data(), id.data()), 0);
    const haddr_t existing_dblock = fh->hdr->man_dtable.table_addr;

    ScopedErrorSilence silence;
    const haddr_t saved_tmp = f->shared->tmp_addr;
    f->shared->tmp_addr = existing_dblock + 64;
    absl::Cleanup restore_tmp = [&] { f->shared->tmp_addr = saved_tmp; };

    haddr_t dummy_addr = HADDR_UNDEF;
    EXPECT_LT(H5HF__man_dblock_create(fh->hdr, nullptr, 0, &dummy_addr, nullptr),
              0);
  }

  // 4. Line 644: H5MF_xfree failure in H5HF__man_dblock_delete with FSM_AGGR.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_FSM_AGGR,
                                   /*persist=*/false, /*threshold=*/1),
        0);
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());

    haddr_t h1_addr = HADDR_UNDEF;
    haddr_t h2_addr = HADDR_UNDEF;
    std::vector<uint8_t> image;
    {
      ScopedFile file(H5Fcreate("dblock_fsm.h5", H5F_ACC_TRUNC, fcpl.get(),
                                fapl.get()));
      ASSERT_TRUE(file.valid());
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = GetFilePtr(file.get());
      ASSERT_NE(f, nullptr);

      const H5HF_create_t cparam = MakeHeapParams();
      H5HF_t* fh1 = H5HF_create(f, &cparam);
      ASSERT_NE(fh1, nullptr);
      h1_addr = fh1->hdr->heap_addr;
      std::vector<uint8_t> id1(fh1->hdr->id_len, 0);
      ASSERT_GE(H5HF_insert(fh1, 24, MakeData(24, 1).data(), id1.data()), 0);
      EXPECT_GE(H5HF_close(fh1), 0);

      // Second heap places space after fh1's root dblock so it is not at EOA.
      H5HF_t* fh2 = H5HF_create(f, &cparam);
      ASSERT_NE(fh2, nullptr);
      h2_addr = fh2->hdr->heap_addr;
      std::vector<uint8_t> id2(fh2->hdr->id_len, 0);
      ASSERT_GE(H5HF_insert(fh2, 24, MakeData(24, 2).data(), id2.data()), 0);
      EXPECT_GE(H5HF_close(fh2), 0);

      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
      ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
      ASSERT_GT(image_size, 0);
      image.resize(static_cast<size_t>(image_size));
      ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
                image_size);
    }

    ScopedPlist reopen_fapl = MakeCoreFapl();
    ASSERT_TRUE(reopen_fapl.valid());
    ASSERT_GE(H5Pset_file_image(reopen_fapl.get(), image.data(), image.size()),
              0);
    ScopedFile reopened(
        H5Fopen("dblock_fsm_reopen.h5", H5F_ACC_RDWR, reopen_fapl.get()));
    ASSERT_TRUE(reopened.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f2 = GetFilePtr(reopened.get());
    ASSERT_NE(f2, nullptr);

    H5HF_hdr_t* hdr1 = H5HF__hdr_protect(f2, h1_addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(hdr1, nullptr);
    const haddr_t daddr1 = hdr1->man_dtable.table_addr;
    ASSERT_GE(
        H5AC_unprotect(f2, H5AC_FHEAP_HDR, h1_addr, hdr1, H5AC__NO_FLAGS_SET),
        0);

    const H5FD_mem_t mapped =
        (f2->shared->fs_type_map[H5FD_MEM_FHEAP_DBLOCK] == H5FD_MEM_DEFAULT)
            ? H5FD_MEM_FHEAP_DBLOCK
            : f2->shared->fs_type_map[H5FD_MEM_FHEAP_DBLOCK];
    const size_t fs_idx = static_cast<size_t>(mapped);
    ASSERT_EQ(f2->shared->fs_man[fs_idx], nullptr);

    {
      ScopedErrorSilence silence;
      const haddr_t saved_fs_addr = f2->shared->fs_addr[fs_idx];
      f2->shared->fs_addr[fs_idx] = 0x2000000;
      absl::Cleanup restore_fs = [&] {
        f2->shared->fs_addr[fs_idx] = saved_fs_addr;
      };
      EXPECT_LT(H5HF__man_dblock_delete(f2, daddr1, 64), 0);
    }

    EXPECT_GE(H5HF_delete(f2, h1_addr), 0);
    EXPECT_GE(H5HF_delete(f2, h2_addr), 0);
  }
}

}  // namespace
}  // namespace hdf5
