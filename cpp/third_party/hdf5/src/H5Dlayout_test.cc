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
// Unit tests for HDF5 dataset layout operations, header message creation/read/
// write, metadata sizing, and version bounds
// (third_party/hdf5/src/H5Dlayout.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dlayout.c

#include "third_party/hdf5/src/hdf5.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields from metadata
// structures pulled by H5private.h and H5Dpkg.h.
#ifndef NDEBUG
#define H5DLAYOUT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5D_FRIEND
#define H5F_FRIEND
#define H5G_FRIEND
#define H5O_FRIEND
#define H5S_FRIEND
#define H5T_FRIEND
// H5B2private.h (pulled by H5Dpkg.h) and H5Fpkg.h use `try` as a parameter
// name, colliding with libc++'s `#define try if (1)` and C++ keyword `try`.
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Pprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#pragma push_macro("H5_HAVE__FLOAT16")
#undef H5_HAVE__FLOAT16
#include "third_party/hdf5/src/H5Tpkg.h"
#pragma pop_macro("H5_HAVE__FLOAT16")
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DLAYOUT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DLAYOUT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dxpl_id = H5P_DEFAULT) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (dxpl_id != H5P_DEFAULT) {
        H5CX_set_dxpl(dxpl_id);
      }
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

herr_t FailChunkInit(H5F_t* /*f*/, H5D_t* /*dset*/, hid_t /*dapl_id*/,
                     bool /*open_op*/) {
  return -1;
}

TEST(H5DlayoutTest, VersionBoundsMatchExpectedFormatVersions) {
  ASSERT_GE(H5open(), 0);
  EXPECT_EQ(H5O_layout_ver_bounds[H5F_LIBVER_EARLIEST],
            static_cast<unsigned>(H5O_LAYOUT_VERSION_1));
  EXPECT_EQ(H5O_layout_ver_bounds[H5F_LIBVER_V18],
            static_cast<unsigned>(H5O_LAYOUT_VERSION_3));
  EXPECT_EQ(H5O_layout_ver_bounds[H5F_LIBVER_V110],
            static_cast<unsigned>(H5O_LAYOUT_VERSION_4));
  EXPECT_EQ(H5O_layout_ver_bounds[H5F_LIBVER_V112],
            static_cast<unsigned>(H5O_LAYOUT_VERSION_4));
  EXPECT_EQ(H5O_layout_ver_bounds[H5F_LIBVER_V114],
            static_cast<unsigned>(H5O_LAYOUT_VERSION_4));
  EXPECT_EQ(H5O_layout_ver_bounds[H5F_LIBVER_V200],
            static_cast<unsigned>(H5O_LAYOUT_VERSION_5));
  EXPECT_EQ(H5O_layout_ver_bounds[H5F_LIBVER_LATEST],
            static_cast<unsigned>(H5O_LAYOUT_VERSION_LATEST));
}

TEST(H5DlayoutTest, CreateReopenAndModifyAllLayoutsAndChunkIndexTypes) {
  // 1. Earliest-format file for compact, contiguous (early/late/zero-size),
  //    EFL, virtual, and v1-B-tree chunked (filtered & unfiltered) layouts.
  TempDir efl_dir("h5dlayout_efl");
  {
    std::ofstream f1(efl_dir.child("slot1.bin"), std::ios::binary);
    std::ofstream f2(efl_dir.child("slot2.bin"), std::ios::binary);
    const std::vector<char> zeros(16, 0);
    f1.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    f2.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  }

  ScopedPlist fapl_early = MakeCoreFapl();
  ASSERT_TRUE(fapl_early.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_early, H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST),
      0);

  ScopedFile file_early(H5Fcreate("layout_early.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                  fapl_early));
  ASSERT_TRUE(file_early.valid());

  constexpr std::array<hsize_t, 1> kDims4 = {4};
  constexpr std::array<hsize_t, 1> kDims8 = {8};
  constexpr std::array<hsize_t, 1> kDimsZero = {0};
  constexpr std::array<hsize_t, 1> kChunk4 = {4};
  ScopedSpace space4(H5Screate_simple(1, kDims4.data(), nullptr));
  ScopedSpace space8(H5Screate_simple(1, kDims8.data(), nullptr));
  ScopedSpace space0(H5Screate_simple(1, kDimsZero.data(), nullptr));
  ASSERT_TRUE(space4.valid());
  ASSERT_TRUE(space8.valid());
  ASSERT_TRUE(space0.valid());

  // 1a. Compact dataset create/write/reopen.
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_layout(dcpl, H5D_COMPACT), 0);
    ScopedDataset dset(H5Dcreate2(file_early, "compact", H5T_NATIVE_INT32,
                                  space4, H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    constexpr std::array<int32_t, 4> kData = {10, 20, 30, 40};
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kData.data()),
              0);
  }
  {
    ScopedDataset dset(H5Dopen2(file_early, "compact", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::array<int32_t, 4> got = {};
    ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      got.data()),
              0);
    EXPECT_THAT(got, ElementsAre(10, 20, 30, 40));
  }

  // 1b. Contiguous datasets: EARLY (constant layout msg), EARLY with 0 points,
  //     and LATE (updated via H5D__layout_oh_write upon first write).
  {
    ScopedPlist dcpl_early(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_early.valid());
    ASSERT_GE(H5Pset_alloc_time(dcpl_early, H5D_ALLOC_TIME_EARLY), 0);

    ScopedDataset d_early(H5Dcreate2(file_early, "contig_early",
                                     H5T_NATIVE_INT32, space4, H5P_DEFAULT,
                                     dcpl_early, H5P_DEFAULT));
    ASSERT_TRUE(d_early.valid());

    ScopedDataset d_zero(H5Dcreate2(file_early, "contig_zero_early",
                                    H5T_NATIVE_INT32, space0, H5P_DEFAULT,
                                    dcpl_early, H5P_DEFAULT));
    ASSERT_TRUE(d_zero.valid());

    ScopedPlist dcpl_late(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_late.valid());
    ASSERT_GE(H5Pset_alloc_time(dcpl_late, H5D_ALLOC_TIME_LATE), 0);

    ScopedDataset d_late(H5Dcreate2(file_early, "contig_late", H5T_NATIVE_INT32,
                                    space4, H5P_DEFAULT, dcpl_late,
                                    H5P_DEFAULT));
    ASSERT_TRUE(d_late.valid());
    constexpr std::array<int32_t, 4> kData = {1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(d_late, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kData.data()),
              0);
  }
  {
    ScopedDataset d_late(H5Dopen2(file_early, "contig_late", H5P_DEFAULT));
    ASSERT_TRUE(d_late.valid());
    std::array<int32_t, 4> got = {};
    ASSERT_GE(H5Dread(d_late, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      got.data()),
              0);
    EXPECT_THAT(got, ElementsAre(1, 2, 3, 4));
  }

  // 1c. External File List (EFL) contiguous dataset with two external files.
  {
    ScopedPlist dcpl_efl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_efl.valid());
    ASSERT_GE(H5Pset_external(dcpl_efl, "slot1.bin", 0, 16), 0);
    ASSERT_GE(H5Pset_external(dcpl_efl, "slot2.bin", 0, 16), 0);

    ScopedPlist dapl_efl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl_efl.valid());
    ASSERT_GE(H5Pset_efile_prefix(dapl_efl, efl_dir.c_str()), 0);

    ScopedDataset d_efl(H5Dcreate2(file_early, "efl_dset", H5T_NATIVE_INT32,
                                   space8, H5P_DEFAULT, dcpl_efl, dapl_efl));
    ASSERT_TRUE(d_efl.valid());
    constexpr std::array<int32_t, 8> kVals = {11, 22, 33, 44, 55, 66, 77, 88};
    ASSERT_GE(H5Dwrite(d_efl, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kVals.data()),
              0);
  }
  {
    ScopedPlist dapl_efl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl_efl.valid());
    ASSERT_GE(H5Pset_efile_prefix(dapl_efl, efl_dir.c_str()), 0);

    ScopedDataset d_efl(H5Dopen2(file_early, "efl_dset", dapl_efl));
    ASSERT_TRUE(d_efl.valid());
    std::array<int32_t, 8> got = {};
    ASSERT_GE(H5Dread(d_efl, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      got.data()),
              0);
    EXPECT_THAT(got, ElementsAre(11, 22, 33, 44, 55, 66, 77, 88));
  }

  // 1d. Virtual dataset (H5D_VIRTUAL) backed by a source dataset in the file.
  {
    ScopedDataset src(H5Dcreate2(file_early, "vds_src", H5T_NATIVE_INT32,
                                 space4, H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
    ASSERT_TRUE(src.valid());
    constexpr std::array<int32_t, 4> kSrcVals = {101, 102, 103, 104};
    ASSERT_GE(H5Dwrite(src, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kSrcVals.data()),
              0);

    ScopedPlist dcpl_vds(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_vds.valid());
    ASSERT_GE(H5Pset_virtual(dcpl_vds, space4, ".", "/vds_src", space4), 0);

    ScopedDataset vds(H5Dcreate2(file_early, "vds", H5T_NATIVE_INT32, space4,
                                 H5P_DEFAULT, dcpl_vds, H5P_DEFAULT));
    ASSERT_TRUE(vds.valid());
  }
  {
    ScopedDataset vds(H5Dopen2(file_early, "vds", H5P_DEFAULT));
    ASSERT_TRUE(vds.valid());
    std::array<int32_t, 4> got = {};
    ASSERT_GE(H5Dread(vds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      got.data()),
              0);
    EXPECT_THAT(got, ElementsAre(101, 102, 103, 104));
  }

  // 1e. Chunked v3 layout (H5D_CHUNK_IDX_BTREE) with deflate filter.
  {
    ScopedPlist dcpl_chk(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_chk.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_chk, 1, kChunk4.data()), 0);
    ASSERT_GE(H5Pset_deflate(dcpl_chk, 6), 0);

    ScopedDataset d_chk(H5Dcreate2(file_early, "chunk_btree_filt",
                                   H5T_NATIVE_INT32, space8, H5P_DEFAULT,
                                   dcpl_chk, H5P_DEFAULT));
    ASSERT_TRUE(d_chk.valid());
    constexpr std::array<int32_t, 8> kVals = {5, 6, 7, 8, 9, 10, 11, 12};
    ASSERT_GE(H5Dwrite(d_chk, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kVals.data()),
              0);
  }
  {
    ScopedDataset d_chk(H5Dopen2(file_early, "chunk_btree_filt", H5P_DEFAULT));
    ASSERT_TRUE(d_chk.valid());
    std::array<int32_t, 8> got = {};
    ASSERT_GE(H5Dread(d_chk, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      got.data()),
              0);
    EXPECT_THAT(got, ElementsAre(5, 6, 7, 8, 9, 10, 11, 12));
  }

  // 2. Modern format file (v1.10+ / latest) for IDX_NONE, IDX_SINGLE (filtered
  //    and unfiltered), IDX_FARRAY, IDX_EARRAY, and IDX_BT2.
  ScopedPlist fapl_v110 = MakeCoreFapl();
  ASSERT_TRUE(fapl_v110.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_v110, H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);
  ScopedFile file_v110(H5Fcreate("layout_v110.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                 fapl_v110));
  ASSERT_TRUE(file_v110.valid());

  // 2a. IDX_NONE (fixed dims, >1 chunks, early alloc, unfiltered).
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl, 1, kChunk4.data()), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl, H5D_ALLOC_TIME_EARLY), 0);
    ScopedDataset d(H5Dcreate2(file_v110, "idx_none", H5T_NATIVE_INT32, space8,
                               H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(d.valid());
    ScopedDataset r(H5Dopen2(file_v110, "idx_none", H5P_DEFAULT));
    ASSERT_TRUE(r.valid());
  }

  // 2b. IDX_SINGLE (unfiltered and filtered).
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl, 1, kChunk4.data()), 0);
    ScopedDataset d(H5Dcreate2(file_v110, "idx_single", H5T_NATIVE_INT32,
                               space4, H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(d.valid());
    ASSERT_GE(H5Pset_deflate(dcpl, 5), 0);
    ScopedDataset df(H5Dcreate2(file_v110, "idx_single_filt", H5T_NATIVE_INT32,
                                space4, H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(df.valid());
    constexpr std::array<int32_t, 4> kVals = {1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(df, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kVals.data()),
              0);
  }

  // 2c. IDX_FARRAY (fixed dims, >1 chunks, late alloc).
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl, 1, kChunk4.data()), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl, H5D_ALLOC_TIME_LATE), 0);
    ScopedDataset d(H5Dcreate2(file_v110, "idx_farray", H5T_NATIVE_INT32,
                               space8, H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(d.valid());
    ScopedDataset r(H5Dopen2(file_v110, "idx_farray", H5P_DEFAULT));
    ASSERT_TRUE(r.valid());
  }

  // 2d. IDX_EARRAY (1 unlimited dim) + extent growth triggering oh_write.
  {
    constexpr std::array<hsize_t, 1> kMaxUnlim = {H5S_UNLIMITED};
    ScopedSpace space_ea(H5Screate_simple(1, kDims4.data(), kMaxUnlim.data()));
    ASSERT_TRUE(space_ea.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl, 1, kChunk4.data()), 0);
    ScopedDataset d(H5Dcreate2(file_v110, "idx_earray", H5T_NATIVE_INT32,
                               space_ea, H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(d.valid());
    constexpr std::array<hsize_t, 1> kExtDims = {8};
    ASSERT_GE(H5Dset_extent(d, kExtDims.data()), 0);
    ScopedDataset r(H5Dopen2(file_v110, "idx_earray", H5P_DEFAULT));
    ASSERT_TRUE(r.valid());
  }

  // 2e. IDX_BT2 (>1 unlimited dims).
  {
    constexpr std::array<hsize_t, 2> kDims2d = {2, 2};
    constexpr std::array<hsize_t, 2> kMax2d = {H5S_UNLIMITED, H5S_UNLIMITED};
    constexpr std::array<hsize_t, 2> kChunk2d = {2, 2};
    ScopedSpace space_bt2(
        H5Screate_simple(2, kDims2d.data(), kMax2d.data()));
    ASSERT_TRUE(space_bt2.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunk2d.data()), 0);
    ScopedDataset d(H5Dcreate2(file_v110, "idx_bt2", H5T_NATIVE_INT32,
                               space_bt2, H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(d.valid());
    ScopedDataset r(H5Dopen2(file_v110, "idx_bt2", H5P_DEFAULT));
    ASSERT_TRUE(r.valid());
  }
}

TEST(H5DlayoutTest, LayoutMetaSizeCoversAllLayoutsVersionsAndErrors) {
  ScopedFile file = CreateInMemoryFile("meta_size.h5");
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file, H5I_FILE));
  ASSERT_NE(f, nullptr);

  const size_t addr_sz = H5F_SIZEOF_ADDR(f);
  const size_t len_sz = H5F_SIZEOF_SIZE(f);

  // Compact: header + 2-byte size, optionally including compact payload.
  H5O_layout_t compact{};
  compact.type = H5D_COMPACT;
  compact.storage.u.compact.size = 24;
  EXPECT_EQ(H5D__layout_meta_size(f, &compact, /*include_compact_data=*/false),
            4u);
  EXPECT_EQ(H5D__layout_meta_size(f, &compact, /*include_compact_data=*/true),
            28u);

  // Contiguous: header + addr + size.
  H5O_layout_t contig{};
  contig.type = H5D_CONTIGUOUS;
  EXPECT_EQ(H5D__layout_meta_size(f, &contig, false), 2u + addr_sz + len_sz);

  // Virtual: header + global heap addr + 4-byte index.
  H5O_layout_t virt{};
  virt.type = H5D_VIRTUAL;
  EXPECT_EQ(H5D__layout_meta_size(f, &virt, false), 2u + addr_sz + 4u);

  // Chunked v3 (< H5O_LAYOUT_VERSION_4): 2 + 1 + addr + ndims * 4.
  H5O_layout_t chunk_v3{};
  chunk_v3.type = H5D_CHUNKED;
  chunk_v3.version = H5O_LAYOUT_VERSION_3;
  chunk_v3.u.chunk.ndims = 3;
  EXPECT_EQ(H5D__layout_meta_size(f, &chunk_v3, false),
            3u + addr_sz + 3u * 4u);

  // Chunked v4+: covers all valid index types plus invalid combinations.
  H5O_layout_t chunk_v4{};
  chunk_v4.type = H5D_CHUNKED;
  chunk_v4.version = H5O_LAYOUT_VERSION_4;
  chunk_v4.u.chunk.ndims = 2;
  chunk_v4.u.chunk.enc_bytes_per_dim = 2;
  const size_t v4_base = 2u + 1u + 1u + 1u + 2u * 2u + 1u + addr_sz;

  chunk_v4.u.chunk.idx_type = H5D_CHUNK_IDX_NONE;
  EXPECT_EQ(H5D__layout_meta_size(f, &chunk_v4, false), v4_base);

  chunk_v4.u.chunk.idx_type = H5D_CHUNK_IDX_SINGLE;
  chunk_v4.u.chunk.flags = 0;
  EXPECT_EQ(H5D__layout_meta_size(f, &chunk_v4, false), v4_base);

  chunk_v4.u.chunk.flags = H5O_LAYOUT_CHUNK_SINGLE_INDEX_WITH_FILTER;
  EXPECT_EQ(H5D__layout_meta_size(f, &chunk_v4, false),
            v4_base + len_sz + 4u);

  chunk_v4.u.chunk.idx_type = H5D_CHUNK_IDX_FARRAY;
  EXPECT_EQ(H5D__layout_meta_size(f, &chunk_v4, false),
            v4_base + H5D_FARRAY_CREATE_PARAM_SIZE);

  chunk_v4.u.chunk.idx_type = H5D_CHUNK_IDX_EARRAY;
  EXPECT_EQ(H5D__layout_meta_size(f, &chunk_v4, false),
            v4_base + H5D_EARRAY_CREATE_PARAM_SIZE);

  chunk_v4.u.chunk.idx_type = H5D_CHUNK_IDX_BT2;
  EXPECT_EQ(H5D__layout_meta_size(f, &chunk_v4, false),
            v4_base + H5D_BT2_CREATE_PARAM_SIZE);

  // Error cases return 0.
  ScopedErrorSilence silence;
  chunk_v4.u.chunk.idx_type = H5D_CHUNK_IDX_BTREE;
  EXPECT_EQ(H5D__layout_meta_size(f, &chunk_v4, false), 0u);

  chunk_v4.u.chunk.idx_type = H5D_CHUNK_IDX_NTYPES;
  EXPECT_EQ(H5D__layout_meta_size(f, &chunk_v4, false), 0u);

  H5O_layout_t bad_layout{};
  bad_layout.type = H5D_LAYOUT_ERROR;
  EXPECT_EQ(H5D__layout_meta_size(f, &bad_layout, false), 0u);
  bad_layout.type = H5D_NLAYOUTS;
  EXPECT_EQ(H5D__layout_meta_size(f, &bad_layout, false), 0u);
}

TEST(H5DlayoutTest, SetIoOpsAndOhReadWriteHandleErrorAndMissingMsgPaths) {
  ScopedFile file = CreateInMemoryFile("layout_errors.h5");
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file, H5I_FILE));
  ASSERT_NE(f, nullptr);

  // 1. H5D__layout_set_io_ops covers all chunk index types and rejects invalid
  //    chunk index and layout types.
  {
    H5D_shared_t shared{};
    H5D_t dset{};
    dset.shared = &shared;

    shared.layout.type = H5D_CHUNKED;
    for (auto idx :
         {H5D_CHUNK_IDX_BTREE, H5D_CHUNK_IDX_NONE, H5D_CHUNK_IDX_SINGLE,
          H5D_CHUNK_IDX_FARRAY, H5D_CHUNK_IDX_EARRAY, H5D_CHUNK_IDX_BT2}) {
      shared.layout.u.chunk.idx_type = idx;
      EXPECT_GE(H5D__layout_set_io_ops(&dset), 0);
    }

    ScopedErrorSilence silence;
    shared.layout.u.chunk.idx_type = H5D_CHUNK_IDX_NTYPES;
    EXPECT_LT(H5D__layout_set_io_ops(&dset), 0);

    shared.layout.type = H5D_LAYOUT_ERROR;
    EXPECT_LT(H5D__layout_set_io_ops(&dset), 0);

    shared.layout.type = H5D_NLAYOUTS;
    EXPECT_LT(H5D__layout_set_io_ops(&dset), 0);
  }

  // 2. H5D__layout_oh_read error rollback (missing msg, wrong plist class for
  //    pline/efl, and ops->init failure) and H5D__layout_oh_write paths.
  {
    ScopedGroup grp(H5Gcreate2(file, "grp", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    auto* g = static_cast<H5G_t*>(H5VL_object_verify(grp, H5I_GROUP));
    ASSERT_NE(g, nullptr);

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    auto* dc_plist = static_cast<H5P_genplist_t*>(
        H5I_object_verify(dcpl, H5I_GENPROP_LST));
    ASSERT_NE(dc_plist, nullptr);

    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    auto* fc_plist = static_cast<H5P_genplist_t*>(
        H5I_object_verify(fcpl, H5I_GENPROP_LST));
    ASSERT_NE(fc_plist, nullptr);

    ScopedPlist fapl_wrong(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl_wrong.valid());
    auto* fa_plist = static_cast<H5P_genplist_t*>(
        H5I_object_verify(fapl_wrong, H5I_GENPROP_LST));
    ASSERT_NE(fa_plist, nullptr);

    H5D_shared_t shared{};
    H5D_t fake_dset{};
    fake_dset.oloc = *H5G_oloc(g);
    fake_dset.shared = &shared;

    {
      ScopedApiContext ctx;
      ScopedErrorSilence silence;
      EXPECT_LT(H5D__layout_oh_read(&fake_dset, H5P_DEFAULT, dc_plist), 0);
    }

    {
      ScopedApiContext ctx;
      H5O_t* oh = H5O_pin(H5G_oloc(g));
      ASSERT_NE(oh, nullptr);
      absl::Cleanup unpin_oh = [oh] { H5O_unpin(oh); };
      EXPECT_GE(H5D__layout_oh_write(&fake_dset, oh, 0), 0);
    }

    // Create a chunked+deflated dataset and an EFL dataset to exercise
    // H5D__layout_oh_read rollback on pline, layout, efl, and ops->init, plus
    // H5D__layout_oh_write failure on invalid layout encoding.
    constexpr std::array<hsize_t, 1> kDims4 = {4};
    constexpr std::array<hsize_t, 1> kDims8 = {8};
    constexpr std::array<hsize_t, 1> kChunk2 = {2};
    ScopedSpace space4(H5Screate_simple(1, kDims4.data(), nullptr));
    ScopedSpace space8(H5Screate_simple(1, kDims8.data(), nullptr));
    ASSERT_TRUE(space4.valid());
    ASSERT_TRUE(space8.valid());

    ScopedPlist dcpl_filt(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_filt.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_filt, 1, kChunk2.data()), 0);
    ASSERT_GE(H5Pset_deflate(dcpl_filt, 5), 0);
    ScopedDataset d_filt(H5Dcreate2(file, "oh_filt", H5T_NATIVE_INT32, space4,
                                    H5P_DEFAULT, dcpl_filt, H5P_DEFAULT));
    ASSERT_TRUE(d_filt.valid());
    auto* real_filt =
        static_cast<H5D_t*>(H5VL_object_verify(d_filt, H5I_DATASET));
    ASSERT_NE(real_filt, nullptr);

    ScopedPlist dcpl_efl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_efl.valid());
    ASSERT_GE(H5Pset_external(dcpl_efl, "oh_efl_unused.bin", 0, 16), 0);
    ScopedDataset d_efl(H5Dcreate2(file, "oh_efl", H5T_NATIVE_INT32, space4,
                                   H5P_DEFAULT, dcpl_efl, H5P_DEFAULT));
    ASSERT_TRUE(d_efl.valid());
    auto* real_efl =
        static_cast<H5D_t*>(H5VL_object_verify(d_efl, H5I_DATASET));
    ASSERT_NE(real_efl, nullptr);

    {
      ScopedApiContext ctx;
      ScopedErrorSilence silence;

      // Missing pipeline property on fa_plist resets pline message (line 441).
      shared = H5D_shared_t{};
      fake_dset.oloc = real_filt->oloc;
      EXPECT_LT(H5D__layout_oh_read(&fake_dset, H5P_DEFAULT, fa_plist), 0);

      // Pipeline exists on fc_plist (via H5P_OBJECT_CREATE), so it advances
      // past pline and resets both pline and layout on further paths.
      shared = H5D_shared_t{};
      fake_dset.oloc = real_filt->oloc;
      EXPECT_LT(H5D__layout_oh_read(&fake_dset, H5P_DEFAULT, fc_plist), 0);

      // Missing EFL property on fc_plist resets layout and efl messages.
      shared = H5D_shared_t{};
      fake_dset.oloc = real_efl->oloc;
      EXPECT_LT(H5D__layout_oh_read(&fake_dset, H5P_DEFAULT, fc_plist), 0);

      // Valid dc_plist but oversized borrowed dataspace causes H5D__efl_init
      // failure in H5D__layout_oh_read and resets layout + efl messages.
      shared = H5D_shared_t{};
      shared.space = static_cast<H5S_t*>(
          H5I_object_verify(space8, H5I_DATASPACE));
      shared.type = static_cast<H5T_t*>(
          H5I_object_verify(H5T_NATIVE_INT32, H5I_DATATYPE));
      absl::Cleanup clear_borrowed = [&] {
        shared.space = nullptr;
        shared.type = nullptr;
      };
      EXPECT_LT(H5D__layout_oh_read(&fake_dset, H5P_DEFAULT, dc_plist), 0);

      // H5D__layout_oh_write failure on an existing dataset header whose layout
      // message is marked H5O_MSG_FLAG_CONSTANT (early-alloc contiguous).
      ScopedPlist dcpl_const(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl_const.valid());
      ASSERT_GE(H5Pset_alloc_time(dcpl_const, H5D_ALLOC_TIME_EARLY), 0);
      ScopedDataset d_const(H5Dcreate2(file, "oh_write_const",
                                       H5T_NATIVE_INT32, space4, H5P_DEFAULT,
                                       dcpl_const, H5P_DEFAULT));
      ASSERT_TRUE(d_const.valid());
      auto* real_const =
          static_cast<H5D_t*>(H5VL_object_verify(d_const, H5I_DATASET));
      ASSERT_NE(real_const, nullptr);
      H5O_t* oh_const = H5O_pin(&real_const->oloc);
      ASSERT_NE(oh_const, nullptr);
      absl::Cleanup unpin_const = [oh_const] { H5O_unpin(oh_const); };
      EXPECT_LT(H5D__layout_oh_write(real_const, oh_const, 0), 0);
    }
  }

  // 3. H5D__layout_oh_create error cleanup when ops->init fails and when
  //    alloc_time==EARLY fails after layout_init==true.
  {
    constexpr std::array<hsize_t, 1> kDims = {4};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset_id(H5Dcreate2(file, "for_oh_create", H5T_NATIVE_INT32,
                                     space, H5P_DEFAULT, H5P_DEFAULT,
                                     H5P_DEFAULT));
    ASSERT_TRUE(dset_id.valid());
    auto* real_dset =
        static_cast<H5D_t*>(H5VL_object_verify(dset_id, H5I_DATASET));
    ASSERT_NE(real_dset, nullptr);

    H5O_loc_t oh_loc{};
    {
      ScopedApiContext ctx;
      ASSERT_GE(H5O_create(f, 256, 0, H5P_DATASET_CREATE_DEFAULT, &oh_loc), 0);
    }
    absl::Cleanup close_oh_loc = [&] {
      ScopedApiContext ctx;
      H5O_close(&oh_loc, nullptr);
    };

    H5D_shared_t bad_shared = *real_dset->shared;
    H5D_t bad_dset = *real_dset;
    bad_dset.shared = &bad_shared;
    bad_dset.oloc = oh_loc;

    H5D_layout_ops_t custom_ops = *H5D_LOPS_CONTIG;
    custom_ops.init = FailChunkInit;
    bad_shared.layout.ops = &custom_ops;

    ScopedApiContext ctx;
    H5O_t* oh = H5O_pin(&oh_loc);
    ASSERT_NE(oh, nullptr);
    absl::Cleanup unpin_oh = [oh] { H5O_unpin(oh); };

    ScopedErrorSilence silence;
    EXPECT_LT(H5D__layout_oh_create(f, oh, &bad_dset, H5P_DEFAULT), 0);

    // Also trigger failure AFTER layout_init=true (early alloc of huge size ->
    // H5D__alloc_storage fails -> layout_ops->dest invoked).
    static int dest_calls = 0;
    dest_calls = 0;
    H5D_layout_ops_t dest_ops = *H5D_LOPS_CONTIG;
    dest_ops.dest = [](H5D_t* /*d*/) -> herr_t {
      ++dest_calls;
      return 0;
    };
    bad_shared.layout.ops = &dest_ops;
    bad_shared.dcpl_cache.fill.alloc_time = H5D_ALLOC_TIME_EARLY;
    bad_shared.layout.storage.u.contig.size = HADDR_MAX;
    bad_shared.layout.storage.u.contig.addr = HADDR_UNDEF;
    EXPECT_LT(H5D__layout_oh_create(f, oh, &bad_dset, H5P_DEFAULT), 0);
    EXPECT_EQ(dest_calls, 1);

    // Also trigger H5O_msg_append_oh(H5O_LAYOUT_ID) failure AFTER layout_init=true
    // with a failing ops->dest callback to cover both cleanup paths. Compact
    // layout size > 65535 fails in H5O__msg_alloc before copying layout.
    H5D_layout_ops_t bad_encode_ops = *H5D_LOPS_CONTIG;
    bad_encode_ops.init = [](H5F_t* /*file*/, H5D_t* d, hid_t /*dapl*/,
                             bool /*open_op*/) -> herr_t {
      d->shared->layout.type = H5D_COMPACT;
      d->shared->layout.version = H5O_LAYOUT_VERSION_3;
      d->shared->layout.storage.type = H5D_COMPACT;
      d->shared->layout.storage.u.compact.size = 70000;
      d->shared->layout.storage.u.compact.buf = nullptr;
      return 0;
    };
    bad_encode_ops.dest = [](H5D_t* /*d*/) -> herr_t { return -1; };
    bad_shared.layout.ops = &bad_encode_ops;
    bad_shared.dcpl_cache.fill.alloc_time = H5D_ALLOC_TIME_LATE;
    EXPECT_LT(H5D__layout_oh_create(f, oh, &bad_dset, H5P_DEFAULT), 0);

    // Oversized pline message (>65535 bytes) fails H5O_msg_append_oh(PLINE).
    {
      H5D_shared_t bad_pline_shared = *real_dset->shared;
      H5D_t bad_pline_dset = *real_dset;
      bad_pline_dset.shared = &bad_pline_shared;
      bad_pline_dset.oloc = oh_loc;
      std::vector<unsigned> big_cd(17000, 1);
      H5Z_filter_info_t big_filter{};
      big_filter.id = H5Z_FILTER_DEFLATE;
      big_filter.cd_nelmts = big_cd.size();
      big_filter.cd_values = big_cd.data();
      bad_pline_shared.layout.type = H5D_CHUNKED;
      bad_pline_shared.dcpl_cache.pline.version = H5O_PLINE_VERSION_2;
      bad_pline_shared.dcpl_cache.pline.nused = 1;
      bad_pline_shared.dcpl_cache.pline.nalloc = 1;
      bad_pline_shared.dcpl_cache.pline.filter = &big_filter;
      EXPECT_LT(H5D__layout_oh_create(f, oh, &bad_pline_dset, H5P_DEFAULT), 0);
    }

    // Oversized EFL message (>65535 bytes) fails H5O_msg_append_oh(EFL) after
    // local heap creation and insertion succeed.
    {
      H5D_shared_t bad_efl_shared = *real_dset->shared;
      H5D_t bad_efl_dset = *real_dset;
      bad_efl_dset.shared = &bad_efl_shared;
      bad_efl_dset.oloc = oh_loc;
      H5D_layout_ops_t efl_noop_ops = *H5D_LOPS_EFL;
      efl_noop_ops.init = nullptr;
      efl_noop_ops.dest = nullptr;
      std::vector<H5O_efl_entry_t> slots(3000);
      for (auto& slot : slots) {
        slot.name = const_cast<char*>("x");
        slot.size = 1;
      }
      bad_efl_shared.layout.type = H5D_CONTIGUOUS;
      bad_efl_shared.layout.ops = &efl_noop_ops;
      bad_efl_shared.dcpl_cache.fill.alloc_time = H5D_ALLOC_TIME_LATE;
      bad_efl_shared.dcpl_cache.efl.nused = slots.size();
      bad_efl_shared.dcpl_cache.efl.nalloc = slots.size();
      bad_efl_shared.dcpl_cache.efl.slot = slots.data();
      EXPECT_LT(H5D__layout_oh_create(f, oh, &bad_efl_dset, H5P_DEFAULT), 0);
    }

    // Read-only file triggers H5D__virtual_store_layout failure cleanly.
    {
      ScopedPlist ro_src_fapl = MakeCoreFapl();
      ASSERT_TRUE(ro_src_fapl.valid());
      ScopedFile ro_src(H5Fcreate("vds_ro_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                  ro_src_fapl));
      ASSERT_TRUE(ro_src.valid());
      ASSERT_GE(H5Fflush(ro_src, H5F_SCOPE_GLOBAL), 0);
      const ssize_t ro_len = H5Fget_file_image(ro_src, nullptr, 0);
      ASSERT_GT(ro_len, 0);
      std::vector<uint8_t> ro_img(static_cast<size_t>(ro_len), 0);
      ASSERT_EQ(H5Fget_file_image(ro_src, ro_img.data(), ro_img.size()),
                ro_len);
      ro_src.reset();

      ScopedPlist ro_fapl = MakeCoreFapl();
      ASSERT_TRUE(ro_fapl.valid());
      ASSERT_GE(H5Pset_file_image(ro_fapl, ro_img.data(), ro_img.size()), 0);
      ScopedFile ro_file(
          H5Fopen("vds_ro_open.h5", H5F_ACC_RDONLY, ro_fapl));
      ASSERT_TRUE(ro_file.valid());
      auto* ro_f =
          static_cast<H5F_t*>(H5VL_object_verify(ro_file, H5I_FILE));
      ASSERT_NE(ro_f, nullptr);

      H5D_shared_t bad_vds_shared = *real_dset->shared;
      H5D_t bad_vds_dset = *real_dset;
      bad_vds_dset.shared = &bad_vds_shared;
      bad_vds_dset.oloc = oh_loc;
      H5D_layout_ops_t vds_noop_ops = *H5D_LOPS_VIRTUAL;
      vds_noop_ops.init = nullptr;
      vds_noop_ops.dest = nullptr;
      H5O_storage_virtual_ent_t vent{};
      vent.source_file_name = const_cast<char*>("src.h5");
      vent.source_dset_name = const_cast<char*>("ds");
      vent.source_file_orig = SIZE_MAX;
      vent.source_dset_orig = SIZE_MAX;
      vent.source_select = real_dset->shared->space;
      vent.source_dset.virtual_select = real_dset->shared->space;
      bad_vds_shared.layout.type = H5D_VIRTUAL;
      bad_vds_shared.layout.storage.type = H5D_VIRTUAL;
      bad_vds_shared.layout.ops = &vds_noop_ops;
      bad_vds_shared.dcpl_cache.fill.alloc_time = H5D_ALLOC_TIME_LATE;
      bad_vds_shared.layout.storage.u.virt.serial_list_hobjid.addr =
          HADDR_UNDEF;
      bad_vds_shared.layout.storage.u.virt.list_nused = 1;
      bad_vds_shared.layout.storage.u.virt.list_nalloc = 1;
      bad_vds_shared.layout.storage.u.virt.list = &vent;
      EXPECT_LT(H5D__layout_oh_create(ro_f, oh, &bad_vds_dset, H5P_DEFAULT),
                0);
    }
  }
}

void RecomputeOhdrV2Checksum(std::vector<uint8_t>& image, size_t field_offset) {
  constexpr std::array<uint8_t, 4> kOhdrMagic = {'O', 'H', 'D', 'R'};
  size_t pos = field_offset;
  while (pos >= 4) {
    --pos;
    if (std::equal(kOhdrMagic.begin(), kOhdrMagic.end(), image.begin() + pos) &&
        pos + 6 < image.size() && image[pos + 4] == 2u) {
      const uint8_t flags = image[pos + 5];
      const size_t size_len = size_t{1} << (flags & 0x03u);
      size_t off = pos + 6;
      if (flags & 0x20u) off += 16;  // H5O_HDR_STORE_TIMES
      if (flags & 0x10u) off += 4;   // H5O_HDR_ATTR_STORE_PHASE_CHANGE
      ASSERT_LE(off + size_len, image.size());
      size_t chunk0_size = 0;
      for (size_t b = 0; b < size_len; ++b) {
        chunk0_size |= static_cast<size_t>(image[off + b]) << (8u * b);
      }
      const size_t csum_off = off + size_len + chunk0_size;
      ASSERT_LE(csum_off + 4, image.size());
      ASSERT_LT(field_offset, csum_off);
      const uint32_t csum =
          H5_checksum_metadata(image.data() + pos, csum_off - pos, 0);
      for (size_t b = 0; b < 4; ++b) {
        image[csum_off + b] = static_cast<uint8_t>((csum >> (8u * b)) & 0xFFu);
      }
      return;
    }
  }
  ADD_FAILURE() << "Could not find enclosing OHDR v2 header";
}

TEST(H5DlayoutTest, CorruptFileImageTriggersOhReadRollbackForPlineAndEfl) {
  // 1. Corrupt EFL slot size in file image so H5Dopen2 -> H5D__layout_oh_read
  //    reads layout + EFL then fails in H5D__efl_init and resets messages.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("corrupt_efl.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims = {4};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    // Unique external filename + recognizable slot size 0x4142 (16706 >= 16).
    ASSERT_GE(H5Pset_external(dcpl, "efl_magic_probe.bin", 0, 0x4142), 0);

    {
      ScopedDataset dset(H5Dcreate2(file, "efl", H5T_STD_U32LE, space,
                                    H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
    }
    ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);

    const ssize_t image_len = H5Fget_file_image(file, nullptr, 0);
    ASSERT_GT(image_len, 0);
    std::vector<uint8_t> image(static_cast<size_t>(image_len), 0);
    ASSERT_EQ(H5Fget_file_image(file, image.data(), image.size()), image_len);
    file.reset();

    // Locate the 8-byte little-endian size 0x0000000000004142 in the EFL
    // message and test both: (a) shrunk size (fails H5D__efl_init) and
    // (b) corrupted EFL version byte (fails H5O_msg_read(H5O_EFL_ID)).
    constexpr std::array<uint8_t, 8> kSizeNeedle = {
        0x42, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    auto it = std::search(image.begin(), image.end(), kSizeNeedle.begin(),
                          kSizeNeedle.end());
    ASSERT_NE(it, image.end());
    const size_t size_offset = static_cast<size_t>(it - image.begin());
    ASSERT_GE(size_offset, 32u);

    std::vector<uint8_t> image_bad_ver = image;
    ASSERT_EQ(image_bad_ver[size_offset - 32], 1u);
    ASSERT_EQ(image_bad_ver[size_offset - 31], 0u);
    ASSERT_EQ(image_bad_ver[size_offset - 30], 0u);
    ASSERT_EQ(image_bad_ver[size_offset - 29], 0u);
    image_bad_ver[size_offset - 32] = 9u;
    RecomputeOhdrV2Checksum(image_bad_ver, size_offset - 32);

    it[0] = 0x08;
    it[1] = 0x00;
    RecomputeOhdrV2Checksum(image, size_offset);

    ScopedPlist open_fapl = MakeCoreFapl();
    ASSERT_TRUE(open_fapl.valid());
    ASSERT_GE(H5Pset_file_image(open_fapl, image.data(), image.size()), 0);
    ScopedFile reopened(
        H5Fopen("corrupt_efl_open.h5", H5F_ACC_RDONLY, open_fapl));
    ASSERT_TRUE(reopened.valid());

    ScopedErrorSilence silence;
    EXPECT_EQ(H5Dopen2(reopened, "efl", H5P_DEFAULT), H5I_INVALID_HID);

    ScopedPlist ver_fapl = MakeCoreFapl();
    ASSERT_TRUE(ver_fapl.valid());
    ASSERT_GE(
        H5Pset_file_image(ver_fapl, image_bad_ver.data(), image_bad_ver.size()),
        0);
    ScopedFile reopened_bad_ver(
        H5Fopen("corrupt_efl_ver.h5", H5F_ACC_RDONLY, ver_fapl));
    ASSERT_TRUE(reopened_bad_ver.valid());
    EXPECT_EQ(H5Dopen2(reopened_bad_ver, "efl", H5P_DEFAULT),
              H5I_INVALID_HID);
  }

  // 1b. Corrupt PLINE message version byte in file image so
  //     H5O_msg_read(H5O_PLINE_ID) fails in H5D__layout_oh_read.
  {
    constexpr H5Z_filter_t kProbeFilterId = 314;
    const H5Z_class2_t probe_cls = {
        H5Z_CLASS_T_VERS,
        kProbeFilterId,
        1,
        1,
        "h5dlayout_pline_probe",
        nullptr,
        nullptr,
        [](unsigned int /*flags*/, size_t /*cd_nelmts*/,
           const unsigned int /*cd_values*/[], size_t nbytes,
           size_t* buf_size, void** /*buf*/) -> size_t {
          *buf_size = nbytes;
          return nbytes;
        },
    };
    ASSERT_GE(H5Zregister(&probe_cls), 0);
    absl::Cleanup unreg_probe = [] { H5Zunregister(kProbeFilterId); };

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("corrupt_pline_ver.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());
    constexpr std::array<hsize_t, 1> kDims = {8};
    constexpr std::array<hsize_t, 1> kChunk = {4};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl, 1, kChunk.data()), 0);
    ASSERT_GE(
        H5Pset_filter(dcpl, kProbeFilterId, H5Z_FLAG_OPTIONAL, 0, nullptr), 0);

    {
      ScopedDataset dset(H5Dcreate2(file, "pl", H5T_STD_U32LE, space,
                                    H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
    }
    ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);

    const ssize_t image_len = H5Fget_file_image(file, nullptr, 0);
    ASSERT_GT(image_len, 0);
    std::vector<uint8_t> image(static_cast<size_t>(image_len), 0);
    ASSERT_EQ(H5Fget_file_image(file, image.data(), image.size()), image_len);
    file.reset();

    const std::string needle = "h5dlayout_pline_probe";
    auto it = std::search(image.begin(), image.end(), needle.begin(),
                          needle.end());
    ASSERT_NE(it, image.end());
    const size_t name_off = static_cast<size_t>(it - image.begin());
    ASSERT_GE(name_off, 10u);
    ASSERT_EQ(image[name_off - 10], 2u);
    ASSERT_EQ(image[name_off - 9], 1u);
    image[name_off - 10] = 99u;
    RecomputeOhdrV2Checksum(image, name_off - 10);

    ScopedPlist open_fapl = MakeCoreFapl();
    ASSERT_TRUE(open_fapl.valid());
    ASSERT_GE(H5Pset_file_image(open_fapl, image.data(), image.size()), 0);
    ScopedFile reopened(
        H5Fopen("corrupt_pline_open.h5", H5F_ACC_RDONLY, open_fapl));
    ASSERT_TRUE(reopened.valid());

    ScopedErrorSilence silence;
    EXPECT_EQ(H5Dopen2(reopened, "pl", H5P_DEFAULT), H5I_INVALID_HID);
  }

  // 2. Corrupt a 2D filtered chunked dataset's chunk dim to 0 in a v3 layout
  //    message so H5Dopen2 -> H5D__layout_oh_read reads pline + layout then
  //    fails H5D__chunk_init and resets both messages.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("corrupt_chunk_pline.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 2> kDimsLarge = {64, 64};
    constexpr std::array<hsize_t, 2> kChunkUnique = {37, 41};
    ScopedSpace space_large(
        H5Screate_simple(2, kDimsLarge.data(), nullptr));
    ASSERT_TRUE(space_large.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunkUnique.data()), 0);
    ASSERT_GE(H5Pset_deflate(dcpl, 1), 0);

    {
      ScopedDataset dset(H5Dcreate2(file, "chk", H5T_STD_U32LE, space_large,
                                    H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
    }
    ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);

    const ssize_t image_len = H5Fget_file_image(file, nullptr, 0);
    ASSERT_GT(image_len, 0);
    std::vector<uint8_t> image(static_cast<size_t>(image_len), 0);
    ASSERT_EQ(H5Fget_file_image(file, image.data(), image.size()), image_len);
    file.reset();

    // v3 chunked layout encodes 3 dimensions (ndims + 1 for datatype size):
    // [37, 41, 4] as uint32_t little-endian.
    constexpr std::array<uint8_t, 12> kChunkNeedle = {
        37, 0, 0, 0, 41, 0, 0, 0, 4, 0, 0, 0};
    auto it = std::search(image.begin(), image.end(), kChunkNeedle.begin(),
                          kChunkNeedle.end());
    ASSERT_NE(it, image.end());
    const size_t chunk_off = static_cast<size_t>(it - image.begin());
    it[0] = 0;
    RecomputeOhdrV2Checksum(image, chunk_off);

    ScopedPlist open_fapl = MakeCoreFapl();
    ASSERT_TRUE(open_fapl.valid());
    ASSERT_GE(H5Pset_file_image(open_fapl, image.data(), image.size()), 0);
    ScopedFile reopened(
        H5Fopen("corrupt_chunk_open.h5", H5F_ACC_RDONLY, open_fapl));
    ASSERT_TRUE(reopened.valid());

    ScopedErrorSilence silence;
    EXPECT_EQ(H5Dopen2(reopened, "chk", H5P_DEFAULT), H5I_INVALID_HID);
  }
}

}  // namespace
}  // namespace hdf5
