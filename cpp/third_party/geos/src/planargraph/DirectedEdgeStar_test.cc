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

#include "third_party/geos/include/geos/planargraph/DirectedEdgeStar.h"

#include <cmath>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdge.h"
#include "third_party/geos/include/geos/planargraph/Edge.h"
#include "third_party/geos/include/geos/planargraph/Node.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::planargraph::DirectedEdge;
using ::geos::planargraph::DirectedEdgeStar;
using ::geos::planargraph::Edge;
using ::geos::planargraph::Node;
using ::geos::planargraph::pdeLessThan;

TEST(DirectedEdgeStarTest, EmptyStarState) {
  DirectedEdgeStar star;
  const DirectedEdgeStar& const_star = star;

  EXPECT_EQ(star.getDegree(), 0U);
  EXPECT_TRUE(star.getEdges().empty());
  EXPECT_EQ(star.begin(), star.end());
  EXPECT_EQ(star.iterator(), star.end());
  EXPECT_EQ(const_star.begin(), const_star.end());

  const Coordinate& coord = star.getCoordinate();
  EXPECT_TRUE(coord.isNull());
  EXPECT_TRUE(std::isnan(coord.x));
  EXPECT_TRUE(std::isnan(coord.y));

  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(1.0, 0.0));
  DirectedEdge de(&n0, &n1, Coordinate(1.0, 0.0), true);
  de.setEdge(nullptr);
  Edge edge;

  EXPECT_EQ(star.getIndex(&edge), -1);
  EXPECT_EQ(star.getIndex(static_cast<const Edge*>(nullptr)), -1);
  EXPECT_EQ(star.getIndex(&de), -1);
  EXPECT_EQ(star.getIndex(static_cast<const DirectedEdge*>(nullptr)), -1);

  // Removing from an empty star is a safe no-op.
  star.remove(&de);
  EXPECT_EQ(star.getDegree(), 0U);
}

TEST(DirectedEdgeStarTest, AddSortIterateAndReSortAfterAdd) {
  Node origin(Coordinate(2.0, 3.0));
  Node dst(Coordinate(0.0, 0.0));

  // Sorted CCW order from positive x-axis:
  // de_q0_x (0 deg), de_q0_45 (45 deg), de_q0_y (90 deg),
  // de_q1_mid (135 deg), de_q1_nx (180 deg),
  // de_q2_mid (225 deg),
  // de_q3_ny (270 deg), de_q3_mid (315 deg).
  DirectedEdge de_q0_x(&origin, &dst, Coordinate(4.0, 3.0), true);
  DirectedEdge de_q0_45(&origin, &dst, Coordinate(3.0, 4.0), true);
  DirectedEdge de_q0_y(&origin, &dst, Coordinate(2.0, 5.0), true);
  DirectedEdge de_q1_mid(&origin, &dst, Coordinate(1.0, 4.0), true);
  DirectedEdge de_q1_nx(&origin, &dst, Coordinate(0.0, 3.0), true);
  DirectedEdge de_q2_mid(&origin, &dst, Coordinate(1.0, 2.0), true);
  DirectedEdge de_q3_ny(&origin, &dst, Coordinate(2.0, 1.0), true);
  DirectedEdge de_q3_mid(&origin, &dst, Coordinate(3.0, 2.0), true);

  DirectedEdgeStar star;
  // Insert in scrambled order.
  star.add(&de_q2_mid);
  star.add(&de_q0_y);
  star.add(&de_q3_mid);
  star.add(&de_q1_nx);
  star.add(&de_q0_x);
  star.add(&de_q3_ny);
  star.add(&de_q1_mid);
  star.add(&de_q0_45);

  EXPECT_EQ(star.getDegree(), 8U);
  EXPECT_EQ(&star.getCoordinate(), &origin.getCoordinate());
  EXPECT_TRUE(star.getCoordinate().equals2D(Coordinate(2.0, 3.0)));

  const std::vector<DirectedEdge*> expected_sorted = {
      &de_q0_x,  &de_q0_45,  &de_q0_y,  &de_q1_mid,
      &de_q1_nx, &de_q2_mid, &de_q3_ny, &de_q3_mid,
  };
  EXPECT_EQ(star.getEdges(), expected_sorted);

  // Calling getEdges() again exercises the already-sorted branch.
  EXPECT_EQ(star.getEdges(), expected_sorted);

  // Non-const iterator(), begin()/end(), and const begin()/end().
  std::vector<DirectedEdge*> iter_edges;
  for (auto it = star.iterator(); it != star.end(); ++it) {
    iter_edges.push_back(*it);
  }
  EXPECT_EQ(iter_edges, expected_sorted);

  const DirectedEdgeStar& const_star = star;
  std::vector<const DirectedEdge*> const_iter_edges;
  for (auto it = const_star.begin(); it != const_star.end(); ++it) {
    const_iter_edges.push_back(*it);
  }
  EXPECT_EQ(const_iter_edges,
            (std::vector<const DirectedEdge*>(expected_sorted.begin(),
                                              expected_sorted.end())));

  // Adding a new edge after sorting must mark the star unsorted so the next
  // access inserts it in angle order.
  DirectedEdge de_q0_22(&origin, &dst, Coordinate(4.0, 4.0), true);
  star.add(&de_q0_22);
  EXPECT_EQ(star.getDegree(), 9U);

  const std::vector<DirectedEdge*> expected_after_add = {
      &de_q0_x,  &de_q0_22,  &de_q0_45, &de_q0_y,   &de_q1_mid,
      &de_q1_nx, &de_q2_mid, &de_q3_ny, &de_q3_mid,
  };
  std::vector<DirectedEdge*> non_const_iter_edges;
  for (DirectedEdge* de : star) {
    non_const_iter_edges.push_back(de);
  }
  EXPECT_EQ(non_const_iter_edges, expected_after_add);

  // Adding another edge and iterating through the const view must also sort.
  DirectedEdge de_q1_110(&origin, &dst, Coordinate(1.0, 6.0), true);
  star.add(&de_q1_110);
  std::vector<const DirectedEdge*> const_resort_edges;
  for (auto it = const_star.begin(); it != const_star.end(); ++it) {
    const_resort_edges.push_back(*it);
  }
  const std::vector<const DirectedEdge*> expected_after_const_resort = {
      &de_q0_x,   &de_q0_22, &de_q0_45,  &de_q0_y,  &de_q1_110,
      &de_q1_mid, &de_q1_nx, &de_q2_mid, &de_q3_ny, &de_q3_mid,
  };
  EXPECT_EQ(const_resort_edges, expected_after_const_resort);
}

TEST(DirectedEdgeStarTest, PdeLessThanComparator) {
  Node origin(Coordinate(0.0, 0.0));
  Node dst(Coordinate(1.0, 1.0));

  DirectedEdge de_east(&origin, &dst, Coordinate(1.0, 0.0), true);
  DirectedEdge de_ne(&origin, &dst, Coordinate(1.0, 1.0), true);
  DirectedEdge de_ne_scaled(&origin, &dst, Coordinate(3.0, 3.0), true);
  DirectedEdge de_sw(&origin, &dst, Coordinate(-1.0, -1.0), true);

  EXPECT_TRUE(pdeLessThan(&de_east, &de_ne));
  EXPECT_TRUE(pdeLessThan(&de_ne, &de_sw));
  EXPECT_FALSE(pdeLessThan(&de_ne, &de_east));
  EXPECT_FALSE(pdeLessThan(&de_sw, &de_ne));
  EXPECT_FALSE(pdeLessThan(&de_ne, &de_ne_scaled));
  EXPECT_FALSE(pdeLessThan(&de_ne_scaled, &de_ne));
}

TEST(DirectedEdgeStarTest, RemoveFrontMiddleBackDuplicatesAndMissing) {
  Node origin(Coordinate(0.0, 0.0));
  Node dst(Coordinate(1.0, 0.0));

  DirectedEdge de0(&origin, &dst, Coordinate(1.0, 0.0), true);
  DirectedEdge de1(&origin, &dst, Coordinate(0.0, 1.0), true);
  DirectedEdge de2(&origin, &dst, Coordinate(-1.0, 0.0), true);
  DirectedEdge de3(&origin, &dst, Coordinate(0.0, -1.0), true);
  DirectedEdge missing(&origin, &dst, Coordinate(1.0, 1.0), true);

  DirectedEdgeStar star;
  star.add(&de0);
  star.add(&de1);
  star.add(&de0);  // Duplicate entry at non-zero index.
  star.add(&de2);
  star.add(&de3);

  // Removing a missing pointer or nullptr does nothing.
  star.remove(&missing);
  star.remove(nullptr);
  EXPECT_EQ(star.getDegree(), 5U);

  // Removing de0 removes index 0 (unsigned --i wraparound) and its duplicate.
  star.remove(&de0);
  EXPECT_EQ(star.getDegree(), 3U);
  EXPECT_EQ(star.getEdges(), (std::vector<DirectedEdge*>{&de1, &de2, &de3}));

  // Remove middle and last after sorting; star order remains consistent.
  star.remove(&de2);
  EXPECT_EQ(star.getEdges(), (std::vector<DirectedEdge*>{&de1, &de3}));
  EXPECT_EQ(star.getNextEdge(&de1), &de3);
  EXPECT_EQ(star.getNextEdge(&de3), &de1);
  star.remove(&de3);
  EXPECT_EQ(star.getEdges(), (std::vector<DirectedEdge*>{&de1}));
  star.remove(&de1);
  EXPECT_EQ(star.getDegree(), 0U);
}

TEST(DirectedEdgeStarTest, GetIndexByEdgeAndDirectedEdge) {
  Node origin(Coordinate(0.0, 0.0));
  Node dst(Coordinate(1.0, 0.0));

  DirectedEdge de0(&origin, &dst, Coordinate(1.0, 0.0), true);   // sorted idx 0
  DirectedEdge de1(&origin, &dst, Coordinate(-1.0, 1.0), true);  // sorted idx 1
  DirectedEdge de2(&origin, &dst, Coordinate(1.0, -1.0), true);  // sorted idx 2
  DirectedEdge outside(&origin, &dst, Coordinate(0.0, 1.0), true);

  Edge edge0;
  Edge edge2;
  Edge unrelated_edge;
  de0.setEdge(&edge0);
  de1.setEdge(nullptr);
  de2.setEdge(&edge2);

  DirectedEdgeStar star;
  // Add out of sorted order so getIndex must sort first.
  star.add(&de2);
  star.add(&de0);
  star.add(&de1);

  EXPECT_EQ(star.getIndex(&edge0), 0);
  EXPECT_EQ(star.getIndex(static_cast<const Edge*>(nullptr)), 1);
  EXPECT_EQ(star.getIndex(&edge2), 2);
  EXPECT_EQ(star.getIndex(&unrelated_edge), -1);

  EXPECT_EQ(star.getIndex(&de0), 0);
  EXPECT_EQ(star.getIndex(&de1), 1);
  EXPECT_EQ(star.getIndex(&de2), 2);
  EXPECT_EQ(star.getIndex(&outside), -1);
  EXPECT_EQ(star.getIndex(static_cast<const DirectedEdge*>(nullptr)), -1);
}

TEST(DirectedEdgeStarTest, GetIndexModularWraparound) {
  Node origin(Coordinate(0.0, 0.0));
  Node dst(Coordinate(1.0, 0.0));

  DirectedEdge de0(&origin, &dst, Coordinate(1.0, 0.0), true);
  DirectedEdge de1(&origin, &dst, Coordinate(0.0, 1.0), true);
  DirectedEdge de2(&origin, &dst, Coordinate(-1.0, 0.0), true);
  DirectedEdge de3(&origin, &dst, Coordinate(0.0, -1.0), true);

  DirectedEdgeStar star;
  star.add(&de0);
  star.add(&de1);
  star.add(&de2);
  star.add(&de3);

  const DirectedEdgeStar& const_star = star;
  int idx_zero = 0;
  EXPECT_EQ(star.getIndex(idx_zero), 0U);
  EXPECT_EQ(const_star.getIndex(0), 0U);
  EXPECT_EQ(star.getIndex(1), 1U);
  EXPECT_EQ(star.getIndex(3), 3U);
  EXPECT_EQ(star.getIndex(4), 0U);
  EXPECT_EQ(star.getIndex(7), 3U);
  EXPECT_EQ(star.getIndex(-1), 3U);
  EXPECT_EQ(star.getIndex(-2), 2U);
  EXPECT_EQ(star.getIndex(-4), 0U);
  EXPECT_EQ(star.getIndex(-5), 3U);
  EXPECT_EQ(star.getIndex(-8), 0U);

  DirectedEdgeStar single_star;
  single_star.add(&de0);
  const DirectedEdgeStar& const_single_star = single_star;
  EXPECT_EQ(single_star.getIndex(idx_zero), 0U);
  EXPECT_EQ(const_single_star.getIndex(0), 0U);
  EXPECT_EQ(single_star.getIndex(5), 0U);
  EXPECT_EQ(single_star.getIndex(-5), 0U);
}

TEST(DirectedEdgeStarTest, GetNextEdgeCyclesAroundStar) {
  Node origin(Coordinate(0.0, 0.0));
  Node dst(Coordinate(1.0, 0.0));

  DirectedEdge de_ne(&origin, &dst, Coordinate(1.0, 1.0), true);
  DirectedEdge de_nw(&origin, &dst, Coordinate(-1.0, 1.0), true);
  DirectedEdge de_sw(&origin, &dst, Coordinate(-1.0, -1.0), true);
  DirectedEdge de_se(&origin, &dst, Coordinate(1.0, -1.0), true);

  DirectedEdgeStar single_star;
  single_star.add(&de_ne);
  EXPECT_EQ(single_star.getNextEdge(&de_ne), &de_ne);

  DirectedEdgeStar star;
  star.add(&de_sw);
  star.add(&de_ne);
  star.add(&de_se);
  star.add(&de_nw);

  EXPECT_EQ(star.getNextEdge(&de_ne), &de_nw);
  EXPECT_EQ(star.getNextEdge(&de_nw), &de_sw);
  EXPECT_EQ(star.getNextEdge(&de_sw), &de_se);
  EXPECT_EQ(star.getNextEdge(&de_se), &de_ne);
}

}  // namespace
}  // namespace geos_google
