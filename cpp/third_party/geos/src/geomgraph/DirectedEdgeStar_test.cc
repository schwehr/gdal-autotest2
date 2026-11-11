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

// Unit tests for third_party/geos/src/geomgraph/DirectedEdgeStar.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for TopologyException checks.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geomgraph/DirectedEdgeStar.cpp

#include "third_party/geos/include/geos/geomgraph/DirectedEdgeStar.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdge.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEnd.h"
#include "third_party/geos/include/geos/geomgraph/EdgeEndStar.h"
#include "third_party/geos/include/geos/geomgraph/EdgeRing.h"
#include "third_party/geos/include/geos/geomgraph/GeometryGraph.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::GeometryFactory;
using ::geos::geom::Location;
using ::geos::geom::Polygon;
using ::geos::geom::Position;
using ::geos::geomgraph::DirectedEdge;
using ::geos::geomgraph::DirectedEdgeStar;
using ::geos::geomgraph::Edge;
using ::geos::geomgraph::EdgeEnd;
using ::geos::geomgraph::EdgeEndStar;
using ::geos::geomgraph::EdgeRing;
using ::geos::geomgraph::GeometryGraph;
using ::geos::geomgraph::Label;
using ::geos::util::TopologyException;
using ::testing::HasSubstr;

class TestEdgeRing final : public EdgeRing {
 public:
  TestEdgeRing() : EdgeRing(nullptr, nullptr) {}

  DirectedEdge* getNext(DirectedEdge* de) override { return de->getNext(); }

  void setEdgeRing(DirectedEdge* de, EdgeRing* er) override {
    de->setEdgeRing(er);
  }
};

struct EdgePair {
  std::unique_ptr<Edge> edge;
  std::unique_ptr<DirectedEdge> fwd;
  std::unique_ptr<DirectedEdge> bwd;
};

EdgePair MakeEdgePair(const Coordinate& p0, const Coordinate& p1,
                      const Label& label, int depth_delta = 0) {
  auto coords = std::make_unique<CoordinateSequence>(
      std::initializer_list<Coordinate>{p0, p1});
  auto edge = std::make_unique<Edge>(coords.release(), label);
  edge->setDepthDelta(depth_delta);
  auto fwd = std::make_unique<DirectedEdge>(edge.get(), true);
  auto bwd = std::make_unique<DirectedEdge>(edge.get(), false);
  fwd->setSym(bwd.get());
  bwd->setSym(fwd.get());
  return EdgePair{
      .edge = std::move(edge),
      .fwd = std::move(fwd),
      .bwd = std::move(bwd),
  };
}

std::unique_ptr<Polygon> MakeSquarePolygon(const GeometryFactory& factory,
                                           double min_x, double min_y,
                                           double max_x, double max_y) {
  CoordinateSequence seq{
      Coordinate(min_x, min_y), Coordinate(max_x, min_y),
      Coordinate(max_x, max_y), Coordinate(min_x, max_y),
      Coordinate(min_x, min_y),
  };
  return factory.createPolygon(std::move(seq));
}

struct RaySpec {
  double dx;
  double dy;
};

struct RightmostCase {
  std::string_view name;
  std::array<RaySpec, 3> rays;
  std::size_t ray_count;
  std::size_t expected_index;
};

constexpr auto kRightmostCases = std::to_array<RightmostCase>({
    {
        .name = "single_edge",
        .rays = {{{1.0, 1.0}, {0.0, 0.0}, {0.0, 0.0}}},
        .ray_count = 1,
        .expected_index = 0,
    },
    {
        .name = "both_northern_east_and_nw",
        .rays = {{{1.0, 0.0}, {-1.0, 1.0}, {0.0, 0.0}}},
        .ray_count = 2,
        .expected_index = 0,
    },
    {
        .name = "both_northern_ne_and_west",
        .rays = {{{1.0, 1.0}, {-1.0, 0.0}, {0.0, 0.0}}},
        .ray_count = 2,
        .expected_index = 0,
    },
    {
        .name = "both_southern_sw_and_se",
        .rays = {{{-1.0, -1.0}, {0.0, -1.0}, {1.0, -1.0}}},
        .ray_count = 3,
        .expected_index = 2,
    },
    {
        .name = "both_southern_same_quadrant",
        .rays = {{{0.0, -1.0}, {2.0, -1.0}, {0.0, 0.0}}},
        .ray_count = 2,
        .expected_index = 1,
    },
    {
        .name = "mixed_hemispheres_first_non_horizontal",
        .rays = {{{1.0, 1.0}, {-1.0, 0.0}, {1.0, -1.0}}},
        .ray_count = 3,
        .expected_index = 0,
    },
    {
        .name = "mixed_hemispheres_first_horizontal_east",
        .rays = {{{1.0, 0.0}, {0.0, 1.0}, {-1.0, -1.0}}},
        .ray_count = 3,
        .expected_index = 2,
    },
    {
        .name = "mixed_hemispheres_first_horizontal_west",
        .rays = {{{-1.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}}},
        .ray_count = 2,
        .expected_index = 1,
    },
});

TEST(DirectedEdgeStarTest, EmptyStarBehavior) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> empty0 = factory->createPolygon();
  std::unique_ptr<Polygon> empty1 = factory->createPolygon();
  std::vector<std::unique_ptr<GeometryGraph>> graphs;
  graphs.push_back(std::make_unique<GeometryGraph>(0, empty0.get()));
  graphs.push_back(std::make_unique<GeometryGraph>(1, empty1.get()));

  std::unique_ptr<EdgeEndStar> poly_star = std::make_unique<DirectedEdgeStar>();
  DirectedEdgeStar star;
  TestEdgeRing ring;

  EXPECT_EQ(poly_star->getDegree(), 0U);
  EXPECT_EQ(star.getDegree(), 0U);
  EXPECT_EQ(star.begin(), star.end());
  EXPECT_TRUE(star.getLabel().isNull());
  EXPECT_EQ(star.getOutgoingDegree(), 0);
  EXPECT_EQ(star.getOutgoingDegree(&ring), 0);
  EXPECT_EQ(star.getOutgoingDegree(nullptr), 0);
  EXPECT_EQ(star.getRightmostEdge(), nullptr);

  star.getLabel().setLocation(0, Location::INTERIOR);
  star.computeLabelling(graphs);
  EXPECT_TRUE(star.getLabel().isNull());

  star.mergeSymLabels();
  star.updateLabelling(Label(Location::INTERIOR));
  star.linkResultDirectedEdges();
  star.linkMinimalDirectedEdges(&ring);
  star.findCoveredLineEdges();

  EXPECT_THAT(star.print(), HasSubstr("DirectedEdgeStar:"));
}

TEST(DirectedEdgeStarTest, InsertOrdersEdgesCounterClockwise) {
  const Coordinate center(1.0, 2.0);
  const Label area_label(0, Location::BOUNDARY, Location::INTERIOR,
                         Location::EXTERIOR);

  EdgePair east = MakeEdgePair(center, Coordinate(2.0, 2.0), area_label);
  EdgePair ne = MakeEdgePair(center, Coordinate(2.0, 3.0), area_label);
  EdgePair north = MakeEdgePair(center, Coordinate(1.0, 3.0), area_label);
  EdgePair west = MakeEdgePair(center, Coordinate(0.0, 2.0), area_label);
  EdgePair south = MakeEdgePair(center, Coordinate(1.0, 1.0), area_label);
  EdgePair east_dup = MakeEdgePair(center, Coordinate(4.0, 2.0), area_label);

  DirectedEdgeStar star;
  EdgeEndStar* base_star = &star;
  base_star->insert(west.fwd.get());
  star.insert(south.fwd.get());
  star.insert(ne.fwd.get());
  star.insert(east.fwd.get());
  base_star->insert(north.fwd.get());
  // Equal direction ray compares equal in EdgeEndLT and is not duplicated.
  star.insert(east_dup.fwd.get());

  EXPECT_EQ(star.getDegree(), 5U);
  EXPECT_TRUE(star.getCoordinate().equals2D(center));

  std::vector<EdgeEnd*> actual;
  for (EdgeEnd* ee : star) {
    actual.push_back(ee);
  }
  const std::vector<EdgeEnd*> expected = {
      east.fwd.get(), ne.fwd.get(),    north.fwd.get(),
      west.fwd.get(), south.fwd.get(),
  };
  EXPECT_EQ(actual, expected);
}

TEST(DirectedEdgeStarTest, GetOutgoingDegreeCountsResultAndEdgeRing) {
  const Coordinate center(0.0, 0.0);
  const Label area_label(0, Location::BOUNDARY, Location::INTERIOR,
                         Location::EXTERIOR);

  TestEdgeRing ring_a;
  TestEdgeRing ring_b;
  TestEdgeRing ring_unused;

  EdgePair e0 = MakeEdgePair(center, Coordinate(1.0, 0.0), area_label);
  EdgePair e1 = MakeEdgePair(center, Coordinate(0.0, 1.0), area_label);
  EdgePair e2 = MakeEdgePair(center, Coordinate(-1.0, 0.0), area_label);
  EdgePair e3 = MakeEdgePair(center, Coordinate(0.0, -1.0), area_label);

  e0.fwd->setInResult(true);
  e0.fwd->setEdgeRing(&ring_a);

  e1.fwd->setInResult(false);
  e1.bwd->setInResult(true);  // Incoming edge in result must not count.
  e1.fwd->setEdgeRing(&ring_a);

  e2.fwd->setInResult(true);
  e2.fwd->setEdgeRing(&ring_b);

  e3.fwd->setInResult(false);
  e3.fwd->setEdgeRing(nullptr);

  DirectedEdgeStar star;
  star.insert(e0.fwd.get());
  star.insert(e1.fwd.get());
  star.insert(e2.fwd.get());
  star.insert(e3.fwd.get());

  EXPECT_EQ(star.getOutgoingDegree(), 2);
  EXPECT_EQ(star.getOutgoingDegree(&ring_a), 2);
  EXPECT_EQ(star.getOutgoingDegree(&ring_b), 1);
  EXPECT_EQ(star.getOutgoingDegree(&ring_unused), 0);
  EXPECT_EQ(star.getOutgoingDegree(nullptr), 1);
}

TEST(DirectedEdgeStarTest, GetRightmostEdgeAcrossHemispheres) {
  const Coordinate center(0.0, 0.0);
  const Label line_label(0, Location::INTERIOR);

  for (const auto& tc : kRightmostCases) {
    SCOPED_TRACE(tc.name);
    std::vector<EdgePair> pairs;
    pairs.reserve(tc.ray_count);
    DirectedEdgeStar star;
    for (std::size_t i = 0; i < tc.ray_count; ++i) {
      pairs.push_back(MakeEdgePair(
          center, Coordinate(tc.rays[i].dx, tc.rays[i].dy), line_label));
      star.insert(pairs.back().fwd.get());
    }
    EXPECT_EQ(star.getRightmostEdge(), pairs[tc.expected_index].fwd.get());
  }
}

TEST(DirectedEdgeStarTest,
     ComputeLabellingAggregatesEdgeLocationsAndPropagates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly0 =
      MakeSquarePolygon(*factory, -5.0, -5.0, 5.0, 5.0);
  std::unique_ptr<Polygon> poly1 =
      MakeSquarePolygon(*factory, 10.0, 10.0, 20.0, 20.0);
  std::vector<std::unique_ptr<GeometryGraph>> graphs;
  graphs.push_back(std::make_unique<GeometryGraph>(0, poly0.get()));
  graphs.push_back(std::make_unique<GeometryGraph>(1, poly1.get()));

  const Coordinate center(0.0, 0.0);
  // Consistent CCW area labelling for geometry 0 with null geometry 1 slots.
  EdgePair e0 = MakeEdgePair(
      center, Coordinate(1.0, 0.0),
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  EdgePair e1 = MakeEdgePair(
      center, Coordinate(0.0, 1.0),
      Label(0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR));
  EdgePair e2 =
      MakeEdgePair(center, Coordinate(-1.0, 0.0), Label(Location::EXTERIOR));

  DirectedEdgeStar star;
  star.insert(e0.fwd.get());
  star.insert(e1.fwd.get());
  star.insert(e2.fwd.get());

  star.computeLabelling(graphs);
  EXPECT_EQ(star.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(star.getLabel().getLocation(1), Location::NONE);
  // EdgeEndStar::computeLabelling fills null geom 1 slots by locating (0,0) in
  // poly1 (disjoint -> EXTERIOR).
  EXPECT_EQ(e0.fwd->getLabel().getLocation(1, Position::ON),
            Location::EXTERIOR);

  // An edge with INTERIOR on geometry 1 and EXTERIOR on geometry 0 sets only
  // geometry 1 to INTERIOR on the star label.
  EdgePair e_geom1 =
      MakeEdgePair(center, Coordinate(1.0, 1.0), Label(1, Location::INTERIOR));
  EdgePair e_geom0_ext = MakeEdgePair(center, Coordinate(-1.0, -1.0),
                                      Label(0, Location::EXTERIOR));

  DirectedEdgeStar star_geom1;
  star_geom1.insert(e_geom1.fwd.get());
  star_geom1.insert(e_geom0_ext.fwd.get());
  star_geom1.computeLabelling(graphs);
  EXPECT_EQ(star_geom1.getLabel().getLocation(0), Location::NONE);
  EXPECT_EQ(star_geom1.getLabel().getLocation(1), Location::INTERIOR);
  EXPECT_EQ(e_geom1.fwd->getLabel().getLocation(0, Position::ON),
            Location::INTERIOR);
}

TEST(DirectedEdgeStarTest, ComputeLabellingThrowsOnSideLocationConflict) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly0 =
      MakeSquarePolygon(*factory, -5.0, -5.0, 5.0, 5.0);
  std::unique_ptr<Polygon> poly1 =
      MakeSquarePolygon(*factory, -5.0, -5.0, 5.0, 5.0);
  std::vector<std::unique_ptr<GeometryGraph>> graphs;
  graphs.push_back(std::make_unique<GeometryGraph>(0, poly0.get()));
  graphs.push_back(std::make_unique<GeometryGraph>(1, poly1.get()));

  const Coordinate center(0.0, 0.0);
  // Both edges have LEFT=INTERIOR and RIGHT=EXTERIOR, causing a side conflict.
  const Label conflicting(0, Location::BOUNDARY, Location::INTERIOR,
                          Location::EXTERIOR);
  EdgePair e0 = MakeEdgePair(center, Coordinate(1.0, 0.0), conflicting);
  EdgePair e1 = MakeEdgePair(center, Coordinate(0.0, 1.0), conflicting);

  DirectedEdgeStar star;
  star.insert(e0.fwd.get());
  star.insert(e1.fwd.get());

  EXPECT_THROW(star.computeLabelling(graphs), TopologyException);
}

TEST(DirectedEdgeStarTest, MergeSymLabelsAndUpdateLabelling) {
  const Coordinate center(0.0, 0.0);
  EdgePair e_area = MakeEdgePair(
      center, Coordinate(1.0, 0.0),
      Label(0, Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR));
  e_area.bwd->getLabel().setLocation(1, Position::ON, Location::BOUNDARY);
  e_area.bwd->getLabel().setLocation(1, Position::LEFT, Location::INTERIOR);
  e_area.bwd->getLabel().setLocation(1, Position::RIGHT, Location::EXTERIOR);

  EdgePair e_line =
      MakeEdgePair(center, Coordinate(0.0, 1.0), Label(0, Location::INTERIOR));
  e_line.bwd->getLabel().setLocation(1, Position::ON, Location::BOUNDARY);

  DirectedEdgeStar star;
  star.insert(e_area.fwd.get());
  star.insert(e_line.fwd.get());

  star.mergeSymLabels();

  // Geometry 0 attributes on fwd were already non-null and remain intact.
  EXPECT_EQ(e_area.fwd->getLabel().getLocation(0, Position::ON),
            Location::BOUNDARY);
  EXPECT_EQ(e_area.fwd->getLabel().getLocation(0, Position::LEFT),
            Location::INTERIOR);
  EXPECT_EQ(e_area.fwd->getLabel().getLocation(0, Position::RIGHT),
            Location::EXTERIOR);
  // Geometry 1 attributes were NONE on fwd and are populated from sym.
  EXPECT_EQ(e_area.fwd->getLabel().getLocation(1, Position::ON),
            Location::BOUNDARY);
  EXPECT_EQ(e_area.fwd->getLabel().getLocation(1, Position::LEFT),
            Location::INTERIOR);
  EXPECT_EQ(e_area.fwd->getLabel().getLocation(1, Position::RIGHT),
            Location::EXTERIOR);
  EXPECT_EQ(e_line.fwd->getLabel().getLocation(1, Position::ON),
            Location::BOUNDARY);

  // Now exercise updateLabelling with partial NONE slots.
  EdgePair e_partial = MakeEdgePair(
      center, Coordinate(-1.0, 0.0),
      Label(0, Location::BOUNDARY, Location::NONE, Location::EXTERIOR));
  EdgePair e_null =
      MakeEdgePair(center, Coordinate(0.0, -1.0), Label(Location::NONE));

  DirectedEdgeStar update_star;
  update_star.insert(e_partial.fwd.get());
  update_star.insert(e_null.fwd.get());

  Label node_label(Location::NONE);
  node_label.setLocation(0, Location::INTERIOR);
  node_label.setLocation(1, Location::EXTERIOR);
  update_star.updateLabelling(node_label);

  EXPECT_EQ(e_partial.fwd->getLabel().getLocation(0, Position::ON),
            Location::BOUNDARY);
  EXPECT_EQ(e_partial.fwd->getLabel().getLocation(0, Position::LEFT),
            Location::INTERIOR);
  EXPECT_EQ(e_partial.fwd->getLabel().getLocation(0, Position::RIGHT),
            Location::EXTERIOR);
  EXPECT_EQ(e_partial.fwd->getLabel().getLocation(1, Position::ON),
            Location::EXTERIOR);
  EXPECT_EQ(e_partial.fwd->getLabel().getLocation(1, Position::LEFT),
            Location::EXTERIOR);
  EXPECT_EQ(e_partial.fwd->getLabel().getLocation(1, Position::RIGHT),
            Location::EXTERIOR);

  EXPECT_EQ(e_null.fwd->getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(e_null.fwd->getLabel().getLocation(1), Location::EXTERIOR);
}

TEST(DirectedEdgeStarTest, LinkResultDirectedEdgesLinksAndCaches) {
  const Coordinate center(0.0, 0.0);
  const Label area_label(0, Location::BOUNDARY, Location::INTERIOR,
                         Location::EXTERIOR);
  const Label line_label(0, Location::INTERIOR);

  // CCW order around center:
  // 0 deg: e_out0 (outgoing area)
  // 30 deg: e_nonres (neither side in result)
  // 60 deg: e_line (line edge in result, skipped by linkResultDirectedEdges)
  // 90 deg: e_in0 (incoming area: sym is in result)
  // 180 deg: e_out1 (outgoing area)
  // 270 deg: e_in1 (incoming area: wraps around to e_out0)
  EdgePair e_out0 = MakeEdgePair(center, Coordinate(1.0, 0.0), area_label);
  EdgePair e_nonres = MakeEdgePair(center, Coordinate(2.0, 1.0), area_label);
  EdgePair e_line = MakeEdgePair(center, Coordinate(1.0, 2.0), line_label);
  EdgePair e_in0 = MakeEdgePair(center, Coordinate(0.0, 1.0), area_label);
  EdgePair e_out1 = MakeEdgePair(center, Coordinate(-1.0, 0.0), area_label);
  EdgePair e_in1 = MakeEdgePair(center, Coordinate(0.0, -1.0), area_label);

  e_out0.fwd->setInResult(true);
  e_line.fwd->setInResult(true);
  e_in0.bwd->setInResult(true);
  e_out1.fwd->setInResult(true);
  e_in1.bwd->setInResult(true);

  DirectedEdgeStar star;
  star.insert(e_out0.fwd.get());
  star.insert(e_nonres.fwd.get());
  star.insert(e_line.fwd.get());
  star.insert(e_in0.fwd.get());
  star.insert(e_out1.fwd.get());
  star.insert(e_in1.fwd.get());

  star.linkResultDirectedEdges();

  EXPECT_EQ(e_in0.bwd->getNext(), e_out1.fwd.get());
  EXPECT_EQ(e_in1.bwd->getNext(), e_out0.fwd.get());
  EXPECT_EQ(e_line.fwd->getNext(), nullptr);
  EXPECT_EQ(e_nonres.fwd->getNext(), nullptr);
  EXPECT_EQ(e_nonres.bwd->getNext(), nullptr);

  // Calling a second time hits the cached resultAreaEdgeList path.
  e_in0.bwd->setNext(nullptr);
  e_in1.bwd->setNext(nullptr);
  star.linkResultDirectedEdges();
  EXPECT_EQ(e_in0.bwd->getNext(), e_out1.fwd.get());
  EXPECT_EQ(e_in1.bwd->getNext(), e_out0.fwd.get());
}

TEST(DirectedEdgeStarTest, LinkResultDirectedEdgesHandlesConsecutiveIncoming) {
  const Coordinate center(0.0, 0.0);
  const Label area_label(0, Location::BOUNDARY, Location::INTERIOR,
                         Location::EXTERIOR);

  // First edge is incoming, second is also incoming (skipped while
  // LINKING_TO_OUTGOING), third is outgoing.
  EdgePair e_in0 = MakeEdgePair(center, Coordinate(1.0, 0.0), area_label);
  EdgePair e_in1 = MakeEdgePair(center, Coordinate(0.0, 1.0), area_label);
  EdgePair e_out = MakeEdgePair(center, Coordinate(-1.0, 0.0), area_label);

  e_in0.bwd->setInResult(true);
  e_in1.bwd->setInResult(true);
  e_out.fwd->setInResult(true);

  DirectedEdgeStar star;
  star.insert(e_in0.fwd.get());
  star.insert(e_in1.fwd.get());
  star.insert(e_out.fwd.get());

  star.linkResultDirectedEdges();
  EXPECT_EQ(e_in0.bwd->getNext(), e_out.fwd.get());
  EXPECT_EQ(e_in1.bwd->getNext(), nullptr);
}

TEST(DirectedEdgeStarTest, LinkResultDirectedEdgesThrowsWhenNoOutgoingFound) {
  const Coordinate center(3.0, 4.0);
  const Label area_label(0, Location::BOUNDARY, Location::INTERIOR,
                         Location::EXTERIOR);

  EdgePair e_in_only = MakeEdgePair(center, Coordinate(4.0, 4.0), area_label);
  e_in_only.bwd->setInResult(true);

  DirectedEdgeStar star;
  star.insert(e_in_only.fwd.get());

  try {
    star.linkResultDirectedEdges();
    FAIL() << "Expected TopologyException when no outgoing edge is in result";
  } catch (TopologyException& ex) {
    EXPECT_THAT(ex.what(), HasSubstr("no outgoing dirEdge found"));
    EXPECT_TRUE(ex.getCoordinate().equals2D(center));
  }
}

TEST(DirectedEdgeStarTest, LinkMinimalDirectedEdgesSplitsRingsAndWraps) {
  const Coordinate center(0.0, 0.0);
  const Label area_label(0, Location::BOUNDARY, Location::INTERIOR,
                         Location::EXTERIOR);

  TestEdgeRing ring1;
  TestEdgeRing ring2;

  // CCW order: e0(out,r1), e1(in,r2), e2(in,r1), e3(out,r2), e4(out,r1),
  // e5(in,r1).
  // CW order for linkMinimalDirectedEdges(&ring1):
  // e5(in,r1) -> e4(out,r1); skip e3(r2); e2(in,r1) -> skip e1(r2) ->
  // e0(out,r1).
  EdgePair e0 = MakeEdgePair(center, Coordinate(1.0, 0.0), area_label);
  EdgePair e1 = MakeEdgePair(center, Coordinate(1.0, 1.0), area_label);
  EdgePair e2 = MakeEdgePair(center, Coordinate(0.0, 1.0), area_label);
  EdgePair e3 = MakeEdgePair(center, Coordinate(-1.0, 1.0), area_label);
  EdgePair e4 = MakeEdgePair(center, Coordinate(-1.0, 0.0), area_label);
  EdgePair e5 = MakeEdgePair(center, Coordinate(0.0, -1.0), area_label);

  e0.fwd->setInResult(true);
  e0.fwd->setEdgeRing(&ring1);
  e1.bwd->setInResult(true);
  e1.bwd->setEdgeRing(&ring2);
  e2.bwd->setInResult(true);
  e2.bwd->setEdgeRing(&ring1);
  e3.fwd->setInResult(true);
  e3.fwd->setEdgeRing(&ring2);
  e4.fwd->setInResult(true);
  e4.fwd->setEdgeRing(&ring1);
  e5.bwd->setInResult(true);
  e5.bwd->setEdgeRing(&ring1);

  DirectedEdgeStar star;
  star.insert(e0.fwd.get());
  star.insert(e1.fwd.get());
  star.insert(e2.fwd.get());
  star.insert(e3.fwd.get());
  star.insert(e4.fwd.get());
  star.insert(e5.fwd.get());

  star.linkResultDirectedEdges();
  star.linkMinimalDirectedEdges(&ring1);

  EXPECT_EQ(e5.bwd->getNextMin(), e4.fwd.get());
  EXPECT_EQ(e2.bwd->getNextMin(), e0.fwd.get());
  EXPECT_EQ(e1.bwd->getNextMin(), nullptr);

  // Wrap-around case: outgoing edge appears first in CW scan, incoming last.
  TestEdgeRing wrap_ring;
  EdgePair w_in = MakeEdgePair(center, Coordinate(1.0, 0.0), area_label);
  EdgePair w_out = MakeEdgePair(center, Coordinate(0.0, 1.0), area_label);
  w_in.bwd->setInResult(true);
  w_in.bwd->setEdgeRing(&wrap_ring);
  w_out.fwd->setInResult(true);
  w_out.fwd->setEdgeRing(&wrap_ring);

  DirectedEdgeStar wrap_star;
  wrap_star.insert(w_in.fwd.get());
  wrap_star.insert(w_out.fwd.get());
  wrap_star.linkResultDirectedEdges();
  wrap_star.linkMinimalDirectedEdges(&wrap_ring);

  EXPECT_EQ(w_in.bwd->getNextMin(), w_out.fwd.get());
}

TEST(DirectedEdgeStarTest, LinkAllDirectedEdgesLinksClockwise) {
  const Coordinate center(0.0, 0.0);
  const Label line_label(0, Location::INTERIOR);

  // Single-edge star links sym to itself.
  EdgePair single = MakeEdgePair(center, Coordinate(1.0, 0.0), line_label);
  DirectedEdgeStar single_star;
  single_star.insert(single.fwd.get());
  single_star.linkAllDirectedEdges();
  EXPECT_EQ(single.bwd->getNext(), single.fwd.get());

  // Multi-edge star links each incoming sym edge to the next CW outgoing edge.
  EdgePair e0 = MakeEdgePair(center, Coordinate(1.0, 0.0), line_label);
  EdgePair e1 = MakeEdgePair(center, Coordinate(0.0, 1.0), line_label);
  EdgePair e2 = MakeEdgePair(center, Coordinate(-1.0, 0.0), line_label);
  EdgePair e3 = MakeEdgePair(center, Coordinate(0.0, -1.0), line_label);

  DirectedEdgeStar star;
  star.insert(e2.fwd.get());
  star.insert(e0.fwd.get());
  star.insert(e3.fwd.get());
  star.insert(e1.fwd.get());

  star.linkAllDirectedEdges();
  EXPECT_EQ(e3.bwd->getNext(), e0.fwd.get());
  EXPECT_EQ(e2.bwd->getNext(), e3.fwd.get());
  EXPECT_EQ(e1.bwd->getNext(), e2.fwd.get());
  EXPECT_EQ(e0.bwd->getNext(), e1.fwd.get());
}

TEST(DirectedEdgeStarTest, FindCoveredLineEdgesMarksCoveredStatus) {
  const Coordinate center(0.0, 0.0);
  const Label area_label(0, Location::BOUNDARY, Location::INTERIOR,
                         Location::EXTERIOR);
  const Label line_label(0, Location::INTERIOR);

  // Case 1: No result area edges -> covered flag stays unset.
  {
    EdgePair line = MakeEdgePair(center, Coordinate(1.0, 0.0), line_label);
    EdgePair area = MakeEdgePair(center, Coordinate(0.0, 1.0), area_label);
    DirectedEdgeStar star;
    star.insert(line.fwd.get());
    star.insert(area.fwd.get());
    star.findCoveredLineEdges();
    EXPECT_FALSE(line.edge->isCoveredSet());
  }

  // Case 2: First result area edge is outgoing -> startLoc = INTERIOR.
  {
    EdgePair l_before = MakeEdgePair(center, Coordinate(1.0, 0.0), line_label);
    EdgePair a_out = MakeEdgePair(center, Coordinate(1.0, 1.0), area_label);
    EdgePair l_out1 = MakeEdgePair(center, Coordinate(0.0, 1.0), line_label);
    EdgePair a_nonres = MakeEdgePair(center, Coordinate(-1.0, 1.0), area_label);
    EdgePair l_out2 = MakeEdgePair(center, Coordinate(-1.0, 0.0), line_label);
    EdgePair a_in = MakeEdgePair(center, Coordinate(-1.0, -1.0), area_label);
    EdgePair l_in = MakeEdgePair(center, Coordinate(0.0, -1.0), line_label);

    a_out.fwd->setInResult(true);
    a_in.bwd->setInResult(true);

    DirectedEdgeStar star;
    star.insert(l_before.fwd.get());
    star.insert(a_out.fwd.get());
    star.insert(l_out1.fwd.get());
    star.insert(a_nonres.fwd.get());
    star.insert(l_out2.fwd.get());
    star.insert(a_in.fwd.get());
    star.insert(l_in.fwd.get());

    star.findCoveredLineEdges();

    EXPECT_TRUE(l_before.edge->isCoveredSet());
    EXPECT_TRUE(l_before.edge->isCovered());
    EXPECT_TRUE(l_out1.edge->isCoveredSet());
    EXPECT_FALSE(l_out1.edge->isCovered());
    EXPECT_TRUE(l_out2.edge->isCoveredSet());
    EXPECT_FALSE(l_out2.edge->isCovered());
    EXPECT_TRUE(l_in.edge->isCoveredSet());
    EXPECT_TRUE(l_in.edge->isCovered());
  }

  // Case 3: First result area edge is incoming -> startLoc = EXTERIOR.
  {
    EdgePair l_before = MakeEdgePair(center, Coordinate(1.0, 0.0), line_label);
    EdgePair a_nonres = MakeEdgePair(center, Coordinate(2.0, 1.0), area_label);
    EdgePair a_in = MakeEdgePair(center, Coordinate(0.0, 1.0), area_label);
    EdgePair l_inside = MakeEdgePair(center, Coordinate(-1.0, 0.0), line_label);
    EdgePair a_out = MakeEdgePair(center, Coordinate(0.0, -1.0), area_label);
    EdgePair l_after = MakeEdgePair(center, Coordinate(1.0, -1.0), line_label);

    a_in.bwd->setInResult(true);
    a_out.fwd->setInResult(true);

    DirectedEdgeStar star;
    star.insert(l_before.fwd.get());
    star.insert(a_nonres.fwd.get());
    star.insert(a_in.fwd.get());
    star.insert(l_inside.fwd.get());
    star.insert(a_out.fwd.get());
    star.insert(l_after.fwd.get());

    star.findCoveredLineEdges();

    EXPECT_TRUE(l_before.edge->isCoveredSet());
    EXPECT_FALSE(l_before.edge->isCovered());
    EXPECT_TRUE(l_inside.edge->isCoveredSet());
    EXPECT_TRUE(l_inside.edge->isCovered());
    EXPECT_TRUE(l_after.edge->isCoveredSet());
    EXPECT_FALSE(l_after.edge->isCovered());
  }
}

TEST(DirectedEdgeStarTest, ComputeDepthsPropagatesAndThrowsOnMismatch) {
  const Coordinate center(5.0, 6.0);
  const Label area_label(0, Location::BOUNDARY, Location::INTERIOR,
                         Location::EXTERIOR);

  // Middle start edge across a 4-edge consistent cycle (+1, +1, -1, -1).
  {
    EdgePair e0 = MakeEdgePair(center, Coordinate(6.0, 6.0), area_label, 1);
    EdgePair e1 = MakeEdgePair(center, Coordinate(5.0, 7.0), area_label, 1);
    EdgePair e2 = MakeEdgePair(center, Coordinate(4.0, 6.0), area_label, -1);
    EdgePair e3 = MakeEdgePair(center, Coordinate(5.0, 5.0), area_label, -1);

    e1.fwd->setEdgeDepths(Position::RIGHT, 1);

    DirectedEdgeStar star;
    star.insert(e0.fwd.get());
    star.insert(e1.fwd.get());
    star.insert(e2.fwd.get());
    star.insert(e3.fwd.get());

    star.computeDepths(e1.fwd.get());

    EXPECT_EQ(e0.fwd->getDepth(Position::RIGHT), 0);
    EXPECT_EQ(e0.fwd->getDepth(Position::LEFT), 1);
    EXPECT_EQ(e1.fwd->getDepth(Position::RIGHT), 1);
    EXPECT_EQ(e1.fwd->getDepth(Position::LEFT), 2);
    EXPECT_EQ(e2.fwd->getDepth(Position::RIGHT), 2);
    EXPECT_EQ(e2.fwd->getDepth(Position::LEFT), 1);
    EXPECT_EQ(e3.fwd->getDepth(Position::RIGHT), 1);
    EXPECT_EQ(e3.fwd->getDepth(Position::LEFT), 0);
  }

  // Starting from begin(), starting from last edge, and single zero-delta edge.
  {
    EdgePair e0 = MakeEdgePair(center, Coordinate(6.0, 6.0), area_label, 1);
    EdgePair e1 = MakeEdgePair(center, Coordinate(5.0, 7.0), area_label, 0);
    EdgePair e2 = MakeEdgePair(center, Coordinate(4.0, 6.0), area_label, -1);
    e0.fwd->setEdgeDepths(Position::RIGHT, 0);

    DirectedEdgeStar star_first;
    star_first.insert(e0.fwd.get());
    star_first.insert(e1.fwd.get());
    star_first.insert(e2.fwd.get());
    star_first.computeDepths(e0.fwd.get());
    EXPECT_EQ(e2.fwd->getDepth(Position::LEFT), 0);
  }

  {
    EdgePair e0 = MakeEdgePair(center, Coordinate(6.0, 6.0), area_label, 1);
    EdgePair e1 = MakeEdgePair(center, Coordinate(5.0, 7.0), area_label, 0);
    EdgePair e2 = MakeEdgePair(center, Coordinate(4.0, 6.0), area_label, -1);
    e2.fwd->setEdgeDepths(Position::RIGHT, 1);

    DirectedEdgeStar star_last;
    star_last.insert(e0.fwd.get());
    star_last.insert(e1.fwd.get());
    star_last.insert(e2.fwd.get());
    star_last.computeDepths(e2.fwd.get());
    EXPECT_EQ(e0.fwd->getDepth(Position::RIGHT), 0);
    EXPECT_EQ(e1.fwd->getDepth(Position::LEFT), 1);
  }

  {
    EdgePair single = MakeEdgePair(center, Coordinate(6.0, 6.0), area_label, 0);
    single.fwd->setEdgeDepths(Position::RIGHT, 2);
    DirectedEdgeStar single_star;
    single_star.insert(single.fwd.get());
    single_star.computeDepths(single.fwd.get());
    EXPECT_EQ(single.fwd->getDepth(Position::LEFT), 2);
  }

  // Depth mismatch throws TopologyException at the edge coordinate.
  {
    EdgePair e_bad = MakeEdgePair(center, Coordinate(6.0, 6.0), area_label, 1);
    e_bad.fwd->setEdgeDepths(Position::RIGHT, 0);
    DirectedEdgeStar bad_star;
    bad_star.insert(e_bad.fwd.get());

    try {
      bad_star.computeDepths(e_bad.fwd.get());
      FAIL() << "Expected TopologyException on depth mismatch";
    } catch (TopologyException& ex) {
      EXPECT_THAT(ex.what(), HasSubstr("depth mismatch"));
      EXPECT_TRUE(ex.getCoordinate().equals2D(center));
    }
  }
}

TEST(DirectedEdgeStarTest, PrintIncludesStarCoordinateAndEdges) {
  const Coordinate center(2.0, 3.0);
  const Label area_label(0, Location::BOUNDARY, Location::INTERIOR,
                         Location::EXTERIOR);

  TestEdgeRing ring;
  EdgePair e0 = MakeEdgePair(center, Coordinate(3.0, 3.0), area_label);
  EdgePair e1 = MakeEdgePair(center, Coordinate(2.0, 4.0), area_label);
  e0.fwd->setInResult(true);
  e0.fwd->setEdgeRing(&ring);

  DirectedEdgeStar star;
  star.insert(e0.fwd.get());
  star.insert(e1.fwd.get());

  const std::string output = star.print();
  EXPECT_THAT(output, HasSubstr("DirectedEdgeStar:"));
  EXPECT_THAT(output, HasSubstr("out "));
  EXPECT_THAT(output, HasSubstr("in "));
  EXPECT_THAT(output, HasSubstr("inResult"));
  EXPECT_THAT(output, HasSubstr("EdgeRing:"));
}

}  // namespace
}  // namespace geos_google
