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
// Tests fractal heap managed-object insertion, length/offset decoding, read,
// write, direct operator invocation, free-space row/single section reuse and
// revival, removal, and malformed-ID / filter / corrupt-block error paths
// (third_party/hdf5/src/H5HFman.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFman.c

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t) inside internal fractal heap structs.
#ifndef NDEBUG
#define H5HFMAN_TEST_RESTORE_UNDEF_NDEBUG 1
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
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#include "third_party/hdf5/src/H5Zprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HFMAN_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HFMAN_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

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

H5HF_create_t MakeManHeapParams(uint32_t max_man_size = 128,
                                unsigned width = 2,
                                size_t start_block_size = 64,
                                size_t max_direct_size = 256,
                                unsigned max_index = 14,
                                unsigned start_root_rows = 1,
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

std::vector<uint8_t> MakePattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>((seed + i * 19) & 0xffU);
  }
  return data;
}

std::vector<uint8_t> EncodeManagedId(const H5HF_hdr_t* hdr, hsize_t off,
                                     size_t len) {
  std::vector<uint8_t> id(hdr->id_len, 0);
  uint8_t* p = id.data();
  H5HF_MAN_ID_ENCODE(p, hdr, off, len);
  return id;
}

// Custom H5Z filter toggled to test H5HF_MAN_WRITE_CHECK_PLINE failure paths.
constexpr H5Z_filter_t kManToggleFilterId = H5Z_FILTER_RESERVED + 12;
bool g_man_filter_allow = true;

htri_t ManFilterCanApply(hid_t /*dcpl_id*/, hid_t /*type_id*/,
                         hid_t /*space_id*/) {
  return g_man_filter_allow ? 1 : -1;
}

size_t ManFilterApply(unsigned int /*flags*/, size_t /*cd_nelmts*/,
                      const unsigned int /*cd_values*/[], size_t nbytes,
                      size_t* /*buf_size*/, void** /*buf*/) {
  return nbytes;
}

TEST(H5HFmanTest, RootDirectBlockAndMultiLevelLifecycle) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  const H5HF_create_t cparam = MakeManHeapParams();
  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
  H5HF_hdr_t* hdr = fh->hdr;

  // 1. Insert, read, write, op, decode len/off, and remove while heap has only
  //    a root direct block (curr_root_rows == 0).
  const std::vector<uint8_t> obj0 = MakePattern(20, 0x11U);
  std::vector<uint8_t> id0(hdr->id_len, 0);
  ASSERT_GE(H5HF__man_insert(hdr, obj0.size(), obj0.data(), id0.data()), 0);
  EXPECT_EQ(hdr->man_dtable.curr_root_rows, 0U);

  size_t got_len = 0;
  hsize_t got_off = 0;
  ASSERT_GE(H5HF__man_get_obj_len(hdr, id0.data(), &got_len), 0);
  H5HF__man_get_obj_off(hdr, id0.data(), &got_off);
  EXPECT_EQ(got_len, obj0.size());
  EXPECT_GT(got_off, 0U);

  std::vector<uint8_t> read_buf(obj0.size(), 0xaaU);
  ASSERT_GE(H5HF__man_read(hdr, id0.data(), read_buf.data()), 0);
  EXPECT_THAT(read_buf, ElementsAreArray(obj0));

  const std::vector<uint8_t> obj0_updated = MakePattern(20, 0x22U);
  ASSERT_GE(H5HF__man_write(hdr, id0.data(), obj0_updated.data()), 0);

  std::vector<uint8_t> op_observed;
  auto copy_op = [](const void* obj, size_t len, void* op_data) -> herr_t {
    const auto* bytes = static_cast<const uint8_t*>(obj);
    static_cast<std::vector<uint8_t>*>(op_data)->assign(bytes, bytes + len);
    return 0;
  };
  ASSERT_GE(H5HF__man_op(hdr, id0.data(), copy_op, &op_observed), 0);
  EXPECT_THAT(op_observed, ElementsAreArray(obj0_updated));

  {
    ScopedErrorSilence silence;
    auto fail_op = [](const void*, size_t, void*) -> herr_t { return -1; };
    EXPECT_LT(H5HF__man_op(hdr, id0.data(), fail_op, nullptr), 0);
  }

  ASSERT_GE(H5HF__man_remove(hdr, id0.data()), 0);
  EXPECT_EQ(hdr->man_nobjs, 0U);

  // 2. Re-insert and grow into indirect blocks (curr_root_rows > 0), then
  //    exercise read, write, op, and removal across indirect rows.
  std::vector<std::vector<uint8_t>> ids;
  std::vector<std::vector<uint8_t>> payloads;
  for (int i = 0; i < 6; ++i) {
    payloads.push_back(MakePattern(40, static_cast<uint8_t>(i * 11 + 3)));
    ids.emplace_back(hdr->id_len, 0);
    ASSERT_GE(H5HF__man_insert(hdr, payloads.back().size(),
                               payloads.back().data(), ids.back().data()),
              0);
  }
  EXPECT_GT(hdr->man_dtable.curr_root_rows, 0U);

  const std::vector<uint8_t> updated_mid = MakePattern(40, 0x77U);
  ASSERT_GE(H5HF__man_write(hdr, ids[3].data(), updated_mid.data()), 0);

  std::vector<uint8_t> out_mid(40, 0);
  ASSERT_GE(H5HF__man_read(hdr, ids[3].data(), out_mid.data()), 0);
  EXPECT_THAT(out_mid, ElementsAreArray(updated_mid));

  op_observed.clear();
  ASSERT_GE(H5HF__man_op(hdr, ids[3].data(), copy_op, &op_observed), 0);
  EXPECT_THAT(op_observed, ElementsAreArray(updated_mid));

  // Remove middle object and re-insert into live single free-space section.
  ASSERT_GE(H5HF__man_remove(hdr, ids[2].data()), 0);
  std::vector<uint8_t> reused_id(hdr->id_len, 0);
  const std::vector<uint8_t> reused_obj = MakePattern(40, 0x55U);
  ASSERT_GE(H5HF__man_insert(hdr, reused_obj.size(), reused_obj.data(),
                             reused_id.data()),
            0);
  std::vector<uint8_t> reused_out(40, 0);
  ASSERT_GE(H5HF__man_read(hdr, reused_id.data(), reused_out.data()), 0);
  EXPECT_THAT(reused_out, ElementsAreArray(reused_obj));
}

TEST(H5HFmanTest, RowSectionAllocationAndSerializedSectionRevival) {
  haddr_t heap_addr = HADDR_UNDEF;
  std::vector<uint8_t> image;

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    // max_direct_size=256 allows a 180B object to skip 64B blocks in rows 0-1,
    // creating row sections; start_block_size=64 holds two 16B objects or one
    // 40B object.
    const H5HF_create_t cparam = MakeManHeapParams(/*max_man_size=*/200);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() {
      if (fh != nullptr) {
        H5HF_close(fh);
      }
    });
    ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);
    H5HF_hdr_t* hdr = fh->hdr;

    // 1. Put two 16B objects in block 0, then remove the first so block 0 has
    //    a 16B single free section (not big enough for 36B).
    std::vector<uint8_t> id_a(hdr->id_len, 0);
    std::vector<uint8_t> id_b(hdr->id_len, 0);
    const std::vector<uint8_t> obj_a = MakePattern(16, 0x01U);
    const std::vector<uint8_t> obj_b = MakePattern(16, 0x02U);
    ASSERT_GE(H5HF__man_insert(hdr, obj_a.size(), obj_a.data(), id_a.data()),
              0);
    ASSERT_GE(H5HF__man_insert(hdr, obj_b.size(), obj_b.data(), id_b.data()),
              0);
    ASSERT_GE(H5HF__man_remove(hdr, id_a.data()), 0);

    // 2. Insert a 180B object requiring a 256B block: skips blocks 1..3 (64B
    //    each) and adds FIRST_ROW / NORMAL_ROW sections.
    std::vector<uint8_t> id_big(hdr->id_len, 0);
    const std::vector<uint8_t> obj_big = MakePattern(180, 0x33U);
    ASSERT_GE(
        H5HF__man_insert(hdr, obj_big.size(), obj_big.data(), id_big.data()),
        0);

    // 3. Insert a 36B object: allocates from a live row section via
    //    H5HF__man_iblock_alloc_row().
    std::vector<uint8_t> id_row_alloc(hdr->id_len, 0);
    const std::vector<uint8_t> obj_row_alloc = MakePattern(36, 0x44U);
    ASSERT_GE(H5HF__man_insert(hdr, obj_row_alloc.size(), obj_row_alloc.data(),
                               id_row_alloc.data()),
              0);

    EXPECT_GE(H5HF_close(fh), 0);
    fh = nullptr;

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  // 4. Reopen the file image so persisted free-space sections start as
  //    H5FS_SECT_SERIALIZED, triggering H5HF__sect_single_revive() inside
  //    H5HF__man_insert() and serialized row revival in alloc_row().
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("man_reopen.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5F_t* f2 = GetFilePtr(reopened.get());
  ASSERT_NE(f2, nullptr);

  H5HF_t* fh2 = H5HF_open(f2, heap_addr);
  ASSERT_NE(fh2, nullptr);
  auto close_fh2 = absl::MakeCleanup([&]() { H5HF_close(fh2); });
  H5HF_hdr_t* hdr2 = fh2->hdr;

  // 16B insert revives the serialized single section in block 0.
  std::vector<uint8_t> id_rev_single(hdr2->id_len, 0);
  const std::vector<uint8_t> obj_rev_single = MakePattern(16, 0x66U);
  ASSERT_GE(H5HF__man_insert(hdr2, obj_rev_single.size(), obj_rev_single.data(),
                             id_rev_single.data()),
            0);
  std::vector<uint8_t> check_single(16, 0);
  ASSERT_GE(H5HF__man_read(hdr2, id_rev_single.data(), check_single.data()), 0);
  EXPECT_THAT(check_single, ElementsAreArray(obj_rev_single));

  // 36B insert revives the serialized row section in block 1..2.
  std::vector<uint8_t> id_rev_row(hdr2->id_len, 0);
  const std::vector<uint8_t> obj_rev_row = MakePattern(36, 0x88U);
  ASSERT_GE(H5HF__man_insert(hdr2, obj_rev_row.size(), obj_rev_row.data(),
                             id_rev_row.data()),
            0);
  std::vector<uint8_t> check_row(36, 0);
  ASSERT_GE(H5HF__man_read(hdr2, id_rev_row.data(), check_row.data()), 0);
  EXPECT_THAT(check_row, ElementsAreArray(obj_rev_row));
}

TEST(H5HFmanTest, MalformedIdsAndFilterRejectionReturnErrors) {
  const H5Z_class2_t toggle_filter = {
      .version = H5Z_CLASS_T_VERS,
      .id = kManToggleFilterId,
      .encoder_present = 1,
      .decoder_present = 1,
      .name = "man_toggle_filter",
      .can_apply = ManFilterCanApply,
      .set_local = nullptr,
      .filter = ManFilterApply,
  };
  g_man_filter_allow = true;
  ASSERT_GE(H5Zregister(&toggle_filter), 0);
  auto unregister_filter = absl::MakeCleanup([&]() {
    g_man_filter_allow = true;
    H5Zunregister(kManToggleFilterId);
  });

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  // max_man_size=256 and max_direct_size=512 ensure hdr->heap_len_size == 2 so
  // both `obj_len > max_direct_size` and `max_man_size < obj_len <=
  // max_direct_size` encode without 8-bit truncation.
  H5HF_create_t cparam = MakeManHeapParams(
      /*max_man_size=*/256, /*width=*/2, /*start_block_size=*/64,
      /*max_direct_size=*/512);
  auto reset_pline = absl::MakeCleanup([&]() {
    if (cparam.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
    }
  });
  ASSERT_GE(
      H5Z_append(&cparam.pline, kManToggleFilterId, H5Z_FLAG_MANDATORY, 0,
                 nullptr),
      0);

  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });
  H5HF_hdr_t* hdr = fh->hdr;
  ASSERT_EQ(hdr->heap_len_size, 2U);

  // Insert 3 objects (40B, 40B, and skip/create entry at row 1) so
  // curr_root_rows > 0 and there is an unallocated direct block entry at
  // offset 128 inside man_size=256.
  std::vector<uint8_t> valid_id(hdr->id_len, 0);
  const std::vector<uint8_t> obj1 = MakePattern(40, 0x10U);
  ASSERT_GE(H5HF__man_insert(hdr, obj1.size(), obj1.data(), valid_id.data()),
            0);

  // Insert a 64B object -> creates root iblock and allocates entry 2 (offset
  // 128, size 64) while entry 1 (offset 64) remains HADDR_UNDEF!
  std::vector<uint8_t> id_row1(hdr->id_len, 0);
  const std::vector<uint8_t> obj2 = MakePattern(44, 0x20U);
  ASSERT_GE(H5HF__man_insert(hdr, obj2.size(), obj2.data(), id_row1.data()), 0);
  std::vector<uint8_t> id_entry2(hdr->id_len, 0);
  ASSERT_GE(H5HF__man_insert(hdr, obj2.size(), obj2.data(), id_entry2.data()),
            0);
  // Now remove entry 1? Entry 1 was used; instead skip row 1 or check where
  // entry 1 vs entry 2 is: remove id_row1 so entry 1 is detached (HADDR_UNDEF),
  // while entry 2 remains allocated so man_size covers [0, 256).
  ASSERT_GE(H5HF__man_remove(hdr, id_row1.data()), 0);

  ScopedErrorSilence silence;
  std::vector<uint8_t> scratch(64, 0);
  auto noop = [](const void*, size_t, void*) -> herr_t { return 0; };

  // 1. H5HF_MAN_WRITE_CHECK_PLINE failure in insert, write, and remove.
  g_man_filter_allow = false;
  hdr->checked_filters = false;
  EXPECT_LT(
      H5HF__man_insert(hdr, obj1.size(), obj1.data(), scratch.data()), 0);
  EXPECT_LT(H5HF__man_write(hdr, valid_id.data(), obj1.data()), 0);
  EXPECT_LT(H5HF__man_remove(hdr, valid_id.data()), 0);
  g_man_filter_allow = true;
  hdr->checked_filters = true;

  // 2. Malformed IDs across H5HF__man_read/write/op/remove.
  const std::array<std::vector<uint8_t>, 8> bad_ids = {{
      // obj_off == 0
      EncodeManagedId(hdr, /*off=*/0, /*len=*/16),
      // obj_off > hdr->man_size
      EncodeManagedId(hdr, hdr->man_size + 16, /*len=*/16),
      // obj_len == 0
      EncodeManagedId(hdr, /*off=*/20, /*len=*/0),
      // obj_len > max_direct_size
      EncodeManagedId(hdr, /*off=*/20, hdr->man_dtable.cparam.max_direct_size + 8),
      // max_man_size < obj_len <= max_direct_size (standalone threshold)
      EncodeManagedId(hdr, /*off=*/20, hdr->max_man_size + 16),
      // Offset inside unallocated direct block entry 1 (HADDR_UNDEF)
      EncodeManagedId(hdr, /*off=*/64 + 20, /*len=*/16),
      // Offset inside direct block prefix (< H5HF_MAN_ABS_DIRECT_OVERHEAD)
      EncodeManagedId(hdr, /*off=*/1, /*len=*/16),
      // Object overruns end of 64B direct block
      EncodeManagedId(hdr, /*off=*/60, /*len=*/16),
  }};

  for (const auto& bad_id : bad_ids) {
    EXPECT_LT(H5HF__man_read(hdr, bad_id.data(), scratch.data()), 0);
    EXPECT_LT(H5HF__man_write(hdr, bad_id.data(), obj1.data()), 0);
    EXPECT_LT(H5HF__man_op(hdr, bad_id.data(), noop, nullptr), 0);
    EXPECT_LT(H5HF__man_remove(hdr, bad_id.data()), 0);
  }
}

TEST(H5HFmanTest, CorruptDirectBlockFailsProtectInInsertAndOp) {
  haddr_t heap_addr = HADDR_UNDEF;
  haddr_t target_dblock_addr = HADDR_UNDEF;
  std::vector<uint8_t> target_id;
  std::vector<uint8_t> image;

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    const H5HF_create_t cparam = MakeManHeapParams(/*max_man_size=*/128);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() {
      if (fh != nullptr) {
        H5HF_close(fh);
      }
    });
    ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);
    H5HF_hdr_t* hdr = fh->hdr;

    // Fill block 0 (40B), then place two objects (16B + 16B) in block 1 and
    // remove the first 16B object so block 1 has a persisted SINGLE free
    // section while block 1 stays alive in the indirect block.
    std::vector<uint8_t> id0(hdr->id_len, 0);
    std::vector<uint8_t> id1_a(hdr->id_len, 0);
    target_id.resize(hdr->id_len, 0);
    const std::vector<uint8_t> obj0 = MakePattern(40, 0x01U);
    const std::vector<uint8_t> obj1_a = MakePattern(16, 0x02U);
    const std::vector<uint8_t> obj1_b = MakePattern(16, 0x03U);
    ASSERT_GE(H5HF__man_insert(hdr, obj0.size(), obj0.data(), id0.data()), 0);
    ASSERT_GE(
        H5HF__man_insert(hdr, obj1_a.size(), obj1_a.data(), id1_a.data()), 0);
    ASSERT_GE(
        H5HF__man_insert(hdr, obj1_b.size(), obj1_b.data(), target_id.data()),
        0);
    ASSERT_GE(H5HF__man_remove(hdr, id1_a.data()), 0);

    // Flush first so block 1 is moved from temporary address space to its
    // permanent real file address, then record that address via dblock_locate.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    hsize_t target_off = 0;
    H5HF__man_get_obj_off(hdr, target_id.data(), &target_off);
    H5HF_indirect_t* iblock = nullptr;
    unsigned dblock_entry = 0;
    bool did_protect = false;
    ASSERT_GE(H5HF__man_dblock_locate(hdr, target_off, &iblock, &dblock_entry,
                                      &did_protect, H5AC__READ_ONLY_FLAG),
              0);
    target_dblock_addr = iblock->ents[dblock_entry].addr;
    ASSERT_GE(
        H5HF__man_iblock_unprotect(iblock, H5AC__NO_FLAGS_SET, did_protect), 0);
    ASSERT_NE(target_dblock_addr, HADDR_UNDEF);

    EXPECT_GE(H5HF_close(fh), 0);
    fh = nullptr;

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  // Corrupt the exact "FHDB" direct block header for block 1 so protecting it
  // fails checksum/magic decoding on reopen.
  ASSERT_LE(target_dblock_addr + 4, image.size());
  ASSERT_EQ(std::memcmp(image.data() + target_dblock_addr, "FHDB", 4), 0);
  image[target_dblock_addr] = 'X';

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("man_corrupt.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5F_t* f2 = GetFilePtr(reopened.get());
  ASSERT_NE(f2, nullptr);

  H5HF_t* fh2 = H5HF_open(f2, heap_addr);
  ASSERT_NE(fh2, nullptr);
  auto close_fh2 = absl::MakeCleanup([&]() { H5HF_close(fh2); });
  H5HF_hdr_t* hdr2 = fh2->hdr;

  ScopedErrorSilence silence;
  std::vector<uint8_t> out(16, 0);
  EXPECT_LT(H5HF__man_read(hdr2, target_id.data(), out.data()), 0);

  // Inserting 16B revives the free section in block 1, then fails
  // H5HF__man_dblock_protect and releases `sec_node` in `done:` cleanup.
  const std::vector<uint8_t> insert_obj = MakePattern(16, 0x99U);
  std::vector<uint8_t> new_id(hdr2->id_len, 0);
  EXPECT_LT(
      H5HF__man_insert(hdr2, insert_obj.size(), insert_obj.data(), new_id.data()),
      0);
}

TEST(H5HFmanTest, CorruptRootDblockIblockAndFspaceHeaderErrorPaths) {
  // 1. Root direct block (curr_root_rows == 0) with corrupt "FHDB" fails read.
  {
    haddr_t heap_addr = HADDR_UNDEF;
    haddr_t root_dblock_addr = HADDR_UNDEF;
    std::vector<uint8_t> obj_id;
    std::vector<uint8_t> image;

    {
      ScopedFile file = CreateInMemoryFile();
      ASSERT_TRUE(file.valid());
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = GetFilePtr(file.get());
      ASSERT_NE(f, nullptr);

      const H5HF_create_t cparam = MakeManHeapParams(/*max_man_size=*/128);
      H5HF_t* fh = H5HF_create(f, &cparam);
      ASSERT_NE(fh, nullptr);
      ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);
      H5HF_hdr_t* hdr = fh->hdr;

      obj_id.resize(hdr->id_len, 0);
      const std::vector<uint8_t> obj = MakePattern(20, 0x15U);
      ASSERT_GE(H5HF__man_insert(hdr, obj.size(), obj.data(), obj_id.data()),
                0);
      ASSERT_EQ(hdr->man_dtable.curr_root_rows, 0U);

      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
      root_dblock_addr = hdr->man_dtable.table_addr;
      ASSERT_NE(root_dblock_addr, HADDR_UNDEF);

      ASSERT_GE(H5HF_close(fh), 0);
      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
      const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
      ASSERT_GT(image_size, 0);
      image.resize(static_cast<size_t>(image_size));
      ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
                image_size);
    }

    ASSERT_LE(root_dblock_addr + 4, image.size());
    ASSERT_EQ(std::memcmp(image.data() + root_dblock_addr, "FHDB", 4), 0);
    image[root_dblock_addr] = 'X';

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedFile reopened(
        H5Fopen("man_corrupt_root_dblock.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(reopened.valid());

    ScopedApiContext ctx2;
    ASSERT_TRUE(ctx2.valid());
    H5F_t* f2 = GetFilePtr(reopened.get());
    ASSERT_NE(f2, nullptr);
    H5HF_t* fh2 = H5HF_open(f2, heap_addr);
    ASSERT_NE(fh2, nullptr);

    {
      ScopedErrorSilence silence;
      std::vector<uint8_t> out(20, 0);
      EXPECT_LT(H5HF__man_read(fh2->hdr, obj_id.data(), out.data()), 0);
    }
    EXPECT_GE(H5HF_close(fh2), 0);
    EXPECT_GE(H5Fclose(reopened.release()), 0);
  }

  // Build one multi-level heap image containing a persisted SINGLE free
  // section in block 1 (no ROW sections) so we can independently corrupt
  // root "FHIB" and heap "FSHD" in two clean copies of the image.
  haddr_t heap_addr = HADDR_UNDEF;
  haddr_t root_iblock_addr = HADDR_UNDEF;
  haddr_t fspace_addr = HADDR_UNDEF;
  std::vector<uint8_t> target_id;
  std::vector<uint8_t> base_image;
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    const H5HF_create_t cparam = MakeManHeapParams(/*max_man_size=*/128);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);
    H5HF_hdr_t* hdr = fh->hdr;

    std::vector<uint8_t> id0(hdr->id_len, 0);
    std::vector<uint8_t> id1_a(hdr->id_len, 0);
    target_id.resize(hdr->id_len, 0);
    const std::vector<uint8_t> obj0 = MakePattern(40, 0x21U);
    const std::vector<uint8_t> obj1_a = MakePattern(16, 0x22U);
    const std::vector<uint8_t> obj1_b = MakePattern(16, 0x23U);
    ASSERT_GE(H5HF__man_insert(hdr, obj0.size(), obj0.data(), id0.data()), 0);
    ASSERT_GE(
        H5HF__man_insert(hdr, obj1_a.size(), obj1_a.data(), id1_a.data()), 0);
    ASSERT_GE(
        H5HF__man_insert(hdr, obj1_b.size(), obj1_b.data(), target_id.data()),
        0);
    ASSERT_GE(H5HF__man_remove(hdr, id1_a.data()), 0);
    ASSERT_GT(hdr->man_dtable.curr_root_rows, 0U);

    // Close and flush first so both the root indirect block and the heap's
    // free-space manager header are serialized in real file space.
    ASSERT_GE(H5HF_close(fh), 0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    fh = H5HF_open(f, heap_addr);
    ASSERT_NE(fh, nullptr);
    root_iblock_addr = fh->hdr->man_dtable.table_addr;
    fspace_addr = fh->hdr->fs_addr;
    ASSERT_NE(root_iblock_addr, HADDR_UNDEF);
    ASSERT_NE(fspace_addr, HADDR_UNDEF);
    ASSERT_GE(H5HF_close(fh), 0);

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    base_image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), base_image.data(), base_image.size()),
              image_size);
  }

  // 2. Corrupt root "FHIB": fails dblock_locate in read, remove, and
  //    H5HF__sect_single_revive in insert cleanly.
  {
    std::vector<uint8_t> image = base_image;
    ASSERT_LE(root_iblock_addr + 4, image.size());
    ASSERT_EQ(std::memcmp(image.data() + root_iblock_addr, "FHIB", 4), 0);
    image[root_iblock_addr] = 'X';

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedFile reopened(
        H5Fopen("man_corrupt_root_iblock.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(reopened.valid());

    ScopedApiContext ctx2;
    ASSERT_TRUE(ctx2.valid());
    H5F_t* f2 = GetFilePtr(reopened.get());
    ASSERT_NE(f2, nullptr);
    H5HF_t* fh2 = H5HF_open(f2, heap_addr);
    ASSERT_NE(fh2, nullptr);
    H5HF_hdr_t* hdr2 = fh2->hdr;

    {
      ScopedErrorSilence silence;
      std::vector<uint8_t> out(16, 0);
      EXPECT_LT(H5HF__man_read(hdr2, target_id.data(), out.data()), 0);
      EXPECT_LT(H5HF__man_remove(hdr2, target_id.data()), 0);

      const std::vector<uint8_t> obj16 = MakePattern(16, 0x44U);
      std::vector<uint8_t> new_id(hdr2->id_len, 0);
      EXPECT_LT(
          H5HF__man_insert(hdr2, obj16.size(), obj16.data(), new_id.data()), 0);
    }
    EXPECT_GE(H5HF_close(fh2), 0);
    EXPECT_GE(H5Fclose(reopened.release()), 0);
  }

  // 3. Corrupt "FSHD" at hdr->fs_addr: fails H5HF__space_find in insert and
  //    H5HF__space_add in remove cleanly.
  {
    std::vector<uint8_t> image = base_image;
    ASSERT_LE(fspace_addr + 4, image.size());
    ASSERT_EQ(std::memcmp(image.data() + fspace_addr, "FSHD", 4), 0);
    image[fspace_addr] = 'X';

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedFile reopened(
        H5Fopen("man_corrupt_fspace_hdr.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(reopened.valid());

    ScopedApiContext ctx2;
    ASSERT_TRUE(ctx2.valid());
    H5F_t* f2 = GetFilePtr(reopened.get());
    ASSERT_NE(f2, nullptr);
    H5HF_t* fh2 = H5HF_open(f2, heap_addr);
    ASSERT_NE(fh2, nullptr);
    H5HF_hdr_t* hdr2 = fh2->hdr;

    {
      ScopedErrorSilence silence;
      const std::vector<uint8_t> obj16 = MakePattern(16, 0x55U);
      std::vector<uint8_t> new_id(hdr2->id_len, 0);
      EXPECT_LT(
          H5HF__man_insert(hdr2, obj16.size(), obj16.data(), new_id.data()), 0);
      EXPECT_LT(H5HF__man_remove(hdr2, target_id.data()), 0);
    }
    EXPECT_GE(H5HF_close(fh2), 0);
    EXPECT_GE(H5Fclose(reopened.release()), 0);
  }
}

}  // namespace
}  // namespace hdf5
