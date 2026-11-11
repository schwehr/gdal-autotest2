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

// Unit tests for third_party/geos/src/noding/SegmentNode.cpp and
// third_party/geos/include/geos/noding/SegmentNode.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/noding/SegmentNode.cpp

#include "third_party/geos/include/geos/noding/SegmentNode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::noding::BasicSegmentString;
using geos::noding::SegmentNode;
using geos::noding::SegmentNodeLT;

TEST(SegmentNodeTest, ConstructsFromAllCoordinateTypesAndSetsInteriorFlag) {
  CoordinateSequence seq = {
      CoordinateXYZM(0.0, 0.0, 10.0, 100.0),
      CoordinateXYZM(10.0, 0.0, 20.0, 200.0),
      CoordinateXYZM(10.0, 10.0, 30.0, 300.0),
  };
  BasicSegmentString ss(&seq, nullptr);

  // Matching segment 0 start in 2D marks node as non-interior even when Z/M
  // are missing.
  const SegmentNode node_xy(ss, CoordinateXY(0.0, 0.0), 0,
                            ss.getSegmentOctant(0));
  EXPECT_DOUBLE_EQ(node_xy.coord.x, 0.0);
  EXPECT_DOUBLE_EQ(node_xy.coord.y, 0.0);
  EXPECT_TRUE(std::isnan(node_xy.coord.z));
  EXPECT_TRUE(std::isnan(node_xy.coord.m));
  EXPECT_EQ(node_xy.segmentIndex, 0u);
  EXPECT_FALSE(node_xy.isInterior());

  // Distinct X/Y on segment 0 marks node as interior and preserves Z.
  const SegmentNode node_xyz(ss, Coordinate(5.0, 0.0, 15.0), 0,
                             ss.getSegmentOctant(0));
  EXPECT_DOUBLE_EQ(node_xyz.coord.x, 5.0);
  EXPECT_DOUBLE_EQ(node_xyz.coord.y, 0.0);
  EXPECT_DOUBLE_EQ(node_xyz.coord.z, 15.0);
  EXPECT_TRUE(std::isnan(node_xyz.coord.m));
  EXPECT_EQ(node_xyz.segmentIndex, 0u);
  EXPECT_TRUE(node_xyz.isInterior());

  // Matching segment 1 start with XYM marks node as non-interior and keeps M.
  const SegmentNode node_xym(ss, CoordinateXYM(10.0, 0.0, 250.0), 1,
                             ss.getSegmentOctant(1));
  EXPECT_DOUBLE_EQ(node_xym.coord.x, 10.0);
  EXPECT_DOUBLE_EQ(node_xym.coord.y, 0.0);
  EXPECT_TRUE(std::isnan(node_xym.coord.z));
  EXPECT_DOUBLE_EQ(node_xym.coord.m, 250.0);
  EXPECT_EQ(node_xym.segmentIndex, 1u);
  EXPECT_FALSE(node_xym.isInterior());

  // SegmentNode checks only segmentIndex vertex, so the second endpoint of
  // segment 1 is marked interior when stored under segmentIndex == 1.
  const SegmentNode node_seg1_end(ss, CoordinateXYZM(10.0, 10.0, 30.0, 300.0),
                                  1, ss.getSegmentOctant(1));
  EXPECT_TRUE(node_seg1_end.isInterior());

  // Normalized at final vertex (segmentIndex == 2).
  const SegmentNode node_xyzm(ss, CoordinateXYZM(10.0, 10.0, 30.0, 300.0), 2,
                              ss.getSegmentOctant(2));
  EXPECT_DOUBLE_EQ(node_xyzm.coord.z, 30.0);
  EXPECT_DOUBLE_EQ(node_xyzm.coord.m, 300.0);
  EXPECT_EQ(node_xyzm.segmentIndex, 2u);
  EXPECT_FALSE(node_xyzm.isInterior());
}

TEST(SegmentNodeTest, IsEndPointHandlesStartMaxAndInteriorSegments) {
  CoordinateSequence seq = {
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(20.0, 0.0),
  };
  BasicSegmentString ss(&seq, nullptr);

  const SegmentNode start_vertex(ss, CoordinateXY(0.0, 0.0), 0, 0);
  const SegmentNode seg0_interior(ss, CoordinateXY(5.0, 0.0), 0, 0);
  const SegmentNode mid_vertex(ss, CoordinateXY(10.0, 0.0), 1, 0);
  const SegmentNode seg1_interior(ss, CoordinateXY(15.0, 0.0), 1, 0);
  const SegmentNode end_vertex(ss, CoordinateXY(20.0, 0.0), 2, -1);
  const SegmentNode end_offset(ss, CoordinateXY(21.0, 0.0), 2, -1);

  EXPECT_TRUE(start_vertex.isEndPoint(2));
  EXPECT_FALSE(seg0_interior.isEndPoint(2));
  EXPECT_FALSE(mid_vertex.isEndPoint(2));
  EXPECT_FALSE(seg1_interior.isEndPoint(2));
  EXPECT_TRUE(end_vertex.isEndPoint(2));
  EXPECT_TRUE(end_offset.isEndPoint(2));

  // When maxSegmentIndex == 0, any node at segmentIndex 0 is an endpoint,
  // while nodes at higher segment indices are not.
  EXPECT_TRUE(start_vertex.isEndPoint(0));
  EXPECT_TRUE(seg0_interior.isEndPoint(0));
  EXPECT_FALSE(mid_vertex.isEndPoint(0));
  EXPECT_FALSE(seg1_interior.isEndPoint(0));
  EXPECT_FALSE(end_vertex.isEndPoint(0));

  // When maxSegmentIndex == 1, nodes at segmentIndex 1 match maxSegmentIndex
  // and are endpoints, while seg0_interior and segmentIndex 2 nodes are not.
  EXPECT_TRUE(start_vertex.isEndPoint(1));
  EXPECT_FALSE(seg0_interior.isEndPoint(1));
  EXPECT_TRUE(mid_vertex.isEndPoint(1));
  EXPECT_TRUE(seg1_interior.isEndPoint(1));
  EXPECT_FALSE(end_vertex.isEndPoint(1));
  EXPECT_FALSE(end_offset.isEndPoint(1));
}

TEST(SegmentNodeTest, CompareToOrdersBySegmentIndexEqualityAndExteriorFirst) {
  CoordinateSequence seq = {
      CoordinateXYZM(0.0, 0.0, 0.0, 0.0),
      CoordinateXYZM(10.0, 0.0, 10.0, 10.0),
      CoordinateXYZM(20.0, 0.0, 20.0, 20.0),
  };
  BasicSegmentString ss(&seq, nullptr);

  const SegmentNode seg0_start(ss, CoordinateXYZM(0.0, 0.0, 1.0, 2.0), 0, 0);
  const SegmentNode seg0_behind(ss, CoordinateXY(-1.0, 0.0), 0, 0);
  const SegmentNode seg0_mid_a(ss, CoordinateXYZM(5.0, 0.0, 1.0, 2.0), 0, 0);
  const SegmentNode seg0_mid_b(ss, CoordinateXYZM(5.0, 0.0, 99.0, 88.0), 0, 0);
  const SegmentNode seg1_start(ss, CoordinateXY(10.0, 0.0), 1, 0);

  // Different segmentIndex orders strictly by segmentIndex.
  EXPECT_EQ(seg0_mid_a.compareTo(seg1_start), -1);
  EXPECT_EQ(seg1_start.compareTo(seg0_mid_a), 1);

  // 2D-equal coordinates on the same segment compare equal despite differing
  // Z/M.
  EXPECT_EQ(seg0_mid_a.compareTo(seg0_mid_b), 0);
  EXPECT_EQ(seg0_start.compareTo(seg0_start), 0);

  // Non-interior (segment start) always precedes any interior node on the same
  // segment, even when coordinate order in that octant would disagree.
  EXPECT_EQ(seg0_start.compareTo(seg0_behind), -1);
  EXPECT_EQ(seg0_behind.compareTo(seg0_start), 1);
}

TEST(SegmentNodeTest, CompareToOrdersInteriorNodesAcrossAllEightOctants) {
  struct OctantCase {
    int expected_octant;
    double end_x;
    double end_y;
    double near_x;
    double near_y;
    double far_x;
    double far_y;
    double tie_a_x;
    double tie_a_y;
    double tie_b_x;
    double tie_b_y;
  };

  constexpr auto kCases = std::to_array<OctantCase>({
      {.expected_octant = 0,
       .end_x = 10.0,
       .end_y = 5.0,
       .near_x = 2.0,
       .near_y = 1.0,
       .far_x = 6.0,
       .far_y = 3.0,
       .tie_a_x = 4.0,
       .tie_a_y = 1.0,
       .tie_b_x = 4.0,
       .tie_b_y = 2.0},
      {.expected_octant = 1,
       .end_x = 5.0,
       .end_y = 10.0,
       .near_x = 1.0,
       .near_y = 2.0,
       .far_x = 3.0,
       .far_y = 6.0,
       .tie_a_x = 1.0,
       .tie_a_y = 4.0,
       .tie_b_x = 2.0,
       .tie_b_y = 4.0},
      {.expected_octant = 2,
       .end_x = -5.0,
       .end_y = 10.0,
       .near_x = -1.0,
       .near_y = 2.0,
       .far_x = -3.0,
       .far_y = 6.0,
       .tie_a_x = -1.0,
       .tie_a_y = 4.0,
       .tie_b_x = -2.0,
       .tie_b_y = 4.0},
      {.expected_octant = 3,
       .end_x = -10.0,
       .end_y = 5.0,
       .near_x = -2.0,
       .near_y = 1.0,
       .far_x = -6.0,
       .far_y = 3.0,
       .tie_a_x = -4.0,
       .tie_a_y = 1.0,
       .tie_b_x = -4.0,
       .tie_b_y = 2.0},
      {.expected_octant = 4,
       .end_x = -10.0,
       .end_y = -5.0,
       .near_x = -2.0,
       .near_y = -1.0,
       .far_x = -6.0,
       .far_y = -3.0,
       .tie_a_x = -4.0,
       .tie_a_y = -1.0,
       .tie_b_x = -4.0,
       .tie_b_y = -2.0},
      {.expected_octant = 5,
       .end_x = -5.0,
       .end_y = -10.0,
       .near_x = -1.0,
       .near_y = -2.0,
       .far_x = -3.0,
       .far_y = -6.0,
       .tie_a_x = -1.0,
       .tie_a_y = -4.0,
       .tie_b_x = -2.0,
       .tie_b_y = -4.0},
      {.expected_octant = 6,
       .end_x = 5.0,
       .end_y = -10.0,
       .near_x = 1.0,
       .near_y = -2.0,
       .far_x = 3.0,
       .far_y = -6.0,
       .tie_a_x = 1.0,
       .tie_a_y = -4.0,
       .tie_b_x = 2.0,
       .tie_b_y = -4.0},
      {.expected_octant = 7,
       .end_x = 10.0,
       .end_y = -5.0,
       .near_x = 2.0,
       .near_y = -1.0,
       .far_x = 6.0,
       .far_y = -3.0,
       .tie_a_x = 4.0,
       .tie_a_y = -1.0,
       .tie_b_x = 4.0,
       .tie_b_y = -2.0},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.expected_octant);
    CoordinateSequence seq = {
        CoordinateXY(0.0, 0.0),
        CoordinateXY(tc.end_x, tc.end_y),
    };
    BasicSegmentString ss(&seq, nullptr);
    const int octant = ss.getSegmentOctant(0);
    ASSERT_EQ(octant, tc.expected_octant);

    const SegmentNode n_near(ss, CoordinateXY(tc.near_x, tc.near_y), 0, octant);
    const SegmentNode n_far(ss, CoordinateXY(tc.far_x, tc.far_y), 0, octant);
    EXPECT_EQ(n_near.compareTo(n_far), -1);
    EXPECT_EQ(n_far.compareTo(n_near), 1);
    EXPECT_EQ(n_near.compareTo(n_near), 0);

    const SegmentNode n_tie_a(ss, CoordinateXY(tc.tie_a_x, tc.tie_a_y), 0,
                              octant);
    const SegmentNode n_tie_b(ss, CoordinateXY(tc.tie_b_x, tc.tie_b_y), 0,
                              octant);
    EXPECT_EQ(n_tie_a.compareTo(n_tie_b), -1);
    EXPECT_EQ(n_tie_b.compareTo(n_tie_a), 1);
  }
}

TEST(SegmentNodeTest, SegmentNodeLTOrdersBothValuesAndPointers) {
  CoordinateSequence seq = {
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(10.0, 10.0),
  };
  BasicSegmentString ss(&seq, nullptr);

  SegmentNode n0(ss, CoordinateXY(0.0, 0.0), 0, ss.getSegmentOctant(0));
  SegmentNode n1(ss, CoordinateXY(3.0, 0.0), 0, ss.getSegmentOctant(0));
  SegmentNode n2(ss, CoordinateXY(7.0, 0.0), 0, ss.getSegmentOctant(0));
  SegmentNode n3(ss, CoordinateXY(10.0, 5.0), 1, ss.getSegmentOctant(1));

  const SegmentNodeLT lt;
  EXPECT_FALSE(lt(n1, n1));
  EXPECT_FALSE(lt(&n1, &n1));
  EXPECT_TRUE(lt(n0, n1));
  EXPECT_TRUE(lt(&n0, &n1));
  EXPECT_FALSE(lt(n2, n1));
  EXPECT_FALSE(lt(&n2, &n1));

  std::vector<SegmentNode> by_value = {n3, n1, n0, n2};
  std::sort(by_value.begin(), by_value.end(), lt);
  EXPECT_DOUBLE_EQ(by_value[0].coord.x, 0.0);
  EXPECT_DOUBLE_EQ(by_value[1].coord.x, 3.0);
  EXPECT_DOUBLE_EQ(by_value[2].coord.x, 7.0);
  EXPECT_EQ(by_value[3].segmentIndex, 1u);

  std::vector<SegmentNode*> by_ptr = {&n3, &n2, &n0, &n1};
  std::sort(by_ptr.begin(), by_ptr.end(), lt);
  EXPECT_EQ(by_ptr[0], &n0);
  EXPECT_EQ(by_ptr[1], &n1);
  EXPECT_EQ(by_ptr[2], &n2);
  EXPECT_EQ(by_ptr[3], &n3);
}

TEST(SegmentNodeTest, StreamInsertionFormatsCoordSegmentIndexAndOctant) {
  CoordinateSequence seq = {
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(0.0, 10.0),
  };
  BasicSegmentString ss(&seq, nullptr);

  const SegmentNode node_xy(ss, CoordinateXY(1.5, 2.5), 0, 3);
  const SegmentNode node_xyz(ss, Coordinate(1.5, 2.5, 4.25), 1, 7);
  const SegmentNode node_xyzm(ss, CoordinateXYZM(1.5, 2.5, 4.25, 9.5), 2, -1);

  std::ostringstream oss_xy;
  std::ostream& returned = (oss_xy << node_xy);
  EXPECT_EQ(&returned, &oss_xy);
  EXPECT_EQ(oss_xy.str(), "1.5 2.5 seg#=0 octant#=3\n");

  std::ostringstream oss_chained;
  oss_chained << node_xyz << node_xyzm;
  EXPECT_EQ(oss_chained.str(),
            "1.5 2.5 4.25 seg#=1 octant#=7\n"
            "1.5 2.5 4.25 9.5 seg#=2 octant#=-1\n");
}

}  // namespace
}  // namespace geos_google
