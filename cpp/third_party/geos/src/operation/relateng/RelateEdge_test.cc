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

#include "third_party/geos/include/geos/operation/relateng/RelateEdge.h"

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/operation/relateng/RelateGeometry.h"
#include "third_party/geos/include/geos/operation/relateng/RelateNode.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Dimension;
using geos::geom::Location;
using geos::geom::Position;
using geos::operation::relateng::RelateEdge;
using geos::operation::relateng::RelateGeometry;
using geos::operation::relateng::RelateNode;

constexpr bool kGeomA = RelateGeometry::GEOM_A;
constexpr bool kGeomB = RelateGeometry::GEOM_B;
constexpr bool kForward = RelateEdge::IS_FORWARD;
constexpr bool kReverse = RelateEdge::IS_REVERSE;
constexpr int kDimUnknown = RelateEdge::DIM_UNKNOWN;
constexpr std::size_t kIndexUnknown = geos::INDEX_UNKNOWN;

void ExpectLocations(const RelateEdge& edge, bool is_a, Location expected_left,
                     Location expected_on, Location expected_right) {
  SCOPED_TRACE(is_a ? "Geometry A" : "Geometry B");
  EXPECT_EQ(edge.location(is_a, Position::LEFT), expected_left);
  EXPECT_EQ(edge.location(is_a, Position::ON), expected_on);
  EXPECT_EQ(edge.location(is_a, Position::RIGHT), expected_right);

  EXPECT_EQ(edge.isInterior(is_a, Position::LEFT),
            expected_left == Location::INTERIOR);
  EXPECT_EQ(edge.isInterior(is_a, Position::ON),
            expected_on == Location::INTERIOR);
  EXPECT_EQ(edge.isInterior(is_a, Position::RIGHT),
            expected_right == Location::INTERIOR);
}

TEST(RelateEdgeTest, ConstantsHaveExpectedValues) {
  EXPECT_TRUE(kForward);
  EXPECT_FALSE(kReverse);
  EXPECT_EQ(kDimUnknown, -1);
}

TEST(RelateEdgeTest, AreaConstructorSetsLocationsForTargetGeometryOnly) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY dir_pt(1.0, 0.0);
  const RelateNode node(&node_pt);

  {
    const RelateEdge edge(&node, &dir_pt, kGeomA, kForward);
    ExpectLocations(edge, kGeomA, Location::EXTERIOR, Location::BOUNDARY,
                    Location::INTERIOR);
    ExpectLocations(edge, kGeomB, Location::NONE, Location::NONE,
                    Location::NONE);
  }
  {
    const RelateEdge edge(&node, &dir_pt, kGeomA, kReverse);
    ExpectLocations(edge, kGeomA, Location::INTERIOR, Location::BOUNDARY,
                    Location::EXTERIOR);
    ExpectLocations(edge, kGeomB, Location::NONE, Location::NONE,
                    Location::NONE);
  }
  {
    const RelateEdge edge(&node, &dir_pt, kGeomB, kForward);
    ExpectLocations(edge, kGeomA, Location::NONE, Location::NONE,
                    Location::NONE);
    ExpectLocations(edge, kGeomB, Location::EXTERIOR, Location::BOUNDARY,
                    Location::INTERIOR);
  }
  {
    const RelateEdge edge(&node, &dir_pt, kGeomB, kReverse);
    ExpectLocations(edge, kGeomA, Location::NONE, Location::NONE,
                    Location::NONE);
    ExpectLocations(edge, kGeomB, Location::INTERIOR, Location::BOUNDARY,
                    Location::EXTERIOR);
  }
}

TEST(RelateEdgeTest, LineConstructorSetsLocationsForTargetGeometryOnly) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY dir_pt(0.0, 1.0);
  const RelateNode node(&node_pt);

  {
    const RelateEdge edge(&node, &dir_pt, kGeomA);
    ExpectLocations(edge, kGeomA, Location::EXTERIOR, Location::INTERIOR,
                    Location::EXTERIOR);
    ExpectLocations(edge, kGeomB, Location::NONE, Location::NONE,
                    Location::NONE);
  }
  {
    const RelateEdge edge(&node, &dir_pt, kGeomB);
    ExpectLocations(edge, kGeomA, Location::NONE, Location::NONE,
                    Location::NONE);
    ExpectLocations(edge, kGeomB, Location::EXTERIOR, Location::INTERIOR,
                    Location::EXTERIOR);
  }
}

TEST(RelateEdgeTest, ExplicitLocationsConstructorSetsExactLocations) {
  const CoordinateXY node_pt(1.0, 2.0);
  const CoordinateXY dir_pt(3.0, 4.0);
  const RelateNode node(&node_pt);

  {
    const RelateEdge edge(&node, &dir_pt, kGeomA, Location::BOUNDARY,
                          Location::INTERIOR, Location::EXTERIOR);
    ExpectLocations(edge, kGeomA, Location::BOUNDARY, Location::EXTERIOR,
                    Location::INTERIOR);
    ExpectLocations(edge, kGeomB, Location::NONE, Location::NONE,
                    Location::NONE);
  }
  {
    const RelateEdge edge(&node, &dir_pt, kGeomB, Location::NONE,
                          Location::BOUNDARY, Location::INTERIOR);
    ExpectLocations(edge, kGeomA, Location::NONE, Location::NONE,
                    Location::NONE);
    ExpectLocations(edge, kGeomB, Location::NONE, Location::INTERIOR,
                    Location::BOUNDARY);
  }
}

TEST(RelateEdgeTest, CreateFactoryBuildsAreaAndLineEdges) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY dir_pt(2.0, 3.0);
  const RelateNode node(&node_pt);

  std::unique_ptr<RelateEdge> area_a_fwd(
      RelateEdge::create(&node, &dir_pt, kGeomA, Dimension::A, kForward));
  ASSERT_NE(area_a_fwd, nullptr);
  ExpectLocations(*area_a_fwd, kGeomA, Location::EXTERIOR, Location::BOUNDARY,
                  Location::INTERIOR);
  ExpectLocations(*area_a_fwd, kGeomB, Location::NONE, Location::NONE,
                  Location::NONE);

  std::unique_ptr<RelateEdge> area_b_rev(
      RelateEdge::create(&node, &dir_pt, kGeomB, Dimension::A, kReverse));
  ASSERT_NE(area_b_rev, nullptr);
  ExpectLocations(*area_b_rev, kGeomA, Location::NONE, Location::NONE,
                  Location::NONE);
  ExpectLocations(*area_b_rev, kGeomB, Location::INTERIOR, Location::BOUNDARY,
                  Location::EXTERIOR);

  std::unique_ptr<RelateEdge> line_a(
      RelateEdge::create(&node, &dir_pt, kGeomA, Dimension::L, kForward));
  ASSERT_NE(line_a, nullptr);
  ExpectLocations(*line_a, kGeomA, Location::EXTERIOR, Location::INTERIOR,
                  Location::EXTERIOR);

  std::unique_ptr<RelateEdge> non_area_b(
      RelateEdge::create(&node, &dir_pt, kGeomB, Dimension::P, kReverse));
  ASSERT_NE(non_area_b, nullptr);
  ExpectLocations(*non_area_b, kGeomB, Location::EXTERIOR, Location::INTERIOR,
                  Location::EXTERIOR);
}

TEST(RelateEdgeTest, FindKnownEdgeIndexFindsFirstKnownOrReturnsIndexUnknown) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY dir_pt_0(1.0, 0.0);
  const CoordinateXY dir_pt_1(0.0, 1.0);
  const CoordinateXY dir_pt_2(-1.0, 0.0);
  const RelateNode node(&node_pt);

  std::vector<std::unique_ptr<RelateEdge>> edges;
  EXPECT_EQ(RelateEdge::findKnownEdgeIndex(edges, kGeomA), kIndexUnknown);
  EXPECT_EQ(RelateEdge::findKnownEdgeIndex(edges, kGeomB), kIndexUnknown);

  edges.push_back(std::make_unique<RelateEdge>(&node, &dir_pt_0, kGeomB));
  edges.push_back(
      std::make_unique<RelateEdge>(&node, &dir_pt_1, kGeomB, kForward));
  EXPECT_EQ(RelateEdge::findKnownEdgeIndex(edges, kGeomA), kIndexUnknown);
  EXPECT_EQ(RelateEdge::findKnownEdgeIndex(edges, kGeomB), std::size_t{0});

  // Setting locations with DIM_UNKNOWN still leaves the geometry unknown.
  edges[0]->setDimLocations(kGeomA, kDimUnknown, Location::EXTERIOR);
  EXPECT_EQ(RelateEdge::findKnownEdgeIndex(edges, kGeomA), kIndexUnknown);

  edges.push_back(std::make_unique<RelateEdge>(&node, &dir_pt_2, kGeomA));
  EXPECT_EQ(RelateEdge::findKnownEdgeIndex(edges, kGeomA), std::size_t{2});

  // Setting a valid dimension makes an earlier edge known.
  edges[1]->setDimLocations(kGeomA, Dimension::P, Location::INTERIOR);
  EXPECT_EQ(RelateEdge::findKnownEdgeIndex(edges, kGeomA), std::size_t{1});
}

TEST(RelateEdgeTest, SetAreaInteriorStaticAndMemberUpdateTargetGeometryOnly) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY dir_pt_0(1.0, 0.0);
  const CoordinateXY dir_pt_1(0.0, 1.0);
  const RelateNode node(&node_pt);

  std::vector<std::unique_ptr<RelateEdge>> empty_edges;
  RelateEdge::setAreaInterior(empty_edges, kGeomA);
  EXPECT_TRUE(empty_edges.empty());

  std::vector<std::unique_ptr<RelateEdge>> edges;
  edges.push_back(
      std::make_unique<RelateEdge>(&node, &dir_pt_0, kGeomA, kForward));
  edges.push_back(std::make_unique<RelateEdge>(&node, &dir_pt_1, kGeomB));

  RelateEdge::setAreaInterior(edges, kGeomA);
  ExpectLocations(*edges[0], kGeomA, Location::INTERIOR, Location::INTERIOR,
                  Location::INTERIOR);
  ExpectLocations(*edges[0], kGeomB, Location::NONE, Location::NONE,
                  Location::NONE);
  ExpectLocations(*edges[1], kGeomA, Location::INTERIOR, Location::INTERIOR,
                  Location::INTERIOR);
  ExpectLocations(*edges[1], kGeomB, Location::EXTERIOR, Location::INTERIOR,
                  Location::EXTERIOR);

  edges[0]->setAreaInterior(kGeomB);
  ExpectLocations(*edges[0], kGeomB, Location::INTERIOR, Location::INTERIOR,
                  Location::INTERIOR);
  ExpectLocations(*edges[0], kGeomA, Location::INTERIOR, Location::INTERIOR,
                  Location::INTERIOR);
}

TEST(RelateEdgeTest, CompareToEdgeOrdersAnglesCounterClockwise) {
  const CoordinateXY node_pt(10.0, 20.0);
  const RelateNode node(&node_pt);

  const CoordinateXY dir_east(11.0, 20.0);
  const CoordinateXY dir_east_far(15.0, 20.0);
  const CoordinateXY dir_ne(11.0, 21.0);
  const CoordinateXY dir_nw_low(9.5, 21.0);
  const CoordinateXY dir_nw_mid(9.0, 21.0);
  const CoordinateXY dir_nw_mid_far(8.0, 22.0);
  const CoordinateXY dir_nw_high(8.0, 21.0);
  const CoordinateXY dir_sw(9.0, 19.0);
  const CoordinateXY dir_se(11.0, 19.0);

  const RelateEdge edge_east(&node, &dir_east, kGeomA);
  EXPECT_EQ(edge_east.compareToEdge(&dir_east_far), 0);
  EXPECT_EQ(edge_east.compareToEdge(&dir_ne), -1);
  EXPECT_EQ(edge_east.compareToEdge(&dir_nw_mid), -1);
  EXPECT_EQ(edge_east.compareToEdge(&dir_sw), -1);
  EXPECT_EQ(edge_east.compareToEdge(&dir_se), -1);

  const RelateEdge edge_nw(&node, &dir_nw_mid, kGeomA);
  EXPECT_EQ(edge_nw.compareToEdge(&dir_east), 1);
  EXPECT_EQ(edge_nw.compareToEdge(&dir_nw_low), 1);
  EXPECT_EQ(edge_nw.compareToEdge(&dir_nw_mid_far), 0);
  EXPECT_EQ(edge_nw.compareToEdge(&dir_nw_high), -1);
  EXPECT_EQ(edge_nw.compareToEdge(&dir_sw), -1);

  EXPECT_THROW(edge_east.compareToEdge(&node_pt),
               geos::util::IllegalArgumentException);
}

TEST(RelateEdgeTest, MergeUnknownGeometryInitializesAllLocations) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY dir_pt(1.0, 1.0);
  const RelateNode node(&node_pt);

  {
    RelateEdge edge(&node, &dir_pt, kGeomA);
    edge.merge(kGeomB, Dimension::L, kReverse);
    ExpectLocations(edge, kGeomA, Location::EXTERIOR, Location::INTERIOR,
                    Location::EXTERIOR);
    ExpectLocations(edge, kGeomB, Location::EXTERIOR, Location::INTERIOR,
                    Location::EXTERIOR);
  }
  {
    RelateEdge edge(&node, &dir_pt, kGeomA);
    edge.merge(kGeomB, Dimension::A, kForward);
    ExpectLocations(edge, kGeomB, Location::EXTERIOR, Location::BOUNDARY,
                    Location::INTERIOR);
  }
  {
    RelateEdge edge(&node, &dir_pt, kGeomA);
    edge.merge(kGeomB, Dimension::A, kReverse);
    ExpectLocations(edge, kGeomB, Location::INTERIOR, Location::BOUNDARY,
                    Location::EXTERIOR);
  }
  {
    RelateEdge edge(&node, &dir_pt, kGeomB, kForward);
    edge.merge(kGeomA, Dimension::L, kForward);
    ExpectLocations(edge, kGeomA, Location::EXTERIOR, Location::INTERIOR,
                    Location::EXTERIOR);
    ExpectLocations(edge, kGeomB, Location::EXTERIOR, Location::BOUNDARY,
                    Location::INTERIOR);
  }
  {
    RelateEdge edge(&node, &dir_pt, kGeomB);
    edge.merge(kGeomA, Dimension::A, kReverse);
    ExpectLocations(edge, kGeomA, Location::INTERIOR, Location::BOUNDARY,
                    Location::EXTERIOR);
  }
}

TEST(RelateEdgeTest, MergeKnownGeometryCombinesDimensionsAndSides) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY dir_pt(1.0, 0.0);
  const RelateNode node(&node_pt);

  // Line + Line keeps line locations.
  {
    RelateEdge edge(&node, &dir_pt, kGeomA);
    edge.merge(kGeomA, Dimension::L, kForward);
    ExpectLocations(edge, kGeomA, Location::EXTERIOR, Location::INTERIOR,
                    Location::EXTERIOR);
  }

  // Line + Area upgrades line to area boundary and interior on the area side.
  {
    RelateEdge edge(&node, &dir_pt, kGeomA);
    edge.merge(kGeomA, Dimension::A, kForward);
    ExpectLocations(edge, kGeomA, Location::EXTERIOR, Location::BOUNDARY,
                    Location::INTERIOR);
    // Merging a line into an already-upgraded area edge preserves boundary and
    // interior side.
    edge.merge(kGeomA, Dimension::L, kReverse);
    ExpectLocations(edge, kGeomA, Location::EXTERIOR, Location::BOUNDARY,
                    Location::INTERIOR);
  }
  {
    RelateEdge edge(&node, &dir_pt, kGeomB);
    edge.merge(kGeomB, Dimension::A, kReverse);
    ExpectLocations(edge, kGeomB, Location::INTERIOR, Location::BOUNDARY,
                    Location::EXTERIOR);
  }

  // Area + Line keeps area boundary and interior side.
  {
    RelateEdge edge(&node, &dir_pt, kGeomA, kReverse);
    edge.merge(kGeomA, Dimension::L, kForward);
    ExpectLocations(edge, kGeomA, Location::INTERIOR, Location::BOUNDARY,
                    Location::EXTERIOR);
  }

  // Area + Area in same direction stays unchanged.
  {
    RelateEdge edge(&node, &dir_pt, kGeomB, kForward);
    edge.merge(kGeomB, Dimension::A, kForward);
    ExpectLocations(edge, kGeomB, Location::EXTERIOR, Location::BOUNDARY,
                    Location::INTERIOR);
  }

  // Area + Area in opposite directions marks both sides INTERIOR.
  {
    RelateEdge edge(&node, &dir_pt, kGeomA, kForward);
    edge.merge(kGeomA, Dimension::A, kReverse);
    ExpectLocations(edge, kGeomA, Location::INTERIOR, Location::BOUNDARY,
                    Location::INTERIOR);
  }
  {
    RelateEdge edge(&node, &dir_pt, kGeomB, kReverse);
    edge.merge(kGeomB, Dimension::A, kForward);
    ExpectLocations(edge, kGeomB, Location::INTERIOR, Location::BOUNDARY,
                    Location::INTERIOR);
  }

  // Known non-line, non-area dimension (P) merging Area updates sides without
  // overwriting ON via mergeDimEdgeLoc.
  {
    RelateEdge edge(&node, &dir_pt, kGeomB);
    edge.setDimLocations(kGeomA, Dimension::P, Location::EXTERIOR);
    edge.merge(kGeomA, Dimension::A, kForward);
    ExpectLocations(edge, kGeomA, Location::EXTERIOR, Location::EXTERIOR,
                    Location::INTERIOR);
  }
}

TEST(RelateEdgeTest, SetLocationUpdatesSinglePositionAndIgnoresInvalidPos) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY dir_pt(1.0, 0.0);
  const RelateNode node(&node_pt);

  RelateEdge edge(&node, &dir_pt, kGeomA, kForward);
  edge.setLocation(kGeomA, Position::LEFT, Location::BOUNDARY);
  ExpectLocations(edge, kGeomA, Location::BOUNDARY, Location::BOUNDARY,
                  Location::INTERIOR);

  edge.setLocation(kGeomA, Position::ON, Location::INTERIOR);
  ExpectLocations(edge, kGeomA, Location::BOUNDARY, Location::INTERIOR,
                  Location::INTERIOR);

  edge.setLocation(kGeomA, Position::RIGHT, Location::EXTERIOR);
  ExpectLocations(edge, kGeomA, Location::BOUNDARY, Location::INTERIOR,
                  Location::EXTERIOR);

  // Unrecognized position falls through without modifying any slot.
  edge.setLocation(kGeomA, 99, Location::NONE);
  ExpectLocations(edge, kGeomA, Location::BOUNDARY, Location::INTERIOR,
                  Location::EXTERIOR);

  edge.setLocation(kGeomB, Position::LEFT, Location::INTERIOR);
  edge.setLocation(kGeomB, Position::ON, Location::BOUNDARY);
  edge.setLocation(kGeomB, Position::RIGHT, Location::EXTERIOR);
  ExpectLocations(edge, kGeomB, Location::INTERIOR, Location::BOUNDARY,
                  Location::EXTERIOR);
  ExpectLocations(edge, kGeomA, Location::BOUNDARY, Location::INTERIOR,
                  Location::EXTERIOR);
}

TEST(RelateEdgeTest, SetAllLocationsSetsAllPositionsForTargetGeometryOnly) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY dir_pt(0.0, -1.0);
  const RelateNode node(&node_pt);

  RelateEdge edge(&node, &dir_pt, kGeomA);
  edge.setAllLocations(kGeomB, Location::BOUNDARY);
  ExpectLocations(edge, kGeomA, Location::EXTERIOR, Location::INTERIOR,
                  Location::EXTERIOR);
  ExpectLocations(edge, kGeomB, Location::BOUNDARY, Location::BOUNDARY,
                  Location::BOUNDARY);

  edge.setAllLocations(kGeomA, Location::NONE);
  ExpectLocations(edge, kGeomA, Location::NONE, Location::NONE, Location::NONE);
  ExpectLocations(edge, kGeomB, Location::BOUNDARY, Location::BOUNDARY,
                  Location::BOUNDARY);
}

TEST(RelateEdgeTest, SetUnknownLocationsUpdatesOnlyNonePositions) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY dir_pt(2.0, 2.0);
  const RelateNode node(&node_pt);

  // All unknown -> all filled.
  {
    RelateEdge edge(&node, &dir_pt, kGeomA, kForward);
    edge.setUnknownLocations(kGeomB, Location::EXTERIOR);
    ExpectLocations(edge, kGeomB, Location::EXTERIOR, Location::EXTERIOR,
                    Location::EXTERIOR);
  }

  // All known -> none overwritten.
  {
    RelateEdge edge(&node, &dir_pt, kGeomA, kForward);
    edge.setUnknownLocations(kGeomA, Location::NONE);
    ExpectLocations(edge, kGeomA, Location::EXTERIOR, Location::BOUNDARY,
                    Location::INTERIOR);
  }

  // Partially unknown slots -> only NONE slots updated.
  {
    RelateEdge edge(&node, &dir_pt, kGeomA, Location::NONE, Location::INTERIOR,
                    Location::BOUNDARY);
    edge.setUnknownLocations(kGeomA, Location::EXTERIOR);
    ExpectLocations(edge, kGeomA, Location::EXTERIOR, Location::BOUNDARY,
                    Location::INTERIOR);
  }
  {
    RelateEdge edge(&node, &dir_pt, kGeomB, Location::EXTERIOR, Location::NONE,
                    Location::INTERIOR);
    edge.setUnknownLocations(kGeomB, Location::BOUNDARY);
    ExpectLocations(edge, kGeomB, Location::EXTERIOR, Location::INTERIOR,
                    Location::BOUNDARY);
  }
  {
    RelateEdge edge(&node, &dir_pt, kGeomA, Location::INTERIOR,
                    Location::EXTERIOR, Location::NONE);
    edge.setUnknownLocations(kGeomA, Location::BOUNDARY);
    ExpectLocations(edge, kGeomA, Location::INTERIOR, Location::BOUNDARY,
                    Location::EXTERIOR);
  }
}

TEST(RelateEdgeTest, SetDimLocationsUpdatesTargetGeometryOnly) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY dir_pt(1.0, 0.0);
  const RelateNode node(&node_pt);

  RelateEdge edge(&node, &dir_pt, kGeomA, kForward);
  edge.setDimLocations(kGeomB, Dimension::L, Location::INTERIOR);
  ExpectLocations(edge, kGeomA, Location::EXTERIOR, Location::BOUNDARY,
                  Location::INTERIOR);
  ExpectLocations(edge, kGeomB, Location::INTERIOR, Location::INTERIOR,
                  Location::INTERIOR);

  edge.setDimLocations(kGeomA, Dimension::P, Location::BOUNDARY);
  ExpectLocations(edge, kGeomA, Location::BOUNDARY, Location::BOUNDARY,
                  Location::BOUNDARY);
  ExpectLocations(edge, kGeomB, Location::INTERIOR, Location::INTERIOR,
                  Location::INTERIOR);
}

TEST(RelateEdgeTest, ToStringAndStreamOperatorFormatLineAndLabels) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY dir_pt(3.0, 4.0);
  const RelateNode node(&node_pt);

  const RelateEdge line_a(&node, &dir_pt, kGeomA);
  EXPECT_EQ(line_a.toString(), "LINESTRING (0 0, 3 4) - A:eie/B:---");

  RelateEdge merged(&node, &dir_pt, kGeomA, kForward);
  EXPECT_EQ(merged.toString(), "LINESTRING (0 0, 3 4) - A:ebi/B:---");

  merged.merge(kGeomB, Dimension::L, kForward);
  EXPECT_EQ(merged.toString(), "LINESTRING (0 0, 3 4) - A:ebi/B:eie");

  RelateEdge custom(&node, &dir_pt, kGeomA, Location::BOUNDARY,
                    Location::INTERIOR, Location::NONE);
  custom.setLocation(kGeomB, Position::LEFT, Location::EXTERIOR);
  custom.setLocation(kGeomB, Position::ON, Location::BOUNDARY);
  EXPECT_EQ(custom.toString(), "LINESTRING (0 0, 3 4) - A:b-i/B:eb-");

  const RelateEdge area_b_rev(&node, &dir_pt, kGeomB, kReverse);
  EXPECT_EQ(area_b_rev.toString(), "LINESTRING (0 0, 3 4) - A:---/B:ibe");

  std::ostringstream oss;
  oss << line_a << " | " << area_b_rev;
  EXPECT_EQ(oss.str(),
            "LINESTRING (0 0, 3 4) - A:eie/B:--- | "
            "LINESTRING (0 0, 3 4) - A:---/B:ibe");
}

}  // namespace
}  // namespace geos_google
