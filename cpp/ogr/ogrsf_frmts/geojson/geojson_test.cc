// Tests geojson OGR driver.
//
// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests GeoJSON OGR driver.
//
// See also:
//   http://www.gdal.org/drv_geojson.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_geojson.py

#include <memory>

#include "gmock.h"
#include "gunit.h"
#include "autotest2/cpp/util/cpl_memory.h"
#include "gcore/gdal.h"
#include "ogr/ogr_api.h"
#include "ogr/ogrsf_frmts/ogrsf_frmts.h"

using std::unique_ptr;

namespace autotest2 {
namespace {

const char kDriverName[] = "GeoJSON";

// Signature of GDALClose.  Sadly, GDALDatasetH is a void *.
typedef void (*GDALDatasetDeleter)(GDALDatasetH);

const char kTestData[] = "autotest2/cpp/ogr/"
    "ogrsf_frmts/geojson/testdata/";

class GeoJsonTest : public ::testing::Test {
  void SetUp() override {
    RegisterOGRGeoJSON();
  }

  void TearDown() override {
    OGRCleanupAll();
  }
};

TEST_F(GeoJsonTest, Point) {
  const string filepath = file::JoinPath(
      FLAGS_test_srcdir, kTestData, "point.geojson");

  {
    // GDAL wants to use a static destroy method rather than delete.
    // Verify that GDALClose does not cause any memory problems.
    unique_ptr<GDALDataset, GDALDatasetDeleter> src(
        static_cast<GDALDataset *>(GDALOpenEx(
            filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_VECTOR,
            nullptr, nullptr, nullptr)),
        GDALClose);
    ASSERT_NE(nullptr, src);
  }

  unique_ptr<GDALDataset> src(
      static_cast<GDALDataset *>(GDALOpenEx(
          filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_VECTOR,
          nullptr, nullptr, nullptr)));

  ASSERT_NE(nullptr, src);

  ASSERT_EQ(1, src->GetLayerCount());

  OGRLayer *layer = src->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  ASSERT_EQ(wkbPoint, layer->GetGeomType());
  ASSERT_EQ(1, layer->GetFeatureCount());
  ASSERT_STREQ("OGRGeoJSON", layer->GetName());

  OGRFeatureDefn *defn = layer->GetLayerDefn();
  ASSERT_EQ(0, defn->GetFieldCount());

  unique_ptr<OGRFeature> feature(layer->GetNextFeature());
  ASSERT_NE(nullptr, feature);
  ASSERT_EQ(0, feature->GetFID());
  OGRGeometry *geometry = feature->GetGeometryRef();
  ASSERT_NE(nullptr, geometry);
  char *wkt_tmp = nullptr;
  ASSERT_EQ(OGRERR_NONE, geometry->exportToWkt(&wkt_tmp));
  std::unique_ptr<char, CplFreeDeleter> wkt(wkt_tmp);
  wkt_tmp = nullptr;
  ASSERT_NE(nullptr, wkt);
  ASSERT_STREQ("POINT (100 0)", wkt.get());
}

TEST_F(GeoJsonTest, PointFromString) {
  const string geojson = R"GeoJSON(
      {
        "type": "Point",
        "coordinates": [100.0, 0.0]
      })GeoJSON";

  unique_ptr<GDALDataset> src(
      static_cast<GDALDataset *>(GDALOpenEx(
          geojson.c_str(), GDAL_OF_READONLY | GDAL_OF_VECTOR,
          nullptr, nullptr, nullptr)));

  ASSERT_NE(nullptr, src);

  ASSERT_EQ(1, src->GetLayerCount());

  OGRLayer *layer = src->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  ASSERT_EQ(wkbPoint, layer->GetGeomType());
  ASSERT_EQ(1, layer->GetFeatureCount());
  ASSERT_STREQ("OGRGeoJSON", layer->GetName());

  unique_ptr<OGRFeature> feature(layer->GetNextFeature());
  OGRGeometry *geometry = feature->GetGeometryRef();
  ASSERT_NE(nullptr, geometry);
  char *wkt_tmp = nullptr;
  ASSERT_EQ(OGRERR_NONE, geometry->exportToWkt(&wkt_tmp));
  std::unique_ptr<char, CplFreeDeleter> wkt(wkt_tmp);
  wkt_tmp = nullptr;
  ASSERT_STREQ("POINT (100 0)", wkt.get());
}

TEST_F(GeoJsonTest, LineWithFields) {
  const string filepath = file::JoinPath(
      FLAGS_test_srcdir, kTestData, "linestring.geojson");

  unique_ptr<GDALDataset> src(
      static_cast<GDALDataset *>(GDALOpenEx(
          filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_VECTOR,
          nullptr, nullptr, nullptr)));

  ASSERT_NE(nullptr, src);

  ASSERT_EQ(1, src->GetLayerCount());

  OGRLayer *layer = src->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  ASSERT_EQ(wkbLineString, layer->GetGeomType());
  ASSERT_EQ(1, layer->GetFeatureCount());
  ASSERT_STREQ("OGRGeoJSON", layer->GetName());

  OGRFeatureDefn *defn = layer->GetLayerDefn();
  ASSERT_EQ(2, defn->GetFieldCount());

  EXPECT_STREQ("first", defn->GetFieldDefn(0)->GetNameRef());
  EXPECT_EQ(OFTInteger, defn->GetFieldDefn(0)->GetType());

  EXPECT_STREQ("2nd", defn->GetFieldDefn(1)->GetNameRef());
  EXPECT_EQ(OFTString, defn->GetFieldDefn(1)->GetType());

  unique_ptr<OGRFeature> feature(layer->GetNextFeature());
  ASSERT_NE(nullptr, feature);

  EXPECT_EQ(1, feature->GetFieldAsInteger(0));
  EXPECT_STREQ("1", feature->GetFieldAsString(0));
  EXPECT_STREQ("second", feature->GetFieldAsString(1));

  OGRGeometry *geometry = feature->GetGeometryRef();
  ASSERT_NE(nullptr, geometry);

  char *wkt_tmp = nullptr;
  ASSERT_EQ(OGRERR_NONE, geometry->exportToWkt(&wkt_tmp));
  std::unique_ptr<char, CplFreeDeleter> wkt(wkt_tmp);
  wkt_tmp = nullptr;
  ASSERT_NE(nullptr, wkt);
  EXPECT_STREQ("LINESTRING (-120.12 38.64,-90.93 45.9)", wkt.get());
}

TEST_F(GeoJsonTest, CreatePoint) {
  GDALDriverManager *drv_manager = GetGDALDriverManager();
  GDALDriver *driver = drv_manager->GetDriverByName(kDriverName);
  ASSERT_NE(nullptr, driver) << "Invalid driver: " << kDriverName;
  const string dst_path = "/vsimem/create.json";

  // TODO(schwehr): Try other values like std::numeric_limits<double>::min().
  const double x = 1.2;
  const double y = 3.4;

  // Write a point GeoJSON to /vsimem
  {
    unique_ptr<GDALDataset, std::function<void(GDALDatasetH)>> data_source(
        driver->Create(dst_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr),
        GDALClose);
    ASSERT_NE(nullptr, data_source) << "Creation failed: " << dst_path;

    OGRLayer *layer =
        data_source->CreateLayer("a_layer", nullptr /* coordinate system */,
                                 wkbPoint, nullptr /* options */);
    ASSERT_NE(nullptr, layer);

    OGRFieldDefn field("name_field", OFTString);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field));

    unique_ptr<OGRFeature> feature(new OGRFeature(layer->GetLayerDefn()));
    feature->SetField("name_field", "a_feature");
    ASSERT_NE(nullptr, feature);
    const OGRPoint point(x, y);
    feature->SetGeometry(&point);
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feature.get()));
  }

  // Read back the created shapefile to verify the write.
  unique_ptr<GDALDataset> src(static_cast<GDALDataset *>(
      GDALOpenEx(dst_path.c_str(), GDAL_OF_READONLY | GDAL_OF_VECTOR, nullptr,
                 nullptr, nullptr)));

  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, src);

  ASSERT_EQ(1, src->GetLayerCount());

  OGRLayer *layer = src->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(wkbPoint, layer->GetGeomType());
  ASSERT_EQ(1, layer->GetFeatureCount());
  EXPECT_STREQ("OGRGeoJSON", layer->GetName());

  unique_ptr<OGRFeature> feature(layer->GetNextFeature());
  OGRGeometry *geometry = feature->GetGeometryRef();
  ASSERT_NE(nullptr, geometry);
  auto point = dynamic_cast<OGRPoint *>(geometry);
  ASSERT_NE(nullptr, point);
  EXPECT_NEAR(x, point->getX(), 1e-7);
  EXPECT_NEAR(y, point->getY(), 1e-7);

  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &stat_buf));

  // Cleanup is not strictly necessary, as GDAL will free all of /vsimem on
  // shutdown.  However, it is good to have at least one test to make sure the
  // files are deleted.
  EXPECT_EQ(CE_None, driver->Delete(dst_path.c_str()));

  // If delete succeeded, VSIStatL should return -1 for does not exist.
  EXPECT_EQ(-1, VSIStatL(dst_path.c_str(), &stat_buf));
}

}  // namespace
}  // namespace autotest2
