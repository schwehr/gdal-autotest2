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
// Tests v2 B-tree chunk indexing for datasets with more than one unlimited
// dimension, covering unfiltered and filtered records, lazy open/reopen paths,
// chunk modification, shrinking/pruning, object copy, format conversion, SWMR
// flush dependencies, and internal index operations
// (third_party/hdf5/src/H5Dbtree2.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dbtree2.c

#include "third_party/hdf5/src/hdf5.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// H5C_cache_entry_t (H5AC_info_t) inside H5B2_hdr_t. Define NDEBUG around both
// `hdf5_internal_prelude.h` and internal headers so struct offsets match.
#ifndef NDEBUG
#define H5DBTREE2_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5D_FRIEND
#define H5B2_FRIEND
// H5B2private.h uses `try` as a parameter name, which collides with libc++'s
// -fno-exceptions `#define try if (1)` macro and the C++ `try` keyword.
#pragma push_macro("try")
#undef try
#define try h5b2_try_param
#include "third_party/hdf5/src/H5B2pkg.h"
#undef try
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5DBTREE2_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DBTREE2_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

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

// Writes one single-chunk hyperslab value pattern starting at `start`.
void WriteChunkBlock(hid_t dset_id, const std::array<hsize_t, 2>& start,
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

TEST(H5Dbtree2Test, UnfilteredLifecycleCreateReopenIterateShrinkDelete) {
  constexpr std::array<hsize_t, 2> kInitDims = {6, 6};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, H5S_UNLIMITED};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 2};

  std::vector<uint8_t> image;
  {
    ScopedPlist fapl =
        MakeCoreFaplWithLibver(H5F_LIBVER_V110, H5F_LIBVER_V110);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("unfilt.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
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
    EXPECT_EQ(idx_type, H5D_CHUNK_IDX_BT2);

    H5D_space_status_t status = H5D_SPACE_STATUS_ERROR;
    ASSERT_GE(H5Dget_space_status(dset.get(), &status), 0);
    EXPECT_EQ(status, H5D_SPACE_STATUS_NOT_ALLOCATED);

    // Empty dataset with LATE alloc has not created index yet; create an empty
    // sibling dataset too so H5Ldelete exercises idx_addr == HADDR_UNDEF.
    ScopedDataset empty_dset(H5Dcreate2(file.get(), "empty_dset",
                                        H5T_NATIVE_INT32, space.get(),
                                        H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(empty_dset.valid());
    empty_dset.reset();
    ASSERT_GE(H5Ldelete(file.get(), "empty_dset", H5P_DEFAULT), 0);

    // Write several chunks across the 6x6 grid (leaving some unallocated).
    WriteChunkBlock(dset.get(), {0, 0}, kChunkDims, 100);
    WriteChunkBlock(dset.get(), {2, 4}, kChunkDims, 200);
    WriteChunkBlock(dset.get(), {4, 2}, kChunkDims, 300);

    ASSERT_GE(H5Dget_space_status(dset.get(), &status), 0);
    EXPECT_EQ(status, H5D_SPACE_STATUS_PART_ALLOCATED);

    // Query index metadata size via H5Oget_native_info_by_name (H5D__bt2_idx_size).
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

  // Reopen image #1: query get_addr FIRST while bt2 is closed (!IS_OPEN), then
  // query an unallocated chunk and existing chunks while open.
  {
    ScopedFile file = OpenCoreFileImage(image, H5F_ACC_RDONLY, H5F_LIBVER_V110,
                                        H5F_LIBVER_V110);
    ASSERT_TRUE(file.valid());
    ScopedDataset dset(H5Dopen2(file.get(), "dset", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    unsigned fmask = 999;
    haddr_t addr = HADDR_UNDEF;
    hsize_t csize = 0;
    constexpr std::array<hsize_t, 2> kCoordAllocated = {2, 4};
    ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), kCoordAllocated.data(),
                                         &fmask, &addr, &csize),
              0);
    EXPECT_EQ(fmask, 0U);
    EXPECT_NE(addr, HADDR_UNDEF);
    EXPECT_EQ(csize, 4 * sizeof(int32_t));

    // Query an unallocated chunk coordinate (hits !found branch in get_addr).
    constexpr std::array<hsize_t, 2> kCoordMissing = {0, 2};
    fmask = 999;
    addr = 123;
    csize = 999;
    ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), kCoordMissing.data(),
                                         &fmask, &addr, &csize),
              0);
    EXPECT_EQ(addr, HADDR_UNDEF);
    EXPECT_EQ(csize, 0U);

    // Read full 6x6 dataset and verify written chunks + default zero fill.
    std::array<int32_t, 36> read_buf{};
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, read_buf.data()),
              0);
    EXPECT_EQ(read_buf[0 * 6 + 0], 100);
    EXPECT_EQ(read_buf[2 * 6 + 4], 200);
    EXPECT_EQ(read_buf[0 * 6 + 2], 0);
  }

  // Reopen image #2: iterate FIRST while bt2 is closed (!IS_OPEN), then test
  // H5_ITER_STOP, H5_ITER_ERROR, and H5Dget_num_chunks.
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
    EXPECT_EQ(full_state.records.size(), 3U);

    // Second iterate while open exercises H5B2_patch_file branch + H5_ITER_STOP.
    ChunkIterState stop_state{.rank = 2, .stop_after = 1};
    EXPECT_GE(H5Dchunk_iter(dset.get(), H5P_DEFAULT, CollectChunkCallback,
                            &stop_state),
              0);
    EXPECT_EQ(stop_state.records.size(), 1U);

    // Error callback return (< 0) exercises iterate error paths.
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
    EXPECT_EQ(num_chunks, 3U);
  }

  // Reopen image #3: call H5Dset_extent shrink FIRST while bt2 is closed
  // (!IS_OPEN), then shrink again while bt2 is open, then copy via H5Ocopy,
  // convert to v1 B-tree, and delete the dataset via H5Ldelete.
  {
    ScopedFile file = OpenCoreFileImage(image, H5F_ACC_RDWR, H5F_LIBVER_V110,
                                        H5F_LIBVER_V110);
    ASSERT_TRUE(file.valid());
    {
      ScopedDataset dset(H5Dopen2(file.get(), "dset", H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());

      // Prune chunk {4, 2} while bt2 is not yet open.
      constexpr std::array<hsize_t, 2> kShrink1 = {4, 6};
      ASSERT_GE(H5Dset_extent(dset.get(), kShrink1.data()), 0);

      // Prune chunk {2, 4} while bt2 is already open.
      constexpr std::array<hsize_t, 2> kShrink2 = {2, 4};
      ASSERT_GE(H5Dset_extent(dset.get(), kShrink2.data()), 0);

      ScopedSpace space(H5Dget_space(dset.get()));
      ASSERT_TRUE(space.valid());
      hsize_t remaining = 0;
      ASSERT_GE(H5Dget_num_chunks(dset.get(), space.get(), &remaining), 0);
      EXPECT_EQ(remaining, 1U);

      std::vector<int32_t> shrunk_vals(8, -1);
      ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                        H5P_DEFAULT, shrunk_vals.data()),
                0);
      EXPECT_EQ(shrunk_vals[0], 100);
      EXPECT_EQ(shrunk_vals[5], 103);
    }

    // Copy object to exercise copy_setup, copy_shutdown, and reset(false).
    ASSERT_GE(H5Ocopy(file.get(), "dset", file.get(), "dset_copy", H5P_DEFAULT,
                      H5P_DEFAULT),
              0);

    // Convert copy_dset's chunk index to v1 B-tree and verify readability.
    {
      ScopedDataset copy_dset(H5Dopen2(file.get(), "dset_copy", H5P_DEFAULT));
      ASSERT_TRUE(copy_dset.valid());
      ScopedSpace copy_space(H5Dget_space(copy_dset.get()));
      ASSERT_TRUE(copy_space.valid());
      std::array<hsize_t, 2> copy_dims{};
      ASSERT_EQ(
          H5Sget_simple_extent_dims(copy_space.get(), copy_dims.data(), nullptr),
          2);
      std::vector<int32_t> pre_conv_vals(
          static_cast<size_t>(copy_dims[0] * copy_dims[1]), -1);
      ASSERT_GE(H5Dread(copy_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                        H5P_DEFAULT, pre_conv_vals.data()),
                0);
      EXPECT_EQ(pre_conv_vals[0], 100);
      EXPECT_EQ(pre_conv_vals[copy_dims[1] + 1], 103);

      ASSERT_GE(H5Dformat_convert(copy_dset.get()), 0);
      std::vector<int32_t> copied_vals(
          static_cast<size_t>(copy_dims[0] * copy_dims[1]), -1);
      ASSERT_GE(H5Dread(copy_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                        H5P_DEFAULT, copied_vals.data()),
                0);
      EXPECT_EQ(copied_vals[0], 100);
      EXPECT_EQ(copied_vals[copy_dims[1] + 1], 103);
    }

    // Delete original populated v2 B-tree dataset (frees chunks via remove_cb).
    ASSERT_GE(H5Ldelete(file.get(), "dset", H5P_DEFAULT), 0);
  }
}

TEST(H5Dbtree2Test, MultiLevelTreeSplitsAndMerges) {
  // Use small 1x1 int32_t chunks on a 2D unlimited dataset and insert 200 chunks
  // so v2 B-tree nodes split into multi-level trees, then shrink to prune nodes.
  constexpr std::array<hsize_t, 2> kDims = {20, 10};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, H5S_UNLIMITED};
  constexpr std::array<hsize_t, 2> kChunkDims = {1, 1};

  ScopedPlist fapl =
      MakeCoreFaplWithLibver(H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(
      H5Fcreate("multilevel.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedSpace space(H5Screate_simple(2, kDims.data(), kMaxDims.data()));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, kChunkDims.data()), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "grid", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::vector<int32_t> data(200);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<int32_t>(i * 3 + 1);
  }
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, data.data()),
            0);
  dset.reset();

  std::vector<uint8_t> image = ExtractFileImage(file.get());
  ASSERT_FALSE(image.empty());
  file.reset();

  ScopedFile reopened = OpenCoreFileImage(image, H5F_ACC_RDWR,
                                          H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(reopened.valid());
  ScopedDataset rdset(H5Dopen2(reopened.get(), "grid", H5P_DEFAULT));
  ASSERT_TRUE(rdset.valid());

  std::vector<int32_t> out(200);
  ASSERT_GE(H5Dread(rdset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, out.data()),
            0);
  EXPECT_EQ(out, data);

  constexpr std::array<hsize_t, 2> kPruned = {5, 4};
  ASSERT_GE(H5Dset_extent(rdset.get(), kPruned.data()), 0);

  ScopedSpace rspace(H5Dget_space(rdset.get()));
  ASSERT_TRUE(rspace.valid());
  hsize_t nchunks = 0;
  ASSERT_GE(H5Dget_num_chunks(rdset.get(), rspace.get(), &nchunks), 0);
  EXPECT_EQ(nchunks, 20U);
}

TEST(H5Dbtree2Test, FilteredChunksV4AndV5EncodeDecodeAndUpdate) {
  ASSERT_GT(H5Zfilter_avail(H5Z_FILTER_DEFLATE), 0);

  constexpr std::array<hsize_t, 2> kDims = {32, 32};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, H5S_UNLIMITED};
  constexpr std::array<hsize_t, 2> kChunkDims = {16, 16};

  for (const H5F_libver_t libver : {H5F_LIBVER_V110, H5F_LIBVER_LATEST}) {
    std::vector<uint8_t> image;
    {
      ScopedPlist fapl = MakeCoreFaplWithLibver(libver, libver);
      ASSERT_TRUE(fapl.valid());
      ScopedFile file(H5Fcreate("filtered.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
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

      // Write uniform zero chunk at {0, 0} -> small compressed size.
      WriteChunkBlock(dset.get(), {0, 0}, kChunkDims, 0);
      // Zero all elements of chunk {0, 0} cleanly for high compression.
      {
        ScopedSpace fs(H5Dget_space(dset.get()));
        ASSERT_TRUE(fs.valid());
        constexpr std::array<hsize_t, 2> kZeroStart = {0, 0};
        ASSERT_GE(H5Sselect_hyperslab(fs.get(), H5S_SELECT_SET,
                                      kZeroStart.data(), nullptr,
                                      kChunkDims.data(), nullptr),
                  0);
        ScopedSpace ms(H5Screate_simple(2, kChunkDims.data(), nullptr));
        ASSERT_TRUE(ms.valid());
        std::vector<int32_t> zeros(16 * 16, 0);
        ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, ms.get(), fs.get(),
                           H5P_DEFAULT, zeros.data()),
                  0);
      }

      // Write another chunk at {16, 16}.
      WriteChunkBlock(dset.get(), {16, 16}, kChunkDims, 5000);
      ASSERT_GE(H5Dflush(dset.get()), 0);

      hsize_t compressed_before = 0;
      constexpr std::array<hsize_t, 2> kOrigin = {0, 0};
      ASSERT_GE(H5Dget_chunk_storage_size(dset.get(), kOrigin.data(),
                                          &compressed_before),
                0);
      EXPECT_LT(compressed_before, 16 * 16 * sizeof(int32_t));

      // Overwrite chunk {0, 0} with incompressible pseudo-random int32_t values
      // to force chunk reallocation and H5D__bt2_mod_cb update.
      {
        ScopedSpace fs(H5Dget_space(dset.get()));
        ASSERT_TRUE(fs.valid());
        ASSERT_GE(H5Sselect_hyperslab(fs.get(), H5S_SELECT_SET, kOrigin.data(),
                                      nullptr, kChunkDims.data(), nullptr),
                  0);
        ScopedSpace ms(H5Screate_simple(2, kChunkDims.data(), nullptr));
        ASSERT_TRUE(ms.valid());
        std::vector<int32_t> noise(16 * 16);
        uint32_t rng = 0x12345678u;
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

    // Reopen filtered image to exercise H5D__bt2_filt_decode and copy/delete.
    ScopedFile file = OpenCoreFileImage(image, H5F_ACC_RDWR, libver, libver);
    ASSERT_TRUE(file.valid());
    {
      ScopedDataset dset(H5Dopen2(file.get(), "fdset", H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());

      ChunkIterState it{.rank = 2};
      ASSERT_EQ(H5Dchunk_iter(dset.get(), H5P_DEFAULT, CollectChunkCallback, &it),
                H5_ITER_CONT);
      EXPECT_EQ(it.records.size(), 2U);

      std::vector<int32_t> read_back(32 * 32);
      ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                        H5P_DEFAULT, read_back.data()),
                0);
      EXPECT_EQ(read_back[16 * 32 + 16], 5000);
    }
    ASSERT_GE(
        H5Ocopy(file.get(), "fdset", file.get(), "fdset_copy", H5P_DEFAULT,
                H5P_DEFAULT),
        0);
    ASSERT_GE(H5Ldelete(file.get(), "fdset", H5P_DEFAULT), 0);
  }
}

TEST(H5Dbtree2Test, SwmrWriteCreatesDependsOpensAndPrunesWithoutFreeing) {
  // SWMR requires a VFD with H5FD_FEAT_SUPPORTS_SWMR_IO (Sec2 VFD).
  TempFile temp_h5("bt2_swmr.h5");

  constexpr std::array<hsize_t, 2> kDims = {4, 4};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, H5S_UNLIMITED};
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
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, kChunkDims.data()), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_INCR), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "swmr_dset", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    // Enter SWMR write BEFORE index creation so idx_create triggers depend().
    ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);

    WriteChunkBlock(dset.get(), {0, 0}, kChunkDims, 11);
    WriteChunkBlock(dset.get(), {2, 2}, kChunkDims, 22);
    ASSERT_GE(H5Dflush(dset.get()), 0);
  }

  // Reopen in H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE so idx_open triggers depend(),
  // then shrink extent (hits SWMR remove branch) and delete dataset.
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
      ScopedDataset dset(H5Dopen2(file.get(), "swmr_dset", H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());

      unsigned fmask = 0;
      haddr_t addr = HADDR_UNDEF;
      hsize_t csize = 0;
      constexpr std::array<hsize_t, 2> kCoord = {0, 0};
      ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), kCoord.data(), &fmask,
                                           &addr, &csize),
                0);
      EXPECT_NE(addr, HADDR_UNDEF);

      // Test shrink under SWMR_WRITE (hits SWMR NULL remove_cb ternaries).
      {
        ScopedErrorSilence silence;
        constexpr std::array<hsize_t, 2> kShrink = {2, 2};
        (void)H5Dset_extent(dset.get(), kShrink.data());
      }
    }
    {
      ScopedErrorSilence silence;
      (void)H5Ldelete(file.get(), "swmr_dset", H5P_DEFAULT);
    }
  }
}

TEST(H5Dbtree2Test, InternalIndexOpsDebugDumpLoadMetadataAndClamp) {
  constexpr std::array<hsize_t, 2> kDims = {4, 4};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, H5S_UNLIMITED};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 2};

  // Helper to build a file image with both unfiltered and filtered datasets.
  std::vector<uint8_t> image;
  {
    ScopedPlist fapl =
        MakeCoreFaplWithLibver(H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("internal.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedSpace space(H5Screate_simple(2, kDims.data(), kMaxDims.data()));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl_u(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_u.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_u.get(), 2, kChunkDims.data()), 0);
    ScopedDataset du(H5Dcreate2(file.get(), "unfilt", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl_u.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(du.valid());
    WriteChunkBlock(du.get(), {0, 0}, kChunkDims, 10);

    ScopedPlist dcpl_f(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_f.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_f.get(), 2, kChunkDims.data()), 0);
    ASSERT_GE(H5Pset_deflate(dcpl_f.get(), 6), 0);
    ScopedDataset df(H5Dcreate2(file.get(), "filt", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl_f.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(df.valid());
    WriteChunkBlock(df.get(), {0, 0}, kChunkDims, 20);

    du.reset();
    df.reset();
    image = ExtractFileImage(file.get());
    ASSERT_FALSE(image.empty());
  }

  ScopedFile file = OpenCoreFileImage(image, H5F_ACC_RDWR, H5F_LIBVER_LATEST,
                                      H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());

  ScopedDataset du(H5Dopen2(file.get(), "unfilt", H5P_DEFAULT));
  ASSERT_TRUE(du.valid());
  ScopedDataset df(H5Dopen2(file.get(), "filt", H5P_DEFAULT));
  ASSERT_TRUE(df.valid());

  auto* dset_u = static_cast<H5D_t*>(H5VL_object_verify(du.get(), H5I_DATASET));
  ASSERT_NE(dset_u, nullptr);
  auto* dset_f = static_cast<H5D_t*>(H5VL_object_verify(df.get(), H5I_DATASET));
  ASSERT_NE(dset_f, nullptr);

  // Check is_open, insert(!IS_OPEN), load_metadata, dump, and debug callbacks.
  {
    ScopedApiContext ctx(dset_u->oloc.addr);
    ASSERT_TRUE(ctx.valid());

    H5O_storage_chunk_t* sc_u = &dset_u->shared->layout.storage.u.chunk;
    H5D_chk_idx_info_t idx_info_u{};
    idx_info_u.f = dset_u->oloc.file;
    idx_info_u.pline = &dset_u->shared->dcpl_cache.pline;
    idx_info_u.layout = &dset_u->shared->layout;

    bool is_open = true;
    ASSERT_GE(sc_u->ops->is_open(&idx_info_u, &is_open), 0);
    EXPECT_FALSE(is_open);

    // Attempt removing a nonexistent chunk while !IS_OPEN so
    // H5D__bt2_idx_remove opens bt2 lazily, then close it via dest() so
    // insert() below also hits its !IS_OPEN branch.
    {
      ScopedErrorSilence silence;
      hsize_t missing_scaled[H5O_LAYOUT_NDIMS] = {9, 9, 0};
      H5D_chunk_common_ud_t rm_ud{};
      rm_ud.layout = &dset_u->shared->layout.u.chunk;
      rm_ud.storage = sc_u;
      rm_ud.scaled = missing_scaled;
      EXPECT_LT(sc_u->ops->remove(&idx_info_u, &rm_ud), 0);
    }
    ASSERT_GE(sc_u->ops->dest(&idx_info_u), 0);
    ASSERT_GE(sc_u->ops->is_open(&idx_info_u, &is_open), 0);
    EXPECT_FALSE(is_open);

    // Allocate a valid chunk at {2, 2} (scaled = {1, 1, 0}) and insert while
    // !IS_OPEN so H5D__bt2_idx_insert opens the v2 B-tree lazily.
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
    std::vector<int32_t> chunk_bytes(4, 99);
    ASSERT_GE(H5F_block_write(idx_info_u.f, H5FD_MEM_DRAW,
                              udata.chunk_block.offset, chunk_bytes.size() * 4,
                              chunk_bytes.data()),
              0);
    ASSERT_GE(sc_u->ops->insert(&idx_info_u, &udata, dset_u), 0);

    ASSERT_GE(sc_u->ops->is_open(&idx_info_u, &is_open), 0);
    EXPECT_TRUE(is_open);

    // load_metadata runs internal fake lookup.
    ASSERT_GE(sc_u->ops->load_metadata(&idx_info_u), 0);

    // Dump index header address via ops->dump (H5D__bt2_idx_dump) into an
    // in-memory FILE* buffer (H5D__chunk_dump_index reads scaled[rank] in
    // H5Dchunk.c:7448, which H5D__bt2_unfilt_decode does not initialize).
    std::array<char, 512> dump_buf{};
    FILE* dump_stream = fmemopen(dump_buf.data(), dump_buf.size() - 1, "w");
    ASSERT_NE(dump_stream, nullptr);
    absl::Cleanup close_dump = [dump_stream]() { std::fclose(dump_stream); };
    ASSERT_GE(sc_u->ops->dump(sc_u, dump_stream), 0);
    std::fflush(dump_stream);
    std::string_view dumped(dump_buf.data());
    EXPECT_THAT(dumped, HasSubstr("Address:"));

    // Invoke H5D__bt2_unfilt_debug directly (no caller exists in HDF5 2.1.0).
    H5B2_t* bt2_u = sc_u->u.btree2.bt2;
    ASSERT_NE(bt2_u, nullptr);
    H5D_chunk_rec_t rec_u{};
    rec_u.scaled[0] = 0;
    rec_u.scaled[1] = 1;
    rec_u.nbytes = dset_u->shared->layout.u.chunk.size;
    rec_u.filter_mask = 0;
    rec_u.chunk_addr = 4096;

    std::array<char, 1024> dbg_u_buf{};
    FILE* dbg_u_stream = fmemopen(dbg_u_buf.data(), dbg_u_buf.size() - 1, "w");
    ASSERT_NE(dbg_u_stream, nullptr);
    absl::Cleanup close_dbg_u = [dbg_u_stream]() { std::fclose(dbg_u_stream); };
    ASSERT_GE(
        H5D_BT2[0].debug(dbg_u_stream, 2, 20, &rec_u, bt2_u->hdr->cb_ctx), 0);
    std::fflush(dbg_u_stream);
    std::string_view dbg_u_text(dbg_u_buf.data());
    EXPECT_THAT(dbg_u_text, HasSubstr("Chunk address:"));
    EXPECT_THAT(dbg_u_text, HasSubstr("Logical offset:"));

    // Cover H5D__bt2_idx_reset(..., false) too.
    H5O_storage_chunk_t temp_sc = *sc_u;
    ASSERT_GE(H5D_chunk_idx_reset(&temp_sc, false), 0);
    EXPECT_NE(temp_sc.idx_addr, HADDR_UNDEF);
    EXPECT_EQ(temp_sc.u.btree2.bt2, nullptr);
  }

  // Exercise H5D__bt2_filt_debug and v4 chunk_size_len > 8 clamp.
  {
    ScopedApiContext ctx(dset_f->oloc.addr);
    ASSERT_TRUE(ctx.valid());

    H5O_storage_chunk_t* sc_f = &dset_f->shared->layout.storage.u.chunk;
    H5D_chk_idx_info_t idx_info_f{};
    idx_info_f.f = dset_f->oloc.file;
    idx_info_f.pline = &dset_f->shared->dcpl_cache.pline;
    idx_info_f.layout = &dset_f->shared->layout;

    // Open bt2 header via load_metadata so bt2_f->hdr->cb_ctx is available.
    ASSERT_GE(sc_f->ops->load_metadata(&idx_info_f), 0);
    H5B2_t* bt2_f = sc_f->u.btree2.bt2;
    ASSERT_NE(bt2_f, nullptr);

    H5D_chunk_rec_t rec_f{};
    rec_f.scaled[0] = 0;
    rec_f.scaled[1] = 1;
    rec_f.nbytes = 42;
    rec_f.filter_mask = 0x1u;
    rec_f.chunk_addr = 8192;

    std::array<char, 1024> dbg_f_buf{};
    FILE* dbg_f_stream = fmemopen(dbg_f_buf.data(), dbg_f_buf.size() - 1, "w");
    ASSERT_NE(dbg_f_stream, nullptr);
    absl::Cleanup close_dbg_f = [dbg_f_stream]() { std::fclose(dbg_f_stream); };
    ASSERT_GE(H5D_BT2_FILT[0].debug(dbg_f_stream, 2, 20, &rec_f,
                                    bt2_f->hdr->cb_ctx),
              0);
    std::fflush(dbg_f_stream);
    std::string_view dbg_f_text(dbg_f_buf.data());
    EXPECT_THAT(dbg_f_text, HasSubstr("Chunk size:"));
    EXPECT_THAT(dbg_f_text, HasSubstr("Filter mask:"));

    // Synthetic layout with v4 and chunk.size >= 2^56 exercises the
    // chunk_size_len > 8 clamp in H5D_BT2_COMPUTE_CHUNK_SIZE_LEN without
    // allocating chunk data.
    H5O_layout_t synth_layout = dset_f->shared->layout;
    synth_layout.version = H5O_LAYOUT_VERSION_4;
    synth_layout.u.chunk.size = uint64_t{1} << 56;
    synth_layout.storage.u.chunk.idx_addr = HADDR_UNDEF;
    synth_layout.storage.u.chunk.u.btree2.bt2 = nullptr;

    H5D_chk_idx_info_t synth_info{};
    synth_info.f = dset_f->oloc.file;
    synth_info.pline = &dset_f->shared->dcpl_cache.pline;
    synth_info.layout = &synth_layout;

    ASSERT_GE(H5D_COPS_BT2[0].create(&synth_info), 0);
    EXPECT_NE(synth_layout.storage.u.chunk.idx_addr, HADDR_UNDEF);
    ASSERT_GE(H5D_COPS_BT2[0].dest(&synth_info), 0);
    ASSERT_GE(H5D_COPS_BT2[0].idx_delete(&synth_info), 0);
  }
}

}  // namespace
}  // namespace hdf5
