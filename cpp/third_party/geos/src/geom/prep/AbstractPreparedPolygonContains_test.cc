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

// Unit tests for
// third_party/geos/src/geom/prep/AbstractPreparedPolygonContains.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/prep/AbstractPreparedPolygonContains.cpp
//   https://github.com/libgeos/geos/blob/main/include/geos/geom/prep/AbstractPreparedPolygonContains.h

#include "third_party/geos/include/geos/geom/prep/AbstractPreparedPolygonContains.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/prep/PreparedPolygon.h"
#include "third_party/geos/include/geos/geom/prep/PreparedPolygonContains.h"
#include "third_party/geos/include/geos/geom/prep/PreparedPolygonCovers.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::prep::AbstractPreparedPolygonContains;
using geos::geom::prep::PreparedPolygon;
using geos::geom::prep::PreparedPolygonContains;
using geos::geom::prep::PreparedPolygonCovers;
using geos::io::WKTReader;

class RecordingPreparedPolygonContains final
    : public AbstractPreparedPolygonContains {
 public:
  explicit RecordingPreparedPolygonContains(const PreparedPolygon* prep_poly)
      : AbstractPreparedPolygonContains(prep_poly) {}

  RecordingPreparedPolygonContains(const PreparedPolygon* prep_poly,
                                   bool require_some_point_in_interior)
      : AbstractPreparedPolygonContains(prep_poly,
                                        require_some_point_in_interior) {}

  bool evaluate(const Geometry* geom) { return eval(geom); }

  bool evaluatePoint(const Geometry* geom, Location outermost_loc) {
    return evalPointTestGeom(geom, outermost_loc);
  }

  int full_predicate_calls() const { return full_predicate_calls_; }

 protected:
  bool fullTopologicalPredicate(const Geometry* geom) override {
    ++full_predicate_calls_;
    if (requireSomePointInInterior) {
      return prepPoly->getGeometry().contains(geom);
    }
    return prepPoly->getGeometry().covers(geom);
  }

 private:
  int full_predicate_calls_ = 0;
};

void ExpectContainsAndCovers(const PreparedPolygon& prep_poly,
                             const Geometry* test_geom, bool expected_contains,
                             bool expected_covers, int expected_full_calls) {
  SCOPED_TRACE(test_geom->toString());
  EXPECT_EQ(prep_poly.contains(test_geom), expected_contains);
  EXPECT_EQ(prep_poly.covers(test_geom), expected_covers);
  EXPECT_EQ(PreparedPolygonContains::contains(&prep_poly, test_geom),
            expected_contains);
  EXPECT_EQ(PreparedPolygonCovers::covers(&prep_poly, test_geom),
            expected_covers);

  RecordingPreparedPolygonContains rec_contains(&prep_poly);
  EXPECT_EQ(rec_contains.evaluate(test_geom), expected_contains);
  EXPECT_EQ(rec_contains.full_predicate_calls(), expected_full_calls);

  RecordingPreparedPolygonContains rec_covers(&prep_poly, false);
  EXPECT_EQ(rec_covers.evaluate(test_geom), expected_covers);
  EXPECT_EQ(rec_covers.full_predicate_calls(), expected_full_calls);
}

TEST(AbstractPreparedPolygonContainsTest,
     PuntalGeometriesAndEvalPointTestGeom) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Non-rectangular pentagon so PreparedPolygon takes PreparedPolygonContains /
  // PreparedPolygonCovers instead of rectangle fast paths.
  Geometry::Ptr target =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 5 12, 0 10, 0 0))");
  PreparedPolygon prep_poly(target.get());

  Geometry::Ptr interior_pt = reader.read("POINT (5 5)");
  Geometry::Ptr edge_pt = reader.read("POINT (5 0)");
  Geometry::Ptr vertex_pt = reader.read("POINT (0 0)");
  // Inside bounding box [0,10]x[0,12] but above edge (10,10)-(5,12).
  Geometry::Ptr env_exterior_pt = reader.read("POINT (9 11.5)");

  ExpectContainsAndCovers(prep_poly, interior_pt.get(), true, true, 0);
  ExpectContainsAndCovers(prep_poly, edge_pt.get(), false, true, 0);
  ExpectContainsAndCovers(prep_poly, vertex_pt.get(), false, true, 0);
  ExpectContainsAndCovers(prep_poly, env_exterior_pt.get(), false, false, 0);

  Geometry::Ptr mp_all_interior = reader.read("MULTIPOINT ((3 3), (7 7))");
  Geometry::Ptr mp_boundary_and_interior =
      reader.read("MULTIPOINT ((0 0), (5 5))");
  Geometry::Ptr mp_all_boundary =
      reader.read("MULTIPOINT ((0 0), (5 0), (10 10))");
  Geometry::Ptr mp_with_exterior = reader.read("MULTIPOINT ((5 5), (9 11.5))");

  ExpectContainsAndCovers(prep_poly, mp_all_interior.get(), true, true, 0);
  ExpectContainsAndCovers(prep_poly, mp_boundary_and_interior.get(), true, true,
                          0);
  ExpectContainsAndCovers(prep_poly, mp_all_boundary.get(), false, true, 0);
  ExpectContainsAndCovers(prep_poly, mp_with_exterior.get(), false, false, 0);

  Geometry::Ptr gc_points_mixed =
      reader.read("GEOMETRYCOLLECTION (POINT (0 0), POINT (5 5))");
  Geometry::Ptr gc_points_boundary =
      reader.read("GEOMETRYCOLLECTION (POINT (0 0), POINT (5 0))");
  ExpectContainsAndCovers(prep_poly, gc_points_mixed.get(), true, true, 0);
  ExpectContainsAndCovers(prep_poly, gc_points_boundary.get(), false, true, 0);

  // Direct evalPointTestGeom coverage including empty point (getNumPoints <=
  // 1).
  Geometry::Ptr empty_pt = reader.read("POINT EMPTY");
  EXPECT_FALSE(prep_poly.contains(empty_pt.get()));
  EXPECT_FALSE(prep_poly.covers(empty_pt.get()));

  RecordingPreparedPolygonContains direct_contains(&prep_poly, true);
  EXPECT_FALSE(direct_contains.evaluatePoint(empty_pt.get(), Location::NONE));
  EXPECT_FALSE(
      direct_contains.evaluatePoint(edge_pt.get(), Location::BOUNDARY));
  EXPECT_TRUE(
      direct_contains.evaluatePoint(interior_pt.get(), Location::INTERIOR));
  EXPECT_FALSE(
      direct_contains.evaluatePoint(env_exterior_pt.get(), Location::EXTERIOR));
  EXPECT_EQ(direct_contains.full_predicate_calls(), 0);

  RecordingPreparedPolygonContains direct_covers(&prep_poly, false);
  EXPECT_TRUE(direct_covers.evaluatePoint(edge_pt.get(), Location::BOUNDARY));
  EXPECT_FALSE(
      direct_covers.evaluatePoint(env_exterior_pt.get(), Location::EXTERIOR));
  EXPECT_EQ(direct_covers.full_predicate_calls(), 0);
}

TEST(AbstractPreparedPolygonContainsTest,
     OutermostComponentInExteriorShortCircuits) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Concave L-shape: notch [4,10]x[4,10] is inside envelope [0,10]x[0,10].
  Geometry::Ptr l_shape =
      reader.read("POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))");
  PreparedPolygon prep_l(l_shape.get());

  Geometry::Ptr line_in_notch = reader.read("LINESTRING (6 6, 8 8)");
  Geometry::Ptr mline_second_in_notch =
      reader.read("MULTILINESTRING ((1 1, 2 2), (6 6, 8 8))");
  Geometry::Ptr poly_in_notch =
      reader.read("POLYGON ((6 6, 8 6, 8 8, 6 8, 6 6))");

  ExpectContainsAndCovers(prep_l, line_in_notch.get(), false, false, 0);
  ExpectContainsAndCovers(prep_l, mline_second_in_notch.get(), false, false, 0);
  ExpectContainsAndCovers(prep_l, poly_in_notch.get(), false, false, 0);

  Geometry::Ptr holed = reader.read(
      "POLYGON ((0 0, 20 0, 20 20, 10 22, 0 20, 0 0), "
      "(8 8, 12 8, 12 12, 8 12, 8 8))");
  PreparedPolygon prep_holed(holed.get());
  Geometry::Ptr line_in_hole = reader.read("LINESTRING (9 9, 11 11)");
  ExpectContainsAndCovers(prep_holed, line_in_hole.get(), false, false, 0);
}

TEST(AbstractPreparedPolygonContainsTest,
     SingleShellProperIntersectionShortCircuits) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr pentagon =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 5 12, 0 10, 0 0))");
  PreparedPolygon prep_poly(pentagon.get());

  // Proper intersection only.
  Geometry::Ptr crossing_line = reader.read("LINESTRING (5 5, 9 11.5)");
  // Both non-proper (vertex (0,0)) and proper intersections; single shell still
  // short-circuits immediately on hasProperIntersection.
  Geometry::Ptr mixed_crossing_line =
      reader.read("LINESTRING (0 0, 5 5, 9 11.5)");
  Geometry::Ptr crossing_poly =
      reader.read("POLYGON ((4 4, 9 4, 9 11.5, 4 11.5, 4 4))");

  ExpectContainsAndCovers(prep_poly, crossing_line.get(), false, false, 0);
  ExpectContainsAndCovers(prep_poly, mixed_crossing_line.get(), false, false,
                          0);
  ExpectContainsAndCovers(prep_poly, crossing_poly.get(), false, false, 0);

  // Single-element MultiPolygon with no holes also satisfies isSingleShell().
  Geometry::Ptr single_mp =
      reader.read("MULTIPOLYGON (((0 0, 10 0, 10 10, 5 12, 0 10, 0 0)))");
  PreparedPolygon prep_single_mp(single_mp.get());
  ExpectContainsAndCovers(prep_single_mp, crossing_line.get(), false, false, 0);
}

TEST(AbstractPreparedPolygonContainsTest,
     HoledAndMultiShellProperIntersectionPaths) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr holed = reader.read(
      "POLYGON ((0 0, 20 0, 20 20, 10 22, 0 20, 0 0), "
      "(8 8, 12 8, 12 12, 8 12, 8 8))");
  PreparedPolygon prep_holed(holed.get());

  // Line crosses the hole edges properly with no non-proper intersections:
  // properIntersectionImpliesNotContained is false, so it short-circuits via
  // (hasSegmentIntersection && !hasNonProperIntersection).
  Geometry::Ptr line_crosses_hole = reader.read("LINESTRING (2 10, 18 10)");
  ExpectContainsAndCovers(prep_holed, line_crosses_hole.get(), false, false, 0);

  // Line touches hole corner (8,8) non-properly AND crosses edge x=12 properly:
  // both hasProperIntersection and hasNonProperIntersection are true with a
  // holed target, forcing fullTopologicalPredicate fallback.
  Geometry::Ptr line_mixed_hole = reader.read("LINESTRING (2 8, 8 8, 14 10)");
  ExpectContainsAndCovers(prep_holed, line_mixed_hole.get(), false, false, 1);

  // Polygonal test geometries still set properIntersectionImpliesNotContained
  // even when target has a hole.
  Geometry::Ptr poly_crosses_hole =
      reader.read("POLYGON ((6 6, 10 6, 10 10, 6 10, 6 6))");
  Geometry::Ptr mpoly_crosses_hole = reader.read(
      "MULTIPOLYGON (((6 6, 10 6, 10 10, 6 10, 6 6)), "
      "((1 1, 2 1, 2 2, 1 2, 1 1)))");
  ExpectContainsAndCovers(prep_holed, poly_crosses_hole.get(), false, false, 0);
  ExpectContainsAndCovers(prep_holed, mpoly_crosses_hole.get(), false, false,
                          0);

  // Single-element MultiPolygon with a hole -> isSingleShell is false.
  Geometry::Ptr single_mp_holed = reader.read(
      "MULTIPOLYGON (((0 0, 20 0, 20 20, 10 22, 0 20, 0 0), "
      "(8 8, 12 8, 12 12, 8 12, 8 8)))");
  PreparedPolygon prep_single_mp_holed(single_mp_holed.get());
  ExpectContainsAndCovers(prep_single_mp_holed, line_crosses_hole.get(), false,
                          false, 0);

  // Two-polygon MultiPolygon -> getNumGeometries() != 1 -> isSingleShell false.
  Geometry::Ptr two_polys = reader.read(
      "MULTIPOLYGON (((0 0, 8 0, 8 8, 0 8, 0 0)), "
      "((12 0, 20 0, 20 8, 12 8, 12 0)))");
  PreparedPolygon prep_two(two_polys.get());
  Geometry::Ptr line_spans_gap = reader.read("LINESTRING (4 4, 16 4)");
  ExpectContainsAndCovers(prep_two, line_spans_gap.get(), false, false, 0);
}

TEST(AbstractPreparedPolygonContainsTest,
     NonProperIntersectionInvokesFullTopologicalPredicate) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr pentagon =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 5 12, 0 10, 0 0))");
  PreparedPolygon prep_poly(pentagon.get());

  // Touches vertices (0,0) and (10,10); interior stays inside pentagon.
  Geometry::Ptr diag_line = reader.read("LINESTRING (0 0, 5 5, 10 10)");
  ExpectContainsAndCovers(prep_poly, diag_line.get(), true, true, 1);

  // Lies entirely along bottom boundary edge: covered, not contained.
  Geometry::Ptr boundary_line = reader.read("LINESTRING (2 0, 8 0)");
  ExpectContainsAndCovers(prep_poly, boundary_line.get(), false, true, 1);

  // Starts on boundary peak (5,12) and goes outside within envelope.
  Geometry::Ptr exterior_from_peak = reader.read("LINESTRING (5 12, 9 11.5)");
  ExpectContainsAndCovers(prep_poly, exterior_from_peak.get(), false, false, 1);

  // Polygon sharing boundary segment inside pentagon.
  Geometry::Ptr inner_edge_poly =
      reader.read("POLYGON ((2 0, 8 0, 8 5, 2 5, 2 0))");
  ExpectContainsAndCovers(prep_poly, inner_edge_poly.get(), true, true, 1);

  // Polygon touching only peak (5,12) outside pentagon within envelope.
  Geometry::Ptr outer_peak_poly =
      reader.read("POLYGON ((5 12, 9 11.5, 9 12, 5 12))");
  ExpectContainsAndCovers(prep_poly, outer_peak_poly.get(), false, false, 1);

  // GeometryCollection of dimension 1 touching vertices non-properly.
  Geometry::Ptr gc_line_and_pt = reader.read(
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 5 5, 10 10), POINT (2 2))");
  ExpectContainsAndCovers(prep_poly, gc_line_and_pt.get(), true, true, 1);

  // Two shells touching at a single vertex (8,8): a line crossing through (8,8)
  // remains wholly contained in the MultiPolygon.
  Geometry::Ptr touching_shells = reader.read(
      "MULTIPOLYGON (((0 0, 8 0, 8 8, 0 8, 0 0)), "
      "((8 8, 16 8, 16 16, 8 16, 8 8)))");
  PreparedPolygon prep_touching(touching_shells.get());
  Geometry::Ptr line_through_pinch =
      reader.read("LINESTRING (2 2, 8 8, 14 14)");
  ExpectContainsAndCovers(prep_touching, line_through_pinch.get(), true, true,
                          1);
}

TEST(AbstractPreparedPolygonContainsTest,
     NoSegmentIntersectionChecksTargetComponentsInAreaTest) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr holed = reader.read(
      "POLYGON ((0 0, 20 0, 20 20, 10 22, 0 20, 0 0), "
      "(8 8, 12 8, 12 12, 8 12, 8 8))");
  PreparedPolygon prep_holed(holed.get());

  // No boundary intersection, but test polygon/multipolygon encloses the hole.
  Geometry::Ptr encloses_hole =
      reader.read("POLYGON ((4 4, 16 4, 16 16, 4 16, 4 4))");
  Geometry::Ptr mp_encloses_hole = reader.read(
      "MULTIPOLYGON (((4 4, 16 4, 16 16, 4 16, 4 4)), "
      "((1 1, 2 1, 2 2, 1 2, 1 1)))");
  ExpectContainsAndCovers(prep_holed, encloses_hole.get(), false, false, 0);
  ExpectContainsAndCovers(prep_holed, mp_encloses_hole.get(), false, false, 0);

  // Annular test polygon whose own hole wraps around target's hole -> valid
  // containment with zero segment intersections.
  Geometry::Ptr annular_inside = reader.read(
      "POLYGON ((4 4, 16 4, 16 16, 4 16, 4 4), "
      "(6 6, 14 6, 14 14, 6 14, 6 6))");
  ExpectContainsAndCovers(prep_holed, annular_inside.get(), true, true, 0);

  // Disjoint from boundaries and not enclosing target hole.
  Geometry::Ptr corner_poly =
      reader.read("POLYGON ((1 1, 6 1, 6 6, 1 6, 1 1))");
  Geometry::Ptr corner_mpoly = reader.read(
      "MULTIPOLYGON (((1 1, 6 1, 6 6, 1 6, 1 1)), "
      "((14 1, 18 1, 18 6, 14 6, 14 1)))");
  Geometry::Ptr inner_line = reader.read("LINESTRING (1 1, 6 6)");
  Geometry::Ptr inner_gc =
      reader.read("GEOMETRYCOLLECTION (LINESTRING (1 1, 6 6), POINT (2 2))");

  ExpectContainsAndCovers(prep_holed, corner_poly.get(), true, true, 0);
  ExpectContainsAndCovers(prep_holed, corner_mpoly.get(), true, true, 0);
  ExpectContainsAndCovers(prep_holed, inner_line.get(), true, true, 0);
  ExpectContainsAndCovers(prep_holed, inner_gc.get(), true, true, 0);
}

}  // namespace
}  // namespace geos_google
