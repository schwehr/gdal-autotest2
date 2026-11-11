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

// Unit tests for third_party/geos/src/geomgraph/NodeMap.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geomgraph/NodeMap.cpp

#include "third_party/geos/include/geos/geomgraph/NodeMap.h"

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEndStar.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/NodeFactory.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Location;
using geos::geomgraph::EdgeEnd;
using geos::geomgraph::EdgeEndStar;
using geos::geomgraph::Label;
using geos::geomgraph::Node;
using geos::geomgraph::NodeFactory;
using geos::geomgraph::NodeMap;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

class NonOwningEdgeEndStar final : public EdgeEndStar {
 public:
  void insert(EdgeEnd* e) override { insertEdgeEnd(e); }
};

class OwningEdgeEndStar final : public EdgeEndStar {
 public:
  ~OwningEdgeEndStar() override {
    for (EdgeEnd* e : edgeMap) {
      delete e;
    }
  }
  void insert(EdgeEnd* e) override { insertEdgeEnd(e); }
};

class NonOwningStarNodeFactory final : public NodeFactory {
 public:
  Node* createNode(const Coordinate& coord) const override {
    return new Node(coord, new NonOwningEdgeEndStar());
  }
};

class OwningStarNodeFactory final : public NodeFactory {
 public:
  Node* createNode(const Coordinate& coord) const override {
    return new Node(coord, new OwningEdgeEndStar());
  }
};

// Calls NodeMap::addNode(Node*) and releases ownership only when NodeMap stores
// the passed Node directly rather than merging into an existing node.
Node* AddOrMergeNode(NodeMap& map, std::unique_ptr<Node> node) {
  Node* raw = node.get();
  Node* result = map.addNode(raw);
  if (result == raw) {
    node.release();
  }
  return result;
}

TEST(NodeMapTest, EmptyMapStateAndIterators) {
  NodeMap map(NodeFactory::instance());

  EXPECT_EQ(map.find(Coordinate(0.0, 0.0)), nullptr);
  EXPECT_EQ(map.begin(), map.end());

  const NodeMap& const_map = map;
  EXPECT_EQ(const_map.begin(), const_map.end());

  std::vector<Node*> boundary_nodes;
  map.getBoundaryNodes(0, boundary_nodes);
  EXPECT_THAT(boundary_nodes, IsEmpty());
  EXPECT_EQ(map.print(), "");

  map.testInvariant();
}

TEST(NodeMapTest, AddNodeByCoordinateCreatesAndAveragesZByXY) {
  NodeMap map(NodeFactory::instance());

  Node* n1 = map.addNode(Coordinate(1.0, 2.0, 10.0));
  ASSERT_NE(n1, nullptr);
  EXPECT_TRUE(n1->getCoordinate().equals3D(Coordinate(1.0, 2.0, 10.0)));
  EXPECT_EQ(map.find(Coordinate(1.0, 2.0)), n1);

  Node* n1_again = map.addNode(Coordinate(1.0, 2.0, 20.0));
  EXPECT_EQ(n1_again, n1);
  EXPECT_DOUBLE_EQ(n1->getCoordinate().z, 15.0);
  EXPECT_THAT(n1->getZ(), ElementsAre(10.0, 20.0));

  // Missing (NaN) Z on duplicate XY coordinate leaves accumulated Z unchanged.
  Node* n1_nan_z = map.addNode(Coordinate(1.0, 2.0));
  EXPECT_EQ(n1_nan_z, n1);
  EXPECT_DOUBLE_EQ(n1->getCoordinate().z, 15.0);

  Node* n2 = map.addNode(Coordinate(3.0, 4.0));
  ASSERT_NE(n2, nullptr);
  EXPECT_NE(n2, n1);
  EXPECT_TRUE(n2->getCoordinate().equals2D(Coordinate(3.0, 4.0)));
  EXPECT_TRUE(std::isnan(n2->getCoordinate().z));
  EXPECT_EQ(map.find(Coordinate(3.0, 4.0)), n2);

  map.testInvariant();
}

TEST(NodeMapTest, AddNodeByPointerInsertsNewAndMergesExistingLabels) {
  NodeMap map(NodeFactory::instance());

  auto first = std::make_unique<Node>(Coordinate(5.0, 6.0), nullptr);
  first->setLabel(0, Location::INTERIOR);
  Node* stored = AddOrMergeNode(map, std::move(first));
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(map.find(Coordinate(5.0, 6.0)), stored);
  EXPECT_EQ(stored->getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(stored->getLabel().getLocation(1), Location::NONE);

  // Adding another node at the same 2D coordinate merges non-null locations for
  // any geometry index currently Location::NONE.
  auto duplicate = std::make_unique<Node>(Coordinate(5.0, 6.0), nullptr);
  duplicate->setLabel(0, Location::BOUNDARY);
  duplicate->setLabel(1, Location::BOUNDARY);
  Node* merged = AddOrMergeNode(map, std::move(duplicate));
  EXPECT_EQ(merged, stored);
  EXPECT_EQ(stored->getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(stored->getLabel().getLocation(1), Location::BOUNDARY);

  map.testInvariant();
}

TEST(NodeMapTest, AddRawEdgeEndCreatesNodeAndAttachesToStar) {
  std::vector<std::unique_ptr<EdgeEnd>> owned_ends;
  NonOwningStarNodeFactory factory;
  NodeMap map(factory);

  owned_ends.push_back(std::make_unique<EdgeEnd>(nullptr, Coordinate(0.0, 0.0),
                                                 Coordinate(1.0, 0.0)));
  owned_ends.push_back(std::make_unique<EdgeEnd>(nullptr, Coordinate(0.0, 0.0),
                                                 Coordinate(0.0, 1.0)));
  owned_ends.push_back(std::make_unique<EdgeEnd>(nullptr, Coordinate(2.0, 2.0),
                                                 Coordinate(3.0, 2.0)));

  map.add(owned_ends[0].get());
  map.add(owned_ends[1].get());
  map.add(owned_ends[2].get());

  Node* origin = map.find(Coordinate(0.0, 0.0));
  ASSERT_NE(origin, nullptr);
  ASSERT_NE(origin->getEdges(), nullptr);
  EXPECT_EQ(origin->getEdges()->getDegree(), 2u);
  EXPECT_EQ(owned_ends[0]->getNode(), origin);
  EXPECT_EQ(owned_ends[1]->getNode(), origin);

  Node* other = map.find(Coordinate(2.0, 2.0));
  ASSERT_NE(other, nullptr);
  ASSERT_NE(other->getEdges(), nullptr);
  EXPECT_EQ(other->getEdges()->getDegree(), 1u);
  EXPECT_EQ(owned_ends[2]->getNode(), other);

  map.testInvariant();
}

TEST(NodeMapTest, AddUniquePtrEdgeEndTransfersOwnership) {
  OwningStarNodeFactory factory;
  NodeMap map(factory);

  map.add(std::make_unique<EdgeEnd>(nullptr, Coordinate(1.0, 1.0),
                                    Coordinate(2.0, 1.0)));
  map.add(std::make_unique<EdgeEnd>(nullptr, Coordinate(1.0, 1.0),
                                    Coordinate(1.0, 2.0)));

  Node* node = map.find(Coordinate(1.0, 1.0));
  ASSERT_NE(node, nullptr);
  ASSERT_NE(node->getEdges(), nullptr);
  EXPECT_EQ(node->getEdges()->getDegree(), 2u);

  map.testInvariant();
}

TEST(NodeMapTest, IteratorsTraverseNodesInCoordinateOrder) {
  NodeMap map(NodeFactory::instance());

  map.addNode(Coordinate(3.0, 0.0));
  map.addNode(Coordinate(1.0, 2.0));
  map.addNode(Coordinate(1.0, 1.0));
  map.addNode(Coordinate(2.0, 0.0));

  std::vector<Coordinate> mutable_order;
  for (auto it = map.begin(); it != map.end(); ++it) {
    mutable_order.push_back(it->second->getCoordinate());
  }

  const NodeMap& const_map = map;
  std::vector<Coordinate> const_order;
  for (auto it = const_map.begin(); it != const_map.end(); ++it) {
    const_order.push_back(it->second->getCoordinate());
  }

  EXPECT_THAT(mutable_order,
              ElementsAre(Coordinate(1.0, 1.0), Coordinate(1.0, 2.0),
                          Coordinate(2.0, 0.0), Coordinate(3.0, 0.0)));
  EXPECT_EQ(const_order, mutable_order);
}

TEST(NodeMapTest, GetBoundaryNodesFiltersByGeometryIndex) {
  NodeMap map(NodeFactory::instance());

  Node* n0 = map.addNode(Coordinate(0.0, 0.0));
  n0->setLabel(0, Location::BOUNDARY);
  n0->setLabel(1, Location::INTERIOR);

  Node* n1 = map.addNode(Coordinate(1.0, 0.0));
  n1->setLabel(0, Location::INTERIOR);
  n1->setLabel(1, Location::BOUNDARY);

  Node* n2 = map.addNode(Coordinate(2.0, 0.0));
  n2->setLabel(0, Location::BOUNDARY);
  n2->setLabel(1, Location::BOUNDARY);

  Node* n3 = map.addNode(Coordinate(3.0, 0.0));
  n3->setLabel(0, Location::EXTERIOR);

  std::vector<Node*> geom0_boundary;
  map.getBoundaryNodes(0, geom0_boundary);
  EXPECT_THAT(geom0_boundary, ElementsAre(n0, n2));

  std::vector<Node*> geom1_boundary;
  map.getBoundaryNodes(1, geom1_boundary);
  EXPECT_THAT(geom1_boundary, ElementsAre(n1, n2));
}

TEST(NodeMapTest, PrintConcatenatesNodeRepresentationsInOrder) {
  NodeMap map(NodeFactory::instance());

  Node* second = map.addNode(Coordinate(10.0, 0.0));
  second->setLabel(0, Location::BOUNDARY);
  Node* first = map.addNode(Coordinate(2.0, 0.0));
  first->setLabel(0, Location::INTERIOR);

  EXPECT_EQ(map.print(), first->print() + second->print());
}

}  // namespace
}  // namespace geos_google
