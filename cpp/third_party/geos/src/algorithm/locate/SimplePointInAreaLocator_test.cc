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

#include "third_party/geos/include/geos/algorithm/locate/SimplePointInAreaLocator.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/locate/PointOnGeometryLocator.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/Surface.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::algorithm::locate::PointOnGeometryLocator;
using geos::algorithm::locate::SimplePointInAreaLocator;
using geos::geom::Coordinate;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::CurvePolygon;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::geom::Surface;
using geos::io::WKTReader;

class SimplePointInAreaLocatorTest : public ::testing::Test {
 protected:
  SimplePointInAreaLocatorTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  template <typename T>
  std::unique_ptr<T> ReadWktAs(const std::string& wkt) const {
    return reader_.read<T>(wkt);
  }

  void ExpectLocation(const Geometry& geom, const CoordinateXY& pt,
                      Location expected) const {
    EXPECT_EQ(SimplePointInAreaLocator::locate(pt, &geom), expected)
        << "at (" << pt.x << ", " << pt.y << ")";
    EXPECT_EQ(SimplePointInAreaLocator::isContained(pt, &geom),
              expected != Location::EXTERIOR)
        << "at (" << pt.x << ", " << pt.y << ")";
    SimplePointInAreaLocator ptr_locator(&geom);
    EXPECT_EQ(ptr_locator.locate(&pt), expected)
        << "at (" << pt.x << ", " << pt.y << ")";
    SimplePointInAreaLocator ref_locator(geom);
    EXPECT_EQ(ref_locator.locate(&pt), expected)
        << "at (" << pt.x << ", " << pt.y << ")";
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(SimplePointInAreaLocatorTest, SimplePolygonInteriorBoundaryExterior) {
  auto poly = ReadWktAs<Polygon>("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  // Interior points
  ExpectLocation(*poly, CoordinateXY(5, 5), Location::INTERIOR);
  ExpectLocation(*poly, CoordinateXY(0.001, 9.999), Location::INTERIOR);
  EXPECT_EQ(
      SimplePointInAreaLocator::locatePointInSurface(CoordinateXY(5, 5), *poly),
      Location::INTERIOR);

  // Boundary vertices and edge midpoints
  for (const auto& pt : std::vector<CoordinateXY>{{0, 0},
                                                  {10, 0},
                                                  {10, 10},
                                                  {0, 10},
                                                  {5, 0},
                                                  {10, 5},
                                                  {5, 10},
                                                  {0, 5}}) {
    ExpectLocation(*poly, pt, Location::BOUNDARY);
    EXPECT_EQ(SimplePointInAreaLocator::locatePointInSurface(pt, *poly),
              Location::BOUNDARY);
  }

  // Exterior points outside the bounding box
  for (const auto& pt : std::vector<CoordinateXY>{
           {-1, 5}, {11, 5}, {5, -1}, {5, 11}, {20, 20}}) {
    ExpectLocation(*poly, pt, Location::EXTERIOR);
    EXPECT_EQ(SimplePointInAreaLocator::locatePointInSurface(pt, *poly),
              Location::EXTERIOR);
  }
}

TEST_F(SimplePointInAreaLocatorTest, ConcavePolygonInEnvelopeButExterior) {
  // L-shaped polygon whose envelope [0,10]x[0,10] covers (7,7) outside the
  // shell.
  auto poly =
      ReadWktAs<Polygon>("POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))");

  ExpectLocation(*poly, CoordinateXY(2, 2), Location::INTERIOR);
  ExpectLocation(*poly, CoordinateXY(8, 2), Location::INTERIOR);
  ExpectLocation(*poly, CoordinateXY(2, 8), Location::INTERIOR);

  ExpectLocation(*poly, CoordinateXY(4, 4), Location::BOUNDARY);
  ExpectLocation(*poly, CoordinateXY(4, 7), Location::BOUNDARY);
  ExpectLocation(*poly, CoordinateXY(7, 4), Location::BOUNDARY);

  // Inside envelope [0,10]x[0,10], but outside shell ring
  ExpectLocation(*poly, CoordinateXY(7, 7), Location::EXTERIOR);
  ExpectLocation(*poly, CoordinateXY(4.1, 4.1), Location::EXTERIOR);
  EXPECT_EQ(
      SimplePointInAreaLocator::locatePointInSurface(CoordinateXY(7, 7), *poly),
      Location::EXTERIOR);
}

TEST_F(SimplePointInAreaLocatorTest, PolygonWithMultipleConvexAndConcaveHoles) {
  // Outer shell [0,20]x[0,20] with:
  // - Hole 0: L-shaped hole inside [2,10]x[2,10] leaving pocket [2,6]x(6,10]
  // - Hole 1: Square [3,5]x[7,9] tucked inside Hole 0's envelope pocket
  // - Hole 2: Disjoint square [14,18]x[14,18]
  auto poly = ReadWktAs<Polygon>(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 10 2, 10 10, 6 10, 6 6, 2 6, 2 2), "
      "(3 7, 5 7, 5 9, 3 9, 3 7), "
      "(14 14, 18 14, 18 18, 14 18, 14 14))");

  // Outside all hole envelopes
  ExpectLocation(*poly, CoordinateXY(1, 1), Location::INTERIOR);
  ExpectLocation(*poly, CoordinateXY(12, 12), Location::INTERIOR);

  // Inside Hole 0's envelope [2,10]x[2,10], exterior to Hole 0,
  // and outside Hole 1 & Hole 2 -> still interior to the polygon
  ExpectLocation(*poly, CoordinateXY(2.5, 8), Location::INTERIOR);

  // Inside Hole 0's envelope (exterior to Hole 0), so loop continues to Hole 1
  // where the point is strictly inside Hole 1 -> EXTERIOR
  ExpectLocation(*poly, CoordinateXY(4, 8), Location::EXTERIOR);
  // Same check, hitting Hole 1's boundary
  ExpectLocation(*poly, CoordinateXY(3, 8), Location::BOUNDARY);

  // Inside Hole 0 and Hole 2
  ExpectLocation(*poly, CoordinateXY(8, 4), Location::EXTERIOR);
  ExpectLocation(*poly, CoordinateXY(16, 16), Location::EXTERIOR);

  // On Hole 0 and Hole 2 boundaries (edges and vertices)
  ExpectLocation(*poly, CoordinateXY(2, 4), Location::BOUNDARY);
  ExpectLocation(*poly, CoordinateXY(6, 6), Location::BOUNDARY);
  ExpectLocation(*poly, CoordinateXY(6, 8), Location::BOUNDARY);
  ExpectLocation(*poly, CoordinateXY(14, 16), Location::BOUNDARY);
  ExpectLocation(*poly, CoordinateXY(18, 18), Location::BOUNDARY);
}

TEST_F(SimplePointInAreaLocatorTest, ReversedRingOrientationAndNegativeCoords) {
  // CW exterior shell with CCW hole and negative coordinates.
  auto poly = ReadWktAs<Polygon>(
      "POLYGON ((-10 -10, -10 10, 10 10, 10 -10, -10 -10), "
      "(-4 -4, 4 -4, 4 4, -4 4, -4 -4))");

  ExpectLocation(*poly, CoordinateXY(0, 0), Location::EXTERIOR);
  ExpectLocation(*poly, CoordinateXY(-4, 0), Location::BOUNDARY);
  ExpectLocation(*poly, CoordinateXY(-7, 7), Location::INTERIOR);
  ExpectLocation(*poly, CoordinateXY(10, 0), Location::BOUNDARY);
  ExpectLocation(*poly, CoordinateXY(-10.1, 0), Location::EXTERIOR);
}

TEST_F(SimplePointInAreaLocatorTest, CurvePolygonCircularAndCompoundRings) {
  // Circle centered at (10, 0) with radius 10.
  auto circle = ReadWktAs<CurvePolygon>(
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 10 10, 20 0, 10 -10, 0 0))");

  ExpectLocation(*circle, CoordinateXY(10, 0), Location::INTERIOR);
  ExpectLocation(*circle, CoordinateXY(10, 5), Location::INTERIOR);
  ExpectLocation(*circle, CoordinateXY(0, 0), Location::BOUNDARY);
  ExpectLocation(*circle, CoordinateXY(10, 10), Location::BOUNDARY);
  ExpectLocation(*circle, CoordinateXY(20, 0), Location::BOUNDARY);
  ExpectLocation(*circle, CoordinateXY(10, -10), Location::BOUNDARY);
  // (16 - 10)^2 + 8^2 = 36 + 64 = 100 = r^2 -> exact boundary point on arc
  ExpectLocation(*circle, CoordinateXY(16, 8), Location::BOUNDARY);
  // Inside bounding box [0,20]x[-10,10] but outside circle
  ExpectLocation(*circle, CoordinateXY(1, 9), Location::EXTERIOR);
  ExpectLocation(*circle, CoordinateXY(25, 0), Location::EXTERIOR);

  EXPECT_EQ(SimplePointInAreaLocator::locatePointInSurface(
                CoordinateXY(10, 0), static_cast<const Surface&>(*circle)),
            Location::INTERIOR);
  EXPECT_EQ(SimplePointInAreaLocator::locatePointInSurface(
                CoordinateXY(25, 0), static_cast<const Surface&>(*circle)),
            Location::EXTERIOR);

  // CompoundCurve shell (rectangle [0,20]x[0,10] topped by semicircle radius 10
  // centered at (10,10)) with both a circular hole and a linear hole.
  auto compound = ReadWktAs<CurvePolygon>(
      "CURVEPOLYGON ("
      "COMPOUNDCURVE ((0 0, 20 0, 20 10), CIRCULARSTRING (20 10, 10 20, 0 10), "
      "(0 10, 0 0)), "
      "CIRCULARSTRING (7 5, 10 8, 13 5, 10 2, 7 5), "
      "(2 2, 2 4, 4 4, 4 2, 2 2))");

  ExpectLocation(*compound, CoordinateXY(10, 15), Location::INTERIOR);
  ExpectLocation(*compound, CoordinateXY(15, 5), Location::INTERIOR);
  ExpectLocation(*compound, CoordinateXY(10, 20), Location::BOUNDARY);
  ExpectLocation(*compound, CoordinateXY(10, 0), Location::BOUNDARY);
  ExpectLocation(*compound, CoordinateXY(1, 19), Location::EXTERIOR);
  ExpectLocation(*compound, CoordinateXY(10, 5), Location::EXTERIOR);
  ExpectLocation(*compound, CoordinateXY(7, 5), Location::BOUNDARY);
  ExpectLocation(*compound, CoordinateXY(3, 3), Location::EXTERIOR);
  ExpectLocation(*compound, CoordinateXY(2, 3), Location::BOUNDARY);
}

TEST_F(SimplePointInAreaLocatorTest, MultiPolygonAndMultiSurface) {
  // Single-element MultiPolygon: numGeometries == 1, type != GEOS_POLYGON.
  auto single_mp = ReadWkt("MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)))");
  ExpectLocation(*single_mp, CoordinateXY(5, 5), Location::INTERIOR);
  ExpectLocation(*single_mp, CoordinateXY(0, 5), Location::BOUNDARY);
  ExpectLocation(*single_mp, CoordinateXY(15, 5), Location::EXTERIOR);

  // Multi-element MultiPolygon with disjoint components.
  auto multi_mp = ReadWkt(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((20 0, 30 0, 30 10, 20 10, 20 0)))");
  ExpectLocation(*multi_mp, CoordinateXY(5, 5), Location::INTERIOR);
  ExpectLocation(*multi_mp, CoordinateXY(10, 5), Location::BOUNDARY);
  ExpectLocation(*multi_mp, CoordinateXY(25, 5), Location::INTERIOR);
  ExpectLocation(*multi_mp, CoordinateXY(20, 5), Location::BOUNDARY);
  // In gap between components, inside overall envelope [0,30]x[0,10]
  ExpectLocation(*multi_mp, CoordinateXY(15, 5), Location::EXTERIOR);
  // Outside overall envelope
  ExpectLocation(*multi_mp, CoordinateXY(-5, 5), Location::EXTERIOR);

  // MultiPolygon touching at a single shared vertex.
  auto touching_mp = ReadWkt(
      "MULTIPOLYGON (((0 0, 10 5, 0 10, 0 0)), "
      "((10 5, 20 0, 20 10, 10 5)))");
  ExpectLocation(*touching_mp, CoordinateXY(10, 5), Location::BOUNDARY);
  ExpectLocation(*touching_mp, CoordinateXY(3, 5), Location::INTERIOR);
  ExpectLocation(*touching_mp, CoordinateXY(17, 5), Location::INTERIOR);
  ExpectLocation(*touching_mp, CoordinateXY(10, 9), Location::EXTERIOR);

  // Single-element MultiSurface: numGeometries == 1, type == GEOS_MULTISURFACE.
  auto single_ms = ReadWkt(
      "MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0, 10 10, 20 0, 10 -10, 0 "
      "0)))");
  ExpectLocation(*single_ms, CoordinateXY(10, 0), Location::INTERIOR);
  ExpectLocation(*single_ms, CoordinateXY(0, 0), Location::BOUNDARY);
  ExpectLocation(*single_ms, CoordinateXY(1, 9), Location::EXTERIOR);

  // Multi-element MultiSurface mixing Polygon and CurvePolygon.
  auto multi_ms = ReadWkt(
      "MULTISURFACE (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "CURVEPOLYGON (CIRCULARSTRING (20 0, 30 10, 40 0, 30 -10, 20 0)))");
  ExpectLocation(*multi_ms, CoordinateXY(5, 5), Location::INTERIOR);
  ExpectLocation(*multi_ms, CoordinateXY(30, 0), Location::INTERIOR);
  ExpectLocation(*multi_ms, CoordinateXY(20, 0), Location::BOUNDARY);
  ExpectLocation(*multi_ms, CoordinateXY(15, 5), Location::EXTERIOR);
}

TEST_F(SimplePointInAreaLocatorTest, GeometryCollectionsAndNestedCollections) {
  // Single-element GeometryCollection containing one Polygon.
  auto single_gc =
      ReadWkt("GEOMETRYCOLLECTION (POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))");
  ExpectLocation(*single_gc, CoordinateXY(5, 5), Location::INTERIOR);
  ExpectLocation(*single_gc, CoordinateXY(10, 5), Location::BOUNDARY);
  ExpectLocation(*single_gc, CoordinateXY(15, 5), Location::EXTERIOR);

  // Mixed-dimension GeometryCollection: 0D and 1D components must be ignored.
  auto mixed_gc = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (15 5), LINESTRING (0 5, 25 5), "
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))");
  ExpectLocation(*mixed_gc, CoordinateXY(15, 5), Location::EXTERIOR);
  ExpectLocation(*mixed_gc, CoordinateXY(22, 5), Location::EXTERIOR);
  ExpectLocation(*mixed_gc, CoordinateXY(5, 5), Location::INTERIOR);
  ExpectLocation(*mixed_gc, CoordinateXY(10, 5), Location::BOUNDARY);
  ExpectLocation(*mixed_gc, CoordinateXY(20, 8), Location::EXTERIOR);

  // Nested GeometryCollection with an empty element before a non-empty Polygon.
  auto nested_gc = ReadWkt(
      "GEOMETRYCOLLECTION (POLYGON EMPTY, "
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))), "
      "MULTIPOLYGON (((20 0, 30 0, 30 10, 20 10, 20 0))))");
  ExpectLocation(*nested_gc, CoordinateXY(5, 5), Location::INTERIOR);
  ExpectLocation(*nested_gc, CoordinateXY(25, 5), Location::INTERIOR);
  ExpectLocation(*nested_gc, CoordinateXY(30, 5), Location::BOUNDARY);
  ExpectLocation(*nested_gc, CoordinateXY(15, 5), Location::EXTERIOR);
}

TEST_F(SimplePointInAreaLocatorTest, NonArealGeometriesReturnExterior) {
  auto point = ReadWkt("POINT (5 5)");
  ExpectLocation(*point, CoordinateXY(5, 5), Location::EXTERIOR);

  auto multi_point = ReadWkt("MULTIPOINT ((0 0), (10 10))");
  ExpectLocation(*multi_point, CoordinateXY(0, 0), Location::EXTERIOR);
  ExpectLocation(*multi_point, CoordinateXY(5, 5), Location::EXTERIOR);

  auto line = ReadWkt("LINESTRING (0 0, 10 10)");
  ExpectLocation(*line, CoordinateXY(5, 5), Location::EXTERIOR);
  ExpectLocation(*line, CoordinateXY(2, 8), Location::EXTERIOR);

  // Closed LinearRing has dimension 1 (< 2), so interior is still EXTERIOR.
  auto ring = ReadWkt("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  ExpectLocation(*ring, CoordinateXY(5, 5), Location::EXTERIOR);
  ExpectLocation(*ring, CoordinateXY(0, 5), Location::EXTERIOR);

  auto multi_line = ReadWkt("MULTILINESTRING ((0 0, 10 0), (0 10, 10 10))");
  ExpectLocation(*multi_line, CoordinateXY(5, 0), Location::EXTERIOR);
  ExpectLocation(*multi_line, CoordinateXY(5, 5), Location::EXTERIOR);

  auto circular_string = ReadWkt("CIRCULARSTRING (0 0, 5 5, 10 0)");
  ExpectLocation(*circular_string, CoordinateXY(5, 2), Location::EXTERIOR);
  ExpectLocation(*circular_string, CoordinateXY(5, 5), Location::EXTERIOR);

  auto compound_curve = ReadWkt("COMPOUNDCURVE ((0 0, 10 0), (10 0, 10 10))");
  ExpectLocation(*compound_curve, CoordinateXY(5, 0), Location::EXTERIOR);
  ExpectLocation(*compound_curve, CoordinateXY(5, 5), Location::EXTERIOR);

  auto non_areal_gc =
      ReadWkt("GEOMETRYCOLLECTION (POINT (5 5), LINESTRING (0 0, 10 10))");
  ExpectLocation(*non_areal_gc, CoordinateXY(5, 5), Location::EXTERIOR);
}

TEST_F(SimplePointInAreaLocatorTest, EmptyGeometriesAndNonFiniteCoordinates) {
  auto empty_poly = ReadWktAs<Polygon>("POLYGON EMPTY");
  ExpectLocation(*empty_poly, CoordinateXY(0, 0), Location::EXTERIOR);
  EXPECT_EQ(SimplePointInAreaLocator::locatePointInSurface(CoordinateXY(0, 0),
                                                           *empty_poly),
            Location::EXTERIOR);

  auto empty_curve_poly = ReadWktAs<CurvePolygon>("CURVEPOLYGON EMPTY");
  ExpectLocation(*empty_curve_poly, CoordinateXY(0, 0), Location::EXTERIOR);
  EXPECT_EQ(
      SimplePointInAreaLocator::locatePointInSurface(
          CoordinateXY(0, 0), static_cast<const Surface&>(*empty_curve_poly)),
      Location::EXTERIOR);

  for (const std::string& wkt :
       {"POINT EMPTY", "LINESTRING EMPTY", "MULTIPOLYGON EMPTY",
        "MULTISURFACE EMPTY", "GEOMETRYCOLLECTION EMPTY"}) {
    auto geom = ReadWkt(wkt);
    ExpectLocation(*geom, CoordinateXY(0, 0), Location::EXTERIOR);
  }

  auto poly = ReadWktAs<Polygon>("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  for (const auto& pt : std::vector<CoordinateXY>{
           {geos::DoubleNotANumber, 5.0},
           {5.0, geos::DoubleNotANumber},
           {geos::DoubleNotANumber, geos::DoubleNotANumber},
           {geos::DoubleInfinity, 5.0},
           {5.0, geos::DoubleNegInfinity}}) {
    ExpectLocation(*poly, pt, Location::EXTERIOR);
    EXPECT_EQ(SimplePointInAreaLocator::locatePointInSurface(pt, *poly),
              Location::EXTERIOR);
  }
}

TEST_F(SimplePointInAreaLocatorTest,
       PolymorphicInterfaceAndExtendedCoordinateTypes) {
  auto poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  std::unique_ptr<PointOnGeometryLocator> ptr_locator =
      std::make_unique<SimplePointInAreaLocator>(poly.get());
  SimplePointInAreaLocator ref_concrete(*poly);
  PointOnGeometryLocator& ref_locator = ref_concrete;

  Coordinate interior_xyz(5.0, 5.0, 42.0);
  CoordinateXYM boundary_xym(10.0, 5.0, 99.0);
  CoordinateXYZM exterior_xyzm(15.0, 5.0, 10.0, 20.0);

  EXPECT_EQ(ptr_locator->locate(&interior_xyz), Location::INTERIOR);
  EXPECT_EQ(ptr_locator->locate(&boundary_xym), Location::BOUNDARY);
  EXPECT_EQ(ptr_locator->locate(&exterior_xyzm), Location::EXTERIOR);

  EXPECT_EQ(ref_locator.locate(&interior_xyz), Location::INTERIOR);
  EXPECT_EQ(ref_locator.locate(&boundary_xym), Location::BOUNDARY);
  EXPECT_EQ(ref_locator.locate(&exterior_xyzm), Location::EXTERIOR);

  EXPECT_EQ(SimplePointInAreaLocator::locate(interior_xyz, poly.get()),
            Location::INTERIOR);
  EXPECT_TRUE(SimplePointInAreaLocator::isContained(boundary_xym, poly.get()));
  EXPECT_FALSE(
      SimplePointInAreaLocator::isContained(exterior_xyzm, poly.get()));
}

TEST_F(SimplePointInAreaLocatorTest, HorizontalRayGrazingVerticesAndEdges) {
  // Chevron polygon whose horizontal line y=5 passes through cusp vertex (5,5),
  // and y=6 passes through top indent vertex (5,6).
  auto chevron = ReadWkt("POLYGON ((0 0, 5 5, 10 0, 10 10, 5 6, 0 10, 0 0))");

  ExpectLocation(*chevron, CoordinateXY(2, 5), Location::INTERIOR);
  ExpectLocation(*chevron, CoordinateXY(5, 5), Location::BOUNDARY);
  ExpectLocation(*chevron, CoordinateXY(8, 5), Location::INTERIOR);
  ExpectLocation(*chevron, CoordinateXY(5, 5.5), Location::INTERIOR);
  ExpectLocation(*chevron, CoordinateXY(5, 6), Location::BOUNDARY);
  ExpectLocation(*chevron, CoordinateXY(5, 7), Location::EXTERIOR);
  ExpectLocation(*chevron, CoordinateXY(5, 2), Location::EXTERIOR);

  // Polygon with horizontal step edges along y=5
  auto stepped = ReadWkt("POLYGON ((0 0, 10 0, 10 5, 6 5, 6 10, 0 10, 0 0))");
  ExpectLocation(*stepped, CoordinateXY(3, 5), Location::INTERIOR);
  ExpectLocation(*stepped, CoordinateXY(6, 5), Location::BOUNDARY);
  ExpectLocation(*stepped, CoordinateXY(8, 5), Location::BOUNDARY);
  ExpectLocation(*stepped, CoordinateXY(8, 7), Location::EXTERIOR);
}

}  // namespace
}  // namespace geos_google
