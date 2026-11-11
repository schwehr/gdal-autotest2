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
#include <cmath>
#include <queue>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/strtree/TemplateSTRNode.h"
// <cassert> must precede TemplateSTRNodePair.h because getItems() uses assert()
// without including <cassert>. Keep this include below system headers.
// clang-format off
#include "third_party/geos/include/geos/index/strtree/TemplateSTRNodePair.h"
// clang-format on
#include "third_party/geos/include/geos/index/strtree/TemplateSTRtree.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

struct CountingEuclideanDistance {
  int calls = 0;

  double operator()(const geos::geom::CoordinateXY& a,
                    const geos::geom::CoordinateXY& b) {
    ++calls;
    return a.distance(b);
  }
};

struct CountingManhattanDistance {
  int calls = 0;

  double operator()(const geos::geom::CoordinateXY& a,
                    const geos::geom::CoordinateXY& b) {
    ++calls;
    return std::abs(a.x - b.x) + std::abs(a.y - b.y);
  }
};

struct ValidatingCoordinateDistance {
  bool enforce = true;

  double operator()(const geos::geom::CoordinateXY& a,
                    const geos::geom::CoordinateXY& b) const {
    if (enforce && (!a.isValid() || !b.isValid())) {
      throw geos::util::IllegalArgumentException(
          "Coordinate must be finite for distance computation");
    }
    return a.distance(b);
  }
};

struct CoordPtrDistance {
  double operator()(const geos::geom::CoordinateXY* a,
                    const geos::geom::CoordinateXY* b) const {
    return a->distance(*b);
  }
};

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

  static double distance(const BoundsType& a, const BoundsType& b) {
    if (a.max_val < b.min_val) {
      return b.min_val - a.max_val;
    }
    if (b.max_val < a.min_val) {
      return a.min_val - b.max_val;
    }
    return 0.0;
  }

  static double maxDistance(const BoundsType& a, const BoundsType& b) {
    return std::max(a.max_val, b.max_val) - std::min(a.min_val, b.min_val);
  }
};

struct IntDiffDistance {
  int calls = 0;

  double operator()(int a, int b) {
    ++calls;
    return static_cast<double>(std::abs(a - b));
  }
};

using CoordNode =
    geos::index::strtree::TemplateSTRNode<geos::geom::CoordinateXY,
                                          geos::index::strtree::EnvelopeTraits>;

TEST(TemplateSTRNodePairTest, LeafPairUsesItemDistanceEvenWhenBoundsOverlap) {
  // Overlapping envelopes have bounds distance 0, verifying leaf pairs invoke
  // ItemDistance instead of BoundsTraits::distance.
  const geos::geom::Envelope env1(0.0, 10.0, 0.0, 10.0);
  const geos::geom::Envelope env2(0.0, 10.0, 0.0, 10.0);
  const CoordNode leaf1(geos::geom::CoordinateXY(0.0, 0.0), env1);
  const CoordNode leaf2(geos::geom::CoordinateXY(3.0, 4.0), env2);

  CountingEuclideanDistance euclid;
  geos::index::strtree::TemplateSTRNodePair<
      geos::geom::CoordinateXY, geos::index::strtree::EnvelopeTraits,
      CountingEuclideanDistance>
      pair(leaf1, leaf2, euclid);

  EXPECT_TRUE(pair.isLeaves());
  EXPECT_EQ(euclid.calls, 1);
  EXPECT_DOUBLE_EQ(pair.getDistance(), 5.0);
  EXPECT_EQ(&pair.getFirst(), &leaf1);
  EXPECT_EQ(&pair.getSecond(), &leaf2);

  const std::pair<geos::geom::CoordinateXY, geos::geom::CoordinateXY> items =
      pair.getItems();
  EXPECT_EQ(items.first, geos::geom::CoordinateXY(0.0, 0.0));
  EXPECT_EQ(items.second, geos::geom::CoordinateXY(3.0, 4.0));

  EXPECT_DOUBLE_EQ(pair.maximumDistance(), std::hypot(10.0, 10.0));

  CountingManhattanDistance manhattan;
  geos::index::strtree::TemplateSTRNodePair<
      geos::geom::CoordinateXY, geos::index::strtree::EnvelopeTraits,
      CountingManhattanDistance>
      manhattan_pair(leaf1, leaf2, manhattan);
  EXPECT_DOUBLE_EQ(manhattan_pair.getDistance(), 7.0);
  EXPECT_EQ(manhattan.calls, 1);

  // Calling distance() directly recomputes via the supplied ItemDistance
  // without altering the cached value returned by getDistance().
  CountingEuclideanDistance second_euclid;
  EXPECT_DOUBLE_EQ(pair.distance(second_euclid), 5.0);
  EXPECT_EQ(second_euclid.calls, 1);
  EXPECT_DOUBLE_EQ(pair.getDistance(), 5.0);
}

TEST(TemplateSTRNodePairTest, CompositeCombinationsUseBoundsDistance) {
  std::vector<CoordNode> nodes;
  nodes.reserve(8);

  // comp1 covers [0, 2] x [0, 2].
  nodes.emplace_back(geos::geom::CoordinateXY(0.0, 0.0),
                     geos::geom::Envelope(0.0, 1.0, 0.0, 1.0));
  nodes.emplace_back(geos::geom::CoordinateXY(2.0, 2.0),
                     geos::geom::Envelope(1.0, 2.0, 1.0, 2.0));
  // comp2 covers [5, 8] x [4, 7].
  nodes.emplace_back(geos::geom::CoordinateXY(5.0, 4.0),
                     geos::geom::Envelope(5.0, 6.0, 4.0, 5.0));
  nodes.emplace_back(geos::geom::CoordinateXY(8.0, 7.0),
                     geos::geom::Envelope(7.0, 8.0, 6.0, 7.0));
  // Separate leaves.
  nodes.emplace_back(geos::geom::CoordinateXY(5.5, 4.5),
                     geos::geom::Envelope(5.0, 6.0, 4.0, 5.0));
  nodes.emplace_back(geos::geom::CoordinateXY(3.0, 3.0),
                     geos::geom::Envelope(2.0, 4.0, 2.0, 4.0));

  nodes.emplace_back(&nodes[0], &nodes[2]);
  nodes.emplace_back(&nodes[2], &nodes[4]);

  const CoordNode& far_leaf = nodes[4];
  const CoordNode& touching_leaf = nodes[5];
  const CoordNode& comp1 = nodes[6];
  const CoordNode& comp2 = nodes[7];

  CountingEuclideanDistance euclid;
  using Pair = geos::index::strtree::TemplateSTRNodePair<
      geos::geom::CoordinateXY, geos::index::strtree::EnvelopeTraits,
      CountingEuclideanDistance>;

  // (leaf, composite)
  Pair leaf_comp(far_leaf, comp1, euclid);
  EXPECT_FALSE(leaf_comp.isLeaves());
  EXPECT_EQ(euclid.calls, 0);
  EXPECT_DOUBLE_EQ(leaf_comp.getDistance(), std::hypot(3.0, 2.0));
  EXPECT_DOUBLE_EQ(leaf_comp.distance(euclid), std::hypot(3.0, 2.0));
  EXPECT_EQ(euclid.calls, 0);
  EXPECT_DOUBLE_EQ(leaf_comp.maximumDistance(), std::hypot(6.0, 5.0));
  EXPECT_EQ(&leaf_comp.getFirst(), &far_leaf);
  EXPECT_EQ(&leaf_comp.getSecond(), &comp1);

  // (composite, leaf)
  Pair comp_leaf(comp1, far_leaf, euclid);
  EXPECT_FALSE(comp_leaf.isLeaves());
  EXPECT_EQ(euclid.calls, 0);
  EXPECT_DOUBLE_EQ(comp_leaf.getDistance(), std::hypot(3.0, 2.0));
  EXPECT_DOUBLE_EQ(comp_leaf.maximumDistance(), std::hypot(6.0, 5.0));

  // Touching (composite, leaf) has bounds distance 0.
  Pair comp_touching(comp1, touching_leaf, euclid);
  EXPECT_FALSE(comp_touching.isLeaves());
  EXPECT_EQ(euclid.calls, 0);
  EXPECT_DOUBLE_EQ(comp_touching.getDistance(), 0.0);
  EXPECT_DOUBLE_EQ(comp_touching.maximumDistance(), std::hypot(4.0, 4.0));

  // (composite, composite)
  Pair comp_comp(comp1, comp2, euclid);
  EXPECT_FALSE(comp_comp.isLeaves());
  EXPECT_EQ(euclid.calls, 0);
  EXPECT_DOUBLE_EQ(comp_comp.getDistance(), std::hypot(3.0, 2.0));
  EXPECT_DOUBLE_EQ(comp_comp.distance(euclid), std::hypot(3.0, 2.0));
  EXPECT_EQ(euclid.calls, 0);
  EXPECT_DOUBLE_EQ(comp_comp.maximumDistance(), std::hypot(8.0, 7.0));
}

TEST(TemplateSTRNodePairTest, MaximumDistanceCases) {
  CountingEuclideanDistance euclid;
  using Pair = geos::index::strtree::TemplateSTRNodePair<
      geos::geom::CoordinateXY, geos::index::strtree::EnvelopeTraits,
      CountingEuclideanDistance>;

  const geos::geom::CoordinateXY pt_a(3.0, 4.0);
  const CoordNode pt_node1(pt_a, geos::geom::Envelope(pt_a));
  const CoordNode pt_node2(pt_a, geos::geom::Envelope(pt_a));
  Pair identical_pts(pt_node1, pt_node2, euclid);
  EXPECT_DOUBLE_EQ(identical_pts.maximumDistance(), 0.0);

  const geos::geom::CoordinateXY pt_origin(0.0, 0.0);
  const CoordNode origin_node(pt_origin, geos::geom::Envelope(pt_origin));
  Pair two_pts(origin_node, pt_node1, euclid);
  EXPECT_DOUBLE_EQ(two_pts.maximumDistance(), 5.0);

  const CoordNode box_a(pt_origin, geos::geom::Envelope(0.0, 3.0, 0.0, 4.0));
  const CoordNode box_b(pt_a, geos::geom::Envelope(1.0, 6.0, 2.0, 8.0));
  Pair overlapping_boxes(box_a, box_b, euclid);
  EXPECT_DOUBLE_EQ(overlapping_boxes.maximumDistance(), 10.0);
}

TEST(TemplateSTRNodePairTest, ExceptionPropagationAndPriorityQueueOrdering) {
  const CoordNode valid_leaf(geos::geom::CoordinateXY(1.0, 2.0),
                             geos::geom::Envelope(1.0, 2.0, 2.0, 3.0));
  const CoordNode invalid_leaf(
      geos::geom::CoordinateXY(geos::DoubleNotANumber, 2.0),
      geos::geom::Envelope(0.0, 1.0, 0.0, 1.0));

  ValidatingCoordinateDistance validating_dist{true};
  using ValidatingPair = geos::index::strtree::TemplateSTRNodePair<
      geos::geom::CoordinateXY, geos::index::strtree::EnvelopeTraits,
      ValidatingCoordinateDistance>;

  EXPECT_THROW(static_cast<void>(
                   ValidatingPair(invalid_leaf, valid_leaf, validating_dist)),
               geos::util::IllegalArgumentException);

  ValidatingCoordinateDistance relaxed_dist{false};
  ValidatingPair deferred_pair(invalid_leaf, valid_leaf, relaxed_dist);
  EXPECT_TRUE(std::isnan(deferred_pair.getDistance()));
  EXPECT_THROW(deferred_pair.distance(validating_dist),
               geos::util::IllegalArgumentException);

  const CoordNode composite(&valid_leaf, &valid_leaf + 1);
  ValidatingPair comp_with_invalid(composite, invalid_leaf, validating_dist);
  EXPECT_FALSE(comp_with_invalid.isLeaves());
  EXPECT_DOUBLE_EQ(comp_with_invalid.getDistance(), std::hypot(0.0, 1.0));

  CountingEuclideanDistance euclid;
  using EuclidPair = geos::index::strtree::TemplateSTRNodePair<
      geos::geom::CoordinateXY, geos::index::strtree::EnvelopeTraits,
      CountingEuclideanDistance>;

  const CoordNode leaf_far(geos::geom::CoordinateXY(10.0, 0.0),
                           geos::geom::Envelope(10.0, 10.0, 0.0, 0.0));
  const CoordNode leaf_near(geos::geom::CoordinateXY(2.0, 2.0),
                            geos::geom::Envelope(2.0, 2.0, 2.0, 2.0));

  EuclidPair p_far(valid_leaf, leaf_far, euclid);
  EuclidPair p_near(valid_leaf, leaf_near, euclid);
  EuclidPair p_copy(p_far);
  p_copy = p_near;
  EXPECT_DOUBLE_EQ(p_copy.getDistance(), 1.0);

  auto min_heap_cmp = [](const EuclidPair& a, const EuclidPair& b) {
    return a.getDistance() > b.getDistance();
  };
  std::priority_queue<EuclidPair, std::vector<EuclidPair>,
                      decltype(min_heap_cmp)>
      pq(min_heap_cmp);
  pq.push(p_far);
  pq.push(p_near);

  EXPECT_EQ(&pq.top().getSecond(), &leaf_near);
  pq.pop();
  EXPECT_EQ(&pq.top().getSecond(), &leaf_far);
}

TEST(TemplateSTRNodePairTest, CustomBoundsTraitsAndTreeIntegration) {
  using CustomNode = geos::index::strtree::TemplateSTRNode<int, Range1DTraits>;
  using CustomPair =
      geos::index::strtree::TemplateSTRNodePair<int, Range1DTraits,
                                                IntDiffDistance>;

  std::vector<CustomNode> custom_nodes;
  custom_nodes.reserve(4);
  custom_nodes.emplace_back(10, Range1D{0.0, 2.0});
  custom_nodes.emplace_back(25, Range1D{4.0, 5.0});
  custom_nodes.emplace_back(100, Range1D{10.0, 12.0});
  custom_nodes.emplace_back(&custom_nodes[0], &custom_nodes[2]);

  IntDiffDistance diff_dist;
  CustomPair leaf_pair(custom_nodes[0], custom_nodes[1], diff_dist);
  EXPECT_TRUE(leaf_pair.isLeaves());
  EXPECT_EQ(diff_dist.calls, 1);
  EXPECT_DOUBLE_EQ(leaf_pair.getDistance(), 15.0);
  EXPECT_DOUBLE_EQ(leaf_pair.maximumDistance(), 5.0);

  CustomPair comp_pair(custom_nodes[3], custom_nodes[2], diff_dist);
  EXPECT_FALSE(comp_pair.isLeaves());
  EXPECT_EQ(diff_dist.calls, 1);
  EXPECT_DOUBLE_EQ(comp_pair.getDistance(), 5.0);
  EXPECT_DOUBLE_EQ(comp_pair.maximumDistance(), 12.0);

  geos::geom::CoordinateXY a0(0.0, 0.0);
  geos::geom::CoordinateXY a1(10.0, 10.0);
  geos::geom::CoordinateXY a2(20.0, 20.0);
  geos::geom::CoordinateXY b0(10.0, 11.0);
  geos::geom::CoordinateXY b1(30.0, 30.0);

  geos::index::strtree::TemplateSTRtree<geos::geom::CoordinateXY*> tree1(2);
  tree1.insert(geos::geom::Envelope(a0), &a0);
  tree1.insert(geos::geom::Envelope(a1), &a1);
  tree1.insert(geos::geom::Envelope(a2), &a2);

  geos::index::strtree::TemplateSTRtree<geos::geom::CoordinateXY*> tree2(2);
  tree2.insert(geos::geom::Envelope(b0), &b0);
  tree2.insert(geos::geom::Envelope(b1), &b1);

  CoordPtrDistance ptr_dist;
  auto nn = tree1.nearestNeighbour(tree2, ptr_dist);
  EXPECT_EQ(nn.first, &a1);
  EXPECT_EQ(nn.second, &b0);
  EXPECT_TRUE(tree1.isWithinDistance<CoordPtrDistance>(tree2, 1.5));
  EXPECT_FALSE(tree1.isWithinDistance<CoordPtrDistance>(tree2, 0.5));
}

}  // namespace
}  // namespace geos_google
