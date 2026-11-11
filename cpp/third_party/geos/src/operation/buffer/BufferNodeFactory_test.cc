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

// Unit tests for geos::operation::buffer::BufferNodeFactory.
// See also:
// https://github.com/libgeos/geos/blob/main/src/operation/buffer/BufferNodeFactory.cpp

#include "third_party/geos/include/geos/operation/buffer/BufferNodeFactory.h"

#include <cmath>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdgeStar.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/NodeFactory.h"
#include "third_party/geos/include/geos/geomgraph/PlanarGraph.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geomgraph::DirectedEdgeStar;
using ::geos::geomgraph::Node;
using ::geos::geomgraph::NodeFactory;
using ::geos::geomgraph::PlanarGraph;
using ::geos::operation::buffer::BufferNodeFactory;

TEST(BufferNodeFactoryTest, InstanceReturnsSameSingleton) {
  const NodeFactory& first = BufferNodeFactory::instance();
  const NodeFactory& second = BufferNodeFactory::instance();
  EXPECT_EQ(&first, &second);
  EXPECT_NE(&first, &NodeFactory::instance());
  EXPECT_NE(dynamic_cast<const BufferNodeFactory*>(&first), nullptr);
}

TEST(BufferNodeFactoryTest, CreateNodeFromInstanceWith2DCoordinate) {
  const NodeFactory& factory = BufferNodeFactory::instance();
  const Coordinate coord(12.5, -7.25);

  std::unique_ptr<Node> node(factory.createNode(coord));
  ASSERT_NE(node, nullptr);
  EXPECT_DOUBLE_EQ(node->getCoordinate().x, 12.5);
  EXPECT_DOUBLE_EQ(node->getCoordinate().y, -7.25);
  EXPECT_TRUE(std::isnan(node->getCoordinate().z));
  EXPECT_TRUE(node->getZ().empty());
  ASSERT_NE(node->getEdges(), nullptr);
  auto* star = dynamic_cast<DirectedEdgeStar*>(node->getEdges());
  ASSERT_NE(star, nullptr);
  EXPECT_EQ(star->getDegree(), 0u);
  // Newly created Nodes start with a null Label (geometry count 0), so
  // isIsolated() (geometry count == 1) is false until labelled.
  EXPECT_TRUE(node->getLabel().isNull());
  EXPECT_FALSE(node->isIsolated());
  EXPECT_FALSE(node->isIncidentEdgeInResult());
}

TEST(BufferNodeFactoryTest, CreateNodeFromDirectInstanceWith3DCoordinate) {
  BufferNodeFactory factory;
  const NodeFactory& as_base = factory;
  const Coordinate coord(3.0, 4.0, 5.5);

  std::unique_ptr<Node> first(as_base.createNode(coord));
  std::unique_ptr<Node> second(factory.createNode(coord));
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first.get(), second.get());
  EXPECT_DOUBLE_EQ(first->getCoordinate().x, 3.0);
  EXPECT_DOUBLE_EQ(first->getCoordinate().y, 4.0);
  EXPECT_DOUBLE_EQ(first->getCoordinate().z, 5.5);
  ASSERT_EQ(first->getZ().size(), 1u);
  EXPECT_DOUBLE_EQ(first->getZ()[0], 5.5);
  EXPECT_NE(dynamic_cast<DirectedEdgeStar*>(first->getEdges()), nullptr);
  EXPECT_NE(dynamic_cast<DirectedEdgeStar*>(second->getEdges()), nullptr);
  EXPECT_NE(first->getEdges(), second->getEdges());
}

TEST(BufferNodeFactoryTest, IntegratesWithPlanarGraph) {
  PlanarGraph buffer_graph(BufferNodeFactory::instance());
  PlanarGraph default_graph;
  Coordinate p1(1.0, 2.0);
  Coordinate p2(5.0, 6.0);

  Node* n1 = buffer_graph.addNode(p1);
  Node* n2 = buffer_graph.addNode(p2);
  Node* n1_dup = buffer_graph.addNode(p1);

  ASSERT_NE(n1, nullptr);
  ASSERT_NE(n2, nullptr);
  EXPECT_NE(n1, n2);
  EXPECT_EQ(n1, n1_dup);
  EXPECT_EQ(buffer_graph.find(p1), n1);
  EXPECT_NE(dynamic_cast<DirectedEdgeStar*>(n1->getEdges()), nullptr);
  EXPECT_NE(dynamic_cast<DirectedEdgeStar*>(n2->getEdges()), nullptr);

  Node* default_node = default_graph.addNode(p1);
  ASSERT_NE(default_node, nullptr);
  EXPECT_EQ(default_node->getEdges(), nullptr);
}

}  // namespace
}  // namespace geos_google
