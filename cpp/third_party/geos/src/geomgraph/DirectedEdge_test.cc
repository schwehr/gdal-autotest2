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

// Unit tests for third_party/geos/src/geomgraph/DirectedEdge.cpp and
// third_party/geos/include/geos/geomgraph/DirectedEdge.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geomgraph/DirectedEdge.cpp

#include "third_party/geos/include/geos/geomgraph/DirectedEdge.h"

#include <array>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/EdgeRing.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Position;
using geos::geomgraph::DirectedEdge;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeRing;
using geos::geomgraph::Label;
using geos::util::TopologyException;
using testing::EndsWith;
using testing::HasSubstr;
using testing::Not;
using testing::StartsWith;

constexpr int kPosOn = Position::ON;
constexpr int kPosLeft = Position::LEFT;
constexpr int kPosRight = Position::RIGHT;

class StubEdgeRing final : public EdgeRing {
 public:
  StubEdgeRing(DirectedEdge* start, const GeometryFactory* gf)
      : EdgeRing(start, gf) {}

  DirectedEdge* getNext(DirectedEdge* de) override { return de->getNext(); }

  void setEdgeRing(DirectedEdge* de, EdgeRing* er) override {
    de->setEdgeRing(er);
  }
};

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords,
                               const Label& label, int depth_delta = 0) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  auto edge = std::make_unique<Edge>(seq.release(), label);
  edge->setDepthDelta(depth_delta);
  return edge;
}

TEST(DirectedEdgeTest, DepthFactorCoversAllLocationPairs) {
  struct DepthFactorCase {
    Location curr;
    Location next;
    int expected;
  };
  constexpr auto kCases = std::to_array<DepthFactorCase>({
      {.curr = Location::NONE, .next = Location::NONE, .expected = 0},
      {.curr = Location::NONE, .next = Location::INTERIOR, .expected = 0},
      {.curr = Location::NONE, .next = Location::BOUNDARY, .expected = 0},
      {.curr = Location::NONE, .next = Location::EXTERIOR, .expected = 0},
      {.curr = Location::INTERIOR, .next = Location::NONE, .expected = 0},
      {.curr = Location::INTERIOR, .next = Location::INTERIOR, .expected = 0},
      {.curr = Location::INTERIOR, .next = Location::BOUNDARY, .expected = 0},
      {.curr = Location::INTERIOR, .next = Location::EXTERIOR, .expected = -1},
      {.curr = Location::BOUNDARY, .next = Location::NONE, .expected = 0},
      {.curr = Location::BOUNDARY, .next = Location::INTERIOR, .expected = 0},
      {.curr = Location::BOUNDARY, .next = Location::BOUNDARY, .expected = 0},
      {.curr = Location::BOUNDARY, .next = Location::EXTERIOR, .expected = 0},
      {.curr = Location::EXTERIOR, .next = Location::NONE, .expected = 0},
      {.curr = Location::EXTERIOR, .next = Location::INTERIOR, .expected = 1},
      {.curr = Location::EXTERIOR, .next = Location::BOUNDARY, .expected = 0},
      {.curr = Location::EXTERIOR, .next = Location::EXTERIOR, .expected = 0},
  });

  for (const auto& tc : kCases) {
    EXPECT_EQ(DirectedEdge::depthFactor(tc.curr, tc.next), tc.expected);
  }
}

TEST(DirectedEdgeTest,
     ConstructorInitializesDirectionAndFlipsLabelWhenReverse) {
  Label area_label(0, Location::BOUNDARY, Location::INTERIOR,
                   Location::EXTERIOR);
  area_label.setLocation(1, kPosOn, Location::INTERIOR);
  area_label.setLocation(1, kPosLeft, Location::EXTERIOR);
  area_label.setLocation(1, kPosRight, Location::BOUNDARY);

  auto edge = MakeEdge(
      {Coordinate(0.0, 0.0), Coordinate(3.0, 4.0), Coordinate(10.0, 5.0)},
      area_label);

  DirectedEdge fwd(edge.get(), true);
  const DirectedEdge& const_fwd = fwd;
  EXPECT_EQ(fwd.getEdge(), edge.get());
  EXPECT_TRUE(const_fwd.isForward());
  EXPECT_FALSE(const_fwd.isInResult());
  EXPECT_FALSE(const_fwd.isVisited());
  EXPECT_EQ(const_fwd.getSym(), nullptr);
  EXPECT_EQ(const_fwd.getNext(), nullptr);
  EXPECT_EQ(const_fwd.getNextMin(), nullptr);
  EXPECT_EQ(const_fwd.getEdgeRing(), nullptr);
  EXPECT_EQ(const_fwd.getMinEdgeRing(), nullptr);
  EXPECT_EQ(const_fwd.getDepth(kPosOn), 0);
  EXPECT_EQ(const_fwd.getDepth(kPosLeft), -999);
  EXPECT_EQ(const_fwd.getDepth(kPosRight), -999);
  EXPECT_TRUE(const_fwd.getCoordinate().equals2D(Coordinate(0.0, 0.0)));
  EXPECT_TRUE(fwd.getDirectedCoordinate().equals2D(Coordinate(3.0, 4.0)));
  EXPECT_DOUBLE_EQ(fwd.getDx(), 3.0);
  EXPECT_DOUBLE_EQ(fwd.getDy(), 4.0);
  EXPECT_EQ(fwd.getQuadrant(), 0);
  EXPECT_EQ(const_fwd.getLabel().getLocation(0, kPosOn), Location::BOUNDARY);
  EXPECT_EQ(const_fwd.getLabel().getLocation(0, kPosLeft), Location::INTERIOR);
  EXPECT_EQ(const_fwd.getLabel().getLocation(0, kPosRight), Location::EXTERIOR);
  EXPECT_EQ(const_fwd.getLabel().getLocation(1, kPosOn), Location::INTERIOR);
  EXPECT_EQ(const_fwd.getLabel().getLocation(1, kPosLeft), Location::EXTERIOR);
  EXPECT_EQ(const_fwd.getLabel().getLocation(1, kPosRight), Location::BOUNDARY);

  DirectedEdge bwd(edge.get(), false);
  const DirectedEdge& const_bwd = bwd;
  EXPECT_EQ(bwd.getEdge(), edge.get());
  EXPECT_FALSE(const_bwd.isForward());
  EXPECT_EQ(const_bwd.getDepth(kPosOn), 0);
  EXPECT_EQ(const_bwd.getDepth(kPosLeft), -999);
  EXPECT_EQ(const_bwd.getDepth(kPosRight), -999);
  EXPECT_TRUE(const_bwd.getCoordinate().equals2D(Coordinate(10.0, 5.0)));
  EXPECT_TRUE(bwd.getDirectedCoordinate().equals2D(Coordinate(3.0, 4.0)));
  EXPECT_DOUBLE_EQ(bwd.getDx(), -7.0);
  EXPECT_DOUBLE_EQ(bwd.getDy(), -1.0);
  EXPECT_EQ(bwd.getQuadrant(), 2);
  EXPECT_EQ(const_bwd.getLabel().getLocation(0, kPosOn), Location::BOUNDARY);
  EXPECT_EQ(const_bwd.getLabel().getLocation(0, kPosLeft), Location::EXTERIOR);
  EXPECT_EQ(const_bwd.getLabel().getLocation(0, kPosRight), Location::INTERIOR);
  EXPECT_EQ(const_bwd.getLabel().getLocation(1, kPosOn), Location::INTERIOR);
  EXPECT_EQ(const_bwd.getLabel().getLocation(1, kPosLeft), Location::BOUNDARY);
  EXPECT_EQ(const_bwd.getLabel().getLocation(1, kPosRight), Location::EXTERIOR);
}

TEST(DirectedEdgeTest, TwoPointEdgeInitializesForwardAndBackwardEndpoints) {
  auto edge = MakeEdge({Coordinate(1.0, 2.0), Coordinate(-2.0, 6.0)},
                       Label(Location::INTERIOR));
  DirectedEdge fwd(edge.get(), true);
  DirectedEdge bwd(edge.get(), false);

  EXPECT_TRUE(fwd.getCoordinate().equals2D(Coordinate(1.0, 2.0)));
  EXPECT_TRUE(fwd.getDirectedCoordinate().equals2D(Coordinate(-2.0, 6.0)));
  EXPECT_TRUE(bwd.getCoordinate().equals2D(Coordinate(-2.0, 6.0)));
  EXPECT_TRUE(bwd.getDirectedCoordinate().equals2D(Coordinate(1.0, 2.0)));
  EXPECT_EQ(fwd.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(bwd.getLabel().getLocation(0), Location::INTERIOR);
}

TEST(DirectedEdgeTest, AccessorsAndVisitedEdgeManageStateAndSymLinks) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto edge = MakeEdge({Coordinate(0.0, 0.0), Coordinate(1.0, 0.0)},
                       Label(Location::BOUNDARY));
  DirectedEdge fwd(edge.get(), true);
  DirectedEdge bwd(edge.get(), false);
  DirectedEdge other(edge.get(), true);
  StubEdgeRing ring(&fwd, factory.get());
  StubEdgeRing min_ring(&fwd, factory.get());

  fwd.setInResult(true);
  EXPECT_TRUE(fwd.isInResult());
  fwd.setInResult(false);
  EXPECT_FALSE(fwd.isInResult());

  fwd.setVisited(true);
  EXPECT_TRUE(fwd.isVisited());
  EXPECT_FALSE(bwd.isVisited());
  fwd.setVisited(false);
  EXPECT_FALSE(fwd.isVisited());

  fwd.setSym(&bwd);
  bwd.setSym(&fwd);
  EXPECT_EQ(fwd.getSym(), &bwd);
  EXPECT_EQ(bwd.getSym(), &fwd);

  fwd.setVisitedEdge(true);
  EXPECT_TRUE(fwd.isVisited());
  EXPECT_TRUE(bwd.isVisited());
  fwd.setVisitedEdge(false);
  EXPECT_FALSE(fwd.isVisited());
  EXPECT_FALSE(bwd.isVisited());

  fwd.setNext(&other);
  fwd.setNextMin(&bwd);
  EXPECT_EQ(fwd.getNext(), &other);
  EXPECT_EQ(fwd.getNextMin(), &bwd);

  absl::Cleanup clear_rings = [&] {
    fwd.setEdgeRing(nullptr);
    fwd.setMinEdgeRing(nullptr);
  };
  fwd.setEdgeRing(&ring);
  fwd.setMinEdgeRing(&min_ring);
  EXPECT_EQ(fwd.getEdgeRing(), &ring);
  EXPECT_EQ(fwd.getMinEdgeRing(), &min_ring);
}

TEST(DirectedEdgeTest, SetDepthSetsInitialAllowsIdempotentAndThrowsOnConflict) {
  auto edge = MakeEdge({Coordinate(5.0, 6.0), Coordinate(7.0, 8.0)},
                       Label(Location::BOUNDARY));
  DirectedEdge de(edge.get(), true);

  // Setting sentinel -999 keeps the slot unassigned.
  de.setDepth(kPosLeft, -999);
  EXPECT_EQ(de.getDepth(kPosLeft), -999);

  de.setDepth(kPosLeft, 2);
  EXPECT_EQ(de.getDepth(kPosLeft), 2);
  de.setDepth(kPosLeft, 2);
  EXPECT_EQ(de.getDepth(kPosLeft), 2);

  try {
    de.setDepth(kPosLeft, 3);
    FAIL() << "Expected TopologyException on mismatched LEFT depth";
  } catch (TopologyException& ex) {
    EXPECT_THAT(ex.what(), HasSubstr("assigned depths do not match"));
    EXPECT_TRUE(ex.getCoordinate().equals2D(de.getCoordinate()));
  }
  EXPECT_EQ(de.getDepth(kPosLeft), 2);

  de.setDepth(kPosRight, -1);
  EXPECT_EQ(de.getDepth(kPosRight), -1);
  EXPECT_THROW(de.setDepth(kPosRight, 0), TopologyException);
  EXPECT_EQ(de.getDepth(kPosRight), -1);

  // Position::ON is initialized to 0.
  de.setDepth(kPosOn, 0);
  EXPECT_EQ(de.getDepth(kPosOn), 0);
  EXPECT_THROW(de.setDepth(kPosOn, 1), TopologyException);
  EXPECT_EQ(de.getDepth(kPosOn), 0);
}

TEST(DirectedEdgeTest, GetDepthDeltaNegatesForReverseEdge) {
  constexpr auto kDeltas = std::to_array<int>({0, 1, -1, 2, -5});
  for (int delta : kDeltas) {
    SCOPED_TRACE(delta);
    auto edge = MakeEdge({Coordinate(0.0, 0.0), Coordinate(1.0, 1.0)},
                         Label(Location::BOUNDARY), delta);
    const DirectedEdge fwd(edge.get(), true);
    const DirectedEdge bwd(edge.get(), false);
    EXPECT_EQ(fwd.getDepthDelta(), delta);
    EXPECT_EQ(bwd.getDepthDelta(), -delta);
  }
}

TEST(DirectedEdgeTest, SetEdgeDepthsComputesOppositeSideForBothOrientations) {
  struct EdgeDepthCase {
    std::string_view name;
    bool is_forward;
    int depth_delta;
    int position;
    int new_depth;
    int expected_left;
    int expected_right;
  };
  constexpr auto kCases = std::to_array<EdgeDepthCase>({
      {.name = "fwd_delta1_from_right",
       .is_forward = true,
       .depth_delta = 1,
       .position = kPosRight,
       .new_depth = 0,
       .expected_left = 1,
       .expected_right = 0},
      {.name = "fwd_delta1_from_left",
       .is_forward = true,
       .depth_delta = 1,
       .position = kPosLeft,
       .new_depth = 1,
       .expected_left = 1,
       .expected_right = 0},
      {.name = "bwd_delta1_from_right",
       .is_forward = false,
       .depth_delta = 1,
       .position = kPosRight,
       .new_depth = 1,
       .expected_left = 0,
       .expected_right = 1},
      {.name = "bwd_delta1_from_left",
       .is_forward = false,
       .depth_delta = 1,
       .position = kPosLeft,
       .new_depth = 0,
       .expected_left = 0,
       .expected_right = 1},
      {.name = "fwd_delta_neg2_from_right",
       .is_forward = true,
       .depth_delta = -2,
       .position = kPosRight,
       .new_depth = 3,
       .expected_left = 1,
       .expected_right = 3},
      {.name = "fwd_delta_neg2_from_left",
       .is_forward = true,
       .depth_delta = -2,
       .position = kPosLeft,
       .new_depth = 1,
       .expected_left = 1,
       .expected_right = 3},
      {.name = "bwd_delta_neg2_from_right",
       .is_forward = false,
       .depth_delta = -2,
       .position = kPosRight,
       .new_depth = 1,
       .expected_left = 3,
       .expected_right = 1},
      {.name = "bwd_delta_neg2_from_left",
       .is_forward = false,
       .depth_delta = -2,
       .position = kPosLeft,
       .new_depth = 3,
       .expected_left = 3,
       .expected_right = 1},
      {.name = "fwd_delta0_from_right",
       .is_forward = true,
       .depth_delta = 0,
       .position = kPosRight,
       .new_depth = 2,
       .expected_left = 2,
       .expected_right = 2},
      {.name = "bwd_delta0_from_left",
       .is_forward = false,
       .depth_delta = 0,
       .position = kPosLeft,
       .new_depth = 2,
       .expected_left = 2,
       .expected_right = 2},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    auto edge = MakeEdge({Coordinate(0.0, 0.0), Coordinate(2.0, 0.0)},
                         Label(Location::BOUNDARY), tc.depth_delta);
    DirectedEdge de(edge.get(), tc.is_forward);
    de.setEdgeDepths(tc.position, tc.new_depth);
    EXPECT_EQ(de.getDepth(kPosLeft), tc.expected_left);
    EXPECT_EQ(de.getDepth(kPosRight), tc.expected_right);

    // Repeating with matching depths succeeds.
    de.setEdgeDepths(tc.position, tc.new_depth);
    EXPECT_EQ(de.getDepth(kPosLeft), tc.expected_left);
    EXPECT_EQ(de.getDepth(kPosRight), tc.expected_right);

    // Conflicting depth throws TopologyException.
    EXPECT_THROW(de.setEdgeDepths(tc.position, tc.new_depth + 1),
                 TopologyException);
  }

  // Conflict detected on the opposite side when primary side was unset.
  auto edge = MakeEdge({Coordinate(0.0, 0.0), Coordinate(2.0, 0.0)},
                       Label(Location::BOUNDARY), 1);
  DirectedEdge de(edge.get(), true);
  de.setDepth(kPosRight, 99);
  EXPECT_THROW(de.setEdgeDepths(kPosLeft, 1), TopologyException);
}

TEST(DirectedEdgeTest, IsLineEdgeHandlesLineAndAreaCombinations) {
  struct LineEdgeCase {
    std::string_view name;
    Label label;
    bool expected;
  };

  Label geom0_line_geom1_ext_area(Location::EXTERIOR, Location::EXTERIOR,
                                  Location::EXTERIOR);
  geom0_line_geom1_ext_area.toLine(0);

  Label geom0_ext_area_geom1_line(Location::EXTERIOR, Location::EXTERIOR,
                                  Location::EXTERIOR);
  geom0_ext_area_geom1_line.toLine(1);

  Label geom0_line_geom1_int_left(Location::EXTERIOR, Location::INTERIOR,
                                  Location::EXTERIOR);
  geom0_line_geom1_int_left.toLine(0);

  Label geom0_line_geom1_int_right(Location::EXTERIOR, Location::EXTERIOR,
                                   Location::INTERIOR);
  geom0_line_geom1_int_right.toLine(0);

  Label geom0_line_geom1_bnd_on(Location::BOUNDARY, Location::EXTERIOR,
                                Location::EXTERIOR);
  geom0_line_geom1_bnd_on.toLine(0);

  Label geom0_bnd_area_geom1_line(Location::BOUNDARY, Location::INTERIOR,
                                  Location::EXTERIOR);
  geom0_bnd_area_geom1_line.toLine(1);

  const std::array<LineEdgeCase, 11> kCases = {{
      {.name = "default_null_line_label", .label = Label(), .expected = true},
      {.name = "both_line_interior",
       .label = Label(Location::INTERIOR),
       .expected = true},
      {.name = "single_geom_line_boundary",
       .label = Label(0, Location::BOUNDARY),
       .expected = true},
      {.name = "geom0_line_geom1_all_exterior_area",
       .label = geom0_line_geom1_ext_area,
       .expected = true},
      {.name = "geom0_all_exterior_area_geom1_line",
       .label = geom0_ext_area_geom1_line,
       .expected = true},
      {.name = "geom0_line_geom1_area_interior_left",
       .label = geom0_line_geom1_int_left,
       .expected = false},
      {.name = "geom0_line_geom1_area_interior_right",
       .label = geom0_line_geom1_int_right,
       .expected = false},
      {.name = "geom0_line_geom1_area_boundary_on",
       .label = geom0_line_geom1_bnd_on,
       .expected = false},
      {.name = "geom0_area_boundary_geom1_line",
       .label = geom0_bnd_area_geom1_line,
       .expected = false},
      {.name = "both_area_all_exterior",
       .label =
           Label(Location::EXTERIOR, Location::EXTERIOR, Location::EXTERIOR),
       .expected = false},
      {.name = "both_area_boundary",
       .label =
           Label(Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR),
       .expected = false},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    auto edge =
        MakeEdge({Coordinate(0.0, 0.0), Coordinate(1.0, 0.0)}, tc.label);
    DirectedEdge fwd(edge.get(), true);
    DirectedEdge bwd(edge.get(), false);
    EXPECT_EQ(fwd.isLineEdge(), tc.expected);
    EXPECT_EQ(bwd.isLineEdge(), tc.expected);
  }
}

TEST(DirectedEdgeTest, IsInteriorAreaEdgeRequiresBothGeometriesInteriorSides) {
  struct InteriorAreaCase {
    std::string_view name;
    Label label;
    bool expected;
  };

  Label geom0_line(Location::INTERIOR, Location::INTERIOR, Location::INTERIOR);
  geom0_line.toLine(0);

  Label geom1_line(Location::INTERIOR, Location::INTERIOR, Location::INTERIOR);
  geom1_line.toLine(1);

  Label geom0_left_ext(Location::BOUNDARY, Location::INTERIOR,
                       Location::INTERIOR);
  geom0_left_ext.setLocation(0, kPosLeft, Location::EXTERIOR);

  Label geom0_right_ext(Location::BOUNDARY, Location::INTERIOR,
                        Location::INTERIOR);
  geom0_right_ext.setLocation(0, kPosRight, Location::EXTERIOR);

  Label geom1_left_bnd(Location::BOUNDARY, Location::INTERIOR,
                       Location::INTERIOR);
  geom1_left_bnd.setLocation(1, kPosLeft, Location::BOUNDARY);

  Label geom1_right_ext(Location::BOUNDARY, Location::INTERIOR,
                        Location::INTERIOR);
  geom1_right_ext.setLocation(1, kPosRight, Location::EXTERIOR);

  const std::array<InteriorAreaCase, 10> kCases = {{
      {.name = "both_area_boundary_on_interior_sides",
       .label =
           Label(Location::BOUNDARY, Location::INTERIOR, Location::INTERIOR),
       .expected = true},
      {.name = "both_area_all_interior",
       .label =
           Label(Location::INTERIOR, Location::INTERIOR, Location::INTERIOR),
       .expected = true},
      {.name = "both_area_exterior_on_interior_sides",
       .label =
           Label(Location::EXTERIOR, Location::INTERIOR, Location::INTERIOR),
       .expected = true},
      {.name = "geom0_is_line", .label = geom0_line, .expected = false},
      {.name = "geom1_is_line", .label = geom1_line, .expected = false},
      {.name = "both_are_lines",
       .label = Label(Location::INTERIOR),
       .expected = false},
      {.name = "geom0_left_not_interior",
       .label = geom0_left_ext,
       .expected = false},
      {.name = "geom0_right_not_interior",
       .label = geom0_right_ext,
       .expected = false},
      {.name = "geom1_left_not_interior",
       .label = geom1_left_bnd,
       .expected = false},
      {.name = "geom1_right_not_interior",
       .label = geom1_right_ext,
       .expected = false},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    auto edge =
        MakeEdge({Coordinate(0.0, 0.0), Coordinate(1.0, 0.0)}, tc.label);
    DirectedEdge fwd(edge.get(), true);
    DirectedEdge bwd(edge.get(), false);
    EXPECT_EQ(fwd.isInteriorAreaEdge(), tc.expected);
    EXPECT_EQ(bwd.isInteriorAreaEdge(), tc.expected);
  }
}

TEST(DirectedEdgeTest, PrintAndPrintEdgeFormatStateCorrectly) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto edge = MakeEdge(
      {Coordinate(0.0, 0.0), Coordinate(1.0, 0.0), Coordinate(1.0, 1.0)},
      Label(Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR), 0);

  DirectedEdge fwd(edge.get(), true);
  DirectedEdge bwd(edge.get(), false);

  const std::string initial_print = fwd.print();
  EXPECT_THAT(initial_print, StartsWith(fwd.EdgeEnd::print()));
  EXPECT_THAT(initial_print, HasSubstr(" -999/-999 (0)"));
  EXPECT_THAT(initial_print, Not(HasSubstr(" inResult")));
  EXPECT_THAT(initial_print, HasSubstr(" EdgeRing: "));
  EXPECT_THAT(initial_print, Not(HasSubstr(" (EdgeRing[")));

  EXPECT_EQ(fwd.printEdge(), edge->print());
  EXPECT_THAT(fwd.printEdge(), HasSubstr("LINESTRING"));
  EXPECT_EQ(bwd.printEdge(), edge->printReverse());
  EXPECT_THAT(bwd.printEdge(), HasSubstr("EDGE (rev)"));

  edge->setDepthDelta(1);
  fwd.setEdgeDepths(kPosRight, 0);
  fwd.setInResult(true);
  StubEdgeRing ring(&fwd, factory.get());
  absl::Cleanup clear_ring = [&] { fwd.setEdgeRing(nullptr); };
  fwd.setEdgeRing(&ring);

  std::ostringstream expected_ring_suffix;
  expected_ring_suffix << " EdgeRing: " << &ring << " (" << ring << ")";

  const std::string full_print = fwd.print();
  EXPECT_THAT(full_print, StartsWith(fwd.EdgeEnd::print()));
  EXPECT_THAT(full_print, HasSubstr(" 1/0 (1) inResult"));
  EXPECT_THAT(full_print, EndsWith(expected_ring_suffix.str()));

  bwd.setEdgeDepths(kPosRight, 1);
  const std::string bwd_print = bwd.print();
  EXPECT_THAT(bwd_print, HasSubstr(" 0/1 (-1)"));
}

}  // namespace
}  // namespace geos_google
