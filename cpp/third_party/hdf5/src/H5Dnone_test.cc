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
// Tests implicit (non-indexed) chunked dataset storage, lifecycle, iteration,
// copy setup, deletion, dumping, and error paths (H5Dnone.c).
//
// Google3 build caveats:
//   - Compiles `:hdf5` with `-DNDEBUG` (internal structs omit debug fields).
//   - Parallel MPI (H5_HAVE_PARALLEL) is disabled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dnone.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <span>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields in internal
// metadata structures. Define NDEBUG around both the prelude and internal
// headers so internal struct layouts match the compiled library ABI.
#ifndef NDEBUG
#define H5DNONE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#define H5D_FRIEND
#define H5F_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DNONE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DNONE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

// Pushes an HDF5 API context and metadata tag for direct internal calls.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dxpl_id = H5P_DEFAULT,
                            haddr_t tag = 0x10000) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (dxpl_id != H5P_DEFAULT) {
        H5CX_set_dxpl(dxpl_id);
      }
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

H5D_t* UnwrapDataset(hid_t dset_id) {
  return static_cast<H5D_t*>(H5VL_object_verify(dset_id, H5I_DATASET));
}

ScopedPlist MakeLatestFapl(size_t increment = 4096) {
  ScopedPlist fapl = MakeCoreFapl(increment);
  if (fapl.valid()) {
    if (H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST) <
        0) {
      fapl.reset();
    }
  }
  return fapl;
}

ScopedPlist MakeNoneIdxDcpl(std::span<const hsize_t> chunk_dims) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  if (dcpl.valid()) {
    if (H5Pset_chunk(dcpl.get(), static_cast<int>(chunk_dims.size()),
                     chunk_dims.data()) < 0 ||
        H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_EARLY) < 0) {
      dcpl.reset();
    }
  }
  return dcpl;
}

TEST(H5DnoneTest, EarlyAllocExtendShrinkReopenAndInfo) {
  ScopedPlist fapl = MakeLatestFapl();
  ASSERT_TRUE(fapl.valid());

  std::vector<uint8_t> image;
  {
    ScopedFile file(
        H5Fcreate("none_idx_lifecycle.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());

    // 1. Fixed finite max_dims > cur_dims, > 1 max chunks, no filters, and
    //    H5D_ALLOC_TIME_EARLY selects H5D_COPS_NONE (H5D_CHUNK_IDX_NONE).
    constexpr std::array<hsize_t, 2> kInitDims = {2, 2};
    constexpr std::array<hsize_t, 2> kMaxDims = {4, 6};
    constexpr std::array<hsize_t, 2> kChunkDims = {2, 2};
    ScopedSpace space(
        H5Screate_simple(2, kInitDims.data(), kMaxDims.data()));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl = MakeNoneIdxDcpl(kChunkDims);
    ASSERT_TRUE(dcpl.valid());

    ScopedDataset dset(H5Dcreate2(file, "none_ds", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    const H5D_t* ds_ptr = UnwrapDataset(dset);
    ASSERT_NE(ds_ptr, nullptr);
    EXPECT_EQ(ds_ptr->shared->layout.storage.u.chunk.idx_type,
              H5D_CHUNK_IDX_NONE);
    EXPECT_EQ(ds_ptr->shared->layout.storage.u.chunk.ops, H5D_COPS_NONE);

    // 2. Verify space is already allocated early and chunk query succeeds.
    H5D_space_status_t status = H5D_SPACE_STATUS_NOT_ALLOCATED;
    ASSERT_GE(H5Dget_space_status(dset, &status), 0);
    EXPECT_EQ(status, H5D_SPACE_STATUS_ALLOCATED);

    constexpr std::array<hsize_t, 2> kCoord0 = {0, 0};
    unsigned filter_mask = 99;
    haddr_t chunk_addr = HADDR_UNDEF;
    hsize_t chunk_bytes = 0;
    ASSERT_GE(H5Dget_chunk_info_by_coord(dset, kCoord0.data(), &filter_mask,
                                         &chunk_addr, &chunk_bytes),
              0);
    EXPECT_EQ(filter_mask, 0u);
    EXPECT_NE(chunk_addr, HADDR_UNDEF);
    EXPECT_EQ(chunk_bytes, 16u);

    // 3. Extend to full max_dims (6 chunks), write and read back, then shrink
    //    back to {2, 2} to trigger H5D__none_idx_remove on pruned chunks.
    constexpr std::array<int32_t, 4> kInitData = {1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kInitData.data()),
              0);
    ASSERT_GE(H5Dset_extent(dset, kMaxDims.data()), 0);

    std::array<int32_t, 24> full_write{};
    std::iota(full_write.begin(), full_write.end(), 100);
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       full_write.data()),
              0);

    constexpr std::array<hsize_t, 2> kTailCoord = {2, 4};
    haddr_t tail_addr = HADDR_UNDEF;
    hsize_t tail_bytes = 0;
    ASSERT_GE(H5Dget_chunk_info_by_coord(dset, kTailCoord.data(), &filter_mask,
                                         &tail_addr, &tail_bytes),
              0);
    EXPECT_NE(tail_addr, HADDR_UNDEF);
    EXPECT_GT(tail_addr, chunk_addr);
    EXPECT_EQ(tail_bytes, 16u);

    H5O_native_info_t native_info{};
    ASSERT_GE(H5Oget_native_info(dset, &native_info, H5O_NATIVE_INFO_META_SIZE),
              0);
    EXPECT_EQ(native_info.meta_size.obj.index_size, 0u);

    std::array<int32_t, 24> pre_shrink_read{};
    ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      pre_shrink_read.data()),
              0);
    EXPECT_EQ(pre_shrink_read[0], 100);
    EXPECT_EQ(pre_shrink_read[23], 123);

    ASSERT_GE(H5Dset_extent(dset, kInitDims.data()), 0);
    ASSERT_GE(H5Dset_extent(dset, kMaxDims.data()), 0);
    ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);

    const ssize_t img_len = H5Fget_file_image(file, nullptr, 0);
    ASSERT_GT(img_len, 0);
    image.resize(static_cast<size_t>(img_len));
    ASSERT_EQ(H5Fget_file_image(file, image.data(), image.size()), img_len);
  }

  // 4. Reopen persisted image to exercise open/load_metadata/get_addr/close.
  //    Dataset was created as 4x6 (row stride 6) with 2x2 chunks. Chunk {0, 0}
  //    occupies rows {0, 1} x cols {0, 1} = linear indices {0, 1, 6, 7}, which
  //    retained {100, 101, 106, 107}, while pruned chunks removed during shrink
  //    revert to fill value 0 after re-extension.
  ScopedPlist reopen_fapl = MakeLatestFapl();
  ASSERT_TRUE(reopen_fapl.valid());
  ASSERT_GE(H5Pset_file_image(reopen_fapl, image.data(), image.size()), 0);
  ScopedFile reopened(
      H5Fopen("none_idx_reopen.h5", H5F_ACC_RDONLY, reopen_fapl));
  ASSERT_TRUE(reopened.valid());

  ScopedDataset dset(H5Dopen2(reopened, "none_ds", H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  std::array<int32_t, 24> read_back{};
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_back.data()),
            0);
  EXPECT_EQ(read_back[0], 100);
  EXPECT_EQ(read_back[1], 101);
  EXPECT_EQ(read_back[4], 0);
  EXPECT_EQ(read_back[5], 0);
  EXPECT_EQ(read_back[6], 106);
  EXPECT_EQ(read_back[7], 107);
  EXPECT_EQ(read_back[23], 0);
}

TEST(H5DnoneTest, ChunkIterateMultiDimCarryStopAndCallbackError) {
  ScopedPlist fapl = MakeLatestFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(
      H5Fcreate("none_idx_iter.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  // Use a 3D dataset with 2x3x2 chunks so H5D__none_idx_iterate exercises
  // multi-dimensional coordinate carry rollovers across slow/fast dims.
  constexpr std::array<hsize_t, 3> kDims = {4, 6, 4};
  constexpr std::array<hsize_t, 3> kChunks = {2, 2, 2};
  ScopedSpace space(H5Screate_simple(3, kDims.data(), kDims.data()));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl = MakeNoneIdxDcpl(kChunks);
  ASSERT_TRUE(dcpl.valid());

  ScopedDataset dset(H5Dcreate2(file, "grid3d", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  struct IterState {
    int count = 0;
    int stop_after = -1;
    bool return_error = false;
    hsize_t last_size = 0;
  };

  auto cb = [](const hsize_t* /*offset*/, unsigned filter_mask, haddr_t addr,
               hsize_t size, void* op_data) -> int {
    auto* st = static_cast<IterState*>(op_data);
    if (st->return_error && st->count == 1) {
      return H5_ITER_ERROR;
    }
    EXPECT_EQ(filter_mask, 0u);
    EXPECT_NE(addr, HADDR_UNDEF);
    st->last_size = size;
    ++st->count;
    if (st->stop_after > 0 && st->count >= st->stop_after) {
      return H5_ITER_STOP;
    }
    return H5_ITER_CONT;
  };

  // 1. Full iteration across all 12 chunks.
  IterState full_state;
  ASSERT_GE(H5Dchunk_iter(dset, H5P_DEFAULT, cb, &full_state), 0);
  EXPECT_EQ(full_state.count, 12);
  EXPECT_EQ(full_state.last_size, 32u);

  // 2. Early stop after 3 chunks (H5VL_dataset_optional normalizes positive
  //    H5_ITER_STOP from H5D__none_idx_iterate to SUCCEED >= 0).
  IterState stop_state;
  stop_state.stop_after = 3;
  EXPECT_GE(H5Dchunk_iter(dset, H5P_DEFAULT, cb, &stop_state), 0);
  EXPECT_EQ(stop_state.count, 3);

  // 3. Callback error triggers H5D__none_idx_iterate failure path.
  IterState err_state;
  err_state.return_error = true;
  ScopedErrorSilence silence;
  EXPECT_LT(H5Dchunk_iter(dset, H5P_DEFAULT, cb, &err_state), 0);
}

TEST(H5DnoneTest, ObjectCopyAndLinkDeleteExerciseCopySetupAndDelete) {
  ScopedPlist fapl = MakeLatestFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFile src_file(
      H5Fcreate("none_idx_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(src_file.valid());
  ScopedFile dst_file(
      H5Fcreate("none_idx_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(dst_file.valid());

  constexpr std::array<hsize_t, 1> kDims = {8};
  constexpr std::array<hsize_t, 1> kChunk = {2};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), kDims.data()));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl = MakeNoneIdxDcpl(kChunk);
  ASSERT_TRUE(dcpl.valid());

  std::array<int32_t, 8> wdata = {10, 20, 30, 40, 50, 60, 70, 80};
  {
    ScopedDataset ds(H5Dcreate2(src_file, "copy_me", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    ASSERT_GE(H5Dwrite(ds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       wdata.data()),
              0);
  }

  ASSERT_GE(H5Ocopy(src_file, "copy_me", dst_file, "copied", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);

  ScopedDataset copied(H5Dopen2(dst_file, "copied", H5P_DEFAULT));
  ASSERT_TRUE(copied.valid());
  std::array<int32_t, 8> rdata{};
  ASSERT_GE(H5Dread(copied, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    rdata.data()),
            0);
  EXPECT_THAT(rdata, ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));

  // Delete source dataset to exercise H5D__none_idx_delete freeing chunk space.
  ASSERT_GE(H5Ldelete(src_file, "copy_me", H5P_DEFAULT), 0);
}

TEST(H5DnoneTest, InternalOpsDumpResetAndAllocationFreeErrorPaths) {
  ScopedPlist fapl = MakeLatestFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(
      H5Fcreate("none_idx_internal.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {8};
  constexpr std::array<hsize_t, 1> kChunk = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), kDims.data()));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl = MakeNoneIdxDcpl(kChunk);
  ASSERT_TRUE(dcpl.valid());

  ScopedDataset dset(H5Dcreate2(file, "internal_ds", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  H5D_t* ds = UnwrapDataset(dset);
  ASSERT_NE(ds, nullptr);

  ScopedApiContext ctx(H5P_DEFAULT, ds->oloc.addr);
  ASSERT_TRUE(ctx.valid());

  H5D_chk_idx_info_t idx_info{};
  idx_info.f = ds->oloc.file;
  idx_info.pline = &ds->shared->dcpl_cache.pline;
  idx_info.layout = &ds->shared->layout;

  // 1. Direct query/reset/size/load_metadata/remove/open/close/is_open checks.
  bool is_open = false;
  ASSERT_GE(H5D_COPS_NONE->is_open(&idx_info, &is_open), 0);
  EXPECT_TRUE(is_open);

  EXPECT_TRUE(
      H5D_COPS_NONE->is_space_alloc(&ds->shared->layout.storage.u.chunk));
  H5O_storage_chunk_t tmp_storage = ds->shared->layout.storage.u.chunk;
  ASSERT_GE(H5D_COPS_NONE->reset(&tmp_storage, /*reset_addr=*/false), 0);
  EXPECT_TRUE(H5D_COPS_NONE->is_space_alloc(&tmp_storage));
  ASSERT_GE(H5D_COPS_NONE->reset(&tmp_storage, /*reset_addr=*/true), 0);
  EXPECT_FALSE(H5D_COPS_NONE->is_space_alloc(&tmp_storage));

  hsize_t idx_size = 999;
  ASSERT_GE(H5D_COPS_NONE->size(&idx_info, &idx_size), 0);
  EXPECT_EQ(idx_size, 0u);
  EXPECT_GE(H5D_COPS_NONE->load_metadata(&idx_info), 0);
  EXPECT_GE(H5D_COPS_NONE->open(&idx_info), 0);
  EXPECT_GE(H5D_COPS_NONE->close(&idx_info), 0);
  EXPECT_GE(H5D_COPS_NONE->remove(&idx_info, nullptr), 0);

  // 2. Dump chunk index to an in-memory FILE* via POSIX open_memstream().
  char* dump_buf = nullptr;
  size_t dump_len = 0;
  FILE* memstream = open_memstream(&dump_buf, &dump_len);
  ASSERT_NE(memstream, nullptr);
  absl::Cleanup close_memstream = [&] {
    if (memstream != nullptr) {
      std::fclose(memstream);
    }
    std::free(dump_buf);
  };
  ASSERT_GE(H5D__chunk_dump_index(ds, memstream), 0);
  ASSERT_GE(
      H5D_COPS_NONE->dump(&ds->shared->layout.storage.u.chunk, memstream), 0);
  std::fclose(memstream);
  memstream = nullptr;
  EXPECT_THAT(std::string(dump_buf, dump_len), HasSubstr("Address:"));

  // 3. Error paths: H5MF_alloc failure in create and copy_setup, plus
  //    H5MF_xfree failure in idx_delete when given a temporary address.
  ScopedErrorSilence silence;
  const haddr_t orig_lf_maxaddr = ds->oloc.file->shared->lf->maxaddr;
  const haddr_t orig_sh_maxaddr = ds->oloc.file->shared->maxaddr;
  absl::Cleanup restore_maxaddr = [&] {
    ds->oloc.file->shared->lf->maxaddr = orig_lf_maxaddr;
    ds->oloc.file->shared->maxaddr = orig_sh_maxaddr;
  };
  ds->oloc.file->shared->lf->maxaddr = 4096;
  ds->oloc.file->shared->maxaddr = 4096;

  H5O_layout_t bad_layout = ds->shared->layout;
  bad_layout.storage.u.chunk.idx_addr = HADDR_UNDEF;
  bad_layout.u.chunk.max_nchunks = 1;
  bad_layout.u.chunk.size = 65536;
  H5D_chk_idx_info_t bad_idx = idx_info;
  bad_idx.layout = &bad_layout;
  EXPECT_LT(H5D_COPS_NONE->create(&bad_idx), 0);
  EXPECT_LT(H5D_COPS_NONE->copy_setup(&idx_info, &bad_idx), 0);
  std::move(restore_maxaddr).Invoke();

  bad_layout.u.chunk.max_nchunks = 1;
  bad_layout.u.chunk.size = 16;
  bad_layout.storage.u.chunk.idx_addr = ds->oloc.file->shared->tmp_addr;
  EXPECT_LT(H5D_COPS_NONE->idx_delete(&bad_idx), 0);
}

}  // namespace
}  // namespace hdf5
