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

// Unit tests for third_party/geos/src/geomgraph/NodeFactory.cpp and
// third_party/geos/include/geos/geomgraph/NodeFactory.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geomgraph/NodeFactory.cpp

#include "third_party/geos/include/geos/geomgraph/NodeFactory.h"

#include <cmath>
#include <memory>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdgeStar.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEndStar.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/NodeMap.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geomgraph::DirectedEdgeStar;
using geos::geomgraph::Node;
using geos::geomgraph::NodeFactory;
using geos::geomgraph::NodeMap;
using testing::ElementsAre;

class StarNodeFactory final : public NodeFactory {
 public:
  StarNodeFactory() = default;
  ~StarNodeFactory() override = default;

  Node* createNode(const Coordinate& coord) const override {
    return new Node(coord, new DirectedEdgeStar());
  }
};

TEST(NodeFactoryTest, InstanceReturnsSingletonReference) {
  const NodeFactory& first = NodeFactory::instance();
  const NodeFactory& second = NodeFactory::instance();
  EXPECT_EQ(&first, &second);
}

TEST(NodeFactoryTest, CreateNodeProducesIndependentNodesWithNullEdgeStars) {
  const NodeFactory& factory = NodeFactory::instance();

  std::unique_ptr<Node> node2d(factory.createNode(Coordinate(1.0, 2.0)));
  ASSERT_NE(node2d, nullptr);
  EXPECT_TRUE(node2d->getCoordinate().equals2D(Coordinate(1.0, 2.0)));
  EXPECT_TRUE(std::isnan(node2d->getCoordinate().z));
  EXPECT_TRUE(node2d->getZ().empty());
  EXPECT_EQ(node2d->getEdges(), nullptr);
  EXPECT_TRUE(node2d->getLabel().isNull());
  EXPECT_FALSE(node2d->isIsolated());
  EXPECT_FALSE(node2d->isIncidentEdgeInResult());

  std::unique_ptr<Node> node3d(factory.createNode(Coordinate(3.0, 4.0, 5.0)));
  ASSERT_NE(node3d, nullptr);
  EXPECT_NE(node2d.get(), node3d.get());
  EXPECT_TRUE(node3d->getCoordinate().equals3D(Coordinate(3.0, 4.0, 5.0)));
  EXPECT_THAT(node3d->getZ(), ElementsAre(5.0));
  EXPECT_EQ(node3d->getEdges(), nullptr);
}

TEST(NodeFactoryTest, SubclassSupportsPolymorphicCreateAndDestruction) {
  std::unique_ptr<NodeFactory> factory = std::make_unique<StarNodeFactory>();

  std::unique_ptr<Node> custom_node(
      factory->createNode(Coordinate(-2.5, 7.5, 1.0)));
  ASSERT_NE(custom_node, nullptr);
  EXPECT_TRUE(
      custom_node->getCoordinate().equals3D(Coordinate(-2.5, 7.5, 1.0)));
  ASSERT_NE(custom_node->getEdges(), nullptr);
  EXPECT_EQ(custom_node->getEdges()->getDegree(), 0u);

  std::unique_ptr<Node> base_node(
      factory->NodeFactory::createNode(Coordinate(-2.5, 7.5)));
  ASSERT_NE(base_node, nullptr);
  EXPECT_EQ(base_node->getEdges(), nullptr);
}

TEST(NodeFactoryTest, IntegratesWithNodeMapForDefaultAndCustomFactories) {
  NodeMap default_map(NodeFactory::instance());
  Node* inserted = default_map.addNode(Coordinate(10.0, 20.0));
  ASSERT_NE(inserted, nullptr);
  EXPECT_EQ(inserted->getEdges(), nullptr);

  Node* reused = default_map.addNode(Coordinate(10.0, 20.0, 4.0));
  EXPECT_EQ(reused, inserted);
  EXPECT_DOUBLE_EQ(reused->getCoordinate().z, 4.0);

  StarNodeFactory star_factory;
  NodeMap star_map(star_factory);
  Node* star_node = star_map.addNode(Coordinate(0.0, 0.0));
  ASSERT_NE(star_node, nullptr);
  ASSERT_NE(star_node->getEdges(), nullptr);
  EXPECT_EQ(star_node->getEdges()->getDegree(), 0u);
}

}  // namespace
}  // namespace geos_google
