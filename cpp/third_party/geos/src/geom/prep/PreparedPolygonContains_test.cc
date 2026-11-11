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

// Unit tests for third_party/geos/src/geom/prep/PreparedPolygonContains.cpp.
//
// google3 notes:
// - Creates an isolated GeometryFactory per test to avoid shared _refCount
//   mutation on GeometryFactory::getDefaultInstance().
// - Includes test cases specifically exercising boundary-touch and collinear
//   overlap geometries so fullTopologicalPredicate() is invoked for both true
//   and false outcomes.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/prep/PreparedPolygonContains.cpp

#include "third_party/geos/include/geos/geom/prep/PreparedPolygonContains.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/PreparedPolygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::prep::PreparedPolygon;
using geos::geom::prep::PreparedPolygonContains;
using geos::io::WKTReader;

struct ContainsCase {
  std::string_view name;
  std::string_view test_wkt;
  bool expected;
};

class PreparedPolygonContainsTest : public ::testing::Test {
 protected:
  Geometry::Ptr Read(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  void VerifyContains(const Geometry& target, const PreparedPolygon& prep,
                      std::span<const ContainsCase> cases) const {
    for (const auto& c : cases) {
      SCOPED_TRACE(c.name);
      Geometry::Ptr test_geom = Read(c.test_wkt);
      EXPECT_EQ(PreparedPolygonContains::contains(&prep, test_geom.get()),
                c.expected);
      PreparedPolygonContains op(&prep);
      EXPECT_EQ(op.contains(test_geom.get()), c.expected);
      EXPECT_EQ(target.contains(test_geom.get()), c.expected);
    }
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(PreparedPolygonContainsTest, StaticAndInstancePuntalPredicates) {
  Geometry::Ptr square = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  PreparedPolygon prep(square.get());

  constexpr auto kCases = std::to_array<ContainsCase>({
      {.name = "point_inside", .test_wkt = "POINT (5 5)", .expected = true},
      {.name = "point_on_edge", .test_wkt = "POINT (0 5)", .expected = false},
      {.name = "point_on_vertex", .test_wkt = "POINT (0 0)", .expected = false},
      {.name = "point_outside", .test_wkt = "POINT (15 5)", .expected = false},
      {.name = "point_empty", .test_wkt = "POINT EMPTY", .expected = false},
      {.name = "multipoint_all_inside",
       .test_wkt = "MULTIPOINT ((2 2), (8 8))",
       .expected = true},
      {.name = "multipoint_one_inside_one_boundary",
       .test_wkt = "MULTIPOINT ((0 5), (5 5))",
       .expected = true},
      {.name = "multipoint_all_boundary",
       .test_wkt = "MULTIPOINT ((0 5), (10 5))",
       .expected = false},
      {.name = "multipoint_one_inside_one_outside",
       .test_wkt = "MULTIPOINT ((5 5), (15 5))",
       .expected = false},
      {.name = "multipoint_empty",
       .test_wkt = "MULTIPOINT EMPTY",
       .expected = false},
  });

  VerifyContains(*square, prep, kCases);
}

TEST_F(PreparedPolygonContainsTest, LinealAndPolygonalShortCircuits) {
  Geometry::Ptr chamfer = Read("POLYGON ((0 0, 10 0, 10 8, 8 10, 0 10, 0 0))");
  PreparedPolygon prep_chamfer(chamfer.get());

  constexpr auto kSingleShellCases = std::to_array<ContainsCase>({
      {.name = "line_strictly_inside",
       .test_wkt = "LINESTRING (1 1, 7 7)",
       .expected = true},
      {.name = "line_outside",
       .test_wkt = "LINESTRING (15 5, 20 5)",
       .expected = false},
      {.name = "line_crossing_boundary_properly",
       .test_wkt = "LINESTRING (5 5, 15 5)",
       .expected = false},
      {.name = "polygon_strictly_inside",
       .test_wkt = "POLYGON ((1 1, 5 1, 5 5, 1 5, 1 1))",
       .expected = true},
      {.name = "polygon_crossing_boundary_properly",
       .test_wkt = "POLYGON ((5 5, 12 5, 12 8, 5 8, 5 5))",
       .expected = false},
  });
  VerifyContains(*chamfer, prep_chamfer, kSingleShellCases);

  Geometry::Ptr donut = Read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(3 3, 7 3, 7 7, 3 7, 3 3))");
  PreparedPolygon prep_donut(donut.get());

  constexpr auto kDonutCases = std::to_array<ContainsCase>({
      {.name = "line_crossing_hole_properly",
       .test_wkt = "LINESTRING (1 5, 9 5)",
       .expected = false},
      {.name = "polygon_enclosing_hole_without_segment_intersection",
       .test_wkt = "POLYGON ((1 1, 9 1, 9 9, 1 9, 1 1))",
       .expected = false},
      {.name = "polygon_with_larger_hole_inside_donut",
       .test_wkt = "POLYGON ((1 1, 9 1, 9 9, 1 9, 1 1), "
                   "(2 2, 8 2, 8 8, 2 8, 2 2))",
       .expected = true},
  });
  VerifyContains(*donut, prep_donut, kDonutCases);
}

TEST_F(PreparedPolygonContainsTest,
       FullTopologicalPredicateReturningTrueAndFalse) {
  Geometry::Ptr chamfer = Read("POLYGON ((0 0, 10 0, 10 8, 8 10, 0 10, 0 0))");
  PreparedPolygon prep_chamfer(chamfer.get());

  // Non-proper boundary touches/overlaps without proper intersections bypass
  // the fast short-circuits and evaluate fullTopologicalPredicate().
  constexpr auto kFullEvalCases = std::to_array<ContainsCase>({
      {.name = "line_touching_boundary_non_properly_into_interior",
       .test_wkt = "LINESTRING (0 5, 5 5, 10 5)",
       .expected = true},
      {.name = "line_wholly_on_boundary",
       .test_wkt = "LINESTRING (0 2, 0 8)",
       .expected = false},
      {.name = "polygon_sharing_boundary_edges_inside",
       .test_wkt = "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))",
       .expected = true},
      {.name = "polygon_sharing_boundary_and_extending_past_chamfer",
       .test_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
       .expected = false},
      {.name = "collection_with_boundary_touching_line_and_interior_point",
       .test_wkt = "GEOMETRYCOLLECTION (POINT (5 5), LINESTRING (0 5, 5 5))",
       .expected = true},
      {.name = "collection_with_boundary_only_line",
       .test_wkt = "GEOMETRYCOLLECTION (LINESTRING (0 2, 0 8))",
       .expected = false},
  });
  VerifyContains(*chamfer, prep_chamfer, kFullEvalCases);

  Geometry::Ptr pinched = Read(
      "MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "((5 5, 10 5, 10 10, 5 10, 5 5)))");
  PreparedPolygon prep_pinched(pinched.get());

  constexpr auto kPinchedCases = std::to_array<ContainsCase>({
      {.name = "line_bridging_pinched_vertex_into_second_shell",
       .test_wkt = "LINESTRING (2 2, 5 5, 8 8)",
       .expected = true},
      {.name = "line_exiting_through_pinched_vertex",
       .test_wkt = "LINESTRING (2 2, 5 5, 2 8)",
       .expected = false},
      {.name = "polygon_inside_single_component",
       .test_wkt = "POLYGON ((1 1, 4 1, 4 4, 1 4, 1 1))",
       .expected = true},
  });
  VerifyContains(*pinched, prep_pinched, kPinchedCases);
}

TEST_F(PreparedPolygonContainsTest, ReusesSingleInstanceAcrossQueries) {
  Geometry::Ptr chamfer = Read("POLYGON ((0 0, 10 0, 10 8, 8 10, 0 10, 0 0))");
  PreparedPolygon prep(chamfer.get());
  PreparedPolygonContains op(&prep);

  Geometry::Ptr on_boundary_line = Read("LINESTRING (0 2, 0 8)");
  Geometry::Ptr inside_line = Read("LINESTRING (2 2, 6 6)");
  Geometry::Ptr crossing_line = Read("LINESTRING (5 5, 15 5)");
  Geometry::Ptr inside_poly = Read("POLYGON ((1 1, 5 1, 5 5, 1 5, 1 1))");

  EXPECT_FALSE(op.contains(on_boundary_line.get()));
  EXPECT_TRUE(op.contains(inside_line.get()));
  EXPECT_FALSE(op.contains(crossing_line.get()));
  EXPECT_TRUE(op.contains(inside_poly.get()));
}

}  // namespace
}  // namespace geos_google
