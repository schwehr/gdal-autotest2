// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/operation/distance/GeometryLocation.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::GeometryFactory;
using ::geos::geom::LinearRing;
using ::geos::geom::LineString;
using ::geos::geom::Point;
using ::geos::geom::Polygon;
using ::geos::operation::distance::GeometryLocation;

TEST(GeometryLocationTest, DefaultConstructorInitializesZeroedFields) {
  GeometryLocation loc;
  EXPECT_EQ(loc.getGeometryComponent(), nullptr);
  EXPECT_EQ(loc.getSegmentIndex(), 0u);
  EXPECT_FALSE(loc.isInsideArea());
  EXPECT_DOUBLE_EQ(loc.getCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(loc.getCoordinate().y, 0.0);
}

TEST(GeometryLocationTest,
     SegmentConstructorStoresComponentIndexAndCoordinate) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> line =
      factory->createLineString(CoordinateSequence{
          Coordinate(0.0, 0.0), Coordinate(10.0, 0.0), Coordinate(10.0, 10.0)});

  GeometryLocation loc(line.get(), 1, CoordinateXY(10.0, 4.5));
  EXPECT_EQ(loc.getGeometryComponent(), line.get());
  EXPECT_EQ(loc.getSegmentIndex(), 1u);
  EXPECT_FALSE(loc.isInsideArea());
  EXPECT_DOUBLE_EQ(loc.getCoordinate().x, 10.0);
  EXPECT_DOUBLE_EQ(loc.getCoordinate().y, 4.5);
}

TEST(GeometryLocationTest, SegmentConstructorAllowsNullComponent) {
  GeometryLocation loc(nullptr, 3, CoordinateXY(-2.5, 7.25));
  EXPECT_EQ(loc.getGeometryComponent(), nullptr);
  EXPECT_EQ(loc.getSegmentIndex(), 3u);
  EXPECT_FALSE(loc.isInsideArea());
  EXPECT_DOUBLE_EQ(loc.getCoordinate().x, -2.5);
  EXPECT_DOUBLE_EQ(loc.getCoordinate().y, 7.25);
}

TEST(GeometryLocationTest, InsideAreaConstructorSetsFlagAndSentinelIndex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> shell =
      factory->createLinearRing(CoordinateSequence{
          Coordinate(0.0, 0.0), Coordinate(10.0, 0.0), Coordinate(10.0, 10.0),
          Coordinate(0.0, 10.0), Coordinate(0.0, 0.0)});
  std::unique_ptr<Polygon> poly = factory->createPolygon(std::move(shell));

  GeometryLocation loc(poly.get(), CoordinateXY(5.0, 5.0));
  const int inside_area_sentinel = +GeometryLocation::INSIDE_AREA;
  EXPECT_EQ(inside_area_sentinel, -1);
  EXPECT_EQ(loc.getGeometryComponent(), poly.get());
  EXPECT_TRUE(loc.isInsideArea());
  EXPECT_EQ(loc.getSegmentIndex(),
            static_cast<std::size_t>(inside_area_sentinel));
  EXPECT_DOUBLE_EQ(loc.getCoordinate().x, 5.0);
  EXPECT_DOUBLE_EQ(loc.getCoordinate().y, 5.0);
}

TEST(GeometryLocationTest, GetCoordinateReturnsMutableReference) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.0, 2.0));

  GeometryLocation loc(pt.get(), 0, CoordinateXY(1.0, 2.0));
  CoordinateXY& coord = loc.getCoordinate();
  coord.x = 3.5;
  coord.y = -4.25;

  EXPECT_DOUBLE_EQ(loc.getCoordinate().x, 3.5);
  EXPECT_DOUBLE_EQ(loc.getCoordinate().y, -4.25);
  EXPECT_EQ(loc.toString(), "Point[0]-POINT (3.5 -4.25 )");
}

TEST(GeometryLocationTest, ToStringFormatsComponentTypeIndexAndPoint) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.5, 2.5));
  std::unique_ptr<LineString> line =
      factory->createLineString(CoordinateSequence{
          Coordinate(0.0, 0.0), Coordinate(5.0, 0.0), Coordinate(5.0, 5.0)});
  std::unique_ptr<LinearRing> shell =
      factory->createLinearRing(CoordinateSequence{
          Coordinate(0.0, 0.0), Coordinate(10.0, 0.0), Coordinate(10.0, 10.0),
          Coordinate(0.0, 10.0), Coordinate(0.0, 0.0)});
  std::unique_ptr<Polygon> poly = factory->createPolygon(std::move(shell));

  GeometryLocation pt_loc(pt.get(), 0, CoordinateXY(1.5, 2.5));
  EXPECT_EQ(pt_loc.toString(), "Point[0]-POINT (1.5 2.5 )");

  GeometryLocation line_loc(line.get(), 1, CoordinateXY(5.0, 3.0));
  EXPECT_EQ(line_loc.toString(), "LineString[1]-POINT (5 3 )");

  const int inside_area_sentinel = +GeometryLocation::INSIDE_AREA;
  GeometryLocation area_loc(poly.get(), CoordinateXY(2.0, 3.0));
  EXPECT_EQ(area_loc.toString(),
            "Polygon[" +
                std::to_string(static_cast<std::size_t>(inside_area_sentinel)) +
                "]-POINT (2 3 )");
}

TEST(GeometryLocationTest, CopyConstructionAndAssignmentPreserveState) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> line = factory->createLineString(
      CoordinateSequence{Coordinate(0.0, 0.0), Coordinate(4.0, 4.0)});
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(9.0, 8.0));

  GeometryLocation original(line.get(), 2, CoordinateXY(1.5, 1.5));
  GeometryLocation copied(original);
  EXPECT_EQ(copied.getGeometryComponent(), line.get());
  EXPECT_EQ(copied.getSegmentIndex(), 2u);
  EXPECT_FALSE(copied.isInsideArea());
  EXPECT_DOUBLE_EQ(copied.getCoordinate().x, 1.5);
  EXPECT_DOUBLE_EQ(copied.getCoordinate().y, 1.5);

  GeometryLocation assigned(pt.get(), CoordinateXY(9.0, 8.0));
  EXPECT_TRUE(assigned.isInsideArea());
  assigned = original;
  EXPECT_EQ(assigned.getGeometryComponent(), line.get());
  EXPECT_EQ(assigned.getSegmentIndex(), 2u);
  EXPECT_FALSE(assigned.isInsideArea());
  EXPECT_DOUBLE_EQ(assigned.getCoordinate().x, 1.5);
  EXPECT_DOUBLE_EQ(assigned.getCoordinate().y, 1.5);

  // Mutating the copy does not alter the original.
  copied.getCoordinate().x = 99.0;
  EXPECT_DOUBLE_EQ(original.getCoordinate().x, 1.5);
}

}  // namespace
}  // namespace geos_google
