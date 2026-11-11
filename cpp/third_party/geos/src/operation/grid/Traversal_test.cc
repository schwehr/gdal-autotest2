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

#include "third_party/geos/include/geos/operation/grid/Traversal.h"

#include <stdexcept>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/operation/grid/Side.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateXY;
using ::geos::operation::grid::Side;
using ::geos::operation::grid::Traversal;

// Note: geos::operation::grid::operator<<(ostream&, const Side&) is declared
// in Side.h without a library definition, so convert Side before GTest prints.
constexpr std::string_view SideName(Side s) {
  switch (s) {
    case Side::NONE:
      return "NONE";
    case Side::LEFT:
      return "LEFT";
    case Side::RIGHT:
      return "RIGHT";
    case Side::TOP:
      return "TOP";
    case Side::BOTTOM:
      return "BOTTOM";
  }
  return "UNKNOWN";
}

TEST(TraversalTest, DefaultConstructedState) {
  const Traversal t;

  EXPECT_TRUE(t.isEmpty());
  EXPECT_FALSE(t.isEntered());
  EXPECT_FALSE(t.isExited());
  EXPECT_FALSE(t.isTraversed());
  EXPECT_FALSE(t.isClosedRing());
  EXPECT_FALSE(t.hasMultipleUniqueCoordinates());
  EXPECT_EQ(SideName(t.getEntrySide()), "NONE");
  EXPECT_EQ(SideName(t.getExitSide()), "NONE");
  EXPECT_TRUE(t.getCoordinates().empty());
  EXPECT_THROW(t.getLastCoordinate(), std::out_of_range);
  EXPECT_THROW(t.getExitCoordinate(), std::runtime_error);
}

TEST(TraversalTest, EnterAddAndExitLifecycle) {
  Traversal t;
  const CoordinateXY entry(0.0, 2.0);
  const CoordinateXY mid(3.0, 5.0);
  const CoordinateXY exit_pt(6.0, 5.0);

  t.enter(entry, Side::LEFT);
  EXPECT_FALSE(t.isEmpty());
  EXPECT_TRUE(t.isEntered());
  EXPECT_FALSE(t.isExited());
  EXPECT_FALSE(t.isTraversed());
  EXPECT_FALSE(t.isClosedRing());
  EXPECT_FALSE(t.hasMultipleUniqueCoordinates());
  EXPECT_EQ(SideName(t.getEntrySide()), "LEFT");
  EXPECT_EQ(SideName(t.getExitSide()), "NONE");
  EXPECT_EQ(t.getLastCoordinate(), entry);
  EXPECT_THROW(t.getExitCoordinate(), std::runtime_error);

  t.add(mid);
  EXPECT_FALSE(t.isEmpty());
  EXPECT_TRUE(t.isEntered());
  EXPECT_FALSE(t.isExited());
  EXPECT_FALSE(t.isTraversed());
  EXPECT_FALSE(t.isClosedRing());
  EXPECT_TRUE(t.hasMultipleUniqueCoordinates());
  EXPECT_EQ(t.getLastCoordinate(), mid);
  EXPECT_THROW(t.getExitCoordinate(), std::runtime_error);

  t.exit(exit_pt, Side::RIGHT);
  EXPECT_TRUE(t.isEntered());
  EXPECT_TRUE(t.isExited());
  EXPECT_TRUE(t.isTraversed());
  EXPECT_FALSE(t.isClosedRing());
  EXPECT_TRUE(t.hasMultipleUniqueCoordinates());
  EXPECT_EQ(SideName(t.getEntrySide()), "LEFT");
  EXPECT_EQ(SideName(t.getExitSide()), "RIGHT");
  EXPECT_EQ(t.getLastCoordinate(), exit_pt);
  EXPECT_EQ(t.getExitCoordinate(), exit_pt);

  const std::vector<CoordinateXY> expected = {entry, mid, exit_pt};
  EXPECT_EQ(t.getCoordinates(), expected);
}

TEST(TraversalTest, EnterWhenNonEmptyThrows) {
  {
    Traversal t;
    t.enter(CoordinateXY(0.0, 1.0), Side::LEFT);
    EXPECT_THROW(t.enter(CoordinateXY(1.0, 1.0), Side::BOTTOM),
                 std::runtime_error);
    EXPECT_EQ(t.getCoordinates().size(), 1U);
    EXPECT_EQ(SideName(t.getEntrySide()), "LEFT");
  }

  {
    Traversal t;
    t.add(CoordinateXY(2.0, 2.0));
    EXPECT_THROW(t.enter(CoordinateXY(0.0, 2.0), Side::LEFT),
                 std::runtime_error);
    EXPECT_EQ(t.getCoordinates().size(), 1U);
    EXPECT_EQ(SideName(t.getEntrySide()), "NONE");
  }
}

TEST(TraversalTest, SideNoneAndForceExitBehavior) {
  // Entered with Side::NONE does not count as entered/traversed.
  {
    Traversal t;
    t.enter(CoordinateXY(2.0, 2.0), Side::NONE);
    EXPECT_FALSE(t.isEmpty());
    EXPECT_FALSE(t.isEntered());
    t.exit(CoordinateXY(5.0, 10.0), Side::TOP);
    EXPECT_TRUE(t.isExited());
    EXPECT_FALSE(t.isTraversed());
    EXPECT_EQ(t.getExitCoordinate(), CoordinateXY(5.0, 10.0));
  }

  // Exited without entry is not traversed.
  {
    Traversal t;
    t.add(CoordinateXY(1.0, 1.0));
    t.exit(CoordinateXY(10.0, 1.0), Side::RIGHT);
    EXPECT_FALSE(t.isEntered());
    EXPECT_TRUE(t.isExited());
    EXPECT_FALSE(t.isTraversed());
    EXPECT_EQ(t.getExitCoordinate(), CoordinateXY(10.0, 1.0));
  }

  // forceExit sets exit side without adding a coordinate, and Side::NONE clears
  // it.
  {
    Traversal t;
    t.enter(CoordinateXY(0.0, 3.0), Side::LEFT);
    t.add(CoordinateXY(4.0, 0.0));
    t.forceExit(Side::BOTTOM);
    EXPECT_TRUE(t.isExited());
    EXPECT_TRUE(t.isTraversed());
    EXPECT_EQ(SideName(t.getExitSide()), "BOTTOM");
    EXPECT_EQ(t.getExitCoordinate(), CoordinateXY(4.0, 0.0));
    EXPECT_EQ(t.getCoordinates().size(), 2U);

    t.forceExit(Side::NONE);
    EXPECT_FALSE(t.isExited());
    EXPECT_FALSE(t.isTraversed());
    EXPECT_THROW(t.getExitCoordinate(), std::runtime_error);
  }

  // forceExit on empty traversal makes isExited true, then getExitCoordinate
  // fails bounds.
  {
    Traversal t;
    t.forceExit(Side::TOP);
    EXPECT_TRUE(t.isExited());
    EXPECT_THROW(t.getExitCoordinate(), std::out_of_range);
  }
}

TEST(TraversalTest, IsClosedRingCases) {
  const CoordinateXY a(1.0, 1.0);
  const CoordinateXY b(5.0, 1.0);
  const CoordinateXY c(3.0, 4.0);
  const CoordinateXY d(1.0, 4.0);

  // Fewer than 3 coordinates: never a closed ring even when endpoints match.
  {
    Traversal t;
    EXPECT_FALSE(t.isClosedRing());
    t.add(a);
    EXPECT_FALSE(t.isClosedRing());
    t.add(a);
    EXPECT_FALSE(t.isClosedRing());
  }

  // 2 distinct coordinates
  {
    Traversal t;
    t.add(a);
    t.add(b);
    EXPECT_FALSE(t.isClosedRing());
  }

  // 3 coordinates closed (A -> B -> A)
  {
    Traversal t;
    t.add(a);
    t.add(b);
    t.add(a);
    EXPECT_TRUE(t.isClosedRing());
  }

  // 3 identical coordinates (A -> A -> A)
  {
    Traversal t;
    t.add(a);
    t.add(a);
    t.add(a);
    EXPECT_TRUE(t.isClosedRing());
  }

  // 3 coordinates open
  {
    Traversal t;
    t.add(a);
    t.add(b);
    t.add(c);
    EXPECT_FALSE(t.isClosedRing());
  }

  // 4 coordinates closed triangle
  {
    Traversal t;
    t.add(a);
    t.add(b);
    t.add(c);
    t.add(a);
    EXPECT_TRUE(t.isClosedRing());
  }

  // 4 coordinates open polyline
  {
    Traversal t;
    t.add(a);
    t.add(b);
    t.add(c);
    t.add(d);
    EXPECT_FALSE(t.isClosedRing());
  }
}

TEST(TraversalTest, HasMultipleUniqueCoordinatesCases) {
  const CoordinateXY a(2.0, 3.0);
  const CoordinateXY b(4.0, 3.0);
  const CoordinateXY c(4.0, 7.0);

  {
    Traversal t;
    EXPECT_FALSE(t.hasMultipleUniqueCoordinates());
    t.add(a);
    EXPECT_FALSE(t.hasMultipleUniqueCoordinates());
    t.add(a);
    EXPECT_FALSE(t.hasMultipleUniqueCoordinates());
    t.add(a);
    EXPECT_FALSE(t.hasMultipleUniqueCoordinates());
  }

  {
    Traversal t;
    t.add(a);
    t.add(b);
    EXPECT_TRUE(t.hasMultipleUniqueCoordinates());
  }

  {
    Traversal t;
    t.add(a);
    t.add(a);
    t.add(b);
    EXPECT_TRUE(t.hasMultipleUniqueCoordinates());
  }

  {
    Traversal t;
    t.add(a);
    t.add(b);
    t.add(c);
    t.add(a);
    EXPECT_TRUE(t.hasMultipleUniqueCoordinates());
  }
}

}  // namespace
}  // namespace geos_google
