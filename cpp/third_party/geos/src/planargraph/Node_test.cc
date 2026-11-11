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

#include "third_party/geos/include/geos/planargraph/Node.h"

#include <cmath>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdge.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdgeStar.h"
#include "third_party/geos/include/geos/planargraph/Edge.h"
#include "third_party/geos/include/geos/planargraph/GraphComponent.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::planargraph::DirectedEdge;
using ::geos::planargraph::DirectedEdgeStar;
using ::geos::planargraph::Edge;
using ::geos::planargraph::GraphComponent;
using ::geos::planargraph::Node;

class TaggedNode : public Node {
 public:
  TaggedNode(const Coordinate& pt, int tag) : Node(pt), tag_(tag) {}

  int tag() const { return tag_; }

 private:
  int tag_;
};

TEST(NodeTest, ConstructFromCoordinateAndAccessors) {
  const Coordinate input_2d(1.5, -2.5);
  Node node(input_2d);
  const Node& const_node = node;

  EXPECT_NE(&node.getCoordinate(), &input_2d);
  EXPECT_DOUBLE_EQ(node.getCoordinate().x, 1.5);
  EXPECT_DOUBLE_EQ(node.getCoordinate().y, -2.5);
  EXPECT_TRUE(std::isnan(node.getCoordinate().z));
  EXPECT_EQ(node.getDegree(), 0U);
  EXPECT_EQ(const_node.getDegree(), 0U);
  EXPECT_NE(node.getOutEdges(), nullptr);
  EXPECT_EQ(const_node.getOutEdges(), node.getOutEdges());
  EXPECT_EQ(node.getOutEdges()->getDegree(), 0U);
  EXPECT_FALSE(node.isMarked());
  EXPECT_FALSE(node.isVisited());

  // Mutating through getCoordinate() updates the stored coordinate in place.
  node.getCoordinate().x = 9.0;
  node.getCoordinate().z = 12.25;
  EXPECT_TRUE(node.getCoordinate().equals3D(Coordinate(9.0, -2.5, 12.25)));
  EXPECT_DOUBLE_EQ(input_2d.x, 1.5);

  // 3D coordinate is preserved on construction.
  Node node_3d(Coordinate(3.0, 4.0, 5.5));
  EXPECT_TRUE(node_3d.getCoordinate().equals3D(Coordinate(3.0, 4.0, 5.5)));
}

TEST(NodeTest, ConstructWithOwnedDirectedEdgeStarAndSubclass) {
  Coordinate pt(2.0, 3.0, 4.0);
  Node anchor(Coordinate(2.0, 3.0));
  Node target(Coordinate(5.0, 7.0));
  DirectedEdge de(&anchor, &target, Coordinate(5.0, 7.0), true);
  de.setEdge(nullptr);
  de.setSym(nullptr);

  auto* star = new DirectedEdgeStar();
  star->add(&de);

  Node node(pt, star);
  EXPECT_EQ(node.getOutEdges(), star);
  EXPECT_EQ(node.getDegree(), 1U);
  EXPECT_TRUE(node.getCoordinate().equals3D(Coordinate(2.0, 3.0, 4.0)));
  EXPECT_EQ(node.getOutEdges()->getEdges(), (std::vector<DirectedEdge*>{&de}));

  // Subclass deletion through GraphComponent exercises virtual ~Node().
  std::unique_ptr<TaggedNode> tagged =
      std::make_unique<TaggedNode>(Coordinate(-1.0, 6.0), 42);
  EXPECT_EQ(tagged->tag(), 42);
  std::unique_ptr<GraphComponent> base = std::move(tagged);
  EXPECT_FALSE(base->isMarked());
  base->setMarked(true);
  EXPECT_TRUE(base->isMarked());
}

TEST(NodeTest, AddOutEdgeAndGetIndex) {
  Node hub(Coordinate(0.0, 0.0));
  Node east(Coordinate(2.0, 0.0));
  Node north(Coordinate(0.0, 2.0));
  Node west(Coordinate(-2.0, 0.0));
  Node south(Coordinate(0.0, -2.0));

  Edge unrelated;
  EXPECT_EQ(hub.getIndex(&unrelated), -1);

  DirectedEdge h_to_w(&hub, &west, west.getCoordinate(), true);
  DirectedEdge w_to_h(&west, &hub, hub.getCoordinate(), false);
  DirectedEdge h_to_n(&hub, &north, north.getCoordinate(), true);
  DirectedEdge n_to_h(&north, &hub, hub.getCoordinate(), false);
  DirectedEdge h_to_e(&hub, &east, east.getCoordinate(), true);
  DirectedEdge e_to_h(&east, &hub, hub.getCoordinate(), false);

  // Edge::Edge adds each DirectedEdge to its from-node; insert unsorted by
  // angle.
  Edge e_west(&h_to_w, &w_to_h);
  Edge e_north(&h_to_n, &n_to_h);
  Edge e_east(&h_to_e, &e_to_h);

  DirectedEdge h_to_s(&hub, &south, south.getCoordinate(), true);
  h_to_s.setEdge(nullptr);
  h_to_s.setSym(nullptr);
  hub.addOutEdge(&h_to_s);

  EXPECT_EQ(hub.getDegree(), 4U);
  // Sorted CCW order from +x: east (0), north (1), west (2), south/null (3).
  EXPECT_EQ(hub.getIndex(&e_east), 0);
  EXPECT_EQ(hub.getIndex(&e_north), 1);
  EXPECT_EQ(hub.getIndex(&e_west), 2);
  EXPECT_EQ(hub.getIndex(nullptr), 3);
  EXPECT_EQ(hub.getIndex(&unrelated), -1);
  EXPECT_EQ(hub.getOutEdges()->getEdges(),
            (std::vector<DirectedEdge*>{&h_to_e, &h_to_n, &h_to_w, &h_to_s}));
}

TEST(NodeTest, StreamOutputIncludesCoordinateDegreeAndFlags) {
  Node node(Coordinate(1.5, -2.5));
  {
    std::ostringstream oss;
    oss << node;
    EXPECT_EQ(oss.str(), "Node 1.5 -2.5 with degree 0");
  }

  Node target(Coordinate(4.0, -2.5));
  DirectedEdge out_de(&node, &target, target.getCoordinate(), true);
  out_de.setEdge(nullptr);
  out_de.setSym(nullptr);
  node.addOutEdge(&out_de);
  node.setMarked(true);
  {
    std::ostringstream oss;
    oss << node;
    EXPECT_EQ(oss.str(), "Node 1.5 -2.5 with degree 1 Marked ");
  }

  node.setMarked(false);
  node.setVisited(true);
  {
    std::ostringstream oss;
    oss << node;
    EXPECT_EQ(oss.str(), "Node 1.5 -2.5 with degree 1 Visited ");
  }

  Node node_3d(Coordinate(1.5, -2.5, 3.25));
  node_3d.setMarked(true);
  node_3d.setVisited(true);
  {
    std::ostringstream oss;
    oss << node_3d;
    EXPECT_EQ(oss.str(), "Node 1.5 -2.5 3.25 with degree 0 Marked  Visited ");
  }
}

TEST(NodeTest, GetEdgesBetweenEmptyAndDisjointEdgeSets) {
  // Note: upstream Node::getEdgesBetween passes commonEdges->end() rather than
  // std::back_inserter(*commonEdges) to std::set_intersection, so it is only
  // memory-safe when the two nodes share no common Edge pointer.
  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(10.0, 0.0));

  {
    std::unique_ptr<std::vector<Edge*>> common(Node::getEdgesBetween(&n0, &n1));
    ASSERT_NE(common, nullptr);
    EXPECT_TRUE(common->empty());
  }

  Node aux0(Coordinate(0.0, 5.0));
  Node aux1(Coordinate(10.0, 5.0));
  Edge edges[4];

  DirectedEdge d0_a(&n0, &aux0, aux0.getCoordinate(), true);
  DirectedEdge d0_b(&aux0, &n0, n0.getCoordinate(), false);
  DirectedEdge d1_a(&n0, &aux0, aux0.getCoordinate(), true);
  DirectedEdge d1_b(&aux0, &n0, n0.getCoordinate(), false);
  // Add higher-address edge first so std::sort reorders edges0.
  edges[2].setDirectedEdges(&d0_a, &d0_b);
  edges[0].setDirectedEdges(&d1_a, &d1_b);

  {
    std::unique_ptr<std::vector<Edge*>> common(Node::getEdgesBetween(&n0, &n1));
    ASSERT_NE(common, nullptr);
    EXPECT_TRUE(common->empty());
  }

  DirectedEdge d2_a(&n1, &aux1, aux1.getCoordinate(), true);
  DirectedEdge d2_b(&aux1, &n1, n1.getCoordinate(), false);
  DirectedEdge d3_a(&n1, &aux1, aux1.getCoordinate(), true);
  DirectedEdge d3_b(&aux1, &n1, n1.getCoordinate(), false);
  // Interleaved addresses (&edges[0], &edges[2]) vs (&edges[1], &edges[3])
  // exercise both std::sort and both comparison paths of std::set_intersection.
  edges[3].setDirectedEdges(&d2_a, &d2_b);
  edges[1].setDirectedEdges(&d3_a, &d3_b);

  {
    std::unique_ptr<std::vector<Edge*>> common(Node::getEdgesBetween(&n0, &n1));
    ASSERT_NE(common, nullptr);
    EXPECT_TRUE(common->empty());
  }
  {
    std::unique_ptr<std::vector<Edge*>> common(Node::getEdgesBetween(&n1, &n0));
    ASSERT_NE(common, nullptr);
    EXPECT_TRUE(common->empty());
  }
}

}  // namespace
}  // namespace geos_google
