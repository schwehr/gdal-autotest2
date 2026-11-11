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
// Unit tests for ISO-19111 DatabaseContext and AuthorityFactory in
// proj/src/iso19111/factory.cpp. Note: in google3 default non-GCE builds,
// proj.db is embedded in memory via memvfs and skips checkDatabaseLayout() when
// opened with an empty path.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/factory.cpp

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/google/util/temp_file.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/coordinateoperation.hpp"
#include "third_party/proj4/proj/include/proj/coordinates.hpp"
#include "third_party/proj4/proj/include/proj/coordinatesystem.hpp"
#include "third_party/proj4/proj/include/proj/crs.hpp"
#include "third_party/proj4/proj/include/proj/datum.hpp"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/metadata.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"
#include "third_party/sqlite/sqlite3.h"

namespace proj {
namespace {

using ::osgeo::proj::common::Angle;
using ::osgeo::proj::common::DateTime;
using ::osgeo::proj::common::IdentifiedObject;
using ::osgeo::proj::common::Length;
using ::osgeo::proj::common::Measure;
using ::osgeo::proj::common::ObjectDomain;
using ::osgeo::proj::common::ObjectUsage;
using ::osgeo::proj::common::Scale;
using ::osgeo::proj::common::UnitOfMeasure;
using ::osgeo::proj::coordinates::CoordinateMetadata;
using ::osgeo::proj::crs::BoundCRS;
using ::osgeo::proj::crs::CompoundCRS;
using ::osgeo::proj::crs::CRS;
using ::osgeo::proj::crs::CRSNNPtr;
using ::osgeo::proj::crs::DerivedProjectedCRS;
using ::osgeo::proj::crs::EngineeringCRS;
using ::osgeo::proj::crs::GeodeticCRS;
using ::osgeo::proj::crs::GeographicCRS;
using ::osgeo::proj::crs::ProjectedCRS;
using ::osgeo::proj::crs::TemporalCRS;
using ::osgeo::proj::crs::VerticalCRS;
using ::osgeo::proj::cs::AxisDirection;
using ::osgeo::proj::cs::CartesianCS;
using ::osgeo::proj::cs::CoordinateSystemAxis;
using ::osgeo::proj::cs::EllipsoidalCS;
using ::osgeo::proj::cs::Meridian;
using ::osgeo::proj::cs::OrdinalCS;
using ::osgeo::proj::cs::SphericalCS;
using ::osgeo::proj::cs::VerticalCS;
using ::osgeo::proj::datum::Datum;
using ::osgeo::proj::datum::DatumEnsemble;
using ::osgeo::proj::datum::DatumNNPtr;
using ::osgeo::proj::datum::DynamicGeodeticReferenceFrame;
using ::osgeo::proj::datum::DynamicVerticalReferenceFrame;
using ::osgeo::proj::datum::Ellipsoid;
using ::osgeo::proj::datum::EngineeringDatum;
using ::osgeo::proj::datum::GeodeticReferenceFrame;
using ::osgeo::proj::datum::PrimeMeridian;
using ::osgeo::proj::datum::RealizationMethod;
using ::osgeo::proj::datum::TemporalDatum;
using ::osgeo::proj::datum::VerticalReferenceFrame;
using ::osgeo::proj::io::AuthorityFactory;
using ::osgeo::proj::io::DatabaseContext;
using ::osgeo::proj::io::DatabaseContextNNPtr;
using ::osgeo::proj::io::FactoryException;
using ::osgeo::proj::io::NoSuchAuthorityCodeException;
using ::osgeo::proj::io::WKTFormatter;
using ::osgeo::proj::metadata::Extent;
using ::osgeo::proj::metadata::GeographicBoundingBox;
using ::osgeo::proj::metadata::Identifier;
using ::osgeo::proj::metadata::PositionalAccuracy;
using ::osgeo::proj::operation::ConcatenatedOperation;
using ::osgeo::proj::operation::Conversion;
using ::osgeo::proj::operation::CoordinateOperation;
using ::osgeo::proj::operation::GeneralParameterValueNNPtr;
using ::osgeo::proj::operation::OperationMethod;
using ::osgeo::proj::operation::OperationParameter;
using ::osgeo::proj::operation::OperationParameterNNPtr;
using ::osgeo::proj::operation::OperationParameterValue;
using ::osgeo::proj::operation::ParameterValue;
using ::osgeo::proj::operation::ParameterValueNNPtr;
using ::osgeo::proj::operation::PointMotionOperation;
using ::osgeo::proj::operation::Transformation;
using ::osgeo::proj::util::ArrayOfBaseObject;
using ::osgeo::proj::util::IComparable;
using ::osgeo::proj::util::optional;
using ::osgeo::proj::util::PropertyMap;

PropertyMap NameMap(const std::string& name) {
  return PropertyMap().set(IdentifiedObject::NAME_KEY, name);
}

struct SqliteDeleter {
  void operator()(sqlite3* db) const {
    if (db != nullptr) {
      sqlite3_close(db);
    }
  }
};
using ScopedSqlite = std::unique_ptr<sqlite3, SqliteDeleter>;

struct SqliteStmtDeleter {
  void operator()(sqlite3_stmt* stmt) const {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
    }
  }
};
using ScopedStmt = std::unique_ptr<sqlite3_stmt, SqliteStmtDeleter>;

class ScopedEnvVar {
 public:
  ScopedEnvVar(const char* name, const char* value) : name_(name) {
    if (const char* prev = std::getenv(name)) {
      had_previous_ = true;
      previous_ = prev;
    }
    if (value != nullptr) {
      setenv(name_.c_str(), value, /*overwrite=*/1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ~ScopedEnvVar() {
    if (had_previous_) {
      setenv(name_.c_str(), previous_.c_str(), /*overwrite=*/1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

 private:
  std::string name_;
  bool had_previous_ = false;
  std::string previous_;
};

// Helper to execute arbitrary SQL on a raw sqlite3* handle.
void ExecSql(sqlite3* db, const std::string& sql) {
  char* errmsg = nullptr;
  int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errmsg);
  std::string err = errmsg ? errmsg : "";
  sqlite3_free(errmsg);
  ASSERT_EQ(SQLITE_OK, rc) << "SQL error: " << err << " in: " << sql;
}

void CreateSqliteDbWithSql(const std::string& path, const std::string& sql) {
  sqlite3* raw_db = nullptr;
  const int rc = sqlite3_open(path.c_str(), &raw_db);
  ScopedSqlite db(raw_db);
  ASSERT_EQ(SQLITE_OK, rc);
  ExecSql(db.get(), sql);
}

// Helper to query a single string value via raw sqlite3*.
std::string QuerySingleValue(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw_stmt, nullptr) !=
      SQLITE_OK) {
    return "";
  }
  ScopedStmt stmt(raw_stmt);
  if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    const unsigned char* txt = sqlite3_column_text(stmt.get(), 0);
    return txt ? reinterpret_cast<const char*>(txt) : "NULL";
  }
  return "";
}

// Copies the embedded proj.db into a disk file so external-DB code paths and
// custom row insertions can be tested without constraints.
void CloneEmbeddedDbToFile(DatabaseContextNNPtr src_ctx,
                           const std::string& dst_path,
                           bool drop_triggers = true) {
  sqlite3* raw_dst_db = nullptr;
  const int open_rc = sqlite3_open(dst_path.c_str(), &raw_dst_db);
  ScopedSqlite dst_db(raw_dst_db);
  ASSERT_EQ(SQLITE_OK, open_rc);

  auto* src_db = static_cast<sqlite3*>(src_ctx->getSqliteHandle());
  ASSERT_NE(nullptr, src_db);
  sqlite3_backup* backup =
      sqlite3_backup_init(dst_db.get(), "main", src_db, "main");
  ASSERT_NE(nullptr, backup);
  EXPECT_EQ(SQLITE_DONE, sqlite3_backup_step(backup, -1));
  EXPECT_EQ(SQLITE_OK, sqlite3_backup_finish(backup));

  if (drop_triggers) {
    sqlite3_stmt* raw_stmt = nullptr;
    const int prep_rc = sqlite3_prepare_v2(
        dst_db.get(), "SELECT name FROM sqlite_master WHERE type = 'trigger'",
        -1, &raw_stmt, nullptr);
    ScopedStmt stmt(raw_stmt);
    ASSERT_EQ(SQLITE_OK, prep_rc);
    std::vector<std::string> triggers;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      const char* name =
          reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
      if (name) triggers.emplace_back(name);
    }
    stmt.reset();
    for (const auto& t : triggers) {
      ExecSql(dst_db.get(), "DROP TRIGGER \"" + t + "\"");
    }
  }
}

// ---------------------------------------------------------------------------
// Custom SQLite Functions & DatabaseContext Creation / Layout Checks
// ---------------------------------------------------------------------------

TEST(FactoryTest, CustomSqliteFunctionsAndBasicDatabaseContext) {
  absl::Cleanup cleanup = []() {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());

  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
  EXPECT_FALSE(db_ctx->getPath().empty());
  EXPECT_NE(nullptr, db_ctx->getSqliteHandle());
  EXPECT_EQ(0U, db_ctx->getQueryCounter());

  // Test metadata lookups.
  const char* major = db_ctx->getMetadata("DATABASE.LAYOUT.VERSION.MAJOR");
  ASSERT_NE(nullptr, major);
  EXPECT_STREQ("1", major);
  EXPECT_GT(db_ctx->getQueryCounter(), 0U);
  const char* minor = db_ctx->getMetadata("DATABASE.LAYOUT.VERSION.MINOR");
  ASSERT_NE(nullptr, minor);
  EXPECT_EQ(nullptr, db_ctx->getMetadata("DOES_NOT_EXIST_KEY"));

  // Test authorities and database structure.
  std::set<std::string> auths = db_ctx->getAuthorities();
  EXPECT_NE(auths.end(), auths.find("EPSG"));
  EXPECT_NE(auths.end(), auths.find("ESRI"));
  EXPECT_NE(auths.end(), auths.find("OGC"));
  EXPECT_NE(auths.end(), auths.find("PROJ"));

  std::vector<std::string> structure = db_ctx->getDatabaseStructure();
  EXPECT_FALSE(structure.empty());

  // Exercise custom SQLite functions registered on the handle:
  // pseudo_area_from_swne and intersects_bbox.
  auto* sql_db = static_cast<sqlite3*>(db_ctx->getSqliteHandle());
  // Float args, normal east >= west.
  EXPECT_FALSE(
      QuerySingleValue(sql_db,
                       "SELECT pseudo_area_from_swne(-10.5, 20.5, 10.5, 40.5)")
          .empty());
  // Integer args, antimeridian crossing (east < west).
  EXPECT_FALSE(QuerySingleValue(
                   sql_db, "SELECT pseudo_area_from_swne(-10, 170, 10, -170)")
                   .empty());
  // Invalid / NULL args -> returns NULL.
  EXPECT_EQ("NULL",
            QuerySingleValue(sql_db,
                             "SELECT pseudo_area_from_swne(NULL, 0, 10, 10)"));
  EXPECT_EQ("NULL",
            QuerySingleValue(sql_db,
                             "SELECT pseudo_area_from_swne('bad', 0, 10, 10)"));
  EXPECT_EQ("NULL",
            QuerySingleValue(sql_db,
                             "SELECT pseudo_area_from_swne(0, NULL, 10, 10)"));

  // intersects_bbox: intersecting, disjoint, and NULL (non-numeric or south >
  // north).
  EXPECT_EQ("1",
            QuerySingleValue(sql_db,
                             "SELECT intersects_bbox(-10.0, -10.0, 10.0, 10.0, "
                             "0.0, 0.0, 20.0, 20.0)"));
  EXPECT_EQ("0",
            QuerySingleValue(
                sql_db,
                "SELECT intersects_bbox(-20, -20, -10, -10, 10, 10, 20, 20)"));
  EXPECT_EQ("NULL",
            QuerySingleValue(
                sql_db,
                "SELECT intersects_bbox(NULL, -20, -10, -10, 10, 10, 20, 20)"));
  EXPECT_EQ("0", QuerySingleValue(
                     sql_db,
                     "SELECT intersects_bbox(-10, 20, 10, -20, 0, 0, 10, 10)"));

  // DatabaseContext::create(void*) wrapping existing sqlite3* handle.
  auto wrapped_ctx = DatabaseContext::create(sql_db);
  EXPECT_EQ(sql_db, wrapped_ctx->getSqliteHandle());
}

TEST(FactoryTest, ExternalDatabaseAndAuxiliaryDatabases) {
  absl::Cleanup cleanup = []() {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());

  // Non-existent path throws FactoryException.
  EXPECT_THROW(DatabaseContext::create("/nonexistent/dir/proj_missing.db", {},
                                       ctx.get()),
               FactoryException);

  // Corrupt file throws FactoryException.
  TempFile bad_file("bad_proj.db");
  ASSERT_TRUE(WriteTextFile(bad_file.path(), "not a sqlite file"));
  EXPECT_THROW(DatabaseContext::create(bad_file.path().string(), {}, ctx.get()),
               FactoryException);

  // Valid SQLite DB without metadata table -> throws FactoryException.
  TempDir dir("factory_ext_db");
  std::string empty_db_path = (dir.path() / "empty.db").string();
  CreateSqliteDbWithSql(empty_db_path, "CREATE TABLE dummy(x INT);");
  EXPECT_THROW(DatabaseContext::create(empty_db_path, {}, ctx.get()),
               FactoryException);

  // Valid SQLite DB with empty metadata table -> throws FactoryException.
  std::string no_ver_db_path = (dir.path() / "no_ver.db").string();
  CreateSqliteDbWithSql(no_ver_db_path,
                        "CREATE TABLE metadata(key TEXT, value TEXT);");
  EXPECT_THROW(DatabaseContext::create(no_ver_db_path, {}, ctx.get()),
               FactoryException);

  // Wrong MAJOR version -> throws FactoryException.
  std::string bad_major_path = (dir.path() / "bad_major.db").string();
  CreateSqliteDbWithSql(
      bad_major_path,
      "CREATE TABLE metadata(key TEXT, value TEXT);"
      "INSERT INTO metadata VALUES ('DATABASE.LAYOUT.VERSION.MAJOR', '99');"
      "INSERT INTO metadata VALUES ('DATABASE.LAYOUT.VERSION.MINOR', '0');");
  EXPECT_THROW(DatabaseContext::create(bad_major_path, {}, ctx.get()),
               FactoryException);

  // Old MINOR version (< 4) -> throws FactoryException.
  std::string old_minor_path = (dir.path() / "old_minor.db").string();
  CreateSqliteDbWithSql(
      old_minor_path,
      "CREATE TABLE metadata(key TEXT, value TEXT);"
      "INSERT INTO metadata VALUES ('DATABASE.LAYOUT.VERSION.MAJOR', '1');"
      "INSERT INTO metadata VALUES ('DATABASE.LAYOUT.VERSION.MINOR', '0');");
  EXPECT_THROW(DatabaseContext::create(old_minor_path, {}, ctx.get()),
               FactoryException);

  // Clone embedded DB to a real disk file and open it via
  // DatabaseContext::create.
  auto embedded_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
  std::string main_db_path = (dir.path() / "main_proj.db").string();
  CloneEmbeddedDbToFile(embedded_ctx, main_db_path);

  auto disk_ctx1 = DatabaseContext::create(main_db_path, {}, ctx.get());
  EXPECT_EQ(main_db_path, disk_ctx1->getPath());
  // Opening a second time hits SQLiteHandleCache.
  auto disk_ctx2 = DatabaseContext::create(main_db_path, {}, ctx.get());
  EXPECT_EQ(main_db_path, disk_ctx2->getPath());

  // Auxiliary DB 1: no metadata table (sparse grid_alternatives DB).
  // Auxiliary DB 2: empty metadata table.
  // Auxiliary DB 3: matching metadata version (1.7) with a custom
  // prime_meridian.
  std::string aux_sparse_path = (dir.path() / "aux_sparse.db").string();
  CreateSqliteDbWithSql(aux_sparse_path, "CREATE TABLE extra_table(id INT);");

  std::string aux_empty_meta_path = (dir.path() / "aux_empty_meta.db").string();
  CreateSqliteDbWithSql(aux_empty_meta_path,
                        "CREATE TABLE metadata(key TEXT, value TEXT);");

  const char* major_ptr =
      embedded_ctx->getMetadata("DATABASE.LAYOUT.VERSION.MAJOR");
  ASSERT_NE(nullptr, major_ptr);
  const std::string major_str(major_ptr);
  const char* minor_ptr =
      embedded_ctx->getMetadata("DATABASE.LAYOUT.VERSION.MINOR");
  ASSERT_NE(nullptr, minor_ptr);
  const std::string minor_str(minor_ptr);
  std::string aux_valid_path = (dir.path() / "aux_valid.db").string();
  CreateSqliteDbWithSql(
      aux_valid_path,
      std::string("CREATE TABLE metadata(key TEXT, value TEXT);") +
          "INSERT INTO metadata VALUES ('DATABASE.LAYOUT.VERSION.MAJOR', '" +
          major_str + "');" +
          "INSERT INTO metadata VALUES ('DATABASE.LAYOUT.VERSION.MINOR', '" +
          minor_str + "');" +
          "CREATE TABLE prime_meridian("
          "auth_name TEXT, code TEXT, name TEXT, longitude DOUBLE, "
          "uom_auth_name TEXT, uom_code TEXT, deprecated INT);"
          "INSERT INTO prime_meridian VALUES "
          "('AUX_AUTH', '9901', 'Aux Meridian', 12.5, 'EPSG', '9102', 0);");

  auto with_aux_ctx = DatabaseContext::create(
      main_db_path, {aux_sparse_path, aux_empty_meta_path, aux_valid_path},
      ctx.get());
  auto aux_factory = AuthorityFactory::create(with_aux_ctx, "AUX_AUTH");
  auto aux_pm = aux_factory->createPrimeMeridian("9901");
  EXPECT_EQ("Aux Meridian", aux_pm->nameStr());

  // Auxiliary DB with mismatched version -> throws FactoryException.
  std::string aux_bad_ver_path = (dir.path() / "aux_bad_ver.db").string();
  CreateSqliteDbWithSql(
      aux_bad_ver_path,
      "CREATE TABLE metadata(key TEXT, value TEXT);"
      "INSERT INTO metadata VALUES ('DATABASE.LAYOUT.VERSION.MAJOR', '1');"
      "INSERT INTO metadata VALUES ('DATABASE.LAYOUT.VERSION.MINOR', "
      "'999');");
  EXPECT_THROW(
      DatabaseContext::create(main_db_path, {aux_bad_ver_path}, ctx.get()),
      FactoryException);

  // Test PROJ_AUX_DB environment variable fallback (using ':' and ';').
  std::string env_val = aux_sparse_path + ":" + aux_valid_path;
  ScopedEnvVar scoped_aux_db("PROJ_AUX_DB", env_val.c_str());
  auto env_aux_ctx = DatabaseContext::create(main_db_path, {}, ctx.get());
  auto env_aux_factory = AuthorityFactory::create(env_aux_ctx, "AUX_AUTH");
  EXPECT_EQ("Aux Meridian",
            env_aux_factory->createPrimeMeridian("9901")->nameStr());
}

// ---------------------------------------------------------------------------
// DatabaseContext Internal & Helper Methods
// ---------------------------------------------------------------------------

TEST(FactoryTest, DatabaseContextHelperAndGridMethods) {
  absl::Cleanup cleanup = []() {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());
  InMemoryFileSystem mem_fs;
  mem_fs.Install(ctx.get());
  constexpr std::array<float, 4> kShifts = {1.0f, 1.0f, 1.0f, 1.0f};
  mem_fs.AddFile("/mem/local_custom_grid.gtx",
                 BuildGtxGrid(0.0, 0.0, 1.0, 1.0, 2, 2, kShifts));
  const char* search_paths[] = {"/mem"};
  proj_context_set_search_paths(ctx.get(), 1, search_paths);

  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());

  // lookForGridAlternative
  std::string proj_filename;
  std::string proj_format;
  bool inverse = false;
  EXPECT_TRUE(db_ctx->lookForGridAlternative(
      "OSTN15_NTv2_OSGBtoETRS.gsb", proj_filename, proj_format, inverse));
  EXPECT_FALSE(proj_filename.empty());
  EXPECT_FALSE(db_ctx->lookForGridAlternative(
      "non_existent_official_grid.gsb", proj_filename, proj_format, inverse));

  // getProjGridName and getOldProjGridName
  std::string new_name = db_ctx->getProjGridName("OSTN15_NTv2_OSGBtoETRS.gsb");
  if (!new_name.empty()) {
    EXPECT_EQ("OSTN15_NTv2_OSGBtoETRS.gsb",
              db_ctx->getOldProjGridName(new_name));
  }
  EXPECT_TRUE(db_ctx->getProjGridName("non_existent_old.gsb").empty());
  EXPECT_TRUE(db_ctx->getOldProjGridName("non_existent_new.tif").empty());

  // lookForGridInfo: "null" special case
  std::string full_filename;
  std::string package_name;
  std::string url;
  bool direct_download = false;
  bool open_license = false;
  bool grid_available = false;
  EXPECT_TRUE(db_ctx->lookForGridInfo("null", false, full_filename,
                                      package_name, url, direct_download,
                                      open_license, grid_available));
  EXPECT_TRUE(grid_available);
  EXPECT_TRUE(open_license);

  // lookForGridInfo: known grid in grid_alternatives with
  // considerKnownGridsAsAvailable = false and true, plus cache hit & eviction.
  EXPECT_TRUE(db_ctx->lookForGridInfo(
      "uk_os_OSTN15_NTv2_OSGBtoETRS.tif", false, full_filename, package_name,
      url, direct_download, open_license, grid_available));
  // Second call hits cacheGridNameToInfo.
  EXPECT_TRUE(db_ctx->lookForGridInfo(
      "uk_os_OSTN15_NTv2_OSGBtoETRS.tif", false, full_filename, package_name,
      url, direct_download, open_license, grid_available));

  // With custom URL endpoint and considerKnownGridsAsAvailable = true.
  proj_context_set_url_endpoint(ctx.get(), "https://mirror.example.com/proj");
  db_ctx->invalidateGridInfo("uk_os_OSTN15_NTv2_OSGBtoETRS.tif");
  EXPECT_TRUE(db_ctx->lookForGridInfo(
      "uk_os_OSTN15_NTv2_OSGBtoETRS.tif", true, full_filename, package_name,
      url, direct_download, open_license, grid_available));
  EXPECT_TRUE(grid_available);
  EXPECT_NE(std::string::npos, url.find("https://mirror.example.com/proj/"));

  // lookForGridInfo with old_proj_grid_name when new grid file is present in
  // /mem.
  if (!new_name.empty()) {
    mem_fs.AddTextFile("/mem/" + new_name, "dummy");
    db_ctx->invalidateGridInfo("OSTN15_NTv2_OSGBtoETRS.gsb");
    EXPECT_TRUE(db_ctx->lookForGridInfo(
        "OSTN15_NTv2_OSGBtoETRS.gsb", false, full_filename, package_name, url,
        direct_download, open_license, grid_available));
    EXPECT_TRUE(grid_available);
  }

  // lookForGridInfo: unknown grid found locally in /mem.
  EXPECT_FALSE(db_ctx->lookForGridInfo(
      "local_custom_grid.gtx", false, full_filename, package_name, url,
      direct_download, open_license, grid_available));
  EXPECT_TRUE(grid_available);
  EXPECT_EQ("/mem/local_custom_grid.gtx", full_filename);

  // isKnownName, getName, getTextDefinition
  EXPECT_TRUE(db_ctx->isKnownName("WGS 84", "geodetic_crs"));
  EXPECT_FALSE(db_ctx->isKnownName("NO_SUCH_CRS_NAME_XYZ", "geodetic_crs"));
  EXPECT_EQ("WGS 84", db_ctx->getName("geodetic_crs", "EPSG", "4326"));
  // Second call hits cacheNames_.
  EXPECT_EQ("WGS 84", db_ctx->getName("geodetic_crs", "EPSG", "4326"));
  EXPECT_TRUE(db_ctx->getName("geodetic_crs", "EPSG", "9999999").empty());
  EXPECT_TRUE(
      db_ctx->getTextDefinition("geodetic_crs", "EPSG", "4326").empty());
  EXPECT_TRUE(
      db_ctx->getTextDefinition("geodetic_crs", "EPSG", "9999999").empty());

  // getAliasFromOfficialName & getAliases
  std::string esri_alias =
      db_ctx->getAliasFromOfficialName("WGS 84", "geographic_2D_crs", "ESRI");
  EXPECT_FALSE(esri_alias.empty());
  EXPECT_TRUE(
      db_ctx->getAliasFromOfficialName("NAD83", "geographic_3D_crs", "ESRI")
          .empty());
  EXPECT_TRUE(db_ctx
                  ->getAliasFromOfficialName("NON_EXISTENT_OFFICIAL",
                                             "geodetic_crs", "ESRI")
                  .empty());

  auto aliases1 =
      db_ctx->getAliases("EPSG", "4326", "WGS 84", "geographic_2D_crs", "ESRI");
  EXPECT_FALSE(aliases1.empty());
  // Cache hit.
  auto aliases1_cached =
      db_ctx->getAliases("EPSG", "4326", "WGS 84", "geographic_2D_crs", "ESRI");
  EXPECT_EQ(aliases1, aliases1_cached);

  auto aliases_by_name =
      db_ctx->getAliases("", "", "WGS 84", "geographic_2D_crs", "not EPSG_OLD");
  auto aliases_3d =
      db_ctx->getAliases("", "", "WGS 84", "geographic_3D_crs", "");
  auto aliases_missing =
      db_ctx->getAliases("", "", "NON_EXISTENT_XYZ", "geodetic_crs", "");
  EXPECT_TRUE(aliases_missing.empty());

  // getAllowedAuthorities
  EXPECT_FALSE(db_ctx->getAllowedAuthorities("EPSG", "EPSG").empty());
  // Second call hits cacheAllowedAuthorities_.
  EXPECT_FALSE(db_ctx->getAllowedAuthorities("EPSG", "EPSG").empty());
  EXPECT_FALSE(db_ctx->getAllowedAuthorities("ESRI", "EPSG").empty());
  EXPECT_TRUE(db_ctx->getAllowedAuthorities("EPSG", "ESRI").empty());
  EXPECT_TRUE(db_ctx->getAllowedAuthorities("UNKNOWN_A", "UNKNOWN_B").empty());

  // getNonDeprecated
  auto non_dep = db_ctx->getNonDeprecated("geodetic_crs", "EPSG", "4268");
  (void)non_dep;
  auto* sql_db = static_cast<sqlite3*>(db_ctx->getSqliteHandle());
  std::string dep_code = QuerySingleValue(
      sql_db,
      "SELECT deprecated_code FROM deprecation WHERE table_name = "
      "'geodetic_crs' AND deprecated_auth_name = 'EPSG' LIMIT 1");
  if (!dep_code.empty()) {
    EXPECT_FALSE(
        db_ctx->getNonDeprecated("geodetic_crs", "EPSG", dep_code).empty());
  }

  // getAuthorityAndVersion, getVersionedAuthority,
  // getVersionedAuthoritiesFromName
  std::string auth_out;
  std::string ver_out;
  EXPECT_TRUE(db_ctx->getAuthorityAndVersion("IAU_2015", auth_out, ver_out));
  EXPECT_EQ("IAU", auth_out);
  EXPECT_EQ("2015", ver_out);
  EXPECT_FALSE(
      db_ctx->getAuthorityAndVersion("NOT_VERSIONED", auth_out, ver_out));

  std::string versioned_out;
  EXPECT_TRUE(db_ctx->getVersionedAuthority("IAU", "2015", versioned_out));
  EXPECT_EQ("IAU_2015", versioned_out);
  EXPECT_FALSE(db_ctx->getVersionedAuthority("IAU", "9999", versioned_out));

  auto iau_versions = db_ctx->getVersionedAuthoritiesFromName("IAU");
  EXPECT_FALSE(iau_versions.empty());
  EXPECT_TRUE(db_ctx->getVersionedAuthoritiesFromName("NONE").empty());

  // getTransformationsForGridName
  auto grid_ops = DatabaseContext::getTransformationsForGridName(
      db_ctx, "uk_os_OSTN15_NTv2_OSGBtoETRS.tif");
  EXPECT_FALSE(grid_ops.empty());

  // toWGS84AutocorrectWrongValues
  double tx = 0.0, ty = 0.0, tz = 0.0, rx = 0.0, ry = 0.0, rz = 0.0, s = 0.0;
  EXPECT_FALSE(
      db_ctx->toWGS84AutocorrectWrongValues(tx, ty, tz, rx, ry, rz, s));

  // Query a real 9607 Coordinate Frame transformation from
  // helmert_transformation_table.
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(
          sql_db,
          "SELECT tx, ty, tz, rx, ry, rz, scale_difference FROM "
          "helmert_transformation_table WHERE method_auth_name = 'EPSG' AND "
          "method_code = 9607 AND translation_uom_code = 9001 AND "
          "rotation_uom_code = 9104 AND scale_difference_uom_code = 9202 AND "
          "rx != 0 AND ry != 0 AND rz != 0 AND scale_difference != 0 AND "
          "deprecated = 0 LIMIT 1",
          -1, &raw_stmt, nullptr) == SQLITE_OK) {
    ScopedStmt stmt(raw_stmt);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      tx = sqlite3_column_double(stmt.get(), 0);
      ty = sqlite3_column_double(stmt.get(), 1);
      tz = sqlite3_column_double(stmt.get(), 2);
      rx = sqlite3_column_double(stmt.get(), 3);
      ry = sqlite3_column_double(stmt.get(), 4);
      rz = sqlite3_column_double(stmt.get(), 5);
      s = sqlite3_column_double(stmt.get(), 6);
      double orig_rx = rx;
      EXPECT_TRUE(
          db_ctx->toWGS84AutocorrectWrongValues(tx, ty, tz, rx, ry, rz, s));
      EXPECT_NEAR(-orig_rx, rx, 1e-12);
    }
  }
}

// ---------------------------------------------------------------------------
// DatabaseContext: suggestsCodeFor & getInsertStatementsFor
// ---------------------------------------------------------------------------

TEST(FactoryTest, SuggestsCodeForAndGetInsertStatementsFor) {
  absl::Cleanup cleanup = []() {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());
  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
  auto epsg_factory = AuthorityFactory::create(db_ctx, "EPSG");

  // Calling getInsertStatementsFor before startInsertStatementsSession throws.
  auto wgs84 = epsg_factory->createGeographicCRS("4326");
  EXPECT_THROW(db_ctx->getInsertStatementsFor(wgs84, "HOBU", "4326", false),
               FactoryException);

  // Construct custom objects of every supported type.
  auto custom_ang_uom =
      UnitOfMeasure("custom_deg", 0.017, UnitOfMeasure::Type::ANGULAR);
  auto custom_lin_uom =
      UnitOfMeasure("custom_m", 1.5, UnitOfMeasure::Type::LINEAR);
  auto custom_scale_uom =
      UnitOfMeasure("custom_scale", 0.99, UnitOfMeasure::Type::SCALE);
  auto custom_time_uom =
      UnitOfMeasure("custom_sec", 2.0, UnitOfMeasure::Type::TIME);

  auto custom_pm = PrimeMeridian::create(NameMap("Custom Meridian (Test)"),
                                         Angle(12.34, custom_ang_uom));

  auto custom_ellps_two_axis = Ellipsoid::createTwoAxis(
      NameMap("Custom TwoAxis Ellipsoid"), Length(1234567.0, custom_lin_uom),
      Length(1230000.0, custom_lin_uom));
  auto custom_ellps_flat =
      Ellipsoid::createFlattenedSphere(NameMap("Custom Earth Ellipsoid"),
                                       Length(6378137.0), Scale(298.257223563));
  auto custom_ellps_sphere =
      Ellipsoid::createSphere(NameMap("Custom Sphere"), Length(6371000.0));

  auto bbox = GeographicBoundingBox::create(-10.0, 40.0, 10.0, 50.0);
  auto custom_extent = Extent::create(
      optional<std::string>("Custom European Area"), {bbox}, {}, {});
  auto known_extent = epsg_factory->createExtent("1262");

  auto custom_grf = GeodeticReferenceFrame::create(
      NameMap("Custom Geodetic Datum")
          .set(ObjectUsage::SCOPE_KEY, "Custom Geodetic Scope")
          .set(ObjectUsage::DOMAIN_OF_VALIDITY_KEY, custom_extent)
          .set("PUBLICATION_DATE", "2022-05-01")
          .set("ANCHOR_EPOCH", "2020.0"),
      custom_ellps_two_axis, optional<std::string>("Custom Anchor"), custom_pm);

  auto custom_dyn_grf = DynamicGeodeticReferenceFrame::create(
      NameMap("Custom Dynamic Geodetic Datum")
          .set(ObjectUsage::SCOPE_KEY, "Geodesy.")
          .set(ObjectUsage::DOMAIN_OF_VALIDITY_KEY, known_extent),
      custom_ellps_flat, optional<std::string>("Dyn Anchor"),
      PrimeMeridian::GREENWICH, Measure(2015.0, UnitOfMeasure::YEAR),
      optional<std::string>());

  auto custom_grf_member2 = GeodeticReferenceFrame::create(
      NameMap("Custom Member Datum 2"), custom_ellps_two_axis,
      optional<std::string>(), custom_pm);
  auto custom_grf_sphere = GeodeticReferenceFrame::create(
      NameMap("Custom Sphere Datum"), custom_ellps_sphere,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  auto custom_geog_sphere = GeographicCRS::create(
      NameMap("Custom Geog Sphere"), custom_grf_sphere,
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));

  auto custom_geod_ensemble = DatumEnsemble::create(
      NameMap("Custom Geodetic Ensemble"),
      std::vector<DatumNNPtr>{custom_grf, custom_grf_member2},
      PositionalAccuracy::create("2.5"));

  auto custom_vrf =
      VerticalReferenceFrame::create(NameMap("Custom Vertical Datum")
                                         .set("PUBLICATION_DATE", "2021-01-01")
                                         .set("ANCHOR_EPOCH", "2019.5"),
                                     optional<std::string>("Vert Anchor"));

  auto custom_dyn_vrf = DynamicVerticalReferenceFrame::create(
      NameMap("Custom Dynamic Vertical Datum"),
      optional<std::string>("Dyn Vert Anchor"),
      optional<RealizationMethod>(RealizationMethod::GEOID),
      Measure(2018.0, UnitOfMeasure::YEAR), optional<std::string>());

  auto custom_vert_ensemble =
      DatumEnsemble::create(NameMap("Custom Vertical Ensemble"),
                            std::vector<DatumNNPtr>{custom_vrf, custom_dyn_vrf},
                            PositionalAccuracy::create("1.0"));

  // Custom GeodeticCRSs:
  // 1) Standard 2D geographic CRS with custom datum.
  auto custom_geog_2d = GeographicCRS::create(
      NameMap("Custom Geog 2D"), custom_grf,
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
  // 2) Custom 2D geographic CRS with custom CS (lon/lat in custom angular unit)
  //    and datum ensemble.
  auto custom_ellps_cs_2d = EllipsoidalCS::create(
      PropertyMap(),
      CoordinateSystemAxis::create(NameMap("Geodetic longitude"), "lon",
                                   AxisDirection::EAST, custom_ang_uom),
      CoordinateSystemAxis::create(NameMap("Geodetic latitude"), "lat",
                                   AxisDirection::NORTH, custom_ang_uom));
  auto custom_geog_2d_ens =
      GeographicCRS::create(NameMap("Custom Geog 2D Ensemble"), nullptr,
                            custom_geod_ensemble, custom_ellps_cs_2d);

  // 3) Custom 3D geographic CRS with custom 3D EllipsoidalCS.
  auto custom_ellps_cs_3d = EllipsoidalCS::create(
      PropertyMap(),
      CoordinateSystemAxis::create(NameMap("Geodetic latitude"), "lat",
                                   AxisDirection::NORTH, custom_ang_uom),
      CoordinateSystemAxis::create(NameMap("Geodetic longitude"), "lon",
                                   AxisDirection::EAST, custom_ang_uom),
      CoordinateSystemAxis::create(NameMap("Ellipsoidal height"), "h",
                                   AxisDirection::UP, custom_lin_uom));
  auto custom_geog_3d = GeographicCRS::create(
      NameMap("Custom Geog 3D"), custom_dyn_grf, custom_ellps_cs_3d);

  // 4) Custom Geocentric CRS with custom 3D CartesianCS.
  auto custom_geocentric_cs = CartesianCS::createGeocentric(custom_lin_uom);
  auto custom_geocentric = GeodeticCRS::create(
      NameMap("Custom Geocentric"), custom_grf, custom_geocentric_cs);

  // Custom ProjectedCRS:
  // 1) With UTM conversion and standard CartesianCS.
  auto utm_conv = Conversion::createUTM(NameMap("Custom UTM 31N"), 31, true);
  auto custom_proj_crs = ProjectedCRS::create(
      NameMap("Custom Projected UTM"), custom_geog_2d, utm_conv,
      CartesianCS::createEastingNorthing(UnitOfMeasure::METRE));

  // 2) With custom conversion (using WKT2 method name without EPSG ID, plus
  //    custom scale and time units) and custom 2D CartesianCS.
  auto param_lat =
      OperationParameter::create(NameMap("Latitude of natural origin"));
  auto param_scale =
      OperationParameter::create(NameMap("Scale factor at natural origin"));
  auto param_time =
      OperationParameter::create(NameMap("Transformation reference epoch")
                                     .set(Identifier::CODESPACE_KEY, "EPSG")
                                     .set(Identifier::CODE_KEY, 1049));
  auto custom_conv = Conversion::create(
      NameMap("Custom Geos Conv"), NameMap("Geostationary Satellite (Sweep X)"),
      {param_lat, param_scale, param_time},
      {ParameterValue::create(Angle(0.0, custom_ang_uom)),
       ParameterValue::create(Scale(0.9996, custom_scale_uom)),
       ParameterValue::create(Measure(2020.0, custom_time_uom))});
  auto custom_cart_2d = CartesianCS::create(
      PropertyMap(),
      CoordinateSystemAxis::create(NameMap("Easting"), "E", AxisDirection::EAST,
                                   custom_lin_uom),
      CoordinateSystemAxis::create(NameMap("Northing"), "N",
                                   AxisDirection::NORTH, custom_lin_uom));
  auto custom_proj_crs2 = ProjectedCRS::create(
      NameMap("Custom Projected 2"), wgs84, custom_conv, custom_cart_2d);

  // Custom VerticalCRS:
  // 1) With custom vertical datum and standard VerticalCS (EPSG:6499).
  auto custom_vert_crs = VerticalCRS::create(
      NameMap("Custom Vertical CRS").set("GEOID_MODEL", "GEOID18"), custom_vrf,
      VerticalCS::createGravityRelatedHeight(UnitOfMeasure::METRE));
  // 2) With vertical datum ensemble and custom VerticalCS.
  auto custom_vert_cs = VerticalCS::create(
      PropertyMap(),
      CoordinateSystemAxis::create(NameMap("Depth"), "D", AxisDirection::DOWN,
                                   custom_lin_uom));
  auto custom_vert_crs_ens =
      VerticalCRS::create(NameMap("Custom Vertical Ensemble CRS"), nullptr,
                          custom_vert_ensemble, custom_vert_cs);

  // Custom CompoundCRS & BoundCRS:
  auto custom_compound = CompoundCRS::create(
      NameMap("Custom Compound CRS"),
      std::vector<CRSNNPtr>{custom_geog_2d, custom_vert_crs});
  auto custom_bound = BoundCRS::createFromTOWGS84(
      custom_geog_sphere, {1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 0.0});

  // Test suggestsCodeFor (both alphanumeric and numeric) for all types.
  for (bool numeric : {false, true}) {
    EXPECT_FALSE(db_ctx->suggestsCodeFor(custom_pm, "HOBU", numeric).empty());
    EXPECT_FALSE(db_ctx->suggestsCodeFor(custom_ellps_two_axis, "HOBU", numeric)
                     .empty());
    EXPECT_FALSE(db_ctx->suggestsCodeFor(custom_grf, "HOBU", numeric).empty());
    EXPECT_FALSE(db_ctx->suggestsCodeFor(custom_vrf, "HOBU", numeric).empty());
    EXPECT_FALSE(
        db_ctx->suggestsCodeFor(custom_geod_ensemble, "HOBU", numeric).empty());
    EXPECT_FALSE(
        db_ctx->suggestsCodeFor(custom_vert_ensemble, "HOBU", numeric).empty());
    EXPECT_FALSE(
        db_ctx->suggestsCodeFor(custom_geog_2d, "HOBU", numeric).empty());
    EXPECT_FALSE(
        db_ctx->suggestsCodeFor(custom_proj_crs, "HOBU", numeric).empty());
    EXPECT_FALSE(
        db_ctx->suggestsCodeFor(custom_vert_crs, "HOBU", numeric).empty());
    EXPECT_FALSE(
        db_ctx->suggestsCodeFor(custom_compound, "HOBU", numeric).empty());
    EXPECT_FALSE(
        db_ctx->suggestsCodeFor(custom_bound, "HOBU", numeric).empty());
    EXPECT_FALSE(db_ctx->suggestsCodeFor(utm_conv, "HOBU", numeric).empty());
  }
  // Also test suggestsCodeFor with "EPSG" where existing codes exist.
  auto greenwich = epsg_factory->createPrimeMeridian("8901");
  EXPECT_EQ("GREENWICH", db_ctx->suggestsCodeFor(greenwich, "HOBU", false));
  EXPECT_FALSE(db_ctx->suggestsCodeFor(greenwich, "EPSG", true).empty());

  // Unhandled object type in suggestsCodeFor throws FactoryException.
  auto eng_datum = EngineeringDatum::create(NameMap("Custom Eng Datum"),
                                            optional<std::string>());
  EXPECT_THROW(db_ctx->suggestsCodeFor(eng_datum, "HOBU", false),
               FactoryException);

  // Start insert statements session.
  db_ctx->startInsertStatementsSession();
  // Calling startInsertStatementsSession twice throws FactoryException.
  EXPECT_THROW(db_ctx->startInsertStatementsSession(), FactoryException);

  // Already-known CRS returns empty statement list.
  EXPECT_TRUE(
      db_ctx->getInsertStatementsFor(wgs84, "EPSG", "4326", false, {"EPSG"})
          .empty());

  // Generate insert statements for all supported object types.
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_pm, "HOBU", "PM1", false).empty());
  EXPECT_FALSE(
      db_ctx
          ->getInsertStatementsFor(custom_ellps_two_axis, "HOBU", "EL1", false)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_ellps_flat, "HOBU", "EL2", true)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_ellps_sphere, "HOBU", "EL3", false)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_grf, "HOBU", "GD1", false).empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_dyn_grf, "HOBU", "GD2", true)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_geod_ensemble, "HOBU", "GE1", false)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_vrf, "HOBU", "VD1", false).empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_dyn_vrf, "HOBU", "VD2", true)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_vert_ensemble, "HOBU", "VE1", false)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_geog_2d, "HOBU", "CRS1", false)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_geog_2d_ens, "HOBU", "CRS2", true)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_geog_3d, "HOBU", "CRS3", false)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_geocentric, "HOBU", "CRS4", true)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_proj_crs, "HOBU", "PCRS1", false)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_proj_crs2, "HOBU", "PCRS2", true)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_vert_crs, "HOBU", "VCRS1", false)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_vert_crs_ens, "HOBU", "VCRS2", true)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_compound, "HOBU", "CCRS1", false)
          .empty());
  EXPECT_FALSE(
      db_ctx->getInsertStatementsFor(custom_bound, "HOBU", "BCRS1", true)
          .empty());

  // Error cases inside getInsertStatementsFor:
  // 1) Unhandled object type.
  EXPECT_THROW(db_ctx->getInsertStatementsFor(eng_datum, "HOBU", "E1", false),
               FactoryException);

  // 2) ProjectedCRS with > 7 parameters in conversion.
  std::vector<OperationParameterNNPtr> eight_op_params;
  std::vector<ParameterValueNNPtr> eight_param_vals;
  for (int i = 1; i <= 8; ++i) {
    eight_op_params.push_back(
        OperationParameter::create(NameMap("Param " + std::to_string(i))
                                       .set(Identifier::CODESPACE_KEY, "EPSG")
                                       .set(Identifier::CODE_KEY, 8800 + i)));
    eight_param_vals.push_back(ParameterValue::create(Measure(1.0)));
  }
  auto conv_8_params = Conversion::create(NameMap("Conv8"), NameMap("Method8"),
                                          eight_op_params, eight_param_vals);
  auto proj_8_params = ProjectedCRS::create(
      NameMap("Proj8"), wgs84, conv_8_params,
      CartesianCS::createEastingNorthing(UnitOfMeasure::METRE));
  EXPECT_THROW(
      db_ctx->getInsertStatementsFor(proj_8_params, "HOBU", "P8", false),
      FactoryException);

  // 3) ProjectedCRS with unknown parameter name and no EPSG code.
  auto bad_param_conv = Conversion::create(
      NameMap("BadParamConv"), NameMap("Transverse Mercator"),
      {OperationParameter::create(NameMap("Completely Unknown Parameter"))},
      {ParameterValue::create(Measure(1.0))});
  auto proj_bad_param = ProjectedCRS::create(
      NameMap("ProjBadParam"), wgs84, bad_param_conv,
      CartesianCS::createEastingNorthing(UnitOfMeasure::METRE));
  EXPECT_THROW(
      db_ctx->getInsertStatementsFor(proj_bad_param, "HOBU", "PBP", false),
      FactoryException);

  db_ctx->stopInsertStatementsSession();
  // Calling stopInsertStatementsSession again is a safe no-op.
  db_ctx->stopInsertStatementsSession();
}

// ---------------------------------------------------------------------------
// AuthorityFactory: Basic Creation, Exceptions, Units, Extents, PMs, Ellipsoids
// ---------------------------------------------------------------------------

TEST(FactoryTest, AuthorityFactoryPrimitivesAndExceptions) {
  absl::Cleanup cleanup = []() {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());
  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());

  // Case-insensitive "epsg" normalizes to "EPSG".
  auto epsg_factory = AuthorityFactory::create(db_ctx, "epsg");
  EXPECT_EQ("EPSG", epsg_factory->getAuthority());
  EXPECT_EQ(db_ctx, epsg_factory->databaseContext());

  // Exception classes and copy constructors.
  try {
    epsg_factory->createUnitOfMeasure("99999999");
    FAIL() << "Expected NoSuchAuthorityCodeException";
  } catch (const NoSuchAuthorityCodeException& ex) {
    NoSuchAuthorityCodeException copy_ex(ex);
    EXPECT_EQ("EPSG", copy_ex.getAuthority());
    EXPECT_EQ("99999999", copy_ex.getAuthorityCode());
    EXPECT_NE(nullptr, copy_ex.what());
  }

  try {
    // Ambiguous code "4326" exists in both geodetic_crs and conversion.
    epsg_factory->createObject("4326");
    FAIL() << "Expected FactoryException";
  } catch (const FactoryException& ex) {
    FactoryException copy_ex(ex);
    EXPECT_NE(nullptr, copy_ex.what());
  }

  // Units of measure: linear, angular (including 9107/9108/9110), scale, time.
  auto uom_m = epsg_factory->createUnitOfMeasure("9001");
  EXPECT_EQ(UnitOfMeasure::Type::LINEAR, uom_m->type());
  // Cache hit on second call.
  EXPECT_EQ(uom_m, epsg_factory->createUnitOfMeasure("9001"));

  auto uom_deg = epsg_factory->createUnitOfMeasure("9102");
  EXPECT_EQ(UnitOfMeasure::Type::ANGULAR, uom_deg->type());
  auto uom_deg_supp = epsg_factory->createUnitOfMeasure("9122");
  EXPECT_EQ("degree", uom_deg_supp->name());
  auto uom_dms = epsg_factory->createUnitOfMeasure("9107");
  EXPECT_EQ(UnitOfMeasure::Type::ANGULAR, uom_dms->type());
  auto uom_arcsec = epsg_factory->createUnitOfMeasure("9104");
  EXPECT_EQ(UnitOfMeasure::Type::ANGULAR, uom_arcsec->type());
  auto uom_scale = epsg_factory->createUnitOfMeasure("9201");
  EXPECT_EQ(UnitOfMeasure::Type::SCALE, uom_scale->type());
  auto uom_time = epsg_factory->createUnitOfMeasure("1040");
  EXPECT_EQ(UnitOfMeasure::Type::TIME, uom_time->type());

  // Extents: world, antimeridian-crossing, and not found.
  auto ext_world = epsg_factory->createExtent("1262");
  EXPECT_EQ(1U, ext_world->geographicElements().size());
  EXPECT_THROW(epsg_factory->createExtent("99999999"),
               NoSuchAuthorityCodeException);

  // PrimeMeridians: Greenwich (8901), Paris (8903), Ferro (8909 with 9110 DMS).
  auto pm_greenwich = epsg_factory->createPrimeMeridian("8901");
  EXPECT_EQ("Greenwich", pm_greenwich->nameStr());
  EXPECT_EQ(pm_greenwich, epsg_factory->createPrimeMeridian("8901"));
  auto pm_ferro = epsg_factory->createPrimeMeridian("8909");
  EXPECT_NEAR(-17.6666666667, pm_ferro->longitude().value(), 1e-6);
  EXPECT_THROW(epsg_factory->createPrimeMeridian("99999999"),
               NoSuchAuthorityCodeException);

  // Ellipsoids: flattened sphere (7030), sphere (7048), two-axis (7042 or IAU).
  auto ellps_wgs84 = epsg_factory->createEllipsoid("7030");
  EXPECT_TRUE(ellps_wgs84->inverseFlattening().has_value());
  EXPECT_EQ(ellps_wgs84, epsg_factory->createEllipsoid("7030"));

  auto ellps_sphere = epsg_factory->createEllipsoid("7048");
  EXPECT_TRUE(ellps_sphere->isSphere());

  auto* sql_db = static_cast<sqlite3*>(db_ctx->getSqliteHandle());
  std::string two_axis_code = QuerySingleValue(
      sql_db,
      "SELECT code FROM ellipsoid WHERE auth_name = 'EPSG' AND "
      "inv_flattening IS NULL AND semi_major_axis != semi_minor_axis LIMIT 1");
  if (!two_axis_code.empty()) {
    auto ellps_two = epsg_factory->createEllipsoid(two_axis_code);
    EXPECT_TRUE(ellps_two->semiMinorAxis().has_value());
  }
  EXPECT_THROW(epsg_factory->createEllipsoid("99999999"),
               NoSuchAuthorityCodeException);

  // identifyBodyFromSemiMajorAxis
  EXPECT_EQ("Earth",
            epsg_factory->identifyBodyFromSemiMajorAxis(6378137.0, 1e-5));
  EXPECT_EQ("Earth",
            epsg_factory->identifyBodyFromSemiMajorAxis(6378137.0, 0.0));
  EXPECT_THROW(epsg_factory->identifyBodyFromSemiMajorAxis(1.0, 1e-9),
               FactoryException);
  EXPECT_THROW(epsg_factory->identifyBodyFromSemiMajorAxis(6378137.0, 100.0),
               FactoryException);
}

// ---------------------------------------------------------------------------
// AuthorityFactory: Datums, DatumEnsembles, CoordinateSystems, CRSs
// ---------------------------------------------------------------------------

TEST(FactoryTest, AuthorityFactoryDatumsCoordinateSystemsAndCRSs) {
  absl::Cleanup cleanup = []() {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());
  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
  auto epsg_factory = AuthorityFactory::create(db_ctx, "EPSG");
  auto esri_factory = AuthorityFactory::create(db_ctx, "ESRI");
  auto ogc_factory = AuthorityFactory::create(db_ctx, "OGC");
  auto iau_factory = AuthorityFactory::create(db_ctx, "IAU_2015");
  auto* sql_db = static_cast<sqlite3*>(db_ctx->getSqliteHandle());

  // Geodetic datums & ensembles:
  // "6269" (NAD83), "1061" (ITRF2008 dynamic), "6326" (WGS 84 ensemble).
  auto nad83_datum = epsg_factory->createGeodeticDatum("6269");
  EXPECT_NE(nullptr, nad83_datum);
  EXPECT_EQ(nad83_datum, epsg_factory->createGeodeticDatum("6269"));

  auto itrf2008 = epsg_factory->createGeodeticDatum("1061");
  EXPECT_NE(nullptr,
            dynamic_cast<const DynamicGeodeticReferenceFrame*>(itrf2008.get()));

  auto wgs84_ens = epsg_factory->createDatumEnsemble("6326");
  EXPECT_FALSE(wgs84_ens->datums().empty());
  // When turnEnsembleAsDatum = true (createGeodeticDatum), "6326" is returned
  // as a GeodeticReferenceFrame with massaged name.
  auto wgs84_as_datum = epsg_factory->createGeodeticDatum("6326");
  EXPECT_EQ("World Geodetic System 1984", wgs84_as_datum->nameStr());

  // Vertical datums & ensembles:
  auto navd88 = epsg_factory->createVerticalDatum("5103");
  EXPECT_NE(nullptr, navd88);
  std::string dyn_vert_code = QuerySingleValue(
      sql_db,
      "SELECT code FROM vertical_datum WHERE auth_name = 'EPSG' AND "
      "frame_reference_epoch IS NOT NULL LIMIT 1");
  if (!dyn_vert_code.empty()) {
    auto dyn_v = epsg_factory->createVerticalDatum(dyn_vert_code);
    EXPECT_NE(nullptr,
              dynamic_cast<const DynamicVerticalReferenceFrame*>(dyn_v.get()));
  }
  std::string vert_ens_code = QuerySingleValue(
      sql_db,
      "SELECT code FROM vertical_datum WHERE auth_name = 'EPSG' AND "
      "ensemble_accuracy IS NOT NULL LIMIT 1");
  if (!vert_ens_code.empty()) {
    auto v_ens =
        epsg_factory->createDatumEnsemble(vert_ens_code, "vertical_datum");
    EXPECT_FALSE(v_ens->datums().empty());
    // Requesting with wrong type throws NoSuchAuthorityCodeException.
    EXPECT_THROW(
        epsg_factory->createDatumEnsemble(vert_ens_code, "geodetic_datum"),
        NoSuchAuthorityCodeException);
  }
  std::string esri_vdatum_code = QuerySingleValue(
      sql_db,
      "SELECT code FROM vertical_datum WHERE auth_name = 'ESRI' AND "
      "code LIKE 'from_geogdatum_%' LIMIT 1");
  if (!esri_vdatum_code.empty()) {
    EXPECT_NE(nullptr, esri_factory->createVerticalDatum(esri_vdatum_code));
  }

  // createDatum dispatches across geodetic_datum and vertical_datum.
  EXPECT_NE(nullptr, epsg_factory->createDatum("6269"));
  EXPECT_NE(nullptr, epsg_factory->createDatum("5103"));
  EXPECT_THROW(epsg_factory->createDatum("99999999"),
               NoSuchAuthorityCodeException);
  EXPECT_THROW(epsg_factory->createGeodeticDatum("99999999"),
               NoSuchAuthorityCodeException);
  EXPECT_THROW(epsg_factory->createVerticalDatum("99999999"),
               NoSuchAuthorityCodeException);
  EXPECT_THROW(epsg_factory->createEngineeringDatum("99999999"),
               NoSuchAuthorityCodeException);
  EXPECT_THROW(epsg_factory->createDatumEnsemble("99999999"),
               NoSuchAuthorityCodeException);

  // CoordinateSystems:
  // Ellipsoidal 2D (6422), 3D (6423), Cartesian 2D (4400), 3D (6500),
  // Polar Cartesian with "North along ..." / "South along ..." (4482, 4483),
  // Spherical 3D (6404), Vertical (6499), Ordinal (4498).
  for (const char* cs_code : {"6422", "6423", "4400", "6500", "4482", "4483",
                              "6404", "6499", "4498"}) {
    auto cs = epsg_factory->createCoordinateSystem(cs_code);
    EXPECT_FALSE(cs->axisList().empty());
    // Cache hit on second call.
    EXPECT_EQ(cs, epsg_factory->createCoordinateSystem(cs_code));
  }
  EXPECT_THROW(epsg_factory->createCoordinateSystem("99999999"),
               NoSuchAuthorityCodeException);

  // GeodeticCRS & GeographicCRS:
  // 2D (4326), 3D (4979), Geocentric (4978), Planetocentric / spherical (IAU).
  // Note: because createGeodeticDatum("6326") was called earlier on db_ctx,
  // datum "EPSG6326" is cached as a GeodeticReferenceFrame in db_ctx. On a
  // fresh DatabaseContext, createGeographicCRS("4326") populates datumEnsemble.
  auto crs_4326 = epsg_factory->createGeographicCRS("4326");
  EXPECT_NE(nullptr, crs_4326->datum());
  auto crs_4979 = epsg_factory->createGeographicCRS("4979");
  EXPECT_EQ(3U, crs_4979->coordinateSystem()->axisList().size());
  auto crs_4978 = epsg_factory->createGeodeticCRS("4978");
  EXPECT_TRUE(crs_4978->isGeocentric());
  // Calling createGeographicCRS on a geocentric CRS throws; on a fresh
  // DatabaseContext, createGeographicCRS("4326") uses datumEnsemble().
  {
    auto fresh_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
    auto fresh_epsg = AuthorityFactory::create(fresh_ctx, "EPSG");
    EXPECT_NE(nullptr,
              fresh_epsg->createGeographicCRS("4326")->datumEnsemble());
    EXPECT_THROW(fresh_epsg->createGeographicCRS("4978"),
                 NoSuchAuthorityCodeException);
  }
  std::string spherical_crs_code = QuerySingleValue(
      sql_db,
      "SELECT code FROM geodetic_crs WHERE auth_name = 'IAU_2015' AND "
      "type = 'other' LIMIT 1");
  if (!spherical_crs_code.empty()) {
    EXPECT_NE(nullptr, iau_factory->createGeodeticCRS(spherical_crs_code));
  }

  // VerticalCRS, ProjectedCRS, CompoundCRS:
  auto vcrs_5703 = epsg_factory->createVerticalCRS("5703");
  EXPECT_EQ("NAVD88 height", vcrs_5703->nameStr());
  auto pcrs_32631 = epsg_factory->createProjectedCRS("32631");
  EXPECT_EQ("WGS 84 / UTM zone 31N", pcrs_32631->nameStr());
  auto ccrs_7405 = epsg_factory->createCompoundCRS("7405");
  EXPECT_EQ(2U, ccrs_7405->componentReferenceSystems().size());

  // Cross-type cache hit throws NoSuchAuthorityCodeException (e.g. requesting
  // cached GeodeticCRS "4326" via createVerticalCRS / createProjectedCRS /
  // createEngineeringCRS / createDerivedProjectedCRS).
  EXPECT_THROW(epsg_factory->createVerticalCRS("4326"),
               NoSuchAuthorityCodeException);
  EXPECT_THROW(epsg_factory->createProjectedCRS("4326"),
               NoSuchAuthorityCodeException);
  EXPECT_THROW(epsg_factory->createEngineeringCRS("4326"),
               NoSuchAuthorityCodeException);
  EXPECT_THROW(epsg_factory->createDerivedProjectedCRS("4326"),
               NoSuchAuthorityCodeException);
  EXPECT_THROW(epsg_factory->createGeodeticCRS("32631"),
               NoSuchAuthorityCodeException);

  // createCoordinateReferenceSystem for EPSG and OGC special codes.
  EXPECT_NE(nullptr, epsg_factory->createCoordinateReferenceSystem("4326"));
  EXPECT_NE(nullptr, epsg_factory->createCoordinateReferenceSystem("5703"));
  EXPECT_NE(nullptr, epsg_factory->createCoordinateReferenceSystem("32631"));
  EXPECT_NE(nullptr, epsg_factory->createCoordinateReferenceSystem("7405"));
  for (const char* ogc_code :
       {"AnsiDate", "JulianDate", "UnixTime", "84", "CRS84"}) {
    EXPECT_NE(nullptr, ogc_factory->createCoordinateReferenceSystem(ogc_code));
  }
  EXPECT_THROW(epsg_factory->createCoordinateReferenceSystem("99999999"),
               NoSuchAuthorityCodeException);

  // createCoordinateMetadata on existing DB rows (if any).
  std::string cm_code = QuerySingleValue(
      sql_db,
      "SELECT code FROM coordinate_metadata WHERE auth_name = 'EPSG' LIMIT 1");
  if (!cm_code.empty()) {
    EXPECT_NE(nullptr, epsg_factory->createCoordinateMetadata(cm_code));
  }
  EXPECT_THROW(epsg_factory->createCoordinateMetadata("99999999"),
               NoSuchAuthorityCodeException);
}

// ---------------------------------------------------------------------------
// Custom DB Rows: EngineeringCRS, DerivedProjectedCRS, text_definition,
// CoordinateMetadata, other_transformation (PROJString/WKT), CS edge cases
// ---------------------------------------------------------------------------

TEST(FactoryTest, CustomDatabaseRowsCoverAllRareBranches) {
  absl::Cleanup cleanup = []() {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());

  auto embedded_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
  TempDir dir("factory_custom_db");
  std::string custom_db_path = (dir.path() / "custom_proj.db").string();
  CloneEmbeddedDbToFile(embedded_ctx, custom_db_path, /*drop_triggers=*/true);

  {
    sqlite3* db = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(custom_db_path.c_str(), &db));
    absl::Cleanup close_db = [&db]() {
      if (db) sqlite3_close(db);
    };

    // 1) Engineering datum and engineering CRS.
    ExecSql(db,
            "INSERT INTO engineering_datum (auth_name, code, name, "
            "publication_date, anchor, anchor_epoch, deprecated) VALUES ("
            "'EPSG', '99001', 'Test Eng Datum', '2020-01-01', "
            "'Eng Anchor', '2020.0', 0);"
            "INSERT INTO engineering_crs (auth_name, code, name, description, "
            "coordinate_system_auth_name, coordinate_system_code, "
            "datum_auth_name, datum_code, deprecated) VALUES ("
            "'EPSG', '99002', 'Test Eng CRS', NULL, 'EPSG', '4400', "
            "'EPSG', '99001', 0);");

    // 2) DerivedProjectedCRS: one from base_crs/conversion and one from
    //    text_definition, plus one with invalid text_definition.
    std::string derived_proj_wkt =
        "DERIVEDPROJCRS[\"Test DerivedProj WKT\","
        "BASEPROJCRS[\"WGS 84 / UTM zone 31N\","
        "BASEGEOGCRS[\"WGS 84\","
        "DATUM[\"World Geodetic System 1984\","
        "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
        "PRIMEM[\"Greenwich\",0,ANGLEUNIT[\"degree\",0.0174532925199433]]],"
        "CONVERSION[\"UTM zone 31N\","
        "METHOD[\"Transverse Mercator\",ID[\"EPSG\",9807]],"
        "PARAMETER[\"Latitude of natural "
        "origin\",0,ANGLEUNIT[\"degree\",0.0174532925199433],ID[\"EPSG\",8801]]"
        ","
        "PARAMETER[\"Longitude of natural "
        "origin\",3,ANGLEUNIT[\"degree\",0.0174532925199433],ID[\"EPSG\",8802]]"
        ","
        "PARAMETER[\"Scale factor at natural "
        "origin\",0.9996,SCALEUNIT[\"unity\",1],ID[\"EPSG\",8805]],"
        "PARAMETER[\"False "
        "easting\",500000,LENGTHUNIT[\"metre\",1],ID[\"EPSG\",8806]],"
        "PARAMETER[\"False "
        "northing\",0,LENGTHUNIT[\"metre\",1],ID[\"EPSG\",8807]]]],"
        "DERIVINGCONVERSION[\"Affine\","
        "METHOD[\"Affine parametric transformation\",ID[\"EPSG\",9624]],"
        "PARAMETER[\"A0\",0,LENGTHUNIT[\"metre\",1],ID[\"EPSG\",8623]],"
        "PARAMETER[\"A1\",1,SCALEUNIT[\"unity\",1],ID[\"EPSG\",8624]],"
        "PARAMETER[\"A2\",0,SCALEUNIT[\"unity\",1],ID[\"EPSG\",8625]],"
        "PARAMETER[\"B0\",0,LENGTHUNIT[\"metre\",1],ID[\"EPSG\",8639]],"
        "PARAMETER[\"B1\",0,SCALEUNIT[\"unity\",1],ID[\"EPSG\",8640]],"
        "PARAMETER[\"B2\",1,SCALEUNIT[\"unity\",1],ID[\"EPSG\",8641]]],"
        "CS[Cartesian,2],"
        "AXIS[\"(E)\",east,ORDER[1],LENGTHUNIT[\"metre\",1]],"
        "AXIS[\"(N)\",north,ORDER[2],LENGTHUNIT[\"metre\",1]]]";
    ExecSql(db,
            "INSERT INTO conversion_table (auth_name, code, name, description, "
            "method_auth_name, method_code, "
            "param1_auth_name, param1_code, param1_value, "
            "param1_uom_auth_name, param1_uom_code, deprecated) VALUES ("
            "'EPSG', '99010', 'unnamed', 'test conv', "
            "'EPSG', '9624', "
            "'EPSG', '8623', 0.0, 'EPSG', '9001', 0);"
            "INSERT INTO derived_projected_crs (auth_name, code, name, "
            "description, coordinate_system_auth_name, coordinate_system_code, "
            "base_crs_auth_name, base_crs_code, conversion_auth_name, "
            "conversion_code, text_definition, deprecated) VALUES ("
            "'EPSG', '99003', 'Test DerivedProj DB', NULL, 'EPSG', '4400', "
            "'EPSG', '32631', 'EPSG', '99010', NULL, 0);"
            "INSERT INTO derived_projected_crs (auth_name, code, name, "
            "description, coordinate_system_auth_name, coordinate_system_code, "
            "base_crs_auth_name, base_crs_code, conversion_auth_name, "
            "conversion_code, text_definition, deprecated) VALUES ("
            "'EPSG', '99004', 'Test DerivedProj WKT', NULL, NULL, NULL, "
            "NULL, NULL, NULL, NULL, '" +
                derived_proj_wkt +
                "', 0);"
                "INSERT INTO derived_projected_crs (auth_name, code, name, "
                "description, coordinate_system_auth_name, "
                "coordinate_system_code, base_crs_auth_name, base_crs_code, "
                "conversion_auth_name, conversion_code, text_definition, "
                "deprecated) VALUES ("
                "'EPSG', '99005', 'Bad DerivedProj', NULL, NULL, NULL, "
                "NULL, NULL, NULL, NULL, 'EPSG:4326', 0);");

    // 3) GeodeticCRS with text_definition: GeogCRS, Geocentric, BoundCRS
    //    (+towgs84), and invalid (ProjectedCRS).
    ExecSql(db,
            "INSERT INTO geodetic_crs (auth_name, code, name, description, "
            "type, coordinate_system_auth_name, coordinate_system_code, "
            "datum_auth_name, datum_code, text_definition, deprecated) VALUES ("
            "'EPSG', '99011', 'Text Geog2D', 'remark1', 'geographic 2D', NULL, "
            "NULL, NULL, NULL, '+proj=longlat +ellps=WGS84', 0),"
            "('EPSG', '99012', 'Text Geocentric', NULL, 'geocentric', NULL, "
            "NULL, NULL, NULL, '+proj=cart +ellps=WGS84', 0),"
            "('EPSG', '99013', 'Text Bound Geog', NULL, 'geographic 2D', NULL, "
            "NULL, NULL, NULL, '+proj=longlat +ellps=GRS80 +towgs84=1,2,3', 0),"
            "('EPSG', '99014', 'Text Bad Geog', NULL, 'geographic 2D', NULL, "
            "NULL, NULL, NULL, 'EPSG:32631', 0);");

    // 4) ProjectedCRS with text_definition: regular, BoundCRS (+towgs84), and
    //    invalid (GeogCRS).
    ExecSql(
        db,
        "INSERT INTO projected_crs (auth_name, code, name, description, "
        "coordinate_system_auth_name, coordinate_system_code, "
        "geodetic_crs_auth_name, geodetic_crs_code, conversion_auth_name, "
        "conversion_code, text_definition, deprecated) VALUES ("
        "'EPSG', '99021', 'Text Proj', NULL, NULL, NULL, NULL, NULL, NULL, "
        "NULL, '+proj=utm +zone=31 +datum=WGS84', 0),"
        "('EPSG', '99022', 'Text Bound Proj', NULL, NULL, NULL, NULL, NULL, "
        "NULL, NULL, '+proj=utm +zone=31 +ellps=GRS80 +towgs84=1,2,3', 0),"
        "('EPSG', '99023', 'Text Bad Proj', NULL, NULL, NULL, NULL, NULL, "
        "NULL, NULL, 'EPSG:4326', 0);");

    // 5) CoordinateMetadata: crs_auth_name+epoch, crs_text_definition without
    //    epoch, and invalid crs_text_definition.
    ExecSql(db,
            "INSERT INTO coordinate_metadata (auth_name, code, description, "
            "crs_auth_name, crs_code, crs_text_definition, coordinate_epoch, "
            "deprecated) VALUES ("
            "'EPSG', '99031', NULL, 'EPSG', '4979', NULL, 2021.5, 0),"
            "('EPSG', '99032', NULL, NULL, NULL, 'EPSG:4326', NULL, 0),"
            "('EPSG', '99033', NULL, NULL, NULL, '+proj=noop', NULL, 0);");

    // 6) other_transformation with PROJ:PROJString and PROJ:WKT methods.
    std::string op_wkt =
        "COORDINATEOPERATION[\"Test WKT Op\","
        "SOURCECRS[GEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\","
        "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
        "PRIMEM[\"Greenwich\",0,ANGLEUNIT[\"degree\",0.0174532925199433]],"
        "CS[ellipsoidal,2],"
        "AXIS[\"latitude\",north,ORDER[1],ANGLEUNIT[\"degree\",0."
        "0174532925199433]],"
        "AXIS[\"longitude\",east,ORDER[2],ANGLEUNIT[\"degree\",0."
        "0174532925199433]]]],"
        "TARGETCRS[GEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\","
        "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
        "PRIMEM[\"Greenwich\",0,ANGLEUNIT[\"degree\",0.0174532925199433]],"
        "CS[ellipsoidal,2],"
        "AXIS[\"latitude\",north,ORDER[1],ANGLEUNIT[\"degree\",0."
        "0174532925199433]],"
        "AXIS[\"longitude\",east,ORDER[2],ANGLEUNIT[\"degree\",0."
        "0174532925199433]]]],"
        "METHOD[\"Geographic2D offsets\",ID[\"EPSG\",9619]],"
        "PARAMETER[\"Latitude "
        "offset\",0,ANGLEUNIT[\"degree\",0.0174532925199433],ID[\"EPSG\",8601]]"
        ","
        "PARAMETER[\"Longitude "
        "offset\",0,ANGLEUNIT[\"degree\",0.0174532925199433],ID[\"EPSG\",8602]]"
        "]";
    ExecSql(
        db,
        "INSERT INTO other_transformation (auth_name, code, name, "
        "description, method_auth_name, method_code, method_name, "
        "source_crs_auth_name, source_crs_code, target_crs_auth_name, "
        "target_crs_code, accuracy, deprecated) VALUES "
        "('EPSG', '99041', 'Test PROJString Op', 'desc', 'PROJ', "
        "'PROJString', '+proj=noop', 'EPSG', '4326', 'EPSG', '4258', 1.0, 0),"
        "('EPSG', '99042', 'Test WKT Op', 'desc', 'PROJ', "
        "'WKT', '" +
            op_wkt +
            "', 'EPSG', '4326', 'EPSG', '4258', 1.0, 0),"
            "('EPSG', '99043', 'Test Bad WKT Op', 'desc', 'PROJ', "
            "'WKT', 'EPSG:4326', 'EPSG', '4326', 'EPSG', '4258', 1.0, 0);");

    // 7) Spherical 2D CS and invalid CS entry (vertical CS with 0 axes).
    ExecSql(db,
            "INSERT INTO coordinate_system (auth_name, code, type, dimension) "
            "VALUES ('EPSG', '99051', 'spherical', 2), "
            "('EPSG', '99052', 'vertical', 1);"
            "INSERT INTO axis (auth_name, code, name, abbrev, orientation, "
            "coordinate_system_auth_name, coordinate_system_code, "
            "coordinate_system_order, uom_auth_name, uom_code) VALUES "
            "('EPSG', '990511', 'Planetocentric latitude', 'lat', 'north', "
            "'EPSG', '99051', 1, 'EPSG', '9102'),"
            "('EPSG', '990512', 'Planetocentric longitude', 'lon', 'east', "
            "'EPSG', '99051', 2, 'EPSG', '9102');");
  }

  auto db_ctx = DatabaseContext::create(custom_db_path, {}, ctx.get());
  auto epsg_factory = AuthorityFactory::create(db_ctx, "EPSG");

  // 1) EngineeringDatum and EngineeringCRS
  auto eng_datum = epsg_factory->createEngineeringDatum("99001");
  EXPECT_EQ("Test Eng Datum", eng_datum->nameStr());
  EXPECT_EQ("Test Eng Datum", epsg_factory->createDatum("99001")->nameStr());
  EXPECT_EQ("Test Eng Datum", nn_dynamic_pointer_cast<IdentifiedObject>(
                                  epsg_factory->createObject("99001"))
                                  ->nameStr());

  auto eng_crs = epsg_factory->createEngineeringCRS("99002");
  EXPECT_EQ("Test Eng CRS", eng_crs->nameStr());
  EXPECT_EQ(eng_crs, epsg_factory->createEngineeringCRS("99002"));
  EXPECT_EQ(eng_crs, epsg_factory->createCoordinateReferenceSystem("99002"));
  EXPECT_EQ("Test Eng CRS", nn_dynamic_pointer_cast<IdentifiedObject>(
                                epsg_factory->createObject("99002"))
                                ->nameStr());

  // 2) DerivedProjectedCRS
  auto dproj_db = epsg_factory->createDerivedProjectedCRS("99003");
  EXPECT_EQ("Test DerivedProj DB", dproj_db->nameStr());
  EXPECT_EQ(dproj_db, epsg_factory->createDerivedProjectedCRS("99003"));
  EXPECT_EQ(dproj_db, epsg_factory->createCoordinateReferenceSystem("99003"));
  EXPECT_EQ("Test DerivedProj DB", nn_dynamic_pointer_cast<IdentifiedObject>(
                                       epsg_factory->createObject("99003"))
                                       ->nameStr());

  auto dproj_wkt = epsg_factory->createDerivedProjectedCRS("99004");
  EXPECT_EQ("Test DerivedProj WKT", dproj_wkt->nameStr());
  EXPECT_THROW(epsg_factory->createDerivedProjectedCRS("99005"),
               FactoryException);

  // 3) GeodeticCRS from text_definition
  EXPECT_EQ("Text Geog2D", epsg_factory->createGeodeticCRS("99011")->nameStr());
  EXPECT_EQ("Text Geocentric",
            epsg_factory->createGeodeticCRS("99012")->nameStr());
  EXPECT_EQ("Text Bound Geog",
            epsg_factory->createGeodeticCRS("99013")->nameStr());
  EXPECT_THROW(epsg_factory->createGeodeticCRS("99014"), FactoryException);

  // 4) ProjectedCRS from text_definition
  EXPECT_EQ("Text Proj", epsg_factory->createProjectedCRS("99021")->nameStr());
  EXPECT_EQ("Text Bound Proj",
            epsg_factory->createProjectedCRS("99022")->nameStr());
  EXPECT_THROW(epsg_factory->createProjectedCRS("99023"), FactoryException);

  // 5) CoordinateMetadata
  auto cm1 = epsg_factory->createCoordinateMetadata("99031");
  EXPECT_NEAR(2021.5, cm1->coordinateEpochAsDecimalYear(), 1e-6);
  EXPECT_THROW(epsg_factory->createObject("99031"),
               NoSuchAuthorityCodeException);
  auto cm2 = epsg_factory->createCoordinateMetadata("99032");
  EXPECT_FALSE(cm2->coordinateEpoch().has_value());
  EXPECT_THROW(epsg_factory->createCoordinateMetadata("99033"),
               FactoryException);

  // 6) other_transformation with PROJString & WKT
  auto op_proj = epsg_factory->createCoordinateOperation("99041", false);
  EXPECT_EQ("Test PROJString Op", op_proj->nameStr());
  auto op_wkt_obj = epsg_factory->createCoordinateOperation("99042", false);
  EXPECT_NE(nullptr, op_wkt_obj);
  EXPECT_THROW(epsg_factory->createCoordinateOperation("99043", false),
               FactoryException);

  // 7) Spherical 2D CS & invalid Vertical 2D CS
  auto sph_2d = epsg_factory->createCoordinateSystem("99051");
  EXPECT_EQ(2U, sph_2d->axisList().size());
  EXPECT_THROW(epsg_factory->createCoordinateSystem("99052"), FactoryException);
}

// ---------------------------------------------------------------------------
// AuthorityFactory: CoordinateOperations & createObject across all tables
// ---------------------------------------------------------------------------

TEST(FactoryTest, AuthorityFactoryCoordinateOperationsAndCreateObject) {
  absl::Cleanup cleanup = []() {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());
  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
  auto epsg_factory = AuthorityFactory::create(db_ctx, "EPSG");
  auto* sql_db = static_cast<sqlite3*>(db_ctx->getSqliteHandle());

  // Conversions:
  // Regular conversion in conversion table (16031).
  auto conv_16031 = epsg_factory->createConversion("16031");
  EXPECT_EQ("UTM zone 31N", conv_16031->nameStr());
  // Conversion stored in other_transformation (Change of Vertical Unit / Height
  // Depth Reversal).
  std::string other_conv_code = QuerySingleValue(
      sql_db,
      "SELECT code FROM other_transformation WHERE auth_name = 'EPSG' AND "
      "method_code IN ('1104', '1068', '1069') LIMIT 1");
  if (!other_conv_code.empty()) {
    auto other_conv = epsg_factory->createConversion(other_conv_code);
    EXPECT_NE(nullptr, other_conv);
  }
  EXPECT_THROW(epsg_factory->createConversion("99999999"),
               NoSuchAuthorityCodeException);

  // createCoordinateOperation:
  // 1) conversion ("16031")
  EXPECT_NE(nullptr, epsg_factory->createCoordinateOperation("16031", false));

  // 2) helmert_transformation:
  //    - 3-parameter ("1172")
  //    - 7-parameter ("1188")
  //    - 8-parameter (rx != NULL, rate_tx IS NULL, epoch IS NOT NULL)
  //    - 15-parameter (rate_tx IS NOT NULL)
  //    - 10-parameter Molodensky-Badekas (px IS NOT NULL)
  EXPECT_NE(nullptr, epsg_factory->createCoordinateOperation("1172", false));
  EXPECT_NE(nullptr, epsg_factory->createCoordinateOperation("1188", false));
  for (const char* where_clause : {
           "rx IS NOT NULL AND rate_tx IS NULL AND epoch IS NOT NULL",
           "rate_tx IS NOT NULL",
           "px IS NOT NULL",
       }) {
    std::string code = QuerySingleValue(
        sql_db,
        std::string("SELECT code FROM helmert_transformation WHERE auth_name = "
                    "'EPSG' AND ") +
            where_clause + " LIMIT 1");
    if (!code.empty()) {
      EXPECT_NE(nullptr, epsg_factory->createCoordinateOperation(code, false));
    }
  }

  // 3) grid_transformation:
  //    - 1-grid ("15851") with usePROJAlternativeGridNames = false and true
  //    - 2-grid (grid2_name IS NOT NULL)
  //    - with param1_value IS NOT NULL
  //    - with interpolation_crs_code IS NOT NULL
  //    - Point motion operation (method_name LIKE 'Point motion%')
  EXPECT_NE(nullptr, epsg_factory->createCoordinateOperation("15851", false));
  EXPECT_NE(nullptr, epsg_factory->createCoordinateOperation("15851", true));
  for (const char* where_clause : {
           "grid2_name IS NOT NULL",
           "param1_name IS NOT NULL",
           "interpolation_crs_code IS NOT NULL",
           "method_name LIKE 'Point motion%'",
       }) {
    std::string code = QuerySingleValue(
        sql_db,
        std::string("SELECT code FROM grid_transformation WHERE auth_name = "
                    "'EPSG' AND ") +
            where_clause + " LIMIT 1");
    if (!code.empty()) {
      EXPECT_NE(nullptr, epsg_factory->createCoordinateOperation(code, false));
      EXPECT_NE(nullptr, epsg_factory->createCoordinateOperation(code, true));
    }
  }

  // 4) other_transformation:
  //    - with grid_name IS NOT NULL
  //    - with param1_name IS NOT NULL
  //    - with interpolation_crs_code IS NOT NULL
  for (const char* where_clause : {
           "grid_name IS NOT NULL",
           "param1_name IS NOT NULL",
           "interpolation_crs_code IS NOT NULL",
       }) {
    std::string code = QuerySingleValue(
        sql_db,
        std::string("SELECT code FROM other_transformation WHERE auth_name = "
                    "'EPSG' AND ") +
            where_clause + " LIMIT 1");
    if (!code.empty()) {
      EXPECT_NE(nullptr, epsg_factory->createCoordinateOperation(code, false));
      EXPECT_NE(nullptr, epsg_factory->createCoordinateOperation(code, true));
    }
  }

  // 5) concatenated_operation:
  //    - with accuracy IS NOT NULL
  //    - with accuracy IS NULL
  //    - with explicit step_direction
  for (const char* where_clause : {
           "accuracy IS NOT NULL",
           "accuracy IS NULL",
       }) {
    std::string code = QuerySingleValue(
        sql_db,
        std::string("SELECT code FROM concatenated_operation WHERE auth_name = "
                    "'EPSG' AND ") +
            where_clause + " LIMIT 1");
    if (!code.empty()) {
      EXPECT_NE(nullptr, epsg_factory->createCoordinateOperation(code, false));
      EXPECT_NE(nullptr, epsg_factory->createCoordinateOperation(code, true));
    }
  }
  auto nkg_factory = AuthorityFactory::create(db_ctx, "NKG");
  EXPECT_NE(nullptr,
            nkg_factory->createCoordinateOperation("ITRF2000_TO_DK", false));
  EXPECT_THROW(epsg_factory->createCoordinateOperation("99999999", false),
               NoSuchAuthorityCodeException);

  // createObject: test unique codes across tables in EPSG and ESRI.
  for (const char* code : {
           "1589",  // extent
           "6303",  // geodetic_datum
           "4882",  // geodetic_crs (geocentric)
           "4804",  // geodetic_crs (geographic 2D)
           "4883",  // geodetic_crs (geographic 3D)
           "5237",  // vertical_crs
           "5000",  // conversion
           "4873",  // projected_crs
           "5318",  // compound_crs
           "5800",  // engineering_crs
           "4905",  // helmert_transformation
           "5334",  // grid_transformation
           "5045",  // other_transformation
           "5230",  // concatenated_operation
       }) {
    EXPECT_NE(nullptr, epsg_factory->createObject(code)) << "Code: " << code;
  }
  auto esri_factory = AuthorityFactory::create(db_ctx, "ESRI");
  for (const char* code : {
           "109002",  // unit_of_measure
           "108900",  // prime_meridian
           "7023",    // ellipsoid
           "105100",  // vertical_datum
       }) {
    EXPECT_NE(nullptr, esri_factory->createObject(code)) << "Code: " << code;
  }
  // axis ("1") in object_view hits "unimplemented factory for axis".
  EXPECT_THROW(epsg_factory->createObject("1"), FactoryException);
  EXPECT_THROW(epsg_factory->createObject("99999999"),
               NoSuchAuthorityCodeException);
}

// ---------------------------------------------------------------------------
// AuthorityFactory: getAuthorityCodes, getDescriptionText, getGeoidModels,
// getCRSInfoList, getUnitList, getCelestialBodyList, listAreaOfUseFromName
// ---------------------------------------------------------------------------

TEST(FactoryTest, AuthorityFactoryIntrospectionAndMetadataLists) {
  absl::Cleanup cleanup = []() {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());
  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
  auto epsg_factory = AuthorityFactory::create(db_ctx, "EPSG");
  auto any_factory = AuthorityFactory::create(db_ctx, "");

  // getAuthorityCodes across all 24 ObjectType values.
  constexpr AuthorityFactory::ObjectType kAllObjectTypes[] = {
      AuthorityFactory::ObjectType::PRIME_MERIDIAN,
      AuthorityFactory::ObjectType::ELLIPSOID,
      AuthorityFactory::ObjectType::DATUM,
      AuthorityFactory::ObjectType::GEODETIC_REFERENCE_FRAME,
      AuthorityFactory::ObjectType::DYNAMIC_GEODETIC_REFERENCE_FRAME,
      AuthorityFactory::ObjectType::VERTICAL_REFERENCE_FRAME,
      AuthorityFactory::ObjectType::DYNAMIC_VERTICAL_REFERENCE_FRAME,
      AuthorityFactory::ObjectType::DATUM_ENSEMBLE,
      AuthorityFactory::ObjectType::CRS,
      AuthorityFactory::ObjectType::GEODETIC_CRS,
      AuthorityFactory::ObjectType::GEOCENTRIC_CRS,
      AuthorityFactory::ObjectType::GEOGRAPHIC_CRS,
      AuthorityFactory::ObjectType::GEOGRAPHIC_2D_CRS,
      AuthorityFactory::ObjectType::GEOGRAPHIC_3D_CRS,
      AuthorityFactory::ObjectType::PROJECTED_CRS,
      AuthorityFactory::ObjectType::DERIVED_PROJECTED_CRS,
      AuthorityFactory::ObjectType::VERTICAL_CRS,
      AuthorityFactory::ObjectType::COMPOUND_CRS,
      AuthorityFactory::ObjectType::COORDINATE_OPERATION,
      AuthorityFactory::ObjectType::CONVERSION,
      AuthorityFactory::ObjectType::TRANSFORMATION,
      AuthorityFactory::ObjectType::CONCATENATED_OPERATION,
      AuthorityFactory::ObjectType::ENGINEERING_DATUM,
      AuthorityFactory::ObjectType::ENGINEERING_CRS,
  };
  for (auto obj_type : kAllObjectTypes) {
    auto codes_non_dep = epsg_factory->getAuthorityCodes(obj_type, false);
    auto codes_with_dep = epsg_factory->getAuthorityCodes(obj_type, true);
    EXPECT_GE(codes_with_dep.size(), codes_non_dep.size());
  }
  // getAuthorityCodes always filters WHERE auth_name = d->authority(), so an
  // unrestricted ("") factory matches auth_name = '' (empty).
  EXPECT_TRUE(any_factory
                  ->getAuthorityCodes(
                      AuthorityFactory::ObjectType::PRIME_MERIDIAN, false)
                  .empty());

  // getDescriptionText: CRS priority ("4326", "8901" which is both Greenwich PM
  // and RGWF96 CRS), non-CRS ("7030" WGS 84 ellipsoid), and missing.
  EXPECT_EQ("WGS 84", epsg_factory->getDescriptionText("4326"));
  EXPECT_EQ("RGWF96 (lon-lat)", epsg_factory->getDescriptionText("8901"));
  EXPECT_EQ("WGS 84", epsg_factory->getDescriptionText("7030"));
  EXPECT_THROW(epsg_factory->getDescriptionText("99999999"),
               NoSuchAuthorityCodeException);

  // getGeoidModels
  EXPECT_FALSE(epsg_factory->getGeoidModels("5703").empty());
  EXPECT_FALSE(any_factory->getGeoidModels("5703").empty());

  // getCRSInfoList on EPSG and unrestricted factory.
  auto epsg_crs_info = epsg_factory->getCRSInfoList();
  EXPECT_FALSE(epsg_crs_info.empty());
  auto any_crs_info = any_factory->getCRSInfoList();
  EXPECT_GT(any_crs_info.size(), epsg_crs_info.size());

  // getUnitList on EPSG and unrestricted factory.
  auto epsg_units = epsg_factory->getUnitList();
  EXPECT_FALSE(epsg_units.empty());
  auto any_units = any_factory->getUnitList();
  EXPECT_GE(any_units.size(), epsg_units.size());

  // getCelestialBodyList on EPSG (0 rows: bodies use auth_name PROJ/IAU_2015),
  // PROJ, and unrestricted factory.
  auto epsg_bodies = epsg_factory->getCelestialBodyList();
  EXPECT_TRUE(epsg_bodies.empty());
  auto proj_bodies =
      AuthorityFactory::create(db_ctx, "PROJ")->getCelestialBodyList();
  EXPECT_FALSE(proj_bodies.empty());
  auto any_bodies = any_factory->getCelestialBodyList();
  EXPECT_GT(any_bodies.size(), proj_bodies.size());

  // listAreaOfUseFromName (approximate and exact, restricted and unrestricted).
  EXPECT_FALSE(epsg_factory->listAreaOfUseFromName("France", true).empty());
  EXPECT_FALSE(any_factory->listAreaOfUseFromName("France", true).empty());
  EXPECT_FALSE(epsg_factory->listAreaOfUseFromName("World", false).empty());
  EXPECT_FALSE(any_factory->listAreaOfUseFromName("World", false).empty());

  // getOfficialNameFromAlias (exact and equivalent spelling).
  std::string out_table;
  std::string out_auth;
  std::string out_code;
  EXPECT_EQ("WGS 84", epsg_factory->getOfficialNameFromAlias(
                          "GCS_WGS_1984", "geodetic_crs", "ESRI", false,
                          out_table, out_auth, out_code));
  EXPECT_EQ("WGS 84",
            epsg_factory->getOfficialNameFromAlias(
                "GCS_WGS_1984", "", "", true, out_table, out_auth, out_code));
  EXPECT_FALSE(epsg_factory
                   ->getOfficialNameFromAlias("D_North_American_1983",
                                              "geodetic_datum", "", true,
                                              out_table, out_auth, out_code)
                   .empty());
  EXPECT_TRUE(epsg_factory
                  ->getOfficialNameFromAlias("Lambert_Conformal_Conic_2SP",
                                             "conversion_method", "", true,
                                             out_table, out_auth, out_code)
                  .empty());
  EXPECT_TRUE(epsg_factory
                  ->getOfficialNameFromAlias("NON_EXISTENT_ALIAS_XYZ", "", "",
                                             true, out_table, out_auth,
                                             out_code)
                  .empty());

  // getPointMotionOperationsFor
  auto nad83_csrs_v7 = epsg_factory->createGeodeticCRS("8255");
  auto pmos_official =
      any_factory->getPointMotionOperationsFor(nad83_csrs_v7, false);
  auto pmos_proj =
      any_factory->getPointMotionOperationsFor(nad83_csrs_v7, true);
  EXPECT_EQ(pmos_official.size(), pmos_proj.size());
}

// ---------------------------------------------------------------------------
// AuthorityFactory: createObjectsFromName
// ---------------------------------------------------------------------------

TEST(FactoryTest, CreateObjectsFromNameAllBranches) {
  absl::Cleanup cleanup = []() {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());
  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
  auto epsg_factory = AuthorityFactory::create(db_ctx, "EPSG");
  auto any_factory = AuthorityFactory::create(db_ctx, "");

  // Short string (< 2 chars) with approximateMatch = true returns empty.
  EXPECT_TRUE(epsg_factory->createObjectsFromName("a", {}, true).empty());

  // Special GEODETIC_REFERENCE_FRAME fast path on unrestricted factory with
  // approximateMatch = true (exercises mapCanonicalizeGRFName exact &
  // substring).
  auto grf_exact = any_factory->createObjectsFromName(
      "World Geodetic System 1984",
      {AuthorityFactory::ObjectType::GEODETIC_REFERENCE_FRAME}, true, 5);
  EXPECT_FALSE(grf_exact.empty());
  auto grf_substr = any_factory->createObjectsFromName(
      "Geodetic System 1984",
      {AuthorityFactory::ObjectType::GEODETIC_REFERENCE_FRAME}, true, 5);
  EXPECT_FALSE(grf_substr.empty());

  // " (deprecated)" suffix searches deprecated objects.
  auto dep_objs = epsg_factory->createObjectsFromName(
      "WGS 84 (deprecated)", {AuthorityFactory::ObjectType::CRS}, true, 5);
  (void)dep_objs;

  // "D_" prefix hides vertical datums when searching DATUM.
  auto d_prefix_objs = any_factory->createObjectsFromName(
      "D_WGS_1984", {AuthorityFactory::ObjectType::DATUM}, true, 5);
  EXPECT_FALSE(d_prefix_objs.empty());

  // Exact match (approximateMatch = false) vs approximate match across all
  // allowedObjectTypes.
  EXPECT_FALSE(epsg_factory
                   ->createObjectsFromName(
                       "WGS 84", {AuthorityFactory::ObjectType::CRS}, false, 5)
                   .empty());
  EXPECT_FALSE(
      epsg_factory->createObjectsFromName("WGS 84", {}, true, 10).empty());

  constexpr AuthorityFactory::ObjectType kSearchTypes[] = {
      AuthorityFactory::ObjectType::PRIME_MERIDIAN,
      AuthorityFactory::ObjectType::ELLIPSOID,
      AuthorityFactory::ObjectType::DATUM,
      AuthorityFactory::ObjectType::GEODETIC_REFERENCE_FRAME,
      AuthorityFactory::ObjectType::DYNAMIC_GEODETIC_REFERENCE_FRAME,
      AuthorityFactory::ObjectType::VERTICAL_REFERENCE_FRAME,
      AuthorityFactory::ObjectType::DYNAMIC_VERTICAL_REFERENCE_FRAME,
      AuthorityFactory::ObjectType::DATUM_ENSEMBLE,
      AuthorityFactory::ObjectType::CRS,
      AuthorityFactory::ObjectType::GEODETIC_CRS,
      AuthorityFactory::ObjectType::GEOCENTRIC_CRS,
      AuthorityFactory::ObjectType::GEOGRAPHIC_CRS,
      AuthorityFactory::ObjectType::GEOGRAPHIC_2D_CRS,
      AuthorityFactory::ObjectType::GEOGRAPHIC_3D_CRS,
      AuthorityFactory::ObjectType::PROJECTED_CRS,
      AuthorityFactory::ObjectType::DERIVED_PROJECTED_CRS,
      AuthorityFactory::ObjectType::VERTICAL_CRS,
      AuthorityFactory::ObjectType::COMPOUND_CRS,
      AuthorityFactory::ObjectType::COORDINATE_OPERATION,
      AuthorityFactory::ObjectType::CONVERSION,
      AuthorityFactory::ObjectType::TRANSFORMATION,
      AuthorityFactory::ObjectType::CONCATENATED_OPERATION,
      AuthorityFactory::ObjectType::ENGINEERING_DATUM,
      AuthorityFactory::ObjectType::ENGINEERING_CRS,
  };
  for (auto t : kSearchTypes) {
    (void)any_factory->createObjectsFromName("WGS", {t}, true, 2);
  }
}

// ---------------------------------------------------------------------------
// AuthorityFactory: createFromCoordinateReferenceSystemCodes & Intermediates
// ---------------------------------------------------------------------------

TEST(FactoryTest, CreateFromCRSCodesDirectAndWithIntermediates) {
  absl::Cleanup cleanup = []() {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());
  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
  auto epsg_factory = AuthorityFactory::create(db_ctx, "EPSG");
  auto any_factory = AuthorityFactory::create(db_ctx, "");

  // 2-argument overload: ProjectedCRS from its base GeographicCRS, and
  // transformation between two GeographicCRSs (NAD27 -> NAD83 forward via
  // 2-arg overload vs NAD83 -> NAD27 reversed via 12-arg tryReverseOrder=true).
  auto ops_4326_32631 =
      epsg_factory->createFromCoordinateReferenceSystemCodes("4326", "32631");
  EXPECT_EQ(1U, ops_4326_32631.size());
  auto ops_32631_4326_no_rev =
      epsg_factory->createFromCoordinateReferenceSystemCodes("32631", "4326");
  EXPECT_TRUE(ops_32631_4326_no_rev.empty());
  auto ops_4269_4267_no_rev =
      epsg_factory->createFromCoordinateReferenceSystemCodes("4269", "4267");
  EXPECT_TRUE(ops_4269_4267_no_rev.empty());
  auto ops_4269_4267_rev =
      epsg_factory->createFromCoordinateReferenceSystemCodes(
          "EPSG", "4269", "EPSG", "4267", false, false, false, true, true,
          false, nullptr, nullptr);
  EXPECT_FALSE(ops_4269_4267_rev.empty());
  auto ops_4267_4269 =
      epsg_factory->createFromCoordinateReferenceSystemCodes("4267", "4269");
  EXPECT_FALSE(ops_4267_4269.empty());

  // Full overload with various flags:
  // - empty source or target auth name
  // - discardIfMissingGrid = true/false
  // - considerKnownGridsAsAvailable = true/false
  // - discardSuperseded = true/false
  // - tryReverseOrder = true/false
  // - reportOnlyIntersectingTransformations with intersecting and disjoint
  // extents
  EXPECT_FALSE(any_factory
                   ->createFromCoordinateReferenceSystemCodes(
                       "", "4267", "EPSG", "4269", false, false, false, false,
                       false, false, nullptr, nullptr)
                   .empty());
  EXPECT_FALSE(any_factory
                   ->createFromCoordinateReferenceSystemCodes(
                       "EPSG", "4267", "", "4269", true, true, true, true, true,
                       false, nullptr, nullptr)
                   .empty());

  auto us_extent = epsg_factory->createExtent("1262");
  auto small_antarctic_bbox =
      GeographicBoundingBox::create(-179.0, -89.0, -178.0, -88.0);
  auto antarctic_extent = Extent::create(
      optional<std::string>("Antarctic patch"), {small_antarctic_bbox}, {}, {});

  auto ops_intersecting = any_factory->createFromCoordinateReferenceSystemCodes(
      "EPSG", "4267", "EPSG", "4269", false, false, false, true, true, true,
      us_extent, us_extent);
  EXPECT_FALSE(ops_intersecting.empty());

  auto ops_disjoint = any_factory->createFromCoordinateReferenceSystemCodes(
      "EPSG", "4267", "EPSG", "4269", false, false, false, true, true, true,
      antarctic_extent, antarctic_extent);
  EXPECT_TRUE(ops_disjoint.empty());

  // createFromCRSCodesWithIntermediates:
  // 1) Same source & target -> empty.
  EXPECT_TRUE(any_factory
                  ->createFromCRSCodesWithIntermediates(
                      "EPSG", "4326", "EPSG", "4326", false, false, false, true,
                      {}, AuthorityFactory::ObjectType::GEOGRAPHIC_CRS, {},
                      nullptr, nullptr)
                  .empty());

  // 2) Non-existent CRS -> empty.
  EXPECT_TRUE(any_factory
                  ->createFromCRSCodesWithIntermediates(
                      "EPSG", "99999999", "EPSG", "4326", false, false, false,
                      true, {}, AuthorityFactory::ObjectType::GEOGRAPHIC_CRS,
                      {}, nullptr, nullptr)
                  .empty());

  // 3) OSGB36 (4277) to ETRS89 (4258) via intermediate CRS (e.g. WGS 84 4326):
  auto uk_extent = Extent::createFromBBOX(-10.0, 49.0, 2.0, 61.0).as_nullable();
  auto interm_ops1 = any_factory->createFromCRSCodesWithIntermediates(
      "EPSG", "4277", "EPSG", "4258", false, false, false, true,
      {{"EPSG", "4326"}}, AuthorityFactory::ObjectType::GEOGRAPHIC_CRS,
      {"EPSG"}, uk_extent, uk_extent, false);
  EXPECT_FALSE(interm_ops1.empty());

  // With allowedAuthorities, discardIfMissingGrid,
  // considerKnownGridsAsAvailable, allowedIntermediateObjectType = CRS,
  // skipIntermediateExtentIntersection = true.
  auto interm_ops2 = any_factory->createFromCRSCodesWithIntermediates(
      "EPSG", "4277", "EPSG", "4258", true, true, true, false, {},
      AuthorityFactory::ObjectType::CRS, {"EPSG"}, uk_extent, uk_extent, true);
  EXPECT_FALSE(interm_ops2.empty());

  // 4) ETRF to ETRF pivot filtering (e.g. ETRF2000 EPSG:7930 to ETRF2014
  // EPSG:8401).
  auto etrf_ops = any_factory->createFromCRSCodesWithIntermediates(
      "EPSG", "7930", "EPSG", "8401", false, false, false, true, {},
      AuthorityFactory::ObjectType::CRS, {"EPSG"}, nullptr, nullptr);
  EXPECT_FALSE(etrf_ops.empty());

  // 5) NAD83(CSRS) to NAD83(CSRS) pivot filtering:
  // - 8237 (v2 2D) -> 8240 (v3 2D) only has NAD83/WGS84 pivots, which
  // checkPivot
  //   rejects (returning empty).
  // - 8235 (v2 3D) -> 8251 (v6 3D) pivots through 8244 (v4 3D), which
  // checkPivot
  //   accepts (returning non-empty).
  auto nad83_csrs_rejected = any_factory->createFromCRSCodesWithIntermediates(
      "EPSG", "8237", "EPSG", "8240", false, false, true, true, {},
      AuthorityFactory::ObjectType::GEOGRAPHIC_CRS, {"EPSG"}, nullptr, nullptr);
  EXPECT_TRUE(nad83_csrs_rejected.empty());
  auto nad83_csrs_accepted = any_factory->createFromCRSCodesWithIntermediates(
      "EPSG", "8235", "EPSG", "8251", false, false, true, true, {},
      AuthorityFactory::ObjectType::GEOGRAPHIC_CRS, {"EPSG"}, nullptr, nullptr);
  EXPECT_FALSE(nad83_csrs_accepted.empty());

  // 6) createBetweenGeodeticCRSWithDatumBasedIntermediates:
  auto crs_8237 = epsg_factory->createGeodeticCRS("8237");
  auto crs_8252 = epsg_factory->createGeodeticCRS("8252");
  auto datum_interm_ops =
      any_factory->createBetweenGeodeticCRSWithDatumBasedIntermediates(
          crs_8237, "EPSG", "8237", crs_8252, "EPSG", "8252", false, false,
          true, true, {"EPSG"}, nullptr, nullptr, false);
  EXPECT_FALSE(datum_interm_ops.empty());
}

TEST(FactoryTest,
     CreateProjectedAndCompoundAndVerticalAndGeodeticFromExisting) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());
  absl::Cleanup cleanup = [] {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };

  auto db = DatabaseContext::create(std::string(), {}, ctx.get());
  auto epsg_factory = AuthorityFactory::create(db, "EPSG");
  auto ignf_factory = AuthorityFactory::create(db, "IGNF");
  auto any_factory = AuthorityFactory::create(db, "");

  // 1) createProjectedCRSFromExisting on EPSG:2154 (Lambert Conic Conformal
  // 2SP) exercises the 1st/2nd standard parallel SQL query and exact name
  // match.
  auto crs_2154 = epsg_factory->createProjectedCRS("2154");
  auto matches_2154 = epsg_factory->createProjectedCRSFromExisting(crs_2154);
  EXPECT_FALSE(matches_2154.empty());

  // 2) createProjectedCRSFromExisting on IGNF:LAMB93 (defined via
  // text_definition in projected_crs table) exercises the sqlRes2
  // text_definition fallback loop.
  auto ignf_lamb93 = ignf_factory->createProjectedCRS("LAMB93");
  auto matches_lamb93 =
      any_factory->createProjectedCRSFromExisting(ignf_lamb93);
  EXPECT_FALSE(matches_lamb93.empty());

  // 3) createGeodeticCRSFromDatum(datum, preferredAuthName, type):
  // - With an identified EPSG datum and preferredAuthName == "EPSG".
  auto datum_6326 = epsg_factory->createGeodeticDatum("6326");
  auto geod_from_id_datum =
      epsg_factory->createGeodeticCRSFromDatum(datum_6326, "EPSG", "");
  EXPECT_FALSE(geod_from_id_datum.empty());

  // - With an unidentified datum matching "World Geodetic System 1984" by name.
  auto unid_wgs84 = GeodeticReferenceFrame::create(
      NameMap("World Geodetic System 1984"), Ellipsoid::WGS84,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  auto geod_from_unid_datum = any_factory->createGeodeticCRSFromDatum(
      unid_wgs84, "EPSG", "geographic 2D");
  EXPECT_FALSE(geod_from_unid_datum.empty());

  // 4) createVerticalCRSFromDatum with specific authority and empty authority:
  auto vert_from_epsg =
      epsg_factory->createVerticalCRSFromDatum("EPSG", "5103");
  EXPECT_FALSE(vert_from_epsg.empty());
  auto vert_from_any = any_factory->createVerticalCRSFromDatum("EPSG", "5103");
  EXPECT_FALSE(vert_from_any.empty());

  // 5) createCompoundCRSFromExisting:
  // - 2-component CompoundCRS (EPSG:7405) with EPSG and any authority.
  auto comp_7405 = epsg_factory->createCompoundCRS("7405");
  EXPECT_FALSE(epsg_factory->createCompoundCRSFromExisting(comp_7405).empty());
  EXPECT_FALSE(any_factory->createCompoundCRSFromExisting(comp_7405).empty());

  // - 3-component CompoundCRS returns empty immediately.
  auto crs_4326 = epsg_factory->createGeographicCRS("4326");
  auto crs_5703 = epsg_factory->createVerticalCRS("5703");
  auto comp_3 =
      CompoundCRS::create(NameMap("ThreeComp"),
                          std::vector<CRSNNPtr>{crs_4326, crs_5703, crs_5703});
  EXPECT_TRUE(epsg_factory->createCompoundCRSFromExisting(comp_3).empty());

  // 6) getTransformationsForGeoid with usePROJAlternativeGridNames false &
  // true:
  auto geoid_ops_orig =
      any_factory->getTransformationsForGeoid("GEOID18", false);
  EXPECT_FALSE(geoid_ops_orig.empty());
  auto geoid_ops_proj =
      any_factory->getTransformationsForGeoid("GEOID18", true);
  EXPECT_FALSE(geoid_ops_proj.empty());

  // 7) getPointMotionOperationsFor:
  // - On epsg_factory (d->hasAuthorityRestriction() == true) with NAD83(CSRS)
  // v7
  //   (EPSG:8255).
  auto crs_8255 = epsg_factory->createGeodeticCRS("8255");
  auto pmo_list = epsg_factory->getPointMotionOperationsFor(crs_8255, false);
  EXPECT_FALSE(pmo_list.empty());

  // - On a custom GeographicCRS with an unknown datum (crsList.empty() ->
  // empty).
  auto unknown_datum = GeodeticReferenceFrame::create(
      NameMap("Unknown_Custom_Datum_XYZ"), Ellipsoid::WGS84,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  auto unknown_geog_crs = GeographicCRS::create(
      NameMap("Unknown_Custom_CRS_XYZ"), unknown_datum,
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
  EXPECT_TRUE(epsg_factory->getPointMotionOperationsFor(unknown_geog_crs, false)
                  .empty());
}

TEST(FactoryTest, CreateWithIntermediatesAndDatumIntermediatesBranches) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());
  absl::Cleanup cleanup = [] {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };

  auto db = DatabaseContext::create(std::string(), {}, ctx.get());
  auto epsg_factory = AuthorityFactory::create(db, "EPSG");
  auto any_factory = AuthorityFactory::create(db, "");

  // 1) createFromCRSCodesWithIntermediates:
  // Reversing NAD83(CSRS) v6 3D (8251) -> v2 3D (8235) exercises Case 4
  // ((intermediate->source) and (target->intermediate)) and multiple
  // allowedAuthorities.
  auto case4_ops = any_factory->createFromCRSCodesWithIntermediates(
      "EPSG", "8251", "EPSG", "8235", false, false, true, true, {},
      AuthorityFactory::ObjectType::GEOGRAPHIC_CRS, {"EPSG", "PROJ"}, nullptr,
      nullptr);
  EXPECT_FALSE(case4_ops.empty());

  // Calling with allowedIntermediateObjectType = ObjectType::CRS on 8237 ->
  // 8240 bypasses the SQL publication_date filter and reaches C++ checkPivot
  // rejection for NAD83/WGS84 pivots.
  auto pivot_rejected_crs = any_factory->createFromCRSCodesWithIntermediates(
      "EPSG", "8237", "EPSG", "8240", false, false, true, true, {},
      AuthorityFactory::ObjectType::CRS, {"EPSG", "PROJ"}, nullptr, nullptr);
  EXPECT_FALSE(pivot_rejected_crs.empty());

  // 2) createBetweenGeodeticCRSWithDatumBasedIntermediates:
  auto crs_8237 = epsg_factory->createGeodeticCRS("8237");
  auto crs_8252 = epsg_factory->createGeodeticCRS("8252");

  // - Same source and target code -> returns empty.
  EXPECT_TRUE(any_factory
                  ->createBetweenGeodeticCRSWithDatumBasedIntermediates(
                      crs_8237, "EPSG", "8237", crs_8237, "EPSG", "8237", false,
                      false, true, true, {}, nullptr, nullptr, false)
                  .empty());

  // - Non-GeodeticCRS (e.g. ProjectedCRS EPSG:32631) -> returns empty.
  auto crs_32631 = epsg_factory->createProjectedCRS("32631");
  EXPECT_TRUE(any_factory
                  ->createBetweenGeodeticCRSWithDatumBasedIntermediates(
                      crs_32631, "EPSG", "32631", crs_8252, "EPSG", "8252",
                      false, false, true, true, {}, nullptr, nullptr, false)
                  .empty());

  // - GeodeticCRS with a DatumEnsemble (datum() == nullptr) and no identifiers
  // -> falls back to datumEnsemble() (EPSG:6326) and finds candidates.
  auto unid_geog_with_ens = GeographicCRS::create(
      NameMap("Unid_Geog_Ens"), nullptr,
      epsg_factory->createDatumEnsemble("6326", "geodetic_datum"),
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
  EXPECT_FALSE(any_factory
                   ->createBetweenGeodeticCRSWithDatumBasedIntermediates(
                       unid_geog_with_ens, "", "", crs_8252, "EPSG", "8252",
                       false, false, true, true, {}, nullptr, nullptr, false)
                   .empty());

  // - Custom GeographicCRS whose datum has no identifiers:
  //   a) With valid sourceCRSAuthName/Code ("EPSG", "8237") -> looks up 8237 in
  //   DB.
  auto unid_datum = GeodeticReferenceFrame::create(
      NameMap("Custom_Unid_Datum"), Ellipsoid::GRS1980, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  auto custom_geog_unid = GeographicCRS::create(
      NameMap("Custom_Geog_Unid"), unid_datum,
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
  auto ops_from_unid_lookup =
      any_factory->createBetweenGeodeticCRSWithDatumBasedIntermediates(
          custom_geog_unid, "EPSG", "8237", crs_8252, "EPSG", "8252", false,
          false, true, true, {"EPSG"}, nullptr, nullptr, false);
  EXPECT_FALSE(ops_from_unid_lookup.empty());

  //   b) With invalid sourceCRSAuthName/Code ("EPSG", "99999999") -> exception
  //      caught, sourceCandidates empty -> returns empty.
  EXPECT_TRUE(any_factory
                  ->createBetweenGeodeticCRSWithDatumBasedIntermediates(
                      custom_geog_unid, "EPSG", "99999999", crs_8252, "EPSG",
                      "8252", false, false, true, true, {"EPSG"}, nullptr,
                      nullptr, false)
                  .empty());

  // - Custom GeographicCRS whose datum has an identifier with no matching CRS
  //   in DB -> createGeodeticCRSFromDatum returns empty, falls back to pushing
  //   sourceCRS into sourceCandidates.
  auto fake_id_datum = GeodeticReferenceFrame::create(
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "Fake_Id_Datum")
          .set(Identifier::CODESPACE_KEY, "EPSG")
          .set(Identifier::CODE_KEY, "99999999"),
      Ellipsoid::GRS1980, optional<std::string>(), PrimeMeridian::GREENWICH);
  auto custom_geog_fake_id = GeographicCRS::create(
      NameMap("Custom_Geog_Fake_Id"), fake_id_datum,
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
  EXPECT_TRUE(any_factory
                  ->createBetweenGeodeticCRSWithDatumBasedIntermediates(
                      custom_geog_fake_id, "EPSG", "99999999", crs_8252, "EPSG",
                      "8252", false, false, true, true, {"EPSG"}, nullptr,
                      nullptr, false)
                  .empty());

  // - Calling on epsg_factory (hasAuthorityRestriction() == true) with multiple
  //   allowedAuthorities and non-world extents (intersecting and disjoint).
  auto canada_extent =
      Extent::createFromBBOX(-141.0, 41.0, -52.0, 84.0).as_nullable();
  auto antarctic_extent =
      Extent::createFromBBOX(-179.0, -89.0, -178.0, -88.0).as_nullable();
  auto ops_with_extent =
      epsg_factory->createBetweenGeodeticCRSWithDatumBasedIntermediates(
          crs_8237, "EPSG", "8237", crs_8252, "EPSG", "8252", false, false,
          true, true, {"EPSG", "PROJ"}, canada_extent, canada_extent, false);
  EXPECT_FALSE(ops_with_extent.empty());

  auto ops_disjoint_extent =
      epsg_factory->createBetweenGeodeticCRSWithDatumBasedIntermediates(
          crs_8237, "EPSG", "8237", crs_8252, "EPSG", "8252", false, false,
          true, true, {"EPSG", "PROJ"}, antarctic_extent, antarctic_extent,
          false);
  EXPECT_TRUE(ops_disjoint_extent.empty());
}

TEST(FactoryTest, GetInsertStatementsForRecursiveAndEdgeCases) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor log_suppressor(ctx.get());
  absl::Cleanup cleanup = [] {
    pj_clear_sqlite_cache();
    proj_cleanup();
  };

  // 1) Let a DatabaseContext with an active insert statements session go out of
  // scope without calling stopInsertStatementsSession() to exercise destructor
  // cleanup.
  {
    auto temp_db = DatabaseContext::create(std::string(), {}, ctx.get());
    temp_db->startInsertStatementsSession();
  }

  auto db = DatabaseContext::create(std::string(), {}, ctx.get());
  auto epsg_factory = AuthorityFactory::create(db, "EPSG");

  // 2) Calling getInsertStatementsFor on already-existing EPSG objects with
  // authName="EPSG" and matching code exercises instantiateFunc and
  // early-return branches for PrimeMeridian, Ellipsoid, GeodeticReferenceFrame,
  // DatumEnsemble, and VerticalReferenceFrame.
  db->startInsertStatementsSession();
  auto pm_8901 = epsg_factory->createPrimeMeridian("8901");
  EXPECT_TRUE(
      db->getInsertStatementsFor(pm_8901, "EPSG", "8901", false, {"EPSG"})
          .empty());
  auto ellps_7030 = epsg_factory->createEllipsoid("7030");
  EXPECT_TRUE(
      db->getInsertStatementsFor(ellps_7030, "EPSG", "7030", false, {"EPSG"})
          .empty());
  auto gdatum_6269 = epsg_factory->createGeodeticDatum("6269");
  EXPECT_TRUE(
      db->getInsertStatementsFor(gdatum_6269, "EPSG", "6269", false, {"EPSG"})
          .empty());
  auto ensemble_6326 =
      epsg_factory->createDatumEnsemble("6326", "geodetic_datum");
  EXPECT_TRUE(
      db->getInsertStatementsFor(ensemble_6326, "EPSG", "6326", false, {"EPSG"})
          .empty());
  auto vdatum_5103 = epsg_factory->createVerticalDatum("5103");
  EXPECT_TRUE(
      db->getInsertStatementsFor(vdatum_5103, "EPSG", "5103", false, {"EPSG"})
          .empty());
  db->stopInsertStatementsSession();

  // 3) Uninserted hierarchy: CompoundCRS -> GeographicCRS -> GeodeticDatum
  // (with single quotes in name & anchor) -> Ellipsoid -> PrimeMeridian, plus
  // VerticalCRS -> VerticalDatum, tested with both numericCode = false and
  // true.
  for (bool numeric_code : {false, true}) {
    auto session_db = DatabaseContext::create(std::string(), {}, ctx.get());
    session_db->startInsertStatementsSession();

    // Use custom units without codeSpace that match degree / metre SI factors.
    UnitOfMeasure custom_deg("custom_deg",
                             UnitOfMeasure::DEGREE.conversionToSI(),
                             UnitOfMeasure::Type::ANGULAR);
    UnitOfMeasure custom_m("custom_m", 1.0, UnitOfMeasure::Type::LINEAR);

    auto custom_pm =
        PrimeMeridian::create(NameMap("Custom's PM"), Angle(2.5, custom_deg));
    auto custom_ellps = Ellipsoid::createFlattenedSphere(
        NameMap("Custom's Ellipsoid"), Length(6378137.5, custom_m),
        Scale(298.257,
              UnitOfMeasure("custom_scale", 1.0, UnitOfMeasure::Type::SCALE)),
        "CustomBody");

    // Attach 2 domains (one with extent without description and empty scope) to
    // exercise multi-domain and null-description/scope branches.
    auto bbox1 = GeographicBoundingBox::create(-5.0, 40.0, 10.0, 52.0);
    auto ext_no_desc = Extent::create(optional<std::string>(), {bbox1}, {}, {});
    auto ext_with_desc = Extent::create(
        optional<std::string>("France's extent"), {bbox1}, {}, {});
    auto dom1 = ObjectDomain::create(optional<std::string>(), ext_no_desc);
    auto dom2 =
        ObjectDomain::create(optional<std::string>("Scope's 2"), ext_with_desc);

    auto custom_gdatum = GeodeticReferenceFrame::create(
        PropertyMap()
            .set(IdentifiedObject::NAME_KEY, "Custom's Geodetic Datum")
            .set(ObjectUsage::OBJECT_DOMAIN_KEY, dom1),
        custom_ellps, optional<std::string>("Custom's Anchor"), custom_pm);
    auto custom_geog = GeographicCRS::create(
        PropertyMap()
            .set(IdentifiedObject::NAME_KEY, "Custom's Geog CRS")
            .set(ObjectUsage::DOMAIN_OF_VALIDITY_KEY, ext_no_desc)
            .set(ObjectUsage::OBJECT_DOMAIN_KEY, dom2),
        custom_gdatum, EllipsoidalCS::createLatitudeLongitude(custom_deg));

    auto custom_vdatum =
        VerticalReferenceFrame::create(NameMap("Custom's Vertical Datum"),
                                       optional<std::string>("Vert's Anchor"));
    auto custom_vert =
        VerticalCRS::create(NameMap("Custom's Vert CRS"), custom_vdatum,
                            VerticalCS::createGravityRelatedHeight(custom_m));

    auto custom_comp =
        CompoundCRS::create(NameMap("Custom's Compound CRS"),
                            std::vector<CRSNNPtr>{custom_geog, custom_vert});

    auto sql_comp = session_db->getInsertStatementsFor(
        custom_comp, "HOBU", numeric_code ? "100" : "COMP_A", numeric_code,
        {"EPSG"});
    EXPECT_FALSE(sql_comp.empty());
    session_db->stopInsertStatementsSession();

    // Also test ProjectedCRS with an uninserted base GeographicCRS that uses an
    // uninserted DatumEnsemble, and a VerticalCRS with an uninserted vertical
    // DatumEnsemble (insert the ensemble first with an explicit non-1 code when
    // numeric_code is true so auto-assigned member codes 1, 2 do not collide
    // with the ensemble's own code).
    auto session_db2 = DatabaseContext::create(std::string(), {}, ctx.get());
    session_db2->startInsertStatementsSession();
    auto member_g1 = GeodeticReferenceFrame::create(
        NameMap("Ensemble Member G1"), custom_ellps, optional<std::string>(),
        custom_pm);
    auto member_g2 = GeodeticReferenceFrame::create(
        NameMap("Ensemble Member G2"), custom_ellps, optional<std::string>(),
        custom_pm);
    auto geod_ens =
        DatumEnsemble::create(NameMap("Custom Geodetic Ensemble"),
                              std::vector<DatumNNPtr>{member_g1, member_g2},
                              PositionalAccuracy::create("2.5"));
    if (numeric_code) {
      EXPECT_FALSE(
          session_db2
              ->getInsertStatementsFor(geod_ens, "HOBU", "50", true, {"EPSG"})
              .empty());
    }
    auto geod_ens_for_proj =
        numeric_code ? DatumEnsemble::create(
                           NameMap("Custom Geodetic Ensemble 2"),
                           std::vector<DatumNNPtr>{
                               epsg_factory->createGeodeticDatum("1156"),
                               epsg_factory->createGeodeticDatum("1155")},
                           PositionalAccuracy::create("2.5"))
                     : geod_ens;
    auto geog_with_ens = GeographicCRS::create(
        NameMap("Custom Geog With Ensemble"), nullptr, geod_ens_for_proj,
        EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
    auto proj_with_uninserted_base = ProjectedCRS::create(
        NameMap("Custom Proj With Uninserted Base"), geog_with_ens,
        Conversion::createUTM(PropertyMap(), 31, true),
        CartesianCS::createEastingNorthing(UnitOfMeasure::METRE));
    auto sql_proj = session_db2->getInsertStatementsFor(
        proj_with_uninserted_base, "HOBU", numeric_code ? "200" : "PROJ_A",
        numeric_code, {"EPSG"});
    EXPECT_FALSE(sql_proj.empty());
    session_db2->stopInsertStatementsSession();

    auto session_db3 = DatabaseContext::create(std::string(), {}, ctx.get());
    session_db3->startInsertStatementsSession();
    auto member_v1 = VerticalReferenceFrame::create(
        NameMap("Ensemble Member V1"), optional<std::string>());
    auto member_v2 = VerticalReferenceFrame::create(
        NameMap("Ensemble Member V2"), optional<std::string>());
    auto vert_ens =
        DatumEnsemble::create(NameMap("Custom Vertical Ensemble"),
                              std::vector<DatumNNPtr>{member_v1, member_v2},
                              PositionalAccuracy::create("1.0"));
    if (numeric_code) {
      EXPECT_FALSE(
          session_db3
              ->getInsertStatementsFor(vert_ens, "HOBU", "60", true, {"EPSG"})
              .empty());
    }
    auto vert_ens_for_crs =
        numeric_code ? DatumEnsemble::create(
                           NameMap("Custom Vertical Ensemble 2"),
                           std::vector<DatumNNPtr>{
                               epsg_factory->createVerticalDatum("5103"),
                               epsg_factory->createVerticalDatum("5102")},
                           PositionalAccuracy::create("1.0"))
                     : vert_ens;
    auto vert_with_ens = VerticalCRS::create(
        NameMap("Custom Vert With Ensemble"), nullptr, vert_ens_for_crs,
        VerticalCS::createGravityRelatedHeight(UnitOfMeasure::METRE));
    auto sql_vert_ens = session_db3->getInsertStatementsFor(
        vert_with_ens, "HOBU", numeric_code ? "300" : "VERT_ENS_A",
        numeric_code, {"EPSG"});
    EXPECT_FALSE(sql_vert_ens.empty());
    session_db3->stopInsertStatementsSession();
  }

  // 4) Exercise findFreeCode collision suffixes (_2 .. _9) and "too many
  // similar codes" exception on the 10th insertion with the same name.
  {
    auto dup_db = DatabaseContext::create(std::string(), {}, ctx.get());
    dup_db->startInsertStatementsSession();
    for (int i = 1; i <= 10; ++i) {
      auto pm = PrimeMeridian::create(NameMap("DupPM"),
                                      Angle(0.1 * i, UnitOfMeasure::DEGREE));
      if (i < 10) {
        std::string code = dup_db->suggestsCodeFor(pm, "HOBU", false);
        EXPECT_FALSE(code.empty());
        EXPECT_FALSE(
            dup_db->getInsertStatementsFor(pm, "HOBU", code, false, {"EPSG"})
                .empty());
      } else {
        EXPECT_THROW(dup_db->suggestsCodeFor(pm, "HOBU", false),
                     FactoryException);
      }
    }
    dup_db->stopInsertStatementsSession();
  }

  // 5) Error branches in getInsertStatementsFor:
  {
    auto err_db = DatabaseContext::create(std::string(), {}, ctx.get());
    err_db->startInsertStatementsSession();

    // Unit of type UNKNOWN -> throws FactoryException when SI factor does not
    // match any unit in DB.
    UnitOfMeasure unknown_unit("bad_unit", 0.1234567,
                               UnitOfMeasure::Type::UNKNOWN);
    auto pm_bad_unit =
        PrimeMeridian::create(NameMap("PM Bad Unit"), Angle(1.0, unknown_unit));
    EXPECT_THROW(err_db->getInsertStatementsFor(pm_bad_unit, "HOBU", "1", true,
                                                {"EPSG"}),
                 FactoryException);

    // GeodeticCRS with 3D SphericalCS -> inserts as 3D geocentric CRS.
    auto spherical_cs = SphericalCS::create(
        PropertyMap(),
        CoordinateSystemAxis::create(
            NameMap("lat"), "lat", AxisDirection::NORTH, UnitOfMeasure::DEGREE),
        CoordinateSystemAxis::create(NameMap("lon"), "lon", AxisDirection::EAST,
                                     UnitOfMeasure::DEGREE),
        CoordinateSystemAxis::create(NameMap("r"), "r", AxisDirection::UP,
                                     UnitOfMeasure::METRE));
    auto spherical_geod_crs =
        GeodeticCRS::create(NameMap("Spherical Geod CRS"),
                            GeodeticReferenceFrame::EPSG_6326, spherical_cs);
    EXPECT_FALSE(err_db
                     ->getInsertStatementsFor(spherical_geod_crs, "HOBU", "2",
                                              true, {"EPSG"})
                     .empty());

    // CompoundCRS with 3 components -> throws FactoryException.
    auto crs_4326 = epsg_factory->createGeographicCRS("4326");
    auto crs_5703 = epsg_factory->createVerticalCRS("5703");
    auto comp_3 = CompoundCRS::create(
        NameMap("Comp3"), std::vector<CRSNNPtr>{crs_4326, crs_5703, crs_5703});
    EXPECT_THROW(
        err_db->getInsertStatementsFor(comp_3, "HOBU", "3", true, {"EPSG"}),
        FactoryException);

    // DatumEnsemble with EngineeringDatum members -> throws FactoryException.
    auto eng_d1 = EngineeringDatum::create(NameMap("Eng1"));
    auto eng_d2 = EngineeringDatum::create(NameMap("Eng2"));
    auto eng_ens = DatumEnsemble::create(
        NameMap("EngEnsemble"), std::vector<DatumNNPtr>{eng_d1, eng_d2},
        PositionalAccuracy::create("1.0"));
    EXPECT_THROW(
        err_db->getInsertStatementsFor(eng_ens, "HOBU", "4", true, {"EPSG"}),
        FactoryException);

    err_db->stopInsertStatementsSession();
  }
}

}  // namespace
}  // namespace proj
