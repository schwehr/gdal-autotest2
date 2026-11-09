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
// Tests fractal heap header creation, parameter validation, doubling-table
// free-space computation, block-iterator progression and reversal across
// multi-level indirect blocks, emptying, freeing, and deletion
// (third_party/hdf5/src/H5HFhdr.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFhdr.c

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
#define H5HFHDR_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5HFHDR_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HFHDR_TEST_RESTORE_UNDEF_NDEBUG
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

H5HF_create_t MakeHeapParams(
    uint16_t id_len = 0, uint32_t max_man_size = 200, unsigned width = 2,
    size_t start_block_size = 64, size_t max_direct_size = 256,
    unsigned max_index = 16, unsigned start_root_rows = 1,
    bool checksum_dblocks = true) {
  H5HF_create_t cparam{};
  cparam.managed.width = width;
  cparam.managed.start_block_size = start_block_size;
  cparam.managed.max_direct_size = max_direct_size;
  cparam.managed.max_index = max_index;
  cparam.managed.start_root_rows = start_root_rows;
  cparam.checksum_dblocks = checksum_dblocks;
  cparam.max_man_size = max_man_size;
  cparam.id_len = id_len;
  return cparam;
}

std::vector<uint8_t> MakeData(size_t len, uint8_t seed) {
  std::vector<uint8_t> buf(len);
  for (size_t i = 0; i < len; ++i) {
    buf[i] = static_cast<uint8_t>((seed + i * 11) & 0xffU);
  }
  return buf;
}

// Custom filter with toggleable can_apply/set_local error injection.
constexpr H5Z_filter_t kHdrFilterId = H5Z_FILTER_RESERVED + 32;
bool g_allow_can_apply = true;
bool g_allow_set_local = true;

htri_t HdrFilterCanApply(hid_t /*dcpl_id*/, hid_t /*type_id*/,
                         hid_t /*space_id*/) {
  return g_allow_can_apply ? 1 : -1;
}

herr_t HdrFilterSetLocal(hid_t /*dcpl_id*/, hid_t /*type_id*/,
                         hid_t /*space_id*/) {
  return g_allow_set_local ? 0 : -1;
}

size_t HdrFilterApply(unsigned int /*flags*/, size_t /*cd_nelmts*/,
                      const unsigned int /*cd_values*/[], size_t nbytes,
                      size_t* /*buf_size*/, void** /*buf*/) {
  return nbytes;
}

TEST(H5HFhdrTest, CreateModesFilteredAndUnfilteredAndValidationFailures) {
  const H5Z_class2_t hdr_filter = {
      .version = H5Z_CLASS_T_VERS,
      .id = kHdrFilterId,
      .encoder_present = 1,
      .decoder_present = 1,
      .name = "test_hdr_filter",
      .can_apply = HdrFilterCanApply,
      .set_local = HdrFilterSetLocal,
      .filter = HdrFilterApply,
  };
  g_allow_can_apply = true;
  g_allow_set_local = true;
  ASSERT_GE(H5Zregister(&hdr_filter), 0);
  absl::Cleanup unregister = [&] {
    g_allow_can_apply = true;
    g_allow_set_local = true;
    H5Zunregister(kHdrFilterId);
  };

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  // 1. Direct huge ID (id_len=1) WITH filter_len > 0 exercises the filtered
  //    case in H5HF__hdr_create's id_len==1 branch and H5HF__hdr_dirty resize.
  {
    H5HF_create_t cparam = MakeHeapParams(/*id_len=*/1);
    absl::Cleanup reset_pline = [&] {
      if (cparam.pline.nused > 0) {
        H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
      }
    };
    ASSERT_GE(H5Z_append(&cparam.pline, kHdrFilterId, H5Z_FLAG_MANDATORY, 0,
                         nullptr),
              0);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    EXPECT_GT(fh->hdr->filter_len, 0U);
    EXPECT_EQ(fh->hdr->id_len,
              1U + fh->hdr->sizeof_addr + 2 * fh->hdr->sizeof_size + 4U);

    // Insert a small managed object (36B > tiny_max_len=28B) so H5HF__hdr_dirty
    // triggers resize and filtered root direct block exists, then delete heap
    // -> hits filtered root dblock deletion in H5HF__hdr_delete!
    std::vector<uint8_t> id(fh->hdr->id_len, 0);
    const std::vector<uint8_t> obj = MakeData(36, 0x10U);
    ASSERT_GE(H5HF_insert(fh, obj.size(), obj.data(), id.data()), 0);
    const haddr_t addr = fh->hdr->heap_addr;
    EXPECT_GE(H5HF_close(fh), 0);
    EXPECT_GE(H5HF_delete(f, addr), 0);
  }

  // 2. Delete an empty heap, unfiltered root direct block heap, indirect-root
  //    heap, and huge-object heap, plus unfiltered id_len=1 and custom id_len.
  {
    const H5HF_create_t cparam = MakeHeapParams(/*id_len=*/0);
    H5HF_t* fh_empty = H5HF_create(f, &cparam);
    ASSERT_NE(fh_empty, nullptr);
    const haddr_t empty_addr = fh_empty->hdr->heap_addr;
    EXPECT_GE(H5HF_close(fh_empty), 0);
    EXPECT_GE(H5HF_delete(f, empty_addr), 0);

    H5HF_t* fh_dblock = H5HF_create(f, &cparam);
    ASSERT_NE(fh_dblock, nullptr);
    const haddr_t db_addr = fh_dblock->hdr->heap_addr;
    std::vector<uint8_t> id(fh_dblock->hdr->id_len, 0);
    const std::vector<uint8_t> obj = MakeData(24, 0x20U);
    ASSERT_GE(H5HF_insert(fh_dblock, obj.size(), obj.data(), id.data()), 0);
    EXPECT_GE(H5HF_close(fh_dblock), 0);
    EXPECT_GE(H5HF_delete(f, db_addr), 0);

    const H5HF_create_t id1_param = MakeHeapParams(/*id_len=*/1);
    H5HF_t* fh_id1 = H5HF_create(f, &id1_param);
    ASSERT_NE(fh_id1, nullptr);
    EXPECT_EQ(fh_id1->hdr->id_len,
              1U + fh_id1->hdr->sizeof_addr + fh_id1->hdr->sizeof_size);
    const haddr_t id1_addr = fh_id1->hdr->heap_addr;
    EXPECT_GE(H5HF_close(fh_id1), 0);
    EXPECT_GE(H5HF_delete(f, id1_addr), 0);

    const H5HF_create_t id32_param = MakeHeapParams(/*id_len=*/32);
    H5HF_t* fh_id32 = H5HF_create(f, &id32_param);
    ASSERT_NE(fh_id32, nullptr);
    EXPECT_EQ(fh_id32->hdr->id_len, 32U);
    const haddr_t id32_addr = fh_id32->hdr->heap_addr;
    EXPECT_GE(H5HF_close(fh_id32), 0);
    EXPECT_GE(H5HF_delete(f, id32_addr), 0);

    const H5HF_create_t indir_param = MakeHeapParams(
        /*id_len=*/0, /*max_man_size=*/200, /*width=*/2,
        /*start_block_size=*/64, /*max_direct_size=*/256, /*max_index=*/16,
        /*start_root_rows=*/2);
    H5HF_t* fh_indir = H5HF_create(f, &indir_param);
    ASSERT_NE(fh_indir, nullptr);
    const haddr_t indir_addr = fh_indir->hdr->heap_addr;
    std::vector<uint8_t> iid(fh_indir->hdr->id_len, 0);
    ASSERT_GE(H5HF_insert(fh_indir, 40, MakeData(40, 1).data(), iid.data()), 0);
    ASSERT_GE(H5HF_insert(fh_indir, 40, MakeData(40, 2).data(), iid.data()), 0);
    EXPECT_GT(fh_indir->hdr->man_dtable.curr_root_rows, 0U);
    EXPECT_GE(H5HF_close(fh_indir), 0);
    EXPECT_GE(H5HF_delete(f, indir_addr), 0);

    H5HF_t* fh_huge = H5HF_create(f, &cparam);
    ASSERT_NE(fh_huge, nullptr);
    const haddr_t huge_addr = fh_huge->hdr->heap_addr;
    std::vector<uint8_t> hid(fh_huge->hdr->id_len, 0);
    ASSERT_GE(H5HF_insert(fh_huge, 250, MakeData(250, 3).data(), hid.data()),
              0);
    EXPECT_NE(fh_huge->hdr->huge_bt2_addr, HADDR_UNDEF);
    EXPECT_GE(H5HF_close(fh_huge), 0);
    EXPECT_GE(H5HF_delete(f, huge_addr), 0);
  }

  // 3. Error branches in H5HF__hdr_create (all triggering H5HF__hdr_free):
  //    can_apply fail, set_local fail, id_len too small, id_len too large, and
  //    (max_direct_size - dblock_overhead) < max_man_size.
  {
    ScopedErrorSilence silence;

    H5HF_create_t filt_param = MakeHeapParams(/*id_len=*/0);
    absl::Cleanup reset_pline = [&] {
      if (filt_param.pline.nused > 0) {
        H5O_msg_reset(H5O_PLINE_ID, &filt_param.pline);
      }
    };
    ASSERT_GE(H5Z_append(&filt_param.pline, kHdrFilterId, H5Z_FLAG_MANDATORY, 0,
                         nullptr),
              0);

    g_allow_can_apply = false;
    EXPECT_EQ(H5HF_create(f, &filt_param), nullptr);
    g_allow_can_apply = true;

    g_allow_set_local = false;
    EXPECT_EQ(H5HF_create(f, &filt_param), nullptr);
    g_allow_set_local = true;

    // Also test id_len out of range WITH pline present so H5HF__hdr_free resets
    // pline cleanly.
    filt_param.id_len = 2;
    EXPECT_EQ(H5HF_create(f, &filt_param), nullptr);

    const H5HF_create_t too_large_id =
        MakeHeapParams(/*id_len=*/H5HF_MAX_ID_LEN + 1);
    EXPECT_EQ(H5HF_create(f, &too_large_id), nullptr);

    const H5HF_create_t too_large_man = MakeHeapParams(
        /*id_len=*/0, /*max_man_size=*/256, /*width=*/2,
        /*start_block_size=*/64, /*max_direct_size=*/256);
    EXPECT_EQ(H5HF_create(f, &too_large_man), nullptr);
  }

  // 4. Direct test of H5HF__hdr_alloc, H5HF__hdr_free, H5HF__hdr_adj_free,
  //    and H5HF__hdr_protect error path.
  {
    H5HF_hdr_t* raw_hdr = H5HF__hdr_alloc(f);
    ASSERT_NE(raw_hdr, nullptr);
    EXPECT_EQ(raw_hdr->f, f);
    EXPECT_GE(H5HF__hdr_free(raw_hdr), 0);

    const H5HF_create_t cparam = MakeHeapParams(/*id_len=*/0);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    const hsize_t orig_free = fh->hdr->total_man_free;
    EXPECT_GE(H5HF__hdr_adj_free(fh->hdr, 10), 0);
    EXPECT_EQ(fh->hdr->total_man_free, orig_free + 10U);
    EXPECT_GE(H5HF__hdr_adj_free(fh->hdr, -10), 0);
    EXPECT_EQ(fh->hdr->total_man_free, orig_free);
    EXPECT_GE(H5HF_close(fh), 0);

    ScopedErrorSilence silence;
    EXPECT_EQ(H5HF__hdr_protect(f, 0x2000000, H5AC__READ_ONLY_FLAG), nullptr);
  }
}

TEST(H5HFhdrTest, UpdateIterSkipsRowsDoublesRootWalksUpAndDownChildIblocks) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  // Case 1: Start root iblock at nrows=4 (<= max_direct_rows=5), fill row 3,
  // double root 4->8, fill row 4, then at row 5 (next_row >= max_direct_rows)
  // trigger unclamped child_entry skip (row_block_size[2]=128 < 512), walk
  // down into row 7's 5-row child iblock, and skip child rows 0..3!
  {
    const H5HF_create_t cparam = MakeHeapParams(
        /*id_len=*/0, /*max_man_size=*/450, /*width=*/4,
        /*start_block_size=*/64, /*max_direct_size=*/512, /*max_index=*/20,
        /*start_root_rows=*/4);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    absl::Cleanup close_fh = [&] { H5HF_close(fh); };

    // Insert tiny managed object in root dblock, then 200B object
    // (min_dblock_size=256 => min_dblock_row=3) -> root iblock has nrows=4.
    std::vector<uint8_t> id(fh->hdr->id_len, 0);
    ASSERT_GE(H5HF_insert(fh, 24, MakeData(24, 1).data(), id.data()), 0);
    ASSERT_GE(H5HF_insert(fh, 200, MakeData(200, 2).data(), id.data()), 0);
    EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 4U);

    // Fill remaining 3 cols of row 3 (256B blocks), then 450B doubles root 4->8
    // and enters row 4 col 0; 3 more 450B fill row 4 cols 1..3 -> next_row=5!
    for (int i = 0; i < 3; ++i) {
      ASSERT_GE(H5HF_insert(fh, 200, MakeData(200, 3).data(), id.data()), 0);
    }
    for (int i = 0; i < 4; ++i) {
      ASSERT_GE(H5HF_insert(fh, 450, MakeData(450, 4).data(), id.data()), 0);
    }
    EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 8U);

    // At next_row=5 in an 8-row root iblock, requesting 450B
    // (min_dblock_size=512) sees child_nrows=3 (max 128B < 512), skips rows
    // 5..6 (unclamped child_entry = 28 <= 32), walks down into row 7 col 0, and
    // skips child rows 0..3!
    ASSERT_GE(H5HF_insert(fh, 450, MakeData(450, 5).data(), id.data()), 0);
  }

  // Case 2: Hit BOTH min_dblock_row >= iblock->nrows AND clamped child_entry
  // (lines 1004-1005) using start_root_rows=3 -> doubles 3->6 -> iblock->nrows
  // == 6 at next_row=5!
  {
    const H5HF_create_t cparam = MakeHeapParams(
        /*id_len=*/0, /*max_man_size=*/450, /*width=*/4,
        /*start_block_size=*/64, /*max_direct_size=*/512, /*max_index=*/20,
        /*start_root_rows=*/3);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    absl::Cleanup close_fh = [&] { H5HF_close(fh); };

    std::vector<uint8_t> id(fh->hdr->id_len, 0);
    // Row 0 col 0 (24B), then 40B (min_dblock_size=64 => rows_needed=1 < 3)
    // creates 3-row root iblock at next_entry=2.
    ASSERT_GE(H5HF_insert(fh, 24, MakeData(24, 1).data(), id.data()), 0);
    ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, 2).data(), id.data()), 0);
    EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 3U);

    // Inserting 450B (min_dblock_row=4 >= iblock->nrows=3) skips rows 0..2
    // via line 934, doubles root 3->6, and places 450B at row 4 col 0.
    for (int i = 0; i < 4; ++i) {
      ASSERT_GE(H5HF_insert(fh, 450, MakeData(450, 3).data(), id.data()), 0);
    }
    EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 6U);

    // At next_row=5 with iblock->nrows=6: requesting min_dblock_size=512
    // computes child_entry = 7*4 = 28 > 6*4 = 24 -> clamps to 24 (lines
    // 1004-1005), then while loop doubles root 6->12 cleanly!
    ASSERT_GE(H5HF_insert(fh, 450, MakeData(450, 4).data(), id.data()), 0);
    EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 12U);
  }

  // Case 3: Also exercise min_dblock_row < iblock->nrows (line 936) in skip.
  {
    const H5HF_create_t cparam = MakeHeapParams(
        /*id_len=*/0, /*max_man_size=*/200, /*width=*/2,
        /*start_block_size=*/64, /*max_direct_size=*/256, /*max_index=*/16,
        /*start_root_rows=*/4);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    absl::Cleanup close_fh = [&] { H5HF_close(fh); };

    std::vector<uint8_t> id(fh->hdr->id_len, 0);
    ASSERT_GE(H5HF_insert(fh, 24, MakeData(24, 1).data(), id.data()), 0);
    ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, 2).data(), id.data()), 0);
    EXPECT_EQ(fh->hdr->man_dtable.curr_root_rows, 4U);
    // Request 100B (min_dblock_size=128 => min_dblock_row=2 < nrows=4) ->
    // skips to row 2 via line 936.
    ASSERT_GE(H5HF_insert(fh, 100, MakeData(100, 3).data(), id.data()), 0);
  }

  // Case 4: Also exercise start_root_rows=1 doubling (1->2->4->8) across row
  // skips and multi-level indirect blocks.
  {
    const H5HF_create_t cparam = MakeHeapParams(
        /*id_len=*/0, /*max_man_size=*/200, /*width=*/2,
        /*start_block_size=*/64, /*max_direct_size=*/256, /*max_index=*/16,
        /*start_root_rows=*/1);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    absl::Cleanup close_fh = [&] { H5HF_close(fh); };

    std::vector<uint8_t> id(fh->hdr->id_len, 0);
    ASSERT_GE(H5HF_insert(fh, 24, MakeData(24, 1).data(), id.data()), 0);
    ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, 2).data(), id.data()), 0);
    ASSERT_GE(H5HF_insert(fh, 200, MakeData(200, 3).data(), id.data()), 0);
    for (int i = 0; i < 6; ++i) {
      ASSERT_GE(H5HF_insert(fh, 200, MakeData(200, 4).data(), id.data()), 0);
    }
  }

  // Case 5: Fill every block inside a small child indirect block so
  // while (next_row >= iblock->nrows) hits iblock->parent != NULL (lines
  // 960-969), advancing the parent iterator from (row 4, col 0) to (4, 1).
  {
    const H5HF_create_t cparam = MakeHeapParams(
        /*id_len=*/0, /*max_man_size=*/200, /*width=*/2,
        /*start_block_size=*/64, /*max_direct_size=*/256, /*max_index=*/16,
        /*start_root_rows=*/2);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    absl::Cleanup close_fh = [&] { H5HF_close(fh); };

    std::vector<uint8_t> id(fh->hdr->id_len, 0);
    // Rows 0..3 have widths 2 each = 8 direct blocks in root.
    for (int i = 0; i < 4; ++i) {
      ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, i).data(), id.data()), 0);
    }
    for (int i = 0; i < 2; ++i) {
      ASSERT_GE(H5HF_insert(fh, 100, MakeData(100, i).data(), id.data()), 0);
    }
    for (int i = 0; i < 2; ++i) {
      ASSERT_GE(H5HF_insert(fh, 200, MakeData(200, i).data(), id.data()), 0);
    }
    // Now inside child indirect block at (row 4, col 0): 3 rows (64, 64, 128).
    for (int i = 0; i < 4; ++i) {
      ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, 20 + i).data(), id.data()), 0);
    }
    for (int i = 0; i < 2; ++i) {
      ASSERT_GE(H5HF_insert(fh, 100, MakeData(100, 30 + i).data(), id.data()),
                0);
    }
    // Next insert overflows child indirect block (row 4, col 0) and triggers
    // iblock->parent != NULL in while (next_row >= iblock->nrows), advancing
    // to (row 4, col 1) and creating child 1!
    ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, 40).data(), id.data()), 0);
  }
}

TEST(H5HFhdrTest, ReverseIterAcrossChildIblocksReopenAndResetToEmpty) {
  haddr_t reopen_heap_addr = HADDR_UNDEF;
  std::vector<uint8_t> reopen_last_id;
  std::vector<uint8_t> image;

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    // 1. Reverse across child indirect blocks:
    //    Build heap down to child iblock 0 at (row 4, col 0) AND child iblock 1
    //    at (row 4, col 1), then remove child 1's sole block -> reverse_iter
    //    walks UP out of child 1, finds child 0 (`row >= max_direct_rows`), and
    //    walks DOWN into child 0!
    //    Then also test skipping HADDR_UNDEF holes and reversing to empty root!
    {
      const H5HF_create_t cparam = MakeHeapParams(
          /*id_len=*/0, /*max_man_size=*/200, /*width=*/2,
          /*start_block_size=*/64, /*max_direct_size=*/256, /*max_index=*/16,
          /*start_root_rows=*/2);
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      absl::Cleanup close_fh = [&] { H5HF_close(fh); };

      std::vector<std::vector<uint8_t>> ids;
      auto insert_one = [&](size_t sz, uint8_t seed) {
        std::vector<uint8_t> id(fh->hdr->id_len, 0);
        const std::vector<uint8_t> payload = MakeData(sz, seed);
        ASSERT_GE(H5HF_insert(fh, payload.size(), payload.data(), id.data()),
                  0);
        ids.push_back(id);
      };

      // Root direct entries: rows 0..3 (8 blocks total: 4*40, 2*100, 2*200).
      for (int i = 0; i < 4; ++i) insert_one(40, i);
      for (int i = 0; i < 2; ++i) insert_one(100, 10 + i);
      for (int i = 0; i < 2; ++i) insert_one(200, 20 + i);
      // Child iblock 0 at row 4 col 0 (4*40, 2*100).
      for (int i = 0; i < 4; ++i) insert_one(40, 30 + i);
      for (int i = 0; i < 2; ++i) insert_one(100, 40 + i);
      // Child iblock 1 at row 4 col 1 (1 object of 40B).
      insert_one(40, 50);

      // Removing child iblock 1's only block (ids.back()) reverses out of
      // child 1 (`iblock->parent != NULL`), finds child 0 at (row 4, col 0),
      // and walks DOWN into child 0!
      EXPECT_GE(H5HF_remove(fh, ids.back().data()), 0);
      ids.pop_back();

      // Remove remaining objects EXCEPT ids[1] (entry 1), then remove ids[0]
      // (creating an HADDR_UNDEF hole at entry 0 while root still has entry 1),
      // and finally remove ids[1] -> reverse_iter sees tmp_entry < 0 with
      // iblock->parent == NULL, resetting man_iter_off=0 and next_block!
      for (size_t i = ids.size(); i > 2; --i) {
        EXPECT_GE(H5HF_remove(fh, ids[i - 1].data()), 0);
      }
      EXPECT_GE(H5HF_remove(fh, ids[0].data()), 0);
      EXPECT_GE(H5HF_remove(fh, ids[1].data()), 0);
      EXPECT_EQ(fh->hdr->man_iter_off, 0U);
      EXPECT_EQ(fh->hdr->man_dtable.table_addr, HADDR_UNDEF);
    }

    // 2. Create heap with indirect root + 2 blocks, flush and save file image
    //    so reopening tests:
    //    - H5HF__hdr_finish_init / compute_free_space with max_root_rows >
    //      max_direct_rows
    //    - update_iter when !H5HF__man_iter_ready(&hdr->next_block)
    //    - reverse_iter when !H5HF__man_iter_ready(&hdr->next_block)
    {
      const H5HF_create_t cparam = MakeHeapParams(
          /*id_len=*/0, /*max_man_size=*/200, /*width=*/2,
          /*start_block_size=*/64, /*max_direct_size=*/256, /*max_index=*/16,
          /*start_root_rows=*/2);
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      reopen_heap_addr = fh->hdr->heap_addr;
      std::vector<uint8_t> id1(fh->hdr->id_len, 0);
      reopen_last_id.resize(fh->hdr->id_len);
      ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, 1).data(), id1.data()), 0);
      ASSERT_GE(
          H5HF_insert(fh, 40, MakeData(40, 2).data(), reopen_last_id.data()),
          0);
      EXPECT_GE(H5HF_close(fh), 0);
    }

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  // Reopen A: remove highest block while !H5HF__man_iter_ready -> reverse_iter
  // initializes iterator via H5HF__man_iter_start_offset!
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedFile f2_sc(H5Fopen("hdr_reopen1.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(f2_sc.valid());

    ScopedApiContext ctx2;
    H5HF_t* fh = H5HF_open(GetFilePtr(f2_sc.get()), reopen_heap_addr);
    ASSERT_NE(fh, nullptr);
    EXPECT_FALSE(H5HF__man_iter_ready(&fh->hdr->next_block));
    EXPECT_GE(H5HF_remove(fh, reopen_last_id.data()), 0);
    EXPECT_GE(H5HF_close(fh), 0);
  }

  // Reopen B: insert another block while !H5HF__man_iter_ready -> update_iter
  // initializes iterator via H5HF__man_iter_start_offset!
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedFile f2_sc(H5Fopen("hdr_reopen2.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(f2_sc.valid());

    ScopedApiContext ctx2;
    H5HF_t* fh = H5HF_open(GetFilePtr(f2_sc.get()), reopen_heap_addr);
    ASSERT_NE(fh, nullptr);
    EXPECT_FALSE(H5HF__man_iter_ready(&fh->hdr->next_block));
    std::vector<uint8_t> id3(fh->hdr->id_len, 0);
    EXPECT_GE(H5HF_insert(fh, 40, MakeData(40, 3).data(), id3.data()), 0);
    EXPECT_GE(H5HF_close(fh), 0);
  }
}

TEST(H5HFhdrTest, HeaderCreateCacheReverseAndDeleteErrorPaths) {
  // 1. Line 391: H5O_pline_set_version failure when version exceeds the file's
  //    high-bound pipeline version on a filtered heap.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_create_t cparam = MakeHeapParams();
    unsigned int cd_vals[1] = {1};
    ASSERT_GE(H5Z_append(&cparam.pline, H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL,
                         1, cd_vals),
              0);
    cparam.pline.version = 99;
    absl::Cleanup reset_pline = [&] {
      if (cparam.pline.nused > 0) {
        H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
      }
    };
    ScopedErrorSilence silence;
    EXPECT_EQ(H5HF_create(f, &cparam), nullptr);
  }

  // 2. Lines 465 and 469: H5MF_alloc failure (tmp_addr collision on metadata
  //    extension) and H5AC_insert_entry failure (cleared H5F_ACC_RDWR flag).
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_meta_block_size(fapl.get(), 128), 0);
    ScopedFile file(
        H5Fcreate("hdr_alloc_err.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    hid_t grp =
        H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    ASSERT_GE(grp, 0);
    EXPECT_GE(H5Gclose(grp), 0);

    const H5HF_create_t cparam = MakeHeapParams();
    {
      ScopedErrorSilence silence;
      const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      const haddr_t saved_tmp = f->shared->tmp_addr;
      f->shared->tmp_addr = eoa + 16;
      absl::Cleanup restore_tmp = [&] { f->shared->tmp_addr = saved_tmp; };
      EXPECT_EQ(H5HF_create(f, &cparam), nullptr);
    }

    {
      ScopedErrorSilence silence;
      const unsigned saved_flags = f->shared->flags;
      f->shared->flags &= ~H5F_ACC_RDWR;
      absl::Cleanup restore_flags = [&] { f->shared->flags = saved_flags; };
      EXPECT_EQ(H5HF_create(f, &cparam), nullptr);
    }
  }

  // 3. Lines 549, 585, 661, 665, 698, 734, 1293: unpinned & unprotected
  //    cache failure paths in H5HF__hdr_{incr,decr,dirty,adj_free,adjust_heap,
  //    empty}.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    // Filtered closed heap header -> H5HF__hdr_dirty resize failure (661).
    {
      H5HF_create_t filt_cparam = MakeHeapParams();
      unsigned int cd_vals[1] = {1};
      ASSERT_GE(H5Z_append(&filt_cparam.pline, H5Z_FILTER_DEFLATE,
                           H5Z_FLAG_OPTIONAL, 1, cd_vals),
                0);
      absl::Cleanup reset_pline = [&] {
        if (filt_cparam.pline.nused > 0) {
          H5O_msg_reset(H5O_PLINE_ID, &filt_cparam.pline);
        }
      };

      H5HF_t* fh = H5HF_create(f, &filt_cparam);
      ASSERT_NE(fh, nullptr);
      const haddr_t addr = fh->hdr->heap_addr;
      EXPECT_GE(H5HF_close(fh), 0);

      H5HF_hdr_t* hdr = H5HF__hdr_protect(f, addr, H5AC__READ_ONLY_FLAG);
      ASSERT_NE(hdr, nullptr);
      ASSERT_GE(H5AC_unprotect(f, H5AC_FHEAP_HDR, addr, hdr, H5AC__NO_FLAGS_SET),
                0);
      ASSERT_EQ(hdr->rc, 0U);
      ASSERT_EQ(hdr->file_rc, 0U);

      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5HF__hdr_dirty(hdr), 0);
      }
      EXPECT_GE(H5HF_delete(f, addr), 0);
    }

    // Unfiltered closed heap header -> 549, 665, 698, 734, 1293, 585.
    {
      const H5HF_create_t cparam = MakeHeapParams();
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      const haddr_t addr = fh->hdr->heap_addr;
      EXPECT_GE(H5HF_close(fh), 0);

      H5HF_hdr_t* hdr = H5HF__hdr_protect(f, addr, H5AC__READ_ONLY_FLAG);
      ASSERT_NE(hdr, nullptr);
      ASSERT_GE(H5AC_unprotect(f, H5AC_FHEAP_HDR, addr, hdr, H5AC__NO_FLAGS_SET),
                0);
      ASSERT_EQ(hdr->rc, 0U);
      ASSERT_EQ(hdr->file_rc, 0U);

      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5HF__hdr_incr(hdr), 0);
        EXPECT_LT(H5HF__hdr_dirty(hdr), 0);
        const hsize_t saved_free = hdr->total_man_free;
        const hsize_t saved_man_size = hdr->man_size;
        const hsize_t saved_man_alloc = hdr->man_alloc_size;
        EXPECT_LT(H5HF__hdr_adj_free(hdr, 0), 0);
        EXPECT_LT(H5HF__hdr_adjust_heap(hdr, 0, 0), 0);
        EXPECT_LT(H5HF__hdr_empty(hdr), 0);
        hdr->total_man_free = saved_free;
        hdr->man_size = saved_man_size;
        hdr->man_alloc_size = saved_man_alloc;

        hdr->rc = 1;
        absl::Cleanup restore_rc = [&] { hdr->rc = 0; };
        EXPECT_LT(H5HF__hdr_decr(hdr), 0);
      }
      EXPECT_GE(H5HF_delete(f, addr), 0);
    }
  }

  // 4. Lines 903, 958, 1020, 1222: update_iter & reverse_iter error paths.
  {
    // (a) Line 903: empty heap root_create failure on H5AC_insert_entry
    //     (!RDWR), failing cleanly after iblock->parent = NULL is initialized.
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

      ScopedErrorSilence silence;
      const unsigned saved_flags = f->shared->flags;
      f->shared->flags &= ~H5F_ACC_RDWR;
      absl::Cleanup restore_flags = [&] { f->shared->flags = saved_flags; };
      EXPECT_LT(H5HF__hdr_update_iter(fh->hdr, 200), 0);
    }

    // (b) Line 958: indirect root with next_row == iblock->nrows == 2 ->
    //     root_double fails cleanly on exhausted tmp_addr.
    {
      ScopedFile file = CreateInMemoryFile();
      ASSERT_TRUE(file.valid());
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = GetFilePtr(file.get());
      ASSERT_NE(f, nullptr);

      const H5HF_create_t cparam = MakeHeapParams(
          /*id_len=*/0, /*max_man_size=*/200, /*width=*/2,
          /*start_block_size=*/64, /*max_direct_size=*/256, /*max_index=*/16,
          /*start_root_rows=*/2);
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      absl::Cleanup close_fh = [&] { H5HF_close(fh); };

      std::vector<uint8_t> id(fh->hdr->id_len, 0);
      for (int i = 0; i < 4; ++i) {
        ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, i).data(), id.data()), 0);
      }
      ASSERT_EQ(fh->hdr->man_dtable.curr_root_rows, 2U);
      ASSERT_NE(fh->hdr->root_iblock, nullptr);

      ScopedErrorSilence silence;
      const unsigned saved_nrows = fh->hdr->root_iblock->nrows;
      const size_t saved_size = fh->hdr->root_iblock->size;
      const haddr_t saved_tmp = f->shared->tmp_addr;
      f->shared->tmp_addr = H5F_get_eoa(f, H5FD_MEM_DEFAULT) + 16;
      absl::Cleanup restore_state = [&] {
        f->shared->tmp_addr = saved_tmp;
        fh->hdr->root_iblock->nrows = saved_nrows;
        fh->hdr->root_iblock->size = saved_size;
      };
      EXPECT_LT(H5HF__hdr_update_iter(fh->hdr, 200), 0);
    }

    // (c) Line 1020: start_root_rows=8 with rows 0..3 filled so next_row ==
    //     4 == max_direct_rows and root->nrows == 8; child iblock creation
    //     fails cleanly at H5AC_insert_entry (!RDWR) after parent is set.
    {
      ScopedFile file = CreateInMemoryFile();
      ASSERT_TRUE(file.valid());
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = GetFilePtr(file.get());
      ASSERT_NE(f, nullptr);

      const H5HF_create_t cparam = MakeHeapParams(
          /*id_len=*/0, /*max_man_size=*/200, /*width=*/2,
          /*start_block_size=*/64, /*max_direct_size=*/256, /*max_index=*/16,
          /*start_root_rows=*/8);
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
      ASSERT_EQ(fh->hdr->man_dtable.curr_root_rows, 8U);
      ASSERT_NE(fh->hdr->root_iblock, nullptr);

      ScopedErrorSilence silence;
      H5HF_indirect_t* root_ib = fh->hdr->root_iblock;
      const unsigned saved_nchildren = root_ib->nchildren;
      const unsigned saved_max_child = root_ib->max_child;
      const unsigned saved_flags = f->shared->flags;
      f->shared->flags &= ~H5F_ACC_RDWR;
      absl::Cleanup restore_state = [&] {
        f->shared->flags = saved_flags;
        root_ib->ents[8].addr = HADDR_UNDEF;
        root_ib->nchildren = saved_nchildren;
        root_ib->max_child = saved_max_child;
      };
      EXPECT_LT(H5HF__hdr_update_iter(fh->hdr, 64), 0);
    }

    // (d) Line 1222: build heap down to child 0 (4,0) and child 1 (4,1),
    //     reopen so child 0 is not in cache, insert 1x40 into child 1 col 1,
    //     remove it so reverse_iter initializes next_block at child 1 entry 1,
    //     then corrupt root_iblock->ents[8].addr and call reverse_iter!
    std::vector<uint8_t> image;
    haddr_t multi_addr = HADDR_UNDEF;
    {
      ScopedFile file = CreateInMemoryFile();
      ASSERT_TRUE(file.valid());
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = GetFilePtr(file.get());
      ASSERT_NE(f, nullptr);

      const H5HF_create_t cparam = MakeHeapParams(
          /*id_len=*/0, /*max_man_size=*/200, /*width=*/2,
          /*start_block_size=*/64, /*max_direct_size=*/256, /*max_index=*/16,
          /*start_root_rows=*/2);
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      multi_addr = fh->hdr->heap_addr;

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
      // Fill child 0 at (4,0) and add 1x40B into child 1 at (4,1) col 0.
      for (int i = 0; i < 4; ++i) {
        ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, 30 + i).data(), id.data()),
                  0);
      }
      for (int i = 0; i < 2; ++i) {
        ASSERT_GE(H5HF_insert(fh, 100, MakeData(100, 40 + i).data(), id.data()),
                  0);
      }
      ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, 50).data(), id.data()), 0);
      EXPECT_GE(H5HF_close(fh), 0);

      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
      ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
      ASSERT_GT(image_size, 0);
      image.resize(static_cast<size_t>(image_size));
      ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
                image_size);
    }

    // Reopen so child 0 is not in cache; insert 1x40 into child 1 col 1,
    // remove it so reverse_iter initializes next_block at child 1 entry 1, then
    // corrupt root_iblock->ents[8].addr and call reverse_iter to trigger 1222!
    {
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
      ScopedFile f2_sc(H5Fopen("hdr_rev_err.h5", H5F_ACC_RDWR, fapl.get()));
      ASSERT_TRUE(f2_sc.valid());
      ScopedApiContext ctx2;
      ASSERT_TRUE(ctx2.valid());

      H5HF_t* fh = H5HF_open(GetFilePtr(f2_sc.get()), multi_addr);
      ASSERT_NE(fh, nullptr);
      absl::Cleanup close_fh = [&] { H5HF_close(fh); };

      std::vector<uint8_t> id_c1_col1(fh->hdr->id_len, 0);
      ASSERT_GE(H5HF_insert(fh, 40, MakeData(40, 51).data(), id_c1_col1.data()),
                0);
      EXPECT_GE(H5HF_remove(fh, id_c1_col1.data()), 0);

      H5HF_indirect_t* cur_ib = nullptr;
      ASSERT_GE(H5HF__man_iter_curr(&fh->hdr->next_block, nullptr, nullptr,
                                    nullptr, &cur_ib),
                0);
      ASSERT_NE(cur_ib, nullptr);
      ASSERT_NE(cur_ib->parent, nullptr);
      const haddr_t c1_col0_addr = cur_ib->ents[0].addr;

      ScopedErrorSilence silence;
      const haddr_t saved_ent8 = cur_ib->parent->ents[8].addr;
      cur_ib->parent->ents[8].addr = 0x2000000;
      absl::Cleanup restore_ent8 = [&] {
        cur_ib->parent->ents[8].addr = saved_ent8;
      };
      EXPECT_LT(H5HF__hdr_reverse_iter(fh->hdr, c1_col0_addr), 0);
    }
  }

  // 5. Lines 1380, 1401, 1407, 1416: H5HF__hdr_delete error paths.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_FSM_AGGR,
                                   /*persist=*/false, /*threshold=*/1),
        0);
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());

    haddr_t h_fs_addr = HADDR_UNDEF;
    haddr_t h_db_addr = HADDR_UNDEF;
    haddr_t h_ib_addr = HADDR_UNDEF;
    haddr_t h_huge_addr = HADDR_UNDEF;
    std::vector<uint8_t> image;

    {
      ScopedFile file(
          H5Fcreate("hdr_del_err.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
      ASSERT_TRUE(file.valid());
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = GetFilePtr(file.get());
      ASSERT_NE(f, nullptr);

      // (a) Heap with open free-space section -> fs_addr != HADDR_UNDEF (1380).
      {
        const H5HF_create_t cparam = MakeHeapParams();
        H5HF_t* fh = H5HF_create(f, &cparam);
        ASSERT_NE(fh, nullptr);
        h_fs_addr = fh->hdr->heap_addr;
        std::vector<uint8_t> id(fh->hdr->id_len, 0);
        ASSERT_GE(H5HF_insert(fh, 24, MakeData(24, 1).data(), id.data()), 0);
        EXPECT_GE(H5HF_close(fh), 0);
      }

      // (b) Heap with 100% full 64B root direct block -> fs_addr == HADDR_UNDEF
      //     (so H5HF__hdr_delete proceeds directly to dblock_delete -> 1401).
      {
        const H5HF_create_t cparam = MakeHeapParams();
        H5HF_t* fh = H5HF_create(f, &cparam);
        ASSERT_NE(fh, nullptr);
        h_db_addr = fh->hdr->heap_addr;
        const size_t avail = fh->hdr->man_dtable.row_tot_dblock_free[0];
        std::vector<uint8_t> id(fh->hdr->id_len, 0);
        ASSERT_GE(
            H5HF_insert(fh, avail, MakeData(avail, 2).data(), id.data()), 0);
        EXPECT_GE(H5HF_close(fh), 0);
      }

      // (c) Heap with 2 full 64B blocks in indirect root -> fs_addr ==
      //     HADDR_UNDEF, curr_root_rows == 2 (so proceeds to iblock_delete ->
      //     1407).
      {
        const H5HF_create_t cparam = MakeHeapParams(
            /*id_len=*/0, /*max_man_size=*/200, /*width=*/2,
            /*start_block_size=*/64, /*max_direct_size=*/256, /*max_index=*/16,
            /*start_root_rows=*/2);
        H5HF_t* fh = H5HF_create(f, &cparam);
        ASSERT_NE(fh, nullptr);
        h_ib_addr = fh->hdr->heap_addr;
        const size_t avail = fh->hdr->man_dtable.row_tot_dblock_free[0];
        std::vector<uint8_t> id(fh->hdr->id_len, 0);
        ASSERT_GE(
            H5HF_insert(fh, avail, MakeData(avail, 3).data(), id.data()), 0);
        ASSERT_GE(
            H5HF_insert(fh, avail, MakeData(avail, 4).data(), id.data()), 0);
        EXPECT_GE(H5HF_close(fh), 0);
      }

      // (d) Heap with a huge object (no managed space, fs_addr == HADDR_UNDEF)
      //     -> proceeds to H5HF__huge_delete -> 1416.
      {
        const H5HF_create_t cparam = MakeHeapParams();
        H5HF_t* fh = H5HF_create(f, &cparam);
        ASSERT_NE(fh, nullptr);
        h_huge_addr = fh->hdr->heap_addr;
        std::vector<uint8_t> id(fh->hdr->id_len, 0);
        ASSERT_GE(H5HF_insert(fh, 250, MakeData(250, 5).data(), id.data()), 0);
        EXPECT_GE(H5HF_close(fh), 0);
      }

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
        H5Fopen("hdr_del_err_reopen.h5", H5F_ACC_RDWR, reopen_fapl.get()));
    ASSERT_TRUE(reopened.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f2 = GetFilePtr(reopened.get());
    ASSERT_NE(f2, nullptr);

    // Line 1401: corrupt f2->shared->fs_addr[fs_idx] with fs_man == nullptr,
    // fail H5HF__hdr_delete on h_db_addr, restore, then delete cleanly. Must run
    // before any H5MF_xfree on f2 initializes fs_man[fs_idx].
    {
      const H5FD_mem_t mapped =
          (f2->shared->fs_type_map[H5FD_MEM_FHEAP_DBLOCK] == H5FD_MEM_DEFAULT)
              ? H5FD_MEM_FHEAP_DBLOCK
              : f2->shared->fs_type_map[H5FD_MEM_FHEAP_DBLOCK];
      const size_t fs_idx = static_cast<size_t>(mapped);
      ASSERT_EQ(f2->shared->fs_man[fs_idx], nullptr);

      H5HF_hdr_t* hdr = H5HF__hdr_protect(f2, h_db_addr, H5AC__NO_FLAGS_SET);
      ASSERT_NE(hdr, nullptr);
      ASSERT_EQ(hdr->fs_addr, HADDR_UNDEF);
      {
        ScopedErrorSilence silence;
        const haddr_t saved_f_fs = f2->shared->fs_addr[fs_idx];
        f2->shared->fs_addr[fs_idx] = 0x2000000;
        absl::Cleanup restore_f_fs = [&] {
          f2->shared->fs_addr[fs_idx] = saved_f_fs;
        };
        EXPECT_LT(H5HF__hdr_delete(hdr), 0);
      }
      EXPECT_GE(H5HF_delete(f2, h_db_addr), 0);
    }

    // Line 1380: corrupt fs_addr on reopened heap, fail H5HF__hdr_delete,
    // restore fs_addr, then delete cleanly.
    {
      H5HF_hdr_t* hdr = H5HF__hdr_protect(f2, h_fs_addr, H5AC__NO_FLAGS_SET);
      ASSERT_NE(hdr, nullptr);
      ASSERT_NE(hdr->fs_addr, HADDR_UNDEF);
      const haddr_t saved_fs = hdr->fs_addr;
      hdr->fs_addr = 0x2000000;
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5HF__hdr_delete(hdr), 0);
      }
      hdr = H5HF__hdr_protect(f2, h_fs_addr, H5AC__NO_FLAGS_SET);
      ASSERT_NE(hdr, nullptr);
      hdr->fs_addr = saved_fs;
      EXPECT_GE(
          H5AC_unprotect(f2, H5AC_FHEAP_HDR, h_fs_addr, hdr, H5AC__NO_FLAGS_SET),
          0);
      EXPECT_GE(H5HF_delete(f2, h_fs_addr), 0);
    }

    // Line 1407: corrupt table_addr on h_ib_addr, fail H5HF__hdr_delete,
    // restore table_addr, then delete cleanly.
    {
      H5HF_hdr_t* hdr = H5HF__hdr_protect(f2, h_ib_addr, H5AC__NO_FLAGS_SET);
      ASSERT_NE(hdr, nullptr);
      ASSERT_EQ(hdr->fs_addr, HADDR_UNDEF);
      ASSERT_GT(hdr->man_dtable.curr_root_rows, 0U);
      const haddr_t saved_tbl = hdr->man_dtable.table_addr;
      hdr->man_dtable.table_addr = 0x2000000;
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5HF__hdr_delete(hdr), 0);
      }
      hdr = H5HF__hdr_protect(f2, h_ib_addr, H5AC__NO_FLAGS_SET);
      ASSERT_NE(hdr, nullptr);
      hdr->man_dtable.table_addr = saved_tbl;
      EXPECT_GE(
          H5AC_unprotect(f2, H5AC_FHEAP_HDR, h_ib_addr, hdr, H5AC__NO_FLAGS_SET),
          0);
      EXPECT_GE(H5HF_delete(f2, h_ib_addr), 0);
    }

    // Line 1416: corrupt huge_bt2_addr on h_huge_addr, fail H5HF__hdr_delete,
    // restore huge_bt2_addr, then delete cleanly.
    {
      H5HF_hdr_t* hdr = H5HF__hdr_protect(f2, h_huge_addr, H5AC__NO_FLAGS_SET);
      ASSERT_NE(hdr, nullptr);
      ASSERT_NE(hdr->huge_bt2_addr, HADDR_UNDEF);
      const haddr_t saved_bt2 = hdr->huge_bt2_addr;
      hdr->huge_bt2_addr = 0x2000000;
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5HF__hdr_delete(hdr), 0);
      }
      hdr = H5HF__hdr_protect(f2, h_huge_addr, H5AC__NO_FLAGS_SET);
      ASSERT_NE(hdr, nullptr);
      hdr->huge_bt2_addr = saved_bt2;
      EXPECT_GE(H5AC_unprotect(f2, H5AC_FHEAP_HDR, h_huge_addr, hdr,
                               H5AC__NO_FLAGS_SET),
                0);
      EXPECT_GE(H5HF_delete(f2, h_huge_addr), 0);
    }
  }
}

}  // namespace
}  // namespace hdf5
