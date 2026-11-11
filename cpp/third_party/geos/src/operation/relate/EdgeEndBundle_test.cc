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

// Unit tests for third_party/geos/src/operation/relate/EdgeEndBundle.cpp and
// third_party/geos/include/geos/operation/relate/EdgeEndBundle.h.

#include "third_party/geos/include/geos/operation/relate/EdgeEndBundle.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geom/Quadrant.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Dimension;
using geos::geom::IntersectionMatrix;
using geos::geom::Location;
using geos::geom::Position;
using geos::geom::Quadrant;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeEnd;
using geos::geomgraph::Label;
using geos::operation::relate::EdgeEndBundle;

class CountingEdgeEnd final : public EdgeEnd {
 public:
  CountingEdgeEnd(Edge* edge, const Coordinate& p0, const Coordinate& p1,
                  const Label& label, int* destroyed_count)
      : EdgeEnd(edge, p0, p1, label), destroyed_count_(destroyed_count) {}

  ~CountingEdgeEnd() override { ++(*destroyed_count_); }

 private:
  int* destroyed_count_;
};

std::unique_ptr<Edge> MakeSegmentEdge(const Coordinate& p0,
                                      const Coordinate& p1,
                                      const Label& label) {
  return std::make_unique<Edge>(new CoordinateSequence{p0, p1}, label);
}

TEST(EdgeEndBundleTest, ConstructorCopiesMetadataAndTakesOwnership) {
  int destroyed = 0;
  Label init_label(0, Location::BOUNDARY, Location::INTERIOR,
                   Location::EXTERIOR);
  auto edge = MakeSegmentEdge(Coordinate(1, 2), Coordinate(4, 6), init_label);

  {
    auto* raw = new CountingEdgeEnd(edge.get(), Coordinate(1, 2),
                                    Coordinate(4, 6), init_label, &destroyed);
    EdgeEndBundle bundle(raw);

    EXPECT_EQ(bundle.getEdge(), edge.get());
    EXPECT_TRUE(bundle.getCoordinate().equals2D(Coordinate(1, 2)));
    EXPECT_TRUE(bundle.getDirectedCoordinate().equals2D(Coordinate(4, 6)));
    EXPECT_DOUBLE_EQ(bundle.getDx(), 3.0);
    EXPECT_DOUBLE_EQ(bundle.getDy(), 4.0);
    EXPECT_EQ(bundle.getQuadrant(), +Quadrant::NE);

    ASSERT_EQ(bundle.getEdgeEnds().size(), 1u);
    EXPECT_EQ(bundle.getEdgeEnds()[0], raw);
    EXPECT_TRUE(bundle.getLabel().isArea(0));
    EXPECT_EQ(bundle.getLabel().getLocation(0, Position::ON),
              Location::BOUNDARY);
    EXPECT_EQ(bundle.getLabel().getLocation(0, Position::LEFT),
              Location::INTERIOR);
    EXPECT_EQ(bundle.getLabel().getLocation(0, Position::RIGHT),
              Location::EXTERIOR);
    EXPECT_EQ(destroyed, 0);
  }
  EXPECT_EQ(destroyed, 1);

  // Deleting an EdgeEndBundle via a base EdgeEnd* invokes the virtual
  // destructor and frees all owned EdgeEnds.
  destroyed = 0;
  {
    std::unique_ptr<EdgeEnd> base = std::make_unique<EdgeEndBundle>(
        new CountingEdgeEnd(edge.get(), Coordinate(1, 2), Coordinate(4, 6),
                            init_label, &destroyed));
    EXPECT_EQ(destroyed, 0);
  }
  EXPECT_EQ(destroyed, 1);
}

TEST(EdgeEndBundleTest, InsertAppendsInOrderAndDestructorDeletesAll) {
  int destroyed = 0;
  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(10, 0),
                              Label(0, Location::INTERIOR));

  {
    auto* e0 =
        new CountingEdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(10, 0),
                            Label(0, Location::INTERIOR), &destroyed);
    auto* e1 =
        new CountingEdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(5, 0),
                            Label(0, Location::BOUNDARY), &destroyed);
    auto* e2 =
        new CountingEdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(2, 0),
                            Label(1, Location::INTERIOR), &destroyed);

    EdgeEndBundle bundle(e0);
    bundle.insert(e1);
    bundle.insert(e2);

    const std::vector<EdgeEnd*>& members = bundle.getEdgeEnds();
    ASSERT_EQ(members.size(), 3u);
    EXPECT_EQ(members[0], e0);
    EXPECT_EQ(members[1], e1);
    EXPECT_EQ(members[2], e2);
    EXPECT_EQ(destroyed, 0);
  }
  EXPECT_EQ(destroyed, 3);
}

TEST(EdgeEndBundleTest, ComputeLabelLineEdgesNoneExteriorInteriorAndBoundary) {
  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(5, 0), Label());

  // NONE and EXTERIOR produce NONE ON-location; neither is INTERIOR/BOUNDARY.
  {
    EdgeEndBundle bundle(new EdgeEnd(edge.get(), Coordinate(0, 0),
                                     Coordinate(5, 0), Label(Location::NONE)));
    bundle.insert(new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(5, 0),
                              Label(Location::EXTERIOR)));
    EdgeEnd* base = &bundle;
    base->computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());

    EXPECT_FALSE(bundle.getLabel().isArea());
    EXPECT_TRUE(bundle.getLabel().isLine(0));
    EXPECT_TRUE(bundle.getLabel().isLine(1));
    EXPECT_EQ(bundle.getLabel().getLocation(0), Location::NONE);
    EXPECT_EQ(bundle.getLabel().getLocation(1), Location::NONE);
  }

  // INTERIOR for geom 0, BOUNDARY for geom 1.
  {
    EdgeEndBundle bundle(new EdgeEnd(edge.get(), Coordinate(0, 0),
                                     Coordinate(5, 0),
                                     Label(0, Location::INTERIOR)));
    bundle.insert(new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(5, 0),
                              Label(1, Location::BOUNDARY)));
    bundle.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());

    EXPECT_FALSE(bundle.getLabel().isArea());
    EXPECT_EQ(bundle.getLabel().getLocation(0), Location::INTERIOR);
    EXPECT_EQ(bundle.getLabel().getLocation(1), Location::BOUNDARY);
  }

  // Boundary takes precedence over Interior when both occur in geom 0.
  {
    EdgeEndBundle bundle(new EdgeEnd(edge.get(), Coordinate(0, 0),
                                     Coordinate(5, 0),
                                     Label(0, Location::INTERIOR)));
    bundle.insert(new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(5, 0),
                              Label(0, Location::BOUNDARY)));
    bundle.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());

    EXPECT_EQ(bundle.getLabel().getLocation(0), Location::BOUNDARY);
    EXPECT_EQ(bundle.getLabel().getLocation(1), Location::NONE);
  }
}

TEST(EdgeEndBundleTest, ComputeLabelRespectsBoundaryNodeRules) {
  struct RuleCase {
    int boundary_count;
    const BoundaryNodeRule* rule;
    Location expected;
  };
  const RuleCase cases[] = {
      {1, &BoundaryNodeRule::getBoundaryRuleMod2(), Location::BOUNDARY},
      {2, &BoundaryNodeRule::getBoundaryRuleMod2(), Location::INTERIOR},
      {3, &BoundaryNodeRule::getBoundaryRuleMod2(), Location::BOUNDARY},
      {1, &BoundaryNodeRule::getBoundaryOGCSFS(), Location::BOUNDARY},
      {2, &BoundaryNodeRule::getBoundaryOGCSFS(), Location::INTERIOR},
      {1, &BoundaryNodeRule::getBoundaryEndPoint(), Location::BOUNDARY},
      {2, &BoundaryNodeRule::getBoundaryEndPoint(), Location::BOUNDARY},
      {3, &BoundaryNodeRule::getBoundaryEndPoint(), Location::BOUNDARY},
      {1, &BoundaryNodeRule::getBoundaryMultivalentEndPoint(),
       Location::INTERIOR},
      {2, &BoundaryNodeRule::getBoundaryMultivalentEndPoint(),
       Location::BOUNDARY},
      {3, &BoundaryNodeRule::getBoundaryMultivalentEndPoint(),
       Location::BOUNDARY},
      {1, &BoundaryNodeRule::getBoundaryMonovalentEndPoint(),
       Location::BOUNDARY},
      {2, &BoundaryNodeRule::getBoundaryMonovalentEndPoint(),
       Location::INTERIOR},
      {3, &BoundaryNodeRule::getBoundaryMonovalentEndPoint(),
       Location::INTERIOR},
  };

  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(5, 0), Label());
  for (const RuleCase& tc : cases) {
    EdgeEndBundle bundle(new EdgeEnd(edge.get(), Coordinate(0, 0),
                                     Coordinate(5, 0),
                                     Label(0, Location::BOUNDARY)));
    for (int i = 1; i < tc.boundary_count; ++i) {
      bundle.insert(new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(5, 0),
                                Label(0, Location::BOUNDARY)));
    }
    bundle.computeLabel(*tc.rule);
    EXPECT_EQ(bundle.getLabel().getLocation(0), tc.expected)
        << "boundary_count=" << tc.boundary_count
        << ", rule=" << tc.rule->toString();
    EXPECT_EQ(bundle.getLabel().getLocation(1), Location::NONE);
  }
}

TEST(EdgeEndBundleTest, ComputeLabelSingleAreaEdgePreservesSides) {
  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(5, 0), Label());
  EdgeEndBundle bundle(new EdgeEnd(
      edge.get(), Coordinate(0, 0), Coordinate(5, 0),
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR)));
  bundle.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());

  EXPECT_TRUE(bundle.getLabel().isArea());
  EXPECT_EQ(bundle.getLabel().getLocation(0, Position::ON), Location::BOUNDARY);
  EXPECT_EQ(bundle.getLabel().getLocation(0, Position::LEFT),
            Location::INTERIOR);
  EXPECT_EQ(bundle.getLabel().getLocation(0, Position::RIGHT),
            Location::EXTERIOR);
  EXPECT_EQ(bundle.getLabel().getLocation(1, Position::ON), Location::NONE);
  EXPECT_EQ(bundle.getLabel().getLocation(1, Position::LEFT), Location::NONE);
  EXPECT_EQ(bundle.getLabel().getLocation(1, Position::RIGHT), Location::NONE);
}

TEST(EdgeEndBundleTest, ComputeLabelTwoTouchingPolygonsInteriorPrimacy) {
  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(5, 0), Label());
  // First edge: LEFT=INTERIOR, RIGHT=EXTERIOR.
  // Second edge: LEFT=EXTERIOR, RIGHT=INTERIOR.
  // Tests both early-return (INTERIOR before EXTERIOR on LEFT) and overwrite
  // (EXTERIOR before INTERIOR on RIGHT).
  EdgeEndBundle bundle(new EdgeEnd(
      edge.get(), Coordinate(0, 0), Coordinate(5, 0),
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR)));
  bundle.insert(new EdgeEnd(
      edge.get(), Coordinate(0, 0), Coordinate(5, 0),
      Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR)));

  bundle.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());
  EXPECT_EQ(bundle.getLabel().getLocation(0, Position::ON), Location::INTERIOR);
  EXPECT_EQ(bundle.getLabel().getLocation(0, Position::LEFT),
            Location::INTERIOR);
  EXPECT_EQ(bundle.getLabel().getLocation(0, Position::RIGHT),
            Location::INTERIOR);

  // Under EndPoint rule, boundaryCount == 2 remains BOUNDARY.
  bundle.computeLabel(BoundaryNodeRule::getBoundaryEndPoint());
  EXPECT_EQ(bundle.getLabel().getLocation(0, Position::ON), Location::BOUNDARY);
  EXPECT_EQ(bundle.getLabel().getLocation(0, Position::LEFT),
            Location::INTERIOR);
  EXPECT_EQ(bundle.getLabel().getLocation(0, Position::RIGHT),
            Location::INTERIOR);

  // Early return on INTERIOR prevents a trailing EXTERIOR edge from
  // overwriting the side location.
  {
    EdgeEndBundle trailing_ext(new EdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(5, 0),
        Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::EXTERIOR)));
    trailing_ext.insert(new EdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(5, 0),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::INTERIOR)));
    trailing_ext.insert(new EdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(5, 0),
        Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::EXTERIOR)));
    trailing_ext.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());
    EXPECT_EQ(trailing_ext.getLabel().getLocation(0, Position::ON),
              Location::BOUNDARY);
    EXPECT_EQ(trailing_ext.getLabel().getLocation(0, Position::LEFT),
              Location::INTERIOR);
    EXPECT_EQ(trailing_ext.getLabel().getLocation(0, Position::RIGHT),
              Location::INTERIOR);
  }
}

TEST(EdgeEndBundleTest, ComputeLabelAreaSidesExteriorNoneAndBothGeometries) {
  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(5, 0), Label());

  // Side with NONE followed by EXTERIOR resolves to EXTERIOR; all-NONE side
  // stays NONE.
  {
    EdgeEndBundle bundle(new EdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(5, 0),
        Label(0, Location::BOUNDARY, Location::NONE, Location::EXTERIOR)));
    bundle.insert(new EdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(5, 0),
        Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::NONE)));
    bundle.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());

    EXPECT_EQ(bundle.getLabel().getLocation(0, Position::ON),
              Location::INTERIOR);
    EXPECT_EQ(bundle.getLabel().getLocation(0, Position::LEFT),
              Location::EXTERIOR);
    EXPECT_EQ(bundle.getLabel().getLocation(0, Position::RIGHT),
              Location::EXTERIOR);
    EXPECT_EQ(bundle.getLabel().getLocation(1, Position::ON), Location::NONE);
    EXPECT_EQ(bundle.getLabel().getLocation(1, Position::LEFT), Location::NONE);
    EXPECT_EQ(bundle.getLabel().getLocation(1, Position::RIGHT),
              Location::NONE);
  }

  // Area edges from both geometry 0 and geometry 1 preserve each geometry's
  // sides independently.
  {
    EdgeEndBundle bundle(new EdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(5, 0),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR)));
    bundle.insert(new EdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(5, 0),
        Label(1, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR)));
    bundle.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());

    EXPECT_EQ(bundle.getLabel().getLocation(0, Position::ON),
              Location::BOUNDARY);
    EXPECT_EQ(bundle.getLabel().getLocation(0, Position::LEFT),
              Location::INTERIOR);
    EXPECT_EQ(bundle.getLabel().getLocation(0, Position::RIGHT),
              Location::EXTERIOR);
    EXPECT_EQ(bundle.getLabel().getLocation(1, Position::ON),
              Location::BOUNDARY);
    EXPECT_EQ(bundle.getLabel().getLocation(1, Position::LEFT),
              Location::EXTERIOR);
    EXPECT_EQ(bundle.getLabel().getLocation(1, Position::RIGHT),
              Location::INTERIOR);
  }
}

TEST(EdgeEndBundleTest, ComputeLabelMixedLineAndAreaEdges) {
  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(5, 0), Label());
  // Put line edges before and alongside an area edge so computeLabelSides
  // exercises both non-area and area branches.
  EdgeEndBundle bundle(new EdgeEnd(edge.get(), Coordinate(0, 0),
                                   Coordinate(5, 0),
                                   Label(1, Location::INTERIOR)));
  bundle.insert(new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(5, 0),
                            Label(0, Location::BOUNDARY)));
  bundle.insert(new EdgeEnd(
      edge.get(), Coordinate(0, 0), Coordinate(5, 0),
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR)));

  bundle.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());
  EXPECT_TRUE(bundle.getLabel().isArea());
  // Geom 0 has two BOUNDARY edges (line + area) -> INTERIOR under Mod2.
  EXPECT_EQ(bundle.getLabel().getLocation(0, Position::ON), Location::INTERIOR);
  EXPECT_EQ(bundle.getLabel().getLocation(0, Position::LEFT),
            Location::INTERIOR);
  EXPECT_EQ(bundle.getLabel().getLocation(0, Position::RIGHT),
            Location::EXTERIOR);
  // Geom 1 has only a line INTERIOR edge -> ON=INTERIOR, sides=NONE.
  EXPECT_EQ(bundle.getLabel().getLocation(1, Position::ON), Location::INTERIOR);
  EXPECT_EQ(bundle.getLabel().getLocation(1, Position::LEFT), Location::NONE);
  EXPECT_EQ(bundle.getLabel().getLocation(1, Position::RIGHT), Location::NONE);
}

TEST(EdgeEndBundleTest, UpdateIMWritesLineAreaAndSkipsUnpairedLocations) {
  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(5, 0), Label());

  // Line bundle: (BOUNDARY, INTERIOR) -> 1.
  {
    EdgeEndBundle bundle(new EdgeEnd(edge.get(), Coordinate(0, 0),
                                     Coordinate(5, 0),
                                     Label(0, Location::BOUNDARY)));
    bundle.insert(new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(5, 0),
                              Label(1, Location::INTERIOR)));
    bundle.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());

    IntersectionMatrix im;
    bundle.updateIM(im);
    EXPECT_EQ(im.get(Location::BOUNDARY, Location::INTERIOR), +Dimension::L);
    EXPECT_EQ(im.toString(), "FFF1FFFFF");

    // Pre-existing higher dimension is not downgraded by updateIM.
    im.set(Location::BOUNDARY, Location::INTERIOR, Dimension::A);
    bundle.updateIM(im);
    EXPECT_EQ(im.get(Location::BOUNDARY, Location::INTERIOR), +Dimension::A);
    EXPECT_EQ(im.toString(), "FFF2FFFFF");
  }

  // Area bundle: (B,B)->1, (I,E)->2, (E,I)->2.
  {
    EdgeEndBundle bundle(new EdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(5, 0),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR)));
    bundle.insert(new EdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(5, 0),
        Label(1, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR)));
    bundle.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());

    IntersectionMatrix im;
    bundle.updateIM(im);
    EXPECT_EQ(im.toString(), "FF2F1F2FF");
  }

  // Mixed area + line bundle: ON=(B,I)->1, unpaired NONE sides are skipped.
  {
    EdgeEndBundle bundle(new EdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(5, 0),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR)));
    bundle.insert(new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(5, 0),
                              Label(1, Location::INTERIOR)));
    bundle.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());

    IntersectionMatrix im;
    bundle.updateIM(im);
    EXPECT_EQ(im.toString(), "FFF1FFFFF");
  }

  // Single-geometry bundle leaves IntersectionMatrix untouched.
  {
    EdgeEndBundle bundle(new EdgeEnd(
        edge.get(), Coordinate(0, 0), Coordinate(5, 0),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR)));
    bundle.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());

    IntersectionMatrix im;
    bundle.updateIM(im);
    EXPECT_EQ(im.toString(), "FFFFFFFFF");
  }
}

TEST(EdgeEndBundleTest, PrintFormatsBundleLabelAndAllContainedEdgeEnds) {
  auto edge = MakeSegmentEdge(Coordinate(0, 0), Coordinate(5, 0), Label());
  auto* e0 = new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(5, 0),
                         Label(0, Location::BOUNDARY));
  auto* e1 = new EdgeEnd(edge.get(), Coordinate(0, 0), Coordinate(10, 0),
                         Label(1, Location::INTERIOR));
  const std::string e0_str = e0->print();
  const std::string e1_str = e1->print();

  EdgeEndBundle bundle(e0);
  bundle.insert(e1);
  bundle.computeLabel(BoundaryNodeRule::getBoundaryRuleMod2());

  const std::string expected =
      "EdgeEndBundle--> Label: " + bundle.getLabel().toString() + "\n" +
      e0_str + "\n" + e1_str + "\n";
  EXPECT_EQ(bundle.print(), expected);

  const EdgeEnd* base = &bundle;
  EXPECT_EQ(base->print(), expected);
}

}  // namespace
}  // namespace geos_google
