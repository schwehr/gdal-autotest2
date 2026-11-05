// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <limits.h>

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/ogrshape.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::NotNull;

TEST(OGRShapeDataSourceTest, OpenAndOpenFile) {
  constexpr const char* kBase = "/vsimem/ods_open";
  constexpr const char* kShp = "/vsimem/ods_open.shp";
  constexpr const char* kShx = "/vsimem/ods_open.shx";
  constexpr const char* kDbf = "/vsimem/ods_open.dbf";
  auto cl = absl::MakeCleanup([kShp, kShx, kDbf]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
    VSIUnlink(kDbf);
  });

  // 1. Force single file data source stub
  {
    GDALOpenInfo oi(kShp, GA_Update);
    OGRShapeDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, false, true));
    EXPECT_EQ(ds.GetLayerCount(), 0);
  }

  // 2. Non-existent file
  {
    GDALOpenInfo oi("/vsimem/nonexistent.shp", GA_ReadOnly);
    OGRShapeDataSource ds;
    EXPECT_FALSE(ds.Open(&oi, true));
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&oi, false));
  }

  // 3. Invalid extension
  {
    VSIFCloseL(VSIFOpenL("/vsimem/dummy.txt", "wb"));
    auto clTxt = absl::MakeCleanup([]() { VSIUnlink("/vsimem/dummy.txt"); });
    GDALOpenInfo oi("/vsimem/dummy.txt", GA_ReadOnly);
    OGRShapeDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&oi, false));
  }

  // 4. Corrupt shapefile (size < 100 bytes)
  {
    VSILFILE* fp = VSIFOpenL(kShp, "wb");
    ASSERT_THAT(fp, NotNull());
    VSIFWriteL("corrupt", 1, 7, fp);
    VSIFCloseL(fp);
    GDALOpenInfo oi(kShp, GA_ReadOnly);
    OGRShapeDataSource ds;
    EXPECT_FALSE(ds.Open(&oi, true));
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&oi, false));
    VSIUnlink(kShp);
  }

  // 5. Update mode where DBF cannot be opened in update mode (DBF is a
  // directory)
  {
    SHPHandle h = SHPCreate(kBase, SHPT_POINT);
    ASSERT_THAT(h, NotNull());
    SHPClose(h);
    VSIMkdir(kDbf, 0755);
    auto clDbfDir = absl::MakeCleanup([kDbf]() { VSIRmdir(kDbf); });
    GDALOpenInfo oi(kShp, GA_Update);
    OGRShapeDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&oi, false));
  }

  // 6. Valid single shapefile with DBF
  {
    SHPHandle h = SHPCreate(kBase, SHPT_POINT);
    ASSERT_THAT(h, NotNull());
    SHPClose(h);
    DBFHandle hD = DBFCreate(kDbf);
    ASSERT_THAT(hD, NotNull());
    DBFAddField(hD, "ID", FTInteger, 10, 0);
    DBFClose(hD);

    const char* const papszOpenOpts[] = {"DBF_DATE_LAST_UPDATE=2026-11-03",
                                         "AUTO_REPACK=YES", "DBF_EOF_CHAR=NO",
                                         nullptr};
    GDALOpenInfo oi(kShp, GA_Update, papszOpenOpts);
    OGRShapeDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, false));
    EXPECT_EQ(ds.GetLayerCount(), 1);
    EXPECT_THAT(ds.GetLayer(0), NotNull());
    EXPECT_STREQ(ds.GetLayer(0)->GetName(), "ods_open");
  }

  // 7. Standalone DBF with SHAPE_RESTORE_SHX
  {
    CPLSetConfigOption("SHAPE_RESTORE_SHX", "YES");
    auto clOpt = absl::MakeCleanup(
        []() { CPLSetConfigOption("SHAPE_RESTORE_SHX", nullptr); });
    GDALOpenInfo oi(kDbf, GA_ReadOnly);
    OGRShapeDataSource ds;
    WithQuietHandler quiet;
    EXPECT_TRUE(ds.Open(&oi, false));
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }

  // 8. /vsicurl/ lazy shx loading in DS_SHPOpen
  {
    OGRShapeDataSource ds;
    WithQuietHandler quiet;
    SHPHandle h = ds.DS_SHPOpen("/vsicurl/http://127.0.0.1:1/test.shp", "r");
    EXPECT_EQ(h, nullptr);
  }
}

TEST(OGRShapeDataSourceTest, DirectoryOpenAndDeferredLayers) {
  constexpr const char* kDir = "/vsimem/ods_dir";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  // Layer 1: valid shp + dbf
  SHPHandle h1 = SHPCreate("/vsimem/ods_dir/layer1", SHPT_POINT);
  ASSERT_THAT(h1, NotNull());
  SHPClose(h1);
  DBFHandle d1 = DBFCreate("/vsimem/ods_dir/layer1.dbf");
  ASSERT_THAT(d1, NotNull());
  DBFAddField(d1, "ID", FTInteger, 5, 0);
  DBFClose(d1);

  // Layer 2: standalone dbf
  DBFHandle d2 = DBFCreate("/vsimem/ods_dir/layer2.dbf");
  ASSERT_THAT(d2, NotNull());
  DBFAddField(d2, "VAL", FTString, 10, 0);
  DBFClose(d2);

  // Layer 3: dbf with .tab file (should be ignored as MapInfo candidate)
  DBFHandle dTab = DBFCreate("/vsimem/ods_dir/tablayer.dbf");
  ASSERT_THAT(dTab, NotNull());
  DBFClose(dTab);
  VSIFCloseL(VSIFOpenL("/vsimem/ods_dir/tablayer.tab", "wb"));

  // Short candidate (< 4 chars) and ignored file
  VSIFCloseL(VSIFOpenL("/vsimem/ods_dir/a", "wb"));
  VSIFCloseL(VSIFOpenL("/vsimem/ods_dir/ignored.txt", "wb"));

  GDALOpenInfo oi(kDir, GA_Update);
  OGRShapeDataSource ds;
  EXPECT_TRUE(ds.Open(&oi, false));

  // Access layer1 by exact name
  OGRLayer* l1 = ds.GetLayerByName("layer1");
  ASSERT_THAT(l1, NotNull());
  EXPECT_STREQ(l1->GetName(), "layer1");

  // Access layer2 by case-insensitive name
  OGRLayer* l2 = ds.GetLayerByName("LAYER2");
  ASSERT_THAT(l2, NotNull());
  EXPECT_STREQ(l2->GetName(), "layer2");

  // Non-existent layer
  EXPECT_EQ(ds.GetLayerByName("nonexistent"), nullptr);

  // Now trigger GetLayerCount() to load remaining deferred layers
  EXPECT_EQ(ds.GetLayerCount(), 2);
  EXPECT_THAT(ds.GetLayer(0), NotNull());
  EXPECT_THAT(ds.GetLayer(1), NotNull());
  EXPECT_EQ(ds.GetLayer(-1), nullptr);
  EXPECT_EQ(ds.GetLayer(2), nullptr);

  // Verify tablayer was ignored
  EXPECT_EQ(ds.GetLayerByName("tablayer"), nullptr);
}

TEST(OGRShapeDataSourceTest, DirectoryOldCoverageCheck) {
  constexpr const char* kDir = "/vsimem/ods_cov";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  // Directory with "ARC" and a dbf, but no shapefiles
  VSIMkdir("/vsimem/ods_cov/ARC", 0755);
  DBFHandle d = DBFCreate("/vsimem/ods_cov/coverage.dbf");
  ASSERT_THAT(d, NotNull());
  DBFClose(d);

  GDALOpenInfo oi(kDir, GA_ReadOnly);
  OGRShapeDataSource ds;
  // Should detect old coverage and ignore dbf without shapefile
  EXPECT_FALSE(ds.Open(&oi, true));
}

TEST(OGRShapeDataSourceTest, CreateLayerGeometryTypesAndOverrides) {
  constexpr const char* kDir = "/vsimem/ods_create";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  GDALOpenInfo oi(kDir, GA_Update);
  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  ASSERT_TRUE(ds.Open(&oi, false));

  // 1. Read-only error check
  {
    OGRShapeDataSource dsRO;
    GDALOpenInfo oiRO(kDir, GA_ReadOnly);
    ASSERT_TRUE(dsRO.Open(&oiRO, false));
    WithQuietHandler quiet;
    EXPECT_EQ(dsRO.ICreateLayer("fail_ro", nullptr, nullptr), nullptr);
  }

  // 2. All geometry type mappings in ICreateLayer
  const struct {
    OGRwkbGeometryType eType;
    int expectedShapeType;
  } kGeomMappings[] = {
      {wkbUnknown, SHPT_ARC},
      {wkbLineString, SHPT_ARC},
      {wkbPoint, SHPT_POINT},
      {wkbPolygon, SHPT_POLYGON},
      {wkbTriangle, SHPT_POLYGON},
      {wkbMultiPoint, SHPT_MULTIPOINT},
      {wkbPoint25D, SHPT_POINTZ},
      {wkbPointM, SHPT_POINTM},
      {wkbPointZM, SHPT_POINTZ},
      {wkbLineString25D, SHPT_ARCZ},
      {wkbLineStringM, SHPT_ARCM},
      {wkbLineStringZM, SHPT_ARCZ},
      {wkbMultiLineString, SHPT_ARC},
      {wkbMultiLineString25D, SHPT_ARCZ},
      {wkbMultiLineStringM, SHPT_ARCM},
      {wkbMultiLineStringZM, SHPT_ARCZ},
      {wkbPolygon25D, SHPT_POLYGONZ},
      {wkbTriangleZ, SHPT_POLYGONZ},
      {wkbPolygonM, SHPT_POLYGONM},
      {wkbTriangleM, SHPT_POLYGONM},
      {wkbPolygonZM, SHPT_POLYGONZ},
      {wkbTriangleZM, SHPT_POLYGONZ},
      {wkbMultiPolygon, SHPT_POLYGON},
      {wkbMultiPolygon25D, SHPT_POLYGONZ},
      {wkbMultiPolygonM, SHPT_POLYGONM},
      {wkbMultiPolygonZM, SHPT_POLYGONZ},
      {wkbMultiPoint25D, SHPT_MULTIPOINTZ},
      {wkbMultiPointM, SHPT_MULTIPOINTM},
      {wkbMultiPointZM, SHPT_MULTIPOINTZ},
      {wkbTIN, SHPT_MULTIPATCH},
      {wkbPolyhedralSurface, SHPT_MULTIPATCH},
      {wkbNone, SHPT_NULL},
  };

  for (size_t i = 0; i < std::size(kGeomMappings); ++i) {
    std::string lyrName = "geom_" + std::to_string(i);
    OGRGeomFieldDefn gf("geom", kGeomMappings[i].eType);
    OGRLayer* lyr = ds.ICreateLayer(lyrName.c_str(), &gf, nullptr);
    ASSERT_THAT(lyr, NotNull());
    EXPECT_STREQ(lyr->GetName(), lyrName.c_str());
  }

  // Duplicate layer name error
  {
    OGRGeomFieldDefn gf("geom", wkbPoint);
    WithQuietHandler quiet;
    EXPECT_EQ(ds.ICreateLayer("geom_0", &gf, nullptr), nullptr);
  }

  // Unsupported geometry type
  {
    OGRGeomFieldDefn gf("geom", wkbCircularString);
    WithQuietHandler quiet;
    EXPECT_EQ(ds.ICreateLayer("circ_unsupported", &gf, nullptr), nullptr);
  }

  // 3. SHPT override options
  const char* const kShptOverrides[] = {
      "POINT",        "ARC",         "POLYGON",     "MULTIPOINT", "POINTZ",
      "ARCZ",         "POLYGONZ",    "MULTIPOINTZ", "POINTM",     "ARCM",
      "POLYGONM",     "MULTIPOINTM", "POINTZM",     "ARCZM",      "POLYGONZM",
      "MULTIPOINTZM", "MULTIPATCH",  "NONE",        "NULL",
  };

  for (size_t i = 0; i < std::size(kShptOverrides); ++i) {
    std::string lyrName = "shpt_" + std::string(kShptOverrides[i]);
    const char* const papszOpts[] = {CPLSPrintf("SHPT=%s", kShptOverrides[i]),
                                     nullptr};
    OGRLayer* lyr = ds.ICreateLayer(lyrName.c_str(), nullptr, papszOpts);
    ASSERT_THAT(lyr, NotNull());
  }

  // Invalid SHPT override
  {
    const char* const papszOpts[] = {"SHPT=INVALID_TYPE", nullptr};
    WithQuietHandler quiet;
    EXPECT_EQ(ds.ICreateLayer("shpt_invalid", nullptr, papszOpts), nullptr);
  }

  // 4. Creation with SRS, ENCODING, RESIZE, SPATIAL_INDEX, 2GB_LIMIT, and
  // laundering
  {
    OGRSpatialReference srs;
    srs.SetFromUserInput("EPSG:4326");
    OGRGeomFieldDefn gf("geom", wkbPoint);
    gf.SetSpatialRef(&srs);
    const char* const papszOpts[] = {"2GB_LIMIT=YES",
                                     "ENCODING=UTF-8",
                                     "RESIZE=YES",
                                     "SPATIAL_INDEX=YES",
                                     "DBF_DATE_LAST_UPDATE=2026-11-03",
                                     "AUTO_REPACK=NO",
                                     "DBF_EOF_CHAR=NO",
                                     nullptr};
    WithQuietHandler quiet;
    OGRLayer* lyr = ds.ICreateLayer("bad:layer/name", &gf, papszOpts);
    ASSERT_THAT(lyr, NotNull());
    EXPECT_STREQ(lyr->GetName(), "bad_layer_name");
  }
}

TEST(OGRShapeDataSourceTest, TestCapability) {
  constexpr const char* kDir = "/vsimem/ods_cap";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource dsUpdate;
  GDALOpenInfo oiUpdate(kDir, GA_Update);
  ASSERT_TRUE(dsUpdate.Open(&oiUpdate, false));

  EXPECT_TRUE(dsUpdate.TestCapability(ODsCCreateLayer));
  EXPECT_TRUE(dsUpdate.TestCapability(ODsCDeleteLayer));
  EXPECT_TRUE(dsUpdate.TestCapability(ODsCMeasuredGeometries));
  EXPECT_TRUE(dsUpdate.TestCapability(ODsCZGeometries));
  EXPECT_TRUE(dsUpdate.TestCapability(ODsCRandomLayerWrite));
  EXPECT_FALSE(dsUpdate.TestCapability("UnknownCapability"));

  OGRShapeDataSource dsRO;
  GDALOpenInfo oiRO(kDir, GA_ReadOnly);
  ASSERT_TRUE(dsRO.Open(&oiRO, false));

  EXPECT_FALSE(dsRO.TestCapability(ODsCCreateLayer));
  EXPECT_FALSE(dsRO.TestCapability(ODsCDeleteLayer));
  EXPECT_FALSE(dsRO.TestCapability(ODsCRandomLayerWrite));
}

TEST(OGRShapeDataSourceTest, DeleteLayer) {
  constexpr const char* kDir = "/vsimem/ods_del";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  OGRGeomFieldDefn gf("geom", wkbPoint);
  OGRLayer* lyr1 = ds.ICreateLayer("del_layer1", &gf, nullptr);
  ASSERT_THAT(lyr1, NotNull());
  OGRLayer* lyr2 = ds.ICreateLayer("del_layer2", &gf, nullptr);
  ASSERT_THAT(lyr2, NotNull());
  EXPECT_EQ(ds.GetLayerCount(), 2);

  // Read-only delete error
  {
    OGRShapeDataSource dsRO;
    dsRO.SetDescription(kDir);
    GDALOpenInfo oiRO(kDir, GA_ReadOnly);
    ASSERT_TRUE(dsRO.Open(&oiRO, false));
    WithQuietHandler quiet;
    EXPECT_EQ(dsRO.DeleteLayer(0), OGRERR_FAILURE);
  }

  // Out of range index
  {
    WithQuietHandler quiet;
    EXPECT_EQ(ds.DeleteLayer(-1), OGRERR_FAILURE);
    EXPECT_EQ(ds.DeleteLayer(10), OGRERR_FAILURE);
  }

  // Successful deletion of layer 0
  EXPECT_EQ(ds.DeleteLayer(0), OGRERR_NONE);
  EXPECT_EQ(ds.GetLayerCount(), 1);
  EXPECT_STREQ(ds.GetLayer(0)->GetName(), "del_layer2");
  VSIStatBufL sStat;
  EXPECT_EQ(VSIStatL("/vsimem/ods_del/del_layer1.shp", &sStat), -1);
}

TEST(OGRShapeDataSourceTest, ExecuteSQLCommands) {
  constexpr const char* kDir = "/vsimem/ods_sql";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  OGRGeomFieldDefn gf("geom", wkbPolygon);
  OGRLayer* lyr = ds.ICreateLayer("sql_lyr", &gf, nullptr);
  ASSERT_THAT(lyr, NotNull());
  OGRFieldDefn fld("NAME", OFTString);
  EXPECT_EQ(lyr->CreateField(&fld), OGRERR_NONE);

  // UNCOMPRESS and RECOMPRESS on non-zip (no-op)
  EXPECT_EQ(ds.ExecuteSQL("UNCOMPRESS", nullptr, nullptr), nullptr);
  EXPECT_EQ(ds.ExecuteSQL("RECOMPRESS", nullptr, nullptr), nullptr);

  // REPACK
  EXPECT_EQ(ds.ExecuteSQL("REPACK sql_lyr", nullptr, nullptr), nullptr);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(ds.ExecuteSQL("REPACK nonexistent", nullptr, nullptr), nullptr);
  }

  // RESIZE
  EXPECT_EQ(ds.ExecuteSQL("RESIZE sql_lyr", nullptr, nullptr), nullptr);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(ds.ExecuteSQL("RESIZE nonexistent", nullptr, nullptr), nullptr);
  }

  // RECOMPUTE EXTENT ON
  EXPECT_EQ(ds.ExecuteSQL("RECOMPUTE EXTENT ON sql_lyr", nullptr, nullptr),
            nullptr);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        ds.ExecuteSQL("RECOMPUTE EXTENT ON nonexistent", nullptr, nullptr),
        nullptr);
  }

  // CREATE SPATIAL INDEX ON & DROP SPATIAL INDEX ON
  EXPECT_EQ(ds.ExecuteSQL("CREATE SPATIAL INDEX ON sql_lyr", nullptr, nullptr),
            nullptr);
  EXPECT_EQ(ds.ExecuteSQL("CREATE SPATIAL INDEX ON sql_lyr DEPTH 4", nullptr,
                          nullptr),
            nullptr);
  EXPECT_EQ(ds.ExecuteSQL("DROP SPATIAL INDEX ON sql_lyr", nullptr, nullptr),
            nullptr);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        ds.ExecuteSQL("DROP SPATIAL INDEX ON nonexistent", nullptr, nullptr),
        nullptr);
    EXPECT_EQ(
        ds.ExecuteSQL("CREATE SPATIAL INDEX ON nonexistent", nullptr, nullptr),
        nullptr);
    EXPECT_EQ(ds.ExecuteSQL("CREATE SPATIAL INDEX ON", nullptr, nullptr),
              nullptr);
  }

  // CREATE INDEX ON / DROP INDEX ON attribute index
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        ds.ExecuteSQL("CREATE INDEX ON sql_lyr USING NAME", nullptr, nullptr),
        nullptr);
    EXPECT_EQ(ds.ExecuteSQL("DROP INDEX ON sql_lyr NAME", nullptr, nullptr),
              nullptr);
  }

  // Generic SQL query
  OGRLayer* lyrQuery = ds.ExecuteSQL("SELECT * FROM sql_lyr", nullptr, nullptr);
  ASSERT_THAT(lyrQuery, NotNull());
  ds.ReleaseResultSet(lyrQuery);
}

TEST(OGRShapeDataSourceTest, LayerPoolAndSuppressOnClose) {
  constexpr const char* kDir = "/vsimem/ods_pool";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  // Add layers up to pool max (default 100) to trigger AddLayer pool
  // registration
  const int nMax = ds.GetPool()->GetMaxSimultaneouslyOpened();
  for (int i = 0; i < nMax; ++i) {
    std::string name = "pool_" + std::to_string(i);
    DBFHandle d = DBFCreate(("/vsimem/ods_pool/" + name + ".dbf").c_str());
    ASSERT_THAT(d, NotNull());
    DBFAddField(d, "F", FTInteger, 5, 0);
    DBFClose(d);
    ds.OpenFile(("/vsimem/ods_pool/" + name + ".dbf").c_str(), true);
  }
  EXPECT_EQ(ds.GetLayerCount(), nMax);

  // SetLastUsedLayer when size >= max
  OGRShapeLayer* l0 =
      cpl::down_cast<OGRShapeLayer*>(const_cast<OGRLayer*>(ds.GetLayer(0)));
  ds.SetLastUsedLayer(l0);

  // GetFileList
  char** papszFiles = ds.GetFileList();
  ASSERT_THAT(papszFiles, NotNull());
  EXPECT_GT(CSLCount(papszFiles), 0);
  CSLDestroy(papszFiles);

  ds.Close();

  // Test MarkSuppressOnClose via driver
  RegisterOGRShape();
  constexpr const char* kSuppressDir = "/vsimem/ods_suppress";
  VSIMkdir(kSuppressDir, 0755);
  auto clSuppress =
      absl::MakeCleanup([kSuppressDir]() { VSIRmdirRecursive(kSuppressDir); });
  {
    GDALDriver* poDriver =
        GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    ASSERT_THAT(poDriver, NotNull());
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(kSuppressDir, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_THAT(poDS, NotNull());
    OGRGeomFieldDefn gf("geom", wkbPoint);
    ASSERT_THAT(poDS->CreateLayer("supp_layer", &gf, nullptr), NotNull());
    poDS->MarkSuppressOnClose();
  }
}

TEST(OGRShapeDataSourceTest, SingleLayerZip) {
  constexpr const char* kShz = "/vsimem/ods_single.shz";
  auto cl = absl::MakeCleanup([kShz]() { VSIUnlink(kShz); });

  CPLSetConfigOption("OGR_SHAPE_PACK_IN_PLACE", "YES");
  CPLSetConfigOption("OGR_SHAPE_USE_VSIMEM_FOR_TEMP", "YES");
  auto clOpts = absl::MakeCleanup([]() {
    CPLSetConfigOption("OGR_SHAPE_PACK_IN_PLACE", nullptr);
    CPLSetConfigOption("OGR_SHAPE_USE_VSIMEM_FOR_TEMP", nullptr);
  });

  // 1. Create .shz
  {
    OGRShapeDataSource ds;
    ds.SetDescription(kShz);
    ASSERT_TRUE(ds.CreateZip(kShz));
    EXPECT_TRUE(ds.IsZip());
    EXPECT_TRUE(ds.TestCapability(ODsCCreateLayer));

    // Create 1st layer without geometry so only .dbf is created, avoiding
    // upstream .shp sort comparator strict-weak-ordering issue under libc++.
    // Pass ENCODING=UTF-8 to create a .cpg file as well, ensuring >= 2 files
    // in the zip archive so /vsizip/ stats it as a directory.
    OGRGeomFieldDefn gf("", wkbNone);
    const char* const papszOpts[] = {"ENCODING=UTF-8", nullptr};
    OGRLayer* lyr1 = ds.ICreateLayer("single_lyr", &gf, papszOpts);
    ASSERT_THAT(lyr1, NotNull());
    OGRFieldDefn fld("ID", OFTInteger);
    EXPECT_EQ(lyr1->CreateField(&fld), OGRERR_NONE);

    // Creating 2nd layer should fail in .shz
    {
      WithQuietHandler quiet;
      EXPECT_EQ(ds.ICreateLayer("second_lyr", &gf, nullptr), nullptr);
      EXPECT_FALSE(ds.TestCapability(ODsCCreateLayer));
    }

    // DeleteLayer in .shz should fail
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(ds.TestCapability(ODsCDeleteLayer));
      EXPECT_EQ(ds.DeleteLayer(0), OGRERR_FAILURE);
    }

    // GetFileList on Zip returns the zip filename
    char** papszFiles = ds.GetFileList();
    ASSERT_THAT(papszFiles, NotNull());
    EXPECT_STREQ(papszFiles[0], kShz);
    CSLDestroy(papszFiles);

    EXPECT_EQ(ds.Close(), CE_None);
  }

  // 2. Open .shz
  {
    std::string vsiZipPath = std::string("/vsizip/{") + kShz + "}";
    VSIStatBufL sStat;
    EXPECT_EQ(VSIStatL(kShz, &sStat), 0);
    EXPECT_EQ(VSIStatL(vsiZipPath.c_str(), &sStat), 0);
    char** pList = VSIReadDir(vsiZipPath.c_str());
    ASSERT_THAT(pList, NotNull());
    EXPECT_GT(CSLCount(pList), 0);
    CSLDestroy(pList);

    GDALOpenInfo oi(vsiZipPath.c_str(), GA_ReadOnly);
    OGRShapeDataSource ds;
    ASSERT_TRUE(ds.OpenZip(&oi, kShz)) << "Error: " << CPLGetLastErrorMsg();
    EXPECT_EQ(ds.GetLayerCount(), 1);
    EXPECT_STREQ(ds.GetLayer(0)->GetName(), "single_lyr");
  }
}

TEST(OGRShapeDataSourceTest, MultiLayerZipWithLockFileAndRecompress) {
  constexpr const char* kZip = "/vsimem/ods_multi.zip";
  constexpr const char* kLock = "/vsimem/ods_multi.zip.gdal.lock";
  auto cl = absl::MakeCleanup([kZip, kLock]() {
    VSIUnlink(kZip);
    VSIUnlink(kLock);
  });

  // 1. Create multi-layer zip
  {
    OGRShapeDataSource ds;
    ds.SetDescription(kZip);
    ASSERT_TRUE(ds.CreateZip(kZip));
    EXPECT_TRUE(ds.IsZip());

    OGRGeomFieldDefn gf1("", wkbNone);
    OGRLayer* lyr1 = ds.ICreateLayer("lyr1", &gf1, nullptr);
    ASSERT_THAT(lyr1, NotNull());
    OGRFieldDefn fld1("ID", OFTInteger);
    EXPECT_EQ(lyr1->CreateField(&fld1), OGRERR_NONE);

    OGRGeomFieldDefn gf2("", wkbNone);
    OGRLayer* lyr2 = ds.ICreateLayer("lyr2", &gf2, nullptr);
    ASSERT_THAT(lyr2, NotNull());
    OGRFieldDefn fld2("VAL", OFTReal);
    EXPECT_EQ(lyr2->CreateField(&fld2), OGRERR_NONE);

    EXPECT_EQ(ds.Close(), CE_None);
  }

  // 2. Open multi-layer zip in update mode with RefreshLockFile thread
  {
    CPLSetConfigOption("OGR_SHAPE_LOCK_DELAY", "0.05");
    CPLSetConfigOption("OGR_SHAPE_PACK_IN_PLACE", "YES");
    CPLSetConfigOption("OGR_SHAPE_USE_VSIMEM_FOR_TEMP", "YES");
    auto clOpts = absl::MakeCleanup([]() {
      CPLSetConfigOption("OGR_SHAPE_LOCK_DELAY", nullptr);
      CPLSetConfigOption("OGR_SHAPE_PACK_IN_PLACE", nullptr);
      CPLSetConfigOption("OGR_SHAPE_USE_VSIMEM_FOR_TEMP", nullptr);
    });

    std::string vsiZipPath = std::string("/vsizip/{") + kZip + "}";
    GDALOpenInfo oi(vsiZipPath.c_str(), GA_ReadOnly);
    oi.eAccess = GA_Update;
    OGRShapeDataSource ds;
    ASSERT_TRUE(ds.OpenZip(&oi, kZip));
    EXPECT_EQ(ds.GetLayerCount(), 2);

    // Uncompress creates lock file and starts thread
    EXPECT_TRUE(ds.UncompressIfNeeded());

    // Sleep 120ms to allow COND_TIMED_WAIT_TIME_OUT to execute in
    // RefreshLockFile
    CPLSleep(0.12);

    // Another task trying to uncompress while lock is active should fail
    {
      OGRShapeDataSource ds2;
      GDALOpenInfo oi2(vsiZipPath.c_str(), GA_ReadOnly);
      oi2.eAccess = GA_Update;
      ASSERT_TRUE(ds2.OpenZip(&oi2, kZip));
      WithQuietHandler quiet;
      EXPECT_FALSE(ds2.UncompressIfNeeded());
      ds2.Close();
    }

    // ExecuteSQL UNCOMPRESS & RECOMPRESS on zip dataset
    EXPECT_EQ(ds.ExecuteSQL("UNCOMPRESS", nullptr, nullptr), nullptr);
    EXPECT_EQ(ds.ExecuteSQL("RECOMPRESS", nullptr, nullptr), nullptr);

    // Close re-compresses with CopyInPlace and removes lock file
    EXPECT_EQ(ds.Close(), CE_None);
    VSIStatBufL sStat;
    EXPECT_EQ(VSIStatL(kLock, &sStat), -1);
  }

  // 3. Test OGR_SHAPE_PACK_IN_PLACE=NO and OGR_SHAPE_USE_VSIMEM_FOR_TEMP=NO
  {
    CPLSetConfigOption("OGR_SHAPE_LOCK_DELAY", "0.05");
    CPLSetConfigOption("OGR_SHAPE_PACK_IN_PLACE", "NO");
    CPLSetConfigOption("OGR_SHAPE_USE_VSIMEM_FOR_TEMP", "NO");
    auto clOpts = absl::MakeCleanup([]() {
      CPLSetConfigOption("OGR_SHAPE_LOCK_DELAY", nullptr);
      CPLSetConfigOption("OGR_SHAPE_PACK_IN_PLACE", nullptr);
      CPLSetConfigOption("OGR_SHAPE_USE_VSIMEM_FOR_TEMP", nullptr);
    });

    std::string vsiZipPath = std::string("/vsizip/{") + kZip + "}";
    GDALOpenInfo oi(vsiZipPath.c_str(), GA_ReadOnly);
    oi.eAccess = GA_Update;
    OGRShapeDataSource ds;
    ASSERT_TRUE(ds.OpenZip(&oi, kZip));
    EXPECT_TRUE(ds.UncompressIfNeeded());
    EXPECT_EQ(ds.Close(), CE_None);
  }

  // 4. Test CopyInPlace helper
  {
    constexpr const char* kSrc = "/vsimem/ods_cip_src.bin";
    constexpr const char* kDst = "/vsimem/ods_cip_dst.bin";
    auto clFiles = absl::MakeCleanup([kSrc, kDst]() {
      VSIUnlink(kSrc);
      VSIUnlink(kDst);
    });

    const std::string content = "Hello CopyInPlace Test Content";
    VSILFILE* fpSrc = VSIFOpenL(kSrc, "wb");
    ASSERT_THAT(fpSrc, NotNull());
    VSIFWriteL(content.data(), 1, content.size(), fpSrc);
    VSIFCloseL(fpSrc);

    VSILFILE* fpDst = VSIFOpenL(kDst, "wb");
    ASSERT_THAT(fpDst, NotNull());
    EXPECT_TRUE(OGRShapeDataSource::CopyInPlace(fpDst, kSrc));
    VSIFCloseL(fpDst);

    VSILFILE* fpCheck = VSIFOpenL(kDst, "rb");
    ASSERT_THAT(fpCheck, NotNull());
    std::string readBack(content.size(), '\0');
    EXPECT_EQ(VSIFReadL(readBack.data(), 1, content.size(), fpCheck),
              content.size());
    EXPECT_EQ(readBack, content);
    VSIFCloseL(fpCheck);
  }
}

}  // namespace
}  // namespace autotest2
