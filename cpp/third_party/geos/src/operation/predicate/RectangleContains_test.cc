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

#include "third_party/geos/include/geos/operation/predicate/RectangleContains.h"

#include <memory>
#include <string>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
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
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::predicate::RectangleContains;

static_assert(!std::is_copy_constructible_v<RectangleContains>);
static_assert(!std::is_copy_assignable_v<RectangleContains>);

TEST(RectangleContainsTest, EmptyGeometriesAndEmptyRectangleReturnFalse) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto rect = reader.read<Polygon>("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_TRUE(rect->isRectangle());

  auto empty_pt = factory->createPoint();
  auto empty_ls = factory->createLineString();
  auto empty_poly = factory->createPolygon();
  auto empty_mpt = factory->createMultiPoint();
  auto empty_mls = factory->createMultiLineString();
  auto empty_mpoly = factory->createMultiPolygon();
  auto empty_gc = factory->createGeometryCollection();

  EXPECT_FALSE(RectangleContains::contains(*rect, *empty_pt));
  EXPECT_FALSE(RectangleContains::contains(*rect, *empty_ls));
  EXPECT_FALSE(RectangleContains::contains(*rect, *empty_poly));
  EXPECT_FALSE(RectangleContains::contains(*rect, *empty_mpt));
  EXPECT_FALSE(RectangleContains::contains(*rect, *empty_mls));
  EXPECT_FALSE(RectangleContains::contains(*rect, *empty_mpoly));
  EXPECT_FALSE(RectangleContains::contains(*rect, *empty_gc));

  auto empty_rect = factory->createPolygon();
  auto pt_inside = reader.read("POINT (5 5)");
  EXPECT_FALSE(RectangleContains::contains(*empty_rect, *pt_inside));
  EXPECT_FALSE(RectangleContains::contains(*empty_rect, *empty_pt));
}

TEST(RectangleContainsTest, PointsInteriorBoundaryCornersAndExterior) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto rect = reader.read<Polygon>("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  RectangleContains rc(*rect);

  // Strictly interior points.
  for (const std::string& wkt : {"POINT (5 5)", "POINT (0.001 9.999)"}) {
    auto geom = reader.read(wkt);
    EXPECT_TRUE(RectangleContains::contains(*rect, *geom)) << wkt;
    EXPECT_TRUE(rc.contains(*geom)) << wkt;
    EXPECT_TRUE(rect->contains(geom.get())) << wkt;
  }

  // Points on each of the four boundary edges and corners.
  for (const std::string& wkt : {
           "POINT (0 5)",    // minX
           "POINT (10 5)",   // maxX
           "POINT (5 0)",    // minY
           "POINT (5 10)",   // maxY
           "POINT (0 0)",    // minX & minY corner
           "POINT (10 0)",   // maxX & minY corner
           "POINT (10 10)",  // maxX & maxY corner
           "POINT (0 10)",   // minX & maxY corner
       }) {
    auto geom = reader.read(wkt);
    EXPECT_FALSE(RectangleContains::contains(*rect, *geom)) << wkt;
    EXPECT_FALSE(rc.contains(*geom)) << wkt;
    EXPECT_FALSE(rect->contains(geom.get())) << wkt;
  }

  // Points strictly outside the rectangle envelope.
  for (const std::string& wkt : {
           "POINT (-1 5)",
           "POINT (11 5)",
           "POINT (5 -1)",
           "POINT (5 11)",
       }) {
    auto geom = reader.read(wkt);
    EXPECT_FALSE(RectangleContains::contains(*rect, *geom)) << wkt;
    EXPECT_FALSE(rc.contains(*geom)) << wkt;
  }
}

TEST(RectangleContainsTest, LineStringsBoundaryInteriorCrossingAndOutside) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto rect = reader.read<Polygon>("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  RectangleContains rc(*rect);

  // LineStrings that enter the rectangle interior must return true.
  for (const std::string& wkt : {
           "LINESTRING (2 5, 8 5)",        // horizontal strictly inside
           "LINESTRING (5 2, 5 8)",        // vertical strictly inside
           "LINESTRING (2 2, 8 8)",        // diagonal strictly inside
           "LINESTRING (0 5, 10 5)",       // horizontal between opposite edges
           "LINESTRING (5 0, 5 10)",       // vertical between opposite edges
           "LINESTRING (0 0, 10 10)",      // corner-to-corner diagonal
           "LINESTRING (0 5, 5 0)",        // adjacent-edge chord
           "LINESTRING (0 0, 10 0, 5 5)",  // boundary segment then interior
           "LINESTRING (5 5, 10 0, 10 10)",  // interior segment then boundary
       }) {
    auto geom = reader.read(wkt);
    EXPECT_TRUE(RectangleContains::contains(*rect, *geom)) << wkt;
    EXPECT_TRUE(rc.contains(*geom)) << wkt;
    EXPECT_TRUE(rect->contains(geom.get())) << wkt;
  }

  // LineStrings wholly on the rectangle boundary must return false.
  for (const std::string& wkt : {
           "LINESTRING (0 2, 0 8)",                     // minX edge
           "LINESTRING (10 2, 10 8)",                   // maxX edge
           "LINESTRING (2 0, 8 0)",                     // minY edge
           "LINESTRING (2 10, 8 10)",                   // maxY edge
           "LINESTRING (0 0, 10 0, 10 10)",             // two boundary edges
           "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)",  // full boundary
       }) {
    auto geom = reader.read(wkt);
    EXPECT_FALSE(RectangleContains::contains(*rect, *geom)) << wkt;
    EXPECT_FALSE(rc.contains(*geom)) << wkt;
    EXPECT_FALSE(rect->contains(geom.get())) << wkt;
  }

  // LineStrings extending outside the rectangle envelope must return false.
  for (const std::string& wkt : {
           "LINESTRING (5 5, 12 5)",
           "LINESTRING (-1 0, 5 0)",
           "LINESTRING (-2 -2, 12 12)",
       }) {
    auto geom = reader.read(wkt);
    EXPECT_FALSE(RectangleContains::contains(*rect, *geom)) << wkt;
    EXPECT_FALSE(rc.contains(*geom)) << wkt;
  }
}

TEST(RectangleContainsTest, DegenerateZeroLengthLineSegments) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto rect = reader.read<Polygon>("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  auto boundary_degenerate = factory->createLineString(
      CoordinateSequence{CoordinateXY{0, 5}, CoordinateXY{0, 5}});
  auto interior_degenerate = factory->createLineString(
      CoordinateSequence{CoordinateXY{5, 5}, CoordinateXY{5, 5}});
  auto repeated_boundary = factory->createLineString(CoordinateSequence{
      CoordinateXY{0, 0}, CoordinateXY{0, 0}, CoordinateXY{10, 0}});
  auto repeated_interior = factory->createLineString(CoordinateSequence{
      CoordinateXY{0, 0}, CoordinateXY{5, 5}, CoordinateXY{5, 5}});

  EXPECT_FALSE(RectangleContains::contains(*rect, *boundary_degenerate));
  EXPECT_TRUE(RectangleContains::contains(*rect, *interior_degenerate));
  EXPECT_FALSE(RectangleContains::contains(*rect, *repeated_boundary));
  EXPECT_TRUE(RectangleContains::contains(*rect, *repeated_interior));
}

TEST(RectangleContainsTest, PolygonsAndLinearRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto rect = reader.read<Polygon>("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  RectangleContains rc(*rect);

  // Polygons inside or equal to the rectangle envelope are always contained.
  for (const std::string& wkt : {
           "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
           "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
           "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))",
           "POLYGON ((1 1, 9 1, 9 9, 1 9, 1 1), (3 3, 7 3, 7 7, 3 7, 3 3))",
       }) {
    auto poly = reader.read(wkt);
    EXPECT_TRUE(RectangleContains::contains(*rect, *poly)) << wkt;
    EXPECT_TRUE(rc.contains(*poly)) << wkt;
    EXPECT_TRUE(rect->contains(poly.get())) << wkt;
  }

  // Polygon extending outside the rectangle envelope is not contained.
  auto outside_poly = reader.read("POLYGON ((5 5, 12 5, 12 8, 5 8, 5 5))");
  EXPECT_FALSE(rc.contains(*outside_poly));

  // LinearRing is a LineString subclass: interior ring is true, boundary is
  // false.
  auto interior_ring = factory->createLinearRing(CoordinateSequence{
      CoordinateXY{2, 2}, CoordinateXY{8, 2}, CoordinateXY{8, 8},
      CoordinateXY{2, 8}, CoordinateXY{2, 2}});
  auto boundary_ring = factory->createLinearRing(CoordinateSequence{
      CoordinateXY{0, 0}, CoordinateXY{10, 0}, CoordinateXY{10, 10},
      CoordinateXY{0, 10}, CoordinateXY{0, 0}});
  EXPECT_TRUE(rc.contains(*interior_ring));
  EXPECT_FALSE(rc.contains(*boundary_ring));
}

TEST(RectangleContainsTest, MultiGeometriesAndNestedCollections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto rect = reader.read<Polygon>("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  RectangleContains rc(*rect);

  // MultiPoint: all boundary -> false; mixed boundary + interior -> true;
  // outside -> false.
  EXPECT_FALSE(rc.contains(*reader.read("MULTIPOINT ((0 0), (10 5), (5 10))")));
  EXPECT_TRUE(rc.contains(*reader.read("MULTIPOINT ((0 0), (5 5))")));
  EXPECT_TRUE(rc.contains(*reader.read("MULTIPOINT ((5 5), (10 10))")));
  EXPECT_FALSE(rc.contains(*reader.read("MULTIPOINT ((5 5), (11 5))")));

  // MultiLineString: all boundary -> false; one boundary + one interior ->
  // true.
  EXPECT_FALSE(rc.contains(
      *reader.read("MULTILINESTRING ((0 0, 0 10), (10 0, 10 10))")));
  EXPECT_TRUE(
      rc.contains(*reader.read("MULTILINESTRING ((0 0, 0 10), (2 5, 8 5))")));

  // MultiPolygon: inside -> true; one outside -> false.
  EXPECT_TRUE(
      rc.contains(*reader.read("MULTIPOLYGON (((1 1, 4 1, 4 4, 1 4, 1 1)), "
                               "((6 6, 9 6, 9 9, 6 9, 6 6)))")));
  EXPECT_FALSE(
      rc.contains(*reader.read("MULTIPOLYGON (((1 1, 4 1, 4 4, 1 4, 1 1)), "
                               "((6 6, 12 6, 12 9, 6 9, 6 6)))")));

  // Homogeneous and nested GeometryCollections.
  EXPECT_FALSE(rc.contains(*reader.read(
      "GEOMETRYCOLLECTION (POINT (0 5), LINESTRING (0 0, 10 0))")));
  EXPECT_TRUE(
      rc.contains(*reader.read("GEOMETRYCOLLECTION (POINT (0 5), "
                               "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2)))")));
  EXPECT_FALSE(rc.contains(*reader.read(
      "GEOMETRYCOLLECTION ("
      "GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (10 0, 10 10)), "
      "POINT (5 10))")));
  EXPECT_TRUE(
      rc.contains(*reader.read("GEOMETRYCOLLECTION ("
                               "GEOMETRYCOLLECTION (POINT (0 0), POINT (5 5)), "
                               "LINESTRING (0 10, 10 10))")));
}

TEST(RectangleContainsTest, ReusedInstanceWithNegativeAndFractionalBounds) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto rect = reader.read<Polygon>(
      "POLYGON ((-5.5 -2.5, 4.5 -2.5, 4.5 7.5, -5.5 7.5, -5.5 -2.5))");
  ASSERT_TRUE(rect->isRectangle());
  RectangleContains rc(*rect);

  EXPECT_TRUE(rc.contains(*reader.read("POINT (0 0)")));
  EXPECT_FALSE(rc.contains(*reader.read("POINT (-5.5 0)")));
  EXPECT_FALSE(rc.contains(*reader.read("LINESTRING (-5.5 -2.5, 4.5 -2.5)")));
  EXPECT_TRUE(rc.contains(*reader.read("LINESTRING (-5.5 2.5, 4.5 2.5)")));
  EXPECT_FALSE(rc.contains(*reader.read("POINT (4.5001 2.5)")));
}

}  // namespace
}  // namespace geos_google
