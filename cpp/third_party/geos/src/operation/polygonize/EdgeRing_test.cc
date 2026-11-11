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

// Unit tests for third_party/geos/src/operation/polygonize/EdgeRing.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/polygonize/EdgeRing.cpp

#include "third_party/geos/include/geos/operation/polygonize/EdgeRing.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/operation/polygonize/HoleAssigner.h"
#include "third_party/geos/include/geos/operation/polygonize/PolygonizeDirectedEdge.h"
#include "third_party/geos/include/geos/operation/polygonize/PolygonizeEdge.h"
#include "third_party/geos/include/geos/operation/polygonize/PolygonizeGraph.h"
#include "third_party/geos/include/geos/planargraph/Node.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Polygon;
using geos::operation::polygonize::EdgeRing;
using geos::operation::polygonize::HoleAssigner;
using geos::operation::polygonize::PolygonizeDirectedEdge;
using geos::operation::polygonize::PolygonizeEdge;
using geos::operation::polygonize::PolygonizeGraph;
using geos::planargraph::Node;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

std::unique_ptr<LineString> MakeLine(const GeometryFactory* factory,
                                     const std::vector<Coordinate>& coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, 2u);
  for (const Coordinate& pt : coords) {
    seq->add(pt);
  }
  return factory->createLineString(std::move(seq));
}

// Owns synthetic planargraph components for isolated EdgeRing unit tests.
class SyntheticGraph {
 public:
  explicit SyntheticGraph(const GeometryFactory* factory) : factory_(factory) {}

  Node* AddNode(const Coordinate& pt) {
    auto node = std::make_unique<Node>(pt);
    Node* raw = node.get();
    nodes_.push_back(std::move(node));
    return raw;
  }

  // Adds a directed edge pair for `coords` between `from` and `to`, returning
  // the forward directed edge (sym is reachable via getSym()).
  PolygonizeDirectedEdge* AddEdge(Node* from, Node* to,
                                  const std::vector<Coordinate>& coords) {
    auto line = MakeLine(factory_, coords);
    const CoordinateSequence* pts = line->getCoordinatesRO();
    auto de_fwd =
        std::make_unique<PolygonizeDirectedEdge>(from, to, pts->getAt(1), true);
    auto de_rev = std::make_unique<PolygonizeDirectedEdge>(
        to, from, pts->getAt(pts->getSize() - 2), false);
    auto edge = std::make_unique<PolygonizeEdge>(line.get());
    edge->setDirectedEdges(de_fwd.get(), de_rev.get());

    PolygonizeDirectedEdge* raw_fwd = de_fwd.get();
    lines_.push_back(std::move(line));
    dir_edges_.push_back(std::move(de_fwd));
    dir_edges_.push_back(std::move(de_rev));
    edges_.push_back(std::move(edge));
    return raw_fwd;
  }

  EdgeRing* MakeRingFromLoop(const std::vector<Coordinate>& coords) {
    Node* n = AddNode(coords.front());
    PolygonizeDirectedEdge* fwd = AddEdge(n, n, coords);
    fwd->setNext(fwd);
    auto* rev = static_cast<PolygonizeDirectedEdge*>(fwd->getSym());
    rev->setNext(rev);
    auto er = std::make_unique<EdgeRing>(factory_);
    er->build(fwd);
    er->computeValid();
    er->computeHole();
    EdgeRing* raw = er.get();
    rings_.push_back(std::move(er));
    return raw;
  }

  EdgeRing* MakeCwSquare(double min_x, double min_y, double max_x,
                         double max_y) {
    return MakeRingFromLoop({
        Coordinate(min_x, min_y),
        Coordinate(min_x, max_y),
        Coordinate(max_x, max_y),
        Coordinate(max_x, min_y),
        Coordinate(min_x, min_y),
    });
  }

  EdgeRing* MakeCcwSquare(double min_x, double min_y, double max_x,
                          double max_y) {
    return MakeRingFromLoop({
        Coordinate(min_x, min_y),
        Coordinate(max_x, min_y),
        Coordinate(max_x, max_y),
        Coordinate(min_x, max_y),
        Coordinate(min_x, min_y),
    });
  }

 private:
  const GeometryFactory* factory_;
  std::vector<std::unique_ptr<LineString>> lines_;
  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<std::unique_ptr<PolygonizeDirectedEdge>> dir_edges_;
  std::vector<std::unique_ptr<PolygonizeEdge>> edges_;
  std::vector<std::unique_ptr<EdgeRing>> rings_;
};

TEST(EdgeRingTest, InitialStateDefaults) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  EdgeRing ring(factory.get());

  EXPECT_THAT(ring.getEdges(), IsEmpty());
  EXPECT_FALSE(ring.isHole());
  EXPECT_FALSE(ring.isValid());
  EXPECT_FALSE(ring.isProcessed());
  EXPECT_FALSE(ring.isIncludedSet());
  EXPECT_FALSE(ring.isIncluded());
  EXPECT_FALSE(ring.hasShell());
  EXPECT_EQ(ring.getShell(), &ring);
  EXPECT_FALSE(ring.isOuterHole());
  EXPECT_FALSE(ring.isOuterShell());
  EXPECT_EQ(ring.getOuterHole(), nullptr);
}

TEST(EdgeRingTest, StateFlagsAndShellAccessors) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  SyntheticGraph graph(factory.get());

  EdgeRing* shell = graph.MakeCwSquare(0.0, 0.0, 10.0, 10.0);
  EdgeRing* hole = graph.MakeCcwSquare(2.0, 2.0, 8.0, 8.0);

  EXPECT_FALSE(shell->isHole());
  EXPECT_TRUE(hole->isHole());
  EXPECT_EQ(shell->getShell(), shell);
  EXPECT_EQ(hole->getShell(), nullptr);
  EXPECT_FALSE(shell->isOuterHole());
  EXPECT_TRUE(hole->isOuterHole());

  hole->setProcessed(true);
  EXPECT_TRUE(hole->isProcessed());
  hole->setProcessed(false);
  EXPECT_FALSE(hole->isProcessed());

  shell->setIncluded(false);
  EXPECT_TRUE(shell->isIncludedSet());
  EXPECT_FALSE(shell->isIncluded());
  shell->setIncluded(true);
  EXPECT_TRUE(shell->isIncludedSet());
  EXPECT_TRUE(shell->isIncluded());

  hole->setShell(shell);
  EXPECT_TRUE(hole->hasShell());
  EXPECT_EQ(hole->getShell(), shell);
  EXPECT_FALSE(hole->isOuterHole());
}

TEST(EdgeRingTest, FindDirEdgesInRingSingleAndMultiEdgeCycles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  SyntheticGraph graph(factory.get());

  Node* n0 = graph.AddNode(Coordinate(0.0, 0.0));
  Node* n1 = graph.AddNode(Coordinate(10.0, 0.0));
  Node* n2 = graph.AddNode(Coordinate(5.0, 8.0));

  PolygonizeDirectedEdge* de0 =
      graph.AddEdge(n0, n1, {Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)});
  PolygonizeDirectedEdge* de1 =
      graph.AddEdge(n1, n2, {Coordinate(10.0, 0.0), Coordinate(5.0, 8.0)});
  PolygonizeDirectedEdge* de2 =
      graph.AddEdge(n2, n0, {Coordinate(5.0, 8.0), Coordinate(0.0, 0.0)});

  de0->setNext(de1);
  de1->setNext(de2);
  de2->setNext(de0);

  EXPECT_THAT(EdgeRing::findDirEdgesInRing(de0), ElementsAre(de0, de1, de2));
  EXPECT_THAT(EdgeRing::findDirEdgesInRing(de1), ElementsAre(de1, de2, de0));

  // Self-loop single-edge ring.
  PolygonizeDirectedEdge* loop =
      graph.AddEdge(n0, n0,
                    {Coordinate(0.0, 0.0), Coordinate(0.0, 2.0),
                     Coordinate(2.0, 0.0), Coordinate(0.0, 0.0)});
  loop->setNext(loop);
  EXPECT_THAT(EdgeRing::findDirEdgesInRing(loop), ElementsAre(loop));
}

TEST(EdgeRingTest, BuildAndAddAssembleMixedDirectionEdges) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  SyntheticGraph graph(factory.get());

  Node* n0 = graph.AddNode(Coordinate(0.0, 0.0));
  Node* n1 = graph.AddNode(Coordinate(0.0, 10.0));
  Node* n2 = graph.AddNode(Coordinate(10.0, 10.0));
  Node* n3 = graph.AddNode(Coordinate(10.0, 0.0));

  // Forward edge with an intermediate vertex.
  PolygonizeDirectedEdge* e0 = graph.AddEdge(
      n0, n1,
      {Coordinate(0.0, 0.0), Coordinate(0.0, 5.0), Coordinate(0.0, 10.0)});
  // Add backwards edge from n2 -> n1 so its sym traverses n1 -> n2 (!dir).
  PolygonizeDirectedEdge* e1_pair = graph.AddEdge(
      n2, n1,
      {Coordinate(10.0, 10.0), Coordinate(5.0, 10.0), Coordinate(0.0, 10.0)});
  auto* e1 = static_cast<PolygonizeDirectedEdge*>(e1_pair->getSym());
  // Forward edge n2 -> n3.
  PolygonizeDirectedEdge* e2 =
      graph.AddEdge(n2, n3, {Coordinate(10.0, 10.0), Coordinate(10.0, 0.0)});
  // Backwards edge n0 -> n3 so its sym traverses n3 -> n0 (!dir).
  PolygonizeDirectedEdge* e3_pair =
      graph.AddEdge(n0, n3, {Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)});
  auto* e3 = static_cast<PolygonizeDirectedEdge*>(e3_pair->getSym());

  e0->setNext(e1);
  e1->setNext(e2);
  e2->setNext(e3);
  e3->setNext(e0);

  EdgeRing ring(factory.get());
  ring.build(e0);

  EXPECT_THAT(ring.getEdges(), ElementsAre(e0, e1, e2, e3));
  EXPECT_TRUE(e0->isInRing());
  EXPECT_TRUE(e1->isInRing());
  EXPECT_TRUE(e2->isInRing());
  EXPECT_TRUE(e3->isInRing());
  EXPECT_EQ(e0->getRing(), &ring);
  EXPECT_EQ(e3->getRing(), &ring);

  ring.computeValid();
  ring.computeHole();
  EXPECT_TRUE(ring.isValid());
  EXPECT_FALSE(ring.isHole());

  std::unique_ptr<LineString> ls = ring.getLineString();
  ASSERT_NE(ls, nullptr);
  EXPECT_EQ(ls->getNumPoints(), 7u);
  EXPECT_TRUE(ls->isClosed());

  // Repeated getRingInternal() returns cached ring pointer.
  LinearRing* r1 = ring.getRingInternal();
  LinearRing* r2 = ring.getRingInternal();
  ASSERT_NE(r1, nullptr);
  EXPECT_EQ(r1, r2);
  EXPECT_EQ(r1->getNumPoints(), 7u);

  // Verify add() directly on another EdgeRing.
  EdgeRing manual(factory.get());
  manual.add(e0);
  EXPECT_THAT(manual.getEdges(), ElementsAre(e0));
}

TEST(EdgeRingTest, IsInListAndPtNotInList) {
  const CoordinateSequence empty_pts(0u, 2u);
  const CoordinateSequence pts = {
      Coordinate(0.0, 0.0, 1.0),
      Coordinate(10.0, 0.0, 2.0),
      Coordinate(10.0, 10.0, 3.0),
  };

  EXPECT_FALSE(EdgeRing::isInList(Coordinate(0.0, 0.0), &empty_pts));
  EXPECT_TRUE(EdgeRing::isInList(Coordinate(0.0, 0.0), &pts));
  // 2D equality ignores Z.
  EXPECT_TRUE(EdgeRing::isInList(Coordinate(10.0, 0.0, 99.0), &pts));
  EXPECT_TRUE(EdgeRing::isInList(Coordinate(10.0, 10.0), &pts));
  EXPECT_FALSE(EdgeRing::isInList(Coordinate(5.0, 5.0), &pts));

  const CoordinateSequence test_mixed = {
      Coordinate(0.0, 0.0),
      Coordinate(10.0, 0.0),
      Coordinate(5.0, 5.0),
      Coordinate(10.0, 10.0),
  };
  const Coordinate& found = EdgeRing::ptNotInList(&test_mixed, &pts);
  EXPECT_FALSE(found.isNull());
  EXPECT_TRUE(found.equals2D(Coordinate(5.0, 5.0)));

  const CoordinateSequence test_subset = {
      Coordinate(10.0, 10.0),
      Coordinate(0.0, 0.0),
  };
  EXPECT_TRUE(EdgeRing::ptNotInList(&test_subset, &pts).isNull());
  EXPECT_TRUE(EdgeRing::ptNotInList(&empty_pts, &pts).isNull());
}

TEST(EdgeRingTest, ComputeValidHandlesTooFewPointsAndSelfIntersections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  SyntheticGraph graph(factory.get());

  // 1. Two edges forming a collapsed 3-point closed ring (<= 3 coordinates).
  Node* n0 = graph.AddNode(Coordinate(0.0, 0.0));
  Node* n1 = graph.AddNode(Coordinate(5.0, 0.0));
  PolygonizeDirectedEdge* fwd =
      graph.AddEdge(n0, n1, {Coordinate(0.0, 0.0), Coordinate(5.0, 0.0)});
  auto* rev = static_cast<PolygonizeDirectedEdge*>(fwd->getSym());
  fwd->setNext(rev);
  rev->setNext(fwd);

  EdgeRing collapsed(factory.get());
  collapsed.build(fwd);
  collapsed.computeValid();
  EXPECT_FALSE(collapsed.isValid());
  EXPECT_EQ(collapsed.getLineString()->getNumPoints(), 3u);
  // LinearRing::MINIMUM_VALID_SIZE is 3, so a closed 3-point ring constructs
  // a non-null LinearRing, though IsValidOp still reports it invalid.
  LinearRing* collapsed_lr = collapsed.getRingInternal();
  ASSERT_NE(collapsed_lr, nullptr);
  EXPECT_EQ(collapsed_lr->getNumPoints(), 3u);
  EXPECT_FALSE(collapsed_lr->isValid());

  // 1b. Two-point unclosed edge (<= 3 coordinates, fails LinearRing).
  PolygonizeDirectedEdge* seg_edge =
      graph.AddEdge(n0, n1, {Coordinate(0.0, 0.0), Coordinate(5.0, 0.0)});
  EdgeRing two_pt(factory.get());
  two_pt.add(seg_edge);
  two_pt.computeValid();
  EXPECT_FALSE(two_pt.isValid());
  EXPECT_EQ(two_pt.getLineString()->getNumPoints(), 2u);
  EXPECT_EQ(two_pt.getRingInternal(), nullptr);

  // 2. Empty EdgeRing has 0 points (<= 3).
  EdgeRing empty_ring(factory.get());
  empty_ring.computeValid();
  EXPECT_FALSE(empty_ring.isValid());
  EXPECT_TRUE(empty_ring.getLineString()->isEmpty());
  LinearRing* empty_lr = empty_ring.getRingInternal();
  ASSERT_NE(empty_lr, nullptr);
  EXPECT_TRUE(empty_lr->isEmpty());
  std::unique_ptr<Polygon> empty_poly = empty_ring.getPolygon();
  ASSERT_NE(empty_poly, nullptr);
  EXPECT_TRUE(empty_poly->isEmpty());
  EXPECT_TRUE(empty_poly->isValid());

  // 3. Self-intersecting bow-tie with 5 points (> 3, closed, invalid ring).
  EdgeRing* bowtie = graph.MakeRingFromLoop({
      Coordinate(0.0, 0.0),
      Coordinate(10.0, 10.0),
      Coordinate(10.0, 0.0),
      Coordinate(0.0, 10.0),
      Coordinate(0.0, 0.0),
  });
  EXPECT_FALSE(bowtie->isValid());
  ASSERT_NE(bowtie->getRingInternal(), nullptr);

  // 4. Valid triangle (4 points).
  EdgeRing* tri = graph.MakeRingFromLoop({
      Coordinate(0.0, 0.0),
      Coordinate(0.0, 10.0),
      Coordinate(10.0, 0.0),
      Coordinate(0.0, 0.0),
  });
  EXPECT_TRUE(tri->isValid());
  EXPECT_FALSE(tri->isHole());
}

TEST(EdgeRingTest, UnclosedRingCatchesIllegalArgumentAndReturnsNullptr) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  SyntheticGraph graph(factory.get());

  Node* n0 = graph.AddNode(Coordinate(0.0, 0.0));
  Node* n1 = graph.AddNode(Coordinate(5.0, 5.0));
  PolygonizeDirectedEdge* open_edge = graph.AddEdge(
      n0, n1,
      {Coordinate(0.0, 0.0), Coordinate(5.0, 0.0), Coordinate(5.0, 5.0)});

  EdgeRing unclosed(factory.get());
  unclosed.add(open_edge);

  EXPECT_EQ(unclosed.getRingInternal(), nullptr);
  EXPECT_EQ(unclosed.getRingOwnership(), nullptr);

  EdgeRing* outer = graph.MakeCwSquare(-10.0, -10.0, 20.0, 20.0);
  std::vector<EdgeRing*> candidates = {outer};
  EXPECT_EQ(unclosed.findEdgeRingContaining(candidates), nullptr);

  std::unique_ptr<LineString> ls = unclosed.getLineString();
  ASSERT_NE(ls, nullptr);
  EXPECT_EQ(ls->getNumPoints(), 3u);

  // 4-point unclosed edge still fails LinearRing construction.
  Node* n2 = graph.AddNode(Coordinate(0.0, 5.0));
  PolygonizeDirectedEdge* open_edge_4 =
      graph.AddEdge(n0, n2,
                    {Coordinate(0.0, 0.0), Coordinate(5.0, 0.0),
                     Coordinate(5.0, 5.0), Coordinate(0.0, 5.0)});
  EdgeRing unclosed4(factory.get());
  unclosed4.add(open_edge_4);
  EXPECT_EQ(unclosed4.getRingInternal(), nullptr);
  EXPECT_EQ(unclosed4.getLineString()->getNumPoints(), 4u);
}

TEST(EdgeRingTest, AddHoleAndGetPolygonOwnership) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  SyntheticGraph graph(factory.get());

  // Polygon with 0 holes.
  EdgeRing* plain_shell = graph.MakeCwSquare(0.0, 0.0, 10.0, 10.0);
  std::unique_ptr<Polygon> plain_poly = plain_shell->getPolygon();
  ASSERT_NE(plain_poly, nullptr);
  EXPECT_EQ(plain_poly->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(plain_poly->getArea(), 100.0);
  EXPECT_TRUE(plain_poly->isValid());

  // Polygon with holes added via both addHole(EdgeRing*) and
  // addHole(LinearRing*).
  EdgeRing* shell = graph.MakeCwSquare(0.0, 0.0, 20.0, 20.0);
  EdgeRing* hole_er = graph.MakeCcwSquare(2.0, 2.0, 6.0, 6.0);
  EdgeRing* standalone_hole = graph.MakeCcwSquare(10.0, 10.0, 16.0, 16.0);
  std::unique_ptr<LinearRing> owned_lr = standalone_hole->getRingOwnership();
  ASSERT_NE(owned_lr, nullptr);
  const LinearRing* orig_lr_ptr = owned_lr.get();
  // Calling getRingInternal() after getRingOwnership() lazily regenerates ring.
  LinearRing* regenerated_lr = standalone_hole->getRingInternal();
  ASSERT_NE(regenerated_lr, nullptr);
  EXPECT_NE(regenerated_lr, orig_lr_ptr);

  shell->addHole(hole_er);
  shell->addHole(owned_lr.release());
  EXPECT_TRUE(hole_er->hasShell());
  EXPECT_EQ(hole_er->getShell(), shell);

  std::unique_ptr<Polygon> poly = shell->getPolygon();
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getNumInteriorRing(), 2u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 400.0 - 16.0 - 36.0);
  EXPECT_TRUE(poly->isValid());
}

TEST(EdgeRingTest, IsInRingAndFindEdgeRingContainingAllBranches) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  SyntheticGraph graph(factory.get());

  EdgeRing* outer = graph.MakeCwSquare(0.0, 0.0, 100.0, 100.0);
  EdgeRing* middle = graph.MakeCwSquare(10.0, 10.0, 90.0, 90.0);
  EdgeRing* disjoint = graph.MakeCwSquare(200.0, 200.0, 220.0, 220.0);
  EdgeRing* partial = graph.MakeCwSquare(15.0, 15.0, 50.0, 50.0);
  EdgeRing* concave = graph.MakeRingFromLoop({
      Coordinate(0.0, 0.0),
      Coordinate(0.0, 100.0),
      Coordinate(100.0, 100.0),
      Coordinate(100.0, 80.0),
      Coordinate(20.0, 80.0),
      Coordinate(20.0, 20.0),
      Coordinate(100.0, 20.0),
      Coordinate(100.0, 0.0),
      Coordinate(0.0, 0.0),
  });
  EdgeRing* same_bbox_as_hole = graph.MakeCwSquare(30.0, 30.0, 70.0, 70.0);
  EdgeRing* target_hole = graph.MakeCcwSquare(30.0, 30.0, 70.0, 70.0);

  EXPECT_TRUE(outer->isInRing(Coordinate(50.0, 50.0)));
  EXPECT_TRUE(outer->isInRing(Coordinate(0.0, 50.0)));
  EXPECT_FALSE(outer->isInRing(Coordinate(-1.0, 50.0)));

  // Empty candidate list returns nullptr.
  std::vector<EdgeRing*> empty_list;
  EXPECT_EQ(target_hole->findEdgeRingContaining(empty_list), nullptr);

  // Non-empty candidates that all fail containment also return nullptr.
  std::vector<EdgeRing*> non_containing = {
      target_hole, same_bbox_as_hole, disjoint, partial, concave,
  };
  EXPECT_EQ(target_hole->findEdgeRingContaining(non_containing), nullptr);

  // Outer before middle: updates minRing when a smaller containing ring
  // appears. Also tests equal envelope skip, disjoint/partial skip, and concave
  // skip.
  std::vector<EdgeRing*> outer_first = {
      target_hole, same_bbox_as_hole, disjoint, partial, concave, outer, middle,
  };
  EXPECT_EQ(target_hole->findEdgeRingContaining(outer_first), middle);

  // Middle before outer: retains middle when larger outer ring appears later.
  std::vector<EdgeRing*> inner_first = {middle, outer};
  EXPECT_EQ(target_hole->findEdgeRingContaining(inner_first), middle);

  // Hole sharing vertex (10,10) with `middle` skips the shared vertex in
  // ptNotInList and picks (50,30) inside `middle`.
  EdgeRing* touching_hole = graph.MakeRingFromLoop({
      Coordinate(10.0, 10.0),
      Coordinate(50.0, 30.0),
      Coordinate(30.0, 50.0),
      Coordinate(10.0, 10.0),
  });
  std::vector<EdgeRing*> nested_shells = {outer, middle};
  EXPECT_EQ(touching_hole->findEdgeRingContaining(nested_shells), middle);
}

TEST(EdgeRingTest,
     OuterHoleAndUpdateIncludedRecursiveWithNestedAndAdjacentShells) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::vector<std::unique_ptr<LineString>> lines;
  auto add_seg = [&](double x0, double y0, double x1, double y1,
                     PolygonizeGraph& g) {
    lines.push_back(
        MakeLine(factory.get(), {Coordinate(x0, y0), Coordinate(x1, y1)}));
    g.addEdge(lines.back().get());
  };

  // Component 1: three concentric squares [0,60], [10,50], [20,40].
  PolygonizeGraph nested_graph(factory.get());
  auto add_box = [&](double a, double b, PolygonizeGraph& g) {
    add_seg(a, a, a, b, g);
    add_seg(a, b, b, b, g);
    add_seg(b, b, b, a, g);
    add_seg(b, a, a, a, g);
  };
  add_box(0.0, 60.0, nested_graph);
  add_box(10.0, 50.0, nested_graph);
  add_box(20.0, 40.0, nested_graph);

  std::vector<EdgeRing*> rings;
  nested_graph.getEdgeRings(rings);
  ASSERT_EQ(rings.size(), 6u);

  std::vector<EdgeRing*> holes;
  std::vector<EdgeRing*> shells;
  for (EdgeRing* er : rings) {
    er->computeValid();
    er->computeHole();
    if (er->isHole()) {
      holes.push_back(er);
    } else {
      shells.push_back(er);
    }
  }
  ASSERT_EQ(holes.size(), 3u);
  ASSERT_EQ(shells.size(), 3u);

  auto sort_by_width = [](EdgeRing* a, EdgeRing* b) {
    return a->getRingInternal()->getEnvelopeInternal()->getWidth() >
           b->getRingInternal()->getEnvelopeInternal()->getWidth();
  };
  std::sort(shells.begin(), shells.end(), sort_by_width);
  std::sort(holes.begin(), holes.end(), sort_by_width);

  HoleAssigner::assignHolesToShells(holes, shells);

  EdgeRing* outer_shell = shells[0];
  EdgeRing* mid_shell = shells[1];
  EdgeRing* inner_shell = shells[2];
  EdgeRing* outer_hole = holes[0];
  EdgeRing* mid_hole = holes[1];
  EdgeRing* inner_hole = holes[2];

  EXPECT_EQ(outer_hole->getOuterHole(), nullptr);
  EXPECT_FALSE(outer_hole->isOuterShell());
  EXPECT_TRUE(outer_hole->isOuterHole());
  EXPECT_EQ(outer_shell->getOuterHole(), outer_hole);
  EXPECT_TRUE(outer_shell->isOuterShell());

  EXPECT_FALSE(mid_hole->isOuterHole());
  EXPECT_EQ(mid_shell->getOuterHole(), nullptr);
  EXPECT_FALSE(mid_shell->isOuterShell());
  EXPECT_FALSE(inner_hole->isOuterHole());
  EXPECT_TRUE(inner_hole->hasShell());
  EXPECT_EQ(inner_hole->getShell(), mid_shell);
  EXPECT_EQ(inner_hole->getOuterHole(), nullptr);
  EXPECT_FALSE(inner_hole->isOuterShell());
  EXPECT_EQ(inner_shell->getOuterHole(), nullptr);

  // Calling updateIncludedRecursive() on a hole returns early without setting.
  mid_hole->updateIncludedRecursive();
  EXPECT_FALSE(mid_hole->isIncludedSet());

  // Mark outer shell included, then update innermost shell recursively through
  // mid_shell.
  outer_shell->setIncluded(true);
  inner_shell->updateIncludedRecursive();
  EXPECT_TRUE(mid_shell->isIncludedSet());
  EXPECT_FALSE(mid_shell->isIncluded());
  EXPECT_TRUE(inner_shell->isIncludedSet());
  EXPECT_TRUE(inner_shell->isIncluded());

  // Component 2: two adjacent squares sharing edge (10,0)-(10,10) with no
  // initial included shell, testing recursion cycle breaking and unanchored
  // exit.
  PolygonizeGraph adj_graph(factory.get());
  add_seg(0.0, 0.0, 0.0, 10.0, adj_graph);
  add_seg(0.0, 10.0, 10.0, 10.0, adj_graph);
  add_seg(10.0, 10.0, 10.0, 0.0, adj_graph);
  add_seg(10.0, 0.0, 0.0, 0.0, adj_graph);
  add_seg(10.0, 10.0, 20.0, 10.0, adj_graph);
  add_seg(20.0, 10.0, 20.0, 0.0, adj_graph);
  add_seg(20.0, 0.0, 10.0, 0.0, adj_graph);

  std::vector<EdgeRing*> adj_rings;
  adj_graph.getEdgeRings(adj_rings);
  ASSERT_EQ(adj_rings.size(), 3u);
  std::vector<EdgeRing*> adj_shells;
  std::vector<EdgeRing*> adj_holes;
  for (EdgeRing* er : adj_rings) {
    er->computeValid();
    er->computeHole();
    if (er->isHole()) {
      adj_holes.push_back(er);
    } else {
      adj_shells.push_back(er);
    }
  }
  ASSERT_EQ(adj_shells.size(), 2u);
  ASSERT_EQ(adj_holes.size(), 1u);
  EXPECT_TRUE(adj_holes[0]->isOuterHole());
  EXPECT_EQ(adj_shells[0]->getOuterHole(), adj_holes[0]);
  EXPECT_EQ(adj_shells[1]->getOuterHole(), adj_holes[0]);
  EXPECT_TRUE(adj_shells[0]->isOuterShell());
  EXPECT_TRUE(adj_shells[1]->isOuterShell());

  adj_shells[0]->updateIncludedRecursive();
  EXPECT_FALSE(adj_shells[0]->isIncludedSet());
  EXPECT_FALSE(adj_shells[1]->isIncludedSet());

  // Now anchor adj_shells[0] and verify adj_shells[1] gets opposite inclusion.
  adj_shells[0]->setIncluded(true);
  adj_shells[1]->updateIncludedRecursive();
  EXPECT_TRUE(adj_shells[1]->isIncludedSet());
  EXPECT_FALSE(adj_shells[1]->isIncluded());
}

TEST(EdgeRingTest, GetOuterHoleSkipsNonOuterHoleAdjacentEdge) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  SyntheticGraph graph(factory.get());

  Node* n0 = graph.AddNode(Coordinate(0.0, 0.0));
  Node* n1 = graph.AddNode(Coordinate(0.0, 10.0));

  PolygonizeDirectedEdge* left_fwd = graph.AddEdge(
      n0, n1,
      {Coordinate(0.0, 0.0), Coordinate(-5.0, 5.0), Coordinate(0.0, 10.0)});
  auto* left_rev = static_cast<PolygonizeDirectedEdge*>(left_fwd->getSym());

  PolygonizeDirectedEdge* chord_fwd =
      graph.AddEdge(n1, n0, {Coordinate(0.0, 10.0), Coordinate(0.0, 0.0)});
  auto* chord_rev = static_cast<PolygonizeDirectedEdge*>(chord_fwd->getSym());

  PolygonizeDirectedEdge* right_fwd = graph.AddEdge(
      n1, n0,
      {Coordinate(0.0, 10.0), Coordinate(5.0, 5.0), Coordinate(0.0, 0.0)});
  auto* right_rev = static_cast<PolygonizeDirectedEdge*>(right_fwd->getSym());

  // Shell A (CW): start at chord_fwd so deList[0]'s sym belongs to Shell B.
  left_fwd->setNext(chord_fwd);
  chord_fwd->setNext(left_fwd);
  EdgeRing shell_a(factory.get());
  shell_a.build(chord_fwd);
  shell_a.computeValid();
  shell_a.computeHole();

  // Shell B (CW): chord_rev -> right_fwd.
  chord_rev->setNext(right_fwd);
  right_fwd->setNext(chord_rev);
  EdgeRing shell_b(factory.get());
  shell_b.build(chord_rev);
  shell_b.computeValid();
  shell_b.computeHole();

  // Outer hole (CCW): right_rev -> left_rev.
  right_rev->setNext(left_rev);
  left_rev->setNext(right_rev);
  EdgeRing outer_hole(factory.get());
  outer_hole.build(right_rev);
  outer_hole.computeValid();
  outer_hole.computeHole();

  ASSERT_TRUE(shell_a.isValid());
  ASSERT_FALSE(shell_a.isHole());
  ASSERT_TRUE(shell_b.isValid());
  ASSERT_FALSE(shell_b.isHole());
  ASSERT_TRUE(outer_hole.isValid());
  ASSERT_TRUE(outer_hole.isHole());
  ASSERT_TRUE(outer_hole.isOuterHole());

  EXPECT_EQ(shell_a.getOuterHole(), &outer_hole);
  EXPECT_TRUE(shell_a.isOuterShell());
  EXPECT_EQ(shell_b.getOuterHole(), &outer_hole);
  EXPECT_TRUE(shell_b.isOuterShell());
}

}  // namespace
}  // namespace geos_google
