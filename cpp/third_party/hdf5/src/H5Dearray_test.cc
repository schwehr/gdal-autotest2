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
// Tests extensible array chunk indexing for datasets with exactly one
// unlimited dimension, covering unfiltered and filtered records, unlim_dim == 0
// and unlim_dim > 0 swizzling, lazy open/reopen paths, chunk updates,
// shrinking/pruning, object copy, format conversion, SWMR flush dependencies,
// and internal callbacks (third_party/hdf5/src/H5Dearray.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dearray.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

// `:hdf5` is built with -DNDEBUG, which omits debug/stats fields from
// H5C_cache_entry_t inside internal structs. Define NDEBUG around both
// `hdf5_internal_prelude.h` and internal headers so struct layouts match.
#ifndef NDEBUG
#define H5DEARRAY_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5D_FRIEND
#define H5EA_FRIEND
#define H5F_FRIEND
#define H5G_FRIEND
// H5B2private.h (pulled by H5Dpkg.h) and H5Fpkg.h use `try` as a parameter
// name, colliding with libc++'s `#define try if (1)` macro and `try` keyword.
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5EApkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DEARRAY_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DEARRAY_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

// Mirrors the file-local H5D_earray_filt_elmt_t layout in H5Dearray.c for
// testing H5EA_CLS_FILT_CHUNK[0].debug.
struct EarrayFiltElmt {
  haddr_t addr;
  hsize_t nbytes;
  uint32_t filter_mask;
};

// RAII wrapper pushing an HDF5 API context and tagging cache entries.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag) {
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

// Creates a Core VFD FAPL configured with the requested libver bounds.
ScopedPlist MakeCoreFaplWithLibver(H5F_libver_t low, H5F_libver_t high) {
  ScopedPlist fapl = MakeCoreFapl();
  if (fapl.valid() && H5Pset_libver_bounds(fapl.get(), low, high) < 0) {
    fapl.reset();
  }
  return fapl;
}

// Extracts the serialized file image from an open Core VFD HDF5 file.
std::vector<uint8_t> ExtractFileImage(hid_t file_id) {
  if (H5Fflush(file_id, H5F_SCOPE_GLOBAL) < 0) {
    return {};
  }
  const ssize_t image_size = H5Fget_file_image(file_id, nullptr, 0);
  if (image_size <= 0) {
    return {};
  }
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  const ssize_t copied = H5Fget_file_image(file_id, image.data(), image.size());
  if (copied != image_size) {
    return {};
  }
  return image;
}

// Opens an in-memory HDF5 file image via the Core VFD.
ScopedFile OpenCoreFileImage(std::vector<uint8_t>& image, unsigned flags,
                             H5F_libver_t low, H5F_libver_t high) {
  ScopedPlist fapl = MakeCoreFaplWithLibver(low, high);
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_file_image(fapl.get(), image.data(), image.size()) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fopen("in_memory_image.h5", flags, fapl.get()));
}

struct ChunkIterRecord {
  std::vector<hsize_t> offset;
  unsigned filter_mask = 0;
  haddr_t addr = HADDR_UNDEF;
  hsize_t size = 0;
};

struct ChunkIterState {
  unsigned rank = 0;
  std::vector<ChunkIterRecord> records;
  int stop_after = -1;
  bool fail_on_first = false;
};

int CollectChunkCallback(const hsize_t* offset, unsigned filter_mask,
                         haddr_t addr, hsize_t size, void* op_data) {
  auto* state = static_cast<ChunkIterState*>(op_data);
  if (state->fail_on_first) {
    return H5_ITER_ERROR;
  }
  ChunkIterRecord rec;
  rec.offset.assign(offset, offset + state->rank);
  rec.filter_mask = filter_mask;
  rec.addr = addr;
  rec.size = size;
  state->records.push_back(std::move(rec));
  if (state->stop_after > 0 &&
      static_cast<int>(state->records.size()) >= state->stop_after) {
    return H5_ITER_STOP;
  }
  return H5_ITER_CONT;
}

// Writes one 2D single-chunk hyperslab value pattern starting at `start`.
void WriteChunkBlock2D(hid_t dset_id, const std::array<hsize_t, 2>& start,
                       const std::array<hsize_t, 2>& count, int32_t base_val) {
  ScopedSpace fspace(H5Dget_space(dset_id));
  ASSERT_TRUE(fspace.valid());
  ASSERT_GE(H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, start.data(),
                                nullptr, count.data(), nullptr),
            0);

  ScopedSpace mspace(H5Screate_simple(2, count.data(), nullptr));
  ASSERT_TRUE(mspace.valid());

  std::vector<int32_t> data(static_cast<size_t>(count[0] * count[1]));
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = base_val + static_cast<int32_t>(i);
  }
  ASSERT_GE(H5Dwrite(dset_id, H5T_NATIVE_INT32, mspace.get(), fspace.get(),
                     H5P_DEFAULT, data.data()),
            0);
}

TEST(H5DearrayTest, UnfilteredLifecycleCreateReopenIterateShrinkDelete) {
  // 28x8 dataset with 2x4 chunks -> 14x2 = 28 total logical chunk slots.
  // Sparsely populating chunks up to index 27 allocates the EA index block,
  // data blocks, and superblock while leaving gaps for H5D__earray_fill.
  constexpr std::array<hsize_t, 2> kInitDims = {28, 8};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, 8};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 4};

  std::vector<uint8_t> image;
  {
    ScopedPlist fapl =
        MakeCoreFaplWithLibver(H5F_LIBVER_V110, H5F_LIBVER_V110);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("earray_unfilt.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedSpace space(H5Screate_simple(2, kInitDims.data(), kMaxDims.data()));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, kChunkDims.data()), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_INCR), 0);

    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl.valid());
    ASSERT_GE(H5Pset_chunk_cache(dapl.get(), 0, 0, 0.0), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "dset", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  dapl.get()));
    ASSERT_TRUE(dset.valid());

    H5D_chunk_index_t idx_type = H5D_CHUNK_IDX_NONE;
    ASSERT_GE(H5Dget_chunk_index_type(dset.get(), &idx_type), 0);
    EXPECT_EQ(idx_type, H5D_CHUNK_IDX_EARRAY);

    H5D_space_status_t status = H5D_SPACE_STATUS_ERROR;
    ASSERT_GE(H5Dget_space_status(dset.get(), &status), 0);
    EXPECT_EQ(status, H5D_SPACE_STATUS_NOT_ALLOCATED);

    // Delete an empty dataset before index creation (idx_addr == HADDR_UNDEF).
    ScopedDataset empty_dset(H5Dcreate2(file.get(), "empty_dset",
                                        H5T_NATIVE_INT32, space.get(),
                                        H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(empty_dset.valid());
    empty_dset.reset();
    ASSERT_GE(H5Ldelete(file.get(), "empty_dset", H5P_DEFAULT), 0);

    // Populate sparse chunks across index block, first data block, and later
    // superblock/data block.
    WriteChunkBlock2D(dset.get(), {0, 0}, kChunkDims, 100);
    WriteChunkBlock2D(dset.get(), {2, 4}, kChunkDims, 200);
    WriteChunkBlock2D(dset.get(), {6, 0}, kChunkDims, 300);
    WriteChunkBlock2D(dset.get(), {14, 4}, kChunkDims, 400);
    WriteChunkBlock2D(dset.get(), {24, 0}, kChunkDims, 500);
    WriteChunkBlock2D(dset.get(), {26, 4}, kChunkDims, 600);

    ASSERT_GE(H5Dget_space_status(dset.get(), &status), 0);
    EXPECT_EQ(status, H5D_SPACE_STATUS_PART_ALLOCATED);

    // Query index metadata size via H5Oget_native_info_by_name
    // (H5D__earray_idx_size).
    H5O_native_info_t ninfo{};
    ASSERT_GE(H5Oget_native_info_by_name(file.get(), "dset", &ninfo,
                                          H5O_NATIVE_INFO_META_SIZE,
                                          H5P_DEFAULT),
              0);
    EXPECT_GT(ninfo.meta_size.obj.index_size, 0U);

    dset.reset();
    image = ExtractFileImage(file.get());
    ASSERT_FALSE(image.empty());
  }

  // Reopen image #1: query get_addr FIRST while earray is closed (!IS_OPEN),
  // then query missing chunks (index block gap and data block gap) while open.
  {
    ScopedFile file = OpenCoreFileImage(image, H5F_ACC_RDONLY, H5F_LIBVER_V110,
                                        H5F_LIBVER_V110);
    ASSERT_TRUE(file.valid());
    ScopedDataset dset(H5Dopen2(file.get(), "dset", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    unsigned fmask = 999;
    haddr_t addr = HADDR_UNDEF;
    hsize_t csize = 0;
    constexpr std::array<hsize_t, 2> kCoordAllocated = {24, 0};
    ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), kCoordAllocated.data(),
                                         &fmask, &addr, &csize),
              0);
    EXPECT_EQ(fmask, 0U);
    EXPECT_NE(addr, HADDR_UNDEF);
    EXPECT_EQ(csize, 8 * sizeof(int32_t));

    // Query unallocated chunk coordinates (hits !H5_addr_defined in get_addr).
    constexpr std::array<hsize_t, 2> kCoordMissingIdxBlk = {0, 4};
    fmask = 999;
    addr = 123;
    csize = 999;
    ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), kCoordMissingIdxBlk.data(),
                                         &fmask, &addr, &csize),
              0);
    EXPECT_EQ(addr, HADDR_UNDEF);
    EXPECT_EQ(csize, 0U);

    constexpr std::array<hsize_t, 2> kCoordMissingDblk = {20, 4};
    addr = 123;
    csize = 999;
    ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), kCoordMissingDblk.data(),
                                         &fmask, &addr, &csize),
              0);
    EXPECT_EQ(addr, HADDR_UNDEF);
    EXPECT_EQ(csize, 0U);

    std::vector<int32_t> read_buf(28 * 8, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, read_buf.data()),
              0);
    EXPECT_EQ(read_buf[0 * 8 + 0], 100);
    EXPECT_EQ(read_buf[2 * 8 + 4], 200);
    EXPECT_EQ(read_buf[24 * 8 + 0], 500);
    EXPECT_EQ(read_buf[26 * 8 + 4], 600);
    EXPECT_EQ(read_buf[0 * 8 + 4], 0);
  }

  // Reopen image #2: iterate FIRST while earray is closed (!IS_OPEN), then test
  // H5_ITER_STOP, H5_ITER_ERROR, and H5Dget_num_chunks while open.
  {
    ScopedFile file = OpenCoreFileImage(image, H5F_ACC_RDONLY, H5F_LIBVER_V110,
                                        H5F_LIBVER_V110);
    ASSERT_TRUE(file.valid());
    ScopedDataset dset(H5Dopen2(file.get(), "dset", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    ChunkIterState full_state{.rank = 2};
    ASSERT_EQ(H5Dchunk_iter(dset.get(), H5P_DEFAULT, CollectChunkCallback,
                            &full_state),
              H5_ITER_CONT);
    EXPECT_EQ(full_state.records.size(), 6U);

    // Iterate while already open -> H5EA_patch_file + H5_ITER_STOP.
    ChunkIterState stop_state{.rank = 2, .stop_after = 2};
    EXPECT_GE(H5Dchunk_iter(dset.get(), H5P_DEFAULT, CollectChunkCallback,
                            &stop_state),
              0);
    EXPECT_EQ(stop_state.records.size(), 2U);

    {
      ScopedErrorSilence silence;
      ChunkIterState err_state{.rank = 2, .fail_on_first = true};
      EXPECT_LT(H5Dchunk_iter(dset.get(), H5P_DEFAULT, CollectChunkCallback,
                              &err_state),
                0);
    }

    hsize_t num_chunks = 0;
    ScopedSpace space(H5Dget_space(dset.get()));
    ASSERT_TRUE(space.valid());
    ASSERT_GE(H5Dget_num_chunks(dset.get(), space.get(), &num_chunks), 0);
    EXPECT_EQ(num_chunks, 6U);
  }

  // Reopen image #3: call H5Dset_extent shrink FIRST while earray is closed
  // (!IS_OPEN), then shrink again while open, copy via H5Ocopy, convert to v1
  // B-tree, and delete original via H5Ldelete.
  {
    ScopedFile file = OpenCoreFileImage(image, H5F_ACC_RDWR, H5F_LIBVER_V110,
                                        H5F_LIBVER_V110);
    ASSERT_TRUE(file.valid());
    {
      ScopedDataset dset(H5Dopen2(file.get(), "dset", H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());

      // Shrink 28 -> 26 prunes chunk {26, 4} while earray is not yet open.
      constexpr std::array<hsize_t, 2> kShrink1 = {26, 8};
      ASSERT_GE(H5Dset_extent(dset.get(), kShrink1.data()), 0);

      // Shrink 26 -> 12 prunes chunks {24, 0} and {14, 4} while earray is open.
      constexpr std::array<hsize_t, 2> kShrink2 = {12, 8};
      ASSERT_GE(H5Dset_extent(dset.get(), kShrink2.data()), 0);

      ScopedSpace space(H5Dget_space(dset.get()));
      ASSERT_TRUE(space.valid());
      hsize_t remaining = 0;
      ASSERT_GE(H5Dget_num_chunks(dset.get(), space.get(), &remaining), 0);
      EXPECT_EQ(remaining, 3U);

      std::vector<int32_t> shrunk_vals(12 * 8, -1);
      ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                        H5P_DEFAULT, shrunk_vals.data()),
                0);
      EXPECT_EQ(shrunk_vals[0], 100);
      EXPECT_EQ(shrunk_vals[2 * 8 + 4], 200);
      EXPECT_EQ(shrunk_vals[6 * 8 + 0], 300);
    }

    // Object copy on reopened dataset hits copy_setup, copy_shutdown, reset.
    ASSERT_GE(H5Ocopy(file.get(), "dset", file.get(), "dset_copy", H5P_DEFAULT,
                      H5P_DEFAULT),
              0);

    {
      ScopedDataset copy_dset(H5Dopen2(file.get(), "dset_copy", H5P_DEFAULT));
      ASSERT_TRUE(copy_dset.valid());
      ASSERT_GE(H5Dformat_convert(copy_dset.get()), 0);

      std::vector<int32_t> copied_vals(12 * 8, -1);
      ASSERT_GE(H5Dread(copy_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                        H5P_DEFAULT, copied_vals.data()),
                0);
      EXPECT_EQ(copied_vals[0], 100);
      EXPECT_EQ(copied_vals[2 * 8 + 4], 200);
      EXPECT_EQ(copied_vals[6 * 8 + 0], 300);
    }

    ASSERT_GE(H5Ldelete(file.get(), "dset", H5P_DEFAULT), 0);
  }
}

TEST(H5DearrayTest, NonZeroUnlimDimSwizzlingResizeGetAddrAndRemove) {
  // Place unlimited dimension at index 1 in 2D and index 1 in 3D so
  // unlim_dim > 0 branches in resize, get_addr, and remove execute.
  constexpr std::array<hsize_t, 2> kDims2D = {6, 12};
  constexpr std::array<hsize_t, 2> kMax2D = {6, H5S_UNLIMITED};
  constexpr std::array<hsize_t, 2> kChunk2D = {2, 2};

  constexpr std::array<hsize_t, 3> kDims3D = {4, 6, 4};
  constexpr std::array<hsize_t, 3> kMax3D = {4, H5S_UNLIMITED, 4};
  constexpr std::array<hsize_t, 3> kChunk3D = {2, 2, 2};

  std::vector<uint8_t> image;
  {
    ScopedPlist fapl =
        MakeCoreFaplWithLibver(H5F_LIBVER_V110, H5F_LIBVER_V110);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("earray_swizzle.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedSpace s2(H5Screate_simple(2, kDims2D.data(), kMax2D.data()));
    ASSERT_TRUE(s2.valid());
    ScopedPlist dcpl2(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl2.valid());
    ASSERT_GE(H5Pset_chunk(dcpl2.get(), 2, kChunk2D.data()), 0);

    ScopedDataset d2(H5Dcreate2(file.get(), "d2_swizzle", H5T_NATIVE_INT32,
                                s2.get(), H5P_DEFAULT, dcpl2.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(d2.valid());

    H5D_chunk_index_t idx_type = H5D_CHUNK_IDX_NONE;
    ASSERT_GE(H5Dget_chunk_index_type(d2.get(), &idx_type), 0);
    EXPECT_EQ(idx_type, H5D_CHUNK_IDX_EARRAY);

    WriteChunkBlock2D(d2.get(), {0, 0}, kChunk2D, 10);
    WriteChunkBlock2D(d2.get(), {2, 4}, kChunk2D, 20);
    WriteChunkBlock2D(d2.get(), {4, 10}, kChunk2D, 30);

    // Extend along dimension 1 (triggers H5D__earray_idx_resize with swizzle).
    constexpr std::array<hsize_t, 2> kExtended2D = {6, 16};
    ASSERT_GE(H5Dset_extent(d2.get(), kExtended2D.data()), 0);
    WriteChunkBlock2D(d2.get(), {4, 14}, kChunk2D, 40);

    // Also create and populate a 3D dataset with unlim_dim == 1.
    ScopedSpace s3(H5Screate_simple(3, kDims3D.data(), kMax3D.data()));
    ASSERT_TRUE(s3.valid());
    ScopedPlist dcpl3(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl3.valid());
    ASSERT_GE(H5Pset_chunk(dcpl3.get(), 3, kChunk3D.data()), 0);
    ScopedDataset d3(H5Dcreate2(file.get(), "d3_swizzle", H5T_NATIVE_INT32,
                                s3.get(), H5P_DEFAULT, dcpl3.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(d3.valid());
    std::vector<int32_t> vals3(4 * 6 * 4);
    for (size_t i = 0; i < vals3.size(); ++i) {
      vals3[i] = static_cast<int32_t>(i + 1);
    }
    ASSERT_GE(H5Dwrite(d3.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, vals3.data()),
              0);

    d2.reset();
    d3.reset();
    image = ExtractFileImage(file.get());
    ASSERT_FALSE(image.empty());
  }

  // Reopen image: verify swizzled get_addr and shrink (remove with unlim_dim >
  // 0).
  ScopedFile file = OpenCoreFileImage(image, H5F_ACC_RDWR, H5F_LIBVER_V110,
                                      H5F_LIBVER_V110);
  ASSERT_TRUE(file.valid());

  {
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl.valid());
    ASSERT_GE(H5Pset_chunk_cache(dapl.get(), 0, 0, 0.0), 0);

    ScopedDataset d2(H5Dopen2(file.get(), "d2_swizzle", dapl.get()));
    ASSERT_TRUE(d2.valid());

    // Note: H5Dget_chunk_info_by_coord uses unswizzled iterate_cb order when
    // unlim_dim > 0; H5Dget_chunk_storage_size calls H5D__earray_idx_get_addr
    // directly with swizzling.
    hsize_t csize = 0;
    constexpr std::array<hsize_t, 2> kCoord = {4, 14};
    ASSERT_GE(H5Dget_chunk_storage_size(d2.get(), kCoord.data(), &csize), 0);
    EXPECT_EQ(csize, 4 * sizeof(int32_t));

    // Verify chunk data via hyperslab read (also invokes swizzled get_addr).
    {
      ScopedSpace fs(H5Dget_space(d2.get()));
      ASSERT_TRUE(fs.valid());
      ASSERT_GE(H5Sselect_hyperslab(fs.get(), H5S_SELECT_SET, kCoord.data(),
                                    nullptr, kChunk2D.data(), nullptr),
                0);
      ScopedSpace ms(H5Screate_simple(2, kChunk2D.data(), nullptr));
      ASSERT_TRUE(ms.valid());
      std::array<int32_t, 4> got{};
      ASSERT_GE(H5Dread(d2.get(), H5T_NATIVE_INT32, ms.get(), fs.get(),
                        H5P_DEFAULT, got.data()),
                0);
      EXPECT_EQ(got[0], 40);
      EXPECT_EQ(got[3], 43);
    }

    // Shrink 16 -> 6 (prunes {4, 10} and {4, 14}), then 6 -> 2 (prunes {2, 4}).
    constexpr std::array<hsize_t, 2> kShrink1 = {6, 6};
    ASSERT_GE(H5Dset_extent(d2.get(), kShrink1.data()), 0);
    constexpr std::array<hsize_t, 2> kShrink2 = {6, 2};
    ASSERT_GE(H5Dset_extent(d2.get(), kShrink2.data()), 0);

    std::vector<int32_t> remaining(6 * 2, -1);
    ASSERT_GE(H5Dread(d2.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      remaining.data()),
              0);
    EXPECT_EQ(remaining[0], 10);
  }

  {
    ScopedDataset d3(H5Dopen2(file.get(), "d3_swizzle", H5P_DEFAULT));
    ASSERT_TRUE(d3.valid());
    constexpr std::array<hsize_t, 3> kShrink3 = {4, 2, 4};
    ASSERT_GE(H5Dset_extent(d3.get(), kShrink3.data()), 0);
    std::vector<int32_t> vals3_after(4 * 2 * 4, -1);
    ASSERT_GE(H5Dread(d3.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      vals3_after.data()),
              0);
    EXPECT_EQ(vals3_after[0], 1);
  }
}

TEST(H5DearrayTest, FilteredChunksV4AndV5EncodeDecodeUpdateAndShrink) {
  ASSERT_GT(H5Zfilter_avail(H5Z_FILTER_DEFLATE), 0);

  // 168x16 with 8x8 chunks -> 21x2 = 42 chunk slots (index block + superblock
  // + multiple data blocks).
  constexpr std::array<hsize_t, 2> kDims = {168, 16};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, 16};
  constexpr std::array<hsize_t, 2> kChunkDims = {8, 8};

  for (const H5F_libver_t libver : {H5F_LIBVER_V110, H5F_LIBVER_LATEST}) {
    std::vector<uint8_t> image;
    {
      ScopedPlist fapl = MakeCoreFaplWithLibver(libver, libver);
      ASSERT_TRUE(fapl.valid());
      ScopedFile file(H5Fcreate("earray_filt.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                fapl.get()));
      ASSERT_TRUE(file.valid());

      ScopedSpace space(H5Screate_simple(2, kDims.data(), kMaxDims.data()));
      ASSERT_TRUE(space.valid());

      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, kChunkDims.data()), 0);
      ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);

      ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
      ASSERT_TRUE(dapl.valid());
      ASSERT_GE(H5Pset_chunk_cache(dapl.get(), 0, 0, 0.0), 0);

      ScopedDataset dset(H5Dcreate2(file.get(), "fdset", H5T_NATIVE_INT32,
                                    space.get(), H5P_DEFAULT, dcpl.get(),
                                    dapl.get()));
      ASSERT_TRUE(dset.valid());

      // Write uniform zeros at {0, 0} -> small compressed size.
      {
        ScopedSpace fs(H5Dget_space(dset.get()));
        ASSERT_TRUE(fs.valid());
        constexpr std::array<hsize_t, 2> kOrigin = {0, 0};
        ASSERT_GE(H5Sselect_hyperslab(fs.get(), H5S_SELECT_SET, kOrigin.data(),
                                      nullptr, kChunkDims.data(), nullptr),
                  0);
        ScopedSpace ms(H5Screate_simple(2, kChunkDims.data(), nullptr));
        ASSERT_TRUE(ms.valid());
        std::vector<int32_t> zeros(8 * 8, 0);
        ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, ms.get(), fs.get(),
                           H5P_DEFAULT, zeros.data()),
                  0);
      }

      // Write sparse chunks at {40, 8} and {160, 8} so superblocks/datablocks
      // contain holes requiring H5D__earray_filt_fill.
      WriteChunkBlock2D(dset.get(), {40, 8}, kChunkDims, 3000);
      WriteChunkBlock2D(dset.get(), {160, 8}, kChunkDims, 7000);
      ASSERT_GE(H5Dflush(dset.get()), 0);

      hsize_t compressed_before = 0;
      constexpr std::array<hsize_t, 2> kOrigin = {0, 0};
      ASSERT_GE(H5Dget_chunk_storage_size(dset.get(), kOrigin.data(),
                                          &compressed_before),
                0);
      EXPECT_LT(compressed_before, 8 * 8 * sizeof(int32_t));

      // Overwrite chunk {0, 0} with incompressible values to update filtered EA
      // record in place.
      {
        ScopedSpace fs(H5Dget_space(dset.get()));
        ASSERT_TRUE(fs.valid());
        ASSERT_GE(H5Sselect_hyperslab(fs.get(), H5S_SELECT_SET, kOrigin.data(),
                                      nullptr, kChunkDims.data(), nullptr),
                  0);
        ScopedSpace ms(H5Screate_simple(2, kChunkDims.data(), nullptr));
        ASSERT_TRUE(ms.valid());
        std::vector<int32_t> noise(8 * 8);
        uint32_t rng = 0x13572468u;
        for (int32_t& val : noise) {
          rng = rng * 1664525u + 1013904223u;
          val = static_cast<int32_t>(rng);
        }
        ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, ms.get(), fs.get(),
                           H5P_DEFAULT, noise.data()),
                  0);
      }

      hsize_t compressed_after = 0;
      ASSERT_GE(H5Dget_chunk_storage_size(dset.get(), kOrigin.data(),
                                          &compressed_after),
                0);
      EXPECT_GT(compressed_after, compressed_before);

      dset.reset();
      image = ExtractFileImage(file.get());
      ASSERT_FALSE(image.empty());
    }

    // Reopen filtered image, check get_addr FIRST, iterate, shrink (filtered
    // remove), copy, and delete.
    ScopedFile file = OpenCoreFileImage(image, H5F_ACC_RDWR, libver, libver);
    ASSERT_TRUE(file.valid());
    {
      ScopedDataset dset(H5Dopen2(file.get(), "fdset", H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());

      unsigned fmask = 999;
      haddr_t addr = HADDR_UNDEF;
      hsize_t csize = 0;
      constexpr std::array<hsize_t, 2> kCoord = {40, 8};
      ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), kCoord.data(), &fmask,
                                           &addr, &csize),
                0);
      EXPECT_NE(addr, HADDR_UNDEF);
      EXPECT_GT(csize, 0U);

      ChunkIterState it{.rank = 2};
      ASSERT_EQ(H5Dchunk_iter(dset.get(), H5P_DEFAULT, CollectChunkCallback, &it),
                H5_ITER_CONT);
      EXPECT_EQ(it.records.size(), 3U);

      std::vector<int32_t> read_back(168 * 16);
      ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                        H5P_DEFAULT, read_back.data()),
                0);
      EXPECT_EQ(read_back[40 * 16 + 8], 3000);
      EXPECT_EQ(read_back[160 * 16 + 8], 7000);

      // Shrink to prune chunk {160, 8} (filtered H5D__earray_idx_remove).
      constexpr std::array<hsize_t, 2> kShrink = {80, 16};
      ASSERT_GE(H5Dset_extent(dset.get(), kShrink.data()), 0);
    }
    ASSERT_GE(H5Ocopy(file.get(), "fdset", file.get(), "fdset_copy",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ldelete(file.get(), "fdset", H5P_DEFAULT), 0);
  }
}

TEST(H5DearrayTest, SwmrWriteCreatesDependsOpensAndPrunesWithoutFreeing) {
  ASSERT_GT(H5Zfilter_avail(H5Z_FILTER_DEFLATE), 0);
  // SWMR requires a VFD supporting H5FD_FEAT_SUPPORTS_SWMR_IO (Sec2 VFD).
  TempFile temp_h5("earray_swmr.h5");

  constexpr std::array<hsize_t, 2> kDims = {4, 4};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, 4};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 2};

  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST,
                                   H5F_LIBVER_LATEST),
              0);

    ScopedFile file(H5Fcreate(temp_h5.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedSpace space(H5Screate_simple(2, kDims.data(), kMaxDims.data()));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl_u(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_u.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_u.get(), 2, kChunkDims.data()), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl_u.get(), H5D_ALLOC_TIME_INCR), 0);
    ScopedDataset du(H5Dcreate2(file.get(), "swmr_unfilt", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl_u.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(du.valid());

    ScopedPlist dcpl_f(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_f.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_f.get(), 2, kChunkDims.data()), 0);
    ASSERT_GE(H5Pset_deflate(dcpl_f.get(), 6), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl_f.get(), H5D_ALLOC_TIME_INCR), 0);
    ScopedDataset df(H5Dcreate2(file.get(), "swmr_filt", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl_f.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(df.valid());

    // Enter SWMR write BEFORE index creation so idx_create triggers depend().
    ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);

    WriteChunkBlock2D(du.get(), {0, 0}, kChunkDims, 11);
    WriteChunkBlock2D(du.get(), {2, 2}, kChunkDims, 22);
    WriteChunkBlock2D(df.get(), {0, 0}, kChunkDims, 33);
    WriteChunkBlock2D(df.get(), {2, 2}, kChunkDims, 44);
    ASSERT_GE(H5Dflush(du.get()), 0);
    ASSERT_GE(H5Dflush(df.get()), 0);
  }

  // Reopen in SWMR write mode so idx_open triggers depend(), and shrinking hits
  // the SWMR intent branches in H5D__earray_idx_remove without H5MF_xfree.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST,
                                   H5F_LIBVER_LATEST),
              0);

    ScopedFile file(H5Fopen(temp_h5.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE,
                            fapl.get()));
    ASSERT_TRUE(file.valid());
    {
      ScopedDataset du(H5Dopen2(file.get(), "swmr_unfilt", H5P_DEFAULT));
      ASSERT_TRUE(du.valid());
      ScopedDataset df(H5Dopen2(file.get(), "swmr_filt", H5P_DEFAULT));
      ASSERT_TRUE(df.valid());

      unsigned fmask = 0;
      haddr_t addr = HADDR_UNDEF;
      hsize_t csize = 0;
      constexpr std::array<hsize_t, 2> kCoord = {0, 0};
      ASSERT_GE(H5Dget_chunk_info_by_coord(du.get(), kCoord.data(), &fmask,
                                           &addr, &csize),
                0);
      EXPECT_NE(addr, HADDR_UNDEF);
      ASSERT_GE(H5Dget_chunk_info_by_coord(df.get(), kCoord.data(), &fmask,
                                           &addr, &csize),
                0);
      EXPECT_NE(addr, HADDR_UNDEF);

      constexpr std::array<hsize_t, 2> kShrink = {2, 4};
      ASSERT_GE(H5Dset_extent(du.get(), kShrink.data()), 0);
      ASSERT_GE(H5Dset_extent(df.get(), kShrink.data()), 0);
      ASSERT_GE(H5Dflush(du.get()), 0);
      ASSERT_GE(H5Dflush(df.get()), 0);
    }
  }
}

TEST(H5DearrayTest, InternalInitErrorsInsertRemoveDebugDumpAndV4Clamp) {
  ASSERT_GT(H5Zfilter_avail(H5Z_FILTER_DEFLATE), 0);

  constexpr std::array<hsize_t, 2> kDims = {4, 4};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, 4};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 2};

  std::vector<uint8_t> image;
  {
    ScopedPlist fapl =
        MakeCoreFaplWithLibver(H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("earray_internal.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedGroup grp(
        H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    ScopedSpace space(H5Screate_simple(2, kDims.data(), kMaxDims.data()));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl_u(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_u.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_u.get(), 2, kChunkDims.data()), 0);
    ScopedDataset du(H5Dcreate2(file.get(), "unfilt", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl_u.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(du.valid());
    WriteChunkBlock2D(du.get(), {0, 0}, kChunkDims, 10);

    ScopedPlist dcpl_f(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_f.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_f.get(), 2, kChunkDims.data()), 0);
    ASSERT_GE(H5Pset_deflate(dcpl_f.get(), 6), 0);
    ScopedDataset df(H5Dcreate2(file.get(), "filt", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl_f.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(df.valid());
    WriteChunkBlock2D(df.get(), {0, 0}, kChunkDims, 20);
    WriteChunkBlock2D(df.get(), {2, 2}, kChunkDims, 30);

    du.reset();
    df.reset();
    grp.reset();
    image = ExtractFileImage(file.get());
    ASSERT_FALSE(image.empty());
  }

  ScopedFile file = OpenCoreFileImage(image, H5F_ACC_RDWR, H5F_LIBVER_LATEST,
                                      H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(H5Gopen2(file.get(), "grp", H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  H5G_loc_t gloc{};
  ASSERT_GE(H5G_loc(grp.get(), &gloc), 0);

  ScopedDataset du(H5Dopen2(file.get(), "unfilt", H5P_DEFAULT));
  ASSERT_TRUE(du.valid());
  ScopedDataset df(H5Dopen2(file.get(), "filt", H5P_DEFAULT));
  ASSERT_TRUE(df.valid());

  auto* dset_u = static_cast<H5D_t*>(H5VL_object_verify(du.get(), H5I_DATASET));
  ASSERT_NE(dset_u, nullptr);
  auto* dset_f = static_cast<H5D_t*>(H5VL_object_verify(df.get(), H5I_DATASET));
  ASSERT_NE(dset_f, nullptr);

  {
    ScopedApiContext ctx(dset_u->oloc.addr);
    ASSERT_TRUE(ctx.valid());

    H5O_storage_chunk_t* sc_u = &dset_u->shared->layout.storage.u.chunk;
    H5D_chk_idx_info_t idx_info_u{};
    idx_info_u.f = dset_u->oloc.file;
    idx_info_u.pline = &dset_u->shared->dcpl_cache.pline;
    idx_info_u.layout = &dset_u->shared->layout;

    // 1. Test H5D__earray_idx_init error paths (0 unlimited dims and >1
    //    unlimited dims).
    {
      ScopedErrorSilence silence;
      H5O_layout_t bad_layout = dset_u->shared->layout;
      H5D_chk_idx_info_t bad_info = idx_info_u;
      bad_info.layout = &bad_layout;

      ScopedSpace fixed_space_id(
          H5Screate_simple(2, kDims.data(), kDims.data()));
      ASSERT_TRUE(fixed_space_id.valid());
      auto* fixed_space = static_cast<const H5S_t*>(
          H5I_object_verify(fixed_space_id.get(), H5I_DATASPACE));
      ASSERT_NE(fixed_space, nullptr);
      EXPECT_LT(
          H5D_COPS_EARRAY[0].init(&bad_info, fixed_space, dset_u->oloc.addr),
          0);

      constexpr std::array<hsize_t, 2> kTwoUnlim = {H5S_UNLIMITED,
                                                    H5S_UNLIMITED};
      ScopedSpace multi_unlim_id(
          H5Screate_simple(2, kDims.data(), kTwoUnlim.data()));
      ASSERT_TRUE(multi_unlim_id.valid());
      auto* multi_unlim_space = static_cast<const H5S_t*>(
          H5I_object_verify(multi_unlim_id.get(), H5I_DATASPACE));
      ASSERT_NE(multi_unlim_space, nullptr);
      EXPECT_LT(H5D_COPS_EARRAY[0].init(&bad_info, multi_unlim_space,
                                        dset_u->oloc.addr),
                0);
    }

    // 2. Test is_open, insert(!IS_OPEN) error checks and valid insert, and
    //    remove(!IS_OPEN).
    bool is_open = true;
    ASSERT_GE(sc_u->ops->is_open(&idx_info_u, &is_open), 0);
    EXPECT_FALSE(is_open);

    {
      ScopedErrorSilence silence;
      H5D_chunk_ud_t bad_ud{};
      hsize_t bad_scaled[H5O_LAYOUT_NDIMS] = {1, 1, 0};
      bad_ud.common.layout = &dset_u->shared->layout.u.chunk;
      bad_ud.common.storage = sc_u;
      bad_ud.common.scaled = bad_scaled;
      bad_ud.chunk_block.offset = HADDR_UNDEF;
      bad_ud.chunk_idx = 0;
      // Opens earray lazily during insert, then fails on HADDR_UNDEF.
      EXPECT_LT(sc_u->ops->insert(&idx_info_u, &bad_ud, dset_u), 0);

      // Chunk index >= 2^32 error check while already open.
      bad_ud.chunk_block.offset = 4096;
      bad_ud.chunk_idx = uint64_t{1} << 32;
      EXPECT_LT(sc_u->ops->insert(&idx_info_u, &bad_ud, dset_u), 0);
    }

    // Close so valid insert below hits !IS_OPEN path cleanly.
    ASSERT_GE(sc_u->ops->dest(&idx_info_u), 0);
    ASSERT_GE(sc_u->ops->is_open(&idx_info_u, &is_open), 0);
    EXPECT_FALSE(is_open);

    hsize_t scaled[H5O_LAYOUT_NDIMS] = {1, 1, 0};
    H5D_chunk_ud_t udata{};
    udata.common.layout = &dset_u->shared->layout.u.chunk;
    udata.common.storage = sc_u;
    udata.common.scaled = scaled;
    udata.chunk_block.offset = HADDR_UNDEF;
    udata.chunk_block.length = dset_u->shared->layout.u.chunk.size;
    udata.idx_hint = UINT_MAX;
    bool need_insert = false;
    ASSERT_GE(H5D__chunk_file_alloc(&idx_info_u, nullptr, &udata.chunk_block,
                                    &need_insert, scaled),
              0);
    ASSERT_TRUE(need_insert);
    udata.chunk_idx = 3;
    std::vector<int32_t> chunk_bytes(4, 99);
    ASSERT_GE(H5F_block_write(idx_info_u.f, H5FD_MEM_DRAW,
                              udata.chunk_block.offset, chunk_bytes.size() * 4,
                              chunk_bytes.data()),
              0);
    ASSERT_GE(sc_u->ops->insert(&idx_info_u, &udata, dset_u), 0);

    // Close and test H5D__earray_idx_remove while !IS_OPEN.
    ASSERT_GE(sc_u->ops->dest(&idx_info_u), 0);
    H5D_chunk_common_ud_t rm_ud{};
    rm_ud.layout = &dset_u->shared->layout.u.chunk;
    rm_ud.storage = sc_u;
    rm_ud.scaled = scaled;
    ASSERT_GE(sc_u->ops->remove(&idx_info_u, &rm_ud), 0);

    // 3. Test load_metadata and dump index (both H5D__earray_idx_dump and
    //    H5D__chunk_dump_index).
    ASSERT_GE(sc_u->ops->dest(&idx_info_u), 0);
    ASSERT_GE(sc_u->ops->load_metadata(&idx_info_u), 0);
    ASSERT_GE(sc_u->ops->is_open(&idx_info_u, &is_open), 0);
    EXPECT_TRUE(is_open);

    std::array<char, 1024> dump_buf{};
    FILE* dump_stream = fmemopen(dump_buf.data(), dump_buf.size() - 1, "w");
    ASSERT_NE(dump_stream, nullptr);
    absl::Cleanup close_dump = [dump_stream]() { std::fclose(dump_stream); };
    ASSERT_GE(H5D__chunk_dump_index(dset_u, dump_stream), 0);
    std::fflush(dump_stream);
    EXPECT_THAT(std::string_view(dump_buf.data()), HasSubstr("Address:"));

    // 4. Test H5EA_CLS_CHUNK[0] debug callbacks directly.
    void* dbg_ctx_u =
        H5EA_CLS_CHUNK[0].crt_dbg_ctx(idx_info_u.f, dset_u->oloc.addr);
    ASSERT_NE(dbg_ctx_u, nullptr);
    std::array<char, 512> dbg_u_buf{};
    FILE* dbg_u_stream = fmemopen(dbg_u_buf.data(), dbg_u_buf.size() - 1, "w");
    ASSERT_NE(dbg_u_stream, nullptr);
    absl::Cleanup close_dbg_u = [dbg_u_stream]() { std::fclose(dbg_u_stream); };
    haddr_t sample_addr = 4096;
    ASSERT_GE(H5EA_CLS_CHUNK[0].debug(dbg_u_stream, 2, 20, 0, &sample_addr), 0);
    std::fflush(dbg_u_stream);
    EXPECT_THAT(std::string_view(dbg_u_buf.data()), HasSubstr("Element #0:"));
    EXPECT_GE(H5EA_CLS_CHUNK[0].dst_dbg_ctx(dbg_ctx_u), 0);

    // 5. Test H5D_chunk_idx_reset with reset_addr = false and true.
    H5O_storage_chunk_t temp_sc = *sc_u;
    ASSERT_GE(H5D_chunk_idx_reset(&temp_sc, false), 0);
    EXPECT_NE(temp_sc.idx_addr, HADDR_UNDEF);
    EXPECT_EQ(temp_sc.u.earray.ea, nullptr);
    ASSERT_GE(H5D_chunk_idx_reset(&temp_sc, true), 0);
    EXPECT_EQ(temp_sc.idx_addr, HADDR_UNDEF);
    EXPECT_EQ(temp_sc.u.earray.dset_ohdr_addr, HADDR_UNDEF);
  }

  // Filtered internal callbacks: remove while !IS_OPEN, filt debug callbacks,
  // negative filt_crt_dbg_context paths, copy_setup when open, empty iterate,
  // and v4 chunk_size_len > 8 clamp.
  {
    ScopedApiContext ctx(dset_f->oloc.addr);
    ASSERT_TRUE(ctx.valid());

    H5O_storage_chunk_t* sc_f = &dset_f->shared->layout.storage.u.chunk;
    H5D_chk_idx_info_t idx_info_f{};
    idx_info_f.f = dset_f->oloc.file;
    idx_info_f.pline = &dset_f->shared->dcpl_cache.pline;
    idx_info_f.layout = &dset_f->shared->layout;

    // Remove chunk {2, 2} while filtered earray is closed (!IS_OPEN).
    hsize_t filt_scaled[H5O_LAYOUT_NDIMS] = {1, 1, 0};
    H5D_chunk_common_ud_t rm_filt{};
    rm_filt.layout = &dset_f->shared->layout.u.chunk;
    rm_filt.storage = sc_f;
    rm_filt.scaled = filt_scaled;
    ASSERT_GE(sc_f->ops->remove(&idx_info_f, &rm_filt), 0);

    // Filtered debug callbacks directly.
    void* dbg_ctx_f =
        H5EA_CLS_FILT_CHUNK[0].crt_dbg_ctx(idx_info_f.f, dset_f->oloc.addr);
    ASSERT_NE(dbg_ctx_f, nullptr);
    EarrayFiltElmt filt_elmt{8192, 42, 0x1u};
    std::array<char, 512> dbg_f_buf{};
    FILE* dbg_f_stream = fmemopen(dbg_f_buf.data(), dbg_f_buf.size() - 1, "w");
    ASSERT_NE(dbg_f_stream, nullptr);
    absl::Cleanup close_dbg_f = [dbg_f_stream]() { std::fclose(dbg_f_stream); };
    ASSERT_GE(H5EA_CLS_FILT_CHUNK[0].debug(dbg_f_stream, 2, 20, 1, &filt_elmt),
              0);
    std::fflush(dbg_f_stream);
    EXPECT_THAT(std::string_view(dbg_f_buf.data()), HasSubstr("Element #1:"));
    EXPECT_GE(H5EA_CLS_FILT_CHUNK[0].dst_dbg_ctx(dbg_ctx_f), 0);

    // Negative filt_crt_dbg_context on group header (lacks H5O_LAYOUT_ID) and
    // unallocated object address.
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(
          H5EA_CLS_FILT_CHUNK[0].crt_dbg_ctx(idx_info_f.f, gloc.oloc->addr),
          nullptr);
      EXPECT_EQ(H5EA_CLS_FILT_CHUNK[0].crt_dbg_ctx(
                    idx_info_f.f, idx_info_f.f->shared->tmp_addr),
                nullptr);
    }

    // Synthetic v4 layout with chunk.size >= 2^56 exercises the
    // chunk_size_len > 8 clamp in H5D_EARRAY_FILT_COMPUTE_CHUNK_SIZE_LEN;
    // iterating on it while max_idx_set == 0 hits empty iterate path;
    // copy_setup + copy_shutdown when src is closed/open exercises both.
    H5O_layout_t synth_layout = dset_f->shared->layout;
    synth_layout.version = H5O_LAYOUT_VERSION_4;
    synth_layout.u.chunk.size = uint64_t{1} << 56;
    synth_layout.storage.u.chunk.idx_addr = HADDR_UNDEF;
    synth_layout.storage.u.chunk.u.earray.ea = nullptr;
    synth_layout.storage.u.chunk.u.earray.dset_ohdr_addr = dset_f->oloc.addr;

    H5D_chk_idx_info_t synth_info{};
    synth_info.f = dset_f->oloc.file;
    synth_info.pline = &dset_f->shared->dcpl_cache.pline;
    synth_info.layout = &synth_layout;

    ASSERT_GE(H5D_COPS_EARRAY[0].create(&synth_info), 0);
    EXPECT_NE(synth_layout.storage.u.chunk.idx_addr, HADDR_UNDEF);
    ASSERT_GE(H5D_COPS_EARRAY[0].dest(&synth_info), 0);

    // Iterate when EA exists with 0 elements set (max_idx_set == 0).
    auto noop_cb = [](const H5D_chunk_rec_t*, void*) -> int {
      return H5_ITER_CONT;
    };
    int dummy = 0;
    EXPECT_EQ(H5D_COPS_EARRAY[0].iterate(&synth_info, noop_cb, &dummy),
              H5_ITER_CONT);

    // copy_setup when src is ALREADY open, followed by copy_shutdown + delete.
    H5O_layout_t dst_layout = synth_layout;
    dst_layout.storage.u.chunk.idx_addr = HADDR_UNDEF;
    dst_layout.storage.u.chunk.u.earray.ea = nullptr;
    H5D_chk_idx_info_t dst_info = synth_info;
    dst_info.layout = &dst_layout;
    ASSERT_GE(H5D_COPS_EARRAY[0].copy_setup(&synth_info, &dst_info), 0);
    ASSERT_GE(
        H5D_COPS_EARRAY[0].copy_shutdown(&synth_layout.storage.u.chunk,
                                         &dst_layout.storage.u.chunk),
        0);

    ASSERT_GE(H5D_COPS_EARRAY[0].idx_delete(&dst_info), 0);
    ASSERT_GE(H5D_COPS_EARRAY[0].idx_delete(&synth_info), 0);
  }
}

}  // namespace
}  // namespace hdf5
