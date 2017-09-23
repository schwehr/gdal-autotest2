// Copyright 2017 Google Inc. All Rights Reserved.
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
// Tests MITAB OGR driver.
//
// See also:
//   http://www.gdal.org/drv_mitab.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_mitab.py

#include <memory>
#include <string>

#include "file/base/path.h"
#include "googletest.h"
#include "gunit.h"
#include "autotest2/cpp/util/cpl_memory.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_api.h"
#include "ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_feature.h"
#include "ogr/ogr_geometry.h"
#include "port/cpl_error.h"

using std::unique_ptr;

namespace autotest2 {
namespace {

const char kTestData[] = "cpp/ogr/ogrsf_frmts/mitab/testdata/";

class KmlTest : public ::testing::Test {
  void SetUp() override {
    RegisterOGRTAB();
  }

  void TearDown() override {
    OGRCleanupAll();
  }
};

TEST_F(KmlTest, Point) {
  const string filepath = file::JoinPath(
      FLAGS_test_srcdir, kTestData, "point-wgs84", "point.tab");

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
  char *wkt_tmp = nullptr;
  ASSERT_EQ(OGRERR_NONE, geometry->exportToWkt(&wkt_tmp));
  std::unique_ptr<char, CplFreeDeleter> wkt(wkt_tmp);
  wkt_tmp = nullptr;
  ASSERT_STREQ("POINT (0 1)", wkt.get());
}

}  // namespace
}  // namespace autotest2
