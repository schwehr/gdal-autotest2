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

// Unit tests for third_party/geos/src/operation/overlayng/OverlayLabel.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/OverlayLabel.cpp

#include "third_party/geos/include/geos/operation/overlayng/OverlayLabel.h"

#include <array>
#include <cstdint>
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
using geos::operation::overlayng::OverlayLabel;

static_assert(OverlayLabel::LOC_UNKNOWN == Location::NONE);

constexpr Location kLocUnknown = OverlayLabel::LOC_UNKNOWN;

std::string FormatLabel(const OverlayLabel& lbl, bool is_forward) {
  std::ostringstream oss;
  lbl.toString(is_forward, oss);
  return oss.str();
}

std::string StreamLabel(const OverlayLabel& lbl) {
  std::ostringstream oss;
  oss << lbl;
  return oss.str();
}

TEST(OverlayLabelTest, DefaultConstructorInitializesNotPartUnknown) {
  OverlayLabel lbl;

  for (uint8_t idx = 0; idx < 2; ++idx) {
    SCOPED_TRACE(static_cast<int>(idx));
    EXPECT_EQ(lbl.dimension(idx), +OverlayLabel::DIM_NOT_PART);
    EXPECT_TRUE(lbl.isNotPart(idx));
    EXPECT_FALSE(lbl.isKnown(idx));
    EXPECT_FALSE(lbl.isLine(idx));
    EXPECT_FALSE(lbl.isLinear(idx));
    EXPECT_FALSE(lbl.isBoundary(idx));
    EXPECT_FALSE(lbl.isCollapse(idx));
    EXPECT_FALSE(lbl.isHole(idx));
    EXPECT_FALSE(lbl.hasSides(idx));
    EXPECT_TRUE(lbl.isLineLocationUnknown(idx));
    EXPECT_EQ(lbl.getLineLocation(idx), kLocUnknown);
    EXPECT_EQ(lbl.getLocation(idx), kLocUnknown);
    EXPECT_FALSE(lbl.isLineInArea(static_cast<int8_t>(idx)));
    EXPECT_FALSE(lbl.isLineInterior(idx));
    EXPECT_EQ(lbl.getLocation(idx, Position::LEFT, true), kLocUnknown);
    EXPECT_EQ(lbl.getLocation(idx, Position::RIGHT, false), kLocUnknown);
    EXPECT_EQ(lbl.getLocation(idx, Position::ON, true), kLocUnknown);
    EXPECT_EQ(lbl.getLocationBoundaryOrLine(idx, Position::LEFT, true),
              kLocUnknown);
  }

  EXPECT_FALSE(lbl.isLine());
  EXPECT_FALSE(lbl.isBoundaryEither());
  EXPECT_FALSE(lbl.isBoundaryBoth());
  EXPECT_FALSE(lbl.isBoundarySingleton());
  EXPECT_FALSE(lbl.isBoundaryTouch());
  EXPECT_TRUE(lbl.isBoundaryCollapse());
  EXPECT_FALSE(lbl.isInteriorCollapse());
  EXPECT_FALSE(lbl.isCollapseAndNotPartInterior());
  EXPECT_EQ(StreamLabel(lbl), "A:-/B:-");
  EXPECT_EQ(FormatLabel(lbl, true), "A:-/B:-");
  EXPECT_EQ(FormatLabel(lbl, false), "A:-/B:-");
}

TEST(OverlayLabelTest, LineConstructorAndInitLineForBothSources) {
  for (uint8_t idx = 0; idx < 2; ++idx) {
    const uint8_t other = idx == 0 ? 1 : 0;
    SCOPED_TRACE(static_cast<int>(idx));

    OverlayLabel lbl(idx);
    EXPECT_EQ(lbl.dimension(idx), +OverlayLabel::DIM_LINE);
    EXPECT_EQ(lbl.dimension(other), +OverlayLabel::DIM_NOT_PART);
    EXPECT_TRUE(lbl.isLine());
    EXPECT_TRUE(lbl.isLine(idx));
    EXPECT_FALSE(lbl.isLine(other));
    EXPECT_TRUE(lbl.isLinear(idx));
    EXPECT_FALSE(lbl.isLinear(other));
    EXPECT_TRUE(lbl.isKnown(idx));
    EXPECT_FALSE(lbl.isKnown(other));
    EXPECT_FALSE(lbl.isNotPart(idx));
    EXPECT_TRUE(lbl.isNotPart(other));
    EXPECT_FALSE(lbl.isBoundaryCollapse());
    EXPECT_FALSE(lbl.isBoundarySingleton());
    EXPECT_TRUE(lbl.isLineLocationUnknown(idx));

    lbl.setLocationLine(idx, Location::INTERIOR);
    EXPECT_FALSE(lbl.isLineLocationUnknown(idx));
    EXPECT_EQ(lbl.getLineLocation(idx), Location::INTERIOR);
    EXPECT_EQ(lbl.getLocation(idx), Location::INTERIOR);
    EXPECT_TRUE(lbl.isLineInArea(static_cast<int8_t>(idx)));
    EXPECT_TRUE(lbl.isLineInterior(idx));
    EXPECT_EQ(lbl.getLocationBoundaryOrLine(idx, Position::LEFT, true),
              Location::INTERIOR);

    if (idx == 0) {
      EXPECT_EQ(StreamLabel(lbl), "A:iL/B:-");
    } else {
      EXPECT_EQ(StreamLabel(lbl), "A:-/B:iL");
    }

    // Re-initializing as line resets the linear location back to LOC_UNKNOWN.
    lbl.initLine(idx);
    EXPECT_TRUE(lbl.isLineLocationUnknown(idx));
    EXPECT_EQ(lbl.getLineLocation(idx), kLocUnknown);
    EXPECT_FALSE(lbl.isLineInArea(static_cast<int8_t>(idx)));
    EXPECT_FALSE(lbl.isLineInterior(idx));

    // Initializing the second source as a line works symmetrically.
    lbl.initLine(other);
    EXPECT_TRUE(lbl.isLine(other));
    EXPECT_EQ(StreamLabel(lbl), "A:-L/B:-L");
  }
}

TEST(OverlayLabelTest, BoundaryConstructorAndInitBoundary) {
  OverlayLabel lbl_a(0, Location::EXTERIOR, Location::INTERIOR, false);
  EXPECT_EQ(lbl_a.dimension(0), +OverlayLabel::DIM_BOUNDARY);
  EXPECT_TRUE(lbl_a.isKnown(0));
  EXPECT_TRUE(lbl_a.isBoundary(0));
  EXPECT_FALSE(lbl_a.isBoundary(1));
  EXPECT_TRUE(lbl_a.isBoundaryEither());
  EXPECT_FALSE(lbl_a.isBoundaryBoth());
  EXPECT_TRUE(lbl_a.isBoundarySingleton());
  EXPECT_TRUE(lbl_a.isBoundaryCollapse());
  EXPECT_FALSE(lbl_a.isBoundaryTouch());
  EXPECT_FALSE(lbl_a.isLinear(0));
  EXPECT_FALSE(lbl_a.isHole(0));
  EXPECT_TRUE(lbl_a.hasSides(0));
  EXPECT_FALSE(lbl_a.hasSides(1));
  EXPECT_FALSE(lbl_a.isLineLocationUnknown(0));
  EXPECT_EQ(lbl_a.getLineLocation(0), Location::INTERIOR);
  EXPECT_TRUE(lbl_a.isLineInArea(0));
  EXPECT_TRUE(lbl_a.isLineInterior(0));
  EXPECT_EQ(lbl_a.getLocation(0, Position::LEFT, true), Location::EXTERIOR);
  EXPECT_EQ(lbl_a.getLocation(0, Position::RIGHT, true), Location::INTERIOR);
  EXPECT_EQ(lbl_a.getLocation(0, Position::LEFT, false), Location::INTERIOR);
  EXPECT_EQ(lbl_a.getLocation(0, Position::RIGHT, false), Location::EXTERIOR);
  EXPECT_EQ(lbl_a.getLocation(0, Position::ON, true), Location::INTERIOR);
  EXPECT_EQ(lbl_a.getLocation(0, Position::ON, false), Location::INTERIOR);
  EXPECT_EQ(lbl_a.getLocationBoundaryOrLine(0, Position::LEFT, true),
            Location::EXTERIOR);
  EXPECT_EQ(lbl_a.getLocationBoundaryOrLine(0, Position::LEFT, false),
            Location::INTERIOR);
  EXPECT_EQ(lbl_a.getLocationBoundaryOrLine(1, Position::LEFT, true),
            kLocUnknown);
  EXPECT_EQ(StreamLabel(lbl_a), "A:eiB/B:-");
  EXPECT_EQ(FormatLabel(lbl_a, false), "A:ieB/B:-");

  OverlayLabel lbl_b(1, Location::INTERIOR, Location::EXTERIOR, true);
  EXPECT_EQ(lbl_b.dimension(1), +OverlayLabel::DIM_BOUNDARY);
  EXPECT_TRUE(lbl_b.isKnown(1));
  EXPECT_TRUE(lbl_b.isBoundary(1));
  EXPECT_FALSE(lbl_b.isBoundary(0));
  EXPECT_TRUE(lbl_b.isBoundaryEither());
  EXPECT_FALSE(lbl_b.isBoundaryBoth());
  EXPECT_TRUE(lbl_b.isBoundarySingleton());
  EXPECT_TRUE(lbl_b.isHole(1));
  EXPECT_TRUE(lbl_b.hasSides(1));
  EXPECT_FALSE(lbl_b.hasSides(0));
  EXPECT_EQ(lbl_b.getLineLocation(1), Location::INTERIOR);
  EXPECT_TRUE(lbl_b.isLineInArea(1));
  EXPECT_TRUE(lbl_b.isLineInterior(1));
  EXPECT_EQ(lbl_b.getLocation(1, Position::LEFT, true), Location::INTERIOR);
  EXPECT_EQ(lbl_b.getLocation(1, Position::RIGHT, true), Location::EXTERIOR);
  EXPECT_EQ(lbl_b.getLocation(1, Position::LEFT, false), Location::EXTERIOR);
  EXPECT_EQ(lbl_b.getLocation(1, Position::RIGHT, false), Location::INTERIOR);
  EXPECT_EQ(lbl_b.getLocation(1, Position::ON, true), Location::INTERIOR);
  EXPECT_EQ(lbl_b.getLocation(1, Position::ON, false), Location::INTERIOR);
  EXPECT_EQ(lbl_b.getLocationBoundaryOrLine(1, Position::RIGHT, true),
            Location::EXTERIOR);
  EXPECT_EQ(lbl_b.getLocationBoundaryOrLine(1, Position::RIGHT, false),
            Location::INTERIOR);
  EXPECT_EQ(StreamLabel(lbl_b), "A:-/B:ieB");
  EXPECT_EQ(FormatLabel(lbl_b, false), "A:-/B:eiB");
}

TEST(OverlayLabelTest, BoundaryBothAndTouchPredicates) {
  OverlayLabel same_side(0, Location::EXTERIOR, Location::INTERIOR, false);
  same_side.initBoundary(1, Location::EXTERIOR, Location::INTERIOR, true);
  EXPECT_TRUE(same_side.isBoundaryEither());
  EXPECT_TRUE(same_side.isBoundaryBoth());
  EXPECT_FALSE(same_side.isBoundarySingleton());
  EXPECT_FALSE(same_side.isBoundaryCollapse());
  EXPECT_FALSE(same_side.isBoundaryTouch());
  EXPECT_FALSE(same_side.isHole(0));
  EXPECT_TRUE(same_side.isHole(1));
  EXPECT_EQ(StreamLabel(same_side), "A:eiB/B:eiB");

  OverlayLabel touching(0, Location::EXTERIOR, Location::INTERIOR, false);
  touching.initBoundary(1, Location::INTERIOR, Location::EXTERIOR, false);
  EXPECT_TRUE(touching.isBoundaryBoth());
  EXPECT_TRUE(touching.isBoundaryTouch());
  EXPECT_EQ(StreamLabel(touching), "A:eiB/B:ieB");
  EXPECT_EQ(FormatLabel(touching, false), "A:ieB/B:eiB");

  // Boundary combined with line is not a singleton and not a boundary collapse.
  OverlayLabel boundary_and_line(0, Location::EXTERIOR, Location::INTERIOR,
                                 false);
  boundary_and_line.initLine(1);
  EXPECT_TRUE(boundary_and_line.isBoundaryEither());
  EXPECT_FALSE(boundary_and_line.isBoundaryBoth());
  EXPECT_FALSE(boundary_and_line.isBoundarySingleton());
  EXPECT_FALSE(boundary_and_line.isBoundaryCollapse());
  EXPECT_FALSE(boundary_and_line.isBoundaryTouch());
}

TEST(OverlayLabelTest, CollapseInitializationAndSetLocationCollapse) {
  struct CollapseCase {
    uint8_t index;
    bool is_hole;
    Location expected_loc;
    std::string_view before_loc_str;
    std::string_view after_loc_str;
    bool expected_interior_collapse;
  };

  constexpr auto kCases = std::to_array<CollapseCase>({
      {
          .index = 0,
          .is_hole = false,
          .expected_loc = Location::EXTERIOR,
          .before_loc_str = "A:-Cs/B:-",
          .after_loc_str = "A:eCs/B:-",
          .expected_interior_collapse = false,
      },
      {
          .index = 0,
          .is_hole = true,
          .expected_loc = Location::INTERIOR,
          .before_loc_str = "A:-Ch/B:-",
          .after_loc_str = "A:iCh/B:-",
          .expected_interior_collapse = true,
      },
      {
          .index = 1,
          .is_hole = false,
          .expected_loc = Location::EXTERIOR,
          .before_loc_str = "A:-/B:-Cs",
          .after_loc_str = "A:-/B:eCs",
          .expected_interior_collapse = false,
      },
      {
          .index = 1,
          .is_hole = true,
          .expected_loc = Location::INTERIOR,
          .before_loc_str = "A:-/B:-Ch",
          .after_loc_str = "A:-/B:iCh",
          .expected_interior_collapse = true,
      },
  });

  for (const auto& c : kCases) {
    OverlayLabel lbl;
    lbl.initCollapse(c.index, c.is_hole);
    EXPECT_EQ(lbl.dimension(c.index), +OverlayLabel::DIM_COLLAPSE);
    EXPECT_TRUE(lbl.isCollapse(c.index));
    EXPECT_TRUE(lbl.isLinear(c.index));
    EXPECT_TRUE(lbl.isKnown(c.index));
    EXPECT_FALSE(lbl.isBoundary(c.index));
    EXPECT_FALSE(lbl.isLine(c.index));
    EXPECT_EQ(lbl.isHole(c.index), c.is_hole);
    EXPECT_TRUE(lbl.isLineLocationUnknown(c.index));
    EXPECT_FALSE(lbl.isInteriorCollapse());
    EXPECT_EQ(StreamLabel(lbl), c.before_loc_str);

    lbl.setLocationCollapse(c.index);
    EXPECT_FALSE(lbl.isLineLocationUnknown(c.index));
    EXPECT_EQ(lbl.getLineLocation(c.index), c.expected_loc);
    EXPECT_EQ(lbl.isInteriorCollapse(), c.expected_interior_collapse);
    EXPECT_EQ(StreamLabel(lbl), c.after_loc_str);
  }

  // Coincident collapse on one source with boundary on the other.
  OverlayLabel boundary_with_collapse(0, Location::EXTERIOR, Location::INTERIOR,
                                      false);
  boundary_with_collapse.initCollapse(1, false);
  EXPECT_TRUE(boundary_with_collapse.isBoundaryCollapse());
  EXPECT_FALSE(boundary_with_collapse.isBoundarySingleton());
  EXPECT_EQ(StreamLabel(boundary_with_collapse), "A:eiB/B:-Cs");
}

TEST(OverlayLabelTest, NotPartAndLocationMutations) {
  OverlayLabel lbl(0, Location::EXTERIOR, Location::INTERIOR, true);
  lbl.initCollapse(1, false);
  lbl.setLocationLine(1, Location::BOUNDARY);
  EXPECT_EQ(lbl.getLineLocation(1), Location::BOUNDARY);

  // setLocationAll updates line, left, and right locations for the target
  // index.
  lbl.setLocationAll(0, Location::EXTERIOR);
  EXPECT_EQ(lbl.getLineLocation(0), Location::EXTERIOR);
  EXPECT_EQ(lbl.getLocation(0, Position::LEFT, true), Location::EXTERIOR);
  EXPECT_EQ(lbl.getLocation(0, Position::RIGHT, true), Location::EXTERIOR);
  EXPECT_EQ(lbl.getLocation(0, Position::ON, true), Location::EXTERIOR);
  EXPECT_TRUE(lbl.hasSides(0));

  lbl.setLocationAll(1, Location::INTERIOR);
  EXPECT_EQ(lbl.getLineLocation(1), Location::INTERIOR);
  EXPECT_EQ(lbl.getLocation(1, Position::LEFT, true), Location::INTERIOR);
  EXPECT_EQ(lbl.getLocation(1, Position::RIGHT, false), Location::INTERIOR);
  EXPECT_EQ(lbl.getLocation(1, Position::ON, false), Location::INTERIOR);
  EXPECT_TRUE(lbl.hasSides(1));

  // Verify hasSides(index) when only one side differs from LOC_UNKNOWN.
  OverlayLabel left_only_side(0, Location::INTERIOR, kLocUnknown, false);
  EXPECT_TRUE(left_only_side.hasSides(0));
  OverlayLabel right_only_side(1, kLocUnknown, Location::EXTERIOR, false);
  EXPECT_TRUE(right_only_side.hasSides(1));

  // initNotPart resets dimension to DIM_NOT_PART for both indices.
  lbl.initNotPart(0);
  EXPECT_TRUE(lbl.isNotPart(0));
  EXPECT_FALSE(lbl.isKnown(0));
  EXPECT_FALSE(lbl.isBoundary(0));
  lbl.initNotPart(1);
  EXPECT_TRUE(lbl.isNotPart(1));
  EXPECT_FALSE(lbl.isKnown(1));
  EXPECT_FALSE(lbl.isCollapse(1));
  EXPECT_EQ(StreamLabel(lbl), "A:e/B:i");
}

TEST(OverlayLabelTest, GetLocationHandlesInvalidPositions) {
  OverlayLabel lbl(0, Location::EXTERIOR, Location::INTERIOR, false);
  lbl.initBoundary(1, Location::INTERIOR, Location::EXTERIOR, true);

  for (int invalid_pos : std::to_array({-1, 3, 99})) {
    EXPECT_EQ(lbl.getLocation(0, invalid_pos, true), kLocUnknown);
    EXPECT_EQ(lbl.getLocation(0, invalid_pos, false), kLocUnknown);
    EXPECT_EQ(lbl.getLocation(1, invalid_pos, true), kLocUnknown);
    EXPECT_EQ(lbl.getLocation(1, invalid_pos, false), kLocUnknown);
  }
}

TEST(OverlayLabelTest, IsCollapseAndNotPartInteriorAllBranches) {
  // Positive case 1: A is COLLAPSE, B is NOT_PART with INTERIOR location.
  OverlayLabel a_collapse_b_int;
  a_collapse_b_int.initCollapse(0, false);
  a_collapse_b_int.initNotPart(1);
  a_collapse_b_int.setLocationLine(1, Location::INTERIOR);
  EXPECT_TRUE(a_collapse_b_int.isCollapseAndNotPartInterior());

  // Negative variants for A=COLLAPSE: B not INTERIOR or B is not NOT_PART.
  OverlayLabel a_collapse_b_ext = a_collapse_b_int.copy();
  a_collapse_b_ext.setLocationLine(1, Location::EXTERIOR);
  EXPECT_FALSE(a_collapse_b_ext.isCollapseAndNotPartInterior());

  OverlayLabel a_collapse_b_none = a_collapse_b_int.copy();
  a_collapse_b_none.setLocationLine(1, Location::NONE);
  EXPECT_FALSE(a_collapse_b_none.isCollapseAndNotPartInterior());

  OverlayLabel a_collapse_b_line = a_collapse_b_int.copy();
  a_collapse_b_line.initLine(1);
  a_collapse_b_line.setLocationLine(1, Location::INTERIOR);
  EXPECT_FALSE(a_collapse_b_line.isCollapseAndNotPartInterior());

  // Positive case 2: B is COLLAPSE, A is NOT_PART with INTERIOR location.
  OverlayLabel b_collapse_a_int;
  b_collapse_a_int.initNotPart(0);
  b_collapse_a_int.setLocationLine(0, Location::INTERIOR);
  b_collapse_a_int.initCollapse(1, true);
  EXPECT_TRUE(b_collapse_a_int.isCollapseAndNotPartInterior());

  // Negative variants for B=COLLAPSE: A not INTERIOR or A is not NOT_PART.
  OverlayLabel b_collapse_a_ext = b_collapse_a_int.copy();
  b_collapse_a_ext.setLocationLine(0, Location::EXTERIOR);
  EXPECT_FALSE(b_collapse_a_ext.isCollapseAndNotPartInterior());

  OverlayLabel b_collapse_a_boundary = b_collapse_a_int.copy();
  b_collapse_a_boundary.initBoundary(0, Location::EXTERIOR, Location::INTERIOR,
                                     false);
  EXPECT_FALSE(b_collapse_a_boundary.isCollapseAndNotPartInterior());
}

TEST(OverlayLabelTest, CopyCreatesIndependentLabel) {
  OverlayLabel orig(0, Location::EXTERIOR, Location::INTERIOR, false);
  orig.initCollapse(1, true);
  orig.setLocationLine(1, Location::BOUNDARY);

  OverlayLabel cloned = orig.copy();
  EXPECT_EQ(StreamLabel(cloned), "A:eiB/B:bCh");
  EXPECT_EQ(FormatLabel(cloned, false), "A:ieB/B:bCh");

  orig.setLocationLine(1, Location::EXTERIOR);
  orig.initNotPart(0);
  EXPECT_EQ(StreamLabel(orig), "A:i/B:eCh");
  EXPECT_EQ(StreamLabel(cloned), "A:eiB/B:bCh");
  EXPECT_TRUE(cloned.isBoundary(0));
  EXPECT_EQ(cloned.getLineLocation(1), Location::BOUNDARY);
}

TEST(OverlayLabelTest, ToStringFormatsAllCombinations) {
  enum class DimKind { kNotPart, kLine, kBoundary, kCollapse };
  struct FormatCase {
    DimKind a_dim;
    Location a_left;
    Location a_right;
    Location a_line;
    bool a_hole;
    DimKind b_dim;
    Location b_left;
    Location b_right;
    Location b_line;
    bool b_hole;
    bool is_forward;
    std::string_view expected;
  };

  auto apply_dim = [](OverlayLabel& lbl, uint8_t idx, DimKind kind,
                      Location left, Location right, Location line,
                      bool is_hole) {
    switch (kind) {
      case DimKind::kNotPart:
        lbl.initNotPart(idx);
        lbl.setLocationLine(idx, line);
        break;
      case DimKind::kLine:
        lbl.initLine(idx);
        lbl.setLocationLine(idx, line);
        break;
      case DimKind::kBoundary:
        lbl.initBoundary(idx, left, right, is_hole);
        break;
      case DimKind::kCollapse:
        lbl.initCollapse(idx, is_hole);
        lbl.setLocationLine(idx, line);
        break;
    }
  };

  constexpr auto kCases = std::to_array<FormatCase>({
      {
          .a_dim = DimKind::kNotPart,
          .a_left = kLocUnknown,
          .a_right = kLocUnknown,
          .a_line = Location::NONE,
          .a_hole = false,
          .b_dim = DimKind::kNotPart,
          .b_left = kLocUnknown,
          .b_right = kLocUnknown,
          .b_line = Location::NONE,
          .b_hole = false,
          .is_forward = true,
          .expected = "A:-/B:-",
      },
      {
          .a_dim = DimKind::kLine,
          .a_left = kLocUnknown,
          .a_right = kLocUnknown,
          .a_line = Location::BOUNDARY,
          .a_hole = false,
          .b_dim = DimKind::kLine,
          .b_left = kLocUnknown,
          .b_right = kLocUnknown,
          .b_line = Location::EXTERIOR,
          .b_hole = false,
          .is_forward = true,
          .expected = "A:bL/B:eL",
      },
      {
          .a_dim = DimKind::kBoundary,
          .a_left = Location::INTERIOR,
          .a_right = Location::BOUNDARY,
          .a_line = Location::INTERIOR,
          .a_hole = false,
          .b_dim = DimKind::kCollapse,
          .b_left = kLocUnknown,
          .b_right = kLocUnknown,
          .b_line = Location::INTERIOR,
          .b_hole = true,
          .is_forward = true,
          .expected = "A:ibB/B:iCh",
      },
      {
          .a_dim = DimKind::kBoundary,
          .a_left = Location::INTERIOR,
          .a_right = Location::EXTERIOR,
          .a_line = Location::INTERIOR,
          .a_hole = true,
          .b_dim = DimKind::kCollapse,
          .b_left = kLocUnknown,
          .b_right = kLocUnknown,
          .b_line = Location::EXTERIOR,
          .b_hole = false,
          .is_forward = false,
          .expected = "A:eiB/B:eCs",
      },
  });

  for (const auto& c : kCases) {
    OverlayLabel lbl;
    apply_dim(lbl, 0, c.a_dim, c.a_left, c.a_right, c.a_line, c.a_hole);
    apply_dim(lbl, 1, c.b_dim, c.b_left, c.b_right, c.b_line, c.b_hole);
    EXPECT_EQ(FormatLabel(lbl, c.is_forward), c.expected);
    if (c.is_forward) {
      EXPECT_EQ(StreamLabel(lbl), c.expected);
    }
  }
}

}  // namespace
}  // namespace geos_google
