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

// Unit tests for third_party/geos/src/edgegraph/EdgeGraphBuilder.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/edgegraph/EdgeGraphBuilder.cpp

#include "third_party/geos/include/geos/edgegraph/EdgeGraphBuilder.h"

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/edgegraph/EdgeGraph.h"
#include "third_party/geos/include/geos/edgegraph/HalfEdge.h"
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

using geos::edgegraph::EdgeGraph;
using geos::edgegraph::EdgeGraphBuilder;
using geos::edgegraph::HalfEdge;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::Point;
using geos::geom::Polygon;

TEST(EdgeGraphBuilderTest, EmptyBuilderProducesEmptyGraph) {
  EdgeGraphBuilder builder;
  std::unique_ptr<EdgeGraph> graph = builder.getGraph();
  ASSERT_NE(graph, nullptr);

  std::vector<const HalfEdge*> vertex_edges;
  graph->getVertexEdges(vertex_edges);
  EXPECT_TRUE(vertex_edges.empty());
}

TEST(EdgeGraphBuilderTest, AddLineStringLinksMultiSegmentChain) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence seq({Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
                          Coordinate(1.0, 1.0), Coordinate(2.0, 1.0)});
  std::unique_ptr<LineString> line = factory->createLineString(seq);

  EdgeGraphBuilder builder;
  builder.add(line.get());
  std::unique_ptr<EdgeGraph> graph = builder.getGraph();
  ASSERT_NE(graph, nullptr);

  std::vector<const HalfEdge*> vertex_edges;
  graph->getVertexEdges(vertex_edges);
  EXPECT_EQ(vertex_edges.size(), 4u);

  HalfEdge* e01 =
      graph->findEdge(CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0));
  HalfEdge* e12 =
      graph->findEdge(CoordinateXY(1.0, 0.0), CoordinateXY(1.0, 1.0));
  HalfEdge* e23 =
      graph->findEdge(CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 1.0));
  ASSERT_NE(e01, nullptr);
  ASSERT_NE(e12, nullptr);
  ASSERT_NE(e23, nullptr);

  EXPECT_EQ(e01->degree(), 1);
  EXPECT_EQ(e12->degree(), 2);
  EXPECT_EQ(e23->degree(), 2);
  EXPECT_EQ(e23->sym()->degree(), 1);
  EXPECT_EQ(e01->next(), e12);
  EXPECT_EQ(e12->next(), e23);
  EXPECT_EQ(e23->prev(), e12);
  EXPECT_EQ(e12->prev(), e01);
}

TEST(EdgeGraphBuilderTest, AddEmptyAndRepeatedPointLineStrings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> empty_line = factory->createLineString();
  CoordinateSequence repeated_seq({Coordinate(0.0, 0.0), Coordinate(0.0, 0.0),
                                   Coordinate(2.0, 0.0), Coordinate(2.0, 0.0)});
  std::unique_ptr<LineString> repeated_line =
      factory->createLineString(repeated_seq);

  EdgeGraphBuilder builder;
  builder.add(empty_line.get());
  builder.add(repeated_line.get());
  std::unique_ptr<EdgeGraph> graph = builder.getGraph();
  ASSERT_NE(graph, nullptr);

  std::vector<const HalfEdge*> vertex_edges;
  graph->getVertexEdges(vertex_edges);
  EXPECT_EQ(vertex_edges.size(), 2u);
  EXPECT_NE(graph->findEdge(CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)),
            nullptr);
  EXPECT_NE(graph->findEdge(CoordinateXY(2.0, 0.0), CoordinateXY(0.0, 0.0)),
            nullptr);
}

TEST(EdgeGraphBuilderTest, AddLineStringPreservesZAndSetsMToNaN) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence seq(
      {Coordinate(0.0, 0.0, 5.5), Coordinate(3.0, 4.0, -2.25)});
  std::unique_ptr<LineString> line = factory->createLineString(seq);

  EdgeGraphBuilder builder;
  builder.add(line.get());
  std::unique_ptr<EdgeGraph> graph = builder.getGraph();
  ASSERT_NE(graph, nullptr);

  HalfEdge* edge =
      graph->findEdge(CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 4.0));
  ASSERT_NE(edge, nullptr);
  EXPECT_DOUBLE_EQ(edge->orig().z, 5.5);
  EXPECT_DOUBLE_EQ(edge->dest().z, -2.25);
  EXPECT_TRUE(std::isnan(edge->orig().m));
  EXPECT_TRUE(std::isnan(edge->dest().m));
}

TEST(EdgeGraphBuilderTest, AddGeometryFiltersLineStringsAndIgnoresNonLineal) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(9.0, 9.0));

  CoordinateSequence ring_seq({Coordinate(0.0, 0.0), Coordinate(2.0, 0.0),
                               Coordinate(2.0, 2.0), Coordinate(0.0, 2.0),
                               Coordinate(0.0, 0.0)});
  std::unique_ptr<LinearRing> ring = factory->createLinearRing(ring_seq);

  CoordinateSequence poly_ring_seq(
      {Coordinate(10.0, 10.0), Coordinate(12.0, 10.0), Coordinate(12.0, 12.0),
       Coordinate(10.0, 12.0), Coordinate(10.0, 10.0)});
  std::unique_ptr<Polygon> poly =
      factory->createPolygon(factory->createLinearRing(poly_ring_seq));

  std::vector<std::unique_ptr<LineString>> mls_parts;
  mls_parts.push_back(factory->createLineString(
      CoordinateSequence({Coordinate(3.0, 0.0), Coordinate(4.0, 0.0)})));
  mls_parts.push_back(factory->createLineString(
      CoordinateSequence({Coordinate(4.0, 0.0), Coordinate(4.0, 1.0)})));
  std::unique_ptr<MultiLineString> mls =
      factory->createMultiLineString(std::move(mls_parts));

  EdgeGraphBuilder builder;
  // Pass via const Geometry* to exercise EdgeGraphBuilder::add(const
  // Geometry*).
  builder.add(static_cast<const Geometry*>(pt.get()));
  builder.add(static_cast<const Geometry*>(poly.get()));
  builder.add(static_cast<const Geometry*>(ring.get()));
  builder.add(static_cast<const Geometry*>(mls.get()));

  std::unique_ptr<EdgeGraph> graph = builder.getGraph();
  ASSERT_NE(graph, nullptr);

  // 4 vertices from LinearRing + 3 vertices from MultiLineString = 7 vertices.
  std::vector<const HalfEdge*> vertex_edges;
  graph->getVertexEdges(vertex_edges);
  EXPECT_EQ(vertex_edges.size(), 7u);

  // Point and Polygon components were ignored by EdgeGraphLinestringFilter.
  EXPECT_EQ(graph->findEdge(CoordinateXY(10.0, 10.0), CoordinateXY(12.0, 10.0)),
            nullptr);
  EXPECT_NE(graph->findEdge(CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)),
            nullptr);
  EXPECT_NE(graph->findEdge(CoordinateXY(2.0, 2.0), CoordinateXY(0.0, 2.0)),
            nullptr);
  EXPECT_NE(graph->findEdge(CoordinateXY(3.0, 0.0), CoordinateXY(4.0, 0.0)),
            nullptr);
  EXPECT_NE(graph->findEdge(CoordinateXY(4.0, 0.0), CoordinateXY(4.0, 1.0)),
            nullptr);
}

TEST(EdgeGraphBuilderTest, AddGeometryCollectionHandlesEmptyAndMixedElements) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<GeometryCollection> empty_gc =
      factory->createGeometryCollection();

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(factory->createPoint(CoordinateXY(50.0, 50.0)));
  geoms.push_back(factory->createLineString(
      CoordinateSequence({Coordinate(0.0, 0.0), Coordinate(1.0, 0.0)})));
  geoms.push_back(factory->createLinearRing(
      CoordinateSequence({Coordinate(1.0, 0.0), Coordinate(2.0, 0.0),
                          Coordinate(2.0, 1.0), Coordinate(1.0, 0.0)})));

  std::vector<std::unique_ptr<LineString>> nested_lines;
  nested_lines.push_back(factory->createLineString(
      CoordinateSequence({Coordinate(2.0, 1.0), Coordinate(3.0, 1.0)})));
  geoms.push_back(factory->createMultiLineString(std::move(nested_lines)));

  std::unique_ptr<GeometryCollection> mixed_gc =
      factory->createGeometryCollection(std::move(geoms));

  EdgeGraphBuilder builder;
  builder.add(empty_gc.get());
  builder.add(mixed_gc.get());
  std::unique_ptr<EdgeGraph> graph = builder.getGraph();
  ASSERT_NE(graph, nullptr);

  HalfEdge* shared =
      graph->findEdge(CoordinateXY(1.0, 0.0), CoordinateXY(0.0, 0.0));
  ASSERT_NE(shared, nullptr);
  EXPECT_EQ(shared->degree(), 3);
  EXPECT_TRUE(shared->isEdgesSorted());

  HalfEdge* tail =
      graph->findEdge(CoordinateXY(2.0, 1.0), CoordinateXY(3.0, 1.0));
  ASSERT_NE(tail, nullptr);
  EXPECT_EQ(tail->degree(), 3);
  EXPECT_EQ(tail->sym()->degree(), 1);
}

TEST(EdgeGraphBuilderTest,
     StaticBuildFromGeometryCollectionAndEmptyCollection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<GeometryCollection> empty_gc =
      factory->createGeometryCollection();
  std::unique_ptr<EdgeGraph> empty_graph =
      EdgeGraphBuilder::build(empty_gc.get());
  ASSERT_NE(empty_graph, nullptr);
  std::vector<const HalfEdge*> empty_vertices;
  empty_graph->getVertexEdges(empty_vertices);
  EXPECT_TRUE(empty_vertices.empty());

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(factory->createLineString(CoordinateSequence(
      {Coordinate(0.0, 0.0), Coordinate(1.0, 0.0), Coordinate(2.0, 0.0)})));
  geoms.push_back(factory->createLineString(
      CoordinateSequence({Coordinate(1.0, 0.0), Coordinate(1.0, 2.0)})));
  std::unique_ptr<GeometryCollection> t_junction =
      factory->createGeometryCollection(std::move(geoms));

  std::unique_ptr<EdgeGraph> graph = EdgeGraphBuilder::build(t_junction.get());
  ASSERT_NE(graph, nullptr);

  HalfEdge* branch =
      graph->findEdge(CoordinateXY(1.0, 0.0), CoordinateXY(1.0, 2.0));
  ASSERT_NE(branch, nullptr);
  EXPECT_EQ(branch->degree(), 3);
  EXPECT_TRUE(branch->isEdgesSorted());
  EXPECT_NE(graph->findEdge(CoordinateXY(1.0, 0.0), CoordinateXY(0.0, 0.0)),
            nullptr);
  EXPECT_NE(graph->findEdge(CoordinateXY(1.0, 0.0), CoordinateXY(2.0, 0.0)),
            nullptr);
}

TEST(EdgeGraphBuilderTest, MultipleAddCallsDeduplicateSharedSegments) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> line_a =
      factory->createLineString(CoordinateSequence(
          {Coordinate(0.0, 0.0), Coordinate(1.0, 0.0), Coordinate(1.0, 1.0)}));
  std::unique_ptr<LineString> line_b =
      factory->createLineString(CoordinateSequence(
          {Coordinate(0.0, 0.0), Coordinate(1.0, 0.0), Coordinate(2.0, 0.0)}));

  EdgeGraphBuilder builder;
  builder.add(line_a.get());
  builder.add(line_b.get());
  std::unique_ptr<EdgeGraph> graph = builder.getGraph();
  ASSERT_NE(graph, nullptr);

  HalfEdge* hub =
      graph->findEdge(CoordinateXY(1.0, 0.0), CoordinateXY(0.0, 0.0));
  ASSERT_NE(hub, nullptr);
  EXPECT_EQ(hub->degree(), 3);
  EXPECT_TRUE(hub->isEdgesSorted());

  std::vector<const HalfEdge*> vertex_edges;
  graph->getVertexEdges(vertex_edges);
  EXPECT_EQ(vertex_edges.size(), 4u);
}

}  // namespace
}  // namespace geos_google
