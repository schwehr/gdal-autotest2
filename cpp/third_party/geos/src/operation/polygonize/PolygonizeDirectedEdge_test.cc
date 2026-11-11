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

// Unit tests for
// third_party/geos/src/operation/polygonize/PolygonizeDirectedEdge.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/polygonize/PolygonizeDirectedEdge.cpp

#include "third_party/geos/include/geos/operation/polygonize/PolygonizeDirectedEdge.h"

#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/operation/polygonize/EdgeRing.h"
#include "third_party/geos/include/geos/operation/polygonize/PolygonizeEdge.h"
#include "third_party/geos/include/geos/planargraph/Node.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::operation::polygonize::EdgeRing;
using geos::operation::polygonize::PolygonizeDirectedEdge;
using geos::operation::polygonize::PolygonizeEdge;
using geos::planargraph::Node;
using geos::util::IllegalArgumentException;

TEST(PolygonizeDirectedEdgeTest, InitialStateAndForwardDirection) {
  Node from(Coordinate(0.0, 0.0));
  Node to(Coordinate(10.0, 0.0));
  const Coordinate dir_pt(5.0, 0.0);

  PolygonizeDirectedEdge edge(&from, &to, dir_pt, true);

  EXPECT_EQ(edge.getFromNode(), &from);
  EXPECT_EQ(edge.getToNode(), &to);
  EXPECT_TRUE(edge.getDirectionPt().equals2D(dir_pt));
  EXPECT_TRUE(edge.getEdgeDirection());
  EXPECT_EQ(edge.getQuadrant(), 0);
  EXPECT_DOUBLE_EQ(edge.getAngle(), 0.0);

  EXPECT_EQ(edge.getLabel(), -1L);
  EXPECT_EQ(edge.getNext(), nullptr);
  EXPECT_EQ(edge.getRing(), nullptr);
  EXPECT_FALSE(edge.isInRing());
  EXPECT_FALSE(edge.isMarked());
  EXPECT_FALSE(edge.isVisited());
}

TEST(PolygonizeDirectedEdgeTest, OppositeDirectionAndQuadrant) {
  Node from(Coordinate(10.0, 10.0));
  Node to(Coordinate(0.0, 0.0));
  const Coordinate dir_pt(5.0, 5.0);

  PolygonizeDirectedEdge edge(&from, &to, dir_pt, false);

  EXPECT_EQ(edge.getFromNode(), &from);
  EXPECT_EQ(edge.getToNode(), &to);
  EXPECT_TRUE(edge.getDirectionPt().equals2D(dir_pt));
  EXPECT_FALSE(edge.getEdgeDirection());
  EXPECT_EQ(edge.getQuadrant(), 2);
  EXPECT_NEAR(edge.getAngle(), -3.0 * std::atan2(1.0, 1.0), 1e-12);
  EXPECT_EQ(edge.getLabel(), -1L);
  EXPECT_EQ(edge.getNext(), nullptr);
  EXPECT_EQ(edge.getRing(), nullptr);
  EXPECT_FALSE(edge.isInRing());
}

TEST(PolygonizeDirectedEdgeTest, ThrowsWhenDirectionPointMatchesFromNode) {
  Node from(Coordinate(3.0, 4.0));
  Node to(Coordinate(8.0, 4.0));

  EXPECT_THROW(PolygonizeDirectedEdge(&from, &to, Coordinate(3.0, 4.0), true),
               IllegalArgumentException);
}

TEST(PolygonizeDirectedEdgeTest, SetAndGetLabel) {
  Node from(Coordinate(0.0, 0.0));
  Node to(Coordinate(1.0, 0.0));
  PolygonizeDirectedEdge edge(&from, &to, Coordinate(1.0, 0.0), true);

  EXPECT_EQ(edge.getLabel(), -1L);

  edge.setLabel(0L);
  EXPECT_EQ(edge.getLabel(), 0L);

  edge.setLabel(42L);
  EXPECT_EQ(edge.getLabel(), 42L);

  edge.setLabel(-99L);
  EXPECT_EQ(edge.getLabel(), -99L);

  edge.setLabel(std::numeric_limits<long>::max());
  EXPECT_EQ(edge.getLabel(), std::numeric_limits<long>::max());

  edge.setLabel(std::numeric_limits<long>::min());
  EXPECT_EQ(edge.getLabel(), std::numeric_limits<long>::min());
}

TEST(PolygonizeDirectedEdgeTest, SetAndGetNextFormsCycleAndResets) {
  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(10.0, 0.0));
  Node n2(Coordinate(0.0, 10.0));

  PolygonizeDirectedEdge de0(&n0, &n1, Coordinate(10.0, 0.0), true);
  PolygonizeDirectedEdge de1(&n1, &n2, Coordinate(0.0, 10.0), true);
  PolygonizeDirectedEdge de2(&n2, &n0, Coordinate(0.0, 0.0), true);

  EXPECT_EQ(de0.getNext(), nullptr);
  EXPECT_EQ(de1.getNext(), nullptr);
  EXPECT_EQ(de2.getNext(), nullptr);

  de0.setNext(&de1);
  de1.setNext(&de2);
  de2.setNext(&de0);

  EXPECT_EQ(de0.getNext(), &de1);
  EXPECT_EQ(de1.getNext(), &de2);
  EXPECT_EQ(de2.getNext(), &de0);

  de0.setNext(nullptr);
  EXPECT_EQ(de0.getNext(), nullptr);
}

TEST(PolygonizeDirectedEdgeTest, SetRingUpdatesIsInRingAndGetRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  EdgeRing ring1(factory.get());
  EdgeRing ring2(factory.get());

  Node from(Coordinate(0.0, 0.0));
  Node to(Coordinate(0.0, 5.0));
  PolygonizeDirectedEdge edge(&from, &to, Coordinate(0.0, 5.0), true);

  EXPECT_FALSE(edge.isInRing());
  EXPECT_EQ(edge.getRing(), nullptr);

  edge.setRing(&ring1);
  EXPECT_TRUE(edge.isInRing());
  EXPECT_EQ(edge.getRing(), &ring1);

  edge.setRing(&ring2);
  EXPECT_TRUE(edge.isInRing());
  EXPECT_EQ(edge.getRing(), &ring2);

  edge.setRing(nullptr);
  EXPECT_FALSE(edge.isInRing());
  EXPECT_EQ(edge.getRing(), nullptr);
}

TEST(PolygonizeDirectedEdgeTest, ParentEdgeSymAndGraphComponentIntegration) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto seq = std::make_unique<CoordinateSequence>(0u, 2u);
  seq->add(Coordinate(0.0, 0.0));
  seq->add(Coordinate(10.0, 0.0));
  std::unique_ptr<LineString> line = factory->createLineString(std::move(seq));

  Node n0(Coordinate(0.0, 0.0));
  Node n1(Coordinate(10.0, 0.0));
  Node n2(Coordinate(0.0, 10.0));

  PolygonizeDirectedEdge de_fwd(&n0, &n1, Coordinate(10.0, 0.0), true);
  PolygonizeDirectedEdge de_rev(&n1, &n0, Coordinate(0.0, 0.0), false);
  PolygonizeDirectedEdge de_up(&n0, &n2, Coordinate(0.0, 10.0), true);

  PolygonizeEdge parent(line.get());
  parent.setDirectedEdges(&de_fwd, &de_rev);

  EXPECT_EQ(de_fwd.getEdge(), &parent);
  EXPECT_EQ(de_rev.getEdge(), &parent);
  EXPECT_EQ(de_fwd.getSym(), &de_rev);
  EXPECT_EQ(de_rev.getSym(), &de_fwd);
  EXPECT_EQ(n0.getDegree(), 1u);
  EXPECT_EQ(n1.getDegree(), 1u);

  // de_up points along +y (pi/2) while de_fwd points along +x (0).
  EXPECT_LT(de_fwd.compareDirection(&de_up), 0);
  EXPECT_GT(de_up.compareDirection(&de_fwd), 0);
  EXPECT_EQ(de_fwd.compareDirection(&de_fwd), 0);

  de_fwd.setMarked(true);
  de_fwd.setVisited(true);
  EXPECT_TRUE(de_fwd.isMarked());
  EXPECT_TRUE(de_fwd.isVisited());
  EXPECT_FALSE(de_rev.isMarked());
  EXPECT_FALSE(de_rev.isVisited());
}

}  // namespace
}  // namespace geos_google
