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

#include "third_party/geos/include/geos/planargraph/PlanarGraph.h"

#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdge.h"
#include "third_party/geos/include/geos/planargraph/Edge.h"
#include "third_party/geos/include/geos/planargraph/Node.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::planargraph::DirectedEdge;
using ::geos::planargraph::Edge;
using ::geos::planargraph::Node;
using ::geos::planargraph::PlanarGraph;

class TestPlanarGraph : public PlanarGraph {
 public:
  using PlanarGraph::add;
};

struct GraphEdgeBundle {
  DirectedEdge de0;
  DirectedEdge de1;
  Edge edge;

  GraphEdgeBundle(Node* n0, Node* n1)
      : de0(n0, n1, n1->getCoordinate(), true),
        de1(n1, n0, n0->getCoordinate(), false),
        edge(&de0, &de1) {}
};

TEST(PlanarGraphTest, EmptyGraphStateAndVirtualDestructor) {
  TestPlanarGraph graph;
  const TestPlanarGraph& const_graph = graph;

  EXPECT_EQ(graph.findNode(Coordinate(0.0, 0.0)), nullptr);
  EXPECT_EQ(graph.nodeBegin(), graph.nodeEnd());
  EXPECT_EQ(graph.nodeIterator(), graph.nodeEnd());
  EXPECT_EQ(const_graph.nodeBegin(), const_graph.nodeEnd());

  EXPECT_EQ(graph.dirEdgeBegin(), graph.dirEdgeEnd());
  EXPECT_EQ(graph.dirEdgeIterator(), graph.dirEdgeEnd());

  EXPECT_EQ(graph.edgeBegin(), graph.edgeEnd());
  EXPECT_EQ(graph.edgeIterator(), graph.edgeEnd());
  ASSERT_NE(graph.getEdges(), nullptr);
  EXPECT_TRUE(graph.getEdges()->empty());

  std::vector<Node*> nodes;
  graph.getNodes(nodes);
  EXPECT_TRUE(nodes.empty());

  std::unique_ptr<std::vector<Node*>> deg0(graph.findNodesOfDegree(0));
  ASSERT_NE(deg0, nullptr);
  EXPECT_TRUE(deg0->empty());

  std::unique_ptr<PlanarGraph> poly_graph = std::make_unique<TestPlanarGraph>();
  EXPECT_EQ(poly_graph->findNode(Coordinate(1.0, 2.0)), nullptr);
}

TEST(PlanarGraphTest, AddNodesEdgesAndIterators) {
  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(3.0, 0.0));
  Node n2(Coordinate(0.0, 4.0));
  GraphEdgeBundle b01(&n0, &n1);
  GraphEdgeBundle b12(&n1, &n2);
  GraphEdgeBundle b20(&n2, &n0);
  DirectedEdge standalone_de(&n0, &n2, n2.getCoordinate(), true);
  standalone_de.setEdge(nullptr);
  standalone_de.setSym(nullptr);

  TestPlanarGraph graph;
  // Insert nodes out of CoordinateLessThan order.
  graph.add(&n1);
  graph.add(&n2);
  graph.add(&n0);
  graph.add(&b01.edge);
  graph.add(&b12.edge);
  graph.add(&b20.edge);
  graph.add(&standalone_de);

  EXPECT_EQ(graph.findNode(Coordinate(0.0, 0.0)), &n0);
  EXPECT_EQ(graph.findNode(Coordinate(3.0, 0.0)), &n1);
  EXPECT_EQ(graph.findNode(Coordinate(0.0, 4.0)), &n2);
  EXPECT_EQ(graph.findNode(Coordinate(5.0, 5.0)), nullptr);

  // NodeMap orders nodes by (x, y): (0,0), (0,4), (3,0).
  std::vector<Node*> got_nodes;
  graph.getNodes(got_nodes);
  EXPECT_EQ(got_nodes, (std::vector<Node*>{&n0, &n2, &n1}));

  std::vector<Node*> begin_nodes;
  for (auto it = graph.nodeBegin(); it != graph.nodeEnd(); ++it) {
    begin_nodes.push_back(it->second);
  }
  EXPECT_EQ(begin_nodes, (std::vector<Node*>{&n0, &n2, &n1}));

  std::vector<Node*> iter_nodes;
  for (auto it = graph.nodeIterator(); it != graph.nodeEnd(); ++it) {
    iter_nodes.push_back(it->second);
  }
  EXPECT_EQ(iter_nodes, (std::vector<Node*>{&n0, &n2, &n1}));

  const TestPlanarGraph& const_graph = graph;
  std::vector<Node*> const_iter_nodes;
  for (auto it = const_graph.nodeBegin(); it != const_graph.nodeEnd(); ++it) {
    const_iter_nodes.push_back(it->second);
  }
  EXPECT_EQ(const_iter_nodes, (std::vector<Node*>{&n0, &n2, &n1}));

  PlanarGraph::EdgeContainer* edges = graph.getEdges();
  EXPECT_EQ(*edges, (std::vector<Edge*>{&b01.edge, &b12.edge, &b20.edge}));

  std::vector<Edge*> begin_edges;
  for (PlanarGraph::EdgeIterator it = graph.edgeBegin(); it != graph.edgeEnd();
       ++it) {
    begin_edges.push_back(*it);
  }
  EXPECT_EQ(begin_edges, (std::vector<Edge*>{&b01.edge, &b12.edge, &b20.edge}));

  std::vector<Edge*> iter_edges;
  for (PlanarGraph::EdgeIterator it = graph.edgeIterator();
       it != graph.edgeEnd(); ++it) {
    iter_edges.push_back(*it);
  }
  EXPECT_EQ(iter_edges, (std::vector<Edge*>{&b01.edge, &b12.edge, &b20.edge}));

  std::vector<DirectedEdge*> iter_dir_edges;
  for (auto it = graph.dirEdgeIterator(); it != graph.dirEdgeEnd(); ++it) {
    iter_dir_edges.push_back(*it);
  }
  EXPECT_EQ(iter_dir_edges,
            (std::vector<DirectedEdge*>{&b01.de0, &b01.de1, &b12.de0, &b12.de1,
                                        &b20.de0, &b20.de1, &standalone_de}));
}

TEST(PlanarGraphTest, FindNodesOfDegreeOverloads) {
  Node isolated(Coordinate(0.0, 0.0));
  Node leaf_a(Coordinate(1.0, 1.0));
  Node hub(Coordinate(2.0, 0.0));
  Node mid(Coordinate(2.0, -2.0));
  Node leaf_b(Coordinate(3.0, 1.0));
  Node leaf_c(Coordinate(4.0, -2.0));

  GraphEdgeBundle e_a(&hub, &leaf_a);
  GraphEdgeBundle e_b(&hub, &leaf_b);
  GraphEdgeBundle e_m(&hub, &mid);
  GraphEdgeBundle e_c(&mid, &leaf_c);

  TestPlanarGraph graph;
  graph.add(&isolated);
  graph.add(&leaf_a);
  graph.add(&hub);
  graph.add(&mid);
  graph.add(&leaf_b);
  graph.add(&leaf_c);
  graph.add(&e_a.edge);
  graph.add(&e_b.edge);
  graph.add(&e_m.edge);
  graph.add(&e_c.edge);

  std::unique_ptr<std::vector<Node*>> deg0(graph.findNodesOfDegree(0));
  EXPECT_EQ(*deg0, (std::vector<Node*>{&isolated}));

  std::unique_ptr<std::vector<Node*>> deg1(graph.findNodesOfDegree(1));
  EXPECT_EQ(*deg1, (std::vector<Node*>{&leaf_a, &leaf_b, &leaf_c}));

  std::unique_ptr<std::vector<Node*>> deg2(graph.findNodesOfDegree(2));
  EXPECT_EQ(*deg2, (std::vector<Node*>{&mid}));

  std::unique_ptr<std::vector<Node*>> deg3(graph.findNodesOfDegree(3));
  EXPECT_EQ(*deg3, (std::vector<Node*>{&hub}));

  std::unique_ptr<std::vector<Node*>> deg4(graph.findNodesOfDegree(4));
  EXPECT_TRUE(deg4->empty());

  // Output-parameter overload appends to existing elements in `to`.
  std::vector<Node*> appended = {&isolated};
  graph.findNodesOfDegree(1, appended);
  EXPECT_EQ(appended,
            (std::vector<Node*>{&isolated, &leaf_a, &leaf_b, &leaf_c}));
}

TEST(PlanarGraphTest, RemoveDirectedEdgeClearsSymAndLeavesNodes) {
  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(2.0, 0.0));
  Node n2(Coordinate(4.0, 0.0));
  GraphEdgeBundle b0(&n0, &n1);
  GraphEdgeBundle b1(&n1, &n2);

  Node orphan_from(Coordinate(10.0, 10.0));
  Node orphan_to(Coordinate(11.0, 10.0));
  DirectedEdge unadded_de(&orphan_from, &orphan_to, orphan_to.getCoordinate(),
                          true);
  unadded_de.setEdge(nullptr);
  unadded_de.setSym(nullptr);
  orphan_from.addOutEdge(&unadded_de);

  TestPlanarGraph graph;
  graph.add(&n0);
  graph.add(&n1);
  graph.add(&n2);
  graph.add(&b0.edge);
  graph.add(&b1.edge);

  // Removing b0.de0 (with non-null sym b0.de1) clears b0.de1's sym pointer
  // and leaves b1 and all nodes intact.
  graph.remove(&b0.de0);
  EXPECT_EQ(b0.de1.getSym(), nullptr);
  EXPECT_EQ(n0.getDegree(), 0U);
  EXPECT_EQ(n1.getDegree(), 2U);
  EXPECT_EQ(graph.findNode(n0.getCoordinate()), &n0);
  EXPECT_EQ(
      (std::vector<DirectedEdge*>(graph.dirEdgeBegin(), graph.dirEdgeEnd())),
      (std::vector<DirectedEdge*>{&b0.de1, &b1.de0, &b1.de1}));

  // Now b0.de1 has sym == nullptr; removing it takes the null-sym path.
  graph.remove(&b0.de1);
  EXPECT_EQ(n1.getDegree(), 1U);
  EXPECT_EQ(
      (std::vector<DirectedEdge*>(graph.dirEdgeBegin(), graph.dirEdgeEnd())),
      (std::vector<DirectedEdge*>{&b1.de0, &b1.de1}));

  // Removing a DirectedEdge not present in graph.dirEdges scans dirEdges
  // without removing b1's edges while still unhooking from orphan_from.
  graph.remove(&unadded_de);
  EXPECT_EQ(orphan_from.getDegree(), 0U);
  EXPECT_EQ(
      (std::vector<DirectedEdge*>(graph.dirEdgeBegin(), graph.dirEdgeEnd())),
      (std::vector<DirectedEdge*>{&b1.de0, &b1.de1}));
}

TEST(PlanarGraphTest, RemoveEdgeRemovesDirectedEdgesButNotNodes) {
  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(3.0, 0.0));
  Node n2(Coordinate(0.0, 4.0));
  GraphEdgeBundle b0(&n0, &n1);
  GraphEdgeBundle b1(&n1, &n2);
  GraphEdgeBundle b2(&n2, &n0);
  GraphEdgeBundle unadded(&n0, &n2);

  TestPlanarGraph graph;
  graph.add(&n0);
  graph.add(&n1);
  graph.add(&n2);
  graph.add(&b0.edge);
  graph.add(&b1.edge);
  graph.add(&b2.edge);

  // Removing middle edge leaves first and last edges intact.
  graph.remove(&b1.edge);
  EXPECT_EQ(*graph.getEdges(), (std::vector<Edge*>{&b0.edge, &b2.edge}));
  EXPECT_EQ(
      (std::vector<DirectedEdge*>(graph.dirEdgeBegin(), graph.dirEdgeEnd())),
      (std::vector<DirectedEdge*>{&b0.de0, &b0.de1, &b2.de0, &b2.de1}));
  EXPECT_EQ(n0.getDegree(), 3U);
  EXPECT_EQ(n1.getDegree(), 1U);
  EXPECT_EQ(n2.getDegree(), 2U);

  // Removing an edge not added to the graph does not remove b0 or b2.
  graph.remove(&unadded.edge);
  EXPECT_EQ(*graph.getEdges(), (std::vector<Edge*>{&b0.edge, &b2.edge}));
  EXPECT_EQ(n0.getDegree(), 2U);
  EXPECT_EQ(n2.getDegree(), 1U);

  // Remove remaining front and back edges.
  graph.remove(&b0.edge);
  graph.remove(&b2.edge);
  EXPECT_TRUE(graph.getEdges()->empty());
  EXPECT_EQ(graph.dirEdgeBegin(), graph.dirEdgeEnd());

  // Nodes are never removed by remove(Edge*).
  std::unique_ptr<std::vector<Node*>> deg0(graph.findNodesOfDegree(0));
  EXPECT_EQ(*deg0, (std::vector<Node*>{&n0, &n2, &n1}));
}

TEST(PlanarGraphTest, RemoveNodeUnhooksIncidentAndOrphanEdges) {
  Node hub(Coordinate(0.0, 0.0));
  Node a(Coordinate(2.0, 0.0));
  Node b(Coordinate(0.0, 2.0));
  Node isolated(Coordinate(-5.0, -5.0));

  GraphEdgeBundle e_hub_a(&hub, &a);
  GraphEdgeBundle e_hub_b(&hub, &b);
  GraphEdgeBundle e_a_b(&a, &b);

  // Standalone DirectedEdge with null sym and null parent Edge attached to hub.
  DirectedEdge orphan_de(&hub, &a, Coordinate(1.0, 1.0), true);
  orphan_de.setSym(nullptr);
  orphan_de.setEdge(nullptr);
  hub.addOutEdge(&orphan_de);

  TestPlanarGraph graph;
  graph.add(&hub);
  graph.add(&a);
  graph.add(&b);
  graph.add(&isolated);
  graph.add(&e_hub_a.edge);
  graph.add(&e_hub_b.edge);
  graph.add(&e_a_b.edge);
  graph.add(&orphan_de);

  graph.remove(&hub);

  EXPECT_EQ(graph.findNode(hub.getCoordinate()), nullptr);
  EXPECT_EQ(graph.findNode(a.getCoordinate()), &a);
  EXPECT_EQ(graph.findNode(b.getCoordinate()), &b);
  EXPECT_EQ(a.getDegree(), 1U);
  EXPECT_EQ(b.getDegree(), 1U);
  EXPECT_EQ(*graph.getEdges(), (std::vector<Edge*>{&e_a_b.edge}));
  EXPECT_EQ(
      (std::vector<DirectedEdge*>(graph.dirEdgeBegin(), graph.dirEdgeEnd())),
      (std::vector<DirectedEdge*>{&e_a_b.de0, &e_a_b.de1}));

  // Removing a degree-0 node removes only that node from nodeMap.
  graph.remove(&isolated);
  EXPECT_EQ(graph.findNode(isolated.getCoordinate()), nullptr);
  EXPECT_EQ(*graph.getEdges(), (std::vector<Edge*>{&e_a_b.edge}));
}

TEST(PlanarGraphTest, RemoveNodeWithSelfLoopEdge) {
  Node loop_node(Coordinate(1.0, 1.0));
  DirectedEdge de0(&loop_node, &loop_node, Coordinate(2.0, 1.0), true);
  DirectedEdge de1(&loop_node, &loop_node, Coordinate(1.0, 2.0), false);
  Edge self_loop(&de0, &de1);

  TestPlanarGraph graph;
  graph.add(&loop_node);
  graph.add(&self_loop);

  graph.remove(&loop_node);
  EXPECT_EQ(graph.findNode(loop_node.getCoordinate()), nullptr);
  EXPECT_TRUE(graph.getEdges()->empty());
  EXPECT_EQ(graph.dirEdgeBegin(), graph.dirEdgeEnd());
}

}  // namespace
}  // namespace geos_google
