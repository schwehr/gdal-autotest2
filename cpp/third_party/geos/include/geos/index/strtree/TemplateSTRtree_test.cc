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

#include "third_party/geos/include/geos/index/strtree/TemplateSTRtree.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <set>
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
#include "third_party/geos/include/geos/index/SpatialIndex.h"
#include "third_party/geos/include/geos/index/strtree/Interval.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Envelope;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryFactory;
using ::geos::index::ItemVisitor;
using ::geos::index::SpatialIndex;
using ::geos::index::strtree::EnvelopeTraits;
using ::geos::index::strtree::Interval;
using ::geos::index::strtree::IntervalTraits;
using ::geos::index::strtree::TemplateSTRtree;
using ::geos::index::strtree::TemplateSTRtreeImpl;
using ::geos::util::IllegalArgumentException;
using ::testing::IsEmpty;
using ::testing::Pair;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedElementsAreArray;

struct PointDistance {
  double operator()(const Geometry* a, const Geometry* b) const {
    return a->distance(b);
  }
};

struct DistinctPointDistance {
  double operator()(const Geometry* a, const Geometry* b) const {
    if (a == b) {
      return geos::DoubleMax;
    }
    return a->distance(b);
  }
};

struct TrackingDistance {
  int calls = 0;

  double operator()(const Geometry* a, const Geometry* b) {
    ++calls;
    return a->distance(b);
  }
};

class CollectingItemVisitor final : public ItemVisitor {
 public:
  void visitItem(void* item) override { items.push_back(item); }

  std::vector<void*> items;
};

struct GeomHandle {
  const Geometry* geom = nullptr;
  int id = 0;

  const GeomHandle* operator->() const { return this; }

  const Envelope* getEnvelopeInternal() const {
    return geom->getEnvelopeInternal();
  }

  bool operator==(const GeomHandle& other) const = default;
};

class TemplateSTRtreeTest : public ::testing::Test {
 protected:
  TemplateSTRtreeTest() : factory_(GeometryFactory::create()) {}

  Geometry* MakePoint(double x, double y) {
    owned_geoms_.push_back(factory_->createPoint(CoordinateXY(x, y)));
    return owned_geoms_.back().get();
  }

  Geometry* MakeEmptyPoint() {
    owned_geoms_.push_back(factory_->createPoint());
    return owned_geoms_.back().get();
  }

  Geometry* MakeBox(double min_x, double max_x, double min_y, double max_y) {
    Envelope env(min_x, max_x, min_y, max_y);
    owned_geoms_.push_back(factory_->toGeometry(&env));
    return owned_geoms_.back().get();
  }

  Geometry* MakeSegment(double x0, double y0, double x1, double y1) {
    auto seq = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXY>{{x0, y0}, {x1, y1}});
    owned_geoms_.push_back(factory_->createLineString(std::move(seq)));
    return owned_geoms_.back().get();
  }

  GeometryFactory::Ptr factory_;
  std::vector<std::unique_ptr<Geometry>> owned_geoms_;
};

TEST_F(TemplateSTRtreeTest, ConstructionAndValidation) {
  TemplateSTRtree<Geometry*> default_tree;
  EXPECT_FALSE(default_tree.built());
  EXPECT_EQ(default_tree.getRoot(), nullptr);
  EXPECT_FALSE(default_tree.built());

  TemplateSTRtree<Geometry*> min_capacity_tree(2);
  EXPECT_FALSE(min_capacity_tree.built());

  TemplateSTRtree<Geometry*> preallocated_zero(4, 0);
  EXPECT_EQ(preallocated_zero.getRoot(), nullptr);

  TemplateSTRtree<Geometry*> preallocated_one(4, 1);
  EXPECT_EQ(preallocated_one.getRoot(), nullptr);

  TemplateSTRtree<Geometry*> preallocated_many(3, 50);
  for (int i = 0; i < 50; ++i) {
    preallocated_many.insert(MakePoint(static_cast<double>(i), 0.0));
  }
  preallocated_many.build();
  EXPECT_TRUE(preallocated_many.built());
  EXPECT_NE(preallocated_many.getRoot(), nullptr);

  std::vector<Geometry*> results;
  preallocated_many.query(Envelope(9.5, 12.5, -1.0, 1.0), results);
  EXPECT_THAT(results, SizeIs(3));

  EXPECT_THROW(([&] {
                 TemplateSTRtree<Geometry*> t(0);
                 (void)t;
               }()),
               IllegalArgumentException);
  EXPECT_THROW(([&] {
                 TemplateSTRtree<Geometry*> t(1);
                 (void)t;
               }()),
               IllegalArgumentException);
  // Note: the 2-arg constructor invokes treeSize(itemCapacity) before
  // validateConstruction(), so itemCapacity > 1 with nodeCapacity < 2 would
  // divide by zero (capacity == 0) or loop infinitely (capacity == 1).
  // Testing itemCapacity <= 1 exercises the 2-arg validation throw cleanly.
  EXPECT_THROW(([&] {
                 TemplateSTRtree<Geometry*> t(0, 0);
                 (void)t;
               }()),
               IllegalArgumentException);
  EXPECT_THROW(([&] {
                 TemplateSTRtree<Geometry*> t(0, 1);
                 (void)t;
               }()),
               IllegalArgumentException);
  EXPECT_THROW(([&] {
                 TemplateSTRtree<Geometry*> t(1, 0);
                 (void)t;
               }()),
               IllegalArgumentException);
  EXPECT_THROW(([&] {
                 TemplateSTRtree<Geometry*> t(1, 1);
                 (void)t;
               }()),
               IllegalArgumentException);
}

TEST_F(TemplateSTRtreeTest, CopyConstructionAndAssignment) {
  // Test copying and assigning unbuilt trees using TemplateSTRtreeImpl directly
  // and primary template value type.
  TemplateSTRtreeImpl<Geometry*, EnvelopeTraits> empty_src(3);
  TemplateSTRtreeImpl<Geometry*, EnvelopeTraits> empty_copy(empty_src);
  EXPECT_FALSE(empty_copy.built());
  EXPECT_EQ(empty_copy.getRoot(), nullptr);

  std::vector<Geometry*> expected;
  auto src =
      std::make_unique<TemplateSTRtreeImpl<Geometry*, EnvelopeTraits>>(3);
  for (int i = 0; i < 12; ++i) {
    Geometry* p = MakePoint(static_cast<double>(i), static_cast<double>(i % 4));
    expected.push_back(p);
    src->insert(p);
  }
  EXPECT_FALSE(src->built());

  TemplateSTRtreeImpl<Geometry*, EnvelopeTraits> copy_ctor(*src);
  TemplateSTRtreeImpl<Geometry*, EnvelopeTraits> copy_assign(5);
  copy_assign.insert(MakePoint(999.0, 999.0));
  copy_assign = *src;

  // Destroy source before building and querying copies to confirm independence.
  src.reset();

  EXPECT_FALSE(copy_ctor.built());
  EXPECT_FALSE(copy_assign.built());

  std::vector<Geometry*> ctor_results;
  copy_ctor.query(Envelope(-1.0, 20.0, -1.0, 10.0), ctor_results);
  EXPECT_TRUE(copy_ctor.built());
  EXPECT_THAT(ctor_results, UnorderedElementsAreArray(expected));

  std::vector<Geometry*> assign_results;
  copy_assign.query(Envelope(-1.0, 20.0, -1.0, 10.0), assign_results);
  EXPECT_TRUE(copy_assign.built());
  EXPECT_THAT(assign_results, UnorderedElementsAreArray(expected));

  // Assigning an empty tree replaces previous contents.
  copy_assign = empty_src;
  EXPECT_FALSE(copy_assign.built());
  EXPECT_EQ(copy_assign.getRoot(), nullptr);
}

TEST_F(TemplateSTRtreeTest, InsertOverloadsAndNullBounds) {
  TemplateSTRtree<Geometry*> tree(2);

  Geometry* p1 = MakePoint(1.0, 1.0);
  Geometry* p2 = MakePoint(2.0, 2.0);
  Geometry* p3 = MakePoint(3.0, 3.0);
  Geometry* empty_p = MakeEmptyPoint();

  // Lvalue and rvalue insert(item) overloads.
  tree.insert(p1);
  tree.insert(MakePoint(1.5, 1.5));

  // Lvalue and rvalue insert(bounds, item) overloads.
  Envelope env2(1.9, 2.1, 1.9, 2.1);
  tree.insert(env2, p2);
  tree.insert(Envelope(2.9, 3.1, 2.9, 3.1), MakePoint(3.0, 3.0));

  // Null envelopes and empty geometries must be ignored by all overloads.
  Envelope null_env;
  tree.insert(empty_p);
  tree.insert(MakeEmptyPoint());
  tree.insert(null_env, p3);
  tree.insert(null_env, MakePoint(5.0, 5.0));

  std::vector<Geometry*> all_items;
  for (Geometry* g : tree.items()) {
    all_items.push_back(g);
  }
  EXPECT_THAT(all_items, SizeIs(4));
  EXPECT_EQ(std::count(all_items.begin(), all_items.end(), p1), 1);
  EXPECT_EQ(std::count(all_items.begin(), all_items.end(), p2), 1);
  EXPECT_EQ(std::count(all_items.begin(), all_items.end(), empty_p), 0);
  EXPECT_EQ(std::count(all_items.begin(), all_items.end(), p3), 0);
}

TEST_F(TemplateSTRtreeTest, EmptyTreeOperations) {
  TemplateSTRtree<Geometry*> empty_tree(4);
  TemplateSTRtree<Geometry*> other_tree(4);
  Geometry* other_pt = MakePoint(10.0, 10.0);
  other_tree.insert(other_pt);

  empty_tree.build();
  EXPECT_FALSE(empty_tree.built());
  EXPECT_EQ(empty_tree.getRoot(), nullptr);

  Envelope query_env(-100.0, 100.0, -100.0, 100.0);
  std::vector<Geometry*> vec_results;
  empty_tree.query(query_env, vec_results);
  EXPECT_THAT(vec_results, IsEmpty());

  int visit_count = 0;
  empty_tree.query(query_env, [&](const Geometry*) { ++visit_count; });
  EXPECT_EQ(visit_count, 0);

  empty_tree.queryPairs(
      [&](const Geometry*, const Geometry*) { ++visit_count; });
  EXPECT_EQ(visit_count, 0);

  empty_tree.iterate([&](const Geometry*) { ++visit_count; });
  EXPECT_EQ(visit_count, 0);
  EXPECT_FALSE(empty_tree.built());

  auto items = empty_tree.items();
  EXPECT_TRUE(items.begin() == items.end());
  EXPECT_FALSE(items.begin() != items.end());

  EXPECT_FALSE(empty_tree.remove(query_env, other_pt));

  PointDistance dist;
  // Note: 0-arg TemplateSTRtreeImpl::nearestNeighbour<ItemDistance>() has an
  // upstream template forwarding bug (calls nearestNeighbour(*this) without
  // explicit <ItemDistance>), so exercise the 1-arg and 2-arg overloads.
  EXPECT_THAT(empty_tree.nearestNeighbour(dist), Pair(nullptr, nullptr));
  EXPECT_THAT(empty_tree.nearestNeighbour<PointDistance>(empty_tree),
              Pair(nullptr, nullptr));
  EXPECT_THAT(empty_tree.nearestNeighbour(other_tree, dist),
              Pair(nullptr, nullptr));
  EXPECT_THAT(other_tree.nearestNeighbour(empty_tree, dist),
              Pair(nullptr, nullptr));
  EXPECT_THAT(empty_tree.nearestNeighbour<PointDistance>(other_tree),
              Pair(nullptr, nullptr));
  EXPECT_THAT(other_tree.nearestNeighbour<PointDistance>(empty_tree),
              Pair(nullptr, nullptr));
  EXPECT_EQ(empty_tree.nearestNeighbour(*other_pt->getEnvelopeInternal(),
                                        other_pt, dist),
            nullptr);
  EXPECT_EQ(empty_tree.nearestNeighbour<PointDistance>(
                *other_pt->getEnvelopeInternal(), other_pt),
            nullptr);

  EXPECT_FALSE(empty_tree.isWithinDistance<PointDistance>(other_tree, 1000.0));
  EXPECT_FALSE(other_tree.isWithinDistance<PointDistance>(empty_tree, 1000.0));
}

TEST_F(TemplateSTRtreeTest, SingleItemLeafRootTree) {
  TemplateSTRtree<Geometry*> tree(4);
  Geometry* pt = MakePoint(5.0, 5.0);
  Geometry* other_pt = MakePoint(6.0, 6.0);
  tree.insert(pt);

  int iterate_count = 0;
  tree.iterate([&](const Geometry* g) {
    EXPECT_EQ(g, pt);
    ++iterate_count;
  });
  EXPECT_EQ(iterate_count, 1);
  EXPECT_FALSE(tree.built());

  // Disjoint query builds the tree without visiting the leaf.
  std::vector<Geometry*> results;
  tree.query(Envelope(0.0, 1.0, 0.0, 1.0), results);
  EXPECT_THAT(results, IsEmpty());
  EXPECT_TRUE(tree.built());
  ASSERT_NE(tree.getRoot(), nullptr);
  EXPECT_TRUE(tree.getRoot()->isLeaf());

  // Intersecting query with vector and all four visitLeaf visitor signatures.
  Envelope hit_env(4.0, 6.0, 4.0, 6.0);
  tree.query(hit_env, results);
  EXPECT_THAT(results, UnorderedElementsAre(pt));

  int v1 = 0;
  tree.query(hit_env, [&](const Geometry* g) {
    EXPECT_EQ(g, pt);
    ++v1;
  });
  EXPECT_EQ(v1, 1);

  int v2 = 0;
  tree.query(hit_env, [&](const Envelope& env, const Geometry* g) {
    EXPECT_EQ(env, *pt->getEnvelopeInternal());
    EXPECT_EQ(g, pt);
    ++v2;
  });
  EXPECT_EQ(v2, 1);

  int v3 = 0;
  tree.query(hit_env, [&](const Geometry* g) -> bool {
    EXPECT_EQ(g, pt);
    ++v3;
    return false;
  });
  EXPECT_EQ(v3, 1);

  int v4 = 0;
  tree.query(hit_env, [&](const Envelope& env, const Geometry* g) -> bool {
    EXPECT_EQ(env, *pt->getEnvelopeInternal());
    EXPECT_EQ(g, pt);
    ++v4;
    return true;
  });
  EXPECT_EQ(v4, 1);

  // queryPairs with < 2 items visits 0 pairs.
  int pair_count = 0;
  tree.queryPairs([&](const Geometry*, const Geometry*) { ++pair_count; });
  EXPECT_EQ(pair_count, 0);

  // Nearest neighbour with 1 item.
  PointDistance dist;
  EXPECT_THAT(tree.nearestNeighbour(dist), Pair(pt, pt));
  EXPECT_THAT(tree.nearestNeighbour<PointDistance>(tree), Pair(pt, pt));
  EXPECT_EQ(
      tree.nearestNeighbour(*other_pt->getEnvelopeInternal(), other_pt, dist),
      pt);
  EXPECT_EQ(tree.nearestNeighbour<PointDistance>(
                *other_pt->getEnvelopeInternal(), other_pt),
            pt);

  // Remove mismatch, match, and duplicate remove on leaf root.
  EXPECT_FALSE(tree.remove(hit_env, other_pt));
  EXPECT_TRUE(tree.remove(hit_env, pt));
  EXPECT_FALSE(tree.remove(hit_env, pt));

  // Verify deleted leaf is skipped by items() and iterate().
  EXPECT_TRUE(tree.items().begin() == tree.items().end());
  int after_delete_count = 0;
  tree.iterate([&](const Geometry*) { ++after_delete_count; });
  EXPECT_EQ(after_delete_count, 0);
}

TEST_F(TemplateSTRtreeTest, MultiLevelQueryAndVisitorSignatures) {
  TemplateSTRtree<Geometry*> tree(2);
  std::vector<Geometry*> all_pts;
  std::vector<Geometry*> center_pts;

  for (int x = 0; x < 5; ++x) {
    for (int y = 0; y < 5; ++y) {
      Geometry* p = MakePoint(static_cast<double>(x), static_cast<double>(y));
      all_pts.push_back(p);
      if (x >= 1 && x <= 2 && y >= 1 && y <= 2) {
        center_pts.push_back(p);
      }
      tree.insert(p);
    }
  }

  // iterate() does not build the tree.
  std::vector<Geometry*> prebuild_items;
  tree.iterate([&](Geometry* g) { prebuild_items.push_back(g); });
  EXPECT_FALSE(tree.built());
  EXPECT_THAT(prebuild_items, UnorderedElementsAreArray(all_pts));

  tree.build();
  EXPECT_TRUE(tree.built());
  // Calling build() again is a no-op.
  tree.build();
  ASSERT_NE(tree.getRoot(), nullptr);
  EXPECT_FALSE(tree.getRoot()->isLeaf());

  // Disjoint query envelope against root.
  std::vector<Geometry*> disjoint_results;
  tree.query(Envelope(100.0, 105.0, 100.0, 105.0), disjoint_results);
  EXPECT_THAT(disjoint_results, IsEmpty());

  // Sub-region query with pruning across multiple tree levels.
  std::vector<Geometry*> sub_results;
  tree.query(Envelope(0.5, 2.5, 0.5, 2.5), sub_results);
  EXPECT_THAT(sub_results, UnorderedElementsAreArray(center_pts));

  Envelope full_env(-1.0, 10.0, -1.0, 10.0);

  // 1-arg void visitor.
  std::vector<Geometry*> v1_items;
  tree.query(full_env, [&](Geometry* g) { v1_items.push_back(g); });
  EXPECT_THAT(v1_items, UnorderedElementsAreArray(all_pts));

  // 2-arg void visitor.
  std::vector<Geometry*> v2_items;
  tree.query(full_env, [&](const Envelope& env, Geometry* g) {
    EXPECT_EQ(env, *g->getEnvelopeInternal());
    v2_items.push_back(g);
  });
  EXPECT_THAT(v2_items, UnorderedElementsAreArray(all_pts));

  // 1-arg bool visitor: continue vs early abort across multi-level branches.
  int v3_full = 0;
  tree.query(full_env, [&](const Geometry*) -> bool {
    ++v3_full;
    return true;
  });
  EXPECT_EQ(v3_full, 25);

  int v3_abort = 0;
  tree.query(full_env, [&](const Geometry*) -> bool {
    ++v3_abort;
    return v3_abort < 3;
  });
  EXPECT_EQ(v3_abort, 3);

  // 2-arg bool visitor: continue vs early abort across multi-level branches.
  int v4_full = 0;
  tree.query(full_env, [&](const Envelope& env, const Geometry* g) -> bool {
    EXPECT_EQ(env, *g->getEnvelopeInternal());
    ++v4_full;
    return true;
  });
  EXPECT_EQ(v4_full, 25);

  int v4_abort = 0;
  tree.query(full_env, [&](const Envelope&, const Geometry*) -> bool {
    ++v4_abort;
    return v4_abort < 4;
  });
  EXPECT_EQ(v4_abort, 4);
}

TEST_F(TemplateSTRtreeTest, ItemsIteratorAndRemovals) {
  TemplateSTRtree<Geometry*> tree(3);
  std::vector<Geometry*> pts;
  for (int i = 0; i < 15; ++i) {
    Geometry* p = MakePoint(static_cast<double>(i), static_cast<double>(i % 3));
    pts.push_back(p);
    tree.insert(p);
  }
  tree.build();

  // Removing with a disjoint envelope or non-existent item returns false.
  Geometry* outside = MakePoint(500.0, 500.0);
  EXPECT_FALSE(tree.remove(Envelope(500.0, 500.0, 500.0, 500.0), pts[0]));
  EXPECT_FALSE(tree.remove(*pts[5]->getEnvelopeInternal(), outside));
  EXPECT_FALSE(tree.remove(Envelope(-1.0, 20.0, -1.0, 5.0), outside));

  // Remove first, consecutive middle, and last points.
  EXPECT_TRUE(tree.remove(*pts[0]->getEnvelopeInternal(), pts[0]));
  EXPECT_TRUE(tree.remove(*pts[5]->getEnvelopeInternal(), pts[5]));
  EXPECT_TRUE(tree.remove(*pts[6]->getEnvelopeInternal(), pts[6]));
  EXPECT_TRUE(tree.remove(*pts[14]->getEnvelopeInternal(), pts[14]));
  // Removing an already-removed item returns false.
  EXPECT_FALSE(tree.remove(*pts[5]->getEnvelopeInternal(), pts[5]));

  std::vector<Geometry*> expected_remaining;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    if (i != 0 && i != 5 && i != 6 && i != 14) {
      expected_remaining.push_back(pts[i]);
    }
  }

  std::vector<Geometry*> via_items;
  auto items_view = tree.items();
  for (auto it = items_view.begin(); it != items_view.end(); ++it) {
    via_items.push_back(*it);
  }
  EXPECT_THAT(via_items, UnorderedElementsAreArray(expected_remaining));

  std::vector<Geometry*> via_iterate;
  tree.iterate([&](Geometry* g) { via_iterate.push_back(g); });
  EXPECT_THAT(via_iterate, UnorderedElementsAreArray(expected_remaining));

  std::vector<Geometry*> via_query;
  tree.query(Envelope(-1.0, 20.0, -1.0, 5.0), via_query);
  EXPECT_THAT(via_query, UnorderedElementsAreArray(expected_remaining));

  // Remove all remaining items and verify skipDeleted reaches end cleanly.
  for (Geometry* p : expected_remaining) {
    EXPECT_TRUE(tree.remove(*p->getEnvelopeInternal(), p));
  }
  auto empty_view = tree.items();
  EXPECT_TRUE(empty_view.begin() == empty_view.end());

  std::vector<Geometry*> after_clear_query;
  tree.query(Envelope(-1.0, 20.0, -1.0, 5.0), after_clear_query);
  EXPECT_THAT(after_clear_query, IsEmpty());
}

TEST_F(TemplateSTRtreeTest, QueryPairsAllAndAbortAndDeleted) {
  // Cluster 1: 5 overlapping boxes around [0, 5].
  // Cluster 2: 5 overlapping boxes around [100, 105].
  TemplateSTRtree<Geometry*> tree(2);
  std::vector<Geometry*> boxes;
  for (int i = 0; i < 5; ++i) {
    double offset = 0.2 * static_cast<double>(i);
    boxes.push_back(MakeBox(offset, offset + 2.0, offset, offset + 2.0));
    boxes.push_back(MakeBox(100.0 + offset, 102.0 + offset, 100.0 + offset,
                            102.0 + offset));
  }
  for (Geometry* b : boxes) {
    tree.insert(b);
  }

  std::set<std::pair<const Geometry*, const Geometry*>> expected_pairs;
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    for (std::size_t j = i + 1; j < boxes.size(); ++j) {
      if (boxes[i]->getEnvelopeInternal()->intersects(
              boxes[j]->getEnvelopeInternal())) {
        const Geometry* lo = std::min(boxes[i], boxes[j]);
        const Geometry* hi = std::max(boxes[i], boxes[j]);
        expected_pairs.emplace(lo, hi);
      }
    }
  }
  ASSERT_EQ(expected_pairs.size(), 20U);

  // Calling queryPairs before explicit build() triggers build().
  std::set<std::pair<const Geometry*, const Geometry*>> found_void;
  tree.queryPairs([&](const Geometry* a, const Geometry* b) {
    EXPECT_NE(a, b);
    found_void.emplace(std::min(a, b), std::max(a, b));
  });
  EXPECT_TRUE(tree.built());
  EXPECT_EQ(found_void, expected_pairs);

  // Bool visitor returning true visits all pairs.
  std::set<std::pair<const Geometry*, const Geometry*>> found_bool_all;
  tree.queryPairs([&](const Geometry* a, const Geometry* b) -> bool {
    found_bool_all.emplace(std::min(a, b), std::max(a, b));
    return true;
  });
  EXPECT_EQ(found_bool_all, expected_pairs);

  // Bool visitor returning false aborts each node's recursive search early.
  int abort_count = 0;
  tree.queryPairs([&](const Geometry*, const Geometry*) -> bool {
    ++abort_count;
    return false;
  });
  EXPECT_GT(abort_count, 0);
  EXPECT_LT(abort_count, static_cast<int>(expected_pairs.size()));

  // Deleted higher-index leaf is skipped in queryPairs.
  TemplateSTRtree<Geometry*> two_box_tree(10);
  Geometry* left = MakeBox(0.0, 5.0, 0.0, 5.0);
  Geometry* right = MakeBox(2.0, 8.0, 0.0, 5.0);
  two_box_tree.insert(left);
  two_box_tree.insert(right);
  EXPECT_TRUE(two_box_tree.remove(*right->getEnvelopeInternal(), right));
  int pairs_after_delete = 0;
  two_box_tree.queryPairs(
      [&](const Geometry*, const Geometry*) { ++pairs_after_delete; });
  EXPECT_EQ(pairs_after_delete, 0);
}

TEST_F(TemplateSTRtreeTest, NearestNeighbourSingleAndTwoTree) {
  TemplateSTRtree<Geometry*> tree1(2);
  TemplateSTRtree<Geometry*> tree2(2);

  std::vector<Geometry*> pts1 = {
      MakePoint(0.0, 0.0),  MakePoint(2.0, 0.0),  MakePoint(5.0, 0.0),
      MakePoint(10.0, 0.0), MakePoint(10.0, 1.0), MakePoint(20.0, 0.0),
      MakePoint(20.0, 3.0), MakePoint(30.0, 4.0),
  };
  std::vector<Geometry*> pts2 = {
      MakePoint(0.0, 10.0),  MakePoint(5.0, 10.0),  MakePoint(10.0, 3.5),
      MakePoint(15.0, 10.0), MakePoint(20.0, 10.0), MakePoint(25.0, 10.0),
  };
  for (Geometry* p : pts1) {
    tree1.insert(p);
  }
  for (Geometry* p : pts2) {
    tree2.insert(p);
  }

  // Single-tree nearest neighbour (distinct items).
  DistinctPointDistance distinct_dist;
  auto nn_self_default = tree1.nearestNeighbour<DistinctPointDistance>(tree1);
  auto nn_self_ref = tree1.nearestNeighbour(distinct_dist);
  EXPECT_NE(nn_self_default.first, nn_self_default.second);
  EXPECT_DOUBLE_EQ(nn_self_default.first->distance(nn_self_default.second),
                   1.0);
  EXPECT_DOUBLE_EQ(nn_self_ref.first->distance(nn_self_ref.second), 1.0);

  // Asymmetric leaf-tree vs composite-tree nearest neighbour.
  TemplateSTRtree<Geometry*> single_tree(2);
  Geometry* single_pt = MakePoint(10.0, 4.0);
  single_tree.insert(single_pt);
  PointDistance point_dist;
  EXPECT_THAT(single_tree.nearestNeighbour(tree1, point_dist),
              Pair(single_pt, pts1[4]));
  EXPECT_THAT(tree1.nearestNeighbour(single_tree, point_dist),
              Pair(pts1[4], single_pt));

  // Two-tree nearest neighbour: closest pair is (10, 1) and (10, 3.5) -> 2.5.
  TrackingDistance tracked_dist;
  auto nn_two_default = tree1.nearestNeighbour<PointDistance>(tree2);
  auto nn_two_ref = tree1.nearestNeighbour(tree2, tracked_dist);
  EXPECT_GT(tracked_dist.calls, 0);
  EXPECT_DOUBLE_EQ(nn_two_default.first->distance(nn_two_default.second), 2.5);
  EXPECT_DOUBLE_EQ(nn_two_ref.first->distance(nn_two_ref.second), 2.5);

  // Query-item nearest neighbour.
  Geometry* query_pt = MakePoint(19.0, 2.5);
  TrackingDistance item_tracked_dist;
  Geometry* nn_item_default = tree1.nearestNeighbour<PointDistance>(
      *query_pt->getEnvelopeInternal(), query_pt);
  Geometry* nn_item_ref = tree1.nearestNeighbour(
      *query_pt->getEnvelopeInternal(), query_pt, item_tracked_dist);
  EXPECT_GT(item_tracked_dist.calls, 0);
  EXPECT_EQ(nn_item_default, pts1[6]);
  EXPECT_EQ(nn_item_ref, pts1[6]);
}

TEST_F(TemplateSTRtreeTest, IsWithinDistanceCases) {
  TemplateSTRtree<Geometry*> tree_a(2);
  TemplateSTRtree<Geometry*> tree_b(2);
  for (int i = 0; i < 8; ++i) {
    tree_a.insert(MakePoint(static_cast<double>(i), 0.0));
    tree_b.insert(MakePoint(static_cast<double>(i), 10.0));
  }

  // Immediate accept via root maximumDistance <= maxDistance.
  EXPECT_TRUE(tree_a.isWithinDistance<PointDistance>(tree_b, 20.0));
  // Exact leaf distance == maxDistance (root maxDistance ~ 12.2 > 10.0).
  EXPECT_TRUE(tree_a.isWithinDistance<PointDistance>(tree_b, 10.0));
  // Immediate reject via root distance > maxDistance.
  EXPECT_FALSE(tree_a.isWithinDistance<PointDistance>(tree_b, 9.9));

  // Overlapping bounding boxes with parallel diagonal segments separated by
  // 2 * sqrt(2) (~2.828) force tree expansion before rejecting/accepting.
  TemplateSTRtree<Geometry*> diag_a(2);
  TemplateSTRtree<Geometry*> diag_b(2);
  for (int i = 0; i < 6; ++i) {
    double t0 = static_cast<double>(i);
    double t1 = t0 + 1.0;
    diag_a.insert(MakeSegment(t0, t0 + 2.0, t1, t1 + 2.0));
    diag_b.insert(MakeSegment(t0 + 2.0, t0, t1 + 2.0, t1));
  }
  EXPECT_FALSE(diag_a.isWithinDistance<PointDistance>(diag_b, 1.0));
  EXPECT_TRUE(diag_a.isWithinDistance<PointDistance>(diag_b, 3.0));
}

TEST_F(TemplateSTRtreeTest, SpatialIndexInterfaceOverrides) {
  TemplateSTRtree<Geometry*> concrete_tree(3);
  SpatialIndex* index = &concrete_tree;

  std::vector<Geometry*> geoms;
  for (int i = 0; i < 10; ++i) {
    Geometry* p = MakePoint(static_cast<double>(i), static_cast<double>(i));
    geoms.push_back(p);
    index->insert(p->getEnvelopeInternal(), p);
  }
  Envelope null_env;
  index->insert(&null_env, geoms[0]);

  Envelope query_env(2.5, 5.5, 2.5, 5.5);
  std::vector<void*> vec_results;
  index->query(&query_env, vec_results);
  EXPECT_THAT(vec_results, UnorderedElementsAre(geoms[3], geoms[4], geoms[5]));

  CollectingItemVisitor visitor;
  index->query(&query_env, visitor);
  EXPECT_THAT(visitor.items,
              UnorderedElementsAre(geoms[3], geoms[4], geoms[5]));

  EXPECT_TRUE(index->remove(geoms[4]->getEnvelopeInternal(), geoms[4]));
  EXPECT_FALSE(index->remove(geoms[4]->getEnvelopeInternal(), geoms[4]));

  std::vector<void*> after_remove;
  index->query(&query_env, after_remove);
  EXPECT_THAT(after_remove, UnorderedElementsAre(geoms[3], geoms[5]));
}

TEST_F(TemplateSTRtreeTest, PrimaryTemplateWithValueItemAndEnvelopeTraits) {
  TemplateSTRtree<GeomHandle> tree(2);
  std::vector<GeomHandle> handles;
  for (int i = 0; i < 8; ++i) {
    handles.push_back(GeomHandle{MakePoint(static_cast<double>(i), 0.0), i});
  }

  // Exercise both lvalue and rvalue inserts on primary template.
  tree.insert(handles[0]);
  tree.insert(GeomHandle{handles[1].geom, handles[1].id});
  tree.insert(*handles[2].getEnvelopeInternal(), handles[2]);
  tree.insert(*handles[3].getEnvelopeInternal(),
              GeomHandle{handles[3].geom, handles[3].id});
  for (std::size_t i = 4; i < handles.size(); ++i) {
    tree.insert(handles[i]);
  }

  TemplateSTRtree<GeomHandle> copied(tree);
  TemplateSTRtree<GeomHandle> assigned(3);
  assigned = tree;

  std::vector<GeomHandle> results;
  copied.query(Envelope(1.5, 4.5, -1.0, 1.0), results);
  EXPECT_THAT(results,
              UnorderedElementsAre(handles[2], handles[3], handles[4]));

  EXPECT_TRUE(assigned.remove(*handles[2].getEnvelopeInternal(), handles[2]));
  std::vector<GeomHandle> remaining;
  for (const GeomHandle& h : assigned.items()) {
    remaining.push_back(h);
  }
  EXPECT_THAT(remaining, SizeIs(7));
}

TEST_F(TemplateSTRtreeTest, IntervalTraitsOneDimensionalTree) {
  TemplateSTRtree<int, IntervalTraits> tree(3);

  for (int i = 0; i < 20; ++i) {
    double start = static_cast<double>(i) * 2.0;
    Interval iv(start, start + 3.0);
    if (i % 2 == 0) {
      tree.insert(iv, int{i});
    } else {
      int lvalue_item = i;
      tree.insert(iv, lvalue_item);
    }
  }

  // TwoDimensional::value is false, so build skips Y sorting.
  tree.build();
  EXPECT_TRUE(tree.built());

  std::vector<int> hits;
  tree.query(Interval(5.5, 9.5), hits);
  // Intervals [2,5]..[10,13]: i=2 ([4,7]), i=3 ([6,9]), i=4 ([8,11]).
  EXPECT_THAT(hits, UnorderedElementsAre(2, 3, 4));

  std::vector<int> misses;
  tree.query(Interval(100.0, 110.0), misses);
  EXPECT_THAT(misses, IsEmpty());

  int pair_count = 0;
  tree.queryPairs([&](int a, int b) {
    EXPECT_NE(a, b);
    ++pair_count;
  });
  // Adjacent intervals [2i, 2i+3] and [2(i+1), 2(i+1)+3] overlap for i=0..18.
  EXPECT_EQ(pair_count, 19);

  EXPECT_TRUE(tree.remove(Interval(4.0, 7.0), 2));
  EXPECT_FALSE(tree.remove(Interval(4.0, 7.0), 2));

  std::vector<int> after_remove;
  tree.query(Interval(5.5, 9.5), after_remove);
  EXPECT_THAT(after_remove, UnorderedElementsAre(3, 4));

  // Direct coverage of all IntervalTraits static helpers.
  Interval a(1.0, 5.0);
  Interval b(4.0, 9.0);
  Interval c(10.0, 12.0);
  EXPECT_TRUE(IntervalTraits::intersects(a, b));
  EXPECT_FALSE(IntervalTraits::intersects(a, c));
  EXPECT_DOUBLE_EQ(IntervalTraits::size(a), 4.0);
  EXPECT_DOUBLE_EQ(IntervalTraits::getX(a), 6.0);
  EXPECT_DOUBLE_EQ(IntervalTraits::getY(a), 6.0);
  EXPECT_FALSE(IntervalTraits::isNull(a));
  IntervalTraits::expandToInclude(a, b);
  EXPECT_DOUBLE_EQ(a.getMin(), 1.0);
  EXPECT_DOUBLE_EQ(a.getMax(), 9.0);
}

TEST_F(TemplateSTRtreeTest, EnvelopeTraitsDirectAndNanSorting) {
  // Direct coverage of EnvelopeTraits static helpers including empty() and
  // both fromItem overloads.
  Envelope empty_env = EnvelopeTraits::empty();
  EXPECT_TRUE(EnvelopeTraits::isNull(empty_env));

  Envelope a(0.0, 2.0, 0.0, 3.0);
  Envelope b(5.0, 7.0, 4.0, 6.0);
  EXPECT_FALSE(EnvelopeTraits::isNull(a));
  EXPECT_FALSE(EnvelopeTraits::intersects(a, b));
  EXPECT_DOUBLE_EQ(EnvelopeTraits::size(a), 6.0);
  EXPECT_DOUBLE_EQ(EnvelopeTraits::distance(a, b), std::hypot(3.0, 1.0));
  EXPECT_DOUBLE_EQ(EnvelopeTraits::maxDistance(a, b), std::hypot(7.0, 6.0));
  EXPECT_DOUBLE_EQ(EnvelopeTraits::getX(a), 2.0);
  EXPECT_DOUBLE_EQ(EnvelopeTraits::getY(a), 3.0);

  Geometry* pt = MakePoint(3.0, 4.0);
  const Geometry* const const_pt = pt;
  EXPECT_EQ(EnvelopeTraits::fromItem(const_pt), *pt->getEnvelopeInternal());
  EXPECT_EQ(EnvelopeTraits::fromItem(MakePoint(3.0, 4.0)),
            *pt->getEnvelopeInternal());

  EnvelopeTraits::expandToInclude(a, b);
  EXPECT_TRUE(EnvelopeTraits::intersects(a, b));
  EXPECT_DOUBLE_EQ(a.getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(a.getMaxX(), 7.0);
  EXPECT_DOUBLE_EQ(a.getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(a.getMaxY(), 6.0);

  // Infinite envelopes (-inf, +inf) are non-null, yet getX/getY produce NaN
  // (-inf + inf), exercising NaN guards in both sortNodesX and sortNodesY.
  TemplateSTRtree<int> tree(2);
  Envelope inf_env(geos::DoubleNegInfinity, geos::DoubleInfinity,
                   geos::DoubleNegInfinity, geos::DoubleInfinity);
  Envelope inf_y_hit(0.5, 0.6, geos::DoubleNegInfinity, geos::DoubleInfinity);
  Envelope inf_y_miss(8.5, 8.6, geos::DoubleNegInfinity, geos::DoubleInfinity);
  Envelope inf_x_hit(geos::DoubleNegInfinity, geos::DoubleInfinity, 0.5, 0.6);
  Envelope inf_x_miss(geos::DoubleNegInfinity, geos::DoubleInfinity, 8.5, 8.6);
  tree.insert(inf_env, 100);
  for (int i = 0; i < 10; ++i) {
    double v = static_cast<double>(i);
    tree.insert(Envelope(v, v + 1.0, v, v + 1.0), i);
  }
  tree.insert(inf_env, 101);
  tree.insert(inf_y_hit, 102);
  tree.insert(inf_y_miss, 103);
  tree.insert(inf_x_hit, 104);
  tree.insert(inf_x_miss, 105);

  std::vector<int> hits;
  tree.query(Envelope(0.25, 0.75, 0.25, 0.75), hits);
  EXPECT_THAT(hits, UnorderedElementsAre(0, 100, 101, 102, 104));

  std::vector<int> high_y_hits;
  tree.query(Envelope(0.25, 0.75, 8.55, 8.65), high_y_hits);
  EXPECT_THAT(high_y_hits, UnorderedElementsAre(100, 101, 102, 105));
}

TEST_F(TemplateSTRtreeTest, RemoveMatchingBranchesAndIteratorSkipDeleted) {
  // Multiple items at identical coordinates force remove() to descend into
  // matching-envelope branches and backtrack when the item pointer differs.
  TemplateSTRtree<Geometry*> dup_tree(2);
  std::vector<Geometry*> dups;
  for (int i = 0; i < 6; ++i) {
    dups.push_back(MakePoint(1.0, 2.0));
    dup_tree.insert(dups.back());
  }
  dup_tree.build();

  Envelope pt_env(1.0, 1.0, 2.0, 2.0);
  EXPECT_TRUE(dup_tree.remove(pt_env, dups[5]));
  EXPECT_TRUE(dup_tree.remove(pt_env, dups[0]));
  EXPECT_FALSE(dup_tree.remove(pt_env, dups[5]));
  EXPECT_FALSE(dup_tree.remove(pt_env, MakePoint(1.0, 2.0)));

  std::vector<Geometry*> remaining_dups;
  dup_tree.query(pt_env, remaining_dups);
  EXPECT_THAT(remaining_dups,
              UnorderedElementsAre(dups[1], dups[2], dups[3], dups[4]));

  // Distinct X-sorted items in a single-branch tree verify deterministic
  // skipDeleted() on leading, middle, and trailing deleted entries.
  TemplateSTRtree<Geometry*> seq_tree(10);
  Geometry* p0 = MakePoint(0.0, 0.0);
  Geometry* p1 = MakePoint(1.0, 0.0);
  Geometry* p2 = MakePoint(2.0, 0.0);
  Geometry* p3 = MakePoint(3.0, 0.0);
  Geometry* p4 = MakePoint(4.0, 0.0);
  seq_tree.insert(p0);
  seq_tree.insert(p1);
  seq_tree.insert(p2);
  seq_tree.insert(p3);
  seq_tree.insert(p4);
  seq_tree.build();

  EXPECT_TRUE(seq_tree.remove(*p0->getEnvelopeInternal(), p0));
  EXPECT_TRUE(seq_tree.remove(*p2->getEnvelopeInternal(), p2));
  EXPECT_TRUE(seq_tree.remove(*p4->getEnvelopeInternal(), p4));

  auto view = seq_tree.items();
  auto it = view.begin();
  ASSERT_TRUE(it != view.end());
  EXPECT_EQ(*it, p1);
  ++it;
  ASSERT_TRUE(it != view.end());
  EXPECT_EQ(*it, p3);
  ++it;
  EXPECT_TRUE(it == view.end());

  // Asymmetric single-leaf vs composite-tree isWithinDistance.
  TemplateSTRtree<Geometry*> leaf_tree(2);
  leaf_tree.insert(MakePoint(1.0, 2.5));
  EXPECT_TRUE(leaf_tree.isWithinDistance<PointDistance>(dup_tree, 0.6));
  EXPECT_FALSE(leaf_tree.isWithinDistance<PointDistance>(dup_tree, 0.4));
  EXPECT_TRUE(dup_tree.isWithinDistance<PointDistance>(leaf_tree, 0.6));
  EXPECT_FALSE(dup_tree.isWithinDistance<PointDistance>(leaf_tree, 0.4));
}

}  // namespace
}  // namespace geos_google
