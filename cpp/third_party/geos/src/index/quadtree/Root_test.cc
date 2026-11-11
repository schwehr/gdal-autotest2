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

// Unit tests for third_party/geos/src/index/quadtree/Root.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/quadtree/Root.cpp

#include "third_party/geos/include/geos/index/quadtree/Root.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/quadtree/NodeBase.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::index::ItemVisitor;
using geos::index::quadtree::NodeBase;
using geos::index::quadtree::Root;
using geos::util::IllegalArgumentException;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

class RecordingVisitor : public ItemVisitor {
 public:
  void visitItem(void* item) override { visited_.push_back(item); }

  const std::vector<void*>& visited() const { return visited_; }

 private:
  std::vector<void*> visited_;
};

TEST(RootTest, InitialStateIsEmptyAndPrunable) {
  Root root;
  EXPECT_EQ(root.depth(), 1u);
  EXPECT_EQ(root.size(), 0u);
  EXPECT_EQ(root.getNodeCount(), 1u);
  EXPECT_FALSE(root.hasItems());
  EXPECT_FALSE(root.hasChildren());
  EXPECT_TRUE(root.isPrunable());
}

TEST(RootTest, ThrowsIllegalArgumentOnNonFiniteEnvelopes) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

  Root root;
  int item = 1;

  const Envelope null_env;
  EXPECT_THROW(root.insert(&null_env, &item), IllegalArgumentException);

  const Envelope pos_inf_x(0.0, kInf, 0.0, 1.0);
  EXPECT_THROW(root.insert(&pos_inf_x, &item), IllegalArgumentException);

  const Envelope neg_inf_x(-kInf, 0.0, 0.0, 1.0);
  EXPECT_THROW(root.insert(&neg_inf_x, &item), IllegalArgumentException);

  const Envelope pos_inf_y(0.0, 1.0, 0.0, kInf);
  EXPECT_THROW(root.insert(&pos_inf_y, &item), IllegalArgumentException);

  const Envelope neg_inf_y(0.0, 1.0, -kInf, 0.0);
  EXPECT_THROW(root.insert(&neg_inf_y, &item), IllegalArgumentException);

  const Envelope nan_x(kNan, 1.0, 0.0, 1.0);
  EXPECT_THROW(root.insert(&nan_x, &item), IllegalArgumentException);

  const Envelope nan_y(0.0, 1.0, kNan, 1.0);
  EXPECT_THROW(root.insert(&nan_y, &item), IllegalArgumentException);

  EXPECT_EQ(root.size(), 0u);
  EXPECT_FALSE(root.hasItems());
  EXPECT_FALSE(root.hasChildren());
}

TEST(RootTest, AxisCrossingEnvelopesStoredDirectlyOnRoot) {
  Root root;
  int cross_y = 1;
  int cross_x = 2;
  int cross_both = 3;

  const Envelope env_cross_y(-1.0, 1.0, 2.0, 3.0);
  const Envelope env_cross_x(2.0, 3.0, -1.0, 1.0);
  const Envelope env_cross_both(-2.0, 2.0, -2.0, 2.0);

  root.insert(&env_cross_y, &cross_y);
  root.insert(&env_cross_x, &cross_x);
  root.insert(&env_cross_both, &cross_both);

  EXPECT_TRUE(root.hasItems());
  EXPECT_FALSE(root.hasChildren());
  EXPECT_EQ(root.depth(), 1u);
  EXPECT_EQ(root.size(), 3u);
  EXPECT_THAT(root.getItems(),
              UnorderedElementsAre(&cross_y, &cross_x, &cross_both));
}

TEST(RootTest, IsSearchMatchAlwaysTrueForRootItems) {
  Root root;
  int root_item = 1;
  int q3_item = 2;

  const Envelope crossing(-1.0, 1.0, -1.0, 1.0);
  const Envelope q3_env(2.0, 3.0, 2.0, 3.0);
  root.insert(&crossing, &root_item);
  root.insert(&q3_env, &q3_item);

  // Even for a disjoint or null search envelope, Root::isSearchMatch returns
  // true, so root-level items are visited/returned while disjoint quadrant
  // subnodes are filtered out.
  const Envelope distant(1000.0, 1001.0, 1000.0, 1001.0);
  std::vector<void*> overlapping;
  root.addAllItemsFromOverlapping(distant, overlapping);
  EXPECT_THAT(overlapping, UnorderedElementsAre(&root_item));

  RecordingVisitor visitor;
  root.visit(&distant, visitor);
  EXPECT_THAT(visitor.visited(), UnorderedElementsAre(&root_item));

  const Envelope null_env;
  std::vector<void*> null_overlapping;
  root.addAllItemsFromOverlapping(null_env, null_overlapping);
  EXPECT_THAT(null_overlapping, UnorderedElementsAre(&root_item));
}

TEST(RootTest, InsertsIntoAllFourQuadrants) {
  Root root;
  int q0 = 0;
  int q1 = 1;
  int q2 = 2;
  int q3 = 3;

  const Envelope env0(-3.0, -1.0, -3.0, -1.0);
  const Envelope env1(1.0, 3.0, -3.0, -1.0);
  const Envelope env2(-3.0, -1.0, 1.0, 3.0);
  const Envelope env3(1.0, 3.0, 1.0, 3.0);

  root.insert(&env0, &q0);
  root.insert(&env1, &q1);
  root.insert(&env2, &q2);
  root.insert(&env3, &q3);

  EXPECT_FALSE(root.hasItems());
  EXPECT_TRUE(root.hasChildren());
  EXPECT_EQ(root.size(), 4u);
  EXPECT_GE(root.depth(), 2u);

  std::vector<void*> q0_hits;
  root.addAllItemsFromOverlapping(Envelope(-2.5, -1.5, -2.5, -1.5), q0_hits);
  EXPECT_THAT(q0_hits, UnorderedElementsAre(&q0));

  std::vector<void*> q1_hits;
  root.addAllItemsFromOverlapping(Envelope(1.5, 2.5, -2.5, -1.5), q1_hits);
  EXPECT_THAT(q1_hits, UnorderedElementsAre(&q1));

  std::vector<void*> q2_hits;
  root.addAllItemsFromOverlapping(Envelope(-2.5, -1.5, 1.5, 2.5), q2_hits);
  EXPECT_THAT(q2_hits, UnorderedElementsAre(&q2));

  std::vector<void*> q3_hits;
  root.addAllItemsFromOverlapping(Envelope(1.5, 2.5, 1.5, 2.5), q3_hits);
  EXPECT_THAT(q3_hits, UnorderedElementsAre(&q3));
}

TEST(RootTest, InsertsContainedAndExpandsUpwardWhenOutsideSubquad) {
  Root root;
  int first = 1;
  int inner = 2;
  int outer = 3;

  // First item creates Q3 subquad [0, 2] x [0, 2].
  const Envelope env_first(1.0, 2.0, 1.0, 2.0);
  root.insert(&env_first, &first);
  const std::size_t initial_depth = root.depth();

  // Second item lies strictly inside existing subquad [0, 2] x [0, 2] ->
  // takes the non-expanding branch and subdivides downward.
  const Envelope env_inner(0.125, 0.375, 0.125, 0.375);
  root.insert(&env_inner, &inner);
  EXPECT_GT(root.depth(), initial_depth);
  EXPECT_EQ(root.size(), 2u);

  // Third item lies outside [0, 2] x [0, 2] -> expands Q3 upward.
  const Envelope env_outer(10.0, 11.0, 10.0, 11.0);
  root.insert(&env_outer, &outer);
  EXPECT_EQ(root.size(), 3u);

  std::vector<void*> all;
  root.addAllItems(all);
  EXPECT_THAT(all, UnorderedElementsAre(&first, &inner, &outer));

  std::vector<void*> outer_hits;
  root.addAllItemsFromOverlapping(Envelope(10.2, 10.8, 10.2, 10.8), outer_hits);
  EXPECT_THAT(outer_hits, UnorderedElementsAre(&outer));

  std::vector<void*> inner_hits;
  root.addAllItemsFromOverlapping(Envelope(0.2, 0.3, 0.2, 0.3), inner_hits);
  EXPECT_THAT(inner_hits, UnorderedElementsAre(&inner));
}

TEST(RootTest, InsertContainedHandlesZeroWidthAndNearZeroWidthEnvelopes) {
  Root root;
  int box = 1;
  int sub_box = 2;
  int point = 3;
  int vertical_seg = 4;
  int horizontal_seg = 5;
  int q1_box = 6;
  int sub_ulp_x = 7;
  int sub_ulp_y = 8;

  // Establish a subdivided Q3 quadrant ([1, 2] -> [1, 1.5] -> [1, 1.25]).
  const Envelope box_env(1.0, 1.5, 1.0, 1.5);
  const Envelope sub_env(1.1, 1.2, 1.1, 1.2);
  root.insert(&box_env, &box);
  root.insert(&sub_env, &sub_box);
  const std::size_t q3_depth = root.depth();
  EXPECT_EQ(q3_depth, 4u);

  // Point envelope (isZeroX && isZeroY) uses tree->find without infinite loop.
  const Envelope point_env(1.15, 1.15, 1.15, 1.15);
  root.insert(&point_env, &point);

  // Vertical line segment (isZeroX && !isZeroY).
  const Envelope vert_env(1.25, 1.25, 1.0, 1.5);
  root.insert(&vert_env, &vertical_seg);

  // Horizontal line segment (!isZeroX && isZeroY).
  const Envelope horiz_env(1.0, 1.5, 1.25, 1.25);
  root.insert(&horiz_env, &horizontal_seg);

  // Establish an independent Q1 quadrant ([1024, 1026] -> [1024, 1025] x
  // [-2, -1]) and insert positive widths below 2^-50 relative to magnitude.
  const Envelope q1_env(1024.0, 1025.0, -2.0, -1.0);
  root.insert(&q1_env, &q1_box);
  EXPECT_EQ(root.depth(), q3_depth);

  // Positive X width with relative size < 2^-50 (isZeroX && !isZeroY).
  const Envelope tiny_rel_x(1024.0, 1024.0 + std::ldexp(1.0, -40), -2.0, -1.75);
  ASSERT_GT(tiny_rel_x.getWidth(), 0.0);
  root.insert(&tiny_rel_x, &sub_ulp_x);

  // Positive Y width with relative size <= 2^-50 (!isZeroX && isZeroY).
  const Envelope tiny_rel_y(1024.0, 1024.25, -2.0, -2.0 + std::ldexp(1.0, -50));
  ASSERT_GT(tiny_rel_y.getHeight(), 0.0);
  root.insert(&tiny_rel_y, &sub_ulp_y);

  // Near-zero-width envelopes route through Node::find() rather than
  // subdividing dozens of levels down.
  EXPECT_EQ(root.size(), 8u);
  EXPECT_EQ(root.depth(), q3_depth);

  std::vector<void*> near_tiny;
  root.addAllItemsFromOverlapping(Envelope(1024.0, 1025.0, -2.0, -1.0),
                                  near_tiny);
  EXPECT_THAT(near_tiny, UnorderedElementsAre(&q1_box, &sub_ulp_x, &sub_ulp_y));

  std::vector<void*> local_hits;
  root.addAllItemsFromOverlapping(Envelope(1.1, 1.3, 1.1, 1.3), local_hits);
  EXPECT_THAT(local_hits, UnorderedElementsAre(&box, &sub_box, &point,
                                               &vertical_seg, &horizontal_seg));

  std::vector<void*> parent_quad_hits;
  root.addAllItemsFromOverlapping(Envelope(1.3, 1.4, 1.3, 1.4),
                                  parent_quad_hits);
  EXPECT_THAT(parent_quad_hits,
              UnorderedElementsAre(&box, &vertical_seg, &horizontal_seg));
}

TEST(RootTest, RemoveItemsFromRootAndQuadrantsPrunesTree) {
  Root root;
  int root_item = 1;
  int q2_item = 2;

  const Envelope crossing(-1.0, 1.0, -1.0, 1.0);
  const Envelope q2_env(-3.0, -2.0, 2.0, 3.0);
  root.insert(&crossing, &root_item);
  root.insert(&q2_env, &q2_item);
  EXPECT_EQ(root.size(), 2u);

  EXPECT_TRUE(root.remove(&q2_env, &q2_item));
  EXPECT_FALSE(root.hasChildren());
  EXPECT_EQ(root.size(), 1u);

  EXPECT_TRUE(root.remove(&crossing, &root_item));
  EXPECT_FALSE(root.hasItems());
  EXPECT_TRUE(root.isPrunable());
  EXPECT_EQ(root.size(), 0u);

  std::vector<void*> remaining;
  root.addAllItems(remaining);
  EXPECT_THAT(remaining, IsEmpty());
}

TEST(RootTest, OriginAndAxisTouchingEnvelopesRouteCorrectly) {
  {
    Root origin_only;
    int origin_item = 1;
    const Envelope origin_pt(0.0, 0.0, 0.0, 0.0);
    origin_only.insert(&origin_pt, &origin_item);

    EXPECT_FALSE(origin_only.hasItems());
    EXPECT_TRUE(origin_only.hasChildren());
    EXPECT_EQ(origin_only.size(), 1u);

    std::vector<void*> hits;
    origin_only.addAllItemsFromOverlapping(origin_pt, hits);
    EXPECT_THAT(hits, UnorderedElementsAre(&origin_item));
  }

  Root root;
  int q0 = 0;
  int q1 = 1;
  int q2 = 2;
  int q3 = 3;
  int axis_crossing = 4;

  // Envelopes touching the axes without crossing either axis belong to a
  // single quadrant.
  const Envelope env0(-2.0, 0.0, -2.0, 0.0);
  const Envelope env1(0.0, 2.0, -2.0, 0.0);
  const Envelope env2(-2.0, 0.0, 0.0, 2.0);
  const Envelope env3(0.0, 2.0, 0.0, 2.0);
  root.insert(&env0, &q0);
  root.insert(&env1, &q1);
  root.insert(&env2, &q2);
  root.insert(&env3, &q3);

  EXPECT_FALSE(root.hasItems());
  EXPECT_TRUE(root.hasChildren());
  EXPECT_EQ(root.size(), 4u);

  // Degenerate segment along Y=0 crossing X=0 belongs directly on root.
  const Envelope env_cross_y_at_zero(-1.0, 1.0, 0.0, 0.0);
  root.insert(&env_cross_y_at_zero, &axis_crossing);
  EXPECT_TRUE(root.hasItems());
  EXPECT_EQ(root.size(), 5u);
  EXPECT_THAT(root.getItems(), UnorderedElementsAre(&axis_crossing));
}

TEST(RootTest, PolymorphicDestructionAndDuplicateHandling) {
  auto root = std::make_unique<Root>();
  int dup_item = 1;
  int outer_item = 2;
  int missing_item = 3;

  const Envelope q0_env(-3.0, -2.0, -3.0, -2.0);
  const Envelope q0_outer(-100.0, -99.0, -100.0, -99.0);
  root->insert(&q0_env, &dup_item);
  root->insert(&q0_env, &dup_item);
  root->insert(&q0_outer, &outer_item);

  EXPECT_EQ(root->size(), 3u);
  EXPECT_GT(root->getNodeCount(), 1u);
  EXPECT_FALSE(root->toString().empty());

  EXPECT_FALSE(root->remove(&q0_env, &missing_item));
  EXPECT_TRUE(root->remove(&q0_env, &dup_item));
  EXPECT_EQ(root->size(), 2u);
  EXPECT_TRUE(root->remove(&q0_env, &dup_item));
  EXPECT_EQ(root->size(), 1u);
  EXPECT_FALSE(root->remove(&q0_env, &dup_item));

  std::unique_ptr<NodeBase> base_root = std::move(root);
  EXPECT_EQ(base_root->size(), 1u);
  base_root.reset();
}

}  // namespace
}  // namespace geos_google
