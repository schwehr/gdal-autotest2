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

// Unit tests for third_party/geos/src/index/bintree/Root.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/bintree/Root.cpp

#include "third_party/geos/include/geos/index/bintree/Root.h"

#include <cmath>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/index/bintree/Interval.h"
#include "third_party/geos/include/geos/index/bintree/NodeBase.h"
#include "third_party/geos/include/geos/index/quadtree/IntervalSize.h"

namespace geos_google {
namespace {

using ::geos::index::bintree::Interval;
using ::geos::index::bintree::NodeBase;
using ::geos::index::bintree::Root;
using ::geos::index::quadtree::IntervalSize;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

TEST(RootTest, EmptyRootHasDefaultMetricsAndNoItems) {
  std::unique_ptr<NodeBase> base = std::make_unique<Root>();
  EXPECT_EQ(base->size(), 0);
  EXPECT_EQ(base->depth(), 1);
  EXPECT_EQ(base->nodeSize(), 1);
  ASSERT_NE(base->getItems(), nullptr);
  EXPECT_THAT(*base->getItems(), IsEmpty());

  Interval query(-10.0, 10.0);
  std::vector<void*> found;
  base->addAllItemsFromOverlapping(&query, &found);
  EXPECT_THAT(found, IsEmpty());
}

TEST(RootTest, InsertSpanningOriginStoresItemsDirectlyAtRoot) {
  int item_sym = 1;
  int item_left_heavy = 2;
  int item_right_heavy = 3;
  Interval iv_sym(-1.0, 1.0);
  Interval iv_left_heavy(-5.0, 0.25);
  Interval iv_right_heavy(-0.1, 10.0);

  Root root;
  root.insert(&iv_sym, &item_sym);
  root.insert(&iv_left_heavy, &item_left_heavy);
  root.insert(&iv_right_heavy, &item_right_heavy);

  EXPECT_EQ(root.size(), 3);
  EXPECT_EQ(root.depth(), 1);
  EXPECT_EQ(root.nodeSize(), 1);
  ASSERT_NE(root.getItems(), nullptr);
  EXPECT_THAT(
      *root.getItems(),
      UnorderedElementsAre(&item_sym, &item_left_heavy, &item_right_heavy));

  // Root::isSearchMatch always returns true, so root-level items are always
  // included as overlap candidates regardless of the query interval.
  Interval far_pos_query(100.0, 200.0);
  std::vector<void*> pos_found;
  root.addAllItemsFromOverlapping(&far_pos_query, &pos_found);
  EXPECT_THAT(pos_found, UnorderedElementsAre(&item_sym, &item_left_heavy,
                                              &item_right_heavy));

  Interval far_neg_query(-200.0, -100.0);
  std::vector<void*> neg_found;
  root.addAllItemsFromOverlapping(&far_neg_query, &neg_found);
  EXPECT_THAT(neg_found, UnorderedElementsAre(&item_sym, &item_left_heavy,
                                              &item_right_heavy));
}

TEST(RootTest, InsertPositiveSideCreatesContainsAndExpandsSubtree) {
  int item_initial = 10;
  int item_nested = 11;
  int item_expanded = 12;
  int item_wider = 13;

  Interval iv_initial(1.0, 2.0);
  Interval iv_nested(1.25, 1.75);
  Interval iv_expanded(4.0, 8.0);
  Interval iv_wider(0.5, 32.0);

  Root root;
  // Subnode[1] is initially null; inserts into newly created [0, 2] subtree.
  root.insert(&iv_initial, &item_initial);
  EXPECT_EQ(root.size(), 1);
  EXPECT_GE(root.depth(), 2);
  EXPECT_THAT(*root.getItems(), IsEmpty());

  // Existing subnode[1] ([0, 2]) already contains [1.25, 1.75]; no expansion.
  root.insert(&iv_nested, &item_nested);
  EXPECT_EQ(root.size(), 2);

  // [4, 8] lies outside [0, 2], triggering upward tree expansion.
  root.insert(&iv_expanded, &item_expanded);
  EXPECT_EQ(root.size(), 3);

  // [0.5, 32] triggers another upward expansion of subnode[1].
  root.insert(&iv_wider, &item_wider);
  EXPECT_EQ(root.size(), 4);
  EXPECT_THAT(*root.getItems(), IsEmpty());

  Interval all_query(0.0, 64.0);
  std::vector<void*> all_items;
  root.addAllItemsFromOverlapping(&all_query, &all_items);
  EXPECT_THAT(all_items, UnorderedElementsAre(&item_initial, &item_nested,
                                              &item_expanded, &item_wider));

  // Query overlapping only the wider interval and [4, 8].
  Interval mid_query(5.0, 6.0);
  std::vector<void*> mid_found;
  root.addAllItemsFromOverlapping(&mid_query, &mid_found);
  EXPECT_THAT(mid_found, UnorderedElementsAre(&item_expanded, &item_wider));

  // Query overlapping the initial/nested interval and the wider container.
  Interval inner_query(1.3, 1.4);
  std::vector<void*> inner_found;
  root.addAllItemsFromOverlapping(&inner_query, &inner_found);
  EXPECT_THAT(inner_found,
              UnorderedElementsAre(&item_initial, &item_nested, &item_wider));

  // Query outside the entire positive subtree returns nothing.
  Interval neg_query(-10.0, -1.0);
  std::vector<void*> neg_found;
  root.addAllItemsFromOverlapping(&neg_query, &neg_found);
  EXPECT_THAT(neg_found, IsEmpty());
}

TEST(RootTest, InsertNegativeSideAndOriginBoundaries) {
  int item_neg1 = 20;
  int item_neg_nested = 21;
  int item_neg_expand = 22;
  int item_touch_pos = 23;
  int item_touch_neg = 24;
  int item_at_origin = 25;

  Interval iv_neg1(-2.0, -1.0);
  Interval iv_neg_nested(-1.75, -1.25);
  Interval iv_neg_expand(-8.0, -4.0);
  Interval iv_touch_pos(0.0, 2.0);
  Interval iv_touch_neg(-2.0, 0.0);
  Interval iv_at_origin(0.0, 0.0);

  Root root;
  root.insert(&iv_neg1, &item_neg1);
  root.insert(&iv_neg_nested, &item_neg_nested);
  root.insert(&iv_neg_expand, &item_neg_expand);
  root.insert(&iv_touch_pos, &item_touch_pos);
  root.insert(&iv_touch_neg, &item_touch_neg);
  root.insert(&iv_at_origin, &item_at_origin);

  EXPECT_EQ(root.size(), 6);
  // Intervals ending or starting at 0.0 belong to subnode 0 or 1, not Root.
  EXPECT_THAT(*root.getItems(), IsEmpty());

  Interval all_query(-16.0, 4.0);
  std::vector<void*> all_items;
  root.addAllItemsFromOverlapping(&all_query, &all_items);
  EXPECT_THAT(
      all_items,
      UnorderedElementsAre(&item_neg1, &item_neg_nested, &item_neg_expand,
                           &item_touch_pos, &item_touch_neg, &item_at_origin));

  Interval pos_query(1.5, 1.5);
  std::vector<void*> pos_found;
  root.addAllItemsFromOverlapping(&pos_query, &pos_found);
  EXPECT_THAT(pos_found, UnorderedElementsAre(&item_touch_pos));

  Interval far_neg_query(-7.0, -5.0);
  std::vector<void*> far_neg_found;
  root.addAllItemsFromOverlapping(&far_neg_query, &far_neg_found);
  EXPECT_THAT(far_neg_found, UnorderedElementsAre(&item_neg_expand));

  Interval origin_query(0.0, 0.0);
  std::vector<void*> origin_found;
  root.addAllItemsFromOverlapping(&origin_query, &origin_found);
  EXPECT_THAT(
      origin_found,
      UnorderedElementsAre(&item_touch_pos, &item_touch_neg, &item_at_origin));
}

TEST(RootTest, InsertZeroWidthAndEffectivelyZeroWidthIntervals) {
  int item_box = 30;
  int item_pt_inside = 31;
  int item_pt_same = 32;
  int item_pt_neg = 33;
  int item_tiny_pos = 34;

  // Pick a positive width that satisfies IntervalSize::isZeroWidth without
  // collapsing to zero in IEEE-754 double precision.
  constexpr double kTinyBase = 1024.0;
  const double tiny_max = kTinyBase + std::ldexp(1.0, -41);
  ASSERT_GT(tiny_max, kTinyBase);
  ASSERT_TRUE(IntervalSize::isZeroWidth(kTinyBase, tiny_max));

  Interval iv_box(1.0, 2.0);
  Interval iv_pt_inside(1.5, 1.5);
  Interval iv_pt_same(1.5, 1.5);
  Interval iv_pt_neg(-3.25, -3.25);
  Interval iv_tiny_pos(kTinyBase, tiny_max);

  Root root;
  root.insert(&iv_box, &item_box);
  const int nodes_before_zero_width = root.nodeSize();

  // Zero-width inserts use tree->find(...) rather than creating deeper nodes.
  root.insert(&iv_pt_inside, &item_pt_inside);
  root.insert(&iv_pt_same, &item_pt_same);
  EXPECT_EQ(root.nodeSize(), nodes_before_zero_width);

  root.insert(&iv_pt_neg, &item_pt_neg);

  Interval query_pt(1.5, 1.5);
  std::vector<void*> found_pt_before_expand;
  root.addAllItemsFromOverlapping(&query_pt, &found_pt_before_expand);
  EXPECT_THAT(found_pt_before_expand,
              UnorderedElementsAre(&item_box, &item_pt_inside, &item_pt_same));

  // Inserting [1024, tiny_max] expands subnode[1] to [0, 2048] and stores
  // item_tiny_pos on [0, 2048] (because subnode[1] = [1024, 2048] does not yet
  // exist for find() to descend into).
  root.insert(&iv_tiny_pos, &item_tiny_pos);

  EXPECT_EQ(root.size(), 5);
  Interval all_query(-10.0, 2048.0);
  std::vector<void*> all_items;
  root.addAllItemsFromOverlapping(&all_query, &all_items);
  EXPECT_THAT(all_items,
              UnorderedElementsAre(&item_box, &item_pt_inside, &item_pt_same,
                                   &item_pt_neg, &item_tiny_pos));

  std::vector<void*> found_pt_after_expand;
  root.addAllItemsFromOverlapping(&query_pt, &found_pt_after_expand);
  EXPECT_THAT(found_pt_after_expand,
              UnorderedElementsAre(&item_box, &item_pt_inside, &item_pt_same,
                                   &item_tiny_pos));

  Interval query_tiny(kTinyBase, tiny_max);
  std::vector<void*> found_tiny;
  root.addAllItemsFromOverlapping(&query_tiny, &found_tiny);
  EXPECT_THAT(found_tiny, UnorderedElementsAre(&item_tiny_pos));

  Interval query_neg_pt(-3.25, -3.25);
  std::vector<void*> found_neg_pt;
  root.addAllItemsFromOverlapping(&query_neg_pt, &found_neg_pt);
  EXPECT_THAT(found_neg_pt, UnorderedElementsAre(&item_pt_neg));
}

TEST(RootTest, MixedRootAndBothSubtreesWithZeroWidthFirst) {
  int item_pos_pt = 40;
  int item_pos_expand = 41;
  int item_pos_nested = 42;
  int item_origin = 43;
  int item_neg_pt = 44;
  int item_neg_expand = 45;
  int item_neg_nested = 46;

  Interval iv_pos_pt(3.5, 3.5);
  Interval iv_pos_expand(2.0, 5.0);
  Interval iv_pos_nested(2.5, 2.75);
  Interval iv_origin(-1.0, 1.0);
  Interval iv_neg_pt(-3.5, -3.5);
  Interval iv_neg_expand(-6.0, -2.0);
  Interval iv_neg_nested(-5.5, -5.0);

  Root root;
  root.insert(&iv_pos_pt, &item_pos_pt);
  root.insert(&iv_pos_expand, &item_pos_expand);
  root.insert(&iv_pos_nested, &item_pos_nested);
  root.insert(&iv_origin, &item_origin);
  root.insert(&iv_neg_pt, &item_neg_pt);
  root.insert(&iv_neg_expand, &item_neg_expand);
  root.insert(&iv_neg_nested, &item_neg_nested);

  EXPECT_EQ(root.size(), 7);
  EXPECT_THAT(*root.getItems(), UnorderedElementsAre(&item_origin));

  Interval all_query(-8.0, 8.0);
  std::vector<void*> all_items;
  root.addAllItemsFromOverlapping(&all_query, &all_items);
  EXPECT_THAT(all_items,
              UnorderedElementsAre(&item_pos_pt, &item_pos_expand,
                                   &item_pos_nested, &item_origin, &item_neg_pt,
                                   &item_neg_expand, &item_neg_nested));

  // Root-stored item always matches, plus overlapping positive nested items.
  Interval pos_nested_query(2.6, 2.7);
  std::vector<void*> pos_nested_found;
  root.addAllItemsFromOverlapping(&pos_nested_query, &pos_nested_found);
  EXPECT_THAT(
      pos_nested_found,
      UnorderedElementsAre(&item_origin, &item_pos_expand, &item_pos_nested));

  Interval neg_nested_query(-5.25, -5.1);
  std::vector<void*> neg_nested_found;
  root.addAllItemsFromOverlapping(&neg_nested_query, &neg_nested_found);
  EXPECT_THAT(
      neg_nested_found,
      UnorderedElementsAre(&item_origin, &item_neg_expand, &item_neg_nested));
}

}  // namespace
}  // namespace geos_google
