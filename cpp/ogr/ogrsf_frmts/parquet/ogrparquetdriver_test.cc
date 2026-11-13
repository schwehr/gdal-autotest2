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
// Unit tests for ogrparquetdriver.cpp (OGRParquetDriver, open, create, update,
// editable layer synchronization, and row-group extent debug visualization).
//
// Build caveat in google3: GDAL_USE_ARROWDATASET and GDAL_USE_ARROWCOMPUTE are
// disabled in //third_party/gdal/BUILD, so Arrow dataset / HIVE partitioning
// helpers in ogrparquetdriver.cpp are compiled out.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/parquet/ogrparquetdriver.cpp
//   https://gdal.org/drivers/vector/parquet.html

#include <filesystem>  // NOLINT
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/ogr/ogrsf_frmts/parquet/parquet_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogrparquetdriver_test";

class OgrParquetDriverTest : public ::testing::Test {
 protected:
  void SetUp() override { RegisterOGRParquet(); }
};

TEST_F(OgrParquetDriverTest, RegistrationAndDriverMetadata) {
  // Re-registering is idempotent.
  RegisterOGRParquet();

  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
  ASSERT_NE(nullptr, drv);
  EXPECT_NE(nullptr, drv->GetMetadataItem("ARROW_VERSION"));
  EXPECT_EQ(nullptr, drv->GetMetadataItem("ARROW_DATASET"));

  const char* lco = drv->GetMetadataItem(GDAL_DS_LAYER_CREATIONOPTIONLIST);
  ASSERT_NE(nullptr, lco);
  EXPECT_THAT(lco, HasSubstr("COMPRESSION"));
  EXPECT_THAT(lco, HasSubstr("COMPRESSION_LEVEL"));
  EXPECT_THAT(lco, HasSubstr("GEOMETRY_ENCODING"));
  EXPECT_THAT(lco, HasSubstr("ROW_GROUP_SIZE"));
  EXPECT_THAT(lco, HasSubstr("USE_PARQUET_GEO_TYPES"));
  EXPECT_THAT(lco, HasSubstr("SORT_BY_BBOX"));

  CSLConstList md = drv->GetMetadata("");
  EXPECT_GT(CSLCount(md), 0);
}

TEST_F(OgrParquetDriverTest, CreateAndOpenErrorPaths) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  VSIMkdirRecursive(kTestDir, 0755);

  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
  ASSERT_NE(nullptr, drv);

  // 1. Create with non-zero raster dimensions fails cleanly.
  EXPECT_EQ(nullptr, drv->Create("/vsimem/ogrparquetdriver_test/r.parquet", 1,
                                 1, 1, GDT_Byte, nullptr));

  // 2. Opening non-existent path returns nullptr.
  EXPECT_EQ(nullptr, OpenParquetDataset("/does_not_exist.parquet"));

  // 3. Opening a valid POSIX file via TempDir exercises non-/vsimem/ branch.
  const std::string posix_path =
      (std::filesystem::path(::testing::TempDir()) / "driver_posix.parquet")
          .string();
  absl::Cleanup posix_cleanup = [&posix_path] {
    std::error_code ec;
    std::filesystem::remove(posix_path, ec);
  };
  {
    CPLConfigOptionSetter no_vsi("OGR_PARQUET_USE_VSI", "NO", false);
    ASSERT_TRUE(CreateSampleParquetViaGdal(posix_path.c_str(), /*row_count=*/2,
                                           /*row_group_size=*/1));
    auto ro_ds = OpenParquetDataset(posix_path.c_str());
    ASSERT_NE(nullptr, ro_ds);
    EXPECT_EQ(2, ro_ds->GetLayer(0)->GetFeatureCount(/*bForce=*/1));
  }

  // 4. PARQUET: prefix strips prefix cleanly for valid file.
  {
    const std::string prefixed = "PARQUET:" + posix_path;
    auto prefixed_ds = OpenParquetDataset(prefixed.c_str());
    ASSERT_NE(nullptr, prefixed_ds);
    EXPECT_EQ(2, prefixed_ds->GetLayer(0)->GetFeatureCount(/*bForce=*/1));
  }

  // 5. Creating at a non-existent POSIX directory path when
  // OGR_PARQUET_USE_VSI=NO fails cleanly.
  {
    const std::string missing_posix =
        (std::filesystem::path(::testing::TempDir()) / "missing_sub" /
         "bad.parquet")
            .string();
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLConfigOptionSetter no_vsi("OGR_PARQUET_USE_VSI", "NO", false);
    EXPECT_EQ(nullptr, drv->Create(missing_posix.c_str(), 0, 0, 0, GDT_Unknown,
                                   nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(OgrParquetDriverTest, UpdateEditableLayerSyncToDiskAndFilters) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] = "/vsimem/ogrparquetdriver_test/editable.parquet";
  {
    VSIMkdirRecursive(kTestDir, 0755);
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
    ASSERT_NE(nullptr, drv);
    GDALDatasetUniquePtr wr(drv->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, wr);
    OGRSpatialReference srs;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srs.importFromEPSG(32631);
    CPLStringList opts;
    opts.SetNameValue("FID", "my_fid");
    opts.SetNameValue("GEOMETRY_NAME", "my_geom");
    opts.SetNameValue("EDGES", "SPHERICAL");
    opts.SetNameValue("ROW_GROUP_SIZE", "1");
    OGRLayer* lyr = wr->CreateLayer("edit_lyr", &srs, wkbPoint, opts.List());
    ASSERT_NE(nullptr, lyr);
    OGRFieldDefn str_fld("name", OFTString);
    OGRFieldDefn int_fld("num", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&str_fld));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&int_fld));

    OGRFeature feat(lyr->GetLayerDefn());
    feat.SetFID(10);
    feat.SetField(0, "alpha");
    feat.SetField(1, 100);
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    feat.SetGeometryDirectly(pt.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));

    // Additional feature outside both attribute (num > 0) and spatial filters
    // so filter save/restore across SyncToDisk() is observable.
    OGRFeature outside(lyr->GetLayerDefn());
    outside.SetFID(5);
    outside.SetField(0, "zero");
    outside.SetField(1, -5);
    auto pt_out = std::make_unique<OGRPoint>(50.0, 50.0);
    outside.SetGeometryDirectly(pt_out.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&outside));

    EXPECT_EQ(CE_None, wr->Close());
  }

  // Open in update mode with active attribute + spatial filters during
  // SyncToDisk to cover filter save/restore in
  // OGRParquetEditableLayerSynchronizer.
  {
    auto upd_ds = OpenParquetDataset(kPath, GDAL_OF_VECTOR | GDAL_OF_UPDATE);
    ASSERT_NE(nullptr, upd_ds);
    OGRLayer* lyr = upd_ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_STREQ("my_fid", lyr->GetFIDColumn());
    EXPECT_STREQ("my_geom", lyr->GetGeometryColumn());
    EXPECT_STREQ("SPHERICAL", lyr->GetMetadataItem("EDGES"));
    EXPECT_EQ(TRUE, lyr->TestCapability(OLCSequentialWrite));
    EXPECT_EQ(TRUE, lyr->TestCapability(OLCRandomWrite));
    EXPECT_EQ(TRUE, lyr->TestCapability(OLCCreateField));
    EXPECT_EQ(TRUE, lyr->TestCapability(OLCDeleteField));

    lyr->SetAttributeFilter("num > 0");
    lyr->SetSpatialFilterRect(0.0, 0.0, 10.0, 10.0);

    OGRFieldDefn real_fld("score", OFTReal);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&real_fld));

    std::unique_ptr<OGRFeature> f0(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f0);
    f0->SetField("score", 9.5);
    ASSERT_EQ(OGRERR_NONE, lyr->SetFeature(f0.get()));

    OGRFeature f1(lyr->GetLayerDefn());
    f1.SetFID(20);
    f1.SetField("name", "beta");
    f1.SetField("num", 200);
    f1.SetField("score", 4.5);
    auto pt2 = std::make_unique<OGRPoint>(3.0, 4.0);
    f1.SetGeometryDirectly(pt2.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&f1));

    // DeleteFeature path too.
    OGRFeature f2(lyr->GetLayerDefn());
    f2.SetFID(30);
    f2.SetField("name", "gamma");
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&f2));
    ASSERT_EQ(OGRERR_NONE, lyr->DeleteFeature(30));

    EXPECT_EQ(OGRERR_NONE, lyr->SyncToDisk());
    // Both attribute and spatial filters remain restored (FID 5 excluded).
    EXPECT_EQ(2, lyr->GetFeatureCount(/*bForce=*/1));
    EXPECT_EQ(CE_None, upd_ds->Close());
  }

  // Reopen read-only and verify all 3 persisted features (5, 10, 20) & options.
  {
    auto ro_ds = OpenParquetDataset(kPath);
    ASSERT_NE(nullptr, ro_ds);
    OGRLayer* lyr = ro_ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_STREQ("my_fid", lyr->GetFIDColumn());
    EXPECT_STREQ("my_geom", lyr->GetGeometryColumn());
    EXPECT_STREQ("SPHERICAL", lyr->GetMetadataItem("EDGES"));
    EXPECT_STREQ("3", lyr->GetMetadataItem("NUM_ROW_GROUPS", "_PARQUET_"));
    EXPECT_EQ(3, lyr->GetFeatureCount(/*bForce=*/1));

    std::unique_ptr<OGRFeature> f0(lyr->GetFeature(10));
    ASSERT_NE(nullptr, f0);
    EXPECT_DOUBLE_EQ(9.5, f0->GetFieldAsDouble("score"));
    std::unique_ptr<OGRFeature> f_out(lyr->GetFeature(5));
    ASSERT_NE(nullptr, f_out);
    EXPECT_EQ(-5, f_out->GetFieldAsInteger("num"));
    std::unique_ptr<OGRFeature> f20(lyr->GetFeature(20));
    ASSERT_NE(nullptr, f20);
    EXPECT_STREQ("beta", f20->GetFieldAsString("name"));
  }
}

TEST_F(OgrParquetDriverTest, ShowRowGroupExtentAndFactoryConfig) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] = "/vsimem/ogrparquetdriver_test/rg_extent.parquet";
  ASSERT_TRUE(CreateSampleParquetViaGdal(kPath, /*row_count=*/4,
                                         /*row_group_size=*/2));

  {
    CPLConfigOptionSetter show_rg("OGR_PARQUET_SHOW_ROW_GROUP_EXTENT", "YES",
                                  false);
    auto ds = OpenParquetDataset(kPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_STREQ("footprint", lyr->GetName());
    EXPECT_EQ(2, lyr->GetFeatureCount(/*bForce=*/1));
    std::unique_ptr<OGRFeature> f0(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f0);
    EXPECT_EQ(2, f0->GetFieldAsInteger64("feature_count"));
    ASSERT_NE(nullptr, f0->GetGeometryRef());
    EXPECT_EQ(wkbPolygon, wkbFlatten(f0->GetGeometryRef()->getGeometryType()));
  }

  // Non-geometry file returns nullptr when SHOW_ROW_GROUP_EXTENT=YES.
  constexpr char kNoGeom[] = "/vsimem/ogrparquetdriver_test/no_geom.parquet";
  ASSERT_TRUE(CreateSampleParquetViaGdal(kNoGeom, /*row_count=*/2,
                                         /*row_group_size=*/2,
                                         /*with_fid_column=*/false,
                                         /*with_geom=*/false));
  {
    CPLConfigOptionSetter show_rg("OGR_PARQUET_SHOW_ROW_GROUP_EXTENT", "YES",
                                  false);
    EXPECT_EQ(nullptr, OpenParquetDataset(kNoGeom));
  }

  // Exercise invalid OGR_PARQUET_LOAD_FILE_SYSTEM_FACTORIES warning path on
  // fresh driver registration.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLConfigOptionSetter fs_fact("OGR_PARQUET_LOAD_FILE_SYSTEM_FACTORIES",
                                  "/does_not_exist.so", false);
    auto* mgr = GetGDALDriverManager();
    GDALDriver* saved = mgr->GetDriverByName("Parquet");
    mgr->DeregisterDriver(saved);
    delete saved;
    RegisterOGRParquet();
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("LoadFileSystemFactories"));
    EXPECT_NE(nullptr, mgr->GetDriverByName("Parquet"));
    CPLErrorReset();
  }
}

// TODO: b/335317901 - Cover OGRParquetEditableLayerSynchronizer backup/rename
// failure paths and GDAL_USE_ARROWDATASET dataset-open branches if enabled.

}  // namespace
}  // namespace autotest2
