// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/operation/buffer/MinimalEdgeRing.h"

#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdge.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/EdgeRing.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/PlanarGraph.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/buffer/BufferNodeFactory.h"
#include "third_party/geos/include/geos/operation/buffer/BufferOp.h"
#include "third_party/geos/include/geos/operation/buffer/MaximalEdgeRing.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::geomgraph::DirectedEdge;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeEnd;
using geos::geomgraph::EdgeRing;
using geos::geomgraph::Label;
using geos::geomgraph::PlanarGraph;
using geos::io::WKTReader;
using geos::operation::buffer::BufferNodeFactory;
using geos::operation::buffer::BufferOp;
using geos::operation::buffer::MaximalEdgeRing;
using geos::operation::buffer::MinimalEdgeRing;
using geos::util::TopologyException;

Label MakeAreaLabel(Location left_loc, Location right_loc) {
  return Label(0, Location::BOUNDARY, left_loc, right_loc);
}

// Builds a degree-4 pinched planar graph from two closed loops meeting at a
// vertex, and links a single MaximalEdgeRing over both.
struct PinchedMaximalRingSetup {
  PlanarGraph graph;
  DirectedEdge* start_de = nullptr;
  std::unique_ptr<MaximalEdgeRing> max_ring;

  PinchedMaximalRingSetup(const GeometryFactory* gf,
                          const CoordinateSequence& first_pts,
                          const CoordinateSequence& second_pts)
      : graph(BufferNodeFactory::instance()) {
    // PlanarGraph owns the Edge* objects added via addEdges().
    std::vector<Edge*> edges{
        new Edge(new CoordinateSequence(first_pts),
                 MakeAreaLabel(Location::EXTERIOR, Location::INTERIOR)),
        new Edge(new CoordinateSequence(second_pts),
                 MakeAreaLabel(Location::EXTERIOR, Location::INTERIOR)),
    };
    graph.addEdges(edges);

    const std::vector<EdgeEnd*>* edge_ends = graph.getEdgeEnds();
    if (edge_ends != nullptr) {
      for (EdgeEnd* ee : *edge_ends) {
        auto* de = static_cast<DirectedEdge*>(ee);
        if (de->isForward()) {
          de->setInResult(true);
          if (start_de == nullptr) {
            start_de = de;
          }
        }
      }
    }
    graph.linkResultDirectedEdges();
    if (start_de != nullptr) {
      max_ring = std::make_unique<MaximalEdgeRing>(start_de, gf);
      max_ring->linkDirectedEdgesForMinimalEdgeRings();
    }
  }

  explicit PinchedMaximalRingSetup(const GeometryFactory* gf)
      : PinchedMaximalRingSetup(gf,
                                CoordinateSequence{
                                    CoordinateXY(0.0, 0.0),
                                    CoordinateXY(0.0, 10.0),
                                    CoordinateXY(10.0, 10.0),
                                    CoordinateXY(10.0, 0.0),
                                    CoordinateXY(0.0, 0.0),
                                },
                                CoordinateSequence{
                                    CoordinateXY(0.0, 0.0),
                                    CoordinateXY(5.0, 2.0),
                                    CoordinateXY(5.0, 5.0),
                                    CoordinateXY(2.0, 5.0),
                                    CoordinateXY(0.0, 0.0),
                                }) {}
};

TEST(MinimalEdgeRingTest, StandaloneClockwiseShellMinimalRing) {
  GeometryFactory::Ptr gf = GeometryFactory::create();

  // Clockwise ring has EXTERIOR on LEFT and INTERIOR on RIGHT.
  auto* pts = new CoordinateSequence{
      CoordinateXY(0.0, 0.0),   CoordinateXY(0.0, 10.0),
      CoordinateXY(10.0, 10.0), CoordinateXY(10.0, 0.0),
      CoordinateXY(0.0, 0.0),
  };
  auto edge = std::make_unique<Edge>(
      pts, MakeAreaLabel(Location::EXTERIOR, Location::INTERIOR));
  DirectedEdge de(edge.get(), true);
  de.setNextMin(&de);

  MinimalEdgeRing ring(&de, gf.get());

  EXPECT_EQ(ring.getNext(&de), &de);
  EXPECT_EQ(de.getMinEdgeRing(), &ring);
  EXPECT_EQ(de.getEdgeRing(), nullptr);
  EXPECT_FALSE(ring.isHole());
  EXPECT_TRUE(ring.isShell());
  EXPECT_TRUE(ring.isIsolated());
  ASSERT_EQ(ring.getEdges().size(), 1u);
  EXPECT_EQ(ring.getEdges()[0], &de);

  const LinearRing* lr = ring.getLinearRing();
  ASSERT_NE(lr, nullptr);
  EXPECT_EQ(lr->getNumPoints(), 5u);
  EXPECT_TRUE(ring.containsPoint(Coordinate(5.0, 5.0)));
  EXPECT_FALSE(ring.containsPoint(Coordinate(15.0, 5.0)));

  // computeRing is idempotent once computed.
  ring.computeRing();
  EXPECT_EQ(ring.getLinearRing(), lr);

  std::unique_ptr<Polygon> poly = ring.toPolygon(gf.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_DOUBLE_EQ(poly->getArea(), 100.0);
  EXPECT_EQ(poly->getNumInteriorRing(), 0u);
}

TEST(MinimalEdgeRingTest,
     StandaloneCounterClockwiseHoleWithBackwardEdgeAndShell) {
  GeometryFactory::Ptr gf = GeometryFactory::create();

  auto* shell_pts = new CoordinateSequence{
      CoordinateXY(0.0, 0.0),   CoordinateXY(0.0, 10.0),
      CoordinateXY(10.0, 10.0), CoordinateXY(10.0, 0.0),
      CoordinateXY(0.0, 0.0),
  };
  auto shell_edge = std::make_unique<Edge>(
      shell_pts, MakeAreaLabel(Location::EXTERIOR, Location::INTERIOR));
  DirectedEdge shell_de(shell_edge.get(), true);
  shell_de.setNextMin(&shell_de);
  MinimalEdgeRing shell(&shell_de, gf.get());

  // Build hole points reversed (CW) so backward DirectedEdge traverses CCW.
  // For a forward CW edge where LEFT=INTERIOR and RIGHT=EXTERIOR, the
  // backward DirectedEdge swaps sides to LEFT=EXTERIOR and RIGHT=INTERIOR.
  auto* hole_pts = new CoordinateSequence{
      CoordinateXY(2.0, 2.0), CoordinateXY(2.0, 6.0), CoordinateXY(6.0, 6.0),
      CoordinateXY(6.0, 2.0), CoordinateXY(2.0, 2.0),
  };
  auto hole_edge = std::make_unique<Edge>(
      hole_pts, MakeAreaLabel(Location::INTERIOR, Location::EXTERIOR));
  DirectedEdge hole_de(hole_edge.get(), false);
  hole_de.setNextMin(&hole_de);

  // shell takes ownership of heap-allocated hole via setShell -> addHole.
  // EdgeRing::isShell() checks whether a containing shell has been assigned.
  auto* hole = new MinimalEdgeRing(&hole_de, gf.get());
  EXPECT_TRUE(hole->isHole());
  EXPECT_TRUE(hole->isShell());
  EXPECT_EQ(hole_de.getMinEdgeRing(), hole);
  EXPECT_EQ(hole->getShell(), nullptr);

  hole->setShell(&shell);
  EXPECT_EQ(hole->getShell(), &shell);
  EXPECT_FALSE(hole->isShell());
  EXPECT_TRUE(shell.isShell());
  EXPECT_TRUE(shell.containsPoint(Coordinate(1.0, 1.0)));
  EXPECT_FALSE(shell.containsPoint(Coordinate(4.0, 4.0)));

  std::unique_ptr<Polygon> poly = shell.toPolygon(gf.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getNumInteriorRing(), 1u);
  // Outer 10x10 (100) minus inner 4x4 (16) = 84.
  EXPECT_DOUBLE_EQ(poly->getArea(), 84.0);
}

TEST(MinimalEdgeRingTest,
     MultiEdgeMinimalRingFollowsNextMinAndMixedDirections) {
  GeometryFactory::Ptr gf = GeometryFactory::create();

  // Four edges forming a 10x10 CW square using alternating forward and backward
  // DirectedEdges, labeled for both parent geometries (non-isolated).
  const Label both_cw(Location::BOUNDARY, Location::EXTERIOR,
                      Location::INTERIOR);
  const Label both_reversed(Location::BOUNDARY, Location::INTERIOR,
                            Location::EXTERIOR);

  auto edge1 = std::make_unique<Edge>(
      new CoordinateSequence{CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 10.0)},
      both_cw);
  auto edge2 = std::make_unique<Edge>(
      new CoordinateSequence{CoordinateXY(10.0, 10.0), CoordinateXY(0.0, 10.0)},
      both_reversed);
  auto edge3 = std::make_unique<Edge>(
      new CoordinateSequence{CoordinateXY(10.0, 10.0), CoordinateXY(10.0, 0.0)},
      both_cw);
  auto edge4 = std::make_unique<Edge>(
      new CoordinateSequence{CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)},
      both_reversed);

  DirectedEdge de1(edge1.get(), true);
  DirectedEdge de2(edge2.get(), false);
  DirectedEdge de3(edge3.get(), true);
  DirectedEdge de4(edge4.get(), false);

  de1.setNextMin(&de2);
  de2.setNextMin(&de3);
  de3.setNextMin(&de4);
  de4.setNextMin(&de1);

  MinimalEdgeRing ring(&de1, gf.get());

  EXPECT_FALSE(ring.isHole());
  EXPECT_TRUE(ring.isShell());
  EXPECT_FALSE(ring.isIsolated());
  EXPECT_EQ(ring.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(ring.getLabel().getLocation(1), Location::INTERIOR);

  const std::vector<DirectedEdge*>& ring_edges = ring.getEdges();
  ASSERT_EQ(ring_edges.size(), 4u);
  EXPECT_EQ(ring_edges[0], &de1);
  EXPECT_EQ(ring_edges[1], &de2);
  EXPECT_EQ(ring_edges[2], &de3);
  EXPECT_EQ(ring_edges[3], &de4);

  for (DirectedEdge* de : ring_edges) {
    EXPECT_EQ(de->getMinEdgeRing(), &ring);
    EXPECT_EQ(de->getEdgeRing(), nullptr);
  }
  EXPECT_EQ(ring.getNext(&de1), &de2);
  EXPECT_EQ(ring.getNext(&de2), &de3);
  EXPECT_EQ(ring.getNext(&de3), &de4);
  EXPECT_EQ(ring.getNext(&de4), &de1);

  std::unique_ptr<Polygon> poly = ring.toPolygon(gf.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getExteriorRing()->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 100.0);
}

TEST(MinimalEdgeRingTest, VirtualSetEdgeRingOverrideUpdatesMinEdgeRing) {
  GeometryFactory::Ptr gf = GeometryFactory::create();

  auto* pts = new CoordinateSequence{
      CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 4.0), CoordinateXY(4.0, 4.0),
      CoordinateXY(4.0, 0.0), CoordinateXY(0.0, 0.0),
  };
  auto edge = std::make_unique<Edge>(
      pts, MakeAreaLabel(Location::EXTERIOR, Location::INTERIOR));
  DirectedEdge de(edge.get(), true);
  de.setNextMin(&de);

  auto* other_pts = new CoordinateSequence{
      CoordinateXY(10.0, 10.0), CoordinateXY(10.0, 12.0),
      CoordinateXY(12.0, 12.0), CoordinateXY(12.0, 10.0),
      CoordinateXY(10.0, 10.0),
  };
  auto other_edge = std::make_unique<Edge>(
      other_pts, MakeAreaLabel(Location::EXTERIOR, Location::INTERIOR));
  DirectedEdge other_de(other_edge.get(), true);
  other_de.setNextMin(&other_de);

  MinimalEdgeRing ring(&de, gf.get());
  EdgeRing* as_base = &ring;
  EXPECT_EQ(as_base->getNext(&de), &de);
  EXPECT_EQ(other_de.getMinEdgeRing(), nullptr);
  as_base->setEdgeRing(&other_de, &ring);
  EXPECT_EQ(other_de.getMinEdgeRing(), &ring);
  EXPECT_EQ(other_de.getEdgeRing(), nullptr);
}

TEST(MinimalEdgeRingTest, ThrowsTopologyExceptionOnNullStartOrNullNextMin) {
  GeometryFactory::Ptr gf = GeometryFactory::create();

  EXPECT_THROW(MinimalEdgeRing(nullptr, gf.get()), TopologyException);

  auto* pts1 = new CoordinateSequence{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(0.0, 5.0),
  };
  auto* pts2 = new CoordinateSequence{
      CoordinateXY(0.0, 5.0),
      CoordinateXY(5.0, 0.0),
  };
  auto edge1 = std::make_unique<Edge>(
      pts1, MakeAreaLabel(Location::EXTERIOR, Location::INTERIOR));
  auto edge2 = std::make_unique<Edge>(
      pts2, MakeAreaLabel(Location::EXTERIOR, Location::INTERIOR));

  DirectedEdge de1(edge1.get(), true);
  DirectedEdge de2(edge2.get(), true);
  de1.setNextMin(&de2);
  de2.setNextMin(nullptr);

  EXPECT_THROW(MinimalEdgeRing(&de1, gf.get()), TopologyException);
}

TEST(MinimalEdgeRingTest, BuiltFromMaximalEdgeRingAtPinchedDegreeFourNode) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  PinchedMaximalRingSetup setup(gf.get());
  ASSERT_NE(setup.start_de, nullptr);
  ASSERT_NE(setup.max_ring, nullptr);
  EXPECT_EQ(setup.max_ring->getMaxNodeDegree(), 4);

  std::vector<MinimalEdgeRing*> min_rings;
  setup.max_ring->buildMinimalRings(min_rings);
  ASSERT_EQ(min_rings.size(), 2u);

  std::unique_ptr<MinimalEdgeRing> first(min_rings[0]);
  std::unique_ptr<MinimalEdgeRing> second(min_rings[1]);

  MinimalEdgeRing* shell = first->isHole() ? second.get() : first.get();
  MinimalEdgeRing* hole = first->isHole() ? first.get() : second.get();
  EXPECT_FALSE(shell->isHole());
  EXPECT_TRUE(hole->isHole());

  // Outer shell has area 100 and quadrilateral hole has area (15 + 15) / 2
  // = 15.
  std::unique_ptr<Polygon> shell_poly = shell->toPolygon(gf.get());
  std::unique_ptr<Polygon> hole_poly = hole->toPolygon(gf.get());
  EXPECT_DOUBLE_EQ(shell_poly->getArea(), 100.0);
  EXPECT_DOUBLE_EQ(hole_poly->getArea(), 15.0);
  EXPECT_EQ(setup.start_de->getMinEdgeRing()->isHole(),
            setup.start_de->getMinEdgeRing() == hole);
}

TEST(MinimalEdgeRingTest, BuiltFromMaximalEdgeRingIntoBaseEdgeRingVector) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  PinchedMaximalRingSetup setup(gf.get());
  ASSERT_NE(setup.max_ring, nullptr);

  std::vector<EdgeRing*> base_rings;
  setup.max_ring->buildMinimalRings(base_rings);
  ASSERT_EQ(base_rings.size(), 2u);

  // Polymorphic deletion through EdgeRing* exercises MinimalEdgeRing's virtual
  // destructor.
  std::unique_ptr<EdgeRing> base_first(base_rings[0]);
  std::unique_ptr<EdgeRing> base_second(base_rings[1]);
  EXPECT_NE(dynamic_cast<MinimalEdgeRing*>(base_first.get()), nullptr);
  EXPECT_NE(dynamic_cast<MinimalEdgeRing*>(base_second.get()), nullptr);
  EXPECT_NE(base_first->isHole(), base_second->isHole());
}

TEST(MinimalEdgeRingTest, BufferOpCreatesPinchedRingWithHole) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  // Pinched U-shape buffered with butt caps touches itself at (2, 2),
  // exercising MinimalEdgeRing splitting in PolygonBuilder during BufferOp.
  std::unique_ptr<Geometry> line =
      reader.read("LINESTRING (2 0, 10 0, 10 10, 0 10, 0 2)");
  std::unique_ptr<Geometry> buffered =
      BufferOp::bufferOp(line.get(), 2.0, 8, BufferOp::CAP_BUTT);
  ASSERT_NE(buffered, nullptr);
  EXPECT_TRUE(buffered->isValid());
  const auto* poly = dynamic_cast<const Polygon*>(buffered.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_GE(poly->getNumInteriorRing(), 1u);
}

TEST(MinimalEdgeRingTest, MinimalEdgeRingPreservesGeometryFactorySrid) {
  PrecisionModel pm;
  GeometryFactory::Ptr gf = GeometryFactory::create(&pm, 4326);

  auto* pts = new CoordinateSequence{
      CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 4.0), CoordinateXY(4.0, 4.0),
      CoordinateXY(4.0, 0.0), CoordinateXY(0.0, 0.0),
  };
  auto edge = std::make_unique<Edge>(
      pts, MakeAreaLabel(Location::EXTERIOR, Location::INTERIOR));
  DirectedEdge de(edge.get(), true);
  de.setNextMin(&de);

  MinimalEdgeRing ring(&de, gf.get());
  ASSERT_NE(ring.getLinearRing(), nullptr);
  EXPECT_EQ(ring.getLinearRing()->getSRID(), 4326);

  std::unique_ptr<Polygon> poly = ring.toPolygon(gf.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getSRID(), 4326);
  EXPECT_DOUBLE_EQ(poly->getArea(), 16.0);
}

TEST(MinimalEdgeRingTest, BuiltFromMaximalEdgeRingForTwoPinchedHoles) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  PinchedMaximalRingSetup setup(gf.get(),
                                CoordinateSequence{
                                    CoordinateXY(0.0, 0.0),
                                    CoordinateXY(2.0, 1.0),
                                    CoordinateXY(1.0, 2.0),
                                    CoordinateXY(0.0, 0.0),
                                },
                                CoordinateSequence{
                                    CoordinateXY(0.0, 0.0),
                                    CoordinateXY(-2.0, -1.0),
                                    CoordinateXY(-1.0, -2.0),
                                    CoordinateXY(0.0, 0.0),
                                });
  ASSERT_NE(setup.max_ring, nullptr);
  EXPECT_EQ(setup.max_ring->getMaxNodeDegree(), 4);
  EXPECT_TRUE(setup.max_ring->isHole());

  std::vector<MinimalEdgeRing*> min_rings;
  setup.max_ring->buildMinimalRings(min_rings);
  ASSERT_EQ(min_rings.size(), 2u);

  std::unique_ptr<MinimalEdgeRing> first(min_rings[0]);
  std::unique_ptr<MinimalEdgeRing> second(min_rings[1]);
  EXPECT_TRUE(first->isHole());
  EXPECT_TRUE(second->isHole());
  EXPECT_DOUBLE_EQ(first->toPolygon(gf.get())->getArea(), 1.5);
  EXPECT_DOUBLE_EQ(second->toPolygon(gf.get())->getArea(), 1.5);
}

}  // namespace
}  // namespace geos_google
