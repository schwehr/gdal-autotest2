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

#include "third_party/geos/include/geos/geom/HeuristicOverlay.h"

#include <cstddef>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"
#include "third_party/geos/include/geos/util/AssertionFailedException.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::HeuristicOverlay;
using geos::geom::PrecisionModel;
using geos::geom::StructuredCollection;
using geos::operation::overlayng::OverlayNG;

class HeuristicOverlayTest : public ::testing::Test {
 protected:
  HeuristicOverlayTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> Read(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  void ExpectNormalizedExactEquals(Geometry* actual,
                                   const std::string& expected_wkt,
                                   double tolerance = 1e-9) const {
    ASSERT_NE(actual, nullptr);
    auto expected = Read(expected_wkt);
    actual->normalize();
    expected->normalize();
    EXPECT_TRUE(actual->equalsExact(expected.get(), tolerance))
        << "Actual:   " << actual->toText()
        << "\nExpected: " << expected->toText();
  }

  void ExpectTopoEquals(const Geometry* actual,
                        const std::string& expected_wkt) const {
    ASSERT_NE(actual, nullptr);
    auto expected = Read(expected_wkt);
    EXPECT_TRUE(actual->equals(expected.get()))
        << "Actual:   " << actual->toText()
        << "\nExpected: " << expected->toText();
  }

  std::size_t CountGeometryType(const Geometry* geom,
                                geos::geom::GeometryTypeId type_id) const {
    std::size_t count = 0;
    for (std::size_t i = 0; i < geom->getNumGeometries(); ++i) {
      if (geom->getGeometryN(i)->getGeometryTypeId() == type_id) {
        ++count;
      }
    }
    return count;
  }

  GeometryFactory::Ptr factory_;
  geos::io::WKTReader reader_;
};

TEST_F(HeuristicOverlayTest, CombinableUnionDisjointAtomicsAndMultiGeometries) {
  auto p0 = Read("POINT (0 0)");
  auto p1 = Read("POINT (5 5)");
  auto pt_union = HeuristicOverlay(p0.get(), p1.get(), OverlayNG::UNION);
  EXPECT_EQ(pt_union->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(pt_union->getNumGeometries(), 2u);
  ExpectNormalizedExactEquals(pt_union.get(), "MULTIPOINT ((0 0), (5 5))");

  auto mp0 = Read(
      "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "((2 0, 3 0, 3 1, 2 1, 2 0)))");
  auto poly1 = Read("POLYGON ((10 10, 12 10, 12 12, 10 12, 10 10))");
  auto poly_union = HeuristicOverlay(mp0.get(), poly1.get(), OverlayNG::UNION);
  EXPECT_EQ(poly_union->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(poly_union->getNumGeometries(), 3u);
  EXPECT_DOUBLE_EQ(poly_union->getArea(), 6.0);
  ExpectTopoEquals(poly_union.get(),
                   "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
                   "((2 0, 3 0, 3 1, 2 1, 2 0)), "
                   "((10 10, 12 10, 12 12, 10 12, 10 10)))");

  // Disjoint MultiLineString with crossing segments + Point uses combineReduced
  // fast path, preserving un-noded segments from extractElements.
  auto crossing_mls = Read("MULTILINESTRING ((0 0, 2 2), (0 2, 2 0))");
  auto far_pt = Read("POINT (10 10)");
  auto mixed_union =
      HeuristicOverlay(crossing_mls.get(), far_pt.get(), OverlayNG::UNION);
  EXPECT_EQ(mixed_union->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(mixed_union->getNumGeometries(), 3u);
  EXPECT_EQ(mixed_union->getNumPoints(), 5u);
  ExpectNormalizedExactEquals(
      mixed_union.get(),
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 2 2), LINESTRING (0 2, 2 0), "
      "POINT (10 10))");
}

TEST_F(HeuristicOverlayTest, CombinableSymDifferenceDisjoint) {
  auto poly0 = Read("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  auto poly1 = Read("POLYGON ((10 10, 11 10, 11 11, 10 11, 10 10))");
  auto poly_symdiff =
      HeuristicOverlay(poly0.get(), poly1.get(), OverlayNG::SYMDIFFERENCE);
  EXPECT_EQ(poly_symdiff->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(poly_symdiff->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(poly_symdiff->getArea(), 2.0);
  ExpectTopoEquals(poly_symdiff.get(),
                   "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
                   "((10 10, 11 10, 11 11, 10 11, 10 10)))");

  auto pt = Read("POINT (0 0)");
  auto line = Read("LINESTRING (5 5, 6 6)");
  auto mixed_symdiff =
      HeuristicOverlay(pt.get(), line.get(), OverlayNG::SYMDIFFERENCE);
  EXPECT_EQ(mixed_symdiff->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(mixed_symdiff->getNumGeometries(), 2u);
  ExpectNormalizedExactEquals(
      mixed_symdiff.get(),
      "GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (5 5, 6 6))");
}

TEST_F(HeuristicOverlayTest, CombinableNestedCollectionsWithEmptyElements) {
  auto g0 = Read(
      "GEOMETRYCOLLECTION (POINT EMPTY, "
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)))");
  auto g1 = Read(
      "GEOMETRYCOLLECTION (LINESTRING EMPTY, "
      "GEOMETRYCOLLECTION (POINT (10 10)))");
  auto result = HeuristicOverlay(g0.get(), g1.get(), OverlayNG::UNION);
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(result->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(result->getArea(), 1.0);
  ExpectNormalizedExactEquals(result.get(),
                              "GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 "
                              "1, 0 0)), POINT (10 10))");
}

TEST_F(HeuristicOverlayTest, NotCombinableFallbacks) {
  // Both empty: bypasses combineReduced and uses OverlayNG return-type logic.
  auto pt_empty = Read("POINT EMPTY");
  auto poly_empty = Read("POLYGON EMPTY");
  auto both_empty =
      HeuristicOverlay(pt_empty.get(), poly_empty.get(), OverlayNG::UNION);
  EXPECT_TRUE(both_empty->isEmpty());
  EXPECT_EQ(both_empty->getGeometryTypeId(), geos::geom::GEOS_POLYGON);

  // One empty, one non-empty.
  auto pt = Read("POINT (1 1)");
  auto left_empty =
      HeuristicOverlay(pt_empty.get(), pt.get(), OverlayNG::UNION);
  ExpectNormalizedExactEquals(left_empty.get(), "POINT (1 1)");

  auto right_empty =
      HeuristicOverlay(pt.get(), pt_empty.get(), OverlayNG::UNION);
  ExpectNormalizedExactEquals(right_empty.get(), "POINT (1 1)");

  auto gc_empty = Read("GEOMETRYCOLLECTION EMPTY");
  auto gc_empty_union =
      HeuristicOverlay(gc_empty.get(), pt.get(), OverlayNG::UNION);
  ExpectNormalizedExactEquals(gc_empty_union.get(), "POINT (1 1)");

  // Disjoint GC containing two non-empty elements fails
  // hasSingleNonEmptyElement.
  auto two_pts_gc = Read("GEOMETRYCOLLECTION (POINT (0 0), POINT (1 1))");
  auto far_pt = Read("POINT (10 10)");
  auto two_pts_union =
      HeuristicOverlay(two_pts_gc.get(), far_pt.get(), OverlayNG::UNION);
  ExpectNormalizedExactEquals(two_pts_union.get(),
                              "MULTIPOINT ((0 0), (1 1), (10 10))");

  // Nested GC whose inner GC has two non-empty elements fails
  // hasSingleNonEmptyElement.
  auto nested_multi_gc = Read(
      "GEOMETRYCOLLECTION (GEOMETRYCOLLECTION (POINT (0 0), POINT (1 1)))");
  auto nested_union =
      HeuristicOverlay(nested_multi_gc.get(), far_pt.get(), OverlayNG::UNION);
  ExpectNormalizedExactEquals(nested_union.get(),
                              "MULTIPOINT ((0 0), (1 1), (10 10))");

  // Disjoint geometries with intersecting envelopes bypass combineReduced.
  auto diag0 = Read("LINESTRING (0 0, 2 2)");
  auto diag1 = Read("LINESTRING (0 1.5, 0.5 2)");
  auto diag_union =
      HeuristicOverlay(diag0.get(), diag1.get(), OverlayNG::UNION);
  EXPECT_EQ(diag_union->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  ExpectNormalizedExactEquals(diag_union.get(),
                              "MULTILINESTRING ((0 0, 2 2), (0 1.5, 0.5 2))");

  // Disjoint single elements with INTERSECTION / DIFFERENCE bypass
  // combineReduced.
  auto poly = Read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto far_line = Read("LINESTRING (5 5, 6 6)");
  auto inter =
      HeuristicOverlay(poly.get(), far_line.get(), OverlayNG::INTERSECTION);
  EXPECT_TRUE(inter->isEmpty());
  EXPECT_EQ(inter->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);

  auto diff =
      HeuristicOverlay(poly.get(), far_line.get(), OverlayNG::DIFFERENCE);
  ExpectTopoEquals(diff.get(), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
}

TEST_F(HeuristicOverlayTest,
       OverlayNGRobustStandardOperationsAndGeometryWrappers) {
  auto p0 = Read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto p1 = Read("POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))");

  auto u = HeuristicOverlay(p0.get(), p1.get(), OverlayNG::UNION);
  EXPECT_DOUBLE_EQ(u->getArea(), 6.0);
  ExpectTopoEquals(u.get(), "POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0))");

  auto i = HeuristicOverlay(p0.get(), p1.get(), OverlayNG::INTERSECTION);
  EXPECT_DOUBLE_EQ(i->getArea(), 2.0);
  ExpectTopoEquals(i.get(), "POLYGON ((1 0, 2 0, 2 2, 1 2, 1 0))");

  auto d = HeuristicOverlay(p0.get(), p1.get(), OverlayNG::DIFFERENCE);
  EXPECT_DOUBLE_EQ(d->getArea(), 2.0);
  ExpectTopoEquals(d.get(), "POLYGON ((0 0, 1 0, 1 2, 0 2, 0 0))");

  auto sd = HeuristicOverlay(p0.get(), p1.get(), OverlayNG::SYMDIFFERENCE);
  EXPECT_DOUBLE_EQ(sd->getArea(), 4.0);
  ExpectTopoEquals(sd.get(),
                   "MULTIPOLYGON (((0 0, 1 0, 1 2, 0 2, 0 0)), "
                   "((2 0, 3 0, 3 2, 2 2, 2 0)))");

  // Verify Geometry::* member methods dispatch identically.
  ExpectTopoEquals(p0->Union(p1.get()).get(),
                   "POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0))");
  ExpectTopoEquals(p0->intersection(p1.get()).get(),
                   "POLYGON ((1 0, 2 0, 2 2, 1 2, 1 0))");
  ExpectTopoEquals(p0->difference(p1.get()).get(),
                   "POLYGON ((0 0, 1 0, 1 2, 0 2, 0 0))");
  ExpectTopoEquals(p0->symDifference(p1.get()).get(),
                   "MULTIPOLYGON (((0 0, 1 0, 1 2, 0 2, 0 0)), "
                   "((2 0, 3 0, 3 2, 2 2, 2 0)))");

  // Homogeneous lineal and puntal GeometryCollections are handled by OverlayNG.
  auto line_gc =
      Read("GEOMETRYCOLLECTION (LINESTRING (0 0, 2 0), LINESTRING (1 0, 3 0))");
  auto cross_line = Read("LINESTRING (1 -1, 1 1)");
  auto line_inter = HeuristicOverlay(line_gc.get(), cross_line.get(),
                                     OverlayNG::INTERSECTION);
  ExpectNormalizedExactEquals(line_inter.get(), "POINT (1 0)");

  auto pt_gc = Read("GEOMETRYCOLLECTION (POINT (0 0), POINT (1 1))");
  auto pt = Read("POINT (1 1)");
  auto pt_diff = HeuristicOverlay(pt_gc.get(), pt.get(), OverlayNG::DIFFERENCE);
  ExpectNormalizedExactEquals(pt_diff.get(), "POINT (0 0)");

  // Empty mixed-dimension GC has isMixedDimension() == true and isEmpty() ==
  // true, so isHandledByOverlayNG returns true.
  auto empty_mixed = Read("GEOMETRYCOLLECTION (LINESTRING EMPTY, POINT EMPTY)");
  auto empty_mixed_union =
      HeuristicOverlay(empty_mixed.get(), empty_mixed.get(), OverlayNG::UNION);
  EXPECT_TRUE(empty_mixed_union->isEmpty());
  EXPECT_EQ(empty_mixed_union->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);

  // Fixed PrecisionModel factory.
  PrecisionModel fixed_pm(10.0);
  auto fixed_factory = GeometryFactory::create(&fixed_pm);
  geos::io::WKTReader fixed_reader(*fixed_factory);
  auto fp0 = fixed_reader.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto fp1 = fixed_reader.read("POLYGON ((1.1 0, 3.1 0, 3.1 2, 1.1 2, 1.1 0))");
  auto fixed_inter =
      HeuristicOverlay(fp0.get(), fp1.get(), OverlayNG::INTERSECTION);
  EXPECT_NEAR(fixed_inter->getArea(), 1.8, 1e-9);
}

TEST_F(HeuristicOverlayTest, HeuristicOverlayRoutesToStructuredCollection) {
  auto mixed_gc = Read(
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), POINT (5 5))");
  auto poly = Read("POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))");

  // g0 is mixed-dimension -> StructuredCollection.
  auto u_left = HeuristicOverlay(mixed_gc.get(), poly.get(), OverlayNG::UNION);
  EXPECT_EQ(u_left->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(u_left->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(u_left->getArea(), 6.0);
  ExpectTopoEquals(
      u_left.get(),
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0)), POINT (5 5))");

  // g0 is simple, g1 is mixed-dimension -> hits second branch of
  // !isHandledByOverlayNG(g0) || !isHandledByOverlayNG(g1).
  auto u_right = HeuristicOverlay(poly.get(), mixed_gc.get(), OverlayNG::UNION);
  EXPECT_EQ(u_right->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(u_right->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(u_right->getArea(), 6.0);
  ExpectTopoEquals(
      u_right.get(),
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0)), POINT (5 5))");

  // Homogeneous polygonal GeometryCollection (not mixed-dim, dim == 2) ->
  // isHandledByOverlayNG returns false and routes to StructuredCollection.
  auto poly_gc = Read(
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0)))");
  auto single_poly = Read("POLYGON ((2.5 0, 4 0, 4 2, 2.5 2, 2.5 0))");

  auto poly_gc_left =
      HeuristicOverlay(poly_gc.get(), single_poly.get(), OverlayNG::UNION);
  EXPECT_EQ(poly_gc_left->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(poly_gc_left->getArea(), 8.0);
  ExpectTopoEquals(poly_gc_left.get(), "POLYGON ((0 0, 4 0, 4 2, 0 2, 0 0))");

  auto poly_gc_right =
      HeuristicOverlay(single_poly.get(), poly_gc.get(), OverlayNG::UNION);
  EXPECT_EQ(poly_gc_right->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(poly_gc_right->getArea(), 8.0);
  ExpectTopoEquals(poly_gc_right.get(), "POLYGON ((0 0, 4 0, 4 2, 0 2, 0 0))");
}

TEST_F(HeuristicOverlayTest,
       StructuredCollectionUnionDeduplicatesCoveredComponents) {
  // g0 orders Point -> LineString -> Polygon (ascending dimension updates).
  // Includes nested collections, Multi* geometries, duplicate points,
  // points inside polygons/on lines, and lines partially inside polygons.
  auto g0 = Read(
      "GEOMETRYCOLLECTION ("
      "  POINT (1 1), "
      "  MULTIPOINT ((5 2), (10 10), (10 10)), "
      "  LINESTRING (1 1, 4 1), "
      "  MULTILINESTRING ((5 0, 5 4), (6 2, 8 2)), "
      "  POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "  GEOMETRYCOLLECTION (POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0)), "
      "                      POINT (2.5 1.5))"
      ")");

  // g1 orders Polygon -> LineString -> Point (descending order exercises
  // addDimension false branch).
  auto g1 = Read(
      "GEOMETRYCOLLECTION ("
      "  MULTIPOLYGON (((7 0, 9 0, 9 2, 7 2, 7 0))), "
      "  LINESTRING (5 2, 7 2), "
      "  POINT (8 1), "
      "  POINT (12 12)"
      ")");

  auto res =
      StructuredCollection::overlay(g0.get(), g1.get(), OverlayNG::UNION);
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  // Expect:
  // - Polygons: [0,3]x[0,2] (area 6) and [7,9]x[0,2] (area 4) -> total area 10.
  // - Lines:
  //   (1,1)-(4,1) clipped outside [0,3]x[0,2] -> (3,1)-(4,1) (len 1)
  //   (5,0)-(5,4) (len 4)
  //   (6,2)-(8,2) + (5,2)-(7,2) = (5,2)-(8,2), clipped outside [7,9]x[0,2] ->
  //   (5,2)-(7,2) (len 2, split at (5,2) -> (5,2)-(7,2), wait: (5,0)-(5,4)
  //   nodes at (5,2)!)
  // - Points: (1 1), (2.5 1.5), (8 1) are covered by polygons; (5 2) is on a
  //   line; only (10 10) and (12 12) remain!
  EXPECT_DOUBLE_EQ(res->getArea(), 10.0);
  EXPECT_DOUBLE_EQ(res->getLength(),
                   18.0 + 7.0);  // perimeters (10+8) + lines (1+4+2)
  ExpectTopoEquals(res.get(),
                   "GEOMETRYCOLLECTION ("
                   "  POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0)), "
                   "  POLYGON ((7 0, 9 0, 9 2, 7 2, 7 0)), "
                   "  LINESTRING (3 1, 4 1), "
                   "  LINESTRING (5 0, 5 4), "
                   "  LINESTRING (5 2, 7 2), "
                   "  POINT (10 10), "
                   "  POINT (12 12)"
                   ")");

  // Verify the covered points (1 1), (2.5 1.5), (5 2), (8 1) were stripped and
  // the overlapping polygons merged, leaving 2 polygons and 2 free points.
  EXPECT_EQ(CountGeometryType(res.get(), geos::geom::GEOS_POLYGON), 2u);
  EXPECT_EQ(CountGeometryType(res.get(), geos::geom::GEOS_POINT), 2u);
  EXPECT_GE(CountGeometryType(res.get(), geos::geom::GEOS_LINESTRING), 3u);
}

TEST_F(HeuristicOverlayTest,
       StructuredCollectionIntersectionAllPairsAndDeduplication) {
  // Crafted so every one of the 9 component-pair intersections in
  // StructuredCollection::doIntersection contributes, plus a point (3 3)
  // that intersects P1 and is later stripped by doUnaryUnion because it falls
  // inside P0 & P1.
  auto g0 = Read(
      "GEOMETRYCOLLECTION ("
      "  POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "  LINESTRING (0 6, 4 6), "
      "  LINESTRING (8 0, 8 4), "
      "  LINESTRING (10 0, 10 4), "
      "  POINT (5 5), "
      "  POINT (9 2), "
      "  POINT (12 12), "
      "  POINT (3 3)"
      ")");

  auto g1 = Read(
      "GEOMETRYCOLLECTION ("
      "  POLYGON ((2 2, 6 2, 6 8, 2 8, 2 2)), "
      "  LINESTRING (0 1, 5 1), "
      "  LINESTRING (7 2, 9 2), "
      "  LINESTRING (10 1, 10 3), "
      "  POINT (0.5 0.5), "
      "  POINT (8 3), "
      "  POINT (12 12)"
      ")");

  auto res = StructuredCollection::overlay(g0.get(), g1.get(),
                                           OverlayNG::INTERSECTION);
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  // Surviving components:
  // - Polygon: [2,4]x[2,4] (area 4, perimeter 8)
  // - Lines: (0 1, 4 1) [len 4], (2 6, 4 6) [len 2], (10 1, 10 3) [len 2]
  // - Points: (0.5 0.5), (5 5), (8 2), (8 3), (9 2), (12 12) [6 points]
  // Note: (3 3) was stripped by doUnaryUnion.
  EXPECT_DOUBLE_EQ(res->getArea(), 4.0);
  EXPECT_DOUBLE_EQ(res->getLength(), 8.0 + 8.0);
  EXPECT_EQ(res->getNumGeometries(), 10u);
  ExpectNormalizedExactEquals(res.get(),
                              "GEOMETRYCOLLECTION ("
                              "  POLYGON ((2 2, 4 2, 4 4, 2 4, 2 2)), "
                              "  LINESTRING (0 1, 4 1), "
                              "  LINESTRING (2 6, 4 6), "
                              "  LINESTRING (10 1, 10 3), "
                              "  POINT (0.5 0.5), "
                              "  POINT (5 5), "
                              "  POINT (8 2), "
                              "  POINT (8 3), "
                              "  POINT (9 2), "
                              "  POINT (12 12)"
                              ")");

  // Also exercise via HeuristicOverlay.
  auto res_h = HeuristicOverlay(g0.get(), g1.get(), OverlayNG::INTERSECTION);
  EXPECT_EQ(res_h->getNumGeometries(), 10u);
  ExpectTopoEquals(res_h.get(), res->toText());
}

TEST_F(HeuristicOverlayTest, StructuredCollectionDifference) {
  auto g0 = Read(
      "GEOMETRYCOLLECTION ("
      "  POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "  LINESTRING (4 2, 8 2), "
      "  LINESTRING (10 0, 10 4), "
      "  POINT (5 2), "
      "  POINT (7 2), "
      "  POINT (10 0.5), "
      "  POINT (12 0), "
      "  POINT (12 5)"
      ")");

  auto g1 = Read(
      "GEOMETRYCOLLECTION ("
      "  POLYGON ((2 0, 6 0, 6 4, 2 4, 2 0)), "
      "  LINESTRING (10 1, 10 3), "
      "  POINT (12 0)"
      ")");

  auto res =
      StructuredCollection::overlay(g0.get(), g1.get(), OverlayNG::DIFFERENCE);
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  // Surviving components:
  // - Polygon: [0,2]x[0,4] (area 8, perimeter 12)
  // - Lines: (6 2, 8 2) [len 2], (10 0, 10 1) [len 1], (10 3, 10 4) [len 1]
  // - Points: (5 2) removed by g1 poly; (7 2) & (10 0.5) removed by
  //   line_diff_poly_line; (12 0) removed by g1 point; (12 5) survives.
  EXPECT_DOUBLE_EQ(res->getArea(), 8.0);
  EXPECT_DOUBLE_EQ(res->getLength(), 12.0 + 4.0);
  EXPECT_EQ(res->getNumGeometries(), 5u);
  ExpectNormalizedExactEquals(res.get(),
                              "GEOMETRYCOLLECTION ("
                              "  POLYGON ((0 0, 2 0, 2 4, 0 4, 0 0)), "
                              "  LINESTRING (6 2, 8 2), "
                              "  LINESTRING (10 0, 10 1), "
                              "  LINESTRING (10 3, 10 4), "
                              "  POINT (12 5)"
                              ")");

  auto res_h = HeuristicOverlay(g0.get(), g1.get(), OverlayNG::DIFFERENCE);
  EXPECT_EQ(res_h->getNumGeometries(), 5u);
  ExpectTopoEquals(res_h.get(), res->toText());
}

TEST_F(HeuristicOverlayTest, StructuredCollectionSymDifference) {
  auto g0 = Read(
      "GEOMETRYCOLLECTION ("
      "  POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "  LINESTRING (5 0, 5 4), "
      "  POINT (2.5 1), "
      "  POINT (7 7), "
      "  POINT (8 8)"
      ")");
  auto g1 = Read(
      "GEOMETRYCOLLECTION ("
      "  POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0)), "
      "  LINESTRING (5 1, 5 3), "
      "  POINT (7 7)"
      ")");

  // Envelopes intersect, so HeuristicOverlay routes to StructuredCollection.
  auto res = HeuristicOverlay(g0.get(), g1.get(), OverlayNG::SYMDIFFERENCE);
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  // - Polygons: symmetric difference [0,1]x[0,2] and [2,3]x[0,2] (area 4,
  //   perimeter 12).
  // - Lines: doSymDifference applies DIFFERENCE on lines -> (5 0, 5 1) and
  //   (5 3, 5 4) (len 2).
  // - Points: doSymDifference applies DIFFERENCE on points -> (7 7) removed,
  //   (2.5 1) removed by doUnaryUnion inside [2,3]x[0,2], (8 8) survives.
  EXPECT_DOUBLE_EQ(res->getArea(), 4.0);
  EXPECT_DOUBLE_EQ(res->getLength(), 12.0 + 2.0);
  EXPECT_EQ(res->getNumGeometries(), 5u);
  ExpectNormalizedExactEquals(res.get(),
                              "GEOMETRYCOLLECTION ("
                              "  POLYGON ((0 0, 1 0, 1 2, 0 2, 0 0)), "
                              "  POLYGON ((2 0, 3 0, 3 2, 2 2, 2 0)), "
                              "  LINESTRING (5 0, 5 1), "
                              "  LINESTRING (5 3, 5 4), "
                              "  POINT (8 8)"
                              ")");
}

TEST_F(HeuristicOverlayTest, StructuredCollectionEmptyResultsAcrossDimensions) {
  // resultDim = 2 -> POLYGON EMPTY
  auto poly_gc0 = Read(
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "POINT (0.5 0.5))");
  auto poly_gc1 = Read(
      "GEOMETRYCOLLECTION (POLYGON ((10 10, 11 10, 11 11, 10 11, 10 10)), "
      "POINT (10.5 10.5))");
  auto empty_poly = StructuredCollection::overlay(
      poly_gc0.get(), poly_gc1.get(), OverlayNG::INTERSECTION);
  EXPECT_TRUE(empty_poly->isEmpty());
  EXPECT_EQ(empty_poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);

  // resultDim = 1 -> LINESTRING EMPTY
  auto line_gc = Read(
      "GEOMETRYCOLLECTION (LINESTRING (0.2 0.2, 0.8 0.8), POINT (0.5 0.5))");
  auto cover_gc = Read(
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "POINT (5 5))");
  auto empty_line =
      HeuristicOverlay(line_gc.get(), cover_gc.get(), OverlayNG::DIFFERENCE);
  EXPECT_TRUE(empty_line->isEmpty());
  EXPECT_EQ(empty_line->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);

  // resultDim = 0 -> POINT EMPTY
  auto covered_pt = Read("POINT (0.5 0.5)");
  auto empty_pt =
      HeuristicOverlay(covered_pt.get(), cover_gc.get(), OverlayNG::DIFFERENCE);
  EXPECT_TRUE(empty_pt->isEmpty());
  EXPECT_EQ(empty_pt->getGeometryTypeId(), geos::geom::GEOS_POINT);

  // Identical collection SYMDIFFERENCE -> POLYGON EMPTY
  auto self_symdiff = StructuredCollection::overlay(
      poly_gc0.get(), poly_gc0.get(), OverlayNG::SYMDIFFERENCE);
  EXPECT_TRUE(self_symdiff->isEmpty());
  EXPECT_EQ(self_symdiff->getGeometryTypeId(), geos::geom::GEOS_POLYGON);

  // Completely empty GeometryCollections in StructuredCollection::overlay leave
  // dimension at DONTCARE (-3), triggering createEmptyResult assertion.
  auto empty_gc = Read("GEOMETRYCOLLECTION EMPTY");
  EXPECT_THROW(StructuredCollection::overlay(empty_gc.get(), empty_gc.get(),
                                             OverlayNG::UNION),
               geos::util::AssertionFailedException);
}

TEST_F(HeuristicOverlayTest, ErrorPathsInvalidOpCodeAndUnexpectedTypes) {
  auto mixed_gc = Read(
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), POINT (5 5))");
  auto pt = Read("POINT (1 1)");

  EXPECT_THROW(StructuredCollection::overlay(mixed_gc.get(), pt.get(), 99),
               geos::util::IllegalArgumentException);
  EXPECT_THROW(HeuristicOverlay(mixed_gc.get(), pt.get(), 99),
               geos::util::IllegalArgumentException);

  // LinearRing is non-empty, not a collection, and not Point/LineString/Polygon
  // -> throws IllegalArgumentException in StructuredCollection::readCollection.
  auto ring = Read("LINEARRING (0 0, 1 0, 1 1, 0 0)");
  EXPECT_THROW(
      StructuredCollection::overlay(ring.get(), pt.get(), OverlayNG::UNION),
      geos::util::IllegalArgumentException);

  auto gc_with_ring =
      Read("GEOMETRYCOLLECTION (LINEARRING (0 0, 1 0, 1 1, 0 0), POINT (2 2))");
  EXPECT_THROW(HeuristicOverlay(gc_with_ring.get(), pt.get(), OverlayNG::UNION),
               geos::util::IllegalArgumentException);

  // CircularString also hits unexpected type in readCollection, and throws
  // UnsupportedOperationException when routed directly to OverlayNGRobust.
  auto circ = Read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(
      StructuredCollection::overlay(circ.get(), pt.get(), OverlayNG::UNION),
      geos::util::IllegalArgumentException);
  EXPECT_THROW(HeuristicOverlay(circ.get(), pt.get(), OverlayNG::INTERSECTION),
               geos::util::UnsupportedOperationException);
}

TEST_F(HeuristicOverlayTest, ThreeDimensionalAndEmptyAtomicsInCollections) {
  auto g0 = Read(
      "GEOMETRYCOLLECTION ("
      "  POLYGON Z ((0 0 1, 2 0 1, 2 2 1, 0 2 1, 0 0 1)), "
      "  POINT Z (5 5 10), "
      "  POINT EMPTY"
      ")");
  auto g1 = Read(
      "GEOMETRYCOLLECTION ("
      "  POLYGON Z ((1 0 2, 3 0 2, 3 2 2, 1 2 2, 1 0 2)), "
      "  LINESTRING EMPTY"
      ")");

  auto u = HeuristicOverlay(g0.get(), g1.get(), OverlayNG::UNION);
  EXPECT_EQ(u->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(u->hasZ());
  EXPECT_EQ(u->getCoordinateDimension(), 3u);
  EXPECT_DOUBLE_EQ(u->getArea(), 6.0);
  EXPECT_EQ(u->getNumGeometries(), 2u);
  ExpectTopoEquals(u.get(),
                   "GEOMETRYCOLLECTION ("
                   "  POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0)), "
                   "  POINT (5 5)"
                   ")");

  // Subtract g0 from itself via StructuredCollection to cover resultCoordDim
  // calculation on empty result with 3D inputs.
  auto diff_empty =
      StructuredCollection::overlay(g0.get(), g0.get(), OverlayNG::DIFFERENCE);
  EXPECT_TRUE(diff_empty->isEmpty());
  EXPECT_EQ(diff_empty->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
}

}  // namespace
}  // namespace geos_google
