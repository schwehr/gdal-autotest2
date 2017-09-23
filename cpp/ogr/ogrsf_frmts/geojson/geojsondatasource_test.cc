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

#include <memory>

#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_feature.h"
#include "ogr/ogr_geometry.h"
#include "ogr/ogrsf_frmts/geojson/ogr_geojson.h"
#include "ogr/ogrsf_frmts/geojson/ogrgeojsonutils.h"
#include "ogr/ogrsf_frmts/ogrsf_frmts.h"

namespace autotest2 {
namespace {

TEST(GeoJsonDatasourceTest, Int64Overflow) {
  // Autofuzz POC from b/65523372
  const char kJson[] = R"json(
    {
      "type":"Feature",
      ":FFFFws":[5328949842375111,-22222222222,-2222222222,
                 -222222222222222222222222222,false],
      ":FFFfws":
        [111111222,-222222222222-080000000015328949842375111,-22222222222,
         -2222222222,-222222222222222222222222222.-2210]
    })json";
  auto open_info =
      absl::MakeUnique<GDALOpenInfo>(kJson, GDAL_OF_READONLY, nullptr);
  auto dataset = absl::MakeUnique<OGRGeoJSONDataSource>();

  EXPECT_TRUE(dataset->Open(open_info.get(), eGeoJSONSourceText));

  ASSERT_EQ(1, dataset->GetLayerCount());

  OGRLayer *layer = dataset->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(wkbUnknown, layer->GetGeomType());
  ASSERT_EQ(1, layer->GetFeatureCount());

  auto feature = absl::WrapUnique(layer->GetNextFeature());
  EXPECT_EQ(nullptr, feature->GetGeometryRef());
}

}  // namespace
}  // namespace autotest2
