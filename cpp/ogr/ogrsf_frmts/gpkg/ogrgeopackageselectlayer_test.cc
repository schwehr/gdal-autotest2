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

// Unit tests for OGRGeoPackageSelectLayer.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gpkg/ogrgeopackageselectlayer.cpp
//   https://gdal.org/drivers/vector/gpkg.html

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpkg/ogr_geopackage.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/ogrgeopackageselectlayer_test";

class OGRGeoPackageSelectLayerTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { RegisterOGRGeoPackage(); }

  static void TearDownTestSuite() { GDALDestroyDriverManager(); }

  void SetUp() override {
    VSIUnlink((std::string(kTestDir) + "/test.gpkg").c_str());
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  std::unique_ptr<GDALDataset> CreateTestDataset(const std::string& path) {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
    if (driver == nullptr) {
      return nullptr;
    }
    GDALDataset* raw_ds =
        driver->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (raw_ds == nullptr) {
      return nullptr;
    }
    std::unique_ptr<GDALDataset> ds(raw_ds);

    OGRLayer* layer = ds->CreateLayer("points", nullptr, wkbPoint, nullptr);
    if (layer == nullptr) {
      return nullptr;
    }

    OGRFieldDefn field_name("name", OFTString);
    if (layer->CreateField(&field_name) != OGRERR_NONE) {
      return nullptr;
    }

    auto add_feature = [&](const char* name, double x, double y) {
      OGRFeatureDefn* defn = layer->GetLayerDefn();
      OGRFeature feature(defn);
      feature.SetField("name", name);
      OGRPoint pt(x, y);
      feature.SetGeometry(&pt);
      return layer->CreateFeature(&feature) == OGRERR_NONE;
    };

    if (!add_feature("alpha", 0.0, 0.0) || !add_feature("beta", 10.0, 10.0) ||
        !add_feature("gamma", 20.0, 20.0)) {
      return nullptr;
    }

    return ds;
  }
};

TEST_F(OGRGeoPackageSelectLayerTest, BasicIterationAndCapabilities) {
  const std::string path = std::string(kTestDir) + "/test.gpkg";
  auto ds = CreateTestDataset(path);
  ASSERT_NE(ds, nullptr);

  OGRLayer* layer = ds->ExecuteSQL("SELECT * FROM points", nullptr, nullptr);
  ASSERT_NE(layer, nullptr);
  absl::Cleanup cleanup = [&] { ds->ReleaseResultSet(layer); };

  EXPECT_EQ(3, layer->GetFeatureCount(FALSE));
  EXPECT_EQ(3, layer->GetFeatureCount(TRUE));

  // Iterate features.
  std::unique_ptr<OGRFeature> f1(layer->GetNextFeature());
  ASSERT_NE(f1, nullptr);
  EXPECT_STREQ("alpha", f1->GetFieldAsString("name"));

  std::unique_ptr<OGRFeature> f2(layer->GetNextFeature());
  ASSERT_NE(f2, nullptr);
  EXPECT_STREQ("beta", f2->GetFieldAsString("name"));

  std::unique_ptr<OGRFeature> f3(layer->GetNextFeature());
  ASSERT_NE(f3, nullptr);
  EXPECT_STREQ("gamma", f3->GetFieldAsString("name"));

  EXPECT_EQ(nullptr, layer->GetNextFeature());
  // Second EOF check hits m_bEOF path.
  EXPECT_EQ(nullptr, layer->GetNextFeature());

  // Reset reading.
  layer->ResetReading();
  std::unique_ptr<OGRFeature> f1_again(layer->GetNextFeature());
  ASSERT_NE(f1_again, nullptr);
  EXPECT_STREQ("alpha", f1_again->GetFieldAsString("name"));

  // Extents.
  OGREnvelope env;
  EXPECT_EQ(OGRERR_NONE, layer->GetExtent(&env, TRUE));
  EXPECT_DOUBLE_EQ(0.0, env.MinX);
  EXPECT_DOUBLE_EQ(20.0, env.MaxX);
  EXPECT_EQ(OGRERR_NONE, layer->GetExtent(0, &env, TRUE));
  EXPECT_NE(OGRERR_NONE, layer->GetExtent(-1, &env, TRUE));
  EXPECT_NE(OGRERR_NONE, layer->GetExtent(5, &env, TRUE));

  // Capabilities.
  EXPECT_FALSE(layer->TestCapability(OLCFastFeatureCount));
  EXPECT_TRUE(layer->TestCapability(OLCFastSpatialFilter));
  EXPECT_TRUE(layer->TestCapability(OLCStringsAsUTF8));
  EXPECT_FALSE(layer->TestCapability("NonExistentCapability"));
}

TEST_F(OGRGeoPackageSelectLayerTest, AttributeFilter) {
  const std::string path = std::string(kTestDir) + "/test.gpkg";
  auto ds = CreateTestDataset(path);
  ASSERT_NE(ds, nullptr);

  OGRLayer* layer = ds->ExecuteSQL("SELECT * FROM points", nullptr, nullptr);
  ASSERT_NE(layer, nullptr);
  absl::Cleanup cleanup = [&] { ds->ReleaseResultSet(layer); };

  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter("name = 'beta'"));
  EXPECT_EQ(1, layer->GetFeatureCount(TRUE));

  std::unique_ptr<OGRFeature> f(layer->GetNextFeature());
  ASSERT_NE(f, nullptr);
  EXPECT_STREQ("beta", f->GetFieldAsString("name"));
  EXPECT_EQ(nullptr, layer->GetNextFeature());

  // Clear attribute filter.
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter(nullptr));
  EXPECT_EQ(3, layer->GetFeatureCount(TRUE));

  // Bad syntax filter: accepted by SetAttributeFilter, fails on GetNextFeature.
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter("syntax error ==="));
    std::unique_ptr<OGRFeature> f_err(layer->GetNextFeature());
    EXPECT_EQ(nullptr, f_err);
  }
}

TEST_F(OGRGeoPackageSelectLayerTest, SpatialFilter) {
  const std::string path = std::string(kTestDir) + "/test.gpkg";
  auto ds = CreateTestDataset(path);
  ASSERT_NE(ds, nullptr);

  OGRLayer* layer = ds->ExecuteSQL("SELECT * FROM points", nullptr, nullptr);
  ASSERT_NE(layer, nullptr);
  absl::Cleanup cleanup = [&] { ds->ReleaseResultSet(layer); };

  layer->SetSpatialFilterRect(5.0, 5.0, 15.0, 15.0);
  EXPECT_EQ(1, layer->GetFeatureCount(TRUE));
  std::unique_ptr<OGRFeature> f1(layer->GetNextFeature());
  ASSERT_NE(f1, nullptr);
  EXPECT_STREQ("beta", f1->GetFieldAsString("name"));
  EXPECT_EQ(nullptr, layer->GetNextFeature());

  // Using geometry filter.
  OGRPoint pt(20.0, 20.0);
  layer->SetSpatialFilter(&pt);
  EXPECT_EQ(1, layer->GetFeatureCount(TRUE));
  std::unique_ptr<OGRFeature> f2(layer->GetNextFeature());
  ASSERT_NE(f2, nullptr);
  EXPECT_STREQ("gamma", f2->GetFieldAsString("name"));

  // Reset spatial filter.
  layer->SetSpatialFilter(nullptr);
  EXPECT_EQ(3, layer->GetFeatureCount(TRUE));

  // Invalid geom field index.
  layer->SetSpatialFilter(-1, nullptr);
}

TEST_F(OGRGeoPackageSelectLayerTest, EmptyResultSet) {
  const std::string path = std::string(kTestDir) + "/test.gpkg";
  auto ds = CreateTestDataset(path);
  ASSERT_NE(ds, nullptr);

  // WHERE 1 = 0 triggers empty layer constructor path.
  OGRLayer* layer =
      ds->ExecuteSQL("SELECT * FROM points WHERE 1 = 0", nullptr, nullptr);
  ASSERT_NE(layer, nullptr);
  absl::Cleanup cleanup = [&] { ds->ReleaseResultSet(layer); };

  EXPECT_EQ(0, layer->GetFeatureCount(FALSE));
  EXPECT_EQ(0, layer->GetFeatureCount(TRUE));
  EXPECT_EQ(nullptr, layer->GetNextFeature());

  layer->ResetReading();
  EXPECT_EQ(nullptr, layer->GetNextFeature());
}

TEST_F(OGRGeoPackageSelectLayerTest, ResetStatementFailure) {
  const std::string path = std::string(kTestDir) + "/test.gpkg";
  auto ds = CreateTestDataset(path);
  ASSERT_NE(ds, nullptr);

  // Create a temporary table that will be dropped.
  OGRLayer* temp_layer =
      ds->CreateLayer("temp_drop_tbl", nullptr, wkbNone, nullptr);
  ASSERT_NE(temp_layer, nullptr);
  OGRFieldDefn field_val("val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, temp_layer->CreateField(&field_val));

  OGRFeatureDefn* defn = temp_layer->GetLayerDefn();
  OGRFeature feature(defn);
  feature.SetField("val", 42);
  ASSERT_EQ(OGRERR_NONE, temp_layer->CreateFeature(&feature));

  // Execute SELECT on temp_drop_tbl.
  OGRLayer* select_layer =
      ds->ExecuteSQL("SELECT * FROM temp_drop_tbl", nullptr, nullptr);
  ASSERT_NE(select_layer, nullptr);
  absl::Cleanup cleanup = [&] { ds->ReleaseResultSet(select_layer); };

  // Verify first feature is read and then read until EOF to release the
  // statement.
  {
    std::unique_ptr<OGRFeature> f(select_layer->GetNextFeature());
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(42, f->GetFieldAsInteger("val"));
    while (true) {
      std::unique_ptr<OGRFeature> f_more(select_layer->GetNextFeature());
      if (f_more == nullptr) {
        break;
      }
    }
  }

  // Drop temp_drop_tbl behind the select layer's back via raw SQL.
  OGRLayer* drop_res =
      ds->ExecuteSQL("DROP TABLE temp_drop_tbl", nullptr, nullptr);
  if (drop_res != nullptr) {
    ds->ReleaseResultSet(drop_res);
  }

  // Now ResetReading() -> ResetStatement() -> sqlite3_prepare_v2 will fail
  // because the table no longer exists.
  {
    WithQuietHandler quiet_handler;
    select_layer->ResetReading();
    std::unique_ptr<OGRFeature> f_after(select_layer->GetNextFeature());
    EXPECT_EQ(nullptr, f_after);
  }
}

// TODO: b/335317901 - Cover remaining SQL query translation and custom
// FID column branches in ogrgeopackageselectlayer.cpp.

}  // namespace
}  // namespace autotest2
