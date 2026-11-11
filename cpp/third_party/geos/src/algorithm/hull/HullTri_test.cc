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

// Unit tests for third_party/geos/src/algorithm/hull/HullTri.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/hull/HullTri.cpp

#include "third_party/geos/include/geos/algorithm/hull/HullTri.h"

#include <cmath>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/triangulate/tri/TriList.h"
#include "third_party/geos/include/geos/triangulate/tri/TriangulationBuilder.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::algorithm::hull::HullTri;
using geos::geom::Coordinate;
using geos::triangulate::tri::TriangulationBuilder;
using geos::triangulate::tri::TriList;
using geos::util::IllegalArgumentException;

void LinkTris(TriList<HullTri>& triList) {
  TriangulationBuilder tb;
  for (HullTri* tri : triList) {
    tb.add(tri);
  }
}

// Builds a CW center triangle surrounded on all three edges by three CW
// neighbor triangles, matching the outer triangle (0,0)-(2,4)-(4,0).
// Index 0: T_center, Index 1: T1 (edge 0), Index 2: T2 (edge 1), Index 3: T0
// (edge 2).
void BuildCenterAndThreeNeighbors(TriList<HullTri>& triList) {
  const Coordinate a(0, 0);
  const Coordinate b(2, 4);
  const Coordinate c(4, 0);
  const Coordinate m_ab(1, 2);
  const Coordinate m_bc(3, 2);
  const Coordinate m_ca(2, 0);

  triList.add(m_ab, m_bc, m_ca);  // T_center (0)
  triList.add(m_ab, b, m_bc);     // T1 (1), shares edge 0 of T_center
  triList.add(m_bc, c, m_ca);     // T2 (2), shares edge 1 of T_center
  triList.add(a, m_ab, m_ca);     // T0 (3), shares edge 2 of T_center
  LinkTris(triList);
  triList.validate();
}

// Builds 4 CW triangles forming square [0,2]x[0,2] meeting at interior point
// (1,1). For every T_i, edge 0 is on the square boundary and edges 1 & 2 are
// interior.
void BuildSquareFan(TriList<HullTri>& triList) {
  const Coordinate center(1, 1);
  triList.add(Coordinate(0, 0), Coordinate(0, 2), center);  // T0
  triList.add(Coordinate(0, 2), Coordinate(2, 2), center);  // T1
  triList.add(Coordinate(2, 2), Coordinate(2, 0), center);  // T2
  triList.add(Coordinate(2, 0), Coordinate(0, 0), center);  // T3
  LinkTris(triList);
  triList.validate();
}

TEST(HullTriTest, ConstructorAndSizeSetters) {
  // 3-4-5 right triangle in clockwise orientation.
  HullTri tri(Coordinate(0, 0), Coordinate(0, 3), Coordinate(4, 0));

  EXPECT_DOUBLE_EQ(tri.lengthOfLongestEdge(), 5.0);
  EXPECT_DOUBLE_EQ(tri.getSize(), 5.0);
  EXPECT_DOUBLE_EQ(tri.lengthOfBoundary(), 12.0);
  EXPECT_DOUBLE_EQ(tri.getArea(), 6.0);

  tri.setSizeToBoundary();
  EXPECT_DOUBLE_EQ(tri.getSize(), 12.0);

  tri.setSizeToCircumradius();
  EXPECT_DOUBLE_EQ(tri.getSize(), 2.5);

  tri.setSizeToLongestEdge();
  EXPECT_DOUBLE_EQ(tri.getSize(), 5.0);
}

TEST(HullTriTest, SetSizeToCircumradiusAcuteAndCollinear) {
  HullTri acute(Coordinate(0, 0), Coordinate(3, 4), Coordinate(6, 0));
  acute.setSizeToCircumradius();
  EXPECT_DOUBLE_EQ(acute.getSize(), 3.125);

  HullTri right_iso(Coordinate(0, 0), Coordinate(0, 2), Coordinate(2, 0));
  right_iso.setSizeToCircumradius();
  EXPECT_NEAR(right_iso.getSize(), std::sqrt(2.0), 1e-12);

  HullTri collinear(Coordinate(0, 0), Coordinate(1, 0), Coordinate(2, 0));
  collinear.setSizeToCircumradius();
  EXPECT_TRUE(std::isinf(collinear.getSize()));
  EXPECT_GT(collinear.getSize(), 0.0);
}

TEST(HullTriTest, HullTriCompareOrdersBySizeThenArea) {
  // Longest edge 4.0, area 2.0 (sides sqrt(5), sqrt(5), 4.0).
  HullTri small_size(Coordinate(0, 0), Coordinate(2, 1), Coordinate(4, 0));
  // Longest edge 5.0, area 2.5 (sides sqrt(7.25), sqrt(7.25), 5.0).
  HullTri same_size_small_area(Coordinate(0, 0), Coordinate(2.5, 1.0),
                               Coordinate(5, 0));
  // Longest edge 5.0, area 6.0 (3-4-5 right triangle).
  HullTri same_size_large_area(Coordinate(0, 0), Coordinate(0, 3),
                               Coordinate(4, 0));
  // Distinct triangle with same longest edge 5.0 and same area 6.0.
  HullTri same_size_same_area(Coordinate(0, 0), Coordinate(0, 4),
                              Coordinate(3, 0));

  ASSERT_DOUBLE_EQ(small_size.getSize(), 4.0);
  ASSERT_EQ(same_size_small_area.getSize(), same_size_large_area.getSize());
  ASSERT_DOUBLE_EQ(same_size_large_area.getSize(), 5.0);
  ASSERT_DOUBLE_EQ(same_size_small_area.getArea(), 2.5);
  ASSERT_DOUBLE_EQ(same_size_large_area.getArea(), 6.0);
  ASSERT_DOUBLE_EQ(same_size_same_area.getArea(), 6.0);

  HullTri::HullTriCompare cmp;
  EXPECT_TRUE(cmp(&small_size, &same_size_small_area));
  EXPECT_FALSE(cmp(&same_size_small_area, &small_size));

  EXPECT_TRUE(cmp(&same_size_small_area, &same_size_large_area));
  EXPECT_FALSE(cmp(&same_size_large_area, &same_size_small_area));
  EXPECT_FALSE(cmp(&same_size_large_area, &same_size_large_area));
  EXPECT_FALSE(cmp(&same_size_large_area, &same_size_same_area));
  EXPECT_FALSE(cmp(&same_size_same_area, &same_size_large_area));

  std::priority_queue<HullTri*, std::vector<HullTri*>, HullTri::HullTriCompare>
      pq;
  pq.push(&small_size);
  pq.push(&same_size_small_area);
  pq.push(&same_size_large_area);

  EXPECT_EQ(pq.top(), &same_size_large_area);
  pq.pop();
  EXPECT_EQ(pq.top(), &same_size_small_area);
  pq.pop();
  EXPECT_EQ(pq.top(), &small_size);
}

TEST(HullTriTest, MarkedFlagDefaultsAndToggles) {
  HullTri tri(Coordinate(0, 0), Coordinate(0, 1), Coordinate(1, 0));
  EXPECT_FALSE(tri.isMarked());
  tri.setMarked(true);
  EXPECT_TRUE(tri.isMarked());
  tri.setMarked(false);
  EXPECT_FALSE(tri.isMarked());
}

TEST(HullTriTest, IsRemovedAndRemoveFromTriList) {
  HullTri isolated(Coordinate(0, 0), Coordinate(0, 1), Coordinate(1, 0));
  EXPECT_TRUE(isolated.isRemoved());

  TriList<HullTri> triList;
  BuildCenterAndThreeNeighbors(triList);
  HullTri* center = triList[0];
  HullTri* t1 = triList[1];
  HullTri* t2 = triList[2];
  HullTri* t0 = triList[3];

  EXPECT_FALSE(center->isRemoved());
  EXPECT_FALSE(t1->isRemoved());

  t1->remove(triList);
  EXPECT_EQ(triList.size(), 3u);
  EXPECT_TRUE(t1->isRemoved());
  EXPECT_FALSE(center->isRemoved());
  EXPECT_FALSE(center->isAdjacent(t1));
  EXPECT_TRUE(center->isAdjacent(t2));
  EXPECT_TRUE(center->isAdjacent(t0));

  t2->remove(triList);
  t0->remove(triList);
  EXPECT_EQ(triList.size(), 1u);
  EXPECT_TRUE(center->isRemoved());
}

TEST(HullTriTest, BoundaryIndicesAndLengthsAcrossAllEdgeCombinations) {
  const double diag = std::sqrt(5.0);

  // 1. All 3 edges adjacent (interior tri).
  {
    TriList<HullTri> triList;
    BuildCenterAndThreeNeighbors(triList);
    HullTri* c = triList[0];
    EXPECT_EQ(c->boundaryIndex(), -1);
    EXPECT_EQ(c->boundaryIndexCCW(), -1);
    EXPECT_EQ(c->boundaryIndexCW(), -1);
    EXPECT_EQ(c->adjacent2VertexIndex(), 1);
    EXPECT_DOUBLE_EQ(c->lengthOfBoundary(), 0.0);
  }

  // 2. Only edge 2 is boundary.
  {
    TriList<HullTri> triList;
    BuildCenterAndThreeNeighbors(triList);
    HullTri* c = triList[0];
    HullTri* t0 = triList[3];
    t0->remove(triList);
    EXPECT_EQ(triList.size(), 3u);
    EXPECT_EQ(c->boundaryIndex(), 2);
    EXPECT_EQ(c->boundaryIndexCCW(), 2);
    EXPECT_EQ(c->boundaryIndexCW(), 2);
    EXPECT_EQ(c->adjacent2VertexIndex(), 1);
    EXPECT_DOUBLE_EQ(c->lengthOfBoundary(), diag);
  }

  // 3. Only edge 1 is boundary.
  {
    TriList<HullTri> triList;
    BuildCenterAndThreeNeighbors(triList);
    HullTri* c = triList[0];
    HullTri* t2 = triList[2];
    t2->remove(triList);
    EXPECT_EQ(triList.size(), 3u);
    EXPECT_EQ(c->boundaryIndex(), 1);
    EXPECT_EQ(c->boundaryIndexCCW(), 1);
    EXPECT_EQ(c->boundaryIndexCW(), 1);
    EXPECT_EQ(c->adjacent2VertexIndex(), 0);
    EXPECT_DOUBLE_EQ(c->lengthOfBoundary(), diag);
  }

  // 4. Only edge 0 is boundary.
  {
    TriList<HullTri> triList;
    BuildCenterAndThreeNeighbors(triList);
    HullTri* c = triList[0];
    HullTri* t1 = triList[1];
    t1->remove(triList);
    EXPECT_EQ(triList.size(), 3u);
    EXPECT_EQ(c->boundaryIndex(), 0);
    EXPECT_EQ(c->boundaryIndexCCW(), 0);
    EXPECT_EQ(c->boundaryIndexCW(), 0);
    EXPECT_EQ(c->adjacent2VertexIndex(), 2);
    EXPECT_DOUBLE_EQ(c->lengthOfBoundary(), 2.0);
  }

  // 5. Edges 0 and 1 are boundary (only edge 2 adjacent).
  {
    TriList<HullTri> triList;
    BuildCenterAndThreeNeighbors(triList);
    HullTri* c = triList[0];
    HullTri* t1 = triList[1];
    HullTri* t2 = triList[2];
    t1->remove(triList);
    t2->remove(triList);
    EXPECT_EQ(triList.size(), 2u);
    EXPECT_EQ(c->boundaryIndex(), 0);
    EXPECT_EQ(c->boundaryIndexCCW(), 0);
    EXPECT_EQ(c->boundaryIndexCW(), 1);
    EXPECT_EQ(c->adjacent2VertexIndex(), -1);
    EXPECT_DOUBLE_EQ(c->lengthOfBoundary(), 2.0 + diag);
  }

  // 6. Edges 0 and 2 are boundary (only edge 1 adjacent).
  {
    TriList<HullTri> triList;
    BuildCenterAndThreeNeighbors(triList);
    HullTri* c = triList[0];
    HullTri* t1 = triList[1];
    HullTri* t0 = triList[3];
    t1->remove(triList);
    t0->remove(triList);
    EXPECT_EQ(triList.size(), 2u);
    EXPECT_EQ(c->boundaryIndex(), 0);
    EXPECT_EQ(c->boundaryIndexCCW(), 2);
    EXPECT_EQ(c->boundaryIndexCW(), 0);
    EXPECT_EQ(c->adjacent2VertexIndex(), -1);
    EXPECT_DOUBLE_EQ(c->lengthOfBoundary(), 2.0 + diag);
  }

  // 7. Edges 1 and 2 are boundary (only edge 0 adjacent).
  {
    TriList<HullTri> triList;
    BuildCenterAndThreeNeighbors(triList);
    HullTri* c = triList[0];
    HullTri* t2 = triList[2];
    HullTri* t0 = triList[3];
    t2->remove(triList);
    t0->remove(triList);
    EXPECT_EQ(triList.size(), 2u);
    EXPECT_EQ(c->boundaryIndex(), 1);
    EXPECT_EQ(c->boundaryIndexCCW(), 1);
    EXPECT_EQ(c->boundaryIndexCW(), 2);
    EXPECT_EQ(c->adjacent2VertexIndex(), -1);
    EXPECT_DOUBLE_EQ(c->lengthOfBoundary(), 2.0 * diag);
  }

  // 8. All 3 edges are boundary (0 adjacent).
  {
    TriList<HullTri> triList;
    BuildCenterAndThreeNeighbors(triList);
    HullTri* c = triList[0];
    HullTri* t1 = triList[1];
    HullTri* t2 = triList[2];
    HullTri* t0 = triList[3];
    t1->remove(triList);
    t2->remove(triList);
    t0->remove(triList);
    EXPECT_EQ(triList.size(), 1u);
    EXPECT_EQ(c->boundaryIndex(), 0);
    EXPECT_EQ(c->boundaryIndexCCW(), 2);
    EXPECT_EQ(c->boundaryIndexCW(), 1);
    EXPECT_EQ(c->adjacent2VertexIndex(), -1);
    EXPECT_DOUBLE_EQ(c->lengthOfBoundary(), 2.0 + 2.0 * diag);
  }
}

TEST(HullTriTest, IsConnectingDetectsBridgeAndNonBridgeBorderTris) {
  TriList<HullTri> triList;
  BuildSquareFan(triList);
  HullTri* t0 = triList[0];
  HullTri* t1 = triList[1];
  HullTri* t2 = triList[2];

  // Center (1,1) is surrounded by all 4 tris, so removing t0 leaves {t1,t2,t3}
  // connected.
  EXPECT_FALSE(t0->isConnecting());

  // Removing t2 leaves chain t3 -- t0 -- t1; (1,1) is now on the boundary,
  // so t0 connects two otherwise disconnected components.
  t2->remove(triList);
  EXPECT_TRUE(t0->isConnecting());

  // t1 now has only 1 adjacent tri (adjacent2VertexIndex == -1).
  EXPECT_EQ(t1->adjacent2VertexIndex(), -1);
  EXPECT_THROW(t1->isConnecting(), IllegalArgumentException);

  // Also test adjacent2VertexIndex slots 0 and 1 on bridge triangles.
  {
    TriList<HullTri> patch;
    BuildCenterAndThreeNeighbors(patch);
    HullTri* center = patch[0];
    HullTri* pt1 = patch[1];
    pt1->remove(patch);
    EXPECT_EQ(center->adjacent2VertexIndex(), 2);
    EXPECT_TRUE(center->isConnecting());
  }
  {
    TriList<HullTri> patch;
    BuildCenterAndThreeNeighbors(patch);
    HullTri* center = patch[0];
    HullTri* pt2 = patch[2];
    pt2->remove(patch);
    EXPECT_EQ(center->adjacent2VertexIndex(), 0);
    EXPECT_TRUE(center->isConnecting());
  }
  {
    TriList<HullTri> patch;
    BuildCenterAndThreeNeighbors(patch);
    HullTri* center = patch[0];
    HullTri* pt0 = patch[3];
    pt0->remove(patch);
    EXPECT_EQ(center->adjacent2VertexIndex(), 1);
    EXPECT_TRUE(center->isConnecting());
  }
}

TEST(HullTriTest, IsolatedVertexIndexCoversAllSlotsAndNone) {
  {
    TriList<HullTri> empty;
    HullTri outside(Coordinate(0, 0), Coordinate(0, 1), Coordinate(1, 0));
    EXPECT_EQ(outside.isolatedVertexIndex(empty), 0);
  }

  {
    TriList<HullTri> single;
    single.add(Coordinate(0, 0), Coordinate(0, 1), Coordinate(1, 0));
    EXPECT_EQ(single[0]->isolatedVertexIndex(single), 0);
  }

  {
    TriList<HullTri> twoTris;
    twoTris.add(Coordinate(0, 0), Coordinate(0, 1), Coordinate(1, 1));
    twoTris.add(Coordinate(0, 0), Coordinate(1, 1), Coordinate(1, 0));
    LinkTris(twoTris);
    // For twoTris[0]: (0,0) deg 2, (0,1) deg 1, (1,1) deg 2 -> index 1.
    EXPECT_EQ(twoTris[0]->isolatedVertexIndex(twoTris), 1);
    // For twoTris[1]: (0,0) deg 2, (1,1) deg 2, (1,0) deg 1 -> index 2.
    EXPECT_EQ(twoTris[1]->isolatedVertexIndex(twoTris), 2);
  }

  {
    TriList<HullTri> fourTris;
    BuildCenterAndThreeNeighbors(fourTris);
    // All 3 vertices of T_center belong to 3 triangles -> no isolated vertex.
    EXPECT_EQ(fourTris[0]->isolatedVertexIndex(fourTris), -1);
  }
}

TEST(HullTriTest, HasBoundaryTouchCoversInteriorAndBoundaryVertexCases) {
  HullTri isolated(Coordinate(0, 0), Coordinate(0, 1), Coordinate(1, 0));
  EXPECT_FALSE(isolated.hasBoundaryTouch());

  {
    TriList<HullTri> triList;
    BuildSquareFan(triList);
    HullTri* t0 = triList[0];
    HullTri* t1 = triList[1];
    HullTri* t2 = triList[2];

    // Vertex 2 is interior; vertices 0 & 1 lie on boundary edge 0.
    EXPECT_FALSE(t0->hasBoundaryTouch());

    // After removing t2, vertex 2 of t0 is no longer interior, while neither
    // edge 1 nor edge 2 of t0 is a boundary edge.
    t2->remove(triList);
    EXPECT_TRUE(t0->hasBoundaryTouch());
    // t1 now has boundary edges 0 and 1, so all 3 vertices lie on boundary
    // edges.
    EXPECT_FALSE(t1->hasBoundaryTouch());
  }

  {
    TriList<HullTri> fourTris;
    BuildCenterAndThreeNeighbors(fourTris);
    // Center tri has 0 boundary edges, yet all 3 vertices touch the outer
    // boundary.
    EXPECT_FALSE(fourTris[0]->isBorder());
    EXPECT_TRUE(fourTris[0]->hasBoundaryTouch());
  }
}

TEST(HullTriTest, FindTriIsAllMarkedAndClearMarks) {
  TriList<HullTri> empty;
  EXPECT_EQ(HullTri::findTri(empty, nullptr), nullptr);
  EXPECT_TRUE(HullTri::isAllMarked(empty));
  HullTri::clearMarks(empty);

  TriList<HullTri> one;
  one.add(Coordinate(0, 0), Coordinate(0, 1), Coordinate(1, 0));
  EXPECT_EQ(HullTri::findTri(one, one[0]), nullptr);
  EXPECT_EQ(HullTri::findTri(one, nullptr), one[0]);

  TriList<HullTri> multi;
  BuildSquareFan(multi);
  EXPECT_EQ(HullTri::findTri(multi, multi[0]), multi[1]);
  EXPECT_FALSE(HullTri::isAllMarked(multi));

  // Partially marked: first marked, later unmarked -> still false.
  multi[0]->setMarked(true);
  multi[1]->setMarked(true);
  EXPECT_FALSE(HullTri::isAllMarked(multi));

  for (HullTri* tri : multi) {
    tri->setMarked(true);
  }
  EXPECT_TRUE(HullTri::isAllMarked(multi));

  HullTri::clearMarks(multi);
  for (HullTri* tri : multi) {
    EXPECT_FALSE(tri->isMarked());
  }
  EXPECT_FALSE(HullTri::isAllMarked(multi));
}

TEST(HullTriTest, MarkConnectedAndIsConnected) {
  TriList<HullTri> empty;
  EXPECT_FALSE(HullTri::isConnected(empty, nullptr));

  TriList<HullTri> single;
  single.add(Coordinate(0, 0), Coordinate(0, 1), Coordinate(1, 0));
  EXPECT_FALSE(HullTri::isConnected(single, single[0]));

  // 4-tri fan: cycle around (1,1), so excluding any tri keeps the rest
  // connected.
  TriList<HullTri> fan;
  BuildSquareFan(fan);
  HullTri* t0 = fan[0];
  HullTri* t1 = fan[1];
  HullTri* t2 = fan[2];
  HullTri* t3 = fan[3];

  HullTri::markConnected(t0, t2);
  EXPECT_TRUE(t0->isMarked());
  EXPECT_TRUE(t1->isMarked());
  EXPECT_FALSE(t2->isMarked());
  EXPECT_TRUE(t3->isMarked());

  HullTri::clearMarks(fan);
  HullTri::markConnected(t0, nullptr);
  EXPECT_TRUE(HullTri::isAllMarked(fan));

  // Leave marks dirty beforehand to confirm isConnected resets marks
  // internally.
  EXPECT_TRUE(HullTri::isConnected(fan, t0));

  // Removing t2 leaves chain t3 -- t0 -- t1; excluding t0 disconnects t3 and
  // t1.
  t2->remove(fan);
  EXPECT_FALSE(HullTri::isConnected(fan, t0));

  // Disconnected component added to triList: excluding t1 still leaves {t0, t3}
  // disconnected from the extra component.
  fan.add(Coordinate(10, 0), Coordinate(10, 2), Coordinate(12, 0));
  EXPECT_FALSE(HullTri::isConnected(fan, t1));
}

TEST(HullTriTest, StreamInsertionFormatsCoordinatesAdjacentsAndSize) {
  HullTri isolated(Coordinate(0, 0), Coordinate(0, 3), Coordinate(4, 0));
  std::ostringstream oss_isolated;
  oss_isolated << isolated;
  const std::string str_isolated = oss_isolated.str();
  EXPECT_NE(str_isolated.find("(0 0,0 3,4 0)"), std::string::npos);
  EXPECT_NE(str_isolated.find(" 5"), std::string::npos);

  TriList<HullTri> triList;
  BuildCenterAndThreeNeighbors(triList);
  std::ostringstream oss_linked;
  oss_linked << *triList[0];
  const std::string str_linked = oss_linked.str();
  EXPECT_NE(str_linked.find("(1 2,3 2,2 0)"), std::string::npos);
  std::ostringstream oss_size;
  oss_size << " " << triList[0]->getSize();
  EXPECT_NE(str_linked.find(oss_size.str()), std::string::npos);
}

}  // namespace
}  // namespace geos_google
