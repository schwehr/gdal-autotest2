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
// third_party/geos/include/geos/index/strtree/TemplateSTRtreeDistance.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Allocates isolated GeometryFactory instances via GeometryFactory::create().
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/index/strtree/TemplateSTRtreeDistance.h

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <type_traits>
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
#include "third_party/geos/include/geos/index/strtree/TemplateSTRNode.h"
#include "third_party/geos/include/geos/index/strtree/TemplateSTRNodePair.h"
#include "third_party/geos/include/geos/index/strtree/TemplateSTRtree.h"
// <cassert> must precede TemplateSTRtreeDistance.h because TemplateSTRNode.h
// and TemplateSTRNodePair.h use assert() without including <cassert>. Keep this
// include below system headers.
// clang-format off
#include "third_party/geos/include/geos/index/strtree/TemplateSTRtreeDistance.h"
// clang-format on
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
using geos::index::strtree::EnvelopeTraits;
using geos::index::strtree::TemplateSTRNode;
using geos::index::strtree::TemplateSTRNodePair;
using geos::index::strtree::TemplateSTRtree;
using geos::index::strtree::TemplateSTRtreeDistance;
using geos::util::GEOSException;
using ::testing::HasSubstr;

struct BoxItem {
  int id;
  Envelope env;
  CoordinateXY anchor;
};

BoxItem MakePointItem(int id, double x, double y) {
  return BoxItem{
      .id = id,
      .env = Envelope(x, x, y, y),
      .anchor = CoordinateXY(x, y),
  };
}

BoxItem MakeBoxItem(int id, double minx, double maxx, double miny, double maxy,
                    double anchor_x, double anchor_y) {
  return BoxItem{
      .id = id,
      .env = Envelope(minx, maxx, miny, maxy),
      .anchor = CoordinateXY(anchor_x, anchor_y),
  };
}

struct AnchorDistance {
  int calls = 0;

  double operator()(const BoxItem* a, const BoxItem* b) {
    ++calls;
    return a->anchor.distance(b->anchor);
  }
};

using BoxTree = TemplateSTRtree<const BoxItem*, EnvelopeTraits>;
using BoxNode = TemplateSTRNode<const BoxItem*, EnvelopeTraits>;
using BoxNodePair =
    TemplateSTRNodePair<const BoxItem*, EnvelopeTraits, AnchorDistance>;
using BoxTreeDistance =
    TemplateSTRtreeDistance<const BoxItem*, EnvelopeTraits, AnchorDistance>;

void InsertAll(BoxTree& tree, const std::vector<BoxItem>& items) {
  for (const auto& item : items) {
    tree.insert(item.env, &item);
  }
}

double BruteForceMinDistance(const std::vector<BoxItem>& lhs,
                             const std::vector<BoxItem>& rhs) {
  double best = std::numeric_limits<double>::infinity();
  for (const auto& a : lhs) {
    for (const auto& b : rhs) {
      best = std::min(best, a.anchor.distance(b.anchor));
    }
  }
  return best;
}

TEST(TemplateSTRtreeDistanceTest, SingleLeafPairNearestNeighbourOverloads) {
  const BoxItem item1 = MakePointItem(1, 0.0, 0.0);
  const BoxItem item2 = MakePointItem(2, 3.0, 4.0);
  const BoxNode root1(&item1, item1.env);
  const BoxNode root2(&item2, item2.env);

  {
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    const auto nearest = td.nearestNeighbour(root1, root2);
    EXPECT_EQ(nearest.first, &item1);
    EXPECT_EQ(nearest.second, &item2);
    EXPECT_EQ(dist.calls, 1);
  }
  {
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    BoxNodePair init_pair(root1, root2, dist);
    const auto nearest = td.nearestNeighbour(init_pair);
    EXPECT_EQ(nearest.first, &item1);
    EXPECT_EQ(nearest.second, &item2);
    EXPECT_EQ(dist.calls, 1);
  }
}

TEST(TemplateSTRtreeDistanceTest, NearestNeighbourStopsEarlyOnZeroDistance) {
  const std::vector<BoxItem> items1 = {
      MakePointItem(10, 10.0, 10.0),   MakePointItem(11, 100.0, 100.0),
      MakePointItem(12, 200.0, 100.0), MakePointItem(13, 300.0, 100.0),
      MakePointItem(14, 400.0, 100.0), MakePointItem(15, 500.0, 100.0),
  };
  const std::vector<BoxItem> items2 = {
      MakePointItem(20, 10.0, 10.0),   MakePointItem(21, 150.0, 150.0),
      MakePointItem(22, 250.0, 150.0), MakePointItem(23, 350.0, 150.0),
      MakePointItem(24, 450.0, 150.0), MakePointItem(25, 550.0, 150.0),
  };

  BoxTree tree1(2);
  BoxTree tree2(2);
  InsertAll(tree1, items1);
  InsertAll(tree2, items2);

  AnchorDistance dist;
  BoxTreeDistance td(dist);
  const auto nearest = td.nearestNeighbour(*tree1.getRoot(), *tree2.getRoot());
  ASSERT_NE(nearest.first, nullptr);
  ASSERT_NE(nearest.second, nullptr);
  EXPECT_EQ(nearest.first->id, 10);
  EXPECT_EQ(nearest.second->id, 20);
  EXPECT_DOUBLE_EQ(nearest.first->anchor.distance(nearest.second->anchor), 0.0);
  EXPECT_LT(dist.calls, static_cast<int>(items1.size() * items2.size()));
}

TEST(TemplateSTRtreeDistanceTest, NearestNeighbourPrunesQueueAtPositiveBound) {
  // Minimum distance is 5.0 (with a tie at two distinct locations), while
  // other pairs remain queued with larger bounds so the queue-pruning break
  // branch executes.
  const std::vector<BoxItem> items1 = {
      MakePointItem(1, 0.0, 0.0),    MakePointItem(2, 100.0, 0.0),
      MakePointItem(3, 200.0, 50.0), MakePointItem(4, 300.0, 50.0),
      MakePointItem(5, 400.0, 50.0), MakePointItem(6, 500.0, 50.0),
  };
  const std::vector<BoxItem> items2 = {
      MakePointItem(11, 3.0, 4.0),    MakePointItem(12, 103.0, 4.0),
      MakePointItem(13, 220.0, 80.0), MakePointItem(14, 320.0, 80.0),
      MakePointItem(15, 420.0, 80.0), MakePointItem(16, 520.0, 80.0),
  };

  BoxTree tree1(2);
  BoxTree tree2(2);
  InsertAll(tree1, items1);
  InsertAll(tree2, items2);

  AnchorDistance dist;
  BoxTreeDistance td(dist);
  const auto nearest = td.nearestNeighbour(*tree1.getRoot(), *tree2.getRoot());
  ASSERT_NE(nearest.first, nullptr);
  ASSERT_NE(nearest.second, nullptr);
  EXPECT_DOUBLE_EQ(nearest.first->anchor.distance(nearest.second->anchor), 5.0);
  EXPECT_LT(dist.calls, static_cast<int>(items1.size() * items2.size()));
}

TEST(TemplateSTRtreeDistanceTest,
     ExpandHeuristicCoversLargerLeftLargerRightEqualAndLeafSides) {
  // Large composite: spans [0, 100] x [0, 100] (area = 10000).
  const std::vector<BoxItem> large_items = {
      MakePointItem(1, 0.0, 0.0),
      MakePointItem(2, 0.0, 100.0),
      MakePointItem(3, 100.0, 0.0),
      MakePointItem(4, 100.0, 100.0),
  };
  // Small composite: spans [103, 105] x [0, 4] (area = 8).
  const std::vector<BoxItem> small_items = {
      MakePointItem(11, 103.0, 0.0),
      MakePointItem(12, 105.0, 0.0),
      MakePointItem(13, 103.0, 4.0),
      MakePointItem(14, 105.0, 4.0),
  };
  // Equal-area composite: spans [102, 202] x [0, 100] (area = 10000).
  const std::vector<BoxItem> equal_items = {
      MakePointItem(21, 102.0, 0.0),
      MakePointItem(22, 202.0, 0.0),
      MakePointItem(23, 102.0, 100.0),
      MakePointItem(24, 202.0, 100.0),
  };
  // Single leaf item.
  const std::vector<BoxItem> single_item = {
      MakePointItem(31, 101.0, 0.0),
  };

  BoxTree large_tree(2);
  BoxTree small_tree(2);
  BoxTree equal_tree(2);
  BoxTree single_tree(2);
  InsertAll(large_tree, large_items);
  InsertAll(small_tree, small_items);
  InsertAll(equal_tree, equal_items);
  InsertAll(single_tree, single_item);

  ASSERT_TRUE(large_tree.getRoot()->isComposite());
  ASSERT_TRUE(small_tree.getRoot()->isComposite());
  ASSERT_TRUE(equal_tree.getRoot()->isComposite());
  ASSERT_TRUE(single_tree.getRoot()->isLeaf());

  // 1. Both composite, left root area > right root area.
  {
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    const auto p =
        td.nearestNeighbour(*large_tree.getRoot(), *small_tree.getRoot());
    EXPECT_EQ(p.first->id, 3);
    EXPECT_EQ(p.second->id, 11);
    EXPECT_DOUBLE_EQ(p.first->anchor.distance(p.second->anchor), 3.0);
  }
  // 2. Both composite, left root area < right root area.
  {
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    const auto p =
        td.nearestNeighbour(*small_tree.getRoot(), *large_tree.getRoot());
    EXPECT_EQ(p.first->id, 11);
    EXPECT_EQ(p.second->id, 3);
    EXPECT_DOUBLE_EQ(p.first->anchor.distance(p.second->anchor), 3.0);
  }
  // 3. Both composite, left root area == right root area.
  {
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    const auto p =
        td.nearestNeighbour(*large_tree.getRoot(), *equal_tree.getRoot());
    EXPECT_DOUBLE_EQ(p.first->anchor.distance(p.second->anchor), 2.0);
  }
  // 4. Left composite, right leaf.
  {
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    const auto p =
        td.nearestNeighbour(*large_tree.getRoot(), *single_tree.getRoot());
    EXPECT_EQ(p.first->id, 3);
    EXPECT_EQ(p.second->id, 31);
    EXPECT_DOUBLE_EQ(p.first->anchor.distance(p.second->anchor), 1.0);
  }
  // 5. Left leaf, right composite.
  {
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    const auto p =
        td.nearestNeighbour(*single_tree.getRoot(), *large_tree.getRoot());
    EXPECT_EQ(p.first->id, 31);
    EXPECT_EQ(p.second->id, 3);
    EXPECT_DOUBLE_EQ(p.first->anchor.distance(p.second->anchor), 1.0);
  }
}

TEST(TemplateSTRtreeDistanceTest,
     NearestNeighbourMatchesBruteForceOnMultiLevelNonPointBoxes) {
  std::vector<BoxItem> lhs;
  for (int i = 0; i < 14; ++i) {
    const double x = static_cast<double>(i) * 5.0;
    const double y = static_cast<double>(i % 4) * 7.0;
    lhs.push_back(
        MakeBoxItem(100 + i, x, x + 4.0, y, y + 4.0, x + 1.0, y + 1.0));
  }

  std::vector<BoxItem> rhs;
  for (int i = 0; i < 16; ++i) {
    const double x = static_cast<double>(i) * 4.5 + 1.0;
    const double y = static_cast<double>((i + 1) % 5) * 6.0;
    rhs.push_back(
        MakeBoxItem(200 + i, x, x + 4.0, y, y + 4.0, x + 3.0, y + 3.0));
  }

  BoxTree tree1(2);
  BoxTree tree2(2);
  InsertAll(tree1, lhs);
  InsertAll(tree2, rhs);

  AnchorDistance dist;
  BoxTreeDistance td(dist);
  const auto nearest = td.nearestNeighbour(*tree1.getRoot(), *tree2.getRoot());
  ASSERT_NE(nearest.first, nullptr);
  ASSERT_NE(nearest.second, nullptr);

  const double expected = BruteForceMinDistance(lhs, rhs);
  EXPECT_DOUBLE_EQ(nearest.first->anchor.distance(nearest.second->anchor),
                   expected);
  EXPECT_LT(dist.calls, static_cast<int>(lhs.size() * rhs.size()));
}

TEST(TemplateSTRtreeDistanceTest, NearestNeighbourThrowsWhenNoLeafPairsExist) {
  const BoxItem item1 = MakeBoxItem(1, 0.0, 10.0, 0.0, 10.0, 5.0, 5.0);
  const BoxItem item2 = MakeBoxItem(2, 11.0, 20.0, 0.0, 10.0, 15.0, 5.0);

  const std::array<BoxNode, 1> anchor = {BoxNode(&item1, item1.env)};
  const BoxNode empty_composite(anchor.data(), anchor.data());
  const BoxNode leaf(&item2, item2.env);
  ASSERT_TRUE(empty_composite.isComposite());

  AnchorDistance dist;
  BoxTreeDistance td(dist);

  try {
    td.nearestNeighbour(empty_composite, leaf);
    FAIL() << "Expected GEOSException for empty composite left root";
  } catch (const GEOSException& e) {
    EXPECT_THAT(e.what(), HasSubstr("nearest neighbor"));
  }

  EXPECT_THROW(td.nearestNeighbour(leaf, empty_composite), GEOSException);
  EXPECT_EQ(dist.calls, 0);
}

TEST(TemplateSTRtreeDistanceTest,
     IsWithinDistanceCoversBoundsPruningMaxDistanceShortCircuitAndLeafChecks) {
  // 1. Root bounds distance > maxDistance (including negative maxDistance).
  {
    const BoxItem a = MakePointItem(1, 0.0, 0.0);
    const BoxItem b = MakePointItem(2, 5.0, 0.0);
    const BoxNode node_a(&a, a.env);
    const BoxNode node_b(&b, b.env);
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    // Leaf-leaf NodePair computes item distance eagerly in its constructor.
    EXPECT_FALSE(td.isWithinDistance(node_a, node_b, 4.99));
    EXPECT_EQ(dist.calls, 1);

    const BoxNode node_a2(&a, a.env);
    EXPECT_FALSE(td.isWithinDistance(node_a, node_a2, -0.1));
    EXPECT_EQ(dist.calls, 2);

    // Composite roots whose bounds distance > maxDistance prune before any leaf
    // item distance calls.
    const std::vector<BoxItem> left_items = {
        MakeBoxItem(10, 0.0, 1.0, 0.0, 1.0, 0.5, 0.5),
        MakeBoxItem(11, 0.0, 1.0, 2.0, 3.0, 0.5, 2.5),
    };
    const std::vector<BoxItem> right_items = {
        MakeBoxItem(20, 6.0, 7.0, 0.0, 1.0, 6.5, 0.5),
        MakeBoxItem(21, 6.0, 7.0, 2.0, 3.0, 6.5, 2.5),
    };
    BoxTree left_tree(2);
    BoxTree right_tree(2);
    InsertAll(left_tree, left_items);
    InsertAll(right_tree, right_items);
    ASSERT_TRUE(left_tree.getRoot()->isComposite());
    ASSERT_TRUE(right_tree.getRoot()->isComposite());

    AnchorDistance comp_dist;
    BoxTreeDistance comp_td(comp_dist);
    EXPECT_FALSE(comp_td.isWithinDistance(*left_tree.getRoot(),
                                          *right_tree.getRoot(), 4.99));
    EXPECT_FALSE(comp_td.isWithinDistance(*left_tree.getRoot(),
                                          *left_tree.getRoot(), -0.1));
    EXPECT_EQ(comp_dist.calls, 0);
  }

  // 2. Root maximumDistance <= maxDistance returns true (without item calls
  //    when at least one root is composite, and with one eager leaf-pair call
  //    when both roots are single leaves).
  {
    const BoxItem a = MakeBoxItem(1, 0.0, 3.0, 0.0, 4.0, 1.5, 2.0);
    const BoxItem b = MakePointItem(2, 0.0, 0.0);
    // maxDistance(a.env, b.env) == hypot(3, 4) == 5.0.
    const BoxNode node_a(&a, a.env);
    const BoxNode node_b(&b, b.env);
    AnchorDistance leaf_dist;
    BoxTreeDistance leaf_td(leaf_dist);
    EXPECT_TRUE(leaf_td.isWithinDistance(node_a, node_b, 5.0));
    EXPECT_TRUE(leaf_td.isWithinDistance(node_a, node_b, 6.0));
    EXPECT_EQ(leaf_dist.calls, 2);

    const std::vector<BoxItem> comp_items = {
        MakeBoxItem(10, 0.0, 3.0, 0.0, 4.0, 1.0, 1.0),
        MakeBoxItem(11, 0.0, 3.0, 0.0, 4.0, 2.0, 3.0),
    };
    BoxTree comp_tree(2);
    InsertAll(comp_tree, comp_items);
    ASSERT_TRUE(comp_tree.getRoot()->isComposite());
    AnchorDistance comp_dist;
    BoxTreeDistance comp_td(comp_dist);
    EXPECT_TRUE(comp_td.isWithinDistance(*comp_tree.getRoot(), node_b, 5.0));
    EXPECT_TRUE(comp_td.isWithinDistance(*comp_tree.getRoot(), node_b, 6.0));
    EXPECT_EQ(comp_dist.calls, 0);
  }

  // 3. Composite roots exceed maxDistance on maximumDistance(), then expand to
  // a
  //    composite child pair whose maximumDistance() <= maxDistance without
  //    reaching any leaf-leaf pairs.
  {
    const std::vector<BoxItem> items1 = {
        MakeBoxItem(1, 0.0, 1.0, 0.0, 1.0, 0.5, 0.5),
        MakeBoxItem(2, 0.5, 1.5, 0.0, 1.0, 1.0, 0.5),
        MakeBoxItem(3, 200.0, 201.0, 0.0, 1.0, 200.5, 0.5),
        MakeBoxItem(4, 300.0, 301.0, 0.0, 1.0, 300.5, 0.5),
    };
    const std::vector<BoxItem> items2 = {
        MakeBoxItem(11, 2.0, 3.0, 0.0, 1.0, 2.5, 0.5),
        MakeBoxItem(12, 2.2, 3.2, 0.0, 1.0, 2.7, 0.5),
        MakeBoxItem(13, 500.0, 501.0, 0.0, 1.0, 500.5, 0.5),
        MakeBoxItem(14, 600.0, 601.0, 0.0, 1.0, 600.5, 0.5),
    };
    BoxTree tree1(2);
    BoxTree tree2(2);
    InsertAll(tree1, items1);
    InsertAll(tree2, items2);

    ASSERT_GT(
        tree1.getRoot()->getBounds().maxDistance(tree2.getRoot()->getBounds()),
        3.5);
    // Child clusters [0, 1.5] x [0, 1] and [2.0, 3.2] x [0, 1] have
    // distance = 0.5 and maxDistance = hypot(3.2, 1.0) = sqrt(11.24) ~ 3.353.
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    EXPECT_TRUE(td.isWithinDistance(*tree1.getRoot(), *tree2.getRoot(), 3.5));
    EXPECT_EQ(dist.calls, 0);
  }

  // 4. Leaf distance <= maxDistance < bounds.maxDistance exercises the leaf
  //    check branch (both exact equality == and strict <), as well as leaf
  //    rejection when ItemDistance > maxDistance.
  {
    const BoxItem a = MakeBoxItem(1, 0.0, 10.0, 0.0, 10.0, 1.0, 5.0);
    const BoxItem b = MakeBoxItem(2, 11.0, 20.0, 0.0, 10.0, 19.0, 5.0);
    // env.distance = 1.0, env.maxDistance = hypot(20, 10) > 22.36,
    // actual anchor distance = 18.0.
    const BoxNode node_a(&a, a.env);
    const BoxNode node_b(&b, b.env);

    {
      AnchorDistance dist;
      BoxTreeDistance td(dist);
      EXPECT_TRUE(td.isWithinDistance(node_a, node_b, 18.0));
      EXPECT_EQ(dist.calls, 1);
    }
    {
      AnchorDistance dist;
      BoxTreeDistance td(dist);
      EXPECT_TRUE(td.isWithinDistance(node_a, node_b, 19.0));
      EXPECT_EQ(dist.calls, 1);
    }
    {
      AnchorDistance dist;
      BoxTreeDistance td(dist);
      EXPECT_FALSE(td.isWithinDistance(node_a, node_b, 17.99));
      EXPECT_EQ(dist.calls, 1);
    }
  }

  // 5. Multi-level trees whose root bounds overlap (distance = 0) but every
  //    actual item pair is separated by >= 18.0.
  {
    const std::vector<BoxItem> items1 = {
        MakeBoxItem(1, 0.0, 10.0, 0.0, 10.0, 1.0, 5.0),
        MakeBoxItem(2, 0.0, 10.0, 20.0, 30.0, 1.0, 25.0),
        MakeBoxItem(3, 0.0, 10.0, 40.0, 50.0, 1.0, 45.0),
        MakeBoxItem(4, 0.0, 10.0, 60.0, 70.0, 1.0, 65.0),
    };
    const std::vector<BoxItem> items2 = {
        MakeBoxItem(11, 5.0, 25.0, 0.0, 10.0, 20.0, 5.0),
        MakeBoxItem(12, 5.0, 25.0, 20.0, 30.0, 20.0, 25.0),
        MakeBoxItem(13, 5.0, 25.0, 40.0, 50.0, 20.0, 45.0),
        MakeBoxItem(14, 5.0, 25.0, 60.0, 70.0, 20.0, 65.0),
    };
    BoxTree tree1(2);
    BoxTree tree2(2);
    InsertAll(tree1, items1);
    InsertAll(tree2, items2);

    AnchorDistance dist;
    BoxTreeDistance td(dist);
    EXPECT_FALSE(td.isWithinDistance(*tree1.getRoot(), *tree2.getRoot(), 18.5));
    EXPECT_GT(dist.calls, 0);
    EXPECT_TRUE(td.isWithinDistance(*tree1.getRoot(), *tree2.getRoot(), 19.0));
  }

  // 6. Composite node with empty child range exhausts priority queue and
  //    returns false after loop termination.
  {
    const BoxItem item1 = MakeBoxItem(1, 0.0, 10.0, 0.0, 10.0, 5.0, 5.0);
    const BoxItem item2 = MakeBoxItem(2, 11.0, 20.0, 0.0, 10.0, 15.0, 5.0);
    const std::array<BoxNode, 1> anchor = {BoxNode(&item1, item1.env)};
    const BoxNode empty_composite(anchor.data(), anchor.data());
    const BoxNode leaf(&item2, item2.env);

    AnchorDistance dist;
    BoxTreeDistance td(dist);
    // env.distance = 1.0 <= 2.0 < env.maxDistance (~22.36), so expansion runs
    // and pushes zero children, draining the priority queue to return false.
    EXPECT_FALSE(td.isWithinDistance(empty_composite, leaf, 2.0));
    EXPECT_FALSE(td.isWithinDistance(leaf, empty_composite, 2.0));
    EXPECT_EQ(dist.calls, 0);
  }
}

TEST(TemplateSTRtreeDistanceTest, IntegrationViaTemplateSTRtreeWithGeometries) {
  const GeometryFactory::Ptr factory = GeometryFactory::create();

  auto make_line = [&factory](double x1, double y1, double x2, double y2) {
    auto seq = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXY>{
            CoordinateXY(x1, y1),
            CoordinateXY(x2, y2),
        });
    return factory->createLineString(std::move(seq));
  };

  std::vector<std::unique_ptr<Geometry>> geoms1;
  geoms1.push_back(make_line(0.0, 0.0, 10.0, 10.0));
  geoms1.push_back(make_line(20.0, 0.0, 30.0, 10.0));
  geoms1.push_back(factory->createPoint(CoordinateXY(40.0, 0.0)));
  geoms1.push_back(factory->createPoint(CoordinateXY(60.0, 0.0)));

  std::vector<std::unique_ptr<Geometry>> geoms2;
  geoms2.push_back(make_line(0.0, 10.0, 4.0, 6.0));
  geoms2.push_back(make_line(20.0, 10.0, 22.0, 8.0));
  geoms2.push_back(factory->createPoint(CoordinateXY(45.0, 0.0)));
  geoms2.push_back(factory->createPoint(CoordinateXY(65.0, 0.0)));

  struct GeomDist {
    double operator()(const Geometry* a, const Geometry* b) const {
      return a->distance(b);
    }
  };

  TemplateSTRtree<const Geometry*, EnvelopeTraits> tree1(2);
  TemplateSTRtree<const Geometry*, EnvelopeTraits> tree2(2);
  for (const auto& g : geoms1) {
    tree1.insert(g.get());
  }
  for (const auto& g : geoms2) {
    tree2.insert(g.get());
  }

  // Distance from (0,0)-(10,10) to (0,10)-(4,6) is sqrt(2) (midpoint at (5,5)
  // to endpoint (4,6) is sqrt(2)); to (20,10)-(22,8) from (20,0)-(30,10) is
  // 3 * sqrt(2).
  const auto nearest_pair = tree1.nearestNeighbour<GeomDist>(tree2);
  EXPECT_EQ(nearest_pair.first, geoms1[0].get());
  EXPECT_EQ(nearest_pair.second, geoms2[0].get());
  const double expected_dist = std::sqrt(2.0);
  EXPECT_NEAR(nearest_pair.first->distance(nearest_pair.second), expected_dist,
              1e-12);

  // Query nearestNeighbour against a single target geometry (exercises the
  // NodePair& overload in TemplateSTRtreeDistance).
  const std::unique_ptr<Point> query_pt =
      factory->createPoint(CoordinateXY(41.0, 0.0));
  GeomDist geom_dist;
  const Geometry* nearest_single = tree1.nearestNeighbour(
      *query_pt->getEnvelopeInternal(), query_pt.get(), geom_dist);
  EXPECT_EQ(nearest_single, geoms1[2].get());

  EXPECT_FALSE(tree1.isWithinDistance<GeomDist>(tree2, expected_dist - 1e-6));
  EXPECT_TRUE(tree1.isWithinDistance<GeomDist>(tree2, expected_dist + 1e-6));
}

struct DistinctAnchorDistance {
  int calls = 0;

  double operator()(const BoxItem* a, const BoxItem* b) {
    ++calls;
    if (a == b) {
      return geos::DoubleMax;
    }
    return a->anchor.distance(b->anchor);
  }
};

struct Interval1D {
  double lo;
  double hi;
};

struct Interval1DTraits {
  using BoundsType = Interval1D;
  using TwoDimensional = std::false_type;

  static bool intersects(const BoundsType& a, const BoundsType& b) {
    return a.lo <= b.hi && a.hi >= b.lo;
  }

  static double size(const BoundsType& a) { return a.hi - a.lo; }

  static double distance(const BoundsType& a, const BoundsType& b) {
    if (a.hi < b.lo) {
      return b.lo - a.hi;
    }
    if (b.hi < a.lo) {
      return a.lo - b.hi;
    }
    return 0.0;
  }

  static double maxDistance(const BoundsType& a, const BoundsType& b) {
    return std::max(a.hi, b.hi) - std::min(a.lo, b.lo);
  }

  static double getX(const BoundsType& a) { return a.lo + a.hi; }

  static double getY(const BoundsType& a) { return a.lo + a.hi; }

  static void expandToInclude(BoundsType& a, const BoundsType& b) {
    a.lo = std::min(a.lo, b.lo);
    a.hi = std::max(a.hi, b.hi);
  }

  static bool isNull(const BoundsType&) { return false; }
};

struct ValItem {
  int id;
  double pos;
};

struct ValItemDistance {
  int calls = 0;

  double operator()(const ValItem& a, const ValItem& b) {
    ++calls;
    return std::abs(a.pos - b.pos);
  }
};

TEST(TemplateSTRtreeDistanceTest,
     IsWithinDistanceBoundaryAndSpecialMaxDistance) {
  // 1. maxDistance == 0.0 cases (coincident points, overlapping boxes with
  //    coincident anchors, and overlapping boxes with separated anchors).
  {
    const BoxItem p1 = MakePointItem(1, 5.0, 5.0);
    const BoxItem p2 = MakePointItem(2, 5.0, 5.0);
    const BoxNode n1(&p1, p1.env);
    const BoxNode n2(&p2, p2.env);
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    EXPECT_TRUE(td.isWithinDistance(n1, n2, 0.0));
    EXPECT_EQ(dist.calls, 1);

    const std::vector<BoxItem> coincident_items = {
        MakePointItem(10, 5.0, 5.0),
        MakePointItem(11, 5.0, 5.0),
    };
    BoxTree coincident_tree(2);
    InsertAll(coincident_tree, coincident_items);
    ASSERT_TRUE(coincident_tree.getRoot()->isComposite());
    AnchorDistance comp_dist;
    BoxTreeDistance comp_td(comp_dist);
    EXPECT_TRUE(comp_td.isWithinDistance(*coincident_tree.getRoot(), n2, 0.0));
    EXPECT_EQ(comp_dist.calls, 0);
  }
  {
    const BoxItem b1 = MakeBoxItem(1, 0.0, 4.0, 0.0, 4.0, 2.0, 2.0);
    const BoxItem b2 = MakeBoxItem(2, 1.0, 5.0, 1.0, 5.0, 2.0, 2.0);
    const BoxNode n1(&b1, b1.env);
    const BoxNode n2(&b2, b2.env);
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    EXPECT_TRUE(td.isWithinDistance(n1, n2, 0.0));
    EXPECT_EQ(dist.calls, 1);
  }
  {
    const BoxItem b1 = MakeBoxItem(3, 0.0, 4.0, 0.0, 4.0, 0.5, 2.0);
    const BoxItem b2 = MakeBoxItem(4, 1.0, 5.0, 1.0, 5.0, 4.5, 2.0);
    const BoxNode n1(&b1, b1.env);
    const BoxNode n2(&b2, b2.env);
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    EXPECT_FALSE(td.isWithinDistance(n1, n2, 0.0));
    EXPECT_EQ(dist.calls, 1);
  }

  // 2. +infinity, -infinity, and NaN maxDistance values.
  const std::vector<BoxItem> items1 = {
      MakeBoxItem(1, 0.0, 4.0, 0.0, 4.0, 1.0, 2.0),
      MakeBoxItem(2, 0.0, 4.0, 10.0, 14.0, 1.0, 12.0),
      MakeBoxItem(3, 50.0, 54.0, 0.0, 4.0, 51.0, 2.0),
      MakeBoxItem(4, 50.0, 54.0, 10.0, 14.0, 51.0, 12.0),
  };
  const std::vector<BoxItem> items2 = {
      MakeBoxItem(11, 2.0, 6.0, 0.0, 4.0, 5.0, 2.0),
      MakeBoxItem(12, 2.0, 6.0, 10.0, 14.0, 5.0, 12.0),
      MakeBoxItem(13, 60.0, 64.0, 0.0, 4.0, 63.0, 2.0),
      MakeBoxItem(14, 60.0, 64.0, 10.0, 14.0, 63.0, 12.0),
  };
  BoxTree tree1(2);
  BoxTree tree2(2);
  InsertAll(tree1, items1);
  InsertAll(tree2, items2);

  {
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    const double pos_inf = std::numeric_limits<double>::infinity();
    const double neg_inf = -std::numeric_limits<double>::infinity();
    EXPECT_TRUE(
        td.isWithinDistance(*tree1.getRoot(), *tree2.getRoot(), pos_inf));
    EXPECT_FALSE(
        td.isWithinDistance(*tree1.getRoot(), *tree2.getRoot(), neg_inf));
    EXPECT_EQ(dist.calls, 0);
  }
  {
    const double nan_val = std::numeric_limits<double>::quiet_NaN();
    const BoxNode leaf1(&items1[0], items1[0].env);
    const BoxNode leaf2(&items2[0], items2[0].env);
    AnchorDistance leaf_dist;
    BoxTreeDistance leaf_td(leaf_dist);
    EXPECT_FALSE(leaf_td.isWithinDistance(leaf1, leaf2, nan_val));
    EXPECT_EQ(leaf_dist.calls, 1);

    AnchorDistance tree_dist;
    BoxTreeDistance tree_td(tree_dist);
    EXPECT_FALSE(
        tree_td.isWithinDistance(*tree1.getRoot(), *tree2.getRoot(), nan_val));
    EXPECT_GT(tree_dist.calls, 0);
  }
}

TEST(TemplateSTRtreeDistanceTest,
     SelfTreeNearestNeighbourWithDistinctDistance) {
  const std::vector<BoxItem> single_items = {
      MakePointItem(1, 3.0, 4.0),
  };
  BoxTree single_tree(2);
  InsertAll(single_tree, single_items);
  {
    DistinctAnchorDistance dist;
    TemplateSTRtreeDistance<const BoxItem*, EnvelopeTraits,
                            DistinctAnchorDistance>
        td(dist);
    const auto nearest =
        td.nearestNeighbour(*single_tree.getRoot(), *single_tree.getRoot());
    EXPECT_EQ(nearest.first, &single_items[0]);
    EXPECT_EQ(nearest.second, &single_items[0]);
    EXPECT_EQ(dist.calls, 1);
  }

  const std::vector<BoxItem> items = {
      MakePointItem(1, 0.0, 0.0),  MakePointItem(2, 10.0, 0.0),
      MakePointItem(3, 20.0, 0.0), MakePointItem(4, 20.0, 1.0),
      MakePointItem(5, 30.0, 5.0), MakePointItem(6, 40.0, 5.0),
      MakePointItem(7, 50.0, 5.0), MakePointItem(8, 60.0, 5.0),
  };
  BoxTree tree(3);
  InsertAll(tree, items);

  DistinctAnchorDistance dist;
  TemplateSTRtreeDistance<const BoxItem*, EnvelopeTraits,
                          DistinctAnchorDistance>
      td(dist);
  const auto nearest = td.nearestNeighbour(*tree.getRoot(), *tree.getRoot());
  ASSERT_NE(nearest.first, nullptr);
  ASSERT_NE(nearest.second, nullptr);
  EXPECT_NE(nearest.first, nearest.second);
  EXPECT_DOUBLE_EQ(nearest.first->anchor.distance(nearest.second->anchor), 1.0);
  EXPECT_LT(dist.calls, static_cast<int>(items.size() * items.size()));
}

TEST(TemplateSTRtreeDistanceTest, MultiLevelCapacityFourMatchesBruteForce) {
  std::vector<BoxItem> lhs;
  std::vector<BoxItem> rhs;
  for (int i = 0; i < 25; ++i) {
    const double x = static_cast<double>(i % 5) * 12.0;
    const double y = static_cast<double>(i / 5) * 12.0;
    lhs.push_back(
        MakeBoxItem(100 + i, x, x + 4.0, y, y + 4.0, x + 1.0, y + 1.0));
    rhs.push_back(MakeBoxItem(200 + i, x + 3.0, x + 7.0, y + 5.0, y + 9.0,
                              x + 6.0, y + 8.0));
  }

  BoxTree tree1(4);
  BoxTree tree2(4);
  InsertAll(tree1, lhs);
  InsertAll(tree2, rhs);
  ASSERT_TRUE(tree1.getRoot()->isComposite());
  ASSERT_TRUE(tree2.getRoot()->isComposite());

  const double expected = BruteForceMinDistance(lhs, rhs);
  AnchorDistance nn_dist;
  BoxTreeDistance nn_td(nn_dist);
  const auto nearest =
      nn_td.nearestNeighbour(*tree1.getRoot(), *tree2.getRoot());
  EXPECT_DOUBLE_EQ(nearest.first->anchor.distance(nearest.second->anchor),
                   expected);
  EXPECT_LT(nn_dist.calls, static_cast<int>(lhs.size() * rhs.size()));

  AnchorDistance wd_dist;
  BoxTreeDistance wd_td(wd_dist);
  EXPECT_FALSE(wd_td.isWithinDistance(*tree1.getRoot(), *tree2.getRoot(),
                                      expected - 1e-6));
  EXPECT_TRUE(
      wd_td.isWithinDistance(*tree1.getRoot(), *tree2.getRoot(), expected));
}

TEST(TemplateSTRtreeDistanceTest, CustomBoundsTraitsAndValueItemType) {
  TemplateSTRtree<ValItem, Interval1DTraits> tree1(3);
  TemplateSTRtree<ValItem, Interval1DTraits> tree2(3);

  for (int i = 0; i < 10; ++i) {
    const double start1 = static_cast<double>(i) * 10.0;
    tree1.insert(Interval1D{start1, start1 + 4.0}, ValItem{i, start1 + 1.0});

    if (i == 5) {
      tree2.insert(Interval1D{51.0, 54.0}, ValItem{105, 52.5});
    } else {
      const double start2 = static_cast<double>(i) * 10.0 + 4.5;
      tree2.insert(Interval1D{start2, start2 + 4.5},
                   ValItem{100 + i, start2 + 3.5});
    }
  }

  ASSERT_TRUE(tree1.getRoot()->isComposite());
  ASSERT_TRUE(tree2.getRoot()->isComposite());

  ValItemDistance dist;
  TemplateSTRtreeDistance<ValItem, Interval1DTraits, ValItemDistance> td(dist);
  const auto nearest = td.nearestNeighbour(*tree1.getRoot(), *tree2.getRoot());
  EXPECT_EQ(nearest.first.id, 5);
  EXPECT_EQ(nearest.second.id, 105);
  EXPECT_DOUBLE_EQ(std::abs(nearest.first.pos - nearest.second.pos), 1.5);

  EXPECT_FALSE(td.isWithinDistance(*tree1.getRoot(), *tree2.getRoot(), 1.49));
  EXPECT_TRUE(td.isWithinDistance(*tree1.getRoot(), *tree2.getRoot(), 1.5));
}

TEST(TemplateSTRtreeDistanceTest,
     ExpandFiltersWithFiniteUpperBoundAndCompositeNodePairOverload) {
  const std::vector<BoxItem> items = {
      MakeBoxItem(1, 0.0, 1.0, 0.0, 1.0, 0.5, 0.5),
      MakeBoxItem(2, 2.0, 3.0, 0.0, 1.0, 2.5, 0.5),
      MakeBoxItem(3, 25.0, 26.0, 0.0, 1.0, 25.5, 0.5),
      MakeBoxItem(4, 35.0, 36.0, 0.0, 1.0, 35.5, 0.5),
  };
  const BoxItem target = MakeBoxItem(10, 0.0, 1.0, 0.0, 1.0, 0.5, 0.5);
  BoxTree tree(2);
  InsertAll(tree, items);
  ASSERT_TRUE(tree.getRoot()->isComposite());
  const BoxNode leaf_target(&target, target.env);

  // Exercises nearestNeighbour(NodePair&) directly when one side is composite.
  {
    AnchorDistance dist;
    BoxTreeDistance td(dist);
    BoxNodePair init_pair(*tree.getRoot(), leaf_target, dist);
    const auto nearest = td.nearestNeighbour(init_pair);
    EXPECT_EQ(nearest.first->id, 1);
    EXPECT_EQ(nearest.second->id, 10);
  }

  // Exercises expand() with a finite minDistance (distanceUpperBound) on both
  // acceptance (sp.getDistance() < minDistance) and pruning (>= minDistance),
  // in both non-flipped and flipped orientations.
  struct IdTableDistance {
    int calls = 0;

    double operator()(const BoxItem* a, const BoxItem* b) {
      ++calls;
      const int item_id = (a->id == 10) ? b->id : a->id;
      switch (item_id) {
        case 1:
          return 5.0;
        case 2:
          return 20.0;
        case 3:
          return 2.0;
        case 4:
          return 30.0;
        default:
          return 100.0;
      }
    }
  };

  const double nan_val = std::numeric_limits<double>::quiet_NaN();
  {
    IdTableDistance dist;
    TemplateSTRtreeDistance<const BoxItem*, EnvelopeTraits, IdTableDistance> td(
        dist);
    EXPECT_FALSE(td.isWithinDistance(*tree.getRoot(), leaf_target, nan_val));
    EXPECT_EQ(dist.calls, 4);
  }
  {
    IdTableDistance dist;
    TemplateSTRtreeDistance<const BoxItem*, EnvelopeTraits, IdTableDistance> td(
        dist);
    EXPECT_FALSE(td.isWithinDistance(leaf_target, *tree.getRoot(), nan_val));
    EXPECT_EQ(dist.calls, 4);
  }
}

}  // namespace
}  // namespace geos_google
