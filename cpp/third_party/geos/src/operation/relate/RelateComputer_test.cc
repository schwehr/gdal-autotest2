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

#include "third_party/geos/include/geos/operation/relate/RelateComputer.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geomgraph/GeometryGraph.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relate/RelateOp.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::IntersectionMatrix;
using geos::geom::Location;
using geos::geomgraph::GeometryGraph;
using geos::io::WKTReader;
using geos::operation::relate::RelateComputer;
using geos::operation::relate::RelateOp;

class RelateComputerTest : public ::testing::Test {
 protected:
  RelateComputerTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  std::unique_ptr<IntersectionMatrix> ComputeIM(
      const Geometry* a, const Geometry* b,
      const BoundaryNodeRule& rule = BoundaryNodeRule::getBoundaryOGCSFS()) {
    std::vector<std::unique_ptr<GeometryGraph>> graphs;
    graphs.push_back(std::make_unique<GeometryGraph>(0, a, rule));
    graphs.push_back(std::make_unique<GeometryGraph>(1, b, rule));
    RelateComputer computer(graphs);
    return computer.computeIM();
  }

  void ExpectRelate(
      const std::string& wkt_a, const std::string& wkt_b,
      const std::string& expected_im,
      const BoundaryNodeRule& rule = BoundaryNodeRule::getBoundaryOGCSFS()) {
    auto a = ReadWkt(wkt_a);
    auto b = ReadWkt(wkt_b);
    auto im = ComputeIM(a.get(), b.get(), rule);
    ASSERT_NE(im, nullptr);
    EXPECT_EQ(im->toString(), expected_im)
        << "A=" << wkt_a << ", B=" << wkt_b << ", rule=" << rule.toString();

    auto op_im = RelateOp::relate(a.get(), b.get(), rule);
    ASSERT_NE(op_im, nullptr);
    EXPECT_EQ(op_im->toString(), expected_im);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(RelateComputerTest, EmptyGeometriesComputeDisjointIM) {
  ExpectRelate("POINT EMPTY", "POINT EMPTY", "FFFFFFFF2");
  ExpectRelate("LINESTRING EMPTY", "POLYGON EMPTY", "FFFFFFFF2");
  ExpectRelate("GEOMETRYCOLLECTION EMPTY", "MULTIPOLYGON EMPTY", "FFFFFFFF2");

  // One empty, one non-empty: exercises each branch of computeDisjointIM.
  ExpectRelate("POINT EMPTY", "POINT (1 2)", "FFFFFF0F2");
  ExpectRelate("POINT (1 2)", "POINT EMPTY", "FF0FFFFF2");
  ExpectRelate("LINESTRING EMPTY", "LINESTRING (0 0, 1 1)", "FFFFFF102");
  ExpectRelate("LINESTRING (0 0, 1 1)", "POLYGON EMPTY", "FF1FF0FF2");
  ExpectRelate("POLYGON EMPTY", "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
               "FFFFFF212");
  ExpectRelate("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))", "LINESTRING EMPTY",
               "FF2FF1FF2");
}

TEST_F(RelateComputerTest, DisjointNonEmptyGeometriesAcrossDimensions) {
  ExpectRelate("POINT (0 0)", "POINT (5 5)", "FF0FFF0F2");
  ExpectRelate("MULTIPOINT ((0 0), (1 1))", "MULTIPOINT ((5 5), (6 6))",
               "FF0FFF0F2");
  ExpectRelate("POINT (0 0)", "LINESTRING (5 0, 6 0)", "FF0FFF102");
  ExpectRelate("LINESTRING (0 0, 1 1)", "POLYGON ((5 5, 6 5, 6 6, 5 6, 5 5))",
               "FF1FF0212");
  ExpectRelate("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
               "POLYGON ((5 5, 6 5, 6 6, 5 6, 5 5))", "FF2FF1212");
}

TEST_F(RelateComputerTest, DisjointLinearGeometriesRespectBoundaryNodeRules) {
  const std::string closed_line = "LINESTRING (0 0, 1 0, 1 1, 0 0)";
  const std::string open_line = "LINESTRING (0 0, 1 1)";
  const std::string open_multi_line = "MULTILINESTRING ((0 0, 1 1))";
  const std::string disjoint_pt = "POINT (5 5)";

  // Closed ring has no boundary under Mod2 / OGC SFS / Monovalent,
  // and has boundary dimension P (0) under EndPoint / Multivalent.
  ExpectRelate(closed_line, disjoint_pt, "FF1FFF0F2",
               BoundaryNodeRule::getBoundaryOGCSFS());
  ExpectRelate(closed_line, disjoint_pt, "FF1FFF0F2",
               BoundaryNodeRule::getBoundaryRuleMod2());
  ExpectRelate(closed_line, disjoint_pt, "FF1FFF0F2",
               BoundaryNodeRule::getBoundaryMonovalentEndPoint());
  ExpectRelate(closed_line, disjoint_pt, "FF1FF00F2",
               BoundaryNodeRule::getBoundaryEndPoint());
  ExpectRelate(closed_line, disjoint_pt, "FF1FF00F2",
               BoundaryNodeRule::getBoundaryMultivalentEndPoint());

  // Open MultiLineString has valence-1 endpoints: boundary under Mod2 /
  // EndPoint / Monovalent, no boundary under Multivalent. Note that
  // BoundaryOp::boundaryLineString always treats a non-closed single
  // LineString's endpoints as boundary regardless of rule.
  ExpectRelate(open_line, disjoint_pt, "FF1FF00F2",
               BoundaryNodeRule::getBoundaryRuleMod2());
  ExpectRelate(open_line, disjoint_pt, "FF1FF00F2",
               BoundaryNodeRule::getBoundaryEndPoint());
  ExpectRelate(open_line, disjoint_pt, "FF1FF00F2",
               BoundaryNodeRule::getBoundaryMonovalentEndPoint());
  ExpectRelate(open_line, disjoint_pt, "FF1FF00F2",
               BoundaryNodeRule::getBoundaryMultivalentEndPoint());
  ExpectRelate(open_multi_line, disjoint_pt, "FF1FF00F2",
               BoundaryNodeRule::getBoundaryRuleMod2());
  ExpectRelate(open_multi_line, disjoint_pt, "FF1FF00F2",
               BoundaryNodeRule::getBoundaryEndPoint());
  ExpectRelate(open_multi_line, disjoint_pt, "FF1FF00F2",
               BoundaryNodeRule::getBoundaryMonovalentEndPoint());
  ExpectRelate(open_multi_line, disjoint_pt, "FF1FFF0F2",
               BoundaryNodeRule::getBoundaryMultivalentEndPoint());

  // Two lines sharing both endpoints have valence-2 endpoints.
  const std::string two_arc_loop = "MULTILINESTRING ((0 0, 1 0), (1 0, 0 0))";
  const std::string disjoint_poly = "POLYGON ((5 5, 6 5, 6 6, 5 6, 5 5))";
  ExpectRelate(two_arc_loop, disjoint_poly, "FF1FFF212",
               BoundaryNodeRule::getBoundaryRuleMod2());
  ExpectRelate(two_arc_loop, disjoint_poly, "FF1FFF212",
               BoundaryNodeRule::getBoundaryMonovalentEndPoint());
  ExpectRelate(two_arc_loop, disjoint_poly, "FF1FF0212",
               BoundaryNodeRule::getBoundaryEndPoint());
  ExpectRelate(two_arc_loop, disjoint_poly, "FF1FF0212",
               BoundaryNodeRule::getBoundaryMultivalentEndPoint());

  // LinearRing is a closed LineString subclass whose getBoundaryDim under
  // EndPoint must still report Dimension::P rather than LinearRing's
  // default Dimension::False.
  CoordinateSequence ring_coords{Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
                                 Coordinate(1.0, 1.0), Coordinate(0.0, 0.0)};
  auto ring = factory_->createLinearRing(ring_coords);
  auto pt_far = ReadWkt(disjoint_pt);
  EXPECT_EQ(ComputeIM(ring.get(), pt_far.get(),
                      BoundaryNodeRule::getBoundaryRuleMod2())
                ->toString(),
            "FF1FFF0F2");
  EXPECT_EQ(ComputeIM(ring.get(), pt_far.get(),
                      BoundaryNodeRule::getBoundaryEndPoint())
                ->toString(),
            "FF1FF00F2");
}

TEST_F(RelateComputerTest, PointAndMultiPointWithOverlappingEnvelopes) {
  ExpectRelate("POINT (1 2)", "POINT (1 2)", "0FFFFFFF2");
  ExpectRelate("MULTIPOINT ((0 0), (2 2))", "POINT (0 0)", "0F0FFFFF2");
  ExpectRelate("POINT (0 0)", "MULTIPOINT ((0 0), (2 2))", "0FFFFF0F2");
  ExpectRelate("MULTIPOINT ((0 0), (2 2))", "POINT (1 1)", "FF0FFF0F2");
}

TEST_F(RelateComputerTest, PointAgainstLineStringAndLinearRing) {
  // Exercises labelIsolatedEdge when target has dimension 0.
  ExpectRelate("POINT (5 5)", "LINESTRING (0 0, 10 10)", "0FFFFF102");
  ExpectRelate("LINESTRING (0 0, 10 10)", "POINT (5 5)", "0F1FF0FF2");
  ExpectRelate("POINT (0 0)", "LINESTRING (0 0, 10 10)", "F0FFFF102");
  ExpectRelate("LINESTRING (0 0, 10 10)", "POINT (0 0)", "FF10F0FF2");
  ExpectRelate("POINT (5 1)", "LINESTRING (0 0, 10 10)", "FF0FFF102");

  // Closed LineString endpoint under Mod2 vs EndPoint.
  // Under EndPoint, (0, 0) is the sole boundary point of `closed`, so
  // POINT (0 0) covers the entire boundary (EB == F), while POINT (10 0) leaves
  // (0, 0) in Exterior(A) (EB == 0).
  const std::string closed = "LINESTRING (0 0, 10 0, 10 10, 0 0)";
  ExpectRelate("POINT (0 0)", closed, "0FFFFF1F2",
               BoundaryNodeRule::getBoundaryRuleMod2());
  ExpectRelate("POINT (0 0)", closed, "F0FFFF1F2",
               BoundaryNodeRule::getBoundaryEndPoint());
  ExpectRelate("POINT (10 0)", closed, "0FFFFF1F2",
               BoundaryNodeRule::getBoundaryRuleMod2());
  ExpectRelate("POINT (10 0)", closed, "0FFFFF102",
               BoundaryNodeRule::getBoundaryEndPoint());

  // LinearRing constructed directly via GeometryFactory.
  CoordinateSequence ring_coords{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                                 Coordinate(10.0, 10.0), Coordinate(0.0, 0.0)};
  auto ring = factory_->createLinearRing(ring_coords);
  auto pt_on_ring = ReadWkt("POINT (5 0)");
  auto pt_inside_ring = ReadWkt("POINT (5 2)");
  EXPECT_EQ(ComputeIM(ring.get(), pt_on_ring.get())->toString(), "0F1FFFFF2");
  EXPECT_EQ(ComputeIM(ring.get(), pt_inside_ring.get())->toString(),
            "FF1FFF0F2");
}

TEST_F(RelateComputerTest, PointAndMultiPointAgainstPolygon) {
  const std::string poly = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))";
  // Inside polygon, on vertex, on edge interior, outside triangle in envelope.
  ExpectRelate("POINT (5 5)", poly, "0FFFFF212");
  ExpectRelate(poly, "POINT (5 5)", "0F2FF1FF2");
  ExpectRelate("POINT (0 0)", poly, "F0FFFF212");
  ExpectRelate(poly, "POINT (0 0)", "FF20F1FF2");
  ExpectRelate("POINT (5 0)", poly, "F0FFFF212");
  ExpectRelate(poly, "POINT (5 0)", "FF20F1FF2");

  const std::string poly_with_hole =
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))";
  ExpectRelate("POINT (5 5)", poly_with_hole, "FF0FFF212");
  ExpectRelate("POINT (8 8)", "POLYGON ((0 0, 10 0, 0 10, 0 0))", "FF0FFF212");

  ExpectRelate("MULTIPOINT ((5 5), (0 0), (15 15))", poly, "000FFF212");
  ExpectRelate(poly, "MULTIPOINT ((5 5), (0 0), (15 15))", "0F20F10F2");
  ExpectRelate("MULTIPOINT ((5 5), (2 2), (15 15))", poly_with_hole,
               "F00FFF212");
  ExpectRelate("MULTIPOINT ((1 1), (2 2), (5 5))", poly_with_hole, "000FFF212");
}

TEST_F(RelateComputerTest, LineAgainstLineInteractionsAndProperIntersections) {
  // Proper interior crossing (dimA == 1 && dimB == 1, hasProperInterior ==
  // true).
  ExpectRelate("LINESTRING (0 5, 10 5)", "LINESTRING (5 0, 5 10)", "0F1FF0102");

  // Endpoint touch, T-junctions, equality, partial overlap, containment.
  ExpectRelate("LINESTRING (0 0, 5 5)", "LINESTRING (5 5, 10 0)", "FF1F00102");
  ExpectRelate("LINESTRING (0 5, 5 5)", "LINESTRING (5 0, 5 10)", "FF10F0102");
  ExpectRelate("LINESTRING (5 0, 5 10)", "LINESTRING (0 5, 5 5)", "F01FF0102");
  ExpectRelate("LINESTRING (0 0, 10 0)", "LINESTRING (10 0, 0 0)", "1FFF0FFF2");
  ExpectRelate("LINESTRING (0 0, 6 0)", "LINESTRING (4 0, 10 0)", "1010F0102");
  ExpectRelate("LINESTRING (2 0, 8 0)", "LINESTRING (0 0, 10 0)", "1FF0FF102");

  // Disjoint lines with overlapping envelopes (isolated edges on both sides).
  ExpectRelate("LINESTRING (0 0, 10 10)", "LINESTRING (0 9, 1 10)",
               "FF1FF0102");

  // MultiLineString where one edge intersects and another edge is isolated.
  // B's endpoints (2, 0) and (8, 0) lie in the interior of A's bottom edge.
  ExpectRelate("MULTILINESTRING ((0 0, 10 0), (0 8, 10 8))",
               "LINESTRING (2 0, 8 0)", "101FF0FF2");

  // Multiple edges hitting the same improper intersection node (5, 5)
  // (computeIntersectionNodes visits an already-labelled node on the second
  // edge for both argIndex == 0 and argIndex == 1).
  ExpectRelate("MULTILINESTRING ((0 5, 10 5), (5 0, 5 10))",
               "LINESTRING (0 0, 5 5, 10 10)", "0F1FF0102");
  ExpectRelate("LINESTRING (0 0, 5 5, 10 10)",
               "MULTILINESTRING ((0 5, 10 5), (5 0, 5 10))", "0F1FF0102");
  ExpectRelate("LINESTRING (0 0, 10 10, 0 10, 10 0)", "LINESTRING (0 5, 10 5)",
               "0F1FF0102");

  // Proper intersection occurring at a boundary node of A:
  // hasProper is true while hasProperInterior is false for dimA=1, dimB=1.
  ExpectRelate("MULTILINESTRING ((5 5, 8 8), (2 5, 8 5))",
               "LINESTRING (5 0, 5 10)", "FF10F0102");
  ExpectRelate("LINESTRING (5 0, 5 10)",
               "MULTILINESTRING ((5 5, 8 8), (2 5, 8 5))", "F01FF0102");

  // Both graphs have intersecting and isolated edges, plus repeated vertices.
  ExpectRelate("MULTILINESTRING ((0 0, 0 0, 10 0), (0 2, 10 2))",
               "MULTILINESTRING ((5 -1, 5 1), (0 8, 10 8))", "0F1FF0102");

  // Self-intersecting bow-tie LineString vs point at self-crossing node.
  const std::string bowtie = "LINESTRING (0 0, 10 10, 10 0, 0 10)";
  ExpectRelate(bowtie, "POINT (5 5)", "0F1FF0FF2");
  ExpectRelate("POINT (5 5)", bowtie, "0FFFFF102");
}

TEST_F(RelateComputerTest, LineAgainstLineRespectsBoundaryNodeRules) {
  const std::string ring = "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)";
  const std::string spur = "LINESTRING (0 0, -5 0)";
  ExpectRelate(ring, spur, "F01FFF102",
               BoundaryNodeRule::getBoundaryRuleMod2());
  ExpectRelate(ring, spur, "FF1F0F102",
               BoundaryNodeRule::getBoundaryEndPoint());

  // 2-edge junction has valence 2 at (5, 0) and valence 1 at outer endpoints:
  // Mod2 and Monovalent treat (5, 0) as interior, EndPoint treats it as
  // boundary, and GeometryGraph::insertBoundaryPoint leaves all endpoints as
  // interior under Multivalent.
  const std::string two_edge_net = "MULTILINESTRING ((0 0, 5 0), (5 0, 10 0))";
  ExpectRelate("POINT (5 0)", two_edge_net, "0FFFFF102",
               BoundaryNodeRule::getBoundaryRuleMod2());
  ExpectRelate("POINT (5 0)", two_edge_net, "F0FFFF102",
               BoundaryNodeRule::getBoundaryEndPoint());
  ExpectRelate("POINT (5 0)", two_edge_net, "0FFFFF102",
               BoundaryNodeRule::getBoundaryMonovalentEndPoint());
  ExpectRelate("POINT (5 0)", two_edge_net, "0FFFFF1F2",
               BoundaryNodeRule::getBoundaryMultivalentEndPoint());

  // 3-way junction has valence 3 at (5, 0): boundary under Mod2 and EndPoint;
  // GeometryGraph::insertBoundaryPoint toggles BOUNDARY/INTERIOR across the
  // 3 incident endpoints under Monovalent and keeps INTERIOR under Multivalent.
  const std::string y_net =
      "MULTILINESTRING ((0 0, 5 0), (5 0, 10 0), (5 0, 5 5))";
  ExpectRelate("POINT (5 0)", y_net, "F0FFFF102",
               BoundaryNodeRule::getBoundaryRuleMod2());
  ExpectRelate("POINT (5 0)", y_net, "F0FFFF102",
               BoundaryNodeRule::getBoundaryEndPoint());
  ExpectRelate("POINT (5 0)", y_net, "0FFFFF1F2",
               BoundaryNodeRule::getBoundaryMultivalentEndPoint());
  ExpectRelate("POINT (5 0)", y_net, "F0FFFF102",
               BoundaryNodeRule::getBoundaryMonovalentEndPoint());
}

TEST_F(RelateComputerTest, LineAgainstPolygonAndPolygonAgainstLine) {
  const std::string square = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))";

  // Proper interior intersection (both dimA=1,dimB=2 and dimA=2,dimB=1).
  ExpectRelate("LINESTRING (-2 5, 12 5)", square, "101FF0212");
  ExpectRelate(square, "LINESTRING (-2 5, 12 5)", "1F20F1102");

  // Line wholly inside polygon (isolated edges on both graphs).
  ExpectRelate("LINESTRING (2 5, 8 5)", square, "1FF0FF212");
  ExpectRelate(square, "LINESTRING (2 5, 8 5)", "102FF1FF2");

  // Disjoint line inside polygon envelope.
  ExpectRelate("LINESTRING (6 6, 9 9)", "POLYGON ((0 0, 10 0, 0 10, 0 0))",
               "FF1FF0212");

  // Touching boundary at vertex, edge interior, and collinear boundary overlap.
  ExpectRelate("LINESTRING (-5 0, 0 0)", square, "FF1F00212");
  ExpectRelate("LINESTRING (5 -5, 5 0)", square, "FF1F00212");
  ExpectRelate("LINESTRING (2 0, 8 0)", square, "F1FF0F212");

  // Line crossing polygon with hole, chord across hole, and along hole edge.
  const std::string with_hole =
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (3 3, 7 3, 7 7, 3 7, 3 3))";
  ExpectRelate("LINESTRING (-2 5, 12 5)", with_hole, "101FF0212");
  ExpectRelate(with_hole, "LINESTRING (3 5, 7 5)", "FF2F011F2");
  ExpectRelate("LINESTRING (3 5, 7 5)", with_hole, "FF1F0F212");
  ExpectRelate(with_hole, "LINESTRING (3 3, 7 3)", "FF2101FF2");
  ExpectRelate("LINESTRING (3 3, 7 3)", with_hole, "F1FF0F212");

  // Proper intersection occurring at a boundary node (hasProper == true,
  // hasProperInterior == false) for both (dimA=2, dimB=1) and (dimA=1, dimB=2).
  const std::string half_square = "POLYGON ((0 0, 5 0, 5 10, 0 10, 0 0))";
  const std::string boundary_proper_line =
      "MULTILINESTRING ((5 5, 8 8), (2 5, 8 5))";
  ExpectRelate(boundary_proper_line, half_square, "101000212");
  ExpectRelate(half_square, boundary_proper_line, "102001102");
}

TEST_F(RelateComputerTest, PolygonAgainstPolygonAndMultiPolygon) {
  // Overlapping polygons with proper edge crossings (dimA=2, dimB=2,
  // hasProper).
  ExpectRelate("POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0))",
               "POLYGON ((3 3, 9 3, 9 9, 3 9, 3 3))", "212101212");

  // Overlapping polygons sharing collinear boundary segments without proper
  // segment crossings (hasProper == false; BB == 1).
  ExpectRelate("POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0))",
               "POLYGON ((2 0, 8 0, 8 6, 2 6, 2 0))", "212111212");

  // Touching at a vertex or along a shared edge.
  ExpectRelate("POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0))",
               "POLYGON ((6 6, 10 6, 10 10, 6 10, 6 6))", "FF2F01212");
  ExpectRelate("POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0))",
               "POLYGON ((6 0, 10 0, 10 6, 6 6, 6 0))", "FF2F11212");

  // Strict containment (isolated edges for both polygons).
  const std::string outer = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))";
  const std::string inner = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))";
  ExpectRelate(outer, inner, "212FF1FF2");
  ExpectRelate(inner, outer, "2FF1FF212");

  // Equal polygons (same and opposite ring orientations).
  ExpectRelate(outer, "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))", "2FFF1FFF2");

  // Polygon with hole (and reversed ring orientation) vs polygon inside hole
  // or filling hole (inner's entire boundary lies on donut's hole boundary,
  // so Exterior(donut) & Boundary(inner) is empty).
  const std::string donut =
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))";
  ExpectRelate(donut, "POLYGON ((3 3, 7 3, 7 7, 3 7, 3 3))", "FF2FF1212");
  ExpectRelate(
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))",
      "POLYGON ((3 3, 7 3, 7 7, 3 7, 3 3))", "FF2FF1212");
  ExpectRelate(donut, inner, "FF2F112F2");
  ExpectRelate(inner, donut, "FF2F1F212");

  // Disjoint polygons with overlapping envelopes.
  ExpectRelate("POLYGON ((0 0, 10 0, 0 10, 0 0))",
               "POLYGON ((6 6, 10 6, 6 10, 6 6))", "FF2FF1212");

  // MultiPolygon with isolated and containing shells, and vertex-touching
  // shells.
  ExpectRelate(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((6 6, 10 6, 10 10, 6 10, 6 6)))",
      "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))", "212FF1FF2");
  ExpectRelate(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((20 20, 22 20, 22 22, 20 22, 20 20)))",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (1 1, 3 1, 3 3, 1 3, 1 1))",
      "212111212");
  ExpectRelate(
      "MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "((5 5, 10 5, 10 10, 5 10, 5 5)))",
      "LINESTRING (0 10, 10 0)", "FF20F1102");
}

TEST_F(RelateComputerTest, GeometryCollectionsAnd3DCoordinates) {
  ExpectRelate("GEOMETRYCOLLECTION (POINT (15 15), LINESTRING (0 5, 10 5))",
               "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", "1F0F0F212");
  ExpectRelate("GEOMETRYCOLLECTION (POINT (5 5), LINESTRING (0 0, 10 0))",
               "POINT (5 5)", "0F1FF0FF2");
  ExpectRelate(
      "GEOMETRYCOLLECTION (GEOMETRYCOLLECTION (POINT (5 5)), "
      "LINESTRING (0 0, 10 0))",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", "01FF0F212");

  // Verify 3D coordinates compute topological DE-9IM cleanly.
  ExpectRelate("LINESTRING Z (0 5 1, 10 5 9)",
               "POLYGON Z ((0 0 2, 10 0 4, 10 10 6, 0 10 8, 0 0 2))",
               "1FFF0F212");
  ExpectRelate("LINESTRING Z (-2 5 1, 12 5 9)",
               "POLYGON Z ((0 0 2, 10 0 4, 10 10 6, 0 10 8, 0 0 2))",
               "101FF0212");
}

TEST_F(RelateComputerTest, IntersectionMatrixPredicatesFromComputedResults) {
  auto poly_a = ReadWkt("POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0))");
  auto poly_b = ReadWkt("POLYGON ((3 3, 9 3, 9 9, 3 9, 3 3))");
  auto im_overlap = ComputeIM(poly_a.get(), poly_b.get());
  EXPECT_FALSE(im_overlap->isDisjoint());
  EXPECT_TRUE(im_overlap->isIntersects());
  EXPECT_TRUE(im_overlap->isOverlaps(+Dimension::A, +Dimension::A));
  EXPECT_FALSE(im_overlap->isWithin());
  EXPECT_FALSE(im_overlap->isContains());
  EXPECT_FALSE(im_overlap->isEquals(+Dimension::A, +Dimension::A));

  auto inner = ReadWkt("POLYGON ((1 1, 5 1, 5 5, 1 5, 1 1))");
  auto im_contains = ComputeIM(poly_a.get(), inner.get());
  EXPECT_TRUE(im_contains->isContains());
  EXPECT_TRUE(im_contains->isCovers());
  EXPECT_FALSE(im_contains->isWithin());

  auto im_within = ComputeIM(inner.get(), poly_a.get());
  EXPECT_TRUE(im_within->isWithin());
  EXPECT_TRUE(im_within->isCoveredBy());
  EXPECT_FALSE(im_within->isContains());

  auto line_a = ReadWkt("LINESTRING (0 5, 10 5)");
  auto line_b = ReadWkt("LINESTRING (5 0, 5 10)");
  auto im_cross = ComputeIM(line_a.get(), line_b.get());
  EXPECT_TRUE(im_cross->isCrosses(+Dimension::L, +Dimension::L));
  EXPECT_FALSE(im_cross->isTouches(+Dimension::L, +Dimension::L));
  EXPECT_EQ(im_cross->get(Location::INTERIOR, Location::INTERIOR),
            +Dimension::P);
  EXPECT_EQ(im_cross->get(Location::EXTERIOR, Location::EXTERIOR),
            +Dimension::A);
}

}  // namespace
}  // namespace geos_google
