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

// Unit tests for third_party/geos/src/index/strtree/SimpleSTRdistance.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - SimpleSTRdistance stores raw pointers to caller-owned SimpleSTRnode and
//   ItemDistance instances, which must outlive the SimpleSTRdistance object.

#include "third_party/geos/include/geos/index/strtree/SimpleSTRdistance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
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
#include "third_party/geos/include/geos/index/strtree/GeometryItemDistance.h"
#include "third_party/geos/include/geos/index/strtree/ItemBoundable.h"
#include "third_party/geos/include/geos/index/strtree/ItemDistance.h"
#include "third_party/geos/include/geos/index/strtree/SimpleSTRnode.h"
#include "third_party/geos/include/geos/index/strtree/SimpleSTRtree.h"
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
using geos::index::strtree::GeometryItemDistance;
using geos::index::strtree::ItemBoundable;
using geos::index::strtree::ItemDistance;
using geos::index::strtree::SimpleSTRdistance;
using geos::index::strtree::SimpleSTRnode;
using geos::index::strtree::SimpleSTRpair;
using geos::index::strtree::SimpleSTRtree;
using geos::util::GEOSException;

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

class EnvelopeItemDistance final : public ItemDistance {
 public:
  explicit EnvelopeItemDistance(
      std::optional<double> same_item_distance = std::nullopt)
      : same_item_distance_(same_item_distance) {}

  double distance(const ItemBoundable* item1,
                  const ItemBoundable* item2) override {
    if (same_item_distance_.has_value() &&
        item1->getItem() == item2->getItem()) {
      return *same_item_distance_;
    }
    const auto* e1 = static_cast<const Envelope*>(item1->getBounds());
    const auto* e2 = static_cast<const Envelope*>(item2->getBounds());
    return e1->distance(*e2);
  }

 private:
  std::optional<double> same_item_distance_;
};

class FixedValueItemDistance final : public ItemDistance {
 public:
  explicit FixedValueItemDistance(double value) : value_(value) {}

  double distance(const ItemBoundable* /*item1*/,
                  const ItemBoundable* /*item2*/) override {
    return value_;
  }

 private:
  double value_;
};

TEST(SimpleSTRdistanceTest, PairAccessorsDistanceMaximumDistanceAndStream) {
  int item1 = 1;
  int item2 = 2;
  const Envelope env1(0.0, 1.0, 0.0, 1.0);
  const Envelope env2(4.0, 5.0, 0.0, 1.0);

  SimpleSTRnode leaf1(0, &env1, &item1);
  SimpleSTRnode leaf2(0, &env2, &item2);
  SimpleSTRnode comp1(1);
  comp1.addChildNode(&leaf1);
  SimpleSTRnode comp2(1);
  comp2.addChildNode(&leaf2);

  FixedValueItemDistance fixed_dist(42.5);

  // Leaf-leaf pair uses ItemDistance rather than Envelope::distance.
  SimpleSTRpair leaf_pair(&leaf1, &leaf2, &fixed_dist);
  EXPECT_EQ(leaf_pair.getNode(0), &leaf1);
  EXPECT_EQ(leaf_pair.getNode(1), &leaf2);
  EXPECT_EQ(leaf_pair.getNode(2), &leaf2);
  EXPECT_TRUE(leaf_pair.isLeaves());
  EXPECT_DOUBLE_EQ(leaf_pair.getDistance(), 42.5);
  EXPECT_DOUBLE_EQ(leaf_pair.maximumDistance(), std::hypot(5.0, 1.0));

  // Any pair with at least one composite node uses Envelope::distance (3.0).
  SimpleSTRpair leaf_comp(&leaf1, &comp2, &fixed_dist);
  EXPECT_FALSE(leaf_comp.isLeaves());
  EXPECT_DOUBLE_EQ(leaf_comp.getDistance(), 3.0);

  SimpleSTRpair comp_leaf(&comp1, &leaf2, &fixed_dist);
  EXPECT_FALSE(comp_leaf.isLeaves());
  EXPECT_DOUBLE_EQ(comp_leaf.getDistance(), 3.0);

  SimpleSTRpair comp_comp(&comp1, &comp2, &fixed_dist);
  EXPECT_FALSE(comp_comp.isLeaves());
  EXPECT_DOUBLE_EQ(comp_comp.getDistance(), 3.0);

  EnvelopeItemDistance env_dist;
  SimpleSTRpair printable(&leaf1, &leaf2, &env_dist);
  std::ostringstream oss;
  oss << printable;
  EXPECT_EQ(oss.str(), "Env[0:1,0:1] Env[4:5,0:1] 3");
}

TEST(SimpleSTRdistanceTest, PriorityQueueOrdersSmallestDistanceFirst) {
  int item = 0;
  const Envelope e0(0.0, 1.0, 0.0, 1.0);
  const Envelope e_mid(6.0, 7.0, 0.0, 1.0);    // distance 5.0
  const Envelope e_near(3.0, 4.0, 0.0, 1.0);   // distance 2.0
  const Envelope e_far(11.0, 12.0, 0.0, 1.0);  // distance 10.0

  SimpleSTRnode n0(0, &e0, &item);
  SimpleSTRnode n_mid(0, &e_mid, &item);
  SimpleSTRnode n_near(0, &e_near, &item);
  SimpleSTRnode n_far(0, &e_far, &item);

  EnvelopeItemDistance dist;
  SimpleSTRpair p_far(&n0, &n_far, &dist);
  SimpleSTRpair p_near(&n0, &n_near, &dist);
  SimpleSTRpair p_mid(&n0, &n_mid, &dist);

  SimpleSTRdistance::STRpairQueue pq;
  pq.push(&p_far);
  pq.push(&p_mid);
  pq.push(&p_near);

  EXPECT_DOUBLE_EQ(pq.top()->getDistance(), 2.0);
  pq.pop();
  EXPECT_DOUBLE_EQ(pq.top()->getDistance(), 5.0);
  pq.pop();
  EXPECT_DOUBLE_EQ(pq.top()->getDistance(), 10.0);
  pq.pop();
  EXPECT_TRUE(pq.empty());
}

TEST(SimpleSTRdistanceTest, NearestNeighbourDirectLeafLeaf) {
  int a = 11;
  int b = 22;
  const Envelope env_a(0.0, 1.0, 0.0, 1.0);
  const Envelope env_b(4.0, 5.0, 0.0, 1.0);
  SimpleSTRnode leaf_a(0, &env_a, &a);
  SimpleSTRnode leaf_b(0, &env_b, &b);

  EnvelopeItemDistance dist;
  SimpleSTRdistance solver(&leaf_a, &leaf_b, &dist);
  auto result = solver.nearestNeighbour();
  EXPECT_EQ(result.first, &a);
  EXPECT_EQ(result.second, &b);
}

TEST(SimpleSTRdistanceTest, NearestNeighbourLeafAndCompositeBothSides) {
  std::vector<TaggedBox> boxes = {
      {Envelope(0.0, 1.0, 0.0, 1.0), 1},
      {Envelope(5.0, 6.0, 0.0, 1.0), 2},
      {Envelope(10.0, 11.0, 0.0, 1.0), 3},
      {Envelope(20.0, 21.0, 0.0, 1.0), 4},
  };
  SimpleSTRtree tree(2);
  for (TaggedBox& box : boxes) {
    tree.insert(&box.env, &box);
  }
  SimpleSTRnode* root = tree.getRoot();
  ASSERT_NE(root, nullptr);

  TaggedBox query{Envelope(4.2, 4.4, 0.2, 0.8), 99};
  SimpleSTRnode query_leaf(0, &query.env, &query);
  EnvelopeItemDistance dist;

  // Leaf on left, composite on right (isFlipped = true in expandToQueue).
  {
    SimpleSTRdistance solver(&query_leaf, root, &dist);
    auto nearest = solver.nearestNeighbour();
    EXPECT_EQ(nearest.first, &query);
    EXPECT_EQ(static_cast<const TaggedBox*>(nearest.second)->id, 2);
  }

  // Composite on left, leaf on right (isFlipped = false in expandToQueue).
  {
    SimpleSTRdistance solver(root, &query_leaf, &dist);
    auto nearest = solver.nearestNeighbour();
    EXPECT_EQ(static_cast<const TaggedBox*>(nearest.first)->id, 2);
    EXPECT_EQ(nearest.second, &query);
  }
}

TEST(SimpleSTRdistanceTest,
     NearestNeighbourCompositeAreaHeuristicAndEqualAreas) {
  // large_tree spans [0, 100] x [0, 100] (area 10000).
  std::vector<TaggedBox> large_boxes = {
      {Envelope(0.0, 1.0, 0.0, 1.0), 10},
      {Envelope(50.0, 51.0, 50.0, 51.0), 11},
      {Envelope(99.0, 100.0, 49.0, 51.0), 12},
      {Envelope(20.0, 21.0, 99.0, 100.0), 13},
  };
  SimpleSTRtree large_tree(2);
  for (TaggedBox& box : large_boxes) {
    large_tree.insert(&box.env, &box);
  }

  // small_tree spans [102, 104] x [49, 51] (area 4).
  std::vector<TaggedBox> small_boxes = {
      {Envelope(102.0, 102.5, 49.0, 50.0), 20},
      {Envelope(103.5, 104.0, 50.0, 51.0), 21},
  };
  SimpleSTRtree small_tree(2);
  for (TaggedBox& box : small_boxes) {
    small_tree.insert(&box.env, &box);
  }

  EnvelopeItemDistance dist;

  // node1->area() > node2->area().
  {
    SimpleSTRdistance solver(large_tree.getRoot(), small_tree.getRoot(), &dist);
    auto nearest = solver.nearestNeighbour();
    EXPECT_EQ(static_cast<const TaggedBox*>(nearest.first)->id, 12);
    EXPECT_EQ(static_cast<const TaggedBox*>(nearest.second)->id, 20);
  }

  // node1->area() < node2->area().
  {
    SimpleSTRdistance solver(small_tree.getRoot(), large_tree.getRoot(), &dist);
    auto nearest = solver.nearestNeighbour();
    EXPECT_EQ(static_cast<const TaggedBox*>(nearest.first)->id, 20);
    EXPECT_EQ(static_cast<const TaggedBox*>(nearest.second)->id, 12);
  }

  // Equal composite areas: [0, 10] x [0, 10] vs [12, 22] x [0, 10].
  std::vector<TaggedBox> eq_a = {
      {Envelope(0.0, 1.0, 0.0, 1.0), 1},
      {Envelope(9.0, 10.0, 9.0, 10.0), 2},
  };
  std::vector<TaggedBox> eq_b = {
      {Envelope(12.0, 13.0, 9.0, 10.0), 3},
      {Envelope(21.0, 22.0, 0.0, 1.0), 4},
  };
  SimpleSTRtree eq_tree_a(2);
  SimpleSTRtree eq_tree_b(2);
  for (TaggedBox& box : eq_a) {
    eq_tree_a.insert(&box.env, &box);
  }
  for (TaggedBox& box : eq_b) {
    eq_tree_b.insert(&box.env, &box);
  }
  SimpleSTRdistance eq_solver(eq_tree_a.getRoot(), eq_tree_b.getRoot(), &dist);
  auto eq_nearest = eq_solver.nearestNeighbour();
  EXPECT_EQ(static_cast<const TaggedBox*>(eq_nearest.first)->id, 2);
  EXPECT_EQ(static_cast<const TaggedBox*>(eq_nearest.second)->id, 3);
}

TEST(SimpleSTRdistanceTest,
     NearestNeighbourEarlyExitZeroDistanceQueueBreakAndInfinityPruning) {
  // 1. Overlapping pair (distance 0.0) exits loop with candidates left in priQ.
  std::vector<TaggedBox> boxes_a = {
      {Envelope(0.0, 2.0, 0.0, 2.0), 1},
      {Envelope(50.0, 52.0, 50.0, 52.0), 2},
      {Envelope(100.0, 102.0, 100.0, 102.0), 3},
      {Envelope(150.0, 152.0, 150.0, 152.0), 4},
  };
  std::vector<TaggedBox> boxes_b = {
      {Envelope(1.0, 3.0, 1.0, 3.0), 10},
      {Envelope(60.0, 62.0, 60.0, 62.0), 20},
      {Envelope(110.0, 112.0, 110.0, 112.0), 30},
      {Envelope(160.0, 162.0, 160.0, 162.0), 40},
  };
  SimpleSTRtree tree_a(2);
  SimpleSTRtree tree_b(2);
  for (TaggedBox& box : boxes_a) {
    tree_a.insert(&box.env, &box);
  }
  for (TaggedBox& box : boxes_b) {
    tree_b.insert(&box.env, &box);
  }
  EnvelopeItemDistance dist;
  SimpleSTRdistance zero_solver(tree_a.getRoot(), tree_b.getRoot(), &dist);
  auto zero_pair = zero_solver.nearestNeighbour();
  EXPECT_EQ(static_cast<const TaggedBox*>(zero_pair.first)->id, 1);
  EXPECT_EQ(static_cast<const TaggedBox*>(zero_pair.second)->id, 10);

  // 2. Self-tree nearest neighbour with DoubleInfinity for identical items:
  // exercises sp->getDistance() < minDistance false branch in expand() and
  // currentDistance >= distanceLowerBound break + remaining queue drain.
  constexpr std::array<BoxSpec, 6> kSelfSpecs = {{
      {.min_x = 0.0, .max_x = 1.0, .min_y = 0.0, .max_y = 1.0, .id = 1},
      {.min_x = 10.0, .max_x = 11.0, .min_y = 0.0, .max_y = 1.0, .id = 2},
      {.min_x = 20.0, .max_x = 21.0, .min_y = 20.0, .max_y = 21.0, .id = 3},
      {.min_x = 22.5, .max_x = 23.5, .min_y = 20.0, .max_y = 21.0, .id = 4},
      {.min_x = 5.0, .max_x = 6.0, .min_y = 12.0, .max_y = 13.0, .id = 5},
      {.min_x = -8.0, .max_x = -6.0, .min_y = -6.0, .max_y = -4.0, .id = 6},
  }};
  std::vector<TaggedBox> self_boxes;
  self_boxes.reserve(kSelfSpecs.size());
  SimpleSTRtree self_tree(2);
  for (const BoxSpec& spec : kSelfSpecs) {
    self_boxes.push_back(
        {Envelope(spec.min_x, spec.max_x, spec.min_y, spec.max_y), spec.id});
    self_tree.insert(&self_boxes.back().env, &self_boxes.back());
  }

  EnvelopeItemDistance distinct_dist(geos::DoubleInfinity);
  SimpleSTRdistance self_solver(self_tree.getRoot(), self_tree.getRoot(),
                                &distinct_dist);
  auto closest_distinct = self_solver.nearestNeighbour();
  const int id1 = static_cast<const TaggedBox*>(closest_distinct.first)->id;
  const int id2 = static_cast<const TaggedBox*>(closest_distinct.second)->id;
  EXPECT_EQ(std::min(id1, id2), 3);
  EXPECT_EQ(std::max(id1, id2), 4);
}

TEST(SimpleSTRdistanceTest, NearestNeighbourWithGeometryItemDistance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::vector<std::unique_ptr<Point>> pts_a;
  pts_a.push_back(factory->createPoint(CoordinateXY(0.0, 0.0)));
  pts_a.push_back(factory->createPoint(CoordinateXY(3.0, 4.0)));
  pts_a.push_back(factory->createPoint(CoordinateXY(10.0, 10.0)));

  std::vector<std::unique_ptr<Point>> pts_b;
  pts_b.push_back(factory->createPoint(CoordinateXY(9.5, 10.5)));
  pts_b.push_back(factory->createPoint(CoordinateXY(50.0, 50.0)));
  pts_b.push_back(factory->createPoint(CoordinateXY(-20.0, -20.0)));

  SimpleSTRtree tree_a(2);
  for (const auto& pt : pts_a) {
    tree_a.insert(pt.get());
  }
  SimpleSTRtree tree_b(2);
  for (const auto& pt : pts_b) {
    tree_b.insert(pt.get());
  }

  GeometryItemDistance geom_dist;
  SimpleSTRdistance solver(tree_a.getRoot(), tree_b.getRoot(), &geom_dist);
  auto nearest = solver.nearestNeighbour();
  EXPECT_EQ(nearest.first, static_cast<const Geometry*>(pts_a[2].get()));
  EXPECT_EQ(nearest.second, static_cast<const Geometry*>(pts_b[0].get()));
}

TEST(SimpleSTRdistanceTest, NearestNeighbourEmptyOrInfiniteDistanceThrows) {
  int item = 1;
  const Envelope env(0.0, 1.0, 0.0, 1.0);
  SimpleSTRnode leaf(0, &env, &item);
  SimpleSTRnode empty_comp_a(1);
  SimpleSTRnode empty_comp_b(1);
  EnvelopeItemDistance env_dist;

  {
    SimpleSTRdistance solver(&empty_comp_a, &leaf, &env_dist);
    EXPECT_THROW(solver.nearestNeighbour(), GEOSException);
  }
  {
    SimpleSTRdistance solver(&leaf, &empty_comp_b, &env_dist);
    EXPECT_THROW(solver.nearestNeighbour(), GEOSException);
  }
  {
    SimpleSTRdistance solver(&empty_comp_a, &empty_comp_b, &env_dist);
    EXPECT_THROW(solver.nearestNeighbour(), GEOSException);
  }

  // Composite with valid leaf children whose ItemDistance returns +infinity
  // prunes every leaf pair and exhausts the queue with minPair == nullptr.
  SimpleSTRnode comp_with_leaf(1);
  comp_with_leaf.addChildNode(&leaf);
  FixedValueItemDistance inf_dist(geos::DoubleInfinity);
  SimpleSTRdistance inf_solver(&comp_with_leaf, &leaf, &inf_dist);
  EXPECT_THROW(inf_solver.nearestNeighbour(), GEOSException);
}

TEST(SimpleSTRdistanceTest, IsWithinDistanceRootQuickDecisions) {
  std::vector<TaggedBox> left_boxes = {
      {Envelope(0.0, 1.0, 0.0, 1.0), 1},
      {Envelope(2.0, 3.0, 0.0, 1.0), 2},
  };
  std::vector<TaggedBox> right_boxes = {
      {Envelope(8.0, 9.0, 0.0, 1.0), 3},
      {Envelope(10.0, 11.0, 0.0, 1.0), 4},
  };
  SimpleSTRtree left_tree(2);
  SimpleSTRtree right_tree(2);
  for (TaggedBox& box : left_boxes) {
    left_tree.insert(&box.env, &box);
  }
  for (TaggedBox& box : right_boxes) {
    right_tree.insert(&box.env, &box);
  }

  EnvelopeItemDistance dist;

  // Root bounds distance is 5.0 ([0,3]x[0,1] vs [8,11]x[0,1]).
  {
    SimpleSTRdistance solver(left_tree.getRoot(), right_tree.getRoot(), &dist);
    EXPECT_FALSE(solver.isWithinDistance(4.99));
    EXPECT_FALSE(solver.isWithinDistance(-1.0));
  }

  // Combined bounds [0,11]x[0,1] have diagonal hypot(11, 1).
  {
    SimpleSTRdistance solver(left_tree.getRoot(), right_tree.getRoot(), &dist);
    EXPECT_TRUE(solver.isWithinDistance(std::hypot(11.0, 1.0)));
    EXPECT_TRUE(solver.isWithinDistance(20.0));
  }
}

TEST(SimpleSTRdistanceTest,
     IsWithinDistanceExpandsSubtreesAndHandlesLeafUpperBound) {
  // Overlapping root bounds ([0, 120]x[0, 2.5] vs [10, 210]x[0, 2.5]) force
  // tree expansion before deciding true vs false.
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::vector<std::unique_ptr<LineString>> lines_a;
  lines_a.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(0.0, 0.0), CoordinateXY(20.0, 0.0)}));
  lines_a.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(100.0, 0.0), CoordinateXY(120.0, 0.0)}));

  std::vector<std::unique_ptr<LineString>> lines_b;
  lines_b.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(10.0, 1.5), CoordinateXY(11.0, 2.5)}));
  lines_b.push_back(factory->createLineString(
      CoordinateSequence{CoordinateXY(200.0, 0.0), CoordinateXY(210.0, 0.0)}));

  SimpleSTRtree tree_a(2);
  for (const auto& line : lines_a) {
    tree_a.insert(line.get());
  }
  SimpleSTRtree tree_b(2);
  for (const auto& line : lines_b) {
    tree_b.insert(line.get());
  }

  GeometryItemDistance geom_dist;

  // Closest leaf pair (lines_a[0], lines_b[0]) has distance 1.5, while its
  // combined envelope diagonal is hypot(20, 2.5) > 20.0.
  {
    SimpleSTRdistance solver(tree_a.getRoot(), tree_b.getRoot(), &geom_dist);
    // All expanded pairs exceed 1.49 -> returns false after expansion.
    EXPECT_FALSE(solver.isWithinDistance(1.49));
    // Hits isLeaves() branch (distanceUpperBound = 1.5 <= 2.0) -> returns true.
    EXPECT_TRUE(solver.isWithinDistance(2.0));
    EXPECT_TRUE(solver.isWithinDistance(1.5));
  }

  // Two-leaf trees with disjoint gaps [0,1] vs [10,11] at distance 9.0 and
  // expanded leaf pair maximumDistance = hypot(11, 1).
  std::vector<TaggedBox> boxes_left = {
      {Envelope(0.0, 1.0, 0.0, 1.0), 1},
      {Envelope(100.0, 101.0, 0.0, 1.0), 2},
  };
  std::vector<TaggedBox> boxes_right = {
      {Envelope(10.0, 11.0, 0.0, 1.0), 3},
      {Envelope(200.0, 201.0, 0.0, 1.0), 4},
  };
  SimpleSTRtree box_tree_l(2);
  SimpleSTRtree box_tree_r(2);
  for (TaggedBox& box : boxes_left) {
    box_tree_l.insert(&box.env, &box);
  }
  for (TaggedBox& box : boxes_right) {
    box_tree_r.insert(&box.env, &box);
  }

  EnvelopeItemDistance env_dist;
  SimpleSTRdistance box_solver(box_tree_l.getRoot(), box_tree_r.getRoot(),
                               &env_dist);
  EXPECT_FALSE(box_solver.isWithinDistance(8.99));
  EXPECT_TRUE(box_solver.isWithinDistance(9.0));
  EXPECT_TRUE(box_solver.isWithinDistance(std::hypot(11.0, 1.0)));
}

TEST(SimpleSTRdistanceTest, IsWithinDistanceQueueExhaustionReturnsFalse) {
  int item = 1;
  const Envelope env(0.0, 1.0, 0.0, 1.0);
  const Envelope comp_env(0.0, 10.0, 0.0, 10.0);
  SimpleSTRnode leaf(0, &env, &item);
  SimpleSTRnode empty_comp_a(1, &comp_env, nullptr);
  SimpleSTRnode empty_comp_b(1, &comp_env, nullptr);
  EnvelopeItemDistance env_dist;

  // Empty composite nodes (0 children) with valid envelopes expand to nothing
  // and exhaust the queue, returning false at the end of isWithinDistance.
  {
    SimpleSTRdistance solver(&empty_comp_a, &leaf, &env_dist);
    EXPECT_FALSE(solver.isWithinDistance(5.0));
  }
  {
    SimpleSTRdistance solver(&leaf, &empty_comp_b, &env_dist);
    EXPECT_FALSE(solver.isWithinDistance(5.0));
  }
  {
    SimpleSTRdistance solver(&empty_comp_a, &empty_comp_b, &env_dist);
    EXPECT_FALSE(solver.isWithinDistance(5.0));
  }

  // Composite with leaf + infinite item distance skips pushing leaf pairs in
  // expand() when maxDistance is smaller than the root maximumDistance.
  SimpleSTRnode comp_with_leaf(1);
  comp_with_leaf.addChildNode(&leaf);
  FixedValueItemDistance inf_dist(geos::DoubleInfinity);
  {
    SimpleSTRdistance solver(&comp_with_leaf, &leaf, &inf_dist);
    EXPECT_FALSE(solver.isWithinDistance(0.5));
  }

  // Same pruning with two children and composite on the right (isFlipped =
  // true).
  int far_item = 2;
  const Envelope far_env(20.0, 21.0, 0.0, 1.0);
  SimpleSTRnode far_leaf(0, &far_env, &far_item);
  SimpleSTRnode comp_two_leaves(1);
  comp_two_leaves.addChildNode(&leaf);
  comp_two_leaves.addChildNode(&far_leaf);
  {
    SimpleSTRdistance solver(&leaf, &comp_two_leaves, &inf_dist);
    EXPECT_FALSE(solver.isWithinDistance(10.0));
  }

  // Leaf-leaf pair with NaN maxDistance falls through isLeaves() to return
  // false at the end of the search loop.
  {
    SimpleSTRdistance solver(&leaf, &leaf, &env_dist);
    EXPECT_FALSE(solver.isWithinDistance(geos::DoubleNotANumber));
  }

  // Composite tree with NaN maxDistance updates distanceUpperBound across
  // popped leaf pairs and passes it to subsequent expandToQueue calls.
  std::vector<TaggedBox> boxes = {
      {Envelope(0.0, 1.0, 0.0, 1.0), 1},
      {Envelope(0.0, 1.0, 0.0, 1.0), 2},
      {Envelope(5.0, 6.0, 0.0, 1.0), 3},
      {Envelope(10.0, 11.0, 0.0, 1.0), 4},
  };
  SimpleSTRtree tree(2);
  for (TaggedBox& box : boxes) {
    tree.insert(&box.env, &box);
  }
  SimpleSTRdistance nan_tree_solver(tree.getRoot(), &leaf, &env_dist);
  EXPECT_FALSE(nan_tree_solver.isWithinDistance(geos::DoubleNotANumber));
}

TEST(SimpleSTRdistanceTest,
     IsWithinDistanceDeepTreeOverlappingEnvelopesAndZeroArea) {
  // Multi-level trees whose root envelopes overlap (0.0 distance) and whose
  // closest cluster pair has distance 1.0 with maximumDistance < 8.0.
  std::vector<TaggedBox> left_boxes = {
      {Envelope(0.0, 1.0, 0.0, 1.0), 1},
      {Envelope(1.0, 2.0, 0.0, 1.0), 2},
      {Envelope(2.0, 3.0, 0.0, 1.0), 3},
      {Envelope(12.0, 13.0, 0.0, 1.0), 4},
      {Envelope(14.0, 15.0, 0.0, 1.0), 5},
      {Envelope(100.0, 101.0, 5.0, 6.0), 6},
  };
  std::vector<TaggedBox> right_boxes = {
      {Envelope(-50.0, -49.0, 5.0, 6.0), 10},
      {Envelope(-40.0, -39.0, 5.0, 6.0), 11},
      {Envelope(-30.0, -29.0, 5.0, 6.0), 12},
      {Envelope(16.0, 17.0, 0.0, 1.0), 13},
      {Envelope(18.0, 19.0, 0.0, 1.0), 14},
      {Envelope(200.0, 201.0, 5.0, 6.0), 15},
  };
  SimpleSTRtree left_tree(2);
  SimpleSTRtree right_tree(2);
  for (TaggedBox& box : left_boxes) {
    left_tree.insert(&box.env, &box);
  }
  for (TaggedBox& box : right_boxes) {
    right_tree.insert(&box.env, &box);
  }

  EnvelopeItemDistance env_dist;
  {
    SimpleSTRdistance solver(left_tree.getRoot(), right_tree.getRoot(),
                             &env_dist);
    EXPECT_FALSE(solver.isWithinDistance(0.99));
    EXPECT_TRUE(solver.isWithinDistance(1.0));
    EXPECT_TRUE(solver.isWithinDistance(8.0));
  }
  {
    SimpleSTRdistance solver(right_tree.getRoot(), left_tree.getRoot(),
                             &env_dist);
    EXPECT_FALSE(solver.isWithinDistance(0.99));
    EXPECT_TRUE(solver.isWithinDistance(8.0));
  }

  // Parallel diagonal segments have overlapping envelopes (distance 0.0) while
  // their exact geometry distance is sqrt(2).
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> diag_a = factory->createLineString(
      CoordinateSequence{CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 10.0)});
  std::unique_ptr<LineString> diag_b = factory->createLineString(
      CoordinateSequence{CoordinateXY(0.0, 2.0), CoordinateXY(10.0, 12.0)});
  SimpleSTRnode diag_node_a(0, diag_a->getEnvelopeInternal(), diag_a.get());
  SimpleSTRnode diag_node_b(0, diag_b->getEnvelopeInternal(), diag_b.get());
  GeometryItemDistance geom_dist;
  SimpleSTRdistance diag_solver(&diag_node_a, &diag_node_b, &geom_dist);
  EXPECT_FALSE(diag_solver.isWithinDistance(1.4));
  EXPECT_TRUE(diag_solver.isWithinDistance(1.42));

  // Touching boxes are within distance 0.0; separated boxes are not.
  int t1 = 1;
  int t2 = 2;
  const Envelope touch_a(0.0, 1.0, 0.0, 1.0);
  const Envelope touch_b(1.0, 2.0, 0.0, 1.0);
  const Envelope near_b(1.01, 2.0, 0.0, 1.0);
  SimpleSTRnode touch_node_a(0, &touch_a, &t1);
  SimpleSTRnode touch_node_b(0, &touch_b, &t2);
  SimpleSTRnode near_node_b(0, &near_b, &t2);
  SimpleSTRdistance touch_solver(&touch_node_a, &touch_node_b, &env_dist);
  SimpleSTRdistance near_solver(&touch_node_a, &near_node_b, &env_dist);
  EXPECT_TRUE(touch_solver.isWithinDistance(0.0));
  EXPECT_FALSE(near_solver.isWithinDistance(0.0));

  // Degenerate zero-area composite envelopes (collinear point envelopes).
  std::vector<TaggedBox> zero_a = {
      {Envelope(0.0, 0.0, 0.0, 0.0), 1},
      {Envelope(5.0, 5.0, 0.0, 0.0), 2},
  };
  std::vector<TaggedBox> zero_b = {
      {Envelope(7.0, 7.0, 0.0, 0.0), 3},
      {Envelope(12.0, 12.0, 0.0, 0.0), 4},
  };
  SimpleSTRtree zero_tree_a(2);
  SimpleSTRtree zero_tree_b(2);
  for (TaggedBox& box : zero_a) {
    zero_tree_a.insert(&box.env, &box);
  }
  for (TaggedBox& box : zero_b) {
    zero_tree_b.insert(&box.env, &box);
  }
  SimpleSTRdistance zero_area_solver(zero_tree_a.getRoot(),
                                     zero_tree_b.getRoot(), &env_dist);
  auto zero_nearest = zero_area_solver.nearestNeighbour();
  EXPECT_EQ(static_cast<const TaggedBox*>(zero_nearest.first)->id, 2);
  EXPECT_EQ(static_cast<const TaggedBox*>(zero_nearest.second)->id, 3);
  EXPECT_FALSE(zero_area_solver.isWithinDistance(1.99));
  EXPECT_TRUE(zero_area_solver.isWithinDistance(2.0));
}

}  // namespace
}  // namespace geos_google
