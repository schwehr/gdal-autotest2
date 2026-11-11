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

// Unit tests for third_party/geos/src/index/bintree/NodeBase.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/bintree/NodeBase.cpp

#include "third_party/geos/include/geos/index/bintree/NodeBase.h"

#include <array>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/index/bintree/Interval.h"
#include "third_party/geos/include/geos/index/bintree/Node.h"

namespace geos_google {
namespace {

using geos::index::bintree::Interval;
using geos::index::bintree::Node;
using geos::index::bintree::NodeBase;
using ::testing::ElementsAre;

static_assert(!std::is_copy_constructible_v<NodeBase>);
static_assert(!std::is_copy_assignable_v<NodeBase>);

std::unique_ptr<Node> MakeNode(double min, double max, int level) {
  auto interval = std::make_unique<Interval>(min, max);
  return std::make_unique<Node>(interval.release(), level);
}

class TestNodeBase final : public NodeBase {
 public:
  explicit TestNodeBase(bool match_result = true)
      : match_result_(match_result) {}

  void set_match_result(bool match_result) { match_result_ = match_result; }

  void SetSubnode(int index, std::unique_ptr<Node> child) {
    std::unique_ptr<Node> previous(subnode[index]);
    subnode[index] = child.release();
  }

  Interval* last_query_interval() const { return last_query_interval_; }

 protected:
  bool isSearchMatch(Interval* interval) override {
    last_query_interval_ = interval;
    return match_result_;
  }

 private:
  bool match_result_;
  Interval* last_query_interval_ = nullptr;
};

class TrackedNode final : public Node {
 public:
  TrackedNode(double min, double max, int level, int* destroyed_count)
      : Node(std::make_unique<Interval>(min, max).release(), level),
        destroyed_count_(destroyed_count) {}

  ~TrackedNode() override { ++(*destroyed_count_); }

 private:
  int* destroyed_count_;
};

struct SubnodeIndexCase {
  std::string_view name;
  double min;
  double max;
  double centre;
  int expected_index;
};

constexpr auto kSubnodeIndexCases = std::to_array<SubnodeIndexCase>({
    {
        .name = "strictly_left",
        .min = 1.0,
        .max = 2.0,
        .centre = 5.0,
        .expected_index = 0,
    },
    {
        .name = "left_touching_centre",
        .min = 1.0,
        .max = 5.0,
        .centre = 5.0,
        .expected_index = 0,
    },
    {
        .name = "strictly_right",
        .min = 6.0,
        .max = 8.0,
        .centre = 5.0,
        .expected_index = 1,
    },
    {
        .name = "right_touching_centre",
        .min = 5.0,
        .max = 8.0,
        .centre = 5.0,
        .expected_index = 1,
    },
    {
        .name = "straddling_centre",
        .min = 4.0,
        .max = 6.0,
        .centre = 5.0,
        .expected_index = -1,
    },
    {
        .name = "zero_width_left",
        .min = 2.0,
        .max = 2.0,
        .centre = 5.0,
        .expected_index = 0,
    },
    {
        .name = "zero_width_right",
        .min = 7.0,
        .max = 7.0,
        .centre = 5.0,
        .expected_index = 1,
    },
    {
        .name = "zero_width_at_centre_prefers_left",
        .min = 5.0,
        .max = 5.0,
        .centre = 5.0,
        .expected_index = 0,
    },
    {
        .name = "negative_centre_straddling",
        .min = -6.0,
        .max = -4.0,
        .centre = -5.0,
        .expected_index = -1,
    },
    {
        .name = "negative_centre_left",
        .min = -10.0,
        .max = -5.0,
        .centre = -5.0,
        .expected_index = 0,
    },
    {
        .name = "negative_centre_right",
        .min = -5.0,
        .max = -1.0,
        .centre = -5.0,
        .expected_index = 1,
    },
});

TEST(NodeBaseTest, GetSubnodeIndexClassifiesRelativeToCentre) {
  for (const auto& tc : kSubnodeIndexCases) {
    SCOPED_TRACE(tc.name);
    Interval interval(tc.min, tc.max);
    EXPECT_EQ(NodeBase::getSubnodeIndex(&interval, tc.centre),
              tc.expected_index);
  }
}

TEST(NodeBaseTest, EmptyNodeDefaults) {
  TestNodeBase node;
  ASSERT_NE(node.getItems(), nullptr);
  EXPECT_TRUE(node.getItems()->empty());
  EXPECT_EQ(node.depth(), 1);
  EXPECT_EQ(node.size(), 0);
  EXPECT_EQ(node.nodeSize(), 1);
}

TEST(NodeBaseTest, AddAndGetItemsPreserveOrder) {
  TestNodeBase node;
  int item1 = 1;
  int item2 = 2;
  int item3 = 3;
  node.add(&item1);
  node.add(&item2);
  node.add(&item3);

  ASSERT_NE(node.getItems(), nullptr);
  EXPECT_THAT(*node.getItems(), ElementsAre(&item1, &item2, &item3));
  EXPECT_EQ(node.size(), 3);
  EXPECT_EQ(node.depth(), 1);
  EXPECT_EQ(node.nodeSize(), 1);
}

TEST(NodeBaseTest, DepthSizeAndNodeSizeReflectAsymmetricSubtrees) {
  int item = 0;
  {
    TestNodeBase right_only;
    right_only.SetSubnode(1, MakeNode(4.0, 8.0, 2));
    EXPECT_EQ(right_only.depth(), 2);
    EXPECT_EQ(right_only.size(), 0);
    EXPECT_EQ(right_only.nodeSize(), 2);
  }

  TestNodeBase root;
  root.add(&item);

  std::unique_ptr<Node> left = MakeNode(0.0, 4.0, 2);
  left->add(&item);
  left->add(&item);
  std::unique_ptr<Node> left_grandchild = MakeNode(0.0, 2.0, 1);
  left_grandchild->add(&item);
  left_grandchild->add(&item);
  left_grandchild->add(&item);
  left->insert(left_grandchild.release());
  root.SetSubnode(0, std::move(left));

  EXPECT_EQ(root.depth(), 3);
  EXPECT_EQ(root.size(), 6);
  EXPECT_EQ(root.nodeSize(), 3);

  // Add a shallower right branch so left depth > right depth.
  std::unique_ptr<Node> right = MakeNode(4.0, 8.0, 2);
  Node* raw_right = right.get();
  for (int i = 0; i < 4; ++i) {
    right->add(&item);
  }
  root.SetSubnode(1, std::move(right));

  EXPECT_EQ(root.depth(), 3);
  EXPECT_EQ(root.size(), 10);
  EXPECT_EQ(root.nodeSize(), 4);

  // Extend the right branch deeper than the left branch (depth 3 under root).
  std::unique_ptr<Node> right_great_grandchild = MakeNode(4.0, 5.0, 0);
  right_great_grandchild->add(&item);
  raw_right->insert(right_great_grandchild.release());

  EXPECT_EQ(root.depth(), 4);
  EXPECT_EQ(root.size(), 11);
  EXPECT_EQ(root.nodeSize(), 6);
}

TEST(NodeBaseTest, AddAllItemsAppendsNewItemsIntoNodeAndDescendants) {
  TestNodeBase root;
  int r0 = 0, l0 = 1, g0 = 2, r1 = 3, extra1 = 4, extra2 = 5;
  root.add(&r0);

  std::unique_ptr<Node> left = MakeNode(0.0, 4.0, 2);
  Node* left_ptr = left.get();
  left_ptr->add(&l0);

  std::unique_ptr<Node> grandchild = MakeNode(0.0, 2.0, 1);
  Node* grandchild_ptr = grandchild.get();
  grandchild_ptr->add(&g0);
  left_ptr->insert(grandchild.release());

  std::unique_ptr<Node> right = MakeNode(4.0, 8.0, 2);
  Node* right_ptr = right.get();
  right_ptr->add(&r1);

  root.SetSubnode(0, std::move(left));
  root.SetSubnode(1, std::move(right));

  std::vector<void*> empty_items;
  EXPECT_EQ(root.addAllItems(&empty_items), root.getItems());
  EXPECT_THAT(*root.getItems(), ElementsAre(&r0));

  std::vector<void*> new_items = {&extra1, &extra2};
  EXPECT_EQ(root.addAllItems(&new_items), root.getItems());

  EXPECT_THAT(*root.getItems(), ElementsAre(&r0, &extra1, &extra2));
  EXPECT_THAT(*left_ptr->getItems(), ElementsAre(&l0, &extra1, &extra2));
  EXPECT_THAT(*grandchild_ptr->getItems(), ElementsAre(&g0, &extra1, &extra2));
  EXPECT_THAT(*right_ptr->getItems(), ElementsAre(&r1, &extra1, &extra2));
  EXPECT_THAT(new_items, ElementsAre(&extra1, &extra2));
  EXPECT_EQ(root.size(), 12);
}

TEST(NodeBaseTest, AddAllItemsFromOverlappingRespectsSearchMatchAndRecurses) {
  TestNodeBase root(/*match_result=*/false);
  int r0 = 0, l0 = 1, g0 = 2, r1 = 3, sentinel = 99;
  root.add(&r0);

  // Leaf node check with no subnodes when match is true.
  {
    TestNodeBase leaf(/*match_result=*/true);
    leaf.add(&r0);
    Interval query(0.0, 1.0);
    std::vector<void*> results;
    EXPECT_EQ(leaf.addAllItemsFromOverlapping(&query, &results),
              leaf.getItems());
    EXPECT_THAT(results, ElementsAre(&r0));
  }

  std::unique_ptr<Node> left = MakeNode(0.0, 4.0, 2);
  std::unique_ptr<Node> grandchild = MakeNode(0.0, 2.0, 1);
  left->add(&l0);
  grandchild->add(&g0);
  left->insert(grandchild.release());

  std::unique_ptr<Node> right = MakeNode(4.0, 8.0, 2);
  right->add(&r1);

  root.SetSubnode(0, std::move(left));
  root.SetSubnode(1, std::move(right));

  {
    Interval query(0.0, 8.0);
    std::vector<void*> results = {&sentinel};
    EXPECT_EQ(root.addAllItemsFromOverlapping(&query, &results),
              root.getItems());
    EXPECT_EQ(root.last_query_interval(), &query);
    EXPECT_THAT(results, ElementsAre(&sentinel));
  }

  root.set_match_result(true);
  {
    Interval left_query(0.0, 2.5);
    std::vector<void*> results;
    EXPECT_EQ(root.addAllItemsFromOverlapping(&left_query, &results),
              root.getItems());
    EXPECT_THAT(results, ElementsAre(&r0, &l0, &g0));
  }
  {
    Interval right_query(5.0, 6.0);
    std::vector<void*> results;
    root.addAllItemsFromOverlapping(&right_query, &results);
    EXPECT_THAT(results, ElementsAre(&r0, &r1));
  }
  {
    Interval all_query(0.0, 8.0);
    std::vector<void*> results;
    root.addAllItemsFromOverlapping(&all_query, &results);
    EXPECT_THAT(results, ElementsAre(&r0, &l0, &g0, &r1));
  }
}

TEST(NodeBaseTest, DestructorRecursivelyDeletesSubnodes) {
  int destroyed_count = 0;
  {
    TestNodeBase root;
    auto left = std::make_unique<TrackedNode>(0.0, 4.0, 2, &destroyed_count);
    auto grandchild =
        std::make_unique<TrackedNode>(0.0, 2.0, 1, &destroyed_count);
    auto right = std::make_unique<TrackedNode>(4.0, 8.0, 2, &destroyed_count);

    left->insert(grandchild.release());
    root.SetSubnode(0, std::move(left));
    root.SetSubnode(1, std::move(right));
    EXPECT_EQ(root.nodeSize(), 4);
    EXPECT_EQ(destroyed_count, 0);
  }
  EXPECT_EQ(destroyed_count, 3);
}

}  // namespace
}  // namespace geos_google
