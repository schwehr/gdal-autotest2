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

// Unit tests for third_party/geos/src/operation/buffer/PolygonBuilder.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/buffer/PolygonBuilder.cpp

#include "third_party/geos/include/geos/operation/buffer/PolygonBuilder.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdge.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/PlanarGraph.h"
#include "third_party/geos/include/geos/operation/buffer/BufferNodeFactory.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::Envelope;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryFactory;
using ::geos::geom::LinearRing;
using ::geos::geom::Location;
using ::geos::geom::Polygon;
using ::geos::geom::PrecisionModel;
using ::geos::geomgraph::DirectedEdge;
using ::geos::geomgraph::Edge;
using ::geos::geomgraph::EdgeEnd;
using ::geos::geomgraph::Label;
using ::geos::geomgraph::Node;
using ::geos::geomgraph::PlanarGraph;
using ::geos::operation::buffer::BufferNodeFactory;
using ::geos::operation::buffer::PolygonBuilder;
using ::geos::util::IllegalArgumentException;
using ::geos::util::TopologyException;
using ::testing::IsEmpty;

Label AreaLabel(std::uint32_t geom_index = 0,
                Location left = Location::EXTERIOR,
                Location right = Location::INTERIOR) {
  return Label(geom_index, Location::BOUNDARY, left, right);
}

DirectedEdge* AddGraphEdge(PlanarGraph& graph,
                           std::initializer_list<Coordinate> coords,
                           const Label& label, bool mark_fwd_in_result = true,
                           bool mark_bwd_in_result = false) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  auto edge = std::make_unique<Edge>(seq.release(), label);
  Edge* raw_edge = edge.get();
  graph.addEdges({edge.release()});

  DirectedEdge* fwd_de = nullptr;
  for (EdgeEnd* ee : *graph.getEdgeEnds()) {
    auto* de = dynamic_cast<DirectedEdge*>(ee);
    if (de == nullptr || de->getEdge() != raw_edge) {
      continue;
    }
    if (de->isForward()) {
      de->setInResult(mark_fwd_in_result);
      fwd_de = de;
    } else {
      de->setInResult(mark_bwd_in_result);
    }
  }
  return fwd_de;
}

// Adds a rectangular ring whose interior (for PolygonBuilder) lies on the RHS.
// CW rings represent shells; CCW rings represent holes.
void AddBoxRing(PlanarGraph& graph, double min_x, double min_y, double max_x,
                double max_y, bool clockwise, bool mark_in_result = true,
                std::uint32_t geom_index = 0) {
  const Label lbl = AreaLabel(geom_index);
  if (clockwise) {
    AddGraphEdge(graph,
                 {{min_x, min_y},
                  {min_x, max_y},
                  {max_x, max_y},
                  {max_x, min_y},
                  {min_x, min_y}},
                 lbl, mark_in_result, false);
  } else {
    AddGraphEdge(graph,
                 {{min_x, min_y},
                  {max_x, min_y},
                  {max_x, max_y},
                  {min_x, max_y},
                  {min_x, min_y}},
                 lbl, mark_in_result, false);
  }
}

void CollectDirectedEdges(PlanarGraph& graph,
                          std::vector<DirectedEdge*>& dir_edges) {
  const std::vector<EdgeEnd*>* edge_ends = graph.getEdgeEnds();
  ASSERT_NE(edge_ends, nullptr);
  dir_edges.clear();
  dir_edges.reserve(edge_ends->size());
  for (EdgeEnd* ee : *edge_ends) {
    auto* de = dynamic_cast<DirectedEdge*>(ee);
    ASSERT_NE(de, nullptr);
    dir_edges.push_back(de);
  }
}

void PreparePartialNodeInputs(PlanarGraph& graph, const Coordinate& node_coord,
                              std::vector<DirectedEdge*>& dir_edges,
                              std::vector<Node*>& partial_nodes) {
  ASSERT_NO_FATAL_FAILURE(CollectDirectedEdges(graph, dir_edges));
  Coordinate coord = node_coord;
  Node* node = graph.find(coord);
  ASSERT_NE(node, nullptr);
  partial_nodes = {node};
}

void AddGraphViaVectors(PolygonBuilder& builder, PlanarGraph& graph) {
  std::vector<DirectedEdge*> dir_edges;
  ASSERT_NO_FATAL_FAILURE(CollectDirectedEdges(graph, dir_edges));

  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  builder.add(&dir_edges, &nodes);
}

std::vector<const Polygon*> AsSortedPolygonsByAreaDesc(
    const std::vector<std::unique_ptr<Geometry>>& geoms) {
  std::vector<const Polygon*> polys;
  polys.reserve(geoms.size());
  for (const auto& geom : geoms) {
    EXPECT_NE(geom, nullptr);
    if (geom == nullptr) {
      continue;
    }
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(geom->isValid());
    const auto* poly = dynamic_cast<const Polygon*>(geom.get());
    EXPECT_NE(poly, nullptr);
    if (poly != nullptr) {
      polys.push_back(poly);
    }
  }
  std::sort(polys.begin(), polys.end(), [](const Polygon* a, const Polygon* b) {
    return a->getArea() > b->getArea();
  });
  return polys;
}

TEST(PolygonBuilderTest, EmptyInputsProduceNoPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  PolygonBuilder builder(factory.get());

  EXPECT_THAT(builder.getPolygons(), IsEmpty());

  PlanarGraph empty_graph(BufferNodeFactory::instance());
  builder.add(&empty_graph);
  EXPECT_THAT(builder.getPolygons(), IsEmpty());

  std::vector<DirectedEdge*> empty_edges;
  std::vector<Node*> empty_nodes;
  builder.add(&empty_edges, &empty_nodes);
  EXPECT_THAT(builder.getPolygons(), IsEmpty());
}

TEST(PolygonBuilderTest, MultiEdgeShellSkipsNonResultAndLineEdges) {
  PrecisionModel pm(100.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 3857);
  PolygonBuilder builder(factory.get());
  PlanarGraph graph(BufferNodeFactory::instance());

  // Build CW [0,10]x[0,10] out of four edges, including one backward edge.
  AddGraphEdge(graph, {{0.0, 0.0}, {0.0, 10.0}}, AreaLabel(), true, false);
  AddGraphEdge(graph, {{0.0, 10.0}, {10.0, 10.0}}, AreaLabel(), true, false);
  AddGraphEdge(
      graph, {{10.0, 0.0}, {10.0, 10.0}},
      AreaLabel(/*geom_index=*/0, Location::INTERIOR, Location::EXTERIOR),
      false, true);
  AddGraphEdge(graph, {{10.0, 0.0}, {0.0, 0.0}}, AreaLabel(), true, false);

  // Non-result area edge and in-result line edge must both be ignored.
  AddGraphEdge(graph, {{0.0, 0.0}, {5.0, 5.0}}, AreaLabel(), false, false);
  AddGraphEdge(graph, {{20.0, 0.0}, {25.0, 0.0}}, Label(0, Location::INTERIOR),
               true, false);

  builder.add(&graph);

  const std::vector<std::unique_ptr<Geometry>> first_result =
      builder.getPolygons();
  ASSERT_EQ(first_result.size(), 1u);
  const auto polys = AsSortedPolygonsByAreaDesc(first_result);
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_EQ(polys[0]->getFactory(), factory.get());
  EXPECT_EQ(polys[0]->getSRID(), 3857);
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 100.0);

  // Calling getPolygons() repeatedly is non-destructive.
  const std::vector<std::unique_ptr<Geometry>> second_result =
      builder.getPolygons();
  ASSERT_EQ(second_result.size(), 1u);
  EXPECT_TRUE(second_result[0]->equalsExact(first_result[0].get(), 1e-12));
}

TEST(PolygonBuilderTest, AcceptsAreaLabelOnSecondGeometryIndex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  PolygonBuilder builder(factory.get());
  PlanarGraph graph(BufferNodeFactory::instance());

  AddGraphEdge(graph, {{0.0, 0.0}, {0.0, 6.0}, {8.0, 0.0}, {0.0, 0.0}},
               AreaLabel(/*geom_index=*/1), true, false);

  builder.add(&graph);
  const auto geoms = builder.getPolygons();
  const auto polys = AsSortedPolygonsByAreaDesc(geoms);
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 24.0);
}

TEST(PolygonBuilderTest,
     DirectDirectedEdgesAndNodesApiSupportsIncrementalAdds) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  PolygonBuilder builder(factory.get());

  // Step 1: Add outer CW shell [0,20]x[0,20].
  {
    PlanarGraph g1(BufferNodeFactory::instance());
    AddBoxRing(g1, 0.0, 0.0, 20.0, 20.0, /*clockwise=*/true);
    AddGraphViaVectors(builder, g1);

    const auto geoms = builder.getPolygons();
    const auto polys = AsSortedPolygonsByAreaDesc(geoms);
    ASSERT_EQ(polys.size(), 1u);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 400.0);
    EXPECT_EQ(polys[0]->getNumInteriorRing(), 0u);
  }

  // Step 2: Add first CCW hole [4,16]x[4,16] inside the existing shell.
  {
    PlanarGraph g2(BufferNodeFactory::instance());
    AddBoxRing(g2, 4.0, 4.0, 16.0, 16.0, /*clockwise=*/false);
    AddGraphViaVectors(builder, g2);

    const auto geoms = builder.getPolygons();
    const auto polys = AsSortedPolygonsByAreaDesc(geoms);
    ASSERT_EQ(polys.size(), 1u);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 256.0);
    EXPECT_EQ(polys[0]->getNumInteriorRing(), 1u);
  }

  // Step 3: Add a second disjoint shell [30,40]x[0,10] and a second hole
  // [1,3]x[1,3] whose envelope is contained in the first shell only.
  {
    PlanarGraph g3(BufferNodeFactory::instance());
    AddBoxRing(g3, 30.0, 0.0, 40.0, 10.0, /*clockwise=*/true);
    AddBoxRing(g3, 1.0, 1.0, 3.0, 3.0, /*clockwise=*/false);
    AddGraphViaVectors(builder, g3);

    const auto geoms = builder.getPolygons();
    const auto polys = AsSortedPolygonsByAreaDesc(geoms);
    ASSERT_EQ(polys.size(), 2u);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 252.0);
    EXPECT_EQ(polys[0]->getNumInteriorRing(), 2u);
    EXPECT_DOUBLE_EQ(polys[1]->getArea(), 100.0);
    EXPECT_EQ(polys[1]->getNumInteriorRing(), 0u);
  }
}

TEST(PolygonBuilderTest, SingleGraphWithMultipleShellsAndHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  PolygonBuilder builder(factory.get());
  PlanarGraph graph(BufferNodeFactory::instance());

  AddBoxRing(graph, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true);
  AddBoxRing(graph, 20.0, 0.0, 30.0, 10.0, /*clockwise=*/true);
  AddBoxRing(graph, 2.0, 2.0, 8.0, 8.0, /*clockwise=*/false);
  AddBoxRing(graph, 22.0, 2.0, 28.0, 8.0, /*clockwise=*/false);

  builder.add(&graph);

  const auto geoms = builder.getPolygons();
  const auto polys = AsSortedPolygonsByAreaDesc(geoms);
  ASSERT_EQ(polys.size(), 2u);
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 64.0);
  EXPECT_EQ(polys[1]->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(polys[1]->getArea(), 64.0);
}

TEST(PolygonBuilderTest, ShellWithTouchingHolesSplitsMaximalEdgeRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Single hole touching shell at (0,0): node degree 4 -> MinimalEdgeRings.
  {
    PolygonBuilder builder(factory.get());
    PlanarGraph graph(BufferNodeFactory::instance());

    AddBoxRing(graph, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true);
    AddGraphEdge(graph, {{0.0, 0.0}, {6.0, 2.0}, {2.0, 6.0}, {0.0, 0.0}},
                 AreaLabel(), true, false);

    builder.add(&graph);

    const auto geoms = builder.getPolygons();
    const auto polys = AsSortedPolygonsByAreaDesc(geoms);
    ASSERT_EQ(polys.size(), 1u);
    EXPECT_EQ(polys[0]->getNumInteriorRing(), 1u);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 84.0);
  }

  // Two disjoint holes touching shell at (0,0): node degree 6.
  {
    PolygonBuilder builder(factory.get());
    PlanarGraph graph(BufferNodeFactory::instance());

    AddBoxRing(graph, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true);
    AddGraphEdge(graph, {{0.0, 0.0}, {4.0, 1.0}, {2.0, 3.0}, {0.0, 0.0}},
                 AreaLabel(), true, false);
    AddGraphEdge(graph, {{0.0, 0.0}, {3.0, 5.0}, {1.0, 4.0}, {0.0, 0.0}},
                 AreaLabel(), true, false);

    builder.add(&graph);

    const auto geoms = builder.getPolygons();
    const auto polys = AsSortedPolygonsByAreaDesc(geoms);
    ASSERT_EQ(polys.size(), 1u);
    EXPECT_EQ(polys[0]->getNumInteriorRing(), 2u);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 91.5);
  }
}

TEST(PolygonBuilderTest, ConnectedHolesAtVertexBecomeFreeHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Two CCW holes touching at (0,0) inside [-10,10]x[-10,10] shell:
  // findShell returns nullptr and both minimal rings move to freeHoleList.
  {
    PolygonBuilder builder(factory.get());
    PlanarGraph graph(BufferNodeFactory::instance());

    AddBoxRing(graph, -10.0, -10.0, 10.0, 10.0, /*clockwise=*/true);
    AddGraphEdge(graph, {{0.0, 0.0}, {4.0, 1.0}, {1.0, 4.0}, {0.0, 0.0}},
                 AreaLabel(), true, false);
    AddGraphEdge(graph, {{0.0, 0.0}, {-4.0, -1.0}, {-1.0, -4.0}, {0.0, 0.0}},
                 AreaLabel(), true, false);

    builder.add(&graph);

    const auto geoms = builder.getPolygons();
    const auto polys = AsSortedPolygonsByAreaDesc(geoms);
    ASSERT_EQ(polys.size(), 1u);
    EXPECT_EQ(polys[0]->getNumInteriorRing(), 2u);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 385.0);
  }

  // Same connected holes without any enclosing shell are discarded cleanly.
  {
    PolygonBuilder builder(factory.get());
    PlanarGraph graph(BufferNodeFactory::instance());

    AddGraphEdge(graph, {{0.0, 0.0}, {4.0, 1.0}, {1.0, 4.0}, {0.0, 0.0}},
                 AreaLabel(), true, false);
    AddGraphEdge(graph, {{0.0, 0.0}, {-4.0, -1.0}, {-1.0, -4.0}, {0.0, 0.0}},
                 AreaLabel(), true, false);

    builder.add(&graph);
    EXPECT_THAT(builder.getPolygons(), IsEmpty());
  }
}

TEST(PolygonBuilderTest, OrphanHolesOutsideAllShellsAreDiscarded) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Standalone CCW hole with no shells.
  {
    PolygonBuilder builder(factory.get());
    PlanarGraph graph(BufferNodeFactory::instance());
    AddBoxRing(graph, 2.0, 2.0, 8.0, 8.0, /*clockwise=*/false);

    builder.add(&graph);
    EXPECT_THAT(builder.getPolygons(), IsEmpty());
  }

  // Hole outside an existing shell's envelope is discarded.
  {
    PolygonBuilder builder(factory.get());
    PlanarGraph graph(BufferNodeFactory::instance());
    AddBoxRing(graph, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true);
    AddBoxRing(graph, 20.0, 20.0, 25.0, 25.0, /*clockwise=*/false);

    builder.add(&graph);
    const auto geoms = builder.getPolygons();
    const auto polys = AsSortedPolygonsByAreaDesc(geoms);
    ASSERT_EQ(polys.size(), 1u);
    EXPECT_EQ(polys[0]->getNumInteriorRing(), 0u);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 100.0);
  }
}

TEST(PolygonBuilderTest, ConcaveShellEnvelopeAndEqualEnvelopeHandling) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Hole [10,15]x[10,15] lies inside the L-shaped shell's [0,20]x[0,20]
  // envelope, but lies in the exterior concavity and must be discarded.
  {
    PolygonBuilder builder(factory.get());
    PlanarGraph graph(BufferNodeFactory::instance());

    AddGraphEdge(graph,
                 {{0.0, 0.0},
                  {0.0, 20.0},
                  {4.0, 20.0},
                  {4.0, 4.0},
                  {20.0, 4.0},
                  {20.0, 0.0},
                  {0.0, 0.0}},
                 AreaLabel(), true, false);
    AddBoxRing(graph, 10.0, 10.0, 15.0, 15.0, /*clockwise=*/false);

    builder.add(&graph);
    const auto geoms = builder.getPolygons();
    const auto polys = AsSortedPolygonsByAreaDesc(geoms);
    ASSERT_EQ(polys.size(), 1u);
    EXPECT_EQ(polys[0]->getNumInteriorRing(), 0u);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 144.0);
  }

  // Hole sharing exact [0,10]x[0,10] envelope with a shell skips that shell
  // and is placed inside the larger [-5,15]x[-5,15] outer shell.
  {
    PolygonBuilder builder(factory.get());

    PlanarGraph shells_graph(BufferNodeFactory::instance());
    AddBoxRing(shells_graph, -5.0, -5.0, 15.0, 15.0, /*clockwise=*/true);
    AddBoxRing(shells_graph, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true);
    builder.add(&shells_graph);

    PlanarGraph hole_graph(BufferNodeFactory::instance());
    AddGraphEdge(hole_graph,
                 {{0.0, 5.0}, {5.0, 0.0}, {10.0, 5.0}, {5.0, 10.0}, {0.0, 5.0}},
                 AreaLabel(), true, false);
    builder.add(&hole_graph);

    const auto geoms = builder.getPolygons();
    const auto polys = AsSortedPolygonsByAreaDesc(geoms);
    ASSERT_EQ(polys.size(), 2u);
    EXPECT_EQ(polys[0]->getNumInteriorRing(), 1u);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 350.0);
    EXPECT_EQ(polys[1]->getNumInteriorRing(), 0u);
    EXPECT_DOUBLE_EQ(polys[1]->getArea(), 100.0);
  }

  // Equal-envelope hole with only the equal-envelope shell is discarded.
  {
    PolygonBuilder builder(factory.get());

    PlanarGraph shell_graph(BufferNodeFactory::instance());
    AddBoxRing(shell_graph, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true);
    builder.add(&shell_graph);

    PlanarGraph hole_graph(BufferNodeFactory::instance());
    AddGraphEdge(hole_graph,
                 {{0.0, 5.0}, {5.0, 0.0}, {10.0, 5.0}, {5.0, 10.0}, {0.0, 5.0}},
                 AreaLabel(), true, false);
    builder.add(&hole_graph);

    const auto geoms = builder.getPolygons();
    const auto polys = AsSortedPolygonsByAreaDesc(geoms);
    ASSERT_EQ(polys.size(), 1u);
    EXPECT_EQ(polys[0]->getNumInteriorRing(), 0u);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 100.0);
  }
}

TEST(PolygonBuilderTest, NestedIslandsSelectInnermostShellRegardlessOfOrder) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  for (bool outer_first : {true, false}) {
    PolygonBuilder builder(factory.get());

    PlanarGraph outer_shell(BufferNodeFactory::instance());
    AddBoxRing(outer_shell, 0.0, 0.0, 100.0, 100.0, /*clockwise=*/true);

    PlanarGraph island_shell(BufferNodeFactory::instance());
    AddBoxRing(island_shell, 20.0, 20.0, 80.0, 80.0, /*clockwise=*/true);

    if (outer_first) {
      builder.add(&outer_shell);
      builder.add(&island_shell);
    } else {
      builder.add(&island_shell);
      builder.add(&outer_shell);
    }

    PlanarGraph holes_graph(BufferNodeFactory::instance());
    AddBoxRing(holes_graph, 10.0, 10.0, 90.0, 90.0, /*clockwise=*/false);
    AddBoxRing(holes_graph, 30.0, 30.0, 70.0, 70.0, /*clockwise=*/false);
    builder.add(&holes_graph);

    const auto geoms = builder.getPolygons();
    const auto polys = AsSortedPolygonsByAreaDesc(geoms);
    ASSERT_EQ(polys.size(), 2u);

    const LinearRing* outer_ext = polys[0]->getExteriorRing();
    ASSERT_NE(outer_ext, nullptr);
    const Envelope* outer_ext_env = outer_ext->getEnvelopeInternal();
    EXPECT_DOUBLE_EQ(outer_ext_env->getWidth(), 100.0);
    ASSERT_EQ(polys[0]->getNumInteriorRing(), 1u);
    const LinearRing* outer_hole = polys[0]->getInteriorRingN(0);
    ASSERT_NE(outer_hole, nullptr);
    EXPECT_DOUBLE_EQ(outer_hole->getEnvelopeInternal()->getWidth(), 80.0);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 3600.0);

    const LinearRing* island_ext = polys[1]->getExteriorRing();
    ASSERT_NE(island_ext, nullptr);
    EXPECT_DOUBLE_EQ(island_ext->getEnvelopeInternal()->getWidth(), 60.0);
    ASSERT_EQ(polys[1]->getNumInteriorRing(), 1u);
    const LinearRing* island_hole = polys[1]->getInteriorRingN(0);
    ASSERT_NE(island_hole, nullptr);
    EXPECT_DOUBLE_EQ(island_hole->getEnvelopeInternal()->getWidth(), 40.0);
    EXPECT_DOUBLE_EQ(polys[1]->getArea(), 2000.0);
  }
}

TEST(PolygonBuilderTest, BothGeometryIndicesAreaLabelProducesShell) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  PolygonBuilder builder(factory.get());
  PlanarGraph graph(BufferNodeFactory::instance());

  AddGraphEdge(
      graph, {{0.0, 0.0}, {0.0, 5.0}, {4.0, 5.0}, {4.0, 0.0}, {0.0, 0.0}},
      Label(Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR), true,
      false);

  builder.add(&graph);
  const auto geoms = builder.getPolygons();
  const auto polys = AsSortedPolygonsByAreaDesc(geoms);
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 20.0);
}

TEST(PolygonBuilderTest, HoleWithVertexOnShellBoundarySegmentIsPlacedInside) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  PolygonBuilder builder(factory.get());
  PlanarGraph graph(BufferNodeFactory::instance());

  AddBoxRing(graph, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true);
  // First vertex (5,0) lies on the shell's bottom segment (not a shell vertex),
  // exercising Location::BOUNDARY != Location::EXTERIOR in PIP containment.
  AddGraphEdge(graph, {{5.0, 0.0}, {8.0, 4.0}, {2.0, 4.0}, {5.0, 0.0}},
               AreaLabel(), true, false);

  builder.add(&graph);
  const auto geoms = builder.getPolygons();
  const auto polys = AsSortedPolygonsByAreaDesc(geoms);
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 88.0);
}

TEST(PolygonBuilderTest,
     MaximalShellWithTouchingHoleAlsoAcceptsDisjointFreeHole) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  PolygonBuilder builder(factory.get());
  PlanarGraph graph(BufferNodeFactory::instance());

  AddBoxRing(graph, 0.0, 0.0, 20.0, 20.0, /*clockwise=*/true);
  AddGraphEdge(graph, {{0.0, 0.0}, {6.0, 2.0}, {2.0, 6.0}, {0.0, 0.0}},
               AreaLabel(), true, false);
  AddBoxRing(graph, 12.0, 12.0, 16.0, 16.0, /*clockwise=*/false);

  builder.add(&graph);
  const auto geoms = builder.getPolygons();
  const auto polys = AsSortedPolygonsByAreaDesc(geoms);
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 2u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 368.0);
}

TEST(PolygonBuilderTest, ThreeLevelNestedShellsSelectInnermostContainingShell) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  for (bool outer_first : {true, false}) {
    PolygonBuilder builder(factory.get());

    PlanarGraph outer_shell(BufferNodeFactory::instance());
    AddBoxRing(outer_shell, 0.0, 0.0, 100.0, 100.0, /*clockwise=*/true);

    PlanarGraph middle_shell(BufferNodeFactory::instance());
    AddBoxRing(middle_shell, 20.0, 20.0, 80.0, 80.0, /*clockwise=*/true);

    PlanarGraph inner_shell(BufferNodeFactory::instance());
    AddBoxRing(inner_shell, 40.0, 40.0, 60.0, 60.0, /*clockwise=*/true);

    if (outer_first) {
      builder.add(&outer_shell);
      builder.add(&middle_shell);
      builder.add(&inner_shell);
    } else {
      builder.add(&inner_shell);
      builder.add(&middle_shell);
      builder.add(&outer_shell);
    }

    PlanarGraph holes_graph(BufferNodeFactory::instance());
    AddBoxRing(holes_graph, 10.0, 10.0, 90.0, 90.0, /*clockwise=*/false);
    AddBoxRing(holes_graph, 30.0, 30.0, 70.0, 70.0, /*clockwise=*/false);
    AddBoxRing(holes_graph, 45.0, 45.0, 55.0, 55.0, /*clockwise=*/false);
    builder.add(&holes_graph);

    const auto geoms = builder.getPolygons();
    const auto polys = AsSortedPolygonsByAreaDesc(geoms);
    ASSERT_EQ(polys.size(), 3u);

    EXPECT_EQ(polys[0]->getNumInteriorRing(), 1u);
    EXPECT_DOUBLE_EQ(polys[0]->getArea(), 3600.0);
    EXPECT_DOUBLE_EQ(
        polys[0]->getExteriorRing()->getEnvelopeInternal()->getWidth(), 100.0);
    EXPECT_DOUBLE_EQ(
        polys[0]->getInteriorRingN(0)->getEnvelopeInternal()->getWidth(), 80.0);

    EXPECT_EQ(polys[1]->getNumInteriorRing(), 1u);
    EXPECT_DOUBLE_EQ(polys[1]->getArea(), 2000.0);
    EXPECT_DOUBLE_EQ(
        polys[1]->getExteriorRing()->getEnvelopeInternal()->getWidth(), 60.0);
    EXPECT_DOUBLE_EQ(
        polys[1]->getInteriorRingN(0)->getEnvelopeInternal()->getWidth(), 40.0);

    EXPECT_EQ(polys[2]->getNumInteriorRing(), 1u);
    EXPECT_DOUBLE_EQ(polys[2]->getArea(), 300.0);
    EXPECT_DOUBLE_EQ(
        polys[2]->getExteriorRing()->getEnvelopeInternal()->getWidth(), 20.0);
    EXPECT_DOUBLE_EQ(
        polys[2]->getInteriorRingN(0)->getEnvelopeInternal()->getWidth(), 10.0);
  }
}

TEST(PolygonBuilderTest, TouchingShellsAtSharedVertexStaySeparateShells) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  PolygonBuilder builder(factory.get());
  PlanarGraph graph(BufferNodeFactory::instance());

  // Shell 1 [0,10]x[0,10] with a hole touching at (0,0) (node degree > 2).
  AddBoxRing(graph, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true);
  AddGraphEdge(graph, {{0.0, 0.0}, {6.0, 2.0}, {2.0, 6.0}, {0.0, 0.0}},
               AreaLabel(), true, false);

  // Shell 2 [-10,0]x[-10,0] also meets (0,0), plus a free hole inside it.
  AddGraphEdge(
      graph,
      {{0.0, 0.0}, {0.0, -10.0}, {-10.0, -10.0}, {-10.0, 0.0}, {0.0, 0.0}},
      AreaLabel(), true, false);
  AddBoxRing(graph, -8.0, -8.0, -2.0, -2.0, /*clockwise=*/false);

  builder.add(&graph);
  const auto geoms = builder.getPolygons();
  const auto polys = AsSortedPolygonsByAreaDesc(geoms);
  ASSERT_EQ(polys.size(), 2u);
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 84.0);
  EXPECT_EQ(polys[1]->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(polys[1]->getArea(), 64.0);
}

TEST(PolygonBuilderTest, ExceptionPathsCleanUpMaximalRingsAndRethrow) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Case 1: Unmatched incoming result edge throws during directed-edge linking.
  {
    PolygonBuilder builder(factory.get());
    PlanarGraph graph(BufferNodeFactory::instance());
    AddGraphEdge(graph, {{0.0, 0.0}, {5.0, 0.0}}, AreaLabel(), true, false);

    EXPECT_THROW(builder.add(&graph), TopologyException);
    EXPECT_THAT(builder.getPolygons(), IsEmpty());
  }

  // Case 2a: Valid ring followed by an unclosed 2-point DirectedEdge throws
  // IllegalArgumentException inside LinearRing::validateConstruction via
  // MaximalEdgeRing, cleaning up the earlier MaximalEdgeRing.
  {
    PolygonBuilder builder(factory.get());
    PlanarGraph graph(BufferNodeFactory::instance());
    AddBoxRing(graph, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true);
    DirectedEdge* unclosed_de = AddGraphEdge(graph, {{20.0, 0.0}, {30.0, 0.0}},
                                             AreaLabel(), true, false);
    ASSERT_NE(unclosed_de, nullptr);
    unclosed_de->setNext(unclosed_de);

    std::vector<DirectedEdge*> dir_edges;
    std::vector<Node*> partial_nodes;
    ASSERT_NO_FATAL_FAILURE(PreparePartialNodeInputs(
        graph, Coordinate(0.0, 0.0), dir_edges, partial_nodes));

    EXPECT_THROW(builder.add(&dir_edges, &partial_nodes),
                 IllegalArgumentException);
    EXPECT_THAT(builder.getPolygons(), IsEmpty());
  }

  // Case 2b: Valid ring followed by a closed ring with a non-finite interior
  // vertex throws IllegalArgumentException inside Orientation::isCCW during
  // MaximalEdgeRing construction via PlanarGraph*, cleaning up earlier rings.
  {
    PolygonBuilder builder(factory.get());
    PlanarGraph graph(BufferNodeFactory::instance());
    AddBoxRing(graph, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true);
    AddGraphEdge(graph,
                 {{20.0, 0.0},
                  {25.0, 1.0},
                  {26.0, 10.0},
                  {28.0, -std::numeric_limits<double>::infinity()},
                  {29.0, -1.0},
                  {20.0, 0.0}},
                 AreaLabel(), true, false);

    EXPECT_THROW(builder.add(&graph), IllegalArgumentException);
    EXPECT_THAT(builder.getPolygons(), IsEmpty());
  }

  // Case 3: Valid ring followed by an unlinked DirectedEdge (node omitted from
  // nodes list) throws TopologyException in EdgeRing::computePoints and cleans
  // up the earlier MaximalEdgeRing.
  {
    PolygonBuilder builder(factory.get());
    PlanarGraph graph(BufferNodeFactory::instance());
    AddBoxRing(graph, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true);
    AddGraphEdge(graph, {{20.0, 0.0}, {30.0, 0.0}}, AreaLabel(), true, false);

    std::vector<DirectedEdge*> dir_edges;
    std::vector<Node*> partial_nodes;
    ASSERT_NO_FATAL_FAILURE(PreparePartialNodeInputs(
        graph, Coordinate(0.0, 0.0), dir_edges, partial_nodes));

    EXPECT_THROW(builder.add(&dir_edges, &partial_nodes), TopologyException);
    EXPECT_THAT(builder.getPolygons(), IsEmpty());
  }

  // Case 4: Valid ring followed by a DirectedEdge chain terminating in a
  // self-loop throws TopologyException ("Directed Edge visited twice") in
  // EdgeRing::computePoints and cleans up the earlier MaximalEdgeRing.
  {
    PolygonBuilder builder(factory.get());
    PlanarGraph graph(BufferNodeFactory::instance());
    AddBoxRing(graph, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true);
    DirectedEdge* de1 = AddGraphEdge(graph, {{20.0, 0.0}, {30.0, 0.0}},
                                     AreaLabel(), true, false);
    DirectedEdge* de2 = AddGraphEdge(graph, {{30.0, 0.0}, {40.0, 0.0}},
                                     AreaLabel(), true, false);
    ASSERT_NE(de1, nullptr);
    ASSERT_NE(de2, nullptr);
    de1->setNext(de2);
    de2->setNext(de2);

    std::vector<DirectedEdge*> dir_edges;
    std::vector<Node*> partial_nodes;
    ASSERT_NO_FATAL_FAILURE(PreparePartialNodeInputs(
        graph, Coordinate(0.0, 0.0), dir_edges, partial_nodes));

    EXPECT_THROW(builder.add(&dir_edges, &partial_nodes), TopologyException);
    EXPECT_THAT(builder.getPolygons(), IsEmpty());
  }
}

}  // namespace
}  // namespace geos_google
