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
// Tests ESRI Shapefile OGR driver.
//
// See also:
//   http://www.gdal.org/drv_shapefile.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_shapefile.py

#include <stddef.h>
#include <memory>
#include <string>
#include <vector>

#include "file/base/path.h"
#include "gmock.h"
#include "googletest.h"
#include "gunit.h"
#include "autotest2/cpp/util/cpl_cstringlist.h"
#include "autotest2/cpp/util/cpl_memory.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_feature.h"
#include "ogr/ogr_geometry.h"
#include "ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "port/cpl_error.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"

using std::unique_ptr;

namespace autotest2 {
namespace {

const char kDriverName[] = "ESRI Shapefile";

const char kTestData[] = "autotest2/cpp/ogr/"
    "ogrsf_frmts/shape/testdata/";

class ShapeTest : public ::testing::Test {
 protected:
  static void SetUpTestCase() {
    RegisterOGRShape();
  }
  static void TearDownTestCase() {
    OGRCleanupAll();
  }
};

TEST_F(ShapeTest, Point) {
  const string filepath = file::JoinPath(
      FLAGS_test_srcdir, kTestData, "point/point.shp");

  unique_ptr<GDALDataset> src(
      static_cast<GDALDataset *>(GDALOpenEx(
          filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_VECTOR,
          nullptr, nullptr, nullptr)));

  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, src);

  ASSERT_EQ(1, src->GetLayerCount());

  OGRLayer *layer = src->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(wkbPoint, layer->GetGeomType());
  EXPECT_NE(wkbPoint25D, layer->GetGeomType());
  ASSERT_EQ(1, layer->GetFeatureCount());
  ASSERT_STREQ("point", layer->GetName());

  unique_ptr<OGRFeature> feature(layer->GetNextFeature());
  OGRGeometry *geometry = feature->GetGeometryRef();
  ASSERT_NE(nullptr, geometry);
  const auto point = dynamic_cast<OGRPoint *>(geometry);
  ASSERT_NE(nullptr, point);
  EXPECT_NEAR(1, point->getX(), 1e-7);
  EXPECT_NEAR(2, point->getY(), 1e-7);

  char *wkt_tmp = nullptr;
  ASSERT_EQ(OGRERR_NONE, geometry->exportToWkt(&wkt_tmp));
  std::unique_ptr<char, CplFreeDeleter> wkt(wkt_tmp);
  wkt_tmp = nullptr;
  ASSERT_STREQ("POINT (1 2)", wkt.get());
}

TEST_F(ShapeTest, WritePoint) {
  // Test round trip write and then read a point shapefile via /vsimem,
  // GDAL's in-memory filesystem.
  GDALDriverManager *drv_manager = GetGDALDriverManager();
  GDALDriver *driver = drv_manager->GetDriverByName(kDriverName);
  ASSERT_NE(nullptr, driver) << "Invalid driver: " << kDriverName;

  const string dst_path = "/vsimem/shapetestwrite";
  const double x = 1.2;
  const double y = 3.4;

  // Write a point shapefile to /vsimem
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
  EXPECT_STREQ("a_layer", layer->GetName());

  unique_ptr<OGRFeature> feature(layer->GetNextFeature());
  OGRGeometry *geometry = feature->GetGeometryRef();
  ASSERT_NE(nullptr, geometry);
  auto point = dynamic_cast<OGRPoint *>(geometry);
  ASSERT_NE(nullptr, point);
  EXPECT_NEAR(x, point->getX(), 1e-7);
  EXPECT_NEAR(y, point->getY(), 1e-7);

  StringListPtr files_string_list(VSIReadDir(dst_path.c_str()), CSLDestroy);
  ASSERT_NE(nullptr, files_string_list);
  const auto files = CslToVector(files_string_list.get());
  EXPECT_THAT(files, testing::UnorderedElementsAre("a_layer.shp", "a_layer.dbf",
                                                   "a_layer.shx"));

  // Cleanup is not strictly necessary, as GDAL will free all of /vsimem on
  // shutdown.  However, it is good to have at least one test to make sure the
  // files are deleted.
  EXPECT_EQ(CE_None, driver->Delete(dst_path.c_str()));

  // If delete succeeded, VSIStatL should return -1 for does not exist.
  VSIStatBufL stat_buf;
  EXPECT_EQ(-1, VSIStatL(dst_path.c_str(), &stat_buf));
}

}  // namespace
}  // namespace autotest2
