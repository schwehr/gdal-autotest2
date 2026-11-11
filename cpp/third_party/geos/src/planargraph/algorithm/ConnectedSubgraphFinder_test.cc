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

#include "third_party/geos/include/geos/planargraph/algorithm/ConnectedSubgraphFinder.h"

#include <cstddef>
#include <memory>
#include <set>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdge.h"
#include "third_party/geos/include/geos/planargraph/Edge.h"
#include "third_party/geos/include/geos/planargraph/Node.h"
#include "third_party/geos/include/geos/planargraph/PlanarGraph.h"
#include "third_party/geos/include/geos/planargraph/Subgraph.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::planargraph::DirectedEdge;
using ::geos::planargraph::Edge;
using ::geos::planargraph::Node;
using ::geos::planargraph::PlanarGraph;
using ::geos::planargraph::Subgraph;
using ::geos::planargraph::algorithm::ConnectedSubgraphFinder;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedElementsAreArray;

// Concrete PlanarGraph subclass that owns its allocated graph components.
class TestPlanarGraph : public PlanarGraph {
 public:
  Node* GetOrCreateNode(const Coordinate& pt) {
    if (Node* existing = findNode(pt); existing != nullptr) {
      return existing;
    }
    owned_nodes_.push_back(std::make_unique<Node>(pt));
    Node* node = owned_nodes_.back().get();
    PlanarGraph::add(node);
    return node;
  }

  Edge* AddEdgeWithDirs(const Coordinate& p0, const Coordinate& p1,
                        const Coordinate& dir0, const Coordinate& dir1) {
    Node* n0 = GetOrCreateNode(p0);
    Node* n1 = GetOrCreateNode(p1);
    owned_dir_edges_.push_back(
        std::make_unique<DirectedEdge>(n0, n1, dir0, true));
    DirectedEdge* de0 = owned_dir_edges_.back().get();
    owned_dir_edges_.push_back(
        std::make_unique<DirectedEdge>(n1, n0, dir1, false));
    DirectedEdge* de1 = owned_dir_edges_.back().get();
    owned_edges_.push_back(std::make_unique<Edge>(de0, de1));
    Edge* edge = owned_edges_.back().get();
    PlanarGraph::add(edge);
    return edge;
  }

  Edge* AddEdge(const Coordinate& p0, const Coordinate& p1) {
    return AddEdgeWithDirs(p0, p1, p1, p0);
  }

  Edge* AddSelfLoop(const Coordinate& pt, const Coordinate& dir0,
                    const Coordinate& dir1) {
    return AddEdgeWithDirs(pt, pt, dir0, dir1);
  }

 private:
  std::vector<std::unique_ptr<Node>> owned_nodes_;
  std::vector<std::unique_ptr<DirectedEdge>> owned_dir_edges_;
  std::vector<std::unique_ptr<Edge>> owned_edges_;
};

// RAII owner for heap-allocated Subgraph pointers returned by
// ConnectedSubgraphFinder::getConnectedSubgraphs.
struct ScopedSubgraphList {
  std::vector<Subgraph*> items;

  ~ScopedSubgraphList() {
    for (Subgraph* sg : items) {
      delete sg;
    }
  }
};

std::vector<Edge*> CollectEdges(Subgraph& subgraph) {
  return std::vector<Edge*>(subgraph.edgeBegin(), subgraph.edgeEnd());
}

std::vector<Node*> CollectNodes(const Subgraph& subgraph) {
  std::vector<Node*> nodes;
  for (auto it = subgraph.nodeBegin(); it != subgraph.nodeEnd(); ++it) {
    nodes.push_back(it->second);
  }
  return nodes;
}

Subgraph* FindSubgraphContaining(const std::vector<Subgraph*>& subgraphs,
                                 Edge* target) {
  for (Subgraph* sg : subgraphs) {
    if (sg != nullptr && sg->contains(target)) {
      return sg;
    }
  }
  return nullptr;
}

TEST(ConnectedSubgraphFinderTest, EmptyGraphProducesNoSubgraphs) {
  TestPlanarGraph graph;
  ConnectedSubgraphFinder finder(graph);
  ScopedSubgraphList subgraphs;

  finder.getConnectedSubgraphs(subgraphs.items);
  EXPECT_THAT(subgraphs.items, IsEmpty());
}

TEST(ConnectedSubgraphFinderTest,
     IsolatedNodesOnlyProduceNoSubgraphsAndResetVisitedFlags) {
  TestPlanarGraph graph;
  Node* n0 = graph.GetOrCreateNode(Coordinate(0.0, 0.0));
  Node* n1 = graph.GetOrCreateNode(Coordinate(5.0, 5.0));
  n0->setVisited(true);
  n1->setVisited(false);

  ConnectedSubgraphFinder finder(graph);
  ScopedSubgraphList subgraphs;
  finder.getConnectedSubgraphs(subgraphs.items);

  EXPECT_THAT(subgraphs.items, IsEmpty());
  EXPECT_FALSE(n0->isVisited());
  EXPECT_FALSE(n1->isVisited());
}

TEST(ConnectedSubgraphFinderTest, SingleEdgeGraphProducesOneSubgraph) {
  TestPlanarGraph graph;
  Edge* edge = graph.AddEdge(Coordinate(0.0, 0.0), Coordinate(3.0, 4.0));
  Node* n0 = graph.findNode(Coordinate(0.0, 0.0));
  Node* n1 = graph.findNode(Coordinate(3.0, 4.0));

  ConnectedSubgraphFinder finder(graph);
  ScopedSubgraphList subgraphs;
  finder.getConnectedSubgraphs(subgraphs.items);

  ASSERT_EQ(subgraphs.items.size(), 1u);
  Subgraph* sg = subgraphs.items[0];
  ASSERT_NE(sg, nullptr);
  EXPECT_EQ(&sg->getParent(), &graph);
  EXPECT_THAT(CollectEdges(*sg), ElementsAre(edge));
  EXPECT_THAT(CollectNodes(*sg), UnorderedElementsAre(n0, n1));
  EXPECT_TRUE(n0->isVisited());
  EXPECT_TRUE(n1->isVisited());
}

TEST(ConnectedSubgraphFinderTest,
     LinearChainInsertedOutOfOrderProducesSingleSubgraph) {
  TestPlanarGraph graph;
  // Insert disconnected segments before bridging them.
  Edge* e3 = graph.AddEdge(Coordinate(3.0, 0.0), Coordinate(4.0, 0.0));
  Edge* e1 = graph.AddEdge(Coordinate(1.0, 0.0), Coordinate(2.0, 0.0));
  Edge* e0 = graph.AddEdge(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0));
  Edge* e2 = graph.AddEdge(Coordinate(2.0, 0.0), Coordinate(3.0, 0.0));

  ConnectedSubgraphFinder finder(graph);
  ScopedSubgraphList subgraphs;
  finder.getConnectedSubgraphs(subgraphs.items);

  ASSERT_EQ(subgraphs.items.size(), 1u);
  Subgraph* sg = subgraphs.items[0];
  ASSERT_NE(sg, nullptr);
  EXPECT_EQ(&sg->getParent(), &graph);
  EXPECT_THAT(CollectEdges(*sg), UnorderedElementsAre(e0, e1, e2, e3));
  EXPECT_THAT(CollectNodes(*sg),
              UnorderedElementsAre(graph.findNode(Coordinate(0.0, 0.0)),
                                   graph.findNode(Coordinate(1.0, 0.0)),
                                   graph.findNode(Coordinate(2.0, 0.0)),
                                   graph.findNode(Coordinate(3.0, 0.0)),
                                   graph.findNode(Coordinate(4.0, 0.0))));
}

TEST(ConnectedSubgraphFinderTest, TraversesBothDirectedEdgeOrientations) {
  TestPlanarGraph graph;
  // e0's dirEdge(0) goes (0,0)->(1,0); e1's dirEdge(0) goes (2,0)->(1,0),
  // requiring DFS from (0,0) to follow e1's opposite dirEdge(1).
  Edge* e0 = graph.AddEdge(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0));
  Edge* e1 = graph.AddEdge(Coordinate(2.0, 0.0), Coordinate(1.0, 0.0));

  ConnectedSubgraphFinder finder(graph);
  ScopedSubgraphList subgraphs;
  finder.getConnectedSubgraphs(subgraphs.items);

  ASSERT_EQ(subgraphs.items.size(), 1u);
  EXPECT_THAT(CollectEdges(*subgraphs.items[0]), UnorderedElementsAre(e0, e1));
  EXPECT_THAT(CollectNodes(*subgraphs.items[0]),
              UnorderedElementsAre(graph.findNode(Coordinate(0.0, 0.0)),
                                   graph.findNode(Coordinate(1.0, 0.0)),
                                   graph.findNode(Coordinate(2.0, 0.0))));
}

TEST(ConnectedSubgraphFinderTest,
     CyclicDiamondAndTriangleExerciseVisitedNeighborsAndDuplicateStackEntries) {
  TestPlanarGraph graph;
  const Coordinate p0(0.0, 0.0);
  const Coordinate p1(2.0, 0.0);
  const Coordinate p2(1.0, 2.0);
  const Coordinate p3(3.0, 2.0);

  // K4 minus one edge plus closing chord forms two triangles sharing p1-p2.
  Edge* e01 = graph.AddEdge(p0, p1);
  Edge* e02 = graph.AddEdge(p0, p2);
  Edge* e12 = graph.AddEdge(p1, p2);
  Edge* e13 = graph.AddEdge(p1, p3);
  Edge* e23 = graph.AddEdge(p2, p3);

  ConnectedSubgraphFinder finder(graph);
  ScopedSubgraphList subgraphs;
  finder.getConnectedSubgraphs(subgraphs.items);

  ASSERT_EQ(subgraphs.items.size(), 1u);
  Subgraph* sg = subgraphs.items[0];
  EXPECT_EQ(&sg->getParent(), &graph);
  EXPECT_THAT(CollectEdges(*sg), UnorderedElementsAre(e01, e02, e12, e13, e23));
  EXPECT_THAT(CollectNodes(*sg),
              UnorderedElementsAre(graph.findNode(p0), graph.findNode(p1),
                                   graph.findNode(p2), graph.findNode(p3)));
}

TEST(ConnectedSubgraphFinderTest, SelfLoopAndParallelEdges) {
  TestPlanarGraph graph;
  Edge* loop = graph.AddSelfLoop(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
                                 Coordinate(0.0, 1.0));
  Edge* parallel_a =
      graph.AddEdgeWithDirs(Coordinate(10.0, 0.0), Coordinate(11.0, 0.0),
                            Coordinate(10.5, 0.5), Coordinate(10.5, 0.5));
  Edge* parallel_b =
      graph.AddEdgeWithDirs(Coordinate(10.0, 0.0), Coordinate(11.0, 0.0),
                            Coordinate(10.5, -0.5), Coordinate(10.5, -0.5));

  ConnectedSubgraphFinder finder(graph);
  ScopedSubgraphList subgraphs;
  finder.getConnectedSubgraphs(subgraphs.items);

  ASSERT_EQ(subgraphs.items.size(), 2u);

  Subgraph* loop_sg = FindSubgraphContaining(subgraphs.items, loop);
  ASSERT_NE(loop_sg, nullptr);
  EXPECT_THAT(CollectEdges(*loop_sg), ElementsAre(loop));
  EXPECT_THAT(CollectNodes(*loop_sg),
              ElementsAre(graph.findNode(Coordinate(0.0, 0.0))));

  Subgraph* parallel_sg = FindSubgraphContaining(subgraphs.items, parallel_a);
  ASSERT_NE(parallel_sg, nullptr);
  EXPECT_NE(parallel_sg, loop_sg);
  EXPECT_THAT(CollectEdges(*parallel_sg),
              UnorderedElementsAre(parallel_a, parallel_b));
  EXPECT_THAT(CollectNodes(*parallel_sg),
              UnorderedElementsAre(graph.findNode(Coordinate(10.0, 0.0)),
                                   graph.findNode(Coordinate(11.0, 0.0))));
}

TEST(ConnectedSubgraphFinderTest,
     MultipleDisconnectedComponentsAndIsolatedNode) {
  TestPlanarGraph graph;

  // Component 1: single edge.
  Edge* c1_e0 = graph.AddEdge(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0));

  // Component 2: 3-edge star centered at (10, 10).
  Edge* c2_e0 = graph.AddEdge(Coordinate(10.0, 10.0), Coordinate(11.0, 10.0));
  Edge* c2_e1 = graph.AddEdge(Coordinate(10.0, 10.0), Coordinate(10.0, 11.0));
  Edge* c2_e2 = graph.AddEdge(Coordinate(10.0, 10.0), Coordinate(9.0, 10.0));

  // Component 3: 4-edge square cycle.
  Edge* c3_e0 = graph.AddEdge(Coordinate(20.0, 0.0), Coordinate(21.0, 0.0));
  Edge* c3_e1 = graph.AddEdge(Coordinate(21.0, 0.0), Coordinate(21.0, 1.0));
  Edge* c3_e2 = graph.AddEdge(Coordinate(21.0, 1.0), Coordinate(20.0, 1.0));
  Edge* c3_e3 = graph.AddEdge(Coordinate(20.0, 1.0), Coordinate(20.0, 0.0));

  // Isolated degree-0 node should not appear in any connected subgraph.
  Node* isolated = graph.GetOrCreateNode(Coordinate(100.0, 100.0));

  ConnectedSubgraphFinder finder(graph);
  ScopedSubgraphList subgraphs;
  finder.getConnectedSubgraphs(subgraphs.items);

  ASSERT_EQ(subgraphs.items.size(), 3u);
  for (Subgraph* sg : subgraphs.items) {
    ASSERT_NE(sg, nullptr);
    EXPECT_EQ(&sg->getParent(), &graph);
  }

  Subgraph* sg1 = FindSubgraphContaining(subgraphs.items, c1_e0);
  Subgraph* sg2 = FindSubgraphContaining(subgraphs.items, c2_e0);
  Subgraph* sg3 = FindSubgraphContaining(subgraphs.items, c3_e0);
  ASSERT_NE(sg1, nullptr);
  ASSERT_NE(sg2, nullptr);
  ASSERT_NE(sg3, nullptr);
  EXPECT_NE(sg1, sg2);
  EXPECT_NE(sg1, sg3);
  EXPECT_NE(sg2, sg3);

  EXPECT_THAT(CollectEdges(*sg1), ElementsAre(c1_e0));
  EXPECT_THAT(CollectNodes(*sg1),
              UnorderedElementsAre(graph.findNode(Coordinate(0.0, 0.0)),
                                   graph.findNode(Coordinate(1.0, 0.0))));

  EXPECT_THAT(CollectEdges(*sg2), UnorderedElementsAre(c2_e0, c2_e1, c2_e2));
  EXPECT_THAT(CollectNodes(*sg2),
              UnorderedElementsAre(graph.findNode(Coordinate(10.0, 10.0)),
                                   graph.findNode(Coordinate(11.0, 10.0)),
                                   graph.findNode(Coordinate(10.0, 11.0)),
                                   graph.findNode(Coordinate(9.0, 10.0))));

  EXPECT_THAT(CollectEdges(*sg3),
              UnorderedElementsAre(c3_e0, c3_e1, c3_e2, c3_e3));
  EXPECT_THAT(CollectNodes(*sg3),
              UnorderedElementsAre(graph.findNode(Coordinate(20.0, 0.0)),
                                   graph.findNode(Coordinate(21.0, 0.0)),
                                   graph.findNode(Coordinate(21.0, 1.0)),
                                   graph.findNode(Coordinate(20.0, 1.0))));

  EXPECT_FALSE(isolated->isVisited());
  std::set<Node*> seen_nodes;
  for (Subgraph* sg : subgraphs.items) {
    for (Node* node : CollectNodes(*sg)) {
      EXPECT_TRUE(seen_nodes.insert(node).second);
    }
  }
  EXPECT_EQ(seen_nodes.size(), 10u);
  EXPECT_EQ(seen_nodes.count(isolated), 0u);
}

TEST(ConnectedSubgraphFinderTest,
     ResetsPreExistingVisitedFlagsAndSupportsRepeatedCallsAfterEdits) {
  TestPlanarGraph graph;
  Edge* e1 = graph.AddEdge(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0));
  Edge* e2 = graph.AddEdge(Coordinate(5.0, 0.0), Coordinate(6.0, 0.0));

  for (auto it = graph.nodeBegin(); it != graph.nodeEnd(); ++it) {
    it->second->setVisited(true);
  }

  ConnectedSubgraphFinder finder(graph);
  ScopedSubgraphList first_run;
  finder.getConnectedSubgraphs(first_run.items);
  ASSERT_EQ(first_run.items.size(), 2u);

  // Merge the two components while all nodes currently have visited == true.
  Edge* bridge = graph.AddEdge(Coordinate(1.0, 0.0), Coordinate(5.0, 0.0));
  ScopedSubgraphList second_run;
  finder.getConnectedSubgraphs(second_run.items);
  ASSERT_EQ(second_run.items.size(), 1u);
  EXPECT_THAT(CollectEdges(*second_run.items[0]),
              UnorderedElementsAre(e1, e2, bridge));

  // Removing the bridge should split the graph back into two components.
  graph.remove(bridge);
  ScopedSubgraphList third_run;
  finder.getConnectedSubgraphs(third_run.items);
  ASSERT_EQ(third_run.items.size(), 2u);
  EXPECT_NE(FindSubgraphContaining(third_run.items, e1),
            FindSubgraphContaining(third_run.items, e2));
}

TEST(ConnectedSubgraphFinderTest, DeepChainUsesIterativeTraversal) {
  constexpr int kEdgeCount = 2000;
  TestPlanarGraph graph;
  std::vector<Edge*> expected_edges;
  expected_edges.reserve(kEdgeCount);
  for (int i = 0; i < kEdgeCount; ++i) {
    expected_edges.push_back(
        graph.AddEdge(Coordinate(static_cast<double>(i), 0.0),
                      Coordinate(static_cast<double>(i + 1), 0.0)));
  }

  ConnectedSubgraphFinder finder(graph);
  ScopedSubgraphList subgraphs;
  finder.getConnectedSubgraphs(subgraphs.items);

  ASSERT_EQ(subgraphs.items.size(), 1u);
  EXPECT_THAT(CollectEdges(*subgraphs.items[0]),
              UnorderedElementsAreArray(expected_edges));
  EXPECT_EQ(CollectNodes(*subgraphs.items[0]).size(),
            static_cast<std::size_t>(kEdgeCount + 1));
}

TEST(ConnectedSubgraphFinderTest, AppendsToNonEmptyOutputVector) {
  TestPlanarGraph graph;
  Edge* edge = graph.AddEdge(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0));

  ScopedSubgraphList subgraphs;
  auto* pre_existing = new Subgraph(graph);
  subgraphs.items.push_back(pre_existing);

  ConnectedSubgraphFinder finder(graph);
  finder.getConnectedSubgraphs(subgraphs.items);

  ASSERT_EQ(subgraphs.items.size(), 2u);
  EXPECT_EQ(subgraphs.items[0], pre_existing);
  EXPECT_THAT(CollectEdges(*subgraphs.items[0]), IsEmpty());
  EXPECT_THAT(CollectEdges(*subgraphs.items[1]), ElementsAre(edge));
}

static_assert(!std::is_copy_constructible_v<ConnectedSubgraphFinder>);
static_assert(!std::is_copy_assignable_v<ConnectedSubgraphFinder>);

}  // namespace
}  // namespace geos_google
