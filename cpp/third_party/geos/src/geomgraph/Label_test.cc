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

// Unit tests for third_party/geos/src/geomgraph/Label.cpp and
// third_party/geos/include/geos/geomgraph/Label.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geomgraph/Label.cpp

#include "third_party/geos/include/geos/geomgraph/Label.h"

#include <array>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"

namespace geos_google {
namespace {

using geos::geom::Location;
using geos::geom::Position;
using geos::geomgraph::Label;

TEST(LabelTest, DefaultConstructorCreatesNullLineLabel) {
  const Label label;
  EXPECT_TRUE(label.isNull());
  EXPECT_TRUE(label.isNull(0));
  EXPECT_TRUE(label.isNull(1));
  EXPECT_TRUE(label.isAnyNull(0));
  EXPECT_TRUE(label.isAnyNull(1));
  EXPECT_FALSE(label.isArea());
  EXPECT_FALSE(label.isArea(0));
  EXPECT_FALSE(label.isArea(1));
  EXPECT_TRUE(label.isLine(0));
  EXPECT_TRUE(label.isLine(1));
  EXPECT_EQ(label.getGeometryCount(), 0);
  EXPECT_EQ(label.getLocation(0), Location::NONE);
  EXPECT_EQ(label.getLocation(1), Location::NONE);
  EXPECT_TRUE(label.allPositionsEqual(0, Location::NONE));
  EXPECT_TRUE(label.allPositionsEqual(1, Location::NONE));
  EXPECT_EQ(label.toString(), "A:- B:-");
}

TEST(LabelTest, SingleLocationConstructorAppliesToBothGeometries) {
  struct LocationCase {
    std::string_view name;
    Location loc;
    bool expected_null;
    int expected_geom_count;
    std::string_view expected_str;
  };
  constexpr auto kCases = std::to_array<LocationCase>({
      {
          .name = "none",
          .loc = Location::NONE,
          .expected_null = true,
          .expected_geom_count = 0,
          .expected_str = "A:- B:-",
      },
      {
          .name = "interior",
          .loc = Location::INTERIOR,
          .expected_null = false,
          .expected_geom_count = 2,
          .expected_str = "A:i B:i",
      },
      {
          .name = "boundary",
          .loc = Location::BOUNDARY,
          .expected_null = false,
          .expected_geom_count = 2,
          .expected_str = "A:b B:b",
      },
      {
          .name = "exterior",
          .loc = Location::EXTERIOR,
          .expected_null = false,
          .expected_geom_count = 2,
          .expected_str = "A:e B:e",
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const Label label(tc.loc);
    EXPECT_EQ(label.isNull(), tc.expected_null);
    EXPECT_EQ(label.isNull(0), tc.expected_null);
    EXPECT_EQ(label.isNull(1), tc.expected_null);
    EXPECT_EQ(label.getGeometryCount(), tc.expected_geom_count);
    EXPECT_EQ(label.getLocation(0), tc.loc);
    EXPECT_EQ(label.getLocation(1), tc.loc);
    EXPECT_EQ(label.getLocation(0, Position::ON), tc.loc);
    EXPECT_EQ(label.getLocation(1, Position::ON), tc.loc);
    EXPECT_TRUE(label.isLine(0));
    EXPECT_TRUE(label.isLine(1));
    EXPECT_FALSE(label.isArea());
    EXPECT_EQ(label.toString(), tc.expected_str);
  }
}

TEST(LabelTest, IndexedSingleLocationConstructorInitializesTargetOnly) {
  const Label geom0(0, Location::BOUNDARY);
  EXPECT_FALSE(geom0.isNull());
  EXPECT_FALSE(geom0.isNull(0));
  EXPECT_TRUE(geom0.isNull(1));
  EXPECT_EQ(geom0.getGeometryCount(), 1);
  EXPECT_EQ(geom0.getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(geom0.getLocation(1), Location::NONE);
  EXPECT_TRUE(geom0.isLine(0));
  EXPECT_TRUE(geom0.isLine(1));
  EXPECT_EQ(geom0.toString(), "A:b B:-");

  const Label geom1(1, Location::EXTERIOR);
  EXPECT_FALSE(geom1.isNull());
  EXPECT_TRUE(geom1.isNull(0));
  EXPECT_FALSE(geom1.isNull(1));
  EXPECT_EQ(geom1.getGeometryCount(), 1);
  EXPECT_EQ(geom1.getLocation(0), Location::NONE);
  EXPECT_EQ(geom1.getLocation(1), Location::EXTERIOR);
  EXPECT_EQ(geom1.toString(), "A:- B:e");

  const Label none_geom(0, Location::NONE);
  EXPECT_TRUE(none_geom.isNull());
  EXPECT_EQ(none_geom.getGeometryCount(), 0);
  EXPECT_EQ(none_geom.toString(), "A:- B:-");
}

TEST(LabelTest, AreaConstructorSetsOnLeftRightForBothGeometries) {
  const Label label(Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR);
  EXPECT_TRUE(label.isArea());
  EXPECT_TRUE(label.isArea(0));
  EXPECT_TRUE(label.isArea(1));
  EXPECT_FALSE(label.isLine(0));
  EXPECT_FALSE(label.isLine(1));
  EXPECT_FALSE(label.isNull());
  EXPECT_FALSE(label.isAnyNull(0));
  EXPECT_FALSE(label.isAnyNull(1));
  EXPECT_EQ(label.getGeometryCount(), 2);

  for (uint32_t i = 0; i < 2; ++i) {
    EXPECT_EQ(label.getLocation(i), Location::BOUNDARY);
    EXPECT_EQ(label.getLocation(i, Position::ON), Location::BOUNDARY);
    EXPECT_EQ(label.getLocation(i, Position::LEFT), Location::INTERIOR);
    EXPECT_EQ(label.getLocation(i, Position::RIGHT), Location::EXTERIOR);
  }
  EXPECT_EQ(label.toString(), "A:ibe B:ibe");

  const Label null_area(Location::NONE, Location::NONE, Location::NONE);
  EXPECT_TRUE(null_area.isArea());
  EXPECT_TRUE(null_area.isArea(0));
  EXPECT_TRUE(null_area.isArea(1));
  EXPECT_TRUE(null_area.isNull());
  EXPECT_TRUE(null_area.isAnyNull(0));
  EXPECT_TRUE(null_area.isAnyNull(1));
  EXPECT_EQ(null_area.getGeometryCount(), 0);
  EXPECT_TRUE(null_area.allPositionsEqual(0, Location::NONE));
  EXPECT_TRUE(null_area.allPositionsEqual(1, Location::NONE));
  EXPECT_EQ(null_area.toString(), "A:--- B:---");
}

TEST(LabelTest, IndexedAreaConstructorLeavesOtherGeometryNullArea) {
  const Label geom0(0, Location::BOUNDARY, Location::INTERIOR,
                    Location::EXTERIOR);
  EXPECT_TRUE(geom0.isArea());
  EXPECT_TRUE(geom0.isArea(0));
  EXPECT_TRUE(geom0.isArea(1));
  EXPECT_FALSE(geom0.isNull());
  EXPECT_FALSE(geom0.isNull(0));
  EXPECT_TRUE(geom0.isNull(1));
  EXPECT_FALSE(geom0.isAnyNull(0));
  EXPECT_TRUE(geom0.isAnyNull(1));
  EXPECT_EQ(geom0.getGeometryCount(), 1);
  EXPECT_EQ(geom0.toString(), "A:ibe B:---");

  const Label geom1(1, Location::INTERIOR, Location::NONE, Location::EXTERIOR);
  EXPECT_TRUE(geom1.isNull(0));
  EXPECT_FALSE(geom1.isNull(1));
  EXPECT_TRUE(geom1.isAnyNull(1));
  EXPECT_EQ(geom1.getGeometryCount(), 1);
  EXPECT_EQ(geom1.toString(), "A:--- B:-ie");
}

TEST(LabelTest, CopyConstructorAndAssignmentCreateIndependentCopies) {
  const Label original(0, Location::BOUNDARY, Location::INTERIOR,
                       Location::EXTERIOR);
  Label copy_constructed(original);
  EXPECT_EQ(copy_constructed.toString(), "A:ibe B:---");

  copy_constructed.flip();
  copy_constructed.setLocation(1, Location::INTERIOR);
  EXPECT_EQ(copy_constructed.toString(), "A:ebi B:-i-");
  EXPECT_EQ(original.toString(), "A:ibe B:---");

  Label assigned(Location::EXTERIOR);
  Label& assigned_ref = (assigned = original);
  EXPECT_EQ(&assigned_ref, &assigned);
  EXPECT_EQ(assigned.toString(), "A:ibe B:---");
  assigned.setAllLocations(1, Location::EXTERIOR);
  EXPECT_EQ(assigned.toString(), "A:ibe B:eee");
  EXPECT_EQ(original.toString(), "A:ibe B:---");
}

TEST(LabelTest, ToLineLabelAndToLineDropSideLocations) {
  Label area(Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR);
  area.setLocation(1, Position::ON, Location::INTERIOR);
  area.setLocation(1, Position::LEFT, Location::EXTERIOR);
  area.setLocation(1, Position::RIGHT, Location::BOUNDARY);

  const Label line = Label::toLineLabel(area);
  EXPECT_FALSE(line.isArea());
  EXPECT_TRUE(line.isLine(0));
  EXPECT_TRUE(line.isLine(1));
  EXPECT_EQ(line.getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(line.getLocation(1), Location::INTERIOR);
  EXPECT_EQ(line.getLocation(0, Position::LEFT), Location::NONE);
  EXPECT_EQ(line.getLocation(0, Position::RIGHT), Location::NONE);
  EXPECT_EQ(line.toString(), "A:b B:i");

  area.toLine(0);
  EXPECT_TRUE(area.isLine(0));
  EXPECT_TRUE(area.isArea(1));
  EXPECT_TRUE(area.isArea());
  EXPECT_EQ(area.toString(), "A:b B:eib");

  // Calling toLine on an already-line geometry leaves it unchanged.
  area.toLine(0);
  EXPECT_EQ(area.toString(), "A:b B:eib");

  area.toLine(1);
  EXPECT_FALSE(area.isArea());
  EXPECT_EQ(area.toString(), "A:b B:i");

  Label mixed_geom0_area(Location::BOUNDARY, Location::INTERIOR,
                         Location::EXTERIOR);
  mixed_geom0_area.setLocation(1, Position::ON, Location::INTERIOR);
  mixed_geom0_area.toLine(1);
  EXPECT_TRUE(mixed_geom0_area.isArea(0));
  EXPECT_TRUE(mixed_geom0_area.isLine(1));
  EXPECT_TRUE(mixed_geom0_area.isArea());
  EXPECT_EQ(mixed_geom0_area.toString(), "A:ibe B:i");

  mixed_geom0_area.flip();
  EXPECT_EQ(mixed_geom0_area.toString(), "A:ebi B:i");

  const Label already_line(0, Location::EXTERIOR);
  EXPECT_EQ(Label::toLineLabel(already_line).toString(), "A:e B:-");
  EXPECT_EQ(Label::toLineLabel(Label()).toString(), "A:- B:-");
}

TEST(LabelTest, FlipSwapsLeftAndRightSidesOnlyForAreaLabels) {
  Label area(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR);
  area.setLocation(1, Position::ON, Location::EXTERIOR);
  area.setLocation(1, Position::LEFT, Location::BOUNDARY);
  area.setLocation(1, Position::RIGHT, Location::INTERIOR);
  EXPECT_EQ(area.toString(), "A:ibe B:bei");

  area.flip();
  EXPECT_EQ(area.toString(), "A:ebi B:ieb");
  EXPECT_EQ(area.getLocation(0, Position::LEFT), Location::EXTERIOR);
  EXPECT_EQ(area.getLocation(0, Position::ON), Location::BOUNDARY);
  EXPECT_EQ(area.getLocation(0, Position::RIGHT), Location::INTERIOR);

  Label line(Location::BOUNDARY);
  line.setLocation(1, Location::INTERIOR);
  line.flip();
  EXPECT_EQ(line.toString(), "A:b B:i");
}

TEST(LabelTest, MergeFillsNullSlotsAndPromotesLineToArea) {
  Label merged_lines(0, Location::INTERIOR);
  merged_lines.merge(Label(1, Location::BOUNDARY));
  EXPECT_EQ(merged_lines.toString(), "A:i B:b");

  // Non-null attributes are preserved when merging.
  merged_lines.merge(Label(Location::EXTERIOR));
  EXPECT_EQ(merged_lines.toString(), "A:i B:b");

  // Merging an area label into a line label expands the destination to area.
  Label line_into_area(0, Location::INTERIOR);
  line_into_area.merge(
      Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR));
  EXPECT_TRUE(line_into_area.isArea(0));
  EXPECT_EQ(line_into_area.getLocation(0, Position::ON), Location::INTERIOR);
  EXPECT_EQ(line_into_area.getLocation(0, Position::LEFT), Location::EXTERIOR);
  EXPECT_EQ(line_into_area.getLocation(0, Position::RIGHT), Location::INTERIOR);
  EXPECT_EQ(line_into_area.toString(), "A:eii B:---");

  // Merging a line label into an area label fills only the null ON slot.
  Label area_target(0, Location::BOUNDARY, Location::EXTERIOR,
                    Location::INTERIOR);
  area_target.merge(Label(Location::INTERIOR));
  EXPECT_EQ(area_target.toString(), "A:ebi B:-i-");

  Label partial_area(0, Location::NONE, Location::INTERIOR, Location::EXTERIOR);
  partial_area.merge(Label(Location::BOUNDARY));
  EXPECT_EQ(partial_area.toString(), "A:ibe B:-b-");
}

TEST(LabelTest, SetLocationsAndAllPositionsEqualUpdateSelectively) {
  Label label(0, Location::NONE, Location::INTERIOR, Location::NONE);
  EXPECT_FALSE(label.allPositionsEqual(0, Location::INTERIOR));
  EXPECT_TRUE(label.isAnyNull(0));

  label.setAllLocationsIfNull(0, Location::EXTERIOR);
  EXPECT_EQ(label.toString(), "A:iee B:---");
  EXPECT_FALSE(label.isAnyNull(0));
  EXPECT_TRUE(label.isNull(1));

  label.setAllLocationsIfNull(Location::BOUNDARY);
  EXPECT_EQ(label.toString(), "A:iee B:bbb");
  EXPECT_TRUE(label.allPositionsEqual(1, Location::BOUNDARY));
  EXPECT_FALSE(label.allPositionsEqual(0, Location::EXTERIOR));

  label.setAllLocations(0, Location::INTERIOR);
  EXPECT_TRUE(label.allPositionsEqual(0, Location::INTERIOR));
  EXPECT_EQ(label.toString(), "A:iii B:bbb");

  label.setLocation(1, Location::EXTERIOR);
  EXPECT_EQ(label.getLocation(1, Position::ON), Location::EXTERIOR);
  EXPECT_FALSE(label.allPositionsEqual(1, Location::BOUNDARY));

  Label line_set_all(Location::NONE);
  line_set_all.setAllLocations(0, Location::BOUNDARY);
  EXPECT_TRUE(line_set_all.isLine(0));
  EXPECT_EQ(line_set_all.getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(line_set_all.toString(), "A:b B:-");
}

TEST(LabelTest, IsEqualOnSideChecksBothGeometriesForSpecifiedSide) {
  const Label lhs(Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR);
  Label rhs(Location::EXTERIOR, Location::INTERIOR, Location::BOUNDARY);

  EXPECT_TRUE(lhs.isEqualOnSide(rhs, Position::LEFT));
  EXPECT_FALSE(lhs.isEqualOnSide(rhs, Position::ON));
  EXPECT_FALSE(lhs.isEqualOnSide(rhs, Position::RIGHT));

  rhs.setLocation(1, Position::LEFT, Location::EXTERIOR);
  EXPECT_FALSE(lhs.isEqualOnSide(rhs, Position::LEFT));
}

TEST(LabelTest, StreamInsertionFormatsAndReturnsStreamReference) {
  const Label first(0, Location::INTERIOR, Location::BOUNDARY,
                    Location::EXTERIOR);
  const Label second(1, Location::BOUNDARY);

  std::ostringstream oss;
  std::ostream& returned = (oss << first);
  EXPECT_EQ(&returned, &oss);
  EXPECT_EQ(oss.str(), first.toString());
  EXPECT_EQ(oss.str(), "A:bie B:---");

  std::ostringstream chained;
  chained << first << " | " << second;
  EXPECT_EQ(chained.str(), "A:bie B:--- | A:- B:b");
}

}  // namespace
}  // namespace geos_google
