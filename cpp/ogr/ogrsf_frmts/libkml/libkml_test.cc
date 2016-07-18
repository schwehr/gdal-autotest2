// Tests libkml driver.
//
// Copyright 2016 Google Inc. All Rights Reserved.
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
// Test the Keyhole Markup Language (KML) GDAL/OGR vector driver.
//
// See also:
//   http://www.gdal.org/drv_libkml.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_libkml.py

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "gmock.h"
#include "gunit.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_feature.h"
#include "ogr/ogr_geometry.h"
#include "ogr/ogr_spatialref.h"
#include "ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "port/cpl_error.h"

using std::unique_ptr;
using testing::Eq;

namespace autotest2 {
namespace {

class LibkmlTest : public ::testing::Test {
 public:
  LibkmlTest() {
    // Needs to register libkml driver explicitly.
    RegisterOGRLIBKML();
  }
  ~LibkmlTest() { OGRCleanupAll(); }
};

TEST_F(LibkmlTest, EmptyTest) {
  string kml =
      R"kml(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://earth.google.com/kml/2.1">
<Document>
</Document>
</kml>
      )kml";

  const char filepath[] = "/vsimem/libkml_test_empty.kml";
  // Save the testing KML literal as vsimem file.
  {
    VSILFILE* const file = VSIFOpenL(filepath, "w");
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(kml.size(), VSIFWriteL(kml.c_str(), 1, kml.size(), file));
    ASSERT_EQ(0, VSIFFlushL(file));
    ASSERT_EQ(0, VSIFCloseL(file));
  }
  unique_ptr<GDALDataset> src(static_cast<GDALDataset*>(GDALOpenEx(
      filepath, GDAL_OF_READONLY | GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, src);

  EXPECT_EQ(CPLE_None, CPLGetLastErrorNo());
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());

  EXPECT_STREQ(filepath, src->GetDescription());
  EXPECT_EQ(0, src->GetLayerCount());
  ASSERT_EQ(0, VSIUnlink(filepath));
}

TEST_F(LibkmlTest, PointTest) {
  string kml =
      R"kml(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
<Document>
  <Placemark>
    <name>Clipperton Island</name>
    <Point>
      <coordinates>-109.27,10.30</coordinates>
    </Point>
  </Placemark>
</Document>
</kml>
      )kml";

  const char filepath[] = "/vsimem/libkml_test_point.kml";
  {
    VSILFILE *const file = VSIFOpenL(filepath, "w");
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(kml.size(), VSIFWriteL(kml.c_str(), 1, kml.size(), file));
    ASSERT_EQ(0, VSIFFlushL(file));
    ASSERT_EQ(0, VSIFCloseL(file));
  }

  unique_ptr<GDALDataset> src(static_cast<GDALDataset *>(GDALOpenEx(
      filepath, GDAL_OF_READONLY | GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, src);
  EXPECT_STREQ(filepath, src->GetDescription());

  EXPECT_EQ(1, src->GetLayerCount());
  OGRLayer *layer = src->GetLayer(0);

  unique_ptr<OGRFeature> feature(layer->GetNextFeature());
  ASSERT_NE(nullptr, feature);
  EXPECT_EQ(1, feature->GetFID());
  OGRGeometry *geometry = feature->GetGeometryRef();
  ASSERT_NE(nullptr, geometry);
  auto point = dynamic_cast<OGRPoint *>(geometry);
  EXPECT_NEAR(-109.27, point->getX(), 0.00001);
  EXPECT_NEAR(10.30, point->getY(), 0.00001);

  ASSERT_EQ(0, VSIUnlink(filepath));
}

TEST_F(LibkmlTest, PolygonTest) {
  string kml =
      R"kml(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
<Document>
  <Folder>
    <name>empty layer</name>
    <description>empty layer is still a layer</description>
  </Folder>
  <Folder>
    <name>polygon test</name>
    <visibility>0</visibility>
    <description>test whether the polygons can be parsed.</description>
      <Placemark>
        <name>a square</name>
        <description>a square in the middle of nowhere</description>
        <Polygon>
          <outerBoundaryIs>
            <LinearRing>
              <coordinates>0,0,5,
                0,1,5,
                1,1,5,
                1,0,5</coordinates>
            </LinearRing>
          </outerBoundaryIs>
        </Polygon>
      </Placemark>
  </Folder>
</Document>
</kml>
      )kml";
  const char filepath[] = "/vsimem/libkml_test_polygon.kml";
  {
    VSILFILE *const file = VSIFOpenL(filepath, "w");
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(kml.size(), VSIFWriteL(kml.c_str(), 1, kml.size(), file));
    ASSERT_EQ(0, VSIFFlushL(file));
    ASSERT_EQ(0, VSIFCloseL(file));
  }

  unique_ptr<GDALDataset> src(static_cast<GDALDataset *>(GDALOpenEx(
      filepath, GDAL_OF_READONLY | GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, src);
  EXPECT_STREQ(filepath, src->GetDescription());

  EXPECT_EQ(2, src->GetLayerCount());
  OGRLayer *layer = src->GetLayer(0);
  ASSERT_STREQ("empty layer", layer->GetName());
  layer = src->GetLayerByName("polygon test");
  unique_ptr<OGRFeature> feature(layer->GetNextFeature());
  ASSERT_NE(nullptr, feature);
  ASSERT_STREQ("a square", feature->GetFieldAsString("name"));
  ASSERT_STREQ("a square in the middle of nowhere",
               feature->GetFieldAsString("description"));
  OGRGeometry *geometry = feature->GetGeometryRef();
  ASSERT_NE(nullptr, geometry);
  auto polygon = dynamic_cast<OGRPolygon *>(geometry);
  ASSERT_NE(nullptr, polygon);
  EXPECT_NEAR(1, polygon->get_Area(), 1e-5);
  ASSERT_EQ(0, VSIUnlink(filepath));
}

}  // namespace
}  // namespace autotest2
