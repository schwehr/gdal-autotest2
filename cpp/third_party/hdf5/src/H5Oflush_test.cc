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
// Tests Object Header flush and refresh routines (H5Oflush, H5Orefresh,
// H5O_flush, H5O_flush_common, H5O_refresh_metadata,
// H5O_refresh_metadata_reopen, flush callbacks, cork preservation, and SWMR
// reopen paths) in third_party/hdf5/src/H5Oflush.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Oflush.c

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is built with -DNDEBUG, omitting debug fields from cache/object
// header structs. Define NDEBUG around internal headers so struct offsets match.
#ifndef NDEBUG
#define H5OFLUSH_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5Fpkg.h's H5F__efc_open uses `try` as a parameter name, which is both a C++
// keyword and macro-defined under -fno-exceptions.
#pragma push_macro("try")
#undef try
#define try h5f_try_param
extern "C" {
#define H5F_FRIEND
#define H5O_FRIEND
#define H5T_FRIEND
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Mprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
// H5private.h omits the H5__Float16 typedef under __cplusplus while
// H5Tpkg.h declares H5__Float16 H5_DLLVARs under H5_HAVE__FLOAT16.
#pragma push_macro("H5_HAVE__FLOAT16")
#undef H5_HAVE__FLOAT16
#include "third_party/hdf5/src/H5Tpkg.h"
#pragma pop_macro("H5_HAVE__FLOAT16")
#include "third_party/hdf5/src/H5VLpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#undef try
#pragma pop_macro("try")
#ifdef H5OFLUSH_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OFLUSH_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

class ScopedApiContext {
 public:
  ScopedApiContext() {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

struct FlushCallbackRecord {
  std::vector<hid_t> flushed_ids;
  herr_t return_status = 0;
};

herr_t RecordFlushCallback(hid_t object_id, void* udata) {
  auto* rec = static_cast<FlushCallbackRecord*>(udata);
  rec->flushed_ids.push_back(object_id);
  return rec->return_status;
}

TEST(H5OflushTest, PublicFlushApisInvokeObjectFlushCallbackAndRejectErrors) {
  FlushCallbackRecord cb_rec;

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);
  ASSERT_GE(H5Pset_object_flush_cb(fapl.get(), RecordFlushCallback, &cb_rec),
            0);

  ScopedFile file(
      H5Fcreate("flush_cb.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  // Create group, committed datatype, contiguous dataset, and chunked dataset.
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedType dtype(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(dtype.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/dtype", dtype.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  const std::array<hsize_t, 1> dims = {8};
  ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedPlist chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(chunk_dcpl.valid());
  const std::array<hsize_t, 1> chunk_dims = {4};
  ASSERT_GE(H5Pset_chunk(chunk_dcpl.get(), 1, chunk_dims.data()), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "/dset", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, chunk_dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  const std::array<int32_t, 8> values = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, values.data()),
            0);

  EXPECT_GE(H5Oflush(grp.get()), 0);
  EXPECT_GE(H5Gflush(grp.get()), 0);
  EXPECT_GE(H5Oflush(dtype.get()), 0);
  EXPECT_GE(H5Tflush(dtype.get()), 0);
  EXPECT_GE(H5Oflush(dset.get()), 0);
  EXPECT_GE(H5Dflush(dset.get()), 0);

  EXPECT_THAT(cb_rec.flushed_ids,
              ElementsAre(grp.get(), grp.get(), dtype.get(), dtype.get(),
                          dset.get(), dset.get()));

  // Flush callback error propagates through H5O_flush_common / H5O_flush.
  ScopedErrorSilence silence;
  cb_rec.return_status = -1;
  EXPECT_LT(H5Dflush(dset.get()), 0);
  EXPECT_LT(H5Oflush(grp.get()), 0);
  EXPECT_LT(H5Oflush(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Oflush(space.get()), 0);
  cb_rec.return_status = 0;
}

TEST(H5OflushTest, PublicRefreshRdwrNoopAndRdonlySingleAndMultipleOpens) {
  // 1. Create an in-memory file with a group, committed datatype, and datasets.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);

  ScopedFile file(
      H5Fcreate("refresh.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedType dtype(H5Tcopy(H5T_NATIVE_INT64));
  ASSERT_TRUE(dtype.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/dtype", dtype.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  const std::array<hsize_t, 1> dims = {4};
  ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ScopedPlist chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(chunk_dcpl.valid());
  ASSERT_GE(H5Pset_chunk(chunk_dcpl.get(), 1, dims.data()), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "/dset", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, chunk_dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  const std::array<int32_t, 4> init_vals = {10, 20, 30, 40};
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, init_vals.data()),
            0);

  // Refresh on RDWR file hits the no-op fast path in H5O_refresh_metadata.
  EXPECT_GE(H5Orefresh(dset.get()), 0);
  EXPECT_GE(H5Grefresh(grp.get()), 0);
  EXPECT_GE(H5Trefresh(dtype.get()), 0);

  dset.reset();
  dtype.reset();
  grp.reset();
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
            image_size);
  file.reset();

  // 2. Reopen read-only from the memory image to exercise full refresh close
  // and reopen paths.
  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(H5Fopen("refresh.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());

  ScopedGroup ro_grp(H5Gopen2(ro_file.get(), "/grp", H5P_DEFAULT));
  ASSERT_TRUE(ro_grp.valid());
  EXPECT_GE(H5Grefresh(ro_grp.get()), 0);
  EXPECT_GE(H5Orefresh(ro_grp.get()), 0);

  ScopedType ro_dtype(H5Topen2(ro_file.get(), "/dtype", H5P_DEFAULT));
  ASSERT_TRUE(ro_dtype.valid());
  EXPECT_GE(H5Trefresh(ro_dtype.get()), 0);
  EXPECT_GE(H5Orefresh(ro_dtype.get()), 0);

  // Open two handles to the same dataset so fo_count > 1 exercises multi-open
  // close & reopen helpers, then drop to a single handle to test corking
  // preservation across H5Drefresh (corked objects cannot stay open twice).
  ScopedDataset ro_dset1(H5Dopen2(ro_file.get(), "/dset", H5P_DEFAULT));
  ASSERT_TRUE(ro_dset1.valid());
  ScopedDataset ro_dset2(H5Dopen2(ro_file.get(), "/dset", H5P_DEFAULT));
  ASSERT_TRUE(ro_dset2.valid());

  EXPECT_GE(H5Drefresh(ro_dset1.get()), 0);
  std::array<int32_t, 4> read_vals = {};
  ASSERT_GE(H5Dread(ro_dset2.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, read_vals.data()),
            0);
  EXPECT_THAT(read_vals, ElementsAre(10, 20, 30, 40));

  ro_dset2.reset();

  ASSERT_GE(H5Odisable_mdc_flushes(ro_dset1.get()), 0);
  hbool_t disabled = false;
  ASSERT_GE(H5Oare_mdc_flushes_disabled(ro_dset1.get(), &disabled), 0);
  ASSERT_TRUE(disabled);

  EXPECT_GE(H5Drefresh(ro_dset1.get()), 0);
  disabled = false;
  ASSERT_GE(H5Oare_mdc_flushes_disabled(ro_dset1.get(), &disabled), 0);
  EXPECT_TRUE(disabled);
  EXPECT_GE(H5Oenable_mdc_flushes(ro_dset1.get()), 0);

  read_vals = {};
  ASSERT_GE(H5Dread(ro_dset1.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, read_vals.data()),
            0);
  EXPECT_THAT(read_vals, ElementsAre(10, 20, 30, 40));

  ScopedErrorSilence silence;
  EXPECT_LT(H5Orefresh(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Orefresh(space.get()), 0);
}

TEST(H5OflushTest, StartSwmrWriteRefreshesOpenGroupAndDataset) {
  TempFile tmp_file("swmr_refresh.h5");

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);
  ASSERT_GE(H5Pset_file_locking(fapl.get(), /*use_file_locking=*/false,
                                /*ignore_disabled_locks=*/true),
            0);

  ScopedFile file(
      H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  const std::array<hsize_t, 1> dims = {4};
  ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/d", H5T_NATIVE_INT32, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  const std::array<int32_t, 4> data = {3, 6, 9, 12};
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, data.data()),
            0);

  // H5Fstart_swmr_write calls H5O_refresh_metadata_reopen(..., start_swmr=true)
  // for all currently open groups and datasets.
  ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);

  std::array<int32_t, 4> read_back = {};
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_back.data()),
            0);
  EXPECT_THAT(read_back, ElementsAre(3, 6, 9, 12));
}

TEST(H5OflushTest, InternalFlushAndRefreshErrorPaths) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);

  // Write a zero-filled contiguous dataset so we have a valid file offset inside
  // EOA containing zeros (guaranteed not an object header and not in MDC).
  haddr_t bogus_oh_addr = HADDR_UNDEF;
  std::vector<uint8_t> image;
  {
    ScopedFile file(
        H5Fcreate("flush_err.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedGroup grp(
        H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    ScopedType dtype(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(dtype.valid());
    ASSERT_GE(H5Tcommit2(file.get(), "/dtype", dtype.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);

    const std::array<hsize_t, 1> dims = {16};
    ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_EARLY), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "/raw_zeros", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    const std::array<int32_t, 16> zeros = {};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, zeros.data()),
              0);

    H5O_loc_t* dset_oloc = H5O_get_loc(dset.get());
    ASSERT_NE(dset_oloc, nullptr);
    bogus_oh_addr = H5Dget_offset(dset.get());
    ASSERT_NE(bogus_oh_addr, HADDR_UNDEF);

    // 1. Direct H5O_flush / H5O_flush_common failure paths on RDWR file.
    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      ScopedErrorSilence silence;

      // Invalid obj_id fails H5VL_object.
      EXPECT_LT(H5O_flush(dset_oloc, H5I_INVALID_HID), 0);

      // Invalid ohdr address fails H5O__obj_class in H5O_flush and
      // H5O__oh_tag / H5O_protect in H5O_flush_common.
      H5O_loc_t bad_oloc = *dset_oloc;
      bad_oloc.addr = bogus_oh_addr;
      EXPECT_LT(H5O_flush(&bad_oloc, dset.get()), 0);
      EXPECT_LT(H5O_flush_common(&bad_oloc, dset.get()), 0);

      // Setting H5FD_FEAT_HAS_MPI triggers the parallel-unsupported guard.
      unsigned long orig_flags = 0;
      ASSERT_GE(H5FD_get_feature_flags(dset_oloc->file->shared->lf, &orig_flags),
                0);
      const auto restore_flags = absl::MakeCleanup([&]() {
        H5FD_set_feature_flags(dset_oloc->file->shared->lf, orig_flags);
      });
      ASSERT_GE(H5FD_set_feature_flags(dset_oloc->file->shared->lf,
                                       orig_flags | H5FD_FEAT_HAS_MPI),
                0);
      EXPECT_LT(H5O_flush(dset_oloc, dset.get()), 0);
    }

    dset.reset();
    dtype.reset();
    grp.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t image_len = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_len, 0);
    image.resize(static_cast<size_t>(image_len));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_len);
  }

  // 2. Reopen read-only to test H5O_refresh_metadata and
  // H5O_refresh_metadata_reopen error branches.
  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(H5Fopen("flush_err.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());

  ScopedGroup ro_grp(H5Gopen2(ro_file.get(), "/grp", H5P_DEFAULT));
  ASSERT_TRUE(ro_grp.valid());
  ScopedType ro_dtype(H5Topen2(ro_file.get(), "/dtype", H5P_DEFAULT));
  ASSERT_TRUE(ro_dtype.valid());
  ScopedDataset ro_dset(H5Dopen2(ro_file.get(), "/raw_zeros", H5P_DEFAULT));
  ASSERT_TRUE(ro_dset.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());

  H5O_loc_t* ro_oloc = H5O_get_loc(ro_dset.get());
  ASSERT_NE(ro_oloc, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  // Invalid oid fails H5VL_vol_object in H5O_refresh_metadata.
  EXPECT_LT(H5O_refresh_metadata(ro_oloc, H5I_INVALID_HID), 0);

  auto* vol_obj = static_cast<H5VL_object_t*>(H5VL_vol_object(ro_dset.get()));
  ASSERT_NE(vol_obj, nullptr);
  H5VL_connector_t* conn = H5VL_OBJ_CONNECTOR(vol_obj);
  ASSERT_NE(conn, nullptr);

  H5O_loc_t bad_oloc{};
  H5G_name_t empty_path{};
  H5G_loc_t bad_gloc{&bad_oloc, &empty_path};
  auto reset_bad_gloc = [&]() {
    H5G_loc_reset(&bad_gloc);
    bad_oloc.file = ro_oloc->file;
    bad_oloc.addr = bogus_oh_addr;
    bad_oloc.holding_file = false;
  };

  // H5O_refresh_metadata_reopen failure branches across object types. Note that
  // failed reopen calls free/reset bad_gloc on unwind, so reset before each.
  reset_bad_gloc();
  EXPECT_LT(H5O_refresh_metadata_reopen(space.get(), H5P_DEFAULT, &bad_gloc,
                                        conn, false),
            0);
  reset_bad_gloc();
  EXPECT_LT(H5O_refresh_metadata_reopen(ro_grp.get(), H5P_DEFAULT, &bad_gloc,
                                        conn, false),
            0);
  reset_bad_gloc();
  EXPECT_LT(H5O_refresh_metadata_reopen(ro_dtype.get(), H5P_DEFAULT, &bad_gloc,
                                        conn, false),
            0);
  // Passing a FAPL (not a DAPL) fails H5CX_set_apl for H5I_DATASET.
  reset_bad_gloc();
  EXPECT_LT(H5O_refresh_metadata_reopen(ro_dset.get(), ro_fapl.get(), &bad_gloc,
                                        conn, false),
            0);
  // Bogus object location fails H5D_open for H5I_DATASET.
  reset_bad_gloc();
  EXPECT_LT(H5O_refresh_metadata_reopen(ro_dset.get(), H5P_DEFAULT, &bad_gloc,
                                        conn, false),
            0);

  // Committed datatype whose underlying VOL data pointer is temporarily cleared
  // fails H5T_save_refresh_state cleanly inside H5O_refresh_metadata.
  {
    const H5O_loc_t* orig_dtype_oloc = H5O_get_loc(ro_dtype.get());
    ASSERT_NE(orig_dtype_oloc, nullptr);
    H5O_loc_t dtype_oloc = *orig_dtype_oloc;
    auto* dt =
        static_cast<H5T_t*>(H5I_object_verify(ro_dtype.get(), H5I_DATATYPE));
    ASSERT_NE(dt, nullptr);
    ASSERT_NE(dt->vol_obj, nullptr);
    void* saved_vol_data = dt->vol_obj->data;
    ASSERT_NE(saved_vol_data, nullptr);
    const auto restore_vol_data =
        absl::MakeCleanup([&]() { dt->vol_obj->data = saved_vol_data; });
    dt->vol_obj->data = nullptr;
    EXPECT_LT(H5O_refresh_metadata(&dtype_oloc, ro_dtype.get()), 0);
  }

  // H5I_MAP IDs are rejected explicitly in H5O_refresh_metadata_reopen.
  {
    ASSERT_GE(H5M_init(), 0);
    int dummy_map_obj = 0;
    const hid_t map_id = H5I_register(H5I_MAP, &dummy_map_obj, false);
    ASSERT_GE(map_id, 0);
    const auto remove_map_id = absl::MakeCleanup([map_id]() {
      if (map_id >= 0) {
        H5I_remove(map_id);
      }
    });
    reset_bad_gloc();
    EXPECT_LT(
        H5O_refresh_metadata_reopen(map_id, H5P_DEFAULT, &bad_gloc, conn, false),
        0);
  }
  // Remaining uncovered lines in H5Oflush.c are non-executable post-HGOTO_ERROR
  // braces/break (241, 247, 415), H5CX_DEBUG/H5_HAVE_PARALLEL-only APL
  // failure (383), upstream error-unwind resource leaks (256, 304, 391, 420),
  // and defensive internal invariants (158, 163, 251, 312, 320).
}

struct ProxyVfdController {
  hid_t sec2_fapl = H5I_INVALID_HID;
  bool fail_read = false;
  bool fail_write = false;
  bool fail_flush = false;
};

ProxyVfdController* g_proxy_ctrl = nullptr;

struct ProxyVfdFile {
  H5FD_t pub;  // Must be first so H5FD_t* casts cleanly to/from ProxyVfdFile*.
  H5FD_t* inner;
};

H5FD_t* ProxyOpen(const char* name, unsigned flags, hid_t /*fapl_id*/,
                  haddr_t /*maxaddr*/) {
  if (g_proxy_ctrl == nullptr) return nullptr;
  H5FD_t* inner =
      H5FDopen(name, flags, g_proxy_ctrl->sec2_fapl, HADDR_UNDEF);
  if (inner == nullptr) return nullptr;
  auto* file = new ProxyVfdFile{};
  file->inner = inner;
  return &file->pub;
}

herr_t ProxyClose(H5FD_t* f) {
  auto* file = reinterpret_cast<ProxyVfdFile*>(f);
  herr_t rc = H5FDclose(file->inner);
  delete file;
  return rc;
}

int ProxyCmp(const H5FD_t* f1, const H5FD_t* f2) {
  return H5FDcmp(reinterpret_cast<const ProxyVfdFile*>(f1)->inner,
                 reinterpret_cast<const ProxyVfdFile*>(f2)->inner);
}

herr_t ProxyQuery(const H5FD_t* f, unsigned long* flags) {
  if (f == nullptr) {
    if (flags != nullptr) *flags = 0;
    return 0;
  }
  return H5FDquery(reinterpret_cast<const ProxyVfdFile*>(f)->inner, flags);
}

haddr_t ProxyGetEoa(const H5FD_t* f, H5FD_mem_t type) {
  return H5FDget_eoa(reinterpret_cast<const ProxyVfdFile*>(f)->inner, type);
}

herr_t ProxySetEoa(H5FD_t* f, H5FD_mem_t type, haddr_t addr) {
  return H5FDset_eoa(reinterpret_cast<ProxyVfdFile*>(f)->inner, type, addr);
}

haddr_t ProxyGetEof(const H5FD_t* f, H5FD_mem_t type) {
  return H5FDget_eof(reinterpret_cast<const ProxyVfdFile*>(f)->inner, type);
}

herr_t ProxyGetHandle(H5FD_t* f, hid_t fapl, void** file_handle) {
  return H5FDget_vfd_handle(reinterpret_cast<ProxyVfdFile*>(f)->inner, fapl,
                            file_handle);
}

herr_t ProxyRead(H5FD_t* f, H5FD_mem_t type, hid_t dxpl_id, haddr_t addr,
                 size_t size, void* buf) {
  if (g_proxy_ctrl != nullptr && g_proxy_ctrl->fail_read) return -1;
  return H5FDread(reinterpret_cast<ProxyVfdFile*>(f)->inner, type, dxpl_id,
                  addr, size, buf);
}

herr_t ProxyWrite(H5FD_t* f, H5FD_mem_t type, hid_t dxpl_id, haddr_t addr,
                  size_t size, const void* buf) {
  if (g_proxy_ctrl != nullptr && g_proxy_ctrl->fail_write) return -1;
  return H5FDwrite(reinterpret_cast<ProxyVfdFile*>(f)->inner, type, dxpl_id,
                   addr, size, buf);
}

herr_t ProxyFlush(H5FD_t* f, hid_t dxpl_id, bool closing) {
  if (g_proxy_ctrl != nullptr && g_proxy_ctrl->fail_flush) return -1;
  return H5FDflush(reinterpret_cast<ProxyVfdFile*>(f)->inner, dxpl_id, closing);
}

herr_t ProxyTruncate(H5FD_t* f, hid_t dxpl_id, bool closing) {
  return H5FDtruncate(reinterpret_cast<ProxyVfdFile*>(f)->inner, dxpl_id,
                      closing);
}

herr_t ProxyLock(H5FD_t* f, bool rw) {
  return H5FDlock(reinterpret_cast<ProxyVfdFile*>(f)->inner, rw);
}

herr_t ProxyUnlock(H5FD_t* f) {
  return H5FDunlock(reinterpret_cast<ProxyVfdFile*>(f)->inner);
}

herr_t ProxyDelete(const char* name, hid_t /*fapl_id*/) {
  if (g_proxy_ctrl == nullptr) return -1;
  return H5FDdelete(name, g_proxy_ctrl->sec2_fapl);
}

herr_t ProxyCtl(H5FD_t* f, uint64_t op_code, uint64_t flags, const void* input,
                void** output) {
  return H5FDctl(reinterpret_cast<ProxyVfdFile*>(f)->inner, op_code, flags,
                 input, output);
}

const H5FD_class_t kProxyVfdClass = {
    H5FD_CLASS_VERSION,                                            // version
    static_cast<H5FD_class_value_t>(512),                          // value
    "oflush_fail_proxy",                                           // name
    static_cast<haddr_t>(std::numeric_limits<int64_t>::max()),     // maxaddr
    H5F_CLOSE_WEAK,                                                // fc_degree
    nullptr,                                                       // terminate
    nullptr,                                                       // sb_size
    nullptr,                                                       // sb_encode
    nullptr,                                                       // sb_decode
    0,                                                             // fapl_size
    nullptr,                                                       // fapl_get
    nullptr,                                                       // fapl_copy
    nullptr,                                                       // fapl_free
    0,                                                             // dxpl_size
    nullptr,                                                       // dxpl_copy
    nullptr,                                                       // dxpl_free
    ProxyOpen,                                                     // open
    ProxyClose,                                                    // close
    ProxyCmp,                                                      // cmp
    ProxyQuery,                                                    // query
    nullptr,                                                       // get_type_map
    nullptr,                                                       // alloc
    nullptr,                                                       // free
    ProxyGetEoa,                                                   // get_eoa
    ProxySetEoa,                                                   // set_eoa
    ProxyGetEof,                                                   // get_eof
    ProxyGetHandle,                                                // get_handle
    ProxyRead,                                                     // read
    ProxyWrite,                                                    // write
    nullptr,                                                       // read_vector
    nullptr,                                                       // write_vector
    nullptr,                                                       // read_selection
    nullptr,                                                       // write_selection
    ProxyFlush,                                                    // flush
    ProxyTruncate,                                                 // truncate
    ProxyLock,                                                     // lock
    ProxyUnlock,                                                   // unlock
    ProxyDelete,                                                   // del
    ProxyCtl,                                                      // ctl
    H5FD_FLMAP_DICHOTOMY,                                          // fl_map
};

TEST(H5OflushTest, ProxyVfdFlushRefreshIoAndMetadataCloseErrorPaths) {
  TempFile tmp_file("proxy_flush_err.h5");

  ScopedPlist sec2_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(sec2_fapl.valid());
  ASSERT_GE(H5Pset_fapl_sec2(sec2_fapl.get()), 0);
  ASSERT_GE(H5Pset_file_locking(sec2_fapl.get(), /*use_file_locking=*/false,
                                /*ignore_disabled_locks=*/true),
            0);

  ProxyVfdController ctrl;
  ctrl.sec2_fapl = sec2_fapl.get();
  g_proxy_ctrl = &ctrl;
  const auto reset_ctrl = absl::MakeCleanup([]() { g_proxy_ctrl = nullptr; });

  const hid_t proxy_vfd_id = H5FDregister(&kProxyVfdClass);
  ASSERT_GE(proxy_vfd_id, 0);
  const auto unregister_vfd = absl::MakeCleanup([proxy_vfd_id]() {
    if (proxy_vfd_id >= 0) {
      H5FDunregister(proxy_vfd_id);
    }
  });

  ScopedPlist proxy_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(proxy_fapl.valid());
  ASSERT_GE(H5Pset_driver(proxy_fapl.get(), proxy_vfd_id, nullptr), 0);
  ASSERT_GE(
      H5Pset_libver_bounds(proxy_fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
      0);

  H5O_token_t grp_token{};
  haddr_t bogus_oh_addr = HADDR_UNDEF;

  // Phase 1: Create chunked + contiguous datasets and test H5O_flush write
  // failure and H5O_flush_common VFD flush failure on RDWR file.
  {
    ScopedFile file(H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              proxy_fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedGroup grp(
        H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    H5O_info2_t grp_info{};
    ASSERT_GE(H5Oget_info3(grp.get(), &grp_info, H5O_INFO_BASIC), 0);
    grp_token = grp_info.token;

    // Early-allocated contiguous dataset gives a zero-filled offset outside
    // object header cache for H5O__oh_tag failure tests.
    const std::array<hsize_t, 1> contig_dims = {16};
    ScopedSpace contig_space(H5Screate_simple(1, contig_dims.data(), nullptr));
    ASSERT_TRUE(contig_space.valid());
    ScopedPlist contig_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(contig_dcpl.valid());
    ASSERT_GE(H5Pset_alloc_time(contig_dcpl.get(), H5D_ALLOC_TIME_EARLY), 0);
    ScopedDataset contig_dset(H5Dcreate2(file.get(), "/zeros", H5T_NATIVE_INT32,
                                         contig_space.get(), H5P_DEFAULT,
                                         contig_dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(contig_dset.valid());
    const std::array<int32_t, 16> zeros = {};
    ASSERT_GE(H5Dwrite(contig_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, zeros.data()),
              0);
    bogus_oh_addr = H5Dget_offset(contig_dset.get());
    ASSERT_NE(bogus_oh_addr, HADDR_UNDEF);

    // Pre-allocated chunked dataset with partial writes keeps dirty chunks in
    // rdcc so H5Oflush -> obj_class->flush fails when write fails.
    const std::array<hsize_t, 1> dset_dims = {4};
    ScopedSpace full_space(H5Screate_simple(1, dset_dims.data(), nullptr));
    ASSERT_TRUE(full_space.valid());
    ScopedPlist chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(chunk_dcpl.valid());
    ASSERT_GE(H5Pset_chunk(chunk_dcpl.get(), 1, dset_dims.data()), 0);
    ASSERT_GE(H5Pset_alloc_time(chunk_dcpl.get(), H5D_ALLOC_TIME_EARLY), 0);
    ScopedDataset chunk_dset(H5Dcreate2(file.get(), "/chunked",
                                        H5T_NATIVE_INT32, full_space.get(),
                                        H5P_DEFAULT, chunk_dcpl.get(),
                                        H5P_DEFAULT));
    ASSERT_TRUE(chunk_dset.valid());

    const std::array<hsize_t, 1> mem_dims = {2};
    ScopedSpace mem_space(H5Screate_simple(1, mem_dims.data(), nullptr));
    ASSERT_TRUE(mem_space.valid());
    ScopedSpace file_space(H5Dget_space(chunk_dset.get()));
    ASSERT_TRUE(file_space.valid());

    const std::array<hsize_t, 1> start0 = {0};
    const std::array<hsize_t, 1> count = {2};
    ASSERT_GE(H5Sselect_hyperslab(file_space.get(), H5S_SELECT_SET,
                                  start0.data(), nullptr, count.data(), nullptr),
              0);
    const std::array<int32_t, 2> part0 = {11, 22};
    ASSERT_GE(H5Dwrite(chunk_dset.get(), H5T_NATIVE_INT32, mem_space.get(),
                       file_space.get(), H5P_DEFAULT, part0.data()),
              0);
    ASSERT_GE(H5Oflush(chunk_dset.get()), 0);

    const std::array<hsize_t, 1> start1 = {2};
    ASSERT_GE(H5Sselect_hyperslab(file_space.get(), H5S_SELECT_SET,
                                  start1.data(), nullptr, count.data(), nullptr),
              0);
    const std::array<int32_t, 2> part1 = {33, 44};
    ASSERT_GE(H5Dwrite(chunk_dset.get(), H5T_NATIVE_INT32, mem_space.get(),
                       file_space.get(), H5P_DEFAULT, part1.data()),
              0);

    {
      ScopedErrorSilence silence;
      ctrl.fail_write = true;
      EXPECT_LT(H5Oflush(chunk_dset.get()), 0);
      ctrl.fail_write = false;
    }

    EXPECT_GE(H5Oflush(chunk_dset.get()), 0);

    {
      ScopedErrorSilence silence;
      ctrl.fail_flush = true;
      EXPECT_LT(H5Oflush(chunk_dset.get()), 0);
      ctrl.fail_flush = false;
    }
  }

  // Phase 2: Reopen RDONLY and cover H5O__refresh_metadata_close and
  // H5O_refresh_metadata_reopen error branches. Opening targets via
  // H5Oopen_by_token keeps obj_loc.path unallocated so failing refresh paths do
  // not leak copied paths.
  ScopedFile ro_file(
      H5Fopen(tmp_file.c_str(), H5F_ACC_RDONLY, proxy_fapl.get()));
  ASSERT_TRUE(ro_file.valid());

  // 2a. VFD flush failure in H5O__refresh_metadata_close.
  {
    hid_t raw_tok = H5Oopen_by_token(ro_file.get(), grp_token);
    ASSERT_GE(raw_tok, 0);
    const auto close_tok = absl::MakeCleanup([&]() {
      if (H5Iis_valid(raw_tok) > 0) H5Oclose(raw_tok);
    });
    const auto restore_flush =
        absl::MakeCleanup([&]() { ctrl.fail_flush = false; });
    ScopedErrorSilence silence;
    ctrl.fail_flush = true;
    EXPECT_LT(H5Grefresh(raw_tok), 0);
  }

  // 2b. VFD read failure during H5O_refresh_metadata_reopen after eviction.
  {
    hid_t raw_tok = H5Oopen_by_token(ro_file.get(), grp_token);
    ASSERT_GE(raw_tok, 0);
    const auto close_tok = absl::MakeCleanup([&]() {
      if (H5Iis_valid(raw_tok) > 0) H5Oclose(raw_tok);
    });
    const auto restore_read =
        absl::MakeCleanup([&]() { ctrl.fail_read = false; });
    ScopedErrorSilence silence;
    ctrl.fail_read = true;
    EXPECT_LT(H5Grefresh(raw_tok), 0);
  }

  // 2c. H5O__oh_tag failure inside H5O__refresh_metadata_close.
  {
    ScopedGroup tok_grp(H5Oopen_by_token(ro_file.get(), grp_token));
    ASSERT_TRUE(tok_grp.valid());
    H5O_loc_t* orig_oloc = H5O_get_loc(tok_grp.get());
    ASSERT_NE(orig_oloc, nullptr);
    H5O_loc_t bad_oloc = *orig_oloc;
    bad_oloc.addr = bogus_oh_addr;

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;
    EXPECT_LT(H5O_refresh_metadata(&bad_oloc, tok_grp.get()), 0);
  }

  // 2d. Pinned cache entry prevents H5AC_evict_tagged_metadata during refresh.
  {
    hid_t raw_tok = H5Oopen_by_token(ro_file.get(), grp_token);
    ASSERT_GE(raw_tok, 0);
    const auto close_tok = absl::MakeCleanup([&]() {
      if (H5Iis_valid(raw_tok) > 0) H5Oclose(raw_tok);
    });
    H5O_t* pinned_oh = nullptr;
    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5O_loc_t* oloc = H5O_get_loc(raw_tok);
      ASSERT_NE(oloc, nullptr);
      H5O_t* oh = H5O_protect(oloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      const herr_t pin_rc = H5AC_pin_protected_entry(oh);
      EXPECT_GE(H5O_unprotect(oloc, oh, H5AC__NO_FLAGS_SET), 0);
      ASSERT_GE(pin_rc, 0);
      pinned_oh = oh;
    }
    const auto unpin_oh = absl::MakeCleanup([&]() {
      if (pinned_oh != nullptr) {
        ScopedApiContext ctx;
        H5AC_unpin_entry(pinned_oh);
      }
    });
    ScopedErrorSilence silence;
    EXPECT_LT(H5Grefresh(raw_tok), 0);
  }

  // 2e. Corked object with a second open handle fails H5AC_cork(SET_CORK)
  // inside H5O__refresh_metadata_close because closing the first handle does
  // not uncork while another open exists.
  {
    ScopedGroup keep_grp(H5Gopen2(ro_file.get(), "/grp", H5P_DEFAULT));
    ASSERT_TRUE(keep_grp.valid());
    hid_t cork_tok = H5Oopen_by_token(ro_file.get(), grp_token);
    ASSERT_GE(cork_tok, 0);
    const auto close_tok = absl::MakeCleanup([&]() {
      if (H5Iis_valid(cork_tok) > 0) H5Oclose(cork_tok);
    });
    ASSERT_GE(H5Odisable_mdc_flushes(cork_tok), 0);
    const auto uncork_grp = absl::MakeCleanup([&]() {
      H5Oenable_mdc_flushes(keep_grp.get());
    });
    ScopedErrorSilence silence;
    EXPECT_LT(H5Grefresh(cork_tok), 0);
  }
}

}  // namespace
}  // namespace hdf5
