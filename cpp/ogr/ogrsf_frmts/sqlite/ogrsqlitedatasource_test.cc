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
// Tests for OGRSQLiteDataSource and OGRSQLiteBaseDataSource
// (ogrsqlitedatasource.cpp).
//
// See also:
//   https://gdal.org/drivers/vector/sqlite.html
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqlitedatasource.cpp
//
// google3 build caveats:
//   - Compiled without HAVE_SPATIALITE or HAVE_RASTERLITE2;
//     ENABLE_SQL_SQLITE_FORMAT is gated on DEBUG / fuzzing builds in
//     ogr_sqlite.h:49 (disabled in google3 fastbuild/opt).

#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogr_sqlite.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogrsqlitebase.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/sqlite/sqlite3.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

template <typename Tag>
struct Result {
  using type = typename Tag::type;
  static type ptr;
};
template <typename Tag>
typename Result<Tag>::type Result<Tag>::ptr;

template <typename Tag, typename Tag::type p>
struct Rob : Result<Tag> {
  struct Filler {
    Filler() { Result<Tag>::ptr = p; }
  };
  static Filler filler;
};
template <typename Tag, typename Tag::type p>
typename Rob<Tag, p>::Filler Rob<Tag, p>::filler;

template <typename Tag>
typename Tag::type GetPrivate(Tag) {
  return Result<Tag>::ptr;
}

struct InitWithEPSGTag {
  using type = bool (OGRSQLiteDataSource::*)();
};
template struct Rob<InitWithEPSGTag, &OGRSQLiteDataSource::InitWithEPSG>;

struct SaveStatsTag {
  using type = void (OGRSQLiteDataSource::*)();
};
template struct Rob<SaveStatsTag, &OGRSQLiteDataSource::SaveStatistics>;

struct OpenVirtualTableTag {
  using type = bool (OGRSQLiteDataSource::*)(const char*, const char*);
};
template struct Rob<OpenVirtualTableTag,
                    &OGRSQLiteDataSource::OpenVirtualTable>;

struct GetSRTEXTColNameTag {
  using type = const char* (OGRSQLiteDataSource::*)();
};
template struct Rob<GetSRTEXTColNameTag,
                    &OGRSQLiteDataSource::GetSRTEXTColName>;

struct IsSpatialiteDBTag {
  using type = bool OGRSQLiteDataSource::*;
};
template struct Rob<IsSpatialiteDBTag, &OGRSQLiteDataSource::m_bIsSpatiaLiteDB>;

struct Spatialite4LayoutTag {
  using type = bool OGRSQLiteDataSource::*;
};
template struct Rob<Spatialite4LayoutTag,
                    &OGRSQLiteDataSource::m_bSpatialite4Layout>;

struct HaveGeomColsTag {
  using type = bool OGRSQLiteDataSource::*;
};
template struct Rob<HaveGeomColsTag,
                    &OGRSQLiteDataSource::m_bHaveGeometryColumns>;

struct SetSynchronousTag {
  using type = bool (OGRSQLiteBaseDataSource::*)();
};
template struct Rob<SetSynchronousTag,
                    &OGRSQLiteBaseDataSource::SetSynchronous>;

struct SetCacheSizeTag {
  using type = bool (OGRSQLiteBaseDataSource::*)();
};
template struct Rob<SetCacheSizeTag, &OGRSQLiteBaseDataSource::SetCacheSize>;

struct FinalFilenameTag {
  using type = std::string OGRSQLiteBaseDataSource::*;
};
template struct Rob<FinalFilenameTag,
                    &OGRSQLiteBaseDataSource::m_osFinalFilename>;

class ExposedGDALDataset : public GDALDataset {
 public:
  static void SetUpdate(GDALDataset* poDS) {
    static_cast<ExposedGDALDataset*>(poDS)->eAccess = GA_Update;
  }
};

class OgrSqliteDataSourceTest : public ::testing::Test {
 protected:
  static constexpr const char* kTestDir = "/vsimem/test_ogrsqlitedatasource";

  void SetUp() override {
    VSIMkdir(kTestDir, 0755);
    RegisterOGRSQLite();
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_VSIL_USE_TEMP_FILE_FOR_RANDOM_WRITE", nullptr);
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    CPLSetConfigOption("OGR_SQLITE_SYNCHRONOUS", nullptr);
    CPLSetConfigOption("OGR_SQLITE_CACHE", nullptr);
    CPLSetConfigOption("OGR_SQLITE_LOAD_EXTENSIONS", nullptr);
    CPLSetConfigOption("OGR_SQLITE_PRAGMA", nullptr);
    CPLSetConfigOption("OGR_SQLITE_JOURNAL", nullptr);
    CPLSetConfigOption("SQLITE_USE_URI", nullptr);
    CPLSetConfigOption("SQLITE_USE_OGR_VFS", nullptr);
    CPLSetConfigOption("OGR_SQLITE_DEFENSIVE", nullptr);
    CPLSetConfigOption("OGR_SQLITE_PERSIST_WAL", nullptr);
    CPLSetConfigOption("OGR_VFK_DB_READ", nullptr);
    CPLSetConfigOption("ALLOW_OGR_SQL_FUNCTIONS_FROM_TRIGGER_AND_VIEW",
                       nullptr);
    VSIRmdirRecursive(kTestDir);
  }
};

TEST_F(OgrSqliteDataSourceTest, ZlibFunctionsAndConfigAndOpenOptions) {
  const std::string dbPath = std::string(kTestDir) + "/options.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  CPLSetConfigOption("OGR_SQLITE_SYNCHRONOUS", "OFF");
  CPLSetConfigOption("OGR_SQLITE_CACHE", "2");
  CPLSetConfigOption("OGR_SQLITE_LOAD_EXTENSIONS",
                     "ENABLE_SQL_LOAD_EXTENSION,nonexistent_ext_xyz");
  CPLSetConfigOption(
      "OGR_SQLITE_PRAGMA",
      "page_size=4096,journal_mode=MEMORY,secure_delete=0,temp_store=2");

  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(poDS, nullptr);

  CPLSetConfigOption("OGR_SQLITE_SYNCHRONOUS", nullptr);
  CPLSetConfigOption("OGR_SQLITE_CACHE", nullptr);
  CPLSetConfigOption("OGR_SQLITE_LOAD_EXTENSIONS", nullptr);
  CPLSetConfigOption("OGR_SQLITE_PRAGMA", nullptr);

  auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
  ASSERT_NE(poSQLiteDS, nullptr);
  sqlite3* hDB = poSQLiteDS->GetDB();
  ASSERT_NE(hDB, nullptr);

  // 1. Test SetSynchronous values
  {
    for (const char* mode : {"0", "FALSE", "NORMAL", "1", "ON", "FULL", "2",
                             "TRUE", "INVALID_VAL"}) {
      CPLSetConfigOption("OGR_SQLITE_SYNCHRONOUS", mode);
      CPLPushErrorHandler(CPLQuietErrorHandler);
      (poSQLiteDS->*GetPrivate(SetSynchronousTag{}))();
      CPLPopErrorHandler();
    }
    CPLSetConfigOption("OGR_SQLITE_SYNCHRONOUS", nullptr);
  }

  // 3. Test SetCacheSize edge cases
  {
    CPLSetConfigOption("OGR_SQLITE_CACHE", "0");
    (poSQLiteDS->*GetPrivate(SetCacheSizeTag{}))();
    CPLSetConfigOption("OGR_SQLITE_CACHE", nullptr);
  }

  // 4. Test QueryLogger and Envelope methods
  {
    static bool bLoggerCalled = false;
    static std::string osLastLoggedSql;
    bLoggerCalled = false;
    poSQLiteDS->SetQueryLoggerFunc(
        [](const char* pszQuery, const char* pszError, int64_t, int64_t,
           void* poUserArg) {
          bLoggerCalled = true;
          if (pszQuery) {
            osLastLoggedSql = pszQuery;
          }
        },
        nullptr);

    sqlite3_exec(hDB, "SELECT 1", nullptr, nullptr, nullptr);
    EXPECT_TRUE(bLoggerCalled);

    // Trigger prepareSql failure logger
    sqlite3_stmt* hBadStmt = nullptr;
    poSQLiteDS->prepareSql(hDB, "INVALID SQL SYNTAX ===", -1, &hBadStmt,
                           nullptr);

    poSQLiteDS->SetQueryLoggerFunc(nullptr, nullptr);

    // Envelope cache
    OGREnvelope oEnv;
    oEnv.MinX = 10;
    oEnv.MaxX = 20;
    oEnv.MinY = 30;
    oEnv.MaxY = 40;
    poSQLiteDS->SetEnvelopeForSQL("SELECT * FROM dummy", oEnv);
    const OGREnvelope* pCachedEnv =
        poSQLiteDS->GetEnvelopeFromSQL("SELECT * FROM dummy");
    ASSERT_NE(pCachedEnv, nullptr);
    EXPECT_DOUBLE_EQ(pCachedEnv->MinX, 10);
    EXPECT_EQ(poSQLiteDS->GetEnvelopeFromSQL("SELECT * FROM nonexistent"),
              nullptr);

    // AbortSQL
    EXPECT_EQ(poSQLiteDS->AbortSQL(), OGRERR_NONE);

    // GetInternalHandle
    EXPECT_EQ(poSQLiteDS->GetInternalHandle("SQLITE_HANDLE"), hDB);
    EXPECT_EQ(poSQLiteDS->GetInternalHandle("OTHER"), nullptr);
    EXPECT_EQ(poSQLiteDS->GetInternalHandle(nullptr), nullptr);
  }

  // 5. Test close on read-only dataset
  poDS.reset();

  {
    std::unique_ptr<GDALDataset> poRO(
        GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(poRO, nullptr);
    poRO.reset();
  }

  // 6. Test PRELUDE_STATEMENTS open option
  {
    CPLStringList osPreludeOptions;
    osPreludeOptions.AddNameValue("PRELUDE_STATEMENTS",
                                  "CREATE TABLE prelude_tbl (id INT)");
    std::unique_ptr<GDALDataset> poWithPrelude(
        GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                          nullptr, osPreludeOptions.List(), nullptr));
    ASSERT_NE(poWithPrelude, nullptr);
    OGRLayer* poPreludeLayer = poWithPrelude->GetLayerByName("prelude_tbl");
    EXPECT_NE(poPreludeLayer, nullptr);

    // Invalid PRELUDE_STATEMENTS fails open
    CPLStringList osBadPrelude;
    osBadPrelude.AddNameValue("PRELUDE_STATEMENTS", "INVALID SQL SYNTAX ===");
    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poBadPrelude(
        GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                          nullptr, osBadPrelude.List(), nullptr));
    EXPECT_EQ(poBadPrelude, nullptr);
    CPLPopErrorHandler();
  }

  // 7. Test OGR_VFK_DB_READ=YES rejection
  {
    std::string vfkDbPath = std::string(kTestDir) + "/vfk.db";
    std::unique_ptr<GDALDataset> poVFK(
        poDriver->Create(vfkDbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poVFK, nullptr);
    auto* poVFKSqlite = dynamic_cast<OGRSQLiteDataSource*>(poVFK.get());
    ASSERT_NE(poVFKSqlite, nullptr);
    sqlite3_exec(poVFKSqlite->GetDB(), "CREATE TABLE vfk_tables (id INT)",
                 nullptr, nullptr, nullptr);
    poVFK.reset();

    CPLSetConfigOption("OGR_VFK_DB_READ", "YES");
    std::unique_ptr<GDALDataset> poVFKOpen(
        GDALDataset::Open(vfkDbPath.c_str(), GDAL_OF_VECTOR));
    EXPECT_EQ(poVFKOpen, nullptr);
    CPLSetConfigOption("OGR_VFK_DB_READ", nullptr);
  }

  // 8. Test security check for OGR SQL functions in trigger or view
  {
    std::string secDbPath = std::string(kTestDir) + "/security.db";
    std::unique_ptr<GDALDataset> poSec(
        poDriver->Create(secDbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poSec, nullptr);
    auto* poSecSqlite = dynamic_cast<OGRSQLiteDataSource*>(poSec.get());
    ASSERT_NE(poSecSqlite, nullptr);
    sqlite3_exec(
        poSecSqlite->GetDB(),
        "CREATE VIEW sec_view AS SELECT 1 WHERE 0 LIKE '%ogr_geocode%';",
        nullptr, nullptr, nullptr);
    poSec.reset();

    // Default configuration rejects open
    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poSecFail(
        GDALDataset::Open(secDbPath.c_str(), GDAL_OF_VECTOR));
    EXPECT_EQ(poSecFail, nullptr);
    CPLPopErrorHandler();

    // Allowed when config option is set
    CPLSetConfigOption("ALLOW_OGR_SQL_FUNCTIONS_FROM_TRIGGER_AND_VIEW", "YES");
    std::unique_ptr<GDALDataset> poSecOk(
        GDALDataset::Open(secDbPath.c_str(), GDAL_OF_VECTOR));
    EXPECT_NE(poSecOk, nullptr);
    CPLSetConfigOption("ALLOW_OGR_SQL_FUNCTIONS_FROM_TRIGGER_AND_VIEW",
                       nullptr);
  }

  // 9. Test long filename >= 505 chars triggering OGR VFS
  {
    std::string longPath = std::string(kTestDir) + "/";
    while (longPath.size() < 510) {
      longPath += "a_very_long_path_segment_";
    }
    longPath += ".db";
    std::unique_ptr<GDALDataset> poLong(
        poDriver->Create(longPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poLong, nullptr);
    OGRLayer* poL = poLong->CreateLayer("t1", nullptr, wkbNone, nullptr);
    EXPECT_NE(poL, nullptr);
  }
}

TEST_F(OgrSqliteDataSourceTest, DealWithOgrSchemaOpenOption) {
  const std::string dbPath = std::string(kTestDir) + "/schema.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  {
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer = poDS->CreateLayer("items", nullptr, wkbPoint, nullptr);
    ASSERT_NE(poLayer, nullptr);
    OGRFieldDefn fName("name", OFTString);
    EXPECT_EQ(poLayer->CreateField(&fName), OGRERR_NONE);
    OGRFieldDefn fAge("age", OFTInteger);
    EXPECT_EQ(poLayer->CreateField(&fAge), OGRERR_NONE);

    OGRFeature oF(poLayer->GetLayerDefn());
    oF.SetField("name", "Bob");
    oF.SetField("age", 30);
    EXPECT_EQ(poLayer->CreateFeature(&oF), OGRERR_NONE);
  }

  // 1. OGR_SCHEMA in update mode -> fails
  {
    CPLStringList osOptions;
    osOptions.AddNameValue("OGR_SCHEMA", "{}");
    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poDS(
        GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                          nullptr, osOptions.List(), nullptr));
    EXPECT_EQ(poDS, nullptr);
    CPLPopErrorHandler();
  }

  // 2. OGR_SCHEMA with invalid JSON -> fails
  {
    CPLStringList osOptions;
    osOptions.AddNameValue("OGR_SCHEMA", "{invalid_json");
    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poDS(
        GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, osOptions.List(), nullptr));
    EXPECT_EQ(poDS, nullptr);
    CPLPopErrorHandler();
  }

  // 3. OGR_SCHEMA with empty JSON -> fails validation
  {
    CPLStringList osOptions;
    osOptions.AddNameValue("OGR_SCHEMA", "{}");
    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poDS(
        GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, osOptions.List(), nullptr));
    EXPECT_EQ(poDS, nullptr);
    CPLPopErrorHandler();
  }

  // 4. Valid OGR_SCHEMA altering field age to age_str (OFTString) -> succeeds
  {
    CPLStringList osOptions;
    const char* pszSchema =
        R"({"layers":[{"name":"items","fields":[{"name":"age","newName":"age_str","type":"String"}]}]})";
    osOptions.AddNameValue("OGR_SCHEMA", pszSchema);
    std::unique_ptr<GDALDataset> poDS(
        GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, osOptions.List(), nullptr));
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer = poDS->GetLayerByName("items");
    ASSERT_NE(poLayer, nullptr);
    int idx = poLayer->GetLayerDefn()->GetFieldIndex("age_str");
    ASSERT_GE(idx, 0);
    EXPECT_EQ(poLayer->GetLayerDefn()->GetFieldDefn(idx)->GetType(), OFTString);
  }

  // 5. Valid JSON with non-existent layer/field -> DefaultApply fails
  {
    CPLStringList osOptions;
    const char* pszSchema =
        R"({"layers":[{"name":"nonexistent_layer","schemaType":"Patch","fields":[{"name":"bad","type":"Integer"}]}]})";
    osOptions.AddNameValue("OGR_SCHEMA", pszSchema);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poDS(
        GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, osOptions.List(), nullptr));
    EXPECT_EQ(poDS, nullptr);
    CPLPopErrorHandler();
  }
}

TEST_F(OgrSqliteDataSourceTest, RelationshipsLoadValidateAndAdd) {
  const std::string dbPath = std::string(kTestDir) + "/rel.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  {
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
    ASSERT_NE(poSQLiteDS, nullptr);
    sqlite3* hDB = poSQLiteDS->GetDB();

    ASSERT_EQ(
        sqlite3_exec(
            hDB,
            "CREATE TABLE parent (id INTEGER PRIMARY KEY, u_code TEXT UNIQUE, "
            "non_u TEXT);"
            "CREATE TABLE child1 (id INTEGER PRIMARY KEY, p_id INT REFERENCES "
            "parent(id), p_id2 INT REFERENCES parent(id) ON DELETE CASCADE);"
            "CREATE TABLE parent_multi (a INT, b INT, PRIMARY KEY(a, b));"
            "CREATE TABLE child_multi (id INTEGER PRIMARY KEY, a INT, b INT, "
            "FOREIGN KEY(a, b) REFERENCES parent_multi(a, b));"
            "CREATE TABLE child_target (id INTEGER PRIMARY KEY, p_code TEXT, "
            "p_id INT);"
            "CREATE TABLE child_target2 (id INTEGER PRIMARY KEY, p_code TEXT, "
            "p_id INT);"
            "CREATE VIEW view_parent AS SELECT * FROM parent;",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
  }

  CPLStringList osOpenOptions;
  osOpenOptions.AddNameValue("LIST_ALL_TABLES", "YES");
  std::unique_ptr<GDALDataset> poDS(
      GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                        nullptr, osOpenOptions.List(), nullptr));
  ASSERT_NE(poDS, nullptr);

  auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
  ASSERT_NE(poSQLiteDS, nullptr);

  // Test relationship loading and retrieval
  std::vector<std::string> names = poSQLiteDS->GetRelationshipNames();
  EXPECT_FALSE(names.empty());
  for (const auto& name : names) {
    const GDALRelationship* rel = poSQLiteDS->GetRelationship(name);
    ASSERT_NE(rel, nullptr);
    EXPECT_FALSE(rel->GetLeftTableName().empty());
    EXPECT_FALSE(rel->GetRightTableName().empty());
  }
  EXPECT_EQ(poSQLiteDS->GetRelationship("nonexistent_rel"), nullptr);

  // Test LoadRelationshipsFromForeignKeys with excludedTables
  poSQLiteDS->LoadRelationshipsFromForeignKeys({"child1"});

  // Test ValidateRelationship and AddRelationship error paths
  std::string failureReason;

  // 1. Read-only dataset
  poSQLiteDS->DisableUpdate();
  auto relRO = std::make_unique<GDALRelationship>(
      "r_ro", "parent", "child_target", GRC_ONE_TO_MANY);
  relRO->SetLeftTableFields({"id"});
  relRO->SetRightTableFields({"p_id"});
  EXPECT_FALSE(poSQLiteDS->AddRelationship(std::move(relRO), failureReason));
  ExposedGDALDataset::SetUpdate(poSQLiteDS);

  // 2. Cardinality != ONE_TO_MANY
  auto relCard = std::make_unique<GDALRelationship>(
      "r_card", "parent", "child_target", GRC_MANY_TO_MANY);
  EXPECT_FALSE(poSQLiteDS->ValidateRelationship(relCard.get(), failureReason));

  // 3. Type != COMPOSITE and != ASSOCIATION
  auto relType = std::make_unique<GDALRelationship>(
      "r_type", "parent", "child_target", GRC_ONE_TO_MANY);
  relType->SetType(GRT_AGGREGATION);
  EXPECT_FALSE(poSQLiteDS->ValidateRelationship(relType.get(), failureReason));

  // 4. Non-existent left table
  auto relNoLeft = std::make_unique<GDALRelationship>(
      "r_noleft", "bad_left", "child_target", GRC_ONE_TO_MANY);
  EXPECT_FALSE(
      poSQLiteDS->ValidateRelationship(relNoLeft.get(), failureReason));

  // 5. Non-existent right table
  auto relNoRight = std::make_unique<GDALRelationship>(
      "r_noright", "parent", "bad_right", GRC_ONE_TO_MANY);
  EXPECT_FALSE(
      poSQLiteDS->ValidateRelationship(relNoRight.get(), failureReason));

  // 6. Left table fields empty / > 1 / non-existent
  auto relEmptyLeft = std::make_unique<GDALRelationship>(
      "r_el", "parent", "child_target", GRC_ONE_TO_MANY);
  EXPECT_FALSE(
      poSQLiteDS->ValidateRelationship(relEmptyLeft.get(), failureReason));

  auto relMultiLeft = std::make_unique<GDALRelationship>(
      "r_ml", "parent", "child_target", GRC_ONE_TO_MANY);
  relMultiLeft->SetLeftTableFields({"id", "u_code"});
  EXPECT_FALSE(
      poSQLiteDS->ValidateRelationship(relMultiLeft.get(), failureReason));

  auto relBadLeftFld = std::make_unique<GDALRelationship>(
      "r_blf", "parent", "child_target", GRC_ONE_TO_MANY);
  relBadLeftFld->SetLeftTableFields({"nonexistent_field"});
  EXPECT_FALSE(
      poSQLiteDS->ValidateRelationship(relBadLeftFld.get(), failureReason));

  // 7. Right table fields empty / > 1 / non-existent
  auto relEmptyRight = std::make_unique<GDALRelationship>(
      "r_er", "parent", "child_target", GRC_ONE_TO_MANY);
  relEmptyRight->SetLeftTableFields({"id"});
  EXPECT_FALSE(
      poSQLiteDS->ValidateRelationship(relEmptyRight.get(), failureReason));

  auto relMultiRight = std::make_unique<GDALRelationship>(
      "r_mr", "parent", "child_target", GRC_ONE_TO_MANY);
  relMultiRight->SetLeftTableFields({"id"});
  relMultiRight->SetRightTableFields({"p_id", "p_code"});
  EXPECT_FALSE(
      poSQLiteDS->ValidateRelationship(relMultiRight.get(), failureReason));

  auto relBadRightFld = std::make_unique<GDALRelationship>(
      "r_brf", "parent", "child_target", GRC_ONE_TO_MANY);
  relBadRightFld->SetLeftTableFields({"id"});
  relBadRightFld->SetRightTableFields({"nonexistent_field"});
  EXPECT_FALSE(
      poSQLiteDS->ValidateRelationship(relBadRightFld.get(), failureReason));

  // 8. Base table field without unique constraint
  auto relNonUnique = std::make_unique<GDALRelationship>(
      "r_nu", "parent", "child_target", GRC_ONE_TO_MANY);
  relNonUnique->SetLeftTableFields({"non_u"});
  relNonUnique->SetRightTableFields({"p_code"});
  EXPECT_FALSE(
      poSQLiteDS->AddRelationship(std::move(relNonUnique), failureReason));

  // 9. Valid AddRelationship with GRT_ASSOCIATION
  auto relValidAssoc = std::make_unique<GDALRelationship>(
      "r_assoc", "parent", "child_target", GRC_ONE_TO_MANY);
  relValidAssoc->SetType(GRT_ASSOCIATION);
  relValidAssoc->SetLeftTableFields({"u_code"});
  relValidAssoc->SetRightTableFields({"p_code"});
  EXPECT_TRUE(
      poSQLiteDS->AddRelationship(std::move(relValidAssoc), failureReason));

  // Repopulate relationship map
  poSQLiteDS->GetRelationshipNames();

  // Duplicate relationship check
  auto relDup = std::make_unique<GDALRelationship>(
      "r_dup", "parent", "child_target", GRC_ONE_TO_MANY);
  relDup->SetLeftTableFields({"u_code"});
  relDup->SetRightTableFields({"p_code"});
  EXPECT_FALSE(poSQLiteDS->ValidateRelationship(relDup.get(), failureReason));

  // AddRelationship on same right table fails because index
  // idx_child_target_related_id already exists
  auto relDupIndex = std::make_unique<GDALRelationship>(
      "r_dup_idx", "parent", "child_target", GRC_ONE_TO_MANY);
  relDupIndex->SetType(GRT_COMPOSITE);
  relDupIndex->SetLeftTableFields({"u_code"});
  relDupIndex->SetRightTableFields({"p_code"});
  EXPECT_FALSE(
      poSQLiteDS->AddRelationship(std::move(relDupIndex), failureReason));

  // 10. Valid AddRelationship with GRT_COMPOSITE (CASCADE) on child_target2
  auto relValidComp = std::make_unique<GDALRelationship>(
      "r_comp", "parent", "child_target2", GRC_ONE_TO_MANY);
  relValidComp->SetType(GRT_COMPOSITE);
  relValidComp->SetLeftTableFields({"u_code"});
  relValidComp->SetRightTableFields({"p_code"});
  EXPECT_TRUE(
      poSQLiteDS->AddRelationship(std::move(relValidComp), failureReason));
}

TEST_F(OgrSqliteDataSourceTest, CreateTempFileAndInitWithEpsg) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  // 1. Create with SPATIALITE=YES fails cleanly when libspatialite is not
  // present
  {
    const std::string splPath = std::string(kTestDir) + "/fail_spl.db";
    CPLStringList osLCO;
    osLCO.AddNameValue("SPATIALITE", "YES");
    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poSpl(
        poDriver->Create(splPath.c_str(), 0, 0, 0, GDT_Unknown, osLCO.List()));
    EXPECT_EQ(poSpl, nullptr);
    CPLPopErrorHandler();
  }

  // 2. Create with CPL_VSIL_USE_TEMP_FILE_FOR_RANDOM_WRITE=FORCED
  {
    const std::string tmpPath = std::string(kTestDir) + "/temp_write.db";
    CPLSetConfigOption("CPL_VSIL_USE_TEMP_FILE_FOR_RANDOM_WRITE", "FORCED");
    CPLSetConfigOption("CPL_TMPDIR", kTestDir);
    std::unique_ptr<GDALDataset> poTempDS(
        poDriver->Create(tmpPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poTempDS, nullptr);
    OGRLayer* poL = poTempDS->CreateLayer("layer1", nullptr, wkbNone, nullptr);
    ASSERT_NE(poL, nullptr);
    poTempDS.reset();  // destructor copies temporary file onto tmpPath and
                       // unlinks temp file
    CPLSetConfigOption("CPL_VSIL_USE_TEMP_FILE_FOR_RANDOM_WRITE", nullptr);
    CPLSetConfigOption("CPL_TMPDIR", nullptr);

    VSIStatBufL sStat;
    EXPECT_EQ(VSIStatL(tmpPath.c_str(), &sStat), 0);
  }

  // 3. InitWithEPSG on Standard OGR DB
  {
    const std::string epsgDb = std::string(kTestDir) + "/epsg_std.db";
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(epsgDb.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
    ASSERT_NE(poSQLiteDS, nullptr);
    sqlite3* hDB = poSQLiteDS->GetDB();

    // Limit spatial_ref_sys to 1 row via unique constraint so InitWithEPSG
    // exercises both insert success and the rollback / failure path rapidly in
    // < 1 ms
    ASSERT_EQ(sqlite3_exec(hDB,
                           "CREATE UNIQUE INDEX idx_limit_epsg ON "
                           "spatial_ref_sys((srid > 0));",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);

    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE((poSQLiteDS->*GetPrivate(InitWithEPSGTag{}))());
    CPLPopErrorHandler();
  }

  // 4. InitWithEPSG on SpatiaLite 4 DB
  {
    const std::string epsgSp4Db = std::string(kTestDir) + "/epsg_sp4.db";
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(epsgSp4Db.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
    ASSERT_NE(poSQLiteDS, nullptr);
    sqlite3* hDB = poSQLiteDS->GetDB();

    ASSERT_EQ(
        sqlite3_exec(hDB,
                     "DROP TABLE spatial_ref_sys;"
                     "CREATE TABLE spatial_ref_sys ("
                     "  srid INTEGER UNIQUE, auth_name TEXT, auth_srid TEXT, "
                     "ref_sys_name TEXT, proj4text TEXT, srtext TEXT);"
                     "CREATE UNIQUE INDEX idx_limit_epsg ON "
                     "spatial_ref_sys((srid > 0));",
                     nullptr, nullptr, nullptr),
        SQLITE_OK);

    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = true;
    poSQLiteDS->*GetPrivate(Spatialite4LayoutTag{}) = true;

    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE((poSQLiteDS->*GetPrivate(InitWithEPSGTag{}))());
    CPLPopErrorHandler();
  }

  // 5. InitWithEPSG on SpatiaLite 2/3 DB (without srtext / srs_wkt)
  {
    const std::string epsgSp3Db = std::string(kTestDir) + "/epsg_sp3.db";
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(epsgSp3Db.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
    ASSERT_NE(poSQLiteDS, nullptr);
    sqlite3* hDB = poSQLiteDS->GetDB();

    ASSERT_EQ(sqlite3_exec(hDB,
                           "DROP TABLE spatial_ref_sys;"
                           "CREATE TABLE spatial_ref_sys ("
                           "  srid INTEGER UNIQUE, auth_name TEXT, auth_srid "
                           "TEXT, ref_sys_name TEXT, proj4text TEXT);"
                           "CREATE UNIQUE INDEX idx_limit_epsg ON "
                           "spatial_ref_sys((srid > 0));",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);

    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = true;
    poSQLiteDS->*GetPrivate(Spatialite4LayoutTag{}) = false;

    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE((poSQLiteDS->*GetPrivate(InitWithEPSGTag{}))());
    CPLPopErrorHandler();
  }
}

TEST_F(OgrSqliteDataSourceTest, OpenLayoutsSqlDumpsVirtualTablesAndViews) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  // 1. Valid SQL dump file opening
  {
    const std::string dumpPath = std::string(kTestDir) + "/dump.sql";
    const std::string content =
        "-- SQL SQLITE\n"
        "CREATE TABLE dump_tbl (id INT, name TEXT);\n"
        "INSERT INTO dump_tbl VALUES (1, 'val');\n"
        "-- comment line\n"
        "INVALID LINE THAT EMITS DEBUG;\n";
    VSILFILE* fp = VSIFOpenL(dumpPath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content.data(), 1, content.size(), fp);
    VSIFCloseL(fp);

    CPLStringList osOptions;
    osOptions.AddNameValue("LIST_ALL_TABLES", "YES");
    // ogr_sqlite.h:49 only defines ENABLE_SQL_SQLITE_FORMAT in DEBUG/fuzz
    // builds, so keep both expectations for fastbuild/opt vs dbg portability.
#ifdef ENABLE_SQL_SQLITE_FORMAT
    std::unique_ptr<GDALDataset> poDumpDS(GDALDataset::Open(
        dumpPath.c_str(), GDAL_OF_VECTOR, nullptr, osOptions.List(), nullptr));
    ASSERT_NE(poDumpDS, nullptr);
    OGRLayer* poDumpL = poDumpDS->GetLayerByName("dump_tbl");
    ASSERT_NE(poDumpL, nullptr);
    EXPECT_EQ(poDumpL->GetFeatureCount(TRUE), 1);
#else
    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poDumpDS(GDALDataset::Open(
        dumpPath.c_str(), GDAL_OF_VECTOR, nullptr, osOptions.List(), nullptr));
    EXPECT_EQ(poDumpDS, nullptr);
    CPLPopErrorHandler();
#endif
  }

#ifdef ENABLE_SQL_SQLITE_FORMAT
  // 2. Unsafe SQL dump line rejected
  {
    const std::string unsafePath = std::string(kTestDir) + "/unsafe.sql";
    const std::string unsafeContent =
        "-- SQL SQLITE\n"
        "SELECT LOAD_EXTENSION('malicious');\n";
    VSILFILE* fp = VSIFOpenL(unsafePath.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(unsafeContent.data(), 1, unsafeContent.size(), fp);
    VSIFCloseL(fp);

    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poUnsafeDS(
        GDALDataset::Open(unsafePath.c_str(), GDAL_OF_VECTOR));
    EXPECT_EQ(poUnsafeDS, nullptr);
    CPLPopErrorHandler();
  }
#endif

  // 3. SQLITE: prefix
  {
    const std::string prefixDb = std::string(kTestDir) + "/prefix.db";
    std::unique_ptr<GDALDataset> poInit(
        poDriver->Create(prefixDb.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poInit, nullptr);
    OGRLayer* poL = poInit->CreateLayer("p_layer", nullptr, wkbNone, nullptr);
    ASSERT_NE(poL, nullptr);
    poInit.reset();

    std::string prefixedPath = "SQLITE:" + prefixDb;
    std::unique_ptr<GDALDataset> poPrefixed(
        GDALDataset::Open(prefixedPath.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(poPrefixed, nullptr);
    EXPECT_NE(poPrefixed->GetLayerByName("p_layer"), nullptr);
  }

  // 4. SpatiaLite 4 layout with raster_coverages, views_geometry_columns, and
  // SaveStatistics
  {
    const std::string sp4Db = std::string(kTestDir) + "/open_sp4.db";
    CPLStringList osDSCO;
    osDSCO.AddNameValue("METADATA", "NO");
    std::unique_ptr<GDALDataset> poInit(
        poDriver->Create(sp4Db.c_str(), 0, 0, 0, GDT_Unknown, osDSCO.List()));
    ASSERT_NE(poInit, nullptr);
    auto* poSQLiteInit = dynamic_cast<OGRSQLiteDataSource*>(poInit.get());
    ASSERT_NE(poSQLiteInit, nullptr);
    sqlite3* hDB = poSQLiteInit->GetDB();

    ASSERT_EQ(
        sqlite3_exec(
            hDB,
            "CREATE TABLE spatial_ref_sys (srid INTEGER UNIQUE, auth_name "
            "TEXT, auth_srid TEXT, ref_sys_name TEXT, proj4text TEXT, srtext "
            "TEXT);"
            "CREATE TABLE geometry_columns (f_table_name TEXT, "
            "f_geometry_column TEXT, geometry_type INTEGER, coord_dimension "
            "INTEGER, srid INTEGER, spatial_index_enabled INTEGER);"
            "CREATE TABLE raster_coverages (coverage_name TEXT);"
            "INSERT INTO raster_coverages VALUES ('cov1');"
            "CREATE TABLE cov1_sections (id INT);"
            "CREATE TABLE cov1_tiles (id INT);"
            "CREATE TABLE sp_data (ogc_fid INTEGER PRIMARY KEY, geom BLOB, val "
            "INT);"
            "INSERT INTO geometry_columns VALUES ('sp_data', 'geom', 1, 2, "
            "4326, 0);"
            "CREATE VIEW sp_view AS SELECT ogc_fid, geom, val FROM sp_data;"
            "CREATE TABLE views_geometry_columns (view_name TEXT, "
            "view_geometry TEXT, view_rowid TEXT, f_table_name TEXT, "
            "f_geometry_column TEXT);"
            "INSERT INTO views_geometry_columns VALUES ('sp_view', 'geom', "
            "'ogc_fid', 'sp_data', 'geom');"
            "CREATE TABLE spatialite_history (event_id INTEGER PRIMARY KEY "
            "AUTOINCREMENT, table_name TEXT, geometry_column TEXT, event TEXT, "
            "timestamp TEXT, ver_sqlite TEXT, ver_splite TEXT);"
            "INSERT INTO spatialite_history VALUES (1, 'ALL-TABLES', "
            "'ALL-GEOMETRY-COLUMNS', 'UpdateLayerStatistics', '2026-01-01', "
            "'3.0', '4.0');",
            nullptr, nullptr, nullptr),
        SQLITE_OK);

    poInit.reset();

    // Opening SpatiaLite in update mode without libspatialite fails
    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poFailUpdate(
        GDALDataset::Open(sp4Db.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    EXPECT_EQ(poFailUpdate, nullptr);
    CPLPopErrorHandler();

    // Opening in read-only mode succeeds
    std::unique_ptr<GDALDataset> poSp4RO(
        GDALDataset::Open(sp4Db.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(poSp4RO, nullptr);

    auto* poSp4DS = dynamic_cast<OGRSQLiteDataSource*>(poSp4RO.get());
    ASSERT_NE(poSp4DS, nullptr);
    EXPECT_TRUE(poSp4DS->IsSpatialiteDB());
    EXPECT_TRUE(poSp4DS->HasSpatialite4Layout());
    EXPECT_NE(poSp4DS->GetLayerByName("sp_data"), nullptr);
    EXPECT_NE(poSp4DS->GetLayerByName("sp_view"), nullptr);

    // Call SaveStatistics via private member accessor
    ExposedGDALDataset::SetUpdate(poSp4DS);
    (poSp4DS->*GetPrivate(SaveStatsTag{}))();

    // Direct call to OpenVirtualTable
    (poSp4DS->*GetPrivate(OpenVirtualTableTag{}))(
        "v_shape",
        "CREATE VIRTUAL TABLE v_shape USING VirtualShape('shp', 'UTF-8', "
        "4326)");
  }
}

TEST_F(OgrSqliteDataSourceTest, LayersExecuteSqlTransactionsAndSrsManagement) {
  const std::string dbPath = std::string(kTestDir) + "/mgmt.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(poDS, nullptr);

  auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
  ASSERT_NE(poSQLiteDS, nullptr);

  // 1. ICreateLayer option variations
  {
    // Invalid FORMAT
    CPLStringList osBadFormat;
    osBadFormat.AddNameValue("FORMAT", "BOGUS");
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(
        poDS->CreateLayer("bad_fmt", nullptr, wkbPoint, osBadFormat.List()),
        nullptr);
    CPLPopErrorHandler();

    // FORMAT=WKT, FORMAT=WKB, custom FID and GEOMETRY_NAME, LAUNDER=NO
    CPLStringList osWktOptions;
    osWktOptions.AddNameValue("FORMAT", "WKT");
    osWktOptions.AddNameValue("FID", "custom_id");
    osWktOptions.AddNameValue("GEOMETRY_NAME", "custom_geom");
    osWktOptions.AddNameValue("LAUNDER", "NO");
    OGRLayer* poWktL =
        poDS->CreateLayer("wkt_layer", nullptr, wkbPoint, osWktOptions.List());
    ASSERT_NE(poWktL, nullptr);
    EXPECT_STREQ(poWktL->GetFIDColumn(), "custom_id");
    EXPECT_STREQ(poWktL->GetGeometryColumn(), "custom_geom");

    // Overwrite without OVERWRITE=YES fails
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poDS->CreateLayer("wkt_layer", nullptr, wkbPoint, nullptr),
              nullptr);
    CPLPopErrorHandler();

    // Overwrite with OVERWRITE=YES succeeds
    CPLStringList osOverwrite;
    osOverwrite.AddNameValue("OVERWRITE", "YES");
    EXPECT_NE(
        poDS->CreateLayer("wkt_layer", nullptr, wkbPoint, osOverwrite.List()),
        nullptr);

    // Layer with SRID=4326 option
    CPLStringList osSridOptions;
    osSridOptions.AddNameValue("SRID", "4326");
    OGRLayer* poSridL = poDS->CreateLayer("srid_layer", nullptr, wkbPoint,
                                          osSridOptions.List());
    ASSERT_NE(poSridL, nullptr);

    // Layer with strict, compress geom, and compress columns
    CPLStringList osAdvOptions;
    osAdvOptions.AddNameValue("COMPRESS_GEOM", "YES");
    osAdvOptions.AddNameValue("COMPRESS_COLUMNS", "txt_col");
    osAdvOptions.AddNameValue("STRICT", "YES");
    OGRLayer* poAdvL =
        poDS->CreateLayer("adv_layer", nullptr, wkbPoint, osAdvOptions.List());
    ASSERT_NE(poAdvL, nullptr);
  }

  // 2. DeleteLayer
  {
    EXPECT_EQ(poDS->DeleteLayer(-1), OGRERR_FAILURE);
    EXPECT_EQ(poDS->DeleteLayer(999), OGRERR_FAILURE);

    int idx = -1;
    for (int i = 0; i < poDS->GetLayerCount(); ++i) {
      if (strcmp(poDS->GetLayer(i)->GetName(), "srid_layer") == 0) {
        idx = i;
        break;
      }
    }
    ASSERT_GE(idx, 0);
    EXPECT_EQ(poDS->DeleteLayer(idx), OGRERR_NONE);
  }

  // 3. GetLayer, GetLayerByName, IsLayerPrivate, GetLayerByNameNotVisible
  {
    EXPECT_EQ(poDS->GetLayer(-1), nullptr);
    EXPECT_EQ(poDS->GetLayer(999), nullptr);

    OGRLayer* poFound = poDS->GetLayerByName("wkt_layer");
    ASSERT_NE(poFound, nullptr);
    EXPECT_EQ(poDS->GetLayerByName("nonexistent_layer"), nullptr);

    // IsLayerPrivate
    EXPECT_FALSE(poSQLiteDS->IsLayerPrivate(0));
    EXPECT_FALSE(poSQLiteDS->IsLayerPrivate(-1));

    // GetLayerByNameNotVisible: visible layer, invisible table, cached
    // invisible, and non-existent
    EXPECT_NE(poSQLiteDS->GetLayerByNameNotVisible("wkt_layer"), nullptr);
    EXPECT_NE(poSQLiteDS->GetLayerByNameNotVisible("geometry_columns"),
              nullptr);
    EXPECT_NE(poSQLiteDS->GetLayerByNameNotVisible("geometry_columns"),
              nullptr);
    EXPECT_EQ(poSQLiteDS->GetLayerByNameNotVisible("completely_bogus_tbl"),
              nullptr);

    // GetLayerWithGetSpatialWhereByName
    auto pair = poSQLiteDS->GetLayerWithGetSpatialWhereByName("wkt_layer");
    EXPECT_NE(pair.first, nullptr);

    // LaunderName
    char* pszLaundered =
        OGRSQLiteDataSource::LaunderName("Col-Name'With#Chars");
    EXPECT_STREQ(pszLaundered, "col_name_with_chars");
    CPLFree(pszLaundered);
  }

  // 4. TestCapabilities
  {
    EXPECT_TRUE(poDS->TestCapability(ODsCCreateLayer));
    EXPECT_TRUE(poDS->TestCapability(ODsCDeleteLayer));
    EXPECT_TRUE(poDS->TestCapability(ODsCCreateGeomFieldAfterCreateLayer));
    EXPECT_TRUE(poDS->TestCapability(ODsCRandomLayerWrite));
    EXPECT_TRUE(poDS->TestCapability(GDsCAddRelationship));
    EXPECT_TRUE(poDS->TestCapability(ODsCCurveGeometries));
    EXPECT_TRUE(poDS->TestCapability(ODsCMeasuredGeometries));
    EXPECT_TRUE(poDS->TestCapability(ODsCTransactions));
    EXPECT_TRUE(poDS->TestCapability(ODsCZGeometries));
    EXPECT_FALSE(poDS->TestCapability("BogusCapability"));
  }

  // 5. Transactions and Savepoints
  {
    // Standard transaction
    EXPECT_EQ(poDS->StartTransaction(), OGRERR_NONE);
    EXPECT_EQ(poDS->StartTransaction(), OGRERR_FAILURE);  // already started
    EXPECT_EQ(poDS->CommitTransaction(), OGRERR_NONE);
    EXPECT_EQ(poDS->CommitTransaction(), OGRERR_FAILURE);  // not established

    EXPECT_EQ(poDS->StartTransaction(), OGRERR_NONE);
    EXPECT_EQ(poDS->RollbackTransaction(), OGRERR_NONE);
    EXPECT_EQ(poDS->RollbackTransaction(), OGRERR_FAILURE);  // not established

    // Savepoints
    EXPECT_EQ(poSQLiteDS->StartSavepoint("sp1"), OGRERR_NONE);
    EXPECT_EQ(poSQLiteDS->StartSavepoint("sp2"), OGRERR_NONE);
    EXPECT_EQ(poSQLiteDS->RollbackToSavepoint("sp2"), OGRERR_NONE);
    EXPECT_EQ(poSQLiteDS->RollbackToSavepoint("nonexistent_sp"),
              OGRERR_FAILURE);
    EXPECT_EQ(poSQLiteDS->ReleaseSavepoint("sp1"), OGRERR_NONE);
    EXPECT_EQ(poSQLiteDS->ReleaseSavepoint("nonexistent_sp"), OGRERR_FAILURE);

    // SoftRollbackTransaction with active savepoint
    EXPECT_EQ(poSQLiteDS->StartSavepoint("sp_to_abort"), OGRERR_NONE);
    EXPECT_EQ(poSQLiteDS->SoftRollbackTransaction(), OGRERR_NONE);

    // PragmaCheck
    EXPECT_EQ(poSQLiteDS->PragmaCheck("page_size", "4096", 1), OGRERR_NONE);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poSQLiteDS->PragmaCheck("page_size", "9999", 1), OGRERR_FAILURE);
    EXPECT_EQ(poSQLiteDS->PragmaCheck("page_size", "4096", 99), OGRERR_FAILURE);
    EXPECT_EQ(poSQLiteDS->PragmaCheck("invalid_pragma ===", "4096", 1),
              OGRERR_FAILURE);
    CPLPopErrorHandler();
  }

  // 6. ExecuteSQL and ReleaseResultSet
  {
    // PRAGMA case_sensitive_like
    EXPECT_EQ(
        poDS->ExecuteSQL("PRAGMA case_sensitive_like = 0", nullptr, nullptr),
        nullptr);
    EXPECT_EQ(
        poDS->ExecuteSQL("PRAGMA case_sensitive_like = 1", nullptr, nullptr),
        nullptr);

    // DELLAYER
    EXPECT_EQ(poDS->ExecuteSQL("DELLAYER: adv_layer", nullptr, nullptr),
              nullptr);
    EXPECT_EQ(poDS->GetLayerByName("adv_layer"), nullptr);

    // SQLITE_HAS_COLUMN_METADATA()
    OGRLayer* poMetaL =
        poDS->ExecuteSQL("SQLITE_HAS_COLUMN_METADATA()", nullptr, nullptr);
    ASSERT_NE(poMetaL, nullptr);
    poDS->ReleaseResultSet(poMetaL);

    // VACUUM
    EXPECT_EQ(poDS->ExecuteSQL("VACUUM", nullptr, nullptr), nullptr);

    // ProcessTransactionSQL via ExecuteSQL
    EXPECT_EQ(poDS->ExecuteSQL("BEGIN", nullptr, nullptr), nullptr);
    EXPECT_EQ(poDS->ExecuteSQL("SAVEPOINT sp_sql", nullptr, nullptr), nullptr);
    EXPECT_EQ(
        poDS->ExecuteSQL("ROLLBACK TO SAVEPOINT sp_sql", nullptr, nullptr),
        nullptr);
    EXPECT_EQ(poDS->ExecuteSQL("RELEASE SAVEPOINT sp_sql", nullptr, nullptr),
              nullptr);
    EXPECT_EQ(poDS->ExecuteSQL("COMMIT", nullptr, nullptr), nullptr);

    // Side-effects function simulation
    sqlite3_create_function(
        poSQLiteDS->GetDB(), "UpdateLayerStatistics", 0, SQLITE_UTF8, nullptr,
        [](sqlite3_context* ctx, int, sqlite3_value**) {
          sqlite3_result_int(ctx, 1);
        },
        nullptr, nullptr);
    OGRLayer* poSideL =
        poDS->ExecuteSQL("SELECT UpdateLayerStatistics()", nullptr, nullptr);
    ASSERT_NE(poSideL, nullptr);
    poDS->ReleaseResultSet(poSideL);

    // SELECT with ORDER BY and spatial filter
    OGRLayer* poSelectL = poDS->ExecuteSQL(
        "SELECT * FROM wkt_layer ORDER BY custom_id", nullptr, nullptr);
    ASSERT_NE(poSelectL, nullptr);
    poDS->ReleaseResultSet(poSelectL);

    // Dialect OGRSQL and INDIRECT_SQLITE
    OGRLayer* poOgrSql =
        poDS->ExecuteSQL("SELECT * FROM wkt_layer", nullptr, "OGRSQL");
    ASSERT_NE(poOgrSql, nullptr);
    poDS->ReleaseResultSet(poOgrSql);

    OGRLayer* poIndirect =
        poDS->ExecuteSQL("SELECT * FROM wkt_layer", nullptr, "INDIRECT_SQLITE");
    ASSERT_NE(poIndirect, nullptr);
    poDS->ReleaseResultSet(poIndirect);

    // Invalid SQL
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poDS->ExecuteSQL("INVALID SQL STATEMENT ===", nullptr, nullptr),
              nullptr);
    CPLPopErrorHandler();
  }

  // 7. SRS Management: FetchSRSId and FetchSRS
  {
    EXPECT_EQ(poSQLiteDS->FetchSRSId(nullptr), poSQLiteDS->GetUndefinedSRID());

    OGRSpatialReference oSRS4326;
    oSRS4326.importFromEPSG(4326);
    oSRS4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    int srid4326 = poSQLiteDS->FetchSRSId(&oSRS4326);
    EXPECT_EQ(srid4326, 4326);

    // Cached lookup
    EXPECT_EQ(poSQLiteDS->FetchSRSId(&oSRS4326), 4326);

    // FetchSRS
    EXPECT_EQ(poSQLiteDS->FetchSRS(0), nullptr);
    EXPECT_EQ(poSQLiteDS->FetchSRS(-1), nullptr);
    OGRSpatialReference* poSRS = poSQLiteDS->FetchSRS(4326);
    ASSERT_NE(poSRS, nullptr);
    EXPECT_TRUE(poSRS->IsSame(&oSRS4326));

    // Custom SRS without authority (exercises MAX(srid) + 1 insertion)
    OGRSpatialReference oCustomSRS;
    oCustomSRS.importFromWkt(
        "GEOGCS[\"MyCustomDatum\",DATUM[\"Custom\",SPHEROID[\"Custom\",6378137,"
        "298.257]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]"
        "]");
    oCustomSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    int customSRID = poSQLiteDS->FetchSRSId(&oCustomSRS);
    EXPECT_GT(customSRID, 0);
    OGRSpatialReference* poCustomFetched = poSQLiteDS->FetchSRS(customSRID);
    ASSERT_NE(poCustomFetched, nullptr);

    // FetchSRSId with authority code on SpatiaLite layout
    ASSERT_EQ(
        sqlite3_exec(poSQLiteDS->GetDB(),
                     "ALTER TABLE spatial_ref_sys ADD COLUMN ref_sys_name TEXT;"
                     "ALTER TABLE spatial_ref_sys ADD COLUMN proj4text TEXT;",
                     nullptr, nullptr, nullptr),
        SQLITE_OK);
    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = true;
    OGRSpatialReference oProjSRS;
    oProjSRS.importFromEPSG(32632);
    oProjSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    int sridProj = poSQLiteDS->FetchSRSId(&oProjSRS);
    EXPECT_EQ(sridProj, 32632);

    // Custom SRS without authority on SpatiaLite (exercises 'OGR' fake
    // authority insertion)
    OGRSpatialReference oSplCustom;
    oSplCustom.importFromWkt(
        "GEOGCS[\"SplCustom\",DATUM[\"Custom\",SPHEROID[\"Custom\",6378137,298."
        "257]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]");
    oSplCustom.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    int sridSplCustom = poSQLiteDS->FetchSRSId(&oSplCustom);
    EXPECT_GT(sridSplCustom, 0);

    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = false;
  }

  // 8. FlushCache and Close
  EXPECT_EQ(poDS->FlushCache(), CE_None);
  EXPECT_EQ(poDS->Close(), CE_None);
}

TEST_F(OgrSqliteDataSourceTest, ExtendedOpenOptionsAndUriAndWal) {
  const std::string dbPath = std::string(kTestDir) + "/ext_options.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  // 1. Create DB and verify defensive and persistent WAL options
  {
    CPLSetConfigOption("OGR_SQLITE_DEFENSIVE", "YES");
    CPLSetConfigOption("OGR_SQLITE_PERSIST_WAL", "NO");
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    CPLSetConfigOption("OGR_SQLITE_DEFENSIVE", nullptr);
    CPLSetConfigOption("OGR_SQLITE_PERSIST_WAL", nullptr);
  }

  // 2. Open with URI options: NOLOCK, IMMUTABLE, CACHE=PRIVATE, CACHE=SHARED
  {
    CPLSetConfigOption("SQLITE_USE_URI", "YES");
    for (const char* cacheVal : {"PRIVATE", "SHARED"}) {
      CPLStringList osOptions;
      osOptions.AddNameValue("NOLOCK", "YES");
      osOptions.AddNameValue("IMMUTABLE", "YES");
      osOptions.AddNameValue("CACHE", cacheVal);
      std::unique_ptr<GDALDataset> poDS(
          GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                            nullptr, osOptions.List(), nullptr));
      EXPECT_NE(poDS, nullptr);
    }

    // NOLOCK only (without IMMUTABLE)
    {
      CPLStringList osOptions;
      osOptions.AddNameValue("NOLOCK", "YES");
      std::unique_ptr<GDALDataset> poDS(
          GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                            nullptr, osOptions.List(), nullptr));
      EXPECT_NE(poDS, nullptr);
    }
    CPLSetConfigOption("SQLITE_USE_URI", nullptr);
  }

  // 3. Journal modes: DELETE, MEMORY, OFF, and WAL with CloseDB cleanup
  {
    for (const char* jmode : {"DELETE", "MEMORY", "OFF"}) {
      CPLSetConfigOption("OGR_SQLITE_JOURNAL", jmode);
      std::unique_ptr<GDALDataset> poDS(
          GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                            nullptr, nullptr, nullptr));
      EXPECT_NE(poDS, nullptr);
    }
    CPLSetConfigOption("OGR_SQLITE_JOURNAL", nullptr);

    // WAL mode: create table, insert feature, then close to exercise CloseDB()
    // WAL cleanup
    const std::string walDbPath = std::string(kTestDir) + "/wal_cleanup.db";
    CPLSetConfigOption("OGR_SQLITE_JOURNAL", "WAL");
    {
      std::unique_ptr<GDALDataset> poDS(
          poDriver->Create(walDbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(poDS, nullptr);
      OGRLayer* poL = poDS->CreateLayer("tbl", nullptr, wkbNone, nullptr);
      ASSERT_NE(poL, nullptr);
      OGRFeature oF(poL->GetLayerDefn());
      EXPECT_EQ(poL->CreateFeature(&oF), OGRERR_NONE);
    }
    CPLSetConfigOption("OGR_SQLITE_JOURNAL", nullptr);
  }

  // 4. OGR_VFK_DB_READ option (lines 1545-1552)
  {
    const std::string vfkPath = std::string(kTestDir) + "/vfk.db";
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(vfkPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poL = poDS->CreateLayer("vfk_tables", nullptr, wkbNone, nullptr);
    ASSERT_NE(poL, nullptr);
    poDS.reset();

    CPLSetConfigOption("OGR_VFK_DB_READ", "YES");
    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poVFK(
        GDALDataset::Open(vfkPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    EXPECT_EQ(poVFK, nullptr);
    CPLPopErrorHandler();
    CPLSetConfigOption("OGR_VFK_DB_READ", nullptr);
  }

  // 5. Corrupt SQLite header and corrupt WAL detection (lines 1569-1625)
  {
    // Synthesize corrupt standard database: byte 18=1, corrupt interior page
    const std::string corruptStd = std::string(kTestDir) + "/corrupt_std.db";
    std::vector<uint8_t> stdBytes(4096, 0);
    const char magic[] = "SQLite format 3";
    std::memcpy(stdBytes.data(), magic, sizeof(magic));
    stdBytes[16] = 0x10;   // 4096 page size
    stdBytes[18] = 1;      // read version
    stdBytes[19] = 1;      // write version
    stdBytes[100] = 0xFF;  // corrupt page type
    VSILFILE* fp = VSIFOpenL(corruptStd.c_str(), "wb");
    if (fp) {
      VSIFWriteL(stdBytes.data(), 1, stdBytes.size(), fp);
      VSIFCloseL(fp);
    }
    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poCorruptStd(GDALDataset::Open(
        corruptStd.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    EXPECT_EQ(poCorruptStd, nullptr);
    CPLPopErrorHandler();

    // Synthesize corrupt WAL database: byte 18=2 (WAL mode), triggers retry
    // with IMMUTABLE=YES
    const std::string corruptWal = std::string(kTestDir) + "/corrupt_wal.db";
    std::vector<uint8_t> walBytes = stdBytes;
    walBytes[18] = 2;  // WAL read version
    walBytes[19] = 2;  // WAL write version
    fp = VSIFOpenL(corruptWal.c_str(), "wb");
    if (fp) {
      VSIFWriteL(walBytes.data(), 1, walBytes.size(), fp);
      VSIFCloseL(fp);
    }
    CPLPushErrorHandler(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poCorruptWal(GDALDataset::Open(
        corruptWal.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    EXPECT_EQ(poCorruptWal, nullptr);
    CPLPopErrorHandler();
  }
}

TEST_F(OgrSqliteDataSourceTest, ExtendedSrsManagement) {
  const std::string dbPath = std::string(kTestDir) + "/srs_ext.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(poDS, nullptr);
  auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
  ASSERT_NE(poSQLiteDS, nullptr);
  sqlite3* hDB = poSQLiteDS->GetDB();
  ASSERT_NE(hDB, nullptr);

  // 1. Same pointer cache lookup and IsSame cache lookup
  OGRSpatialReference oSRS4326;
  oSRS4326.importFromEPSG(4326);
  oSRS4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_EQ(poSQLiteDS->FetchSRSId(&oSRS4326), 4326);
  // Pointer equality in cache
  EXPECT_EQ(poSQLiteDS->FetchSRSId(&oSRS4326), 4326);
  // IsSame equality in cache
  OGRSpatialReference oSRS4326Clone(oSRS4326);
  EXPECT_EQ(poSQLiteDS->FetchSRSId(&oSRS4326Clone), 4326);

  // 2. Lowercase 'epsg' lookup in spatial_ref_sys (lines 4453-4465)
  ASSERT_EQ(
      sqlite3_exec(
          hDB,
          "INSERT INTO spatial_ref_sys (srid, auth_name, auth_srid, srtext) "
          "VALUES (2154, 'epsg', '2154', 'PROJCS[\"RGF93 / Lambert-93\"]')",
          nullptr, nullptr, nullptr),
      SQLITE_OK);
  OGRSpatialReference oSRS2154;
  oSRS2154.importFromEPSG(2154);
  oSRS2154.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_EQ(poSQLiteDS->FetchSRSId(&oSRS2154), 2154);

  // 3. SpatiaLite 4 layout: Geographic vs Projected CS with and without
  // authority
  ASSERT_EQ(
      sqlite3_exec(hDB,
                   "ALTER TABLE spatial_ref_sys ADD COLUMN ref_sys_name TEXT;"
                   "ALTER TABLE spatial_ref_sys ADD COLUMN proj4text TEXT;",
                   nullptr, nullptr, nullptr),
      SQLITE_OK);
  poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = true;
  poSQLiteDS->*GetPrivate(Spatialite4LayoutTag{}) = true;

  // Geographic CS with authority (lines 4742-4755)
  OGRSpatialReference oGeogAuth;
  oGeogAuth.importFromEPSG(4269);  // NAD83
  oGeogAuth.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_EQ(poSQLiteDS->FetchSRSId(&oGeogAuth), 4269);

  // Projected CS without authority ('OGR' fake authority, lines 4762-4777)
  OGRSpatialReference oProjNoAuth;
  oProjNoAuth.importFromWkt(
      "PROJCS[\"CustomProj\",GEOGCS[\"G\",DATUM[\"D\",SPHEROID[\"S\",6378137,"
      "298.257]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
      "PROJECTION[\"Transverse_Mercator\"],PARAMETER[\"latitude_of_origin\",0],"
      "PARAMETER[\"central_meridian\",0],PARAMETER[\"scale_factor\",1],"
      "PARAMETER[\"false_easting\",500000],PARAMETER[\"false_northing\",0],"
      "UNIT[\"metre\",1]]");
  oProjNoAuth.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_GT(poSQLiteDS->FetchSRSId(&oProjNoAuth), 0);

  // Geographic CS without authority ('OGR' fake authority, lines 4779-4795)
  OGRSpatialReference oGeogNoAuth;
  oGeogNoAuth.importFromWkt(
      "GEOGCS[\"CustomGeog\",DATUM[\"D2\",SPHEROID[\"S2\",6378137,298.257]],"
      "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]");
  oGeogNoAuth.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_GT(poSQLiteDS->FetchSRSId(&oGeogNoAuth), 0);

  // Non-numeric authority code triggers fake OGR authority fallback (lines
  // 4647-4658)
  OGRSpatialReference oNonNumAuth;
  oNonNumAuth.importFromWkt(
      "GEOGCS[\"LAMB93\",DATUM[\"D_RGF93\",SPHEROID[\"GRS_1980\",6378137,298."
      "257222101]],PRIMEM[\"Greenwich\",0],UNIT[\"Degree\",0.0174532925199433],"
      "AUTHORITY[\"IGNF\",\"LAMB93\"]]");
  oNonNumAuth.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_GT(poSQLiteDS->FetchSRSId(&oNonNumAuth), 0);

  // 4. SpatiaLite 2/3 layout (without srtext / GetSRTEXTColName() == nullptr)
  poSQLiteDS->*GetPrivate(Spatialite4LayoutTag{}) = false;

  OGRSpatialReference oSp3ProjAuth;
  oSp3ProjAuth.importFromEPSG(32631);
  oSp3ProjAuth.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_EQ(poSQLiteDS->FetchSRSId(&oSp3ProjAuth), 32631);

  OGRSpatialReference oSp3GeogAuth;
  oSp3GeogAuth.importFromEPSG(4322);
  oSp3GeogAuth.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_EQ(poSQLiteDS->FetchSRSId(&oSp3GeogAuth), 4322);

  OGRSpatialReference oSp3ProjNoAuth;
  oSp3ProjNoAuth.importFromWkt(
      "PROJCS[\"Sp3Proj\",GEOGCS[\"G\",DATUM[\"D\",SPHEROID[\"S\",6378137,298."
      "257]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
      "PROJECTION[\"Transverse_Mercator\"],PARAMETER[\"latitude_of_origin\",0],"
      "PARAMETER[\"central_meridian\",1],PARAMETER[\"scale_factor\",1],"
      "PARAMETER[\"false_easting\",500000],PARAMETER[\"false_northing\",0],"
      "UNIT[\"metre\",1]]");
  oSp3ProjNoAuth.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_GT(poSQLiteDS->FetchSRSId(&oSp3ProjNoAuth), 0);

  OGRSpatialReference oSp3GeogNoAuth;
  oSp3GeogNoAuth.importFromWkt(
      "GEOGCS[\"Sp3Geog\",DATUM[\"D3\",SPHEROID[\"S3\",6378137,298.257]],"
      "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]");
  oSp3GeogNoAuth.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_GT(poSQLiteDS->FetchSRSId(&oSp3GeogNoAuth), 0);

  // 5. FetchSRS DB lookups for standard and SpatiaLite flavors (lines
  // 4862-4991)
  {
    // Standard OGR layout lookup
    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = false;
    ASSERT_EQ(
        sqlite3_exec(
            hDB,
            "INSERT INTO spatial_ref_sys (srid, auth_name, auth_srid, srtext) "
            "VALUES (7001, 'EPSG', '7001', 'GEOGCS[\"WGS "
            "84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
            "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT["
            "\"degree\",0.0174532925199433]]');"
            "INSERT INTO spatial_ref_sys (srid, auth_name, auth_srid, srtext) "
            "VALUES (7002, 'CUSTOM', '7002', 'INVALID_WKT');",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
    EXPECT_NE(poSQLiteDS->FetchSRS(7001), nullptr);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poSQLiteDS->FetchSRS(7002), nullptr);
    CPLPopErrorHandler();
    EXPECT_EQ(poSQLiteDS->FetchSRS(7999), nullptr);

    // SpatiaLite layout lookup (without srtext / GetSRTEXTColName() == nullptr)
    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = true;
    poSQLiteDS->*GetPrivate(Spatialite4LayoutTag{}) = false;
    ASSERT_EQ(
        sqlite3_exec(
            hDB,
            "DROP TABLE spatial_ref_sys;"
            "CREATE TABLE spatial_ref_sys (srid INTEGER UNIQUE, proj4text "
            "TEXT, auth_name TEXT, auth_srid TEXT, srs_wkt TEXT);"
            "INSERT INTO spatial_ref_sys VALUES (8001, NULL, 'EPSG', '4326', "
            "NULL);"
            "INSERT INTO spatial_ref_sys VALUES (8002, NULL, 'CUSTOM', '8002', "
            "'GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
            "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT["
            "\"degree\",0.0174532925199433]]');"
            "INSERT INTO spatial_ref_sys VALUES (8003, '+proj=longlat "
            "+datum=WGS84 +no_defs', 'CUSTOM', '8003', NULL);"
            "INSERT INTO spatial_ref_sys VALUES (8004, 'INVALID_PROJ4', "
            "'CUSTOM', '8004', 'INVALID_WKT');",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
    EXPECT_NE(poSQLiteDS->FetchSRS(8001), nullptr);
    EXPECT_NE(poSQLiteDS->FetchSRS(8002), nullptr);
    EXPECT_NE(poSQLiteDS->FetchSRS(8003), nullptr);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poSQLiteDS->FetchSRS(8004), nullptr);
    CPLPopErrorHandler();
    EXPECT_EQ(poSQLiteDS->FetchSRS(8999), nullptr);

    // Drop table and test DB query error path
    ASSERT_EQ(sqlite3_exec(hDB, "DROP TABLE spatial_ref_sys;", nullptr, nullptr,
                           nullptr),
              SQLITE_OK);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poSQLiteDS->FetchSRS(9000), nullptr);
    CPLPopErrorHandler();
  }

  // 6. Empty spatial_ref_sys table -> MAX(srid) returns null -> srid 50000
  // (line 4678)
  ASSERT_EQ(
      sqlite3_exec(
          hDB,
          "CREATE TABLE spatial_ref_sys (srid INTEGER UNIQUE, srtext TEXT);",
          nullptr, nullptr, nullptr),
      SQLITE_OK);
  poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = false;
  OGRSpatialReference oSRS50000;
  oSRS50000.importFromWkt(
      "GEOGCS[\"SRS50000\",DATUM[\"D5\",SPHEROID[\"S5\",6378137,298.257]],"
      "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]");
  oSRS50000.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_EQ(poSQLiteDS->FetchSRSId(&oSRS50000), 50000);

  // 7. Failures: SELECT MAX(srid) failure and INSERT failure
  ASSERT_EQ(sqlite3_exec(hDB, "DROP TABLE spatial_ref_sys;", nullptr, nullptr,
                         nullptr),
            SQLITE_OK);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  OGRSpatialReference oSRSFail;
  oSRSFail.importFromWkt(
      "GEOGCS[\"Fail\",DATUM[\"DF\",SPHEROID[\"SF\",6378137,298.257]],PRIMEM["
      "\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]");
  EXPECT_EQ(poSQLiteDS->FetchSRSId(&oSRSFail), poSQLiteDS->GetUndefinedSRID());
  CPLPopErrorHandler();
}

TEST_F(OgrSqliteDataSourceTest, ExtendedLayersAndTransactionsAndVirtualTables) {
  const std::string dbPath = std::string(kTestDir) + "/ext_layers.db";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  std::unique_ptr<GDALDataset> poDS(
      poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(poDS, nullptr);
  auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
  ASSERT_NE(poSQLiteDS, nullptr);

  // SetName (lines 4998-5002)
  poSQLiteDS->SetName(dbPath.c_str());

  // 1. ICreateLayer option variations and error paths
  {
    // Unsupported geometry type for SpatiaLite format (lines 3467-3479)
    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = true;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(
        poDS->CreateLayer("unsupp_geom", nullptr, wkbCircularString, nullptr),
        nullptr);
    CPLPopErrorHandler();
    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = false;

    // FORMAT=SPATIALITE warning on non-SpatiaLite DB (lines 3514, 3625)
    CPLStringList osSplFmt;
    osSplFmt.AddNameValue("FORMAT", "SPATIALITE");
    CPLErrorReset();
    OGRLayer* poSplL =
        poDS->CreateLayer("spl_fmt_layer", nullptr, wkbPoint, osSplFmt.List());
    EXPECT_NE(poSplL, nullptr);

    // Launder GEOMETRY_NAME (line 3538)
    CPLStringList osLaunderGeom;
    osLaunderGeom.AddNameValue("GEOMETRY_NAME", "my-bad#geom");
    osLaunderGeom.AddNameValue("LAUNDER", "YES");
    OGRLayer* poLaunderedL = poDS->CreateLayer("launder_layer", nullptr,
                                               wkbPoint, osLaunderGeom.List());
    ASSERT_NE(poLaunderedL, nullptr);
    EXPECT_STREQ(poLaunderedL->GetGeometryColumn(), "my_bad_geom");

    // SPATIAL_INDEX=YES (line 3681) and SPATIAL_INDEX=NO (line 3683)
    CPLStringList osIdxYes;
    osIdxYes.AddNameValue("SPATIAL_INDEX", "YES");
    OGRLayer* poIdxYesL =
        poDS->CreateLayer("idx_yes_layer", nullptr, wkbPoint, osIdxYes.List());
    ASSERT_NE(poIdxYesL, nullptr);

    CPLStringList osIdxNo;
    osIdxNo.AddNameValue("SPATIAL_INDEX", "NO");
    OGRLayer* poIdxNoL =
        poDS->CreateLayer("idx_no_layer", nullptr, wkbPoint, osIdxNo.List());
    ASSERT_NE(poIdxNoL, nullptr);

    // DeleteLayer on layer with spatial index (drops RTree tables, lines
    // 3823-3844)
    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = true;
    EXPECT_EQ(poDS->ExecuteSQL("DELLAYER:idx_yes_layer", nullptr, nullptr),
              nullptr);
    EXPECT_EQ(poDS->GetLayerByName("idx_yes_layer"), nullptr);
    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = false;

    // DeleteLayer out-of-range index (lines 3767-3773)
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poDS->DeleteLayer(-1), OGRERR_FAILURE);
    EXPECT_EQ(poDS->DeleteLayer(999), OGRERR_FAILURE);
    // DeleteLayer on non-existent layer via DELLAYER command (line 3751)
    EXPECT_EQ(
        poDS->ExecuteSQL("DELLAYER:nonexistent_layer_xyz", nullptr, nullptr),
        nullptr);
    CPLPopErrorHandler();
  }

  // 2. Read-only dataset errors for CreateLayer and DeleteLayer
  {
    std::unique_ptr<GDALDataset> poRO(
        GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(poRO, nullptr);

    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(poRO->CreateLayer("new_layer", nullptr, wkbPoint, nullptr),
              nullptr);
    EXPECT_EQ(poRO->DeleteLayer(0), OGRERR_FAILURE);
    EXPECT_EQ(poRO->ExecuteSQL("DELLAYER:spl_fmt_layer", nullptr, nullptr),
              nullptr);
    CPLPopErrorHandler();
  }

  // 3. ProcessTransactionSQL variations
  {
    poDS->ExecuteSQL("BEGIN", nullptr, nullptr);
    poDS->ExecuteSQL("SAVEPOINT sp_test", nullptr, nullptr);
    poDS->ExecuteSQL("ROLLBACK TO SAVEPOINT sp_test", nullptr, nullptr);
    poDS->ExecuteSQL("RELEASE SAVEPOINT sp_test", nullptr, nullptr);

    poDS->ExecuteSQL("SAVEPOINT sp_test2", nullptr, nullptr);
    poDS->ExecuteSQL("RELEASE sp_test2", nullptr, nullptr);

    poDS->ExecuteSQL("SAVEPOINT sp_test3", nullptr, nullptr);
    poDS->ExecuteSQL("ROLLBACK TO sp_test3", nullptr, nullptr);
    poDS->ExecuteSQL("ROLLBACK TRANSACTION", nullptr, nullptr);

    // Invalid tokens
    EXPECT_EQ(poDS->ExecuteSQL("SAVEPOINT", nullptr, nullptr), nullptr);
    EXPECT_EQ(poDS->ExecuteSQL("RELEASE", nullptr, nullptr), nullptr);
  }

  // 4. OpenVirtualTable on table with feature (lines 2824-2850)
  {
    OGRLayer* poVTLayer =
        poDS->CreateLayer("v_target", nullptr, wkbPoint, nullptr);
    ASSERT_NE(poVTLayer, nullptr);
    OGRFeature oF(poVTLayer->GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    oF.SetGeometry(&pt);
    EXPECT_EQ(poVTLayer->CreateFeature(&oF), OGRERR_NONE);

    EXPECT_TRUE((poSQLiteDS->*GetPrivate(OpenVirtualTableTag{}))(
        "v_target",
        "CREATE VIRTUAL TABLE v_target USING VirtualShape('shp', 'UTF-8', "
        "4326)"));
  }

  // 5. Open with LIST_VIRTUAL_OGR=YES and VirtualOGR table (lines 2446-2462)
  {
    const std::string vogrSrc = std::string(kTestDir) + "/vogr_src.db";
    std::unique_ptr<GDALDataset> poSrc(
        poDriver->Create(vogrSrc.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poSrc, nullptr);
    OGRLayer* poSrcL = poSrc->CreateLayer("pts", nullptr, wkbPoint, nullptr);
    ASSERT_NE(poSrcL, nullptr);
    poSrc.reset();

    std::string vsql =
        "CREATE VIRTUAL TABLE v_ogr USING VirtualOGR('" + vogrSrc + "')";
    EXPECT_EQ(sqlite3_exec(poSQLiteDS->GetDB(), vsql.c_str(), nullptr, nullptr,
                           nullptr),
              SQLITE_OK);
  }
  poDS.reset();
  {
    CPLStringList osVOgrOptions;
    osVOgrOptions.AddNameValue("LIST_VIRTUAL_OGR", "YES");
    osVOgrOptions.AddNameValue("LIST_ALL_TABLES", "YES");
    std::unique_ptr<GDALDataset> poVOgrDS(
        GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR, nullptr,
                          osVOgrOptions.List(), nullptr));
    ASSERT_NE(poVOgrDS, nullptr);
  }
}

TEST_F(OgrSqliteDataSourceTest, AdditionalCoverageForCloseOpenSrsAndStats) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(poDriver, nullptr);

  // 1. Driver unload hook (lines 233, 246)
  {
    OGRSQLiteDriverUnload(nullptr);
    RegisterOGRSQLite();
  }

  // 2. DealWithOgrSchemaOpenOption with "OGR_SCHEMA={}" (lines 273-276)
  {
    const std::string dbPath = std::string(kTestDir) + "/schema_empty.db";
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    poDS->CreateLayer("items", nullptr, wkbPoint, nullptr);
    poDS.reset();

    CPLStringList osOpenOptions;
    osOpenOptions.AddNameValue("OGR_SCHEMA", "{}");
    CPLPushErrorHandler(CPLQuietErrorHandler);
    CPLErrorReset();
    std::unique_ptr<GDALDataset> poReopen(
        GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, osOpenOptions.List(), nullptr));
    EXPECT_EQ(poReopen, nullptr);
    EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
    CPLPopErrorHandler();
    CPLErrorReset();
  }

  // 3. GetRelationshipNames & GetRelationship lazy loading & AddRelationship
  // errors (lines 355-358, 395-398, 863-864)
  {
    const std::string dbPath = std::string(kTestDir) + "/rel_lazy.db";
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    poDS->ExecuteSQL("CREATE TABLE parent(id INTEGER PRIMARY KEY);", nullptr,
                     nullptr);
    poDS->ExecuteSQL(
        "CREATE TABLE child(id INTEGER PRIMARY KEY, pid INTEGER REFERENCES "
        "parent(id));",
        nullptr, nullptr);
    poDS.reset();

    {
      std::unique_ptr<GDALDataset> poReopen(
          GDALDataset::Open(dbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                            nullptr, nullptr, nullptr));
      ASSERT_NE(poReopen, nullptr);
      EXPECT_NE(poReopen->GetRelationship("parent_child"), nullptr);
      auto relNames = poReopen->GetRelationshipNames(nullptr);
      EXPECT_FALSE(relNames.empty());
    }

    const std::string relErrDb = std::string(kTestDir) + "/rel_err.db";
    std::unique_ptr<GDALDataset> poRelDS(
        poDriver->Create(relErrDb.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poRelDS, nullptr);
    poRelDS->ExecuteSQL("CREATE TABLE left_t(id INTEGER PRIMARY KEY UNIQUE);",
                        nullptr, nullptr);
    poRelDS->ExecuteSQL(
        "CREATE TABLE right_t(id INTEGER PRIMARY KEY, lid INTEGER);", nullptr,
        nullptr);
    poRelDS.reset();

    poRelDS.reset(GDALDataset::Open(relErrDb.c_str(),
                                    GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr,
                                    nullptr, nullptr));
    ASSERT_NE(poRelDS, nullptr);

    std::string failureReason;
    auto relBadRight = std::make_unique<GDALRelationship>(
        "rel_bad_r", "left_t", "nonexistent_right_t", GRC_ONE_TO_MANY);
    relBadRight->SetLeftTableFields({"id"});
    relBadRight->SetRightTableFields({"lid"});
    EXPECT_FALSE(
        poRelDS->AddRelationship(std::move(relBadRight), failureReason));
    EXPECT_THAT(failureReason, HasSubstr("not an existing layer"));

    // Note: upstream AddRelationship uses "idx_%qw_related_id" in
    // sqlite3_mprintf, where %q formats right_t followed by literal 'w'.
    poRelDS->ExecuteSQL("CREATE TABLE other_t (x INT);", nullptr, nullptr);
    poRelDS->ExecuteSQL(
        "CREATE INDEX \"idx_right_tw_related_id\" ON \"other_t\" (\"x\");",
        nullptr, nullptr);
    auto relIdxFail = std::make_unique<GDALRelationship>(
        "rel_idx_fail", "left_t", "right_t", GRC_ONE_TO_MANY);
    relIdxFail->SetLeftTableFields({"id"});
    relIdxFail->SetRightTableFields({"lid"});
    CPLPushErrorHandler(CPLQuietErrorHandler);
    CPLErrorReset();
    EXPECT_FALSE(
        poRelDS->AddRelationship(std::move(relIdxFail), failureReason));
    EXPECT_THAT(failureReason, HasSubstr("Could not create index"));
    CPLPopErrorHandler();
    CPLErrorReset();
  }

  // 4. CloseDB() & OpenOrCreateDB WAL/URI/error branches (lines 549-559,
  // 587-624, 1362-1363, 1424-1429, 1523-1536, 1649, 1686)
  {
    // 4a. /vsimem/ read-only -wal cleanup where sqlite3_open fails (lines
    // 587-591)
    const std::string vsiDb = std::string(kTestDir) + "/vsi_wal_ro.db";
    std::unique_ptr<GDALDataset> poVsiDS(
        poDriver->Create(vsiDb.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poVsiDS, nullptr);
    poVsiDS.reset();

    const std::string vsiWal = vsiDb + "-wal";
    {
      std::unique_ptr<GDALDataset> poReopen(
          GDALDataset::Open(vsiDb.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                            nullptr, nullptr, nullptr));
      ASSERT_NE(poReopen, nullptr);

      VSILFILE* fpWal = VSIFOpenL(vsiWal.c_str(), "wb");
      ASSERT_NE(fpWal, nullptr);
      VSIFCloseL(fpWal);
    }
    VSIUnlink(vsiWal.c_str());

    // 4b. POSIX TempDir() database with persistent WAL (lines 592-624,
    // 1362-1363, 1523-1536, 1649, 1686)
    const std::string posixDb =
        (std::filesystem::path(::testing::TempDir()) / "wal_persist_posix.db")
            .string();
    const std::string posixWal = posixDb + "-wal";
    const std::string posixShm = posixDb + "-shm";
    absl::Cleanup unlinkPosix = [&posixDb, &posixWal, &posixShm] {
      std::filesystem::remove(posixDb);
      std::filesystem::remove(posixWal);
      std::filesystem::remove(posixShm);
    };

    sqlite3* rawDb = nullptr;
    ASSERT_EQ(sqlite3_open(posixDb.c_str(), &rawDb), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(rawDb,
                           "PRAGMA journal_mode=WAL; CREATE TABLE t(id INT); "
                           "INSERT INTO t VALUES(1);",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
#ifdef SQLITE_FCNTL_PERSIST_WAL
    int nPersist = 1;
    sqlite3_file_control(rawDb, "main", SQLITE_FCNTL_PERSIST_WAL, &nPersist);
#endif
    sqlite3_close(rawDb);

    // Open with "//" in filename and NOLOCK=YES
    {
      std::string doubleSlashPath =
          std::filesystem::path(::testing::TempDir()).string() +
          "//wal_persist_posix.db";
      CPLStringList osNolockOpts;
      osNolockOpts.AddNameValue("NOLOCK", "YES");
      std::unique_ptr<GDALDataset> poNolockDS(GDALDataset::Open(
          doubleSlashPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
          osNolockOpts.List(), nullptr));
      ASSERT_NE(poNolockDS, nullptr);
    }

    // nolock=1 + immutable=1 URI and open-options checks in Open()
    {
      std::string uriNolock =
          "SQLITE:file:" + posixDb + "?mode=ro&nolock=1&immutable=1";
      std::unique_ptr<GDALDataset> poUriDS(GDALDataset::Open(
          uriNolock.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
          nullptr, nullptr));
      EXPECT_NE(poUriDS, nullptr);

      CPLStringList osNolockImmOpts;
      osNolockImmOpts.AddNameValue("NOLOCK", "YES");
      osNolockImmOpts.AddNameValue("IMMUTABLE", "YES");
      std::unique_ptr<GDALDataset> poNolockImmDS(
          GDALDataset::Open(posixDb.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                            nullptr, osNolockImmOpts.List(), nullptr));
      EXPECT_NE(poNolockImmDS, nullptr);
    }

    // LoadExtensions in Open() (line 1686)
    {
      CPLSetConfigOption("OGR_SQLITE_LOAD_EXTENSIONS", "dummy_ext_on_open");
      CPLPushErrorHandler(CPLQuietErrorHandler);
      std::unique_ptr<GDALDataset> poExtDS(
          GDALDataset::Open(posixDb.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                            nullptr, nullptr, nullptr));
      CPLPopErrorHandler();
      CPLSetConfigOption("OGR_SQLITE_LOAD_EXTENSIONS", nullptr);
      EXPECT_NE(poExtDS, nullptr);
    }

    // sqlite3_open_v2 failure (lines 1424-1429)
    {
      std::string failPath = (std::filesystem::path(::testing::TempDir()) /
                              "no_dir_xyz" / "fail.db")
                                 .string();
      CPLPushErrorHandler(CPLQuietErrorHandler);
      CPLErrorReset();
      std::unique_ptr<GDALDataset> poFail(
          poDriver->Create(failPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      EXPECT_EQ(poFail, nullptr);
      EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
      CPLPopErrorHandler();
      CPLErrorReset();
    }

    // 4c. m_osFinalFilename copy and delete failure (lines 549-559)
    {
      const std::string tempFinalSrc = std::string(kTestDir) + "/final_src.db";
      std::unique_ptr<GDALDataset> poFinalDS(poDriver->Create(
          tempFinalSrc.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(poFinalDS, nullptr);
      auto* poSQLiteFinal = dynamic_cast<OGRSQLiteDataSource*>(poFinalDS.get());
      ASSERT_NE(poSQLiteFinal, nullptr);
      poSQLiteFinal->*GetPrivate(FinalFilenameTag{}) =
          "/vsimem/nonexistent_final_dir_xyz/target.db";
      VSIUnlink(tempFinalSrc.c_str());
      CPLPushErrorHandler(CPLQuietErrorHandler);
      CPLErrorReset();
      poFinalDS.reset();
      CPLPopErrorHandler();
      CPLErrorReset();
    }
  }

  // 5. SaveStatistics() stub & Spatialite trigger safety checks (lines
  // 1012-1014, 1770-1795)
  {
    const std::string dbPath = std::string(kTestDir) + "/stats_stub.db";
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
    ASSERT_NE(poSQLiteDS, nullptr);
    EXPECT_FALSE(poSQLiteDS->SpatialiteRequiresTrustedSchemaOn());
    EXPECT_TRUE(poSQLiteDS->AreSpatialiteTriggersSafe());
    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = true;
    (poSQLiteDS->*GetPrivate(SaveStatsTag{}))();
    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = false;
  }

  // 6. InitWithEPSG() on SpatiaLite spatial_ref_sys with srs_wkt column
  {
    const std::string dbPathWithSrtext =
        std::string(kTestDir) + "/init_epsg_withsrtext.db";
    std::unique_ptr<GDALDataset> poDS(poDriver->Create(
        dbPathWithSrtext.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
    ASSERT_NE(poSQLiteDS, nullptr);
    sqlite3* hDB = poSQLiteDS->GetDB();

    ASSERT_EQ(sqlite3_exec(hDB,
                           "DROP TABLE IF EXISTS spatial_ref_sys;"
                           "CREATE TABLE spatial_ref_sys (srid INTEGER PRIMARY "
                           "KEY, auth_name TEXT, auth_srid INTEGER, "
                           "ref_sys_name TEXT, proj4text TEXT, srs_wkt TEXT);"
                           "CREATE UNIQUE INDEX idx_limit_epsg ON "
                           "spatial_ref_sys((srid > 0));",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);

    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = true;
    absl::Cleanup cleanup = [poSQLiteDS] {
      poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = false;
    };

    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_FALSE((poSQLiteDS->*GetPrivate(InitWithEPSGTag{}))());
    CPLPopErrorHandler();
  }

  // 7. Open() SpatiaLite 4 geometry_columns with VirtualOGR,
  // views_geometry_columns, LIST_ALL_TABLES, and GDAL_OF_RASTER (lines
  // 2480-2483, 2534-2542, 2612-2640, 2662-2677, 2715-2736, 2786-2789,
  // 2843-2845, 3003-3004)
  {
    const std::string srcDb = std::string(kTestDir) + "/vogr_source.db";
    std::unique_ptr<GDALDataset> poSrc(
        poDriver->Create(srcDb.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poSrc, nullptr);
    OGRLayer* poSrcL =
        poSrc->CreateLayer("src_pts", nullptr, wkbPoint, nullptr);
    ASSERT_NE(poSrcL, nullptr);
    OGRFeature oFSrc(poSrcL->GetLayerDefn());
    OGRPoint ptSrc(2.0, 3.0);
    oFSrc.SetGeometry(&ptSrc);
    EXPECT_EQ(poSrcL->CreateFeature(&oFSrc), OGRERR_NONE);
    poSrc.reset();

    const std::string sp4FullDb = std::string(kTestDir) + "/sp4_full_views.db";
    {
      std::unique_ptr<GDALDataset> poDS(
          poDriver->Create(sp4FullDb.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(poDS, nullptr);
      OGRLayer* poBaseL =
          poDS->CreateLayer("base_pts", nullptr, wkbPoint, nullptr);
      ASSERT_NE(poBaseL, nullptr);
      OGRFeature oF(poBaseL->GetLayerDefn());
      OGRPoint pt(1.0, 1.0);
      oF.SetGeometry(&pt);
      EXPECT_EQ(poBaseL->CreateFeature(&oF), OGRERR_NONE);

      auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
      ASSERT_NE(poSQLiteDS, nullptr);
      sqlite3* hDB = poSQLiteDS->GetDB();

      EXPECT_EQ(sqlite3_exec(hDB, "DROP TABLE IF EXISTS geometry_columns;",
                             nullptr, nullptr, nullptr),
                SQLITE_OK);
      EXPECT_EQ(
          sqlite3_exec(
              hDB,
              "CREATE TABLE geometry_columns (f_table_name TEXT, "
              "f_geometry_column TEXT, geometry_type INT, coord_dimension INT, "
              "srid INT, spatial_index_enabled INT);",
              nullptr, nullptr, nullptr),
          SQLITE_OK);
      EXPECT_EQ(
          sqlite3_exec(hDB,
                       "INSERT INTO geometry_columns VALUES (NULL, NULL, 1, 2, "
                       "4326, 0);",
                       nullptr, nullptr, nullptr),
          SQLITE_OK);
      EXPECT_EQ(
          sqlite3_exec(
              hDB,
              "INSERT INTO geometry_columns VALUES ('base_pts', 'GEOMETRY', "
              "1, 2, 4326, 0);",
              nullptr, nullptr, nullptr),
          SQLITE_OK);

      std::string vsql =
          "CREATE VIRTUAL TABLE v_ogr_tbl USING VirtualOGR('" + srcDb + "');";
      EXPECT_EQ(sqlite3_exec(hDB, vsql.c_str(), nullptr, nullptr, nullptr),
                SQLITE_OK);
      EXPECT_EQ(sqlite3_exec(hDB,
                             "CREATE VIEW my_spatial_view AS SELECT OGC_FID "
                             "AS ROWID, GEOMETRY FROM base_pts;",
                             nullptr, nullptr, nullptr),
                SQLITE_OK);
      EXPECT_EQ(sqlite3_exec(
                    hDB,
                    "CREATE TABLE views_geometry_columns (view_name TEXT, "
                    "view_geometry TEXT, view_rowid TEXT, f_table_name TEXT, "
                    "f_geometry_column TEXT);",
                    nullptr, nullptr, nullptr),
                SQLITE_OK);
      EXPECT_EQ(sqlite3_exec(hDB,
                             "INSERT INTO views_geometry_columns VALUES (NULL, "
                             "'GEOMETRY', 'ROWID', 'base_pts', 'GEOMETRY');",
                             nullptr, nullptr, nullptr),
                SQLITE_OK);
      EXPECT_EQ(
          sqlite3_exec(hDB,
                       "INSERT INTO views_geometry_columns VALUES "
                       "('my_spatial_view', 'GEOMETRY', 'ROWID', 'base_pts', "
                       "'GEOMETRY');",
                       nullptr, nullptr, nullptr),
          SQLITE_OK);
      EXPECT_EQ(sqlite3_exec(hDB, "CREATE TABLE extra_tbl (id INT);", nullptr,
                             nullptr, nullptr),
                SQLITE_OK);
    }

    // Reopen read-only with LIST_VIRTUAL_OGR=YES and LIST_ALL_TABLES=YES
    {
      CPLStringList osOpts;
      osOpts.AddNameValue("LIST_VIRTUAL_OGR", "YES");
      osOpts.AddNameValue("LIST_ALL_TABLES", "YES");
      std::unique_ptr<GDALDataset> poReopen(GDALDataset::Open(
          sp4FullDb.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
          osOpts.List(), nullptr));
      ASSERT_NE(poReopen, nullptr);
      auto* poReopenSQLite = dynamic_cast<OGRSQLiteDataSource*>(poReopen.get());
      ASSERT_NE(poReopenSQLite, nullptr);

      for (int i = 0; i < poReopen->GetLayerCount(); ++i) {
        poReopenSQLite->IsLayerPrivate(i);
      }
    }

    // Reopen without LIST_ALL_TABLES and test GetLayerByName with parenthesis
    // (lines 3003-3004)
    {
      std::unique_ptr<GDALDataset> poReopen(GDALDataset::Open(
          sp4FullDb.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
          nullptr, nullptr));
      ASSERT_NE(poReopen, nullptr);
      EXPECT_EQ(poReopen->GetLayerByName("extra_tbl(dummy_col)"), nullptr);
    }

    // Reopen in update mode: SpatiaLite v4 update error (lines 2534-2542)
    {
      CPLPushErrorHandler(CPLQuietErrorHandler);
      CPLErrorReset();
      std::unique_ptr<GDALDataset> poReopenUpdate(
          GDALDataset::Open(sp4FullDb.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                            nullptr, nullptr, nullptr));
      EXPECT_EQ(poReopenUpdate, nullptr);
      EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
      CPLPopErrorHandler();
      CPLErrorReset();
    }

    const std::string plainDb = std::string(kTestDir) + "/plain_non_spatial.db";
    {
      std::unique_ptr<GDALDataset> poPlain(
          poDriver->Create(plainDb.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(poPlain, nullptr);
      poPlain->ExecuteSQL("CREATE TABLE plain_t(x INT);", nullptr, nullptr);
    }

    for (const std::string& rPath : {sp4FullDb, plainDb}) {
      OGRSQLiteDataSource oRasterProbe;
      GDALOpenInfo oOpenInfo(rPath.c_str(), GDAL_OF_RASTER);
      CPLPushErrorHandler(CPLQuietErrorHandler);
      CPLErrorReset();
      EXPECT_FALSE(oRasterProbe.Open(&oOpenInfo));
      CPLPopErrorHandler();
      CPLErrorReset();
    }
  }

  // 8. ExecuteSQL VACUUM stats refresh, step error, CREATE VIRTUAL TABLE,
  // savepoints (lines 3262, 3267-3282, 3353-3358, 3363-3374, 3423-3424, 4147,
  // 4179, 4210-4211, 4254-4256)
  {
    const std::string dbPath = std::string(kTestDir) + "/exec_sql_extended.db";
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
    ASSERT_NE(poSQLiteDS, nullptr);
    sqlite3* hDB = poSQLiteDS->GetDB();

    // 8a. VACUUM stats refresh (lines 3262, 3267-3282)
    OGRLayer* poL = poDS->CreateLayer("v_pts", nullptr, wkbPoint, nullptr);
    ASSERT_NE(poL, nullptr);
    OGRFeature oF(poL->GetLayerDefn());
    OGRPoint pt(5.0, 6.0);
    oF.SetGeometry(&pt);
    EXPECT_EQ(poL->CreateFeature(&oF), OGRERR_NONE);

    EXPECT_EQ(sqlite3_exec(hDB, "DROP TABLE IF EXISTS geometry_columns;",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    EXPECT_EQ(
        sqlite3_exec(
            hDB,
            "CREATE TABLE geometry_columns (f_table_name TEXT, "
            "f_geometry_column TEXT, geometry_type INT, coord_dimension INT, "
            "srid INT, spatial_index_enabled INT);",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
    EXPECT_EQ(sqlite3_exec(
                  hDB,
                  "INSERT INTO geometry_columns VALUES ('v_pts', 'geometry', "
                  "1, 2, 0, 0);",
                  nullptr, nullptr, nullptr),
              SQLITE_OK);
    EXPECT_EQ(
        sqlite3_exec(
            hDB,
            "CREATE TABLE geometry_columns_statistics (f_table_name TEXT, "
            "f_geometry_column TEXT, row_count INT, extent_min_x DOUBLE, "
            "extent_min_y DOUBLE, extent_max_x DOUBLE, extent_max_y "
            "DOUBLE, last_verified TEXT);",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
    EXPECT_EQ(sqlite3_exec(hDB,
                           "INSERT INTO geometry_columns_statistics VALUES "
                           "('v_pts', 'geometry', 1, 5.0, 6.0, 5.0, 6.0, "
                           "'2026-01-01T00:00:00Z');",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    poDS.reset();

    poDS.reset(GDALDataset::Open(dbPath.c_str(),
                                 GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                                 nullptr, nullptr));
    ASSERT_NE(poDS, nullptr);
    poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
    ASSERT_NE(poSQLiteDS, nullptr);
    EXPECT_NE(poDS->GetLayerByName("v_pts"), nullptr);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    CPLErrorReset();
    OGRLayer* poVacuumLayer = poDS->ExecuteSQL("VACUUM", nullptr, nullptr);
    if (poVacuumLayer != nullptr) {
      poDS->ReleaseResultSet(poVacuumLayer);
    }
    CPLPopErrorHandler();
    CPLErrorReset();

    // 8e. SELECT with spatial filter (lines 3423-3424)
    OGRPoint filterPt(5.0, 6.0);
    OGRLayer* poFiltered =
        poDS->ExecuteSQL("SELECT * FROM v_pts", &filterPt, nullptr);
    if (poFiltered != nullptr) {
      poDS->ReleaseResultSet(poFiltered);
    }
    poDS.reset();

    const std::string dbPath2 = std::string(kTestDir) + "/exec_sql_step_err.db";
    std::unique_ptr<GDALDataset> poDS2(
        poDriver->Create(dbPath2.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS2, nullptr);
    auto* poSQLiteDS2 = dynamic_cast<OGRSQLiteDataSource*>(poDS2.get());
    ASSERT_NE(poSQLiteDS2, nullptr);

    // 8b. Step error on duplicate PK constraint (lines 3353-3358)
    poDS2->ExecuteSQL("CREATE TABLE uniq_pk (id INTEGER PRIMARY KEY);", nullptr,
                      nullptr);
    poDS2->ExecuteSQL("INSERT INTO uniq_pk VALUES (1);", nullptr, nullptr);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    CPLErrorReset();
    EXPECT_EQ(
        poDS2->ExecuteSQL("INSERT INTO uniq_pk VALUES (1);", nullptr, nullptr),
        nullptr);
    EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
    CPLPopErrorHandler();
    CPLErrorReset();

    // 8c. CREATE VIRTUAL TABLE via ExecuteSQL (lines 3363-3374)
    const std::string vogrSrc = std::string(kTestDir) + "/vogr_source.db";
    std::string vsql =
        "CREATE VIRTUAL TABLE v_exec_tbl USING VirtualOGR('" + vogrSrc + "')";
    EXPECT_EQ(poDS2->ExecuteSQL(vsql.c_str(), nullptr, nullptr), nullptr);

    // 8d. Savepoints nested unwind (lines 4147, 4179, 4210-4211, 4254-4256)
    poDS2->StartTransaction();
    poSQLiteDS2->StartSavepoint("sp1");
    poSQLiteDS2->StartSavepoint("sp2");
    poSQLiteDS2->ReleaseSavepoint("sp1");
    poDS2->CommitTransaction();

    poDS2->StartTransaction();
    poSQLiteDS2->StartSavepoint("sp1");
    poSQLiteDS2->StartSavepoint("sp2");
    poSQLiteDS2->RollbackToSavepoint("sp1");
    poDS2->ExecuteSQL("ROLLBACK sp1", nullptr, nullptr);
    poDS2->ExecuteSQL("ROLLBACK TRANSACTION", nullptr, nullptr);
    poDS2->ExecuteSQL("ROLLBACK", nullptr, nullptr);
  }

  // 9. ICreateLayer SRS, SpatialIndex warning, and SpatiaLite errors (lines
  // 3474-3477, 3548-3552, 3616, 3626-3631, 3668-3674)
  {
    const std::string dbPath = std::string(kTestDir) + "/create_layer_ext.db";
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
    ASSERT_NE(poSQLiteDS, nullptr);

    OGRSpatialReference srs4326;
    srs4326.importFromEPSG(4326);
    OGRLayer* poWithSrs =
        poDS->CreateLayer("with_srs", &srs4326, wkbPoint, nullptr);
    ASSERT_NE(poWithSrs, nullptr);

    CPLStringList osSiOpts;
    osSiOpts.AddNameValue("FORMAT", "SPATIALITE");
    osSiOpts.AddNameValue("SPATIAL_INDEX", "YES");
    CPLPushErrorHandler(CPLQuietErrorHandler);
    CPLErrorReset();
    poDS->CreateLayer("sp_idx_warn", nullptr, wkbPoint, osSiOpts.List());
    EXPECT_EQ(CPLGetLastErrorType(), CE_Warning);
    CPLPopErrorHandler();
    CPLErrorReset();

    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = true;
    absl::Cleanup cleanupSpatialite = [poSQLiteDS] {
      poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = false;
    };

    {
      OGRGeomFieldDefn oGeomField("geom", wkbCircularString);
      CPLPushErrorHandler(CPLQuietErrorHandler);
      CPLErrorReset();
      EXPECT_EQ(poSQLiteDS->ICreateLayer("bad_geom", &oGeomField, nullptr),
                nullptr);
      EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
      CPLPopErrorHandler();
      CPLErrorReset();
    }

    {
      OGRGeomFieldDefn oGeomField("geom", wkbPoint);
      CPLStringList osWktOpts;
      osWktOpts.AddNameValue("FORMAT", "WKT");
      CPLPushErrorHandler(CPLQuietErrorHandler);
      CPLErrorReset();
      EXPECT_EQ(
          poSQLiteDS->ICreateLayer("bad_fmt", &oGeomField, osWktOpts.List()),
          nullptr);
      EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
      CPLPopErrorHandler();
      CPLErrorReset();
    }
  }

  // 10. FetchSRSId pointer cache hit, AutoIdentifyEPSG, SpatiaLite inserts, and
  // failure (lines 4360, 4389-4398, 4742-4754, 4779-4789, 4809-4813)
  {
    const std::string dbPath = std::string(kTestDir) + "/srs_mgmt_ext.db";
    std::unique_ptr<GDALDataset> poDS(
        poDriver->Create(dbPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poDS, nullptr);
    auto* poSQLiteDS = dynamic_cast<OGRSQLiteDataSource*>(poDS.get());
    ASSERT_NE(poSQLiteDS, nullptr);
    sqlite3* hDB = poSQLiteDS->GetDB();

    OGRSpatialReference srs4326;
    srs4326.importFromEPSG(4326);
    poSQLiteDS->FetchSRSId(&srs4326);
    const OGRSpatialReference* pCached = poSQLiteDS->FetchSRS(4326);
    ASSERT_NE(pCached, nullptr);
    EXPECT_EQ(poSQLiteDS->FetchSRSId(pCached), 4326);

    OGRSpatialReference srsWgsNoAuth;
    srsWgsNoAuth.SetFromUserInput(
        R"(GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]])");
    int nIdIdentified = poSQLiteDS->FetchSRSId(&srsWgsNoAuth);
    EXPECT_GT(nIdIdentified, 0);

    OGRSpatialReference srs32631NoAuth;
    srs32631NoAuth.importFromEPSG(32631);
    const int iAuth = srs32631NoAuth.GetRoot()->FindChild("AUTHORITY");
    if (iAuth >= 0) {
      srs32631NoAuth.GetRoot()->DestroyChild(iAuth);
    }
    EXPECT_EQ(poSQLiteDS->FetchSRSId(&srs32631NoAuth), 32631);

    poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = true;
    absl::Cleanup cleanupSpatialiteSRS = [poSQLiteDS] {
      poSQLiteDS->*GetPrivate(IsSpatialiteDBTag{}) = false;
    };

    EXPECT_EQ(sqlite3_exec(hDB, "DROP TABLE IF EXISTS spatial_ref_sys;",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    EXPECT_EQ(
        sqlite3_exec(hDB,
                     "CREATE TABLE spatial_ref_sys (srid INTEGER PRIMARY "
                     "KEY, auth_name TEXT NOT NULL, auth_srid INTEGER NOT "
                     "NULL, ref_sys_name TEXT, proj4text TEXT NOT NULL, "
                     "srs_wkt TEXT);",
                     nullptr, nullptr, nullptr),
        SQLITE_OK);
    (poSQLiteDS->*GetPrivate(GetSRTEXTColNameTag{}))();

    OGRSpatialReference srs4269;
    srs4269.importFromEPSG(4269);
    EXPECT_EQ(poSQLiteDS->FetchSRSId(&srs4269), 4269);

    OGRSpatialReference srsCustom;
    srsCustom.SetFromUserInput(
        R"(GEOGCS["CustomSphere",DATUM["CustomDatum",SPHEROID["Sphere",6371000,0]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]])");
    int nCustomId = poSQLiteDS->FetchSRSId(&srsCustom);
    EXPECT_GT(nCustomId, 0);

    OGRSpatialReference srs4978;
    srs4978.importFromEPSG(4978);
    EXPECT_EQ(poSQLiteDS->FetchSRSId(&srs4978), 4978);

    OGRSpatialReference srsCustomGeoc;
    srsCustomGeoc.SetFromUserInput(
        R"(GEOCCS["CustomGeocentric",DATUM["CustomDatum",SPHEROID["Sphere",6371000,0]],PRIMEM["Greenwich",0],UNIT["metre",1]])");
    EXPECT_GT(poSQLiteDS->FetchSRSId(&srsCustomGeoc), 0);

    EXPECT_EQ(
        sqlite3_exec(hDB,
                     "CREATE TRIGGER fail_srs BEFORE INSERT ON spatial_ref_sys "
                     "BEGIN SELECT RAISE(ABORT, 'srs insert blocked'); END;",
                     nullptr, nullptr, nullptr),
        SQLITE_OK);
    OGRSpatialReference srsBlocked;
    srsBlocked.importFromEPSG(32634);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    CPLErrorReset();
    EXPECT_EQ(poSQLiteDS->FetchSRSId(&srsBlocked), 0);
    EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
    CPLPopErrorHandler();
    CPLErrorReset();

    const std::string srsNocaseDb = std::string(kTestDir) + "/srs_nocase.db";
    std::unique_ptr<GDALDataset> poNocaseDS(
        poDriver->Create(srsNocaseDb.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(poNocaseDS, nullptr);
    auto* poNocaseSQLite = dynamic_cast<OGRSQLiteDataSource*>(poNocaseDS.get());
    ASSERT_NE(poNocaseSQLite, nullptr);
    sqlite3* hDBNocase = poNocaseSQLite->GetDB();
    EXPECT_EQ(sqlite3_exec(
                  hDBNocase,
                  "DROP TABLE IF EXISTS spatial_ref_sys; "
                  "CREATE TABLE spatial_ref_sys (srid INT, auth_name TEXT, "
                  "auth_srid TEXT, srtext TEXT); "
                  "INSERT INTO spatial_ref_sys VALUES (32632, 'epsg', '32632', "
                  "'LOCAL_CS[\"d\"]');",
                  nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_create_collation(hDBNocase, "NOCASE", SQLITE_UTF8, nullptr,
                             nullptr);
    OGRSpatialReference srs32632;
    srs32632.importFromEPSG(32632);
    EXPECT_EQ(poNocaseSQLite->FetchSRSId(&srs32632), 32632);
    EXPECT_EQ(sqlite3_exec(hDBNocase, "DROP TABLE spatial_ref_sys;", nullptr,
                           nullptr, nullptr),
              SQLITE_OK);
    OGRSpatialReference srs32633;
    srs32633.importFromEPSG(32633);
    poNocaseSQLite->FetchSRSId(&srs32633);
  }
}

// TODO: b/335317901 - Cover live libspatialite dlopen symbol loading and
// RasterLite2 overview dataset paths when compiled in.

}  // namespace
}  // namespace autotest2
