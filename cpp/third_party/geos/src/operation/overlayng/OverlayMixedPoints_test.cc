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
// third_party/geos/src/operation/overlayng/OverlayMixedPoints.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/OverlayMixedPoints.cpp

#include "third_party/geos/include/geos/operation/overlayng/OverlayMixedPoints.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"
#include "third_party/geos/include/geos/util/AssertionFailedException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Point;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::overlayng::OverlayMixedPoints;
using geos::operation::overlayng::OverlayNG;
using geos::util::AssertionFailedException;

struct OverlayCase {
  std::string_view name;
  int op_code;
  std::string_view geom0_wkt;
  std::string_view geom1_wkt;
  std::string_view expected_wkt;
};

constexpr std::string_view kSquareWithHole =
    "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
    "(1.5 1.5, 2.5 1.5, 2.5 2.5, 1.5 2.5, 1.5 1.5))";
constexpr std::string_view kTwoSquares =
    "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
    "((4 0, 6 0, 6 2, 4 2, 4 0)))";
constexpr std::string_view kLine = "LINESTRING (0 0, 4 0)";
constexpr std::string_view kTwoLines =
    "MULTILINESTRING ((0 0, 2 0), (0 2, 2 2))";

constexpr auto kFloatingOverlayCases = std::to_array<OverlayCase>({
    // Point vs Polygon (LHS = Point)
    {
        .name = "point_interior_polygon_intersection",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "POINT (1 1)",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "POINT (1 1)",
    },
    {
        .name = "point_boundary_polygon_intersection",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "POINT (0 2)",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "POINT (0 2)",
    },
    {
        .name = "point_hole_polygon_intersection",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "POINT (2 2)",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "POINT EMPTY",
    },
    {
        .name = "point_exterior_polygon_intersection",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "POINT (6 6)",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "POINT EMPTY",
    },
    {
        .name = "point_interior_polygon_union",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = "POINT (1 1)",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = kSquareWithHole,
    },
    {
        .name = "point_exterior_polygon_union",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = "POINT (6 6)",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "GEOMETRYCOLLECTION ("
                        "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
                        "(1.5 1.5, 2.5 1.5, 2.5 2.5, 1.5 2.5, 1.5 1.5)), "
                        "POINT (6 6))",
    },
    {
        .name = "point_hole_polygon_symdifference",
        .op_code = OverlayNG::SYMDIFFERENCE,
        .geom0_wkt = "POINT (2 2)",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "GEOMETRYCOLLECTION ("
                        "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
                        "(1.5 1.5, 2.5 1.5, 2.5 2.5, 1.5 2.5, 1.5 1.5)), "
                        "POINT (2 2))",
    },
    {
        .name = "point_interior_polygon_difference",
        .op_code = OverlayNG::DIFFERENCE,
        .geom0_wkt = "POINT (1 1)",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "POINT EMPTY",
    },
    {
        .name = "point_exterior_polygon_difference",
        .op_code = OverlayNG::DIFFERENCE,
        .geom0_wkt = "POINT (6 6)",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "POINT (6 6)",
    },

    // Polygon vs Point (isPointRHS = true)
    {
        .name = "polygon_rhs_point_intersection",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = kSquareWithHole,
        .geom1_wkt = "POINT (1 1)",
        .expected_wkt = "POINT (1 1)",
    },
    {
        .name = "polygon_rhs_point_union",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = kSquareWithHole,
        .geom1_wkt = "POINT (6 6)",
        .expected_wkt = "GEOMETRYCOLLECTION ("
                        "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
                        "(1.5 1.5, 2.5 1.5, 2.5 2.5, 1.5 2.5, 1.5 1.5)), "
                        "POINT (6 6))",
    },
    {
        .name = "polygon_rhs_point_symdifference",
        .op_code = OverlayNG::SYMDIFFERENCE,
        .geom0_wkt = kSquareWithHole,
        .geom1_wkt = "POINT (6 6)",
        .expected_wkt = "GEOMETRYCOLLECTION ("
                        "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
                        "(1.5 1.5, 2.5 1.5, 2.5 2.5, 1.5 2.5, 1.5 1.5)), "
                        "POINT (6 6))",
    },
    {
        .name = "polygon_rhs_point_difference",
        .op_code = OverlayNG::DIFFERENCE,
        .geom0_wkt = kSquareWithHole,
        .geom1_wkt = "POINT (1 1)",
        .expected_wkt = kSquareWithHole,
    },

    // MultiPoint deduplication, ordering, single-result atomic Point
    {
        .name = "multipoint_polygon_intersection_dedup",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "MULTIPOINT ((5 5), (1 1), (0 0), (1 1), (2 2))",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "MULTIPOINT ((0 0), (1 1))",
    },
    {
        .name = "multipoint_polygon_intersection_single",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "MULTIPOINT ((5 5), (1 1), (1 1), (2 2))",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "POINT (1 1)",
    },
    {
        .name = "multipoint_polygon_difference_dedup",
        .op_code = OverlayNG::DIFFERENCE,
        .geom0_wkt = "MULTIPOINT ((8 8), (1 1), (2 2), (8 8), (5 5))",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "MULTIPOINT ((2 2), (5 5), (8 8))",
    },

    // MultiPolygon
    {
        .name = "multipoint_multipolygon_intersection",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "MULTIPOINT ((5 1), (4 0), (3 1), (1 1), (5 1))",
        .geom1_wkt = kTwoSquares,
        .expected_wkt = "MULTIPOINT ((1 1), (4 0), (5 1))",
    },
    {
        .name = "multipoint_multipolygon_union_with_exterior",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = "MULTIPOINT ((1 1), (3 1))",
        .geom1_wkt = kTwoSquares,
        .expected_wkt = "GEOMETRYCOLLECTION ("
                        "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
                        "POLYGON ((4 0, 6 0, 6 2, 4 2, 4 0)), "
                        "POINT (3 1))",
    },
    {
        .name = "multipoint_multipolygon_union_all_covered",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = "MULTIPOINT ((1 1), (5 1))",
        .geom1_wkt = kTwoSquares,
        .expected_wkt = kTwoSquares,
    },

    // Point / MultiPoint vs LineString / MultiLineString
    {
        .name = "point_interior_line_intersection",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "POINT (2 0)",
        .geom1_wkt = kLine,
        .expected_wkt = "POINT (2 0)",
    },
    {
        .name = "point_endpoint_line_intersection",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "POINT (0 0)",
        .geom1_wkt = kLine,
        .expected_wkt = "POINT (0 0)",
    },
    {
        .name = "point_exterior_line_intersection",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "POINT (2 1)",
        .geom1_wkt = kLine,
        .expected_wkt = "POINT EMPTY",
    },
    {
        .name = "point_exterior_line_union",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = "POINT (2 1)",
        .geom1_wkt = kLine,
        .expected_wkt =
            "GEOMETRYCOLLECTION (LINESTRING (0 0, 4 0), POINT (2 1))",
    },
    {
        .name = "point_covered_line_symdifference",
        .op_code = OverlayNG::SYMDIFFERENCE,
        .geom0_wkt = "POINT (2 0)",
        .geom1_wkt = kLine,
        .expected_wkt = kLine,
    },
    {
        .name = "point_line_difference",
        .op_code = OverlayNG::DIFFERENCE,
        .geom0_wkt = "MULTIPOINT ((0 0), (2 0), (2 1), (5 0))",
        .geom1_wkt = kLine,
        .expected_wkt = "MULTIPOINT ((2 1), (5 0))",
    },
    {
        .name = "line_rhs_point_difference",
        .op_code = OverlayNG::DIFFERENCE,
        .geom0_wkt = kLine,
        .geom1_wkt = "POINT (2 0)",
        .expected_wkt = kLine,
    },
    {
        .name = "multipoint_multiline_union",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = "MULTIPOINT ((1 0), (1 5), (1 5))",
        .geom1_wkt = kTwoLines,
        .expected_wkt = "GEOMETRYCOLLECTION ("
                        "LINESTRING (0 0, 2 0), "
                        "LINESTRING (0 2, 2 2), "
                        "POINT (1 5))",
    },
    {
        .name = "multipoint_closed_line_intersection",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "MULTIPOINT ((0 0), (1 0), (1 1))",
        .geom1_wkt = "LINESTRING (0 0, 2 0, 2 2, 0 2, 0 0)",
        .expected_wkt = "MULTIPOINT ((0 0), (1 0))",
    },

    // Empty inputs
    {
        .name = "empty_point_polygon_intersection",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "POINT EMPTY",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "POINT EMPTY",
    },
    {
        .name = "empty_point_polygon_union",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = "POINT EMPTY",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = kSquareWithHole,
    },
    {
        .name = "empty_point_polygon_difference",
        .op_code = OverlayNG::DIFFERENCE,
        .geom0_wkt = "POINT EMPTY",
        .geom1_wkt = kSquareWithHole,
        .expected_wkt = "POINT EMPTY",
    },
    {
        .name = "empty_multipoint_line_union",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = "MULTIPOINT EMPTY",
        .geom1_wkt = kLine,
        .expected_wkt = kLine,
    },
    {
        .name = "point_empty_polygon_intersection",
        .op_code = OverlayNG::INTERSECTION,
        .geom0_wkt = "POINT (1 1)",
        .geom1_wkt = "POLYGON EMPTY",
        .expected_wkt = "POINT EMPTY",
    },
    {
        .name = "point_empty_polygon_union",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = "POINT (1 1)",
        .geom1_wkt = "POLYGON EMPTY",
        .expected_wkt = "POINT (1 1)",
    },
    {
        .name = "point_empty_polygon_difference_lhs",
        .op_code = OverlayNG::DIFFERENCE,
        .geom0_wkt = "POINT (1 1)",
        .geom1_wkt = "POLYGON EMPTY",
        .expected_wkt = "POINT (1 1)",
    },
    {
        .name = "point_empty_polygon_difference",
        .op_code = OverlayNG::DIFFERENCE,
        .geom0_wkt = "POLYGON EMPTY",
        .geom1_wkt = "POINT (1 1)",
        .expected_wkt = "POLYGON EMPTY",
    },
    {
        .name = "point_empty_line_union",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = "POINT (1 1)",
        .geom1_wkt = "LINESTRING EMPTY",
        .expected_wkt = "POINT (1 1)",
    },
    {
        .name = "point_empty_line_symdifference",
        .op_code = OverlayNG::SYMDIFFERENCE,
        .geom0_wkt = "POINT (1 1)",
        .geom1_wkt = "LINESTRING EMPTY",
        .expected_wkt = "POINT (1 1)",
    },
    {
        .name = "point_empty_line_difference",
        .op_code = OverlayNG::DIFFERENCE,
        .geom0_wkt = "LINESTRING EMPTY",
        .geom1_wkt = "POINT (1 1)",
        .expected_wkt = "LINESTRING EMPTY",
    },
    {
        .name = "both_empty_polygon_union",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = "POINT EMPTY",
        .geom1_wkt = "POLYGON EMPTY",
        .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
    },
    {
        .name = "both_empty_line_union",
        .op_code = OverlayNG::UNION,
        .geom0_wkt = "POINT EMPTY",
        .geom1_wkt = "LINESTRING EMPTY",
        .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
    },
});

class OverlayMixedPointsTest : public ::testing::Test {
 protected:
  OverlayMixedPointsTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  Geometry::Ptr Read(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  void AssertGeomsEqual(const Geometry* actual,
                        std::string_view expected_wkt) const {
    ASSERT_NE(actual, nullptr);
    Geometry::Ptr expected = Read(expected_wkt);
    ASSERT_NE(expected, nullptr);

    Geometry::Ptr actual_norm = actual->clone();
    actual_norm->normalize();
    expected->normalize();

    SCOPED_TRACE("Actual: " + actual_norm->toText() +
                 "\nExpected: " + expected->toText());
    EXPECT_TRUE(actual_norm->equalsExact(expected.get(), 1e-9));
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(OverlayMixedPointsTest, FloatingPrecisionOverlayCases) {
  PrecisionModel floating_pm;
  for (const OverlayCase& tc : kFloatingOverlayCases) {
    SCOPED_TRACE(std::string(tc.name));
    Geometry::Ptr g0 = Read(tc.geom0_wkt);
    Geometry::Ptr g1 = Read(tc.geom1_wkt);
    Geometry::Ptr result = OverlayMixedPoints::overlay(tc.op_code, g0.get(),
                                                       g1.get(), &floating_pm);
    AssertGeomsEqual(result.get(), tc.expected_wkt);
  }
}

TEST_F(OverlayMixedPointsTest, DirectConstructorAndNullPrecisionFallback) {
  PrecisionModel fixed_pm(1.0);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&fixed_pm);

  // When p_pm is nullptr, constructor falls back to geom0->getPrecisionModel().
  Geometry::Ptr pt_lhs = fixed_factory->createPoint(CoordinateXY(1.2, 2.7));
  Geometry::Ptr poly_rhs = Read("POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))");

  OverlayMixedPoints omp_lhs(OverlayNG::INTERSECTION, pt_lhs.get(),
                             poly_rhs.get(), nullptr);
  Geometry::Ptr res_lhs = omp_lhs.getResult();
  AssertGeomsEqual(res_lhs.get(), "POINT (1 3)");

  // Also test fallback when geom0 is non-point with fixed PrecisionModel.
  CoordinateSequence line_coords(
      {CoordinateXY(0.1, 0.2), CoordinateXY(3.8, 0.1)});
  Geometry::Ptr line_lhs = fixed_factory->createLineString(line_coords);
  Geometry::Ptr pt_rhs = Read("POINT (9.2 9.4)");

  OverlayMixedPoints omp_rhs(OverlayNG::DIFFERENCE, line_lhs.get(),
                             pt_rhs.get(), nullptr);
  Geometry::Ptr res_rhs = omp_rhs.getResult();
  AssertGeomsEqual(res_rhs.get(), "LINESTRING (0 0, 4 0)");
}

TEST_F(OverlayMixedPointsTest, FixedPrecisionRoundsAndDeduplicatesPoints) {
  PrecisionModel fixed_pm(1.0);
  Geometry::Ptr pts = Read("MULTIPOINT ((1.2 1.8), (1.4 2.1), (5.2 4.8))");
  Geometry::Ptr poly = Read("POLYGON ((0 0, 3 0, 3 3, 0 3, 0 0))");

  // (1.2, 1.8) and (1.4, 2.1) both round to (1, 2) and collapse to one Point.
  Geometry::Ptr inter = OverlayMixedPoints::overlay(
      OverlayNG::INTERSECTION, pts.get(), poly.get(), &fixed_pm);
  AssertGeomsEqual(inter.get(), "POINT (1 2)");

  Geometry::Ptr diff = OverlayMixedPoints::overlay(
      OverlayNG::DIFFERENCE, pts.get(), poly.get(), &fixed_pm);
  AssertGeomsEqual(diff.get(), "POINT (5 5)");
}

TEST_F(OverlayMixedPointsTest, ResultDimZeroSkipsNonPointRoundingAndNoding) {
  PrecisionModel fixed_pm(1.0);
  Geometry::Ptr pt = Read("POINT (0.2 0.2)");
  Geometry::Ptr tiny_poly = Read("POLYGON ((0 0, 0.4 0, 0.4 0.4, 0 0.4, 0 0))");

  // For INTERSECTION (resultDim == 0), tiny_poly is not rounded/noded, so it
  // does not collapse and still touches the rounded point (0, 0).
  Geometry::Ptr inter = OverlayMixedPoints::overlay(
      OverlayNG::INTERSECTION, pt.get(), tiny_poly.get(), &fixed_pm);
  AssertGeomsEqual(inter.get(), "POINT (0 0)");

  // For UNION (resultDim == 2), tiny_poly is noded with fixed_pm and collapses
  // to POLYGON EMPTY, leaving only the rounded exterior point.
  Geometry::Ptr uni = OverlayMixedPoints::overlay(OverlayNG::UNION, pt.get(),
                                                  tiny_poly.get(), &fixed_pm);
  AssertGeomsEqual(uni.get(), "POINT (0 0)");

  // Similarly, a small line collapses to LINESTRING EMPTY under fixed_pm.
  Geometry::Ptr tiny_line = Read("LINESTRING (0 0, 0.2 0.2)");
  Geometry::Ptr line_uni = OverlayMixedPoints::overlay(
      OverlayNG::UNION, pt.get(), tiny_line.get(), &fixed_pm);
  AssertGeomsEqual(line_uni.get(), "POINT (0 0)");
}

TEST_F(OverlayMixedPointsTest, FixedPrecisionRoundsNonPointOutput) {
  PrecisionModel fixed_pm(1.0);
  Geometry::Ptr pt = Read("POINT (1.3 1.3)");
  Geometry::Ptr poly = Read("POLYGON ((0 0, 1.4 0, 1.4 1.4, 0 1.4, 0 0))");

  // INTERSECTION compares rounded (1, 1) against unrounded [0, 1.4]^2 ->
  // inside.
  Geometry::Ptr inter = OverlayMixedPoints::overlay(
      OverlayNG::INTERSECTION, pt.get(), poly.get(), &fixed_pm);
  AssertGeomsEqual(inter.get(), "POINT (1 1)");

  // UNION rounds poly to [0, 1]^2; (1, 1) lies on its boundary -> only polygon.
  Geometry::Ptr uni = OverlayMixedPoints::overlay(OverlayNG::UNION, pt.get(),
                                                  poly.get(), &fixed_pm);
  AssertGeomsEqual(uni.get(), "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");

  // DIFFERENCE(poly, pt) returns the rounded polygon.
  Geometry::Ptr diff = OverlayMixedPoints::overlay(
      OverlayNG::DIFFERENCE, poly.get(), pt.get(), &fixed_pm);
  AssertGeomsEqual(diff.get(), "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
}

TEST_F(OverlayMixedPointsTest, CrossingMultiLineIsNodedInUnion) {
  PrecisionModel floating_pm;
  Geometry::Ptr pt = Read("POINT (5 5)");
  Geometry::Ptr lines = Read("MULTILINESTRING ((0 0, 2 2), (0 2, 2 0))");

  Geometry::Ptr uni = OverlayMixedPoints::overlay(OverlayNG::UNION, pt.get(),
                                                  lines.get(), &floating_pm);
  AssertGeomsEqual(uni.get(),
                   "GEOMETRYCOLLECTION ("
                   "LINESTRING (0 0, 1 1), LINESTRING (1 1, 2 2), "
                   "LINESTRING (0 2, 1 1), LINESTRING (1 1, 2 0), "
                   "POINT (5 5))");
}

TEST_F(OverlayMixedPointsTest, PreservesXYZAndDeduplicates2D) {
  PrecisionModel floating_pm;
  Geometry::Ptr pts = Read("MULTIPOINT Z ((5 5 30), (1 1 10), (1 1 20))");
  Geometry::Ptr poly = Read("POLYGON ((0 0, 3 0, 3 3, 0 3, 0 0))");

  Geometry::Ptr inter = OverlayMixedPoints::overlay(
      OverlayNG::INTERSECTION, pts.get(), poly.get(), &floating_pm);
  const auto* inter_pt = dynamic_cast<const Point*>(inter.get());
  ASSERT_NE(inter_pt, nullptr);
  EXPECT_TRUE(inter_pt->hasZ());
  EXPECT_FALSE(inter_pt->hasM());
  EXPECT_DOUBLE_EQ(inter_pt->getX(), 1.0);
  EXPECT_DOUBLE_EQ(inter_pt->getY(), 1.0);
  EXPECT_DOUBLE_EQ(inter_pt->getZ(), 10.0);
  EXPECT_TRUE(std::isnan(inter_pt->getM()));

  Geometry::Ptr diff = OverlayMixedPoints::overlay(
      OverlayNG::DIFFERENCE, pts.get(), poly.get(), &floating_pm);
  Geometry::Ptr expected_diff = Read("POINT Z (5 5 30)");
  EXPECT_TRUE(diff->equalsIdentical(expected_diff.get()));
}

TEST_F(OverlayMixedPointsTest, PreservesXYMAndXYZMOrdinates) {
  PrecisionModel fixed_pm(1.0);

  // XYM input with unsorted coordinates and fixed rounding.
  Geometry::Ptr xym_pts = Read("MULTIPOINT M ((5.2 4.8 200), (1.1 1.2 100))");
  Geometry::Ptr poly = Read("POLYGON ((0 0, 3 0, 3 3, 0 3, 0 0))");

  Geometry::Ptr xym_inter = OverlayMixedPoints::overlay(
      OverlayNG::INTERSECTION, xym_pts.get(), poly.get(), &fixed_pm);
  const auto* xym_pt = dynamic_cast<const Point*>(xym_inter.get());
  ASSERT_NE(xym_pt, nullptr);
  EXPECT_FALSE(xym_pt->hasZ());
  EXPECT_TRUE(xym_pt->hasM());
  EXPECT_DOUBLE_EQ(xym_pt->getX(), 1.0);
  EXPECT_DOUBLE_EQ(xym_pt->getY(), 1.0);
  EXPECT_TRUE(std::isnan(xym_pt->getZ()));
  EXPECT_DOUBLE_EQ(xym_pt->getM(), 100.0);

  Geometry::Ptr xym_diff = OverlayMixedPoints::overlay(
      OverlayNG::DIFFERENCE, xym_pts.get(), poly.get(), &fixed_pm);
  const auto* xym_diff_pt = dynamic_cast<const Point*>(xym_diff.get());
  ASSERT_NE(xym_diff_pt, nullptr);
  EXPECT_FALSE(xym_diff_pt->hasZ());
  EXPECT_TRUE(xym_diff_pt->hasM());
  EXPECT_DOUBLE_EQ(xym_diff_pt->getX(), 5.0);
  EXPECT_DOUBLE_EQ(xym_diff_pt->getY(), 5.0);
  EXPECT_DOUBLE_EQ(xym_diff_pt->getM(), 200.0);

  // XYZM input with 2D duplicate after rounding.
  Geometry::Ptr xyzm_pts = Read(
      "MULTIPOINT ZM ((5.1 5.1 30 300), (1.1 1.1 10 100), (1.2 1.2 20 200))");
  Geometry::Ptr line = Read("LINESTRING (0 0, 3 3)");

  Geometry::Ptr xyzm_inter = OverlayMixedPoints::overlay(
      OverlayNG::INTERSECTION, xyzm_pts.get(), line.get(), &fixed_pm);
  const auto* xyzm_pt = dynamic_cast<const Point*>(xyzm_inter.get());
  ASSERT_NE(xyzm_pt, nullptr);
  EXPECT_TRUE(xyzm_pt->hasZ());
  EXPECT_TRUE(xyzm_pt->hasM());
  EXPECT_DOUBLE_EQ(xyzm_pt->getX(), 1.0);
  EXPECT_DOUBLE_EQ(xyzm_pt->getY(), 1.0);
  EXPECT_DOUBLE_EQ(xyzm_pt->getZ(), 10.0);
  EXPECT_DOUBLE_EQ(xyzm_pt->getM(), 100.0);
}

TEST_F(OverlayMixedPointsTest, SupportsHomogeneousGeometryCollections) {
  PrecisionModel floating_pm;
  Geometry::Ptr point_coll = Read(
      "GEOMETRYCOLLECTION (POINT (5 5), POINT EMPTY, MULTIPOINT ((1 1), (1 "
      "1)))");
  Geometry::Ptr poly_coll = Read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "POLYGON ((8 0, 10 0, 10 2, 8 2, 8 0)))");

  Geometry::Ptr inter = OverlayMixedPoints::overlay(
      OverlayNG::INTERSECTION, point_coll.get(), poly_coll.get(), &floating_pm);
  AssertGeomsEqual(inter.get(), "POINT (1 1)");

  Geometry::Ptr uni = OverlayMixedPoints::overlay(
      OverlayNG::UNION, point_coll.get(), poly_coll.get(), &floating_pm);
  AssertGeomsEqual(uni.get(),
                   "GEOMETRYCOLLECTION ("
                   "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
                   "POLYGON ((8 0, 10 0, 10 2, 8 2, 8 0)), "
                   "POINT (5 5))");
}

TEST_F(OverlayMixedPointsTest, ThrowsOnUnknownOverlayOpCode) {
  PrecisionModel floating_pm;
  Geometry::Ptr pt = Read("POINT (1 1)");
  Geometry::Ptr poly = Read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");

  try {
    OverlayMixedPoints::overlay(99, pt.get(), poly.get(), &floating_pm);
    FAIL() << "Expected AssertionFailedException for unknown overlay op code";
  } catch (const AssertionFailedException& e) {
    EXPECT_NE(std::string_view(e.what()).find("Unknown overlay op code"),
              std::string_view::npos);
  }
}

}  // namespace
}  // namespace geos_google
