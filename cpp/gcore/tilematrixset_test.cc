// Copyright 2026 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/gcore/tilematrixset.hpp"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

void WriteTextFile(const char* path, const std::string& content) {
  VSILFILE* fp = VSIFOpenL(path, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(VSIFWriteL(content.data(), 1, content.size(), fp), content.size());
  ASSERT_EQ(VSIFCloseL(fp), 0);
}

TEST(TileMatrixSetTest, PredefinedAndRasterCreation) {
  // Set up a mock GDAL_DATA directory in /vsimem with tms_NZTM2000.json and
  // tms_CustomTest.json so listPredefinedTileMatrixSets() and CPLFindFile work.
  VSIMkdir("/vsimem/tms_gdal_data", 0755);
  const std::string valid_v1_json = R"json({
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
  WriteTextFile("/vsimem/tms_gdal_data/tms_NZTM2000.json", valid_v1_json);
  WriteTextFile("/vsimem/tms_gdal_data/tms_CustomTest.json", valid_v1_json);
  CPLPushFinderLocation("/vsimem/tms_gdal_data");

  auto list = gdal::TileMatrixSet::listPredefinedTileMatrixSets();
  EXPECT_GE(list.size(), 6U);

  for (const char* name :
       {"GoogleMapsCompatible", "WebMercatorQuad", "WorldMercatorWGS84Quad",
        "PseudoTMS_GlobalMercator", "WorldCRS84Quad", "InspireCRS84Quad",
        "PseudoTMS_GlobalGeodetic", "GoogleCRS84Quad", "CustomTest"}) {
    auto tms = gdal::TileMatrixSet::parse(name);
    ASSERT_NE(tms, nullptr) << "Failed for " << name;
    EXPECT_FALSE(tms->identifier().empty());
    EXPECT_FALSE(tms->title().empty());
    EXPECT_FALSE(tms->crs().empty());
    EXPECT_FALSE(tms->tileMatrixList().empty());
    EXPECT_TRUE(tms->haveAllLevelsSameTopLeft());
    EXPECT_TRUE(tms->haveAllLevelsSameTileSize());
    EXPECT_TRUE(tms->hasOnlyPowerOfTwoVaryingScales());
  }

  auto custom_tms = gdal::TileMatrixSet::parse("CustomTest");
  ASSERT_NE(custom_tms, nullptr);
  EXPECT_EQ(custom_tms->abstract(), "Test abstract");
  EXPECT_FALSE(custom_tms->wellKnownScaleSet().empty());
  EXPECT_EQ(custom_tms->bbox().mLowerCornerX, -1000.0);
  EXPECT_TRUE(custom_tms->hasVariableMatrixWidth());

  std::string exported = custom_tms->exportToTMSJsonV1();
  EXPECT_NE(exported.find("TileMatrixSetType"), std::string::npos);
  auto re_parsed = gdal::TileMatrixSet::parse(exported.c_str());
  ASSERT_NE(re_parsed, nullptr);
  EXPECT_EQ(re_parsed->identifier(), "CustomTest");

  // Also test parse from VSI file path directly.
  auto from_file =
      gdal::TileMatrixSet::parse("/vsimem/tms_gdal_data/tms_CustomTest.json");
  ASSERT_NE(from_file, nullptr);

  // Test createRaster.
  auto raster_tms = gdal::TileMatrixSet::createRaster(
      1024, 512, 256, 3, 0.0, 512.0, 1.0, 1.0, "EPSG:3857");
  ASSERT_NE(raster_tms, nullptr);
  EXPECT_EQ(raster_tms->tileMatrixList().size(), 3U);
  EXPECT_FALSE(raster_tms->hasVariableMatrixWidth());

  // Mutate tileMatrixList() to test false branches of inspection methods.
  auto& matrices = raster_tms->tileMatrixList();
  matrices[1].mTopLeftX = 999.0;
  EXPECT_FALSE(raster_tms->haveAllLevelsSameTopLeft());
  matrices[1].mTileWidth = 512;
  EXPECT_FALSE(raster_tms->haveAllLevelsSameTileSize());
  matrices[1].mScaleDenominator = 123.456;
  EXPECT_FALSE(raster_tms->hasOnlyPowerOfTwoVaryingScales());

  CPLPopFinderLocation();
  VSIRmdirRecursive("/vsimem/tms_gdal_data");
}

TEST(TileMatrixSetTest, ParseTMSv2AndErrorCases) {
  // TMS v2 with crs object containing uri, wkt string, wkt object,
  // geographic CRS, cornerOfOrigin != topLeft, and variableMatrixWidths.
  const std::string v2_uri_json = R"json({
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

  CPLPushErrorHandler(CPLQuietErrorHandler);
  auto tms_v2 = gdal::TileMatrixSet::parse(v2_uri_json.c_str());
  ASSERT_NE(tms_v2, nullptr);
  EXPECT_EQ(tms_v2->identifier(), "TMSv2_URI");
  EXPECT_EQ(tms_v2->abstract(), "v2 desc");
  EXPECT_TRUE(tms_v2->hasVariableMatrixWidth());

  // TMS v2 with crs.wkt as JSON object.
  const std::string v2_wkt_obj_json = R"json({
    "id": "TMSv2_WKTObj",
    "crs": { "wkt": { "type": "GeographicCRS" } },
    "tileMatrices": []
  })json";
  EXPECT_EQ(gdal::TileMatrixSet::parse(v2_wkt_obj_json.c_str()), nullptr);

  // Error cases:
  // 1. Non-existent name:
  EXPECT_EQ(gdal::TileMatrixSet::parse("NonExistentTMSName"), nullptr);

  // 2. Malformed JSON in memory:
  EXPECT_EQ(gdal::TileMatrixSet::parse("{\"crs\": \"tileMatrices\" broken"),
            nullptr);

  // 3. JSON file missing TileMatrixSetType:
  WriteTextFile("/vsimem/bad_tms.json", "{\"hello\": \"world\"}");
  EXPECT_EQ(gdal::TileMatrixSet::parse("/vsimem/bad_tms.json"), nullptr);
  VSIUnlink("/vsimem/bad_tms.json");

  // 4. Invalid CRS:
  EXPECT_EQ(gdal::TileMatrixSet::parse(R"json({
    "type": "TileMatrixSetType",
    "supportedCRS": "INVALID_CRS_STRING",
    "tileMatrix": []
  })json"),
            nullptr);

  // 5. Empty tileMatrix array:
  EXPECT_EQ(gdal::TileMatrixSet::parse(R"json({
    "type": "TileMatrixSetType",
    "supportedCRS": "EPSG:3857",
    "tileMatrix": []
  })json"),
            nullptr);

  // 6. Invalid scaleDenominator (<= 0):
  EXPECT_EQ(gdal::TileMatrixSet::parse(R"json({
    "type": "TileMatrixSetType",
    "supportedCRS": "EPSG:3857",
    "tileMatrix": [{ "identifier": "0", "scaleDenominator": -1.0,
                     "tileWidth": 256, "tileHeight": 256,
                     "matrixWidth": 1, "matrixHeight": 1 }]
  })json"),
            nullptr);

  // 7. Invalid tileWidth (<= 0):
  EXPECT_EQ(gdal::TileMatrixSet::parse(R"json({
    "type": "TileMatrixSetType",
    "supportedCRS": "EPSG:3857",
    "tileMatrix": [{ "identifier": "0", "scaleDenominator": 100.0,
                     "tileWidth": 0, "tileHeight": 256,
                     "matrixWidth": 1, "matrixHeight": 1 }]
  })json"),
            nullptr);

  // 8. Invalid tileHeight (<= 0):
  EXPECT_EQ(gdal::TileMatrixSet::parse(R"json({
    "type": "TileMatrixSetType",
    "supportedCRS": "EPSG:3857",
    "tileMatrix": [{ "identifier": "0", "scaleDenominator": 100.0,
                     "tileWidth": 256, "tileHeight": 0,
                     "matrixWidth": 1, "matrixHeight": 1 }]
  })json"),
            nullptr);

  // 9. tileWidth * tileHeight > INT_MAX:
  EXPECT_EQ(gdal::TileMatrixSet::parse(R"json({
    "type": "TileMatrixSetType",
    "supportedCRS": "EPSG:3857",
    "tileMatrix": [{ "identifier": "0", "scaleDenominator": 100.0,
                     "tileWidth": 100000, "tileHeight": 100000,
                     "matrixWidth": 1, "matrixHeight": 1 }]
  })json"),
            nullptr);

  // 10. Invalid matrixWidth (<= 0):
  EXPECT_EQ(gdal::TileMatrixSet::parse(R"json({
    "type": "TileMatrixSetType",
    "supportedCRS": "EPSG:3857",
    "tileMatrix": [{ "identifier": "0", "scaleDenominator": 100.0,
                     "tileWidth": 256, "tileHeight": 256,
                     "matrixWidth": 0, "matrixHeight": 1 }]
  })json"),
            nullptr);

  // 11. Invalid matrixHeight (<= 0):
  EXPECT_EQ(gdal::TileMatrixSet::parse(R"json({
    "type": "TileMatrixSetType",
    "supportedCRS": "EPSG:3857",
    "tileMatrix": [{ "identifier": "0", "scaleDenominator": 100.0,
                     "tileWidth": 256, "tileHeight": 256,
                     "matrixWidth": 1, "matrixHeight": 0 }]
  })json"),
            nullptr);

  CPLPopErrorHandler();
}

}  // namespace
