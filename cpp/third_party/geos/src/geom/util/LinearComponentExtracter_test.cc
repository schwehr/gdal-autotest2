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

// Unit tests for third_party/geos/src/geom/util/LinearComponentExtracter.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/util/LinearComponentExtracter.cpp

#include "third_party/geos/include/geos/geom/util/LinearComponentExtracter.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::MultiPolygon;
using geos::geom::Polygon;
using geos::geom::util::LinearComponentExtracter;
using geos::io::WKTReader;

struct EmptyExtractionCase {
  std::string_view name;
  std::string_view wkt;
};

constexpr auto kEmptyExtractionCases = std::to_array<EmptyExtractionCase>({
    {.name = "point", .wkt = "POINT (1 2)"},
    {.name = "point_empty", .wkt = "POINT EMPTY"},
    {.name = "multipoint", .wkt = "MULTIPOINT ((1 2), (3 4))"},
    {.name = "multipoint_empty", .wkt = "MULTIPOINT EMPTY"},
    {.name = "point_only_collection",
     .wkt = "GEOMETRYCOLLECTION (POINT (1 2), MULTIPOINT ((3 4), (5 6)))"},
    {.name = "linestring_empty", .wkt = "LINESTRING EMPTY"},
    {.name = "linearring_empty", .wkt = "LINEARRING EMPTY"},
    {.name = "polygon_empty", .wkt = "POLYGON EMPTY"},
    {.name = "multilinestring_empty", .wkt = "MULTILINESTRING EMPTY"},
    {.name = "multipolygon_empty", .wkt = "MULTIPOLYGON EMPTY"},
    {.name = "geometrycollection_empty", .wkt = "GEOMETRYCOLLECTION EMPTY"},
    {.name = "circularstring", .wkt = "CIRCULARSTRING (0 0, 1 1, 2 0)"},
});

TEST(LinearComponentExtracterTest,
     ReturnsEmptyForPointEmptyAndCurvedGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kEmptyExtractionCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr geom = reader.read(std::string(tc.wkt));
    ASSERT_NE(geom, nullptr);

    std::vector<const LineString*> lines;
    LinearComponentExtracter::getLines(*geom, lines);
    EXPECT_TRUE(lines.empty());
  }
}

TEST(LinearComponentExtracterTest, ExtractsLineStringAndLinearRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr line = reader.read("LINESTRING (0 0, 3 4, 6 0)");
  std::vector<const LineString*> line_comps;
  LinearComponentExtracter::getLines(*line, line_comps);
  ASSERT_EQ(line_comps.size(), 1u);
  EXPECT_EQ(line_comps[0], line.get());
  EXPECT_EQ(line_comps[0]->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_DOUBLE_EQ(line_comps[0]->getLength(), 10.0);

  Geometry::Ptr ring = reader.read("LINEARRING (0 0, 2 0, 2 2, 0 2, 0 0)");
  std::vector<const LineString*> ring_comps;
  LinearComponentExtracter::getLines(*ring, ring_comps);
  ASSERT_EQ(ring_comps.size(), 1u);
  EXPECT_EQ(ring_comps[0], ring.get());
  EXPECT_EQ(ring_comps[0]->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_DOUBLE_EQ(ring_comps[0]->getLength(), 8.0);
}

TEST(LinearComponentExtracterTest, ExtractsShellAndHolesFromPolygon) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Polygon> poly = reader.read<Polygon>(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(1 1, 1 3, 3 3, 3 1, 1 1), "
      "(5 5, 5 7, 7 7, 7 5, 5 5))");

  std::vector<const LineString*> lines;
  LinearComponentExtracter::getLines(*poly, lines);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], poly->getExteriorRing());
  EXPECT_EQ(lines[1], poly->getInteriorRingN(0));
  EXPECT_EQ(lines[2], poly->getInteriorRingN(1));
  for (const LineString* comp : lines) {
    EXPECT_EQ(comp->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  }
}

TEST(LinearComponentExtracterTest,
     ExtractsMultiGeometriesSkippingEmptyMembers) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::vector<std::unique_ptr<LineString>> mls_parts;
  mls_parts.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(0, 0), CoordinateXY(1, 0)}));
  mls_parts.push_back(factory->createLineString());
  mls_parts.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(2, 0), CoordinateXY(5, 0)}));
  std::unique_ptr<MultiLineString> mls =
      factory->createMultiLineString(std::move(mls_parts));

  std::vector<const LineString*> mls_lines;
  LinearComponentExtracter::getLines(*mls, mls_lines);
  ASSERT_EQ(mls_lines.size(), 2u);
  EXPECT_EQ(mls_lines[0], mls->getGeometryN(0));
  EXPECT_EQ(mls_lines[1], mls->getGeometryN(2));

  std::vector<std::unique_ptr<Polygon>> mp_parts;
  mp_parts.push_back(reader.read<Polygon>(
      "POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0), (2 2, 2 3, 3 3, 3 2, 2 2))"));
  mp_parts.push_back(factory->createPolygon());
  mp_parts.push_back(
      reader.read<Polygon>("POLYGON ((10 0, 12 0, 12 2, 10 2, 10 0))"));
  std::unique_ptr<MultiPolygon> mp =
      factory->createMultiPolygon(std::move(mp_parts));

  std::vector<const LineString*> mp_lines;
  LinearComponentExtracter::getLines(*mp, mp_lines);
  ASSERT_EQ(mp_lines.size(), 3u);
  EXPECT_EQ(mp_lines[0], mp->getGeometryN(0)->getExteriorRing());
  EXPECT_EQ(mp_lines[1], mp->getGeometryN(0)->getInteriorRingN(0));
  EXPECT_EQ(mp_lines[2], mp->getGeometryN(2)->getExteriorRing());
}

TEST(LinearComponentExtracterTest, ExtractsFromNestedGeometryCollection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr gc = reader.read(
      "GEOMETRYCOLLECTION ("
      "POINT (0 0), "
      "LINESTRING EMPTY, "
      "LINESTRING (0 0, 1 0), "
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 1 2, 2 2, 2 1, 1 1)), "
      "MULTILINESTRING ((10 0, 11 0), (12 0, 13 0))))");

  std::vector<const LineString*> lines;
  LinearComponentExtracter::getLines(*gc, lines);
  ASSERT_EQ(lines.size(), 5u);
  EXPECT_DOUBLE_EQ(lines[0]->getLength(), 1.0);
  EXPECT_DOUBLE_EQ(lines[1]->getLength(), 16.0);
  EXPECT_DOUBLE_EQ(lines[2]->getLength(), 4.0);
  EXPECT_DOUBLE_EQ(lines[3]->getLength(), 1.0);
  EXPECT_DOUBLE_EQ(lines[4]->getLength(), 1.0);
}

TEST(LinearComponentExtracterTest, AppendsAcrossMultipleCallsAndDirectFilter) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr pt = reader.read("POINT (5 5)");
  Geometry::Ptr line = reader.read("LINESTRING (0 0, 3 0)");
  std::unique_ptr<Polygon> poly =
      reader.read<Polygon>("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");

  std::vector<const LineString*> via_static;
  LinearComponentExtracter::getLines(*line, via_static);
  LinearComponentExtracter::getLines(*pt, via_static);
  LinearComponentExtracter::getLines(*poly, via_static);
  ASSERT_EQ(via_static.size(), 2u);
  EXPECT_EQ(via_static[0], line.get());
  EXPECT_EQ(via_static[1], poly->getExteriorRing());

  std::vector<const LineString*> via_filter;
  LinearComponentExtracter extracter(via_filter);
  pt->apply_ro(&extracter);
  line->apply_ro(&extracter);
  poly->apply_ro(&extracter);
  ASSERT_EQ(via_filter.size(), 2u);
  EXPECT_EQ(via_filter[0], line.get());
  EXPECT_EQ(via_filter[1], poly->getExteriorRing());
}

}  // namespace
}  // namespace geos_google
