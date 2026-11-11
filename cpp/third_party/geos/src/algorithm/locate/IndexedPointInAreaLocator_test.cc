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
// third_party/geos/src/algorithm/locate/IndexedPointInAreaLocator.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/locate/IndexedPointInAreaLocator.cpp

#include "third_party/geos/include/geos/algorithm/locate/IndexedPointInAreaLocator.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/locate/PointOnGeometryLocator.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::algorithm::locate::IndexedPointInAreaLocator;
using geos::algorithm::locate::PointOnGeometryLocator;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::util::IllegalArgumentException;

struct PointCase {
  std::string_view name;
  double x;
  double y;
  Location expected;
};

constexpr auto kSquareCases = std::to_array<PointCase>({
    {.name = "center", .x = 5.0, .y = 5.0, .expected = Location::INTERIOR},
    {.name = "near_lower_left",
     .x = 0.001,
     .y = 0.001,
     .expected = Location::INTERIOR},
    {.name = "near_upper_right",
     .x = 9.999,
     .y = 9.999,
     .expected = Location::INTERIOR},
    {.name = "vertex_0_0", .x = 0.0, .y = 0.0, .expected = Location::BOUNDARY},
    {.name = "vertex_10_0",
     .x = 10.0,
     .y = 0.0,
     .expected = Location::BOUNDARY},
    {.name = "vertex_10_10",
     .x = 10.0,
     .y = 10.0,
     .expected = Location::BOUNDARY},
    {.name = "vertex_0_10",
     .x = 0.0,
     .y = 10.0,
     .expected = Location::BOUNDARY},
    {.name = "bottom_edge", .x = 5.0, .y = 0.0, .expected = Location::BOUNDARY},
    {.name = "top_edge", .x = 5.0, .y = 10.0, .expected = Location::BOUNDARY},
    {.name = "left_edge", .x = 0.0, .y = 5.0, .expected = Location::BOUNDARY},
    {.name = "right_edge", .x = 10.0, .y = 5.0, .expected = Location::BOUNDARY},
    {.name = "outside_left",
     .x = -1.0,
     .y = 5.0,
     .expected = Location::EXTERIOR},
    {.name = "outside_right",
     .x = 11.0,
     .y = 5.0,
     .expected = Location::EXTERIOR},
    {.name = "outside_below",
     .x = 5.0,
     .y = -1.0,
     .expected = Location::EXTERIOR},
    {.name = "outside_above",
     .x = 5.0,
     .y = 11.0,
     .expected = Location::EXTERIOR},
    {.name = "collinear_left_bottom",
     .x = -5.0,
     .y = 0.0,
     .expected = Location::EXTERIOR},
    {.name = "collinear_right_bottom",
     .x = 15.0,
     .y = 0.0,
     .expected = Location::EXTERIOR},
    {.name = "collinear_left_top",
     .x = -5.0,
     .y = 10.0,
     .expected = Location::EXTERIOR},
    {.name = "collinear_right_top",
     .x = 15.0,
     .y = 10.0,
     .expected = Location::EXTERIOR},
});

constexpr auto kConcaveUCases = std::to_array<PointCase>({
    {.name = "left_arm", .x = 1.5, .y = 7.0, .expected = Location::INTERIOR},
    {.name = "notch_exterior",
     .x = 5.0,
     .y = 7.0,
     .expected = Location::EXTERIOR},
    {.name = "right_arm", .x = 8.5, .y = 7.0, .expected = Location::INTERIOR},
    {.name = "base_interior",
     .x = 5.0,
     .y = 2.0,
     .expected = Location::INTERIOR},
    {.name = "notch_bottom_ray_left",
     .x = 1.0,
     .y = 4.0,
     .expected = Location::INTERIOR},
    {.name = "notch_bottom_boundary",
     .x = 5.0,
     .y = 4.0,
     .expected = Location::BOUNDARY},
    {.name = "notch_bottom_ray_right",
     .x = 8.0,
     .y = 4.0,
     .expected = Location::INTERIOR},
    {.name = "notch_bottom_ray_outside_left",
     .x = -1.0,
     .y = 4.0,
     .expected = Location::EXTERIOR},
    {.name = "notch_bottom_ray_outside_right",
     .x = 11.0,
     .y = 4.0,
     .expected = Location::EXTERIOR},
    {.name = "top_ray_outside_left",
     .x = -1.0,
     .y = 10.0,
     .expected = Location::EXTERIOR},
    {.name = "top_left_edge",
     .x = 1.5,
     .y = 10.0,
     .expected = Location::BOUNDARY},
    {.name = "top_notch_exterior",
     .x = 5.0,
     .y = 10.0,
     .expected = Location::EXTERIOR},
    {.name = "top_right_edge",
     .x = 8.5,
     .y = 10.0,
     .expected = Location::BOUNDARY},
    {.name = "top_ray_outside_right",
     .x = 11.0,
     .y = 10.0,
     .expected = Location::EXTERIOR},
});

constexpr auto kMultiPolygonIslandCases = std::to_array<PointCase>({
    {.name = "outer_ring_interior",
     .x = 1.0,
     .y = 1.0,
     .expected = Location::INTERIOR},
    {.name = "outer_hole_exterior",
     .x = 4.0,
     .y = 4.0,
     .expected = Location::EXTERIOR},
    {.name = "island_interior",
     .x = 7.0,
     .y = 7.0,
     .expected = Location::INTERIOR},
    {.name = "inner_hole_exterior",
     .x = 10.0,
     .y = 10.0,
     .expected = Location::EXTERIOR},
    {.name = "outer_shell_boundary",
     .x = 0.0,
     .y = 10.0,
     .expected = Location::BOUNDARY},
    {.name = "outer_hole_boundary",
     .x = 2.0,
     .y = 10.0,
     .expected = Location::BOUNDARY},
    {.name = "island_shell_boundary",
     .x = 6.0,
     .y = 10.0,
     .expected = Location::BOUNDARY},
    {.name = "inner_hole_boundary",
     .x = 8.0,
     .y = 10.0,
     .expected = Location::BOUNDARY},
    {.name = "outside_all",
     .x = 25.0,
     .y = 10.0,
     .expected = Location::EXTERIOR},
});

TEST(IndexedPointInAreaLocatorTest,
     GetGeometryAndLazyDestructionWithoutLocate) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  const std::string wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))";
  std::unique_ptr<Geometry> geom = reader.read(wkt);
  ASSERT_NE(geom, nullptr);

  IndexedPointInAreaLocator locator(*geom);
  EXPECT_EQ(&locator.getGeometry(), geom.get());
}

TEST(IndexedPointInAreaLocatorTest, SquareInteriorBoundaryAndExterior) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_NE(geom, nullptr);

  IndexedPointInAreaLocator locator(*geom);
  for (const auto& tc : kSquareCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY pt(tc.x, tc.y);
    EXPECT_EQ(locator.locate(&pt), tc.expected);
  }
}

TEST(IndexedPointInAreaLocatorTest, ConcaveUPolygonAndVertexGrazingRays) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 7 10, 7 4, 3 4, 3 10, 0 10, 0 0))");
  ASSERT_NE(geom, nullptr);

  IndexedPointInAreaLocator locator(*geom);
  for (const auto& tc : kConcaveUCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY pt(tc.x, tc.y);
    EXPECT_EQ(locator.locate(&pt), tc.expected);
  }
}

TEST(IndexedPointInAreaLocatorTest, DiamondDiagonalEdgesAndVertexRay) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom =
      reader.read("POLYGON ((0 5, 5 0, 10 5, 5 10, 0 5))");
  ASSERT_NE(geom, nullptr);

  IndexedPointInAreaLocator locator(*geom);
  const CoordinateXY inside(5.0, 5.0);
  const CoordinateXY diag_bottom_left(2.5, 2.5);
  const CoordinateXY diag_top_right(7.5, 7.5);
  const CoordinateXY left_vertex(0.0, 5.0);
  const CoordinateXY right_vertex(10.0, 5.0);
  const CoordinateXY left_outside(-1.0, 5.0);
  const CoordinateXY right_outside(11.0, 5.0);

  EXPECT_EQ(locator.locate(&inside), Location::INTERIOR);
  EXPECT_EQ(locator.locate(&diag_bottom_left), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&diag_top_right), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&left_vertex), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&right_vertex), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&left_outside), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&right_outside), Location::EXTERIOR);
}

TEST(IndexedPointInAreaLocatorTest, PolygonWithMultipleHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom = reader.read(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0),"
      " (2 2, 2 8, 8 8, 8 2, 2 2),"
      " (12 12, 12 18, 18 18, 18 12, 12 12))");
  ASSERT_NE(geom, nullptr);

  IndexedPointInAreaLocator locator(*geom);
  const CoordinateXY between_holes(10.0, 10.0);
  const CoordinateXY in_hole1(5.0, 5.0);
  const CoordinateXY in_hole2(15.0, 15.0);
  const CoordinateXY on_hole1_edge(2.0, 5.0);
  const CoordinateXY on_hole2_vertex(12.0, 12.0);

  EXPECT_EQ(locator.locate(&between_holes), Location::INTERIOR);
  EXPECT_EQ(locator.locate(&in_hole1), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&in_hole2), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&on_hole1_edge), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&on_hole2_vertex), Location::BOUNDARY);
}

TEST(IndexedPointInAreaLocatorTest, MultiPolygonWithNestedIslandInHole) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 20 0, 20 20, 0 20, 0 0), (2 2, 2 18, 18 18, 18 2, 2 2)),"
      "((6 6, 14 6, 14 14, 6 14, 6 6), (8 8, 8 12, 12 12, 12 8, 8 8)))");
  ASSERT_NE(geom, nullptr);

  IndexedPointInAreaLocator locator(*geom);
  for (const auto& tc : kMultiPolygonIslandCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY pt(tc.x, tc.y);
    EXPECT_EQ(locator.locate(&pt), tc.expected);
  }
}

TEST(IndexedPointInAreaLocatorTest, LinearRingAndClosedLineStringSupported) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr auto kClosedWkts = std::to_array<std::string_view>({
      "LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)",
      "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)",
  });
  for (const std::string_view wkt : kClosedWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = reader.read(std::string(wkt));
    ASSERT_NE(geom, nullptr);

    IndexedPointInAreaLocator locator(*geom);
    const CoordinateXY inside(5.0, 5.0);
    const CoordinateXY boundary(0.0, 5.0);
    const CoordinateXY outside(15.0, 5.0);
    EXPECT_EQ(locator.locate(&inside), Location::INTERIOR);
    EXPECT_EQ(locator.locate(&boundary), Location::BOUNDARY);
    EXPECT_EQ(locator.locate(&outside), Location::EXTERIOR);
  }
}

TEST(IndexedPointInAreaLocatorTest, OpenLineStringsAreIgnored) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Pure open LineString produces an empty index.
  std::unique_ptr<Geometry> open_line =
      reader.read("LINESTRING (0 0, 10 0, 10 10)");
  ASSERT_NE(open_line, nullptr);
  IndexedPointInAreaLocator open_locator(*open_line);
  const CoordinateXY on_open(5.0, 0.0);
  const CoordinateXY inside_open(5.0, 5.0);
  EXPECT_EQ(open_locator.locate(&on_open), Location::EXTERIOR);
  EXPECT_EQ(open_locator.locate(&inside_open), Location::EXTERIOR);

  // GeometryCollection combining an open LineString and a Polygon skips only
  // the open LineString while indexing the Polygon ring.
  std::unique_ptr<Geometry> mixed = reader.read(
      "GEOMETRYCOLLECTION ("
      "LINESTRING (100 0, 110 0, 110 10),"
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))");
  ASSERT_NE(mixed, nullptr);
  IndexedPointInAreaLocator mixed_locator(*mixed);
  const CoordinateXY in_poly(5.0, 5.0);
  const CoordinateXY on_poly(0.0, 5.0);
  const CoordinateXY on_mixed_open(105.0, 0.0);
  EXPECT_EQ(mixed_locator.locate(&in_poly), Location::INTERIOR);
  EXPECT_EQ(mixed_locator.locate(&on_poly), Location::BOUNDARY);
  EXPECT_EQ(mixed_locator.locate(&on_mixed_open), Location::EXTERIOR);
}

TEST(IndexedPointInAreaLocatorTest, EmptyAndPuntalGeometriesReturnExterior) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr auto kEmptyAndPuntalWkts = std::to_array<std::string_view>({
      "POLYGON EMPTY",
      "MULTIPOLYGON EMPTY",
      "LINEARRING EMPTY",
      "LINESTRING EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
      "POINT (5 5)",
      "MULTIPOINT ((5 5), (0 0))",
  });
  const CoordinateXY pt(5.0, 5.0);
  for (const std::string_view wkt : kEmptyAndPuntalWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = reader.read(std::string(wkt));
    ASSERT_NE(geom, nullptr);

    IndexedPointInAreaLocator locator(*geom);
    EXPECT_EQ(locator.locate(&pt), Location::EXTERIOR);
  }
}

TEST(IndexedPointInAreaLocatorTest,
     ManyVertexPolygonExercisesMultiLevelSTRtree) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  constexpr std::size_t kNumSides = 64;
  constexpr double kRadius = 10.0;

  auto coords = std::make_unique<CoordinateSequence>(0u, false, false);
  coords->reserve(kNumSides + 1);
  for (std::size_t i = 0; i < kNumSides; ++i) {
    const double angle = 2.0 * std::numbers::pi * static_cast<double>(i) /
                         static_cast<double>(kNumSides);
    coords->add(kRadius * std::cos(angle), kRadius * std::sin(angle));
  }
  coords->add(kRadius, 0.0);

  std::unique_ptr<LinearRing> shell =
      factory->createLinearRing(std::move(coords));
  std::unique_ptr<Polygon> polygon = factory->createPolygon(std::move(shell));
  ASSERT_NE(polygon, nullptr);

  IndexedPointInAreaLocator locator(*polygon);
  const CoordinateXY center(0.0, 0.0);
  const CoordinateXY inner(5.0, 3.0);
  const CoordinateXY vertex(kRadius, 0.0);
  const CoordinateXY just_outside(10.01, 0.0);
  const CoordinateXY corner_outside(8.0, 8.0);

  EXPECT_EQ(locator.locate(&center), Location::INTERIOR);
  EXPECT_EQ(locator.locate(&inner), Location::INTERIOR);
  EXPECT_EQ(locator.locate(&vertex), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&just_outside), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&corner_outside), Location::EXTERIOR);
}

TEST(IndexedPointInAreaLocatorTest, CoordinateDimensionsXYZXymAndXyzm) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr auto kDimWkts = std::to_array<std::string_view>({
      "POLYGON Z ((0 0 1, 10 0 2, 10 10 3, 0 10 4, 0 0 1))",
      "POLYGON M ((0 0 1, 10 0 2, 10 10 3, 0 10 4, 0 0 1))",
      "POLYGON ZM ((0 0 1 5, 10 0 2 6, 10 10 3 7, 0 10 4 8, 0 0 1 5))",
  });
  const CoordinateXY inside(5.0, 5.0);
  const CoordinateXY boundary(10.0, 5.0);
  const CoordinateXY outside(-2.0, 5.0);

  for (const std::string_view wkt : kDimWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = reader.read(std::string(wkt));
    ASSERT_NE(geom, nullptr);

    IndexedPointInAreaLocator locator(*geom);
    EXPECT_EQ(locator.locate(&inside), Location::INTERIOR);
    EXPECT_EQ(locator.locate(&boundary), Location::BOUNDARY);
    EXPECT_EQ(locator.locate(&outside), Location::EXTERIOR);
  }
}

TEST(IndexedPointInAreaLocatorTest, NanAndInfQueryCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_NE(geom, nullptr);

  IndexedPointInAreaLocator locator(*geom);
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();

  const CoordinateXY nan_x_overlapping_y(kNan, 5.0);
  const CoordinateXY neg_inf_x_overlapping_y(-kInf, 5.0);
  const CoordinateXY nan_x_outside_y(kNan, 20.0);
  const CoordinateXY nan_y(5.0, kNan);
  const CoordinateXY nan_xy(kNan, kNan);
  const CoordinateXY pos_inf_x(kInf, 5.0);
  const CoordinateXY pos_inf_y(5.0, kInf);
  const CoordinateXY neg_inf_y(5.0, -kInf);

  EXPECT_THROW(locator.locate(&nan_x_overlapping_y), IllegalArgumentException);
  EXPECT_THROW(locator.locate(&neg_inf_x_overlapping_y),
               IllegalArgumentException);
  EXPECT_EQ(locator.locate(&nan_x_outside_y), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&nan_y), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&nan_xy), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&pos_inf_x), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&pos_inf_y), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&neg_inf_y), Location::EXTERIOR);

  std::unique_ptr<Geometry> empty_poly = reader.read("POLYGON EMPTY");
  ASSERT_NE(empty_poly, nullptr);
  IndexedPointInAreaLocator empty_locator(*empty_poly);
  EXPECT_EQ(empty_locator.locate(&nan_x_overlapping_y), Location::EXTERIOR);
}

TEST(IndexedPointInAreaLocatorTest, MultiLineStringMixedClosedAndOpen) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom = reader.read(
      "MULTILINESTRING ((0 0, 10 0, 10 10, 0 10, 0 0), (20 0, 30 0, 30 10))");
  ASSERT_NE(geom, nullptr);

  IndexedPointInAreaLocator locator(*geom);
  const CoordinateXY in_closed(5.0, 5.0);
  const CoordinateXY on_closed(10.0, 5.0);
  const CoordinateXY between_parts(15.0, 5.0);
  const CoordinateXY on_open(25.0, 0.0);
  const CoordinateXY near_open(25.0, 5.0);

  EXPECT_EQ(locator.locate(&in_closed), Location::INTERIOR);
  EXPECT_EQ(locator.locate(&on_closed), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&between_parts), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&on_open), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&near_open), Location::EXTERIOR);
}

TEST(IndexedPointInAreaLocatorTest,
     NestedGeometryCollectionWithMixedComponents) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom = reader.read(
      "GEOMETRYCOLLECTION ("
      "POINT (5 5),"
      "LINESTRING (100 0, 110 0),"
      "GEOMETRYCOLLECTION ("
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 8, 8 8, 8 2, 2 "
      "2))),"
      "LINEARRING (20 0, 30 0, 30 10, 20 10, 20 0)))");
  ASSERT_NE(geom, nullptr);

  IndexedPointInAreaLocator locator(*geom);
  const CoordinateXY in_poly(1.0, 5.0);
  const CoordinateXY in_hole(5.0, 5.0);
  const CoordinateXY on_hole(2.0, 5.0);
  const CoordinateXY in_ring(25.0, 5.0);
  const CoordinateXY on_ring(30.0, 5.0);
  const CoordinateXY on_open_line(105.0, 0.0);

  EXPECT_EQ(locator.locate(&in_poly), Location::INTERIOR);
  EXPECT_EQ(locator.locate(&in_hole), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&on_hole), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&in_ring), Location::INTERIOR);
  EXPECT_EQ(locator.locate(&on_ring), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&on_open_line), Location::EXTERIOR);
}

TEST(IndexedPointInAreaLocatorTest, DegenerateClosedLineAndRepeatedVertices) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Geometry> degenerate_closed =
      reader.read("LINESTRING (0 0, 10 0, 0 0)");
  ASSERT_NE(degenerate_closed, nullptr);
  IndexedPointInAreaLocator degen_locator(*degenerate_closed);
  const CoordinateXY on_seg(5.0, 0.0);
  const CoordinateXY at_start(0.0, 0.0);
  const CoordinateXY off_left(-1.0, 0.0);
  const CoordinateXY above(5.0, 1.0);
  EXPECT_EQ(degen_locator.locate(&on_seg), Location::BOUNDARY);
  EXPECT_EQ(degen_locator.locate(&at_start), Location::BOUNDARY);
  EXPECT_EQ(degen_locator.locate(&off_left), Location::EXTERIOR);
  EXPECT_EQ(degen_locator.locate(&above), Location::EXTERIOR);

  std::unique_ptr<Geometry> repeated_vertices =
      reader.read("POLYGON ((0 0, 0 0, 5 0, 10 0, 10 10, 10 10, 0 10, 0 0))");
  ASSERT_NE(repeated_vertices, nullptr);
  IndexedPointInAreaLocator repeated_locator(*repeated_vertices);
  const CoordinateXY inside(5.0, 5.0);
  const CoordinateXY split_vertex(5.0, 0.0);
  const CoordinateXY on_split_edge(7.5, 0.0);
  const CoordinateXY repeated_corner(10.0, 10.0);
  const CoordinateXY outside_right(15.0, 0.0);
  const CoordinateXY outside_left(-5.0, 0.0);
  EXPECT_EQ(repeated_locator.locate(&inside), Location::INTERIOR);
  EXPECT_EQ(repeated_locator.locate(&at_start), Location::BOUNDARY);
  EXPECT_EQ(repeated_locator.locate(&split_vertex), Location::BOUNDARY);
  EXPECT_EQ(repeated_locator.locate(&on_split_edge), Location::BOUNDARY);
  EXPECT_EQ(repeated_locator.locate(&repeated_corner), Location::BOUNDARY);
  EXPECT_EQ(repeated_locator.locate(&outside_right), Location::EXTERIOR);
  EXPECT_EQ(repeated_locator.locate(&outside_left), Location::EXTERIOR);
}

TEST(IndexedPointInAreaLocatorTest, ClockwiseAndCounterclockwiseOrientations) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr auto kOrientedWkts = std::to_array<std::string_view>({
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 8, 8 8, 8 2, 2 2))",
  });
  const CoordinateXY inside(1.0, 5.0);
  const CoordinateXY in_hole(5.0, 5.0);
  const CoordinateXY on_hole(2.0, 5.0);
  const CoordinateXY on_shell(0.0, 5.0);
  const CoordinateXY outside(12.0, 5.0);

  for (const std::string_view wkt : kOrientedWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = reader.read(std::string(wkt));
    ASSERT_NE(geom, nullptr);

    IndexedPointInAreaLocator locator(*geom);
    EXPECT_EQ(locator.locate(&inside), Location::INTERIOR);
    EXPECT_EQ(locator.locate(&in_hole), Location::EXTERIOR);
    EXPECT_EQ(locator.locate(&on_hole), Location::BOUNDARY);
    EXPECT_EQ(locator.locate(&on_shell), Location::BOUNDARY);
    EXPECT_EQ(locator.locate(&outside), Location::EXTERIOR);
  }
}

TEST(IndexedPointInAreaLocatorTest, QueryWithHigherDimensionCoordinateTypes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_NE(geom, nullptr);

  IndexedPointInAreaLocator locator(*geom);
  const Coordinate xyz_inside(5.0, 5.0, 100.0);
  const Coordinate xyz_boundary(10.0, 5.0, -50.0);
  const CoordinateXYZM xyzm_outside(12.0, 5.0, 1.0, 2.0);
  const CoordinateXYZM xyzm_inside(2.0, 2.0,
                                   std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::quiet_NaN());

  EXPECT_EQ(locator.locate(&xyz_inside), Location::INTERIOR);
  EXPECT_EQ(locator.locate(&xyz_boundary), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&xyzm_outside), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&xyzm_inside), Location::INTERIOR);
}

TEST(IndexedPointInAreaLocatorTest, WorksViaPointOnGeometryLocatorInterface) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom =
      reader.read("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  ASSERT_NE(geom, nullptr);

  std::unique_ptr<PointOnGeometryLocator> locator =
      std::make_unique<IndexedPointInAreaLocator>(*geom);
  const CoordinateXY inside(2.0, 2.0);
  const CoordinateXY boundary(0.0, 2.0);
  const CoordinateXY outside(5.0, 2.0);

  EXPECT_EQ(locator->locate(&inside), Location::INTERIOR);
  EXPECT_EQ(locator->locate(&boundary), Location::BOUNDARY);
  EXPECT_EQ(locator->locate(&outside), Location::EXTERIOR);
}

}  // namespace
}  // namespace geos_google
