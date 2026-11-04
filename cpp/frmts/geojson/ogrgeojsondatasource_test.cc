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

// Tests for the GeoJSON datasource.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/geojson/ogrgeojsondatasource.cpp
// third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsondatasource.cpp

#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogr_geojson.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonutils.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_http.h"
#include "third_party/gdal/port/cpl_json_header.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogrgeojsondatasource_test";

class OGRGeoJSONDataSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRGeoJSON();
    RegisterOGRESRIJSON();
    RegisterOGRTopoJSON();
    RegisterOGRGeoJSONSeq();
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(OGRGeoJSONDataSourceTest, LifecycleAccessorsAndCapabilities) {
  OGRGeoJSONDataSource ds;

  EXPECT_EQ(ds.GetLayerCount(), 0);
  EXPECT_EQ(ds.GetLayer(0), nullptr);
  EXPECT_EQ(ds.GetLayer(-1), nullptr);
  EXPECT_EQ(ds.GetLayer(5), nullptr);
  EXPECT_EQ(ds.GetOutputFile(), nullptr);
  EXPECT_FALSE(ds.GetFpOutputIsSeekable());
  EXPECT_EQ(ds.GetBBOXInsertLocation(), 0);
  EXPECT_FALSE(ds.HasOtherPages());
  EXPECT_FALSE(ds.IsUpdatable());
  EXPECT_STREQ(ds.GetJSonFlavor().c_str(), "");

  ds.SetGeometryTranslation(OGRGeoJSONDataSource::eGeometryAsCollection);
  ds.SetGeometryTranslation(OGRGeoJSONDataSource::eGeometryPreserve);
  ds.SetAttributesTranslation(OGRGeoJSONDataSource::eAttributesSkip);
  ds.SetAttributesTranslation(OGRGeoJSONDataSource::eAttributesPreserve);

  EXPECT_FALSE(ds.TestCapability(ODsCCreateLayer));

  ds.SetSupportsMGeometries(true);
  EXPECT_TRUE(ds.TestCapability(ODsCMeasuredGeometries));
  ds.SetSupportsMGeometries(false);
  EXPECT_FALSE(ds.TestCapability(ODsCMeasuredGeometries));

  ds.SetSupportsZGeometries(true);
  EXPECT_TRUE(ds.TestCapability(ODsCZGeometries));
  ds.SetSupportsZGeometries(false);
  EXPECT_FALSE(ds.TestCapability(ODsCZGeometries));

  EXPECT_FALSE(ds.TestCapability("UnknownCapability"));

  EXPECT_EQ(ds.FlushCache(false), CE_None);
  EXPECT_EQ(ds.Close(), CE_None);
}

TEST_F(OGRGeoJSONDataSourceTest, AddLayer) {
  OGRGeoJSONDataSource ds;
  auto* poLayer =
      new OGRGeoJSONLayer("test_layer", nullptr, wkbPoint, &ds, nullptr);
  ds.AddLayer(poLayer);

  EXPECT_EQ(ds.GetLayerCount(), 1);
  EXPECT_EQ(ds.GetLayer(0), poLayer);
  EXPECT_EQ(ds.Close(), CE_None);
}

TEST_F(OGRGeoJSONDataSourceTest, DealWithOgrSchemaOpenOption) {
  // 1. OGR_SCHEMA in update mode fails
  {
    OGRGeoJSONDataSource ds;
    const std::string dummyPath =
        std::string(kTestDir) + "/dummy_update.geojson";
    GDALOpenInfo oi(dummyPath.c_str(), GA_Update);
    const char* const apszOptions[] = {"OGR_SCHEMA=bad", nullptr};
    oi.papszOpenOptions = const_cast<char**>(apszOptions);
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.DealWithOgrSchemaOpenOption(&oi));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("not supported in update mode"));
  }

  // 2. OGR_SCHEMA with invalid JSON
  {
    OGRGeoJSONDataSource ds;
    const std::string dummyPath = std::string(kTestDir) + "/dummy_ro.geojson";
    GDALOpenInfo oi(dummyPath.c_str(), GA_ReadOnly);
    const char* const apszOptions[] = {"OGR_SCHEMA={invalid_json}", nullptr};
    oi.papszOpenOptions = const_cast<char**>(apszOptions);
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.DealWithOgrSchemaOpenOption(&oi));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("SCHEMA"));
  }

  // 3. Valid OGR_SCHEMA
  {
    const std::string file_path = std::string(kTestDir) + "/schema.geojson";
    const char kData[] =
        "{\"type\":\"FeatureCollection\",\"features\":["
        "{\"type\":\"Feature\",\"geometry\":null,\"properties\":{\"foo\":"
        "\"123\"}}"
        "]}";
    VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(kData, 1, strlen(kData), fp);
    VSIFCloseL(fp);

    const char* const apszOptions[] = {
        "OGR_SCHEMA={\"layers\":[{\"name\":\"schema\",\"fields\":[{\"name\":"
        "\"foo\",\"type\":\"Integer\"}]}]}",
        nullptr};
    GDALOpenInfo oi(file_path.c_str(), GA_ReadOnly);
    oi.papszOpenOptions = const_cast<char**>(apszOptions);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceFile, "GeoJSON"));
    ASSERT_EQ(ds.GetLayerCount(), 1);
    const OGRLayer* layer = ds.GetLayer(0);
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(layer->GetLayerDefn()->GetFieldDefn(0)->GetType(), OFTInteger);
  }
}

TEST_F(OGRGeoJSONDataSourceTest, OpenWithTextVariants) {
  // 1. Valid FeatureCollection text
  {
    const char kValid[] =
        "{\"type\":\"FeatureCollection\",\"features\":["
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[1.0,2.0]},"
        "\"properties\":{\"name\":\"item1\"}}"
        "]}";
    GDALOpenInfo oi(kValid, GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceText, "GeoJSON"));
    EXPECT_EQ(ds.GetLayerCount(), 1);
    OGRLayer* layer = const_cast<OGRLayer*>(ds.GetLayer(0));
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(layer->GetFeatureCount(TRUE), 1);
  }

  // 2. JSONP loadGeoJSON(...) wrapper
  {
    const char kJSONP[] =
        "loadGeoJSON({\"type\":\"FeatureCollection\",\"features\":[]})";
    GDALOpenInfo oi(kJSONP, GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceText, "GeoJSON"));
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }

  // 3. JSONP jsonp(...) wrapper
  {
    const char kJSONP[] =
        "jsonp({\"type\":\"FeatureCollection\",\"features\":[]})";
    GDALOpenInfo oi(kJSONP, GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceText, "GeoJSON"));
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }

  // 4. CouchDB rejection strings
  {
    const char* const kCouchRejections[] = {
        "{\"couchdb\":\"Welcome\"}",
        "{\"db_name\":\"mydb\"}",
        "{\"total_rows\":10}",
        "{\"rows\":[]}",
    };
    for (const char* pszText : kCouchRejections) {
      GDALOpenInfo oi(pszText, GA_ReadOnly);
      OGRGeoJSONDataSource ds;
      EXPECT_FALSE(ds.Open(&oi, eGeoJSONSourceText, "GeoJSON"));
    }
  }

  // 5. Update mode on inline text -> not supported
  {
    GDALOpenInfo oi("{\"type\":\"FeatureCollection\",\"features\":[]}",
                    GA_Update);
    OGRGeoJSONDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&oi, eGeoJSONSourceText, "GeoJSON"));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Update from inline definition not supported"));
  }

  // 6. Unknown source type
  {
    GDALOpenInfo oi("{\"type\":\"FeatureCollection\",\"features\":[]}",
                    GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_FALSE(ds.Open(&oi, eGeoJSONSourceUnknown, "GeoJSON"));
  }

  // 7. Invalid non-GeoJSON text
  {
    GDALOpenInfo oi("{\"not\":\"geojson\"}", GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&oi, eGeoJSONSourceText, "GeoJSON"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Failed to read GeoJSON data"));
  }
}

TEST_F(OGRGeoJSONDataSourceTest, OpenWithFileVariants) {
  // 1. Direct file
  const std::string file_path = std::string(kTestDir) + "/valid.geojson";
  const char kData[] =
      "{\"type\":\"FeatureCollection\",\"features\":["
      "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":["
      "10,20]},"
      "\"properties\":{\"id\":100}}"
      "]}";
  VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kData, 1, strlen(kData), fp);
  VSIFCloseL(fp);

  {
    GDALOpenInfo oi(file_path.c_str(), GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceFile, "GeoJSON"));
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }

  // 2. Prefixed file: "GeoJSON:/vsimem/..."
  {
    std::string prefixed = "GeoJSON:" + file_path;
    GDALOpenInfo oi(prefixed.c_str(), GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceFile, "GeoJSON"));
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }

  // 3. Update mode on GeoJSON file
  {
    GDALOpenInfo oiUpdate(file_path.c_str(), GA_Update);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oiUpdate, eGeoJSONSourceFile, "GeoJSON"));
    EXPECT_TRUE(ds.IsUpdatable());
  }

  // 4. Update mode on non-GeoJSON flavor -> rejected
  {
    GDALOpenInfo oiUpdate(file_path.c_str(), GA_Update);
    OGRGeoJSONDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&oiUpdate, eGeoJSONSourceFile, "TopoJSON"));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Update of TopoJSON not supported"));
  }

  // 5. ESRIJSON file
  const std::string esri_file = std::string(kTestDir) + "/test.esri.json";
  const char kESRIData[] =
      "{\"geometryType\":\"esriGeometryPoint\",\"spatialReference\":{\"wkid\":"
      "4326},"
      "\"fields\":[{\"name\":\"ID\",\"type\":\"esriFieldTypeOID\"}],"
      "\"features\":[{\"geometry\":{\"x\":1.0,\"y\":2.0},\"attributes\":{"
      "\"ID\":1}}]}";
  fp = VSIFOpenL(esri_file.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kESRIData, 1, strlen(kESRIData), fp);
  VSIFCloseL(fp);

  {
    GDALOpenInfo oi(esri_file.c_str(), GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceFile, "ESRIJSON"));
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }

  // 6. TopoJSON file
  const std::string topo_file = std::string(kTestDir) + "/test.topo.json";
  const char kTopoData[] =
      "{\"type\":\"Topology\",\"objects\":{\"layer1\":{\"type\":"
      "\"GeometryCollection\","
      "\"geometries\":[]}},\"arcs\":[]}";
  fp = VSIFOpenL(topo_file.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kTopoData, 1, strlen(kTopoData), fp);
  VSIFCloseL(fp);

  {
    GDALOpenInfo oi(topo_file.c_str(), GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceFile, "TOPOJSON"));
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }

  // 7. Streaming interface via OGR_GEOJSON_MAX_BYTES_FIRST_PASS
  {
    CPLSetConfigOption("OGR_GEOJSON_MAX_BYTES_FIRST_PASS", "1000");
    absl::Cleanup resetConfig = [] {
      CPLSetConfigOption("OGR_GEOJSON_MAX_BYTES_FIRST_PASS", nullptr);
    };

    GDALOpenInfo oi(file_path.c_str(), GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceFile, "GeoJSON"));
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }
}

static CPLHTTPResult* MockDataSourceFetchCallback(
    const char* pszURL, CSLConstList /*papszOptions*/,
    GDALProgressFunc /*pfnProgress*/, void* /*pProgressArg*/,
    CPLHTTPFetchWriteFunc /*pfnWrite*/, void* /*pWriteArg*/,
    void* /*pUserData*/) {
  auto* psResult =
      static_cast<CPLHTTPResult*>(CPLCalloc(1, sizeof(CPLHTTPResult)));

  if (strstr(pszURL, "service_esri")) {
    const char kData[] =
        "{\"geometryType\":\"esriGeometryPoint\",\"features\":[]}";
    psResult->nStatus = 0;
    psResult->pabyData = reinterpret_cast<GByte*>(CPLStrdup(kData));
    psResult->nDataLen = strlen(kData);
    return psResult;
  }

  if (strstr(pszURL, "service_topo")) {
    const char kData[] =
        "{\"type\":\"Topology\",\"objects\":{\"l\":{\"type\":"
        "\"GeometryCollection\",\"geometries\":[]}},\"arcs\":[]}";
    psResult->nStatus = 0;
    psResult->pabyData = reinterpret_cast<GByte*>(CPLStrdup(kData));
    psResult->nDataLen = strlen(kData);
    return psResult;
  }

  if (strstr(pszURL, "exceeded_root")) {
    const char kData[] =
        "{\"type\":\"FeatureCollection\",\"features\":[],"
        "\"exceededTransferLimit\":true}";
    psResult->nStatus = 0;
    psResult->pabyData = reinterpret_cast<GByte*>(CPLStrdup(kData));
    psResult->nDataLen = strlen(kData);
    return psResult;
  }

  if (strstr(pszURL, "exceeded_props")) {
    const char kData[] =
        "{\"type\":\"FeatureCollection\",\"features\":[],\"properties\":{"
        "\"exceededTransferLimit\":true}}";
    psResult->nStatus = 0;
    psResult->pabyData = reinterpret_cast<GByte*>(CPLStrdup(kData));
    psResult->nDataLen = strlen(kData);
    return psResult;
  }

  if (strstr(pszURL, "not_geojson")) {
    const char kData[] = "{\"invalid\":\"not_supported\"}";
    psResult->nStatus = 0;
    psResult->pabyData = reinterpret_cast<GByte*>(CPLStrdup(kData));
    psResult->nDataLen = strlen(kData);
    return psResult;
  }

  const char kData[] =
      "{\"type\":\"FeatureCollection\",\"features\":["
      "{\"type\":\"Feature\",\"geometry\":null,\"properties\":{\"val\":1}}"
      "]}";
  psResult->nStatus = 0;
  psResult->pabyData = reinterpret_cast<GByte*>(CPLStrdup(kData));
  psResult->nDataLen = strlen(kData);
  return psResult;
}

TEST_F(OGRGeoJSONDataSourceTest, OpenWithServiceVariants) {
  CPLHTTPPushFetchCallback(MockDataSourceFetchCallback, nullptr);
  absl::Cleanup popCallback = [] { CPLHTTPPopFetchCallback(); };

  // 1. Valid GeoJSON service
  {
    GDALOpenInfo oi("http://example.com/geojson", GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceService, "GeoJSON"));
    EXPECT_EQ(ds.GetLayerCount(), 1);
    EXPECT_FALSE(ds.HasOtherPages());
  }

  // 2. Service with exceededTransferLimit at root
  {
    GDALOpenInfo oi("http://example.com/exceeded_root", GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceService, "GeoJSON"));
    EXPECT_TRUE(ds.HasOtherPages());
  }

  // 3. Service with exceededTransferLimit inside properties
  {
    GDALOpenInfo oi("http://example.com/exceeded_props", GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceService, "GeoJSON"));
    EXPECT_TRUE(ds.HasOtherPages());
  }

  // 4. Update mode on remote service -> rejected
  {
    GDALOpenInfo oi("http://example.com/geojson", GA_Update);
    OGRGeoJSONDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&oi, eGeoJSONSourceService, "GeoJSON"));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Update from remote service not supported"));
  }

  // 5. Caching: service returns ESRIJSON when flavor is GeoJSON -> stored
  {
    GDALOpenInfo oi("http://example.com/service_esri", GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_FALSE(ds.Open(&oi, eGeoJSONSourceService, "GeoJSON"));

    // Now open with ESRIJSON flavor steals the stored content
    OGRGeoJSONDataSource dsESRI;
    EXPECT_TRUE(dsESRI.Open(&oi, eGeoJSONSourceService, "ESRIJSON"));
    EXPECT_EQ(dsESRI.GetLayerCount(), 1);
  }

  // 6. Caching: service returns TopoJSON when flavor is GeoJSON -> stored
  {
    GDALOpenInfo oi("http://example.com/service_topo", GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_FALSE(ds.Open(&oi, eGeoJSONSourceService, "GeoJSON"));

    OGRGeoJSONDataSource dsTopo;
    EXPECT_TRUE(dsTopo.Open(&oi, eGeoJSONSourceService, "TopoJSON"));
    EXPECT_EQ(dsTopo.GetLayerCount(), 1);
  }

  // 7. Caching: service returns non-json-like content
  {
    GDALOpenInfo oi("http://example.com/not_geojson", GA_ReadOnly);
    OGRGeoJSONDataSource ds;
    EXPECT_FALSE(ds.Open(&oi, eGeoJSONSourceService, "GeoJSON"));
    char* stolen =
        OGRGeoJSONDriverStealStoredContent("http://example.com/not_geojson");
    if (stolen) {
      CPLFree(stolen);
    }
  }
}

TEST_F(OGRGeoJSONDataSourceTest, OpenOptionsOnReader) {
  const std::string file_path = std::string(kTestDir) + "/options.geojson";
  const char kData[] =
      "{\"type\":\"FeatureCollection\",\"features\":["
      "{\"type\":\"Feature\",\"geometry\":null,"
      "\"properties\":{\"nested\":{\"field\":\"val\"},\"date\":\"2023-01-01\","
      "\"arr\":[1,2]}}"
      "]}";
  VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kData, 1, strlen(kData), fp);
  VSIFCloseL(fp);

  {
    const char* const apszOptions[] = {"FLATTEN_NESTED_ATTRIBUTES=YES",
                                       "NESTED_ATTRIBUTE_SEPARATOR=_",
                                       "NATIVE_DATA=YES",
                                       "ARRAY_AS_STRING=YES",
                                       "DATE_AS_STRING=YES",
                                       "FOREIGN_MEMBERS=ALL",
                                       nullptr};

    GDALOpenInfo oi(file_path.c_str(), GA_ReadOnly);
    oi.papszOpenOptions = const_cast<char**>(apszOptions);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceFile, "GeoJSON"));
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }

  // Test translations and FOREIGN_MEMBERS=NONE and STAC
  {
    const char* const apszOptions[] = {"FOREIGN_MEMBERS=NONE", nullptr};

    GDALOpenInfo oi(file_path.c_str(), GA_ReadOnly);
    oi.papszOpenOptions = const_cast<char**>(apszOptions);
    OGRGeoJSONDataSource ds;
    ds.SetGeometryTranslation(OGRGeoJSONDataSource::eGeometryAsCollection);
    ds.SetAttributesTranslation(OGRGeoJSONDataSource::eAttributesSkip);
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceFile, "GeoJSON"));
  }

  {
    const char* const apszOptions[] = {"FOREIGN_MEMBERS=STAC", nullptr};

    GDALOpenInfo oi(file_path.c_str(), GA_ReadOnly);
    oi.papszOpenOptions = const_cast<char**>(apszOptions);
    OGRGeoJSONDataSource ds;
    EXPECT_TRUE(ds.Open(&oi, eGeoJSONSourceFile, "GeoJSON"));
  }
}

TEST_F(OGRGeoJSONDataSourceTest, StandaloneFeatureReadFromFile) {
  const std::string file_path =
      std::string(kTestDir) + "/standalone_feat.geojson";
  const char kData[] =
      "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":["
      "1,2]},\"properties\":{}}";
  VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kData, 1, strlen(kData), fp);
  VSIFCloseL(fp);

  // Direct file
  GDALOpenInfo oiDirect(file_path.c_str(), GA_ReadOnly);
  OGRGeoJSONDataSource dsDirect;
  EXPECT_TRUE(dsDirect.Open(&oiDirect, eGeoJSONSourceFile, "GeoJSON"));
  EXPECT_EQ(dsDirect.GetLayerCount(), 1);

  // Prefixed file
  std::string prefixed = "GeoJSON:" + file_path;
  GDALOpenInfo oiPrefixed(prefixed.c_str(), GA_ReadOnly);
  OGRGeoJSONDataSource dsPrefixed;
  EXPECT_TRUE(dsPrefixed.Open(&oiPrefixed, eGeoJSONSourceFile, "GeoJSON"));
  EXPECT_EQ(dsPrefixed.GetLayerCount(), 1);
}

TEST_F(OGRGeoJSONDataSourceTest, ServiceEmptyLayersHiddenFile) {
  CPLHTTPPushFetchCallback(
      [](const char*, CSLConstList, GDALProgressFunc, void*,
         CPLHTTPFetchWriteFunc, void*, void*) -> CPLHTTPResult* {
        auto* res =
            static_cast<CPLHTTPResult*>(CPLCalloc(1, sizeof(CPLHTTPResult)));
        res->nStatus = 0;
        const char kDoc[] = "{\"something_else\":\"not_geojson\"}";
        res->pabyData = reinterpret_cast<GByte*>(CPLStrdup(kDoc));
        res->nDataLen = strlen(kDoc);
        return res;
      },
      nullptr);
  absl::Cleanup popCallback = [] { CPLHTTPPopFetchCallback(); };

  GDALOpenInfo oi("http://example.com/not_geojson_service", GA_ReadOnly);
  OGRGeoJSONDataSource ds;
  WithQuietHandler quiet;
  EXPECT_FALSE(ds.Open(&oi, eGeoJSONSourceService, "GeoJSON"));
  char* stolen = OGRGeoJSONDriverStealStoredContent(
      "http://example.com/not_geojson_service");
  if (stolen) {
    CPLFree(stolen);
  }
}

TEST_F(OGRGeoJSONDataSourceTest, CreateAndLayerCreationOptions) {
  // 1. Create on valid path
  const std::string out_path = std::string(kTestDir) + "/created.geojson";
  OGRGeoJSONDataSource ds;
  ASSERT_TRUE(ds.Create(out_path.c_str(), nullptr));
  EXPECT_NE(ds.GetOutputFile(), nullptr);
  EXPECT_TRUE(ds.GetFpOutputIsSeekable());
  EXPECT_TRUE(ds.TestCapability(ODsCCreateLayer));

  // Overwriting existing file fails
  {
    OGRGeoJSONDataSource dsFail;
    WithQuietHandler quiet;
    EXPECT_FALSE(dsFail.Create(out_path.c_str(), nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("does not overwrite existing files"));
  }

  // Invalid path fails (path is an existing directory)
  {
    const std::string dir_path = std::string(kTestDir) + "/is_a_dir.geojson";
    VSIMkdir(dir_path.c_str(), 0755);
    OGRGeoJSONDataSource dsFail;
    WithQuietHandler quiet;
    EXPECT_FALSE(dsFail.Create(dir_path.c_str(), nullptr));
  }

  // 2. ICreateLayer option validations
  // FOREIGN_MEMBERS_COLLECTION invalid braces
  {
    const char* const apszBad[] = {"FOREIGN_MEMBERS_COLLECTION=no_braces",
                                   nullptr};
    WithQuietHandler quiet;
    EXPECT_EQ(
        ds.CreateLayer("bad", nullptr, wkbPoint, const_cast<char**>(apszBad)),
        nullptr);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("start with { and end with }"));
  }

  // FOREIGN_MEMBERS_COLLECTION invalid JSON
  {
    const char* const apszBad[] = {"FOREIGN_MEMBERS_COLLECTION={bad_json}",
                                   nullptr};
    WithQuietHandler quiet;
    EXPECT_EQ(
        ds.CreateLayer("bad", nullptr, wkbPoint, const_cast<char**>(apszBad)),
        nullptr);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("invalid JSON"));
  }

  // FOREIGN_MEMBERS_FEATURE invalid braces
  {
    const char* const apszBad[] = {"FOREIGN_MEMBERS_FEATURE=no_braces",
                                   nullptr};
    WithQuietHandler quiet;
    EXPECT_EQ(
        ds.CreateLayer("bad", nullptr, wkbPoint, const_cast<char**>(apszBad)),
        nullptr);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("start with { and end with }"));
  }

  // FOREIGN_MEMBERS_FEATURE invalid JSON
  {
    const char* const apszBad[] = {"FOREIGN_MEMBERS_FEATURE={bad_json}",
                                   nullptr};
    WithQuietHandler quiet;
    EXPECT_EQ(
        ds.CreateLayer("bad", nullptr, wkbPoint, const_cast<char**>(apszBad)),
        nullptr);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("invalid JSON"));
  }

  // 3. Successful layer creation with all options
  OGRSpatialReference oSRS;
  oSRS.importFromEPSG(4326);
  oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRGeomFieldDefn oGFD("geom", wkbPoint);
  oGFD.SetSpatialRef(&oSRS);
  OGRGeomCoordinatePrecision oPrec;
  oPrec.dfXYResolution = 0.001;
  oPrec.dfZResolution = 0.01;
  oGFD.SetCoordinatePrecision(oPrec);

  const char* const apszLayerOptions[] = {
      "FOREIGN_MEMBERS_COLLECTION={\"coll_key\":\"coll_val\"}",
      "FOREIGN_MEMBERS_FEATURE={\"feat_key\":\"feat_val\"}",
      "WRITE_BBOX=YES",
      "RFC7946=YES",
      "NATIVE_DATA={\"type\":\"FeatureCollection\",\"custom\":\"native\","
      "\"name\":\"nat_name\",\"description\":\"nat_desc\"}",
      "NATIVE_MEDIA_TYPE=application/vnd.geo+json",
      "DESCRIPTION=layer description",
      "COORDINATE_PRECISION=4",
      nullptr};

  OGRLayer* layer =
      ds.CreateLayer("points", &oGFD, const_cast<char**>(apszLayerOptions));
  ASSERT_NE(layer, nullptr);
  EXPECT_EQ(ds.GetLayerCount(), 1);
  EXPECT_EQ(ds.GetLayer(0), layer);

  // Attempt to create second layer -> rejected
  {
    WithQuietHandler quiet;
    EXPECT_EQ(ds.CreateLayer("second_layer", nullptr, wkbPoint, nullptr),
              nullptr);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("more than one layer"));
  }

  // Add a feature and close
  OGRFeatureDefn* poFDefn = layer->GetLayerDefn();
  auto poFeat = std::make_unique<OGRFeature>(poFDefn);
  auto poPt = std::make_unique<OGRPoint>(1.234567, 2.345678);
  poFeat->SetGeometry(poPt.get());
  EXPECT_EQ(layer->CreateFeature(poFeat.get()), OGRERR_NONE);

  EXPECT_EQ(ds.FlushCache(false), CE_None);
  EXPECT_EQ(ds.Close(), CE_None);
}

TEST_F(OGRGeoJSONDataSourceTest, CreateLayerNonRFC7946And3D) {
  // Test non-RFC7946 with EPSG:3857 and 3D precision
  const std::string out_path = std::string(kTestDir) + "/out_3857.geojson";
  OGRGeoJSONDataSource ds;
  ASSERT_TRUE(ds.Create(out_path.c_str(), nullptr));

  OGRSpatialReference oSRS;
  oSRS.importFromEPSG(3857);

  OGRGeomFieldDefn oGFD("geom", wkbPoint25D);
  oGFD.SetSpatialRef(&oSRS);

  const char* const apszOptions[] = {"RFC7946=NO", "@NAME=CustomLayerName",
                                     "COORDINATE_PRECISION=2", nullptr};

  OGRLayer* layer =
      ds.CreateLayer("test3d", &oGFD, const_cast<char**>(apszOptions));
  ASSERT_NE(layer, nullptr);

  auto poFeat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  auto poPt = std::make_unique<OGRPoint>(100.0, 200.0, 300.0);
  poFeat->SetGeometry(poPt.get());
  EXPECT_EQ(layer->CreateFeature(poFeat.get()), OGRERR_NONE);

  EXPECT_EQ(ds.Close(), CE_None);
}

TEST_F(OGRGeoJSONDataSourceTest, CreateLayerRFC7946SRSVariations) {
  // 1. RFC7946 with null SRS -> warning
  {
    const std::string out_path =
        std::string(kTestDir) + "/rfc7946_null_srs.geojson";
    OGRGeoJSONDataSource ds;
    ASSERT_TRUE(ds.Create(out_path.c_str(), nullptr));
    const char* const apszOptions[] = {"RFC7946=YES", nullptr};
    WithQuietHandler quiet;
    OGRLayer* layer =
        ds.CreateLayer("nosrs", nullptr, const_cast<char**>(apszOptions));
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(ds.Close(), CE_None);
  }

  // 2. RFC7946 with 3D EPSG:4979
  {
    const std::string out_path = std::string(kTestDir) + "/rfc7946_3d.geojson";
    OGRGeoJSONDataSource ds;
    ASSERT_TRUE(ds.Create(out_path.c_str(), nullptr));

    OGRSpatialReference oSRS;
    oSRS.importFromEPSG(4979);
    OGRGeomFieldDefn oGFD("geom", wkbPoint25D);
    oGFD.SetSpatialRef(&oSRS);
    OGRGeomCoordinatePrecision oPrec;
    oPrec.dfXYResolution = 0.001;
    oPrec.dfZResolution = 0.01;
    oGFD.SetCoordinatePrecision(oPrec);

    const char* const apszOptions[] = {"RFC7946=YES", nullptr};
    OGRLayer* layer =
        ds.CreateLayer("p3d", &oGFD, const_cast<char**>(apszOptions));
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(ds.Close(), CE_None);
  }

  // 3. RFC7946 with 2D EPSG:3857 and coordinate precision
  {
    const std::string out_path =
        std::string(kTestDir) + "/rfc7946_3857.geojson";
    OGRGeoJSONDataSource ds;
    ASSERT_TRUE(ds.Create(out_path.c_str(), nullptr));

    OGRSpatialReference oSRS;
    oSRS.importFromEPSG(3857);
    OGRGeomFieldDefn oGFD("geom", wkbPoint);
    oGFD.SetSpatialRef(&oSRS);
    OGRGeomCoordinatePrecision oPrec;
    oPrec.dfXYResolution = 0.5;
    oGFD.SetCoordinatePrecision(oPrec);

    const char* const apszOptions[] = {"RFC7946=YES", nullptr};
    OGRLayer* layer =
        ds.CreateLayer("p3857", &oGFD, const_cast<char**>(apszOptions));
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(ds.Close(), CE_None);
  }

  // 4. COORDINATE_PRECISION=3 with 3D SRS
  {
    const std::string out_path =
        std::string(kTestDir) + "/coordprec_3d.geojson";
    OGRGeoJSONDataSource ds;
    ASSERT_TRUE(ds.Create(out_path.c_str(), nullptr));

    OGRSpatialReference oSRS;
    oSRS.importFromEPSG(4979);
    OGRGeomFieldDefn oGFD("geom", wkbPoint25D);
    oGFD.SetSpatialRef(&oSRS);

    const char* const apszOptions[] = {"COORDINATE_PRECISION=3", nullptr};
    OGRLayer* layer =
        ds.CreateLayer("prec3d", &oGFD, const_cast<char**>(apszOptions));
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(ds.Close(), CE_None);
  }

  // 5. NATIVE_DATA with bbox and crs
  {
    const std::string out_path =
        std::string(kTestDir) + "/native_bbox_crs.geojson";
    OGRGeoJSONDataSource ds;
    ASSERT_TRUE(ds.Create(out_path.c_str(), nullptr));

    const char* const apszOptions[] = {
        "NATIVE_DATA={\"type\":\"FeatureCollection\",\"bbox\":[0,0,1,1],"
        "\"crs\":{\"type\":\"name\",\"properties\":{\"name\":\"urn:ogc:def:crs:"
        "OGC:1.3:CRS84\"}}}",
        "NATIVE_MEDIA_TYPE=application/vnd.geo+json", "RFC7946=NO", nullptr};
    OGRLayer* layer =
        ds.CreateLayer("nat", nullptr, const_cast<char**>(apszOptions));
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(ds.Close(), CE_None);
  }
}

TEST_F(OGRGeoJSONDataSourceTest, CreateLayerOnReadOnlyFails) {
  OGRGeoJSONDataSource ds;
  WithQuietHandler quiet;
  EXPECT_EQ(ds.CreateLayer("fail", nullptr, wkbNone, nullptr), nullptr);
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("read-only datasource"));
}

TEST_F(OGRGeoJSONDataSourceTest, UpdateModeFlushCacheSingleFeatureNativeData) {
  const std::string file_path =
      std::string(kTestDir) + "/single_native.geojson";
  const char kData[] =
      "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":["
      "1,2]},"
      "\"properties\":{\"name\":\"orig\"}}";
  VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kData, 1, strlen(kData), fp);
  VSIFCloseL(fp);

  {
    GDALOpenInfo oi(file_path.c_str(), GA_Update);
    OGRGeoJSONDataSource ds;
    ASSERT_TRUE(ds.Open(&oi, eGeoJSONSourceFile, "GeoJSON"));
    ASSERT_EQ(ds.GetLayerCount(), 1);
    OGRLayer* layer = const_cast<OGRLayer*>(ds.GetLayer(0));
    ASSERT_NE(layer, nullptr);

    layer->ResetReading();
    std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
    ASSERT_NE(feat, nullptr);
    feat->SetField("name", "updated");
    EXPECT_EQ(layer->SetFeature(feat.get()), OGRERR_NONE);

    EXPECT_EQ(ds.FlushCache(false), CE_None);
    EXPECT_EQ(ds.Close(), CE_None);
  }
}

TEST_F(OGRGeoJSONDataSourceTest, UpdateModeFlushCacheMultipleFeaturesInPlace) {
  const std::string file_path =
      std::string(kTestDir) + "/multi_inplace.geojson";
  const char kData[] =
      "{\"type\":\"FeatureCollection\",\"features\":["
      "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":["
      "1,2]},\"properties\":{\"id\":1}},"
      "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":["
      "3,4]},\"properties\":{\"id\":2}}"
      "]}";
  VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kData, 1, strlen(kData), fp);
  VSIFCloseL(fp);

  // Set OGR_GEOJSON_REWRITE_IN_PLACE=YES
  CPLSetConfigOption("OGR_GEOJSON_REWRITE_IN_PLACE", "YES");
  absl::Cleanup resetConfig = [] {
    CPLSetConfigOption("OGR_GEOJSON_REWRITE_IN_PLACE", nullptr);
  };

  {
    GDALOpenInfo oi(file_path.c_str(), GA_Update);
    OGRGeoJSONDataSource ds;
    ASSERT_TRUE(ds.Open(&oi, eGeoJSONSourceFile, "GeoJSON"));
    ASSERT_EQ(ds.GetLayerCount(), 1);
    OGRLayer* layer = const_cast<OGRLayer*>(ds.GetLayer(0));
    ASSERT_NE(layer, nullptr);

    layer->ResetReading();
    std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
    ASSERT_NE(feat, nullptr);
    feat->SetField("id", 10);
    EXPECT_EQ(layer->SetFeature(feat.get()), OGRERR_NONE);

    EXPECT_EQ(ds.FlushCache(false), CE_None);
    EXPECT_EQ(ds.Close(), CE_None);
  }
}

TEST_F(OGRGeoJSONDataSourceTest,
       UpdateModeFlushCacheMultipleFeaturesBackupRename) {
  const std::string file_path = std::string(kTestDir) + "/multi_backup.geojson";
  const char kData[] =
      "{\"type\":\"FeatureCollection\",\"features\":["
      "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":["
      "1,2]},\"properties\":{\"id\":1}},"
      "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":["
      "3,4]},\"properties\":{\"id\":2}}"
      "]}";
  VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kData, 1, strlen(kData), fp);
  VSIFCloseL(fp);

  // Set OGR_GEOJSON_REWRITE_IN_PLACE=NO
  CPLSetConfigOption("OGR_GEOJSON_REWRITE_IN_PLACE", "NO");
  absl::Cleanup resetConfig = [] {
    CPLSetConfigOption("OGR_GEOJSON_REWRITE_IN_PLACE", nullptr);
  };

  {
    GDALOpenInfo oi(file_path.c_str(), GA_Update);
    OGRGeoJSONDataSource ds;
    ASSERT_TRUE(ds.Open(&oi, eGeoJSONSourceFile, "GeoJSON"));
    ASSERT_EQ(ds.GetLayerCount(), 1);
    OGRLayer* layer = const_cast<OGRLayer*>(ds.GetLayer(0));
    ASSERT_NE(layer, nullptr);

    layer->ResetReading();
    std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
    ASSERT_NE(feat, nullptr);
    feat->SetField("id", 20);
    EXPECT_EQ(layer->SetFeature(feat.get()), OGRERR_NONE);

    EXPECT_EQ(ds.FlushCache(false), CE_None);
    EXPECT_EQ(ds.Close(), CE_None);
  }
}

}  // namespace
}  // namespace autotest2
