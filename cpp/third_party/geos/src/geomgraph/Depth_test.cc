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

// Unit tests for third_party/geos/src/geomgraph/Depth.cpp and
// third_party/geos/include/geos/geomgraph/Depth.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geomgraph/Depth.cpp

#include "third_party/geos/include/geos/geomgraph/Depth.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"

namespace geos_google {
namespace {

using geos::geom::Location;
using geos::geom::Position;
using geos::geomgraph::Depth;
using geos::geomgraph::Label;

constexpr int kPosOn = Position::ON;
constexpr int kPosLeft = Position::LEFT;
constexpr int kPosRight = Position::RIGHT;

TEST(DepthTest, DepthAtLocationMapsAllValidLocations) {
  struct LocationDepthCase {
    std::string_view name;
    Location loc;
    int expected;
  };
  constexpr auto kCases = std::to_array<LocationDepthCase>({
      {.name = "exterior", .loc = Location::EXTERIOR, .expected = 0},
      {.name = "interior", .loc = Location::INTERIOR, .expected = 1},
      {.name = "boundary", .loc = Location::BOUNDARY, .expected = -1},
      {.name = "none", .loc = Location::NONE, .expected = -1},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(Depth::depthAtLocation(tc.loc), tc.expected);
  }
}

TEST(DepthTest, DefaultConstructedDepthIsNullAndExterior) {
  const Depth depth;
  EXPECT_TRUE(depth.isNull());
  for (uint8_t geom_index = 0; geom_index < 2; ++geom_index) {
    EXPECT_TRUE(depth.isNull(geom_index));
    EXPECT_EQ(depth.getDelta(geom_index), 0);
    for (uint8_t pos_index = 0; pos_index < 3; ++pos_index) {
      EXPECT_TRUE(depth.isNull(geom_index, pos_index));
      EXPECT_EQ(depth.getDepth(geom_index, pos_index), -1);
      EXPECT_EQ(depth.getLocation(geom_index, pos_index), Location::EXTERIOR);
    }
  }
  EXPECT_EQ(depth.toString(), "A:-1,-1 B:-1,-1]");
}

TEST(DepthTest, SetGetAndIsNullTrackIndividualPositions) {
  Depth depth;

  // Setting Position::ON clears global isNull() without clearing isNull(geom),
  // because isNull(uint8_t geomIndex) checks Position::LEFT (index 1).
  depth.setDepth(0, kPosOn, 0);
  EXPECT_FALSE(depth.isNull());
  EXPECT_FALSE(depth.isNull(0, kPosOn));
  EXPECT_TRUE(depth.isNull(0));
  EXPECT_TRUE(depth.isNull(1));
  EXPECT_EQ(depth.getDepth(0, kPosOn), 0);

  // Setting Position::RIGHT on geom 1 also leaves isNull(1) true.
  depth.setDepth(1, kPosRight, 2);
  EXPECT_FALSE(depth.isNull(1, kPosRight));
  EXPECT_TRUE(depth.isNull(1));
  EXPECT_EQ(depth.getDepth(1, kPosRight), 2);

  // Setting Position::LEFT clears isNull(geomIndex).
  depth.setDepth(0, kPosLeft, 1);
  depth.setDepth(1, kPosLeft, 0);
  EXPECT_FALSE(depth.isNull(0));
  EXPECT_FALSE(depth.isNull(1));
  EXPECT_EQ(depth.getDepth(0, kPosLeft), 1);
  EXPECT_EQ(depth.getDepth(1, kPosLeft), 0);

  // Restoring all positions to sentinel -1 makes isNull() true again.
  depth.setDepth(0, kPosOn, -1);
  depth.setDepth(0, kPosLeft, -1);
  depth.setDepth(1, kPosLeft, -1);
  depth.setDepth(1, kPosRight, -1);
  EXPECT_TRUE(depth.isNull());
}

TEST(DepthTest, GetLocationMapsNonPositiveToExteriorAndPositiveToInterior) {
  struct LocationMappingCase {
    int depth_value;
    Location expected;
  };
  constexpr auto kCases = std::to_array<LocationMappingCase>({
      {.depth_value = -5, .expected = Location::EXTERIOR},
      {.depth_value = -1, .expected = Location::EXTERIOR},
      {.depth_value = 0, .expected = Location::EXTERIOR},
      {.depth_value = 1, .expected = Location::INTERIOR},
      {.depth_value = 2, .expected = Location::INTERIOR},
      {.depth_value = 100, .expected = Location::INTERIOR},
  });

  Depth depth;
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.depth_value);
    for (int geom_index = 0; geom_index < 2; ++geom_index) {
      for (int pos_index = 0; pos_index < 3; ++pos_index) {
        depth.setDepth(geom_index, pos_index, tc.depth_value);
        EXPECT_EQ(depth.getLocation(geom_index, pos_index), tc.expected);
      }
    }
  }
}

TEST(DepthTest, GetDeltaComputesRightMinusLeft) {
  struct DeltaCase {
    std::string_view name;
    int left;
    int right;
    int expected_delta;
  };
  constexpr auto kCases = std::to_array<DeltaCase>({
      {.name = "increasing_0_to_1", .left = 0, .right = 1, .expected_delta = 1},
      {.name = "decreasing_1_to_0",
       .left = 1,
       .right = 0,
       .expected_delta = -1},
      {.name = "equal_depths", .left = 3, .right = 3, .expected_delta = 0},
      {.name = "multi_step_positive",
       .left = 2,
       .right = 5,
       .expected_delta = 3},
      {.name = "negative_left", .left = -2, .right = 3, .expected_delta = 5},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    Depth depth;
    for (int geom_index = 0; geom_index < 2; ++geom_index) {
      depth.setDepth(geom_index, kPosOn, 99);
      depth.setDepth(geom_index, kPosLeft, tc.left);
      depth.setDepth(geom_index, kPosRight, tc.right);
      EXPECT_EQ(depth.getDelta(geom_index), tc.expected_delta);
    }
  }
}

TEST(DepthTest, AddLocationIncrementsOnlyForInterior) {
  Depth depth;

  depth.add(0, kPosLeft, Location::EXTERIOR);
  EXPECT_EQ(depth.getDepth(0, kPosLeft), -1);

  depth.add(0, kPosLeft, Location::BOUNDARY);
  EXPECT_EQ(depth.getDepth(0, kPosLeft), -1);

  depth.add(0, kPosLeft, Location::NONE);
  EXPECT_EQ(depth.getDepth(0, kPosLeft), -1);

  depth.add(0, kPosLeft, Location::INTERIOR);
  EXPECT_EQ(depth.getDepth(0, kPosLeft), 0);

  depth.add(0, kPosLeft, Location::INTERIOR);
  EXPECT_EQ(depth.getDepth(0, kPosLeft), 1);

  depth.add(0, kPosLeft, Location::EXTERIOR);
  EXPECT_EQ(depth.getDepth(0, kPosLeft), 1);

  depth.setDepth(1, kPosRight, 3);
  depth.add(1, kPosRight, Location::INTERIOR);
  EXPECT_EQ(depth.getDepth(1, kPosRight), 4);
}

TEST(DepthTest, AddLabelInitializesNullSidesAndAccumulatesSubsequentLabels) {
  Depth depth;

  // Initial area label sets LEFT/RIGHT for both geometries while leaving ON
  // null.
  const Label first(Location::INTERIOR, Location::INTERIOR, Location::EXTERIOR);
  depth.add(first);
  EXPECT_EQ(depth.getDepth(0, kPosOn), -1);
  EXPECT_EQ(depth.getDepth(0, kPosLeft), 1);
  EXPECT_EQ(depth.getDepth(0, kPosRight), 0);
  EXPECT_EQ(depth.getDepth(1, kPosOn), -1);
  EXPECT_EQ(depth.getDepth(1, kPosLeft), 1);
  EXPECT_EQ(depth.getDepth(1, kPosRight), 0);

  // Second label adds EXTERIOR (0) and INTERIOR (+1) to existing values.
  Label second(0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR);
  second.setLocation(1, kPosLeft, Location::INTERIOR);
  second.setLocation(1, kPosRight, Location::EXTERIOR);
  depth.add(second);
  EXPECT_EQ(depth.getDepth(0, kPosLeft), 1);
  EXPECT_EQ(depth.getDepth(0, kPosRight), 1);
  EXPECT_EQ(depth.getDepth(1, kPosLeft), 2);
  EXPECT_EQ(depth.getDepth(1, kPosRight), 0);
  EXPECT_EQ(depth.getDepth(0, kPosOn), -1);
  EXPECT_EQ(depth.getDepth(1, kPosOn), -1);
}

TEST(DepthTest, AddLabelIgnoresLineBoundaryAndNoneLocations) {
  Depth depth;

  // Line label has Location::NONE at LEFT and RIGHT.
  depth.add(Label(Location::INTERIOR));
  EXPECT_TRUE(depth.isNull());

  // Boundary locations on LEFT and RIGHT are also ignored.
  depth.add(Label(Location::BOUNDARY, Location::BOUNDARY, Location::BOUNDARY));
  EXPECT_TRUE(depth.isNull());

  // Mixed label: only INTERIOR/EXTERIOR sides affect depth.
  Label mixed(0, Location::BOUNDARY, Location::INTERIOR, Location::BOUNDARY);
  mixed.setLocation(1, kPosLeft, Location::NONE);
  mixed.setLocation(1, kPosRight, Location::EXTERIOR);
  depth.add(mixed);

  EXPECT_EQ(depth.getDepth(0, kPosLeft), 1);
  EXPECT_EQ(depth.getDepth(0, kPosRight), -1);
  EXPECT_EQ(depth.getDepth(1, kPosLeft), -1);
  EXPECT_EQ(depth.getDepth(1, kPosRight), 0);
}

TEST(DepthTest, NormalizeReducesEachNonNullGeometryToZeroOne) {
  struct NormalizeCase {
    std::string_view name;
    int left_in;
    int right_in;
    int left_out;
    int right_out;
  };
  constexpr auto kCases = std::to_array<NormalizeCase>({
      {.name = "both_zero",
       .left_in = 0,
       .right_in = 0,
       .left_out = 0,
       .right_out = 0},
      {.name = "equal_positive",
       .left_in = 2,
       .right_in = 2,
       .left_out = 0,
       .right_out = 0},
      {.name = "left_min_zero",
       .left_in = 0,
       .right_in = 1,
       .left_out = 0,
       .right_out = 1},
      {.name = "right_min_zero",
       .left_in = 1,
       .right_in = 0,
       .left_out = 1,
       .right_out = 0},
      {.name = "left_min_positive",
       .left_in = 2,
       .right_in = 5,
       .left_out = 0,
       .right_out = 1},
      {.name = "right_min_positive",
       .left_in = 5,
       .right_in = 2,
       .left_out = 1,
       .right_out = 0},
      {.name = "right_negative_clamps_min_to_zero",
       .left_in = 2,
       .right_in = -1,
       .left_out = 1,
       .right_out = 0},
      {.name = "zero_and_negative_clamps_to_zero",
       .left_in = 0,
       .right_in = -3,
       .left_out = 0,
       .right_out = 0},
      {.name = "left_negative_clamps_min_to_zero",
       .left_in = -2,
       .right_in = 3,
       .left_out = 0,
       .right_out = 1},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    Depth depth;
    depth.setDepth(0, kPosOn, 7);
    depth.setDepth(0, kPosLeft, tc.left_in);
    depth.setDepth(0, kPosRight, tc.right_in);
    depth.setDepth(1, kPosOn, 8);
    depth.setDepth(1, kPosLeft, tc.left_in);
    depth.setDepth(1, kPosRight, tc.right_in);

    depth.normalize();

    EXPECT_EQ(depth.getDepth(0, kPosLeft), tc.left_out);
    EXPECT_EQ(depth.getDepth(0, kPosRight), tc.right_out);
    EXPECT_EQ(depth.getDepth(0, kPosOn), 7);
    EXPECT_EQ(depth.getDepth(1, kPosLeft), tc.left_out);
    EXPECT_EQ(depth.getDepth(1, kPosRight), tc.right_out);
    EXPECT_EQ(depth.getDepth(1, kPosOn), 8);
  }
}

TEST(DepthTest, NormalizeSkipsNullGeometries) {
  // Both geometries null.
  Depth both_null;
  both_null.normalize();
  EXPECT_TRUE(both_null.isNull());

  // Only geom 0 non-null; geom 1 LEFT is null while RIGHT is non-null.
  Depth only_geom0;
  only_geom0.setDepth(0, kPosLeft, 1);
  only_geom0.setDepth(0, kPosRight, 2);
  only_geom0.setDepth(1, kPosRight, 5);
  only_geom0.normalize();
  EXPECT_EQ(only_geom0.getDepth(0, kPosLeft), 0);
  EXPECT_EQ(only_geom0.getDepth(0, kPosRight), 1);
  EXPECT_EQ(only_geom0.getDepth(1, kPosLeft), -1);
  EXPECT_EQ(only_geom0.getDepth(1, kPosRight), 5);

  // Only geom 1 non-null.
  Depth only_geom1;
  only_geom1.setDepth(1, kPosLeft, 3);
  only_geom1.setDepth(1, kPosRight, 1);
  only_geom1.normalize();
  EXPECT_TRUE(only_geom1.isNull(0));
  EXPECT_EQ(only_geom1.getDepth(1, kPosLeft), 1);
  EXPECT_EQ(only_geom1.getDepth(1, kPosRight), 0);
}

TEST(DepthTest, ToStringFormatsSidesForBothGeometries) {
  Depth depth;
  EXPECT_EQ(depth.toString(), "A:-1,-1 B:-1,-1]");

  depth.setDepth(0, kPosOn, 9);
  depth.setDepth(0, kPosLeft, 1);
  depth.setDepth(0, kPosRight, 0);
  depth.setDepth(1, kPosLeft, 2);
  depth.setDepth(1, kPosRight, 3);

  const std::string formatted = depth.toString();
  EXPECT_EQ(formatted, "A:1,0 B:2,3]");

  depth.normalize();
  EXPECT_EQ(depth.toString(), "A:1,0 B:0,1]");
}

}  // namespace
}  // namespace geos_google
