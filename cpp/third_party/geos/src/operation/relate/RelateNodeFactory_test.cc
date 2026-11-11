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

#include "third_party/geos/include/geos/operation/relate/RelateNodeFactory.h"

#include <cmath>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/NodeFactory.h"
#include "third_party/geos/include/geos/geomgraph/NodeMap.h"
#include "third_party/geos/include/geos/operation/relate/EdgeEndBundleStar.h"
#include "third_party/geos/include/geos/operation/relate/RelateNode.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Location;
using geos::geomgraph::EdgeEnd;
using geos::geomgraph::Label;
using geos::geomgraph::Node;
using geos::geomgraph::NodeFactory;
using geos::geomgraph::NodeMap;
using geos::operation::relate::EdgeEndBundleStar;
using geos::operation::relate::RelateNode;
using geos::operation::relate::RelateNodeFactory;

TEST(RelateNodeFactoryTest, InstanceReturnsSingletonDistinctFromBaseFactory) {
  const NodeFactory& instance_a = RelateNodeFactory::instance();
  const NodeFactory& instance_b = RelateNodeFactory::instance();
  EXPECT_EQ(&instance_a, &instance_b);
  EXPECT_NE(&instance_a, &NodeFactory::instance());
}

TEST(RelateNodeFactoryTest, CreateNodeReturnsRelateNodeWithEdgeEndBundleStar) {
  const Coordinate coord(10.5, -20.25, 3.5);
  std::unique_ptr<Node> node(RelateNodeFactory::instance().createNode(coord));
  ASSERT_NE(node, nullptr);

  auto* relate_node = dynamic_cast<RelateNode*>(node.get());
  ASSERT_NE(relate_node, nullptr);
  EXPECT_TRUE(relate_node->getCoordinate().equals3D(coord));

  auto* star = dynamic_cast<EdgeEndBundleStar*>(relate_node->getEdges());
  ASSERT_NE(star, nullptr);
  EXPECT_EQ(star->getDegree(), 0u);
}

TEST(RelateNodeFactoryTest, CreateNodeWithDefault2DCoordinatePreservesNaNZ) {
  const Coordinate coord(1.0, 2.0);
  std::unique_ptr<Node> node(RelateNodeFactory::instance().createNode(coord));
  ASSERT_NE(node, nullptr);
  EXPECT_DOUBLE_EQ(node->getCoordinate().x, 1.0);
  EXPECT_DOUBLE_EQ(node->getCoordinate().y, 2.0);
  EXPECT_TRUE(std::isnan(node->getCoordinate().z));
}

TEST(RelateNodeFactoryTest, NodeMapUsesFactoryToCreateAndBundleEdgeEnds) {
  NodeMap node_map(RelateNodeFactory::instance());

  Node* node = node_map.addNode(Coordinate(0.0, 0.0));
  ASSERT_NE(node, nullptr);
  EXPECT_NE(dynamic_cast<RelateNode*>(node), nullptr);
  EXPECT_EQ(node_map.find(Coordinate(0.0, 0.0)), node);
  EXPECT_EQ(node_map.find(Coordinate(5.0, 5.0)), nullptr);

  // Adding the same 2D coordinate reuses the existing RelateNode and records Z.
  Node* same_node = node_map.addNode(Coordinate(0.0, 0.0, 7.0));
  EXPECT_EQ(same_node, node);
  EXPECT_DOUBLE_EQ(same_node->getCoordinate().z, 7.0);

  // Two parallel EdgeEnds at the same origin bundle into one EdgeEndBundle;
  // a third EdgeEnd in a distinct direction creates a second bundle.
  auto ee1 = std::make_unique<EdgeEnd>(nullptr, Coordinate(0.0, 0.0),
                                       Coordinate(1.0, 0.0),
                                       Label(0, Location::INTERIOR));
  auto ee2 = std::make_unique<EdgeEnd>(nullptr, Coordinate(0.0, 0.0),
                                       Coordinate(2.0, 0.0),
                                       Label(1, Location::INTERIOR));
  auto ee3 = std::make_unique<EdgeEnd>(nullptr, Coordinate(0.0, 0.0),
                                       Coordinate(0.0, 1.0),
                                       Label(0, Location::INTERIOR));

  node_map.add(std::move(ee1));
  node_map.add(std::move(ee2));
  node_map.add(std::move(ee3));

  EXPECT_EQ(node->getEdges()->getDegree(), 2u);
}

}  // namespace
}  // namespace geos_google
