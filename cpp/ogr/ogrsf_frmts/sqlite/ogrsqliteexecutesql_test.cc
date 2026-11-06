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
// Tests for SQLite ExecuteSQL dialect (ogrsqliteexecutesql.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqliteexecutesql.cpp
//
// google3 build caveats:
//   - Compiled without HAVE_SPATIALITE; SQLite dialect uses VirtualOGR and
//     common OGR SQLite SQL functions.

#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogrsqliteexecutesql.h"

#include <memory>
#include <set>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

void GDALRegister_MEM();

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/sqlite_execsql_test";

class OGRSQLiteExecuteSQLTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRSQLite();
    GDALRegister_MEM();
    VSIMkdir(kTestDir, 0777);
  }

  void TearDown() override {
    CPLSetConfigOption("OGR_SQLITE_DIALECT_ALLOW_CREATE_VIRTUAL_TABLE",
                       nullptr);
    VSIRmdirRecursive(kTestDir);
  }
};

TEST_F(OGRSQLiteExecuteSQLTest, GetReferencedLayersParser) {
  // 1. Basic layer reference
  std::set<LayerDesc> layers =
      OGRSQLiteGetReferencedLayers("SELECT * FROM my_layer");
  EXPECT_EQ(1, layers.size());
  EXPECT_EQ("my_layer", layers.begin()->osLayerName);
  EXPECT_TRUE(layers.begin()->osDSName.empty());

  // 2. Quoted names and quotes inside quotes
  layers = OGRSQLiteGetReferencedLayers("SELECT * FROM \"my\"\"layer\"");
  EXPECT_EQ(1, layers.size());
  EXPECT_EQ("my\"layer", layers.begin()->osLayerName);

  layers = OGRSQLiteGetReferencedLayers("SELECT * FROM 'my''layer'");
  EXPECT_EQ(1, layers.size());
  EXPECT_EQ("my'layer", layers.begin()->osLayerName);

  // 3. DataSource.layer syntax, repeated layer reference
  const char* pszMultiDS =
      "SELECT a.id, b.id FROM 'ds1.db'.'lyr1' a "
      "JOIN 'ds1.db'.'lyr1' b ON a.id = b.id "
      "JOIN \"ds2.db\".\"lyr2\" c ON a.id = c.id";
  layers = OGRSQLiteGetReferencedLayers(pszMultiDS);
  EXPECT_EQ(2, layers.size());

  // 4. ogr_layer_ function calls
  layers = OGRSQLiteGetReferencedLayers("SELECT ogr_layer_Extent('lyr_ext')");
  EXPECT_EQ(1, layers.size());
  EXPECT_EQ("lyr_ext", layers.begin()->osLayerName);

  // ogr_layer_ without parenthesis
  layers = OGRSQLiteGetReferencedLayers("SELECT ogr_layer_something");
  EXPECT_EQ(0, layers.size());

  // 5. SpatialIndex with f_table_name
  layers = OGRSQLiteGetReferencedLayers(
      "SELECT * FROM SpatialIndex WHERE f_table_name = 'lyr_sp'");
  EXPECT_EQ(
      0,
      layers.size());  // SpatialIndex is recognized as spatial index, not table

  // 6. Subqueries and parentheses in FROM
  layers = OGRSQLiteGetReferencedLayers(
      "SELECT * FROM (SELECT * FROM sub1), (SELECT * FROM sub2) WHERE 1 = 1");
  EXPECT_EQ(2, layers.size());

  // 7. Aliases with and without AS, keywords
  layers = OGRSQLiteGetReferencedLayers(
      "SELECT * FROM lyr1 AS l1, lyr2 l2 WHERE l1.id = l2.id "
      "GROUP BY l1.id ORDER BY l1.id LIMIT 10");
  EXPECT_EQ(2, layers.size());

  // 8. JOIN, INTO, UPDATE statements
  layers = OGRSQLiteGetReferencedLayers("INSERT INTO lyr_into VALUES (1)");
  EXPECT_EQ(1, layers.size());
  EXPECT_EQ("lyr_into", layers.begin()->osLayerName);

  layers = OGRSQLiteGetReferencedLayers("UPDATE lyr_up SET val = 1");
  EXPECT_EQ(1, layers.size());
  EXPECT_EQ("lyr_up", layers.begin()->osLayerName);
}

TEST_F(OGRSQLiteExecuteSQLTest, ExecuteSQLCommandsAndQueries) {
  GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDriver);

  std::unique_ptr<GDALDataset> poDS(
      poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poDS);

  OGRSpatialReference oSRS;
  oSRS.importFromEPSG(4326);
  OGRLayer* poSrcLayer = poDS->CreateLayer("poly", &oSRS, wkbPoint, nullptr);
  ASSERT_NE(nullptr, poSrcLayer);

  OGRFieldDefn oFieldId("id", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, poSrcLayer->CreateField(&oFieldId));
  OGRFieldDefn oFieldName("name", OFTString);
  ASSERT_EQ(OGRERR_NONE, poSrcLayer->CreateField(&oFieldName));

  OGRGeomFieldDefn oGeomField3D("geom3d", wkbPoint25D);
  poSrcLayer->CreateGeomField(&oGeomField3D);

  // Insert test features
  OGRFeatureUniquePtr poFeature(new OGRFeature(poSrcLayer->GetLayerDefn()));
  poFeature->SetField("id", 1);
  poFeature->SetField("name", "Alice");
  OGRPoint pt1(1.0, 2.0);
  poFeature->SetGeometry(&pt1);
  OGRPoint pt1_3d(1.0, 2.0, 3.0);
  poFeature->SetGeomField(1, &pt1_3d);
  ASSERT_EQ(OGRERR_NONE, poSrcLayer->CreateFeature(poFeature.get()));

  poFeature.reset(new OGRFeature(poSrcLayer->GetLayerDefn()));
  poFeature->SetField("id", 2);
  poFeature->SetField("name", "Bob");
  OGRPoint pt2(10.0, 20.0);
  poFeature->SetGeometry(&pt2);
  ASSERT_EQ(OGRERR_NONE, poSrcLayer->CreateFeature(poFeature.get()));
  poFeature.reset();

  // 1. Rejected commands
  EXPECT_EQ(nullptr, poDS->ExecuteSQL("ALTER TABLE poly ADD COLUMN x INT",
                                      nullptr, "SQLITE"));
  EXPECT_EQ(nullptr, poDS->ExecuteSQL("DROP TABLE poly", nullptr, "SQLITE"));
  EXPECT_EQ(nullptr, poDS->ExecuteSQL("CREATE INDEX idx ON poly(id)", nullptr,
                                      "SQLITE"));
  EXPECT_EQ(nullptr, poDS->ExecuteSQL("DROP INDEX idx", nullptr, "SQLITE"));

  // 2. CREATE VIRTUAL TABLE with config option
  CPLSetConfigOption("OGR_SQLITE_DIALECT_ALLOW_CREATE_VIRTUAL_TABLE", "YES");
  absl::Cleanup reset_opt = [] {
    CPLSetConfigOption("OGR_SQLITE_DIALECT_ALLOW_CREATE_VIRTUAL_TABLE",
                       nullptr);
  };
  OGRLayer* poVT = poDS->ExecuteSQL(
      "CREATE VIRTUAL TABLE vt USING VirtualOGR('poly')", nullptr, "SQLITE");
  EXPECT_EQ(nullptr, poVT);

  // 3. Unsupported command
  EXPECT_EQ(nullptr,
            poDS->ExecuteSQL("PRAGMA table_info(poly)", nullptr, "SQLITE"));

  // 4. Valid SELECT with OGR_STYLE and single layer metadata
  OGRLayer* poRes = poDS->ExecuteSQL(
      "SELECT id, name, OGR_STYLE, GEOMETRY, geom3d FROM poly ORDER BY id",
      nullptr, "SQLITE");
  ASSERT_NE(nullptr, poRes);
  absl::Cleanup release_res = [&] { poDS->ReleaseResultSet(poRes); };
  EXPECT_FALSE(poRes->TestCapability(OLCStringsAsUTF8));
  EXPECT_FALSE(poRes->TestCapability(OLCRandomWrite));

  OGRFeatureUniquePtr poF(poRes->GetNextFeature());
  ASSERT_NE(nullptr, poF);
  EXPECT_EQ(1, poF->GetFieldAsInteger("id"));
  EXPECT_STREQ("Alice", poF->GetFieldAsString("name"));

  poF.reset(poRes->GetNextFeature());
  ASSERT_NE(nullptr, poF);
  EXPECT_EQ(2, poF->GetFieldAsInteger("id"));
  EXPECT_STREQ("Bob", poF->GetFieldAsString("name"));

  EXPECT_EQ(nullptr, poRes->GetNextFeature());

  // 5. Empty SELECT resultset (bEmptyLayer = true)
  OGRLayer* poEmpty =
      poDS->ExecuteSQL("SELECT * FROM poly WHERE 1 = 0", nullptr, "SQLITE");
  ASSERT_NE(nullptr, poEmpty);
  absl::Cleanup release_empty = [&] { poDS->ReleaseResultSet(poEmpty); };
  EXPECT_EQ(0, poEmpty->GetFeatureCount(true));
  EXPECT_EQ(nullptr, poEmpty->GetNextFeature());

  // 6. DML statements
  OGRLayer* poInsert = poDS->ExecuteSQL(
      "INSERT INTO poly (id, name) VALUES (3, 'Charlie')", nullptr, "SQLITE");
  EXPECT_EQ(nullptr, poInsert);  // DML returns nullptr layer on success
  EXPECT_EQ(3, poSrcLayer->GetFeatureCount(true));

  OGRLayer* poUpdate = poDS->ExecuteSQL(
      "UPDATE poly SET name = 'AliceUpdated' WHERE id = 1", nullptr, "SQLITE");
  EXPECT_EQ(nullptr, poUpdate);

  OGRLayer* poDelete =
      poDS->ExecuteSQL("DELETE FROM poly WHERE id = 3", nullptr, "SQLITE");
  EXPECT_EQ(nullptr, poDelete);
  EXPECT_EQ(OGRERR_NONE, poSrcLayer->SetAttributeFilter(nullptr));
  EXPECT_EQ(2, poSrcLayer->GetFeatureCount(true));

  // 7. Invalid SQL
  EXPECT_EQ(nullptr, poDS->ExecuteSQL("SELECT non_existent_col FROM poly",
                                      nullptr, "SQLITE"));

  // 8. Spatial filter passed to ExecuteSQL
  OGRPolygon oFilterPoly;
  OGRLinearRing oRing;
  oRing.addPoint(0, 0);
  oRing.addPoint(5, 0);
  oRing.addPoint(5, 5);
  oRing.addPoint(0, 5);
  oRing.addPoint(0, 0);
  oFilterPoly.addRing(&oRing);

  OGRLayer* poSpatial = poDS->ExecuteSQL(
      "SELECT id, name, GEOMETRY, geom3d FROM poly", &oFilterPoly, "SQLITE");
  ASSERT_NE(nullptr, poSpatial);
  absl::Cleanup release_spatial = [&] { poDS->ReleaseResultSet(poSpatial); };
  EXPECT_EQ(1, poSpatial->GetFeatureCount(true));
  OGRFeatureUniquePtr poSpatialF(poSpatial->GetNextFeature());
  ASSERT_NE(nullptr, poSpatialF);
  EXPECT_EQ(1, poSpatialF->GetFieldAsInteger("id"));
}

TEST_F(OGRSQLiteExecuteSQLTest, ExternalDataSourceAttachment) {
  GDALDriver* poSQLiteDriver =
      GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(nullptr, poSQLiteDriver);

  std::string osExtDB = std::string(kTestDir) + "/ext.sqlite";
  GDALDatasetUniquePtr poExtDS(
      poSQLiteDriver->Create(osExtDB.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poExtDS);

  OGRLayer* poExtLyr =
      poExtDS->CreateLayer("ext_lyr", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, poExtLyr);
  OGRFieldDefn oFieldVal("val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, poExtLyr->CreateField(&oFieldVal));

  OGRFeatureUniquePtr poF(new OGRFeature(poExtLyr->GetLayerDefn()));
  poF->SetField("val", 999);
  ASSERT_EQ(OGRERR_NONE, poExtLyr->CreateFeature(poF.get()));
  poF.reset();
  poExtDS.reset();

  // Main dataset
  GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> poMainDS(
      poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poMainDS);
  poMainDS->CreateLayer("dummy", nullptr, wkbNone, nullptr);

  // 1. Valid external table query
  std::string osSQL = "SELECT val FROM '" + osExtDB + "'.'ext_lyr'";
  OGRLayer* poRes = poMainDS->ExecuteSQL(osSQL.c_str(), nullptr, "SQLITE");
  ASSERT_NE(nullptr, poRes);
  absl::Cleanup release_res = [&] { poMainDS->ReleaseResultSet(poRes); };
  OGRFeatureUniquePtr poFeat(poRes->GetNextFeature());
  ASSERT_NE(nullptr, poFeat);
  EXPECT_EQ(999, poFeat->GetFieldAsInteger("val"));

  // 2. Missing external datasource
  std::string osSQLMissingDS =
      "SELECT val FROM '/vsimem/sqlite_execsql_test/missing.sqlite'.'ext_lyr'";
  EXPECT_EQ(nullptr,
            poMainDS->ExecuteSQL(osSQLMissingDS.c_str(), nullptr, "SQLITE"));

  // 3. Missing external layer
  std::string osSQLMissingLyr =
      "SELECT val FROM '" + osExtDB + "'.'missing_lyr'";
  EXPECT_EQ(nullptr,
            poMainDS->ExecuteSQL(osSQLMissingLyr.c_str(), nullptr, "SQLITE"));
}

// TODO: b/335317901 - Cover OGRSQLiteExecuteSQL spatial index evaluation when
// spatialite is loaded.

}  // namespace
}  // namespace autotest2
