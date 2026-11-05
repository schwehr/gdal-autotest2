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

// Tests for the GeoJSON driver.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/geojson/ogrgeojsondriver.cpp
// third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsondriver.cpp

#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogr_geojson.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonutils.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_http.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/ogrgeojsondriver_test";

class OGRGeoJSONDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRGeoJSON();
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(OGRGeoJSONDriverTest, RegistrationAndMetadata) {
  RegisterOGRGeoJSON();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
  ASSERT_NE(poDriver, nullptr);

  EXPECT_STREQ(poDriver->GetDescription(), "GeoJSON");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_VECTOR), "YES");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_LONGNAME), "GeoJSON");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_EXTENSIONS), "json geojson");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_HELPTOPIC),
               "drivers/vector/geojson.html");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_VIRTUALIO), "YES");
  EXPECT_NE(poDriver->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST), nullptr);
  EXPECT_NE(poDriver->pfnOpen, nullptr);
  EXPECT_NE(poDriver->pfnIdentify, nullptr);
  EXPECT_NE(poDriver->pfnCreate, nullptr);
  EXPECT_NE(poDriver->pfnDelete, nullptr);
}

TEST_F(OGRGeoJSONDriverTest, StoredContentManagement) {
  const char kSource1[] = "http://example.com/data1";
  const char kSource2[] = "http://example.com/data2";

  // Store content
  OGRGeoJSONDriverStoreContent(kSource1, CPLStrdup("content1"));

  // Stealing with mismatched source returns nullptr
  char* stolenWrong = OGRGeoJSONDriverStealStoredContent(kSource2);
  EXPECT_EQ(stolenWrong, nullptr);

  // Stealing with matching source succeeds
  char* stolenRight = OGRGeoJSONDriverStealStoredContent(kSource1);
  ASSERT_NE(stolenRight, nullptr);
  EXPECT_STREQ(stolenRight, "content1");
  CPLFree(stolenRight);

  // Subsequent steal returns nullptr
  EXPECT_EQ(OGRGeoJSONDriverStealStoredContent(kSource1), nullptr);

  // Store again and unload driver to test cleanup
  OGRGeoJSONDriverStoreContent(kSource1, CPLStrdup("content_to_cleanup"));
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
  ASSERT_NE(poDriver, nullptr);
  if (poDriver->pfnUnloadDriver) {
    poDriver->pfnUnloadDriver(poDriver);
  }
  RegisterOGRGeoJSON();
}

TEST_F(OGRGeoJSONDriverTest, IdentifyVariants) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
  ASSERT_NE(poDriver, nullptr);

  // 1. Non-existent file -> FALSE
  {
    GDALOpenInfo open_info("/vsimem/does_not_exist.geojson", GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), FALSE);
  }

  // 2. Starts with {"properties":{ -> returns GDAL_IDENTIFY_UNKNOWN (-1)
  {
    const std::string props_file = std::string(kTestDir) + "/props.json";
    const char kPropsOnly[] = "{\"properties\":{\"a\":1}}";
    VSILFILE* fp = VSIFOpenL(props_file.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(kPropsOnly, 1, strlen(kPropsOnly), fp);
    VSIFCloseL(fp);
    GDALOpenInfo open_info(props_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&open_info), GDAL_IDENTIFY_UNKNOWN);
  }

  // 3. Service URLs
  {
    // Without allowed driver -> -1
    GDALOpenInfo oi1("http://example.com/test.geojson", GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&oi1), -1);

    // With single allowed driver GeoJSON -> TRUE
    const char* const apszAllowed[] = {"GeoJSON", nullptr};
    GDALOpenInfo oi2("http://example.com/test.geojson", GDAL_OF_VECTOR);
    oi2.papszAllowedDrivers = apszAllowed;
    EXPECT_EQ(poDriver->pfnIdentify(&oi2), TRUE);

    // With GeoJSON: prefix -> TRUE
    GDALOpenInfo oi3("GeoJSON:http://example.com/test.geojson", GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&oi3), TRUE);
  }

  // 4. STACTA driver check: when STACTA driver is registered, stac_extensions +
  // tiled-assets -> FALSE
  {
    GDALDriver* poSTACTA = new GDALDriver();
    poSTACTA->SetDescription("STACTA");
    GetGDALDriverManager()->RegisterDriver(poSTACTA);
    absl::Cleanup cleanupSTACTA = [poSTACTA] {
      GetGDALDriverManager()->DeregisterDriver(poSTACTA);
      delete poSTACTA;
    };

    const std::string stac_file = std::string(kTestDir) + "/stac.json";
    const char kStacJson[] =
        R"json({"type":"FeatureCollection","stac_extensions":[],"tiled-assets":{},"features":[]})json";
    VSILFILE* fp = VSIFOpenL(stac_file.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(kStacJson, 1, strlen(kStacJson), fp);
    VSIFCloseL(fp);

    GDALOpenInfo oiStac(stac_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&oiStac), FALSE);

    // But if GeoJSON is the single allowed driver -> returns TRUE
    const char* const apszAllowed[] = {"GeoJSON", nullptr};
    oiStac.papszAllowedDrivers = apszAllowed;
    EXPECT_EQ(poDriver->pfnIdentify(&oiStac), TRUE);
  }
}

TEST_F(OGRGeoJSONDriverTest, CreateAndDelete) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
  ASSERT_NE(poDriver, nullptr);

  const std::string filename = std::string(kTestDir) + "/created.geojson";

  // Create on /vsimem/
  GDALDataset* poDS =
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
  ASSERT_NE(poDS, nullptr);
  GDALClose(poDS);

  // Delete valid file
  EXPECT_EQ(poDriver->Delete(filename.c_str()), CE_None);

  // Delete non-existent file
  WithQuietHandler quiet;
  EXPECT_EQ(poDriver->Delete("/vsimem/nonexistent_file.geojson"), CE_Failure);
  CPLErrorReset();

  // Create invalid path (existing directory)
  const std::string dir_path = std::string(kTestDir) + "/is_a_dir";
  VSIMkdir(dir_path.c_str(), 0755);
  WithQuietHandler quiet2;
  EXPECT_EQ(poDriver->Create(dir_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr),
            nullptr);
  CPLErrorReset();
}

static CPLHTTPResult* FakeESRIFSOffsetCallback(
    const char* pszURL, CSLConstList /*papszOptions*/,
    GDALProgressFunc /*pfnProgress*/, void* /*pProgressArg*/,
    CPLHTTPFetchWriteFunc /*pfnWrite*/, void* /*pWriteArg*/,
    void* /*pUserData*/) {
  auto* psResult =
      static_cast<CPLHTTPResult*>(CPLCalloc(1, sizeof(CPLHTTPResult)));
  psResult->nStatus = 0;

  if (strstr(pszURL, "returnCountOnly=true")) {
    if (strstr(pszURL, "fail_count")) {
      psResult->pabyData = reinterpret_cast<GByte*>(CPLStrdup("{}"));
      psResult->nDataLen = 2;
    } else {
      psResult->pabyData =
          reinterpret_cast<GByte*>(CPLStrdup("{\"count\": 42}"));
      psResult->nDataLen = strlen(reinterpret_cast<char*>(psResult->pabyData));
    }
  } else if (strstr(pszURL, "returnExtentOnly=true")) {
    if (strstr(pszURL, "use_extent_obj")) {
      psResult->pabyData = reinterpret_cast<GByte*>(
          CPLStrdup("{\"extent\": {\"xmin\": 10, \"ymin\": 20, \"xmax\": 30, "
                    "\"ymax\": 40}}"));
      psResult->nDataLen = strlen(reinterpret_cast<char*>(psResult->pabyData));
    } else if (strstr(pszURL, "fail_extent")) {
      psResult->pabyData = reinterpret_cast<GByte*>(CPLStrdup("{}"));
      psResult->nDataLen = 2;
    } else {
      psResult->pabyData = reinterpret_cast<GByte*>(
          CPLStrdup("{\"bbox\": [1.0, 2.0, 3.0, 4.0]}"));
      psResult->nDataLen = strlen(reinterpret_cast<char*>(psResult->pabyData));
    }
  }

  return psResult;
}

TEST_F(OGRGeoJSONDriverTest, ESRIFeatureServicePagingAndLayer) {
  // Test OGRESRIFeatureServiceDataset and OGRESRIFeatureServiceLayer
  // Triggered when poDS->HasOtherPages() is true
  // (properties.exceededTransferLimit == true) and filename starts with
  // /vsimem/
  const std::string page0_name = std::string(kTestDir) + "/page.json";
  const std::string page0_offset0 =
      std::string(kTestDir) + "/page.json?resultRecordCount=1&resultOffset=0";
  const std::string page1_name =
      std::string(kTestDir) + "/page.json?resultRecordCount=1&resultOffset=1";

  const char kPage0[] = R"json({
    "type": "FeatureCollection",
    "properties": {"exceededTransferLimit": true},
    "features": [
      {
        "type": "Feature",
        "id": 0,
        "properties": {"name": "f1"},
        "geometry": {"type": "Point", "coordinates": [1.0, 2.0]}
      }
    ]
  })json";

  const char kPage1[] = R"json({
    "type": "FeatureCollection",
    "properties": {"exceededTransferLimit": false},
    "features": [
      {
        "type": "Feature",
        "id": 0,
        "properties": {"name": "f2"},
        "geometry": {"type": "Point", "coordinates": [3.0, 4.0]}
      }
    ]
  })json";

  VSILFILE* fp0 = VSIFOpenL(page0_name.c_str(), "wb");
  ASSERT_NE(fp0, nullptr);
  VSIFWriteL(kPage0, 1, strlen(kPage0), fp0);
  VSIFCloseL(fp0);

  VSILFILE* fp0_off = VSIFOpenL(page0_offset0.c_str(), "wb");
  ASSERT_NE(fp0_off, nullptr);
  VSIFWriteL(kPage0, 1, strlen(kPage0), fp0_off);
  VSIFCloseL(fp0_off);

  VSILFILE* fp1 = VSIFOpenL(page1_name.c_str(), "wb");
  ASSERT_NE(fp1, nullptr);
  VSIFWriteL(kPage1, 1, strlen(kPage1), fp1);
  VSIFCloseL(fp1);

  GDALDataset* poDS = static_cast<GDALDataset*>(GDALOpenEx(
      page0_name.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
  ASSERT_NE(poDS, nullptr);

  EXPECT_EQ(poDS->GetLayerCount(), 1);
  EXPECT_EQ(poDS->GetLayer(-1), nullptr);
  EXPECT_EQ(poDS->GetLayer(1), nullptr);

  OGRLayer* poLayer = poDS->GetLayer(0);
  ASSERT_NE(poLayer, nullptr);

  // Test capabilities
  EXPECT_TRUE(poLayer->TestCapability(OLCFastFeatureCount));
  EXPECT_FALSE(poLayer->TestCapability(OLCFastGetExtent));
  EXPECT_TRUE(poLayer->TestCapability(OLCStringsAsUTF8));
  EXPECT_TRUE(poLayer->TestCapability(OLCRandomRead));

  // Read features across pages: page 0 has f1, page 1 has f2
  OGRFeature* feat1 = poLayer->GetNextFeature();
  ASSERT_NE(feat1, nullptr);
  EXPECT_EQ(feat1->GetFID(), 0);
  EXPECT_STREQ(feat1->GetFieldAsString("name"), "f1");
  delete feat1;

  OGRFeature* feat2 = poLayer->GetNextFeature();
  ASSERT_NE(feat2, nullptr);
  EXPECT_EQ(feat2->GetFID(),
            1);  // Sequential FID assigned because first FID was 0
  EXPECT_STREQ(feat2->GetFieldAsString("name"), "f2");
  delete feat2;

  EXPECT_EQ(poLayer->GetNextFeature(), nullptr);

  // ResetReading rewinds to offset 0 and reloads page 0
  poLayer->ResetReading();
  OGRFeature* featReload = poLayer->GetNextFeature();
  ASSERT_NE(featReload, nullptr);
  EXPECT_EQ(featReload->GetFID(), 0);
  delete featReload;

  // Spatial and attribute filter evaluation
  poLayer->SetSpatialFilterRect(2.5, 3.5, 3.5, 4.5);
  EXPECT_FALSE(poLayer->TestCapability(OLCFastFeatureCount));
  EXPECT_FALSE(poLayer->TestCapability(OLCFastGetExtent));
  poLayer->SetSpatialFilter(nullptr);

  poLayer->SetAttributeFilter("name = 'f1'");
  poLayer->SetAttributeFilter(nullptr);

  // Test HTTP callback integration for returnCountOnly and returnExtentOnly
  CPLHTTPPushFetchCallback(FakeESRIFSOffsetCallback, nullptr);
  absl::Cleanup popHttp = [] { CPLHTTPPopFetchCallback(); };

  EXPECT_EQ(poLayer->GetFeatureCount(), 42);

  OGREnvelope sExtent;
  EXPECT_EQ(poLayer->GetExtent(&sExtent, TRUE), OGRERR_NONE);
  EXPECT_DOUBLE_EQ(sExtent.MinX, 1.0);
  EXPECT_DOUBLE_EQ(sExtent.MaxX, 3.0);

  GDALClose(poDS);
}

TEST_F(OGRGeoJSONDriverTest, ESRIFeatureServiceLoopAndRecordCountWarning) {
  // 1. Loop detection: next page has identical first FID -> stops paging
  {
    const std::string loop0 = std::string(kTestDir) + "/loop.json";
    const std::string loop1 =
        std::string(kTestDir) + "/loop.json?resultRecordCount=1&resultOffset=1";

    const char kLoopData[] = R"json({
      "type": "FeatureCollection",
      "properties": {"exceededTransferLimit": true},
      "features": [
        {"type": "Feature", "id": 10, "properties": {}, "geometry": null}
      ]
    })json";

    VSILFILE* f0 = VSIFOpenL(loop0.c_str(), "wb");
    ASSERT_NE(f0, nullptr);
    VSIFWriteL(kLoopData, 1, strlen(kLoopData), f0);
    VSIFCloseL(f0);

    VSILFILE* f1 = VSIFOpenL(loop1.c_str(), "wb");
    ASSERT_NE(f1, nullptr);
    VSIFWriteL(kLoopData, 1, strlen(kLoopData), f1);
    VSIFCloseL(f1);

    auto poDS = GDALDatasetUniquePtr(static_cast<GDALDataset*>(
        GDALOpenEx(loop0.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer = poDS->GetLayer(0);
    ASSERT_NE(poLayer, nullptr);

    OGRFeatureUniquePtr fFirst(poLayer->GetNextFeature());
    ASSERT_NE(fFirst, nullptr);

    // Second call loads loop1, sees FID 10 == nFirstFID, stops paging!
    EXPECT_EQ(poLayer->GetNextFeature(), nullptr);
  }

  // 2. resultRecordCount warning when greater than actual features
  {
    const std::string rc0 =
        std::string(kTestDir) + "/rc.json?resultRecordCount=10";
    const std::string rc1 =
        std::string(kTestDir) + "/rc.json?resultRecordCount=10&resultOffset=1";

    const char kData0[] = R"json({
      "type": "FeatureCollection",
      "properties": {"exceededTransferLimit": true},
      "features": [
        {"type": "Feature", "id": 1, "properties": {}, "geometry": null}
      ]
    })json";

    const char kData1[] = R"json({
      "type": "FeatureCollection",
      "properties": {"exceededTransferLimit": false},
      "features": []
    })json";

    VSILFILE* f0 = VSIFOpenL(rc0.c_str(), "wb");
    ASSERT_NE(f0, nullptr);
    VSIFWriteL(kData0, 1, strlen(kData0), f0);
    VSIFCloseL(f0);

    VSILFILE* f1 = VSIFOpenL(rc1.c_str(), "wb");
    ASSERT_NE(f1, nullptr);
    VSIFWriteL(kData1, 1, strlen(kData1), f1);
    VSIFCloseL(f1);

    WithQuietHandler quiet;
    auto poDS = GDALDatasetUniquePtr(static_cast<GDALDataset*>(
        GDALOpenEx(rc0.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(poDS, nullptr);
    OGRLayer* poLayer = poDS->GetLayer(0);
    ASSERT_NE(poLayer, nullptr);
    OGRFeatureUniquePtr feat(poLayer->GetNextFeature());
    ASSERT_NE(feat, nullptr);
    EXPECT_EQ(feat->GetFID(), 1);
    OGRFeatureUniquePtr featNext(poLayer->GetNextFeature());
    EXPECT_EQ(featNext, nullptr);
    CPLErrorReset();
  }
}

TEST_F(OGRGeoJSONDriverTest, OpenInternalOptions) {
  // Test GEOMETRY_AS_COLLECTION and ATTRIBUTES_SKIP config options
  CPLSetThreadLocalConfigOption("GEOMETRY_AS_COLLECTION", "YES");
  CPLSetThreadLocalConfigOption("ATTRIBUTES_SKIP", "YES");
  absl::Cleanup cleanup = [] {
    CPLSetThreadLocalConfigOption("GEOMETRY_AS_COLLECTION", nullptr);
    CPLSetThreadLocalConfigOption("ATTRIBUTES_SKIP", nullptr);
  };

  const char kJson[] = R"json({
    "type": "FeatureCollection",
    "features": [
      {
        "type": "Feature",
        "properties": {"name": "test"},
        "geometry": {"type": "Point", "coordinates": [1.0, 2.0]}
      }
    ]
  })json";

  auto poDS = GDALDatasetUniquePtr(static_cast<GDALDataset*>(
      GDALOpenEx(kJson, GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  ASSERT_NE(poDS, nullptr);
  OGRLayer* poLayer = poDS->GetLayer(0);
  ASSERT_NE(poLayer, nullptr);

  OGRFeatureUniquePtr poFeat(poLayer->GetNextFeature());
  ASSERT_NE(poFeat, nullptr);
  ASSERT_NE(poFeat->GetGeometryRef(), nullptr);
  // With GEOMETRY_AS_COLLECTION=YES, geometry is GeometryCollection
  EXPECT_EQ(poFeat->GetGeometryRef()->getGeometryType(), wkbGeometryCollection);
}

}  // namespace
}  // namespace autotest2
