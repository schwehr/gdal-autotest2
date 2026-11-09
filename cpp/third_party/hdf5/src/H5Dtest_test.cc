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
// Tests internal H5D layout, chunk index, compact dirty-flag, and chunk cache
// inspection functions (third_party/hdf5/src/H5Dtest.c). Deflate (zlib) is
// enabled in the default google3 build.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dtest.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

// `:hdf5` compiles with -DNDEBUG, which omits `#ifndef NDEBUG` fields from
// internal metadata cache structs pulled in by H5Dpkg.h -> H5ACprivate.h ->
// H5Cprivate.h. Match `:hdf5` struct layouts across the prelude and H5Dpkg.h.
#ifndef NDEBUG
#define H5DTEST_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#pragma push_macro("try")
#undef try
#define try h5b2_try_param
#define H5D_FRIEND
#define H5D_TESTING
#include "third_party/hdf5/src/H5Dpkg.h"
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DTEST_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DTEST_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

ScopedFile CreateCoreFileWithBounds(const char* name, H5F_libver_t low,
                                    H5F_libver_t high) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_libver_bounds(fapl, low, high) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(name, H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
}

TEST(H5DtestTest, LayoutVersionReflectsLibverBoundsAndLayout) {
  // 1. V18 bounds keep contiguous, compact, and chunked layouts at version 3.
  {
    ScopedFile file = CreateCoreFileWithBounds(
        "v18.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_V18);
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims = {4};
    constexpr std::array<hsize_t, 1> kChunkDims = {2};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    ScopedDataset contig(H5Dcreate2(file, "contig", H5T_NATIVE_INT32, space,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(contig.valid());
    unsigned version = 0;
    EXPECT_GE(H5D__layout_version_test(contig, &version), 0);
    EXPECT_EQ(version, static_cast<unsigned>(H5O_LAYOUT_VERSION_3));

    // Passing nullptr for `version` must succeed without error.
    EXPECT_GE(H5D__layout_version_test(contig, nullptr), 0);

    ScopedPlist compact_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(compact_dcpl.valid());
    ASSERT_GE(H5Pset_layout(compact_dcpl, H5D_COMPACT), 0);
    ScopedDataset compact(H5Dcreate2(file, "compact", H5T_NATIVE_INT32, space,
                                     H5P_DEFAULT, compact_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(compact.valid());
    version = 0;
    EXPECT_GE(H5D__layout_version_test(compact, &version), 0);
    EXPECT_EQ(version, static_cast<unsigned>(H5O_LAYOUT_VERSION_3));

    ScopedPlist chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(chunk_dcpl.valid());
    ASSERT_GE(H5Pset_chunk(chunk_dcpl, 1, kChunkDims.data()), 0);
    ScopedDataset chunked(H5Dcreate2(file, "chunked", H5T_NATIVE_INT32, space,
                                     H5P_DEFAULT, chunk_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(chunked.valid());
    version = 0;
    EXPECT_GE(H5D__layout_version_test(chunked, &version), 0);
    EXPECT_EQ(version, static_cast<unsigned>(H5O_LAYOUT_VERSION_3));
  }

  // 2. V110+ bounds keep contiguous/compact at v3 and upgrade chunked/VDS to v4.
  {
    ScopedFile file = CreateCoreFileWithBounds(
        "v110.h5", H5F_LIBVER_V110, H5F_LIBVER_LATEST);
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims = {4};
    constexpr std::array<hsize_t, 1> kChunkDims = {2};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    ScopedDataset contig(H5Dcreate2(file, "contig", H5T_NATIVE_INT32, space,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(contig.valid());
    unsigned version = 0;
    EXPECT_GE(H5D__layout_version_test(contig, &version), 0);
    EXPECT_EQ(version, static_cast<unsigned>(H5O_LAYOUT_VERSION_3));

    ScopedPlist chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(chunk_dcpl.valid());
    ASSERT_GE(H5Pset_chunk(chunk_dcpl, 1, kChunkDims.data()), 0);
    ScopedDataset chunked(H5Dcreate2(file, "chunked", H5T_NATIVE_INT32, space,
                                     H5P_DEFAULT, chunk_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(chunked.valid());
    version = 0;
    EXPECT_GE(H5D__layout_version_test(chunked, &version), 0);
    EXPECT_EQ(version, static_cast<unsigned>(H5O_LAYOUT_VERSION_4));

    ScopedPlist vds_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(vds_dcpl.valid());
    ASSERT_GE(H5Pset_virtual(vds_dcpl, space, ".", "/contig", space), 0);
    ScopedDataset virt(H5Dcreate2(file, "virt", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, vds_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(virt.valid());
    version = 0;
    EXPECT_GE(H5D__layout_version_test(virt, &version), 0);
    EXPECT_EQ(version, static_cast<unsigned>(H5O_LAYOUT_VERSION_4));
  }

  // 3. V200 bounds + filter pipeline upgrade chunked layout to version 5.
  {
    ScopedFile file = CreateCoreFileWithBounds(
        "v200.h5", H5F_LIBVER_V200, H5F_LIBVER_LATEST);
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims = {8};
    constexpr std::array<hsize_t, 1> kChunkDims = {4};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl, 1, kChunkDims.data()), 0);
    ASSERT_GE(H5Pset_deflate(dcpl, 6), 0);
    ScopedDataset filtered(H5Dcreate2(file, "filtered", H5T_NATIVE_INT32, space,
                                      H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(filtered.valid());
    unsigned version = 0;
    EXPECT_GE(H5D__layout_version_test(filtered, &version), 0);
    EXPECT_EQ(version, static_cast<unsigned>(H5O_LAYOUT_VERSION_5));
  }

  // 4. Reject invalid or non-dataset identifiers.
  {
    ScopedFile file = CreateInMemoryFile("invalid_ver.h5");
    ASSERT_TRUE(file.valid());
    ScopedSpace space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());

    ScopedErrorSilence silence;
    unsigned version = 999;
    EXPECT_LT(H5D__layout_version_test(H5I_INVALID_HID, &version), 0);
    EXPECT_LT(H5D__layout_version_test(file, &version), 0);
    EXPECT_LT(H5D__layout_version_test(space, &version), 0);
    EXPECT_EQ(version, 999u);
  }
}

TEST(H5DtestTest, LayoutTypeMatchesAllDatasetLayouts) {
  ScopedFile file = CreateCoreFileWithBounds(
      "layout_types.h5", H5F_LIBVER_V110, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {6};
  constexpr std::array<hsize_t, 1> kChunkDims = {3};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset contig(H5Dcreate2(file, "contig", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(contig.valid());

  ScopedPlist compact_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(compact_dcpl.valid());
  ASSERT_GE(H5Pset_layout(compact_dcpl, H5D_COMPACT), 0);
  ScopedDataset compact(H5Dcreate2(file, "compact", H5T_NATIVE_INT32, space,
                                   H5P_DEFAULT, compact_dcpl, H5P_DEFAULT));
  ASSERT_TRUE(compact.valid());

  ScopedPlist chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(chunk_dcpl.valid());
  ASSERT_GE(H5Pset_chunk(chunk_dcpl, 1, kChunkDims.data()), 0);
  ScopedDataset chunked(H5Dcreate2(file, "chunked", H5T_NATIVE_INT32, space,
                                   H5P_DEFAULT, chunk_dcpl, H5P_DEFAULT));
  ASSERT_TRUE(chunked.valid());

  ScopedPlist vds_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(vds_dcpl.valid());
  ASSERT_GE(H5Pset_virtual(vds_dcpl, space, ".", "/contig", space), 0);
  ScopedDataset virt(H5Dcreate2(file, "virt", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, vds_dcpl, H5P_DEFAULT));
  ASSERT_TRUE(virt.valid());

  struct ExpectedLayout {
    const char* name;
    hid_t id;
    H5D_layout_t expected;
  };
  const std::array<ExpectedLayout, 4> kCases = {{
      {.name = "contig", .id = contig, .expected = H5D_CONTIGUOUS},
      {.name = "compact", .id = compact, .expected = H5D_COMPACT},
      {.name = "chunked", .id = chunked, .expected = H5D_CHUNKED},
      {.name = "virt", .id = virt, .expected = H5D_VIRTUAL},
  }};

  for (const auto& tc : kCases) {
    H5D_layout_t layout_type = H5D_LAYOUT_ERROR;
    EXPECT_GE(H5D__layout_type_test(tc.id, &layout_type), 0);
    EXPECT_EQ(layout_type, tc.expected) << tc.name;
  }

  // Close dataset handles and reopen each to verify decoded layout types.
  virt.reset();
  chunked.reset();
  compact.reset();
  contig.reset();
  ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);

  for (const auto& tc : kCases) {
    ScopedDataset reopened(H5Dopen2(file, tc.name, H5P_DEFAULT));
    ASSERT_TRUE(reopened.valid()) << tc.name;
    H5D_layout_t layout_type = H5D_LAYOUT_ERROR;
    EXPECT_GE(H5D__layout_type_test(reopened, &layout_type), 0);
    EXPECT_EQ(layout_type, tc.expected) << tc.name;
  }

  ScopedErrorSilence silence;
  H5D_layout_t layout_type = H5D_NLAYOUTS;
  EXPECT_LT(H5D__layout_type_test(H5I_INVALID_HID, &layout_type), 0);
  EXPECT_LT(H5D__layout_type_test(file, &layout_type), 0);
  EXPECT_EQ(layout_type, H5D_NLAYOUTS);
}

TEST(H5DtestTest, LayoutContigSizeReportsExpectedByteCounts) {
  ScopedFile file = CreateInMemoryFile("contig_sizes.h5");
  ASSERT_TRUE(file.valid());

  // 1. 1D contiguous dataset.
  constexpr std::array<hsize_t, 1> kDims1d = {5};
  ScopedSpace space1d(H5Screate_simple(1, kDims1d.data(), nullptr));
  ASSERT_TRUE(space1d.valid());
  ScopedDataset dset1d(H5Dcreate2(file, "d1", H5T_NATIVE_INT32, space1d,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset1d.valid());
  hsize_t size = 0;
  EXPECT_GE(H5D__layout_contig_size_test(dset1d, &size), 0);
  EXPECT_EQ(size, 5 * sizeof(int32_t));

  // Passing nullptr for `size` must succeed without error.
  EXPECT_GE(H5D__layout_contig_size_test(dset1d, nullptr), 0);

  // 2. 2D contiguous dataset.
  constexpr std::array<hsize_t, 2> kDims2d = {3, 4};
  ScopedSpace space2d(H5Screate_simple(2, kDims2d.data(), nullptr));
  ASSERT_TRUE(space2d.valid());
  ScopedDataset dset2d(H5Dcreate2(file, "d2", H5T_NATIVE_DOUBLE, space2d,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset2d.valid());
  size = 0;
  EXPECT_GE(H5D__layout_contig_size_test(dset2d, &size), 0);
  EXPECT_EQ(size, 12 * sizeof(double));

  // 3. Scalar and zero-element contiguous datasets.
  ScopedSpace scalar_space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar_space.valid());
  ScopedDataset scalar_dset(H5Dcreate2(file, "scalar", H5T_NATIVE_UINT16,
                                       scalar_space, H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
  ASSERT_TRUE(scalar_dset.valid());
  size = 0;
  EXPECT_GE(H5D__layout_contig_size_test(scalar_dset, &size), 0);
  EXPECT_EQ(size, sizeof(uint16_t));

  constexpr std::array<hsize_t, 1> kZeroDims = {0};
  ScopedSpace zero_space(H5Screate_simple(1, kZeroDims.data(), nullptr));
  ASSERT_TRUE(zero_space.valid());
  ScopedDataset zero_dset(H5Dcreate2(file, "zero", H5T_NATIVE_INT32, zero_space,
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(zero_dset.valid());
  size = 999;
  EXPECT_GE(H5D__layout_contig_size_test(zero_dset, &size), 0);
  EXPECT_EQ(size, 0u);

  // 4. Reject non-dataset identifiers.
  ScopedErrorSilence silence;
  size = 777;
  EXPECT_LT(H5D__layout_contig_size_test(H5I_INVALID_HID, &size), 0);
  EXPECT_LT(H5D__layout_contig_size_test(file, &size), 0);
  EXPECT_EQ(size, 777u);
}

TEST(H5DtestTest, LayoutCompactDirtyTracksCreationWriteAndFlush) {
  ScopedFile file = CreateInMemoryFile("compact_dirty.h5");
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_layout(dcpl, H5D_COMPACT), 0);

  // 1. Newly allocated non-empty compact buffer starts dirty.
  ScopedDataset dset(H5Dcreate2(file, "compact", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  bool dirty = false;
  EXPECT_GE(H5D__layout_compact_dirty_test(dset, &dirty), 0);
  EXPECT_TRUE(dirty);

  // Passing nullptr for `dirty` must succeed.
  EXPECT_GE(H5D__layout_compact_dirty_test(dset, nullptr), 0);

  // 2. Flushing the dataset clears the compact dirty flag.
  ASSERT_GE(H5Dflush(dset), 0);
  EXPECT_GE(H5D__layout_compact_dirty_test(dset, &dirty), 0);
  EXPECT_FALSE(dirty);

  // 3. Writing to the compact dataset marks it dirty again until flushed.
  constexpr std::array<int32_t, 4> kInput = {11, 22, 33, 44};
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     kInput.data()),
            0);
  EXPECT_GE(H5D__layout_compact_dirty_test(dset, &dirty), 0);
  EXPECT_TRUE(dirty);

  ASSERT_GE(H5Dflush(dset), 0);
  EXPECT_GE(H5D__layout_compact_dirty_test(dset, &dirty), 0);
  EXPECT_FALSE(dirty);

  // 4. Reopening the compact dataset starts clean and preserves written data.
  dset.reset();
  ScopedDataset reopened(H5Dopen2(file, "compact", H5P_DEFAULT));
  ASSERT_TRUE(reopened.valid());
  dirty = true;
  EXPECT_GE(H5D__layout_compact_dirty_test(reopened, &dirty), 0);
  EXPECT_FALSE(dirty);

  std::array<int32_t, 4> output = {0, 0, 0, 0};
  ASSERT_GE(H5Dread(reopened, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    output.data()),
            0);
  EXPECT_THAT(output, ElementsAre(11, 22, 33, 44));

  // 5. Zero-element compact dataset has size 0 and starts clean.
  constexpr std::array<hsize_t, 1> kZeroDims = {0};
  ScopedSpace zero_space(H5Screate_simple(1, kZeroDims.data(), nullptr));
  ASSERT_TRUE(zero_space.valid());
  ScopedDataset zero_dset(H5Dcreate2(file, "empty_compact", H5T_NATIVE_INT32,
                                     zero_space, H5P_DEFAULT, dcpl,
                                     H5P_DEFAULT));
  ASSERT_TRUE(zero_dset.valid());
  dirty = true;
  EXPECT_GE(H5D__layout_compact_dirty_test(zero_dset, &dirty), 0);
  EXPECT_FALSE(dirty);

  // 6. Reject invalid or non-dataset identifiers.
  ScopedErrorSilence silence;
  dirty = true;
  EXPECT_LT(H5D__layout_compact_dirty_test(H5I_INVALID_HID, &dirty), 0);
  EXPECT_LT(H5D__layout_compact_dirty_test(file, &dirty), 0);
  EXPECT_TRUE(dirty);
}

TEST(H5DtestTest, LayoutIdxTypeCoversAllChunkIndexStrategies) {
  struct IndexCase {
    const char* name;
    H5F_libver_t low;
    H5F_libver_t high;
    std::array<hsize_t, 2> dims;
    std::array<hsize_t, 2> maxdims;
    std::array<hsize_t, 2> chunks;
    H5D_alloc_time_t alloc_time;
    H5D_chunk_index_t expected_idx;
  };

  constexpr std::array<IndexCase, 6> kCases = {{
      {.name = "btree_v1",
       .low = H5F_LIBVER_EARLIEST,
       .high = H5F_LIBVER_V18,
       .dims = {4, 4},
       .maxdims = {4, 4},
       .chunks = {2, 2},
       .alloc_time = H5D_ALLOC_TIME_DEFAULT,
       .expected_idx = H5D_CHUNK_IDX_BTREE},
      {.name = "single",
       .low = H5F_LIBVER_V110,
       .high = H5F_LIBVER_LATEST,
       .dims = {4, 4},
       .maxdims = {4, 4},
       .chunks = {4, 4},
       .alloc_time = H5D_ALLOC_TIME_DEFAULT,
       .expected_idx = H5D_CHUNK_IDX_SINGLE},
      {.name = "none_implicit",
       .low = H5F_LIBVER_V110,
       .high = H5F_LIBVER_LATEST,
       .dims = {8, 4},
       .maxdims = {8, 4},
       .chunks = {4, 4},
       .alloc_time = H5D_ALLOC_TIME_EARLY,
       .expected_idx = H5D_CHUNK_IDX_NONE},
      {.name = "farray",
       .low = H5F_LIBVER_V110,
       .high = H5F_LIBVER_LATEST,
       .dims = {8, 4},
       .maxdims = {8, 4},
       .chunks = {4, 4},
       .alloc_time = H5D_ALLOC_TIME_INCR,
       .expected_idx = H5D_CHUNK_IDX_FARRAY},
      {.name = "earray",
       .low = H5F_LIBVER_V110,
       .high = H5F_LIBVER_LATEST,
       .dims = {4, 4},
       .maxdims = {H5S_UNLIMITED, 4},
       .chunks = {4, 4},
       .alloc_time = H5D_ALLOC_TIME_DEFAULT,
       .expected_idx = H5D_CHUNK_IDX_EARRAY},
      {.name = "btree_v2",
       .low = H5F_LIBVER_V110,
       .high = H5F_LIBVER_LATEST,
       .dims = {4, 4},
       .maxdims = {H5S_UNLIMITED, H5S_UNLIMITED},
       .chunks = {4, 4},
       .alloc_time = H5D_ALLOC_TIME_DEFAULT,
       .expected_idx = H5D_CHUNK_IDX_BT2},
  }};

  for (const auto& tc : kCases) {
    ScopedFile file = CreateCoreFileWithBounds(tc.name, tc.low, tc.high);
    ASSERT_TRUE(file.valid()) << tc.name;

    ScopedSpace space(
        H5Screate_simple(2, tc.dims.data(), tc.maxdims.data()));
    ASSERT_TRUE(space.valid()) << tc.name;

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid()) << tc.name;
    ASSERT_GE(H5Pset_chunk(dcpl, 2, tc.chunks.data()), 0) << tc.name;
    if (tc.alloc_time != H5D_ALLOC_TIME_DEFAULT) {
      ASSERT_GE(H5Pset_alloc_time(dcpl, tc.alloc_time), 0) << tc.name;
    }

    ScopedDataset dset(H5Dcreate2(file, "dset", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid()) << tc.name;

    H5D_chunk_index_t idx_type = H5D_CHUNK_IDX_NTYPES;
    EXPECT_GE(H5D__layout_idx_type_test(dset, &idx_type), 0) << tc.name;
    EXPECT_EQ(idx_type, tc.expected_idx) << tc.name;

    // Passing nullptr for `idx_type` must succeed.
    EXPECT_GE(H5D__layout_idx_type_test(dset, nullptr), 0) << tc.name;
  }

  // Verify H5Dformat_convert downgrades a v4 chunked index to v1 B-tree.
  {
    ScopedFile file = CreateCoreFileWithBounds(
        "format_convert.h5", H5F_LIBVER_V110, H5F_LIBVER_LATEST);
    ASSERT_TRUE(file.valid());
    constexpr std::array<hsize_t, 2> kDims = {8, 4};
    constexpr std::array<hsize_t, 2> kChunks = {4, 4};
    ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunks.data()), 0);
    ScopedDataset dset(H5Dcreate2(file, "conv", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    H5D_chunk_index_t idx_type = H5D_CHUNK_IDX_NTYPES;
    ASSERT_GE(H5D__layout_idx_type_test(dset, &idx_type), 0);
    ASSERT_EQ(idx_type, H5D_CHUNK_IDX_FARRAY);

    ASSERT_GE(H5Dformat_convert(dset), 0);
    unsigned version = 0;
    EXPECT_GE(H5D__layout_version_test(dset, &version), 0);
    EXPECT_EQ(version, static_cast<unsigned>(H5O_LAYOUT_VERSION_3));
    dset.reset();
    ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);

    // Reopen from serialized image bytes so H5O_layout_decode re-parses the
    // downgraded v3 layout message (which clears the cached v4 idx_type).
    const ssize_t image_len = H5Fget_file_image(file, nullptr, 0);
    ASSERT_GT(image_len, 0);
    std::vector<uint8_t> image(static_cast<size_t>(image_len));
    ASSERT_EQ(H5Fget_file_image(file, image.data(), image.size()), image_len);
    file.reset();

    ScopedPlist ro_fapl = MakeCoreFapl();
    ASSERT_TRUE(ro_fapl.valid());
    ASSERT_GE(H5Pset_file_image(ro_fapl, image.data(), image.size()), 0);
    ScopedFile ro_file(H5Fopen("reopened_conv.h5", H5F_ACC_RDONLY, ro_fapl));
    ASSERT_TRUE(ro_file.valid());
    ScopedDataset reopened(H5Dopen2(ro_file, "conv", H5P_DEFAULT));
    ASSERT_TRUE(reopened.valid());

    version = 0;
    EXPECT_GE(H5D__layout_version_test(reopened, &version), 0);
    EXPECT_EQ(version, static_cast<unsigned>(H5O_LAYOUT_VERSION_3));
    EXPECT_GE(H5D__layout_idx_type_test(reopened, &idx_type), 0);
    EXPECT_EQ(idx_type, H5D_CHUNK_IDX_BTREE);
  }

  // Non-chunked datasets and invalid IDs must fail.
  {
    ScopedFile file = CreateCoreFileWithBounds(
        "non_chunked.h5", H5F_LIBVER_V110, H5F_LIBVER_LATEST);
    ASSERT_TRUE(file.valid());
    constexpr std::array<hsize_t, 1> kDims = {4};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    ScopedDataset contig(H5Dcreate2(file, "contig", H5T_NATIVE_INT32, space,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(contig.valid());

    ScopedPlist compact_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(compact_dcpl.valid());
    ASSERT_GE(H5Pset_layout(compact_dcpl, H5D_COMPACT), 0);
    ScopedDataset compact(H5Dcreate2(file, "compact", H5T_NATIVE_INT32, space,
                                     H5P_DEFAULT, compact_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(compact.valid());

    ScopedPlist vds_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(vds_dcpl.valid());
    ASSERT_GE(H5Pset_virtual(vds_dcpl, space, ".", "/contig", space), 0);
    ScopedDataset virt(H5Dcreate2(file, "virt", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, vds_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(virt.valid());

    ScopedErrorSilence silence;
    H5D_chunk_index_t idx_type = H5D_CHUNK_IDX_NTYPES;
    EXPECT_LT(H5D__layout_idx_type_test(contig, &idx_type), 0);
    EXPECT_LT(H5D__layout_idx_type_test(compact, &idx_type), 0);
    EXPECT_LT(H5D__layout_idx_type_test(virt, &idx_type), 0);
    EXPECT_LT(H5D__layout_idx_type_test(H5I_INVALID_HID, &idx_type), 0);
    EXPECT_LT(H5D__layout_idx_type_test(file, &idx_type), 0);
    EXPECT_EQ(idx_type, H5D_CHUNK_IDX_NTYPES);
  }
}

TEST(H5DtestTest, CurrentCacheSizeTracksChunkCachingAndEviction) {
  ScopedFile file = CreateInMemoryFile("chunk_cache.h5");
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kTotalElems = 16;
  constexpr hsize_t kChunkElems = 4;
  constexpr size_t kChunkBytes = kChunkElems * sizeof(int32_t);
  constexpr std::array<hsize_t, 1> kDims = {kTotalElems};
  constexpr std::array<hsize_t, 1> kChunkDims = {kChunkElems};

  ScopedSpace fspace(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(fspace.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl, 1, kChunkDims.data()), 0);

  // Configure raw-data chunk cache to hold at most 2 chunks (32 bytes).
  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl.valid());
  ASSERT_GE(H5Pset_chunk_cache(dapl, 13, 2 * kChunkBytes, 0.75), 0);

  ScopedDataset dset(H5Dcreate2(file, "cached", H5T_NATIVE_INT32, fspace,
                                H5P_DEFAULT, dcpl, dapl));
  ASSERT_TRUE(dset.valid());

  // 1. Cache is initially empty before any raw-data I/O.
  size_t nbytes_used = 999;
  int nused = 999;
  EXPECT_GE(H5D__current_cache_size_test(dset, &nbytes_used, &nused), 0);
  EXPECT_EQ(nbytes_used, 0u);
  EXPECT_EQ(nused, 0);

  // Write 2 elements into chunk 0, then 2 elements into chunk 1.
  constexpr std::array<hsize_t, 1> kCount = {2};
  ScopedSpace mspace(H5Screate_simple(1, kCount.data(), nullptr));
  ASSERT_TRUE(mspace.valid());
  constexpr std::array<int32_t, 2> kVals = {10, 20};

  auto write_partial_chunk = [&](hsize_t start) {
    ASSERT_GE(H5Sselect_hyperslab(fspace, H5S_SELECT_SET, &start, nullptr,
                                  kCount.data(), nullptr),
              0);
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, mspace, fspace, H5P_DEFAULT,
                       kVals.data()),
              0);
  };

  // 2. Caching chunk 0 uses 1 slot and 16 bytes.
  write_partial_chunk(0);
  EXPECT_GE(H5D__current_cache_size_test(dset, &nbytes_used, &nused), 0);
  EXPECT_EQ(nused, 1);
  EXPECT_EQ(nbytes_used, kChunkBytes);

  // 3. Caching chunk 1 uses 2 slots and 32 bytes.
  write_partial_chunk(4);
  EXPECT_GE(H5D__current_cache_size_test(dset, &nbytes_used, &nused), 0);
  EXPECT_EQ(nused, 2);
  EXPECT_EQ(nbytes_used, 2 * kChunkBytes);

  // 4. Exercise individual non-null / null out-parameter combinations.
  size_t only_bytes = 0;
  int only_nused = 0;
  EXPECT_GE(H5D__current_cache_size_test(dset, &only_bytes, nullptr), 0);
  EXPECT_EQ(only_bytes, 2 * kChunkBytes);
  EXPECT_GE(H5D__current_cache_size_test(dset, nullptr, &only_nused), 0);
  EXPECT_EQ(only_nused, 2);
  EXPECT_GE(H5D__current_cache_size_test(dset, nullptr, nullptr), 0);

  // 5. Accessing chunks 2 and 3 evicts older entries within the 2-chunk cap.
  write_partial_chunk(8);
  write_partial_chunk(12);
  EXPECT_GE(H5D__current_cache_size_test(dset, &nbytes_used, &nused), 0);
  EXPECT_GT(nused, 0);
  EXPECT_LE(nused, 2);
  EXPECT_EQ(nbytes_used, static_cast<size_t>(nused) * kChunkBytes);

  // 6. Reopening resets chunk cache; reading one element populates 1 slot.
  dset.reset();
  ScopedDataset reopened(H5Dopen2(file, "cached", dapl));
  ASSERT_TRUE(reopened.valid());
  EXPECT_GE(H5D__current_cache_size_test(reopened, &nbytes_used, &nused), 0);
  EXPECT_EQ(nused, 0);
  EXPECT_EQ(nbytes_used, 0u);

  constexpr std::array<hsize_t, 1> kOne = {1};
  constexpr std::array<hsize_t, 1> kZeroStart = {0};
  ScopedSpace one_space(H5Screate_simple(1, kOne.data(), nullptr));
  ASSERT_TRUE(one_space.valid());
  ASSERT_GE(H5Sselect_hyperslab(fspace, H5S_SELECT_SET, kZeroStart.data(),
                                nullptr, kOne.data(), nullptr),
            0);
  int32_t read_val = 0;
  ASSERT_GE(H5Dread(reopened, H5T_NATIVE_INT32, one_space, fspace, H5P_DEFAULT,
                    &read_val),
            0);
  EXPECT_EQ(read_val, 10);
  EXPECT_GE(H5D__current_cache_size_test(reopened, &nbytes_used, &nused), 0);
  EXPECT_EQ(nused, 1);
  EXPECT_EQ(nbytes_used, kChunkBytes);

  // 7. Unfiltered dataset with 0-byte chunk cache caches nothing across I/O.
  ScopedPlist no_cache_dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(no_cache_dapl.valid());
  ASSERT_GE(H5Pset_chunk_cache(no_cache_dapl, 0, 0, 0.0), 0);
  ScopedSpace full_space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(full_space.valid());
  ScopedDataset uncached(H5Dcreate2(file, "uncached", H5T_NATIVE_INT32,
                                    full_space, H5P_DEFAULT, dcpl,
                                    no_cache_dapl));
  ASSERT_TRUE(uncached.valid());
  std::array<int32_t, kTotalElems> full_data{};
  full_data.fill(7);
  ASSERT_GE(H5Dwrite(uncached, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     full_data.data()),
            0);
  nbytes_used = 123;
  nused = 123;
  EXPECT_GE(H5D__current_cache_size_test(uncached, &nbytes_used, &nused), 0);
  EXPECT_EQ(nbytes_used, 0u);
  EXPECT_EQ(nused, 0);

  // 8. Reject invalid or non-dataset identifiers.
  ScopedErrorSilence silence;
  nbytes_used = 555;
  nused = 555;
  EXPECT_LT(H5D__current_cache_size_test(H5I_INVALID_HID, &nbytes_used, &nused),
            0);
  EXPECT_LT(H5D__current_cache_size_test(file, &nbytes_used, &nused), 0);
  EXPECT_EQ(nbytes_used, 555u);
  EXPECT_EQ(nused, 555);
}

}  // namespace
}  // namespace hdf5
