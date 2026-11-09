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
// Tests fixed array metadata statistics retrieval across empty, unpaged,
// paged, reopened, and 1D fixed-size chunked dataset callers
// (third_party/hdf5/src/H5FAstat.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FAstat.c

#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields from H5AC_info_t
// (first field of H5FA_hdr_t). Define NDEBUG around internal headers so C++
// struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5FASTAT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5FA_FRIEND
#define H5FA_TESTING
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FApkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5FASTAT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FASTAT_TEST_RESTORE_UNDEF_NDEBUG
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

class ScopedFa {
 public:
  ScopedFa() = default;
  explicit ScopedFa(H5FA_t* fa) : fa_(fa) {}
  ~ScopedFa() { reset(); }

  ScopedFa(const ScopedFa&) = delete;
  ScopedFa& operator=(const ScopedFa&) = delete;

  void reset(H5FA_t* next = nullptr) {
    if (fa_ != nullptr) {
      H5FA_close(fa_);
    }
    fa_ = next;
  }

  H5FA_t* get() const { return fa_; }

 private:
  H5FA_t* fa_ = nullptr;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5FA_create_t MakeTestCreateParams(hsize_t nelmts) {
  H5FA_create_t cparam{};
  cparam.cls = H5FA_CLS_TEST;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_dblk_page_nelmts_bits = 4;  // 16 elements per page.
  cparam.nelmts = nelmts;
  return cparam;
}

std::vector<uint8_t> SnapshotFileImage(hid_t file_id) {
  const ssize_t size = H5Fget_file_image(file_id, nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> image(static_cast<size_t>(size));
  const ssize_t copied = H5Fget_file_image(file_id, image.data(), image.size());
  if (copied != size) {
    return {};
  }
  return image;
}

TEST(H5FAstatTest, EmptyUnpagedPagedAndReopenedArrayStatsMatchLayout) {
  haddr_t unpaged_addr = HADDR_UNDEF;
  haddr_t paged_addr = HADDR_UNDEF;
  H5FA_stat_t expected_unpaged_stats{};
  H5FA_stat_t expected_paged_stats{};
  std::vector<uint8_t> image;

  // 1. Create empty, unpaged, and paged fixed arrays in Core VFD and inspect
  // statistics before and after data block allocation.
  {
    ScopedFile file = CreateInMemoryFile("h5fastat_layout.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    // Unpaged array (10 elements <= 16 elements per page).
    const H5FA_create_t unpaged_cparam = MakeTestCreateParams(/*nelmts=*/10);
    ScopedFa unpaged_fa(H5FA_create(f, &unpaged_cparam, nullptr));
    ASSERT_NE(unpaged_fa.get(), nullptr);
    ASSERT_GE(H5FA_get_addr(unpaged_fa.get(), &unpaged_addr), 0);

    H5FA_stat_t stats{};
    EXPECT_GE(H5FA_get_stats(unpaged_fa.get(), &stats), 0);
    EXPECT_EQ(stats.hdr_size, H5FA_HEADER_SIZE_HDR(unpaged_fa.get()->hdr));
    EXPECT_EQ(stats.dblk_size, 0u);
    EXPECT_EQ(stats.nelmts, 10u);

    uint64_t val = 1003;
    ASSERT_GE(H5FA_set(unpaged_fa.get(), 3, &val), 0);
    EXPECT_GE(H5FA_get_stats(unpaged_fa.get(), &expected_unpaged_stats), 0);
    const hsize_t expected_unpaged_dblk_size =
        H5FA_METADATA_PREFIX_SIZE(true) + unpaged_fa.get()->hdr->sizeof_addr +
        10u * sizeof(uint64_t);
    EXPECT_EQ(expected_unpaged_stats.hdr_size,
              H5FA_HEADER_SIZE_HDR(unpaged_fa.get()->hdr));
    EXPECT_EQ(expected_unpaged_stats.dblk_size, expected_unpaged_dblk_size);
    EXPECT_EQ(expected_unpaged_stats.nelmts, 10u);

    // Paged array (40 elements -> 3 pages: 16, 16, and 8 elements).
    const H5FA_create_t paged_cparam = MakeTestCreateParams(/*nelmts=*/40);
    ScopedFa paged_fa(H5FA_create(f, &paged_cparam, nullptr));
    ASSERT_NE(paged_fa.get(), nullptr);
    ASSERT_GE(H5FA_get_addr(paged_fa.get(), &paged_addr), 0);

    H5FA_stat_t empty_paged_stats{};
    EXPECT_GE(H5FA_get_stats(paged_fa.get(), &empty_paged_stats), 0);
    EXPECT_EQ(empty_paged_stats.hdr_size,
              H5FA_HEADER_SIZE_HDR(paged_fa.get()->hdr));
    EXPECT_EQ(empty_paged_stats.dblk_size, 0u);
    EXPECT_EQ(empty_paged_stats.nelmts, 40u);

    val = 2039;
    ASSERT_GE(H5FA_set(paged_fa.get(), 39, &val), 0);
    EXPECT_GE(H5FA_get_stats(paged_fa.get(), &expected_paged_stats), 0);
    // Paged data block adds 1-byte page-init bitmap for 3 pages and 3
    // checksums.
    constexpr size_t kNumPages = 3;
    constexpr size_t kPageInitBytes = (kNumPages + 7) / 8;
    const hsize_t expected_paged_dblk_size =
        H5FA_METADATA_PREFIX_SIZE(true) + paged_fa.get()->hdr->sizeof_addr +
        kPageInitBytes + 40u * sizeof(uint64_t) +
        kNumPages * H5FA_SIZEOF_CHKSUM;
    EXPECT_EQ(expected_paged_stats.hdr_size,
              H5FA_HEADER_SIZE_HDR(paged_fa.get()->hdr));
    EXPECT_EQ(expected_paged_stats.dblk_size, expected_paged_dblk_size);
    EXPECT_EQ(expected_paged_stats.nelmts, 40u);

    unpaged_fa.reset();
    paged_fa.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    image = SnapshotFileImage(file.get());
    ASSERT_FALSE(image.empty());
  }

  // 2. Reopen the serialized file image and verify H5FA_open preserves stats.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(
      H5Fopen("h5fastat_reopen.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(reopened.valid());
  H5F_t* rf = GetH5File(reopened.get());
  ASSERT_NE(rf, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  {
    ScopedFa unpaged_fa(H5FA_open(rf, unpaged_addr, nullptr));
    ASSERT_NE(unpaged_fa.get(), nullptr);
    H5FA_stat_t reopened_stats{};
    EXPECT_GE(H5FA_get_stats(unpaged_fa.get(), &reopened_stats), 0);
    EXPECT_EQ(reopened_stats.hdr_size, expected_unpaged_stats.hdr_size);
    EXPECT_EQ(reopened_stats.dblk_size, expected_unpaged_stats.dblk_size);
    EXPECT_EQ(reopened_stats.nelmts, expected_unpaged_stats.nelmts);
  }

  {
    ScopedFa paged_fa(H5FA_open(rf, paged_addr, nullptr));
    ASSERT_NE(paged_fa.get(), nullptr);
    H5FA_stat_t reopened_stats{};
    EXPECT_GE(H5FA_get_stats(paged_fa.get(), &reopened_stats), 0);
    EXPECT_EQ(reopened_stats.hdr_size, expected_paged_stats.hdr_size);
    EXPECT_EQ(reopened_stats.dblk_size, expected_paged_stats.dblk_size);
    EXPECT_EQ(reopened_stats.nelmts, expected_paged_stats.nelmts);
  }
}

TEST(H5FAstatTest, PublicFixed1DChunkedDatasetUsesFixedArrayStats) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);

  ScopedFile file(
      H5Fcreate("h5fastat_public.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kDims[1] = {64};
  constexpr hsize_t kChunkDims[1] = {4};
  ScopedSpace space(H5Screate_simple(1, kDims, kDims));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, kChunkDims), 0);

  constexpr char kDatasetName[] = "/fa_chunked";
  ScopedDataset dset(H5Dcreate2(file.get(), kDatasetName, H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::vector<int32_t> data(64);
  for (int32_t i = 0; i < 64; ++i) {
    data[i] = i + 10;
  }
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, data.data()),
            0);

  EXPECT_EQ(H5Dget_storage_size(dset.get()), 64u * sizeof(int32_t));
  hsize_t num_chunks = 0;
  ASSERT_GE(H5Dget_num_chunks(dset.get(), space.get(), &num_chunks), 0);
  EXPECT_EQ(num_chunks, 16u);
}

}  // namespace
}  // namespace hdf5
