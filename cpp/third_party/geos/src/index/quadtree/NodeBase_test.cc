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

// Unit tests for third_party/geos/src/index/quadtree/NodeBase.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/quadtree/NodeBase.cpp

#include "third_party/geos/include/geos/index/quadtree/NodeBase.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/quadtree/Node.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Envelope;
using geos::index::ItemVisitor;
using geos::index::quadtree::Node;
using geos::index::quadtree::NodeBase;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

// Minimal concrete NodeBase exposing protected subnode hooks and a
// configurable isSearchMatch predicate for isolated unit testing.
class TestNodeBase : public NodeBase {
 public:
  TestNodeBase() = default;
  explicit TestNodeBase(const Envelope& match_env)
      : match_all_(false), match_env_(match_env) {}

  void SetSubnode(std::size_t index, std::unique_ptr<Node> node) {
    delete subnodes.at(index);
    subnodes.at(index) = node.release();
  }

  Node* GetSubnode(std::size_t index) const { return subnodes.at(index); }

 protected:
  bool isSearchMatch(const Envelope& search_env) const override {
    if (match_all_) {
      return true;
    }
    return match_env_.intersects(search_env);
  }

 private:
  bool match_all_ = true;
  Envelope match_env_;
};

class RecordingVisitor : public ItemVisitor {
 public:
  void visitItem(void* item) override { visited_.push_back(item); }

  const std::vector<void*>& visited() const { return visited_; }

 private:
  std::vector<void*> visited_;
};

std::unique_ptr<Node> MakeNode(double min_x, double max_x, double min_y,
                               double max_y, int level) {
  return std::make_unique<Node>(
      std::make_unique<Envelope>(min_x, max_x, min_y, max_y), level);
}

struct SubnodeIndexCase {
  std::string_view name;
  double min_x;
  double max_x;
  double min_y;
  double max_y;
  double centre_x;
  double centre_y;
  int expected_index;
};

constexpr auto kSubnodeIndexCases = std::to_array<SubnodeIndexCase>({
    {
        .name = "quadrant_0_lower_left",
        .min_x = -4.0,
        .max_x = -1.0,
        .min_y = -4.0,
        .max_y = -1.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = 0,
    },
    {
        .name = "quadrant_1_lower_right",
        .min_x = 1.0,
        .max_x = 4.0,
        .min_y = -4.0,
        .max_y = -1.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = 1,
    },
    {
        .name = "quadrant_2_upper_left",
        .min_x = -4.0,
        .max_x = -1.0,
        .min_y = 1.0,
        .max_y = 4.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = 2,
    },
    {
        .name = "quadrant_3_upper_right",
        .min_x = 1.0,
        .max_x = 4.0,
        .min_y = 1.0,
        .max_y = 4.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = 3,
    },
    {
        .name = "boundary_touching_quadrant_0",
        .min_x = -2.0,
        .max_x = 0.0,
        .min_y = -2.0,
        .max_y = 0.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = 0,
    },
    {
        .name = "boundary_touching_quadrant_1",
        .min_x = 0.0,
        .max_x = 2.0,
        .min_y = -2.0,
        .max_y = 0.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = 1,
    },
    {
        .name = "boundary_touching_quadrant_2",
        .min_x = -2.0,
        .max_x = 0.0,
        .min_y = 0.0,
        .max_y = 2.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = 2,
    },
    {
        .name = "boundary_touching_quadrant_3",
        .min_x = 0.0,
        .max_x = 2.0,
        .min_y = 0.0,
        .max_y = 2.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = 3,
    },
    {
        .name = "straddles_centre_x_above_y",
        .min_x = -1.0,
        .max_x = 1.0,
        .min_y = 1.0,
        .max_y = 3.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = -1,
    },
    {
        .name = "straddles_centre_x_below_y",
        .min_x = -1.0,
        .max_x = 1.0,
        .min_y = -3.0,
        .max_y = -1.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = -1,
    },
    {
        .name = "straddles_centre_y_left_of_x",
        .min_x = -3.0,
        .max_x = -1.0,
        .min_y = -1.0,
        .max_y = 1.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = -1,
    },
    {
        .name = "straddles_centre_y_right_of_x",
        .min_x = 1.0,
        .max_x = 3.0,
        .min_y = -1.0,
        .max_y = 1.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = -1,
    },
    {
        .name = "straddles_both_axes",
        .min_x = -1.0,
        .max_x = 1.0,
        .min_y = -1.0,
        .max_y = 1.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = -1,
    },
    {
        .name = "degenerate_point_at_centre",
        .min_x = 0.0,
        .max_x = 0.0,
        .min_y = 0.0,
        .max_y = 0.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = 0,
    },
    {
        .name = "segment_along_positive_x_axis",
        .min_x = 1.0,
        .max_x = 2.0,
        .min_y = 0.0,
        .max_y = 0.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = 1,
    },
    {
        .name = "segment_along_positive_y_axis",
        .min_x = 0.0,
        .max_x = 0.0,
        .min_y = 1.0,
        .max_y = 2.0,
        .centre_x = 0.0,
        .centre_y = 0.0,
        .expected_index = 2,
    },
    {
        .name = "non_origin_centre_quadrant_3",
        .min_x = 12.0,
        .max_x = 15.0,
        .min_y = -3.0,
        .max_y = -1.0,
        .centre_x = 10.0,
        .centre_y = -5.0,
        .expected_index = 3,
    },
    {
        .name = "non_origin_centre_straddling",
        .min_x = 9.0,
        .max_x = 11.0,
        .min_y = -6.0,
        .max_y = -4.0,
        .centre_x = 10.0,
        .centre_y = -5.0,
        .expected_index = -1,
    },
});

TEST(NodeBaseTest, GetSubnodeIndexAllQuadrantsAndBoundaries) {
  for (const auto& tc : kSubnodeIndexCases) {
    SCOPED_TRACE(tc.name);
    const Envelope env(tc.min_x, tc.max_x, tc.min_y, tc.max_y);
    const Coordinate centre(tc.centre_x, tc.centre_y);
    EXPECT_EQ(NodeBase::getSubnodeIndex(&env, centre), tc.expected_index);
  }
}

TEST(NodeBaseTest, EmptyNodeStateAndInlinePredicates) {
  TestNodeBase node;
  EXPECT_FALSE(node.hasItems());
  EXPECT_FALSE(node.hasChildren());
  EXPECT_TRUE(node.isPrunable());
  EXPECT_TRUE(node.getItems().empty());
  EXPECT_EQ(node.depth(), 1u);
  EXPECT_EQ(node.size(), 0u);
  EXPECT_EQ(node.getNodeCount(), 1u);

  std::vector<void*> collected;
  std::vector<void*>& returned = node.addAllItems(collected);
  EXPECT_EQ(&returned, &collected);
  EXPECT_THAT(collected, IsEmpty());
}

TEST(NodeBaseTest, AddGetItemsAndPrunableWithItemsOrChildren) {
  TestNodeBase node;
  int item_a = 1;
  int item_b = 2;

  node.add(&item_a);
  EXPECT_TRUE(node.hasItems());
  EXPECT_FALSE(node.hasChildren());
  EXPECT_FALSE(node.isPrunable());
  EXPECT_EQ(node.size(), 1u);

  node.add(&item_b);
  EXPECT_THAT(node.getItems(), ElementsAre(&item_a, &item_b));
  EXPECT_EQ(node.size(), 2u);
  EXPECT_EQ(node.getNodeCount(), 1u);

  // Add a child subnode as well (both items and children -> not prunable).
  auto child = MakeNode(0.0, 4.0, 0.0, 4.0, 2);
  int item_c = 3;
  child->add(&item_c);
  node.SetSubnode(3, std::move(child));
  EXPECT_TRUE(node.hasChildren());
  EXPECT_FALSE(node.isPrunable());
  EXPECT_EQ(node.size(), 3u);
  // Leaf child contains 1 item and 0 grandchildren, so parent count is 2.
  EXPECT_EQ(node.getNodeCount(), 2u);

  // Children only (clear items) -> still not prunable.
  node.getItems().clear();
  EXPECT_FALSE(node.hasItems());
  EXPECT_TRUE(node.hasChildren());
  EXPECT_FALSE(node.isPrunable());
}

TEST(NodeBaseTest, DepthAcrossMultiLevelAndNonMonotonicSubnodes) {
  TestNodeBase root;

  // subnodes[0]: depth 2 (child -> grandchild)
  auto child0 = MakeNode(-8.0, 0.0, -8.0, 0.0, 3);
  child0->insertNode(MakeNode(-8.0, -4.0, -8.0, -4.0, 2));
  root.SetSubnode(0, std::move(child0));

  // subnodes[1]: depth 1 (shallower than subnodes[0] to hit sqd <= maxSubDepth)
  root.SetSubnode(1, MakeNode(0.0, 8.0, -8.0, 0.0, 3));

  // subnodes[2]: depth 3 (child -> grandchild -> great-grandchild)
  auto child2 = MakeNode(-8.0, 0.0, 0.0, 8.0, 3);
  auto grandchild2 = MakeNode(-8.0, -4.0, 0.0, 4.0, 2);
  grandchild2->insertNode(MakeNode(-8.0, -6.0, 0.0, 2.0, 1));
  child2->insertNode(std::move(grandchild2));
  root.SetSubnode(2, std::move(child2));

  // subnodes[3] remains nullptr.
  EXPECT_EQ(root.depth(), 4u);
}

TEST(NodeBaseTest, DestructorCleansUpPopulatedSubnodes) {
  auto root = std::make_unique<TestNodeBase>();
  int a = 1;
  int b = 2;
  int c = 3;
  int d = 4;
  auto n0 = MakeNode(-4.0, 0.0, -4.0, 0.0, 2);
  n0->add(&a);
  auto n1 = MakeNode(0.0, 4.0, -4.0, 0.0, 2);
  n1->add(&b);
  auto n2 = MakeNode(-4.0, 0.0, 0.0, 4.0, 2);
  n2->add(&c);
  auto n3 = MakeNode(0.0, 4.0, 0.0, 4.0, 2);
  n3->add(&d);

  root->SetSubnode(0, std::move(n0));
  root->SetSubnode(1, std::move(n1));
  root->SetSubnode(2, std::move(n2));
  root->SetSubnode(3, std::move(n3));

  EXPECT_EQ(root->size(), 4u);
  root.reset();
}

TEST(NodeBaseTest, AddAllItemsAppendsRecursivelyAndPreservesExisting) {
  TestNodeBase root;
  int pre_existing = 0;
  int root_item = 1;
  int child0_item = 2;
  int grandchild_item = 3;
  int child3_item = 4;

  root.add(&root_item);

  auto child0 = MakeNode(-8.0, 0.0, -8.0, 0.0, 3);
  child0->add(&child0_item);
  auto grandchild = MakeNode(-8.0, -4.0, -8.0, -4.0, 2);
  grandchild->add(&grandchild_item);
  child0->insertNode(std::move(grandchild));
  root.SetSubnode(0, std::move(child0));

  auto child3 = MakeNode(0.0, 8.0, 0.0, 8.0, 3);
  child3->add(&child3_item);
  root.SetSubnode(3, std::move(child3));

  std::vector<void*> out = {&pre_existing};
  std::vector<void*>& returned = root.addAllItems(out);
  EXPECT_EQ(&returned, &out);
  EXPECT_THAT(out, ElementsAre(&pre_existing, &root_item, &child0_item,
                               &grandchild_item, &child3_item));
}

TEST(NodeBaseTest, AddAllItemsFromOverlappingFiltersBySearchMatch) {
  TestNodeBase root(Envelope(-10.0, 10.0, -10.0, 10.0));
  int root_item = 1;
  int q0_item = 2;
  int q3_item = 3;
  root.add(&root_item);

  auto q0 = MakeNode(-8.0, 0.0, -8.0, 0.0, 3);
  q0->add(&q0_item);
  root.SetSubnode(0, std::move(q0));

  auto q3 = MakeNode(0.0, 8.0, 0.0, 8.0, 3);
  q3->add(&q3_item);
  root.SetSubnode(3, std::move(q3));

  // Search envelope completely disjoint from root's match envelope: skips all.
  std::vector<void*> disjoint_out;
  root.addAllItemsFromOverlapping(Envelope(20.0, 30.0, 20.0, 30.0),
                                  disjoint_out);
  EXPECT_THAT(disjoint_out, IsEmpty());

  // Search envelope hits root and q3, but misses q0.
  std::vector<void*> q3_out;
  root.addAllItemsFromOverlapping(Envelope(1.0, 2.0, 1.0, 2.0), q3_out);
  EXPECT_THAT(q3_out, UnorderedElementsAre(&root_item, &q3_item));

  // Search envelope hits root and both children.
  std::vector<void*> both_out;
  root.addAllItemsFromOverlapping(Envelope(-1.0, 1.0, -1.0, 1.0), both_out);
  EXPECT_THAT(both_out, UnorderedElementsAre(&root_item, &q0_item, &q3_item));
}

TEST(NodeBaseTest, VisitRespectsSearchMatchAndSkipsNonMatchingSubnodes) {
  TestNodeBase root(Envelope(-10.0, 10.0, -10.0, 10.0));
  int root_item = 1;
  int q0_item = 2;
  int q2_item = 3;
  root.add(&root_item);

  auto q0 = MakeNode(-8.0, 0.0, -8.0, 0.0, 3);
  q0->add(&q0_item);
  root.SetSubnode(0, std::move(q0));

  auto q2 = MakeNode(-8.0, 0.0, 0.0, 8.0, 3);
  q2->add(&q2_item);
  root.SetSubnode(2, std::move(q2));

  {
    RecordingVisitor visitor;
    const Envelope miss(50.0, 60.0, 50.0, 60.0);
    root.visit(&miss, visitor);
    EXPECT_THAT(visitor.visited(), IsEmpty());
  }

  {
    RecordingVisitor visitor;
    const Envelope hit_q2(-5.0, -1.0, 1.0, 5.0);
    root.visit(&hit_q2, visitor);
    EXPECT_THAT(visitor.visited(), UnorderedElementsAre(&root_item, &q2_item));
  }
}

TEST(NodeBaseTest, RemoveHandlesMismatchSubnodePruningAndLocalItems) {
  TestNodeBase root(Envelope(-10.0, 10.0, -10.0, 10.0));
  int root_item = 1;
  int q0_item = 2;
  int q2_item_a = 3;
  int q2_item_b = 4;
  int missing_item = 99;

  root.add(&root_item);
  root.add(&root_item);  // Duplicate entry in root to test single-copy erase.

  auto q0 = MakeNode(-8.0, 0.0, -8.0, 0.0, 3);
  auto q0_grandchild = MakeNode(-8.0, -4.0, -8.0, -4.0, 2);
  q0_grandchild->add(&q0_item);
  q0->insertNode(std::move(q0_grandchild));
  root.SetSubnode(0, std::move(q0));

  auto q2 = MakeNode(-8.0, 0.0, 0.0, 8.0, 3);
  q2->add(&q2_item_a);
  q2->add(&q2_item_b);
  root.SetSubnode(2, std::move(q2));

  // Outside root's match envelope: returns false even for existing items.
  const Envelope outside(50.0, 60.0, 50.0, 60.0);
  EXPECT_FALSE(root.remove(&outside, &root_item));
  EXPECT_FALSE(root.remove(&outside, &q0_item));
  EXPECT_EQ(root.size(), 5u);

  // Inside root's match envelope, item not present anywhere: returns false.
  const Envelope anywhere(-5.0, 5.0, -5.0, 5.0);
  EXPECT_FALSE(root.remove(&anywhere, &missing_item));
  EXPECT_EQ(root.size(), 5u);

  // Remove one of two items in q2: q2 is NOT prunable, remains allocated.
  const Envelope q2_env(-4.0, -2.0, 2.0, 4.0);
  EXPECT_TRUE(root.remove(&q2_env, &q2_item_a));
  EXPECT_NE(root.GetSubnode(2), nullptr);
  EXPECT_EQ(root.size(), 4u);

  // Remove the last item in q2: q2 becomes prunable and is deleted/nulled.
  EXPECT_TRUE(root.remove(&q2_env, &q2_item_b));
  EXPECT_EQ(root.GetSubnode(2), nullptr);
  EXPECT_EQ(root.size(), 3u);

  // Remove the item in q0's grandchild: prunes grandchild, then prunes q0 too.
  const Envelope q0_env(-7.0, -5.0, -7.0, -5.0);
  EXPECT_TRUE(root.remove(&q0_env, &q0_item));
  EXPECT_EQ(root.GetSubnode(0), nullptr);
  EXPECT_FALSE(root.hasChildren());
  EXPECT_EQ(root.size(), 2u);

  // Remove duplicate root items one at a time.
  EXPECT_TRUE(root.remove(&anywhere, &root_item));
  EXPECT_EQ(root.size(), 1u);
  EXPECT_TRUE(root.remove(&anywhere, &root_item));
  EXPECT_EQ(root.size(), 0u);
  EXPECT_TRUE(root.isPrunable());
  EXPECT_FALSE(root.remove(&anywhere, &root_item));
}

TEST(NodeBaseTest, RemoveStopsAfterFirstMatchingSubnode) {
  TestNodeBase root;
  int other_item = 5;
  int shared_item = 10;

  // Put another item in subnode[0] so the loop tests a matching subnode that
  // does not contain `shared_item`, then finds `shared_item` in subnode[1]
  // and stops before touching root's own item list.
  auto q0 = MakeNode(-4.0, 0.0, -4.0, 0.0, 2);
  q0->add(&other_item);
  root.SetSubnode(0, std::move(q0));

  auto q1 = MakeNode(0.0, 4.0, -4.0, 0.0, 2);
  q1->add(&shared_item);
  root.SetSubnode(1, std::move(q1));
  root.add(&shared_item);

  const Envelope wide_env(-2.0, 2.0, -2.0, -1.0);
  EXPECT_TRUE(root.remove(&wide_env, &shared_item));
  EXPECT_NE(root.GetSubnode(0), nullptr);
  EXPECT_EQ(root.GetSubnode(1), nullptr);
  EXPECT_THAT(root.getItems(), ElementsAre(&shared_item));

  // Subnode[0] matches `wide_env` but lacks `shared_item`, falling through to
  // root's own items.
  EXPECT_TRUE(root.remove(&wide_env, &shared_item));
  EXPECT_THAT(root.getItems(), IsEmpty());
  EXPECT_EQ(root.size(), 1u);
}

TEST(NodeBaseTest, ToStringFormatsItemsAndSubnodes) {
  TestNodeBase root;
  const std::string empty_str = root.toString();
  EXPECT_THAT(empty_str, HasSubstr("ITEMS:0"));
  EXPECT_THAT(empty_str, HasSubstr("subnode[0] NULL"));
  EXPECT_THAT(empty_str, HasSubstr("subnode[1] NULL"));
  EXPECT_THAT(empty_str, HasSubstr("subnode[2] NULL"));
  EXPECT_THAT(empty_str, HasSubstr("subnode[3] NULL"));

  int a = 1;
  int b = 2;
  int c = 3;
  root.add(&a);
  root.add(&b);
  auto child = MakeNode(0.0, 4.0, 0.0, 4.0, 2);
  child->add(&c);
  root.SetSubnode(3, std::move(child));

  const std::string populated_str = root.toString();
  EXPECT_THAT(populated_str, HasSubstr("ITEMS:2"));
  EXPECT_THAT(populated_str, HasSubstr("subnode[0] NULL"));
  EXPECT_THAT(populated_str, HasSubstr("subnode[3] L2"));
  EXPECT_THAT(populated_str, HasSubstr("ITEMS:1"));
}

}  // namespace
}  // namespace geos_google
