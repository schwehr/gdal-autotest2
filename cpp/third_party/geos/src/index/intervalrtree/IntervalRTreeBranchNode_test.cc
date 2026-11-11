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

// Unit tests for
// third_party/geos/src/index/intervalrtree/IntervalRTreeBranchNode.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/intervalrtree/IntervalRTreeBranchNode.cpp

#include "third_party/geos/include/geos/index/intervalrtree/IntervalRTreeBranchNode.h"

#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/intervalrtree/IntervalRTreeLeafNode.h"
#include "third_party/geos/include/geos/index/intervalrtree/IntervalRTreeNode.h"

namespace geos_google {
namespace {

using geos::index::ItemVisitor;
using geos::index::intervalrtree::IntervalRTreeBranchNode;
using geos::index::intervalrtree::IntervalRTreeLeafNode;
using geos::index::intervalrtree::IntervalRTreeNode;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Pair;

class RecordingVisitor final : public ItemVisitor {
 public:
  void visitItem(void* item) override {
    items_.push_back(*static_cast<const int*>(item));
  }

  const std::vector<int>& items() const { return items_; }
  void clear() { items_.clear(); }

 private:
  std::vector<int> items_;
};

class SpyNode final : public IntervalRTreeNode {
 public:
  SpyNode(double p_min, double p_max) : IntervalRTreeNode(p_min, p_max) {}

  void query(double queryMin, double queryMax,
             ItemVisitor* /*visitor*/) const override {
    queries_.emplace_back(queryMin, queryMax);
  }

  const std::vector<std::pair<double, double>>& queries() const {
    return queries_;
  }

 private:
  mutable std::vector<std::pair<double, double>> queries_;
};

class DefaultEmptyNode final : public IntervalRTreeNode {
 public:
  void query(double /*queryMin*/, double /*queryMax*/,
             ItemVisitor* /*visitor*/) const override {}
};

TEST(IntervalRTreeBranchNodeTest,
     ComputesCombinedBoundsRegardlessOfChildOrder) {
  int a = 1;
  int b = 2;
  const IntervalRTreeLeafNode left(1.0, 3.0, &a);
  const IntervalRTreeLeafNode right(5.0, 9.0, &b);

  const IntervalRTreeBranchNode forward(&left, &right);
  EXPECT_DOUBLE_EQ(forward.getMin(), 1.0);
  EXPECT_DOUBLE_EQ(forward.getMax(), 9.0);

  const IntervalRTreeBranchNode reversed(&right, &left);
  EXPECT_DOUBLE_EQ(reversed.getMin(), 1.0);
  EXPECT_DOUBLE_EQ(reversed.getMax(), 9.0);
}

TEST(IntervalRTreeBranchNodeTest,
     ComputesCombinedBoundsForNestedAndOverlapping) {
  int a = 1;
  int b = 2;
  const IntervalRTreeLeafNode outer(-10.0, 20.0, &a);
  const IntervalRTreeLeafNode inner(0.0, 5.0, &b);

  const IntervalRTreeBranchNode outer_first(&outer, &inner);
  EXPECT_DOUBLE_EQ(outer_first.getMin(), -10.0);
  EXPECT_DOUBLE_EQ(outer_first.getMax(), 20.0);

  const IntervalRTreeBranchNode inner_first(&inner, &outer);
  EXPECT_DOUBLE_EQ(inner_first.getMin(), -10.0);
  EXPECT_DOUBLE_EQ(inner_first.getMax(), 20.0);

  const IntervalRTreeLeafNode overlap_a(-4.0, 3.0, &a);
  const IntervalRTreeLeafNode overlap_b(1.0, 8.0, &b);
  const IntervalRTreeBranchNode overlapping(&overlap_a, &overlap_b);
  EXPECT_DOUBLE_EQ(overlapping.getMin(), -4.0);
  EXPECT_DOUBLE_EQ(overlapping.getMax(), 8.0);
}

TEST(IntervalRTreeBranchNodeTest, AbsorbsDefaultEmptyChildNodeBounds) {
  int a = 1;
  const DefaultEmptyNode empty_node;
  const IntervalRTreeLeafNode leaf(2.0, 6.0, &a);

  const IntervalRTreeBranchNode branch(&empty_node, &leaf);
  EXPECT_DOUBLE_EQ(branch.getMin(), 2.0);
  EXPECT_DOUBLE_EQ(branch.getMax(), 6.0);

  RecordingVisitor visitor;
  branch.query(3.0, 4.0, &visitor);
  EXPECT_THAT(visitor.items(), ElementsAre(1));
}

TEST(IntervalRTreeBranchNodeTest,
     PrunesDisjointQueriesWithoutVisitingChildren) {
  const SpyNode n1(1.0, 3.0);
  const SpyNode n2(7.0, 9.0);
  const IntervalRTreeBranchNode branch(&n1, &n2);
  RecordingVisitor visitor;

  branch.query(-2.0, 0.5, &visitor);
  branch.query(9.5, 12.0, &visitor);

  EXPECT_THAT(n1.queries(), IsEmpty());
  EXPECT_THAT(n2.queries(), IsEmpty());
  EXPECT_THAT(visitor.items(), IsEmpty());
}

TEST(IntervalRTreeBranchNodeTest, DispatchesIntersectingQueryToBothChildren) {
  const SpyNode n1(1.0, 3.0);
  const SpyNode n2(7.0, 9.0);
  const IntervalRTreeBranchNode branch(&n1, &n2);
  RecordingVisitor visitor;

  // Query lies in the gap [4, 6], which intersects branch [1, 9].
  branch.query(4.0, 6.0, &visitor);
  // Query touches branch minimum [1, 1].
  branch.query(1.0, 1.0, &visitor);
  // Query touches branch maximum [9, 9].
  branch.query(9.0, 9.0, &visitor);

  EXPECT_THAT(n1.queries(),
              ElementsAre(Pair(4.0, 6.0), Pair(1.0, 1.0), Pair(9.0, 9.0)));
  EXPECT_THAT(n2.queries(),
              ElementsAre(Pair(4.0, 6.0), Pair(1.0, 1.0), Pair(9.0, 9.0)));
}

TEST(IntervalRTreeBranchNodeTest, QuerySelectsMatchingLeaves) {
  int first = 10;
  int second = 20;
  const IntervalRTreeLeafNode left(1.0, 3.0, &first);
  const IntervalRTreeLeafNode right(7.0, 9.0, &second);
  const IntervalRTreeBranchNode branch(&left, &right);
  RecordingVisitor visitor;

  branch.query(0.0, 0.5, &visitor);
  EXPECT_THAT(visitor.items(), IsEmpty());

  visitor.clear();
  branch.query(4.0, 6.0, &visitor);
  EXPECT_THAT(visitor.items(), IsEmpty());

  visitor.clear();
  branch.query(2.0, 5.0, &visitor);
  EXPECT_THAT(visitor.items(), ElementsAre(10));

  visitor.clear();
  branch.query(5.0, 8.0, &visitor);
  EXPECT_THAT(visitor.items(), ElementsAre(20));

  visitor.clear();
  branch.query(3.0, 7.0, &visitor);
  EXPECT_THAT(visitor.items(), ElementsAre(10, 20));
}

TEST(IntervalRTreeBranchNodeTest, MultiLevelHierarchyAndPolymorphicDelete) {
  int i1 = 1;
  int i2 = 2;
  int i3 = 3;
  int i4 = 4;
  const IntervalRTreeLeafNode l1(-10.0, -5.0, &i1);
  const IntervalRTreeLeafNode l2(-3.0, 0.0, &i2);
  const IntervalRTreeLeafNode l3(2.0, 6.0, &i3);
  const IntervalRTreeLeafNode l4(8.0, 12.0, &i4);

  const IntervalRTreeBranchNode b_left(&l1, &l2);
  const IntervalRTreeBranchNode b_right(&l3, &l4);
  std::unique_ptr<IntervalRTreeNode> root =
      std::make_unique<IntervalRTreeBranchNode>(&b_left, &b_right);

  EXPECT_DOUBLE_EQ(root->getMin(), -10.0);
  EXPECT_DOUBLE_EQ(root->getMax(), 12.0);

  RecordingVisitor visitor;
  root->query(-4.0, 3.0, &visitor);
  EXPECT_THAT(visitor.items(), ElementsAre(2, 3));

  visitor.clear();
  root->query(geos::DoubleNegInfinity, geos::DoubleInfinity, &visitor);
  EXPECT_THAT(visitor.items(), ElementsAre(1, 2, 3, 4));
}

}  // namespace
}  // namespace geos_google
