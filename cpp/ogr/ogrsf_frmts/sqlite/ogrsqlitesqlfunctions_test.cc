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
// Unit tests for SQLite extension SQL functions (ogrsqlitesqlfunctions.cpp).
//
// See also:
//   https://gdal.org/drivers/vector/sqlite.html
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqlitesqlfunctions.cpp

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/sqlite/sqlite3.h"

extern "C" void RegisterOGRSQLite();

namespace autotest2 {
namespace {

constexpr const char* kTestDir = "/vsimem/sqlite_sqlfunc_test";

class OGRSQLiteSQLFunctionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRSQLite();
    VSIRmdirRecursive(kTestDir);
    VSIMkdir(kTestDir, 0777);

    db_path_ = std::string(kTestDir) + "/test.db";
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
    ASSERT_NE(nullptr, poDriver);

    char** papszOpts = CSLAddString(nullptr, "METADATA=NO");
    poDS_.reset(
        poDriver->Create(db_path_.c_str(), 0, 0, 0, GDT_Unknown, papszOpts));
    CSLDestroy(papszOpts);
    ASSERT_NE(nullptr, poDS_);

    hDB_ = static_cast<sqlite3*>(poDS_->GetInternalHandle("SQLITE_HANDLE"));
    ASSERT_NE(nullptr, hDB_);
  }

  void TearDown() override {
    poDS_.reset();
    VSIRmdirRecursive(kTestDir);
  }

  std::string db_path_;
  std::unique_ptr<GDALDataset> poDS_;
  sqlite3* hDB_ = nullptr;
};

TEST_F(OGRSQLiteSQLFunctionsTest, OGRVersion) {
  sqlite3_stmt* hStmt = nullptr;

  // 0 args -> RELEASE_NAME
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(hDB_, "SELECT ogr_version();", -1,
                                          &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_TEXT, sqlite3_column_type(hStmt, 0));
  const char* pszVer =
      reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 0));
  EXPECT_NE(nullptr, pszVer);
  sqlite3_finalize(hStmt);

  // Non-text arg -> falls back to RELEASE_NAME
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(hDB_, "SELECT ogr_version(123);", -1,
                                          &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_TEXT, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // Text arg -> custom version info query
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ogr_version('RELEASE_NAME');", -1,
                               &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_TEXT, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ogr_version('VERSION_NUM');", -1,
                               &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_TEXT, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsTest, Transform3) {
  sqlite3_stmt* hStmt = nullptr;

  // 1. Arg 0 not blob -> NULL
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                           hDB_, "SELECT Transform3('not_blob', 4326, 3857);",
                           -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 2. Arg 1 not int -> NULL
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                           hDB_, "SELECT Transform3(X'00', 'not_int', 3857);",
                           -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 3. Arg 2 not int -> NULL
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                           hDB_, "SELECT Transform3(X'00', 4326, 'not_int');",
                           -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 4. Invalid SRS -> poModule->GetTransform returns nullptr -> NULL
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT Transform3(ST_MakePoint(2.0, 49.0), -999, 3857);",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 5. Corrupt blob -> ImportSpatiaLiteGeometry fails -> NULL
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                           hDB_, "SELECT Transform3(X'01020304', 4326, 3857);",
                           -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 6. Valid transform 4326 to 3857 -> non-null blob
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT Transform3(ST_MakePoint(2.0, 49.0), 4326, 3857);",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsTest, DeflateInflate) {
  sqlite3_stmt* hStmt = nullptr;

  // 1. Invalid arg 0 type -> NULL
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(hDB_, "SELECT ogr_deflate(123);", -1,
                                          &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 2. Invalid arg 1 type -> NULL
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ogr_deflate('hello', 'not_int');",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 3. Valid text deflate -> BLOB
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ogr_deflate('hello world');", -1,
                               &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 4. Valid blob deflate with level -> BLOB
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ogr_deflate(X'0102030405', 5);",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 5. Invalid deflate level -> NULL
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ogr_deflate('hello', 999);", -1,
                               &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 6. Inflate invalid arg 0 type -> NULL
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ogr_inflate('not_blob');", -1,
                               &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 7. Inflate roundtrip
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_, "SELECT CAST(ogr_inflate(ogr_deflate('hello world')) AS TEXT);",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_TEXT, sqlite3_column_type(hStmt, 0));
  EXPECT_STREQ("hello world",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 0)));
  sqlite3_finalize(hStmt);

  // 8. Corrupt zlib blob -> NULL
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ogr_inflate(X'deadbeef');", -1,
                               &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsTest, GeocodeStubInGoogle3) {
  sqlite3_stmt* hStmt = nullptr;

  // In google3 OGRGeocode is disabled via #if 1 return;
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(hDB_, "SELECT ogr_geocode('Paris');",
                                          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  sqlite3_finalize(hStmt);

  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT ogr_geocode_reverse(2.0, 49.0, 'geometry');", -1,
                &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsTest, DatasourceLoadLayers) {
  sqlite3_stmt* hStmt = nullptr;

  // 1. Arg 0 not text -> 0
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ogr_datasource_load_layers(123);",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 2. Arg 1 not int -> 0
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                           hDB_,
                           "SELECT ogr_datasource_load_layers('/vsimem/no.db', "
                           "'not_int');",
                           -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 3. Arg 2 not text -> 0
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                           hDB_,
                           "SELECT ogr_datasource_load_layers('/vsimem/no.db', "
                           "0, 123);",
                           -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 4. Nonexistent datasource -> 0
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_,
          "SELECT ogr_datasource_load_layers('/vsimem/sqlite_sqlfunc_test/"
          "nonexistent.db');",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 5. Create valid auxiliary dataset
  std::string aux_path = std::string(kTestDir) + "/aux.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  char** papszOpts = CSLAddString(nullptr, "METADATA=NO");
  auto poAuxDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(aux_path.c_str(), 0, 0, 0, GDT_Unknown, papszOpts));
  CSLDestroy(papszOpts);
  ASSERT_NE(nullptr, poAuxDS);

  OGRLayer* poLyr = poAuxDS->CreateLayer("aux_lyr", nullptr, wkbPoint);
  ASSERT_NE(nullptr, poLyr);
  OGRFeature* poF = new OGRFeature(poLyr->GetLayerDefn());
  OGRPoint pt(10.0, 20.0);
  poF->SetGeometry(&pt);
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateFeature(poF));
  delete poF;
  poAuxDS.reset();  // flush and close

  // Load layers without prefix (1 arg) -> returns 1
  std::string sql1 = "SELECT ogr_datasource_load_layers('" + aux_path + "');";
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, sql1.c_str(), -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  sqlite3_finalize(hStmt);

  // Verify created virtual table "aux_lyr"
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(hDB_, "SELECT count(*) FROM aux_lyr;",
                                          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  sqlite3_finalize(hStmt);

  // Load layers with prefix (3 args) -> returns 1
  std::string sql2 =
      "SELECT ogr_datasource_load_layers('" + aux_path + "', 0, 'pfx');";
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, sql2.c_str(), -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  sqlite3_finalize(hStmt);

  // Verify created virtual table "pfx_aux_lyr"
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT count(*) FROM pfx_aux_lyr;", -1,
                               &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsTest, GeodesicAreaAndLength) {
  sqlite3_stmt* hStmt = nullptr;

  // 1. ST_Area & Area with invalid geom -> NULL
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ST_Area('not_blob', 1);", -1,
                               &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(hDB_, "SELECT Area('not_blob', 1);",
                                          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 2. ST_Area with use_ellipsoid = 0 (triggers warning, assumes 4326) ->
  // double > 0
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_,
          "SELECT ST_Area(ST_GeomFromText('POLYGON((0 0,0 1,1 1,1 0,0 0))'), "
          "0);",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_FLOAT, sqlite3_column_type(hStmt, 0));
  EXPECT_GT(sqlite3_column_double(hStmt, 0), 0.0);
  sqlite3_finalize(hStmt);

  // 3. ST_Area with explicit SRID 4326 and use_ellipsoid = 1 -> double > 0
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_,
          "SELECT ST_Area(ST_GeomFromText('POLYGON((0 0,0 1,1 1,1 0,0 0))', "
          "4326), 1);",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_FLOAT, sqlite3_column_type(hStmt, 0));
  EXPECT_GT(sqlite3_column_double(hStmt, 0), 0.0);
  sqlite3_finalize(hStmt);

  // 4. ST_Area with invalid SRID -> NULL
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_,
          "SELECT ST_Area(ST_GeomFromText('POLYGON((0 0,0 1,1 1,1 0,0 0))', "
          "999999), 1);",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 5. ST_Length with invalid geom -> NULL
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ST_Length('not_blob', 1);", -1,
                               &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 6. ST_Length with use_ellipsoid = 0 -> double > 0
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_, "SELECT ST_Length(ST_GeomFromText('LINESTRING(0 0,1 1)'), 0);",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_FLOAT, sqlite3_column_type(hStmt, 0));
  EXPECT_GT(sqlite3_column_double(hStmt, 0), 0.0);
  sqlite3_finalize(hStmt);

  // 7. ST_Length with explicit SRID 4326 -> double > 0
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_,
          "SELECT ST_Length(ST_GeomFromText('LINESTRING(0 0,1 1)', 4326), "
          "1);",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_FLOAT, sqlite3_column_type(hStmt, 0));
  EXPECT_GT(sqlite3_column_double(hStmt, 0), 0.0);
  sqlite3_finalize(hStmt);

  // 8. ST_Length with invalid SRID -> NULL
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_,
          "SELECT ST_Length(ST_GeomFromText('LINESTRING(0 0,1 1)', 999999), "
          "1);",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsTest, GeometryConversions) {
  sqlite3_stmt* hStmt = nullptr;

  // 1. ST_AsText & AsText
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_,
                               "SELECT ST_AsText(ST_MakePoint(1.0, 2.0)), "
                               "AsText(ST_MakePoint(1.0, 2.0)), "
                               "ST_AsText('bad'), ST_AsText(X'000102');",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_STREQ("POINT (1 2)",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 0)));
  EXPECT_STREQ("POINT (1 2)",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 1)));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 2));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 3));
  sqlite3_finalize(hStmt);

  // 2. ST_AsBinary & AsBinary
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_,
                               "SELECT ST_AsBinary(ST_MakePoint(1.0, 2.0)), "
                               "AsBinary(ST_MakePoint(1.0, 2.0)), "
                               "ST_AsBinary('bad');",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 1));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 2));
  sqlite3_finalize(hStmt);

  // 3. ST_GeomFromText & GeomFromText
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(hDB_,
                                          "SELECT ST_GeomFromText(123), "
                                          "ST_GeomFromText('POINT (1 2)'), "
                                          "GeomFromText('POINT (1 2)', 4326), "
                                          "ST_GeomFromText('BAD_WKT');",
                                          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 1));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 2));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 3));
  sqlite3_finalize(hStmt);

  // 4. ST_GeomFromWKB & GeomFromWKB
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_GeomFromWKB('bad'), "
                "ST_GeomFromWKB(ST_AsBinary(ST_MakePoint(1.0, 2.0))), "
                "GeomFromWKB(ST_AsBinary(ST_MakePoint(1.0, 2.0)), 4326), "
                "ST_GeomFromWKB(X'deadbeef');",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 1));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 2));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 3));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsTest, BinaryPredicates) {
  sqlite3_stmt* hStmt = nullptr;

  // Intersects
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_Intersects(ST_MakePoint(0, 0), ST_MakePoint(0, 0)), "
                "Intersects(ST_MakePoint(0, 0), ST_MakePoint(1, 1)), "
                "ST_Intersects('bad', ST_MakePoint(0, 0)), "
                "ST_Intersects(ST_MakePoint(0, 0), 'bad');",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 1));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 2));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 3));
  sqlite3_finalize(hStmt);

  // Equals
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_Equals(ST_MakePoint(1, 1), ST_MakePoint(1, 1)), "
                "Equals(ST_MakePoint(1, 1), ST_MakePoint(2, 2)), "
                "ST_Equals('bad', ST_MakePoint(1, 1));",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 1));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 2));
  sqlite3_finalize(hStmt);

  // Disjoint
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_Disjoint(ST_MakePoint(0, 0), ST_MakePoint(1, 1)), "
                "Disjoint(ST_MakePoint(0, 0), ST_MakePoint(0, 0)), "
                "ST_Disjoint('bad', ST_MakePoint(0, 0));",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 1));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 2));
  sqlite3_finalize(hStmt);

  // Touches
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(hDB_,
                         "SELECT ST_Touches(ST_GeomFromText('POLYGON((0 0,0 "
                         "1,1 1,1 0,0 0))'), "
                         "ST_GeomFromText('POLYGON((1 0,1 1,2 1,2 0,1 0))')), "
                         "Touches(ST_MakePoint(0, 0), ST_MakePoint(1, 1)), "
                         "ST_Touches('bad', ST_MakePoint(0, 0));",
                         -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 1));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 2));
  sqlite3_finalize(hStmt);

  // Crosses
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_Crosses(ST_GeomFromText('LINESTRING(0 0,2 2)'), "
                "ST_GeomFromText('LINESTRING(0 2,2 0)')), "
                "Crosses(ST_MakePoint(0, 0), ST_MakePoint(1, 1)), "
                "ST_Crosses('bad', ST_MakePoint(0, 0));",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 1));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 2));
  sqlite3_finalize(hStmt);

  // Within
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(hDB_,
                         "SELECT ST_Within(ST_MakePoint(0.5, 0.5), "
                         "ST_GeomFromText('POLYGON((0 0,0 1,1 1,1 0,0 0))')), "
                         "Within(ST_MakePoint(5, 5), "
                         "ST_GeomFromText('POLYGON((0 0,0 1,1 1,1 0,0 0))')), "
                         "ST_Within('bad', ST_MakePoint(0, 0));",
                         -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 1));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 2));
  sqlite3_finalize(hStmt);

  // Contains
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_Contains(ST_GeomFromText('POLYGON((0 0,0 1,1 1,1 "
                "0,0 0))'), "
                "ST_MakePoint(0.5, 0.5)), "
                "Contains(ST_GeomFromText('POLYGON((0 0,0 1,1 1,1 0,0 0))'), "
                "ST_MakePoint(5, 5)), "
                "ST_Contains('bad', ST_MakePoint(0, 0));",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 1));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 2));
  sqlite3_finalize(hStmt);

  // Overlaps
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(hDB_,
                         "SELECT ST_Overlaps(ST_GeomFromText('POLYGON((0 0,0 "
                         "2,2 2,2 0,0 0))'), "
                         "ST_GeomFromText('POLYGON((1 1,1 3,3 3,3 1,1 1))')), "
                         "Overlaps(ST_MakePoint(0, 0), ST_MakePoint(1, 1)), "
                         "ST_Overlaps('bad', ST_MakePoint(0, 0));",
                         -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 1));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 2));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsTest, UnaryPredicates) {
  sqlite3_stmt* hStmt = nullptr;

  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_,
          "SELECT ST_IsEmpty(ST_GeomFromText('GEOMETRYCOLLECTION EMPTY')), "
          "IsEmpty(ST_MakePoint(0, 0)), "
          "ST_IsEmpty('bad'), "
          "ST_IsSimple(ST_MakePoint(0, 0)), "
          "IsSimple('bad'), "
          "ST_IsValid(ST_MakePoint(0, 0)), "
          "IsValid(ST_GeomFromText('POLYGON((0 0,2 2,0 2,2 0,0 0))')), "
          "ST_IsValid('bad');",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 1));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 2));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 3));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 4));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 5));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 6));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 7));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsTest, OverlayOperators) {
  sqlite3_stmt* hStmt = nullptr;

  // 1. Intersection
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_AsText(ST_Intersection("
                "ST_GeomFromText('POLYGON((0 0,0 2,2 2,2 0,0 0))', 4326), "
                "ST_GeomFromText('POLYGON((1 0,1 2,3 2,3 0,1 0))', 4326))), "
                "Intersection(ST_MakePoint(0, 0), 'bad'), "
                "ST_Intersection('bad', ST_MakePoint(0, 0));",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_TEXT, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 2));
  sqlite3_finalize(hStmt);

  // 2. Difference
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_AsText(Difference("
                "ST_GeomFromText('POLYGON((0 0,0 2,2 2,2 0,0 0))', 4326), "
                "ST_GeomFromText('POLYGON((1 0,1 2,3 2,3 0,1 0))', 4326))), "
                "ST_Difference('bad', ST_MakePoint(0, 0));",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_TEXT, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  sqlite3_finalize(hStmt);

  // 3. ST_Union
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_AsText(ST_Union("
                "ST_GeomFromText('POLYGON((0 0,0 2,2 2,2 0,0 0))', 4326), "
                "ST_GeomFromText('POLYGON((1 0,1 2,3 2,3 0,1 0))', 4326))), "
                "ST_Union('bad', ST_MakePoint(0, 0));",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_TEXT, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  sqlite3_finalize(hStmt);

  // 4. SymDifference
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_AsText(SymDifference("
                "ST_GeomFromText('POLYGON((0 0,0 2,2 2,2 0,0 0))', 4326), "
                "ST_GeomFromText('POLYGON((1 0,1 2,3 2,3 0,1 0))', 4326))), "
                "ST_SymDifference('bad', ST_MakePoint(0, 0));",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_TEXT, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsTest, GeometryPropertiesAndBuffer) {
  sqlite3_stmt* hStmt = nullptr;

  // SRID & ST_SRID
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_SRID(ST_GeomFromText('POINT (1 2)', 4326)), "
                "SRID(ST_MakePoint(1, 2)), "
                "ST_SRID('bad');",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(4326, sqlite3_column_int(hStmt, 0));
  EXPECT_EQ(-1, sqlite3_column_int(hStmt, 1));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 2));
  sqlite3_finalize(hStmt);

  // Area & ST_Area (1 arg)
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_,
          "SELECT ST_Area(ST_GeomFromText('POLYGON((0 0,0 2,2 2,2 0,0 0))')), "
          "Area(ST_MakePoint(1, 2)), "
          "ST_Area('bad');",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_DOUBLE_EQ(4.0, sqlite3_column_double(hStmt, 0));
  EXPECT_DOUBLE_EQ(0.0, sqlite3_column_double(hStmt, 1));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 2));
  sqlite3_finalize(hStmt);

  // ST_Length (1 arg)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_Length(ST_GeomFromText('LINESTRING(0 0,0 3)')), "
                "ST_Length('bad');",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_DOUBLE_EQ(3.0, sqlite3_column_double(hStmt, 0));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  sqlite3_finalize(hStmt);

  // Buffer & ST_Buffer (with double dist, int dist, and errors)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_,
                               "SELECT ST_Buffer(ST_MakePoint(0, 0), 1.5), "
                               "Buffer(ST_MakePoint(0, 0), 2), "
                               "ST_Buffer(ST_MakePoint(0, 0), 'bad'), "
                               "ST_Buffer('bad', 1.0);",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 1));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 2));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 3));
  sqlite3_finalize(hStmt);

  // MakePoint & ST_MakePoint (2 and 3 args, plus errors)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_,
                               "SELECT ST_AsText(ST_MakePoint(1.5, 2.5)), "
                               "ST_AsText(MakePoint(1, 2, 3)), "
                               "ST_MakePoint('bad', 2.0), "
                               "ST_MakePoint(1.0, 'bad'), "
                               "ST_MakePoint(1.0, 2.0, 'bad');",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_STREQ("POINT (1.5 2.5)",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 0)));
  EXPECT_STREQ("POINT (1 2 3)",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 1)));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 2));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 3));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 4));
  sqlite3_finalize(hStmt);

  // MakeValid & ST_MakeValid
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ST_AsText(ST_MakeValid("
                "ST_GeomFromText('POLYGON((0 0,2 2,0 2,2 0,0 0))', 4326))), "
                "MakeValid(ST_MakePoint(1, 2)), "
                "ST_MakeValid('bad');",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_TEXT, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 1));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 2));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsTest, HStoreGetValue) {
  sqlite3_stmt* hStmt = nullptr;

  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_,
                               "SELECT hstore_get_value('a=>1,b=>2', 'b'), "
                               "hstore_get_value('a=>1', 'missing'), "
                               "hstore_get_value(123, 'a'), "
                               "hstore_get_value('a=>1', 123);",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_STREQ("2",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 0)));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 2));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 3));
  sqlite3_finalize(hStmt);
}

// TODO: b/335317901 - Cover OGRSQLiteSQLFunctions GEOS geometry methods if GEOS
// is enabled.

}  // namespace
}  // namespace autotest2
