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
// Tests KML OGR driver (not based on libkml).
//
// The KML driver does not support reading KML from strings through Open.
//
// See also:
//   http://www.gdal.org/drv_kml.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_kml.py

#include <memory>

#include "gunit.h"
#include "autotest2/cpp/util/cpl_memory.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_api.h"
#include "ogr/ogrsf_frmts/ogrsf_frmts.h"

using std::unique_ptr;

namespace autotest2 {
namespace {

const char kTestData[] = "autotest2/cpp/ogr/"
    "ogrsf_frmts/kml/testdata/";

class KmlTest : public ::testing::Test {
  void SetUp() override {
    RegisterOGRKML();
  }

  void TearDown() override {
    OGRCleanupAll();
  }
};

TEST_F(KmlTest, Point) {
  const string filepath = file::JoinPath(
      FLAGS_test_srcdir, kTestData, "point.kml");

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
  // "Layer #" plus the actual layer number is assigned to layers
  // without names by OGRKMLDataSource::Open.
  ASSERT_STREQ("Layer #0", layer->GetName());

  unique_ptr<OGRFeature> feature(layer->GetNextFeature());
  OGRGeometry *geometry = feature->GetGeometryRef();
  ASSERT_NE(nullptr, geometry);
  char *wkt_tmp = nullptr;
  ASSERT_EQ(OGRERR_NONE, geometry->exportToWkt(&wkt_tmp));
  std::unique_ptr<char, CplFreeDeleter> wkt(wkt_tmp);
  wkt_tmp = nullptr;
  ASSERT_STREQ("POINT (-109.27 10.3)", wkt.get());
}

}  // namespace
}  // namespace autotest2
