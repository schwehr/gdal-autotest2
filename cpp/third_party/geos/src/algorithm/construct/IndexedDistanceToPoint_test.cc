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

#include "third_party/geos/include/geos/algorithm/construct/IndexedDistanceToPoint.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::algorithm::construct::IndexedDistanceToPoint;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Point;
using geos::io::WKTReader;
using geos::util::GEOSException;

class IndexedDistanceToPointTest : public ::testing::Test {
 protected:
  IndexedDistanceToPointTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadGeom(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  std::unique_ptr<Point> ReadPoint(const std::string& wkt) const {
    return reader_.read<Point>(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(IndexedDistanceToPointTest, DistanceAndNearestPointsToPolygon) {
  auto poly = ReadGeom("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  IndexedDistanceToPoint indexed(*poly);

  // Strictly interior point has distance 0 (first call initializes indexes).
  EXPECT_DOUBLE_EQ(indexed.distance(*ReadPoint("POINT (5 5)")), 0.0);

  // Boundary edge and vertex points also have distance 0.
  EXPECT_DOUBLE_EQ(indexed.distance(*ReadPoint("POINT (0 5)")), 0.0);
  EXPECT_DOUBLE_EQ(indexed.distance(*ReadPoint("POINT (10 10)")), 0.0);

  // Exterior point computes Euclidean distance to nearest polygon corner.
  auto outside_pt = ReadPoint("POINT (13 14)");
  EXPECT_NEAR(indexed.distance(*outside_pt), 5.0, 1e-12);

  auto nearest_outside = indexed.nearestPoints(*outside_pt);
  ASSERT_EQ(nearest_outside->size(), 2u);
  EXPECT_TRUE(nearest_outside->getAt<CoordinateXY>(0).equals2D(
      CoordinateXY(10.0, 10.0), 1e-12));
  EXPECT_TRUE(nearest_outside->getAt<CoordinateXY>(1).equals2D(
      CoordinateXY(13.0, 14.0), 1e-12));

  // nearestPoints for an interior point returns the closest boundary facet
  // point and the query point.
  auto nearest_inside = indexed.nearestPoints(*ReadPoint("POINT (2 5)"));
  ASSERT_EQ(nearest_inside->size(), 2u);
  EXPECT_TRUE(nearest_inside->getAt<CoordinateXY>(0).equals2D(
      CoordinateXY(0.0, 5.0), 1e-12));
  EXPECT_TRUE(nearest_inside->getAt<CoordinateXY>(1).equals2D(
      CoordinateXY(2.0, 5.0), 1e-12));
}

TEST_F(IndexedDistanceToPointTest, DistanceAndNearestPointsWithPolygonHole) {
  auto poly = ReadGeom(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2))");
  IndexedDistanceToPoint indexed(*poly);

  EXPECT_DOUBLE_EQ(indexed.distance(*ReadPoint("POINT (1 5)")), 0.0);
  EXPECT_DOUBLE_EQ(indexed.distance(*ReadPoint("POINT (2 5)")), 0.0);

  // Point inside the hole is in the exterior and has positive distance to
  // the hole ring.
  auto hole_pt = ReadPoint("POINT (5 4)");
  EXPECT_NEAR(indexed.distance(*hole_pt), 2.0, 1e-12);

  auto nearest = indexed.nearestPoints(*hole_pt);
  ASSERT_EQ(nearest->size(), 2u);
  EXPECT_TRUE(
      nearest->getAt<CoordinateXY>(0).equals2D(CoordinateXY(5.0, 2.0), 1e-12));
  EXPECT_TRUE(
      nearest->getAt<CoordinateXY>(1).equals2D(CoordinateXY(5.0, 4.0), 1e-12));
}

TEST_F(IndexedDistanceToPointTest,
       NearestPointsInitializesLazilyBeforeDistance) {
  auto poly = ReadGeom("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  IndexedDistanceToPoint indexed(*poly);

  auto nearest = indexed.nearestPoints(*ReadPoint("POINT (7 4)"));
  ASSERT_EQ(nearest->size(), 2u);
  EXPECT_TRUE(
      nearest->getAt<CoordinateXY>(0).equals2D(CoordinateXY(4.0, 4.0), 1e-12));
  EXPECT_TRUE(
      nearest->getAt<CoordinateXY>(1).equals2D(CoordinateXY(7.0, 4.0), 1e-12));

  // Subsequent distance() reuses the already-initialized index.
  EXPECT_DOUBLE_EQ(indexed.distance(*ReadPoint("POINT (2 2)")), 0.0);
  EXPECT_NEAR(indexed.distance(*ReadPoint("POINT (7 8)")), 5.0, 1e-12);
}

TEST_F(IndexedDistanceToPointTest, DistanceToLineStringAndPointGeometries) {
  {
    auto line = ReadGeom("LINESTRING (0 0, 10 0)");
    IndexedDistanceToPoint indexed(*line);

    EXPECT_DOUBLE_EQ(indexed.distance(*ReadPoint("POINT (5 0)")), 0.0);
    auto off_pt = ReadPoint("POINT (3 4)");
    EXPECT_NEAR(indexed.distance(*off_pt), 4.0, 1e-12);

    auto nearest = indexed.nearestPoints(*off_pt);
    ASSERT_EQ(nearest->size(), 2u);
    EXPECT_TRUE(nearest->getAt<CoordinateXY>(0).equals2D(CoordinateXY(3.0, 0.0),
                                                         1e-12));
    EXPECT_TRUE(nearest->getAt<CoordinateXY>(1).equals2D(CoordinateXY(3.0, 4.0),
                                                         1e-12));
  }
  {
    auto target_pt = ReadGeom("POINT (2 3)");
    IndexedDistanceToPoint indexed(*target_pt);

    EXPECT_DOUBLE_EQ(indexed.distance(*ReadPoint("POINT (2 3)")), 0.0);
    auto other_pt = ReadPoint("POINT (5 7)");
    EXPECT_NEAR(indexed.distance(*other_pt), 5.0, 1e-12);

    auto nearest = indexed.nearestPoints(*other_pt);
    ASSERT_EQ(nearest->size(), 2u);
    EXPECT_TRUE(nearest->getAt<CoordinateXY>(0).equals2D(CoordinateXY(2.0, 3.0),
                                                         1e-12));
    EXPECT_TRUE(nearest->getAt<CoordinateXY>(1).equals2D(CoordinateXY(5.0, 7.0),
                                                         1e-12));
  }
}

TEST_F(IndexedDistanceToPointTest,
       DistanceToMultiPolygonAndHeterogeneousCollection) {
  auto multipoly = ReadGeom(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((10 0, 12 0, 12 2, 10 2, 10 0)))");
  IndexedDistanceToPoint mp_indexed(*multipoly);
  EXPECT_DOUBLE_EQ(mp_indexed.distance(*ReadPoint("POINT (11 1)")), 0.0);
  EXPECT_NEAR(mp_indexed.distance(*ReadPoint("POINT (5 1)")), 3.0, 1e-12);

  auto collection = ReadGeom(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "LINESTRING (10 0, 10 4), "
      "POINT (20 0))");
  IndexedDistanceToPoint gc_indexed(*collection);
  EXPECT_DOUBLE_EQ(gc_indexed.distance(*ReadPoint("POINT (2 2)")), 0.0);
  EXPECT_NEAR(gc_indexed.distance(*ReadPoint("POINT (13 4)")), 3.0, 1e-12);
  EXPECT_NEAR(gc_indexed.distance(*ReadPoint("POINT (20 2)")), 2.0, 1e-12);
}

TEST_F(IndexedDistanceToPointTest, ThrowsOnEmptyTargetGeometry) {
  auto empty_poly = ReadGeom("POLYGON EMPTY");
  IndexedDistanceToPoint poly_indexed(*empty_poly);
  auto query = ReadPoint("POINT (1 1)");
  EXPECT_THROW(poly_indexed.distance(*query), GEOSException);
  EXPECT_THROW(poly_indexed.nearestPoints(*query), GEOSException);

  auto empty_gc = ReadGeom("GEOMETRYCOLLECTION EMPTY");
  IndexedDistanceToPoint gc_indexed(*empty_gc);
  EXPECT_THROW(gc_indexed.distance(*query), GEOSException);
}

}  // namespace
}  // namespace geos_google
