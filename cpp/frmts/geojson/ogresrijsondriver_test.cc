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

// Tests for the ESRIJSON driver.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/geojson/ogresrijsondriver.cpp
// third_party/gdal/ogr/ogrsf_frmts/geojson/ogresrijsondriver.cpp

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

constexpr char kTestDir[] = "/vsimem/ogresrijsondriver_test";

class OGRESRIJSONDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRESRIJSON();
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(OGRESRIJSONDriverTest, RegistrationAndMetadata) {
  // Calling RegisterOGRESRIJSON again hits the idempotency guard.
  RegisterOGRESRIJSON();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRIJSON");
  ASSERT_NE(poDriver, nullptr);

  EXPECT_STREQ(poDriver->GetDescription(), "ESRIJSON");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_VECTOR), "YES");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_LONGNAME), "ESRIJSON");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_EXTENSION), "json");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_HELPTOPIC),
               "drivers/vector/esrijson.html");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_Z_GEOMETRIES), "YES");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_MEASURED_GEOMETRIES), "YES");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_VIRTUALIO), "YES");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS),
               "OGRSQL SQLITE");
  EXPECT_NE(poDriver->GetMetadataItem(GDAL_DMD_OPENOPTIONLIST), nullptr);
  EXPECT_NE(poDriver->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST), nullptr);
  EXPECT_NE(poDriver->pfnOpen, nullptr);
  EXPECT_NE(poDriver->pfnIdentify, nullptr);
}

TEST_F(OGRESRIJSONDriverTest, IdentifyUnknownSource) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRIJSON");
  ASSERT_NE(poDriver, nullptr);

  // Non-existent file.
  {
    GDALOpenInfo open_info("/vsimem/does_not_exist_file.json", GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), FALSE);
  }

  // Not ESRIJSON text.
  {
    const char kJson[] = "{\"type\": \"FeatureCollection\"}";
    GDALOpenInfo open_info(kJson, GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), FALSE);
  }
}

TEST_F(OGRESRIJSONDriverTest, IdentifyServiceSource) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRIJSON");
  ASSERT_NE(poDriver, nullptr);

  // Service URL without single allowed driver -> returns -1 (unknown).
  {
    GDALOpenInfo open_info(
        "http://example.com/arcgis/rest/services/query?f=json", GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), -1);
  }

  // Service URL with single allowed driver ESRIJSON -> returns TRUE.
  {
    const char* const apszAllowedDrivers[] = {"ESRIJSON", nullptr};
    GDALOpenInfo open_info(
        "http://example.com/arcgis/rest/services/query?f=json", GDAL_OF_VECTOR);
    open_info.papszAllowedDrivers = apszAllowedDrivers;
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), TRUE);
  }

  // Service URL with ESRIJSON: prefix -> returns TRUE.
  {
    GDALOpenInfo open_info(
        "ESRIJSON:http://example.com/arcgis/rest/services/query?f=json",
        GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), TRUE);
  }
}

TEST_F(OGRESRIJSONDriverTest, IdentifyValidSources) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRIJSON");
  ASSERT_NE(poDriver, nullptr);

  // Valid ESRIJSON text.
  const char kValidJson[] =
      R"json({"geometryType":"esriGeometryPoint","features":[]})json";
  {
    GDALOpenInfo open_info(kValidJson, GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), TRUE);
  }

  // Valid ESRIJSON file.
  const std::string test_file = std::string(kTestDir) + "/valid.json";
  VSILFILE* fp = VSIFOpenL(test_file.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kValidJson, 1, strlen(kValidJson), fp);
  VSIFCloseL(fp);

  {
    GDALOpenInfo open_info(test_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), TRUE);
  }
}

TEST_F(OGRESRIJSONDriverTest, OpenUnknownSourceReturnsNull) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRIJSON");
  ASSERT_NE(poDriver, nullptr);

  GDALOpenInfo open_info("{}", GDAL_OF_VECTOR);
  EXPECT_EQ(poDriver->pfnOpen(&open_info), nullptr);
}

TEST_F(OGRESRIJSONDriverTest, OpenValidSources) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRIJSON");
  ASSERT_NE(poDriver, nullptr);

  const char kValidJson[] = R"json({
    "geometryType": "esriGeometryPoint",
    "spatialReference": {"wkid": 4326},
    "fields": [
      {"name": "ID", "type": "esriFieldTypeInteger", "alias": "Identifier"}
    ],
    "features": [
      {
        "attributes": {"ID": 1},
        "geometry": {"x": 10.0, "y": 20.0}
      }
    ]
  })json";

  // Open via pfnOpen from text.
  {
    GDALOpenInfo open_info(kValidJson, GDAL_OF_VECTOR);
    GDALDataset* poDS = poDriver->pfnOpen(&open_info);
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetLayerCount(), 1);
    OGRLayer* poLayer = poDS->GetLayer(0);
    ASSERT_NE(poLayer, nullptr);
    EXPECT_EQ(poLayer->GetFeatureCount(), 1);
    GDALClose(poDS);
  }

  // Open via GDALOpenEx from file.
  const std::string test_file = std::string(kTestDir) + "/test_open.json";
  VSILFILE* fp = VSIFOpenL(test_file.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kValidJson, 1, strlen(kValidJson), fp);
  VSIFCloseL(fp);

  {
    const char* const apszAllowedDrivers[] = {"ESRIJSON", nullptr};
    GDALDataset* poDS = static_cast<GDALDataset*>(
        GDALOpenEx(test_file.c_str(), GDAL_OF_VECTOR, apszAllowedDrivers,
                   nullptr, nullptr));
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetLayerCount(), 1);
    GDALClose(poDS);
  }
}

TEST_F(OGRESRIJSONDriverTest, OpenCorruptJsonReturnsNull) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("ESRIJSON");
  ASSERT_NE(poDriver, nullptr);

  const char kCorruptJson[] =
      R"json({"geometryType":"esriGeometryPoint","features":[corrupt})json";

  WithQuietHandler quiet;
  GDALOpenInfo open_info(kCorruptJson, GDAL_OF_VECTOR);
  GDALDataset* poDS = poDriver->pfnOpen(&open_info);
  EXPECT_EQ(poDS, nullptr);
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
