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

// Unit tests for third_party/geos/src/index/quadtree/Quadtree.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/quadtree/Quadtree.cpp

#include "third_party/geos/include/geos/index/quadtree/Quadtree.h"

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/SpatialIndex.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::index::ItemVisitor;
using geos::index::SpatialIndex;
using geos::index::quadtree::Quadtree;
using geos::util::IllegalArgumentException;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedElementsAreArray;

class RecordingVisitor : public ItemVisitor {
 public:
  void visitItem(void* item) override { visited_.push_back(item); }

  const std::vector<void*>& visited() const { return visited_; }

 private:
  std::vector<void*> visited_;
};

TEST(QuadtreeTest, EnsureExtentHandlesNonZeroAndZeroExtents) {
  // Non-zero X and Y extent: returns original pointer unchanged.
  const Envelope box(1.0, 3.0, 2.0, 5.0);
  Envelope* same = Quadtree::ensureExtent(&box, 2.0);
  EXPECT_EQ(same, &box);

  // Zero X extent only: allocates padded envelope containing original X.
  // Quadtree::ensureExtent updates minx first and sets maxx = minx +
  // minExtent/2.
  const Envelope vertical(4.0, 4.0, 2.0, 5.0);
  std::unique_ptr<Envelope> padded_x(Quadtree::ensureExtent(&vertical, 2.0));
  ASSERT_NE(padded_x.get(), &vertical);
  EXPECT_DOUBLE_EQ(padded_x->getMinX(), 3.0);
  EXPECT_DOUBLE_EQ(padded_x->getMaxX(), 4.0);
  EXPECT_DOUBLE_EQ(padded_x->getMinY(), 2.0);
  EXPECT_DOUBLE_EQ(padded_x->getMaxY(), 5.0);
  EXPECT_GT(padded_x->getWidth(), 0.0);
  EXPECT_TRUE(padded_x->contains(vertical));

  // Zero Y extent only: allocates padded envelope containing original Y.
  const Envelope horizontal(1.0, 3.0, 5.0, 5.0);
  std::unique_ptr<Envelope> padded_y(Quadtree::ensureExtent(&horizontal, 2.0));
  ASSERT_NE(padded_y.get(), &horizontal);
  EXPECT_DOUBLE_EQ(padded_y->getMinX(), 1.0);
  EXPECT_DOUBLE_EQ(padded_y->getMaxX(), 3.0);
  EXPECT_DOUBLE_EQ(padded_y->getMinY(), 4.0);
  EXPECT_DOUBLE_EQ(padded_y->getMaxY(), 5.0);
  EXPECT_GT(padded_y->getHeight(), 0.0);
  EXPECT_TRUE(padded_y->contains(horizontal));

  // Zero X and Y extent (point): pads both dimensions.
  const Envelope point(4.0, 4.0, 5.0, 5.0);
  std::unique_ptr<Envelope> padded_xy(Quadtree::ensureExtent(&point, 2.0));
  ASSERT_NE(padded_xy.get(), &point);
  EXPECT_DOUBLE_EQ(padded_xy->getMinX(), 3.0);
  EXPECT_DOUBLE_EQ(padded_xy->getMaxX(), 4.0);
  EXPECT_DOUBLE_EQ(padded_xy->getMinY(), 4.0);
  EXPECT_DOUBLE_EQ(padded_xy->getMaxY(), 5.0);
  EXPECT_GT(padded_xy->getWidth(), 0.0);
  EXPECT_GT(padded_xy->getHeight(), 0.0);
  EXPECT_TRUE(padded_xy->contains(point));
}

TEST(QuadtreeTest, EmptyTreeQueriesAndToString) {
  Quadtree tree;
  EXPECT_EQ(tree.depth(), 1u);
  EXPECT_EQ(tree.size(), 0u);

  std::unique_ptr<std::vector<void*>> all(tree.queryAll());
  ASSERT_NE(all, nullptr);
  EXPECT_THAT(*all, IsEmpty());

  const Envelope search(-10.0, 10.0, -10.0, 10.0);
  std::vector<void*> found;
  tree.query(&search, found);
  EXPECT_THAT(found, IsEmpty());

  RecordingVisitor visitor;
  tree.query(&search, visitor);
  EXPECT_THAT(visitor.visited(), IsEmpty());

  int dummy = 42;
  EXPECT_FALSE(tree.remove(&search, &dummy));

  const std::string text = tree.toString();
  EXPECT_THAT(text, HasSubstr("ITEMS:0"));
  EXPECT_THAT(text, HasSubstr("NULL"));
}

TEST(QuadtreeTest, InsertCollectsStatsAndHandlesAllExtentKinds) {
  Quadtree tree;
  int pt_before = 1;
  int large_box = 2;
  int narrow_x = 3;
  int narrow_y = 4;
  int vert_seg = 5;
  int horiz_seg = 6;
  int origin_cross = 7;

  // Point inserted first when minExtent is default 1.0 (padded & owned).
  const Envelope env_pt(2.0, 2.0, 3.0, 3.0);
  tree.insert(&env_pt, &pt_before);

  // Larger than minExtent (delX >= 1.0, delY >= 1.0): does not shrink
  // minExtent.
  const Envelope env_large(10.0, 14.0, 10.0, 15.0);
  tree.insert(&env_large, &large_box);

  // Shrinks minExtent via delX = 0.25.
  const Envelope env_narrow_x(1.0, 1.25, 1.0, 3.0);
  tree.insert(&env_narrow_x, &narrow_x);

  // Shrinks minExtent further via delY = 0.1.
  const Envelope env_narrow_y(1.0, 2.0, 1.0, 1.1);
  tree.insert(&env_narrow_y, &narrow_y);

  // Zero-width and zero-height extents padded with current minExtent = 0.1.
  const Envelope env_vert(-2.0, -2.0, -3.0, -1.0);
  const Envelope env_horiz(-3.0, -1.0, 2.0, 2.0);
  const Envelope env_origin(-1.0, 1.0, -1.0, 1.0);
  tree.insert(&env_vert, &vert_seg);
  tree.insert(&env_horiz, &horiz_seg);
  tree.insert(&env_origin, &origin_cross);

  EXPECT_EQ(tree.size(), 7u);
  EXPECT_GT(tree.depth(), 1u);

  std::unique_ptr<std::vector<void*>> all(tree.queryAll());
  ASSERT_NE(all, nullptr);
  EXPECT_THAT(*all,
              UnorderedElementsAre(&pt_before, &large_box, &narrow_x, &narrow_y,
                                   &vert_seg, &horiz_seg, &origin_cross));
  EXPECT_THAT(tree.toString(), HasSubstr("ITEMS:1"));
}

TEST(QuadtreeTest, ThrowsIllegalArgumentOnInfiniteEnvelope) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  Quadtree tree;
  int item = 1;

  const Envelope inf_x(0.0, kInf, 0.0, 1.0);
  EXPECT_THROW(tree.insert(&inf_x, &item), IllegalArgumentException);

  const Envelope neg_inf_y(0.0, 1.0, -kInf, 0.0);
  EXPECT_THROW(tree.insert(&neg_inf_y, &item), IllegalArgumentException);

  EXPECT_EQ(tree.size(), 0u);
}

TEST(QuadtreeTest, QueryVectorAndVisitorMatchAndFilterDisjointRegions) {
  Quadtree tree;
  int q0 = 0;
  int q1 = 1;
  int q2 = 2;
  int q3_a = 3;
  int q3_b = 4;
  int root_cross = 5;

  const Envelope env_q0(-20.0, -18.0, -20.0, -18.0);
  const Envelope env_q1(18.0, 20.0, -20.0, -18.0);
  const Envelope env_q2(-20.0, -18.0, 18.0, 20.0);
  const Envelope env_q3_a(1.0, 2.0, 1.0, 2.0);
  const Envelope env_q3_b(50.0, 52.0, 50.0, 52.0);
  const Envelope env_cross(-1.0, 1.0, -1.0, 1.0);

  tree.insert(&env_q0, &q0);
  tree.insert(&env_q1, &q1);
  tree.insert(&env_q2, &q2);
  tree.insert(&env_q3_a, &q3_a);
  tree.insert(&env_q3_b, &q3_b);
  tree.insert(&env_cross, &root_cross);

  // Query near env_q3_a: must include env_q3_a and root_cross, exclude distant
  // quadrants (and distant env_q3_b in another subquad of Q3).
  const Envelope search_near_q3_a(1.2, 1.8, 1.2, 1.8);
  int sentinel = 99;
  std::vector<void*> found = {&sentinel};
  tree.query(&search_near_q3_a, found);
  EXPECT_EQ(found.front(), &sentinel);
  EXPECT_THAT(found, Contains(&q3_a));
  EXPECT_THAT(found, Contains(&root_cross));
  EXPECT_THAT(found, Not(Contains(&q0)));
  EXPECT_THAT(found, Not(Contains(&q1)));
  EXPECT_THAT(found, Not(Contains(&q2)));
  EXPECT_THAT(found, Not(Contains(&q3_b)));

  // Visitor overload returns the exact same candidates as vector query.
  std::vector<void*> vec_only;
  tree.query(&search_near_q3_a, vec_only);
  RecordingVisitor visitor;
  tree.query(&search_near_q3_a, visitor);
  EXPECT_THAT(visitor.visited(), UnorderedElementsAreArray(vec_only));

  // Query far away from all quadrant items still finds root-crossing items.
  const Envelope far_away(1000.0, 1001.0, 1000.0, 1001.0);
  std::vector<void*> far_found;
  tree.query(&far_away, far_found);
  EXPECT_THAT(far_found, ElementsAre(&root_cross));

  // Null search envelope only matches root-level items safely.
  const Envelope null_env;
  std::vector<void*> null_found;
  tree.query(&null_env, null_found);
  EXPECT_THAT(null_found, ElementsAre(&root_cross));
}

TEST(QuadtreeTest, RemoveHandlesBoxesPointsShrinksAndNonExistent) {
  Quadtree tree;
  int box_item = 1;
  int point_item = 2;
  int tiny_box_item = 3;
  int other_item = 4;

  const Envelope box_env(10.0, 12.0, 10.0, 12.0);
  const Envelope point_env(5.0, 5.0, 5.0, 5.0);
  const Envelope tiny_env(0.1, 0.12, 0.1, 0.12);

  tree.insert(&box_env, &box_item);
  tree.insert(&point_env, &point_item);
  tree.insert(&tiny_env, &tiny_box_item);
  EXPECT_EQ(tree.size(), 3u);

  // Wrong item pointer or wrong envelope fails cleanly.
  EXPECT_FALSE(tree.remove(&box_env, &other_item));
  const Envelope wrong_env(-50.0, -49.0, -50.0, -49.0);
  EXPECT_FALSE(tree.remove(&wrong_env, &box_item));
  EXPECT_EQ(tree.size(), 3u);

  // Remove point even after minExtent shrunk from 1.0 to 0.02 (exercises
  // `posEnv != itemEnv` deletion path in remove()).
  EXPECT_TRUE(tree.remove(&point_env, &point_item));
  EXPECT_FALSE(tree.remove(&point_env, &point_item));
  EXPECT_EQ(tree.size(), 2u);

  // Remove remaining 2D boxes (exercises `posEnv == itemEnv` branch).
  EXPECT_TRUE(tree.remove(&tiny_env, &tiny_box_item));
  EXPECT_TRUE(tree.remove(&box_env, &box_item));
  EXPECT_EQ(tree.size(), 0u);
  EXPECT_EQ(tree.depth(), 1u);

  std::unique_ptr<std::vector<void*>> all(tree.queryAll());
  EXPECT_THAT(*all, IsEmpty());
}

TEST(QuadtreeTest, PolymorphicSpatialIndexInterface) {
  std::unique_ptr<SpatialIndex> index = std::make_unique<Quadtree>();
  int item_a = 1;
  int item_b = 2;

  const Envelope env_a(1.0, 2.0, 1.0, 2.0);
  const Envelope env_b(-4.0, -3.0, -4.0, -3.0);
  index->insert(&env_a, &item_a);
  index->insert(&env_b, &item_b);

  std::vector<void*> vec_hits;
  index->query(&env_a, vec_hits);
  EXPECT_THAT(vec_hits, ElementsAre(&item_a));

  RecordingVisitor visitor;
  index->query(&env_b, visitor);
  EXPECT_THAT(visitor.visited(), ElementsAre(&item_b));

  EXPECT_TRUE(index->remove(&env_a, &item_a));
  EXPECT_FALSE(index->remove(&env_a, &item_a));
}

TEST(QuadtreeTest, PrimaryFilterGuaranteesNoFalseNegativesOverGrid) {
  Quadtree tree;
  std::vector<Envelope> envelopes;
  envelopes.reserve(32);

  for (int ix = -2; ix < 2; ++ix) {
    for (int iy = -2; iy < 2; ++iy) {
      const double x = ix * 3.0 + 0.25;
      const double y = iy * 3.0 + 0.25;
      envelopes.emplace_back(x, x + 1.5, y, y + 1.5);
      envelopes.emplace_back(x + 0.5, x + 0.5, y + 0.5, y + 0.5);
    }
  }
  for (Envelope& env : envelopes) {
    tree.insert(&env, &env);
  }
  EXPECT_EQ(tree.size(), envelopes.size());

  const std::array<Envelope, 4> windows = {
      Envelope(-4.0, 4.0, -4.0, 4.0),
      Envelope(0.1, 2.0, 0.1, 2.0),
      Envelope(-6.0, -1.0, 2.0, 6.0),
      Envelope(100.0, 102.0, 100.0, 102.0),
  };

  for (const Envelope& win : windows) {
    std::vector<void*> candidates;
    tree.query(&win, candidates);
    for (Envelope& env : envelopes) {
      if (win.intersects(env)) {
        EXPECT_THAT(candidates, Contains(&env));
      }
    }
  }

  // Remove every other item and re-verify remaining items.
  for (std::size_t i = 0; i < envelopes.size(); i += 2) {
    EXPECT_TRUE(tree.remove(&envelopes[i], &envelopes[i]));
  }
  EXPECT_EQ(tree.size(), envelopes.size() / 2);

  const Envelope full_range(-20.0, 20.0, -20.0, 20.0);
  std::vector<void*> remaining;
  tree.query(&full_range, remaining);
  EXPECT_EQ(remaining.size(), envelopes.size() / 2);
  for (std::size_t i = 0; i < envelopes.size(); ++i) {
    if (i % 2 == 0) {
      EXPECT_THAT(remaining, Not(Contains(&envelopes[i])));
    } else {
      EXPECT_THAT(remaining, Contains(&envelopes[i]));
    }
  }
}

}  // namespace
}  // namespace geos_google
