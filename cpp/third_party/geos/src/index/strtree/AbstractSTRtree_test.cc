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

// Unit tests for third_party/geos/src/index/strtree/AbstractSTRtree.cpp.
//
// google3 notes:
// - Covers AbstractSTRtree via both a minimal concrete subclass (exposing
//   protected methods and default Y-sorting createParentBoundables) and
//   STRtree.
// - Envelopes inserted into trees are stored by raw pointer; tests keep all
//   inserted Envelope instances in a std::deque outliving the tree.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/strtree/AbstractSTRtree.cpp

#include "third_party/geos/include/geos/index/strtree/AbstractSTRtree.h"

#include <array>
#include <cstddef>
#include <deque>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/strtree/AbstractNode.h"
#include "third_party/geos/include/geos/index/strtree/Boundable.h"
#include "third_party/geos/include/geos/index/strtree/ItemBoundable.h"
#include "third_party/geos/include/geos/index/strtree/STRtree.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::index::ItemVisitor;
using geos::index::strtree::AbstractNode;
using geos::index::strtree::AbstractSTRtree;
using geos::index::strtree::Boundable;
using geos::index::strtree::BoundableList;
using geos::index::strtree::ItemBoundable;
using geos::index::strtree::ItemsList;
using geos::index::strtree::ItemsListItem;
using geos::index::strtree::STRtree;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

struct BoxSpec {
  int id;
  double minx;
  double maxx;
  double miny;
  double maxy;
};

struct ItemRecord {
  int id;
  Envelope env;
};

class TestAbstractNode : public AbstractNode {
 public:
  TestAbstractNode(int level, std::size_t capacity)
      : AbstractNode(level, capacity) {}

  ~TestAbstractNode() override { delete static_cast<Envelope*>(bounds); }

 protected:
  void* computeBounds() const override {
    const BoundableList* children = getChildBoundables();
    if (children->empty()) {
      return nullptr;
    }
    auto env = std::make_unique<Envelope>();
    for (const Boundable* child : *children) {
      const auto* child_env = static_cast<const Envelope*>(child->getBounds());
      if (child_env != nullptr) {
        env->expandToInclude(child_env);
      }
    }
    if (env->isNull()) {
      return nullptr;
    }
    return env.release();
  }
};

class TestSTRtree : public AbstractSTRtree {
 public:
  explicit TestSTRtree(std::size_t capacity = 4) : AbstractSTRtree(capacity) {}

  using AbstractSTRtree::boundablesAtLevel;
  using AbstractSTRtree::createParentBoundables;
  using AbstractSTRtree::getRoot;
  using AbstractSTRtree::insert;
  using AbstractSTRtree::lastNode;
  using AbstractSTRtree::query;

  bool remove(const void* item_env, void* item) {
    return AbstractSTRtree::remove(item_env, item);
  }

  void addEmptyLevelZeroChildToRoot() {
    build();
    root->addChildBoundable(createNode(0));
  }

 protected:
  AbstractNode* createNode(int level) override {
    auto node = std::make_unique<TestAbstractNode>(level, nodeCapacity);
    nodes->push_back(node.get());
    return node.release();
  }

  IntersectsOp* getIntersectsOp() override { return &intersects_op_; }

 private:
  class TestIntersectsOp : public AbstractSTRtree::IntersectsOp {
   public:
    bool intersects(const void* a_bounds, const void* b_bounds) override {
      if (a_bounds == nullptr || b_bounds == nullptr) {
        return false;
      }
      return static_cast<const Envelope*>(a_bounds)->intersects(
          static_cast<const Envelope*>(b_bounds));
    }
  };

  TestIntersectsOp intersects_op_;
};

class CollectingVisitor : public ItemVisitor {
 public:
  void visitItem(void* item) override { items_.push_back(item); }

  const std::vector<void*>& items() const { return items_; }

 private:
  std::vector<void*> items_;
};

ItemRecord& AddRecord(std::deque<ItemRecord>& storage, int id, double minx,
                      double maxx, double miny, double maxy) {
  storage.push_back(ItemRecord{
      .id = id,
      .env = Envelope(minx, maxx, miny, maxy),
  });
  return storage.back();
}

std::vector<int> ExtractIds(const std::vector<void*>& ptrs) {
  std::vector<int> ids;
  ids.reserve(ptrs.size());
  for (void* p : ptrs) {
    ids.push_back(static_cast<const ItemRecord*>(p)->id);
  }
  return ids;
}

void CollectItemsListIds(const ItemsList& list, std::vector<int>& out_ids,
                         int& sublist_count) {
  for (const ItemsListItem& entry : list) {
    if (entry.get_type() == ItemsListItem::item_is_geometry) {
      out_ids.push_back(
          static_cast<const ItemRecord*>(entry.get_geometry())->id);
    } else {
      ++sublist_count;
      const ItemsList* child = entry.get_itemslist();
      ASSERT_NE(child, nullptr);
      EXPECT_FALSE(child->empty());
      CollectItemsListIds(*child, out_ids, sublist_count);
    }
  }
}

// Walks node hierarchy without invoking AbstractSTRtree::boundablesAtLevel on
// internal nodes, because AbstractSTRtree.cpp:313 asserts
// typeid(*boundable) == typeid(AbstractNode), which aborts in non-NDEBUG
// builds for any concrete AbstractNode subclass.
BoundableList CollectNodesAtLevel(AbstractNode* top, int target_level) {
  BoundableList result;
  if (top->getLevel() == target_level) {
    result.push_back(top);
    return result;
  }
  for (Boundable* child : *top->getChildBoundables()) {
    if (child->isLeaf()) {
      if (target_level == -1) {
        result.push_back(child);
      }
    } else {
      BoundableList sub =
          CollectNodesAtLevel(static_cast<AbstractNode*>(child), target_level);
      result.insert(result.end(), sub.begin(), sub.end());
    }
  }
  return result;
}

TEST(AbstractSTRtreeTest, NodeCapacityAndUnbuiltDestruction) {
  TestSTRtree cap_two(2);
  EXPECT_EQ(cap_two.getNodeCapacity(), 2U);

  TestSTRtree cap_ten(10);
  EXPECT_EQ(cap_ten.getNodeCapacity(), 10U);

  // Destruction of an unbuilt tree that holds inserted item boundables.
  std::deque<ItemRecord> records;
  ItemRecord& r1 = AddRecord(records, 1, 0.0, 1.0, 0.0, 1.0);
  ItemRecord& r2 = AddRecord(records, 2, 2.0, 3.0, 2.0, 3.0);
  cap_two.insert(&r1.env, &r1);
  cap_two.insert(&r2.env, &r2);
}

TEST(AbstractSTRtreeTest, BuildEmptyTreeAndIdempotence) {
  TestSTRtree tree(3);
  tree.build();
  AbstractNode* root = tree.getRoot();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->getLevel(), 0);
  EXPECT_THAT(*root->getChildBoundables(), IsEmpty());
  EXPECT_EQ(root->getBounds(), nullptr);

  // Subsequent build() calls are no-ops.
  tree.build();
  EXPECT_EQ(tree.getRoot(), root);
}

TEST(AbstractSTRtreeTest, BuildSingleAndMultiLevelHierarchy) {
  std::deque<ItemRecord> records;

  // 1 item with capacity 2 -> single level (root at level 0 with 1 leaf).
  {
    TestSTRtree single_item_tree(2);
    ItemRecord& rec = AddRecord(records, 0, 0.0, 1.0, 0.0, 1.0);
    single_item_tree.insert(&rec.env, &rec);
    single_item_tree.build();
    ASSERT_NE(single_item_tree.getRoot(), nullptr);
    EXPECT_EQ(single_item_tree.getRoot()->getLevel(), 0);
    EXPECT_THAT(*single_item_tree.getRoot()->getChildBoundables(), SizeIs(1));
  }

  // 2 items with capacity 2 -> single level (root at level 0).
  {
    TestSTRtree tree(2);
    for (int i = 0; i < 2; ++i) {
      ItemRecord& rec =
          AddRecord(records, i, i * 1.0, i * 1.0 + 0.5, i * 1.0, i * 1.0 + 0.5);
      tree.insert(&rec.env, &rec);
    }
    tree.build();
    ASSERT_NE(tree.getRoot(), nullptr);
    EXPECT_EQ(tree.getRoot()->getLevel(), 0);
    EXPECT_THAT(*tree.getRoot()->getChildBoundables(), SizeIs(2));
  }

  // 9 items with capacity 2 -> 5 level-0, 3 level-1, 2 level-2, 1 level-3 root.
  TestSTRtree deep_tree(2);
  for (int i = 0; i < 9; ++i) {
    ItemRecord& rec = AddRecord(records, i, 0.0, 1.0, i * 2.0, i * 2.0 + 1.0);
    deep_tree.insert(&rec.env, &rec);
  }
  deep_tree.build();
  ASSERT_NE(deep_tree.getRoot(), nullptr);
  EXPECT_EQ(deep_tree.getRoot()->getLevel(), 3);

  BoundableList l0 = CollectNodesAtLevel(deep_tree.getRoot(), 0);
  BoundableList l1 = CollectNodesAtLevel(deep_tree.getRoot(), 1);
  BoundableList l2 = CollectNodesAtLevel(deep_tree.getRoot(), 2);
  std::unique_ptr<BoundableList> l3 = deep_tree.boundablesAtLevel(3);
  EXPECT_THAT(l0, SizeIs(5));
  EXPECT_THAT(l1, SizeIs(3));
  EXPECT_THAT(l2, SizeIs(2));
  EXPECT_THAT(*l3, SizeIs(1));
  EXPECT_EQ(deep_tree.lastNode(&l0), l0.back());
}

TEST(AbstractSTRtreeTest, SortBoundablesYOrdersByMidpointY) {
  constexpr auto kBoxes = std::to_array<BoxSpec>({
      {.id = 50, .minx = 0.0, .maxx = 1.0, .miny = 49.0, .maxy = 51.0},
      {.id = -10, .minx = 0.0, .maxx = 1.0, .miny = -12.0, .maxy = -8.0},
      {.id = 30, .minx = 0.0, .maxx = 1.0, .miny = 25.0, .maxy = 35.0},
      {.id = 0, .minx = 0.0, .maxx = 1.0, .miny = 0.0, .maxy = 0.0},
      {.id = 20, .minx = 0.0, .maxx = 1.0, .miny = 18.0, .maxy = 22.0},
      {.id = 10, .minx = 0.0, .maxx = 1.0, .miny = 9.0, .maxy = 11.0},
  });

  std::deque<ItemRecord> records;
  TestSTRtree tree(2);
  for (const BoxSpec& spec : kBoxes) {
    ItemRecord& rec =
        AddRecord(records, spec.id, spec.minx, spec.maxx, spec.miny, spec.maxy);
    tree.insert(&rec.env, &rec);
  }
  tree.build();

  BoundableList leaves = CollectNodesAtLevel(tree.getRoot(), 0);
  ASSERT_THAT(leaves, SizeIs(3));

  auto leaf_ids = [](Boundable* b) {
    auto* node = static_cast<AbstractNode*>(b);
    std::vector<int> ids;
    for (Boundable* child : *node->getChildBoundables()) {
      auto* ib = static_cast<ItemBoundable*>(child);
      ids.push_back(static_cast<const ItemRecord*>(ib->getItem())->id);
    }
    return ids;
  };

  EXPECT_THAT(leaf_ids(leaves[0]), ElementsAre(-10, 0));
  EXPECT_THAT(leaf_ids(leaves[1]), ElementsAre(10, 20));
  EXPECT_THAT(leaf_ids(leaves[2]), ElementsAre(30, 50));

  // Verify strict weak ordering when many envelopes share identical Y midpoint.
  TestSTRtree equal_y_tree(4);
  std::deque<ItemRecord> equal_records;
  for (int i = 0; i < 40; ++i) {
    double delta = static_cast<double>(i % 5);
    ItemRecord& rec = AddRecord(equal_records, i, i * 1.0, i * 1.0 + 0.5,
                                5.0 - delta, 5.0 + delta);
    equal_y_tree.insert(&rec.env, &rec);
  }
  equal_y_tree.build();
  BoundableList all_items = CollectNodesAtLevel(equal_y_tree.getRoot(), -1);
  EXPECT_THAT(all_items, SizeIs(40));
}

TEST(AbstractSTRtreeTest, IterateDoesNotBuildTree) {
  TestSTRtree tree(2);
  CollectingVisitor empty_visitor;
  tree.iterate(empty_visitor);
  EXPECT_THAT(empty_visitor.items(), IsEmpty());

  std::deque<ItemRecord> records;
  for (int i = 1; i <= 3; ++i) {
    ItemRecord& rec = AddRecord(records, i, 0.0, 1.0, i * 1.0, i * 1.0 + 0.5);
    tree.insert(&rec.env, &rec);
  }

  CollectingVisitor before_build_visitor;
  tree.iterate(before_build_visitor);
  EXPECT_THAT(ExtractIds(before_build_visitor.items()), ElementsAre(1, 2, 3));

  // Tree remains unbuilt after iterate(), so insert() is still allowed.
  ItemRecord& extra = AddRecord(records, 4, 0.0, 1.0, 4.0, 4.5);
  tree.insert(&extra.env, &extra);

  tree.build();
  CollectingVisitor after_build_visitor;
  tree.iterate(after_build_visitor);
  EXPECT_THAT(ExtractIds(after_build_visitor.items()), ElementsAre(1, 2, 3, 4));
}

TEST(AbstractSTRtreeTest, QueryVectorOverloadsAndAutoBuild) {
  // Empty tree query auto-builds and yields no matches.
  {
    TestSTRtree empty_tree(2);
    Envelope search(0.0, 10.0, 0.0, 10.0);
    std::vector<void*> matches;
    empty_tree.query(&search, matches);
    EXPECT_THAT(matches, IsEmpty());

    // Calling again after already built.
    empty_tree.query(&search, matches);
    EXPECT_THAT(matches, IsEmpty());
  }

  std::deque<ItemRecord> records;
  TestSTRtree tree(2);
  for (int i = 0; i < 9; ++i) {
    ItemRecord& rec = AddRecord(records, i, i * 10.0, i * 10.0 + 2.0, i * 10.0,
                                i * 10.0 + 2.0);
    tree.insert(&rec.env, &rec);
  }

  // First query auto-builds the multi-level tree.
  Envelope partial_search(19.0, 41.0, 19.0, 41.0);
  std::vector<void*> matches;
  tree.query(&partial_search, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAre(2, 3, 4));

  // Query outside root bounds returns early with no matches.
  Envelope disjoint_search(500.0, 600.0, 500.0, 600.0);
  matches.clear();
  tree.query(&disjoint_search, matches);
  EXPECT_THAT(matches, IsEmpty());

  // Touching boundary point matches item 0.
  Envelope touch_search(2.0, 5.0, 2.0, 5.0);
  matches.clear();
  tree.query(&touch_search, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAre(0));

  // Direct public 3-arg query and protected 3-arg visitor query scoped to a
  // single level-1 subtree.
  BoundableList level1_nodes = CollectNodesAtLevel(tree.getRoot(), 1);
  ASSERT_THAT(level1_nodes, SizeIs(3));
  const auto* first_subtree = static_cast<const AbstractNode*>(level1_nodes[0]);
  Envelope wide_search(-10.0, 1000.0, -10.0, 1000.0);
  matches.clear();
  tree.query(&wide_search, first_subtree, &matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAre(0, 1, 2, 3));

  CollectingVisitor subtree_visitor;
  tree.query(&wide_search, *first_subtree, subtree_visitor);
  EXPECT_THAT(ExtractIds(subtree_visitor.items()),
              UnorderedElementsAre(0, 1, 2, 3));
}

TEST(AbstractSTRtreeTest, QueryVisitorOverloadsAndAutoBuild) {
  // Empty tree visitor query auto-builds and visits nothing.
  {
    TestSTRtree empty_tree(2);
    Envelope search(0.0, 10.0, 0.0, 10.0);
    CollectingVisitor visitor;
    empty_tree.query(&search, visitor);
    EXPECT_THAT(visitor.items(), IsEmpty());

    // Calling again after already built.
    empty_tree.query(&search, visitor);
    EXPECT_THAT(visitor.items(), IsEmpty());
  }

  std::deque<ItemRecord> records;
  TestSTRtree tree(2);
  for (int i = 0; i < 9; ++i) {
    ItemRecord& rec =
        AddRecord(records, i, i * 5.0, i * 5.0 + 1.0, i * 5.0, i * 5.0 + 1.0);
    tree.insert(&rec.env, &rec);
  }

  // First visitor query triggers build().
  Envelope search(10.0, 25.0, 10.0, 25.0);
  CollectingVisitor visitor;
  tree.query(&search, visitor);
  EXPECT_THAT(ExtractIds(visitor.items()), UnorderedElementsAre(2, 3, 4, 5));

  // Query outside root bounds does not invoke visitor.
  Envelope outside(1000.0, 1010.0, 1000.0, 1010.0);
  CollectingVisitor outside_visitor;
  tree.query(&outside, outside_visitor);
  EXPECT_THAT(outside_visitor.items(), IsEmpty());

  // Query intersecting everything.
  Envelope all(-1.0, 100.0, -1.0, 100.0);
  CollectingVisitor all_visitor;
  tree.query(&all, all_visitor);
  EXPECT_THAT(ExtractIds(all_visitor.items()),
              UnorderedElementsAre(0, 1, 2, 3, 4, 5, 6, 7, 8));
}

TEST(AbstractSTRtreeTest, BoundablesAtLevelCoversLeafAndRootLevels) {
  // Empty tree.
  {
    TestSTRtree empty_tree(2);
    empty_tree.build();
    EXPECT_THAT(*empty_tree.boundablesAtLevel(-1), IsEmpty());
    EXPECT_THAT(*empty_tree.boundablesAtLevel(0),
                ElementsAre(empty_tree.getRoot()));
    EXPECT_THAT(*empty_tree.boundablesAtLevel(1), IsEmpty());
  }

  // Single-level tree (root at level 0 with leaf ItemBoundable children).
  std::deque<ItemRecord> records;
  {
    TestSTRtree single_tree(4);
    for (int i = 0; i < 3; ++i) {
      ItemRecord& rec = AddRecord(records, i, i * 1.0, i * 1.0 + 0.5, 0.0, 1.0);
      single_tree.insert(&rec.env, &rec);
    }
    single_tree.build();
    EXPECT_THAT(*single_tree.boundablesAtLevel(-1), SizeIs(3));
    EXPECT_THAT(*single_tree.boundablesAtLevel(0),
                ElementsAre(single_tree.getRoot()));
    // Level higher than root descends to ItemBoundables where level != -1.
    EXPECT_THAT(*single_tree.boundablesAtLevel(1), IsEmpty());
  }

  // Multi-level tree: boundablesAtLevel works at root level and on level-0
  // leaf-parent nodes whose children are ItemBoundables. (Descending through
  // internal AbstractNode children triggers AbstractSTRtree.cpp:313's
  // assert(typeid(*boundable) == typeid(AbstractNode)).)
  TestSTRtree deep_tree(2);
  for (int i = 0; i < 7; ++i) {
    ItemRecord& rec = AddRecord(records, i, 0.0, 1.0, i * 3.0, i * 3.0 + 1.0);
    deep_tree.insert(&rec.env, &rec);
  }
  deep_tree.build();
  ASSERT_EQ(deep_tree.getRoot()->getLevel(), 2);

  BoundableList level0 = CollectNodesAtLevel(deep_tree.getRoot(), 0);
  ASSERT_THAT(level0, SizeIs(4));
  auto* first_l0 = static_cast<AbstractNode*>(level0[0]);

  BoundableList node_at_level0;
  deep_tree.boundablesAtLevel(0, first_l0, &node_at_level0);
  EXPECT_THAT(node_at_level0, ElementsAre(first_l0));

  BoundableList subtree_items;
  deep_tree.boundablesAtLevel(-1, first_l0, &subtree_items);
  EXPECT_THAT(subtree_items, SizeIs(2));

  BoundableList above_l0;
  deep_tree.boundablesAtLevel(1, first_l0, &above_l0);
  EXPECT_THAT(above_l0, IsEmpty());

  EXPECT_THAT(*deep_tree.boundablesAtLevel(2),
              ElementsAre(deep_tree.getRoot()));
}

TEST(AbstractSTRtreeTest, RemoveEmptyDisjointAndAutoBuild) {
  // Empty tree remove triggers auto-build and returns false (also when built).
  TestSTRtree empty_tree(2);
  Envelope search(0.0, 1.0, 0.0, 1.0);
  int dummy = 0;
  EXPECT_FALSE(empty_tree.remove(&search, &dummy));
  EXPECT_FALSE(empty_tree.remove(&search, &dummy));

  // Unbuilt non-empty tree auto-builds inside remove() and removes item.
  std::deque<ItemRecord> records;
  TestSTRtree tree(2);
  ItemRecord& r1 = AddRecord(records, 1, 0.0, 2.0, 0.0, 2.0);
  ItemRecord& r2 = AddRecord(records, 2, 10.0, 12.0, 10.0, 12.0);
  tree.insert(&r1.env, &r1);
  tree.insert(&r2.env, &r2);

  // Disjoint search bounds returns false without removing anything.
  Envelope far_away(100.0, 101.0, 100.0, 101.0);
  EXPECT_FALSE(tree.remove(&far_away, &r1));

  EXPECT_TRUE(tree.remove(&r1.env, &r1));
  EXPECT_FALSE(tree.remove(&r1.env, &r1));

  Envelope all(0.0, 20.0, 0.0, 20.0);
  std::vector<void*> matches;
  tree.query(&all, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAre(2));

  EXPECT_TRUE(tree.remove(&r2.env, &r2));
  EXPECT_FALSE(tree.remove(&r2.env, &r2));
}

TEST(AbstractSTRtreeTest, RemoveSingleLevelLeafMissAndDuplicatePointer) {
  std::deque<ItemRecord> records;
  TestSTRtree tree(4);
  ItemRecord& r1 = AddRecord(records, 1, 0.0, 2.0, 0.0, 2.0);
  ItemRecord& r2 = AddRecord(records, 2, 10.0, 12.0, 10.0, 12.0);
  ItemRecord& r3 = AddRecord(records, 3, 20.0, 22.0, 20.0, 22.0);
  ItemRecord missing{.id = 99, .env = Envelope(0.0, 2.0, 0.0, 2.0)};

  tree.insert(&r1.env, &r1);
  tree.insert(&r2.env, &r2);
  tree.insert(&r2.env, &r2);
  tree.insert(&r3.env, &r3);
  tree.build();

  // Intersects r1 only, but asks to remove `missing` -> leaf children checked,
  // r2/r3 skipped by bounds check, r1 fails pointer match -> returns false.
  EXPECT_FALSE(tree.remove(&r1.env, &missing));

  // Removing duplicated pointer removes one occurrence per call.
  EXPECT_TRUE(tree.remove(&r2.env, &r2));
  Envelope all(0.0, 30.0, 0.0, 30.0);
  std::vector<void*> matches;
  tree.query(&all, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAre(1, 2, 3));

  EXPECT_TRUE(tree.remove(&r2.env, &r2));
  EXPECT_FALSE(tree.remove(&r2.env, &r2));
  matches.clear();
  tree.query(&all, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAre(1, 3));
}

TEST(AbstractSTRtreeTest, RemoveMultiLevelOverlappingSubtreesAndPruning) {
  // Overlapping Y bounds place items 0..3 across two level-0 nodes whose
  // bounding envelopes overlap on X and Y, while items 4..5 are far away.
  constexpr auto kBoxes = std::to_array<BoxSpec>({
      {.id = 0, .minx = 0.0, .maxx = 10.0, .miny = 0.0, .maxy = 10.0},
      {.id = 1, .minx = 0.0, .maxx = 10.0, .miny = 1.0, .maxy = 11.0},
      {.id = 2, .minx = 0.0, .maxx = 10.0, .miny = 2.0, .maxy = 12.0},
      {.id = 3, .minx = 0.0, .maxx = 10.0, .miny = 3.0, .maxy = 13.0},
      {.id = 4, .minx = 100.0, .maxx = 110.0, .miny = 100.0, .maxy = 110.0},
      {.id = 5, .minx = 100.0, .maxx = 110.0, .miny = 101.0, .maxy = 111.0},
  });

  std::deque<ItemRecord> records;
  TestSTRtree tree(2);
  std::vector<ItemRecord*> ptrs;
  for (const BoxSpec& spec : kBoxes) {
    ItemRecord& rec =
        AddRecord(records, spec.id, spec.minx, spec.maxx, spec.miny, spec.maxy);
    ptrs.push_back(&rec);
    tree.insert(&rec.env, &rec);
  }
  tree.build();
  ASSERT_EQ(tree.getRoot()->getLevel(), 2);

  // Item 2 lives in the second level-0 node, while its envelope overlaps the
  // first level-0 node and is disjoint from the third level-0 node (items 4,5).
  EXPECT_TRUE(tree.remove(&ptrs[2]->env, ptrs[2]));
  // Removing item 3 empties the second level-0 node, pruning it from level 1.
  EXPECT_TRUE(tree.remove(&ptrs[3]->env, ptrs[3]));
  EXPECT_THAT(CollectNodesAtLevel(tree.getRoot(), 0), SizeIs(2));

  // Attempting to remove an absent item whose bounds overlap remaining nodes.
  EXPECT_FALSE(tree.remove(&ptrs[2]->env, ptrs[2]));

  // Removing items 4 and 5 empties the entire second level-1 subtree, pruning
  // both its level-0 node and its level-1 node from root.
  EXPECT_TRUE(tree.remove(&ptrs[4]->env, ptrs[4]));
  EXPECT_TRUE(tree.remove(&ptrs[5]->env, ptrs[5]));
  EXPECT_THAT(CollectNodesAtLevel(tree.getRoot(), 0), SizeIs(1));
  EXPECT_THAT(CollectNodesAtLevel(tree.getRoot(), 1), SizeIs(1));

  // Remove final remaining items 0 and 1.
  EXPECT_TRUE(tree.remove(&ptrs[0]->env, ptrs[0]));
  EXPECT_TRUE(tree.remove(&ptrs[1]->env, ptrs[1]));
  EXPECT_THAT(CollectNodesAtLevel(tree.getRoot(), 0), IsEmpty());
  EXPECT_FALSE(tree.remove(&ptrs[0]->env, ptrs[0]));

  Envelope all(-10.0, 200.0, -10.0, 200.0);
  std::vector<void*> matches;
  tree.query(&all, matches);
  EXPECT_THAT(matches, IsEmpty());
}

TEST(AbstractSTRtreeTest, ItemsListAndItemsTreeLifecycle) {
  // Direct ItemsList / ItemsListItem unit behavior and owned-sublist cleanup.
  int a = 10;
  int b = 20;
  auto root_list = std::make_unique<ItemsList>();
  root_list->push_back(&a);
  auto* child_list = new ItemsList();
  child_list->push_back(&b);
  root_list->push_back_owned(child_list);

  ASSERT_THAT(*root_list, SizeIs(2));
  EXPECT_EQ((*root_list)[0].get_type(), ItemsListItem::item_is_geometry);
  EXPECT_EQ((*root_list)[0].get_geometry(), &a);
  EXPECT_EQ((*root_list)[1].get_type(), ItemsListItem::item_is_list);
  EXPECT_EQ((*root_list)[1].get_itemslist(), child_list);

  // Empty tree itemsTree() auto-builds and returns an empty ItemsList.
  {
    TestSTRtree empty_tree(2);
    std::unique_ptr<ItemsList> items_tree(empty_tree.itemsTree());
    ASSERT_NE(items_tree, nullptr);
    EXPECT_TRUE(items_tree->empty());
  }

  // Single-level itemsTree().
  std::deque<ItemRecord> records;
  {
    TestSTRtree single_tree(3);
    for (int i = 1; i <= 2; ++i) {
      ItemRecord& rec = AddRecord(records, i, 0.0, 1.0, i * 1.0, i * 1.0 + 0.5);
      single_tree.insert(&rec.env, &rec);
    }
    std::unique_ptr<ItemsList> items_tree(single_tree.itemsTree());
    ASSERT_NE(items_tree, nullptr);
    std::vector<int> ids;
    int sublists = 0;
    CollectItemsListIds(*items_tree, ids, sublists);
    EXPECT_EQ(sublists, 0);
    EXPECT_THAT(ids, UnorderedElementsAre(1, 2));
  }

  // Multi-level itemsTree(), skipping empty subtrees and after removals.
  TestSTRtree tree(2);
  std::vector<ItemRecord*> ptrs;
  for (int i = 0; i < 5; ++i) {
    ItemRecord& rec = AddRecord(records, i, 0.0, 1.0, i * 2.0, i * 2.0 + 1.0);
    ptrs.push_back(&rec);
    tree.insert(&rec.env, &rec);
  }
  tree.addEmptyLevelZeroChildToRoot();

  std::unique_ptr<ItemsList> items_tree(tree.itemsTree());
  ASSERT_NE(items_tree, nullptr);
  std::vector<int> ids;
  int sublists = 0;
  CollectItemsListIds(*items_tree, ids, sublists);
  EXPECT_GT(sublists, 0);
  EXPECT_THAT(ids, UnorderedElementsAre(0, 1, 2, 3, 4));

  for (ItemRecord* rec : ptrs) {
    EXPECT_TRUE(tree.remove(&rec->env, rec));
  }
  std::unique_ptr<ItemsList> emptied_tree(tree.itemsTree());
  ASSERT_NE(emptied_tree, nullptr);
  EXPECT_TRUE(emptied_tree->empty());
}

TEST(AbstractSTRtreeTest, ConcreteSTRtreeExercisesAbstractSTRtree) {
  std::deque<ItemRecord> records;
  STRtree tree(2);
  EXPECT_EQ(tree.getNodeCapacity(), 2U);

  for (int x = 0; x < 4; ++x) {
    for (int y = 0; y < 3; ++y) {
      int id = x * 10 + y;
      ItemRecord& rec = AddRecord(records, id, x * 2.0, x * 2.0 + 1.0, y * 2.0,
                                  y * 2.0 + 1.0);
      tree.insert(&rec.env, &rec);
    }
  }
  tree.build();

  Envelope query_env(1.5, 4.5, 1.5, 4.5);
  std::vector<void*> matches;
  tree.query(&query_env, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAre(11, 12, 21, 22));

  CollectingVisitor visitor;
  tree.query(&query_env, visitor);
  EXPECT_THAT(ExtractIds(visitor.items()),
              UnorderedElementsAre(11, 12, 21, 22));

  std::unique_ptr<ItemsList> nested(tree.itemsTree());
  ASSERT_NE(nested, nullptr);
  std::vector<int> all_ids;
  int sublists = 0;
  CollectItemsListIds(*nested, all_ids, sublists);
  EXPECT_GT(sublists, 0);
  EXPECT_THAT(all_ids, SizeIs(12));

  EXPECT_TRUE(tree.remove(&records[0].env, &records[0]));
  matches.clear();
  Envelope corner_env(0.0, 0.5, 0.0, 0.5);
  tree.query(&corner_env, matches);
  EXPECT_THAT(matches, IsEmpty());
}

}  // namespace
}  // namespace geos_google
