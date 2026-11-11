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

// Unit tests for third_party/geos/include/geos/index/strtree/AbstractNode.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/index/strtree/AbstractNode.h

#include "third_party/geos/include/geos/index/strtree/AbstractNode.h"

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/strtree/Boundable.h"
#include "third_party/geos/include/geos/index/strtree/Interval.h"
#include "third_party/geos/include/geos/index/strtree/ItemBoundable.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::index::strtree::AbstractNode;
using geos::index::strtree::Boundable;
using geos::index::strtree::Interval;
using geos::index::strtree::ItemBoundable;

static_assert(std::is_abstract_v<AbstractNode>);
static_assert(std::is_base_of_v<Boundable, AbstractNode>);
static_assert(std::has_virtual_destructor_v<AbstractNode>);
static_assert(
    std::is_same_v<decltype(std::declval<AbstractNode&>().getChildBoundables()),
                   std::vector<Boundable*>*>);
static_assert(std::is_same_v<decltype(std::declval<const AbstractNode&>()
                                          .getChildBoundables()),
                             const std::vector<Boundable*>*>);

class EnvelopeNode final : public AbstractNode {
 public:
  explicit EnvelopeNode(int level, int* destroyed_count = nullptr)
      : AbstractNode(level), destroyed_count_(destroyed_count) {}

  EnvelopeNode(int level, std::size_t capacity, int* destroyed_count = nullptr)
      : AbstractNode(level, capacity), destroyed_count_(destroyed_count) {}

  EnvelopeNode(const EnvelopeNode&) = delete;
  EnvelopeNode& operator=(const EnvelopeNode&) = delete;

  ~EnvelopeNode() override {
    delete static_cast<Envelope*>(bounds);
    if (destroyed_count_ != nullptr) {
      ++(*destroyed_count_);
    }
  }

  int computeCallCount() const { return compute_calls_; }

 protected:
  void* computeBounds() const override {
    ++compute_calls_;
    const std::vector<Boundable*>* children = getChildBoundables();
    std::unique_ptr<Envelope> combined;
    for (const Boundable* child : *children) {
      if (child == nullptr) {
        continue;
      }
      const auto* child_env = static_cast<const Envelope*>(child->getBounds());
      if (child_env == nullptr) {
        continue;
      }
      if (combined == nullptr) {
        combined = std::make_unique<Envelope>(*child_env);
      } else {
        combined->expandToInclude(child_env);
      }
    }
    return combined.release();
  }

 private:
  mutable int compute_calls_ = 0;
  int* destroyed_count_;
};

class IntervalNode final : public AbstractNode {
 public:
  explicit IntervalNode(int level) : AbstractNode(level) {}

  IntervalNode(const IntervalNode&) = delete;
  IntervalNode& operator=(const IntervalNode&) = delete;

  ~IntervalNode() override { delete static_cast<Interval*>(bounds); }

  int computeCallCount() const { return compute_calls_; }

 protected:
  void* computeBounds() const override {
    ++compute_calls_;
    const std::vector<Boundable*>* children = getChildBoundables();
    std::unique_ptr<Interval> combined;
    for (const Boundable* child : *children) {
      if (child == nullptr) {
        continue;
      }
      const auto* child_interval =
          static_cast<const Interval*>(child->getBounds());
      if (child_interval == nullptr) {
        continue;
      }
      if (combined == nullptr) {
        combined = std::make_unique<Interval>(*child_interval);
      } else {
        combined->expandToInclude(child_interval);
      }
    }
    return combined.release();
  }

 private:
  mutable int compute_calls_ = 0;
};

TEST(AbstractNodeTest, DefaultCapacityAndLevelInitialization) {
  EnvelopeNode node(0);
  const Boundable& as_boundable = node;

  EXPECT_EQ(node.getLevel(), 0);
  EXPECT_FALSE(node.isLeaf());
  EXPECT_FALSE(as_boundable.isLeaf());

  const std::vector<Boundable*>* children =
      std::as_const(node).getChildBoundables();
  ASSERT_NE(children, nullptr);
  EXPECT_TRUE(children->empty());
  EXPECT_GE(children->capacity(), 10u);
  EXPECT_EQ(node.computeCallCount(), 0);
}

TEST(AbstractNodeTest, ExplicitCapacitiesAndEdgeLevels) {
  struct NodeInitCase {
    int level;
    std::size_t capacity;
  };
  constexpr auto kCases = std::to_array<NodeInitCase>({
      {.level = 0, .capacity = 0},
      {.level = 1, .capacity = 1},
      {.level = 5, .capacity = 64},
      {.level = -1, .capacity = 4},
      {.level = std::numeric_limits<int>::min(), .capacity = 2},
      {.level = std::numeric_limits<int>::max(), .capacity = 8},
  });

  for (const NodeInitCase& tc : kCases) {
    SCOPED_TRACE(tc.level);
    EnvelopeNode node(tc.level, tc.capacity);
    EXPECT_EQ(node.getLevel(), tc.level);
    EXPECT_FALSE(node.isLeaf());
    ASSERT_NE(node.getChildBoundables(), nullptr);
    EXPECT_TRUE(node.getChildBoundables()->empty());
    EXPECT_GE(node.getChildBoundables()->capacity(), tc.capacity);
  }
}

TEST(AbstractNodeTest, ConstAndNonConstGetChildBoundablesAliasSameVector) {
  EnvelopeNode node(0, 2);
  const Envelope e1(0.0, 1.0, 0.0, 1.0);
  const Envelope e2(2.0, 3.0, 2.0, 3.0);
  const Envelope e3(4.0, 5.0, 4.0, 5.0);
  int i1 = 1;
  int i2 = 2;
  int i3 = 3;
  ItemBoundable b1(&e1, &i1);
  ItemBoundable b2(&e2, &i2);
  ItemBoundable b3(&e3, &i3);

  node.addChildBoundable(&b1);
  node.addChildBoundable(&b2);
  // Exceeds initial capacity of 2 to verify vector growth.
  node.addChildBoundable(&b3);

  std::vector<Boundable*>* mutable_children = node.getChildBoundables();
  const std::vector<Boundable*>* const_children =
      std::as_const(node).getChildBoundables();
  EXPECT_EQ(mutable_children, const_children);

  ASSERT_EQ(const_children->size(), 3u);
  EXPECT_EQ((*const_children)[0], &b1);
  EXPECT_EQ((*const_children)[1], &b2);
  EXPECT_EQ((*const_children)[2], &b3);

  mutable_children->pop_back();
  EXPECT_EQ(const_children->size(), 2u);
}

TEST(AbstractNodeTest, GetBoundsComputesLazilyAndCachesResult) {
  EnvelopeNode node(0);
  const Envelope e1(0.0, 2.0, 1.0, 3.0);
  const Envelope e2(-1.0, 1.0, 2.0, 5.0);
  int i1 = 1;
  int i2 = 2;
  ItemBoundable b1(&e1, &i1);
  ItemBoundable b2(&e2, &i2);

  node.addChildBoundable(&b1);
  node.addChildBoundable(&b2);
  EXPECT_EQ(node.computeCallCount(), 0);

  const void* first = std::as_const(node).getBounds();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(node.computeCallCount(), 1);

  const Envelope expected(-1.0, 2.0, 1.0, 5.0);
  EXPECT_TRUE(static_cast<const Envelope*>(first)->equals(&expected));

  const void* second = node.getBounds();
  const Boundable& as_boundable = node;
  const void* third = as_boundable.getBounds();

  EXPECT_EQ(second, first);
  EXPECT_EQ(third, first);
  EXPECT_EQ(node.computeCallCount(), 1);
}

TEST(AbstractNodeTest, NullComputedBoundsIsNotCachedAndAllowsAddingChildren) {
  EnvelopeNode node(0);

  EXPECT_EQ(node.getBounds(), nullptr);
  EXPECT_EQ(node.computeCallCount(), 1);

  EXPECT_EQ(node.getBounds(), nullptr);
  EXPECT_EQ(node.computeCallCount(), 2);

  // bounds remains nullptr, so addChildBoundable preconditions still hold.
  const Envelope env(10.0, 20.0, 30.0, 40.0);
  int item = 42;
  ItemBoundable leaf(&env, &item);
  node.addChildBoundable(&leaf);

  const void* computed = node.getBounds();
  ASSERT_NE(computed, nullptr);
  EXPECT_EQ(node.computeCallCount(), 3);
  EXPECT_TRUE(static_cast<const Envelope*>(computed)->equals(&env));

  EXPECT_EQ(node.getBounds(), computed);
  EXPECT_EQ(node.computeCallCount(), 3);
}

TEST(AbstractNodeTest, MultiLevelTreePropagatesAndCachesBounds) {
  const Envelope e1(0.0, 1.0, 0.0, 1.0);
  const Envelope e2(2.0, 4.0, 1.0, 3.0);
  const Envelope e3(-3.0, -1.0, 5.0, 6.0);
  const Envelope e4(1.0, 2.0, -2.0, 0.0);
  int items[] = {1, 2, 3, 4};

  ItemBoundable leaf1(&e1, &items[0]);
  ItemBoundable leaf2(&e2, &items[1]);
  ItemBoundable leaf3(&e3, &items[2]);
  ItemBoundable leaf4(&e4, &items[3]);

  EnvelopeNode left_child(0);
  left_child.addChildBoundable(&leaf1);
  left_child.addChildBoundable(&leaf2);

  EnvelopeNode right_child(0);
  right_child.addChildBoundable(&leaf3);
  right_child.addChildBoundable(&leaf4);

  EnvelopeNode mid_node(1);
  mid_node.addChildBoundable(&left_child);
  mid_node.addChildBoundable(&right_child);

  EnvelopeNode root(2);
  root.addChildBoundable(&mid_node);

  EXPECT_EQ(left_child.getLevel(), 0);
  EXPECT_EQ(right_child.getLevel(), 0);
  EXPECT_EQ(mid_node.getLevel(), 1);
  EXPECT_EQ(root.getLevel(), 2);

  EXPECT_FALSE(root.isLeaf());
  EXPECT_FALSE(mid_node.isLeaf());
  EXPECT_FALSE(left_child.isLeaf());
  EXPECT_FALSE(right_child.isLeaf());

  const auto* root_env = static_cast<const Envelope*>(root.getBounds());
  ASSERT_NE(root_env, nullptr);
  const Envelope expected_root(-3.0, 4.0, -2.0, 6.0);
  EXPECT_TRUE(root_env->equals(&expected_root));

  EXPECT_EQ(root.getBounds(), root_env);
  EXPECT_EQ(root.computeCallCount(), 1);
  EXPECT_EQ(mid_node.computeCallCount(), 1);
  EXPECT_EQ(left_child.computeCallCount(), 1);
  EXPECT_EQ(right_child.computeCallCount(), 1);
}

TEST(AbstractNodeTest, SupportsOneDimensionalIntervalBounds) {
  IntervalNode node(0);
  const Interval i1(1.0, 4.0);
  const Interval i2(-2.0, 2.5);
  int v1 = 10;
  int v2 = 20;
  ItemBoundable b1(&i1, &v1);
  ItemBoundable b2(&i2, &v2);

  node.addChildBoundable(&b1);
  node.addChildBoundable(&b2);

  const auto* combined = static_cast<const Interval*>(node.getBounds());
  ASSERT_NE(combined, nullptr);
  const Interval expected(-2.0, 4.0);
  EXPECT_TRUE(combined->equals(&expected));
  EXPECT_EQ(node.getBounds(), combined);
  EXPECT_EQ(node.computeCallCount(), 1);
}

TEST(AbstractNodeTest, StoresNullChildPointerWhenBoundsUncomputed) {
  EnvelopeNode node(0);
  const Envelope env(1.0, 2.0, 3.0, 4.0);
  int item = 5;
  ItemBoundable leaf(&env, &item);

  node.addChildBoundable(nullptr);
  node.addChildBoundable(&leaf);

  ASSERT_EQ(node.getChildBoundables()->size(), 2u);
  EXPECT_EQ((*node.getChildBoundables())[0], nullptr);
  EXPECT_EQ((*node.getChildBoundables())[1], &leaf);

  const auto* computed = static_cast<const Envelope*>(node.getBounds());
  ASSERT_NE(computed, nullptr);
  EXPECT_TRUE(computed->equals(&env));
}

TEST(AbstractNodeTest, VirtualDestructorViaAbstractNodeAndBoundablePointers) {
  int destroyed_count = 0;
  const Envelope env(0.0, 5.0, 0.0, 5.0);
  int item = 1;
  ItemBoundable leaf(&env, &item);

  std::unique_ptr<AbstractNode> as_abstract =
      std::make_unique<EnvelopeNode>(0, 4, &destroyed_count);
  as_abstract->addChildBoundable(&leaf);
  ASSERT_NE(as_abstract->getBounds(), nullptr);
  as_abstract.reset();
  EXPECT_EQ(destroyed_count, 1);

  std::unique_ptr<Boundable> as_boundable =
      std::make_unique<EnvelopeNode>(0, 4, &destroyed_count);
  static_cast<AbstractNode*>(as_boundable.get())->addChildBoundable(&leaf);
  ASSERT_NE(as_boundable->getBounds(), nullptr);
  as_boundable.reset();
  EXPECT_EQ(destroyed_count, 2);
}

}  // namespace
}  // namespace geos_google
