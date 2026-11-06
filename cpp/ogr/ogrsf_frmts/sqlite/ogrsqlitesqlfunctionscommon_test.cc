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
// Unit tests for SQLite common extension SQL functions
// (ogrsqlitesqlfunctionscommon.cpp).
//
// See also:
//   https://gdal.org/drivers/vector/sqlite.html
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqlitesqlfunctionscommon.cpp

#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/sqlite/sqlite3.h"

extern "C" void RegisterOGRSQLite();

namespace autotest2 {
namespace {

constexpr const char* kTestDir = "/vsimem/sqlite_sqlcommon_test";

class DummyPixelBandCommon final : public GDALRasterBand {
 public:
  DummyPixelBandCommon() {
    eDataType = GDT_Byte;
    nBlockXSize = 2;
    nBlockYSize = 2;
  }

  CPLErr IReadBlock(int, int, void* pImage) override {
    static_cast<GByte*>(pImage)[0] = 42;
    return CE_None;
  }

  CPLErr InterpolateAtPoint(double, double, GDALRIOResampleAlg,
                            double* pdfRealValue,
                            double* = nullptr) const override {
    *pdfRealValue = 42.0;
    return CE_None;
  }
};

class DummyPixelDatasetCommon final : public GDALDataset {
 public:
  DummyPixelDatasetCommon() {
    nRasterXSize = 10;
    nRasterYSize = 10;
    SetBand(1, new DummyPixelBandCommon());
  }

  CPLErr GetGeoTransform(GDALGeoTransform& gt) const override {
    gt[0] = 0.0;
    gt[1] = 1.0;
    gt[2] = 0.0;
    gt[3] = 10.0;
    gt[4] = 0.0;
    gt[5] = -1.0;
    return CE_None;
  }
};

class DummyRasterDriverCommon final : public GDALDriver {
 public:
  DummyRasterDriverCommon() {
    SetDescription("DUMMY_COMMON_RASTER");
    pfnOpen = [](GDALOpenInfo* poOpenInfo) -> GDALDataset* {
      if (poOpenInfo->pszFilename &&
          strcmp(poOpenInfo->pszFilename, "dummy_px_common.tif") == 0) {
        return new DummyPixelDatasetCommon();
      }
      return nullptr;
    };
  }
};

class OGRSQLiteSQLFunctionsCommonTest : public ::testing::Test {
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

    pDummyDriver_ = new DummyRasterDriverCommon();
    GetGDALDriverManager()->RegisterDriver(pDummyDriver_);
  }

  void TearDown() override {
    if (pDummyDriver_) {
      GetGDALDriverManager()->DeregisterDriver(pDummyDriver_);
      delete pDummyDriver_;
      pDummyDriver_ = nullptr;
    }
    poDS_.reset();
    VSIRmdirRecursive(kTestDir);
  }

  std::string db_path_;
  std::unique_ptr<GDALDataset> poDS_;
  sqlite3* hDB_ = nullptr;
  DummyRasterDriverCommon* pDummyDriver_ = nullptr;
};

TEST_F(OGRSQLiteSQLFunctionsCommonTest, GDALGetPixelValue) {
  // 1. Without OGR_SQLITE_ALLOW_EXTERNAL_ACCESS -> returns NULL
  CPLSetConfigOption("OGR_SQLITE_ALLOW_EXTERNAL_ACCESS", "NO");
  sqlite3_stmt* hStmt = nullptr;
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                           hDB_,
                           "SELECT gdal_get_pixel_value('dummy_px_common.tif', "
                           "1, 'pixel', 0, 0);",
                           -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 2. With OGR_SQLITE_ALLOW_EXTERNAL_ACCESS=YES
  CPLSetConfigOption("OGR_SQLITE_ALLOW_EXTERNAL_ACCESS", "YES");
  absl::Cleanup access_cleanup = [] {
    CPLSetConfigOption("OGR_SQLITE_ALLOW_EXTERNAL_ACCESS", nullptr);
  };

  // Non-text 1st argument
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT gdal_get_pixel_value(123, 1, 'pixel', 0, 0);", -1,
                &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // Non-existent raster
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_,
          "SELECT gdal_get_pixel_value('non_existent.tif', 1, 'pixel', 0, 0);",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // Valid 5-arg call (initial dataset open)
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                           hDB_,
                           "SELECT gdal_get_pixel_value('dummy_px_common.tif', "
                           "1, 'pixel', 1, 1);",
                           -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_FLOAT, sqlite3_column_type(hStmt, 0));
  EXPECT_DOUBLE_EQ(42.0, sqlite3_column_double(hStmt, 0));
  sqlite3_finalize(hStmt);

  // Valid 6-arg call (hits oCachedDS cache)
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                           hDB_,
                           "SELECT gdal_get_pixel_value('dummy_px_common.tif', "
                           "1, 'georef', 2.0, 8.0, 'nearest');",
                           -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_FLOAT, sqlite3_column_type(hStmt, 0));
  EXPECT_DOUBLE_EQ(42.0, sqlite3_column_double(hStmt, 0));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsCommonTest, LikeFunction) {
  sqlite3_stmt* hStmt = nullptr;

  // 1. Basic 2-arg LIKE (case-insensitive by default)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT LIKE('abc', 'ABC'), LIKE('abc%', 'abcdef');", -1,
                &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 1));
  sqlite3_finalize(hStmt);

  // 2. 3-arg LIKE with escape
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(
          hDB_, "SELECT LIKE('a!_b', 'a_b', '!'), LIKE('a!%b', 'a%b', '!');",
          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 0));
  EXPECT_EQ(1, sqlite3_column_int(hStmt, 1));
  sqlite3_finalize(hStmt);

  // 3. Null inputs
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
                           hDB_, "SELECT LIKE(NULL, 'abc'), LIKE('abc', NULL);",
                           -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  sqlite3_finalize(hStmt);

  // 4. Invalid escape (multi-char escape or NULL escape)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT LIKE('a', 'a', '!!'), LIKE('a', 'a', NULL);", -1,
                &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  sqlite3_finalize(hStmt);

  // 5. Case-sensitive LIKE via PRAGMA
  poDS_->ExecuteSQL("PRAGMA case_sensitive_like = 1;", nullptr, nullptr);
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(hDB_, "SELECT LIKE('abc', 'ABC');",
                                          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(0, sqlite3_column_int(hStmt, 0));
  sqlite3_finalize(hStmt);

  // Reset case_sensitive_like
  poDS_->ExecuteSQL("PRAGMA case_sensitive_like = 0;", nullptr, nullptr);
}

TEST_F(OGRSQLiteSQLFunctionsCommonTest, StdDevFunctions) {
  ASSERT_EQ(SQLITE_OK, sqlite3_exec(hDB_,
                                    "CREATE TABLE num_data (v REAL);"
                                    "CREATE TABLE empty_data (v REAL);"
                                    "CREATE TABLE single_data (v REAL);",
                                    nullptr, nullptr, nullptr));

  ASSERT_EQ(SQLITE_OK, sqlite3_exec(hDB_,
                                    "INSERT INTO single_data VALUES (10.0);"
                                    "INSERT INTO num_data VALUES (10.0);"
                                    "INSERT INTO num_data VALUES (20.0);"
                                    "INSERT INTO num_data VALUES (30.0);"
                                    "INSERT INTO num_data VALUES (NULL);"
                                    "INSERT INTO num_data VALUES ('text');",
                                    nullptr, nullptr, nullptr));

  sqlite3_stmt* hStmt = nullptr;

  // Empty table
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT STDDEV_POP(v), STDDEV_SAMP(v) FROM empty_data;",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  sqlite3_finalize(hStmt);

  // Single row: STDDEV_POP is 0, STDDEV_SAMP is NULL
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT STDDEV_POP(v), STDDEV_SAMP(v) FROM single_data;",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_DOUBLE_EQ(0.0, sqlite3_column_double(hStmt, 0));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 1));
  sqlite3_finalize(hStmt);

  // Multiple rows: 10, 20, 30
  // Mean = 20. Pop var = (100 + 0 + 100)/3 = 200/3. StdDevPop = sqrt(200/3)
  // = 8.1649658 Samp var = 200/2 = 100. StdDevSamp = 10.0
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT STDDEV_POP(v), STDDEV_SAMP(v) FROM num_data;", -1,
                &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_NEAR(std::sqrt(200.0 / 3.0), sqlite3_column_double(hStmt, 0), 1e-6);
  EXPECT_NEAR(10.0, sqlite3_column_double(hStmt, 1), 1e-6);
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsCommonTest, PercentileAndMedianFunctions) {
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_exec(hDB_,
                   "CREATE TABLE pct_data (v REAL, p REAL);"
                   "INSERT INTO pct_data VALUES (10.0, 50.0);"
                   "INSERT INTO pct_data VALUES (20.0, 50.0);"
                   "INSERT INTO pct_data VALUES (30.0, 50.0);"
                   "INSERT INTO pct_data VALUES (40.0, 50.0);"
                   "INSERT INTO pct_data VALUES (NULL, 50.0);"
                   "INSERT INTO pct_data VALUES (9e999 - 9e999, 50.0);"  // NaN
                   "CREATE TABLE empty_pct (v REAL, p REAL);",
                   nullptr, nullptr, nullptr));

  sqlite3_stmt* hStmt = nullptr;

  // 1. median(v) = 25.0
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT median(v) FROM pct_data;", -1,
                               &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_NEAR(25.0, sqlite3_column_double(hStmt, 0), 1e-6);
  sqlite3_finalize(hStmt);

  // 2. percentile(v, P)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_,
                "SELECT percentile(v, 0), percentile(v, 100), "
                "percentile(v, 50), percentile(v, 25.5) FROM pct_data;",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_NEAR(10.0, sqlite3_column_double(hStmt, 0), 1e-6);
  EXPECT_NEAR(40.0, sqlite3_column_double(hStmt, 1), 1e-6);
  EXPECT_NEAR(25.0, sqlite3_column_double(hStmt, 2), 1e-6);
  EXPECT_GT(sqlite3_column_double(hStmt, 3), 10.0);
  sqlite3_finalize(hStmt);

  // 3. percentile_cont(v, P)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_,
                               "SELECT percentile_cont(v, 0.5), "
                               "percentile_cont(v, 0.25) FROM pct_data;",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_NEAR(25.0, sqlite3_column_double(hStmt, 0), 1e-6);
  sqlite3_finalize(hStmt);

  // 4. Empty table returns NULL
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT percentile(v, 50) FROM empty_pct;",
                               -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 5. Error branches: out of range P for percentile (< 0 or > 100)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT percentile(v, -1) FROM pct_data;",
                               -1, &hStmt, nullptr));
  EXPECT_NE(SQLITE_ROW, sqlite3_step(hStmt));
  sqlite3_finalize(hStmt);

  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT percentile(v, 101) FROM pct_data;",
                               -1, &hStmt, nullptr));
  EXPECT_NE(SQLITE_ROW, sqlite3_step(hStmt));
  sqlite3_finalize(hStmt);

  // Out of range P for percentile_cont (< 0 or > 1)
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(hDB_, "SELECT percentile_cont(v, -0.1) FROM pct_data;",
                         -1, &hStmt, nullptr));
  EXPECT_NE(SQLITE_ROW, sqlite3_step(hStmt));
  sqlite3_finalize(hStmt);

  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_prepare_v2(hDB_, "SELECT percentile_cont(v, 1.5) FROM pct_data;",
                         -1, &hStmt, nullptr));
  EXPECT_NE(SQLITE_ROW, sqlite3_step(hStmt));
  sqlite3_finalize(hStmt);

  // Inconsistent P across rows
  ASSERT_EQ(SQLITE_OK, sqlite3_exec(hDB_,
                                    "CREATE TABLE inc_p (v REAL, p REAL);"
                                    "INSERT INTO inc_p VALUES (1, 10);"
                                    "INSERT INTO inc_p VALUES (2, 20);",
                                    nullptr, nullptr, nullptr));
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT percentile(v, p) FROM inc_p;", -1,
                               &hStmt, nullptr));
  EXPECT_NE(SQLITE_ROW, sqlite3_step(hStmt));
  sqlite3_finalize(hStmt);

  // Non-numeric 1st arg
  ASSERT_EQ(SQLITE_OK, sqlite3_exec(hDB_,
                                    "CREATE TABLE text_pct (v TEXT, p REAL);"
                                    "INSERT INTO text_pct VALUES ('foo', 50);",
                                    nullptr, nullptr, nullptr));
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT percentile(v, p) FROM text_pct;",
                               -1, &hStmt, nullptr));
  EXPECT_NE(SQLITE_ROW, sqlite3_step(hStmt));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsCommonTest, ModeFunction) {
  ASSERT_EQ(SQLITE_OK, sqlite3_exec(hDB_,
                                    "CREATE TABLE mode_num (v REAL);"
                                    "INSERT INTO mode_num VALUES (10.0);"
                                    "INSERT INTO mode_num VALUES (20.0);"
                                    "INSERT INTO mode_num VALUES (20.0);"
                                    "INSERT INTO mode_num VALUES (NULL);"
                                    "CREATE TABLE mode_text (v TEXT);"
                                    "INSERT INTO mode_text VALUES ('apple');"
                                    "INSERT INTO mode_text VALUES ('banana');"
                                    "INSERT INTO mode_text VALUES ('banana');"
                                    "CREATE TABLE mode_blob (v BLOB);"
                                    "INSERT INTO mode_blob VALUES (X'0102');"
                                    "CREATE TABLE mode_empty (v REAL);",
                                    nullptr, nullptr, nullptr));

  sqlite3_stmt* hStmt = nullptr;

  // 1. Numeric mode
  ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(hDB_, "SELECT mode(v) FROM mode_num;",
                                          -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_DOUBLE_EQ(20.0, sqlite3_column_double(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 2. Text mode
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT mode(v) FROM mode_text;", -1,
                               &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_STREQ("banana",
               reinterpret_cast<const char*>(sqlite3_column_text(hStmt, 0)));
  sqlite3_finalize(hStmt);

  // 3. Empty mode
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT mode(v) FROM mode_empty;", -1,
                               &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 4. BLOB error
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(hDB_, "SELECT mode(v) FROM mode_blob;", -1,
                               &hStmt, nullptr));
  EXPECT_NE(SQLITE_ROW, sqlite3_step(hStmt));
  sqlite3_finalize(hStmt);
}

TEST_F(OGRSQLiteSQLFunctionsCommonTest, GetTransformViaTransform3) {
  sqlite3_stmt* hStmt = nullptr;

  // 1. Initial transform creation (4326 to 3857)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT Transform3(ST_MakePoint(2.0, 49.0), 4326, 3857);",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  // Returns blob
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 2. Second transform with same SRS (cache hit in oCachedTransformsMap)
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT Transform3(ST_MakePoint(3.0, 50.0), 4326, 3857);",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_BLOB, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);

  // 3. Invalid EPSG code
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(
                hDB_, "SELECT Transform3(ST_MakePoint(2.0, 49.0), -999, 3857);",
                -1, &hStmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(hStmt));
  EXPECT_EQ(SQLITE_NULL, sqlite3_column_type(hStmt, 0));
  sqlite3_finalize(hStmt);
}

// TODO: b/335317901 - Cover OGRSQLiteSQLFunctionsCommon spatial reference
// transformation edge cases.

}  // namespace
}  // namespace autotest2
