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

// Unit tests for third_party/geos/src/geom/prep/PreparedPolygonDistance.cpp
// and third_party/geos/include/geos/geom/prep/PreparedPolygonDistance.h.

#include "third_party/geos/include/geos/geom/prep/PreparedPolygonDistance.h"

#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometry.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/PreparedPolygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::DoubleInfinity;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::prep::PreparedGeometry;
using geos::geom::prep::PreparedGeometryFactory;
using geos::geom::prep::PreparedPolygon;
using geos::geom::prep::PreparedPolygonDistance;
using geos::io::WKTReader;

static_assert(!std::is_copy_constructible_v<PreparedPolygonDistance>);
static_assert(!std::is_copy_assignable_v<PreparedPolygonDistance>);

struct DistanceCase {
  std::string_view label;
  std::string_view test_wkt;
  double expected_distance;
};

void VerifyDistanceMatches(const Geometry* target,
                           const PreparedPolygon& prep_poly,
                           const PreparedGeometry& factory_prep,
                           const DistanceCase& tc, const WKTReader& reader) {
  SCOPED_TRACE(std::string(tc.label));
  std::unique_ptr<Geometry> test_geom = reader.read(std::string(tc.test_wkt));
  ASSERT_NE(test_geom, nullptr);

  PreparedPolygonDistance op(prep_poly);

  EXPECT_DOUBLE_EQ(target->distance(test_geom.get()), tc.expected_distance);
  EXPECT_DOUBLE_EQ(
      PreparedPolygonDistance::distance(prep_poly, test_geom.get()),
      tc.expected_distance);
  EXPECT_DOUBLE_EQ(op.distance(test_geom.get()), tc.expected_distance);
  EXPECT_DOUBLE_EQ(prep_poly.distance(test_geom.get()), tc.expected_distance);
  EXPECT_DOUBLE_EQ(factory_prep.distance(test_geom.get()),
                   tc.expected_distance);

  if (tc.expected_distance == 0.0) {
    EXPECT_TRUE(op.isWithinDistance(test_geom.get(), 0.0));
    EXPECT_TRUE(op.isWithinDistance(test_geom.get(), 1.0));
    EXPECT_TRUE(prep_poly.isWithinDistance(test_geom.get(), 0.0));
    EXPECT_TRUE(factory_prep.isWithinDistance(test_geom.get(), 0.0));
  } else {
    const double below = tc.expected_distance - 1e-4;
    const double above = tc.expected_distance + 1e-4;
    EXPECT_FALSE(op.isWithinDistance(test_geom.get(), below));
    EXPECT_TRUE(op.isWithinDistance(test_geom.get(), tc.expected_distance));
    EXPECT_TRUE(op.isWithinDistance(test_geom.get(), above));
    EXPECT_FALSE(prep_poly.isWithinDistance(test_geom.get(), below));
    EXPECT_TRUE(
        prep_poly.isWithinDistance(test_geom.get(), tc.expected_distance));
    EXPECT_FALSE(factory_prep.isWithinDistance(test_geom.get(), below));
    EXPECT_TRUE(
        factory_prep.isWithinDistance(test_geom.get(), tc.expected_distance));
  }
}

TEST(PreparedPolygonDistanceTest,
     EmptyTargetAndEmptyTestGeometriesReturnInfinityAndFalse) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<Geometry> poly =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  PreparedPolygon prep_poly(poly.get());
  PreparedPolygonDistance op(prep_poly);
  std::unique_ptr<PreparedGeometry> factory_prep =
      PreparedGeometryFactory::prepare(poly.get());

  const std::vector<std::string_view> empty_wkts = {
      "POINT EMPTY",   "MULTIPOINT EMPTY",   "LINESTRING EMPTY",
      "POLYGON EMPTY", "MULTIPOLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY",
  };

  for (std::string_view wkt : empty_wkts) {
    SCOPED_TRACE(std::string(wkt));
    std::unique_ptr<Geometry> empty_geom = reader.read(std::string(wkt));

    const double d_static =
        PreparedPolygonDistance::distance(prep_poly, empty_geom.get());
    const double d_instance = op.distance(empty_geom.get());
    const double d_prep = prep_poly.distance(empty_geom.get());
    const double d_factory = factory_prep->distance(empty_geom.get());

    EXPECT_TRUE(std::isinf(d_static) && d_static > 0.0);
    EXPECT_TRUE(std::isinf(d_instance) && d_instance > 0.0);
    EXPECT_TRUE(std::isinf(d_prep) && d_prep > 0.0);
    EXPECT_TRUE(std::isinf(d_factory) && d_factory > 0.0);

    EXPECT_FALSE(op.isWithinDistance(empty_geom.get(), 0.0));
    EXPECT_FALSE(op.isWithinDistance(empty_geom.get(), 1e6));
    EXPECT_FALSE(op.isWithinDistance(empty_geom.get(), DoubleInfinity));
    EXPECT_FALSE(prep_poly.isWithinDistance(empty_geom.get(), DoubleInfinity));
    EXPECT_FALSE(
        factory_prep->isWithinDistance(empty_geom.get(), DoubleInfinity));
  }

  std::unique_ptr<Geometry> empty_target = reader.read("POLYGON EMPTY");
  PreparedPolygon prep_empty(empty_target.get());
  PreparedPolygonDistance empty_op(prep_empty);
  std::unique_ptr<PreparedGeometry> factory_empty =
      PreparedGeometryFactory::prepare(empty_target.get());

  for (std::string_view wkt :
       {"POINT (5 5)", "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))", "POINT EMPTY",
        "POLYGON EMPTY"}) {
    SCOPED_TRACE(std::string(wkt));
    std::unique_ptr<Geometry> test_geom = reader.read(std::string(wkt));

    const double d = empty_op.distance(test_geom.get());
    const double d_static =
        PreparedPolygonDistance::distance(prep_empty, test_geom.get());
    EXPECT_TRUE(std::isinf(d) && d > 0.0);
    EXPECT_TRUE(std::isinf(d_static) && d_static > 0.0);
    EXPECT_TRUE(std::isinf(prep_empty.distance(test_geom.get())));
    EXPECT_TRUE(std::isinf(factory_empty->distance(test_geom.get())));

    EXPECT_FALSE(empty_op.isWithinDistance(test_geom.get(), 1e6));
    EXPECT_FALSE(empty_op.isWithinDistance(test_geom.get(), DoubleInfinity));
    EXPECT_FALSE(prep_empty.isWithinDistance(test_geom.get(), 1e6));
    EXPECT_FALSE(factory_empty->isWithinDistance(test_geom.get(), 1e6));
  }
}

TEST(PreparedPolygonDistanceTest,
     PolygonWithHolePunctualLinealAndCollectionDistances) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // [0,10]x[0,10] with a [3,7]x[3,7] hole.
  std::unique_ptr<Geometry> target = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(3 3, 7 3, 7 7, 3 7, 3 3))");
  PreparedPolygon prep_poly(target.get());
  std::unique_ptr<PreparedGeometry> factory_prep =
      PreparedGeometryFactory::prepare(target.get());

  const std::vector<DistanceCase> cases = {
      // isAnyTestComponentInTarget() == true -> short-circuit 0.0 / true.
      {"point_in_solid_interior", "POINT (1 1)", 0.0},
      {"point_on_outer_boundary", "POINT (0 5)", 0.0},
      {"point_on_hole_boundary", "POINT (3 5)", 0.0},
      {"multipoint_first_outside_second_inside", "MULTIPOINT ((-5 -5), (1 1))",
       0.0},
      {"line_starting_inside", "LINESTRING (1 1, 20 20)", 0.0},
      {"poly_inside_solid_margin", "POLYGON ((1 1, 2 1, 2 2, 1 2, 1 1))", 0.0},
      {"gc_with_inside_point",
       "GEOMETRYCOLLECTION (POINT (20 20), POINT (1 1))", 0.0},

      // isAnyTestComponentInTarget() == false, IndexedFacetDistance evaluated.
      {"point_exterior_orthogonal", "POINT (13 5)", 3.0},
      {"point_exterior_orthogonal_z", "POINT Z (13 5 100)", 3.0},
      {"point_exterior_diagonal", "POINT (13 14)", 5.0},
      {"point_inside_hole", "POINT (5 5)", 2.0},
      {"multipoint_all_outside", "MULTIPOINT ((-4 5), (15 5))", 4.0},
      {"line_disjoint_vertical", "LINESTRING (13 0, 13 10)", 3.0},
      {"line_first_pt_outside_crossing_target", "LINESTRING (-5 5, 15 5)", 0.0},
      {"line_inside_hole", "LINESTRING (4 5, 6 5)", 1.0},
      {"gc_with_empty_and_disjoint_point",
       "GEOMETRYCOLLECTION (POINT EMPTY, POINT (13 5))", 3.0},
      {"gc_with_empty_polygon_and_disjoint_point",
       "GEOMETRYCOLLECTION (POLYGON EMPTY, POINT (13 5))", 3.0},
  };

  for (const DistanceCase& tc : cases) {
    VerifyDistanceMatches(target.get(), prep_poly, *factory_prep, tc, reader);
  }
}

TEST(PreparedPolygonDistanceTest,
     ArealTestGeometryContainmentAndDisjointBranches) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Case 1: g->getDimension() == 2, boundaries touch (dist == 0.0) while
  // isAnyTestComponentInTarget(g) is false because first vertex (15,2) is
  // exterior.
  {
    std::unique_ptr<Geometry> target =
        reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
    PreparedPolygon prep_poly(target.get());
    std::unique_ptr<PreparedGeometry> factory_prep =
        PreparedGeometryFactory::prepare(target.get());

    const DistanceCase tc = {
        "touching_polygon_first_vertex_exterior",
        "POLYGON ((15 2, 15 8, 10 8, 10 2, 15 2))",
        0.0,
    };
    VerifyDistanceMatches(target.get(), prep_poly, *factory_prep, tc, reader);
  }

  // Case 2: target enclosed strictly inside areal g (dist between boundaries is
  // positive, so isAnyTargetComponentInAreaTest returns true -> 0.0 / true).
  {
    std::unique_ptr<Geometry> target =
        reader.read("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
    PreparedPolygon prep_poly(target.get());
    std::unique_ptr<PreparedGeometry> factory_prep =
        PreparedGeometryFactory::prepare(target.get());

    const std::vector<DistanceCase> cases = {
        {"enclosing_polygon", "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", 0.0},
        {"enclosing_gc_with_disjoint_point_and_poly",
         "GEOMETRYCOLLECTION (POINT (50 50), "
         "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))",
         0.0},
        {"disjoint_polygon", "POLYGON ((11 2, 15 2, 15 8, 11 8, 11 2))", 3.0},
    };
    for (const DistanceCase& tc : cases) {
      VerifyDistanceMatches(target.get(), prep_poly, *factory_prep, tc, reader);
    }

    // Specifically verify !withinDistance triggers
    // isAnyTargetComponentInAreaTest when d is smaller than boundary distance
    // (2.0).
    std::unique_ptr<Geometry> enclosing =
        reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
    PreparedPolygonDistance op(prep_poly);
    EXPECT_TRUE(op.isWithinDistance(enclosing.get(), 0.5));
    EXPECT_TRUE(op.isWithinDistance(enclosing.get(), 3.0));
  }

  // Case 3: target lies strictly inside a hole of areal g ->
  // isAnyTargetComponentInAreaTest returns false and positive hole distance
  // is returned.
  {
    std::unique_ptr<Geometry> target =
        reader.read("POLYGON ((4 4, 6 4, 6 6, 4 6, 4 4))");
    PreparedPolygon prep_poly(target.get());
    std::unique_ptr<PreparedGeometry> factory_prep =
        PreparedGeometryFactory::prepare(target.get());

    const DistanceCase tc = {
        "target_inside_hole_of_test_polygon",
        "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
        "(2 2, 8 2, 8 8, 2 8, 2 2))",
        2.0,
    };
    VerifyDistanceMatches(target.get(), prep_poly, *factory_prep, tc, reader);
  }

  // Case 4: MultiPolygon target where one component is enclosed by test polygon
  // and another is disjoint.
  {
    std::unique_ptr<Geometry> target = reader.read(
        "MULTIPOLYGON (((2 2, 4 2, 4 4, 2 4, 2 2)), "
        "((20 2, 22 2, 22 4, 20 4, 20 2)))");
    PreparedPolygon prep_poly(target.get());
    std::unique_ptr<PreparedGeometry> factory_prep =
        PreparedGeometryFactory::prepare(target.get());

    const std::vector<DistanceCase> cases = {
        {"encloses_first_component", "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
         0.0},
        {"encloses_second_component",
         "POLYGON ((18 0, 24 0, 24 6, 18 6, 18 0))", 0.0},
        {"disjoint_from_both_components",
         "POLYGON ((12 2, 16 2, 16 4, 12 4, 12 2))", 4.0},
    };
    for (const DistanceCase& tc : cases) {
      VerifyDistanceMatches(target.get(), prep_poly, *factory_prep, tc, reader);
    }
  }
}

}  // namespace
}  // namespace geos_google
