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

#include "third_party/geos/include/geos/algorithm/construct/IndexedPointInPolygonsLocator.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::algorithm::construct::IndexedPointInPolygonsLocator;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::io::WKTReader;

class IndexedPointInPolygonsLocatorTest : public ::testing::Test {
 protected:
  IndexedPointInPolygonsLocatorTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadGeom(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(IndexedPointInPolygonsLocatorTest,
       SinglePolygonInteriorBoundaryExterior) {
  auto poly = ReadGeom("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  IndexedPointInPolygonsLocator locator(*poly);

  const CoordinateXY inside(5.0, 5.0);
  const CoordinateXY on_edge(0.0, 5.0);
  const CoordinateXY on_vertex(10.0, 10.0);
  const CoordinateXY outside(20.0, 20.0);

  // First call initializes the index; subsequent calls reuse it.
  EXPECT_EQ(locator.locate(&inside), Location::INTERIOR);
  EXPECT_EQ(locator.locate(&on_edge), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&on_vertex), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&outside), Location::EXTERIOR);
}

TEST_F(IndexedPointInPolygonsLocatorTest,
       ConcaveEnvelopeAndPolygonWithHoleReturnExterior) {
  auto c_poly =
      ReadGeom("POLYGON ((0 0, 10 0, 10 2, 2 2, 2 8, 10 8, 10 10, 0 10, 0 0))");
  IndexedPointInPolygonsLocator c_locator(*c_poly);

  // Inside the bounding box of the concave polygon, but outside the polygon.
  const CoordinateXY in_concavity(5.0, 5.0);
  const CoordinateXY in_arm(1.0, 5.0);
  const CoordinateXY on_inner_edge(2.0, 5.0);
  EXPECT_EQ(c_locator.locate(&in_concavity), Location::EXTERIOR);
  EXPECT_EQ(c_locator.locate(&in_arm), Location::INTERIOR);
  EXPECT_EQ(c_locator.locate(&on_inner_edge), Location::BOUNDARY);

  auto holed_poly = ReadGeom(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2))");
  IndexedPointInPolygonsLocator holed_locator(*holed_poly);

  const CoordinateXY in_hole(5.0, 5.0);
  const CoordinateXY on_hole_edge(2.0, 5.0);
  const CoordinateXY in_shell(1.0, 5.0);
  EXPECT_EQ(holed_locator.locate(&in_hole), Location::EXTERIOR);
  EXPECT_EQ(holed_locator.locate(&on_hole_edge), Location::BOUNDARY);
  EXPECT_EQ(holed_locator.locate(&in_shell), Location::INTERIOR);
}

TEST_F(IndexedPointInPolygonsLocatorTest,
       MultiPolygonAndNestedHeterogeneousGeometryCollection) {
  auto multipoly = ReadGeom(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((10 0, 12 0, 12 2, 10 2, 10 0)))");
  IndexedPointInPolygonsLocator mp_locator(*multipoly);

  const CoordinateXY mp_in1(1.0, 1.0);
  const CoordinateXY mp_in2(11.0, 1.0);
  const CoordinateXY mp_bnd(12.0, 1.0);
  const CoordinateXY mp_gap(5.0, 1.0);
  EXPECT_EQ(mp_locator.locate(&mp_in1), Location::INTERIOR);
  EXPECT_EQ(mp_locator.locate(&mp_in2), Location::INTERIOR);
  EXPECT_EQ(mp_locator.locate(&mp_bnd), Location::BOUNDARY);
  EXPECT_EQ(mp_locator.locate(&mp_gap), Location::EXTERIOR);

  // Overlapping envelopes: L-shaped polygon covers [0,10]x[0,10], while a
  // second square sits inside its empty upper-right quadrant [5,8]x[5,8].
  auto collection = ReadGeom(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0)), "
      "POLYGON ((5 5, 8 5, 8 8, 5 8, 5 5)), "
      "LINESTRING (0 15, 10 15), "
      "POINT (5 20), "
      "GEOMETRYCOLLECTION (POLYGON ((30 0, 32 0, 32 2, 30 2, 30 0))))");
  IndexedPointInPolygonsLocator gc_locator(*collection);

  const CoordinateXY in_nested_square(6.0, 6.0);
  const CoordinateXY on_nested_square(5.0, 6.0);
  const CoordinateXY in_l_concavity_outside_both(9.0, 9.0);
  const CoordinateXY in_l_arm(1.0, 8.0);
  const CoordinateXY in_deep_collection(31.0, 1.0);
  const CoordinateXY on_line(5.0, 15.0);
  const CoordinateXY on_point(5.0, 20.0);

  EXPECT_EQ(gc_locator.locate(&in_nested_square), Location::INTERIOR);
  EXPECT_EQ(gc_locator.locate(&on_nested_square), Location::BOUNDARY);
  EXPECT_EQ(gc_locator.locate(&in_l_concavity_outside_both),
            Location::EXTERIOR);
  EXPECT_EQ(gc_locator.locate(&in_l_arm), Location::INTERIOR);
  EXPECT_EQ(gc_locator.locate(&in_deep_collection), Location::INTERIOR);
  EXPECT_EQ(gc_locator.locate(&on_line), Location::EXTERIOR);
  EXPECT_EQ(gc_locator.locate(&on_point), Location::EXTERIOR);
}

TEST_F(IndexedPointInPolygonsLocatorTest,
       AdjacentPolygonsSharedBoundaryInCollection) {
  auto touching = ReadGeom(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "POLYGON ((5 0, 10 0, 10 5, 5 5, 5 0)))");
  IndexedPointInPolygonsLocator locator(*touching);

  const CoordinateXY on_shared_edge(5.0, 2.5);
  EXPECT_EQ(locator.locate(&on_shared_edge), Location::BOUNDARY);
}

TEST_F(IndexedPointInPolygonsLocatorTest,
       NonPolygonalAndEmptyGeometriesReturnExterior) {
  const CoordinateXY origin(0.0, 0.0);
  const CoordinateXY on_pt(1.0, 1.0);
  const CoordinateXY on_line(5.0, 0.0);

  {
    auto pt = ReadGeom("POINT (1 1)");
    IndexedPointInPolygonsLocator locator(*pt);
    EXPECT_EQ(locator.locate(&on_pt), Location::EXTERIOR);
  }
  {
    auto line = ReadGeom("LINESTRING (0 0, 10 0)");
    IndexedPointInPolygonsLocator locator(*line);
    EXPECT_EQ(locator.locate(&on_line), Location::EXTERIOR);
  }
  {
    auto empty_poly = ReadGeom("POLYGON EMPTY");
    IndexedPointInPolygonsLocator locator(*empty_poly);
    EXPECT_EQ(locator.locate(&origin), Location::EXTERIOR);
  }
  {
    auto empty_gc = ReadGeom("GEOMETRYCOLLECTION EMPTY");
    IndexedPointInPolygonsLocator locator(*empty_gc);
    EXPECT_EQ(locator.locate(&origin), Location::EXTERIOR);
  }
}

}  // namespace
}  // namespace geos_google
