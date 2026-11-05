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

// Unit tests for OGRGeoPackageLayer.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gpkg/ogrgeopackagelayer.cpp
//   https://gdal.org/drivers/vector/gpkg.html

#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpkg/ogr_geopackage.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpkg/ogrgeopackageutility.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogr_sqlite.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestGpkg[] = "/vsimem/ogrgeopackagelayer_test.gpkg";

class OGRGeoPackageLayerTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { RegisterOGRGeoPackage(); }

  static void TearDownTestSuite() { GDALDestroyDriverManager(); }

  void SetUp() override {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_NE(nullptr, driver);
    dataset_.reset(driver->Create(kTestGpkg, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, dataset_);
  }

  void TearDown() override {
    dataset_.reset();
    VSIUnlink(kTestGpkg);
  }

  std::unique_ptr<GDALDataset> dataset_;
};

TEST_F(OGRGeoPackageLayerTest, LayerCapabilitiesAndFidColumn) {
  OGRLayer* layer =
      dataset_->CreateLayer("test_cap", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  EXPECT_TRUE(layer->TestCapability(OLCIgnoreFields));
  EXPECT_TRUE(layer->TestCapability(OLCStringsAsUTF8));
  EXPECT_TRUE(layer->TestCapability(OLCFastGetArrowStream));
  EXPECT_TRUE(layer->TestCapability(OLCZGeometries));
  EXPECT_FALSE(layer->TestCapability("UNKNOWN_CAPABILITY"));

  EXPECT_STREQ("fid", layer->GetFIDColumn());

  // ExecuteSQL query with no explicit FID column.
  OGRLayer* query_layer =
      dataset_->ExecuteSQL("SELECT 1 AS num", nullptr, nullptr);
  ASSERT_NE(nullptr, query_layer);
  EXPECT_STREQ("", query_layer->GetFIDColumn());
  dataset_->ReleaseResultSet(query_layer);
}

TEST_F(OGRGeoPackageLayerTest, FeatureIterationAndFilters) {
  OGRLayer* layer = dataset_->CreateLayer("pts", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn val_field("val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&val_field));

  for (int i = 1; i <= 5; ++i) {
    OGRFeature feature(layer->GetLayerDefn());
    feature.SetField("val", i);
    OGRPoint pt(i, i);
    feature.SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(&feature));
  }

  // Normal iteration.
  layer->ResetReading();
  int count = 0;
  while (auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature())) {
    count++;
  }
  EXPECT_EQ(5, count);
  // Line 82: calling GetNextFeature when m_bEOF is true
  EXPECT_EQ(nullptr, layer->GetNextFeature());

  // Spatial filter.
  OGRPolygon poly;
  OGRLinearRing ring;
  ring.addPoint(0.0, 0.0);
  ring.addPoint(10.0, 0.0);
  ring.addPoint(0.0, 10.0);
  ring.addPoint(0.0, 0.0);
  poly.addRing(&ring);

  layer->SetSpatialFilter(&poly);
  layer->ResetReading();
  count = 0;
  while (auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature())) {
    count++;
  }
  EXPECT_GT(count, 0);  // Tests envelope pass but polygon reject (line 129)
  layer->SetSpatialFilter(nullptr);

  // Attribute filter.
  layer->SetAttributeFilter("val >= 4");
  layer->ResetReading();
  count = 0;
  while (auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature())) {
    count++;
  }
  EXPECT_EQ(2, count);  // Points 4 and 5
  layer->SetAttributeFilter(nullptr);
}

TEST_F(OGRGeoPackageLayerTest, IgnoredFields) {
  OGRLayer* layer = dataset_->CreateLayer("ign", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f1("f1", OFTString);
  OGRFieldDefn f2("f2", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f1));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f2));

  OGRFeature feature(layer->GetLayerDefn());
  feature.SetField("f1", "hello");
  feature.SetField("f2", 42);
  OGRPoint pt(10, 20);
  feature.SetGeometry(&pt);
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(&feature));

  // Ignore geometry and f1.
  const char* ignored[] = {"OGR_GEOMETRY", "f1", nullptr};
  EXPECT_EQ(OGRERR_NONE, layer->SetIgnoredFields(ignored));

  layer->ResetReading();
  auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_EQ(nullptr, feat->GetGeometryRef());
  EXPECT_FALSE(feat->IsFieldSet(0));  // f1 is ignored
  EXPECT_TRUE(feat->IsFieldSet(1));   // f2 is present
  EXPECT_EQ(42, feat->GetFieldAsInteger(1));

  // Reset ignored fields.
  EXPECT_EQ(OGRERR_NONE, layer->SetIgnoredFields(nullptr));
  layer->ResetReading();
  feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_NE(nullptr, feat->GetGeometryRef());
  EXPECT_TRUE(feat->IsFieldSet(0));
}

TEST_F(OGRGeoPackageLayerTest, TranslateFeatureAllFieldTypes) {
  OGRLayer* layer =
      dataset_->CreateLayer("all_types", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_int("f_int", OFTInteger);
  OGRFieldDefn f_int64("f_int64", OFTInteger64);
  OGRFieldDefn f_real("f_real", OFTReal);
  OGRFieldDefn f_str("f_str", OFTString);
  OGRFieldDefn f_bin("f_bin", OFTBinary);
  OGRFieldDefn f_date("f_date", OFTDate);
  OGRFieldDefn f_dt("f_dt", OFTDateTime);

  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int64));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_real));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_str));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_bin));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_date));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dt));

  // Row 1: Fully populated valid data.
  {
    OGRFeature feature(layer->GetLayerDefn());
    feature.SetField(0, 123);
    feature.SetField(1, static_cast<GIntBig>(9876543210LL));
    feature.SetField(2, 3.14159);
    feature.SetField(3, "test string");
    const GByte bin_data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    feature.SetField(4, 4, bin_data);
    feature.SetField(5, 2025, 3, 15, 0, 0, 0.0f, 0);
    feature.SetField(6, 2025, 3, 15, 14, 30, 45.0f, 100);
    OGRPoint pt(1.0, 2.0);
    feature.SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(&feature));
  }

  // Row 2: Null and empty fields.
  {
    OGRFeature feature(layer->GetLayerDefn());
    feature.SetFieldNull(0);
    feature.SetFieldNull(1);
    feature.SetFieldNull(2);
    feature.SetFieldNull(3);
    const GByte empty_bin[] = {};
    feature.SetField(4, 0, empty_bin);
    feature.SetFieldNull(5);
    feature.SetFieldNull(6);
    feature.SetGeometry(nullptr);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(&feature));
  }

  layer->ResetReading();

  // Read Row 1.
  auto feat1 = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat1);
  EXPECT_EQ(123, feat1->GetFieldAsInteger("f_int"));
  EXPECT_EQ(static_cast<GIntBig>(9876543210LL),
            feat1->GetFieldAsInteger64("f_int64"));
  EXPECT_DOUBLE_EQ(3.14159, feat1->GetFieldAsDouble("f_real"));
  EXPECT_STREQ("test string", feat1->GetFieldAsString("f_str"));
  int bin_size = 0;
  const GByte* bin_read = feat1->GetFieldAsBinary(4, &bin_size);
  EXPECT_EQ(4, bin_size);
  EXPECT_EQ(0xDE, bin_read[0]);

  int year, month, day, hour, minute, tz;
  float sec;
  EXPECT_TRUE(feat1->GetFieldAsDateTime(5, &year, &month, &day, &hour, &minute,
                                        &sec, &tz));
  EXPECT_EQ(2025, year);
  EXPECT_EQ(3, month);
  EXPECT_EQ(15, day);

  EXPECT_TRUE(feat1->GetFieldAsDateTime(6, &year, &month, &day, &hour, &minute,
                                        &sec, &tz));
  EXPECT_EQ(2025, year);
  EXPECT_EQ(3, month);
  EXPECT_EQ(15, day);
  EXPECT_EQ(14, hour);

  ASSERT_NE(nullptr, feat1->GetGeometryRef());

  // Read Row 2.
  auto feat2 = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat2);
  EXPECT_TRUE(feat2->IsFieldNull(0));
  EXPECT_TRUE(feat2->IsFieldNull(2));
  EXPECT_EQ(nullptr, feat2->GetGeometryRef());
}

TEST_F(OGRGeoPackageLayerTest, DateAndDateTimeParsingVariations) {
  // Test via ExecuteSQL on raw table expressions to hit ParseDateField and
  // ParseDateTimeField branches.
  OGRLayer* layer =
      dataset_->CreateLayer("date_test", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_d("d", OFTDate);
  OGRFieldDefn f_dt("dt", OFTDateTime);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_d));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dt));

  const int idx_d = layer->GetLayerDefn()->GetFieldIndex("d");
  const int idx_dt = layer->GetLayerDefn()->GetFieldIndex("dt");

  // Insert raw invalid/non-standard dates directly via SQL:
  // 1. Nominal ISO date & datetime
  // 2. Lax date (with slash "2025/03/15")
  // 3. Integer timestamp in date column
  // 4. Invalid text string ("not-a-date")
  dataset_->ExecuteSQL(
      "INSERT INTO date_test (d, dt) VALUES ('2025-06-20', "
      "'2025-06-20T12:30:00Z')",
      nullptr, nullptr);
  dataset_->ExecuteSQL(
      "INSERT INTO date_test (d, dt) VALUES ('2025/06/20', '2025/06/20 "
      "12:30:00')",
      nullptr, nullptr);
  dataset_->ExecuteSQL(
      "INSERT INTO date_test (d, dt) VALUES (1718880000, 1718880000)", nullptr,
      nullptr);
  dataset_->ExecuteSQL(
      "INSERT INTO date_test (d, dt) VALUES ('invalid-date', "
      "'invalid-datetime')",
      nullptr, nullptr);
  dataset_->ExecuteSQL(
      "INSERT INTO date_test (d, dt) VALUES ('2025-99-99', "
      "'2025-99-99T99:99:99Z')",
      nullptr, nullptr);

  // Read row 1: nominal
  layer->ResetReading();
  auto f1 = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
  ASSERT_NE(nullptr, f1);
  EXPECT_TRUE(f1->IsFieldSet(idx_d));
  EXPECT_TRUE(f1->IsFieldSet(idx_dt));

  // Read row 2: lax parsing (warning emitted)
  {
    WithQuietHandler quiet_handler;
    auto f2 = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
    ASSERT_NE(nullptr, f2);
    EXPECT_TRUE(f2->IsFieldSet(idx_d));
  }

  // Read row 3: integer data type (warning emitted)
  {
    WithQuietHandler quiet_handler;
    auto f3 = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
    ASSERT_NE(nullptr, f3);
    EXPECT_FALSE(f3->IsFieldSet(idx_d));
  }

  // Read row 4: invalid text content (warning emitted)
  {
    WithQuietHandler quiet_handler;
    auto f4 = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
    ASSERT_NE(nullptr, f4);
    EXPECT_FALSE(f4->IsFieldSet(idx_d));
  }

  // Read row 5: out of range month/day (exercises lines 205-206)
  {
    WithQuietHandler quiet_handler;
    auto f5 = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
    ASSERT_NE(nullptr, f5);
    EXPECT_FALSE(f5->IsFieldSet(idx_d));
  }
}

TEST_F(OGRGeoPackageLayerTest, CoordinatePrecisionAndSpatiaLiteGeometry) {
  // Test coordinate precision reduction on reading.
  const char* lco[] = {"DISCARD_COORD_LSB=YES",
                       "UNDO_DISCARD_COORD_LSB_ON_READING=YES", nullptr};
  OGRLayer* layer = dataset_->CreateLayer("prec_test", nullptr, wkbPoint,
                                          const_cast<char**>(lco));
  ASSERT_NE(nullptr, layer);

  OGRFeature feature(layer->GetLayerDefn());
  OGRPoint pt(1.123456789, 2.987654321);
  feature.SetGeometry(&pt);
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(&feature));

  // Read normally.
  layer->ResetReading();
  auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  ASSERT_NE(nullptr, feat->GetGeometryRef());

  // Read with UNDO_DISCARD_COORD_LSB_ON_READING=YES config option.
  CPLSetThreadLocalConfigOption("UNDO_DISCARD_COORD_LSB_ON_READING", "YES");
  absl::Cleanup reset_opt = [] {
    CPLSetThreadLocalConfigOption("UNDO_DISCARD_COORD_LSB_ON_READING", nullptr);
  };
  layer->ResetReading();
  auto feat_undo = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat_undo);
  ASSERT_NE(nullptr, feat_undo->GetGeometryRef());

  // Test zero FID with no explicit FID column (lines 348-349)
  {
    OGRLayer* q = dataset_->ExecuteSQL("SELECT 0 AS fid_val, 'test' AS txt",
                                       nullptr, nullptr);
    ASSERT_NE(nullptr, q);
    auto feat0 = std::unique_ptr<OGRFeature>(q->GetNextFeature());
    ASSERT_NE(nullptr, feat0);
    dataset_->ReleaseResultSet(q);
  }
}

TEST_F(OGRGeoPackageLayerTest, BuildFeatureDefnComplexQueries) {
  OGRLayer* tbl =
      dataset_->CreateLayer("src_table", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, tbl);
  OGRFieldDefn col_a("a", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, tbl->CreateField(&col_a));

  OGRFeature f(tbl->GetLayerDefn());
  f.SetField("a", 100);
  OGRPoint pt(5, 10);
  f.SetGeometry(&pt);
  ASSERT_EQ(OGRERR_NONE, tbl->CreateFeature(&f));

  // 1. Duplicate column names ("SELECT a, a FROM src_table") -> second is
  // renamed/deduplicated.
  {
    OGRLayer* q =
        dataset_->ExecuteSQL("SELECT a, a FROM src_table", nullptr, nullptr);
    ASSERT_NE(nullptr, q);
    EXPECT_EQ(1, q->GetLayerDefn()->GetFieldCount());
    dataset_->ReleaseResultSet(q);
  }

  // 2. Query with _rowid_ column and explicit FID column.
  {
    OGRLayer* q = dataset_->ExecuteSQL("SELECT _rowid_, fid, a FROM src_table",
                                       nullptr, nullptr);
    ASSERT_NE(nullptr, q);
    EXPECT_STREQ("fid", q->GetFIDColumn());
    dataset_->ReleaseResultSet(q);
  }

  // 3. Query with expression columns (literals, geometry function returning
  // blob or text).
  {
    OGRLayer* q = dataset_->ExecuteSQL(
        "SELECT 1 + 2 AS calc, 'hello' AS greeting, 3.14 AS pi_val, X'0102' AS "
        "blob_col FROM src_table",
        nullptr, nullptr);
    ASSERT_NE(nullptr, q);
    EXPECT_EQ(4, q->GetLayerDefn()->GetFieldCount());
    EXPECT_EQ(OFTInteger, q->GetLayerDefn()->GetFieldDefn(0)->GetType());
    EXPECT_EQ(OFTString, q->GetLayerDefn()->GetFieldDefn(1)->GetType());
    EXPECT_EQ(OFTReal, q->GetLayerDefn()->GetFieldDefn(2)->GetType());
    EXPECT_EQ(OFTBinary, q->GetLayerDefn()->GetFieldDefn(3)->GetType());
    dataset_->ReleaseResultSet(q);
  }

  // 4. Query with NULL column named "geom" or "geometry" (exercises lines
  // 1104-1117).
  {
    OGRLayer* q = dataset_->ExecuteSQL("SELECT NULL AS geom, a FROM src_table",
                                       nullptr, nullptr);
    ASSERT_NE(nullptr, q);
    EXPECT_EQ(1, q->GetLayerDefn()->GetGeomFieldCount());
    EXPECT_STREQ("geom", q->GetLayerDefn()->GetGeomFieldDefn(0)->GetNameRef());
    dataset_->ReleaseResultSet(q);
  }

  // 5. Query returning geometry from table.
  {
    OGRLayer* q =
        dataset_->ExecuteSQL("SELECT geom, a FROM src_table", nullptr, nullptr);
    ASSERT_NE(nullptr, q);
    EXPECT_EQ(1, q->GetLayerDefn()->GetGeomFieldCount());
    EXPECT_EQ(1, q->GetLayerDefn()->GetFieldCount());
    dataset_->ReleaseResultSet(q);
  }

  // 6. With OGR_PROMOTE_TO_INTEGER64=TRUE.
  {
    CPLSetThreadLocalConfigOption("OGR_PROMOTE_TO_INTEGER64", "TRUE");
    absl::Cleanup reset_prom = [] {
      CPLSetThreadLocalConfigOption("OGR_PROMOTE_TO_INTEGER64", nullptr);
    };
    OGRLayer* q =
        dataset_->ExecuteSQL("SELECT 100 AS int_val", nullptr, nullptr);
    ASSERT_NE(nullptr, q);
    EXPECT_EQ(OFTInteger64, q->GetLayerDefn()->GetFieldDefn(0)->GetType());
    dataset_->ReleaseResultSet(q);
  }

  // 7. Query with NULL column named after a spatial function (lines 1108-1117)
  {
    OGRLayer* q = dataset_->ExecuteSQL("SELECT NULL AS \"Envelope(geom)\"",
                                       nullptr, nullptr);
    ASSERT_NE(nullptr, q);
    EXPECT_EQ(1, q->GetLayerDefn()->GetGeomFieldCount());
    dataset_->ReleaseResultSet(q);
  }

  // 8. Query with non-geometry BLOB (lines 1148-1150)
  {
    OGRLayer* q = dataset_->ExecuteSQL("SELECT X'0102030405060708' AS not_geom",
                                       nullptr, nullptr);
    ASSERT_NE(nullptr, q);
    EXPECT_EQ(0, q->GetLayerDefn()->GetGeomFieldCount());
    EXPECT_EQ(1, q->GetLayerDefn()->GetFieldCount());
    dataset_->ReleaseResultSet(q);
  }
}

TEST_F(OGRGeoPackageLayerTest, SpatiaLiteAndCorruptGeometries) {
  OGRLayer* layer =
      dataset_->CreateLayer("spatialite_test", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  // 1. Export a SpatiaLite geometry blob.
  OGRPoint pt(12.34, 56.78);
  GByte* pabySpatiaLite = nullptr;
  int nSpatiaLiteLen = 0;
  ASSERT_EQ(OGRERR_NONE, OGRSQLiteExportSpatiaLiteGeometry(
                             &pt, 4326, wkbNDR, false, false, &pabySpatiaLite,
                             &nSpatiaLiteLen));
  ASSERT_NE(nullptr, pabySpatiaLite);
  absl::Cleanup free_spat = [&] { CPLFree(pabySpatiaLite); };

  char* pszHexSpat = CPLBinaryToHex(nSpatiaLiteLen, pabySpatiaLite);
  ASSERT_NE(nullptr, pszHexSpat);
  absl::Cleanup free_hex = [&] { CPLFree(pszHexSpat); };

  // Insert SpatiaLite geometry blob directly.
  std::string sql_spat =
      std::string("INSERT INTO spatialite_test (geom) VALUES (X'") +
      pszHexSpat + "')";
  dataset_->ExecuteSQL(sql_spat.c_str(), nullptr, nullptr);

  // Insert corrupt geometry blob.
  dataset_->ExecuteSQL(
      "INSERT INTO spatialite_test (geom) VALUES (X'0102030405060708')",
      nullptr, nullptr);

  // Read SpatiaLite geometry row (exercises lines 377-385).
  layer->ResetReading();
  auto feat_spat = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat_spat);
  ASSERT_NE(nullptr, feat_spat->GetGeometryRef());
  EXPECT_DOUBLE_EQ(12.34, feat_spat->GetGeometryRef()->toPoint()->getX());

  // Read corrupt geometry row (exercises lines 381-383).
  {
    WithQuietHandler quiet_handler;
    auto feat_corrupt = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
    ASSERT_NE(nullptr, feat_corrupt);
    EXPECT_EQ(nullptr, feat_corrupt->GetGeometryRef());
  }

  // 2. BuildFeatureDefn recognizing geometry blob from SQLite BLOB column
  // (lines 1123-1187).
  {
    std::string sql_blob =
        std::string("SELECT X'") + pszHexSpat + "' AS geom_blob";
    OGRLayer* q = dataset_->ExecuteSQL(sql_blob.c_str(), nullptr, nullptr);
    ASSERT_NE(nullptr, q);
    EXPECT_EQ(1, q->GetLayerDefn()->GetGeomFieldCount());
    dataset_->ReleaseResultSet(q);
  }

  // BuildFeatureDefn recognizing GPKG geometry blob from BLOB column (lines
  // 1136-1142).
  {
    size_t nGpkgLen = 0;
    GByte* pabyGpkg = GPkgGeometryFromOGR(&pt, 4326, nullptr, &nGpkgLen);
    ASSERT_NE(nullptr, pabyGpkg);
    absl::Cleanup free_gpkg = [&] { CPLFree(pabyGpkg); };
    char* pszHexGpkg = CPLBinaryToHex(static_cast<int>(nGpkgLen), pabyGpkg);
    ASSERT_NE(nullptr, pszHexGpkg);
    absl::Cleanup free_hex_gpkg = [&] { CPLFree(pszHexGpkg); };

    std::string sql_gpkg_blob =
        std::string("SELECT X'") + pszHexGpkg + "' AS geom_gpkg";
    OGRLayer* q = dataset_->ExecuteSQL(sql_gpkg_blob.c_str(), nullptr, nullptr);
    ASSERT_NE(nullptr, q);
    EXPECT_EQ(1, q->GetLayerDefn()->GetGeomFieldCount());
    dataset_->ReleaseResultSet(q);
  }

  // 3. Stream spatialite_test via GetArrowStream to exercise lines 649-668
  {
    OGRLayer* q = dataset_->ExecuteSQL("SELECT geom FROM spatialite_test",
                                       nullptr, nullptr);
    ASSERT_NE(nullptr, q);
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(q->GetArrowStream(&stream, nullptr));
    ASSERT_NE(nullptr, stream.get_next);
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    WithQuietHandler quiet_handler;
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    if (array.release) {
      array.release(&array);
    }
    if (stream.release) {
      stream.release(&stream);
    }
    dataset_->ReleaseResultSet(q);
  }
}

TEST_F(OGRGeoPackageLayerTest, ArrowStreamOnSelectLayer) {
  OGRLayer* layer =
      dataset_->CreateLayer("arrow_test", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_bool("f_bool", OFTInteger);
  f_bool.SetSubType(OFSTBoolean);
  OGRFieldDefn f_int16("f_int16", OFTInteger);
  f_int16.SetSubType(OFSTInt16);
  OGRFieldDefn f_int("f_int", OFTInteger);
  OGRFieldDefn f_int64("f_int64", OFTInteger64);
  OGRFieldDefn f_float("f_float", OFTReal);
  f_float.SetSubType(OFSTFloat32);
  OGRFieldDefn f_real("f_real", OFTReal);
  OGRFieldDefn f_bin("f_bin", OFTBinary);
  OGRFieldDefn f_date("f_date", OFTDate);
  OGRFieldDefn f_dt("f_dt", OFTDateTime);
  OGRFieldDefn f_str("f_str", OFTString);

  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_bool));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int16));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int64));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_float));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_real));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_bin));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_date));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_dt));
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_str));

  // Row 1: Valid values.
  {
    OGRFeature feat(layer->GetLayerDefn());
    feat.SetField(0, 1);
    feat.SetField(1, 16);
    feat.SetField(2, 32);
    feat.SetField(3, static_cast<GIntBig>(64));
    feat.SetField(4, 1.25);
    feat.SetField(5, 2.5);
    const GByte bin_data[] = {0x01, 0x02};
    feat.SetField(6, 2, bin_data);
    feat.SetField(7, 2025, 4, 1, 0, 0, 0.0f, 0);
    feat.SetField(8, 2025, 4, 1, 10, 20, 30.0f, 0);
    feat.SetField(9, "hello");
    OGRPoint pt(1, 2);
    feat.SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // Row 2: Null values across all columns.
  {
    OGRFeature feat(layer->GetLayerDefn());
    feat.SetFieldNull(0);
    feat.SetFieldNull(1);
    feat.SetFieldNull(2);
    feat.SetFieldNull(3);
    feat.SetFieldNull(4);
    feat.SetFieldNull(5);
    feat.SetFieldNull(6);
    feat.SetFieldNull(7);
    feat.SetFieldNull(8);
    feat.SetFieldNull(9);
    feat.SetGeometry(nullptr);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // Row 3: Empty string and binary.
  {
    OGRFeature feat(layer->GetLayerDefn());
    feat.SetField(0, 0);
    feat.SetField(1, 0);
    feat.SetField(2, 0);
    feat.SetField(3, static_cast<GIntBig>(0));
    feat.SetField(4, 0.0);
    feat.SetField(5, 0.0);
    const GByte empty_bin[] = {};
    feat.SetField(6, 0, empty_bin);
    feat.SetField(7, 2025, 1, 1, 0, 0, 0.0f, 0);
    feat.SetField(8, 2025, 1, 1, 0, 0, 0.0f, 0);
    feat.SetField(9, "");
    OGRPoint pt(10, 20);
    feat.SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // Get select layer via ExecuteSQL.
  OGRLayer* select_layer =
      dataset_->ExecuteSQL("SELECT * FROM arrow_test", nullptr, nullptr);
  ASSERT_NE(nullptr, select_layer);

  // 1. Read ArrowArrayStream using OGRGeoPackageLayer::GetArrowStream and
  // GetNextArrowArray.
  {
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    EXPECT_TRUE(select_layer->GetArrowStream(&stream, nullptr));
    ASSERT_NE(nullptr, stream.get_next);

    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(3, array.length);
    if (array.release) {
      array.release(&array);
    }

    // Next call returns EOF.
    EXPECT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(0, array.length);
    if (array.release) {
      array.release(&array);
    }

    if (stream.release) {
      stream.release(&stream);
    }
  }

  // 2. Read with DATETIME_AS_STRING=YES.
  {
    select_layer->ResetReading();
    const char* dt_opts[] = {"DATETIME_AS_STRING=YES", nullptr};
    struct ArrowArrayStream stream_dt;
    memset(&stream_dt, 0, sizeof(stream_dt));
    EXPECT_TRUE(select_layer->GetArrowStream(&stream_dt, dt_opts));
    ASSERT_NE(nullptr, stream_dt.get_next);

    struct ArrowArray array_dt;
    memset(&array_dt, 0, sizeof(array_dt));
    EXPECT_EQ(0, stream_dt.get_next(&stream_dt, &array_dt));
    EXPECT_EQ(3, array_dt.length);
    if (array_dt.release) {
      array_dt.release(&array_dt);
    }
    if (stream_dt.release) {
      stream_dt.release(&stream_dt);
    }
  }

  // 3. Read with spatial filter (exercises slow poGeom exportToWkb path in
  // lines 673-706).
  {
    select_layer->ResetReading();
    struct ArrowArrayStream stream_filt;
    memset(&stream_filt, 0, sizeof(stream_filt));
    EXPECT_TRUE(select_layer->GetArrowStream(&stream_filt, nullptr));
    ASSERT_NE(nullptr, stream_filt.get_next);

    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(5, 0);
    ring.addPoint(5, 5);
    ring.addPoint(0, 5);
    ring.addPoint(0, 0);
    poly.addRing(&ring);
    select_layer->SetSpatialFilter(&poly);

    struct ArrowArray array_filt;
    memset(&array_filt, 0, sizeof(array_filt));
    EXPECT_EQ(0, stream_filt.get_next(&stream_filt, &array_filt));
    EXPECT_GT(array_filt.length, 0);
    if (array_filt.release) {
      array_filt.release(&array_filt);
    }
    if (stream_filt.release) {
      stream_filt.release(&stream_filt);
    }
    select_layer->SetSpatialFilter(nullptr);
  }

  // 4. Read ArrowArrayStream with OGR_GPKG_STREAM_BASE_IMPL=YES (falls back to
  // OGRLayer).
  {
    CPLSetThreadLocalConfigOption("OGR_GPKG_STREAM_BASE_IMPL", "YES");
    absl::Cleanup reset_opt = [] {
      CPLSetThreadLocalConfigOption("OGR_GPKG_STREAM_BASE_IMPL", nullptr);
    };

    select_layer->ResetReading();
    struct ArrowArrayStream stream_base;
    memset(&stream_base, 0, sizeof(stream_base));
    EXPECT_TRUE(select_layer->GetArrowStream(&stream_base, nullptr));
    ASSERT_NE(nullptr, stream_base.get_next);

    struct ArrowArray array_base;
    memset(&array_base, 0, sizeof(array_base));
    EXPECT_EQ(0, stream_base.get_next(&stream_base, &array_base));
    EXPECT_EQ(3, array_base.length);
    if (array_base.release) {
      array_base.release(&array_base);
    }
    if (stream_base.release) {
      stream_base.release(&stream_base);
    }
  }

  // 5. Read with low OGR_ARROW_MEM_LIMIT to trigger batch limit splitting
  // (lines 808-819, 880-891).
  {
    CPLSetThreadLocalConfigOption("OGR_ARROW_MEM_LIMIT", "10");
    absl::Cleanup reset_mem = [] {
      CPLSetThreadLocalConfigOption("OGR_ARROW_MEM_LIMIT", nullptr);
    };
    select_layer->ResetReading();
    struct ArrowArrayStream stream_mem;
    memset(&stream_mem, 0, sizeof(stream_mem));
    EXPECT_TRUE(select_layer->GetArrowStream(&stream_mem, nullptr));
    ASSERT_NE(nullptr, stream_mem.get_next);
    struct ArrowArray array_mem;
    memset(&array_mem, 0, sizeof(array_mem));
    while (stream_mem.get_next(&stream_mem, &array_mem) == 0 &&
           array_mem.release != nullptr) {
      array_mem.release(&array_mem);
      memset(&array_mem, 0, sizeof(array_mem));
    }
    if (stream_mem.release) {
      stream_mem.release(&stream_mem);
    }
  }

  // 6. Read Arrow stream with an ignored field (exercises line 732).
  {
    select_layer->ResetReading();
    const char* ignored[] = {"f_int", nullptr};
    EXPECT_EQ(OGRERR_NONE, select_layer->SetIgnoredFields(ignored));

    struct ArrowArrayStream stream_ign;
    memset(&stream_ign, 0, sizeof(stream_ign));
    EXPECT_TRUE(select_layer->GetArrowStream(&stream_ign, nullptr));
    ASSERT_NE(nullptr, stream_ign.get_next);

    struct ArrowArray array_ign;
    memset(&array_ign, 0, sizeof(array_ign));
    EXPECT_EQ(0, stream_ign.get_next(&stream_ign, &array_ign));
    EXPECT_EQ(3, array_ign.length);
    if (array_ign.release) {
      array_ign.release(&array_ign);
    }
    if (stream_ign.release) {
      stream_ign.release(&stream_ign);
    }
    select_layer->SetIgnoredFields(nullptr);
  }

  dataset_->ReleaseResultSet(select_layer);
}

// TODO: b/335317901 - Cover remaining virtual layer geometry translation
// error branches in ogrgeopackagelayer.cpp.

}  // namespace
}  // namespace autotest2
