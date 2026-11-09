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
// Tests reference datatype conversion (H5T__conv_ref) across object, region,
// attribute, nil, external, deprecated H5T_STD_REF_OBJ/DSETREG, strided
// compound, disk-to-disk copy, background strides, and error-unwind paths
// (third_party/hdf5/src/H5Tconv_reference.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tconv_reference.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5TCONV_REF_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5T_FRIEND
// H5private.h omits H5__Float16 under __cplusplus, while H5Tpkg.h still
// declares unreferenced extern INF globals with that type.
using H5__Float16 = uint16_t;
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tconv.h"
#include "third_party/hdf5/src/H5Tconv_reference.h"
#include "third_party/hdf5/src/H5Tpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5TCONV_REF_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TCONV_REF_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

void DestroyRefs(std::vector<H5R_ref_t>& refs) {
  for (H5R_ref_t& ref : refs) {
    H5Rdestroy(&ref);
  }
}

bool IsZeroBytes(const H5R_ref_t& ref) {
  std::array<uint8_t, sizeof(H5R_ref_t)> bytes{};
  std::memcpy(bytes.data(), &ref, sizeof(bytes));
  return std::all_of(bytes.begin(), bytes.end(),
                     [](uint8_t b) { return b == 0; });
}

TEST(H5TconvReferenceTest, ReadWriteMixedObjectRegionAttrNilAndExternalRefs) {
  // 1. Create target and external Core VFD files.
  ScopedFile file = CreateInMemoryFile("ref_main.h5");
  ASSERT_GE(file, 0);
  ScopedFile ext_file = CreateInMemoryFile("ref_external.h5");
  ASSERT_GE(ext_file, 0);

  constexpr std::array<hsize_t, 1> kDataDims = {4};
  ScopedSpace data_space(H5Screate_simple(1, kDataDims.data(), nullptr));
  ASSERT_GE(data_space, 0);

  ScopedDataset target_dset(H5Dcreate2(file, "target", H5T_NATIVE_INT32,
                                       data_space, H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
  ASSERT_GE(target_dset, 0);
  constexpr std::array<int32_t, 4> kValues = {10, 20, 30, 40};
  ASSERT_GE(H5Dwrite(target_dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, kValues.data()),
            0);

  ScopedSpace attr_space(H5Screate(H5S_SCALAR));
  ASSERT_GE(attr_space, 0);
  ScopedAttr target_attr(H5Acreate2(target_dset, "tag", H5T_NATIVE_INT32,
                                    attr_space, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(target_attr, 0);
  constexpr int32_t kAttrVal = 99;
  ASSERT_GE(H5Awrite(target_attr, H5T_NATIVE_INT32, &kAttrVal), 0);

  ScopedGroup ext_grp(
      H5Gcreate2(ext_file, "ext_group", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(ext_grp, 0);

  // Region selection covering 2 elements.
  ScopedSpace reg_space(H5Scopy(data_space));
  ASSERT_GE(reg_space, 0);
  constexpr std::array<hsize_t, 2> kCoords = {1, 3};
  ASSERT_GE(
      H5Sselect_elements(reg_space, H5S_SELECT_SET, 2, kCoords.data()), 0);

  // 2. Build >= 6 references so reading hits both safe>=2 and safe<2 passes,
  //    starts with small obj ref and grows conv_buf for region/attr/ext refs,
  //    and exercises is_nil -> setnull.
  constexpr size_t kNumRefs = 6;
  std::vector<H5R_ref_t> write_refs(kNumRefs);
  std::memset(write_refs.data(), 0, write_refs.size() * sizeof(H5R_ref_t));
  absl::Cleanup destroy_write = [&] { DestroyRefs(write_refs); };

  ASSERT_GE(
      H5Rcreate_object(file, "target", H5P_DEFAULT, &write_refs[0]), 0);
  ASSERT_GE(
      H5Rcreate_region(file, "target", reg_space, H5P_DEFAULT, &write_refs[1]),
      0);
  ASSERT_GE(
      H5Rcreate_attr(file, "target", "tag", H5P_DEFAULT, &write_refs[2]), 0);
  // write_refs[3] remains zeroed (nil reference).
  ASSERT_GE(
      H5Rcreate_object(file, "target", H5P_DEFAULT, &write_refs[4]), 0);
  ASSERT_GE(H5Rcreate_object(ext_file, "ext_group", H5P_DEFAULT, &write_refs[5]),
            0);

  constexpr hsize_t kRefDim = kNumRefs;
  ScopedSpace ref_space(H5Screate_simple(1, &kRefDim, nullptr));
  ASSERT_GE(ref_space, 0);
  ScopedDataset ref_dset(H5Dcreate2(file, "refs", H5T_STD_REF, ref_space,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(ref_dset, 0);
  ASSERT_GE(H5Dwrite(ref_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     write_refs.data()),
            0);

  // 3. Read back and validate every reference type including nil and external.
  std::vector<H5R_ref_t> read_refs(kNumRefs);
  std::memset(read_refs.data(), 0, read_refs.size() * sizeof(H5R_ref_t));
  absl::Cleanup destroy_read = [&] { DestroyRefs(read_refs); };

  ASSERT_GE(H5Dread(ref_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_refs.data()),
            0);

  EXPECT_EQ(H5Rget_type(&read_refs[0]), H5R_OBJECT2);
  EXPECT_EQ(H5Rget_type(&read_refs[1]), H5R_DATASET_REGION2);
  EXPECT_EQ(H5Rget_type(&read_refs[2]), H5R_ATTR);
  EXPECT_TRUE(IsZeroBytes(read_refs[3]));  // nil setnull zeros H5R_ref_t
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Ropen_object(&read_refs[3], H5P_DEFAULT, H5P_DEFAULT), 0);
  }
  EXPECT_EQ(H5Rget_type(&read_refs[4]), H5R_OBJECT2);
  EXPECT_EQ(H5Rget_type(&read_refs[5]), H5R_OBJECT2);

  EXPECT_GT(H5Requal(&read_refs[0], &write_refs[0]), 0);
  ScopedSpace reopened_region(
      H5Ropen_region(&read_refs[1], H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(reopened_region, 0);
  EXPECT_EQ(H5Sget_select_npoints(reopened_region), 2);
  EXPECT_GT(H5Requal(&read_refs[2], &write_refs[2]), 0);
  EXPECT_GT(H5Requal(&read_refs[4], &write_refs[4]), 0);
}

TEST(H5TconvReferenceTest, DeprecatedObjAndDsetRegConvertToStdRef) {
  ScopedFile file = CreateInMemoryFile("ref_legacy.h5");
  ASSERT_GE(file, 0);

  constexpr std::array<hsize_t, 1> kDims = {3};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_GE(space, 0);
  ScopedDataset target(H5Dcreate2(file, "data", H5T_NATIVE_INT, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(target, 0);

  // Object references via H5T_STD_REF_OBJ -> read as H5T_STD_REF.
  constexpr size_t kN = 5;
  std::vector<hobj_ref_t> obj_refs(kN, 0);
  ASSERT_GE(H5Rcreate(&obj_refs[0], file, "data", H5R_OBJECT, H5I_INVALID_HID),
            0);
  ASSERT_GE(H5Rcreate(&obj_refs[4], file, "data", H5R_OBJECT, H5I_INVALID_HID),
            0);

  constexpr hsize_t kRefN = kN;
  ScopedSpace ref_space(H5Screate_simple(1, &kRefN, nullptr));
  ASSERT_GE(ref_space, 0);
  ScopedDataset obj_dset(H5Dcreate2(file, "obj_refs", H5T_STD_REF_OBJ,
                                    ref_space, H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
  ASSERT_GE(obj_dset, 0);
  ASSERT_GE(H5Dwrite(obj_dset, H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     obj_refs.data()),
            0);

  std::vector<H5R_ref_t> out_obj_refs(kN);
  std::memset(out_obj_refs.data(), 0, out_obj_refs.size() * sizeof(H5R_ref_t));
  absl::Cleanup destroy_obj = [&] { DestroyRefs(out_obj_refs); };
  ASSERT_GE(H5Dread(obj_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    out_obj_refs.data()),
            0);
  // Reading legacy H5T_STD_REF_OBJ into H5T_STD_REF yields H5R_OBJECT2.
  EXPECT_EQ(H5Rget_type(&out_obj_refs[0]), H5R_OBJECT2);
  EXPECT_TRUE(IsZeroBytes(out_obj_refs[1]));
  EXPECT_EQ(H5Rget_type(&out_obj_refs[4]), H5R_OBJECT2);

  // Region reference via H5T_STD_REF_DSETREG -> read as H5T_STD_REF.
  ScopedSpace reg_sel(H5Scopy(space));
  ASSERT_GE(reg_sel, 0);
  constexpr hsize_t kStart = 0;
  constexpr hsize_t kCount = 2;
  ASSERT_GE(H5Sselect_hyperslab(reg_sel, H5S_SELECT_SET, &kStart, nullptr,
                                &kCount, nullptr),
            0);
  hdset_reg_ref_t reg_ref{};
  ASSERT_GE(H5Rcreate(&reg_ref, file, "data", H5R_DATASET_REGION, reg_sel), 0);

  constexpr hsize_t kOne = 1;
  ScopedSpace one_space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_GE(one_space, 0);
  ScopedDataset reg_dset(H5Dcreate2(file, "reg_ref", H5T_STD_REF_DSETREG,
                                    one_space, H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
  ASSERT_GE(reg_dset, 0);
  ASSERT_GE(H5Dwrite(reg_dset, H5T_STD_REF_DSETREG, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &reg_ref),
            0);

  std::vector<H5R_ref_t> out_reg_refs(1);
  std::memset(out_reg_refs.data(), 0, sizeof(H5R_ref_t));
  absl::Cleanup destroy_reg = [&] { DestroyRefs(out_reg_refs); };
  ASSERT_GE(H5Dread(reg_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    out_reg_refs.data()),
            0);
  EXPECT_EQ(H5Rget_type(&out_reg_refs[0]), H5R_DATASET_REGION2);
  ScopedSpace upgraded_reg(
      H5Ropen_region(&out_reg_refs[0], H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(upgraded_reg, 0);
  EXPECT_EQ(H5Sget_select_npoints(upgraded_reg), 2);
}

TEST(H5TconvReferenceTest, StridedCompoundWithReferencesExercisesBufStride) {
  // Keep H5R_ref_t first (offset 0) and write 1 element so HDF5's packed
  // on-disk compound conversion buffer keeps H5R_ref_t 8-byte aligned.
  struct RefRecord {
    H5R_ref_t ref;
    int32_t key;
    int32_t tag;
  };

  ScopedFile file = CreateInMemoryFile("ref_compound.h5");
  ASSERT_GE(file, 0);
  ScopedGroup grp(
      H5Gcreate2(file, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(grp, 0);

  ScopedType cmp_type(H5Tcreate(H5T_COMPOUND, sizeof(RefRecord)));
  ASSERT_GE(cmp_type, 0);
  ASSERT_GE(
      H5Tinsert(cmp_type, "ref", offsetof(RefRecord, ref), H5T_STD_REF), 0);
  ASSERT_GE(
      H5Tinsert(cmp_type, "key", offsetof(RefRecord, key), H5T_NATIVE_INT32),
      0);
  ASSERT_GE(
      H5Tinsert(cmp_type, "tag", offsetof(RefRecord, tag), H5T_NATIVE_INT32),
      0);

  std::array<RefRecord, 1> write_rows{};
  absl::Cleanup destroy_w = [&] {
    for (auto& r : write_rows) H5Rdestroy(&r.ref);
  };
  write_rows[0].key = 1;
  write_rows[0].tag = 11;
  ASSERT_GE(H5Rcreate_object(file, "grp", H5P_DEFAULT, &write_rows[0].ref), 0);

  constexpr hsize_t kRows = 1;
  ScopedSpace space(H5Screate_simple(1, &kRows, nullptr));
  ASSERT_GE(space, 0);
  ScopedDataset dset(H5Dcreate2(file, "cmp_refs", cmp_type, space, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(dset, 0);
  ASSERT_GE(H5Dwrite(dset, cmp_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     write_rows.data()),
            0);

  std::array<RefRecord, 1> read_rows{};
  absl::Cleanup destroy_r = [&] {
    for (auto& r : read_rows) H5Rdestroy(&r.ref);
  };
  ASSERT_GE(H5Dread(dset, cmp_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_rows.data()),
            0);
  EXPECT_EQ(read_rows[0].key, 1);
  EXPECT_EQ(read_rows[0].tag, 11);
  EXPECT_EQ(H5Rget_type(&read_rows[0].ref), H5R_OBJECT2);
  EXPECT_GT(H5Requal(&read_rows[0].ref, &write_rows[0].ref), 0);
}

TEST(H5TconvReferenceTest, DirectInternalValidationDiskCopyBkgAndUnwind) {
  ScopedFile file = CreateInMemoryFile("ref_internal.h5");
  ASSERT_GE(file, 0);
  ScopedGroup grp(
      H5Gcreate2(file, "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(grp, 0);
  ScopedSpace sspace(H5Screate(H5S_SCALAR));
  ASSERT_GE(sspace, 0);
  ScopedAttr attr(H5Acreate2(grp, "a", H5T_NATIVE_INT, sspace, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_GE(attr, 0);

  H5VL_object_t* file_vol = H5VL_vol_object(file);
  ASSERT_NE(file_vol, nullptr);

  ScopedType mem_id(H5Tcopy(H5T_STD_REF));
  ScopedType disk_src_id(H5Tcopy(H5T_STD_REF));
  ScopedType disk_dst_id(H5Tcopy(H5T_STD_REF));
  ScopedType non_opaque_id(H5Tcopy(H5T_STD_REF_OBJ));
  ScopedType int_id(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_GE(mem_id, 0);
  ASSERT_GE(disk_src_id, 0);
  ASSERT_GE(disk_dst_id, 0);
  ASSERT_GE(non_opaque_id, 0);
  ASSERT_GE(int_id, 0);

  auto* mem_dt = static_cast<H5T_t*>(H5I_object_verify(mem_id, H5I_DATATYPE));
  auto* disk_src_dt =
      static_cast<H5T_t*>(H5I_object_verify(disk_src_id, H5I_DATATYPE));
  auto* disk_dst_dt =
      static_cast<H5T_t*>(H5I_object_verify(disk_dst_id, H5I_DATATYPE));
  auto* non_opaque_dt =
      static_cast<H5T_t*>(H5I_object_verify(non_opaque_id, H5I_DATATYPE));
  auto* int_dt = static_cast<H5T_t*>(H5I_object_verify(int_id, H5I_DATATYPE));

  H5R_ref_t obj_mem_ref{};
  H5R_ref_t attr_mem_ref{};
  ASSERT_GE(H5Rcreate_object(file, "g", H5P_DEFAULT, &obj_mem_ref), 0);
  absl::Cleanup d_obj = [&] { H5Rdestroy(&obj_mem_ref); };
  ASSERT_GE(H5Rcreate_attr(file, "g", "a", H5P_DEFAULT, &attr_mem_ref), 0);
  absl::Cleanup d_attr = [&] { H5Rdestroy(&attr_mem_ref); };

  // Push API context so H5T_set_loc and VOL blob callbacks invoked directly by
  // H5T__conv_ref can access the active VOL wrapper stack.
  H5CX_node_t api_ctx{};
  ASSERT_GE(H5CX_push(&api_ctx), 0);
  absl::Cleanup pop_ctx = [] { H5CX_pop(false); };

  ASSERT_GE(H5T_set_loc(disk_src_dt, file_vol, H5T_LOC_DISK), 0);
  ASSERT_GE(H5T_set_loc(disk_dst_dt, file_vol, H5T_LOC_DISK), 0);

  const size_t disk_sz = disk_src_dt->shared->size;
  ASSERT_LT(disk_sz, sizeof(H5R_ref_t));

  H5T_conv_ctx_t ctx{};
  H5T_cdata_t cdata{};

  // 1. Validate INIT & FREE & invalid command paths.
  {
    ScopedErrorSilence silence;
    cdata.command = H5T_CONV_INIT;
    EXPECT_LT(
        H5T__conv_ref(nullptr, mem_dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    EXPECT_LT(
        H5T__conv_ref(mem_dt, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    EXPECT_LT(
        H5T__conv_ref(int_dt, mem_dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    EXPECT_LT(
        H5T__conv_ref(mem_dt, int_dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    EXPECT_LT(H5T__conv_ref(mem_dt, non_opaque_dt, &cdata, &ctx, 0, 0, 0,
                            nullptr, nullptr),
              0);

    cdata.command = H5T_CONV_CONV;
    EXPECT_LT(
        H5T__conv_ref(nullptr, mem_dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    EXPECT_LT(
        H5T__conv_ref(mem_dt, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);

    cdata.command = static_cast<H5T_cmd_t>(999);
    EXPECT_LT(
        H5T__conv_ref(mem_dt, mem_dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
  }

  cdata.command = H5T_CONV_INIT;
  ASSERT_GE(
      H5T__conv_ref(mem_dt, disk_src_dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_EQ(cdata.need_bkg, H5T_BKG_NO);
  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(
      H5T__conv_ref(mem_dt, disk_src_dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);

  // 2. Encode valid references into disk buffers (both obj ref and attr ref),
  //    then exercise disk->disk direct copy (hits dst_copy && dst->loc == DISK).
  std::vector<uint8_t> disk_obj_bytes(sizeof(H5R_ref_t), 0);
  std::memcpy(disk_obj_bytes.data(), &obj_mem_ref, sizeof(H5R_ref_t));
  cdata.command = H5T_CONV_INIT;
  ASSERT_GE(H5T__conv_ref(mem_dt, disk_src_dt, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);
  cdata.command = H5T_CONV_CONV;
  ASSERT_GE(H5T__conv_ref(mem_dt, disk_src_dt, &cdata, &ctx, 1, 0, 0,
                          disk_obj_bytes.data(), nullptr),
            0);

  std::vector<uint8_t> disk_attr_bytes(sizeof(H5R_ref_t), 0);
  std::memcpy(disk_attr_bytes.data(), &attr_mem_ref, sizeof(H5R_ref_t));
  ASSERT_GE(H5T__conv_ref(mem_dt, disk_src_dt, &cdata, &ctx, 1, 0, 0,
                          disk_attr_bytes.data(), nullptr),
            0);

  // Disk -> disk copy of H5R_OBJECT2 hits line 223-224 (dst_copy && dst == DISK).
  std::vector<uint8_t> disk_copy(disk_sz, 0);
  std::memcpy(disk_copy.data(), disk_obj_bytes.data(), disk_sz);
  cdata.command = H5T_CONV_INIT;
  ASSERT_GE(H5T__conv_ref(disk_src_dt, disk_dst_dt, &cdata, &ctx, 0, 0, 0,
                          nullptr, nullptr),
            0);
  cdata.command = H5T_CONV_CONV;
  ASSERT_GE(H5T__conv_ref(disk_src_dt, disk_dst_dt, &cdata, &ctx, 1, 0, 0,
                          disk_copy.data(), nullptr),
            0);
  EXPECT_EQ(std::memcmp(disk_copy.data(), disk_obj_bytes.data(), disk_sz), 0);

  // 3. Background buffer with bkg_stride != 0 and d_stride > s_stride (both
  //    safe >= 2 and safe < 2). Use 6 elements of disk_obj_bytes -> mem_dt.
  constexpr size_t kN = 6;
  std::vector<uint8_t> conv_with_bkg(kN * sizeof(H5R_ref_t), 0);
  std::vector<uint8_t> dummy_bkg(kN * sizeof(H5R_ref_t), 0);
  for (size_t i = 0; i < kN; ++i) {
    std::memcpy(conv_with_bkg.data() + i * disk_sz, disk_obj_bytes.data(),
                disk_sz);
  }
  cdata.command = H5T_CONV_INIT;
  ASSERT_GE(
      H5T__conv_ref(disk_src_dt, mem_dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  cdata.command = H5T_CONV_CONV;
  ASSERT_GE(H5T__conv_ref(disk_src_dt, mem_dt, &cdata, &ctx, kN, 0,
                          sizeof(H5R_ref_t), conv_with_bkg.data(),
                          dummy_bkg.data()),
            0);
  for (size_t i = 0; i < kN; ++i) {
    H5R_ref_t* r =
        reinterpret_cast<H5R_ref_t*>(conv_with_bkg.data() + i * sizeof(H5R_ref_t));
    EXPECT_EQ(H5Rget_type(r), H5R_OBJECT2);
    H5Rdestroy(r);
  }

  // 4. Error-unwinding paths (lines 258-301):
  //    4a. Forward pass (buf_stride = sizeof(H5R_ref_t)): element 0 converts,
  //        element 1 fails H5T__ref_disk_getsize with corrupt ref_type.
  {
    ScopedErrorSilence silence;
    std::vector<uint8_t> buf(2 * sizeof(H5R_ref_t), 0);
    std::memcpy(buf.data(), disk_obj_bytes.data(), disk_sz);
    buf[sizeof(H5R_ref_t)] = static_cast<uint8_t>(H5R_MAXTYPE);
    EXPECT_LT(H5T__conv_ref(disk_src_dt, mem_dt, &cdata, &ctx, 2,
                            sizeof(H5R_ref_t), 0, buf.data(), nullptr),
              0);
  }

  //    4b. Backward pass 1 failure (d_stride > 0, nelmts == orig_nelmts):
  //        Compute exact Pass-1 start index: safe = kN - (kN*disk_sz + 63)/64.
  {
    ScopedErrorSilence silence;
    const size_t safe1 =
        kN - (((kN * disk_sz) + (sizeof(H5R_ref_t) - 1)) / sizeof(H5R_ref_t));
    ASSERT_GE(safe1, 2u);
    const size_t start_idx = kN - safe1;
    std::vector<uint8_t> buf(kN * sizeof(H5R_ref_t), 0);
    for (size_t i = 0; i < kN; ++i) {
      std::memcpy(buf.data() + i * disk_sz, disk_obj_bytes.data(), disk_sz);
    }
    // Let Pass 1 element 0 succeed, then fail on Pass 1 element 1.
    buf[(start_idx + 1) * disk_sz] = static_cast<uint8_t>(H5R_MAXTYPE);
    EXPECT_LT(H5T__conv_ref(disk_src_dt, mem_dt, &cdata, &ctx, kN, 0, 0,
                            buf.data(), nullptr),
              0);
  }

  //    4c. Backward pass 2 failure (d_stride < 0, nelmts < orig_nelmts):
  //        Let Pass 1 finish cleanly, then fail on element 0 (after elements
  //        at top of remaining slice succeeded in reverse order).
  {
    ScopedErrorSilence silence;
    std::vector<uint8_t> buf(kN * sizeof(H5R_ref_t), 0);
    for (size_t i = 0; i < kN; ++i) {
      std::memcpy(buf.data() + i * disk_sz, disk_obj_bytes.data(), disk_sz);
    }
    buf[0] = static_cast<uint8_t>(H5R_MAXTYPE);
    EXPECT_LT(H5T__conv_ref(disk_src_dt, mem_dt, &cdata, &ctx, kN, 0, 0,
                            buf.data(), nullptr),
              0);
  }

  // 5. Exercise cls->read error (valid H5R_ATTR type with bogus blob ID).
  {
    ScopedErrorSilence silence;
    std::vector<uint8_t> bad_blob(sizeof(H5R_ref_t), 0);
    std::memcpy(bad_blob.data(), disk_attr_bytes.data(), disk_sz);
    // Corrupt blob ID bytes (after 2-byte header + 4-byte encoded size) so
    // H5T__ref_disk_getsize succeeds (>0) but H5VL_blob_get fails.
    std::memset(bad_blob.data() + 6, 0xFF, disk_sz - 6);
    EXPECT_LT(H5T__conv_ref(disk_src_dt, mem_dt, &cdata, &ctx, 1, 0, 0,
                            bad_blob.data(), nullptr),
              0);
  }

  // Ensure disk_attr_bytes was valid before cleanup.
  std::vector<uint8_t> attr_roundtrip(sizeof(H5R_ref_t), 0);
  std::memcpy(attr_roundtrip.data(), disk_attr_bytes.data(), disk_sz);
  ASSERT_GE(H5T__conv_ref(disk_src_dt, mem_dt, &cdata, &ctx, 1, 0, 0,
                          attr_roundtrip.data(), nullptr),
            0);
  H5R_ref_t* rt_attr = reinterpret_cast<H5R_ref_t*>(attr_roundtrip.data());
  EXPECT_EQ(H5Rget_type(rt_attr), H5R_ATTR);
  H5Rdestroy(rt_attr);
}

}  // namespace
}  // namespace hdf5
