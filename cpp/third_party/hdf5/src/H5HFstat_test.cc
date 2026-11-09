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
// Tests fractal heap statistics and storage-size accounting across empty,
// tiny, direct/indirect managed (with free space), and huge object heaps,
// dense attribute public API usage, and error paths for
// third_party/hdf5/src/H5HFstat.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFstat.c

#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t) inside internal fractal heap/B-tree structs.
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
#define H5B2_FRIEND
#include "third_party/hdf5/src/H5B2pkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
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

H5HF_create_t MakeHeapCreateParams(uint16_t id_len = 0,
                                   uint32_t max_man_size = 200,
                                   unsigned width = 2,
                                   size_t start_block_size = 64,
                                   size_t max_direct_size = 256,
                                   unsigned max_index = 14) {
  H5HF_create_t cparam{};
  cparam.managed.width = width;
  cparam.managed.start_block_size = start_block_size;
  cparam.managed.max_direct_size = max_direct_size;
  cparam.managed.max_index = max_index;
  cparam.managed.start_root_rows = 1;
  cparam.checksum_dblocks = true;
  cparam.max_man_size = max_man_size;
  cparam.id_len = id_len;
  return cparam;
}

TEST(H5HFstatTest, StatInfoAndSizeTrackEmptyTinyManagedFreeSpaceIndirectAndHuge) {
  haddr_t heap_addr = HADDR_UNDEF;
  hsize_t expected_total_before_reopen = 0;
  std::vector<uint8_t> image;

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    const H5HF_create_t cparam = MakeHeapCreateParams(/*id_len=*/0);
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

    // 1. Empty heap: only header size is counted; H5HF_size adds onto caller's
    //    initial *heap_size value.
    H5HF_stat_t stats{};
    EXPECT_GE(H5HF_stat_info(fh, &stats), 0);
    EXPECT_EQ(stats.man_size, 0U);
    EXPECT_EQ(stats.man_alloc_size, 0U);
    EXPECT_EQ(stats.man_iter_off, 0U);
    EXPECT_EQ(stats.man_nobjs, 0U);
    EXPECT_EQ(stats.man_free_space, 0U);
    EXPECT_EQ(stats.huge_size, 0U);
    EXPECT_EQ(stats.huge_nobjs, 0U);
    EXPECT_EQ(stats.tiny_size, 0U);
    EXPECT_EQ(stats.tiny_nobjs, 0U);

    hsize_t empty_size = 100;
    EXPECT_GE(H5HF_size(fh, &empty_size), 0);
    EXPECT_EQ(empty_size, 100U + fh->hdr->heap_size);

    // 2. Tiny objects increase tiny_nobjs and tiny_size only. With auto id_len=4
    //    and tiny_max_len=3, 3 bytes is stored as tiny while >=4 bytes is managed.
    const std::vector<uint8_t> tiny_obj(3, 0x11U);
    std::vector<uint8_t> id_tiny(id_len);
    ASSERT_GE(
        H5HF_insert(fh, tiny_obj.size(), tiny_obj.data(), id_tiny.data()), 0);
    EXPECT_GE(H5HF_stat_info(fh, &stats), 0);
    EXPECT_EQ(stats.tiny_nobjs, 1U);
    EXPECT_EQ(stats.tiny_size, 3U);

    // 3. Managed objects promote root direct block to indirect block(s), then
    //    removing one creates free space tracking (fs_addr defined).
    std::vector<std::vector<uint8_t>> man_ids;
    for (int i = 0; i < 6; ++i) {
      std::vector<uint8_t> man_obj(32, static_cast<uint8_t>(i + 1));
      std::vector<uint8_t> id(id_len);
      ASSERT_GE(H5HF_insert(fh, man_obj.size(), man_obj.data(), id.data()), 0);
      man_ids.push_back(id);
    }
    EXPECT_GE(H5HF_remove(fh, man_ids[3].data()), 0);

    // 4. Huge objects create huge v2 B-tree storage (huge_bt2_addr defined).
    const std::vector<uint8_t> huge_obj(300, 0x55U);
    std::vector<uint8_t> id_huge1(id_len);
    std::vector<uint8_t> id_huge2(id_len);
    ASSERT_GE(
        H5HF_insert(fh, huge_obj.size(), huge_obj.data(), id_huge1.data()), 0);
    ASSERT_GE(
        H5HF_insert(fh, huge_obj.size(), huge_obj.data(), id_huge2.data()), 0);

    EXPECT_GE(H5HF_stat_info(fh, &stats), 0);
    EXPECT_EQ(stats.man_nobjs, 5U);
    EXPECT_EQ(stats.man_size, fh->hdr->man_size);
    EXPECT_GE(stats.man_size, stats.man_alloc_size);
    EXPECT_GE(stats.man_alloc_size, 128U);
    EXPECT_GT(stats.man_iter_off, 64U);
    EXPECT_GT(stats.man_free_space, 0U);
    EXPECT_EQ(stats.huge_nobjs, 2U);
    EXPECT_EQ(stats.huge_size, 600U);
    EXPECT_EQ(stats.tiny_nobjs, 1U);
    EXPECT_EQ(stats.tiny_size, 3U);

    hsize_t total_size = 0;
    EXPECT_GE(H5HF_size(fh, &total_size), 0);
    EXPECT_GT(total_size,
              fh->hdr->heap_size + stats.man_alloc_size + stats.huge_size);
    expected_total_before_reopen = total_size;

    EXPECT_GE(H5HF_close(fh), 0);
    fh = nullptr;

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  // 5. Reopen from file image and confirm H5HF_size recovers exact persisted
  //    storage across header, indirect blocks, B-tree, and free space.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("reopen_stat.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5F_t* f2 = GetFilePtr(reopened.get());
  ASSERT_NE(f2, nullptr);

  H5HF_t* fh2 = H5HF_open(f2, heap_addr);
  ASSERT_NE(fh2, nullptr);
  auto close_fh2 = absl::MakeCleanup([&]() { H5HF_close(fh2); });

  hsize_t reopened_size = 0;
  EXPECT_GE(H5HF_size(fh2, &reopened_size), 0);
  EXPECT_EQ(reopened_size, expected_total_before_reopen);
}

TEST(H5HFstatTest, PublicNativeInfoReportsDenseAttributeFractalHeapSize) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ScopedFile file =
      CreateInMemoryFile("dense_attr.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  // Force dense attribute storage (fractal heap + v2 B-tree) from 1st attr.
  ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 0, 0), 0);

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/dense", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  const hsize_t dim = 16;
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid());
  const std::vector<int32_t> vals(16, 7);

  for (int i = 0; i < 4; ++i) {
    const std::string name = "attr_" + std::to_string(i);
    ScopedAttr attr(H5Acreate2(grp.get(), name.c_str(), H5T_NATIVE_INT32,
                               space.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, vals.data()), 0);
  }
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  H5O_native_info_t ninfo{};
  ASSERT_GE(H5Oget_native_info(grp.get(), &ninfo, H5O_NATIVE_INFO_META_SIZE),
            0);
  EXPECT_GT(ninfo.meta_size.attr.heap_size, 0U);
  EXPECT_GT(ninfo.meta_size.attr.index_size, 0U);
}

TEST(H5HFstatTest, SizeReportsErrorsOnInvalidIndirectBtreeAndFreeSpaceAddrs) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  const H5HF_create_t cparam = MakeHeapCreateParams(/*id_len=*/0);
  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);

  // Create indirect managed blocks + free space + multi-record huge B-tree.
  std::vector<std::vector<uint8_t>> man_ids;
  for (int i = 0; i < 5; ++i) {
    std::vector<uint8_t> man_obj(32, static_cast<uint8_t>(i));
    std::vector<uint8_t> id(id_len);
    ASSERT_GE(H5HF_insert(fh, man_obj.size(), man_obj.data(), id.data()), 0);
    man_ids.push_back(id);
  }
  ASSERT_GE(H5HF_remove(fh, man_ids[3].data()), 0);

  const std::vector<uint8_t> huge_obj(300, 0x77U);
  std::vector<uint8_t> huge_id(id_len);
  ASSERT_GE(H5HF_insert(fh, huge_obj.size(), huge_obj.data(), huge_id.data()),
            0);

  // Flush so invalid unaligned address inside the file fails cache decode.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  const haddr_t kBadAddr = fh->hdr->heap_addr + 1;
  H5HF_hdr_t* hdr = fh->hdr;
  ASSERT_GT(hdr->man_dtable.curr_root_rows, 0U);
  ASSERT_NE(hdr->huge_bt2_addr, HADDR_UNDEF);
  ASSERT_NE(hdr->fs_addr, HADDR_UNDEF);

  ScopedErrorSilence silence;
  hsize_t heap_size = 0;

  // 1. Failing indirect block protect in H5HF__man_iblock_size (temporarily
  //    clear pinned root_iblock fast-path so H5HF__man_iblock_protect loads
  //    table_addr from cache).
  const haddr_t orig_table_addr = hdr->man_dtable.table_addr;
  const unsigned orig_root_flags = hdr->root_iblock_flags;
  H5HF_indirect_t* orig_root_iblock = hdr->root_iblock;
  hdr->man_dtable.table_addr = kBadAddr;
  hdr->root_iblock_flags = 0;
  hdr->root_iblock = nullptr;
  EXPECT_LT(H5HF_size(fh, &heap_size), 0);
  hdr->man_dtable.table_addr = orig_table_addr;
  hdr->root_iblock_flags = orig_root_flags;
  hdr->root_iblock = orig_root_iblock;

  // 2. Failing H5B2_open on invalid huge_bt2_addr.
  const haddr_t orig_bt2_addr = hdr->huge_bt2_addr;
  hdr->huge_bt2_addr = kBadAddr;
  EXPECT_LT(H5HF_size(fh, &heap_size), 0);
  hdr->huge_bt2_addr = orig_bt2_addr;

  // 3. Failing H5B2_size when depth > 0 points to an invalid internal node.
  H5B2_t* bt2 = H5B2_open(f, hdr->huge_bt2_addr, f);
  ASSERT_NE(bt2, nullptr);
  const uint16_t orig_depth = bt2->hdr->depth;
  const haddr_t orig_root_addr = bt2->hdr->root.addr;
  bt2->hdr->depth = 1;
  bt2->hdr->root.addr = kBadAddr;
  EXPECT_LT(H5HF_size(fh, &heap_size), 0);
  bt2->hdr->depth = orig_depth;
  bt2->hdr->root.addr = orig_root_addr;
  EXPECT_GE(H5B2_close(bt2), 0);

  // 4. Failing H5HF__space_size when fspace is closed and fs_addr is invalid.
  ASSERT_GE(H5HF__space_close(hdr), 0);
  const haddr_t orig_fs_addr = hdr->fs_addr;
  ASSERT_NE(orig_fs_addr, HADDR_UNDEF);
  hdr->fs_addr = kBadAddr;
  EXPECT_LT(H5HF_size(fh, &heap_size), 0);
  hdr->fs_addr = orig_fs_addr;
}

}  // namespace
}  // namespace hdf5
