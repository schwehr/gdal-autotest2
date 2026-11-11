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

#include "third_party/geos/include/geos/planargraph/Subgraph.h"

#include <cstddef>
#include <iterator>
#include <memory>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdge.h"
#include "third_party/geos/include/geos/planargraph/Edge.h"
#include "third_party/geos/include/geos/planargraph/Node.h"
#include "third_party/geos/include/geos/planargraph/PlanarGraph.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::planargraph::DirectedEdge;
using ::geos::planargraph::Edge;
using ::geos::planargraph::Node;
using ::geos::planargraph::PlanarGraph;
using ::geos::planargraph::Subgraph;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

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

std::vector<Edge*> CollectEdges(Subgraph& subgraph) {
  return std::vector<Edge*>(subgraph.edgeBegin(), subgraph.edgeEnd());
}

std::vector<Node*> CollectNodes(Subgraph& subgraph) {
  std::vector<Node*> nodes;
  for (auto it = subgraph.nodeBegin(); it != subgraph.nodeEnd(); ++it) {
    nodes.push_back(it->second);
  }
  return nodes;
}

std::vector<Node*> CollectNodesConst(const Subgraph& subgraph) {
  std::vector<Node*> nodes;
  for (auto it = subgraph.nodeBegin(); it != subgraph.nodeEnd(); ++it) {
    nodes.push_back(it->second);
  }
  return nodes;
}

std::vector<const DirectedEdge*> CollectDirEdges(Subgraph& subgraph,
                                                 std::size_t count) {
  std::vector<const DirectedEdge*> result;
  result.reserve(count);
  auto it = subgraph.getDirEdgeBegin();
  for (std::size_t i = 0; i < count; ++i, ++it) {
    result.push_back(*it);
  }
  return result;
}

TEST(SubgraphTest, EmptySubgraphStateAndParentReference) {
  TestPlanarGraph graph;
  Edge* edge = graph.AddEdge(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0));
  graph.GetOrCreateNode(Coordinate(5.0, 5.0));

  Subgraph subgraph(graph);
  const Subgraph& const_subgraph = subgraph;

  EXPECT_EQ(&subgraph.getParent(), &graph);
  EXPECT_EQ(&const_subgraph.getParent(), &graph);
  EXPECT_EQ(subgraph.edgeBegin(), subgraph.edgeEnd());
  EXPECT_EQ(subgraph.nodeBegin(), subgraph.nodeEnd());
  EXPECT_EQ(const_subgraph.nodeBegin(), const_subgraph.nodeEnd());
  EXPECT_FALSE(subgraph.contains(edge));
  EXPECT_FALSE(subgraph.contains(nullptr));
}

TEST(SubgraphTest, AddSingleEdgePopulatesEdgesDirEdgesAndNodes) {
  TestPlanarGraph graph;
  Edge* e1 = graph.AddEdge(Coordinate(0.0, 0.0), Coordinate(10.0, 0.0));
  Edge* e2 = graph.AddEdge(Coordinate(10.0, 0.0), Coordinate(20.0, 0.0));

  Subgraph subgraph(graph);
  const auto [it, inserted] = subgraph.add(e1);
  EXPECT_TRUE(inserted);
  ASSERT_NE(it, subgraph.edgeEnd());
  EXPECT_EQ(*it, e1);

  EXPECT_TRUE(subgraph.contains(e1));
  EXPECT_FALSE(subgraph.contains(e2));
  EXPECT_THAT(CollectEdges(subgraph), ElementsAre(e1));

  const std::vector<const DirectedEdge*> expected_dir_edges = {
      e1->getDirEdge(0),
      e1->getDirEdge(1),
  };
  EXPECT_THAT(CollectDirEdges(subgraph, 2),
              ElementsAreArray(expected_dir_edges));

  Node* n0 = graph.findNode(Coordinate(0.0, 0.0));
  Node* n1 = graph.findNode(Coordinate(10.0, 0.0));
  ASSERT_NE(n0, nullptr);
  ASSERT_NE(n1, nullptr);
  EXPECT_THAT(CollectNodes(subgraph), ElementsAre(n0, n1));
  EXPECT_THAT(CollectNodesConst(subgraph), ElementsAre(n0, n1));
}

TEST(SubgraphTest, AddDuplicateEdgeIsNoOpAndPreservesDirEdgeOrder) {
  TestPlanarGraph graph;
  Edge* e1 = graph.AddEdge(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0));
  Edge* e2 = graph.AddEdge(Coordinate(1.0, 0.0), Coordinate(2.0, 0.0));

  Subgraph subgraph(graph);
  const auto [first_it, first_inserted] = subgraph.add(e1);
  EXPECT_TRUE(first_inserted);
  EXPECT_EQ(*first_it, e1);

  const auto [dup_it, dup_inserted] = subgraph.add(e1);
  EXPECT_FALSE(dup_inserted);
  EXPECT_EQ(*dup_it, e1);

  const auto [second_it, second_inserted] = subgraph.add(e2);
  EXPECT_TRUE(second_inserted);
  EXPECT_EQ(*second_it, e2);

  EXPECT_THAT(CollectEdges(subgraph), UnorderedElementsAre(e1, e2));

  // If the duplicate add had appended dirEdges, indices 2..3 would repeat e1.
  const std::vector<const DirectedEdge*> expected_dir_edges = {
      e1->getDirEdge(0),
      e1->getDirEdge(1),
      e2->getDirEdge(0),
      e2->getDirEdge(1),
  };
  EXPECT_THAT(CollectDirEdges(subgraph, 4),
              ElementsAreArray(expected_dir_edges));

  Node* n0 = graph.findNode(Coordinate(0.0, 0.0));
  Node* n1 = graph.findNode(Coordinate(1.0, 0.0));
  Node* n2 = graph.findNode(Coordinate(2.0, 0.0));
  EXPECT_THAT(CollectNodes(subgraph), ElementsAre(n0, n1, n2));
  EXPECT_THAT(CollectNodesConst(subgraph), ElementsAre(n0, n1, n2));
}

TEST(SubgraphTest, AddSelfLoopRegistersOneNodeAndTwoDirectedEdges) {
  TestPlanarGraph graph;
  Edge* loop = graph.AddSelfLoop(Coordinate(3.0, 4.0), Coordinate(4.0, 4.0),
                                 Coordinate(3.0, 5.0));

  Subgraph subgraph(graph);
  const auto [it, inserted] = subgraph.add(loop);
  EXPECT_TRUE(inserted);
  EXPECT_EQ(*it, loop);
  EXPECT_TRUE(subgraph.contains(loop));
  EXPECT_THAT(CollectEdges(subgraph), ElementsAre(loop));

  const std::vector<const DirectedEdge*> expected_dir_edges = {
      loop->getDirEdge(0),
      loop->getDirEdge(1),
  };
  EXPECT_THAT(CollectDirEdges(subgraph, 2),
              ElementsAreArray(expected_dir_edges));

  Node* node = graph.findNode(Coordinate(3.0, 4.0));
  ASSERT_NE(node, nullptr);
  EXPECT_THAT(CollectNodes(subgraph), ElementsAre(node));
  EXPECT_THAT(CollectNodesConst(subgraph), ElementsAre(node));
}

TEST(SubgraphTest, IndependentSubgraphsOverSameParentDoNotMutateParent) {
  TestPlanarGraph graph;
  Edge* e0 = graph.AddEdge(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0));
  Edge* e1 = graph.AddEdge(Coordinate(1.0, 0.0), Coordinate(2.0, 0.0));
  Edge* e2 = graph.AddEdge(Coordinate(10.0, 0.0), Coordinate(11.0, 0.0));
  Edge* e3 = graph.AddEdge(Coordinate(11.0, 0.0), Coordinate(12.0, 0.0));
  Node* isolated = graph.GetOrCreateNode(Coordinate(50.0, 50.0));

  Subgraph sub_a(graph);
  Subgraph sub_b(graph);
  EXPECT_TRUE(sub_a.add(e0).second);
  EXPECT_TRUE(sub_a.add(e2).second);
  EXPECT_TRUE(sub_b.add(e2).second);
  EXPECT_TRUE(sub_b.add(e3).second);

  EXPECT_TRUE(sub_a.contains(e0));
  EXPECT_FALSE(sub_a.contains(e1));
  EXPECT_TRUE(sub_a.contains(e2));
  EXPECT_FALSE(sub_a.contains(e3));

  EXPECT_FALSE(sub_b.contains(e0));
  EXPECT_FALSE(sub_b.contains(e1));
  EXPECT_TRUE(sub_b.contains(e2));
  EXPECT_TRUE(sub_b.contains(e3));

  EXPECT_THAT(CollectEdges(sub_a), UnorderedElementsAre(e0, e2));
  EXPECT_THAT(CollectEdges(sub_b), UnorderedElementsAre(e2, e3));

  Node* n0 = graph.findNode(Coordinate(0.0, 0.0));
  Node* n1 = graph.findNode(Coordinate(1.0, 0.0));
  Node* n10 = graph.findNode(Coordinate(10.0, 0.0));
  Node* n11 = graph.findNode(Coordinate(11.0, 0.0));
  Node* n12 = graph.findNode(Coordinate(12.0, 0.0));

  EXPECT_THAT(CollectNodes(sub_a), ElementsAre(n0, n1, n10, n11));
  EXPECT_THAT(CollectNodes(sub_b), ElementsAre(n10, n11, n12));

  // Verify parent graph state and flags remain untouched.
  EXPECT_THAT(*graph.getEdges(), ElementsAre(e0, e1, e2, e3));
  EXPECT_FALSE(isolated->isVisited());
  EXPECT_FALSE(e0->isVisited());
  EXPECT_FALSE(e2->isMarked());
  EXPECT_FALSE(n10->isVisited());
}

TEST(SubgraphTest, NodesAreOrderedByCoordinateLessThan) {
  TestPlanarGraph graph;
  Edge* e0 = graph.AddEdge(Coordinate(10.0, 5.0), Coordinate(0.0, 10.0));
  Edge* e1 = graph.AddEdge(Coordinate(0.0, 2.0), Coordinate(-3.0, 7.0));

  Subgraph subgraph(graph);
  EXPECT_TRUE(subgraph.add(e0).second);
  EXPECT_TRUE(subgraph.add(e1).second);

  Node* n_neg3_7 = graph.findNode(Coordinate(-3.0, 7.0));
  Node* n_0_2 = graph.findNode(Coordinate(0.0, 2.0));
  Node* n_0_10 = graph.findNode(Coordinate(0.0, 10.0));
  Node* n_10_5 = graph.findNode(Coordinate(10.0, 5.0));

  EXPECT_THAT(CollectNodes(subgraph),
              ElementsAre(n_neg3_7, n_0_2, n_0_10, n_10_5));
  EXPECT_THAT(CollectNodesConst(subgraph),
              ElementsAre(n_neg3_7, n_0_2, n_0_10, n_10_5));

  // Verify getDirEdgeBegin still starts at the first inserted edge even when
  // nodeMap reorders nodes by coordinate.
  EXPECT_EQ(*subgraph.getDirEdgeBegin(), e0->getDirEdge(0));
  EXPECT_EQ(*std::next(subgraph.getDirEdgeBegin(), 2), e1->getDirEdge(0));
}

TEST(SubgraphTest, SubgraphWithNoMatchingEdgesReturnsEmptyCollections) {
  TestPlanarGraph graph;
  Subgraph subgraph(graph);

  EXPECT_THAT(CollectEdges(subgraph), IsEmpty());
  EXPECT_THAT(CollectNodes(subgraph), IsEmpty());
  EXPECT_THAT(CollectNodesConst(subgraph), IsEmpty());
}

static_assert(!std::is_copy_constructible_v<Subgraph>);
static_assert(!std::is_copy_assignable_v<Subgraph>);

}  // namespace
}  // namespace geos_google
