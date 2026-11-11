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
// Unit tests for TIN-based transformations (+proj=tinshift) in
// proj/src/transformations/tinshift.cpp.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/tinshift.cpp

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/google/util/temp_file.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"
#include "third_party/sqlite/sqlite3.h"

namespace proj {
namespace {

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

// Encodes a 29-byte little-endian GeoPackage Point binary geometry blob.
std::vector<uint8_t> MakeGpkgPointBlob(double x, double y) {
  std::vector<uint8_t> blob = {
      'G', 'P', 0, 1, 0, 0, 0, 0,  // 8-byte GP header (LE, srs_id=0)
      1,   1,   0, 0, 0,           // 5-byte WKB Point header (LE, type=1)
  };
  internal::AppendLeFloat64(blob, x);
  internal::AppendLeFloat64(blob, y);
  return blob;
}

bool CreateMinimalVerticalTinGpkg(const TempFile& file) {
  sqlite3* raw_db = nullptr;
  if (sqlite3_open(file.c_str(), &raw_db) != SQLITE_OK) {
    if (raw_db != nullptr) {
      sqlite3_close(raw_db);
    }
    return false;
  }
  ScopedSqlite db(raw_db);

  constexpr char kSchemaSql[] =
      "CREATE TABLE gpkg_metadata (md_standard_uri TEXT, metadata TEXT);"
      "INSERT INTO gpkg_metadata VALUES ('https://proj.org', "
      "'{\"file_type\":\"triangulation_file\",\"format_version\":\"1.0\","
      "\"transformed_components\":[\"vertical\"]}');"
      "CREATE TABLE gpkg_contents ("
      "table_name TEXT, min_x REAL, min_y REAL, max_x REAL, max_y REAL);"
      "INSERT INTO gpkg_contents VALUES ('vertices', 0.0, 0.0, 10.0, 10.0);"
      "CREATE TABLE vertices ("
      "fid INTEGER PRIMARY KEY, geom BLOB, offset_z REAL);"
      "CREATE TABLE triangles_def ("
      "fid INTEGER PRIMARY KEY, idx_vertex1 INTEGER, idx_vertex2 INTEGER, "
      "idx_vertex3 INTEGER);"
      "INSERT INTO triangles_def VALUES (1, 1, 2, 3);"
      "CREATE TABLE rtree_triangles_geom ("
      "id INTEGER PRIMARY KEY, minx REAL, maxx REAL, miny REAL, maxy REAL);"
      "INSERT INTO rtree_triangles_geom VALUES (1, 0.0, 10.0, 0.0, 10.0);";
  if (sqlite3_exec(db.get(), kSchemaSql, nullptr, nullptr, nullptr) !=
      SQLITE_OK) {
    return false;
  }

  struct Vertex {
    int fid;
    double x;
    double y;
    double offset_z;
  };
  constexpr Vertex kVertices[] = {
      {.fid = 1, .x = 0.0, .y = 0.0, .offset_z = 5.0},
      {.fid = 2, .x = 10.0, .y = 0.0, .offset_z = 5.0},
      {.fid = 3, .x = 0.0, .y = 10.0, .offset_z = 5.0},
  };
  for (const auto& v : kVertices) {
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(
            db.get(),
            "INSERT INTO vertices (fid, geom, offset_z) VALUES (?, ?, ?);", -1,
            &raw_stmt, nullptr) != SQLITE_OK) {
      return false;
    }
    ScopedStmt stmt(raw_stmt);
    const std::vector<uint8_t> geom = MakeGpkgPointBlob(v.x, v.y);
    sqlite3_bind_int(stmt.get(), 1, v.fid);
    sqlite3_bind_blob(stmt.get(), 2, geom.data(), static_cast<int>(geom.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt.get(), 3, v.offset_z);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      return false;
    }
  }
  return true;
}

constexpr std::string_view kValidTinJson = R"json({
  "file_type": "triangulation_file",
  "format_version": "1.0",
  "input_crs": "EPSG:2393",
  "output_crs": "EPSG:3067",
  "transformed_components": [ "horizontal", "vertical" ],
  "vertices_columns": [ "source_x", "source_y", "target_x", "target_y", "offset_z" ],
  "triangles_columns": [ "idx_vertex1", "idx_vertex2", "idx_vertex3" ],
  "vertices": [
    [ 0.0, 0.0, 100.0, 200.0, 10.0 ],
    [ 10.0, 0.0, 110.0, 200.0, 20.0 ],
    [ 0.0, 10.0, 100.0, 210.0, 30.0 ]
  ],
  "triangles": [
    [ 0, 1, 2 ]
  ]
})json";

TEST(TinshiftTest, MissingFileAndJsonValidationErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);

  // 1. Missing +file= parameter returns PROJ_ERR_INVALID_OP_MISSING_ARG.
  ScopedPj missing_file_arg(proj_create(ctx.get(), "+proj=tinshift"));
  EXPECT_EQ(nullptr, missing_file_arg.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("+file= should be specified."));

  // 2. Non-existent JSON file returns
  //    PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID.
  logs.Clear();
  proj_context_errno_set(ctx.get(), 0);
  ScopedPj missing_json(
      proj_create(ctx.get(), "+proj=tinshift +file=/mem/does_not_exist.json"));
  EXPECT_EQ(nullptr, missing_json.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("Cannot open /mem/does_not_exist.json"));

  // 3. Corrupt / incomplete JSON model fails in TINShiftJSONFile::parse.
  fs.AddTextFile("/mem/bad_model.json", R"({"file_type": "wrong"})");
  logs.Clear();
  proj_context_errno_set(ctx.get(), 0);
  ScopedPj bad_json(
      proj_create(ctx.get(), "+proj=tinshift +file=/mem/bad_model.json"));
  EXPECT_EQ(nullptr, bad_json.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("invalid model:"));
}

TEST(TinshiftTest, JsonFileTooLargeAndShortReadErrors) {
  struct MockFileState {
    unsigned long long reported_size = 0;  // NOLINT(runtime/int)
    size_t bytes_to_read = 0;
    unsigned long long pos = 0;  // NOLINT(runtime/int)
  };

  constexpr PROJ_FILE_API kMockFileApi = {
      /*version=*/1,
      /*open_cbk=*/
      +[](PJ_CONTEXT*, const char*, PROJ_OPEN_ACCESS,
          void* user_data) -> PROJ_FILE_HANDLE* {
        auto* state = static_cast<MockFileState*>(user_data);
        state->pos = 0;
        return reinterpret_cast<PROJ_FILE_HANDLE*>(state);
      },
      /*read_cbk=*/
      +[](PJ_CONTEXT*, PROJ_FILE_HANDLE* handle, void* buffer,
          size_t size_bytes, void*) -> size_t {
        auto* state = reinterpret_cast<MockFileState*>(handle);
        const size_t n = std::min(size_bytes, state->bytes_to_read);
        if (n > 0 && buffer != nullptr) {
          std::fill_n(static_cast<char*>(buffer), n, ' ');
        }
        return n;
      },
      /*write_cbk=*/
      +[](PJ_CONTEXT*, PROJ_FILE_HANDLE*, const void*, size_t,
          void*) -> size_t { return 0; },
      /*seek_cbk=*/
      // NOLINTNEXTLINE(runtime/int)
      +[](PJ_CONTEXT*, PROJ_FILE_HANDLE* handle, long long offset, int whence,
          void*) -> int {
        auto* state = reinterpret_cast<MockFileState*>(handle);
        if (whence == SEEK_END) {
          state->pos = state->reported_size;
        } else if (whence == SEEK_SET) {
          state->pos = static_cast<unsigned long long>(offset);
        }
        return 1;
      },
      /*tell_cbk=*/
      // NOLINTNEXTLINE(runtime/int)
      +[](PJ_CONTEXT*, PROJ_FILE_HANDLE* handle, void*) -> unsigned long long {
        return reinterpret_cast<MockFileState*>(handle)->pos;
      },
      /*close_cbk=*/+[](PJ_CONTEXT*, PROJ_FILE_HANDLE*, void*) {},
      /*exists_cbk=*/+[](PJ_CONTEXT*, const char*, void*) -> int { return 1; },
      /*mkdir_cbk=*/+[](PJ_CONTEXT*, const char*, void*) -> int { return 0; },
      /*unlink_cbk=*/+[](PJ_CONTEXT*, const char*, void*) -> int { return 0; },
      /*rename_cbk=*/
      +[](PJ_CONTEXT*, const char*, const char*, void*) -> int { return 0; },
  };

  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  MockFileState state;
  ASSERT_NE(0, proj_context_set_fileapi(ctx.get(), &kMockFileApi, &state));
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);

  // 1. File size > 100 MB threshold is rejected before allocation.
  state.reported_size = 100ULL * 1024ULL * 1024ULL + 1ULL;
  state.bytes_to_read = 0;
  ScopedPj too_large(
      proj_create(ctx.get(), "+proj=tinshift +file=/mem/huge.json"));
  EXPECT_EQ(nullptr, too_large.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("File /mem/huge.json too large"));

  // 2. Short read (file->read != jsonStr.size()) is rejected.
  logs.Clear();
  proj_context_errno_set(ctx.get(), 0);
  state.reported_size = 32;
  state.bytes_to_read = 8;
  ScopedPj short_read(
      proj_create(ctx.get(), "+proj=tinshift +file=/mem/short.json"));
  EXPECT_EQ(nullptr, short_read.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("Cannot read /mem/short.json"));
}

TEST(TinshiftTest,
     JsonEvaluatorForwardInverseRoundTripOutOfBoundsAndDestructor) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  fs.AddTextFile("/mem/valid_tin.json", kValidTinJson);

  ScopedPj pj(
      proj_create(ctx.get(), "+proj=tinshift +file=/mem/valid_tin.json"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));

  // 1. Point (2.0, 3.0, 50.0, 2026.0) inside triangle (0,0)-(10,0)-(0,10):
  //    lambda1 = 0.5, lambda2 = 0.2, lambda3 = 0.3
  //    x_out = 102.0, y_out = 203.0
  //    dz = 0.5*10 + 0.2*20 + 0.3*30 = 18.0 -> z_out = 68.0.
  const PJ_COORD in = proj_coord(2.0, 3.0, 50.0, 2026.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(102.0, fwd.xyzt.x, 1e-9);
  EXPECT_NEAR(203.0, fwd.xyzt.y, 1e-9);
  EXPECT_NEAR(68.0, fwd.xyzt.z, 1e-9);
  EXPECT_DOUBLE_EQ(2026.0, fwd.xyzt.t);

  // 2. Inverse round-trip back to (2.0, 3.0, 50.0, 2026.0).
  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.xyzt.x, inv.xyzt.x, 1e-9);
  EXPECT_NEAR(in.xyzt.y, inv.xyzt.y, 1e-9);
  EXPECT_NEAR(in.xyzt.z, inv.xyzt.z, 1e-9);
  EXPECT_DOUBLE_EQ(in.xyzt.t, inv.xyzt.t);

  // 3. Point outside triangulation returns proj_coord_error() in both forward
  //    and reverse.
  const PJ_COORD outside = proj_coord(50.0, 50.0, 10.0, 0.0);
  const PJ_COORD fwd_out = proj_trans(pj.get(), PJ_FWD, outside);
  EXPECT_TRUE(std::isinf(fwd_out.xyzt.x));

  const PJ_COORD inv_out = proj_trans(pj.get(), PJ_INV, outside);
  EXPECT_TRUE(std::isinf(inv_out.xyzt.x));
}

TEST(TinshiftTest, GeopackageBranchesHttpMissingCorruptAndValidFile) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);

  // 1. http:// and https:// .gpkg / .GPKG URLs enter the URL branch and fail
  //    when TINShiftGeopackageFile::open throws (networking disabled in
  //    google3).
  for (const char* def : {
           "+proj=tinshift +file=http://example.com/model.gpkg",
           "+proj=tinshift +file=https://example.com/model.GPKG",
       }) {
    logs.Clear();
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj pj_url(proj_create(ctx.get(), def));
    EXPECT_EQ(nullptr, pj_url.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_context_errno(ctx.get()));
    EXPECT_TRUE(logs.ContainsMessage("invalid file:"));
  }

  // 2. Non-existent local .gpkg file fails in pj_find_file with "non existing
  //    file".
  logs.Clear();
  proj_context_errno_set(ctx.get(), 0);
  ScopedPj pj_missing_gpkg(
      proj_create(ctx.get(), "+proj=tinshift +file=nonexistent_tin.gpkg"));
  EXPECT_EQ(nullptr, pj_missing_gpkg.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("non existing file"));

  // 3. Corrupt local .gpkg file is found by pj_find_file and caught in
  //    TINShiftGeopackageFile::open.
  TempFile corrupt_gpkg("tinshift_test_corrupt.gpkg");
  ASSERT_TRUE(WriteTextFile(corrupt_gpkg.path(), "not a sqlite file"));
  logs.Clear();
  proj_context_errno_set(ctx.get(), 0);
  const std::string corrupt_def =
      std::string("+proj=tinshift +file=") + corrupt_gpkg.string();
  ScopedPj pj_corrupt(proj_create(ctx.get(), corrupt_def.c_str()));
  EXPECT_EQ(nullptr, pj_corrupt.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("invalid file:"));

  // 4. Valid local .gpkg file constructs TINShiftGeopackageEvaluator and
  //    transforms coordinates in forward and reverse.
  TempFile valid_gpkg("tinshift_test_valid.gpkg");
  ASSERT_TRUE(CreateMinimalVerticalTinGpkg(valid_gpkg));
  proj_context_errno_set(ctx.get(), 0);
  const std::string valid_def =
      std::string("+proj=tinshift +file=") + valid_gpkg.string();
  ScopedPj pj_valid(proj_create(ctx.get(), valid_def.c_str()));
  ASSERT_NE(nullptr, pj_valid.get());

  const PJ_COORD in = proj_coord(2.0, 2.0, 100.0, 0.0);
  const PJ_COORD fwd = proj_trans(pj_valid.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj_valid.get()));
  EXPECT_NEAR(2.0, fwd.xyz.x, 1e-9);
  EXPECT_NEAR(2.0, fwd.xyz.y, 1e-9);
  EXPECT_NEAR(105.0, fwd.xyz.z, 1e-9);

  const PJ_COORD inv = proj_trans(pj_valid.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj_valid.get()));
  EXPECT_NEAR(100.0, inv.xyz.z, 1e-9);
}

}  // namespace
}  // namespace proj
