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

// Unit tests for third_party/geos/src/geomgraph/EdgeIntersectionList.cpp and
// third_party/geos/include/geos/geomgraph/EdgeIntersectionList.h.

#include "third_party/geos/include/geos/geomgraph/EdgeIntersectionList.h"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersection.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Location;
using geos::geom::Position;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeIntersection;
using geos::geomgraph::EdgeIntersectionList;
using geos::geomgraph::Label;
using testing::HasSubstr;

std::vector<std::unique_ptr<Edge>> SplitEdgesAndOwn(
    EdgeIntersectionList& list) {
  std::vector<Edge*> raw_edges;
  absl::Cleanup delete_raw = [&raw_edges] {
    for (Edge* e : raw_edges) {
      delete e;
    }
  };
  list.addSplitEdges(&raw_edges);
  std::vector<std::unique_ptr<Edge>> owned;
  owned.reserve(raw_edges.size());
  for (Edge* e : raw_edges) {
    owned.emplace_back(e);
  }
  raw_edges.clear();
  return owned;
}

TEST(EdgeIntersectionListTest, EmptyListInitialState) {
  Edge edge(
      new CoordinateSequence({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)}),
      Label(Location::INTERIOR));
  const EdgeIntersectionList& list = edge.getEdgeIntersectionList();

  EXPECT_EQ(list.edge, &edge);
  EXPECT_TRUE(list.isEmpty());
  EXPECT_EQ(list.begin(), list.end());
  EXPECT_FALSE(list.isIntersection(Coordinate(0.0, 0.0)));
  EXPECT_FALSE(list.isIntersection(Coordinate(5.0, 0.0)));
  EXPECT_EQ(list.print(), "Intersections:\n");

  std::ostringstream oss;
  oss << list;
  EXPECT_EQ(oss.str(), "Intersections:\n");
}

TEST(EdgeIntersectionListTest, AddDeduplicatesAndMaintainsSortedness) {
  Edge edge(new CoordinateSequence({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                                    Coordinate(20.0, 0.0)}),
            Label(Location::INTERIOR));
  EdgeIntersectionList list(&edge);

  // First element enters the empty-vector fast path.
  list.add(Coordinate(15.0, 0.0), 1, 5.0);
  EXPECT_FALSE(list.isEmpty());

  // Consecutive duplicate (same segmentIndex and dist) is skipped.
  list.add(Coordinate(15.0, 0.0), 1, 5.0);

  // Same segmentIndex with smaller dist is appended out of order.
  list.add(Coordinate(12.0, 0.0), 1, 2.0);

  // Non-adjacent duplicate (not equal to back()) is appended while unsorted.
  list.add(Coordinate(15.0, 0.0), 1, 5.0);

  // Iterating via begin() sorts and deduplicates, and sets sorted = true.
  std::vector<EdgeIntersection> pass1(list.begin(), list.end());
  ASSERT_EQ(pass1.size(), 2U);
  EXPECT_EQ(pass1[0].segmentIndex, 1U);
  EXPECT_DOUBLE_EQ(pass1[0].dist, 2.0);
  EXPECT_TRUE(pass1[0].coord.equals2D(Coordinate(12.0, 0.0)));
  EXPECT_EQ(pass1[1].segmentIndex, 1U);
  EXPECT_DOUBLE_EQ(pass1[1].dist, 5.0);
  EXPECT_TRUE(pass1[1].coord.equals2D(Coordinate(15.0, 0.0)));

  // Calling begin() while already sorted hits the fast path.
  std::vector<EdgeIntersection> pass2(list.begin(), list.end());
  EXPECT_EQ(pass2, pass1);

  // Adding a strictly greater entry preserves sortedness.
  list.add(Coordinate(18.0, 0.0), 1, 8.0);
  std::vector<EdgeIntersection> pass3(list.begin(), list.end());
  ASSERT_EQ(pass3.size(), 3U);
  EXPECT_DOUBLE_EQ(pass3[2].dist, 8.0);

  // Adding an entry smaller than back() resets sorted = false so next begin()
  // re-sorts.
  list.add(Coordinate(5.0, 0.0), 0, 5.0);
  std::vector<EdgeIntersection> pass4(list.begin(), list.end());
  ASSERT_EQ(pass4.size(), 4U);
  EXPECT_EQ(pass4[0].segmentIndex, 0U);
  EXPECT_DOUBLE_EQ(pass4[0].dist, 5.0);
  EXPECT_EQ(pass4[1].segmentIndex, 1U);
  EXPECT_DOUBLE_EQ(pass4[1].dist, 2.0);
  EXPECT_EQ(pass4[2].segmentIndex, 1U);
  EXPECT_DOUBLE_EQ(pass4[2].dist, 5.0);
  EXPECT_EQ(pass4[3].segmentIndex, 1U);
  EXPECT_DOUBLE_EQ(pass4[3].dist, 8.0);

  // Adding a non-adjacent duplicate after sorted = true resets sorted = false
  // and deduplicates on the next begin().
  list.add(Coordinate(12.0, 0.0), 1, 2.0);
  std::vector<EdgeIntersection> pass5(list.begin(), list.end());
  EXPECT_EQ(pass5.size(), 4U);
}

TEST(EdgeIntersectionListTest, IsIntersectionMatches2DCoordinatesBeforeSort) {
  Edge edge(new CoordinateSequence(
                {Coordinate(0.0, 0.0, 1.0), Coordinate(10.0, 0.0, 2.0)}),
            Label(Location::INTERIOR));
  EdgeIntersectionList list(&edge);
  list.add(Coordinate(5.0, 0.0, 9.0), 0, 5.0);
  list.add(Coordinate(2.0, 0.0, 3.0), 0, 2.0);

  EXPECT_TRUE(list.isIntersection(Coordinate(5.0, 0.0)));
  EXPECT_TRUE(list.isIntersection(Coordinate(2.0, 0.0, 99.0)));
  EXPECT_FALSE(list.isIntersection(Coordinate(0.0, 0.0)));
  EXPECT_FALSE(list.isIntersection(Coordinate(5.0, 0.1)));
}

TEST(EdgeIntersectionListTest, AddEndpointsAddsStartAndEndIdempotently) {
  Edge edge(
      new CoordinateSequence({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                              Coordinate(10.0, 10.0), Coordinate(0.0, 10.0)}),
      Label(Location::INTERIOR));
  EdgeIntersectionList list(&edge);

  list.addEndpoints();
  std::vector<EdgeIntersection> entries(list.begin(), list.end());
  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].segmentIndex, 0U);
  EXPECT_DOUBLE_EQ(entries[0].dist, 0.0);
  EXPECT_TRUE(entries[0].coord.equals2D(Coordinate(0.0, 0.0)));
  EXPECT_EQ(entries[1].segmentIndex, 3U);
  EXPECT_DOUBLE_EQ(entries[1].dist, 0.0);
  EXPECT_TRUE(entries[1].coord.equals2D(Coordinate(0.0, 10.0)));

  list.addEndpoints();
  std::vector<EdgeIntersection> after_repeat(list.begin(), list.end());
  EXPECT_EQ(after_repeat.size(), 2U);
}

TEST(EdgeIntersectionListTest, AddSplitEdgesWithNoInteriorIntersections) {
  Edge edge(
      new CoordinateSequence(
          {Coordinate(0.0, 0.0), Coordinate(10.0, 0.0), Coordinate(20.0, 5.0)}),
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));

  std::vector<std::unique_ptr<Edge>> splits = SplitEdgesAndOwn(edge.eiList);
  ASSERT_EQ(splits.size(), 1U);

  const CoordinateSequence* pts = splits[0]->getCoordinates();
  ASSERT_EQ(pts->size(), 3U);
  EXPECT_TRUE(pts->getAt(0).equals2D(Coordinate(0.0, 0.0)));
  EXPECT_TRUE(pts->getAt(1).equals2D(Coordinate(10.0, 0.0)));
  EXPECT_TRUE(pts->getAt(2).equals2D(Coordinate(20.0, 5.0)));

  const Label& lbl = splits[0]->getLabel();
  EXPECT_EQ(lbl.getLocation(0, Position::ON), Location::BOUNDARY);
  EXPECT_EQ(lbl.getLocation(0, Position::LEFT), Location::INTERIOR);
  EXPECT_EQ(lbl.getLocation(0, Position::RIGHT), Location::EXTERIOR);
}

TEST(EdgeIntersectionListTest,
     AddSplitEdgesAcrossMultipleSegmentsAndVertexIntersections) {
  Edge edge(
      new CoordinateSequence({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                              Coordinate(10.0, 10.0), Coordinate(0.0, 10.0)}),
      Label(Location::INTERIOR));

  // Add interior and vertex intersections out of order.
  edge.eiList.add(Coordinate(4.0, 10.0), 2, 6.0);
  edge.eiList.add(Coordinate(10.0, 0.0), 1, 0.0);
  edge.eiList.add(Coordinate(3.0, 0.0), 0, 3.0);
  edge.eiList.add(Coordinate(7.0, 0.0), 0, 7.0);

  std::vector<std::unique_ptr<Edge>> splits = SplitEdgesAndOwn(edge.eiList);
  ASSERT_EQ(splits.size(), 5U);

  // Split 0: (0,0) -> (3,0)
  ASSERT_EQ(splits[0]->getNumPoints(), 2U);
  EXPECT_TRUE(splits[0]->getCoordinate(0).equals2D(Coordinate(0.0, 0.0)));
  EXPECT_TRUE(splits[0]->getCoordinate(1).equals2D(Coordinate(3.0, 0.0)));

  // Split 1: (3,0) -> (7,0)
  ASSERT_EQ(splits[1]->getNumPoints(), 2U);
  EXPECT_TRUE(splits[1]->getCoordinate(0).equals2D(Coordinate(3.0, 0.0)));
  EXPECT_TRUE(splits[1]->getCoordinate(1).equals2D(Coordinate(7.0, 0.0)));

  // Split 2: (7,0) -> (10,0), where ei1 is vertex (seg=1, dist=0).
  ASSERT_EQ(splits[2]->getNumPoints(), 2U);
  EXPECT_TRUE(splits[2]->getCoordinate(0).equals2D(Coordinate(7.0, 0.0)));
  EXPECT_TRUE(splits[2]->getCoordinate(1).equals2D(Coordinate(10.0, 0.0)));

  // Split 3: (10,0) -> (10,10) -> (4,10), preserving intermediate vertex.
  ASSERT_EQ(splits[3]->getNumPoints(), 3U);
  EXPECT_TRUE(splits[3]->getCoordinate(0).equals2D(Coordinate(10.0, 0.0)));
  EXPECT_TRUE(splits[3]->getCoordinate(1).equals2D(Coordinate(10.0, 10.0)));
  EXPECT_TRUE(splits[3]->getCoordinate(2).equals2D(Coordinate(4.0, 10.0)));

  // Split 4: (4,10) -> (0,10)
  ASSERT_EQ(splits[4]->getNumPoints(), 2U);
  EXPECT_TRUE(splits[4]->getCoordinate(0).equals2D(Coordinate(4.0, 10.0)));
  EXPECT_TRUE(splits[4]->getCoordinate(1).equals2D(Coordinate(0.0, 10.0)));
}

TEST(EdgeIntersectionListTest,
     CreateSplitEdgeHandlesZeroDistNonEqualVertexAndPreservesZ) {
  Edge edge(new CoordinateSequence({Coordinate(0.0, 0.0, 1.0),
                                    Coordinate(10.0, 0.0, 2.0),
                                    Coordinate(20.0, 0.0, 3.0)}),
            Label(Location::BOUNDARY));
  EdgeIntersectionList list(&edge);

  // When ei1->dist == 0.0 but ei1->coord differs in 2D from the segment start
  // point, useIntPt1 must be true and append ei1->coord after the segment
  // start.
  EdgeIntersection ei_start(Coordinate(0.0, 0.0, 1.0), 0, 0.0);
  EdgeIntersection ei_offset(Coordinate(10.001, 0.0, 9.0), 1, 0.0);
  std::unique_ptr<Edge> split_offset(
      list.createSplitEdge(&ei_start, &ei_offset));
  ASSERT_EQ(split_offset->getNumPoints(), 3U);
  EXPECT_TRUE(
      split_offset->getCoordinate(0).equals3D(Coordinate(0.0, 0.0, 1.0)));
  EXPECT_TRUE(
      split_offset->getCoordinate(1).equals3D(Coordinate(10.0, 0.0, 2.0)));
  EXPECT_TRUE(
      split_offset->getCoordinate(2).equals3D(Coordinate(10.001, 0.0, 9.0)));

  // When ei1->dist == 0.0 and ei1->coord equals the segment start point in 2D,
  // useIntPt1 is false; ei1->coord replaces the final vertex so its Z is kept.
  EdgeIntersection ei_end_vertex(Coordinate(20.0, 0.0, 99.0), 2, 0.0);
  std::unique_ptr<Edge> split_vertex(
      list.createSplitEdge(&ei_start, &ei_end_vertex));
  ASSERT_EQ(split_vertex->getNumPoints(), 3U);
  EXPECT_TRUE(
      split_vertex->getCoordinate(0).equals3D(Coordinate(0.0, 0.0, 1.0)));
  EXPECT_TRUE(
      split_vertex->getCoordinate(1).equals3D(Coordinate(10.0, 0.0, 2.0)));
  EXPECT_TRUE(
      split_vertex->getCoordinate(2).equals3D(Coordinate(20.0, 0.0, 99.0)));
}

TEST(EdgeIntersectionListTest, PrintAndStreamOperatorSortAndFormatEntries) {
  Edge edge(new CoordinateSequence({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                                    Coordinate(20.0, 0.0)}),
            Label(Location::INTERIOR));
  EdgeIntersectionList list(&edge);
  list.add(Coordinate(15.0, 0.0), 1, 5.0);
  list.add(Coordinate(4.0, 0.0), 0, 4.0);

  std::string text = list.print();
  EXPECT_THAT(text, HasSubstr("Intersections:\n"));
  const std::size_t first_pos = text.find("4 0 seg # = 0 dist = 4");
  const std::size_t second_pos = text.find("15 0 seg # = 1 dist = 5");
  ASSERT_NE(first_pos, std::string::npos);
  ASSERT_NE(second_pos, std::string::npos);
  EXPECT_LT(first_pos, second_pos);

  std::ostringstream oss;
  oss << list;
  EXPECT_EQ(oss.str(), text);
}

}  // namespace
}  // namespace geos_google
