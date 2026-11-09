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
// Tests fractal heap v2 B-tree callbacks for "huge" objects across all four
// tracker classes (indirect, filtered indirect, direct, filtered direct):
// context lifecycle, store/compare/encode/decode serialization, found/remove
// callbacks, debug record formatting, and H5MF_xfree error propagation for
// third_party/hdf5/src/H5HFbtree2.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFbtree2.c

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from cache
// entries in internal headers. Match those struct layouts here.
#ifndef NDEBUG
#define H5HFBTREE2_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

// H5B2private.h uses `try` as a C parameter name, which collides with both the
// C++ keyword and Google3's `-fno-exceptions` `try` macro.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_param_try
extern "C" {
#define H5HF_FRIEND
#include "third_party/hdf5/src/H5B2private.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5HFpkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#include "third_party/hdf5/src/H5Zprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HFBTREE2_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HFBTREE2_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;
using ::testing::HasSubstr;

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

H5HF_create_t MakeHugeHeapParams(uint16_t id_len) {
  H5HF_create_t cparam{};
  cparam.managed.width = 4;
  cparam.managed.start_block_size = 512;
  cparam.managed.max_direct_size = 4096;
  cparam.managed.max_index = 20;
  cparam.managed.start_root_rows = 1;
  cparam.checksum_dblocks = true;
  cparam.max_man_size = 64;
  cparam.id_len = id_len;
  return cparam;
}

std::vector<uint8_t> MakeObjectPayload(size_t size, int index) {
  std::vector<uint8_t> buf(size);
  for (size_t i = 0; i < size; ++i) {
    // Low entropy on half the bytes ensures DEFLATE compresses cleanly while
    // keeping each object's content unique.
    buf[i] = static_cast<uint8_t>((i < size / 2) ? (index & 0xff)
                                                 : ((index * 13 + i) & 0xff));
  }
  return buf;
}

void VerifyTrackerClassRoundTrip(bool direct, bool filtered, size_t addr_size) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_sizes(fcpl.get(), addr_size, addr_size), 0);

  haddr_t heap_addr = HADDR_UNDEF;
  constexpr int kNumObjects = 36;
  constexpr size_t kObjSize = 128;
  std::vector<std::vector<uint8_t>> ids;
  std::vector<std::vector<uint8_t>> payloads;
  std::vector<uint8_t> image;

  {
    ScopedFile file =
        CreateInMemoryFile("bt2_huge.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    H5HF_create_t cparam = MakeHugeHeapParams(direct ? 1 : 0);
    auto reset_pline = absl::MakeCleanup([&]() {
      if (cparam.pline.nused > 0) {
        H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
      }
    });
    if (filtered) {
      unsigned level = 6;
      ASSERT_GE(H5Z_append(&cparam.pline, H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL,
                           1, &level),
                0);
    }

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

    // Inserting 36 huge objects overflows a single 512-byte v2 B-tree root node
    // and triggers node splitting, compare, store, and found lookups.
    for (int i = 0; i < kNumObjects; ++i) {
      payloads.push_back(MakeObjectPayload(kObjSize + (i % 5) * 16, i));
      std::vector<uint8_t> id(id_len, 0);
      ASSERT_GE(H5HF_insert(fh, payloads.back().size(), payloads.back().data(),
                            id.data()),
                0);
      ids.push_back(std::move(id));
    }

    // Remove a few objects before flush to exercise v2 B-tree remove callbacks.
    for (int idx : {0, 12, 25}) {
      ASSERT_GE(H5HF_remove(fh, ids[idx].data()), 0);
    }

    EXPECT_GE(H5HF_close(fh), 0);
    fh = nullptr;

    // Flushing and reading the file image triggers v2 B-tree node encode.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  // Reopen the image to trigger decode callbacks on the persisted v2 B-tree.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("bt2_huge_reopen.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5F_t* f2 = GetFilePtr(reopened.get());
  ASSERT_NE(f2, nullptr);

  H5HF_t* reopened_fh = H5HF_open(f2, heap_addr);
  ASSERT_NE(reopened_fh, nullptr);
  auto close_reopened_fh = absl::MakeCleanup([&]() {
    if (reopened_fh != nullptr) {
      H5HF_close(reopened_fh);
    }
  });

  for (int i = 0; i < kNumObjects; ++i) {
    if (i == 0 || i == 12 || i == 25) {
      continue;
    }
    size_t got_len = 0;
    EXPECT_GE(H5HF_get_obj_len(reopened_fh, ids[i].data(), &got_len), 0);
    EXPECT_EQ(got_len, payloads[i].size());

    std::vector<uint8_t> read_buf(got_len, 0xffU);
    EXPECT_GE(H5HF_read(reopened_fh, ids[i].data(), read_buf.data()), 0);
    EXPECT_THAT(read_buf, ElementsAreArray(payloads[i]));
  }

  // Remove a couple more after reopen, then delete the whole heap to drive
  // H5B2_delete through the tracker class remove callback.
  ASSERT_GE(H5HF_remove(reopened_fh, ids[5].data()), 0);
  ASSERT_GE(H5HF_remove(reopened_fh, ids[30].data()), 0);

  EXPECT_GE(H5HF_close(reopened_fh), 0);
  reopened_fh = nullptr;

  EXPECT_GE(H5HF_delete(f2, heap_addr), 0);
}

TEST(H5HFbtree2Test, IndirectTrackerRoundTrip8ByteAnd4Byte) {
  VerifyTrackerClassRoundTrip(/*direct=*/false, /*filtered=*/false, 8);
  VerifyTrackerClassRoundTrip(/*direct=*/false, /*filtered=*/false, 4);
}

TEST(H5HFbtree2Test, FilteredIndirectTrackerRoundTrip8ByteAnd4Byte) {
  VerifyTrackerClassRoundTrip(/*direct=*/false, /*filtered=*/true, 8);
  VerifyTrackerClassRoundTrip(/*direct=*/false, /*filtered=*/true, 4);
}

TEST(H5HFbtree2Test, DirectTrackerRoundTrip8ByteAnd4Byte) {
  VerifyTrackerClassRoundTrip(/*direct=*/true, /*filtered=*/false, 8);
  VerifyTrackerClassRoundTrip(/*direct=*/true, /*filtered=*/false, 4);
}

TEST(H5HFbtree2Test, FilteredDirectTrackerRoundTrip8ByteAnd4Byte) {
  VerifyTrackerClassRoundTrip(/*direct=*/true, /*filtered=*/true, 8);
  VerifyTrackerClassRoundTrip(/*direct=*/true, /*filtered=*/true, 4);
}

TEST(H5HFbtree2Test, DirectCompareBranchesAndFiltDirFoundCallback) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  // 1. H5HF_HUGE_BT2_DIR: insert records sharing the same addr but different
  //    lengths so H5B2_insert/find exercise addr <, addr >, len <, len >, ==.
  {
    H5B2_create_t bt2_cparam{};
    bt2_cparam.cls = H5HF_HUGE_BT2_DIR;
    bt2_cparam.node_size = 512;
    bt2_cparam.rrec_size =
        static_cast<uint32_t>(H5F_SIZEOF_ADDR(f) + H5F_SIZEOF_SIZE(f));
    bt2_cparam.split_percent = 100;
    bt2_cparam.merge_percent = 40;

    H5B2_t* bt2 = H5B2_create(f, &bt2_cparam, f);
    ASSERT_NE(bt2, nullptr);
    haddr_t bt2_addr = HADDR_UNDEF;
    ASSERT_GE(H5B2_get_addr(bt2, &bt2_addr), 0);

    std::vector<H5HF_huge_bt2_dir_rec_t> recs = {
        {.addr = 0x2000, .len = 200}, {.addr = 0x2000, .len = 100},
        {.addr = 0x2000, .len = 300}, {.addr = 0x1000, .len = 150},
        {.addr = 0x3000, .len = 250},
    };
    for (auto& rec : recs) {
      ASSERT_GE(H5B2_insert(bt2, &rec), 0);
    }

    // Lookup hits == and misses with smaller/larger lengths at equal addr.
    auto ok_found = [](const void* nrec, void* op_data) -> herr_t {
      *static_cast<H5HF_huge_bt2_dir_rec_t*>(op_data) =
          *static_cast<const H5HF_huge_bt2_dir_rec_t*>(nrec);
      return 0;
    };
    bool found = false;
    H5HF_huge_bt2_dir_rec_t found_rec{};
    H5HF_huge_bt2_dir_rec_t query{.addr = 0x2000, .len = 200};
    ASSERT_GE(H5B2_find(bt2, &query, &found, ok_found, &found_rec), 0);
    EXPECT_TRUE(found);
    EXPECT_EQ(found_rec.addr, 0x2000U);
    EXPECT_EQ(found_rec.len, 200U);

    query = {.addr = 0x2000, .len = 150};
    ASSERT_GE(H5B2_find(bt2, &query, &found, ok_found, &found_rec), 0);
    EXPECT_FALSE(found);

    ASSERT_GE(H5B2_close(bt2), 0);
    ASSERT_GE(H5B2_delete(f, bt2_addr, f, nullptr, nullptr), 0);
  }

  // 2. H5HF_HUGE_BT2_FILT_DIR: exercise compare branches plus the
  //    H5HF__huge_bt2_filt_dir_found callback via H5B2_find.
  {
    H5B2_create_t bt2_cparam{};
    bt2_cparam.cls = H5HF_HUGE_BT2_FILT_DIR;
    bt2_cparam.node_size = 512;
    bt2_cparam.rrec_size = static_cast<uint32_t>(
        H5F_SIZEOF_ADDR(f) + H5F_SIZEOF_SIZE(f) + 4 + H5F_SIZEOF_SIZE(f));
    bt2_cparam.split_percent = 100;
    bt2_cparam.merge_percent = 40;

    H5B2_t* bt2 = H5B2_create(f, &bt2_cparam, f);
    ASSERT_NE(bt2, nullptr);
    haddr_t bt2_addr = HADDR_UNDEF;
    ASSERT_GE(H5B2_get_addr(bt2, &bt2_addr), 0);

    std::vector<H5HF_huge_bt2_filt_dir_rec_t> recs = {
        {.addr = 0x4000, .len = 200, .filter_mask = 0x1, .obj_size = 400},
        {.addr = 0x4000, .len = 100, .filter_mask = 0x2, .obj_size = 200},
        {.addr = 0x4000, .len = 300, .filter_mask = 0x0, .obj_size = 600},
        {.addr = 0x1000, .len = 120, .filter_mask = 0x3, .obj_size = 240},
        {.addr = 0x5000, .len = 240, .filter_mask = 0x0, .obj_size = 480},
    };
    for (auto& rec : recs) {
      ASSERT_GE(H5B2_insert(bt2, &rec), 0);
    }

    bool found = false;
    H5HF_huge_bt2_filt_dir_rec_t found_rec{};
    H5HF_huge_bt2_filt_dir_rec_t query{.addr = 0x4000, .len = 300};
    ASSERT_GE(H5B2_find(bt2, &query, &found, H5HF__huge_bt2_filt_dir_found,
                        &found_rec),
              0);
    EXPECT_TRUE(found);
    EXPECT_EQ(found_rec.addr, 0x4000U);
    EXPECT_EQ(found_rec.len, 300U);
    EXPECT_EQ(found_rec.obj_size, 600U);

    query = {.addr = 0x4000, .len = 250};
    ASSERT_GE(H5B2_find(bt2, &query, &found, H5HF__huge_bt2_filt_dir_found,
                        &found_rec),
              0);
    EXPECT_FALSE(found);

    ASSERT_GE(H5B2_close(bt2), 0);
    ASSERT_GE(H5B2_delete(f, bt2_addr, f, nullptr, nullptr), 0);
  }
}

TEST(H5HFbtree2Test, DebugFormattingAndRemoveXfreeErrorAllClasses) {
  // 1. Exercise record debug printers for all four huge v2 B-tree classes.
  {
    H5HF_huge_bt2_indir_rec_t indir_rec{.addr = 0x1111, .len = 222, .id = 3};
    H5HF_huge_bt2_filt_indir_rec_t filt_indir_rec{.addr = 0x2222,
                                                  .len = 333,
                                                  .filter_mask = 0x5a,
                                                  .obj_size = 444,
                                                  .id = 7};
    H5HF_huge_bt2_dir_rec_t dir_rec{.addr = 0x3333, .len = 555};
    H5HF_huge_bt2_filt_dir_rec_t filt_dir_rec{
        .addr = 0x4444, .len = 666, .filter_mask = 0x3c, .obj_size = 777};

    char* buf = nullptr;
    size_t size = 0;
    FILE* memfp = open_memstream(&buf, &size);
    ASSERT_NE(memfp, nullptr);
    auto close_stream = absl::MakeCleanup([&]() {
      if (memfp != nullptr) {
        std::fclose(memfp);
      }
      std::free(buf);
    });

    ASSERT_GE(H5HF_HUGE_BT2_INDIR[0].debug(memfp, 2, 10, &indir_rec, nullptr),
              0);
    ASSERT_GE(H5HF_HUGE_BT2_FILT_INDIR[0].debug(memfp, 2, 10, &filt_indir_rec,
                                                nullptr),
              0);
    ASSERT_GE(H5HF_HUGE_BT2_DIR[0].debug(memfp, 2, 10, &dir_rec, nullptr), 0);
    ASSERT_GE(
        H5HF_HUGE_BT2_FILT_DIR[0].debug(memfp, 2, 10, &filt_dir_rec, nullptr),
        0);
    std::fclose(memfp);
    memfp = nullptr;

    const std::string output(buf, size);
    EXPECT_THAT(output, HasSubstr("Record:"));
    EXPECT_THAT(output, HasSubstr("222, 3}"));
    EXPECT_THAT(output, HasSubstr("5a, 444, 7}"));
    EXPECT_THAT(output, HasSubstr("555}"));
    EXPECT_THAT(output, HasSubstr("3c, 777}"));
  }

  // 2. Passing an address >= f->shared->tmp_addr (HADDR_MAX) causes
  //    H5MF_xfree to fail in every *_remove callback.
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  const H5HF_create_t cparam = MakeHugeHeapParams(/*id_len=*/0);
  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

  H5HF_huge_remove_ud_t udata{};
  udata.hdr = fh->hdr;
  udata.hdr->f = f;

  ScopedErrorSilence silence;
  const H5HF_huge_bt2_indir_rec_t bad_indir{
      .addr = HADDR_MAX, .len = 64, .id = 1};
  EXPECT_LT(H5HF__huge_bt2_indir_remove(&bad_indir, &udata), 0);

  const H5HF_huge_bt2_filt_indir_rec_t bad_filt_indir{
      .addr = HADDR_MAX, .len = 64, .filter_mask = 0, .obj_size = 128, .id = 1};
  EXPECT_LT(H5HF__huge_bt2_filt_indir_remove(&bad_filt_indir, &udata), 0);

  const H5HF_huge_bt2_dir_rec_t bad_dir{.addr = HADDR_MAX, .len = 64};
  EXPECT_LT(H5HF__huge_bt2_dir_remove(&bad_dir, &udata), 0);

  const H5HF_huge_bt2_filt_dir_rec_t bad_filt_dir{
      .addr = HADDR_MAX, .len = 64, .filter_mask = 0, .obj_size = 128};
  EXPECT_LT(H5HF__huge_bt2_filt_dir_remove(&bad_filt_dir, &udata), 0);
}

}  // namespace
}  // namespace hdf5
