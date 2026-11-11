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

// Unit tests for third_party/geos/src/algorithm/Centroid.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for curved-geometry exception
//   assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/Centroid.cpp

#include "third_party/geos/include/geos/algorithm/Centroid.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
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
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::Centroid;
using geos::geom::CircularString;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::util::UnsupportedOperationException;

CoordinateSequence::Ptr MakeSeq(std::initializer_list<CoordinateXY> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

CoordinateSequence::Ptr MakeSeqFromSpan(std::span<const CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords.size(), false, false);
  for (std::size_t i = 0; i < coords.size(); ++i) {
    seq->setAt(coords[i], i);
  }
  return seq;
}

TEST(CentroidTest, EmptyGeometriesReturnFalse) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::vector<std::unique_ptr<Geometry>> empties;
  empties.push_back(factory->createPoint());
  empties.push_back(factory->createLineString());
  empties.push_back(factory->createLinearRing());
  empties.push_back(factory->createPolygon());
  empties.push_back(factory->createMultiPoint());
  empties.push_back(factory->createMultiLineString());
  empties.push_back(factory->createMultiPolygon());
  empties.push_back(factory->createGeometryCollection());

  CoordinateXY cent(99.0, 99.0);
  for (const auto& geom : empties) {
    EXPECT_FALSE(Centroid::getCentroid(*geom, cent));
    Centroid calculator(*geom);
    EXPECT_FALSE(calculator.getCentroid(cent));
  }

  std::unique_ptr<GeometryCollection> collection_of_empties =
      factory->createGeometryCollection(std::move(empties));
  EXPECT_FALSE(Centroid::getCentroid(*collection_of_empties, cent));
  EXPECT_DOUBLE_EQ(cent.x, 99.0);
  EXPECT_DOUBLE_EQ(cent.y, 99.0);
}

TEST(CentroidTest, PointAndMultiPoint) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(3.5, -7.25));
  CoordinateXY cent;
  ASSERT_TRUE(Centroid::getCentroid(*pt, cent));
  EXPECT_DOUBLE_EQ(cent.x, 3.5);
  EXPECT_DOUBLE_EQ(cent.y, -7.25);

  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory->createPoint());
  pts.push_back(factory->createPoint(CoordinateXY(0.0, 0.0)));
  pts.push_back(factory->createPoint(CoordinateXY(3.0, 0.0)));
  pts.push_back(factory->createPoint(CoordinateXY(0.0, 6.0)));
  std::unique_ptr<MultiPoint> mp = factory->createMultiPoint(std::move(pts));
  ASSERT_TRUE(Centroid::getCentroid(*mp, cent));
  EXPECT_DOUBLE_EQ(cent.x, 1.0);
  EXPECT_DOUBLE_EQ(cent.y, 2.0);
}

TEST(CentroidTest, LineStringLinearRingAndMultiLineString) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateXY cent;

  // Single segment.
  std::unique_ptr<LineString> seg =
      factory->createLineString(MakeSeq({{0.0, 0.0}, {4.0, 6.0}}));
  ASSERT_TRUE(Centroid::getCentroid(*seg, cent));
  EXPECT_DOUBLE_EQ(cent.x, 2.0);
  EXPECT_DOUBLE_EQ(cent.y, 3.0);

  // Multi-segment length-weighted centroid:
  // (0,0)->(4,0) len 4 mid (2,0); (4,0)->(4,2) len 2 mid (4,1).
  std::unique_ptr<LineString> l_line =
      factory->createLineString(MakeSeq({{0.0, 0.0}, {4.0, 0.0}, {4.0, 2.0}}));
  ASSERT_TRUE(Centroid::getCentroid(*l_line, cent));
  EXPECT_DOUBLE_EQ(cent.x, 8.0 / 3.0);
  EXPECT_DOUBLE_EQ(cent.y, 1.0 / 3.0);

  // Repeated consecutive vertices produce a zero-length segment that is
  // skipped.
  std::unique_ptr<LineString> repeated =
      factory->createLineString(MakeSeq({{0.0, 0.0}, {0.0, 0.0}, {4.0, 0.0}}));
  ASSERT_TRUE(Centroid::getCentroid(*repeated, cent));
  EXPECT_DOUBLE_EQ(cent.x, 2.0);
  EXPECT_DOUBLE_EQ(cent.y, 0.0);

  // Completely zero-length LineString falls back to point centroid.
  std::unique_ptr<LineString> collapsed_line = factory->createLineString(
      MakeSeq({{7.0, -3.0}, {7.0, -3.0}, {7.0, -3.0}}));
  ASSERT_TRUE(Centroid::getCentroid(*collapsed_line, cent));
  EXPECT_DOUBLE_EQ(cent.x, 7.0);
  EXPECT_DOUBLE_EQ(cent.y, -3.0);

  // LinearRing uses 1D edge-length weighting, distinct from 2D areal centroid.
  // Triangle (0,0)-(3,0)-(0,4)-(0,0): lengths 3, 5, 4; mids (1.5,0), (1.5,2),
  // (0,2). Linear centroid = (1.0, 1.5), whereas areal centroid is (1.0, 4/3).
  std::unique_ptr<LinearRing> ring = factory->createLinearRing(
      MakeSeq({{0.0, 0.0}, {3.0, 0.0}, {0.0, 4.0}, {0.0, 0.0}}));
  ASSERT_TRUE(Centroid::getCentroid(*ring, cent));
  EXPECT_DOUBLE_EQ(cent.x, 1.0);
  EXPECT_DOUBLE_EQ(cent.y, 1.5);

  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(factory->createLineString(MakeSeq({{0.0, 0.0}, {2.0, 0.0}})));
  lines.push_back(factory->createLineString(MakeSeq({{0.0, 4.0}, {4.0, 4.0}})));
  std::unique_ptr<MultiLineString> mls =
      factory->createMultiLineString(std::move(lines));
  ASSERT_TRUE(Centroid::getCentroid(*mls, cent));
  EXPECT_DOUBLE_EQ(cent.x, 5.0 / 3.0);
  EXPECT_DOUBLE_EQ(cent.y, 8.0 / 3.0);
}

TEST(CentroidTest, PolygonSimpleConcaveAndLargeOffset) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateXY cent;

  // L-shaped concave polygon: [0,6]x[0,2] (area 12, center (3,1)) +
  // [0,2]x[2,6] (area 8, center (1,4)) => total area 20, centroid (2.2, 2.2).
  std::unique_ptr<Polygon> l_poly =
      factory->createPolygon(factory->createLinearRing(MakeSeq({{0.0, 0.0},
                                                                {6.0, 0.0},
                                                                {6.0, 2.0},
                                                                {2.0, 2.0},
                                                                {2.0, 6.0},
                                                                {0.0, 6.0},
                                                                {0.0, 0.0}})));
  ASSERT_TRUE(Centroid::getCentroid(*l_poly, cent));
  EXPECT_NEAR(cent.x, 2.2, 1e-12);
  EXPECT_NEAR(cent.y, 2.2, 1e-12);

  // Far-from-origin polygon verifies areaBasePt local translation stability.
  const double offset = 1e8;
  std::unique_ptr<Polygon> shifted = factory->createPolygon(
      factory->createLinearRing(MakeSeq({{offset, offset},
                                         {offset + 2.0, offset},
                                         {offset + 2.0, offset + 4.0},
                                         {offset, offset + 4.0},
                                         {offset, offset}})));
  ASSERT_TRUE(Centroid::getCentroid(*shifted, cent));
  EXPECT_NEAR(cent.x, offset + 1.0, 1e-9);
  EXPECT_NEAR(cent.y, offset + 2.0, 1e-9);
}

TEST(CentroidTest, PolygonWithHolesAllRingOrientationsAndEmptyHole) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Outer [0,4]x[0,4] (area 16, center (2,2)) minus hole [1,2]x[1,2]
  // (area 1, center (1.5,1.5)) => centroid (61/30, 61/30).
  const auto ccw_shell = std::to_array<CoordinateXY>(
      {{0.0, 0.0}, {4.0, 0.0}, {4.0, 4.0}, {0.0, 4.0}, {0.0, 0.0}});
  const auto cw_shell = std::to_array<CoordinateXY>(
      {{0.0, 0.0}, {0.0, 4.0}, {4.0, 4.0}, {4.0, 0.0}, {0.0, 0.0}});
  const auto cw_hole = std::to_array<CoordinateXY>(
      {{1.0, 1.0}, {1.0, 2.0}, {2.0, 2.0}, {2.0, 1.0}, {1.0, 1.0}});
  const auto ccw_hole = std::to_array<CoordinateXY>(
      {{1.0, 1.0}, {2.0, 1.0}, {2.0, 2.0}, {1.0, 2.0}, {1.0, 1.0}});

  for (bool shell_ccw : {true, false}) {
    for (bool hole_ccw : {true, false}) {
      std::unique_ptr<LinearRing> shell = factory->createLinearRing(
          MakeSeqFromSpan(shell_ccw ? ccw_shell : cw_shell));
      std::vector<std::unique_ptr<LinearRing>> holes;
      holes.push_back(factory->createLinearRing(
          MakeSeqFromSpan(hole_ccw ? ccw_hole : cw_hole)));
      std::unique_ptr<Polygon> poly =
          factory->createPolygon(std::move(shell), std::move(holes));

      CoordinateXY cent;
      ASSERT_TRUE(Centroid::getCentroid(*poly, cent));
      EXPECT_NEAR(cent.x, 61.0 / 30.0, 1e-12);
      EXPECT_NEAR(cent.y, 61.0 / 30.0, 1e-12);
    }
  }

  // Polygon with multiple interior holes: [0,6]x[0,6] (area 36, center (3,3))
  // minus [1,2]x[1,2] (area 1, center (1.5,1.5)) and [4,5]x[1,2] (area 1,
  // center (4.5,1.5)) => net area 34, centroid (3.0, 105/34).
  std::vector<std::unique_ptr<LinearRing>> multi_holes;
  multi_holes.push_back(factory->createLinearRing(MakeSeqFromSpan(cw_hole)));
  multi_holes.push_back(factory->createLinearRing(
      MakeSeq({{4.0, 1.0}, {4.0, 2.0}, {5.0, 2.0}, {5.0, 1.0}, {4.0, 1.0}})));
  std::unique_ptr<Polygon> poly_two_holes = factory->createPolygon(
      factory->createLinearRing(MakeSeq(
          {{0.0, 0.0}, {6.0, 0.0}, {6.0, 6.0}, {0.0, 6.0}, {0.0, 0.0}})),
      std::move(multi_holes));
  CoordinateXY cent;
  ASSERT_TRUE(Centroid::getCentroid(*poly_two_holes, cent));
  EXPECT_NEAR(cent.x, 3.0, 1e-12);
  EXPECT_NEAR(cent.y, 105.0 / 34.0, 1e-12);

  // Polygon with an empty interior ring ignores the empty hole.
  std::unique_ptr<LinearRing> outer =
      factory->createLinearRing(MakeSeqFromSpan(ccw_shell));
  std::vector<std::unique_ptr<LinearRing>> empty_holes;
  empty_holes.push_back(factory->createLinearRing());
  std::unique_ptr<Polygon> poly_empty_hole =
      factory->createPolygon(std::move(outer), std::move(empty_holes));
  ASSERT_TRUE(Centroid::getCentroid(*poly_empty_hole, cent));
  EXPECT_DOUBLE_EQ(cent.x, 2.0);
  EXPECT_DOUBLE_EQ(cent.y, 2.0);
}

TEST(CentroidTest, DegeneratePolygonsFallbackToLowerDimensions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateXY cent;

  // Collapsed collinear polygon has zero area but positive perimeter length.
  std::unique_ptr<Polygon> collinear_poly =
      factory->createPolygon(factory->createLinearRing(
          MakeSeq({{0.0, 0.0}, {4.0, 0.0}, {2.0, 0.0}, {0.0, 0.0}})));
  ASSERT_TRUE(Centroid::getCentroid(*collinear_poly, cent));
  EXPECT_DOUBLE_EQ(cent.x, 2.0);
  EXPECT_DOUBLE_EQ(cent.y, 0.0);

  // Zero-area and zero-length polygon falls back to point centroid.
  std::unique_ptr<Polygon> point_poly =
      factory->createPolygon(factory->createLinearRing(
          MakeSeq({{5.0, 7.0}, {5.0, 7.0}, {5.0, 7.0}, {5.0, 7.0}})));
  ASSERT_TRUE(Centroid::getCentroid(*point_poly, cent));
  EXPECT_DOUBLE_EQ(cent.x, 5.0);
  EXPECT_DOUBLE_EQ(cent.y, 7.0);
}

TEST(CentroidTest, MultiPolygonAndHeterogeneousCollections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateXY cent;

  // MultiPolygon: [0,2]x[0,2] (area 4, center (1,1)) +
  // [4,8]x[0,4] (area 16, center (6,2)) => (5.0, 1.8).
  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(factory->createPolygon(factory->createLinearRing(
      MakeSeq({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}}))));
  polys.push_back(factory->createPolygon(factory->createLinearRing(
      MakeSeq({{4.0, 0.0}, {8.0, 0.0}, {8.0, 4.0}, {4.0, 4.0}, {4.0, 0.0}}))));
  std::unique_ptr<MultiPolygon> mp =
      factory->createMultiPolygon(std::move(polys));
  ASSERT_TRUE(Centroid::getCentroid(*mp, cent));
  EXPECT_DOUBLE_EQ(cent.x, 5.0);
  EXPECT_DOUBLE_EQ(cent.y, 1.8);

  // Heterogeneous collection: highest-dimension non-empty geometry dominates.
  std::vector<std::unique_ptr<Geometry>> mixed;
  mixed.push_back(factory->createPoint(CoordinateXY(100.0, 100.0)));
  mixed.push_back(
      factory->createLineString(MakeSeq({{-10.0, 0.0}, {-10.0, 50.0}})));
  mixed.push_back(factory->createPolygon(factory->createLinearRing(
      MakeSeq({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}}))));
  std::unique_ptr<GeometryCollection> gc =
      factory->createGeometryCollection(std::move(mixed));
  Centroid stateful(*gc);
  ASSERT_TRUE(stateful.getCentroid(cent));
  EXPECT_DOUBLE_EQ(cent.x, 1.0);
  EXPECT_DOUBLE_EQ(cent.y, 1.0);

  // Line + Point collection: 1D dominates over 0D.
  std::vector<std::unique_ptr<Geometry>> line_and_point;
  line_and_point.push_back(factory->createPoint(CoordinateXY(50.0, 50.0)));
  line_and_point.push_back(
      factory->createLineString(MakeSeq({{0.0, 0.0}, {4.0, 0.0}})));
  std::unique_ptr<GeometryCollection> gc_line =
      factory->createGeometryCollection(std::move(line_and_point));
  ASSERT_TRUE(Centroid::getCentroid(*gc_line, cent));
  EXPECT_DOUBLE_EQ(cent.x, 2.0);
  EXPECT_DOUBLE_EQ(cent.y, 0.0);

  // Nested collection with zero-area polygon + line combines 1D segments.
  std::vector<std::unique_ptr<Geometry>> inner;
  inner.push_back(factory->createPolygon(factory->createLinearRing(
      MakeSeq({{0.0, 0.0}, {2.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}}))));
  std::vector<std::unique_ptr<Geometry>> outer;
  outer.push_back(factory->createGeometryCollection(std::move(inner)));
  outer.push_back(factory->createLineString(MakeSeq({{0.0, 4.0}, {4.0, 4.0}})));
  std::unique_ptr<GeometryCollection> nested =
      factory->createGeometryCollection(std::move(outer));
  ASSERT_TRUE(Centroid::getCentroid(*nested, cent));
  EXPECT_DOUBLE_EQ(cent.x, 1.5);
  EXPECT_DOUBLE_EQ(cent.y, 2.0);
}

TEST(CentroidTest, ThrowsOnCurvedGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateXY cent;

  std::unique_ptr<CircularString> arc = factory->createCircularString(
      MakeSeq({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  EXPECT_THROW(Centroid::getCentroid(*arc, cent),
               UnsupportedOperationException);

  std::unique_ptr<CircularString> empty_arc =
      factory->createCircularString(false, false);
  EXPECT_THROW(Centroid::getCentroid(*empty_arc, cent),
               UnsupportedOperationException);

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(factory->createPoint(CoordinateXY(0.0, 0.0)));
  geoms.push_back(std::move(arc));
  std::unique_ptr<GeometryCollection> gc =
      factory->createGeometryCollection(std::move(geoms));
  EXPECT_THROW(Centroid::getCentroid(*gc, cent), UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
