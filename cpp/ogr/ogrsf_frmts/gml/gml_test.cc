// Test GML OGR driver.
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
// Test the Geographic Markup Language (GML) GDAL/OGR vector driver.
//
// See also:
//   http://www.gdal.org/drv_gml.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_gml_geom.py
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_gml_read.py

#include <memory>
#include <string>

#include "gunit.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_feature.h"
#include "ogr/ogr_geometry.h"
#include "ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "port/cpl_error.h"
#include "port/cpl_vsi.h"

using std::unique_ptr;

namespace autotest2 {
namespace {

class GmlTest : public ::testing::Test {
  void SetUp() override {
    RegisterOGRGML();
  }

  void TearDown() override {
    OGRCleanupAll();
  }
};

TEST_F(GmlTest, EmptyTest) {
  string gml =
      R"gml(<?xml version="1.0" encoding="utf-8" ?>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://ogr.maptools.org/ .xsd"
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
  <gml:boundedBy><gml:null>missing</gml:null></gml:boundedBy>
</ogr:FeatureCollection>
      )gml";

  const char filepath[] = "/vsimem/gml_test_empty.gml";
  {
    VSILFILE * const file = VSIFOpenL(filepath, "w");
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(gml.size(), VSIFWriteL(gml.c_str(), 1, gml.size(), file));
    ASSERT_EQ(0, VSIFFlushL(file));
    ASSERT_EQ(0, VSIFCloseL(file));
  }

  unique_ptr<GDALDataset> src(
      static_cast<GDALDataset *>(GDALOpenEx(
          filepath, GDAL_OF_READONLY | GDAL_OF_VECTOR,
          nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, src);

  EXPECT_EQ(CPLE_None, CPLGetLastErrorNo());
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());

  EXPECT_STREQ(filepath, src->GetDescription());
  EXPECT_EQ(0, src->GetLayerCount());
  ASSERT_EQ(0, VSIUnlink(filepath));
}

TEST_F(GmlTest, PointTest) {
  string gml =
      R"gml(<?xml version="1.0" encoding="utf-8" ?>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://ogr.maptools.org/ point.xsd"
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <ogr:OGRGeoJSON fid="pt">
      <ogr:geometryProperty>
        <gml:Point>
          <gml:coordinates>100,0</gml:coordinates>
        </gml:Point>
      </ogr:geometryProperty>
    </ogr:OGRGeoJSON>
  </gml:featureMember>
</ogr:FeatureCollection>
      )gml";

  const char filepath[] = "/vsimem/gml_test_point.gml";
  {
    VSILFILE * const file = VSIFOpenL(filepath, "w");
    ASSERT_NE(nullptr, file);
    ASSERT_EQ(gml.size(), VSIFWriteL(gml.c_str(), 1, gml.size(), file));
    ASSERT_EQ(0, VSIFFlushL(file));
    ASSERT_EQ(0, VSIFCloseL(file));
  }

  unique_ptr<GDALDataset> src(
      static_cast<GDALDataset *>(GDALOpenEx(
          filepath, GDAL_OF_READONLY | GDAL_OF_VECTOR,
          nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, src);

  EXPECT_STREQ(filepath, src->GetDescription());

  EXPECT_EQ(1, src->GetLayerCount());
  OGRLayer *layer = src->GetLayer(0);

  unique_ptr<OGRFeature> feature(layer->GetNextFeature());
  ASSERT_NE(nullptr, feature);
  EXPECT_EQ(0, feature->GetFID());
  OGRGeometry *geometry = feature->GetGeometryRef();
  ASSERT_NE(nullptr, geometry);
  auto point = dynamic_cast<OGRPoint *>(geometry);
  EXPECT_NEAR(100.0, point->getX(), 0.00001);
  EXPECT_NEAR(0.0, point->getY(), 0.00001);

  ASSERT_EQ(0, VSIUnlink(filepath));
}

}  // namespace
}  // namespace autotest2
