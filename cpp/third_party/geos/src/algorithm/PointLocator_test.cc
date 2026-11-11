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

// Unit tests for third_party/geos/src/algorithm/PointLocator.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/PointLocator.cpp

#include "third_party/geos/include/geos/algorithm/PointLocator.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::PointLocator;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::util::UnsupportedOperationException;

class PointLocatorTest : public ::testing::Test {
 protected:
  PointLocatorTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  void ExpectLocation(const Geometry& geom, const CoordinateXY& pt,
                      Location expected) {
    EXPECT_EQ(locator_.locate(pt, &geom), expected);
    EXPECT_EQ(locator_.intersects(pt, &geom), expected != Location::EXTERIOR);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
  PointLocator locator_;
};

TEST_F(PointLocatorTest, EmptyGeometriesReturnExterior) {
  constexpr auto kEmptyWkts = std::to_array<std::string_view>({
      "POINT EMPTY",
      "LINESTRING EMPTY",
      "LINEARRING EMPTY",
      "POLYGON EMPTY",
      "MULTIPOINT EMPTY",
      "MULTILINESTRING EMPTY",
      "MULTIPOLYGON EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
      "CIRCULARSTRING EMPTY",
  });

  for (const std::string_view wkt : kEmptyWkts) {
    SCOPED_TRACE(wkt);
    const Geometry::Ptr geom = reader_.read(std::string(wkt));
    ExpectLocation(*geom, CoordinateXY{0.0, 0.0}, Location::EXTERIOR);
  }
}

TEST_F(PointLocatorTest, PointLocationAndIntersects) {
  const Geometry::Ptr pt2d = reader_.read("POINT (3 4)");
  ExpectLocation(*pt2d, CoordinateXY{3.0, 4.0}, Location::INTERIOR);
  ExpectLocation(*pt2d, CoordinateXY{3.0, 5.0}, Location::EXTERIOR);
  ExpectLocation(*pt2d, CoordinateXY{4.0, 4.0}, Location::EXTERIOR);

  // Point matching compares 2D ordinates only.
  const Geometry::Ptr pt3d = reader_.read("POINT Z (3 4 99)");
  ExpectLocation(*pt3d, CoordinateXY{3.0, 4.0}, Location::INTERIOR);
}

TEST_F(PointLocatorTest, OpenAndClosedLineString) {
  const Geometry::Ptr open_line = reader_.read("LINESTRING (0 0, 10 0, 10 10)");
  ExpectLocation(*open_line, CoordinateXY{0.0, 0.0}, Location::BOUNDARY);
  ExpectLocation(*open_line, CoordinateXY{10.0, 10.0}, Location::BOUNDARY);
  ExpectLocation(*open_line, CoordinateXY{10.0, 0.0}, Location::INTERIOR);
  ExpectLocation(*open_line, CoordinateXY{5.0, 0.0}, Location::INTERIOR);
  ExpectLocation(*open_line, CoordinateXY{10.0, 5.0}, Location::INTERIOR);
  // Inside bounding box, not on any segment.
  ExpectLocation(*open_line, CoordinateXY{5.0, 5.0}, Location::EXTERIOR);
  // Outside bounding box.
  ExpectLocation(*open_line, CoordinateXY{20.0, 0.0}, Location::EXTERIOR);

  const Geometry::Ptr closed_line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  // Closed LineString has no boundary; its start/end vertex is interior.
  ExpectLocation(*closed_line, CoordinateXY{0.0, 0.0}, Location::INTERIOR);
  ExpectLocation(*closed_line, CoordinateXY{5.0, 0.0}, Location::INTERIOR);
  // A closed LineString is 1D and does not enclose an area.
  ExpectLocation(*closed_line, CoordinateXY{5.0, 5.0}, Location::EXTERIOR);
  ExpectLocation(*closed_line, CoordinateXY{15.0, 5.0}, Location::EXTERIOR);
}

TEST_F(PointLocatorTest, LinearRingEnclosesNoArea) {
  const Geometry::Ptr ring =
      reader_.read("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  ExpectLocation(*ring, CoordinateXY{0.0, 0.0}, Location::INTERIOR);
  ExpectLocation(*ring, CoordinateXY{5.0, 0.0}, Location::INTERIOR);
  ExpectLocation(*ring, CoordinateXY{5.0, 5.0}, Location::EXTERIOR);
  ExpectLocation(*ring, CoordinateXY{15.0, 5.0}, Location::EXTERIOR);
}

TEST_F(PointLocatorTest, ConcavePolygonAndPolygonWithHoles) {
  const Geometry::Ptr l_poly =
      reader_.read("POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))");
  ExpectLocation(*l_poly, CoordinateXY{2.0, 2.0}, Location::INTERIOR);
  ExpectLocation(*l_poly, CoordinateXY{0.0, 0.0}, Location::BOUNDARY);
  ExpectLocation(*l_poly, CoordinateXY{5.0, 0.0}, Location::BOUNDARY);
  // Inside shell bounding box but in the concave exterior notch.
  ExpectLocation(*l_poly, CoordinateXY{8.0, 8.0}, Location::EXTERIOR);
  // Outside shell bounding box.
  ExpectLocation(*l_poly, CoordinateXY{-1.0, 5.0}, Location::EXTERIOR);

  const Geometry::Ptr holed_poly = reader_.read(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 8 2, 8 4, 4 4, 4 8, 2 8, 2 2), "
      "(12 12, 18 12, 18 18, 12 18, 12 12))");
  ExpectLocation(*holed_poly, CoordinateXY{10.0, 10.0}, Location::INTERIOR);
  // Inside hole 1's bounding box, outside hole 1 itself (in its notch).
  ExpectLocation(*holed_poly, CoordinateXY{6.0, 6.0}, Location::INTERIOR);
  ExpectLocation(*holed_poly, CoordinateXY{3.0, 3.0}, Location::EXTERIOR);
  ExpectLocation(*holed_poly, CoordinateXY{2.0, 5.0}, Location::BOUNDARY);
  ExpectLocation(*holed_poly, CoordinateXY{15.0, 15.0}, Location::EXTERIOR);
  ExpectLocation(*holed_poly, CoordinateXY{12.0, 12.0}, Location::BOUNDARY);
  ExpectLocation(*holed_poly, CoordinateXY{0.0, 10.0}, Location::BOUNDARY);
  ExpectLocation(*holed_poly, CoordinateXY{-2.0, 10.0}, Location::EXTERIOR);
}

TEST_F(PointLocatorTest, MultiPointIncludingEmptyComponent) {
  const Geometry::Ptr mp = reader_.read("MULTIPOINT (EMPTY, (1 2), (5 6))");
  ExpectLocation(*mp, CoordinateXY{1.0, 2.0}, Location::INTERIOR);
  ExpectLocation(*mp, CoordinateXY{5.0, 6.0}, Location::INTERIOR);
  ExpectLocation(*mp, CoordinateXY{0.0, 0.0}, Location::EXTERIOR);
}

TEST_F(PointLocatorTest, MultiLineStringAndBoundaryDeterminationRule) {
  const Geometry::Ptr disjoint_mls =
      reader_.read("MULTILINESTRING (EMPTY, (0 0, 4 0), (10 0, 14 0))");
  ExpectLocation(*disjoint_mls, CoordinateXY{0.0, 0.0}, Location::BOUNDARY);
  ExpectLocation(*disjoint_mls, CoordinateXY{2.0, 0.0}, Location::INTERIOR);
  ExpectLocation(*disjoint_mls, CoordinateXY{12.0, 0.0}, Location::INTERIOR);
  ExpectLocation(*disjoint_mls, CoordinateXY{14.0, 0.0}, Location::BOUNDARY);
  ExpectLocation(*disjoint_mls, CoordinateXY{7.0, 0.0}, Location::EXTERIOR);

  // Two endpoints meeting at (5, 0) yield boundaryCount == 2 -> INTERIOR.
  const Geometry::Ptr two_lines =
      reader_.read("MULTILINESTRING ((0 0, 5 0), (5 0, 10 0))");
  ExpectLocation(*two_lines, CoordinateXY{5.0, 0.0}, Location::INTERIOR);
  ExpectLocation(*two_lines, CoordinateXY{0.0, 0.0}, Location::BOUNDARY);

  // Three endpoints meeting at (5, 0) yield boundaryCount == 3 -> BOUNDARY.
  const Geometry::Ptr three_lines =
      reader_.read("MULTILINESTRING ((0 0, 5 0), (10 0, 5 0), (5 5, 5 0))");
  ExpectLocation(*three_lines, CoordinateXY{5.0, 0.0}, Location::BOUNDARY);

  // T-junction: interior of one line + endpoint of another -> BOUNDARY.
  const Geometry::Ptr t_junction =
      reader_.read("MULTILINESTRING ((0 0, 10 0), (5 0, 5 5))");
  ExpectLocation(*t_junction, CoordinateXY{5.0, 0.0}, Location::BOUNDARY);

  // Closed loop + open line touching at (0, 0): 1 boundary + interior ->
  // BOUNDARY.
  const Geometry::Ptr loop_and_tail =
      reader_.read("MULTILINESTRING ((0 0, 4 0, 4 4, 0 4, 0 0), (0 0, -4 0))");
  ExpectLocation(*loop_and_tail, CoordinateXY{0.0, 0.0}, Location::BOUNDARY);
}

TEST_F(PointLocatorTest, MultiPolygonIncludingEmptyComponentAndMod2Node) {
  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(factory_->createPolygon());
  polys.push_back(reader_.read<Polygon>("POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))"));
  polys.push_back(
      reader_.read<Polygon>("POLYGON ((5 5, 10 5, 10 10, 5 10, 5 5))"));
  const auto mp = factory_->createMultiPolygon(std::move(polys));

  ExpectLocation(*mp, CoordinateXY{2.0, 2.0}, Location::INTERIOR);
  ExpectLocation(*mp, CoordinateXY{7.0, 7.0}, Location::INTERIOR);
  ExpectLocation(*mp, CoordinateXY{2.0, 0.0}, Location::BOUNDARY);
  // Shared corner vertex hits boundary of two polygons -> INTERIOR by mod-2
  // rule.
  ExpectLocation(*mp, CoordinateXY{5.0, 5.0}, Location::INTERIOR);
  ExpectLocation(*mp, CoordinateXY{2.0, 8.0}, Location::EXTERIOR);
}

TEST_F(PointLocatorTest, GeometryCollectionAndNestedCollections) {
  const Geometry::Ptr gc = reader_.read(
      "GEOMETRYCOLLECTION ("
      "POINT EMPTY, "
      "POINT (20 20), "
      "LINESTRING (0 0, 10 0), "
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "LINEARRING (30 0, 34 0, 34 4, 30 4, 30 0))");

  ExpectLocation(*gc, CoordinateXY{20.0, 20.0}, Location::INTERIOR);
  ExpectLocation(*gc, CoordinateXY{2.0, 2.0}, Location::INTERIOR);
  ExpectLocation(*gc, CoordinateXY{7.0, 0.0}, Location::INTERIOR);
  ExpectLocation(*gc, CoordinateXY{10.0, 0.0}, Location::BOUNDARY);
  ExpectLocation(*gc, CoordinateXY{4.0, 2.0}, Location::BOUNDARY);
  // Endpoint of LineString + boundary of Polygon -> numBoundaries == 2 ->
  // INTERIOR.
  ExpectLocation(*gc, CoordinateXY{0.0, 0.0}, Location::INTERIOR);
  // LinearRing boundary is treated as closed curve interior and encloses no
  // area.
  ExpectLocation(*gc, CoordinateXY{30.0, 0.0}, Location::INTERIOR);
  ExpectLocation(*gc, CoordinateXY{32.0, 2.0}, Location::EXTERIOR);
  ExpectLocation(*gc, CoordinateXY{50.0, 50.0}, Location::EXTERIOR);

  const Geometry::Ptr nested = reader_.read(
      "GEOMETRYCOLLECTION ("
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))), "
      "POINT (2 0))");
  ExpectLocation(*nested, CoordinateXY{2.0, 0.0}, Location::BOUNDARY);
  ExpectLocation(*nested, CoordinateXY{2.0, 2.0}, Location::INTERIOR);
  ExpectLocation(*nested, CoordinateXY{9.0, 9.0}, Location::EXTERIOR);
}

TEST_F(PointLocatorTest, ReusesLocatorInstanceWithoutStateLeak) {
  const Geometry::Ptr line = reader_.read("LINESTRING (0 0, 10 0)");
  const Geometry::Ptr poly =
      reader_.read("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  const Geometry::Ptr multi_line =
      reader_.read("MULTILINESTRING ((0 0, 5 0), (5 0, 10 0))");

  ExpectLocation(*line, CoordinateXY{0.0, 0.0}, Location::BOUNDARY);
  ExpectLocation(*poly, CoordinateXY{8.0, 8.0}, Location::EXTERIOR);
  ExpectLocation(*multi_line, CoordinateXY{5.0, 0.0}, Location::INTERIOR);
  ExpectLocation(*multi_line, CoordinateXY{0.0, 0.0}, Location::BOUNDARY);
  ExpectLocation(*poly, CoordinateXY{2.0, 2.0}, Location::INTERIOR);
}

TEST_F(PointLocatorTest, UnsupportedCurvedGeometriesThrow) {
  constexpr auto kUnsupportedWkts = std::to_array<std::string_view>({
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 2, 3 1))",
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 2 2, 4 0, 2 -2, 0 0))",
      "MULTICURVE ((0 0, 1 1), CIRCULARSTRING (0 0, 1 1, 2 0))",
      "MULTISURFACE (((0 0, 2 0, 2 2, 0 2, 0 0)))",
  });

  for (const std::string_view wkt : kUnsupportedWkts) {
    SCOPED_TRACE(wkt);
    const Geometry::Ptr geom = reader_.read(std::string(wkt));
    EXPECT_THROW(locator_.locate(CoordinateXY{1.0, 1.0}, geom.get()),
                 UnsupportedOperationException);
  }
}

}  // namespace
}  // namespace geos_google
