// Copyright 2017 Google Inc. All Rights Reserved.
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
// Tests HDF5 raster driver.
//
// See also:
//   https://gdal.org/drivers/raster/hdf5.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hdf5/hdf5dataset.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/hdf5.py

#include "third_party/gdal/frmts/hdf5/hdf5dataset.h"

#include <cstdint>
#include <filesystem>  // NOLINT
#include <format>  // NOLINT
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/frmts/hdf5/hdf5_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/hdf5/hdf5_api.h"
#include "third_party/gdal/frmts/hdf5/hdf5drivercore.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class DummyRedirectDataset final : public GDALDataset {};

class TestableHDF5Dataset final : public HDF5Dataset {
 public:
  TestableHDF5Dataset() = default;
  ~TestableHDF5Dataset() override = default;

  bool OpenForWhiteBoxTest(const std::string& filename) {
    m_hHDF5 = GDAL_HDF5Open(filename);
    if (m_hHDF5 < 0) return false;
    hGroupID = H5Gopen(m_hHDF5, "/");
    if (hGroupID < 0) return false;
    ReadGlobalAttributes(TRUE);
    SetMetadata(m_aosMetadata.List());
    return true;
  }

  HDF5GroupObjects* RootGroupObjects() const { return poH5RootGroup; }
  using HDF5Dataset::HDF5FindDatasetObjects;
  using HDF5Dataset::HDF5FindDatasetObjectsbyPath;
  using HDF5Dataset::HDF5ReadDoubleAttr;
  using HDF5Dataset::ReadGlobalAttributes;
};

GDALDataset* FakeSxxOpen(GDALOpenInfo*) { return new DummyRedirectDataset(); }

std::unique_ptr<GDALDriver> RegisterFakeDriver(
    const char* name, GDALDataset* (*open_fn)(GDALOpenInfo*)) {
  auto drv = std::make_unique<GDALDriver>();
  drv->SetDescription(name);
  drv->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
  drv->SetMetadataItem(GDAL_DCAP_OPEN, "YES");
  drv->pfnOpen = open_fn;
  GDALRegisterDriver(drv.get());
  return drv;
}

template <typename T>
void Write1DResizableAttr(hid_t loc_id, const char* name, hid_t type_id,
                          const std::vector<T>& values) {
  const hsize_t dim = values.size();
  const hsize_t max_dim = dim == 0 ? 1 : dim;
  ScopedH5Dataspace space(H5Screate_simple(1, &dim, &max_dim));
  ASSERT_GE(space.get(), 0);
  ScopedH5Attribute attr(H5Acreate(loc_id, name, type_id, space, H5P_DEFAULT));
  ASSERT_GE(attr.get(), 0);
  if (!values.empty()) {
    ASSERT_GE(H5Awrite(attr, type_id, values.data()), 0);
  }
}

void Create2DIntDataset(hid_t group_id, const char* name, hsize_t rows,
                        hsize_t cols) {
  const hsize_t dims[2] = {rows, cols};
  ScopedH5Dataspace space(H5Screate_simple(2, dims, nullptr));
  ASSERT_GE(space.get(), 0);
  ScopedH5Dataset dset(
      H5Dcreate(group_id, name, H5T_NATIVE_INT16, space, H5P_DEFAULT));
  ASSERT_GE(dset.get(), 0);
}

void Create3DFloatDataset(hid_t group_id, const char* name, hsize_t dim0,
                          hsize_t dim1, hsize_t dim2) {
  const hsize_t dims[3] = {dim0, dim1, dim2};
  ScopedH5Dataspace space(H5Screate_simple(3, dims, nullptr));
  ASSERT_GE(space.get(), 0);
  ScopedH5Dataset dset(
      H5Dcreate(group_id, name, H5T_NATIVE_FLOAT, space, H5P_DEFAULT));
  ASSERT_GE(dset.get(), 0);
}

TEST(IdentifyTest, DoesNotExist) {
  auto open_info = std::make_unique<GDALOpenInfo>("/does_not_exist",
                                                  GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, HDF5DatasetIdentify(open_info.get()));
  EXPECT_EQ(nullptr, HDF5Dataset::Open(open_info.get()));
}

TEST(Hdf5DatasetTest, FileDriverAndDataTypes) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;

  // 1. File driver lifecycle and registration idempotency.
  EXPECT_GE(HDF5GetFileDriver(), 0);
  HDF5UnloadFileDriver();
  EXPECT_GE(HDF5GetFileDriver(), 0);

  GDALRegister_HDF5();
  EXPECT_NE(nullptr, GDALGetDriverByName(HDF5_DRIVER_NAME));
  GDALRegister_HDF5();

  // 2. Predefined scalar GDALDataType and human-readable names.
  struct TypeExpectation {
    hid_t h5_type;
    GDALDataType gdal_type;
    const char* name;
  };
  const TypeExpectation kScalarTypes[] = {
      {.h5_type = H5T_NATIVE_CHAR,
       .gdal_type = std::is_signed_v<char> ? GDT_Int8 : GDT_Byte,
       .name = "8-bit character"},
      {.h5_type = H5T_NATIVE_SCHAR,
       .gdal_type = GDT_Int8,
       .name = std::is_signed_v<char> ? "8-bit character"
                                      : "8-bit signed character"},
      {.h5_type = H5T_NATIVE_UCHAR,
       .gdal_type = GDT_Byte,
       .name = std::is_signed_v<char> ? "8-bit unsigned character"
                                      : "8-bit character"},
      {.h5_type = H5T_NATIVE_SHORT,
       .gdal_type = GDT_Int16,
       .name = "16-bit integer"},
      {.h5_type = H5T_NATIVE_USHORT,
       .gdal_type = GDT_UInt16,
       .name = "16-bit unsigned integer"},
      {.h5_type = H5T_NATIVE_INT,
       .gdal_type = GDT_Int32,
       .name = "32-bit integer"},
      {.h5_type = H5T_NATIVE_UINT,
       .gdal_type = GDT_UInt32,
       .name = "32-bit unsigned integer"},
      {.h5_type = H5T_NATIVE_INT64,
       .gdal_type = GDT_Int64,
       .name = "64-bit integer"},
      {.h5_type = H5T_NATIVE_UINT64,
       .gdal_type = GDT_UInt64,
       .name = "64-bit unsigned integer"},
      {.h5_type = H5T_NATIVE_LONG,
       .gdal_type = sizeof(long) == 8 ? GDT_Int64 : GDT_Int32,
       .name = sizeof(long) == 8 ? "64-bit integer" : "32/64-bit integer"},
      {.h5_type = H5T_NATIVE_ULONG,
       .gdal_type = sizeof(unsigned long) == 8 ? GDT_UInt64 : GDT_UInt32,
       .name = sizeof(unsigned long) == 8 ? "64-bit unsigned integer"
                                          : "32/64-bit unsigned integer"},
      {.h5_type = H5T_NATIVE_FLOAT,
       .gdal_type = GDT_Float32,
       .name = "32-bit floating-point"},
      {.h5_type = H5T_NATIVE_DOUBLE,
       .gdal_type = GDT_Float64,
       .name = "64-bit floating-point"},
      {.h5_type = H5T_STD_REF_OBJ, .gdal_type = GDT_Unknown, .name = "Unknown"},
  };
  for (const TypeExpectation& tc : kScalarTypes) {
    EXPECT_EQ(tc.gdal_type, HDF5Dataset::GetDataType(tc.h5_type));
    EXPECT_STREQ(tc.name, HDF5Dataset::GetDataTypeName(tc.h5_type));
  }

#ifdef HDF5_HAVE_FLOAT16
  EXPECT_EQ(GDT_Float32, HDF5Dataset::GetDataType(H5T_NATIVE_FLOAT16));
  EXPECT_STREQ("16-bit floating-point",
               HDF5Dataset::GetDataTypeName(H5T_NATIVE_FLOAT16));
#endif

  // 3. Compound complex and malformed compound types.
  ScopedH5Datatype cint16_t(
      CreateComplexH5Type(H5T_NATIVE_SHORT, "real", "imag"));
  ScopedH5Datatype cint32_t(CreateComplexH5Type(H5T_NATIVE_INT, "R", "I"));
  ScopedH5Datatype clong_t(CreateComplexH5Type(H5T_NATIVE_LONG, "r", "i"));
  ScopedH5Datatype cflt32_t(CreateComplexH5Type(H5T_NATIVE_FLOAT, "r", "i"));
  ScopedH5Datatype cflt64_t(CreateComplexH5Type(H5T_NATIVE_DOUBLE, "Re", "Im"));
  ScopedH5Datatype cuchar_t(CreateComplexH5Type(H5T_NATIVE_UCHAR, "r", "i"));
  ScopedH5Datatype bad_names_t(CreateComplexH5Type(H5T_NATIVE_FLOAT, "x", "y"));

  ScopedH5Datatype mismatch_t(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_GE(mismatch_t.get(), 0);
  ASSERT_GE(H5Tinsert(mismatch_t, "r", 0, H5T_NATIVE_INT), 0);
  ASSERT_GE(H5Tinsert(mismatch_t, "i", 4, H5T_NATIVE_FLOAT), 0);

  ScopedH5Datatype single_member_t(H5Tcreate(H5T_COMPOUND, 4));
  ASSERT_GE(single_member_t.get(), 0);
  ASSERT_GE(H5Tinsert(single_member_t, "r", 0, H5T_NATIVE_FLOAT), 0);

  const TypeExpectation kCompoundTypes[] = {
      {.h5_type = cint16_t,
       .gdal_type = GDT_CInt16,
       .name = "complex, 16-bit integer"},
      {.h5_type = cint32_t,
       .gdal_type = GDT_CInt32,
       .name = "complex, 32-bit integer"},
      {.h5_type = clong_t,
       .gdal_type = GDT_Unknown,
       .name = "complex, 32/64-bit integer"},
      {.h5_type = cflt32_t,
       .gdal_type = GDT_CFloat32,
       .name = "complex, 32-bit floating-point"},
      {.h5_type = cflt64_t,
       .gdal_type = GDT_CFloat64,
       .name = "complex, 64-bit floating-point"},
      {.h5_type = cuchar_t, .gdal_type = GDT_Unknown, .name = "Unknown"},
      {.h5_type = bad_names_t,
       .gdal_type = GDT_Unknown,
       .name = "complex, 32-bit floating-point"},
      {.h5_type = mismatch_t, .gdal_type = GDT_Unknown, .name = "Unknown"},
      {.h5_type = single_member_t, .gdal_type = GDT_Unknown, .name = "Unknown"},
  };
  for (const TypeExpectation& tc : kCompoundTypes) {
    EXPECT_EQ(tc.gdal_type, HDF5Dataset::GetDataType(tc.h5_type));
    EXPECT_STREQ(tc.name, HDF5Dataset::GetDataTypeName(tc.h5_type));
  }

  EXPECT_FALSE(HDF5Dataset::IsNativeCFloat16(H5T_NATIVE_FLOAT));
  EXPECT_FALSE(HDF5Dataset::IsNativeCFloat16(single_member_t));
  EXPECT_FALSE(HDF5Dataset::IsNativeCFloat16(cflt32_t));
#ifdef HDF5_HAVE_FLOAT16
  ScopedH5Datatype cflt16_t(CreateComplexH5Type(H5T_NATIVE_FLOAT16, "r", "i"));
  EXPECT_TRUE(HDF5Dataset::IsNativeCFloat16(cflt16_t));
  EXPECT_EQ(GDT_CFloat32, HDF5Dataset::GetDataType(cflt16_t));
  EXPECT_STREQ("complex, 16-bit floating-point",
               HDF5Dataset::GetDataTypeName(cflt16_t));
#endif
}

TEST(Hdf5DatasetTest, OpenHierarchySubdatasetsAndAttributes) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;

  // 1. Invalid signature-prefixed file fails GDAL_HDF5Open cleanly.
  {
    ScopedVsimemFile corrupt("corrupt_sig.h5");
    std::string payload(64, '\0');
    constexpr std::string_view kHeader = "\211HDF\r\n\032\ncorrupt_payload";
    payload.replace(0, kHeader.size(), kHeader);
    VsiMemTempWrapper tmp(corrupt.str(), payload);

    ScopedQuietHdf5Errors quiet;
    GDALOpenInfo open_info(corrupt.path(), GDAL_OF_READONLY);
    EXPECT_EQ(nullptr, HDF5Dataset::Open(&open_info));
  }

  // 2. Build HDF5 hierarchy with groups, links, link loops, named types,
  // 0D/1D/2D/3D datasets, and rich attribute types.
  ScopedVsimemFile mem_file("complex_hierarchy.h5");
  {
    ScopedH5File file_id(CreateVsimemHdf5File(mem_file.path()));
    ASSERT_GE(file_id.get(), 0);

    {
      ScopedH5Datatype mutable_float(H5Tcopy(H5T_NATIVE_FLOAT));
      ASSERT_GE(mutable_float.get(), 0);
      ASSERT_GE(H5Tcommit(file_id, "committed_float", mutable_float), 0);
    }

    {
      ScopedH5Group grp_space(H5Gcreate(file_id, "Group With Spaces", 0));
      ASSERT_GE(grp_space.get(), 0);
      Create2DIntDataset(grp_space, "Grid Field", 4, 6);
      Write1DResizableAttr<double>(grp_space, "grp double attr",
                                   H5T_NATIVE_DOUBLE, {10.5, 20.5});

      ScopedH5Group loop_grp(H5Gcreate(grp_space, "LoopSubGroup", 0));
      ASSERT_GE(loop_grp.get(), 0);
      ASSERT_GE(H5Glink(file_id, H5L_TYPE_HARD, "/",
                        "/Group With Spaces/LoopSubGroup/link_to_root"),
                0);
    }

    {
      ScopedH5Group empty_grp(H5Gcreate(file_id, "empty_subgrp", 0));
      ASSERT_GE(empty_grp.get(), 0);
    }

    ASSERT_GE(H5Glink(file_id, H5L_TYPE_SOFT, "/Group With Spaces",
                      "/soft_link_to_group"),
              0);

    Create3DFloatDataset(file_id, "cube_f32", 3, 4, 5);
    {
      ScopedH5Dataset dset_cube(H5Dopen(file_id, "cube_f32"));
      ASSERT_GE(dset_cube.get(), 0);
      Write1DResizableAttr<int>(dset_cube, "dset_attr", H5T_NATIVE_INT, {7, 8});
    }

    // 0D scalar, 1D vector, and unsupported 2D string datasets.
    {
      ScopedH5Dataspace scalar_sp(H5Screate(H5S_SCALAR));
      ASSERT_GE(scalar_sp.get(), 0);
      ScopedH5Dataset dset_0d(H5Dcreate(file_id, "scalar_dset", H5T_NATIVE_INT,
                                        scalar_sp, H5P_DEFAULT));
      ASSERT_GE(dset_0d.get(), 0);

      const hsize_t d1 = 5;
      ScopedH5Dataspace sp_1d(H5Screate_simple(1, &d1, nullptr));
      ASSERT_GE(sp_1d.get(), 0);
      ScopedH5Dataset dset_1d(
          H5Dcreate(file_id, "vec_1d", H5T_NATIVE_INT, sp_1d, H5P_DEFAULT));
      ASSERT_GE(dset_1d.get(), 0);

      const hsize_t dims[2] = {2, 2};
      ScopedH5Dataspace sp_2d(H5Screate_simple(2, dims, nullptr));
      ASSERT_GE(sp_2d.get(), 0);
      ScopedH5Datatype str_t(H5Tcopy(H5T_C_S1));
      ASSERT_GE(H5Tset_size(str_t, 4), 0);
      ScopedH5Dataset dset_str(
          H5Dcreate(file_id, "str_grid_2d", str_t, sp_2d, H5P_DEFAULT));
      ASSERT_GE(dset_str.get(), 0);
    }

    // Attributes of supported and skipped types on root group.
    Write1DResizableAttr<signed char>(file_id, "schar arr", H5T_NATIVE_SCHAR,
                                      {-5, 6});
    Write1DResizableAttr<unsigned char>(file_id, "uchar arr", H5T_NATIVE_UCHAR,
                                        {200, 210});
    Write1DResizableAttr<short>(file_id, "short arr", H5T_NATIVE_SHORT,
                                {-100, 200});
    Write1DResizableAttr<unsigned short>(file_id, "ushort arr",
                                         H5T_NATIVE_USHORT, {1000, 2000});
    Write1DResizableAttr<int>(file_id, "int arr", H5T_NATIVE_INT, {-10, 20});
    Write1DResizableAttr<unsigned int>(file_id, "uint arr", H5T_NATIVE_UINT,
                                       {10U, 30U});
    Write1DResizableAttr<int64_t>(file_id, "i64 arr", H5T_NATIVE_INT64,
                                  {-9999999999LL});
    Write1DResizableAttr<uint64_t>(file_id, "u64 arr", H5T_NATIVE_UINT64,
                                   {9999999999ULL});
    Write1DResizableAttr<long>(file_id, "long arr", H5T_NATIVE_LONG,
                               {-42L, 42L});
    Write1DResizableAttr<unsigned long>(file_id, "ulong arr", H5T_NATIVE_ULONG,
                                        {42UL, 84UL});
    Write1DResizableAttr<float>(file_id, "f32 arr", H5T_NATIVE_FLOAT,
                                {1.5f, 2.5f});
    Write1DResizableAttr<double>(file_id, "f64_root_arr", H5T_NATIVE_DOUBLE,
                                 {3.25, 4.5, 5.75});
    Write1DResizableAttr<double>(file_id, "empty_f64", H5T_NATIVE_DOUBLE, {});
#ifdef HDF5_HAVE_FLOAT16
    Write1DResizableAttr<_Float16>(file_id, "f16 arr", H5T_NATIVE_FLOAT16,
                                   {static_cast<_Float16>(1.25f)});
#endif

    {
      const hsize_t dim = 3;
      ScopedH5Dataspace sp(H5Screate_simple(1, &dim, nullptr));
      ASSERT_GE(sp.get(), 0);
      ScopedH5Datatype vstr_t(H5Tcopy(H5T_C_S1));
      ASSERT_GE(H5Tset_size(vstr_t, H5T_VARIABLE), 0);
      ScopedH5Attribute attr(
          H5Acreate(file_id, "vlen str arr", vstr_t, sp, H5P_DEFAULT));
      ASSERT_GE(attr.get(), 0);
      const char* ptrs[3] = {"alpha", nullptr, "gamma"};
      ASSERT_GE(H5Awrite(attr, vstr_t, ptrs), 0);
    }

    ASSERT_TRUE(WriteScalarStringAttr(file_id, "fixed_str_attr", "abcde",
                                      H5T_STR_NULLPAD));

    {
      ScopedH5Datatype vlen_t(H5Tvlen_create(H5T_NATIVE_INT));
      ASSERT_GE(vlen_t.get(), 0);
      ScopedH5Dataspace sp(H5Screate(H5S_SCALAR));
      ASSERT_GE(sp.get(), 0);
      ScopedH5Attribute attr(
          H5Acreate(file_id, "vlen_attr", vlen_t, sp, H5P_DEFAULT));
      ASSERT_GE(attr.get(), 0);
    }

    std::vector<int> big_vec(15000, 123456);
    Write1DResizableAttr<int>(file_id, "huge_attr", H5T_NATIVE_INT, big_vec);
  }

  // 3. Open as raster dataset (multi-subdataset mode).
  {
    ScopedQuietHdf5Errors quiet;
    GDALOpenInfo open_info(mem_file.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    auto ds = std::unique_ptr<GDALDataset>(HDF5Dataset::Open(&open_info));
    ASSERT_NE(nullptr, ds);

    EXPECT_STREQ("-5 6", ds->GetMetadataItem("schar_arr"));
    EXPECT_STREQ("200 210", ds->GetMetadataItem("uchar_arr"));
    EXPECT_STREQ("-100 200", ds->GetMetadataItem("short_arr"));
    EXPECT_STREQ("1000 2000", ds->GetMetadataItem("ushort_arr"));
    EXPECT_STREQ("-10 20", ds->GetMetadataItem("int_arr"));
    EXPECT_STREQ("10 30", ds->GetMetadataItem("uint_arr"));
    EXPECT_STREQ("alpha {NULL} gamma", ds->GetMetadataItem("vlen_str_arr"));
    EXPECT_STREQ("abcde", ds->GetMetadataItem("fixed_str_attr"));
    EXPECT_EQ(nullptr, ds->GetMetadataItem("vlen_attr"));

    CSLConstList subdatasets = ds->GetMetadata("SUBDATASETS");
    ASSERT_NE(nullptr, subdatasets);
    EXPECT_EQ(2, CSLCount(subdatasets) / 2);
  }

  // 4. White-box test of protected helpers via TestableHDF5Dataset.
  {
    auto wb_ds = std::make_unique<TestableHDF5Dataset>();
    ASSERT_TRUE(wb_ds->OpenForWhiteBoxTest(mem_file.path()));

    HDF5GroupObjects* root_obj = wb_ds->RootGroupObjects();
    ASSERT_NE(nullptr, root_obj);
    EXPECT_NE(nullptr, wb_ds->HDF5FindDatasetObjectsbyPath(
                           root_obj, "//Group_With_Spaces/Grid_Field"));
    EXPECT_EQ(nullptr, wb_ds->HDF5FindDatasetObjectsbyPath(
                           root_obj, "//NoSuchGroup/NoSuchDset"));

    EXPECT_NE(nullptr, wb_ds->HDF5FindDatasetObjects(root_obj, "cube_f32"));
    EXPECT_EQ(nullptr, wb_ds->HDF5FindDatasetObjects(root_obj, "missing_name"));

    auto read_doubles = [&wb_ds](const char* attr_path,
                                 std::unique_ptr<double[], CplFreeDeleter>& out,
                                 int& len) {
      double* raw = nullptr;
      const CPLErr err = wb_ds->HDF5ReadDoubleAttr(attr_path, &raw, &len);
      out.reset(raw);
      return err;
    };

    std::unique_ptr<double[], CplFreeDeleter> dbl_vals;
    int len = 0;
    ASSERT_EQ(CE_None, read_doubles("f64_root_arr", dbl_vals, len));
    EXPECT_EQ(3, len);
    EXPECT_DOUBLE_EQ(3.25, dbl_vals[0]);
    EXPECT_DOUBLE_EQ(5.75, dbl_vals[2]);

    ASSERT_EQ(CE_None, read_doubles("/Group With Spaces/grp double attr",
                                    dbl_vals, len));
    EXPECT_EQ(2, len);
    EXPECT_DOUBLE_EQ(20.5, dbl_vals[1]);

    {
      ScopedQuietHdf5Errors quiet;
      EXPECT_EQ(CE_Failure, read_doubles("/MissingGroup/attr", dbl_vals, len));
      EXPECT_EQ(CE_Failure, read_doubles("missing_root_attr", dbl_vals, len));
      EXPECT_EQ(CE_Failure, read_doubles("int arr", dbl_vals, len));
      // 0-element H5S_SIMPLE dataspace causes VSI_MALLOC2_VERBOSE(0, 8) to
      // return nullptr and fail in HDF5ReadDoubleAttr.
      EXPECT_EQ(CE_Failure, read_doubles("empty_f64", dbl_vals, len));
    }

    auto unopened_ds = std::make_unique<TestableHDF5Dataset>();
    EXPECT_EQ(CE_None, unopened_ds->ReadGlobalAttributes(TRUE));
  }

  // 5. Empty HDF5 file (root group nbObjs == 0) and mode/update checks.
  {
    ScopedVsimemFile empty_file("empty_root.h5");
    {
      ScopedH5File file_id(CreateVsimemHdf5File(empty_file.path()));
      ASSERT_GE(file_id.get(), 0);
    }

    GDALOpenInfo open_info(empty_file.path(),
                           GDAL_OF_RASTER | GDAL_OF_READONLY);
    auto ds = std::unique_ptr<GDALDataset>(HDF5Dataset::Open(&open_info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(nullptr, ds->GetMetadata("SUBDATASETS"));
  }

  {
    CPLConfigOptionSetter char_as_str("GDAL_HDF5_CHAR_AS_STRING", "YES", false);
    ScopedQuietHdf5Errors quiet;
    GDALOpenInfo open_info(mem_file.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    auto ds = std::unique_ptr<GDALDataset>(HDF5Dataset::Open(&open_info));
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetMetadataItem("schar_arr"));
  }

  {
    ScopedQuietHdf5Errors quiet;
    GDALOpenInfo update_info(mem_file.path(), GDAL_OF_RASTER | GDAL_OF_UPDATE);
    EXPECT_EQ(nullptr, HDF5Dataset::Open(&update_info));
    CPLErrorReset();
  }

  {
    GDALOpenInfo md_info(mem_file.path(),
                         GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY);
    auto ds = std::unique_ptr<GDALDataset>(HDF5Dataset::Open(&md_info));
    ASSERT_NE(nullptr, ds);
    auto root_grp = ds->GetRootGroup();
    ASSERT_NE(nullptr, root_grp);
    EXPECT_NE(nullptr, root_grp->OpenMDArray("cube_f32"));
  }
}

TEST(Hdf5DatasetTest, SingleSubdatasetAndDriverRedirects) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;

  // 1. Single 2D subdataset auto-opens via HDF5Image driver.
  ScopedVsimemFile single_ds_file("single_2d.h5");
  {
    ScopedH5File file_id(CreateVsimemHdf5File(single_ds_file.path()));
    ASSERT_GE(file_id.get(), 0);
    Create2DIntDataset(file_id, "single_band", 5, 7);
  }
  {
    ScopedDriverRegistration reg_hdf5(HDF5_DRIVER_NAME, GDALRegister_HDF5);
    ScopedDriverRegistration reg_hdf5_img(HDF5_IMAGE_DRIVER_NAME,
                                          GDALRegister_HDF5Image);
    GDALOpenInfo open_info(single_ds_file.path(),
                           GDAL_OF_RASTER | GDAL_OF_READONLY);
    auto ds = std::unique_ptr<GDALDataset>(HDF5Dataset::Open(&open_info));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(7, ds->GetRasterXSize());
    EXPECT_EQ(5, ds->GetRasterYSize());
    EXPECT_EQ(1, ds->GetRasterCount());
  }

  // 2. Sentinel-3 SRAL + MWR defers to netCDF driver when available.
  ScopedVsimemFile sentinel_file("sentinel3.h5");
  {
    ScopedH5File file_id(CreateVsimemHdf5File(sentinel_file.path()));
    ASSERT_GE(file_id.get(), 0);
    ASSERT_TRUE(WriteScalarStringAttr(file_id, "mission_name", "Sentinel 3A",
                                      H5T_STR_NULLPAD));
    ASSERT_TRUE(WriteScalarStringAttr(file_id, "altimeter_sensor_name", "SRAL",
                                      H5T_STR_NULLPAD));
    ASSERT_TRUE(WriteScalarStringAttr(file_id, "radiometer_sensor_name", "MWR",
                                      H5T_STR_NULLPAD));
    Create2DIntDataset(file_id, "s3_band", 4, 4);
  }
  {
    auto nc_drv = RegisterFakeDriver("netCDF", FakeSxxOpen);
    auto dereg = absl::MakeCleanup([&] { GDALDeregisterDriver(nc_drv.get()); });
    GDALOpenInfo open_info(sentinel_file.path(), GDAL_OF_READONLY);
    auto sentinel_ds =
        std::unique_ptr<GDALDataset>(HDF5Dataset::Open(&open_info));
    EXPECT_EQ(nullptr, sentinel_ds);
  }

  // 3. Safety-belt redirects for S102, S104, S111 via productSpecification.
  struct SxxRedirectCase {
    const char* driver_name;
    std::string_view product_spec;
  };
  constexpr SxxRedirectCase kSxxCases[] = {
      {.driver_name = "S102", .product_spec = "INT.IHO.S-102.2.1"},
      {.driver_name = "S104", .product_spec = "INT.IHO.S-104.1.0"},
      {.driver_name = "S111", .product_spec = "INT.IHO.S-111.1.0"},
  };
  for (const SxxRedirectCase& tc : kSxxCases) {
    ScopedVsimemFile sxx_file(std::format("{}_redirect.h5", tc.driver_name));
    {
      ScopedH5File file_id(CreateVsimemHdf5File(sxx_file.path()));
      ASSERT_GE(file_id.get(), 0);
      ASSERT_TRUE(WriteScalarStringAttr(file_id, "productSpecification",
                                        tc.product_spec, H5T_STR_NULLPAD));
      Create2DIntDataset(file_id, "sxx_band", 4, 4);
    }

    auto sxx_drv = RegisterFakeDriver(tc.driver_name, FakeSxxOpen);
    auto dereg =
        absl::MakeCleanup([&] { GDALDeregisterDriver(sxx_drv.get()); });
    GDALOpenInfo open_info(sxx_file.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    auto redirected =
        std::unique_ptr<GDALDataset>(HDF5Dataset::Open(&open_info));
    EXPECT_NE(nullptr, redirected) << tc.driver_name;
  }
}

TEST(Hdf5DatasetTest, HdfEosGridAndSwathSubdatasetFormatting) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;

  // 1. HDF-EOS5 GRID dataset listing (2D and 3D dimension labels).
  ScopedVsimemFile eos_grid("eos_grid_sds.h5");
  {
    ScopedH5File file_id(CreateVsimemHdf5File(eos_grid.path()));
    ASSERT_GE(file_id.get(), 0);
    ASSERT_TRUE(WriteStructMetadata(file_id, R"(
GROUP=GridStructure
  GROUP=GRID_1
    GridName="MyGrid"
    Projection="HE5_GCTP_GEO"
    GROUP=Dimension
      OBJECT=Dimension_1
        DimensionName="Band"
        Size=3
      END_OBJECT=Dimension_1
      OBJECT=Dimension_2
        DimensionName="YDim"
        Size=10
      END_OBJECT=Dimension_2
      OBJECT=Dimension_3
        DimensionName="XDim"
        Size=20
      END_OBJECT=Dimension_3
    END_GROUP=Dimension
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="Field2D"
        DimList=("YDim","XDim")
      END_OBJECT=DataField_1
      OBJECT=DataField_2
        DataFieldName="Field3D"
        DimList=("Band","YDim","XDim")
      END_OBJECT=DataField_2
      OBJECT=DataField_3
        DataFieldName="Field3DTrailing"
        DimList=("YDim","XDim","Band")
      END_OBJECT=DataField_3
    END_GROUP=DataField
  END_GROUP=GRID_1
)"
    // HDF5Dataset::HDF5ListGroupObjects (hdf5dataset.cpp:1458) looks up grid
    // metadata via GetGridMetadata(pszUnderscorePath) instead of
    // GetGridDataFieldMetadata. GRID_1 ("MyGrid") provides the canonical
    // structure, while synthetic alias grids GRID_2..GRID_4 set GridName to
    // the full "//HDFEOS/GRIDS/..." subdataset path to work around this and
    // exercise 2D and 3D dimension label formatting.
    R"(  GROUP=GRID_2
    GridName="//HDFEOS/GRIDS/MyGrid/Data_Fields/Field3D"
    Projection="HE5_GCTP_GEO"
    GROUP=Dimension
      OBJECT=Dimension_1
        DimensionName="Band"
        Size=3
      END_OBJECT=Dimension_1
      OBJECT=Dimension_2
        DimensionName="YDim"
        Size=10
      END_OBJECT=Dimension_2
      OBJECT=Dimension_3
        DimensionName="XDim"
        Size=20
      END_OBJECT=Dimension_3
    END_GROUP=Dimension
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="Field3D"
        DimList=("Band","YDim","XDim")
      END_OBJECT=DataField_1
    END_GROUP=DataField
  END_GROUP=GRID_2
)"
    // Dimension-group declaration order (not DimList) determines leading vs
    // trailing dimension placement in HDF5ListGroupObjects (hdf5dataset.cpp).
    R"(  GROUP=GRID_3
    GridName="//HDFEOS/GRIDS/MyGrid/Data_Fields/Field3DTrailing"
    Projection="HE5_GCTP_GEO"
    GROUP=Dimension
      OBJECT=Dimension_1
        DimensionName="YDim"
        Size=10
      END_OBJECT=Dimension_1
      OBJECT=Dimension_2
        DimensionName="XDim"
        Size=20
      END_OBJECT=Dimension_2
      OBJECT=Dimension_3
        DimensionName="Band"
        Size=3
      END_OBJECT=Dimension_3
    END_GROUP=Dimension
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="Field3DTrailing"
        DimList=("YDim","XDim","Band")
      END_OBJECT=DataField_1
    END_GROUP=DataField
  END_GROUP=GRID_3
  GROUP=GRID_4
    GridName="//HDFEOS/GRIDS/MyGrid/Data_Fields/Field2D"
    Projection="HE5_GCTP_GEO"
    GROUP=Dimension
      OBJECT=Dimension_1
        DimensionName="YDim"
        Size=10
      END_OBJECT=Dimension_1
      OBJECT=Dimension_2
        DimensionName="XDim"
        Size=20
      END_OBJECT=Dimension_2
    END_GROUP=Dimension
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="Field2D"
        DimList=("YDim","XDim")
      END_OBJECT=DataField_1
    END_GROUP=DataField
  END_GROUP=GRID_4
END_GROUP=GridStructure
END
)"));
    ScopedH5Group g_eos(H5Gcreate(file_id, "HDFEOS", 0));
    ASSERT_GE(g_eos.get(), 0);
    ScopedH5Group g_grids(H5Gcreate(g_eos, "GRIDS", 0));
    ASSERT_GE(g_grids.get(), 0);
    ScopedH5Group g_mygrid(H5Gcreate(g_grids, "MyGrid", 0));
    ASSERT_GE(g_mygrid.get(), 0);
    ScopedH5Group g_df(H5Gcreate(g_mygrid, "Data Fields", 0));
    ASSERT_GE(g_df.get(), 0);
    Create2DIntDataset(g_df, "Field2D", 10, 20);
    Create3DFloatDataset(g_df, "Field3D", 3, 10, 20);
    Create3DFloatDataset(g_df, "Field3DTrailing", 10, 20, 3);
  }
  {
    GDALOpenInfo open_info(eos_grid.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    auto ds = std::unique_ptr<GDALDataset>(HDF5Dataset::Open(&open_info));
    ASSERT_NE(nullptr, ds);
    CSLConstList sds = ds->GetMetadata("SUBDATASETS");
    ASSERT_NE(nullptr, sds);
    EXPECT_EQ(3, CSLCount(sds) / 2);
    EXPECT_STREQ(
        "[(y=10)x(x=20)] //HDFEOS/GRIDS/MyGrid/Data_Fields/Field2D "
        "(16-bit integer)",
        CSLFetchNameValue(sds, "SUBDATASET_1_DESC"));
    EXPECT_STREQ(
        "[(Band=3)x(y=10)x(x=20)] //HDFEOS/GRIDS/MyGrid/Data_Fields/Field3D "
        "(32-bit floating-point)",
        CSLFetchNameValue(sds, "SUBDATASET_2_DESC"));
    EXPECT_STREQ(
        "[(y=10)x(x=20)x(Band=3)] "
        "//HDFEOS/GRIDS/MyGrid/Data_Fields/Field3DTrailing "
        "(32-bit floating-point)",
        CSLFetchNameValue(sds, "SUBDATASET_3_DESC"));
  }

  // 2. HDF-EOS5 SWATH dataset listing (2D and 3D dimension labels).
  ScopedVsimemFile eos_swath("eos_swath_sds.h5");
  {
    ScopedH5File file_id(CreateVsimemHdf5File(eos_swath.path()));
    ASSERT_GE(file_id.get(), 0);
    ASSERT_TRUE(WriteStructMetadata(file_id, R"(
GROUP=SwathStructure
  GROUP=SWATH_1
    SwathName="MySwath"
    GROUP=Dimension
      OBJECT=Dimension_1
        DimensionName="Track"
        Size=8
      END_OBJECT=Dimension_1
      OBJECT=Dimension_2
        DimensionName="Xtrack"
        Size=12
      END_OBJECT=Dimension_2
      OBJECT=Dimension_3
        DimensionName="Channel"
        Size=4
      END_OBJECT=Dimension_3
    END_GROUP=Dimension
    GROUP=GeoField
      OBJECT=GeoField_1
        GeoFieldName="Latitude"
        DimList=("Track","Xtrack")
      END_OBJECT=GeoField_1
      OBJECT=GeoField_2
        GeoFieldName="Longitude"
        DimList=("Track","Xtrack")
      END_OBJECT=GeoField_2
    END_GROUP=GeoField
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="Swath2D"
        DimList=("Track","Xtrack")
      END_OBJECT=DataField_1
      OBJECT=DataField_2
        DataFieldName="Swath3D"
        DimList=("Channel","Track","Xtrack")
      END_OBJECT=DataField_2
      OBJECT=DataField_3
        DataFieldName="Swath3DTrailing"
        DimList=("Track","Xtrack","Channel")
      END_OBJECT=DataField_3
    END_GROUP=DataField
  END_GROUP=SWATH_1
END_GROUP=SwathStructure
END
)"));
    ScopedH5Group g_eos(H5Gcreate(file_id, "HDFEOS", 0));
    ASSERT_GE(g_eos.get(), 0);
    ScopedH5Group g_swaths(H5Gcreate(g_eos, "SWATHS", 0));
    ASSERT_GE(g_swaths.get(), 0);
    ScopedH5Group g_myswath(H5Gcreate(g_swaths, "MySwath", 0));
    ASSERT_GE(g_myswath.get(), 0);
    ScopedH5Group g_df(H5Gcreate(g_myswath, "Data Fields", 0));
    ASSERT_GE(g_df.get(), 0);
    Create2DIntDataset(g_df, "Swath2D", 8, 12);
    Create3DFloatDataset(g_df, "Swath3D", 4, 8, 12);
    Create3DFloatDataset(g_df, "Swath3DTrailing", 8, 12, 4);
  }
  {
    GDALOpenInfo open_info(eos_swath.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    auto ds = std::unique_ptr<GDALDataset>(HDF5Dataset::Open(&open_info));
    ASSERT_NE(nullptr, ds);
    CSLConstList sds = ds->GetMetadata("SUBDATASETS");
    ASSERT_NE(nullptr, sds);
    EXPECT_EQ(3, CSLCount(sds) / 2);
    EXPECT_STREQ(
        "[(Track=8)x(Xtrack=12)] //HDFEOS/SWATHS/MySwath/Data_Fields/Swath2D "
        "(16-bit integer)",
        CSLFetchNameValue(sds, "SUBDATASET_1_DESC"));
    EXPECT_STREQ(
        "[(Channel=4)x(Track=8)x(Xtrack=12)] "
        "//HDFEOS/SWATHS/MySwath/Data_Fields/Swath3D (32-bit floating-point)",
        CSLFetchNameValue(sds, "SUBDATASET_2_DESC"));
    EXPECT_STREQ(
        "[(Track=8)x(Xtrack=12)x(Channel=4)] "
        "//HDFEOS/SWATHS/MySwath/Data_Fields/Swath3DTrailing "
        "(32-bit floating-point)",
        CSLFetchNameValue(sds, "SUBDATASET_3_DESC"));
  }
}

TEST(Hdf5DatasetTest, FamilyDriverHeuristic) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;

  // Synthesize a small POSIX HDF5 'family' split file in TempDir() to exercise
  // GDAL_HDF5Open's family driver fallback when filename ends in 0.h5.
  const std::filesystem::path temp_dir(::testing::TempDir());
  const std::string pattern = (temp_dir / "hdf5_family_%d.h5").string();
  const std::string first_part = (temp_dir / "hdf5_family_0.h5").string();
  auto cleanup_files = absl::MakeCleanup([&temp_dir] {
    for (int i = 0; i < 8; ++i) {
      VSIUnlink((temp_dir / std::format("hdf5_family_{}.h5", i)).c_str());
    }
  });

  {
    ScopedH5PropertyList fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(fapl.get(), 0);
    ASSERT_GE(H5Pset_fapl_family(fapl, 2048, H5P_DEFAULT), 0);
    ScopedH5File file_id(
        H5Fcreate(pattern.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_GE(file_id.get(), 0);
    const hsize_t dims[2] = {32, 32};
    ScopedH5Dataspace sp(H5Screate_simple(2, dims, nullptr));
    ASSERT_GE(sp.get(), 0);
    ScopedH5Dataset dset(
        H5Dcreate(file_id, "family_dset", H5T_NATIVE_INT, sp, H5P_DEFAULT));
    ASSERT_GE(dset.get(), 0);
    std::vector<int> data(32 * 32, 7);
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       data.data()),
              0);
    Create2DIntDataset(file_id, "family_dset_2", 8, 8);
  }

  GDALOpenInfo open_info(first_part.c_str(), GDAL_OF_READONLY);
  auto ds = std::unique_ptr<GDALDataset>(HDF5Dataset::Open(&open_info));
  ASSERT_NE(nullptr, ds);
  EXPECT_NE(nullptr, ds->GetMetadata("SUBDATASETS"));
}

// TODO: b/335317901 - Cover remaining MAX_METADATA_LEN attribute truncation
// branches in HDF5AttrIterate and >INT_MAX dimension rejection in
// HDF5Dataset::HDF5ListGroupObjects.

}  // namespace
}  // namespace autotest2
