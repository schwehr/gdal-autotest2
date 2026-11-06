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
// Tests for SQLite utility functions (ogrsqliteutility.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqliteutility.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogrsqliteutility.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogr_sqlite.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/sqlite/sqlite3.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/sqlite_utility_test";

class DummyPixelBand final : public GDALRasterBand {
 public:
  DummyPixelBand(GDALDataType eType, int64_t val) : val_(val) {
    eDataType = eType;
    nBlockXSize = 2;
    nBlockYSize = 2;
  }

  CPLErr IReadBlock(int, int, void* pImage) override {
    if (eDataType == GDT_Byte) {
      static_cast<GByte*>(pImage)[0] = static_cast<GByte>(val_);
    } else if (eDataType == GDT_Int64) {
      static_cast<int64_t*>(pImage)[0] = val_;
    } else if (eDataType == GDT_UInt64) {
      static_cast<uint64_t*>(pImage)[0] = static_cast<uint64_t>(val_);
    }
    return CE_None;
  }

  CPLErr IRasterIO(GDALRWFlag, int, int, int, int, void* pData, int, int,
                   GDALDataType eBufType, GSpacing, GSpacing,
                   GDALRasterIOExtraArg*) override {
    if (eBufType == GDT_Int64) {
      *static_cast<int64_t*>(pData) = val_;
      return CE_None;
    }
    if (eBufType == GDT_UInt64) {
      *static_cast<uint64_t*>(pData) = static_cast<uint64_t>(val_);
      return CE_None;
    }
    return CE_Failure;
  }

  CPLErr InterpolateAtPoint(double, double, GDALRIOResampleAlg,
                            double* pdfRealValue,
                            double* = nullptr) const override {
    *pdfRealValue = static_cast<double>(val_);
    return CE_None;
  }

 private:
  int64_t val_;
};

class DummyPixelDataset final : public GDALDataset {
 public:
  DummyPixelDataset(bool has_geotransform, bool invertible_gt)
      : has_gt_(has_geotransform), invertible_gt_(invertible_gt) {
    nRasterXSize = 2;
    nRasterYSize = 2;
    SetBand(1, new DummyPixelBand(GDT_Byte, 42));
    SetBand(2, new DummyPixelBand(GDT_Int64, 1234567890123LL));
    SetBand(3, new DummyPixelBand(GDT_UInt64, 0x9000000000000000ULL));
    SetBand(4, new DummyPixelBand(GDT_UInt64, 100ULL));
  }

  CPLErr GetGeoTransform(GDALGeoTransform& gt) const override {
    if (!has_gt_) return CE_Failure;
    if (invertible_gt_) {
      gt[0] = 100.0;
      gt[1] = 1.0;
      gt[2] = 0.0;
      gt[3] = 200.0;
      gt[4] = 0.0;
      gt[5] = -1.0;
    } else {
      for (int i = 0; i < 6; ++i) gt[i] = 0.0;
    }
    return CE_None;
  }

 private:
  bool has_gt_;
  bool invertible_gt_;
};

class OGRSQLiteUtilityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    VSIMkdir(kTestDir, 0777);
    ASSERT_EQ(
        SQLITE_OK,
        sqlite3_open_v2(":memory:", &hDB_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr));
  }

  void TearDown() override {
    if (hDB_ != nullptr) {
      sqlite3_close(hDB_);
    }
    VSIRmdirRecursive(kTestDir);
  }

  sqlite3* hDB_ = nullptr;
};

TEST_F(OGRSQLiteUtilityTest, SQLCommandAndQuery) {
  // SQLCommand valid
  EXPECT_EQ(
      OGRERR_NONE,
      SQLCommand(
          hDB_,
          "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, val REAL);"));
  // SQLCommand invalid
  EXPECT_NE(OGRERR_NONE, SQLCommand(hDB_, "CREATE MALFORMED TABLE;"));

  EXPECT_EQ(OGRERR_NONE,
            SQLCommand(hDB_, "INSERT INTO t VALUES (1, 'Alice', 1.5);"));
  EXPECT_EQ(OGRERR_NONE,
            SQLCommand(hDB_, "INSERT INTO t VALUES (2, 'Bob', 2.5);"));
  EXPECT_EQ(OGRERR_NONE,
            SQLCommand(hDB_, "INSERT INTO t VALUES (3, NULL, NULL);"));

  // SQLQuery valid
  std::unique_ptr<SQLResult> poResult =
      SQLQuery(hDB_, "SELECT id, name, val FROM t ORDER BY id;");
  ASSERT_NE(nullptr, poResult);
  EXPECT_EQ(3, poResult->RowCount());
  EXPECT_EQ(3, poResult->ColCount());

  // (col, row) argument order
  EXPECT_STREQ("1", poResult->GetValue(0, 0));
  EXPECT_STREQ("Alice", poResult->GetValue(1, 0));
  EXPECT_STREQ("1.5", poResult->GetValue(2, 0));
  EXPECT_EQ(1, poResult->GetValueAsInteger(0, 0));
  EXPECT_DOUBLE_EQ(1.5, poResult->GetValueAsDouble(2, 0));

  EXPECT_STREQ("2", poResult->GetValue(0, 1));
  EXPECT_STREQ("Bob", poResult->GetValue(1, 1));
  EXPECT_EQ(2, poResult->GetValueAsInteger(0, 1));
  EXPECT_DOUBLE_EQ(2.5, poResult->GetValueAsDouble(2, 1));

  // NULL values
  EXPECT_EQ(nullptr, poResult->GetValue(1, 2));
  EXPECT_EQ(nullptr, poResult->GetValue(2, 2));
  EXPECT_EQ(0, poResult->GetValueAsInteger(1, 2));
  EXPECT_DOUBLE_EQ(0.0, poResult->GetValueAsDouble(2, 2));

  // LimitRowCount
  poResult->LimitRowCount(1);
  EXPECT_EQ(1, poResult->RowCount());

  // SQLQuery invalid
  std::unique_ptr<SQLResult> poBad =
      SQLQuery(hDB_, "SELECT * FROM non_existent_tbl;");
  EXPECT_EQ(nullptr, poBad);

  // SQLGetInteger / SQLGetInteger64
  OGRErr err = OGRERR_NONE;
  EXPECT_EQ(
      1, SQLGetInteger(hDB_, "SELECT id FROM t WHERE name = 'Alice';", &err));
  EXPECT_EQ(OGRERR_NONE, err);

  EXPECT_EQ(
      2, SQLGetInteger64(hDB_, "SELECT id FROM t WHERE name = 'Bob';", &err));
  EXPECT_EQ(OGRERR_NONE, err);

  SQLGetInteger(hDB_, "SELECT id FROM non_existent;", &err);
  EXPECT_NE(OGRERR_NONE, err);

  SQLGetInteger64(hDB_, "SELECT id FROM non_existent;", &err);
  EXPECT_NE(OGRERR_NONE, err);

  // Queries returning 0 rows (hits lines 141-144)
  EXPECT_EQ(0, SQLGetInteger(hDB_, "SELECT id FROM t WHERE 1 = 0;", &err));
  EXPECT_NE(OGRERR_NONE, err);

  EXPECT_EQ(0, SQLGetInteger64(hDB_, "SELECT id FROM t WHERE 1 = 0;", &err));
  EXPECT_NE(OGRERR_NONE, err);

  // SQLPrepareWithError
  sqlite3_stmt* hStmt = nullptr;
  const char* pszTail = nullptr;
  EXPECT_EQ(SQLITE_OK,
            SQLPrepareWithError(hDB_, "SELECT 1;", -1, &hStmt, &pszTail));
  ASSERT_NE(nullptr, hStmt);
  sqlite3_finalize(hStmt);

  EXPECT_NE(SQLITE_OK, SQLPrepareWithError(hDB_, "SELECT BAD_SYNTAX;", -1,
                                           &hStmt, &pszTail));
  EXPECT_EQ(nullptr, hStmt);
}

TEST_F(OGRSQLiteUtilityTest, EscapingAndTokenize) {
  // SQLUnescape
  EXPECT_STREQ("foo", SQLUnescape("'foo'").c_str());
  EXPECT_STREQ("it's", SQLUnescape("'it''s'").c_str());
  EXPECT_STREQ("bar", SQLUnescape("\"bar\"").c_str());
  EXPECT_STREQ("normal", SQLUnescape("normal").c_str());

  // SQLEscapeLiteral returns without surrounding single quotes
  EXPECT_STREQ("", SQLEscapeLiteral("").c_str());
  EXPECT_STREQ("it''s", SQLEscapeLiteral("it's").c_str());
  EXPECT_STREQ("hello", SQLEscapeLiteral("hello").c_str());

  // SQLEscapeName returns without surrounding double quotes
  EXPECT_STREQ("my_col", SQLEscapeName("my_col").c_str());
  EXPECT_STREQ("my\"\"col", SQLEscapeName("my\"col").c_str());

  // SQLTokenize
  char** papszTokens = SQLTokenize("SELECT 'a', \"b\", 123 FROM t;");
  ASSERT_NE(nullptr, papszTokens);
  CPLStringList aosTokens(papszTokens);
  EXPECT_GE(aosTokens.size(), 5);

  // Unspaced parentheses/commas, doubled quotes, and multi-word quoted strings
  char** papszTokens2 =
      SQLTokenize("func(arg1,arg2,'it''s a string',\"col\"\"name\")");
  ASSERT_NE(nullptr, papszTokens2);
  CSLDestroy(papszTokens2);
}

TEST_F(OGRSQLiteUtilityTest, UniqueFieldConstraints) {
  // Table with column UNIQUE, table-level UNIQUE, and UNIQUE INDEX
  const char* pszSchema =
      "CREATE TABLE tbl (\n"
      "  col1 INTEGER UNIQUE,\n"
      "  col2 TEXT,\n"
      "  col3 REAL,\n"
      "  col4 TEXT,\n"
      "  col5 TEXT,\n"
      "  CONSTRAINT uq_col2 UNIQUE (col2),\n"
      "  UNIQUE (col3, col4)\n"
      ");\n"
      "CREATE UNIQUE INDEX idx_col5 ON tbl(col5);\n"
      "CREATE INDEX idx_col2_non_unique ON tbl(col2);\n";

  EXPECT_EQ(OGRERR_NONE, SQLCommand(hDB_, pszSchema));

  // 1. sqlite3* overload
  std::set<std::string> oCols1 = SQLGetUniqueFieldUCConstraints(hDB_, "tbl");
  EXPECT_TRUE(oCols1.count("COL1") > 0);
  EXPECT_TRUE(oCols1.count("COL2") > 0);
  EXPECT_TRUE(oCols1.count("COL5") > 0);
  // Composite unique (col3, col4) should not be reported as single-field
  // constraint
  EXPECT_FALSE(oCols1.count("COL3") > 0);
  EXPECT_FALSE(oCols1.count("COL4") > 0);

  // Non-existent table
  std::set<std::string> oColsNone =
      SQLGetUniqueFieldUCConstraints(hDB_, "non_existent_table");
  EXPECT_TRUE(oColsNone.empty());

  // View
  EXPECT_EQ(OGRERR_NONE,
            SQLCommand(hDB_, "CREATE VIEW v_tbl AS SELECT * FROM tbl;"));
  std::set<std::string> oColsView =
      SQLGetUniqueFieldUCConstraints(hDB_, "v_tbl");
  EXPECT_TRUE(oColsView.empty());

  // 2. sqliteMasterContent overload
  std::vector<SQLSqliteMasterContent> contents;
  SQLSqliteMasterContent c1;
  c1.osType = "table";
  c1.osTableName = "tbl2";
  c1.osSQL =
      "CREATE TABLE tbl2 (\"col\"\"1\" INTEGER UNIQUE, col2 TEXT, "
      "CONSTRAINT uq_multi UNIQUE (\"col\"\"1\", col2), "
      "CONSTRAINT uq UNIQUE (\"col2\"), "
      "CONSTRAINT chk CHECK (col2 != 'a''b' AND (1 = 1)));";
  contents.push_back(c1);

  SQLSqliteMasterContent c2;
  c2.osType = "index";
  c2.osTableName = "tbl2";
  c2.osSQL = "CREATE UNIQUE INDEX idx_col5 ON tbl2(col5);";
  contents.push_back(c2);

  SQLSqliteMasterContent cView;
  cView.osType = "view";
  cView.osTableName = "v_tbl2";
  cView.osSQL = "CREATE VIEW v_tbl2 AS SELECT * FROM tbl2;";
  contents.push_back(cView);

  std::set<std::string> oCols2 =
      SQLGetUniqueFieldUCConstraints(nullptr, "tbl2", contents);
  EXPECT_TRUE(oCols2.count("COL\"1") > 0);
  EXPECT_TRUE(oCols2.count("COL2") > 0);
  EXPECT_TRUE(oCols2.count("COL5") > 0);

  std::set<std::string> oCols2View =
      SQLGetUniqueFieldUCConstraints(nullptr, "v_tbl2", contents);
  EXPECT_TRUE(oCols2View.empty());

  std::set<std::string> oCols2Missing =
      SQLGetUniqueFieldUCConstraints(nullptr, "missing", contents);
  EXPECT_TRUE(oCols2Missing.empty());
}

TEST_F(OGRSQLiteUtilityTest, SpatialFunctionAndRTreeChecks) {
  OGRSQLiteRTreeRequiresTrustedSchemaOn();

  EXPECT_TRUE(OGRSQLiteIsSpatialFunctionReturningGeometry("SetSRID("));
  EXPECT_TRUE(OGRSQLiteIsSpatialFunctionReturningGeometry("ST_SetSRID("));
  EXPECT_TRUE(OGRSQLiteIsSpatialFunctionReturningGeometry("Buffer("));
  EXPECT_TRUE(OGRSQLiteIsSpatialFunctionReturningGeometry("ST_Buffer("));
  EXPECT_TRUE(OGRSQLiteIsSpatialFunctionReturningGeometry("ST_Expand("));
  EXPECT_TRUE(OGRSQLiteIsSpatialFunctionReturningGeometry("CastToXY("));
  EXPECT_TRUE(OGRSQLiteIsSpatialFunctionReturningGeometry("MakePolygon("));
  EXPECT_TRUE(OGRSQLiteIsSpatialFunctionReturningGeometry("gpkgMakePoint("));

  EXPECT_FALSE(OGRSQLiteIsSpatialFunctionReturningGeometry("ST_Area("));
  EXPECT_FALSE(OGRSQLiteIsSpatialFunctionReturningGeometry("ST_Length("));
  EXPECT_FALSE(OGRSQLiteIsSpatialFunctionReturningGeometry("NonExistentFunc("));
}

TEST_F(OGRSQLiteUtilityTest, GdalGetPixelValueCommon) {
  auto poDS = std::make_unique<DummyPixelDataset>(/*has_geotransform=*/true,
                                                  /*invertible_gt=*/true);
  auto poDSNoGT = std::make_unique<DummyPixelDataset>(
      /*has_geotransform=*/false, /*invertible_gt=*/false);
  auto poDSBadGT = std::make_unique<DummyPixelDataset>(
      /*has_geotransform=*/true, /*invertible_gt=*/false);

  auto callback = [](sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    auto* poDSIn = static_cast<GDALDataset*>(sqlite3_user_data(ctx));
    OGRSQLite_gdal_get_pixel_value_common("test_px", ctx, argc, argv, poDSIn);
  };

  ASSERT_EQ(SQLITE_OK,
            sqlite3_create_function(hDB_, "test_px", -1, SQLITE_UTF8,
                                    poDS.get(), callback, nullptr, nullptr));
  ASSERT_EQ(SQLITE_OK, sqlite3_create_function(hDB_, "test_px_nogt", -1,
                                               SQLITE_UTF8, poDSNoGT.get(),
                                               callback, nullptr, nullptr));
  ASSERT_EQ(SQLITE_OK, sqlite3_create_function(hDB_, "test_px_badgt", -1,
                                               SQLITE_UTF8, poDSBadGT.get(),
                                               callback, nullptr, nullptr));

  // 1. Valid Byte band (band 1) pixel mode
  std::unique_ptr<SQLResult> res =
      SQLQuery(hDB_, "SELECT test_px('ds', 1, 'pixel', 0.5, 0.5);");
  ASSERT_NE(nullptr, res);
  EXPECT_DOUBLE_EQ(42.0, res->GetValueAsDouble(0, 0));

  // 2. 6 arguments with resampling algorithm
  res =
      SQLQuery(hDB_, "SELECT test_px('ds', 1, 'pixel', 0.5, 0.5, 'bilinear');");
  ASSERT_NE(nullptr, res);
  EXPECT_DOUBLE_EQ(42.0, res->GetValueAsDouble(0, 0));

  // 3. Int64 band (band 2)
  res = SQLQuery(hDB_, "SELECT test_px('ds', 2, 'pixel', 0, 0);");
  ASSERT_NE(nullptr, res);
  EXPECT_EQ(1234567890123LL,
            SQLGetInteger64(hDB_, "SELECT test_px('ds', 2, 'pixel', 0, 0);",
                            nullptr));

  // 4. UInt64 band (band 3: > INT64_MAX, band 4: <= INT64_MAX)
  res = SQLQuery(hDB_, "SELECT test_px('ds', 3, 'pixel', 0, 0);");
  ASSERT_NE(nullptr, res);
  EXPECT_GT(res->GetValueAsDouble(0, 0), 1e18);

  res = SQLQuery(hDB_, "SELECT test_px('ds', 4, 'pixel', 0, 0);");
  ASSERT_NE(nullptr, res);
  EXPECT_EQ(100, res->GetValueAsInteger(0, 0));

  // 5. Georef mode with valid GT: X=100.5 -> x=0.5, Y=199.5 -> y=0.5
  res = SQLQuery(hDB_, "SELECT test_px('ds', 1, 'georef', 100.5, 199.5);");
  ASSERT_NE(nullptr, res);
  EXPECT_DOUBLE_EQ(42.0, res->GetValueAsDouble(0, 0));

  // 6. Georef with no GT and bad GT
  res = SQLQuery(hDB_, "SELECT test_px_nogt('ds', 1, 'georef', 100.5, 199.5);");
  ASSERT_NE(nullptr, res);
  EXPECT_EQ(nullptr, res->GetValue(0, 0));

  res =
      SQLQuery(hDB_, "SELECT test_px_badgt('ds', 1, 'georef', 100.5, 199.5);");
  ASSERT_NE(nullptr, res);
  EXPECT_EQ(nullptr, res->GetValue(0, 0));

  // 7. Out of bounds coordinates
  res = SQLQuery(hDB_, "SELECT test_px('ds', 1, 'pixel', -1, 0);");
  ASSERT_NE(nullptr, res);
  EXPECT_EQ(nullptr, res->GetValue(0, 0));

  res = SQLQuery(hDB_, "SELECT test_px('ds', 1, 'pixel', 10, 10);");
  ASSERT_NE(nullptr, res);
  EXPECT_EQ(nullptr, res->GetValue(0, 0));

  // 8. Invalid coord type
  res = SQLQuery(hDB_, "SELECT test_px('ds', 1, 'bad_coord_type', 0, 0);");
  ASSERT_NE(nullptr, res);
  EXPECT_EQ(nullptr, res->GetValue(0, 0));

  // 9. Missing band
  res = SQLQuery(hDB_, "SELECT test_px('ds', 99, 'pixel', 0, 0);");
  ASSERT_NE(nullptr, res);
  EXPECT_EQ(nullptr, res->GetValue(0, 0));

  // 10. Invalid arg types
  EXPECT_EQ(nullptr,
            SQLQuery(hDB_, "SELECT test_px('ds', 'bad_band', 'pixel', 0, 0);")
                ->GetValue(0, 0));
  EXPECT_EQ(
      nullptr,
      SQLQuery(hDB_, "SELECT test_px('ds', 1, 123, 0, 0);")->GetValue(0, 0));
  EXPECT_EQ(nullptr,
            SQLQuery(hDB_, "SELECT test_px('ds', 1, 'pixel', 'bad_x', 0);")
                ->GetValue(0, 0));
  EXPECT_EQ(nullptr,
            SQLQuery(hDB_, "SELECT test_px('ds', 1, 'pixel', 0, 'bad_y');")
                ->GetValue(0, 0));
  EXPECT_EQ(nullptr,
            SQLQuery(hDB_, "SELECT test_px('ds', 1, 'pixel', 0, 0, 123);")
                ->GetValue(0, 0));
}

#if defined(DEBUG) || defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION) || \
    defined(ALLOW_FORMAT_DUMPS)
TEST_F(OGRSQLiteUtilityTest, SQLCheckLineIsSafe) {
  EXPECT_TRUE(SQLCheckLineIsSafe("CREATE TABLE foo (a INT, b TEXT);"));
  EXPECT_TRUE(SQLCheckLineIsSafe("INSERT INTO foo VALUES (1, 'val');"));
  EXPECT_TRUE(
      SQLCheckLineIsSafe("CREATE VIRTUAL TABLE foo_rtree USING rtree(id, minx, "
                         "maxx, miny, maxy);"));
  EXPECT_TRUE(
      SQLCheckLineIsSafe("CREATE VIRTUAL TABLE \"foo_rtree\" USING rtree(id, "
                         "minx, maxx, miny, maxy);"));
  EXPECT_TRUE(SQLCheckLineIsSafe(
      "INSERT INTO rtree_poly_geom SELECT fid, ST_MinX ( geom ) , ST_MaxX ( "
      "geom ) , ST_MinY ( geom ) , ST_MaxY ( geom ) FROM poly ;"));
  EXPECT_FALSE(SQLCheckLineIsSafe("DROP TABLE foo;"));
  EXPECT_FALSE(SQLCheckLineIsSafe("ALTER TABLE foo ADD COLUMN c INT;"));
  EXPECT_FALSE(SQLCheckLineIsSafe("ATTACH DATABASE 'foo' AS bar;"));
  EXPECT_FALSE(SQLCheckLineIsSafe("SELECT * FROM foo;"));
}
#endif

// TODO: b/335317901 - Cover OGRSQLiteUtility SQLUnescape with odd backslash
// escapes if supported.

}  // namespace
}  // namespace autotest2
