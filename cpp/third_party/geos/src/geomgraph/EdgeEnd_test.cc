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

#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geom/Quadrant.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::algorithm::BoundaryNodeRule;
using ::geos::algorithm::Orientation;
using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::Location;
using ::geos::geom::Position;
using ::geos::geom::Quadrant;
using ::geos::geomgraph::Edge;
using ::geos::geomgraph::EdgeEnd;
using ::geos::geomgraph::EdgeEndLT;
using ::geos::geomgraph::Label;
using ::geos::geomgraph::Node;
using ::geos::util::IllegalArgumentException;
using ::testing::HasSubstr;

class SubclassEdgeEnd : public EdgeEnd {
 public:
  explicit SubclassEdgeEnd(Edge* new_edge) : EdgeEnd(new_edge) {}

  void InitSegment(const Coordinate& new_p0, const Coordinate& new_p1) {
    init(new_p0, new_p1);
  }
};

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(seq.release());
}

TEST(EdgeEndTest, DefaultConstructorInitializesZeroState) {
  EdgeEnd ee;

  EXPECT_EQ(ee.getEdge(), nullptr);
  EXPECT_EQ(ee.getNode(), nullptr);
  EXPECT_TRUE(ee.getLabel().isNull());
  const EdgeEnd& const_ee = ee;
  EXPECT_TRUE(const_ee.getLabel().isNull());
  EXPECT_DOUBLE_EQ(ee.getDx(), 0.0);
  EXPECT_DOUBLE_EQ(ee.getDy(), 0.0);
  EXPECT_EQ(ee.getQuadrant(), 0);
  EXPECT_EQ(ee.getCoordinate(), Coordinate(0.0, 0.0));
  EXPECT_EQ(const_ee.getCoordinate(), Coordinate(0.0, 0.0));
  EXPECT_EQ(ee.getDirectedCoordinate(), Coordinate(0.0, 0.0));
}

TEST(EdgeEndTest, ConstructWithAndWithoutLabelAcrossAllQuadrants) {
  auto edge = MakeEdge({{1.0, 2.0}, {4.0, 6.0}});

  EdgeEnd ee_ne(edge.get(), {1.0, 2.0}, {4.0, 6.0});
  EXPECT_EQ(ee_ne.getEdge(), edge.get());
  EXPECT_EQ(ee_ne.getNode(), nullptr);
  EXPECT_TRUE(ee_ne.getLabel().isNull());
  EXPECT_EQ(ee_ne.getCoordinate(), Coordinate(1.0, 2.0));
  EXPECT_EQ(ee_ne.getDirectedCoordinate(), Coordinate(4.0, 6.0));
  EXPECT_DOUBLE_EQ(ee_ne.getDx(), 3.0);
  EXPECT_DOUBLE_EQ(ee_ne.getDy(), 4.0);
  EXPECT_EQ(ee_ne.getQuadrant(), +Quadrant::NE);

  Label lbl(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR);
  EdgeEnd ee_nw(edge.get(), {1.0, 2.0}, {-2.0, 5.0}, lbl);
  EXPECT_EQ(ee_nw.getEdge(), edge.get());
  EXPECT_EQ(ee_nw.getLabel().getLocation(0, Position::ON), Location::BOUNDARY);
  EXPECT_EQ(ee_nw.getLabel().getLocation(0, Position::LEFT),
            Location::INTERIOR);
  EXPECT_EQ(ee_nw.getLabel().getLocation(0, Position::RIGHT),
            Location::EXTERIOR);
  EXPECT_DOUBLE_EQ(ee_nw.getDx(), -3.0);
  EXPECT_DOUBLE_EQ(ee_nw.getDy(), 3.0);
  EXPECT_EQ(ee_nw.getQuadrant(), +Quadrant::NW);

  EdgeEnd ee_sw(edge.get(), {1.0, 2.0}, {-1.0, -3.0}, lbl);
  EXPECT_DOUBLE_EQ(ee_sw.getDx(), -2.0);
  EXPECT_DOUBLE_EQ(ee_sw.getDy(), -5.0);
  EXPECT_EQ(ee_sw.getQuadrant(), +Quadrant::SW);

  EdgeEnd ee_se(edge.get(), {1.0, 2.0}, {3.0, -2.0}, lbl);
  EXPECT_DOUBLE_EQ(ee_se.getDx(), 2.0);
  EXPECT_DOUBLE_EQ(ee_se.getDy(), -4.0);
  EXPECT_EQ(ee_se.getQuadrant(), +Quadrant::SE);

  // Axis-aligned rays.
  EXPECT_EQ(EdgeEnd(nullptr, {0.0, 0.0}, {1.0, 0.0}).getQuadrant(),
            +Quadrant::NE);
  EXPECT_EQ(EdgeEnd(nullptr, {0.0, 0.0}, {0.0, 1.0}).getQuadrant(),
            +Quadrant::NE);
  EXPECT_EQ(EdgeEnd(nullptr, {0.0, 0.0}, {-1.0, 0.0}).getQuadrant(),
            +Quadrant::NW);
  EXPECT_EQ(EdgeEnd(nullptr, {0.0, 0.0}, {0.0, -1.0}).getQuadrant(),
            +Quadrant::SE);
}

TEST(EdgeEndTest, ProtectedConstructorAndInit) {
  auto edge = MakeEdge({{10.0, 20.0}, {7.0, 15.0}});
  SubclassEdgeEnd sub_ee(edge.get());

  EXPECT_EQ(sub_ee.getEdge(), edge.get());
  EXPECT_EQ(sub_ee.getNode(), nullptr);
  EXPECT_TRUE(sub_ee.getLabel().isNull());
  EXPECT_DOUBLE_EQ(sub_ee.getDx(), 0.0);
  EXPECT_DOUBLE_EQ(sub_ee.getDy(), 0.0);
  EXPECT_EQ(sub_ee.getQuadrant(), 0);

  sub_ee.InitSegment({10.0, 20.0}, {7.0, 15.0});
  EXPECT_EQ(sub_ee.getCoordinate(), Coordinate(10.0, 20.0));
  EXPECT_EQ(sub_ee.getDirectedCoordinate(), Coordinate(7.0, 15.0));
  EXPECT_DOUBLE_EQ(sub_ee.getDx(), -3.0);
  EXPECT_DOUBLE_EQ(sub_ee.getDy(), -5.0);
  EXPECT_EQ(sub_ee.getQuadrant(), +Quadrant::SW);
}

TEST(EdgeEndTest, IdenticalEndpointsThrowsIllegalArgumentException) {
  EXPECT_THROW(EdgeEnd(nullptr, {5.0, 5.0}, {5.0, 5.0}),
               IllegalArgumentException);
  EXPECT_THROW(
      EdgeEnd(nullptr, {5.0, 5.0}, {5.0, 5.0}, Label(Location::INTERIOR)),
      IllegalArgumentException);
}

TEST(EdgeEndTest, SetNodeAndGetNode) {
  EdgeEnd ee(nullptr, {3.0, 4.0, 1.0}, {5.0, 6.0, 2.0});
  EXPECT_EQ(ee.getNode(), nullptr);

  Node node(Coordinate(3.0, 4.0, 99.0), nullptr);
  ee.setNode(&node);
  EXPECT_EQ(ee.getNode(), &node);
}

TEST(EdgeEndTest, CompareDirectionCompareToAndEdgeEndLT) {
  EdgeEnd e_pos_x(nullptr, {0.0, 0.0}, {1.0, 0.0});
  EdgeEnd e_low(nullptr, {0.0, 0.0}, {2.0, 1.0});
  EdgeEnd e_mid(nullptr, {0.0, 0.0}, {1.0, 1.0});
  EdgeEnd e_mid_long(nullptr, {0.0, 0.0}, {2.0, 2.0});
  EdgeEnd e_same_dx_higher_y(nullptr, {0.0, 0.0}, {1.0, 2.0});
  EdgeEnd e_pos_y(nullptr, {0.0, 0.0}, {0.0, 1.0});
  EdgeEnd e_nw(nullptr, {0.0, 0.0}, {-1.0, 1.0});
  EdgeEnd e_neg_x(nullptr, {0.0, 0.0}, {-1.0, 0.0});
  EdgeEnd e_sw(nullptr, {0.0, 0.0}, {-1.0, -1.0});
  EdgeEnd e_neg_y(nullptr, {0.0, 0.0}, {0.0, -1.0});
  EdgeEnd e_se(nullptr, {0.0, 0.0}, {1.0, -1.0});
  EdgeEnd e_shifted_mid(nullptr, {10.0, 10.0}, {11.0, 11.0});

  // Identical direction vectors return 0.
  EXPECT_EQ(e_mid.compareDirection(&e_mid), 0);
  EXPECT_EQ(e_mid.compareDirection(&e_shifted_mid), 0);
  EXPECT_EQ(e_mid.compareTo(&e_shifted_mid), 0);

  // Different quadrants order by quadrant index.
  EXPECT_EQ(e_se.compareDirection(&e_pos_x), 1);
  EXPECT_EQ(e_pos_x.compareDirection(&e_se), -1);
  EXPECT_EQ(e_nw.compareDirection(&e_pos_y), 1);
  EXPECT_EQ(e_sw.compareDirection(&e_nw), 1);
  EXPECT_EQ(e_pos_x.compareTo(&e_se), -1);

  // Same quadrant with differing direction vectors delegates to Orientation.
  EXPECT_EQ(e_same_dx_higher_y.compareDirection(&e_low),
            +Orientation::COUNTERCLOCKWISE);
  EXPECT_EQ(e_low.compareDirection(&e_same_dx_higher_y),
            +Orientation::CLOCKWISE);
  EXPECT_EQ(e_same_dx_higher_y.compareDirection(&e_mid),
            +Orientation::COUNTERCLOCKWISE);
  EXPECT_EQ(e_mid.compareDirection(&e_same_dx_higher_y),
            +Orientation::CLOCKWISE);
  EXPECT_EQ(e_pos_y.compareDirection(&e_pos_x), +Orientation::COUNTERCLOCKWISE);
  EXPECT_EQ(e_pos_x.compareDirection(&e_pos_y), +Orientation::CLOCKWISE);
  EXPECT_EQ(e_mid_long.compareDirection(&e_mid), +Orientation::COLLINEAR);

  // EdgeEndLT sorts rays in CCW order starting from the positive x-axis.
  std::vector<EdgeEnd*> rays = {
      &e_se,    &e_nw,  &e_pos_x, &e_sw,  &e_pos_y,
      &e_neg_y, &e_mid, &e_neg_x, &e_low, &e_same_dx_higher_y,
  };
  std::sort(rays.begin(), rays.end(), EdgeEndLT{});

  const std::vector<EdgeEnd*> expected_ccw = {
      &e_pos_x, &e_low, &e_mid,   &e_same_dx_higher_y,
      &e_pos_y, &e_nw,  &e_neg_x, &e_sw,
      &e_neg_y, &e_se,
  };
  EXPECT_EQ(rays, expected_ccw);

  // Collinear rays of different lengths compare equivalent in EdgeEndLT.
  std::set<EdgeEnd*, EdgeEndLT> ray_set;
  ray_set.insert(&e_mid);
  ray_set.insert(&e_mid_long);
  EXPECT_EQ(ray_set.size(), 1u);
}

TEST(EdgeEndTest, ComputeLabelIsNoOpInBaseClass) {
  Label lbl(0, Location::INTERIOR);
  EdgeEnd ee(nullptr, {0.0, 0.0}, {1.0, 0.0}, lbl);
  EdgeEnd* base_ptr = &ee;

  base_ptr->computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());
  EXPECT_EQ(ee.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(ee.getLabel().getLocation(1), Location::NONE);

  base_ptr->computeLabel(BoundaryNodeRule::getBoundaryEndPoint());
  EXPECT_EQ(ee.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(ee.getLabel().getLocation(1), Location::NONE);
}

TEST(EdgeEndTest, PrintAndStreamOperator) {
  Label lbl(0, Location::BOUNDARY);
  EdgeEnd ee(nullptr, {1.0, 2.0}, {4.0, 6.0}, lbl);

  std::string printed = ee.print();
  std::ostringstream oss;
  oss << ee;
  EXPECT_EQ(printed, oss.str());

  std::ostringstream expected_angle;
  expected_angle << "0:" << std::atan2(4.0, 3.0);

  EXPECT_THAT(printed, HasSubstr("EdgeEnd: "));
  EXPECT_THAT(printed, HasSubstr(expected_angle.str()));
  EXPECT_THAT(printed, HasSubstr("A:"));
}

}  // namespace
}  // namespace geos_google
