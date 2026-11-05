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

// Tests for the TopoJSON driver.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/geojson/ogrtopojsondriver.cpp
// third_party/gdal/ogr/ogrsf_frmts/geojson/ogrtopojsondriver.cpp

#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogr_geojson.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/ogrtopojsondriver_test";

class OGRTopoJSONDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRTopoJSON();
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(OGRTopoJSONDriverTest, RegistrationAndMetadata) {
  // Calling RegisterOGRTopoJSON again tests the idempotency check.
  RegisterOGRTopoJSON();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("TopoJSON");
  ASSERT_NE(poDriver, nullptr);

  EXPECT_STREQ(poDriver->GetDescription(), "TopoJSON");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_VECTOR), "YES");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_LONGNAME), "TopoJSON");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_EXTENSIONS), "json topojson");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_HELPTOPIC),
               "drivers/vector/topojson.html");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_VIRTUALIO), "YES");
  EXPECT_NE(poDriver->GetMetadataItem(GDAL_DMD_OPENOPTIONLIST), nullptr);
  EXPECT_NE(poDriver->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST), nullptr);
  EXPECT_NE(poDriver->pfnOpen, nullptr);
  EXPECT_NE(poDriver->pfnIdentify, nullptr);
}

TEST_F(OGRTopoJSONDriverTest, IdentifyUnknownSource) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("TopoJSON");
  ASSERT_NE(poDriver, nullptr);

  // Non-existent file.
  {
    GDALOpenInfo open_info("/vsimem/nonexistent.json", GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), FALSE);
  }

  // Not TopoJSON text.
  {
    const char kJson[] = "{\"type\": \"FeatureCollection\"}";
    GDALOpenInfo open_info(kJson, GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), FALSE);
  }
}

TEST_F(OGRTopoJSONDriverTest, IdentifyServiceSource) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("TopoJSON");
  ASSERT_NE(poDriver, nullptr);

  // Service URL without single allowed driver -> returns -1 (unknown).
  {
    GDALOpenInfo open_info("http://example.com/data.topojson", GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), -1);
  }

  // Service URL with single allowed driver TopoJSON -> returns TRUE.
  {
    const char* const apszAllowedDrivers[] = {"TopoJSON", nullptr};
    GDALOpenInfo open_info("http://example.com/data.topojson", GDAL_OF_VECTOR);
    open_info.papszAllowedDrivers = apszAllowedDrivers;
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), TRUE);
  }

  // Service URL with TopoJSON: prefix -> returns TRUE.
  {
    GDALOpenInfo open_info("TopoJSON:http://example.com/data.topojson",
                           GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), TRUE);
  }
}

TEST_F(OGRTopoJSONDriverTest, IdentifyValidSources) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("TopoJSON");
  ASSERT_NE(poDriver, nullptr);

  const char kValidJson[] =
      R"json({"type":"Topology","objects":{},"arcs":[]})json";

  // Inline text.
  {
    GDALOpenInfo open_info(kValidJson, GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), TRUE);
  }

  // File in /vsimem/.
  const std::string test_file = std::string(kTestDir) + "/valid.topojson";
  VSILFILE* fp = VSIFOpenL(test_file.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kValidJson, 1, strlen(kValidJson), fp);
  VSIFCloseL(fp);

  {
    GDALOpenInfo open_info(test_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), TRUE);
  }
}

TEST_F(OGRTopoJSONDriverTest, OpenUnknownSourceReturnsNull) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("TopoJSON");
  ASSERT_NE(poDriver, nullptr);

  GDALOpenInfo open_info("{}", GDAL_OF_VECTOR);
  EXPECT_EQ(poDriver->pfnOpen(&open_info), nullptr);
}

TEST_F(OGRTopoJSONDriverTest, OpenValidSources) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("TopoJSON");
  ASSERT_NE(poDriver, nullptr);

  const char kValidTopoJson[] = R"json({
    "type": "Topology",
    "transform": {
      "scale": [1.0, 1.0],
      "translate": [0.0, 0.0]
    },
    "objects": {
      "layer1": {
        "type": "GeometryCollection",
        "geometries": [
          {
            "type": "Point",
            "coordinates": [10.0, 20.0],
            "properties": {"name": "p1"}
          }
        ]
      }
    },
    "arcs": []
  })json";

  // Open via pfnOpen from text.
  {
    GDALOpenInfo open_info(kValidTopoJson, GDAL_OF_VECTOR);
    GDALDataset* poDS = poDriver->pfnOpen(&open_info);
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetLayerCount(), 1);
    OGRLayer* poLayer = poDS->GetLayer(0);
    ASSERT_NE(poLayer, nullptr);
    EXPECT_EQ(poLayer->GetFeatureCount(), 1);
    GDALClose(poDS);
  }

  // Open via GDALOpenEx from file.
  const std::string test_file = std::string(kTestDir) + "/test_open.topojson";
  VSILFILE* fp = VSIFOpenL(test_file.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kValidTopoJson, 1, strlen(kValidTopoJson), fp);
  VSIFCloseL(fp);

  {
    const char* const apszAllowedDrivers[] = {"TopoJSON", nullptr};
    GDALDataset* poDS = static_cast<GDALDataset*>(
        GDALOpenEx(test_file.c_str(), GDAL_OF_VECTOR, apszAllowedDrivers,
                   nullptr, nullptr));
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetLayerCount(), 1);
    GDALClose(poDS);
  }
}

TEST_F(OGRTopoJSONDriverTest, OpenCorruptJsonReturnsNull) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("TopoJSON");
  ASSERT_NE(poDriver, nullptr);

  const char kCorruptJson[] =
      R"json({"type":"Topology","objects":corrupt})json";

  WithQuietHandler quiet;
  GDALOpenInfo open_info(kCorruptJson, GDAL_OF_VECTOR);
  GDALDataset* poDS = poDriver->pfnOpen(&open_info);
  EXPECT_EQ(poDS, nullptr);
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
