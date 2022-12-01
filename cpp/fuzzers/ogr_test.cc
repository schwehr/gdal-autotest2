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
// Test the basics of the OGR vector fuzzer.
//
// These tests are very weak.  It would be possible to have OGRFuzzOneInput
// return an enum that indicated where is exists the fuzzer, but that would
// only be used for testing.
//
// See also:
//   https://trac.osgeo.org/gdal/browser/trunk/gdal/fuzzers

#include "autotest2/cpp/fuzzers/ogr.h"

#include <memory>

#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogrsf_frmts/geojson/ogr_geojson.h"
#include "ogr/ogrsf_frmts/geojson/ogrgeojsonutils.h"

namespace autotest2 {
namespace {

TEST(OgrFuzzTest, Empty) {
  // Should not die.
  OGRFuzzOneInput(nullptr);
}

TEST(OgrFuzzTest, BasicLayer) {
  const char kGeoJson[] = R"json({"type": "Point", "coordinates": [1, 2]})json";

  auto open_info =
      std::make_unique<GDALOpenInfo>(kGeoJson, GDAL_OF_READONLY, nullptr);
  auto datasource = std::make_unique<OGRGeoJSONDataSource>();

  ASSERT_NE(nullptr, datasource);
  EXPECT_TRUE(datasource->Open(open_info.get(), eGeoJSONSourceText, "GeoJSON"));

  OGRFuzzOneInput(datasource.get());
}

}  // namespace
}  // namespace autotest2
