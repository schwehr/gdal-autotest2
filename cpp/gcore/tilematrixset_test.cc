// Copyright 2026 Google LLC
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
//
// Tests OGC TileMatrixSet parsing and raster creation (tilematrixset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/tilematrixset.cpp
//   https://github.com/OSGeo/gdal/blob/master/gcore/tilematrixset.hpp

#include "third_party/gdal/gcore/tilematrixset.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr std::string_view kValidV1Json = R"json({
  "type": "TileMatrixSetType",
  "title": "Custom TMS",
  "identifier": "CustomTest",
  "abstract": "Test abstract",
  "boundingBox": {
    "type": "BoundingBoxType",
    "crs": "http://www.opengis.net/def/crs/EPSG/0/3857",
    "lowerCorner": [-1000.0, -1000.0],
    "upperCorner": [1000.0, 1000.0]
  },
  "supportedCRS": "http://www.opengis.net/def/crs/EPSG/0/3857",
  "wellKnownScaleSet": "http://www.opengis.net/def/wkss/OGC/1.0/GoogleMapsCompatible",
  "tileMatrix": [
    {
      "type": "TileMatrixType",
      "identifier": "0",
      "scaleDenominator": 100000.0,
      "topLeftCorner": [-1000.0, 1000.0],
      "tileWidth": 256,
      "tileHeight": 256,
      "matrixWidth": 2,
      "matrixHeight": 2,
      "variableMatrixWidth": [
        {
          "coalesce": 2,
          "minTileRow": 0,
          "maxTileRow": 1
        }
      ]
    },
    {
      "type": "TileMatrixType",
      "identifier": "1",
      "scaleDenominator": 50000.0,
      "topLeftCorner": [-1000.0, 1000.0],
      "tileWidth": 256,
      "tileHeight": 256,
      "matrixWidth": 4,
      "matrixHeight": 4
    }
  ]
})json";

constexpr std::string_view kInvalidDefs[] = {
    // 1. Non-existent name:
    "NonExistentTMSName",
    // 2. Malformed JSON in memory:
    R"({"crs": "tileMatrices" broken)",
    // 3. TMS v2 with crs.wkt as JSON object:
    R"json({
      "id": "TMSv2_WKTObj",
      "crs": { "wkt": { "type": "GeographicCRS" } },
      "tileMatrices": []
    })json",
    // 4. Invalid CRS:
    R"json({
      "type": "TileMatrixSetType",
      "supportedCRS": "INVALID_CRS_STRING",
      "tileMatrix": []
    })json",
    // 5. Empty tileMatrix array:
    R"json({
      "type": "TileMatrixSetType",
      "supportedCRS": "EPSG:3857",
      "tileMatrix": []
    })json",
    // 6. Invalid scaleDenominator (<= 0):
    R"json({
      "type": "TileMatrixSetType",
      "supportedCRS": "EPSG:3857",
      "tileMatrix": [{ "identifier": "0", "scaleDenominator": -1.0,
                       "tileWidth": 256, "tileHeight": 256,
                       "matrixWidth": 1, "matrixHeight": 1 }]
    })json",
    // 7. Invalid tileWidth (<= 0):
    R"json({
      "type": "TileMatrixSetType",
      "supportedCRS": "EPSG:3857",
      "tileMatrix": [{ "identifier": "0", "scaleDenominator": 100.0,
                       "tileWidth": 0, "tileHeight": 256,
                       "matrixWidth": 1, "matrixHeight": 1 }]
    })json",
    // 8. Invalid tileHeight (<= 0):
    R"json({
      "type": "TileMatrixSetType",
      "supportedCRS": "EPSG:3857",
      "tileMatrix": [{ "identifier": "0", "scaleDenominator": 100.0,
                       "tileWidth": 256, "tileHeight": 0,
                       "matrixWidth": 1, "matrixHeight": 1 }]
    })json",
    // 9. tileWidth * tileHeight > INT_MAX:
    R"json({
      "type": "TileMatrixSetType",
      "supportedCRS": "EPSG:3857",
      "tileMatrix": [{ "identifier": "0", "scaleDenominator": 100.0,
                       "tileWidth": 100000, "tileHeight": 100000,
                       "matrixWidth": 1, "matrixHeight": 1 }]
    })json",
    // 10. Invalid matrixWidth (<= 0):
    R"json({
      "type": "TileMatrixSetType",
      "supportedCRS": "EPSG:3857",
      "tileMatrix": [{ "identifier": "0", "scaleDenominator": 100.0,
                       "tileWidth": 256, "tileHeight": 256,
                       "matrixWidth": 0, "matrixHeight": 1 }]
    })json",
    // 11. Invalid matrixHeight (<= 0):
    R"json({
      "type": "TileMatrixSetType",
      "supportedCRS": "EPSG:3857",
      "tileMatrix": [{ "identifier": "0", "scaleDenominator": 100.0,
                       "tileWidth": 256, "tileHeight": 256,
                       "matrixWidth": 1, "matrixHeight": 0 }]
    })json",
};

TEST(TileMatrixSetTest, PredefinedAndRasterCreation) {
  // Set up a mock GDAL_DATA directory in /vsimem with tms_NZTM2000.json and
  // tms_CustomTest.json so listPredefinedTileMatrixSets() and CPLFindFile work.
  constexpr char kSuiteDir[] = "/vsimem/tilematrixset_test";
  constexpr char kGdalDataDir[] = "/vsimem/tilematrixset_test/tms_gdal_data";
  constexpr char kCustomPath[] =
      "/vsimem/tilematrixset_test/tms_gdal_data/tms_CustomTest.json";
  absl::Cleanup cleanup_dir = [kSuiteDir] { VSIRmdirRecursive(kSuiteDir); };

  const std::string valid_v1_json(kValidV1Json);
  VsiMemTempWrapper nztm_file(
      "/vsimem/tilematrixset_test/tms_gdal_data/tms_NZTM2000.json",
      valid_v1_json);
  VsiMemTempWrapper custom_file(kCustomPath, valid_v1_json);

  CPLPushFinderLocation(kGdalDataDir);
  absl::Cleanup pop_finder = [] { CPLPopFinderLocation(); };

  auto list = gdal::TileMatrixSet::listPredefinedTileMatrixSets();
  EXPECT_GE(list.size(), 6U);

  for (const char* name :
       {"GoogleMapsCompatible", "WebMercatorQuad", "WorldMercatorWGS84Quad",
        "PseudoTMS_GlobalMercator", "WorldCRS84Quad", "InspireCRS84Quad",
        "PseudoTMS_GlobalGeodetic", "GoogleCRS84Quad", "CustomTest"}) {
    auto tms = gdal::TileMatrixSet::parse(name);
    ASSERT_NE(nullptr, tms) << "Failed for " << name;
    EXPECT_FALSE(tms->identifier().empty());
    EXPECT_FALSE(tms->title().empty());
    EXPECT_FALSE(tms->crs().empty());
    EXPECT_FALSE(tms->tileMatrixList().empty());
    EXPECT_TRUE(tms->haveAllLevelsSameTopLeft());
    EXPECT_TRUE(tms->haveAllLevelsSameTileSize());
    EXPECT_TRUE(tms->hasOnlyPowerOfTwoVaryingScales());
  }

  auto custom_tms = gdal::TileMatrixSet::parse("CustomTest");
  ASSERT_NE(nullptr, custom_tms);
  EXPECT_EQ("Test abstract", custom_tms->abstract());
  EXPECT_FALSE(custom_tms->wellKnownScaleSet().empty());
  EXPECT_DOUBLE_EQ(-1000.0, custom_tms->bbox().mLowerCornerX);
  EXPECT_TRUE(custom_tms->hasVariableMatrixWidth());

  std::string exported = custom_tms->exportToTMSJsonV1();
  EXPECT_THAT(exported, HasSubstr("TileMatrixSetType"));
  auto re_parsed = gdal::TileMatrixSet::parse(exported.c_str());
  ASSERT_NE(nullptr, re_parsed);
  EXPECT_EQ("CustomTest", re_parsed->identifier());

  // Also test parse from VSI file path directly.
  auto from_file = gdal::TileMatrixSet::parse(kCustomPath);
  ASSERT_NE(nullptr, from_file);

  // Test createRaster.
  auto raster_tms = gdal::TileMatrixSet::createRaster(
      1024, 512, 256, 3, 0.0, 512.0, 1.0, 1.0, "EPSG:3857");
  ASSERT_NE(nullptr, raster_tms);
  EXPECT_EQ(3U, raster_tms->tileMatrixList().size());
  EXPECT_FALSE(raster_tms->hasVariableMatrixWidth());

  // Mutate tileMatrixList() to test false branches of inspection methods.
  auto& matrices = raster_tms->tileMatrixList();
  matrices[1].mTopLeftX = 999.0;
  EXPECT_FALSE(raster_tms->haveAllLevelsSameTopLeft());
  matrices[1].mTileWidth = 512;
  EXPECT_FALSE(raster_tms->haveAllLevelsSameTileSize());
  matrices[1].mScaleDenominator = 123.456;
  EXPECT_FALSE(raster_tms->hasOnlyPowerOfTwoVaryingScales());
}

TEST(TileMatrixSetTest, ParseTMSv2AndErrorCases) {
  // TMS v2 with crs object containing uri, wkt string, geographic CRS,
  // cornerOfOrigin != topLeft (emits CE_Warning), and variableMatrixWidths.
  constexpr std::string_view kV2UriJson = R"json({
    "id": "TMSv2_URI",
    "title": "TMS v2 URI",
    "description": "v2 desc",
    "crs": { "uri": "http://www.opengis.net/def/crs/OGC/1.3/CRS84" },
    "boundingBox": {
      "crs": { "wkt": "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]" },
      "lowerCorner": [-180.0, -90.0],
      "upperCorner": [180.0, 90.0]
    },
    "tileMatrices": [
      {
        "id": "0",
        "scaleDenominator": 500000.0,
        "cornerOfOrigin": "bottomLeft",
        "pointOfOrigin": [-180.0, 90.0],
        "tileWidth": 256,
        "tileHeight": 256,
        "matrixWidth": 2,
        "matrixHeight": 1,
        "variableMatrixWidths": [
          { "coalesce": 2, "minTileRow": 0, "maxTileRow": 0 }
        ]
      }
    ]
  })json";

  std::unique_ptr<gdal::TileMatrixSet> tms_v2;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    tms_v2 = gdal::TileMatrixSet::parse(std::string(kV2UriJson).c_str());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    CPLErrorReset();
  }
  ASSERT_NE(nullptr, tms_v2);
  EXPECT_EQ("TMSv2_URI", tms_v2->identifier());
  EXPECT_EQ("v2 desc", tms_v2->abstract());
  EXPECT_TRUE(tms_v2->hasVariableMatrixWidth());

  // Inline / named error cases:
  for (std::string_view def : kInvalidDefs) {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, gdal::TileMatrixSet::parse(std::string(def).c_str()))
        << def;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType()) << def;
    CPLErrorReset();
  }

  // JSON file missing TileMatrixSetType:
  constexpr char kSuiteDir[] = "/vsimem/tilematrixset_test";
  constexpr char kBadTmsPath[] = "/vsimem/tilematrixset_test/bad_tms.json";
  absl::Cleanup cleanup_dir = [kSuiteDir] { VSIRmdirRecursive(kSuiteDir); };
  VsiMemTempWrapper bad_tms(kBadTmsPath, R"({"hello": "world"})");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, gdal::TileMatrixSet::parse(kBadTmsPath));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
