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

#include "third_party/gdal/ogr/ogresrijsongeometry.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrlibjsonutils.h"
#include "third_party/json_c/json_object.h"
#include "third_party/json_c/json_types.h"

namespace autotest2 {
namespace {

TEST(OgresrijsongeometryTest, GetGeometryType) {
  EXPECT_EQ(wkbUnknown, OGRESRIJSONGetGeometryType(nullptr));

  struct JsonDeleter {
    void operator()(json_object* poObj) const {
      if (poObj) json_object_put(poObj);
    }
  };

  auto parse_json = [](const char* json_str) {
    json_object* obj = nullptr;
    OGRJSonParse(json_str, &obj);
    return std::unique_ptr<json_object, JsonDeleter>(obj);
  };

  auto obj_no_type = parse_json("{}");
  EXPECT_EQ(wkbNone, OGRESRIJSONGetGeometryType(obj_no_type.get()));

  auto obj_point = parse_json("{\"geometryType\": \"esriGeometryPoint\"}");
  EXPECT_EQ(wkbPoint, OGRESRIJSONGetGeometryType(obj_point.get()));

  auto obj_polyline =
      parse_json("{\"geometryType\": \"esriGeometryPolyline\"}");
  EXPECT_EQ(wkbLineString, OGRESRIJSONGetGeometryType(obj_polyline.get()));

  auto obj_polygon = parse_json("{\"geometryType\": \"esriGeometryPolygon\"}");
  EXPECT_EQ(wkbPolygon, OGRESRIJSONGetGeometryType(obj_polygon.get()));

  auto obj_mp = parse_json("{\"geometryType\": \"esriGeometryMultiPoint\"}");
  EXPECT_EQ(wkbMultiPoint, OGRESRIJSONGetGeometryType(obj_mp.get()));

  auto obj_unknown = parse_json("{\"geometryType\": \"unknownType\"}");
  EXPECT_EQ(wkbUnknown, OGRESRIJSONGetGeometryType(obj_unknown.get()));
}

TEST(OgresrijsongeometryTest, CreateGeometryFromEsriJsonInvalidInputs) {
  WithQuietHandler quiet_handler;
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson(nullptr));
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson("invalid json"));
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson("{}"));
}

TEST(OgresrijsongeometryTest, PointGeometry) {
  WithQuietHandler quiet_handler;

  // 2D Point
  OGRGeometryH hGeom =
      OGR_G_CreateGeometryFromEsriJson("{\"x\": 10.5, \"y\": 20.5}");
  ASSERT_NE(nullptr, hGeom);
  OGRGeometry* poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbPoint, poGeom->getGeometryType());
  OGRPoint* poPoint = poGeom->toPoint();
  EXPECT_DOUBLE_EQ(10.5, poPoint->getX());
  EXPECT_DOUBLE_EQ(20.5, poPoint->getY());
  OGR_G_DestroyGeometry(hGeom);

  // 3D Point
  hGeom =
      OGR_G_CreateGeometryFromEsriJson("{\"x\": 1.0, \"y\": 2.0, \"z\": 3.5}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbPoint25D, poGeom->getGeometryType());
  poPoint = poGeom->toPoint();
  EXPECT_DOUBLE_EQ(1.0, poPoint->getX());
  EXPECT_DOUBLE_EQ(2.0, poPoint->getY());
  EXPECT_DOUBLE_EQ(3.5, poPoint->getZ());
  OGR_G_DestroyGeometry(hGeom);

  // Missing y
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson("{\"x\": 10.0}"));

  // Non-numeric x or y
  EXPECT_EQ(nullptr,
            OGR_G_CreateGeometryFromEsriJson("{\"x\": \"bad\", \"y\": 20.0}"));
  EXPECT_EQ(nullptr,
            OGR_G_CreateGeometryFromEsriJson("{\"x\": 10.0, \"y\": \"bad\"}"));

  // Non-numeric z
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson(
                         "{\"x\": 10.0, \"y\": 20.0, \"z\": \"bad\"}"));
}

TEST(OgresrijsongeometryTest, LineStringGeometry) {
  WithQuietHandler quiet_handler;

  // Empty paths
  OGRGeometryH hGeom = OGR_G_CreateGeometryFromEsriJson("{\"paths\": []}");
  ASSERT_NE(nullptr, hGeom);
  OGRGeometry* poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbLineString, poGeom->getGeometryType());
  EXPECT_EQ(0, poGeom->toLineString()->getNumPoints());
  OGR_G_DestroyGeometry(hGeom);

  // Single 2D path
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"paths\": [[[0.0, 1.0], [2.0, 3.0]]]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbLineString, poGeom->getGeometryType());
  OGRLineString* poLine = poGeom->toLineString();
  EXPECT_EQ(2, poLine->getNumPoints());
  EXPECT_DOUBLE_EQ(0.0, poLine->getX(0));
  EXPECT_DOUBLE_EQ(1.0, poLine->getY(0));
  EXPECT_DOUBLE_EQ(2.0, poLine->getX(1));
  EXPECT_DOUBLE_EQ(3.0, poLine->getY(1));
  OGR_G_DestroyGeometry(hGeom);

  // Multi-path
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"paths\": [[[0, 1], [2, 3]], [[4, 5], [6, 7]]]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbMultiLineString, poGeom->getGeometryType());
  OGRMultiLineString* poMLS = poGeom->toMultiLineString();
  EXPECT_EQ(2, poMLS->getNumGeometries());
  OGR_G_DestroyGeometry(hGeom);

  // 3D path with hasZ
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"hasZ\": true, \"paths\": [[[0, 1, 10], [2, 3, 20]]]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbLineString25D, poGeom->getGeometryType());
  poLine = poGeom->toLineString();
  EXPECT_DOUBLE_EQ(10.0, poLine->getZ(0));
  OGR_G_DestroyGeometry(hGeom);

  // Measured path with hasM (hasZ false)
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"hasM\": true, \"paths\": [[[0, 1, 100], [2, 3, 200]]]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbLineStringM, poGeom->getGeometryType());
  poLine = poGeom->toLineString();
  EXPECT_DOUBLE_EQ(100.0, poLine->getM(0));
  OGR_G_DestroyGeometry(hGeom);

  // Z and M path with 4 coordinates
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"hasZ\": true, \"hasM\": true, \"paths\": [[[0, 1, 10, 100], [2, 3, "
      "20, 200]]]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbLineStringZM, poGeom->getGeometryType());
  poLine = poGeom->toLineString();
  EXPECT_DOUBLE_EQ(10.0, poLine->getZ(0));
  EXPECT_DOUBLE_EQ(100.0, poLine->getM(0));
  OGR_G_DestroyGeometry(hGeom);

  // Invalid paths: not array, or element not array, or invalid coordinates
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson("{\"paths\": \"bad\"}"));
  EXPECT_EQ(nullptr,
            OGR_G_CreateGeometryFromEsriJson("{\"paths\": [\"not_array\"]}"));
  EXPECT_EQ(nullptr,
            OGR_G_CreateGeometryFromEsriJson("{\"paths\": [[[1]]]}"));  // len 1
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson(
                         "{\"paths\": [[[1, 2, 3, 4, 5]]]}"));  // len 5
  EXPECT_EQ(nullptr,
            OGR_G_CreateGeometryFromEsriJson("{\"paths\": [[[\"bad\", 2]]]}"));
  EXPECT_EQ(nullptr,
            OGR_G_CreateGeometryFromEsriJson("{\"paths\": [[[1, \"bad\"]]]}"));
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson(
                         "{\"paths\": [[[1, 2, \"bad\"]]]}"));
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson(
                         "{\"paths\": [[[1, 2, 3, \"bad\"]]]}"));
}

TEST(OgresrijsongeometryTest, PolygonGeometry) {
  WithQuietHandler quiet_handler;

  // Empty rings
  OGRGeometryH hGeom = OGR_G_CreateGeometryFromEsriJson("{\"rings\": []}");
  ASSERT_NE(nullptr, hGeom);
  OGRGeometry* poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_TRUE(poGeom->IsEmpty());
  OGR_G_DestroyGeometry(hGeom);

  // Single 2D ring
  std::unique_ptr<OGRGeometry> poGeom2D(
      OGRGeometry::FromHandle(OGR_G_CreateGeometryFromEsriJson(
          "{\"rings\": [[[0, 0], [0, 10], [10, 10], [10, 0], [0, 0]]]}")));
  ASSERT_NE(nullptr, poGeom2D);
  EXPECT_EQ(wkbPolygon, poGeom2D->getGeometryType());
  OGRPolygon* poPoly = poGeom2D->toPolygon();
  ASSERT_NE(nullptr, poPoly);
  ASSERT_NE(nullptr, poPoly->getExteriorRing());
  EXPECT_EQ(5, poPoly->getExteriorRing()->getNumPoints());

  // 3D ring with hasZ
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"hasZ\": true, \"rings\": [[[0, 0, 5], [0, 10, 5], [10, 10, 5], [10, "
      "0, 5], [0, 0, 5]]]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbPolygon25D, poGeom->getGeometryType());
  OGR_G_DestroyGeometry(hGeom);

  // M ring with hasM
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"hasM\": true, \"rings\": [[[0, 0, 1], [0, 10, 2], [10, 10, 3], [10, "
      "0, 4], [0, 0, 1]]]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbPolygonM, poGeom->getGeometryType());
  OGR_G_DestroyGeometry(hGeom);

  // ZM ring
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"hasZ\": true, \"hasM\": true, \"rings\": [[[0, 0, 1, 10], [0, 10, 1, "
      "20], [10, 10, 1, 30], [10, 0, 1, 40], [0, 0, 1, 10]]]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbPolygonZM, poGeom->getGeometryType());
  OGR_G_DestroyGeometry(hGeom);

  // Exterior + interior rings
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"rings\": ["
      "[[0, 0], [0, 10], [10, 10], [10, 0], [0, 0]],"
      "[[2, 2], [8, 2], [8, 8], [2, 8], [2, 2]]"
      "]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbPolygon, poGeom->getGeometryType());
  poPoly = poGeom->toPolygon();
  EXPECT_EQ(1, poPoly->getNumInteriorRings());
  OGR_G_DestroyGeometry(hGeom);

  // Invalid rings
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson("{\"rings\": \"bad\"}"));
  EXPECT_EQ(nullptr,
            OGR_G_CreateGeometryFromEsriJson("{\"rings\": [\"not_array\"]}"));
  // Second ring has invalid coordinates after first ring succeeds
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson(
                         "{\"rings\": [[[0, 0], [0, 1], [1, 1], [0, 0]], "
                         "\"bad\"]}"));
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson(
                         "{\"rings\": [[[0, 0], [0, 1], [1, 1], [0, 0]], "
                         "[[1]]]}"));
}

TEST(OgresrijsongeometryTest, MultiPointGeometry) {
  WithQuietHandler quiet_handler;

  // Empty points
  OGRGeometryH hGeom = OGR_G_CreateGeometryFromEsriJson("{\"points\": []}");
  ASSERT_NE(nullptr, hGeom);
  OGRGeometry* poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbMultiPoint, poGeom->getGeometryType());
  EXPECT_EQ(0, poGeom->toMultiPoint()->getNumGeometries());
  OGR_G_DestroyGeometry(hGeom);

  // 2D points
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"points\": [[1.0, 2.0], [3.0, 4.0]]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbMultiPoint, poGeom->getGeometryType());
  OGRMultiPoint* poMP = poGeom->toMultiPoint();
  EXPECT_EQ(2, poMP->getNumGeometries());
  OGR_G_DestroyGeometry(hGeom);

  // 3D points with hasZ
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"hasZ\": true, \"points\": [[1.0, 2.0, 3.0]]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbMultiPoint25D, poGeom->getGeometryType());
  OGR_G_DestroyGeometry(hGeom);

  // M points with hasM
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"hasM\": true, \"points\": [[1.0, 2.0, 50.0]]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbMultiPointM, poGeom->getGeometryType());
  OGR_G_DestroyGeometry(hGeom);

  // ZM points (4 coords)
  hGeom = OGR_G_CreateGeometryFromEsriJson(
      "{\"hasZ\": true, \"hasM\": true, \"points\": [[1.0, 2.0, 3.0, 4.0]]}");
  ASSERT_NE(nullptr, hGeom);
  poGeom = OGRGeometry::FromHandle(hGeom);
  EXPECT_EQ(wkbMultiPointZM, poGeom->getGeometryType());
  OGR_G_DestroyGeometry(hGeom);

  // Invalid points
  EXPECT_EQ(nullptr, OGR_G_CreateGeometryFromEsriJson("{\"points\": \"bad\"}"));
  EXPECT_EQ(nullptr,
            OGR_G_CreateGeometryFromEsriJson("{\"points\": [[1, \"bad\"]]}"));
}

TEST(OgresrijsongeometryTest, SpatialReference) {
  WithQuietHandler quiet_handler;

  struct JsonDeleter {
    void operator()(json_object* poObj) const {
      if (poObj) json_object_put(poObj);
    }
  };

  auto parse_json = [](const char* json_str) {
    json_object* obj = nullptr;
    OGRJSonParse(json_str, &obj);
    return std::unique_ptr<json_object, JsonDeleter>(obj);
  };

  // No spatialReference
  auto obj = parse_json("{}");
  EXPECT_EQ(nullptr, OGRESRIJSONReadSpatialReference(obj.get()));

  // wkid
  obj = parse_json("{\"spatialReference\": {\"wkid\": 4326}}");
  OGRSpatialReference* srs = OGRESRIJSONReadSpatialReference(obj.get());
  ASSERT_NE(nullptr, srs);
  EXPECT_TRUE(srs->IsGeographic());
  delete srs;

  // latestWkid overrides wkid
  obj = parse_json(
      "{\"spatialReference\": {\"wkid\": 102100, \"latestWkid\": 3857}}");
  srs = OGRESRIJSONReadSpatialReference(obj.get());
  ASSERT_NE(nullptr, srs);
  EXPECT_TRUE(srs->IsProjected());
  delete srs;

  // Invalid wkid
  obj = parse_json("{\"spatialReference\": {\"wkid\": -999999}}");
  EXPECT_EQ(nullptr, OGRESRIJSONReadSpatialReference(obj.get()));

  // wkt
  obj = parse_json(
      "{\"spatialReference\": {\"wkt\": \"GEOGCS[\\\"WGS 84\\\",DATUM[\\\"WGS_"
      "1984\\\",SPHEROID[\\\"WGS 84\\\",6378137,298.257223563]],"
      "PRIMEM[\\\"Greenwich\\\",0],UNIT[\\\"degree\\\",0.0174532925199433]]\""
      "}}");
  srs = OGRESRIJSONReadSpatialReference(obj.get());
  ASSERT_NE(nullptr, srs);
  EXPECT_TRUE(srs->IsGeographic());
  delete srs;

  // Invalid wkt
  obj = parse_json("{\"spatialReference\": {\"wkt\": \"INVALID_WKT\"}}");
  EXPECT_EQ(nullptr, OGRESRIJSONReadSpatialReference(obj.get()));

  // Empty spatialReference
  obj = parse_json("{\"spatialReference\": {}}");
  EXPECT_EQ(nullptr, OGRESRIJSONReadSpatialReference(obj.get()));
}

}  // namespace
}  // namespace autotest2
