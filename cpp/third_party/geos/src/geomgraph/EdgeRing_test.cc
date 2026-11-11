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

// Unit tests for third_party/geos/src/geomgraph/EdgeRing.cpp.

#include "third_party/geos/include/geos/geomgraph/EdgeRing.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdge.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdgeStar.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::geomgraph::DirectedEdge;
using geos::geomgraph::DirectedEdgeStar;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeRing;
using geos::geomgraph::Label;
using geos::geomgraph::Node;
using geos::util::TopologyException;

class TestEdgeRing final : public EdgeRing {
 public:
  TestEdgeRing(DirectedEdge* start, const GeometryFactory* factory,
               bool compute_now = true)
      : EdgeRing(start, factory) {
    if (compute_now) {
      computePoints(start);
      computeRing();
    }
  }

  DirectedEdge* getNext(DirectedEdge* de) override { return de->getNext(); }

  void setEdgeRing(DirectedEdge* de, EdgeRing* er) override {
    de->setEdgeRing(er);
  }

  using EdgeRing::addPoints;
  using EdgeRing::computePoints;
  using EdgeRing::mergeLabel;
};

class GraphArena {
 public:
  GraphArena() : factory_(GeometryFactory::create()) {}

  const GeometryFactory* factory() const { return factory_.get(); }

  Edge* AddEdge(std::initializer_list<Coordinate> coords, const Label& label) {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    edges_.push_back(std::make_unique<Edge>(seq.release(), label));
    return edges_.back().get();
  }

  DirectedEdge* AddDirectedEdge(Edge* edge, bool is_forward) {
    dir_edges_.push_back(std::make_unique<DirectedEdge>(edge, is_forward));
    return dir_edges_.back().get();
  }

  Node* AddNode(const Coordinate& coord) {
    auto star = std::make_unique<DirectedEdgeStar>();
    nodes_.push_back(std::make_unique<Node>(coord, star.release()));
    return nodes_.back().get();
  }

  DirectedEdge* MakeLeg(std::initializer_list<Coordinate> coords,
                        bool is_forward, const Label& label) {
    Edge* edge = AddEdge(coords, label);
    DirectedEdge* de = AddDirectedEdge(edge, is_forward);
    Node* node = AddNode(de->getCoordinate());
    node->add(de);
    return de;
  }

  static void LinkCycle(const std::vector<DirectedEdge*>& des) {
    for (std::size_t i = 0; i < des.size(); ++i) {
      des[i]->setNext(des[(i + 1) % des.size()]);
    }
  }

 private:
  GeometryFactory::Ptr factory_;
  // Destroy nodes before directed edges, and directed edges before edges.
  std::vector<std::unique_ptr<Edge>> edges_;
  std::vector<std::unique_ptr<DirectedEdge>> dir_edges_;
  std::vector<std::unique_ptr<Node>> nodes_;
};

Label AreaLabel0(Location left = Location::EXTERIOR,
                 Location right = Location::INTERIOR) {
  return Label(0, Location::BOUNDARY, left, right);
}

CoordinateSequence MakeSeq(std::initializer_list<Coordinate> coords) {
  return CoordinateSequence(coords);
}

TestEdgeRing* MakeSquareRingOnHeap(GraphArena& arena, double x0, double y0,
                                   double x1, double y1, bool clockwise) {
  const Label lbl = AreaLabel0();
  DirectedEdge* de0 = nullptr;
  DirectedEdge* de1 = nullptr;
  DirectedEdge* de2 = nullptr;
  DirectedEdge* de3 = nullptr;
  if (clockwise) {
    de0 = arena.MakeLeg({{x0, y0}, {x0, y1}}, true, lbl);
    de1 = arena.MakeLeg({{x0, y1}, {x1, y1}}, true, lbl);
    de2 = arena.MakeLeg({{x1, y1}, {x1, y0}}, true, lbl);
    de3 = arena.MakeLeg({{x1, y0}, {x0, y0}}, true, lbl);
  } else {
    de0 = arena.MakeLeg({{x0, y0}, {x1, y0}}, true, lbl);
    de1 = arena.MakeLeg({{x1, y0}, {x1, y1}}, true, lbl);
    de2 = arena.MakeLeg({{x1, y1}, {x0, y1}}, true, lbl);
    de3 = arena.MakeLeg({{x0, y1}, {x0, y0}}, true, lbl);
  }
  GraphArena::LinkCycle({de0, de1, de2, de3});
  return new TestEdgeRing(de0, arena.factory());
}

TEST(EdgeRingTest, InitialStateAndStreamFormatting) {
  GraphArena arena;
  TestEdgeRing ring(nullptr, arena.factory(), /*compute_now=*/false);

  EXPECT_EQ(ring.getLinearRing(), nullptr);
  EXPECT_TRUE(ring.isShell());
  EXPECT_EQ(ring.getShell(), nullptr);
  EXPECT_TRUE(ring.getEdges().empty());
  EXPECT_TRUE(ring.getLabel().isNull());
  EXPECT_FALSE(ring.isIsolated());

  ring.setShell(nullptr);
  EXPECT_TRUE(ring.isShell());
  EXPECT_EQ(ring.getShell(), nullptr);

  std::ostringstream oss;
  oss << ring;
  EXPECT_NE(oss.str().find("EdgeRing["), std::string::npos);
  EXPECT_NE(oss.str().find("]: "), std::string::npos);
}

TEST(EdgeRingTest, ComputePointsForwardAndBackwardMultiVertexEdges) {
  GraphArena arena;
  const Label lbl = AreaLabel0();

  // CW square: starts forward, continues forward, closes with a backward edge.
  DirectedEdge* f0 =
      arena.MakeLeg({{0.0, 0.0}, {0.0, 5.0}, {0.0, 10.0}}, true, lbl);
  DirectedEdge* f1 =
      arena.MakeLeg({{0.0, 10.0}, {5.0, 10.0}, {10.0, 10.0}}, true, lbl);
  DirectedEdge* f2 =
      arena.MakeLeg({{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}}, false,
                    AreaLabel0(Location::INTERIOR, Location::EXTERIOR));
  GraphArena::LinkCycle({f0, f1, f2});

  TestEdgeRing cw_ring(f0, arena.factory());
  const std::vector<DirectedEdge*>& cw_edges = cw_ring.getEdges();
  ASSERT_EQ(cw_edges.size(), 3u);
  EXPECT_EQ(cw_edges[0], f0);
  EXPECT_EQ(cw_edges[1], f1);
  EXPECT_EQ(cw_edges[2], f2);
  EXPECT_EQ(f0->getEdgeRing(), &cw_ring);
  EXPECT_EQ(f1->getEdgeRing(), &cw_ring);
  EXPECT_EQ(f2->getEdgeRing(), &cw_ring);

  LinearRing* cw_lr = cw_ring.getLinearRing();
  ASSERT_NE(cw_lr, nullptr);
  EXPECT_FALSE(cw_ring.isHole());
  const CoordinateSequence* cw_pts = cw_lr->getCoordinatesRO();
  const CoordinateSequence expected_cw = MakeSeq({
      {0.0, 0.0},
      {0.0, 5.0},
      {0.0, 10.0},
      {5.0, 10.0},
      {10.0, 10.0},
      {10.0, 0.0},
      {0.0, 0.0},
  });
  EXPECT_TRUE(CoordinateSequence::equals(cw_pts, &expected_cw));
  EXPECT_EQ(cw_ring.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_TRUE(cw_ring.isIsolated());

  // Calling computeRing() again must be a no-op and retain the same ring.
  cw_ring.computeRing();
  EXPECT_EQ(cw_ring.getLinearRing(), cw_lr);

  // CCW square: starts backward, continues backward, closes forward.
  DirectedEdge* b0 =
      arena.MakeLeg({{10.0, 0.0}, {5.0, 0.0}, {0.0, 0.0}}, false,
                    AreaLabel0(Location::INTERIOR, Location::EXTERIOR));
  DirectedEdge* b1 =
      arena.MakeLeg({{10.0, 10.0}, {10.0, 5.0}, {10.0, 0.0}}, false,
                    AreaLabel0(Location::INTERIOR, Location::EXTERIOR));
  DirectedEdge* b2 =
      arena.MakeLeg({{10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}, true, lbl);
  GraphArena::LinkCycle({b0, b1, b2});

  TestEdgeRing ccw_ring(b0, arena.factory());
  LinearRing* ccw_lr = ccw_ring.getLinearRing();
  ASSERT_NE(ccw_lr, nullptr);
  EXPECT_TRUE(ccw_ring.isHole());
  const CoordinateSequence* ccw_pts = ccw_lr->getCoordinatesRO();
  const CoordinateSequence expected_ccw = MakeSeq({
      {0.0, 0.0},
      {5.0, 0.0},
      {10.0, 0.0},
      {10.0, 5.0},
      {10.0, 10.0},
      {0.0, 10.0},
      {0.0, 0.0},
  });
  EXPECT_TRUE(CoordinateSequence::equals(ccw_pts, &expected_ccw));
}

TEST(EdgeRingTest, AddPointsDirectCoversAllDirectionAndFirstEdgeModes) {
  GraphArena arena;
  const Label lbl = AreaLabel0();

  Edge* e0 = arena.AddEdge({{0.0, 0.0}, {0.0, 2.0}, {0.0, 4.0}}, lbl);
  Edge* e1 = arena.AddEdge({{0.0, 4.0}, {2.0, 4.0}, {4.0, 4.0}}, lbl);
  Edge* e2 = arena.AddEdge({{4.0, 0.0}, {4.0, 2.0}, {4.0, 4.0}}, lbl);
  Edge* e3 = arena.AddEdge({{4.0, 0.0}, {2.0, 0.0}, {0.0, 0.0}}, lbl);

  TestEdgeRing ring_fwd_start(nullptr, arena.factory(), /*compute_now=*/false);
  ring_fwd_start.addPoints(e0, /*isForward=*/true, /*isFirstEdge=*/true);
  ring_fwd_start.addPoints(e1, /*isForward=*/true, /*isFirstEdge=*/false);
  ring_fwd_start.addPoints(e2, /*isForward=*/false, /*isFirstEdge=*/false);
  ring_fwd_start.addPoints(e3, /*isForward=*/true, /*isFirstEdge=*/false);
  ring_fwd_start.computeRing();

  EXPECT_FALSE(ring_fwd_start.isHole());
  const CoordinateSequence expected_fwd = MakeSeq({
      {0.0, 0.0},
      {0.0, 2.0},
      {0.0, 4.0},
      {2.0, 4.0},
      {4.0, 4.0},
      {4.0, 2.0},
      {4.0, 0.0},
      {2.0, 0.0},
      {0.0, 0.0},
  });
  EXPECT_TRUE(CoordinateSequence::equals(
      ring_fwd_start.getLinearRing()->getCoordinatesRO(), &expected_fwd));

  Edge* r0 = arena.AddEdge({{4.0, 0.0}, {2.0, 0.0}, {0.0, 0.0}}, lbl);
  Edge* r1 = arena.AddEdge({{4.0, 4.0}, {4.0, 2.0}, {4.0, 0.0}}, lbl);
  Edge* r2 = arena.AddEdge({{4.0, 4.0}, {0.0, 4.0}, {0.0, 0.0}}, lbl);
  TestEdgeRing ring_bwd_start(nullptr, arena.factory(), /*compute_now=*/false);
  ring_bwd_start.addPoints(r0, /*isForward=*/false, /*isFirstEdge=*/true);
  ring_bwd_start.addPoints(r1, /*isForward=*/false, /*isFirstEdge=*/false);
  ring_bwd_start.addPoints(r2, /*isForward=*/true, /*isFirstEdge=*/false);
  ring_bwd_start.computeRing();

  EXPECT_TRUE(ring_bwd_start.isHole());
  const CoordinateSequence expected_bwd = MakeSeq({
      {0.0, 0.0},
      {2.0, 0.0},
      {4.0, 0.0},
      {4.0, 2.0},
      {4.0, 4.0},
      {0.0, 4.0},
      {0.0, 0.0},
  });
  EXPECT_TRUE(CoordinateSequence::equals(
      ring_bwd_start.getLinearRing()->getCoordinatesRO(), &expected_bwd));
}

TEST(EdgeRingTest, ComputePointsThrowsOnNullOrRevisitedDirectedEdge) {
  GraphArena arena;
  const Label lbl = AreaLabel0();

  TestEdgeRing null_ring(nullptr, arena.factory(), /*compute_now=*/false);
  EXPECT_THROW(null_ring.computePoints(nullptr), TopologyException);

  DirectedEdge* b0 = arena.MakeLeg({{0.0, 0.0}, {0.0, 2.0}}, true, lbl);
  DirectedEdge* b1 = arena.MakeLeg({{0.0, 2.0}, {2.0, 2.0}}, true, lbl);
  b0->setNext(b1);
  b1->setNext(nullptr);
  TestEdgeRing broken_ring(b0, arena.factory(), /*compute_now=*/false);
  EXPECT_THROW(broken_ring.computePoints(b0), TopologyException);

  DirectedEdge* l0 = arena.MakeLeg({{0.0, 0.0}, {0.0, 2.0}}, true, lbl);
  DirectedEdge* l1 = arena.MakeLeg({{0.0, 2.0}, {2.0, 2.0}}, true, lbl);
  DirectedEdge* l2 = arena.MakeLeg({{2.0, 2.0}, {2.0, 0.0}}, true, lbl);
  l0->setNext(l1);
  l1->setNext(l2);
  l2->setNext(l1);
  TestEdgeRing lasso_ring(l0, arena.factory(), /*compute_now=*/false);
  try {
    lasso_ring.computePoints(l0);
    FAIL() << "Expected TopologyException when visiting DirectedEdge twice";
  } catch (TopologyException& ex) {
    EXPECT_TRUE(ex.getCoordinate().equals2D(Coordinate(0.0, 2.0)));
  }
}

TEST(EdgeRingTest, MergeLabelAndIsIsolated) {
  GraphArena arena;
  TestEdgeRing ring(nullptr, arena.factory(), /*compute_now=*/false);

  EXPECT_FALSE(ring.isIsolated());

  // Merge label with geom 0 RHS = INTERIOR and geom 1 = NONE.
  ring.mergeLabel(
      Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR));
  EXPECT_EQ(ring.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(ring.getLabel().getLocation(1), Location::NONE);
  EXPECT_TRUE(ring.isIsolated());

  // Existing non-NONE location for geom 0 must not be overwritten.
  ring.mergeLabel(
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  EXPECT_EQ(ring.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_TRUE(ring.isIsolated());

  // Merge geom 1 RHS = EXTERIOR; now both geometry slots are populated.
  ring.mergeLabel(
      Label(1, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  EXPECT_EQ(ring.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(ring.getLabel().getLocation(1), Location::EXTERIOR);
  EXPECT_FALSE(ring.isIsolated());

  // Merging a NONE RHS for geomIndex 1 leaves geom 1 unchanged.
  ring.mergeLabel(
      Label(1, Location::BOUNDARY, Location::INTERIOR, Location::NONE), 1);
  EXPECT_EQ(ring.getLabel().getLocation(1), Location::EXTERIOR);
}

TEST(EdgeRingTest, ShellHoleRelationshipAndToPolygon) {
  GraphArena arena;
  std::unique_ptr<TestEdgeRing> shell(
      MakeSquareRingOnHeap(arena, 0.0, 0.0, 10.0, 10.0, /*clockwise=*/true));

  EXPECT_TRUE(shell->isShell());
  EXPECT_EQ(shell->getShell(), nullptr);
  EXPECT_FALSE(shell->isHole());

  std::unique_ptr<Polygon> poly_without_holes =
      shell->toPolygon(arena.factory());
  ASSERT_NE(poly_without_holes, nullptr);
  EXPECT_EQ(poly_without_holes->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(poly_without_holes->getArea(), 100.0);

  // Shell takes ownership of holes via setShell() -> addHole().
  TestEdgeRing* hole1 =
      MakeSquareRingOnHeap(arena, 1.0, 1.0, 3.0, 3.0, /*clockwise=*/false);
  TestEdgeRing* hole2 =
      MakeSquareRingOnHeap(arena, 5.0, 5.0, 8.0, 8.0, /*clockwise=*/false);
  hole1->setShell(shell.get());
  hole2->setShell(shell.get());

  EXPECT_FALSE(hole1->isShell());
  EXPECT_EQ(hole1->getShell(), shell.get());
  EXPECT_TRUE(hole1->isHole());
  EXPECT_FALSE(hole2->isShell());
  EXPECT_EQ(hole2->getShell(), shell.get());
  EXPECT_TRUE(hole2->isHole());
  EXPECT_TRUE(shell->isShell());

  std::unique_ptr<Polygon> poly_with_holes = shell->toPolygon(arena.factory());
  ASSERT_NE(poly_with_holes, nullptr);
  EXPECT_EQ(poly_with_holes->getNumInteriorRing(), 2u);
  EXPECT_DOUBLE_EQ(poly_with_holes->getArea(), 87.0);
  EXPECT_TRUE(CoordinateSequence::equals(
      poly_with_holes->getExteriorRing()->getCoordinatesRO(),
      shell->getLinearRing()->getCoordinatesRO()));
  EXPECT_TRUE(CoordinateSequence::equals(
      poly_with_holes->getInteriorRingN(0)->getCoordinatesRO(),
      hole1->getLinearRing()->getCoordinatesRO()));
  EXPECT_TRUE(CoordinateSequence::equals(
      poly_with_holes->getInteriorRingN(1)->getCoordinatesRO(),
      hole2->getLinearRing()->getCoordinatesRO()));
}

TEST(EdgeRingTest, ContainsPointHandlesEnvelopeRingAndMultipleHoles) {
  GraphArena arena;
  const Label lbl = AreaLabel0();

  // CW L-shaped shell occupying [0,10]x[0,10] minus upper-right [5,10]x[5,10].
  DirectedEdge* s0 = arena.MakeLeg({{0.0, 0.0}, {0.0, 10.0}}, true, lbl);
  DirectedEdge* s1 = arena.MakeLeg({{0.0, 10.0}, {5.0, 10.0}}, true, lbl);
  DirectedEdge* s2 = arena.MakeLeg({{5.0, 10.0}, {5.0, 5.0}}, true, lbl);
  DirectedEdge* s3 = arena.MakeLeg({{5.0, 5.0}, {10.0, 5.0}}, true, lbl);
  DirectedEdge* s4 = arena.MakeLeg({{10.0, 5.0}, {10.0, 0.0}}, true, lbl);
  DirectedEdge* s5 = arena.MakeLeg({{10.0, 0.0}, {0.0, 0.0}}, true, lbl);
  GraphArena::LinkCycle({s0, s1, s2, s3, s4, s5});
  TestEdgeRing shell(s0, arena.factory());

  TestEdgeRing* hole1 =
      MakeSquareRingOnHeap(arena, 1.0, 1.0, 3.0, 3.0, /*clockwise=*/false);
  TestEdgeRing* hole2 =
      MakeSquareRingOnHeap(arena, 1.0, 6.0, 3.0, 8.0, /*clockwise=*/false);
  hole1->setShell(&shell);
  hole2->setShell(&shell);

  // Outside envelope.
  EXPECT_FALSE(shell.containsPoint(Coordinate(-1.0, 5.0)));
  EXPECT_FALSE(shell.containsPoint(Coordinate(11.0, 5.0)));
  EXPECT_FALSE(shell.containsPoint(Coordinate(5.0, -1.0)));
  EXPECT_FALSE(shell.containsPoint(Coordinate(5.0, 11.0)));

  // Inside envelope, outside the L-shaped ring.
  EXPECT_FALSE(shell.containsPoint(Coordinate(7.5, 7.5)));

  // Inside holes or on hole boundary.
  EXPECT_FALSE(shell.containsPoint(Coordinate(2.0, 2.0)));
  EXPECT_FALSE(shell.containsPoint(Coordinate(1.0, 2.0)));
  EXPECT_FALSE(shell.containsPoint(Coordinate(2.0, 7.0)));
  EXPECT_TRUE(hole1->containsPoint(Coordinate(2.0, 2.0)));
  EXPECT_FALSE(hole1->containsPoint(Coordinate(4.0, 4.0)));

  // Inside shell and outside all holes, plus on outer shell boundary.
  EXPECT_TRUE(shell.containsPoint(Coordinate(4.0, 4.0)));
  EXPECT_TRUE(shell.containsPoint(Coordinate(8.0, 2.0)));
  EXPECT_TRUE(shell.containsPoint(Coordinate(2.0, 9.0)));
  EXPECT_TRUE(shell.containsPoint(Coordinate(0.0, 5.0)));
}

TEST(EdgeRingTest, GetMaxNodeDegreeForSimpleAndPinchedRings) {
  GraphArena arena;
  const Label lbl = AreaLabel0();

  // Simple triangle ring: every node has outgoing degree 1 => max degree 2.
  DirectedEdge* t0 = arena.MakeLeg({{0.0, 0.0}, {0.0, 3.0}}, true, lbl);
  DirectedEdge* t1 = arena.MakeLeg({{0.0, 3.0}, {4.0, 0.0}}, true, lbl);
  DirectedEdge* t2 = arena.MakeLeg({{4.0, 0.0}, {0.0, 0.0}}, true, lbl);
  GraphArena::LinkCycle({t0, t1, t2});
  TestEdgeRing simple_ring(t0, arena.factory());
  EXPECT_EQ(simple_ring.getMaxNodeDegree(), 2);
  EXPECT_EQ(simple_ring.getMaxNodeDegree(), 2);

  // Pinched figure-8 ring sharing node (2, 2) with 2 outgoing ring edges.
  Node* pinch = arena.AddNode(Coordinate(2.0, 2.0));
  Node* left_top = arena.AddNode(Coordinate(0.0, 4.0));
  Node* right_bot = arena.AddNode(Coordinate(4.0, 1.0));

  Edge* e0 = arena.AddEdge({{2.0, 2.0}, {0.0, 0.0}, {0.0, 4.0}}, lbl);
  Edge* e1 = arena.AddEdge({{0.0, 4.0}, {2.0, 2.0}}, lbl);
  Edge* e2 = arena.AddEdge({{2.0, 2.0}, {4.0, 3.0}, {4.0, 1.0}}, lbl);
  Edge* e3 = arena.AddEdge({{4.0, 1.0}, {2.0, 2.0}}, lbl);
  Edge* e_unrelated = arena.AddEdge({{2.0, 2.0}, {2.0, 5.0}}, lbl);

  DirectedEdge* d0 = arena.AddDirectedEdge(e0, true);
  DirectedEdge* d1 = arena.AddDirectedEdge(e1, true);
  DirectedEdge* d2 = arena.AddDirectedEdge(e2, true);
  DirectedEdge* d3 = arena.AddDirectedEdge(e3, true);
  DirectedEdge* d_unrelated = arena.AddDirectedEdge(e_unrelated, true);

  pinch->add(d0);
  pinch->add(d2);
  pinch->add(d_unrelated);
  left_top->add(d1);
  right_bot->add(d3);

  // Start at degree-1 node d1 so maxNodeDegree increases and also skips lower.
  GraphArena::LinkCycle({d1, d2, d3, d0});
  TestEdgeRing pinched_ring(d1, arena.factory());
  EXPECT_EQ(pinched_ring.getMaxNodeDegree(), 4);
  EXPECT_EQ(pinched_ring.getMaxNodeDegree(), 4);
}

TEST(EdgeRingTest, SetInResultMarksEveryEdgeInRing) {
  GraphArena arena;
  const Label lbl = AreaLabel0();

  Edge* e0 = arena.AddEdge({{0.0, 0.0}, {0.0, 2.0}}, lbl);
  Edge* e1 = arena.AddEdge({{0.0, 2.0}, {2.0, 0.0}}, lbl);
  Edge* e2 = arena.AddEdge({{2.0, 0.0}, {0.0, 0.0}}, lbl);
  Edge* e_unused = arena.AddEdge({{10.0, 10.0}, {12.0, 12.0}}, lbl);

  DirectedEdge* d0 = arena.AddDirectedEdge(e0, true);
  DirectedEdge* d1 = arena.AddDirectedEdge(e1, true);
  DirectedEdge* d2 = arena.AddDirectedEdge(e2, true);
  GraphArena::LinkCycle({d0, d1, d2});

  TestEdgeRing ring(d0, arena.factory());
  EXPECT_FALSE(e0->isInResult());
  EXPECT_FALSE(e1->isInResult());
  EXPECT_FALSE(e2->isInResult());
  EXPECT_FALSE(e_unused->isInResult());

  ring.setInResult();
  EXPECT_TRUE(e0->isInResult());
  EXPECT_TRUE(e1->isInResult());
  EXPECT_TRUE(e2->isInResult());
  EXPECT_FALSE(e_unused->isInResult());
}

}  // namespace
}  // namespace geos_google
