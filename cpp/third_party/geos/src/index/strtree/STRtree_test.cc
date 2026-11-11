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

// Unit tests for third_party/geos/src/index/strtree/STRtree.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - STRtree stores raw pointers to caller-owned Envelope and item objects;
//   inserted envelopes and items must outlive the STRtree.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/strtree/STRtree.cpp

#include "third_party/geos/include/geos/index/strtree/STRtree.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/strtree/AbstractNode.h"
#include "third_party/geos/include/geos/index/strtree/AbstractSTRtree.h"
#include "third_party/geos/include/geos/index/strtree/BoundablePair.h"
#include "third_party/geos/include/geos/index/strtree/GeometryItemDistance.h"
#include "third_party/geos/include/geos/index/strtree/ItemBoundable.h"
#include "third_party/geos/include/geos/index/strtree/ItemDistance.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Point;
using geos::index::ItemVisitor;
using geos::index::strtree::AbstractNode;
using geos::index::strtree::BoundableList;
using geos::index::strtree::BoundablePair;
using geos::index::strtree::GeometryItemDistance;
using geos::index::strtree::ItemBoundable;
using geos::index::strtree::ItemDistance;
using geos::index::strtree::ItemsList;
using geos::index::strtree::ItemsListItem;
using geos::index::strtree::STRtree;
using geos::util::GEOSException;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAreArray;

struct BoxSpec {
  double min_x;
  double max_x;
  double min_y;
  double max_y;
  int id;
};

struct TaggedBox {
  Envelope env;
  int id;
};

class CollectingVisitor final : public ItemVisitor {
 public:
  void visitItem(void* item) override { items_.push_back(item); }

  const std::vector<void*>& items() const { return items_; }

 private:
  std::vector<void*> items_;
};

class EnvelopeItemDistance final : public ItemDistance {
 public:
  explicit EnvelopeItemDistance(bool exclude_same_item = false)
      : exclude_same_item_(exclude_same_item) {}

  double distance(const ItemBoundable* item1,
                  const ItemBoundable* item2) override {
    if (exclude_same_item_ && item1->getItem() == item2->getItem()) {
      return geos::DoubleMax;
    }
    const auto* e1 = static_cast<const Envelope*>(item1->getBounds());
    const auto* e2 = static_cast<const Envelope*>(item2->getBounds());
    return e1->distance(*e2);
  }

 private:
  bool exclude_same_item_;
};

// Exposes protected AbstractSTRtree accessors needed to test BoundablePair
// overloads and tree-level decomposition directly.
class ExposedSTRtree final : public STRtree {
 public:
  using STRtree::boundablesAtLevel;
  using STRtree::getRoot;
  using STRtree::STRtree;
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
  for (void* p : ptrs) {
    ids.push_back(static_cast<const TaggedBox*>(p)->id);
  }
  return ids;
}

TEST(STRtreeTest, DefaultAndCustomNodeCapacity) {
  STRtree default_tree;
  EXPECT_EQ(default_tree.getNodeCapacity(), 10U);

  STRtree cap2_tree(2);
  EXPECT_EQ(cap2_tree.getNodeCapacity(), 2U);

  STRtree cap5_tree(5);
  EXPECT_EQ(cap5_tree.getNodeCapacity(), 5U);
}

TEST(STRtreeTest, StaticAvgAndCentreY) {
  EXPECT_DOUBLE_EQ(STRtree::avg(0.0, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(STRtree::avg(-4.0, 6.0), 1.0);
  EXPECT_DOUBLE_EQ(STRtree::avg(2.5, 7.5), 5.0);

  const Envelope box(1.0, 5.0, -3.0, 9.0);
  EXPECT_DOUBLE_EQ(STRtree::centreY(&box), 3.0);

  const Envelope point_env(CoordinateXY(4.0, -2.5));
  EXPECT_DOUBLE_EQ(STRtree::centreY(&point_env), -2.5);
}

TEST(STRtreeTest, EmptyTreeQueryIterateAndItemsTree) {
  STRtree tree(4);
  const Envelope search(-10.0, 10.0, -10.0, 10.0);

  std::vector<void*> matches;
  tree.query(&search, matches);
  EXPECT_THAT(matches, IsEmpty());

  // Calling build() after implicit build via query() is a no-op.
  tree.build();

  CollectingVisitor query_visitor;
  tree.query(&search, query_visitor);
  EXPECT_THAT(query_visitor.items(), IsEmpty());

  CollectingVisitor iter_visitor;
  tree.iterate(iter_visitor);
  EXPECT_THAT(iter_visitor.items(), IsEmpty());

  std::unique_ptr<ItemsList> items_tree(tree.itemsTree());
  ASSERT_NE(items_tree, nullptr);
  EXPECT_THAT(*items_tree, IsEmpty());
}

TEST(STRtreeTest, EmptyTreeNearestNeighbourAndWithinDistanceThrow) {
  STRtree empty_a(2);
  STRtree empty_b(2);
  empty_a.build();
  empty_b.build();

  STRtree non_empty(2);
  Envelope item_env(1.0, 2.0, 1.0, 2.0);
  int item = 42;
  non_empty.insert(&item_env, &item);
  non_empty.build();

  EnvelopeItemDistance dist;
  // Calling nearestNeighbour or isWithinDistance on a built empty tree reaches
  // STRAbstractNode::computeBounds() with empty childBoundables (null bounds),
  // triggering GEOSException inside BoundablePair::distance().
  EXPECT_THROW(empty_a.nearestNeighbour(&dist), GEOSException);
  EXPECT_THROW(empty_a.nearestNeighbour(&item_env, &item, &dist),
               GEOSException);
  EXPECT_THROW(empty_a.nearestNeighbour(&non_empty, &dist), GEOSException);
  EXPECT_THROW(non_empty.nearestNeighbour(&empty_b, &dist), GEOSException);
  EXPECT_THROW(empty_a.isWithinDistance(&non_empty, &dist, 100.0),
               GEOSException);
  EXPECT_THROW(non_empty.isWithinDistance(&empty_b, &dist, 100.0),
               GEOSException);
}

TEST(STRtreeTest, InsertIgnoresNullEnvelope) {
  STRtree tree(3);
  Envelope null_env;
  ASSERT_TRUE(null_env.isNull());
  int ignored_item = -1;
  tree.insert(&null_env, &ignored_item);

  TaggedBox valid{Envelope(2.0, 4.0, 3.0, 5.0), 7};
  tree.insert(&valid.env, &valid);

  CollectingVisitor iter_visitor;
  tree.iterate(iter_visitor);
  EXPECT_THAT(ExtractIds(iter_visitor.items()), UnorderedElementsAreArray({7}));

  const Envelope full(-100.0, 100.0, -100.0, 100.0);
  std::vector<void*> matches;
  tree.query(&full, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray({7}));
}

TEST(STRtreeTest, IterateBeforeBuildAllowsMoreInserts) {
  STRtree tree(3);
  TaggedBox first{Envelope(0.0, 1.0, 0.0, 1.0), 1};
  TaggedBox second{Envelope(5.0, 6.0, 5.0, 6.0), 2};
  tree.insert(&first.env, &first);

  CollectingVisitor iter_visitor;
  tree.iterate(iter_visitor);
  EXPECT_THAT(ExtractIds(iter_visitor.items()), UnorderedElementsAreArray({1}));

  // iterate() does not build the tree, so additional inserts are still valid.
  tree.insert(&second.env, &second);

  const Envelope all(-1.0, 10.0, -1.0, 10.0);
  std::vector<void*> matches;
  tree.query(&all, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray({1, 2}));
}

TEST(STRtreeTest, QueryVectorAndVisitorCoverBoundaryAndDisjointCases) {
  constexpr auto kSpecs = std::to_array<BoxSpec>({
      {.min_x = 0.0, .max_x = 2.0, .min_y = 0.0, .max_y = 2.0, .id = 1},
      {.min_x = 4.0, .max_x = 6.0, .min_y = 0.0, .max_y = 2.0, .id = 2},
      {.min_x = 0.0, .max_x = 2.0, .min_y = 4.0, .max_y = 6.0, .id = 3},
      {.min_x = 4.0, .max_x = 6.0, .min_y = 4.0, .max_y = 6.0, .id = 4},
      {.min_x = 10.0, .max_x = 12.0, .min_y = 10.0, .max_y = 12.0, .id = 5},
  });

  std::vector<TaggedBox> boxes;
  boxes.reserve(kSpecs.size());
  STRtree tree(2);
  for (const BoxSpec& spec : kSpecs) {
    boxes.push_back(
        {Envelope(spec.min_x, spec.max_x, spec.min_y, spec.max_y), spec.id});
    tree.insert(&boxes.back().env, &boxes.back());
  }

  // Outside root bounds.
  {
    const Envelope outside(20.0, 25.0, 20.0, 25.0);
    std::vector<void*> matches;
    tree.query(&outside, matches);
    EXPECT_THAT(matches, IsEmpty());

    CollectingVisitor visitor;
    tree.query(&outside, visitor);
    EXPECT_THAT(visitor.items(), IsEmpty());
  }

  // Inside root bounds [0,12]x[0,12], disjoint from every leaf envelope.
  {
    const Envelope hole(2.5, 3.5, 2.5, 3.5);
    std::vector<void*> matches;
    tree.query(&hole, matches);
    EXPECT_THAT(matches, IsEmpty());

    CollectingVisitor visitor;
    tree.query(&hole, visitor);
    EXPECT_THAT(visitor.items(), IsEmpty());
  }

  // Boundary touch at (2, 2) hits only box 1.
  {
    const Envelope corner(CoordinateXY(2.0, 2.0));
    std::vector<void*> matches;
    tree.query(&corner, matches);
    EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray({1}));

    CollectingVisitor visitor;
    tree.query(&corner, visitor);
    EXPECT_THAT(ExtractIds(visitor.items()), UnorderedElementsAreArray({1}));
  }

  // Partial overlap matching boxes 2 and 4.
  {
    const Envelope right_strip(4.5, 5.5, 1.5, 4.5);
    std::vector<void*> matches;
    tree.query(&right_strip, matches);
    EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray({2, 4}));

    CollectingVisitor visitor;
    tree.query(&right_strip, visitor);
    EXPECT_THAT(ExtractIds(visitor.items()), UnorderedElementsAreArray({2, 4}));
  }
}

TEST(STRtreeTest, SingleItemAndNonDivisibleSlicesBuildMultiLevelHierarchy) {
  // Single-item tree: minLeafCount = 1, sliceCount = 1.
  {
    STRtree single_tree(2);
    TaggedBox only{Envelope(1.0, 3.0, -2.0, 0.0), 99};
    single_tree.insert(&only.env, &only);
    single_tree.build();

    std::unique_ptr<ItemsList> items(single_tree.itemsTree());
    std::vector<void*> collected;
    CollectItemsList(*items, collected);
    EXPECT_THAT(ExtractIds(collected), UnorderedElementsAreArray({99}));
  }

  // 10 unsorted items with nodeCapacity = 2:
  // minLeafCount = ceil(10/2) = 5, sliceCount = ceil(sqrt(5)) = 3,
  // sliceCapacity = ceil(10/3) = 4 -> slices have 4, 4, and 2 items.
  // Also tests negative coordinates, point envelopes, and equal X midpoints.
  constexpr auto kTenSpecs = std::to_array<BoxSpec>({
      {.min_x = 9.0, .max_x = 11.0, .min_y = 8.0, .max_y = 10.0, .id = 0},
      {.min_x = -5.0, .max_x = -3.0, .min_y = 7.0, .max_y = 9.0, .id = 1},
      {.min_x = 3.0, .max_x = 3.0, .min_y = 2.0, .max_y = 2.0, .id = 2},
      {.min_x = 0.0, .max_x = 6.0, .min_y = -4.0, .max_y = 0.0, .id = 3},
      {.min_x = 1.0, .max_x = 5.0, .min_y = 5.0, .max_y = 7.0, .id = 4},
      {.min_x = -8.0, .max_x = -6.0, .min_y = -2.0, .max_y = 1.0, .id = 5},
      {.min_x = 15.0, .max_x = 17.0, .min_y = -5.0, .max_y = -3.0, .id = 6},
      {.min_x = 4.0, .max_x = 8.0, .min_y = 12.0, .max_y = 14.0, .id = 7},
      {.min_x = -2.0, .max_x = 0.0, .min_y = 3.0, .max_y = 4.0, .id = 8},
      {.min_x = 6.0, .max_x = 8.0, .min_y = -1.0, .max_y = 1.0, .id = 9},
  });

  std::vector<TaggedBox> boxes;
  boxes.reserve(kTenSpecs.size());
  ExposedSTRtree tree(2);
  for (const BoxSpec& spec : kTenSpecs) {
    boxes.push_back(
        {Envelope(spec.min_x, spec.max_x, spec.min_y, spec.max_y), spec.id});
    tree.insert(&boxes.back().env, &boxes.back());
  }
  tree.build();

  AbstractNode* root = tree.getRoot();
  ASSERT_NE(root, nullptr);
  EXPECT_GE(root->getLevel(), 2);

  std::unique_ptr<BoundableList> leaf_items = tree.boundablesAtLevel(-1);
  EXPECT_EQ(leaf_items->size(), kTenSpecs.size());

  std::unique_ptr<ItemsList> items(tree.itemsTree());
  std::vector<void*> collected;
  CollectItemsList(*items, collected);
  EXPECT_THAT(ExtractIds(collected),
              UnorderedElementsAreArray({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

TEST(STRtreeTest, RemoveExistingAndMissingItems) {
  constexpr auto kSpecs = std::to_array<BoxSpec>({
      {.min_x = 0.0, .max_x = 1.0, .min_y = 0.0, .max_y = 1.0, .id = 10},
      {.min_x = 2.0, .max_x = 3.0, .min_y = 0.0, .max_y = 1.0, .id = 20},
      {.min_x = 4.0, .max_x = 5.0, .min_y = 0.0, .max_y = 1.0, .id = 30},
      {.min_x = 6.0, .max_x = 7.0, .min_y = 0.0, .max_y = 1.0, .id = 40},
  });

  std::vector<TaggedBox> boxes;
  boxes.reserve(kSpecs.size());
  STRtree tree(2);
  for (const BoxSpec& spec : kSpecs) {
    boxes.push_back(
        {Envelope(spec.min_x, spec.max_x, spec.min_y, spec.max_y), spec.id});
    tree.insert(&boxes.back().env, &boxes.back());
  }

  // Disjoint search envelope fails to find item even though item is in tree.
  const Envelope wrong_env(100.0, 101.0, 100.0, 101.0);
  EXPECT_FALSE(tree.remove(&wrong_env, &boxes[0]));

  // Remove existing item, then verify duplicate removal returns false.
  EXPECT_TRUE(tree.remove(&boxes[1].env, &boxes[1]));
  EXPECT_FALSE(tree.remove(&boxes[1].env, &boxes[1]));

  const Envelope all(-1.0, 10.0, -1.0, 10.0);
  std::vector<void*> matches;
  tree.query(&all, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray({10, 30, 40}));

  // Remove remaining items and verify subsequent remove on emptied tree fails.
  EXPECT_TRUE(tree.remove(&boxes[0].env, &boxes[0]));
  EXPECT_TRUE(tree.remove(&boxes[2].env, &boxes[2]));
  EXPECT_TRUE(tree.remove(&boxes[3].env, &boxes[3]));

  matches.clear();
  tree.query(&all, matches);
  EXPECT_THAT(matches, IsEmpty());
  EXPECT_FALSE(tree.remove(&boxes[0].env, &boxes[0]));
}

TEST(STRtreeTest, NearestNeighbourSingleQueryItem) {
  std::vector<TaggedBox> items;
  items.reserve(16);
  STRtree tree(2);
  for (int x = 0; x < 4; ++x) {
    for (int y = 0; y < 4; ++y) {
      double dx = static_cast<double>(x) * 3.0;
      double dy = static_cast<double>(y) * 3.0;
      items.push_back({Envelope(dx, dx + 1.0, dy, dy + 1.0), x * 10 + y});
    }
  }
  for (TaggedBox& item : items) {
    tree.insert(&item.env, &item);
  }

  EnvelopeItemDistance dist;

  // First call on an unbuilt tree implicitly calls build().
  const Envelope query_near_21(6.1, 6.2, 3.1, 3.2);
  int query_tag = 0;
  const void* nearest =
      tree.nearestNeighbour(&query_near_21, &query_tag, &dist);
  ASSERT_NE(nearest, nullptr);
  EXPECT_EQ(static_cast<const TaggedBox*>(nearest)->id, 21);

  // Overlapping envelope yields distance 0.0 and exits the queue loop early
  // while candidate pairs remain in the priority queue.
  const Envelope overlapping_00(0.2, 0.8, 0.2, 0.8);
  const void* overlap_nearest =
      tree.nearestNeighbour(&overlapping_00, &query_tag, &dist);
  ASSERT_NE(overlap_nearest, nullptr);
  EXPECT_EQ(static_cast<const TaggedBox*>(overlap_nearest)->id, 0);

  // Distant query forces queue pruning and break on currentDistance >=
  // lowerBound.
  const Envelope query_far(20.0, 21.0, 20.0, 21.0);
  const void* far_nearest =
      tree.nearestNeighbour(&query_far, &query_tag, &dist);
  ASSERT_NE(far_nearest, nullptr);
  EXPECT_EQ(static_cast<const TaggedBox*>(far_nearest)->id, 33);
}

TEST(STRtreeTest, NearestNeighbourSelfInTree) {
  constexpr auto kSpecs = std::to_array<BoxSpec>({
      {.min_x = 0.0, .max_x = 1.0, .min_y = 0.0, .max_y = 1.0, .id = 1},
      {.min_x = 10.0, .max_x = 12.0, .min_y = 0.0, .max_y = 2.0, .id = 2},
      {.min_x = 20.0, .max_x = 21.0, .min_y = 20.0, .max_y = 21.0, .id = 3},
      {.min_x = 21.5, .max_x = 22.5, .min_y = 20.0, .max_y = 21.0, .id = 4},
      {.min_x = 5.0, .max_x = 7.0, .min_y = 10.0, .max_y = 13.0, .id = 5},
      {.min_x = -8.0, .max_x = -5.0, .min_y = -6.0, .max_y = -4.0, .id = 6},
  });

  std::vector<TaggedBox> boxes;
  boxes.reserve(kSpecs.size());
  STRtree tree(2);
  for (const BoxSpec& spec : kSpecs) {
    boxes.push_back(
        {Envelope(spec.min_x, spec.max_x, spec.min_y, spec.max_y), spec.id});
    tree.insert(&boxes.back().env, &boxes.back());
  }
  tree.build();

  // Closest distinct pair is (3, 4) with gap 0.5 along X.
  EnvelopeItemDistance distinct_dist(/*exclude_same_item=*/true);
  auto distinct_pair = tree.nearestNeighbour(&distinct_dist);
  ASSERT_NE(distinct_pair.first, nullptr);
  ASSERT_NE(distinct_pair.second, nullptr);
  const int id_a = static_cast<const TaggedBox*>(distinct_pair.first)->id;
  const int id_b = static_cast<const TaggedBox*>(distinct_pair.second)->id;
  EXPECT_EQ(std::min(id_a, id_b), 3);
  EXPECT_EQ(std::max(id_a, id_b), 4);

  // Without excluding identical items, self-distance 0.0 hits early exit.
  EnvelopeItemDistance raw_dist(/*exclude_same_item=*/false);
  auto self_pair = tree.nearestNeighbour(&raw_dist);
  ASSERT_NE(self_pair.first, nullptr);
  ASSERT_NE(self_pair.second, nullptr);
  EXPECT_DOUBLE_EQ(
      static_cast<const TaggedBox*>(self_pair.first)
          ->env.distance(static_cast<const TaggedBox*>(self_pair.second)->env),
      0.0);
}

TEST(STRtreeTest, NearestNeighbourBetweenTwoTrees) {
  constexpr auto kLeftSpecs = std::to_array<BoxSpec>({
      {.min_x = -10.0, .max_x = -8.0, .min_y = -10.0, .max_y = -8.0, .id = 1},
      {.min_x = -4.0, .max_x = -2.0, .min_y = 0.0, .max_y = 3.0, .id = 2},
      {.min_x = -3.0, .max_x = -1.0, .min_y = 5.0, .max_y = 7.0, .id = 3},
      {.min_x = -1.5, .max_x = 0.0, .min_y = 1.0, .max_y = 2.0, .id = 4},
      {.min_x = -12.0, .max_x = -9.0, .min_y = 4.0, .max_y = 8.0, .id = 5},
  });
  constexpr auto kRightSpecs = std::to_array<BoxSpec>({
      {.min_x = 10.0, .max_x = 12.0, .min_y = 10.0, .max_y = 12.0, .id = 101},
      {.min_x = 2.0, .max_x = 4.0, .min_y = 1.0, .max_y = 2.0, .id = 102},
      {.min_x = 5.0, .max_x = 8.0, .min_y = -5.0, .max_y = -1.0, .id = 103},
      {.min_x = 6.0, .max_x = 7.0, .min_y = 4.0, .max_y = 6.0, .id = 104},
      {.min_x = 15.0, .max_x = 16.0, .min_y = -2.0, .max_y = 0.0, .id = 105},
  });

  std::vector<TaggedBox> left_boxes;
  left_boxes.reserve(kLeftSpecs.size());
  std::vector<TaggedBox> right_boxes;
  right_boxes.reserve(kRightSpecs.size());
  STRtree left_tree(2);
  STRtree right_tree(2);
  for (const BoxSpec& spec : kLeftSpecs) {
    left_boxes.push_back(
        {Envelope(spec.min_x, spec.max_x, spec.min_y, spec.max_y), spec.id});
    left_tree.insert(&left_boxes.back().env, &left_boxes.back());
  }
  for (const BoxSpec& spec : kRightSpecs) {
    right_boxes.push_back(
        {Envelope(spec.min_x, spec.max_x, spec.min_y, spec.max_y), spec.id});
    right_tree.insert(&right_boxes.back().env, &right_boxes.back());
  }
  left_tree.build();
  right_tree.build();

  EnvelopeItemDistance dist;
  auto closest = left_tree.nearestNeighbour(&right_tree, &dist);
  ASSERT_NE(closest.first, nullptr);
  ASSERT_NE(closest.second, nullptr);
  EXPECT_EQ(static_cast<const TaggedBox*>(closest.first)->id, 4);
  EXPECT_EQ(static_cast<const TaggedBox*>(closest.second)->id, 102);

  auto closest_rev = right_tree.nearestNeighbour(&left_tree, &dist);
  ASSERT_NE(closest_rev.first, nullptr);
  ASSERT_NE(closest_rev.second, nullptr);
  EXPECT_EQ(static_cast<const TaggedBox*>(closest_rev.first)->id, 102);
  EXPECT_EQ(static_cast<const TaggedBox*>(closest_rev.second)->id, 4);

  // Overlapping pair across two trees triggers zero-distance queue drain.
  TaggedBox touch_box{Envelope(-0.5, 2.5, 1.0, 2.0), 200};
  STRtree touch_tree(2);
  touch_tree.insert(&touch_box.env, &touch_box);
  TaggedBox dummy_box{Envelope(50.0, 51.0, 50.0, 51.0), 201};
  touch_tree.insert(&dummy_box.env, &dummy_box);
  touch_tree.build();

  auto touching = left_tree.nearestNeighbour(&touch_tree, &dist);
  EXPECT_EQ(static_cast<const TaggedBox*>(touching.first)->id, 4);
  EXPECT_EQ(static_cast<const TaggedBox*>(touching.second)->id, 200);
}

TEST(STRtreeTest, NearestNeighbourWithGeometryItemDistance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::vector<std::unique_ptr<Point>> pts_a;
  pts_a.push_back(factory->createPoint(CoordinateXY(0.0, 0.0)));
  pts_a.push_back(factory->createPoint(CoordinateXY(3.0, 4.0)));
  pts_a.push_back(factory->createPoint(CoordinateXY(10.0, 10.0)));
  pts_a.push_back(factory->createPoint(CoordinateXY(-5.0, 8.0)));

  STRtree tree_a(2);
  for (const auto& pt : pts_a) {
    tree_a.insert(pt->getEnvelopeInternal(), static_cast<Geometry*>(pt.get()));
  }
  tree_a.build();

  std::unique_ptr<Point> query_pt =
      factory->createPoint(CoordinateXY(3.2, 3.9));
  GeometryItemDistance geom_dist;
  const void* nearest = tree_a.nearestNeighbour(
      query_pt->getEnvelopeInternal(),
      static_cast<const Geometry*>(query_pt.get()), &geom_dist);
  EXPECT_EQ(nearest, static_cast<const Geometry*>(pts_a[1].get()));

  std::vector<std::unique_ptr<Point>> pts_b;
  pts_b.push_back(factory->createPoint(CoordinateXY(9.5, 10.5)));
  pts_b.push_back(factory->createPoint(CoordinateXY(100.0, 100.0)));
  STRtree tree_b(2);
  for (const auto& pt : pts_b) {
    tree_b.insert(pt->getEnvelopeInternal(), static_cast<Geometry*>(pt.get()));
  }
  tree_b.build();

  auto closest = tree_a.nearestNeighbour(&tree_b, &geom_dist);
  EXPECT_EQ(closest.first, static_cast<const Geometry*>(pts_a[2].get()));
  EXPECT_EQ(closest.second, static_cast<const Geometry*>(pts_b[0].get()));
}

TEST(STRtreeTest, NearestNeighbourBoundablePairDirectAndMaxDistance) {
  STRtree helper_tree(2);
  EnvelopeItemDistance dist;

  // Direct leaf-leaf BoundablePair exercises minPair == initBndPair.
  Envelope env_a(0.0, 1.0, 0.0, 1.0);
  Envelope env_b(4.0, 5.0, 0.0, 1.0);
  int tag_a = 11;
  int tag_b = 22;
  ItemBoundable leaf_a(&env_a, &tag_a);
  ItemBoundable leaf_b(&env_b, &tag_b);
  BoundablePair leaf_pair(&leaf_a, &leaf_b, &dist);

  auto found = helper_tree.nearestNeighbour(&leaf_pair);
  EXPECT_EQ(found.first, &tag_a);
  EXPECT_EQ(found.second, &tag_b);

  // Non-positive maxDistance skips the search loop, preserves initBndPair in
  // the queue drain, and throws GEOSException because minPair is null.
  EXPECT_THROW(helper_tree.nearestNeighbour(&leaf_pair, 0.0), GEOSException);
  EXPECT_THROW(helper_tree.nearestNeighbour(&leaf_pair, -2.5), GEOSException);

  // Composite BoundablePair with finite positive maxDistance:
  // Root envelopes overlap (distance 0), while actual items are distance 5.0.
  ExposedSTRtree tree1(2);
  TaggedBox item0{Envelope(0.0, 0.0, 0.0, 0.0), 1};
  TaggedBox item10{Envelope(10.0, 10.0, 0.0, 0.0), 2};
  tree1.insert(&item0.env, &item0);
  tree1.insert(&item10.env, &item10);
  tree1.build();

  ExposedSTRtree tree2(2);
  TaggedBox item5{Envelope(5.0, 5.0, 0.0, 0.0), 3};
  tree2.insert(&item5.env, &item5);
  tree2.build();

  BoundablePair composite_pair(tree1.getRoot(), tree2.getRoot(), &dist);

  // maxDistance strictly less than actual item distance prunes all children.
  EXPECT_THROW(helper_tree.nearestNeighbour(&composite_pair, 1.0),
               GEOSException);

  // maxDistance large enough succeeds.
  auto matched = helper_tree.nearestNeighbour(&composite_pair, 6.0);
  EXPECT_EQ(matched.second, &item5);
}

TEST(STRtreeTest, IsWithinDistanceBetweenTrees) {
  // Note: upstream STRtree::isWithinDistance(BoundablePair*, double) omits
  // priQ.pop() in its loop, so any call where rootDistance <= maxDistance and
  // maximumDistance > maxDistance enters an infinite allocation loop. Only
  // cases decided at the root bounds check (false when rootDist > maxDistance,
  // true when maxEnvelopeDistance <= maxDistance) terminate cleanly.
  STRtree tree1(2);
  TaggedBox box1{Envelope(0.0, 1.0, 0.0, 1.0), 1};
  TaggedBox box2{Envelope(2.0, 3.0, 0.0, 1.0), 2};
  tree1.insert(&box1.env, &box1);
  tree1.insert(&box2.env, &box2);
  tree1.build();

  STRtree far_tree(2);
  TaggedBox far1{Envelope(10.0, 11.0, 0.0, 1.0), 3};
  TaggedBox far2{Envelope(12.0, 13.0, 0.0, 1.0), 4};
  far_tree.insert(&far1.env, &far1);
  far_tree.insert(&far2.env, &far2);
  far_tree.build();

  EnvelopeItemDistance dist;

  // Root bounds distance = 7.0 ([0,3]x[0,1] vs [10,13]x[0,1]).
  EXPECT_FALSE(tree1.isWithinDistance(&far_tree, &dist, 6.99));
  EXPECT_FALSE(tree1.isWithinDistance(&tree1, &dist, -1.0));

  // Combined bounding box [0,13]x[0,1] has diagonal sqrt(13^2 + 1^2) =
  // sqrt(170). Note: Intermediate maxDistance values (e.g. 7.5) trigger an
  // upstream infinite loop/OOM bug in STRtree::isWithinDistance(BoundablePair*,
  // double) because priQ.pop() is never called before expandToQueue().
  const double max_far_diag = std::hypot(13.0, 1.0);
  EXPECT_TRUE(tree1.isWithinDistance(&far_tree, &dist, max_far_diag));
  EXPECT_TRUE(tree1.isWithinDistance(&far_tree, &dist, 20.0));

  // Single-item trees where min and max bounds distances are identical.
  STRtree p1(2);
  TaggedBox pt1{Envelope(CoordinateXY(0.0, 0.0)), 10};
  p1.insert(&pt1.env, &pt1);
  p1.build();

  STRtree p2(2);
  TaggedBox pt2{Envelope(CoordinateXY(3.0, 4.0)), 11};
  p2.insert(&pt2.env, &pt2);
  p2.build();

  EXPECT_FALSE(p1.isWithinDistance(&p2, &dist, 4.9));
  EXPECT_TRUE(p1.isWithinDistance(&p2, &dist, 5.0));
  EXPECT_TRUE(p1.isWithinDistance(&p1, &dist, 0.0));
  EXPECT_FALSE(p1.isWithinDistance(&p1, &dist, -0.001));
}

TEST(STRtreeTest, EmptyTreeBuildWithoutQueryDestroysCleanly) {
  ExposedSTRtree tree(4);
  tree.build();
  AbstractNode* root = tree.getRoot();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->getLevel(), 0);
  EXPECT_THAT(*root->getChildBoundables(), IsEmpty());
}

TEST(STRtreeTest, NullSearchEnvelopeQueryAndRemove) {
  STRtree tree(2);
  TaggedBox box{Envelope(1.0, 2.0, 1.0, 2.0), 1};
  tree.insert(&box.env, &box);
  tree.build();

  const Envelope null_env;
  ASSERT_TRUE(null_env.isNull());

  std::vector<void*> matches;
  tree.query(&null_env, matches);
  EXPECT_THAT(matches, IsEmpty());

  CollectingVisitor visitor;
  tree.query(&null_env, visitor);
  EXPECT_THAT(visitor.items(), IsEmpty());

  EXPECT_FALSE(tree.remove(&null_env, &box));
}

TEST(STRtreeTest, DefaultCapacityGridAndTieBreakingCoordinates) {
  // 25 items inserted in reverse order with default nodeCapacity = 10:
  // minLeafCount = ceil(25/10) = 3, sliceCount = ceil(sqrt(3)) = 2.
  STRtree default_tree;
  std::vector<TaggedBox> items;
  items.reserve(25);
  for (int x = 4; x >= 0; --x) {
    for (int y = 4; y >= 0; --y) {
      const double dx = static_cast<double>(x) * 2.0;
      const double dy = static_cast<double>(y) * 2.0;
      items.push_back({Envelope(dx, dx + 1.0, dy, dy + 1.0), x * 10 + y});
      default_tree.insert(&items.back().env, &items.back());
    }
  }
  default_tree.build();

  const Envelope window(2.5, 6.5, 2.5, 6.5);
  std::vector<void*> matches;
  default_tree.query(&window, matches);
  EXPECT_THAT(ExtractIds(matches),
              UnorderedElementsAreArray({11, 12, 13, 21, 22, 23, 31, 32, 33}));

  // Verify itemsTree preserves all 25 items across slices.
  std::unique_ptr<ItemsList> tree_list(default_tree.itemsTree());
  std::vector<void*> collected;
  CollectItemsList(*tree_list, collected);
  EXPECT_EQ(collected.size(), std::size_t{25});

  // Query nearest neighbor on default-capacity multi-slice tree.
  EnvelopeItemDistance dist;
  const Envelope target(4.2, 4.3, 6.2, 6.3);
  int tag = 0;
  const void* nearest = default_tree.nearestNeighbour(&target, &tag, &dist);
  ASSERT_NE(nearest, nullptr);
  EXPECT_EQ(static_cast<const TaggedBox*>(nearest)->id, 23);
}

TEST(STRtreeTest, DuplicateEnvelopesQueryAndSelectiveRemove) {
  STRtree tree(2);
  const Envelope shared_env(1.0, 3.0, 2.0, 4.0);
  TaggedBox a{shared_env, 100};
  TaggedBox b{shared_env, 200};
  TaggedBox c{shared_env, 300};
  tree.insert(&a.env, &a);
  tree.insert(&b.env, &b);
  tree.insert(&c.env, &c);

  std::vector<void*> matches;
  tree.query(&shared_env, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray({100, 200, 300}));

  EXPECT_TRUE(tree.remove(&shared_env, &b));
  matches.clear();
  tree.query(&shared_env, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray({100, 300}));

  EXPECT_TRUE(tree.remove(&shared_env, &a));
  EXPECT_TRUE(tree.remove(&shared_env, &c));

  std::unique_ptr<ItemsList> emptied_items(tree.itemsTree());
  ASSERT_NE(emptied_items, nullptr);
  EXPECT_THAT(*emptied_items, IsEmpty());
}

TEST(STRtreeTest, NearestNeighbourOverlappingEnvelopeLineStrings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Two LineStrings whose bounding boxes overlap [0,10]x[0,10], while their
  // actual geometry distances to the diagonal query line differ significantly.
  std::unique_ptr<LineString> query_line =
      factory->createLineString(CoordinateSequence{
          CoordinateXY(0.0, 10.0),
          CoordinateXY(10.0, 0.0),
      });
  std::unique_ptr<LineString> far_line =
      factory->createLineString(CoordinateSequence{
          CoordinateXY(0.0, 0.0),
          CoordinateXY(2.0, 2.0),
      });
  std::unique_ptr<LineString> close_line =
      factory->createLineString(CoordinateSequence{
          CoordinateXY(4.5, 4.5),
          CoordinateXY(4.6, 4.6),
      });
  std::unique_ptr<LineString> distant_line =
      factory->createLineString(CoordinateSequence{
          CoordinateXY(20.0, 20.0),
          CoordinateXY(22.0, 22.0),
      });

  STRtree tree(2);
  tree.insert(far_line->getEnvelopeInternal(),
              static_cast<Geometry*>(far_line.get()));
  tree.insert(close_line->getEnvelopeInternal(),
              static_cast<Geometry*>(close_line.get()));
  tree.insert(distant_line->getEnvelopeInternal(),
              static_cast<Geometry*>(distant_line.get()));
  tree.build();

  GeometryItemDistance geom_dist;
  const void* nearest = tree.nearestNeighbour(
      query_line->getEnvelopeInternal(),
      static_cast<const Geometry*>(query_line.get()), &geom_dist);
  EXPECT_EQ(nearest, static_cast<const Geometry*>(close_line.get()));

  // Self nearest neighbor with GeometryItemDistance hits zero-distance exit.
  auto self_nearest = tree.nearestNeighbour(&geom_dist);
  ASSERT_NE(self_nearest.first, nullptr);
  ASSERT_NE(self_nearest.second, nullptr);
  EXPECT_DOUBLE_EQ(
      static_cast<const Geometry*>(self_nearest.first)
          ->distance(static_cast<const Geometry*>(self_nearest.second)),
      0.0);
}

TEST(STRtreeTest, NearestNeighbourMaxDistanceExactBoundaryAndSingleItemTree) {
  ExposedSTRtree tree1(2);
  TaggedBox item0{Envelope(0.0, 0.0, 0.0, 0.0), 1};
  tree1.insert(&item0.env, &item0);
  tree1.build();

  ExposedSTRtree tree2(2);
  TaggedBox item5{Envelope(5.0, 5.0, 0.0, 0.0), 2};
  tree2.insert(&item5.env, &item5);
  tree2.build();

  EnvelopeItemDistance dist;
  // Single-item trees: exact distance 5.0 fails strict `< maxDistance` check
  // in BoundablePair::expand, while 5.001 succeeds.
  BoundablePair pair(tree1.getRoot(), tree2.getRoot(), &dist);
  EXPECT_THROW(tree1.nearestNeighbour(&pair, 5.0), GEOSException);

  auto found = tree1.nearestNeighbour(&pair, 5.001);
  EXPECT_EQ(found.first, &item0);
  EXPECT_EQ(found.second, &item5);

  // Single-item tree self nearest neighbour with finite distance.
  auto self_single = tree1.nearestNeighbour(&dist);
  EXPECT_EQ(self_single.first, &item0);
  EXPECT_EQ(self_single.second, &item0);

  // Single-item tree query with item.
  const Envelope q_env(3.0, 3.0, 4.0, 4.0);
  int q_tag = 9;
  EXPECT_EQ(tree1.nearestNeighbour(&q_env, &q_tag, &dist), &item0);
}

TEST(STRtreeTest, MultiLevelBuildDestroyAndPrunedBranchRemove) {
  std::vector<TaggedBox> boxes;
  boxes.reserve(12);
  STRtree tree(2);
  for (int i = 0; i < 12; ++i) {
    const double base = static_cast<double>(i) * 5.0;
    boxes.push_back({Envelope(base, base + 1.0, base, base + 1.0), i});
    tree.insert(&boxes.back().env, &boxes.back());
  }
  tree.build();

  // Remove half the items (including full leaf groups) so empty internal
  // nodes are pruned cleanly, then verify remaining queries and tree items.
  for (int i = 0; i < 6; ++i) {
    EXPECT_TRUE(tree.remove(&boxes[i].env, &boxes[i]));
  }
  const Envelope all(-1.0, 100.0, -1.0, 100.0);
  std::vector<void*> matches;
  tree.query(&all, matches);
  EXPECT_THAT(ExtractIds(matches),
              UnorderedElementsAreArray({6, 7, 8, 9, 10, 11}));

  std::unique_ptr<ItemsList> items(tree.itemsTree());
  std::vector<void*> collected;
  CollectItemsList(*items, collected);
  EXPECT_THAT(ExtractIds(collected),
              UnorderedElementsAreArray({6, 7, 8, 9, 10, 11}));

  // Build another multi-level tree and let it destroy without querying so
  // STRAbstractNode::~STRAbstractNode handles uncomputed root bounds (nullptr).
  STRtree unqueried_tree(2);
  for (TaggedBox& box : boxes) {
    unqueried_tree.insert(&box.env, &box);
  }
  unqueried_tree.build();
}

TEST(STRtreeTest, NearestNeighbourMultiLevelTreesAndQueueBreak) {
  std::vector<TaggedBox> left_boxes;
  left_boxes.reserve(9);
  STRtree left_tree(2);
  for (int x = 0; x < 3; ++x) {
    for (int y = 0; y < 3; ++y) {
      const double cx = static_cast<double>(x) * 4.0;
      const double cy = static_cast<double>(y) * 4.0;
      left_boxes.push_back({Envelope(cx, cx + 1.0, cy, cy + 1.0), x * 10 + y});
      left_tree.insert(&left_boxes.back().env, &left_boxes.back());
    }
  }
  left_tree.build();

  std::vector<TaggedBox> right_boxes;
  right_boxes.reserve(9);
  STRtree right_tree(2);
  for (int x = 0; x < 3; ++x) {
    for (int y = 0; y < 3; ++y) {
      const double cx = 12.0 + static_cast<double>(x) * 4.0;
      const double cy = static_cast<double>(y) * 4.0;
      right_boxes.push_back(
          {Envelope(cx, cx + 1.0, cy, cy + 1.0), 100 + x * 10 + y});
      right_tree.insert(&right_boxes.back().env, &right_boxes.back());
    }
  }
  right_tree.build();

  // Closest across the gap [9, 12] occurs for x=2 on left (cx=8..9) and
  // x=0 on right (cx=12..13) with matching y; distance = 3.0.
  EnvelopeItemDistance dist;
  auto closest = left_tree.nearestNeighbour(&right_tree, &dist);
  ASSERT_NE(closest.first, nullptr);
  ASSERT_NE(closest.second, nullptr);
  const auto* left_match = static_cast<const TaggedBox*>(closest.first);
  const auto* right_match = static_cast<const TaggedBox*>(closest.second);
  EXPECT_EQ(left_match->id / 10, 2);
  EXPECT_EQ(right_match->id / 10, 10);
  EXPECT_EQ(left_match->id % 10, right_match->id % 10);
  EXPECT_DOUBLE_EQ(left_match->env.distance(right_match->env), 3.0);
}

}  // namespace
}  // namespace geos_google
