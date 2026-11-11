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

// Unit tests for third_party/geos/src/operation/overlayng/OverlayEdgeRing.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/OverlayEdgeRing.cpp

#include "third_party/geos/include/geos/operation/overlayng/OverlayEdgeRing.h"

#include <cstddef>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/operation/overlayng/Edge.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayEdge.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayGraph.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayLabel.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::operation::overlayng::OverlayEdge;
using geos::operation::overlayng::OverlayEdgeRing;
using geos::operation::overlayng::OverlayGraph;
using geos::operation::overlayng::OverlayLabel;
using geos::util::TopologyException;

std::vector<OverlayEdge*> BuildLinkedRingEdges(
    OverlayGraph& graph, const std::vector<Coordinate>& closedRingCoords,
    bool isHole, bool hasZ = false) {
  EdgeSourceInfo esi(0, 1, isHole);
  std::vector<OverlayEdge*> edges;
  const std::size_t segCount = closedRingCoords.size() - 1;
  edges.reserve(segCount);
  for (std::size_t i = 0; i < segCount; ++i) {
    auto seq = std::make_unique<CoordinateSequence>(0u, hasZ, false);
    seq->add(closedRingCoords[i]);
    seq->add(closedRingCoords[i + 1]);
    Edge edge(std::move(seq), &esi);
    OverlayEdge* oe = graph.addEdge(&edge);
    oe->markInResultArea();
    edges.push_back(oe);
  }
  for (std::size_t i = 0; i < segCount; ++i) {
    edges[i]->setNextResult(edges[(i + 1) % segCount]);
  }
  return edges;
}

TEST(OverlayEdgeRingTest, ConstructsShellRingFromCwEdges) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;
  std::vector<OverlayEdge*> edges = BuildLinkedRingEdges(
      graph,
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
       Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
      /*isHole=*/false);

  OverlayEdgeRing ring(edges.front(), factory.get());
  EXPECT_FALSE(ring.isHole());
  EXPECT_EQ(ring.getEdge(), edges.front());
  EXPECT_FALSE(ring.hasShell());
  EXPECT_EQ(ring.getShell(), &ring);

  const LinearRing* lr = ring.getRingPtr();
  ASSERT_NE(lr, nullptr);
  EXPECT_TRUE(lr->isClosed());
  EXPECT_EQ(lr->getNumPoints(), 5u);
  EXPECT_TRUE(ring.getCoordinate().equals2D(lr->getCoordinateN(0)));
  for (OverlayEdge* e : edges) {
    EXPECT_EQ(e->getEdgeRing(), &ring);
  }
}

TEST(OverlayEdgeRingTest, ConstructsHoleRingFromCcwEdges) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;
  std::vector<OverlayEdge*> edges = BuildLinkedRingEdges(
      graph,
      {Coordinate(2.0, 2.0), Coordinate(8.0, 2.0), Coordinate(8.0, 8.0),
       Coordinate(2.0, 8.0), Coordinate(2.0, 2.0)},
      /*isHole=*/true);

  OverlayEdgeRing hole(edges.front(), factory.get());
  EXPECT_TRUE(hole.isHole());
  EXPECT_FALSE(hole.hasShell());
  EXPECT_EQ(hole.getShell(), nullptr);
  ASSERT_NE(hole.getRingPtr(), nullptr);
  EXPECT_EQ(hole.getRingPtr()->getNumPoints(), 5u);
}

TEST(OverlayEdgeRingTest, ConstructsFromBackwardEdgesAndPreservesZ) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;
  EdgeSourceInfo esi(0, -1, false);

  // Add edges in CCW order so their symOE() edges run CW with Z values.
  const std::vector<Coordinate> ccwCoords = {
      Coordinate(0.0, 0.0, 10.0), Coordinate(10.0, 0.0, 20.0),
      Coordinate(10.0, 10.0, 30.0), Coordinate(0.0, 10.0, 40.0),
      Coordinate(0.0, 0.0, 10.0)};
  std::vector<OverlayEdge*> symEdges;
  for (std::size_t i = 0; i + 1 < ccwCoords.size(); ++i) {
    auto seq = std::make_unique<CoordinateSequence>(0u, true, false);
    seq->add(ccwCoords[i]);
    seq->add(ccwCoords[i + 1]);
    Edge edge(std::move(seq), &esi);
    OverlayEdge* fwd = graph.addEdge(&edge);
    OverlayEdge* rev = fwd->symOE();
    rev->markInResultArea();
    symEdges.push_back(rev);
  }
  // Link symEdges in CW order: 3 -> 2 -> 1 -> 0 -> 3.
  symEdges[3]->setNextResult(symEdges[2]);
  symEdges[2]->setNextResult(symEdges[1]);
  symEdges[1]->setNextResult(symEdges[0]);
  symEdges[0]->setNextResult(symEdges[3]);

  OverlayEdgeRing ring(symEdges[3], factory.get());
  EXPECT_FALSE(ring.isHole());
  const LinearRing* lr = ring.getRingPtr();
  ASSERT_NE(lr, nullptr);
  EXPECT_TRUE(lr->hasZ());
  EXPECT_EQ(lr->getNumPoints(), 5u);
  const CoordinateSequence* coords = lr->getCoordinatesRO();
  // OverlayEdge::addCoordinates starts at the second vertex along start's
  // direction (symEdges[3]'s destination) and wraps back via closeRing().
  EXPECT_TRUE(coords->getAt(0).equals3D(Coordinate(0.0, 10.0, 40.0)));
  EXPECT_TRUE(coords->getAt(1).equals3D(Coordinate(10.0, 10.0, 30.0)));
  EXPECT_TRUE(coords->getAt(2).equals3D(Coordinate(10.0, 0.0, 20.0)));
  EXPECT_TRUE(coords->getAt(3).equals3D(Coordinate(0.0, 0.0, 10.0)));
  EXPECT_TRUE(coords->getAt(4).equals3D(Coordinate(0.0, 10.0, 40.0)));
}

TEST(OverlayEdgeRingTest, MultiPointEdgesPreserveZAndM) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;
  EdgeSourceInfo esi(0, 1, false);

  // Two 3-point XYZM forward edges forming a CW square.
  auto seq0 = std::make_unique<CoordinateSequence>(0u, true, true);
  seq0->add(CoordinateXYZM(0.0, 0.0, 1.0, 101.0));
  seq0->add(CoordinateXYZM(0.0, 10.0, 2.0, 102.0));
  seq0->add(CoordinateXYZM(10.0, 10.0, 3.0, 103.0));
  Edge edge0(std::move(seq0), &esi);
  OverlayEdge* fwd0 = graph.addEdge(&edge0);

  auto seq1 = std::make_unique<CoordinateSequence>(0u, true, true);
  seq1->add(CoordinateXYZM(10.0, 10.0, 3.0, 103.0));
  seq1->add(CoordinateXYZM(10.0, 0.0, 4.0, 104.0));
  seq1->add(CoordinateXYZM(0.0, 0.0, 1.0, 101.0));
  Edge edge1(std::move(seq1), &esi);
  OverlayEdge* fwd1 = graph.addEdge(&edge1);

  fwd0->setNextResult(fwd1);
  fwd1->setNextResult(fwd0);

  OverlayEdgeRing ring(fwd0, factory.get());
  EXPECT_FALSE(ring.isHole());
  const LinearRing* lr = ring.getRingPtr();
  ASSERT_NE(lr, nullptr);
  EXPECT_TRUE(lr->hasZ());
  EXPECT_TRUE(lr->hasM());
  ASSERT_EQ(lr->getNumPoints(), 5u);
  const CoordinateSequence* coords = lr->getCoordinatesRO();
  EXPECT_TRUE(coords->getAt(0).equals3D(Coordinate(0.0, 10.0, 2.0)));
  EXPECT_DOUBLE_EQ(coords->getOrdinate(0, CoordinateSequence::M), 102.0);
  EXPECT_TRUE(coords->getAt(1).equals3D(Coordinate(10.0, 10.0, 3.0)));
  EXPECT_DOUBLE_EQ(coords->getOrdinate(1, CoordinateSequence::M), 103.0);
  EXPECT_TRUE(coords->getAt(2).equals3D(Coordinate(10.0, 0.0, 4.0)));
  EXPECT_DOUBLE_EQ(coords->getOrdinate(2, CoordinateSequence::M), 104.0);
  EXPECT_TRUE(coords->getAt(3).equals3D(Coordinate(0.0, 0.0, 1.0)));
  EXPECT_DOUBLE_EQ(coords->getOrdinate(3, CoordinateSequence::M), 101.0);
  EXPECT_TRUE(coords->getAt(4).equals3D(Coordinate(0.0, 10.0, 2.0)));

  // Multi-point backward edge paired with a 2-point backward edge.
  EdgeSourceInfo revInfo(0, -1, false);
  auto seqA = std::make_unique<CoordinateSequence>(0u, true, false);
  seqA->add(Coordinate(0.0, 0.0, 10.0));
  seqA->add(Coordinate(10.0, 0.0, 20.0));
  seqA->add(Coordinate(10.0, 10.0, 30.0));
  Edge edgeA(std::move(seqA), &revInfo);
  OverlayEdge* revA = graph.addEdge(&edgeA)->symOE();

  auto seqB = std::make_unique<CoordinateSequence>(0u, true, false);
  seqB->add(Coordinate(10.0, 10.0, 30.0));
  seqB->add(Coordinate(0.0, 0.0, 10.0));
  Edge edgeB(std::move(seqB), &revInfo);
  OverlayEdge* revB = graph.addEdge(&edgeB)->symOE();

  revA->setNextResult(revB);
  revB->setNextResult(revA);

  OverlayEdgeRing triRing(revA, factory.get());
  EXPECT_FALSE(triRing.isHole());
  const LinearRing* triLr = triRing.getRingPtr();
  ASSERT_NE(triLr, nullptr);
  EXPECT_TRUE(triLr->hasZ());
  ASSERT_EQ(triLr->getNumPoints(), 4u);
  const CoordinateSequence* triCoords = triLr->getCoordinatesRO();
  EXPECT_TRUE(triCoords->getAt(0).equals3D(Coordinate(10.0, 0.0, 20.0)));
  EXPECT_TRUE(triCoords->getAt(1).equals3D(Coordinate(0.0, 0.0, 10.0)));
  EXPECT_TRUE(triCoords->getAt(2).equals3D(Coordinate(10.0, 10.0, 30.0)));
  EXPECT_TRUE(triCoords->getAt(3).equals3D(Coordinate(10.0, 0.0, 20.0)));
}

TEST(OverlayEdgeRingTest, SingleMultiPointEdgeClosesIntoValidRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;
  EdgeSourceInfo esi(0, 1, false);
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  seq->add(Coordinate(0.0, 0.0));
  seq->add(Coordinate(0.0, 10.0));
  seq->add(Coordinate(10.0, 0.0));
  seq->add(Coordinate(0.0, 0.0));
  Edge edge(std::move(seq), &esi);
  OverlayEdge* oe = graph.addEdge(&edge);
  oe->setNextResult(oe);

  OverlayEdgeRing ring(oe, factory.get());
  EXPECT_FALSE(ring.isHole());
  ASSERT_NE(ring.getRingPtr(), nullptr);
  EXPECT_EQ(ring.getRingPtr()->getNumPoints(), 4u);
  EXPECT_TRUE(ring.getRingPtr()->isClosed());
}

TEST(OverlayEdgeRingTest, SetShellAddHoleAndToPolygonWithAndWithoutHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;

  // Polygon without holes.
  std::vector<OverlayEdge*> shellOnlyEdges = BuildLinkedRingEdges(
      graph,
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
       Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
      false);
  OverlayEdgeRing shellOnly(shellOnlyEdges.front(), factory.get());
  std::unique_ptr<Polygon> polyNoHoles = shellOnly.toPolygon(factory.get());
  ASSERT_NE(polyNoHoles, nullptr);
  EXPECT_EQ(polyNoHoles->getNumInteriorRing(), 0u);
  EXPECT_TRUE(polyNoHoles->isValid());
  EXPECT_DOUBLE_EQ(polyNoHoles->getArea(), 100.0);
  EXPECT_EQ(shellOnly.getRingPtr(), nullptr);

  // Polygon with two holes and null-shell edge case.
  std::vector<OverlayEdge*> shellEdges = BuildLinkedRingEdges(
      graph,
      {Coordinate(20.0, 0.0), Coordinate(20.0, 10.0), Coordinate(30.0, 10.0),
       Coordinate(30.0, 0.0), Coordinate(20.0, 0.0)},
      false);
  std::vector<OverlayEdge*> hole1Edges = BuildLinkedRingEdges(
      graph,
      {Coordinate(21.0, 1.0), Coordinate(23.0, 1.0), Coordinate(23.0, 3.0),
       Coordinate(21.0, 3.0), Coordinate(21.0, 1.0)},
      true);
  std::vector<OverlayEdge*> hole2Edges = BuildLinkedRingEdges(
      graph,
      {Coordinate(25.0, 5.0), Coordinate(28.0, 5.0), Coordinate(28.0, 8.0),
       Coordinate(25.0, 8.0), Coordinate(25.0, 5.0)},
      true);
  std::vector<OverlayEdge*> orphanEdges = BuildLinkedRingEdges(
      graph,
      {Coordinate(21.0, 5.0), Coordinate(22.0, 5.0), Coordinate(22.0, 6.0),
       Coordinate(21.0, 6.0), Coordinate(21.0, 5.0)},
      true);

  OverlayEdgeRing shellWithHoles(shellEdges.front(), factory.get());
  OverlayEdgeRing hole1(hole1Edges.front(), factory.get());
  OverlayEdgeRing hole2(hole2Edges.front(), factory.get());
  OverlayEdgeRing orphanHole(orphanEdges.front(), factory.get());

  orphanHole.setShell(nullptr);
  EXPECT_FALSE(orphanHole.hasShell());
  EXPECT_EQ(orphanHole.getShell(), nullptr);

  hole1.setShell(&shellWithHoles);
  hole2.setShell(&shellWithHoles);
  EXPECT_TRUE(hole1.hasShell());
  EXPECT_TRUE(hole2.hasShell());
  EXPECT_EQ(hole1.getShell(), &shellWithHoles);
  EXPECT_EQ(hole2.getShell(), &shellWithHoles);

  std::unique_ptr<Polygon> polyWithHoles =
      shellWithHoles.toPolygon(factory.get());
  ASSERT_NE(polyWithHoles, nullptr);
  EXPECT_EQ(polyWithHoles->getNumInteriorRing(), 2u);
  EXPECT_TRUE(polyWithHoles->isValid());
  EXPECT_DOUBLE_EQ(polyWithHoles->getArea(), 100.0 - 4.0 - 9.0);
  EXPECT_EQ(shellWithHoles.getRingPtr(), nullptr);
  EXPECT_EQ(hole1.getRingPtr(), nullptr);
  EXPECT_EQ(hole2.getRingPtr(), nullptr);

  // Direct addHole() also populates interior rings in toPolygon().
  std::vector<OverlayEdge*> directShellEdges = BuildLinkedRingEdges(
      graph,
      {Coordinate(20.0, 0.0), Coordinate(20.0, 10.0), Coordinate(30.0, 10.0),
       Coordinate(30.0, 0.0), Coordinate(20.0, 0.0)},
      false);
  OverlayEdgeRing directShell(directShellEdges.front(), factory.get());
  directShell.addHole(&orphanHole);
  std::unique_ptr<Polygon> directPoly = directShell.toPolygon(factory.get());
  ASSERT_NE(directPoly, nullptr);
  EXPECT_EQ(directPoly->getNumInteriorRing(), 1u);
  EXPECT_TRUE(directPoly->isValid());
  EXPECT_DOUBLE_EQ(directPoly->getArea(), 99.0);
}

TEST(OverlayEdgeRingTest, GetRingTransfersOwnership) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;
  std::vector<OverlayEdge*> edges =
      BuildLinkedRingEdges(graph,
                           {Coordinate(0.0, 0.0), Coordinate(0.0, 5.0),
                            Coordinate(5.0, 0.0), Coordinate(0.0, 0.0)},
                           false);

  OverlayEdgeRing ring(edges.front(), factory.get());
  const LinearRing* rawPtr = ring.getRingPtr();
  ASSERT_NE(rawPtr, nullptr);

  std::unique_ptr<LinearRing> owned = ring.getRing();
  EXPECT_EQ(owned.get(), rawPtr);
  EXPECT_EQ(ring.getRingPtr(), nullptr);
  EXPECT_EQ(ring.getRing(), nullptr);
}

TEST(OverlayEdgeRingTest, IsInRingInsideBoundaryOutsideAndCachesLocator) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;
  std::vector<OverlayEdge*> edges = BuildLinkedRingEdges(
      graph,
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
       Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
      false);

  OverlayEdgeRing ring(edges.front(), factory.get());
  EXPECT_TRUE(ring.isInRing(Coordinate(5.0, 5.0)));
  // Second and subsequent calls reuse the cached IndexedPointInAreaLocator.
  EXPECT_TRUE(ring.isInRing(Coordinate(0.0, 5.0)));
  EXPECT_TRUE(ring.isInRing(Coordinate(0.0, 0.0)));
  EXPECT_FALSE(ring.isInRing(Coordinate(15.0, 5.0)));
  EXPECT_FALSE(ring.isInRing(Coordinate(-1.0, -1.0)));
}

TEST(OverlayEdgeRingTest, FindEdgeRingContainingNestedNonConvexAndTouching) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;

  std::vector<OverlayEdge*> outerEdges = BuildLinkedRingEdges(
      graph,
      {Coordinate(0.0, 0.0), Coordinate(0.0, 100.0), Coordinate(100.0, 100.0),
       Coordinate(100.0, 0.0), Coordinate(0.0, 0.0)},
      false);
  std::vector<OverlayEdge*> middleEdges = BuildLinkedRingEdges(
      graph,
      {Coordinate(10.0, 10.0), Coordinate(10.0, 90.0), Coordinate(90.0, 90.0),
       Coordinate(90.0, 10.0), Coordinate(10.0, 10.0)},
      false);
  std::vector<OverlayEdge*> innerShellEdges = BuildLinkedRingEdges(
      graph,
      {Coordinate(15.0, 15.0), Coordinate(15.0, 85.0), Coordinate(85.0, 85.0),
       Coordinate(85.0, 15.0), Coordinate(15.0, 15.0)},
      false);
  std::vector<OverlayEdge*> overlappingEdges = BuildLinkedRingEdges(
      graph,
      {Coordinate(25.0, 25.0), Coordinate(25.0, 40.0), Coordinate(40.0, 40.0),
       Coordinate(40.0, 25.0), Coordinate(25.0, 25.0)},
      false);
  std::vector<OverlayEdge*> disjointEdges =
      BuildLinkedRingEdges(graph,
                           {Coordinate(200.0, 200.0), Coordinate(200.0, 300.0),
                            Coordinate(300.0, 300.0), Coordinate(300.0, 200.0),
                            Coordinate(200.0, 200.0)},
                           false);
  std::vector<OverlayEdge*> holeEdges = BuildLinkedRingEdges(
      graph,
      {Coordinate(20.0, 20.0), Coordinate(30.0, 20.0), Coordinate(30.0, 30.0),
       Coordinate(20.0, 30.0), Coordinate(20.0, 20.0)},
      true);
  std::vector<OverlayEdge*> sameEnvEdges = BuildLinkedRingEdges(
      graph,
      {Coordinate(20.0, 20.0), Coordinate(20.0, 30.0), Coordinate(30.0, 30.0),
       Coordinate(30.0, 20.0), Coordinate(20.0, 20.0)},
      false);
  // C-shaped shell with concave bay [20, 50] x [10, 40].
  std::vector<OverlayEdge*> cShapeEdges = BuildLinkedRingEdges(
      graph,
      {Coordinate(0.0, 0.0), Coordinate(0.0, 50.0), Coordinate(50.0, 50.0),
       Coordinate(50.0, 40.0), Coordinate(20.0, 40.0), Coordinate(20.0, 10.0),
       Coordinate(50.0, 10.0), Coordinate(50.0, 0.0), Coordinate(0.0, 0.0)},
      false);
  std::vector<OverlayEdge*> bayEdges = BuildLinkedRingEdges(
      graph,
      {Coordinate(30.0, 20.0), Coordinate(40.0, 20.0), Coordinate(40.0, 30.0),
       Coordinate(30.0, 30.0), Coordinate(30.0, 20.0)},
      true);
  // Triangle hole touching the outer corner (0, 0) at a single vertex.
  std::vector<OverlayEdge*> touchingHoleEdges =
      BuildLinkedRingEdges(graph,
                           {Coordinate(0.0, 0.0), Coordinate(6.0, 2.0),
                            Coordinate(2.0, 6.0), Coordinate(0.0, 0.0)},
                           true);

  OverlayEdgeRing outer(outerEdges.front(), factory.get());
  OverlayEdgeRing middle(middleEdges.front(), factory.get());
  OverlayEdgeRing innerShell(innerShellEdges.front(), factory.get());
  OverlayEdgeRing overlapping(overlappingEdges.front(), factory.get());
  OverlayEdgeRing disjoint(disjointEdges.front(), factory.get());
  OverlayEdgeRing hole(holeEdges.front(), factory.get());
  OverlayEdgeRing sameEnv(sameEnvEdges.front(), factory.get());
  OverlayEdgeRing cShape(cShapeEdges.front(), factory.get());
  OverlayEdgeRing bayRing(bayEdges.front(), factory.get());
  OverlayEdgeRing touchingHole(touchingHoleEdges.front(), factory.get());

  EXPECT_EQ(hole.findEdgeRingContaining({}), nullptr);
  EXPECT_EQ(
      hole.findEdgeRingContaining({&hole, &sameEnv, &disjoint, &overlapping}),
      nullptr);

  // Envelope of cShape contains bayRing's envelope, but point is outside
  // cShape.
  EXPECT_EQ(bayRing.findEdgeRingContaining({&cShape}), nullptr);

  // Innermost shell is selected regardless of candidate list order.
  EXPECT_EQ(hole.findEdgeRingContaining({&outer, &middle, &disjoint, &hole}),
            &middle);
  EXPECT_EQ(hole.findEdgeRingContaining({&middle, &outer}), &middle);
  EXPECT_EQ(hole.findEdgeRingContaining({&outer, &innerShell, &middle}),
            &innerShell);

  // Hole sharing a single vertex with outer still resolves outer as enclosing.
  EXPECT_EQ(touchingHole.findEdgeRingContaining({&outer}), &outer);
}

TEST(OverlayEdgeRingTest, ThrowsOnNullNextEdgeOrRevisitedEdge) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence pts0{CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)};
  CoordinateSequence pts1{CoordinateXY(10.0, 0.0), CoordinateXY(0.0, 10.0)};
  OverlayLabel label(0, Location::EXTERIOR, Location::INTERIOR, false);

  auto makeXY = [](double x, double y) {
    return CoordinateXYZM(x, y, DoubleNotANumber, DoubleNotANumber);
  };

  OverlayEdge e0(makeXY(0.0, 0.0), makeXY(10.0, 0.0), true, &label, &pts0);
  OverlayEdge e0Sym(makeXY(10.0, 0.0), makeXY(0.0, 0.0), false, &label, &pts0);
  e0.link(&e0Sym);

  // Missing nextResult (nullptr).
  EXPECT_THROW(
      { OverlayEdgeRing ring(&e0, factory.get()); }, TopologyException);

  // Fresh lasso e2 -> e3 -> e3 revisits e3 while searching for e2.
  OverlayEdge e2(makeXY(0.0, 0.0), makeXY(10.0, 0.0), true, &label, &pts0);
  OverlayEdge e2Sym(makeXY(10.0, 0.0), makeXY(0.0, 0.0), false, &label, &pts0);
  e2.link(&e2Sym);
  OverlayEdge e3(makeXY(10.0, 0.0), makeXY(0.0, 10.0), true, &label, &pts1);
  OverlayEdge e3Sym(makeXY(0.0, 10.0), makeXY(10.0, 0.0), false, &label, &pts1);
  e3.link(&e3Sym);
  e2.setNextResult(&e3);
  e3.setNextResult(&e3);

  EXPECT_THROW(
      { OverlayEdgeRing ring(&e2, factory.get()); }, TopologyException);
}

TEST(OverlayEdgeRingTest, SinglePointEdgeProducesEmptyRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence pts{CoordinateXY(1.0, 2.0)};
  OverlayLabel label(0);
  CoordinateXYZM pt(1.0, 2.0, DoubleNotANumber, DoubleNotANumber);
  OverlayEdge fwd(pt, pt, true, &label, &pts);
  OverlayEdge rev(pt, pt, false, &label, &pts);
  fwd.link(&rev);
  fwd.setNextResult(&fwd);

  OverlayEdgeRing emptyRing(&fwd, factory.get());
  EXPECT_FALSE(emptyRing.isHole());
  ASSERT_NE(emptyRing.getRingPtr(), nullptr);
  EXPECT_TRUE(emptyRing.getRingPtr()->isEmpty());
}

}  // namespace
}  // namespace geos_google
