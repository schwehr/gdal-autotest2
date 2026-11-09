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
// Tests v2 B-tree metadata statistics and storage size calculation across
// empty, single-leaf, multi-level, failure, and public dense-group callers
// (third_party/hdf5/src/H5B2stat.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5B2stat.c

#include <cstdint>
#include <string>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with `-DNDEBUG`, which omits debug/stats fields from
// `H5C_cache_entry_t` (`H5AC_info_t`), the first member of `H5B2_hdr_t`. Match
// `NDEBUG` around the internal prelude and headers to preserve struct offsets.
#ifndef NDEBUG
#define H5B2STAT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma push_macro("try")
#undef try
#define try h5_try_param
extern "C" {
#define H5B2_FRIEND
#define H5B2_TESTING
#include "third_party/hdf5/src/H5B2pkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#ifdef H5B2STAT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5B2STAT_TEST_RESTORE_UNDEF_NDEBUG
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

class ScopedBtree2 {
 public:
  ScopedBtree2() = default;
  explicit ScopedBtree2(H5B2_t* bt2) : bt2_(bt2) {}
  ~ScopedBtree2() { reset(); }

  ScopedBtree2(const ScopedBtree2&) = delete;
  ScopedBtree2& operator=(const ScopedBtree2&) = delete;

  void reset(H5B2_t* next = nullptr) {
    if (bt2_ != nullptr) {
      H5B2_close(bt2_);
    }
    bt2_ = next;
  }

  H5B2_t* get() const { return bt2_; }

 private:
  H5B2_t* bt2_ = nullptr;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5B2_create_t MakeTestCreateParams(H5F_t* f, uint32_t node_size = 128) {
  H5B2_create_t cparam{};
  cparam.cls = H5B2_TEST;
  cparam.node_size = node_size;
  cparam.rrec_size = H5F_SIZEOF_SIZE(f);
  cparam.split_percent = 100;
  cparam.merge_percent = 40;
  return cparam;
}

TEST(H5B2StatTest, StatInfoAndSizeAcrossTreeDepths) {
  ScopedFile file = CreateInMemoryFile("h5b2stat_depths.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5B2_create_t cparam = MakeTestCreateParams(f, /*node_size=*/128);
  ScopedBtree2 bt2(H5B2_create(f, &cparam, f));
  ASSERT_NE(bt2.get(), nullptr);

  // Empty tree: depth = 0, nrecords = 0, size adds only header size.
  H5B2_stat_t stat{};
  EXPECT_GE(H5B2_stat_info(bt2.get(), &stat), 0);
  EXPECT_EQ(stat.depth, 0U);
  EXPECT_EQ(stat.nrecords, 0ULL);

  const hsize_t hdr_size = bt2.get()->hdr->hdr_size;
  const hsize_t node_size = bt2.get()->hdr->node_size;

  hsize_t size = 0;
  EXPECT_GE(H5B2_size(bt2.get(), &size), 0);
  EXPECT_EQ(size, hdr_size);

  // Verify H5B2_size accumulates into an existing non-zero total.
  hsize_t accumulated = 100;
  EXPECT_GE(H5B2_size(bt2.get(), &accumulated), 0);
  EXPECT_EQ(accumulated, 100 + hdr_size);

  // Single-leaf tree (depth == 0, nrecords > 0): adds header + 1 leaf node.
  for (hsize_t k = 1; k <= 5; ++k) {
    ASSERT_GE(H5B2_insert(bt2.get(), &k), 0);
  }
  EXPECT_GE(H5B2_stat_info(bt2.get(), &stat), 0);
  EXPECT_EQ(stat.depth, 0U);
  EXPECT_EQ(stat.nrecords, 5ULL);

  hsize_t leaf_tree_size = 0;
  EXPECT_GE(H5B2_size(bt2.get(), &leaf_tree_size), 0);
  EXPECT_EQ(leaf_tree_size, hdr_size + node_size);

  // Depth-1 tree: insert past leaf capacity so root becomes an internal node.
  for (hsize_t k = 6; k <= 30; ++k) {
    ASSERT_GE(H5B2_insert(bt2.get(), &k), 0);
  }
  EXPECT_GE(H5B2_stat_info(bt2.get(), &stat), 0);
  EXPECT_EQ(stat.depth, 1U);
  EXPECT_EQ(stat.nrecords, 30ULL);

  hsize_t depth1_size = 0;
  EXPECT_GE(H5B2_size(bt2.get(), &depth1_size), 0);
  EXPECT_GT(depth1_size, leaf_tree_size);
  EXPECT_EQ((depth1_size - hdr_size) % node_size, 0ULL);

  // Depth-2 tree: insert more records to trigger H5B2__node_size recursion.
  for (hsize_t k = 31; k <= 140; ++k) {
    ASSERT_GE(H5B2_insert(bt2.get(), &k), 0);
  }
  EXPECT_GE(H5B2_stat_info(bt2.get(), &stat), 0);
  EXPECT_GE(stat.depth, 2U);
  EXPECT_EQ(stat.nrecords, 140ULL);

  hsize_t depth2_size = 0;
  EXPECT_GE(H5B2_size(bt2.get(), &depth2_size), 0);
  EXPECT_GT(depth2_size, depth1_size);
  EXPECT_EQ((depth2_size - hdr_size) % node_size, 0ULL);
}

TEST(H5B2StatTest, SizeReturnsFailWhenInternalNodeLookupFails) {
  ScopedFile file = CreateInMemoryFile("h5b2stat_fail.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5B2_create_t cparam = MakeTestCreateParams(f, /*node_size=*/128);
  ScopedBtree2 bt2(H5B2_create(f, &cparam, f));
  ASSERT_NE(bt2.get(), nullptr);

  for (hsize_t k = 1; k <= 30; ++k) {
    ASSERT_GE(H5B2_insert(bt2.get(), &k), 0);
  }
  H5B2_stat_t stat{};
  ASSERT_GE(H5B2_stat_info(bt2.get(), &stat), 0);
  ASSERT_GE(stat.depth, 1U);

  // Point the root internal node temporarily at an unmapped file address so
  // H5B2__node_size() fails to protect it, while keeping clean teardown.
  const haddr_t saved_root_addr = bt2.get()->hdr->root.addr;
  bt2.get()->hdr->root.addr = 0x7fffffff0000ULL;
  absl::Cleanup restore_addr = [&]() {
    bt2.get()->hdr->root.addr = saved_root_addr;
  };

  ScopedErrorSilence silence;
  hsize_t btree_size = 0;
  EXPECT_LT(H5B2_size(bt2.get(), &btree_size), 0);
}

TEST(H5B2StatTest, PublicDenseGroupAndChunkedDatasetExerciseBtree2Size) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);

  ScopedFile file(
      H5Fcreate("h5b2stat_public.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  // Dense group link storage uses v2 B-trees for name and creation-order index.
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_phase_change(gcpl.get(), 0, 0), 0);

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/dense", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  for (int i = 0; i < 16; ++i) {
    const std::string link_name = "item_" + std::to_string(i);
    ASSERT_GE(H5Lcreate_soft("/dense", grp.get(), link_name.c_str(),
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
  H5G_info_t ginfo{};
  ASSERT_GE(H5Gget_info(grp.get(), &ginfo), 0);
  EXPECT_EQ(ginfo.storage_type, H5G_STORAGE_TYPE_DENSE);
  EXPECT_EQ(ginfo.nlinks, 16);

  // 2D unlimited chunked dataset uses a v2 B-tree chunk index.
  const hsize_t init_dims[2] = {2, 2};
  const hsize_t max_dims[2] = {H5S_UNLIMITED, H5S_UNLIMITED};
  const hsize_t chunk_dims[2] = {1, 1};
  ScopedSpace space(H5Screate_simple(2, init_dims, max_dims));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, chunk_dims), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "/chunked", H5T_NATIVE_INT,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  const int data[4] = {10, 20, 30, 40};
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     data),
            0);
  EXPECT_GT(H5Dget_storage_size(dset.get()), 0U);
}

}  // namespace
}  // namespace hdf5
