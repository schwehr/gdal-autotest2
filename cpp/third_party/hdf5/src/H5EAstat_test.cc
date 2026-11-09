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
// Tests extensible array metadata statistics retrieval across empty, index-block,
// data-block, super-block, paged, reopened, and 1D chunked dataset callers
// (third_party/hdf5/src/H5EAstat.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5EAstat.c

#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields from H5AC_info_t
// (first field of H5EA_hdr_t). Define NDEBUG around internal headers so C++
// struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5EASTAT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5EA_FRIEND
#define H5EA_TESTING
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5EApkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5EASTAT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5EASTAT_TEST_RESTORE_UNDEF_NDEBUG
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

class ScopedEa {
 public:
  ScopedEa() = default;
  explicit ScopedEa(H5EA_t* ea) : ea_(ea) {}
  ~ScopedEa() { reset(); }

  ScopedEa(const ScopedEa&) = delete;
  ScopedEa& operator=(const ScopedEa&) = delete;

  void reset(H5EA_t* next = nullptr) {
    if (ea_ != nullptr) {
      H5EA_close(ea_);
    }
    ea_ = next;
  }

  H5EA_t* get() const { return ea_; }

 private:
  H5EA_t* ea_ = nullptr;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5EA_create_t MakeTestCreateParams() {
  H5EA_create_t cparam{};
  cparam.cls = H5EA_CLS_TEST;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_nelmts_bits = 10;
  cparam.idx_blk_elmts = 4;
  cparam.data_blk_min_elmts = 4;
  cparam.sup_blk_min_data_ptrs = 4;
  cparam.max_dblk_page_nelmts_bits = 4;  // 16 elements per page.
  return cparam;
}

TEST(H5EAstatTest, EmptyPopulatedPagedAndReopenedArrayStatsMatchLayout) {
  ScopedFile file = CreateInMemoryFile("h5eastat_layout.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5EA_create_t cparam = MakeTestCreateParams();
  ScopedEa ea(H5EA_create(f, &cparam, nullptr));
  ASSERT_NE(ea.get(), nullptr);

  // 1. Empty array: only header size is populated; no blocks allocated yet.
  H5EA_stat_t stats{};
  EXPECT_GE(H5EA_get_stats(ea.get(), &stats), 0);
  EXPECT_EQ(stats.computed.hdr_size, H5EA_HEADER_SIZE_HDR(ea.get()->hdr));
  EXPECT_EQ(stats.computed.nindex_blks, 0u);
  EXPECT_EQ(stats.computed.index_blk_size, 0u);
  EXPECT_EQ(stats.stored.nsuper_blks, 0u);
  EXPECT_EQ(stats.stored.super_blk_size, 0u);
  EXPECT_EQ(stats.stored.ndata_blks, 0u);
  EXPECT_EQ(stats.stored.data_blk_size, 0u);
  EXPECT_EQ(stats.stored.max_idx_set, 0u);
  EXPECT_EQ(stats.stored.nelmts, 0u);

  // 2. Setting index 2 allocates only the index block.
  uint64_t val = 1002;
  ASSERT_GE(H5EA_set(ea.get(), 2, &val), 0);
  EXPECT_GE(H5EA_get_stats(ea.get(), &stats), 0);
  EXPECT_EQ(stats.computed.nindex_blks, 1u);
  EXPECT_GT(stats.computed.index_blk_size, 0u);
  EXPECT_EQ(stats.stored.nsuper_blks, 0u);
  EXPECT_EQ(stats.stored.ndata_blks, 0u);
  EXPECT_EQ(stats.stored.max_idx_set, 3u);
  EXPECT_EQ(stats.stored.nelmts, 4u);

  // 3. Index 4 enters sblk 0 (data block pointed to directly from index block).
  val = 1004;
  ASSERT_GE(H5EA_set(ea.get(), 4, &val), 0);
  EXPECT_GE(H5EA_get_stats(ea.get(), &stats), 0);
  EXPECT_EQ(stats.stored.nsuper_blks, 0u);
  EXPECT_EQ(stats.stored.ndata_blks, 1u);
  EXPECT_GT(stats.stored.data_blk_size, 0u);
  EXPECT_EQ(stats.stored.max_idx_set, 5u);
  EXPECT_EQ(stats.stored.nelmts, 8u);

  // 4. Index 64 enters sblk 4 (first external super block, unpaged 16-elmt dblk).
  val = 1064;
  ASSERT_GE(H5EA_set(ea.get(), 64, &val), 0);
  EXPECT_GE(H5EA_get_stats(ea.get(), &stats), 0);
  EXPECT_EQ(stats.stored.nsuper_blks, 1u);
  EXPECT_GT(stats.stored.super_blk_size, 0u);
  EXPECT_EQ(stats.stored.ndata_blks, 2u);
  EXPECT_EQ(stats.stored.max_idx_set, 65u);
  EXPECT_EQ(stats.stored.nelmts, 24u);

  // 5. Index 128 enters sblk 5 (paged 32-element dblk with 2 pages of 16).
  val = 1128;
  ASSERT_GE(H5EA_set(ea.get(), 128, &val), 0);
  EXPECT_GE(H5EA_get_stats(ea.get(), &stats), 0);
  EXPECT_EQ(stats.stored.nsuper_blks, 2u);
  EXPECT_EQ(stats.stored.ndata_blks, 3u);
  EXPECT_EQ(stats.stored.max_idx_set, 129u);
  EXPECT_EQ(stats.stored.nelmts, 56u);

  // 6. Verify H5EA_open preserves identical statistics.
  haddr_t ea_addr = HADDR_UNDEF;
  ASSERT_GE(H5EA_get_addr(ea.get(), &ea_addr), 0);
  ea.reset();

  ea.reset(H5EA_open(f, ea_addr, nullptr));
  ASSERT_NE(ea.get(), nullptr);
  H5EA_stat_t reopened_stats{};
  EXPECT_GE(H5EA_get_stats(ea.get(), &reopened_stats), 0);
  EXPECT_EQ(reopened_stats.computed.hdr_size, stats.computed.hdr_size);
  EXPECT_EQ(reopened_stats.computed.nindex_blks, stats.computed.nindex_blks);
  EXPECT_EQ(reopened_stats.computed.index_blk_size,
            stats.computed.index_blk_size);
  EXPECT_EQ(reopened_stats.stored.nsuper_blks, stats.stored.nsuper_blks);
  EXPECT_EQ(reopened_stats.stored.super_blk_size, stats.stored.super_blk_size);
  EXPECT_EQ(reopened_stats.stored.ndata_blks, stats.stored.ndata_blks);
  EXPECT_EQ(reopened_stats.stored.data_blk_size, stats.stored.data_blk_size);
  EXPECT_EQ(reopened_stats.stored.max_idx_set, stats.stored.max_idx_set);
  EXPECT_EQ(reopened_stats.stored.nelmts, stats.stored.nelmts);
}

TEST(H5EAstatTest, PublicUnlimited1DChunkedDatasetUsesExtensibleArrayStats) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
            0);

  ScopedFile file(
      H5Fcreate("h5eastat_public.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kInitDims[1] = {32};
  constexpr hsize_t kMaxDims[1] = {H5S_UNLIMITED};
  constexpr hsize_t kChunkDims[1] = {4};
  ScopedSpace space(H5Screate_simple(1, kInitDims, kMaxDims));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, kChunkDims), 0);

  constexpr char kDatasetName[] = "/ea_chunked";
  ScopedDataset dset(H5Dcreate2(file.get(), kDatasetName, H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::vector<int32_t> data(32);
  for (int32_t i = 0; i < 32; ++i) {
    data[i] = i + 10;
  }
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, data.data()),
            0);
  EXPECT_GT(H5Dget_storage_size(dset.get()), 0u);
}

}  // namespace
}  // namespace hdf5
