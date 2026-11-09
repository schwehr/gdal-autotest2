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
// Tests native VOL connector attribute callbacks (`H5VL__native_attr_*`),
// public H5A* operations (by self, name, idx, iteration, rename, delete,
// deprecated H5Aiterate1), and negative paths
// (third_party/hdf5/src/H5VLnative_attr.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLnative_attr.c

#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5VLNATIVE_ATTR_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5VLnative_private.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5VLNATIVE_ATTR_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5VLNATIVE_ATTR_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

class ScopedApiContext {
 public:
  ScopedApiContext() { pushed_ = (H5CX_push(&node_) >= 0); }
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

herr_t CollectAttrV2(hid_t /*loc_id*/, const char* name,
                     const H5A_info_t* /*ainfo*/, void* op_data) {
  auto* names = static_cast<std::vector<std::string>*>(op_data);
  names->emplace_back(name);
  return 0;
}

herr_t FailIterV2(hid_t /*loc_id*/, const char* /*name*/,
                  const H5A_info_t* /*ainfo*/, void* /*op_data*/) {
  return -1;
}

herr_t CollectAttrV1(hid_t /*loc_id*/, const char* name, void* op_data) {
  auto* names = static_cast<std::vector<std::string>*>(op_data);
  names->emplace_back(name);
  return 0;
}

herr_t FailIterV1(hid_t /*loc_id*/, const char* /*name*/, void* /*op_data*/) {
  return -1;
}

TEST(H5VLNativeAttrTest, PublicLifecycleAndAllLocationModes) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // Committed datatype exercises H5T_get_actual_type on named dtype in create.
  ScopedType committed_dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(committed_dt.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/committed_i32", committed_dt.get(),
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // H5Acreate2, H5Awrite, H5Aread, H5Aget_{space,type,create_plist,name,info,storage_size}.
  {
    ScopedAttr a1(H5Acreate2(grp.get(), "alpha_attr", committed_dt.get(),
                             scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a1.valid());
    const int32_t wval = 42;
    EXPECT_GE(H5Awrite(a1.get(), H5T_NATIVE_INT32, &wval), 0);
    int32_t rval = 0;
    EXPECT_GE(H5Aread(a1.get(), H5T_NATIVE_INT32, &rval), 0);
    EXPECT_EQ(rval, 42);

    ScopedSpace got_space(H5Aget_space(a1.get()));
    EXPECT_TRUE(got_space.valid());
    ScopedType got_type(H5Aget_type(a1.get()));
    EXPECT_TRUE(got_type.valid());
    ScopedPlist got_acpl(H5Aget_create_plist(a1.get()));
    EXPECT_TRUE(got_acpl.valid());
    EXPECT_EQ(H5Aget_storage_size(a1.get()), sizeof(int32_t));

    std::string self_name(32, '\0');
    const ssize_t nlen =
        H5Aget_name(a1.get(), self_name.size(), self_name.data());
    ASSERT_GT(nlen, 0);
    self_name.resize(static_cast<size_t>(nlen));
    EXPECT_EQ(self_name, "alpha_attr");

    H5A_info_t info{};
    EXPECT_GE(H5Aget_info(a1.get(), &info), 0);
    EXPECT_EQ(info.data_size, sizeof(int32_t));
  }

  // H5Acreate_by_name, H5Aopen, H5Aopen_by_name, H5Aopen_by_idx.
  {
    ScopedAttr a2(H5Acreate_by_name(file.get(), "/grp", "beta_long_attribute",
                                    H5T_NATIVE_INT32, scalar.get(), H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a2.valid());
    const int32_t wval = 7;
    EXPECT_GE(H5Awrite(a2.get(), H5T_NATIVE_INT32, &wval), 0);
  }
  {
    ScopedAttr by_self(H5Aopen(grp.get(), "alpha_attr", H5P_DEFAULT));
    EXPECT_TRUE(by_self.valid());
    ScopedAttr by_name(H5Aopen_by_name(file.get(), "/grp", "beta_long_attribute",
                                       H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_TRUE(by_name.valid());
    ScopedAttr by_idx(H5Aopen_by_idx(file.get(), "/grp", H5_INDEX_NAME,
                                     H5_ITER_INC, 0, H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_TRUE(by_idx.valid());
  }

  // H5Aget_name_by_idx: query length (nullptr), full copy, truncated copy.
  EXPECT_EQ(H5Aget_name_by_idx(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                               1, nullptr, 0, H5P_DEFAULT),
            19);
  std::string full_name(32, '\0');
  EXPECT_EQ(H5Aget_name_by_idx(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                               1, full_name.data(), full_name.size(),
                               H5P_DEFAULT),
            19);
  EXPECT_STREQ(full_name.c_str(), "beta_long_attribute");

  std::string trunc_name(5, 'x');
  EXPECT_EQ(H5Aget_name_by_idx(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                               1, trunc_name.data(), trunc_name.size(),
                               H5P_DEFAULT),
            19);
  EXPECT_STREQ(trunc_name.c_str(), "beta");

  // H5Aget_info_by_name, H5Aget_info_by_idx.
  H5A_info_t info_by_name{};
  EXPECT_GE(H5Aget_info_by_name(file.get(), "/grp", "beta_long_attribute",
                                &info_by_name, H5P_DEFAULT),
            0);
  EXPECT_EQ(info_by_name.data_size, sizeof(int32_t));
  H5A_info_t info_by_idx{};
  EXPECT_GE(H5Aget_info_by_idx(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                               0, &info_by_idx, H5P_DEFAULT),
            0);

  // H5Aexists, H5Aexists_by_name, H5Aiterate2, H5Aiterate_by_name, H5Aiterate1.
  EXPECT_GT(H5Aexists(grp.get(), "alpha_attr"), 0);
  EXPECT_EQ(H5Aexists_by_name(file.get(), "/grp", "missing", H5P_DEFAULT), 0);

  hsize_t idx = 0;
  std::vector<std::string> names_v2;
  EXPECT_GE(H5Aiterate2(grp.get(), H5_INDEX_NAME, H5_ITER_INC, &idx,
                        CollectAttrV2, &names_v2),
            0);
  EXPECT_THAT(names_v2, ElementsAre("alpha_attr", "beta_long_attribute"));

  idx = 0;
  names_v2.clear();
  EXPECT_GE(H5Aiterate_by_name(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                               &idx, CollectAttrV2, &names_v2, H5P_DEFAULT),
            0);
  EXPECT_THAT(names_v2, ElementsAre("alpha_attr", "beta_long_attribute"));

  // Deprecated H5Aiterate1 covers H5VL__native_attr_optional.
  unsigned old_idx = 0;
  std::vector<std::string> names_v1;
  EXPECT_GE(H5Aiterate1(grp.get(), &old_idx, CollectAttrV1, &names_v1), 0);
  EXPECT_THAT(names_v1, ElementsAre("alpha_attr", "beta_long_attribute"));
  {
    ScopedErrorSilence silence;
    old_idx = 0;
    EXPECT_LT(H5Aiterate1(grp.get(), &old_idx, FailIterV1, nullptr), 0);
    idx = 0;
    EXPECT_LT(H5Aiterate2(grp.get(), H5_INDEX_NAME, H5_ITER_INC, &idx,
                          FailIterV2, nullptr),
              0);
  }

  // H5Arename, H5Arename_by_name, H5Adelete, H5Adelete_by_name, H5Adelete_by_idx.
  EXPECT_GE(H5Arename(grp.get(), "alpha_attr", "alpha_renamed"), 0);
  EXPECT_GE(H5Arename_by_name(file.get(), "/grp", "alpha_renamed",
                              "alpha_final", H5P_DEFAULT),
            0);
  EXPECT_GE(H5Adelete(grp.get(), "alpha_final"), 0);
  EXPECT_GE(H5Adelete_by_idx(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  {
    ScopedAttr a3(H5Acreate2(grp.get(), "to_del_by_name", H5T_NATIVE_INT32,
                             scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a3.valid());
  }
  EXPECT_GE(
      H5Adelete_by_name(file.get(), "/grp", "to_del_by_name", H5P_DEFAULT), 0);
}

TEST(H5VLNativeAttrTest, DirectCallbacksAndErrorPaths) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile rw_file(H5Fcreate("attr_direct_rw.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                               fapl.get()));
  ASSERT_TRUE(rw_file.valid());
  void* file_obj = H5VL_object(rw_file.get());
  ASSERT_NE(file_obj, nullptr);

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedType str_type(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str_type.valid());
  ASSERT_GE(H5Tset_size(str_type.get(), 8), 0);

  H5VL_loc_params_t self_loc{};
  self_loc.type = H5VL_OBJECT_BY_SELF;
  self_loc.obj_type = H5I_FILE;

  H5VL_loc_params_t name_loc{};
  name_loc.type = H5VL_OBJECT_BY_NAME;
  name_loc.obj_type = H5I_FILE;
  name_loc.loc_data.loc_by_name.name = ".";
  name_loc.loc_data.loc_by_name.lapl_id = H5P_LINK_ACCESS_DEFAULT;

  H5VL_loc_params_t idx_loc{};
  idx_loc.type = H5VL_OBJECT_BY_IDX;
  idx_loc.obj_type = H5I_FILE;
  idx_loc.loc_data.loc_by_idx.name = ".";
  idx_loc.loc_data.loc_by_idx.idx_type = H5_INDEX_NAME;
  idx_loc.loc_data.loc_by_idx.order = H5_ITER_INC;
  idx_loc.loc_data.loc_by_idx.n = 0;
  idx_loc.loc_data.loc_by_idx.lapl_id = H5P_LINK_ACCESS_DEFAULT;

  H5VL_loc_params_t bad_loc{};
  bad_loc.obj_type = H5I_BADID;
  bad_loc.type = H5VL_OBJECT_BY_TOKEN;

  ScopedApiContext api_ctx;

  // Create and exercise read/write/get/specific/close directly.
  void* attr_obj = H5VL__native_attr_create(
      file_obj, &self_loc, "direct_str", str_type.get(), scalar.get(),
      H5P_ATTRIBUTE_CREATE_DEFAULT, H5P_ATTRIBUTE_ACCESS_DEFAULT,
      H5P_DATASET_XFER_DEFAULT, nullptr);
  ASSERT_NE(attr_obj, nullptr);

  {
    ScopedErrorSilence silence;
    // Duplicate name -> H5A__create fails.
    EXPECT_EQ(H5VL__native_attr_create(
                  file_obj, &self_loc, "direct_str", str_type.get(),
                  scalar.get(), H5P_ATTRIBUTE_CREATE_DEFAULT,
                  H5P_ATTRIBUTE_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT,
                  nullptr),
              nullptr);
    // Missing target object -> H5A__create_by_name fails.
    H5VL_loc_params_t missing_name_loc = name_loc;
    missing_name_loc.loc_data.loc_by_name.name = "/no_such_obj";
    EXPECT_EQ(H5VL__native_attr_create(
                  file_obj, &missing_name_loc, "x", str_type.get(), scalar.get(),
                  H5P_ATTRIBUTE_CREATE_DEFAULT, H5P_ATTRIBUTE_ACCESS_DEFAULT,
                  H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    // Unsupported loc_params->type in create.
    EXPECT_EQ(H5VL__native_attr_create(
                  file_obj, &idx_loc, "x", str_type.get(), scalar.get(),
                  H5P_ATTRIBUTE_CREATE_DEFAULT, H5P_ATTRIBUTE_ACCESS_DEFAULT,
                  H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    // Bad args in create.
    EXPECT_EQ(H5VL__native_attr_create(
                  file_obj, &bad_loc, "x", str_type.get(), scalar.get(),
                  H5P_ATTRIBUTE_CREATE_DEFAULT, H5P_ATTRIBUTE_ACCESS_DEFAULT,
                  H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_attr_create(
                  file_obj, &self_loc, "x", str_type.get(), scalar.get(),
                  H5P_ATTRIBUTE_CREATE_DEFAULT, H5P_FILE_ACCESS_DEFAULT,
                  H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_attr_create(
                  file_obj, &self_loc, "x", H5I_INVALID_HID, scalar.get(),
                  H5P_ATTRIBUTE_CREATE_DEFAULT, H5P_ATTRIBUTE_ACCESS_DEFAULT,
                  H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_attr_create(
                  file_obj, &self_loc, "x", str_type.get(), H5I_INVALID_HID,
                  H5P_ATTRIBUTE_CREATE_DEFAULT, H5P_ATTRIBUTE_ACCESS_DEFAULT,
                  H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);

    // Invalid read/write datatype ID & incompatible conversion (OPAQUE <-> INT).
    char buf[16] = "abc";
    EXPECT_LT(H5VL__native_attr_write(attr_obj, H5I_INVALID_HID, buf,
                                      H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_LT(H5VL__native_attr_read(attr_obj, H5I_INVALID_HID, buf,
                                     H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    ScopedType opaque_type(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opaque_type.valid());
    void* opaque_attr = H5VL__native_attr_create(
        file_obj, &self_loc, "direct_opaque", opaque_type.get(), scalar.get(),
        H5P_ATTRIBUTE_CREATE_DEFAULT, H5P_ATTRIBUTE_ACCESS_DEFAULT,
        H5P_DATASET_XFER_DEFAULT, nullptr);
    ASSERT_NE(opaque_attr, nullptr);
    const std::array<uint8_t, 4> op_bytes = {1, 2, 3, 4};
    ASSERT_GE(
        H5VL__native_attr_write(opaque_attr, opaque_type.get(), op_bytes.data(),
                                 H5P_DATASET_XFER_DEFAULT, nullptr),
        0);
    int bad_int = 0;
    EXPECT_LT(H5VL__native_attr_write(opaque_attr, H5T_NATIVE_INT, &bad_int,
                                      H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_LT(H5VL__native_attr_read(opaque_attr, H5T_NATIVE_INT, &bad_int,
                                     H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_GE(H5VL__native_attr_close(opaque_attr, H5P_DATASET_XFER_DEFAULT,
                                      nullptr),
              0);
  }

  ASSERT_GE(
      H5VL__native_attr_close(attr_obj, H5P_DATASET_XFER_DEFAULT, nullptr), 0);

  // Read-only attribute test for no-write-intent guard in create.
  ScopedType ro_check(H5Tcopy(H5T_NATIVE_INT));
  TempFile ro_tmp("ro_attr_base.h5");
  {
    ScopedFile rw_init(
        H5Fcreate(ro_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(rw_init.valid());
  }
  ScopedFile ro_file(H5Fopen(ro_tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(ro_file.valid());
  void* ro_file_obj = H5VL_object(ro_file.get());
  ASSERT_NE(ro_file_obj, nullptr);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5VL__native_attr_create(
                  ro_file_obj, &self_loc, "ro_attr", ro_check.get(),
                  scalar.get(), H5P_ATTRIBUTE_CREATE_DEFAULT,
                  H5P_ATTRIBUTE_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT,
                  nullptr),
              nullptr);
  }

  // Open failures across all branches.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5VL__native_attr_open(file_obj, &bad_loc, "a",
                                    H5P_ATTRIBUTE_ACCESS_DEFAULT,
                                    H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_attr_open(file_obj, &self_loc, "a",
                                    H5P_FILE_ACCESS_DEFAULT,
                                    H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_attr_open(file_obj, &self_loc, "nonexistent",
                                    H5P_ATTRIBUTE_ACCESS_DEFAULT,
                                    H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_attr_open(file_obj, &name_loc, "nonexistent",
                                    H5P_ATTRIBUTE_ACCESS_DEFAULT,
                                    H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    idx_loc.loc_data.loc_by_idx.n = 999;
    EXPECT_EQ(H5VL__native_attr_open(file_obj, &idx_loc, nullptr,
                                    H5P_ATTRIBUTE_ACCESS_DEFAULT,
                                    H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    H5VL_loc_params_t tok_loc = self_loc;
    tok_loc.type = H5VL_OBJECT_BY_TOKEN;
    EXPECT_EQ(H5VL__native_attr_open(file_obj, &tok_loc, "a",
                                    H5P_ATTRIBUTE_ACCESS_DEFAULT,
                                    H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
  }

  // Get/Specific/Optional error branches.
  {
    ScopedErrorSilence silence;
    H5VL_attr_get_args_t gargs{};
    size_t out_len = 0;
    H5A_info_t out_info{};

    gargs.op_type = H5VL_ATTR_GET_NAME;
    gargs.args.get_name.loc_params = idx_loc;
    gargs.args.get_name.loc_params.obj_type = H5I_BADID;
    gargs.args.get_name.attr_name_len = &out_len;
    EXPECT_LT(H5VL__native_attr_get(file_obj, &gargs, H5P_DATASET_XFER_DEFAULT,
                                    nullptr),
              0);

    idx_loc.loc_data.loc_by_idx.n = 999;
    gargs.args.get_name.loc_params = idx_loc;
    EXPECT_LT(H5VL__native_attr_get(file_obj, &gargs, H5P_DATASET_XFER_DEFAULT,
                                    nullptr),
              0);

    gargs.args.get_name.loc_params = name_loc;
    EXPECT_LT(H5VL__native_attr_get(file_obj, &gargs, H5P_DATASET_XFER_DEFAULT,
                                    nullptr),
              0);

    gargs = {};
    gargs.op_type = H5VL_ATTR_GET_INFO;
    gargs.args.get_info.ainfo = &out_info;
    gargs.args.get_info.attr_name = "missing";
    gargs.args.get_info.loc_params = name_loc;
    gargs.args.get_info.loc_params.obj_type = H5I_BADID;
    EXPECT_LT(H5VL__native_attr_get(file_obj, &gargs, H5P_DATASET_XFER_DEFAULT,
                                    nullptr),
              0);

    gargs.args.get_info.loc_params = name_loc;
    EXPECT_LT(H5VL__native_attr_get(file_obj, &gargs, H5P_DATASET_XFER_DEFAULT,
                                    nullptr),
              0);

    gargs.args.get_info.loc_params = idx_loc;
    gargs.args.get_info.loc_params.obj_type = H5I_BADID;
    EXPECT_LT(H5VL__native_attr_get(file_obj, &gargs, H5P_DATASET_XFER_DEFAULT,
                                    nullptr),
              0);

    gargs.args.get_info.loc_params = idx_loc;
    EXPECT_LT(H5VL__native_attr_get(file_obj, &gargs, H5P_DATASET_XFER_DEFAULT,
                                    nullptr),
              0);

    gargs.args.get_info.loc_params.type = H5VL_OBJECT_BY_TOKEN;
    gargs.args.get_info.loc_params.obj_type = H5I_FILE;
    EXPECT_LT(H5VL__native_attr_get(file_obj, &gargs, H5P_DATASET_XFER_DEFAULT,
                                    nullptr),
              0);

    gargs = {};
    gargs.op_type = static_cast<H5VL_attr_get_t>(999);
    EXPECT_LT(H5VL__native_attr_get(file_obj, &gargs, H5P_DATASET_XFER_DEFAULT,
                                    nullptr),
              0);

    // Specific error paths.
    H5VL_attr_specific_args_t sargs{};
    sargs.op_type = H5VL_ATTR_DELETE;
    sargs.args.del.name = "missing";
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &bad_loc, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &self_loc, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &name_loc, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &idx_loc, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    sargs = {};
    sargs.op_type = H5VL_ATTR_DELETE_BY_IDX;
    sargs.args.delete_by_idx.idx_type = H5_INDEX_NAME;
    sargs.args.delete_by_idx.order = H5_ITER_INC;
    sargs.args.delete_by_idx.n = 999;
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &name_loc, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &self_loc, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    bool exists_out = false;
    sargs = {};
    sargs.op_type = H5VL_ATTR_EXISTS;
    sargs.args.exists.name = "missing";
    sargs.args.exists.exists = &exists_out;
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &idx_loc, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    H5VL_loc_params_t missing_obj = name_loc;
    missing_obj.loc_data.loc_by_name.name = "/no_such_obj";
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &missing_obj, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    sargs = {};
    sargs.op_type = H5VL_ATTR_ITER;
    sargs.args.iterate.idx_type = H5_INDEX_NAME;
    sargs.args.iterate.order = H5_ITER_INC;
    sargs.args.iterate.op = CollectAttrV2;
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &idx_loc, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    sargs = {};
    sargs.op_type = H5VL_ATTR_RENAME;
    sargs.args.rename.old_name = "missing_a";
    sargs.args.rename.new_name = "missing_b";
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &self_loc, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &name_loc, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &idx_loc, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    sargs = {};
    sargs.op_type = static_cast<H5VL_attr_specific_t>(999);
    EXPECT_LT(H5VL__native_attr_specific(file_obj, &self_loc, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    // Optional invalid op_type.
    H5VL_optional_args_t opt_args{};
    opt_args.op_type = 999;
    opt_args.args = nullptr;
    EXPECT_LT(H5VL__native_attr_optional(file_obj, &opt_args,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
  }
}

}  // namespace
}  // namespace hdf5
