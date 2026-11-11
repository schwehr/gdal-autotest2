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

#include "third_party/geos/include/geos/operation/overlayng/OverlayLabeller.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/Edge.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeNodingBuilder.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/operation/overlayng/InputGeometry.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayEdge.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayGraph.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayLabel.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"
#include "third_party/geos/include/geos/util/AssertionFailedException.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Position;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeNodingBuilder;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::operation::overlayng::InputGeometry;
using geos::operation::overlayng::OverlayEdge;
using geos::operation::overlayng::OverlayGraph;
using geos::operation::overlayng::OverlayLabel;
using geos::operation::overlayng::OverlayLabeller;
using geos::operation::overlayng::OverlayNG;
using geos::util::AssertionFailedException;
using geos::util::TopologyException;

class OverlayLabellerTest : public ::testing::Test {
 protected:
  OverlayLabellerTest() : gf_(GeometryFactory::create()), reader_(gf_.get()) {}

  std::unique_ptr<Geometry> readWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  static void buildGraph(const Geometry* g0, const Geometry* g1,
                         const PrecisionModel* pm, OverlayGraph& graph,
                         InputGeometry& inputGeom) {
    EdgeNodingBuilder builder(pm, nullptr);
    std::vector<Edge*> edges = builder.build(g0, g1);
    inputGeom.setCollapsed(0, !builder.hasEdgesFor(0));
    inputGeom.setCollapsed(1, !builder.hasEdgesFor(1));
    for (Edge* e : edges) {
      graph.addEdge(e);
    }
  }

  static Edge makeEdge(double x0, double y0, double x1, double y1,
                       const EdgeSourceInfo& info) {
    auto cs = std::make_unique<CoordinateSequence>(0u, false, false);
    cs->add(x0, y0);
    cs->add(x1, y1);
    return Edge(std::move(cs), &info);
  }

  static Edge makeMergedEdge(double x0, double y0, double x1, double y1,
                             const EdgeSourceInfo& info0,
                             const EdgeSourceInfo& info1) {
    Edge e0 = makeEdge(x0, y0, x1, y1, info0);
    Edge e1 = makeEdge(x0, y0, x1, y1, info1);
    e0.merge(&e1);
    return e0;
  }

  static OverlayEdge* findEdge(OverlayGraph& graph, const CoordinateXY& orig,
                               const CoordinateXY& dest) {
    for (OverlayEdge* e : graph.getEdges()) {
      if (e->equals(orig, dest)) {
        return e;
      }
    }
    return nullptr;
  }

  GeometryFactory::Ptr gf_;
  WKTReader reader_;
};

TEST_F(OverlayLabellerTest, EmptyGraphLabellingAndMarking) {
  auto emptyPoly = readWkt("POLYGON EMPTY");
  InputGeometry inputGeom(emptyPoly.get(), nullptr);
  OverlayGraph graph;
  OverlayLabeller labeller(&graph, &inputGeom);

  labeller.computeLabelling();
  labeller.markResultAreaEdges(+OverlayNG::UNION);
  labeller.unmarkDuplicateEdgesFromResultArea();

  EXPECT_TRUE(graph.getEdges().empty());
  EXPECT_TRUE(graph.getResultAreaEdges().empty());

  auto emptyPolyB = readWkt("POLYGON EMPTY");
  PrecisionModel pm;
  InputGeometry emptyPair(emptyPoly.get(), emptyPolyB.get());
  OverlayGraph pairGraph;
  buildGraph(emptyPoly.get(), emptyPolyB.get(), &pm, pairGraph, emptyPair);
  OverlayLabeller pairLabeller(&pairGraph, &emptyPair);
  pairLabeller.computeLabelling();
  pairLabeller.markResultAreaEdges(+OverlayNG::INTERSECTION);
  pairLabeller.unmarkDuplicateEdgesFromResultArea();

  EXPECT_TRUE(pairGraph.getEdges().empty());
  EXPECT_TRUE(pairGraph.getResultAreaEdges().empty());
}

TEST_F(OverlayLabellerTest, SinglePolygonBoundaryAndAllOpCodes) {
  auto poly = readWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  PrecisionModel pm;

  const struct {
    int opCode;
    std::size_t expectedResultAreaEdges;
  } kCases[] = {
      {+OverlayNG::UNION, 1},
      {+OverlayNG::DIFFERENCE, 1},
      {+OverlayNG::SYMDIFFERENCE, 1},
      {+OverlayNG::INTERSECTION, 0},
      {999, 0},
  };

  for (const auto& tc : kCases) {
    OverlayGraph graph;
    InputGeometry inputGeom(poly.get(), nullptr);
    buildGraph(poly.get(), nullptr, &pm, graph, inputGeom);
    ASSERT_EQ(graph.getEdges().size(), 2u);

    OverlayLabeller labeller(&graph, &inputGeom);
    labeller.computeLabelling();

    for (OverlayEdge* e : graph.getEdges()) {
      const OverlayLabel* lbl = e->getLabel();
      EXPECT_TRUE(lbl->isBoundary(0));
      EXPECT_TRUE(lbl->isNotPart(1));
      EXPECT_EQ(lbl->getLineLocation(1), Location::EXTERIOR);
      EXPECT_EQ(lbl->getLocation(1, Position::LEFT, e->isForward()),
                Location::EXTERIOR);
      EXPECT_EQ(lbl->getLocation(1, Position::RIGHT, e->isForward()),
                Location::EXTERIOR);
    }

    labeller.markResultAreaEdges(tc.opCode);
    labeller.unmarkDuplicateEdgesFromResultArea();
    EXPECT_EQ(graph.getResultAreaEdges().size(), tc.expectedResultAreaEdges);
  }
}

TEST_F(OverlayLabellerTest, PolygonWithPointSecondInputHasNoEdgesForB) {
  auto polyA = readWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto ptB = readWkt("POINT (5 5)");
  PrecisionModel pm;

  OverlayGraph graph;
  InputGeometry inputGeom(polyA.get(), ptB.get());
  buildGraph(polyA.get(), ptB.get(), &pm, graph, inputGeom);
  ASSERT_FALSE(inputGeom.hasEdges(1));

  OverlayLabeller labeller(&graph, &inputGeom);
  labeller.computeLabelling();

  for (OverlayEdge* e : graph.getEdges()) {
    EXPECT_TRUE(e->getLabel()->isBoundary(0));
    EXPECT_EQ(e->getLabel()->getLineLocation(1), Location::EXTERIOR);
  }
}

TEST_F(OverlayLabellerTest, PolygonWithHoleAndCrossingAndDisconnectedLines) {
  auto polyA = readWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(5 5, 15 5, 15 15, 5 15, 5 5))");
  // First linestring crosses shell and hole; second pair forms a degree-2
  // interior node with no boundary edges; third is inside hole; fourth is
  // outside shell.
  auto lineB = readWkt(
      "MULTILINESTRING ("
      "(-5 10, 25 10), "
      "(2 2, 2.5 2.5), (2.5 2.5, 3 3), "
      "(7 7, 13 9), "
      "(30 0, 35 0))");
  PrecisionModel pm;

  OverlayGraph graph;
  InputGeometry inputGeom(polyA.get(), lineB.get());
  buildGraph(polyA.get(), lineB.get(), &pm, graph, inputGeom);

  OverlayLabeller labeller(&graph, &inputGeom);
  labeller.computeLabelling();

  // Line segments crossing shell and hole labeled by propagateAreaLocations.
  struct ExpectedSegment {
    CoordinateXY orig;
    CoordinateXY dest;
    Location locA;
  };
  const std::vector<ExpectedSegment> expected = {
      {{-5, 10}, {0, 10}, Location::EXTERIOR},
      {{0, 10}, {5, 10}, Location::INTERIOR},
      {{5, 10}, {15, 10}, Location::EXTERIOR},
      {{15, 10}, {20, 10}, Location::INTERIOR},
      {{20, 10}, {25, 10}, Location::EXTERIOR},
      // Disconnected interior node (2.5, 2.5) labeled via locateEdgeBothEnds.
      {{2, 2}, {2.5, 2.5}, Location::INTERIOR},
      {{2.5, 2.5}, {3, 3}, Location::INTERIOR},
      // Disconnected segment inside the hole.
      {{7, 7}, {13, 9}, Location::EXTERIOR},
      // Disconnected segment outside the shell.
      {{30, 0}, {35, 0}, Location::EXTERIOR},
  };

  for (const auto& seg : expected) {
    SCOPED_TRACE(::testing::Message()
                 << "(" << seg.orig.x << "," << seg.orig.y << ")->("
                 << seg.dest.x << "," << seg.dest.y << ")");
    // Hole boundary chains also start at (5,10) and end at (15,10), so match
    // the 2-point linear edge belonging to geometry 1.
    OverlayEdge* e = nullptr;
    for (OverlayEdge* candidate : graph.getEdges()) {
      if (candidate->equals(seg.orig, seg.dest) &&
          candidate->getLabel()->isLine(1)) {
        e = candidate;
        break;
      }
    }
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->getLabel()->getLineLocation(0), seg.locA);
    EXPECT_EQ(e->getLabel()->getLineLocation(1), Location::EXTERIOR);
  }
}

TEST_F(OverlayLabellerTest, FindPropagationStartEdgeSkipsLeadingNonBoundary) {
  auto lineA = readWkt("LINESTRING (0 5, 5 5)");
  auto polyB = readWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  PrecisionModel pm;

  // Insert Line A before Polygon B so nodeMap[(0, 5)] holds the Line A edge
  // first, forcing findPropagationStartEdge(..., 1) to advance via oNext().
  OverlayGraph graph;
  InputGeometry inputGeom(lineA.get(), polyB.get());
  buildGraph(lineA.get(), polyB.get(), &pm, graph, inputGeom);

  OverlayEdge* nodeEdge = graph.getNodeEdge(Coordinate(0, 5));
  ASSERT_NE(nodeEdge, nullptr);
  EXPECT_FALSE(nodeEdge->getLabel()->isBoundary(1));

  OverlayLabeller labeller(&graph, &inputGeom);
  labeller.computeLabelling();

  OverlayEdge* seg = findEdge(graph, {0, 5}, {5, 5});
  ASSERT_NE(seg, nullptr);
  EXPECT_EQ(seg->getLabel()->getLineLocation(1), Location::INTERIOR);
  EXPECT_EQ(seg->getLabel()->getLineLocation(0), Location::EXTERIOR);
}

TEST_F(OverlayLabellerTest,
       CollapsedShellAndHoleAndConnectedLinesGeom0AndGeom1) {
  // Fixed precision model with scale 1.0 snaps thin rings to collapsed edges.
  PrecisionModel pm(1.0);
  auto polyWithCollapsedRings = readWkt(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0), (2 2.1, 8 2.1, 8 2.3, 2 2.3, 2 2.1)), "
      "((20 0.1, 25 0.1, 25 0.3, 20 0.3, 20 0.1)))");
  auto lineTouchingCollapsed =
      readWkt("MULTILINESTRING ((4 2, 4 5), (22 0, 22 5))");

  // Collapsed area as geom 0, lines as geom 1.
  {
    OverlayGraph graph;
    InputGeometry inputGeom(polyWithCollapsedRings.get(),
                            lineTouchingCollapsed.get());
    buildGraph(polyWithCollapsedRings.get(), lineTouchingCollapsed.get(), &pm,
               graph, inputGeom);

    OverlayLabeller labeller(&graph, &inputGeom);
    labeller.computeLabelling();

    // Collapsed hole segments get Location::INTERIOR (isHole == true) and
    // propagate INTERIOR to connected line (4, 2)-(4, 5).
    OverlayEdge* holeSeg = findEdge(graph, {2, 2}, {4, 2});
    ASSERT_NE(holeSeg, nullptr);
    EXPECT_TRUE(holeSeg->getLabel()->isCollapse(0));
    EXPECT_TRUE(holeSeg->getLabel()->isHole(0));
    EXPECT_EQ(holeSeg->getLabel()->getLineLocation(0), Location::INTERIOR);

    OverlayEdge* lineFromHole = findEdge(graph, {4, 2}, {4, 5});
    ASSERT_NE(lineFromHole, nullptr);
    EXPECT_EQ(lineFromHole->getLabel()->getLineLocation(0), Location::INTERIOR);

    // Collapsed shell segments get Location::EXTERIOR (isHole == false) and
    // propagate EXTERIOR to connected line (22, 0)-(22, 5).
    OverlayEdge* shellSeg = findEdge(graph, {20, 0}, {22, 0});
    ASSERT_NE(shellSeg, nullptr);
    EXPECT_TRUE(shellSeg->getLabel()->isCollapse(0));
    EXPECT_FALSE(shellSeg->getLabel()->isHole(0));
    EXPECT_EQ(shellSeg->getLabel()->getLineLocation(0), Location::EXTERIOR);

    OverlayEdge* lineFromShell = findEdge(graph, {22, 0}, {22, 5});
    ASSERT_NE(lineFromShell, nullptr);
    EXPECT_EQ(lineFromShell->getLabel()->getLineLocation(0),
              Location::EXTERIOR);
  }

  // Swap roles: lines as geom 0, collapsed area as geom 1.
  {
    OverlayGraph graph;
    InputGeometry inputGeom(lineTouchingCollapsed.get(),
                            polyWithCollapsedRings.get());
    buildGraph(lineTouchingCollapsed.get(), polyWithCollapsedRings.get(), &pm,
               graph, inputGeom);

    OverlayLabeller labeller(&graph, &inputGeom);
    labeller.computeLabelling();

    OverlayEdge* holeSeg = findEdge(graph, {2, 2}, {4, 2});
    ASSERT_NE(holeSeg, nullptr);
    EXPECT_TRUE(holeSeg->getLabel()->isCollapse(1));
    EXPECT_EQ(holeSeg->getLabel()->getLineLocation(1), Location::INTERIOR);

    OverlayEdge* lineFromHole = findEdge(graph, {4, 2}, {4, 5});
    ASSERT_NE(lineFromHole, nullptr);
    EXPECT_EQ(lineFromHole->getLabel()->getLineLocation(1), Location::INTERIOR);

    OverlayEdge* shellSeg = findEdge(graph, {20, 0}, {22, 0});
    ASSERT_NE(shellSeg, nullptr);
    EXPECT_TRUE(shellSeg->getLabel()->isCollapse(1));
    EXPECT_EQ(shellSeg->getLabel()->getLineLocation(1), Location::EXTERIOR);

    OverlayEdge* lineFromShell = findEdge(graph, {22, 0}, {22, 5});
    ASSERT_NE(lineFromShell, nullptr);
    EXPECT_EQ(lineFromShell->getLabel()->getLineLocation(1),
              Location::EXTERIOR);
  }
}

TEST_F(OverlayLabellerTest,
       NarrowGoreCollapseGetsInteriorFromBoundaryBeforeCollapsePass) {
  PrecisionModel pm(1.0);
  // Under scale 1.0, (5.2,10)-(5.0,4)-(4.8,10) snaps to a collapsed shell edge
  // (5,10)-(5,4) inside polyGore [0,10]x[0,10]. The adjacent top polygon
  // ensures (5,10) is noded on the top boundary. Even though the gore's ring
  // role is shell (isHole=false), propagateAreaLocations at (5,10) must label
  // it INTERIOR for geom 0 before labelCollapsedEdges runs.
  auto polyGore =
      readWkt("POLYGON ((0 0, 10 0, 10 10, 5.2 10, 5.0 4, 4.8 10, 0 10, 0 0))");
  auto topPoly = readWkt("POLYGON ((0 10, 10 10, 10 20, 0 20, 0 10))");
  OverlayGraph graph;
  InputGeometry inputGeom(polyGore.get(), topPoly.get());
  buildGraph(polyGore.get(), topPoly.get(), &pm, graph, inputGeom);

  OverlayLabeller labeller(&graph, &inputGeom);
  labeller.computeLabelling();

  OverlayEdge* goreEdge = findEdge(graph, {5, 10}, {5, 4});
  ASSERT_NE(goreEdge, nullptr);
  EXPECT_TRUE(goreEdge->getLabel()->isCollapse(0));
  EXPECT_FALSE(goreEdge->getLabel()->isHole(0));
  EXPECT_EQ(goreEdge->getLabel()->getLineLocation(0), Location::INTERIOR);
  EXPECT_EQ(goreEdge->getLabel()->getLineLocation(1), Location::EXTERIOR);
}

TEST_F(OverlayLabellerTest,
       MultiHopLinearPropagationThroughCollapsedAndNotPartEdges) {
  auto polyA = readWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto lineB = readWkt("LINESTRING (20 0, 20 5)");
  InputGeometry inputGeom(polyA.get(), lineB.get());

  OverlayGraph graph;
  // Boundary edges of A meeting at (5, 10) with interior on y < 10.
  Edge bLeft = makeEdge(0, 10, 5, 10, EdgeSourceInfo(0, 1, false));
  Edge bRight = makeEdge(5, 10, 10, 10, EdgeSourceInfo(0, 1, false));
  // Chain of 3 connected edges entering the interior from (5, 10):
  // (5,10)-(5,7) [collapse of A] -> (5,7)-(5,4) [collapse of A] ->
  // (5,4)-(8,4) [NOT_PART of A, LINE of B].
  Edge c1 = makeEdge(5, 10, 5, 7, EdgeSourceInfo(0, 0, false));
  Edge c2 = makeEdge(5, 7, 5, 4, EdgeSourceInfo(0, 0, false));
  Edge l1 = makeEdge(5, 4, 8, 4, EdgeSourceInfo(1));
  // Also add an already-labeled boundary edge at (5,7) -> no, leave (5,7) with
  // only collapse edges and add a second branch (5,7)-(2,7) [collapse of A]
  // to verify edgeStack pops multiple items and skips already-labeled edges.
  Edge c3 = makeEdge(5, 7, 2, 7, EdgeSourceInfo(0, 0, false));

  graph.addEdge(&bLeft);
  graph.addEdge(&bRight);
  graph.addEdge(&c1);
  graph.addEdge(&c2);
  graph.addEdge(&l1);
  graph.addEdge(&c3);

  OverlayLabeller labeller(&graph, &inputGeom);
  labeller.computeLabelling();

  OverlayEdge* oeC1 = findEdge(graph, {5, 10}, {5, 7});
  OverlayEdge* oeC2 = findEdge(graph, {5, 7}, {5, 4});
  OverlayEdge* oeC3 = findEdge(graph, {5, 7}, {2, 7});
  OverlayEdge* oeL1 = findEdge(graph, {5, 4}, {8, 4});
  ASSERT_NE(oeC1, nullptr);
  ASSERT_NE(oeC2, nullptr);
  ASSERT_NE(oeC3, nullptr);
  ASSERT_NE(oeL1, nullptr);

  EXPECT_EQ(oeC1->getLabel()->getLineLocation(0), Location::INTERIOR);
  EXPECT_EQ(oeC2->getLabel()->getLineLocation(0), Location::INTERIOR);
  EXPECT_EQ(oeC3->getLabel()->getLineLocation(0), Location::INTERIOR);
  EXPECT_EQ(oeL1->getLabel()->getLineLocation(0), Location::INTERIOR);
}

TEST_F(OverlayLabellerTest, PropagateLinearLocationsForLineInputGuards) {
  auto polyA =
      readWkt("POLYGON ((100 100, 110 100, 110 110, 100 110, 100 100))");
  auto lineB = readWkt("LINESTRING (0 0, 10 0)");
  InputGeometry inputGeom(polyA.get(), lineB.get());

  OverlayGraph graph;
  // Connected pair with pre-seeded INTERIOR location on Line input (geom 1):
  // propagateLinearLocationAtNode must NOT propagate non-EXTERIOR locations
  // when isInputLine is true, so eInt2 remains UNKNOWN until
  // labelDisconnectedEdges sets it to EXTERIOR.
  Edge eInt1 = makeEdge(0, 0, 5, 0, EdgeSourceInfo(1));
  Edge eInt2 = makeEdge(5, 0, 10, 0, EdgeSourceInfo(1));
  // Connected pair with pre-seeded EXTERIOR location on Line input (geom 1):
  // propagateLinearLocationAtNode DOES propagate EXTERIOR to connected
  // unknown edges via the stack.
  Edge eExt1 = makeEdge(20, 0, 25, 0, EdgeSourceInfo(1));
  Edge eExt2 = makeEdge(25, 0, 30, 0, EdgeSourceInfo(1));

  OverlayEdge* oeInt1 = graph.addEdge(&eInt1);
  OverlayEdge* oeInt2 = graph.addEdge(&eInt2);
  OverlayEdge* oeExt1 = graph.addEdge(&eExt1);
  OverlayEdge* oeExt2 = graph.addEdge(&eExt2);

  oeInt1->getLabel()->setLocationLine(1, Location::INTERIOR);
  oeExt1->getLabel()->setLocationLine(1, Location::EXTERIOR);

  OverlayLabeller labeller(&graph, &inputGeom);
  labeller.computeLabelling();

  EXPECT_EQ(oeInt1->getLabel()->getLineLocation(1), Location::INTERIOR);
  EXPECT_EQ(oeInt2->getLabel()->getLineLocation(1), Location::EXTERIOR);
  EXPECT_EQ(oeExt1->getLabel()->getLineLocation(1), Location::EXTERIOR);
  EXPECT_EQ(oeExt2->getLabel()->getLineLocation(1), Location::EXTERIOR);
}

TEST_F(OverlayLabellerTest, PropagateAreaLocationsEarlyReturnsAndExceptions) {
  auto polyA = readWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto lineB = readWkt("LINESTRING (0 0, 10 0)");
  InputGeometry inputGeom(polyA.get(), lineB.get());

  // 1. !isArea(geomIndex) and degree() == 1 and eStart == nullptr early
  // returns.
  {
    OverlayGraph graph;
    Edge e1 = makeEdge(2, 2, 5, 2, EdgeSourceInfo(1));
    Edge e2 = makeEdge(5, 2, 8, 2, EdgeSourceInfo(1));
    OverlayEdge* oe1 = graph.addEdge(&e1);
    OverlayEdge* oe2 = graph.addEdge(&e2);
    OverlayLabeller labeller(&graph, &inputGeom);

    // geomIndex 1 is a Line -> early return.
    labeller.propagateAreaLocations(oe2, 1);
    EXPECT_TRUE(oe2->getLabel()->isLineLocationUnknown(1));

    // orig of oe1 has degree 1 -> early return.
    EXPECT_EQ(oe1->degree(), 1);
    labeller.propagateAreaLocations(oe1, 0);
    EXPECT_TRUE(oe1->getLabel()->isLineLocationUnknown(0));

    // orig of oe2 (5, 2) has degree 2 but no boundary edge of A -> eStart ==
    // nullptr.
    EXPECT_EQ(oe2->degree(), 2);
    labeller.propagateAreaLocations(oe2, 0);
    EXPECT_TRUE(oe2->getLabel()->isLineLocationUnknown(0));
  }

  // 2. Side location conflict throws TopologyException.
  {
    OverlayGraph graph;
    Edge e1 = makeEdge(0, 0, 10, 0, EdgeSourceInfo(0, 1, false));
    Edge e2 = makeEdge(0, 0, 0, 10, EdgeSourceInfo(0, 1, false));
    graph.addEdge(&e1);
    graph.addEdge(&e2);

    OverlayLabeller labeller(&graph, &inputGeom);
    OverlayEdge* nodeEdge = graph.getNodeEdge(Coordinate(0, 0));
    ASSERT_NE(nodeEdge, nullptr);
    try {
      labeller.propagateAreaLocations(nodeEdge, 0);
      FAIL() << "Expected TopologyException";
    } catch (const TopologyException& ex) {
      EXPECT_NE(std::string(ex.what()).find("side location conflict"),
                std::string::npos);
    }
  }

  // 3. Single null side (LEFT == NONE, RIGHT matches currLoc) throws
  // AssertionFailedException.
  {
    OverlayGraph graph;
    Edge e1 = makeEdge(0, 0, 10, 0, EdgeSourceInfo(0, -1, false));
    Edge e2 = makeEdge(0, 0, 0, 10, EdgeSourceInfo(0, 1, false));
    OverlayEdge* oe1 = graph.addEdge(&e1);
    OverlayEdge* oe2 = graph.addEdge(&e2);
    // For oe1 forward: LEFT = INTERIOR, RIGHT = EXTERIOR.
    // Set oe2 forward: LEFT = NONE, RIGHT = INTERIOR (matches oe1's LEFT).
    oe2->getLabel()->initBoundary(0, Location::NONE, Location::INTERIOR, false);

    OverlayLabeller labeller(&graph, &inputGeom);
    EXPECT_THROW(labeller.propagateAreaLocations(oe1, 0),
                 AssertionFailedException);
  }

  // 4. Boundary edge with both sides NONE fails hasSides assertion in both
  // propagateAreaLocations (from oe1) and findPropagationStartEdge (from oe2).
  {
    OverlayGraph graph;
    Edge e1 = makeEdge(0, 0, 10, 0, EdgeSourceInfo(0, -1, false));
    Edge e2 = makeEdge(0, 0, 0, 10, EdgeSourceInfo(0, 1, false));
    OverlayEdge* oe1 = graph.addEdge(&e1);
    OverlayEdge* oe2 = graph.addEdge(&e2);
    oe2->getLabel()->initBoundary(0, Location::NONE, Location::NONE, false);

    OverlayLabeller labeller(&graph, &inputGeom);
    EXPECT_THROW(labeller.propagateAreaLocations(oe1, 0),
                 AssertionFailedException);
    EXPECT_THROW(labeller.propagateAreaLocations(oe2, 0),
                 AssertionFailedException);
  }
}

TEST_F(OverlayLabellerTest, LocateEdgeBothEndsAndCollapsedInputGeometry) {
  auto polyA = readWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto lineB = readWkt("LINESTRING (2 2, 8 8)");

  // Disconnected edges tested against Area A using locateEdgeBothEnds.
  {
    InputGeometry inputGeom(polyA.get(), lineB.get());
    OverlayGraph graph;
    Edge inside = makeEdge(2, 2, 8, 8, EdgeSourceInfo(1));
    Edge boundaryToInside = makeEdge(0, 5, 5, 5, EdgeSourceInfo(1));
    Edge insideToOutside = makeEdge(5, 5, 15, 5, EdgeSourceInfo(1));
    Edge outsideToInside = makeEdge(15, 6, 5, 6, EdgeSourceInfo(1));
    Edge outside = makeEdge(20, 20, 30, 30, EdgeSourceInfo(1));

    OverlayEdge* oeInside = graph.addEdge(&inside);
    OverlayEdge* oeBndToIn = graph.addEdge(&boundaryToInside);
    OverlayEdge* oeInToOut = graph.addEdge(&insideToOutside);
    OverlayEdge* oeOutToIn = graph.addEdge(&outsideToInside);
    OverlayEdge* oeOutside = graph.addEdge(&outside);

    OverlayLabeller labeller(&graph, &inputGeom);
    labeller.computeLabelling();

    EXPECT_EQ(oeInside->getLabel()->getLineLocation(0), Location::INTERIOR);
    EXPECT_EQ(oeBndToIn->getLabel()->getLineLocation(0), Location::INTERIOR);
    EXPECT_EQ(oeInToOut->getLabel()->getLineLocation(0), Location::EXTERIOR);
    EXPECT_EQ(oeOutToIn->getLabel()->getLineLocation(0), Location::EXTERIOR);
    EXPECT_EQ(oeOutside->getLabel()->getLineLocation(0), Location::EXTERIOR);
  }

  // Completely collapsed Area A via EdgeNodingBuilder with fixed PM.
  {
    PrecisionModel pm(1.0);
    auto tinyA = readWkt("POLYGON ((0 0, 0.1 0, 0.1 0.1, 0 0.1, 0 0))");
    auto bigB = readWkt("POLYGON ((-5 -5, 5 -5, 5 5, -5 5, -5 -5))");

    OverlayGraph graph;
    InputGeometry inputGeom(tinyA.get(), bigB.get());
    buildGraph(tinyA.get(), bigB.get(), &pm, graph, inputGeom);

    OverlayLabeller labeller(&graph, &inputGeom);
    labeller.computeLabelling();

    for (OverlayEdge* e : graph.getEdges()) {
      EXPECT_EQ(e->getLabel()->getLineLocation(0), Location::EXTERIOR);
      EXPECT_TRUE(e->getLabel()->isBoundary(1));
    }
  }
}

TEST_F(OverlayLabellerTest, MarkResultAreaEdgesAndUnmarkDuplicatesAcrossOps) {
  auto squareA = readWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto squareB = readWkt("POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))");
  PrecisionModel pm;

  // Adjacent squares share edge (10, 0)-(10, 10): UNION and SYMDIFFERENCE
  // mark both sides of the shared edge before unmarking duplicates.
  for (int op : {+OverlayNG::UNION, +OverlayNG::SYMDIFFERENCE}) {
    OverlayGraph graph;
    InputGeometry inputGeom(squareA.get(), squareB.get());
    buildGraph(squareA.get(), squareB.get(), &pm, graph, inputGeom);

    OverlayLabeller labeller(&graph, &inputGeom);
    labeller.computeLabelling();
    labeller.markResultAreaEdges(op);

    OverlayEdge* shared = findEdge(graph, {10, 0}, {10, 10});
    ASSERT_NE(shared, nullptr);
    EXPECT_TRUE(shared->isInResultAreaBoth());
    EXPECT_EQ(graph.getResultAreaEdges().size(), 5u);

    labeller.unmarkDuplicateEdgesFromResultArea();
    EXPECT_FALSE(shared->isInResultArea());
    EXPECT_FALSE(shared->symOE()->isInResultArea());
    EXPECT_EQ(graph.getResultAreaEdges().size(), 3u);
  }

  // DIFFERENCE keeps only A's side of the shared edge marked.
  {
    OverlayGraph graph;
    InputGeometry inputGeom(squareA.get(), squareB.get());
    buildGraph(squareA.get(), squareB.get(), &pm, graph, inputGeom);

    OverlayLabeller labeller(&graph, &inputGeom);
    labeller.computeLabelling();
    labeller.markResultAreaEdges(+OverlayNG::DIFFERENCE);

    OverlayEdge* shared = findEdge(graph, {10, 0}, {10, 10});
    ASSERT_NE(shared, nullptr);
    EXPECT_FALSE(shared->isInResultAreaBoth());
    EXPECT_TRUE(shared->isInResultArea() || shared->symOE()->isInResultArea());

    labeller.unmarkDuplicateEdgesFromResultArea();
    EXPECT_EQ(graph.getResultAreaEdges().size(), 3u);
  }

  // Identical distinct squares node each other at all 4 corners, producing 4
  // merged boundary edges with matching side locations: UNION and INTERSECTION
  // keep all 4 edges, while DIFFERENCE and SYMDIFFERENCE keep 0.
  {
    auto squareSame = readWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
    const struct {
      int op;
      std::size_t expected;
    } kSameCases[] = {
        {+OverlayNG::UNION, 4},
        {+OverlayNG::INTERSECTION, 4},
        {+OverlayNG::DIFFERENCE, 0},
        {+OverlayNG::SYMDIFFERENCE, 0},
    };
    for (const auto& tc : kSameCases) {
      OverlayGraph graph;
      InputGeometry inputGeom(squareA.get(), squareSame.get());
      buildGraph(squareA.get(), squareSame.get(), &pm, graph, inputGeom);

      OverlayLabeller labeller(&graph, &inputGeom);
      labeller.computeLabelling();
      labeller.markResultAreaEdges(tc.op);
      labeller.unmarkDuplicateEdgesFromResultArea();
      EXPECT_EQ(graph.getResultAreaEdges().size(), tc.expected);
    }
  }

  // Overlapping squares with INTERSECTION marks the overlap boundary.
  {
    auto overlapB = readWkt("POLYGON ((5 0, 15 0, 15 10, 5 10, 5 0))");
    OverlayGraph graph;
    InputGeometry inputGeom(squareA.get(), overlapB.get());
    buildGraph(squareA.get(), overlapB.get(), &pm, graph, inputGeom);

    OverlayLabeller labeller(&graph, &inputGeom);
    labeller.computeLabelling();

    OverlayEdge* bLeft = findEdge(graph, {5, 0}, {5, 10});
    ASSERT_NE(bLeft, nullptr);
    EXPECT_TRUE(bLeft->getLabel()->isBoundary(1));
    EXPECT_EQ(bLeft->getLabel()->getLineLocation(0), Location::INTERIOR);

    labeller.markResultAreaEdges(+OverlayNG::INTERSECTION);
    labeller.unmarkDuplicateEdgesFromResultArea();

    EXPECT_EQ(graph.getResultAreaEdges().size(), 4u);
  }
}

TEST_F(OverlayLabellerTest, MarkInResultAreaNonBoundaryAndMixedBoundaryLine) {
  OverlayGraph graph;

  // Non-boundary edge (LINE + LINE): markInResultArea must never mark it.
  Edge lineOnly =
      makeMergedEdge(0, 0, 10, 0, EdgeSourceInfo(0), EdgeSourceInfo(1));
  OverlayEdge* oeLine = graph.addEdge(&lineOnly);
  oeLine->getLabel()->setLocationAll(0, Location::INTERIOR);
  oeLine->getLabel()->setLocationAll(1, Location::INTERIOR);

  for (int op : {+OverlayNG::INTERSECTION, +OverlayNG::UNION,
                 +OverlayNG::DIFFERENCE, +OverlayNG::SYMDIFFERENCE}) {
    OverlayLabeller::markInResultArea(oeLine, op);
    EXPECT_FALSE(oeLine->isInResultArea());
    OverlayLabeller::markInResultArea(oeLine->symOE(), op);
    EXPECT_FALSE(oeLine->symOE()->isInResultArea());
  }

  // Boundary of A (LEFT=INTERIOR, RIGHT=EXTERIOR forward) merged with Line of B
  // (EXTERIOR): only backward half-edge has RIGHT=INTERIOR for A.
  Edge bndWithLine = makeMergedEdge(0, 10, 10, 10, EdgeSourceInfo(0, -1, false),
                                    EdgeSourceInfo(1));
  OverlayEdge* oeFwd = graph.addEdge(&bndWithLine);
  oeFwd->getLabel()->setLocationLine(1, Location::EXTERIOR);

  OverlayLabeller::markInResultArea(oeFwd, +OverlayNG::UNION);
  EXPECT_FALSE(oeFwd->isInResultArea());

  OverlayLabeller::markInResultArea(oeFwd->symOE(), +OverlayNG::UNION);
  EXPECT_TRUE(oeFwd->symOE()->isInResultArea());

  // With B line location INTERIOR, INTERSECTION marks only the half-edge whose
  // RIGHT side is INTERIOR for A.
  Edge bndWithIntLine = makeMergedEdge(
      0, 20, 10, 20, EdgeSourceInfo(0, -1, false), EdgeSourceInfo(1));
  OverlayEdge* oeIntFwd = graph.addEdge(&bndWithIntLine);
  oeIntFwd->getLabel()->setLocationLine(1, Location::INTERIOR);

  OverlayLabeller::markInResultArea(oeIntFwd, +OverlayNG::INTERSECTION);
  EXPECT_FALSE(oeIntFwd->isInResultArea());

  OverlayLabeller::markInResultArea(oeIntFwd->symOE(),
                                    +OverlayNG::INTERSECTION);
  EXPECT_TRUE(oeIntFwd->symOE()->isInResultArea());

  // Boundary of B (LEFT=INTERIOR, RIGHT=EXTERIOR forward) merged with Line of
  // A.
  Edge bndBWithExtLineA = makeMergedEdge(0, 30, 10, 30, EdgeSourceInfo(0),
                                         EdgeSourceInfo(1, -1, false));
  OverlayEdge* oeBFwd = graph.addEdge(&bndBWithExtLineA);
  oeBFwd->getLabel()->setLocationLine(0, Location::EXTERIOR);

  OverlayLabeller::markInResultArea(oeBFwd, +OverlayNG::UNION);
  EXPECT_FALSE(oeBFwd->isInResultArea());

  OverlayLabeller::markInResultArea(oeBFwd->symOE(), +OverlayNG::UNION);
  EXPECT_TRUE(oeBFwd->symOE()->isInResultArea());

  Edge bndBWithIntLineA = makeMergedEdge(0, 40, 10, 40, EdgeSourceInfo(0),
                                         EdgeSourceInfo(1, -1, false));
  OverlayEdge* oeBIntFwd = graph.addEdge(&bndBWithIntLineA);
  oeBIntFwd->getLabel()->setLocationLine(0, Location::INTERIOR);

  OverlayLabeller::markInResultArea(oeBIntFwd, +OverlayNG::INTERSECTION);
  EXPECT_FALSE(oeBIntFwd->isInResultArea());

  OverlayLabeller::markInResultArea(oeBIntFwd->symOE(),
                                    +OverlayNG::INTERSECTION);
  EXPECT_TRUE(oeBIntFwd->symOE()->isInResultArea());
}

TEST_F(OverlayLabellerTest, DisconnectedEdgesAndCollapsedAreaForGeom1) {
  auto lineA = readWkt("LINESTRING (2 2, 8 8)");
  auto polyB = readWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  // Disconnected edges of geom 0 tested against Area B (geomIndex == 1).
  {
    InputGeometry inputGeom(lineA.get(), polyB.get());
    OverlayGraph graph;
    Edge inside = makeEdge(2, 2, 8, 8, EdgeSourceInfo(0));
    Edge boundaryToInside = makeEdge(0, 5, 5, 5, EdgeSourceInfo(0));
    Edge insideToOutside = makeEdge(5, 5, 15, 5, EdgeSourceInfo(0));
    Edge outsideToInside = makeEdge(15, 6, 5, 6, EdgeSourceInfo(0));
    Edge outside = makeEdge(20, 20, 30, 30, EdgeSourceInfo(0));

    OverlayEdge* oeInside = graph.addEdge(&inside);
    OverlayEdge* oeBndToIn = graph.addEdge(&boundaryToInside);
    OverlayEdge* oeInToOut = graph.addEdge(&insideToOutside);
    OverlayEdge* oeOutToIn = graph.addEdge(&outsideToInside);
    OverlayEdge* oeOutside = graph.addEdge(&outside);

    OverlayLabeller labeller(&graph, &inputGeom);
    labeller.computeLabelling();

    for (OverlayEdge* oe :
         {oeInside, oeBndToIn, oeInToOut, oeOutToIn, oeOutside}) {
      EXPECT_EQ(oe->getLabel()->getLineLocation(0), Location::EXTERIOR);
    }
    EXPECT_EQ(oeInside->getLabel()->getLineLocation(1), Location::INTERIOR);
    EXPECT_EQ(oeBndToIn->getLabel()->getLineLocation(1), Location::INTERIOR);
    EXPECT_EQ(oeInToOut->getLabel()->getLineLocation(1), Location::EXTERIOR);
    EXPECT_EQ(oeOutToIn->getLabel()->getLineLocation(1), Location::EXTERIOR);
    EXPECT_EQ(oeOutside->getLabel()->getLineLocation(1), Location::EXTERIOR);
  }

  // Completely collapsed Area B via EdgeNodingBuilder with fixed PM.
  {
    PrecisionModel pm(1.0);
    auto bigA = readWkt("POLYGON ((-5 -5, 5 -5, 5 5, -5 5, -5 -5))");
    auto tinyB = readWkt("POLYGON ((0 0, 0.1 0, 0.1 0.1, 0 0.1, 0 0))");

    OverlayGraph graph;
    InputGeometry inputGeom(bigA.get(), tinyB.get());
    buildGraph(bigA.get(), tinyB.get(), &pm, graph, inputGeom);

    OverlayLabeller labeller(&graph, &inputGeom);
    labeller.computeLabelling();

    for (OverlayEdge* e : graph.getEdges()) {
      EXPECT_TRUE(e->getLabel()->isBoundary(0));
      EXPECT_EQ(e->getLabel()->getLineLocation(1), Location::EXTERIOR);
    }
  }
}

TEST_F(OverlayLabellerTest,
       CoincidentCollapsedEdgesBothInputsAndCyclicLinearPropagation) {
  // Coincident collapsed shell of A and collapsed hole of B on the same edge.
  {
    PrecisionModel pm(1.0);
    auto thinShellA = readWkt("POLYGON ((2 2.1, 8 2.1, 8 2.3, 2 2.3, 2 2.1))");
    auto polyWithThinHoleB = readWkt(
        "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
        "(2 2.1, 8 2.1, 8 2.3, 2 2.3, 2 2.1))");

    OverlayGraph graph;
    InputGeometry inputGeom(thinShellA.get(), polyWithThinHoleB.get());
    buildGraph(thinShellA.get(), polyWithThinHoleB.get(), &pm, graph,
               inputGeom);

    OverlayLabeller labeller(&graph, &inputGeom);
    labeller.computeLabelling();

    OverlayEdge* collapsed = findEdge(graph, {2, 2}, {8, 2});
    ASSERT_NE(collapsed, nullptr);
    EXPECT_TRUE(collapsed->getLabel()->isCollapse(0));
    EXPECT_TRUE(collapsed->getLabel()->isCollapse(1));
    EXPECT_FALSE(collapsed->getLabel()->isHole(0));
    EXPECT_TRUE(collapsed->getLabel()->isHole(1));
    EXPECT_EQ(collapsed->getLabel()->getLineLocation(0), Location::EXTERIOR);
    EXPECT_EQ(collapsed->getLabel()->getLineLocation(1), Location::INTERIOR);
  }

  // Cyclic triangle of collapsed edges plus tail: ensures
  // propagateLinearLocationAtNode handles cycles and skips already-labelled
  // neighbor edges without looping infinitely.
  {
    auto polyA = readWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
    auto lineB = readWkt("LINESTRING (2 7, 2 4)");
    InputGeometry inputGeom(polyA.get(), lineB.get());
    OverlayGraph graph;

    Edge bLeft = makeEdge(0, 10, 5, 10, EdgeSourceInfo(0, 1, false));
    Edge bRight = makeEdge(5, 10, 10, 10, EdgeSourceInfo(0, 1, false));
    Edge c1 = makeEdge(5, 10, 5, 7, EdgeSourceInfo(0, 0, false));
    Edge c2 = makeEdge(5, 10, 2, 7, EdgeSourceInfo(0, 0, false));
    Edge c3 = makeEdge(5, 7, 2, 7, EdgeSourceInfo(0, 0, false));
    Edge lTail = makeEdge(2, 7, 2, 4, EdgeSourceInfo(1));

    graph.addEdge(&bLeft);
    graph.addEdge(&bRight);
    graph.addEdge(&c1);
    graph.addEdge(&c2);
    graph.addEdge(&c3);
    graph.addEdge(&lTail);

    OverlayLabeller labeller(&graph, &inputGeom);
    labeller.computeLabelling();

    for (const auto& pts : std::vector<std::pair<CoordinateXY, CoordinateXY>>{
             {{5, 10}, {5, 7}},
             {{5, 10}, {2, 7}},
             {{5, 7}, {2, 7}},
             {{2, 7}, {2, 4}},
         }) {
      OverlayEdge* oe = findEdge(graph, pts.first, pts.second);
      ASSERT_NE(oe, nullptr);
      EXPECT_EQ(oe->getLabel()->getLineLocation(0), Location::INTERIOR);
    }
  }
}

}  // namespace
}  // namespace geos_google
