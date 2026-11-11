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

#include "third_party/geos/include/geos/algorithm/distance/DistanceToPoint.h"

#include <cmath>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/distance/PointPairDistance.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::algorithm::distance::DistanceToPoint;
using geos::algorithm::distance::PointPairDistance;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineSegment;
using geos::geom::LineString;
using geos::geom::Polygon;
using geos::io::WKTReader;

class DistanceToPointTest : public ::testing::Test {
 protected:
  DistanceToPointTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWKT(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  template <typename T>
  std::unique_ptr<T> ReadWKTAs(const std::string& wkt) const {
    return reader_.read<T>(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(DistanceToPointTest, DefaultConstructorAndEmptyGeometriesResetDistance) {
  DistanceToPoint instance;
  (void)instance;

  const char* const empty_wkts[] = {
      "POINT EMPTY",        "LINESTRING EMPTY",         "LINEARRING EMPTY",
      "POLYGON EMPTY",      "MULTIPOINT EMPTY",         "MULTILINESTRING EMPTY",
      "MULTIPOLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY",
  };

  for (const char* wkt : empty_wkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = ReadWKT(wkt);
    PointPairDistance pt_dist;
    pt_dist.initialize(CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 4.0));
    ASSERT_FALSE(pt_dist.getIsNull());

    DistanceToPoint::computeDistance(*geom, CoordinateXY(1.0, 2.0), pt_dist);
    EXPECT_TRUE(pt_dist.getIsNull());
  }
}

TEST_F(DistanceToPointTest, DirectEmptyTypedOverloadsLeaveDistanceUntouched) {
  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  PointPairDistance pt_dist;
  DistanceToPoint::computeDistance(*empty_line, CoordinateXY(5.0, 5.0),
                                   pt_dist);
  EXPECT_TRUE(pt_dist.getIsNull());

  pt_dist.initialize(CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 2.0));
  DistanceToPoint::computeDistance(*empty_line, CoordinateXY(5.0, 5.0),
                                   pt_dist);
  EXPECT_FALSE(pt_dist.getIsNull());
  EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 2.0);

  std::unique_ptr<Polygon> empty_poly = factory_->createPolygon();
  DistanceToPoint::computeDistance(*empty_poly, CoordinateXY(5.0, 5.0),
                                   pt_dist);
  EXPECT_FALSE(pt_dist.getIsNull());
  EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 2.0);
}

TEST_F(DistanceToPointTest, ComputeDistanceToPointGeometry) {
  std::unique_ptr<Geometry> pt_geom = ReadWKT("POINT (3 4)");
  PointPairDistance pt_dist;

  DistanceToPoint::computeDistance(*pt_geom, CoordinateXY(0.0, 0.0), pt_dist);
  ASSERT_FALSE(pt_dist.getIsNull());
  EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 5.0);
  EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 3.0);
  EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 4.0);
  EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(1).x, 0.0);
  EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(1).y, 0.0);

  // Farther point does not replace closer existing distance.
  std::unique_ptr<Geometry> far_geom = ReadWKT("POINT (100 100)");
  DistanceToPoint::computeDistance(*far_geom, CoordinateXY(0.0, 0.0), pt_dist);
  EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 5.0);
  EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 3.0);
  EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 4.0);

  // Coincident point gives distance zero.
  DistanceToPoint::computeDistance(*pt_geom, CoordinateXY(3.0, 4.0), pt_dist);
  EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 0.0);
  EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 3.0);
  EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 4.0);
}

TEST_F(DistanceToPointTest, ComputeDistanceToLineSegment) {
  // Perpendicular projection to diagonal segment interior.
  {
    LineSegment seg(0.0, 0.0, 10.0, 10.0);
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(seg, CoordinateXY(0.0, 10.0), pt_dist);
    ASSERT_FALSE(pt_dist.getIsNull());
    EXPECT_NEAR(pt_dist.getDistance(), 5.0 * std::sqrt(2.0), 1e-12);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 5.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 5.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(1).x, 0.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(1).y, 10.0);
  }

  // Closest to p0 endpoint.
  {
    LineSegment seg(0.0, 0.0, 10.0, 0.0);
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(seg, CoordinateXY(-3.0, 4.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 5.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 0.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 0.0);
  }

  // Closest to p1 endpoint.
  {
    LineSegment seg(0.0, 0.0, 10.0, 0.0);
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(seg, CoordinateXY(13.0, -4.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 5.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 10.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 0.0);
  }

  // Query point directly on segment and zero-length segment.
  {
    LineSegment seg(0.0, 0.0, 10.0, 0.0);
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(seg, CoordinateXY(6.0, 0.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 0.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 6.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 0.0);

    LineSegment degen(2.0, 3.0, 2.0, 3.0);
    PointPairDistance pt_dist_degen;
    DistanceToPoint::computeDistance(degen, CoordinateXY(5.0, 7.0),
                                     pt_dist_degen);
    EXPECT_DOUBLE_EQ(pt_dist_degen.getDistance(), 5.0);
    EXPECT_DOUBLE_EQ(pt_dist_degen.getCoordinate(0).x, 2.0);
    EXPECT_DOUBLE_EQ(pt_dist_degen.getCoordinate(0).y, 3.0);
  }
}

TEST_F(DistanceToPointTest, ComputeDistanceToLineStringMultiSegment) {
  std::unique_ptr<LineString> line =
      ReadWKTAs<LineString>("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 2)");
  const Geometry& geom = *line;

  // Segment 1 (i = 1): (0,0)-(10,0).
  {
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(geom, CoordinateXY(5.0, 1.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 1.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 5.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 0.0);
  }

  // Segment 2 (i = 2): (10,0)-(10,10).
  {
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(*line, CoordinateXY(9.5, 5.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 0.5);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 10.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 5.0);
  }

  // Segment 3 (i = 3): (10,10)-(0,10).
  {
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(geom, CoordinateXY(4.0, 9.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 1.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 4.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 10.0);
  }

  // Segment 4 (i = 4): (0,10)-(0,2).
  {
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(*line, CoordinateXY(0.25, 6.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 0.25);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 0.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 6.0);
  }
}

TEST_F(DistanceToPointTest, ComputeDistanceToLinearRingViaGeometryDispatch) {
  std::unique_ptr<Geometry> ring =
      ReadWKT("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  ASSERT_EQ(ring->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);

  {
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(*ring, CoordinateXY(5.0, 2.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 2.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 5.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 0.0);
  }

  {
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(*ring, CoordinateXY(13.0, 5.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 3.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 10.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 5.0);
  }
}

TEST_F(DistanceToPointTest, ComputeDistanceToPolygonWithAndWithoutHoles) {
  // Simple polygon without holes: DistanceToPoint measures distance to rings.
  std::unique_ptr<Polygon> simple_poly =
      ReadWKTAs<Polygon>("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  {
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(static_cast<const Geometry&>(*simple_poly),
                                     CoordinateXY(1.0, 5.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 1.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 0.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 5.0);
  }
  {
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(*simple_poly, CoordinateXY(14.0, 13.0),
                                     pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 5.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 10.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 10.0);
  }

  // Polygon with two interior rings (holes).
  std::unique_ptr<Polygon> holed_poly = ReadWKTAs<Polygon>(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 6 2, 6 6, 2 6, 2 2), "
      "(12 12, 18 12, 18 18, 12 18, 12 12))");

  // Closest to interior ring 0.
  {
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(static_cast<const Geometry&>(*holed_poly),
                                     CoordinateXY(4.0, 2.5), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 0.5);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 4.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 2.0);
  }

  // Closest to interior ring 1.
  {
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(*holed_poly, CoordinateXY(15.0, 12.75),
                                     pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 0.75);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 15.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 12.0);
  }
}

TEST_F(DistanceToPointTest, ComputeDistanceToCollections) {
  {
    std::unique_ptr<Geometry> mp = ReadWKT("MULTIPOINT ((10 0), (0 2), (5 5))");
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(*mp, CoordinateXY(0.0, 0.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 2.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 0.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 2.0);
  }

  {
    std::unique_ptr<Geometry> mls =
        ReadWKT("MULTILINESTRING ((0 10, 10 10), (0 2, 10 2))");
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(*mls, CoordinateXY(5.0, 0.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 2.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 5.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 2.0);
  }

  {
    std::unique_ptr<Geometry> mpoly = ReadWKT(
        "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
        "((10 10, 12 10, 12 12, 10 12, 10 10)))");
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(*mpoly, CoordinateXY(5.0, 1.0), pt_dist);
    EXPECT_DOUBLE_EQ(pt_dist.getDistance(), 3.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 2.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 1.0);
  }

  {
    std::unique_ptr<Geometry> gc = ReadWKT(
        "GEOMETRYCOLLECTION (POINT (10 10), LINESTRING (0 5, 10 5), "
        "POLYGON ((20 0, 22 0, 22 2, 20 2, 20 0)), "
        "GEOMETRYCOLLECTION (POINT (1 1)))");
    PointPairDistance pt_dist;
    DistanceToPoint::computeDistance(*gc, CoordinateXY(0.0, 0.0), pt_dist);
    EXPECT_NEAR(pt_dist.getDistance(), std::sqrt(2.0), 1e-12);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).x, 1.0);
    EXPECT_DOUBLE_EQ(pt_dist.getCoordinate(0).y, 1.0);
  }
}

}  // namespace
}  // namespace geos_google
