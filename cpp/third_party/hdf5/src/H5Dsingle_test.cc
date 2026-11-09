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
// Tests single-chunk dataset storage, filtered and unfiltered modes, early vs
// late allocation, chunk iteration, copy, delete, dumping, and error paths
// (H5Dsingle.c).
//
// Google3 build caveats:
//   - Deflate (zlib) filter is enabled in `//third_party/hdf5`.
//   - Compiles `:hdf5` with `-DNDEBUG` (internal structs omit debug fields).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dsingle.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#define H5DSINGLE_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5DSINGLE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DSINGLE_TEST_RESTORE_UNDEF_NDEBUG
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

ScopedPlist MakeSingleChunkDcpl(std::span<const hsize_t> chunk_dims,
                                H5D_alloc_time_t alloc_time, bool deflate) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  if (dcpl.valid()) {
    if (H5Pset_chunk(dcpl.get(), static_cast<int>(chunk_dims.size()),
                     chunk_dims.data()) < 0 ||
        H5Pset_alloc_time(dcpl.get(), alloc_time) < 0 ||
        (deflate && H5Pset_deflate(dcpl.get(), 6) < 0)) {
      dcpl.reset();
    }
  }
  return dcpl;
}

TEST(H5DsingleTest, UnfilteredEarlyAndLateAllocLifecycleCopyAndDelete) {
  ScopedPlist fapl = MakeLatestFapl();
  ASSERT_TRUE(fapl.valid());

  std::vector<uint8_t> image;
  {
    ScopedFile src(
        H5Fcreate("single_unfilt.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(src.valid());
    ScopedFile dst(
        H5Fcreate("single_unfilt_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(dst.valid());

    constexpr std::array<hsize_t, 2> kDims = {2, 4};
    ScopedSpace space(H5Screate_simple(2, kDims.data(), kDims.data()));
    ASSERT_TRUE(space.valid());

    // 1. Late-allocated unfiltered single chunk starts unallocated
    //    (get_addr returns length 0), then allocates on write (H5D__mark).
    ScopedPlist dcpl_late =
        MakeSingleChunkDcpl(kDims, H5D_ALLOC_TIME_LATE, /*deflate=*/false);
    ASSERT_TRUE(dcpl_late.valid());
    ScopedDataset late_ds(H5Dcreate2(src, "late_unfilt", H5T_NATIVE_INT32,
                                     space, H5P_DEFAULT, dcpl_late,
                                     H5P_DEFAULT));
    ASSERT_TRUE(late_ds.valid());

    const H5D_t* ds_ptr = UnwrapDataset(late_ds);
    ASSERT_NE(ds_ptr, nullptr);
    EXPECT_EQ(ds_ptr->shared->layout.storage.u.chunk.idx_type,
              H5D_CHUNK_IDX_SINGLE);
    EXPECT_EQ(ds_ptr->shared->layout.storage.u.chunk.ops, H5D_COPS_SINGLE);

    constexpr std::array<hsize_t, 2> kCoord = {0, 0};
    unsigned filter_mask = 99;
    haddr_t addr = HADDR_UNDEF;
    hsize_t size = 99;
    ASSERT_GE(H5Dget_chunk_info_by_coord(late_ds, kCoord.data(), &filter_mask,
                                         &addr, &size),
              0);
    EXPECT_EQ(addr, HADDR_UNDEF);
    EXPECT_EQ(size, 0u);

    std::array<int32_t, 8> wdata = {10, 20, 30, 40, 50, 60, 70, 80};
    ASSERT_GE(H5Dwrite(late_ds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       wdata.data()),
              0);

    ASSERT_GE(H5Dget_chunk_info_by_coord(late_ds, kCoord.data(), &filter_mask,
                                         &addr, &size),
              0);
    EXPECT_NE(addr, HADDR_UNDEF);
    EXPECT_EQ(size, 32u);
    EXPECT_EQ(filter_mask, 0u);

    // 2. Early-allocated unfiltered single chunk skips H5D__mark in insert.
    ScopedPlist dcpl_early =
        MakeSingleChunkDcpl(kDims, H5D_ALLOC_TIME_EARLY, /*deflate=*/false);
    ASSERT_TRUE(dcpl_early.valid());
    {
      ScopedDataset early_ds(H5Dcreate2(src, "early_unfilt", H5T_NATIVE_INT32,
                                        space, H5P_DEFAULT, dcpl_early,
                                        H5P_DEFAULT));
      ASSERT_TRUE(early_ds.valid());
      H5D_space_status_t status = H5D_SPACE_STATUS_NOT_ALLOCATED;
      ASSERT_GE(H5Dget_space_status(early_ds, &status), 0);
      EXPECT_EQ(status, H5D_SPACE_STATUS_ALLOCATED);
      ASSERT_GE(H5Dwrite(early_ds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                         H5P_DEFAULT, wdata.data()),
                0);
    }

    // 3. Delete an unallocated late single-chunk dataset to exercise the
    //    `!H5_addr_defined(idx_addr)` branch in H5D__single_idx_delete.
    {
      ScopedDataset unalloc_ds(H5Dcreate2(src, "unalloc_del", H5T_NATIVE_INT32,
                                          space, H5P_DEFAULT, dcpl_late,
                                          H5P_DEFAULT));
      ASSERT_TRUE(unalloc_ds.valid());
    }
    ASSERT_GE(H5Ldelete(src, "unalloc_del", H5P_DEFAULT), 0);

    // 4. H5Ocopy and H5Dchunk_iter on unfiltered single-chunk dataset.
    late_ds.reset();
    ASSERT_GE(H5Ocopy(src, "late_unfilt", dst, "copied_unfilt", H5P_DEFAULT,
                      H5P_DEFAULT),
              0);
    ScopedDataset copied(H5Dopen2(dst, "copied_unfilt", H5P_DEFAULT));
    ASSERT_TRUE(copied.valid());
    std::array<int32_t, 8> rdata{};
    ASSERT_GE(H5Dread(copied, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      rdata.data()),
              0);
    EXPECT_THAT(rdata, ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));

    int iter_calls = 0;
    auto iter_cb = [](const hsize_t* /*offset*/, unsigned /*fmask*/,
                      haddr_t /*caddr*/, hsize_t csize, void* ctx) -> int {
      EXPECT_EQ(csize, 32u);
      ++(*static_cast<int*>(ctx));
      return H5_ITER_CONT;
    };
    ASSERT_GE(H5Dchunk_iter(copied, H5P_DEFAULT, iter_cb, &iter_calls), 0);
    EXPECT_EQ(iter_calls, 1);

    H5O_native_info_t native_info{};
    ASSERT_GE(
        H5Oget_native_info(copied, &native_info, H5O_NATIVE_INFO_META_SIZE), 0);
    EXPECT_EQ(native_info.meta_size.obj.index_size, 0u);

    ASSERT_GE(H5Ldelete(src, "early_unfilt", H5P_DEFAULT), 0);
    ASSERT_GE(H5Fflush(src, H5F_SCOPE_GLOBAL), 0);
    const ssize_t img_len = H5Fget_file_image(src, nullptr, 0);
    ASSERT_GT(img_len, 0);
    image.resize(static_cast<size_t>(img_len));
    ASSERT_EQ(H5Fget_file_image(src, image.data(), image.size()), img_len);
  }

  // 5. Reopen persisted image to hit single_idx_init with non-filter storage.
  ScopedPlist reopen_fapl = MakeLatestFapl();
  ASSERT_TRUE(reopen_fapl.valid());
  ASSERT_GE(H5Pset_file_image(reopen_fapl, image.data(), image.size()), 0);
  ScopedFile reopened(
      H5Fopen("single_unfilt_ro.h5", H5F_ACC_RDONLY, reopen_fapl));
  ASSERT_TRUE(reopened.valid());
  ScopedDataset ds(H5Dopen2(reopened, "late_unfilt", H5P_DEFAULT));
  ASSERT_TRUE(ds.valid());
}

TEST(H5DsingleTest, FilteredSingleChunkWriteIterateCopyReopenAndDelete) {
  ScopedPlist fapl = MakeLatestFapl();
  ASSERT_TRUE(fapl.valid());

  std::vector<uint8_t> image;
  {
    ScopedFile src(
        H5Fcreate("single_filt.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(src.valid());
    ScopedFile dst(
        H5Fcreate("single_filt_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(dst.valid());

    constexpr std::array<hsize_t, 1> kDims = {64};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), kDims.data()));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl =
        MakeSingleChunkDcpl(kDims, H5D_ALLOC_TIME_LATE, /*deflate=*/true);
    ASSERT_TRUE(dcpl.valid());

    ScopedDataset dset(H5Dcreate2(src, "filt_single", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    std::array<int32_t, 64> wdata{};
    std::iota(wdata.begin(), wdata.end(), 1);
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       wdata.data()),
              0);

    // Flush chunk to trigger H5D__single_idx_insert storing filtered nbytes.
    ASSERT_GE(H5Fflush(src, H5F_SCOPE_GLOBAL), 0);

    constexpr hsize_t kCoord = 0;
    unsigned filter_mask = 99;
    haddr_t addr = HADDR_UNDEF;
    hsize_t stored_bytes = 0;
    ASSERT_GE(H5Dget_chunk_info_by_coord(dset, &kCoord, &filter_mask, &addr,
                                         &stored_bytes),
              0);
    EXPECT_NE(addr, HADDR_UNDEF);
    EXPECT_LT(stored_bytes, sizeof(wdata));

    int iter_calls = 0;
    auto iter_cb = [](const hsize_t* /*offset*/, unsigned /*fmask*/,
                      haddr_t /*caddr*/, hsize_t csize, void* ctx) -> int {
      EXPECT_GT(csize, 0u);
      ++(*static_cast<int*>(ctx));
      return H5_ITER_CONT;
    };
    ASSERT_GE(H5Dchunk_iter(dset, H5P_DEFAULT, iter_cb, &iter_calls), 0);
    EXPECT_EQ(iter_calls, 1);

    // Callback error in filtered H5D__single_idx_iterate.
    auto err_cb = [](const hsize_t* /*offset*/, unsigned /*fmask*/,
                     haddr_t /*caddr*/, hsize_t /*csize*/, void*) -> int {
      return H5_ITER_ERROR;
    };
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Dchunk_iter(dset, H5P_DEFAULT, err_cb, nullptr), 0);
    }

    dset.reset();
    ASSERT_GE(H5Ocopy(src, "filt_single", dst, "copied_filt", H5P_DEFAULT,
                      H5P_DEFAULT),
              0);
    ScopedDataset copied(H5Dopen2(dst, "copied_filt", H5P_DEFAULT));
    ASSERT_TRUE(copied.valid());
    std::array<int32_t, 64> rdata{};
    ASSERT_GE(H5Dread(copied, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      rdata.data()),
              0);
    EXPECT_EQ(rdata, wdata);

    // Save image before deleting from src so reopen exercises defined filtered
    // idx_addr branch in H5D__single_idx_init.
    ASSERT_GE(H5Fflush(src, H5F_SCOPE_GLOBAL), 0);
    const ssize_t img_len = H5Fget_file_image(src, nullptr, 0);
    ASSERT_GT(img_len, 0);
    image.resize(static_cast<size_t>(img_len));
    ASSERT_EQ(H5Fget_file_image(src, image.data(), image.size()), img_len);

    ASSERT_GE(H5Ldelete(src, "filt_single", H5P_DEFAULT), 0);
  }

  ScopedPlist reopen_fapl = MakeLatestFapl();
  ASSERT_TRUE(reopen_fapl.valid());
  ASSERT_GE(H5Pset_file_image(reopen_fapl, image.data(), image.size()), 0);
  ScopedFile reopened(
      H5Fopen("single_filt_ro.h5", H5F_ACC_RDONLY, reopen_fapl));
  ASSERT_TRUE(reopened.valid());
  ScopedDataset ds(H5Dopen2(reopened, "filt_single", H5P_DEFAULT));
  ASSERT_TRUE(ds.valid());
}

TEST(H5DsingleTest, InternalOpsDumpResetAndErrorPaths) {
  ScopedPlist fapl = MakeLatestFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(
      H5Fcreate("single_internal.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {8};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), kDims.data()));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl =
      MakeSingleChunkDcpl(kDims, H5D_ALLOC_TIME_LATE, /*deflate=*/false);
  ASSERT_TRUE(dcpl.valid());

  ScopedDataset dset(H5Dcreate2(file, "single_int", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  std::array<int32_t, 8> data = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     data.data()),
            0);

  H5D_t* ds = UnwrapDataset(dset);
  ASSERT_NE(ds, nullptr);
  ScopedApiContext ctx(H5P_DEFAULT, ds->oloc.addr);
  ASSERT_TRUE(ctx.valid());

  H5D_chk_idx_info_t idx_info{};
  idx_info.f = ds->oloc.file;
  idx_info.pline = &ds->shared->dcpl_cache.pline;
  idx_info.layout = &ds->shared->layout;

  // 1. Direct query/reset/size/open/close/load_metadata checks.
  bool is_open = false;
  ASSERT_GE(H5D_COPS_SINGLE->is_open(&idx_info, &is_open), 0);
  EXPECT_TRUE(is_open);

  EXPECT_TRUE(
      H5D_COPS_SINGLE->is_space_alloc(&ds->shared->layout.storage.u.chunk));
  H5O_storage_chunk_t tmp_storage = ds->shared->layout.storage.u.chunk;
  ASSERT_GE(H5D_COPS_SINGLE->reset(&tmp_storage, /*reset_addr=*/false), 0);
  EXPECT_TRUE(H5D_COPS_SINGLE->is_space_alloc(&tmp_storage));
  ASSERT_GE(H5D_COPS_SINGLE->reset(&tmp_storage, /*reset_addr=*/true), 0);
  EXPECT_FALSE(H5D_COPS_SINGLE->is_space_alloc(&tmp_storage));

  hsize_t idx_size = 999;
  ASSERT_GE(H5D_COPS_SINGLE->size(&idx_info, &idx_size), 0);
  EXPECT_EQ(idx_size, 0u);
  EXPECT_GE(H5D_COPS_SINGLE->load_metadata(&idx_info), 0);
  EXPECT_GE(H5D_COPS_SINGLE->open(&idx_info), 0);
  EXPECT_GE(H5D_COPS_SINGLE->close(&idx_info), 0);

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
      H5D_COPS_SINGLE->dump(&ds->shared->layout.storage.u.chunk, memstream), 0);
  std::fclose(memstream);
  memstream = nullptr;
  EXPECT_THAT(std::string(dump_buf, dump_len), HasSubstr("Address:"));

  // 3. Error paths: insert failing H5D__mark (corrupt oloc addr in shallow
  //    copy) and remove failing H5MF_xfree (tmp_addr).
  ScopedErrorSilence silence;
  H5O_layout_t bad_layout = ds->shared->layout;
  bad_layout.storage.u.chunk.idx_addr = ds->oloc.file->shared->tmp_addr;
  H5D_chk_idx_info_t bad_idx = idx_info;
  bad_idx.layout = &bad_layout;
  EXPECT_LT(H5D_COPS_SINGLE->remove(&bad_idx, nullptr), 0);

  H5D_t bad_dset = *ds;
  bad_dset.oloc.addr = ds->oloc.file->shared->tmp_addr;
  H5D_chunk_ud_t udata{};
  udata.chunk_block.offset = ds->shared->layout.storage.u.chunk.idx_addr;
  udata.chunk_block.length = 32;
  bad_layout.storage.u.chunk.idx_addr = HADDR_UNDEF;
  EXPECT_LT(H5D_COPS_SINGLE->insert(&bad_idx, &udata, &bad_dset), 0);
}

}  // namespace
}  // namespace hdf5
