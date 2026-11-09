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
// Tests Fixed Array chunk indexing for datasets with fixed dimensions,
// covering unfiltered and filtered records across layout versions 4 and 5,
// lazy reopen, chunk overwrite, extent shrinking/pruning, paged data blocks,
// object copy/delete, SWMR flush dependencies, and internal index operations
// (third_party/hdf5/src/H5Dfarray.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dfarray.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// H5C_cache_entry_t (H5AC_info_t) inside internal metadata structs. Define
// NDEBUG around both `hdf5_internal_prelude.h` and internal headers so struct
// layouts match the compiled library.
#ifndef NDEBUG
#define H5DFARRAY_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5D_FRIEND
// H5B2private.h (included by H5Dpkg.h) uses `try` as a parameter name, which
// collides with libc++'s -fno-exceptions `#define try if (1)` macro and the
// C++ `try` keyword.
#pragma push_macro("try")
#undef try
#define try h5b2_try_param
#include "third_party/hdf5/src/H5Dpkg.h"
#undef try
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FAprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5DFARRAY_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DFARRAY_TEST_RESTORE_UNDEF_NDEBUG
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

ScopedPlist MakeCoreFaplWithLibver(H5F_libver_t low, H5F_libver_t high) {
  ScopedPlist fapl = MakeCoreFapl();
  if (fapl.valid() && H5Pset_libver_bounds(fapl.get(), low, high) < 0) {
    fapl.reset();
  }
  return fapl;
}

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

template <size_t N>
void WriteChunkBlock(hid_t dset_id, const std::array<hsize_t, N>& start,
                     const std::array<hsize_t, N>& count, int32_t base_val) {
  ScopedSpace fspace(H5Dget_space(dset_id));
  ASSERT_TRUE(fspace.valid());
  ASSERT_GE(H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, start.data(),
                                nullptr, count.data(), nullptr),
            0);
  ScopedSpace mspace(H5Screate_simple(static_cast<int>(N), count.data(), nullptr));
  ASSERT_TRUE(mspace.valid());
  hsize_t total = 1;
  for (hsize_t c : count) {
    total *= c;
  }
  std::vector<int32_t> data(static_cast<size_t>(total));
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = base_val + static_cast<int32_t>(i);
  }
  ASSERT_GE(H5Dwrite(dset_id, H5T_NATIVE_INT32, mspace.get(), fspace.get(),
                     H5P_DEFAULT, data.data()),
            0);
}

TEST(H5DfarrayTest, UnfilteredLifecycleCreateReopenIterateShrinkDelete) {
  // Fixed 3D dataset (6x6x4) with 2x2x2 chunks -> 3x3x2 = 18 chunks uses
  // H5D_CHUNK_IDX_FARRAY and exercises multi-dim coordinate carry-over.
  constexpr std::array<hsize_t, 3> kDims = {6, 6, 4};
  constexpr std::array<hsize_t, 3> kChunkDims = {2, 2, 2};

  std::vector<uint8_t> image;
  {
    ScopedPlist fapl =
        MakeCoreFaplWithLibver(H5F_LIBVER_V110, H5F_LIBVER_V110);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("farray_unfiltered.h5", H5F_ACC_TRUNC,
                              H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedSpace space(H5Screate_simple(3, kDims.data(), kDims.data()));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 3, kChunkDims.data()), 0);
    // H5D_ALLOC_TIME_INCR selects Fixed Array instead of None index.
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_INCR), 0);
    const int32_t fill_val = -7;
    ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_INT32, &fill_val), 0);

    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl.valid());
    ASSERT_GE(H5Pset_chunk_cache(dapl.get(), 0, 0, 0.0), 0);

    // Empty dataset never written -> exercises idx_delete when idx_addr is
    // HADDR_UNDEF.
    {
      ScopedDataset empty_dset(H5Dcreate2(file.get(), "empty_dset",
                                          H5T_NATIVE_INT32, space.get(),
                                          H5P_DEFAULT, dcpl.get(), dapl.get()));
      ASSERT_TRUE(empty_dset.valid());
      H5D_chunk_index_t idx_type = H5D_CHUNK_IDX_BTREE;
      ASSERT_GE(H5Dget_chunk_index_type(empty_dset.get(), &idx_type), 0);
      EXPECT_EQ(idx_type, H5D_CHUNK_IDX_FARRAY);

      H5D_space_status_t status = H5D_SPACE_STATUS_ERROR;
      ASSERT_GE(H5Dget_space_status(empty_dset.get(), &status), 0);
      EXPECT_EQ(status, H5D_SPACE_STATUS_NOT_ALLOCATED);
    }
    ASSERT_GE(H5Ldelete(file.get(), "empty_dset", H5P_DEFAULT), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "grid3d", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  dapl.get()));
    ASSERT_TRUE(dset.valid());

    // Write sparse chunks across planes to test partial fill & multi-dim wrap.
    WriteChunkBlock(dset.get(), {0, 0, 0}, kChunkDims, 100);
    WriteChunkBlock(dset.get(), {2, 4, 2}, kChunkDims, 200);
    WriteChunkBlock(dset.get(), {4, 4, 2}, kChunkDims, 300);
    // Overwrite an existing chunk while open.
    WriteChunkBlock(dset.get(), {0, 0, 0}, kChunkDims, 150);

    H5D_space_status_t status = H5D_SPACE_STATUS_ERROR;
    ASSERT_GE(H5Dget_space_status(dset.get(), &status), 0);
    EXPECT_EQ(status, H5D_SPACE_STATUS_PART_ALLOCATED);

    H5O_native_info_t ninfo{};
    ASSERT_GE(H5Oget_native_info_by_name(file.get(), "grid3d", &ninfo,
                                         H5O_NATIVE_INFO_META_SIZE,
                                         H5P_DEFAULT),
              0);
    EXPECT_GT(ninfo.meta_size.obj.index_size, 0U);

    // Paged dataset (> 1024 chunks, default page size is 2^10 = 1024) to
    // exercise multi-element H5FA_CLS_CHUNK fill, encode, and decode.
    constexpr std::array<hsize_t, 2> kPagedDims = {64, 33};
    constexpr std::array<hsize_t, 2> kPagedChunk = {1, 1};
    ScopedSpace paged_space(
        H5Screate_simple(2, kPagedDims.data(), kPagedDims.data()));
    ASSERT_TRUE(paged_space.valid());
    ScopedPlist paged_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(paged_dcpl.valid());
    ASSERT_GE(H5Pset_chunk(paged_dcpl.get(), 2, kPagedChunk.data()), 0);
    ASSERT_GE(H5Pset_alloc_time(paged_dcpl.get(), H5D_ALLOC_TIME_INCR), 0);
    ScopedDataset paged(H5Dcreate2(file.get(), "paged", H5T_NATIVE_INT32,
                                   paged_space.get(), H5P_DEFAULT,
                                   paged_dcpl.get(), dapl.get()));
    ASSERT_TRUE(paged.valid());
    WriteChunkBlock(paged.get(), {0, 0}, kPagedChunk, 11);
    WriteChunkBlock(paged.get(), {63, 32}, kPagedChunk, 22);

    dset.reset();
    paged.reset();
    image = ExtractFileImage(file.get());
    ASSERT_FALSE(image.empty());
  }

  // Reopen 1: Query chunk info while index is closed (!IS_OPEN -> get_addr),
  // including an unallocated chunk (addr == HADDR_UNDEF -> length = 0).
  {
    std::vector<uint8_t> copy_img = image;
    ScopedFile file = OpenCoreFileImage(copy_img, H5F_ACC_RDWR, H5F_LIBVER_V110,
                                        H5F_LIBVER_V110);
    ASSERT_TRUE(file.valid());

    ScopedDataset dset(H5Dopen2(file.get(), "grid3d", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    haddr_t addr = HADDR_UNDEF;
    hsize_t csize = 0;
    unsigned fmask = 0;
    constexpr std::array<hsize_t, 3> kAllocated = {2, 4, 2};
    ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), kAllocated.data(), &fmask,
                                         &addr, &csize),
              0);
    EXPECT_NE(addr, HADDR_UNDEF);
    EXPECT_EQ(csize, 8 * sizeof(int32_t));

    constexpr std::array<hsize_t, 3> kUnallocated = {0, 2, 0};
    ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), kUnallocated.data(), &fmask,
                                         &addr, &csize),
              0);
    EXPECT_EQ(addr, HADDR_UNDEF);
    EXPECT_EQ(csize, 0U);
  }

  // Reopen 2: Iterate while index is closed (!IS_OPEN -> iterate), test
  // CONT / STOP / ERROR return codes, read data including unallocated fill,
  // and iterate paged dataset.
  {
    std::vector<uint8_t> copy_img = image;
    ScopedFile file = OpenCoreFileImage(copy_img, H5F_ACC_RDWR, H5F_LIBVER_V110,
                                        H5F_LIBVER_V110);
    ASSERT_TRUE(file.valid());

    ScopedDataset dset(H5Dopen2(file.get(), "grid3d", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    ChunkIterState full_it{.rank = 3};
    ASSERT_EQ(H5Dchunk_iter(dset.get(), H5P_DEFAULT, CollectChunkCallback,
                            &full_it),
              H5_ITER_CONT);
    EXPECT_EQ(full_it.records.size(), 3U);

    ChunkIterState stop_it{.rank = 3, .stop_after = 2};
    EXPECT_GE(H5Dchunk_iter(dset.get(), H5P_DEFAULT, CollectChunkCallback,
                            &stop_it),
              0);
    EXPECT_EQ(stop_it.records.size(), 2U);

    {
      ScopedErrorSilence silence;
      ChunkIterState err_it{.rank = 3, .fail_on_first = true};
      EXPECT_LT(H5Dchunk_iter(dset.get(), H5P_DEFAULT, CollectChunkCallback,
                              &err_it),
                0);
    }

    std::vector<int32_t> read_back(6 * 6 * 4, 0);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, read_back.data()),
              0);
    // Origin overwritten to 150.
    EXPECT_EQ(read_back[0], 150);
    // Unallocated voxel returns fill value -7.
    EXPECT_EQ(read_back[0 * 24 + 2 * 4 + 0], -7);

    ScopedDataset paged(H5Dopen2(file.get(), "paged", H5P_DEFAULT));
    ASSERT_TRUE(paged.valid());
    ChunkIterState paged_it{.rank = 2};
    ASSERT_EQ(H5Dchunk_iter(paged.get(), H5P_DEFAULT, CollectChunkCallback,
                            &paged_it),
              H5_ITER_CONT);
    EXPECT_EQ(paged_it.records.size(), 2U);
  }

  // Reopen 3: Shrink immediately while !IS_OPEN (remove opens fa lazily),
  // copy closed and open datasets via H5Ocopy, and delete populated datasets.
  {
    std::vector<uint8_t> copy_img = image;
    ScopedFile file = OpenCoreFileImage(copy_img, H5F_ACC_RDWR, H5F_LIBVER_V110,
                                        H5F_LIBVER_V110);
    ASSERT_TRUE(file.valid());

    // Copy before opening in memory -> copy_setup opens src index lazily.
    ASSERT_GE(H5Ocopy(file.get(), "grid3d", file.get(), "grid3d_copied",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);

    {
      ScopedDataset dset(H5Dopen2(file.get(), "grid3d", H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
      // Shrink immediately while index is still closed.
      constexpr std::array<hsize_t, 3> kShrink1 = {4, 6, 4};
      ASSERT_GE(H5Dset_extent(dset.get(), kShrink1.data()), 0);

      // Shrink again while index is already open (H5FA_patch_file branch).
      constexpr std::array<hsize_t, 3> kShrink2 = {2, 2, 2};
      ASSERT_GE(H5Dset_extent(dset.get(), kShrink2.data()), 0);

      ScopedSpace sp(H5Dget_space(dset.get()));
      ASSERT_TRUE(sp.valid());
      hsize_t nchunks = 0;
      ASSERT_GE(H5Dget_num_chunks(dset.get(), sp.get(), &nchunks), 0);
      EXPECT_EQ(nchunks, 1U);
    }

    ASSERT_GE(H5Ldelete(file.get(), "grid3d", H5P_DEFAULT), 0);
    ASSERT_GE(H5Ldelete(file.get(), "grid3d_copied", H5P_DEFAULT), 0);
    ASSERT_GE(H5Ldelete(file.get(), "paged", H5P_DEFAULT), 0);
  }
}

TEST(H5DfarrayTest, FilteredChunksV4AndV5EncodeDecodeCopyAndDelete) {
  ASSERT_GT(H5Zfilter_avail(H5Z_FILTER_DEFLATE), 0);

  constexpr std::array<hsize_t, 2> kDims = {32, 32};
  constexpr std::array<hsize_t, 2> kChunkDims = {16, 16};

  for (const H5F_libver_t libver : {H5F_LIBVER_V110, H5F_LIBVER_LATEST}) {
    std::vector<uint8_t> image;
    {
      ScopedPlist fapl = MakeCoreFaplWithLibver(libver, libver);
      ASSERT_TRUE(fapl.valid());
      ScopedFile file(H5Fcreate("farray_filtered.h5", H5F_ACC_TRUNC,
                                H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(file.valid());

      ScopedSpace space(H5Screate_simple(2, kDims.data(), kDims.data()));
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

      H5D_chunk_index_t idx_type = H5D_CHUNK_IDX_BTREE;
      ASSERT_GE(H5Dget_chunk_index_type(dset.get(), &idx_type), 0);
      EXPECT_EQ(idx_type, H5D_CHUNK_IDX_FARRAY);

      // Write uniform zero chunk at {0, 0} -> small compressed size.
      {
        ScopedSpace fs(H5Dget_space(dset.get()));
        ASSERT_TRUE(fs.valid());
        constexpr std::array<hsize_t, 2> kOrigin = {0, 0};
        ASSERT_GE(H5Sselect_hyperslab(fs.get(), H5S_SELECT_SET, kOrigin.data(),
                                      nullptr, kChunkDims.data(), nullptr),
                  0);
        ScopedSpace ms(H5Screate_simple(2, kChunkDims.data(), nullptr));
        ASSERT_TRUE(ms.valid());
        std::vector<int32_t> zeros(16 * 16, 0);
        ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, ms.get(), fs.get(),
                           H5P_DEFAULT, zeros.data()),
                  0);
      }

      // Leave {0, 16} and {16, 0} unallocated; write chunk {16, 16}.
      WriteChunkBlock(dset.get(), {16, 16}, kChunkDims, 5000);
      ASSERT_GE(H5Dflush(dset.get()), 0);

      hsize_t compressed_before = 0;
      constexpr std::array<hsize_t, 2> kOrigin = {0, 0};
      ASSERT_GE(H5Dget_chunk_storage_size(dset.get(), kOrigin.data(),
                                          &compressed_before),
                0);
      EXPECT_LT(compressed_before, 16 * 16 * sizeof(int32_t));

      // Overwrite chunk {0, 0} with incompressible pseudo-random int32_t data.
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

    // Reopen to exercise H5D__farray_filt_decode, H5D__farray_filt_fill,
    // filtered shrink/remove, copy, and delete_cb.
    ScopedFile file = OpenCoreFileImage(image, H5F_ACC_RDWR, libver, libver);
    ASSERT_TRUE(file.valid());
    {
      ScopedDataset dset(H5Dopen2(file.get(), "fdset", H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());

      ChunkIterState it{.rank = 2};
      ASSERT_EQ(H5Dchunk_iter(dset.get(), H5P_DEFAULT, CollectChunkCallback,
                              &it),
                H5_ITER_CONT);
      EXPECT_EQ(it.records.size(), 2U);

      haddr_t addr = HADDR_UNDEF;
      hsize_t csize = 0;
      unsigned fmask = 0;
      constexpr std::array<hsize_t, 2> kMissing = {0, 16};
      ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), kMissing.data(), &fmask,
                                           &addr, &csize),
                0);
      EXPECT_EQ(addr, HADDR_UNDEF);
      EXPECT_EQ(csize, 0U);

      std::vector<int32_t> read_back(32 * 32);
      ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                        H5P_DEFAULT, read_back.data()),
                0);
      EXPECT_EQ(read_back[16 * 32 + 16], 5000);

      // Shrink dataset to prune filtered chunk {16, 16}.
      constexpr std::array<hsize_t, 2> kShrink = {16, 16};
      ASSERT_GE(H5Dset_extent(dset.get(), kShrink.data()), 0);
    }

    ASSERT_GE(H5Ocopy(file.get(), "fdset", file.get(), "fdset_copy",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ldelete(file.get(), "fdset", H5P_DEFAULT), 0);
    ASSERT_GE(H5Ldelete(file.get(), "fdset_copy", H5P_DEFAULT), 0);
  }
}

TEST(H5DfarrayTest, SwmrWriteCreatesDependsOpensAndRemoves) {
  ASSERT_GT(H5Zfilter_avail(H5Z_FILTER_DEFLATE), 0);
  TempFile temp_h5("farray_swmr.h5");

  constexpr std::array<hsize_t, 2> kDims = {4, 4};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 2};

  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
        0);

    ScopedFile file(H5Fcreate(temp_h5.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedSpace space(H5Screate_simple(2, kDims.data(), kDims.data()));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl_u(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_u.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_u.get(), 2, kChunkDims.data()), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl_u.get(), H5D_ALLOC_TIME_INCR), 0);

    ScopedPlist dcpl_f(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_f.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_f.get(), 2, kChunkDims.data()), 0);
    ASSERT_GE(H5Pset_deflate(dcpl_f.get(), 6), 0);

    ScopedDataset du(H5Dcreate2(file.get(), "swmr_unfilt", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl_u.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(du.valid());
    ScopedDataset df(H5Dcreate2(file.get(), "swmr_filt", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl_f.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(df.valid());

    // Enter SWMR write BEFORE index creation so idx_create triggers depend().
    ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);

    WriteChunkBlock(du.get(), {0, 0}, kChunkDims, 11);
    WriteChunkBlock(du.get(), {2, 2}, kChunkDims, 22);
    WriteChunkBlock(df.get(), {0, 0}, kChunkDims, 33);
    WriteChunkBlock(df.get(), {2, 2}, kChunkDims, 44);
    ASSERT_GE(H5Dflush(du.get()), 0);
    ASSERT_GE(H5Dflush(df.get()), 0);
  }

  // Reopen in SWMR_WRITE so idx_open triggers depend(), and exercise SWMR
  // remove branches without freeing chunk file blocks.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
        0);

    ScopedFile file(H5Fopen(temp_h5.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE,
                            fapl.get()));
    ASSERT_TRUE(file.valid());

    for (const char* name : {"swmr_unfilt", "swmr_filt"}) {
      ScopedDataset dset(H5Dopen2(file.get(), name, H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());

      unsigned fmask = 0;
      haddr_t addr = HADDR_UNDEF;
      hsize_t csize = 0;
      constexpr std::array<hsize_t, 2> kCoord = {0, 0};
      ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), kCoord.data(), &fmask,
                                           &addr, &csize),
                0);
      EXPECT_NE(addr, HADDR_UNDEF);

      // Directly invoke ops->remove on an allocated chunk under SWMR_WRITE so
      // both unfiltered and filtered !(SWMR_WRITE) false branches are covered.
      auto* dset_ptr =
          static_cast<H5D_t*>(H5VL_object_verify(dset.get(), H5I_DATASET));
      ASSERT_NE(dset_ptr, nullptr);
      ScopedApiContext ctx(dset_ptr->oloc.addr);
      ASSERT_TRUE(ctx.valid());

      H5O_storage_chunk_t* sc = &dset_ptr->shared->layout.storage.u.chunk;
      H5D_chk_idx_info_t idx_info{};
      idx_info.f = dset_ptr->oloc.file;
      idx_info.pline = &dset_ptr->shared->dcpl_cache.pline;
      idx_info.layout = &dset_ptr->shared->layout;

      hsize_t scaled[H5O_LAYOUT_NDIMS] = {0, 0, 0};
      H5D_chunk_common_ud_t rm_ud{};
      rm_ud.layout = &dset_ptr->shared->layout.u.chunk;
      rm_ud.storage = sc;
      rm_ud.scaled = scaled;
      ASSERT_GE(sc->ops->remove(&idx_info, &rm_ud), 0);

      // Re-verify chunk slot was reset to HADDR_UNDEF.
      H5D_chunk_ud_t verify_ud{};
      verify_ud.common = rm_ud;
      ASSERT_GE(sc->ops->get_addr(&idx_info, &verify_ud), 0);
      EXPECT_EQ(verify_ud.chunk_block.offset, HADDR_UNDEF);
      EXPECT_EQ(verify_ud.chunk_block.length, 0U);
    }
  }
}

TEST(H5DfarrayTest, InternalIndexOpsDebugDumpLoadMetadataClampAndErrors) {
  ASSERT_GT(H5Zfilter_avail(H5Z_FILTER_DEFLATE), 0);

  constexpr std::array<hsize_t, 2> kDims = {4, 4};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 2};

  std::vector<uint8_t> image;
  {
    ScopedPlist fapl =
        MakeCoreFaplWithLibver(H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(H5Fcreate("farray_internal.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedGroup grp(H5Gcreate2(file.get(), "not_a_dset", H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    ScopedSpace space(H5Screate_simple(2, kDims.data(), kDims.data()));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl_u(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_u.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_u.get(), 2, kChunkDims.data()), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl_u.get(), H5D_ALLOC_TIME_INCR), 0);
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
    grp.reset();
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
  ScopedGroup grp(H5Gopen2(file.get(), "not_a_dset", H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  auto* dset_u = static_cast<H5D_t*>(H5VL_object_verify(du.get(), H5I_DATASET));
  ASSERT_NE(dset_u, nullptr);
  auto* dset_f = static_cast<H5D_t*>(H5VL_object_verify(df.get(), H5I_DATASET));
  ASSERT_NE(dset_f, nullptr);
  auto* grp_ptr = static_cast<H5G_t*>(H5VL_object_verify(grp.get(), H5I_GROUP));
  ASSERT_NE(grp_ptr, nullptr);

  // 1. Unfiltered internal ops: is_open, insert(!IS_OPEN and IS_OPEN),
  // insert range/addr errors, load_metadata, dump, debug, reset, copy_setup
  // (when already open) + copy_shutdown.
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

    // Trigger corrupt idx_addr failure paths while index is closed.
    {
      ScopedErrorSilence silence;
      const haddr_t saved_idx_addr = sc_u->idx_addr;
      absl::Cleanup restore_addr = [sc_u, saved_idx_addr]() {
        sc_u->idx_addr = saved_idx_addr;
      };
      // Point idx_addr at object header ('OHDR' != 'FAHD') so H5FA_open fails.
      sc_u->idx_addr = dset_u->oloc.addr;

      EXPECT_LT(sc_u->ops->open(&idx_info_u), 0);
      EXPECT_LT(sc_u->ops->load_metadata(&idx_info_u), 0);

      hsize_t bad_scaled[H5O_LAYOUT_NDIMS] = {1, 1, 0};
      H5D_chunk_ud_t bad_ud{};
      bad_ud.common.layout = &dset_u->shared->layout.u.chunk;
      bad_ud.common.storage = sc_u;
      bad_ud.common.scaled = bad_scaled;
      bad_ud.chunk_block.offset = dset_u->oloc.addr;
      bad_ud.chunk_block.length = dset_u->shared->layout.u.chunk.size;
      bad_ud.chunk_idx = 1;
      EXPECT_LT(sc_u->ops->insert(&idx_info_u, &bad_ud, dset_u), 0);
      EXPECT_LT(sc_u->ops->get_addr(&idx_info_u, &bad_ud), 0);

      ChunkIterState dummy{.rank = 2};
      EXPECT_LT(sc_u->ops->iterate(&idx_info_u,
                                   [](const H5D_chunk_rec_t*, void*) {
                                     return H5_ITER_CONT;
                                   },
                                   &dummy),
                0);

      H5D_chunk_common_ud_t rm_ud{};
      rm_ud.layout = &dset_u->shared->layout.u.chunk;
      rm_ud.storage = sc_u;
      rm_ud.scaled = bad_scaled;
      EXPECT_LT(sc_u->ops->remove(&idx_info_u, &rm_ud), 0);

      hsize_t idx_size = 0;
      EXPECT_LT(sc_u->ops->size(&idx_info_u, &idx_size), 0);
      EXPECT_LT(sc_u->ops->idx_delete(&idx_info_u), 0);

      H5O_layout_t tmp_dst = dset_u->shared->layout;
      tmp_dst.storage.u.chunk.idx_addr = HADDR_UNDEF;
      tmp_dst.storage.u.chunk.u.farray.fa = nullptr;
      H5D_chk_idx_info_t dst_info{idx_info_u.f, idx_info_u.pline, &tmp_dst};
      EXPECT_LT(sc_u->ops->copy_setup(&idx_info_u, &dst_info), 0);
    }

    // Now allocate a valid chunk at {2, 2} and insert while !IS_OPEN.
    hsize_t scaled_11[H5O_LAYOUT_NDIMS] = {1, 1, 0};
    H5D_chunk_ud_t udata{};
    udata.common.layout = &dset_u->shared->layout.u.chunk;
    udata.common.storage = sc_u;
    udata.common.scaled = scaled_11;
    udata.chunk_block.offset = HADDR_UNDEF;
    udata.chunk_block.length = dset_u->shared->layout.u.chunk.size;
    udata.chunk_idx = 3;
    udata.idx_hint = UINT_MAX;
    bool need_insert = false;
    ASSERT_GE(H5D__chunk_file_alloc(&idx_info_u, nullptr, &udata.chunk_block,
                                    &need_insert, scaled_11),
              0);
    std::vector<int32_t> chunk_bytes(4, 77);
    ASSERT_GE(H5F_block_write(idx_info_u.f, H5FD_MEM_DRAW,
                              udata.chunk_block.offset, chunk_bytes.size() * 4,
                              chunk_bytes.data()),
              0);
    ASSERT_GE(sc_u->ops->insert(&idx_info_u, &udata, dset_u), 0);

    ASSERT_GE(sc_u->ops->is_open(&idx_info_u, &is_open), 0);
    EXPECT_TRUE(is_open);

    // insert() validation errors while open: HADDR_UNDEF offset and >= 2^32
    // chunk_idx.
    {
      ScopedErrorSilence silence;
      H5D_chunk_ud_t invalid_ud = udata;
      invalid_ud.chunk_block.offset = HADDR_UNDEF;
      EXPECT_LT(sc_u->ops->insert(&idx_info_u, &invalid_ud, dset_u), 0);

      invalid_ud.chunk_block.offset = udata.chunk_block.offset;
      invalid_ud.chunk_idx = uint64_t{1} << 32;
      EXPECT_LT(sc_u->ops->insert(&idx_info_u, &invalid_ud, dset_u), 0);
    }

    // Re-insert while already open -> H5FA_patch_file branch.
    ASSERT_GE(sc_u->ops->insert(&idx_info_u, &udata, dset_u), 0);

    // load_metadata performs fake lookup.
    ASSERT_GE(sc_u->ops->load_metadata(&idx_info_u), 0);

    // Dump index info and iterate dump via H5D__chunk_dump_index.
    std::array<char, 1024> dump_buf{};
    FILE* dump_stream = fmemopen(dump_buf.data(), dump_buf.size() - 1, "w");
    ASSERT_NE(dump_stream, nullptr);
    absl::Cleanup close_dump = [dump_stream]() { std::fclose(dump_stream); };
    ASSERT_GE(H5D__chunk_dump_index(dset_u, dump_stream), 0);
    std::fflush(dump_stream);
    std::string_view dumped(dump_buf.data());
    EXPECT_THAT(dumped, HasSubstr("Address:"));
    EXPECT_THAT(dumped, HasSubstr("Logical Offset"));

    // Invoke H5FA_CLS_CHUNK debug callbacks directly (no caller in HDF5 2.1.0).
    void* dbg_ctx =
        H5FA_CLS_CHUNK[0].crt_dbg_ctx(idx_info_u.f, dset_u->oloc.addr);
    ASSERT_NE(dbg_ctx, nullptr);
    std::array<char, 512> dbg_buf{};
    FILE* dbg_stream = fmemopen(dbg_buf.data(), dbg_buf.size() - 1, "w");
    ASSERT_NE(dbg_stream, nullptr);
    absl::Cleanup close_dbg = [dbg_stream]() { std::fclose(dbg_stream); };
    const haddr_t sample_addr = udata.chunk_block.offset;
    ASSERT_GE(H5FA_CLS_CHUNK[0].debug(dbg_stream, 2, 20, 0, &sample_addr), 0);
    std::fflush(dbg_stream);
    EXPECT_THAT(std::string_view(dbg_buf.data()), HasSubstr("Element #0:"));
    ASSERT_GE(H5FA_CLS_CHUNK[0].dst_dbg_ctx(dbg_ctx), 0);

    // Test H5D_chunk_idx_reset(..., false).
    H5O_storage_chunk_t temp_sc = *sc_u;
    ASSERT_GE(H5D_chunk_idx_reset(&temp_sc, false), 0);
    EXPECT_NE(temp_sc.idx_addr, HADDR_UNDEF);
    EXPECT_EQ(temp_sc.u.farray.fa, nullptr);

    // Test copy_setup while src is ALREADY open + copy_shutdown.
    H5O_layout_t dst_layout = dset_u->shared->layout;
    dst_layout.storage.u.chunk.idx_addr = HADDR_UNDEF;
    dst_layout.storage.u.chunk.u.farray.fa = nullptr;
    H5D_chk_idx_info_t dst_info{idx_info_u.f, idx_info_u.pline, &dst_layout};
    ASSERT_GE(sc_u->ops->copy_setup(&idx_info_u, &dst_info), 0);
    EXPECT_NE(dst_layout.storage.u.chunk.idx_addr, HADDR_UNDEF);
    ASSERT_GE(sc_u->ops->copy_shutdown(sc_u, &dst_layout.storage.u.chunk), 0);
    // Clean up destination fixed array from file.
    ASSERT_GE(dst_layout.storage.u.chunk.ops->idx_delete(&dst_info), 0);
    // Re-open closed index or leave fa == NULL for dest().
    ASSERT_GE(sc_u->ops->is_open(&idx_info_u, &is_open), 0);
    EXPECT_FALSE(is_open);
  }

  // 2. Filtered internal ops: debug callbacks, crt_dbg_ctx error cleanup,
  // filtered insert (!IS_OPEN and IS_OPEN), and v4 chunk_size_len > 8 clamp.
  {
    ScopedApiContext ctx(dset_f->oloc.addr);
    ASSERT_TRUE(ctx.valid());

    H5O_storage_chunk_t* sc_f = &dset_f->shared->layout.storage.u.chunk;
    H5D_chk_idx_info_t idx_info_f{};
    idx_info_f.f = dset_f->oloc.file;
    idx_info_f.pline = &dset_f->shared->dcpl_cache.pline;
    idx_info_f.layout = &dset_f->shared->layout;

    // Invoke H5FA_CLS_FILT_CHUNK debug callbacks directly.
    void* filt_dbg_ctx =
        H5FA_CLS_FILT_CHUNK[0].crt_dbg_ctx(idx_info_f.f, dset_f->oloc.addr);
    ASSERT_NE(filt_dbg_ctx, nullptr);

    struct FiltElmt {
      haddr_t addr;
      hsize_t nbytes;
      uint32_t filter_mask;
    };
    ASSERT_EQ(sizeof(FiltElmt), H5FA_CLS_FILT_CHUNK[0].nat_elmt_size);
    const FiltElmt sample_filt{.addr = 4096, .nbytes = 42, .filter_mask = 0x1u};
    std::array<char, 512> fdbg_buf{};
    FILE* fdbg_stream = fmemopen(fdbg_buf.data(), fdbg_buf.size() - 1, "w");
    ASSERT_NE(fdbg_stream, nullptr);
    absl::Cleanup close_fdbg = [fdbg_stream]() { std::fclose(fdbg_stream); };
    ASSERT_GE(H5FA_CLS_FILT_CHUNK[0].debug(fdbg_stream, 2, 20, 1, &sample_filt),
              0);
    std::fflush(fdbg_stream);
    std::string_view fdbg_text(fdbg_buf.data());
    EXPECT_THAT(fdbg_text, HasSubstr("Element #1:"));
    EXPECT_THAT(fdbg_text, HasSubstr("4096"));
    ASSERT_GE(H5FA_CLS_FILT_CHUNK[0].dst_dbg_ctx(filt_dbg_ctx), 0);

    // Filtered crt_dbg_ctx error paths:
    // (a) Non-OHDR address fails H5O_open.
    // (b) Group OHDR address opens successfully then fails H5O_msg_read(LAYOUT)
    //     and hits cleanup H5O_close.
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(
          H5FA_CLS_FILT_CHUNK[0].crt_dbg_ctx(idx_info_f.f, sc_f->idx_addr),
          nullptr);
      H5O_loc_t* grp_oloc = H5G_oloc(grp_ptr);
      ASSERT_NE(grp_oloc, nullptr);
      EXPECT_EQ(
          H5FA_CLS_FILT_CHUNK[0].crt_dbg_ctx(idx_info_f.f, grp_oloc->addr),
          nullptr);
    }

    // Filtered insert while !IS_OPEN then while IS_OPEN.
    hsize_t scaled_11[H5O_LAYOUT_NDIMS] = {1, 1, 0};
    H5D_chunk_ud_t fud{};
    fud.common.layout = &dset_f->shared->layout.u.chunk;
    fud.common.storage = sc_f;
    fud.common.scaled = scaled_11;
    fud.chunk_block.offset = HADDR_UNDEF;
    fud.chunk_block.length = 16;
    fud.filter_mask = 0;
    fud.chunk_idx = 3;
    fud.idx_hint = UINT_MAX;
    bool need_insert = false;
    ASSERT_GE(H5D__chunk_file_alloc(&idx_info_f, nullptr, &fud.chunk_block,
                                    &need_insert, scaled_11),
              0);
    ASSERT_GE(sc_f->ops->insert(&idx_info_f, &fud, dset_f), 0);
    // Re-insert while already open to exercise H5FA_patch_file branch.
    ASSERT_GE(sc_f->ops->insert(&idx_info_f, &fud, dset_f), 0);

    // Synthetic layout with v4 and chunk.size >= 2^56 exercises
    // chunk_size_len > 8 clamp in H5D_FARRAY_FILT_COMPUTE_CHUNK_SIZE_LEN.
    H5O_layout_t synth_layout = dset_f->shared->layout;
    synth_layout.version = H5O_LAYOUT_VERSION_4;
    synth_layout.u.chunk.size = uint64_t{1} << 56;
    synth_layout.storage.u.chunk.idx_addr = HADDR_UNDEF;
    synth_layout.storage.u.chunk.u.farray.fa = nullptr;

    H5D_chk_idx_info_t synth_info{dset_f->oloc.file,
                                  &dset_f->shared->dcpl_cache.pline,
                                  &synth_layout};
    ASSERT_GE(H5D_COPS_FARRAY[0].create(&synth_info), 0);
    EXPECT_NE(synth_layout.storage.u.chunk.idx_addr, HADDR_UNDEF);
    ASSERT_GE(H5D_COPS_FARRAY[0].dest(&synth_info), 0);
    ASSERT_GE(H5D_COPS_FARRAY[0].idx_delete(&synth_info), 0);
  }
}

}  // namespace
}  // namespace hdf5
