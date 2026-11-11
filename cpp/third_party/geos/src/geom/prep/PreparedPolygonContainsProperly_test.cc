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
// third_party/geos/src/geom/prep/PreparedPolygonContainsProperly.cpp.
//
// google3 notes:
// - Creates an isolated GeometryFactory per test to avoid shared _refCount
//   mutation on GeometryFactory::getDefaultInstance().
// - Covers puntal, lineal, polygonal, multipolygonal, hole-enclosing, and
//   boundary-touching cases, and contrasts containsProperly() with contains().
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/prep/PreparedPolygonContainsProperly.cpp

#include "third_party/geos/include/geos/geom/prep/PreparedPolygonContainsProperly.h"

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
using geos::geom::prep::PreparedPolygonContainsProperly;
using geos::io::WKTReader;

struct ContainsProperlyCase {
  std::string_view name;
  std::string_view test_wkt;
  bool expected;
};

class PreparedPolygonContainsProperlyTest : public ::testing::Test {
 protected:
  Geometry::Ptr Read(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  void VerifyContainsProperly(
      const Geometry& target, const PreparedPolygon& prep,
      std::span<const ContainsProperlyCase> cases) const {
    for (const auto& c : cases) {
      SCOPED_TRACE(c.name);
      Geometry::Ptr test_geom = Read(c.test_wkt);
      EXPECT_EQ(PreparedPolygonContainsProperly::containsProperly(
                    &prep, test_geom.get()),
                c.expected);
      PreparedPolygonContainsProperly op(&prep);
      EXPECT_EQ(op.containsProperly(test_geom.get()), c.expected);
      EXPECT_EQ(target.relate(test_geom.get(), "T**FF*FF*"), c.expected);
    }
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(PreparedPolygonContainsProperlyTest, PuntalAndCollectionPredicates) {
  Geometry::Ptr square = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  PreparedPolygon prep(square.get());

  constexpr auto kCases = std::to_array<ContainsProperlyCase>({
      {.name = "point_strictly_inside",
       .test_wkt = "POINT (5 5)",
       .expected = true},
      {.name = "point_on_edge", .test_wkt = "POINT (0 5)", .expected = false},
      {.name = "point_on_vertex", .test_wkt = "POINT (0 0)", .expected = false},
      {.name = "point_outside", .test_wkt = "POINT (15 5)", .expected = false},
      {.name = "multipoint_all_strictly_inside",
       .test_wkt = "MULTIPOINT ((2 2), (8 8))",
       .expected = true},
      {.name = "multipoint_one_inside_one_boundary",
       .test_wkt = "MULTIPOINT ((5 5), (0 5))",
       .expected = false},
      {.name = "multipoint_one_inside_one_outside",
       .test_wkt = "MULTIPOINT ((5 5), (12 5))",
       .expected = false},
      {.name = "collection_point_and_line_strictly_inside",
       .test_wkt = "GEOMETRYCOLLECTION (POINT (5 5), LINESTRING (2 2, 8 8))",
       .expected = true},
      {.name = "collection_with_boundary_point",
       .test_wkt = "GEOMETRYCOLLECTION (POINT (5 5), POINT (10 5))",
       .expected = false},
  });

  VerifyContainsProperly(*square, prep, kCases);
}

TEST_F(PreparedPolygonContainsProperlyTest,
       LinealAndBoundaryDistinctionFromContains) {
  Geometry::Ptr square = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  PreparedPolygon prep(square.get());

  constexpr auto kCases = std::to_array<ContainsProperlyCase>({
      {.name = "line_strictly_inside",
       .test_wkt = "LINESTRING (2 2, 8 8)",
       .expected = true},
      {.name = "multiline_strictly_inside",
       .test_wkt = "MULTILINESTRING ((1 1, 4 4), (6 6, 9 9))",
       .expected = true},
      {.name = "line_starting_on_boundary_going_inside",
       .test_wkt = "LINESTRING (0 5, 5 5)",
       .expected = false},
      {.name = "line_starting_inside_ending_on_boundary",
       .test_wkt = "LINESTRING (5 5, 10 5)",
       .expected = false},
      {.name = "line_starting_inside_crossing_outside",
       .test_wkt = "LINESTRING (5 5, 15 5)",
       .expected = false},
      {.name = "multiline_one_inside_one_touching_boundary",
       .test_wkt = "MULTILINESTRING ((2 2, 4 4), (5 5, 10 5))",
       .expected = false},
      {.name = "polygon_strictly_inside",
       .test_wkt = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
       .expected = true},
      {.name = "polygon_sharing_boundary_at_first_vertex",
       .test_wkt = "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))",
       .expected = false},
      {.name = "polygon_first_vertex_inside_later_vertex_on_boundary",
       .test_wkt = "POLYGON ((2 2, 10 5, 2 8, 2 2))",
       .expected = false},
      {.name = "identical_polygon",
       .test_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
       .expected = false},
  });

  VerifyContainsProperly(*square, prep, kCases);

  // Confirm semantic distinction: contains() is true when touching boundary
  // while extending into interior, whereas containsProperly() is false.
  Geometry::Ptr line_touching_boundary = Read("LINESTRING (0 5, 5 5)");
  Geometry::Ptr poly_sharing_edge = Read("POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))");
  EXPECT_TRUE(square->contains(line_touching_boundary.get()));
  EXPECT_FALSE(PreparedPolygonContainsProperly::containsProperly(
      &prep, line_touching_boundary.get()));
  EXPECT_TRUE(square->contains(poly_sharing_edge.get()));
  EXPECT_FALSE(PreparedPolygonContainsProperly::containsProperly(
      &prep, poly_sharing_edge.get()));
}

TEST_F(PreparedPolygonContainsProperlyTest,
       PolygonalAndMultiPolygonHoleEnclosureChecks) {
  Geometry::Ptr donut = Read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(3 3, 7 3, 7 7, 3 7, 3 3))");
  PreparedPolygon prep_donut(donut.get());

  constexpr auto kDonutCases = std::to_array<ContainsProperlyCase>({
      {.name = "point_inside_hole",
       .test_wkt = "POINT (5 5)",
       .expected = false},
      {.name = "line_crossing_hole",
       .test_wkt = "LINESTRING (1 5, 9 5)",
       .expected = false},
      {.name = "polygon_in_ring_beside_hole",
       .test_wkt = "POLYGON ((0.5 0.5, 2.5 0.5, 2.5 9.5, 0.5 9.5, 0.5 0.5))",
       .expected = true},
      {.name = "polygon_enclosing_hole_without_edge_intersection",
       .test_wkt = "POLYGON ((1 1, 9 1, 9 9, 1 9, 1 1))",
       .expected = false},
      {.name = "polygon_with_larger_hole_excluding_target_hole",
       .test_wkt = "POLYGON ((1 1, 9 1, 9 9, 1 9, 1 1), "
                   "(2 2, 8 2, 8 8, 2 8, 2 2))",
       .expected = true},
      {.name = "multipolygon_with_component_enclosing_hole",
       .test_wkt = "MULTIPOLYGON (((1 1, 9 1, 9 9, 1 9, 1 1)), "
                   "((0.2 0.2, 0.5 0.2, 0.5 0.5, 0.2 0.5, 0.2 0.2)))",
       .expected = false},
      {.name = "multipolygon_components_strictly_in_ring",
       .test_wkt =
           "MULTIPOLYGON (((0.5 0.5, 2.5 0.5, 2.5 2.5, 0.5 2.5, 0.5 0.5)), "
           "((7.5 7.5, 9.5 7.5, 9.5 9.5, 7.5 9.5, 7.5 7.5)))",
       .expected = true},
  });
  VerifyContainsProperly(*donut, prep_donut, kDonutCases);

  Geometry::Ptr donut_with_island = Read(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2)), "
      "((4 4, 6 4, 6 6, 4 6, 4 4)))");
  PreparedPolygon prep_island(donut_with_island.get());

  constexpr auto kIslandCases = std::to_array<ContainsProperlyCase>({
      {.name = "multipoint_across_shell_and_island",
       .test_wkt = "MULTIPOINT ((1 1), (5 5))",
       .expected = true},
      {.name = "polygon_enclosing_hole_and_island",
       .test_wkt = "POLYGON ((1 1, 9 1, 9 9, 1 9, 1 1))",
       .expected = false},
      {.name = "multipolygon_inside_shell_and_island",
       .test_wkt =
           "MULTIPOLYGON (((0.5 0.5, 1.5 0.5, 1.5 1.5, 0.5 1.5, 0.5 0.5)), "
           "((4.5 4.5, 5.5 4.5, 5.5 5.5, 4.5 5.5, 4.5 4.5)))",
       .expected = true},
  });
  VerifyContainsProperly(*donut_with_island, prep_island, kIslandCases);
}

TEST_F(PreparedPolygonContainsProperlyTest,
       EmptyGeometriesHandledViaPreparedPolygonEnvelopeGuard) {
  Geometry::Ptr square = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  PreparedPolygon prep(square.get());

  for (std::string_view empty_wkt :
       {"POINT EMPTY", "LINESTRING EMPTY", "POLYGON EMPTY"}) {
    SCOPED_TRACE(empty_wkt);
    Geometry::Ptr empty_geom = Read(empty_wkt);
    EXPECT_FALSE(prep.containsProperly(empty_geom.get()));
    EXPECT_FALSE(square->relate(empty_geom.get(), "T**FF*FF*"));
  }

  Geometry::Ptr empty_poly = Read("POLYGON EMPTY");
  PreparedPolygon empty_prep(empty_poly.get());
  Geometry::Ptr pt = Read("POINT (5 5)");
  EXPECT_FALSE(empty_prep.containsProperly(pt.get()));
  EXPECT_FALSE(empty_poly->relate(pt.get(), "T**FF*FF*"));
}

}  // namespace
}  // namespace geos_google
