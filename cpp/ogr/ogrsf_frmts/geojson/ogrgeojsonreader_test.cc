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

// Tests for the GeoJSON reader.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/geojson/ogrgeojsonreader.cpp
// third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonreader.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonreader.h"

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

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
#include "third_party/gdal/ogr/ogrlibjsonutils.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogr_geojson.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonutils.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json_header.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogrgeojsonreader_test";

class OGRGeoJSONReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRGeoJSON();
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(OGRGeoJSONReaderTest, UpdateLayerGeomType) {
  bool bFirst = true;
  OGRwkbGeometryType eLayer = wkbUnknown;

  // 1. First geometry sets layer type
  EXPECT_TRUE(OGRGeoJSONUpdateLayerGeomType(bFirst, wkbPoint, eLayer));
  EXPECT_FALSE(bFirst);
  EXPECT_EQ(eLayer, wkbPoint);

  // 2. Matching type keeps layer type
  EXPECT_TRUE(OGRGeoJSONUpdateLayerGeomType(bFirst, wkbPoint, eLayer));
  EXPECT_EQ(eLayer, wkbPoint);

  // 3. 2D + 3D of same base type -> promotes to 3D
  bFirst = false;
  eLayer = wkbPoint;
  EXPECT_TRUE(OGRGeoJSONUpdateLayerGeomType(bFirst, wkbPoint25D, eLayer));
  EXPECT_EQ(eLayer, wkbPoint25D);

  // 4. 3D + 2D of same base type -> keeps 3D
  bFirst = false;
  eLayer = wkbPoint25D;
  EXPECT_TRUE(OGRGeoJSONUpdateLayerGeomType(bFirst, wkbPoint, eLayer));
  EXPECT_EQ(eLayer, wkbPoint25D);

  // 5. Incompatible types -> returns false, sets wkbUnknown
  bFirst = false;
  eLayer = wkbPoint;
  EXPECT_FALSE(OGRGeoJSONUpdateLayerGeomType(bFirst, wkbPolygon, eLayer));
  EXPECT_EQ(eLayer, wkbUnknown);

  // 6. Already wkbUnknown + any geometry -> returns true, remains wkbUnknown
  bFirst = false;
  eLayer = wkbUnknown;
  EXPECT_TRUE(OGRGeoJSONUpdateLayerGeomType(bFirst, wkbPolygon, eLayer));
  EXPECT_EQ(eLayer, wkbUnknown);
}

TEST_F(OGRGeoJSONReaderTest, GetExtent3D) {
  // 1. Null inputs
  OGREnvelope3D sEnvelope;
  EXPECT_FALSE(OGRGeoJSONGetExtent3D(nullptr, &sEnvelope));
  EXPECT_FALSE(OGRGeoJSONGetExtent3D(nullptr, nullptr));

  // 2. Point 2D coordinates
  {
    OGREnvelope3D env;
    json_object* poPt2D = nullptr;
    ASSERT_TRUE(OGRJSonParse(
        "{\"type\":\"Point\",\"coordinates\":[10.0, 20.0]}", &poPt2D));
    EXPECT_TRUE(OGRGeoJSONGetExtent3D(poPt2D, &env));
    EXPECT_DOUBLE_EQ(env.MinX, 10.0);
    EXPECT_DOUBLE_EQ(env.MaxX, 10.0);
    EXPECT_DOUBLE_EQ(env.MinY, 20.0);
    EXPECT_DOUBLE_EQ(env.MaxY, 20.0);
    json_object_put(poPt2D);
  }

  // 3. Point 3D coordinates
  {
    OGREnvelope3D env;
    json_object* poPt3D = nullptr;
    ASSERT_TRUE(OGRJSonParse(
        "{\"type\":\"Point\",\"coordinates\":[10.0, 20.0, 30.0]}", &poPt3D));
    EXPECT_TRUE(OGRGeoJSONGetExtent3D(poPt3D, &env));
    EXPECT_DOUBLE_EQ(env.MinZ, 30.0);
    EXPECT_DOUBLE_EQ(env.MaxZ, 30.0);
    json_object_put(poPt3D);
  }

  // 4. LineString coordinates
  {
    OGREnvelope3D env;
    json_object* poLine = nullptr;
    ASSERT_TRUE(
        OGRJSonParse("{\"type\":\"LineString\",\"coordinates\":[[0.0, 0.0, "
                     "1.0], [10.0, 20.0, 5.0]]}",
                     &poLine));
    EXPECT_TRUE(OGRGeoJSONGetExtent3D(poLine, &env));
    EXPECT_DOUBLE_EQ(env.MinX, 0.0);
    EXPECT_DOUBLE_EQ(env.MaxX, 10.0);
    EXPECT_DOUBLE_EQ(env.MinY, 0.0);
    EXPECT_DOUBLE_EQ(env.MaxY, 20.0);
    EXPECT_DOUBLE_EQ(env.MinZ, 1.0);
    EXPECT_DOUBLE_EQ(env.MaxZ, 5.0);
    json_object_put(poLine);
  }

  // 5. GeometryCollection with geometries array
  {
    OGREnvelope3D env;
    json_object* poGC = nullptr;
    ASSERT_TRUE(
        OGRJSonParse("{\"type\":\"GeometryCollection\",\"geometries\":["
                     "{\"type\":\"Point\",\"coordinates\":[-5.0, 15.0]},"
                     "{\"type\":\"Point\",\"coordinates\":[25.0, -10.0]}"
                     "]}",
                     &poGC));
    EXPECT_TRUE(OGRGeoJSONGetExtent3D(poGC, &env));
    EXPECT_DOUBLE_EQ(env.MinX, -5.0);
    EXPECT_DOUBLE_EQ(env.MaxX, 25.0);
    EXPECT_DOUBLE_EQ(env.MinY, -10.0);
    EXPECT_DOUBLE_EQ(env.MaxY, 15.0);
    json_object_put(poGC);
  }

  // 6. Invalid / malformed coordinates
  {
    OGREnvelope3D env;
    json_object* poBad = nullptr;
    ASSERT_TRUE(OGRJSonParse(
        "{\"type\":\"Point\",\"coordinates\":\"not_an_array\"}", &poBad));
    EXPECT_FALSE(OGRGeoJSONGetExtent3D(poBad, &env));
    json_object_put(poBad);
  }
}

TEST_F(OGRGeoJSONReaderTest, GenerateFeatureDefnDealWithID) {
  // 1. Integer ID
  {
    int nPrevFieldIdx = -1;
    std::map<std::string, int> oMapFieldNameToIdx;
    std::vector<std::unique_ptr<OGRFieldDefn>> apoFieldDefn;
    gdal::DirectedAcyclicGraph<int, std::string> dag;
    bool bFeatureLevelIdAsFID = false;
    bool bFeatureLevelIdAsAttribute = false;
    bool bNeedFID64 = false;

    json_object* poObj = nullptr;
    ASSERT_TRUE(OGRJSonParse("{\"id\": 42, \"properties\": {}}", &poObj));
    json_object* poProps = json_object_object_get(poObj, "properties");
    OGRGeoJSONGenerateFeatureDefnDealWithID(
        poObj, poProps, nPrevFieldIdx, oMapFieldNameToIdx, apoFieldDefn, dag,
        bFeatureLevelIdAsFID, bFeatureLevelIdAsAttribute, bNeedFID64);
    EXPECT_TRUE(bFeatureLevelIdAsFID);
    EXPECT_FALSE(bNeedFID64);
    json_object_put(poObj);
  }

  // 2. 64-bit ID
  {
    int nPrevFieldIdx = -1;
    std::map<std::string, int> oMapFieldNameToIdx;
    std::vector<std::unique_ptr<OGRFieldDefn>> apoFieldDefn;
    gdal::DirectedAcyclicGraph<int, std::string> dag;
    bool bFeatureLevelIdAsFID = false;
    bool bFeatureLevelIdAsAttribute = false;
    bool bNeedFID64 = false;

    json_object* poObj = nullptr;
    ASSERT_TRUE(
        OGRJSonParse("{\"id\": 9999999999999, \"properties\": {}}", &poObj));
    json_object* poProps = json_object_object_get(poObj, "properties");
    OGRGeoJSONGenerateFeatureDefnDealWithID(
        poObj, poProps, nPrevFieldIdx, oMapFieldNameToIdx, apoFieldDefn, dag,
        bFeatureLevelIdAsFID, bFeatureLevelIdAsAttribute, bNeedFID64);
    EXPECT_TRUE(bNeedFID64);
    json_object_put(poObj);
  }

  // 3. String ID without properties.id creates id attribute
  {
    int nPrevFieldIdx = -1;
    std::map<std::string, int> oMapFieldNameToIdx;
    std::vector<std::unique_ptr<OGRFieldDefn>> apoFieldDefn;
    gdal::DirectedAcyclicGraph<int, std::string> dag;
    bool bFeatureLevelIdAsFID = false;
    bool bFeatureLevelIdAsAttribute = false;
    bool bNeedFID64 = false;

    json_object* poObj = nullptr;
    ASSERT_TRUE(
        OGRJSonParse("{\"id\": \"alpha\", \"properties\": {}}", &poObj));
    json_object* poProps = json_object_object_get(poObj, "properties");
    OGRGeoJSONGenerateFeatureDefnDealWithID(
        poObj, poProps, nPrevFieldIdx, oMapFieldNameToIdx, apoFieldDefn, dag,
        bFeatureLevelIdAsFID, bFeatureLevelIdAsAttribute, bNeedFID64);
    EXPECT_TRUE(bFeatureLevelIdAsAttribute);
    EXPECT_FALSE(bFeatureLevelIdAsFID);
    EXPECT_EQ(oMapFieldNameToIdx["id"], 0);
    ASSERT_EQ(apoFieldDefn.size(), 1);
    EXPECT_EQ(apoFieldDefn[0]->GetType(), OFTString);
    json_object_put(poObj);
  }

  // 4. Conflicting id in properties
  {
    int nPrevFieldIdx = -1;
    std::map<std::string, int> oMapFieldNameToIdx;
    std::vector<std::unique_ptr<OGRFieldDefn>> apoFieldDefn;
    gdal::DirectedAcyclicGraph<int, std::string> dag;
    bool bFeatureLevelIdAsFID = false;
    bool bFeatureLevelIdAsAttribute = false;
    bool bNeedFID64 = false;

    json_object* poObj = nullptr;
    ASSERT_TRUE(OGRJSonParse("{\"id\": \"beta\", \"properties\": {\"id\": 20}}",
                             &poObj));
    json_object* poProps = json_object_object_get(poObj, "properties");
    OGRGeoJSONGenerateFeatureDefnDealWithID(
        poObj, poProps, nPrevFieldIdx, oMapFieldNameToIdx, apoFieldDefn, dag,
        bFeatureLevelIdAsFID, bFeatureLevelIdAsAttribute, bNeedFID64);
    // When properties already has an "id" field, top-level id attribute is
    // ignored
    EXPECT_FALSE(bFeatureLevelIdAsAttribute);
    json_object_put(poObj);
  }
}

TEST_F(OGRGeoJSONReaderTest, AddOrUpdateFieldAndSetField) {
  std::vector<int> retIndices;
  std::map<std::string, int> oMapFieldNameToIdx;
  std::vector<std::unique_ptr<OGRFieldDefn>> apoFieldDefn;
  std::set<int> aoSetUndeterminedTypeFields;

  // 1. Boolean field
  json_object* poBool = json_object_new_boolean(true);
  OGRGeoJSONReaderAddOrUpdateField(
      retIndices, oMapFieldNameToIdx, apoFieldDefn, "is_active", poBool,
      /*bFlattenNestedAttributes=*/false, /*chNestedAttributeSeparator=*/'_',
      /*bArrayAsString=*/false, /*bDateAsString=*/false,
      aoSetUndeterminedTypeFields);
  json_object_put(poBool);
  ASSERT_EQ(apoFieldDefn.size(), 1);
  EXPECT_EQ(apoFieldDefn[0]->GetType(), OFTInteger);
  EXPECT_EQ(apoFieldDefn[0]->GetSubType(), OFSTBoolean);

  // 2. Integer field
  json_object* poInt = json_object_new_int(123);
  OGRGeoJSONReaderAddOrUpdateField(retIndices, oMapFieldNameToIdx, apoFieldDefn,
                                   "count", poInt, false, '_', false, false,
                                   aoSetUndeterminedTypeFields);
  json_object_put(poInt);
  ASSERT_EQ(apoFieldDefn.size(), 2);
  EXPECT_EQ(apoFieldDefn[1]->GetType(), OFTInteger);

  // 3. Int64 field
  json_object* poInt64 = json_object_new_int64(9999999999999LL);
  OGRGeoJSONReaderAddOrUpdateField(retIndices, oMapFieldNameToIdx, apoFieldDefn,
                                   "big_id", poInt64, false, '_', false, false,
                                   aoSetUndeterminedTypeFields);
  json_object_put(poInt64);
  ASSERT_EQ(apoFieldDefn.size(), 3);
  EXPECT_EQ(apoFieldDefn[2]->GetType(), OFTInteger64);

  // 4. Double field
  json_object* poDouble = json_object_new_double(3.14159);
  OGRGeoJSONReaderAddOrUpdateField(retIndices, oMapFieldNameToIdx, apoFieldDefn,
                                   "ratio", poDouble, false, '_', false, false,
                                   aoSetUndeterminedTypeFields);
  json_object_put(poDouble);
  ASSERT_EQ(apoFieldDefn.size(), 4);
  EXPECT_EQ(apoFieldDefn[3]->GetType(), OFTReal);

  // 5. String field
  json_object* poStr = json_object_new_string("hello");
  OGRGeoJSONReaderAddOrUpdateField(retIndices, oMapFieldNameToIdx, apoFieldDefn,
                                   "label", poStr, false, '_', false, false,
                                   aoSetUndeterminedTypeFields);
  json_object_put(poStr);
  ASSERT_EQ(apoFieldDefn.size(), 5);
  EXPECT_EQ(apoFieldDefn[4]->GetType(), OFTString);

  // 6. Date field
  json_object* poDate = json_object_new_string("2023-05-18");
  OGRGeoJSONReaderAddOrUpdateField(retIndices, oMapFieldNameToIdx, apoFieldDefn,
                                   "created_date", poDate, false, '_', false,
                                   false, aoSetUndeterminedTypeFields);
  json_object_put(poDate);
  ASSERT_EQ(apoFieldDefn.size(), 6);
  EXPECT_EQ(apoFieldDefn[5]->GetType(), OFTDate);

  // 7. Int array
  json_object* poIntArr = json_object_new_array();
  json_object_array_add(poIntArr, json_object_new_int(1));
  json_object_array_add(poIntArr, json_object_new_int(2));
  OGRGeoJSONReaderAddOrUpdateField(retIndices, oMapFieldNameToIdx, apoFieldDefn,
                                   "int_list", poIntArr, false, '_', false,
                                   false, aoSetUndeterminedTypeFields);
  json_object_put(poIntArr);
  ASSERT_EQ(apoFieldDefn.size(), 7);
  EXPECT_EQ(apoFieldDefn[6]->GetType(), OFTIntegerList);

  // 8. Nested object with flattening
  json_object* poNested = json_object_new_object();
  json_object_object_add(poNested, "subfield",
                         json_object_new_string("nested_val"));
  OGRGeoJSONReaderAddOrUpdateField(retIndices, oMapFieldNameToIdx, apoFieldDefn,
                                   "nested", poNested,
                                   /*bFlattenNestedAttributes=*/true, '_',
                                   false, false, aoSetUndeterminedTypeFields);
  json_object_put(poNested);
  EXPECT_TRUE(oMapFieldNameToIdx.contains("nested_subfield"));

  // Build an OGRGeoJSONLayer and test OGRGeoJSONReaderSetField safely
  OGRGeoJSONDataSource ds;
  auto poLayer = std::make_unique<OGRGeoJSONLayer>("layer", nullptr, wkbPoint,
                                                   &ds, nullptr);
  for (const auto& fld : apoFieldDefn) {
    whileUnsealing(poLayer->GetLayerDefn())->AddFieldDefn(fld.get());
  }

  auto poFeat = std::make_unique<OGRFeature>(poLayer->GetLayerDefn());

  json_object* poValInt = json_object_new_int(99);
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(), 1, "", poValInt, false,
                           '_');
  json_object_put(poValInt);
  EXPECT_EQ(poFeat->GetFieldAsInteger(1), 99);
}

TEST_F(OGRGeoJSONReaderTest, ReadGeometryTypes) {
  OGRGeoJSONReader reader;

  // 1. Point
  {
    json_object* poObj = nullptr;
    ASSERT_TRUE(OGRJSonParse("{\"type\":\"Point\",\"coordinates\":[1.0, 2.0]}",
                             &poObj));
    std::unique_ptr<OGRGeometry> geom(reader.ReadGeometry(poObj, nullptr));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryType(), wkbPoint);
    auto* pt = geom->toPoint();
    EXPECT_DOUBLE_EQ(pt->getX(), 1.0);
    EXPECT_DOUBLE_EQ(pt->getY(), 2.0);
    json_object_put(poObj);
  }

  // 2. MultiPoint
  {
    json_object* poObj = nullptr;
    ASSERT_TRUE(OGRJSonParse(
        "{\"type\":\"MultiPoint\",\"coordinates\":[[1.0, 2.0],[3.0, 4.0]]}",
        &poObj));
    std::unique_ptr<OGRGeometry> geom(reader.ReadGeometry(poObj, nullptr));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryType(), wkbMultiPoint);
    json_object_put(poObj);
  }

  // 3. LineString
  {
    json_object* poObj = nullptr;
    ASSERT_TRUE(OGRJSonParse(
        "{\"type\":\"LineString\",\"coordinates\":[[1,2],[3,4],[5,6]]}",
        &poObj));
    std::unique_ptr<OGRGeometry> geom(reader.ReadGeometry(poObj, nullptr));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryType(), wkbLineString);
    json_object_put(poObj);
  }

  // 4. MultiLineString
  {
    json_object* poObj = nullptr;
    ASSERT_TRUE(
        OGRJSonParse("{\"type\":\"MultiLineString\",\"coordinates\":[[[1,2],[3,"
                     "4]],[[5,6],[7,8]]]}",
                     &poObj));
    std::unique_ptr<OGRGeometry> geom(reader.ReadGeometry(poObj, nullptr));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryType(), wkbMultiLineString);
    json_object_put(poObj);
  }

  // 5. Polygon with interior ring
  {
    json_object* poObj = nullptr;
    ASSERT_TRUE(
        OGRJSonParse("{\"type\":\"Polygon\",\"coordinates\":["
                     "[[0,0],[10,0],[10,10],[0,10],[0,0]],"
                     "[[2,2],[8,2],[8,8],[2,8],[2,2]]"
                     "]}",
                     &poObj));
    std::unique_ptr<OGRGeometry> geom(reader.ReadGeometry(poObj, nullptr));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryType(), wkbPolygon);
    auto* poly = geom->toPolygon();
    EXPECT_EQ(poly->getNumInteriorRings(), 1);
    json_object_put(poObj);
  }

  // 6. MultiPolygon
  {
    json_object* poObj = nullptr;
    ASSERT_TRUE(
        OGRJSonParse("{\"type\":\"MultiPolygon\",\"coordinates\":["
                     "[[[0,0],[1,0],[1,1],[0,1],[0,0]]]"
                     "]}",
                     &poObj));
    std::unique_ptr<OGRGeometry> geom(reader.ReadGeometry(poObj, nullptr));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryType(), wkbMultiPolygon);
    json_object_put(poObj);
  }

  // 7. GeometryCollection
  {
    json_object* poObj = nullptr;
    ASSERT_TRUE(
        OGRJSonParse("{\"type\":\"GeometryCollection\",\"geometries\":["
                     "{\"type\":\"Point\",\"coordinates\":[0,0]},"
                     "{\"type\":\"LineString\",\"coordinates\":[[0,0],[1,1]]}"
                     "]}",
                     &poObj));
    std::unique_ptr<OGRGeometry> geom(reader.ReadGeometry(poObj, nullptr));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryType(), wkbGeometryCollection);
    json_object_put(poObj);
  }

  // 8. Geometry preserve false -> converts to GeometryCollection
  {
    reader.SetPreserveGeometryType(false);
    json_object* poObj = nullptr;
    ASSERT_TRUE(OGRJSonParse("{\"type\":\"Point\",\"coordinates\":[1.0, 2.0]}",
                             &poObj));
    std::unique_ptr<OGRGeometry> geom(reader.ReadGeometry(poObj, nullptr));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryType(), wkbGeometryCollection);
    json_object_put(poObj);
  }
}

TEST_F(OGRGeoJSONReaderTest, BaseReaderOptionsAndForeignMembers) {
  OGRGeoJSONBaseReader reader;
  reader.SetSkipAttributes(true);
  reader.SetFlattenNestedAttributes(true, '.');
  reader.SetStoreNativeData(true);
  reader.SetArrayAsString(true);
  reader.SetDateAsString(true);
  reader.SetForeignMemberProcessing(
      OGRGeoJSONBaseReader::ForeignMemberProcessing::STAC);

  EXPECT_FALSE(reader.ExtentRead());
  OGREnvelope3D env = reader.GetExtent3D();
  EXPECT_TRUE(std::isinf(env.MinX));

  OGRGeoJSONDataSource ds;
  auto poLayer = std::make_unique<OGRGeoJSONLayer>("layer", nullptr, wkbPoint,
                                                   &ds, nullptr);

  json_object* poObj = nullptr;
  ASSERT_TRUE(
      OGRJSonParse("{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
                   "\"coordinates\":[10,20]},"
                   "\"stac_version\":\"1.0.0\",\"assets\":{},\"links\":[],"
                   "\"properties\":{\"foo\":\"bar\"}}",
                   &poObj));

  std::unique_ptr<OGRFeature> feat(
      reader.ReadFeature(poLayer.get(), poObj, ""));
  ASSERT_NE(feat, nullptr);
  // Attributes are skipped because SetSkipAttributes(true) was set
  EXPECT_EQ(feat->GetDefnRef()->GetFieldCount(), 0);
  EXPECT_EQ(feat->GetFieldIndex("foo"), -1);
  json_object_put(poObj);
}

TEST_F(OGRGeoJSONReaderTest, ParseAndReadLayersFeatureCollection) {
  const char kDoc[] =
      "{\"type\":\"FeatureCollection\",\"name\":\"my_layer\",\"bbox\":[0,0,10,"
      "10],"
      "\"crs\":{\"type\":\"name\",\"properties\":{\"name\":\"urn:ogc:def:crs:"
      "OGC:1.3:CRS84\"}},"
      "\"features\":["
      "{\"type\":\"Feature\",\"id\":1,\"geometry\":{\"type\":\"Point\","
      "\"coordinates\":[1,2]},"
      "\"properties\":{\"name\":\"feat1\",\"score\":9.5}},"
      "{\"type\":\"Feature\",\"id\":2,\"geometry\":{\"type\":\"Point\","
      "\"coordinates\":[3,4]},"
      "\"properties\":{\"name\":\"feat2\",\"score\":8.0}}"
      "]}";

  OGRGeoJSONReader reader;
  EXPECT_EQ(reader.Parse(kDoc), OGRERR_NONE);
  EXPECT_NE(reader.GetJSonObject(), nullptr);

  OGRGeoJSONDataSource ds;
  reader.ReadLayers(&ds);
  ASSERT_EQ(ds.GetLayerCount(), 1);
  OGRLayer* layer = const_cast<OGRLayer*>(ds.GetLayer(0));
  ASSERT_NE(layer, nullptr);
  EXPECT_STREQ(layer->GetName(), "my_layer");
  EXPECT_EQ(layer->GetFeatureCount(TRUE), 2);
}

TEST_F(OGRGeoJSONReaderTest, ParseAndReadLayersStandaloneFeatureAndGeometry) {
  // 1. Standalone feature at root
  {
    const char kFeat[] =
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[5,5]},"
        "\"properties\":{\"attr\":\"val\"}}";
    OGRGeoJSONReader reader;
    EXPECT_EQ(reader.Parse(kFeat), OGRERR_NONE);
    OGRGeoJSONDataSource ds;
    reader.ReadLayers(&ds);
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }

  // 2. Standalone geometry at root
  {
    const char kGeom[] = "{\"type\":\"Point\",\"coordinates\":[10,20]}";
    OGRGeoJSONReader reader;
    EXPECT_EQ(reader.Parse(kGeom), OGRERR_NONE);
    OGRGeoJSONDataSource ds;
    reader.ReadLayers(&ds);
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }

  // 3. GeoCouch spatiallist format feature
  {
    const char kGeoCouch[] =
        "{\"type\":\"Feature\",\"geometry\":null,\"properties\":{"
        "\"_id\":\"item1\",\"_rev\":\"1-abc\",\"type\":\"Feature\","
        "\"properties\":{\"real_key\":\"real_val\"}}}";
    OGRGeoJSONReader reader;
    EXPECT_EQ(reader.Parse(kGeoCouch), OGRERR_NONE);
    OGRGeoJSONDataSource ds;
    reader.ReadLayers(&ds);
    ASSERT_EQ(ds.GetLayerCount(), 1);
    OGRLayer* poLayer = const_cast<OGRLayer*>(ds.GetLayer(0));
    ASSERT_NE(poLayer, nullptr);
    EXPECT_EQ(poLayer->GetFeatureCount(TRUE), 1);
  }

  // 4. Invalid data / null data
  {
    OGRGeoJSONReader reader;
    EXPECT_EQ(reader.Parse(nullptr), OGRERR_NONE);
    EXPECT_EQ(reader.Parse(""), OGRERR_CORRUPT_DATA);
    EXPECT_EQ(reader.Parse("{invalid_json}"), OGRERR_CORRUPT_DATA);
  }
}

TEST_F(OGRGeoJSONReaderTest, StreamingParserFirstPassAndFeatureReading) {
  const std::string file_path = std::string(kTestDir) + "/streaming.geojson";
  const char kData[] =
      "{\"type\":\"FeatureCollection\",\"name\":\"stream_layer\",\"bbox\":[0,0,"
      "10,10],"
      "\"features\":[\n"
      "{\"type\":\"Feature\",\"id\":10,\"geometry\":{\"type\":\"Point\","
      "\"coordinates\":[1,2]},\"properties\":{\"tag\":\"A\"}},\n"
      "{\"type\":\"Feature\",\"id\":20,\"geometry\":{\"type\":\"Point\","
      "\"coordinates\":[3,4]},\"properties\":{\"tag\":\"B\"}},\n"
      "{\"type\":\"Feature\",\"id\":30,\"geometry\":{\"type\":\"Point\","
      "\"coordinates\":[5,6]},\"properties\":{\"tag\":\"C\"}}\n"
      "]}";
  VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kData, 1, strlen(kData), fp);
  VSIFCloseL(fp);

  fp = VSIFOpenL(file_path.c_str(), "rb");
  ASSERT_NE(fp, nullptr);

  auto poReader = std::make_unique<OGRGeoJSONReader>();
  bool bTryStandardReading = false;
  OGRGeoJSONDataSource ds;
  bool ok = poReader->FirstPassReadLayer(&ds, fp, bTryStandardReading);
  if (!ok) {
    VSIFCloseL(fp);
  }
  ASSERT_TRUE(ok);
  // OGRGeoJSONLayer took ownership of fp and poReader.
  OGRGeoJSONReader* rawReader = poReader.release();
  EXPECT_FALSE(bTryStandardReading);
  EXPECT_TRUE(rawReader->CanEasilyAppend());
  EXPECT_TRUE(rawReader->FCHasBBOX());

  ASSERT_EQ(ds.GetLayerCount(), 1);
  auto* poLayer = const_cast<OGRGeoJSONLayer*>(
      cpl::down_cast<const OGRGeoJSONLayer*>(ds.GetLayer(0)));
  ASSERT_NE(poLayer, nullptr);

  // Sequential reading via GetNextFeature
  rawReader->ResetReading();
  std::unique_ptr<OGRFeature> feat1(rawReader->GetNextFeature(poLayer));
  ASSERT_NE(feat1, nullptr);
  EXPECT_EQ(feat1->GetFID(), 10);
  EXPECT_STREQ(feat1->GetFieldAsString("tag"), "A");

  std::unique_ptr<OGRFeature> feat2(rawReader->GetNextFeature(poLayer));
  ASSERT_NE(feat2, nullptr);
  EXPECT_EQ(feat2->GetFID(), 20);

  // Random access reading via GetFeature(poLayer, nFID)
  std::unique_ptr<OGRFeature> featRandom(rawReader->GetFeature(poLayer, 30));
  ASSERT_NE(featRandom, nullptr);
  EXPECT_EQ(featRandom->GetFID(), 30);
  EXPECT_STREQ(featRandom->GetFieldAsString("tag"), "C");

  // UnsetReader on poLayer and re-acquire ownership so
  // rawReader->IngestAll(poLayer) can be directly tested without
  // poLayer->AddFeature() re-triggering IngestAll.
  poLayer->UnsetReader();
  std::unique_ptr<OGRGeoJSONReader> reclaimedReader(rawReader);
  EXPECT_TRUE(reclaimedReader->IngestAll(poLayer));
  EXPECT_EQ(poLayer->GetFeatureCount(TRUE), 3);
}

TEST_F(OGRGeoJSONReaderTest, AddOrUpdateFieldTypeMatrix) {
  std::vector<int> retIndices;
  std::map<std::string, int> oMapFieldNameToIdx;
  std::vector<std::unique_ptr<OGRFieldDefn>> apoFieldDefn;
  std::set<int> aoSetUndeterminedTypeFields;

  auto AddFieldVal = [&](const char* pszKey, const char* pszJsonVal,
                         bool bFlatten = false, char chSep = '_',
                         bool bArrayAsString = false,
                         bool bDateAsString = false) {
    json_object* poVal = nullptr;
    if (pszJsonVal != nullptr) {
      poVal = json_tokener_parse(pszJsonVal);
    }
    OGRGeoJSONReaderAddOrUpdateField(
        retIndices, oMapFieldNameToIdx, apoFieldDefn, pszKey, poVal, bFlatten,
        chSep, bArrayAsString, bDateAsString, aoSetUndeterminedTypeFields);
    if (poVal) {
      json_object_put(poVal);
    }
  };

  // 1. Undetermined null first, then typed value
  AddFieldVal("null_then_int", nullptr);
  EXPECT_TRUE(aoSetUndeterminedTypeFields.contains(
      oMapFieldNameToIdx.at("null_then_int")));
  AddFieldVal("null_then_int", "123");
  EXPECT_FALSE(aoSetUndeterminedTypeFields.contains(
      oMapFieldNameToIdx.at("null_then_int")));
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("null_then_int")]->GetType(),
            OFTInteger);

  AddFieldVal("null_then_date", nullptr);
  AddFieldVal("null_then_date", "\"2024-01-01\"");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("null_then_date")]->GetType(),
            OFTDate);

  // 2. Integer upgrades
  AddFieldVal("int_bool_to_int", "true");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("int_bool_to_int")]->GetSubType(),
      OFSTBoolean);
  AddFieldVal("int_bool_to_int", "42");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("int_bool_to_int")]->GetSubType(),
      OFSTNone);

  AddFieldVal("int_to_int64", "10");
  AddFieldVal("int_to_int64", "9999999999999");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int_to_int64")]->GetType(),
            OFTInteger64);

  AddFieldVal("int_to_real", "10");
  AddFieldVal("int_to_real", "3.14");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int_to_real")]->GetType(),
            OFTReal);

  AddFieldVal("int_to_intlist", "10");
  AddFieldVal("int_to_intlist", "[1, 2]");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int_to_intlist")]->GetType(),
            OFTIntegerList);

  AddFieldVal("int_to_int64list", "10");
  AddFieldVal("int_to_int64list", "[9999999999999]");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int_to_int64list")]->GetType(),
            OFTInteger64List);

  AddFieldVal("int_to_reallist", "10");
  AddFieldVal("int_to_reallist", "[1.5, 2.5]");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int_to_reallist")]->GetType(),
            OFTRealList);

  AddFieldVal("int_to_strlist", "10");
  AddFieldVal("int_to_strlist", "[\"a\", \"b\"]");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int_to_strlist")]->GetType(),
            OFTStringList);

  AddFieldVal("int_to_str_json", "10");
  AddFieldVal("int_to_str_json", "\"hello\"");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int_to_str_json")]->GetType(),
            OFTString);
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("int_to_str_json")]->GetSubType(),
      OFSTJSON);

  // 3. Integer64 upgrades
  AddFieldVal("int64_to_real", "9999999999999");
  AddFieldVal("int64_to_real", "3.14");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int64_to_real")]->GetType(),
            OFTReal);

  AddFieldVal("int64_to_intlist", "9999999999999");
  AddFieldVal("int64_to_intlist", "[1, 2]");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int64_to_intlist")]->GetType(),
            OFTInteger64List);

  AddFieldVal("int64_to_reallist", "9999999999999");
  AddFieldVal("int64_to_reallist", "[1.5, 2.5]");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int64_to_reallist")]->GetType(),
            OFTRealList);

  AddFieldVal("int64_to_strlist", "9999999999999");
  AddFieldVal("int64_to_strlist", "[\"a\"]");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int64_to_strlist")]->GetType(),
            OFTStringList);

  AddFieldVal("int64_to_str_json", "9999999999999");
  AddFieldVal("int64_to_str_json", "\"val\"");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int64_to_str_json")]->GetType(),
            OFTString);
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("int64_to_str_json")]->GetSubType(),
      OFSTJSON);

  // 4. Real upgrades
  AddFieldVal("real_to_intlist", "3.14");
  AddFieldVal("real_to_intlist", "[1, 2]");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("real_to_intlist")]->GetType(),
            OFTRealList);

  AddFieldVal("real_to_strlist", "3.14");
  AddFieldVal("real_to_strlist", "[\"s\"]");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("real_to_strlist")]->GetType(),
            OFTStringList);

  AddFieldVal("real_to_str_json", "3.14");
  AddFieldVal("real_to_str_json", "\"txt\"");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("real_to_str_json")]->GetType(),
            OFTString);
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("real_to_str_json")]->GetSubType(),
      OFSTJSON);

  // 5. String upgrades
  AddFieldVal("str_to_strlist", "\"abc\"");
  AddFieldVal("str_to_strlist", "[\"def\"]");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("str_to_strlist")]->GetType(),
            OFTStringList);

  AddFieldVal("str_to_json", "\"abc\"");
  AddFieldVal("str_to_json", "123");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("str_to_json")]->GetType(),
            OFTString);
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("str_to_json")]->GetSubType(),
            OFSTJSON);

  // 6. IntegerList upgrades
  AddFieldVal("intlist_to_str_json", "[1, 2]");
  AddFieldVal("intlist_to_str_json", "\"not_empty\"");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("intlist_to_str_json")]->GetType(),
      OFTString);
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("intlist_to_str_json")]->GetSubType(),
      OFSTJSON);

  AddFieldVal("intlist_to_int64list", "[1, 2]");
  AddFieldVal("intlist_to_int64list", "[9999999999999]");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("intlist_to_int64list")]->GetType(),
      OFTInteger64List);

  AddFieldVal("intlist_to_reallist", "[1, 2]");
  AddFieldVal("intlist_to_reallist", "[1.5]");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("intlist_to_reallist")]->GetType(),
      OFTRealList);

  AddFieldVal("intlist_to_strlist", "[1, 2]");
  AddFieldVal("intlist_to_strlist", "[\"x\"]");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("intlist_to_strlist")]->GetType(),
      OFTStringList);

  AddFieldVal("intlist_to_int64_scalar", "[1, 2]");
  AddFieldVal("intlist_to_int64_scalar", "9999999999999");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("intlist_to_int64_scalar")]->GetType(),
      OFTInteger64List);

  AddFieldVal("intlist_to_real_scalar", "[1, 2]");
  AddFieldVal("intlist_to_real_scalar", "3.14");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("intlist_to_real_scalar")]->GetType(),
      OFTRealList);

  AddFieldVal("intlist_bool_to_int", "[true, false]");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("intlist_bool_to_int")]->GetSubType(),
      OFSTBoolean);
  AddFieldVal("intlist_bool_to_int", "10");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("intlist_bool_to_int")]->GetSubType(),
      OFSTNone);

  AddFieldVal("intlist_to_obj", "[1, 2]");
  AddFieldVal("intlist_to_obj", "{\"foo\":\"bar\"}");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("intlist_to_obj")]->GetType(),
            OFTString);
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("intlist_to_obj")]->GetSubType(),
            OFSTJSON);

  // 7. Integer64List upgrades
  AddFieldVal("int64list_to_reallist", "[9999999999999]");
  AddFieldVal("int64list_to_reallist", "[1.5]");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("int64list_to_reallist")]->GetType(),
      OFTRealList);

  AddFieldVal("int64list_to_real_scalar", "[9999999999999]");
  AddFieldVal("int64list_to_real_scalar", "3.14");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int64list_to_real_scalar")]
                ->GetType(),
            OFTRealList);

  AddFieldVal("int64list_to_obj", "[9999999999999]");
  AddFieldVal("int64list_to_obj", "{\"a\":1}");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("int64list_to_obj")]->GetType(),
            OFTString);
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("int64list_to_obj")]->GetSubType(),
      OFSTJSON);

  // 8. RealList upgrades
  AddFieldVal("reallist_to_strlist", "[1.5, 2.5]");
  AddFieldVal("reallist_to_strlist", "[\"text\"]");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("reallist_to_strlist")]->GetType(),
      OFTStringList);

  AddFieldVal("reallist_to_str_json", "[1.5]");
  AddFieldVal("reallist_to_str_json", "\"str\"");
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("reallist_to_str_json")]->GetType(),
      OFTString);
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("reallist_to_str_json")]->GetSubType(),
      OFSTJSON);

  // 9. StringList upgrades
  AddFieldVal("strlist_to_json", "[\"a\", \"b\"]");
  AddFieldVal("strlist_to_json", "{\"json_key\": \"json_val\"}");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("strlist_to_json")]->GetType(),
            OFTString);
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("strlist_to_json")]->GetSubType(),
      OFSTJSON);

  // 10. Date / Time / DateTime upgrades
  AddFieldVal("date_to_datetime", "\"2023-05-18\"");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("date_to_datetime")]->GetType(),
            OFTDate);
  AddFieldVal("date_to_datetime", "\"2023-05-18T10:30:00\"");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("date_to_datetime")]->GetType(),
            OFTDateTime);

  AddFieldVal("date_tz_mixed", "\"2023-05-18T10:30:00Z\"");
  AddFieldVal("date_tz_mixed", "\"2023-05-18T10:30:00+02:00\"");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("date_tz_mixed")]->GetTZFlag(),
            OGR_TZFLAG_MIXED_TZ);

  AddFieldVal("date_to_str", "\"2023-05-18\"");
  AddFieldVal("date_to_str", "\"not_a_date\"");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("date_to_str")]->GetType(),
            OFTString);
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("date_to_str")]->GetSubType(),
            OFSTNone);

  AddFieldVal("date_to_int_json", "\"2023-05-18\"");
  AddFieldVal("date_to_int_json", "123");
  EXPECT_EQ(apoFieldDefn[oMapFieldNameToIdx.at("date_to_int_json")]->GetType(),
            OFTString);
  EXPECT_EQ(
      apoFieldDefn[oMapFieldNameToIdx.at("date_to_int_json")]->GetSubType(),
      OFSTJSON);

  // 11. Multi-level nested flattening
  AddFieldVal("nested_deep", "{\"level1\":{\"level2\":{\"val\":99}}}", true,
              '_');
  EXPECT_TRUE(oMapFieldNameToIdx.contains("nested_deep_level1_level2_val"));
}

TEST_F(OGRGeoJSONReaderTest, ReaderSetFieldComprehensive) {
  OGRGeoJSONDataSource ds;
  auto poLayer = std::make_unique<OGRGeoJSONLayer>("test_layer", nullptr,
                                                   wkbPoint, &ds, nullptr);
  poLayer->SetFIDColumn("id_fid");

  auto* poDefn = poLayer->GetLayerDefn();
  {
    auto oUnsealer = whileUnsealing(poDefn);
    OGRFieldDefn fldFid("id_fid", OFTInteger);
    oUnsealer->AddFieldDefn(&fldFid);
    OGRFieldDefn fldFid64("id_fid64", OFTInteger64);
    oUnsealer->AddFieldDefn(&fldFid64);
    OGRFieldDefn fldInt("f_int", OFTInteger);
    oUnsealer->AddFieldDefn(&fldInt);
    OGRFieldDefn fldInt64("f_int64", OFTInteger64);
    oUnsealer->AddFieldDefn(&fldInt64);
    OGRFieldDefn fldReal("f_real", OFTReal);
    oUnsealer->AddFieldDefn(&fldReal);
    OGRFieldDefn fldIntList("f_intlist", OFTIntegerList);
    oUnsealer->AddFieldDefn(&fldIntList);
    OGRFieldDefn fldInt64List("f_int64list", OFTInteger64List);
    oUnsealer->AddFieldDefn(&fldInt64List);
    OGRFieldDefn fldRealList("f_reallist", OFTRealList);
    oUnsealer->AddFieldDefn(&fldRealList);
    OGRFieldDefn fldStrList("f_strlist", OFTStringList);
    oUnsealer->AddFieldDefn(&fldStrList);
    OGRFieldDefn fldStr("f_str", OFTString);
    oUnsealer->AddFieldDefn(&fldStr);
    OGRFieldDefn fldNested("pref_sub", OFTString);
    oUnsealer->AddFieldDefn(&fldNested);
  }

  auto poFeat = std::make_unique<OGRFeature>(poDefn);

  // 1. Null field
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(),
                           poDefn->GetFieldIndex("f_int"), "", nullptr, false,
                           '_');
  EXPECT_TRUE(poFeat->IsFieldNull(poDefn->GetFieldIndex("f_int")));

  // 2. Integer matching FID column
  json_object* poFidVal = json_object_new_int(77);
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(),
                           poDefn->GetFieldIndex("id_fid"), "", poFidVal, false,
                           '_');
  json_object_put(poFidVal);
  EXPECT_EQ(poFeat->GetFID(), 77);

  // 3. Integer64 matching FID column
  poLayer->SetFIDColumn("id_fid64");
  json_object* poFid64Val = json_object_new_int64(9999999999999LL);
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(),
                           poDefn->GetFieldIndex("id_fid64"), "", poFid64Val,
                           false, '_');
  json_object_put(poFid64Val);
  EXPECT_EQ(poFeat->GetFID(), 9999999999999LL);

  // 4. OFTReal
  json_object* poRealVal = json_object_new_double(2.718);
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(),
                           poDefn->GetFieldIndex("f_real"), "", poRealVal,
                           false, '_');
  json_object_put(poRealVal);
  EXPECT_DOUBLE_EQ(poFeat->GetFieldAsDouble(poDefn->GetFieldIndex("f_real")),
                   2.718);

  // 5. OFTIntegerList from array and scalar
  json_object* poIntListVal = json_object_new_array();
  json_object_array_add(poIntListVal, json_object_new_int(10));
  json_object_array_add(poIntListVal, json_object_new_int(20));
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(),
                           poDefn->GetFieldIndex("f_intlist"), "", poIntListVal,
                           false, '_');
  json_object_put(poIntListVal);
  int nCount = 0;
  const int* panList = poFeat->GetFieldAsIntegerList(
      poDefn->GetFieldIndex("f_intlist"), &nCount);
  ASSERT_EQ(nCount, 2);
  EXPECT_EQ(panList[0], 10);
  EXPECT_EQ(panList[1], 20);

  json_object* poIntScalar = json_object_new_int(30);
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(),
                           poDefn->GetFieldIndex("f_intlist"), "", poIntScalar,
                           false, '_');
  json_object_put(poIntScalar);
  panList = poFeat->GetFieldAsIntegerList(poDefn->GetFieldIndex("f_intlist"),
                                          &nCount);
  ASSERT_EQ(nCount, 1);
  EXPECT_EQ(panList[0], 30);

  // 6. OFTInteger64List from array and scalar
  json_object* poInt64ListVal = json_object_new_array();
  json_object_array_add(poInt64ListVal, json_object_new_int64(100000000000LL));
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(),
                           poDefn->GetFieldIndex("f_int64list"), "",
                           poInt64ListVal, false, '_');
  json_object_put(poInt64ListVal);
  const GIntBig* panList64 = poFeat->GetFieldAsInteger64List(
      poDefn->GetFieldIndex("f_int64list"), &nCount);
  ASSERT_EQ(nCount, 1);
  EXPECT_EQ(panList64[0], 100000000000LL);

  json_object* poInt64Scalar = json_object_new_int64(200000000000LL);
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(),
                           poDefn->GetFieldIndex("f_int64list"), "",
                           poInt64Scalar, false, '_');
  json_object_put(poInt64Scalar);
  panList64 = poFeat->GetFieldAsInteger64List(
      poDefn->GetFieldIndex("f_int64list"), &nCount);
  ASSERT_EQ(nCount, 1);
  EXPECT_EQ(panList64[0], 200000000000LL);

  // 7. OFTRealList from array and scalar
  json_object* poRealListVal = json_object_new_array();
  json_object_array_add(poRealListVal, json_object_new_double(1.1));
  json_object_array_add(poRealListVal, json_object_new_double(2.2));
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(),
                           poDefn->GetFieldIndex("f_reallist"), "",
                           poRealListVal, false, '_');
  json_object_put(poRealListVal);
  const double* padfList = poFeat->GetFieldAsDoubleList(
      poDefn->GetFieldIndex("f_reallist"), &nCount);
  ASSERT_EQ(nCount, 2);
  EXPECT_DOUBLE_EQ(padfList[0], 1.1);

  json_object* poRealScalar = json_object_new_double(3.3);
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(),
                           poDefn->GetFieldIndex("f_reallist"), "",
                           poRealScalar, false, '_');
  json_object_put(poRealScalar);
  padfList = poFeat->GetFieldAsDoubleList(poDefn->GetFieldIndex("f_reallist"),
                                          &nCount);
  ASSERT_EQ(nCount, 1);
  EXPECT_DOUBLE_EQ(padfList[0], 3.3);

  // 8. OFTStringList from array and scalar
  json_object* poStrListVal = json_object_new_array();
  json_object_array_add(poStrListVal, json_object_new_string("apple"));
  json_object_array_add(poStrListVal, json_object_new_string("banana"));
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(),
                           poDefn->GetFieldIndex("f_strlist"), "", poStrListVal,
                           false, '_');
  json_object_put(poStrListVal);
  char** papszStrList =
      poFeat->GetFieldAsStringList(poDefn->GetFieldIndex("f_strlist"));
  ASSERT_NE(papszStrList, nullptr);
  EXPECT_STREQ(papszStrList[0], "apple");
  EXPECT_STREQ(papszStrList[1], "banana");

  json_object* poStrScalar = json_object_new_string("cherry");
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(),
                           poDefn->GetFieldIndex("f_strlist"), "", poStrScalar,
                           false, '_');
  json_object_put(poStrScalar);
  papszStrList =
      poFeat->GetFieldAsStringList(poDefn->GetFieldIndex("f_strlist"));
  ASSERT_NE(papszStrList, nullptr);
  EXPECT_STREQ(papszStrList[0], "cherry");

  // 9. Nested attribute flattening
  json_object* poNested = json_object_new_object();
  json_object_object_add(poNested, "sub", json_object_new_string("deep_val"));
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(), -1, "pref", poNested,
                           true, '_');
  json_object_put(poNested);
  EXPECT_STREQ(poFeat->GetFieldAsString(poDefn->GetFieldIndex("pref_sub")),
               "deep_val");

  // 10. Negative field index returns safely
  OGRGeoJSONReaderSetField(poLayer.get(), poFeat.get(), -1, "", nullptr, false,
                           '_');
}

TEST_F(OGRGeoJSONReaderTest, DealWithIDAdvancedVariants) {
  // 1. Negative integer ID creates attribute "id"
  {
    int nPrevFieldIdx = -1;
    std::map<std::string, int> oMapFieldNameToIdx;
    std::vector<std::unique_ptr<OGRFieldDefn>> apoFieldDefn;
    gdal::DirectedAcyclicGraph<int, std::string> dag;
    bool bFeatureLevelIdAsFID = false;
    bool bFeatureLevelIdAsAttribute = false;
    bool bNeedFID64 = false;

    json_object* poObj = nullptr;
    ASSERT_TRUE(OGRJSonParse("{\"id\": -10, \"properties\": {}}", &poObj));
    json_object* poProps = json_object_object_get(poObj, "properties");
    OGRGeoJSONGenerateFeatureDefnDealWithID(
        poObj, poProps, nPrevFieldIdx, oMapFieldNameToIdx, apoFieldDefn, dag,
        bFeatureLevelIdAsFID, bFeatureLevelIdAsAttribute, bNeedFID64);
    EXPECT_FALSE(bFeatureLevelIdAsFID);
    EXPECT_TRUE(bFeatureLevelIdAsAttribute);
    ASSERT_EQ(apoFieldDefn.size(), 1);
    EXPECT_EQ(apoFieldDefn[0]->GetType(), OFTInteger);
    json_object_put(poObj);

    // Followed by 64-bit ID promotes to OFTInteger64
    json_object* poObj64 = nullptr;
    ASSERT_TRUE(
        OGRJSonParse("{\"id\": -9999999999999, \"properties\": {}}", &poObj64));
    json_object* poProps64 = json_object_object_get(poObj64, "properties");
    OGRGeoJSONGenerateFeatureDefnDealWithID(
        poObj64, poProps64, nPrevFieldIdx, oMapFieldNameToIdx, apoFieldDefn,
        dag, bFeatureLevelIdAsFID, bFeatureLevelIdAsAttribute, bNeedFID64);
    EXPECT_EQ(apoFieldDefn[0]->GetType(), OFTInteger64);
    json_object_put(poObj64);

    // Followed by string ID promotes to OFTString
    json_object* poObjStr = nullptr;
    ASSERT_TRUE(
        OGRJSonParse("{\"id\": \"alpha\", \"properties\": {}}", &poObjStr));
    json_object* poPropsStr = json_object_object_get(poObjStr, "properties");
    OGRGeoJSONGenerateFeatureDefnDealWithID(
        poObjStr, poPropsStr, nPrevFieldIdx, oMapFieldNameToIdx, apoFieldDefn,
        dag, bFeatureLevelIdAsFID, bFeatureLevelIdAsAttribute, bNeedFID64);
    EXPECT_EQ(apoFieldDefn[0]->GetType(), OFTString);
    json_object_put(poObjStr);
  }
}

TEST_F(OGRGeoJSONReaderTest, ForeignMembersAndPropertiesAtRoot) {
  // 1. Feature without "properties" member (attributes at root)
  {
    const char kFeatNoProps[] =
        "{\"type\":\"Feature\",\"id\":5,\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[1,2]},"
        "\"root_attr\":\"root_val\",\"null_attr\":null}";
    OGRGeoJSONReader reader;
    EXPECT_EQ(reader.Parse(kFeatNoProps), OGRERR_NONE);
    OGRGeoJSONDataSource ds;
    reader.ReadLayers(&ds);
    ASSERT_EQ(ds.GetLayerCount(), 1);
    auto* poLayer = ds.GetLayer(0);
    ASSERT_NE(poLayer, nullptr);
    ASSERT_EQ(poLayer->GetFeatureCount(TRUE), 1);
    std::unique_ptr<OGRFeature> feat(poLayer->GetNextFeature());
    ASSERT_NE(feat, nullptr);
    EXPECT_STREQ(feat->GetFieldAsString("root_attr"), "root_val");
  }

  // 2. Feature with "properties": null and "properties": []
  {
    const char kDocNullProps[] =
        "{\"type\":\"FeatureCollection\",\"features\":["
        "{\"type\":\"Feature\",\"geometry\":null,\"properties\":null},"
        "{\"type\":\"Feature\",\"geometry\":null,\"properties\":[]}"
        "]}";
    OGRGeoJSONReader reader;
    EXPECT_EQ(reader.Parse(kDocNullProps), OGRERR_NONE);
    OGRGeoJSONDataSource ds;
    reader.ReadLayers(&ds);
    ASSERT_EQ(ds.GetLayerCount(), 1);
    EXPECT_EQ(ds.GetLayer(0)->GetFeatureCount(TRUE), 2);
  }

  // 3. Feature missing geometry member
  {
    const char kFeatNoGeom[] =
        "{\"type\":\"Feature\",\"properties\":{\"key\":\"val\"}}";
    OGRGeoJSONReader reader;
    EXPECT_EQ(reader.Parse(kFeatNoGeom), OGRERR_NONE);
    OGRGeoJSONDataSource ds;
    reader.ReadLayers(&ds);
    ASSERT_EQ(ds.GetLayerCount(), 1);
    std::unique_ptr<OGRFeature> feat(ds.GetLayer(0)->GetNextFeature());
    ASSERT_NE(feat, nullptr);
    EXPECT_EQ(feat->GetGeometryRef(), nullptr);
  }

  // 4. Foreign members in STAC format
  {
    const char kStacDoc[] =
        "{\"type\":\"FeatureCollection\",\"features\":["
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[0,0]},"
        "\"stac_version\":\"1.0.0\","
        "\"assets\":{\"thumbnail\":{\"href\":\"http://"
        "img.jpg\",\"type\":\"image/jpeg\"}},"
        "\"custom_str\":\"extra_val\","
        "\"properties\":{\"foo\":\"bar\"}}"
        "]}";
    OGRGeoJSONReader reader;
    reader.SetStoreNativeData(true);
    EXPECT_EQ(reader.Parse(kStacDoc), OGRERR_NONE);
    OGRGeoJSONDataSource ds;
    reader.ReadLayers(&ds);
    ASSERT_EQ(ds.GetLayerCount(), 1);
    auto* poLayer = ds.GetLayer(0);
    ASSERT_NE(poLayer, nullptr);
    std::unique_ptr<OGRFeature> feat(poLayer->GetNextFeature());
    ASSERT_NE(feat, nullptr);
    EXPECT_STREQ(feat->GetFieldAsString("foo"), "bar");
    EXPECT_STREQ(feat->GetFieldAsString("custom_str"), "extra_val");
    EXPECT_STREQ(feat->GetFieldAsString("assets.thumbnail.href"),
                 "http://img.jpg");
    EXPECT_NE(feat->GetNativeData(), nullptr);
  }
}

TEST_F(OGRGeoJSONReaderTest, MultiLayerAndCoordinateResolution) {
  // 1. Multi-layer root object
  {
    const char kMultiLayerDoc[] =
        "{\"layer_one\":{\"type\":\"FeatureCollection\",\"features\":["
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[1,2]},\"properties\":{}}"
        "]},"
        "\"layer_two\":{\"type\":\"FeatureCollection\",\"features\":["
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[3,4]},\"properties\":{}}"
        "]}}";
    OGRGeoJSONReader reader;
    EXPECT_EQ(reader.Parse(kMultiLayerDoc), OGRERR_NONE);
    OGRGeoJSONDataSource ds;
    reader.ReadLayers(&ds);
    EXPECT_EQ(ds.GetLayerCount(), 2);
    EXPECT_NE(ds.GetLayerByName("layer_one"), nullptr);
    EXPECT_NE(ds.GetLayerByName("layer_two"), nullptr);
  }

  // 2. FeatureCollection with description, coordinate resolutions, and 3D
  // points
  {
    const char kResolutionDoc[] =
        "{\"type\":\"FeatureCollection\",\"description\":\"Layer Description\","
        "\"xy_coordinate_resolution\":0.001,\"z_coordinate_resolution\":0.01,"
        "\"features\":["
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[10,20,30]},\"properties\":{}}"
        "]}";
    OGRGeoJSONReader reader;
    reader.SetStoreNativeData(true);
    EXPECT_EQ(reader.Parse(kResolutionDoc), OGRERR_NONE);
    OGRGeoJSONDataSource ds;
    reader.ReadLayers(&ds);
    ASSERT_EQ(ds.GetLayerCount(), 1);
    auto* poLayer = ds.GetLayer(0);
    ASSERT_NE(poLayer, nullptr);
    EXPECT_STREQ(poLayer->GetMetadataItem("DESCRIPTION"), "Layer Description");
    EXPECT_NE(poLayer->GetMetadata("NATIVE_DATA"), nullptr);

    // Verify coordinate precision was applied to layer geom field
    auto* poGeomField = poLayer->GetLayerDefn()->GetGeomFieldDefn(0);
    ASSERT_NE(poGeomField, nullptr);
    EXPECT_DOUBLE_EQ(poGeomField->GetCoordinatePrecision().dfXYResolution,
                     0.001);
    EXPECT_DOUBLE_EQ(poGeomField->GetCoordinatePrecision().dfZResolution, 0.01);

    // 3D geometry gets EPSG:4979
    ASSERT_NE(poGeomField->GetSpatialRef(), nullptr);
    EXPECT_TRUE(poGeomField->GetSpatialRef()->IsGeographic());
  }

  // 3. Invalid FeatureCollection missing "features"
  {
    const char kBadFC[] = "{\"type\":\"FeatureCollection\"}";
    OGRGeoJSONReader reader;
    EXPECT_EQ(reader.Parse(kBadFC), OGRERR_NONE);
    OGRGeoJSONDataSource ds;
    reader.ReadLayers(&ds);
    EXPECT_EQ(ds.GetLayerCount(), 0);
  }

  // 4. Parse with UTF-8 BOM
  {
    std::string bomDoc =
        "\xef\xbb\xbf{\"type\":\"Point\",\"coordinates\":[1,2]}";
    OGRGeoJSONReader reader;
    EXPECT_EQ(reader.Parse(bomDoc.c_str()), OGRERR_NONE);
    OGRGeoJSONDataSource ds;
    reader.ReadLayers(&ds);
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }
}

TEST_F(OGRGeoJSONReaderTest, StreamingParserPrologEpilogAndThresholds) {
  // 1. File with UTF-8 BOM and loadGeoJSON( ... ) JSONP wrapper
  {
    const std::string file_path = std::string(kTestDir) + "/jsonp.geojson";
    std::string data =
        "\xef\xbb\xbfloadGeoJSON({\"type\":\"FeatureCollection\",\"name\":"
        "\"jsonp_layer\","
        "\"features\":["
        "{\"type\":\"Feature\",\"id\":100,\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[1,2]},\"properties\":{}},"
        "{\"type\":\"Feature\",\"id\":100,\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[3,4]},\"properties\":{}},"
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[5,6]},\"properties\":{}}"
        "]})";

    VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(data.data(), 1, data.size(), fp);
    VSIFCloseL(fp);

    fp = VSIFOpenL(file_path.c_str(), "rb");
    ASSERT_NE(fp, nullptr);

    auto poReader = std::make_unique<OGRGeoJSONReader>();
    bool bTryStandardReading = false;
    OGRGeoJSONDataSource ds;
    bool ok = poReader->FirstPassReadLayer(&ds, fp, bTryStandardReading);
    if (!ok) {
      VSIFCloseL(fp);
    }
    ASSERT_TRUE(ok);
    // ds / poLayer took ownership of fp and the reader.
    OGRGeoJSONReader* rawReader = poReader.release();
    ASSERT_EQ(ds.GetLayerCount(), 1);
    auto* poLayer = const_cast<OGRGeoJSONLayer*>(
        cpl::down_cast<const OGRGeoJSONLayer*>(ds.GetLayer(0)));
    ASSERT_NE(poLayer, nullptr);

    // Reading features: duplicate ID 100 altered to unique, missing ID assigned
    // unique
    std::unique_ptr<OGRFeature> f1(rawReader->GetNextFeature(poLayer));
    ASSERT_NE(f1, nullptr);
    EXPECT_EQ(f1->GetFID(), 100);

    std::unique_ptr<OGRFeature> f2(rawReader->GetNextFeature(poLayer));
    ASSERT_NE(f2, nullptr);
    EXPECT_NE(f2->GetFID(), 100);  // duplicate ID was altered

    std::unique_ptr<OGRFeature> f3(rawReader->GetNextFeature(poLayer));
    ASSERT_NE(f3, nullptr);
    EXPECT_NE(f3->GetFID(), OGRNullFID);

    // GetFeature non-existent FID returns nullptr
    EXPECT_EQ(rawReader->GetFeature(poLayer, 999999), nullptr);
  }

  // 2. Early exit via OGR_GEOJSON_MAX_FEATURES_FIRST_PASS
  {
    const std::string file_path = std::string(kTestDir) + "/max_feat.geojson";
    const char kData[] =
        "{\"type\":\"FeatureCollection\",\"features\":["
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[1,2]},\"properties\":{}},"
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[3,4]},\"properties\":{}}"
        "]}";
    VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(kData, 1, strlen(kData), fp);
    VSIFCloseL(fp);

    CPLSetConfigOption("OGR_GEOJSON_MAX_FEATURES_FIRST_PASS", "1");
    auto oResetConfig = absl::MakeCleanup([]() {
      CPLSetConfigOption("OGR_GEOJSON_MAX_FEATURES_FIRST_PASS", nullptr);
    });

    fp = VSIFOpenL(file_path.c_str(), "rb");
    ASSERT_NE(fp, nullptr);

    auto poReader = std::make_unique<OGRGeoJSONReader>();
    bool bTryStandardReading = false;
    OGRGeoJSONDataSource ds;
    bool ok = poReader->FirstPassReadLayer(&ds, fp, bTryStandardReading);
    if (!ok) {
      VSIFCloseL(fp);
    }
    ASSERT_TRUE(ok);
    poReader.release();
    ASSERT_EQ(ds.GetLayerCount(), 1);
    auto* poLayer = const_cast<OGRGeoJSONLayer*>(
        cpl::down_cast<const OGRGeoJSONLayer*>(ds.GetLayer(0)));
    ASSERT_NE(poLayer, nullptr);
  }

  // 3. Early exit via OGR_GEOJSON_MAX_BYTES_FIRST_PASS
  {
    const std::string file_path = std::string(kTestDir) + "/max_bytes.geojson";
    const char kData[] =
        "{\"type\":\"FeatureCollection\",\"features\":["
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[1,2]},\"properties\":{}}"
        "]}";
    VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(kData, 1, strlen(kData), fp);
    VSIFCloseL(fp);

    CPLSetConfigOption("OGR_GEOJSON_MAX_BYTES_FIRST_PASS", "1");
    auto oResetConfig = absl::MakeCleanup([]() {
      CPLSetConfigOption("OGR_GEOJSON_MAX_BYTES_FIRST_PASS", nullptr);
    });

    fp = VSIFOpenL(file_path.c_str(), "rb");
    ASSERT_NE(fp, nullptr);

    auto poReader = std::make_unique<OGRGeoJSONReader>();
    bool bTryStandardReading = false;
    OGRGeoJSONDataSource ds;
    bool ok = poReader->FirstPassReadLayer(&ds, fp, bTryStandardReading);
    if (!ok) {
      VSIFCloseL(fp);
    }
    ASSERT_TRUE(ok);
    poReader.release();
    ASSERT_EQ(ds.GetLayerCount(), 1);
    auto* poLayer = const_cast<OGRGeoJSONLayer*>(
        cpl::down_cast<const OGRGeoJSONLayer*>(ds.GetLayer(0)));
    ASSERT_NE(poLayer, nullptr);
  }

  // 4. TooComplex via OGR_GEOJSON_MAX_OBJ_SIZE
  {
    const std::string file_path =
        std::string(kTestDir) + "/too_complex.geojson";
    const char kData[] =
        "{\"type\":\"FeatureCollection\",\"features\":["
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
        "\"coordinates\":[1,2]},\"properties\":{\"big\":\"very_long_string_"
        "content_to_trigger_size_limit\"}}"
        "]}";
    VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(kData, 1, strlen(kData), fp);
    VSIFCloseL(fp);

    CPLSetConfigOption("OGR_GEOJSON_MAX_OBJ_SIZE", "0.00001");
    auto oResetConfig = absl::MakeCleanup(
        []() { CPLSetConfigOption("OGR_GEOJSON_MAX_OBJ_SIZE", nullptr); });

    fp = VSIFOpenL(file_path.c_str(), "rb");
    ASSERT_NE(fp, nullptr);

    OGRGeoJSONReader reader;
    bool bTryStandardReading = false;
    OGRGeoJSONDataSource ds;
    // Should fail and delete poLayer
    EXPECT_FALSE(reader.FirstPassReadLayer(&ds, fp, bTryStandardReading));
    VSIFCloseL(fp);
  }

  // 5. Non-FeatureCollection streaming file triggers standard reading
  {
    const std::string file_path = std::string(kTestDir) + "/standalone.geojson";
    const char kData[] = "{\"type\":\"Point\",\"coordinates\":[1,2]}";
    VSILFILE* fp = VSIFOpenL(file_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(kData, 1, strlen(kData), fp);
    VSIFCloseL(fp);

    fp = VSIFOpenL(file_path.c_str(), "rb");
    ASSERT_NE(fp, nullptr);

    OGRGeoJSONReader reader;
    bool bTryStandardReading = false;
    OGRGeoJSONDataSource ds;
    EXPECT_FALSE(reader.FirstPassReadLayer(&ds, fp, bTryStandardReading));
    EXPECT_TRUE(bTryStandardReading);
    VSIFCloseL(fp);
  }
}

}  // namespace
}  // namespace autotest2
