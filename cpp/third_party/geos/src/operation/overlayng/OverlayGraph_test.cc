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

// Unit tests for third_party/geos/src/operation/overlayng/OverlayGraph.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/OverlayGraph.cpp

#include "third_party/geos/include/geos/operation/overlayng/OverlayGraph.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/operation/overlayng/Edge.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayEdge.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayLabel.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::Location;
using geos::geom::Position;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::operation::overlayng::OverlayEdge;
using geos::operation::overlayng::OverlayGraph;
using geos::operation::overlayng::OverlayLabel;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

std::unique_ptr<CoordinateSequence> MakeSeqXY(
    const std::vector<CoordinateXY>& coords) {
  auto cs = std::make_unique<CoordinateSequence>(coords.size(), false, false);
  for (std::size_t i = 0; i < coords.size(); ++i) {
    cs->setAt(coords[i], i);
  }
  return cs;
}

std::unique_ptr<CoordinateSequence> MakeSeqXYZM(
    const std::vector<CoordinateXYZM>& coords) {
  auto cs = std::make_unique<CoordinateSequence>(coords.size(), true, true);
  for (std::size_t i = 0; i < coords.size(); ++i) {
    cs->setAt(coords[i], i);
  }
  return cs;
}

OverlayEdge* AddAreaEdge(OverlayGraph& graph,
                         const std::vector<CoordinateXY>& coords, uint8_t index,
                         int depth_delta, bool is_hole) {
  EdgeSourceInfo info(index, depth_delta, is_hole);
  Edge edge(MakeSeqXY(coords), &info);
  return graph.addEdge(&edge);
}

OverlayEdge* AddLineEdge(OverlayGraph& graph,
                         const std::vector<CoordinateXY>& coords,
                         uint8_t index) {
  EdgeSourceInfo info(index);
  Edge edge(MakeSeqXY(coords), &info);
  return graph.addEdge(&edge);
}

TEST(OverlayGraphTest, EmptyGraph) {
  OverlayGraph graph;

  EXPECT_THAT(graph.getEdges(), IsEmpty());
  EXPECT_THAT(graph.getNodeEdges(), IsEmpty());
  EXPECT_THAT(graph.getResultAreaEdges(), IsEmpty());
  EXPECT_EQ(graph.getNodeEdge(Coordinate(0.0, 0.0)), nullptr);

  std::ostringstream oss;
  oss << graph;
  EXPECT_EQ(oss.str(), "OGRPH \nNODEMAP [0]\nEDGES [0]\n");
}

TEST(OverlayGraphTest, AddSingleEdgeCreatesSymmetricPairAndTwoNodes) {
  OverlayGraph graph;
  EdgeSourceInfo info(0, 1, false);
  Edge edge(MakeSeqXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)}),
            &info);

  OverlayEdge* e = graph.addEdge(&edge);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(edge.getCoordinatesRO(), nullptr);

  OverlayEdge* sym = e->symOE();
  ASSERT_NE(sym, nullptr);
  EXPECT_EQ(sym->symOE(), e);
  EXPECT_TRUE(e->isForward());
  EXPECT_FALSE(sym->isForward());

  EXPECT_TRUE(e->orig().equals2D(CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(e->dest().equals2D(CoordinateXY(10.0, 0.0)));
  EXPECT_TRUE(e->directionPt().equals2D(CoordinateXY(10.0, 0.0)));
  EXPECT_TRUE(sym->orig().equals2D(CoordinateXY(10.0, 0.0)));
  EXPECT_TRUE(sym->dest().equals2D(CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(sym->directionPt().equals2D(CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(std::isnan(e->orig().z));
  EXPECT_TRUE(std::isnan(e->orig().m));

  // Graph stores both half-edges of the symmetric pair.
  EXPECT_THAT(graph.getEdges(), UnorderedElementsAre(e, sym));
  EXPECT_THAT(graph.getNodeEdges(), UnorderedElementsAre(e, sym));
  EXPECT_EQ(graph.getNodeEdge(Coordinate(0.0, 0.0)), e);
  EXPECT_EQ(graph.getNodeEdge(Coordinate(10.0, 0.0)), sym);
  // Node lookup uses 2D coordinate equality/hash.
  EXPECT_EQ(graph.getNodeEdge(Coordinate(0.0, 0.0, 42.0)), e);
  EXPECT_EQ(graph.getNodeEdge(Coordinate(5.0, 0.0)), nullptr);

  // Symmetric half-edges share the same OverlayLabel with flipped sides.
  ASSERT_NE(e->getLabel(), nullptr);
  EXPECT_EQ(e->getLabel(), sym->getLabel());
  EXPECT_TRUE(e->getLabel()->isBoundary(0));
  EXPECT_TRUE(e->getLabel()->isNotPart(1));
  EXPECT_EQ(e->getLocation(0, Position::LEFT), Location::EXTERIOR);
  EXPECT_EQ(e->getLocation(0, Position::RIGHT), Location::INTERIOR);
  EXPECT_EQ(sym->getLocation(0, Position::LEFT), Location::INTERIOR);
  EXPECT_EQ(sym->getLocation(0, Position::RIGHT), Location::EXTERIOR);
}

TEST(OverlayGraphTest, MultiPointXYZMCoordinatesPreserved) {
  OverlayGraph graph;
  EdgeSourceInfo info(1);
  Edge edge(MakeSeqXYZM({
                CoordinateXYZM(0.0, 0.0, 1.0, 10.0),
                CoordinateXYZM(3.0, 4.0, 2.0, 20.0),
                CoordinateXYZM(6.0, 1.0, 3.0, 30.0),
                CoordinateXYZM(10.0, 0.0, 4.0, 40.0),
            }),
            &info);

  OverlayEdge* e = graph.addEdge(&edge);
  ASSERT_NE(e, nullptr);
  OverlayEdge* sym = e->symOE();
  ASSERT_NE(sym, nullptr);

  EXPECT_TRUE(e->orig().equals4D(CoordinateXYZM(0.0, 0.0, 1.0, 10.0)));
  EXPECT_TRUE(e->directionPt().equals4D(CoordinateXYZM(3.0, 4.0, 2.0, 20.0)));
  EXPECT_TRUE(e->dest().equals4D(CoordinateXYZM(10.0, 0.0, 4.0, 40.0)));

  EXPECT_TRUE(sym->orig().equals4D(CoordinateXYZM(10.0, 0.0, 4.0, 40.0)));
  EXPECT_TRUE(sym->directionPt().equals4D(CoordinateXYZM(6.0, 1.0, 3.0, 30.0)));
  EXPECT_TRUE(sym->dest().equals4D(CoordinateXYZM(0.0, 0.0, 1.0, 10.0)));

  // Intermediate polyline vertices are not graph nodes.
  EXPECT_EQ(graph.getNodeEdge(Coordinate(3.0, 4.0)), nullptr);
  EXPECT_EQ(graph.getNodeEdge(Coordinate(6.0, 1.0)), nullptr);

  EXPECT_TRUE(e->getLabel()->isNotPart(0));
  EXPECT_TRUE(e->getLabel()->isLine(1));
}

TEST(OverlayGraphTest, SharedNodesFormSortedCCWStars) {
  OverlayGraph graph;

  // Insert spokes around (0, 0) in non-CCW order (W, N, E, S) and rim edges.
  AddLineEdge(graph, {CoordinateXY(0.0, 0.0), CoordinateXY(-1.0, 0.0)}, 0);
  AddLineEdge(graph, {CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 1.0)}, 0);
  AddLineEdge(graph, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)}, 0);
  AddLineEdge(graph, {CoordinateXY(0.0, 0.0), CoordinateXY(0.0, -1.0)}, 0);

  AddAreaEdge(graph, {CoordinateXY(1.0, 0.0), CoordinateXY(0.0, 1.0)}, 0, 1,
              false);
  AddAreaEdge(graph, {CoordinateXY(0.0, 1.0), CoordinateXY(-1.0, 0.0)}, 0, 1,
              false);
  AddAreaEdge(graph, {CoordinateXY(-1.0, 0.0), CoordinateXY(0.0, -1.0)}, 0, 1,
              false);
  AddAreaEdge(graph, {CoordinateXY(0.0, -1.0), CoordinateXY(1.0, 0.0)}, 0, 1,
              false);

  EXPECT_EQ(graph.getEdges().size(), 16u);
  std::vector<OverlayEdge*> node_edges = graph.getNodeEdges();
  EXPECT_EQ(node_edges.size(), 5u);

  int total_degree = 0;
  for (OverlayEdge* node_edge : node_edges) {
    ASSERT_NE(node_edge, nullptr);
    EXPECT_TRUE(node_edge->isEdgesSorted());
    total_degree += node_edge->degree();
  }
  EXPECT_EQ(total_degree, 16);

  OverlayEdge* center = graph.getNodeEdge(Coordinate(0.0, 0.0));
  ASSERT_NE(center, nullptr);
  EXPECT_EQ(center->degree(), 4);

  // Starting from any edge around (0, 0), traversing oNextOE() visits all 4
  // directions in cyclic CCW order: E -> N -> W -> S -> E.
  OverlayEdge* east = center;
  while (!east->dest().equals2D(CoordinateXY(1.0, 0.0))) {
    east = east->oNextOE();
    ASSERT_NE(east, center);
  }
  OverlayEdge* north = east->oNextOE();
  OverlayEdge* west = north->oNextOE();
  OverlayEdge* south = west->oNextOE();
  EXPECT_TRUE(north->dest().equals2D(CoordinateXY(0.0, 1.0)));
  EXPECT_TRUE(west->dest().equals2D(CoordinateXY(-1.0, 0.0)));
  EXPECT_TRUE(south->dest().equals2D(CoordinateXY(0.0, -1.0)));
  EXPECT_EQ(south->oNextOE(), east);

  for (const auto& pt :
       std::vector<Coordinate>{Coordinate(1.0, 0.0), Coordinate(0.0, 1.0),
                               Coordinate(-1.0, 0.0), Coordinate(0.0, -1.0)}) {
    OverlayEdge* rim = graph.getNodeEdge(pt);
    ASSERT_NE(rim, nullptr);
    EXPECT_EQ(rim->degree(), 3);
  }
}

TEST(OverlayGraphTest, GetResultAreaEdgesFiltersMarkedEdges) {
  OverlayGraph graph;
  OverlayEdge* e0 = AddAreaEdge(
      graph, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)}, 0, 1, false);
  OverlayEdge* e1 = AddAreaEdge(
      graph, {CoordinateXY(1.0, 0.0), CoordinateXY(0.0, 1.0)}, 0, 1, false);
  OverlayEdge* e2 =
      AddLineEdge(graph, {CoordinateXY(0.0, 1.0), CoordinateXY(0.0, 0.0)}, 1);

  EXPECT_THAT(graph.getResultAreaEdges(), IsEmpty());

  e0->markInResultArea();
  e1->markInResultAreaBoth();
  e2->markInResultLine();

  EXPECT_TRUE(e0->isInResultArea());
  EXPECT_FALSE(e0->symOE()->isInResultArea());
  EXPECT_TRUE(e1->isInResultAreaBoth());
  EXPECT_TRUE(e2->isInResultLine());
  EXPECT_FALSE(e2->isInResultArea());

  EXPECT_THAT(graph.getResultAreaEdges(),
              UnorderedElementsAre(e0, e1, e1->symOE()));

  e1->unmarkFromResultAreaBoth();
  EXPECT_FALSE(e1->isInResultArea());
  EXPECT_FALSE(e1->symOE()->isInResultArea());
  EXPECT_THAT(graph.getResultAreaEdges(), UnorderedElementsAre(e0));
}

TEST(OverlayGraphTest, CreateOverlayLabelReflectsEdgeTopology) {
  OverlayGraph graph;

  // Source 0 boundary shell + Source 1 line via merge.
  EdgeSourceInfo a_shell(0, 1, false);
  EdgeSourceInfo b_line(1);
  Edge edge_ab(MakeSeqXY({CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)}),
               &a_shell);
  Edge edge_b(MakeSeqXY({CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)}),
              &b_line);
  edge_ab.merge(&edge_b);

  OverlayLabel* lbl_ab = graph.createOverlayLabel(&edge_ab);
  ASSERT_NE(lbl_ab, nullptr);
  EXPECT_TRUE(lbl_ab->isBoundary(0));
  EXPECT_FALSE(lbl_ab->isHole(0));
  EXPECT_EQ(lbl_ab->getLocation(0, Position::LEFT, true), Location::EXTERIOR);
  EXPECT_EQ(lbl_ab->getLocation(0, Position::RIGHT, true), Location::INTERIOR);
  EXPECT_TRUE(lbl_ab->isLine(1));

  // Source 0 shell collapsed with opposite-direction shell -> shell collapse.
  Edge edge_shell_fwd(
      MakeSeqXY({CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)}), &a_shell);
  Edge edge_shell_rev(
      MakeSeqXY({CoordinateXY(2.0, 0.0), CoordinateXY(0.0, 0.0)}), &a_shell);
  edge_shell_fwd.merge(&edge_shell_rev);

  OverlayLabel* lbl_shell_collapse = graph.createOverlayLabel(&edge_shell_fwd);
  ASSERT_NE(lbl_shell_collapse, nullptr);
  EXPECT_TRUE(lbl_shell_collapse->isCollapse(0));
  EXPECT_FALSE(lbl_shell_collapse->isHole(0));
  EXPECT_TRUE(lbl_shell_collapse->isNotPart(1));

  // Source 1 hole collapsed with opposite-direction hole -> hole collapse.
  EdgeSourceInfo b_hole(1, -1, true);
  Edge edge_hole_fwd(
      MakeSeqXY({CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)}), &b_hole);
  Edge edge_hole_rev(
      MakeSeqXY({CoordinateXY(2.0, 0.0), CoordinateXY(0.0, 0.0)}), &b_hole);
  edge_hole_fwd.merge(&edge_hole_rev);

  OverlayLabel* lbl_hole_collapse = graph.createOverlayLabel(&edge_hole_fwd);
  ASSERT_NE(lbl_hole_collapse, nullptr);
  EXPECT_TRUE(lbl_hole_collapse->isNotPart(0));
  EXPECT_TRUE(lbl_hole_collapse->isCollapse(1));
  EXPECT_TRUE(lbl_hole_collapse->isHole(1));
}

TEST(OverlayGraphTest, PointerStabilityAcrossManyEdges) {
  OverlayGraph graph;
  constexpr int kEdgeCount = 200;
  std::vector<OverlayEdge*> added;
  std::vector<OverlayLabel*> labels;
  added.reserve(kEdgeCount);
  labels.reserve(kEdgeCount);

  for (int i = 0; i < kEdgeCount; ++i) {
    const double x = static_cast<double>(i);
    OverlayEdge* e = AddAreaEdge(
        graph, {CoordinateXY(x, 0.0), CoordinateXY(x + 1.0, 0.0)}, 0, 1, false);
    ASSERT_NE(e, nullptr);
    added.push_back(e);
    labels.push_back(e->getLabel());
  }

  EXPECT_EQ(graph.getEdges().size(), static_cast<std::size_t>(kEdgeCount * 2));
  EXPECT_EQ(graph.getNodeEdges().size(),
            static_cast<std::size_t>(kEdgeCount + 1));

  for (int i = 0; i < kEdgeCount; ++i) {
    const double x = static_cast<double>(i);
    OverlayEdge* e = added[i];
    EXPECT_TRUE(e->orig().equals2D(CoordinateXY(x, 0.0)));
    EXPECT_TRUE(e->dest().equals2D(CoordinateXY(x + 1.0, 0.0)));
    EXPECT_EQ(e->getLabel(), labels[i]);
    EXPECT_TRUE(e->getLabel()->isBoundary(0));
    EXPECT_NE(graph.getNodeEdge(Coordinate(x, 0.0)), nullptr);
  }
}

TEST(OverlayGraphTest, StreamOutputIncludesNodesAndEdges) {
  OverlayGraph graph;
  OverlayEdge* e0 = AddAreaEdge(
      graph, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)}, 0, 1, false);
  OverlayEdge* e1 = AddLineEdge(
      graph,
      {CoordinateXY(1.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 2.0)},
      1);

  e0->markInResultArea();
  e1->markInResultLine();

  std::ostringstream oss;
  oss << graph;
  const std::string out = oss.str();

  EXPECT_THAT(out, HasSubstr("OGRPH \nNODEMAP [3]\n"));
  EXPECT_THAT(out, HasSubstr("EDGES [4]\n"));
  EXPECT_THAT(out, HasSubstr("OE("));
  EXPECT_THAT(out, HasSubstr(" resA"));
  EXPECT_THAT(out, HasSubstr(" resL"));
  // Header + NODEMAP + 3 node entries + EDGES + 4 edge entries = 10 lines.
  EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 10);
}

}  // namespace
}  // namespace geos_google
