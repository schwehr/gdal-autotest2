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
// third_party/geos/src/operation/relateng/LineStringExtracter.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/relateng/LineStringExtracter.cpp

#include "third_party/geos/include/geos/operation/relateng/LineStringExtracter.h"

#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::relateng::LineStringExtracter;

TEST(LineStringExtracterTest, ExtractsSingleLineString) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<LineString> line =
      reader.read<LineString>("LINESTRING (0 0, 1 1, 2 0)");

  std::vector<const LineString*> returned =
      LineStringExtracter::getLines(line.get());
  ASSERT_EQ(returned.size(), 1);
  EXPECT_EQ(returned[0], line.get());

  std::vector<const LineString*> populated;
  LineStringExtracter::getLines(line.get(), populated);
  ASSERT_EQ(populated.size(), 1);
  EXPECT_EQ(populated[0], line.get());
}

TEST(LineStringExtracterTest, ExtractsEmptyLineString) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<LineString> empty_line =
      reader.read<LineString>("LINESTRING EMPTY");

  std::vector<const LineString*> lines =
      LineStringExtracter::getLines(empty_line.get());
  ASSERT_EQ(lines.size(), 1);
  EXPECT_EQ(lines[0], empty_line.get());
  EXPECT_TRUE(lines[0]->isEmpty());
}

TEST(LineStringExtracterTest, SkipsNonLineStringElementalGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Point> pt = reader.read<Point>("POINT (3 4)");
  std::unique_ptr<Point> empty_pt = reader.read<Point>("POINT EMPTY");
  std::unique_ptr<LinearRing> ring =
      reader.read<LinearRing>("LINEARRING (0 0, 1 0, 1 1, 0 0)");
  std::unique_ptr<Polygon> poly = reader.read<Polygon>(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 4 2, 4 4, 2 2))");
  std::unique_ptr<Geometry> circular =
      reader.read("CIRCULARSTRING (0 0, 1 1, 2 0)");

  EXPECT_TRUE(LineStringExtracter::getLines(pt.get()).empty());
  EXPECT_TRUE(LineStringExtracter::getLines(empty_pt.get()).empty());
  EXPECT_TRUE(LineStringExtracter::getLines(ring.get()).empty());
  EXPECT_TRUE(LineStringExtracter::getLines(poly.get()).empty());
  EXPECT_TRUE(LineStringExtracter::getLines(circular.get()).empty());
}

TEST(LineStringExtracterTest, ExtractsFromHomogeneousCollections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<MultiLineString> mls = reader.read<MultiLineString>(
      "MULTILINESTRING ((0 0, 1 0), (2 2, 3 3, 4 2))");
  std::vector<const LineString*> mls_lines =
      LineStringExtracter::getLines(mls.get());
  ASSERT_EQ(mls_lines.size(), 2);
  EXPECT_EQ(mls_lines[0], mls->getGeometryN(0));
  EXPECT_EQ(mls_lines[1], mls->getGeometryN(1));

  std::unique_ptr<MultiLineString> empty_mls =
      reader.read<MultiLineString>("MULTILINESTRING EMPTY");
  EXPECT_TRUE(LineStringExtracter::getLines(empty_mls.get()).empty());

  std::unique_ptr<Geometry> multi_pt = reader.read("MULTIPOINT ((0 0), (1 1))");
  EXPECT_TRUE(LineStringExtracter::getLines(multi_pt.get()).empty());

  std::unique_ptr<Geometry> multi_poly =
      reader.read("MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)))");
  EXPECT_TRUE(LineStringExtracter::getLines(multi_poly.get()).empty());
}

TEST(LineStringExtracterTest, ExtractsRecursivelyFromMixedCollections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<GeometryCollection> empty_gc =
      reader.read<GeometryCollection>("GEOMETRYCOLLECTION EMPTY");
  EXPECT_TRUE(LineStringExtracter::getLines(empty_gc.get()).empty());

  std::unique_ptr<GeometryCollection> mixed_gc =
      reader.read<GeometryCollection>(
          "GEOMETRYCOLLECTION ("
          "  POINT (0 0),"
          "  LINESTRING (1 1, 2 2),"
          "  POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0)),"
          "  MULTILINESTRING ((10 0, 11 0), (12 0, 13 0)),"
          "  GEOMETRYCOLLECTION ("
          "    MULTIPOINT ((3 3), (4 4)),"
          "    LINESTRING EMPTY,"
          "    LINESTRING (20 0, 21 1)"
          "  )"
          ")");

  std::vector<const LineString*> lines =
      LineStringExtracter::getLines(mixed_gc.get());
  ASSERT_EQ(lines.size(), 5);

  const auto* top_line =
      dynamic_cast<const LineString*>(mixed_gc->getGeometryN(1));
  const auto* inner_mls =
      dynamic_cast<const MultiLineString*>(mixed_gc->getGeometryN(3));
  const auto* nested_gc =
      dynamic_cast<const GeometryCollection*>(mixed_gc->getGeometryN(4));
  ASSERT_NE(top_line, nullptr);
  ASSERT_NE(inner_mls, nullptr);
  ASSERT_NE(nested_gc, nullptr);

  EXPECT_EQ(lines[0], top_line);
  EXPECT_EQ(lines[1], inner_mls->getGeometryN(0));
  EXPECT_EQ(lines[2], inner_mls->getGeometryN(1));
  EXPECT_EQ(lines[3], nested_gc->getGeometryN(1));
  EXPECT_EQ(lines[4], nested_gc->getGeometryN(2));
}

TEST(LineStringExtracterTest, SkipsLinearRingInsideCollectionAndAppendsOutput) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<LineString> existing_line =
      reader.read<LineString>("LINESTRING (-1 -1, 0 0)");
  std::unique_ptr<LinearRing> ring =
      reader.read<LinearRing>("LINEARRING (0 0, 1 0, 1 1, 0 0)");
  std::unique_ptr<LineString> second_line =
      reader.read<LineString>("LINESTRING (5 5, 6 6)");
  const LineString* expected_second = second_line.get();

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(std::move(ring));
  geoms.push_back(std::move(second_line));
  std::unique_ptr<GeometryCollection> gc =
      factory->createGeometryCollection(std::move(geoms));

  std::vector<const LineString*> out = {existing_line.get()};
  LineStringExtracter::getLines(gc.get(), out);
  ASSERT_EQ(out.size(), 2);
  EXPECT_EQ(out[0], existing_line.get());
  EXPECT_EQ(out[1], expected_second);
}

TEST(LineStringExtracterTest, DirectFilterRoExtractsOnlyLineStrings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Point> pt = reader.read<Point>("POINT (1 2)");
  std::unique_ptr<LinearRing> ring =
      reader.read<LinearRing>("LINEARRING (0 0, 2 0, 2 2, 0 0)");
  std::unique_ptr<LineString> line =
      reader.read<LineString>("LINESTRING (0 0, 3 4)");
  std::unique_ptr<MultiLineString> mls =
      reader.read<MultiLineString>("MULTILINESTRING ((1 1, 2 2), (3 3, 4 4))");

  std::vector<const LineString*> extracted;
  LineStringExtracter extracter(extracted);

  extracter.filter_ro(pt.get());
  extracter.filter_ro(ring.get());
  EXPECT_TRUE(extracted.empty());

  extracter.filter_ro(line.get());
  ASSERT_EQ(extracted.size(), 1);
  EXPECT_EQ(extracted[0], line.get());

  mls->apply_ro(&extracter);
  ASSERT_EQ(extracted.size(), 3);
  EXPECT_EQ(extracted[1], mls->getGeometryN(0));
  EXPECT_EQ(extracted[2], mls->getGeometryN(1));
}

}  // namespace
}  // namespace geos_google
