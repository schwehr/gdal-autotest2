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

// Unit tests for GDALGeoPackageDataset.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gpkg/ogrgeopackagedatasource.cpp
//   https://gdal.org/drivers/vector/gpkg.html
//   https://gdal.org/drivers/raster/gpkg.html

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpkg/gpkgmbtilescommon.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpkg/ogr_geopackage.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpkg/ogrgeopackageutility.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/sqlite/sqlite3.h"

namespace autotest2 {
namespace {

class GDALGeoPackageDatasetTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    RegisterOGRGeoPackage();
    GDALRegister_MEM();
    GDALRegister_PNG();
    GDALRegister_JPEG();
    GDALRegister_WEBP();
    GDALRegister_GTiff();
    GDALRegister_VRT();
  }

  static void TearDownTestSuite() { GDALDestroyDriverManager(); }
};

// ---------------------------------------------------------------------------
// 1. Dataset Creation, Versions, Open Options, and Capabilities
// ---------------------------------------------------------------------------
TEST_F(GDALGeoPackageDatasetTest, DatasetCreationVersionsAndCapabilities) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  const char* const versions[] = {"1.0", "1.1", "1.2", "1.3", "1.4", "AUTO"};
  for (const char* ver : versions) {
    const std::string path = absl::StrCat("/vsimem/test_ver_", ver, ".gpkg");
    absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

    const char* const opts[] = {CPLSPrintf("VERSION=%s", ver),
                                "ADD_GPKG_OGR_CONTENTS=YES",
                                "DATETIME_FORMAT=WITH_TZ", nullptr};
    auto ds = std::unique_ptr<GDALDataset>(drv->Create(
        path.c_str(), 0, 0, 0, GDT_Unknown, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);

    auto* gpkg_ds = dynamic_cast<GDALGeoPackageDataset*>(ds.get());
    ASSERT_NE(nullptr, gpkg_ds);
    EXPECT_NE(nullptr, gpkg_ds->GetDB());

    // Capabilities test
    EXPECT_TRUE(ds->TestCapability(ODsCCreateLayer));
    EXPECT_TRUE(ds->TestCapability(ODsCDeleteLayer));
    EXPECT_FALSE(ds->TestCapability(ODsCCreateGeomFieldAfterCreateLayer));
    EXPECT_TRUE(ds->TestCapability(ODsCCurveGeometries));
    EXPECT_TRUE(ds->TestCapability(ODsCTransactions));
    EXPECT_FALSE(ds->TestCapability(ODsCEmulatedTransactions));
    EXPECT_TRUE(ds->TestCapability(ODsCRandomLayerWrite));
    EXPECT_TRUE(ds->TestCapability(ODsCMeasuredGeometries));
    EXPECT_TRUE(ds->TestCapability(ODsCZGeometries));
    EXPECT_TRUE(ds->TestCapability(ODsCAddFieldDomain));
    EXPECT_TRUE(ds->TestCapability(GDsCAddRelationship));
    EXPECT_TRUE(ds->TestCapability(GDsCDeleteRelationship));
    EXPECT_TRUE(ds->TestCapability(GDsCUpdateRelationship));
    EXPECT_TRUE(ds->TestCapability("RenameLayer"));
    EXPECT_FALSE(ds->TestCapability("UNKNOWN_CAP"));

    char** file_list = ds->GetFileList();
    EXPECT_NE(nullptr, file_list);
    CSLDestroy(file_list);

    EXPECT_EQ(CE_None, ds->FlushCache(false));
    EXPECT_EQ(CE_None, ds->FlushCache(true));
  }
}

TEST_F(GDALGeoPackageDatasetTest, OpenOptionsAndZip) {
  const std::string path = "/vsimem/test_open_opts.gpkg";
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  {
    auto ds = std::unique_ptr<GDALDataset>(
        drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    OGRLayer* ly = ds->CreateLayer("lyr1", &srs, wkbPoint, nullptr);
    ASSERT_NE(nullptr, ly);
    OGRFeature f(ly->GetLayerDefn());
    OGRPoint pt(10, 20);
    f.SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, ly->CreateFeature(&f));
    ASSERT_EQ(OGRERR_NONE, ly->SyncToDisk());
  }
  absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

  // Test opening with diverse options
  const char* const open_opts[] = {
      "LIST_ALL_TABLES=YES", "NOLOCK=YES", "IMMUTABLE=YES",
      "PRELUDE_STATEMENTS=PRAGMA cache_size=1000;", nullptr};
  auto opened = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR, nullptr,
                        const_cast<char**>(open_opts), nullptr));
  ASSERT_NE(nullptr, opened);
  EXPECT_EQ(1, opened->GetLayerCount());

  // Test CloseDB and ReOpenDB
  auto* gds = dynamic_cast<GDALGeoPackageDataset*>(opened.get());
  ASSERT_NE(nullptr, gds);
  EXPECT_NE(nullptr, gds->GetDB());

  // Test Close
  EXPECT_EQ(CE_None, opened->Close());
}

// ---------------------------------------------------------------------------
// 2. Spatial Reference System Management
// ---------------------------------------------------------------------------
TEST_F(GDALGeoPackageDatasetTest, SpatialReferenceManagement) {
  const std::string path = "/vsimem/test_srs_mgmt.gpkg";
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

  auto* gpkg_ds = dynamic_cast<GDALGeoPackageDataset*>(ds.get());
  ASSERT_NE(nullptr, gpkg_ds);

  // Standard EPSG:4326 and EPSG:3857
  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  int srsid_4326 = gpkg_ds->GetSrsId(&srs4326);
  EXPECT_EQ(4326, srsid_4326);

  OGRSpatialReference srs3857;
  srs3857.importFromEPSG(3857);
  int srsid_3857 = gpkg_ds->GetSrsId(&srs3857);
  EXPECT_EQ(3857, srsid_3857);

  // Undefined SRS
  int srsid_null = gpkg_ds->GetSrsId(nullptr);
  EXPECT_EQ(99999, srsid_null);

  // Custom SRS
  OGRSpatialReference srs_custom;
  srs_custom.importFromProj4(
      "+proj=lcc +lat_1=49 +lat_2=44 +lat_0=46.5 +lon_0=3 +x_0=700000 "
      "+y_0=6600000 +ellps=GRS80 +units=m +no_defs");
  int srsid_custom = gpkg_ds->GetSrsId(&srs_custom);
  EXPECT_GE(srsid_custom, GDALGeoPackageDataset::FIRST_CUSTOM_SRSID);

  // Retrieval
  auto retrieved_srs = gpkg_ds->GetSpatialRef(srsid_4326);
  ASSERT_NE(nullptr, retrieved_srs);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  retrieved_srs->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_TRUE(retrieved_srs->IsSame(&srs4326));

  auto retrieved_custom = gpkg_ds->GetSpatialRef(srsid_custom);
  ASSERT_NE(nullptr, retrieved_custom);

  // GetSrsName
  const char* srs_name = gpkg_ds->GetSrsName(srs4326);
  EXPECT_NE(nullptr, srs_name);

  // Coordinate epoch
  OGRSpatialReference srs_epoch;
  srs_epoch.importFromEPSG(4326);
  srs_epoch.SetCoordinateEpoch(2021.0);
  int srsid_epoch = gpkg_ds->GetSrsId(&srs_epoch);
  EXPECT_GE(srsid_epoch, 0);

  // Compound CRS
  OGRSpatialReference srs_horiz;
  srs_horiz.importFromEPSG(4326);
  OGRSpatialReference srs_vert;
  srs_vert.importFromEPSG(5773);
  OGRSpatialReference srs_compound;
  srs_compound.SetCompoundCS("WGS 84 + EGM96 height", &srs_horiz, &srs_vert);
  int srsid_compound = gpkg_ds->GetSrsId(&srs_compound);
  EXPECT_GE(srsid_compound, 0);

  // SetSpatialRef on vector-only dataset returns CE_Failure
  EXPECT_EQ(CE_Failure, ds->SetSpatialRef(&srs4326));
  EXPECT_EQ(nullptr, gpkg_ds->GetSpatialRefRasterOnly());

  // Test SetSpatialRef and SetGeoTransform on a raster dataset
  {
    const std::string r_path = "/vsimem/test_srs_raster.gpkg";
    absl::Cleanup r_unlink([&r_path] { VSIUnlink(r_path.c_str()); });
    auto rds = std::unique_ptr<GDALDataset>(
        drv->Create(r_path.c_str(), 64, 64, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, rds);
    EXPECT_EQ(CE_None, rds->SetSpatialRef(&srs4326));
    EXPECT_NE(nullptr, rds->GetSpatialRef());
    GDALGeoTransform gt_in{100.0, 1.0, 0.0, 200.0, 0.0, -1.0};
    EXPECT_EQ(CE_None, rds->SetGeoTransform(gt_in));
    GDALGeoTransform gt_out{};
    EXPECT_EQ(CE_None, rds->GetGeoTransform(gt_out));
    EXPECT_DOUBLE_EQ(100.0, gt_out[0]);
  }
}

// ---------------------------------------------------------------------------
// 3. Raster Creation, Tiling Schemes, Overviews, Gridded Coverage, Subdatasets
// ---------------------------------------------------------------------------
TEST_F(GDALGeoPackageDatasetTest, RasterCreationTilingSchemesAndFormats) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  const char* const schemes[] = {"CUSTOM", "GoogleCRS84Quad",
                                 "PseudoTMS_GlobalMercator"};

  for (const char* scheme : schemes) {
    const std::string path =
        absl::StrCat("/vsimem/raster_scheme_", scheme, ".gpkg");
    absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

    const char* const opts[] = {CPLSPrintf("TILING_SCHEME=%s", scheme),
                                "TILE_FORMAT=PNG", nullptr};
    auto ds = std::unique_ptr<GDALDataset>(drv->Create(
        path.c_str(), 256, 256, 4, GDT_Byte, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterCount());

    if (strcmp(scheme, "GoogleCRS84Quad") == 0) {
      ds->SetGeoTransform(GDALGeoTransform{-180.0, 360.0 / 256.0, 0.0, 180.0,
                                           0.0, -360.0 / 256.0});
    } else if (strcmp(scheme, "PseudoTMS_GlobalMercator") == 0) {
      ds->SetGeoTransform(GDALGeoTransform{-20037508.34, 78271.516, 0.0,
                                           20037508.34, 0.0, -78271.516});
    } else {
      ds->SetGeoTransform(GDALGeoTransform{0.0, 1.0, 0.0, 256.0, 0.0, -1.0});
    }

    // Write a block
    std::vector<uint8_t> buffer(256 * 256, 128);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 256, 256,
                                                      buffer.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Test tile formats: JPEG, WEBP, TIFF, PNG_JPEG
  const char* const formats[] = {"PNG", "JPEG", "WEBP", "TIFF", "PNG_JPEG"};
  for (const char* fmt : formats) {
    const std::string path = absl::StrCat("/vsimem/raster_fmt_", fmt, ".gpkg");
    absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

    const char* const opts[] = {CPLSPrintf("TILE_FORMAT=%s", fmt), "QUALITY=75",
                                "ZLEVEL=6", nullptr};
    auto ds = std::unique_ptr<GDALDataset>(
        drv->Create(path.c_str(), 128, 128, (strcmp(fmt, "JPEG") == 0 ? 3 : 4),
                    GDT_Byte, const_cast<char**>(opts)));
    if (ds != nullptr) {
      ds->SetGeoTransform(GDALGeoTransform{0.0, 1.0, 0.0, 128.0, 0.0, -1.0});
      std::vector<uint8_t> buf(128 * 128, 200);
      CPL_IGNORE_RET_VAL(
          ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 128, 128, buf.data(),
                                         128, 128, GDT_Byte, 0, 0, nullptr));
      ds->FlushCache(false);
    }
  }
}

TEST_F(GDALGeoPackageDatasetTest, GriddedCoverageAndCreateCopy) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  // Test gridded coverage (Int16, UInt16, Float32)
  const GDALDataType types[] = {GDT_Int16, GDT_UInt16, GDT_Float32};
  for (GDALDataType dt : types) {
    const std::string path =
        absl::StrCat("/vsimem/gridded_cov_", GDALGetDataTypeName(dt), ".gpkg");
    absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

    const char* const opts[] = {"GRID_CELL_ENCODING=grid-cell-encoding",
                                "UOM=metre",
                                "FIELD_NAME=elevation",
                                "QUANTITY_DEFINITION=Height above datum",
                                "OFFSET=0",
                                "SCALE=1",
                                "PRECISION=1",
                                "DATA_NULL=9999",
                                nullptr};
    auto ds = std::unique_ptr<GDALDataset>(
        drv->Create(path.c_str(), 64, 64, 1, dt, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);
    ds->SetGeoTransform(GDALGeoTransform{0.0, 1.0, 0.0, 64.0, 0.0, -1.0});

    std::vector<float> fbuf(64 * 64, 42.5f);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 64, 64, fbuf.data(), 64, 64,
                           GDT_Float32, 0, 0, nullptr));
    ds->FlushCache(false);

    // Test CreateCopy
    const std::string copy_path = absl::StrCat(path, "_copy.gpkg");
    absl::Cleanup copy_unlink([&copy_path] { VSIUnlink(copy_path.c_str()); });

    auto ds_copy = std::unique_ptr<GDALDataset>(drv->CreateCopy(
        copy_path.c_str(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds_copy);
    EXPECT_EQ(1, ds_copy->GetRasterCount());
  }
}

TEST_F(GDALGeoPackageDatasetTest, RasterOverviewsAndSubdatasets) {
  const std::string path = "/vsimem/raster_ovr_sub.gpkg";
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

  {
    const char* const opts[] = {"RASTER_TABLE=r1",
                                "RASTER_IDENTIFIER=First Raster",
                                "RASTER_DESCRIPTION=Primary coverage", nullptr};
    auto ds = std::unique_ptr<GDALDataset>(drv->Create(
        path.c_str(), 1024, 1024, 1, GDT_Byte, const_cast<char**>(opts)));
    ASSERT_NE(nullptr, ds);
    ds->SetGeoTransform(GDALGeoTransform{0.0, 1.0, 0.0, 1024.0, 0.0, -1.0});

    std::vector<uint8_t> buf(1024 * 1024, 100);
    CPL_IGNORE_RET_VAL(
        ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1024, 1024, buf.data(),
                                       1024, 1024, GDT_Byte, 0, 0, nullptr));
    ds->FlushCache(false);

    // Build overviews
    int overview_list[] = {2, 4};
    const char* const ovr_resamplings[] = {"NEAREST", "AVERAGE", "BILINEAR",
                                           "CUBIC"};
    for (const char* resampling : ovr_resamplings) {
      EXPECT_EQ(CE_None, ds->BuildOverviews(resampling, 2, overview_list, 0,
                                            nullptr, nullptr, nullptr));
    }

    // Clear overviews
    EXPECT_EQ(CE_None, ds->BuildOverviews("NONE", 0, nullptr, 0, nullptr,
                                          nullptr, nullptr));
  }

  // Append a second raster subdataset
  {
    const char* const opts2[] = {"APPEND_SUBDATASET=YES", "RASTER_TABLE=r2",
                                 "RASTER_IDENTIFIER=Second Raster", nullptr};
    auto ds2 = std::unique_ptr<GDALDataset>(drv->Create(
        path.c_str(), 256, 256, 1, GDT_Byte, const_cast<char**>(opts2)));
    ASSERT_NE(nullptr, ds2);
    ds2->SetGeoTransform(GDALGeoTransform{0.0, 1.0, 0.0, 256.0, 0.0, -1.0});
    ds2->FlushCache(false);
  }

  // Open dataset and check SUBDATASETS domain
  auto opened = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(path.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(nullptr, opened);
  char** subds_meta = opened->GetMetadata("SUBDATASETS");
  ASSERT_NE(nullptr, subds_meta);
  EXPECT_NE(nullptr, CSLFetchNameValue(subds_meta, "SUBDATASET_1_NAME"));
  EXPECT_NE(nullptr, CSLFetchNameValue(subds_meta, "SUBDATASET_2_NAME"));

  // Open specific subdataset
  const char* subds1_name = CSLFetchNameValue(subds_meta, "SUBDATASET_1_NAME");
  auto sub_ds1 = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(subds1_name, GDAL_OF_RASTER));
  ASSERT_NE(nullptr, sub_ds1);
  EXPECT_EQ(1024, sub_ds1->GetRasterXSize());

  auto* gds = dynamic_cast<GDALGeoPackageDataset*>(opened.get());
  ASSERT_NE(nullptr, gds);
  EXPECT_NE(nullptr, gds->GetRasterLayerDataset("r1"));
  EXPECT_NE(nullptr, gds->GetRasterLayerDataset("r2"));
  EXPECT_EQ(nullptr, gds->GetRasterLayerDataset("non_existent"));

  // Dataset level IRasterIO
  std::vector<uint8_t> read_buf(100 * 100);
  int panBands[] = {1};
  EXPECT_EQ(CE_None,
            sub_ds1->RasterIO(GF_Read, 0, 0, 100, 100, read_buf.data(), 100,
                              100, GDT_Byte, 1, panBands, 0, 0, 0, nullptr));
}

// ---------------------------------------------------------------------------
// 4. Layer Operations: Create, Delete, Rename, Find
// ---------------------------------------------------------------------------
TEST_F(GDALGeoPackageDatasetTest, LayerOperations) {
  const std::string path = "/vsimem/layer_ops.gpkg";
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);

  // Vector layer
  const char* const l_opts[] = {"FID=fid", "GEOMETRY_NAME=geom", nullptr};
  OGRLayer* ly1 =
      ds->CreateLayer("v1", &srs, wkbPoint, const_cast<char**>(l_opts));
  ASSERT_NE(nullptr, ly1);
  OGRFeature f1(ly1->GetLayerDefn());
  OGRPoint pt(1, 2);
  f1.SetGeometry(&pt);
  ASSERT_EQ(OGRERR_NONE, ly1->CreateFeature(&f1));
  ASSERT_EQ(OGRERR_NONE, ly1->SyncToDisk());

  // Non-spatial layer (aspatial)
  const char* const aspatial_opts[] = {"ASPATIAL_VARIANT=GPKG_ATTRIBUTES",
                                       nullptr};
  OGRLayer* ly_aspatial = ds->CreateLayer("aspatial_tbl", nullptr, wkbNone,
                                          const_cast<char**>(aspatial_opts));
  ASSERT_NE(nullptr, ly_aspatial);
  OGRFieldDefn f_fld("prop", OFTString);
  CPL_IGNORE_RET_VAL(ly_aspatial->CreateField(&f_fld));
  OGRFeature f_asp(ly_aspatial->GetLayerDefn());
  f_asp.SetField("prop", "val");
  ASSERT_EQ(OGRERR_NONE, ly_aspatial->CreateFeature(&f_asp));
  ASSERT_EQ(OGRERR_NONE, ly_aspatial->SyncToDisk());

  // Layer to overwrite
  OGRLayer* ly_overwrite =
      ds->CreateLayer("to_overwrite", &srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, ly_overwrite);
  ly_overwrite->SyncToDisk();

  const char* const ow_opts[] = {"OVERWRITE=YES", nullptr};
  OGRLayer* ly_overwritten = ds->CreateLayer(
      "to_overwrite", &srs, wkbLineString, const_cast<char**>(ow_opts));
  ASSERT_NE(nullptr, ly_overwritten);
  EXPECT_EQ(wkbLineString, ly_overwritten->GetGeomType());

  // Find layer by name
  EXPECT_NE(nullptr, ds->GetLayerByName("v1"));
  EXPECT_NE(nullptr, ds->GetLayerByName("aspatial_tbl"));
  EXPECT_EQ(nullptr, ds->GetLayerByName("non_existent"));

  // GetLayerWithGetSpatialWhereByName
  auto* gds = dynamic_cast<GDALGeoPackageDataset*>(ds.get());
  ASSERT_NE(nullptr, gds);
  auto pair = gds->GetLayerWithGetSpatialWhereByName("v1");
  EXPECT_NE(nullptr, pair.first);
  EXPECT_NE(nullptr, pair.second);

  gds->ResetReadingAllLayers();

  // Rename layer using SQL ALTER TABLE ... RENAME TO
  OGRLayer* res =
      ds->ExecuteSQL("ALTER TABLE v1 RENAME TO v1_renamed", nullptr, nullptr);
  if (res) ds->ReleaseResultSet(res);
  EXPECT_NE(nullptr, ds->GetLayerByName("v1_renamed"));
  EXPECT_EQ(nullptr, ds->GetLayerByName("v1"));

  // Delete layer via DeleteLayer API
  int idx = -1;
  for (int i = 0; i < ds->GetLayerCount(); ++i) {
    if (strcmp(ds->GetLayer(i)->GetName(), "to_overwrite") == 0) {
      idx = i;
      break;
    }
  }
  ASSERT_GE(idx, 0);
  EXPECT_EQ(OGRERR_NONE, ds->DeleteLayer(idx));
  EXPECT_EQ(nullptr, ds->GetLayerByName("to_overwrite"));

  // Delete layer via SQL DELLAYER:
  res = ds->ExecuteSQL("DELLAYER:aspatial_tbl", nullptr, nullptr);
  if (res) ds->ReleaseResultSet(res);
  EXPECT_EQ(nullptr, ds->GetLayerByName("aspatial_tbl"));

  // Delete layer via SQL DROP TABLE
  res = ds->ExecuteSQL("DROP TABLE v1_renamed", nullptr, nullptr);
  if (res) ds->ReleaseResultSet(res);
  EXPECT_EQ(nullptr, ds->GetLayerByName("v1_renamed"));
}

// ---------------------------------------------------------------------------
// 5. Metadata Handling (Dataset, Layer, Domains, XML)
// ---------------------------------------------------------------------------
TEST_F(GDALGeoPackageDatasetTest, MetadataHandling) {
  const std::string path = "/vsimem/metadata_test.gpkg";
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  {
    auto ds = std::unique_ptr<GDALDataset>(
        drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    OGRLayer* ly = ds->CreateLayer("meta_layer", &srs, wkbPoint, nullptr);
    ASSERT_NE(nullptr, ly);
    ly->SyncToDisk();

    // Dataset level metadata
    EXPECT_EQ(CE_None, ds->SetMetadataItem("DATASET_KEY", "DATASET_VAL"));
    EXPECT_STREQ("DATASET_VAL", ds->GetMetadataItem("DATASET_KEY"));

    EXPECT_EQ(CE_None,
              ds->SetMetadataItem("CUSTOM_ITEM", "CUSTOM_VAL", "GEOPACKAGE"));
    EXPECT_STREQ("CUSTOM_VAL",
                 ds->GetMetadataItem("CUSTOM_ITEM", "GEOPACKAGE"));

    // XML metadata
    const char* const xml_data =
        "<gmd:MD_Metadata xmlns:gmd=\"http://www.isotc211.org/2005/gmd\">"
        "<gmd:fileIdentifier><gco:CharacterString "
        "xmlns:gco=\"http://www.isotc211.org/2005/gco\">test_id</"
        "gco:CharacterString></gmd:fileIdentifier>"
        "</gmd:MD_Metadata>";
    char* xml_meta[] = {const_cast<char*>(xml_data), nullptr};
    EXPECT_EQ(CE_None, ds->SetMetadata(xml_meta, "xml:metadata"));

    char** retrieved_xml = ds->GetMetadata("xml:metadata");
    ASSERT_NE(nullptr, retrieved_xml);
    EXPECT_NE(nullptr, retrieved_xml[0]);

    // Layer metadata
    EXPECT_EQ(CE_None, ly->SetMetadataItem("LAYER_KEY", "LAYER_VAL"));
    EXPECT_STREQ("LAYER_VAL", ly->GetMetadataItem("LAYER_KEY"));

    // Domain list
    char** domain_list = ds->GetMetadataDomainList();
    EXPECT_NE(nullptr, domain_list);
    CSLDestroy(domain_list);

    ds->FlushCache(false);
  }
  absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

  // Reopen and check persistence
  auto reopened = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, reopened);
  EXPECT_STREQ("DATASET_VAL", reopened->GetMetadataItem("DATASET_KEY"));
  EXPECT_STREQ("CUSTOM_VAL",
               reopened->GetMetadataItem("CUSTOM_ITEM", "GEOPACKAGE"));

  OGRLayer* rly = reopened->GetLayerByName("meta_layer");
  ASSERT_NE(nullptr, rly);
  EXPECT_STREQ("LAYER_VAL", rly->GetMetadataItem("LAYER_KEY"));
}

// ---------------------------------------------------------------------------
// 6. Field Domains (Coded, Range, Glob)
// ---------------------------------------------------------------------------
TEST_F(GDALGeoPackageDatasetTest, FieldDomainsManagement) {
  const std::string path = "/vsimem/domains_test.gpkg";
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  {
    auto ds = std::unique_ptr<GDALDataset>(
        drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    OGRLayer* ly_dummy =
        ds->CreateLayer("dom_layer", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, ly_dummy);
    ASSERT_EQ(OGRERR_NONE, ly_dummy->SyncToDisk());

    std::string failureReason;

    // Coded domain
    std::vector<OGRCodedValue> codedValues;
    codedValues.push_back({CPLStrdup("1"), CPLStrdup("Code One")});
    codedValues.push_back({CPLStrdup("2"), CPLStrdup("Code Two")});
    codedValues.push_back({nullptr, nullptr});
    auto codedDomain = std::make_unique<OGRCodedFieldDomain>(
        "CodedDom", "Test coded domain", OFTInteger, OFSTNone,
        std::move(codedValues));
    EXPECT_TRUE(ds->AddFieldDomain(std::move(codedDomain), failureReason));

    // Range domain (inclusive min, exclusive max)
    OGRField minVal;
    minVal.Integer = 10;
    OGRField maxVal;
    maxVal.Integer = 100;
    auto rangeDomain = std::make_unique<OGRRangeFieldDomain>(
        "RangeDom", "Test range domain", OFTInteger, OFSTNone, minVal, true,
        maxVal, false);
    EXPECT_TRUE(ds->AddFieldDomain(std::move(rangeDomain), failureReason));

    // Glob domain
    auto globDomain = std::make_unique<OGRGlobFieldDomain>(
        "GlobDom", "Test glob domain", OFTString, OFSTNone, "*@google.com");
    EXPECT_TRUE(ds->AddFieldDomain(std::move(globDomain), failureReason));

    // Duplicate domain should fail
    auto dupDomain = std::make_unique<OGRGlobFieldDomain>(
        "GlobDom", "Duplicate", OFTString, OFSTNone, "*.org");
    EXPECT_FALSE(ds->AddFieldDomain(std::move(dupDomain), failureReason));

    // Verification
    std::vector<std::string> names = ds->GetFieldDomainNames();
    EXPECT_THAT(names, testing::UnorderedElementsAre("CodedDom", "RangeDom",
                                                     "GlobDom"));

    const OGRFieldDomain* dom = ds->GetFieldDomain("CodedDom");
    ASSERT_NE(nullptr, dom);
    EXPECT_EQ(OFDT_CODED, dom->GetDomainType());

    dom = ds->GetFieldDomain("RangeDom");
    ASSERT_NE(nullptr, dom);
    EXPECT_EQ(OFDT_RANGE, dom->GetDomainType());

    dom = ds->GetFieldDomain("GlobDom");
    ASSERT_NE(nullptr, dom);
    EXPECT_EQ(OFDT_GLOB, dom->GetDomainType());
  }
  absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

  // Reopen and check reading field domains from database
  auto reopened = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR));
  ASSERT_NE(nullptr, reopened);
  std::vector<std::string> names = reopened->GetFieldDomainNames();
  EXPECT_THAT(names,
              testing::UnorderedElementsAre("CodedDom", "RangeDom", "GlobDom"));

  const OGRFieldDomain* coded = reopened->GetFieldDomain("CodedDom");
  ASSERT_NE(nullptr, coded);
  EXPECT_EQ(OFDT_CODED, coded->GetDomainType());
}

// ---------------------------------------------------------------------------
// 7. Relationships (1:N, N:M, Associations, Aggregations, Foreign Keys)
// ---------------------------------------------------------------------------
TEST_F(GDALGeoPackageDatasetTest, RelationshipsManagement) {
  const std::string path = "/vsimem/relations_test.gpkg";
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  {
    auto ds = std::unique_ptr<GDALDataset>(
        drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    OGRSpatialReference srs;
    srs.importFromEPSG(4326);

    OGRLayer* parent = ds->CreateLayer("parent_table", &srs, wkbPoint, nullptr);
    ASSERT_NE(nullptr, parent);
    OGRFieldDefn f_pid("pid", OFTInteger);
    CPL_IGNORE_RET_VAL(parent->CreateField(&f_pid));
    parent->SyncToDisk();

    OGRLayer* child = ds->CreateLayer("child_table", &srs, wkbPoint, nullptr);
    ASSERT_NE(nullptr, child);
    OGRFieldDefn f_cid("cid", OFTInteger);
    CPL_IGNORE_RET_VAL(child->CreateField(&f_cid));
    OGRFieldDefn f_parent_id("parent_id", OFTInteger);
    CPL_IGNORE_RET_VAL(child->CreateField(&f_parent_id));
    child->SyncToDisk();

    std::string failureReason;

    // 1:N / many-to-many relationship with auto-generated mapping table
    auto rel1 = std::make_unique<GDALRelationship>(
        "parent_table_child_table_features", "parent_table", "child_table",
        GRC_MANY_TO_MANY);
    rel1->SetLeftTableFields({"pid"});
    rel1->SetRightTableFields({"cid"});
    rel1->SetRelatedTableType("features");
    rel1->SetType(GRT_COMPOSITE);
    EXPECT_TRUE(ds->AddRelationship(std::move(rel1), failureReason));

    // N:M relationship with explicit mapping table
    OGRLayer* mapping = ds->CreateLayer("map_table", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, mapping);
    OGRFieldDefn f_m1("base_id", OFTInteger);
    CPL_IGNORE_RET_VAL(mapping->CreateField(&f_m1));
    OGRFieldDefn f_m2("related_id", OFTInteger);
    CPL_IGNORE_RET_VAL(mapping->CreateField(&f_m2));
    mapping->SyncToDisk();

    auto rel2 = std::make_unique<GDALRelationship>(
        "parent_table_child_table_media", "parent_table", "child_table",
        GRC_MANY_TO_MANY);
    rel2->SetLeftTableFields({"pid"});
    rel2->SetRightTableFields({"cid"});
    rel2->SetRelatedTableType("media");
    rel2->SetMappingTableName("map_table");
    EXPECT_TRUE(ds->AddRelationship(std::move(rel2), failureReason));

    // Invalid relationship should fail
    auto rel_invalid = std::make_unique<GDALRelationship>(
        "rel_bad", "non_existent_table", "child_table", GRC_ONE_TO_MANY);
    EXPECT_FALSE(ds->AddRelationship(std::move(rel_invalid), failureReason));

    // GetRelationshipNames & GetRelationship
    std::vector<std::string> rel_names = ds->GetRelationshipNames();
    EXPECT_TRUE(std::find(rel_names.begin(), rel_names.end(),
                          "parent_table_child_table_features") !=
                rel_names.end());
    EXPECT_TRUE(std::find(rel_names.begin(), rel_names.end(),
                          "parent_table_child_table_media") != rel_names.end());

    const GDALRelationship* got_rel =
        ds->GetRelationship("parent_table_child_table_features");
    ASSERT_NE(nullptr, got_rel);
    EXPECT_EQ(GRC_MANY_TO_MANY, got_rel->GetCardinality());
    EXPECT_EQ(GRT_ASSOCIATION, got_rel->GetType());

    // Update relationship
    auto rel_updated = std::make_unique<GDALRelationship>(
        "parent_table_child_table_features", "parent_table", "child_table",
        GRC_MANY_TO_MANY);
    rel_updated->SetLeftTableFields({"pid"});
    rel_updated->SetRightTableFields({"cid"});
    rel_updated->SetRelatedTableType("features");
    rel_updated->SetMappingTableName("parent_table_child_table");
    rel_updated->SetType(GRT_AGGREGATION);
    EXPECT_TRUE(ds->UpdateRelationship(std::move(rel_updated), failureReason));

    got_rel = ds->GetRelationship("parent_table_child_table_features");
    ASSERT_NE(nullptr, got_rel);

    // Delete relationship
    EXPECT_TRUE(ds->DeleteRelationship("parent_table_child_table_media",
                                       failureReason));
    EXPECT_EQ(nullptr, ds->GetRelationship("parent_table_child_table_media"));
  }
  absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

  // Reopen and check reading relationships from related_tables extension
  auto reopened = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR));
  ASSERT_NE(nullptr, reopened);
  const GDALRelationship* rel =
      reopened->GetRelationship("parent_table_child_table_features");
  ASSERT_NE(nullptr, rel);
  EXPECT_EQ(GRC_MANY_TO_MANY, rel->GetCardinality());
}

// ---------------------------------------------------------------------------
// 8. ExecuteSQL Special Commands & Custom SQLite Functions
// ---------------------------------------------------------------------------
TEST_F(GDALGeoPackageDatasetTest, ExecuteSQLCommandsAndFunctions) {
  CPLSetConfigOption("GPKG_DEBUG", "TRUE");
  absl::Cleanup cleanup_debug(
      [] { CPLSetConfigOption("GPKG_DEBUG", nullptr); });

  const std::string path = "/vsimem/exec_sql_test.gpkg";
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);

  OGRLayer* ly = ds->CreateLayer("features", &srs, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, ly);
  OGRFeature f(ly->GetLayerDefn());
  OGRPolygon poly;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(0, 10);
  ring.addPoint(10, 10);
  ring.addPoint(10, 0);
  ring.addPoint(0, 0);
  poly.addRing(&ring);
  f.SetGeometry(&poly);
  ASSERT_EQ(OGRERR_NONE, ly->CreateFeature(&f));
  ASSERT_EQ(OGRERR_NONE, ly->SyncToDisk());

  // Special commands
  const char* const special_cmds[] = {
      "SELECT CreateSpatialIndex('features', 'geom')",
      "SELECT HasSpatialIndex('features', 'geom')",
      "SELECT DisableSpatialIndex('features', 'geom')",
      "SELECT HasSpatialIndex('features', 'geom')",
      "RECOMPUTE EXTENT ON features",
      "VACUUM",
  };
  for (const char* cmd : special_cmds) {
    OGRLayer* res = ds->ExecuteSQL(cmd, nullptr, nullptr);
    if (res) {
      OGRFeature* feat = res->GetNextFeature();
      if (feat) OGRFeature::DestroyFeature(feat);
      ds->ReleaseResultSet(res);
    }
  }

  // Custom SQL functions
  const char* const func_queries[] = {
      "SELECT ST_MinX(geom), ST_MinY(geom), ST_MaxX(geom), ST_MaxY(geom) FROM "
      "features",
      "SELECT ST_IsEmpty(geom), ST_GeometryType(geom) FROM features",
      "SELECT GPKG_IsAssignable('POINT', 'GEOMETRY')",
      "SELECT ST_SRID(geom), SetSRID(geom, 3857) FROM features",
      "SELECT ST_EnvelopesIntersects(geom, 0, 0, 10, 10), "
      "ST_EnvIntersects(geom, geom) FROM features",
      "SELECT ST_Area(geom), ST_Area(geom, 1) FROM features",
      "SELECT ST_Length(geom), ST_Length(geom, 1) FROM features",
      "SELECT ST_Transform(geom, 3857), Transform(geom, 4326) FROM features",
      "SELECT SridFromAuthCRS('EPSG', 4326), ImportFromEPSG(4326)",
      "SELECT RegisterGeometryExtension('features', 'geom', 'POLYGON')",
      "SELECT hstore_get_value('a=>1, b=>2', 'b')",
      "SELECT GDAL_GetMimeType(geom), GDAL_GetBandCount(geom), "
      "GDAL_HasColorTable(geom) FROM features",
      "SELECT ogr_layer_Extent('features')",
  };

  for (const char* sql : func_queries) {
    OGRLayer* res = ds->ExecuteSQL(sql, nullptr, nullptr);
    ASSERT_NE(nullptr, res) << "Failed executing: " << sql;
    OGRFeature* feat = res->GetNextFeature();
    EXPECT_NE(nullptr, feat) << "No feature returned for: " << sql;
    if (feat) OGRFeature::DestroyFeature(feat);
    ds->ReleaseResultSet(res);
  }
}

// ---------------------------------------------------------------------------
// 9. Transactions, Savepoints, and Metadata Fixups
// ---------------------------------------------------------------------------
TEST_F(GDALGeoPackageDatasetTest, TransactionsSavepointsAndFixups) {
  const std::string path = "/vsimem/tx_fixups_test.gpkg";
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  absl::Cleanup unlink_cleanup([&path] { VSIUnlink(path.c_str()); });

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  OGRLayer* ly = ds->CreateLayer("tx_table", &srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, ly);
  ly->SyncToDisk();

  auto* gds = dynamic_cast<GDALGeoPackageDataset*>(ds.get());
  ASSERT_NE(nullptr, gds);

  // Commit transaction
  EXPECT_FALSE(gds->IsInTransaction());
  EXPECT_EQ(OGRERR_NONE, ds->StartTransaction());
  EXPECT_TRUE(gds->IsInTransaction());
  OGRFeature f1(ly->GetLayerDefn());
  OGRPoint pt1(1, 1);
  f1.SetGeometry(&pt1);
  EXPECT_EQ(OGRERR_NONE, ly->CreateFeature(&f1));
  EXPECT_EQ(OGRERR_NONE, ds->CommitTransaction());
  EXPECT_FALSE(gds->IsInTransaction());
  EXPECT_EQ(1, ly->GetFeatureCount(true));

  // Rollback transaction
  EXPECT_EQ(OGRERR_NONE, ds->StartTransaction());
  EXPECT_TRUE(gds->IsInTransaction());
  OGRFeature f2(ly->GetLayerDefn());
  OGRPoint pt2(2, 2);
  f2.SetGeometry(&pt2);
  EXPECT_EQ(OGRERR_NONE, ly->CreateFeature(&f2));
  EXPECT_EQ(OGRERR_NONE, ds->RollbackTransaction());
  EXPECT_FALSE(gds->IsInTransaction());
  EXPECT_EQ(1, ly->GetFeatureCount(true));

  // Fixup tests (FixupWrongRTreeTrigger,
  // FixupWrongMedataReferenceColumnNameUpdate)
  sqlite3* hDB = gds->GetDB();
  ASSERT_NE(nullptr, hDB);

  // Intentionally inject faulty trigger text and faulty metadata table
  sqlite3_exec(
      hDB,
      "CREATE TABLE faulty_tbl (id INTEGER PRIMARY KEY, geom BLOB);"
      "CREATE VIRTUAL TABLE rtree_faulty_tbl_geom USING rtree(id, minx, maxx, "
      "miny, maxy);"
      "CREATE TRIGGER rtree_faulty_tbl_geom_update AFTER UPDATE OF geom ON "
      "faulty_tbl "
      "BEGIN DELETE FROM rtree_faulty_tbl_geom WHERE id = OLD.id; END;",
      nullptr, nullptr, nullptr);

  ds.reset();

  // Reopening in update mode triggers FixupWrongRTreeTrigger and
  // FixupWrongMedataReferenceColumnNameUpdate
  auto reopened = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, reopened);

  // Test LaunderName
  EXPECT_EQ("abc_def", GDALGeoPackageDataset::LaunderName("abc def"));
  EXPECT_EQ("field_1", GDALGeoPackageDataset::LaunderName("field#1"));
}

TEST_F(GDALGeoPackageDatasetTest,
       CreateCopyWithTilingSchemesAndGetTilingScheme) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  // 1. Create a 64x64 MEM dataset spanning latitudes [-90, +90] to trigger
  // EPSG:3857 clamping
  auto mem_ds = std::unique_ptr<GDALDataset>(
      mem_drv->Create("", 64, 64, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mem_ds);
  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  mem_ds->SetSpatialRef(&srs4326);
  double gt[6] = {-180.0, 5.625, 0.0, 90.0, 0.0, -2.8125};
  mem_ds->SetGeoTransform(gt);
  std::vector<GByte> raster_data(64 * 64, 128);
  EXPECT_EQ(CE_None, mem_ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 64, 64,
                                                        raster_data.data(), 64,
                                                        64, GDT_Byte, 0, 0));

  // TILING_SCHEME=PseudoTMS_GlobalMercator (triggers EPSG:3857 latitude clamp +
  // VRT warp)
  const std::string path_merc = "/vsimem/gpkg_merc.gpkg";
  absl::Cleanup cl_merc = [&path_merc] { VSIUnlink(path_merc.c_str()); };
  CPLStringList opts_merc;
  opts_merc.AddString("TILING_SCHEME=PseudoTMS_GlobalMercator");
  opts_merc.AddString("ZOOM_LEVEL=0");
  auto ds_merc = std::unique_ptr<GDALDataset>(
      drv->CreateCopy(path_merc.c_str(), mem_ds.get(), FALSE, opts_merc.List(),
                      nullptr, nullptr));
  EXPECT_NE(nullptr, ds_merc);

  // TILING_SCHEME=GoogleCRS84Quad with strategies and resampling
  const std::string path_crs84 = "/vsimem/gpkg_crs84.gpkg";
  absl::Cleanup cl_crs84 = [&path_crs84] { VSIUnlink(path_crs84.c_str()); };
  CPLStringList opts_crs84;
  opts_crs84.AddString("TILING_SCHEME=GoogleCRS84Quad");
  opts_crs84.AddString("ZOOM_LEVEL_STRATEGY=LOWER");
  opts_crs84.AddString("RESAMPLING=BILINEAR");
  opts_crs84.AddString("ZOOM_LEVEL=0");
  auto ds_crs84 = std::unique_ptr<GDALDataset>(
      drv->CreateCopy(path_crs84.c_str(), mem_ds.get(), FALSE,
                      opts_crs84.List(), nullptr, nullptr));
  EXPECT_NE(nullptr, ds_crs84);

  // Other strategies
  opts_crs84.SetNameValue("ZOOM_LEVEL_STRATEGY", "UPPER");
  opts_crs84.SetNameValue("RESAMPLING", "NEAREST");
  const std::string path_crs84_up = "/vsimem/gpkg_crs84_up.gpkg";
  absl::Cleanup cl_up = [&path_crs84_up] { VSIUnlink(path_crs84_up.c_str()); };
  auto ds_up = std::unique_ptr<GDALDataset>(
      drv->CreateCopy(path_crs84_up.c_str(), mem_ds.get(), FALSE,
                      opts_crs84.List(), nullptr, nullptr));
  EXPECT_NE(nullptr, ds_up);

  opts_crs84.SetNameValue("ZOOM_LEVEL_STRATEGY", "AUTO");
  const std::string path_crs84_auto = "/vsimem/gpkg_crs84_auto.gpkg";
  absl::Cleanup cl_auto = [&path_crs84_auto] {
    VSIUnlink(path_crs84_auto.c_str());
  };
  auto ds_auto = std::unique_ptr<GDALDataset>(
      drv->CreateCopy(path_crs84_auto.c_str(), mem_ds.get(), FALSE,
                      opts_crs84.List(), nullptr, nullptr));
  EXPECT_NE(nullptr, ds_auto);

  // Invalid ZOOM_LEVEL=99
  CPLStringList opts_bad_zoom;
  opts_bad_zoom.AddString("TILING_SCHEME=GoogleCRS84Quad");
  opts_bad_zoom.AddString("ZOOM_LEVEL=99");
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              drv->CreateCopy("/vsimem/bad_z.gpkg", mem_ds.get(), FALSE,
                              opts_bad_zoom.List(), nullptr, nullptr));
  }

  // Explicit valid ZOOM_LEVEL=1
  const std::string path_z1 = "/vsimem/gpkg_z1.gpkg";
  absl::Cleanup cl_z1 = [&path_z1] { VSIUnlink(path_z1.c_str()); };
  CPLStringList opts_z1;
  opts_z1.AddString("TILING_SCHEME=GoogleCRS84Quad");
  opts_z1.AddString("ZOOM_LEVEL=1");
  auto ds_z1 = std::unique_ptr<GDALDataset>(drv->CreateCopy(
      path_z1.c_str(), mem_ds.get(), FALSE, opts_z1.List(), nullptr, nullptr));
  EXPECT_NE(nullptr, ds_z1);

  // Color table source with RESAMPLING=BILINEAR and NEAREST
  auto ct_ds = std::unique_ptr<GDALDataset>(
      mem_drv->Create("", 64, 64, 1, GDT_Byte, nullptr));
  ct_ds->SetSpatialRef(&srs4326);
  ct_ds->SetGeoTransform(gt);
  GDALColorTable ct;
  GDALColorEntry e0 = {0, 0, 0, 255};
  GDALColorEntry e1 = {255, 0, 0, 255};
  ct.SetColorEntry(0, &e0);
  ct.SetColorEntry(1, &e1);
  ct_ds->GetRasterBand(1)->SetColorTable(&ct);
  EXPECT_EQ(CE_None, ct_ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 64, 64,
                                                       raster_data.data(), 64,
                                                       64, GDT_Byte, 0, 0));

  const std::string path_ct = "/vsimem/gpkg_ct.gpkg";
  absl::Cleanup cl_ct = [&path_ct] { VSIUnlink(path_ct.c_str()); };
  CPLStringList opts_ct;
  opts_ct.AddString("TILING_SCHEME=GoogleCRS84Quad");
  opts_ct.AddString("RESAMPLING=BILINEAR");
  opts_ct.AddString("ZOOM_LEVEL=0");
  {
    WithQuietHandler quiet;
    auto ds_ct = std::unique_ptr<GDALDataset>(drv->CreateCopy(
        path_ct.c_str(), ct_ds.get(), FALSE, opts_ct.List(), nullptr, nullptr));
    EXPECT_NE(nullptr, ds_ct);
  }

  // 4-band GDT_Byte with alpha
  auto rgba_ds = std::unique_ptr<GDALDataset>(
      mem_drv->Create("", 64, 64, 4, GDT_Byte, nullptr));
  rgba_ds->SetSpatialRef(&srs4326);
  rgba_ds->SetGeoTransform(gt);
  const std::string path_rgba = "/vsimem/gpkg_rgba.gpkg";
  absl::Cleanup cl_rgba = [&path_rgba] { VSIUnlink(path_rgba.c_str()); };
  CPLStringList opts_rgba;
  opts_rgba.AddString("TILING_SCHEME=GoogleCRS84Quad");
  opts_rgba.AddString("ZOOM_LEVEL=0");
  auto ds_rgba = std::unique_ptr<GDALDataset>(
      drv->CreateCopy(path_rgba.c_str(), rgba_ds.get(), FALSE, opts_rgba.List(),
                      nullptr, nullptr));
  EXPECT_NE(nullptr, ds_rgba);

  // 1-band Float32 with nodata=0.0 and nodata=-9999.0
  auto flt_ds = std::unique_ptr<GDALDataset>(
      mem_drv->Create("", 64, 64, 1, GDT_Float32, nullptr));
  flt_ds->SetSpatialRef(&srs4326);
  flt_ds->SetGeoTransform(gt);
  flt_ds->GetRasterBand(1)->SetNoDataValue(0.0);
  const std::string path_flt0 = "/vsimem/gpkg_flt0.gpkg";
  absl::Cleanup cl_flt0 = [&path_flt0] { VSIUnlink(path_flt0.c_str()); };
  CPLStringList opts_flt;
  opts_flt.AddString("TILING_SCHEME=GoogleCRS84Quad");
  opts_flt.AddString("ZOOM_LEVEL=0");
  auto ds_flt0 = std::unique_ptr<GDALDataset>(
      drv->CreateCopy(path_flt0.c_str(), flt_ds.get(), FALSE, opts_flt.List(),
                      nullptr, nullptr));
  EXPECT_NE(nullptr, ds_flt0);

  flt_ds->GetRasterBand(1)->SetNoDataValue(-9999.0);
  const std::string path_flt_neg = "/vsimem/gpkg_flt_neg.gpkg";
  absl::Cleanup cl_flt_neg = [&path_flt_neg] {
    VSIUnlink(path_flt_neg.c_str());
  };
  auto ds_flt_neg = std::unique_ptr<GDALDataset>(
      drv->CreateCopy(path_flt_neg.c_str(), flt_ds.get(), FALSE,
                      opts_flt.List(), nullptr, nullptr));
  EXPECT_NE(nullptr, ds_flt_neg);

  // Inline JSON TMS definitions
  const char* pszValidJsonCRS84 = R"json({
    "type": "TileMatrixSetType",
    "identifier": "ValidCRS84",
    "supportedCRS": "http://www.opengis.net/def/crs/OGC/1.3/CRS84",
    "tileMatrix": [{
      "identifier": "0", "scaleDenominator": 559082264.0287178,
      "topLeftCorner": [-180.0, 90.0],
      "tileWidth": 256, "tileHeight": 256,
      "matrixWidth": 2, "matrixHeight": 1
    }]
  })json";
  CPLStringList opts_tms_crs84;
  opts_tms_crs84.AddString(CPLSPrintf("TILING_SCHEME=%s", pszValidJsonCRS84));
  opts_tms_crs84.AddString("ZOOM_LEVEL=0");
  const std::string path_tms1 = "/vsimem/tms_crs84.gpkg";
  absl::Cleanup cl_tms1 = [&path_tms1] { VSIUnlink(path_tms1.c_str()); };
  auto ds_tms1 = std::unique_ptr<GDALDataset>(
      drv->CreateCopy(path_tms1.c_str(), mem_ds.get(), FALSE,
                      opts_tms_crs84.List(), nullptr, nullptr));
  EXPECT_NE(nullptr, ds_tms1);

  const char* pszValidJsonEPSG4326 = R"json({
    "type": "TileMatrixSetType",
    "identifier": "ValidEPSG4326",
    "supportedCRS": "http://www.opengis.net/def/crs/EPSG/0/4326",
    "tileMatrix": [{
      "identifier": "0", "scaleDenominator": 559082264.0287178,
      "topLeftCorner": [90.0, -180.0],
      "tileWidth": 256, "tileHeight": 256,
      "matrixWidth": 2, "matrixHeight": 1
    }]
  })json";
  CPLStringList opts_tms_4326;
  opts_tms_4326.AddString(CPLSPrintf("TILING_SCHEME=%s", pszValidJsonEPSG4326));
  opts_tms_4326.AddString("ZOOM_LEVEL=0");
  const std::string path_tms2 = "/vsimem/tms_4326.gpkg";
  absl::Cleanup cl_tms2 = [&path_tms2] { VSIUnlink(path_tms2.c_str()); };
  auto ds_tms2 = std::unique_ptr<GDALDataset>(
      drv->CreateCopy(path_tms2.c_str(), mem_ds.get(), FALSE,
                      opts_tms_4326.List(), nullptr, nullptr));
  EXPECT_NE(nullptr, ds_tms2);

  // Invalid JSON TMS definitions
  const char* pszDiffTopLeft = R"json({
    "type": "TileMatrixSetType", "supportedCRS": "EPSG:3857",
    "tileMatrix": [
      {"identifier": "0", "scaleDenominator": 559082264.0287178, "topLeftCorner": [0.0, 0.0], "tileWidth": 256, "tileHeight": 256, "matrixWidth": 1, "matrixHeight": 1},
      {"identifier": "1", "scaleDenominator": 50000.0, "topLeftCorner": [10.0, 10.0], "tileWidth": 256, "tileHeight": 256, "matrixWidth": 2, "matrixHeight": 2}
    ]
  })json";
  {
    WithQuietHandler quiet;
    CPLStringList bad_opt;
    bad_opt.AddString(CPLSPrintf("TILING_SCHEME=%s", pszDiffTopLeft));
    EXPECT_EQ(nullptr,
              drv->CreateCopy("/vsimem/bad_tl.gpkg", mem_ds.get(), FALSE,
                              bad_opt.List(), nullptr, nullptr));
  }

  const char* pszDiffTileSize = R"json({
    "type": "TileMatrixSetType", "supportedCRS": "EPSG:3857",
    "tileMatrix": [
      {"identifier": "0", "scaleDenominator": 559082264.0287178, "topLeftCorner": [0.0, 0.0], "tileWidth": 256, "tileHeight": 256, "matrixWidth": 1, "matrixHeight": 1},
      {"identifier": "1", "scaleDenominator": 50000.0, "topLeftCorner": [0.0, 0.0], "tileWidth": 512, "tileHeight": 512, "matrixWidth": 2, "matrixHeight": 2}
    ]
  })json";
  {
    WithQuietHandler quiet;
    CPLStringList bad_opt;
    bad_opt.AddString(CPLSPrintf("TILING_SCHEME=%s", pszDiffTileSize));
    EXPECT_EQ(nullptr,
              drv->CreateCopy("/vsimem/bad_ts.gpkg", mem_ds.get(), FALSE,
                              bad_opt.List(), nullptr, nullptr));
  }

  const char* pszNotPowerTwo = R"json({
    "type": "TileMatrixSetType", "supportedCRS": "EPSG:3857",
    "tileMatrix": [
      {"identifier": "0", "scaleDenominator": 559082264.0287178, "topLeftCorner": [0.0, 0.0], "tileWidth": 256, "tileHeight": 256, "matrixWidth": 1, "matrixHeight": 1},
      {"identifier": "1", "scaleDenominator": 33333.0, "topLeftCorner": [0.0, 0.0], "tileWidth": 256, "tileHeight": 256, "matrixWidth": 2, "matrixHeight": 2}
    ]
  })json";
  {
    WithQuietHandler quiet;
    CPLStringList bad_opt;
    bad_opt.AddString(CPLSPrintf("TILING_SCHEME=%s", pszNotPowerTwo));
    EXPECT_EQ(nullptr,
              drv->CreateCopy("/vsimem/bad_p2.gpkg", mem_ds.get(), FALSE,
                              bad_opt.List(), nullptr, nullptr));
  }

  const char* pszVarWidth = R"json({
    "type": "TileMatrixSetType", "supportedCRS": "EPSG:3857",
    "tileMatrix": [
      {"identifier": "0", "scaleDenominator": 559082264.0287178, "topLeftCorner": [0.0, 0.0], "tileWidth": 256, "tileHeight": 256, "matrixWidth": 1, "matrixHeight": 1,
       "variableMatrixWidth": [{"coalesce": 2, "minTileRow": 0, "maxTileRow": 0}]}
    ]
  })json";
  {
    WithQuietHandler quiet;
    CPLStringList bad_opt;
    bad_opt.AddString(CPLSPrintf("TILING_SCHEME=%s", pszVarWidth));
    EXPECT_EQ(nullptr,
              drv->CreateCopy("/vsimem/bad_vw.gpkg", mem_ds.get(), FALSE,
                              bad_opt.List(), nullptr, nullptr));
  }
}

TEST_F(GDALGeoPackageDatasetTest, GriddedCoverageAndOpenRasterOptions) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  double gt[6] = {-180.0, 5.625, 0.0, 90.0, 0.0, -2.8125};

  // Float32 raster
  const std::string path_grid_flt = "/vsimem/gpkg_grid_flt.gpkg";
  absl::Cleanup cl_gf = [&path_grid_flt] { VSIUnlink(path_grid_flt.c_str()); };
  {
    auto ds_gf = std::unique_ptr<GDALDataset>(
        drv->Create(path_grid_flt.c_str(), 64, 64, 1, GDT_Float32, nullptr));
    ASSERT_NE(nullptr, ds_gf);
    ds_gf->SetSpatialRef(&srs4326);
    ds_gf->SetGeoTransform(gt);
    ds_gf->GetRasterBand(1)->SetNoDataValue(-9999.0);
    ds_gf->SetMetadataItem("UOM", "m");
    ds_gf->SetMetadataItem("FIELD_NAME", "elevation");
    ds_gf->SetMetadataItem("AREA_OR_POINT", "Point");
    std::vector<float> data(64 * 64, 12.5f);
    EXPECT_EQ(CE_None, ds_gf->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 64, 64,
                                                         data.data(), 64, 64,
                                                         GDT_Float32, 0, 0));
  }

  // Reopen in read-only and update modes with various open options
  {
    CPLStringList open_opts;
    open_opts.AddString("ZOOM_LEVEL=0");
    open_opts.AddString("USE_TILE_EXTENT=YES");
    open_opts.AddString("BAND_COUNT=1");
    auto ro_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(path_grid_flt.c_str(), GDAL_OF_RASTER, nullptr,
                          open_opts.List(), nullptr));
    ASSERT_NE(nullptr, ro_ds);
    EXPECT_EQ(1, ro_ds->GetRasterCount());
    EXPECT_EQ(GDT_Float32, ro_ds->GetRasterBand(1)->GetRasterDataType());
  }

  {
    CPLStringList open_opts;
    open_opts.AddString("ZOOM_LEVEL=0");
    auto rw_ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        path_grid_flt.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE, nullptr,
        open_opts.List(), nullptr));
    ASSERT_NE(nullptr, rw_ds);
  }

  // Int16 raster with scale and offset
  const std::string path_grid_i16 = "/vsimem/gpkg_grid_i16.gpkg";
  absl::Cleanup cl_gi16 = [&path_grid_i16] {
    VSIUnlink(path_grid_i16.c_str());
  };
  {
    auto ds_gi16 = std::unique_ptr<GDALDataset>(
        drv->Create(path_grid_i16.c_str(), 64, 64, 1, GDT_Int16, nullptr));
    ASSERT_NE(nullptr, ds_gi16);
    ds_gi16->SetSpatialRef(&srs4326);
    ds_gi16->SetGeoTransform(gt);
    ds_gi16->GetRasterBand(1)->SetScale(1.0);
    ds_gi16->GetRasterBand(1)->SetOffset(-32768.0);
    ds_gi16->GetRasterBand(1)->SetNoDataValue(65535.0);
    ds_gi16->SetMetadataItem("AREA_OR_POINT", "Area");
    std::vector<int16_t> data(64 * 64, 100);
    EXPECT_EQ(CE_None, ds_gi16->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 64, 64, data.data(), 64, 64,
                           GDT_Int16, 0, 0));
  }

  {
    auto ro_i16 = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(path_grid_i16.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, ro_i16);
  }

  // Test opening with bounds, WHERE, BAND_COUNT permutations
  {
    CPLStringList open_opts;
    open_opts.AddString("WHERE=zoom_level >= 0");
    open_opts.AddString("MINX=-180");
    open_opts.AddString("MINY=-90");
    open_opts.AddString("MAXX=180");
    open_opts.AddString("MAXY=90");
    open_opts.AddString("BAND_COUNT=2");
    auto ds_b2 = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(path_grid_flt.c_str(), GDAL_OF_RASTER, nullptr,
                          open_opts.List(), nullptr));
    EXPECT_NE(nullptr, ds_b2);
  }
}

TEST_F(GDALGeoPackageDatasetTest, BuildOverviewsZoomOtherAndClean) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  double gt[6] = {-180.0, 0.3515625, 0.0, 90.0, 0.0, -0.17578125};

  const std::string path_ov = "/vsimem/gpkg_ov.gpkg";
  absl::Cleanup cl_ov = [&path_ov] { VSIUnlink(path_ov.c_str()); };
  auto ds_ov = std::unique_ptr<GDALDataset>(
      drv->Create(path_ov.c_str(), 1024, 1024, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds_ov);
  ds_ov->SetSpatialRef(&srs4326);
  ds_ov->SetGeoTransform(gt);
  std::vector<GByte> tile(256 * 256, 100);
  EXPECT_EQ(CE_None, ds_ov->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 256, 256,
                                                       tile.data(), 256, 256,
                                                       GDT_Byte, 0, 0));

  int ov_bad = 65536;
  {
    WithQuietHandler quiet;
    EXPECT_EQ(CE_None, ds_ov->BuildOverviews("NEAREST", 1, &ov_bad, 0, nullptr,
                                             nullptr, nullptr));
  }

  // Non-power-of-two factor with zoom other disallowed
  int ov3 = 3;
  CPLSetThreadLocalConfigOption("ALLOW_GPKG_ZOOM_OTHER_EXTENSION", "NO");
  {
    WithQuietHandler quiet;
    EXPECT_NE(CE_None, ds_ov->BuildOverviews("NEAREST", 1, &ov3, 0, nullptr,
                                             nullptr, nullptr));
  }
  CPLSetThreadLocalConfigOption("ALLOW_GPKG_ZOOM_OTHER_EXTENSION", "YES");
  EXPECT_EQ(CE_None, ds_ov->BuildOverviews("NEAREST", 1, &ov3, 0, nullptr,
                                           nullptr, nullptr));
  CPLSetThreadLocalConfigOption("ALLOW_GPKG_ZOOM_OTHER_EXTENSION", nullptr);

  // Clean overviews
  EXPECT_EQ(CE_None, ds_ov->BuildOverviews("NONE", 0, nullptr, 0, nullptr,
                                           nullptr, nullptr));
}

TEST_F(GDALGeoPackageDatasetTest, RenameAndDeleteRasterAndExecuteSQLBranches) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  double gt[6] = {-180.0, 1.40625, 0.0, 90.0, 0.0, -0.703125};

  const std::string path_sql = "/vsimem/gpkg_sql_test.gpkg";
  absl::Cleanup cl_sql = [&path_sql] { VSIUnlink(path_sql.c_str()); };
  CPLStringList create_opts;
  create_opts.AddString("METADATA_TABLES=YES");
  auto ds_sql = std::unique_ptr<GDALDataset>(
      drv->Create(path_sql.c_str(), 256, 256, 1, GDT_Byte, create_opts.List()));
  ASSERT_NE(nullptr, ds_sql);
  ds_sql->SetSpatialRef(&srs4326);
  ds_sql->SetGeoTransform(gt);

  auto* lyr1 = ds_sql->CreateLayer("vec1", &srs4326, wkbPoint, nullptr);
  ASSERT_NE(nullptr, lyr1);
  OGRFieldDefn fld1("col1", OFTString);
  EXPECT_EQ(OGRERR_NONE, lyr1->CreateField(&fld1));
  OGRFieldDefn fld2("col2", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, lyr1->CreateField(&fld2));
  OGRFeature feat(lyr1->GetLayerDefn());
  feat.SetField("col1", "test_val");
  feat.SetField("col2", 42);
  OGRPoint pt(10, 20);
  feat.SetGeometry(&pt);
  EXPECT_EQ(OGRERR_NONE, lyr1->CreateFeature(&feat));

  auto* lyr2 = ds_sql->CreateLayer("vec2", &srs4326, wkbPoint, nullptr);
  ASSERT_NE(nullptr, lyr2);

  // Rename raster layer when target already exists -> fails (exercising L7153)
  {
    WithQuietHandler quiet;
    ds_sql->ExecuteSQL("ALTER TABLE gpkg_sql_test RENAME TO vec1", nullptr,
                       nullptr);
  }

  // Rename raster layer to ras2 -> succeeds (exercising L7142-L7336)
  ds_sql->ExecuteSQL("ALTER TABLE gpkg_sql_test RENAME TO ras2", nullptr,
                     nullptr);

  // Delete raster layer ras2 -> succeeds (exercising L7021-L7115)
  ds_sql->ExecuteSQL("DROP TABLE ras2", nullptr, nullptr);

  // ExecuteSQL ALTER TABLE vec1 RENAME COLUMN
  ds_sql->ExecuteSQL("ALTER TABLE vec1 RENAME COLUMN col1 TO col1_renamed",
                     nullptr, nullptr);
  EXPECT_NE(-1, lyr1->GetLayerDefn()->GetFieldIndex("col1_renamed"));

  // ALTER TABLE vec1 DROP COLUMN
  ds_sql->ExecuteSQL("ALTER TABLE vec1 DROP COLUMN col2", nullptr, nullptr);
  EXPECT_EQ(-1, lyr1->GetLayerDefn()->GetFieldIndex("col2"));

  // DELETE FROM vec1 (optimization in ExecuteSQL)
  ds_sql->ExecuteSQL("DELETE FROM vec1", nullptr, nullptr);
  EXPECT_EQ(0, lyr1->GetFeatureCount(true));

  // Dialect INDIRECT_SQLITE
  auto* indir_layer =
      ds_sql->ExecuteSQL("SELECT * FROM vec1", nullptr, "INDIRECT_SQLITE");
  if (indir_layer) ds_sql->ReleaseResultSet(indir_layer);

  // PRAGMA case_sensitive_like
  ds_sql->ExecuteSQL("PRAGMA case_sensitive_like = 0", nullptr, nullptr);
  ds_sql->ExecuteSQL("PRAGMA case_sensitive_like = 1", nullptr, nullptr);

  // SELECT nolock
  auto* nolock_layer = ds_sql->ExecuteSQL("SELECT nolock", nullptr, nullptr);
  if (nolock_layer) ds_sql->ReleaseResultSet(nolock_layer);

  // DELLAYER / RECOMPUTE EXTENT on nonexistent
  {
    WithQuietHandler quiet;
    ds_sql->ExecuteSQL("DELLAYER:nonexistent", nullptr, nullptr);
    ds_sql->ExecuteSQL("RECOMPUTE EXTENT ON nonexistent", nullptr, nullptr);
  }

  // Dialect OGRSQL
  auto* ogrsql_layer =
      ds_sql->ExecuteSQL("SELECT * FROM vec1", nullptr, "OGRSQL");
  if (ogrsql_layer) ds_sql->ReleaseResultSet(ogrsql_layer);

  // Leading semicolon and ORDER BY
  auto* order_layer = ds_sql->ExecuteSQL("  ;SELECT * FROM vec1 ORDER BY fid;",
                                         nullptr, nullptr);
  if (order_layer) ds_sql->ReleaseResultSet(order_layer);
}

TEST_F(GDALGeoPackageDatasetTest, MetadataImageStructureAndFlushMetadata) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  double gt[6] = {-180.0, 5.625, 0.0, 90.0, 0.0, -2.8125};

  auto ct_ds = std::unique_ptr<GDALDataset>(
      mem_drv->Create("", 64, 64, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ct_ds);
  ct_ds->SetSpatialRef(&srs4326);
  ct_ds->SetGeoTransform(gt);
  GDALColorTable ct;
  GDALColorEntry e0 = {0, 0, 0, 255};
  GDALColorEntry e1 = {255, 0, 0, 255};
  ct.SetColorEntry(0, &e0);
  ct.SetColorEntry(1, &e1);
  ct_ds->GetRasterBand(1)->SetColorTable(&ct);
  ct_ds->GetRasterBand(1)->SetNoDataValue(255);
  ct_ds->GetRasterBand(1)->SetMetadataItem("BAND_KEY", "BAND_VAL");
  std::vector<GByte> raster_data(64 * 64, 0);
  EXPECT_EQ(CE_None, ct_ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 64, 64,
                                                       raster_data.data(), 64,
                                                       64, GDT_Byte, 0, 0));

  // TILE_FORMAT=PNG8
  const std::string path_md = "/vsimem/gpkg_md_fmt.gpkg";
  absl::Cleanup cl_md = [&path_md] { VSIUnlink(path_md.c_str()); };
  CPLStringList md_opts;
  md_opts.AddString("TILE_FORMAT=PNG8");
  auto ds_md = std::unique_ptr<GDALDataset>(drv->CreateCopy(
      path_md.c_str(), ct_ds.get(), FALSE, md_opts.List(), nullptr, nullptr));
  ASSERT_NE(nullptr, ds_md);
  ds_md->SetMetadataItem("TEST_KEY", "TEST_VAL", "MY_DOMAIN");
  ds_md->SetMetadataItem("IDENTIFIER", "my_raster_id");
  ds_md->SetMetadataItem("DESCRIPTION", "my_raster_desc");

  auto* vlyr = ds_md->CreateLayer("vmd", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, vlyr);
  vlyr->SetMetadataItem("IDENTIFIER", "vlyr_id");
  vlyr->SetMetadataItem("DESCRIPTION", "vlyr_desc");
  vlyr->SetMetadataItem("CUSTOM_LYR_KEY", "custom_lyr_val", "LYR_DOMAIN");

  ds_md.reset();

  // Reopen and inspect
  auto ds_reopened = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      path_md.c_str(), GDAL_OF_RASTER | GDAL_OF_VECTOR | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, ds_reopened);
  EXPECT_STREQ("PNG8",
               ds_reopened->GetMetadataItem("TILE_FORMAT", "IMAGE_STRUCTURE"));
  EXPECT_STREQ("TEST_VAL",
               ds_reopened->GetMetadataItem("TEST_KEY", "MY_DOMAIN"));

  // Clear metadata to test deletion
  ds_reopened->SetMetadata(nullptr);
  ds_reopened->FlushCache();
}

TEST_F(GDALGeoPackageDatasetTest, FieldDomainsAndDataColumnsExtended) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  const std::string path_fd = "/vsimem/gpkg_fd_ext.gpkg";
  absl::Cleanup cl_fd = [&path_fd] { VSIUnlink(path_fd.c_str()); };
  auto ds_fd = std::unique_ptr<GDALDataset>(
      drv->Create(path_fd.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds_fd);

  // Coded domain with int, int64, real, string
  std::vector<OGRCodedValue> coded_vals;
  coded_vals.push_back({CPLStrdup("1"), CPLStrdup("One")});
  coded_vals.push_back({CPLStrdup("10000000000"), CPLStrdup("Ten Billion")});
  coded_vals.push_back({CPLStrdup("1.5"), CPLStrdup("One Point Five")});
  coded_vals.push_back({CPLStrdup("abc"), CPLStrdup("Alphabet")});
  coded_vals.push_back({nullptr, nullptr});
  auto coded_dom = std::make_unique<OGRCodedFieldDomain>(
      "multi_coded", "Multi Coded Domain", OFTString, OFSTNone,
      std::move(coded_vals));
  std::string failure_reason;
  EXPECT_TRUE(ds_fd->AddFieldDomain(std::move(coded_dom), failure_reason));

  // Range domains
  OGRField min_int, max_int;
  min_int.Integer = 1;
  max_int.Integer = 100;
  auto range_int_dom = std::make_unique<OGRRangeFieldDomain>(
      "range_int", "Int Range", OFTInteger, OFSTNone, min_int, true, max_int,
      true);
  EXPECT_TRUE(ds_fd->AddFieldDomain(std::move(range_int_dom), failure_reason));

  OGRField min_i64, max_i64;
  min_i64.Integer64 = 1000;
  max_i64.Integer64 = 10000000000LL;
  auto range_i64_dom = std::make_unique<OGRRangeFieldDomain>(
      "range_i64", "Int64 Range", OFTInteger64, OFSTNone, min_i64, true,
      max_i64, false);
  EXPECT_TRUE(ds_fd->AddFieldDomain(std::move(range_i64_dom), failure_reason));

  OGRField min_real, max_real;
  min_real.Real = 0.5;
  max_real.Real = 99.5;
  auto range_real_dom = std::make_unique<OGRRangeFieldDomain>(
      "range_real", "Real Range", OFTReal, OFSTNone, min_real, false, max_real,
      true);
  EXPECT_TRUE(ds_fd->AddFieldDomain(std::move(range_real_dom), failure_reason));

  // Glob domain
  auto glob_dom = std::make_unique<OGRGlobFieldDomain>(
      "glob_str", "Glob Domain", OFTString, OFSTNone, "*test*");
  EXPECT_TRUE(ds_fd->AddFieldDomain(std::move(glob_dom), failure_reason));

  // Create vector layer and fields pointing to these domains
  auto* fd_lyr = ds_fd->CreateLayer("fd_lyr", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, fd_lyr);
  OGRFieldDefn fld_c("fld_c", OFTString);
  fld_c.SetDomainName("multi_coded");
  EXPECT_EQ(OGRERR_NONE, fd_lyr->CreateField(&fld_c));

  OGRFieldDefn fld_ri("fld_ri", OFTInteger);
  fld_ri.SetDomainName("range_int");
  EXPECT_EQ(OGRERR_NONE, fd_lyr->CreateField(&fld_ri));

  OGRFieldDefn fld_ri64("fld_ri64", OFTInteger64);
  fld_ri64.SetDomainName("range_i64");
  EXPECT_EQ(OGRERR_NONE, fd_lyr->CreateField(&fld_ri64));

  OGRFieldDefn fld_rr("fld_rr", OFTReal);
  fld_rr.SetDomainName("range_real");
  EXPECT_EQ(OGRERR_NONE, fd_lyr->CreateField(&fld_rr));

  // Corrupted rows in gpkg_data_column_constraints
  ds_fd->ExecuteSQL(
      "INSERT INTO gpkg_data_column_constraints (constraint_name, "
      "constraint_type, value) "
      "VALUES ('corrupt_glob', 'glob', NULL)",
      nullptr, nullptr);
  ds_fd->ExecuteSQL(
      "INSERT INTO gpkg_data_column_constraints (constraint_name, "
      "constraint_type, value) "
      "VALUES ('corrupt_type', 'invalid_type', 'val')",
      nullptr, nullptr);

  ds_fd.reset();

  // Reopen and retrieve all domains
  auto ds_fd_ro = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(path_fd.c_str(), GDAL_OF_VECTOR));
  ASSERT_NE(nullptr, ds_fd_ro);
  EXPECT_NE(nullptr, ds_fd_ro->GetFieldDomain("multi_coded"));
  EXPECT_NE(nullptr, ds_fd_ro->GetFieldDomain("range_int"));
  EXPECT_NE(nullptr, ds_fd_ro->GetFieldDomain("range_i64"));
  EXPECT_NE(nullptr, ds_fd_ro->GetFieldDomain("range_real"));
  EXPECT_NE(nullptr, ds_fd_ro->GetFieldDomain("glob_str"));
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, ds_fd_ro->GetFieldDomain("corrupt_glob"));
    EXPECT_EQ(nullptr, ds_fd_ro->GetFieldDomain("corrupt_type"));
    EXPECT_EQ(nullptr, ds_fd_ro->GetFieldDomain("nonexistent_domain"));
  }
}

TEST_F(GDALGeoPackageDatasetTest, RelationshipsValidationAndLoadEdgeCases) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  const std::string path_rel = "/vsimem/gpkg_rel_ext.gpkg";
  absl::Cleanup cl_rel = [&path_rel] { VSIUnlink(path_rel.c_str()); };
  auto ds_rel = std::unique_ptr<GDALDataset>(
      drv->Create(path_rel.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds_rel);
  auto* left_lyr = ds_rel->CreateLayer("left_tbl", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, left_lyr);
  auto* right_lyr = ds_rel->CreateLayer("right_tbl", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, right_lyr);

  std::string failure_reason;
  // 1. Invalid related table type
  auto rel_bad_type = std::make_unique<GDALRelationship>(
      "bad_rel_type", "left_tbl", "right_tbl", GRC_MANY_TO_MANY);
  rel_bad_type->SetType(GRT_COMPOSITE);
  rel_bad_type->SetRelatedTableType("invalid_type");
  EXPECT_FALSE(
      ds_rel->AddRelationship(std::move(rel_bad_type), failure_reason));

  // 2. Non-existent left table
  auto rel_no_left = std::make_unique<GDALRelationship>(
      "no_left", "nonexistent_left", "right_tbl", GRC_MANY_TO_MANY);
  rel_no_left->SetRelatedTableType("features");
  EXPECT_FALSE(ds_rel->AddRelationship(std::move(rel_no_left), failure_reason));

  // 3. Non-existent right table
  auto rel_no_right = std::make_unique<GDALRelationship>(
      "no_right", "left_tbl", "nonexistent_right", GRC_MANY_TO_MANY);
  rel_no_right->SetRelatedTableType("features");
  EXPECT_FALSE(
      ds_rel->AddRelationship(std::move(rel_no_right), failure_reason));

  // 4. Invalid left field
  auto rel_bad_lfield = std::make_unique<GDALRelationship>(
      "bad_lf", "left_tbl", "right_tbl", GRC_MANY_TO_MANY);
  rel_bad_lfield->SetRelatedTableType("features");
  rel_bad_lfield->SetLeftTableFields({"nonexistent_field"});
  EXPECT_FALSE(
      ds_rel->AddRelationship(std::move(rel_bad_lfield), failure_reason));

  // 5. Invalid right field
  auto rel_bad_rfield = std::make_unique<GDALRelationship>(
      "bad_rf", "left_tbl", "right_tbl", GRC_MANY_TO_MANY);
  rel_bad_rfield->SetRelatedTableType("features");
  rel_bad_rfield->SetLeftTableFields({"fid"});
  rel_bad_rfield->SetRightTableFields({"nonexistent_field"});
  EXPECT_FALSE(
      ds_rel->AddRelationship(std::move(rel_bad_rfield), failure_reason));

  // 6. DeleteRelationship nonexistent
  EXPECT_FALSE(ds_rel->DeleteRelationship("nonexistent_rel", failure_reason));

  // 7. UpdateRelationship nonexistent
  auto rel_up = std::make_unique<GDALRelationship>(
      "nonexistent_rel", "left_tbl", "right_tbl", GRC_MANY_TO_MANY);
  EXPECT_FALSE(ds_rel->UpdateRelationship(std::move(rel_up), failure_reason));

  // 8. Add valid relationship
  auto rel_valid = std::make_unique<GDALRelationship>(
      "valid_rel", "left_tbl", "right_tbl", GRC_MANY_TO_MANY);
  rel_valid->SetRelatedTableType("features");
  rel_valid->SetLeftTableFields({"fid"});
  rel_valid->SetRightTableFields({"fid"});
  EXPECT_TRUE(ds_rel->AddRelationship(std::move(rel_valid), failure_reason));

  // Try updating existing relationship with altered left table name (fails)
  auto rel_up_change_left = std::make_unique<GDALRelationship>(
      "valid_rel", "right_tbl", "right_tbl", GRC_MANY_TO_MANY);
  EXPECT_FALSE(ds_rel->UpdateRelationship(std::move(rel_up_change_left),
                                          failure_reason));

  // Inject rows into gpkgext_relations for attributes and tiles
  ds_rel->ExecuteSQL(
      "INSERT INTO gpkgext_relations (relation_name, base_table_name, "
      "base_primary_column, "
      "related_table_name, related_primary_column, mapping_table_name) "
      "VALUES ('attributes', 'left_tbl', 'fid', 'right_tbl', 'fid', "
      "'valid_rel_mapping')",
      nullptr, nullptr);
  ds_rel->ExecuteSQL(
      "INSERT INTO gpkgext_relations (relation_name, base_table_name, "
      "base_primary_column, "
      "related_table_name, related_primary_column, mapping_table_name) "
      "VALUES ('tiles', 'left_tbl', 'fid', 'right_tbl', 'fid', "
      "'valid_rel_mapping')",
      nullptr, nullptr);

  ds_rel.reset();

  // Reopen and call GetRelationshipNames
  auto ds_rel_ro = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(path_rel.c_str(), GDAL_OF_VECTOR));
  ASSERT_NE(nullptr, ds_rel_ro);
  EXPECT_FALSE(ds_rel_ro->GetRelationshipNames().empty());
}

TEST_F(GDALGeoPackageDatasetTest, SpatialFilterIntersectsAndSQLFunctions) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  double gt[6] = {-180.0, 5.625, 0.0, 90.0, 0.0, -2.8125};

  CPLSetThreadLocalConfigOption("GPKG_DEBUG", "TRUE");
  const std::string path_sp = "/vsimem/gpkg_sp_func.gpkg";
  absl::Cleanup cl_sp = [&path_sp] {
    VSIUnlink(path_sp.c_str());
    CPLSetThreadLocalConfigOption("GPKG_DEBUG", nullptr);
  };
  CPLStringList sp_opts;
  sp_opts.AddString("SPATIAL_INDEX=YES");
  auto ds_sp = std::unique_ptr<GDALDataset>(
      drv->Create(path_sp.c_str(), 64, 64, 1, GDT_Byte, sp_opts.List()));
  ASSERT_NE(nullptr, ds_sp);
  ds_sp->SetSpatialRef(&srs4326);
  ds_sp->SetGeoTransform(gt);
  std::vector<GByte> px(64 * 64, 42);
  EXPECT_EQ(CE_None,
            ds_sp->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 64, 64, px.data(),
                                              64, 64, GDT_Byte, 0, 0));
  EXPECT_EQ(CE_None, ds_sp->FlushCache());

  auto* plyr =
      ds_sp->CreateLayer("poly_lyr", &srs4326, wkbPolygon, sp_opts.List());
  ASSERT_NE(nullptr, plyr);

  // Insert polygon 1: box [1,1] to [3,3]
  OGRFeature f1(plyr->GetLayerDefn());
  OGRPolygon p1;
  OGRLinearRing r1;
  r1.addPoint(1, 1);
  r1.addPoint(1, 3);
  r1.addPoint(3, 3);
  r1.addPoint(3, 1);
  r1.addPoint(1, 1);
  p1.addRing(&r1);
  f1.SetGeometry(&p1);
  EXPECT_EQ(OGRERR_NONE, plyr->CreateFeature(&f1));

  // Insert polygon 2: box [10,10] to [20,20]
  OGRFeature f2(plyr->GetLayerDefn());
  OGRPolygon p2;
  OGRLinearRing r2;
  r2.addPoint(10, 10);
  r2.addPoint(10, 20);
  r2.addPoint(20, 20);
  r2.addPoint(20, 10);
  r2.addPoint(10, 10);
  p2.addRing(&r2);
  f2.SetGeometry(&p2);
  EXPECT_EQ(OGRERR_NONE, plyr->CreateFeature(&f2));

  // Test OGR_GPKG_Intersects_Spatial_Filter
  // Case 1: envelope contains geometry envelope
  plyr->SetSpatialFilterRect(0, 0, 5, 5);
  EXPECT_EQ(1, plyr->GetFeatureCount(true));

  // Case 2: partial envelope intersection (pessimistic)
  plyr->SetSpatialFilterRect(2, 2, 2.5, 2.5);
  EXPECT_EQ(1, plyr->GetFeatureCount(true));

  // Case 3: non-rectangular geometry filter
  OGRPolygon tri;
  OGRLinearRing tr;
  tr.addPoint(1.5, 1.5);
  tr.addPoint(1.5, 2.5);
  tr.addPoint(2.5, 2.5);
  tr.addPoint(1.5, 1.5);
  tri.addRing(&tr);
  plyr->SetSpatialFilter(&tri);
  EXPECT_EQ(1, plyr->GetFeatureCount(true));

  // Reset filter
  plyr->SetSpatialFilter(nullptr);
  EXPECT_EQ(2, plyr->GetFeatureCount(true));

  // Test SQL functions
  const char* const queries[] = {
      "SELECT ST_MakeValid(geom), ST_MakeValid(NULL) FROM poly_lyr",
      "SELECT ST_Area(geom), ST_Area(geom, 1), ST_Area(NULL), ST_Area(NULL, 1) "
      "FROM poly_lyr",
      "SELECT ST_Length(geom), ST_Length(geom, 1), ST_Length(NULL) FROM "
      "poly_lyr",
      "SELECT ST_Transform(geom, 3857), ST_Transform(NULL, 3857), "
      "ST_Transform(geom, -999) FROM poly_lyr",
      "SELECT SetSRID(geom, 3857), SetSRID(NULL, 3857) FROM poly_lyr",
      "SELECT ST_EnvelopesIntersects(geom, geom), ST_EnvelopesIntersects(NULL, "
      "geom), "
      "       ST_EnvelopesIntersects(geom, 0, 0, 10, 10), "
      "ST_EnvelopesIntersects(NULL, 0, 0, 10, 10) FROM poly_lyr",
      "SELECT gdal_get_layer_pixel_value('gpkg_sp_func', 1, 0, 0, 'nearest'), "
      "       gdal_get_layer_pixel_value('unknown', 1, 0, 0, 'nearest'), "
      "gdal_get_layer_pixel_value(123, 1, 0, 0, 'nearest')",
      "SELECT GDAL_GetMimeType(tile_data), GDAL_GetBandCount(tile_data), "
      "GDAL_HasColorTable(tile_data), "
      "       GDAL_GetMimeType(NULL), GDAL_GetBandCount(NULL), "
      "GDAL_HasColorTable(NULL) FROM gpkg_sp_func",
      "SELECT ogr_layer_Extent('poly_lyr'), ogr_layer_Extent('unknown'), "
      "ogr_layer_Extent(123)",
      "SELECT CreateSpatialIndex('poly_lyr', 'geom'), "
      "HasSpatialIndex('poly_lyr', 'geom'), "
      "       DisableSpatialIndex('poly_lyr', 'geom')",
      "SELECT RegisterGeometryExtension('poly_lyr', 'geom', 'POLYGON')",
      "SELECT hstore_get_value('a=>b', 'a'), ImportFromEPSG(4326)",
  };

  for (const char* q : queries) {
    WithQuietHandler quiet;
    auto* res = ds_sp->ExecuteSQL(q, nullptr, nullptr);
    if (res) {
      while (auto* f = res->GetNextFeature()) {
        OGRFeature::DestroyFeature(f);
      }
      ds_sp->ReleaseResultSet(res);
    }
  }
  CPLSetThreadLocalConfigOption("GPKG_DEBUG", nullptr);
}

TEST_F(GDALGeoPackageDatasetTest,
       TriggerFixupsUnknownExtensionsAndLayerOptions) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  const std::string path_opts = "/vsimem/gpkg_opts_ext.gpkg";
  absl::Cleanup cl_opts = [&path_opts] { VSIUnlink(path_opts.c_str()); };
  auto ds_opts = std::unique_ptr<GDALDataset>(
      drv->Create(path_opts.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds_opts);
  auto* gds = dynamic_cast<GDALGeoPackageDataset*>(ds_opts.get());
  ASSERT_NE(nullptr, gds);

  // ICreateLayer options
  CPLStringList lyr_opts;
  lyr_opts.AddString("IDENTIFIER=lyr_one");
  auto* l1 = ds_opts->CreateLayer("l1", nullptr, wkbNone, lyr_opts.List());
  ASSERT_NE(nullptr, l1);

  // Duplicate IDENTIFIER -> fails
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              ds_opts->CreateLayer("l2", nullptr, wkbNone, lyr_opts.List()));
  }

  // ASPATIAL_VARIANT options
  {
    WithQuietHandler quiet;
    CPLStringList aspat_bad;
    aspat_bad.AddString("ASPATIAL_VARIANT=OGR_ASPATIAL");
    EXPECT_EQ(nullptr, ds_opts->CreateLayer("l_bad_aspat", nullptr, wkbNone,
                                            aspat_bad.List()));

    CPLStringList aspat_notreg;
    aspat_notreg.AddString("ASPATIAL_VARIANT=NOT_REGISTERED");
    EXPECT_NE(nullptr, ds_opts->CreateLayer("l_notreg", nullptr, wkbNone,
                                            aspat_notreg.List()));
  }

  // DATETIME_PRECISION options
  {
    CPLStringList dt_ms;
    dt_ms.AddString("DATETIME_PRECISION=MILLISECOND");
    EXPECT_NE(nullptr,
              ds_opts->CreateLayer("l_dt_ms", nullptr, wkbNone, dt_ms.List()));

    CPLStringList dt_sec;
    dt_sec.AddString("DATETIME_PRECISION=SECOND");
    EXPECT_NE(nullptr, ds_opts->CreateLayer("l_dt_sec", nullptr, wkbNone,
                                            dt_sec.List()));

    CPLStringList dt_min;
    dt_min.AddString("DATETIME_PRECISION=MINUTE");
    EXPECT_NE(nullptr, ds_opts->CreateLayer("l_dt_min", nullptr, wkbNone,
                                            dt_min.List()));

    WithQuietHandler quiet;
    CPLStringList dt_bad;
    dt_bad.AddString("DATETIME_PRECISION=INVALID");
    EXPECT_EQ(nullptr, ds_opts->CreateLayer("l_dt_bad", nullptr, wkbNone,
                                            dt_bad.List()));
  }

  // CheckUnknownExtensions: inject rows into gpkg_extensions
  ds_opts->ExecuteSQL(
      "INSERT INTO gpkg_extensions (table_name, column_name, extension_name, "
      "definition, scope) "
      "VALUES ('l1', 'geom', 'gpkg_zoom_other', 'http://example.com', "
      "'read-write')",
      nullptr, nullptr);
  ds_opts->ExecuteSQL(
      "INSERT INTO gpkg_extensions (table_name, column_name, extension_name, "
      "definition, scope) "
      "VALUES ('l1', 'geom', 'unknown_write_only_ext', 'http://example.com', "
      "'write-only')",
      nullptr, nullptr);
  ds_opts->ExecuteSQL(
      "INSERT INTO gpkg_extensions (table_name, column_name, extension_name, "
      "definition, scope) "
      "VALUES ('l1', 'geom', 'unknown_rw_ext', 'http://example.com', "
      "'read-write')",
      nullptr, nullptr);

  ds_opts.reset();

  // Reopen in read-only and update mode
  {
    WithQuietHandler quiet;
    auto ro = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(path_opts.c_str(), GDAL_OF_VECTOR));
    EXPECT_NE(nullptr, ro);

    auto rw = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(path_opts.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    EXPECT_NE(nullptr, rw);
  }
}

TEST_F(GDALGeoPackageDatasetTest, GetSrsIdEdgeCasesAndSqliteMasterBatchLoad) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  const std::string path = "/vsimem/gpkg_srs_boost.gpkg";
  absl::Cleanup cl = [&path] { VSIUnlink(path.c_str()); };

  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  auto* gds = dynamic_cast<GDALGeoPackageDataset*>(ds.get());
  ASSERT_NE(nullptr, gds);

  // 1. ReOpenDB via VACUUM
  ds->ExecuteSQL("VACUUM", nullptr, nullptr);

  // 2. GetSpatialRef on special/edge IDs
  // Undefined geographic SRS (id 0)
  auto srs0 = gds->GetSpatialRef(0);
  EXPECT_NE(nullptr, srs0);

  // Non-existent SRS ID (warning and returns nullptr)
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, gds->GetSpatialRef(999999));
  }

  // Corrupt WKT in gpkg_spatial_ref_sys to trigger parse failure
  gds->ExecuteSQL(
      "INSERT INTO gpkg_spatial_ref_sys (srs_name, srs_id, organization, "
      "organization_coordsys_id, definition) "
      "VALUES ('bad_wkt', 88888, 'TEST', 88888, "
      "'INVALID_WKT_CONTENT_THAT_FAILS_PARSE')",
      nullptr, nullptr);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, gds->GetSpatialRef(88888));
  }

  // 3. Add definition_12_063 and gpkg_crs_wkt, reopen, then GetSrsId with
  // CoordinateEpoch -> covers L850-878
  EXPECT_EQ(OGRERR_NONE, gds->CreateExtensionsTableIfNecessary());
  gds->ExecuteSQL(
      "ALTER TABLE gpkg_spatial_ref_sys ADD COLUMN definition_12_063 TEXT",
      nullptr, nullptr);
  gds->ExecuteSQL(
      "INSERT INTO gpkg_extensions (table_name, column_name, extension_name, "
      "definition, scope) "
      "VALUES ('gpkg_spatial_ref_sys', 'definition_12_063', 'gpkg_crs_wkt', "
      "'http://example.com', 'read-write')",
      nullptr, nullptr);
  ds.reset();

  ds = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, ds);
  gds = dynamic_cast<GDALGeoPackageDataset*>(ds.get());
  ASSERT_NE(nullptr, gds);

  OGRSpatialReference srs_epoch;
  srs_epoch.importFromEPSG(4326);
  srs_epoch.SetCoordinateEpoch(2022.5);
  int srsid_epoch = gds->GetSrsId(&srs_epoch);
  EXPECT_GT(srsid_epoch, 0);

  // 4. Unnamed SRS (e.g. Proj4 string without name)
  OGRSpatialReference unnamed_srs;
  unnamed_srs.importFromProj4(
      "+proj=laea +lat_0=45 +lon_0=-100 +x_0=0 +y_0=0 +a=6378137 +b=6378137 "
      "+units=m +no_defs");
  int srsid_unnamed = gds->GetSrsId(&unnamed_srs);
  EXPECT_GT(srsid_unnamed, 0);

  // 5. OGR_GPKG_CHECK_SRS=YES branch and incompatible SRS warning
  CPLSetThreadLocalConfigOption("OGR_GPKG_CHECK_SRS", "YES");
  OGRSpatialReference srs_check;
  srs_check.importFromEPSG(4326);
  EXPECT_GT(gds->GetSrsId(&srs_check), 0);

  {
    WithQuietHandler quiet;
    OGRSpatialReference srs_bad_epsg;
    srs_bad_epsg.importFromEPSG(4326);
    srs_bad_epsg.SetLinearUnits("foot", 0.3048);
    srs_bad_epsg.SetAuthority("GEODCRS", "EPSG", 4326);
    EXPECT_GT(gds->GetSrsId(&srs_bad_epsg), 0);
  }
  CPLSetThreadLocalConfigOption("OGR_GPKG_CHECK_SRS", nullptr);
}

TEST_F(GDALGeoPackageDatasetTest, ManyLayersThresholdAndMasterContent) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  const std::string path = "/vsimem/gpkg_many_lyrs.gpkg";
  absl::Cleanup cl = [&path] { VSIUnlink(path.c_str()); };

  // Create dataset with 12 layers (threshold for GetSqliteMasterContent is 10)
  {
    auto ds = std::unique_ptr<GDALDataset>(
        drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    for (int i = 0; i < 12; ++i) {
      std::string name = CPLSPrintf("layer_%02d", i);
      auto* lyr = ds->CreateLayer(name.c_str(), nullptr, wkbPoint, nullptr);
      ASSERT_NE(nullptr, lyr);
      OGRFieldDefn fld("val", OFTInteger);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld));
      OGRFeature feat(lyr->GetLayerDefn());
      feat.SetField("val", i);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));
    }
  }

  // Reopen and access layers to trigger GetSqliteMasterContent
  {
    auto ds_ro = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, ds_ro);
    EXPECT_EQ(12, ds_ro->GetLayerCount());
    for (int i = 0; i < 12; ++i) {
      auto* lyr = ds_ro->GetLayer(i);
      ASSERT_NE(nullptr, lyr);
      EXPECT_EQ(1, lyr->GetFeatureCount(true));
    }
  }

  // Reopen with OGR_TABLE_LIMIT=2 config option (triggers warning and table
  // limit)
  {
    WithQuietHandler quiet;
    CPLSetThreadLocalConfigOption("OGR_TABLE_LIMIT", "2");
    auto ds_lim = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR));
    CPLSetThreadLocalConfigOption("OGR_TABLE_LIMIT", nullptr);
    ASSERT_NE(nullptr, ds_lim);
    EXPECT_EQ(2, ds_lim->GetLayerCount());
  }
}

TEST_F(GDALGeoPackageDatasetTest, CreateCopyRasterIOAndOverviews) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  // 1. CreateCopy of vector-only dataset (nBands == 0) -> hits L5905-5915
  const std::string path_vec_src = "/vsimem/gpkg_src_vec.gpkg";
  absl::Cleanup cl_vs = [&path_vec_src] { VSIUnlink(path_vec_src.c_str()); };
  {
    auto ds_v = std::unique_ptr<GDALDataset>(
        drv->Create(path_vec_src.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds_v);
    auto* l = ds_v->CreateLayer("vl", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, l);
    OGRFieldDefn f("f", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, l->CreateField(&f));
  }
  auto ds_v_ro = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(path_vec_src.c_str(), GDAL_OF_VECTOR));
  ASSERT_NE(nullptr, ds_v_ro);

  const std::string path_vec_dst = "/vsimem/gpkg_dst_vec.gpkg";
  absl::Cleanup cl_vd = [&path_vec_dst] { VSIUnlink(path_vec_dst.c_str()); };
  auto ds_v_copied = std::unique_ptr<GDALDataset>(drv->CreateCopy(
      path_vec_dst.c_str(), ds_v_ro.get(), FALSE, nullptr, nullptr, nullptr));
  EXPECT_NE(nullptr, ds_v_copied);

  // 2. CreateCopy of raster with UnitType and without RASTER_TABLE option ->
  // hits L5925-5928 and L5942-5943
  auto mem_ds = std::unique_ptr<GDALDataset>(mem_drv->Create(
      "/vsimem/my_named_raster", 128, 128, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mem_ds);
  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  mem_ds->SetSpatialRef(&srs4326);
  double gt[6] = {-180.0, 0.5, 0.0, 90.0, 0.0, -0.5};
  mem_ds->SetGeoTransform(gt);
  mem_ds->GetRasterBand(1)->SetUnitType("m");
  std::vector<GByte> px(128 * 128, 10);
  EXPECT_EQ(CE_None,
            mem_ds->GetRasterBand(1)->RasterIO(
                GF_Write, 0, 0, 128, 128, px.data(), 128, 128, GDT_Byte, 0, 0));

  const std::string path_uom = "/vsimem/gpkg_uom.gpkg";
  absl::Cleanup cl_uom = [&path_uom] { VSIUnlink(path_uom.c_str()); };
  auto ds_uom = std::unique_ptr<GDALDataset>(drv->CreateCopy(
      path_uom.c_str(), mem_ds.get(), FALSE, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds_uom);

  // 3. CreateCopy with TILING_SCHEME=GoogleCRS84Quad without explicit
  // ZOOM_LEVEL (hits L6139-L6175 auto zoom search and strategies)
  const std::string path_zsearch = "/vsimem/gpkg_zsearch.gpkg";
  absl::Cleanup cl_zs = [&path_zsearch] { VSIUnlink(path_zsearch.c_str()); };
  CPLStringList opts_zs;
  opts_zs.AddString("TILING_SCHEME=GoogleCRS84Quad");
  opts_zs.AddString("ZOOM_LEVEL_STRATEGY=LOWER");
  auto ds_zs = std::unique_ptr<GDALDataset>(
      drv->CreateCopy(path_zsearch.c_str(), mem_ds.get(), FALSE, opts_zs.List(),
                      nullptr, nullptr));
  EXPECT_NE(nullptr, ds_zs);

  opts_zs.SetNameValue("ZOOM_LEVEL_STRATEGY", "UPPER");
  const std::string path_zs_up = "/vsimem/gpkg_zs_up.gpkg";
  absl::Cleanup cl_zsu = [&path_zs_up] { VSIUnlink(path_zs_up.c_str()); };
  auto ds_zsu = std::unique_ptr<GDALDataset>(
      drv->CreateCopy(path_zs_up.c_str(), mem_ds.get(), FALSE, opts_zs.List(),
                      nullptr, nullptr));
  EXPECT_NE(nullptr, ds_zsu);

  // 4. IRasterIO dataset level read/write
  std::vector<GByte> buf(64 * 64);
  EXPECT_EQ(CE_None, ds_uom->RasterIO(GF_Read, 0, 0, 64, 64, buf.data(), 64, 64,
                                      GDT_Byte, 1, nullptr, 0, 0, 0, nullptr));
}

TEST_F(GDALGeoPackageDatasetTest, SpatialFilterOptimizationAndFaultyTriggers) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  const std::string path = "/vsimem/gpkg_sp_triggers.gpkg";
  absl::Cleanup cl = [&path] { VSIUnlink(path.c_str()); };

  // Create dataset with spatial index
  {
    auto ds = std::unique_ptr<GDALDataset>(
        drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    CPLStringList lyr_opts;
    lyr_opts.AddString("SPATIAL_INDEX=YES");
    auto* lyr =
        ds->CreateLayer("poly_lyr", &srs4326, wkbPolygon, lyr_opts.List());
    ASSERT_NE(nullptr, lyr);

    // Box [1,1] to [3,3]
    OGRFeature f1(lyr->GetLayerDefn());
    OGRPolygon p1;
    OGRLinearRing r1;
    r1.addPoint(1, 1);
    r1.addPoint(1, 3);
    r1.addPoint(3, 3);
    r1.addPoint(3, 1);
    r1.addPoint(1, 1);
    p1.addRing(&r1);
    f1.SetGeometry(&p1);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&f1));

    // Box [10,10] to [20,20]
    OGRFeature f2(lyr->GetLayerDefn());
    OGRPolygon p2;
    OGRLinearRing r2;
    r2.addPoint(10, 10);
    r2.addPoint(10, 20);
    r2.addPoint(20, 20);
    r2.addPoint(20, 10);
    r2.addPoint(10, 10);
    p2.addRing(&r2);
    f2.SetGeometry(&p2);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&f2));
  }

  // Reopen: spatial index is now active (!m_bDeferredSpatialIndexCreation)
  {
    auto ds_reopened = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, ds_reopened);
    auto* lyr = ds_reopened->GetLayerByName("poly_lyr");
    ASSERT_NE(nullptr, lyr);

    // Spatial filter (Contains) -> hits OGR_GPKG_Intersects_Spatial_Filter
    lyr->SetSpatialFilterRect(0, 0, 5, 5);
    EXPECT_EQ(1, lyr->GetFeatureCount(true));

    // Pessimistic filter
    lyr->SetSpatialFilterRect(2.0, 2.0, 2.5, 2.5);
    EXPECT_EQ(1, lyr->GetFeatureCount(true));

    // Arbitrary geometry filter
    OGRPolygon tri;
    OGRLinearRing tr;
    tr.addPoint(1.5, 1.5);
    tr.addPoint(1.5, 2.5);
    tr.addPoint(2.5, 2.5);
    tr.addPoint(1.5, 1.5);
    tri.addRing(&tr);
    lyr->SetSpatialFilter(&tri);
    EXPECT_EQ(1, lyr->GetFeatureCount(true));

    lyr->SetSpatialFilter(nullptr);

    // Inject faulty triggers into sqlite_master to exercise
    // FixupWrongRTreeTrigger and FixupWrongMedataReferenceColumnNameUpdate on
    // next reopen
    ds_reopened->ExecuteSQL(
        "CREATE TRIGGER rtree_poly_lyr_geom_update3 AFTER UPDATE OF \"geom\" "
        "ON poly_lyr "
        "BEGIN SELECT 1; END;",
        nullptr, nullptr);
    ds_reopened->ExecuteSQL(
        "CREATE TRIGGER rtree_poly_lyr_geom_unquoted_update3 AFTER UPDATE OF "
        "geom ON poly_lyr "
        "BEGIN SELECT 1; END;",
        nullptr, nullptr);
    ds_reopened->ExecuteSQL(
        "CREATE TRIGGER gpkg_metadata_reference_column_name_update AFTER "
        "UPDATE ON poly_lyr "
        "BEGIN SELECT 'column_nameIS'; END;",
        nullptr, nullptr);
  }

  // Reopening in update mode triggers FixupWrongRTreeTrigger and
  // FixupWrongMedataReferenceColumnNameUpdate
  {
    auto ds_fix = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, ds_fix);
  }
}

TEST_F(GDALGeoPackageDatasetTest, RasterAncillaryAndOpenRasterModes) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  double gt[6] = {-180.0, 1.40625, 0.0, 90.0, 0.0, -0.703125};

  const std::string path = "/vsimem/gpkg_ancillary_test.gpkg";
  absl::Cleanup cl = [&path] { VSIUnlink(path.c_str()); };

  {
    CPLStringList create_opts;
    create_opts.AddString("METADATA_TABLES=YES");
    auto ds = std::unique_ptr<GDALDataset>(
        drv->Create(path.c_str(), 512, 512, 1, GDT_Byte, create_opts.List()));
    ASSERT_NE(nullptr, ds);
    ds->SetSpatialRef(&srs4326);
    ds->SetGeoTransform(gt);
    std::vector<GByte> tile(512 * 512, 50);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 512, 512,
                                                      tile.data(), 512, 512,
                                                      GDT_Byte, 0, 0));

    // Create ancillary tables referencing table name "gpkg_ancillary_test"
    ds->ExecuteSQL(
        "CREATE TABLE gpkg_2d_gridded_coverage_ancillary ("
        "id INTEGER PRIMARY KEY, tile_matrix_set_name TEXT, datatype TEXT);",
        nullptr, nullptr);
    ds->ExecuteSQL(
        "INSERT INTO gpkg_2d_gridded_coverage_ancillary (tile_matrix_set_name, "
        "datatype) "
        "VALUES ('gpkg_ancillary_test', 'float');",
        nullptr, nullptr);

    ds->ExecuteSQL(
        "CREATE TABLE gpkg_2d_gridded_tile_ancillary ("
        "id INTEGER PRIMARY KEY, tpudt_name TEXT);",
        nullptr, nullptr);
    ds->ExecuteSQL(
        "INSERT INTO gpkg_2d_gridded_tile_ancillary (tpudt_name) "
        "VALUES ('gpkg_ancillary_test');",
        nullptr, nullptr);

    ds->ExecuteSQL(
        "CREATE TABLE layer_styles (f_table_name TEXT, styleName TEXT);",
        nullptr, nullptr);
    ds->ExecuteSQL(
        "INSERT INTO layer_styles (f_table_name, styleName) VALUES "
        "('gpkg_ancillary_test', 'style1');",
        nullptr, nullptr);

    ds->ExecuteSQL(
        "CREATE TABLE gpkgext_relations (relation_name TEXT, base_table_name "
        "TEXT, "
        "base_primary_column TEXT, related_table_name TEXT, "
        "related_primary_column TEXT, "
        "mapping_table_name TEXT);",
        nullptr, nullptr);
    ds->ExecuteSQL(
        "INSERT INTO gpkgext_relations (base_table_name, related_table_name, "
        "mapping_table_name) "
        "VALUES ('gpkg_ancillary_test', 'other', 'map_tbl');",
        nullptr, nullptr);

    // Build overviews so reopening tests overview loading loop
    int ov2 = 2;
    EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ov2, 0, nullptr,
                                          nullptr, nullptr));

    // Rename raster table: hits all HasGriddedCoverageAncillaryTable,
    // HasQGISLayerStyles, HasGpkgextRelationsTable
    ds->ExecuteSQL("ALTER TABLE gpkg_ancillary_test RENAME TO ras_renamed",
                   nullptr, nullptr);

    // Delete raster table: hits HasGriddedCoverageAncillaryTable deletion
    ds->ExecuteSQL("DROP TABLE ras_renamed", nullptr, nullptr);
  }

  // Test opening raster with overviews, TILE_FORMAT=WEBP in update mode, and
  // GRID_CELL_ENCODING
  const std::string path_ov_reopen = "/vsimem/gpkg_ov_reopen.gpkg";
  absl::Cleanup cl_or = [&path_ov_reopen] {
    VSIUnlink(path_ov_reopen.c_str());
  };
  {
    auto ds_or = std::unique_ptr<GDALDataset>(
        drv->Create(path_ov_reopen.c_str(), 512, 512, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds_or);
    ds_or->SetSpatialRef(&srs4326);
    ds_or->SetGeoTransform(gt);
    std::vector<GByte> tile(256 * 256, 80);
    EXPECT_EQ(CE_None, ds_or->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 256,
                                                         256, tile.data(), 256,
                                                         256, GDT_Byte, 0, 0));
    int ov2 = 2;
    EXPECT_EQ(CE_None, ds_or->BuildOverviews("NEAREST", 1, &ov2, 0, nullptr,
                                             nullptr, nullptr));
  }

  // Reopen in update mode with TILE_FORMAT=PNG and then TILE_FORMAT=WEBP
  {
    CPLStringList open_opts;
    open_opts.AddString("TILE_FORMAT=PNG");
    open_opts.AddString("GRID_CELL_ENCODING=grid-value-is-area");
    auto rw_ov = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        path_ov_reopen.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE, nullptr,
        open_opts.List(), nullptr));
    ASSERT_NE(nullptr, rw_ov);
    EXPECT_EQ(1, rw_ov->GetRasterBand(1)->GetOverviewCount());
  }
}

TEST_F(GDALGeoPackageDatasetTest, RelationshipAndFieldDomainValidation) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  const std::string path = "/vsimem/gpkg_rel_boost.gpkg";
  absl::Cleanup cl = [&path] { VSIUnlink(path.c_str()); };

  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  auto* l_left = ds->CreateLayer("left_t", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, l_left);
  auto* l_right = ds->CreateLayer("right_t", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, l_right);
  auto* l_other = ds->CreateLayer("other_t", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, l_other);
  auto* l_map_pre =
      ds->CreateLayer("left_t_right_t", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, l_map_pre);

  std::string failure_reason;
  // 1. Add valid relationship. Generated name is "left_t_right_t_features".
  auto rel1 = std::make_unique<GDALRelationship>("rel1", "left_t", "right_t",
                                                 GRC_MANY_TO_MANY);
  rel1->SetRelatedTableType("features");
  rel1->SetLeftTableFields({"fid"});
  rel1->SetRightTableFields({"fid"});
  EXPECT_TRUE(ds->AddRelationship(std::move(rel1), failure_reason));

  // 2. Duplicate relationship name (name is derived from left/right/type)
  auto rel_dup = std::make_unique<GDALRelationship>(
      "rel_dup", "left_t", "right_t", GRC_MANY_TO_MANY);
  rel_dup->SetRelatedTableType("features");
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_dup), failure_reason));
  EXPECT_THAT(failure_reason, ::testing::HasSubstr("already exists"));

  // Empty and multiple left/right table fields -> L2225-2258
  auto rel_empty_l = std::make_unique<GDALRelationship>(
      "rel_el", "other_t", "right_t", GRC_MANY_TO_MANY);
  rel_empty_l->SetRelatedTableType("features");
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_empty_l), failure_reason));

  auto rel_multi_l = std::make_unique<GDALRelationship>(
      "rel_ml", "other_t", "right_t", GRC_MANY_TO_MANY);
  rel_multi_l->SetRelatedTableType("features");
  rel_multi_l->SetLeftTableFields({"fid", "fid2"});
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_multi_l), failure_reason));

  auto rel_empty_r = std::make_unique<GDALRelationship>(
      "rel_er", "other_t", "right_t", GRC_MANY_TO_MANY);
  rel_empty_r->SetRelatedTableType("features");
  rel_empty_r->SetLeftTableFields({"fid"});
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_empty_r), failure_reason));

  auto rel_multi_r = std::make_unique<GDALRelationship>(
      "rel_mr", "other_t", "right_t", GRC_MANY_TO_MANY);
  rel_multi_r->SetRelatedTableType("features");
  rel_multi_r->SetLeftTableFields({"fid"});
  rel_multi_r->SetRightTableFields({"fid", "fid2"});
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_multi_r), failure_reason));

  // Empty RelatedTableType defaults to "features" -> L10251-10254
  auto rel_def_type = std::make_unique<GDALRelationship>(
      "rel_dt", "right_t", "other_t", GRC_MANY_TO_MANY);
  rel_def_type->SetLeftTableFields({"fid"});
  rel_def_type->SetRightTableFields({"fid"});
  EXPECT_TRUE(ds->AddRelationship(std::move(rel_def_type), failure_reason));

  // 3. Add second relationship with different related table and unique key
  // constraints
  ds->ExecuteSQL("CREATE UNIQUE INDEX idx_left_fid ON left_t(fid)", nullptr,
                 nullptr);
  ds->ExecuteSQL("CREATE UNIQUE INDEX idx_other_fid ON other_t(fid)", nullptr,
                 nullptr);

  auto rel2 = std::make_unique<GDALRelationship>("rel2", "left_t", "other_t",
                                                 GRC_MANY_TO_MANY);
  rel2->SetRelatedTableType("features");
  rel2->SetLeftTableFields({"fid"});
  rel2->SetRightTableFields({"fid"});
  EXPECT_TRUE(ds->AddRelationship(std::move(rel2), failure_reason));

  // 4. Update relationship: changing left table, right table, mapping table
  // The existing relationship name in dataset is "left_t_right_t_features"
  auto rel_chg_left = std::make_unique<GDALRelationship>(
      "left_t_right_t_features", "right_t", "right_t", GRC_MANY_TO_MANY);
  rel_chg_left->SetLeftTableFields({"fid"});
  rel_chg_left->SetRightTableFields({"fid"});
  rel_chg_left->SetRelatedTableType("features");
  EXPECT_FALSE(ds->UpdateRelationship(std::move(rel_chg_left), failure_reason));
  EXPECT_THAT(failure_reason, ::testing::HasSubstr("Cannot change base table"));

  auto rel_chg_right = std::make_unique<GDALRelationship>(
      "left_t_right_t_features", "left_t", "left_t", GRC_MANY_TO_MANY);
  rel_chg_right->SetLeftTableFields({"fid"});
  rel_chg_right->SetRightTableFields({"fid"});
  rel_chg_right->SetRelatedTableType("features");
  EXPECT_FALSE(
      ds->UpdateRelationship(std::move(rel_chg_right), failure_reason));
  EXPECT_THAT(failure_reason,
              ::testing::HasSubstr("Cannot change related table"));

  auto rel_chg_map = std::make_unique<GDALRelationship>(
      "left_t_right_t_features", "left_t", "right_t", GRC_MANY_TO_MANY);
  rel_chg_map->SetLeftTableFields({"fid"});
  rel_chg_map->SetRightTableFields({"fid"});
  rel_chg_map->SetRelatedTableType("features");
  rel_chg_map->SetMappingTableName("other_map");
  EXPECT_FALSE(ds->UpdateRelationship(std::move(rel_chg_map), failure_reason));
  EXPECT_THAT(failure_reason,
              ::testing::HasSubstr("Cannot change mapping table"));

  // Non-existent mapping table -> L10418-10420
  auto rel_bad_map = std::make_unique<GDALRelationship>(
      "rel_bad_map", "other_t", "right_t", GRC_MANY_TO_MANY);
  rel_bad_map->SetLeftTableFields({"fid"});
  rel_bad_map->SetRightTableFields({"fid"});
  rel_bad_map->SetRelatedTableType("features");
  rel_bad_map->SetMappingTableName("non_existent_map_table");
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_bad_map), failure_reason));
  EXPECT_THAT(failure_reason, ::testing::HasSubstr("Could not retrieve table"));

  // 5. Add relationship with custom mapping table that is missing base_id or
  // related_id
  auto* lyr_no_base =
      ds->CreateLayer("custom_map_no_base", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, lyr_no_base);
  OGRFieldDefn f_rel("related_id", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, lyr_no_base->CreateField(&f_rel));

  auto rel_no_base = std::make_unique<GDALRelationship>(
      "rel_nobase", "other_t", "left_t", GRC_MANY_TO_MANY);
  rel_no_base->SetRelatedTableType("features");
  rel_no_base->SetMappingTableName("custom_map_no_base");
  rel_no_base->SetLeftTableFields({"fid"});
  rel_no_base->SetRightTableFields({"fid"});
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_no_base), failure_reason));
  EXPECT_THAT(failure_reason, ::testing::HasSubstr("base_id must exist"));

  auto* lyr_no_rel =
      ds->CreateLayer("custom_map_no_rel", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, lyr_no_rel);
  OGRFieldDefn f_base("base_id", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, lyr_no_rel->CreateField(&f_base));

  auto rel_no_rel = std::make_unique<GDALRelationship>(
      "rel_norel", "right_t", "left_t", GRC_MANY_TO_MANY);
  rel_no_rel->SetRelatedTableType("features");
  rel_no_rel->SetMappingTableName("custom_map_no_rel");
  rel_no_rel->SetLeftTableFields({"fid"});
  rel_no_rel->SetRightTableFields({"fid"});
  EXPECT_FALSE(ds->AddRelationship(std::move(rel_no_rel), failure_reason));
  EXPECT_THAT(failure_reason, ::testing::HasSubstr("related_id must exist"));

  // 6. Relationship operations on read-only dataset -> covers L10195, L10471,
  // L10521
  ds.reset();
  {
    WithQuietHandler quiet;
    auto ds_ro = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, ds_ro);
    auto rel_ro = std::make_unique<GDALRelationship>(
        "r_ro", "left_t", "right_t", GRC_MANY_TO_MANY);
    EXPECT_FALSE(ds_ro->AddRelationship(std::move(rel_ro), failure_reason));
    auto rel_ro_up = std::make_unique<GDALRelationship>(
        "left_t_right_t_features", "left_t", "right_t", GRC_MANY_TO_MANY);
    EXPECT_FALSE(
        ds_ro->UpdateRelationship(std::move(rel_ro_up), failure_reason));
    EXPECT_FALSE(
        ds_ro->DeleteRelationship("left_t_right_t_features", failure_reason));
  }
}

TEST_F(GDALGeoPackageDatasetTest, LegacyVersionsSubdatasetsAndSqlErrorPaths) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);

  // -------------------------------------------------------------------------
  // Part A: Dataset Creation, Versions, and Open Options Failures / Warnings
  // -------------------------------------------------------------------------
  {
    WithQuietHandler quiet;
    // 1. Version 1.0 and 1.1 creation & reopening
    const std::string path_v10 = "/vsimem/gpkg_booster_v10.gpkg";
    absl::Cleanup cl_v10 = [&path_v10] { VSIUnlink(path_v10.c_str()); };
    CPLStringList v10_opts;
    v10_opts.AddString("VERSION=1.0");
    auto ds_v10 = std::unique_ptr<GDALDataset>(
        drv->Create(path_v10.c_str(), 0, 0, 0, GDT_Unknown, v10_opts.List()));
    ASSERT_NE(nullptr, ds_v10);
    auto* v10_lyr = ds_v10->CreateLayer("v10_lyr", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, v10_lyr);
    ds_v10->ExecuteSQL(
        "CREATE TABLE gpkg_data_column_constraints ("
        "constraint_name TEXT, constraint_type TEXT, value TEXT, "
        "min NUMERIC, minIsInclusive BOOLEAN, max NUMERIC, maxIsInclusive "
        "BOOLEAN, description TEXT);",
        nullptr, nullptr);
    ds_v10.reset();

    {
      auto ds_v10_rw = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(path_v10.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
      ASSERT_NE(nullptr, ds_v10_rw);
      std::string failure_reason;
      OGRField min_val, max_val;
      min_val.Integer = 1;
      max_val.Integer = 10;
      auto rdom_v10 = std::make_unique<OGRRangeFieldDomain>(
          "r_v10", "desc", OFTInteger, OFSTNone, min_val, true, max_val, true);
      EXPECT_TRUE(
          ds_v10_rw->AddFieldDomain(std::move(rdom_v10), failure_reason));
    }

    const std::string path_v11 = "/vsimem/gpkg_booster_v11.gpkg";
    absl::Cleanup cl_v11 = [&path_v11] { VSIUnlink(path_v11.c_str()); };
    CPLStringList v11_opts;
    v11_opts.AddString("VERSION=1.1");
    auto ds_v11 = std::unique_ptr<GDALDataset>(
        drv->Create(path_v11.c_str(), 0, 0, 0, GDT_Unknown, v11_opts.List()));
    ASSERT_NE(nullptr, ds_v11);
    auto* v11_lyr = ds_v11->CreateLayer("v11_lyr", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, v11_lyr);
    ds_v11.reset();
    auto ds_v11_ro = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(path_v11.c_str(), GDAL_OF_VECTOR));
    if (ds_v11_ro) ds_v11_ro.reset();

    // 2. Raster create failures / edge options
    const std::string path_rfail = "/vsimem/gpkg_booster_rfail.gpkg";
    absl::Cleanup cl_rfail = [&path_rfail] { VSIUnlink(path_rfail.c_str()); };

    // RASTER_TABLE empty
    VSIUnlink(path_rfail.c_str());
    CPLStringList rtbl_empty;
    rtbl_empty.AddString("RASTER_TABLE=");
    EXPECT_EQ(nullptr, drv->Create(path_rfail.c_str(), 64, 64, 1, GDT_Byte,
                                   rtbl_empty.List()));

    // Invalid block sizes
    VSIUnlink(path_rfail.c_str());
    CPLStringList blk_huge;
    blk_huge.AddString("BLOCKXSIZE=100000");
    EXPECT_EQ(nullptr, drv->Create(path_rfail.c_str(), 64, 64, 1, GDT_Byte,
                                   blk_huge.List()));

    VSIUnlink(path_rfail.c_str());
    CPLStringList blk_tiny;
    blk_tiny.AddString("BLOCKYSIZE=4");
    EXPECT_EQ(nullptr, drv->Create(path_rfail.c_str(), 64, 64, 1, GDT_Byte,
                                   blk_tiny.List()));

    // Tiling scheme invalid tile size
    VSIUnlink(path_rfail.c_str());
    CPLStringList ts_bad_blk;
    ts_bad_blk.AddString("TILING_SCHEME=GoogleCRS84Quad");
    ts_bad_blk.AddString("BLOCKXSIZE=512");
    EXPECT_EQ(nullptr, drv->Create(path_rfail.c_str(), 64, 64, 1, GDT_Byte,
                                   ts_bad_blk.List()));

    // Tiling scheme invalid zoom level
    VSIUnlink(path_rfail.c_str());
    CPLStringList ts_bad_zl;
    ts_bad_zl.AddString("TILING_SCHEME=GoogleCRS84Quad");
    ts_bad_zl.AddString("ZOOM_LEVEL=99");
    EXPECT_EQ(nullptr, drv->Create(path_rfail.c_str(), 64, 64, 1, GDT_Byte,
                                   ts_bad_zl.List()));

    // CUSTOM tiling scheme with ZOOM_LEVEL
    VSIUnlink(path_rfail.c_str());
    CPLStringList ts_custom_zl;
    ts_custom_zl.AddString("TILING_SCHEME=CUSTOM");
    ts_custom_zl.AddString("ZOOM_LEVEL=2");
    EXPECT_EQ(nullptr, drv->Create(path_rfail.c_str(), 64, 64, 1, GDT_Byte,
                                   ts_custom_zl.List()));

    // GDT_Int16 with TILE_FORMAT=JPEG (warning)
    VSIUnlink(path_rfail.c_str());
    CPLStringList tf_i16_jpg;
    tf_i16_jpg.AddString("TILE_FORMAT=JPEG");
    auto ds_i16_jpg = std::unique_ptr<GDALDataset>(drv->Create(
        path_rfail.c_str(), 64, 64, 1, GDT_Int16, tf_i16_jpg.List()));
    if (ds_i16_jpg) ds_i16_jpg.reset();

    // GDT_Float32 with TILE_FORMAT=JPEG (warning) and TILE_FORMAT=PNG
    VSIUnlink(path_rfail.c_str());
    CPLStringList tf_f32_jpg;
    tf_f32_jpg.AddString("TILE_FORMAT=JPEG");
    auto ds_f32_jpg = std::unique_ptr<GDALDataset>(drv->Create(
        path_rfail.c_str(), 64, 64, 1, GDT_Float32, tf_f32_jpg.List()));
    if (ds_f32_jpg) ds_f32_jpg.reset();

    // Tile format names enum coverage -> L2605-2622
    EXPECT_STREQ("png", GDALMBTilesGetTileFormatName(GPKG_TF_PNG));
    EXPECT_STREQ("jpg", GDALMBTilesGetTileFormatName(GPKG_TF_JPEG));
    EXPECT_STREQ("webp", GDALMBTilesGetTileFormatName(GPKG_TF_WEBP));
    {
      WithQuietHandler quiet;
      EXPECT_EQ(nullptr, GDALMBTilesGetTileFormatName(GPKG_TF_PNG_16BIT));
    }

    // Gridded coverage clean overviews test
    VSIUnlink(path_rfail.c_str());
    CPLStringList tf_f32_png;
    tf_f32_png.AddString("TILE_FORMAT=PNG");
    auto ds_f32_png = std::unique_ptr<GDALDataset>(drv->Create(
        path_rfail.c_str(), 512, 512, 1, GDT_Float32, tf_f32_png.List()));
    if (ds_f32_png) {
      double gt_f32[6] = {0.0, 1.0, 0.0, 512.0, 0.0, -1.0};
      EXPECT_EQ(CE_None, ds_f32_png->SetGeoTransform(gt_f32));
      std::vector<float> fpx(512 * 512, 12.34f);
      EXPECT_EQ(CE_None, ds_f32_png->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 512, 512, fpx.data(), 512, 512,
                             GDT_Float32, 0, 0));
      int ov2 = 2;
      EXPECT_EQ(CE_None, ds_f32_png->BuildOverviews("NEAREST", 1, &ov2, 0,
                                                    nullptr, nullptr, nullptr));
      EXPECT_EQ(CE_None, ds_f32_png->BuildOverviews("NONE", 0, nullptr, 0,
                                                    nullptr, nullptr, nullptr));
      ds_f32_png.reset();
    }

    // Subdataset syntax open failures
    EXPECT_EQ(nullptr, GDALDataset::Open("GPKG"));
    EXPECT_EQ(nullptr, GDALDataset::Open("GPKG:/vsimem/no_colon.gpkg"));
    EXPECT_EQ(nullptr, GDALDataset::Open("GPKG:C:/fake/path.gpkg:table"));
    EXPECT_EQ(nullptr,
              GDALDataset::Open(
                  "GPKG:/vsicurl/http://example.com/a:b/test.gpkg:tbl"));

    // .gpkg.zip dataset creation and open
    const std::string path_zip = "/vsimem/gpkg_booster.gpkg.zip";
    absl::Cleanup cl_zip = [&path_zip] { VSIUnlink(path_zip.c_str()); };
    auto ds_zip = std::unique_ptr<GDALDataset>(
        drv->Create(path_zip.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    if (ds_zip) {
      ds_zip->CreateLayer("zlyr", nullptr, wkbPoint, nullptr);
      ds_zip.reset();
      auto ds_zip_ro = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(path_zip.c_str(), GDAL_OF_VECTOR));
      if (ds_zip_ro) ds_zip_ro.reset();
    }

    // Raster tiling scheme error branches
    const std::string path_rts = "/vsimem/gpkg_rts.gpkg";
    absl::Cleanup cl_rts = [&path_rts] { VSIUnlink(path_rts.c_str()); };
    CPLStringList rts_opts;
    rts_opts.AddString("TILING_SCHEME=GoogleCRS84Quad");
    auto ds_rts = std::unique_ptr<GDALDataset>(
        drv->Create(path_rts.c_str(), 256, 256, 1, GDT_Byte, rts_opts.List()));
    if (ds_rts) {
      OGRSpatialReference srs3857;
      srs3857.importFromEPSG(3857);
      EXPECT_EQ(CE_Failure, ds_rts->SetSpatialRef(&srs3857));
      double rot_gt[6] = {0, 1, 0.5, 0, 0.5, -1};
      EXPECT_EQ(CE_Failure, ds_rts->SetGeoTransform(rot_gt));
      double bad_px_gt[6] = {0, 0.12345, 0, 0, 0, -0.12345};
      EXPECT_EQ(CE_Failure, ds_rts->SetGeoTransform(bad_px_gt));
      ds_rts.reset();
    }

    // Raster unknown extensions and BAND_COUNT option
    const std::string path_rext = "/vsimem/gpkg_rext.gpkg";
    absl::Cleanup cl_rext = [&path_rext] { VSIUnlink(path_rext.c_str()); };
    auto ds_rext = std::unique_ptr<GDALDataset>(
        drv->Create(path_rext.c_str(), 256, 256, 1, GDT_Byte, nullptr));
    if (ds_rext) {
      ds_rext->ExecuteSQL(
          "INSERT INTO gpkg_extensions (table_name, column_name, "
          "extension_name, definition, scope) "
          "VALUES ('gpkg_rext', 'tile_data', 'gpkg_webp', "
          "'http://example.com', 'read-write')",
          nullptr, nullptr);
      ds_rext->ExecuteSQL(
          "INSERT INTO gpkg_extensions (table_name, column_name, "
          "extension_name, definition, scope) "
          "VALUES ('gpkg_rext', 'tile_data', 'gpkg_zoom_other', "
          "'http://example.com', 'read-write')",
          nullptr, nullptr);
      ds_rext->ExecuteSQL(
          "INSERT INTO gpkg_extensions (table_name, column_name, "
          "extension_name, definition, scope) "
          "VALUES ('gpkg_rext', 'tile_data', 'unk_wo_ext', "
          "'http://example.com', 'write-only')",
          nullptr, nullptr);
      ds_rext->ExecuteSQL(
          "INSERT INTO gpkg_extensions (table_name, column_name, "
          "extension_name, definition, scope) "
          "VALUES ('gpkg_rext', 'tile_data', 'unk_rw_ext', "
          "'http://example.com', 'read-write')",
          nullptr, nullptr);
      ds_rext.reset();

      CPLStringList b1_opts;
      b1_opts.AddString("BAND_COUNT=1");
      auto ro_ext = std::unique_ptr<GDALDataset>(GDALDataset::Open(
          path_rext.c_str(), GDAL_OF_RASTER, nullptr, b1_opts.List(), nullptr));
      if (ro_ext) ro_ext.reset();

      auto rw_ext = std::unique_ptr<GDALDataset>(GDALDataset::Open(
          path_rext.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE));
      if (rw_ext) rw_ext.reset();
    }

    // Metadata flush and deletion
    const std::string path_md = "/vsimem/gpkg_md_flush.gpkg";
    absl::Cleanup cl_md = [&path_md] { VSIUnlink(path_md.c_str()); };
    CPLStringList md_opts;
    md_opts.AddString("METADATA_TABLES=YES");
    auto ds_md = std::unique_ptr<GDALDataset>(
        drv->Create(path_md.c_str(), 256, 256, 1, GDT_Byte, md_opts.List()));
    if (ds_md) {
      ds_md->SetMetadataItem("MDKEY", "MDVAL");
      ds_md->FlushCache();
      ds_md->SetMetadata(nullptr);
      ds_md->FlushCache();
      ds_md.reset();
    }

    // CreateLayer on read-only dataset & z=2, m=2
    const std::string path_ro_test = "/vsimem/gpkg_ro_test.gpkg";
    absl::Cleanup cl_ro = [&path_ro_test] { VSIUnlink(path_ro_test.c_str()); };
    auto ds_ro_test = std::unique_ptr<GDALDataset>(
        drv->Create(path_ro_test.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds_ro_test);
    auto* dummy = ds_ro_test->CreateLayer("dummy", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, dummy);
    ds_ro_test->ExecuteSQL(
        "UPDATE gpkg_geometry_columns SET z = 2, m = 2, geometry_type_name = "
        "'GEOMETRY'",
        nullptr, nullptr);
    ds_ro_test.reset();

    auto ds_ro_opened = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(path_ro_test.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, ds_ro_opened);
    EXPECT_EQ(nullptr, ds_ro_opened->CreateLayer("fail_lyr"));
    EXPECT_EQ(CE_Failure, ds_ro_opened->SetProjection("LOCAL_CS[\"test\"]"));
    double dummy_gt[6] = {0, 1, 0, 0, 0, -1};
    EXPECT_EQ(CE_Failure, ds_ro_opened->SetGeoTransform(dummy_gt));
  }

  // -------------------------------------------------------------------------
  // Part B: SQL Functions & Geometry Blob Edge Cases
  // -------------------------------------------------------------------------
  {
    CPLSetThreadLocalConfigOption("GPKG_DEBUG", "TRUE");
    const std::string path_sql = "/vsimem/gpkg_booster_sql.gpkg";
    absl::Cleanup cl_sql = [&path_sql] {
      VSIUnlink(path_sql.c_str());
      CPLSetThreadLocalConfigOption("GPKG_DEBUG", nullptr);
    };

    OGRSpatialReference srs4326;
    srs4326.importFromEPSG(4326);
    srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    auto ds = std::unique_ptr<GDALDataset>(
        drv->Create(path_sql.c_str(), 64, 64, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {-180.0, 5.625, 0.0, 90.0, 0.0, -2.8125};
    ds->SetSpatialRef(&srs4326);
    ds->SetGeoTransform(gt);
    std::vector<GByte> px(64 * 64, 25);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 64, 64, px.data(),
                                             64, 64, GDT_Byte, 0, 0));
    ds->FlushCache();

    // Vector layer with various geometries
    auto* lyr = ds->CreateLayer("geom_tbl", &srs4326, wkbUnknown, nullptr);
    ASSERT_NE(nullptr, lyr);

    // Aspatial layer
    auto* aspat_lyr = ds->CreateLayer("aspat_tbl", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, aspat_lyr);

    // 1. Point
    OGRFeature f1(lyr->GetLayerDefn());
    OGRPoint pt(1.0, 2.0);
    f1.SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&f1));

    // 2. Empty Polygon
    OGRFeature f2(lyr->GetLayerDefn());
    OGRPolygon poly_empty;
    f2.SetGeometry(&poly_empty);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&f2));

    // 3. MultiPolygon
    OGRFeature f3(lyr->GetLayerDefn());
    OGRMultiPolygon mpoly;
    OGRPolygon p1;
    OGRLinearRing r1;
    r1.addPoint(0, 0);
    r1.addPoint(0, 1);
    r1.addPoint(1, 1);
    r1.addPoint(1, 0);
    r1.addPoint(0, 0);
    p1.addRing(&r1);
    mpoly.addGeometry(&p1);
    f3.SetGeometry(&mpoly);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&f3));

    // 4. CurvePolygon
    OGRFeature f4(lyr->GetLayerDefn());
    OGRGeometry* geom_curve = nullptr;
    const char* wkt_curve =
        "CURVEPOLYGON(COMPOUNDCURVE((0 0, 0 2, 2 2, 2 0, 0 0)))";
    OGRGeometryFactory::createFromWkt(&wkt_curve, nullptr, &geom_curve);
    if (geom_curve) {
      f4.SetGeometryDirectly(geom_curve);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&f4));
    }

    // 5. MultiSurface
    OGRFeature f5(lyr->GetLayerDefn());
    OGRGeometry* geom_ms = nullptr;
    const char* wkt_ms =
        "MULTISURFACE(CURVEPOLYGON(COMPOUNDCURVE((0 0, 0 2, 2 2, 2 0, 0 0))))";
    OGRGeometryFactory::createFromWkt(&wkt_ms, nullptr, &geom_ms);
    if (geom_ms) {
      f5.SetGeometryDirectly(geom_ms);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&f5));
    }

    // 6. Disjoint Polygon (+100, +100)
    OGRFeature f6(lyr->GetLayerDefn());
    OGRGeometry* geom_p2 = nullptr;
    const char* wkt_p2 =
        "POLYGON((100 100, 100 101, 101 101, 101 100, 100 100))";
    OGRGeometryFactory::createFromWkt(&wkt_p2, nullptr, &geom_p2);
    if (geom_p2) {
      f6.SetGeometryDirectly(geom_p2);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&f6));
    }

    // 7. Disjoint Polygon (-100, -100)
    OGRFeature f7(lyr->GetLayerDefn());
    OGRGeometry* geom_p3 = nullptr;
    const char* wkt_p3 =
        "POLYGON((-100 -100, -100 -99, -99 -99, -99 -100, -100 -100))";
    OGRGeometryFactory::createFromWkt(&wkt_p3, nullptr, &geom_p3);
    if (geom_p3) {
      f7.SetGeometryDirectly(geom_p3);
      EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&f7));
    }

    // Execute comprehensive SQL queries exercising every branch
    const char* const sql_queries[] = {
        // ST_MinX, ST_MinY, ST_MaxX, ST_MaxY, ST_IsEmpty, ST_GeometryType,
        // ST_SRID
        "SELECT ST_MinX(geom), ST_MinY(geom), ST_MaxX(geom), ST_MaxY(geom), "
        "       ST_IsEmpty(geom), ST_GeometryType(geom), ST_SRID(geom) FROM "
        "geom_tbl",
        "SELECT ST_MinX(NULL), ST_MinY(NULL), ST_MaxX(NULL), ST_MaxY(NULL), "
        "       ST_IsEmpty(NULL), ST_GeometryType(NULL), ST_SRID(NULL)",
        "SELECT ST_MinX(X'0001'), ST_MinY(X'0001'), ST_MaxX(X'0001'), "
        "ST_MaxY(X'0001'), "
        "       ST_IsEmpty(X'0001'), ST_GeometryType(X'0001'), "
        "ST_SRID(X'0001')",
        // SpatiaLite blob on ST_GeometryType
        "SELECT "
        "ST_GeometryType(X'"
        "0001E6100000000000000000F03F0000000000000040000000000000F03F0000000000"
        "0000407C01000000000000000000F03F0000000000000040FE')",

        // GPKG_IsAssignable
        "SELECT GPKG_IsAssignable('GEOMETRY', 'POINT'), "
        "GPKG_IsAssignable('POINT', 'POLYGON'), "
        "       GPKG_IsAssignable(NULL, 'POINT'), GPKG_IsAssignable('POINT', "
        "NULL)",

        // ST_EnvelopesIntersects (5-args) with all non-overlapping branches
        "SELECT ST_EnvelopesIntersects(geom, 10, 10, 20, 20), "
        "       ST_EnvelopesIntersects(geom, -20, 10, -10, 20), "
        "       ST_EnvelopesIntersects(geom, -20, -20, -10, -10), "
        "       ST_EnvelopesIntersects(geom, -20, -20, 20, -10), "
        "       ST_EnvelopesIntersects(geom, -5, -5, 5, 5), "
        "       ST_EnvelopesIntersects(NULL, 0, 0, 1, 1) FROM geom_tbl",

        // ST_EnvelopesIntersects (2-args)
        "SELECT ST_EnvelopesIntersects(a.geom, b.geom), "
        "ST_EnvelopesIntersects(NULL, b.geom), "
        "       ST_EnvelopesIntersects(a.geom, NULL) FROM geom_tbl a, geom_tbl "
        "b WHERE a.ROWID = 1 AND b.ROWID = 3",
        "SELECT ST_EnvelopesIntersects(a.geom, b.geom) FROM geom_tbl a, "
        "geom_tbl b",

        // SetSRID
        "SELECT SetSRID(geom, 3857), SetSRID(NULL, 3857), SetSRID(X'0001', "
        "3857) FROM geom_tbl",

        // ST_MakeValid
        "SELECT ST_MakeValid(geom), ST_MakeValid(NULL), ST_MakeValid(X'0001') "
        "FROM geom_tbl",
        "SELECT "
        "ST_MakeValid(X'"
        "0001E6100000000000000000F03F0000000000000040000000000000F03F0000000000"
        "0000407C01000000000000000000F03F0000000000000040FE')",

        // ST_Area (1-arg and 2-arg)
        "SELECT ST_Area(geom), ST_Area(geom, 1), ST_Area(geom, 0), "
        "ST_Area(NULL), ST_Area(NULL, 1), "
        "       ST_Area(X'0001'), ST_Area(X'0001', 1) FROM geom_tbl",
        "SELECT "
        "ST_Area(X'"
        "0001E6100000000000000000F03F0000000000000040000000000000F03F0000000000"
        "0000407C01000000000000000000F03F0000000000000040FE')",
        "SELECT "
        "ST_Area(X'"
        "0001E6100000000000000000F03F0000000000000040000000000000F03F0000000000"
        "0000407C01000000000000000000F03F0000000000000040FE', 1)",
        "SELECT ST_Area(SetSRID(geom, -99999), 1), ST_Length(SetSRID(geom, "
        "-99999), 1), ST_Transform(SetSRID(geom, -99999), 4326) FROM geom_tbl",

        // ST_Length (1-arg and 2-arg)
        "SELECT ST_Length(geom), ST_Length(geom, 1), ST_Length(geom, 0), "
        "ST_Length(NULL), ST_Length(NULL, 1), "
        "       ST_Length(X'0001'), ST_Length(X'0001', 1) FROM geom_tbl",
        "SELECT "
        "ST_Length(X'"
        "0001E6100000000000000000F03F0000000000000040000000000000F03F0000000000"
        "0000407C01000000000000000000F03F0000000000000040FE')",
        "SELECT "
        "ST_Length(X'"
        "0001E6100000000000000000F03F0000000000000040000000000000F03F0000000000"
        "0000407C01000000000000000000F03F0000000000000040FE', 1)",

        // ST_Transform: same SRID, cached CT reuse, invalid dest SRID,
        // SpatiaLite blob
        "SELECT ST_Transform(geom, 4326), ST_Transform(geom, 3857), "
        "ST_Transform(geom, 3857), "
        "       ST_Transform(geom, 999999), ST_Transform(NULL, 3857), "
        "ST_Transform(X'0001', 3857) FROM geom_tbl",
        "SELECT "
        "ST_Transform(X'"
        "0001E6100000000000000000F03F0000000000000040000000000000F03F0000000000"
        "0000407C01000000000000000000F03F0000000000000040FE', 3857)",

        // SridFromAuthCRS
        "SELECT SridFromAuthCRS('EPSG', 4326), SridFromAuthCRS('EPSG', "
        "999999), "
        "       SridFromAuthCRS(NULL, 4326), SridFromAuthCRS('EPSG', NULL)",

        // ImportFromEPSG
        "SELECT ImportFromEPSG(4326), ImportFromEPSG(-999999), "
        "ImportFromEPSG(NULL)",

        // RegisterGeometryExtension
        "SELECT RegisterGeometryExtension('geom_tbl', 'geom', 'POLYGON'), "
        "       RegisterGeometryExtension('unknown_tbl', 'geom', 'POLYGON'), "
        "       RegisterGeometryExtension('geom_tbl', 'unknown_col', "
        "'POLYGON'), "
        "       RegisterGeometryExtension('geom_tbl', 'geom', 'UNKNOWN_TYPE'), "
        "       RegisterGeometryExtension(NULL, 'geom', 'POLYGON')",

        // Spatial Index SQL functions
        "SELECT CreateSpatialIndex('geom_tbl', 'geom'), "
        "HasSpatialIndex('geom_tbl', 'geom'), "
        "       DisableSpatialIndex('geom_tbl', 'geom')",
        "SELECT CreateSpatialIndex('unknown_tbl', 'geom'), "
        "HasSpatialIndex('unknown_tbl', 'geom'), "
        "       DisableSpatialIndex('unknown_tbl', 'geom')",
        "SELECT CreateSpatialIndex('geom_tbl', 'unknown_col'), "
        "HasSpatialIndex('geom_tbl', 'unknown_col'), "
        "       DisableSpatialIndex('geom_tbl', 'unknown_col')",
        "SELECT CreateSpatialIndex(NULL, 'geom'), HasSpatialIndex(NULL, "
        "'geom'), "
        "       DisableSpatialIndex(NULL, 'geom')",

        // hstore_get_value
        "SELECT hstore_get_value('a=>1,b=>2', 'a'), "
        "hstore_get_value('a=>1,b=>2', 'missing'), "
        "       hstore_get_value(NULL, 'a'), hstore_get_value('a=>1', NULL)",

        // ogr_layer_Extent
        "SELECT ogr_layer_Extent('geom_tbl'), ogr_layer_Extent('aspat_tbl'), "
        "       ogr_layer_Extent('unknown_tbl'), ogr_layer_Extent(123)",

        // gdal_get_layer_pixel_value
        "SELECT gdal_get_layer_pixel_value(123, 1, 0, 0, 'nearest'), "
        "       gdal_get_layer_pixel_value('non_existent_raster', 1, 0, 0, "
        "'nearest')",

        // GDAL_GetMimeType, GDAL_GetBandCount, GDAL_HasColorTable
        "SELECT GDAL_GetMimeType(NULL), GDAL_GetBandCount(NULL), "
        "GDAL_HasColorTable(NULL)",
        "SELECT GDAL_GetMimeType(123), GDAL_GetBandCount(123), "
        "GDAL_HasColorTable(123)",
        "SELECT GDAL_GetMimeType(X'0102030405060708'), "
        "GDAL_GetBandCount(X'0102030405060708'), "
        "       GDAL_HasColorTable(X'0102030405060708')",
        // Valid GPkg header with corrupt WKB payload -> hits
        // OGRSQLiteImportSpatiaLiteGeometry error branch
        "SELECT ST_MakeValid(X'47500001E6100000FFFFFFFF'), "
        "ST_Area(X'47500001E6100000FFFFFFFF', 1), "
        "       ST_Length(X'47500001E6100000FFFFFFFF', 1), "
        "ST_Transform(X'47500001E6100000FFFFFFFF', 3857)",
        // GDAL_GetMimeType, GDAL_GetBandCount, GDAL_HasColorTable on actual
        // flushed PNG tile
        "SELECT GDAL_GetMimeType(tile_data), GDAL_GetBandCount(tile_data), "
        "GDAL_HasColorTable(tile_data) FROM gpkg_booster_sql",

        // PRAGMA and ORDER BY ExecuteSQL special cases
        "PRAGMA user_version",
        "PRAGMA journal_mode",
        "SELECT * FROM geom_tbl ORDER BY fid",
    };

    for (const char* q : sql_queries) {
      WithQuietHandler quiet;
      auto* res = ds->ExecuteSQL(q, nullptr, nullptr);
      if (res) {
        while (auto* f = res->GetNextFeature()) {
          OGRFeature::DestroyFeature(f);
        }
        ds->ReleaseResultSet(res);
      }
    }
    {
      WithQuietHandler quiet;
      auto* l_dbg = ds->ExecuteSQL("SELECT nolock", nullptr, "DEBUG");
      if (l_dbg) ds->ReleaseResultSet(l_dbg);
    }

    // Insert raster table extensions to cover CheckUnknownExtensions(true)
    // L7859-7926
    ds->ExecuteSQL(
        "INSERT INTO gpkg_extensions (table_name, column_name, extension_name, "
        "definition, scope) VALUES "
        "('gpkg_booster_sql', 'tile_data', 'gpkg_webp', 'http://def', "
        "'read-write'), "
        "('gpkg_booster_sql', NULL, 'gpkg_zoom_other', 'http://def', "
        "'read-write'), "
        "('gpkg_booster_sql', NULL, 'custom_wo', 'http://def', 'write-only'), "
        "('gpkg_booster_sql', NULL, 'custom_rw', 'http://def', 'read-write')",
        nullptr, nullptr);
    ds.reset();

    {
      WithQuietHandler quiet;
      auto ro_rast = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(path_sql.c_str(), GDAL_OF_RASTER));
      ASSERT_NE(nullptr, ro_rast);
      EXPECT_EQ(CE_Failure, ro_rast->SetGeoTransform(gt));
      int ov2 = 2;
      EXPECT_EQ(CE_Failure, ro_rast->BuildOverviews("NEAREST", 1, &ov2, 0,
                                                    nullptr, nullptr, nullptr));
      EXPECT_EQ(CE_Failure,
                ro_rast->GetRasterBand(1)->WriteBlock(0, 0, px.data()));
    }
    {
      WithQuietHandler quiet;
      auto rw_rast = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(path_sql.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE));
      ASSERT_NE(nullptr, rw_rast);
      EXPECT_EQ(CE_Failure, rw_rast->SetGeoTransform(gt));
      int ov2 = 2;
      EXPECT_EQ(CE_Failure, rw_rast->BuildOverviews("NEAREST", 1, &ov2, 0,
                                                    nullptr, nullptr, nullptr));
    }
  }

  // -------------------------------------------------------------------------
  // Part C: Field Domains & Data Columns Extended Edge Cases
  // -------------------------------------------------------------------------
  {
    const std::string path_dom = "/vsimem/gpkg_booster_dom.gpkg";
    absl::Cleanup cl_dom = [&path_dom] { VSIUnlink(path_dom.c_str()); };

    {
      auto ds_d = std::unique_ptr<GDALDataset>(
          drv->Create(path_dom.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(nullptr, ds_d);

      auto* t1 = ds_d->CreateLayer("t1", nullptr, wkbNone, nullptr);
      ASSERT_NE(nullptr, t1);
      OGRFieldDefn f_i("f_int", OFTInteger);
      EXPECT_EQ(OGRERR_NONE, t1->CreateField(&f_i));
      OGRFieldDefn f_i64("f_int64", OFTInteger64);
      EXPECT_EQ(OGRERR_NONE, t1->CreateField(&f_i64));
      OGRFieldDefn f_str("f_str", OFTString);
      EXPECT_EQ(OGRERR_NONE, t1->CreateField(&f_str));

      ds_d->ExecuteSQL(
          "CREATE TABLE gpkg_data_columns (table_name TEXT, column_name TEXT, "
          "name TEXT, title TEXT, description TEXT, mime_type TEXT, "
          "constraint_name TEXT);",
          nullptr, nullptr);

      ds_d->ExecuteSQL(
          "CREATE TABLE gpkg_data_column_constraints ("
          "constraint_name TEXT, constraint_type TEXT, value TEXT, "
          "min NUMERIC, min_is_inclusive BOOLEAN, max NUMERIC, "
          "max_is_inclusive BOOLEAN, "
          "description TEXT);",
          nullptr, nullptr);

      // 1. Column pointing to c_int_int64
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_columns (table_name, column_name, "
          "constraint_name) "
          "VALUES ('t1', 'f_int', 'c_int_int64'), ('t1', 'f_int64', "
          "'c_int_int64');",
          nullptr, nullptr);
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_column_constraints (constraint_name, "
          "constraint_type, value) "
          "VALUES ('c_int_int64', 'enum', '1'), ('c_int_int64', 'enum', '2');",
          nullptr, nullptr);

      // 2. Column pointing to c_int64_int
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_columns (table_name, column_name, "
          "constraint_name) "
          "VALUES ('t1', 'f_int64', 'c_int64_int'), ('t1', 'f_int', "
          "'c_int64_int');",
          nullptr, nullptr);
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_column_constraints (constraint_name, "
          "constraint_type, value) "
          "VALUES ('c_int64_int', 'enum', '1'), ('c_int64_int', 'enum', '2');",
          nullptr, nullptr);

      // 3. Column pointing to c_incompat
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_columns (table_name, column_name, "
          "constraint_name) "
          "VALUES ('t1', 'f_int', 'c_incompat'), ('t1', 'f_str', "
          "'c_incompat');",
          nullptr, nullptr);
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_column_constraints (constraint_name, "
          "constraint_type, value) "
          "VALUES ('c_incompat', 'enum', '1'), ('c_incompat', 'enum', '2');",
          nullptr, nullptr);

      // 4. Unreferenced domains with various inferred types from code
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_column_constraints (constraint_name, "
          "constraint_type, value) "
          "VALUES ('unref_int', 'enum', '10'), ('unref_int', 'enum', '20');",
          nullptr, nullptr);
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_column_constraints (constraint_name, "
          "constraint_type, value) "
          "VALUES ('unref_int64', 'enum', '10000000000');",
          nullptr, nullptr);
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_column_constraints (constraint_name, "
          "constraint_type, value) "
          "VALUES ('unref_real', 'enum', '3.1415');",
          nullptr, nullptr);
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_column_constraints (constraint_name, "
          "constraint_type, value) "
          "VALUES ('unref_str', 'enum', 'foo_text');",
          nullptr, nullptr);

      // 5. Faulty constraints to hit parse error paths
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_column_constraints (constraint_name, "
          "constraint_type, value) "
          "VALUES ('bad_multi_range', 'range', '1'), ('bad_multi_range', "
          "'range', '2');",
          nullptr, nullptr);
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_column_constraints (constraint_name, "
          "constraint_type, value) "
          "VALUES ('bad_enum_null', 'enum', NULL);",
          nullptr, nullptr);
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_column_constraints (constraint_name, "
          "constraint_type, value) "
          "VALUES ('bad_glob_null', 'glob', NULL);",
          nullptr, nullptr);
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_data_column_constraints (constraint_name, "
          "constraint_type, value) "
          "VALUES ('bad_type', 'unknown_type', 'val');",
          nullptr, nullptr);

      // QGIS layer_styles table for RenameLayer coverage (L7242-7251)
      ds_d->ExecuteSQL(
          "CREATE TABLE layer_styles (id INTEGER PRIMARY KEY, f_table_name "
          "TEXT);"
          "INSERT INTO layer_styles (f_table_name) VALUES ('t1');",
          nullptr, nullptr);

      // gpkgext_relations with NULL columns for
      // LoadRelationshipsUsingRelatedTablesExtension (L2042-2085)
      ds_d->ExecuteSQL(
          "CREATE TABLE gpkgext_relations ("
          "id INTEGER PRIMARY KEY, base_table_name TEXT, base_primary_column "
          "TEXT, "
          "related_table_name TEXT, related_primary_column TEXT, relation_name "
          "TEXT, mapping_table_name TEXT);"
          "INSERT INTO gpkgext_relations (base_table_name, "
          "base_primary_column, related_table_name, "
          "related_primary_column, relation_name, mapping_table_name) VALUES "
          "(NULL, 'c1', 't2', 'c2', 'rel', 'map'), "
          "('t1', NULL, 't2', 'c2', 'rel', 'map'), "
          "('t1', 'c1', NULL, 'c2', 'rel', 'map'), "
          "('t1', 'c1', 't2', NULL, 'rel', 'map'), "
          "('t1', 'c1', 't2', 'c2', NULL, 'map'), "
          "('t1', 'c1', 't2', 'c2', 'rel', NULL);",
          nullptr, nullptr);

      // Non-existent table in gpkg_contents (L1684-1690)
      ds_d->ExecuteSQL(
          "INSERT INTO gpkg_contents (table_name, data_type, identifier) "
          "VALUES ('ghost_table', 'features', 'dup_ident');",
          nullptr, nullptr);
    }

    // Reopen read-only: call GetFieldDomain on all of them
    {
      WithQuietHandler quiet;
      auto ds_ro = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(path_dom.c_str(), GDAL_OF_VECTOR));
      ASSERT_NE(nullptr, ds_ro);

      EXPECT_TRUE(ds_ro->GetRelationshipNames().empty());
      EXPECT_NE(nullptr, ds_ro->GetFieldDomain("c_int_int64"));
      EXPECT_NE(nullptr, ds_ro->GetFieldDomain("c_int64_int"));
      EXPECT_NE(nullptr, ds_ro->GetFieldDomain("c_incompat"));
      EXPECT_NE(nullptr, ds_ro->GetFieldDomain("unref_int"));
      EXPECT_NE(nullptr, ds_ro->GetFieldDomain("unref_int64"));
      EXPECT_NE(nullptr, ds_ro->GetFieldDomain("unref_real"));
      EXPECT_NE(nullptr, ds_ro->GetFieldDomain("unref_str"));
      EXPECT_EQ(nullptr, ds_ro->GetFieldDomain("bad_multi_range"));
      EXPECT_EQ(nullptr, ds_ro->GetFieldDomain("bad_enum_null"));
      EXPECT_EQ(nullptr, ds_ro->GetFieldDomain("bad_glob_null"));
      EXPECT_EQ(nullptr, ds_ro->GetFieldDomain("bad_type"));

      // AddFieldDomain on read-only dataset -> fails
      std::string failure_reason;
      OGRField fmin, fmax;
      fmin.Integer = 1;
      fmax.Integer = 10;
      auto rdom = std::make_unique<OGRRangeFieldDomain>(
          "new_dom", "", OFTInteger, OFSTNone, fmin, true, fmax, true);
      EXPECT_FALSE(ds_ro->AddFieldDomain(std::move(rdom), failure_reason));
    }

    // Reopen in update mode: test AddFieldDomain error branches and successful
    // branches
    {
      WithQuietHandler quiet;
      auto ds_rw = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(path_dom.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
      ASSERT_NE(nullptr, ds_rw);

      std::string failure_reason;
      // Duplicate name
      OGRField fmin, fmax;
      fmin.Integer = 1;
      fmax.Integer = 10;
      auto rdom_dup = std::make_unique<OGRRangeFieldDomain>(
          "unref_int", "", OFTInteger, OFSTNone, fmin, true, fmax, true);
      EXPECT_FALSE(ds_rw->AddFieldDomain(std::move(rdom_dup), failure_reason));

      // Range domain with non-numeric type (e.g. OFTString) -> fails
      auto rdom_str = std::make_unique<OGRRangeFieldDomain>(
          "range_str_bad", "", OFTString, OFSTNone, fmin, true, fmax, true);
      EXPECT_FALSE(ds_rw->AddFieldDomain(std::move(rdom_str), failure_reason));

      // Successful AddFieldDomain for OFTInteger, OFTInteger64, and Coded
      // without description
      auto rdom_int = std::make_unique<OGRRangeFieldDomain>(
          "valid_range_int", "", OFTInteger, OFSTNone, fmin, true, fmax, true);
      EXPECT_TRUE(ds_rw->AddFieldDomain(std::move(rdom_int), failure_reason));

      OGRField fmin64, fmax64;
      fmin64.Integer64 = 100;
      fmax64.Integer64 = 1000;
      auto rdom_i64 = std::make_unique<OGRRangeFieldDomain>(
          "valid_range_i64", "with desc", OFTInteger64, OFSTNone, fmin64, true,
          fmax64, true);
      EXPECT_TRUE(ds_rw->AddFieldDomain(std::move(rdom_i64), failure_reason));

      std::vector<OGRCodedValue> enum_vals;
      OGRCodedValue cv1;
      cv1.pszCode = CPLStrdup("CODE1");
      cv1.pszValue = CPLStrdup("Val1");
      enum_vals.push_back(cv1);
      enum_vals.push_back({nullptr, nullptr});
      auto cdom_nodesc = std::make_unique<OGRCodedFieldDomain>(
          "coded_nodesc", "", OFTString, OFSTNone, std::move(enum_vals));
      EXPECT_TRUE(
          ds_rw->AddFieldDomain(std::move(cdom_nodesc), failure_reason));

      // Duplicate IDENTIFIER in gpkg_contents -> L6595-6601
      CPLStringList id_opts;
      id_opts.AddString("IDENTIFIER=dup_ident");
      EXPECT_EQ(nullptr, ds_rw->CreateLayer("t_dup_id", nullptr, wkbPoint,
                                            id_opts.List()));

      // RenameLayer and DeleteLayer with gpkg_data_columns and layer_styles ->
      // L7234-7251, L6980-6985
      auto* lyr_t1 = ds_rw->GetLayerByName("t1");
      ASSERT_NE(nullptr, lyr_t1);
      EXPECT_EQ(OGRERR_NONE, lyr_t1->Rename("t1_renamed"));
      EXPECT_EQ(OGRERR_NONE, ds_rw->DeleteLayer(0));
    }
  }
}

// TODO: b/335317901 - Cover remaining OOM/sqlite3 allocation failure and
// /vsicurl/ branches in ogrgeopackagedatasource.cpp.

}  // namespace
}  // namespace autotest2
