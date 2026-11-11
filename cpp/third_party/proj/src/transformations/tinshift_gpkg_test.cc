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
// Unit tests for GeoPackage-based Triangulated Irregular Network (TIN) shift
// transformations (proj/src/transformations/tinshift_gpkg.cpp).
// Note: in google3 default non-GCE builds, networking is disabled and
// EMBED_RESOURCE_FILES is unset.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/tinshift_gpkg.cpp

#include "third_party/proj4/proj/src/transformations/tinshift_gpkg.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>  // NOLINT(build/c++20)
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/google/util/temp_file.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/sqlite/sqlite3.h"

namespace proj {
namespace {

using ::testing::HasSubstr;

struct SqliteDeleter {
  void operator()(sqlite3* db) const noexcept {
    if (db != nullptr) {
      sqlite3_close(db);
    }
  }
};
using ScopedSqlite = std::unique_ptr<sqlite3, SqliteDeleter>;

struct StmtDeleter {
  void operator()(sqlite3_stmt* stmt) const noexcept {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
    }
  }
};
using ScopedStmt = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

// Encodes a 29-byte little-endian GeoPackage Point binary geometry blob
// (8-byte GP header + 5-byte WKB Point header + 2 IEEE-754 doubles).
std::vector<uint8_t> MakeGpkgPointBlob(double x, double y) {
  std::vector<uint8_t> blob;
  blob.reserve(29);
  blob.push_back('G');
  blob.push_back('P');
  blob.push_back(0);  // version = 0
  blob.push_back(1);  // flags = 1 (little-endian, no envelope)
  for (int i = 0; i < 4; ++i) {
    blob.push_back(0);  // srs_id = 0
  }
  blob.push_back(1);  // WKB byteOrder = 1 (little-endian)
  blob.push_back(1);  // wkbType = 1 (Point, uint32 LE)
  blob.push_back(0);
  blob.push_back(0);
  blob.push_back(0);

  internal::AppendLeFloat64(blob, x);
  internal::AppendLeFloat64(blob, y);
  return blob;
}

bool ExecSql(sqlite3* db, const std::string& sql) {
  return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
}

ScopedSqlite CreateOpenSqlite(const TempFile& file) {
  sqlite3* raw_db = nullptr;
  if (sqlite3_open(file.c_str(), &raw_db) != SQLITE_OK) {
    if (raw_db != nullptr) {
      sqlite3_close(raw_db);
    }
    return nullptr;
  }
  return ScopedSqlite(raw_db);
}

bool InsertMetadata(sqlite3* db, std::string_view metadata_json) {
  if (!ExecSql(db,
               "CREATE TABLE IF NOT EXISTS gpkg_metadata ("
               "md_standard_uri TEXT, metadata TEXT);")) {
    return false;
  }
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO gpkg_metadata (md_standard_uri, metadata)"
                         " VALUES ('https://proj.org', ?);",
                         -1, &raw_stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  ScopedStmt stmt(raw_stmt);
  sqlite3_bind_text(stmt.get(), 1, metadata_json.data(),
                    static_cast<int>(metadata_json.size()), SQLITE_TRANSIENT);
  return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool InsertContentsBbox(sqlite3* db, double min_x, double min_y, double max_x,
                        double max_y) {
  if (!ExecSql(db,
               "CREATE TABLE IF NOT EXISTS gpkg_contents ("
               "table_name TEXT, min_x REAL, min_y REAL, "
               "max_x REAL, max_y REAL);")) {
    return false;
  }
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "INSERT INTO gpkg_contents (table_name, min_x, min_y, max_x, max_y) "
          "VALUES ('vertices', ?, ?, ?, ?);",
          -1, &raw_stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  ScopedStmt stmt(raw_stmt);
  sqlite3_bind_double(stmt.get(), 1, min_x);
  sqlite3_bind_double(stmt.get(), 2, min_y);
  sqlite3_bind_double(stmt.get(), 3, max_x);
  sqlite3_bind_double(stmt.get(), 4, max_y);
  return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

struct VertexRow {
  int fid = 0;
  double source_x = 0.0;
  double source_y = 0.0;
  double target_x = 0.0;
  double target_y = 0.0;
  double source_z = 0.0;
  double target_z = 0.0;
  double offset_z = 0.0;
  std::vector<uint8_t> custom_geom = {};
};

struct TriangleRow {
  int fid = 0;
  int v1 = 0;
  int v2 = 0;
  int v3 = 0;
  double minx = 0.0;
  double maxx = 0.0;
  double miny = 0.0;
  double maxy = 0.0;
};

bool PopulateTriangles(sqlite3* db, std::span<const TriangleRow> triangles) {
  if (!ExecSql(db,
               "CREATE TABLE triangles_def ("
               "fid INTEGER PRIMARY KEY, idx_vertex1 INTEGER, "
               "idx_vertex2 INTEGER, idx_vertex3 INTEGER);"
               "CREATE TABLE rtree_triangles_geom ("
               "id INTEGER PRIMARY KEY, minx REAL, maxx REAL, "
               "miny REAL, maxy REAL);")) {
    return false;
  }
  for (const auto& t : triangles) {
    sqlite3_stmt* raw_def = nullptr;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO triangles_def "
                           "(fid, idx_vertex1, idx_vertex2, idx_vertex3) "
                           "VALUES (?, ?, ?, ?);",
                           -1, &raw_def, nullptr) != SQLITE_OK) {
      return false;
    }
    ScopedStmt def_stmt(raw_def);
    sqlite3_bind_int(def_stmt.get(), 1, t.fid);
    sqlite3_bind_int(def_stmt.get(), 2, t.v1);
    sqlite3_bind_int(def_stmt.get(), 3, t.v2);
    sqlite3_bind_int(def_stmt.get(), 4, t.v3);
    if (sqlite3_step(def_stmt.get()) != SQLITE_DONE) {
      return false;
    }

    sqlite3_stmt* raw_rtree = nullptr;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO rtree_triangles_geom "
                           "(id, minx, maxx, miny, maxy) "
                           "VALUES (?, ?, ?, ?, ?);",
                           -1, &raw_rtree, nullptr) != SQLITE_OK) {
      return false;
    }
    ScopedStmt rtree_stmt(raw_rtree);
    sqlite3_bind_int(rtree_stmt.get(), 1, t.fid);
    sqlite3_bind_double(rtree_stmt.get(), 2, t.minx);
    sqlite3_bind_double(rtree_stmt.get(), 3, t.maxx);
    sqlite3_bind_double(rtree_stmt.get(), 4, t.miny);
    sqlite3_bind_double(rtree_stmt.get(), 5, t.maxy);
    if (sqlite3_step(rtree_stmt.get()) != SQLITE_DONE) {
      return false;
    }
  }
  return true;
}

bool PopulateVertices(sqlite3* db, std::span<const VertexRow> vertices,
                      bool include_xy, bool include_source_target_z,
                      bool include_offset_z) {
  std::string create_sql =
      "CREATE TABLE vertices (fid INTEGER PRIMARY KEY, geom BLOB";
  if (include_xy) {
    create_sql += ", target_x REAL, target_y REAL";
  }
  if (include_source_target_z) {
    create_sql += ", source_z REAL, target_z REAL";
  }
  if (include_offset_z) {
    create_sql += ", offset_z REAL";
  }
  create_sql += ", extra_col TEXT);";
  if (!ExecSql(db, create_sql)) {
    return false;
  }

  for (const auto& v : vertices) {
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO vertices (fid, geom) VALUES (?, ?);",
                           -1, &raw_stmt, nullptr) != SQLITE_OK) {
      return false;
    }
    ScopedStmt stmt(raw_stmt);
    sqlite3_bind_int(stmt.get(), 1, v.fid);
    const std::vector<uint8_t> geom =
        v.custom_geom.empty() ? MakeGpkgPointBlob(v.source_x, v.source_y)
                              : v.custom_geom;
    sqlite3_bind_blob(stmt.get(), 2, geom.data(), static_cast<int>(geom.size()),
                      SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      return false;
    }

    if (include_xy) {
      sqlite3_stmt* raw_xy = nullptr;
      if (sqlite3_prepare_v2(
              db,
              "UPDATE vertices SET target_x = ?, target_y = ? WHERE fid = ?;",
              -1, &raw_xy, nullptr) != SQLITE_OK) {
        return false;
      }
      ScopedStmt xy_stmt(raw_xy);
      sqlite3_bind_double(xy_stmt.get(), 1, v.target_x);
      sqlite3_bind_double(xy_stmt.get(), 2, v.target_y);
      sqlite3_bind_int(xy_stmt.get(), 3, v.fid);
      if (sqlite3_step(xy_stmt.get()) != SQLITE_DONE) {
        return false;
      }
    }
    if (include_source_target_z) {
      sqlite3_stmt* raw_stz = nullptr;
      if (sqlite3_prepare_v2(
              db,
              "UPDATE vertices SET source_z = ?, target_z = ? WHERE fid = ?;",
              -1, &raw_stz, nullptr) != SQLITE_OK) {
        return false;
      }
      ScopedStmt stz_stmt(raw_stz);
      sqlite3_bind_double(stz_stmt.get(), 1, v.source_z);
      sqlite3_bind_double(stz_stmt.get(), 2, v.target_z);
      sqlite3_bind_int(stz_stmt.get(), 3, v.fid);
      if (sqlite3_step(stz_stmt.get()) != SQLITE_DONE) {
        return false;
      }
    }
    if (include_offset_z) {
      sqlite3_stmt* raw_oz = nullptr;
      if (sqlite3_prepare_v2(db,
                             "UPDATE vertices SET offset_z = ? WHERE fid = ?;",
                             -1, &raw_oz, nullptr) != SQLITE_OK) {
        return false;
      }
      ScopedStmt oz_stmt(raw_oz);
      sqlite3_bind_double(oz_stmt.get(), 1, v.offset_z);
      sqlite3_bind_int(oz_stmt.get(), 2, v.fid);
      if (sqlite3_step(oz_stmt.get()) != SQLITE_DONE) {
        return false;
      }
    }
  }
  return true;
}

TEST(TinshiftGpkgTest, OpenNonExistentAndNetworkUrlsThrowException) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor log_suppressor(ctx.get());

  // 1. Non-existent local file throws TINShiftGeopackageException and populates
  //    what().
  bool caught = false;
  try {
    TINShiftGeopackageFile::open(ctx.get(), "/nonexistent/path/missing.gpkg");
  } catch (const TINShiftGeopackageException& ex) {
    caught = true;
    EXPECT_THAT(ex.what(), HasSubstr("Open of /nonexistent/path/missing.gpkg"));
  }
  EXPECT_TRUE(caught);

  // 2. HTTP and HTTPS URLs exercise the SQLite3VFS::createNetwork(ctx) branch.
  EXPECT_THROW(
      TINShiftGeopackageFile::open(ctx.get(), "http://example.com/tin.gpkg"),
      TINShiftGeopackageException);
  EXPECT_THROW(
      TINShiftGeopackageFile::open(ctx.get(), "https://example.com/tin.gpkg"),
      TINShiftGeopackageException);
}

TEST(TinshiftGpkgTest, OpenRejectsMissingOrInvalidMetadataTableAndJson) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Missing gpkg_metadata table.
  {
    TempFile file("tin_no_meta_table.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(ExecSql(db.get(), "CREATE TABLE dummy (id INT);"));
    db.reset();

    try {
      TINShiftGeopackageFile::open(ctx.get(), file.string());
      FAIL() << "Expected TINShiftGeopackageException";
    } catch (const TINShiftGeopackageException& ex) {
      EXPECT_THAT(ex.what(), HasSubstr("Cannot get metadata:"));
    }
  }

  // 2. Empty gpkg_metadata table (0 rows for https://proj.org).
  {
    TempFile file("tin_empty_meta_table.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(ExecSql(
        db.get(),
        "CREATE TABLE gpkg_metadata (md_standard_uri TEXT, metadata TEXT);"));
    db.reset();

    try {
      TINShiftGeopackageFile::open(ctx.get(), file.string());
      FAIL() << "Expected TINShiftGeopackageException";
    } catch (const TINShiftGeopackageException& ex) {
      EXPECT_THAT(ex.what(),
                  HasSubstr("Cannot get metadata in gpkg_metadata table"));
    }
  }

  // 3. Malformed JSON syntax in gpkg_metadata.
  {
    TempFile file("tin_malformed_json.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), "{invalid_json"));
    db.reset();

    try {
      TINShiftGeopackageFile::open(ctx.get(), file.string());
      FAIL() << "Expected TINShiftGeopackageException";
    } catch (const TINShiftGeopackageException& ex) {
      EXPECT_THAT(ex.what(), HasSubstr("Cannot parse JSON metadata:"));
    }
  }

  // 4. Non-object JSON value in gpkg_metadata.
  {
    TempFile file("tin_non_object_json.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), "[1, 2, 3]"));
    db.reset();

    try {
      TINShiftGeopackageFile::open(ctx.get(), file.string());
      FAIL() << "Expected TINShiftGeopackageException";
    } catch (const TINShiftGeopackageException& ex) {
      EXPECT_THAT(ex.what(), HasSubstr("JSON metadata is not an object"));
    }
  }
}

TEST(TinshiftGpkgTest, OpenRejectsInvalidMetadataFieldsAndTypes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct MetaCase {
    std::string_view json;
    std::string_view expected_substr;
  };
  constexpr MetaCase kCases[] = {
      {
          .json = R"({})",
          .expected_substr = "Missing \"file_type\" key",
      },
      {
          .json = R"({"file_type": 123})",
          .expected_substr = "The value of \"file_type\" should be a string",
      },
      {
          .json = R"({"file_type": "wrong_type"})",
          .expected_substr = "File is not of the expected type",
      },
      {
          .json = R"({"file_type": "triangulation_file",
                      "format_version": "2.0"})",
          .expected_substr = "format_version = 2.0 is not supported",
      },
      {
          .json = R"({"file_type": "triangulation_file",
                      "format_version": "1.0"})",
          .expected_substr = "Missing \"transformed_components\" key",
      },
      {
          .json = R"({"file_type": "triangulation_file",
                      "format_version": "1.0",
                      "transformed_components": "horizontal"})",
          .expected_substr =
              "The value of \"transformed_components\" should be a array",
      },
      {
          .json = R"({"file_type": "triangulation_file",
                      "format_version": "1.0",
                      "transformed_components": [42]})",
          .expected_substr = "transformed_components[] item is not a string",
      },
      {
          .json = R"({"file_type": "triangulation_file",
                      "format_version": "1.0",
                      "transformed_components": ["temporal"]})",
          .expected_substr =
              "transformed_components[] = temporal is not handled",
      },
      {
          .json = R"({"file_type": "triangulation_file",
                      "format_version": "1.0",
                      "transformed_components": ["horizontal"]})",
          .expected_substr = "Missing \"min_shift_x\" key",
      },
      {
          .json = R"({"file_type": "triangulation_file",
                      "format_version": "1.0",
                      "transformed_components": ["horizontal"],
                      "min_shift_x": "not_a_number"})",
          .expected_substr = "The value of \"min_shift_x\" should be a number",
      },
      {
          .json = R"({"file_type": "triangulation_file",
                      "format_version": "1.0",
                      "transformed_components": ["vertical"],
                      "fallback_strategy": "invalid_strategy"})",
          .expected_substr = "invalid fallback_strategy",
      },
      {
          .json = R"({"file_type": "triangulation_file",
                      "format_version": "1.0",
                      "transformed_components": ["vertical"],
                      "fallback_strategy": "nearest_side"})",
          .expected_substr = "Missing \"num_vertices\" key",
      },
      {
          .json = R"({"file_type": "triangulation_file",
                      "format_version": "1.0",
                      "transformed_components": ["vertical"],
                      "fallback_strategy": "nearest_side",
                      "num_vertices": "bad"})",
          .expected_substr =
              "The value of \"num_vertices\" should be an integer",
      },
      {
          .json = R"({"file_type": "triangulation_file",
                      "format_version": "1.0",
                      "transformed_components": ["vertical"],
                      "fallback_strategy": "nearest_centroid",
                      "num_vertices": 0})",
          .expected_substr = "invalid value for num_vertices",
      },
  };

  for (size_t i = 0; i < std::size(kCases); ++i) {
    TempFile file("tin_bad_meta_" + std::to_string(i) + ".gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), kCases[i].json));
    db.reset();

    try {
      TINShiftGeopackageFile::open(ctx.get(), file.string());
      FAIL() << "Expected TINShiftGeopackageException for case " << i;
    } catch (const TINShiftGeopackageException& ex) {
      EXPECT_THAT(ex.what(), HasSubstr(std::string(kCases[i].expected_substr)));
    }
  }
}

TEST(TinshiftGpkgTest, OpenRejectsInvalidGpkgContentsAndVerticesSchema) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr std::string_view kHorizMeta = R"({
    "file_type": "triangulation_file",
    "format_version": "1.0",
    "transformed_components": ["horizontal"],
    "min_shift_x": -1.0,
    "min_shift_y": -1.0,
    "max_shift_x": 2.0,
    "max_shift_y": 2.0,
    "fallback_strategy": "none"
  })";

  constexpr std::string_view kVertMeta = R"({
    "file_type": "triangulation_file",
    "format_version": "1.0",
    "transformed_components": ["vertical"]
  })";

  // 1. Missing gpkg_contents table.
  {
    TempFile file("tin_no_contents.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), kHorizMeta));
    db.reset();

    try {
      TINShiftGeopackageFile::open(ctx.get(), file.string());
      FAIL() << "Expected TINShiftGeopackageException";
    } catch (const TINShiftGeopackageException& ex) {
      EXPECT_THAT(ex.what(),
                  HasSubstr("Cannot get bounding box of vertices table:"));
    }
  }

  // 2. Empty gpkg_contents table.
  {
    TempFile file("tin_empty_contents.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), kHorizMeta));
    ASSERT_TRUE(ExecSql(db.get(),
                        "CREATE TABLE gpkg_contents ("
                        "table_name TEXT, min_x REAL, min_y REAL, "
                        "max_x REAL, max_y REAL);"));
    db.reset();

    try {
      TINShiftGeopackageFile::open(ctx.get(), file.string());
      FAIL() << "Expected TINShiftGeopackageException";
    } catch (const TINShiftGeopackageException& ex) {
      EXPECT_THAT(ex.what(),
                  HasSubstr("Cannot get bounding box of vertices table"));
    }
  }

  // 3. Invalid bounding box (min_x >= max_x).
  {
    TempFile file("tin_inverted_bbox.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), kHorizMeta));
    ASSERT_TRUE(InsertContentsBbox(db.get(), 10.0, 0.0, 5.0, 10.0));
    db.reset();

    try {
      TINShiftGeopackageFile::open(ctx.get(), file.string());
      FAIL() << "Expected TINShiftGeopackageException";
    } catch (const TINShiftGeopackageException& ex) {
      EXPECT_THAT(ex.what(),
                  HasSubstr("Invalid bounding box of vertices table"));
    }
  }

  // 4. Missing vertices table (PRAGMA table_info(vertices) returns 0 rows).
  {
    TempFile file("tin_no_vertices.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), kHorizMeta));
    ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 10.0, 10.0));
    db.reset();

    try {
      TINShiftGeopackageFile::open(ctx.get(), file.string());
      FAIL() << "Expected TINShiftGeopackageException";
    } catch (const TINShiftGeopackageException& ex) {
      EXPECT_THAT(ex.what(),
                  HasSubstr("Cannot get definition of table vertices"));
    }
  }

  // 5. Horizontal transformation missing target_x column.
  {
    TempFile file("tin_missing_target_x.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), kHorizMeta));
    ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 10.0, 10.0));
    ASSERT_TRUE(
        ExecSql(db.get(),
                "CREATE TABLE vertices (fid INTEGER PRIMARY KEY, geom BLOB, "
                "target_y REAL);"));
    db.reset();

    try {
      TINShiftGeopackageFile::open(ctx.get(), file.string());
      FAIL() << "Expected TINShiftGeopackageException";
    } catch (const TINShiftGeopackageException& ex) {
      EXPECT_THAT(ex.what(),
                  HasSubstr("target_x field missing in table vertices"));
    }
  }

  // 6. Horizontal transformation missing target_y column.
  {
    TempFile file("tin_missing_target_y.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), kHorizMeta));
    ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 10.0, 10.0));
    ASSERT_TRUE(
        ExecSql(db.get(),
                "CREATE TABLE vertices (fid INTEGER PRIMARY KEY, geom BLOB, "
                "target_x REAL);"));
    db.reset();

    try {
      TINShiftGeopackageFile::open(ctx.get(), file.string());
      FAIL() << "Expected TINShiftGeopackageException";
    } catch (const TINShiftGeopackageException& ex) {
      EXPECT_THAT(ex.what(),
                  HasSubstr("target_y field missing in table vertices"));
    }
  }

  // 7. Vertical transformation missing target_z / offset_z.
  {
    TempFile file("tin_missing_vertical_cols.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), kVertMeta));
    ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 10.0, 10.0));
    ASSERT_TRUE(
        ExecSql(db.get(),
                "CREATE TABLE vertices (fid INTEGER PRIMARY KEY, geom BLOB, "
                "source_z REAL);"));
    db.reset();

    try {
      TINShiftGeopackageFile::open(ctx.get(), file.string());
      FAIL() << "Expected TINShiftGeopackageException";
    } catch (const TINShiftGeopackageException& ex) {
      EXPECT_THAT(ex.what(),
                  HasSubstr("(source_z and target_z) or offset_z fields "
                            "missing in table vertices"));
    }
  }

  // 8. Missing triangles_def / rtree_triangles_geom fails sqlite3_prepare_v2.
  {
    TempFile file("tin_missing_triangles_table.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), kVertMeta));
    ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 10.0, 10.0));
    ASSERT_TRUE(
        ExecSql(db.get(),
                "CREATE TABLE vertices (fid INTEGER PRIMARY KEY, geom BLOB, "
                "offset_z REAL);"));
    db.reset();

    EXPECT_THROW(TINShiftGeopackageFile::open(ctx.get(), file.string()),
                 TINShiftGeopackageException);
  }
}

TEST(TinshiftGpkgTest, Horizontal2DForwardInverseCacheAndPublicApiRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  TempFile file("tin_horiz_2d.gpkg");
  ScopedSqlite db = CreateOpenSqlite(file);
  ASSERT_NE(nullptr, db.get());

  constexpr std::string_view kMeta = R"({
    "file_type": "triangulation_file",
    "format_version": "1.0",
    "transformed_components": ["horizontal"],
    "min_shift_x": 0.5,
    "min_shift_y": 1.5,
    "max_shift_x": 2.5,
    "max_shift_y": 3.5,
    "fallback_strategy": "none"
  })";
  ASSERT_TRUE(InsertMetadata(db.get(), kMeta));
  ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 10.0, 10.0));

  // Affine mapping: target_x = x + 1.0, target_y = y + 2.0 on [0,10]x[0,10].
  constexpr VertexRow kVertices[] = {
      {.fid = 1,
       .source_x = 0.0,
       .source_y = 0.0,
       .target_x = 1.0,
       .target_y = 2.0},
      {.fid = 2,
       .source_x = 10.0,
       .source_y = 0.0,
       .target_x = 11.0,
       .target_y = 2.0},
      {.fid = 3,
       .source_x = 0.0,
       .source_y = 10.0,
       .target_x = 1.0,
       .target_y = 12.0},
      {.fid = 4,
       .source_x = 10.0,
       .source_y = 10.0,
       .target_x = 11.0,
       .target_y = 12.0},
      {.fid = 5,
       .source_x = 5.0,
       .source_y = 5.0,
       .target_x = 6.0,
       .target_y = 7.0},
  };
  ASSERT_TRUE(PopulateVertices(db.get(), kVertices, /*include_xy=*/true,
                               /*include_source_target_z=*/false,
                               /*include_offset_z=*/false));

  // Include a degenerate collinear triangle (fid=1: v1, v5, v4) before valid
  // triangles 2 and 3 so computeLambdas exercises the det_T < EPSILON branch.
  constexpr TriangleRow kTriangles[] = {
      {.fid = 1,
       .v1 = 1,
       .v2 = 5,
       .v3 = 4,
       .minx = 0.0,
       .maxx = 10.0,
       .miny = 0.0,
       .maxy = 10.0},
      {.fid = 2,
       .v1 = 1,
       .v2 = 2,
       .v3 = 3,
       .minx = 0.0,
       .maxx = 10.0,
       .miny = 0.0,
       .maxy = 10.0},
      {.fid = 3,
       .v1 = 2,
       .v2 = 4,
       .v3 = 3,
       .minx = 0.0,
       .maxx = 10.0,
       .miny = 0.0,
       .maxy = 10.0},
  };
  ASSERT_TRUE(PopulateTriangles(db.get(), kTriangles));
  db.reset();

  auto gpkg = TINShiftGeopackageFile::open(ctx.get(), file.string());
  ASSERT_NE(nullptr, gpkg.get());
  EXPECT_TRUE(gpkg->hasHorizontalTransformation());
  EXPECT_FALSE(gpkg->hasVerticalTransformation());
  EXPECT_FALSE(gpkg->hasOffsetZ());

  TINShiftGeopackageEvaluator eval(std::move(gpkg));

  // 1. Forward evaluation in Triangle 2 (populates triangle cache).
  double x_out = 0.0;
  double y_out = 0.0;
  double z_out = 0.0;
  ASSERT_TRUE(eval.forward(2.0, 3.0, 42.5, x_out, y_out, z_out));
  EXPECT_NEAR(3.0, x_out, 1e-12);
  EXPECT_NEAR(5.0, y_out, 1e-12);
  EXPECT_NEAR(42.5, z_out, 1e-12);

  // 2. Second forward evaluation in Triangle 2 hits cachedVerticesX_/Y_ fast
  //    path without re-querying SQLite.
  ASSERT_TRUE(eval.forward(3.0, 2.0, 10.0, x_out, y_out, z_out));
  EXPECT_NEAR(4.0, x_out, 1e-12);
  EXPECT_NEAR(4.0, y_out, 1e-12);
  EXPECT_NEAR(10.0, z_out, 1e-12);

  // 3. Forward evaluation in Triangle 3 misses Triangle 2 cache and queries DB.
  ASSERT_TRUE(eval.forward(8.0, 7.0, -5.0, x_out, y_out, z_out));
  EXPECT_NEAR(9.0, x_out, 1e-12);
  EXPECT_NEAR(9.0, y_out, 1e-12);
  EXPECT_NEAR(-5.0, z_out, 1e-12);

  // 4. Inverse evaluations in Triangle 3 (first populates cache, second hits
  //    inverse cache).
  ASSERT_TRUE(eval.inverse(9.0, 9.0, -5.0, x_out, y_out, z_out));
  EXPECT_NEAR(8.0, x_out, 1e-12);
  EXPECT_NEAR(7.0, y_out, 1e-12);
  EXPECT_NEAR(-5.0, z_out, 1e-12);

  ASSERT_TRUE(eval.inverse(8.5, 9.5, 0.0, x_out, y_out, z_out));
  EXPECT_NEAR(7.5, x_out, 1e-12);
  EXPECT_NEAR(7.5, y_out, 1e-12);

  // 5. Out-of-bounds point with FALLBACK_NONE returns false.
  EXPECT_FALSE(eval.forward(-5.0, -5.0, 0.0, x_out, y_out, z_out));
  EXPECT_FALSE(eval.inverse(-5.0, -5.0, 0.0, x_out, y_out, z_out));

  // 6. Verify integration via +proj=tinshift +file=<path>.gpkg.
  const std::string proj_str = "+proj=tinshift +file=" + file.string();
  ScopedPj pj(proj_create(ctx.get(), proj_str.c_str()));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(2.5, 3.5, 100.0, 2026.5));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(3.5, fwd.xyzt.x, 1e-12);
  EXPECT_NEAR(5.5, fwd.xyzt.y, 1e-12);
  EXPECT_NEAR(100.0, fwd.xyzt.z, 1e-12);
  EXPECT_NEAR(2026.5, fwd.xyzt.t, 1e-12);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(2.5, inv.xyzt.x, 1e-12);
  EXPECT_NEAR(3.5, inv.xyzt.y, 1e-12);
  EXPECT_NEAR(100.0, inv.xyzt.z, 1e-12);
  EXPECT_NEAR(2026.5, inv.xyzt.t, 1e-12);

  const PJ_COORD out_of_bounds =
      proj_trans(pj.get(), PJ_FWD, proj_coord(50.0, 50.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(out_of_bounds.xyzt.x));
}

TEST(TinshiftGpkgTest, VerticalOnlyWithOffsetZAndSourceTargetZ) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr std::string_view kVertMeta = R"({
    "file_type": "triangulation_file",
    "format_version": "1.0",
    "transformed_components": ["vertical"],
    "fallback_strategy": "none"
  })";

  constexpr VertexRow kVertices[] = {
      {.fid = 1,
       .source_x = 0.0,
       .source_y = 0.0,
       .source_z = 100.0,
       .target_z = 110.0,
       .offset_z = 10.0},
      {.fid = 2,
       .source_x = 10.0,
       .source_y = 0.0,
       .source_z = 100.0,
       .target_z = 120.0,
       .offset_z = 20.0},
      {.fid = 3,
       .source_x = 0.0,
       .source_y = 10.0,
       .source_z = 100.0,
       .target_z = 130.0,
       .offset_z = 30.0},
  };
  constexpr TriangleRow kTriangles[] = {
      {.fid = 1,
       .v1 = 1,
       .v2 = 2,
       .v3 = 3,
       .minx = 0.0,
       .maxx = 10.0,
       .miny = 0.0,
       .maxy = 10.0},
  };

  // 1. Vertical transformation using offset_z column.
  {
    TempFile file("tin_vert_offset_z.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), kVertMeta));
    ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 10.0, 10.0));
    ASSERT_TRUE(PopulateVertices(db.get(), kVertices, /*include_xy=*/false,
                                 /*include_source_target_z=*/false,
                                 /*include_offset_z=*/true));
    ASSERT_TRUE(PopulateTriangles(db.get(), kTriangles));
    db.reset();

    auto gpkg = TINShiftGeopackageFile::open(ctx.get(), file.string());
    ASSERT_NE(nullptr, gpkg.get());
    EXPECT_FALSE(gpkg->hasHorizontalTransformation());
    EXPECT_TRUE(gpkg->hasVerticalTransformation());
    EXPECT_TRUE(gpkg->hasOffsetZ());

    TINShiftGeopackageEvaluator eval(std::move(gpkg));
    double x_out = 0.0;
    double y_out = 0.0;
    double z_out = 0.0;
    // At (2.5, 2.5): lambda1 = 0.5, lambda2 = 0.25, lambda3 = 0.25 ->
    // interpolated offset = 0.5*10 + 0.25*20 + 0.25*30 = 17.5.
    ASSERT_TRUE(eval.forward(2.5, 2.5, 50.0, x_out, y_out, z_out));
    EXPECT_NEAR(2.5, x_out, 1e-12);
    EXPECT_NEAR(2.5, y_out, 1e-12);
    EXPECT_NEAR(67.5, z_out, 1e-12);

    ASSERT_TRUE(eval.inverse(2.5, 2.5, 67.5, x_out, y_out, z_out));
    EXPECT_NEAR(2.5, x_out, 1e-12);
    EXPECT_NEAR(2.5, y_out, 1e-12);
    EXPECT_NEAR(50.0, z_out, 1e-12);
  }

  // 2. Vertical transformation using (source_z, target_z) columns.
  {
    TempFile file("tin_vert_source_target_z.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), kVertMeta));
    ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 10.0, 10.0));
    ASSERT_TRUE(PopulateVertices(db.get(), kVertices, /*include_xy=*/false,
                                 /*include_source_target_z=*/true,
                                 /*include_offset_z=*/false));
    ASSERT_TRUE(PopulateTriangles(db.get(), kTriangles));
    db.reset();

    auto gpkg = TINShiftGeopackageFile::open(ctx.get(), file.string());
    ASSERT_NE(nullptr, gpkg.get());
    EXPECT_FALSE(gpkg->hasHorizontalTransformation());
    EXPECT_TRUE(gpkg->hasVerticalTransformation());
    EXPECT_FALSE(gpkg->hasOffsetZ());

    TINShiftGeopackageEvaluator eval(std::move(gpkg));
    double x_out = 0.0;
    double y_out = 0.0;
    double z_out = 0.0;
    ASSERT_TRUE(eval.forward(2.5, 2.5, 50.0, x_out, y_out, z_out));
    EXPECT_NEAR(67.5, z_out, 1e-12);

    ASSERT_TRUE(eval.inverse(2.5, 2.5, 67.5, x_out, y_out, z_out));
    EXPECT_NEAR(50.0, z_out, 1e-12);
  }
}

TEST(TinshiftGpkgTest, CombinedHorizontalAndVertical3DBothZRepresentations) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr std::string_view kMeta3D = R"({
    "file_type": "triangulation_file",
    "format_version": "1.0",
    "transformed_components": ["horizontal", "vertical"],
    "min_shift_x": 0.5,
    "min_shift_y": 1.5,
    "max_shift_x": 1.5,
    "max_shift_y": 2.5,
    "fallback_strategy": "none"
  })";

  constexpr VertexRow kVertices[] = {
      {.fid = 1,
       .source_x = 0.0,
       .source_y = 0.0,
       .target_x = 1.0,
       .target_y = 2.0,
       .source_z = 10.0,
       .target_z = 14.0,
       .offset_z = 4.0},
      {.fid = 2,
       .source_x = 10.0,
       .source_y = 0.0,
       .target_x = 11.0,
       .target_y = 2.0,
       .source_z = 10.0,
       .target_z = 18.0,
       .offset_z = 8.0},
      {.fid = 3,
       .source_x = 0.0,
       .source_y = 10.0,
       .target_x = 1.0,
       .target_y = 12.0,
       .source_z = 10.0,
       .target_z = 22.0,
       .offset_z = 12.0},
  };
  constexpr TriangleRow kTriangles[] = {
      {.fid = 1,
       .v1 = 1,
       .v2 = 2,
       .v3 = 3,
       .minx = 0.0,
       .maxx = 10.0,
       .miny = 0.0,
       .maxy = 10.0},
  };

  for (bool use_source_target_z : {true, false}) {
    TempFile file(use_source_target_z ? "tin_3d_stz.gpkg" : "tin_3d_oz.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());
    ASSERT_TRUE(InsertMetadata(db.get(), kMeta3D));
    ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 10.0, 10.0));
    ASSERT_TRUE(
        PopulateVertices(db.get(), kVertices, /*include_xy=*/true,
                         /*include_source_target_z=*/use_source_target_z,
                         /*include_offset_z=*/!use_source_target_z));
    ASSERT_TRUE(PopulateTriangles(db.get(), kTriangles));
    db.reset();

    auto gpkg = TINShiftGeopackageFile::open(ctx.get(), file.string());
    ASSERT_NE(nullptr, gpkg.get());
    EXPECT_TRUE(gpkg->hasHorizontalTransformation());
    EXPECT_TRUE(gpkg->hasVerticalTransformation());
    EXPECT_EQ(!use_source_target_z, gpkg->hasOffsetZ());

    TINShiftGeopackageEvaluator eval(std::move(gpkg));
    double x_out = 0.0;
    double y_out = 0.0;
    double z_out = 0.0;
    // At (2.5, 2.5): lambda = (0.5, 0.25, 0.25), z shift = 0.5*4 + 0.25*8 +
    // 0.25*12 = 7.0.
    ASSERT_TRUE(eval.forward(2.5, 2.5, 100.0, x_out, y_out, z_out));
    EXPECT_NEAR(3.5, x_out, 1e-12);
    EXPECT_NEAR(4.5, y_out, 1e-12);
    EXPECT_NEAR(107.0, z_out, 1e-12);

    // Clear forward cache by querying outside first, then test 3D inverse.
    double dummy_x = 0.0;
    double dummy_y = 0.0;
    double dummy_z = 0.0;
    EXPECT_FALSE(eval.forward(-10.0, -10.0, 0.0, dummy_x, dummy_y, dummy_z));

    ASSERT_TRUE(eval.inverse(3.5, 4.5, 107.0, x_out, y_out, z_out));
    EXPECT_NEAR(2.5, x_out, 1e-12);
    EXPECT_NEAR(2.5, y_out, 1e-12);
    EXPECT_NEAR(100.0, z_out, 1e-12);
  }
}

TEST(TinshiftGpkgTest, FallbackNearestSideNearestCentroidAndDegenerateCases) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. FALLBACK_NEAREST_SIDE with large num_vertices (forces search radius
  //    doubling), degenerate triangle skip, and far-triangle bbox pruning.
  {
    TempFile file("tin_fallback_nearest_side.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());

    constexpr std::string_view kMeta = R"({
      "file_type": "triangulation_file",
      "format_version": "1.0",
      "transformed_components": ["horizontal"],
      "min_shift_x": 1.0,
      "min_shift_y": 2.0,
      "max_shift_x": 1.0,
      "max_shift_y": 2.0,
      "fallback_strategy": "nearest_side",
      "num_vertices": 5000
    })";
    ASSERT_TRUE(InsertMetadata(db.get(), kMeta));
    ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 100.0, 100.0));

    constexpr VertexRow kVertices[] = {
        {.fid = 1,
         .source_x = 0.0,
         .source_y = 0.0,
         .target_x = 1.0,
         .target_y = 2.0},
        {.fid = 2,
         .source_x = 10.0,
         .source_y = 0.0,
         .target_x = 11.0,
         .target_y = 2.0},
        {.fid = 3,
         .source_x = 0.0,
         .source_y = 10.0,
         .target_x = 1.0,
         .target_y = 12.0},
        // Duplicate of vertex 1 for a zero-length-side degenerate triangle.
        {.fid = 4,
         .source_x = 0.0,
         .source_y = 0.0,
         .target_x = 1.0,
         .target_y = 2.0},
        // Distant triangle vertices at [80..90]x[80..90].
        {.fid = 5,
         .source_x = 80.0,
         .source_y = 80.0,
         .target_x = 81.0,
         .target_y = 82.0},
        {.fid = 6,
         .source_x = 90.0,
         .source_y = 80.0,
         .target_x = 91.0,
         .target_y = 82.0},
        {.fid = 7,
         .source_x = 80.0,
         .source_y = 90.0,
         .target_x = 81.0,
         .target_y = 92.0},
    };
    ASSERT_TRUE(PopulateVertices(db.get(), kVertices, /*include_xy=*/true,
                                 /*include_source_target_z=*/false,
                                 /*include_offset_z=*/false));

    // Triangle 1: degenerate (v1, v4, v2 -> dist12 == 0).
    // Triangle 2: valid (v1, v2, v3).
    // Triangle 3: distant triangle whose rtree entry spans [0, 100] so it is
    //             fetched in the same query and pruned by the closest_dist bbox
    //             check.
    constexpr TriangleRow kTriangles[] = {
        {.fid = 1,
         .v1 = 1,
         .v2 = 4,
         .v3 = 2,
         .minx = 2.0,
         .maxx = 10.0,
         .miny = 2.0,
         .maxy = 10.0},
        {.fid = 2,
         .v1 = 1,
         .v2 = 2,
         .v3 = 3,
         .minx = 2.0,
         .maxx = 10.0,
         .miny = 2.0,
         .maxy = 10.0},
        {.fid = 3,
         .v1 = 5,
         .v2 = 6,
         .v3 = 7,
         .minx = 2.0,
         .maxx = 100.0,
         .miny = 2.0,
         .maxy = 100.0},
    };
    ASSERT_TRUE(PopulateTriangles(db.get(), kTriangles));
    db.reset();

    auto gpkg = TINShiftGeopackageFile::open(ctx.get(), file.string());
    ASSERT_NE(nullptr, gpkg.get());
    TINShiftGeopackageEvaluator eval(std::move(gpkg));

    double x_out = 0.0;
    double y_out = 0.0;
    double z_out = 0.0;

    // Outside side 2-3 (hypotenuse): closest to side 2-3 (0 < t < 1).
    ASSERT_TRUE(eval.forward(6.0, 6.0, 0.0, x_out, y_out, z_out));
    EXPECT_NEAR(7.0, x_out, 1e-12);
    EXPECT_NEAR(8.0, y_out, 1e-12);

    // Outside side 1-3 (left edge): closest to side 1-3 (0 < t < 1), while
    // side 1-2 has t <= 0 and side 2-3 has t >= 1.
    ASSERT_TRUE(eval.forward(-2.0, 5.0, 0.0, x_out, y_out, z_out));
    EXPECT_NEAR(-1.0, x_out, 1e-12);
    EXPECT_NEAR(7.0, y_out, 1e-12);

    // Inverse outside triangle also uses FALLBACK_NEAREST_SIDE.
    ASSERT_TRUE(eval.inverse(-1.0, 7.0, 0.0, x_out, y_out, z_out));
    EXPECT_NEAR(-2.0, x_out, 1e-12);
    EXPECT_NEAR(5.0, y_out, 1e-12);
  }

  // 2. FALLBACK_NEAREST_CENTROID in forward and inverse directions.
  {
    TempFile file("tin_fallback_nearest_centroid.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());

    constexpr std::string_view kMeta = R"({
      "file_type": "triangulation_file",
      "format_version": "1.0",
      "transformed_components": ["horizontal"],
      "min_shift_x": 1.0,
      "min_shift_y": 2.0,
      "max_shift_x": 1.0,
      "max_shift_y": 2.0,
      "fallback_strategy": "nearest_centroid",
      "num_vertices": 3
    })";
    ASSERT_TRUE(InsertMetadata(db.get(), kMeta));
    ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 10.0, 10.0));

    constexpr VertexRow kVertices[] = {
        {.fid = 1,
         .source_x = 0.0,
         .source_y = 0.0,
         .target_x = 1.0,
         .target_y = 2.0},
        {.fid = 2,
         .source_x = 10.0,
         .source_y = 0.0,
         .target_x = 11.0,
         .target_y = 2.0},
        {.fid = 3,
         .source_x = 0.0,
         .source_y = 10.0,
         .target_x = 1.0,
         .target_y = 12.0},
    };
    ASSERT_TRUE(PopulateVertices(db.get(), kVertices, /*include_xy=*/true,
                                 /*include_source_target_z=*/false,
                                 /*include_offset_z=*/false));
    constexpr TriangleRow kTriangles[] = {
        {.fid = 1,
         .v1 = 1,
         .v2 = 2,
         .v3 = 3,
         .minx = 0.0,
         .maxx = 10.0,
         .miny = 0.0,
         .maxy = 10.0},
    };
    ASSERT_TRUE(PopulateTriangles(db.get(), kTriangles));
    db.reset();

    auto gpkg = TINShiftGeopackageFile::open(ctx.get(), file.string());
    ASSERT_NE(nullptr, gpkg.get());
    TINShiftGeopackageEvaluator eval(std::move(gpkg));

    double x_out = 0.0;
    double y_out = 0.0;
    double z_out = 0.0;
    ASSERT_TRUE(eval.forward(-3.0, -4.0, 0.0, x_out, y_out, z_out));
    EXPECT_NEAR(-2.0, x_out, 1e-12);
    EXPECT_NEAR(-2.0, y_out, 1e-12);

    ASSERT_TRUE(eval.inverse(-2.0, -2.0, 0.0, x_out, y_out, z_out));
    EXPECT_NEAR(-3.0, x_out, 1e-12);
    EXPECT_NEAR(-4.0, y_out, 1e-12);
  }

  // 3. Fallback when only degenerate triangles exist returns false.
  {
    TempFile file("tin_fallback_only_degenerate.gpkg");
    ScopedSqlite db = CreateOpenSqlite(file);
    ASSERT_NE(nullptr, db.get());

    constexpr std::string_view kMeta = R"({
      "file_type": "triangulation_file",
      "format_version": "1.0",
      "transformed_components": ["vertical"],
      "fallback_strategy": "nearest_centroid",
      "num_vertices": 3
    })";
    ASSERT_TRUE(InsertMetadata(db.get(), kMeta));
    ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 10.0, 10.0));

    constexpr VertexRow kVertices[] = {
        {.fid = 1, .source_x = 0.0, .source_y = 0.0, .offset_z = 1.0},
        {.fid = 2, .source_x = 0.0, .source_y = 0.0, .offset_z = 2.0},
        {.fid = 3, .source_x = 10.0, .source_y = 10.0, .offset_z = 3.0},
    };
    ASSERT_TRUE(PopulateVertices(db.get(), kVertices, /*include_xy=*/false,
                                 /*include_source_target_z=*/false,
                                 /*include_offset_z=*/true));
    constexpr TriangleRow kTriangles[] = {
        {.fid = 1,
         .v1 = 1,
         .v2 = 2,
         .v3 = 3,
         .minx = 0.0,
         .maxx = 10.0,
         .miny = 0.0,
         .maxy = 10.0},
    };
    ASSERT_TRUE(PopulateTriangles(db.get(), kTriangles));
    db.reset();

    auto gpkg = TINShiftGeopackageFile::open(ctx.get(), file.string());
    ASSERT_NE(nullptr, gpkg.get());
    TINShiftGeopackageEvaluator eval(std::move(gpkg));

    double x_out = 0.0;
    double y_out = 0.0;
    double z_out = 0.0;
    EXPECT_FALSE(eval.forward(-5.0, -5.0, 0.0, x_out, y_out, z_out));
  }
}

TEST(TinshiftGpkgTest, CorruptedGeometryBlobReturnsFalseInAllSearchPaths) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  TempFile file("tin_corrupted_geom_blob.gpkg");
  ScopedSqlite db = CreateOpenSqlite(file);
  ASSERT_NE(nullptr, db.get());

  constexpr std::string_view kMeta = R"({
    "file_type": "triangulation_file",
    "format_version": "1.0",
    "transformed_components": ["horizontal"],
    "min_shift_x": 0.0,
    "min_shift_y": 0.0,
    "max_shift_x": 0.0,
    "max_shift_y": 0.0,
    "fallback_strategy": "nearest_side",
    "num_vertices": 3
  })";
  ASSERT_TRUE(InsertMetadata(db.get(), kMeta));
  ASSERT_TRUE(InsertContentsBbox(db.get(), 0.0, 0.0, 10.0, 10.0));

  // Vertex 1 has a truncated 4-byte geometry blob instead of 29 bytes.
  const VertexRow kVertices[] = {
      {.fid = 1,
       .source_x = 0.0,
       .source_y = 0.0,
       .target_x = 0.0,
       .target_y = 0.0,
       .custom_geom = {1, 2, 3, 4}},
      {.fid = 2,
       .source_x = 10.0,
       .source_y = 0.0,
       .target_x = 10.0,
       .target_y = 0.0},
      {.fid = 3,
       .source_x = 0.0,
       .source_y = 10.0,
       .target_x = 0.0,
       .target_y = 10.0},
  };
  ASSERT_TRUE(PopulateVertices(db.get(), kVertices, /*include_xy=*/true,
                               /*include_source_target_z=*/false,
                               /*include_offset_z=*/false));
  constexpr TriangleRow kTriangles[] = {
      {.fid = 1,
       .v1 = 1,
       .v2 = 2,
       .v3 = 3,
       .minx = 0.0,
       .maxx = 10.0,
       .miny = 0.0,
       .maxy = 10.0},
  };
  ASSERT_TRUE(PopulateTriangles(db.get(), kTriangles));
  db.reset();

  auto gpkg = TINShiftGeopackageFile::open(ctx.get(), file.string());
  ASSERT_NE(nullptr, gpkg.get());
  TINShiftGeopackageEvaluator eval(std::move(gpkg));

  double x_out = 0.0;
  double y_out = 0.0;
  double z_out = 0.0;

  // 1. Forward inside R-tree bbox: getXY fails in primary loop.
  EXPECT_FALSE(eval.forward(2.0, 2.0, 0.0, x_out, y_out, z_out));

  // 2. Forward outside R-tree bbox: primary loop finds 0 rows, getXY fails in
  //    fallback loop.
  EXPECT_FALSE(eval.forward(-2.0, -2.0, 0.0, x_out, y_out, z_out));

  // 3. Inverse inside target triangle: getXY reads target_x/target_y doubles
  //    and succeeds, then collectValsVertex fails on truncated geom in primary
  //    loop.
  EXPECT_FALSE(eval.inverse(2.0, 2.0, 0.0, x_out, y_out, z_out));

  // 4. Inverse outside target triangle: getXY succeeds in fallback loop, then
  //    collectValsVertex fails on truncated geom in fallback loop.
  EXPECT_FALSE(eval.inverse(-2.0, -2.0, 0.0, x_out, y_out, z_out));
}

}  // namespace
}  // namespace proj
