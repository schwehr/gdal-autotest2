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

// Unit tests for third_party/geos/src/operation/relate/EdgeEndBuilder.cpp and
// third_party/geos/include/geos/operation/relate/EdgeEndBuilder.h.

#include "third_party/geos/include/geos/operation/relate/EdgeEndBuilder.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geom/Quadrant.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersection.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersectionList.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Location;
using geos::geom::Position;
using geos::geom::Quadrant;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeEnd;
using geos::geomgraph::EdgeIntersection;
using geos::geomgraph::EdgeIntersectionList;
using geos::geomgraph::Label;
using geos::operation::relate::EdgeEndBuilder;

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords,
                               const Label& label) {
  return std::make_unique<Edge>(new CoordinateSequence(coords), label);
}

void ExpectEdgeEndCoords(EdgeEnd& ee, const Edge* expected_edge,
                         const Coordinate& expected_p0,
                         const Coordinate& expected_p1) {
  EXPECT_EQ(ee.getEdge(), expected_edge);
  EXPECT_TRUE(ee.getCoordinate().equals2D(expected_p0))
      << "got " << ee.getCoordinate() << ", expected " << expected_p0;
  EXPECT_TRUE(ee.getDirectedCoordinate().equals2D(expected_p1))
      << "got " << ee.getDirectedCoordinate() << ", expected " << expected_p1;
}

void ExpectAreaLabel(const Label& label, std::uint32_t geom_index, Location on,
                     Location left, Location right) {
  EXPECT_TRUE(label.isArea(geom_index));
  EXPECT_EQ(label.getLocation(geom_index, Position::ON), on);
  EXPECT_EQ(label.getLocation(geom_index, Position::LEFT), left);
  EXPECT_EQ(label.getLocation(geom_index, Position::RIGHT), right);
}

class TestableEdgeEndBuilder final : public EdgeEndBuilder {
 public:
  using EdgeEndBuilder::createEdgeEndForNext;
  using EdgeEndBuilder::createEdgeEndForPrev;
};

TEST(EdgeEndBuilderTest, ComputeEdgeEndsEmptyVectorReturnsEmpty) {
  EdgeEndBuilder builder;
  std::vector<Edge*> edges;
  std::vector<std::unique_ptr<EdgeEnd>> result =
      builder.computeEdgeEnds(&edges);
  EXPECT_TRUE(result.empty());
}

TEST(EdgeEndBuilderTest, SingleTwoPointLineEdgeNoExtraIntersections) {
  auto edge = MakeEdge({Coordinate(0, 0), Coordinate(10, 0)},
                       Label(0, Location::INTERIOR));
  EdgeEndBuilder builder;
  std::vector<std::unique_ptr<EdgeEnd>> result;
  builder.computeEdgeEnds(edge.get(), result);

  ASSERT_EQ(result.size(), 2u);
  ExpectEdgeEndCoords(*result[0], edge.get(), Coordinate(0, 0),
                      Coordinate(10, 0));
  EXPECT_EQ(result[0]->getQuadrant(), +Quadrant::NE);
  EXPECT_DOUBLE_EQ(result[0]->getDx(), 10.0);
  EXPECT_DOUBLE_EQ(result[0]->getDy(), 0.0);
  EXPECT_TRUE(result[0]->getLabel().isLine(0));
  EXPECT_EQ(result[0]->getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_TRUE(result[0]->getLabel().isNull(1));

  ExpectEdgeEndCoords(*result[1], edge.get(), Coordinate(10, 0),
                      Coordinate(0, 0));
  EXPECT_EQ(result[1]->getQuadrant(), +Quadrant::NW);
  EXPECT_DOUBLE_EQ(result[1]->getDx(), -10.0);
  EXPECT_DOUBLE_EQ(result[1]->getDy(), 0.0);
  EXPECT_TRUE(result[1]->getLabel().isLine(0));
  EXPECT_EQ(result[1]->getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_TRUE(result[1]->getLabel().isNull(1));
}

TEST(EdgeEndBuilderTest, MultiSegmentAreaEdgeFlipsLabelOnlyForPrev) {
  auto edge = MakeEdge(
      {Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10)},
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  EdgeEndBuilder builder;
  std::vector<std::unique_ptr<EdgeEnd>> result;
  builder.computeEdgeEnds(edge.get(), result);

  ASSERT_EQ(result.size(), 2u);
  // Start endpoint creates only a "next" stub pointing to vertex 1.
  ExpectEdgeEndCoords(*result[0], edge.get(), Coordinate(0, 0),
                      Coordinate(10, 0));
  ExpectAreaLabel(result[0]->getLabel(), 0, Location::BOUNDARY,
                  Location::INTERIOR, Location::EXTERIOR);
  EXPECT_TRUE(result[0]->getLabel().isNull(1));

  // End endpoint creates only a "prev" stub pointing back to vertex 1 with
  // flipped LEFT/RIGHT side locations.
  ExpectEdgeEndCoords(*result[1], edge.get(), Coordinate(10, 10),
                      Coordinate(10, 0));
  ExpectAreaLabel(result[1]->getLabel(), 0, Location::BOUNDARY,
                  Location::EXTERIOR, Location::INTERIOR);
  EXPECT_TRUE(result[1]->getLabel().isNull(1));
}

TEST(EdgeEndBuilderTest, ClosedRingCreatesTwoOppositeStubsAtSharedEndpoint) {
  auto edge = MakeEdge(
      {Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
       Coordinate(0, 10), Coordinate(0, 0)},
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  EdgeEndBuilder builder;
  std::vector<std::unique_ptr<EdgeEnd>> result;
  builder.computeEdgeEnds(edge.get(), result);

  ASSERT_EQ(result.size(), 2u);
  ExpectEdgeEndCoords(*result[0], edge.get(), Coordinate(0, 0),
                      Coordinate(10, 0));
  ExpectAreaLabel(result[0]->getLabel(), 0, Location::BOUNDARY,
                  Location::INTERIOR, Location::EXTERIOR);

  ExpectEdgeEndCoords(*result[1], edge.get(), Coordinate(0, 0),
                      Coordinate(0, 10));
  ExpectAreaLabel(result[1]->getLabel(), 0, Location::BOUNDARY,
                  Location::EXTERIOR, Location::INTERIOR);
}

TEST(EdgeEndBuilderTest, MultipleIntersectionsOnSameSegmentAddedOutOfOrder) {
  auto edge = MakeEdge(
      {Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10)},
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  EdgeIntersectionList& ei_list = edge->getEdgeIntersectionList();
  // Add out of order; EdgeIntersectionList sorts on iteration.
  ei_list.add(Coordinate(7, 0), 0, 7.0);
  ei_list.add(Coordinate(3, 0), 0, 3.0);

  EdgeEndBuilder builder;
  std::vector<std::unique_ptr<EdgeEnd>> result;
  builder.computeEdgeEnds(edge.get(), result);

  ASSERT_EQ(result.size(), 6u);
  // (0,0) -> (3,0) [next on same segment]
  ExpectEdgeEndCoords(*result[0], edge.get(), Coordinate(0, 0),
                      Coordinate(3, 0));
  ExpectAreaLabel(result[0]->getLabel(), 0, Location::BOUNDARY,
                  Location::INTERIOR, Location::EXTERIOR);

  // (3,0) -> (0,0) [prev on same segment, flipped]
  ExpectEdgeEndCoords(*result[1], edge.get(), Coordinate(3, 0),
                      Coordinate(0, 0));
  ExpectAreaLabel(result[1]->getLabel(), 0, Location::BOUNDARY,
                  Location::EXTERIOR, Location::INTERIOR);

  // (3,0) -> (7,0) [next on same segment]
  ExpectEdgeEndCoords(*result[2], edge.get(), Coordinate(3, 0),
                      Coordinate(7, 0));
  ExpectAreaLabel(result[2]->getLabel(), 0, Location::BOUNDARY,
                  Location::INTERIOR, Location::EXTERIOR);

  // (7,0) -> (3,0) [prev on same segment, flipped]
  ExpectEdgeEndCoords(*result[3], edge.get(), Coordinate(7, 0),
                      Coordinate(3, 0));
  ExpectAreaLabel(result[3]->getLabel(), 0, Location::BOUNDARY,
                  Location::EXTERIOR, Location::INTERIOR);

  // (7,0) -> (10,0) [next intersection is on segment 2, so uses vertex 1]
  ExpectEdgeEndCoords(*result[4], edge.get(), Coordinate(7, 0),
                      Coordinate(10, 0));
  ExpectAreaLabel(result[4]->getLabel(), 0, Location::BOUNDARY,
                  Location::INTERIOR, Location::EXTERIOR);

  // (10,10) -> (10,0) [prev intersection is on segment 0 < iPrev(1)]
  ExpectEdgeEndCoords(*result[5], edge.get(), Coordinate(10, 10),
                      Coordinate(10, 0));
  ExpectAreaLabel(result[5]->getLabel(), 0, Location::BOUNDARY,
                  Location::EXTERIOR, Location::INTERIOR);
}

TEST(EdgeEndBuilderTest, IntersectionAtInteriorVertexAndSecondSegment) {
  auto edge = MakeEdge(
      {Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
       Coordinate(0, 10)},
      Label(1, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR));
  EdgeIntersectionList& ei_list = edge->getEdgeIntersectionList();
  ei_list.add(Coordinate(4, 0), 0, 4.0);
  ei_list.add(Coordinate(10, 0), 1, 0.0);
  ei_list.add(Coordinate(10, 6), 1, 6.0);

  EdgeEndBuilder builder;
  std::vector<std::unique_ptr<EdgeEnd>> result;
  builder.computeEdgeEnds(edge.get(), result);

  ASSERT_EQ(result.size(), 8u);
  ExpectEdgeEndCoords(*result[0], edge.get(), Coordinate(0, 0),
                      Coordinate(4, 0));
  ExpectEdgeEndCoords(*result[1], edge.get(), Coordinate(4, 0),
                      Coordinate(0, 0));
  ExpectEdgeEndCoords(*result[2], edge.get(), Coordinate(4, 0),
                      Coordinate(10, 0));
  // At vertex 1 (dist == 0.0, segmentIndex == 1), iPrev decrements to 0 and
  // eiPrev is on segment 0 >= iPrev, so stub points back to (4, 0).
  ExpectEdgeEndCoords(*result[3], edge.get(), Coordinate(10, 0),
                      Coordinate(4, 0));
  ExpectAreaLabel(result[3]->getLabel(), 1, Location::BOUNDARY,
                  Location::INTERIOR, Location::EXTERIOR);

  // At vertex 1, next intersection is on segment 1, so stub points to (10, 6).
  ExpectEdgeEndCoords(*result[4], edge.get(), Coordinate(10, 0),
                      Coordinate(10, 6));
  ExpectAreaLabel(result[4]->getLabel(), 1, Location::BOUNDARY,
                  Location::EXTERIOR, Location::INTERIOR);

  ExpectEdgeEndCoords(*result[5], edge.get(), Coordinate(10, 6),
                      Coordinate(10, 0));
  ExpectEdgeEndCoords(*result[6], edge.get(), Coordinate(10, 6),
                      Coordinate(10, 10));
  ExpectEdgeEndCoords(*result[7], edge.get(), Coordinate(0, 10),
                      Coordinate(10, 10));
}

TEST(EdgeEndBuilderTest, DuplicateEndpointIntersectionsAreDeduplicated) {
  auto edge = MakeEdge({Coordinate(0, 0), Coordinate(10, 0)},
                       Label(0, Location::BOUNDARY));
  edge->getEdgeIntersectionList().add(Coordinate(0, 0), 0, 0.0);
  edge->getEdgeIntersectionList().add(Coordinate(10, 0), 1, 0.0);

  EdgeEndBuilder builder;
  std::vector<std::unique_ptr<EdgeEnd>> result;
  builder.computeEdgeEnds(edge.get(), result);

  ASSERT_EQ(result.size(), 2u);
  ExpectEdgeEndCoords(*result[0], edge.get(), Coordinate(0, 0),
                      Coordinate(10, 0));
  ExpectEdgeEndCoords(*result[1], edge.get(), Coordinate(10, 0),
                      Coordinate(0, 0));
}

TEST(EdgeEndBuilderTest, ComputeEdgeEndsVectorAggregatesAcrossMultipleEdges) {
  auto e1 = MakeEdge({Coordinate(0, 0), Coordinate(5, 0)},
                     Label(0, Location::INTERIOR));
  auto e2 = MakeEdge({Coordinate(0, 0), Coordinate(0, 5)},
                     Label(1, Location::BOUNDARY));
  e2->getEdgeIntersectionList().add(Coordinate(0, 2), 0, 2.0);

  std::vector<Edge*> edges{e1.get(), e2.get()};
  EdgeEndBuilder builder;
  std::vector<std::unique_ptr<EdgeEnd>> result =
      builder.computeEdgeEnds(&edges);

  ASSERT_EQ(result.size(), 6u);
  ExpectEdgeEndCoords(*result[0], e1.get(), Coordinate(0, 0), Coordinate(5, 0));
  ExpectEdgeEndCoords(*result[1], e1.get(), Coordinate(5, 0), Coordinate(0, 0));
  ExpectEdgeEndCoords(*result[2], e2.get(), Coordinate(0, 0), Coordinate(0, 2));
  ExpectEdgeEndCoords(*result[3], e2.get(), Coordinate(0, 2), Coordinate(0, 0));
  ExpectEdgeEndCoords(*result[4], e2.get(), Coordinate(0, 2), Coordinate(0, 5));
  ExpectEdgeEndCoords(*result[5], e2.get(), Coordinate(0, 5), Coordinate(0, 2));
}

TEST(EdgeEndBuilderTest, ProtectedHelpersHandleNullAdjacentIntersections) {
  auto edge = MakeEdge(
      {Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10)},
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  TestableEdgeEndBuilder builder;
  std::vector<std::unique_ptr<EdgeEnd>> result;

  EdgeIntersection ei_mid(Coordinate(4, 0), 0, 4.0);
  builder.createEdgeEndForPrev(edge.get(), result, &ei_mid, nullptr);
  ASSERT_EQ(result.size(), 1u);
  ExpectEdgeEndCoords(*result[0], edge.get(), Coordinate(4, 0),
                      Coordinate(0, 0));
  ExpectAreaLabel(result[0]->getLabel(), 0, Location::BOUNDARY,
                  Location::EXTERIOR, Location::INTERIOR);

  builder.createEdgeEndForNext(edge.get(), result, &ei_mid, nullptr);
  ASSERT_EQ(result.size(), 2u);
  ExpectEdgeEndCoords(*result[1], edge.get(), Coordinate(4, 0),
                      Coordinate(10, 0));
  ExpectAreaLabel(result[1]->getLabel(), 0, Location::BOUNDARY,
                  Location::INTERIOR, Location::EXTERIOR);

  EdgeIntersection ei_start(Coordinate(0, 0), 0, 0.0);
  builder.createEdgeEndForPrev(edge.get(), result, &ei_start, nullptr);
  EXPECT_EQ(result.size(), 2u);

  EdgeIntersection ei_end(Coordinate(10, 10), 2, 0.0);
  builder.createEdgeEndForNext(edge.get(), result, &ei_end, nullptr);
  EXPECT_EQ(result.size(), 2u);
}

}  // namespace
}  // namespace geos_google
