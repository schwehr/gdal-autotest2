// Copyright 2017 Google Inc. All Rights Reserved.
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

#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogr_geojson.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonutils.h"

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
      std::make_unique<GDALOpenInfo>(kJson, GDAL_OF_READONLY, nullptr);
  auto dataset = std::make_unique<OGRGeoJSONDataSource>();

  // Implicitly assert there is no runtime error.
  EXPECT_FALSE(dataset->Open(open_info.get(), eGeoJSONSourceText, "GeoJSON"));
}

}  // namespace
}  // namespace autotest2
