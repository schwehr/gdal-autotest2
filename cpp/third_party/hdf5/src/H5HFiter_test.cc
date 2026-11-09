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
// Tests managed block iterator initialization, offset/entry positioning,
// multi-level up/down traversal, advancement, query, and reset routines
// (third_party/hdf5/src/H5HFiter.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFiter.c

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t) inside internal fractal heap structs.
#ifndef NDEBUG
#define H5HFITER_TEST_RESTORE_UNDEF_NDEBUG 1
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
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5HFpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HFITER_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HFITER_TEST_RESTORE_UNDEF_NDEBUG
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

// Doubling table with width=2, start_block_size=64, max_direct_size=64:
//   Row 0: 2 x 64  [0, 128)      (direct)
//   Row 1: 2 x 64  [128, 256)    (direct)
//   Row 2: 2 x 128 [256, 512)    (child iblocks, nrows=1 -> 2 x 64)
//   Row 3: 2 x 256 [512, 1024)   (child iblocks, nrows=2 -> 4 x 64)
//   Row 4: 2 x 512 [1024, 2048)  (child iblocks, nrows=3 -> 4x64 + 2x128)
H5HF_create_t MakeMultiLevelParams() {
  H5HF_create_t cparam{};
  cparam.managed.width = 2;
  cparam.managed.start_block_size = 64;
  cparam.managed.max_direct_size = 64;
  cparam.managed.max_index = 14;
  cparam.managed.start_root_rows = 1;
  cparam.checksum_dblocks = true;
  cparam.max_man_size = 44;
  cparam.id_len = 0;
  return cparam;
}

std::vector<uint8_t> MakePayload(size_t size, uint8_t seed) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>((seed + i * 13) & 0xffU);
  }
  return data;
}

TEST(H5HFiterTest, InitReadyResetSetEntryNextAndCurrQueries) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  const H5HF_create_t cparam = MakeMultiLevelParams();
  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);

  // Insert 2 objects of 40 bytes each to promote root from dblock to iblock.
  std::vector<uint8_t> id(id_len);
  for (uint8_t i = 0; i < 2; ++i) {
    const std::vector<uint8_t> obj = MakePayload(40, i);
    ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), id.data()), 0);
  }

  H5HF_hdr_t* hdr = fh->hdr;
  ASSERT_GT(hdr->man_dtable.curr_root_rows, 0U);

  H5HF_block_iter_t iter{};
  // 1. Init and empty reset leave iterator unready.
  EXPECT_GE(H5HF__man_iter_init(&iter), 0);
  EXPECT_FALSE(H5HF__man_iter_ready(&iter));
  EXPECT_GE(H5HF__man_iter_reset(&iter), 0);
  EXPECT_FALSE(H5HF__man_iter_ready(&iter));

  // 2. Start iterator from protected root indirect block at entry 1.
  bool did_protect = false;
  H5HF_indirect_t* root_iblock = H5HF__man_iblock_protect(
      hdr, hdr->man_dtable.table_addr, hdr->man_dtable.curr_root_rows, nullptr,
      0, /*must_protect=*/false, H5AC__READ_ONLY_FLAG, &did_protect);
  ASSERT_NE(root_iblock, nullptr);

  EXPECT_GE(H5HF__man_iter_start_entry(hdr, &iter, root_iblock, /*start_entry=*/1),
            0);
  EXPECT_GE(
      H5HF__man_iblock_unprotect(root_iblock, H5AC__NO_FLAGS_SET, did_protect),
      0);
  EXPECT_TRUE(H5HF__man_iter_ready(&iter));

  unsigned row = 99;
  unsigned col = 99;
  unsigned entry = 99;
  H5HF_indirect_t* block = nullptr;
  EXPECT_GE(H5HF__man_iter_curr(&iter, &row, &col, &entry, &block), 0);
  EXPECT_EQ(row, 0U);
  EXPECT_EQ(col, 1U);
  EXPECT_EQ(entry, 1U);
  EXPECT_EQ(block, root_iblock);

  // Query with all-nullptr outputs exercises every optional branch.
  EXPECT_GE(H5HF__man_iter_curr(&iter, nullptr, nullptr, nullptr, nullptr), 0);

  // 3. Advance with H5HF__man_iter_next and jump with H5HF__man_iter_set_entry.
  EXPECT_GE(H5HF__man_iter_next(hdr, &iter, /*nentries=*/2), 0);
  EXPECT_GE(H5HF__man_iter_curr(&iter, &row, &col, &entry, &block), 0);
  EXPECT_EQ(row, 1U);
  EXPECT_EQ(col, 1U);
  EXPECT_EQ(entry, 3U);

  EXPECT_GE(H5HF__man_iter_set_entry(hdr, &iter, /*entry=*/2), 0);
  EXPECT_GE(H5HF__man_iter_curr(&iter, &row, &col, &entry, &block), 0);
  EXPECT_EQ(row, 1U);
  EXPECT_EQ(col, 0U);
  EXPECT_EQ(entry, 2U);

  EXPECT_GE(H5HF__man_iter_reset(&iter), 0);
  EXPECT_FALSE(H5HF__man_iter_ready(&iter));
}

TEST(H5HFiterTest, MultiLevelStartOffsetUpDownAndMultiLevelReset) {
  haddr_t heap_addr = HADDR_UNDEF;
  std::vector<uint8_t> image;

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    const H5HF_create_t cparam = MakeMultiLevelParams();
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

    // Insert 22 objects (40B each => 1 per 64B dblock) to populate:
    //   Row 0..1 (4 blocks: [0, 256))
    //   Row 2    (4 blocks: [256, 512))
    //   Row 3    (8 blocks: [512, 1024))
    //   Row 4 col 0 -> child iblock (nrows=3) -> row 0..1 (4 blocks) + row 2
    //                  grandchild iblock (2 blocks) -> [1024, 1408).
    for (int i = 0; i < 22; ++i) {
      const std::vector<uint8_t> obj =
          MakePayload(40, static_cast<uint8_t>(i));
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
  ScopedFile reopened(H5Fopen("iter_reopen.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5F_t* f2 = GetFilePtr(reopened.get());
  ASSERT_NE(f2, nullptr);

  H5HF_t* fh2 = H5HF_open(f2, heap_addr);
  ASSERT_NE(fh2, nullptr);
  auto close_fh2 = absl::MakeCleanup([&]() { H5HF_close(fh2); });
  H5HF_hdr_t* hdr = fh2->hdr;

  H5HF_block_iter_t iter{};
  ASSERT_GE(H5HF__man_iter_init(&iter), 0);

  // 1. Offset in root direct row (row 0, col 1 => offset 64).
  ASSERT_GE(H5HF__man_iter_start_offset(hdr, &iter, /*offset=*/64), 0);
  EXPECT_TRUE(H5HF__man_iter_ready(&iter));
  unsigned row = 0;
  unsigned col = 0;
  unsigned entry = 0;
  H5HF_indirect_t* block = nullptr;
  ASSERT_GE(H5HF__man_iter_curr(&iter, &row, &col, &entry, &block), 0);
  EXPECT_EQ(row, 0U);
  EXPECT_EQ(col, 1U);
  EXPECT_EQ(entry, 1U);
  EXPECT_EQ(block->block_off, 0U);
  ASSERT_GE(H5HF__man_iter_reset(&iter), 0);

  // 2. Offset at exact boundary of child indirect block (row 2, col 0 => 256):
  //    hits curr_offset == col * row_block_size[row] early exit at root level.
  ASSERT_GE(H5HF__man_iter_start_offset(hdr, &iter, /*offset=*/256), 0);
  ASSERT_GE(H5HF__man_iter_curr(&iter, &row, &col, &entry, &block), 0);
  EXPECT_EQ(row, 2U);
  EXPECT_EQ(col, 0U);
  EXPECT_EQ(entry, 4U);
  EXPECT_EQ(block->block_off, 0U);
  ASSERT_GE(H5HF__man_iter_reset(&iter), 0);

  // 3. Offset inside 3-level hierarchy:
  //    1024 (row 4 col 0) + 256 (child row 2 col 0) + 64 (grandchild col 1) =
  //    1344.
  ASSERT_GE(H5HF__man_iter_start_offset(hdr, &iter, /*offset=*/1344), 0);
  ASSERT_GE(H5HF__man_iter_curr(&iter, &row, &col, &entry, &block), 0);
  EXPECT_EQ(row, 0U);
  EXPECT_EQ(col, 1U);
  EXPECT_EQ(entry, 1U);
  EXPECT_EQ(block->block_off, 1024U + 256U);
  ASSERT_NE(iter.curr->up, nullptr);
  ASSERT_NE(iter.curr->up->up, nullptr);

  // Step up one level (to row 4 col 0 child iblock, at entry 4) then reset
  // remaining 2 levels together via H5HF__man_iter_reset().
  ASSERT_GE(H5HF__man_iter_up(&iter), 0);
  ASSERT_GE(H5HF__man_iter_curr(&iter, &row, &col, &entry, &block), 0);
  EXPECT_EQ(row, 2U);
  EXPECT_EQ(col, 0U);
  EXPECT_EQ(entry, 4U);
  EXPECT_EQ(block->block_off, 1024U);

  // Exercise H5HF__man_iter_down() explicitly on child iblock at root entry 4.
  H5HF_indirect_t* root_iblock = iter.curr->up->context;
  ASSERT_NE(root_iblock, nullptr);
  ASSERT_GE(H5HF__man_iter_up(&iter), 0);
  ASSERT_GE(H5HF__man_iter_set_entry(hdr, &iter, /*entry=*/4), 0);

  bool did_protect = false;
  H5HF_indirect_t* child_iblock = H5HF__man_iblock_protect(
      hdr, root_iblock->ents[4].addr, /*iblock_nrows=*/1, root_iblock,
      /*par_entry=*/4, /*must_protect=*/false, H5AC__READ_ONLY_FLAG,
      &did_protect);
  ASSERT_NE(child_iblock, nullptr);
  ASSERT_GE(H5HF__man_iter_down(&iter, child_iblock), 0);
  ASSERT_GE(
      H5HF__man_iblock_unprotect(child_iblock, H5AC__NO_FLAGS_SET, did_protect),
      0);

  ASSERT_GE(H5HF__man_iter_curr(&iter, &row, &col, &entry, &block), 0);
  EXPECT_EQ(row, 0U);
  EXPECT_EQ(col, 0U);
  EXPECT_EQ(entry, 0U);
  EXPECT_EQ(block, child_iblock);

  EXPECT_GE(H5HF__man_iter_reset(&iter), 0);
  EXPECT_FALSE(H5HF__man_iter_ready(&iter));
}

TEST(H5HFiterTest, PublicInsertionAndReverseRemovalExercisesIteratorPaths) {
  haddr_t heap_addr = HADDR_UNDEF;
  std::vector<std::vector<uint8_t>> ids;
  std::vector<uint8_t> image;

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    const H5HF_create_t cparam = MakeMultiLevelParams();
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
    for (int i = 0; i < 18; ++i) {
      ids.emplace_back(id_len);
      const std::vector<uint8_t> obj =
          MakePayload(40, static_cast<uint8_t>(i + 1));
      ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), ids.back().data()), 0);
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
  ScopedFile reopened(H5Fopen("iter_reverse.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5F_t* f2 = GetFilePtr(reopened.get());
  ASSERT_NE(f2, nullptr);

  H5HF_t* fh2 = H5HF_open(f2, heap_addr);
  ASSERT_NE(fh2, nullptr);
  auto close_fh2 = absl::MakeCleanup([&]() { H5HF_close(fh2); });

  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh2, &id_len), 0);

  // Inserting into a reopened multi-level heap exercises
  // H5HF__man_iter_start_offset() from H5HF__hdr_update_iter().
  for (int i = 0; i < 4; ++i) {
    ids.emplace_back(id_len);
    const std::vector<uint8_t> obj =
        MakePayload(40, static_cast<uint8_t>(50 + i));
    ASSERT_GE(H5HF_insert(fh2, obj.size(), obj.data(), ids.back().data()), 0);
  }

  // Removing trailing objects in reverse order exercises
  // H5HF__hdr_reverse_iter() across child indirect blocks.
  while (!ids.empty()) {
    EXPECT_GE(H5HF_remove(fh2, ids.back().data()), 0);
    ids.pop_back();
  }

  H5HF_stat_t stats{};
  EXPECT_GE(H5HF_stat_info(fh2, &stats), 0);
  EXPECT_EQ(stats.man_nobjs, 0U);
}

}  // namespace
}  // namespace hdf5
