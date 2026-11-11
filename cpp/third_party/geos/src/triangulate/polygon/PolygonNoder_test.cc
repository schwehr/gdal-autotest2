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

// Unit tests for third_party/geos/src/triangulate/polygon/PolygonNoder.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/triangulate/polygon/PolygonNoder.cpp

#include "third_party/geos/include/geos/triangulate/polygon/PolygonNoder.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::triangulate::polygon::PolygonNoder;
using ::testing::ElementsAre;

std::unique_ptr<CoordinateSequence> MakeRing(
    std::initializer_list<CoordinateXY> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

void ExpectCoords2DEq(const CoordinateSequence& actual,
                      std::initializer_list<CoordinateXY> expected) {
  ASSERT_EQ(actual.size(), expected.size());
  std::size_t idx = 0;
  for (const CoordinateXY& exp : expected) {
    SCOPED_TRACE(idx);
    const CoordinateXY& act = actual.getAt<CoordinateXY>(idx);
    EXPECT_DOUBLE_EQ(act.x, exp.x);
    EXPECT_DOUBLE_EQ(act.y, exp.y);
    ++idx;
  }
}

TEST(PolygonNoderTest, TakesOwnershipOfInputRings) {
  auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  std::vector<std::unique_ptr<CoordinateSequence>> holes;
  holes.push_back(MakeRing({{2, 2}, {8, 2}, {8, 8}, {2, 8}, {2, 2}}));

  PolygonNoder noder(shell, holes);

  EXPECT_EQ(shell, nullptr);
  ASSERT_EQ(holes.size(), 1u);
  EXPECT_EQ(holes[0], nullptr);
}

TEST(PolygonNoderTest, NoHolesLeavesShellUnnoded) {
  auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  std::vector<std::unique_ptr<CoordinateSequence>> holes;

  PolygonNoder noder(shell, holes);
  noder.node();

  // Check noded status before calling getNodedShell(), which adds endpoint
  // nodes to the underlying SegmentNodeList.
  EXPECT_FALSE(noder.isShellNoded());
  EXPECT_TRUE(noder.getHolesTouching().empty());

  auto noded_shell = noder.getNodedShell();
  ASSERT_NE(noded_shell, nullptr);
  ExpectCoords2DEq(*noded_shell, {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
}

TEST(PolygonNoderTest, DisjointHoleLeavesAllRingsUnnoded) {
  auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  std::vector<std::unique_ptr<CoordinateSequence>> holes;
  holes.push_back(MakeRing({{2, 2}, {8, 2}, {8, 8}, {2, 8}, {2, 2}}));

  PolygonNoder noder(shell, holes);
  noder.node();

  EXPECT_FALSE(noder.isShellNoded());
  EXPECT_FALSE(noder.isHoleNoded(0));
  EXPECT_THAT(noder.getHolesTouching(), ElementsAre(false));

  auto noded_shell = noder.getNodedShell();
  auto noded_hole = noder.getNodedHole(0);
  ASSERT_NE(noded_shell, nullptr);
  ASSERT_NE(noded_hole, nullptr);
  ExpectCoords2DEq(*noded_shell, {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  ExpectCoords2DEq(*noded_hole, {{2, 2}, {8, 2}, {8, 8}, {2, 8}, {2, 2}});
}

TEST(PolygonNoderTest, HoleVertexTouchingShellEdgeNodesShell) {
  auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  std::vector<std::unique_ptr<CoordinateSequence>> holes;
  // Hole touches bottom shell edge (10,0)->(0,0) at interior point (5,0).
  holes.push_back(MakeRing({{5, 0}, {7, 4}, {3, 4}, {5, 0}}));

  PolygonNoder noder(shell, holes);
  noder.node();

  EXPECT_TRUE(noder.isShellNoded());
  EXPECT_FALSE(noder.isHoleNoded(0));
  EXPECT_THAT(noder.getHolesTouching(), ElementsAre(true));

  auto noded_shell = noder.getNodedShell();
  auto noded_hole = noder.getNodedHole(0);
  ASSERT_NE(noded_shell, nullptr);
  ASSERT_NE(noded_hole, nullptr);
  ExpectCoords2DEq(*noded_shell,
                   {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {5, 0}, {0, 0}});
  ExpectCoords2DEq(*noded_hole, {{5, 0}, {7, 4}, {3, 4}, {5, 0}});
}

TEST(PolygonNoderTest, ShellVertexTouchingHoleEdgeNodesHole) {
  // Reflex shell vertex (5,4) touches the interior of hole bottom edge
  // (2,4)->(8,4).
  auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {5, 4}, {0, 0}});
  std::vector<std::unique_ptr<CoordinateSequence>> holes;
  holes.push_back(MakeRing({{2, 4}, {8, 4}, {8, 8}, {2, 8}, {2, 4}}));

  PolygonNoder noder(shell, holes);
  noder.node();

  EXPECT_FALSE(noder.isShellNoded());
  EXPECT_TRUE(noder.isHoleNoded(0));
  EXPECT_THAT(noder.getHolesTouching(), ElementsAre(true));

  auto noded_shell = noder.getNodedShell();
  auto noded_hole = noder.getNodedHole(0);
  ASSERT_NE(noded_shell, nullptr);
  ASSERT_NE(noded_hole, nullptr);
  ExpectCoords2DEq(*noded_shell,
                   {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {5, 4}, {0, 0}});
  ExpectCoords2DEq(*noded_hole,
                   {{2, 4}, {5, 4}, {8, 4}, {8, 8}, {2, 8}, {2, 4}});
}

TEST(PolygonNoderTest, VertexToVertexTouchMarksTouchingWithoutAddingNodes) {
  auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  std::vector<std::unique_ptr<CoordinateSequence>> holes;
  holes.push_back(MakeRing({{0, 0}, {4, 2}, {2, 4}, {0, 0}}));

  PolygonNoder noder(shell, holes);
  noder.node();

  EXPECT_FALSE(noder.isShellNoded());
  EXPECT_FALSE(noder.isHoleNoded(0));
  EXPECT_THAT(noder.getHolesTouching(), ElementsAre(true));

  auto noded_shell = noder.getNodedShell();
  auto noded_hole = noder.getNodedHole(0);
  ASSERT_NE(noded_shell, nullptr);
  ASSERT_NE(noded_hole, nullptr);
  ExpectCoords2DEq(*noded_shell, {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  ExpectCoords2DEq(*noded_hole, {{0, 0}, {4, 2}, {2, 4}, {0, 0}});
}

TEST(PolygonNoderTest, MultipleNodesOnSingleShellEdgePreserveOrder) {
  auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  std::vector<std::unique_ptr<CoordinateSequence>> holes;
  // Hole touches bottom shell edge (10,0)->(0,0) at both (2,0) and (8,0).
  holes.push_back(MakeRing({{2, 0}, {5, 2}, {8, 0}, {9, 4}, {1, 4}, {2, 0}}));

  PolygonNoder noder(shell, holes);
  noder.node();

  EXPECT_TRUE(noder.isShellNoded());
  EXPECT_FALSE(noder.isHoleNoded(0));
  EXPECT_THAT(noder.getHolesTouching(), ElementsAre(true));

  auto noded_shell = noder.getNodedShell();
  ASSERT_NE(noded_shell, nullptr);
  ExpectCoords2DEq(
      *noded_shell,
      {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {8, 0}, {2, 0}, {0, 0}});
}

TEST(PolygonNoderTest, MultipleHolesTouchingEachOtherShellAndDisjoint) {
  auto shell = MakeRing({{0, 0}, {0, 20}, {20, 20}, {20, 0}, {0, 0}});
  std::vector<std::unique_ptr<CoordinateSequence>> holes;
  // Hole 0 touches shell bottom edge at (10,0).
  holes.push_back(MakeRing({{10, 0}, {12, 5}, {8, 5}, {10, 0}}));
  // Hole 1 touches hole 0 top edge (12,5)->(8,5) at (10,5).
  holes.push_back(MakeRing({{10, 5}, {12, 10}, {8, 10}, {10, 5}}));
  // Hole 2 is isolated.
  holes.push_back(MakeRing({{2, 12}, {4, 12}, {4, 14}, {2, 14}, {2, 12}}));
  // Hole 3 touches hole 1 top edge (12,10)->(8,10) at (10,10).
  holes.push_back(MakeRing({{10, 10}, {12, 14}, {8, 14}, {10, 10}}));

  PolygonNoder noder(shell, holes);
  noder.node();

  EXPECT_TRUE(noder.isShellNoded());
  EXPECT_TRUE(noder.isHoleNoded(0));
  EXPECT_TRUE(noder.isHoleNoded(1));
  EXPECT_FALSE(noder.isHoleNoded(2));
  EXPECT_FALSE(noder.isHoleNoded(3));
  EXPECT_THAT(noder.getHolesTouching(), ElementsAre(true, true, false, true));

  ExpectCoords2DEq(*noder.getNodedShell(),
                   {{0, 0}, {0, 20}, {20, 20}, {20, 0}, {10, 0}, {0, 0}});
  ExpectCoords2DEq(*noder.getNodedHole(0),
                   {{10, 0}, {12, 5}, {10, 5}, {8, 5}, {10, 0}});
  ExpectCoords2DEq(*noder.getNodedHole(1),
                   {{10, 5}, {12, 10}, {10, 10}, {8, 10}, {10, 5}});
  ExpectCoords2DEq(*noder.getNodedHole(2),
                   {{2, 12}, {4, 12}, {4, 14}, {2, 14}, {2, 12}});
  ExpectCoords2DEq(*noder.getNodedHole(3),
                   {{10, 10}, {12, 14}, {8, 14}, {10, 10}});
}

TEST(PolygonNoderTest, InvalidCollinearAndCrossingInputsDoNotThrow) {
  // Collinear overlap sharing shell edge (0,0)-(10,0) produces 2 intersection
  // endpoints for that segment pair (ignored by PolygonNoder), while endpoint
  // contacts trigger touching without throwing or noding interiors.
  {
    auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
    std::vector<std::unique_ptr<CoordinateSequence>> holes;
    holes.push_back(MakeRing({{0, 0}, {10, 0}, {5, 4}, {0, 0}}));

    PolygonNoder noder(shell, holes);
    EXPECT_NO_THROW(noder.node());
    EXPECT_FALSE(noder.isShellNoded());
    EXPECT_FALSE(noder.isHoleNoded(0));
    EXPECT_THAT(noder.getHolesTouching(), ElementsAre(true));
    EXPECT_NE(noder.getNodedShell(), nullptr);
    EXPECT_NE(noder.getNodedHole(0), nullptr);
  }

  // Properly crossing segments also complete without throwing.
  {
    auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
    std::vector<std::unique_ptr<CoordinateSequence>> holes;
    holes.push_back(MakeRing({{-2, 2}, {5, 2}, {5, 8}, {-2, 8}, {-2, 2}}));

    PolygonNoder noder(shell, holes);
    EXPECT_NO_THROW(noder.node());
    EXPECT_TRUE(noder.isShellNoded() || noder.isHoleNoded(0));
    EXPECT_THAT(noder.getHolesTouching(), ElementsAre(true));
    EXPECT_NE(noder.getNodedShell(), nullptr);
    EXPECT_NE(noder.getNodedHole(0), nullptr);
  }
}

TEST(PolygonNoderTest, QueryingBeforeNodeReportsUnnodedAndOriginalCoords) {
  auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  std::vector<std::unique_ptr<CoordinateSequence>> holes;
  holes.push_back(MakeRing({{5, 0}, {7, 4}, {3, 4}, {5, 0}}));

  PolygonNoder noder(shell, holes);

  EXPECT_FALSE(noder.isShellNoded());
  EXPECT_FALSE(noder.isHoleNoded(0));
  EXPECT_THAT(noder.getHolesTouching(), ElementsAre(false));

  auto noded_shell = noder.getNodedShell();
  auto noded_hole = noder.getNodedHole(0);
  ASSERT_NE(noded_shell, nullptr);
  ASSERT_NE(noded_hole, nullptr);
  ExpectCoords2DEq(*noded_shell, {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  ExpectCoords2DEq(*noded_hole, {{5, 0}, {7, 4}, {3, 4}, {5, 0}});
}

TEST(PolygonNoderTest, HoleTouchingTopShellEdgeNodesShell) {
  // Hole at the top edge has larger monotone chain Y-midpoint than the shell,
  // exercising the case where the shell segment string is ss0 and the hole is
  // ss1 in NodeAdder::processIntersections.
  auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  std::vector<std::unique_ptr<CoordinateSequence>> holes;
  holes.push_back(MakeRing({{5, 10}, {3, 6}, {7, 6}, {5, 10}}));

  PolygonNoder noder(shell, holes);
  noder.node();

  EXPECT_TRUE(noder.isShellNoded());
  EXPECT_FALSE(noder.isHoleNoded(0));
  EXPECT_THAT(noder.getHolesTouching(), ElementsAre(true));

  auto noded_shell = noder.getNodedShell();
  auto noded_hole = noder.getNodedHole(0);
  ASSERT_NE(noded_shell, nullptr);
  ASSERT_NE(noded_hole, nullptr);
  ExpectCoords2DEq(*noded_shell,
                   {{0, 0}, {0, 10}, {5, 10}, {10, 10}, {10, 0}, {0, 0}});
  ExpectCoords2DEq(*noded_hole, {{5, 10}, {3, 6}, {7, 6}, {5, 10}});
}

TEST(PolygonNoderTest, DisjointHoleWithOverlappingSegmentEnvelopeDoesNotNode) {
  // Diagonal shell edge (10,10)->(0,0) has envelope [0,10]x[0,10], which
  // overlaps all hole segment envelopes without intersecting them.
  auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {0, 0}});
  std::vector<std::unique_ptr<CoordinateSequence>> holes;
  holes.push_back(MakeRing({{1, 5}, {1, 8}, {4, 8}, {1, 5}}));

  PolygonNoder noder(shell, holes);
  noder.node();

  EXPECT_FALSE(noder.isShellNoded());
  EXPECT_FALSE(noder.isHoleNoded(0));
  EXPECT_THAT(noder.getHolesTouching(), ElementsAre(false));

  auto noded_shell = noder.getNodedShell();
  auto noded_hole = noder.getNodedHole(0);
  ASSERT_NE(noded_shell, nullptr);
  ASSERT_NE(noded_hole, nullptr);
  ExpectCoords2DEq(*noded_shell, {{0, 0}, {0, 10}, {10, 10}, {0, 0}});
  ExpectCoords2DEq(*noded_hole, {{1, 5}, {1, 8}, {4, 8}, {1, 5}});
}

TEST(PolygonNoderTest, HoleTouchingMultipleShellEdgesNodesAllTouchedEdges) {
  auto shell = MakeRing({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}});
  std::vector<std::unique_ptr<CoordinateSequence>> holes;
  holes.push_back(MakeRing({{5, 0}, {0, 5}, {5, 10}, {10, 5}, {5, 0}}));

  PolygonNoder noder(shell, holes);
  noder.node();

  EXPECT_TRUE(noder.isShellNoded());
  EXPECT_FALSE(noder.isHoleNoded(0));
  EXPECT_THAT(noder.getHolesTouching(), ElementsAre(true));

  auto noded_shell = noder.getNodedShell();
  auto noded_hole = noder.getNodedHole(0);
  ASSERT_NE(noded_shell, nullptr);
  ASSERT_NE(noded_hole, nullptr);
  ExpectCoords2DEq(*noded_shell, {{0, 0},
                                  {0, 5},
                                  {0, 10},
                                  {5, 10},
                                  {10, 10},
                                  {10, 5},
                                  {10, 0},
                                  {5, 0},
                                  {0, 0}});
  ExpectCoords2DEq(*noded_hole, {{5, 0}, {0, 5}, {5, 10}, {10, 5}, {5, 0}});
}

}  // namespace
}  // namespace geos_google
