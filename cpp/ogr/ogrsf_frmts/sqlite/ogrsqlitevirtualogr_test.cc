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
// Unit tests for SQLite VirtualOGR virtual table module
// (ogrsqlitevirtualogr.cpp).
//
// See also:
//   https://gdal.org/drivers/vector/sqlite.html
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqlitevirtualogr.cpp
//
// google3 build caveats:
//   - Compiled without HAVE_SPATIALITE; VirtualOGR is statically registered.

#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogrsqlitevirtualogr.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogr_sqlite.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/sqlite/sqlite3.h"

extern "C" int sqlite3_extension_init(sqlite3* hDB, char** pzErrMsg,
                                      const void* pApi);
extern "C" int OGR2SQLITE_static_register(sqlite3* hDB, char** pzErrMsg,
                                          void* pApi);

namespace autotest2 {
namespace {

static const void* g_pCapturedSqliteApi = nullptr;

extern "C" int CaptureSqliteApi(sqlite3*, char**, const void* pApi) {
  g_pCapturedSqliteApi = pApi;
  return SQLITE_OK;
}

constexpr const char* kTestDir = "/vsimem/sqlite_vogr_test";

class DummyVogrLayer : public OGRLayer {
 public:
  DummyVogrLayer(const char* pszName, bool bWithFIDCol, bool bFIDInFields,
                 bool bFastCount)
      : bFastCount_(bFastCount) {
    poDefn_ = new OGRFeatureDefn(pszName);
    poDefn_->Reference();
    if (bWithFIDCol) {
      osFIDCol_ = "custom_fid";
      if (bFIDInFields) {
        OGRFieldDefn oField("custom_fid", OFTInteger);
        poDefn_->AddFieldDefn(&oField);
      }
    }
    OGRFieldDefn oFieldStr("str_col", OFTString);
    poDefn_->AddFieldDefn(&oFieldStr);

    poDefn_->GetGeomFieldDefn(0)->SetName("geom");
    poDefn_->GetGeomFieldDefn(0)->SetType(wkbPoint);
    oSRS_.importFromEPSG(4326);
  }

  ~DummyVogrLayer() override { poDefn_->Release(); }

  OGRFeatureDefn* GetLayerDefn() { return poDefn_; }
  const OGRFeatureDefn* GetLayerDefn() const override { return poDefn_; }
  const char* GetFIDColumn() const override { return osFIDCol_.c_str(); }
  const OGRSpatialReference* GetSpatialRef() const override { return &oSRS_; }

  void ResetReading() override { iCur_ = 0; }

  OGRFeature* GetNextFeature() override {
    if (iCur_ >= 2) return nullptr;
    auto poF = std::make_unique<OGRFeature>(poDefn_);
    poF->SetFID(iCur_ + 1);
    poF->SetField("str_col", iCur_ == 0 ? "val0" : "val1");
    if (iCur_ == 0) {
      auto poPt = std::make_unique<OGRPoint>(iCur_, iCur_);
      poF->SetGeometry(poPt.get());
    }
    iCur_++;
    return poF.release();
  }

  int TestCapability(const char* pszCap) const override {
    if (EQUAL(pszCap, OLCFastFeatureCount)) return bFastCount_ ? 1 : 0;
    if (EQUAL(pszCap, OLCRandomWrite)) return 1;
    if (EQUAL(pszCap, OLCSequentialWrite)) return 1;
    return 0;
  }

  GIntBig GetFeatureCount(int) override { return bFastCount_ ? 2 : -1; }

  OGRErr SetAttributeFilter(const char* pszQuery) override {
    if (bFailAttrFilter_) return OGRERR_FAILURE;
    return OGRLayer::SetAttributeFilter(pszQuery);
  }

  OGRErr IGetExtent(int, OGREnvelope* psExtent, bool) override {
    if (bFailExtent_) return OGRERR_FAILURE;
    psExtent->MinX = 0;
    psExtent->MaxX = 1;
    psExtent->MinY = 0;
    psExtent->MaxY = 1;
    return OGRERR_NONE;
  }

  OGRErr ICreateFeature(OGRFeature* poFeature) override {
    created_fids_.push_back(poFeature->GetFID());
    return OGRERR_NONE;
  }

  bool bFastCount_ = false;
  bool bFailAttrFilter_ = false;
  bool bFailExtent_ = false;
  std::string osFIDCol_;
  OGRSpatialReference oSRS_;
  OGRFeatureDefn* poDefn_ = nullptr;
  int iCur_ = 0;
  std::vector<GIntBig> created_fids_;
};

class DummyVogrDataset : public GDALDataset {
 public:
  DummyVogrDataset() = default;
  int GetLayerCount() const override {
    return static_cast<int>(layers_.size());
  }
  const OGRLayer* GetLayer(int i) const override {
    if (i >= 0 && i < static_cast<int>(layers_.size())) return layers_[i].get();
    return nullptr;
  }
  OGRLayer* GetLayerByName(const char* pszName) override {
    for (auto& lyr : layers_) {
      if (EQUAL(lyr->GetName(), pszName)) return lyr.get();
    }
    return nullptr;
  }
  void AddLayer(std::unique_ptr<OGRLayer> lyr) {
    layers_.push_back(std::move(lyr));
  }
  std::vector<std::unique_ptr<OGRLayer>> layers_;
};

class OGRSQLiteVirtualOGRTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRSQLite();
    GDALRegister_MEM();
    VSIRmdirRecursive(kTestDir);
    VSIMkdir(kTestDir, 0777);

    db_path_ = std::string(kTestDir) + "/main.db";
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
    CPLSetConfigOption("OGR_SQLITE_STATIC_VIRTUAL_OGR", nullptr);
    poDS_.reset();
    VSIRmdirRecursive(kTestDir);
  }

  static void TearDownTestSuite() { OSRCleanup(); }

  std::string db_path_;
  std::unique_ptr<GDALDataset> poDS_;
  sqlite3* hDB_ = nullptr;
};

TEST_F(OGRSQLiteVirtualOGRTest, GetNameForGeometryColumn) {
  GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDriver);
  auto poDS = std::unique_ptr<GDALDataset>(
      poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poDS);

  // 1. Layer with fields named GEOMETRY and GEOMETRY2 -> returns GEOMETRY3
  OGRLayer* poLyr1 = poDS->CreateLayer("lyr1", nullptr, wkbPoint);
  ASSERT_NE(nullptr, poLyr1);
  OGRFieldDefn oField1("GEOMETRY", OFTString);
  OGRFieldDefn oField2("GEOMETRY2", OFTString);
  ASSERT_EQ(OGRERR_NONE, poLyr1->CreateField(&oField1));
  ASSERT_EQ(OGRERR_NONE, poLyr1->CreateField(&oField2));
  EXPECT_STREQ("GEOMETRY3",
               OGR2SQLITE_GetNameForGeometryColumn(poLyr1).c_str());

  // 2. Layer with named geometry column not colliding with fields
  OGRLayer* poLyr2 = poDS->CreateLayer("lyr2", nullptr, wkbNone);
  ASSERT_NE(nullptr, poLyr2);
  OGRGeomFieldDefn oGeomField("custom_geom", wkbPoint);
  ASSERT_EQ(OGRERR_NONE, poLyr2->CreateGeomField(&oGeomField));
  EXPECT_STREQ("custom_geom",
               OGR2SQLITE_GetNameForGeometryColumn(poLyr2).c_str());
}

TEST_F(OGRSQLiteVirtualOGRTest, ExternalConnectCreateErrors) {
  // 1. Invalid argument counts (< 4 or > 8)
  EXPECT_NE(SQLITE_OK, sqlite3_exec(hDB_,
                                    "CREATE VIRTUAL TABLE vt_err1 USING "
                                    "VirtualOGR();",
                                    nullptr, nullptr, nullptr));

  EXPECT_NE(SQLITE_OK, sqlite3_exec(hDB_,
                                    "CREATE VIRTUAL TABLE vt_err2 USING "
                                    "VirtualOGR('1', '2', '3', '4', '5', '6');",
                                    nullptr, nullptr, nullptr));

  // 2. Invalid update_mode (not 0 or 1)
  std::string one_db = std::string(kTestDir) + "/one.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  auto poAuxDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(one_db.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poAuxDS);
  poAuxDS->CreateLayer("one_lyr", nullptr, wkbPoint);
  poAuxDS.reset();

  EXPECT_NE(SQLITE_OK,
            sqlite3_exec(hDB_,
                         CPLSPrintf("CREATE VIRTUAL TABLE vt_err3 USING "
                                    "VirtualOGR('%s', 'invalid_mode');",
                                    one_db.c_str()),
                         nullptr, nullptr, nullptr));

  // 3. Nonexistent datasource
  EXPECT_NE(SQLITE_OK, sqlite3_exec(hDB_,
                                    "CREATE VIRTUAL TABLE vt_err4 USING "
                                    "VirtualOGR('/vsimem/nonexistent_ds.db');",
                                    nullptr, nullptr, nullptr));

  // 4. Datasource with 0 layers
  std::string empty_db = std::string(kTestDir) + "/empty.db";
  auto poEmptyDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(empty_db.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  poEmptyDS.reset();

  EXPECT_NE(SQLITE_OK,
            sqlite3_exec(hDB_,
                         CPLSPrintf("CREATE VIRTUAL TABLE vt_err5 USING "
                                    "VirtualOGR('%s');",
                                    empty_db.c_str()),
                         nullptr, nullptr, nullptr));

  // 5. Datasource with 2 layers without selecting a layer
  std::string two_db = std::string(kTestDir) + "/two.db";
  auto poTwoDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(two_db.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poTwoDS);
  poTwoDS->CreateLayer("layer_a", nullptr, wkbPoint);
  poTwoDS->CreateLayer("layer_b", nullptr, wkbPoint);
  poTwoDS.reset();

  EXPECT_NE(SQLITE_OK,
            sqlite3_exec(hDB_,
                         CPLSPrintf("CREATE VIRTUAL TABLE vt_err6 USING "
                                    "VirtualOGR('%s', 0);",
                                    two_db.c_str()),
                         nullptr, nullptr, nullptr));

  // 6. Nonexistent layer name
  EXPECT_NE(SQLITE_OK,
            sqlite3_exec(hDB_,
                         CPLSPrintf("CREATE VIRTUAL TABLE vt_err7 USING "
                                    "VirtualOGR('%s', 0, 'missing_lyr');",
                                    two_db.c_str()),
                         nullptr, nullptr, nullptr));

  // 7. Datasource with 1 layer without specifying layer name
  EXPECT_EQ(SQLITE_OK,
            sqlite3_exec(hDB_,
                         CPLSPrintf("CREATE VIRTUAL TABLE vt_one_ok USING "
                                    "VirtualOGR('%s', 0);",
                                    one_db.c_str()),
                         nullptr, nullptr, nullptr));
  EXPECT_EQ(SQLITE_OK, sqlite3_exec(hDB_, "DROP TABLE vt_one_ok;", nullptr,
                                    nullptr, nullptr));
}

TEST_F(OGRSQLiteVirtualOGRTest, SuspiciousUsageDetection) {
  std::string one_db = std::string(kTestDir) + "/susp.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  char** papszOpts = CSLAddString(nullptr, "METADATA=NO");
  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(one_db.c_str(), 0, 0, 0, GDT_Unknown, papszOpts));
  CSLDestroy(papszOpts);
  ASSERT_NE(nullptr, poDS);
  poDS->CreateLayer("lyr", nullptr, wkbPoint);
  poDS.reset();

  // Create a view referencing ogr_layer_ in comments, which appears in
  // sqlite_master.sql
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_exec(
          hDB_, "CREATE VIEW view_susp AS SELECT 1 AS col /* ogr_layer_ */ ;",
          nullptr, nullptr, nullptr));

  // With default (ALLOW_VIRTUAL_OGR_FROM_TRIGGER_AND_VIEW=NO), detection fails
  CPLSetConfigOption("ALLOW_VIRTUAL_OGR_FROM_TRIGGER_AND_VIEW", "NO");
  EXPECT_NE(SQLITE_OK,
            sqlite3_exec(hDB_,
                         CPLSPrintf("CREATE VIRTUAL TABLE vt_susp1 USING "
                                    "VirtualOGR('%s', 0, 'lyr');",
                                    one_db.c_str()),
                         nullptr, nullptr, nullptr));

  // With YES, detection allows creation
  CPLSetConfigOption("ALLOW_VIRTUAL_OGR_FROM_TRIGGER_AND_VIEW", "YES");
  char* pzErrMsg = nullptr;
  int rc = sqlite3_exec(hDB_,
                        CPLSPrintf("CREATE VIRTUAL TABLE vt_susp2 USING "
                                   "VirtualOGR('%s', 0, 'lyr');",
                                   one_db.c_str()),
                        nullptr, nullptr, &pzErrMsg);
  EXPECT_EQ(SQLITE_OK, rc) << "SQLite error: "
                           << (pzErrMsg ? pzErrMsg : "none");
  sqlite3_free(pzErrMsg);

  CPLSetConfigOption("ALLOW_VIRTUAL_OGR_FROM_TRIGGER_AND_VIEW", nullptr);
  if (rc == SQLITE_OK) {
    EXPECT_EQ(SQLITE_OK, sqlite3_exec(hDB_, "DROP TABLE vt_susp2;", nullptr,
                                      nullptr, nullptr));
  }
  EXPECT_EQ(SQLITE_OK, sqlite3_exec(hDB_, "DROP VIEW view_susp;", nullptr,
                                    nullptr, nullptr));
}

TEST_F(OGRSQLiteVirtualOGRTest, VirtualOGRFullLifecycleAndQueries) {
  // Create a rich dataset to test all column types, curves, styles, and native
  // data
  std::string full_db = std::string(kTestDir) + "/full.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  char** papszOpts = CSLAddString(nullptr, "METADATA=NO");
  auto poFullDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(full_db.c_str(), 0, 0, 0, GDT_Unknown, papszOpts));
  CSLDestroy(papszOpts);
  ASSERT_NE(nullptr, poFullDS);

  OGRSpatialReference oSRS;
  oSRS.importFromEPSG(4326);
  char** papszLyrOpts = CSLAddString(nullptr, "LAUNDER=NO");
  OGRLayer* poLyr =
      poFullDS->CreateLayer("full_lyr", &oSRS, wkbCircularString, papszLyrOpts);
  CSLDestroy(papszLyrOpts);
  ASSERT_NE(nullptr, poLyr);

  OGRFieldDefn fInt("int_val", OFTInteger);
  OGRFieldDefn fInt64("int64_val", OFTInteger64);
  OGRFieldDefn fReal("real_val", OFTReal);
  OGRFieldDefn fStr("str_val", OFTString);
  OGRFieldDefn fBin("bin_val", OFTBinary);
  OGRFieldDefn fDate("date_val", OFTDate);
  OGRFieldDefn fTime1("time_val1", OFTTime);
  OGRFieldDefn fTime2("time_val2", OFTTime);
  OGRFieldDefn fDT("dt_val", OFTDateTime);
  OGRFieldDefn fKw("SELECT", OFTString);
  OGRFieldDefn fHyphen("hyphen-col", OFTString);
  OGRFieldDefn fNull("null_val", OFTInteger);

  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fInt));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fInt64));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fReal));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fStr));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fBin));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fDate));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fTime1));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fTime2));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fDT));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fKw));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fHyphen));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fNull));

  OGRGeomFieldDefn fGeom2("geom2", wkbPoint);
  fGeom2.SetSpatialRef(&oSRS);
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateGeomField(&fGeom2));

  // Feature 1
  {
    OGRFeature* poF = new OGRFeature(poLyr->GetLayerDefn());
    poF->SetFID(1);
    poF->SetField(0, 10);
    poF->SetField(1, static_cast<GIntBig>(10000000000LL));
    poF->SetField(2, 1.23);
    poF->SetField(3, "val_one");
    GByte pabyBin[] = {0x01, 0x02, 0x03};
    poF->SetField(4, 3, pabyBin);
    poF->SetField(5, "2026-11-03");
    poF->SetField(6, "12:34:56.789");
    poF->SetField(7, "12:34:56");
    poF->SetField(8, "2026-11-03 12:34:56");
    poF->SetField(9, "sel_val");
    poF->SetField(10, "hyphen_val");
    // null_val is unset

    poF->SetStyleString("PEN(c:#FF0000)");
    poF->SetNativeData("{\"type\":\"Feature\"}");
    poF->SetNativeMediaType("application/geo+json");

    OGRCircularString* poCS = new OGRCircularString();
    poCS->addPoint(0.0, 0.0);
    poCS->addPoint(1.0, 1.0);
    poCS->addPoint(2.0, 0.0);
    poF->SetGeomFieldDirectly(0, poCS);

    OGRPoint* poPt = new OGRPoint(5.0, 5.0);
    poF->SetGeomFieldDirectly(1, poPt);

    ASSERT_EQ(OGRERR_NONE, poLyr->CreateFeature(poF));
    delete poF;
  }

  // Feature 2
  {
    OGRFeature* poF = new OGRFeature(poLyr->GetLayerDefn());
    poF->SetFID(2);
    poF->SetField(0, 20);
    poF->SetField(1, static_cast<GIntBig>(20000000000LL));
    poF->SetField(2, 4.56);
    poF->SetField(3, "val_two");
    poF->SetField(5, "2026-12-01");
    poF->SetField(9, "sel_val2");

    ASSERT_EQ(OGRERR_NONE, poLyr->CreateFeature(poF));
    delete poF;
  }

  poFullDS.reset();  // flush and close

  // Create VirtualOGR table with update=1, expose_ogr_style=1,
  // expose_ogr_native_data=1
  ASSERT_EQ(SQLITE_OK,
            sqlite3_exec(hDB_,
                         CPLSPrintf("CREATE VIRTUAL TABLE vt_full USING "
                                    "VirtualOGR('%s', 1, 'full_lyr', 1, 1);",
                                    full_db.c_str()),
                         nullptr, nullptr, nullptr));

  sqlite3_stmt* hStmt = nullptr;

  // 1. SELECT all columns covering OGR2SQLITE_Column for all types
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_,
          "SELECT ogc_fid, int_val, int64_val, real_val, str_val, bin_val, "
          "date_val, time_val1, time_val2, dt_val, \"SELECT\", \"hyphen-col\", "
          "null_val, OGR_STYLE, GEOMETRY, geom2, OGR_NATIVE_DATA, "
          "OGR_NATIVE_MEDIA_TYPE, rowid FROM vt_full WHERE ogc_fid = 1;",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int64(hStmt, 0));
  EXPECT_EQ(10, sqlite3_column_int(hStmt, 1));
  EXPECT_EQ(10000000000LL, sqlite3_column_int64(hStmt, 2));
  EXPECT_DOUBLE_EQ(1.23, sqlite3_column_double(hStmt, 3));
  EXPECT_STREQ("val_one",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 4)));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 5));
  EXPECT_STREQ("2026-11-03",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 6)));
  EXPECT_STREQ("12:34:56.789",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 7)));
  EXPECT_STREQ("12:34:56",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 8)));
  EXPECT_NE(nullptr, sqlite3_column_text(hStmt, 9));
  EXPECT_STREQ("sel_val",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 10)));
  EXPECT_STREQ("hyphen_val",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 11)));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 12));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 13));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 14));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 15));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 16));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 17));
  EXPECT_EQ(1, sqlite3_column_int64(hStmt, 18));
  sqlite3_finalize(hStmt);

  // 2. Feature 2 (has null geometries -> Column handles null geoms)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_,
                               "SELECT GEOMETRY, geom2 FROM vt_full WHERE "
                               "ogc_fid = 2;",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  sqlite3_finalize(hStmt);

  // 3. BestIndex & Filter constraints: EQ, GT, LE, LT, GE, LIKE, NE, IS NOT
  // NULL, IS NULL, GLOB
  const char* apszFilterQueries[] = {
      "SELECT count(*) FROM vt_full WHERE int_val = 10;",
      "SELECT count(*) FROM vt_full WHERE int_val > 5;",
      "SELECT count(*) FROM vt_full WHERE int_val <= 20;",
      "SELECT count(*) FROM vt_full WHERE int_val < 20;",
      "SELECT count(*) FROM vt_full WHERE int_val >= 10;",
      "SELECT count(*) FROM vt_full WHERE str_val LIKE '%val%';",
      "SELECT count(*) FROM vt_full WHERE int_val <> 99;",
      "SELECT count(*) FROM vt_full WHERE int_val IS NOT NULL;",
      "SELECT count(*) FROM vt_full WHERE null_val IS NULL;",
      "SELECT count(*) FROM vt_full WHERE \"SELECT\" = 'sel_val';",
      "SELECT count(*) FROM vt_full WHERE \"hyphen-col\" = 'hyphen_val';",
      "SELECT count(*) FROM vt_full WHERE str_val GLOB 'val*';",
      "SELECT count(*) FROM vt_full WHERE real_val = 1.23;",
      "SELECT count(*) FROM vt_full WHERE str_val = 'val_one';",
      "SELECT count(*) FROM vt_full WHERE str_val IS 'val_one';",
      "SELECT count(*) FROM vt_full WHERE str_val IS NOT 'val_two';",
  };

  for (const char* pszQuery : apszFilterQueries) {
    ASSERT_EQ(SQLITE_OK,
              sqlite3_prepare_v2(hDB_, pszQuery, -1, &hStmt, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
    EXPECT_GT(sqlite3_column_int(hStmt, 0), 0) << "Failed query: " << pszQuery;
    sqlite3_finalize(hStmt);
  }

  // Test MATCH, REGEXP, and unsupported BLOB filter data type
  sqlite3_stmt* hStmtExtra = nullptr;
  if (sqlite3_prepare_v2(
          hDB_, "SELECT count(*) FROM vt_full WHERE str_val MATCH 'val';", -1,
          &hStmtExtra, nullptr) == SQLITE_OK) {
    sqlite3_step(hStmtExtra);
    sqlite3_finalize(hStmtExtra);
  }
  if (sqlite3_prepare_v2(
          hDB_, "SELECT count(*) FROM vt_full WHERE str_val REGEXP 'val.*';",
          -1, &hStmtExtra, nullptr) == SQLITE_OK) {
    sqlite3_step(hStmtExtra);
    sqlite3_finalize(hStmtExtra);
  }
  EXPECT_NE(SQLITE_OK,
            sqlite3_exec(
                hDB_, "SELECT count(*) FROM vt_full WHERE int_val = X'0102';",
                nullptr, nullptr, nullptr));

  // 4. Prepared statement with bind NULL (exercises SQLITE_INDEX_CONSTRAINT_EQ
  // with SQLITE_NULL)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT count(*) FROM vt_full WHERE null_val = ?;", -1,
                &hStmt, nullptr));
  sqlite3_bind_null(hStmt, 1);
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 5. Self-join triggering OGR2SQLITE_Open with nMyRef > 0 (poDupDataSource)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT a.rowid, b.rowid FROM vt_full a JOIN vt_full b ON "
                "a.rowid = b.rowid ORDER BY a.rowid;",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int64(hStmt, 0));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(2, sqlite3_column_int64(hStmt, 0));
  EXPECT_EQ(SQLITE_DONE, sqlite3_step(hStmt));
  sqlite3_finalize(hStmt);

  // 6. DML: INSERT
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_exec(
          hDB_,
          "INSERT INTO vt_full (ogc_fid, int_val, real_val, str_val, date_val, "
          "time_val1, dt_val, bin_val, OGR_STYLE, GEOMETRY, OGR_NATIVE_DATA, "
          "OGR_NATIVE_MEDIA_TYPE) VALUES (3, 30, 3.14, 'str3', 'not_a_date', "
          "'11:22:33', '2026-05-10 11:22:33', X'010203', 'PEN(c:#0000FF)', "
          "ST_MakePoint(3.0, 3.0), '{\"test\":1}', 'text/plain');",
          nullptr, nullptr, nullptr));

  // Verify feature 3 inserted
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT count(*) FROM vt_full WHERE ogc_fid = 3;", -1,
                &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 7. DML: INSERT with rowid and ogc_fid mismatch -> returns error
  EXPECT_NE(
      SQLITE_OK,
      sqlite3_exec(hDB_,
                   "INSERT INTO vt_full (rowid, ogc_fid, int_val) VALUES (99, "
                   "100, 40);",
                   nullptr, nullptr, nullptr));

  // 8. DML: UPDATE
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_exec(hDB_,
                   "UPDATE vt_full SET str_val = 'modified' WHERE ogc_fid = 3;",
                   nullptr, nullptr, nullptr));

  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(hDB_, "SELECT str_val FROM vt_full WHERE ogc_fid = 3;",
                         -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_STREQ("modified",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 0)));
  sqlite3_finalize(hStmt);

  // 9. DML: UPDATE rowid -> unsupported
  EXPECT_NE(SQLITE_OK,
            sqlite3_exec(hDB_,
                         "UPDATE vt_full SET rowid = rowid + 10 WHERE "
                         "ogc_fid = 3;",
                         nullptr, nullptr, nullptr));

  // 10. DML: DELETE
  ASSERT_EQ(SQLITE_OK,
            sqlite3_exec(hDB_, "DELETE FROM vt_full WHERE ogc_fid = 3;",
                         nullptr, nullptr, nullptr));

  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT count(*) FROM vt_full WHERE ogc_fid = 3;", -1,
                &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 11. DDL: RENAME -> unsupported
  EXPECT_NE(SQLITE_OK,
            sqlite3_exec(hDB_, "ALTER TABLE vt_full RENAME TO vt_renamed;",
                         nullptr, nullptr, nullptr));
}

TEST_F(OGRSQLiteVirtualOGRTest, LayerMetadataSQLFunctions) {
  OGRSpatialReference oSRS;
  oSRS.importFromEPSG(4326);

  // Spatial layer 2D on poDS_
  OGRLayer* poLyr2D = poDS_->CreateLayer("ds_lyr_2d", &oSRS, wkbPoint);
  ASSERT_NE(nullptr, poLyr2D);
  OGRFeature* poF = new OGRFeature(poLyr2D->GetLayerDefn());
  OGRPoint pt(1.0, 2.0);
  poF->SetGeometry(&pt);
  ASSERT_EQ(OGRERR_NONE, poLyr2D->CreateFeature(poF));
  delete poF;

  // Spatial layer 3D Z on poDS_
  OGRLayer* poLyr3D = poDS_->CreateLayer("ds_lyr_3d", &oSRS, wkbPoint25D);
  ASSERT_NE(nullptr, poLyr3D);
  poF = new OGRFeature(poLyr3D->GetLayerDefn());
  OGRPoint pt3d(1.0, 2.0, 3.0);
  poF->SetGeometry(&pt3d);
  ASSERT_EQ(OGRERR_NONE, poLyr3D->CreateFeature(poF));
  delete poF;

  // Non-spatial layer on poDS_
  OGRLayer* poLyrNone = poDS_->CreateLayer("ds_lyr_none", nullptr, wkbNone);
  ASSERT_NE(nullptr, poLyrNone);
  poF = new OGRFeature(poLyrNone->GetLayerDefn());
  ASSERT_EQ(OGRERR_NONE, poLyrNone->CreateFeature(poF));
  delete poF;

  sqlite3_stmt* hStmt = nullptr;

  // 1. Error branches: invalid argument type (int instead of text) and unknown
  // virtual table
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT ogr_layer_Extent(123), ogr_layer_SRID(123), "
                "ogr_layer_GeometryType(123), ogr_layer_FeatureCount(123);",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 2));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 3));
  sqlite3_finalize(hStmt);

  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(hDB_, "SELECT ogr_layer_Extent(123);",
                                          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ogr_layer_Extent('unknown_vt');",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 2. ogr_layer_Extent, ogr_layer_SRID, ogr_layer_GeometryType,
  // ogr_layer_FeatureCount on 2D
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_,
                               "SELECT ogr_layer_Extent('ds_lyr_2d'), "
                               "ogr_layer_SRID('ds_lyr_2d'), "
                               "ogr_layer_GeometryType('ds_lyr_2d'), "
                               "ogr_layer_FeatureCount('ds_lyr_2d');",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(-1, sqlite3_column_int(hStmt, 1));
  EXPECT_STREQ("POINT",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 2)));
  EXPECT_EQ(1, sqlite3_column_int64(hStmt, 3));
  sqlite3_finalize(hStmt);

  // 3. ogr_layer_GeometryType on 3D layer -> returns "POINT Z"
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                           hDB_, "SELECT ogr_layer_GeometryType('ds_lyr_3d');",
                           -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_STREQ("POINT Z",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 0)));
  sqlite3_finalize(hStmt);

  // 4. Non-spatial layer -> Extent, SRID, GeometryType return NULL
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_,
                               "SELECT ogr_layer_Extent('ds_lyr_none'), "
                               "ogr_layer_SRID('ds_lyr_none'), "
                               "ogr_layer_GeometryType('ds_lyr_none');",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 2));
  sqlite3_finalize(hStmt);

  // 5. Ping dropped table in GetLayerForVTable
  ASSERT_EQ(SQLITE_OK,
            sqlite3_exec(hDB_,
                         CPLSPrintf("CREATE VIRTUAL TABLE vt_tmp USING "
                                    "VirtualOGR('%s', 0, 'ds_lyr_2d');",
                                    db_path_.c_str()),
                         nullptr, nullptr, nullptr));
  EXPECT_EQ(SQLITE_OK, sqlite3_exec(hDB_, "DROP TABLE vt_tmp;", nullptr,
                                    nullptr, nullptr));
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT ogr_layer_FeatureCount('vt_tmp');",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteVirtualOGRTest, InternalExecuteSQLConnectCreate) {
  // Test internal ExecuteSQL(..., "SQLITE") creation path
  GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDriver);
  auto poMemDS = std::unique_ptr<GDALDataset>(
      poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poMemDS);

  OGRSpatialReference oSRS;
  oSRS.importFromEPSG(4326);
  OGRLayer* poLyr = poMemDS->CreateLayer("mem_lyr", &oSRS, wkbPointZM);
  ASSERT_NE(nullptr, poLyr);

  // 1. Field domain triggers _BEGIN_DOMAIN_NAME_
  OGRFieldDefn fDom("dom_col", OFTString);
  fDom.SetDomainName("test_domain");
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fDom));

  // 2. Duplicate case-insensitive fields
  OGRFieldDefn fDup1("dup_name", OFTString);
  OGRFieldDefn fDup2("DUP_NAME", OFTString);
  OGRFieldDefn fDup3("dup_name2", OFTString);
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fDup1));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fDup2));
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fDup3));

  // 3. Field named OGR_STYLE and second geometry column
  OGRFieldDefn fStyle("OGR_STYLE", OFTString);
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateField(&fStyle));

  OGRGeomFieldDefn fGeom2("geom2", wkbPoint);
  fGeom2.SetSpatialRef(&oSRS);
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateGeomField(&fGeom2));

  OGRFeature* poF = new OGRFeature(poLyr->GetLayerDefn());
  poF->SetField("dom_col", "dom_val");
  poF->SetField("dup_name", "a");
  poF->SetField("DUP_NAME", "b");
  poF->SetField("dup_name2", "c");
  poF->SetField("OGR_STYLE", "style_val");

  OGRCircularString* poCS = new OGRCircularString();
  poCS->addPoint(0.0, 0.0);
  poCS->addPoint(1.0, 1.0);
  poCS->addPoint(2.0, 0.0);
  poF->SetGeomFieldDirectly(0, poCS);

  OGRPoint* poPt = new OGRPoint(1.0, 2.0, 3.0);
  poF->SetGeomFieldDirectly(1, poPt);

  ASSERT_EQ(OGRERR_NONE, poLyr->CreateFeature(poF));
  delete poF;

  // Feature with null geometries
  OGRFeature* poFNull = new OGRFeature(poLyr->GetLayerDefn());
  ASSERT_EQ(OGRERR_NONE, poLyr->CreateFeature(poFNull));
  delete poFNull;

  OGRLayer* poResult = poMemDS->ExecuteSQL(
      "SELECT GEOMETRY, geom2 FROM mem_lyr WHERE rowid = 0", nullptr, "SQLITE");
  ASSERT_NE(nullptr, poResult);
  OGRFeature* poResF = poResult->GetNextFeature();
  ASSERT_NE(nullptr, poResF);
  EXPECT_NE(nullptr, poResF->GetGeomFieldRef(0));
  EXPECT_NE(nullptr, poResF->GetGeomFieldRef(1));
  delete poResF;
  poMemDS->ReleaseResultSet(poResult);

  // Missing layer in GetLayerForVTable when poDS != poSQLiteDS (line 340)
  OGRLayer* poResSRID = poMemDS->ExecuteSQL(
      "SELECT ogr_layer_SRID('missing_layer') FROM mem_lyr", nullptr, "SQLITE");
  ASSERT_NE(nullptr, poResSRID);
  OGRFeature* poResFSRID = poResSRID->GetNextFeature();
  ASSERT_NE(nullptr, poResFSRID);
  EXPECT_TRUE(poResFSRID->IsFieldNull(0));
  delete poResFSRID;
  poMemDS->ReleaseResultSet(poResSRID);

  // 4. Second layer to test OGR_STYLE, OGR_NATIVE_DATA, OGR_NATIVE_MEDIA_TYPE
  OGRLayer* poLyr2 = poMemDS->CreateLayer("mem_lyr2", nullptr, wkbNone);
  ASSERT_NE(nullptr, poLyr2);
  OGRFieldDefn fText("text_col", OFTString);
  ASSERT_EQ(OGRERR_NONE, poLyr2->CreateField(&fText));
  OGRFeature* poF2 = new OGRFeature(poLyr2->GetLayerDefn());
  poF2->SetField("text_col", "some_text");
  poF2->SetStyleString("PEN(c:#FF0000)");
  poF2->SetNativeData("{\"k\":1}");
  poF2->SetNativeMediaType("application/json");
  ASSERT_EQ(OGRERR_NONE, poLyr2->CreateFeature(poF2));
  delete poF2;

  OGRLayer* poResult2 = poMemDS->ExecuteSQL(
      "SELECT text_col, OGR_STYLE FROM mem_lyr2", nullptr, "SQLITE");
  ASSERT_NE(nullptr, poResult2);
  OGRFeature* poResF2 = poResult2->GetNextFeature();
  ASSERT_NE(nullptr, poResF2);
  EXPECT_STREQ("some_text", poResF2->GetFieldAsString(0));
  EXPECT_STREQ("PEN(c:#FF0000)", poResF2->GetFieldAsString(1));
  delete poResF2;
  poMemDS->ReleaseResultSet(poResult2);
}

TEST_F(OGRSQLiteVirtualOGRTest, ExtensionInitAndStaticRegister) {
  g_pCapturedSqliteApi = nullptr;
  sqlite3_auto_extension(reinterpret_cast<void (*)()>(CaptureSqliteApi));
  absl::Cleanup cancel_ext = [] {
    sqlite3_cancel_auto_extension(
        reinterpret_cast<void (*)()>(CaptureSqliteApi));
    g_pCapturedSqliteApi = nullptr;
  };

  sqlite3* hDummy = nullptr;
  ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &hDummy));
  sqlite3_close(hDummy);

  ASSERT_NE(nullptr, g_pCapturedSqliteApi);

  // 1. Direct sqlite3_extension_init on already-loaded DB returns SQLITE_ERROR
  char* pzErrMsg = nullptr;
  EXPECT_EQ(SQLITE_ERROR,
            sqlite3_extension_init(hDB_, &pzErrMsg, g_pCapturedSqliteApi));
  EXPECT_NE(nullptr, pzErrMsg);
  sqlite3_free(pzErrMsg);

  // 2. Direct OGR2SQLITE_static_register on a bare memory DB
  sqlite3* hBareDB = nullptr;
  ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &hBareDB));
  char* pzErr = nullptr;
  EXPECT_EQ(SQLITE_OK,
            OGR2SQLITE_static_register(
                hBareDB, &pzErr, const_cast<void*>(g_pCapturedSqliteApi)));

  // With OGR_SQLITE_STATIC_VIRTUAL_OGR=NO
  CPLSetConfigOption("OGR_SQLITE_STATIC_VIRTUAL_OGR", "NO");
  EXPECT_EQ(SQLITE_OK,
            OGR2SQLITE_static_register(
                hBareDB, &pzErr, const_cast<void*>(g_pCapturedSqliteApi)));
  CPLSetConfigOption("OGR_SQLITE_STATIC_VIRTUAL_OGR", nullptr);

  sqlite3_close(hBareDB);

  // 3. OGR2SQLITE_Register
  OGR2SQLITE_Register();

  // 4. sqlite3_extension_init on a fresh DB opened without static virtual OGR
  CPLSetConfigOption("OGR_SQLITE_STATIC_VIRTUAL_OGR", "NO");
  sqlite3* hFreshDB = nullptr;
  ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &hFreshDB));
  CPLSetConfigOption("OGR_SQLITE_STATIC_VIRTUAL_OGR", nullptr);

  char* pzErrMsgFresh = nullptr;
  EXPECT_EQ(SQLITE_OK, sqlite3_extension_init(hFreshDB, &pzErrMsgFresh,
                                              g_pCapturedSqliteApi));
  sqlite3_free(pzErrMsgFresh);
  sqlite3_close(hFreshDB);
}

TEST_F(OGRSQLiteVirtualOGRTest, DirectModuleAndCustomLayerEdgeCases) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(nullptr, poDriver);
  std::string extra_db = std::string(kTestDir) + "/custom.db";
  auto poCustomDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(extra_db.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poCustomDS);
  sqlite3* hCustomDB =
      static_cast<sqlite3*>(poCustomDS->GetInternalHandle("SQLITE_HANDLE"));
  ASSERT_NE(nullptr, hCustomDB);

  auto poDummyDS = std::make_unique<DummyVogrDataset>();
  auto poLyr1 =
      std::make_unique<DummyVogrLayer>("dummy_lyr", /*bWithFIDCol=*/true,
                                       /*bFIDInFields=*/true,
                                       /*bFastCount=*/false);
  auto pRawLyr1 = poLyr1.get();
  poDummyDS->AddLayer(std::move(poLyr1));

  auto poLyrNoFID = std::make_unique<DummyVogrLayer>("dummy_nofid",
                                                     /*bWithFIDCol=*/false,
                                                     /*bFIDInFields=*/false,
                                                     /*bFastCount=*/false);
  poDummyDS->AddLayer(std::move(poLyrNoFID));

  OGR2SQLITEModule* poModule = OGR2SQLITE_Setup(
      poDummyDS.get(), static_cast<OGRSQLiteDataSource*>(poCustomDS.get()));
  ASSERT_NE(nullptr, poModule);

  OGR2SQLITE_SetCaseSensitiveLike(poModule, true);
  OGR2SQLITE_SetCaseSensitiveLike(poModule, false);

  auto poExtraDS = std::make_unique<DummyVogrDataset>();
  auto poExtraLyr =
      std::make_unique<DummyVogrLayer>("extra_lyr", true, false, false);
  poExtraDS->AddLayer(std::move(poExtraLyr));
  int nDSIdx = OGR2SQLITE_AddExtraDS(poModule, poExtraDS.release());
  EXPECT_EQ(0, nDSIdx);

  // 1. Invalid dataset index
  EXPECT_NE(SQLITE_OK, sqlite3_exec(hCustomDB,
                                    "CREATE VIRTUAL TABLE vt_err_idx USING "
                                    "VirtualOGR(99, 'extra_lyr', 0, 0);",
                                    nullptr, nullptr, nullptr));

  // 2. Layer not found
  EXPECT_NE(SQLITE_OK, sqlite3_exec(hCustomDB,
                                    "CREATE VIRTUAL TABLE vt_err_lyr USING "
                                    "VirtualOGR(0, 'missing', 0, 0);",
                                    nullptr, nullptr, nullptr));

  // 3. Connect to dummy_lyr (has FID in fields and fast count = false)
  ASSERT_EQ(SQLITE_OK, sqlite3_exec(hCustomDB,
                                    "CREATE VIRTUAL TABLE vt_dummy USING "
                                    "VirtualOGR(-1, 'dummy_lyr', 0, 0);",
                                    nullptr, nullptr, nullptr));

  // Test full table scan with fast feature count = false (hits Next, Eof,
  // Filter < 0)
  sqlite3_stmt* hStmt = nullptr;
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hCustomDB, "SELECT geom, str_col FROM vt_dummy;",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 0));
  EXPECT_STREQ("val0",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 1)));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  EXPECT_STREQ("val1",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 1)));
  EXPECT_EQ(SQLITE_DONE, sqlite3_step(hStmt));
  sqlite3_finalize(hStmt);

  // Insert feature with geometry into vt_dummy
  ASSERT_EQ(SQLITE_OK,
            sqlite3_exec(hCustomDB,
                         "INSERT INTO vt_dummy (custom_fid, str_col, geom) "
                         "VALUES (10, 'ins', MakePoint(1.0, 2.0));",
                         nullptr, nullptr, nullptr));

  // Test failing extent
  pRawLyr1->bFailExtent_ = true;
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                           hCustomDB, "SELECT ogr_layer_Extent('dummy_lyr');",
                           -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);
  pRawLyr1->bFailExtent_ = false;

  // Test failing attribute filter
  pRawLyr1->bFailAttrFilter_ = true;
  EXPECT_NE(
      SQLITE_OK,
      sqlite3_exec(hCustomDB,
                   "SELECT count(*) FROM vt_dummy WHERE str_col = 'val0';",
                   nullptr, nullptr, nullptr));
  pRawLyr1->bFailAttrFilter_ = false;

  // 4. Connect to dummy_nofid (no FID column, tests INSERT with rowid)
  ASSERT_EQ(SQLITE_OK, sqlite3_exec(hCustomDB,
                                    "CREATE VIRTUAL TABLE vt_nofid USING "
                                    "VirtualOGR(-1, 'dummy_nofid', 0, 0);",
                                    nullptr, nullptr, nullptr));
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_exec(
          hCustomDB,
          "INSERT INTO vt_nofid (rowid, str_col) VALUES (42, 'inserted');",
          nullptr, nullptr, nullptr));

  sqlite3_exec(hCustomDB, "DROP TABLE vt_dummy;", nullptr, nullptr, nullptr);
  sqlite3_exec(hCustomDB, "DROP TABLE vt_nofid;", nullptr, nullptr, nullptr);
}

// TODO: b/335317901 - Cover OGRSQLiteVirtualOGR dialect SQL escaping and
// multi-geom field VirtualOGR tables.

}  // namespace
}  // namespace autotest2
