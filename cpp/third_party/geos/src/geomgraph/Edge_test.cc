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

#include "third_party/geos/include/geos/geomgraph/Edge.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geomgraph/Depth.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersection.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersectionList.h"
#include "third_party/geos/include/geos/geomgraph/GraphComponent.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/index/MonotoneChainEdge.h"

namespace geos_google {
namespace {

using ::geos::algorithm::LineIntersector;
using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::Dimension;
using ::geos::geom::Envelope;
using ::geos::geom::IntersectionMatrix;
using ::geos::geom::Location;
using ::geos::geom::Position;
using ::geos::geomgraph::Edge;
using ::geos::geomgraph::EdgeIntersection;
using ::geos::geomgraph::GraphComponent;
using ::geos::geomgraph::Label;
using ::geos::geomgraph::index::MonotoneChainEdge;
using ::testing::HasSubstr;

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(seq.release());
}

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords,
                               const Label& label) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(seq.release(), label);
}

TEST(EdgeTest, ConstructWithoutLabelInitializesDefaults) {
  auto edge = MakeEdge({{1.0, 2.0}, {4.0, 6.0}, {7.0, 3.0}});

  EXPECT_EQ(edge->getNumPoints(), 3u);
  EXPECT_EQ(edge->getMaximumSegmentIndex(), 2u);
  EXPECT_EQ(edge->getCoordinate(), Coordinate(1.0, 2.0));
  EXPECT_TRUE(std::isnan(edge->getCoordinate().z));
  EXPECT_EQ(edge->getCoordinate(0), Coordinate(1.0, 2.0));
  EXPECT_EQ(edge->getCoordinate(1), Coordinate(4.0, 6.0));
  EXPECT_EQ(edge->getCoordinate(2), Coordinate(7.0, 3.0));

  ASSERT_NE(edge->getCoordinates(), nullptr);
  EXPECT_EQ(edge->getCoordinates()->getSize(), 3u);

  EXPECT_TRUE(edge->getLabel().isNull());
  EXPECT_TRUE(edge->getDepth().isNull());
  EXPECT_EQ(edge->getDepthDelta(), 0);
  EXPECT_TRUE(edge->isIsolated());
  EXPECT_TRUE(edge->getEdgeIntersectionList().isEmpty());

  const Edge& const_edge = *edge;
  EXPECT_TRUE(const_edge.getEdgeIntersectionList().isEmpty());

  const Envelope* env = edge->getEnvelope();
  ASSERT_NE(env, nullptr);
  EXPECT_DOUBLE_EQ(env->getMinX(), 1.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 7.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), 2.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 6.0);
}

TEST(EdgeTest, ConstructWithLabelAndMutateComponentProperties) {
  Label lbl(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR);
  lbl.setLocation(1, Position::ON, Location::INTERIOR);
  auto edge = MakeEdge({{0.0, 0.0}, {10.0, 0.0}}, lbl);

  EXPECT_EQ(edge->getLabel().getLocation(0, Position::ON), Location::BOUNDARY);
  EXPECT_EQ(edge->getLabel().getLocation(0, Position::LEFT),
            Location::INTERIOR);
  EXPECT_EQ(edge->getLabel().getLocation(0, Position::RIGHT),
            Location::EXTERIOR);
  EXPECT_EQ(edge->getLabel().getLocation(1, Position::ON), Location::INTERIOR);

  edge->setDepthDelta(-1);
  EXPECT_EQ(edge->getDepthDelta(), -1);
  edge->setDepthDelta(2);
  EXPECT_EQ(edge->getDepthDelta(), 2);

  edge->setIsolated(false);
  EXPECT_FALSE(edge->isIsolated());
  edge->setIsolated(true);
  EXPECT_TRUE(edge->isIsolated());

  edge->setInResult(true);
  EXPECT_TRUE(edge->isInResult());
  edge->setCovered(true);
  EXPECT_TRUE(edge->isCovered());
  EXPECT_TRUE(edge->isCoveredSet());
  edge->setVisited(true);
  EXPECT_TRUE(edge->isVisited());
}

TEST(EdgeTest, UpdateIMHandlesLineAreaAndPartialLabels) {
  Label line_lbl(Location::INTERIOR);
  line_lbl.setLocation(1, Location::BOUNDARY);
  IntersectionMatrix im_line;
  Edge::updateIM(line_lbl, im_line);
  EXPECT_EQ(im_line.get(Location::INTERIOR, Location::BOUNDARY), +Dimension::L);
  EXPECT_EQ(im_line.toString(), "F1FFFFFFF");

  Label single_line_lbl(0, Location::INTERIOR);
  IntersectionMatrix im_single;
  Edge::updateIM(single_line_lbl, im_single);
  EXPECT_EQ(im_single.toString(), "FFFFFFFFF");

  Label area_lbl(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR);
  area_lbl.merge(
      Label(1, Location::INTERIOR, Location::EXTERIOR, Location::INTERIOR));
  IntersectionMatrix im_area;
  Edge::updateIM(area_lbl, im_area);
  EXPECT_EQ(im_area.get(Location::BOUNDARY, Location::INTERIOR), +Dimension::L);
  EXPECT_EQ(im_area.get(Location::INTERIOR, Location::EXTERIOR), +Dimension::A);
  EXPECT_EQ(im_area.get(Location::EXTERIOR, Location::INTERIOR), +Dimension::A);
  EXPECT_EQ(im_area.toString(), "FF21FF2FF");

  // Area label for geometry 0 paired with line label for geometry 1 ignores
  // undefined LEFT/RIGHT entries on geometry 1.
  Label mixed_lbl(1, Location::INTERIOR);
  mixed_lbl.merge(
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  IntersectionMatrix im_mixed;
  Edge::updateIM(mixed_lbl, im_mixed);
  EXPECT_EQ(im_mixed.toString(), "FFF1FFFFF");

  // Verify Edge::computeIM and GraphComponent::updateIM forward to
  // Edge::updateIM.
  auto edge = MakeEdge({{0.0, 0.0}, {1.0, 0.0}}, area_lbl);
  IntersectionMatrix im_component;
  edge->computeIM(im_component);
  EXPECT_EQ(im_component.toString(), "FF21FF2FF");

  IntersectionMatrix im_base;
  GraphComponent& component = *edge;
  component.updateIM(im_base);
  EXPECT_EQ(im_base.toString(), "FF21FF2FF");

  // Pre-existing higher dimensions are preserved by setAtLeastIfValid.
  IntersectionMatrix im_higher("222222222");
  edge->computeIM(im_higher);
  EXPECT_EQ(im_higher.toString(), "222222222");

  // Line label for geometry 0 paired with area label for geometry 1.
  Label rev_mixed_lbl(0, Location::INTERIOR);
  rev_mixed_lbl.merge(
      Label(1, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  IntersectionMatrix im_rev_mixed;
  Edge::updateIM(rev_mixed_lbl, im_rev_mixed);
  EXPECT_EQ(im_rev_mixed.toString(), "F1FFFFFFF");
}

TEST(EdgeTest, GetMonotoneChainEdgeCreatesAndCachesInstance) {
  auto edge = MakeEdge({{0.0, 0.0}, {1.0, 2.0}, {3.0, 1.0}, {5.0, 4.0}});

  MonotoneChainEdge* mce1 = edge->getMonotoneChainEdge();
  ASSERT_NE(mce1, nullptr);
  EXPECT_EQ(mce1->getCoordinates(), edge->getCoordinates());
  EXPECT_GE(mce1->getStartIndexes().size(), 2u);

  MonotoneChainEdge* mce2 = edge->getMonotoneChainEdge();
  EXPECT_EQ(mce2, mce1);
}

TEST(EdgeTest, IsClosedChecksEndpointsIn2D) {
  auto open_edge = MakeEdge({{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}});
  EXPECT_FALSE(open_edge->isClosed());

  auto closed_edge = MakeEdge({{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {0.0, 0.0}});
  EXPECT_TRUE(closed_edge->isClosed());

  auto closed_diff_z =
      MakeEdge({{0.0, 0.0, 1.0}, {1.0, 0.0, 2.0}, {0.0, 0.0, 9.0}});
  EXPECT_TRUE(closed_diff_z->isClosed());
}

TEST(EdgeTest, IsCollapsedAndGetCollapsedEdge) {
  Label line_lbl(Location::INTERIOR);
  auto line_edge = MakeEdge({{0.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}}, line_lbl);
  EXPECT_FALSE(line_edge->isCollapsed());

  Label area_lbl(Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR);
  auto two_pt_area = MakeEdge({{0.0, 0.0}, {1.0, 1.0}}, area_lbl);
  EXPECT_FALSE(two_pt_area->isCollapsed());

  auto four_pt_area =
      MakeEdge({{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}}, area_lbl);
  EXPECT_FALSE(four_pt_area->isCollapsed());

  auto non_returning_three_pt =
      MakeEdge({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}, area_lbl);
  EXPECT_FALSE(non_returning_three_pt->isCollapsed());

  Label mixed_lbl(0, Location::BOUNDARY, Location::INTERIOR,
                  Location::EXTERIOR);
  mixed_lbl.setLocation(1, Position::ON, Location::INTERIOR);
  auto collapsed_area =
      MakeEdge({{0.0, 0.0, 1.0}, {5.0, 5.0, 2.0}, {0.0, 0.0, 8.0}}, mixed_lbl);
  EXPECT_TRUE(collapsed_area->isCollapsed());

  std::unique_ptr<Edge> collapsed(collapsed_area->getCollapsedEdge());
  ASSERT_NE(collapsed, nullptr);
  EXPECT_EQ(collapsed->getNumPoints(), 2u);
  EXPECT_EQ(collapsed->getCoordinate(0), Coordinate(0.0, 0.0));
  EXPECT_EQ(collapsed->getCoordinate(1), Coordinate(5.0, 5.0));
  EXPECT_DOUBLE_EQ(collapsed->getCoordinate(0).z, 1.0);
  EXPECT_DOUBLE_EQ(collapsed->getCoordinate(1).z, 2.0);
  EXPECT_FALSE(collapsed->getLabel().isArea());
  EXPECT_EQ(collapsed->getLabel().getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(collapsed->getLabel().getLocation(1), Location::INTERIOR);

  // Collapsed check also succeeds when only geometry 1 carries an area label.
  Label area_on_geom1(0, Location::BOUNDARY);
  area_on_geom1.merge(
      Label(1, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR));
  auto collapsed_geom1 =
      MakeEdge({{1.0, 2.0}, {3.0, 4.0}, {1.0, 2.0}}, area_on_geom1);
  EXPECT_TRUE(collapsed_geom1->isCollapsed());
  std::unique_ptr<Edge> collapsed1(collapsed_geom1->getCollapsedEdge());
  ASSERT_NE(collapsed1, nullptr);
  EXPECT_FALSE(collapsed1->getLabel().isArea());
  EXPECT_EQ(collapsed1->getLabel().getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(collapsed1->getLabel().getLocation(1), Location::BOUNDARY);
}

TEST(EdgeTest, AddIntersectionsAndVertexNormalization) {
  auto edge = MakeEdge({{0.0, 0.0, 1.0}, {10.0, 0.0, 7.0}, {20.0, 0.0, 9.0}});
  LineIntersector li;

  // Disjoint segments produce zero intersections.
  Coordinate p0(0.0, 0.0), p1(10.0, 0.0), q0(0.0, 5.0), q1(10.0, 5.0);
  li.computeIntersection(p0, p1, q0, q1);
  ASSERT_EQ(li.getIntersectionNum(), 0u);
  edge->addIntersections(&li, 0, 0);
  EXPECT_TRUE(edge->getEdgeIntersectionList().isEmpty());

  // Interior intersection on segment 0 stays on segment 0 with edge distance 5.
  Coordinate r0(5.0, -5.0), r1(5.0, 5.0);
  li.computeIntersection(p0, p1, r0, r1);
  ASSERT_EQ(li.getIntersectionNum(), 1u);
  edge->addIntersections(&li, 0, 0);

  // Intersection at segment 0's end vertex (10, 0) normalizes to segment 1,
  // distance 0.0, ignoring Z differences.
  Coordinate s0(0.0, 0.0, 1.0), s1(10.0, 0.0, 7.0);
  Coordinate t0(10.0, -5.0, 2.0), t1(10.0, 5.0, 4.0);
  li.computeIntersection(s0, s1, t0, t1);
  ASSERT_EQ(li.getIntersectionNum(), 1u);
  edge->addIntersections(&li, 0, 0);

  // Collinear overlap on segment 1 adds two intersections.
  Coordinate c0(10.0, 0.0), c1(20.0, 0.0), d0(12.0, 0.0), d1(18.0, 0.0);
  li.computeIntersection(c0, c1, d0, d1);
  ASSERT_EQ(li.getIntersectionNum(), 2u);
  edge->addIntersections(&li, 1, 0);

  std::vector<EdgeIntersection> intersections(
      edge->getEdgeIntersectionList().begin(),
      edge->getEdgeIntersectionList().end());
  ASSERT_EQ(intersections.size(), 4u);

  EXPECT_EQ(intersections[0].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(intersections[0].getDistance(), 5.0);
  EXPECT_EQ(intersections[0].getCoordinate(), Coordinate(5.0, 0.0));

  EXPECT_EQ(intersections[1].getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(intersections[1].getDistance(), 0.0);
  EXPECT_EQ(intersections[1].getCoordinate(), Coordinate(10.0, 0.0));

  EXPECT_EQ(intersections[2].getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(intersections[2].getDistance(), 2.0);
  EXPECT_EQ(intersections[2].getCoordinate(), Coordinate(12.0, 0.0));

  EXPECT_EQ(intersections[3].getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(intersections[3].getDistance(), 8.0);
  EXPECT_EQ(intersections[3].getCoordinate(), Coordinate(18.0, 0.0));

  // Test addIntersection when segmentIndex == getMaximumSegmentIndex()
  // (nextSegIndex == npts, skipping next-vertex normalization).
  auto edge2 = MakeEdge({{0.0, 0.0}, {10.0, 0.0}});
  Coordinate u0(10.0, 0.0), u1(0.0, 0.0), v0(10.0, -5.0), v1(10.0, 5.0);
  li.computeIntersection(u0, u1, v0, v1);
  ASSERT_EQ(li.getIntersectionNum(), 1u);
  edge2->addIntersection(&li, edge2->getMaximumSegmentIndex(), 0, 0);

  std::vector<EdgeIntersection> tail_ints(
      edge2->getEdgeIntersectionList().begin(),
      edge2->getEdgeIntersectionList().end());
  ASSERT_EQ(tail_ints.size(), 1u);
  EXPECT_EQ(tail_ints[0].getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(tail_ints[0].getDistance(), 0.0);
  EXPECT_EQ(tail_ints[0].getCoordinate(), Coordinate(10.0, 0.0));

  // Verify geomIndex == 1 reads edge distance along the second input segment.
  auto edge3 = MakeEdge({{0.0, 0.0}, {10.0, 0.0}});
  Coordinate cross0(3.0, -5.0), cross1(3.0, 5.0);
  Coordinate seg0(0.0, 0.0), seg1(10.0, 0.0);
  li.computeIntersection(cross0, cross1, seg0, seg1);
  ASSERT_EQ(li.getIntersectionNum(), 1u);
  edge3->addIntersections(&li, 0, 1);

  std::vector<EdgeIntersection> geom1_ints(
      edge3->getEdgeIntersectionList().begin(),
      edge3->getEdgeIntersectionList().end());
  ASSERT_EQ(geom1_ints.size(), 1u);
  EXPECT_EQ(geom1_ints[0].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(geom1_ints[0].getDistance(), 3.0);
  EXPECT_EQ(geom1_ints[0].getCoordinate(), Coordinate(3.0, 0.0));
}

TEST(EdgeTest, EqualsAndIsPointwiseEqual) {
  auto base = MakeEdge({{0.0, 0.0, 1.0}, {1.0, 2.0, 2.0}, {3.0, 1.0, 3.0}});
  auto same_diff_z =
      MakeEdge({{0.0, 0.0, 9.0}, {1.0, 2.0, 8.0}, {3.0, 1.0, 7.0}});
  auto reversed = MakeEdge({{3.0, 1.0}, {1.0, 2.0}, {0.0, 0.0}});
  auto shorter = MakeEdge({{0.0, 0.0}, {1.0, 2.0}});
  auto diff_mid_forward = MakeEdge({{0.0, 0.0}, {9.0, 9.0}, {3.0, 1.0}});
  auto diff_mid_reverse = MakeEdge({{3.0, 1.0}, {9.0, 9.0}, {0.0, 0.0}});
  auto diff_last = MakeEdge({{0.0, 0.0}, {1.0, 2.0}, {9.0, 9.0}});
  auto diff_all = MakeEdge({{5.0, 5.0}, {1.0, 2.0}, {6.0, 6.0}});
  auto palindrome = MakeEdge({{0.0, 0.0}, {2.0, 2.0}, {0.0, 0.0}});

  EXPECT_TRUE(base->equals(*same_diff_z));
  EXPECT_TRUE(base->equals(same_diff_z.get()));
  EXPECT_TRUE(*base == *same_diff_z);
  EXPECT_TRUE(base->isPointwiseEqual(same_diff_z.get()));
  EXPECT_TRUE(palindrome->equals(*palindrome));

  EXPECT_TRUE(base->equals(*reversed));
  EXPECT_TRUE(*base == *reversed);
  EXPECT_FALSE(base->isPointwiseEqual(reversed.get()));

  EXPECT_FALSE(base->equals(*shorter));
  EXPECT_FALSE(*base == *shorter);
  EXPECT_FALSE(base->isPointwiseEqual(shorter.get()));

  EXPECT_FALSE(base->equals(*diff_mid_forward));
  EXPECT_FALSE(base->isPointwiseEqual(diff_mid_forward.get()));
  EXPECT_FALSE(base->equals(*diff_mid_reverse));
  EXPECT_FALSE(base->equals(*diff_last));
  EXPECT_FALSE(base->isPointwiseEqual(diff_last.get()));
  EXPECT_FALSE(base->equals(*diff_all));
  EXPECT_FALSE(base->isPointwiseEqual(diff_all.get()));
}

TEST(EdgeTest, PrintPrintReverseAndStreamOperator) {
  Label lbl(Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR);
  auto edge = MakeEdge({{11.0, 22.0}, {33.0, 44.0}, {55.0, 66.0}}, lbl);
  edge->setDepthDelta(-2);

  std::string printed = edge->print();
  std::ostringstream oss;
  oss << *edge;
  EXPECT_EQ(printed, oss.str());
  EXPECT_THAT(printed, HasSubstr("edge"));
  EXPECT_THAT(printed, HasSubstr("LINESTRING"));
  EXPECT_THAT(printed, HasSubstr("-2"));

  std::string rev = edge->printReverse();
  EXPECT_THAT(rev, HasSubstr("EDGE (rev)"));
  EXPECT_THAT(rev, HasSubstr("label:"));
  EXPECT_THAT(rev, HasSubstr("depthDelta:-2"));
  EXPECT_THAT(rev, HasSubstr("LINESTRING("));

  std::size_t pos55 = rev.find("55");
  std::size_t pos33 = rev.find("33");
  std::size_t pos11 = rev.find("11");
  ASSERT_NE(pos55, std::string::npos);
  ASSERT_NE(pos33, std::string::npos);
  ASSERT_NE(pos11, std::string::npos);
  EXPECT_LT(pos55, pos33);
  EXPECT_LT(pos33, pos11);
}

}  // namespace
}  // namespace geos_google
