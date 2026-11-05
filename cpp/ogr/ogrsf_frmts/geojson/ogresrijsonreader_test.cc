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

// Tests for the ESRIJSON reader.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/geojson/ogresrijsonreader.cpp
// third_party/gdal/ogr/ogrsf_frmts/geojson/ogresrijsonreader.cpp

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
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonreader.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/ogresrijsonreader_test";

class OGRESRIJSONReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRESRIJSON();
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(OGRESRIJSONReaderTest, ParseAndReadLayersLifecycle) {
  OGRESRIJSONReader reader;

  // ReadLayers before Parse does nothing
  auto poDS = std::make_unique<OGRGeoJSONDataSource>();
  reader.ReadLayers(poDS.get(), eGeoJSONSourceText);
  EXPECT_EQ(poDS->GetLayerCount(), 0);

  // Parse null text returns OGRERR_NONE
  EXPECT_EQ(reader.Parse(nullptr), OGRERR_NONE);

  // Parse corrupt text returns OGRERR_CORRUPT_DATA
  {
    WithQuietHandler quiet;
    EXPECT_EQ(reader.Parse("{corrupt json"), OGRERR_CORRUPT_DATA);
    CPLErrorReset();
  }

  // Layer name resolution from description
  const char kValidJson[] =
      R"json({"geometryType":"esriGeometryPoint","features":[]})json";
  EXPECT_EQ(reader.Parse(kValidJson), OGRERR_NONE);

  poDS->SetDescription("ESRIJSON:/vsimem/my_layer.json");
  reader.ReadLayers(poDS.get(), eGeoJSONSourceFile);
  ASSERT_EQ(poDS->GetLayerCount(), 1);
  EXPECT_STREQ(poDS->GetLayer(0)->GetName(), "my_layer");
}

TEST_F(OGRESRIJSONReaderTest, GeometryTypeAndSpatialReferenceVariants) {
  // Test all geometry types
  const struct {
    const char* geomType;
    OGRwkbGeometryType expected;
  } kGeomCases[] = {
      {"esriGeometryPoint", wkbPoint},
      {"esriGeometryMultipoint", wkbMultiPoint},
      {"esriGeometryPolyline", wkbLineString},
      {"esriGeometryPolygon", wkbPolygon},
      {"esriGeometryEnvelope", wkbUnknown},
  };

  for (const auto& item : kGeomCases) {
    const std::string json =
        std::string("{\"geometryType\":\"") + item.geomType +
        "\",\"spatialReference\":{\"wkid\":4326},\"features\":[]}";
    OGRESRIJSONReader reader;
    EXPECT_EQ(reader.Parse(json.c_str()), OGRERR_NONE);

    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get(), eGeoJSONSourceText);
    ASSERT_EQ(poDS->GetLayerCount(), 1);
    EXPECT_EQ(poDS->GetLayer(0)->GetGeomType(), item.expected);
  }

  // Missing geometryType with top-level spatialReference -> wkbUnknown
  {
    const char kJsonNoGeom[] =
        R"json({"spatialReference":{"wkid":3857},"features":[]})json";
    OGRESRIJSONReader reader;
    EXPECT_EQ(reader.Parse(kJsonNoGeom), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get(), eGeoJSONSourceText);
    ASSERT_EQ(poDS->GetLayerCount(), 1);
    EXPECT_EQ(poDS->GetLayer(0)->GetGeomType(), wkbUnknown);
  }

  // Missing geometryType and no root SRS, but features[0].geometry has SRS
  {
    const char kJsonFeatureSRS[] = R"json({
      "features": [
        {
          "geometry": {
            "spatialReference": {"wkid": 4326},
            "x": 1.0,
            "y": 2.0
          }
        }
      ]
    })json";
    OGRESRIJSONReader reader;
    EXPECT_EQ(reader.Parse(kJsonFeatureSRS), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get(), eGeoJSONSourceText);
    ASSERT_EQ(poDS->GetLayerCount(), 1);
    EXPECT_EQ(poDS->GetLayer(0)->GetGeomType(), wkbPoint);
  }
}

TEST_F(OGRESRIJSONReaderTest, SchemaFromFieldsArray) {
  // Test all 12 ESRI field types, aliases, length, and unknown field type
  const char kJsonFields[] = R"json({
    "geometryType": "esriGeometryPoint",
    "fields": [
      {"name": "OID", "type": "esriFieldTypeOID", "alias": "Object ID"},
      {"name": "STR_SHORT", "type": "esriFieldTypeString", "length": 50, "alias": "STR_SHORT"},
      {"name": "STR_UNLIMITED", "type": "esriFieldTypeString", "length": 2147483647},
      {"name": "FLT_SNGL", "type": "esriFieldTypeSingle"},
      {"name": "FLT_DBL", "type": "esriFieldTypeDouble"},
      {"name": "INT_SML", "type": "esriFieldTypeSmallInteger"},
      {"name": "INT_STD", "type": "esriFieldTypeInteger"},
      {"name": "INT_BIG", "type": "esriFieldTypeBigInteger"},
      {"name": "DT_FULL", "type": "esriFieldTypeDate"},
      {"name": "DT_DATEONLY", "type": "esriFieldTypeDateOnly"},
      {"name": "DT_TIMEONLY", "type": "esriFieldTypeTimeOnly"},
      {"name": "GUID_VAL", "type": "esriFieldTypeGUID"},
      {"name": "GLOBALID_VAL", "type": "esriFieldTypeGlobalID"},
      {"name": "UNKNOWN_FIELD", "type": "esriFieldTypeXML"}
    ],
    "features": [
      {
        "attributes": {
          "OID": 42,
          "STR_SHORT": "hello",
          "STR_UNLIMITED": "world",
          "FLT_SNGL": 1.5,
          "FLT_DBL": 3.14159,
          "INT_SML": 7,
          "INT_STD": 1000,
          "INT_BIG": 9999999999,
          "DT_FULL": 1609459200000,
          "DT_DATEONLY": "2026-01-01",
          "DT_TIMEONLY": "12:00:00",
          "GUID_VAL": "{12345678-1234-1234-1234-123456789abc}",
          "GLOBALID_VAL": "{87654321-4321-4321-4321-cba987654321}",
          "UNKNOWN_FIELD": "<xml/>",
          "EXTRA_ATTR": "extra"
        },
        "geometry": {"x": 10.0, "y": 20.0, "z": 5.0, "hasZ": true, "hasM": true, "m": 1.0}
      }
    ]
  })json";

  WithQuietHandler quiet;
  OGRESRIJSONReader reader;
  EXPECT_EQ(reader.Parse(kJsonFields), OGRERR_NONE);
  auto poDS = std::make_unique<OGRGeoJSONDataSource>();
  reader.ReadLayers(poDS.get(), eGeoJSONSourceText);
  ASSERT_EQ(poDS->GetLayerCount(), 1);
  OGRLayer* poLayer = poDS->GetLayer(0);
  ASSERT_NE(poLayer, nullptr);

  EXPECT_EQ(poLayer->GetFeatureCount(), 1);
  OGRFeature* poFeat = poLayer->GetNextFeature();
  ASSERT_NE(poFeat, nullptr);
  EXPECT_EQ(poFeat->GetFID(), 42);
  EXPECT_STREQ(poFeat->GetFieldAsString("STR_SHORT"), "hello");
  delete poFeat;
  CPLErrorReset();
}

TEST_F(OGRESRIJSONReaderTest, InvalidFieldsDefinitions) {
  // 1. fields: null
  {
    const char kJson[] = R"json({
      "geometryType": "esriGeometryPoint",
      "fields": null,
      "features": []
    })json";
    OGRESRIJSONReader reader;
    EXPECT_EQ(reader.Parse(kJson), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get(), eGeoJSONSourceText);
    EXPECT_EQ(poDS->GetLayerCount(), 1);
  }

  // 2. fields with missing name
  {
    const char kJson[] = R"json({
      "geometryType": "esriGeometryPoint",
      "fields": [{"type": "esriFieldTypeInteger"}],
      "features": []
    })json";
    WithQuietHandler quiet;
    OGRESRIJSONReader reader;
    EXPECT_EQ(reader.Parse(kJson), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get(), eGeoJSONSourceText);
    EXPECT_EQ(poDS->GetLayerCount(), 0);
    CPLErrorReset();
  }

  // 3. fields with missing type
  {
    const char kJson[] = R"json({
      "geometryType": "esriGeometryPoint",
      "fields": [{"name": "bad_field"}],
      "features": []
    })json";
    WithQuietHandler quiet;
    OGRESRIJSONReader reader;
    EXPECT_EQ(reader.Parse(kJson), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get(), eGeoJSONSourceText);
    EXPECT_EQ(poDS->GetLayerCount(), 0);
    CPLErrorReset();
  }
}

TEST_F(OGRESRIJSONReaderTest, SchemaFromFieldAliasesAndAttributes) {
  // Schema from fieldAliases
  {
    const char kJson[] = R"json({
      "geometryType": "esriGeometryPoint",
      "fieldAliases": {"COL1": "Column 1", "COL2": "Column 2"},
      "features": [
        {
          "attributes": {"COL1": "val1", "COL2": 123},
          "geometry": {"x": 1.0, "y": 2.0}
        }
      ]
    })json";
    OGRESRIJSONReader reader;
    EXPECT_EQ(reader.Parse(kJson), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get(), eGeoJSONSourceText);
    ASSERT_EQ(poDS->GetLayerCount(), 1);
    EXPECT_EQ(poDS->GetLayer(0)->GetFeatureCount(), 1);
  }

  // Schema inferred directly from nested attributes (neither fields nor
  // fieldAliases)
  {
    const char kJson[] = R"json({
      "geometryType": "esriGeometryPoint",
      "features": [
        {
          "attributes": {
            "name": "test",
            "nested": {"sub1": 10, "sub2": "subval"}
          },
          "geometry": {"x": 1.0, "y": 2.0}
        }
      ]
    })json";
    OGRESRIJSONReader reader;
    EXPECT_EQ(reader.Parse(kJson), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get(), eGeoJSONSourceText);
    ASSERT_EQ(poDS->GetLayerCount(), 1);
    OGRLayer* poLayer = poDS->GetLayer(0);
    EXPECT_NE(poLayer->GetLayerDefn()->GetFieldIndex("nested.sub1"), -1);
  }
}

TEST_F(OGRESRIJSONReaderTest, FeaturesEdgeCasesAndErrors) {
  // Missing features member
  {
    const char kJson[] = R"json({"geometryType": "esriGeometryPoint"})json";
    WithQuietHandler quiet;
    OGRESRIJSONReader reader;
    EXPECT_EQ(reader.Parse(kJson), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get(), eGeoJSONSourceText);
    EXPECT_EQ(poDS->GetLayerCount(), 0);
    CPLErrorReset();
  }

  // features contains null and feature with null geometry and negative epoch
  // date
  {
    const char kJson[] = R"json({
      "geometryType": "esriGeometryPoint",
      "fields": [{"name": "dt", "type": "esriFieldTypeDate"}],
      "features": [
        null,
        {
          "attributes": {"dt": -86400000},
          "geometry": null
        },
        {
          "attributes": {"dt": null},
          "geometry": {"x": "invalid", "y": 20.0}
        }
      ]
    })json";
    WithQuietHandler quiet;
    OGRESRIJSONReader reader;
    EXPECT_EQ(reader.Parse(kJson), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get(), eGeoJSONSourceText);
    ASSERT_EQ(poDS->GetLayerCount(), 1);
    EXPECT_EQ(poDS->GetLayer(0)->GetFeatureCount(), 2);
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
