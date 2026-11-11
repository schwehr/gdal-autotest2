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
// third_party/geos/src/operation/predicate/RectangleIntersects.cpp.

#include "third_party/geos/include/geos/operation/predicate/RectangleIntersects.h"

#include <memory>
#include <string>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::predicate::RectangleIntersects;

static_assert(!std::is_copy_constructible_v<RectangleIntersects>);
static_assert(!std::is_copy_assignable_v<RectangleIntersects>);

class RectangleIntersectsTest : public ::testing::Test {
 protected:
  RectangleIntersectsTest()
      : factory_(GeometryFactory::create()),
        reader_(*factory_),
        rect_(ReadPolygon("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))")) {}

  std::unique_ptr<Geometry> ReadGeom(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  std::unique_ptr<Polygon> ReadPolygon(const std::string& wkt) const {
    return reader_.read<Polygon>(wkt);
  }

  void ExpectIntersects(const Polygon& rect, const std::string& wkt,
                        bool expected) const {
    SCOPED_TRACE(wkt);
    auto geom = ReadGeom(wkt);
    RectangleIntersects predicate(rect);
    EXPECT_EQ(predicate.intersects(*geom), expected);
    EXPECT_EQ(RectangleIntersects::intersects(rect, *geom), expected);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
  std::unique_ptr<Polygon> rect_;
};

TEST_F(RectangleIntersectsTest, InstanceReuseAcrossMultipleQueries) {
  ASSERT_TRUE(rect_->isRectangle());
  RectangleIntersects predicate(*rect_);

  auto inside = ReadGeom("POINT (5 5)");
  auto outside = ReadGeom("POINT (20 20)");
  auto corner_hit = ReadGeom("LINESTRING (-5 5, 5 15)");
  auto corner_miss = ReadGeom("LINESTRING (-5 7, 3 15)");
  auto empty = ReadGeom("POLYGON EMPTY");

  EXPECT_TRUE(predicate.intersects(*inside));
  EXPECT_FALSE(predicate.intersects(*outside));
  EXPECT_TRUE(predicate.intersects(*corner_hit));
  EXPECT_FALSE(predicate.intersects(*corner_miss));
  EXPECT_FALSE(predicate.intersects(*empty));
  EXPECT_TRUE(predicate.intersects(*inside));
}

TEST_F(RectangleIntersectsTest, EmptyGeometriesAndDisjointEnvelopes) {
  ExpectIntersects(*rect_, "POINT EMPTY", false);
  ExpectIntersects(*rect_, "LINESTRING EMPTY", false);
  ExpectIntersects(*rect_, "POLYGON EMPTY", false);
  ExpectIntersects(*rect_, "MULTIPOINT EMPTY", false);
  ExpectIntersects(*rect_, "MULTILINESTRING EMPTY", false);
  ExpectIntersects(*rect_, "MULTIPOLYGON EMPTY", false);
  ExpectIntersects(*rect_, "GEOMETRYCOLLECTION EMPTY", false);
  ExpectIntersects(*rect_, "GEOMETRYCOLLECTION (POINT EMPTY)", false);

  auto empty_rect = ReadPolygon("POLYGON EMPTY");
  ExpectIntersects(*empty_rect, "POINT (5 5)", false);
  ExpectIntersects(*empty_rect, "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                   false);

  ExpectIntersects(*rect_, "POINT (11 5)", false);
  ExpectIntersects(*rect_, "LINESTRING (-5 -5, -1 -1)", false);
  ExpectIntersects(*rect_, "POLYGON ((12 0, 20 0, 20 10, 12 10, 12 0))", false);
  ExpectIntersects(*rect_,
                   "MULTIPOLYGON (((-10 -10, -5 -10, -5 -5, -10 -5, -10 -10)), "
                   "((15 15, 20 15, 20 20, 15 20, 15 15)))",
                   false);
}

TEST_F(RectangleIntersectsTest, EnvelopeIntersectsVisitorPaths) {
  // Element envelope completely contained inside rectangle envelope.
  ExpectIntersects(*rect_, "POINT (5 5)", true);
  ExpectIntersects(*rect_, "POINT (0 0)", true);
  ExpectIntersects(*rect_, "POINT (5 0)", true);
  ExpectIntersects(*rect_, "LINESTRING (2 2, 8 8)", true);
  ExpectIntersects(*rect_, "LINESTRING (0 0, 10 0)", true);
  ExpectIntersects(*rect_, "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))", true);
  ExpectIntersects(*rect_, "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", true);

  // Element X extent is inside rect X extent while Y extends outside.
  ExpectIntersects(*rect_, "LINESTRING (5 -5, 5 15)", true);
  ExpectIntersects(*rect_, "LINESTRING (2 -5, 8 5)", true);
  ExpectIntersects(*rect_, "LINESTRING (2 5, 8 15)", true);
  ExpectIntersects(*rect_, "LINESTRING (2 -5, 8 0)", true);
  ExpectIntersects(*rect_, "LINESTRING (0 -2, 10 5)", true);
  ExpectIntersects(*rect_, "POLYGON ((3 -5, 7 -5, 7 15, 3 15, 3 -5))", true);

  // Element Y extent is inside rect Y extent while X extends outside.
  ExpectIntersects(*rect_, "LINESTRING (-5 5, 15 5)", true);
  ExpectIntersects(*rect_, "LINESTRING (-5 2, 5 8)", true);
  ExpectIntersects(*rect_, "LINESTRING (5 2, 15 8)", true);
  ExpectIntersects(*rect_, "POLYGON ((-5 3, 15 3, 15 7, -5 7, -5 3))", true);

  // Element passes minX >= rectMinX check but fails maxX <= rectMaxX, then
  // falls through to LineIntersectsVisitor and hits top edge.
  ExpectIntersects(*rect_, "LINESTRING (2 5, 15 15)", true);

  // Collections with leading disjoint/empty elements and trailing hits.
  ExpectIntersects(*rect_, "MULTIPOINT ((-20 -20), (5 5))", true);
  ExpectIntersects(*rect_, "GEOMETRYCOLLECTION (POINT EMPTY, POINT (5 5))",
                   true);
  // Short-circuit when first element in collection hits.
  ExpectIntersects(*rect_, "MULTIPOINT ((5 5), (-20 -20))", true);
  // Overall envelope intersects rectangle, but every element is disjoint.
  ExpectIntersects(*rect_, "MULTIPOINT ((-20 -20), (20 20))", false);
}

TEST_F(RectangleIntersectsTest, ContainsPointVisitorPaths) {
  // Rectangle completely contained inside a larger polygon.
  ExpectIntersects(*rect_, "POLYGON ((-5 -5, 15 -5, 15 15, -5 15, -5 -5))",
                   true);

  // Polygons containing exactly one corner of the rectangle.
  ExpectIntersects(*rect_, "POLYGON ((-5 -5, 6 -5, -5 6, -5 -5))", true);
  ExpectIntersects(*rect_, "POLYGON ((4 -5, 15 -5, 15 6, 4 -5))", true);
  ExpectIntersects(*rect_, "POLYGON ((4 15, 15 15, 15 4, 4 15))", true);
  ExpectIntersects(*rect_, "POLYGON ((-5 4, -5 15, 6 15, -5 4))", true);

  // Rectangle corner lies exactly on polygon boundary (Location::BOUNDARY).
  ExpectIntersects(*rect_, "POLYGON ((5 15, 15 15, 15 5, 5 15))", true);

  // Rectangle corner falls inside polygon's bounding box, but is exterior to
  // the polygon itself.
  ExpectIntersects(*rect_, "POLYGON ((6 15, 15 15, 15 6, 6 15))", false);

  // Rectangle strictly inside a polygon hole (all 4 corners in exterior).
  ExpectIntersects(*rect_,
                   "POLYGON ((-10 -10, 20 -10, 20 20, -10 20, -10 -10), "
                   "(-2 -2, 12 -2, 12 12, -2 12, -2 -2))",
                   false);

  // MultiPolygon with disjoint leading component, then hitting corner.
  ExpectIntersects(
      *rect_,
      "MULTIPOLYGON (((-20 -20, -15 -20, -15 -15, -20 -15, -20 -20)), "
      "((4 15, 15 15, 15 4, 4 15)))",
      true);
  // MultiPolygon short-circuiting on first component.
  ExpectIntersects(*rect_,
                   "MULTIPOLYGON (((4 15, 15 15, 15 4, 4 15)), "
                   "((-20 -20, -15 -20, -15 -15, -20 -15, -20 -20)))",
                   true);
  // MultiPolygon with disjoint leading component and non-intersecting corner.
  ExpectIntersects(
      *rect_,
      "MULTIPOLYGON (((-20 -20, -15 -20, -15 -15, -20 -15, -20 -20)), "
      "((6 15, 15 15, 15 6, 6 15)))",
      false);

  // GeometryCollection mixing non-Polygon and Polygon elements.
  ExpectIntersects(
      *rect_,
      "GEOMETRYCOLLECTION (POINT (-5 15), POLYGON ((4 15, 15 15, 15 4, 4 15)))",
      true);
}

TEST_F(RectangleIntersectsTest, LineIntersectsVisitorPaths) {
  // Diagonal LineString cutting interior near corner without bisecting X or Y.
  ExpectIntersects(*rect_, "LINESTRING (-5 4, 6 15)", true);
  // Diagonal LineString touching a single corner vertex.
  ExpectIntersects(*rect_, "LINESTRING (-5 5, 5 15)", true);
  // Diagonal LineString whose envelope overlaps corner but segment misses rect.
  ExpectIntersects(*rect_, "LINESTRING (-5 7, 3 15)", false);

  // Thin diagonal polygon whose bounding box contains all 4 rect corners while
  // every rect corner is exterior; polygon edges cross rect edges.
  ExpectIntersects(*rect_, "POLYGON ((-2 21, 21 -2, 21.5 -2, -2 21.5, -2 21))",
                   true);

  // Polygon whose exterior encloses rect and whose hole encloses all 4 rect
  // corners, while an interior-ring spike enters the bottom edge of rect.
  ExpectIntersects(*rect_,
                   "POLYGON ((-10 -10, 20 -10, 20 20, -10 20, -10 -10), "
                   "(-1 -1, 4 -1, 5 3, 6 -1, 11 -1, 11 11, -1 11, -1 -1))",
                   true);

  // MultiLineString with disjoint element followed by corner-crossing element.
  ExpectIntersects(*rect_, "MULTILINESTRING ((-20 -20, -15 -15), (-5 4, 6 15))",
                   true);
  // MultiLineString short-circuiting on first element.
  ExpectIntersects(*rect_, "MULTILINESTRING ((-5 4, 6 15), (-20 -20, -15 -15))",
                   true);
  // MultiLineString with disjoint element and non-intersecting corner element.
  ExpectIntersects(*rect_, "MULTILINESTRING ((-20 -20, -15 -15), (-5 7, 3 15))",
                   false);
}

TEST_F(RectangleIntersectsTest, LinearRingAndNestedGeometryCollections) {
  CoordinateSequence hit_ring_seq = CoordinateSequence::XY(0);
  hit_ring_seq.add(CoordinateXY{-5.0, 5.0});
  hit_ring_seq.add(CoordinateXY{5.0, 15.0});
  hit_ring_seq.add(CoordinateXY{-5.0, 15.0});
  hit_ring_seq.add(CoordinateXY{-5.0, 5.0});
  std::unique_ptr<LinearRing> hit_ring =
      factory_->createLinearRing(hit_ring_seq);
  EXPECT_TRUE(RectangleIntersects::intersects(*rect_, *hit_ring));

  CoordinateSequence miss_ring_seq = CoordinateSequence::XY(0);
  miss_ring_seq.add(CoordinateXY{-5.0, 7.0});
  miss_ring_seq.add(CoordinateXY{3.0, 15.0});
  miss_ring_seq.add(CoordinateXY{-5.0, 15.0});
  miss_ring_seq.add(CoordinateXY{-5.0, 7.0});
  std::unique_ptr<LinearRing> miss_ring =
      factory_->createLinearRing(miss_ring_seq);
  EXPECT_FALSE(RectangleIntersects::intersects(*rect_, *miss_ring));

  ExpectIntersects(
      *rect_,
      "GEOMETRYCOLLECTION ("
      "  GEOMETRYCOLLECTION (POINT (-20 -20), LINESTRING (-5 7, 3 15)),"
      "  GEOMETRYCOLLECTION (LINESTRING (-5 4, 6 15))"
      ")",
      true);
  ExpectIntersects(
      *rect_,
      "GEOMETRYCOLLECTION ("
      "  GEOMETRYCOLLECTION (POINT (-20 -20), LINESTRING (-5 7, 3 15)),"
      "  GEOMETRYCOLLECTION (POLYGON ((6 15, 15 15, 15 6, 6 15)))"
      ")",
      false);
}

TEST_F(RectangleIntersectsTest, ReversedRotatedAnd3DRectangles) {
  std::unique_ptr<Polygon> reversed_rect = rect_->reverse();
  ASSERT_TRUE(reversed_rect->isRectangle());
  ExpectIntersects(*reversed_rect, "POLYGON ((4 15, 15 15, 15 4, 4 15))", true);
  ExpectIntersects(*reversed_rect, "LINESTRING (-5 7, 3 15)", false);

  auto shifted_rect = ReadPolygon("POLYGON ((10 10, 0 10, 0 0, 10 0, 10 10))");
  ASSERT_TRUE(shifted_rect->isRectangle());
  ExpectIntersects(*shifted_rect, "POLYGON ((-5 -5, 6 -5, -5 6, -5 -5))", true);
  ExpectIntersects(*shifted_rect, "LINESTRING (-5 4, 6 15)", true);
  ExpectIntersects(*shifted_rect, "POLYGON ((6 15, 15 15, 15 6, 6 15))", false);

  auto rect_3d =
      ReadPolygon("POLYGON Z ((0 0 5, 10 0 5, 10 10 5, 0 10 5, 0 0 5))");
  ASSERT_TRUE(rect_3d->isRectangle());
  ExpectIntersects(*rect_3d, "LINESTRING Z (-5 4 99, 6 15 99)", true);
  ExpectIntersects(*rect_3d, "POLYGON Z ((4 15 1, 15 15 1, 15 4 1, 4 15 1))",
                   true);
  ExpectIntersects(*rect_3d, "LINESTRING Z (-5 7 99, 3 15 99)", false);
}

}  // namespace
}  // namespace geos_google
