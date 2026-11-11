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

// Unit tests for third_party/geos/src/geom/prep/PreparedPolygonCovers.cpp and
// third_party/geos/include/geos/geom/prep/PreparedPolygonCovers.h.

#include "third_party/geos/include/geos/geom/prep/PreparedPolygonCovers.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometry.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/PreparedPolygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::prep::PreparedGeometry;
using geos::geom::prep::PreparedGeometryFactory;
using geos::geom::prep::PreparedPolygon;
using geos::geom::prep::PreparedPolygonCovers;
using geos::io::WKTReader;

struct CoversCase {
  std::string_view label;
  std::string_view test_wkt;
  bool expected_covers;
};

void VerifyPreparedCoversMatchesDirect(const Geometry* target,
                                       const PreparedPolygon& prep_poly,
                                       const PreparedGeometry& factory_prep,
                                       const CoversCase& tc,
                                       const WKTReader& reader,
                                       bool also_call_direct_helper) {
  SCOPED_TRACE(std::string(tc.label));
  std::unique_ptr<Geometry> test_geom = reader.read(std::string(tc.test_wkt));
  ASSERT_NE(test_geom, nullptr);

  EXPECT_EQ(target->covers(test_geom.get()), tc.expected_covers);
  EXPECT_EQ(prep_poly.covers(test_geom.get()), tc.expected_covers);
  EXPECT_EQ(factory_prep.covers(test_geom.get()), tc.expected_covers);

  if (also_call_direct_helper) {
    EXPECT_EQ(PreparedPolygonCovers::covers(&prep_poly, test_geom.get()),
              tc.expected_covers);
    PreparedPolygonCovers covers_op(&prep_poly);
    EXPECT_EQ(covers_op.covers(test_geom.get()), tc.expected_covers);
  }
}

TEST(PreparedPolygonCoversTest, NonRectangularPolygonTrueAndFalseCases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Concave L-shape ensures PreparedPolygon does not take the isRectangle
  // fast path and exercises PreparedPolygonCovers::fullTopologicalPredicate.
  std::unique_ptr<Geometry> target =
      reader.read("POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))");
  ASSERT_FALSE(target->isRectangle());

  PreparedPolygon prep_poly(target.get());
  std::unique_ptr<PreparedGeometry> factory_prep =
      PreparedGeometryFactory::prepare(target.get());

  const std::vector<CoversCase> cases = {
      // Puntal geometries: boundary and interior are both covered.
      {"point_interior", "POINT (2 2)", true},
      {"point_on_vertex", "POINT (0 0)", true},
      {"point_on_edge", "POINT (5 0)", true},
      {"point_in_concave_notch", "POINT (8 8)", false},
      {"point_exterior", "POINT (-2 5)", false},
      {"multipoint_all_boundary", "MULTIPOINT ((0 0), (10 0), (4 4), (0 10))",
       true},
      {"multipoint_interior_and_boundary", "MULTIPOINT ((2 2), (4 4), (2 8))",
       true},
      {"multipoint_one_in_notch", "MULTIPOINT ((2 2), (8 8))", false},

      // Lineal geometries reaching fullTopologicalPredicate (non-proper
      // boundary/vertex intersections).
      {"line_collinear_bottom_edge", "LINESTRING (0 0, 10 0)", true},
      {"line_collinear_bottom_edge_z", "LINESTRING Z (0 0 5, 10 0 15)", true},
      {"line_collinear_subset_edge", "LINESTRING (2 0, 8 0)", true},
      {"line_collinear_notch_edge", "LINESTRING (4 4, 10 4)", true},
      {"line_vertex_to_interior_to_vertex", "LINESTRING (0 0, 2 2, 4 4)", true},
      {"line_interior_touching_notch_vertex", "LINESTRING (2 2, 4 4, 2 8)",
       true},
      {"multiline_interior_and_collinear",
       "MULTILINESTRING ((1 1, 2 2), (0 0, 0 10))", true},
      {"line_through_notch_via_vertices", "LINESTRING (0 0, 4 4, 10 10)",
       false},
      {"line_interior_touching_vertex_into_notch",
       "LINESTRING (2 2, 10 4, 8 8)", false},
      {"line_strictly_interior", "LINESTRING (1 1, 3 3, 2 8)", true},
      {"line_proper_cross_into_notch", "LINESTRING (2 2, 8 8)", false},

      // Areal geometries reaching fullTopologicalPredicate.
      {"poly_sharing_edges_inside", "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
       true},
      {"poly_self_identical",
       "POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))", true},
      {"poly_strictly_interior", "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))", true},
      {"poly_same_envelope_collinear_extends_into_notch",
       "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", false},
      {"poly_enclosing_target", "POLYGON ((-2 -2, 12 -2, 12 12, -2 12, -2 -2))",
       false},
      {"multipoly_covered_and_boundary_touch",
       "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
       "((1 5, 3 5, 3 8, 1 8, 1 5)))",
       true},
      {"multipoly_one_part_outside",
       "MULTIPOLYGON (((1 1, 3 1, 3 3, 1 3, 1 1)), "
       "((6 6, 9 6, 9 9, 6 9, 6 6)))",
       false},

      // Mixed GeometryCollection cases.
      {"gc_covered_point_line_poly",
       "GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (4 4, 10 4), "
       "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1)))",
       true},
      {"gc_uncovered_notch_line",
       "GEOMETRYCOLLECTION (POINT (2 2), LINESTRING (0 0, 4 4, 10 10))", false},
  };

  for (const CoversCase& tc : cases) {
    VerifyPreparedCoversMatchesDirect(target.get(), prep_poly, *factory_prep,
                                      tc, reader,
                                      /*also_call_direct_helper=*/true);
  }

  // Verify covers returns true on boundary geometries where contains is false.
  std::unique_ptr<Geometry> boundary_pt = reader.read("POINT (0 0)");
  std::unique_ptr<Geometry> boundary_line =
      reader.read("LINESTRING (0 0, 10 0)");
  EXPECT_FALSE(prep_poly.contains(boundary_pt.get()));
  EXPECT_TRUE(prep_poly.covers(boundary_pt.get()));
  EXPECT_FALSE(prep_poly.contains(boundary_line.get()));
  EXPECT_TRUE(prep_poly.covers(boundary_line.get()));
}

TEST(PreparedPolygonCoversTest,
     DirectHelperCoversLinesExtendingOutsideEnvelope) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<Geometry> target =
      reader.read("POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))");
  PreparedPolygon prep_poly(target.get());

  // Calling PreparedPolygonCovers directly skips PreparedPolygon's envelope
  // check and exercises fullTopologicalPredicate returning false when a line
  // starts on/inside target and exits outside target's envelope via a vertex.
  const std::vector<CoversCase> cases = {
      {"line_collinear_past_end", "LINESTRING (0 0, 12 0)", false},
      {"line_interior_vertex_past_envelope", "LINESTRING (2 2, 10 4, 12 6)",
       false},
      {"line_disjoint_outside", "LINESTRING (20 20, 30 30)", false},
      {"poly_disjoint_outside", "POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20))",
       false},
  };

  for (const CoversCase& tc : cases) {
    SCOPED_TRACE(std::string(tc.label));
    std::unique_ptr<Geometry> test_geom = reader.read(std::string(tc.test_wkt));
    EXPECT_EQ(target->covers(test_geom.get()), tc.expected_covers);
    EXPECT_EQ(prep_poly.covers(test_geom.get()), tc.expected_covers);
    EXPECT_EQ(PreparedPolygonCovers::covers(&prep_poly, test_geom.get()),
              tc.expected_covers);
    PreparedPolygonCovers covers_op(&prep_poly);
    EXPECT_EQ(covers_op.covers(test_geom.get()), tc.expected_covers);
  }
}

TEST(PreparedPolygonCoversTest, PolygonWithHoleCases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<Geometry> target = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(3 3, 7 3, 7 7, 3 7, 3 3))");
  PreparedPolygon prep_poly(target.get());
  std::unique_ptr<PreparedGeometry> factory_prep =
      PreparedGeometryFactory::prepare(target.get());

  const std::vector<CoversCase> cases = {
      {"point_on_hole_boundary", "POINT (3 5)", true},
      {"point_inside_hole", "POINT (5 5)", false},
      {"line_along_hole_boundary", "LINESTRING (3 3, 7 3)", true},
      {"line_around_hole_corner_on_boundary", "LINESTRING (3 3, 7 3, 7 7)",
       true},
      {"line_touching_hole_vertex_into_hole", "LINESTRING (2 2, 3 3, 5 5)",
       false},
      {"line_touching_hole_vertex_and_crossing_hole",
       "LINESTRING (2 2, 3 3, 8 5)", false},
      {"line_crossing_hole_properly", "LINESTRING (2 5, 8 5)", false},
      {"poly_in_solid_margin", "POLYGON ((1 1, 2 1, 2 9, 1 9, 1 1))", true},
      {"poly_covering_target_hole", "POLYGON ((1 1, 9 1, 9 9, 1 9, 1 1))",
       false},
  };

  for (const CoversCase& tc : cases) {
    VerifyPreparedCoversMatchesDirect(target.get(), prep_poly, *factory_prep,
                                      tc, reader,
                                      /*also_call_direct_helper=*/true);
  }
}

TEST(PreparedPolygonCoversTest, MultiPolygonCases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Two valid polygons touching at a single shared vertex (5, 5).
  std::unique_ptr<Geometry> touching_multi = reader.read(
      "MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "((5 5, 10 5, 10 10, 5 10, 5 5)))");
  PreparedPolygon prep_touching(touching_multi.get());
  std::unique_ptr<PreparedGeometry> factory_touching =
      PreparedGeometryFactory::prepare(touching_multi.get());

  const std::vector<CoversCase> touching_cases = {
      {"line_through_shared_vertex", "LINESTRING (2 2, 5 5, 8 8)", true},
      {"line_through_exterior_quadrant", "LINESTRING (2 2, 5 5, 8 2)", false},
      {"poly_single_component_covered", "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))",
       true},
      {"multipoly_both_components_covered",
       "MULTIPOLYGON (((1 1, 4 1, 4 4, 1 4, 1 1)), "
       "((6 6, 9 6, 9 9, 6 9, 6 6)))",
       true},
  };

  for (const CoversCase& tc : touching_cases) {
    VerifyPreparedCoversMatchesDirect(touching_multi.get(), prep_touching,
                                      *factory_touching, tc, reader,
                                      /*also_call_direct_helper=*/true);
  }

  // Two disjoint polygons with a gap between x=4 and x=6.
  std::unique_ptr<Geometry> disjoint_multi = reader.read(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((6 0, 10 0, 10 4, 6 4, 6 0)))");
  PreparedPolygon prep_disjoint(disjoint_multi.get());
  std::unique_ptr<PreparedGeometry> factory_disjoint =
      PreparedGeometryFactory::prepare(disjoint_multi.get());

  const std::vector<CoversCase> disjoint_cases = {
      {"line_spanning_gap", "LINESTRING (2 2, 4 2, 6 2, 8 2)", false},
      {"line_collinear_first_shell", "LINESTRING (0 0, 4 0)", true},
      {"poly_bridging_gap", "POLYGON ((1 1, 9 1, 9 3, 1 3, 1 1))", false},
  };

  for (const CoversCase& tc : disjoint_cases) {
    VerifyPreparedCoversMatchesDirect(disjoint_multi.get(), prep_disjoint,
                                      *factory_disjoint, tc, reader,
                                      /*also_call_direct_helper=*/true);
  }
}

TEST(PreparedPolygonCoversTest, DirectHelperOnRectangleAndInstanceReuse) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<Geometry> rect =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_TRUE(rect->isRectangle());
  PreparedPolygon prep_rect(rect.get());

  // Reuse one PreparedPolygonCovers instance across multiple queries on a
  // rectangle (PreparedPolygon::covers short-circuits rectangles, while direct
  // PreparedPolygonCovers exercises full evaluation).
  PreparedPolygonCovers covers_op(&prep_rect);

  std::unique_ptr<Geometry> edge = reader.read("LINESTRING (0 0, 10 0)");
  std::unique_ptr<Geometry> crossing = reader.read("LINESTRING (5 5, 15 5)");
  std::unique_ptr<Geometry> inside_poly =
      reader.read("POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))");
  std::unique_ptr<Geometry> pt_outside = reader.read("POINT (11 5)");
  std::unique_ptr<Geometry> pt_boundary = reader.read("POINT (10 10)");

  EXPECT_TRUE(covers_op.covers(edge.get()));
  EXPECT_FALSE(covers_op.covers(crossing.get()));
  EXPECT_TRUE(covers_op.covers(inside_poly.get()));
  EXPECT_FALSE(covers_op.covers(pt_outside.get()));
  EXPECT_TRUE(covers_op.covers(pt_boundary.get()));

  EXPECT_TRUE(PreparedPolygonCovers::covers(&prep_rect, edge.get()));
  EXPECT_FALSE(PreparedPolygonCovers::covers(&prep_rect, crossing.get()));
}

TEST(PreparedPolygonCoversTest, EmptyTargetAndEmptyTestGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<Geometry> target =
      reader.read("POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))");
  PreparedPolygon prep_poly(target.get());
  std::unique_ptr<PreparedGeometry> factory_prep =
      PreparedGeometryFactory::prepare(target.get());

  const std::vector<CoversCase> empty_tests = {
      {"empty_point", "POINT EMPTY", false},
      {"empty_multipoint", "MULTIPOINT EMPTY", false},
      {"empty_linestring", "LINESTRING EMPTY", false},
      {"empty_polygon", "POLYGON EMPTY", false},
      {"empty_gc", "GEOMETRYCOLLECTION EMPTY", false},
  };

  for (const CoversCase& tc : empty_tests) {
    VerifyPreparedCoversMatchesDirect(target.get(), prep_poly, *factory_prep,
                                      tc, reader,
                                      /*also_call_direct_helper=*/false);
  }

  std::unique_ptr<Geometry> empty_poly = reader.read("POLYGON EMPTY");
  PreparedPolygon prep_empty(empty_poly.get());
  std::unique_ptr<PreparedGeometry> factory_empty =
      PreparedGeometryFactory::prepare(empty_poly.get());

  const std::vector<CoversCase> vs_empty_target = {
      {"nonempty_point_vs_empty_poly", "POINT (1 1)", false},
      {"nonempty_line_vs_empty_poly", "LINESTRING (0 0, 1 1)", false},
      {"empty_point_vs_empty_poly", "POINT EMPTY", false},
      {"empty_poly_vs_empty_poly", "POLYGON EMPTY", false},
  };

  for (const CoversCase& tc : vs_empty_target) {
    VerifyPreparedCoversMatchesDirect(empty_poly.get(), prep_empty,
                                      *factory_empty, tc, reader,
                                      /*also_call_direct_helper=*/false);
  }
  // Verify direct PreparedPolygonCovers returns false for non-empty geometries
  // against an empty target polygon.
  std::unique_ptr<Geometry> non_empty_pt = reader.read("POINT (1 1)");
  std::unique_ptr<Geometry> non_empty_line =
      reader.read("LINESTRING (0 0, 1 1)");
  EXPECT_FALSE(PreparedPolygonCovers::covers(&prep_empty, non_empty_pt.get()));
  EXPECT_FALSE(
      PreparedPolygonCovers::covers(&prep_empty, non_empty_line.get()));
}

}  // namespace
}  // namespace geos_google
