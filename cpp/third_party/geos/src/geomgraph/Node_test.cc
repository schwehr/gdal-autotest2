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

// Unit tests for third_party/geos/src/geomgraph/Node.cpp and
// third_party/geos/include/geos/geomgraph/Node.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geomgraph/Node.cpp

#include "third_party/geos/include/geos/geomgraph/Node.h"

#include <cmath>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdge.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdgeStar.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEndStar.h"
#include "third_party/geos/include/geos/geomgraph/GraphComponent.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::IntersectionMatrix;
using geos::geom::Location;
using geos::geomgraph::DirectedEdge;
using geos::geomgraph::DirectedEdgeStar;
using geos::geomgraph::Edge;
using geos::geomgraph::GraphComponent;
using geos::geomgraph::Label;
using geos::geomgraph::Node;
using geos::util::IllegalArgumentException;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::UnorderedElementsAre;

std::unique_ptr<Edge> MakeEdge(const Coordinate& p0, const Coordinate& p1) {
  auto seq = std::make_unique<CoordinateSequence>();
  seq->add(p0);
  seq->add(p1);
  return std::make_unique<Edge>(seq.release());
}

TEST(NodeTest, ConstructWithNullEdgesAnd2DCoordinate) {
  Node node(Coordinate(3.5, -2.5), nullptr);

  EXPECT_DOUBLE_EQ(node.getCoordinate().x, 3.5);
  EXPECT_DOUBLE_EQ(node.getCoordinate().y, -2.5);
  EXPECT_TRUE(std::isnan(node.getCoordinate().z));
  EXPECT_TRUE(node.getZ().empty());
  EXPECT_EQ(node.getEdges(), nullptr);
  EXPECT_TRUE(node.getLabel().isNull());
  EXPECT_EQ(node.getLabel().getGeometryCount(), 0);
  EXPECT_FALSE(node.isIsolated());
  EXPECT_FALSE(node.isIncidentEdgeInResult());
}

TEST(NodeTest, ConstructWith3DCoordinateInitializesZ) {
  Node node(Coordinate(1.0, 2.0, 5.0), nullptr);

  EXPECT_TRUE(node.getCoordinate().equals3D(Coordinate(1.0, 2.0, 5.0)));
  EXPECT_THAT(node.getZ(), ElementsAre(5.0));
}

TEST(NodeTest, ConstructWithEmptyEdgeStarInitializesEdgesWithoutResult) {
  auto star = std::make_unique<DirectedEdgeStar>();
  Node node(Coordinate(0.0, 0.0), star.release());

  ASSERT_NE(node.getEdges(), nullptr);
  EXPECT_EQ(node.getEdges()->getDegree(), 0u);
  EXPECT_FALSE(node.isIncidentEdgeInResult());
}

TEST(NodeTest, ConstructWithPrePopulatedEdgeStarAccumulatesDistinctZ) {
  std::unique_ptr<Edge> e1 =
      MakeEdge(Coordinate(1.0, 2.0, 10.0), Coordinate(2.0, 2.0, 10.0));
  std::unique_ptr<Edge> e2 =
      MakeEdge(Coordinate(1.0, 2.0, 20.0), Coordinate(1.0, 3.0, 20.0));
  std::unique_ptr<Edge> e3 =
      MakeEdge(Coordinate(1.0, 2.0), Coordinate(0.0, 2.0));
  std::unique_ptr<Edge> e4 =
      MakeEdge(Coordinate(1.0, 2.0, 0.0), Coordinate(1.0, 1.0, 0.0));
  DirectedEdge de1(e1.get(), true);
  DirectedEdge de2(e2.get(), true);
  DirectedEdge de3(e3.get(), true);
  DirectedEdge de4(e4.get(), true);

  auto star = std::make_unique<DirectedEdgeStar>();
  star->insert(&de1);
  star->insert(&de2);
  star->insert(&de3);
  star->insert(&de4);

  Node node(Coordinate(1.0, 2.0, 0.0), star.release());

  ASSERT_NE(node.getEdges(), nullptr);
  EXPECT_EQ(node.getEdges()->getDegree(), 4u);
  EXPECT_THAT(node.getZ(), UnorderedElementsAre(0.0, 10.0, 20.0));
  EXPECT_DOUBLE_EQ(node.getCoordinate().z, 10.0);
}

TEST(NodeTest, AddValidEdgeEndsSetsNodeAndUpdatesZ) {
  std::unique_ptr<Edge> e1 =
      MakeEdge(Coordinate(0.0, 0.0, 4.0), Coordinate(1.0, 0.0, 0.0));
  std::unique_ptr<Edge> e2 =
      MakeEdge(Coordinate(0.0, 0.0, 4.0), Coordinate(0.0, 1.0, 0.0));
  std::unique_ptr<Edge> e3 =
      MakeEdge(Coordinate(0.0, 0.0, 8.0), Coordinate(-1.0, 0.0, 0.0));
  std::unique_ptr<Edge> e4 =
      MakeEdge(Coordinate(0.0, 0.0), Coordinate(0.0, -1.0));
  DirectedEdge de1(e1.get(), true);
  DirectedEdge de2(e2.get(), true);
  DirectedEdge de3(e3.get(), true);
  DirectedEdge de4(e4.get(), true);

  auto star = std::make_unique<DirectedEdgeStar>();
  Node node(Coordinate(0.0, 0.0), star.release());

  node.add(&de1);
  EXPECT_EQ(de1.getNode(), &node);
  EXPECT_EQ(node.getEdges()->getDegree(), 1u);
  EXPECT_THAT(node.getZ(), ElementsAre(4.0));
  EXPECT_DOUBLE_EQ(node.getCoordinate().z, 4.0);

  // Duplicate Z is ignored, but EdgeEnd is still inserted and linked.
  node.add(&de2);
  EXPECT_EQ(de2.getNode(), &node);
  EXPECT_EQ(node.getEdges()->getDegree(), 2u);
  EXPECT_THAT(node.getZ(), ElementsAre(4.0));
  EXPECT_DOUBLE_EQ(node.getCoordinate().z, 4.0);

  // Distinct Z updates the running average.
  node.add(&de3);
  EXPECT_EQ(de3.getNode(), &node);
  EXPECT_EQ(node.getEdges()->getDegree(), 3u);
  EXPECT_THAT(node.getZ(), ElementsAre(4.0, 8.0));
  EXPECT_DOUBLE_EQ(node.getCoordinate().z, 6.0);

  // NaN Z leaves stored Z values and average unchanged.
  node.add(&de4);
  EXPECT_EQ(de4.getNode(), &node);
  EXPECT_EQ(node.getEdges()->getDegree(), 4u);
  EXPECT_THAT(node.getZ(), ElementsAre(4.0, 8.0));
  EXPECT_DOUBLE_EQ(node.getCoordinate().z, 6.0);
}

TEST(NodeTest, AddEdgeEndWithMismatchedCoordinateThrowsIllegalArgument) {
  std::unique_ptr<Edge> bad_x =
      MakeEdge(Coordinate(1.0, 0.0), Coordinate(2.0, 0.0));
  std::unique_ptr<Edge> bad_y =
      MakeEdge(Coordinate(0.0, 1.0), Coordinate(0.0, 2.0));
  std::unique_ptr<Edge> same_xy_diff_z =
      MakeEdge(Coordinate(0.0, 0.0, 7.0), Coordinate(1.0, 1.0, 7.0));
  DirectedEdge de_bad_x(bad_x.get(), true);
  DirectedEdge de_bad_y(bad_y.get(), true);
  DirectedEdge de_valid(same_xy_diff_z.get(), true);

  auto star = std::make_unique<DirectedEdgeStar>();
  Node node(Coordinate(0.0, 0.0), star.release());

  try {
    node.add(&de_bad_x);
    FAIL() << "Expected IllegalArgumentException";
  } catch (const IllegalArgumentException& ex) {
    EXPECT_THAT(ex.what(), HasSubstr("invalid for node"));
  }
  EXPECT_EQ(de_bad_x.getNode(), nullptr);
  EXPECT_EQ(node.getEdges()->getDegree(), 0u);

  EXPECT_THROW(node.add(&de_bad_y), IllegalArgumentException);
  EXPECT_EQ(de_bad_y.getNode(), nullptr);
  EXPECT_EQ(node.getEdges()->getDegree(), 0u);

  // Matching 2D coordinates with different Z succeed.
  node.add(&de_valid);
  EXPECT_EQ(de_valid.getNode(), &node);
  EXPECT_EQ(node.getEdges()->getDegree(), 1u);
  EXPECT_DOUBLE_EQ(node.getCoordinate().z, 7.0);
}

TEST(NodeTest, AddZSkipsNaNAndDuplicatesAndAveragesDistinctValues) {
  Node node(Coordinate(0.0, 0.0), nullptr);

  node.addZ(DoubleNotANumber);
  EXPECT_TRUE(node.getZ().empty());
  EXPECT_TRUE(std::isnan(node.getCoordinate().z));

  node.addZ(10.0);
  EXPECT_THAT(node.getZ(), ElementsAre(10.0));
  EXPECT_DOUBLE_EQ(node.getCoordinate().z, 10.0);

  node.addZ(10.0);
  EXPECT_THAT(node.getZ(), ElementsAre(10.0));
  EXPECT_DOUBLE_EQ(node.getCoordinate().z, 10.0);

  node.addZ(-2.0);
  EXPECT_THAT(node.getZ(), ElementsAre(10.0, -2.0));
  EXPECT_DOUBLE_EQ(node.getCoordinate().z, 4.0);

  node.addZ(DoubleNotANumber);
  EXPECT_THAT(node.getZ(), ElementsAre(10.0, -2.0));
  EXPECT_DOUBLE_EQ(node.getCoordinate().z, 4.0);

  node.addZ(4.0);
  EXPECT_THAT(node.getZ(), ElementsAre(10.0, -2.0, 4.0));
  EXPECT_DOUBLE_EQ(node.getCoordinate().z, 4.0);
}

TEST(NodeTest, IsIncidentEdgeInResultChecksUnderlyingEdges) {
  std::unique_ptr<Edge> e1 =
      MakeEdge(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0));
  std::unique_ptr<Edge> e2 =
      MakeEdge(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0));
  DirectedEdge de1(e1.get(), true);
  DirectedEdge de2(e2.get(), true);

  auto star = std::make_unique<DirectedEdgeStar>();
  Node node(Coordinate(0.0, 0.0), star.release());
  node.add(&de1);
  node.add(&de2);

  EXPECT_FALSE(node.isIncidentEdgeInResult());

  // Only Edge::isInResult() determines node incident-result membership.
  de1.setInResult(true);
  EXPECT_FALSE(node.isIncidentEdgeInResult());

  e2->setInResult(true);
  EXPECT_TRUE(node.isIncidentEdgeInResult());

  e2->setInResult(false);
  EXPECT_FALSE(node.isIncidentEdgeInResult());

  e1->setInResult(true);
  EXPECT_TRUE(node.isIncidentEdgeInResult());
}

TEST(NodeTest, SetLabelHandlesNullAndNonNullStatesAndUpdatesIsolation) {
  Node node(Coordinate(0.0, 0.0), nullptr);
  EXPECT_FALSE(node.isIsolated());

  // First call initializes from null label.
  node.setLabel(0, Location::INTERIOR);
  EXPECT_EQ(node.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(node.getLabel().getLocation(1), Location::NONE);
  EXPECT_TRUE(node.isIsolated());

  // Subsequent calls update existing non-null label.
  node.setLabel(0, Location::BOUNDARY);
  EXPECT_EQ(node.getLabel().getLocation(0), Location::BOUNDARY);
  EXPECT_TRUE(node.isIsolated());

  node.setLabel(1, Location::EXTERIOR);
  EXPECT_EQ(node.getLabel().getLocation(1), Location::EXTERIOR);
  EXPECT_FALSE(node.isIsolated());

  // Resetting both geometries returns the label to null; setting geometry 1
  // re-initializes from the null state.
  node.setLabel(0, Location::NONE);
  node.setLabel(1, Location::NONE);
  EXPECT_TRUE(node.getLabel().isNull());
  EXPECT_FALSE(node.isIsolated());

  node.setLabel(1, Location::INTERIOR);
  EXPECT_TRUE(node.isIsolated());
  EXPECT_EQ(node.getLabel().getLocation(0), Location::NONE);
  EXPECT_EQ(node.getLabel().getLocation(1), Location::INTERIOR);

  // Fresh node initialized directly on geometry index 1.
  Node geom1_only(Coordinate(2.0, 3.0), nullptr);
  geom1_only.setLabel(1, Location::EXTERIOR);
  EXPECT_TRUE(geom1_only.isIsolated());
  EXPECT_EQ(geom1_only.getLabel().getLocation(0), Location::NONE);
  EXPECT_EQ(geom1_only.getLabel().getLocation(1), Location::EXTERIOR);
}

TEST(NodeTest, SetLabelBoundaryAppliesMod2Rule) {
  Node node(Coordinate(0.0, 0.0), nullptr);

  // NONE -> BOUNDARY
  node.setLabelBoundary(0);
  EXPECT_EQ(node.getLabel().getLocation(0), Location::BOUNDARY);

  // BOUNDARY -> INTERIOR
  node.setLabelBoundary(0);
  EXPECT_EQ(node.getLabel().getLocation(0), Location::INTERIOR);

  // INTERIOR -> BOUNDARY
  node.setLabelBoundary(0);
  EXPECT_EQ(node.getLabel().getLocation(0), Location::BOUNDARY);

  // EXTERIOR -> BOUNDARY on geometry 1 while preserving geometry 0.
  node.setLabel(1, Location::EXTERIOR);
  node.setLabelBoundary(1);
  EXPECT_EQ(node.getLabel().getLocation(1), Location::BOUNDARY);
  EXPECT_EQ(node.getLabel().getLocation(0), Location::BOUNDARY);
}

TEST(NodeTest, ComputeMergedLocationEnforcesBoundaryPrecedence) {
  Node node(Coordinate(0.0, 0.0), nullptr);

  // Null source preserves destination location.
  EXPECT_EQ(node.computeMergedLocation(Label(), 0), Location::NONE);

  node.setLabel(0, Location::INTERIOR);
  EXPECT_EQ(node.computeMergedLocation(Label(), 0), Location::INTERIOR);
  EXPECT_EQ(node.computeMergedLocation(Label(0, Location::BOUNDARY), 0),
            Location::BOUNDARY);
  EXPECT_EQ(node.computeMergedLocation(Label(0, Location::EXTERIOR), 0),
            Location::EXTERIOR);

  // Existing EXTERIOR is overwritten by non-null incoming location.
  node.setLabel(0, Location::EXTERIOR);
  EXPECT_EQ(node.computeMergedLocation(Label(0, Location::INTERIOR), 0),
            Location::INTERIOR);
  EXPECT_EQ(node.computeMergedLocation(Label(0, Location::BOUNDARY), 0),
            Location::BOUNDARY);

  // Existing BOUNDARY is never overwritten by a non-boundary source.
  node.setLabel(0, Location::BOUNDARY);
  EXPECT_EQ(node.computeMergedLocation(Label(), 0), Location::BOUNDARY);
  EXPECT_EQ(node.computeMergedLocation(Label(0, Location::INTERIOR), 0),
            Location::BOUNDARY);
  EXPECT_EQ(node.computeMergedLocation(Label(0, Location::EXTERIOR), 0),
            Location::BOUNDARY);
  EXPECT_EQ(node.computeMergedLocation(Label(0, Location::BOUNDARY), 0),
            Location::BOUNDARY);

  // Null destination takes non-null source location on geometry 1.
  EXPECT_EQ(node.computeMergedLocation(Label(1, Location::INTERIOR), 1),
            Location::INTERIOR);
}

TEST(NodeTest, MergeLabelUpdatesOnlyNullGeometryLocations) {
  Node target(Coordinate(0.0, 0.0), nullptr);

  // Merging a null label leaves both geometries NONE.
  target.mergeLabel(Label());
  EXPECT_TRUE(target.getLabel().isNull());

  target.mergeLabel(Label(0, Location::INTERIOR));
  EXPECT_EQ(target.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(target.getLabel().getLocation(1), Location::NONE);

  // Geometry 0 is already non-NONE so it remains INTERIOR; geometry 1 becomes
  // BOUNDARY.
  target.mergeLabel(Label(Location::BOUNDARY));
  EXPECT_EQ(target.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(target.getLabel().getLocation(1), Location::BOUNDARY);

  // Merge from another non-null Node.
  Node source(Coordinate(1.0, 1.0), nullptr);
  source.setLabel(0, Location::BOUNDARY);
  source.setLabel(1, Location::INTERIOR);

  Node receiver(Coordinate(1.0, 1.0), nullptr);
  receiver.setLabel(0, Location::EXTERIOR);
  receiver.mergeLabel(source);
  EXPECT_EQ(receiver.getLabel().getLocation(0), Location::EXTERIOR);
  EXPECT_EQ(receiver.getLabel().getLocation(1), Location::INTERIOR);

  // Merge from a single-geometry Node.
  Node single_geom_source(Coordinate(1.0, 1.0), nullptr);
  single_geom_source.setLabel(1, Location::BOUNDARY);
  Node empty_receiver(Coordinate(1.0, 1.0), nullptr);
  empty_receiver.mergeLabel(single_geom_source);
  EXPECT_EQ(empty_receiver.getLabel().getLocation(0), Location::NONE);
  EXPECT_EQ(empty_receiver.getLabel().getLocation(1), Location::BOUNDARY);
}

TEST(NodeTest, PrintAndStreamOperatorFormat2DAnd3DNodes) {
  Node node2d(Coordinate(1.5, 2.5), nullptr);
  node2d.setLabel(0, Location::BOUNDARY);

  std::ostringstream oss;
  std::ostream& returned = (oss << node2d);
  EXPECT_EQ(&returned, &oss);
  EXPECT_EQ(node2d.print(), oss.str());
  EXPECT_THAT(node2d.print(), HasSubstr("Node["));
  EXPECT_THAT(node2d.print(), HasSubstr("  POINT(1.5 2.5)\n"));
  EXPECT_THAT(node2d.print(), HasSubstr("  lbl: A:b B:-"));

  Node node3d(Coordinate(1.5, 2.5, 3.5), nullptr);
  EXPECT_THAT(node3d.print(), HasSubstr("  POINT(1.5 2.5 3.5)\n"));
  EXPECT_THAT(node3d.print(), HasSubstr("  lbl: A:- B:-"));
}

TEST(NodeTest, UpdateIMLeavesIntersectionMatrixUnchanged) {
  Node node(Coordinate(0.0, 0.0), nullptr);
  node.setLabel(0, Location::INTERIOR);
  node.setLabel(1, Location::BOUNDARY);

  IntersectionMatrix im;
  const std::string initial = im.toString();
  node.updateIM(im);
  EXPECT_EQ(im.toString(), initial);
}

TEST(NodeTest, PolymorphicDestructionAndConstQueriesWork) {
  std::unique_ptr<Edge> e =
      MakeEdge(Coordinate(4.0, 5.0, 9.0), Coordinate(6.0, 5.0, 9.0));
  e->setInResult(true);
  DirectedEdge de(e.get(), true);

  auto star = std::make_unique<DirectedEdgeStar>();
  auto owned_node =
      std::make_unique<Node>(Coordinate(4.0, 5.0), star.release());
  owned_node->add(&de);
  owned_node->setLabel(0, Location::BOUNDARY);

  const Node& const_node = *owned_node;
  EXPECT_TRUE(const_node.getCoordinate().equals3D(Coordinate(4.0, 5.0, 9.0)));
  EXPECT_TRUE(const_node.isIsolated());
  EXPECT_TRUE(const_node.isIncidentEdgeInResult());
  EXPECT_THAT(const_node.getZ(), ElementsAre(9.0));
  EXPECT_THAT(const_node.print(), HasSubstr("  POINT(4 5 9)\n"));

  std::unique_ptr<GraphComponent> base = std::move(owned_node);
  EXPECT_TRUE(base->isIsolated());
  base.reset();
}

}  // namespace
}  // namespace geos_google
