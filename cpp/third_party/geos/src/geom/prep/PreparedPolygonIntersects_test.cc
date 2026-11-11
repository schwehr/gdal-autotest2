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

// Unit tests for third_party/geos/src/geom/prep/PreparedPolygonIntersects.cpp
// and third_party/geos/include/geos/geom/prep/PreparedPolygonIntersects.h.

#include "third_party/geos/include/geos/geom/prep/PreparedPolygonIntersects.h"

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
using geos::geom::prep::PreparedPolygonIntersects;
using geos::io::WKTReader;

struct IntersectsCase {
  std::string_view label;
  std::string_view test_wkt;
  bool expected_intersects;
};

void VerifyIntersectsMatches(const Geometry* target,
                             const PreparedPolygon& prep_poly,
                             const PreparedGeometry& factory_prep,
                             const IntersectsCase& tc,
                             const WKTReader& reader) {
  SCOPED_TRACE(std::string(tc.label));
  std::unique_ptr<Geometry> test_geom = reader.read(std::string(tc.test_wkt));
  ASSERT_NE(test_geom, nullptr);

  EXPECT_EQ(target->intersects(test_geom.get()), tc.expected_intersects);
  EXPECT_EQ(prep_poly.intersects(test_geom.get()), tc.expected_intersects);
  EXPECT_EQ(factory_prep.intersects(test_geom.get()), tc.expected_intersects);
  EXPECT_EQ(PreparedPolygonIntersects::intersects(&prep_poly, test_geom.get()),
            tc.expected_intersects);

  PreparedPolygonIntersects op(&prep_poly);
  EXPECT_EQ(op.intersects(test_geom.get()), tc.expected_intersects);
}

TEST(PreparedPolygonIntersectsTest,
     NonRectangularPolygonWithHoleAllShortCircuitAndSegmentBranches) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Concave L-shape with a hole [1,3]x[1,3] ensures isRectangle() is false.
  std::unique_ptr<Geometry> target = reader.read(
      "POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0), "
      "(1 1, 3 1, 3 3, 1 3, 1 1))");
  ASSERT_FALSE(target->isRectangle());
  PreparedPolygon prep_poly(target.get());
  std::unique_ptr<PreparedGeometry> factory_prep =
      PreparedGeometryFactory::prepare(target.get());

  const std::vector<IntersectsCase> cases = {
      // Branch 1: isAnyTestComponentInTarget() == true -> returns true early.
      {"point_in_solid_interior", "POINT (0.5 0.5)", true},
      {"point_on_outer_edge", "POINT (5 0)", true},
      {"point_on_concave_vertex", "POINT (4 4)", true},
      {"point_on_hole_boundary", "POINT (1 2)", true},
      {"multipoint_first_outside_second_inside",
       "MULTIPOINT ((8 8), (0.5 0.5))", true},
      {"line_starting_in_interior", "LINESTRING (0.5 0.5, 12 12)", true},
      {"poly_first_vertex_in_interior",
       "POLYGON ((0.5 0.5, 20 0.5, 20 20, 0.5 20, 0.5 0.5))", true},
      {"gc_with_inside_point",
       "GEOMETRYCOLLECTION (POINT (8 8), POINT (0.5 0.5))", true},

      // Branch 2: isAnyTestComponentInTarget() == false && isPuntal() -> false.
      {"point_in_concave_notch", "POINT (8 8)", false},
      {"point_inside_hole", "POINT (2 2)", false},
      {"point_outside_envelope", "POINT (20 20)", false},
      {"multipoint_all_exterior_or_in_hole",
       "MULTIPOINT ((8 8), (2 2), (20 20))", false},
      {"point_empty", "POINT EMPTY", false},
      {"multipoint_empty", "MULTIPOINT EMPTY", false},
      {"gc_strictly_puntal_empty", "GEOMETRYCOLLECTION EMPTY", false},
      {"gc_strictly_puntal_outside",
       "GEOMETRYCOLLECTION (POINT (8 8), POINT (2 2))", false},

      // Branch 3: segment intersection finder returns true.
      {"line_crossing_shell_and_hole", "LINESTRING (-5 2, 15 2)", true},
      {"line_crossing_shell_z", "LINESTRING Z (-5 2 10, 15 2 20)", true},
      {"line_touching_outer_boundary_at_endpoint", "LINESTRING (-5 2, 0 2)",
       true},
      {"line_touching_corner_vertex_from_outside",
       "LINESTRING (-2 -2, 0 0, -2 2)", true},
      {"line_starting_in_hole_crossing_hole_edge", "LINESTRING (2 2, 5 2)",
       true},
      {"multiline_first_disjoint_second_crossing",
       "MULTILINESTRING ((8 8, 9 9), (-5 2, 15 2))", true},
      {"poly_first_vertex_in_notch_overlapping_arm",
       "POLYGON ((8 8, 12 8, 12 2, 8 2, 8 8))", true},
      {"poly_first_vertex_outside_touching_edge",
       "POLYGON ((12 0, 12 4, 10 4, 10 0, 12 0))", true},
      {"poly_enclosing_with_hole_crossing_target",
       "POLYGON ((-2 -2, 12 -2, 12 12, -2 12, -2 -2), "
       "(2 2, 5 2, 5 5, 2 5, 2 2))",
       true},
      {"gc_outside_point_and_crossing_line",
       "GEOMETRYCOLLECTION (POINT (8 8), LINESTRING (-5 2, 15 2))", true},

      // Branch 4: no segment intersections, dimension != 2 -> false.
      {"line_disjoint_in_notch", "LINESTRING (6 6, 9 9)", false},
      {"line_disjoint_outside_envelope", "LINESTRING (20 20, 30 30)", false},
      {"line_strictly_inside_hole", "LINESTRING (1.5 1.5, 2.5 2.5)", false},
      {"multiline_disjoint_and_inside_hole",
       "MULTILINESTRING ((6 6, 9 9), (1.5 1.5, 2.5 2.5))", false},
      {"linestring_empty", "LINESTRING EMPTY", false},
      {"multilinestring_empty", "MULTILINESTRING EMPTY", false},
  };

  for (const IntersectsCase& tc : cases) {
    VerifyIntersectsMatches(target.get(), prep_poly, *factory_prep, tc, reader);
  }
}

TEST(PreparedPolygonIntersectsTest,
     ArealTargetInclusionAndHoleExclusionBranches) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Non-rectangular polygon strictly inside a larger areal test geometry.
  {
    std::unique_ptr<Geometry> target =
        reader.read("POLYGON ((2 2, 8 2, 6 8, 2 7, 2 2))");
    PreparedPolygon prep_poly(target.get());
    std::unique_ptr<PreparedGeometry> factory_prep =
        PreparedGeometryFactory::prepare(target.get());

    const std::vector<IntersectsCase> cases = {
        {"enclosed_by_polygon", "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
         true},
        {"enclosed_by_second_multipolygon_component",
         "MULTIPOLYGON (((20 20, 25 20, 25 25, 20 25, 20 20)), "
         "((0 0, 10 0, 10 10, 0 10, 0 0)))",
         true},
        {"enclosed_by_polygon_in_gc",
         "GEOMETRYCOLLECTION (POINT (50 50), "
         "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))",
         true},
        {"disjoint_polygon", "POLYGON ((15 15, 20 15, 20 20, 15 20, 15 15))",
         false},
        {"polygon_empty", "POLYGON EMPTY", false},
        {"multipolygon_empty", "MULTIPOLYGON EMPTY", false},
    };
    for (const IntersectsCase& tc : cases) {
      VerifyIntersectsMatches(target.get(), prep_poly, *factory_prep, tc,
                              reader);
    }
  }

  // Non-rectangular triangle target lying strictly inside a hole of test
  // polygon: no segments intersect, dim == 2, representative point falls in
  // test hole -> returns false.
  {
    std::unique_ptr<Geometry> target =
        reader.read("POLYGON ((4 4, 6 4, 5 6, 4 4))");
    PreparedPolygon prep_poly(target.get());
    std::unique_ptr<PreparedGeometry> factory_prep =
        PreparedGeometryFactory::prepare(target.get());

    const IntersectsCase tc = {
        "target_inside_hole_of_test_polygon",
        "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
        "(2 2, 8 2, 8 8, 2 8, 2 2))",
        false,
    };
    VerifyIntersectsMatches(target.get(), prep_poly, *factory_prep, tc, reader);
  }

  // MultiPolygon target where one component is enclosed by test polygon.
  {
    std::unique_ptr<Geometry> target = reader.read(
        "MULTIPOLYGON (((2 2, 4 2, 3 4, 2 2)), "
        "((20 2, 22 2, 21 4, 20 2)))");
    PreparedPolygon prep_poly(target.get());
    std::unique_ptr<PreparedGeometry> factory_prep =
        PreparedGeometryFactory::prepare(target.get());

    const std::vector<IntersectsCase> cases = {
        {"encloses_first_component", "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
         true},
        {"encloses_second_component",
         "POLYGON ((18 0, 24 0, 24 6, 18 6, 18 0))", true},
        {"disjoint_between_components",
         "POLYGON ((12 0, 16 0, 16 6, 12 6, 12 0))", false},
    };
    for (const IntersectsCase& tc : cases) {
      VerifyIntersectsMatches(target.get(), prep_poly, *factory_prep, tc,
                              reader);
    }
  }
}

TEST(PreparedPolygonIntersectsTest,
     EmptyTargetInstanceReuseAndRectangleDirectHelper) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<Geometry> empty_target = reader.read("POLYGON EMPTY");
  PreparedPolygon prep_empty(empty_target.get());
  std::unique_ptr<PreparedGeometry> factory_empty =
      PreparedGeometryFactory::prepare(empty_target.get());

  const std::vector<IntersectsCase> empty_cases = {
      {"empty_vs_point", "POINT (1 1)", false},
      {"empty_vs_line", "LINESTRING (0 0, 1 1)", false},
      {"empty_vs_poly", "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))", false},
      {"empty_vs_empty_point", "POINT EMPTY", false},
      {"empty_vs_empty_poly", "POLYGON EMPTY", false},
  };
  for (const IntersectsCase& tc : empty_cases) {
    VerifyIntersectsMatches(empty_target.get(), prep_empty, *factory_empty, tc,
                            reader);
  }

  // Reuse a single PreparedPolygonIntersects instance on a rectangular polygon
  // (PreparedPolygon::intersects uses RectangleIntersects, while calling
  // PreparedPolygonIntersects directly exercises the general algorithm).
  std::unique_ptr<Geometry> rect =
      reader.read("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
  ASSERT_TRUE(rect->isRectangle());
  PreparedPolygon prep_rect(rect.get());
  PreparedPolygonIntersects op(&prep_rect);

  std::unique_ptr<Geometry> pt_in = reader.read("POINT (5 5)");
  std::unique_ptr<Geometry> pt_out = reader.read("POINT (12 5)");
  std::unique_ptr<Geometry> line_cross = reader.read("LINESTRING (0 5, 10 5)");
  std::unique_ptr<Geometry> line_out = reader.read("LINESTRING (10 0, 10 10)");
  std::unique_ptr<Geometry> poly_enclose =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> poly_out =
      reader.read("POLYGON ((12 0, 15 0, 15 5, 12 5, 12 0))");

  EXPECT_TRUE(op.intersects(pt_in.get()));
  EXPECT_FALSE(op.intersects(pt_out.get()));
  EXPECT_TRUE(op.intersects(line_cross.get()));
  EXPECT_FALSE(op.intersects(line_out.get()));
  EXPECT_TRUE(op.intersects(poly_enclose.get()));
  EXPECT_FALSE(op.intersects(poly_out.get()));
}

}  // namespace
}  // namespace geos_google
