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

// Unit tests for third_party/geos/src/geomgraph/GeometryGraph.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geomgraph/GeometryGraph.cpp

#include "third_party/geos/include/geos/geomgraph/GeometryGraph.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/geomgraph/NodeMap.h"
#include "third_party/geos/include/geos/geomgraph/PlanarGraph.h"
#include "third_party/geos/include/geos/geomgraph/index/SegmentIntersector.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateFilter;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateSequenceFilter;
using geos::geom::CoordinateXY;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Location;
using geos::geom::MultiPolygon;
using geos::geom::Polygon;
using geos::geom::Position;
using geos::geomgraph::Edge;
using geos::geomgraph::GeometryGraph;
using geos::geomgraph::Label;
using geos::geomgraph::Node;
using geos::geomgraph::PlanarGraph;
using geos::io::WKTReader;
using geos::util::CurrentThreadInterrupt;
using geos::util::GEOSException;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;

// RAII owner for Edge* pointers populated by GeometryGraph::computeSplitEdges.
struct OwnedEdgeList {
  std::vector<Edge*> edges;

  ~OwnedEdgeList() {
    for (Edge* e : edges) {
      std::unique_ptr<Edge> owned(e);
    }
  }
};

// Minimal non-curved Geometry subclass outside GEOS's known concrete hierarchy
// to test GeometryGraph::add's unknown-geometry fallback error path.
class UnknownGeometry final : public Geometry {
 public:
  explicit UnknownGeometry(const GeometryFactory* factory)
      : Geometry(factory) {}

  const CoordinateXY* getCoordinate() const override { return nullptr; }
  std::unique_ptr<CoordinateSequence> getCoordinates() const override {
    return std::make_unique<CoordinateSequence>();
  }
  std::size_t getNumPoints() const override { return 0; }
  std::string getGeometryType() const override { return "UnknownGeometry"; }
  GeometryTypeId getGeometryTypeId() const override {
    return GeometryTypeId::GEOS_GEOMETRYCOLLECTION;
  }
  bool isEmpty() const override { return false; }
  Dimension::DimensionType getDimension() const override {
    return Dimension::P;
  }
  uint8_t getCoordinateDimension() const override { return 2; }
  bool hasZ() const override { return false; }
  bool hasM() const override { return false; }
  std::unique_ptr<Geometry> getBoundary() const override { return nullptr; }
  int getBoundaryDimension() const override { return Dimension::False; }
  const Envelope* getEnvelopeInternal() const override { return &envelope_; }
  bool equalsExact(const Geometry* /*other*/,
                   double /*tolerance*/) const override {
    return false;
  }
  bool equalsIdentical(const Geometry* /*other*/) const override {
    return false;
  }
  void apply_rw(const CoordinateFilter* /*filter*/) override {}
  void apply_ro(CoordinateFilter* /*filter*/) const override {}
  void apply_rw(CoordinateSequenceFilter& /*filter*/) override {}
  void apply_ro(CoordinateSequenceFilter& /*filter*/) const override {}
  void normalize() override {}
  void geometryChangedAction() override {}

 protected:
  Geometry* cloneImpl() const override { return new UnknownGeometry(*this); }
  Geometry* reverseImpl() const override { return new UnknownGeometry(*this); }
  int compareToSameClass(const Geometry* /*geom*/) const override { return 0; }
  int getSortIndex() const override { return 0; }

 private:
  Envelope envelope_;
};

TEST(GeometryGraphTest, StaticIsInBoundaryAndDetermineBoundaryMod2) {
  struct BoundaryCase {
    int count;
    bool expected_in_boundary;
    Location expected_location;
  };
  constexpr auto kCases = std::to_array<BoundaryCase>({
      {.count = 0,
       .expected_in_boundary = false,
       .expected_location = Location::INTERIOR},
      {.count = 1,
       .expected_in_boundary = true,
       .expected_location = Location::BOUNDARY},
      {.count = 2,
       .expected_in_boundary = false,
       .expected_location = Location::INTERIOR},
      {.count = 3,
       .expected_in_boundary = true,
       .expected_location = Location::BOUNDARY},
      {.count = 4,
       .expected_in_boundary = false,
       .expected_location = Location::INTERIOR},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.count);
    EXPECT_EQ(GeometryGraph::isInBoundary(c.count), c.expected_in_boundary);
    EXPECT_EQ(GeometryGraph::determineBoundary(c.count), c.expected_location);
  }
}

TEST(GeometryGraphTest, StaticDetermineBoundaryRespectsAllBoundaryNodeRules) {
  const auto& mod2 = BoundaryNodeRule::getBoundaryRuleMod2();
  const auto& ogsfs = BoundaryNodeRule::getBoundaryOGCSFS();
  const auto& endpoint = BoundaryNodeRule::getBoundaryEndPoint();
  const auto& multivalent = BoundaryNodeRule::getBoundaryMultivalentEndPoint();
  const auto& monovalent = BoundaryNodeRule::getBoundaryMonovalentEndPoint();

  EXPECT_EQ(GeometryGraph::determineBoundary(mod2, 0), Location::INTERIOR);
  EXPECT_EQ(GeometryGraph::determineBoundary(mod2, 1), Location::BOUNDARY);
  EXPECT_EQ(GeometryGraph::determineBoundary(mod2, 2), Location::INTERIOR);

  EXPECT_EQ(GeometryGraph::determineBoundary(ogsfs, 1), Location::BOUNDARY);
  EXPECT_EQ(GeometryGraph::determineBoundary(ogsfs, 2), Location::INTERIOR);

  EXPECT_EQ(GeometryGraph::determineBoundary(endpoint, 0), Location::INTERIOR);
  EXPECT_EQ(GeometryGraph::determineBoundary(endpoint, 1), Location::BOUNDARY);
  EXPECT_EQ(GeometryGraph::determineBoundary(endpoint, 2), Location::BOUNDARY);

  EXPECT_EQ(GeometryGraph::determineBoundary(multivalent, 1),
            Location::INTERIOR);
  EXPECT_EQ(GeometryGraph::determineBoundary(multivalent, 2),
            Location::BOUNDARY);
  EXPECT_EQ(GeometryGraph::determineBoundary(multivalent, 3),
            Location::BOUNDARY);

  EXPECT_EQ(GeometryGraph::determineBoundary(monovalent, 0),
            Location::INTERIOR);
  EXPECT_EQ(GeometryGraph::determineBoundary(monovalent, 1),
            Location::BOUNDARY);
  EXPECT_EQ(GeometryGraph::determineBoundary(monovalent, 2),
            Location::INTERIOR);
}

TEST(GeometryGraphTest, NullParentGeometryConstructorsAndDefaultAccessors) {
  {
    GeometryGraph graph(0, nullptr);
    EXPECT_EQ(graph.getGeometry(), nullptr);
    EXPECT_EQ(&graph.getBoundaryNodeRule(),
              &BoundaryNodeRule::getBoundaryOGCSFS());
    EXPECT_TRUE(graph.getEdges()->empty());
    EXPECT_FALSE(graph.hasTooFewPoints());
    EXPECT_TRUE(graph.getInvalidPoint().equals2D(Coordinate(0.0, 0.0)));
    EXPECT_TRUE(std::isnan(graph.getInvalidPoint().z));
    EXPECT_TRUE(graph.getBoundaryNodes()->empty());
    EXPECT_TRUE(graph.getBoundaryPoints()->isEmpty());
  }

  {
    const auto& ep_rule = BoundaryNodeRule::getBoundaryEndPoint();
    std::unique_ptr<PlanarGraph> base =
        std::make_unique<GeometryGraph>(1, nullptr, ep_rule);
    auto* graph = static_cast<GeometryGraph*>(base.get());
    EXPECT_EQ(graph->getGeometry(), nullptr);
    EXPECT_EQ(&graph->getBoundaryNodeRule(), &ep_rule);
    EXPECT_TRUE(graph->getEdges()->empty());
    EXPECT_TRUE(graph->getBoundaryNodes()->empty());
  }
}

TEST(GeometryGraphTest, EmptyGeometriesProduceEmptyGraph) {
  constexpr std::array<std::string_view, 7> kEmptyWkts = {
      "POINT EMPTY",
      "LINESTRING EMPTY",
      "POLYGON EMPTY",
      "MULTIPOINT EMPTY",
      "MULTILINESTRING EMPTY",
      "MULTIPOLYGON EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
  };

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (std::string_view wkt : kEmptyWkts) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr geom = reader.read(std::string(wkt));
    GeometryGraph graph(0, geom.get());
    EXPECT_EQ(graph.getGeometry(), geom.get());
    EXPECT_TRUE(graph.getEdges()->empty());
    EXPECT_TRUE(graph.getBoundaryNodes()->empty());
    EXPECT_TRUE(graph.getBoundaryPoints()->isEmpty());
    EXPECT_FALSE(graph.hasTooFewPoints());
  }

  std::unique_ptr<LinearRing> empty_ring = factory->createLinearRing();
  GeometryGraph ring_graph(0, empty_ring.get());
  EXPECT_TRUE(ring_graph.getEdges()->empty());
  EXPECT_TRUE(ring_graph.getBoundaryNodes()->empty());
}

TEST(GeometryGraphTest, UnsupportedCurvedAndUnknownGeometriesThrow) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr std::array<std::string_view, 6> kCurvedWkts = {
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "COMPOUNDCURVE ((0 0, 1 0), CIRCULARSTRING (1 0, 2 1, 3 0))",
      "CURVEPOLYGON (COMPOUNDCURVE ((0 0, 2 0), CIRCULARSTRING (2 0, 1 2, 0 "
      "0)))",
      "MULTICURVE (CIRCULARSTRING (0 0, 1 1, 2 0))",
      "MULTISURFACE (CURVEPOLYGON (COMPOUNDCURVE ((0 0, 2 0), CIRCULARSTRING "
      "(2 0, 1 2, 0 0))))",
      "GEOMETRYCOLLECTION (POINT (0 0), CIRCULARSTRING (0 0, 1 1, 2 0))",
  };

  for (std::string_view wkt : kCurvedWkts) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr curved = reader.read(std::string(wkt));
    ASSERT_NE(curved, nullptr);
    EXPECT_THROW(GeometryGraph(0, curved.get()), UnsupportedOperationException);
  }

  UnknownGeometry unknown(factory.get());
  EXPECT_THROW(GeometryGraph(0, &unknown), UnsupportedOperationException);
}

TEST(GeometryGraphTest, PointAndMultiPointInsertInteriorNodes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr pt = reader.read("POINT (3 4)");
  GeometryGraph pt_graph(0, pt.get());
  EXPECT_TRUE(pt_graph.getEdges()->empty());
  EXPECT_TRUE(pt_graph.getBoundaryNodes()->empty());
  EXPECT_TRUE(pt_graph.getBoundaryPoints()->isEmpty());
  Node* node = pt_graph.getNodeMap()->find(Coordinate(3.0, 4.0));
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->getLabel().getLocation(0), Location::INTERIOR);

  // Duplicate point triggers insertPoint when node label is already non-null.
  Geometry::Ptr mpt = reader.read("MULTIPOINT ((1 2), (1 2), (5 6))");
  GeometryGraph mpt_graph(1, mpt.get());
  EXPECT_TRUE(mpt_graph.getEdges()->empty());
  EXPECT_TRUE(mpt_graph.getBoundaryNodes()->empty());
  std::vector<Node*> nodes;
  mpt_graph.getNodes(nodes);
  EXPECT_EQ(nodes.size(), 2);
  Node* dup_node = mpt_graph.getNodeMap()->find(Coordinate(1.0, 2.0));
  ASSERT_NE(dup_node, nullptr);
  EXPECT_EQ(dup_node->getLabel().getLocation(1), Location::INTERIOR);
  EXPECT_EQ(dup_node->getLabel().getLocation(0), Location::NONE);
}

TEST(GeometryGraphTest, OpenLineStringBoundaryCachingAndFindEdge) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  auto ls = reader.read<LineString>("LINESTRING (0 0, 5 0, 10 0)");
  GeometryGraph graph(1, ls.get());

  ASSERT_EQ(graph.getEdges()->size(), 1);
  Edge* edge = graph.findEdge(ls.get());
  ASSERT_NE(edge, nullptr);
  EXPECT_EQ(edge, graph.getEdges()->front());
  EXPECT_EQ(edge->getLabel().getLocation(0), Location::NONE);
  EXPECT_EQ(edge->getLabel().getLocation(1), Location::INTERIOR);

  // Calling getBoundaryNodes() and getBoundaryPoints() twice tests caching.
  std::vector<Node*>* bdy_nodes_1 = graph.getBoundaryNodes();
  std::vector<Node*>* bdy_nodes_2 = graph.getBoundaryNodes();
  EXPECT_EQ(bdy_nodes_1, bdy_nodes_2);
  EXPECT_EQ(bdy_nodes_1->size(), 2);

  std::vector<Node*> explicit_bdy_nodes;
  graph.getBoundaryNodes(explicit_bdy_nodes);
  EXPECT_EQ(explicit_bdy_nodes.size(), 2);

  CoordinateSequence* bdy_pts_1 = graph.getBoundaryPoints();
  CoordinateSequence* bdy_pts_2 = graph.getBoundaryPoints();
  EXPECT_EQ(bdy_pts_1, bdy_pts_2);
  ASSERT_EQ(bdy_pts_1->getSize(), 2);
  EXPECT_TRUE(graph.isBoundaryNode(1, Coordinate(0.0, 0.0)));
  EXPECT_TRUE(graph.isBoundaryNode(1, Coordinate(10.0, 0.0)));
  EXPECT_FALSE(graph.isBoundaryNode(1, Coordinate(5.0, 0.0)));
}

TEST(GeometryGraphTest, ClosedLineStringAndMultiLineStringBoundaryRules) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr closed = reader.read("LINESTRING (0 0, 10 0, 10 10, 0 0)");

  GeometryGraph mod2_graph(0, closed.get(),
                           BoundaryNodeRule::getBoundaryRuleMod2());
  EXPECT_TRUE(mod2_graph.getBoundaryNodes()->empty());
  Node* start_mod2 = mod2_graph.getNodeMap()->find(Coordinate(0.0, 0.0));
  ASSERT_NE(start_mod2, nullptr);
  EXPECT_EQ(start_mod2->getLabel().getLocation(0), Location::INTERIOR);

  GeometryGraph endpoint_graph(0, closed.get(),
                               BoundaryNodeRule::getBoundaryEndPoint());
  EXPECT_EQ(endpoint_graph.getBoundaryNodes()->size(), 1);
  EXPECT_TRUE(endpoint_graph.isBoundaryNode(0, Coordinate(0.0, 0.0)));

  // Two lines meeting at (0, 0): under MonovalentEndPoint, (0, 0) transitions
  // BOUNDARY -> INTERIOR, leaving only degree-1 endpoints as boundary nodes.
  Geometry::Ptr mls_two =
      reader.read("MULTILINESTRING ((0 0, 1 0), (0 0, 0 1), (5 5, 6 6))");
  GeometryGraph monovalent_two(
      0, mls_two.get(), BoundaryNodeRule::getBoundaryMonovalentEndPoint());
  EXPECT_FALSE(monovalent_two.isBoundaryNode(0, Coordinate(0.0, 0.0)));
  Node* shared_monovalent =
      monovalent_two.getNodeMap()->find(Coordinate(0.0, 0.0));
  ASSERT_NE(shared_monovalent, nullptr);
  EXPECT_EQ(shared_monovalent->getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_TRUE(monovalent_two.isBoundaryNode(0, Coordinate(1.0, 0.0)));
  EXPECT_TRUE(monovalent_two.isBoundaryNode(0, Coordinate(0.0, 1.0)));
  EXPECT_TRUE(monovalent_two.isBoundaryNode(0, Coordinate(5.0, 5.0)));
  EXPECT_TRUE(monovalent_two.isBoundaryNode(0, Coordinate(6.0, 6.0)));

  // Under MultivalentEndPoint, the first insertBoundaryPoint evaluates
  // boundaryCount=1 -> INTERIOR, so subsequent inserts never see BOUNDARY.
  GeometryGraph multivalent_two(
      0, mls_two.get(), BoundaryNodeRule::getBoundaryMultivalentEndPoint());
  EXPECT_TRUE(multivalent_two.getBoundaryNodes()->empty());
  EXPECT_FALSE(multivalent_two.isBoundaryNode(0, Coordinate(0.0, 0.0)));
  Node* shared_multivalent =
      multivalent_two.getNodeMap()->find(Coordinate(0.0, 0.0));
  ASSERT_NE(shared_multivalent, nullptr);
  EXPECT_EQ(shared_multivalent->getLabel().getLocation(0), Location::INTERIOR);

  // Three meeting endpoints toggle Mod2: BOUNDARY -> INTERIOR -> BOUNDARY.
  // insertBoundaryPoint only inspects whether the current location is BOUNDARY,
  // so MonovalentEndPoint also toggles BOUNDARY -> INTERIOR -> BOUNDARY.
  Geometry::Ptr mls_three = reader.read(
      "MULTILINESTRING ((0 0, 1 0), (0 0, 0 1), (0 0, -1 0), (5 5, 6 6))");
  GeometryGraph mod2_mls(0, mls_three.get());
  EXPECT_TRUE(mod2_mls.isBoundaryNode(0, Coordinate(0.0, 0.0)));

  GeometryGraph monovalent_three(
      0, mls_three.get(), BoundaryNodeRule::getBoundaryMonovalentEndPoint());
  EXPECT_TRUE(monovalent_three.isBoundaryNode(0, Coordinate(0.0, 0.0)));
  EXPECT_TRUE(monovalent_three.isBoundaryNode(0, Coordinate(1.0, 0.0)));
  EXPECT_TRUE(monovalent_three.isBoundaryNode(0, Coordinate(5.0, 5.0)));
}

TEST(GeometryGraphTest, LinearRingAndPolygonOrientationAndHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // LinearRing added directly goes through addLineString.
  auto ring = reader.read<LinearRing>("LINEARRING (0 0, 4 0, 4 4, 0 4, 0 0)");
  GeometryGraph ring_graph(0, ring.get());
  ASSERT_EQ(ring_graph.getEdges()->size(), 1);
  Edge* ring_edge = ring_graph.findEdge(ring.get());
  ASSERT_NE(ring_edge, nullptr);
  EXPECT_EQ(ring_edge->getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_TRUE(ring_graph.getBoundaryNodes()->empty());

  // Polygon with CCW shell, CW hole, and CCW hole.
  auto poly = reader.read<Polygon>(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 2 5, 5 5, 5 2, 2 2), "
      "(10 10, 15 10, 15 15, 10 15, 10 10))");
  GeometryGraph poly_graph(0, poly.get());
  ASSERT_EQ(poly_graph.getEdges()->size(), 3);

  Edge* ccw_shell = poly_graph.findEdge(poly->getExteriorRing());
  ASSERT_NE(ccw_shell, nullptr);
  EXPECT_EQ(ccw_shell->getLabel().getLocation(0, Position::ON),
            Location::BOUNDARY);
  EXPECT_EQ(ccw_shell->getLabel().getLocation(0, Position::LEFT),
            Location::INTERIOR);
  EXPECT_EQ(ccw_shell->getLabel().getLocation(0, Position::RIGHT),
            Location::EXTERIOR);

  Edge* cw_hole = poly_graph.findEdge(poly->getInteriorRingN(0));
  ASSERT_NE(cw_hole, nullptr);
  EXPECT_EQ(cw_hole->getLabel().getLocation(0, Position::LEFT),
            Location::INTERIOR);
  EXPECT_EQ(cw_hole->getLabel().getLocation(0, Position::RIGHT),
            Location::EXTERIOR);

  Edge* ccw_hole = poly_graph.findEdge(poly->getInteriorRingN(1));
  ASSERT_NE(ccw_hole, nullptr);
  EXPECT_EQ(ccw_hole->getLabel().getLocation(0, Position::LEFT),
            Location::EXTERIOR);
  EXPECT_EQ(ccw_hole->getLabel().getLocation(0, Position::RIGHT),
            Location::INTERIOR);

  // CW shell keeps default cwLeft=EXTERIOR, cwRight=INTERIOR.
  auto cw_poly =
      reader.read<Polygon>("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
  GeometryGraph cw_graph(0, cw_poly.get());
  Edge* cw_shell = cw_graph.findEdge(cw_poly->getExteriorRing());
  ASSERT_NE(cw_shell, nullptr);
  EXPECT_EQ(cw_shell->getLabel().getLocation(0, Position::LEFT),
            Location::EXTERIOR);
  EXPECT_EQ(cw_shell->getLabel().getLocation(0, Position::RIGHT),
            Location::INTERIOR);
  EXPECT_EQ(cw_graph.getBoundaryNodes()->size(), 1);
  EXPECT_TRUE(cw_graph.isBoundaryNode(0, Coordinate(0.0, 0.0)));
}

TEST(GeometryGraphTest, PolygonWithEmptyHoleSkipsEmptyRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto shell_seq = std::make_unique<CoordinateSequence>();
  shell_seq->add(Coordinate(0.0, 0.0));
  shell_seq->add(Coordinate(10.0, 0.0));
  shell_seq->add(Coordinate(10.0, 10.0));
  shell_seq->add(Coordinate(0.0, 10.0));
  shell_seq->add(Coordinate(0.0, 0.0));
  std::unique_ptr<LinearRing> shell =
      factory->createLinearRing(std::move(shell_seq));

  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory->createLinearRing());
  std::unique_ptr<Polygon> poly =
      factory->createPolygon(std::move(shell), std::move(holes));

  GeometryGraph graph(0, poly.get());
  EXPECT_EQ(graph.getEdges()->size(), 1);
  EXPECT_FALSE(graph.hasTooFewPoints());
}

TEST(GeometryGraphTest, CollapsedLineStringAndPolygonRingSetTooFewPoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // LineString whose points collapse below 2 after removing duplicates.
  auto line_seq = std::make_unique<CoordinateSequence>();
  line_seq->add(Coordinate(5.0, 7.0));
  line_seq->add(Coordinate(5.0, 7.0));
  line_seq->add(Coordinate(5.0, 7.0));
  std::unique_ptr<LineString> collapsed_line =
      factory->createLineString(std::move(line_seq));
  GeometryGraph line_graph(0, collapsed_line.get());
  EXPECT_TRUE(line_graph.hasTooFewPoints());
  EXPECT_TRUE(line_graph.getInvalidPoint().equals2D(Coordinate(5.0, 7.0)));
  EXPECT_TRUE(line_graph.getEdges()->empty());

  // Closed LinearRing of 3 points collapses below 4 after removing duplicates.
  auto ring_seq = std::make_unique<CoordinateSequence>();
  ring_seq->add(Coordinate(3.0, 9.0));
  ring_seq->add(Coordinate(4.0, 9.0));
  ring_seq->add(Coordinate(3.0, 9.0));
  std::unique_ptr<Polygon> collapsed_poly =
      factory->createPolygon(factory->createLinearRing(std::move(ring_seq)));
  GeometryGraph poly_graph(0, collapsed_poly.get());
  EXPECT_TRUE(poly_graph.hasTooFewPoints());
  EXPECT_TRUE(poly_graph.getInvalidPoint().equals2D(Coordinate(3.0, 9.0)));
  EXPECT_TRUE(poly_graph.getEdges()->empty());
}

TEST(GeometryGraphTest, PolygonRingWithNanCoordinateThrowsInIsCcw) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto seq = std::make_unique<CoordinateSequence>();
  seq->add(Coordinate(0.0, 0.0));
  seq->add(Coordinate(1.0, 1.0));
  seq->add(Coordinate(2.0, std::numeric_limits<double>::quiet_NaN()));
  seq->add(Coordinate(0.0, 0.0));
  std::unique_ptr<Polygon> nan_poly =
      factory->createPolygon(factory->createLinearRing(std::move(seq)));

  EXPECT_THROW(GeometryGraph(0, nan_poly.get()), IllegalArgumentException);
}

TEST(GeometryGraphTest, GeometryCollectionAndManualAddEdgeAddPoint) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr gc = reader.read(
      "GEOMETRYCOLLECTION ("
      "POINT (0 0), "
      "LINESTRING (0 0, 5 0), "
      "POLYGON ((1 1, 4 1, 4 4, 1 4, 1 1)), "
      "POINT EMPTY)");
  GeometryGraph gc_graph(0, gc.get());
  EXPECT_EQ(gc_graph.getEdges()->size(), 2);
  Node* shared = gc_graph.getNodeMap()->find(Coordinate(0.0, 0.0));
  ASSERT_NE(shared, nullptr);
  EXPECT_EQ(shared->getLabel().getLocation(0), Location::BOUNDARY);

  GeometryGraph manual(0, nullptr);
  auto edge_pts = std::make_unique<CoordinateSequence>();
  edge_pts->add(Coordinate(1.0, 2.0));
  edge_pts->add(Coordinate(3.0, 4.0));
  edge_pts->add(Coordinate(5.0, 6.0));
  auto edge =
      std::make_unique<Edge>(edge_pts.release(), Label(0, Location::INTERIOR));
  manual.addEdge(edge.release());

  Coordinate extra_pt(9.0, 8.0);
  manual.addPoint(extra_pt);

  EXPECT_EQ(manual.getEdges()->size(), 1);
  EXPECT_EQ(manual.getBoundaryNodes()->size(), 2);
  EXPECT_TRUE(manual.isBoundaryNode(0, Coordinate(1.0, 2.0)));
  EXPECT_TRUE(manual.isBoundaryNode(0, Coordinate(5.0, 6.0)));
  Node* interior_node = manual.getNodeMap()->find(Coordinate(9.0, 8.0));
  ASSERT_NE(interior_node, nullptr);
  EXPECT_EQ(interior_node->getLabel().getLocation(0), Location::INTERIOR);

  // Manually populated graph with null parentGeom can still compute self-nodes
  // when no envelope filter is supplied.
  auto cross_pts = std::make_unique<CoordinateSequence>();
  cross_pts->add(Coordinate(1.0, 4.0));
  cross_pts->add(Coordinate(3.0, 2.0));
  auto cross_edge =
      std::make_unique<Edge>(cross_pts.release(), Label(0, Location::INTERIOR));
  manual.addEdge(cross_edge.release());
  LineIntersector li;
  auto self_si = manual.computeSelfNodes(li, false);
  EXPECT_TRUE(self_si->hasIntersection());
  EXPECT_TRUE(self_si->hasProperIntersection());
  EXPECT_TRUE(
      self_si->getProperIntersectionPoint().equals2D(Coordinate(2.0, 3.0)));
  Node* cross_node = manual.getNodeMap()->find(Coordinate(2.0, 3.0));
  ASSERT_NE(cross_node, nullptr);
  EXPECT_EQ(cross_node->getLabel().getLocation(0), Location::INTERIOR);
}

TEST(GeometryGraphTest, ComputeSelfNodesLineAndRingModesAndSplitEdges) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  LineIntersector li;

  // Non-ring LineString self-intersection is detected even when
  // computeRingSelfNodes is false.
  Geometry::Ptr self_line = reader.read("LINESTRING (0 0, 10 10, 10 0, 0 10)");
  GeometryGraph line_graph(0, self_line.get());
  auto line_si = line_graph.computeSelfNodes(&li, false);
  EXPECT_TRUE(line_si->hasIntersection());
  EXPECT_TRUE(line_si->hasProperIntersection());
  EXPECT_TRUE(
      line_si->getProperIntersectionPoint().equals2D(Coordinate(5.0, 5.0)));
  Node* int_node = line_graph.getNodeMap()->find(Coordinate(5.0, 5.0));
  ASSERT_NE(int_node, nullptr);
  EXPECT_EQ(int_node->getLabel().getLocation(0), Location::INTERIOR);

  OwnedEdgeList split_line_edges;
  line_graph.computeSplitEdges(&split_line_edges.edges);
  EXPECT_EQ(split_line_edges.edges.size(), 3);

  // Bowtie LinearRing skips self-test when computeRingSelfNodes is false,
  // and detects self-intersection when computeRingSelfNodes is true.
  Geometry::Ptr bowtie_ring =
      reader.read("LINEARRING (0 0, 10 10, 10 0, 0 10, 0 0)");
  {
    GeometryGraph graph_no_ring(0, bowtie_ring.get());
    auto si = graph_no_ring.computeSelfNodes(li, false);
    EXPECT_FALSE(si->hasIntersection());
  }
  {
    GeometryGraph graph_with_ring(0, bowtie_ring.get());
    auto si = graph_with_ring.computeSelfNodes(li, true);
    EXPECT_TRUE(si->hasIntersection());
    EXPECT_TRUE(
        si->getProperIntersectionPoint().equals2D(Coordinate(5.0, 5.0)));
  }

  // Self-intersecting Polygon exercises useBoundaryDeterminationRule == true
  // with edge location BOUNDARY.
  Geometry::Ptr bowtie_poly =
      reader.read("POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))");
  {
    GeometryGraph poly_no_ring(0, bowtie_poly.get());
    auto si = poly_no_ring.computeSelfNodes(li, false);
    EXPECT_FALSE(si->hasIntersection());

    GeometryGraph poly_with_ring(0, bowtie_poly.get());
    auto si_ring = poly_with_ring.computeSelfNodes(li, true);
    EXPECT_TRUE(si_ring->hasIntersection());
    Node* bdy_int = poly_with_ring.getNodeMap()->find(Coordinate(5.0, 5.0));
    ASSERT_NE(bdy_int, nullptr);
    EXPECT_EQ(bdy_int->getLabel().getLocation(0), Location::BOUNDARY);
  }
}

TEST(GeometryGraphTest,
     ComputeSelfNodesMultiPolygonAndExistingBoundaryNodeHandling) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  LineIntersector li;

  // MultiPolygon sets useBoundaryDeterminationRule = false; overlapping
  // polygons exercise addSelfIntersectionNode when loc == BOUNDARY and rule is
  // disabled, plus skipping already-existing boundary nodes on the second
  // polygon edge.
  auto mp = reader.read<MultiPolygon>(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((5 5, 15 5, 15 15, 5 15, 5 5)))");
  {
    GeometryGraph mp_no_ring(0, mp.get());
    ASSERT_EQ(mp_no_ring.getEdges()->size(), 2);
    EXPECT_NE(mp_no_ring.findEdge(mp->getGeometryN(0)->getExteriorRing()),
              nullptr);
    EXPECT_NE(mp_no_ring.findEdge(mp->getGeometryN(1)->getExteriorRing()),
              nullptr);
    auto si = mp_no_ring.computeSelfNodes(li, false);
    EXPECT_TRUE(si->hasIntersection());
    EXPECT_TRUE(si->hasProperIntersection());
    Node* cross_node = mp_no_ring.getNodeMap()->find(Coordinate(10.0, 5.0));
    ASSERT_NE(cross_node, nullptr);
    EXPECT_EQ(cross_node->getLabel().getLocation(0), Location::BOUNDARY);
  }
  {
    GeometryGraph mp_with_ring(0, mp.get());
    auto si = mp_with_ring.computeSelfNodes(li, true);
    EXPECT_TRUE(si->hasIntersection());
  }

  // Endpoint of second line touches interior of first line: (1, 0) is already a
  // boundary node, hitting the early-return guard in addSelfIntersectionNode.
  Geometry::Ptr mls_touch =
      reader.read("MULTILINESTRING ((0 0, 2 0), (1 -1, 1 0))");
  GeometryGraph touch_graph(0, mls_touch.get());
  auto touch_si = touch_graph.computeSelfNodes(li, false);
  EXPECT_TRUE(touch_si->hasIntersection());
  EXPECT_FALSE(touch_si->hasProperIntersection());
  EXPECT_TRUE(touch_graph.isBoundaryNode(0, Coordinate(1.0, 0.0)));
}

TEST(GeometryGraphTest, ComputeSelfNodesEnvelopeFilteringAndInterrupt) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  LineIntersector li;

  Geometry::Ptr two_crossings = reader.read(
      "MULTILINESTRING ("
      "(0 0, 10 10), (0 10, 10 0), "
      "(20 20, 30 30), (20 30, 30 20))");

  // Envelope covering the whole geometry skips filtering.
  {
    GeometryGraph graph(0, two_crossings.get());
    Envelope full_env(0.0, 30.0, 0.0, 30.0);
    auto si = graph.computeSelfNodes(li, false, &full_env);
    EXPECT_TRUE(si->hasIntersection());
    EXPECT_NE(graph.getNodeMap()->find(Coordinate(5.0, 5.0)), nullptr);
    EXPECT_NE(graph.getNodeMap()->find(Coordinate(25.0, 25.0)), nullptr);
  }

  // Partial envelope filters out edges outside [0, 10] x [0, 10].
  {
    GeometryGraph graph(0, two_crossings.get());
    Envelope left_env(0.0, 10.0, 0.0, 10.0);
    auto si = graph.computeSelfNodes(li, false, &left_env);
    EXPECT_TRUE(si->hasIntersection());
    EXPECT_NE(graph.getNodeMap()->find(Coordinate(5.0, 5.0)), nullptr);
    EXPECT_EQ(graph.getNodeMap()->find(Coordinate(25.0, 25.0)), nullptr);
  }

  // Disjoint envelope filters out all edges.
  {
    GeometryGraph graph(0, two_crossings.get());
    Envelope outside_env(100.0, 110.0, 100.0, 110.0);
    auto si = graph.computeSelfNodes(li, false, &outside_env);
    EXPECT_FALSE(si->hasIntersection());
  }

  // CurrentThreadInterrupt stops computeSelfNodes cleanly without leaks.
  {
    GeometryGraph graph(0, two_crossings.get());
    auto always_interrupt = [](void*) -> int { return 1; };
    auto* prev_cb =
        CurrentThreadInterrupt::registerCallback(always_interrupt, nullptr);
    absl::Cleanup restore_cb([prev_cb] {
      CurrentThreadInterrupt::registerCallback(prev_cb, nullptr);
    });
    EXPECT_THROW(graph.computeSelfNodes(li, false), GEOSException);
  }
}

TEST(GeometryGraphTest, ComputeEdgeIntersectionsIncludeProperAndSplitEdges) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  LineIntersector li;

  Geometry::Ptr line_a = reader.read("LINESTRING (0 0, 10 10)");
  Geometry::Ptr line_b = reader.read("LINESTRING (0 10, 10 0)");

  // includeProper = true records proper intersections on edges for splitting.
  {
    GeometryGraph g0(0, line_a.get());
    GeometryGraph g1(1, line_b.get());
    auto si = g0.computeEdgeIntersections(&g1, &li, true);
    EXPECT_TRUE(si->hasIntersection());
    EXPECT_TRUE(si->hasProperIntersection());
    EXPECT_TRUE(si->hasProperInteriorIntersection());
    EXPECT_TRUE(
        si->getProperIntersectionPoint().equals2D(Coordinate(5.0, 5.0)));

    OwnedEdgeList split0;
    OwnedEdgeList split1;
    g0.computeSplitEdges(&split0.edges);
    g1.computeSplitEdges(&split1.edges);
    EXPECT_EQ(split0.edges.size(), 2);
    EXPECT_EQ(split1.edges.size(), 2);
  }

  // includeProper = false detects proper intersection without splitting.
  {
    GeometryGraph g0(0, line_a.get());
    GeometryGraph g1(1, line_b.get());
    auto si = g0.computeEdgeIntersections(&g1, &li, false);
    EXPECT_TRUE(si->hasIntersection());
    EXPECT_TRUE(si->hasProperIntersection());
    EXPECT_TRUE(si->hasProperInteriorIntersection());

    OwnedEdgeList split0;
    OwnedEdgeList split1;
    g0.computeSplitEdges(&split0.edges);
    g1.computeSplitEdges(&split1.edges);
    EXPECT_EQ(split0.edges.size(), 1);
    EXPECT_EQ(split1.edges.size(), 1);
  }

  // Non-proper endpoint-to-interior touch splits even when includeProper=false.
  Geometry::Ptr base = reader.read("LINESTRING (0 0, 10 0)");
  Geometry::Ptr stem = reader.read("LINESTRING (5 -5, 5 0)");
  {
    GeometryGraph g0(0, base.get());
    GeometryGraph g1(1, stem.get());
    auto si = g0.computeEdgeIntersections(&g1, &li, false);
    EXPECT_TRUE(si->hasIntersection());
    EXPECT_FALSE(si->hasProperIntersection());
    EXPECT_FALSE(si->hasProperInteriorIntersection());

    OwnedEdgeList split0;
    g0.computeSplitEdges(&split0.edges);
    EXPECT_EQ(split0.edges.size(), 2);
  }

  // Proper segment intersection falling on an existing boundary node of g0 is
  // not a proper interior intersection.
  Geometry::Ptr with_bdy =
      reader.read("MULTILINESTRING ((0 0, 10 0), (5 0, 5 5))");
  Geometry::Ptr vertical = reader.read("LINESTRING (5 -5, 5 -1)");
  Geometry::Ptr crossing = reader.read("LINESTRING (5 -5, 5 2)");
  {
    GeometryGraph g0(0, with_bdy.get());
    GeometryGraph g1(1, crossing.get());
    auto si = g0.computeEdgeIntersections(&g1, &li, true);
    EXPECT_TRUE(si->hasIntersection());
    EXPECT_TRUE(si->hasProperIntersection());
    EXPECT_FALSE(si->hasProperInteriorIntersection());
  }
  {
    GeometryGraph g0(0, with_bdy.get());
    GeometryGraph g1(1, vertical.get());
    auto si = g0.computeEdgeIntersections(&g1, &li, true);
    EXPECT_FALSE(si->hasIntersection());
  }
}

TEST(GeometryGraphTest, ComputeEdgeIntersectionsEnvelopeFiltering) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  LineIntersector li;

  Geometry::Ptr mls_a =
      reader.read("MULTILINESTRING ((0 0, 10 10), (20 20, 30 30))");
  Geometry::Ptr mls_b =
      reader.read("MULTILINESTRING ((0 10, 10 0), (20 30, 30 20))");

  // Full envelope covering both geometries finds both intersections.
  {
    GeometryGraph g0(0, mls_a.get());
    GeometryGraph g1(1, mls_b.get());
    Envelope full_env(0.0, 30.0, 0.0, 30.0);
    auto si = g0.computeEdgeIntersections(&g1, &li, true, &full_env);
    EXPECT_TRUE(si->hasIntersection());

    OwnedEdgeList split0;
    g0.computeSplitEdges(&split0.edges);
    EXPECT_EQ(split0.edges.size(), 4);
  }

  // Partial envelope filters both graphs to only the [0, 10] x [0, 10] pair.
  {
    GeometryGraph g0(0, mls_a.get());
    GeometryGraph g1(1, mls_b.get());
    Envelope partial_env(0.0, 10.0, 0.0, 10.0);
    auto si = g0.computeEdgeIntersections(&g1, &li, true, &partial_env);
    EXPECT_TRUE(si->hasIntersection());
    EXPECT_TRUE(
        si->getProperIntersectionPoint().equals2D(Coordinate(5.0, 5.0)));

    OwnedEdgeList split0;
    OwnedEdgeList split1;
    g0.computeSplitEdges(&split0.edges);
    g1.computeSplitEdges(&split1.edges);
    EXPECT_EQ(split0.edges.size(), 3);
    EXPECT_EQ(split1.edges.size(), 3);
  }

  // Envelope covering only g0 filters g1 while keeping all of g0's edges.
  {
    Geometry::Ptr single_a = reader.read("LINESTRING (0 0, 10 10)");
    GeometryGraph g0(0, single_a.get());
    GeometryGraph g1(1, mls_b.get());
    Envelope cover_g0(0.0, 10.0, 0.0, 10.0);
    auto si = g0.computeEdgeIntersections(&g1, &li, true, &cover_g0);
    EXPECT_TRUE(si->hasIntersection());
    EXPECT_TRUE(
        si->getProperIntersectionPoint().equals2D(Coordinate(5.0, 5.0)));

    OwnedEdgeList split0;
    OwnedEdgeList split1;
    g0.computeSplitEdges(&split0.edges);
    g1.computeSplitEdges(&split1.edges);
    EXPECT_EQ(split0.edges.size(), 2);
    EXPECT_EQ(split1.edges.size(), 3);
  }

  // Envelope covering only g1 filters g0 while keeping all of g1's edges.
  {
    Geometry::Ptr single_b = reader.read("LINESTRING (0 10, 10 0)");
    GeometryGraph g0(0, mls_a.get());
    GeometryGraph g1(1, single_b.get());
    Envelope cover_g1(0.0, 10.0, 0.0, 10.0);
    auto si = g0.computeEdgeIntersections(&g1, &li, true, &cover_g1);
    EXPECT_TRUE(si->hasIntersection());
    EXPECT_TRUE(
        si->getProperIntersectionPoint().equals2D(Coordinate(5.0, 5.0)));

    OwnedEdgeList split0;
    OwnedEdgeList split1;
    g0.computeSplitEdges(&split0.edges);
    g1.computeSplitEdges(&split1.edges);
    EXPECT_EQ(split0.edges.size(), 3);
    EXPECT_EQ(split1.edges.size(), 2);
  }

  // Envelope missing both geometries yields zero intersections.
  {
    GeometryGraph g0(0, mls_a.get());
    GeometryGraph g1(1, mls_b.get());
    Envelope empty_env(50.0, 60.0, 50.0, 60.0);
    auto si = g0.computeEdgeIntersections(&g1, &li, true, &empty_env);
    EXPECT_FALSE(si->hasIntersection());
  }
}

}  // namespace
}  // namespace geos_google
