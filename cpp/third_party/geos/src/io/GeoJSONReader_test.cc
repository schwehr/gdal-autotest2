// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for third_party/geos/src/io/GeoJSONReader.cpp.
//
// google3 notes:
// - GeometryFactory::create() returns GeometryFactory::Ptr; keep any custom
//   factory alive longer than both the GeoJSONReader and parsed Geometries.
// - GeoJSON 2D coordinates leave Z/M unset (DoubleNotANumber / quiet_NaN()).
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/io/GeoJSONReader.cpp

#include "third_party/geos/include/geos/io/GeoJSONReader.h"

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/GeoJSON.h"
#include "third_party/geos/include/geos/io/ParseException.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::GeoJSONFeature;
using geos::io::GeoJSONFeatureCollection;
using geos::io::GeoJSONReader;
using geos::io::GeoJSONValue;
using geos::io::ParseException;
using geos::util::IllegalArgumentException;
using testing::HasSubstr;

TEST(GeoJSONReaderTest, DefaultConstructorUsesDefaultFactory) {
  GeoJSONReader reader;
  std::unique_ptr<Geometry> geom =
      reader.read(R"({"type":"Point","coordinates":[1.5, 2.5]})");
  ASSERT_NE(geom, nullptr);
  EXPECT_EQ(geom->getFactory(), GeometryFactory::getDefaultInstance());
  EXPECT_EQ(geom->getSRID(), GeometryFactory::getDefaultInstance()->getSRID());
}

TEST(GeoJSONReaderTest, CustomGeometryFactoryPropagatedToCreatedGeometries) {
  PrecisionModel pm(100.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 4326);
  GeoJSONReader reader(*factory);

  const std::vector<std::string> samples = {
      R"({"type":"Point","coordinates":[1.0, 2.0]})",
      R"({"type":"Point","coordinates":[]})",
      R"({"type":"LineString","coordinates":[[0,0],[1,1]]})",
      R"({"type":"Polygon","coordinates":[[[0,0],[2,0],[2,2],[0,2],[0,0]]]})",
      R"({"type":"Polygon","coordinates":[]})",
      R"({"type":"MultiPoint","coordinates":[[0,0],[1,1]]})",
      R"({"type":"MultiLineString","coordinates":[[[0,0],[1,1]]]})",
      R"({"type":"MultiPolygon","coordinates":[[[[0,0],[1,0],[1,1],[0,0]]]]})",
      R"({"type":"GeometryCollection","geometries":[{"type":"Point","coordinates":[0,0]}]})",
      R"({"type":"Feature","geometry":{"type":"Point","coordinates":[3,4]},"properties":{}})",
      R"({"type":"FeatureCollection","features":[{"type":"Feature","geometry":{"type":"Point","coordinates":[5,6]},"properties":{}}]})",
  };

  for (const std::string& json_text : samples) {
    std::unique_ptr<Geometry> geom = reader.read(json_text);
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getFactory(), factory.get()) << json_text;
    EXPECT_EQ(geom->getSRID(), 4326) << json_text;
  }

  GeoJSONFeatureCollection fc = reader.readFeatures(
      R"({"type":"Feature","geometry":{"type":"Point","coordinates":[7,8]},"properties":{}})");
  ASSERT_EQ(fc.getFeatures().size(), 1u);
  ASSERT_NE(fc.getFeatures()[0].getGeometry(), nullptr);
  EXPECT_EQ(fc.getFeatures()[0].getGeometry()->getFactory(), factory.get());
  EXPECT_EQ(fc.getFeatures()[0].getGeometry()->getSRID(), 4326);
}

TEST(GeoJSONReaderTest, ReadPoint2D3DAndEmpty) {
  GeoJSONReader reader;

  // 2D Point, including extra RFC 7946 member such as "bbox".
  {
    std::unique_ptr<Geometry> geom = reader.read(
        R"({"type":"Point","bbox":[-10.5,20.25,-10.5,20.25],"coordinates":[-10.5, 20.25]})");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POINT);
    EXPECT_FALSE(geom->isEmpty());
    EXPECT_EQ(geom->getCoordinateDimension(), 2);
    EXPECT_FALSE(geom->hasZ());
    EXPECT_FALSE(geom->hasM());

    const auto* pt = dynamic_cast<const Point*>(geom.get());
    ASSERT_NE(pt, nullptr);
    EXPECT_DOUBLE_EQ(pt->getX(), -10.5);
    EXPECT_DOUBLE_EQ(pt->getY(), 20.25);
    EXPECT_TRUE(std::isnan(pt->getZ()));
    EXPECT_TRUE(std::isnan(pt->getM()));
  }

  // 3D Point.
  {
    std::unique_ptr<Geometry> geom = reader.read(
        R"({"type":"Point","coordinates":[-122.4194, 37.7749, 15.25]})");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POINT);
    EXPECT_FALSE(geom->isEmpty());
    EXPECT_EQ(geom->getCoordinateDimension(), 3);
    EXPECT_TRUE(geom->hasZ());
    EXPECT_FALSE(geom->hasM());

    const auto* pt = dynamic_cast<const Point*>(geom.get());
    ASSERT_NE(pt, nullptr);
    EXPECT_DOUBLE_EQ(pt->getX(), -122.4194);
    EXPECT_DOUBLE_EQ(pt->getY(), 37.7749);
    EXPECT_DOUBLE_EQ(pt->getZ(), 15.25);
    EXPECT_TRUE(std::isnan(pt->getM()));
  }

  // Empty Point ("coordinates": []).
  {
    std::unique_ptr<Geometry> geom =
        reader.read(R"({"type":"Point","coordinates":[]})");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POINT);
    EXPECT_TRUE(geom->isEmpty());
    EXPECT_EQ(geom->getNumPoints(), 0u);
    EXPECT_EQ(geom->getCoordinateDimension(), 2);
    EXPECT_FALSE(geom->hasZ());
    EXPECT_FALSE(geom->hasM());
  }
}

TEST(GeoJSONReaderTest, ReadPointInvalidCoordinateLengthThrowsParseException) {
  GeoJSONReader reader;

  try {
    reader.read(R"({"type":"Point","coordinates":[1.0]})");
    FAIL() << "Expected ParseException for 1-ordinate Point";
  } catch (const ParseException& ex) {
    EXPECT_THAT(ex.what(), HasSubstr("found one"));
  }

  try {
    reader.read(R"({"type":"Point","coordinates":[1.0, 2.0, 3.0, 4.0]})");
    FAIL() << "Expected ParseException for 4-ordinate Point";
  } catch (const ParseException& ex) {
    EXPECT_THAT(ex.what(), HasSubstr("more than three"));
  }
}

TEST(GeoJSONReaderTest, ReadLineString2D3DMixedAndEmpty) {
  GeoJSONReader reader;

  // Empty LineString.
  {
    std::unique_ptr<Geometry> geom =
        reader.read(R"({"type":"LineString","coordinates":[]})");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_TRUE(geom->isEmpty());
    EXPECT_EQ(geom->getNumPoints(), 0u);
    EXPECT_EQ(geom->getCoordinateDimension(), 2);
    EXPECT_FALSE(geom->hasZ());
  }

  // 2D LineString.
  {
    std::unique_ptr<Geometry> geom = reader.read(
        R"({"type":"LineString","coordinates":[[0.0, 0.0], [1.5, 2.5], [3.0, -1.0]]})");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_FALSE(geom->isEmpty());
    EXPECT_EQ(geom->getCoordinateDimension(), 2);
    EXPECT_FALSE(geom->hasZ());
    EXPECT_FALSE(geom->hasM());

    const auto* ls = dynamic_cast<const LineString*>(geom.get());
    ASSERT_NE(ls, nullptr);
    ASSERT_EQ(ls->getNumPoints(), 3u);
    const CoordinateSequence* seq = ls->getCoordinatesRO();
    EXPECT_DOUBLE_EQ(seq->getX(0), 0.0);
    EXPECT_DOUBLE_EQ(seq->getY(0), 0.0);
    EXPECT_TRUE(std::isnan(seq->getOrdinate(0, CoordinateSequence::Z)));
    EXPECT_DOUBLE_EQ(seq->getX(1), 1.5);
    EXPECT_DOUBLE_EQ(seq->getY(1), 2.5);
    EXPECT_DOUBLE_EQ(seq->getX(2), 3.0);
    EXPECT_DOUBLE_EQ(seq->getY(2), -1.0);
  }

  // 3D LineString.
  {
    std::unique_ptr<Geometry> geom = reader.read(
        R"({"type":"LineString","coordinates":[[1, 2, 10], [3, 4, 20]]})");
    const auto* ls = dynamic_cast<const LineString*>(geom.get());
    ASSERT_NE(ls, nullptr);
    EXPECT_TRUE(ls->hasZ());
    EXPECT_EQ(ls->getCoordinateDimension(), 3);
    ASSERT_EQ(ls->getNumPoints(), 2u);
    const CoordinateSequence* seq = ls->getCoordinatesRO();
    EXPECT_DOUBLE_EQ(seq->getOrdinate(0, CoordinateSequence::Z), 10.0);
    EXPECT_DOUBLE_EQ(seq->getOrdinate(1, CoordinateSequence::Z), 20.0);
  }

  // Mixed 2D and 3D vertices produce a 3D sequence with NaN on missing Z.
  {
    std::unique_ptr<Geometry> geom = reader.read(
        R"({"type":"LineString","coordinates":[[0, 0], [1, 1, 5.5], [2, 0]]})");
    const auto* ls = dynamic_cast<const LineString*>(geom.get());
    ASSERT_NE(ls, nullptr);
    EXPECT_TRUE(ls->hasZ());
    EXPECT_EQ(ls->getCoordinateDimension(), 3);
    ASSERT_EQ(ls->getNumPoints(), 3u);
    const CoordinateSequence* seq = ls->getCoordinatesRO();
    EXPECT_TRUE(std::isnan(seq->getOrdinate(0, CoordinateSequence::Z)));
    EXPECT_DOUBLE_EQ(seq->getOrdinate(1, CoordinateSequence::Z), 5.5);
    EXPECT_TRUE(std::isnan(seq->getOrdinate(2, CoordinateSequence::Z)));
  }
}

TEST(GeoJSONReaderTest, ReadLineStringInvalidInputsThrowExpectedExceptions) {
  GeoJSONReader reader;

  EXPECT_THROW(
      reader.read(R"({"type":"LineString","coordinates":[[1.0, 2.0]]})"),
      IllegalArgumentException);

  EXPECT_THROW(
      reader.read(R"({"type":"LineString","coordinates":[[], [1.0, 2.0]]})"),
      ParseException);

  try {
    reader.read(R"({"type":"LineString","coordinates":[[1.0], [2.0, 3.0]]})");
    FAIL() << "Expected ParseException for 1-ordinate LineString vertex";
  } catch (const ParseException& ex) {
    EXPECT_THAT(ex.what(), HasSubstr("found one"));
  }

  EXPECT_THROW(
      reader.read(
          R"({"type":"LineString","coordinates":[[1.0, 2.0, 3.0, 4.0], [5.0, 6.0, 7.0, 8.0]]})"),
      ParseException);
}

TEST(GeoJSONReaderTest, ReadPolygonEmptySingleRingAndWithHoles) {
  GeoJSONReader reader;

  // Completely empty Polygon ("coordinates": []).
  {
    std::unique_ptr<Geometry> geom =
        reader.read(R"({"type":"Polygon","coordinates":[]})");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(geom->isEmpty());
    EXPECT_EQ(geom->getCoordinateDimension(), 2);
    EXPECT_FALSE(geom->hasZ());
  }

  // Polygon with a single empty ring ("coordinates": [[]]).
  {
    std::unique_ptr<Geometry> geom =
        reader.read(R"({"type":"Polygon","coordinates":[[]]})");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(geom->isEmpty());
  }

  // Polygon with shell only.
  {
    std::unique_ptr<Geometry> geom = reader.read(
        R"({"type":"Polygon","coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]]]})");
    const auto* poly = dynamic_cast<const Polygon*>(geom.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_FALSE(poly->isEmpty());
    EXPECT_FALSE(poly->hasZ());
    EXPECT_EQ(poly->getCoordinateDimension(), 2);
    EXPECT_EQ(poly->getNumInteriorRing(), 0u);
    EXPECT_EQ(poly->getExteriorRing()->getNumPoints(), 5u);
    EXPECT_DOUBLE_EQ(poly->getArea(), 100.0);
  }

  // Polygon with shell and two interior holes.
  {
    std::unique_ptr<Geometry> geom = reader.read(R"({
      "type": "Polygon",
      "coordinates": [
        [[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]],
        [[1, 1], [1, 3], [3, 3], [3, 1], [1, 1]],
        [[5, 5], [5, 8], [8, 8], [8, 5], [5, 5]]
      ]
    })");
    const auto* poly = dynamic_cast<const Polygon*>(geom.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_FALSE(poly->isEmpty());
    ASSERT_EQ(poly->getNumInteriorRing(), 2u);
    EXPECT_EQ(poly->getInteriorRingN(0)->getNumPoints(), 5u);
    EXPECT_EQ(poly->getInteriorRingN(1)->getNumPoints(), 5u);
    EXPECT_DOUBLE_EQ(poly->getArea(), 100.0 - 4.0 - 9.0);
  }
}

TEST(GeoJSONReaderTest, ReadPolygon3DAndMixedCoordinateDimensions) {
  GeoJSONReader reader;

  std::unique_ptr<Geometry> geom = reader.read(R"({
    "type": "Polygon",
    "coordinates": [
      [[0, 0, 10], [10, 0, 10], [10, 10], [0, 10, 10], [0, 0, 10]],
      [[2, 2], [2, 4], [4, 4], [4, 2], [2, 2]]
    ]
  })");
  const auto* poly = dynamic_cast<const Polygon*>(geom.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_TRUE(poly->hasZ());
  EXPECT_EQ(poly->getCoordinateDimension(), 3);

  const CoordinateSequence* shell_seq =
      poly->getExteriorRing()->getCoordinatesRO();
  EXPECT_TRUE(poly->getExteriorRing()->hasZ());
  EXPECT_DOUBLE_EQ(shell_seq->getOrdinate(0, CoordinateSequence::Z), 10.0);
  EXPECT_TRUE(std::isnan(shell_seq->getOrdinate(2, CoordinateSequence::Z)));

  ASSERT_EQ(poly->getNumInteriorRing(), 1u);
  EXPECT_FALSE(poly->getInteriorRingN(0)->hasZ());
  EXPECT_EQ(poly->getInteriorRingN(0)->getCoordinateDimension(), 2);
}

TEST(GeoJSONReaderTest, ReadPolygonInvalidRingsAndCoordinatesThrow) {
  GeoJSONReader reader;

  // Unclosed ring.
  EXPECT_THROW(
      reader.read(
          R"({"type":"Polygon","coordinates":[[[0,0],[1,0],[1,1],[0,1]]]})"),
      IllegalArgumentException);

  // Too few points (< 3 for non-empty closed ring, e.g. 2 identical points).
  EXPECT_THROW(
      reader.read(R"({"type":"Polygon","coordinates":[[[0,0],[0,0]]]})"),
      IllegalArgumentException);

  // Empty shell with non-empty hole.
  EXPECT_THROW(
      reader.read(
          R"({"type":"Polygon","coordinates":[[],[[0,0],[1,0],[1,1],[0,0]]]})"),
      IllegalArgumentException);

  // Invalid coordinate arity inside ring.
  EXPECT_THROW(
      reader.read(
          R"({"type":"Polygon","coordinates":[[[0],[1,0],[1,1],[0,0]]]})"),
      ParseException);
  EXPECT_THROW(
      reader.read(
          R"({"type":"Polygon","coordinates":[[[0,0,0,0],[1,0,0,0],[1,1,0,0],[0,0,0,0]]]})"),
      ParseException);
}

TEST(GeoJSONReaderTest, ReadMultiPointEmpty2D3DAndMixed) {
  GeoJSONReader reader;

  // Empty MultiPoint.
  {
    std::unique_ptr<Geometry> geom =
        reader.read(R"({"type":"MultiPoint","coordinates":[]})");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
    EXPECT_TRUE(geom->isEmpty());
    EXPECT_EQ(geom->getNumGeometries(), 0u);
  }

  // 2D MultiPoint.
  {
    std::unique_ptr<Geometry> geom = reader.read(
        R"({"type":"MultiPoint","coordinates":[[1, 2], [3.5, -4.5]]})");
    const auto* mp = dynamic_cast<const MultiPoint*>(geom.get());
    ASSERT_NE(mp, nullptr);
    EXPECT_FALSE(mp->hasZ());
    EXPECT_EQ(mp->getCoordinateDimension(), 2);
    ASSERT_EQ(mp->getNumGeometries(), 2u);
    EXPECT_DOUBLE_EQ(mp->getGeometryN(0)->getX(), 1.0);
    EXPECT_DOUBLE_EQ(mp->getGeometryN(0)->getY(), 2.0);
    EXPECT_DOUBLE_EQ(mp->getGeometryN(1)->getX(), 3.5);
    EXPECT_DOUBLE_EQ(mp->getGeometryN(1)->getY(), -4.5);
  }

  // Mixed 2D and 3D MultiPoint.
  {
    std::unique_ptr<Geometry> geom = reader.read(
        R"({"type":"MultiPoint","coordinates":[[1, 2], [3, 4, 5]]})");
    const auto* mp = dynamic_cast<const MultiPoint*>(geom.get());
    ASSERT_NE(mp, nullptr);
    EXPECT_TRUE(mp->hasZ());
    EXPECT_EQ(mp->getCoordinateDimension(), 3);
    ASSERT_EQ(mp->getNumGeometries(), 2u);
    EXPECT_FALSE(mp->getGeometryN(0)->hasZ());
    EXPECT_TRUE(std::isnan(mp->getGeometryN(0)->getZ()));
    EXPECT_TRUE(mp->getGeometryN(1)->hasZ());
    EXPECT_DOUBLE_EQ(mp->getGeometryN(1)->getZ(), 5.0);
  }
}

TEST(GeoJSONReaderTest, ReadMultiPointInvalidCoordinatesThrowParseException) {
  GeoJSONReader reader;

  EXPECT_THROW(reader.read(R"({"type":"MultiPoint","coordinates":[[]]})"),
               ParseException);
  EXPECT_THROW(reader.read(R"({"type":"MultiPoint","coordinates":[[1]]})"),
               ParseException);
  EXPECT_THROW(
      reader.read(R"({"type":"MultiPoint","coordinates":[[1, 2, 3, 4]]})"),
      ParseException);
}

TEST(GeoJSONReaderTest, ReadMultiLineStringEmpty2D3DAndMixed) {
  GeoJSONReader reader;

  // Empty MultiLineString.
  {
    std::unique_ptr<Geometry> geom =
        reader.read(R"({"type":"MultiLineString","coordinates":[]})");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
    EXPECT_TRUE(geom->isEmpty());
    EXPECT_EQ(geom->getNumGeometries(), 0u);
  }

  // Mixed 2D, 3D/mixed, and empty member LineStrings.
  {
    std::unique_ptr<Geometry> geom = reader.read(R"({
      "type": "MultiLineString",
      "coordinates": [
        [[0, 0], [1, 1]],
        [[2, 2, 10], [3, 3]],
        []
      ]
    })");
    const auto* mls = dynamic_cast<const MultiLineString*>(geom.get());
    ASSERT_NE(mls, nullptr);
    EXPECT_FALSE(mls->isEmpty());
    EXPECT_TRUE(mls->hasZ());
    EXPECT_EQ(mls->getCoordinateDimension(), 3);
    ASSERT_EQ(mls->getNumGeometries(), 3u);

    EXPECT_FALSE(mls->getGeometryN(0)->hasZ());
    EXPECT_EQ(mls->getGeometryN(0)->getNumPoints(), 2u);

    const LineString* line1 = mls->getGeometryN(1);
    EXPECT_TRUE(line1->hasZ());
    EXPECT_DOUBLE_EQ(
        line1->getCoordinatesRO()->getOrdinate(0, CoordinateSequence::Z), 10.0);
    EXPECT_TRUE(std::isnan(
        line1->getCoordinatesRO()->getOrdinate(1, CoordinateSequence::Z)));

    EXPECT_TRUE(mls->getGeometryN(2)->isEmpty());
  }
}

TEST(GeoJSONReaderTest, ReadMultiLineStringErrorsThrow) {
  GeoJSONReader reader;

  EXPECT_THROW(
      reader.read(R"({"type":"MultiLineString","coordinates":[[[0, 0]]]})"),
      IllegalArgumentException);
  EXPECT_THROW(
      reader.read(R"({"type":"MultiLineString","coordinates":[[[0]]]})"),
      ParseException);
}

TEST(GeoJSONReaderTest, ReadMultiPolygonEmptyAndMultiplePolygons) {
  GeoJSONReader reader;

  // Empty MultiPolygon.
  {
    std::unique_ptr<Geometry> geom =
        reader.read(R"({"type":"MultiPolygon","coordinates":[]})");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_TRUE(geom->isEmpty());
    EXPECT_EQ(geom->getNumGeometries(), 0u);
  }

  // Multiple Polygons: one with hole, one 3D shell-only, one empty.
  {
    std::unique_ptr<Geometry> geom = reader.read(R"({
      "type": "MultiPolygon",
      "coordinates": [
        [
          [[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]],
          [[2, 2], [2, 4], [4, 4], [4, 2], [2, 2]]
        ],
        [
          [[20, 0, 5], [22, 0, 5], [22, 2, 5], [20, 2, 5], [20, 0, 5]]
        ],
        []
      ]
    })");
    const auto* mp = dynamic_cast<const MultiPolygon*>(geom.get());
    ASSERT_NE(mp, nullptr);
    EXPECT_FALSE(mp->isEmpty());
    EXPECT_TRUE(mp->hasZ());
    ASSERT_EQ(mp->getNumGeometries(), 3u);

    EXPECT_EQ(mp->getGeometryN(0)->getNumInteriorRing(), 1u);
    EXPECT_DOUBLE_EQ(mp->getGeometryN(0)->getArea(), 96.0);

    EXPECT_EQ(mp->getGeometryN(1)->getNumInteriorRing(), 0u);
    EXPECT_TRUE(mp->getGeometryN(1)->hasZ());
    EXPECT_DOUBLE_EQ(mp->getGeometryN(1)->getArea(), 4.0);

    EXPECT_TRUE(mp->getGeometryN(2)->isEmpty());
    EXPECT_DOUBLE_EQ(mp->getArea(), 100.0);
  }
}

TEST(GeoJSONReaderTest, ReadGeometryCollectionEmptyHeterogeneousAndNested) {
  GeoJSONReader reader;

  // Empty GeometryCollection.
  {
    std::unique_ptr<Geometry> geom =
        reader.read(R"({"type":"GeometryCollection","geometries":[]})");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_TRUE(geom->isEmpty());
    EXPECT_EQ(geom->getNumGeometries(), 0u);
  }

  // Heterogeneous and nested GeometryCollection.
  {
    std::unique_ptr<Geometry> geom = reader.read(R"({
      "type": "GeometryCollection",
      "geometries": [
        {"type": "Point", "coordinates": [1, 2]},
        {"type": "LineString", "coordinates": [[0, 0], [3, 4]]},
        {
          "type": "GeometryCollection",
          "geometries": [
            {"type": "Polygon", "coordinates": [[[0,0],[1,0],[1,1],[0,1],[0,0]]]}
          ]
        }
      ]
    })");
    const auto* gc = dynamic_cast<const GeometryCollection*>(geom.get());
    ASSERT_NE(gc, nullptr);
    EXPECT_FALSE(gc->isEmpty());
    ASSERT_EQ(gc->getNumGeometries(), 3u);
    EXPECT_EQ(gc->getGeometryN(0)->getGeometryTypeId(), geos::geom::GEOS_POINT);
    EXPECT_EQ(gc->getGeometryN(1)->getGeometryTypeId(),
              geos::geom::GEOS_LINESTRING);
    EXPECT_EQ(gc->getGeometryN(2)->getGeometryTypeId(),
              geos::geom::GEOS_GEOMETRYCOLLECTION);

    const auto* nested =
        dynamic_cast<const GeometryCollection*>(gc->getGeometryN(2));
    ASSERT_NE(nested, nullptr);
    ASSERT_EQ(nested->getNumGeometries(), 1u);
    EXPECT_EQ(nested->getGeometryN(0)->getGeometryTypeId(),
              geos::geom::GEOS_POLYGON);
  }
}

TEST(GeoJSONReaderTest,
     ReadGeometryCollectionMissingGeometriesThrowsParseException) {
  GeoJSONReader reader;
  EXPECT_THROW(reader.read(R"({"type":"GeometryCollection"})"), ParseException);
}

TEST(GeoJSONReaderTest, ReadFeatureAndFeatureCollectionReturnGeometries) {
  GeoJSONReader reader;

  // Feature returns its underlying Geometry, even if "properties" is omitted.
  {
    std::unique_ptr<Geometry> geom = reader.read(R"({
      "type": "Feature",
      "id": "f-1",
      "geometry": {"type": "Point", "coordinates": [12.5, -7.25]},
      "properties": {"label": "marker"}
    })");
    const auto* pt = dynamic_cast<const Point*>(geom.get());
    ASSERT_NE(pt, nullptr);
    EXPECT_DOUBLE_EQ(pt->getX(), 12.5);
    EXPECT_DOUBLE_EQ(pt->getY(), -7.25);

    std::unique_ptr<Geometry> no_props = reader.read(
        R"({"type":"Feature","geometry":{"type":"Point","coordinates":[1,2]}})");
    const auto* pt2 = dynamic_cast<const Point*>(no_props.get());
    ASSERT_NE(pt2, nullptr);
    EXPECT_DOUBLE_EQ(pt2->getX(), 1.0);
    EXPECT_DOUBLE_EQ(pt2->getY(), 2.0);
  }

  // Empty FeatureCollection returns empty GeometryCollection.
  {
    std::unique_ptr<Geometry> geom =
        reader.read(R"({"type":"FeatureCollection","features":[]})");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_TRUE(geom->isEmpty());
    EXPECT_EQ(geom->getNumGeometries(), 0u);
  }

  // FeatureCollection returns GeometryCollection containing feature geometries.
  {
    std::unique_ptr<Geometry> geom = reader.read(R"({
      "type": "FeatureCollection",
      "features": [
        {
          "type": "Feature",
          "geometry": {"type": "Point", "coordinates": [1, 2]},
          "properties": {}
        },
        {
          "type": "Feature",
          "geometry": {"type": "LineString", "coordinates": [[0, 0], [5, 5]]},
          "properties": null
        },
        {
          "type": "Feature",
          "geometry": {"type": "Polygon", "coordinates": [[[0,0],[2,0],[2,2],[0,2],[0,0]]]},
          "properties": {"a": 1}
        }
      ]
    })");
    const auto* gc = dynamic_cast<const GeometryCollection*>(geom.get());
    ASSERT_NE(gc, nullptr);
    ASSERT_EQ(gc->getNumGeometries(), 3u);
    EXPECT_EQ(gc->getGeometryN(0)->getGeometryTypeId(), geos::geom::GEOS_POINT);
    EXPECT_EQ(gc->getGeometryN(1)->getGeometryTypeId(),
              geos::geom::GEOS_LINESTRING);
    EXPECT_EQ(gc->getGeometryN(2)->getGeometryTypeId(),
              geos::geom::GEOS_POLYGON);
  }
}

TEST(GeoJSONReaderTest,
     ReadFeatureAndFeatureCollectionErrorPathsThrowParseException) {
  GeoJSONReader reader;

  EXPECT_THROW(reader.read(R"({"type":"Feature","properties":{}})"),
               ParseException);
  EXPECT_THROW(
      reader.read(R"({"type":"Feature","geometry":null,"properties":{}})"),
      ParseException);
  EXPECT_THROW(
      reader.read(
          R"({"type":"Feature","geometry":{"type":"Unknown"},"properties":{}})"),
      ParseException);
  EXPECT_THROW(reader.read(R"({"type":"FeatureCollection"})"), ParseException);
  EXPECT_THROW(
      reader.read(R"({"type":"FeatureCollection","features":"not-an-array"})"),
      ParseException);
  EXPECT_THROW(
      reader.read(
          R"({"type":"FeatureCollection","features":[{"type":"Feature","geometry":null,"properties":{}}]})"),
      ParseException);
}

TEST(GeoJSONReaderTest, ReadJsonAndTypeErrorPathsThrowParseException) {
  GeoJSONReader reader;

  const std::vector<std::string> invalid_inputs = {
      "",
      "{",
      "not json",
      "null",
      "42",
      "[]",
      R"("Point")",
      "{}",
      R"({"type": 123})",
      R"({"type": "UnknownType", "coordinates": [0, 0]})",
      R"({"type": "Point"})",
      R"({"type": "Point", "coordinates": "invalid"})",
      R"({"type": "Point", "coordinates": ["x", "y"]})",
      R"({"type": "LineString"})",
      R"({"type": "Polygon"})",
      R"({"type": "MultiPoint"})",
      R"({"type": "MultiLineString"})",
      R"({"type": "MultiPolygon"})",
  };

  for (const std::string& input : invalid_inputs) {
    EXPECT_THROW(reader.read(input), ParseException) << "input: " << input;
  }
}

TEST(GeoJSONReaderTest, ReadFeaturesFromBareGeometryProducesSingleFeature) {
  GeoJSONReader reader;

  GeoJSONFeatureCollection fc =
      reader.readFeatures(R"({"type":"Point","coordinates":[9.0, -3.0]})");
  ASSERT_EQ(fc.getFeatures().size(), 1u);
  const GeoJSONFeature& feature = fc.getFeatures()[0];
  ASSERT_NE(feature.getGeometry(), nullptr);
  EXPECT_EQ(feature.getGeometry()->getGeometryTypeId(), geos::geom::GEOS_POINT);
  const auto* pt = dynamic_cast<const Point*>(feature.getGeometry());
  ASSERT_NE(pt, nullptr);
  EXPECT_DOUBLE_EQ(pt->getX(), 9.0);
  EXPECT_DOUBLE_EQ(pt->getY(), -3.0);
  EXPECT_TRUE(feature.getProperties().empty());
  EXPECT_EQ(feature.getId(), "");
}

TEST(GeoJSONReaderTest, ReadFeaturesAllPropertyValueTypesAndNestedStructures) {
  GeoJSONReader reader;

  GeoJSONFeatureCollection fc = reader.readFeatures(R"({
    "type": "Feature",
    "id": "props-feature",
    "geometry": {"type": "Point", "coordinates": [0, 0]},
    "properties": {
      "str": "hello",
      "int_num": 42,
      "neg_float": -3.5,
      "bool_true": true,
      "bool_false": false,
      "null_val": null,
      "empty_arr": [],
      "mixed_arr": ["x", 7, false, null, [1, 2], {"inner": "yes"}],
      "empty_obj": {},
      "nested_obj": {
        "count": 5,
        "flags": [true],
        "sub": {"k": "v"},
        "nil": null
      }
    }
  })");

  ASSERT_EQ(fc.getFeatures().size(), 1u);
  const GeoJSONFeature& feature = fc.getFeatures()[0];
  EXPECT_EQ(feature.getId(), "props-feature");

  const std::map<std::string, GeoJSONValue>& props = feature.getProperties();
  ASSERT_EQ(props.size(), 10u);

  ASSERT_TRUE(props.contains("str"));
  EXPECT_TRUE(props.at("str").isString());
  EXPECT_EQ(props.at("str").getString(), "hello");

  ASSERT_TRUE(props.contains("int_num"));
  EXPECT_TRUE(props.at("int_num").isNumber());
  EXPECT_DOUBLE_EQ(props.at("int_num").getNumber(), 42.0);

  ASSERT_TRUE(props.contains("neg_float"));
  EXPECT_TRUE(props.at("neg_float").isNumber());
  EXPECT_DOUBLE_EQ(props.at("neg_float").getNumber(), -3.5);

  ASSERT_TRUE(props.contains("bool_true"));
  EXPECT_TRUE(props.at("bool_true").isBoolean());
  EXPECT_TRUE(props.at("bool_true").getBoolean());

  ASSERT_TRUE(props.contains("bool_false"));
  EXPECT_TRUE(props.at("bool_false").isBoolean());
  EXPECT_FALSE(props.at("bool_false").getBoolean());

  ASSERT_TRUE(props.contains("null_val"));
  EXPECT_TRUE(props.at("null_val").isNull());
  EXPECT_EQ(props.at("null_val").getNull(), nullptr);

  ASSERT_TRUE(props.contains("empty_arr"));
  EXPECT_TRUE(props.at("empty_arr").isArray());
  EXPECT_TRUE(props.at("empty_arr").getArray().empty());

  ASSERT_TRUE(props.contains("mixed_arr"));
  EXPECT_TRUE(props.at("mixed_arr").isArray());
  const std::vector<GeoJSONValue>& arr = props.at("mixed_arr").getArray();
  ASSERT_EQ(arr.size(), 6u);
  EXPECT_TRUE(arr[0].isString());
  EXPECT_EQ(arr[0].getString(), "x");
  EXPECT_TRUE(arr[1].isNumber());
  EXPECT_DOUBLE_EQ(arr[1].getNumber(), 7.0);
  EXPECT_TRUE(arr[2].isBoolean());
  EXPECT_FALSE(arr[2].getBoolean());
  EXPECT_TRUE(arr[3].isNull());
  ASSERT_TRUE(arr[4].isArray());
  ASSERT_EQ(arr[4].getArray().size(), 2u);
  EXPECT_DOUBLE_EQ(arr[4].getArray()[0].getNumber(), 1.0);
  EXPECT_DOUBLE_EQ(arr[4].getArray()[1].getNumber(), 2.0);
  ASSERT_TRUE(arr[5].isObject());
  ASSERT_TRUE(arr[5].getObject().contains("inner"));
  EXPECT_EQ(arr[5].getObject().at("inner").getString(), "yes");

  ASSERT_TRUE(props.contains("empty_obj"));
  EXPECT_TRUE(props.at("empty_obj").isObject());
  EXPECT_TRUE(props.at("empty_obj").getObject().empty());

  ASSERT_TRUE(props.contains("nested_obj"));
  EXPECT_TRUE(props.at("nested_obj").isObject());
  const std::map<std::string, GeoJSONValue>& obj =
      props.at("nested_obj").getObject();
  ASSERT_EQ(obj.size(), 4u);
  ASSERT_TRUE(obj.contains("count"));
  EXPECT_DOUBLE_EQ(obj.at("count").getNumber(), 5.0);
  ASSERT_TRUE(obj.contains("flags"));
  ASSERT_TRUE(obj.at("flags").isArray());
  ASSERT_EQ(obj.at("flags").getArray().size(), 1u);
  EXPECT_TRUE(obj.at("flags").getArray()[0].getBoolean());
  ASSERT_TRUE(obj.contains("sub"));
  ASSERT_TRUE(obj.at("sub").isObject());
  ASSERT_TRUE(obj.at("sub").getObject().contains("k"));
  EXPECT_EQ(obj.at("sub").getObject().at("k").getString(), "v");
  ASSERT_TRUE(obj.contains("nil"));
  EXPECT_TRUE(obj.at("nil").isNull());
}

TEST(GeoJSONReaderTest, ReadFeaturesFeatureIdVariantsAndNullProperties) {
  GeoJSONReader reader;

  // Missing "id", empty properties.
  {
    GeoJSONFeatureCollection fc = reader.readFeatures(
        R"({"type":"Feature","geometry":{"type":"Point","coordinates":[0,0]},"properties":{}})");
    ASSERT_EQ(fc.getFeatures().size(), 1u);
    EXPECT_EQ(fc.getFeatures()[0].getId(), "");
    EXPECT_TRUE(fc.getFeatures()[0].getProperties().empty());
  }

  // Null "id" and null "properties".
  {
    GeoJSONFeatureCollection fc = reader.readFeatures(
        R"({"type":"Feature","id":null,"geometry":{"type":"Point","coordinates":[0,0]},"properties":null})");
    ASSERT_EQ(fc.getFeatures().size(), 1u);
    EXPECT_EQ(fc.getFeatures()[0].getId(), "");
    EXPECT_TRUE(fc.getFeatures()[0].getProperties().empty());
  }

  // String "id".
  {
    GeoJSONFeatureCollection fc = reader.readFeatures(
        R"({"type":"Feature","id":"abc-123","geometry":{"type":"Point","coordinates":[0,0]},"properties":{}})");
    ASSERT_EQ(fc.getFeatures().size(), 1u);
    EXPECT_EQ(fc.getFeatures()[0].getId(), "abc-123");
  }

  // Integer and floating-point numeric "id".
  {
    GeoJSONFeatureCollection fc = reader.readFeatures(
        R"({"type":"Feature","id":12345,"geometry":{"type":"Point","coordinates":[0,0]},"properties":{}})");
    ASSERT_EQ(fc.getFeatures().size(), 1u);
    EXPECT_EQ(fc.getFeatures()[0].getId(), "12345");
  }
  {
    GeoJSONFeatureCollection fc = reader.readFeatures(
        R"({"type":"Feature","id":12.5,"geometry":{"type":"Point","coordinates":[0,0]},"properties":{}})");
    ASSERT_EQ(fc.getFeatures().size(), 1u);
    EXPECT_EQ(fc.getFeatures()[0].getId(), "12.5");
  }

  // Non-string, non-number, non-null "id" is ignored (remains empty).
  {
    GeoJSONFeatureCollection fc = reader.readFeatures(
        R"({"type":"Feature","id":true,"geometry":{"type":"Point","coordinates":[0,0]},"properties":{}})");
    ASSERT_EQ(fc.getFeatures().size(), 1u);
    EXPECT_EQ(fc.getFeatures()[0].getId(), "");
  }
}

TEST(GeoJSONReaderTest, ReadFeaturesFromFeatureCollectionEmptyAndMultiple) {
  GeoJSONReader reader;

  // Empty FeatureCollection.
  {
    GeoJSONFeatureCollection fc =
        reader.readFeatures(R"({"type":"FeatureCollection","features":[]})");
    EXPECT_TRUE(fc.getFeatures().empty());
  }

  // Multiple features with heterogeneous geometries, ids, and properties.
  {
    GeoJSONFeatureCollection fc = reader.readFeatures(R"({
      "type": "FeatureCollection",
      "features": [
        {
          "type": "Feature",
          "id": 1,
          "geometry": {"type": "Point", "coordinates": [1, 2]},
          "properties": {"kind": "point"}
        },
        {
          "type": "Feature",
          "id": "line-2",
          "geometry": {"type": "LineString", "coordinates": [[0, 0], [4, 4]]},
          "properties": {"length": 4}
        }
      ]
    })");

    ASSERT_EQ(fc.getFeatures().size(), 2u);

    const GeoJSONFeature& f0 = fc.getFeatures()[0];
    EXPECT_EQ(f0.getId(), "1");
    ASSERT_NE(f0.getGeometry(), nullptr);
    EXPECT_EQ(f0.getGeometry()->getGeometryTypeId(), geos::geom::GEOS_POINT);
    ASSERT_TRUE(f0.getProperties().contains("kind"));
    EXPECT_EQ(f0.getProperties().at("kind").getString(), "point");

    const GeoJSONFeature& f1 = fc.getFeatures()[1];
    EXPECT_EQ(f1.getId(), "line-2");
    ASSERT_NE(f1.getGeometry(), nullptr);
    EXPECT_EQ(f1.getGeometry()->getGeometryTypeId(),
              geos::geom::GEOS_LINESTRING);
    ASSERT_TRUE(f1.getProperties().contains("length"));
    EXPECT_DOUBLE_EQ(f1.getProperties().at("length").getNumber(), 4.0);
  }
}

TEST(GeoJSONReaderTest, ReadFeaturesErrorPathsThrowParseException) {
  GeoJSONReader reader;

  const std::vector<std::string> invalid_inputs = {
      "",
      "{not json",
      "{}",
      R"({"type": 99})",
      R"({"type": "BogusGeometry", "coordinates": [0, 0]})",
      R"({"type": "Point", "coordinates": [1.0]})",
      R"({"type": "Feature", "geometry": {"type": "Point", "coordinates": [0, 0]}})",
      R"({"type": "Feature", "properties": {}})",
      R"({"type": "Feature", "geometry": null, "properties": {}})",
      R"({"type": "FeatureCollection"})",
      R"({"type": "FeatureCollection", "features": "not-an-array"})",
      R"({"type": "FeatureCollection", "features": [null]})",
  };

  for (const std::string& input : invalid_inputs) {
    EXPECT_THROW(reader.readFeatures(input), ParseException)
        << "input: " << input;
  }
}

}  // namespace
}  // namespace geos_google
