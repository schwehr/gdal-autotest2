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
// Unit tests for OGRSQLiteSelectLayer (ogrsqliteselectlayer.cpp).
//
// See also:
//   https://gdal.org/drivers/vector/sqlite.html
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqliteselectlayer.cpp
//
// google3 build caveats:
//   - Compiled without HAVE_SPATIALITE.

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
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/sqlite/sqlite3.h"

extern "C" void GDALRegister_MEM();

namespace autotest2 {
namespace {

constexpr const char* kTestDir = "/vsimem/sqlite_select_test";

std::vector<uint8_t> MakeSpatiaLitePoint(int srid, double x, double y,
                                         bool bXDR = false) {
  std::vector<uint8_t> blob(60, 0);
  blob[0] = 0x00;
  blob[1] = bXDR ? 0x00 : 0x01;
  uint32_t u_srid = static_cast<uint32_t>(srid);
  if (bXDR) {
    u_srid = CPL_MSBWORD32(u_srid);
  }
  memcpy(&blob[2], &u_srid, 4);

  double mbr[4] = {x, y, x, y};
  if (bXDR) {
    for (int i = 0; i < 4; ++i) {
      CPL_MSBPTR64(&mbr[i]);
    }
  }
  memcpy(&blob[6], mbr, 32);
  blob[38] = 0x7C;
  uint32_t geom_type = 1;
  if (bXDR) {
    geom_type = CPL_MSBWORD32(geom_type);
  }
  memcpy(&blob[39], &geom_type, 4);
  double coords[2] = {x, y};
  if (bXDR) {
    CPL_MSBPTR64(&coords[0]);
    CPL_MSBPTR64(&coords[1]);
  }
  memcpy(&blob[43], coords, 16);
  blob[59] = 0xFE;
  return blob;
}

class OGRSQLiteSelectLayerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRSQLite();
    GDALRegister_MEM();
    VSIRmdirRecursive(kTestDir);
    VSIMkdir(kTestDir, 0777);

    db_path_ = std::string(kTestDir) + "/test.db";
    CreateTestDatabase();
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  void CreateTestDatabase() {
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
    ASSERT_NE(nullptr, poDriver);

    char** papszOpts = CSLAddString(nullptr, "METADATA=NO");
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(db_path_.c_str(), 0, 0, 0, GDT_Unknown, papszOpts));
    CSLDestroy(papszOpts);
    ASSERT_NE(nullptr, poDS);

    sqlite3* hDB =
        static_cast<sqlite3*>(poDS->GetInternalHandle("SQLITE_HANDLE"));
    ASSERT_NE(nullptr, hDB);

    // Create table with spatial column and raw BLOB column
    ASSERT_EQ(SQLITE_OK,
              sqlite3_exec(hDB,
                           "CREATE TABLE pts (id INTEGER PRIMARY KEY, "
                           "name TEXT, OGR_STYLE TEXT, GEOMETRY GEOMETRY);",
                           nullptr, nullptr, nullptr));

    ASSERT_EQ(SQLITE_OK,
              sqlite3_exec(hDB,
                           "CREATE TABLE non_spatial (id INTEGER PRIMARY KEY, "
                           "name TEXT);",
                           nullptr, nullptr, nullptr));

    // Register geometry in geometry_columns and spatial_ref_sys
    ASSERT_EQ(
        SQLITE_OK,
        sqlite3_exec(hDB,
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
                     "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
                     "UNIT[\"degree\",0.0174532925199433]]'"
                     ");\n"
                     "CREATE TABLE geometry_columns ("
                     "  f_table_name TEXT,"
                     "  f_geometry_column TEXT,"
                     "  geometry_type INTEGER,"
                     "  coord_dimension INTEGER,"
                     "  srid INTEGER,"
                     "  spatial_index_enabled INTEGER"
                     ");\n"
                     "INSERT INTO geometry_columns VALUES ("
                     "  'pts', 'GEOMETRY', 0, 2, 4326, 0"
                     ");",
                     nullptr, nullptr, nullptr));

    // Insert features into pts
    sqlite3_stmt* hStmt = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_prepare_v2(hDB, "INSERT INTO pts VALUES (?, ?, ?, ?);",
                                 -1, &hStmt, nullptr));

    // Row 1: NDR SpatiaLite point at (1.0, 2.0)
    std::vector<uint8_t> pt1 = MakeSpatiaLitePoint(4326, 1.0, 2.0, false);
    sqlite3_bind_int64(hStmt, 1, 1);
    sqlite3_bind_text(hStmt, 2, "pt1", -1, SQLITE_STATIC);
    sqlite3_bind_text(hStmt, 3, "PEN(c:#000000)", -1, SQLITE_STATIC);
    sqlite3_bind_blob(hStmt, 4, pt1.data(), static_cast<int>(pt1.size()),
                      SQLITE_STATIC);
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(hStmt));
    sqlite3_reset(hStmt);

    // Row 2: XDR SpatiaLite point at (10.0, 20.0)
    std::vector<uint8_t> pt2 = MakeSpatiaLitePoint(4326, 10.0, 20.0, true);
    sqlite3_bind_int64(hStmt, 1, 2);
    sqlite3_bind_text(hStmt, 2, "pt2", -1, SQLITE_STATIC);
    sqlite3_bind_null(hStmt, 3);
    sqlite3_bind_blob(hStmt, 4, pt2.data(), static_cast<int>(pt2.size()),
                      SQLITE_STATIC);
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(hStmt));
    sqlite3_reset(hStmt);

    // Row 3: Point with unknown SRID (999999)
    std::vector<uint8_t> pt3 = MakeSpatiaLitePoint(999999, 100.0, 200.0, false);
    sqlite3_bind_int64(hStmt, 1, 3);
    sqlite3_bind_text(hStmt, 2, "pt3", -1, SQLITE_STATIC);
    sqlite3_bind_null(hStmt, 3);
    sqlite3_bind_blob(hStmt, 4, pt3.data(), static_cast<int>(pt3.size()),
                      SQLITE_STATIC);
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(hStmt));
    sqlite3_finalize(hStmt);

    // Insert row into non_spatial
    ASSERT_EQ(SQLITE_OK,
              sqlite3_exec(hDB, "INSERT INTO non_spatial VALUES (1, 'ns1');",
                           nullptr, nullptr, nullptr));
  }

  std::string db_path_;
};

TEST_F(OGRSQLiteSelectLayerTest, ConstructorBlobSRSAndBasicOps) {
  std::unique_ptr<GDALDataset> poDS(
      GDALDataset::Open(db_path_.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, poDS);

  // 1. SELECT GEOMETRY triggers unknown geometry type SRS extraction
  OGRLayer* poSelectLyr = poDS->ExecuteSQL(
      "SELECT id, name, GEOMETRY FROM pts ORDER BY id", nullptr, nullptr);
  ASSERT_NE(nullptr, poSelectLyr);
  EXPECT_EQ(wkbUnknown, poSelectLyr->GetGeomType());
  // Spatial reference may or may not be resolved depending on PROJ environment
  if (poSelectLyr->GetSpatialRef() != nullptr) {
    EXPECT_TRUE(poSelectLyr->GetSpatialRef()->IsGeographic());
  }

  // 2. Iterate features and ResetReading
  OGRFeature* poF = poSelectLyr->GetNextFeature();
  ASSERT_NE(nullptr, poF);
  EXPECT_EQ(0, poF->GetFID());
  EXPECT_STREQ("pt1", poF->GetFieldAsString("name"));
  delete poF;

  // ResetReading after reading
  poSelectLyr->ResetReading();
  poF = poSelectLyr->GetNextFeature();
  ASSERT_NE(nullptr, poF);
  EXPECT_EQ(0, poF->GetFID());
  delete poF;

  poDS->ReleaseResultSet(poSelectLyr);

  // 3. Select with XDR blob as first geometry row
  OGRLayer* poXDRLyr = poDS->ExecuteSQL(
      "SELECT id, name, GEOMETRY FROM pts WHERE id = 2", nullptr, nullptr);
  ASSERT_NE(nullptr, poXDRLyr);
  EXPECT_EQ(wkbUnknown, poXDRLyr->GetGeomType());
  poDS->ReleaseResultSet(poXDRLyr);

  // 4. Select with unknown SRID blob as first geometry row
  OGRLayer* poUnknownSRIDLyr = poDS->ExecuteSQL(
      "SELECT id, name, GEOMETRY FROM pts WHERE id = 3", nullptr, nullptr);
  ASSERT_NE(nullptr, poUnknownSRIDLyr);
  poDS->ReleaseResultSet(poUnknownSRIDLyr);

  // 5. Empty result layer
  OGRLayer* poEmptyLyr =
      poDS->ExecuteSQL("SELECT * FROM pts WHERE 1 = 0", nullptr, nullptr);
  ASSERT_NE(nullptr, poEmptyLyr);
  EXPECT_EQ(0, poEmptyLyr->GetFeatureCount(true));
  EXPECT_EQ(nullptr, poEmptyLyr->GetNextFeature());
  poDS->ReleaseResultSet(poEmptyLyr);

  // 6. PRAGMA statement (bUseStatementForGetNextFeature = false)
  OGRLayer* poPragmaLyr =
      poDS->ExecuteSQL("PRAGMA table_info(pts)", nullptr, nullptr);
  ASSERT_NE(nullptr, poPragmaLyr);
  EXPECT_GE(poPragmaLyr->GetFeatureCount(true), 1);
  poF = poPragmaLyr->GetNextFeature();
  ASSERT_NE(nullptr, poF);
  delete poF;
  poDS->ReleaseResultSet(poPragmaLyr);
}

TEST_F(OGRSQLiteSelectLayerTest, AttributeFilterVariations) {
  std::unique_ptr<GDALDataset> poDS(
      GDALDataset::Open(db_path_.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, poDS);

  OGRLayer* poLyr = poDS->ExecuteSQL(
      "SELECT id, name, OGR_STYLE, GEOMETRY FROM pts", nullptr, nullptr);
  ASSERT_NE(nullptr, poLyr);

  // Null when already null
  EXPECT_EQ(OGRERR_NONE, poLyr->SetAttributeFilter(nullptr));

  // Normal attribute filter
  EXPECT_EQ(OGRERR_NONE, poLyr->SetAttributeFilter("id = 1"));
  EXPECT_EQ(1, poLyr->GetFeatureCount(true));
  // Reset reading even if index at zero
  poLyr->ResetReading();
  OGRFeature* poF = poLyr->GetNextFeature();
  ASSERT_NE(nullptr, poF);
  EXPECT_EQ(0, poF->GetFID());
  delete poF;

  // Change attribute filter
  EXPECT_EQ(OGRERR_NONE, poLyr->SetAttributeFilter("id >= 2"));
  EXPECT_EQ(2, poLyr->GetFeatureCount(true));

  // Special field filter (HasSpecialFields -> SNT_COLUMN and SNT_OPERATION)
  EXPECT_EQ(OGRERR_NONE, poLyr->SetAttributeFilter("OGR_STYLE IS NOT NULL"));
  EXPECT_EQ(1, poLyr->GetFeatureCount(true));

  // Clear attribute filter
  EXPECT_EQ(OGRERR_NONE, poLyr->SetAttributeFilter(nullptr));
  EXPECT_EQ(3, poLyr->GetFeatureCount(true));

  poDS->ReleaseResultSet(poLyr);

  // Complex UNION query where BuildSQL fails
  OGRLayer* poUnionLyr = poDS->ExecuteSQL(
      "SELECT id, name FROM pts UNION SELECT id, name FROM pts", nullptr,
      nullptr);
  ASSERT_NE(nullptr, poUnionLyr);
  EXPECT_EQ(OGRERR_NONE, poUnionLyr->SetAttributeFilter("id = 1"));
  EXPECT_EQ(1, poUnionLyr->GetFeatureCount(true));
  poDS->ReleaseResultSet(poUnionLyr);
}

TEST_F(OGRSQLiteSelectLayerTest, BuildSQLBranches) {
  std::unique_ptr<GDALDataset> poDS(
      GDALDataset::Open(db_path_.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, poDS);

  // Test various query syntax patterns to hit BuildSQL() branches:
  const char* const apszQueries[] = {
      // 1. WHERE without trailing clauses
      "SELECT * FROM pts WHERE id > 0",
      // 2. WHERE with GROUP BY
      "SELECT id, count(*) FROM pts WHERE id > 0 GROUP BY id",
      // 3. WHERE with ORDER BY
      "SELECT * FROM pts WHERE id > 0 ORDER BY id",
      // 4. WHERE with LIMIT
      "SELECT * FROM pts WHERE id > 0 LIMIT 10",
      // 5. Without WHERE, with GROUP BY
      "SELECT id, count(*) FROM pts GROUP BY id",
      // 6. Without WHERE, with ORDER BY
      "SELECT * FROM pts ORDER BY id",
      // 7. Without WHERE, with LIMIT
      "SELECT * FROM pts LIMIT 10",
      // 8. Quoted table name double quotes
      "SELECT * FROM \"pts\"",
      // 9. Quoted table name single quotes
      "SELECT * FROM 'pts'",
      // 10. Bare table name ending at string end
      "SELECT * FROM pts",
      // 11. Complex trailing token (AS p) -> triggers unhandled syntax
      "SELECT * FROM pts AS p",
  };

  for (const char* pszQuery : apszQueries) {
    OGRLayer* poLyr = poDS->ExecuteSQL(pszQuery, nullptr, nullptr);
    ASSERT_NE(nullptr, poLyr) << pszQuery;
    absl::Cleanup release_lyr = [&] { poDS->ReleaseResultSet(poLyr); };
    // Set attribute filter
    EXPECT_EQ(OGRERR_NONE, poLyr->SetAttributeFilter("id = 1"));
    // Set spatial filter
    poLyr->SetSpatialFilterRect(0.0, 0.0, 5.0, 5.0);
    // Trigger BuildSQL via GetFeatureCount or GetNextFeature
    EXPECT_GE(poLyr->GetFeatureCount(true), 0);
    OGRFeatureUniquePtr poF(poLyr->GetNextFeature());
  }

  // Missing FROM clause
  OGRLayer* poNoFrom = poDS->ExecuteSQL("SELECT 1", nullptr, nullptr);
  ASSERT_NE(nullptr, poNoFrom);
  poNoFrom->SetSpatialFilterRect(0, 0, 1, 1);
  poNoFrom->SetAttributeFilter("1 = 1");
  poDS->ReleaseResultSet(poNoFrom);
}

TEST_F(OGRSQLiteSelectLayerTest, FeatureCountAndCapabilities) {
  std::unique_ptr<GDALDataset> poDS(
      GDALDataset::Open(db_path_.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, poDS);

  // 1. SELECT COUNT(*) FROM pts -> returns 1
  OGRLayer* poCountLyr =
      poDS->ExecuteSQL("SELECT COUNT(*) FROM pts", nullptr, nullptr);
  ASSERT_NE(nullptr, poCountLyr);
  EXPECT_EQ(1, poCountLyr->GetFeatureCount(false));
  poDS->ReleaseResultSet(poCountLyr);

  // 2. Capabilities on simple select layer
  OGRLayer* poSimpleLyr =
      poDS->ExecuteSQL("SELECT * FROM pts", nullptr, nullptr);
  ASSERT_NE(nullptr, poSimpleLyr);
  EXPECT_FALSE(poSimpleLyr->TestCapability(OLCFastFeatureCount));
  EXPECT_FALSE(poSimpleLyr->TestCapability(OLCRandomWrite));
  EXPECT_TRUE(poSimpleLyr->TestCapability(OLCStringsAsUTF8));
  poDS->ReleaseResultSet(poSimpleLyr);

  // 3. Fast spatial filter capability on simple vs complex query
  OGRLayer* poSpatialLyr =
      poDS->ExecuteSQL("SELECT * FROM pts", nullptr, nullptr);
  ASSERT_NE(nullptr, poSpatialLyr);
  poSpatialLyr->TestCapability(OLCFastSpatialFilter);
  poDS->ReleaseResultSet(poSpatialLyr);

  OGRLayer* poUnionLyr = poDS->ExecuteSQL(
      "SELECT * FROM pts UNION SELECT * FROM pts", nullptr, nullptr);
  ASSERT_NE(nullptr, poUnionLyr);
  EXPECT_FALSE(poUnionLyr->TestCapability(OLCFastSpatialFilter));
  poDS->ReleaseResultSet(poUnionLyr);
}

TEST_F(OGRSQLiteSelectLayerTest, GetExtentAndCaching) {
  // Open read-only to exercise extent caching
  std::unique_ptr<GDALDataset> poDS(
      GDALDataset::Open(db_path_.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, poDS);

  // 1. Query with ORDER BY (triggers ORDER BY stripping optimization)
  const char* pszSQL = "SELECT * FROM pts ORDER BY id";
  OGRLayer* poLyr1 = poDS->ExecuteSQL(pszSQL, nullptr, nullptr);
  ASSERT_NE(nullptr, poLyr1);

  OGREnvelope sExtent1;
  EXPECT_EQ(OGRERR_NONE, poLyr1->GetExtent(&sExtent1, true));
  EXPECT_DOUBLE_EQ(1.0, sExtent1.MinX);
  EXPECT_DOUBLE_EQ(100.0, sExtent1.MaxX);
  poDS->ReleaseResultSet(poLyr1);

  // 2. Second query with same SQL hits cached extent in
  // poDS->GetEnvelopeFromSQL
  OGRLayer* poLyr2 = poDS->ExecuteSQL(pszSQL, nullptr, nullptr);
  ASSERT_NE(nullptr, poLyr2);
  OGREnvelope sExtent2;
  EXPECT_EQ(OGRERR_NONE, poLyr2->GetExtent(&sExtent2, true));
  EXPECT_DOUBLE_EQ(sExtent1.MinX, sExtent2.MinX);
  EXPECT_DOUBLE_EQ(sExtent1.MaxX, sExtent2.MaxX);
  poDS->ReleaseResultSet(poLyr2);

  // 3. Error branches in GetExtent
  OGRLayer* poLyr3 = poDS->ExecuteSQL("SELECT * FROM pts", nullptr, nullptr);
  ASSERT_NE(nullptr, poLyr3);
  OGREnvelope sExtentErr;
  EXPECT_EQ(OGRERR_FAILURE, poLyr3->GetExtent(-1, &sExtentErr, true));
  EXPECT_EQ(OGRERR_FAILURE, poLyr3->GetExtent(99, &sExtentErr, true));
  poDS->ReleaseResultSet(poLyr3);

  // Non-spatial layer GetExtent returns failure
  OGRLayer* poNonSpatialLyr =
      poDS->ExecuteSQL("SELECT * FROM non_spatial", nullptr, nullptr);
  ASSERT_NE(nullptr, poNonSpatialLyr);
  EXPECT_EQ(OGRERR_FAILURE, poNonSpatialLyr->GetExtent(&sExtentErr, true));
  poDS->ReleaseResultSet(poNonSpatialLyr);
}

TEST_F(OGRSQLiteSelectLayerTest, MemoryDataSourceSpatialFilter) {
  // Test !bCanReopenBaseDS path via MEM dataset with SQLite dialect
  GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDriver);

  std::unique_ptr<GDALDataset> poMemDS(
      poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poMemDS);

  OGRLayer* poSrc = poMemDS->CreateLayer("mem_pts", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, poSrc);

  OGRFeature* poF = new OGRFeature(poSrc->GetLayerDefn());
  OGRPoint pt(2.0, 3.0);
  poF->SetGeometry(&pt);
  ASSERT_EQ(OGRERR_NONE, poSrc->CreateFeature(poF));
  delete poF;

  OGRLayer* poSelect =
      poMemDS->ExecuteSQL("SELECT * FROM mem_pts", nullptr, "SQLITE");
  ASSERT_NE(nullptr, poSelect);

  // ISetSpatialFilter with !bCanReopenBaseDS
  OGRPoint filterPt(2.0, 3.0);
  EXPECT_EQ(OGRERR_NONE, poSelect->SetSpatialFilter(0, &filterPt));
  EXPECT_EQ(1, poSelect->GetFeatureCount(true));

  // Invalid geom field index
  EXPECT_EQ(OGRERR_FAILURE, poSelect->SetSpatialFilter(99, &filterPt));

  poMemDS->ReleaseResultSet(poSelect);
}

// TODO: b/335317901 - Cover OGRSQLiteSelectLayer spatial index evaluation when
// spatialite is loaded.

}  // namespace
}  // namespace autotest2
