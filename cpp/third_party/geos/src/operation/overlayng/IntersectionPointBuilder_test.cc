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
// third_party/geos/src/operation/overlayng/IntersectionPointBuilder.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/IntersectionPointBuilder.cpp

#include "third_party/geos/include/geos/operation/overlayng/IntersectionPointBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/Edge.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayEdge.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayGraph.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayLabel.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Point;
using geos::io::WKTReader;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::operation::overlayng::IntersectionPointBuilder;
using geos::operation::overlayng::OverlayEdge;
using geos::operation::overlayng::OverlayGraph;
using geos::operation::overlayng::OverlayLabel;
using geos::operation::overlayng::OverlayNG;

OverlayEdge* AddLineEdge(OverlayGraph& graph, const CoordinateXY& p0,
                         const CoordinateXY& p1, uint8_t geom_index) {
  auto cs = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{p0, p1});
  EdgeSourceInfo info(geom_index);
  Edge edge(std::move(cs), &info);
  return graph.addEdge(&edge);
}

OverlayEdge* AddBoundaryEdge(OverlayGraph& graph, const CoordinateXY& p0,
                             const CoordinateXY& p1, uint8_t geom_index,
                             int depth_delta, bool is_hole) {
  auto cs = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{p0, p1});
  EdgeSourceInfo info(geom_index, depth_delta, is_hole);
  Edge edge(std::move(cs), &info);
  return graph.addEdge(&edge);
}

std::vector<CoordinateXY> SortedPoints2D(
    const std::vector<std::unique_ptr<Point>>& pts) {
  std::vector<CoordinateXY> coords;
  coords.reserve(pts.size());
  for (const auto& pt : pts) {
    coords.emplace_back(pt->getX(), pt->getY());
  }
  std::sort(coords.begin(), coords.end());
  return coords;
}

TEST(IntersectionPointBuilderTest, EmptyGraphReturnsNoPoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;
  IntersectionPointBuilder builder(&graph, factory.get());

  std::vector<std::unique_ptr<Point>> pts = builder.getPoints();
  EXPECT_TRUE(pts.empty());
}

TEST(IntersectionPointBuilderTest,
     CrossingLinesYieldOnlySharedIntersectionNode) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;

  // Input 0 crosses (1, 1); input 1 crosses (1, 1). No edges are marked in
  // result, so only the shared crossing node at (1, 1) belongs to both inputs.
  AddLineEdge(graph, {0.0, 0.0}, {1.0, 1.0}, 0);
  AddLineEdge(graph, {1.0, 1.0}, {2.0, 2.0}, 0);
  AddLineEdge(graph, {0.0, 2.0}, {1.0, 1.0}, 1);
  AddLineEdge(graph, {1.0, 1.0}, {2.0, 0.0}, 1);

  IntersectionPointBuilder builder(&graph, factory.get());
  std::vector<CoordinateXY> coords = SortedPoints2D(builder.getPoints());

  ASSERT_EQ(coords.size(), 1u);
  EXPECT_EQ(coords[0], CoordinateXY(1.0, 1.0));
}

TEST(IntersectionPointBuilderTest, MergedSingleEdgeYieldsBothDegreeOneNodes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;

  auto cs = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{1.0, 2.0}, {4.0, 5.0}});
  EdgeSourceInfo info_a(0);
  EdgeSourceInfo info_b(1);
  Edge edge_a(std::move(cs), &info_a);
  auto cs_b = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{1.0, 2.0}, {4.0, 5.0}});
  Edge edge_b(std::move(cs_b), &info_b);
  edge_a.merge(&edge_b);
  graph.addEdge(&edge_a);

  IntersectionPointBuilder builder(&graph, factory.get());
  std::vector<CoordinateXY> coords = SortedPoints2D(builder.getPoints());

  ASSERT_EQ(coords.size(), 2u);
  EXPECT_EQ(coords[0], CoordinateXY(1.0, 2.0));
  EXPECT_EQ(coords[1], CoordinateXY(4.0, 5.0));
}

TEST(IntersectionPointBuilderTest,
     IncidentResultLineOrResultAreaSuppressesNodePoint) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Case A: an incident edge at (1, 1) is marked in result line.
  {
    OverlayGraph graph;
    AddLineEdge(graph, {0.0, 1.0}, {1.0, 1.0}, 0);
    AddLineEdge(graph, {1.0, 0.0}, {1.0, 1.0}, 1);
    OverlayEdge* shared = AddLineEdge(graph, {1.0, 1.0}, {2.0, 1.0}, 0);
    shared->getLabel()->initLine(1);
    shared->markInResultLine();

    IntersectionPointBuilder builder(&graph, factory.get());
    std::vector<CoordinateXY> coords = SortedPoints2D(builder.getPoints());
    // (1, 1) and (2, 1) both touch in-result edges, so neither becomes a
    // result point.
    EXPECT_TRUE(coords.empty());
  }

  // Case B: an incident outgoing edge at (1, 1) is marked in result area.
  {
    OverlayGraph graph;
    OverlayEdge* area_edge =
        AddBoundaryEdge(graph, {1.0, 1.0}, {2.0, 1.0}, 0, 1, false);
    area_edge->markInResultArea();
    AddBoundaryEdge(graph, {0.0, 1.0}, {1.0, 1.0}, 1, 1, false);

    IntersectionPointBuilder builder(&graph, factory.get());
    std::vector<CoordinateXY> coords = SortedPoints2D(builder.getPoints());
    EXPECT_TRUE(coords.empty());
  }
}

TEST(IntersectionPointBuilderTest, StrictModeFiltersBoundaryCollapseNodes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Build two boundary-only edges from A and B meeting at (1, 1) with no
  // shared boundary segment. Because each edge is a boundary of one geometry
  // and not part of the other, OverlayLabel::isBoundaryCollapse() is true.
  auto build_touch_graph = [](OverlayGraph& graph) {
    AddBoundaryEdge(graph, {0.0, 0.0}, {1.0, 1.0}, 0, 1, false);
    AddBoundaryEdge(graph, {1.0, 1.0}, {0.0, 2.0}, 0, 1, false);
    AddBoundaryEdge(graph, {2.0, 0.0}, {1.0, 1.0}, 1, 1, false);
    AddBoundaryEdge(graph, {1.0, 1.0}, {2.0, 2.0}, 1, 1, false);
  };

  {
    OverlayGraph graph;
    build_touch_graph(graph);
    IntersectionPointBuilder builder(&graph, factory.get());
    builder.setStrictMode(false);
    std::vector<CoordinateXY> coords = SortedPoints2D(builder.getPoints());
    ASSERT_EQ(coords.size(), 1u);
    EXPECT_EQ(coords[0], CoordinateXY(1.0, 1.0));
  }

  {
    OverlayGraph graph;
    build_touch_graph(graph);
    IntersectionPointBuilder builder(&graph, factory.get());
    builder.setStrictMode(true);
    std::vector<CoordinateXY> coords = SortedPoints2D(builder.getPoints());
    EXPECT_TRUE(coords.empty());
  }
}

TEST(IntersectionPointBuilderTest,
     StrictModeAllowsSharedBoundaryAndCollapseWithLine) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;

  // Node at (1, 1): edge1 is collapse on A + line on B; edge2 is line on A.
  // Because edge1 is a line on B, isBoundaryCollapse() is false, so strict
  // mode still allows line/boundary checks at (1, 1). Note collapse on A alone
  // does not count as boundary or line for A, so edge2 supplies A.
  OverlayEdge* e1 = AddBoundaryEdge(graph, {1.0, 1.0}, {2.0, 1.0}, 0, 0, false);
  e1->getLabel()->initLine(1);
  AddLineEdge(graph, {0.0, 1.0}, {1.0, 1.0}, 0);

  // Nodes at (5, 5) and (6, 5): shared boundary on both A and B
  // (isBoundaryBoth() == true, so isBoundaryCollapse() == false), not marked
  // in result. Both endpoints qualify as result points.
  OverlayEdge* e_both =
      AddBoundaryEdge(graph, {5.0, 5.0}, {6.0, 5.0}, 0, 1, false);
  e_both->getLabel()->initBoundary(1, Location::INTERIOR, Location::EXTERIOR,
                                   false);

  // Node at (10, 10): collapses only on A and B -> never a result point even
  // in non-strict mode, because neither isBoundary(i) nor isLine(i) holds.
  AddBoundaryEdge(graph, {9.0, 10.0}, {10.0, 10.0}, 0, 0, false);
  AddBoundaryEdge(graph, {10.0, 10.0}, {11.0, 10.0}, 1, 0, false);

  IntersectionPointBuilder builder(&graph, factory.get());
  builder.setStrictMode(true);
  std::vector<CoordinateXY> coords = SortedPoints2D(builder.getPoints());

  ASSERT_EQ(coords.size(), 3u);
  EXPECT_EQ(coords[0], CoordinateXY(1.0, 1.0));
  EXPECT_EQ(coords[1], CoordinateXY(5.0, 5.0));
  EXPECT_EQ(coords[2], CoordinateXY(6.0, 5.0));
}

TEST(IntersectionPointBuilderTest, CollapseEdgesAloneYieldNoPointsInNonStrict) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;

  AddBoundaryEdge(graph, {0.0, 0.0}, {1.0, 1.0}, 0, 0, false);
  AddBoundaryEdge(graph, {1.0, 1.0}, {2.0, 0.0}, 1, 0, false);

  IntersectionPointBuilder builder(&graph, factory.get());
  builder.setStrictMode(false);
  EXPECT_TRUE(builder.getPoints().empty());
}

TEST(IntersectionPointBuilderTest, PreservesCoordinateZAndM) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // 2D XY node -> Z and M are NaN.
  {
    OverlayGraph graph;
    AddLineEdge(graph, {0.0, 1.0}, {1.0, 1.0}, 0);
    AddLineEdge(graph, {1.0, 0.0}, {1.0, 1.0}, 1);

    IntersectionPointBuilder builder(&graph, factory.get());
    std::vector<std::unique_ptr<Point>> pts = builder.getPoints();
    ASSERT_EQ(pts.size(), 1u);
    EXPECT_DOUBLE_EQ(pts[0]->getX(), 1.0);
    EXPECT_DOUBLE_EQ(pts[0]->getY(), 1.0);
    EXPECT_TRUE(std::isnan(pts[0]->getZ()));
    EXPECT_TRUE(std::isnan(pts[0]->getM()));
  }

  // 3D XYZ node -> Z is preserved, M is NaN.
  {
    OverlayGraph graph;
    auto cs_a = std::make_unique<CoordinateSequence>(
        std::initializer_list<Coordinate>{{0.0, 1.0, 7.5}, {1.0, 1.0, 7.5}});
    auto cs_b = std::make_unique<CoordinateSequence>(
        std::initializer_list<Coordinate>{{1.0, 0.0, 7.5}, {1.0, 1.0, 7.5}});
    EdgeSourceInfo info_a(0);
    EdgeSourceInfo info_b(1);
    Edge edge_a(std::move(cs_a), &info_a);
    Edge edge_b(std::move(cs_b), &info_b);
    graph.addEdge(&edge_a);
    graph.addEdge(&edge_b);

    IntersectionPointBuilder builder(&graph, factory.get());
    std::vector<std::unique_ptr<Point>> pts = builder.getPoints();
    ASSERT_EQ(pts.size(), 1u);
    EXPECT_TRUE(pts[0]->hasZ());
    EXPECT_FALSE(pts[0]->hasM());
    EXPECT_DOUBLE_EQ(pts[0]->getZ(), 7.5);
    EXPECT_TRUE(std::isnan(pts[0]->getM()));
  }

  // 3D XYM node -> Z is NaN, M is preserved.
  {
    OverlayGraph graph;
    auto cs_a = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXYM>{{0.0, 1.0, 12.25},
                                             {1.0, 1.0, 12.25}});
    auto cs_b = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXYM>{{1.0, 0.0, 12.25},
                                             {1.0, 1.0, 12.25}});
    EdgeSourceInfo info_a(0);
    EdgeSourceInfo info_b(1);
    Edge edge_a(std::move(cs_a), &info_a);
    Edge edge_b(std::move(cs_b), &info_b);
    graph.addEdge(&edge_a);
    graph.addEdge(&edge_b);

    IntersectionPointBuilder builder(&graph, factory.get());
    std::vector<std::unique_ptr<Point>> pts = builder.getPoints();
    ASSERT_EQ(pts.size(), 1u);
    EXPECT_FALSE(pts[0]->hasZ());
    EXPECT_TRUE(pts[0]->hasM());
    EXPECT_TRUE(std::isnan(pts[0]->getZ()));
    EXPECT_DOUBLE_EQ(pts[0]->getM(), 12.25);
  }

  // 4D XYZM node -> both Z and M are preserved.
  {
    OverlayGraph graph;
    auto cs_a = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXYZM>{{0.0, 1.0, 3.25, 9.5},
                                              {1.0, 1.0, 3.25, 9.5}});
    auto cs_b = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXYZM>{{1.0, 0.0, 3.25, 9.5},
                                              {1.0, 1.0, 3.25, 9.5}});
    EdgeSourceInfo info_a(0);
    EdgeSourceInfo info_b(1);
    Edge edge_a(std::move(cs_a), &info_a);
    Edge edge_b(std::move(cs_b), &info_b);
    graph.addEdge(&edge_a);
    graph.addEdge(&edge_b);

    IntersectionPointBuilder builder(&graph, factory.get());
    std::vector<std::unique_ptr<Point>> pts = builder.getPoints();
    ASSERT_EQ(pts.size(), 1u);
    EXPECT_TRUE(pts[0]->hasZ());
    EXPECT_TRUE(pts[0]->hasM());
    EXPECT_DOUBLE_EQ(pts[0]->getZ(), 3.25);
    EXPECT_DOUBLE_EQ(pts[0]->getM(), 9.5);
  }
}

TEST(IntersectionPointBuilderTest,
     OverlayNGIntegrationPolygonsAndLinesTouchingAtPoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Two polygons touching at a single corner (1, 1).
  Geometry::Ptr poly_a = reader.read("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  Geometry::Ptr poly_b = reader.read("POLYGON ((1 1, 2 1, 2 2, 1 2, 1 1))");

  {
    OverlayNG ov(poly_a.get(), poly_b.get(), OverlayNG::INTERSECTION);
    ov.setStrictMode(false);
    Geometry::Ptr res = ov.getResult();
    ASSERT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POINT);
    const CoordinateXY* c = res->getCoordinate();
    ASSERT_NE(c, nullptr);
    EXPECT_DOUBLE_EQ(c->x, 1.0);
    EXPECT_DOUBLE_EQ(c->y, 1.0);
  }

  {
    OverlayNG ov(poly_a.get(), poly_b.get(), OverlayNG::INTERSECTION);
    ov.setStrictMode(true);
    Geometry::Ptr res = ov.getResult();
    EXPECT_TRUE(res->isEmpty());
  }

  // Two lines crossing at (1, 1) yield POINT (1 1) even in strict mode.
  Geometry::Ptr line_a = reader.read("LINESTRING (0 1, 2 1)");
  Geometry::Ptr line_b = reader.read("LINESTRING (1 0, 1 2)");
  {
    OverlayNG ov(line_a.get(), line_b.get(), OverlayNG::INTERSECTION);
    ov.setStrictMode(true);
    Geometry::Ptr res = ov.getResult();
    ASSERT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POINT);
    const CoordinateXY* c = res->getCoordinate();
    ASSERT_NE(c, nullptr);
    EXPECT_DOUBLE_EQ(c->x, 1.0);
    EXPECT_DOUBLE_EQ(c->y, 1.0);
  }

  // Line touching polygon at a single vertex (0, 0): non-strict yields
  // POINT (0 0); strict filters the boundary-only polygon edges and returns
  // empty.
  Geometry::Ptr touch_line = reader.read("LINESTRING (-1 -1, 0 0)");
  {
    OverlayNG ov(touch_line.get(), poly_a.get(), OverlayNG::INTERSECTION);
    ov.setStrictMode(false);
    Geometry::Ptr res = ov.getResult();
    ASSERT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POINT);
    const CoordinateXY* c = res->getCoordinate();
    ASSERT_NE(c, nullptr);
    EXPECT_DOUBLE_EQ(c->x, 0.0);
    EXPECT_DOUBLE_EQ(c->y, 0.0);
  }
  {
    OverlayNG ov(touch_line.get(), poly_a.get(), OverlayNG::INTERSECTION);
    ov.setStrictMode(true);
    Geometry::Ptr res = ov.getResult();
    EXPECT_TRUE(res->isEmpty());
  }

  // Polygon overlapping with one polygon and touching another at (0, 2):
  // non-strict yields GeometryCollection (Polygon + Point), strict yields
  // Polygon only.
  Geometry::Ptr base_poly = reader.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr multi_poly = reader.read(
      "MULTIPOLYGON (((1 1, 3 1, 3 3, 1 3, 1 1)), "
      "((0 2, -1 2, -1 3, 0 3, 0 2)))");
  {
    OverlayNG ov(base_poly.get(), multi_poly.get(), OverlayNG::INTERSECTION);
    ov.setStrictMode(false);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(res->getNumGeometries(), 2u);
  }
  {
    OverlayNG ov(base_poly.get(), multi_poly.get(), OverlayNG::INTERSECTION);
    ov.setStrictMode(true);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(res->getArea(), 1.0);
  }
}

}  // namespace
}  // namespace geos_google
