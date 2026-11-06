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
// Tests for OGRSQLiteViewLayer (ogrsqliteviewlayer.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqliteviewlayer.cpp

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/sqlite/sqlite3.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/sqlite_view_test";

std::vector<uint8_t> MakeSpatiaLitePoint(int32_t srid, double x, double y) {
  std::vector<uint8_t> blob(60, 0);
  blob[0] = 0;
  blob[1] = 1;  // wkbNDR
  std::memcpy(blob.data() + 2, &srid, 4);
  double minX = x, minY = y, maxX = x, maxY = y;
  std::memcpy(blob.data() + 6, &minX, 8);
  std::memcpy(blob.data() + 14, &minY, 8);
  std::memcpy(blob.data() + 22, &maxX, 8);
  std::memcpy(blob.data() + 30, &maxY, 8);
  blob[38] = 0x7C;
  int32_t geomType = 1;  // Point
  std::memcpy(blob.data() + 39, &geomType, 4);
  std::memcpy(blob.data() + 43, &x, 8);
  std::memcpy(blob.data() + 51, &y, 8);
  blob[59] = 0xFE;
  return blob;
}

class OGRSQLiteViewLayerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRSQLite();
    VSIMkdir(kTestDir, 0777);
    db_path_ = std::string(kTestDir) + "/views.db";
    CreateTestDatabase();
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  void CreateTestDatabase() {
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
    ASSERT_NE(nullptr, poDriver);
    char** papszOpts = CSLAddString(nullptr, "METADATA=NO");
    std::unique_ptr<GDALDataset> poCreateDS(
        poDriver->Create(db_path_.c_str(), 0, 0, 0, GDT_Unknown, papszOpts));
    CSLDestroy(papszOpts);
    ASSERT_NE(nullptr, poCreateDS);

    sqlite3* hDB =
        static_cast<sqlite3*>(poCreateDS->GetInternalHandle("SQLITE_HANDLE"));
    ASSERT_NE(nullptr, hDB);

    const char* pszSchema =
        "CREATE TABLE spatial_ref_sys ("
        "  srid INTEGER PRIMARY KEY,"
        "  auth_name TEXT,"
        "  auth_srid INTEGER,"
        "  ref_sys_name TEXT,"
        "  proj4text TEXT,"
        "  srtext TEXT"
        ");\n"
        "INSERT INTO spatial_ref_sys VALUES ("
        "  4326, 'EPSG', 4326, 'WGS 84',"
        "  '+proj=longlat +datum=WGS84 +no_defs',"
        "  'GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
        "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\","
        "0.0174532925199433]]'"
        ");\n"
        "CREATE TABLE geometry_columns ("
        "  f_table_name TEXT,"
        "  f_geometry_column TEXT,"
        "  type TEXT,"
        "  coord_dimension TEXT,"
        "  srid INTEGER,"
        "  spatial_index_enabled INTEGER"
        ");\n"
        "CREATE TABLE views_geometry_columns ("
        "  view_name TEXT,"
        "  view_geometry TEXT,"
        "  view_rowid TEXT,"
        "  f_table_name TEXT,"
        "  f_geometry_column TEXT"
        ");\n"
        "CREATE TABLE base_idx (ogc_fid INTEGER PRIMARY KEY, name TEXT, geom "
        "BLOB);\n"
        "CREATE VIRTUAL TABLE idx_base_idx_geom USING rtree(pkid, xmin, xmax, "
        "ymin, ymax);\n"
        "INSERT INTO idx_base_idx_geom VALUES (1, 1, 1, 2, 2);\n"
        "INSERT INTO idx_base_idx_geom VALUES (2, 10, 10, 20, 20);\n"
        "CREATE TABLE base_fake_idx (ogc_fid INTEGER PRIMARY KEY, name TEXT, "
        "geom BLOB);\n"
        "CREATE TABLE base_noidx (ogc_fid INTEGER PRIMARY KEY, name TEXT, geom "
        "BLOB);\n"
        "INSERT INTO geometry_columns VALUES ('base_idx', 'geom', 'POINT', "
        "'XY', 4326, 1);\n"
        "INSERT INTO geometry_columns VALUES ('base_fake_idx', 'geom', "
        "'POINT', 'XY', 4326, 1);\n"
        "INSERT INTO geometry_columns VALUES ('base_noidx', 'geom', 'POINT', "
        "'XY', 4326, 0);\n"
        "CREATE VIEW v_idx AS SELECT ogc_fid AS rowid, name, geom FROM "
        "base_idx;\n"
        "CREATE VIEW v_fake_idx AS SELECT ogc_fid AS rowid, name, geom FROM "
        "base_fake_idx;\n"
        "CREATE VIEW v_noidx AS SELECT ogc_fid AS rowid, name, geom FROM "
        "base_noidx;\n"
        "CREATE VIEW v_view_under AS SELECT * FROM v_idx;\n"
        "CREATE VIEW v_bad_geom AS SELECT * FROM base_idx;\n"
        "CREATE VIEW v_missing_tbl AS SELECT 1 AS rowid, 'a' AS name, NULL AS "
        "geom;\n"
        "INSERT INTO views_geometry_columns VALUES ('v_idx', 'geom', 'rowid', "
        "'base_idx', 'geom');\n"
        "INSERT INTO views_geometry_columns VALUES ('v_fake_idx', 'geom', "
        "'rowid', 'base_fake_idx', 'geom');\n"
        "INSERT INTO views_geometry_columns VALUES ('v_noidx', 'geom', "
        "'rowid', 'base_noidx', 'geom');\n"
        "INSERT INTO views_geometry_columns VALUES ('v_view_under', 'geom', "
        "'rowid', 'v_idx', 'geom');\n"
        "INSERT INTO views_geometry_columns VALUES ('v_bad_geom', 'geom', "
        "'rowid', 'base_idx', 'bad_col');\n"
        "INSERT INTO views_geometry_columns VALUES ('v_missing_tbl', 'geom', "
        "'rowid', 'no_such_table', 'geom');\n"
        "INSERT INTO views_geometry_columns VALUES ('v_no_sql_view', 'geom', "
        "'rowid', 'base_idx', 'geom');\n";

    ASSERT_EQ(SQLITE_OK,
              sqlite3_exec(hDB, pszSchema, nullptr, nullptr, nullptr));

    // Insert geometries into base_idx
    sqlite3_stmt* hStmt = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_prepare_v2(hDB, "INSERT INTO base_idx VALUES (?, ?, ?);",
                                 -1, &hStmt, nullptr));

    std::vector<uint8_t> pt1 = MakeSpatiaLitePoint(4326, 1.0, 2.0);
    sqlite3_bind_int64(hStmt, 1, 1);
    sqlite3_bind_text(hStmt, 2, "pt1", -1, SQLITE_STATIC);
    sqlite3_bind_blob(hStmt, 3, pt1.data(), pt1.size(), SQLITE_STATIC);
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(hStmt));
    sqlite3_reset(hStmt);

    std::vector<uint8_t> pt2 = MakeSpatiaLitePoint(4326, 10.0, 20.0);
    sqlite3_bind_int64(hStmt, 1, 2);
    sqlite3_bind_text(hStmt, 2, "pt2", -1, SQLITE_STATIC);
    sqlite3_bind_blob(hStmt, 3, pt2.data(), pt2.size(), SQLITE_STATIC);
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(hStmt));
    sqlite3_finalize(hStmt);

    // Insert geometries into base_fake_idx and base_noidx
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                             hDB, "INSERT INTO base_fake_idx VALUES (?, ?, ?);",
                             -1, &hStmt, nullptr));
    sqlite3_bind_int64(hStmt, 1, 1);
    sqlite3_bind_text(hStmt, 2, "fake_pt", -1, SQLITE_STATIC);
    sqlite3_bind_blob(hStmt, 3, pt1.data(), pt1.size(), SQLITE_STATIC);
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(hStmt));
    sqlite3_finalize(hStmt);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                             hDB, "INSERT INTO base_noidx VALUES (?, ?, ?);",
                             -1, &hStmt, nullptr));
    sqlite3_bind_int64(hStmt, 1, 1);
    sqlite3_bind_text(hStmt, 2, "noidx_pt", -1, SQLITE_STATIC);
    sqlite3_bind_blob(hStmt, 3, pt1.data(), pt1.size(), SQLITE_STATIC);
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(hStmt));
    sqlite3_finalize(hStmt);
  }

  std::string db_path_;
};

TEST_F(OGRSQLiteViewLayerTest, SpatialViewBasicOperations) {
  CPLSetConfigOption("OGR_SQLITE_PRAGMA", "trusted_schema=ON");
  absl::Cleanup pragma_cleanup = [] {
    CPLSetConfigOption("OGR_SQLITE_PRAGMA", nullptr);
  };

  std::unique_ptr<GDALDataset> poDS(
      GDALDataset::Open(db_path_.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, poDS);

  OGRLayer* poLayer = poDS->GetLayerByName("v_idx");
  ASSERT_NE(nullptr, poLayer);

  // 1. GetGeomType before GetLayerDefn
  EXPECT_EQ(wkbPoint, poLayer->GetGeomType());

  // 2. LayerDefn
  OGRFeatureDefn* poDefn = poLayer->GetLayerDefn();
  ASSERT_NE(nullptr, poDefn);
  EXPECT_EQ(wkbPoint, poLayer->GetGeomType());
  EXPECT_EQ(1, poDefn->GetGeomFieldCount());
  EXPECT_STREQ("geom", poDefn->GetGeomFieldDefn(0)->GetNameRef());

  // 3. Capabilities
  EXPECT_TRUE(poLayer->TestCapability(OLCFastFeatureCount));
  EXPECT_TRUE(poLayer->TestCapability(OLCFastSpatialFilter));
  EXPECT_TRUE(poLayer->TestCapability(OLCStringsAsUTF8));
  EXPECT_FALSE(poLayer->TestCapability(OLCRandomWrite));

  // 4. Feature Count
  EXPECT_EQ(2, poLayer->GetFeatureCount(false));
  EXPECT_EQ(2, poLayer->GetFeatureCount(true));

  // 5. GetNextFeature iteration
  OGRFeature* poF = poLayer->GetNextFeature();
  ASSERT_NE(nullptr, poF);
  EXPECT_EQ(1, poF->GetFID());
  EXPECT_STREQ("pt1", poF->GetFieldAsString("name"));
  ASSERT_NE(nullptr, poF->GetGeometryRef());
  EXPECT_EQ(wkbPoint, poF->GetGeometryRef()->getGeometryType());
  delete poF;

  poF = poLayer->GetNextFeature();
  ASSERT_NE(nullptr, poF);
  EXPECT_EQ(2, poF->GetFID());
  EXPECT_STREQ("pt2", poF->GetFieldAsString("name"));
  delete poF;

  EXPECT_EQ(nullptr, poLayer->GetNextFeature());

  // 6. GetFeature by FID
  poF = poLayer->GetFeature(2);
  ASSERT_NE(nullptr, poF);
  EXPECT_EQ(2, poF->GetFID());
  EXPECT_STREQ("pt2", poF->GetFieldAsString("name"));
  delete poF;

  // Non-existent FID
  EXPECT_EQ(nullptr, poLayer->GetFeature(999));

  // 7. Attribute Filter
  EXPECT_EQ(OGRERR_NONE, poLayer->SetAttributeFilter("name = 'pt1'"));
  EXPECT_EQ(1, poLayer->GetFeatureCount(true));
  poF = poLayer->GetNextFeature();
  ASSERT_NE(nullptr, poF);
  EXPECT_EQ(1, poF->GetFID());
  delete poF;
  EXPECT_EQ(nullptr, poLayer->GetNextFeature());

  // 8. Spatial Filter
  EXPECT_EQ(OGRERR_NONE, poLayer->SetAttributeFilter(nullptr));
  poLayer->SetSpatialFilterRect(0.0, 0.0, 5.0, 5.0);
  EXPECT_EQ(1, poLayer->GetFeatureCount(true));
  poF = poLayer->GetNextFeature();
  ASSERT_NE(nullptr, poF);
  EXPECT_EQ(1, poF->GetFID());
  delete poF;

  // 9. Combined spatial and attribute filter
  EXPECT_EQ(OGRERR_NONE, poLayer->SetAttributeFilter("name = 'pt2'"));
  EXPECT_EQ(0, poLayer->GetFeatureCount(true));
  EXPECT_EQ(nullptr, poLayer->GetNextFeature());

  // Reset filters
  poLayer->SetSpatialFilter(nullptr);
  EXPECT_EQ(OGRERR_NONE, poLayer->SetAttributeFilter(nullptr));
  EXPECT_EQ(2, poLayer->GetFeatureCount(true));

  // 10. Malformed attribute filter triggers SQL prepare error
  EXPECT_EQ(OGRERR_NONE, poLayer->SetAttributeFilter("SYNTAX ERROR ==="));
  EXPECT_EQ(nullptr, poLayer->GetNextFeature());
  EXPECT_EQ(OGRERR_NONE, poLayer->SetAttributeFilter(nullptr));
}

TEST_F(OGRSQLiteViewLayerTest, SpatialIndexFallbackAndNoIndex) {
  CPLSetConfigOption("OGR_SQLITE_PRAGMA", "trusted_schema=ON");
  absl::Cleanup pragma_cleanup = [] {
    CPLSetConfigOption("OGR_SQLITE_PRAGMA", nullptr);
  };

  std::unique_ptr<GDALDataset> poDS(
      GDALDataset::Open(db_path_.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, poDS);

  // v_fake_idx claims spatial_index_enabled=1, but idx table does not exist
  OGRLayer* poFakeIdxLayer = poDS->GetLayerByName("v_fake_idx");
  ASSERT_NE(nullptr, poFakeIdxLayer);
  poFakeIdxLayer->SetSpatialFilterRect(0.0, 0.0, 5.0, 5.0);
  // Spatial index table missing check sets m_bHasSpatialIndex = false
  EXPECT_FALSE(poFakeIdxLayer->TestCapability(OLCFastSpatialFilter));
  EXPECT_EQ(1, poFakeIdxLayer->GetFeatureCount(true));
  OGRFeature* poF = poFakeIdxLayer->GetNextFeature();
  ASSERT_NE(nullptr, poF);
  delete poF;

  // v_noidx has spatial_index_enabled=0
  OGRLayer* poNoIdxLayer = poDS->GetLayerByName("v_noidx");
  ASSERT_NE(nullptr, poNoIdxLayer);
  EXPECT_FALSE(poNoIdxLayer->TestCapability(OLCFastSpatialFilter));
  poNoIdxLayer->SetSpatialFilterRect(0.0, 0.0, 5.0, 5.0);
  EXPECT_FALSE(poNoIdxLayer->TestCapability(OLCFastFeatureCount));
  EXPECT_EQ(1, poNoIdxLayer->GetFeatureCount(true));
  poF = poNoIdxLayer->GetNextFeature();
  ASSERT_NE(nullptr, poF);
  delete poF;
}

TEST_F(OGRSQLiteViewLayerTest, BrokenViewsLayerDefnErrors) {
  std::unique_ptr<GDALDataset> poDS(
      GDALDataset::Open(db_path_.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, poDS);

  // List of broken view names
  const char* apszBrokenViews[] = {
      "v_bad_geom",     // Underlying layer does not have expected geom column
      "v_missing_tbl",  // Underlying table does not exist
      "v_no_sql_view",  // View definition does not exist in SQLite
  };

  for (const char* pszViewName : apszBrokenViews) {
    OGRLayer* poLayer = poDS->GetLayerByName(pszViewName);
    ASSERT_NE(nullptr, poLayer) << "Failed to find layer " << pszViewName;

    // Trigger EstablishFeatureDefn
    const OGRFeatureDefn* poDefn = poLayer->GetLayerDefn();
    ASSERT_NE(nullptr, poDefn);

    // All operations should handle layer definition error gracefully
    std::unique_ptr<OGRFeature> poF1(poLayer->GetNextFeature());
    EXPECT_EQ(nullptr, poF1);

    std::unique_ptr<OGRFeature> poF2(poLayer->GetFeature(1));
    EXPECT_EQ(nullptr, poF2);

    EXPECT_EQ(0, poLayer->GetFeatureCount(true));
    EXPECT_FALSE(poLayer->TestCapability(OLCFastFeatureCount));
    EXPECT_FALSE(poLayer->TestCapability(OLCFastSpatialFilter));
  }
}

// TODO: b/335317901 - Cover OGRSQLiteViewLayer spatial filter evaluation when
// spatialite is loaded.

}  // namespace
}  // namespace autotest2
