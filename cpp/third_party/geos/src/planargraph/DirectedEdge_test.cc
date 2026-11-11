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

#include "third_party/geos/include/geos/planargraph/DirectedEdge.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Quadrant.h"
#include "third_party/geos/include/geos/planargraph/Edge.h"
#include "third_party/geos/include/geos/planargraph/Node.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::algorithm::Orientation;
using ::geos::geom::Coordinate;
using ::geos::geom::Quadrant;
using ::geos::planargraph::DirectedEdge;
using ::geos::planargraph::Edge;
using ::geos::planargraph::Node;
using ::geos::util::IllegalArgumentException;

class CustomDirectedEdge : public DirectedEdge {
 public:
  CustomDirectedEdge(Node* new_from, Node* new_to,
                     const Coordinate& direction_pt, bool new_edge_direction,
                     int tag)
      : DirectedEdge(new_from, new_to, direction_pt, new_edge_direction),
        tag_(tag) {}

  int tag() const { return tag_; }

 private:
  int tag_;
};

TEST(DirectedEdgeTest, ConstructionAndAccessors) {
  Node from_node(Coordinate(10.0, -5.0, 3.0));
  Node to_node(Coordinate(13.0, -1.0, 7.0));
  const Coordinate dir_pt(11.0, -4.0, 9.0);

  DirectedEdge de_forward(&from_node, &to_node, dir_pt, true);
  EXPECT_EQ(de_forward.getFromNode(), &from_node);
  EXPECT_EQ(de_forward.getToNode(), &to_node);
  EXPECT_TRUE(de_forward.getEdgeDirection());
  EXPECT_EQ(de_forward.getQuadrant(), +Quadrant::NE);
  EXPECT_DOUBLE_EQ(de_forward.getAngle(), std::numbers::pi / 4.0);
  EXPECT_TRUE(de_forward.getDirectionPt().equals3D(dir_pt));
  EXPECT_EQ(&de_forward.getCoordinate(), &from_node.getCoordinate());
  EXPECT_TRUE(de_forward.getCoordinate().equals3D(Coordinate(10.0, -5.0, 3.0)));

  DirectedEdge de_backward(&from_node, nullptr, Coordinate(9.0, -5.0), false);
  EXPECT_EQ(de_backward.getFromNode(), &from_node);
  EXPECT_EQ(de_backward.getToNode(), nullptr);
  EXPECT_FALSE(de_backward.getEdgeDirection());
  EXPECT_EQ(de_backward.getQuadrant(), +Quadrant::NW);
  EXPECT_DOUBLE_EQ(de_backward.getAngle(), std::numbers::pi);

  // Inherited GraphComponent flags default to false and update cleanly.
  EXPECT_FALSE(de_forward.isMarked());
  EXPECT_FALSE(de_forward.isVisited());
  de_forward.setMarked(true);
  de_forward.setVisited(true);
  EXPECT_TRUE(de_forward.isMarked());
  EXPECT_TRUE(de_forward.isVisited());
  de_forward.setMarked(false);
  de_forward.setVisited(false);
  EXPECT_FALSE(de_forward.isMarked());
  EXPECT_FALSE(de_forward.isVisited());
}

TEST(DirectedEdgeTest, ZeroLengthDirectionThrows) {
  Node from_node(Coordinate(2.0, 3.0, 1.0));
  Node to_node(Coordinate(5.0, 6.0, 2.0));

  EXPECT_THROW(
      DirectedEdge(&from_node, &to_node, Coordinate(2.0, 3.0, 1.0), true),
      IllegalArgumentException);

  // Only 2D displacement (dx, dy) determines the quadrant; equal XY still
  // throws even when Z differs.
  EXPECT_THROW(
      DirectedEdge(&from_node, &to_node, Coordinate(2.0, 3.0, 99.0), false),
      IllegalArgumentException);
}

TEST(DirectedEdgeTest, QuadrantAndAngleAcrossAllDirections) {
  struct Case {
    double dx;
    double dy;
    int expected_quadrant;
    double expected_angle;
  };
  const std::array<Case, 12> cases = {{
      {1.0, 0.0, +Quadrant::NE, 0.0},
      {1.0, 1.0, +Quadrant::NE, std::numbers::pi / 4.0},
      {0.0, 2.0, +Quadrant::NE, std::numbers::pi / 2.0},
      {-1.0, 1.0, +Quadrant::NW, 3.0 * std::numbers::pi / 4.0},
      {-3.0, 0.0, +Quadrant::NW, std::numbers::pi},
      {-1.0, -2.0, +Quadrant::SW, std::atan2(-2.0, -1.0)},
      {-1.0, -1.0, +Quadrant::SW, -3.0 * std::numbers::pi / 4.0},
      {-2.0, -1.0, +Quadrant::SW, std::atan2(-1.0, -2.0)},
      {0.0, -4.0, +Quadrant::SE, -std::numbers::pi / 2.0},
      {1.0, -1.0, +Quadrant::SE, -std::numbers::pi / 4.0},
      {2.0, -1.0, +Quadrant::SE, std::atan2(-1.0, 2.0)},
      {2.0, -1.0 / 1024.0, +Quadrant::SE, std::atan2(-1.0 / 1024.0, 2.0)},
  }};

  Node from_node(Coordinate(5.0, -3.0));
  Node to_node(Coordinate(0.0, 0.0));
  for (const Case& c : cases) {
    const Coordinate dir_pt(5.0 + c.dx, -3.0 + c.dy);
    DirectedEdge de(&from_node, &to_node, dir_pt, true);
    EXPECT_EQ(de.getQuadrant(), c.expected_quadrant)
        << "dx=" << c.dx << ", dy=" << c.dy;
    EXPECT_DOUBLE_EQ(de.getAngle(), c.expected_angle)
        << "dx=" << c.dx << ", dy=" << c.dy;
  }
}

TEST(DirectedEdgeTest, SetAndGetEdgeAndSym) {
  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(4.0, 0.0));
  DirectedEdge de0(&n0, &n1, Coordinate(4.0, 0.0), true);
  DirectedEdge de1(&n1, &n0, Coordinate(0.0, 0.0), false);
  Edge edge;

  de0.setEdge(&edge);
  de1.setEdge(&edge);
  EXPECT_EQ(de0.getEdge(), &edge);
  EXPECT_EQ(de1.getEdge(), &edge);

  de0.setSym(&de1);
  de1.setSym(&de0);
  EXPECT_EQ(de0.getSym(), &de1);
  EXPECT_EQ(de1.getSym(), &de0);

  de0.setEdge(nullptr);
  de0.setSym(nullptr);
  EXPECT_EQ(de0.getEdge(), nullptr);
  EXPECT_EQ(de0.getSym(), nullptr);
}

TEST(DirectedEdgeTest, ToEdgesVectorOverloads) {
  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(1.0, 0.0));
  Node n2(Coordinate(0.0, 1.0));

  DirectedEdge de0(&n0, &n1, Coordinate(1.0, 0.0), true);
  DirectedEdge de1(&n0, &n2, Coordinate(0.0, 1.0), true);
  DirectedEdge de2(&n1, &n2, Coordinate(0.0, 1.0), false);

  Edge edge0;
  Edge edge2;
  de0.setEdge(&edge0);
  de1.setEdge(nullptr);
  de2.setEdge(&edge2);

  std::vector<DirectedEdge*> empty_dir_edges;
  std::unique_ptr<std::vector<Edge*>> empty_result(
      DirectedEdge::toEdges(empty_dir_edges));
  EXPECT_TRUE(empty_result->empty());

  std::vector<Edge*> appended_empty;
  DirectedEdge::toEdges(empty_dir_edges, appended_empty);
  EXPECT_TRUE(appended_empty.empty());

  std::vector<DirectedEdge*> dir_edges = {&de0, &de1, &de2};
  std::unique_ptr<std::vector<Edge*>> new_edges(
      DirectedEdge::toEdges(dir_edges));
  EXPECT_EQ(*new_edges, (std::vector<Edge*>{&edge0, nullptr, &edge2}));

  Edge preexisting;
  std::vector<Edge*> target = {&preexisting};
  DirectedEdge::toEdges(dir_edges, target);
  EXPECT_EQ(target,
            (std::vector<Edge*>{&preexisting, &edge0, nullptr, &edge2}));
}

TEST(DirectedEdgeTest, CompareToAndCompareDirectionAcrossQuadrants) {
  Node center(Coordinate(1.0, 1.0));
  Node dst(Coordinate(0.0, 0.0));

  const DirectedEdge q0(&center, &dst, Coordinate(2.0, 2.0), true);  // NE
  const DirectedEdge q1(&center, &dst, Coordinate(0.0, 2.0), true);  // NW
  const DirectedEdge q2(&center, &dst, Coordinate(0.0, 0.0), true);  // SW
  const DirectedEdge q3(&center, &dst, Coordinate(2.0, 0.0), true);  // SE
  const std::array<const DirectedEdge*, 4> ordered = {&q0, &q1, &q2, &q3};

  for (std::size_t i = 0; i < ordered.size(); ++i) {
    for (std::size_t j = 0; j < ordered.size(); ++j) {
      const int expected = (i < j) ? -1 : ((i > j) ? 1 : 0);
      EXPECT_EQ(ordered[i]->compareDirection(ordered[j]), expected)
          << "i=" << i << ", j=" << j;
      EXPECT_EQ(ordered[i]->compareTo(ordered[j]), expected)
          << "i=" << i << ", j=" << j;
    }
  }
}

TEST(DirectedEdgeTest, CompareDirectionWithinSameQuadrantAndRobustness) {
  Node center(Coordinate(0.0, 0.0));
  Node dst(Coordinate(1.0, 1.0));

  // Quadrant 0: +x axis -> diagonal -> +y axis.
  const DirectedEdge q0_pos_x(&center, &dst, Coordinate(2.0, 0.0), true);
  const DirectedEdge q0_mid(&center, &dst, Coordinate(1.0, 1.0), true);
  const DirectedEdge q0_mid_scaled(&center, &dst, Coordinate(5.0, 5.0), true);
  const DirectedEdge q0_pos_y(&center, &dst, Coordinate(0.0, 3.0), true);

  EXPECT_EQ(q0_pos_x.compareDirection(&q0_mid), +Orientation::CLOCKWISE);
  EXPECT_EQ(q0_mid.compareDirection(&q0_pos_x), +Orientation::COUNTERCLOCKWISE);
  EXPECT_EQ(q0_mid.compareDirection(&q0_pos_y), +Orientation::CLOCKWISE);
  EXPECT_EQ(q0_pos_y.compareDirection(&q0_mid), +Orientation::COUNTERCLOCKWISE);
  EXPECT_EQ(q0_mid.compareDirection(&q0_mid_scaled), +Orientation::COLLINEAR);
  EXPECT_EQ(q0_mid_scaled.compareDirection(&q0_mid), +Orientation::COLLINEAR);

  // Quadrant 1: (+y slightly left) -> (-1, 1) -> -x axis.
  const DirectedEdge q1_early(&center, &dst, Coordinate(-1.0, 3.0), true);
  const DirectedEdge q1_neg_x(&center, &dst, Coordinate(-2.0, 0.0), true);
  EXPECT_EQ(q1_early.compareDirection(&q1_neg_x), -1);
  EXPECT_EQ(q1_neg_x.compareDirection(&q1_early), 1);

  // Quadrant 2: (-2, -1) -> (-1, -2).
  const DirectedEdge q2_early(&center, &dst, Coordinate(-2.0, -1.0), true);
  const DirectedEdge q2_late(&center, &dst, Coordinate(-1.0, -2.0), true);
  EXPECT_EQ(q2_early.compareDirection(&q2_late), -1);
  EXPECT_EQ(q2_late.compareDirection(&q2_early), 1);

  // Quadrant 3: -y axis -> (1, -1) -> (3, -1).
  const DirectedEdge q3_neg_y(&center, &dst, Coordinate(0.0, -1.0), true);
  const DirectedEdge q3_mid(&center, &dst, Coordinate(1.0, -1.0), true);
  const DirectedEdge q3_late(&center, &dst, Coordinate(3.0, -1.0), true);
  EXPECT_EQ(q3_neg_y.compareDirection(&q3_mid), -1);
  EXPECT_EQ(q3_mid.compareDirection(&q3_late), -1);
  EXPECT_EQ(q3_late.compareDirection(&q3_neg_y), 1);

  // Nearly collinear vectors resolved via double-double Orientation::index.
  const DirectedEdge diag(&center, &dst, Coordinate(8e15, 8e15), true);
  const DirectedEdge slightly_ccw(&center, &dst, Coordinate(8e15, 8e15 + 1.0),
                                  true);
  EXPECT_EQ(diag.compareDirection(&slightly_ccw), -1);
  EXPECT_EQ(slightly_ccw.compareDirection(&diag), 1);

  // NaN coordinates with negative or NaN dy fall into quadrant SW; comparing
  // two of them invokes Orientation::index which throws
  // IllegalArgumentException.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const DirectedEdge nan_edge0(&center, &dst, Coordinate(nan, nan), true);
  const DirectedEdge nan_edge1(&center, &dst, Coordinate(nan, -1.0), true);
  EXPECT_EQ(nan_edge0.getQuadrant(), +Quadrant::SW);
  EXPECT_EQ(nan_edge1.getQuadrant(), +Quadrant::SW);
  EXPECT_THROW(nan_edge0.compareDirection(&nan_edge1),
               IllegalArgumentException);
}

TEST(DirectedEdgeTest, PrintAndStreamOperator) {
  Node n0(Coordinate(1.0, 2.0));
  Node n1(Coordinate(1.0, 4.0));
  const DirectedEdge de(&n0, &n1, Coordinate(1.0, 4.0), true);

  std::ostringstream oss;
  oss << de;
  const std::string printed = de.print();
  EXPECT_EQ(printed, oss.str());
  EXPECT_NE(printed.find("DirectedEdge"), std::string::npos);
  EXPECT_NE(printed.find(" - "), std::string::npos);
  EXPECT_NE(printed.find(" 0:"), std::string::npos);

  const CustomDirectedEdge custom(&n0, &n1, Coordinate(1.0, 4.0), false, 42);
  EXPECT_EQ(custom.tag(), 42);
  const std::string custom_printed = custom.print();
  EXPECT_NE(custom_printed.find("CustomDirectedEdge"), std::string::npos);
  EXPECT_NE(custom_printed.find(" 0:"), std::string::npos);
}

}  // namespace
}  // namespace geos_google
