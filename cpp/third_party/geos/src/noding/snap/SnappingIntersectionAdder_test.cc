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

#include "third_party/geos/include/geos/noding/snap/SnappingIntersectionAdder.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentIntersector.h"
#include "third_party/geos/include/geos/noding/SegmentNode.h"
#include "third_party/geos/include/geos/noding/SegmentNodeList.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/noding/snap/SnappingPointIndex.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::noding::NodedSegmentString;
using geos::noding::SegmentIntersector;
using geos::noding::SegmentNode;
using geos::noding::SegmentNodeList;
using geos::noding::snap::SnappingIntersectionAdder;
using geos::noding::snap::SnappingPointIndex;

std::unique_ptr<NodedSegmentString> MakeSegmentString(
    std::initializer_list<Coordinate> coords, const void* context = nullptr) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<NodedSegmentString>(seq.release(), false, false,
                                              context);
}

std::vector<SegmentNode> CollectNodes(const NodedSegmentString& ss) {
  std::vector<SegmentNode> nodes;
  const SegmentNodeList& node_list = ss.getNodeList();
  for (const SegmentNode& node : node_list) {
    nodes.push_back(node);
  }
  return nodes;
}

TEST(SnappingIntersectionAdderTest, IsDoneAlwaysReturnsFalse) {
  SnappingPointIndex index(0.1);
  SnappingIntersectionAdder adder(0.1, index);
  const SegmentIntersector& intersector = adder;
  EXPECT_FALSE(adder.isDone());
  EXPECT_FALSE(intersector.isDone());
}

TEST(SnappingIntersectionAdderTest, SkipsIdenticalSegmentOnSameSegmentString) {
  SnappingPointIndex index(0.1);
  SnappingIntersectionAdder adder(0.1, index);
  auto ss = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}, {20.0, 0.0}});

  adder.processIntersections(ss.get(), 0, ss.get(), 0);
  adder.processIntersections(ss.get(), 1, ss.get(), 1);
  EXPECT_EQ(ss->getNodeList().size(), 0u);
}

TEST(SnappingIntersectionAdderTest, CrossingSegmentsAddIntersectionToIndex) {
  SnappingPointIndex index(0.1);
  SnappingIntersectionAdder adder(0.1, index);
  auto ss0 = MakeSegmentString({{0.0, 0.0}, {10.0, 10.0}});
  auto ss1 = MakeSegmentString({{0.0, 10.0}, {10.0, 0.0}});

  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

  const std::vector<SegmentNode> nodes0 = CollectNodes(*ss0);
  const std::vector<SegmentNode> nodes1 = CollectNodes(*ss1);
  ASSERT_EQ(nodes0.size(), 1u);
  ASSERT_EQ(nodes1.size(), 1u);

  EXPECT_DOUBLE_EQ(nodes0[0].coord.x, 5.0);
  EXPECT_DOUBLE_EQ(nodes0[0].coord.y, 5.0);
  EXPECT_EQ(nodes0[0].segmentIndex, 0u);
  EXPECT_TRUE(nodes0[0].isInterior());

  EXPECT_DOUBLE_EQ(nodes1[0].coord.x, 5.0);
  EXPECT_DOUBLE_EQ(nodes1[0].coord.y, 5.0);
  EXPECT_EQ(nodes1[0].segmentIndex, 0u);
  EXPECT_TRUE(nodes1[0].isInterior());

  // Confirm the computed intersection point was recorded in SnappingPointIndex.
  const Coordinate& snapped_query = index.snap(Coordinate(5.05, 5.0));
  EXPECT_DOUBLE_EQ(snapped_query.x, 5.0);
  EXPECT_DOUBLE_EQ(snapped_query.y, 5.0);
}

TEST(SnappingIntersectionAdderTest, SnapsCrossingToPreExistingSnapPoint) {
  SnappingPointIndex index(0.1);
  const Coordinate& seeded = index.snap(Coordinate(5.05, 5.02));
  SnappingIntersectionAdder adder(0.1, index);

  auto ss0 = MakeSegmentString({{0.0, 0.0}, {10.0, 10.0}});
  auto ss1 = MakeSegmentString({{0.0, 10.0}, {10.0, 0.0}});

  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

  const std::vector<SegmentNode> nodes0 = CollectNodes(*ss0);
  const std::vector<SegmentNode> nodes1 = CollectNodes(*ss1);
  ASSERT_EQ(nodes0.size(), 1u);
  ASSERT_EQ(nodes1.size(), 1u);
  EXPECT_TRUE(nodes0[0].coord.equals2D(seeded));
  EXPECT_TRUE(nodes1[0].coord.equals2D(seeded));
}

TEST(SnappingIntersectionAdderTest, DisjointSegmentsAddNoNodes) {
  SnappingPointIndex index(0.5);
  SnappingIntersectionAdder adder(0.5, index);
  auto ss0 = MakeSegmentString({{0.0, 0.0}, {4.0, 0.0}});
  auto ss1 = MakeSegmentString({{6.0, 0.0}, {10.0, 0.0}});

  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);
  EXPECT_EQ(ss0->getNodeList().size(), 0u);
  EXPECT_EQ(ss1->getNodeList().size(), 0u);
}

TEST(SnappingIntersectionAdderTest, CollinearOverlapNodesInteriorEndpoints) {
  SnappingPointIndex index(0.1);
  SnappingIntersectionAdder adder(0.1, index);
  auto ss0 = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
  auto ss1 = MakeSegmentString({{3.0, 0.0}, {7.0, 0.0}});

  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

  const std::vector<SegmentNode> nodes0 = CollectNodes(*ss0);
  ASSERT_EQ(nodes0.size(), 2u);
  EXPECT_DOUBLE_EQ(nodes0[0].coord.x, 3.0);
  EXPECT_DOUBLE_EQ(nodes0[0].coord.y, 0.0);
  EXPECT_EQ(nodes0[0].segmentIndex, 0u);
  EXPECT_DOUBLE_EQ(nodes0[1].coord.x, 7.0);
  EXPECT_DOUBLE_EQ(nodes0[1].coord.y, 0.0);
  EXPECT_EQ(nodes0[1].segmentIndex, 0u);

  const std::vector<SegmentNode> nodes1 = CollectNodes(*ss1);
  ASSERT_EQ(nodes1.size(), 2u);
  EXPECT_DOUBLE_EQ(nodes1[0].coord.x, 3.0);
  EXPECT_EQ(nodes1[0].segmentIndex, 0u);
  EXPECT_FALSE(nodes1[0].isInterior());
  EXPECT_DOUBLE_EQ(nodes1[1].coord.x, 7.0);
  EXPECT_EQ(nodes1[1].segmentIndex, 1u);
  EXPECT_FALSE(nodes1[1].isInterior());
}

TEST(SnappingIntersectionAdderTest, NearVertexSkipsEndpointsAndExactBoundary) {
  SnappingPointIndex index(0.1);
  SnappingIntersectionAdder adder(0.1, index);
  auto base = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});

  // Candidate vertex within snapTolerance of target start point p0 is skipped.
  auto near_start = MakeSegmentString({{0.05, 0.05}, {10.0, 10.0}});
  adder.processIntersections(base.get(), 0, near_start.get(), 0);
  EXPECT_EQ(base->getNodeList().size(), 0u);
  EXPECT_EQ(near_start->getNodeList().size(), 0u);

  // Candidate vertex within snapTolerance of target end point p1 is skipped.
  auto near_end = MakeSegmentString({{9.95, 0.05}, {10.0, 10.0}});
  adder.processIntersections(base.get(), 0, near_end.get(), 0);
  EXPECT_EQ(base->getNodeList().size(), 0u);
  EXPECT_EQ(near_end->getNodeList().size(), 0u);

  // Candidate vertex at distance == snapTolerance (0.1) is NOT strictly <.
  auto on_boundary = MakeSegmentString({{5.0, 0.1}, {5.0, 10.0}});
  adder.processIntersections(base.get(), 0, on_boundary.get(), 0);
  EXPECT_EQ(base->getNodeList().size(), 0u);
  EXPECT_EQ(on_boundary->getNodeList().size(), 0u);
}

TEST(SnappingIntersectionAdderTest,
     NearVertexInteriorAddsNodesForAllEndpoints) {
  // Check both p10/p11 near seg0 and p00/p01 near seg1.
  {
    SnappingPointIndex index(0.1);
    SnappingIntersectionAdder adder(0.1, index);
    auto target = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
    auto from_start = MakeSegmentString({{3.0, 0.05}, {3.0, 10.0}});
    auto from_end = MakeSegmentString({{7.0, 10.0}, {7.0, 0.05}});

    adder.processIntersections(target.get(), 0, from_start.get(), 0);
    adder.processIntersections(target.get(), 0, from_end.get(), 0);

    const std::vector<SegmentNode> target_nodes = CollectNodes(*target);
    ASSERT_EQ(target_nodes.size(), 2u);
    EXPECT_DOUBLE_EQ(target_nodes[0].coord.x, 3.0);
    EXPECT_DOUBLE_EQ(target_nodes[0].coord.y, 0.05);
    EXPECT_TRUE(target_nodes[0].isInterior());
    EXPECT_DOUBLE_EQ(target_nodes[1].coord.x, 7.0);
    EXPECT_DOUBLE_EQ(target_nodes[1].coord.y, 0.05);
    EXPECT_TRUE(target_nodes[1].isInterior());

    const std::vector<SegmentNode> start_nodes = CollectNodes(*from_start);
    ASSERT_EQ(start_nodes.size(), 1u);
    EXPECT_EQ(start_nodes[0].segmentIndex, 0u);
    EXPECT_DOUBLE_EQ(start_nodes[0].coord.y, 0.05);
    EXPECT_FALSE(start_nodes[0].isInterior());

    const std::vector<SegmentNode> end_nodes = CollectNodes(*from_end);
    ASSERT_EQ(end_nodes.size(), 1u);
    EXPECT_EQ(end_nodes[0].segmentIndex, 1u);
    EXPECT_DOUBLE_EQ(end_nodes[0].coord.y, 0.05);
    EXPECT_FALSE(end_nodes[0].isInterior());
  }

  {
    SnappingPointIndex index(0.1);
    SnappingIntersectionAdder adder(0.1, index);
    auto seg0_start_near = MakeSegmentString({{4.0, 0.04}, {4.0, 10.0}});
    auto seg0_end_near = MakeSegmentString({{6.0, 10.0}, {6.0, 0.04}});
    auto target = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});

    adder.processIntersections(seg0_start_near.get(), 0, target.get(), 0);
    adder.processIntersections(seg0_end_near.get(), 0, target.get(), 0);

    const std::vector<SegmentNode> target_nodes = CollectNodes(*target);
    ASSERT_EQ(target_nodes.size(), 2u);
    EXPECT_DOUBLE_EQ(target_nodes[0].coord.x, 4.0);
    EXPECT_DOUBLE_EQ(target_nodes[1].coord.x, 6.0);
    EXPECT_EQ(seg0_start_near->getNodeList().size(), 1u);
    EXPECT_EQ(seg0_end_near->getNodeList().size(), 1u);
  }
}

TEST(SnappingIntersectionAdderTest,
     AdjacentSegmentsSkipIntersectionButCheckNear) {
  SnappingPointIndex index(0.1);
  SnappingIntersectionAdder adder(0.1, index);

  // Adjacent segments share (10, 0); segment 1 bends sharply back so (5, 0.05)
  // lies within snapTolerance of segment 0.
  auto ss =
      MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}, {5.0, 0.05}, {5.0, 10.0}});

  adder.processIntersections(ss.get(), 0, ss.get(), 1);
  adder.processIntersections(ss.get(), 1, ss.get(), 0);

  const std::vector<SegmentNode> nodes = CollectNodes(*ss);
  // Shared vertex (10, 0) must NOT be added as a node; (5, 0.05) is added on
  // segment 0 and normalized to vertex index 2 on segment 1.
  ASSERT_EQ(nodes.size(), 2u);
  EXPECT_EQ(nodes[0].segmentIndex, 0u);
  EXPECT_DOUBLE_EQ(nodes[0].coord.x, 5.0);
  EXPECT_DOUBLE_EQ(nodes[0].coord.y, 0.05);
  EXPECT_EQ(nodes[1].segmentIndex, 2u);
  EXPECT_DOUBLE_EQ(nodes[1].coord.x, 5.0);
  EXPECT_DOUBLE_EQ(nodes[1].coord.y, 0.05);
}

TEST(SnappingIntersectionAdderTest, OpenSelfIntersectingNonAdjacentSegments) {
  SnappingPointIndex index(0.01);
  SnappingIntersectionAdder adder(0.01, index);
  auto ss =
      MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}, {5.0, 5.0}, {5.0, -5.0}});
  ASSERT_FALSE(ss->isClosed());

  adder.processIntersections(ss.get(), 0, ss.get(), 2);

  const std::vector<SegmentNode> nodes = CollectNodes(*ss);
  ASSERT_EQ(nodes.size(), 2u);
  EXPECT_EQ(nodes[0].segmentIndex, 0u);
  EXPECT_DOUBLE_EQ(nodes[0].coord.x, 5.0);
  EXPECT_DOUBLE_EQ(nodes[0].coord.y, 0.0);
  EXPECT_EQ(nodes[1].segmentIndex, 2u);
  EXPECT_DOUBLE_EQ(nodes[1].coord.x, 5.0);
  EXPECT_DOUBLE_EQ(nodes[1].coord.y, 0.0);
}

TEST(SnappingIntersectionAdderTest, ClosedRingNonAdjacentAndBoundarySegments) {
  // Closed figure-8 ring: non-adjacent segments 1 and 3 intersect at (5, 5).
  {
    SnappingPointIndex index(0.01);
    SnappingIntersectionAdder adder(0.01, index);
    auto figure8 = MakeSegmentString(
        {{0.0, 0.0}, {10.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}, {0.0, 0.0}});
    ASSERT_TRUE(figure8->isClosed());

    adder.processIntersections(figure8.get(), 1, figure8.get(), 3);

    const std::vector<SegmentNode> nodes = CollectNodes(*figure8);
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].segmentIndex, 1u);
    EXPECT_DOUBLE_EQ(nodes[0].coord.x, 5.0);
    EXPECT_DOUBLE_EQ(nodes[0].coord.y, 5.0);
    EXPECT_EQ(nodes[1].segmentIndex, 3u);
    EXPECT_DOUBLE_EQ(nodes[1].coord.x, 5.0);
    EXPECT_DOUBLE_EQ(nodes[1].coord.y, 5.0);
  }

  // Closed square ring: exercise both closing-pair checks in isAdjacent().
  for (bool swap_order : {false, true}) {
    SnappingPointIndex index(0.01);
    SnappingIntersectionAdder adder(0.01, index);
    auto ring = MakeSegmentString(
        {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
    ASSERT_TRUE(ring->isClosed());

    if (!swap_order) {
      adder.processIntersections(ring.get(), 0, ring.get(), 3);
    } else {
      adder.processIntersections(ring.get(), 3, ring.get(), 0);
    }

    const std::vector<SegmentNode> nodes = CollectNodes(*ring);
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].segmentIndex, 0u);
    EXPECT_DOUBLE_EQ(nodes[0].coord.x, 0.0);
    EXPECT_DOUBLE_EQ(nodes[0].coord.y, 0.0);
    EXPECT_EQ(nodes[1].segmentIndex, 4u);
    EXPECT_DOUBLE_EQ(nodes[1].coord.x, 0.0);
    EXPECT_DOUBLE_EQ(nodes[1].coord.y, 0.0);
  }
}

TEST(SnappingIntersectionAdderTest, ZeroToleranceNodesCrossingOnly) {
  SnappingPointIndex index(0.0);
  SnappingIntersectionAdder adder(0.0, index);
  auto ss0 = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
  auto ss1 = MakeSegmentString({{5.0, 0.001}, {5.0, 10.0}});

  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);
  EXPECT_EQ(ss0->getNodeList().size(), 0u);
  EXPECT_EQ(ss1->getNodeList().size(), 0u);

  auto ss2 = MakeSegmentString({{5.0, -5.0}, {5.0, 5.0}});
  adder.processIntersections(ss0.get(), 0, ss2.get(), 0);
  EXPECT_EQ(ss0->getNodeList().size(), 1u);
  EXPECT_EQ(ss2->getNodeList().size(), 1u);
}

TEST(SnappingIntersectionAdderTest, MultiSegmentStringsNonZeroIndices) {
  SnappingPointIndex index(0.1);
  SnappingIntersectionAdder adder(0.1, index);
  auto ss0 =
      MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}, {20.0, 10.0}, {30.0, 10.0}});
  auto ss1 = MakeSegmentString({{0.0, 10.0}, {10.0, 10.0}, {20.0, 0.0}});
  auto ss2 = MakeSegmentString({{25.0, 5.0}, {25.0, 15.0}});

  adder.processIntersections(ss0.get(), 1, ss1.get(), 1);
  adder.processIntersections(ss0.get(), 2, ss2.get(), 0);

  const std::vector<SegmentNode> nodes0 = CollectNodes(*ss0);
  ASSERT_EQ(nodes0.size(), 2u);
  EXPECT_EQ(nodes0[0].segmentIndex, 1u);
  EXPECT_DOUBLE_EQ(nodes0[0].coord.x, 15.0);
  EXPECT_DOUBLE_EQ(nodes0[0].coord.y, 5.0);
  EXPECT_TRUE(nodes0[0].isInterior());
  EXPECT_EQ(nodes0[1].segmentIndex, 2u);
  EXPECT_DOUBLE_EQ(nodes0[1].coord.x, 25.0);
  EXPECT_DOUBLE_EQ(nodes0[1].coord.y, 10.0);
  EXPECT_TRUE(nodes0[1].isInterior());

  const std::vector<SegmentNode> nodes1 = CollectNodes(*ss1);
  ASSERT_EQ(nodes1.size(), 1u);
  EXPECT_EQ(nodes1[0].segmentIndex, 1u);
  EXPECT_DOUBLE_EQ(nodes1[0].coord.x, 15.0);
  EXPECT_DOUBLE_EQ(nodes1[0].coord.y, 5.0);

  const std::vector<SegmentNode> nodes2 = CollectNodes(*ss2);
  ASSERT_EQ(nodes2.size(), 1u);
  EXPECT_EQ(nodes2[0].segmentIndex, 0u);
  EXPECT_DOUBLE_EQ(nodes2[0].coord.x, 25.0);
  EXPECT_DOUBLE_EQ(nodes2[0].coord.y, 10.0);

  const std::unique_ptr<CoordinateSequence> noded_coords =
      ss0->getNodedCoordinates();
  ASSERT_EQ(noded_coords->size(), 6u);
  EXPECT_DOUBLE_EQ(noded_coords->getX(2), 15.0);
  EXPECT_DOUBLE_EQ(noded_coords->getY(2), 5.0);
  EXPECT_DOUBLE_EQ(noded_coords->getX(4), 25.0);
  EXPECT_DOUBLE_EQ(noded_coords->getY(4), 10.0);
}

TEST(SnappingIntersectionAdderTest,
     CrossingWithNearEndpointAddsBothCrossingAndNearVertexNodes) {
  SnappingPointIndex index(0.1);
  SnappingIntersectionAdder adder(0.1, index);
  auto target = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
  auto crossing_near = MakeSegmentString({{7.0, 10.0}, {7.0, -0.05}});

  adder.processIntersections(target.get(), 0, crossing_near.get(), 0);

  const std::vector<SegmentNode> target_nodes = CollectNodes(*target);
  ASSERT_EQ(target_nodes.size(), 2u);
  EXPECT_DOUBLE_EQ(target_nodes[0].coord.x, 7.0);
  EXPECT_DOUBLE_EQ(target_nodes[0].coord.y, -0.05);
  EXPECT_TRUE(target_nodes[0].isInterior());
  EXPECT_DOUBLE_EQ(target_nodes[1].coord.x, 7.0);
  EXPECT_DOUBLE_EQ(target_nodes[1].coord.y, 0.0);
  EXPECT_TRUE(target_nodes[1].isInterior());

  const std::vector<SegmentNode> crossing_nodes = CollectNodes(*crossing_near);
  ASSERT_EQ(crossing_nodes.size(), 2u);
  EXPECT_EQ(crossing_nodes[0].segmentIndex, 0u);
  EXPECT_DOUBLE_EQ(crossing_nodes[0].coord.x, 7.0);
  EXPECT_DOUBLE_EQ(crossing_nodes[0].coord.y, 0.0);
  EXPECT_TRUE(crossing_nodes[0].isInterior());
  EXPECT_EQ(crossing_nodes[1].segmentIndex, 1u);
  EXPECT_DOUBLE_EQ(crossing_nodes[1].coord.x, 7.0);
  EXPECT_DOUBLE_EQ(crossing_nodes[1].coord.y, -0.05);
  EXPECT_FALSE(crossing_nodes[1].isInterior());
}

TEST(SnappingIntersectionAdderTest,
     ProcessNearVertexExactEndpointDistanceBoundary) {
  // Use exact IEEE-754 distances (3-4-5 right triangles with snapTolerance 5.0)
  // to verify strict '<' in endpoint distance guards and segment distance
  // check.
  SnappingPointIndex index(5.0);
  SnappingIntersectionAdder adder(5.0, index);
  auto target = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
  auto near_p0_exact = MakeSegmentString({{3.0, 4.0}, {3.0, 20.0}});
  auto near_p1_exact = MakeSegmentString({{7.0, 20.0}, {7.0, 4.0}});
  auto exact_seg_dist = MakeSegmentString({{5.0, 5.0}, {5.0, 20.0}});

  adder.processIntersections(target.get(), 0, near_p0_exact.get(), 0);
  adder.processIntersections(target.get(), 0, near_p1_exact.get(), 0);
  adder.processIntersections(target.get(), 0, exact_seg_dist.get(), 0);

  const std::vector<SegmentNode> target_nodes = CollectNodes(*target);
  ASSERT_EQ(target_nodes.size(), 2u);
  EXPECT_DOUBLE_EQ(target_nodes[0].coord.x, 3.0);
  EXPECT_DOUBLE_EQ(target_nodes[0].coord.y, 4.0);
  EXPECT_DOUBLE_EQ(target_nodes[1].coord.x, 7.0);
  EXPECT_DOUBLE_EQ(target_nodes[1].coord.y, 4.0);

  const std::vector<SegmentNode> p0_nodes = CollectNodes(*near_p0_exact);
  ASSERT_EQ(p0_nodes.size(), 1u);
  EXPECT_EQ(p0_nodes[0].segmentIndex, 0u);
  EXPECT_DOUBLE_EQ(p0_nodes[0].coord.x, 3.0);
  EXPECT_DOUBLE_EQ(p0_nodes[0].coord.y, 4.0);

  const std::vector<SegmentNode> p1_nodes = CollectNodes(*near_p1_exact);
  ASSERT_EQ(p1_nodes.size(), 1u);
  EXPECT_EQ(p1_nodes[0].segmentIndex, 1u);
  EXPECT_DOUBLE_EQ(p1_nodes[0].coord.x, 7.0);
  EXPECT_DOUBLE_EQ(p1_nodes[0].coord.y, 4.0);

  EXPECT_EQ(exact_seg_dist->getNodeList().size(), 0u);
}

TEST(SnappingIntersectionAdderTest,
     EndpointTouchAndCollinearIdenticalSegments) {
  // T-junction: 1-point intersection at segment endpoint (5, 0).
  {
    SnappingPointIndex index(0.1);
    SnappingIntersectionAdder adder(0.1, index);
    auto base = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
    auto stem = MakeSegmentString({{5.0, 0.0}, {5.0, 10.0}});

    adder.processIntersections(base.get(), 0, stem.get(), 0);

    const std::vector<SegmentNode> base_nodes = CollectNodes(*base);
    ASSERT_EQ(base_nodes.size(), 1u);
    EXPECT_EQ(base_nodes[0].segmentIndex, 0u);
    EXPECT_DOUBLE_EQ(base_nodes[0].coord.x, 5.0);
    EXPECT_DOUBLE_EQ(base_nodes[0].coord.y, 0.0);
    EXPECT_TRUE(base_nodes[0].isInterior());

    const std::vector<SegmentNode> stem_nodes = CollectNodes(*stem);
    ASSERT_EQ(stem_nodes.size(), 1u);
    EXPECT_EQ(stem_nodes[0].segmentIndex, 0u);
    EXPECT_DOUBLE_EQ(stem_nodes[0].coord.x, 5.0);
    EXPECT_DOUBLE_EQ(stem_nodes[0].coord.y, 0.0);
    EXPECT_FALSE(stem_nodes[0].isInterior());
  }

  // Shared corner endpoints between two distinct SegmentStrings.
  {
    SnappingPointIndex index(0.1);
    SnappingIntersectionAdder adder(0.1, index);
    auto ss0 = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
    auto ss1 = MakeSegmentString({{10.0, 0.0}, {10.0, 10.0}});

    adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

    const std::vector<SegmentNode> nodes0 = CollectNodes(*ss0);
    ASSERT_EQ(nodes0.size(), 1u);
    EXPECT_EQ(nodes0[0].segmentIndex, 1u);
    EXPECT_DOUBLE_EQ(nodes0[0].coord.x, 10.0);
    EXPECT_DOUBLE_EQ(nodes0[0].coord.y, 0.0);
    EXPECT_FALSE(nodes0[0].isInterior());

    const std::vector<SegmentNode> nodes1 = CollectNodes(*ss1);
    ASSERT_EQ(nodes1.size(), 1u);
    EXPECT_EQ(nodes1[0].segmentIndex, 0u);
    EXPECT_DOUBLE_EQ(nodes1[0].coord.x, 10.0);
    EXPECT_DOUBLE_EQ(nodes1[0].coord.y, 0.0);
    EXPECT_FALSE(nodes1[0].isInterior());
  }

  // Collinear identical segments on two distinct SegmentStrings add no nodes.
  {
    SnappingPointIndex index(0.1);
    SnappingIntersectionAdder adder(0.1, index);
    auto ss0 = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});
    auto ss1 = MakeSegmentString({{0.0, 0.0}, {10.0, 0.0}});

    adder.processIntersections(ss0.get(), 0, ss1.get(), 0);
    EXPECT_EQ(ss0->getNodeList().size(), 0u);
    EXPECT_EQ(ss1->getNodeList().size(), 0u);
  }
}

}  // namespace
}  // namespace geos_google
