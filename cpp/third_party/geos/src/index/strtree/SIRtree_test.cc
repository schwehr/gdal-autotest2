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

// Unit tests for third_party/geos/src/index/strtree/SIRtree.cpp.
//
// Notes:
// - SIRtree::insert/query store and return raw void* item pointers;
// caller-owned
//   items must outlive the tree.
// - SIRtree::query(double) and query(double, double) return caller-owned
//   std::vector<void*>* pointers wrapped in std::unique_ptr here.
// - Upstream SIRtree::createParentBoundables leaves a trailing empty
//   SIRAbstractNode whenever a level's boundable count n >= C + 1 is a multiple
//   of (nodeCapacity + 1); test sizes avoid that upstream defect.

#include "third_party/geos/include/geos/index/strtree/SIRtree.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/strtree/AbstractNode.h"
#include "third_party/geos/include/geos/index/strtree/AbstractSTRtree.h"
#include "third_party/geos/include/geos/index/strtree/Boundable.h"
#include "third_party/geos/include/geos/index/strtree/Interval.h"
#include "third_party/geos/include/geos/index/strtree/ItemBoundable.h"

namespace geos_google {
namespace {

using geos::index::ItemVisitor;
using geos::index::strtree::AbstractNode;
using geos::index::strtree::AbstractSTRtree;
using geos::index::strtree::Boundable;
using geos::index::strtree::BoundableList;
using geos::index::strtree::Interval;
using geos::index::strtree::ItemBoundable;
using geos::index::strtree::ItemsList;
using geos::index::strtree::ItemsListItem;
using geos::index::strtree::SIRtree;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAreArray;

struct IntervalSpec {
  double x1;
  double x2;
  int id;
};

struct TaggedInterval {
  double x1;
  double x2;
  int id;
};

class CollectingVisitor final : public ItemVisitor {
 public:
  void visitItem(void* item) override { items_.push_back(item); }

  const std::vector<void*>& items() const { return items_; }

 private:
  std::vector<void*> items_;
};

// Exposes protected AbstractSTRtree/SIRtree members to verify node hierarchy,
// boundable sorting, parent grouping, and intersection predicates directly.
class ExposedSIRtree final : public SIRtree {
 public:
  using SIRtree::createParentBoundables;
  using SIRtree::getIntersectsOp;
  using SIRtree::getRoot;
  using SIRtree::SIRtree;
  using SIRtree::sortBoundables;

  void QueryWithVisitor(const void* search_bounds, ItemVisitor& visitor) {
    AbstractSTRtree::query(search_bounds, visitor);
  }

  void QueryWithVector(const void* search_bounds,
                       std::vector<void*>& found_items) {
    AbstractSTRtree::query(search_bounds, found_items);
  }

  bool RemoveItem(const void* item_bounds, void* item) {
    return AbstractSTRtree::remove(item_bounds, item);
  }
};

void CollectItemsList(const ItemsList& list, std::vector<void*>& out) {
  for (const ItemsListItem& entry : list) {
    if (entry.get_type() == ItemsListItem::item_is_geometry) {
      out.push_back(entry.get_geometry());
    } else {
      CollectItemsList(*entry.get_itemslist(), out);
    }
  }
}

std::vector<int> ExtractIds(const std::vector<void*>& ptrs) {
  std::vector<int> ids;
  ids.reserve(ptrs.size());
  for (void* ptr : ptrs) {
    ids.push_back(static_cast<const TaggedInterval*>(ptr)->id);
  }
  return ids;
}

std::vector<int> BruteForceQueryIds(const std::vector<TaggedInterval>& items,
                                    double q1, double q2) {
  const double qmin = std::min(q1, q2);
  const double qmax = std::max(q1, q2);
  std::vector<int> ids;
  for (const TaggedInterval& item : items) {
    const double imin = std::min(item.x1, item.x2);
    const double imax = std::max(item.x1, item.x2);
    if (!(qmin > imax || qmax < imin)) {
      ids.push_back(item.id);
    }
  }
  return ids;
}

TEST(SIRtreeTest, DefaultAndCustomNodeCapacity) {
  SIRtree default_tree;
  EXPECT_EQ(default_tree.getNodeCapacity(), 10U);

  SIRtree cap2_tree(2);
  EXPECT_EQ(cap2_tree.getNodeCapacity(), 2U);

  SIRtree cap5_tree(5);
  EXPECT_EQ(cap5_tree.getNodeCapacity(), 5U);
}

TEST(SIRtreeTest, EmptyTreeQueryBuildIterateAndItemsTree) {
  SIRtree tree(4);

  {
    std::unique_ptr<std::vector<void*>> point_hits(tree.query(0.0));
    ASSERT_NE(point_hits, nullptr);
    EXPECT_THAT(*point_hits, IsEmpty());
  }

  // Calling build() after an implicit build from query() is a safe no-op.
  tree.build();

  {
    std::unique_ptr<std::vector<void*>> range_hits(tree.query(-10.0, 10.0));
    ASSERT_NE(range_hits, nullptr);
    EXPECT_THAT(*range_hits, IsEmpty());
  }

  CollectingVisitor iter_visitor;
  tree.iterate(iter_visitor);
  EXPECT_THAT(iter_visitor.items(), IsEmpty());

  std::unique_ptr<ItemsList> items_tree(tree.itemsTree());
  ASSERT_NE(items_tree, nullptr);
  EXPECT_THAT(*items_tree, IsEmpty());
}

TEST(SIRtreeTest, InsertNormalizesReversedAndPointIntervals) {
  SIRtree tree(4);
  std::vector<TaggedInterval> items = {
      {.x1 = 10.0, .x2 = 4.0, .id = 1},
      {.x1 = -3.5, .x2 = -3.5, .id = 2},
      {.x1 = 0.0, .x2 = 2.0, .id = 3},
  };
  for (TaggedInterval& item : items) {
    tree.insert(item.x1, item.x2, &item);
  }

  // Point queries on exact boundaries, interior, and gaps.
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(-3.5));
    EXPECT_THAT(ExtractIds(*hits), UnorderedElementsAreArray({2}));
  }
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(-3.51));
    EXPECT_THAT(*hits, IsEmpty());
  }
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(-3.49));
    EXPECT_THAT(*hits, IsEmpty());
  }
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(4.0));
    EXPECT_THAT(ExtractIds(*hits), UnorderedElementsAreArray({1}));
  }
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(10.0));
    EXPECT_THAT(ExtractIds(*hits), UnorderedElementsAreArray({1}));
  }
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(7.0));
    EXPECT_THAT(ExtractIds(*hits), UnorderedElementsAreArray({1}));
  }
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(2.5));
    EXPECT_THAT(*hits, IsEmpty());
  }

  // Range queries support reversed bounds and boundary touches.
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(4.0, 0.0));
    EXPECT_THAT(ExtractIds(*hits), UnorderedElementsAreArray({1, 3}));
  }
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(-4.0, 11.0));
    EXPECT_THAT(ExtractIds(*hits), UnorderedElementsAreArray({1, 2, 3}));
  }
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(2.1, 3.9));
    EXPECT_THAT(*hits, IsEmpty());
  }
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(20.0, 30.0));
    EXPECT_THAT(*hits, IsEmpty());
  }
}

TEST(SIRtreeTest, IterateBeforeBuildAllowsAdditionalInserts) {
  SIRtree tree(3);
  TaggedInterval first{.x1 = 1.0, .x2 = 3.0, .id = 10};
  TaggedInterval second{.x1 = 5.0, .x2 = 8.0, .id = 20};
  tree.insert(first.x1, first.x2, &first);

  CollectingVisitor iter_visitor;
  tree.iterate(iter_visitor);
  EXPECT_THAT(ExtractIds(iter_visitor.items()),
              UnorderedElementsAreArray({10}));

  tree.insert(second.x1, second.x2, &second);

  std::unique_ptr<std::vector<void*>> hits(tree.query(0.0, 10.0));
  EXPECT_THAT(ExtractIds(*hits), UnorderedElementsAreArray({10, 20}));
}

TEST(SIRtreeTest, SingleItemTree) {
  ExposedSIRtree tree(2);
  TaggedInterval only{.x1 = 3.0, .x2 = -1.0, .id = 42};
  tree.insert(only.x1, only.x2, &only);
  tree.build();

  AbstractNode* root = tree.getRoot();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->getLevel(), 0);
  const auto* bounds = static_cast<const Interval*>(root->getBounds());
  ASSERT_NE(bounds, nullptr);
  EXPECT_DOUBLE_EQ(bounds->getMin(), -1.0);
  EXPECT_DOUBLE_EQ(bounds->getMax(), 3.0);

  std::unique_ptr<std::vector<void*>> hit(tree.query(0.0));
  EXPECT_THAT(ExtractIds(*hit), UnorderedElementsAreArray({42}));

  std::unique_ptr<std::vector<void*>> miss(tree.query(3.1, 5.0));
  EXPECT_THAT(*miss, IsEmpty());
}

TEST(SIRtreeTest, DefaultCapacitySplitsAcrossMultipleLeafNodes) {
  // 25 items with default capacity 10 creates 3 level-0 nodes (sizes 11, 11, 3)
  // under 1 level-1 root node.
  SIRtree tree;
  std::vector<TaggedInterval> items;
  items.reserve(25);
  for (int i = 0; i < 25; ++i) {
    // Insert in scrambled order with negative, point, and concentric spans.
    const double center = static_cast<double>((i * 7) % 25 - 12);
    const double half_width = static_cast<double>(i % 4) * 0.5;
    items.push_back(
        {.x1 = center - half_width, .x2 = center + half_width, .id = i});
    tree.insert(items.back().x1, items.back().x2, &items.back());
  }
  tree.build();

  constexpr std::array<std::pair<double, double>, 7> kQueries = {{
      {-50.0, -40.0},
      {-12.5, -11.5},
      {-5.0, 0.0},
      {0.0, 0.0},
      {3.25, 8.75},
      {-20.0, 20.0},
      {30.0, 40.0},
  }};
  for (const auto& [q1, q2] : kQueries) {
    std::unique_ptr<std::vector<void*>> hits(tree.query(q1, q2));
    EXPECT_THAT(ExtractIds(*hits),
                UnorderedElementsAreArray(BruteForceQueryIds(items, q1, q2)));
  }

  std::unique_ptr<ItemsList> items_tree(tree.itemsTree());
  std::vector<void*> collected;
  CollectItemsList(*items_tree, collected);
  EXPECT_EQ(collected.size(), 25U);
}

TEST(SIRtreeTest, MultiLevelHierarchyWithSmallCapacity) {
  // 10 items with nodeCapacity = 2:
  // level 0 -> 4 nodes (3, 3, 3, 1), level 1 -> 2 nodes (3, 1), level 2 ->
  // root.
  constexpr std::array<IntervalSpec, 10> kSpecs = {{
      {.x1 = 18.0, .x2 = 20.0, .id = 0},
      {.x1 = -10.0, .x2 = -6.0, .id = 1},
      {.x1 = 4.0, .x2 = 6.0, .id = 2},
      {.x1 = 7.0, .x2 = 3.0, .id = 3},  // same centre 5.0 as id=2, reversed
      {.x1 = 0.0, .x2 = 0.0, .id = 4},
      {.x1 = -3.0, .x2 = -1.0, .id = 5},
      {.x1 = 10.0, .x2 = 12.0, .id = 6},
      {.x1 = 12.0, .x2 = 14.0, .id = 7},
      {.x1 = -15.0, .x2 = -12.0, .id = 8},
      {.x1 = 1.0, .x2 = 2.5, .id = 9},
  }};

  std::vector<TaggedInterval> items;
  items.reserve(kSpecs.size());
  ExposedSIRtree tree(2);
  for (const IntervalSpec& spec : kSpecs) {
    items.push_back({.x1 = spec.x1, .x2 = spec.x2, .id = spec.id});
    tree.insert(items.back().x1, items.back().x2, &items.back());
  }
  tree.build();

  AbstractNode* root = tree.getRoot();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->getLevel(), 2);

  const auto* root_bounds = static_cast<const Interval*>(root->getBounds());
  ASSERT_NE(root_bounds, nullptr);
  EXPECT_DOUBLE_EQ(root_bounds->getMin(), -15.0);
  EXPECT_DOUBLE_EQ(root_bounds->getMax(), 20.0);

  constexpr std::array<std::pair<double, double>, 8> kQueries = {{
      {-20.0, -16.0},
      {-12.0, -10.0},
      {-0.5, 0.5},
      {2.75, 2.9},
      {5.0, 5.0},
      {12.0, 12.0},
      {14.5, 17.5},
      {-20.0, 25.0},
  }};
  for (const auto& [q1, q2] : kQueries) {
    std::unique_ptr<std::vector<void*>> hits(tree.query(q1, q2));
    EXPECT_THAT(ExtractIds(*hits),
                UnorderedElementsAreArray(BruteForceQueryIds(items, q1, q2)));
  }

  std::unique_ptr<ItemsList> items_tree(tree.itemsTree());
  std::vector<void*> collected;
  CollectItemsList(*items_tree, collected);
  EXPECT_THAT(ExtractIds(collected),
              UnorderedElementsAreArray({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

TEST(SIRtreeTest, ProtectedIntersectsOpSortBoundablesParentCreationAndRemove) {
  Interval i0(4.0, 6.0);   // centre 5.0
  Interval i1(-4.0, 0.0);  // centre -2.0
  Interval i2(0.0, 2.0);   // centre 1.0
  Interval i3(-1.0, 3.0);  // centre 1.0
  Interval i4(9.0, 11.0);  // centre 10.0
  int t0 = 0;
  int t1 = 1;
  int t2 = 2;
  int t3 = 3;
  int t4 = 4;
  ItemBoundable b0(&i0, &t0);
  ItemBoundable b1(&i1, &t1);
  ItemBoundable b2(&i2, &t2);
  ItemBoundable b3(&i3, &t3);
  ItemBoundable b4(&i4, &t4);

  ExposedSIRtree tree(2);

  // Verify SIRIntersectsOp directly.
  const Interval a(0.0, 2.0);
  const Interval b_touch_right(2.0, 5.0);
  const Interval b_touch_left(-4.0, 0.0);
  const Interval b_disjoint_right(2.001, 5.0);
  const Interval b_disjoint_left(-5.0, -0.001);
  const Interval b_contained(0.5, 1.5);
  const Interval pt_on_edge(2.0, 2.0);
  const Interval pt_outside(3.0, 3.0);

  auto* intersects_op = tree.getIntersectsOp();
  ASSERT_NE(intersects_op, nullptr);
  EXPECT_TRUE(intersects_op->intersects(&a, &b_touch_right));
  EXPECT_TRUE(intersects_op->intersects(&a, &b_touch_left));
  EXPECT_FALSE(intersects_op->intersects(&a, &b_disjoint_right));
  EXPECT_FALSE(intersects_op->intersects(&a, &b_disjoint_left));
  EXPECT_TRUE(intersects_op->intersects(&a, &b_contained));
  EXPECT_TRUE(intersects_op->intersects(&a, &pt_on_edge));
  EXPECT_FALSE(intersects_op->intersects(&a, &pt_outside));

  // Verify sortBoundables orders intervals by centre (with ties and negatives).
  BoundableList unsorted = {&b0, &b1, &b2, &b3, &b4};
  std::unique_ptr<BoundableList> sorted = tree.sortBoundables(&unsorted);
  ASSERT_EQ(sorted->size(), 5U);
  for (std::size_t idx = 1; idx < sorted->size(); ++idx) {
    const double prev_c =
        static_cast<const Interval*>((*sorted)[idx - 1]->getBounds())
            ->getCentre();
    const double curr_c =
        static_cast<const Interval*>((*sorted)[idx]->getBounds())->getCentre();
    EXPECT_LE(prev_c, curr_c);
  }

  // Verify createParentBoundables with 4 items and capacity 2 groups into 2
  // SIRAbstractNodes (3 items, 1 item) with computed union Interval bounds.
  BoundableList four_items = {&b0, &b1, &b2, &b4};
  std::unique_ptr<BoundableList> parents =
      tree.createParentBoundables(&four_items, 0);
  ASSERT_EQ(parents->size(), 2U);
  const auto* p0_bounds =
      static_cast<const Interval*>((*parents)[0]->getBounds());
  const auto* p1_bounds =
      static_cast<const Interval*>((*parents)[1]->getBounds());
  EXPECT_DOUBLE_EQ(p0_bounds->getMin(), -4.0);
  EXPECT_DOUBLE_EQ(p0_bounds->getMax(), 6.0);
  EXPECT_DOUBLE_EQ(p1_bounds->getMin(), 9.0);
  EXPECT_DOUBLE_EQ(p1_bounds->getMax(), 11.0);

  // Verify sortBoundables on empty and single-element inputs.
  BoundableList empty_list;
  std::unique_ptr<BoundableList> sorted_empty =
      tree.sortBoundables(&empty_list);
  EXPECT_THAT(*sorted_empty, IsEmpty());

  BoundableList single_list = {&b0};
  std::unique_ptr<BoundableList> sorted_single =
      tree.sortBoundables(&single_list);
  ASSERT_EQ(sorted_single->size(), 1U);
  EXPECT_EQ((*sorted_single)[0], &b0);

  // Verify createParentBoundables with a single item creates 1 node.
  std::unique_ptr<BoundableList> single_parent =
      tree.createParentBoundables(&single_list, 0);
  ASSERT_EQ(single_parent->size(), 1U);
  const auto* sp_bounds =
      static_cast<const Interval*>((*single_parent)[0]->getBounds());
  EXPECT_DOUBLE_EQ(sp_bounds->getMin(), 4.0);
  EXPECT_DOUBLE_EQ(sp_bounds->getMax(), 6.0);

  // Verify protected visitor/vector query and remove on a separate populated
  // tree.
  std::array<TaggedInterval, 5> tagged = {{
      {.x1 = 0.0, .x2 = 2.0, .id = 1},
      {.x1 = 3.0, .x2 = 5.0, .id = 2},
      {.x1 = 6.0, .x2 = 8.0, .id = 3},
      {.x1 = 9.0, .x2 = 11.0, .id = 4},
      {.x1 = 12.0, .x2 = 14.0, .id = 5},
  }};
  ExposedSIRtree query_tree(3);
  for (TaggedInterval& item : tagged) {
    query_tree.insert(item.x1, item.x2, &item);
  }

  const Interval search(2.5, 8.5);
  CollectingVisitor visitor;
  query_tree.QueryWithVisitor(&search, visitor);
  EXPECT_THAT(ExtractIds(visitor.items()), UnorderedElementsAreArray({2, 3}));

  std::vector<void*> vec_before_rm;
  query_tree.QueryWithVector(&search, vec_before_rm);
  EXPECT_THAT(ExtractIds(vec_before_rm), UnorderedElementsAreArray({2, 3}));

  const Interval rm_interval(3.0, 5.0);
  EXPECT_TRUE(query_tree.RemoveItem(&rm_interval, &tagged[1]));
  EXPECT_FALSE(query_tree.RemoveItem(&rm_interval, &tagged[1]));

  CollectingVisitor after_rm_visitor;
  query_tree.QueryWithVisitor(&search, after_rm_visitor);
  EXPECT_THAT(ExtractIds(after_rm_visitor.items()),
              UnorderedElementsAreArray({3}));

  std::vector<void*> vec_after_rm;
  query_tree.QueryWithVector(&search, vec_after_rm);
  EXPECT_THAT(ExtractIds(vec_after_rm), UnorderedElementsAreArray({3}));
}

TEST(SIRtreeTest, ExactCapacitySingleNodeAndPolymorphicDelete) {
  std::array<TaggedInterval, 3> items = {{
      {.x1 = -10.0, .x2 = -4.0, .id = 1},
      {.x1 = 0.0, .x2 = 0.0, .id = 2},
      {.x1 = 12.0, .x2 = 5.0, .id = 3},
  }};

  ExposedSIRtree exposed_tree(3);
  for (TaggedInterval& item : items) {
    exposed_tree.insert(item.x1, item.x2, &item);
  }
  exposed_tree.build();
  AbstractNode* root = exposed_tree.getRoot();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->getLevel(), 0);
  const auto* root_bounds = static_cast<const Interval*>(root->getBounds());
  ASSERT_NE(root_bounds, nullptr);
  EXPECT_DOUBLE_EQ(root_bounds->getMin(), -10.0);
  EXPECT_DOUBLE_EQ(root_bounds->getMax(), 12.0);

  auto heap_tree = std::make_unique<SIRtree>(3);
  for (TaggedInterval& item : items) {
    heap_tree->insert(item.x1, item.x2, &item);
  }
  heap_tree->build();
  {
    std::unique_ptr<std::vector<void*>> hits(heap_tree->query(-4.0, 5.0));
    EXPECT_THAT(ExtractIds(*hits), UnorderedElementsAreArray({1, 2, 3}));
  }

  // Exercise virtual deleting destructor via AbstractSTRtree base pointer.
  std::unique_ptr<AbstractSTRtree> base_tree = std::move(heap_tree);
  base_tree.reset();
}

TEST(SIRtreeTest, CoincidentAndNestedIntervalsWithTieCenters) {
  std::vector<TaggedInterval> items = {
      {.x1 = 1.0, .x2 = 5.0, .id = 1},   {.x1 = 5.0, .x2 = 1.0, .id = 2},
      {.x1 = 2.0, .x2 = 4.0, .id = 3},   {.x1 = 3.0, .x2 = 3.0, .id = 4},
      {.x1 = -5.0, .x2 = -1.0, .id = 5},
  };
  SIRtree tree(2);
  for (TaggedInterval& item : items) {
    tree.insert(item.x1, item.x2, &item);
  }

  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(3.0));
    EXPECT_THAT(ExtractIds(*hits), UnorderedElementsAreArray({1, 2, 3, 4}));
  }
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(1.5));
    EXPECT_THAT(ExtractIds(*hits), UnorderedElementsAreArray({1, 2}));
  }
  {
    std::unique_ptr<std::vector<void*>> hits(tree.query(-1.0, 1.0));
    EXPECT_THAT(ExtractIds(*hits), UnorderedElementsAreArray({1, 2, 5}));
  }
}

}  // namespace
}  // namespace geos_google
