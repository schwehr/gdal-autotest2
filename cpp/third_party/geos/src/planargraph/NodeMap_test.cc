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

#include "third_party/geos/include/geos/planargraph/NodeMap.h"

#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/planargraph/Node.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::planargraph::Node;
using ::geos::planargraph::NodeMap;

TEST(NodeMapTest, EmptyNodeMapState) {
  NodeMap node_map;
  const NodeMap& const_node_map = node_map;

  EXPECT_TRUE(node_map.getNodeMap().empty());
  EXPECT_EQ(node_map.begin(), node_map.end());
  EXPECT_EQ(node_map.iterator(), node_map.end());
  EXPECT_EQ(const_node_map.begin(), const_node_map.end());

  EXPECT_EQ(node_map.find(Coordinate(0.0, 0.0)), nullptr);
  Coordinate missing(1.0, 2.0);
  EXPECT_EQ(node_map.remove(missing), nullptr);

  std::vector<Node*> nodes;
  node_map.getNodes(nodes);
  EXPECT_TRUE(nodes.empty());
}

TEST(NodeMapTest, AddFindAndDuplicateCoordinateHandling) {
  Node n0(Coordinate(1.0, 2.0, 5.0));
  Node n1(Coordinate(-1.0, 3.0));
  Node n2(Coordinate(1.0, 0.5));
  Node n0_same_xy(Coordinate(1.0, 2.0, 99.0));

  NodeMap node_map;
  EXPECT_EQ(node_map.add(&n0), &n0);
  EXPECT_EQ(node_map.add(&n1), &n1);
  EXPECT_EQ(node_map.add(&n2), &n2);
  EXPECT_EQ(node_map.getNodeMap().size(), 3U);

  EXPECT_EQ(node_map.find(Coordinate(1.0, 2.0, 5.0)), &n0);
  // CoordinateLessThan orders purely by 2D (x, y), ignoring Z.
  EXPECT_EQ(node_map.find(Coordinate(1.0, 2.0, 99.0)), &n0);
  EXPECT_EQ(node_map.find(Coordinate(-1.0, 3.0)), &n1);
  EXPECT_EQ(node_map.find(Coordinate(1.0, 0.5)), &n2);
  EXPECT_EQ(node_map.find(Coordinate(9.0, 9.0)), nullptr);

  // NodeMap::add delegates to std::map::insert, which retains the existing
  // entry at the same 2D key while returning the passed node pointer.
  EXPECT_EQ(node_map.add(&n0_same_xy), &n0_same_xy);
  EXPECT_EQ(node_map.getNodeMap().size(), 3U);
  EXPECT_EQ(node_map.find(Coordinate(1.0, 2.0)), &n0);
}

TEST(NodeMapTest, IterationAndGetNodesOrderedByCoordinate) {
  Node n_3_1(Coordinate(3.0, 1.0));
  Node n_1_5(Coordinate(1.0, 5.0));
  Node n_1_2(Coordinate(1.0, 2.0));
  Node n_2_m1(Coordinate(2.0, -1.0));

  NodeMap node_map;
  node_map.add(&n_3_1);
  node_map.add(&n_1_5);
  node_map.add(&n_1_2);
  node_map.add(&n_2_m1);

  const std::vector<Node*> expected_order = {
      &n_1_2,
      &n_1_5,
      &n_2_m1,
      &n_3_1,
  };

  std::vector<Node*> via_begin;
  for (auto it = node_map.begin(); it != node_map.end(); ++it) {
    via_begin.push_back(it->second);
  }
  EXPECT_EQ(via_begin, expected_order);

  std::vector<Node*> via_iterator;
  for (auto it = node_map.iterator(); it != node_map.end(); ++it) {
    via_iterator.push_back(it->second);
  }
  EXPECT_EQ(via_iterator, expected_order);

  const NodeMap& const_node_map = node_map;
  std::vector<Node*> via_const_begin;
  for (auto it = const_node_map.begin(); it != const_node_map.end(); ++it) {
    via_const_begin.push_back(it->second);
  }
  EXPECT_EQ(via_const_begin, expected_order);

  // getNodes appends to any pre-existing elements in the output vector.
  Node sentinel(Coordinate(-9.0, -9.0));
  std::vector<Node*> collected = {&sentinel};
  node_map.getNodes(collected);
  EXPECT_EQ(collected,
            (std::vector<Node*>{&sentinel, &n_1_2, &n_1_5, &n_2_m1, &n_3_1}));
}

TEST(NodeMapTest, RemoveExistingAndMissingNodes) {
  Node n0(Coordinate(0.0, 0.0, 1.0));
  Node n1(Coordinate(2.0, 3.0));
  Node n2(Coordinate(4.0, 5.0));

  NodeMap node_map;
  node_map.add(&n0);
  node_map.add(&n1);
  node_map.add(&n2);

  Coordinate missing(9.0, 9.0);
  EXPECT_EQ(node_map.remove(missing), nullptr);
  EXPECT_EQ(node_map.getNodeMap().size(), 3U);

  Coordinate pt0_different_z(0.0, 0.0, 42.0);
  EXPECT_EQ(node_map.remove(pt0_different_z), &n0);
  EXPECT_EQ(node_map.find(Coordinate(0.0, 0.0)), nullptr);
  EXPECT_EQ(node_map.remove(pt0_different_z), nullptr);
  EXPECT_EQ(node_map.getNodeMap().size(), 2U);

  Coordinate pt1(2.0, 3.0);
  Coordinate pt2(4.0, 5.0);
  EXPECT_EQ(node_map.remove(pt1), &n1);
  EXPECT_EQ(node_map.remove(pt2), &n2);
  EXPECT_TRUE(node_map.getNodeMap().empty());
}

TEST(NodeMapTest, VirtualDestructorDoesNotDestroyExternalNodes) {
  Node node(Coordinate(4.0, 5.0, 6.0));
  std::unique_ptr<NodeMap> node_map = std::make_unique<NodeMap>();
  node_map->add(&node);
  EXPECT_EQ(node_map->find(Coordinate(4.0, 5.0)), &node);

  node_map.reset();
  EXPECT_TRUE(node.getCoordinate().equals3D(Coordinate(4.0, 5.0, 6.0)));
}

}  // namespace
}  // namespace geos_google
