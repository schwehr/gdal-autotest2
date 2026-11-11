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

#include "third_party/geos/include/geos/planargraph/Edge.h"

#include <sstream>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdge.h"
#include "third_party/geos/include/geos/planargraph/DirectedEdgeStar.h"
#include "third_party/geos/include/geos/planargraph/Node.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::planargraph::DirectedEdge;
using ::geos::planargraph::Edge;
using ::geos::planargraph::Node;

class TaggedEdge : public Edge {
 public:
  TaggedEdge(DirectedEdge* de0, DirectedEdge* de1, int tag)
      : Edge(de0, de1), tag_(tag) {}

  int tag() const { return tag_; }

 private:
  int tag_;
};

TEST(EdgeTest, DefaultConstructAndSetDirectedEdges) {
  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(3.0, 4.0));
  DirectedEdge de0(&n0, &n1, Coordinate(3.0, 4.0), true);
  DirectedEdge de1(&n1, &n0, Coordinate(0.0, 0.0), false);

  Edge edge;
  edge.setDirectedEdges(&de0, &de1);

  EXPECT_EQ(edge.getDirEdge(0), &de0);
  EXPECT_EQ(edge.getDirEdge(1), &de1);
  EXPECT_EQ(de0.getEdge(), &edge);
  EXPECT_EQ(de1.getEdge(), &edge);
  EXPECT_EQ(de0.getSym(), &de1);
  EXPECT_EQ(de1.getSym(), &de0);

  EXPECT_EQ(n0.getDegree(), 1U);
  EXPECT_EQ(n1.getDegree(), 1U);
  EXPECT_EQ(n0.getIndex(&edge), 0);
  EXPECT_EQ(n1.getIndex(&edge), 0);
  EXPECT_EQ(n0.getOutEdges()->getEdges(), (std::vector<DirectedEdge*>{&de0}));
  EXPECT_EQ(n1.getOutEdges()->getEdges(), (std::vector<DirectedEdge*>{&de1}));
}

TEST(EdgeTest, TwoArgConstructorInitializesSymAndNodes) {
  Node n0(Coordinate(-2.0, 1.0));
  Node n1(Coordinate(4.0, 1.0));
  DirectedEdge de0(&n0, &n1, Coordinate(4.0, 1.0), true);
  DirectedEdge de1(&n1, &n0, Coordinate(-2.0, 1.0), false);

  Edge edge(&de0, &de1);
  EXPECT_EQ(edge.getDirEdge(0), &de0);
  EXPECT_EQ(edge.getDirEdge(1), &de1);
  EXPECT_EQ(de0.getEdge(), &edge);
  EXPECT_EQ(de1.getEdge(), &edge);
  EXPECT_EQ(de0.getSym(), &de1);
  EXPECT_EQ(de1.getSym(), &de0);
  EXPECT_EQ(n0.getDegree(), 1U);
  EXPECT_EQ(n1.getDegree(), 1U);
}

TEST(EdgeTest, GetDirEdgeByNodeAndGetOppositeNode) {
  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(5.0, 0.0));
  Node other(Coordinate(2.0, 2.0));
  DirectedEdge de0(&n0, &n1, Coordinate(5.0, 0.0), true);
  DirectedEdge de1(&n1, &n0, Coordinate(0.0, 0.0), false);

  Edge edge(&de0, &de1);

  EXPECT_EQ(edge.getDirEdge(&n0), &de0);
  EXPECT_EQ(edge.getDirEdge(&n1), &de1);
  EXPECT_EQ(edge.getDirEdge(&other), nullptr);
  EXPECT_EQ(edge.getDirEdge(static_cast<Node*>(nullptr)), nullptr);
  // Signed 0 must unambiguously call getDirEdge(int), not getDirEdge(Node*).
  EXPECT_EQ(edge.getDirEdge(0), &de0);

  EXPECT_EQ(edge.getOppositeNode(&n0), &n1);
  EXPECT_EQ(edge.getOppositeNode(&n1), &n0);
  EXPECT_EQ(edge.getOppositeNode(&other), nullptr);
  EXPECT_EQ(edge.getOppositeNode(nullptr), nullptr);
}

TEST(EdgeTest, SelfLoopEdge) {
  Node loop_node(Coordinate(1.0, 2.0));
  DirectedEdge de0(&loop_node, &loop_node, Coordinate(2.0, 2.0), true);
  DirectedEdge de1(&loop_node, &loop_node, Coordinate(1.0, 3.0), false);

  Edge edge(&de0, &de1);
  EXPECT_EQ(loop_node.getDegree(), 2U);
  EXPECT_EQ(edge.getDirEdge(&loop_node), &de0);
  EXPECT_EQ(edge.getOppositeNode(&loop_node), &loop_node);
  EXPECT_EQ(de0.getSym(), &de1);
  EXPECT_EQ(de1.getSym(), &de0);
}

TEST(EdgeTest, StreamOutputAndGraphComponentFlags) {
  Edge edge;

  EXPECT_FALSE(edge.isMarked());
  EXPECT_FALSE(edge.isVisited());
  {
    std::ostringstream oss;
    oss << edge;
    EXPECT_EQ(oss.str(), "Edge ");
  }

  edge.setMarked(true);
  EXPECT_TRUE(edge.isMarked());
  EXPECT_FALSE(edge.isVisited());
  {
    std::ostringstream oss;
    oss << edge;
    EXPECT_EQ(oss.str(), "Edge  Marked ");
  }

  edge.setMarked(false);
  edge.setVisited(true);
  EXPECT_FALSE(edge.isMarked());
  EXPECT_TRUE(edge.isVisited());
  {
    std::ostringstream oss;
    oss << edge;
    EXPECT_EQ(oss.str(), "Edge  Visited ");
  }

  edge.setMarked(true);
  EXPECT_TRUE(edge.isMarked());
  EXPECT_TRUE(edge.isVisited());
  {
    std::ostringstream oss;
    oss << edge;
    EXPECT_EQ(oss.str(), "Edge  Marked  Visited ");
  }
}

TEST(EdgeTest, SharedHubNodeAndSubclass) {
  Node hub(Coordinate(0.0, 0.0));
  Node east(Coordinate(2.0, 0.0));
  Node north(Coordinate(0.0, 2.0));
  Node west(Coordinate(-2.0, 0.0));

  DirectedEdge h_to_w(&hub, &west, Coordinate(-2.0, 0.0), true);
  DirectedEdge w_to_h(&west, &hub, Coordinate(0.0, 0.0), false);
  DirectedEdge h_to_e(&hub, &east, Coordinate(2.0, 0.0), true);
  DirectedEdge e_to_h(&east, &hub, Coordinate(0.0, 0.0), false);
  DirectedEdge h_to_n(&hub, &north, Coordinate(0.0, 2.0), true);
  DirectedEdge n_to_h(&north, &hub, Coordinate(0.0, 0.0), false);

  TaggedEdge e_west(&h_to_w, &w_to_h, 1);
  TaggedEdge e_east(&h_to_e, &e_to_h, 2);
  TaggedEdge e_north(&h_to_n, &n_to_h, 3);

  EXPECT_EQ(e_west.tag(), 1);
  EXPECT_EQ(e_east.tag(), 2);
  EXPECT_EQ(e_north.tag(), 3);

  EXPECT_EQ(hub.getDegree(), 3U);
  EXPECT_EQ(hub.getIndex(&e_east), 0);
  EXPECT_EQ(hub.getIndex(&e_north), 1);
  EXPECT_EQ(hub.getIndex(&e_west), 2);

  EXPECT_EQ(e_east.getOppositeNode(&hub), &east);
  EXPECT_EQ(e_north.getOppositeNode(&hub), &north);
  EXPECT_EQ(e_west.getOppositeNode(&hub), &west);
}

}  // namespace
}  // namespace geos_google
