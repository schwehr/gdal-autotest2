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
// Tests for the Zarr raster format driver (zarrdriver.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarrdriver.cpp
//   https://gdal.org/drivers/raster/zarr.html

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>  // NOLINT
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/frmts/zarr/zarr.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::NotNull;

class ZarrDriverTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_Zarr(); }
};

// 1. Identify, driver registration, and corrupt input
TEST_F(ZarrDriverTest, IdentifyAndDriverRegistration) {
  // Idempotent registration
  GDALRegister_Zarr();

  // IdentifyDoesNotExist
  EXPECT_EQ(nullptr, GDALDataset::Open("/vsimem/zarr_does_not_exist",
                                       GDAL_OF_RASTER | GDAL_OF_READONLY));

  const ScopedTestDir test_dir("zarr_drv_ident");

  // Corrupt .zarray JSON
  WriteVsiFile(test_dir.File("corrupt/.zarray"), "{invalid json");
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALDataset::Open(test_dir.File("corrupt").c_str(),
                                         GDAL_OF_RASTER));
  }

  // Corrupt .zgroup JSON
  WriteVsiFile(test_dir.File("corrupt_grp/.zgroup"), "{invalid json");
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALDataset::Open(test_dir.File("corrupt_grp").c_str(),
                                         GDAL_OF_RASTER));
  }

  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_THAT(drv, NotNull());
  EXPECT_STREQ("Zarr", drv->GetDescription());
}

// 2. Driver metadata
TEST_F(ZarrDriverTest, DriverMetadata) {
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_THAT(drv, NotNull());

  EXPECT_THAT(drv->GetMetadataItem("COMPRESSORS", nullptr), NotNull());
  // Note: FILTERS is populated lazily by InitMetadata() when COMPRESSORS is
  // queried.
  EXPECT_THAT(drv->GetMetadataItem("FILTERS", nullptr), NotNull());
  EXPECT_THAT(drv->GetMetadataItem("BLOSC_COMPRESSORS", nullptr), NotNull());

  const char* dmd_opts =
      drv->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST, nullptr);
  ASSERT_THAT(dmd_opts, NotNull());
  EXPECT_THAT(dmd_opts, HasSubstr("CreationOptionList"));
  EXPECT_THAT(dmd_opts, HasSubstr("COMPRESS"));
  EXPECT_THAT(dmd_opts, HasSubstr("ARRAY_NAME"));
  EXPECT_THAT(dmd_opts, HasSubstr("CONVERT_TO_KERCHUNK_PARQUET_REFERENCE"));

  const char* mdim_opts =
      drv->GetMetadataItem(GDAL_DMD_MULTIDIM_ARRAY_CREATIONOPTIONLIST, nullptr);
  ASSERT_THAT(mdim_opts, NotNull());
  EXPECT_THAT(mdim_opts, HasSubstr("MultiDimArrayCreationOptionList"));

  char** md = drv->GetMetadata(nullptr);
  ASSERT_THAT(md, NotNull());
}

// 3. Create parameter validation and CleanupCreatedFiles rollback
TEST_F(ZarrDriverTest, CreateValidationAndRollbacks) {
  const ScopedTestDir test_dir("zarr_drv_create");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_THAT(drv, NotNull());

  // Dimensions <= 0
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, drv->Create(test_dir.File("inv_x").c_str(), 0, 4, 1,
                                   GDT_Byte, nullptr));
    EXPECT_EQ(nullptr, drv->Create(test_dir.File("inv_y").c_str(), 4, 0, 1,
                                   GDT_Byte, nullptr));
    EXPECT_EQ(nullptr, drv->Create(test_dir.File("inv_b").c_str(), 4, 4, 0,
                                   GDT_Byte, nullptr));
  }

  // Target already exists as a file
  WriteVsiFile(test_dir.File("exist_file"), "content");
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, drv->Create(test_dir.File("exist_file").c_str(), 4, 4, 1,
                                   GDT_Byte, nullptr));
  }

  // Target already exists as a non-empty directory
  VSIMkdir(test_dir.File("exist_dir").c_str(), 0755);
  WriteVsiFile(test_dir.File("exist_dir/dummy"), "x");
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, drv->Create(test_dir.File("exist_dir").c_str(), 4, 4, 1,
                                   GDT_Byte, nullptr));
  }

  // APPEND_SUBDATASET=YES without ARRAY_NAME
  {
    const char* const no_arr[] = {"APPEND_SUBDATASET=YES", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, drv->Create(test_dir.File("app_no_arr").c_str(), 4, 4, 1,
                                   GDT_Byte, const_cast<char**>(no_arr)));
  }

  // APPEND_SUBDATASET=YES on missing store
  {
    const char* const app_missing[] = {"APPEND_SUBDATASET=YES",
                                       "ARRAY_NAME=sub", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              drv->Create(test_dir.File("app_missing").c_str(), 4, 4, 1,
                          GDT_Byte, const_cast<char**>(app_missing)));
  }

  // CleanupCreatedFiles rollback on V2 default
  {
    const char* const bad_comp[] = {"COMPRESS=INVALID_COMP_NAME", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, drv->Create(test_dir.File("fail_v2").c_str(), 4, 4, 1,
                                   GDT_Byte, const_cast<char**>(bad_comp)));
    VSIStatBufL sStat;
    EXPECT_NE(0, VSIStatL(test_dir.File("fail_v2").c_str(), &sStat));
  }

  // CleanupCreatedFiles rollback on V2 with ARRAY_NAME
  {
    const char* const bad_comp_arr[] = {"COMPRESS=INVALID_COMP_NAME",
                                        "ARRAY_NAME=custom_name", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              drv->Create(test_dir.File("fail_v2_arr").c_str(), 4, 4, 1,
                          GDT_Byte, const_cast<char**>(bad_comp_arr)));
    VSIStatBufL sStat;
    EXPECT_NE(0, VSIStatL(test_dir.File("fail_v2_arr").c_str(), &sStat));
  }

  // CleanupCreatedFiles rollback on V3
  {
    const char* const bad_comp_v3[] = {"FORMAT=ZARR_V3",
                                       "COMPRESS=INVALID_COMP_NAME", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, drv->Create(test_dir.File("fail_v3").c_str(), 4, 4, 1,
                                   GDT_Byte, const_cast<char**>(bad_comp_v3)));
    VSIStatBufL sStat;
    EXPECT_NE(0, VSIStatL(test_dir.File("fail_v3").c_str(), &sStat));
  }

  // CleanupCreatedFiles rollback on 3-band SINGLE_ARRAY=YES
  {
    const char* const bad_single[] = {"SINGLE_ARRAY=YES",
                                      "COMPRESS=INVALID_COMP_NAME", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              drv->Create(test_dir.File("fail_single").c_str(), 4, 4, 3,
                          GDT_Byte, const_cast<char**>(bad_single)));
  }

  // CleanupCreatedFiles rollback on APPEND_SUBDATASET=YES
  {
    const std::string app_base = test_dir.File("app_base");
    GDALDatasetUniquePtr base_ds(
        drv->Create(app_base.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_THAT(base_ds, NotNull());
    base_ds.reset();

    const char* const bad_app[] = {"APPEND_SUBDATASET=YES",
                                   "ARRAY_NAME=sub_bad",
                                   "COMPRESS=INVALID_COMP_NAME", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, drv->Create(app_base.c_str(), 4, 4, 1, GDT_Byte,
                                   const_cast<char**>(bad_app)));
  }
}

// 4. 1-Band Raster Basics, ZarrDataset and ZarrRasterBand
TEST_F(ZarrDriverTest, OneBandRasterBasicsAndBandProperties) {
  const ScopedTestDir test_dir("zarr_drv_1band");
  const std::string root_dir = test_dir.File("ds_1b");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_THAT(drv, NotNull());

  const char* const create_opts[] = {"BLOCKSIZE=4,4", nullptr};
  GDALDatasetUniquePtr ds(drv->Create(root_dir.c_str(), 6, 5, 1, GDT_Byte,
                                      const_cast<char**>(create_opts)));
  ASSERT_THAT(ds, NotNull());

  // GeoTransform before set
  GDALGeoTransform gt{};
  EXPECT_EQ(CE_Failure, ds->GetGeoTransform(gt));

  // Rotated GeoTransform error
  const GDALGeoTransform rot_gt{0.0, 1.0, 0.5, 0.0, 0.0, -1.0};
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ds->SetGeoTransform(rot_gt));
  }

  // Valid GeoTransform
  const GDALGeoTransform valid_gt{100.0, 2.0, 0.0, 200.0, 0.0, -2.0};
  EXPECT_EQ(CE_None, ds->SetGeoTransform(valid_gt));
  // Call SetGeoTransform again to hit re-opening existing X/Y arrays
  EXPECT_EQ(CE_None, ds->SetGeoTransform(valid_gt));

  EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
  EXPECT_DOUBLE_EQ(100.0, gt[0]);
  EXPECT_DOUBLE_EQ(2.0, gt[1]);
  EXPECT_DOUBLE_EQ(0.0, gt[2]);
  EXPECT_DOUBLE_EQ(200.0, gt[3]);
  EXPECT_DOUBLE_EQ(0.0, gt[4]);
  EXPECT_DOUBLE_EQ(-2.0, gt[5]);

  EXPECT_EQ(0, ds->GetGCPCount());
  EXPECT_EQ(6, ds->GetRasterXSize());
  EXPECT_EQ(5, ds->GetRasterYSize());
  EXPECT_EQ(1, ds->GetRasterCount());

  // SpatialReference
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  EXPECT_EQ(CE_None, ds->SetSpatialRef(&srs));
  auto cur_srs = ds->GetSpatialRef();
  ASSERT_THAT(cur_srs, NotNull());
  EXPECT_THAT(*cur_srs, IsSameAs(srs));

  // Metadata
  const char* const meta[] = {"KEY=VALUE", nullptr};
  EXPECT_EQ(CE_None, ds->SetMetadata(const_cast<char**>(meta), ""));
  EXPECT_EQ(CE_None, ds->SetMetadata(const_cast<char**>(meta), "CUSTOM"));

  // RasterBand operations
  auto band = ds->GetRasterBand(1);
  ASSERT_THAT(band, NotNull());

  int bx = 0;
  int by = 0;
  band->GetBlockSize(&bx, &by);
  EXPECT_EQ(4, bx);
  EXPECT_EQ(4, by);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());
  EXPECT_EQ(0, band->GetOverviewCount());
  EXPECT_EQ(nullptr, band->GetColorTable());

  // NoData
  int has_nd = 0;
  band->GetNoDataValue(&has_nd);
  EXPECT_EQ(CE_None, band->SetNoDataValue(255.0));
  EXPECT_DOUBLE_EQ(255.0, band->GetNoDataValue(&has_nd));
  EXPECT_TRUE(has_nd);

  // Offset & Scale
  int has_val = 0;
  EXPECT_EQ(CE_None, band->SetOffset(10.0));
  EXPECT_DOUBLE_EQ(10.0, band->GetOffset(&has_val));
  EXPECT_TRUE(has_val);

  EXPECT_EQ(CE_None, band->SetScale(2.5));
  EXPECT_DOUBLE_EQ(2.5, band->GetScale(&has_val));
  EXPECT_TRUE(has_val);

  // UnitType
  EXPECT_EQ(CE_None, band->SetUnitType("meters"));
  EXPECT_STREQ("meters", band->GetUnitType());
  EXPECT_EQ(CE_None, band->SetUnitType(nullptr));
  EXPECT_STREQ("", band->GetUnitType());

  // ColorInterpretation
  EXPECT_EQ(CE_None, band->SetColorInterpretation(GCI_GrayIndex));
  EXPECT_EQ(GCI_GrayIndex, band->GetColorInterpretation());
  // Call again when attribute already exists
  EXPECT_EQ(CE_None, band->SetColorInterpretation(GCI_GrayIndex));

  // Block I/O: full block (0,0) and partial edge block (1,1)
  std::vector<uint8_t> block_data(16, 42);
  EXPECT_EQ(CE_None, band->WriteBlock(0, 0, block_data.data()));
  EXPECT_EQ(CE_None, band->WriteBlock(1, 1, block_data.data()));

  EXPECT_EQ(CE_None, ds->FlushCache(false));
  EXPECT_EQ(CE_None, ds->DropCache());

  std::vector<uint8_t> r_block(16, 0);
  EXPECT_EQ(CE_None, band->ReadBlock(0, 0, r_block.data()));
  EXPECT_EQ(42, r_block[0]);

  // ComputeRasterMinMax
  double minmax[2] = {0.0, 0.0};
  EXPECT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax));

  // 1:1 RasterIO native and converted
  uint8_t p1 = 99;
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 1, 1, &p1, 1, 1, GDT_Byte,
                                    0, 0, nullptr));
  uint8_t p1_r = 0;
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 1, 1, &p1_r, 1, 1, GDT_Byte,
                                    0, 0, nullptr));
  EXPECT_EQ(99, p1_r);

  int32_t p1_i32 = 120;
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 1, 0, 1, 1, &p1_i32, 1, 1,
                                    GDT_Int32, 0, 0, nullptr));
  int32_t p1_i32_r = 0;
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 1, 0, 1, 1, &p1_i32_r, 1, 1,
                                    GDT_Int32, 0, 0, nullptr));
  EXPECT_EQ(120, p1_i32_r);

  // Resampled RasterIO fallback
  std::vector<uint8_t> w_2x2 = {1, 2, 3, 4};
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 4, 4, w_2x2.data(), 2, 2,
                                    GDT_Byte, 0, 0, nullptr));
  std::vector<uint8_t> r_3x2(6, 0);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 6, 5, r_3x2.data(), 3, 2,
                                    GDT_Byte, 0, 0, nullptr));

  // Close and reopen read-only
  ds.reset();
  ds.reset(
      GDALDataset::Open(root_dir.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(1, ds->GetRasterCount());
  // Subdatasets are suppressed because X and Y indexing vars match
  EXPECT_EQ(nullptr, ds->GetMetadata("SUBDATASETS"));
  EXPECT_THAT(ds->GetSpatialRef(), NotNull());
  EXPECT_STREQ("VALUE", ds->GetMetadataItem("KEY", ""));
  GDALGeoTransform reopened_gt{};
  EXPECT_EQ(CE_None, ds->GetGeoTransform(reopened_gt));
  EXPECT_DOUBLE_EQ(100.0, reopened_gt[0]);
  EXPECT_EQ(GCI_GrayIndex, ds->GetRasterBand(1)->GetColorInterpretation());

  // Int64 and UInt64 NoData tests
  const std::string i64_dir = test_dir.File("ds_i64");
  GDALDatasetUniquePtr ds_i64(
      drv->Create(i64_dir.c_str(), 2, 2, 1, GDT_Int64, nullptr));
  ASSERT_THAT(ds_i64, NotNull());
  auto b_i64 = ds_i64->GetRasterBand(1);
  ASSERT_THAT(b_i64, NotNull());
  EXPECT_EQ(CE_None, b_i64->SetNoDataValueAsInt64(
                         static_cast<int64_t>(-9876543210123LL)));
  int has_nd64 = 0;
  EXPECT_EQ(static_cast<int64_t>(-9876543210123LL),
            b_i64->GetNoDataValueAsInt64(&has_nd64));
  EXPECT_TRUE(has_nd64);

  const std::string u64_dir = test_dir.File("ds_u64");
  GDALDatasetUniquePtr ds_u64(
      drv->Create(u64_dir.c_str(), 2, 2, 1, GDT_UInt64, nullptr));
  ASSERT_THAT(ds_u64, NotNull());
  auto b_u64 = ds_u64->GetRasterBand(1);
  ASSERT_THAT(b_u64, NotNull());
  EXPECT_EQ(CE_None, b_u64->SetNoDataValueAsUInt64(
                         static_cast<uint64_t>(9876543210123ULL)));
  EXPECT_EQ(static_cast<uint64_t>(9876543210123ULL),
            b_u64->GetNoDataValueAsUInt64(&has_nd64));
}

// 5. Multi-Band SINGLE_ARRAY=YES and INTERLEAVE
TEST_F(ZarrDriverTest, MultiBandSingleArrayAndInterleave) {
  const ScopedTestDir test_dir("zarr_drv_multi_single");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_THAT(drv, NotNull());

  // 3-band SINGLE_ARRAY=YES, INTERLEAVE=BAND
  {
    const std::string root_dir = test_dir.File("single_band_order");
    const char* const opts[] = {"SINGLE_ARRAY=YES", "INTERLEAVE=BAND",
                                "BLOCKSIZE=1,2,2", nullptr};
    GDALDatasetUniquePtr ds(drv->Create(root_dir.c_str(), 4, 4, 3, GDT_Byte,
                                        const_cast<char**>(opts)));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(3, ds->GetRasterCount());

    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand));
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand));
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand));

    const char* const meta[] = {"TAG=RGB", nullptr};
    EXPECT_EQ(CE_None, ds->SetMetadata(const_cast<char**>(meta), ""));

    // FlushCache writes the COLOR_INTERPRETATION attribute
    EXPECT_EQ(CE_None, ds->FlushCache(true));
    ds.reset();

    ds.reset(GDALDataset::Open(root_dir.c_str(), GDAL_OF_RASTER));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(3, ds->GetRasterCount());
    EXPECT_STREQ("RGB", ds->GetMetadataItem("TAG", ""));
    EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, ds->GetRasterBand(3)->GetColorInterpretation());
  }

  // 3-band SINGLE_ARRAY=YES, INTERLEAVE=PIXEL, FORMAT=ZARR_V3
  {
    const std::string root_dir = test_dir.File("single_pixel_v3");
    const char* const opts[] = {"FORMAT=ZARR_V3", "SINGLE_ARRAY=YES",
                                "INTERLEAVE=PIXEL", nullptr};
    GDALDatasetUniquePtr ds(drv->Create(root_dir.c_str(), 4, 4, 3, GDT_Byte,
                                        const_cast<char**>(opts)));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(3, ds->GetRasterCount());

    ds.reset();
    ds.reset(GDALDataset::Open(root_dir.c_str(), GDAL_OF_RASTER));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(3, ds->GetRasterCount());
  }
}

// 6. Multi-Band Separate Arrays (SINGLE_ARRAY=NO) and Subdatasets
TEST_F(ZarrDriverTest, MultiBandSeparateArraysAndSubdatasets) {
  const ScopedTestDir test_dir("zarr_drv_subds");
  const std::string root_dir = test_dir.File("ds_sep");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_THAT(drv, NotNull());

  const char* const opts[] = {"SINGLE_ARRAY=NO", nullptr};
  GDALDatasetUniquePtr ds(drv->Create(root_dir.c_str(), 4, 4, 2, GDT_Byte,
                                      const_cast<char**>(opts)));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(2, ds->GetRasterCount());

  const char* const meta[] = {"KEY=SEP", nullptr};
  EXPECT_EQ(CE_None, ds->SetMetadata(const_cast<char**>(meta), ""));
  ds.reset();

  // Re-open returns a ZarrDataset containing SUBDATASETS
  ds.reset(GDALDataset::Open(root_dir.c_str(), GDAL_OF_RASTER));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(0, ds->GetRasterCount());

  char** subds = ds->GetMetadata("SUBDATASETS");
  ASSERT_THAT(subds, NotNull());
  EXPECT_THAT(ds->GetMetadataItem("SUBDATASET_1_NAME", "SUBDATASETS"),
              NotNull());
  EXPECT_EQ(nullptr, ds->GetMetadata(""));
  EXPECT_EQ(nullptr, ds->GetMetadataItem("DUMMY", ""));
  EXPECT_EQ(nullptr, ds->GetSpatialRef());

  GDALGeoTransform gt{};
  EXPECT_EQ(CE_Failure, ds->GetGeoTransform(gt));
  EXPECT_EQ(CE_Failure, ds->SetGeoTransform(gt));

  // Open the first subdataset
  const char* sub1_name =
      ds->GetMetadataItem("SUBDATASET_1_NAME", "SUBDATASETS");
  ASSERT_THAT(sub1_name, NotNull());
  GDALDatasetUniquePtr sub1_ds(GDALDataset::Open(sub1_name, GDAL_OF_RASTER));
  ASSERT_THAT(sub1_ds, NotNull());
  EXPECT_EQ(1, sub1_ds->GetRasterCount());
}

// 7. Append Subdataset
TEST_F(ZarrDriverTest, AppendSubdataset) {
  const ScopedTestDir test_dir("zarr_drv_append");
  const std::string root_dir = test_dir.File("base_store");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_THAT(drv, NotNull());

  GDALDatasetUniquePtr base(
      drv->Create(root_dir.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_THAT(base, NotNull());
  base.reset();

  // Append with matching X/Y dimension size
  const char* const app_match[] = {"APPEND_SUBDATASET=YES",
                                   "ARRAY_NAME=sub_match", nullptr};
  GDALDatasetUniquePtr ds_sub1(drv->Create(root_dir.c_str(), 4, 4, 1, GDT_Byte,
                                           const_cast<char**>(app_match)));
  ASSERT_THAT(ds_sub1, NotNull());
  ds_sub1.reset();

  // Append with differing X/Y dimension size
  const char* const app_diff[] = {"APPEND_SUBDATASET=YES",
                                  "ARRAY_NAME=sub_diff", nullptr};
  GDALDatasetUniquePtr ds_sub2(drv->Create(root_dir.c_str(), 6, 8, 1, GDT_Byte,
                                           const_cast<char**>(app_diff)));
  ASSERT_THAT(ds_sub2, NotNull());
  ds_sub2.reset();

  GDALDatasetUniquePtr reopened(
      GDALDataset::Open(root_dir.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(reopened, NotNull());
  EXPECT_THAT(reopened->GetMetadataItem("SUBDATASET_1_NAME", "SUBDATASETS"),
              NotNull());
  EXPECT_THAT(reopened->GetMetadataItem("SUBDATASET_2_NAME", "SUBDATASETS"),
              NotNull());
  EXPECT_THAT(reopened->GetMetadataItem("SUBDATASET_3_NAME", "SUBDATASETS"),
              NotNull());
}

// 8. Open options, ExploreGroup, and dimension options
TEST_F(ZarrDriverTest, OpenOptionsAndExploreGroup) {
  const ScopedTestDir test_dir("zarr_drv_explore");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_THAT(drv, NotNull());

  // OpenMultidim with trailing '/'
  const std::string mdim_dir = test_dir.File("mdim_store");
  GDALDatasetUniquePtr mdim_ds(
      drv->CreateMultiDimensional(mdim_dir.c_str(), nullptr, nullptr));
  ASSERT_THAT(mdim_ds, NotNull());
  mdim_ds.reset();

  GDALDatasetUniquePtr mdim_open(
      GDALDataset::Open((mdim_dir + "/").c_str(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_THAT(mdim_open, NotNull());
  mdim_open.reset();

  // CreateMultiDimensional with CREATE_ZMETADATA=NO
  const char* const no_zmeta[] = {"CREATE_ZMETADATA=NO", nullptr};
  GDALDatasetUniquePtr no_zmeta_ds(
      drv->CreateMultiDimensional(test_dir.File("no_zmeta").c_str(), nullptr,
                                  const_cast<char**>(no_zmeta)));
  ASSERT_THAT(no_zmeta_ds, NotNull());
  no_zmeta_ds.reset();

  // Subgroup with numeric, string, and compound arrays explored with
  // LIST_ALL_ARRAYS=YES
  const std::string explore_dir = test_dir.File("explore_store");
  GDALDatasetUniquePtr exp_ds(
      drv->CreateMultiDimensional(explore_dir.c_str(), nullptr, nullptr));
  ASSERT_THAT(exp_ds, NotNull());
  auto grp = exp_ds->GetRootGroup();
  ASSERT_THAT(grp, NotNull());
  auto sub = grp->CreateGroup("sub", nullptr);
  ASSERT_THAT(sub, NotNull());

  auto dim4 = sub->CreateDimension("d4", "", "", 4, nullptr);
  ASSERT_THAT(dim4, NotNull());
  ASSERT_THAT(
      sub->CreateMDArray("arr_num", {dim4, dim4},
                         GDALExtendedDataType::Create(GDT_Int32), nullptr),
      NotNull());
  ASSERT_THAT(
      sub->CreateMDArray("arr_str", {dim4},
                         GDALExtendedDataType::CreateString(8), nullptr),
      NotNull());

  std::vector<std::unique_ptr<GDALEDTComponent>> comps;
  comps.push_back(std::make_unique<GDALEDTComponent>(
      "c1", 0, GDALExtendedDataType::Create(GDT_Byte)));
  auto cmp_dt = GDALExtendedDataType::Create("cmp", 1, std::move(comps));
  ASSERT_THAT(sub->CreateMDArray("arr_cmp", {dim4}, cmp_dt, nullptr),
              NotNull());
  exp_ds.reset();

  const char* const list_all_opts[] = {"LIST_ALL_ARRAYS=YES", nullptr};
  GDALDatasetUniquePtr exp_opened(GDALDataset::Open(
      explore_dir.c_str(), GDAL_OF_RASTER, nullptr, list_all_opts));
  ASSERT_THAT(exp_opened, NotNull());
  char** subdatasets = exp_opened->GetMetadata("SUBDATASETS");
  ASSERT_THAT(subdatasets, NotNull());
  const auto subds_vec = CslToVector(subdatasets);
  EXPECT_THAT(subds_vec, Contains(HasSubstr("string type")));
  EXPECT_THAT(subds_vec, Contains(HasSubstr("compound type")));

  // Deep subgroup recursion level limit (nRecCount == 32)
  const std::string deep_dir = test_dir.File("deep_store");
  GDALDatasetUniquePtr deep_ds(
      drv->CreateMultiDimensional(deep_dir.c_str(), nullptr, nullptr));
  ASSERT_THAT(deep_ds, NotNull());
  auto cur_grp = deep_ds->GetRootGroup();
  for (int i = 0; i < 33; ++i) {
    cur_grp = cur_grp->CreateGroup(std::format("sub_{}", i).c_str(), nullptr);
    ASSERT_THAT(cur_grp, NotNull());
  }
  deep_ds.reset();

  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALDataset::Open(deep_dir.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Too deep recursion level in ExploreGroup()"));
  }

  // Empty group (0 arrays)
  const std::string empty_dir = test_dir.File("empty_store");
  GDALDatasetUniquePtr empty_ds(
      drv->CreateMultiDimensional(empty_dir.c_str(), nullptr, nullptr));
  ASSERT_THAT(empty_ds, NotNull());
  empty_ds.reset();
  EXPECT_EQ(nullptr, GDALDataset::Open(empty_dir.c_str(), GDAL_OF_RASTER));

  // DIM_X / DIM_Y options
  const std::string dim_opts_dir = test_dir.File("dim_opts_store");
  GDALDatasetUniquePtr ds_dims(
      drv->CreateMultiDimensional(dim_opts_dir.c_str(), nullptr, nullptr));
  ASSERT_THAT(ds_dims, NotNull());
  auto r_grp = ds_dims->GetRootGroup();
  auto dim_y = r_grp->CreateDimension("my_y", "", "", 4, nullptr);
  auto dim_x = r_grp->CreateDimension("my_x", "", "", 4, nullptr);
  r_grp->CreateMDArray("grid", {dim_y, dim_x},
                       GDALExtendedDataType::Create(GDT_Byte), nullptr);
  ds_dims.reset();

  const char* const dim_by_name[] = {"DIM_X=my_x", "DIM_Y=my_y", nullptr};
  GDALDatasetUniquePtr ds_by_name(GDALDataset::Open(
      dim_opts_dir.c_str(), GDAL_OF_RASTER, nullptr, dim_by_name));
  ASSERT_THAT(ds_by_name, NotNull());

  const char* const dim_by_idx[] = {"DIM_X=1", "DIM_Y=0", nullptr};
  GDALDatasetUniquePtr ds_by_idx(GDALDataset::Open(
      dim_opts_dir.c_str(), GDAL_OF_RASTER, nullptr, dim_by_idx));
  ASSERT_THAT(ds_by_idx, NotNull());

  const char* const dim_invalid[] = {"DIM_X=nonexistent", "DIM_Y=nonexistent",
                                     nullptr};
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds_inv(GDALDataset::Open(
        dim_opts_dir.c_str(), GDAL_OF_RASTER, nullptr, dim_invalid));
    ASSERT_THAT(ds_inv, NotNull());
  }
}

// 9. 3D and 4D Arrays, Slices, and ZARR Connection Syntax
TEST_F(ZarrDriverTest, ThreeDAndFourDArraysAndZarrSyntax) {
  const ScopedTestDir test_dir("zarr_drv_3d_4d");
  const std::string root_dir = test_dir.File("nd_store");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_THAT(drv, NotNull());

  // 1. Store with arr2d
  {
    GDALDatasetUniquePtr ds(
        drv->CreateMultiDimensional(root_dir.c_str(), nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    auto grp = ds->GetRootGroup();
    ASSERT_THAT(grp, NotNull());
    auto dim4 = grp->CreateDimension("d4", "", "", 4, nullptr);
    auto dim5 = grp->CreateDimension("d5", "", "", 5, nullptr);
    grp->CreateMDArray("arr2d", {dim4, dim5},
                       GDALExtendedDataType::Create(GDT_Byte), nullptr);
  }

  // 2. Isolated 3D store (only arr3d) for subdataset generation with
  // MULTIBAND=NO
  const std::string only3d_dir = test_dir.File("only3d_store");
  {
    GDALDatasetUniquePtr ds(
        drv->CreateMultiDimensional(only3d_dir.c_str(), nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    auto grp = ds->GetRootGroup();
    ASSERT_THAT(grp, NotNull());
    auto dim2 = grp->CreateDimension("d2", "", "", 2, nullptr);
    auto dim4 = grp->CreateDimension("d4", "", "", 4, nullptr);
    auto dim5 = grp->CreateDimension("d5", "", "", 5, nullptr);
    grp->CreateMDArray("arr3d", {dim2, dim4, dim5},
                       GDALExtendedDataType::Create(GDT_Byte), nullptr);
  }

  const char* const no_multiband[] = {"MULTIBAND=NO", nullptr};
  GDALDatasetUniquePtr ds_3d(GDALDataset::Open(
      only3d_dir.c_str(), GDAL_OF_RASTER, nullptr, no_multiband));
  ASSERT_THAT(ds_3d, NotNull());
  EXPECT_THAT(ds_3d->GetMetadataItem("SUBDATASET_1_NAME", "SUBDATASETS"),
              NotNull());
  EXPECT_THAT(ds_3d->GetMetadataItem("SUBDATASET_2_NAME", "SUBDATASETS"),
              NotNull());

  // 3. Isolated 4D store (only arr4d) for subdataset generation with
  // MULTIBAND=NO
  const std::string only4d_dir = test_dir.File("only4d_store");
  {
    GDALDatasetUniquePtr ds(
        drv->CreateMultiDimensional(only4d_dir.c_str(), nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    auto grp = ds->GetRootGroup();
    ASSERT_THAT(grp, NotNull());
    auto dim2 = grp->CreateDimension("d2", "", "", 2, nullptr);
    auto dim3 = grp->CreateDimension("d3", "", "", 3, nullptr);
    auto dim4 = grp->CreateDimension("d4", "", "", 4, nullptr);
    auto dim5 = grp->CreateDimension("d5", "", "", 5, nullptr);
    grp->CreateMDArray("arr4d", {dim2, dim3, dim4, dim5},
                       GDALExtendedDataType::Create(GDT_Byte), nullptr);
  }

  GDALDatasetUniquePtr ds_4d(GDALDataset::Open(
      only4d_dir.c_str(), GDAL_OF_RASTER, nullptr, no_multiband));
  ASSERT_THAT(ds_4d, NotNull());
  EXPECT_THAT(ds_4d->GetMetadataItem("SUBDATASET_1_NAME", "SUBDATASETS"),
              NotNull());

  // 4. Open slice directly
  GDALDatasetUniquePtr slice_3d(GDALDataset::Open(
      std::format(R"(ZARR:"{}":/arr3d:1)", only3d_dir).c_str(),
      GDAL_OF_RASTER));
  ASSERT_THAT(slice_3d, NotNull());
  EXPECT_EQ(1, slice_3d->GetRasterCount());

  GDALDatasetUniquePtr slice_4d(GDALDataset::Open(
      std::format(R"(ZARR:"{}":/arr4d:1:2)", only4d_dir).c_str(),
      GDAL_OF_RASTER));
  ASSERT_THAT(slice_4d, NotNull());
  EXPECT_EQ(1, slice_4d->GetRasterCount());

  // 5. Syntax and index errors
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALDataset::Open(
                  std::format(R"(ZARR:"{}":/arr4d:0)", only4d_dir).c_str(),
                  GDAL_OF_RASTER));
    EXPECT_EQ(nullptr,
              GDALDataset::Open(
                  std::format(R"(ZARR:"{}":/arr3d:99)", only3d_dir).c_str(),
                  GDAL_OF_RASTER));
    EXPECT_EQ(nullptr,
              GDALDataset::Open(
                  std::format(R"(ZARR:"{}":/arr2d:0)", root_dir).c_str(),
                  GDAL_OF_RASTER));
    EXPECT_EQ(nullptr,
              GDALDataset::Open(
                  std::format(R"(ZARR:"{}":/arr3d)", only3d_dir).c_str(),
                  GDAL_OF_RASTER, nullptr, no_multiband));
  }

  // 6. Sparse arrays with >65536 samples
  const std::string huge3d_dir = test_dir.File("huge3d_store");
  {
    GDALDatasetUniquePtr huge_ds(
        drv->CreateMultiDimensional(huge3d_dir.c_str(), nullptr, nullptr));
    ASSERT_THAT(huge_ds, NotNull());
    auto h_grp = huge_ds->GetRootGroup();
    auto dim_huge = h_grp->CreateDimension("d_huge", "", "", 70000, nullptr);
    auto dim_1 = h_grp->CreateDimension("d_1", "", "", 1, nullptr);
    const char* const b1_opts[] = {"BLOCKSIZE=1,1,1", nullptr};
    h_grp->CreateMDArray("huge3d", {dim_huge, dim_1, dim_1},
                         GDALExtendedDataType::Create(GDT_Byte), b1_opts);
  }

  const std::string huge4d_dir = test_dir.File("huge4d_store");
  {
    GDALDatasetUniquePtr huge_ds(
        drv->CreateMultiDimensional(huge4d_dir.c_str(), nullptr, nullptr));
    ASSERT_THAT(huge_ds, NotNull());
    auto h_grp = huge_ds->GetRootGroup();
    auto dim_300 = h_grp->CreateDimension("d_300", "", "", 300, nullptr);
    auto dim_1 = h_grp->CreateDimension("d_1", "", "", 1, nullptr);
    const char* const b1_4d_opts[] = {"BLOCKSIZE=1,1,1,1", nullptr};
    h_grp->CreateMDArray("huge4d", {dim_300, dim_300, dim_1, dim_1},
                         GDALExtendedDataType::Create(GDT_Byte), b1_4d_opts);
  }

  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALDataset::Open(
                  std::format(R"(ZARR:"{}":/huge3d)", huge3d_dir).c_str(),
                  GDAL_OF_RASTER));
    EXPECT_EQ(nullptr,
              GDALDataset::Open(
                  std::format(R"(ZARR:"{}":/huge4d)", huge4d_dir).c_str(),
                  GDAL_OF_RASTER));
    GDALDatasetUniquePtr ds_h3(GDALDataset::Open(
        huge3d_dir.c_str(), GDAL_OF_RASTER, nullptr, no_multiband));
    ASSERT_THAT(ds_h3, NotNull());
    EXPECT_EQ(0, ds_h3->GetRasterCount());
    GDALDatasetUniquePtr ds_h4(GDALDataset::Open(
        huge4d_dir.c_str(), GDAL_OF_RASTER, nullptr, no_multiband));
    ASSERT_THAT(ds_h4, NotNull());
    EXPECT_EQ(0, ds_h4->GetRasterCount());
  }

  // 7. Open .zarray file directly
  GDALDatasetUniquePtr direct_zarray(GDALDataset::Open(
      test_dir.File("nd_store/arr2d/.zarray").c_str(), GDAL_OF_RASTER));
  ASSERT_THAT(direct_zarray, NotNull());

  // 8. Root V3 array syntax
  const std::string v3_root = test_dir.File("v3_root_store");
  WriteVsiFile(v3_root + "/zarr.json", R"json({
    "zarr_format": 3,
    "node_type": "array",
    "shape": [4, 5],
    "data_type": "uint8",
    "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4, 5]}},
    "chunk_key_encoding": {"name": "default"},
    "fill_value": 0
  })json");
  GDALDatasetUniquePtr v3_opened(GDALDataset::Open(
      std::format(R"(ZARR:"{}":/)", v3_root).c_str(), GDAL_OF_RASTER));
  ASSERT_THAT(v3_opened, NotNull());
  EXPECT_EQ(5, v3_opened->GetRasterXSize());
  EXPECT_EQ(4, v3_opened->GetRasterYSize());

  // 9. Malformed / HTTP syntax error handling
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALDataset::Open("ZARR:", GDAL_OF_RASTER));
    EXPECT_EQ(nullptr, GDALDataset::Open("ZARR:http://example.com/data.zarr",
                                         GDAL_OF_RASTER));
    EXPECT_EQ(nullptr, GDALDataset::Open("ZARR:https://example.com/data.zarr",
                                         GDAL_OF_RASTER));
    EXPECT_EQ(nullptr,
              GDALDataset::Open("ZARR:/vsicurl/http://example.com/data.zarr",
                                GDAL_OF_RASTER));
    EXPECT_EQ(nullptr,
              GDALDataset::Open("ZARR:\"/vsicurl/http\":/arr", GDAL_OF_RASTER));
  }
}

// 10. Kerchunk, ZARR_DUMMY, and CreateCopy
TEST_F(ZarrDriverTest, KerchunkZarrDummyAndCreateCopy) {
  const ScopedTestDir test_dir("zarr_drv_kc");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_THAT(drv, NotNull());

  // ZARR_DUMMY:
  GDALDatasetUniquePtr dummy(GDALDataset::Open(
      "ZARR_DUMMY:/vsimem/dummy_src", GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(dummy, NotNull());
  EXPECT_EQ(0, dummy->GetRasterXSize());
  EXPECT_EQ(0, dummy->GetRasterYSize());
  EXPECT_STREQ("/vsimem/dummy_src", dummy->GetDescription());

  // Kerchunk V1 JSON reference store
  const std::string kc_json = R"json({
    "version": 1,
    "refs": {
      ".zgroup": "{\"zarr_format\": 2}",
      "data/.zarray": "{\"zarr_format\": 2, \"shape\": [2, 2], \"chunks\": [2, 2], \"dtype\": \"<u1\", \"order\": \"C\", \"fill_value\": 0, \"compressor\": null, \"filters\": null}",
      "data/0.0": "base64:AQIDBA=="
    }
  })json";
  const std::string kc_path = test_dir.File("kc_ref.json");
  WriteVsiFile(kc_path, kc_json);

  // Open directly
  GDALDatasetUniquePtr ds_kc(
      GDALDataset::Open(kc_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(ds_kc, NotNull());
  ds_kc.reset();

  // Open with /vsikerchunk_json_ref/
  GDALDatasetUniquePtr ds_kc_fs(GDALDataset::Open(
      std::format("/vsikerchunk_json_ref/{{{}}}", kc_path).c_str(),
      GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(ds_kc_fs, NotNull());
  ds_kc_fs.reset();

  // Open with CACHE_KERCHUNK_JSON=YES under ScopedFakeParquetDriver
  {
    ScopedFakeParquetDriver parquet_guard;
    const ScopedPosixDir posix_cache("zarr_drv_kc_cache");
    const CPLConfigOptionSetter cache_opt("GDAL_CACHE_DIRECTORY",
                                          posix_cache.path().c_str(), false);
    const CPLConfigOptionSetter test_lock_opt(
        "VSIKERCHUNK_FOR_TESTS", "SHORT_DELAY_STALLED_LOCK", false);

    const std::string fresh_kc_path = test_dir.File("kc_fresh_ref.json");
    WriteVsiFile(fresh_kc_path, kc_json);

    const char* const kc_cache_opts[] = {"CACHE_KERCHUNK_JSON=YES", nullptr};
    GDALDatasetUniquePtr ds_cached(GDALDataset::Open(
        fresh_kc_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
        kc_cache_opts));
    ASSERT_THAT(ds_cached, NotNull());
    ds_cached.reset();
  }

  // CreateCopy with CONVERT_TO_KERCHUNK_PARQUET_REFERENCE=YES
  {
    ScopedFakeParquetDriver parquet_guard;
    GDALDatasetUniquePtr src_ds(
        GDALDataset::Open(kc_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(src_ds, NotNull());

    const std::string parq_path = test_dir.File("kc_parq.parq");
    const char* const conv_opts[] = {
        "CONVERT_TO_KERCHUNK_PARQUET_REFERENCE=YES", nullptr};
    GDALDatasetUniquePtr conv_ds(
        drv->CreateCopy(parq_path.c_str(), src_ds.get(), false,
                        const_cast<char**>(conv_opts), nullptr, nullptr));
    ASSERT_THAT(conv_ds, NotNull());
  }

  // Default CreateCopy on a 1-band dataset
  const std::string src_path = test_dir.File("src_ds");
  GDALDatasetUniquePtr src_ds(
      drv->Create(src_path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_THAT(src_ds, NotNull());
  EXPECT_EQ(CE_None, src_ds->FlushCache(false));

  const std::string copy_path = test_dir.File("copy_ds");
  GDALDatasetUniquePtr copy_ds(drv->CreateCopy(
      copy_path.c_str(), src_ds.get(), false, nullptr, nullptr, nullptr));
  ASSERT_THAT(copy_ds, NotNull());
  EXPECT_EQ(0, copy_ds->GetRasterCount());
  auto copy_rg = copy_ds->GetRootGroup();
  ASSERT_THAT(copy_rg, NotNull());
  auto copy_arr = copy_rg->OpenMDArray("src_ds", nullptr);
  ASSERT_THAT(copy_arr, NotNull());
  EXPECT_EQ(4, copy_arr->GetDimensions()[0]->GetSize());
  EXPECT_EQ(4, copy_arr->GetDimensions()[1]->GetSize());
}

// 11. CopyFiles, Rename, and Delete
TEST_F(ZarrDriverTest, CopyFilesRenameDelete) {
  const ScopedTestDir test_dir("zarr_drv_fileops");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_THAT(drv, NotNull());

  // Operations rejected on "ZARR:" prefix
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, drv->Delete("ZARR:dummy"));
    EXPECT_EQ(CE_Failure, drv->Rename("ZARR:new", "old"));
    EXPECT_EQ(CE_Failure, drv->Rename("new", "ZARR:old"));
    EXPECT_EQ(CE_Failure, drv->CopyFiles("ZARR:new", "old"));
    EXPECT_EQ(CE_Failure, drv->CopyFiles("new", "ZARR:old"));
  }

  // Valid operations on /vsimem/
  const std::string src_dir = test_dir.File("orig_store");
  GDALDatasetUniquePtr ds(
      drv->Create(src_dir.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());
  ds.reset();

  const std::string dst_dir = test_dir.File("copied_store");
  EXPECT_EQ(CE_None, drv->CopyFiles(dst_dir.c_str(), src_dir.c_str()));
  GDALDatasetUniquePtr ds_copied(
      GDALDataset::Open(dst_dir.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(ds_copied, NotNull());
  ds_copied.reset();

  const std::string renamed_dir = test_dir.File("renamed_store");
  EXPECT_EQ(CE_None, drv->Rename(renamed_dir.c_str(), dst_dir.c_str()));
  GDALDatasetUniquePtr ds_renamed(GDALDataset::Open(
      renamed_dir.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(ds_renamed, NotNull());
  ds_renamed.reset();

  EXPECT_EQ(CE_None, drv->Delete(renamed_dir.c_str()));
  VSIStatBufL sStat;
  EXPECT_NE(0, VSIStatL(renamed_dir.c_str(), &sStat));
}

// TODO: b/335317901 - Cover remaining Zarr driver corner cases.

}  // namespace
}  // namespace autotest2
