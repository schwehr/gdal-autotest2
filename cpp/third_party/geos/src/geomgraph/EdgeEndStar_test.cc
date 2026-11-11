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

// Unit tests for third_party/geos/src/geomgraph/EdgeEndStar.cpp and
// third_party/geos/include/geos/geomgraph/EdgeEndStar.h.

#include "third_party/geos/include/geos/geomgraph/EdgeEndStar.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdge.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdgeStar.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/GeometryGraph.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Position;
using geos::geomgraph::DirectedEdge;
using geos::geomgraph::DirectedEdgeStar;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeEnd;
using geos::geomgraph::EdgeEndStar;
using geos::geomgraph::GeometryGraph;
using geos::geomgraph::Label;
using geos::io::WKTReader;
using geos::util::TopologyException;
using testing::ElementsAre;
using testing::HasSubstr;

// EdgeEnd subclass that records computeLabel() invocations and optionally
// applies a new label when computeLabel() is called.
class RecordingEdgeEnd final : public EdgeEnd {
 public:
  RecordingEdgeEnd(Edge* edge, const Coordinate& p0, const Coordinate& p1,
                   const Label& initial_label)
      : EdgeEnd(edge, p0, p1, initial_label) {}

  void SetComputedLabel(const Label& lbl) { computed_label_ = lbl; }

  void computeLabel(const BoundaryNodeRule& bnr) override {
    ++compute_label_calls_;
    last_rule_ = &bnr;
    if (computed_label_.has_value()) {
      label = *computed_label_;
    }
  }

  int compute_label_calls() const { return compute_label_calls_; }
  const BoundaryNodeRule* last_rule() const { return last_rule_; }

 private:
  int compute_label_calls_ = 0;
  const BoundaryNodeRule* last_rule_ = nullptr;
  std::optional<Label> computed_label_;
};

// Minimal concrete EdgeEndStar that owns inserted EdgeEnd objects for clean
// memory management in unit tests.
class TestEdgeEndStar final : public EdgeEndStar {
 public:
  ~TestEdgeEndStar() override { edgeMap.clear(); }

  void insert(EdgeEnd* e) override { insertEdgeEnd(e); }

  template <typename T = EdgeEnd, typename... Args>
  T* AddNew(Args&&... args) {
    auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
    T* raw = ptr.get();
    owned_.push_back(std::move(ptr));
    insert(raw);
    return raw;
  }

 private:
  std::vector<std::unique_ptr<EdgeEnd>> owned_;
};

std::vector<std::unique_ptr<GeometryGraph>> MakeGraphs(
    const Geometry* g0, const Geometry* g1,
    const BoundaryNodeRule& rule = BoundaryNodeRule::getBoundaryOGCSFS()) {
  std::vector<std::unique_ptr<GeometryGraph>> graphs;
  graphs.push_back(std::make_unique<GeometryGraph>(0, g0, rule));
  graphs.push_back(std::make_unique<GeometryGraph>(1, g1, rule));
  return graphs;
}

TEST(EdgeEndStarTest, EmptyStarInitialStateAndCoordinate) {
  TestEdgeEndStar star;
  const EdgeEndStar& const_star = star;

  EXPECT_EQ(star.getDegree(), 0U);
  EXPECT_TRUE(star.getEdges().empty());
  EXPECT_EQ(star.begin(), star.end());
  EXPECT_EQ(const_star.begin(), const_star.end());
  EXPECT_EQ(star.rbegin(), star.rend());

  Coordinate& coord = star.getCoordinate();
  EXPECT_TRUE(coord.isNull());
  EXPECT_TRUE(std::isnan(coord.x));
  EXPECT_TRUE(std::isnan(coord.y));
  EXPECT_TRUE(std::isnan(coord.z));

  const Coordinate& const_coord = const_star.getCoordinate();
  EXPECT_TRUE(const_coord.isNull());
  EXPECT_EQ(&coord, &const_coord);

  EdgeEnd probe(nullptr, Coordinate(0.0, 0.0), Coordinate(1.0, 0.0));
  EXPECT_EQ(star.find(&probe), star.end());
  EXPECT_EQ(star.getNextCW(&probe), nullptr);
}

TEST(EdgeEndStarTest, InsertOrdersCounterClockwiseAndAccessorsWork) {
  TestEdgeEndStar star;
  const Coordinate origin(5.0, 5.0);

  EdgeEnd* e_sw = star.AddNew(nullptr, origin, Coordinate(4.0, 4.0));
  EdgeEnd* e_e = star.AddNew(nullptr, origin, Coordinate(6.0, 5.0));
  EdgeEnd* e_nw = star.AddNew(nullptr, origin, Coordinate(4.0, 6.0));
  EdgeEnd* e_se = star.AddNew(nullptr, origin, Coordinate(6.0, 4.0));
  EdgeEnd* e_ne = star.AddNew(nullptr, origin, Coordinate(6.0, 6.0));

  EXPECT_EQ(star.getDegree(), 5U);
  EXPECT_EQ(star.getEdges().size(), 5U);

  std::vector<EdgeEnd*> forward(star.begin(), star.end());
  EXPECT_THAT(forward, ElementsAre(e_e, e_ne, e_nw, e_sw, e_se));

  const EdgeEndStar& const_star = star;
  std::vector<const EdgeEnd*> const_forward(const_star.begin(),
                                            const_star.end());
  EXPECT_THAT(const_forward, ElementsAre(e_e, e_ne, e_nw, e_sw, e_se));

  std::vector<EdgeEnd*> reverse(star.rbegin(), star.rend());
  EXPECT_THAT(reverse, ElementsAre(e_se, e_sw, e_nw, e_ne, e_e));

  Coordinate& coord = star.getCoordinate();
  EXPECT_TRUE(coord.equals2D(origin));
  EXPECT_EQ(&coord, &e_e->getCoordinate());
  EXPECT_EQ(&const_star.getCoordinate(), &e_e->getCoordinate());

  auto found_it = star.find(e_ne);
  ASSERT_NE(found_it, star.end());
  EXPECT_EQ(*found_it, e_ne);

  EdgeEnd missing(nullptr, origin, Coordinate(5.0, 6.0));
  EXPECT_EQ(star.find(&missing), star.end());
}

TEST(EdgeEndStarTest, GetNextCWHandlesSingleMultipleAndMissingEdge) {
  TestEdgeEndStar single_star;
  EdgeEnd* only =
      single_star.AddNew(nullptr, Coordinate(0.0, 0.0), Coordinate(1.0, 0.0));
  EXPECT_EQ(single_star.getNextCW(only), only);

  TestEdgeEndStar star;
  const Coordinate origin(0.0, 0.0);
  EdgeEnd* e0 = star.AddNew(nullptr, origin, Coordinate(1.0, 0.0));
  EdgeEnd* e1 = star.AddNew(nullptr, origin, Coordinate(0.0, 1.0));
  EdgeEnd* e2 = star.AddNew(nullptr, origin, Coordinate(-1.0, 0.0));
  EdgeEnd* e3 = star.AddNew(nullptr, origin, Coordinate(0.0, -1.0));

  EXPECT_EQ(star.getNextCW(e0), e3);
  EXPECT_EQ(star.getNextCW(e3), e2);
  EXPECT_EQ(star.getNextCW(e2), e1);
  EXPECT_EQ(star.getNextCW(e1), e0);

  EdgeEnd missing(nullptr, origin, Coordinate(1.0, 1.0));
  EXPECT_EQ(star.getNextCW(&missing), nullptr);
}

TEST(EdgeEndStarTest, PrintAndStreamOperatorEmptyAndPopulated) {
  TestEdgeEndStar empty_star;
  std::string empty_text = empty_star.print();
  EXPECT_THAT(empty_text, HasSubstr("EdgeEndStar:"));

  std::ostringstream empty_stream;
  empty_stream << empty_star;
  EXPECT_EQ(empty_stream.str(), empty_text);

  TestEdgeEndStar star;
  const Coordinate origin(1.0, 2.0);
  star.AddNew(nullptr, origin, Coordinate(1.0, 3.0));
  star.AddNew(nullptr, origin, Coordinate(2.0, 2.0));

  std::string text = star.print();
  EXPECT_THAT(text, HasSubstr("EdgeEndStar:"));
  EXPECT_THAT(text, HasSubstr("1 2"));
  const std::size_t pos_e = text.find("1 2 - 2 2");
  const std::size_t pos_n = text.find("1 2 - 1 3");
  ASSERT_NE(pos_e, std::string::npos);
  ASSERT_NE(pos_n, std::string::npos);
  EXPECT_LT(pos_e, pos_n);

  std::ostringstream stream;
  stream << star;
  EXPECT_EQ(stream.str(), text);
}

TEST(EdgeEndStarTest, IsAreaLabelsConsistentCoversEmptyValidAndInvalidStars) {
  auto gf = GeometryFactory::create();
  WKTReader reader(*gf);
  std::unique_ptr<Geometry> poly =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  const BoundaryNodeRule& end_rule = BoundaryNodeRule::getBoundaryEndPoint();
  GeometryGraph gg(0, poly.get(), end_rule);

  TestEdgeEndStar empty_star;
  EXPECT_TRUE(empty_star.isAreaLabelsConsistent(gg));

  // Two-edge corner at (0, 0) using RecordingEdgeEnd to verify computeLabel()
  // receives the GeometryGraph's BoundaryNodeRule.
  {
    TestEdgeEndStar star;
    auto* e0 = star.AddNew<RecordingEdgeEnd>(nullptr, Coordinate(0.0, 0.0),
                                             Coordinate(1.0, 0.0), Label());
    e0->SetComputedLabel(
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
    auto* e1 = star.AddNew<RecordingEdgeEnd>(nullptr, Coordinate(0.0, 0.0),
                                             Coordinate(0.0, 1.0), Label());
    e1->SetComputedLabel(
        Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR));

    EXPECT_TRUE(star.isAreaLabelsConsistent(gg));
    EXPECT_EQ(e0->compute_label_calls(), 1);
    EXPECT_EQ(e1->compute_label_calls(), 1);
    EXPECT_EQ(e0->last_rule(), &end_rule);
    EXPECT_EQ(e1->last_rule(), &end_rule);
  }

  // Four-edge alternating crossing around origin.
  {
    TestEdgeEndStar star;
    const Coordinate origin(0.0, 0.0);
    star.AddNew(
        nullptr, origin, Coordinate(1.0, 0.0),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
    star.AddNew(
        nullptr, origin, Coordinate(0.0, 1.0),
        Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR));
    star.AddNew(
        nullptr, origin, Coordinate(-1.0, 0.0),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
    star.AddNew(
        nullptr, origin, Coordinate(0.0, -1.0),
        Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR));

    EXPECT_TRUE(star.isAreaLabelsConsistent(gg));
  }

  // Inconsistent: leftLoc == rightLoc.
  {
    TestEdgeEndStar star;
    const Coordinate origin(0.0, 0.0);
    star.AddNew(
        nullptr, origin, Coordinate(1.0, 0.0),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::INTERIOR));
    star.AddNew(
        nullptr, origin, Coordinate(0.0, 1.0),
        Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR));

    EXPECT_FALSE(star.isAreaLabelsConsistent(gg));
  }

  // Inconsistent: rightLoc != currLoc between consecutive edges.
  {
    TestEdgeEndStar star;
    const Coordinate origin(0.0, 0.0);
    star.AddNew(
        nullptr, origin, Coordinate(1.0, 0.0),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
    star.AddNew(
        nullptr, origin, Coordinate(0.0, 1.0),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));

    EXPECT_FALSE(star.isAreaLabelsConsistent(gg));
  }

  // Inconsistent single-edge area star (left != right).
  {
    TestEdgeEndStar star;
    star.AddNew(
        nullptr, Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
        Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));

    EXPECT_FALSE(star.isAreaLabelsConsistent(gg));
  }
}

TEST(EdgeEndStarTest, PropagateSideLabelsNoOpWhenNoLabelledAreaSides) {
  TestEdgeEndStar empty_star;
  empty_star.propagateSideLabels(0);
  EXPECT_EQ(empty_star.getDegree(), 0U);

  TestEdgeEndStar star;
  const Coordinate origin(0.0, 0.0);
  EdgeEnd* line_edge = star.AddNew(nullptr, origin, Coordinate(1.0, 0.0),
                                   Label(0, Location::NONE));
  EdgeEnd* null_area_edge =
      star.AddNew(nullptr, origin, Coordinate(0.0, 1.0),
                  Label(0, Location::NONE, Location::NONE, Location::NONE));

  star.propagateSideLabels(0);
  EXPECT_EQ(line_edge->getLabel().getLocation(0), Location::NONE);
  EXPECT_EQ(null_area_edge->getLabel().getLocation(0, Position::ON),
            Location::NONE);
  EXPECT_EQ(null_area_edge->getLabel().getLocation(0, Position::LEFT),
            Location::NONE);
  EXPECT_EQ(null_area_edge->getLabel().getLocation(0, Position::RIGHT),
            Location::NONE);
}

TEST(EdgeEndStarTest, PropagateSideLabelsFillsNullOnLineAndOtherGeomAreaEdges) {
  TestEdgeEndStar star;
  const Coordinate origin(0.0, 0.0);

  // e0 (0 deg): geom 0 area edge with ON already set.
  EdgeEnd* e0 = star.AddNew(
      nullptr, origin, Coordinate(1.0, 0.0),
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));

  // e1 (45 deg): geom 1 area edge -> geom 0 has (NONE, NONE, NONE).
  EdgeEnd* e1 = star.AddNew(
      nullptr, origin, Coordinate(1.0, 1.0),
      Label(1, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));

  // e2 (60 deg): line label for geom 0 with ON=NONE.
  EdgeEnd* e2 = star.AddNew(nullptr, origin, Coordinate(1.0, 2.0),
                            Label(0, Location::NONE));

  // e3 (90 deg): line label for geom 0 with ON already set.
  EdgeEnd* e3 = star.AddNew(nullptr, origin, Coordinate(0.0, 1.0),
                            Label(0, Location::BOUNDARY));

  // e4 (135 deg): geom 0 area edge with ON=NONE, LEFT=EXTERIOR, RIGHT=INTERIOR.
  // Because e4 is the last edge with LEFT != NONE, startLoc is EXTERIOR.
  EdgeEnd* e4 = star.AddNew(
      nullptr, origin, Coordinate(-1.0, 1.0),
      Label(0, Location::NONE, Location::EXTERIOR, Location::INTERIOR));

  // e5 (180 deg): geom 0 area edge with ON=BOUNDARY, LEFT=NONE, RIGHT=NONE.
  EdgeEnd* e5 =
      star.AddNew(nullptr, origin, Coordinate(-1.0, 0.0),
                  Label(0, Location::BOUNDARY, Location::NONE, Location::NONE));

  star.propagateSideLabels(0);

  // e0: ON preserved, currLoc transitions EXTERIOR -> INTERIOR.
  EXPECT_EQ(e0->getLabel().getLocation(0, Position::ON), Location::BOUNDARY);
  EXPECT_EQ(e0->getLabel().getLocation(0, Position::LEFT), Location::INTERIOR);
  EXPECT_EQ(e0->getLabel().getLocation(0, Position::RIGHT), Location::EXTERIOR);

  // e1: ON, LEFT, RIGHT all become INTERIOR.
  EXPECT_EQ(e1->getLabel().getLocation(0, Position::ON), Location::INTERIOR);
  EXPECT_EQ(e1->getLabel().getLocation(0, Position::LEFT), Location::INTERIOR);
  EXPECT_EQ(e1->getLabel().getLocation(0, Position::RIGHT), Location::INTERIOR);

  // e2: line ON becomes INTERIOR, stays non-area.
  EXPECT_TRUE(e2->getLabel().isLine(0));
  EXPECT_EQ(e2->getLabel().getLocation(0), Location::INTERIOR);

  // e3: line ON stays BOUNDARY.
  EXPECT_TRUE(e3->getLabel().isLine(0));
  EXPECT_EQ(e3->getLabel().getLocation(0), Location::BOUNDARY);

  // e4: ON becomes INTERIOR, currLoc transitions INTERIOR -> EXTERIOR.
  EXPECT_EQ(e4->getLabel().getLocation(0, Position::ON), Location::INTERIOR);
  EXPECT_EQ(e4->getLabel().getLocation(0, Position::LEFT), Location::EXTERIOR);
  EXPECT_EQ(e4->getLabel().getLocation(0, Position::RIGHT), Location::INTERIOR);

  // e5: ON stays BOUNDARY, LEFT and RIGHT become EXTERIOR.
  EXPECT_EQ(e5->getLabel().getLocation(0, Position::ON), Location::BOUNDARY);
  EXPECT_EQ(e5->getLabel().getLocation(0, Position::LEFT), Location::EXTERIOR);
  EXPECT_EQ(e5->getLabel().getLocation(0, Position::RIGHT), Location::EXTERIOR);
}

TEST(EdgeEndStarTest, PropagateSideLabelsThrowsOnSideLocationConflict) {
  TestEdgeEndStar star;
  const Coordinate node_coord(3.0, 4.0);
  star.AddNew(
      nullptr, node_coord, Coordinate(4.0, 4.0),
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  star.AddNew(
      nullptr, node_coord, Coordinate(3.0, 5.0),
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));

  try {
    star.propagateSideLabels(0);
    FAIL() << "Expected TopologyException for side location conflict";
  } catch (const TopologyException& ex) {
    EXPECT_THAT(ex.what(), HasSubstr("side location conflict"));
    EXPECT_THAT(ex.what(), HasSubstr("3 4"));
  }
}

TEST(EdgeEndStarTest, ComputeLabellingPropagatesConflictThrowsForGeom1) {
  auto gf = GeometryFactory::create();
  WKTReader reader(*gf);
  std::unique_ptr<Geometry> empty0 = reader.read("POLYGON EMPTY");
  std::unique_ptr<Geometry> empty1 = reader.read("POLYGON EMPTY");
  auto graphs = MakeGraphs(empty0.get(), empty1.get());

  TestEdgeEndStar star;
  const Coordinate node_coord(1.0, 2.0);
  star.AddNew(
      nullptr, node_coord, Coordinate(2.0, 2.0),
      Label(1, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  star.AddNew(
      nullptr, node_coord, Coordinate(1.0, 3.0),
      Label(1, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));

  EXPECT_THROW(star.computeLabelling(graphs), TopologyException);
}

TEST(EdgeEndStarTest,
     ComputeLabellingResolvesNullLabelsViaPointLocatorAndCache) {
  auto gf = GeometryFactory::create();
  WKTReader reader(*gf);
  std::unique_ptr<Geometry> poly0 =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> poly1 =
      reader.read("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
  const BoundaryNodeRule& rule =
      BoundaryNodeRule::getBoundaryMonovalentEndPoint();
  auto graphs = MakeGraphs(poly0.get(), poly1.get(), rule);

  // Star at (5, 5), which is in the interior of poly1. Two edges with null
  // geom 1 labels exercise both initial location lookup and cached reuse.
  {
    TestEdgeEndStar star;
    auto* e0 = star.AddNew<RecordingEdgeEnd>(nullptr, Coordinate(5.0, 5.0),
                                             Coordinate(6.0, 5.0),
                                             Label(0, Location::INTERIOR));
    Label area_null_geom1(0, Location::INTERIOR, Location::INTERIOR,
                          Location::INTERIOR);
    auto* e1 = star.AddNew<RecordingEdgeEnd>(
        nullptr, Coordinate(5.0, 5.0), Coordinate(5.0, 6.0), area_null_geom1);

    star.computeLabelling(graphs);

    EXPECT_EQ(e0->compute_label_calls(), 1);
    EXPECT_EQ(e1->compute_label_calls(), 1);
    EXPECT_EQ(e0->last_rule(), &rule);

    EXPECT_EQ(e0->getLabel().getLocation(0), Location::INTERIOR);
    EXPECT_EQ(e0->getLabel().getLocation(1), Location::INTERIOR);

    EXPECT_EQ(e1->getLabel().getLocation(1, Position::ON), Location::INTERIOR);
    EXPECT_EQ(e1->getLabel().getLocation(1, Position::LEFT),
              Location::INTERIOR);
    EXPECT_EQ(e1->getLabel().getLocation(1, Position::RIGHT),
              Location::INTERIOR);
  }

  // Star at (0, 5): on BOUNDARY of poly0 and EXTERIOR of poly1.
  {
    TestEdgeEndStar star;
    EdgeEnd* e = star.AddNew(nullptr, Coordinate(0.0, 5.0),
                             Coordinate(1.0, 5.0), Label());
    star.computeLabelling(graphs);
    EXPECT_EQ(e->getLabel().getLocation(0), Location::BOUNDARY);
    EXPECT_EQ(e->getLabel().getLocation(1), Location::EXTERIOR);
  }

  // Star at (20, 20): EXTERIOR to both polygons.
  {
    TestEdgeEndStar star;
    EdgeEnd* e = star.AddNew(nullptr, Coordinate(20.0, 20.0),
                             Coordinate(21.0, 20.0), Label());
    star.computeLabelling(graphs);
    EXPECT_EQ(e->getLabel().getLocation(0), Location::EXTERIOR);
    EXPECT_EQ(e->getLabel().getLocation(1), Location::EXTERIOR);
  }
}

TEST(EdgeEndStarTest, ComputeLabellingHandlesDimensionalCollapseEdges) {
  auto gf = GeometryFactory::create();
  WKTReader reader(*gf);
  std::unique_ptr<Geometry> line = reader.read("LINESTRING (0 5, 10 5)");
  std::unique_ptr<Geometry> poly =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  // Dimensional collapse for geom 1 at (5, 5), which lies inside poly.
  {
    auto graphs = MakeGraphs(line.get(), poly.get());
    TestEdgeEndStar star;
    EdgeEnd* collapsed =
        star.AddNew(nullptr, Coordinate(5.0, 5.0), Coordinate(6.0, 5.0),
                    Label(1, Location::BOUNDARY));
    EdgeEnd* unlabelled =
        star.AddNew(nullptr, Coordinate(5.0, 5.0), Coordinate(5.0, 6.0),
                    Label(0, Location::INTERIOR));

    star.computeLabelling(graphs);

    EXPECT_EQ(collapsed->getLabel().getLocation(0), Location::EXTERIOR);
    EXPECT_EQ(collapsed->getLabel().getLocation(1), Location::BOUNDARY);
    // Because hasDimensionalCollapseEdge[1] is true, unlabelled is EXTERIOR
    // rather than INTERIOR.
    EXPECT_EQ(unlabelled->getLabel().getLocation(1), Location::EXTERIOR);
  }

  // Contrast: a line edge labelled INTERIOR (not BOUNDARY) does NOT trigger
  // dimensional collapse suppression for geom 1.
  {
    auto graphs = MakeGraphs(line.get(), poly.get());
    TestEdgeEndStar star;
    star.AddNew(nullptr, Coordinate(5.0, 5.0), Coordinate(6.0, 5.0),
                Label(1, Location::INTERIOR));
    EdgeEnd* unlabelled =
        star.AddNew(nullptr, Coordinate(5.0, 5.0), Coordinate(5.0, 6.0),
                    Label(0, Location::INTERIOR));

    star.computeLabelling(graphs);
    EXPECT_EQ(unlabelled->getLabel().getLocation(1), Location::INTERIOR);
  }

  // Dimensional collapse for geom 0 when geom 0 is an area.
  {
    auto graphs = MakeGraphs(poly.get(), line.get());
    TestEdgeEndStar star;
    star.AddNew(nullptr, Coordinate(5.0, 5.0), Coordinate(6.0, 5.0),
                Label(0, Location::BOUNDARY));
    EdgeEnd* unlabelled =
        star.AddNew(nullptr, Coordinate(5.0, 5.0), Coordinate(5.0, 6.0),
                    Label(1, Location::INTERIOR));

    star.computeLabelling(graphs);
    EXPECT_EQ(unlabelled->getLabel().getLocation(0), Location::EXTERIOR);
  }
}

TEST(EdgeEndStarTest, PolymorphicDirectedEdgeStarComputeLabellingWorks) {
  auto gf = GeometryFactory::create();
  WKTReader reader(*gf);
  std::unique_ptr<Geometry> poly0 =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> poly1 =
      reader.read("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
  auto graphs = MakeGraphs(poly0.get(), poly1.get());

  auto e0_edge = std::make_unique<Edge>(
      new CoordinateSequence({Coordinate(5.0, 5.0), Coordinate(6.0, 5.0)}),
      Label(0, Location::INTERIOR));
  auto de0 = std::make_unique<DirectedEdge>(e0_edge.get(), true);
  auto de0_sym = std::make_unique<DirectedEdge>(e0_edge.get(), false);
  de0->setSym(de0_sym.get());
  de0_sym->setSym(de0.get());

  DirectedEdgeStar directed_star;
  EdgeEndStar* base_star = &directed_star;
  base_star->insert(de0.get());

  base_star->computeLabelling(graphs);
  EXPECT_EQ(de0->getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(de0->getLabel().getLocation(1), Location::INTERIOR);
  EXPECT_THAT(base_star->print(), HasSubstr("DirectedEdgeStar: 5 5"));
}

}  // namespace
}  // namespace geos_google
