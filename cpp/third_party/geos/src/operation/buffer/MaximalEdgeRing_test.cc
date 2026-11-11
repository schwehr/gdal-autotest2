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

#include "third_party/geos/include/geos/operation/buffer/MaximalEdgeRing.h"

#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdge.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/EdgeRing.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/PlanarGraph.h"
#include "third_party/geos/include/geos/operation/buffer/BufferNodeFactory.h"
#include "third_party/geos/include/geos/operation/buffer/MinimalEdgeRing.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::geomgraph::DirectedEdge;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeEnd;
using geos::geomgraph::EdgeRing;
using geos::geomgraph::Label;
using geos::geomgraph::PlanarGraph;
using geos::operation::buffer::BufferNodeFactory;
using geos::operation::buffer::MaximalEdgeRing;
using geos::operation::buffer::MinimalEdgeRing;
using geos::util::TopologyException;

Edge* MakeEdge(std::initializer_list<Coordinate> coords, const Label& label) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(seq.release(), label).release();
}

struct GraphFixture {
  std::unique_ptr<PlanarGraph> graph;
  DirectedEdge* start_de = nullptr;
};

// Builds a CW outer triangle shell and a CCW inner triangle hole meeting at a
// degree-4 node at (0, 0). Result linking joins both into one MaximalEdgeRing.
GraphFixture BuildShellWithTouchingHoleGraph() {
  GraphFixture fixture;
  fixture.graph = std::make_unique<PlanarGraph>(BufferNodeFactory::instance());

  const Label area_label(Location::BOUNDARY, Location::EXTERIOR,
                         Location::INTERIOR);
  // Outer shell oriented CW: (0,0) -> (-10,10) -> (10,10) -> (0,0).
  Edge* s1 =
      MakeEdge({Coordinate(0.0, 0.0), Coordinate(-10.0, 10.0)}, area_label);
  Edge* s2 =
      MakeEdge({Coordinate(-10.0, 10.0), Coordinate(10.0, 10.0)}, area_label);
  Edge* s3 =
      MakeEdge({Coordinate(10.0, 10.0), Coordinate(0.0, 0.0)}, area_label);

  // Inner hole oriented CCW: (0,0) -> (3,6) -> (-3,6) -> (0,0).
  Edge* h1 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(3.0, 6.0)}, area_label);
  Edge* h2 =
      MakeEdge({Coordinate(3.0, 6.0), Coordinate(-3.0, 6.0)}, area_label);
  Edge* h3 =
      MakeEdge({Coordinate(-3.0, 6.0), Coordinate(0.0, 0.0)}, area_label);

  fixture.graph->addEdges({s1, s2, s3, h1, h2, h3});

  for (EdgeEnd* ee : *fixture.graph->getEdgeEnds()) {
    auto* de = dynamic_cast<DirectedEdge*>(ee);
    if (de != nullptr && de->isForward()) {
      de->setInResult(true);
      if (de->getEdge() == s1) {
        fixture.start_de = de;
      }
    }
  }

  fixture.graph->linkResultDirectedEdges();
  return fixture;
}

TEST(MaximalEdgeRingTest, SimpleDegreeTwoRingAccessorsAndMinimalBuild) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  PlanarGraph graph(BufferNodeFactory::instance());
  const Label cw_label(Location::BOUNDARY, Location::EXTERIOR,
                       Location::INTERIOR);

  Edge* e1 = MakeEdge(
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0)},
      cw_label);
  Edge* e2 = MakeEdge(
      {Coordinate(10.0, 10.0), Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
      cw_label);
  graph.addEdges({e1, e2});

  DirectedEdge* de1_fwd = nullptr;
  DirectedEdge* de2_fwd = nullptr;
  DirectedEdge* de1_bwd = nullptr;
  for (EdgeEnd* ee : *graph.getEdgeEnds()) {
    auto* de = dynamic_cast<DirectedEdge*>(ee);
    ASSERT_NE(de, nullptr);
    if (de->getEdge() == e1) {
      if (de->isForward()) {
        de->setInResult(true);
        de1_fwd = de;
      } else {
        de1_bwd = de;
      }
    } else if (de->isForward()) {
      de->setInResult(true);
      de2_fwd = de;
    }
  }
  ASSERT_NE(de1_fwd, nullptr);
  ASSERT_NE(de2_fwd, nullptr);
  ASSERT_NE(de1_bwd, nullptr);
  graph.linkResultDirectedEdges();

  MaximalEdgeRing max_ring(de1_fwd, factory.get());
  EXPECT_EQ(max_ring.getEdges().size(), 2u);
  EXPECT_EQ(max_ring.getMaxNodeDegree(), 2);
  EXPECT_FALSE(max_ring.isHole());
  EXPECT_TRUE(max_ring.isShell());
  EXPECT_EQ(max_ring.getLinearRing()->getNumPoints(), 5u);
  EXPECT_EQ(max_ring.getNext(de1_fwd), de2_fwd);
  EXPECT_EQ(max_ring.getNext(de2_fwd), de1_fwd);
  EXPECT_EQ(de1_fwd->getEdgeRing(), &max_ring);
  EXPECT_EQ(de2_fwd->getEdgeRing(), &max_ring);

  // Direct setEdgeRing verification on an unrelated DirectedEdge.
  max_ring.setEdgeRing(de1_bwd, &max_ring);
  EXPECT_EQ(de1_bwd->getEdgeRing(), &max_ring);
  max_ring.setEdgeRing(de1_bwd, nullptr);
  EXPECT_EQ(de1_bwd->getEdgeRing(), nullptr);

  std::unique_ptr<Polygon> poly = max_ring.toPolygon(factory.get());
  EXPECT_DOUBLE_EQ(poly->getArea(), 100.0);
  EXPECT_EQ(poly->getNumInteriorRing(), 0u);

  max_ring.linkDirectedEdgesForMinimalEdgeRings();
  std::vector<MinimalEdgeRing*> min_rings;
  max_ring.buildMinimalRings(min_rings);
  ASSERT_EQ(min_rings.size(), 1u);
  EXPECT_FALSE(min_rings[0]->isHole());
  EXPECT_DOUBLE_EQ(min_rings[0]->toPolygon(factory.get())->getArea(), 100.0);

  // Re-invoking buildMinimalRings after minEdgeRing is populated skips all.
  std::vector<MinimalEdgeRing*> second_pass;
  max_ring.buildMinimalRings(second_pass);
  EXPECT_TRUE(second_pass.empty());

  for (MinimalEdgeRing* ring : min_rings) {
    delete ring;
  }
}

TEST(MaximalEdgeRingTest, DegreeFourRingLinksAndBuildsMinimalRingsVector) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphFixture fixture = BuildShellWithTouchingHoleGraph();
  ASSERT_NE(fixture.start_de, nullptr);

  MaximalEdgeRing max_ring(fixture.start_de, factory.get());
  EXPECT_EQ(max_ring.getEdges().size(), 6u);
  EXPECT_EQ(max_ring.getMaxNodeDegree(), 4);

  max_ring.linkDirectedEdgesForMinimalEdgeRings();

  std::vector<MinimalEdgeRing*> min_rings;
  max_ring.buildMinimalRings(min_rings);
  ASSERT_EQ(min_rings.size(), 2u);

  MinimalEdgeRing* shell = nullptr;
  MinimalEdgeRing* hole = nullptr;
  for (MinimalEdgeRing* ring : min_rings) {
    if (ring->isHole()) {
      hole = ring;
    } else {
      shell = ring;
    }
  }
  ASSERT_NE(shell, nullptr);
  ASSERT_NE(hole, nullptr);

  EXPECT_EQ(shell->getEdges().size(), 3u);
  EXPECT_EQ(hole->getEdges().size(), 3u);
  EXPECT_DOUBLE_EQ(shell->toPolygon(factory.get())->getArea(), 100.0);
  EXPECT_DOUBLE_EQ(hole->toPolygon(factory.get())->getArea(), 18.0);

  // setShell transfers ownership of hole to shell.
  hole->setShell(shell);
  std::unique_ptr<Polygon> poly = shell->toPolygon(factory.get());
  EXPECT_TRUE(poly->isValid());
  EXPECT_EQ(poly->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 82.0);

  delete shell;
}

TEST(MaximalEdgeRingTest, BuildMinimalRingsPointerVectorOverload) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphFixture fixture = BuildShellWithTouchingHoleGraph();
  ASSERT_NE(fixture.start_de, nullptr);

  MaximalEdgeRing max_ring(fixture.start_de, factory.get());
  max_ring.linkDirectedEdgesForMinimalEdgeRings();

  std::unique_ptr<std::vector<MinimalEdgeRing*>> min_rings(
      max_ring.buildMinimalRings());
  ASSERT_NE(min_rings, nullptr);
  ASSERT_EQ(min_rings->size(), 2u);

  int shell_count = 0;
  int hole_count = 0;
  for (MinimalEdgeRing* ring : *min_rings) {
    if (ring->isHole()) {
      ++hole_count;
    } else {
      ++shell_count;
    }
    delete ring;
  }
  EXPECT_EQ(shell_count, 1);
  EXPECT_EQ(hole_count, 1);
}

TEST(MaximalEdgeRingTest, BuildMinimalRingsBaseEdgeRingVectorOverload) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GraphFixture fixture = BuildShellWithTouchingHoleGraph();
  ASSERT_NE(fixture.start_de, nullptr);

  MaximalEdgeRing max_ring(fixture.start_de, factory.get());
  max_ring.linkDirectedEdgesForMinimalEdgeRings();

  std::vector<EdgeRing*> base_rings;
  max_ring.buildMinimalRings(base_rings);
  ASSERT_EQ(base_rings.size(), 2u);

  int shell_count = 0;
  int hole_count = 0;
  for (EdgeRing* ring : base_rings) {
    if (ring->isHole()) {
      ++hole_count;
    } else {
      ++shell_count;
    }
    delete ring;
  }
  EXPECT_EQ(shell_count, 1);
  EXPECT_EQ(hole_count, 1);
}

TEST(MaximalEdgeRingTest, BackwardDirectedEdgesFormValidCcwHoleRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  PlanarGraph graph(BufferNodeFactory::instance());
  // Forward edge is CW with INTERIOR on LEFT and EXTERIOR on RIGHT, so its
  // backward DirectedEdge is CCW with EXTERIOR on LEFT and INTERIOR on RIGHT.
  const Label hole_fwd_label(Location::BOUNDARY, Location::INTERIOR,
                             Location::EXTERIOR);
  Edge* e1 = MakeEdge(
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0)},
      hole_fwd_label);
  Edge* e2 = MakeEdge(
      {Coordinate(10.0, 10.0), Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
      hole_fwd_label);
  graph.addEdges({e1, e2});

  DirectedEdge* start_bwd = nullptr;
  for (EdgeEnd* ee : *graph.getEdgeEnds()) {
    auto* de = dynamic_cast<DirectedEdge*>(ee);
    ASSERT_NE(de, nullptr);
    if (!de->isForward()) {
      de->setInResult(true);
      start_bwd = de;
    }
  }
  ASSERT_NE(start_bwd, nullptr);
  graph.linkResultDirectedEdges();

  std::unique_ptr<EdgeRing> poly_ring =
      std::make_unique<MaximalEdgeRing>(start_bwd, factory.get());
  EXPECT_TRUE(poly_ring->isHole());
  EXPECT_EQ(poly_ring->getEdges().size(), 2u);
  EXPECT_DOUBLE_EQ(poly_ring->toPolygon(factory.get())->getArea(), 100.0);
}

TEST(MaximalEdgeRingTest, ConstructorThrowsOnInvalidDirectedEdgeLinks) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  EXPECT_THROW(MaximalEdgeRing(nullptr, factory.get()), TopologyException);

  const Label cw_label(Location::BOUNDARY, Location::EXTERIOR,
                       Location::INTERIOR);
  auto seq1 =
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          Coordinate(0.0, 0.0), Coordinate(0.0, 10.0)});
  auto e1 = std::make_unique<Edge>(seq1.release(), cw_label);
  auto seq2 =
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          Coordinate(0.0, 10.0), Coordinate(10.0, 0.0)});
  auto e2 = std::make_unique<Edge>(seq2.release(), cw_label);

  auto de1 = std::make_unique<DirectedEdge>(e1.get(), true);
  auto de2 = std::make_unique<DirectedEdge>(e2.get(), true);

  // Unlinked chain ending in nullptr throws.
  de1->setNext(nullptr);
  EXPECT_THROW(MaximalEdgeRing(de1.get(), factory.get()), TopologyException);

  // Cycle that revisits de2 without returning to startDe throws.
  de1->setNext(de2.get());
  de2->setNext(de2.get());
  EXPECT_THROW(MaximalEdgeRing(de1.get(), factory.get()), TopologyException);
}

}  // namespace
}  // namespace geos_google
