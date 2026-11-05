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

#include "third_party/gdal/ogr/ogrgeojsongeometry.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/ogr/ogrlibjsonutils.h"
#include "third_party/json_c/json_object.h"
#include "third_party/json_c/json_types.h"

namespace autotest2 {
namespace {

struct JsonObjDeleter {
  void operator()(json_object* obj) const {
    if (obj) json_object_put(obj);
  }
};
using UniqueJsonObj = std::unique_ptr<json_object, JsonObjDeleter>;

UniqueJsonObj ParseJson(const char* str) {
  json_object* obj = nullptr;
  if (!OGRJSonParse(str, &obj)) return nullptr;
  return UniqueJsonObj(obj);
}

class OgrGeoJsonGeometryTest : public ::testing::Test {
 protected:
  void TearDown() override { OSRCleanup(); }
};

TEST_F(OgrGeoJsonGeometryTest, GetType) {
  EXPECT_EQ(GeoJSONObject::eUnknown, OGRGeoJSONGetType(nullptr));

  auto obj = ParseJson("{}");
  EXPECT_EQ(GeoJSONObject::eUnknown, OGRGeoJSONGetType(obj.get()));

  obj = ParseJson("{\"type\": \"NotAType\"}");
  EXPECT_EQ(GeoJSONObject::eUnknown, OGRGeoJSONGetType(obj.get()));

  const struct {
    const char* name;
    GeoJSONObject::Type expected;
  } kTypes[] = {
      {"Point", GeoJSONObject::ePoint},
      {"LineString", GeoJSONObject::eLineString},
      {"Polygon", GeoJSONObject::ePolygon},
      {"MultiPoint", GeoJSONObject::eMultiPoint},
      {"MultiLineString", GeoJSONObject::eMultiLineString},
      {"MultiPolygon", GeoJSONObject::eMultiPolygon},
      {"GeometryCollection", GeoJSONObject::eGeometryCollection},
      {"CircularString", GeoJSONObject::eCircularString},
      {"CompoundCurve", GeoJSONObject::eCompoundCurve},
      {"CurvePolygon", GeoJSONObject::eCurvePolygon},
      {"MultiCurve", GeoJSONObject::eMultiCurve},
      {"MultiSurface", GeoJSONObject::eMultiSurface},
      {"Feature", GeoJSONObject::eFeature},
      {"FeatureCollection", GeoJSONObject::eFeatureCollection},
  };

  for (const auto& item : kTypes) {
    std::string json_str = std::string("{\"type\": \"") + item.name + "\"}";
    obj = ParseJson(json_str.c_str());
    ASSERT_TRUE(obj != nullptr);
    EXPECT_EQ(item.expected, OGRGeoJSONGetType(obj.get()))
        << "Failed for type: " << item.name;
  }
}

TEST_F(OgrGeoJsonGeometryTest, JSONFGHasMeasure) {
  EXPECT_FALSE(OGRJSONFGHasMeasure(nullptr, false));
  EXPECT_TRUE(OGRJSONFGHasMeasure(nullptr, true));

  auto obj = ParseJson("{}");
  EXPECT_FALSE(OGRJSONFGHasMeasure(obj.get(), false));
  EXPECT_TRUE(OGRJSONFGHasMeasure(obj.get(), true));

  obj = ParseJson("{\"measures\": {\"enabled\": true}}");
  EXPECT_TRUE(OGRJSONFGHasMeasure(obj.get(), false));

  obj = ParseJson("{\"measures\": {\"enabled\": false}}");
  EXPECT_FALSE(OGRJSONFGHasMeasure(obj.get(), true));
}

TEST_F(OgrGeoJsonGeometryTest, GetOGRGeometryType) {
  EXPECT_EQ(wkbUnknown, OGRGeoJSONGetOGRGeometryType(nullptr, false));

  auto obj = ParseJson("{}");
  EXPECT_EQ(wkbUnknown, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson("{\"type\": \"Feature\"}");
  EXPECT_EQ(wkbUnknown, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  // Point: 2D, 3D (Z), 3D (M), 4D (ZM)
  obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1, 2]}");
  EXPECT_EQ(wkbPoint, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1, 2, 3]}");
  EXPECT_EQ(wkbPoint25D, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1, 2, 3]}");
  EXPECT_EQ(wkbPointM, OGRGeoJSONGetOGRGeometryType(obj.get(), true));

  obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1, 2, 3, 4]}");
  EXPECT_EQ(wkbPointZM, OGRGeoJSONGetOGRGeometryType(obj.get(), true));

  // Empty coordinates
  obj = ParseJson("{\"type\": \"Point\", \"coordinates\": []}");
  EXPECT_EQ(wkbPoint, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  // LineString, Polygon, MultiPoint, MultiLineString, MultiPolygon,
  // CircularString
  obj = ParseJson(
      "{\"type\": \"LineString\", \"coordinates\": [[1, 2], [3, 4]]}");
  EXPECT_EQ(wkbLineString, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson(
      "{\"type\": \"LineString\", \"coordinates\": [[1, 2, 5], [3, 4, 6]]}");
  EXPECT_EQ(wkbLineString25D, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson(
      "{\"type\": \"LineString\", \"coordinates\": [[1, 2, 5], [3, 4, 6]], "
      "\"measures\": {\"enabled\": true}}");
  EXPECT_EQ(wkbLineStringM, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson(
      "{\"type\": \"Polygon\", \"coordinates\": [[[0,0],[1,0],[1,1],[0,0]]]}");
  EXPECT_EQ(wkbPolygon, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson("{\"type\": \"MultiPoint\", \"coordinates\": [[1, 2]]}");
  EXPECT_EQ(wkbMultiPoint, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson(
      "{\"type\": \"MultiLineString\", \"coordinates\": [[[1, 2], [3, 4]]]}");
  EXPECT_EQ(wkbMultiLineString, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson(
      "{\"type\": \"MultiPolygon\", \"coordinates\": "
      "[[[[0,0],[1,0],[1,1],[0,0]]]]}");
  EXPECT_EQ(wkbMultiPolygon, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson(
      "{\"type\": \"CircularString\", \"coordinates\": [[0,0],[1,1],[2,0]]}");
  EXPECT_EQ(wkbCircularString, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  // Collection types: GeometryCollection, MultiCurve, MultiSurface,
  // CompoundCurve, CurvePolygon
  obj = ParseJson("{\"type\": \"GeometryCollection\", \"geometries\": []}");
  EXPECT_EQ(wkbGeometryCollection,
            OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson(
      "{\"type\": \"GeometryCollection\", \"geometries\": [{\"type\": "
      "\"Point\", \"coordinates\": [1, 2, 3]}]}");
  EXPECT_EQ(wkbGeometryCollection25D,
            OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson(
      "{\"type\": \"CompoundCurve\", \"geometries\": [{\"type\": "
      "\"LineString\", "
      "\"coordinates\": [[0,0,1,2],[1,1,1,2]]}]}");
  EXPECT_EQ(wkbCompoundCurveZM, OGRGeoJSONGetOGRGeometryType(obj.get(), true));

  obj = ParseJson("{\"type\": \"CurvePolygon\", \"geometries\": []}");
  EXPECT_EQ(wkbCurvePolygon, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson("{\"type\": \"MultiCurve\", \"geometries\": []}");
  EXPECT_EQ(wkbMultiCurve, OGRGeoJSONGetOGRGeometryType(obj.get(), false));

  obj = ParseJson("{\"type\": \"MultiSurface\", \"geometries\": []}");
  EXPECT_EQ(wkbMultiSurface, OGRGeoJSONGetOGRGeometryType(obj.get(), false));
}

TEST_F(OgrGeoJsonGeometryTest, GetGeometryName) {
  OGRPoint pt;
  EXPECT_STREQ("Point", OGRGeoJSONGetGeometryName(&pt));

  OGRLineString ls;
  EXPECT_STREQ("LineString", OGRGeoJSONGetGeometryName(&ls));

  OGRPolygon poly;
  EXPECT_STREQ("Polygon", OGRGeoJSONGetGeometryName(&poly));

  OGRMultiPoint mp;
  EXPECT_STREQ("MultiPoint", OGRGeoJSONGetGeometryName(&mp));

  OGRMultiLineString mls;
  EXPECT_STREQ("MultiLineString", OGRGeoJSONGetGeometryName(&mls));

  OGRMultiPolygon mpoly;
  EXPECT_STREQ("MultiPolygon", OGRGeoJSONGetGeometryName(&mpoly));

  OGRGeometryCollection gc;
  EXPECT_STREQ("GeometryCollection", OGRGeoJSONGetGeometryName(&gc));

  OGRCircularString cs;
  EXPECT_STREQ("CircularString", OGRGeoJSONGetGeometryName(&cs));

  OGRCompoundCurve cc;
  EXPECT_STREQ("CompoundCurve", OGRGeoJSONGetGeometryName(&cc));

  OGRCurvePolygon cp;
  EXPECT_STREQ("CurvePolygon", OGRGeoJSONGetGeometryName(&cp));

  OGRMultiCurve mc;
  EXPECT_STREQ("MultiCurve", OGRGeoJSONGetGeometryName(&mc));

  OGRMultiSurface ms;
  EXPECT_STREQ("MultiSurface", OGRGeoJSONGetGeometryName(&ms));

  OGRPolyhedralSurface ps;
  EXPECT_STREQ("Unknown", OGRGeoJSONGetGeometryName(&ps));
}

TEST_F(OgrGeoJsonGeometryTest, ReadGeometryPoints) {
  // Valid 2D point
  auto obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1.5, 2.5]}");
  auto geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbPoint, geom->getGeometryType());
  auto* pt = geom->toPoint();
  EXPECT_DOUBLE_EQ(1.5, pt->getX());
  EXPECT_DOUBLE_EQ(2.5, pt->getY());

  // Valid 3D point (Z)
  obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1.5, 2.5, 3.5]}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbPoint25D, geom->getGeometryType());
  pt = geom->toPoint();
  EXPECT_DOUBLE_EQ(3.5, pt->getZ());

  // Valid 3D point (M)
  obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1.5, 2.5, 4.5]}");
  geom = OGRGeoJSONReadGeometry(obj.get(), true, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbPointM, geom->getGeometryType());
  pt = geom->toPoint();
  EXPECT_DOUBLE_EQ(4.5, pt->getM());

  // Valid 4D point (ZM)
  obj = ParseJson(
      "{\"type\": \"Point\", \"coordinates\": [1.5, 2.5, 3.5, 4.5], "
      "\"measures\": {\"enabled\": true}}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbPointZM, geom->getGeometryType());
  pt = geom->toPoint();
  EXPECT_DOUBLE_EQ(3.5, pt->getZ());
  EXPECT_DOUBLE_EQ(4.5, pt->getM());

  // Point with too many dimensions (>3 for GeoJSON, >4 for JSON-FG)
  {
    WithQuietHandler quiet;
    obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1, 2, 3, 4]}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    ASSERT_NE(nullptr, geom);

    obj = ParseJson(
        "{\"type\": \"Point\", \"coordinates\": [1, 2, 3, 4, 5], \"measures\": "
        "{\"enabled\": true}}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    ASSERT_NE(nullptr, geom);
  }

  // Error cases: coordinate dimension < 2
  {
    WithQuietHandler quiet;
    obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1.5]}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    EXPECT_EQ(nullptr, geom);

    // Non-array coordinates
    obj = ParseJson("{\"type\": \"Point\", \"coordinates\": 123}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    EXPECT_EQ(nullptr, geom);

    obj = ParseJson("{\"type\": \"Point\", \"coordinates\": \"bad\"}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    EXPECT_EQ(nullptr, geom);

    obj = ParseJson("{\"type\": \"Point\", \"coordinates\": true}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    EXPECT_EQ(nullptr, geom);

    obj = ParseJson("{\"type\": \"Point\", \"coordinates\": {}}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    EXPECT_EQ(nullptr, geom);

    // Missing coordinates
    obj = ParseJson("{\"type\": \"Point\"}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    EXPECT_EQ(nullptr, geom);

    // Null object
    obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1, null]}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    EXPECT_EQ(nullptr, geom);

    // Non-numeric coordinate item
    obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1, \"text\"]}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    EXPECT_EQ(nullptr, geom);

    obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [\"text\", 1]}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    EXPECT_EQ(nullptr, geom);

    obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1, 2, \"text\"]}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    EXPECT_EQ(nullptr, geom);

    obj = ParseJson(
        "{\"type\": \"Point\", \"coordinates\": [1, 2, 3, \"text\"], "
        "\"measures\": {\"enabled\": true}}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    EXPECT_EQ(nullptr, geom);
  }
}

TEST_F(OgrGeoJsonGeometryTest, ReadMultiPoint) {
  auto obj = ParseJson(
      "{\"type\": \"MultiPoint\", \"coordinates\": [[1, 2], [3, 4]]}");
  auto geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbMultiPoint, geom->getGeometryType());
  EXPECT_EQ(2, geom->toMultiPoint()->getNumGeometries());

  // Error cases
  {
    WithQuietHandler quiet;
    // Missing coordinates
    obj = ParseJson("{\"type\": \"MultiPoint\"}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    // Non-array coordinates
    obj = ParseJson("{\"type\": \"MultiPoint\", \"coordinates\": 123}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    // Invalid point in array
    obj =
        ParseJson("{\"type\": \"MultiPoint\", \"coordinates\": [[1, 2], [3]]}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));
  }
}

TEST_F(OgrGeoJsonGeometryTest, ReadLineStringAndCircularString) {
  auto obj = ParseJson(
      "{\"type\": \"LineString\", \"coordinates\": [[1, 2], [3, 4]]}");
  auto geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbLineString, geom->getGeometryType());
  EXPECT_EQ(2, geom->toLineString()->getNumPoints());

  // CircularString
  obj = ParseJson(
      "{\"type\": \"CircularString\", \"coordinates\": [[0,0],[1,1],[2,0]]}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbCircularString, geom->getGeometryType());

  // 3D and Measured LineString
  obj = ParseJson(
      "{\"type\": \"LineString\", \"coordinates\": [[1, 2, 3], [4, 5, 6]]}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_TRUE(geom->Is3D());

  obj = ParseJson(
      "{\"type\": \"LineString\", \"coordinates\": [[1, 2, 3], [4, 5, 6]], "
      "\"measures\": {\"enabled\": true}}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_TRUE(geom->IsMeasured());

  // Errors
  {
    WithQuietHandler quiet;
    obj = ParseJson("{\"type\": \"LineString\"}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    obj = ParseJson("{\"type\": \"LineString\", \"coordinates\": 123}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    obj =
        ParseJson("{\"type\": \"LineString\", \"coordinates\": [[1, 2], [3]]}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));
  }
}

TEST_F(OgrGeoJsonGeometryTest, ReadMultiLineString) {
  auto obj = ParseJson(
      "{\"type\": \"MultiLineString\", \"coordinates\": [[[1, 2], [3, 4]], "
      "[[5, 6], [7, 8]]]}");
  auto geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbMultiLineString, geom->getGeometryType());
  EXPECT_EQ(2, geom->toMultiLineString()->getNumGeometries());

  // Error cases
  {
    WithQuietHandler quiet;
    obj = ParseJson("{\"type\": \"MultiLineString\"}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    obj = ParseJson("{\"type\": \"MultiLineString\", \"coordinates\": 123}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    obj = ParseJson("{\"type\": \"MultiLineString\", \"coordinates\": [null]}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(0, geom->toMultiLineString()->getNumGeometries());
  }
}

TEST_F(OgrGeoJsonGeometryTest, ReadPolygon) {
  EXPECT_EQ(nullptr, OGRGeoJSONReadPolygon(nullptr, false, false));

  // Valid polygon with exterior and interior ring
  auto obj = ParseJson(
      "{\"type\": \"Polygon\", \"coordinates\": [[[0,0],[10,0],[10,10],[0,0]], "
      "[[1,1],[2,1],[2,2],[1,1]]]}");
  auto geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbPolygon, geom->getGeometryType());
  EXPECT_EQ(1, geom->toPolygon()->getNumInteriorRings());

  // Empty polygon
  obj = ParseJson("{\"type\": \"Polygon\", \"coordinates\": []}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_TRUE(geom->IsEmpty());

  // Polygon with null first ring
  obj = ParseJson("{\"type\": \"Polygon\", \"coordinates\": [null]}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);

  // Polygon with valid outer and null inner ring
  obj = ParseJson(
      "{\"type\": \"Polygon\", \"coordinates\": "
      "[[[0,0],[10,0],[10,10],[0,0]], null]}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(0, geom->toPolygon()->getNumInteriorRings());

  // bRaw mode direct call
  auto rings_obj = ParseJson("[[[0,0],[1,0],[1,1],[0,0]]]");
  auto poly = OGRGeoJSONReadPolygon(rings_obj.get(), false, true);
  ASSERT_NE(nullptr, poly);

  // Errors
  {
    WithQuietHandler quiet;
    obj = ParseJson("{\"type\": \"Polygon\"}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    obj = ParseJson("{\"type\": \"Polygon\", \"coordinates\": 123}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));
  }
}

TEST_F(OgrGeoJsonGeometryTest, ReadMultiPolygon) {
  auto obj = ParseJson(
      "{\"type\": \"MultiPolygon\", \"coordinates\": "
      "[[[[0,0],[1,0],[1,1],[0,0]]], [[[2,2],[3,2],[3,3],[2,2]]]]}");
  auto geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbMultiPolygon, geom->getGeometryType());
  EXPECT_EQ(2, geom->toMultiPolygon()->getNumGeometries());

  // MultiPolygon with null element
  obj = ParseJson("{\"type\": \"MultiPolygon\", \"coordinates\": [null]}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(1, geom->toMultiPolygon()->getNumGeometries());

  // Errors
  {
    WithQuietHandler quiet;
    obj = ParseJson("{\"type\": \"MultiPolygon\"}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    obj = ParseJson("{\"type\": \"MultiPolygon\", \"coordinates\": 123}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));
  }
}

TEST_F(OgrGeoJsonGeometryTest, ReadCollections) {
  // GeometryCollection
  auto obj = ParseJson(
      "{\"type\": \"GeometryCollection\", \"geometries\": ["
      "{\"type\": \"Point\", \"coordinates\": [1, 2]}, "
      "{\"type\": \"LineString\", \"coordinates\": [[0, 0], [1, 1]]}]}");
  auto geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbGeometryCollection, geom->getGeometryType());
  EXPECT_EQ(2, geom->toGeometryCollection()->getNumGeometries());

  // GeometryCollection with null sub-geometry
  {
    WithQuietHandler quiet;
    obj =
        ParseJson("{\"type\": \"GeometryCollection\", \"geometries\": [null]}");
    geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(0, geom->toGeometryCollection()->getNumGeometries());

    obj = ParseJson("{\"type\": \"GeometryCollection\"}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    obj = ParseJson("{\"type\": \"GeometryCollection\", \"geometries\": 123}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));
  }

  // CompoundCurve: valid
  obj = ParseJson(
      "{\"type\": \"CompoundCurve\", \"geometries\": ["
      "{\"type\": \"LineString\", \"coordinates\": [[0, 0], [1, 1]]}, "
      "{\"type\": \"CircularString\", \"coordinates\": [[1, 1], [2, 0], [3, "
      "1]]}]}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbCompoundCurve, geom->getGeometryType());
  EXPECT_EQ(2, geom->toCompoundCurve()->getNumCurves());

  // CompoundCurve: invalid member type (e.g. Point)
  {
    WithQuietHandler quiet;
    obj = ParseJson(
        "{\"type\": \"CompoundCurve\", \"geometries\": ["
        "{\"type\": \"Point\", \"coordinates\": [1, 2]}]}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    // CompoundCurve: non-contiguous curves
    obj = ParseJson(
        "{\"type\": \"CompoundCurve\", \"geometries\": ["
        "{\"type\": \"LineString\", \"coordinates\": [[0, 0], [1, 1]]}, "
        "{\"type\": \"LineString\", \"coordinates\": [[5, 5], [6, 6]]}]}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));
  }

  // CurvePolygon: valid
  obj = ParseJson(
      "{\"type\": \"CurvePolygon\", \"geometries\": ["
      "{\"type\": \"LineString\", \"coordinates\": [[0, 0], [1, 0], [1, 1], "
      "[0, 0]]}]}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbCurvePolygon, geom->getGeometryType());

  // CurvePolygon: invalid member type
  {
    WithQuietHandler quiet;
    obj = ParseJson(
        "{\"type\": \"CurvePolygon\", \"geometries\": ["
        "{\"type\": \"Point\", \"coordinates\": [1, 2]}]}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    // CurvePolygon: unclosed curve
    obj = ParseJson(
        "{\"type\": \"CurvePolygon\", \"geometries\": ["
        "{\"type\": \"LineString\", \"coordinates\": [[0, 0], [1, 0]]}]}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));
  }

  // MultiCurve: valid
  obj = ParseJson(
      "{\"type\": \"MultiCurve\", \"geometries\": ["
      "{\"type\": \"LineString\", \"coordinates\": [[0, 0], [1, 1]]}]}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbMultiCurve, geom->getGeometryType());

  // MultiCurve: invalid child type (e.g. Point)
  {
    WithQuietHandler quiet;
    obj = ParseJson(
        "{\"type\": \"MultiCurve\", \"geometries\": ["
        "{\"type\": \"Point\", \"coordinates\": [1, 2]}]}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));
  }

  // MultiSurface: valid
  obj = ParseJson(
      "{\"type\": \"MultiSurface\", \"geometries\": ["
      "{\"type\": \"Polygon\", \"coordinates\": "
      "[[[0,0],[1,0],[1,1],[0,0]]]}]}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbMultiSurface, geom->getGeometryType());

  // MultiSurface: invalid child type (e.g. Point)
  {
    WithQuietHandler quiet;
    obj = ParseJson(
        "{\"type\": \"MultiSurface\", \"geometries\": ["
        "{\"type\": \"Point\", \"coordinates\": [1, 2]}]}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));
  }

  // Unsupported geometry types (Feature, FeatureCollection, Unknown)
  {
    WithQuietHandler quiet;
    obj = ParseJson("{\"type\": \"Feature\"}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    obj = ParseJson("{\"type\": \"FeatureCollection\"}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));

    obj = ParseJson("{\"type\": \"UnknownType\"}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadGeometry(obj.get(), false, nullptr));
  }
}

TEST_F(OgrGeoJsonGeometryTest, SpatialReferenceHandling) {
  // SpatialReference missing crs or empty
  EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(nullptr));

  auto obj = ParseJson("{}");
  EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));

  // crs without type
  obj = ParseJson("{\"crs\": {}}");
  EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));

  // NAME type
  obj = ParseJson("{\"crs\": {\"type\": \"name\"}}");
  EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));

  obj = ParseJson("{\"crs\": {\"type\": \"name\", \"properties\": {}}}");
  EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));

  obj = ParseJson(
      "{\"crs\": {\"type\": \"name\", \"properties\": {\"name\": "
      "\"urn:ogc:def:crs:OGC:1.3:CRS84\"}}}");
  auto* srs = OGRGeoJSONReadSpatialReference(obj.get());
  ASSERT_NE(nullptr, srs);
  EXPECT_TRUE(srs->IsGeographic());
  srs->Release();

  obj = ParseJson(
      "{\"crs\": {\"type\": \"name\", \"properties\": {\"name\": "
      "\"EPSG:4326\"}}}");
  srs = OGRGeoJSONReadSpatialReference(obj.get());
  ASSERT_NE(nullptr, srs);
  EXPECT_TRUE(srs->IsGeographic());
  srs->Release();

  {
    WithQuietHandler quiet;
    obj = ParseJson(
        "{\"crs\": {\"type\": \"name\", \"properties\": {\"name\": "
        "\"INVALID_NAME_XXX\"}}}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));
  }

  // EPSG type
  obj = ParseJson("{\"crs\": {\"type\": \"EPSG\"}}");
  EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));

  obj = ParseJson("{\"crs\": {\"type\": \"EPSG\", \"properties\": {}}}");
  EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));

  obj = ParseJson(
      "{\"crs\": {\"type\": \"EPSG\", \"properties\": {\"code\": 4326}}}");
  srs = OGRGeoJSONReadSpatialReference(obj.get());
  ASSERT_NE(nullptr, srs);
  EXPECT_TRUE(srs->IsGeographic());
  srs->Release();

  {
    WithQuietHandler quiet;
    obj = ParseJson(
        "{\"crs\": {\"type\": \"EPSG\", \"properties\": {\"code\": "
        "99999999}}}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));
  }

  // URL / LINK type
  obj = ParseJson("{\"crs\": {\"type\": \"URL\"}}");
  EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));

  obj = ParseJson("{\"crs\": {\"type\": \"URL\", \"properties\": {}}}");
  EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));

  {
    WithQuietHandler quiet;
    obj = ParseJson(
        "{\"crs\": {\"type\": \"URL\", \"properties\": {\"url\": "
        "\"http://invalid.example.com\"}}}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));

    obj = ParseJson(
        "{\"crs\": {\"type\": \"LINK\", \"properties\": {\"href\": "
        "\"http://invalid.example.com\"}}}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));
  }

  // OGC type
  obj = ParseJson("{\"crs\": {\"type\": \"OGC\"}}");
  EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));

  obj = ParseJson("{\"crs\": {\"type\": \"OGC\", \"properties\": {}}}");
  EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));

  obj = ParseJson(
      "{\"crs\": {\"type\": \"OGC\", \"properties\": {\"urn\": "
      "\"urn:ogc:def:crs:OGC:1.3:CRS84\"}}}");
  srs = OGRGeoJSONReadSpatialReference(obj.get());
  ASSERT_NE(nullptr, srs);
  EXPECT_TRUE(srs->IsGeographic());
  srs->Release();

  {
    WithQuietHandler quiet;
    obj = ParseJson(
        "{\"crs\": {\"type\": \"OGC\", \"properties\": {\"urn\": "
        "\"urn:ogc:def:crs:INVALID:1.3:CRS84\"}}}");
    EXPECT_EQ(nullptr, OGRGeoJSONReadSpatialReference(obj.get()));
  }

  // CRS assignment on geometry with poParentSRS
  OGRSpatialReference parent_srs;
  parent_srs.importFromEPSG(3857);

  obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1, 2]}");
  auto geom = OGRGeoJSONReadGeometry(obj.get(), false, &parent_srs);
  ASSERT_NE(nullptr, geom);
  ASSERT_NE(nullptr, geom->getSpatialReference());
  EXPECT_TRUE(geom->getSpatialReference()->IsProjected());

  // CRS embedded in geometry overrides parent SRS
  obj = ParseJson(
      "{\"type\": \"Point\", \"coordinates\": [1, 2], \"crs\": {\"type\": "
      "\"EPSG\", \"properties\": {\"code\": 4326}}}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, &parent_srs);
  ASSERT_NE(nullptr, geom);
  ASSERT_NE(nullptr, geom->getSpatialReference());
  EXPECT_TRUE(geom->getSpatialReference()->IsGeographic());

  // Default WGS84 when no CRS and parent SRS is null
  obj = ParseJson("{\"type\": \"Point\", \"coordinates\": [1, 2]}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
  ASSERT_NE(nullptr, geom->getSpatialReference());
  EXPECT_TRUE(geom->getSpatialReference()->IsGeographic());

  // CRS entry present but null
  obj = ParseJson(
      "{\"type\": \"Point\", \"coordinates\": [1, 2], \"crs\": null}");
  geom = OGRGeoJSONReadGeometry(obj.get(), false, nullptr);
  ASSERT_NE(nullptr, geom);
}

TEST_F(OgrGeoJsonGeometryTest, CCreateGeometryFromJson) {
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromJson(nullptr));

  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromJson("{not valid json"));
  }

  OGRGeometryH hGeom = OGR_G_CreateGeometryFromJson(
      "{\"type\": \"Point\", \"coordinates\": [10.0, 20.0]}");
  ASSERT_NE(nullptr, hGeom);
  EXPECT_EQ(wkbPoint, OGR_G_GetGeometryType(hGeom));
  EXPECT_DOUBLE_EQ(10.0, OGR_G_GetX(hGeom, 0));
  EXPECT_DOUBLE_EQ(20.0, OGR_G_GetY(hGeom, 0));
  OGR_G_DestroyGeometry(hGeom);
}

}  // namespace
}  // namespace autotest2
