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

// Tests OGRGeoRSSDataSource.
//
// See also:
//   http://www.gdal.org/drv_georss.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_georss.py

#include <memory>

#include "file/base/path.h"
#include "gunit.h"
#include "absl/memory/memory.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_feature.h"
#include "ogr/ogr_geometry.h"
#include "ogr/ogrsf_frmts/georss/ogr_georss.h"
#include "ogr/ogrsf_frmts/ogrsf_frmts.h"

namespace autotest2 {
namespace {

const char kTestData[] =
    "autotest2/cpp/ogr/ogrsf_frmts/georss/testdata/";

TEST(GeoRssDatasourceTest, BasicTest) {
  const string filepath =
      file::JoinPath(FLAGS_test_srcdir, kTestData, "test.xml");
  auto dataset = absl::make_unique<OGRGeoRSSDataSource>();

  EXPECT_TRUE(dataset->Open(filepath.c_str(), 0));

  ASSERT_EQ(1, dataset->GetLayerCount());

  OGRLayer *layer = dataset->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(wkbPoint, layer->GetGeomType());
  ASSERT_EQ(2, layer->GetFeatureCount());

  auto feature = absl::WrapUnique(layer->GetNextFeature());
  EXPECT_NE(nullptr, feature->GetGeometryRef());
}

}  // namespace
}  // namespace autotest2
