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

// Unit tests for
// third_party/geos/src/operation/relateng/AdjacentEdgeLocator.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/relateng/AdjacentEdgeLocator.cpp

#include "third_party/geos/include/geos/operation/relateng/AdjacentEdgeLocator.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::algorithm::Orientation;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::relateng::AdjacentEdgeLocator;

static_assert(!std::is_copy_constructible_v<AdjacentEdgeLocator>);
static_assert(!std::is_copy_assignable_v<AdjacentEdgeLocator>);

struct LocateCase {
  double x;
  double y;
  Location expected;
};

void CheckLocateCases(std::string_view wkt, std::span<const LocateCase> cases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  Geometry::Ptr geom = reader.read(std::string(wkt));
  ASSERT_NE(geom, nullptr);

  AdjacentEdgeLocator locator(geom.get());
  for (const auto& tc : cases) {
    SCOPED_TRACE(testing::Message()
                 << "point=(" << tc.x << ", " << tc.y << ") in " << wkt);
    const CoordinateXY pt(tc.x, tc.y);
    EXPECT_EQ(locator.locate(&pt), tc.expected);
  }
}

TEST(AdjacentEdgeLocatorTest, EmptyGeometries) {
  constexpr auto kEmptyWkts = std::to_array<std::string_view>({
      "POLYGON EMPTY",
      "MULTIPOLYGON EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
  });
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const CoordinateXY pt(0.0, 0.0);

  for (std::string_view wkt : kEmptyWkts) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr geom = reader.read(std::string(wkt));
    ASSERT_NE(geom, nullptr);
    AdjacentEdgeLocator locator(geom.get());
    EXPECT_EQ(locator.locate(&pt), Location::INTERIOR);
  }
}

TEST(AdjacentEdgeLocatorTest, NonPolygonalGeometriesAreIgnored) {
  constexpr auto kNonPolygonalWkts = std::to_array<std::string_view>({
      "POINT (1 1)",
      "LINESTRING (0 0, 10 0)",
  });
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const CoordinateXY pt(1.0, 1.0);

  for (std::string_view wkt : kNonPolygonalWkts) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr geom = reader.read(std::string(wkt));
    ASSERT_NE(geom, nullptr);
    AdjacentEdgeLocator locator(geom.get());
    EXPECT_EQ(locator.locate(&pt), Location::INTERIOR);
  }
}

TEST(AdjacentEdgeLocatorTest, SinglePolygonBothRingOrientations) {
  // CCW shell requires flipping to CW inside AdjacentEdgeLocator;
  // CW shell uses the underlying CoordinateSequence directly.
  constexpr std::string_view kCcwSquareWkt =
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))";
  constexpr std::string_view kCwSquareWkt =
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))";

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  {
    Geometry::Ptr ccw = reader.read(std::string(kCcwSquareWkt));
    const auto* poly = dynamic_cast<const Polygon*>(ccw.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_TRUE(
        Orientation::isCCW(poly->getExteriorRing()->getCoordinatesRO()));
  }
  {
    Geometry::Ptr cw = reader.read(std::string(kCwSquareWkt));
    const auto* poly = dynamic_cast<const Polygon*>(cw.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_FALSE(
        Orientation::isCCW(poly->getExteriorRing()->getCoordinatesRO()));
  }

  constexpr auto kCases = std::to_array<LocateCase>({
      // Start/closing vertex (hits i == 0 and i == size - 2).
      {.x = 0.0, .y = 0.0, .expected = Location::BOUNDARY},
      // Intermediate vertices.
      {.x = 10.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.x = 10.0, .y = 10.0, .expected = Location::BOUNDARY},
      {.x = 0.0, .y = 10.0, .expected = Location::BOUNDARY},
      // Edge midpoints (hits PointLocation::isOnSegment).
      {.x = 5.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.x = 10.0, .y = 5.0, .expected = Location::BOUNDARY},
      {.x = 5.0, .y = 10.0, .expected = Location::BOUNDARY},
      {.x = 0.0, .y = 5.0, .expected = Location::BOUNDARY},
  });

  CheckLocateCases(kCcwSquareWkt, kCases);
  CheckLocateCases(kCwSquareWkt, kCases);
}

TEST(AdjacentEdgeLocatorTest, PolygonWithCcwAndCwHoles) {
  // Shell is CCW (flipped); first hole is CW (flipped); second hole is CCW
  // (kept as-is).
  constexpr std::string_view kWkt =
      "POLYGON ("
      "(0 0, 10 0, 10 10, 0 10, 0 0), "
      "(1 1, 1 4, 4 4, 4 1, 1 1), "
      "(6 6, 9 6, 9 9, 6 9, 6 6)"
      ")";

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  Geometry::Ptr geom = reader.read(std::string(kWkt));
  const auto* poly = dynamic_cast<const Polygon*>(geom.get());
  ASSERT_NE(poly, nullptr);
  ASSERT_EQ(poly->getNumInteriorRing(), 2u);
  EXPECT_FALSE(
      Orientation::isCCW(poly->getInteriorRingN(0)->getCoordinatesRO()));
  EXPECT_TRUE(
      Orientation::isCCW(poly->getInteriorRingN(1)->getCoordinatesRO()));

  constexpr auto kCases = std::to_array<LocateCase>({
      // Shell vertex and edge.
      {.x = 0.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.x = 5.0, .y = 0.0, .expected = Location::BOUNDARY},
      // First hole (CW -> flipped): start vertex, middle vertex, edge midpoint.
      {.x = 1.0, .y = 1.0, .expected = Location::BOUNDARY},
      {.x = 4.0, .y = 4.0, .expected = Location::BOUNDARY},
      {.x = 2.5, .y = 1.0, .expected = Location::BOUNDARY},
      // Second hole (CCW -> unflipped): start vertex, middle vertex, midpoint.
      {.x = 6.0, .y = 6.0, .expected = Location::BOUNDARY},
      {.x = 9.0, .y = 9.0, .expected = Location::BOUNDARY},
      {.x = 7.5, .y = 9.0, .expected = Location::BOUNDARY},
  });

  CheckLocateCases(kWkt, kCases);
}

TEST(AdjacentEdgeLocatorTest, AdjacentSquaresInMultiPolygonAndCollection) {
  constexpr auto kCases = std::to_array<LocateCase>({
      // Shared edge interior becomes geometry interior under union semantics.
      {.x = 10.0, .y = 5.0, .expected = Location::INTERIOR},
      // Shared corners remain on the outer boundary.
      {.x = 10.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.x = 10.0, .y = 10.0, .expected = Location::BOUNDARY},
      // Unshared outer edges and vertices remain on the boundary.
      {.x = 0.0, .y = 5.0, .expected = Location::BOUNDARY},
      {.x = 20.0, .y = 5.0, .expected = Location::BOUNDARY},
      {.x = 5.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.x = 15.0, .y = 10.0, .expected = Location::BOUNDARY},
      {.x = 0.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.x = 20.0, .y = 10.0, .expected = Location::BOUNDARY},
  });

  // MultiPolygon with two CCW shells.
  CheckLocateCases(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((10 0, 20 0, 20 10, 10 10, 10 0)))",
      kCases);

  // GeometryCollection with mixed CW and CCW shells.
  CheckLocateCases(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0)), "
      "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))"
      ")",
      kCases);
}

TEST(AdjacentEdgeLocatorTest, DiagonalSharedEdgeBetweenTriangles) {
  constexpr std::string_view kWkt =
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 10, 0 0)), "
      "POLYGON ((0 0, 10 10, 0 10, 0 0))"
      ")";

  constexpr auto kCases = std::to_array<LocateCase>({
      // Points along the shared diagonal.
      {.x = 5.0, .y = 5.0, .expected = Location::INTERIOR},
      {.x = 2.5, .y = 2.5, .expected = Location::INTERIOR},
      // Outer corners and leg midpoints remain on the boundary.
      {.x = 0.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.x = 10.0, .y = 10.0, .expected = Location::BOUNDARY},
      {.x = 5.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.x = 0.0, .y = 5.0, .expected = Location::BOUNDARY},
  });

  CheckLocateCases(kWkt, kCases);
}

TEST(AdjacentEdgeLocatorTest, FourSquaresMeetingAtInteriorVertex) {
  constexpr std::string_view kWkt =
      "GEOMETRYCOLLECTION ("
      "POLYGON ((-1 -1, 0 -1, 0 0, -1 0, -1 -1)), "
      "POLYGON ((0 -1, 1 -1, 1 0, 0 0, 0 -1)), "
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "POLYGON ((-1 0, 0 0, 0 1, -1 1, -1 0))"
      ")";

  constexpr auto kCases = std::to_array<LocateCase>({
      // Center vertex completely enclosed by four incident polygons.
      {.x = 0.0, .y = 0.0, .expected = Location::INTERIOR},
      // Shared internal edge midpoints.
      {.x = 0.0, .y = 0.5, .expected = Location::INTERIOR},
      {.x = 0.0, .y = -0.5, .expected = Location::INTERIOR},
      {.x = 0.5, .y = 0.0, .expected = Location::INTERIOR},
      {.x = -0.5, .y = 0.0, .expected = Location::INTERIOR},
      // Outer boundary T-vertices and outer corners.
      {.x = 1.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.x = -1.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.x = 0.0, .y = 1.0, .expected = Location::BOUNDARY},
      {.x = 0.0, .y = -1.0, .expected = Location::BOUNDARY},
      {.x = 1.0, .y = 1.0, .expected = Location::BOUNDARY},
  });

  CheckLocateCases(kWkt, kCases);
}

TEST(AdjacentEdgeLocatorTest, TJunctionsBetweenAdjacentPolygons) {
  // Full T-junction: left [0,10]x[0,10] meets [10,20]x[0,5] and
  // [10,20]x[5,10].
  constexpr auto kFullTJunctionCases = std::to_array<LocateCase>({
      {.x = 10.0, .y = 5.0, .expected = Location::INTERIOR},
      {.x = 10.0, .y = 2.5, .expected = Location::INTERIOR},
      {.x = 10.0, .y = 7.5, .expected = Location::INTERIOR},
      {.x = 15.0, .y = 5.0, .expected = Location::INTERIOR},
      {.x = 20.0, .y = 5.0, .expected = Location::BOUNDARY},
      {.x = 10.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.x = 10.0, .y = 10.0, .expected = Location::BOUNDARY},
  });
  CheckLocateCases(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "POLYGON ((10 0, 20 0, 20 5, 10 5, 10 0)), "
      "POLYGON ((10 5, 20 5, 20 10, 10 10, 10 5))"
      ")",
      kFullTJunctionCases);

  // Partial T-junction: left [0,10]x[0,10] meets only lower-right
  // [10,20]x[0,5], leaving y > 5 along x = 10 on the boundary.
  constexpr auto kPartialTJunctionCases = std::to_array<LocateCase>({
      {.x = 10.0, .y = 2.5, .expected = Location::INTERIOR},
      {.x = 10.0, .y = 5.0, .expected = Location::BOUNDARY},
      {.x = 10.0, .y = 7.5, .expected = Location::BOUNDARY},
  });
  CheckLocateCases(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "POLYGON ((10 0, 20 0, 20 5, 10 5, 10 0))"
      ")",
      kPartialTJunctionCases);
}

TEST(AdjacentEdgeLocatorTest, PolygonInsideHoleWithAdjacentEdges) {
  // Outer polygon [0,10]x[0,10] with hole [2,8]x[2,8], plus inner polygon
  // [2,6]x[2,6] sharing the bottom-left corner and adjacent hole edges.
  constexpr std::string_view kWkt =
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 8, 8 8, 8 2, 2 2)), "
      "POLYGON ((2 2, 6 2, 6 6, 2 6, 2 2))"
      ")";

  constexpr auto kCases = std::to_array<LocateCase>({
      // Shared edges between outer hole and inner polygon.
      {.x = 2.0, .y = 4.0, .expected = Location::INTERIOR},
      {.x = 4.0, .y = 2.0, .expected = Location::INTERIOR},
      {.x = 2.0, .y = 2.0, .expected = Location::INTERIOR},
      // Corners where inner polygon ends and hole exterior remains exposed.
      {.x = 2.0, .y = 6.0, .expected = Location::BOUNDARY},
      {.x = 6.0, .y = 2.0, .expected = Location::BOUNDARY},
      // Unshared inner and hole edges.
      {.x = 6.0, .y = 4.0, .expected = Location::BOUNDARY},
      {.x = 8.0, .y = 5.0, .expected = Location::BOUNDARY},
  });

  CheckLocateCases(kWkt, kCases);
}

TEST(AdjacentEdgeLocatorTest, NestedMixedGeometryCollection) {
  // Verify recursive collection traversal and skipping of Point / LineString.
  constexpr std::string_view kWkt =
      "GEOMETRYCOLLECTION ("
      "POINT (5 5), "
      "LINESTRING (0 0, 20 10), "
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0))), "
      "GEOMETRYCOLLECTION ("
      "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0)), "
      "LINESTRING (10 0, 10 10)"
      ")"
      ")";

  constexpr auto kCases = std::to_array<LocateCase>({
      {.x = 10.0, .y = 5.0, .expected = Location::INTERIOR},
      {.x = 10.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.x = 0.0, .y = 5.0, .expected = Location::BOUNDARY},
      {.x = 20.0, .y = 5.0, .expected = Location::BOUNDARY},
  });

  CheckLocateCases(kWkt, kCases);
}

}  // namespace
}  // namespace geos_google
