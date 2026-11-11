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

#include "third_party/geos/include/geos/index/quadtree/Node.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/quadtree/NodeBase.h"

namespace geos_google {
namespace {

using ::geos::geom::Envelope;
using ::geos::index::ItemVisitor;
using ::geos::index::quadtree::Node;
using ::geos::index::quadtree::NodeBase;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

class RecordingVisitor : public ItemVisitor {
 public:
  void visitItem(void* item) override { visited_.push_back(item); }

  const std::vector<void*>& visited() const { return visited_; }

 private:
  std::vector<void*> visited_;
};

TEST(NodeTest, DirectConstructionAndInitialState) {
  Node node(std::make_unique<Envelope>(0.0, 8.0, 4.0, 12.0), 3);

  ASSERT_NE(node.getEnvelope(), nullptr);
  EXPECT_EQ(*node.getEnvelope(), Envelope(0.0, 8.0, 4.0, 12.0));
  EXPECT_EQ(node.size(), 0u);
  EXPECT_EQ(node.depth(), 1u);
  EXPECT_FALSE(node.hasItems());
  EXPECT_FALSE(node.hasChildren());
  EXPECT_TRUE(node.isPrunable());
}

TEST(NodeTest, CreateNodeUsesKeyBoundsAndLevel) {
  std::unique_ptr<Node> node = Node::createNode(Envelope(1.5, 2.5, 1.5, 2.5));

  ASSERT_NE(node, nullptr);
  EXPECT_EQ(*node->getEnvelope(), Envelope(0.0, 4.0, 0.0, 4.0));
  EXPECT_THAT(node->toString(), HasSubstr("L2 Env[0:4,0:4] Ctr[2 2]"));
}

TEST(NodeTest, CreateExpandedFromNullNode) {
  std::unique_ptr<Node> node =
      Node::createExpanded(nullptr, Envelope(1.0, 2.0, 1.0, 2.0));

  ASSERT_NE(node, nullptr);
  EXPECT_EQ(*node->getEnvelope(), Envelope(0.0, 2.0, 0.0, 2.0));
  EXPECT_EQ(node->depth(), 1u);
  EXPECT_EQ(node->size(), 0u);
  EXPECT_FALSE(node->hasChildren());
}

TEST(NodeTest, CreateExpandedDirectChildPreservesExistingNodeAndItems) {
  std::unique_ptr<Node> small = Node::createNode(Envelope(0.5, 1.5, 0.5, 1.5));
  ASSERT_EQ(*small->getEnvelope(), Envelope(0.0, 2.0, 0.0, 2.0));
  int item = 101;
  small->add(&item);

  std::unique_ptr<Node> expanded =
      Node::createExpanded(std::move(small), Envelope(2.5, 3.5, 0.5, 1.5));
  ASSERT_NE(expanded, nullptr);
  EXPECT_EQ(*expanded->getEnvelope(), Envelope(0.0, 4.0, 0.0, 4.0));
  EXPECT_EQ(expanded->size(), 1u);
  EXPECT_EQ(expanded->depth(), 2u);
  EXPECT_TRUE(expanded->hasChildren());

  const Envelope child_query(0.5, 1.5, 0.5, 1.5);
  NodeBase* found = expanded->find(&child_query);
  ASSERT_NE(found, nullptr);
  EXPECT_NE(found, expanded.get());
  EXPECT_THAT(found->getItems(), UnorderedElementsAre(&item));

  std::vector<void*> all_items;
  expanded->addAllItems(all_items);
  EXPECT_THAT(all_items, UnorderedElementsAre(&item));
}

TEST(NodeTest, CreateExpandedIndirectChildCreatesIntermediateNodes) {
  std::unique_ptr<Node> tiny = Node::createNode(Envelope(0.25, 0.5, 0.25, 0.5));
  ASSERT_EQ(*tiny->getEnvelope(), Envelope(0.0, 0.5, 0.0, 0.5));
  int item = 202;
  tiny->add(&item);

  // [0, 7] x [0, 7] expands to level 3 ([0, 8] x [0, 8]), four levels above
  // level -1, creating levels 2, 1, and 0 along the path.
  std::unique_ptr<Node> expanded =
      Node::createExpanded(std::move(tiny), Envelope(6.0, 7.0, 6.0, 7.0));
  ASSERT_NE(expanded, nullptr);
  EXPECT_EQ(*expanded->getEnvelope(), Envelope(0.0, 8.0, 0.0, 8.0));
  EXPECT_EQ(expanded->depth(), 5u);
  EXPECT_EQ(expanded->size(), 1u);

  const Envelope tiny_query(0.3, 0.4, 0.3, 0.4);
  Node* tiny_node = dynamic_cast<Node*>(expanded->find(&tiny_query));
  ASSERT_NE(tiny_node, nullptr);
  EXPECT_EQ(*tiny_node->getEnvelope(), Envelope(0.0, 0.5, 0.0, 0.5));
  EXPECT_THAT(tiny_node->getItems(), UnorderedElementsAre(&item));
}

TEST(NodeTest, GetNodeCreatesAndReusesAllFourQuadrants) {
  Node root(std::make_unique<Envelope>(0.0, 8.0, 0.0, 8.0), 3);

  // Envelope crossing centre (4, 4) stays at root.
  const Envelope straddle(3.0, 5.0, 3.0, 5.0);
  EXPECT_EQ(root.getNode(&straddle), &root);
  EXPECT_FALSE(root.hasChildren());

  // Envelopes inside each quadrant that straddle their subquad's centre stop
  // at depth 2.
  const Envelope q0_env(1.0, 3.0, 1.0, 3.0);
  const Envelope q1_env(5.0, 7.0, 1.0, 3.0);
  const Envelope q2_env(1.0, 3.0, 5.0, 7.0);
  const Envelope q3_env(5.0, 7.0, 5.0, 7.0);

  Node* n0 = root.getNode(&q0_env);
  Node* n1 = root.getNode(&q1_env);
  Node* n2 = root.getNode(&q2_env);
  Node* n3 = root.getNode(&q3_env);

  ASSERT_NE(n0, nullptr);
  ASSERT_NE(n1, nullptr);
  ASSERT_NE(n2, nullptr);
  ASSERT_NE(n3, nullptr);
  EXPECT_EQ(*n0->getEnvelope(), Envelope(0.0, 4.0, 0.0, 4.0));
  EXPECT_EQ(*n1->getEnvelope(), Envelope(4.0, 8.0, 0.0, 4.0));
  EXPECT_EQ(*n2->getEnvelope(), Envelope(0.0, 4.0, 4.0, 8.0));
  EXPECT_EQ(*n3->getEnvelope(), Envelope(4.0, 8.0, 4.0, 8.0));
  EXPECT_EQ(root.depth(), 2u);

  // Re-querying reuses existing subnodes and descends deeper when contained.
  EXPECT_EQ(root.getNode(&q0_env), n0);
  const Envelope q0_deep_env(0.5, 1.5, 0.5, 1.5);
  Node* n0_deep = root.getNode(&q0_deep_env);
  ASSERT_NE(n0_deep, nullptr);
  EXPECT_NE(n0_deep, n0);
  EXPECT_EQ(*n0_deep->getEnvelope(), Envelope(0.0, 2.0, 0.0, 2.0));
  EXPECT_EQ(root.depth(), 3u);
}

TEST(NodeTest, FindReturnsSmallestExistingSubnodeWithoutCreatingNodes) {
  Node root(std::make_unique<Envelope>(0.0, 8.0, 0.0, 8.0), 3);

  const Envelope pt_q0(1.0, 1.0, 1.0, 1.0);
  EXPECT_EQ(root.find(&pt_q0), &root);
  EXPECT_FALSE(root.hasChildren());

  const Envelope q3_env(4.5, 7.5, 4.5, 7.5);
  Node* q3_node = root.getNode(&q3_env);
  ASSERT_NE(q3_node, nullptr);
  EXPECT_EQ(root.depth(), 2u);

  const Envelope straddle(3.0, 5.0, 4.0, 6.0);
  EXPECT_EQ(root.find(&straddle), &root);

  const Envelope pt_q3(5.0, 5.0, 5.0, 5.0);
  EXPECT_EQ(root.find(&pt_q3), q3_node);

  const Envelope pt_q1(6.0, 6.0, 2.0, 2.0);
  EXPECT_EQ(root.find(&pt_q1), &root);
  EXPECT_EQ(root.depth(), 2u);
}

TEST(NodeTest, InsertNodeDirectAndIndirectReplacingExistingSubnodes) {
  Node root(std::make_unique<Envelope>(0.0, 8.0, 0.0, 8.0), 3);

  // Create empty subnodes in quadrants 0 and 1 first to verify insertNode
  // safely replaces existing subnodes.
  const Envelope q0_area(1.0, 3.0, 1.0, 3.0);
  const Envelope q1_area(5.0, 7.0, 1.0, 3.0);
  ASSERT_NE(root.getNode(&q0_area), nullptr);
  ASSERT_NE(root.getNode(&q1_area), nullptr);

  auto direct =
      std::make_unique<Node>(std::make_unique<Envelope>(0.0, 4.0, 0.0, 4.0), 2);
  int item_direct = 1;
  direct->add(&item_direct);
  root.insertNode(std::move(direct));

  auto indirect =
      std::make_unique<Node>(std::make_unique<Envelope>(5.0, 6.0, 1.0, 2.0), 0);
  int item_indirect = 2;
  indirect->add(&item_indirect);
  root.insertNode(std::move(indirect));

  EXPECT_EQ(root.size(), 2u);
  EXPECT_EQ(root.depth(), 4u);

  const Envelope q0_sub(0.5, 1.5, 0.5, 1.5);
  EXPECT_THAT(root.find(&q0_sub)->getItems(),
              UnorderedElementsAre(&item_direct));

  const Envelope q1_sub(5.2, 5.8, 1.2, 1.8);
  Node* found_indirect = dynamic_cast<Node*>(root.find(&q1_sub));
  ASSERT_NE(found_indirect, nullptr);
  EXPECT_EQ(*found_indirect->getEnvelope(), Envelope(5.0, 6.0, 1.0, 2.0));
  EXPECT_THAT(found_indirect->getItems(), UnorderedElementsAre(&item_indirect));
}

TEST(NodeTest, SearchMatchFilteringAndRemoval) {
  Node root(std::make_unique<Envelope>(0.0, 8.0, 0.0, 8.0), 3);
  int item_root = 10;
  int item_q0 = 20;
  int item_q3 = 30;

  const Envelope env_root(3.0, 5.0, 3.0, 5.0);
  const Envelope env_q0(0.5, 1.5, 0.5, 1.5);
  const Envelope env_q3(5.0, 7.0, 5.0, 7.0);

  root.getNode(&env_root)->add(&item_root);
  root.getNode(&env_q0)->add(&item_q0);
  root.getNode(&env_q3)->add(&item_q3);

  // Disjoint envelope: isSearchMatch returns false on root.
  const Envelope disjoint(10.0, 12.0, 10.0, 12.0);
  std::vector<void*> matches;
  root.addAllItemsFromOverlapping(disjoint, matches);
  EXPECT_THAT(matches, IsEmpty());

  RecordingVisitor disjoint_visitor;
  root.visit(&disjoint, disjoint_visitor);
  EXPECT_THAT(disjoint_visitor.visited(), IsEmpty());
  EXPECT_FALSE(root.remove(&disjoint, &item_q3));

  // Boundary touch at (8, 8) matches root and subquad 3, not subquad 0.
  const Envelope corner_touch(8.0, 9.0, 8.0, 9.0);
  root.addAllItemsFromOverlapping(corner_touch, matches);
  EXPECT_THAT(matches, UnorderedElementsAre(&item_root, &item_q3));

  RecordingVisitor overlap_visitor;
  const Envelope q0_overlap(0.0, 2.0, 0.0, 2.0);
  root.visit(&q0_overlap, overlap_visitor);
  EXPECT_THAT(overlap_visitor.visited(),
              UnorderedElementsAre(&item_root, &item_q0));

  EXPECT_TRUE(root.remove(&env_q0, &item_q0));
  EXPECT_EQ(root.size(), 2u);
  EXPECT_FALSE(root.remove(&env_q0, &item_q0));
}

TEST(NodeTest, ToStringFormatsHierarchyAndNullSubnodes) {
  Node root(std::make_unique<Envelope>(0.0, 8.0, 0.0, 8.0), 3);
  int item = 1;
  root.add(&item);

  const std::string leaf_str = root.toString();
  EXPECT_THAT(leaf_str, HasSubstr("L3 Env[0:8,0:8] Ctr[4 4] ITEMS:1"));
  EXPECT_THAT(leaf_str, HasSubstr("subnode[0] NULL"));
  EXPECT_THAT(leaf_str, HasSubstr("subnode[3] NULL"));

  const Envelope q0_env(1.0, 3.0, 1.0, 3.0);
  root.getNode(&q0_env);
  const std::string tree_str = root.toString();
  EXPECT_THAT(tree_str, HasSubstr("subnode[0] L2 Env[0:4,0:4] Ctr[2 2]"));
  EXPECT_THAT(tree_str, HasSubstr("subnode[1] NULL"));
}

}  // namespace
}  // namespace geos_google
