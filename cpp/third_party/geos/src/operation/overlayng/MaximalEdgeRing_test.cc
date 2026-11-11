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

// Unit tests for third_party/geos/src/operation/overlayng/MaximalEdgeRing.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/MaximalEdgeRing.cpp

#include "third_party/geos/include/geos/operation/overlayng/MaximalEdgeRing.h"

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/operation/overlayng/Edge.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayEdge.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayEdgeRing.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayGraph.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayLabel.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::operation::overlayng::MaximalEdgeRing;
using geos::operation::overlayng::OverlayEdge;
using geos::operation::overlayng::OverlayEdgeRing;
using geos::operation::overlayng::OverlayGraph;
using geos::operation::overlayng::OverlayLabel;
using geos::operation::overlayng::OverlayNG;
using geos::util::TopologyException;

OverlayEdge* AddSegmentEdge(OverlayGraph& graph, const Coordinate& a,
                            const Coordinate& b, bool inResultArea,
                            bool isHole = false) {
  EdgeSourceInfo esi(0, 1, isHole);
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  seq->add(a);
  seq->add(b);
  Edge edge(std::move(seq), &esi);
  OverlayEdge* oe = graph.addEdge(&edge);
  if (inResultArea) {
    oe->markInResultArea();
  }
  return oe;
}

std::vector<OverlayEdge*> AddResultRing(
    OverlayGraph& graph, const std::vector<Coordinate>& closedRingCoords,
    bool isHole) {
  std::vector<OverlayEdge*> edges;
  const std::size_t segCount = closedRingCoords.size() - 1;
  edges.reserve(segCount);
  for (std::size_t i = 0; i < segCount; ++i) {
    edges.push_back(AddSegmentEdge(graph, closedRingCoords[i],
                                   closedRingCoords[i + 1],
                                   /*inResultArea=*/true, isHole));
  }
  return edges;
}

std::unique_ptr<Polygon> MakePolygon(const GeometryFactory* factory,
                                     const std::vector<Coordinate>& coords) {
  CoordinateSequence seq(0u, false, false);
  for (const Coordinate& c : coords) {
    seq.add(c);
  }
  return factory->createPolygon(std::move(seq));
}

TEST(MaximalEdgeRingTest, SimpleSquareLinksAndBuildsSingleShell) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;
  std::vector<OverlayEdge*> edges = AddResultRing(
      graph,
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
       Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
      /*isHole=*/false);

  for (OverlayEdge* e : edges) {
    MaximalEdgeRing::linkResultAreaMaxRingAtNode(e);
  }
  for (std::size_t i = 0; i < edges.size(); ++i) {
    EXPECT_TRUE(edges[i]->isResultMaxLinked());
    EXPECT_EQ(edges[i]->nextResultMax(), edges[(i + 1) % edges.size()]);
  }

  MaximalEdgeRing maxRing(edges[0]);
  for (OverlayEdge* e : edges) {
    EXPECT_EQ(e->getEdgeRingMax(), &maxRing);
  }

  std::vector<std::unique_ptr<OverlayEdgeRing>> minRings =
      maxRing.buildMinimalRings(factory.get());
  ASSERT_EQ(minRings.size(), 1u);
  EXPECT_FALSE(minRings[0]->isHole());

  std::unique_ptr<Polygon> poly = minRings[0]->toPolygon(factory.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_TRUE(poly->isValid());
  EXPECT_EQ(poly->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 100.0);
}

TEST(MaximalEdgeRingTest,
     ShellWithTouchingHoleAndNonResultEdgesSplitsIntoShellAndHole) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;

  // Outer CW square [0, 10] x [0, 10].
  std::vector<OverlayEdge*> shell = AddResultRing(
      graph,
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
       Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
      /*isHole=*/false);
  // Inner CCW triangle hole touching (0, 0) at a single vertex.
  std::vector<OverlayEdge*> hole =
      AddResultRing(graph,
                    {Coordinate(0.0, 0.0), Coordinate(6.0, 2.0),
                     Coordinate(2.0, 6.0), Coordinate(0.0, 0.0)},
                    /*isHole=*/true);

  // Non-result edges interspersed around (0, 0) and (0, 10) to exercise
  // skipping non-result and non-maxRing edges during linking.
  OverlayEdge* nonResult1 =
      AddSegmentEdge(graph, Coordinate(0.0, 0.0), Coordinate(8.0, 1.0), false);
  OverlayEdge* nonResult2 =
      AddSegmentEdge(graph, Coordinate(0.0, 0.0), Coordinate(1.0, 8.0), false);
  OverlayEdge* nonResultAtTop = AddSegmentEdge(graph, Coordinate(0.0, 10.0),
                                               Coordinate(-5.0, 10.0), false);

  // Link edges in result order, including a second call at the touch node
  // (0, 0) and starting at (0, 10) where the next CCW edge is non-result.
  for (OverlayEdge* e : shell) {
    MaximalEdgeRing::linkResultAreaMaxRingAtNode(e);
  }
  for (OverlayEdge* e : hole) {
    MaximalEdgeRing::linkResultAreaMaxRingAtNode(e);
  }

  MaximalEdgeRing maxRing(shell[0]);
  for (OverlayEdge* e : shell) {
    EXPECT_EQ(e->getEdgeRingMax(), &maxRing);
  }
  for (OverlayEdge* e : hole) {
    EXPECT_EQ(e->getEdgeRingMax(), &maxRing);
  }
  EXPECT_EQ(nonResult1->getEdgeRingMax(), nullptr);
  EXPECT_EQ(nonResult2->getEdgeRingMax(), nullptr);
  EXPECT_EQ(nonResultAtTop->getEdgeRingMax(), nullptr);

  std::vector<std::unique_ptr<OverlayEdgeRing>> minRings =
      maxRing.buildMinimalRings(factory.get());
  ASSERT_EQ(minRings.size(), 2u);

  OverlayEdgeRing* shellRing = nullptr;
  OverlayEdgeRing* holeRing = nullptr;
  for (const auto& ring : minRings) {
    if (ring->isHole()) {
      holeRing = ring.get();
    } else {
      shellRing = ring.get();
    }
  }
  ASSERT_NE(shellRing, nullptr);
  ASSERT_NE(holeRing, nullptr);

  holeRing->setShell(shellRing);
  std::unique_ptr<Polygon> poly = shellRing->toPolygon(factory.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_TRUE(poly->isValid());
  EXPECT_EQ(poly->getNumInteriorRing(), 1u);
  // Triangle (0,0)-(6,2)-(2,6) has area 0.5 * |6*6 - 2*2| = 16.
  EXPECT_DOUBLE_EQ(poly->getArea(), 100.0 - 16.0);
}

TEST(MaximalEdgeRingTest, TwoSeparateMaxRingsSharingNodeBuildIndependently) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;

  // Two CW triangles touching only at (0, 0) in opposite quadrants.
  std::vector<OverlayEdge*> tri1 =
      AddResultRing(graph,
                    {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0),
                     Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
                    false);
  std::vector<OverlayEdge*> tri2 =
      AddResultRing(graph,
                    {Coordinate(0.0, 0.0), Coordinate(0.0, -10.0),
                     Coordinate(-10.0, 0.0), Coordinate(0.0, 0.0)},
                    false);

  for (OverlayEdge* e : tri1) {
    MaximalEdgeRing::linkResultAreaMaxRingAtNode(e);
  }
  for (OverlayEdge* e : tri2) {
    MaximalEdgeRing::linkResultAreaMaxRingAtNode(e);
  }

  MaximalEdgeRing maxRing1(tri1[0]);
  MaximalEdgeRing maxRing2(tri2[0]);
  for (OverlayEdge* e : tri1) {
    EXPECT_EQ(e->getEdgeRingMax(), &maxRing1);
  }
  for (OverlayEdge* e : tri2) {
    EXPECT_EQ(e->getEdgeRingMax(), &maxRing2);
  }

  // Building minimal rings for maxRing1 skips maxRing2 edges at (0, 0);
  // then building maxRing2 sees already-linked maxRing1 edges at (0, 0)
  // without mistaking them for maxRing2 edges.
  std::vector<std::unique_ptr<OverlayEdgeRing>> minRings1 =
      maxRing1.buildMinimalRings(factory.get());
  std::vector<std::unique_ptr<OverlayEdgeRing>> minRings2 =
      maxRing2.buildMinimalRings(factory.get());

  ASSERT_EQ(minRings1.size(), 1u);
  ASSERT_EQ(minRings2.size(), 1u);
  EXPECT_FALSE(minRings1[0]->isHole());
  EXPECT_FALSE(minRings2[0]->isHole());

  std::unique_ptr<Polygon> poly1 = minRings1[0]->toPolygon(factory.get());
  std::unique_ptr<Polygon> poly2 = minRings2[0]->toPolygon(factory.get());
  EXPECT_TRUE(poly1->isValid());
  EXPECT_TRUE(poly2->isValid());
  EXPECT_DOUBLE_EQ(poly1->getArea(), 50.0);
  EXPECT_DOUBLE_EQ(poly2->getArea(), 50.0);
}

TEST(MaximalEdgeRingTest, ShellWithTwoTouchingHolesSplitsIntoThreeRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;

  std::vector<OverlayEdge*> shell = AddResultRing(
      graph,
      {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0), Coordinate(10.0, 10.0),
       Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
      false);
  std::vector<OverlayEdge*> hole1 =
      AddResultRing(graph,
                    {Coordinate(0.0, 0.0), Coordinate(8.0, 1.0),
                     Coordinate(6.0, 3.0), Coordinate(0.0, 0.0)},
                    true);
  std::vector<OverlayEdge*> hole2 =
      AddResultRing(graph,
                    {Coordinate(0.0, 0.0), Coordinate(3.0, 6.0),
                     Coordinate(1.0, 8.0), Coordinate(0.0, 0.0)},
                    true);

  for (OverlayEdge* e : shell) {
    MaximalEdgeRing::linkResultAreaMaxRingAtNode(e);
  }
  for (OverlayEdge* e : hole1) {
    MaximalEdgeRing::linkResultAreaMaxRingAtNode(e);
  }
  for (OverlayEdge* e : hole2) {
    MaximalEdgeRing::linkResultAreaMaxRingAtNode(e);
  }

  MaximalEdgeRing maxRing(shell[0]);
  std::vector<std::unique_ptr<OverlayEdgeRing>> minRings =
      maxRing.buildMinimalRings(factory.get());
  ASSERT_EQ(minRings.size(), 3u);

  OverlayEdgeRing* shellRing = nullptr;
  std::vector<OverlayEdgeRing*> holeRings;
  for (const auto& ring : minRings) {
    if (ring->isHole()) {
      holeRings.push_back(ring.get());
    } else {
      shellRing = ring.get();
    }
  }
  ASSERT_NE(shellRing, nullptr);
  ASSERT_EQ(holeRings.size(), 2u);
  for (OverlayEdgeRing* h : holeRings) {
    h->setShell(shellRing);
  }

  std::unique_ptr<Polygon> poly = shellRing->toPolygon(factory.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_TRUE(poly->isValid());
  EXPECT_EQ(poly->getNumInteriorRing(), 2u);
  // Each hole triangle has area 0.5 * |8*3 - 1*6| = 9.
  EXPECT_DOUBLE_EQ(poly->getArea(), 100.0 - 9.0 - 9.0);
}

TEST(MaximalEdgeRingTest,
     LinkResultAreaMaxRingAtNodeThrowsWhenNoOutgoingEdgeFound) {
  OverlayGraph graph;
  OverlayEdge* e = AddSegmentEdge(graph, Coordinate(0.0, 0.0),
                                  Coordinate(10.0, 0.0), /*inResultArea=*/true);
  // Violating precondition (both edge and sym marked in result area) leaves
  // the state machine in STATE_LINK_OUTGOING at loop termination.
  e->markInResultAreaBoth();

  EXPECT_THROW(MaximalEdgeRing::linkResultAreaMaxRingAtNode(e),
               TopologyException);
}

TEST(MaximalEdgeRingTest, ConstructorThrowsOnNullMissingAndRevisitedEdges) {
  // Null start edge.
  EXPECT_THROW(MaximalEdgeRing{nullptr}, TopologyException);

  // Missing nextResultMax link (nullptr).
  CoordinateSequence pts0{CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)};
  CoordinateSequence pts1{CoordinateXY(10.0, 0.0), CoordinateXY(0.0, 10.0)};
  OverlayLabel label(0, Location::EXTERIOR, Location::INTERIOR, false);
  auto makeXY = [](double x, double y) {
    return CoordinateXYZM(x, y, DoubleNotANumber, DoubleNotANumber);
  };

  OverlayEdge e0(makeXY(0.0, 0.0), makeXY(10.0, 0.0), true, &label, &pts0);
  OverlayEdge e0Sym(makeXY(10.0, 0.0), makeXY(0.0, 0.0), false, &label, &pts0);
  e0.link(&e0Sym);
  EXPECT_THROW(MaximalEdgeRing{&e0}, TopologyException);

  // Lasso e0 -> e1 -> e1 revisits e1 while searching for e0.
  OverlayEdge e1(makeXY(10.0, 0.0), makeXY(0.0, 10.0), true, &label, &pts1);
  OverlayEdge e1Sym(makeXY(0.0, 10.0), makeXY(10.0, 0.0), false, &label, &pts1);
  e1.link(&e1Sym);
  e0.setNextResultMax(&e1);
  e1.setNextResultMax(&e1);
  EXPECT_THROW(MaximalEdgeRing{&e0}, TopologyException);
}

TEST(MaximalEdgeRingTest, BuildMinimalRingsThrowsOnUnmatchedEdge) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence pts0{CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 10.0)};
  CoordinateSequence pts1{CoordinateXY(0.0, 10.0), CoordinateXY(10.0, 0.0)};
  CoordinateSequence pts2{CoordinateXY(10.0, 0.0), CoordinateXY(0.0, 0.0)};
  OverlayLabel label(0, Location::EXTERIOR, Location::INTERIOR, false);
  auto makeXY = [](double x, double y) {
    return CoordinateXYZM(x, y, DoubleNotANumber, DoubleNotANumber);
  };

  // Link each half-edge only to its own sym without inserting into node stars,
  // so no incoming max-ring edge exists in e0->oNextOE().
  OverlayEdge e0(makeXY(0.0, 0.0), makeXY(0.0, 10.0), true, &label, &pts0);
  OverlayEdge e0Sym(makeXY(0.0, 10.0), makeXY(0.0, 0.0), false, &label, &pts0);
  e0.link(&e0Sym);

  OverlayEdge e1(makeXY(0.0, 10.0), makeXY(10.0, 0.0), true, &label, &pts1);
  OverlayEdge e1Sym(makeXY(10.0, 0.0), makeXY(0.0, 10.0), false, &label, &pts1);
  e1.link(&e1Sym);

  OverlayEdge e2(makeXY(10.0, 0.0), makeXY(0.0, 0.0), true, &label, &pts2);
  OverlayEdge e2Sym(makeXY(0.0, 0.0), makeXY(10.0, 0.0), false, &label, &pts2);
  e2.link(&e2Sym);

  e0.setNextResultMax(&e1);
  e1.setNextResultMax(&e2);
  e2.setNextResultMax(&e0);

  MaximalEdgeRing maxRing(&e0);
  EXPECT_THROW(maxRing.buildMinimalRings(factory.get()), TopologyException);
}

TEST(MaximalEdgeRingTest, StreamInsertionFormatsClosedAndIncompleteRing) {
  OverlayGraph graph;
  std::vector<OverlayEdge*> edges =
      AddResultRing(graph,
                    {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0),
                     Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)},
                    false);
  for (OverlayEdge* e : edges) {
    MaximalEdgeRing::linkResultAreaMaxRingAtNode(e);
  }

  MaximalEdgeRing maxRing(edges[0]);
  std::ostringstream closedOss;
  closedOss << maxRing;
  EXPECT_EQ(closedOss.str(), "LINESTRING (0 0, 0 10, 10 0, 0 10)");

  // Break the cycle to exercise the early-break guard on nextResultMax() ==
  // nullptr in operator<<.
  edges[1]->setNextResultMax(nullptr);
  std::ostringstream brokenOss;
  brokenOss << maxRing;
  EXPECT_EQ(brokenOss.str(), "LINESTRING (0 0, 0 10, 10 0)");
}

TEST(MaximalEdgeRingTest, OverlayNgDifferenceWithTouchingHoleUsesMaximalRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> outer =
      MakePolygon(factory.get(), {Coordinate(0.0, 0.0), Coordinate(0.0, 10.0),
                                  Coordinate(10.0, 10.0), Coordinate(10.0, 0.0),
                                  Coordinate(0.0, 0.0)});
  std::unique_ptr<Polygon> diamond =
      MakePolygon(factory.get(), {Coordinate(5.0, 0.0), Coordinate(8.0, 4.0),
                                  Coordinate(5.0, 8.0), Coordinate(2.0, 4.0),
                                  Coordinate(5.0, 0.0)});

  std::unique_ptr<Geometry> diff =
      OverlayNG::overlay(outer.get(), diamond.get(), OverlayNG::DIFFERENCE);
  ASSERT_NE(diff, nullptr);
  EXPECT_TRUE(diff->isValid());
  EXPECT_EQ(diff->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  const auto* resultPoly = dynamic_cast<const Polygon*>(diff.get());
  ASSERT_NE(resultPoly, nullptr);
  EXPECT_EQ(resultPoly->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(resultPoly->getArea(), 100.0 - 24.0);
}

}  // namespace
}  // namespace geos_google
