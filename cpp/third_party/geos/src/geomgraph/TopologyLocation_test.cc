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

// Unit tests for third_party/geos/src/geomgraph/TopologyLocation.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geomgraph/TopologyLocation.cpp

#include "third_party/geos/include/geos/geomgraph/TopologyLocation.h"

#include <array>
#include <sstream>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"

namespace geos_google {
namespace {

using geos::geom::Location;
using geos::geom::Position;
using geos::geomgraph::TopologyLocation;
using ::testing::ElementsAre;

struct LineLocationCase {
  std::string_view name;
  Location loc;
  bool expected_is_null;
  std::string_view expected_text;
};

constexpr auto kLineLocationCases = std::to_array<LineLocationCase>({
    {
        .name = "none",
        .loc = Location::NONE,
        .expected_is_null = true,
        .expected_text = "-",
    },
    {
        .name = "interior",
        .loc = Location::INTERIOR,
        .expected_is_null = false,
        .expected_text = "i",
    },
    {
        .name = "boundary",
        .loc = Location::BOUNDARY,
        .expected_is_null = false,
        .expected_text = "b",
    },
    {
        .name = "exterior",
        .loc = Location::EXTERIOR,
        .expected_is_null = false,
        .expected_text = "e",
    },
});

TEST(TopologyLocationTest, LineConstructorAndAccessors) {
  for (const auto& tc : kLineLocationCases) {
    SCOPED_TRACE(tc.name);
    const TopologyLocation tl(tc.loc);

    EXPECT_TRUE(tl.isLine());
    EXPECT_FALSE(tl.isArea());
    EXPECT_EQ(tl.get(Position::ON), tc.loc);
    EXPECT_EQ(tl.get(Position::LEFT), Location::NONE);
    EXPECT_EQ(tl.get(Position::RIGHT), Location::NONE);
    EXPECT_EQ(tl.get(99), Location::NONE);
    EXPECT_EQ(tl.isNull(), tc.expected_is_null);
    EXPECT_EQ(tl.isAnyNull(), tc.expected_is_null);
    EXPECT_TRUE(tl.allPositionsEqual(tc.loc));
    EXPECT_EQ(tl.toString(), tc.expected_text);
  }
}

TEST(TopologyLocationTest, AreaConstructorAndAccessors) {
  const TopologyLocation tl(Location::BOUNDARY, Location::INTERIOR,
                            Location::EXTERIOR);

  EXPECT_TRUE(tl.isArea());
  EXPECT_FALSE(tl.isLine());
  EXPECT_EQ(tl.get(Position::ON), Location::BOUNDARY);
  EXPECT_EQ(tl.get(Position::LEFT), Location::INTERIOR);
  EXPECT_EQ(tl.get(Position::RIGHT), Location::EXTERIOR);
  EXPECT_EQ(tl.get(3), Location::NONE);
  EXPECT_FALSE(tl.isNull());
  EXPECT_FALSE(tl.isAnyNull());
  EXPECT_FALSE(tl.allPositionsEqual(Location::BOUNDARY));
  EXPECT_THAT(
      tl.getLocations(),
      ElementsAre(Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  EXPECT_EQ(tl.toString(), "ibe");

  const TopologyLocation all_none(Location::NONE, Location::NONE,
                                  Location::NONE);
  EXPECT_TRUE(all_none.isNull());
  EXPECT_TRUE(all_none.isAnyNull());
  EXPECT_TRUE(all_none.allPositionsEqual(Location::NONE));
  EXPECT_EQ(all_none.toString(), "---");

  const TopologyLocation partial_none(Location::BOUNDARY, Location::NONE,
                                      Location::EXTERIOR);
  EXPECT_FALSE(partial_none.isNull());
  EXPECT_TRUE(partial_none.isAnyNull());
  EXPECT_EQ(partial_none.toString(), "-be");

  const TopologyLocation all_interior(Location::INTERIOR, Location::INTERIOR,
                                      Location::INTERIOR);
  EXPECT_TRUE(all_interior.allPositionsEqual(Location::INTERIOR));
  EXPECT_EQ(all_interior.toString(), "iii");
}

TEST(TopologyLocationTest, CopyConstructorAndAssignmentAreIndependent) {
  TopologyLocation original(Location::BOUNDARY, Location::INTERIOR,
                            Location::EXTERIOR);
  const TopologyLocation copy_constructed(original);

  TopologyLocation copy_assigned(Location::NONE);
  copy_assigned = original;

  original.setLocations(Location::EXTERIOR, Location::EXTERIOR,
                        Location::EXTERIOR);

  EXPECT_EQ(copy_constructed.toString(), "ibe");
  EXPECT_EQ(copy_assigned.toString(), "ibe");
  EXPECT_TRUE(copy_assigned.isArea());
}

TEST(TopologyLocationTest, FlipSwapsAreaSidesAndLeavesLineUnchanged) {
  TopologyLocation line(Location::BOUNDARY);
  line.flip();
  EXPECT_TRUE(line.isLine());
  EXPECT_EQ(line.get(Position::ON), Location::BOUNDARY);
  EXPECT_EQ(line.toString(), "b");

  TopologyLocation area(Location::BOUNDARY, Location::INTERIOR,
                        Location::EXTERIOR);
  area.flip();
  EXPECT_EQ(area.get(Position::ON), Location::BOUNDARY);
  EXPECT_EQ(area.get(Position::LEFT), Location::EXTERIOR);
  EXPECT_EQ(area.get(Position::RIGHT), Location::INTERIOR);
  EXPECT_EQ(area.toString(), "ebi");

  area.flip();
  EXPECT_EQ(area.toString(), "ibe");
}

TEST(TopologyLocationTest, MutatorsSetLocationsAndFillNulls) {
  TopologyLocation line(Location::NONE);
  line.setAllLocationsIfNull(Location::INTERIOR);
  EXPECT_EQ(line.toString(), "i");
  line.setAllLocationsIfNull(Location::EXTERIOR);
  EXPECT_EQ(line.toString(), "i");

  line.setLocation(Location::BOUNDARY);
  EXPECT_EQ(line.get(Position::ON), Location::BOUNDARY);
  line.setAllLocations(Location::EXTERIOR);
  EXPECT_TRUE(line.allPositionsEqual(Location::EXTERIOR));

  TopologyLocation area(Location::BOUNDARY, Location::NONE, Location::INTERIOR);
  area.setAllLocationsIfNull(Location::EXTERIOR);
  EXPECT_EQ(area.toString(), "ebi");

  area.setLocation(Position::LEFT, Location::INTERIOR);
  EXPECT_EQ(area.toString(), "ibi");

  area.setLocations(Location::INTERIOR, Location::EXTERIOR, Location::BOUNDARY);
  EXPECT_EQ(area.toString(), "eib");

  area.setAllLocations(Location::BOUNDARY);
  EXPECT_TRUE(area.allPositionsEqual(Location::BOUNDARY));
  EXPECT_EQ(area.toString(), "bbb");
}

TEST(TopologyLocationTest, IsEqualOnSideComparesSpecifiedPosition) {
  const TopologyLocation a(Location::BOUNDARY, Location::INTERIOR,
                           Location::EXTERIOR);
  const TopologyLocation b(Location::BOUNDARY, Location::EXTERIOR,
                           Location::EXTERIOR);

  EXPECT_TRUE(a.isEqualOnSide(b, Position::ON));
  EXPECT_FALSE(a.isEqualOnSide(b, Position::LEFT));
  EXPECT_TRUE(a.isEqualOnSide(b, Position::RIGHT));
}

TEST(TopologyLocationTest, MergeLineWithLineUpdatesOnlyNullLocations) {
  TopologyLocation null_line(Location::NONE);
  const TopologyLocation interior_line(Location::INTERIOR);
  null_line.merge(interior_line);
  EXPECT_TRUE(null_line.isLine());
  EXPECT_EQ(null_line.get(Position::ON), Location::INTERIOR);
  EXPECT_EQ(null_line.toString(), "i");

  TopologyLocation boundary_line(Location::BOUNDARY);
  boundary_line.merge(interior_line);
  EXPECT_TRUE(boundary_line.isLine());
  EXPECT_EQ(boundary_line.get(Position::ON), Location::BOUNDARY);
  EXPECT_EQ(boundary_line.toString(), "b");
}

TEST(TopologyLocationTest, MergeLineWithAreaPromotesDestinationToArea) {
  TopologyLocation null_line(Location::NONE);
  const TopologyLocation area(Location::BOUNDARY, Location::INTERIOR,
                              Location::EXTERIOR);
  null_line.merge(area);
  EXPECT_TRUE(null_line.isArea());
  EXPECT_EQ(null_line.get(Position::ON), Location::BOUNDARY);
  EXPECT_EQ(null_line.get(Position::LEFT), Location::INTERIOR);
  EXPECT_EQ(null_line.get(Position::RIGHT), Location::EXTERIOR);
  EXPECT_EQ(null_line.toString(), "ibe");

  TopologyLocation on_interior(Location::INTERIOR);
  const TopologyLocation partial_area(Location::BOUNDARY, Location::NONE,
                                      Location::EXTERIOR);
  on_interior.merge(partial_area);
  EXPECT_TRUE(on_interior.isArea());
  EXPECT_EQ(on_interior.get(Position::ON), Location::INTERIOR);
  EXPECT_EQ(on_interior.get(Position::LEFT), Location::NONE);
  EXPECT_EQ(on_interior.get(Position::RIGHT), Location::EXTERIOR);
  EXPECT_EQ(on_interior.toString(), "-ie");
}

TEST(TopologyLocationTest, MergeAreaWithLinePreservesSideLocations) {
  TopologyLocation area_null_on(Location::NONE, Location::INTERIOR,
                                Location::EXTERIOR);
  const TopologyLocation line_boundary(Location::BOUNDARY);
  area_null_on.merge(line_boundary);
  EXPECT_TRUE(area_null_on.isArea());
  EXPECT_EQ(area_null_on.toString(), "ibe");

  TopologyLocation area_set_on(Location::INTERIOR, Location::NONE,
                               Location::EXTERIOR);
  area_set_on.merge(line_boundary);
  EXPECT_TRUE(area_set_on.isArea());
  EXPECT_EQ(area_set_on.toString(), "-ie");
}

TEST(TopologyLocationTest, MergeAreaWithAreaFillsNullPositions) {
  TopologyLocation dest(Location::NONE, Location::EXTERIOR, Location::NONE);
  const TopologyLocation src(Location::BOUNDARY, Location::INTERIOR,
                             Location::EXTERIOR);
  dest.merge(src);

  EXPECT_EQ(dest.get(Position::ON), Location::BOUNDARY);
  EXPECT_EQ(dest.get(Position::LEFT), Location::EXTERIOR);
  EXPECT_EQ(dest.get(Position::RIGHT), Location::EXTERIOR);
  EXPECT_EQ(dest.toString(), "ebe");
}

TEST(TopologyLocationTest, StreamInsertionFormatsLineAndArea) {
  const TopologyLocation line(Location::BOUNDARY);
  const TopologyLocation area(Location::INTERIOR, Location::NONE,
                              Location::EXTERIOR);
  std::ostringstream os;
  os << line << '|' << area;
  EXPECT_EQ(os.str(), "b|-ie");
}

}  // namespace
}  // namespace geos_google
