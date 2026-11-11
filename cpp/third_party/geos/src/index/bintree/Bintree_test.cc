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

// Unit tests for third_party/geos/src/index/bintree/Bintree.cpp.
//
// google3 notes:
// - Bintree::query(double) allocates `new Interval(x, x)` internally and never
//   frees it; ScopedLeakCheckDisabler invokes weak LSan/HeapLeakChecker hooks
//   around those calls so ASan/LSan and HeapLeakChecker remain enabled overall.
// - Upstream NodeBase::addAllItems appends `newItems` into `items` instead of
//   the reverse, so Bintree::iterator() returns an empty vector.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/bintree/Bintree.cpp

#include "third_party/geos/include/geos/index/bintree/Bintree.h"

#include <memory>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/index/bintree/Interval.h"

#if defined(__has_attribute)
#if __has_attribute(weak)
extern "C" {
// Weak declarations of LSan/HeapLeakChecker runtime hooks used to scope the
// intentional upstream leak in Bintree::query(double) without extra BUILD deps.
__attribute__((weak)) void __lsan_disable();
__attribute__((weak)) void __lsan_enable();
}  // extern "C"
#endif
#endif

namespace geos_google {
namespace {

using geos::index::bintree::Bintree;
using geos::index::bintree::Interval;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::IsSupersetOf;
using ::testing::Not;
using ::testing::UnorderedElementsAre;

static_assert(!std::is_copy_constructible_v<Bintree>);
static_assert(!std::is_copy_assignable_v<Bintree>);

class ScopedLeakCheckDisabler {
 public:
  ScopedLeakCheckDisabler() {
#if defined(__has_attribute)
#if __has_attribute(weak)
    if (&__lsan_disable != nullptr) {
      __lsan_disable();
      active_ = true;
    }
#endif
#endif
  }

  ~ScopedLeakCheckDisabler() {
#if defined(__has_attribute)
#if __has_attribute(weak)
    if (active_ && &__lsan_enable != nullptr) {
      __lsan_enable();
    }
#endif
#endif
  }

  ScopedLeakCheckDisabler(const ScopedLeakCheckDisabler&) = delete;
  ScopedLeakCheckDisabler& operator=(const ScopedLeakCheckDisabler&) = delete;

 private:
  [[maybe_unused]] bool active_ = false;
};

TEST(BintreeTest, EnsureExtentCopiesNonZeroAndPadsZeroWidthIntervals) {
  const Interval non_zero(2.0, 5.0);
  std::unique_ptr<Interval> copied(Bintree::ensureExtent(&non_zero, 10.0));
  ASSERT_NE(copied, nullptr);
  EXPECT_NE(copied.get(), &non_zero);
  EXPECT_DOUBLE_EQ(copied->getMin(), 2.0);
  EXPECT_DOUBLE_EQ(copied->getMax(), 5.0);
  EXPECT_DOUBLE_EQ(copied->getWidth(), 3.0);

  const Interval tiny_non_zero(1.0, 1.0 + 1e-9);
  std::unique_ptr<Interval> copied_tiny(
      Bintree::ensureExtent(&tiny_non_zero, 10.0));
  ASSERT_NE(copied_tiny, nullptr);
  EXPECT_DOUBLE_EQ(copied_tiny->getMin(), 1.0);
  EXPECT_DOUBLE_EQ(copied_tiny->getMax(), 1.0 + 1e-9);

  // When min == max, upstream sets min -= minExtent / 2 and
  // max = min + minExtent / 2, yielding [x - minExtent / 2, x].
  const Interval zero_width(10.0, 10.0);
  std::unique_ptr<Interval> padded(Bintree::ensureExtent(&zero_width, 2.0));
  ASSERT_NE(padded, nullptr);
  EXPECT_TRUE(padded->contains(10.0));
  EXPECT_DOUBLE_EQ(padded->getMin(), 9.0);
  EXPECT_DOUBLE_EQ(padded->getMax(), 10.0);
  EXPECT_DOUBLE_EQ(padded->getWidth(), 1.0);

  const Interval neg_zero_width(-4.0, -4.0);
  std::unique_ptr<Interval> padded_neg(
      Bintree::ensureExtent(&neg_zero_width, 2.0));
  ASSERT_NE(padded_neg, nullptr);
  EXPECT_TRUE(padded_neg->contains(-4.0));
  EXPECT_DOUBLE_EQ(padded_neg->getMin(), -5.0);
  EXPECT_DOUBLE_EQ(padded_neg->getMax(), -4.0);

  const Interval origin(0.0, 0.0);
  std::unique_ptr<Interval> padded_origin(Bintree::ensureExtent(&origin, 1.0));
  ASSERT_NE(padded_origin, nullptr);
  EXPECT_TRUE(padded_origin->contains(0.0));
  EXPECT_DOUBLE_EQ(padded_origin->getMin(), -0.5);
  EXPECT_DOUBLE_EQ(padded_origin->getMax(), 0.0);

  const Interval zero_min_extent(5.0, 5.0);
  std::unique_ptr<Interval> unpadded(
      Bintree::ensureExtent(&zero_min_extent, 0.0));
  ASSERT_NE(unpadded, nullptr);
  EXPECT_DOUBLE_EQ(unpadded->getMin(), 5.0);
  EXPECT_DOUBLE_EQ(unpadded->getMax(), 5.0);
}

TEST(BintreeTest, EmptyTreeStatsAndQueries) {
  Bintree tree;
  EXPECT_EQ(tree.size(), 0);
  EXPECT_EQ(tree.depth(), 1);
  EXPECT_EQ(tree.nodeSize(), 1);

  Interval query_interval(0.0, 10.0);
  std::unique_ptr<std::vector<void*>> results(tree.query(&query_interval));
  ASSERT_NE(results, nullptr);
  EXPECT_THAT(*results, IsEmpty());

  std::vector<void*> appended;
  tree.query(&query_interval, &appended);
  EXPECT_THAT(appended, IsEmpty());

  std::unique_ptr<std::vector<void*>> scalar_results;
  {
    ScopedLeakCheckDisabler leak_disabler;
    scalar_results.reset(tree.query(0.0));
  }
  ASSERT_NE(scalar_results, nullptr);
  EXPECT_THAT(*scalar_results, IsEmpty());

  std::unique_ptr<std::vector<void*>> all_items(tree.iterator());
  ASSERT_NE(all_items, nullptr);
  EXPECT_THAT(*all_items, IsEmpty());
}

TEST(BintreeTest, InsertAndQueryAcrossSubtreesAndOriginSpanningRoot) {
  Bintree tree;
  int storage[5] = {0, 1, 2, 3, 4};
  void* const item_origin = &storage[0];
  void* const item_pos1 = &storage[1];
  void* const item_pos2 = &storage[2];
  void* const item_neg = &storage[3];
  void* const item_tiny = &storage[4];

  Interval reusable(-1.0, 2.0);
  tree.insert(&reusable, item_origin);
  EXPECT_EQ(tree.size(), 1);
  EXPECT_EQ(tree.depth(), 1);
  EXPECT_EQ(tree.nodeSize(), 1);

  // Overwrite caller interval after insert to confirm Bintree keeps its own
  // copy.
  reusable.init(4.0, 6.0);
  tree.insert(&reusable, item_pos1);

  reusable.init(20.0, 30.0);
  tree.insert(&reusable, item_pos2);

  reusable.init(-12.0, -8.0);
  tree.insert(&reusable, item_neg);

  // Width 0.1 < initial minExtent (1.0) updates internal minExtent statistics.
  reusable.init(4.1, 4.2);
  tree.insert(&reusable, item_tiny);
  reusable.init(999.0, 1000.0);

  EXPECT_EQ(tree.size(), 5);
  EXPECT_GE(tree.depth(), 3);
  EXPECT_GE(tree.nodeSize(), 4);

  // Query inside [4.0, 4.5]: overlaps [0, 16] subbranch and root, prunes
  // [16, 32] (item_pos2) and negative subnode (item_neg).
  Interval query_pos(4.0, 4.5);
  std::unique_ptr<std::vector<void*>> pos_hits(tree.query(&query_pos));
  ASSERT_NE(pos_hits, nullptr);
  EXPECT_THAT(*pos_hits, IsSupersetOf({item_origin, item_pos1, item_tiny}));
  EXPECT_THAT(*pos_hits, Not(Contains(item_neg)));
  EXPECT_THAT(*pos_hits, Not(Contains(item_pos2)));

  // Query inside [-10.0, -9.0]: overlaps negative subnode and root, prunes all
  // positive subtree items.
  Interval query_neg(-10.0, -9.0);
  std::unique_ptr<std::vector<void*>> neg_hits(tree.query(&query_neg));
  ASSERT_NE(neg_hits, nullptr);
  EXPECT_THAT(*neg_hits, IsSupersetOf({item_origin, item_neg}));
  EXPECT_THAT(*neg_hits, Not(Contains(item_pos1)));
  EXPECT_THAT(*neg_hits, Not(Contains(item_pos2)));
  EXPECT_THAT(*neg_hits, Not(Contains(item_tiny)));
}

TEST(BintreeTest, DisjointSubtreeQueriesExcludeNonOverlappingItems) {
  Bintree tree;
  int storage[3] = {1, 2, 3};
  void* const left_item = &storage[0];
  void* const mid_item = &storage[1];
  void* const right_item = &storage[2];

  Interval left(-5.0, -2.0);
  Interval mid(1.0, 3.0);
  Interval right(20.0, 24.0);
  tree.insert(&left, left_item);
  tree.insert(&mid, mid_item);
  tree.insert(&right, right_item);

  Interval far_query(100.0, 110.0);
  std::unique_ptr<std::vector<void*>> far_hits(tree.query(&far_query));
  ASSERT_NE(far_hits, nullptr);
  EXPECT_THAT(*far_hits, IsEmpty());

  Interval mid_query(1.5, 2.5);
  std::unique_ptr<std::vector<void*>> mid_hits(tree.query(&mid_query));
  ASSERT_NE(mid_hits, nullptr);
  EXPECT_THAT(*mid_hits, UnorderedElementsAre(mid_item));

  // Closed-interval endpoint touch at 3.0 still overlaps [1.0, 3.0].
  Interval touch_query(3.0, 3.0);
  std::unique_ptr<std::vector<void*>> touch_hits(tree.query(&touch_query));
  ASSERT_NE(touch_hits, nullptr);
  EXPECT_THAT(*touch_hits, UnorderedElementsAre(mid_item));
}

TEST(BintreeTest, ZeroWidthItemsAndCollectStatsBranches) {
  Bintree tree;
  int storage[4] = {1, 2, 3, 4};
  void* const narrow_item = &storage[0];
  void* const point_two = &storage[1];
  void* const wide_item = &storage[2];
  void* const point_eight = &storage[3];

  // Width 0.25 reduces minExtent from 1.0 to 0.25.
  Interval narrow(0.1, 0.35);
  // Zero-width item uses updated minExtent = 0.25.
  Interval pt2(2.0, 2.0);
  // Width 5.0 >= minExtent leaves minExtent unchanged.
  Interval wide(5.0, 10.0);
  // Zero-width item uses minExtent = 0.25.
  Interval pt8(8.0, 8.0);

  tree.insert(&narrow, narrow_item);
  tree.insert(&pt2, point_two);
  tree.insert(&wide, wide_item);
  tree.insert(&pt8, point_eight);
  EXPECT_EQ(tree.size(), 4);

  Interval q2(2.0, 2.0);
  std::unique_ptr<std::vector<void*>> hits2(tree.query(&q2));
  ASSERT_NE(hits2, nullptr);
  EXPECT_THAT(*hits2, Contains(point_two));
  EXPECT_THAT(*hits2, Not(Contains(point_eight)));

  Interval q8(8.0, 8.0);
  std::unique_ptr<std::vector<void*>> hits8(tree.query(&q8));
  ASSERT_NE(hits8, nullptr);
  EXPECT_THAT(*hits8, IsSupersetOf({wide_item, point_eight}));
  EXPECT_THAT(*hits8, Not(Contains(narrow_item)));
}

TEST(BintreeTest, FirstZeroWidthInsertPreservesInitialMinExtentUntilShrunk) {
  Bintree tree;
  int storage[3] = {1, 2, 3};
  void* const point_five = &storage[0];
  void* const narrow_item = &storage[1];
  void* const point_eight = &storage[2];

  // First item has width 0.0, so collectStats leaves minExtent at 1.0 and
  // ensureExtent pads it to [4.5, 5.0].
  Interval pt5(5.0, 5.0);
  tree.insert(&pt5, point_five);

  Interval q_in_pad(4.6, 4.7);
  std::unique_ptr<std::vector<void*>> pad_hits(tree.query(&q_in_pad));
  ASSERT_NE(pad_hits, nullptr);
  EXPECT_THAT(*pad_hits, UnorderedElementsAre(point_five));

  Interval q_before_pad(4.2, 4.3);
  std::unique_ptr<std::vector<void*>> before_hits(tree.query(&q_before_pad));
  ASSERT_NE(before_hits, nullptr);
  EXPECT_THAT(*before_hits, IsEmpty());

  // Shrink minExtent from 1.0 to 0.2, then insert a zero-width item at 8.0,
  // which gets padded to [7.9, 8.0] and stored in [7.875, 8.0].
  Interval narrow(2.0, 2.2);
  Interval pt8(8.0, 8.0);
  tree.insert(&narrow, narrow_item);
  tree.insert(&pt8, point_eight);

  Interval q_outside_shrunk_pad(7.7, 7.8);
  std::unique_ptr<std::vector<void*>> shrunk_miss(
      tree.query(&q_outside_shrunk_pad));
  ASSERT_NE(shrunk_miss, nullptr);
  EXPECT_THAT(*shrunk_miss, Not(Contains(point_eight)));

  Interval q_at_eight(7.95, 8.0);
  std::unique_ptr<std::vector<void*>> shrunk_hit(tree.query(&q_at_eight));
  ASSERT_NE(shrunk_hit, nullptr);
  EXPECT_THAT(*shrunk_hit, Contains(point_eight));
}

TEST(BintreeTest, InsertDuplicateIntervalsAndEffectivelyZeroWidthInterval) {
  Bintree tree;
  int storage[4] = {1, 2, 3, 4};
  void* const first = &storage[0];
  void* const second = &storage[1];
  void* const anchor_item = &storage[2];
  void* const ultra_narrow = &storage[3];

  Interval dup(10.0, 11.0);
  tree.insert(&dup, first);
  tree.insert(&dup, second);

  Interval anchor(1.0, 1.5);
  tree.insert(&anchor, anchor_item);
  const int depth_before = tree.depth();
  const int nodes_before = tree.nodeSize();

  // Non-zero width smaller than IntervalSize::MIN_BINARY_EXPONENT relative to
  // location: ensureExtent copies it unpadded and Root inserts into the
  // existing [1.0, 1.5] node via Node::find without subdividing further.
  Interval near_zero(1.0, 1.0 + 0x1.0p-51);
  tree.insert(&near_zero, ultra_narrow);

  EXPECT_EQ(tree.size(), 4);
  EXPECT_EQ(tree.depth(), depth_before);
  EXPECT_EQ(tree.nodeSize(), nodes_before);

  Interval q_dup(10.2, 10.8);
  std::unique_ptr<std::vector<void*>> dup_hits(tree.query(&q_dup));
  ASSERT_NE(dup_hits, nullptr);
  EXPECT_THAT(*dup_hits, UnorderedElementsAre(first, second));

  Interval q_narrow(1.0, 1.0 + 0x1.0p-50);
  std::unique_ptr<std::vector<void*>> narrow_hits(tree.query(&q_narrow));
  ASSERT_NE(narrow_hits, nullptr);
  EXPECT_THAT(*narrow_hits, UnorderedElementsAre(anchor_item, ultra_narrow));
}

TEST(BintreeTest, QueryIntoExistingVectorAppendsWithoutClearing) {
  Bintree tree;
  int storage[3] = {99, 1, 2};
  void* const sentinel = &storage[0];
  void* const item_a = &storage[1];
  void* const item_b = &storage[2];

  Interval iv_a(1.0, 3.0);
  Interval iv_b(20.0, 25.0);
  tree.insert(&iv_a, item_a);
  tree.insert(&iv_b, item_b);

  std::vector<void*> results = {sentinel};
  Interval q(1.5, 2.5);
  tree.query(&q, &results);
  EXPECT_THAT(results, ElementsAre(sentinel, item_a));
}

TEST(BintreeTest, QueryByScalarValueFindsContainingIntervals) {
  Bintree tree;
  int storage[4] = {1, 2, 3, 4};
  void* const item_a = &storage[0];
  void* const item_b = &storage[1];
  void* const item_neg = &storage[2];
  void* const item_far = &storage[3];

  Interval iv_a(1.0, 3.0);
  Interval iv_b(2.5, 5.0);
  Interval iv_neg(-8.0, -4.0);
  Interval iv_far(20.0, 25.0);
  tree.insert(&iv_a, item_a);
  tree.insert(&iv_b, item_b);
  tree.insert(&iv_neg, item_neg);
  tree.insert(&iv_far, item_far);

  std::unique_ptr<std::vector<void*>> hits_mid;
  std::unique_ptr<std::vector<void*>> hits_edge;
  std::unique_ptr<std::vector<void*>> hits_neg;
  std::unique_ptr<std::vector<void*>> hits_empty;
  {
    ScopedLeakCheckDisabler leak_disabler;
    hits_mid.reset(tree.query(2.75));
    hits_edge.reset(tree.query(1.0));
    hits_neg.reset(tree.query(-6.0));
    hits_empty.reset(tree.query(100.0));
  }

  ASSERT_NE(hits_mid, nullptr);
  EXPECT_THAT(*hits_mid, IsSupersetOf({item_a, item_b}));
  EXPECT_THAT(*hits_mid, Not(Contains(item_neg)));
  EXPECT_THAT(*hits_mid, Not(Contains(item_far)));

  ASSERT_NE(hits_edge, nullptr);
  EXPECT_THAT(*hits_edge, Contains(item_a));
  EXPECT_THAT(*hits_edge, Not(Contains(item_neg)));
  EXPECT_THAT(*hits_edge, Not(Contains(item_far)));

  ASSERT_NE(hits_neg, nullptr);
  EXPECT_THAT(*hits_neg, UnorderedElementsAre(item_neg));

  ASSERT_NE(hits_empty, nullptr);
  EXPECT_THAT(*hits_empty, IsEmpty());
}

TEST(BintreeTest, IteratorLeavesTreeIntact) {
  Bintree tree;
  int storage[3] = {1, 2, 3};
  void* const item_origin = &storage[0];
  void* const item_neg = &storage[1];
  void* const item_pos = &storage[2];
  Interval iv_origin(-1.0, 1.0);
  Interval iv_neg(-5.0, -2.0);
  Interval iv_pos(10.0, 12.0);
  tree.insert(&iv_origin, item_origin);
  tree.insert(&iv_neg, item_neg);
  tree.insert(&iv_pos, item_pos);

  // Upstream NodeBase::addAllItems swaps source and destination vectors, so
  // iterator() returns an empty vector while keeping tree contents intact.
  std::unique_ptr<std::vector<void*>> iter_items(tree.iterator());
  ASSERT_NE(iter_items, nullptr);
  EXPECT_THAT(*iter_items, IsEmpty());
  EXPECT_EQ(tree.size(), 3);

  Interval q(10.0, 12.0);
  std::unique_ptr<std::vector<void*>> q_items(tree.query(&q));
  ASSERT_NE(q_items, nullptr);
  EXPECT_THAT(*q_items, UnorderedElementsAre(item_origin, item_pos));
}

}  // namespace
}  // namespace geos_google
