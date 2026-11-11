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

// Unit tests for third_party/geos/src/noding/SegmentNodeList.cpp and
// third_party/geos/include/geos/noding/SegmentNodeList.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/noding/SegmentNodeList.cpp

#include "third_party/geos/include/geos/noding/SegmentNodeList.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentNode.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::noding::NodedSegmentString;
using geos::noding::SegmentNode;
using geos::noding::SegmentNodeList;
using geos::noding::SegmentString;
using ::testing::HasSubstr;

static_assert(!std::is_copy_constructible_v<SegmentNodeList>);
static_assert(!std::is_copy_assignable_v<SegmentNodeList>);

template <typename CoordType>
std::unique_ptr<NodedSegmentString> MakeNodedEdge(
    std::initializer_list<CoordType> coords, bool construct_z = false,
    bool construct_m = false, const void* context = nullptr) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<NodedSegmentString>(seq.release(), construct_z,
                                              construct_m, context);
}

std::vector<std::unique_ptr<SegmentString>> CollectSplitEdges(
    SegmentNodeList& list, bool via_pointer_overload = false) {
  std::vector<SegmentString*> raw_edges;
  absl::Cleanup cleanup = [&raw_edges] {
    for (SegmentString* edge : raw_edges) {
      delete edge;
    }
  };
  if (via_pointer_overload) {
    list.addSplitEdges(&raw_edges);
  } else {
    list.addSplitEdges(raw_edges);
  }
  std::vector<std::unique_ptr<SegmentString>> owned;
  owned.reserve(raw_edges.size());
  for (SegmentString*& edge : raw_edges) {
    owned.emplace_back(edge);
    edge = nullptr;
  }
  return owned;
}

TEST(SegmentNodeListTest, AccessorsReturnParentEdgeAndDimensionFlags) {
  for (bool construct_z : {false, true}) {
    for (bool construct_m : {false, true}) {
      auto edge = MakeNodedEdge<CoordinateXY>(
          {CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)}, construct_z,
          construct_m);
      const SegmentNodeList& list = edge->getNodeList();
      EXPECT_EQ(&list.getEdge(), edge.get());
      EXPECT_EQ(list.getConstructZ(), construct_z);
      EXPECT_EQ(list.getConstructM(), construct_m);
    }
  }
}

TEST(SegmentNodeListTest, EmptyListAndStreamOperatorFormatRawNodes) {
  auto edge = MakeNodedEdge<CoordinateXY>(
      {CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)});
  SegmentNodeList& list = edge->getNodeList();

  std::ostringstream empty_oss;
  std::ostream& ret = (empty_oss << list);
  EXPECT_EQ(&ret, &empty_oss);
  EXPECT_EQ(empty_oss.str(), "Intersections: (0):\n");

  EXPECT_EQ(list.size(), 0u);
  EXPECT_EQ(list.begin(), list.end());
  EXPECT_EQ(std::as_const(list).begin(), std::as_const(list).end());

  // Adding out-of-order and duplicate nodes should appear directly in
  // operator<< before prepare() sorts and deduplicates them.
  list.add(CoordinateXY(8.0, 0.0), 0);
  list.add(CoordinateXY(2.0, 0.0), 0);
  list.add(CoordinateXY(8.0, 0.0), 0);

  std::ostringstream raw_oss;
  raw_oss << list;
  const std::string output = raw_oss.str();
  EXPECT_THAT(output, HasSubstr("Intersections: (3):\n"));
  EXPECT_THAT(output, HasSubstr(" 8 0 seg#=0 octant#=0\n"));
  EXPECT_THAT(output, HasSubstr(" 2 0 seg#=0 octant#=0\n"));
}

TEST(SegmentNodeListTest, PrepareSortsDeduplicatesAndInvalidatesOnAdd) {
  auto edge = MakeNodedEdge<CoordinateXY>({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(10.0, 10.0),
  });
  SegmentNodeList& list = edge->getNodeList();

  list.add(CoordinateXY(10.0, 5.0), 1);
  list.add(CoordinateXY(8.0, 0.0), 0);
  list.add(CoordinateXY(3.0, 0.0), 0);
  list.add(CoordinateXY(8.0, 0.0), 0);
  list.add(CoordinateXY(10.0, 5.0), 1);

  // First size() prepares (sorts and deduplicates); second hits ready==true.
  EXPECT_EQ(list.size(), 3u);
  EXPECT_EQ(list.size(), 3u);

  std::vector<CoordinateXY> sorted;
  for (const SegmentNode& node : std::as_const(list)) {
    sorted.emplace_back(node.coord.x, node.coord.y);
  }
  ASSERT_EQ(sorted.size(), 3u);
  EXPECT_EQ(sorted[0], CoordinateXY(3.0, 0.0));
  EXPECT_EQ(sorted[1], CoordinateXY(8.0, 0.0));
  EXPECT_EQ(sorted[2], CoordinateXY(10.0, 5.0));

  // Subsequent add() resets ready flag so non-const begin()/end() re-prepare.
  list.add(CoordinateXY(1.0, 0.0), 0);
  list.add(CoordinateXY(3.0, 0.0), 0);
  ASSERT_EQ(list.size(), 4u);
  auto it = list.begin();
  EXPECT_DOUBLE_EQ(it->coord.x, 1.0);
  ++it;
  EXPECT_DOUBLE_EQ(it->coord.x, 3.0);
  EXPECT_NE(it, list.end());
}

TEST(SegmentNodeListTest, AddEndpointsPreservesStartAndEndWithZM) {
  auto edge = MakeNodedEdge<CoordinateXYZM>(
      {
          CoordinateXYZM(1.0, 2.0, 10.0, 100.0),
          CoordinateXYZM(3.0, 4.0, 20.0, 200.0),
          CoordinateXYZM(5.0, 6.0, 30.0, 300.0),
      },
      /*construct_z=*/true, /*construct_m=*/true);
  SegmentNodeList& list = edge->getNodeList();

  list.addEndpoints();
  ASSERT_EQ(list.size(), 2u);

  auto it = list.begin();
  EXPECT_EQ(it->segmentIndex, 0u);
  EXPECT_FALSE(it->isInterior());
  EXPECT_DOUBLE_EQ(it->coord.x, 1.0);
  EXPECT_DOUBLE_EQ(it->coord.y, 2.0);
  EXPECT_DOUBLE_EQ(it->coord.z, 10.0);
  EXPECT_DOUBLE_EQ(it->coord.m, 100.0);

  ++it;
  EXPECT_EQ(it->segmentIndex, 2u);
  EXPECT_FALSE(it->isInterior());
  EXPECT_DOUBLE_EQ(it->coord.x, 5.0);
  EXPECT_DOUBLE_EQ(it->coord.y, 6.0);
  EXPECT_DOUBLE_EQ(it->coord.z, 30.0);
  EXPECT_DOUBLE_EQ(it->coord.m, 300.0);
}

TEST(SegmentNodeListTest, AddSplitEdgesWithNoInteriorNodesAndPointerOverload) {
  const int context_tag = 7;
  auto edge = MakeNodedEdge<CoordinateXY>(
      {
          CoordinateXY(0.0, 0.0),
          CoordinateXY(10.0, 0.0),
          CoordinateXY(10.0, 10.0),
      },
      /*construct_z=*/false, /*construct_m=*/false, &context_tag);

  const std::vector<std::unique_ptr<SegmentString>> split_edges =
      CollectSplitEdges(edge->getNodeList(), /*via_pointer_overload=*/true);
  ASSERT_EQ(split_edges.size(), 1u);
  EXPECT_EQ(split_edges[0]->getData(), &context_tag);
  ASSERT_EQ(split_edges[0]->size(), 3u);
  EXPECT_EQ(split_edges[0]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(0.0, 0.0));
  EXPECT_EQ(split_edges[0]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(10.0, 0.0));
  EXPECT_EQ(split_edges[0]->getCoordinate<CoordinateXY>(2),
            CoordinateXY(10.0, 10.0));
}

TEST(SegmentNodeListTest, AddSplitEdgesSplitsWithinAndAcrossSegments) {
  const int context_tag = 123;
  auto edge = MakeNodedEdge<CoordinateXY>(
      {
          CoordinateXY(0.0, 0.0),
          CoordinateXY(10.0, 0.0),
          CoordinateXY(20.0, 0.0),
          CoordinateXY(20.0, 10.0),
      },
      /*construct_z=*/false, /*construct_m=*/false, &context_tag);
  SegmentNodeList& list = edge->getNodeList();

  // Add out-of-order interior nodes on seg 0, a vertex node at seg 1,
  // duplicates, and an interior node on seg 2.
  list.add(CoordinateXY(20.0, 5.0), 2);
  list.add(CoordinateXY(8.0, 0.0), 0);
  list.add(CoordinateXY(10.0, 0.0), 1);
  list.add(CoordinateXY(4.0, 0.0), 0);
  list.add(CoordinateXY(8.0, 0.0), 0);

  const std::vector<std::unique_ptr<SegmentString>> split_edges =
      CollectSplitEdges(list);
  ASSERT_EQ(split_edges.size(), 5u);
  for (const auto& split : split_edges) {
    EXPECT_EQ(split->getData(), &context_tag);
  }

  // Edge 0: (0,0) -> (4,0) [twoPoints == true]
  ASSERT_EQ(split_edges[0]->size(), 2u);
  EXPECT_EQ(split_edges[0]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(0.0, 0.0));
  EXPECT_EQ(split_edges[0]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(4.0, 0.0));

  // Edge 1: (4,0) -> (8,0) [twoPoints == true]
  ASSERT_EQ(split_edges[1]->size(), 2u);
  EXPECT_EQ(split_edges[1]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(4.0, 0.0));
  EXPECT_EQ(split_edges[1]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(8.0, 0.0));

  // Edge 2: (8,0) -> (10,0) [twoPoints == false, useIntPt1 == false]
  ASSERT_EQ(split_edges[2]->size(), 2u);
  EXPECT_EQ(split_edges[2]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(8.0, 0.0));
  EXPECT_EQ(split_edges[2]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(10.0, 0.0));

  // Edge 3: (10,0) -> (20,0) -> (20,5) [twoPoints == false, useIntPt1 == true]
  ASSERT_EQ(split_edges[3]->size(), 3u);
  EXPECT_EQ(split_edges[3]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(10.0, 0.0));
  EXPECT_EQ(split_edges[3]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(20.0, 0.0));
  EXPECT_EQ(split_edges[3]->getCoordinate<CoordinateXY>(2),
            CoordinateXY(20.0, 5.0));

  // Edge 4: (20,5) -> (20,10) [twoPoints == false, useIntPt1 == false]
  ASSERT_EQ(split_edges[4]->size(), 2u);
  EXPECT_EQ(split_edges[4]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(20.0, 5.0));
  EXPECT_EQ(split_edges[4]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(20.0, 10.0));
}

TEST(SegmentNodeListTest, AddSplitEdgesPropagatesConstructZAndConstructM) {
  const std::initializer_list<CoordinateXYZM> coords = {
      CoordinateXYZM(0.0, 0.0, 10.0, 100.0),
      CoordinateXYZM(10.0, 0.0, 20.0, 200.0),
      CoordinateXYZM(20.0, 0.0, 30.0, 300.0),
  };

  // Case 1: XY only (constructZ=false, constructM=false).
  {
    auto edge = MakeNodedEdge(coords, false, false);
    edge->getNodeList().add(CoordinateXYZM(5.0, 0.0, 15.0, 150.0), 0);
    auto splits = CollectSplitEdges(edge->getNodeList());
    ASSERT_EQ(splits.size(), 2u);
    EXPECT_FALSE(splits[0]->getCoordinates()->hasZ());
    EXPECT_FALSE(splits[0]->getCoordinates()->hasM());
    EXPECT_TRUE(std::isnan(
        splits[0]->getCoordinates()->getOrdinate(1, CoordinateSequence::Z)));
    EXPECT_TRUE(std::isnan(
        splits[1]->getCoordinates()->getOrdinate(1, CoordinateSequence::M)));
  }

  // Case 2: XYZ (constructZ=true, constructM=false).
  {
    auto edge = MakeNodedEdge(coords, true, false);
    edge->getNodeList().add(Coordinate(5.0, 0.0, 15.0), 0);
    auto splits = CollectSplitEdges(edge->getNodeList());
    ASSERT_EQ(splits.size(), 2u);
    EXPECT_TRUE(splits[0]->getCoordinates()->hasZ());
    EXPECT_FALSE(splits[0]->getCoordinates()->hasM());
    EXPECT_DOUBLE_EQ(splits[0]->getCoordinate(0).z, 10.0);
    EXPECT_DOUBLE_EQ(splits[0]->getCoordinate(1).z, 15.0);
    EXPECT_DOUBLE_EQ(splits[1]->getCoordinate(0).z, 15.0);
    EXPECT_DOUBLE_EQ(splits[1]->getCoordinate(1).z, 20.0);
    EXPECT_DOUBLE_EQ(splits[1]->getCoordinate(2).z, 30.0);
  }

  // Case 3: XYM (constructZ=false, constructM=true).
  {
    auto edge = MakeNodedEdge(coords, false, true);
    edge->getNodeList().add(CoordinateXYM(5.0, 0.0, 150.0), 0);
    auto splits = CollectSplitEdges(edge->getNodeList());
    ASSERT_EQ(splits.size(), 2u);
    EXPECT_FALSE(splits[0]->getCoordinates()->hasZ());
    EXPECT_TRUE(splits[0]->getCoordinates()->hasM());
    CoordinateXYM m0, m1;
    splits[0]->getCoordinates()->getAt(0, m0);
    splits[0]->getCoordinates()->getAt(1, m1);
    EXPECT_DOUBLE_EQ(m0.m, 100.0);
    EXPECT_DOUBLE_EQ(m1.m, 150.0);
    EXPECT_DOUBLE_EQ(
        splits[1]->getCoordinates()->getOrdinate(1, CoordinateSequence::M),
        200.0);
    EXPECT_DOUBLE_EQ(
        splits[1]->getCoordinates()->getOrdinate(2, CoordinateSequence::M),
        300.0);
  }

  // Case 4: XYZM (constructZ=true, constructM=true).
  {
    auto edge = MakeNodedEdge(coords, true, true);
    edge->getNodeList().add(CoordinateXYZM(5.0, 0.0, 15.0, 150.0), 0);
    auto splits = CollectSplitEdges(edge->getNodeList());
    ASSERT_EQ(splits.size(), 2u);
    EXPECT_TRUE(splits[0]->getCoordinates()->hasZ());
    EXPECT_TRUE(splits[0]->getCoordinates()->hasM());
    EXPECT_DOUBLE_EQ(splits[0]->getCoordinate<CoordinateXYZM>(1).z, 15.0);
    EXPECT_DOUBLE_EQ(splits[0]->getCoordinate<CoordinateXYZM>(1).m, 150.0);
    EXPECT_DOUBLE_EQ(splits[1]->getCoordinate<CoordinateXYZM>(2).z, 30.0);
    EXPECT_DOUBLE_EQ(splits[1]->getCoordinate<CoordinateXYZM>(2).m, 300.0);
  }
}

TEST(SegmentNodeListTest, AddSplitEdgesNodesExistingVertexCollapses) {
  auto edge = MakeNodedEdge<CoordinateXY>({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(5.0, 0.0),
      CoordinateXY(0.0, 0.0),
      CoordinateXY(5.0, 0.0),
  });

  const std::vector<std::unique_ptr<SegmentString>> split_edges =
      CollectSplitEdges(edge->getNodeList());
  ASSERT_EQ(split_edges.size(), 3u);

  ASSERT_EQ(split_edges[0]->size(), 2u);
  EXPECT_EQ(split_edges[0]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(0.0, 0.0));
  EXPECT_EQ(split_edges[0]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(5.0, 0.0));

  ASSERT_EQ(split_edges[1]->size(), 2u);
  EXPECT_EQ(split_edges[1]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(5.0, 0.0));
  EXPECT_EQ(split_edges[1]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(0.0, 0.0));

  ASSERT_EQ(split_edges[2]->size(), 2u);
  EXPECT_EQ(split_edges[2]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(0.0, 0.0));
  EXPECT_EQ(split_edges[2]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(5.0, 0.0));
}

TEST(SegmentNodeListTest,
     AddSplitEdgesNodesInsertedCollapseBothInteriorAndEnd) {
  // Collapse where the second equal node is interior on the adjacent segment.
  {
    auto edge = MakeNodedEdge<CoordinateXY>({
        CoordinateXY(0.0, 0.0),
        CoordinateXY(10.0, 0.0),
        CoordinateXY(-5.0, 0.0),
    });
    SegmentNodeList& list = edge->getNodeList();
    list.add(CoordinateXY(5.0, 0.0), 0);
    list.add(CoordinateXY(5.0, 0.0), 1);

    const std::vector<std::unique_ptr<SegmentString>> split_edges =
        CollectSplitEdges(list);
    ASSERT_EQ(split_edges.size(), 4u);
    EXPECT_EQ(split_edges[0]->getCoordinate<CoordinateXY>(1),
              CoordinateXY(5.0, 0.0));
    EXPECT_EQ(split_edges[1]->getCoordinate<CoordinateXY>(0),
              CoordinateXY(5.0, 0.0));
    EXPECT_EQ(split_edges[1]->getCoordinate<CoordinateXY>(1),
              CoordinateXY(10.0, 0.0));
    EXPECT_EQ(split_edges[2]->getCoordinate<CoordinateXY>(0),
              CoordinateXY(10.0, 0.0));
    EXPECT_EQ(split_edges[2]->getCoordinate<CoordinateXY>(1),
              CoordinateXY(5.0, 0.0));
    EXPECT_EQ(split_edges[3]->getCoordinate<CoordinateXY>(0),
              CoordinateXY(5.0, 0.0));
    EXPECT_EQ(split_edges[3]->getCoordinate<CoordinateXY>(1),
              CoordinateXY(-5.0, 0.0));
  }

  // Collapse where the second equal node is a vertex node (not interior),
  // exercising the numVerticesBetween decrement in findCollapseIndex().
  {
    auto edge = MakeNodedEdge<CoordinateXY>({
        CoordinateXY(0.0, 0.0),
        CoordinateXY(10.0, 0.0),
        CoordinateXY(5.0, 0.0),
        CoordinateXY(20.0, 0.0),
    });
    SegmentNodeList& list = edge->getNodeList();
    list.add(CoordinateXY(5.0, 0.0), 0);
    list.add(CoordinateXY(5.0, 0.0), 2);

    const std::vector<std::unique_ptr<SegmentString>> split_edges =
        CollectSplitEdges(list);
    ASSERT_EQ(split_edges.size(), 4u);
    EXPECT_EQ(split_edges[1]->getCoordinate<CoordinateXY>(1),
              CoordinateXY(10.0, 0.0));
    EXPECT_EQ(split_edges[2]->getCoordinate<CoordinateXY>(1),
              CoordinateXY(5.0, 0.0));
    EXPECT_EQ(split_edges[3]->getCoordinate<CoordinateXY>(1),
              CoordinateXY(20.0, 0.0));
  }
}

TEST(SegmentNodeListTest, AddSplitEdgesIgnoresNonCollapsedMultiSegmentReturns) {
  // Closed triangle: endpoints share (0,0) across 3 segments (2 vertices
  // between), so it must not be treated as a collapsed edge pair.
  {
    auto edge = MakeNodedEdge<CoordinateXY>({
        CoordinateXY(0.0, 0.0),
        CoordinateXY(10.0, 0.0),
        CoordinateXY(0.0, 10.0),
        CoordinateXY(0.0, 0.0),
    });
    const std::vector<std::unique_ptr<SegmentString>> split_edges =
        CollectSplitEdges(edge->getNodeList());
    ASSERT_EQ(split_edges.size(), 1u);
    EXPECT_EQ(split_edges[0]->size(), 4u);
  }

  // Self-returning edge across two intermediate vertices (numVerticesBetween=2)
  // between interior nodes at (5,0) on segments 0 and 2.
  {
    auto edge = MakeNodedEdge<CoordinateXY>({
        CoordinateXY(0.0, 0.0),
        CoordinateXY(10.0, 0.0),
        CoordinateXY(10.0, 10.0),
        CoordinateXY(-5.0, 0.0),
    });
    SegmentNodeList& list = edge->getNodeList();
    list.add(CoordinateXY(5.0, 0.0), 0);
    list.add(CoordinateXY(5.0, 0.0), 2);

    const std::vector<std::unique_ptr<SegmentString>> split_edges =
        CollectSplitEdges(list);
    ASSERT_EQ(split_edges.size(), 3u);
    EXPECT_EQ(split_edges[1]->size(), 4u);
  }
}

TEST(SegmentNodeListTest, AddSplitEdgesHandlesSingleAndTwoPointEdges) {
  // Single-point edge exercises edge.size() < 2 guard and produces zero splits.
  {
    auto edge = MakeNodedEdge<CoordinateXY>({CoordinateXY(3.0, 4.0)});
    const std::vector<std::unique_ptr<SegmentString>> split_edges =
        CollectSplitEdges(edge->getNodeList());
    EXPECT_TRUE(split_edges.empty());
  }

  // Two-point edge exercises edge.size() == 2 loop bound in collapse search.
  {
    auto edge = MakeNodedEdge<CoordinateXY>({
        CoordinateXY(0.0, 0.0),
        CoordinateXY(10.0, 0.0),
    });
    const std::vector<std::unique_ptr<SegmentString>> split_edges =
        CollectSplitEdges(edge->getNodeList());
    ASSERT_EQ(split_edges.size(), 1u);
    ASSERT_EQ(split_edges[0]->size(), 2u);
  }
}

TEST(SegmentNodeListTest, GetSplitCoordinatesMergesVerticesNodesAndDimensions) {
  // Multi-segment XYZM edge with out-of-order and duplicate interior nodes.
  {
    auto edge = MakeNodedEdge<CoordinateXYZM>(
        {
            CoordinateXYZM(0.0, 0.0, 1.0, 10.0),
            CoordinateXYZM(10.0, 0.0, 2.0, 20.0),
            CoordinateXYZM(10.0, 10.0, 3.0, 30.0),
        },
        /*construct_z=*/true, /*construct_m=*/true);
    SegmentNodeList& list = edge->getNodeList();
    list.add(CoordinateXYZM(10.0, 5.0, 2.5, 25.0), 1);
    list.add(CoordinateXYZM(5.0, 0.0, 1.5, 15.0), 0);
    list.add(CoordinateXYZM(5.0, 0.0, 1.5, 15.0), 0);

    std::unique_ptr<CoordinateSequence> coords = list.getSplitCoordinates();
    ASSERT_NE(coords, nullptr);
    EXPECT_TRUE(coords->hasZ());
    EXPECT_TRUE(coords->hasM());
    EXPECT_FALSE(coords->hasRepeatedPoints());
    ASSERT_EQ(coords->size(), 5u);

    const CoordinateXYZM expected[] = {
        CoordinateXYZM(0.0, 0.0, 1.0, 10.0),
        CoordinateXYZM(5.0, 0.0, 1.5, 15.0),
        CoordinateXYZM(10.0, 0.0, 2.0, 20.0),
        CoordinateXYZM(10.0, 5.0, 2.5, 25.0),
        CoordinateXYZM(10.0, 10.0, 3.0, 30.0),
    };
    for (std::size_t i = 0; i < coords->size(); ++i) {
      const auto& got = coords->getAt<CoordinateXYZM>(i);
      EXPECT_TRUE(got.equals4D(expected[i])) << "at index " << i;
    }
  }

  // Unnoded edge returns original vertices in sequence.
  {
    auto edge = MakeNodedEdge<CoordinateXY>({
        CoordinateXY(0.0, 0.0),
        CoordinateXY(4.0, 3.0),
        CoordinateXY(8.0, 0.0),
    });
    std::unique_ptr<CoordinateSequence> coords =
        edge->getNodeList().getSplitCoordinates();
    ASSERT_EQ(coords->size(), 3u);
    EXPECT_EQ(coords->getAt<CoordinateXY>(0), CoordinateXY(0.0, 0.0));
    EXPECT_EQ(coords->getAt<CoordinateXY>(1), CoordinateXY(4.0, 3.0));
    EXPECT_EQ(coords->getAt<CoordinateXY>(2), CoordinateXY(8.0, 0.0));
  }

  // Single-point edge produces a 1-coordinate sequence without repeats.
  {
    auto edge = MakeNodedEdge<CoordinateXY>({CoordinateXY(7.0, 9.0)});
    std::unique_ptr<CoordinateSequence> coords =
        edge->getNodeList().getSplitCoordinates();
    ASSERT_EQ(coords->size(), 1u);
    EXPECT_EQ(coords->getAt<CoordinateXY>(0), CoordinateXY(7.0, 9.0));
  }
}

TEST(SegmentNodeListTest, ConstIterationPreparesLazilyAndAddSplitEdgesAppends) {
  auto edge1 = MakeNodedEdge<CoordinateXY>({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
  });
  auto edge2 = MakeNodedEdge<CoordinateXY>({
      CoordinateXY(20.0, 10.0),
      CoordinateXY(20.0, 0.0),
      CoordinateXY(10.0, 0.0),
  });

  SegmentNodeList& list2 = edge2->getNodeList();
  list2.add(CoordinateXY(20.0, 2.0), 0);
  list2.add(CoordinateXY(20.0, 8.0), 0);
  list2.add(CoordinateXY(20.0, 2.0), 0);

  // Iterate via const begin()/end() before calling size() or non-const begin().
  const SegmentNodeList& const_list2 = list2;
  std::vector<CoordinateXY> nodes;
  for (auto it = const_list2.begin(); it != const_list2.end(); ++it) {
    nodes.emplace_back(it->coord.x, it->coord.y);
  }
  ASSERT_EQ(nodes.size(), 2u);
  EXPECT_EQ(nodes[0], CoordinateXY(20.0, 8.0));
  EXPECT_EQ(nodes[1], CoordinateXY(20.0, 2.0));

  // Accumulate split edges from multiple edges into the same output vector.
  std::vector<SegmentString*> combined;
  absl::Cleanup cleanup = [&combined] {
    for (SegmentString* s : combined) {
      delete s;
    }
  };
  edge1->getNodeList().addSplitEdges(combined);
  ASSERT_EQ(combined.size(), 1u);
  list2.addSplitEdges(combined);
  ASSERT_EQ(combined.size(), 4u);

  EXPECT_EQ(combined[0]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(0.0, 0.0));
  EXPECT_EQ(combined[0]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(10.0, 0.0));

  ASSERT_EQ(combined[1]->size(), 2u);
  EXPECT_EQ(combined[1]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(20.0, 10.0));
  EXPECT_EQ(combined[1]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(20.0, 8.0));

  ASSERT_EQ(combined[2]->size(), 2u);
  EXPECT_EQ(combined[2]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(20.0, 8.0));
  EXPECT_EQ(combined[2]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(20.0, 2.0));

  ASSERT_EQ(combined[3]->size(), 3u);
  EXPECT_EQ(combined[3]->getCoordinate<CoordinateXY>(0),
            CoordinateXY(20.0, 2.0));
  EXPECT_EQ(combined[3]->getCoordinate<CoordinateXY>(1),
            CoordinateXY(20.0, 0.0));
  EXPECT_EQ(combined[3]->getCoordinate<CoordinateXY>(2),
            CoordinateXY(10.0, 0.0));
}

TEST(SegmentNodeListTest, AddSplitEdgesThreeVertexSpikeAndRepeatedCalls) {
  auto edge = MakeNodedEdge<CoordinateXY>({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(5.0, 0.0),
      CoordinateXY(0.0, 0.0),
  });
  SegmentNodeList& list = edge->getNodeList();

  const std::vector<std::unique_ptr<SegmentString>> first_pass =
      CollectSplitEdges(list);
  const std::vector<std::unique_ptr<SegmentString>> second_pass =
      CollectSplitEdges(list);

  for (const auto* splits : {&first_pass, &second_pass}) {
    ASSERT_EQ(splits->size(), 2u);
    ASSERT_EQ((*splits)[0]->size(), 2u);
    EXPECT_EQ((*splits)[0]->getCoordinate<CoordinateXY>(0),
              CoordinateXY(0.0, 0.0));
    EXPECT_EQ((*splits)[0]->getCoordinate<CoordinateXY>(1),
              CoordinateXY(5.0, 0.0));
    ASSERT_EQ((*splits)[1]->size(), 2u);
    EXPECT_EQ((*splits)[1]->getCoordinate<CoordinateXY>(0),
              CoordinateXY(5.0, 0.0));
    EXPECT_EQ((*splits)[1]->getCoordinate<CoordinateXY>(1),
              CoordinateXY(0.0, 0.0));
  }
}

template <typename Tag, typename Tag::type M>
struct AccessSegmentNodeListPrivate {
  friend typename Tag::type GetSegmentNodeListPrivate(Tag) { return M; }
};

struct CheckSplitEdgesTag {
  using type =
      void (SegmentNodeList::*)(const std::vector<SegmentString*>&) const;
  friend type GetSegmentNodeListPrivate(CheckSplitEdgesTag);
};

template struct AccessSegmentNodeListPrivate<
    CheckSplitEdgesTag, &SegmentNodeList::checkSplitEdgesCorrectness>;

TEST(SegmentNodeListTest,
     CheckSplitEdgesCorrectnessCoversEmptyValidAndMismatch) {
  auto edge = MakeNodedEdge<CoordinateXY>(
      {CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)});
  SegmentNodeList& list = edge->getNodeList();
  auto check_fn = GetSegmentNodeListPrivate(CheckSplitEdgesTag{});

  std::vector<SegmentString*> empty_splits;
  EXPECT_NO_THROW((list.*check_fn)(empty_splits));

  std::vector<std::unique_ptr<SegmentString>> valid_splits =
      CollectSplitEdges(list);
  std::vector<SegmentString*> valid_ptrs;
  for (const auto& s : valid_splits) {
    valid_ptrs.push_back(s.get());
  }
  EXPECT_NO_THROW((list.*check_fn)(valid_ptrs));

  auto bad_start = MakeNodedEdge<CoordinateXY>(
      {CoordinateXY(1.0, 0.0), CoordinateXY(10.0, 0.0)});
  std::vector<SegmentString*> bad_start_ptrs{bad_start.get()};
  EXPECT_THROW((list.*check_fn)(bad_start_ptrs), geos::util::GEOSException);

  auto bad_end = MakeNodedEdge<CoordinateXY>(
      {CoordinateXY(0.0, 0.0), CoordinateXY(9.0, 0.0)});
  std::vector<SegmentString*> bad_end_ptrs{bad_end.get()};
  EXPECT_THROW((list.*check_fn)(bad_end_ptrs), geos::util::GEOSException);
}

}  // namespace
}  // namespace geos_google
