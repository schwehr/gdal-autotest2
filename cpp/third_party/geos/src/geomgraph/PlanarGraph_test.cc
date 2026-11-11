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

// Unit tests for third_party/geos/src/geomgraph/PlanarGraph.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geomgraph/PlanarGraph.cpp

#include "third_party/geos/include/geos/geomgraph/PlanarGraph.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdge.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdgeStar.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/NodeFactory.h"
#include "third_party/geos/include/geos/geomgraph/NodeMap.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Location;
using geos::geomgraph::DirectedEdge;
using geos::geomgraph::DirectedEdgeStar;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeEnd;
using geos::geomgraph::Label;
using geos::geomgraph::Node;
using geos::geomgraph::NodeFactory;
using geos::geomgraph::NodeMap;
using geos::geomgraph::PlanarGraph;
using geos::util::IllegalArgumentException;
using geos::util::TopologyException;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

class DirectedEdgeStarNodeFactory final : public NodeFactory {
 public:
  Node* createNode(const Coordinate& coord) const override {
    return new Node(coord, new DirectedEdgeStar());
  }
};

class TestablePlanarGraph final : public PlanarGraph {
 public:
  using PlanarGraph::insertEdge;
  using PlanarGraph::PlanarGraph;

  std::size_t edgeCount() const { return edges->size(); }
};

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords,
                               const Label& label = Label()) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(seq.release(), label);
}

Node* AddOrMergeNode(PlanarGraph& graph, std::unique_ptr<Node> node) {
  Node* raw = node.get();
  Node* result = graph.addNode(raw);
  if (result == raw) {
    node.release();
  }
  return result;
}

TEST(PlanarGraphTest, DefaultConstructorInitializesEmptyGraph) {
  PlanarGraph graph;

  ASSERT_NE(graph.getNodeMap(), nullptr);
  ASSERT_NE(graph.getEdgeEnds(), nullptr);
  EXPECT_THAT(*graph.getEdgeEnds(), IsEmpty());

  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_THAT(nodes, IsEmpty());
  EXPECT_EQ(graph.getNodeIterator(), graph.getNodeMap()->end());

  Coordinate query(1.0, 2.0);
  EXPECT_EQ(graph.find(query), nullptr);
  EXPECT_FALSE(graph.isBoundaryNode(0, query));
  EXPECT_EQ(graph.findEdgeEnd(nullptr), nullptr);
  EXPECT_EQ(graph.findEdge(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0)),
            nullptr);
  EXPECT_EQ(
      graph.findEdgeInSameDirection(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0)),
      nullptr);
  EXPECT_EQ(graph.printEdges(), "Edges: ");
}

TEST(PlanarGraphTest, CustomFactoryConstructorAndPolymorphicDestruction) {
  DirectedEdgeStarNodeFactory factory;
  std::unique_ptr<PlanarGraph> graph =
      std::make_unique<TestablePlanarGraph>(factory);

  auto edge = MakeEdge({Coordinate(0.0, 0.0), Coordinate(4.0, 0.0)});
  Edge* edge_ptr = edge.get();
  graph->addEdges({edge.release()});

  EXPECT_NE(graph->findEdgeEnd(edge_ptr), nullptr);
  Coordinate start(0.0, 0.0);
  EXPECT_NE(graph->find(start), nullptr);
}

TEST(PlanarGraphTest, AddNodeByCoordinateAndByPointer) {
  PlanarGraph graph;

  Node* n1 = graph.addNode(Coordinate(1.0, 2.0, 5.0));
  ASSERT_NE(n1, nullptr);
  Coordinate query1(1.0, 2.0);
  EXPECT_EQ(graph.find(query1), n1);

  Node* n1_dup = graph.addNode(Coordinate(1.0, 2.0, 15.0));
  EXPECT_EQ(n1_dup, n1);
  EXPECT_DOUBLE_EQ(n1->getCoordinate().z, 10.0);

  auto custom = std::make_unique<Node>(Coordinate(3.0, 4.0), nullptr);
  custom->setLabel(0, Location::BOUNDARY);
  Node* n2 = AddOrMergeNode(graph, std::move(custom));
  ASSERT_NE(n2, nullptr);
  EXPECT_NE(n2, n1);

  auto merge_target = std::make_unique<Node>(Coordinate(3.0, 4.0), nullptr);
  merge_target->setLabel(1, Location::INTERIOR);
  Node* merged = AddOrMergeNode(graph, std::move(merge_target));
  EXPECT_EQ(merged, n2);
  EXPECT_EQ(n2->getLabel().getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(n2->getLabel().getLocation(1), Location::INTERIOR);

  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_THAT(nodes, ElementsAre(n1, n2));

  auto node_it = graph.getNodeIterator();
  ASSERT_NE(node_it, graph.getNodeMap()->end());
  EXPECT_EQ(node_it->second.get(), n1);
  ++node_it;
  ASSERT_NE(node_it, graph.getNodeMap()->end());
  EXPECT_EQ(node_it->second.get(), n2);
}

TEST(PlanarGraphTest, IsBoundaryNodeChecksExistenceNullnessAndGeomIndex) {
  PlanarGraph graph;

  EXPECT_FALSE(graph.isBoundaryNode(0, Coordinate(0.0, 0.0)));

  // Default node created from Coordinate has a null label.
  graph.addNode(Coordinate(0.0, 0.0));
  EXPECT_FALSE(graph.isBoundaryNode(0, Coordinate(0.0, 0.0)));
  EXPECT_FALSE(graph.isBoundaryNode(1, Coordinate(0.0, 0.0)));

  Node* interior_node = graph.addNode(Coordinate(1.0, 0.0));
  interior_node->setLabel(0, Location::INTERIOR);
  EXPECT_FALSE(graph.isBoundaryNode(0, Coordinate(1.0, 0.0)));
  EXPECT_FALSE(graph.isBoundaryNode(1, Coordinate(1.0, 0.0)));

  Node* geom0_bdy = graph.addNode(Coordinate(2.0, 0.0));
  geom0_bdy->setLabel(0, Location::BOUNDARY);
  geom0_bdy->setLabel(1, Location::INTERIOR);
  EXPECT_TRUE(graph.isBoundaryNode(0, Coordinate(2.0, 0.0)));
  EXPECT_FALSE(graph.isBoundaryNode(1, Coordinate(2.0, 0.0)));

  Node* geom1_bdy = graph.addNode(Coordinate(3.0, 0.0));
  geom1_bdy->setLabel(1, Location::BOUNDARY);
  EXPECT_FALSE(graph.isBoundaryNode(0, Coordinate(3.0, 0.0)));
  EXPECT_TRUE(graph.isBoundaryNode(1, Coordinate(3.0, 0.0)));
}

TEST(PlanarGraphTest, InsertEdgePopulatesEdgeListAndPrintEdges) {
  TestablePlanarGraph graph;

  auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(5.0, 0.0)},
                     Label(0, Location::INTERIOR));
  auto e1 = MakeEdge({Coordinate(5.0, 0.0), Coordinate(5.0, 5.0)},
                     Label(0, Location::BOUNDARY));
  Edge* e0_ptr = e0.get();
  Edge* e1_ptr = e1.get();

  graph.insertEdge(e0.release());
  graph.insertEdge(e1.release());

  EXPECT_EQ(graph.edgeCount(), 2u);
  auto it = graph.getEdgeIterator();
  EXPECT_EQ(*it, e0_ptr);
  ++it;
  EXPECT_EQ(*it, e1_ptr);

  const std::string printed = graph.printEdges();
  EXPECT_THAT(printed, HasSubstr("Edges: "));
  EXPECT_THAT(printed, HasSubstr("edge 0:"));
  EXPECT_THAT(printed, HasSubstr("edge 1:"));
}

TEST(PlanarGraphTest, AddEdgeEndPopulatesEdgeEndListAndFindEdgeEnd) {
  auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(3.0, 4.0)});
  auto unrelated = MakeEdge({Coordinate(9.0, 9.0), Coordinate(10.0, 10.0)});

  DirectedEdgeStarNodeFactory factory;
  PlanarGraph graph(factory);

  auto de_fwd = std::make_unique<DirectedEdge>(e0.get(), true);
  auto de_bwd = std::make_unique<DirectedEdge>(e0.get(), false);
  EdgeEnd* de_fwd_ptr = de_fwd.get();

  graph.add(de_fwd.release());
  graph.add(de_bwd.release());

  ASSERT_NE(graph.getEdgeEnds(), nullptr);
  EXPECT_EQ(graph.getEdgeEnds()->size(), 2u);
  EXPECT_EQ(graph.findEdgeEnd(e0.get()), de_fwd_ptr);
  EXPECT_EQ(graph.findEdgeEnd(unrelated.get()), nullptr);

  Coordinate p0(0.0, 0.0);
  Coordinate p1(3.0, 4.0);
  Node* n0 = graph.find(p0);
  Node* n1 = graph.find(p1);
  ASSERT_NE(n0, nullptr);
  ASSERT_NE(n1, nullptr);
  EXPECT_EQ(n0->getEdges()->getDegree(), 1u);
  EXPECT_EQ(n1->getEdges()->getDegree(), 1u);
}

TEST(PlanarGraphTest, AddEdgesCreatesPairedSymmetricDirectedEdges) {
  DirectedEdgeStarNodeFactory factory;
  PlanarGraph graph(factory);

  graph.addEdges({});
  EXPECT_THAT(*graph.getEdgeEnds(), IsEmpty());

  auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)});
  auto e1 = MakeEdge({Coordinate(10.0, 0.0), Coordinate(10.0, 10.0)});
  Edge* e0_ptr = e0.get();
  Edge* e1_ptr = e1.get();

  graph.addEdges({e0.release(), e1.release()});

  ASSERT_EQ(graph.getEdgeEnds()->size(), 4u);
  for (EdgeEnd* ee : *graph.getEdgeEnds()) {
    auto* de = dynamic_cast<DirectedEdge*>(ee);
    ASSERT_NE(de, nullptr);
    ASSERT_NE(de->getSym(), nullptr);
    EXPECT_EQ(de->getSym()->getSym(), de);
    EXPECT_NE(de->getSym(), de);
  }

  Coordinate shared(10.0, 0.0);
  Node* shared_node = graph.find(shared);
  ASSERT_NE(shared_node, nullptr);
  EXPECT_EQ(shared_node->getEdges()->getDegree(), 2u);

  EXPECT_NE(graph.findEdgeEnd(e0_ptr), nullptr);
  EXPECT_NE(graph.findEdgeEnd(e1_ptr), nullptr);
}

TEST(PlanarGraphTest, FindEdgeMatchesExactFirstTwoCoordinates) {
  DirectedEdgeStarNodeFactory factory;
  PlanarGraph graph(factory);

  auto e0 = MakeEdge(
      {Coordinate(0.0, 0.0), Coordinate(5.0, 0.0), Coordinate(10.0, 10.0)});
  auto e1 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(0.0, 5.0)});
  Edge* e0_ptr = e0.get();
  Edge* e1_ptr = e1.get();

  graph.addEdges({e0.release(), e1.release()});

  EXPECT_EQ(graph.findEdge(Coordinate(0.0, 0.0), Coordinate(5.0, 0.0)), e0_ptr);
  EXPECT_EQ(graph.findEdge(Coordinate(0.0, 0.0), Coordinate(0.0, 5.0)), e1_ptr);
  // Must match coordinates 0 and 1 specifically, not endpoint or reversed.
  EXPECT_EQ(graph.findEdge(Coordinate(0.0, 0.0), Coordinate(10.0, 10.0)),
            nullptr);
  EXPECT_EQ(graph.findEdge(Coordinate(5.0, 0.0), Coordinate(0.0, 0.0)),
            nullptr);
  EXPECT_EQ(graph.findEdge(Coordinate(99.0, 99.0), Coordinate(5.0, 0.0)),
            nullptr);
}

TEST(PlanarGraphTest,
     FindEdgeInSameDirectionCoversForwardBackwardAndMismatches) {
  DirectedEdgeStarNodeFactory factory;
  PlanarGraph graph(factory);

  // e_other starts at (0,0) heading North; e0 starts at (0,0) heading East
  // then North to (10,10); e_incoming ends at (0,0) from (20,20).
  auto e_other = MakeEdge({Coordinate(0.0, 0.0), Coordinate(0.0, 10.0)});
  auto e0 = MakeEdge(
      {Coordinate(0.0, 0.0), Coordinate(10.0, 0.0), Coordinate(10.0, 10.0)});
  auto e_incoming = MakeEdge({Coordinate(20.0, 20.0), Coordinate(0.0, 0.0)});

  Edge* e0_ptr = e0.get();
  Edge* e_incoming_ptr = e_incoming.get();
  graph.addEdges({e_other.release(), e0.release(), e_incoming.release()});

  // Matches forward first segment (0,0)->(10,0) with shorter and longer rays.
  EXPECT_EQ(
      graph.findEdgeInSameDirection(Coordinate(0.0, 0.0), Coordinate(3.0, 0.0)),
      e0_ptr);
  EXPECT_EQ(graph.findEdgeInSameDirection(Coordinate(0.0, 0.0),
                                          Coordinate(25.0, 0.0)),
            e0_ptr);

  // Matches backward last segment (10,10)->(10,0) of e0.
  EXPECT_EQ(graph.findEdgeInSameDirection(Coordinate(10.0, 10.0),
                                          Coordinate(10.0, 4.0)),
            e0_ptr);

  // Matches backward segment (0,0)->(20,20) of e_incoming after rejecting its
  // forward start (20,20) != (0,0).
  EXPECT_EQ(
      graph.findEdgeInSameDirection(Coordinate(0.0, 0.0), Coordinate(5.0, 5.0)),
      e_incoming_ptr);

  // Node does not exist.
  EXPECT_EQ(graph.findEdgeInSameDirection(Coordinate(99.0, 99.0),
                                          Coordinate(100.0, 99.0)),
            nullptr);

  // Node exists, but no incident edge has this non-collinear direction.
  EXPECT_EQ(
      graph.findEdgeInSameDirection(Coordinate(0.0, 0.0), Coordinate(5.0, 2.0)),
      nullptr);

  // Collinear with e0's first segment (0,0)->(10,0), but opposite quadrant.
  EXPECT_EQ(graph.findEdgeInSameDirection(Coordinate(0.0, 0.0),
                                          Coordinate(-5.0, 0.0)),
            nullptr);

  // Degenerate zero-length direction triggers Quadrant::quadrant error.
  EXPECT_THROW(
      graph.findEdgeInSameDirection(Coordinate(0.0, 0.0), Coordinate(0.0, 0.0)),
      IllegalArgumentException);
}

TEST(PlanarGraphTest, LinkResultDirectedEdgesLinksCycleAndSupportsSubsetRange) {
  DirectedEdgeStarNodeFactory factory;
  PlanarGraph graph(factory);

  const Label ccw_area_label(0, Location::BOUNDARY, Location::INTERIOR,
                             Location::EXTERIOR);
  auto e0 =
      MakeEdge({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)}, ccw_area_label);
  auto e1 =
      MakeEdge({Coordinate(10.0, 0.0), Coordinate(0.0, 10.0)}, ccw_area_label);
  auto e2 =
      MakeEdge({Coordinate(0.0, 10.0), Coordinate(0.0, 0.0)}, ccw_area_label);
  Edge* e0_ptr = e0.get();
  Edge* e1_ptr = e1.get();
  Edge* e2_ptr = e2.get();

  graph.addEdges({e0.release(), e1.release(), e2.release()});

  DirectedEdge* de0_fwd = nullptr;
  DirectedEdge* de1_fwd = nullptr;
  DirectedEdge* de2_fwd = nullptr;
  for (EdgeEnd* ee : *graph.getEdgeEnds()) {
    auto* de = dynamic_cast<DirectedEdge*>(ee);
    ASSERT_NE(de, nullptr);
    if (!de->isForward()) {
      continue;
    }
    de->setInResult(true);
    if (de->getEdge() == e0_ptr) {
      de0_fwd = de;
    } else if (de->getEdge() == e1_ptr) {
      de1_fwd = de;
    } else if (de->getEdge() == e2_ptr) {
      de2_fwd = de;
    }
  }
  ASSERT_NE(de0_fwd, nullptr);
  ASSERT_NE(de1_fwd, nullptr);
  ASSERT_NE(de2_fwd, nullptr);

  // Link only a subset containing the node at (0,0) first.
  Coordinate node_a_coord(0.0, 0.0);
  Node* node_a = graph.find(node_a_coord);
  ASSERT_NE(node_a, nullptr);
  std::vector<Node*> subset = {node_a};
  PlanarGraph::linkResultDirectedEdges(subset.begin(), subset.end());
  EXPECT_EQ(de2_fwd->getNext(), de0_fwd);
  EXPECT_EQ(de0_fwd->getNext(), nullptr);
  EXPECT_EQ(de1_fwd->getNext(), nullptr);

  // Link all nodes across the graph to complete the ring.
  graph.linkResultDirectedEdges();
  EXPECT_EQ(de0_fwd->getNext(), de1_fwd);
  EXPECT_EQ(de1_fwd->getNext(), de2_fwd);
  EXPECT_EQ(de2_fwd->getNext(), de0_fwd);
}

TEST(PlanarGraphTest, LinkResultDirectedEdgesThrowsOnUnmatchedIncomingEdge) {
  DirectedEdgeStarNodeFactory factory;
  PlanarGraph graph(factory);

  const Label area_label(0, Location::BOUNDARY, Location::INTERIOR,
                         Location::EXTERIOR);
  auto e = MakeEdge({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)}, area_label);
  graph.addEdges({e.release()});

  // Mark only the forward DirectedEdge in result; at (10,0) it is incoming
  // with no outgoing result edge.
  for (EdgeEnd* ee : *graph.getEdgeEnds()) {
    auto* de = dynamic_cast<DirectedEdge*>(ee);
    ASSERT_NE(de, nullptr);
    if (de->isForward()) {
      de->setInResult(true);
    }
  }

  EXPECT_THROW(graph.linkResultDirectedEdges(), TopologyException);
}

TEST(PlanarGraphTest, LinkAllDirectedEdgesLinksEveryEdgeEndInCWOrder) {
  DirectedEdgeStarNodeFactory factory;
  PlanarGraph graph(factory);

  auto e0 = MakeEdge({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)});
  auto e1 = MakeEdge({Coordinate(10.0, 0.0), Coordinate(0.0, 10.0)});
  auto e2 = MakeEdge({Coordinate(0.0, 10.0), Coordinate(0.0, 0.0)});
  graph.addEdges({e0.release(), e1.release(), e2.release()});

  graph.linkAllDirectedEdges();

  ASSERT_EQ(graph.getEdgeEnds()->size(), 6u);
  for (EdgeEnd* ee : *graph.getEdgeEnds()) {
    auto* de = dynamic_cast<DirectedEdge*>(ee);
    ASSERT_NE(de, nullptr);
    EXPECT_NE(de->getNext(), nullptr);
  }
}

}  // namespace
}  // namespace geos_google
