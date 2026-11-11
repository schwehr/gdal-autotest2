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

#include "third_party/geos/include/geos/operation/relateng/RelatePointLocator.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relateng/DimensionLocation.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Location;
using geos::io::WKTReader;
using geos::operation::relateng::DimensionLocation;
using geos::operation::relateng::RelatePointLocator;

class RelatePointLocatorTest : public ::testing::Test {
 protected:
  RelatePointLocatorTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(RelatePointLocatorTest, EmptyGeometriesReturnExterior) {
  const std::vector<std::string> empty_wkts = {
      "POINT EMPTY",        "LINESTRING EMPTY",         "LINEARRING EMPTY",
      "POLYGON EMPTY",      "MULTIPOINT EMPTY",         "MULTILINESTRING EMPTY",
      "MULTIPOLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY",
  };
  const CoordinateXY probe(0.0, 0.0);

  for (const std::string& wkt : empty_wkts) {
    auto geom = ReadWkt(wkt);
    RelatePointLocator locator(geom.get());
    EXPECT_EQ(locator.locate(&probe), Location::EXTERIOR) << wkt;
    EXPECT_EQ(locator.locateWithDim(&probe), +DimensionLocation::EXTERIOR)
        << wkt;
    EXPECT_EQ(locator.locateNode(&probe, nullptr), Location::EXTERIOR) << wkt;
    EXPECT_EQ(locator.locateNodeWithDim(&probe, nullptr),
              +DimensionLocation::EXTERIOR)
        << wkt;
  }
}

TEST_F(RelatePointLocatorTest, CollectionContainingEmptyAndNonEmptyElements) {
  auto geom = ReadWkt(
      "GEOMETRYCOLLECTION (POINT EMPTY, LINESTRING EMPTY, POLYGON EMPTY, "
      "POINT (3 4))");
  RelatePointLocator locator(geom.get());

  const CoordinateXY on_pt(3.0, 4.0);
  const CoordinateXY off_pt(0.0, 0.0);
  EXPECT_EQ(locator.locate(&on_pt), Location::INTERIOR);
  EXPECT_EQ(locator.locateWithDim(&on_pt), +DimensionLocation::POINT_INTERIOR);
  EXPECT_EQ(locator.locate(&off_pt), Location::EXTERIOR);
  EXPECT_EQ(locator.locateWithDim(&off_pt), +DimensionLocation::EXTERIOR);
}

TEST_F(RelatePointLocatorTest, PointAndMultiPointLocation) {
  auto pt = ReadWkt("POINT (2 5)");
  RelatePointLocator pt_locator(pt.get());
  const CoordinateXY hit(2.0, 5.0);
  const CoordinateXY miss(2.0, 6.0);
  EXPECT_EQ(pt_locator.locate(&hit), Location::INTERIOR);
  EXPECT_EQ(pt_locator.locateWithDim(&hit), +DimensionLocation::POINT_INTERIOR);
  EXPECT_EQ(pt_locator.locateNode(&hit, nullptr), Location::INTERIOR);
  EXPECT_EQ(pt_locator.locateNodeWithDim(&hit, nullptr),
            +DimensionLocation::POINT_INTERIOR);
  EXPECT_EQ(pt_locator.locate(&miss), Location::EXTERIOR);
  EXPECT_EQ(pt_locator.locateWithDim(&miss), +DimensionLocation::EXTERIOR);

  auto mpt = ReadWkt("MULTIPOINT ((0 0), (5 5), (10 10))");
  RelatePointLocator mpt_locator(mpt.get());
  const CoordinateXY p0(0.0, 0.0);
  const CoordinateXY p1(5.0, 5.0);
  const CoordinateXY p2(10.0, 10.0);
  const CoordinateXY between(5.0, 0.0);
  EXPECT_EQ(mpt_locator.locateWithDim(&p0), +DimensionLocation::POINT_INTERIOR);
  EXPECT_EQ(mpt_locator.locateWithDim(&p1), +DimensionLocation::POINT_INTERIOR);
  EXPECT_EQ(mpt_locator.locateWithDim(&p2), +DimensionLocation::POINT_INTERIOR);
  EXPECT_EQ(mpt_locator.locateWithDim(&between), +DimensionLocation::EXTERIOR);
}

TEST_F(RelatePointLocatorTest, OpenLineStringLocationAndNodeShortCircuit) {
  auto line = ReadWkt("LINESTRING (0 0, 10 10, 20 0)");
  RelatePointLocator locator(line.get());
  EXPECT_TRUE(locator.hasBoundary());

  const CoordinateXY start(0.0, 0.0);
  const CoordinateXY mid_seg(5.0, 5.0);
  const CoordinateXY middle_vertex(10.0, 10.0);
  const CoordinateXY end(20.0, 0.0);
  const CoordinateXY in_bbox_off_line(10.0, 2.0);
  const CoordinateXY out_of_bbox(30.0, 30.0);

  EXPECT_EQ(locator.locate(&start), Location::BOUNDARY);
  EXPECT_EQ(locator.locateWithDim(&start), +DimensionLocation::LINE_BOUNDARY);
  EXPECT_EQ(locator.locateLineEndWithDim(&start),
            +DimensionLocation::LINE_BOUNDARY);
  EXPECT_EQ(locator.locateNode(&start, nullptr), Location::BOUNDARY);
  EXPECT_EQ(locator.locateNodeWithDim(&start, nullptr),
            +DimensionLocation::LINE_BOUNDARY);

  EXPECT_EQ(locator.locate(&end), Location::BOUNDARY);
  EXPECT_EQ(locator.locateLineEndWithDim(&end),
            +DimensionLocation::LINE_BOUNDARY);

  EXPECT_EQ(locator.locate(&mid_seg), Location::INTERIOR);
  EXPECT_EQ(locator.locateWithDim(&mid_seg), +DimensionLocation::LINE_INTERIOR);
  EXPECT_EQ(locator.locate(&middle_vertex), Location::INTERIOR);
  EXPECT_EQ(locator.locateWithDim(&middle_vertex),
            +DimensionLocation::LINE_INTERIOR);

  // When isNode is true and point is not a boundary, locateOnLines returns
  // INTERIOR without scanning every segment.
  EXPECT_EQ(locator.locateNode(&mid_seg, nullptr), Location::INTERIOR);
  EXPECT_EQ(locator.locateNodeWithDim(&mid_seg, nullptr),
            +DimensionLocation::LINE_INTERIOR);

  EXPECT_EQ(locator.locate(&in_bbox_off_line), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&out_of_bbox), Location::EXTERIOR);
}

TEST_F(RelatePointLocatorTest, LinearRingRespectsBoundaryNodeRules) {
  CoordinateSequence seq = {
      CoordinateXY(0.0, 0.0),   CoordinateXY(10.0, 0.0),
      CoordinateXY(10.0, 10.0), CoordinateXY(0.0, 10.0),
      CoordinateXY(0.0, 0.0),
  };
  std::unique_ptr<LinearRing> ring = factory_->createLinearRing(seq);
  const CoordinateXY endpoint(0.0, 0.0);
  const CoordinateXY vertex(10.0, 0.0);
  const CoordinateXY interior_pt(5.0, 5.0);

  RelatePointLocator mod2_locator(ring.get(), false,
                                  BoundaryNodeRule::getBoundaryRuleMod2());
  EXPECT_FALSE(mod2_locator.hasBoundary());
  EXPECT_EQ(mod2_locator.locateWithDim(&endpoint),
            +DimensionLocation::LINE_INTERIOR);
  EXPECT_EQ(mod2_locator.locateLineEndWithDim(&endpoint),
            +DimensionLocation::LINE_INTERIOR);
  EXPECT_EQ(mod2_locator.locateWithDim(&vertex),
            +DimensionLocation::LINE_INTERIOR);
  EXPECT_EQ(mod2_locator.locateWithDim(&interior_pt),
            +DimensionLocation::EXTERIOR);

  RelatePointLocator ep_locator(ring.get(), false,
                                BoundaryNodeRule::getBoundaryEndPoint());
  EXPECT_TRUE(ep_locator.hasBoundary());
  EXPECT_EQ(ep_locator.locateWithDim(&endpoint),
            +DimensionLocation::LINE_BOUNDARY);
  EXPECT_EQ(ep_locator.locateLineEndWithDim(&endpoint),
            +DimensionLocation::LINE_BOUNDARY);
  EXPECT_EQ(ep_locator.locateWithDim(&vertex),
            +DimensionLocation::LINE_INTERIOR);
}

TEST_F(RelatePointLocatorTest, MultiLineStringRulesAndComponentIteration) {
  auto loop = ReadWkt("MULTILINESTRING ((0 0, 10 0), (10 0, 0 10, 0 0))");
  const CoordinateXY joint(0.0, 0.0);
  const CoordinateXY second_line_mid(5.0, 5.0);

  RelatePointLocator mod2(loop.get(), false,
                          BoundaryNodeRule::getBoundaryRuleMod2());
  EXPECT_FALSE(mod2.hasBoundary());
  EXPECT_EQ(mod2.locateWithDim(&joint), +DimensionLocation::LINE_INTERIOR);
  // First line misses (5, 5), second line contains it.
  EXPECT_EQ(mod2.locateWithDim(&second_line_mid),
            +DimensionLocation::LINE_INTERIOR);

  RelatePointLocator multivalent(
      loop.get(), false, BoundaryNodeRule::getBoundaryMultivalentEndPoint());
  EXPECT_TRUE(multivalent.hasBoundary());
  EXPECT_EQ(multivalent.locateWithDim(&joint),
            +DimensionLocation::LINE_BOUNDARY);

  RelatePointLocator monovalent(
      loop.get(), false, BoundaryNodeRule::getBoundaryMonovalentEndPoint());
  EXPECT_FALSE(monovalent.hasBoundary());
  EXPECT_EQ(monovalent.locateWithDim(&joint),
            +DimensionLocation::LINE_INTERIOR);

  auto t_junction =
      ReadWkt("MULTILINESTRING ((0 0, 5 0), (5 0, 10 0), (5 0, 5 5))");
  RelatePointLocator t_mod2(t_junction.get());
  EXPECT_TRUE(t_mod2.hasBoundary());
  const CoordinateXY degree3(5.0, 0.0);
  const CoordinateXY degree1(5.0, 5.0);
  EXPECT_EQ(t_mod2.locateWithDim(&degree3), +DimensionLocation::LINE_BOUNDARY);
  EXPECT_EQ(t_mod2.locateWithDim(&degree1), +DimensionLocation::LINE_BOUNDARY);
}

TEST_F(RelatePointLocatorTest, PolygonAndMultiPolygonPreparedAndUnprepared) {
  auto poly = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(5 5, 15 5, 15 15, 5 15, 5 5))");
  auto mpoly = ReadWkt(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((20 0, 30 0, 30 10, 20 10, 20 0)))");

  const CoordinateXY poly_in(2.0, 2.0);
  const CoordinateXY poly_shell(0.0, 10.0);
  const CoordinateXY poly_hole_bdy(5.0, 10.0);
  const CoordinateXY poly_in_hole(10.0, 10.0);
  const CoordinateXY poly_out(25.0, 25.0);

  for (bool is_prepared : {false, true}) {
    RelatePointLocator poly_loc(poly.get(), is_prepared,
                                BoundaryNodeRule::getBoundaryRuleMod2());
    EXPECT_EQ(poly_loc.locateWithDim(&poly_in),
              +DimensionLocation::AREA_INTERIOR);
    // Repeated lookup exercises cached locator reuse in getLocator().
    EXPECT_EQ(poly_loc.locateWithDim(&poly_shell),
              +DimensionLocation::AREA_BOUNDARY);
    EXPECT_EQ(poly_loc.locate(&poly_hole_bdy), Location::BOUNDARY);
    EXPECT_EQ(poly_loc.locateWithDim(&poly_in_hole),
              +DimensionLocation::EXTERIOR);
    EXPECT_EQ(poly_loc.locate(&poly_out), Location::EXTERIOR);
    EXPECT_EQ(poly_loc.locateNode(&poly_shell, poly.get()), Location::BOUNDARY);
    EXPECT_EQ(poly_loc.locateNodeWithDim(&poly_hole_bdy, nullptr),
              +DimensionLocation::AREA_BOUNDARY);

    RelatePointLocator mpoly_loc(mpoly.get(), is_prepared,
                                 BoundaryNodeRule::getBoundaryRuleMod2());
    const CoordinateXY mp_in1(5.0, 5.0);
    const CoordinateXY mp_in2(25.0, 5.0);
    const CoordinateXY mp_bdy2(20.0, 5.0);
    const CoordinateXY mp_gap(15.0, 5.0);
    EXPECT_EQ(mpoly_loc.locateWithDim(&mp_in1),
              +DimensionLocation::AREA_INTERIOR);
    EXPECT_EQ(mpoly_loc.locateWithDim(&mp_in2),
              +DimensionLocation::AREA_INTERIOR);
    EXPECT_EQ(mpoly_loc.locateWithDim(&mp_bdy2),
              +DimensionLocation::AREA_BOUNDARY);
    EXPECT_EQ(mpoly_loc.locateWithDim(&mp_gap), +DimensionLocation::EXTERIOR);
    EXPECT_EQ(mpoly_loc.locateNodeWithDim(&mp_bdy2, mpoly.get()),
              +DimensionLocation::AREA_BOUNDARY);
  }
}

TEST_F(RelatePointLocatorTest, MixedGeometryCollectionPrecedenceAndLineEnds) {
  auto gc = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "MULTIPOLYGON (((20 0, 30 0, 30 10, 20 10, 20 0))), "
      "LINESTRING (5 5, 15 5), "
      "LINESTRING (0 5, -5 5), "
      "LINEARRING (40 0, 50 0, 50 10, 40 10, 40 0), "
      "POINT (5 5), "
      "POINT (12 5), "
      "POINT (15 5), "
      "POINT (60 60), "
      "GEOMETRYCOLLECTION (POINT (70 70)))");
  const auto* gc_ptr = dynamic_cast<const GeometryCollection*>(gc.get());
  ASSERT_NE(gc_ptr, nullptr);
  const Geometry* poly0 = gc_ptr->getGeometryN(0);

  RelatePointLocator locator(gc.get());
  EXPECT_TRUE(locator.hasBoundary());

  // Dimension precedence: Polygon > LineString > Point.
  const CoordinateXY in_poly_line_pt(5.0, 5.0);
  const CoordinateXY on_poly_bdy_and_line(10.0, 5.0);
  const CoordinateXY on_poly_bdy_line_end(0.0, 5.0);
  const CoordinateXY on_line_int_and_pt(12.0, 5.0);
  const CoordinateXY on_line_bdy_and_pt(15.0, 5.0);
  const CoordinateXY on_ring_start(40.0, 0.0);
  const CoordinateXY isolated_pt(60.0, 60.0);
  const CoordinateXY nested_pt(70.0, 70.0);
  const CoordinateXY outside(100.0, 100.0);

  EXPECT_EQ(locator.locateWithDim(&in_poly_line_pt),
            +DimensionLocation::AREA_INTERIOR);
  EXPECT_EQ(locator.locateWithDim(&on_poly_bdy_and_line),
            +DimensionLocation::AREA_BOUNDARY);
  EXPECT_EQ(locator.locateWithDim(&on_line_int_and_pt),
            +DimensionLocation::LINE_INTERIOR);
  EXPECT_EQ(locator.locateWithDim(&on_line_bdy_and_pt),
            +DimensionLocation::LINE_BOUNDARY);
  EXPECT_EQ(locator.locateWithDim(&on_ring_start),
            +DimensionLocation::LINE_INTERIOR);
  EXPECT_EQ(locator.locateWithDim(&isolated_pt),
            +DimensionLocation::POINT_INTERIOR);
  EXPECT_EQ(locator.locateWithDim(&nested_pt),
            +DimensionLocation::POINT_INTERIOR);
  EXPECT_EQ(locator.locateWithDim(&outside), +DimensionLocation::EXTERIOR);

  // Line end locations in a mixed collection.
  EXPECT_EQ(locator.locateLineEndWithDim(&in_poly_line_pt),
            +DimensionLocation::AREA_INTERIOR);
  EXPECT_EQ(locator.locateLineEndWithDim(&on_poly_bdy_line_end),
            +DimensionLocation::AREA_BOUNDARY);
  EXPECT_EQ(locator.locateLineEndWithDim(&on_line_bdy_and_pt),
            +DimensionLocation::LINE_BOUNDARY);
  EXPECT_EQ(locator.locateLineEndWithDim(&on_ring_start),
            +DimensionLocation::LINE_INTERIOR);

  // Node location in a mixed collection with parentPolygonal optimization.
  EXPECT_EQ(locator.locateNodeWithDim(&in_poly_line_pt, nullptr),
            +DimensionLocation::AREA_INTERIOR);
  EXPECT_EQ(locator.locateNodeWithDim(&on_poly_bdy_line_end, poly0),
            +DimensionLocation::AREA_BOUNDARY);
  EXPECT_EQ(locator.locateNodeWithDim(&on_line_int_and_pt, nullptr),
            +DimensionLocation::LINE_INTERIOR);
}

TEST_F(RelatePointLocatorTest, MixedCollectionSingleOpenLineEndOutsideArea) {
  auto gc = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "LINESTRING (5 5, 20 5))");
  RelatePointLocator locator(gc.get());

  const CoordinateXY end_inside(5.0, 5.0);
  const CoordinateXY end_outside(20.0, 5.0);
  EXPECT_EQ(locator.locateLineEndWithDim(&end_inside),
            +DimensionLocation::AREA_INTERIOR);
  EXPECT_EQ(locator.locateLineEndWithDim(&end_outside),
            +DimensionLocation::LINE_BOUNDARY);
}

TEST_F(RelatePointLocatorTest, AdjacentAndOverlappingPolygonsUnionSemantics) {
  auto adj = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0)))");

  const CoordinateXY shared_edge_1(10.0, 4.0);
  const CoordinateXY shared_edge_2(10.0, 6.0);
  const CoordinateXY shared_corner(10.0, 0.0);
  const CoordinateXY outer_edge(0.0, 5.0);

  for (bool is_prepared : {false, true}) {
    RelatePointLocator locator(adj.get(), is_prepared,
                               BoundaryNodeRule::getBoundaryRuleMod2());
    // First shared-edge query initializes adjEdgeLocator; second reuses it.
    EXPECT_EQ(locator.locateWithDim(&shared_edge_1),
              +DimensionLocation::AREA_INTERIOR);
    EXPECT_EQ(locator.locateWithDim(&shared_edge_2),
              +DimensionLocation::AREA_INTERIOR);
    EXPECT_EQ(locator.locateWithDim(&shared_corner),
              +DimensionLocation::AREA_BOUNDARY);
    EXPECT_EQ(locator.locateWithDim(&outer_edge),
              +DimensionLocation::AREA_BOUNDARY);
  }

  // Three polygons where two share a boundary and the third contains that point
  // in its interior (must return AREA_INTERIOR via early exit in
  // locateOnPolygons).
  auto adj_plus_cover = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0)), "
      "POLYGON ((5 2, 15 2, 15 8, 5 8, 5 2)))");
  const auto* gc_ptr =
      dynamic_cast<const GeometryCollection*>(adj_plus_cover.get());
  ASSERT_NE(gc_ptr, nullptr);
  const Geometry* poly0 = gc_ptr->getGeometryN(0);

  RelatePointLocator overlap_loc(adj_plus_cover.get());
  const CoordinateXY covered_shared(10.0, 5.0);
  EXPECT_EQ(overlap_loc.locateWithDim(&covered_shared),
            +DimensionLocation::AREA_INTERIOR);
  EXPECT_EQ(overlap_loc.locateNodeWithDim(&covered_shared, poly0),
            +DimensionLocation::AREA_INTERIOR);

  // Polygon with a hole partially filled by an adjacent inner polygon.
  auto hole_filled = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(5 5, 15 5, 15 15, 5 15, 5 5)), "
      "POLYGON ((5 5, 10 5, 10 15, 5 15, 5 5)))");
  RelatePointLocator hole_loc(hole_filled.get());
  const CoordinateXY filled_hole_edge(5.0, 10.0);
  const CoordinateXY filled_hole_corner(5.0, 5.0);
  const CoordinateXY inner_poly_edge(10.0, 10.0);
  const CoordinateXY bottom_hole_corner(10.0, 5.0);

  EXPECT_EQ(hole_loc.locateWithDim(&filled_hole_edge),
            +DimensionLocation::AREA_INTERIOR);
  EXPECT_EQ(hole_loc.locateWithDim(&filled_hole_corner),
            +DimensionLocation::AREA_INTERIOR);
  EXPECT_EQ(hole_loc.locateWithDim(&inner_poly_edge),
            +DimensionLocation::AREA_BOUNDARY);
  EXPECT_EQ(hole_loc.locateWithDim(&bottom_hole_corner),
            +DimensionLocation::AREA_BOUNDARY);
}

}  // namespace
}  // namespace geos_google
