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

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/strtree/Interval.h"
// <cassert> must precede TemplateSTRNode.h because getItem() uses assert()
// without including <cassert>. Keep this include below system headers.
// clang-format off
#include "third_party/geos/include/geos/index/strtree/TemplateSTRNode.h"
// clang-format on
#include "third_party/geos/include/geos/index/strtree/TemplateSTRtree.h"

namespace geos_google {
namespace {

using EnvelopeNode =
    geos::index::strtree::TemplateSTRNode<int,
                                          geos::index::strtree::EnvelopeTraits>;
using IntervalNode =
    geos::index::strtree::TemplateSTRNode<int,
                                          geos::index::strtree::IntervalTraits>;

struct TrackedTrivialItem {
  int id;
  int* copies;
  int* moves;

  TrackedTrivialItem(int id_in, int* copies_in, int* moves_in)
      : id(id_in), copies(copies_in), moves(moves_in) {}

  TrackedTrivialItem(const TrackedTrivialItem& other)
      : id(other.id), copies(other.copies), moves(other.moves) {
    if (copies != nullptr) {
      ++(*copies);
    }
  }

  TrackedTrivialItem(TrackedTrivialItem&& other) noexcept
      : id(other.id), copies(other.copies), moves(other.moves) {
    if (moves != nullptr) {
      ++(*moves);
    }
  }

  TrackedTrivialItem& operator=(const TrackedTrivialItem&) = default;
  TrackedTrivialItem& operator=(TrackedTrivialItem&&) = default;
  ~TrackedTrivialItem() = default;
};

static_assert(std::is_trivially_destructible_v<TrackedTrivialItem>);

struct Range1D {
  double min_val;
  double max_val;
};

struct Range1DTraits {
  using BoundsType = Range1D;

  static bool intersects(const BoundsType& a, const BoundsType& b) {
    return a.min_val <= b.max_val && a.max_val >= b.min_val;
  }

  static double size(const BoundsType& a) { return a.max_val - a.min_val; }

  static void expandToInclude(BoundsType& a, const BoundsType& b) {
    a.min_val = std::min(a.min_val, b.min_val);
    a.max_val = std::max(a.max_val, b.max_val);
  }
};

TEST(TemplateSTRNodeTest, ConstructLeafFromLvalueAndRvalue) {
  using TrackedNode = geos::index::strtree::TemplateSTRNode<
      TrackedTrivialItem, geos::index::strtree::EnvelopeTraits>;

  int copies = 0;
  int moves = 0;
  const TrackedTrivialItem lval_item(11, &copies, &moves);
  const geos::geom::Envelope env1(0.0, 2.0, 0.0, 3.0);
  const geos::geom::Envelope env2(4.0, 5.0, 1.0, 4.0);

  TrackedNode leaf_from_lvalue(lval_item, env1);
  EXPECT_EQ(copies, 1);
  EXPECT_EQ(moves, 0);
  EXPECT_TRUE(leaf_from_lvalue.isLeaf());
  EXPECT_FALSE(leaf_from_lvalue.isComposite());
  EXPECT_FALSE(leaf_from_lvalue.isDeleted());
  EXPECT_EQ(leaf_from_lvalue.getItem().id, 11);
  EXPECT_EQ(leaf_from_lvalue.getBounds(), env1);
  EXPECT_DOUBLE_EQ(leaf_from_lvalue.getSize(), 6.0);
  EXPECT_EQ(leaf_from_lvalue.getNumNodes(), std::size_t{1});
  EXPECT_EQ(leaf_from_lvalue.getNumLeafNodes(), std::size_t{1});

  TrackedNode leaf_from_rvalue(TrackedTrivialItem(22, &copies, &moves), env2);
  EXPECT_EQ(copies, 1);
  EXPECT_EQ(moves, 1);
  EXPECT_TRUE(leaf_from_rvalue.isLeaf());
  EXPECT_FALSE(leaf_from_rvalue.isComposite());
  EXPECT_FALSE(leaf_from_rvalue.isDeleted());
  EXPECT_EQ(leaf_from_rvalue.getItem().id, 22);
  EXPECT_EQ(leaf_from_rvalue.getBounds(), env2);
  EXPECT_DOUBLE_EQ(leaf_from_rvalue.getSize(), 3.0);

  // Also exercise composite node destruction (~TemplateSTRNode with isLeaf() ==
  // false) for non-trivially-copyable ItemType.
  TrackedNode single_composite(&leaf_from_lvalue, &leaf_from_lvalue + 1);
  EXPECT_FALSE(single_composite.isLeaf());
  EXPECT_TRUE(single_composite.isComposite());
  EXPECT_FALSE(single_composite.isDeleted());
  EXPECT_EQ(single_composite.getBounds(), env1);
  EXPECT_EQ(single_composite.getNumNodes(), std::size_t{2});
}

TEST(TemplateSTRNodeTest, LeafLifecycleAndRemoveItem) {
  const geos::geom::Envelope box_env(0.0, 4.0, 0.0, 5.0);
  const geos::geom::Envelope pt_env(geos::geom::CoordinateXY(2.0, 3.0));

  EnvelopeNode box_leaf(42, box_env);
  EnvelopeNode pt_leaf(7, pt_env);

  EXPECT_TRUE(box_leaf.isLeaf());
  EXPECT_FALSE(box_leaf.isComposite());
  EXPECT_FALSE(box_leaf.isDeleted());
  EXPECT_EQ(box_leaf.getItem(), 42);
  EXPECT_EQ(box_leaf.getBounds(), box_env);
  EXPECT_DOUBLE_EQ(box_leaf.getSize(), 20.0);
  EXPECT_DOUBLE_EQ(pt_leaf.getSize(), 0.0);
  EXPECT_EQ(box_leaf.getNumNodes(), std::size_t{1});
  EXPECT_EQ(box_leaf.getNumLeafNodes(), std::size_t{1});

  const EnvelopeNode& const_pt = pt_leaf;
  EXPECT_EQ(const_pt.getItem(), 7);

  box_leaf.removeItem();
  EXPECT_TRUE(box_leaf.isDeleted());
  EXPECT_TRUE(box_leaf.isLeaf());
  EXPECT_FALSE(box_leaf.isComposite());
  EXPECT_EQ(box_leaf.getNumNodes(), std::size_t{0});
  EXPECT_EQ(box_leaf.getNumLeafNodes(), std::size_t{0});
  EXPECT_EQ(box_leaf.getBounds(), box_env);

  // Removing an already-deleted leaf is idempotent.
  box_leaf.removeItem();
  EXPECT_TRUE(box_leaf.isDeleted());
  EXPECT_TRUE(box_leaf.isLeaf());
  EXPECT_EQ(box_leaf.getNumNodes(), std::size_t{0});
  EXPECT_EQ(box_leaf.getNumLeafNodes(), std::size_t{0});
}

TEST(TemplateSTRNodeTest, CompositeNodeSingleAndMultipleChildren) {
  std::vector<EnvelopeNode> leaves;
  leaves.reserve(4);
  leaves.emplace_back(1, geos::geom::Envelope(0.0, 2.0, 0.0, 2.0));
  leaves.emplace_back(2, geos::geom::Envelope(5.0, 7.0, 1.0, 3.0));
  leaves.emplace_back(3, geos::geom::Envelope(-3.0, -1.0, 4.0, 6.0));
  leaves.emplace_back(4, geos::geom::Envelope(1.0, 4.0, -2.0, 0.0));

  EnvelopeNode single_child_comp(leaves.data(), leaves.data() + 1);
  EXPECT_FALSE(single_child_comp.isLeaf());
  EXPECT_TRUE(single_child_comp.isComposite());
  EXPECT_FALSE(single_child_comp.isDeleted());
  EXPECT_EQ(single_child_comp.beginChildren(), leaves.data());
  EXPECT_EQ(single_child_comp.endChildren(), leaves.data() + 1);
  EXPECT_EQ(single_child_comp.getBounds(),
            geos::geom::Envelope(0.0, 2.0, 0.0, 2.0));
  EXPECT_EQ(single_child_comp.boundsFromChildren(),
            single_child_comp.getBounds());
  EXPECT_DOUBLE_EQ(single_child_comp.getSize(), 4.0);
  EXPECT_EQ(single_child_comp.getNumNodes(), std::size_t{2});
  EXPECT_EQ(single_child_comp.getNumLeafNodes(), std::size_t{1});

  const geos::geom::Envelope pair_bounds =
      EnvelopeNode::boundsFromChildren(leaves.data(), leaves.data() + 2);
  EXPECT_EQ(pair_bounds, geos::geom::Envelope(0.0, 7.0, 0.0, 3.0));

  EnvelopeNode multi_comp(leaves.data(), leaves.data() + leaves.size());
  EXPECT_FALSE(multi_comp.isLeaf());
  EXPECT_TRUE(multi_comp.isComposite());
  EXPECT_FALSE(multi_comp.isDeleted());
  EXPECT_EQ(multi_comp.beginChildren(), leaves.data());
  EXPECT_EQ(multi_comp.endChildren(), leaves.data() + leaves.size());

  const geos::geom::Envelope expected_full(-3.0, 7.0, -2.0, 6.0);
  EXPECT_EQ(multi_comp.getBounds(), expected_full);
  EXPECT_EQ(multi_comp.boundsFromChildren(), expected_full);
  EXPECT_DOUBLE_EQ(multi_comp.getSize(), 80.0);
  EXPECT_EQ(multi_comp.getNumNodes(), std::size_t{5});
  EXPECT_EQ(multi_comp.getNumLeafNodes(), std::size_t{4});

  leaves[1].removeItem();
  leaves[3].removeItem();
  EXPECT_EQ(multi_comp.getNumNodes(), std::size_t{3});
  EXPECT_EQ(multi_comp.getNumLeafNodes(), std::size_t{2});
  EXPECT_EQ(multi_comp.getBounds(), expected_full);
  EXPECT_EQ(multi_comp.boundsFromChildren(), expected_full);
}

TEST(TemplateSTRNodeTest, MultiLevelCompositeTreeAndPartialDeletion) {
  std::vector<EnvelopeNode> nodes;
  nodes.reserve(7);

  nodes.emplace_back(10, geos::geom::Envelope(0.0, 1.0, 0.0, 1.0));
  nodes.emplace_back(20, geos::geom::Envelope(2.0, 3.0, 0.0, 1.0));
  nodes.emplace_back(30, geos::geom::Envelope(10.0, 12.0, 5.0, 7.0));
  nodes.emplace_back(40, geos::geom::Envelope(14.0, 15.0, 8.0, 10.0));

  nodes.emplace_back(nodes.data(), nodes.data() + 2);
  nodes.emplace_back(nodes.data() + 2, nodes.data() + 4);
  nodes.emplace_back(nodes.data() + 4, nodes.data() + 6);

  const EnvelopeNode& left_branch = nodes[4];
  const EnvelopeNode& right_branch = nodes[5];
  const EnvelopeNode& root = nodes[6];

  EXPECT_EQ(left_branch.getBounds(), geos::geom::Envelope(0.0, 3.0, 0.0, 1.0));
  EXPECT_EQ(right_branch.getBounds(),
            geos::geom::Envelope(10.0, 15.0, 5.0, 10.0));
  EXPECT_EQ(root.getBounds(), geos::geom::Envelope(0.0, 15.0, 0.0, 10.0));
  EXPECT_EQ(root.getNumNodes(), std::size_t{7});
  EXPECT_EQ(left_branch.getNumLeafNodes(), std::size_t{2});
  EXPECT_EQ(right_branch.getNumLeafNodes(), std::size_t{2});

  nodes[0].removeItem();
  EXPECT_EQ(left_branch.getNumNodes(), std::size_t{2});
  EXPECT_EQ(left_branch.getNumLeafNodes(), std::size_t{1});
  EXPECT_EQ(root.getNumNodes(), std::size_t{6});

  nodes[1].removeItem();
  EXPECT_EQ(left_branch.getNumNodes(), std::size_t{1});
  EXPECT_EQ(left_branch.getNumLeafNodes(), std::size_t{0});
  EXPECT_EQ(root.getNumNodes(), std::size_t{5});

  nodes[2].removeItem();
  nodes[3].removeItem();
  EXPECT_EQ(right_branch.getNumNodes(), std::size_t{1});
  EXPECT_EQ(right_branch.getNumLeafNodes(), std::size_t{0});
  EXPECT_EQ(root.getNumNodes(), std::size_t{3});
}

TEST(TemplateSTRNodeTest, BoundsIntersectWithOverlapTouchDisjointAndNull) {
  std::vector<EnvelopeNode> nodes;
  nodes.reserve(3);
  nodes.emplace_back(1, geos::geom::Envelope(0.0, 4.0, 0.0, 4.0));
  nodes.emplace_back(2, geos::geom::Envelope(6.0, 8.0, 6.0, 8.0));
  nodes.emplace_back(nodes.data(), nodes.data() + 2);

  const EnvelopeNode& leaf = nodes[0];
  const EnvelopeNode& composite = nodes[2];

  EXPECT_TRUE(leaf.boundsIntersect(geos::geom::Envelope(2.0, 5.0, 2.0, 5.0)));
  EXPECT_TRUE(leaf.boundsIntersect(geos::geom::Envelope(4.0, 6.0, 4.0, 6.0)));
  EXPECT_FALSE(leaf.boundsIntersect(geos::geom::Envelope(4.1, 6.0, 0.0, 4.0)));
  EXPECT_FALSE(leaf.boundsIntersect(geos::geom::Envelope()));

  EXPECT_TRUE(
      composite.boundsIntersect(geos::geom::Envelope(4.5, 5.5, 4.5, 5.5)));
  EXPECT_TRUE(
      composite.boundsIntersect(geos::geom::Envelope(8.0, 9.0, 8.0, 9.0)));
  EXPECT_FALSE(
      composite.boundsIntersect(geos::geom::Envelope(-2.0, -0.1, 0.0, 4.0)));
  EXPECT_FALSE(composite.boundsIntersect(geos::geom::Envelope()));
}

TEST(TemplateSTRNodeTest, NullEnvelopeLeavesAndComposites) {
  const geos::geom::Envelope null_env;
  const geos::geom::Envelope box_env(1.0, 3.0, 2.0, 5.0);

  std::vector<EnvelopeNode> nodes;
  nodes.reserve(7);
  nodes.emplace_back(1, null_env);
  nodes.emplace_back(2, null_env);
  nodes.emplace_back(3, box_env);
  nodes.emplace_back(4, null_env);

  nodes.emplace_back(nodes.data(), nodes.data() + 2);
  nodes.emplace_back(nodes.data() + 1, nodes.data() + 3);
  nodes.emplace_back(nodes.data() + 2, nodes.data() + 4);

  const EnvelopeNode& null_leaf = nodes[0];
  EXPECT_TRUE(null_leaf.isLeaf());
  EXPECT_TRUE(null_leaf.getBounds().isNull());
  EXPECT_DOUBLE_EQ(null_leaf.getSize(), 0.0);
  EXPECT_FALSE(null_leaf.boundsIntersect(box_env));
  EXPECT_FALSE(null_leaf.boundsIntersect(null_env));
  EXPECT_EQ(null_leaf.getNumNodes(), std::size_t{1});
  EXPECT_EQ(null_leaf.getNumLeafNodes(), std::size_t{1});

  const EnvelopeNode& all_null_comp = nodes[4];
  EXPECT_TRUE(all_null_comp.isComposite());
  EXPECT_TRUE(all_null_comp.getBounds().isNull());
  EXPECT_TRUE(all_null_comp.boundsFromChildren().isNull());
  EXPECT_DOUBLE_EQ(all_null_comp.getSize(), 0.0);
  EXPECT_FALSE(all_null_comp.boundsIntersect(box_env));
  EXPECT_EQ(all_null_comp.getNumNodes(), std::size_t{3});
  EXPECT_EQ(all_null_comp.getNumLeafNodes(), std::size_t{2});

  const EnvelopeNode& null_first_comp = nodes[5];
  EXPECT_EQ(null_first_comp.getBounds(), box_env);
  EXPECT_DOUBLE_EQ(null_first_comp.getSize(), 6.0);
  EXPECT_TRUE(null_first_comp.boundsIntersect(
      geos::geom::Envelope(2.0, 4.0, 3.0, 4.0)));

  const EnvelopeNode& box_first_comp = nodes[6];
  EXPECT_EQ(box_first_comp.getBounds(), box_env);
  EXPECT_DOUBLE_EQ(box_first_comp.getSize(), 6.0);
}

TEST(TemplateSTRNodeTest, PointerItemsAndIntervalTraits) {
  const std::string tag_a = "alpha";
  const std::string tag_b = "beta";
  using StringPtrNode = geos::index::strtree::TemplateSTRNode<
      const std::string*, geos::index::strtree::EnvelopeTraits>;

  std::vector<StringPtrNode> str_nodes;
  str_nodes.reserve(3);
  str_nodes.emplace_back(&tag_a, geos::geom::Envelope(0.0, 1.0, 0.0, 1.0));
  str_nodes.emplace_back(&tag_b, geos::geom::Envelope(2.0, 3.0, 2.0, 3.0));
  str_nodes.emplace_back(str_nodes.data(), str_nodes.data() + 2);

  EXPECT_EQ(str_nodes[0].getItem(), &tag_a);
  EXPECT_EQ(*str_nodes[1].getItem(), "beta");
  EXPECT_EQ(str_nodes[2].getNumLeafNodes(), std::size_t{2});

  std::vector<IntervalNode> iv_nodes;
  iv_nodes.reserve(4);
  iv_nodes.emplace_back(1, geos::index::strtree::Interval(1.0, 4.0));
  iv_nodes.emplace_back(2, geos::index::strtree::Interval(6.0, 10.0));
  iv_nodes.emplace_back(3, geos::index::strtree::Interval(4.0, 4.0));
  iv_nodes.emplace_back(iv_nodes.data(), iv_nodes.data() + 3);

  EXPECT_DOUBLE_EQ(iv_nodes[0].getSize(), 3.0);
  EXPECT_DOUBLE_EQ(iv_nodes[1].getSize(), 4.0);
  EXPECT_DOUBLE_EQ(iv_nodes[2].getSize(), 0.0);

  const IntervalNode& iv_comp = iv_nodes[3];
  EXPECT_TRUE(iv_comp.isComposite());
  EXPECT_DOUBLE_EQ(iv_comp.getBounds().getMin(), 1.0);
  EXPECT_DOUBLE_EQ(iv_comp.getBounds().getMax(), 10.0);
  EXPECT_DOUBLE_EQ(iv_comp.getSize(), 9.0);
  EXPECT_TRUE(
      iv_comp.boundsIntersect(geos::index::strtree::Interval(3.5, 6.5)));
  EXPECT_FALSE(
      iv_comp.boundsIntersect(geos::index::strtree::Interval(10.5, 12.0)));
  EXPECT_EQ(iv_comp.getNumNodes(), std::size_t{4});
  EXPECT_EQ(iv_comp.getNumLeafNodes(), std::size_t{3});

  iv_nodes[1].removeItem();
  EXPECT_EQ(iv_comp.getNumLeafNodes(), std::size_t{2});
}

TEST(TemplateSTRNodeTest, CustomBoundsTraitsSupport) {
  using CustomNode = geos::index::strtree::TemplateSTRNode<int, Range1DTraits>;

  std::vector<CustomNode> nodes;
  nodes.reserve(3);
  nodes.emplace_back(100, Range1D{-5.0, -2.0});
  nodes.emplace_back(200, Range1D{3.0, 7.0});
  nodes.emplace_back(nodes.data(), nodes.data() + 2);

  const CustomNode& comp = nodes[2];
  EXPECT_DOUBLE_EQ(comp.getBounds().min_val, -5.0);
  EXPECT_DOUBLE_EQ(comp.getBounds().max_val, 7.0);
  EXPECT_DOUBLE_EQ(comp.boundsFromChildren().min_val, -5.0);
  EXPECT_DOUBLE_EQ(comp.boundsFromChildren().max_val, 7.0);
  EXPECT_DOUBLE_EQ(comp.getSize(), 12.0);
  EXPECT_TRUE(comp.boundsIntersect(Range1D{-1.0, 1.0}));
  EXPECT_FALSE(comp.boundsIntersect(Range1D{8.0, 9.0}));
  EXPECT_EQ(nodes[0].getItem(), 100);
}

}  // namespace
}  // namespace geos_google
