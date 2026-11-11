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

#include "third_party/geos/include/geos/operation/polygonize/PolygonizeEdge.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/operation/polygonize/PolygonizeDirectedEdge.h"
#include "third_party/geos/include/geos/operation/polygonize/PolygonizeGraph.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdge.h"
#include "third_party/geos/include/geos/planargraph/Edge.h"
#include "third_party/geos/include/geos/planargraph/GraphComponent.h"
#include "third_party/geos/include/geos/planargraph/Node.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::operation::polygonize::PolygonizeDirectedEdge;
using geos::operation::polygonize::PolygonizeEdge;
using geos::operation::polygonize::PolygonizeGraph;
using geos::planargraph::DirectedEdge;
using geos::planargraph::Edge;
using geos::planargraph::GraphComponent;
using geos::planargraph::Node;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

TEST(PolygonizeEdgeTest, StoresAndReturnsLineStringPointer) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence coords({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(3.0, 0.0),
      CoordinateXY(3.0, 4.0),
  });
  std::unique_ptr<LineString> line = factory->createLineString(coords);
  std::unique_ptr<LineString> empty_line = factory->createLineString();

  PolygonizeEdge edge(line.get());
  EXPECT_EQ(edge.getLine(), line.get());
  ASSERT_NE(edge.getLine(), nullptr);
  EXPECT_FALSE(edge.getLine()->isEmpty());
  EXPECT_EQ(edge.getLine()->getNumPoints(), 3u);
  EXPECT_DOUBLE_EQ(edge.getLine()->getLength(), 7.0);

  PolygonizeEdge empty_edge(empty_line.get());
  EXPECT_EQ(empty_edge.getLine(), empty_line.get());
  ASSERT_NE(empty_edge.getLine(), nullptr);
  EXPECT_TRUE(empty_edge.getLine()->isEmpty());
  EXPECT_EQ(empty_edge.getLine()->getNumPoints(), 0u);
}

TEST(PolygonizeEdgeTest, AcceptsNullptrAndDoesNotOwnLineString) {
  PolygonizeEdge null_edge(nullptr);
  EXPECT_EQ(null_edge.getLine(), nullptr);

  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence coords({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(3.0, 4.0),
  });
  std::unique_ptr<LineString> line = factory->createLineString(coords);

  {
    PolygonizeEdge scoped_edge(line.get());
    EXPECT_EQ(scoped_edge.getLine(), line.get());
  }

  std::unique_ptr<Edge> base_edge =
      std::make_unique<PolygonizeEdge>(line.get());
  auto* as_poly_edge = dynamic_cast<PolygonizeEdge*>(base_edge.get());
  ASSERT_NE(as_poly_edge, nullptr);
  EXPECT_EQ(as_poly_edge->getLine(), line.get());
  base_edge.reset();

  std::unique_ptr<GraphComponent> base_component =
      std::make_unique<PolygonizeEdge>(line.get());
  auto* from_component = dynamic_cast<PolygonizeEdge*>(base_component.get());
  ASSERT_NE(from_component, nullptr);
  EXPECT_EQ(from_component->getLine(), line.get());
  base_component.reset();

  // Destroying PolygonizeEdge instances leaves the externally owned LineString
  // intact.
  EXPECT_EQ(line->getNumPoints(), 2u);
  EXPECT_DOUBLE_EQ(line->getLength(), 5.0);
  EXPECT_EQ(line->getCoordinatesRO()->getAt<CoordinateXY>(1),
            CoordinateXY(3.0, 4.0));
}

TEST(PolygonizeEdgeTest, PreservesLineStringsAcrossCoordinateDimensions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  CoordinateSequence xyz_coords({
      Coordinate(1.0, 2.0, 3.0),
      Coordinate(4.0, 5.0, 6.0),
  });
  std::unique_ptr<LineString> xyz_line = factory->createLineString(xyz_coords);
  PolygonizeEdge xyz_edge(xyz_line.get());
  ASSERT_EQ(xyz_edge.getLine(), xyz_line.get());
  EXPECT_TRUE(xyz_edge.getLine()->hasZ());
  EXPECT_FALSE(xyz_edge.getLine()->hasM());
  const CoordinateSequence* xyz_ro = xyz_edge.getLine()->getCoordinatesRO();
  EXPECT_DOUBLE_EQ(xyz_ro->getOrdinate(0, CoordinateSequence::Z), 3.0);
  EXPECT_TRUE(std::isnan(xyz_ro->getOrdinate(0, CoordinateSequence::M)));

  CoordinateSequence xym_coords({
      CoordinateXYM(1.0, 2.0, 10.0),
      CoordinateXYM(4.0, 5.0, 20.0),
  });
  std::unique_ptr<LineString> xym_line = factory->createLineString(xym_coords);
  PolygonizeEdge xym_edge(xym_line.get());
  ASSERT_EQ(xym_edge.getLine(), xym_line.get());
  EXPECT_FALSE(xym_edge.getLine()->hasZ());
  EXPECT_TRUE(xym_edge.getLine()->hasM());
  const CoordinateSequence* xym_ro = xym_edge.getLine()->getCoordinatesRO();
  CoordinateXYM out_xym;
  xym_ro->getAt(1, out_xym);
  EXPECT_DOUBLE_EQ(out_xym.x, 4.0);
  EXPECT_DOUBLE_EQ(out_xym.y, 5.0);
  EXPECT_DOUBLE_EQ(out_xym.m, 20.0);
  EXPECT_TRUE(std::isnan(xym_ro->getOrdinate(1, CoordinateSequence::Z)));
  EXPECT_DOUBLE_EQ(xym_ro->getOrdinate(1, CoordinateSequence::M), 20.0);

  CoordinateSequence xyzm_coords({
      CoordinateXYZM(1.0, 2.0, 3.0, 4.0),
      CoordinateXYZM(5.0, 6.0, 7.0, 8.0),
  });
  std::unique_ptr<LineString> xyzm_line =
      factory->createLineString(xyzm_coords);
  PolygonizeEdge xyzm_edge(xyzm_line.get());
  ASSERT_EQ(xyzm_edge.getLine(), xyzm_line.get());
  EXPECT_TRUE(xyzm_edge.getLine()->hasZ());
  EXPECT_TRUE(xyzm_edge.getLine()->hasM());
  const CoordinateSequence* xyzm_ro = xyzm_edge.getLine()->getCoordinatesRO();
  CoordinateXYZM out_xyzm;
  xyzm_ro->getAt(1, out_xyzm);
  EXPECT_DOUBLE_EQ(out_xyzm.x, 5.0);
  EXPECT_DOUBLE_EQ(out_xyzm.y, 6.0);
  EXPECT_DOUBLE_EQ(out_xyzm.z, 7.0);
  EXPECT_DOUBLE_EQ(out_xyzm.m, 8.0);
}

TEST(PolygonizeEdgeTest, InheritsPlanargraphEdgeAndGraphComponentBehavior) {
  static_assert(std::is_base_of_v<Edge, PolygonizeEdge>);
  static_assert(std::is_base_of_v<GraphComponent, PolygonizeEdge>);

  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence coords({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
  });
  std::unique_ptr<LineString> line = factory->createLineString(coords);

  PolygonizeEdge edge(line.get());
  EXPECT_FALSE(edge.isMarked());
  EXPECT_FALSE(edge.isVisited());

  edge.setMarked(true);
  edge.setVisited(true);
  EXPECT_TRUE(edge.isMarked());
  EXPECT_TRUE(edge.isVisited());

  std::ostringstream oss;
  oss << edge;
  EXPECT_THAT(oss.str(), HasSubstr("Edge"));
  EXPECT_THAT(oss.str(), HasSubstr("Marked"));
  EXPECT_THAT(oss.str(), HasSubstr("Visited"));

  edge.setMarked(false);
  edge.setVisited(false);
  EXPECT_FALSE(edge.isMarked());
  EXPECT_FALSE(edge.isVisited());

  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(10.0, 0.0));
  Node n_unrelated(Coordinate(5.0, 5.0));

  PolygonizeDirectedEdge de0(&n0, &n1, Coordinate(10.0, 0.0), true);
  PolygonizeDirectedEdge de1(&n1, &n0, Coordinate(0.0, 0.0), false);
  edge.setDirectedEdges(&de0, &de1);

  EXPECT_EQ(edge.getDirEdge(0), &de0);
  EXPECT_EQ(edge.getDirEdge(1), &de1);
  EXPECT_EQ(edge.getDirEdge(&n0), &de0);
  EXPECT_EQ(edge.getDirEdge(&n1), &de1);
  EXPECT_EQ(edge.getDirEdge(&n_unrelated), nullptr);

  EXPECT_EQ(edge.getOppositeNode(&n0), &n1);
  EXPECT_EQ(edge.getOppositeNode(&n1), &n0);
  EXPECT_EQ(edge.getOppositeNode(&n_unrelated), nullptr);

  EXPECT_EQ(de0.getEdge(), &edge);
  EXPECT_EQ(de1.getEdge(), &edge);
  EXPECT_EQ(de0.getSym(), &de1);
  EXPECT_EQ(de1.getSym(), &de0);
  EXPECT_EQ(n0.getDegree(), 1u);
  EXPECT_EQ(n1.getDegree(), 1u);

  auto* recovered = dynamic_cast<PolygonizeEdge*>(de0.getEdge());
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->getLine(), line.get());

  std::vector<DirectedEdge*> dir_edges{&de0, &de1};
  std::vector<Edge*> parent_edges;
  DirectedEdge::toEdges(dir_edges, parent_edges);
  EXPECT_THAT(parent_edges, ElementsAre(&edge, &edge));
}

TEST(PolygonizeEdgeTest, IntegratesWithPolygonizeGraph) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto make_line = [&factory](double x0, double y0, double x1, double y1) {
    return factory->createLineString(CoordinateSequence({
        CoordinateXY(x0, y0),
        CoordinateXY(x1, y1),
    }));
  };

  std::unique_ptr<LineString> e0 = make_line(0.0, 0.0, 4.0, 0.0);
  std::unique_ptr<LineString> e1 = make_line(4.0, 0.0, 2.0, 4.0);
  std::unique_ptr<LineString> e2 = make_line(2.0, 4.0, 0.0, 0.0);
  std::unique_ptr<LineString> bridge = make_line(4.0, 0.0, 6.0, 0.0);
  std::unique_ptr<LineString> e3 = make_line(6.0, 0.0, 10.0, 0.0);
  std::unique_ptr<LineString> e4 = make_line(10.0, 0.0, 8.0, 4.0);
  std::unique_ptr<LineString> e5 = make_line(8.0, 4.0, 6.0, 0.0);
  std::unique_ptr<LineString> dangle = make_line(10.0, 0.0, 12.0, 0.0);
  std::unique_ptr<LineString> empty_line = factory->createLineString();
  std::unique_ptr<LineString> degenerate = make_line(2.0, 2.0, 2.0, 2.0);

  PolygonizeGraph graph(factory.get());
  graph.addEdge(e0.get());
  graph.addEdge(e1.get());
  graph.addEdge(e2.get());
  graph.addEdge(bridge.get());
  graph.addEdge(e3.get());
  graph.addEdge(e4.get());
  graph.addEdge(e5.get());
  graph.addEdge(dangle.get());
  graph.addEdge(empty_line.get());
  graph.addEdge(degenerate.get());

  const std::vector<Edge*>* edges = graph.getEdges();
  ASSERT_NE(edges, nullptr);
  ASSERT_EQ(edges->size(), 8u);

  std::vector<const LineString*> stored_lines;
  stored_lines.reserve(edges->size());
  for (Edge* edge : *edges) {
    auto* poly_edge = dynamic_cast<PolygonizeEdge*>(edge);
    ASSERT_NE(poly_edge, nullptr);
    stored_lines.push_back(poly_edge->getLine());
  }
  EXPECT_THAT(stored_lines,
              ElementsAre(e0.get(), e1.get(), e2.get(), bridge.get(), e3.get(),
                          e4.get(), e5.get(), dangle.get()));

  std::vector<const LineString*> dangle_lines;
  graph.deleteDangles(dangle_lines);
  EXPECT_THAT(dangle_lines, ElementsAre(dangle.get()));

  std::vector<const LineString*> cut_lines;
  graph.deleteCutEdges(cut_lines);
  EXPECT_THAT(cut_lines, ElementsAre(bridge.get()));
}

}  // namespace
}  // namespace geos_google
