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
// Tests HDF5 v1.12+ reference creation, inspection, copying, comparison,
// synchronous/asynchronous dereferencing, name queries, and external-file
// reopening (third_party/hdf5/src/H5R.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5R.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

// `:hdf5` compiles with -DNDEBUG, which omits `#ifndef NDEBUG` fields from
// internal cache/metadata structs. Define NDEBUG around internal HDF5 headers
// so struct layouts match the compiled library.
#ifndef NDEBUG
#define H5R_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5R_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Rpkg.h"
}  // extern "C"
#ifdef H5R_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5R_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// Move-only RAII guard for H5R_ref_t that invokes H5Rdestroy() on exit.
class ScopedRef {
 public:
  ScopedRef() { std::memset(&ref_, 0, sizeof(ref_)); }
  ~ScopedRef() { reset(); }

  ScopedRef(const ScopedRef&) = delete;
  ScopedRef& operator=(const ScopedRef&) = delete;

  void reset() {
    if (active_) {
      H5Rdestroy(&ref_);
      active_ = false;
    }
    std::memset(&ref_, 0, sizeof(ref_));
  }

  H5R_ref_t* get() {
    active_ = true;
    return &ref_;
  }
  const H5R_ref_t* cget() const { return &ref_; }
  void disarm() { active_ = false; }

 private:
  H5R_ref_t ref_{};
  bool active_ = false;
};

// Minimal async-capable VOL connector used to exercise H5ES_insert and
// VOL error branches inside H5Rcreate_* / H5Ropen_* / H5Rget_*.
struct MockAsyncState {
  int file_token = 1;
  int obj_token = 2;
  int attr_token = 3;
  std::array<int, 16> req_tokens{};
  size_t next_req = 0;
  int wait_calls = 0;
  int free_calls = 0;
  int close_calls = 0;
  bool fail_get_file = false;
  bool fail_get_cont_info = false;
  bool fail_get_file_name = false;
  bool fail_get_obj_name = false;
  bool fail_get_space = false;
  bool bad_space_non_dataspace = false;
};

MockAsyncState& GetMockAsyncState() {
  static MockAsyncState state;
  return state;
}

extern const H5VL_class_t kMockAsyncVolClass;

void* MockVolFileCreate(const char* /*name*/, unsigned /*flags*/,
                        hid_t /*fcpl_id*/, hid_t /*fapl_id*/,
                        hid_t /*dxpl_id*/, void** /*req*/) {
  return &GetMockAsyncState().file_token;
}

herr_t MockVolFileGet(void* /*obj*/, H5VL_file_get_args_t* args,
                      hid_t /*dxpl_id*/, void** /*req*/) {
  const auto& state = GetMockAsyncState();
  if (args->op_type == H5VL_FILE_GET_CONT_INFO) {
    if (state.fail_get_cont_info) {
      return -1;
    }
    args->args.get_cont_info.info->version = H5VL_CONTAINER_INFO_VERSION;
    args->args.get_cont_info.info->feature_flags = 0;
    args->args.get_cont_info.info->token_size = sizeof(haddr_t);
    args->args.get_cont_info.info->blob_id_size = 0;
    return 0;
  }
  if (args->op_type == H5VL_FILE_GET_NAME) {
    if (state.fail_get_file_name) {
      return -1;
    }
    if (args->args.get_name.buf != nullptr &&
        args->args.get_name.buf_size > 0) {
      args->args.get_name.buf[0] = '\0';
    }
    *args->args.get_name.file_name_len = 0;
    return 0;
  }
  return -1;
}

herr_t MockVolFileClose(void* /*file*/, hid_t /*dxpl_id*/, void** /*req*/) {
  return 0;
}

void* MockVolObjectOpen(void* /*obj*/, const H5VL_loc_params_t* /*params*/,
                        H5I_type_t* opened_type, hid_t /*dxpl_id*/,
                        void** req) {
  *opened_type = H5I_DATASET;
  if (req != nullptr) {
    auto& state = GetMockAsyncState();
    *req = &state.req_tokens[state.next_req % state.req_tokens.size()];
    ++state.next_req;
  }
  return &GetMockAsyncState().obj_token;
}

herr_t MockVolObjectGet(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                        H5VL_object_get_args_t* args, hid_t /*dxpl_id*/,
                        void** /*req*/) {
  auto& state = GetMockAsyncState();
  if (args->op_type == H5VL_OBJECT_GET_FILE) {
    if (state.fail_get_file) {
      return -1;
    }
    *args->args.get_file.file = &state.file_token;
    return 0;
  }
  if (args->op_type == H5VL_OBJECT_GET_NAME) {
    if (state.fail_get_obj_name) {
      return -1;
    }
    if (args->args.get_name.buf != nullptr &&
        args->args.get_name.buf_size > 0) {
      args->args.get_name.buf[0] = '\0';
    }
    *args->args.get_name.name_len = 0;
    return 0;
  }
  return -1;
}

herr_t MockVolObjectSpecific(void* /*obj*/,
                             const H5VL_loc_params_t* /*loc_params*/,
                             H5VL_object_specific_args_t* args,
                             hid_t /*dxpl_id*/, void** /*req*/) {
  if (args->op_type == H5VL_OBJECT_LOOKUP &&
      args->args.lookup.token_ptr != nullptr) {
    std::memset(args->args.lookup.token_ptr, 0, sizeof(H5O_token_t));
    args->args.lookup.token_ptr->__data[0] = 1;
    return 0;
  }
  return -1;
}

herr_t MockVolDatasetGet(void* /*obj*/, H5VL_dataset_get_args_t* args,
                         hid_t /*dxpl_id*/, void** /*req*/) {
  const auto& state = GetMockAsyncState();
  if (args->op_type == H5VL_DATASET_GET_SPACE) {
    if (state.fail_get_space) {
      return -1;
    }
    if (state.bad_space_non_dataspace) {
      args->args.get_space.space_id = H5Tcopy(H5T_NATIVE_INT32);
      return args->args.get_space.space_id >= 0 ? 0 : -1;
    }
    constexpr hsize_t kDim = 4;
    args->args.get_space.space_id = H5Screate_simple(1, &kDim, nullptr);
    return args->args.get_space.space_id >= 0 ? 0 : -1;
  }
  return -1;
}

herr_t MockVolDatasetClose(void* /*dset*/, hid_t /*dxpl_id*/, void** /*req*/) {
  ++GetMockAsyncState().close_calls;
  return 0;
}

void* MockVolAttrOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                      const char* /*attr_name*/, hid_t /*aapl_id*/,
                      hid_t /*dxpl_id*/, void** /*req*/) {
  return &GetMockAsyncState().attr_token;
}

herr_t MockVolAttrClose(void* /*attr*/, hid_t /*dxpl_id*/, void** /*req*/) {
  return 0;
}

herr_t MockVolGetConnCls(void* /*obj*/, H5VL_get_conn_lvl_t /*lvl*/,
                         const H5VL_class_t** conn_cls) {
  *conn_cls = &kMockAsyncVolClass;
  return 0;
}

herr_t MockVolGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  *cap_flags = kMockAsyncVolClass.cap_flags;
  return 0;
}

herr_t MockVolOptQuery(void* /*obj*/, H5VL_subclass_t /*cls*/,
                       int /*opt_type*/, uint64_t* flags) {
  if (flags != nullptr) {
    *flags = 0;
  }
  return 0;
}

herr_t MockVolRequestWait(void* /*req*/, uint64_t /*timeout*/,
                          H5VL_request_status_t* status) {
  ++GetMockAsyncState().wait_calls;
  if (status != nullptr) {
    *status = H5VL_REQUEST_STATUS_SUCCEED;
  }
  return 0;
}

herr_t MockVolRequestFree(void* /*req*/) {
  ++GetMockAsyncState().free_calls;
  return 0;
}

const H5VL_class_t kMockAsyncVolClass = {
    .version = H5VL_VERSION,
    .value = static_cast<H5VL_class_value_t>(5098),
    .name = "h5r_test_async_vol",
    .conn_version = 1,
    .cap_flags = H5VL_CAP_FLAG_ASYNC,
    .initialize = nullptr,
    .terminate = nullptr,
    .info_cls = {},
    .wrap_cls = {},
    .attr_cls =
        {
            .open = MockVolAttrOpen,
            .close = MockVolAttrClose,
        },
    .dataset_cls =
        {
            .get = MockVolDatasetGet,
            .close = MockVolDatasetClose,
        },
    .datatype_cls = {},
    .file_cls =
        {
            .create = MockVolFileCreate,
            .get = MockVolFileGet,
            .close = MockVolFileClose,
        },
    .group_cls = {},
    .link_cls = {},
    .object_cls =
        {
            .open = MockVolObjectOpen,
            .get = MockVolObjectGet,
            .specific = MockVolObjectSpecific,
        },
    .introspect_cls =
        {
            .get_conn_cls = MockVolGetConnCls,
            .get_cap_flags = MockVolGetCapFlags,
            .opt_query = MockVolOptQuery,
        },
    .request_cls =
        {
            .wait = MockVolRequestWait,
            .free = MockVolRequestFree,
        },
    .blob_cls = {},
    .token_cls = {},
    .optional = nullptr,
};

TEST(H5RTest, CreateObjectInvalidArgumentsFail) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  ScopedRef ref;
  ScopedErrorSilence silence;
  EXPECT_LT(H5Rcreate_object(file, "obj", H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5Rcreate_object(file, nullptr, H5P_DEFAULT, ref.get()), 0);
  ref.disarm();
  EXPECT_LT(H5Rcreate_object(file, "", H5P_DEFAULT, ref.get()), 0);
  ref.disarm();
  EXPECT_LT(H5Rcreate_object(file, "obj", H5I_INVALID_HID, ref.get()), 0);
  ref.disarm();
  EXPECT_LT(H5Rcreate_object(file, "obj", fapl, ref.get()), 0);
  ref.disarm();
  EXPECT_LT(H5Rcreate_object(H5I_INVALID_HID, "obj", H5P_DEFAULT, ref.get()),
            0);
  ref.disarm();
  EXPECT_LT(H5Rcreate_object(scalar, "obj", H5P_DEFAULT, ref.get()), 0);
  ref.disarm();
  EXPECT_LT(H5Rcreate_object(file, "/missing", H5P_DEFAULT, ref.get()), 0);
  ref.disarm();
}

TEST(H5RTest, CreateObjectInspectNamesTypeAndOpenRoundTrip) {
  // 1. Populate an in-memory file with a group, dataset, committed datatype,
  //    and attribute.
  ScopedFile file = CreateInMemoryFile("ref_obj_file.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(H5Gcreate2(file, "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  constexpr hsize_t kDims[1] = {4};
  ScopedSpace space(H5Screate_simple(1, kDims, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(grp, "dset", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  constexpr std::array<int32_t, 4> kValues = {10, 20, 30, 40};
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     kValues.data()),
            0);

  ScopedType dtype(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(dtype.valid());
  ASSERT_GE(H5Tcommit2(grp, "dtype", dtype, H5P_DEFAULT, H5P_DEFAULT,
                       H5P_DEFAULT),
            0);

  ScopedSpace attr_space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(attr_space.valid());
  ScopedAttr attr(H5Acreate2(dset, "tag", H5T_NATIVE_INT32, attr_space,
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());

  // 2. Create object references using both H5P_DEFAULT and an explicit LAPL,
  //    anchored from file, group, and attribute handles.
  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl.valid());

  ScopedRef grp_ref;
  ScopedRef dset_ref;
  ScopedRef dtype_ref;
  ASSERT_GE(H5Rcreate_object(file, "/grp", lapl, grp_ref.get()), 0);
  ASSERT_GE(H5Rcreate_object(grp, "dset", H5P_DEFAULT, dset_ref.get()), 0);
  ASSERT_GE(H5Rcreate_object(attr, "/grp/dtype", H5P_DEFAULT, dtype_ref.get()),
            0);

  EXPECT_EQ(H5Rget_type(grp_ref.cget()), H5R_OBJECT2);
  EXPECT_EQ(H5Rget_type(dset_ref.cget()), H5R_OBJECT2);
  EXPECT_EQ(H5Rget_type(dtype_ref.cget()), H5R_OBJECT2);

  // 3. Query referenced object types.
  H5O_type_t obj_type = H5O_TYPE_UNKNOWN;
  ASSERT_GE(H5Rget_obj_type3(grp_ref.get(), H5P_DEFAULT, &obj_type), 0);
  EXPECT_EQ(obj_type, H5O_TYPE_GROUP);
  ASSERT_GE(H5Rget_obj_type3(dset_ref.get(), H5P_DEFAULT, &obj_type), 0);
  EXPECT_EQ(obj_type, H5O_TYPE_DATASET);
  ASSERT_GE(H5Rget_obj_type3(dtype_ref.get(), H5P_DEFAULT, &obj_type), 0);
  EXPECT_EQ(obj_type, H5O_TYPE_NAMED_DATATYPE);

  // 4. Verify H5Rget_file_name length-query, zero-size non-clobber,
  //    truncation, and full-buffer behavior.
  const ssize_t file_name_len = H5Rget_file_name(dset_ref.cget(), nullptr, 0);
  ASSERT_EQ(file_name_len, 15);
  std::array<char, 32> name_buf{};
  name_buf.fill('#');
  EXPECT_EQ(H5Rget_file_name(dset_ref.cget(), name_buf.data(), 0), 15);
  EXPECT_EQ(name_buf[0], '#');
  EXPECT_EQ(H5Rget_file_name(dset_ref.cget(), name_buf.data(), 4), 15);
  EXPECT_STREQ(name_buf.data(), "ref");
  EXPECT_EQ(
      H5Rget_file_name(dset_ref.cget(), name_buf.data(), name_buf.size()), 15);
  EXPECT_STREQ(name_buf.data(), "ref_obj_file.h5");

  // 5. Verify H5Rget_obj_name length-query, zero-size non-clobber, truncation,
  //    and full-buffer behavior.
  EXPECT_EQ(H5Rget_obj_name(dset_ref.get(), H5P_DEFAULT, nullptr, 0), 9);
  name_buf.fill('#');
  EXPECT_EQ(H5Rget_obj_name(dset_ref.get(), H5P_DEFAULT, name_buf.data(), 0),
            9);
  EXPECT_EQ(name_buf[0], '#');
  EXPECT_EQ(H5Rget_obj_name(dset_ref.get(), H5P_DEFAULT, name_buf.data(), 5),
            9);
  EXPECT_STREQ(name_buf.data(), "/grp");
  EXPECT_EQ(H5Rget_obj_name(dset_ref.get(), H5P_DEFAULT, name_buf.data(),
                            name_buf.size()),
            9);
  EXPECT_STREQ(name_buf.data(), "/grp/dset");

  // 6. Open referenced objects synchronously and asynchronously (H5ES_NONE and
  //    with a real EventSet) and verify dataset payload.
  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl.valid());
  ScopedDataset opened_dset(H5Ropen_object(dset_ref.get(), H5P_DEFAULT, dapl));
  ASSERT_TRUE(opened_dset.valid());
  std::array<int32_t, 4> read_vals = {-1, -1, -1, -1};
  ASSERT_GE(H5Dread(opened_dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, read_vals.data()),
            0);
  EXPECT_THAT(read_vals, ElementsAre(10, 20, 30, 40));

  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());
  ScopedDataset async_none(
      H5Ropen_object_async(dset_ref.get(), H5P_DEFAULT, H5P_DEFAULT, H5ES_NONE));
  ASSERT_TRUE(async_none.valid());
  ScopedDataset async_es(
      H5Ropen_object_async(dset_ref.get(), H5P_DEFAULT, H5P_DEFAULT, es));
  ASSERT_TRUE(async_es.valid());

  // 7. Open object error paths.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ScopedErrorSilence silence;
  EXPECT_EQ(H5Ropen_object(nullptr, H5P_DEFAULT, H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_object(dset_ref.get(), H5I_INVALID_HID, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_object(dset_ref.get(), H5P_DEFAULT, H5I_INVALID_HID),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_object(dset_ref.get(), H5P_DEFAULT, fcpl), H5I_INVALID_HID);
  EXPECT_EQ(
      H5Ropen_object_async(nullptr, H5P_DEFAULT, H5P_DEFAULT, H5ES_NONE),
      H5I_INVALID_HID);
}

TEST(H5RTest, CreateRegionInvalidArgsAndSelectionRoundTrip) {
  ScopedFile file = CreateInMemoryFile("ref_reg.h5");
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kDims[2] = {3, 4};
  ScopedSpace space(H5Screate_simple(2, kDims, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file, "/matrix", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  std::array<int32_t, 12> data{};
  for (int i = 0; i < 12; ++i) {
    data[i] = i + 1;
  }
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     data.data()),
            0);

  // Invalid creation arguments.
  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl.valid());
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  {
    ScopedRef bad_ref;
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Rcreate_region(file, "/matrix", space, H5P_DEFAULT, nullptr), 0);
    EXPECT_LT(
        H5Rcreate_region(file, nullptr, space, H5P_DEFAULT, bad_ref.get()), 0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_region(file, "", space, H5P_DEFAULT, bad_ref.get()), 0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_region(file, "/matrix", H5I_INVALID_HID, H5P_DEFAULT,
                               bad_ref.get()),
              0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_region(file, "/matrix", H5S_ALL, H5P_DEFAULT,
                               bad_ref.get()),
              0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_region(file, "/matrix", H5S_BLOCK, H5P_DEFAULT,
                               bad_ref.get()),
              0);
    bad_ref.disarm();
    EXPECT_LT(
        H5Rcreate_region(file, "/matrix", file, H5P_DEFAULT, bad_ref.get()), 0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_region(file, "/matrix", space, H5I_INVALID_HID,
                               bad_ref.get()),
              0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_region(file, "/matrix", space, fapl, bad_ref.get()), 0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_region(H5I_INVALID_HID, "/matrix", space, H5P_DEFAULT,
                               bad_ref.get()),
              0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_region(space, "/matrix", space, H5P_DEFAULT,
                               bad_ref.get()),
              0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_region(file, "/missing", space, H5P_DEFAULT,
                               bad_ref.get()),
              0);
    bad_ref.disarm();
  }

  // Create hyperslab region reference and point-selection region reference.
  ScopedSpace hyper_space(H5Scopy(space));
  ASSERT_TRUE(hyper_space.valid());
  constexpr hsize_t kStart[2] = {1, 1};
  constexpr hsize_t kCount[2] = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(hyper_space, H5S_SELECT_SET, kStart, nullptr,
                                kCount, nullptr),
            0);

  ScopedRef hyper_ref;
  ASSERT_GE(
      H5Rcreate_region(file, "/matrix", hyper_space, lapl, hyper_ref.get()), 0);
  EXPECT_EQ(H5Rget_type(hyper_ref.cget()), H5R_DATASET_REGION2);

  ScopedSpace pt_space(H5Scopy(space));
  ASSERT_TRUE(pt_space.valid());
  constexpr hsize_t kCoords[4] = {0, 0, 2, 3};
  ASSERT_GE(H5Sselect_elements(pt_space, H5S_SELECT_SET, 2, kCoords), 0);

  ScopedRef pt_ref;
  ASSERT_GE(
      H5Rcreate_region(file, "/matrix", pt_space, H5P_DEFAULT, pt_ref.get()),
      0);

  // Open region synchronously and asynchronously.
  ScopedSpace out_space(H5Ropen_region(hyper_ref.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(out_space.valid());
  EXPECT_EQ(H5Sget_select_type(out_space), H5S_SEL_HYPERSLABS);
  EXPECT_EQ(H5Sget_select_npoints(out_space), 4);

  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());
  ScopedSpace out_pt_none(
      H5Ropen_region_async(pt_ref.get(), H5P_DEFAULT, H5P_DEFAULT, H5ES_NONE));
  ASSERT_TRUE(out_pt_none.valid());
  EXPECT_EQ(H5Sget_select_type(out_pt_none), H5S_SEL_POINTS);
  EXPECT_EQ(H5Sget_select_npoints(out_pt_none), 2);

  ScopedSpace out_pt_es(
      H5Ropen_region_async(pt_ref.get(), H5P_DEFAULT, H5P_DEFAULT, es));
  ASSERT_TRUE(out_pt_es.valid());

  // Read back the selected elements using H5Ropen_object + H5Ropen_region.
  ScopedDataset opened_dset(
      H5Ropen_object(pt_ref.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(opened_dset.valid());
  constexpr hsize_t kMemDims[1] = {2};
  ScopedSpace mem_space(H5Screate_simple(1, kMemDims, nullptr));
  ASSERT_TRUE(mem_space.valid());
  std::array<int32_t, 2> pts = {-1, -1};
  ASSERT_GE(H5Dread(opened_dset, H5T_NATIVE_INT32, mem_space, out_pt_none,
                    H5P_DEFAULT, pts.data()),
            0);
  EXPECT_THAT(pts, ElementsAre(1, 12));

  // Negative region-open checks.
  ScopedRef obj_ref;
  ASSERT_GE(H5Rcreate_object(file, "/matrix", H5P_DEFAULT, obj_ref.get()), 0);
  ScopedErrorSilence silence;
  EXPECT_EQ(H5Ropen_region(nullptr, H5P_DEFAULT, H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_region(obj_ref.get(), H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_region(hyper_ref.get(), H5I_INVALID_HID, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_region(hyper_ref.get(), H5P_DEFAULT, H5I_INVALID_HID),
            H5I_INVALID_HID);
  EXPECT_EQ(
      H5Ropen_region_async(nullptr, H5P_DEFAULT, H5P_DEFAULT, H5ES_NONE),
      H5I_INVALID_HID);
}

TEST(H5RTest, CreateAttrOpenAttrAndGetAttrName) {
  ScopedFile file = CreateInMemoryFile("ref_attr.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(H5Gcreate2(file, "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedAttr attr(H5Acreate2(grp, "version", H5T_NATIVE_INT32, scalar,
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  constexpr int32_t kVersion = 42;
  ASSERT_GE(H5Awrite(attr, H5T_NATIVE_INT32, &kVersion), 0);

  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl.valid());
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ScopedPlist aapl(H5Pcreate(H5P_ATTRIBUTE_ACCESS));
  ASSERT_TRUE(aapl.valid());

  // Invalid creation arguments, including attribute name > H5R_MAX_STRING_LEN.
  {
    ScopedRef bad_ref;
    const std::string oversized_attr(65537, 'a');
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Rcreate_attr(file, "/g", "version", H5P_DEFAULT, nullptr), 0);
    EXPECT_LT(
        H5Rcreate_attr(file, nullptr, "version", H5P_DEFAULT, bad_ref.get()),
        0);
    bad_ref.disarm();
    EXPECT_LT(
        H5Rcreate_attr(file, "", "version", H5P_DEFAULT, bad_ref.get()), 0);
    bad_ref.disarm();
    EXPECT_LT(
        H5Rcreate_attr(file, "/g", nullptr, H5P_DEFAULT, bad_ref.get()), 0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_attr(file, "/g", "", H5P_DEFAULT, bad_ref.get()), 0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_attr(file, "/g", "version", H5I_INVALID_HID,
                             bad_ref.get()),
              0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_attr(file, "/g", "version", fapl, bad_ref.get()), 0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_attr(H5I_INVALID_HID, "/g", "version", H5P_DEFAULT,
                             bad_ref.get()),
              0);
    bad_ref.disarm();
    EXPECT_LT(
        H5Rcreate_attr(scalar, "/g", "version", H5P_DEFAULT, bad_ref.get()), 0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_attr(file, "/missing", "version", H5P_DEFAULT,
                             bad_ref.get()),
              0);
    bad_ref.disarm();
    EXPECT_LT(H5Rcreate_attr(file, "/g", oversized_attr.c_str(), H5P_DEFAULT,
                             bad_ref.get()),
              0);
    bad_ref.disarm();
  }

  ScopedRef attr_ref;
  ASSERT_GE(H5Rcreate_attr(file, "/g", "version", lapl, attr_ref.get()), 0);
  EXPECT_EQ(H5Rget_type(attr_ref.cget()), H5R_ATTR);

  H5O_type_t obj_type = H5O_TYPE_UNKNOWN;
  ASSERT_GE(H5Rget_obj_type3(attr_ref.get(), H5P_DEFAULT, &obj_type), 0);
  EXPECT_EQ(obj_type, H5O_TYPE_GROUP);

  // Attribute name queries (length, zero size non-clobber, truncated, full).
  EXPECT_EQ(H5Rget_attr_name(attr_ref.cget(), nullptr, 0), 7);
  std::array<char, 16> attr_buf{};
  attr_buf.fill('#');
  EXPECT_EQ(H5Rget_attr_name(attr_ref.cget(), attr_buf.data(), 0), 7);
  EXPECT_EQ(attr_buf[0], '#');
  EXPECT_EQ(H5Rget_attr_name(attr_ref.cget(), attr_buf.data(), 4), 7);
  EXPECT_STREQ(attr_buf.data(), "ver");
  EXPECT_EQ(
      H5Rget_attr_name(attr_ref.cget(), attr_buf.data(), attr_buf.size()), 7);
  EXPECT_STREQ(attr_buf.data(), "version");

  // Open attribute synchronously and asynchronously.
  ScopedAttr opened_attr(H5Ropen_attr(attr_ref.get(), H5P_DEFAULT, aapl));
  ASSERT_TRUE(opened_attr.valid());
  int32_t read_val = -1;
  ASSERT_GE(H5Aread(opened_attr, H5T_NATIVE_INT32, &read_val), 0);
  EXPECT_EQ(read_val, 42);

  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());
  ScopedAttr async_none(
      H5Ropen_attr_async(attr_ref.get(), H5P_DEFAULT, H5P_DEFAULT, H5ES_NONE));
  ASSERT_TRUE(async_none.valid());
  ScopedAttr async_es(
      H5Ropen_attr_async(attr_ref.get(), H5P_DEFAULT, H5P_DEFAULT, es));
  ASSERT_TRUE(async_es.valid());

  // Error checks for attr-name and attr-open.
  ScopedRef obj_ref;
  ASSERT_GE(H5Rcreate_object(file, "/g", H5P_DEFAULT, obj_ref.get()), 0);
  ScopedRef missing_attr_ref;
  ASSERT_GE(
      H5Rcreate_attr(file, "/g", "no_such_attr", H5P_DEFAULT,
                     missing_attr_ref.get()),
      0);
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  ScopedErrorSilence silence;
  EXPECT_LT(H5Rget_attr_name(nullptr, attr_buf.data(), attr_buf.size()), 0);
  EXPECT_LT(H5Rget_attr_name(obj_ref.cget(), attr_buf.data(), attr_buf.size()),
            0);
  EXPECT_EQ(H5Ropen_attr(nullptr, H5P_DEFAULT, H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_attr(obj_ref.get(), H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_attr(attr_ref.get(), H5I_INVALID_HID, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_attr(attr_ref.get(), H5P_DEFAULT, H5I_INVALID_HID),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_attr(attr_ref.get(), H5P_DEFAULT, fcpl), H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_attr(missing_attr_ref.get(), H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_attr_async(nullptr, H5P_DEFAULT, H5P_DEFAULT, H5ES_NONE),
            H5I_INVALID_HID);
}

TEST(H5RTest, EqualCopyDestroySemanticsAndNullArgs) {
  ScopedFile file = CreateInMemoryFile("ref_cmp.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(H5Gcreate2(file, "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  constexpr hsize_t kDims1[1] = {4};
  constexpr hsize_t kDims2[1] = {8};
  ScopedSpace s1(H5Screate_simple(1, kDims1, nullptr));
  ScopedSpace s2(H5Screate_simple(1, kDims2, nullptr));
  ASSERT_TRUE(s1.valid());
  ASSERT_TRUE(s2.valid());
  ScopedDataset d1(H5Dcreate2(file, "/d1", H5T_NATIVE_INT32, s1, H5P_DEFAULT,
                              H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset d2(H5Dcreate2(file, "/d2", H5T_NATIVE_INT32, s2, H5P_DEFAULT,
                              H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(d1.valid());
  ASSERT_TRUE(d2.valid());

  ScopedSpace attr_sp(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(attr_sp.valid());
  ScopedAttr a1(H5Acreate2(d1, "a1", H5T_NATIVE_INT32, attr_sp, H5P_DEFAULT,
                           H5P_DEFAULT));
  ScopedAttr a2(H5Acreate2(d1, "a2", H5T_NATIVE_INT32, attr_sp, H5P_DEFAULT,
                           H5P_DEFAULT));
  ASSERT_TRUE(a1.valid());
  ASSERT_TRUE(a2.valid());

  ScopedRef r_g;
  ScopedRef r_d1;
  ScopedRef r_d1_copy;
  ScopedRef r_reg1;
  ScopedRef r_reg2;
  ScopedRef r_a1;
  ScopedRef r_a2;

  ASSERT_GE(H5Rcreate_object(file, "/g", H5P_DEFAULT, r_g.get()), 0);
  ASSERT_GE(H5Rcreate_object(file, "/d1", H5P_DEFAULT, r_d1.get()), 0);
  ASSERT_GE(H5Rcopy(r_d1.cget(), r_d1_copy.get()), 0);
  ASSERT_GE(H5Rcreate_region(file, "/d1", s1, H5P_DEFAULT, r_reg1.get()), 0);
  ASSERT_GE(H5Rcreate_region(file, "/d2", s2, H5P_DEFAULT, r_reg2.get()), 0);
  ASSERT_GE(H5Rcreate_attr(file, "/d1", "a1", H5P_DEFAULT, r_a1.get()), 0);
  ASSERT_GE(H5Rcreate_attr(file, "/d1", "a2", H5P_DEFAULT, r_a2.get()), 0);

  EXPECT_GT(H5Requal(r_d1.cget(), r_d1_copy.cget()), 0);
  EXPECT_EQ(H5Requal(r_d1.cget(), r_g.cget()), 0);
  EXPECT_EQ(H5Requal(r_d1.cget(), r_reg1.cget()), 0);
  EXPECT_EQ(H5Requal(r_reg1.cget(), r_reg2.cget()), 0);
  EXPECT_EQ(H5Requal(r_a1.cget(), r_a2.cget()), 0);

  // Destroy original and verify copied reference still resolves cleanly.
  r_d1.reset();
  H5O_type_t obj_type = H5O_TYPE_UNKNOWN;
  ASSERT_GE(H5Rget_obj_type3(r_d1_copy.get(), H5P_DEFAULT, &obj_type), 0);
  EXPECT_EQ(obj_type, H5O_TYPE_DATASET);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Rdestroy(nullptr), 0);
  EXPECT_EQ(H5Rget_type(nullptr), H5R_BADTYPE);
  EXPECT_LT(H5Requal(nullptr, r_g.cget()), 0);
  EXPECT_LT(H5Requal(r_g.cget(), nullptr), 0);
  EXPECT_LT(H5Rcopy(nullptr, r_d1_copy.get()), 0);
  EXPECT_LT(H5Rcopy(r_g.cget(), nullptr), 0);
  EXPECT_LT(H5Rget_obj_type3(nullptr, H5P_DEFAULT, &obj_type), 0);
  EXPECT_LT(H5Rget_obj_type3(r_g.get(), H5I_INVALID_HID, &obj_type), 0);
  std::array<char, 16> err_buf{};
  EXPECT_LT(H5Rget_file_name(nullptr, err_buf.data(), err_buf.size()), 0);
  EXPECT_LT(H5Rget_obj_name(nullptr, H5P_DEFAULT, err_buf.data(),
                            err_buf.size()),
            0);
  EXPECT_LT(H5Rget_obj_name(r_g.get(), H5I_INVALID_HID, err_buf.data(),
                            err_buf.size()),
            0);
}

TEST(H5RTest, ExternalReferencePersistenceAndReopen) {
  TempDir tmp("h5r_external");
  const std::string target_path = tmp.child("target.h5").native();
  const std::string holder_path = tmp.child("holder.h5").native();

  // 1. Create target.h5 on disk with a dataset and attribute.
  {
    ScopedFile target(
        H5Fcreate(target_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(target.valid());
    constexpr hsize_t kDims[1] = {4};
    ScopedSpace space(H5Screate_simple(1, kDims, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(target, "/ds", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    constexpr std::array<int32_t, 4> kData = {7, 8, 9, 10};
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kData.data()),
              0);

    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    ScopedAttr attr(H5Acreate2(dset, "label", H5T_NATIVE_INT32, scalar,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    constexpr int32_t kLabel = 99;
    ASSERT_GE(H5Awrite(attr, H5T_NATIVE_INT32, &kLabel), 0);
  }

  // 2. Create object, region, and attribute references to target.h5 and store
  //    them in holder.h5 so HDF5 encodes them as external references.
  {
    ScopedFile target(
        H5Fopen(target_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(target.valid());
    constexpr hsize_t kDims[1] = {4};
    ScopedSpace sel_space(H5Screate_simple(1, kDims, nullptr));
    ASSERT_TRUE(sel_space.valid());
    constexpr hsize_t kStart[1] = {1};
    constexpr hsize_t kCount[1] = {2};
    ASSERT_GE(H5Sselect_hyperslab(sel_space, H5S_SELECT_SET, kStart, nullptr,
                                  kCount, nullptr),
              0);

    std::array<H5R_ref_t, 3> refs{};
    absl::Cleanup destroy_refs = [&refs] {
      for (H5R_ref_t& r : refs) {
        H5Rdestroy(&r);
      }
    };
    ASSERT_GE(H5Rcreate_object(target, "/ds", H5P_DEFAULT, &refs[0]), 0);
    ASSERT_GE(
        H5Rcreate_region(target, "/ds", sel_space, H5P_DEFAULT, &refs[1]), 0);
    ASSERT_GE(H5Rcreate_attr(target, "/ds", "label", H5P_DEFAULT, &refs[2]), 0);

    ScopedFile holder(
        H5Fcreate(holder_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(holder.valid());
    constexpr hsize_t kRefCount[1] = {3};
    ScopedSpace ref_space(H5Screate_simple(1, kRefCount, nullptr));
    ASSERT_TRUE(ref_space.valid());
    ScopedDataset ref_dset(H5Dcreate2(holder, "/refs", H5T_STD_REF, ref_space,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ref_dset.valid());
    ASSERT_GE(H5Dwrite(ref_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       refs.data()),
              0);
  }

  // 3. Re-read external references from holder.h5 (unopened state: loc_id ==
  //    H5I_INVALID_HID, filename set).
  auto read_external_refs = [&](std::array<H5R_ref_t, 3>& out_refs) {
    ScopedFile holder(
        H5Fopen(holder_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(holder.valid());
    ScopedDataset ref_dset(H5Dopen2(holder, "/refs", H5P_DEFAULT));
    ASSERT_TRUE(ref_dset.valid());
    ASSERT_GE(H5Dread(ref_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      out_refs.data()),
              0);
  };

  std::array<H5R_ref_t, 3> refs_a{};
  absl::Cleanup destroy_a = [&refs_a] {
    for (H5R_ref_t& r : refs_a) {
      H5Rdestroy(&r);
    }
  };
  read_external_refs(refs_a);

  // Query external filename while still unopened.
  std::array<char, 512> fname{};
  const ssize_t flen =
      H5Rget_file_name(&refs_a[0], fname.data(), fname.size());
  EXPECT_GT(flen, 0);
  EXPECT_STREQ(fname.data(), target_path.c_str());

  // Make separate copies before reopening so each API hits H5R__reopen_file().
  ScopedRef obj_for_open;
  ScopedRef obj_for_type;
  ScopedRef obj_for_name;
  ScopedRef reg_for_open;
  ScopedRef attr_for_open;
  ASSERT_GE(H5Rcopy(&refs_a[0], obj_for_open.get()), 0);
  ASSERT_GE(H5Rcopy(&refs_a[0], obj_for_type.get()), 0);
  ASSERT_GE(H5Rcopy(&refs_a[0], obj_for_name.get()), 0);
  ASSERT_GE(H5Rcopy(&refs_a[1], reg_for_open.get()), 0);
  ASSERT_GE(H5Rcopy(&refs_a[2], attr_for_open.get()), 0);

  ScopedDataset ext_dset(
      H5Ropen_object(obj_for_open.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ext_dset.valid());
  H5O_type_t ext_type = H5O_TYPE_UNKNOWN;
  ASSERT_GE(H5Rget_obj_type3(obj_for_type.get(), H5P_DEFAULT, &ext_type), 0);
  EXPECT_EQ(ext_type, H5O_TYPE_DATASET);
  std::array<char, 64> oname{};
  EXPECT_EQ(
      H5Rget_obj_name(obj_for_name.get(), H5P_DEFAULT, oname.data(),
                      oname.size()),
      3);
  EXPECT_STREQ(oname.data(), "/ds");
  ScopedSpace ext_space(
      H5Ropen_region(reg_for_open.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ext_space.valid());
  EXPECT_EQ(H5Sget_select_npoints(ext_space), 2);
  ScopedAttr ext_attr(
      H5Ropen_attr(attr_for_open.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ext_attr.valid());
  int32_t label = 0;
  ASSERT_GE(H5Aread(ext_attr, H5T_NATIVE_INT32, &label), 0);
  EXPECT_EQ(label, 99);

  // 4. Delete target.h5 and confirm reopening fails cleanly across all APIs.
  std::error_code ec;
  std::filesystem::remove(target_path, ec);
  std::array<H5R_ref_t, 3> refs_missing{};
  absl::Cleanup destroy_missing = [&refs_missing] {
    for (H5R_ref_t& r : refs_missing) {
      H5Rdestroy(&r);
    }
  };
  read_external_refs(refs_missing);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Ropen_object(&refs_missing[0], H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_region(&refs_missing[1], H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_attr(&refs_missing[2], H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_LT(H5Rget_obj_type3(&refs_missing[0], H5P_DEFAULT, &ext_type), 0);
  EXPECT_LT(H5Rget_obj_name(&refs_missing[0], H5P_DEFAULT, oname.data(),
                            oname.size()),
            0);
}

TEST(H5RTest, UnlinkedObjectNameAndCorruptPrivateRefs) {
  ScopedFile file = CreateInMemoryFile("ref_err.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file, "/ephemeral", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedRef unlinked_ref;
  ASSERT_GE(
      H5Rcreate_object(file, "/ephemeral", H5P_DEFAULT, unlinked_ref.get()), 0);
  ASSERT_GE(H5Ldelete(file, "/ephemeral", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Rget_obj_name(unlinked_ref.get(), H5P_DEFAULT, nullptr, 0), 0);

  // Construct synthetic H5R_ref_priv_t values to hit internal-failure and
  // corrupt-token branches in H5R.c without leaking any HDF5 handles.
  ScopedSpace non_vol_id(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(non_vol_id.valid());

  H5R_ref_t bad_type_raw{};
  auto* bad_type_priv = reinterpret_cast<H5R_ref_priv_t*>(&bad_type_raw);
  bad_type_priv->loc_id = H5I_INVALID_HID;
  bad_type_priv->type = H5R_MAXTYPE;
  bad_type_priv->token_size = sizeof(haddr_t);

  H5R_ref_t bad_type_copy_dst{};
  H5O_type_t obj_type = H5O_TYPE_UNKNOWN;
  std::array<char, 32> buf{};

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Rget_type(&bad_type_raw), H5R_BADTYPE);
  EXPECT_LT(H5Rdestroy(&bad_type_raw), 0);
  EXPECT_EQ(H5Requal(&bad_type_raw, unlinked_ref.cget()), 0);
  EXPECT_LT(H5Requal(&bad_type_raw, &bad_type_raw), 0);
  EXPECT_LT(H5Rcopy(&bad_type_raw, &bad_type_copy_dst), 0);
  EXPECT_EQ(H5Ropen_object(&bad_type_raw, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_LT(H5Rget_obj_type3(&bad_type_raw, H5P_DEFAULT, &obj_type), 0);
  EXPECT_LT(H5Rget_file_name(&bad_type_raw, buf.data(), buf.size()), 0);
  EXPECT_LT(H5Rget_obj_name(&bad_type_raw, H5P_DEFAULT, buf.data(), buf.size()),
            0);

  // Valid type but zero token size with attached file loc_id.
  H5R_ref_t zero_tok_raw{};
  auto* zero_tok_priv = reinterpret_cast<H5R_ref_priv_t*>(&zero_tok_raw);
  zero_tok_priv->type = H5R_OBJECT2;
  zero_tok_priv->token_size = 0;
  zero_tok_priv->loc_id = file.get();
  EXPECT_EQ(H5Ropen_object(&zero_tok_raw, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_LT(H5Rget_obj_type3(&zero_tok_raw, H5P_DEFAULT, &obj_type), 0);
  EXPECT_LT(H5Rget_obj_name(&zero_tok_raw, H5P_DEFAULT, buf.data(), buf.size()),
            0);

  zero_tok_priv->type = H5R_DATASET_REGION2;
  EXPECT_EQ(H5Ropen_region(&zero_tok_raw, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  zero_tok_priv->type = H5R_ATTR;
  EXPECT_EQ(H5Ropen_attr(&zero_tok_raw, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);

  // Valid type, H5I_INVALID_HID loc_id, and nullptr filename -> H5Rget_file_name
  // hits H5R__get_file_name() failure.
  zero_tok_priv->type = H5R_OBJECT2;
  zero_tok_priv->loc_id = H5I_INVALID_HID;
  zero_tok_priv->info.obj.filename = nullptr;
  EXPECT_LT(H5Rget_file_name(&zero_tok_raw, buf.data(), buf.size()), 0);

  // Valid type with non-VOL loc_id (dataspace ID) -> H5VL_vol_object() fails.
  zero_tok_priv->loc_id = non_vol_id.get();
  zero_tok_priv->token_size = sizeof(haddr_t);
  zero_tok_priv->info.obj.token.__data[0] = 0x11;
  EXPECT_EQ(H5Ropen_object(&zero_tok_raw, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_LT(H5Rget_obj_type3(&zero_tok_raw, H5P_DEFAULT, &obj_type), 0);
  EXPECT_LT(H5Rget_file_name(&zero_tok_raw, buf.data(), buf.size()), 0);
  EXPECT_LT(H5Rget_obj_name(&zero_tok_raw, H5P_DEFAULT, buf.data(), buf.size()),
            0);
  zero_tok_priv->type = H5R_ATTR;
  EXPECT_EQ(H5Ropen_attr(&zero_tok_raw, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);

  // Valid type, valid file loc_id, bogus token -> VOL object open/get fails.
  H5R_ref_t bogus_tok_raw{};
  auto* bogus_tok_priv = reinterpret_cast<H5R_ref_priv_t*>(&bogus_tok_raw);
  bogus_tok_priv->type = H5R_OBJECT2;
  bogus_tok_priv->loc_id = file.get();
  bogus_tok_priv->token_size = sizeof(haddr_t);
  std::memset(bogus_tok_priv->info.obj.token.__data, 0x7f, sizeof(haddr_t));
  EXPECT_EQ(H5Ropen_object(&bogus_tok_raw, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_LT(H5Rget_obj_type3(&bogus_tok_raw, H5P_DEFAULT, &obj_type), 0);
  EXPECT_EQ(H5Rget_obj_name(&bogus_tok_raw, H5P_DEFAULT, buf.data(), buf.size()),
            0);
  bogus_tok_priv->type = H5R_DATASET_REGION2;
  EXPECT_EQ(H5Ropen_region(&bogus_tok_raw, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  char attr_name[] = "attr";
  bogus_tok_priv->type = H5R_ATTR;
  bogus_tok_priv->info.attr.name = attr_name;
  EXPECT_EQ(H5Ropen_attr(&bogus_tok_raw, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
}

TEST(H5RTest, AsyncVolTokenInsertSuccessAndFailure) {
  GetMockAsyncState() = MockAsyncState{};
  absl::Cleanup reset_mock_state = [] {
    GetMockAsyncState() = MockAsyncState{};
  };

  const hid_t vol_id =
      H5VLregister_connector(&kMockAsyncVolClass, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  absl::Cleanup unregister_vol = [vol_id] { H5VLunregister_connector(vol_id); };

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_vol(fapl, vol_id, nullptr), 0);

  ScopedFile mock_file(
      H5Fcreate("mock_async.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(mock_file.valid());

  constexpr hsize_t kDim[1] = {4};
  ScopedSpace space(H5Screate_simple(1, kDim, nullptr));
  ASSERT_TRUE(space.valid());

  H5O_token_t tok{};
  tok.__data[0] = 1;

  ScopedRef obj_ref;
  auto* obj_priv = reinterpret_cast<H5R_ref_priv_t*>(obj_ref.get());
  ASSERT_GE(H5R__create_object(&tok, sizeof(haddr_t), obj_priv), 0);
  ASSERT_GE(H5R__set_loc_id(obj_priv, mock_file.get(), true, true), 0);

  ScopedRef reg_ref;
  auto* reg_priv = reinterpret_cast<H5R_ref_priv_t*>(reg_ref.get());
  auto* space_ptr =
      static_cast<H5S_t*>(H5I_object_verify(space.get(), H5I_DATASPACE));
  ASSERT_NE(space_ptr, nullptr);
  ASSERT_GE(H5R__create_region(&tok, sizeof(haddr_t), space_ptr, reg_priv), 0);
  ASSERT_GE(H5R__set_loc_id(reg_priv, mock_file.get(), true, true), 0);

  ScopedRef attr_ref;
  auto* attr_priv = reinterpret_cast<H5R_ref_priv_t*>(attr_ref.get());
  ASSERT_GE(H5R__create_attr(&tok, sizeof(haddr_t), "a", attr_priv), 0);
  ASSERT_GE(H5R__set_loc_id(attr_priv, mock_file.get(), true, true), 0);

  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());

  {
    ScopedDataset ds(H5Ropen_object_async(obj_ref.get(), H5P_DEFAULT,
                                          H5P_DEFAULT, es));
    EXPECT_TRUE(ds.valid());
    ScopedSpace sp(H5Ropen_region_async(reg_ref.get(), H5P_DEFAULT,
                                        H5P_DEFAULT, es));
    EXPECT_TRUE(sp.valid());
    ScopedAttr at(
        H5Ropen_attr_async(attr_ref.get(), H5P_DEFAULT, H5P_DEFAULT, es));
    EXPECT_TRUE(at.valid());

    size_t num_in_progress = 0;
    bool err_occurred = false;
    ASSERT_GE(H5ESwait(es, H5ES_WAIT_FOREVER, &num_in_progress, &err_occurred),
              0);
    EXPECT_FALSE(err_occurred);
    EXPECT_EQ(num_in_progress, 0);
  }
  EXPECT_GE(GetMockAsyncState().wait_calls, 3);
  EXPECT_GE(GetMockAsyncState().free_calls, 3);

  // Pass a non-eventset ID (mock_file) as es_id so H5ES_insert() fails and
  // triggers the H5I_dec_app_ref_always_close cleanup branch.
  ScopedErrorSilence silence;
  EXPECT_EQ(H5Ropen_object_async(obj_ref.get(), H5P_DEFAULT, H5P_DEFAULT,
                                 mock_file),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Ropen_region_async(reg_ref.get(), H5P_DEFAULT, H5P_DEFAULT,
                                 mock_file),
            H5I_INVALID_HID);
  EXPECT_EQ(
      H5Ropen_attr_async(attr_ref.get(), H5P_DEFAULT, H5P_DEFAULT, mock_file),
      H5I_INVALID_HID);

  // Exercise VOL callback error paths across H5Rcreate_* / H5Ropen_region /
  // H5Rget_file_name / H5Rget_obj_name.
  ScopedRef tmp_ref;
  GetMockAsyncState().fail_get_file = true;
  EXPECT_LT(
      H5Rcreate_object(mock_file, "/m", H5P_DEFAULT, tmp_ref.get()), 0);
  tmp_ref.disarm();
  EXPECT_LT(
      H5Rcreate_region(mock_file, "/m", space, H5P_DEFAULT, tmp_ref.get()), 0);
  tmp_ref.disarm();
  EXPECT_LT(
      H5Rcreate_attr(mock_file, "/m", "a", H5P_DEFAULT, tmp_ref.get()), 0);
  tmp_ref.disarm();

  GetMockAsyncState().fail_get_file = false;
  GetMockAsyncState().fail_get_cont_info = true;
  EXPECT_LT(
      H5Rcreate_object(mock_file, "/m", H5P_DEFAULT, tmp_ref.get()), 0);
  tmp_ref.disarm();
  EXPECT_LT(
      H5Rcreate_region(mock_file, "/m", space, H5P_DEFAULT, tmp_ref.get()), 0);
  tmp_ref.disarm();
  EXPECT_LT(
      H5Rcreate_attr(mock_file, "/m", "a", H5P_DEFAULT, tmp_ref.get()), 0);
  tmp_ref.disarm();
  GetMockAsyncState().fail_get_cont_info = false;

  // Verify mock create succeeds when VOL callbacks succeed.
  EXPECT_GE(H5Rcreate_object(mock_file, "/m", H5P_DEFAULT, tmp_ref.get()), 0);

  std::array<char, 16> name_buf{};
  GetMockAsyncState().fail_get_file_name = true;
  EXPECT_LT(
      H5Rget_file_name(obj_ref.cget(), name_buf.data(), name_buf.size()), 0);
  GetMockAsyncState().fail_get_file_name = false;

  GetMockAsyncState().fail_get_obj_name = true;
  EXPECT_LT(H5Rget_obj_name(obj_ref.get(), H5P_DEFAULT, name_buf.data(),
                            name_buf.size()),
            0);
  GetMockAsyncState().fail_get_obj_name = false;

  GetMockAsyncState().fail_get_space = true;
  EXPECT_EQ(H5Ropen_region(reg_ref.get(), H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  GetMockAsyncState().fail_get_space = false;

  GetMockAsyncState().bad_space_non_dataspace = true;
  EXPECT_EQ(H5Ropen_region(reg_ref.get(), H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  GetMockAsyncState().bad_space_non_dataspace = false;
}

}  // namespace
}  // namespace hdf5
