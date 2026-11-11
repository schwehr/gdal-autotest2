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

// Unit tests for third_party/geos/src/index/bintree/Node.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/bintree/Node.cpp

#include "third_party/geos/include/geos/index/bintree/Node.h"

#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/index/bintree/Interval.h"
#include "third_party/geos/include/geos/index/bintree/NodeBase.h"

namespace geos_google {
namespace {

using geos::index::bintree::Interval;
using geos::index::bintree::Node;
using geos::index::bintree::NodeBase;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

std::unique_ptr<Node> MakeNode(double min, double max, int level) {
  auto interval = std::make_unique<Interval>(min, max);
  return std::make_unique<Node>(interval.release(), level);
}

TEST(NodeTest, DirectConstructionOwnsIntervalAndStartsEmpty) {
  std::unique_ptr<Node> node = MakeNode(0.0, 8.0, 3);
  ASSERT_NE(node->getInterval(), nullptr);
  EXPECT_DOUBLE_EQ(node->getInterval()->getMin(), 0.0);
  EXPECT_DOUBLE_EQ(node->getInterval()->getMax(), 8.0);
  EXPECT_DOUBLE_EQ(node->getInterval()->getWidth(), 8.0);
  EXPECT_EQ(node->depth(), 1);
  EXPECT_EQ(node->size(), 0);
  EXPECT_EQ(node->nodeSize(), 1);
}

TEST(NodeTest, CreateNodeBuildsPowerOfTwoIntervalFromKey) {
  {
    Interval input(1.5, 2.5);
    std::unique_ptr<Node> node(Node::createNode(&input));
    // Mutating caller interval must not affect node interval.
    input.init(10.0, 20.0);
    ASSERT_NE(node, nullptr);
    EXPECT_DOUBLE_EQ(node->getInterval()->getMin(), 0.0);
    EXPECT_DOUBLE_EQ(node->getInterval()->getMax(), 4.0);
    EXPECT_EQ(node->depth(), 1);
    EXPECT_EQ(node->nodeSize(), 1);
  }
  {
    Interval negative(-3.5, -1.5);
    std::unique_ptr<Node> node(Node::createNode(&negative));
    ASSERT_NE(node, nullptr);
    EXPECT_DOUBLE_EQ(node->getInterval()->getMin(), -4.0);
    EXPECT_DOUBLE_EQ(node->getInterval()->getMax(), 0.0);
  }
  {
    Interval point(2.25, 2.25);
    std::unique_ptr<Node> node(Node::createNode(&point));
    ASSERT_NE(node, nullptr);
    EXPECT_DOUBLE_EQ(node->getInterval()->getMin(), 2.0);
    EXPECT_DOUBLE_EQ(node->getInterval()->getMax(), 3.0);
  }
}

TEST(NodeTest, CreateExpandedFromNullCreatesStandaloneNode) {
  Interval add(2.5, 3.5);
  std::unique_ptr<Node> expanded(Node::createExpanded(nullptr, &add));
  ASSERT_NE(expanded, nullptr);
  EXPECT_DOUBLE_EQ(expanded->getInterval()->getMin(), 2.0);
  EXPECT_DOUBLE_EQ(expanded->getInterval()->getMax(), 4.0);
  EXPECT_EQ(expanded->depth(), 1);
  EXPECT_EQ(expanded->size(), 0);
  EXPECT_EQ(expanded->nodeSize(), 1);
}

TEST(NodeTest, CreateExpandedWrapsExistingNodeRightwardLeftwardAndMultiLevel) {
  {
    Interval first(0.5, 1.5);
    std::unique_ptr<Node> existing(Node::createNode(&first));
    Node* raw_existing = existing.get();
    int item = 1;
    raw_existing->add(&item);

    Interval add(2.5, 3.5);
    std::unique_ptr<Node> expanded(
        Node::createExpanded(existing.release(), &add));
    ASSERT_NE(expanded, nullptr);
    EXPECT_DOUBLE_EQ(expanded->getInterval()->getMin(), 0.0);
    EXPECT_DOUBLE_EQ(expanded->getInterval()->getMax(), 4.0);
    EXPECT_EQ(expanded->depth(), 2);
    EXPECT_EQ(expanded->size(), 1);
    EXPECT_EQ(expanded->nodeSize(), 2);

    Interval search_left(0.25, 0.75);
    EXPECT_EQ(expanded->find(&search_left), raw_existing);
    Interval search_right(2.25, 2.75);
    EXPECT_EQ(expanded->find(&search_right), expanded.get());
  }
  {
    Interval first(6.5, 7.5);
    std::unique_ptr<Node> existing(Node::createNode(&first));
    Node* raw_existing = existing.get();
    int item = 2;
    raw_existing->add(&item);

    Interval add(4.5, 5.5);
    std::unique_ptr<Node> expanded(
        Node::createExpanded(existing.release(), &add));
    ASSERT_NE(expanded, nullptr);
    EXPECT_DOUBLE_EQ(expanded->getInterval()->getMin(), 4.0);
    EXPECT_DOUBLE_EQ(expanded->getInterval()->getMax(), 8.0);
    EXPECT_EQ(expanded->depth(), 2);
    EXPECT_EQ(expanded->size(), 1);
    EXPECT_EQ(expanded->nodeSize(), 2);

    Interval search_right(6.25, 6.75);
    EXPECT_EQ(expanded->find(&search_right), raw_existing);
    Interval search_left(4.25, 4.75);
    EXPECT_EQ(expanded->find(&search_left), expanded.get());
  }
  {
    Interval small(0.25, 0.75);
    std::unique_ptr<Node> existing(Node::createNode(&small));
    Node* raw_existing = existing.get();
    int item = 3;
    raw_existing->add(&item);

    Interval far(7.0, 7.5);
    std::unique_ptr<Node> expanded(
        Node::createExpanded(existing.release(), &far));
    ASSERT_NE(expanded, nullptr);
    EXPECT_DOUBLE_EQ(expanded->getInterval()->getMin(), 0.0);
    EXPECT_DOUBLE_EQ(expanded->getInterval()->getMax(), 8.0);
    EXPECT_EQ(expanded->depth(), 4);
    EXPECT_EQ(expanded->size(), 1);
    EXPECT_EQ(expanded->nodeSize(), 4);

    Interval search_small(0.25, 0.75);
    EXPECT_EQ(expanded->find(&search_small), raw_existing);
  }
}

TEST(NodeTest, GetNodeCreatesAndReusesSubnodesAcrossBothHalves) {
  std::unique_ptr<Node> root = MakeNode(0.0, 8.0, 3);

  Interval straddling(3.0, 5.0);
  EXPECT_EQ(root->getNode(&straddling), root.get());
  EXPECT_EQ(root->nodeSize(), 1);

  Interval left_query(0.5, 1.5);
  Node* left_leaf = root->getNode(&left_query);
  ASSERT_NE(left_leaf, nullptr);
  EXPECT_DOUBLE_EQ(left_leaf->getInterval()->getMin(), 0.0);
  EXPECT_DOUBLE_EQ(left_leaf->getInterval()->getMax(), 2.0);
  EXPECT_EQ(root->depth(), 3);
  EXPECT_EQ(root->nodeSize(), 3);

  // Subsequent lookup in the same subnode must reuse existing nodes.
  Interval same_left_query(0.25, 1.75);
  EXPECT_EQ(root->getNode(&same_left_query), left_leaf);
  EXPECT_EQ(root->nodeSize(), 3);

  Interval right_mid_query(4.0, 6.5);
  Node* right_mid = root->getNode(&right_mid_query);
  ASSERT_NE(right_mid, nullptr);
  EXPECT_DOUBLE_EQ(right_mid->getInterval()->getMin(), 4.0);
  EXPECT_DOUBLE_EQ(right_mid->getInterval()->getMax(), 8.0);
  EXPECT_EQ(root->nodeSize(), 4);

  Interval deep_right_query(6.1, 6.4);
  Node* deep_leaf = root->getNode(&deep_right_query);
  ASSERT_NE(deep_leaf, nullptr);
  EXPECT_DOUBLE_EQ(deep_leaf->getInterval()->getMin(), 6.0);
  EXPECT_DOUBLE_EQ(deep_leaf->getInterval()->getMax(), 6.5);
  EXPECT_EQ(root->depth(), 5);
  EXPECT_EQ(root->nodeSize(), 7);
}

TEST(NodeTest, FindReturnsSmallestExistingNodeWithoutMutatingTree) {
  std::unique_ptr<Node> root = MakeNode(0.0, 8.0, 3);

  // No subnodes exist yet: straddling, zero-width, and half queries hit root.
  Interval straddling(3.0, 5.0);
  Interval point_left(0.5, 0.5);
  Interval right_query(5.0, 6.0);
  EXPECT_EQ(root->find(&straddling), root.get());
  EXPECT_EQ(root->find(&point_left), root.get());
  EXPECT_EQ(root->find(&right_query), root.get());
  EXPECT_EQ(root->nodeSize(), 1);

  // Populate [0, 8] -> [0, 4] -> [0, 2].
  Interval populate_left(0.5, 1.5);
  NodeBase* left_leaf = root->getNode(&populate_left);
  ASSERT_EQ(root->nodeSize(), 3);

  // Point query inside [0, 2] and point exactly on centre 1.0 return left_leaf.
  EXPECT_EQ(root->find(&point_left), left_leaf);
  Interval point_at_centre(1.0, 1.0);
  EXPECT_EQ(root->find(&point_at_centre), left_leaf);
  EXPECT_EQ(root->find(&populate_left), left_leaf);

  // Query in [2, 4] descends to [0, 4] whose right child [2, 4] is absent.
  Interval upper_left_query(2.5, 3.5);
  NodeBase* mid_left = root->find(&upper_left_query);
  Node* mid_left_node = dynamic_cast<Node*>(mid_left);
  ASSERT_NE(mid_left_node, nullptr);
  EXPECT_DOUBLE_EQ(mid_left_node->getInterval()->getMin(), 0.0);
  EXPECT_DOUBLE_EQ(mid_left_node->getInterval()->getMax(), 4.0);

  // Queries in unpopulated right half or outside root interval stop at root.
  EXPECT_EQ(root->find(&right_query), root.get());
  Interval outside(10.0, 12.0);
  EXPECT_EQ(root->find(&outside), root.get());
  EXPECT_EQ(root->nodeSize(), 3);
}

TEST(NodeTest, InsertPlacesDirectAndIndirectDescendantsInCorrectHalves) {
  std::unique_ptr<Node> root = MakeNode(0.0, 8.0, 3);

  // Direct child at level - 1 on the left.
  std::unique_ptr<Node> direct_left = MakeNode(0.0, 4.0, 2);
  Node* raw_direct_left = direct_left.get();
  int left_item = 10;
  raw_direct_left->add(&left_item);
  root->insert(direct_left.release());

  EXPECT_EQ(root->depth(), 2);
  EXPECT_EQ(root->size(), 1);
  EXPECT_EQ(root->nodeSize(), 2);
  Interval left_probe(1.0, 3.0);
  EXPECT_EQ(root->find(&left_probe), raw_direct_left);

  // Indirect descendant two levels down on the right creates [4, 8].
  std::unique_ptr<Node> indirect_right = MakeNode(6.0, 8.0, 1);
  Node* raw_indirect_right = indirect_right.get();
  int right_item = 20;
  raw_indirect_right->add(&right_item);
  root->insert(indirect_right.release());

  EXPECT_EQ(root->depth(), 3);
  EXPECT_EQ(root->size(), 2);
  EXPECT_EQ(root->nodeSize(), 4);
  Interval right_probe(6.5, 7.5);
  EXPECT_EQ(root->find(&right_probe), raw_indirect_right);

  // Also test indirect insertion down the left branch (case 0 in
  // createSubnode).
  std::unique_ptr<Node> root2 = MakeNode(0.0, 8.0, 3);
  std::unique_ptr<Node> indirect_left = MakeNode(0.0, 2.0, 1);
  Node* raw_indirect_left = indirect_left.get();
  root2->insert(indirect_left.release());
  EXPECT_EQ(root2->depth(), 3);
  EXPECT_EQ(root2->nodeSize(), 3);
  Interval deep_left_probe(0.5, 1.5);
  EXPECT_EQ(root2->find(&deep_left_probe), raw_indirect_left);
}

TEST(NodeTest, IsSearchMatchFiltersOverlappingAndDisjointQueries) {
  std::unique_ptr<Node> root = MakeNode(0.0, 8.0, 3);
  int item_root = 1;
  int item_left = 2;
  int item_right = 3;
  int item_right_leaf = 4;

  root->add(&item_root);
  Interval iv_left(0.5, 1.5);
  root->getNode(&iv_left)->add(&item_left);
  Interval iv_right(4.5, 7.5);
  root->getNode(&iv_right)->add(&item_right);
  Interval iv_right_leaf(6.2, 6.8);
  root->getNode(&iv_right_leaf)->add(&item_right_leaf);

  {
    Interval disjoint(-2.0, -0.5);
    std::vector<void*> results;
    root->addAllItemsFromOverlapping(&disjoint, &results);
    EXPECT_THAT(results, IsEmpty());
  }
  {
    // Touching at min=0.0 overlaps [0, 8], [0, 4], [0, 2], but not [4, 8].
    Interval touching_zero(-1.0, 0.0);
    std::vector<void*> results;
    root->addAllItemsFromOverlapping(&touching_zero, &results);
    EXPECT_THAT(results, UnorderedElementsAre(&item_root, &item_left));
  }
  {
    Interval right_only(5.0, 5.5);
    std::vector<void*> results;
    root->addAllItemsFromOverlapping(&right_only, &results);
    EXPECT_THAT(results, ElementsAre(&item_root, &item_right));
  }
  {
    Interval all_overlapping(1.0, 6.5);
    std::vector<void*> results;
    root->addAllItemsFromOverlapping(&all_overlapping, &results);
    EXPECT_THAT(results, UnorderedElementsAre(&item_root, &item_left,
                                              &item_right, &item_right_leaf));
  }
}

}  // namespace
}  // namespace geos_google
