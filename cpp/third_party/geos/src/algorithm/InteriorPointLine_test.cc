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

// Unit tests for third_party/geos/src/algorithm/InteriorPointLine.cpp and
// third_party/geos/include/geos/algorithm/InteriorPointLine.h.

#include "third_party/geos/include/geos/algorithm/InteriorPointLine.h"

#include <cmath>
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
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"

namespace geos_google {
namespace {

using geos::algorithm::InteriorPointLine;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;

std::unique_ptr<LineString> MakeLineXY(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  return factory.createLineString(std::make_unique<CoordinateSequence>(coords));
}

std::unique_ptr<LineString> MakeLineXYZ(
    const GeometryFactory& factory, std::initializer_list<Coordinate> coords) {
  return factory.createLineString(std::make_unique<CoordinateSequence>(coords));
}

std::unique_ptr<LinearRing> MakeRingXY(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  return factory.createLinearRing(std::make_unique<CoordinateSequence>(coords));
}

TEST(InteriorPointLineTest, EmptyGeometriesReturnFalseAndLeaveOutputUnchanged) {
  auto factory = GeometryFactory::create();

  auto empty_line = factory->createLineString();
  auto empty_mls = factory->createMultiLineString();
  auto empty_gc = factory->createGeometryCollection();

  for (const Geometry* geom : std::initializer_list<const Geometry*>{
           empty_line.get(), empty_mls.get(), empty_gc.get()}) {
    InteriorPointLine ipl(geom);
    Coordinate out(7.0, 8.0, 9.0);
    EXPECT_FALSE(ipl.getInteriorPoint(out));
    EXPECT_TRUE(out.equals3D(Coordinate(7.0, 8.0, 9.0)));
  }
}

TEST(InteriorPointLineTest, NonLinearGeometriesReturnFalse) {
  auto factory = GeometryFactory::create();

  auto point = factory->createPoint(CoordinateXY(3.0, 4.0));
  {
    InteriorPointLine ipl(point.get());
    Coordinate out(1.0, 2.0, 3.0);
    EXPECT_FALSE(ipl.getInteriorPoint(out));
    EXPECT_TRUE(out.equals3D(Coordinate(1.0, 2.0, 3.0)));
  }

  auto polygon = factory->createPolygon(MakeRingXY(
      *factory,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}));
  {
    InteriorPointLine ipl(polygon.get());
    Coordinate out;
    EXPECT_FALSE(ipl.getInteriorPoint(out));
  }

  std::vector<std::unique_ptr<Geometry>> pts;
  pts.push_back(factory->createPoint(CoordinateXY(0.0, 0.0)));
  pts.push_back(factory->createPoint(CoordinateXY(5.0, 5.0)));
  auto gc_points = factory->createGeometryCollection(std::move(pts));
  {
    InteriorPointLine ipl(gc_points.get());
    Coordinate out;
    EXPECT_FALSE(ipl.getInteriorPoint(out));
  }
}

TEST(InteriorPointLineTest, TwoVertexLineSelectsEndpoint) {
  auto factory = GeometryFactory::create();

  // Centroid is (5, 0); both endpoints are equidistant, so the first wins.
  auto line = MakeLineXY(*factory, {{0.0, 0.0}, {10.0, 0.0}});
  InteriorPointLine ipl(line.get());
  Coordinate out;
  ASSERT_TRUE(ipl.getInteriorPoint(out));
  EXPECT_DOUBLE_EQ(out.x, 0.0);
  EXPECT_DOUBLE_EQ(out.y, 0.0);
  EXPECT_TRUE(std::isnan(out.z));

  auto reversed = MakeLineXY(*factory, {{10.0, 0.0}, {0.0, 0.0}});
  InteriorPointLine ipl_rev(reversed.get());
  ASSERT_TRUE(ipl_rev.getInteriorPoint(out));
  EXPECT_DOUBLE_EQ(out.x, 10.0);
  EXPECT_DOUBLE_EQ(out.y, 0.0);
}

TEST(InteriorPointLineTest, ZeroLengthLineSelectsEndpoint) {
  auto factory = GeometryFactory::create();

  auto degenerate = MakeLineXY(*factory, {{3.0, 4.0}, {3.0, 4.0}});
  InteriorPointLine ipl(degenerate.get());
  Coordinate out;
  ASSERT_TRUE(ipl.getInteriorPoint(out));
  EXPECT_DOUBLE_EQ(out.x, 3.0);
  EXPECT_DOUBLE_EQ(out.y, 4.0);
}

TEST(InteriorPointLineTest, ThreeVertexLineSelectsInteriorVertex) {
  auto factory = GeometryFactory::create();

  auto line = MakeLineXY(*factory, {{0.0, 0.0}, {4.0, 0.0}, {10.0, 0.0}});
  InteriorPointLine ipl(line.get());
  Coordinate out;
  ASSERT_TRUE(ipl.getInteriorPoint(out));
  EXPECT_DOUBLE_EQ(out.x, 4.0);
  EXPECT_DOUBLE_EQ(out.y, 0.0);

  auto corner = MakeLineXY(*factory, {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}});
  InteriorPointLine ipl_corner(corner.get());
  ASSERT_TRUE(ipl_corner.getInteriorPoint(out));
  EXPECT_DOUBLE_EQ(out.x, 10.0);
  EXPECT_DOUBLE_EQ(out.y, 0.0);
}

TEST(InteriorPointLineTest, MultipleInteriorVerticesSelectsClosestToCentroid) {
  auto factory = GeometryFactory::create();

  // Later interior vertex is closer to centroid (5, 0).
  auto line_later =
      MakeLineXY(*factory, {{0.0, 0.0}, {1.0, 0.0}, {6.0, 0.0}, {10.0, 0.0}});
  {
    InteriorPointLine ipl(line_later.get());
    Coordinate out;
    ASSERT_TRUE(ipl.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 6.0);
    EXPECT_DOUBLE_EQ(out.y, 0.0);
  }

  // Earlier interior vertex is closer to centroid (5, 0).
  auto line_earlier =
      MakeLineXY(*factory, {{0.0, 0.0}, {4.0, 0.0}, {9.0, 0.0}, {10.0, 0.0}});
  {
    InteriorPointLine ipl(line_earlier.get());
    Coordinate out;
    ASSERT_TRUE(ipl.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 4.0);
    EXPECT_DOUBLE_EQ(out.y, 0.0);
  }

  // Tie between interior vertices keeps the first encountered.
  auto line_tie =
      MakeLineXY(*factory, {{0.0, 0.0}, {3.0, 0.0}, {7.0, 0.0}, {10.0, 0.0}});
  {
    InteriorPointLine ipl(line_tie.get());
    Coordinate out;
    ASSERT_TRUE(ipl.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 3.0);
    EXPECT_DOUBLE_EQ(out.y, 0.0);
  }
}

TEST(InteriorPointLineTest,
     InteriorVertexPreferredEvenWhenEndpointCloserToCentroid) {
  auto factory = GeometryFactory::create();

  // Centroid is around (7.07, 0); endpoint (0, 0) is closer to centroid (~7.07)
  // than interior vertices (10, 10) and (10, -10) (~10.42), yet an interior
  // vertex must still be selected.
  auto loop = MakeLineXY(*factory,
                         {{0.0, 0.0}, {10.0, 10.0}, {10.0, -10.0}, {0.0, 0.0}});
  InteriorPointLine ipl(loop.get());
  Coordinate out;
  ASSERT_TRUE(ipl.getInteriorPoint(out));
  EXPECT_DOUBLE_EQ(out.x, 10.0);
  EXPECT_DOUBLE_EQ(out.y, 10.0);
}

TEST(InteriorPointLineTest, LinearRingTreatedAsLineString) {
  auto factory = GeometryFactory::create();

  // Centroid is ~(6.464, 3.536); interior vertex (10, 0) is distance 5.0 while
  // (10, 10) is ~7.37.
  auto ring =
      MakeRingXY(*factory, {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 0.0}});
  InteriorPointLine ipl(ring.get());
  Coordinate out;
  ASSERT_TRUE(ipl.getInteriorPoint(out));
  EXPECT_DOUBLE_EQ(out.x, 10.0);
  EXPECT_DOUBLE_EQ(out.y, 0.0);
}

TEST(InteriorPointLineTest, MultiLineStringWithEmptyAndInteriorComponents) {
  auto factory = GeometryFactory::create();

  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(factory->createLineString());
  lines.push_back(MakeLineXY(*factory, {{0.0, 0.0}, {10.0, 0.0}}));
  lines.push_back(MakeLineXY(*factory, {{2.0, 1.0}, {4.0, 1.0}, {6.0, 1.0}}));
  auto mls = factory->createMultiLineString(std::move(lines));

  InteriorPointLine ipl(mls.get());
  Coordinate out;
  ASSERT_TRUE(ipl.getInteriorPoint(out));
  EXPECT_DOUBLE_EQ(out.x, 4.0);
  EXPECT_DOUBLE_EQ(out.y, 1.0);
}

TEST(InteriorPointLineTest,
     MultiLineStringWithoutInteriorVerticesFallsBackToEndpoints) {
  auto factory = GeometryFactory::create();

  // Weighted centroid of segment1 ((0,0)-(10,0), len 10) and segment2
  // ((2,3)-(2,1), len 2) is (4.5, 1/3). Endpoint (2, 1) is closest (~2.59).
  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(factory->createLineString());
  lines.push_back(MakeLineXY(*factory, {{0.0, 0.0}, {10.0, 0.0}}));
  lines.push_back(MakeLineXY(*factory, {{2.0, 3.0}, {2.0, 1.0}}));
  auto mls = factory->createMultiLineString(std::move(lines));

  InteriorPointLine ipl(mls.get());
  Coordinate out;
  ASSERT_TRUE(ipl.getInteriorPoint(out));
  EXPECT_DOUBLE_EQ(out.x, 2.0);
  EXPECT_DOUBLE_EQ(out.y, 1.0);
}

TEST(InteriorPointLineTest, MixedAndNestedGeometryCollections) {
  auto factory = GeometryFactory::create();

  // Nested collection with interior vertices plus non-linear geometries.
  {
    std::vector<std::unique_ptr<LineString>> inner_lines;
    inner_lines.push_back(
        MakeLineXY(*factory, {{0.0, 0.0}, {1.0, 1.0}, {5.0, 5.0}}));
    inner_lines.push_back(
        MakeLineXY(*factory, {{10.0, 10.0}, {10.0, 11.0}, {10.0, 12.0}}));
    auto inner_mls = factory->createMultiLineString(std::move(inner_lines));

    std::vector<std::unique_ptr<Geometry>> geoms;
    geoms.push_back(factory->createPoint(CoordinateXY(100.0, 100.0)));
    geoms.push_back(factory->createPolygon(MakeRingXY(
        *factory,
        {{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}})));
    geoms.push_back(std::move(inner_mls));
    auto gc = factory->createGeometryCollection(std::move(geoms));

    InteriorPointLine ipl(gc.get());
    Coordinate out;
    ASSERT_TRUE(ipl.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 1.0);
    EXPECT_DOUBLE_EQ(out.y, 1.0);
  }

  // Mixed collection with only 2-vertex lines falls back to addEndpoints,
  // skipping non-linear elements in both passes.
  {
    std::vector<std::unique_ptr<Geometry>> geoms;
    geoms.push_back(factory->createPoint(CoordinateXY(5.0, 5.0)));
    geoms.push_back(MakeLineXY(*factory, {{0.0, 0.0}, {1.0, 0.0}}));
    geoms.push_back(MakeLineXY(*factory, {{4.0, 5.0}, {10.0, 5.0}}));
    auto gc = factory->createGeometryCollection(std::move(geoms));

    InteriorPointLine ipl(gc.get());
    Coordinate out;
    ASSERT_TRUE(ipl.getInteriorPoint(out));
    EXPECT_DOUBLE_EQ(out.x, 4.0);
    EXPECT_DOUBLE_EQ(out.y, 5.0);
  }
}

TEST(InteriorPointLineTest, PreservesZOrdinateFromSelectedVertex) {
  auto factory = GeometryFactory::create();

  auto line = MakeLineXYZ(
      *factory, {{0.0, 0.0, 10.0}, {4.0, 0.0, 25.0}, {10.0, 0.0, 30.0}});
  InteriorPointLine ipl(line.get());
  Coordinate out;
  ASSERT_TRUE(ipl.getInteriorPoint(out));
  EXPECT_DOUBLE_EQ(out.x, 4.0);
  EXPECT_DOUBLE_EQ(out.y, 0.0);
  EXPECT_DOUBLE_EQ(out.z, 25.0);
}

TEST(InteriorPointLineTest, MatchesGeometryGetInteriorPoint) {
  auto factory = GeometryFactory::create();

  auto line =
      MakeLineXY(*factory, {{0.0, 0.0}, {2.0, 5.0}, {8.0, 1.0}, {10.0, 0.0}});
  InteriorPointLine ipl(line.get());
  Coordinate c;
  ASSERT_TRUE(ipl.getInteriorPoint(c));

  auto geom_pt = line->getInteriorPoint();
  ASSERT_FALSE(geom_pt->isEmpty());
  EXPECT_DOUBLE_EQ(c.x, geom_pt->getX());
  EXPECT_DOUBLE_EQ(c.y, geom_pt->getY());
}

}  // namespace
}  // namespace geos_google
