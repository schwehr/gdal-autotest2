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
// Tests deprecated HDF5 reference APIs: H5Rcreate, H5Rget_obj_type1/2,
// H5Rdereference1/2, H5Rget_region, and H5Rget_name
// (third_party/hdf5/src/H5Rdeprec.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Rdeprec.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// Minimal non-native stub VOL connector to verify that H5Rdeprec routines
// reject non-native VOL objects cleanly and handle VOL callback errors.
extern const H5VL_class_t kNonNativeVolClass;

struct StubVolState {
  int dummy_file_handle = 1;
  bool fail_term_conn_cls = false;
  bool fail_get_file = false;
  bool fail_get_cont_info = false;
  bool fail_get_name = false;
  size_t cont_token_size = sizeof(haddr_t);
};

StubVolState& GetStubVolState() {
  static StubVolState state;
  return state;
}

void* StubVolFileCreate(const char* /*name*/, unsigned /*flags*/,
                        hid_t /*fcpl_id*/, hid_t /*fapl_id*/,
                        hid_t /*dxpl_id*/, void** /*req*/) {
  return &GetStubVolState().dummy_file_handle;
}

herr_t StubVolFileGet(void* /*obj*/, H5VL_file_get_args_t* args,
                      hid_t /*dxpl_id*/, void** /*req*/) {
  const auto& state = GetStubVolState();
  if (args->op_type == H5VL_FILE_GET_CONT_INFO) {
    if (state.fail_get_cont_info) {
      return -1;
    }
    args->args.get_cont_info.info->version = H5VL_CONTAINER_INFO_VERSION;
    args->args.get_cont_info.info->feature_flags = 0;
    args->args.get_cont_info.info->token_size = state.cont_token_size;
    args->args.get_cont_info.info->blob_id_size = 0;
    return 0;
  }
  return -1;
}

herr_t StubVolFileClose(void* /*file*/, hid_t /*dxpl_id*/, void** /*req*/) {
  return 0;
}

herr_t StubVolObjectGet(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                        H5VL_object_get_args_t* args, hid_t /*dxpl_id*/,
                        void** /*req*/) {
  auto& state = GetStubVolState();
  if (args->op_type == H5VL_OBJECT_GET_FILE) {
    if (state.fail_get_file) {
      return -1;
    }
    *args->args.get_file.file = &state.dummy_file_handle;
    return 0;
  }
  if (args->op_type == H5VL_OBJECT_GET_NAME) {
    if (state.fail_get_name) {
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

herr_t StubVolGetConnCls(void* /*obj*/, H5VL_get_conn_lvl_t lvl,
                         const H5VL_class_t** conn_cls) {
  if (lvl == H5VL_GET_CONN_LVL_TERM && GetStubVolState().fail_term_conn_cls) {
    return -1;
  }
  *conn_cls = &kNonNativeVolClass;
  return 0;
}

herr_t StubVolGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  *cap_flags = H5VL_CAP_FLAG_NONE;
  return 0;
}

herr_t StubVolOptQuery(void* /*obj*/, H5VL_subclass_t /*cls*/,
                       int /*opt_type*/, uint64_t* flags) {
  if (flags != nullptr) {
    *flags = 0;
  }
  return 0;
}

const H5VL_class_t kNonNativeVolClass = {
    .version = H5VL_VERSION,
    .value = static_cast<H5VL_class_value_t>(5099),
    .name = "h5rdeprec_test_non_native_vol",
    .conn_version = 1,
    .cap_flags = H5VL_CAP_FLAG_NONE,
    .initialize = nullptr,
    .terminate = nullptr,
    .info_cls = {},
    .wrap_cls = {},
    .attr_cls = {},
    .dataset_cls = {},
    .datatype_cls = {},
    .file_cls =
        {
            .create = StubVolFileCreate,
            .get = StubVolFileGet,
            .close = StubVolFileClose,
        },
    .group_cls = {},
    .link_cls = {},
    .object_cls =
        {
            .get = StubVolObjectGet,
        },
    .introspect_cls =
        {
            .get_conn_cls = StubVolGetConnCls,
            .get_cap_flags = StubVolGetCapFlags,
            .opt_query = StubVolOptQuery,
        },
    .request_cls = {},
    .blob_cls = {},
    .token_cls = {},
    .optional = nullptr,
};

TEST(H5RdeprecTest, CreateObjectAndRegionInvalidArgumentsFail) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  constexpr hsize_t kDim[1] = {4};
  ScopedSpace space(H5Screate_simple(1, kDim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file, "/ds", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  hobj_ref_t obj_ref = 0;
  hdset_reg_ref_t reg_ref{};

  ScopedErrorSilence silence;
  EXPECT_LT(H5Rcreate(nullptr, file, "/ds", H5R_OBJECT, H5I_INVALID_HID), 0);
  EXPECT_LT(H5Rcreate(&obj_ref, file, nullptr, H5R_OBJECT, H5I_INVALID_HID), 0);
  EXPECT_LT(H5Rcreate(&obj_ref, file, "", H5R_OBJECT, H5I_INVALID_HID), 0);
  EXPECT_LT(H5Rcreate(&obj_ref, file, "/ds", H5R_BADTYPE, H5I_INVALID_HID), 0);
  EXPECT_LT(H5Rcreate(&obj_ref, file, "/ds", H5R_OBJECT2, H5I_INVALID_HID), 0);
  EXPECT_LT(H5Rcreate(&obj_ref, file, "/ds", H5R_ATTR, H5I_INVALID_HID), 0);
  EXPECT_LT(H5Rcreate(&obj_ref, file, "/ds", H5R_MAXTYPE, H5I_INVALID_HID), 0);
  EXPECT_LT(H5Rcreate(&obj_ref, H5I_INVALID_HID, "/ds", H5R_OBJECT,
                      H5I_INVALID_HID),
            0);
  EXPECT_LT(H5Rcreate(&obj_ref, scalar, "/ds", H5R_OBJECT, H5I_INVALID_HID), 0);
  EXPECT_LT(H5Rcreate(&obj_ref, file, "/missing", H5R_OBJECT, H5I_INVALID_HID),
            0);

  // Region creation requires a valid dataspace identifier.
  EXPECT_LT(
      H5Rcreate(&reg_ref, file, "/ds", H5R_DATASET_REGION, H5I_INVALID_HID), 0);
  EXPECT_LT(H5Rcreate(&reg_ref, file, "/ds", H5R_DATASET_REGION, file), 0);
}

TEST(H5RdeprecTest, ObjectReferenceCreateInspectDereferenceAndName) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(H5Gcreate2(file, "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  constexpr hsize_t kDim[1] = {4};
  ScopedSpace space(H5Screate_simple(1, kDim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(grp, "dset", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  constexpr std::array<int32_t, 4> kValues = {5, 6, 7, 8};
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     kValues.data()),
            0);

  ScopedType dtype(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(dtype.valid());
  ASSERT_GE(H5Tcommit2(grp, "dtype", dtype, H5P_DEFAULT, H5P_DEFAULT,
                       H5P_DEFAULT),
            0);

  hobj_ref_t grp_ref = 0;
  hobj_ref_t dset_ref = 0;
  hobj_ref_t dtype_ref = 0;
  ASSERT_GE(H5Rcreate(&grp_ref, file, "/grp", H5R_OBJECT, H5I_INVALID_HID), 0);
  ASSERT_GE(H5Rcreate(&dset_ref, grp, "dset", H5R_OBJECT, H5I_INVALID_HID), 0);
  ASSERT_GE(H5Rcreate(&dtype_ref, file, "/grp/dtype", H5R_OBJECT,
                      H5I_INVALID_HID),
            0);

  // Deprecated v1 obj_type1.
  EXPECT_EQ(H5Rget_obj_type1(file, H5R_OBJECT, &grp_ref), H5G_GROUP);
  EXPECT_EQ(H5Rget_obj_type1(file, H5R_OBJECT, &dset_ref), H5G_DATASET);
  EXPECT_EQ(H5Rget_obj_type1(file, H5R_OBJECT, &dtype_ref), H5G_TYPE);

  // Deprecated v2 obj_type2.
  H5O_type_t otype = H5O_TYPE_UNKNOWN;
  ASSERT_GE(H5Rget_obj_type2(file, H5R_OBJECT, &grp_ref, &otype), 0);
  EXPECT_EQ(otype, H5O_TYPE_GROUP);
  ASSERT_GE(H5Rget_obj_type2(file, H5R_OBJECT, &dset_ref, &otype), 0);
  EXPECT_EQ(otype, H5O_TYPE_DATASET);
  ASSERT_GE(H5Rget_obj_type2(file, H5R_OBJECT, &dtype_ref, &otype), 0);
  EXPECT_EQ(otype, H5O_TYPE_NAMED_DATATYPE);

  // Dereference v1 and v2.
  ScopedGroup opened_grp1(H5Rdereference1(file, H5R_OBJECT, &grp_ref));
  ASSERT_TRUE(opened_grp1.valid());
  ScopedDataset opened_dset1(H5Rdereference1(grp, H5R_OBJECT, &dset_ref));
  ASSERT_TRUE(opened_dset1.valid());
  ScopedType opened_dtype1(H5Rdereference1(file, H5R_OBJECT, &dtype_ref));
  ASSERT_TRUE(opened_dtype1.valid());

  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl.valid());
  ScopedDataset opened_dset2(
      H5Rdereference2(file, dapl, H5R_OBJECT, &dset_ref));
  ASSERT_TRUE(opened_dset2.valid());
  std::array<int32_t, 4> read_vals = {-1, -1, -1, -1};
  ASSERT_GE(H5Dread(opened_dset2, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, read_vals.data()),
            0);
  EXPECT_THAT(read_vals, ElementsAre(5, 6, 7, 8));

  // Query object name (length, zero-size non-clobber, truncated, full).
  EXPECT_EQ(H5Rget_name(file, H5R_OBJECT, &dset_ref, nullptr, 0), 9);
  std::array<char, 16> name_buf{};
  name_buf.fill('#');
  EXPECT_EQ(H5Rget_name(file, H5R_OBJECT, &dset_ref, name_buf.data(), 0), 9);
  EXPECT_EQ(name_buf[0], '#');
  EXPECT_EQ(H5Rget_name(file, H5R_OBJECT, &dset_ref, name_buf.data(), 5), 9);
  EXPECT_STREQ(name_buf.data(), "/grp");
  EXPECT_EQ(H5Rget_name(file, H5R_OBJECT, &dset_ref, name_buf.data(),
                        name_buf.size()),
            9);
  EXPECT_STREQ(name_buf.data(), "/grp/dset");

  // Also test storing and reading back via H5T_STD_REF_OBJ.
  constexpr hsize_t kOne[1] = {1};
  ScopedSpace ref_space(H5Screate_simple(1, kOne, nullptr));
  ASSERT_TRUE(ref_space.valid());
  ScopedDataset ref_dset(H5Dcreate2(file, "/obj_refs", H5T_STD_REF_OBJ,
                                    ref_space, H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
  ASSERT_TRUE(ref_dset.valid());
  ASSERT_GE(H5Dwrite(ref_dset, H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     &dset_ref),
            0);
  hobj_ref_t roundtrip_ref = 0;
  ASSERT_GE(H5Dread(ref_dset, H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    &roundtrip_ref),
            0);
  EXPECT_EQ(H5Rget_obj_type1(ref_dset, H5R_OBJECT, &roundtrip_ref),
            H5G_DATASET);
}

TEST(H5RdeprecTest, RegionReferenceCreateGetRegionDereferenceAndName) {
  // Exercise both earliest and latest libver bounds for H5CX_set_libver_bounds
  // inside H5R__encode_token_region_compat().
  struct LibverPair {
    H5F_libver_t low;
    H5F_libver_t high;
  };
  for (const LibverPair bounds :
       {LibverPair{H5F_LIBVER_EARLIEST, H5F_LIBVER_V18},
        LibverPair{H5F_LIBVER_LATEST, H5F_LIBVER_LATEST}}) {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl, bounds.low, bounds.high), 0);
    ScopedFile file(H5Fcreate("reg_deprec.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());

    constexpr hsize_t kDims[2] = {3, 4};
    ScopedSpace space(H5Screate_simple(2, kDims, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file, "/mat", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::array<int32_t, 12> vals{};
    for (int i = 0; i < 12; ++i) {
      vals[i] = i * 10;
    }
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       vals.data()),
              0);

    ScopedSpace sel_space(H5Scopy(space));
    ASSERT_TRUE(sel_space.valid());
    constexpr hsize_t kCoords[4] = {0, 1, 2, 3};
    ASSERT_GE(H5Sselect_elements(sel_space, H5S_SELECT_SET, 2, kCoords), 0);

    hdset_reg_ref_t reg_ref{};
    ASSERT_GE(
        H5Rcreate(&reg_ref, file, "/mat", H5R_DATASET_REGION, sel_space), 0);

    EXPECT_EQ(H5Rget_obj_type1(file, H5R_DATASET_REGION, &reg_ref),
              H5G_DATASET);
    H5O_type_t otype = H5O_TYPE_UNKNOWN;
    ASSERT_GE(H5Rget_obj_type2(file, H5R_DATASET_REGION, &reg_ref, &otype), 0);
    EXPECT_EQ(otype, H5O_TYPE_DATASET);

    std::array<char, 16> name_buf{};
    EXPECT_EQ(H5Rget_name(file, H5R_DATASET_REGION, &reg_ref, name_buf.data(),
                          name_buf.size()),
              4);
    EXPECT_STREQ(name_buf.data(), "/mat");

    ScopedSpace got_space(H5Rget_region(file, H5R_DATASET_REGION, &reg_ref));
    ASSERT_TRUE(got_space.valid());
    EXPECT_EQ(H5Sget_select_type(got_space), H5S_SEL_POINTS);
    EXPECT_EQ(H5Sget_select_npoints(got_space), 2);

    ScopedDataset opened1(
        H5Rdereference1(file, H5R_DATASET_REGION, &reg_ref));
    ASSERT_TRUE(opened1.valid());
    ScopedDataset opened2(H5Rdereference2(file, H5P_DEFAULT,
                                          H5R_DATASET_REGION, &reg_ref));
    ASSERT_TRUE(opened2.valid());

    constexpr hsize_t kMemDim[1] = {2};
    ScopedSpace mem_space(H5Screate_simple(1, kMemDim, nullptr));
    ASSERT_TRUE(mem_space.valid());
    std::array<int32_t, 2> out_vals = {-1, -1};
    ASSERT_GE(H5Dread(opened2, H5T_NATIVE_INT32, mem_space, got_space,
                      H5P_DEFAULT, out_vals.data()),
              0);
    EXPECT_THAT(out_vals, ElementsAre(10, 110));
  }
}

TEST(H5RdeprecTest, QueryDereferenceAndRegionInvalidArgsAndCorruptRefsFail) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  constexpr hsize_t kDim[1] = {2};
  ScopedSpace space(H5Screate_simple(1, kDim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file, "/d", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  hobj_ref_t obj_ref = 0;
  ASSERT_GE(H5Rcreate(&obj_ref, file, "/d", H5R_OBJECT, H5I_INVALID_HID), 0);

  hobj_ref_t bogus_obj_ref = static_cast<hobj_ref_t>(0x1234567ULL);
  hdset_reg_ref_t zero_reg_ref{};
  std::array<char, 16> name_buf{};
  H5O_type_t otype = H5O_TYPE_UNKNOWN;

  ScopedErrorSilence silence;

  // H5Rget_obj_type1 failures.
  EXPECT_EQ(H5Rget_obj_type1(file, H5R_OBJECT, nullptr), H5G_UNKNOWN);
  EXPECT_EQ(H5Rget_obj_type1(file, H5R_OBJECT2, &obj_ref), H5G_UNKNOWN);
  EXPECT_EQ(H5Rget_obj_type1(file, H5R_ATTR, &obj_ref), H5G_UNKNOWN);
  EXPECT_EQ(H5Rget_obj_type1(H5I_INVALID_HID, H5R_OBJECT, &obj_ref),
            H5G_UNKNOWN);
  EXPECT_EQ(H5Rget_obj_type1(scalar, H5R_OBJECT, &obj_ref), H5G_UNKNOWN);
  EXPECT_EQ(H5Rget_obj_type1(file, H5R_DATASET_REGION, &zero_reg_ref),
            H5G_UNKNOWN);
  EXPECT_EQ(H5Rget_obj_type1(file, H5R_OBJECT, &bogus_obj_ref), H5G_UNKNOWN);

  // H5Rdereference1 failures.
  EXPECT_EQ(H5Rdereference1(file, H5R_OBJECT, nullptr), H5I_INVALID_HID);
  EXPECT_EQ(H5Rdereference1(file, H5R_OBJECT2, &obj_ref), H5I_INVALID_HID);
  EXPECT_EQ(H5Rdereference1(H5I_INVALID_HID, H5R_OBJECT, &obj_ref),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Rdereference1(scalar, H5R_OBJECT, &obj_ref), H5I_INVALID_HID);
  EXPECT_EQ(H5Rdereference1(file, H5R_DATASET_REGION, &zero_reg_ref),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Rdereference1(file, H5R_OBJECT, &bogus_obj_ref), H5I_INVALID_HID);

  // H5Rget_obj_type2 failures.
  EXPECT_LT(H5Rget_obj_type2(file, H5R_OBJECT, nullptr, &otype), 0);
  EXPECT_LT(H5Rget_obj_type2(file, H5R_OBJECT2, &obj_ref, &otype), 0);
  EXPECT_LT(H5Rget_obj_type2(H5I_INVALID_HID, H5R_OBJECT, &obj_ref, &otype), 0);
  EXPECT_LT(H5Rget_obj_type2(scalar, H5R_OBJECT, &obj_ref, &otype), 0);
  EXPECT_LT(
      H5Rget_obj_type2(file, H5R_DATASET_REGION, &zero_reg_ref, &otype), 0);
  EXPECT_LT(H5Rget_obj_type2(file, H5R_OBJECT, &bogus_obj_ref, &otype), 0);

  // H5Rdereference2 failures.
  EXPECT_EQ(H5Rdereference2(file, H5I_INVALID_HID, H5R_OBJECT, &obj_ref),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Rdereference2(file, H5P_DEFAULT, H5R_OBJECT, nullptr),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Rdereference2(file, H5P_DEFAULT, H5R_OBJECT2, &obj_ref),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Rdereference2(file, fcpl, H5R_OBJECT, &obj_ref), H5I_INVALID_HID);
  EXPECT_EQ(
      H5Rdereference2(H5I_INVALID_HID, H5P_DEFAULT, H5R_OBJECT, &obj_ref),
      H5I_INVALID_HID);
  EXPECT_EQ(H5Rdereference2(scalar, H5P_DEFAULT, H5R_OBJECT, &obj_ref),
            H5I_INVALID_HID);
  EXPECT_EQ(
      H5Rdereference2(file, H5P_DEFAULT, H5R_DATASET_REGION, &zero_reg_ref),
      H5I_INVALID_HID);
  EXPECT_EQ(H5Rdereference2(file, H5P_DEFAULT, H5R_OBJECT, &bogus_obj_ref),
            H5I_INVALID_HID);

  // H5Rget_region failures.
  EXPECT_EQ(H5Rget_region(file, H5R_DATASET_REGION, nullptr), H5I_INVALID_HID);
  EXPECT_EQ(H5Rget_region(file, H5R_OBJECT, &obj_ref), H5I_INVALID_HID);
  EXPECT_EQ(H5Rget_region(H5I_INVALID_HID, H5R_DATASET_REGION, &zero_reg_ref),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Rget_region(scalar, H5R_DATASET_REGION, &zero_reg_ref),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Rget_region(file, H5R_DATASET_REGION, &zero_reg_ref),
            H5I_INVALID_HID);

  // H5Rget_name failures.
  EXPECT_LT(H5Rget_name(file, H5R_OBJECT, nullptr, name_buf.data(),
                        name_buf.size()),
            0);
  EXPECT_LT(H5Rget_name(file, H5R_OBJECT2, &obj_ref, name_buf.data(),
                        name_buf.size()),
            0);
  EXPECT_LT(H5Rget_name(H5I_INVALID_HID, H5R_OBJECT, &obj_ref, name_buf.data(),
                        name_buf.size()),
            0);
  EXPECT_LT(H5Rget_name(scalar, H5R_OBJECT, &obj_ref, name_buf.data(),
                        name_buf.size()),
            0);
  EXPECT_LT(H5Rget_name(file, H5R_DATASET_REGION, &zero_reg_ref,
                        name_buf.data(), name_buf.size()),
            0);
  EXPECT_EQ(H5Rget_name(file, H5R_OBJECT, &bogus_obj_ref, name_buf.data(),
                        name_buf.size()),
            0);
}

TEST(H5RdeprecTest, NonNativeVolConnectorIsRejected) {
  GetStubVolState() = StubVolState{};
  absl::Cleanup reset_stub = [] { GetStubVolState() = StubVolState{}; };

  const hid_t vol_id =
      H5VLregister_connector(&kNonNativeVolClass, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  absl::Cleanup unregister_vol = [vol_id] { H5VLunregister_connector(vol_id); };

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_vol(fapl, vol_id, nullptr), 0);

  ScopedFile stub_file(
      H5Fcreate("non_native_stub.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(stub_file.valid());

  hobj_ref_t obj_ref = 0;
  hdset_reg_ref_t reg_ref{};
  H5O_type_t otype = H5O_TYPE_UNKNOWN;
  std::array<char, 16> name_buf{};

  ScopedErrorSilence silence;
  // 1. H5VL_object_is_native succeeds and returns false.
  EXPECT_LT(
      H5Rcreate(&obj_ref, stub_file, "/obj", H5R_OBJECT, H5I_INVALID_HID), 0);
  EXPECT_EQ(H5Rget_obj_type1(stub_file, H5R_OBJECT, &obj_ref), H5G_UNKNOWN);
  EXPECT_EQ(H5Rdereference1(stub_file, H5R_OBJECT, &obj_ref), H5I_INVALID_HID);
  EXPECT_LT(H5Rget_obj_type2(stub_file, H5R_OBJECT, &obj_ref, &otype), 0);
  EXPECT_EQ(H5Rdereference2(stub_file, H5P_DEFAULT, H5R_OBJECT, &obj_ref),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Rget_region(stub_file, H5R_DATASET_REGION, &reg_ref),
            H5I_INVALID_HID);

  // 2. H5VL_object_is_native fails when get_conn_cls fails on TERM level.
  GetStubVolState().fail_term_conn_cls = true;
  EXPECT_LT(
      H5Rcreate(&obj_ref, stub_file, "/obj", H5R_OBJECT, H5I_INVALID_HID), 0);
  EXPECT_EQ(H5Rget_obj_type1(stub_file, H5R_OBJECT, &obj_ref), H5G_UNKNOWN);
  EXPECT_EQ(H5Rdereference1(stub_file, H5R_OBJECT, &obj_ref), H5I_INVALID_HID);
  EXPECT_LT(H5Rget_obj_type2(stub_file, H5R_OBJECT, &obj_ref, &otype), 0);
  EXPECT_EQ(H5Rdereference2(stub_file, H5P_DEFAULT, H5R_OBJECT, &obj_ref),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Rget_region(stub_file, H5R_DATASET_REGION, &reg_ref),
            H5I_INVALID_HID);
  GetStubVolState().fail_term_conn_cls = false;

  // 3. H5Rget_name() reaches H5R__decode_token_compat(H5R_OBJECT1) and
  //    H5VL_object_get(H5VL_OBJECT_GET_NAME) without checking is_native.
  GetStubVolState().fail_get_file = true;
  EXPECT_LT(H5Rget_name(stub_file, H5R_OBJECT, &obj_ref, name_buf.data(),
                        name_buf.size()),
            0);

  GetStubVolState().fail_get_file = false;
  GetStubVolState().fail_get_cont_info = true;
  EXPECT_LT(H5Rget_name(stub_file, H5R_OBJECT, &obj_ref, name_buf.data(),
                        name_buf.size()),
            0);

  GetStubVolState().fail_get_cont_info = false;
  GetStubVolState().cont_token_size = sizeof(haddr_t) + 1;
  EXPECT_LT(H5Rget_name(stub_file, H5R_OBJECT, &obj_ref, name_buf.data(),
                        name_buf.size()),
            0);

  GetStubVolState().cont_token_size = sizeof(haddr_t);
  GetStubVolState().fail_get_name = true;
  EXPECT_LT(H5Rget_name(stub_file, H5R_OBJECT, &obj_ref, name_buf.data(),
                        name_buf.size()),
            0);

  GetStubVolState().fail_get_name = false;
  EXPECT_EQ(H5Rget_name(stub_file, H5R_OBJECT, &obj_ref, name_buf.data(),
                        name_buf.size()),
            0);
}

TEST(H5RdeprecTest, ReadOnlyRegionCreateAndPreV112LargeCoordFail) {
  // 1. H5Rcreate(H5R_DATASET_REGION) writes to the global heap and must fail
  //    cleanly on a read-only disk file, while H5R_OBJECT succeeds on "/".
  TempFile tmp("ro_deprec.h5");
  {
    ScopedFile wfile(
        H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(wfile.valid());
    constexpr hsize_t kDim[1] = {4};
    ScopedSpace space(H5Screate_simple(1, kDim, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(wfile, "/ds", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
  }

  {
    ScopedFile rfile(H5Fopen(tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(rfile.valid());

    hobj_ref_t root_ref = 0;
    ASSERT_GE(
        H5Rcreate(&root_ref, rfile, "/", H5R_OBJECT, H5I_INVALID_HID), 0);
    std::array<char, 8> root_name{};
    EXPECT_EQ(H5Rget_name(rfile, H5R_OBJECT, &root_ref, root_name.data(),
                          root_name.size()),
              1);
    EXPECT_STREQ(root_name.data(), "/");

    constexpr hsize_t kDim[1] = {4};
    ScopedSpace sel_space(H5Screate_simple(1, kDim, nullptr));
    ASSERT_TRUE(sel_space.valid());
    constexpr hsize_t kPt[1] = {2};
    ASSERT_GE(H5Sselect_elements(sel_space, H5S_SELECT_SET, 1, kPt), 0);

    hdset_reg_ref_t reg_ref{};
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Rcreate(&reg_ref, rfile, "/ds", H5R_DATASET_REGION, sel_space), 0);
  }

  // 2. Selecting a coordinate > UINT32_MAX under (EARLIEST, V18) libver bounds
  //    causes H5S_SELECT_SERIAL_SIZE to fail inside
  //    H5R__encode_token_region_compat().
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18), 0);
  ScopedFile old_file(
      H5Fcreate("old_bounds_deprec.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(old_file.valid());

  constexpr hsize_t kBigDim[1] = {5000000000ULL};
  constexpr hsize_t kChunk[1] = {1};
  ScopedSpace big_space(H5Screate_simple(1, kBigDim, nullptr));
  ASSERT_TRUE(big_space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl, 1, kChunk), 0);
  ScopedDataset big_dset(H5Dcreate2(old_file, "/big", H5T_NATIVE_INT32,
                                    big_space, H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_TRUE(big_dset.valid());

  ScopedSpace big_sel(H5Scopy(big_space));
  ASSERT_TRUE(big_sel.valid());
  constexpr hsize_t kLargeCoord[1] = {4500000000ULL};
  ASSERT_GE(H5Sselect_elements(big_sel, H5S_SELECT_SET, 1, kLargeCoord), 0);

  hdset_reg_ref_t bad_reg_ref{};
  ScopedErrorSilence silence;
  EXPECT_LT(
      H5Rcreate(&bad_reg_ref, old_file, "/big", H5R_DATASET_REGION, big_sel),
      0);
}

}  // namespace
}  // namespace hdf5
