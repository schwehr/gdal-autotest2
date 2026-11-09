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
// Tests deprecated HDF5 dataset APIs (H5Dcreate1, H5Dopen1, H5Dextend,
// H5Dvlen_reclaim, H5Dread_chunk1) in third_party/hdf5/src/H5Ddeprec.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ddeprec.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

TEST(H5DdeprecTest, Create1AndOpen1WithDefaultAndCustomDcpl) {
  // 1. Create an in-memory file with a subgroup for relative/group-scoped paths.
  ScopedFile file = CreateInMemoryFile("deprec_create_open.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  constexpr std::array<hsize_t, 1> kDims = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 2. Create datasets using H5Dcreate1 with H5P_DEFAULT and with a custom
  //    compact DCPL, then write distinct payloads.
  constexpr std::array<int32_t, 4> kDefaultData = {10, 20, 30, 40};
  constexpr std::array<int32_t, 4> kCompactData = {101, 202, 303, 404};

  {
    ScopedDataset dset_default(
        H5Dcreate1(file, "dset_def", H5T_NATIVE_INT32, space, H5P_DEFAULT));
    ASSERT_TRUE(dset_default.valid());
    ASSERT_GE(H5Dwrite(dset_default, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kDefaultData.data()),
              0);

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_layout(dcpl, H5D_COMPACT), 0);

    ScopedDataset dset_compact(
        H5Dcreate1(grp, "dset_compact", H5T_NATIVE_INT32, space, dcpl));
    ASSERT_TRUE(dset_compact.valid());
    ASSERT_GE(H5Dwrite(dset_compact, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kCompactData.data()),
              0);
  }

  // 3. Re-open both datasets via H5Dopen1 and verify contents.
  {
    ScopedDataset dset_default(H5Dopen1(file, "dset_def"));
    ASSERT_TRUE(dset_default.valid());
    std::array<int32_t, 4> got = {-1, -1, -1, -1};
    ASSERT_GE(H5Dread(dset_default, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, got.data()),
              0);
    EXPECT_THAT(got, ElementsAre(10, 20, 30, 40));
  }
  {
    ScopedDataset dset_compact(H5Dopen1(grp, "dset_compact"));
    ASSERT_TRUE(dset_compact.valid());
    std::array<int32_t, 4> got = {-1, -1, -1, -1};
    ASSERT_GE(H5Dread(dset_compact, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, got.data()),
              0);
    EXPECT_THAT(got, ElementsAre(101, 202, 303, 404));
  }
}

TEST(H5DdeprecTest, Create1AndOpen1RejectInvalidArgs) {
  ScopedFile file = CreateInMemoryFile("deprec_create_open_err.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  constexpr std::array<hsize_t, 1> kDims = {3};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset existing(
      H5Dcreate1(file, "existing", H5T_NATIVE_INT32, space, H5P_DEFAULT));
  ASSERT_TRUE(existing.valid());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  ScopedErrorSilence silence;

  // H5Dcreate1 argument validation and creation failures.
  EXPECT_EQ(
      H5Dcreate1(file, nullptr, H5T_NATIVE_INT32, space, H5P_DEFAULT),
      H5I_INVALID_HID);
  EXPECT_EQ(H5Dcreate1(file, "", H5T_NATIVE_INT32, space, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Dcreate1(H5I_INVALID_HID, "bad_loc", H5T_NATIVE_INT32, space,
                       H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Dcreate1(space, "space_loc", H5T_NATIVE_INT32, space,
                       H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Dcreate1(file, "bad_dcpl", H5T_NATIVE_INT32, space, fapl),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Dcreate1(file, "existing", H5T_NATIVE_INT32, space, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Dcreate1(file, "bad_space", H5T_NATIVE_INT32, H5I_INVALID_HID,
                       H5P_DEFAULT),
            H5I_INVALID_HID);

  // H5Dopen1 argument validation and lookup failures.
  EXPECT_EQ(H5Dopen1(file, nullptr), H5I_INVALID_HID);
  EXPECT_EQ(H5Dopen1(file, ""), H5I_INVALID_HID);
  EXPECT_EQ(H5Dopen1(H5I_INVALID_HID, "existing"), H5I_INVALID_HID);
  EXPECT_EQ(H5Dopen1(space, "existing"), H5I_INVALID_HID);
  EXPECT_EQ(H5Dopen1(file, "missing"), H5I_INVALID_HID);
  EXPECT_EQ(H5Dopen1(file, "grp"), H5I_INVALID_HID);
}

TEST(H5DdeprecTest, ExtendGrowsWithoutShrinkingAndRejectsInvalidRequests) {
  ScopedFile file = CreateInMemoryFile("deprec_extend.h5");
  ASSERT_TRUE(file.valid());

  // 1. Create an extendible 2D chunked dataset of shape (2, 3) with max
  //    (6, H5S_UNLIMITED).
  constexpr std::array<hsize_t, 2> kInitDims = {2, 3};
  constexpr std::array<hsize_t, 2> kMaxDims = {6, H5S_UNLIMITED};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 3};

  ScopedSpace space(H5Screate_simple(2, kInitDims.data(), kMaxDims.data()));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunkDims.data()), 0);
  const int32_t fill_val = -99;
  ASSERT_GE(H5Pset_fill_value(dcpl, H5T_NATIVE_INT32, &fill_val), 0);

  ScopedDataset chunked(
      H5Dcreate1(file, "chunked", H5T_NATIVE_INT32, space, dcpl));
  ASSERT_TRUE(chunked.valid());

  // 2. Extend both dimensions to (4, 5).
  constexpr std::array<hsize_t, 2> kGrowBoth = {4, 5};
  ASSERT_GE(H5Dextend(chunked, kGrowBoth.data()), 0);

  {
    ScopedSpace cur_space(H5Dget_space(chunked));
    ASSERT_TRUE(cur_space.valid());
    std::array<hsize_t, 2> dims = {0, 0};
    ASSERT_EQ(H5Sget_simple_extent_dims(cur_space, dims.data(), nullptr), 2);
    EXPECT_THAT(dims, ElementsAre(4, 5));
  }

  // 3. Passing a smaller extent on axis 0 (2 < 4) must keep axis 0 at 4 while
  //    growing axis 1 to 7 (H5Dextend never shrinks dimensions).
  constexpr std::array<hsize_t, 2> kGrowOne = {2, 7};
  ASSERT_GE(H5Dextend(chunked, kGrowOne.data()), 0);

  {
    ScopedSpace cur_space(H5Dget_space(chunked));
    ASSERT_TRUE(cur_space.valid());
    std::array<hsize_t, 2> dims = {0, 0};
    ASSERT_EQ(H5Sget_simple_extent_dims(cur_space, dims.data(), nullptr), 2);
    EXPECT_THAT(dims, ElementsAre(4, 7));
  }

  // 4. Create a fixed-size contiguous dataset to verify H5Dextend rejects
  //    attempts to exceed max_dims as well as invalid arguments.
  ScopedSpace fixed_space(H5Screate_simple(2, kInitDims.data(), nullptr));
  ASSERT_TRUE(fixed_space.valid());
  ScopedDataset contig(
      H5Dcreate1(file, "contig", H5T_NATIVE_INT32, fixed_space, H5P_DEFAULT));
  ASSERT_TRUE(contig.valid());

  ScopedErrorSilence silence;
  EXPECT_LT(H5Dextend(H5I_INVALID_HID, kGrowBoth.data()), 0);
  EXPECT_LT(H5Dextend(file, kGrowBoth.data()), 0);
  EXPECT_LT(H5Dextend(chunked, nullptr), 0);

  constexpr std::array<hsize_t, 2> kTooLarge = {10, 5};
  EXPECT_LT(H5Dextend(chunked, kTooLarge.data()), 0);
  EXPECT_LT(H5Dextend(contig, kGrowBoth.data()), 0);
}

TEST(H5DdeprecTest, VlenReclaimFreesDefaultAndCustomAllocators) {
  ScopedFile file = CreateInMemoryFile("deprec_vlen_reclaim.h5");
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {2};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. Reclaim variable-length integer sequences using H5P_DEFAULT.
  ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vl_int.valid());

  std::array<int32_t, 2> row0 = {1, 2};
  std::array<int32_t, 3> row1 = {3, 4, 5};
  std::array<hvl_t, 2> w_vlen = {
      hvl_t{row0.size(), row0.data()},
      hvl_t{row1.size(), row1.data()},
  };

  ScopedDataset dset_vl(
      H5Dcreate1(file, "vlen_int", vl_int, space, H5P_DEFAULT));
  ASSERT_TRUE(dset_vl.valid());
  ASSERT_GE(H5Dwrite(dset_vl, vl_int, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     w_vlen.data()),
            0);

  std::array<hvl_t, 2> r_vlen{};
  ASSERT_GE(
      H5Dread(dset_vl, vl_int, H5S_ALL, H5S_ALL, H5P_DEFAULT, r_vlen.data()),
      0);
  absl::Cleanup reclaim_vlen_fallback = [&] {
    if (r_vlen[0].p != nullptr || r_vlen[1].p != nullptr) {
      H5Dvlen_reclaim(vl_int, space, H5P_DEFAULT, r_vlen.data());
    }
  };
  ASSERT_EQ(r_vlen[0].len, 2u);
  EXPECT_EQ(static_cast<int32_t*>(r_vlen[0].p)[1], 2);
  ASSERT_EQ(r_vlen[1].len, 3u);
  EXPECT_EQ(static_cast<int32_t*>(r_vlen[1].p)[2], 5);

  ASSERT_GE(H5Dvlen_reclaim(vl_int, space, H5P_DEFAULT, r_vlen.data()), 0);
  std::memset(r_vlen.data(), 0, sizeof(r_vlen));

  // 2. Reclaim variable-length strings with a custom DXPL allocator to verify
  //    non-default dxpl_id handling and custom free callback invocation.
  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_str.valid());
  ASSERT_GE(H5Tset_size(vl_str, H5T_VARIABLE), 0);

  constexpr std::array<const char*, 2> kWriteStrs = {"alpha", "beta_long"};
  ScopedDataset dset_str(
      H5Dcreate1(file, "vlen_str", vl_str, space, H5P_DEFAULT));
  ASSERT_TRUE(dset_str.valid());
  ASSERT_GE(H5Dwrite(dset_str, vl_str, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     kWriteStrs.data()),
            0);

  struct MemCounters {
    int allocs = 0;
    int frees = 0;
  } counters;
  auto custom_alloc = [](size_t size, void* alloc_info) -> void* {
    ++static_cast<MemCounters*>(alloc_info)->allocs;
    return std::malloc(size);
  };
  auto custom_free = [](void* mem, void* free_info) {
    if (mem != nullptr) {
      ++static_cast<MemCounters*>(free_info)->frees;
      std::free(mem);
    }
  };

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(
      H5Pset_vlen_mem_manager(dxpl, custom_alloc, &counters, custom_free,
                              &counters),
      0);

  std::array<char*, 2> r_strs = {nullptr, nullptr};
  ASSERT_GE(
      H5Dread(dset_str, vl_str, H5S_ALL, H5S_ALL, dxpl, r_strs.data()), 0);
  absl::Cleanup reclaim_str_fallback = [&] {
    if (r_strs[0] != nullptr || r_strs[1] != nullptr) {
      H5Dvlen_reclaim(vl_str, space, dxpl, r_strs.data());
    }
  };
  EXPECT_EQ(counters.allocs, 2);
  EXPECT_STREQ(r_strs[0], "alpha");
  EXPECT_STREQ(r_strs[1], "beta_long");

  ASSERT_GE(H5Dvlen_reclaim(vl_str, space, dxpl, r_strs.data()), 0);
  r_strs = {nullptr, nullptr};
  EXPECT_EQ(counters.frees, 2);

  // 3. Negative checks for H5Dvlen_reclaim.
  ScopedSpace no_extent_space(H5Screate(H5S_SIMPLE));
  ASSERT_TRUE(no_extent_space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());

  int32_t dummy = 0;
  ScopedErrorSilence silence;
  EXPECT_LT(H5Dvlen_reclaim(vl_int, space, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5Dvlen_reclaim(H5I_INVALID_HID, space, H5P_DEFAULT, &dummy), 0);
  EXPECT_LT(H5Dvlen_reclaim(vl_int, H5I_INVALID_HID, H5P_DEFAULT, &dummy), 0);
  EXPECT_LT(H5Dvlen_reclaim(vl_int, no_extent_space, H5P_DEFAULT, &dummy), 0);
  EXPECT_LT(H5Dvlen_reclaim(vl_int, space, dcpl, &dummy), 0);
}

TEST(H5DdeprecTest, ReadChunk1ReadsRawChunksAndRejectsInvalidInputs) {
  ScopedFile file = CreateInMemoryFile("deprec_read_chunk.h5");
  ASSERT_TRUE(file.valid());

  // 1. Create an uncompressed chunked 2D dataset and write a known pattern.
  constexpr std::array<hsize_t, 2> kDims = {4, 4};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 2};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunkDims.data()), 0);

  ScopedDataset chunked(
      H5Dcreate1(file, "chunked", H5T_NATIVE_INT32, space, dcpl));
  ASSERT_TRUE(chunked.valid());

  std::array<int32_t, 16> data{};
  for (int32_t i = 0; i < 16; ++i) {
    data[static_cast<size_t>(i)] = 100 + i;
  }
  ASSERT_GE(H5Dwrite(chunked, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     data.data()),
            0);

  // 2. Read the chunk at offset (2, 2) using H5P_DEFAULT and verify its values.
  constexpr std::array<hsize_t, 2> kChunkOffset = {2, 2};
  std::array<int32_t, 4> raw_chunk = {-1, -1, -1, -1};
  uint32_t filters = 0xFFFFFFFFu;
  ASSERT_GE(H5Dread_chunk1(chunked, H5P_DEFAULT, kChunkOffset.data(), &filters,
                           raw_chunk.data()),
            0);
  EXPECT_EQ(filters, 0u);
  EXPECT_THAT(raw_chunk, ElementsAre(110, 111, 114, 115));

  // 3. Read a chunk from a deflate-compressed dataset with a custom DXPL.
  ScopedPlist dcpl_comp(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_comp.valid());
  ASSERT_GE(H5Pset_chunk(dcpl_comp, 2, kChunkDims.data()), 0);
  ASSERT_GE(H5Pset_deflate(dcpl_comp, 6), 0);

  ScopedDataset compressed(
      H5Dcreate1(file, "compressed", H5T_NATIVE_INT32, space, dcpl_comp));
  ASSERT_TRUE(compressed.valid());
  ASSERT_GE(H5Dwrite(compressed, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, data.data()),
            0);

  constexpr std::array<hsize_t, 2> kOrigin = {0, 0};
  hsize_t chunk_nbytes = 0;
  ASSERT_GE(H5Dget_chunk_storage_size(compressed, kOrigin.data(),
                                      &chunk_nbytes),
            0);
  ASSERT_GT(chunk_nbytes, 0u);

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  std::vector<uint8_t> comp_buf(static_cast<size_t>(chunk_nbytes), 0);
  filters = 0xFFFFFFFFu;
  ASSERT_GE(H5Dread_chunk1(compressed, dxpl, kOrigin.data(), &filters,
                           comp_buf.data()),
            0);
  EXPECT_EQ(filters, 0u);

  // 4. Create a contiguous dataset to verify H5Dread_chunk1 rejects non-chunked
  //    datasets and invalid arguments.
  ScopedDataset contig(
      H5Dcreate1(file, "contig", H5T_NATIVE_INT32, space, H5P_DEFAULT));
  ASSERT_TRUE(contig.valid());

  constexpr std::array<hsize_t, 2> kBadOffset = {10, 10};
  ScopedErrorSilence silence;
  EXPECT_LT(H5Dread_chunk1(H5I_INVALID_HID, H5P_DEFAULT, kOrigin.data(),
                           &filters, raw_chunk.data()),
            0);
  EXPECT_LT(
      H5Dread_chunk1(file, H5P_DEFAULT, kOrigin.data(), &filters,
                     raw_chunk.data()),
      0);
  EXPECT_LT(
      H5Dread_chunk1(chunked, H5P_DEFAULT, kOrigin.data(), &filters, nullptr),
      0);
  EXPECT_LT(H5Dread_chunk1(chunked, H5P_DEFAULT, nullptr, &filters,
                           raw_chunk.data()),
            0);
  EXPECT_LT(H5Dread_chunk1(chunked, H5P_DEFAULT, kOrigin.data(), nullptr,
                           raw_chunk.data()),
            0);
  EXPECT_LT(H5Dread_chunk1(chunked, dcpl, kOrigin.data(), &filters,
                           raw_chunk.data()),
            0);
  EXPECT_LT(H5Dread_chunk1(contig, H5P_DEFAULT, kOrigin.data(), &filters,
                           raw_chunk.data()),
            0);
  EXPECT_LT(H5Dread_chunk1(chunked, H5P_DEFAULT, kBadOffset.data(), &filters,
                           raw_chunk.data()),
            0);
}

}  // namespace
}  // namespace hdf5
