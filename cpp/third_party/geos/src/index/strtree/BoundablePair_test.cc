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

// Unit tests for third_party/geos/src/index/strtree/BoundablePair.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/strtree/BoundablePair.cpp

#include "third_party/geos/include/geos/index/strtree/BoundablePair.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/index/strtree/AbstractNode.h"
#include "third_party/geos/include/geos/index/strtree/Boundable.h"
#include "third_party/geos/include/geos/index/strtree/GeometryItemDistance.h"
#include "third_party/geos/include/geos/index/strtree/ItemBoundable.h"
#include "third_party/geos/include/geos/index/strtree/ItemDistance.h"
#include "third_party/geos/include/geos/index/strtree/STRtree.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::GeometryFactory;
using geos::geom::Point;
using geos::index::strtree::AbstractNode;
using geos::index::strtree::Boundable;
using geos::index::strtree::BoundablePair;
using geos::index::strtree::GeometryItemDistance;
using geos::index::strtree::ItemBoundable;
using geos::index::strtree::ItemDistance;
using geos::index::strtree::STRtree;
using geos::util::GEOSException;
using geos::util::IllegalArgumentException;

// Concrete AbstractNode subclass for constructing composite Boundables in
// tests.
class TestNode : public AbstractNode {
 public:
  explicit TestNode(int level, std::size_t capacity = 10)
      : AbstractNode(level, capacity) {}

  ~TestNode() override { delete static_cast<Envelope*>(bounds); }

 protected:
  void* computeBounds() const override {
    const std::vector<Boundable*>* children = getChildBoundables();
    if (children->empty()) {
      return nullptr;
    }
    const auto* first_env =
        static_cast<const Envelope*>((*children)[0]->getBounds());
    if (first_env == nullptr) {
      return nullptr;
    }
    auto merged = std::make_unique<Envelope>(*first_env);
    for (std::size_t i = 1; i < children->size(); ++i) {
      const auto* child_env =
          static_cast<const Envelope*>((*children)[i]->getBounds());
      if (child_env != nullptr) {
        merged->expandToInclude(child_env);
      }
    }
    return merged.release();
  }
};

// ItemDistance using CoordinateXY* items to distinguish item vs envelope
// distance.
class CoordinateItemDistance : public ItemDistance {
 public:
  double distance(const ItemBoundable* item1,
                  const ItemBoundable* item2) override {
    ++call_count;
    last_first = item1;
    last_second = item2;
    const auto* c1 = static_cast<const CoordinateXY*>(item1->getItem());
    const auto* c2 = static_cast<const CoordinateXY*>(item2->getItem());
    return c1->distance(*c2);
  }

  int call_count = 0;
  const ItemBoundable* last_first = nullptr;
  const ItemBoundable* last_second = nullptr;
};

std::vector<std::unique_ptr<BoundablePair>> DrainQueue(
    BoundablePair::BoundablePairQueue& queue) {
  std::vector<std::unique_ptr<BoundablePair>> items;
  items.reserve(queue.size());
  while (!queue.empty()) {
    items.emplace_back(queue.top());
    queue.pop();
  }
  return items;
}

void ClearQueue(BoundablePair::BoundablePairQueue& queue) {
  while (!queue.empty()) {
    std::unique_ptr<BoundablePair> owner(queue.top());
    queue.pop();
  }
}

TEST(BoundablePairTest, AccessorsAndLeafCompositeDetection) {
  CoordinateXY c1(0.0, 0.0);
  CoordinateXY c2(3.0, 4.0);
  Envelope env1(c1);
  Envelope env2(c2);
  ItemBoundable leaf1(&env1, &c1);
  ItemBoundable leaf2(&env2, &c2);

  TestNode node1(0);
  node1.addChildBoundable(&leaf1);
  TestNode node2(0);
  node2.addChildBoundable(&leaf2);

  EXPECT_FALSE(BoundablePair::isComposite(&leaf1));
  EXPECT_TRUE(BoundablePair::isComposite(&node1));

  CoordinateItemDistance dist;

  BoundablePair leaf_leaf(&leaf1, &leaf2, &dist);
  EXPECT_TRUE(leaf_leaf.isLeaves());
  EXPECT_EQ(leaf_leaf.getBoundable(0), &leaf1);
  EXPECT_EQ(leaf_leaf.getBoundable(1), &leaf2);
  EXPECT_EQ(leaf_leaf.getBoundable(2), &leaf2);
  EXPECT_EQ(leaf_leaf.getBoundable(-1), &leaf2);

  BoundablePair comp_leaf(&node1, &leaf2, &dist);
  EXPECT_FALSE(comp_leaf.isLeaves());
  EXPECT_EQ(comp_leaf.getBoundable(0), &node1);
  EXPECT_EQ(comp_leaf.getBoundable(1), &leaf2);

  BoundablePair leaf_comp(&leaf1, &node2, &dist);
  EXPECT_FALSE(leaf_comp.isLeaves());
  EXPECT_EQ(leaf_comp.getBoundable(0), &leaf1);
  EXPECT_EQ(leaf_comp.getBoundable(1), &node2);

  BoundablePair comp_comp(&node1, &node2, &dist);
  EXPECT_FALSE(comp_comp.isLeaves());
  EXPECT_EQ(comp_comp.getBoundable(0), &node1);
  EXPECT_EQ(comp_comp.getBoundable(1), &node2);
}

TEST(BoundablePairTest, AreaComputesBoundEnvelopeArea) {
  CoordinateXY pt(1.0, 2.0);
  Envelope pt_env(pt);
  Envelope line_env(0.0, 5.0, 2.0, 2.0);
  Envelope box_env(1.0, 4.0, 2.0, 6.0);
  Envelope other_env(0.0, 2.0, 0.0, 3.0);

  ItemBoundable pt_leaf(&pt_env, &pt);
  ItemBoundable line_leaf(&line_env, &pt);
  ItemBoundable box_leaf(&box_env, &pt);
  ItemBoundable other_leaf(&other_env, &pt);

  TestNode node(0);
  node.addChildBoundable(&box_leaf);
  node.addChildBoundable(&other_leaf);

  EXPECT_DOUBLE_EQ(BoundablePair::area(&pt_leaf), 0.0);
  EXPECT_DOUBLE_EQ(BoundablePair::area(&line_leaf), 0.0);
  EXPECT_DOUBLE_EQ(BoundablePair::area(&box_leaf), 12.0);
  // Merged envelope is [0, 4] x [0, 6], area = 24.
  EXPECT_DOUBLE_EQ(BoundablePair::area(&node), 24.0);
}

TEST(BoundablePairTest, DistanceDispatchesByLeafOrComposite) {
  // Coordinates are inside overlapping envelopes so item distance (5.0)
  // differs from envelope distance (0.0).
  CoordinateXY c1(1.0, 1.0);
  CoordinateXY c2(4.0, 5.0);
  Envelope env1(0.0, 10.0, 0.0, 10.0);
  Envelope env2(5.0, 15.0, 5.0, 15.0);
  ItemBoundable leaf1(&env1, &c1);
  ItemBoundable leaf2(&env2, &c2);

  CoordinateItemDistance dist;
  BoundablePair leaves_pair(&leaf1, &leaf2, &dist);
  EXPECT_DOUBLE_EQ(leaves_pair.getDistance(), 5.0);
  EXPECT_EQ(dist.call_count, 1);
  EXPECT_EQ(dist.last_first, &leaf1);
  EXPECT_EQ(dist.last_second, &leaf2);
  EXPECT_DOUBLE_EQ(leaves_pair.distance(), 5.0);
  EXPECT_EQ(dist.call_count, 2);

  TestNode node1(0);
  node1.addChildBoundable(&leaf1);
  BoundablePair overlapping_comp_pair(&node1, &leaf2, &dist);
  EXPECT_DOUBLE_EQ(overlapping_comp_pair.getDistance(), 0.0);
  EXPECT_DOUBLE_EQ(overlapping_comp_pair.distance(), 0.0);
  EXPECT_EQ(dist.call_count, 2);

  CoordinateXY c3(14.0, 15.0);
  Envelope env3(13.0, 15.0, 14.0, 16.0);
  ItemBoundable leaf3(&env3, &c3);
  TestNode node3(0);
  node3.addChildBoundable(&leaf3);
  BoundablePair disjoint_comp_pair(&node1, &node3, &dist);
  EXPECT_DOUBLE_EQ(disjoint_comp_pair.getDistance(), 5.0);
  EXPECT_DOUBLE_EQ(disjoint_comp_pair.distance(), 5.0);
  EXPECT_EQ(dist.call_count, 2);
}

TEST(BoundablePairTest, ThrowsWhenCompositePairHasNullBounds) {
  CoordinateXY c(1.0, 1.0);
  Envelope valid_env(0.0, 1.0, 0.0, 1.0);
  ItemBoundable valid_leaf(&valid_env, &c);
  ItemBoundable null_bounds_leaf(nullptr, &c);

  TestNode valid_node(0);
  valid_node.addChildBoundable(&valid_leaf);
  TestNode empty_node(0);

  CoordinateItemDistance dist;
  EXPECT_THROW(BoundablePair(&empty_node, &valid_node, &dist), GEOSException);
  EXPECT_THROW(BoundablePair(&valid_node, &empty_node, &dist), GEOSException);
  EXPECT_THROW(BoundablePair(&null_bounds_leaf, &valid_node, &dist),
               GEOSException);
  EXPECT_THROW(BoundablePair(&valid_node, &null_bounds_leaf, &dist),
               GEOSException);
  EXPECT_THROW(BoundablePair(&empty_node, &empty_node, &dist), GEOSException);
}

TEST(BoundablePairTest, ComputesMaximumDistanceAcrossBoundables) {
  CoordinateXY c1(0.0, 0.0);
  CoordinateXY c2(3.0, 4.0);
  Envelope env1(0.0, 1.0, 0.0, 1.0);
  Envelope env2(3.0, 4.0, 4.0, 5.0);
  ItemBoundable leaf1(&env1, &c1);
  ItemBoundable leaf2(&env2, &c2);

  CoordinateItemDistance dist;
  BoundablePair leaf_pair(&leaf1, &leaf2, &dist);
  EXPECT_DOUBLE_EQ(leaf_pair.maximumDistance(), std::hypot(4.0, 5.0));

  Envelope env3(0.0, 2.0, 0.0, 3.0);
  Envelope env4(1.0, 3.0, 2.0, 4.0);
  ItemBoundable leaf3(&env3, &c1);
  ItemBoundable leaf4(&env4, &c2);
  TestNode node_a(0);
  node_a.addChildBoundable(&leaf3);
  BoundablePair comp_leaf_pair(&node_a, &leaf4, &dist);
  EXPECT_DOUBLE_EQ(comp_leaf_pair.maximumDistance(), 5.0);

  Envelope env5(5.0, 8.0, 6.0, 12.0);
  ItemBoundable leaf5(&env5, &c2);
  TestNode node_b(0);
  node_b.addChildBoundable(&leaf5);
  BoundablePair comp_comp_pair(&node_a, &node_b, &dist);
  EXPECT_DOUBLE_EQ(comp_comp_pair.maximumDistance(), std::hypot(8.0, 12.0));
}

TEST(BoundablePairTest, PriorityQueueOrdersByMinimumDistance) {
  CoordinateXY origin(0.0, 0.0);
  CoordinateXY c2(2.0, 0.0);
  CoordinateXY c5(5.0, 0.0);
  CoordinateXY c10(10.0, 0.0);
  Envelope env0(origin);
  Envelope env2(c2);
  Envelope env5(c5);
  Envelope env10(c10);

  ItemBoundable b0(&env0, &origin);
  ItemBoundable b2(&env2, &c2);
  ItemBoundable b5(&env5, &c5);
  ItemBoundable b10(&env10, &c10);

  CoordinateItemDistance dist;
  BoundablePair pair10(&b0, &b10, &dist);
  BoundablePair pair2(&b0, &b2, &dist);
  BoundablePair pair5(&b0, &b5, &dist);

  BoundablePair::BoundablePairQueueCompare compare;
  EXPECT_TRUE(compare(&pair10, &pair2));
  EXPECT_FALSE(compare(&pair2, &pair10));
  EXPECT_FALSE(compare(&pair5, &pair5));

  BoundablePair::BoundablePairQueue queue;
  queue.push(&pair10);
  queue.push(&pair2);
  queue.push(&pair5);

  ASSERT_EQ(queue.size(), 3u);
  EXPECT_EQ(queue.top(), &pair2);
  queue.pop();
  EXPECT_EQ(queue.top(), &pair5);
  queue.pop();
  EXPECT_EQ(queue.top(), &pair10);
  queue.pop();
}

TEST(BoundablePairTest, ExpandToQueueThrowsWhenNeitherBoundableIsComposite) {
  CoordinateXY c1(0.0, 0.0);
  CoordinateXY c2(1.0, 0.0);
  Envelope env1(c1);
  Envelope env2(c2);
  ItemBoundable leaf1(&env1, &c1);
  ItemBoundable leaf2(&env2, &c2);

  CoordinateItemDistance dist;
  BoundablePair pair(&leaf1, &leaf2, &dist);
  BoundablePair::BoundablePairQueue queue;

  EXPECT_THROW(pair.expandToQueue(queue, geos::DoubleInfinity),
               IllegalArgumentException);
  EXPECT_TRUE(queue.empty());
}

TEST(BoundablePairTest, ExpandToQueueExpandsFirstCompositeWhenSecondIsLeaf) {
  CoordinateXY c_near(0.0, 0.0);
  CoordinateXY c_far(3.0, 4.0);
  CoordinateXY c_target(0.0, 0.0);
  Envelope env_near(c_near);
  Envelope env_far(c_far);
  Envelope env_target(c_target);

  ItemBoundable child_near(&env_near, &c_near);
  ItemBoundable child_far(&env_far, &c_far);
  ItemBoundable target(&env_target, &c_target);

  TestNode composite(0);
  composite.addChildBoundable(&child_far);
  composite.addChildBoundable(&child_near);

  CoordinateItemDistance dist;
  BoundablePair pair(&composite, &target, &dist);

  BoundablePair::BoundablePairQueue queue;
  absl::Cleanup drain_on_exit = [&queue] { ClearQueue(queue); };
  pair.expandToQueue(queue, geos::DoubleInfinity);
  std::vector<std::unique_ptr<BoundablePair>> expanded = DrainQueue(queue);

  ASSERT_EQ(expanded.size(), 2u);
  EXPECT_EQ(expanded[0]->getBoundable(0), &child_near);
  EXPECT_EQ(expanded[0]->getBoundable(1), &target);
  EXPECT_DOUBLE_EQ(expanded[0]->getDistance(), 0.0);

  EXPECT_EQ(expanded[1]->getBoundable(0), &child_far);
  EXPECT_EQ(expanded[1]->getBoundable(1), &target);
  EXPECT_DOUBLE_EQ(expanded[1]->getDistance(), 5.0);
}

TEST(BoundablePairTest, ExpandToQueueExpandsSecondCompositeWhenFirstIsLeaf) {
  CoordinateXY c_target(0.0, 0.0);
  CoordinateXY c_mid(3.0, 4.0);
  CoordinateXY c_far(6.0, 8.0);
  Envelope env_target(c_target);
  Envelope env_mid(c_mid);
  Envelope env_far(c_far);

  ItemBoundable target(&env_target, &c_target);
  ItemBoundable child_mid(&env_mid, &c_mid);
  ItemBoundable child_far(&env_far, &c_far);

  TestNode composite(0);
  composite.addChildBoundable(&child_far);
  composite.addChildBoundable(&child_mid);

  CoordinateItemDistance dist;
  BoundablePair pair(&target, &composite, &dist);

  BoundablePair::BoundablePairQueue queue;
  absl::Cleanup drain_on_exit = [&queue] { ClearQueue(queue); };
  pair.expandToQueue(queue, geos::DoubleInfinity);
  std::vector<std::unique_ptr<BoundablePair>> expanded = DrainQueue(queue);

  ASSERT_EQ(expanded.size(), 2u);
  EXPECT_EQ(expanded[0]->getBoundable(0), &target);
  EXPECT_EQ(expanded[0]->getBoundable(1), &child_mid);
  EXPECT_DOUBLE_EQ(expanded[0]->getDistance(), 5.0);

  EXPECT_EQ(expanded[1]->getBoundable(0), &target);
  EXPECT_EQ(expanded[1]->getBoundable(1), &child_far);
  EXPECT_DOUBLE_EQ(expanded[1]->getDistance(), 10.0);
}

TEST(BoundablePairTest, ExpandToQueueSelectsLargerAreaWhenBothComposite) {
  CoordinateXY dummy(0.0, 0.0);
  // Area 100 vs area 4.
  Envelope env_large_a(0.0, 5.0, 0.0, 10.0);
  Envelope env_large_b(5.0, 10.0, 0.0, 10.0);
  Envelope env_small_a(20.0, 21.0, 0.0, 2.0);
  Envelope env_small_b(21.0, 22.0, 0.0, 2.0);
  Envelope env_small_c(20.0, 22.0, 1.0, 2.0);

  ItemBoundable l_a(&env_large_a, &dummy);
  ItemBoundable l_b(&env_large_b, &dummy);
  ItemBoundable s_a(&env_small_a, &dummy);
  ItemBoundable s_b(&env_small_b, &dummy);
  ItemBoundable s_c(&env_small_c, &dummy);

  TestNode large_node(0);
  large_node.addChildBoundable(&l_a);
  large_node.addChildBoundable(&l_b);

  TestNode small_node(0);
  small_node.addChildBoundable(&s_a);
  small_node.addChildBoundable(&s_b);
  small_node.addChildBoundable(&s_c);

  ASSERT_DOUBLE_EQ(BoundablePair::area(&large_node), 100.0);
  ASSERT_DOUBLE_EQ(BoundablePair::area(&small_node), 4.0);

  CoordinateItemDistance dist;

  // Case 1: first has strictly larger area -> expand first (isFlipped = false).
  {
    BoundablePair pair(&large_node, &small_node, &dist);
    BoundablePair::BoundablePairQueue queue;
    absl::Cleanup drain_on_exit = [&queue] { ClearQueue(queue); };
    pair.expandToQueue(queue, geos::DoubleInfinity);
    std::vector<std::unique_ptr<BoundablePair>> expanded = DrainQueue(queue);

    ASSERT_EQ(expanded.size(), 2u);
    std::unordered_set<const Boundable*> seen_children;
    for (const auto& bp : expanded) {
      seen_children.insert(bp->getBoundable(0));
      EXPECT_EQ(bp->getBoundable(1), &small_node);
    }
    EXPECT_EQ(seen_children.size(), 2u);
    EXPECT_EQ(seen_children.count(&l_a), 1u);
    EXPECT_EQ(seen_children.count(&l_b), 1u);
  }

  // Case 2: second has strictly larger area -> expand second (isFlipped =
  // true).
  {
    BoundablePair pair(&small_node, &large_node, &dist);
    BoundablePair::BoundablePairQueue queue;
    absl::Cleanup drain_on_exit = [&queue] { ClearQueue(queue); };
    pair.expandToQueue(queue, geos::DoubleInfinity);
    std::vector<std::unique_ptr<BoundablePair>> expanded = DrainQueue(queue);

    ASSERT_EQ(expanded.size(), 2u);
    std::unordered_set<const Boundable*> seen_children;
    for (const auto& bp : expanded) {
      EXPECT_EQ(bp->getBoundable(0), &small_node);
      seen_children.insert(bp->getBoundable(1));
    }
    EXPECT_EQ(seen_children.size(), 2u);
    EXPECT_EQ(seen_children.count(&l_a), 1u);
    EXPECT_EQ(seen_children.count(&l_b), 1u);
  }

  // Case 3: equal area -> else branch expands second (isFlipped = true).
  Envelope env_eq1_a(0.0, 2.0, 0.0, 4.0);
  Envelope env_eq1_b(2.0, 4.0, 0.0, 4.0);
  Envelope env_eq2_a(10.0, 12.0, 0.0, 4.0);
  Envelope env_eq2_b(12.0, 14.0, 0.0, 2.0);
  Envelope env_eq2_c(12.0, 14.0, 2.0, 4.0);
  ItemBoundable eq1_a(&env_eq1_a, &dummy);
  ItemBoundable eq1_b(&env_eq1_b, &dummy);
  ItemBoundable eq2_a(&env_eq2_a, &dummy);
  ItemBoundable eq2_b(&env_eq2_b, &dummy);
  ItemBoundable eq2_c(&env_eq2_c, &dummy);

  TestNode eq_node1(0);
  eq_node1.addChildBoundable(&eq1_a);
  eq_node1.addChildBoundable(&eq1_b);
  TestNode eq_node2(0);
  eq_node2.addChildBoundable(&eq2_a);
  eq_node2.addChildBoundable(&eq2_b);
  eq_node2.addChildBoundable(&eq2_c);

  ASSERT_DOUBLE_EQ(BoundablePair::area(&eq_node1), 16.0);
  ASSERT_DOUBLE_EQ(BoundablePair::area(&eq_node2), 16.0);

  {
    BoundablePair pair(&eq_node1, &eq_node2, &dist);
    BoundablePair::BoundablePairQueue queue;
    absl::Cleanup drain_on_exit = [&queue] { ClearQueue(queue); };
    pair.expandToQueue(queue, geos::DoubleInfinity);
    std::vector<std::unique_ptr<BoundablePair>> expanded = DrainQueue(queue);

    ASSERT_EQ(expanded.size(), 3u);
    std::unordered_set<const Boundable*> seen_children;
    for (const auto& bp : expanded) {
      EXPECT_EQ(bp->getBoundable(0), &eq_node1);
      seen_children.insert(bp->getBoundable(1));
    }
    EXPECT_EQ(seen_children.size(), 3u);
    EXPECT_EQ(seen_children.count(&eq2_a), 1u);
    EXPECT_EQ(seen_children.count(&eq2_b), 1u);
    EXPECT_EQ(seen_children.count(&eq2_c), 1u);
  }
}

TEST(BoundablePairTest, ExpandPrunesPairsAtOrAboveMinDistance) {
  CoordinateXY c_target(0.0, 0.0);
  CoordinateXY c1(1.0, 0.0);
  CoordinateXY c5(5.0, 0.0);
  CoordinateXY c8(8.0, 0.0);
  Envelope env_target(c_target);
  Envelope env1(c1);
  Envelope env5(c5);
  Envelope env8(c8);

  ItemBoundable target(&env_target, &c_target);
  ItemBoundable child1(&env1, &c1);
  ItemBoundable child5(&env5, &c5);
  ItemBoundable child8(&env8, &c8);

  TestNode node(0);
  node.addChildBoundable(&child1);
  node.addChildBoundable(&child5);
  node.addChildBoundable(&child8);

  CoordinateItemDistance dist;
  BoundablePair pair(&node, &target, &dist);

  // Strict '< minDistance' excludes distance == 5.0 and 8.0.
  {
    BoundablePair::BoundablePairQueue queue;
    absl::Cleanup drain_on_exit = [&queue] { ClearQueue(queue); };
    pair.expandToQueue(queue, 5.0);
    std::vector<std::unique_ptr<BoundablePair>> expanded = DrainQueue(queue);

    ASSERT_EQ(expanded.size(), 1u);
    EXPECT_EQ(expanded[0]->getBoundable(0), &child1);
    EXPECT_EQ(expanded[0]->getBoundable(1), &target);
    EXPECT_DOUBLE_EQ(expanded[0]->getDistance(), 1.0);
  }

  // Direct expand() call with flipped=true and threshold 5.1 includes 1.0
  // and 5.0.
  {
    BoundablePair::BoundablePairQueue queue;
    absl::Cleanup drain_on_exit = [&queue] { ClearQueue(queue); };
    pair.expand(&node, &target, true, queue, 5.1);
    std::vector<std::unique_ptr<BoundablePair>> expanded = DrainQueue(queue);

    ASSERT_EQ(expanded.size(), 2u);
    EXPECT_EQ(expanded[0]->getBoundable(0), &target);
    EXPECT_EQ(expanded[0]->getBoundable(1), &child1);
    EXPECT_DOUBLE_EQ(expanded[0]->getDistance(), 1.0);

    EXPECT_EQ(expanded[1]->getBoundable(0), &target);
    EXPECT_EQ(expanded[1]->getBoundable(1), &child5);
    EXPECT_DOUBLE_EQ(expanded[1]->getDistance(), 5.0);
  }

  // Zero threshold prunes all non-negative distances.
  {
    BoundablePair::BoundablePairQueue queue;
    absl::Cleanup drain_on_exit = [&queue] { ClearQueue(queue); };
    pair.expandToQueue(queue, 0.0);
    EXPECT_TRUE(queue.empty());
  }

  // Empty composite node yields no child pairs.
  {
    TestNode empty_node(0);
    BoundablePair::BoundablePairQueue queue;
    absl::Cleanup drain_on_exit = [&queue] { ClearQueue(queue); };
    pair.expand(&empty_node, &target, false, queue, geos::DoubleInfinity);
    EXPECT_TRUE(queue.empty());
  }
}

TEST(BoundablePairTest, MultiLevelStrTreeNearestNeighbourIntegration) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::vector<std::unique_ptr<Point>> pts1;
  pts1.push_back(factory->createPoint(CoordinateXY(0.0, 0.0)));
  pts1.push_back(factory->createPoint(CoordinateXY(10.0, 0.0)));
  pts1.push_back(factory->createPoint(CoordinateXY(20.0, 0.0)));
  pts1.push_back(factory->createPoint(CoordinateXY(30.0, 0.0)));
  pts1.push_back(factory->createPoint(CoordinateXY(40.0, 0.0)));
  pts1.push_back(factory->createPoint(CoordinateXY(50.0, 0.0)));

  std::vector<std::unique_ptr<Point>> pts2;
  pts2.push_back(factory->createPoint(CoordinateXY(100.0, 100.0)));
  pts2.push_back(factory->createPoint(CoordinateXY(31.0, 1.0)));
  pts2.push_back(factory->createPoint(CoordinateXY(-50.0, -50.0)));
  pts2.push_back(factory->createPoint(CoordinateXY(5.0, 10.0)));

  STRtree tree1(2);
  for (const auto& pt : pts1) {
    tree1.insert(pt->getEnvelopeInternal(), pt.get());
  }
  tree1.build();

  STRtree tree2(2);
  for (const auto& pt : pts2) {
    tree2.insert(pt->getEnvelopeInternal(), pt.get());
  }
  tree2.build();

  GeometryItemDistance metric;
  const std::pair<const void*, const void*> nearest =
      tree1.nearestNeighbour(&tree2, &metric);
  EXPECT_EQ(nearest.first, pts1[3].get());
  EXPECT_EQ(nearest.second, pts2[1].get());
}

}  // namespace
}  // namespace geos_google
