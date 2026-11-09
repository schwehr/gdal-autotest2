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
// Tests HDF5 High-Level Dataset Optimizations (third_party/hdf5/hl/src/H5DO.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/src/H5DO.c
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/test/test_dset_opt.c

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5_test_util.h"
#include "third_party/hdf5/hl/src/H5DOpublic.h"
#include "third_party/hdf5/hl/src/hdf5_hl.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

struct AppendCallbackState {
  int call_count = 0;
  herr_t return_status = 0;
  hsize_t last_dim0 = 0;
  hsize_t last_dim1 = 0;
};

herr_t RecordAppendFlush(hid_t /*dataset_id*/, hsize_t* cur_dims,
                         void* user_data) {
  auto* state = static_cast<AppendCallbackState*>(user_data);
  ++state->call_count;
  state->last_dim0 = cur_dims[0];
  state->last_dim1 = cur_dims[1];
  return state->return_status;
}

TEST(H5doTest, WriteAndReadChunkRoundTripAndInvalidId) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  constexpr std::array<hsize_t, 2> kDims = {4, 4};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 2};
  ScopedDataspace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_GE(space.get(), 0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunkDims.data()), 0);

  ScopedDataset dset(H5Dcreate2(file, "chunked", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_GE(dset.get(), 0);

  constexpr std::array<int32_t, 4> kChunkData = {10, 20, 30, 40};
  constexpr std::array<hsize_t, 2> kOffset = {2, 2};
  EXPECT_GE(H5DOwrite_chunk(dset, H5P_DEFAULT, 0, kOffset.data(),
                            sizeof(kChunkData), kChunkData.data()),
            0);

  std::array<int32_t, 4> read_buf = {-1, -1, -1, -1};
  uint32_t read_filters = 999;
  EXPECT_GE(H5DOread_chunk(dset, H5P_DEFAULT, kOffset.data(), &read_filters,
                           read_buf.data()),
            0);
  EXPECT_EQ(0u, read_filters);
  EXPECT_THAT(read_buf, ElementsAre(10, 20, 30, 40));

  ScopedErrorSilence silence;
  EXPECT_LT(H5DOwrite_chunk(H5I_INVALID_HID, H5P_DEFAULT, 0, kOffset.data(),
                            sizeof(kChunkData), kChunkData.data()),
            0);
  EXPECT_LT(H5DOread_chunk(H5I_INVALID_HID, H5P_DEFAULT, kOffset.data(),
                           &read_filters, read_buf.data()),
            0);
}

TEST(H5doTest, AppendAlongBothAxesWithAndWithoutDxpl) {
  ScopedFile file = CreateCoreFile(/*latest_format=*/true);
  ASSERT_GE(file.get(), 0);

  constexpr std::array<hsize_t, 2> kInitDims = {0, 3};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, H5S_UNLIMITED};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 3};
  ScopedDataspace space(H5Screate_simple(2, kInitDims.data(), kMaxDims.data()));
  ASSERT_GE(space.get(), 0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunkDims.data()), 0);

  ScopedDataset dset(H5Dcreate2(file, "appendable", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_GE(dset.get(), 0);

  // 1. Append 2 rows along axis 0 using default DXPL.
  constexpr std::array<int32_t, 6> kRows = {1, 2, 3, 4, 5, 6};
  ASSERT_GE(H5DOappend(dset, H5P_DEFAULT, 0, 2, H5T_NATIVE_INT32, kRows.data()),
            0);

  // 2. Append 1 column along axis 1 using an explicit DXPL.
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(dxpl.get(), 0);
  constexpr std::array<int32_t, 2> kCol = {7, 8};
  ASSERT_GE(H5DOappend(dset, dxpl, 1, 1, H5T_NATIVE_INT32, kCol.data()), 0);

  // 3. Verify updated dataspace dimensions and elements.
  ScopedDataspace cur_space(H5Dget_space(dset));
  ASSERT_GE(cur_space.get(), 0);
  std::array<hsize_t, 2> cur_dims = {0, 0};
  ASSERT_EQ(2, H5Sget_simple_extent_dims(cur_space, cur_dims.data(), nullptr));
  EXPECT_THAT(cur_dims, ElementsAre(2, 4));

  std::vector<int32_t> actual(8, -999);
  ASSERT_GE(
      H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl, actual.data()),
      0);
  EXPECT_THAT(actual, ElementsAre(1, 2, 3, 7, 4, 5, 6, 8));
}

TEST(H5doTest, AppendFlushBoundariesAndCallbackStatus) {
  ScopedFile file = CreateCoreFile(/*latest_format=*/true);
  ASSERT_GE(file.get(), 0);

  constexpr std::array<hsize_t, 2> kInitDims = {0, 2};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, 2};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 2};
  ScopedDataspace space(H5Screate_simple(2, kInitDims.data(), kMaxDims.data()));
  ASSERT_GE(space.get(), 0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunkDims.data()), 0);

  AppendCallbackState cb_state;
  constexpr std::array<hsize_t, 2> kBoundary = {3, 0};
  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_GE(dapl.get(), 0);
  ASSERT_GE(H5Pset_append_flush(dapl, 2, kBoundary.data(), RecordAppendFlush,
                                &cb_state),
            0);

  ScopedDataset dset(H5Dcreate2(file, "flushed", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, dapl));
  ASSERT_GE(dset.get(), 0);

  // 1. Append 2 rows (0 -> 2): does not cross boundary multiple 3.
  constexpr std::array<int32_t, 4> kBatch = {10, 11, 12, 13};
  ASSERT_GE(
      H5DOappend(dset, H5P_DEFAULT, 0, 2, H5T_NATIVE_INT32, kBatch.data()), 0);
  EXPECT_EQ(0, cb_state.call_count);

  // 2. Append 2 rows (2 -> 4): crosses k = 2 (row 3), invoking callback +
  // flush.
  ASSERT_GE(
      H5DOappend(dset, H5P_DEFAULT, 0, 2, H5T_NATIVE_INT32, kBatch.data()), 0);
  EXPECT_EQ(1, cb_state.call_count);
  EXPECT_EQ(4u, cb_state.last_dim0);
  EXPECT_EQ(2u, cb_state.last_dim1);

  // 3. Callback returning FAIL aborts H5DOappend at the next boundary (6);
  //    H5Dset_extent (4 -> 6) and H5Dwrite already completed before the flush
  //    callback failed, so step 4 uses a fresh dataset.
  cb_state.return_status = -1;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5DOappend(dset, H5P_DEFAULT, 0, 2, H5T_NATIVE_INT32, kBatch.data()),
        0);
  }
  EXPECT_EQ(2, cb_state.call_count);

  // 4. Null callback on append flush still succeeds and calls H5Dflush.
  constexpr std::array<hsize_t, 2> kBoundaryNullCb = {2, 0};
  ScopedPlist dapl_null_cb(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_GE(dapl_null_cb.get(), 0);
  ASSERT_GE(H5Pset_append_flush(dapl_null_cb, 2, kBoundaryNullCb.data(),
                                nullptr, nullptr),
            0);
  ScopedDataset dset_null_cb(H5Dcreate2(file, "flushed_null_cb",
                                        H5T_NATIVE_INT32, space, H5P_DEFAULT,
                                        dcpl, dapl_null_cb));
  ASSERT_GE(dset_null_cb.get(), 0);
  EXPECT_GE(H5DOappend(dset_null_cb, H5P_DEFAULT, 0, 2, H5T_NATIVE_INT32,
                       kBatch.data()),
            0);
}

TEST(H5doTest, AppendRejectsInvalidArgumentsAndOverflow) {
  ScopedFile file = CreateCoreFile(/*latest_format=*/true);
  ASSERT_GE(file.get(), 0);

  constexpr std::array<hsize_t, 2> kInitDims = {2, 2};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, 2};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 2};
  ScopedDataspace chunk_space(
      H5Screate_simple(2, kInitDims.data(), kMaxDims.data()));
  ASSERT_GE(chunk_space.get(), 0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunkDims.data()), 0);

  ScopedDataset chunk_dset(H5Dcreate2(file, "ext", H5T_NATIVE_INT32,
                                      chunk_space, H5P_DEFAULT, dcpl,
                                      H5P_DEFAULT));
  ASSERT_GE(chunk_dset.get(), 0);

  // Non-extendible contiguous dataset causes H5Dset_extent() to fail.
  ScopedDataspace contig_space(H5Screate_simple(2, kInitDims.data(), nullptr));
  ASSERT_GE(contig_space.get(), 0);
  ScopedDataset contig_dset(H5Dcreate2(file, "contig", H5T_NATIVE_INT32,
                                       contig_space, H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
  ASSERT_GE(contig_dset.get(), 0);

  // Wrong property list class for dxpl_id.
  ScopedPlist bad_dxpl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(bad_dxpl.get(), 0);

  constexpr std::array<int32_t, 2> kRow = {1, 2};
  ScopedErrorSilence silence;

  EXPECT_LT(H5DOappend(file, H5P_DEFAULT, 0, 1, H5T_NATIVE_INT32, kRow.data()),
            0);
  EXPECT_LT(
      H5DOappend(chunk_dset, bad_dxpl, 0, 1, H5T_NATIVE_INT32, kRow.data()), 0);
  EXPECT_LT(
      H5DOappend(chunk_dset, H5P_DEFAULT, 2, 1, H5T_NATIVE_INT32, kRow.data()),
      0);
  EXPECT_LT(
      H5DOappend(chunk_dset, H5P_DEFAULT, 0, std::numeric_limits<size_t>::max(),
                 H5T_NATIVE_INT32, kRow.data()),
      0);
  EXPECT_LT(
      H5DOappend(contig_dset, H5P_DEFAULT, 0, 1, H5T_NATIVE_INT32, kRow.data()),
      0);
  EXPECT_LT(
      H5DOappend(chunk_dset, H5P_DEFAULT, 0, 1, H5I_INVALID_HID, kRow.data()),
      0);
}

// Minimal pass-through VOL connector that forwards directly to H5VL_NATIVE
// while allowing selective fault injection on dataset_cls.{get,specific}.
struct FaultVolState {
  int get_space_calls = 0;
  int fail_get_space_call = 0;
  int bad_get_space_call = 0;
  bool fail_get_dapl = false;
  bool bad_get_dapl = false;
  bool fail_flush = false;

  void Reset() { *this = FaultVolState{}; }
};

FaultVolState g_fault_vol_state;

void* FaultFileCreate(const char* name, unsigned flags, hid_t fcpl_id,
                      hid_t fapl_id, hid_t dxpl_id, void** req) {
  ScopedPlist under_fapl(H5Pcopy(fapl_id));
  if (!under_fapl.valid() || H5Pset_vol(under_fapl, H5VL_NATIVE, nullptr) < 0) {
    return nullptr;
  }
  return H5VLfile_create(name, flags, fcpl_id, under_fapl, dxpl_id, req);
}

herr_t FaultFileOptional(void* file, H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLfile_optional(file, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultFileGet(void* file, H5VL_file_get_args_t* args, hid_t dxpl_id,
                    void** req) {
  return H5VLfile_get(file, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultFileClose(void* file, hid_t dxpl_id, void** req) {
  return H5VLfile_close(file, H5VL_NATIVE, dxpl_id, req);
}

void* FaultDatasetCreate(void* obj, const H5VL_loc_params_t* loc_params,
                         const char* name, hid_t lcpl_id, hid_t type_id,
                         hid_t space_id, hid_t dcpl_id, hid_t dapl_id,
                         hid_t dxpl_id, void** req) {
  return H5VLdataset_create(obj, loc_params, H5VL_NATIVE, name, lcpl_id,
                            type_id, space_id, dcpl_id, dapl_id, dxpl_id, req);
}

void* FaultDatasetOpen(void* obj, const H5VL_loc_params_t* loc_params,
                       const char* name, hid_t dapl_id, hid_t dxpl_id,
                       void** req) {
  return H5VLdataset_open(obj, loc_params, H5VL_NATIVE, name, dapl_id, dxpl_id,
                          req);
}

herr_t FaultDatasetRead(size_t count, void* dset[], hid_t mem_type_id[],
                        hid_t mem_space_id[], hid_t file_space_id[],
                        hid_t dxpl_id, void* buf[], void** req) {
  return H5VLdataset_read(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                          file_space_id, dxpl_id, buf, req);
}

herr_t FaultDatasetOptional(void* obj, H5VL_optional_args_t* args,
                            hid_t dxpl_id, void** req) {
  return H5VLdataset_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultDatasetWrite(size_t count, void* dset[], hid_t mem_type_id[],
                         hid_t mem_space_id[], hid_t file_space_id[],
                         hid_t dxpl_id, const void* buf[], void** req) {
  return H5VLdataset_write(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                           file_space_id, dxpl_id, buf, req);
}

herr_t FaultDatasetGet(void* dset, H5VL_dataset_get_args_t* args, hid_t dxpl_id,
                       void** req) {
  if (args->op_type == H5VL_DATASET_GET_SPACE) {
    ++g_fault_vol_state.get_space_calls;
    if (g_fault_vol_state.fail_get_space_call ==
        g_fault_vol_state.get_space_calls) {
      return -1;
    }
    if (g_fault_vol_state.bad_get_space_call ==
        g_fault_vol_state.get_space_calls) {
      args->args.get_space.space_id = H5S_ALL;  // 0 is never a valid object ID
      return 0;
    }
  } else if (args->op_type == H5VL_DATASET_GET_DAPL) {
    if (g_fault_vol_state.fail_get_dapl) {
      return -1;
    }
    if (g_fault_vol_state.bad_get_dapl) {
      args->args.get_dapl.dapl_id =
          H5P_DEFAULT;  // 0 is never a valid object ID
      return 0;
    }
  }
  return H5VLdataset_get(dset, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultDatasetSpecific(void* obj, H5VL_dataset_specific_args_t* args,
                            hid_t dxpl_id, void** req) {
  if (args->op_type == H5VL_DATASET_FLUSH && g_fault_vol_state.fail_flush) {
    return -1;
  }
  return H5VLdataset_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultDatasetClose(void* dset, hid_t dxpl_id, void** req) {
  return H5VLdataset_close(dset, H5VL_NATIVE, dxpl_id, req);
}

extern const H5VL_class_t kFaultVolClass;

herr_t FaultIntrospectGetConnCls(void* obj, H5VL_get_conn_lvl_t lvl,
                                 const H5VL_class_t** conn_cls) {
  if (lvl == H5VL_GET_CONN_LVL_CURR) {
    *conn_cls = &kFaultVolClass;
    return 0;
  }
  return H5VLintrospect_get_conn_cls(obj, H5VL_NATIVE, lvl, conn_cls);
}

herr_t FaultIntrospectGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  return H5VLintrospect_get_cap_flags(nullptr, H5VL_NATIVE, cap_flags);
}

herr_t FaultIntrospectOptQuery(void* obj, H5VL_subclass_t cls, int opt_type,
                               uint64_t* flags) {
  return H5VLintrospect_opt_query(obj, H5VL_NATIVE, cls, opt_type, flags);
}

const H5VL_class_t kFaultVolClass = {
    .version = H5VL_VERSION,
    .value = static_cast<H5VL_class_value_t>(51001),
    .name = "h5do_fault_vol",
    .conn_version = 1,
    .cap_flags = 0,
    .initialize = nullptr,
    .terminate = nullptr,
    .info_cls = {},
    .wrap_cls = {},
    .attr_cls = {},
    .dataset_cls =
        {
            .create = FaultDatasetCreate,
            .open = FaultDatasetOpen,
            .read = FaultDatasetRead,
            .write = FaultDatasetWrite,
            .get = FaultDatasetGet,
            .specific = FaultDatasetSpecific,
            .optional = FaultDatasetOptional,
            .close = FaultDatasetClose,
        },
    .datatype_cls = {},
    .file_cls =
        {
            .create = FaultFileCreate,
            .open = nullptr,
            .get = FaultFileGet,
            .specific = nullptr,
            .optional = FaultFileOptional,
            .close = FaultFileClose,
        },
    .group_cls = {},
    .link_cls = {},
    .object_cls = {},
    .introspect_cls =
        {
            .get_conn_cls = FaultIntrospectGetConnCls,
            .get_cap_flags = FaultIntrospectGetCapFlags,
            .opt_query = FaultIntrospectOptQuery,
        },
    .request_cls = {},
    .blob_cls = {},
    .token_cls = {},
    .optional = nullptr,
};

TEST(H5doTest, AppendPropagatesInternalDatasetAndDataspaceFailures) {
  g_fault_vol_state.Reset();
  const hid_t vol_id = H5VLregister_connector(&kFaultVolClass, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  const absl::Cleanup cleanup_vol = [vol_id] {
    g_fault_vol_state.Reset();
    H5VLunregister_connector(vol_id);
  };

  ScopedFile file =
      CreateCoreVolFile(vol_id, "h5do_fault_vol.h5", /*latest_format=*/true);
  ASSERT_GE(file.get(), 0);

  constexpr std::array<hsize_t, 2> kInitDims = {0, 2};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, 2};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 2};
  ScopedDataspace space(H5Screate_simple(2, kInitDims.data(), kMaxDims.data()));
  ASSERT_GE(space.get(), 0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunkDims.data()), 0);

  constexpr std::array<hsize_t, 2> kBoundary = {1, 0};
  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_GE(dapl.get(), 0);
  ASSERT_GE(H5Pset_append_flush(dapl, 2, kBoundary.data(), nullptr, nullptr),
            0);

  ScopedDataset dset(H5Dcreate2(file, "faulted", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, dapl));
  ASSERT_GE(dset.get(), 0);

  constexpr std::array<int32_t, 2> kRow = {1, 2};
  ScopedErrorSilence silence;

  // 1. Fail first H5Dget_space() (H5DO.c:123).
  g_fault_vol_state.Reset();
  g_fault_vol_state.fail_get_space_call = 1;
  EXPECT_LT(H5DOappend(dset, H5P_DEFAULT, 0, 1, H5T_NATIVE_INT32, kRow.data()),
            0);

  // 2. Return H5S_ALL from first H5Dget_space() -> H5Sget_simple_extent_ndims &
  //    H5Sclose fail (H5DO.c:127, 219).
  g_fault_vol_state.Reset();
  g_fault_vol_state.bad_get_space_call = 1;
  EXPECT_LT(H5DOappend(dset, H5P_DEFAULT, 0, 1, H5T_NATIVE_INT32, kRow.data()),
            0);

  // 3. Fail second H5Dget_space() after extent growth (H5DO.c:152).
  g_fault_vol_state.Reset();
  g_fault_vol_state.fail_get_space_call = 2;
  EXPECT_LT(H5DOappend(dset, H5P_DEFAULT, 0, 1, H5T_NATIVE_INT32, kRow.data()),
            0);

  // 4. Return H5S_ALL from second H5Dget_space() -> H5Sselect_hyperslab &
  //    H5Sclose fail (H5DO.c:166, 223).
  g_fault_vol_state.Reset();
  g_fault_vol_state.bad_get_space_call = 2;
  EXPECT_LT(H5DOappend(dset, H5P_DEFAULT, 0, 1, H5T_NATIVE_INT32, kRow.data()),
            0);

  // 5. Fail H5Dget_access_plist() (H5DO.c:183).
  g_fault_vol_state.Reset();
  g_fault_vol_state.fail_get_dapl = true;
  EXPECT_LT(H5DOappend(dset, H5P_DEFAULT, 0, 1, H5T_NATIVE_INT32, kRow.data()),
            0);

  // 6. Return H5P_DEFAULT from H5Dget_access_plist() -> H5Pget_append_flush &
  //    H5Pclose fail (H5DO.c:190, 231).
  g_fault_vol_state.Reset();
  g_fault_vol_state.bad_get_dapl = true;
  EXPECT_LT(H5DOappend(dset, H5P_DEFAULT, 0, 1, H5T_NATIVE_INT32, kRow.data()),
            0);

  // 7. Fail H5Dflush() at boundary (H5DO.c:209).
  g_fault_vol_state.Reset();
  g_fault_vol_state.fail_flush = true;
  EXPECT_LT(H5DOappend(dset, H5P_DEFAULT, 0, 1, H5T_NATIVE_INT32, kRow.data()),
            0);
}

}  // namespace
}  // namespace hdf5

// TODO(schwehr): Remaining uncovered lines in H5DO.c are defensive error
// branches on internal HDF5 calls not reachable via public VOL callbacks.
