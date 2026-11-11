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

// Unit tests for third_party/geos/src/algorithm/InteriorPointPoint.cpp and
// third_party/geos/include/geos/algorithm/InteriorPointPoint.h.

#include "third_party/geos/include/geos/algorithm/InteriorPointPoint.h"

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"

namespace geos_google {
namespace {

using geos::algorithm::InteriorPointPoint;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Point;

TEST(InteriorPointPointTest,
     EmptyGeometriesReturnFalseAndLeaveOutputUnchanged) {
  auto factory = GeometryFactory::create();

  auto empty_pt = factory->createPoint();
  auto empty_mp = factory->createMultiPoint();
  auto empty_gc = factory->createGeometryCollection();

  std::vector<std::unique_ptr<Point>> only_empty_pts;
  only_empty_pts.push_back(factory->createPoint());
  only_empty_pts.push_back(factory->createPoint());
  auto mp_of_empty = factory->createMultiPoint(std::move(only_empty_pts));

  for (const Geometry* geom : std::initializer_list<const Geometry*>{
           empty_pt.get(), empty_mp.get(), empty_gc.get(), mp_of_empty.get()}) {
    InteriorPointPoint ipp(geom);
    CoordinateXY out(11.0, 22.0);
    EXPECT_FALSE(ipp.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 11.0);
    EXPECT_DOUBLE_EQ(out.y, 22.0);
  }
}

TEST(InteriorPointPointTest, SinglePointReturnsItsCoordinate) {
  auto factory = GeometryFactory::create();

  auto pt = factory->createPoint(CoordinateXY(3.5, -2.25));
  InteriorPointPoint ipp(pt.get());
  CoordinateXY out;
  ASSERT_TRUE(ipp.getInteriorPoint(out));
  EXPECT_DOUBLE_EQ(out.x, 3.5);
  EXPECT_DOUBLE_EQ(out.y, -2.25);
}

TEST(InteriorPointPointTest, MultiPointSelectsClosestToCentroid) {
  auto factory = GeometryFactory::create();

  // Later point is closer to centroid (14/3, 0).
  {
    auto mp = factory->createMultiPoint(
        std::vector<CoordinateXY>{{0.0, 0.0}, {10.0, 0.0}, {4.0, 0.0}});
    InteriorPointPoint ipp(mp.get());
    CoordinateXY out;
    ASSERT_TRUE(ipp.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 4.0);
    EXPECT_DOUBLE_EQ(out.y, 0.0);
  }

  // First point is already closest to centroid (14/3, 0).
  {
    auto mp = factory->createMultiPoint(
        std::vector<CoordinateXY>{{4.0, 0.0}, {0.0, 0.0}, {10.0, 0.0}});
    InteriorPointPoint ipp(mp.get());
    CoordinateXY out;
    ASSERT_TRUE(ipp.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 4.0);
    EXPECT_DOUBLE_EQ(out.y, 0.0);
  }

  // Equidistant tie preserves the first point encountered.
  {
    auto mp = factory->createMultiPoint(
        std::vector<CoordinateXY>{{0.0, 0.0}, {10.0, 0.0}});
    InteriorPointPoint ipp(mp.get());
    CoordinateXY out;
    ASSERT_TRUE(ipp.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 0.0);
    EXPECT_DOUBLE_EQ(out.y, 0.0);
  }

  // Repeated coordinates pull the centroid toward (0, 0).
  {
    auto mp = factory->createMultiPoint(
        std::vector<CoordinateXY>{{0.0, 0.0}, {0.0, 0.0}, {9.0, 0.0}});
    InteriorPointPoint ipp(mp.get());
    CoordinateXY out;
    ASSERT_TRUE(ipp.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 0.0);
    EXPECT_DOUBLE_EQ(out.y, 0.0);
  }

  // 2D cluster with centroid at (4.8, 5.2).
  {
    auto mp = factory->createMultiPoint(std::vector<CoordinateXY>{
        {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {4.0, 6.0}});
    InteriorPointPoint ipp(mp.get());
    CoordinateXY out;
    ASSERT_TRUE(ipp.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 4.0);
    EXPECT_DOUBLE_EQ(out.y, 6.0);
  }
}

TEST(InteriorPointPointTest, MultiPointSkipsEmptyComponents) {
  auto factory = GeometryFactory::create();

  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory->createPoint());
  pts.push_back(factory->createPoint(CoordinateXY(0.0, 0.0)));
  pts.push_back(factory->createPoint(CoordinateXY(3.0, 0.0)));
  pts.push_back(factory->createPoint(CoordinateXY(10.0, 0.0)));
  auto mp = factory->createMultiPoint(std::move(pts));

  InteriorPointPoint ipp(mp.get());
  CoordinateXY out;
  ASSERT_TRUE(ipp.getInteriorPoint(out));
  EXPECT_DOUBLE_EQ(out.x, 3.0);
  EXPECT_DOUBLE_EQ(out.y, 0.0);
}

TEST(InteriorPointPointTest, GeometryCollectionWithNestedAndMixedGeometries) {
  auto factory = GeometryFactory::create();

  // LineString dominates Centroid over Points -> centroid is (5, 0).
  // Non-Point components are ignored when choosing the interior point.
  {
    std::vector<std::unique_ptr<Geometry>> geoms;
    geoms.push_back(factory->createPoint());
    geoms.push_back(
        factory->createLineString(std::make_unique<CoordinateSequence>(
            std::initializer_list<CoordinateXY>{{0.0, 0.0}, {10.0, 0.0}})));
    geoms.push_back(factory->createPoint(CoordinateXY(1.0, 0.0)));
    geoms.push_back(factory->createMultiPoint(
        std::vector<CoordinateXY>{{6.0, 0.0}, {9.0, 0.0}}));
    auto gc = factory->createGeometryCollection(std::move(geoms));

    InteriorPointPoint ipp(gc.get());
    CoordinateXY out;
    ASSERT_TRUE(ipp.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 6.0);
    EXPECT_DOUBLE_EQ(out.y, 0.0);
  }

  // Polygon dominates Centroid -> centroid is (5, 5).
  {
    auto shell = factory->createLinearRing(std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXY>{
            {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}));
    std::vector<std::unique_ptr<Geometry>> geoms;
    geoms.push_back(factory->createPolygon(std::move(shell)));
    geoms.push_back(factory->createPoint(CoordinateXY(1.0, 1.0)));
    geoms.push_back(factory->createPoint(CoordinateXY(5.0, 4.0)));
    auto gc = factory->createGeometryCollection(std::move(geoms));

    InteriorPointPoint ipp(gc.get());
    CoordinateXY out;
    ASSERT_TRUE(ipp.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 5.0);
    EXPECT_DOUBLE_EQ(out.y, 4.0);
  }
}

TEST(InteriorPointPointTest, HigherDimensionPointsWriteXYOnly) {
  auto factory = GeometryFactory::create();

  auto pt_xyz = factory->createPoint(Coordinate(2.0, 3.0, 99.0));
  InteriorPointPoint ipp_xyz(pt_xyz.get());
  Coordinate out_xyz(0.0, 0.0, 77.0);
  ASSERT_TRUE(ipp_xyz.getInteriorPoint(out_xyz));
  EXPECT_DOUBLE_EQ(out_xyz.x, 2.0);
  EXPECT_DOUBLE_EQ(out_xyz.y, 3.0);
  EXPECT_DOUBLE_EQ(out_xyz.z, 77.0);

  auto pt_xyzm = factory->createPoint(CoordinateXYZM(-1.5, 4.5, 10.0, 20.0));
  InteriorPointPoint ipp_xyzm(pt_xyzm.get());
  CoordinateXY out_xy;
  ASSERT_TRUE(ipp_xyzm.getInteriorPoint(out_xy));
  EXPECT_DOUBLE_EQ(out_xy.x, -1.5);
  EXPECT_DOUBLE_EQ(out_xy.y, 4.5);
}

TEST(InteriorPointPointTest, MatchesGeometryGetInteriorPoint) {
  auto factory = GeometryFactory::create();

  auto mp = factory->createMultiPoint(
      std::vector<CoordinateXY>{{-3.0, -4.0}, {2.0, 1.0}, {10.0, 8.0}});
  InteriorPointPoint ipp(mp.get());
  CoordinateXY c;
  ASSERT_TRUE(ipp.getInteriorPoint(c));

  auto geom_pt = mp->getInteriorPoint();
  ASSERT_FALSE(geom_pt->isEmpty());
  EXPECT_DOUBLE_EQ(c.x, geom_pt->getX());
  EXPECT_DOUBLE_EQ(c.y, geom_pt->getY());
}

}  // namespace
}  // namespace geos_google
