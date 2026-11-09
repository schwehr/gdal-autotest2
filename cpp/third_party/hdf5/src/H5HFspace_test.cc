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
// Tests fractal heap free-space manager startup, section add/find/remove,
// class transitions, root direct/indirect transitions, size accounting,
// close/delete behavior, and error paths for third_party/hdf5/src/H5HFspace.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFspace.c

#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t) inside internal fractal heap/free-space structs.
#ifndef NDEBUG
#define H5HF_TEST_RESTORE_UNDEF_NDEBUG 1
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
#define H5HF_FRIEND
#define H5FS_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FSpkg.h"
#include "third_party/hdf5/src/H5HFpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HF_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HF_TEST_RESTORE_UNDEF_NDEBUG
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

H5HF_create_t MakeHeapCreateParams(uint32_t max_man_size = 450,
                                   unsigned width = 2,
                                   size_t start_block_size = 128,
                                   size_t max_direct_size = 512,
                                   unsigned max_index = 14) {
  H5HF_create_t cparam{};
  cparam.managed.width = width;
  cparam.managed.start_block_size = start_block_size;
  cparam.managed.max_direct_size = max_direct_size;
  cparam.managed.max_index = max_index;
  cparam.managed.start_root_rows = 1;
  cparam.checksum_dblocks = true;
  cparam.max_man_size = max_man_size;
  cparam.id_len = 0;
  return cparam;
}

TEST(H5HFspaceTest, StartFindSizeCloseAndDeleteWithEmptyAndNonEmptyFspace) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  const H5HF_create_t cparam = MakeHeapCreateParams();
  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

  H5HF_hdr_t* hdr = fh->hdr;
  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);

  // 1. Fresh heap has fs_addr == HADDR_UNDEF and fspace == nullptr:
  //    start(false), find, revert_root, close, and size handle absent fspace.
  EXPECT_EQ(hdr->fspace, nullptr);
  EXPECT_EQ(hdr->fs_addr, HADDR_UNDEF);
  EXPECT_GE(H5HF__space_start(hdr, false), 0);
  EXPECT_EQ(hdr->fspace, nullptr);

  H5HF_free_section_t* found = nullptr;
  EXPECT_EQ(H5HF__space_find(hdr, 16, &found), 0);
  EXPECT_EQ(found, nullptr);
  EXPECT_GE(H5HF__space_revert_root(hdr), 0);
  EXPECT_GE(H5HF__space_close(hdr), 0);

  hsize_t fs_size = 999;
  EXPECT_GE(H5HF__space_size(hdr, &fs_size), 0);
  EXPECT_EQ(fs_size, 0U);

  // 2. start(true) creates fspace with 0 sections; closing deletes the empty
  //    free-space manager and resets fs_addr back to HADDR_UNDEF.
  EXPECT_GE(H5HF__space_start(hdr, true), 0);
  EXPECT_NE(hdr->fspace, nullptr);
  EXPECT_NE(hdr->fs_addr, HADDR_UNDEF);
  EXPECT_GE(H5HF__space_close(hdr), 0);
  EXPECT_EQ(hdr->fspace, nullptr);
  EXPECT_EQ(hdr->fs_addr, HADDR_UNDEF);

  // 3. Create real managed free space via insert + remove, then inspect
  //    space_size while open, close with non-zero sections, space_size when
  //    closed (reopening from fs_addr), and find + space_delete.
  //    Fill block 0 completely (start_block_size=128, overhead=19 => 109B
  //    usable: 32B + 77B = 109B) so there is no unallocated tail free section.
  const size_t usable_b0 =
      hdr->man_dtable.cparam.start_block_size - H5HF_MAN_ABS_DIRECT_OVERHEAD(hdr);
  ASSERT_EQ(usable_b0, 109U);
  const std::vector<uint8_t> obj1(32, 0x11U);
  const std::vector<uint8_t> obj2(usable_b0 - obj1.size(), 0x22U);
  std::vector<uint8_t> id1(id_len);
  std::vector<uint8_t> id2(id_len);
  ASSERT_GE(H5HF_insert(fh, obj1.size(), obj1.data(), id1.data()), 0);
  ASSERT_GE(H5HF_insert(fh, obj2.size(), obj2.data(), id2.data()), 0);
  ASSERT_GE(H5HF_remove(fh, id1.data()), 0);
  ASSERT_NE(hdr->fspace, nullptr);
  ASSERT_NE(hdr->fs_addr, HADDR_UNDEF);

  hsize_t open_fs_size = 0;
  EXPECT_GE(H5HF__space_size(hdr, &open_fs_size), 0);
  EXPECT_GT(open_fs_size, 0U);

  // Close with sections still present -> fs_addr stays defined.
  EXPECT_GE(H5HF__space_close(hdr), 0);
  EXPECT_EQ(hdr->fspace, nullptr);
  EXPECT_NE(hdr->fs_addr, HADDR_UNDEF);

  // space_size reopens fspace cleanly when closed and returns same size.
  hsize_t reopened_fs_size = 0;
  EXPECT_GE(H5HF__space_size(hdr, &reopened_fs_size), 0);
  EXPECT_EQ(reopened_fs_size, open_fs_size);
  EXPECT_NE(hdr->fspace, nullptr);

  // Find returns the sole live 32-byte section (caller now owns it); freeing
  // it and closing deletes the empty fspace manager cleanly.
  EXPECT_GT(H5HF__space_find(hdr, 16, &found), 0);
  ASSERT_NE(found, nullptr);
  EXPECT_GE(H5HF__sect_single_free(&found->sect_info), 0);
  EXPECT_GE(H5HF__space_close(hdr), 0);
  EXPECT_EQ(hdr->fs_addr, HADDR_UNDEF);

  // 4. Create free space again, close with sections preserved, then delete
  //    explicitly via H5HF__space_delete and reset fs_addr before H5HF_close.
  std::vector<uint8_t> id3(id_len);
  const std::vector<uint8_t> obj3(32, 0x33U);
  ASSERT_GE(H5HF_insert(fh, obj3.size(), obj3.data(), id3.data()), 0);
  ASSERT_GE(H5HF_remove(fh, id2.data()), 0);
  EXPECT_GE(H5HF__space_close(hdr), 0);
  ASSERT_NE(hdr->fs_addr, HADDR_UNDEF);
  EXPECT_GE(H5HF__space_delete(hdr), 0);
  hdr->fs_addr = HADDR_UNDEF;
  hdr->total_man_free = usable_b0 - obj3.size();
}

TEST(H5HFspaceTest, RootPromotionAndReversionUpdateLiveAndSerializedSections) {
  haddr_t heap_addr = HADDR_UNDEF;
  std::vector<uint8_t> id_keep;
  std::vector<uint8_t> image;

  const std::vector<uint8_t> keep_payload(40, 0x7aU);

  // 1. Natural promotion (root direct -> root indirect with live SINGLE free
  //    sections) and natural reversion (root indirect -> root direct).
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    const H5HF_create_t cparam = MakeHeapCreateParams(
        /*max_man_size=*/450, /*width=*/2, /*start_block_size=*/128,
        /*max_direct_size=*/512, /*max_index=*/14);
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
    id_keep.resize(id_len);
    std::vector<uint8_t> id_free(id_len);
    std::vector<uint8_t> id_second_block(id_len);

    // Block 0 (128 bytes, ~109 usable): insert two 40-byte objects and free the
    // first so a live 40-byte SINGLE section sits in block 0.
    const std::vector<uint8_t> free_payload(40, 0x11U);
    ASSERT_GE(
        H5HF_insert(fh, free_payload.size(), free_payload.data(), id_free.data()),
        0);
    ASSERT_GE(
        H5HF_insert(fh, keep_payload.size(), keep_payload.data(), id_keep.data()),
        0);
    ASSERT_GE(H5HF_remove(fh, id_free.data()), 0);
    EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 0U);
    ASSERT_NE(fh->hdr->fspace, nullptr);

    // Inserting 64 bytes exceeds both the 40-byte free section and the ~29
    // bytes left in block 0 -> promotes root direct block to indirect block,
    // calling H5HF__space_create_root + H5HF__space_create_root_cb (live).
    const std::vector<uint8_t> spill_payload(64, 0x22U);
    ASSERT_GE(H5HF_insert(fh, spill_payload.size(), spill_payload.data(),
                          id_second_block.data()),
              0);
    EXPECT_GT(fh->hdr->man_dtable.curr_root_rows, 0U);

    // Removing the sole object in block 1 detaches block 1 and reverts the root
    // back to block 0 -> H5HF__space_revert_root + _cb (live SINGLE).
    ASSERT_GE(H5HF_remove(fh, id_second_block.data()), 0);
    EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 0U);

    // Close heap cleanly, preserving serialized 40-byte section in block 0.
    EXPECT_GE(H5HF_close(fh), 0);
    fh = nullptr;

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  // 2. Reopen root direct block heap with persisted free space, exercise
  //    H5HF__space_revert_root on a serialized section (cb skips it), and then
  //    spill to block 1 so H5HF__space_create_root_cb transitions SERIALIZED ->
  //    LIVE!
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("reopen_root.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5F_t* f2 = GetFilePtr(reopened.get());
  ASSERT_NE(f2, nullptr);

  H5HF_t* fh2 = H5HF_open(f2, heap_addr);
  ASSERT_NE(fh2, nullptr);
  auto close_fh2 = absl::MakeCleanup([&]() { H5HF_close(fh2); });
  EXPECT_EQ(fh2->hdr->man_dtable.curr_root_rows, 0U);
  EXPECT_EQ(fh2->hdr->fspace, nullptr);
  EXPECT_NE(fh2->hdr->fs_addr, HADDR_UNDEF);

  // Confirm H5HF__space_create_root is a safe no-op when fspace == nullptr
  // by allocating a fresh indirect block temporarily, or simply open fspace and
  // call H5HF__space_revert_root while section state is H5FS_SECT_SERIALIZED.
  ASSERT_GE(H5HF__space_start(fh2->hdr, false), 0);
  ASSERT_NE(fh2->hdr->fspace, nullptr);
  EXPECT_GE(H5HF__space_revert_root(fh2->hdr), 0);

  // Insert 64 bytes: H5HF__space_find deserializes the 40-byte section as
  // SERIALIZED and declines it; root promotion runs H5HF__space_create_root_cb
  // and promotes the SERIALIZED section to LIVE!
  std::vector<uint8_t> id_spill(id_keep.size());
  const std::vector<uint8_t> spill2(64, 0x55U);
  ASSERT_GE(H5HF_insert(fh2, spill2.size(), spill2.data(), id_spill.data()), 0);
  EXPECT_GT(fh2->hdr->man_dtable.curr_root_rows, 0U);

  std::vector<uint8_t> got_keep(keep_payload.size(), 0);
  EXPECT_GE(H5HF_read(fh2, id_keep.data(), got_keep.data()), 0);
  EXPECT_THAT(got_keep, ElementsAreArray(keep_payload));
}

TEST(H5HFspaceTest, SkipRowsPromotesNormalRowClassAndShrinkRemovesNormalRows) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  // start_block_size = 64, max_direct_size = 512, width = 2:
  // Row 0: two 64B blocks; Row 1: two 128B blocks; Row 2: two 256B blocks;
  // Row 3: two 512B blocks.
  const H5HF_create_t cparam = MakeHeapCreateParams(
      /*max_man_size=*/450, /*width=*/2, /*start_block_size=*/64,
      /*max_direct_size=*/512, /*max_index=*/14);
  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);

  // 1. Insert 24B in block 0, then 200B forcing jump to row 2 (256B block).
  //    This skips entry 1 (row 0 -> FIRST_ROW) and entries 2..3 (row 1 ->
  //    NORMAL_ROW).
  const std::vector<uint8_t> obj0(24, 0x10U);
  const std::vector<uint8_t> obj_big(200, 0x20U);
  std::vector<uint8_t> id0(id_len);
  std::vector<uint8_t> id_big(id_len);
  ASSERT_GE(H5HF_insert(fh, obj0.size(), obj0.data(), id0.data()), 0);
  ASSERT_GE(H5HF_insert(fh, obj_big.size(), obj_big.data(), id_big.data()), 0);

  // 2. Insert 30B into entry 1, exhausting the FIRST_ROW section in row 0 so
  //    row 1's NORMAL_ROW is promoted to FIRST_ROW via
  //    H5HF__space_sect_change_class.
  const std::vector<uint8_t> obj1(30, 0x30U);
  std::vector<uint8_t> id1(id_len);
  ASSERT_GE(H5HF_insert(fh, obj1.size(), obj1.data(), id1.data()), 0);

  // 3. Repeat jump to row 3 (350B) leaving multiple NORMAL_ROWs skipped, then
  //    remove the 350B object so indirect shrinking removes NORMAL_ROW sections
  //    via H5HF__space_remove.
  const std::vector<uint8_t> obj_huge_man(350, 0x40U);
  std::vector<uint8_t> id_huge_man(id_len);
  ASSERT_GE(H5HF_insert(fh, obj_huge_man.size(), obj_huge_man.data(),
                        id_huge_man.data()),
            0);
  ASSERT_GE(H5HF_remove(fh, id_huge_man.data()), 0);

  // 4. Remove remaining blocks so root reverts back to block 0, leaving block 0
  //    partially occupied without triggering a spurious leak in shrink paths.
  ASSERT_GE(H5HF_remove(fh, id_big.data()), 0);
  ASSERT_GE(H5HF_remove(fh, id1.data()), 0);
  ASSERT_EQ(fh->hdr->man_dtable.curr_root_rows, 0U);
}

TEST(H5HFspaceTest, ManualSpaceAddSectChangeClassAndRemove) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  const H5HF_create_t cparam = MakeHeapCreateParams();
  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);

  // Fill block 0 completely so no auto-created tail free section collides.
  const size_t usable_b0 = fh->hdr->man_dtable.cparam.start_block_size -
                           H5HF_MAN_ABS_DIRECT_OVERHEAD(fh->hdr);
  const std::vector<uint8_t> obj_full(usable_b0, 0x10U);
  std::vector<uint8_t> id_full(id_len);
  ASSERT_GE(
      H5HF_insert(fh, obj_full.size(), obj_full.data(), id_full.data()), 0);

  H5HF_free_section_t* manual_sect = H5HF__sect_single_new(
      /*sect_off=*/H5HF_MAN_ABS_DIRECT_OVERHEAD(fh->hdr),
      /*sect_size=*/12, /*parent=*/nullptr, /*par_entry=*/0);
  ASSERT_NE(manual_sect, nullptr);
  ASSERT_GE(H5HF__space_add(fh->hdr, manual_sect, H5FS_ADD_RETURNED_SPACE), 0);
  EXPECT_GE(H5HF__space_sect_change_class(fh->hdr, manual_sect,
                                          H5HF_FSPACE_SECT_SINGLE),
            0);
  EXPECT_GE(H5HF__space_remove(fh->hdr, manual_sect), 0);
  EXPECT_GE(H5HF__sect_single_free(&manual_sect->sect_info), 0);
  EXPECT_GE(H5HF__space_close(fh->hdr), 0);
}

TEST(H5HFspaceTest, ErrorPathsCoverOpenAddFindIterateRemoveChangeCloseDelete) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  const H5HF_create_t cparam = MakeHeapCreateParams();
  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

  H5HF_hdr_t* hdr = fh->hdr;
  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);

  // Persist a real free-space manager with one section (fill block 0 fully
  // with obj1 + obj2 so removing obj1 leaves exactly one free section).
  const size_t usable_b0 =
      hdr->man_dtable.cparam.start_block_size - H5HF_MAN_ABS_DIRECT_OVERHEAD(hdr);
  ASSERT_EQ(usable_b0, 109U);
  const std::vector<uint8_t> obj1(32, 0x11U);
  const std::vector<uint8_t> obj2(usable_b0 - obj1.size(), 0x22U);
  std::vector<uint8_t> id1(id_len);
  std::vector<uint8_t> id2(id_len);
  ASSERT_GE(H5HF_insert(fh, obj1.size(), obj1.data(), id1.data()), 0);
  ASSERT_GE(H5HF_insert(fh, obj2.size(), obj2.data(), id2.data()), 0);
  ASSERT_GE(H5HF_remove(fh, id1.data()), 0);

  // Close fspace (keeping serialized fs_addr on disk) and flush file so bad
  // addresses inside file bounds trigger metadata decode failures cleanly.
  ASSERT_GE(H5HF__space_close(hdr), 0);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  const haddr_t real_fs_addr = hdr->fs_addr;
  ASSERT_NE(real_fs_addr, HADDR_UNDEF);
  const haddr_t kBadAddr = hdr->heap_addr + 1;

  // Create a synthetic section solely for failing paths before it is consumed.
  H5HF_free_section_t* orphan_sect =
      H5HF__sect_single_new(0, 16, nullptr, 0);
  ASSERT_NE(orphan_sect, nullptr);
  auto free_orphan = absl::MakeCleanup([&]() {
    if (orphan_sect != nullptr) {
      H5HF__sect_single_free(&orphan_sect->sect_info);
    }
  });

  ScopedErrorSilence silence;

  // 1. Errors when fspace == nullptr and hdr->fs_addr is invalid:
  //    start, add (via start failure), find (via start failure), size, delete.
  hdr->fs_addr = kBadAddr;
  EXPECT_LT(H5HF__space_start(hdr, false), 0);
  EXPECT_LT(H5HF__space_add(hdr, orphan_sect, 0), 0);
  H5HF_free_section_t* out_sect = nullptr;
  EXPECT_LT(H5HF__space_find(hdr, 16, &out_sect), 0);
  hsize_t bad_fs_size = 0;
  EXPECT_LT(H5HF__space_size(hdr, &bad_fs_size), 0);
  EXPECT_LT(H5HF__space_delete(hdr), 0);
  hdr->fs_addr = real_fs_addr;

  // 2. Reopen fspace with sinfo == nullptr, then corrupt fspace->sect_addr to
  //    fail sinfo_lock across add, find, revert_root, create_root, remove, and
  //    sect_change_class.
  ASSERT_GE(H5HF__space_start(hdr, false), 0);
  ASSERT_NE(hdr->fspace, nullptr);
  ASSERT_EQ(hdr->fspace->sinfo, nullptr);
  const haddr_t real_sect_addr = hdr->fspace->sect_addr;
  ASSERT_NE(real_sect_addr, HADDR_UNDEF);

  hdr->fspace->sect_addr = kBadAddr;
  EXPECT_LT(H5HF__space_add(hdr, orphan_sect, 0), 0);
  EXPECT_LT(H5HF__space_find(hdr, 16, &out_sect), 0);
  EXPECT_LT(H5HF__space_revert_root(hdr), 0);
  EXPECT_LT(H5HF__space_remove(hdr, orphan_sect), 0);
  EXPECT_LT(
      H5HF__space_sect_change_class(hdr, orphan_sect, H5HF_FSPACE_SECT_SINGLE),
      0);

  // Also cover H5HF__space_create_root failure path using a temporary iblock.
  H5HF_indirect_t dummy_iblock{};
  dummy_iblock.hdr = hdr;
  EXPECT_LT(H5HF__space_create_root(hdr, &dummy_iblock), 0);

  hdr->fspace->sect_addr = real_sect_addr;

  // 3. Remove the real persisted section cleanly so nsects == 0, then trigger
  //    H5FS_delete failure inside H5HF__space_close by swapping fs_addr.
  EXPECT_GT(H5HF__space_find(hdr, 16, &out_sect), 0);
  ASSERT_NE(out_sect, nullptr);
  EXPECT_GE(H5HF__sect_single_free(&out_sect->sect_info), 0);

  hdr->fs_addr = kBadAddr;
  EXPECT_LT(H5HF__space_close(hdr), 0);
  EXPECT_EQ(hdr->fspace, nullptr);

  // Cleanly delete the real free-space header on disk before tearing down.
  EXPECT_GE(H5FS_delete(hdr->f, real_fs_addr), 0);
  hdr->fs_addr = HADDR_UNDEF;
}

}  // namespace
}  // namespace hdf5
