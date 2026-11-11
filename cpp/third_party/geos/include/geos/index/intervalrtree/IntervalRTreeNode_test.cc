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

// Unit tests for
// third_party/geos/include/geos/index/intervalrtree/IntervalRTreeNode.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/index/intervalrtree/IntervalRTreeNode.h

#include "third_party/geos/include/geos/index/intervalrtree/IntervalRTreeNode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/intervalrtree/IntervalRTreeBranchNode.h"
#include "third_party/geos/include/geos/index/intervalrtree/IntervalRTreeLeafNode.h"

namespace geos_google {
namespace {

using geos::index::ItemVisitor;
using geos::index::intervalrtree::IntervalRTreeBranchNode;
using geos::index::intervalrtree::IntervalRTreeLeafNode;
using geos::index::intervalrtree::IntervalRTreeNode;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

class TestIntervalRTreeNode final : public IntervalRTreeNode {
 public:
  explicit TestIntervalRTreeNode(bool* destroyed = nullptr)
      : IntervalRTreeNode(), destroyed_(destroyed) {}

  TestIntervalRTreeNode(double p_min, double p_max, void* item = nullptr,
                        bool* destroyed = nullptr)
      : IntervalRTreeNode(p_min, p_max), item_(item), destroyed_(destroyed) {}

  ~TestIntervalRTreeNode() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  bool Intersects(double query_min, double query_max) const {
    return intersects(query_min, query_max);
  }

  void query(double query_min, double query_max,
             ItemVisitor* visitor) const override {
    if (intersects(query_min, query_max) && visitor != nullptr &&
        item_ != nullptr) {
      visitor->visitItem(item_);
    }
  }

 private:
  void* item_ = nullptr;
  bool* destroyed_ = nullptr;
};

class CollectingItemVisitor final : public ItemVisitor {
 public:
  void visitItem(void* item) override {
    if (item != nullptr) {
      items_.push_back(*static_cast<const int*>(item));
    }
  }

  const std::vector<int>& items() const { return items_; }

 private:
  std::vector<int> items_;
};

TEST(IntervalRTreeNodeTest, CompileTimeTraitsAndTypes) {
  static_assert(std::is_abstract_v<IntervalRTreeNode>);
  static_assert(std::has_virtual_destructor_v<IntervalRTreeNode>);
  static_assert(std::is_same_v<IntervalRTreeNode::ConstVect,
                               std::vector<const IntervalRTreeNode*>>);

  EXPECT_TRUE(std::is_abstract_v<IntervalRTreeNode>);
  EXPECT_TRUE(std::has_virtual_destructor_v<IntervalRTreeNode>);
}

TEST(IntervalRTreeNodeTest, DefaultConstructorSetsInvertedInfiniteBounds) {
  const TestIntervalRTreeNode node;
  EXPECT_TRUE(std::isinf(node.getMin()));
  EXPECT_GT(node.getMin(), 0.0);
  EXPECT_EQ(node.getMin(), geos::DoubleInfinity);

  EXPECT_TRUE(std::isinf(node.getMax()));
  EXPECT_LT(node.getMax(), 0.0);
  EXPECT_EQ(node.getMax(), geos::DoubleNegInfinity);
}

TEST(IntervalRTreeNodeTest, ParameterizedConstructorStoresBounds) {
  struct BoundsCase {
    std::string_view name;
    double min;
    double max;
  };
  constexpr auto kCases = std::to_array<BoundsCase>({
      {.name = "positive_range", .min = 1.25, .max = 9.75},
      {.name = "negative_range", .min = -10.5, .max = -2.5},
      {.name = "straddling_zero", .min = -4.0, .max = 6.0},
      {.name = "point_interval", .min = 3.5, .max = 3.5},
      {.name = "inverted_bounds", .min = 8.0, .max = 2.0},
      {.name = "unbounded_full",
       .min = geos::DoubleNegInfinity,
       .max = geos::DoubleInfinity},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const TestIntervalRTreeNode node(tc.min, tc.max);
    EXPECT_EQ(node.getMin(), tc.min);
    EXPECT_EQ(node.getMax(), tc.max);
  }
}

TEST(IntervalRTreeNodeTest,
     IntersectsHandlesOverlapTouchDisjointAndPointCases) {
  struct IntersectsCase {
    std::string_view name;
    double node_min;
    double node_max;
    double query_min;
    double query_max;
    bool expected;
  };
  constexpr auto kCases = std::to_array<IntersectsCase>({
      {.name = "disjoint_query_strictly_left",
       .node_min = 2.0,
       .node_max = 5.0,
       .query_min = -1.0,
       .query_max = 1.999,
       .expected = false},
      {.name = "disjoint_query_strictly_right",
       .node_min = 2.0,
       .node_max = 5.0,
       .query_min = 5.001,
       .query_max = 8.0,
       .expected = false},
      {.name = "touching_left_endpoint",
       .node_min = 2.0,
       .node_max = 5.0,
       .query_min = 0.0,
       .query_max = 2.0,
       .expected = true},
      {.name = "touching_right_endpoint",
       .node_min = 2.0,
       .node_max = 5.0,
       .query_min = 5.0,
       .query_max = 7.0,
       .expected = true},
      {.name = "partial_overlap_left",
       .node_min = 2.0,
       .node_max = 5.0,
       .query_min = 1.0,
       .query_max = 3.0,
       .expected = true},
      {.name = "partial_overlap_right",
       .node_min = 2.0,
       .node_max = 5.0,
       .query_min = 4.0,
       .query_max = 6.0,
       .expected = true},
      {.name = "query_contained_in_node",
       .node_min = 2.0,
       .node_max = 5.0,
       .query_min = 3.0,
       .query_max = 4.0,
       .expected = true},
      {.name = "node_contained_in_query",
       .node_min = 2.0,
       .node_max = 5.0,
       .query_min = 0.0,
       .query_max = 10.0,
       .expected = true},
      {.name = "point_query_inside_node",
       .node_min = 2.0,
       .node_max = 5.0,
       .query_min = 3.5,
       .query_max = 3.5,
       .expected = true},
      {.name = "point_query_outside_node",
       .node_min = 2.0,
       .node_max = 5.0,
       .query_min = 5.5,
       .query_max = 5.5,
       .expected = false},
      {.name = "point_node_exact_match",
       .node_min = 3.5,
       .node_max = 3.5,
       .query_min = 3.5,
       .query_max = 3.5,
       .expected = true},
      {.name = "point_node_disjoint_left",
       .node_min = 3.5,
       .node_max = 3.5,
       .query_min = 1.0,
       .query_max = 3.0,
       .expected = false},
      {.name = "point_node_disjoint_right",
       .node_min = 3.5,
       .node_max = 3.5,
       .query_min = 4.0,
       .query_max = 6.0,
       .expected = false},
      {.name = "negative_intervals_overlapping",
       .node_min = -10.0,
       .node_max = -5.0,
       .query_min = -6.0,
       .query_max = -1.0,
       .expected = true},
      {.name = "negative_intervals_disjoint",
       .node_min = -10.0,
       .node_max = -5.0,
       .query_min = -4.0,
       .query_max = -1.0,
       .expected = false},
      {.name = "signed_zero_intervals",
       .node_min = -0.0,
       .node_max = 0.0,
       .query_min = 0.0,
       .query_max = 0.0,
       .expected = true},
      {.name = "inverted_node_disjoint_by_min",
       .node_min = 5.0,
       .node_max = 2.0,
       .query_min = 3.0,
       .query_max = 4.0,
       .expected = false},
      {.name = "inverted_node_disjoint_by_max",
       .node_min = 5.0,
       .node_max = 2.0,
       .query_min = 6.0,
       .query_max = 7.0,
       .expected = false},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const TestIntervalRTreeNode node(tc.node_min, tc.node_max);
    EXPECT_EQ(node.Intersects(tc.query_min, tc.query_max), tc.expected);
  }
}

TEST(IntervalRTreeNodeTest, IntersectsHandlesDefaultEmptyAndNonFiniteBounds) {
  const TestIntervalRTreeNode default_node;
  EXPECT_FALSE(default_node.Intersects(0.0, 10.0));
  EXPECT_FALSE(default_node.Intersects(-10.0, -1.0));
  EXPECT_FALSE(default_node.Intersects(geos::DoubleNegInfinity, 0.0));
  EXPECT_FALSE(default_node.Intersects(0.0, geos::DoubleInfinity));
  // IEEE-754 comparisons (+inf > +inf and -inf < -inf) are both false.
  EXPECT_TRUE(
      default_node.Intersects(geos::DoubleNegInfinity, geos::DoubleInfinity));

  const TestIntervalRTreeNode finite_node(2.0, 5.0);
  EXPECT_TRUE(finite_node.Intersects(geos::DoubleNegInfinity, 2.0));
  EXPECT_TRUE(finite_node.Intersects(5.0, geos::DoubleInfinity));
  EXPECT_TRUE(finite_node.Intersects(geos::DoubleNotANumber, 6.0));
  EXPECT_FALSE(finite_node.Intersects(6.0, geos::DoubleNotANumber));
}

TEST(IntervalRTreeNodeTest, CompareOrdersIntervalsByDescendingMidpoint) {
  const TestIntervalRTreeNode low(1.0, 3.0);             // min + max = 4.0
  const TestIntervalRTreeNode high(4.0, 6.0);            // min + max = 10.0
  const TestIntervalRTreeNode same_mid_wide(0.0, 10.0);  // min + max = 10.0
  const TestIntervalRTreeNode neg_low(-10.0, -4.0);      // min + max = -14.0
  const TestIntervalRTreeNode neg_high(-2.0, -1.0);      // min + max = -3.0

  EXPECT_TRUE(IntervalRTreeNode::compare(&high, &low));
  EXPECT_FALSE(IntervalRTreeNode::compare(&low, &high));
  EXPECT_FALSE(IntervalRTreeNode::compare(&high, &high));

  // Equal midpoints yield false in both directions (strict weak ordering).
  EXPECT_FALSE(IntervalRTreeNode::compare(&high, &same_mid_wide));
  EXPECT_FALSE(IntervalRTreeNode::compare(&same_mid_wide, &high));

  EXPECT_TRUE(IntervalRTreeNode::compare(&neg_high, &neg_low));
  EXPECT_FALSE(IntervalRTreeNode::compare(&neg_low, &neg_high));

  IntervalRTreeNode::ConstVect nodes = {&neg_low, &low, &neg_high, &high};
  std::sort(nodes.begin(), nodes.end(), IntervalRTreeNode::compare);
  EXPECT_THAT(nodes, ElementsAre(&high, &low, &neg_high, &neg_low));
}

TEST(IntervalRTreeNodeTest, PolymorphicQueryAndVirtualDestruction) {
  int item1 = 101;
  int item2 = 202;
  bool test_node_destroyed = false;

  {
    std::unique_ptr<IntervalRTreeNode> poly_node =
        std::make_unique<TestIntervalRTreeNode>(1.0, 4.0, &item1,
                                                &test_node_destroyed);
    CollectingItemVisitor visitor;
    poly_node->query(3.0, 5.0, &visitor);
    poly_node->query(5.0, 6.0, &visitor);
    EXPECT_THAT(visitor.items(), ElementsAre(101));
    EXPECT_FALSE(test_node_destroyed);
  }
  EXPECT_TRUE(test_node_destroyed);

  std::unique_ptr<IntervalRTreeNode> default_poly =
      std::make_unique<TestIntervalRTreeNode>();
  EXPECT_EQ(default_poly->getMin(), geos::DoubleInfinity);
  EXPECT_EQ(default_poly->getMax(), geos::DoubleNegInfinity);
  default_poly.reset();

  auto leaf1 = std::make_unique<IntervalRTreeLeafNode>(1.0, 3.0, &item1);
  auto leaf2 = std::make_unique<IntervalRTreeLeafNode>(5.0, 8.0, &item2);
  std::unique_ptr<IntervalRTreeNode> branch =
      std::make_unique<IntervalRTreeBranchNode>(leaf1.get(), leaf2.get());
  EXPECT_DOUBLE_EQ(branch->getMin(), 1.0);
  EXPECT_DOUBLE_EQ(branch->getMax(), 8.0);

  CollectingItemVisitor hit_visitor;
  branch->query(2.0, 6.0, &hit_visitor);
  EXPECT_THAT(hit_visitor.items(), ElementsAre(101, 202));

  CollectingItemVisitor miss_visitor;
  branch->query(3.5, 4.5, &miss_visitor);
  EXPECT_THAT(miss_visitor.items(), IsEmpty());

  // Polymorphic destruction of real GEOS subclasses through IntervalRTreeNode*.
  std::unique_ptr<IntervalRTreeNode> base_leaf1 = std::move(leaf1);
  std::unique_ptr<IntervalRTreeNode> base_leaf2 = std::move(leaf2);
  branch.reset();
  base_leaf1.reset();
  base_leaf2.reset();
}

}  // namespace
}  // namespace geos_google
