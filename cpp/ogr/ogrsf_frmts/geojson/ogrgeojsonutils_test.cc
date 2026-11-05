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

// Tests for GeoJSON utilities.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/geojson/ogrgeojsonutils.cpp
// third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonutils.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonutils.h"

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

constexpr char kTestDir[] = "/vsimem/ogrgeojsonutils_test";

bool TestGeoJSONIsObject(const char* pszText) {
  if (!pszText) return false;
  GDALOpenInfo oi(pszText, GDAL_OF_VECTOR);
  return GeoJSONIsObject(pszText, &oi);
}

bool TestESRIJSONIsObject(const char* pszText) {
  if (!pszText) return false;
  GDALOpenInfo oi(pszText, GDAL_OF_VECTOR);
  return ESRIJSONIsObject(pszText, &oi);
}

bool TestTopoJSONIsObject(const char* pszText) {
  if (!pszText) return false;
  GDALOpenInfo oi(pszText, GDAL_OF_VECTOR);
  return TopoJSONIsObject(pszText, &oi);
}

bool TestGeoJSONSeqIsObject(const char* pszText) {
  if (!pszText) return false;
  GDALOpenInfo oi(pszText, GDAL_OF_VECTOR);
  return GeoJSONSeqIsObject(pszText, &oi);
}

bool TestJSONFGIsObject(const char* pszText) {
  if (!pszText) return false;
  GDALOpenInfo oi(pszText, GDAL_OF_VECTOR);
  return JSONFGIsObject(pszText, &oi);
}

class OGRGeoJSONUtilsTest : public ::testing::Test {
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

TEST_F(OGRGeoJSONUtilsTest, GeoJSONIsObjectVariants) {
  // 1. nullptr and empty
  EXPECT_FALSE(GeoJSONIsObject(nullptr, nullptr));
  EXPECT_FALSE(TestGeoJSONIsObject(""));

  // 2. UTF-8 BOM prefix
  const char kBOMGeoJSON[] =
      "\xEF\xBB\xBF{\"type\":\"FeatureCollection\",\"features\":[]}";
  EXPECT_TRUE(TestGeoJSONIsObject(kBOMGeoJSON));

  // 3. Leading whitespace
  EXPECT_TRUE(TestGeoJSONIsObject(
      "   \t\r\n{\"type\":\"FeatureCollection\",\"features\":[]}"));

  // 4. JSONP wrappers: loadGeoJSON(...) and jsonp(...)
  EXPECT_TRUE(TestGeoJSONIsObject(
      "loadGeoJSON({\"type\":\"FeatureCollection\",\"features\":[]})"));
  EXPECT_TRUE(TestGeoJSONIsObject(
      "jsonp({\"type\":\"FeatureCollection\",\"features\":[]})"));

  // 5. Non-object prefix
  EXPECT_FALSE(TestGeoJSONIsObject("[1, 2, 3]"));
  EXPECT_FALSE(TestGeoJSONIsObject("not json"));

  // 6. Valid FeatureCollection and Feature
  EXPECT_TRUE(
      TestGeoJSONIsObject("{\"type\":\"FeatureCollection\",\"features\":[]}"));
  EXPECT_TRUE(TestGeoJSONIsObject(
      "{\"type\":\"Feature\",\"geometry\":null,\"properties\":{}}"));

  // 7. Direct Geometry types
  EXPECT_TRUE(
      TestGeoJSONIsObject("{\"type\":\"Point\",\"coordinates\":[1.0, 2.0]}"));
  EXPECT_TRUE(TestGeoJSONIsObject(
      "{\"type\":\"LineString\",\"coordinates\":[[1,2],[3,4]]}"));
  EXPECT_TRUE(TestGeoJSONIsObject("{\"type\":\"Polygon\",\"coordinates\":[]}"));
  EXPECT_TRUE(
      TestGeoJSONIsObject("{\"type\":\"MultiPoint\",\"coordinates\":[]}"));
  EXPECT_TRUE(
      TestGeoJSONIsObject("{\"type\":\"MultiLineString\",\"coordinates\":[]}"));
  EXPECT_TRUE(
      TestGeoJSONIsObject("{\"type\":\"MultiPolygon\",\"coordinates\":[]}"));
  EXPECT_TRUE(TestGeoJSONIsObject(
      "{\"type\":\"GeometryCollection\",\"geometries\":[]}"));

  // 8. Without type but starting with {"features":[
  EXPECT_TRUE(TestGeoJSONIsObject("{\"features\":[{\"type\":\"Feature\"}]}"));

  // 9. Without type but starting with {"coordinates":[
  EXPECT_TRUE(TestGeoJSONIsObject("{\"coordinates\":[1.0, 2.0]}"));
  EXPECT_TRUE(
      TestGeoJSONIsObject("{\"geometry\":{\"coordinates\":[1.0, 2.0]}}"));
  EXPECT_TRUE(TestGeoJSONIsObject(
      "{\"geometry\":{\"type\":\"Point\",\"coordinates\":[1.0, 2.0]}}"));
  EXPECT_TRUE(TestGeoJSONIsObject(
      "{\"geometry\":{\"type\":\"LineString\",\"coordinates\":[]}}"));
  EXPECT_TRUE(TestGeoJSONIsObject(
      "{\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[]}}"));
  EXPECT_TRUE(TestGeoJSONIsObject(
      "{\"geometry\":{\"type\":\"MultiPoint\",\"coordinates\":[]}}"));
  EXPECT_TRUE(TestGeoJSONIsObject(
      "{\"geometry\":{\"type\":\"MultiLineString\",\"coordinates\":[]}}"));
  EXPECT_TRUE(TestGeoJSONIsObject(
      "{\"geometry\":{\"type\":\"MultiPolygon\",\"coordinates\":[]}}"));
  EXPECT_TRUE(TestGeoJSONIsObject(
      "{\"geometry\":{\"type\":\"GeometryCollection\",\"geometries\":[]}}"));

  // 10. Topology -> false
  EXPECT_FALSE(TestGeoJSONIsObject(
      "{\"type\":\"Topology\",\"objects\":{},\"arcs\":[]}"));

  // 10b. Properties starting object (might be sequence, read more bytes)
  EXPECT_FALSE(TestGeoJSONIsObject("{\"properties\":{\"a\":1}}"));

  // 10c. Escaped characters inside string literal
  EXPECT_FALSE(TestGeoJSONIsObject(
      "{\"foo\":\"escaped\\\"quote\\\\and\\\\backslash\"}"));

  // 11. Single allowed driver
  GDALOpenInfo oiAllowed("{\"type\":\"UnknownCustom\"}", GDAL_OF_VECTOR);
  const char* const apszAllowed[] = {"GeoJSON", nullptr};
  oiAllowed.papszAllowedDrivers = apszAllowed;
  EXPECT_TRUE(GeoJSONIsObject("{\"type\":\"UnknownCustom\"}", &oiAllowed));
}

TEST_F(OGRGeoJSONUtilsTest, ESRIJSONIsObjectVariants) {
  EXPECT_FALSE(ESRIJSONIsObject(nullptr, nullptr));
  EXPECT_FALSE(TestESRIJSONIsObject(""));

  // 1. ESRI features with rings (polygon)
  const char kESRIRings[] =
      "{\"features\":[{\"geometry\":{\"rings\":[[[0,0],[1,1],[1,0],[0,0]]]}}]}";
  EXPECT_TRUE(TestESRIJSONIsObject(kESRIRings));

  // 2. ESRI features with attributes
  const char kESRIAttributes[] =
      "{\"features\":[{\"attributes\":{\"OBJECTID\":1,\"NAME\":\"test\"}}]}";
  EXPECT_TRUE(TestESRIJSONIsObject(kESRIAttributes));

  // 3. ESRI root fields: geometryType, fields, fieldAliases, spatialReference
  EXPECT_TRUE(TestESRIJSONIsObject(
      "{\"geometryType\":\"esriGeometryPoint\",\"features\":[]}"));
  EXPECT_TRUE(TestESRIJSONIsObject(
      "{\"fields\":[{\"name\":\"id\",\"type\":\"esriFieldTypeOID\"}]}"));
  EXPECT_TRUE(TestESRIJSONIsObject("{\"fieldAliases\":{\"id\":\"ID\"}}"));
  EXPECT_TRUE(TestESRIJSONIsObject("{\"spatialReference\":{\"wkid\":4326}}"));

  // 4. Non-ESRI
  EXPECT_FALSE(
      TestESRIJSONIsObject("{\"type\":\"FeatureCollection\",\"features\":[]}"));
  EXPECT_FALSE(TestESRIJSONIsObject("{\"foo\":\"bar\"}"));

  // 5. Allowed driver
  GDALOpenInfo oiAllowed("{\"foo\":\"bar\"}", GDAL_OF_VECTOR);
  const char* const apszAllowed[] = {"ESRIJSON", nullptr};
  oiAllowed.papszAllowedDrivers = apszAllowed;
  EXPECT_TRUE(ESRIJSONIsObject("{\"foo\":\"bar\"}", &oiAllowed));

  // 6. UTF-8 BOM
  EXPECT_TRUE(TestESRIJSONIsObject(
      "\xEF\xBB\xBF{\"geometryType\":\"esriGeometryPoint\",\"features\":[]}"));
}

TEST_F(OGRGeoJSONUtilsTest, TopoJSONIsObjectVariants) {
  EXPECT_FALSE(TopoJSONIsObject(nullptr, nullptr));
  EXPECT_FALSE(TestTopoJSONIsObject(""));

  // 1. Valid TopoJSON topology
  EXPECT_TRUE(TestTopoJSONIsObject(
      "{\"type\":\"Topology\",\"objects\":{},\"arcs\":[]}"));

  // 2. Non-TopoJSON
  EXPECT_FALSE(
      TestTopoJSONIsObject("{\"type\":\"FeatureCollection\",\"features\":[]}"));
  EXPECT_FALSE(
      TestTopoJSONIsObject("{\"type\":\"Point\",\"coordinates\":[0,0]}"));

  // 3. Allowed driver
  GDALOpenInfo oiAllowed("{\"type\":\"Other\"}", GDAL_OF_VECTOR);
  const char* const apszAllowed[] = {"TOPOJSON", nullptr};
  oiAllowed.papszAllowedDrivers = apszAllowed;
  EXPECT_TRUE(TopoJSONIsObject("{\"type\":\"Other\"}", &oiAllowed));
}

TEST_F(OGRGeoJSONUtilsTest, GeoJSONSeqIsObjectVariants) {
  EXPECT_FALSE(TestGeoJSONSeqIsObject(""));

  // 1. RS-delimited (\x1e) record
  EXPECT_TRUE(
      TestGeoJSONSeqIsObject("\x1e{\"type\":\"Feature\",\"geometry\":null}\n"));

  // 2. Newline-delimited sequence of features
  const char kTwoFeatures[] =
      "{\"type\":\"Feature\",\"properties\":{}}\n"
      "{\"type\":\"Feature\",\"properties\":{}}\n";
  EXPECT_TRUE(TestGeoJSONSeqIsObject(kTwoFeatures));

  // 3. Single standalone feature without newline sequence -> false
  EXPECT_FALSE(
      TestGeoJSONSeqIsObject("{\"type\":\"Feature\",\"properties\":{}}"));

  // 4. FeatureCollection -> false for GeoJSONSeq
  EXPECT_FALSE(TestGeoJSONSeqIsObject(
      "{\"type\":\"FeatureCollection\",\"features\":[]}"));

  // 5. Single allowed driver
  GDALOpenInfo oiAllowed(kTwoFeatures, GDAL_OF_VECTOR);
  const char* const apszAllowed[] = {"GeoJSONSeq", nullptr};
  oiAllowed.papszAllowedDrivers = apszAllowed;
  EXPECT_TRUE(GeoJSONSeqIsObject(kTwoFeatures, &oiAllowed));
}

TEST_F(OGRGeoJSONUtilsTest, JSONFGIsObjectVariants) {
  EXPECT_FALSE(JSONFGIsObject(nullptr, nullptr));
  EXPECT_FALSE(TestJSONFGIsObject(""));

  // 1. coordRefSys at root level
  EXPECT_TRUE(TestJSONFGIsObject(
      "{\"coordRefSys\":\"[CRS84]\",\"type\":\"FeatureCollection\"}"));

  // 2. featureType at root level
  EXPECT_TRUE(TestJSONFGIsObject(
      "{\"featureType\":\"building\",\"type\":\"FeatureCollection\"}"));

  // 3. coordRefSys or featureType at Feature level inside features array
  const char kFeatureLevelJSONFG[] =
      "{\"type\":\"FeatureCollection\",\"features\":["
      "{\"type\":\"Feature\",\"featureType\":\"road\",\"geometry\":null}"
      "]}";
  EXPECT_TRUE(TestJSONFGIsObject(kFeatureLevelJSONFG));

  const char kFeatureLevelCRS[] =
      "{\"type\":\"FeatureCollection\",\"features\":["
      "{\"type\":\"Feature\",\"coordRefSys\":\"[CRS84]\",\"geometry\":null}"
      "]}";
  EXPECT_TRUE(TestJSONFGIsObject(kFeatureLevelCRS));

  // 4. Regular GeoJSON without JSONFG keys -> false
  EXPECT_FALSE(
      TestJSONFGIsObject("{\"type\":\"FeatureCollection\",\"features\":[]}"));

  // 5. Allowed driver
  GDALDriver* poJSONFG = new GDALDriver();
  poJSONFG->SetDescription("JSONFG");
  GetGDALDriverManager()->RegisterDriver(poJSONFG);
  absl::Cleanup deregister = [poJSONFG] {
    GetGDALDriverManager()->DeregisterDriver(poJSONFG);
    delete poJSONFG;
  };

  GDALOpenInfo oiAllowed("{\"type\":\"FeatureCollection\"}", GDAL_OF_VECTOR);
  const char* const apszAllowed[] = {"JSONFG", nullptr};
  oiAllowed.papszAllowedDrivers = apszAllowed;
  EXPECT_TRUE(JSONFGIsObject("{\"type\":\"FeatureCollection\"}", &oiAllowed));

  // Also verify GeoJSONIsObject rejects JSONFG when JSONFG driver is registered
  const char* const kJSONFGDoc =
      "{\"featureType\":\"building\",\"type\":\"FeatureCollection\"}";
  GDALOpenInfo oiGeoJSON(kJSONFGDoc, GDAL_OF_VECTOR);
  EXPECT_FALSE(GeoJSONIsObject(kJSONFGDoc, &oiGeoJSON));

  // 6. conformsTo variants
  EXPECT_TRUE(TestJSONFGIsObject("{\"conformsTo\":[\"[ogc-json-fg-1-\"]}"));
  EXPECT_TRUE(TestJSONFGIsObject(
      "{\"conformsTo\":[\"http://www.opengis.net/spec/json-fg-1/\"]}"));
  EXPECT_TRUE(
      TestJSONFGIsObject("{\"conformsTo\":[\"http:\\/\\/www.opengis.net\\/"
                         "spec\\/json-fg-1\\/\"]}"));

  // 7. place, time, and curve geometry types
  EXPECT_TRUE(TestJSONFGIsObject(
      "{\"place\":{\"type\":\"Point\",\"coordinates\":[0,0]}}"));
  EXPECT_TRUE(TestJSONFGIsObject("{\"place\":{\"coordinates\":[0,0]}}"));
  EXPECT_TRUE(TestJSONFGIsObject("{\"time\":{\"date\":\"2023-01-01\"}}"));
  EXPECT_TRUE(TestJSONFGIsObject(
      "{\"time\":{\"timestamp\":\"2023-01-01T00:00:00Z\"}}"));
  EXPECT_TRUE(TestJSONFGIsObject(
      "{\"time\":{\"interval\":[\"2023-01-01\",\"2023-01-02\"]}}"));
  EXPECT_TRUE(TestJSONFGIsObject("{\"type\":\"CircularString\"}"));
  EXPECT_TRUE(TestJSONFGIsObject("{\"type\":\"CompoundCurve\"}"));
  EXPECT_TRUE(TestJSONFGIsObject("{\"type\":\"CurvePolygon\"}"));
  EXPECT_TRUE(TestJSONFGIsObject("{\"type\":\"MultiCurve\"}"));
  EXPECT_TRUE(TestJSONFGIsObject("{\"type\":\"MultiSurface\"}"));

  // 8. EndObject and EndArray in parser
  EXPECT_TRUE(TestJSONFGIsObject(
      "{\"dict\":{\"inner\":1},\"arr\":[1,2],\"featureType\":\"building\"}"));
}

TEST_F(OGRGeoJSONUtilsTest, SourceTypeDetectors) {
  // 1. GeoJSONGetSourceType
  {
    // GEOJSON: prefix with service URL
    GDALOpenInfo oiService1("GEOJSON:http://example.com/test", GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONGetSourceType(&oiService1), eGeoJSONSourceService);

    // Bare HTTP service with single allowed driver
    GDALOpenInfo oiService2("http://example.com/test", GDAL_OF_VECTOR);
    const char* const apszGeoJSON[] = {"GeoJSON", nullptr};
    oiService2.papszAllowedDrivers = apszGeoJSON;
    EXPECT_EQ(GeoJSONGetSourceType(&oiService2), eGeoJSONSourceService);

    // WFS service without json -> unknown
    GDALOpenInfo oiWFS("http://example.com/wfs?SERVICE=WFS", GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONGetSourceType(&oiWFS), eGeoJSONSourceUnknown);

    // Likely ESRIJSON URL (f=json) -> unknown for GeoJSON
    GDALOpenInfo oiEsriUrl("http://example.com/arcgis/rest?f=json",
                           GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONGetSourceType(&oiEsriUrl), eGeoJSONSourceUnknown);

    // GeoJSON: prefix with existing file
    const std::string file_path = std::string(kTestDir) + "/source.geojson";
    const char kData[] = "{\"type\":\"FeatureCollection\",\"features\":[]}";
    VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(kData, 1, strlen(kData), fp);
    VSIFCloseL(fp);

    std::string prefixed_file = "GeoJSON:" + file_path;
    GDALOpenInfo oiPrefixedFile(prefixed_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONGetSourceType(&oiPrefixedFile), eGeoJSONSourceFile);

    // GeoJSON: prefix with inline text
    std::string prefixed_text = std::string("GeoJSON:") + kData;
    GDALOpenInfo oiPrefixedText(prefixed_text.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONGetSourceType(&oiPrefixedText), eGeoJSONSourceText);

    // Direct inline text
    GDALOpenInfo oiDirectText(kData, GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONGetSourceType(&oiDirectText), eGeoJSONSourceText);

    // Direct file
    GDALOpenInfo oiDirectFile(file_path.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONGetSourceType(&oiDirectFile), eGeoJSONSourceFile);

    // URL variants
    GDALOpenInfo oiEsriPjson("http://example.com/arcgis?f=pjson",
                             GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONGetSourceType(&oiEsriPjson), eGeoJSONSourceUnknown);

    GDALOpenInfo oiEsriCount("http://example.com/arcgis?resultRecordCount=10",
                             GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONGetSourceType(&oiEsriCount), eGeoJSONSourceUnknown);

    GDALOpenInfo oiOapif("http://example.com/items?f=json", GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONGetSourceType(&oiOapif), eGeoJSONSourceService);

    GDALOpenInfo oiPrefixedInvalid("GeoJSON:invalid_non_existent",
                                   GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONGetSourceType(&oiPrefixedInvalid), eGeoJSONSourceUnknown);

    // Two-line feature sequence file (rejected as standalone GeoJSON)
    const std::string twolines_file =
        std::string(kTestDir) + "/twolines.geojsonl";
    const char kTwoLines[] =
        "{\"type\":\"Feature\",\"geometry\":null}\n{\"type\":\"Feature\","
        "\"geometry\":null}\n";
    VSILFILE* fpSeq = VSIFOpenL(twolines_file.c_str(), "wb");
    ASSERT_NE(fpSeq, nullptr);
    VSIFWriteL(kTwoLines, 1, strlen(kTwoLines), fpSeq);
    VSIFCloseL(fpSeq);

    GDALOpenInfo oiTwoLines(twolines_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONGetSourceType(&oiTwoLines), eGeoJSONSourceUnknown);
  }

  // Common empty file
  const std::string empty_file = std::string(kTestDir) + "/empty.json";
  VSILFILE* fpEmpty = VSIFOpenL(empty_file.c_str(), "wb");
  ASSERT_NE(fpEmpty, nullptr);
  VSIFCloseL(fpEmpty);

  // 2. ESRIJSONDriverGetSourceType
  {
    GDALOpenInfo oiESRI1("ESRIJSON:http://example.com/arcgis", GDAL_OF_VECTOR);
    EXPECT_EQ(ESRIJSONDriverGetSourceType(&oiESRI1), eGeoJSONSourceService);

    GDALOpenInfo oiESRI2("http://example.com/arcgis/rest?f=json",
                         GDAL_OF_VECTOR);
    EXPECT_EQ(ESRIJSONDriverGetSourceType(&oiESRI2), eGeoJSONSourceService);

    const char* const apszESRI[] = {"ESRIJSON", nullptr};
    GDALOpenInfo oiAllowed("http://example.com/data", GDAL_OF_VECTOR);
    oiAllowed.papszAllowedDrivers = apszESRI;
    EXPECT_EQ(ESRIJSONDriverGetSourceType(&oiAllowed), eGeoJSONSourceService);

    GDALOpenInfo oiNotEsriUrl("http://example.com/data", GDAL_OF_VECTOR);
    EXPECT_EQ(ESRIJSONDriverGetSourceType(&oiNotEsriUrl),
              eGeoJSONSourceUnknown);

    GDALOpenInfo oiPrefixedInvalid("ESRIJSON:invalid_non_existent",
                                   GDAL_OF_VECTOR);
    EXPECT_EQ(ESRIJSONDriverGetSourceType(&oiPrefixedInvalid),
              eGeoJSONSourceUnknown);

    GDALOpenInfo oiNotEsriText("{\"foo\":\"bar\"}", GDAL_OF_VECTOR);
    EXPECT_EQ(ESRIJSONDriverGetSourceType(&oiNotEsriText),
              eGeoJSONSourceUnknown);

    GDALOpenInfo oiEmptyFile(empty_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(ESRIJSONDriverGetSourceType(&oiEmptyFile), eGeoJSONSourceUnknown);

    const std::string file_path = std::string(kTestDir) + "/esri.json";
    const char kData[] =
        "{\"geometryType\":\"esriGeometryPoint\",\"features\":[]}";
    VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(kData, 1, strlen(kData), fp);
    VSIFCloseL(fp);

    std::string prefixed_file = "ESRIJSON:" + file_path;
    GDALOpenInfo oiPrefixedFile(prefixed_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(ESRIJSONDriverGetSourceType(&oiPrefixedFile), eGeoJSONSourceFile);

    std::string prefixed_text = std::string("ESRIJSON:") + kData;
    GDALOpenInfo oiPrefixedText(prefixed_text.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(ESRIJSONDriverGetSourceType(&oiPrefixedText), eGeoJSONSourceText);

    GDALOpenInfo oiDirectText(kData, GDAL_OF_VECTOR);
    EXPECT_EQ(ESRIJSONDriverGetSourceType(&oiDirectText), eGeoJSONSourceText);

    GDALOpenInfo oiDirectFile(file_path.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(ESRIJSONDriverGetSourceType(&oiDirectFile), eGeoJSONSourceFile);
  }

  // 3. TopoJSONDriverGetSourceType
  {
    GDALOpenInfo oiTopo1("TopoJSON:http://example.com/topo", GDAL_OF_VECTOR);
    EXPECT_EQ(TopoJSONDriverGetSourceType(&oiTopo1), eGeoJSONSourceService);

    const char* const apszTopo[] = {"TOPOJSON", nullptr};
    GDALOpenInfo oiAllowed("http://example.com/data", GDAL_OF_VECTOR);
    oiAllowed.papszAllowedDrivers = apszTopo;
    EXPECT_EQ(TopoJSONDriverGetSourceType(&oiAllowed), eGeoJSONSourceService);

    GDALOpenInfo oiEsriUrl("http://example.com/arcgis?f=json", GDAL_OF_VECTOR);
    EXPECT_EQ(TopoJSONDriverGetSourceType(&oiEsriUrl), eGeoJSONSourceUnknown);

    GDALOpenInfo oiTopoUrl("http://example.com/other", GDAL_OF_VECTOR);
    EXPECT_EQ(TopoJSONDriverGetSourceType(&oiTopoUrl), eGeoJSONSourceService);

    GDALOpenInfo oiPrefixedInvalid("TopoJSON:invalid_non_existent",
                                   GDAL_OF_VECTOR);
    EXPECT_EQ(TopoJSONDriverGetSourceType(&oiPrefixedInvalid),
              eGeoJSONSourceUnknown);

    GDALOpenInfo oiNotTopoText("{\"foo\":\"bar\"}", GDAL_OF_VECTOR);
    EXPECT_EQ(TopoJSONDriverGetSourceType(&oiNotTopoText),
              eGeoJSONSourceUnknown);

    GDALOpenInfo oiEmptyFile(empty_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(TopoJSONDriverGetSourceType(&oiEmptyFile), eGeoJSONSourceUnknown);

    const std::string non_topo_file = std::string(kTestDir) + "/not_topo.json";
    VSILFILE* fpNotTopo = VSIFOpenL(non_topo_file.c_str(), "wb");
    ASSERT_NE(fpNotTopo, nullptr);
    VSIFWriteL("{\"foo\":\"bar\"}", 1, 13, fpNotTopo);
    VSIFCloseL(fpNotTopo);
    GDALOpenInfo oiNotTopoFile(non_topo_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(TopoJSONDriverGetSourceType(&oiNotTopoFile),
              eGeoJSONSourceUnknown);

    const std::string file_path = std::string(kTestDir) + "/topo.json";
    const char kData[] = "{\"type\":\"Topology\",\"objects\":{},\"arcs\":[]}";
    VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(kData, 1, strlen(kData), fp);
    VSIFCloseL(fp);

    std::string prefixed_file = "TopoJSON:" + file_path;
    GDALOpenInfo oiPrefixedFile(prefixed_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(TopoJSONDriverGetSourceType(&oiPrefixedFile), eGeoJSONSourceFile);

    std::string prefixed_text = std::string("TopoJSON:") + kData;
    GDALOpenInfo oiPrefixedText(prefixed_text.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(TopoJSONDriverGetSourceType(&oiPrefixedText), eGeoJSONSourceText);

    GDALOpenInfo oiDirectText(kData, GDAL_OF_VECTOR);
    EXPECT_EQ(TopoJSONDriverGetSourceType(&oiDirectText), eGeoJSONSourceText);

    GDALOpenInfo oiDirectFile(file_path.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(TopoJSONDriverGetSourceType(&oiDirectFile), eGeoJSONSourceFile);
  }

  // 4. GeoJSONSeqGetSourceType
  {
    GDALOpenInfo oiSeq1("GEOJSONSeq:http://example.com/seq", GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONSeqGetSourceType(&oiSeq1), eGeoJSONSourceService);

    const char* const apszSeq[] = {"GeoJSONSeq", nullptr};
    GDALOpenInfo oiAllowed("http://example.com/data", GDAL_OF_VECTOR);
    oiAllowed.papszAllowedDrivers = apszSeq;
    EXPECT_EQ(GeoJSONSeqGetSourceType(&oiAllowed), eGeoJSONSourceService);

    GDALOpenInfo oiEsri("http://example.com/arcgis?f=json", GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONSeqGetSourceType(&oiEsri), eGeoJSONSourceUnknown);

    GDALOpenInfo oiUrl("http://example.com/data", GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONSeqGetSourceType(&oiUrl), eGeoJSONSourceService);

    GDALOpenInfo oiPrefixedInvalid("GEOJSONSeq:invalid_non_existent",
                                   GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONSeqGetSourceType(&oiPrefixedInvalid),
              eGeoJSONSourceUnknown);

    const std::string twolines_file =
        std::string(kTestDir) + "/twolines.geojsonl";
    GDALOpenInfo oiTwoLinesSeq(twolines_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONSeqGetSourceType(&oiTwoLinesSeq), eGeoJSONSourceFile);

    GDALOpenInfo oiAllowedFile(twolines_file.c_str(), GDAL_OF_VECTOR);
    oiAllowedFile.papszAllowedDrivers = apszSeq;
    EXPECT_EQ(GeoJSONSeqGetSourceType(&oiAllowedFile), eGeoJSONSourceFile);

    const std::string file_path = std::string(kTestDir) + "/seq.geojsonl";
    const char kData[] = "\x1e{\"type\":\"Feature\",\"geometry\":null}\n";
    VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(kData, 1, strlen(kData), fp);
    VSIFCloseL(fp);

    std::string prefixed_file = "GEOJSONSeq:" + file_path;
    GDALOpenInfo oiPrefixedFile(prefixed_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONSeqGetSourceType(&oiPrefixedFile), eGeoJSONSourceFile);

    std::string prefixed_text = std::string("GEOJSONSeq:") + kData;
    GDALOpenInfo oiPrefixedText(prefixed_text.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONSeqGetSourceType(&oiPrefixedText), eGeoJSONSourceText);

    GDALOpenInfo oiDirectText(kData, GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONSeqGetSourceType(&oiDirectText), eGeoJSONSourceText);

    GDALOpenInfo oiDirectFile(file_path.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(GeoJSONSeqGetSourceType(&oiDirectFile), eGeoJSONSourceFile);
  }

  // 5. JSONFGDriverGetSourceType
  {
    GDALOpenInfo oiJSONFG1("JSONFG:http://example.com/fg", GDAL_OF_VECTOR);
    EXPECT_EQ(JSONFGDriverGetSourceType(&oiJSONFG1), eGeoJSONSourceService);

    const char* const apszJSONFG[] = {"JSONFG", nullptr};
    GDALOpenInfo oiAllowed("http://example.com/data", GDAL_OF_VECTOR);
    oiAllowed.papszAllowedDrivers = apszJSONFG;
    EXPECT_EQ(JSONFGDriverGetSourceType(&oiAllowed), eGeoJSONSourceService);

    GDALOpenInfo oiEsri("http://example.com/arcgis?f=json", GDAL_OF_VECTOR);
    EXPECT_EQ(JSONFGDriverGetSourceType(&oiEsri), eGeoJSONSourceUnknown);

    GDALOpenInfo oiUrl("http://example.com/data", GDAL_OF_VECTOR);
    EXPECT_EQ(JSONFGDriverGetSourceType(&oiUrl), eGeoJSONSourceService);

    GDALOpenInfo oiPrefixedInvalid("JSONFG:invalid_non_existent",
                                   GDAL_OF_VECTOR);
    EXPECT_EQ(JSONFGDriverGetSourceType(&oiPrefixedInvalid),
              eGeoJSONSourceUnknown);

    GDALOpenInfo oiEmptyFile(empty_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(JSONFGDriverGetSourceType(&oiEmptyFile), eGeoJSONSourceUnknown);

    const std::string file_path = std::string(kTestDir) + "/fg.json";
    const char kData[] =
        "{\"coordRefSys\":\"[CRS84]\",\"type\":\"FeatureCollection\"}";
    VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(kData, 1, strlen(kData), fp);
    VSIFCloseL(fp);

    std::string prefixed_file = "JSONFG:" + file_path;
    GDALOpenInfo oiPrefixedFile(prefixed_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(JSONFGDriverGetSourceType(&oiPrefixedFile), eGeoJSONSourceFile);

    std::string prefixed_text = std::string("JSONFG:") + kData;
    GDALOpenInfo oiPrefixedText(prefixed_text.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(JSONFGDriverGetSourceType(&oiPrefixedText), eGeoJSONSourceText);

    GDALOpenInfo oiDirectText(kData, GDAL_OF_VECTOR);
    EXPECT_EQ(JSONFGDriverGetSourceType(&oiDirectText), eGeoJSONSourceText);

    GDALOpenInfo oiDirectFile(file_path.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(JSONFGDriverGetSourceType(&oiDirectFile), eGeoJSONSourceFile);
  }
}

TEST_F(OGRGeoJSONUtilsTest, GeoJSONStringPropertyToFieldType) {
  int nTZFlag = 0;

  // 1. nullptr -> OFTString
  EXPECT_EQ(GeoJSONStringPropertyToFieldType(nullptr, nTZFlag), OFTString);

  // 2. Plain string
  json_object* poStr = json_object_new_string("plain text");
  EXPECT_EQ(GeoJSONStringPropertyToFieldType(poStr, nTZFlag), OFTString);
  json_object_put(poStr);

  // 3. Date string ("2023-05-18" or "2023/05/18")
  json_object* poDate = json_object_new_string("2023-05-18");
  EXPECT_EQ(GeoJSONStringPropertyToFieldType(poDate, nTZFlag), OFTDate);
  json_object_put(poDate);

  // 4. DateTime string ("2023-05-18T14:30:00Z")
  json_object* poDateTime = json_object_new_string("2023-05-18T14:30:00Z");
  EXPECT_EQ(GeoJSONStringPropertyToFieldType(poDateTime, nTZFlag), OFTDateTime);
  json_object_put(poDateTime);

  // 5. Time string ("14:30:00")
  json_object* poTime = json_object_new_string("14:30:00");
  EXPECT_EQ(GeoJSONStringPropertyToFieldType(poTime, nTZFlag), OFTTime);
  json_object_put(poTime);
}

static CPLHTTPResult* MockFetchCallback(const char* pszURL,
                                        CSLConstList papszOptions,
                                        GDALProgressFunc /*pfnProgress*/,
                                        void* /*pProgressArg*/,
                                        CPLHTTPFetchWriteFunc /*pfnWrite*/,
                                        void* /*pWriteArg*/,
                                        void* /*pUserData*/) {
  auto* psResult =
      static_cast<CPLHTTPResult*>(CPLCalloc(1, sizeof(CPLHTTPResult)));

  if (strstr(pszURL, "fail_http")) {
    psResult->nStatus = 500;
    psResult->pszErrBuf = CPLStrdup("Internal Server Error");
    psResult->pabyData = reinterpret_cast<GByte*>(CPLStrdup("error body"));
    psResult->nDataLen = strlen("error body");
    return psResult;
  }

  if (strstr(pszURL, "empty_http")) {
    psResult->nStatus = 0;
    psResult->nDataLen = 0;
    return psResult;
  }

  psResult->nStatus = 0;
  const char* pszHeaders = CSLFetchNameValue(papszOptions, "HEADERS");
  const char kResp[] = "{\"type\":\"FeatureCollection\",\"features\":[]}";
  psResult->pabyData = reinterpret_cast<GByte*>(CPLStrdup(kResp));
  psResult->nDataLen = strlen(kResp);

  if (pszHeaders && strstr(pszHeaders, "Custom-Header")) {
    psResult->pszContentType = CPLStrdup("application/json");
  }

  return psResult;
}

TEST_F(OGRGeoJSONUtilsTest, GeoJSONHTTPFetchWithContentTypeHeader) {
  CPLHTTPPushFetchCallback(MockFetchCallback, nullptr);
  absl::Cleanup popCallback = [] { CPLHTTPPopFetchCallback(); };

  // 1. Successful fetch with default headers
  {
    CPLHTTPResult* res =
        GeoJSONHTTPFetchWithContentTypeHeader("http://example.com/data.json");
    absl::Cleanup destroyResult = [res] { CPLHTTPDestroyResult(res); };
    ASSERT_NE(res, nullptr);
    EXPECT_GT(res->nDataLen, 0);
  }

  // 2. Successful fetch with GDAL_HTTP_HEADERS set (raw \r\n separated)
  {
    CPLSetConfigOption("GDAL_HTTP_HEADERS",
                       "Custom-Header: val\r\nAccept: application/geo+json");
    absl::Cleanup resetOption = [] {
      CPLSetConfigOption("GDAL_HTTP_HEADERS", nullptr);
    };

    CPLHTTPResult* res =
        GeoJSONHTTPFetchWithContentTypeHeader("http://example.com/data.json");
    absl::Cleanup destroyResult = [res] { CPLHTTPDestroyResult(res); };
    ASSERT_NE(res, nullptr);
    EXPECT_GT(res->nDataLen, 0);
  }

  // 3. Successful fetch with comma-separated GDAL_HTTP_HEADERS
  {
    CPLSetConfigOption("GDAL_HTTP_HEADERS",
                       "Accept: text/plain, application/json");
    absl::Cleanup resetOption = [] {
      CPLSetConfigOption("GDAL_HTTP_HEADERS", nullptr);
    };

    CPLHTTPResult* res =
        GeoJSONHTTPFetchWithContentTypeHeader("http://example.com/data.json");
    absl::Cleanup destroyResult = [res] { CPLHTTPDestroyResult(res); };
    ASSERT_NE(res, nullptr);
    EXPECT_GT(res->nDataLen, 0);
  }

  // 3b. Multiple headers without Accept:
  {
    CPLSetConfigOption("GDAL_HTTP_HEADERS", "X-Custom: foo, X-Other: bar");
    absl::Cleanup resetOption = [] {
      CPLSetConfigOption("GDAL_HTTP_HEADERS", nullptr);
    };

    CPLHTTPResult* res =
        GeoJSONHTTPFetchWithContentTypeHeader("http://example.com/data.json");
    absl::Cleanup destroyResult = [res] { CPLHTTPDestroyResult(res); };
    ASSERT_NE(res, nullptr);
    EXPECT_GT(res->nDataLen, 0);
  }

  // 4. Failed fetch (error status)
  {
    WithQuietHandler quiet;
    CPLHTTPResult* res =
        GeoJSONHTTPFetchWithContentTypeHeader("http://example.com/fail_http");
    EXPECT_EQ(res, nullptr);
    CPLErrorReset();
  }

  // 5. Empty fetch result
  {
    CPLHTTPResult* res =
        GeoJSONHTTPFetchWithContentTypeHeader("http://example.com/empty_http");
    EXPECT_EQ(res, nullptr);
  }
}

}  // namespace
}  // namespace autotest2
