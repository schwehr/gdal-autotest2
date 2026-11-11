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

// Unit tests for third_party/geos/src/algorithm/MinimumAreaRectangle.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - MinimumAreaRectangle::getMinimumRectangle() (0-arg instance method) is
//   private in upstream GEOS, so the public entry point is the static
//   MinimumAreaRectangle::getMinimumRectangle(const Geometry*) method.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/MinimumAreaRectangle.cpp

#include "third_party/geos/include/geos/algorithm/MinimumAreaRectangle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/MinimumDiameter.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/CoordinateSequenceFilter.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::MinimumAreaRectangle;
using geos::algorithm::MinimumDiameter;
using geos::algorithm::Orientation;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateSequenceFilter;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::util::UnsupportedOperationException;

class VertexDistanceChecker : public CoordinateSequenceFilter {
 public:
  VertexDistanceChecker(const Geometry* rect, const GeometryFactory* factory,
                        double dist_tol)
      : rect_(rect), factory_(factory), dist_tol_(dist_tol) {}

  void filter_ro(const CoordinateSequence& seq, std::size_t i) override {
    const CoordinateXY& p = seq.getAt<CoordinateXY>(i);
    std::unique_ptr<Point> pt = factory_->createPoint(p);
    EXPECT_LE(rect_->distance(pt.get()), dist_tol_)
        << "Vertex (" << p.x << ", " << p.y << ") outside enclosing rectangle";
  }

  bool isDone() const override { return false; }
  bool isGeometryChanged() const override { return false; }

 private:
  const Geometry* rect_;
  const GeometryFactory* factory_;
  double dist_tol_;
};

class MinimumAreaRectangleTest : public ::testing::Test {
 protected:
  void ExpectRectangleGeometry(const Geometry* rect, const Geometry* input,
                               double expected_area, double area_tol = 1e-9,
                               double dist_tol = 1e-11) const {
    ASSERT_NE(rect, nullptr);
    EXPECT_EQ(rect->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    EXPECT_FALSE(rect->isEmpty());
    EXPECT_TRUE(rect->isValid());
    EXPECT_FALSE(rect->hasZ());
    EXPECT_FALSE(rect->hasM());
    EXPECT_EQ(rect->getCoordinateDimension(), 2);

    const auto* poly = dynamic_cast<const Polygon*>(rect);
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 0u);

    const LinearRing* shell = poly->getExteriorRing();
    ASSERT_NE(shell, nullptr);
    const CoordinateSequence* ring = shell->getCoordinatesRO();
    ASSERT_NE(ring, nullptr);
    ASSERT_EQ(ring->size(), 5u);

    const CoordinateXY& c0 = ring->getAt<CoordinateXY>(0);
    const CoordinateXY& c4 = ring->getAt<CoordinateXY>(4);
    EXPECT_DOUBLE_EQ(c0.x, c4.x);
    EXPECT_DOUBLE_EQ(c0.y, c4.y);

    // Rectangle::createFromSidePts constructs exterior rings clockwise.
    EXPECT_FALSE(Orientation::isCCW(ring));

    std::array<double, 4> dx{};
    std::array<double, 4> dy{};
    std::array<double, 4> len{};
    for (std::size_t i = 0; i < 4; ++i) {
      const CoordinateXY& p = ring->getAt<CoordinateXY>(i);
      const CoordinateXY& q = ring->getAt<CoordinateXY>(i + 1);
      dx[i] = q.x - p.x;
      dy[i] = q.y - p.y;
      len[i] = std::hypot(dx[i], dy[i]);
      ASSERT_GT(len[i], 0.0);
    }
    EXPECT_NEAR(len[0], len[2], dist_tol * std::max(1.0, len[0]));
    EXPECT_NEAR(len[1], len[3], dist_tol * std::max(1.0, len[1]));
    for (std::size_t i = 0; i < 4; ++i) {
      const std::size_t j = (i + 1) % 4;
      const double cos_angle =
          (dx[i] * dx[j] + dy[i] * dy[j]) / (len[i] * len[j]);
      EXPECT_NEAR(cos_angle, 0.0, 1e-11);
    }

    EXPECT_NEAR(poly->getArea(), expected_area, area_tol);

    if (input != nullptr && !input->isEmpty()) {
      VertexDistanceChecker checker(rect, factory_.get(), dist_tol);
      input->apply_ro(checker);
    }
  }

  void ExpectNormalizedMatch(const Geometry* actual,
                             std::string_view expected_wkt,
                             double tol = 1e-9) const {
    ASSERT_NE(actual, nullptr);
    std::unique_ptr<Geometry> actual_norm = actual->clone();
    actual_norm->normalize();
    std::unique_ptr<Geometry> expected =
        reader_.read(std::string(expected_wkt));
    ASSERT_NE(expected, nullptr);
    expected->normalize();
    EXPECT_TRUE(actual_norm->equalsExact(expected.get(), tol))
        << "Actual:   " << actual_norm->toText()
        << "\nExpected: " << expected->toText();
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

struct EmptyCase {
  std::string_view name;
  std::string_view wkt;
};

constexpr auto kEmptyCases = std::to_array<EmptyCase>({
    {.name = "empty_point", .wkt = "POINT EMPTY"},
    {.name = "empty_linestring", .wkt = "LINESTRING EMPTY"},
    {.name = "empty_polygon", .wkt = "POLYGON EMPTY"},
    {.name = "empty_multipoint", .wkt = "MULTIPOINT EMPTY"},
    {.name = "empty_multilinestring", .wkt = "MULTILINESTRING EMPTY"},
    {.name = "empty_multipolygon", .wkt = "MULTIPOLYGON EMPTY"},
    {.name = "empty_collection", .wkt = "GEOMETRYCOLLECTION EMPTY"},
    {
        .name = "collection_of_empties",
        .wkt =
            "GEOMETRYCOLLECTION (POINT EMPTY, LINESTRING EMPTY, POLYGON EMPTY)",
    },
    {.name = "empty_circularstring", .wkt = "CIRCULARSTRING EMPTY"},
});

TEST_F(MinimumAreaRectangleTest, EmptyGeometriesReturnEmptyPolygon) {
  for (const EmptyCase& tc : kEmptyCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> input = reader_.read(std::string(tc.wkt));
    ASSERT_NE(input, nullptr);

    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(input.get());
    ASSERT_NE(rect, nullptr);
    EXPECT_EQ(rect->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    EXPECT_TRUE(rect->isEmpty());
    EXPECT_DOUBLE_EQ(rect->getArea(), 0.0);
    EXPECT_EQ(rect->getFactory(), factory_.get());
  }
}

struct PointCase {
  std::string_view name;
  std::string_view wkt;
  double expected_x;
  double expected_y;
};

constexpr auto kSinglePointCases = std::to_array<PointCase>({
    {
        .name = "single_point",
        .wkt = "POINT (3.5 -7.25)",
        .expected_x = 3.5,
        .expected_y = -7.25,
    },
    {
        .name = "duplicate_multipoint",
        .wkt = "MULTIPOINT ((2 4), (2 4), (2 4))",
        .expected_x = 2.0,
        .expected_y = 4.0,
    },
    {
        .name = "collapsed_linestring",
        .wkt = "LINESTRING (1.5 2.5, 1.5 2.5, 1.5 2.5)",
        .expected_x = 1.5,
        .expected_y = 2.5,
    },
    {
        .name = "collection_of_identical_points",
        .wkt = "GEOMETRYCOLLECTION (POINT (-1 8), LINESTRING (-1 8, -1 8))",
        .expected_x = -1.0,
        .expected_y = 8.0,
    },
});

TEST_F(MinimumAreaRectangleTest, SingleUniqueCoordinateReturnsPoint) {
  for (const PointCase& tc : kSinglePointCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> input = reader_.read(std::string(tc.wkt));
    ASSERT_NE(input, nullptr);

    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(input.get());
    ASSERT_NE(rect, nullptr);
    EXPECT_EQ(rect->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
    EXPECT_FALSE(rect->isEmpty());

    const auto* pt = dynamic_cast<const Point*>(rect.get());
    ASSERT_NE(pt, nullptr);
    EXPECT_DOUBLE_EQ(pt->getX(), tc.expected_x);
    EXPECT_DOUBLE_EQ(pt->getY(), tc.expected_y);
  }
}

struct LineCase {
  std::string_view name;
  std::string_view input_wkt;
  std::string_view expected_wkt;
};

constexpr auto kCollinearCases = std::to_array<LineCase>({
    {
        .name = "two_point_positive_slope_increasing",
        .input_wkt = "LINESTRING (1 2, 5 6)",
        .expected_wkt = "LINESTRING (1 2, 5 6)",
    },
    {
        .name = "two_point_positive_slope_decreasing",
        .input_wkt = "LINESTRING (5 6, 1 2)",
        .expected_wkt = "LINESTRING (1 2, 5 6)",
    },
    {
        .name = "two_point_negative_slope_left_to_right",
        .input_wkt = "LINESTRING (1 6, 5 2)",
        .expected_wkt = "LINESTRING (1 6, 5 2)",
    },
    {
        .name = "two_point_negative_slope_right_to_left",
        .input_wkt = "LINESTRING (5 2, 1 6)",
        .expected_wkt = "LINESTRING (1 6, 5 2)",
    },
    {
        .name = "horizontal_collinear_multipoint",
        .input_wkt = "MULTIPOINT ((10 3), (-2 3), (4 3), (7 3))",
        .expected_wkt = "LINESTRING (-2 3, 10 3)",
    },
    {
        .name = "vertical_two_point_ascending",
        .input_wkt = "LINESTRING (4 1, 4 9)",
        .expected_wkt = "LINESTRING (4 1, 4 9)",
    },
    {
        .name = "vertical_two_point_descending",
        .input_wkt = "LINESTRING (4 9, 4 1)",
        .expected_wkt = "LINESTRING (4 1, 4 9)",
    },
    {
        .name = "vertical_collinear_linestring",
        .input_wkt = "LINESTRING (3 5, 3 -2, 3 10, 3 1)",
        .expected_wkt = "LINESTRING (3 -2, 3 10)",
    },
    {
        .name = "diagonal_collinear_collection",
        .input_wkt = "GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (2 4, 4 8), "
                     "POINT (1 2))",
        .expected_wkt = "LINESTRING (0 0, 4 8)",
    },
});

TEST_F(MinimumAreaRectangleTest, CollinearAndTwoPointInputsReturnMaximumLine) {
  for (const LineCase& tc : kCollinearCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> input = reader_.read(std::string(tc.input_wkt));
    ASSERT_NE(input, nullptr);

    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(input.get());
    ASSERT_NE(rect, nullptr);
    EXPECT_EQ(rect->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);

    const auto* line = dynamic_cast<const LineString*>(rect.get());
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(line->getNumPoints(), 2u);

    std::unique_ptr<Geometry> expected =
        reader_.read(std::string(tc.expected_wkt));
    ASSERT_NE(expected, nullptr);
    EXPECT_TRUE(rect->equalsExact(expected.get(), 1e-12))
        << "Actual: " << rect->toText() << "\nExpected: " << expected->toText();
  }
}

struct PolygonCase {
  std::string_view name;
  std::string_view input_wkt;
  std::string_view expected_wkt;
  double expected_area;
};

constexpr auto kAxisAlignedCases = std::to_array<PolygonCase>({
    {
        .name = "axis_aligned_rectangle",
        .input_wkt = "POLYGON ((1 2, 6 2, 6 5, 1 5, 1 2))",
        .expected_wkt = "POLYGON ((1 2, 6 2, 6 5, 1 5, 1 2))",
        .expected_area = 15.0,
    },
    {
        .name = "axis_aligned_square",
        .input_wkt = "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
        .expected_wkt = "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
        .expected_area = 16.0,
    },
    {
        .name = "linear_ring_input",
        .input_wkt = "LINEARRING (0 0, 3 0, 3 2, 0 2, 0 0)",
        .expected_wkt = "POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0))",
        .expected_area = 6.0,
    },
    {
        .name = "negative_coordinates_rectangle",
        .input_wkt = "POLYGON ((-10 -20, -6 -20, -6 -17, -10 -17, -10 -20))",
        .expected_wkt = "POLYGON ((-10 -20, -6 -20, -6 -17, -10 -17, -10 -20))",
        .expected_area = 12.0,
    },
    {
        .name = "non_convex_l_shape",
        .input_wkt = "POLYGON ((0 0, 6 0, 6 2, 2 2, 2 4, 0 4, 0 0))",
        .expected_wkt = "POLYGON ((0 0, 6 0, 6 4, 0 4, 0 0))",
        .expected_area = 24.0,
    },
});

TEST_F(MinimumAreaRectangleTest, AxisAlignedAndShiftedGeometries) {
  for (const PolygonCase& tc : kAxisAlignedCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> input = reader_.read(std::string(tc.input_wkt));
    ASSERT_NE(input, nullptr);

    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(input.get());
    ExpectRectangleGeometry(rect.get(), input.get(), tc.expected_area);
    ExpectNormalizedMatch(rect.get(), tc.expected_wkt);
  }
}

constexpr auto kRotatedCases = std::to_array<PolygonCase>({
    {
        .name = "diamond_45_degrees",
        .input_wkt = "POLYGON ((0 1, 1 0, 2 1, 1 2, 0 1))",
        .expected_wkt = "POLYGON ((0 1, 1 0, 2 1, 1 2, 0 1))",
        .expected_area = 2.0,
    },
    {
        .name = "rotated_3_4_5_open_c_shape",
        .input_wkt = "LINESTRING (0 0, 30 40, 22 46, -8 6)",
        .expected_wkt = "POLYGON ((-8 6, 0 0, 30 40, 22 46, -8 6))",
        .expected_area = 500.0,
    },
    {
        .name = "translated_rotated_3_4_5_rectangle",
        .input_wkt = "POLYGON ((-1000 2500, -970 2540, -978 2546, -1008 2506, "
                     "-1000 2500))",
        .expected_wkt = "POLYGON ((-1000 2500, -970 2540, -978 2546, -1008 "
                        "2506, -1000 2500))",
        .expected_area = 500.0,
    },
});

TEST_F(MinimumAreaRectangleTest,
       RotatedRectanglesRecoverExactOrientationAndSmallerArea) {
  for (const PolygonCase& tc : kRotatedCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> input = reader_.read(std::string(tc.input_wkt));
    ASSERT_NE(input, nullptr);

    std::unique_ptr<Geometry> env = input->getEnvelope();
    ASSERT_NE(env, nullptr);
    EXPECT_LT(tc.expected_area, env->getArea());

    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(input.get());
    ExpectRectangleGeometry(rect.get(), input.get(), tc.expected_area);
    ExpectNormalizedMatch(rect.get(), tc.expected_wkt);
  }
}

TEST_F(MinimumAreaRectangleTest, TrianglesExerciseMinimumAreaEdgeSelection) {
  {
    SCOPED_TRACE("right_triangle");
    std::unique_ptr<Geometry> input =
        reader_.read("POLYGON ((0 0, 3 0, 0 4, 0 0))");
    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(input.get());
    // Every edge-aligned enclosing rectangle has area 2 * triangle_area = 12.
    ExpectRectangleGeometry(rect.get(), input.get(), 12.0);
  }
  {
    SCOPED_TRACE("obtuse_triangle_later_edge_is_minimum");
    std::unique_ptr<Geometry> input =
        reader_.read("POLYGON ((0 0, -2 1, -5 5, 0 0))");
    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(input.get());
    // Longest edge (-5,5)-(0,0) has line x+y=0, opposite vertex (-2,1) on
    // x+y=-1, giving area 5.0 (smaller than 15.0 and 7.0 on earlier edges).
    ExpectRectangleGeometry(rect.get(), input.get(), 5.0);
    ExpectNormalizedMatch(rect.get(),
                          "POLYGON ((-5 5, 0 0, -0.5 -0.5, -5.5 4.5, -5 5))");
  }
  {
    SCOPED_TRACE("obtuse_triangle_reversed_winding");
    std::unique_ptr<Geometry> input =
        reader_.read("POLYGON ((0 0, -5 5, -2 1, 0 0))");
    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(input.get());
    ExpectRectangleGeometry(rect.get(), input.get(), 5.0);
    ExpectNormalizedMatch(rect.get(),
                          "POLYGON ((-5 5, 0 0, -0.5 -0.5, -5.5 4.5, -5 5))");
  }
  {
    SCOPED_TRACE("obtuse_triangle_first_edge_is_minimum");
    std::unique_ptr<Geometry> input =
        reader_.read("POLYGON ((0 0, 10 0, 12 1, 0 0))");
    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(input.get());
    ExpectRectangleGeometry(rect.get(), input.get(), 10.0);
  }
}

TEST_F(MinimumAreaRectangleTest, MinimumAreaDiffersFromMinimumWidthRectangle) {
  // For quadrilateral (0,0)-(10,0)-(8,1)-(0,3):
  // - Axis-aligned box [0,10]x[0,3] has area 30.0 and width 3.0.
  // - Box aligned to (8,1)-(0,3) has smaller width 12/sqrt(17) (~2.9104),
  //   but larger area 516/17 (~30.3529).
  std::unique_ptr<Geometry> input =
      reader_.read("POLYGON ((0 0, 10 0, 8 1, 0 3, 0 0))");
  ASSERT_NE(input, nullptr);

  std::unique_ptr<Geometry> mar =
      MinimumAreaRectangle::getMinimumRectangle(input.get());
  ExpectRectangleGeometry(mar.get(), input.get(), 30.0);
  ExpectNormalizedMatch(mar.get(), "POLYGON ((0 0, 10 0, 10 3, 0 3, 0 0))");

  MinimumDiameter md(input.get());
  std::unique_ptr<Geometry> md_rect = md.getMinimumRectangle();
  ASSERT_NE(md_rect, nullptr);
  EXPECT_NEAR(md.getLength(), 12.0 / std::sqrt(17.0), 1e-12);
  EXPECT_LT(md.getLength(), 3.0);
  EXPECT_NEAR(md_rect->getArea(), 516.0 / 17.0, 1e-9);
  EXPECT_LT(mar->getArea(), md_rect->getArea());
}

constexpr auto kComplexShapeCases = std::to_array<PolygonCase>({
    {
        .name = "multipoint_with_interior_points",
        .input_wkt = "MULTIPOINT ((0 0), (2 1), (4 0), (4 3), (0 3), (1 2))",
        .expected_wkt = "POLYGON ((0 0, 4 0, 4 3, 0 3, 0 0))",
        .expected_area = 12.0,
    },
    {
        .name = "multilinestring_cross",
        .input_wkt = "MULTILINESTRING ((0 0, 2 2), (2 0, 0 2))",
        .expected_wkt = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
        .expected_area = 4.0,
    },
    {
        .name = "multipolygon_disjoint_squares",
        .input_wkt = "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), ((3 0, 4 0, 4 "
                     "1, 3 1, 3 0)))",
        .expected_wkt = "POLYGON ((0 0, 4 0, 4 1, 0 1, 0 0))",
        .expected_area = 4.0,
    },
    {
        .name = "polygon_with_hole",
        .input_wkt =
            "POLYGON ((0 0, 5 0, 5 4, 0 4, 0 0), (1 1, 1 3, 4 3, 4 1, 1 1))",
        .expected_wkt = "POLYGON ((0 0, 5 0, 5 4, 0 4, 0 0))",
        .expected_area = 20.0,
    },
    {
        .name = "self_intersecting_bowtie_polygon",
        .input_wkt = "POLYGON ((0 0, 2 2, 2 0, 0 2, 0 0))",
        .expected_wkt = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
        .expected_area = 4.0,
    },
    {
        .name = "mixed_geometry_collection_with_empty",
        .input_wkt = "GEOMETRYCOLLECTION (POINT EMPTY, POINT (0 0), LINESTRING "
                     "(1 1, 4 0), "
                     "POLYGON ((2 1, 3 1, 3 3, 2 3, 2 1)))",
        .expected_wkt = "POLYGON ((0 0, 4 0, 4 3, 0 3, 0 0))",
        .expected_area = 12.0,
    },
});

TEST_F(MinimumAreaRectangleTest,
       MultiGeometriesHolesSelfIntersectionAndCollections) {
  for (const PolygonCase& tc : kComplexShapeCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> input = reader_.read(std::string(tc.input_wkt));
    ASSERT_NE(input, nullptr);

    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(input.get());
    ExpectRectangleGeometry(rect.get(), input.get(), tc.expected_area);
    ExpectNormalizedMatch(rect.get(), tc.expected_wkt);
  }
}

TEST_F(MinimumAreaRectangleTest,
       LargeConvexAndNonConvexPolygonsOverReduceThreshold) {
  {
    SCOPED_TRACE("regular_64_gon");
    constexpr std::size_t kSides = 64;
    constexpr double kRadius = 2.0;
    constexpr double kCx = 10.0;
    constexpr double kCy = -5.0;

    CoordinateSequence seq = CoordinateSequence::XY(kSides + 1);
    for (std::size_t i = 0; i < kSides; ++i) {
      const double angle = (2.0 * std::numbers::pi * static_cast<double>(i)) /
                           static_cast<double>(kSides);
      seq.setAt(CoordinateXY(kCx + kRadius * std::cos(angle),
                             kCy + kRadius * std::sin(angle)),
                i);
    }
    seq.setAt(seq.getAt<CoordinateXY>(0), kSides);
    std::unique_ptr<Polygon> poly = factory_->createPolygon(std::move(seq));

    const double apothem =
        kRadius * std::cos(std::numbers::pi / static_cast<double>(kSides));
    const double expected_area = 4.0 * apothem * apothem;

    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(poly.get());
    ExpectRectangleGeometry(rect.get(), poly.get(), expected_area, 1e-9, 1e-10);
    EXPECT_LT(rect->getArea(), poly->getEnvelope()->getArea());
  }
  {
    SCOPED_TRACE("non_convex_64_point_star");
    constexpr std::size_t kVertices = 64;
    constexpr double kOuterRadius = 3.0;
    constexpr double kInnerRadius = 0.5;

    CoordinateSequence seq = CoordinateSequence::XY(kVertices + 1);
    for (std::size_t i = 0; i < kVertices; ++i) {
      const double angle = (2.0 * std::numbers::pi * static_cast<double>(i)) /
                           static_cast<double>(kVertices);
      const double r = (i % 2 == 0) ? kOuterRadius : kInnerRadius;
      seq.setAt(CoordinateXY(r * std::cos(angle), r * std::sin(angle)), i);
    }
    seq.setAt(seq.getAt<CoordinateXY>(0), kVertices);
    std::unique_ptr<Polygon> star = factory_->createPolygon(std::move(seq));

    // The 32 outer points form a regular 32-gon of radius 3.0.
    const double apothem = kOuterRadius * std::cos(std::numbers::pi / 32.0);
    const double expected_area = 4.0 * apothem * apothem;

    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(star.get());
    ExpectRectangleGeometry(rect.get(), star.get(), expected_area, 1e-9, 1e-10);
  }
}

TEST_F(MinimumAreaRectangleTest, NearCollinearFloatingPointPrecisionEdgeCase) {
  // Orientation::index (double-double) distinguishes (1, 0.3) from the line
  // (0,0)-(1e8, 3e7), so ConvexHull returns a non-collinear Polygon while
  // double-precision perpendicular distance underflows to 0.0, exercising
  // findFurthestVertex's full-ring wrap guard.
  std::unique_ptr<Geometry> input =
      reader_.read("POLYGON ((0 0, 1 0.3, 100000000 30000000, 0 0))");
  ASSERT_NE(input, nullptr);

  std::unique_ptr<Geometry> rect =
      MinimumAreaRectangle::getMinimumRectangle(input.get());
  ASSERT_NE(rect, nullptr);
  EXPECT_EQ(rect->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_EQ(rect->getNumPoints(), 5u);

  const auto* poly = dynamic_cast<const Polygon*>(rect.get());
  ASSERT_NE(poly, nullptr);
  const CoordinateSequence* ring = poly->getExteriorRing()->getCoordinatesRO();
  for (std::size_t i = 0; i < ring->size(); ++i) {
    const CoordinateXY& c = ring->getAt<CoordinateXY>(i);
    EXPECT_TRUE(std::isfinite(c.x));
    EXPECT_TRUE(std::isfinite(c.y));
  }
  EXPECT_GE(rect->getArea(), 0.0);
  EXPECT_LT(rect->getArea(), 1e-3);
}

TEST_F(MinimumAreaRectangleTest, DropsZAndMCoordinates) {
  constexpr auto kZmCases = std::to_array<EmptyCase>({
      {.name = "point_z", .wkt = "POINT Z (1 2 99)"},
      {.name = "point_m", .wkt = "POINT M (1 2 42)"},
      {.name = "linestring_zm", .wkt = "LINESTRING ZM (0 0 10 20, 3 4 30 40)"},
      {
          .name = "polygon_z",
          .wkt = "POLYGON Z ((0 0 1, 2 0 2, 2 3 3, 0 3 4, 0 0 1))",
      },
  });

  for (const EmptyCase& tc : kZmCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> input = reader_.read(std::string(tc.wkt));
    ASSERT_NE(input, nullptr);

    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(input.get());
    ASSERT_NE(rect, nullptr);
    EXPECT_FALSE(rect->hasZ());
    EXPECT_FALSE(rect->hasM());
    EXPECT_EQ(rect->getCoordinateDimension(), 2);
  }
}

TEST_F(MinimumAreaRectangleTest, PreservesCustomFactoryAndSrid) {
  PrecisionModel pm(100.0);
  GeometryFactory::Ptr custom_factory = GeometryFactory::create(&pm, 4326);
  WKTReader custom_reader(*custom_factory);

  constexpr auto kWkts = std::to_array<std::string_view>({
      "POINT EMPTY",
      "POINT (1 2)",
      "LINESTRING (0 0, 3 4)",
      "POLYGON ((0 0, 2 0, 2 5, 0 5, 0 0))",
  });

  for (std::string_view wkt : kWkts) {
    SCOPED_TRACE(std::string(wkt));
    std::unique_ptr<Geometry> input = custom_reader.read(std::string(wkt));
    ASSERT_NE(input, nullptr);

    std::unique_ptr<Geometry> rect =
        MinimumAreaRectangle::getMinimumRectangle(input.get());
    ASSERT_NE(rect, nullptr);
    EXPECT_EQ(rect->getFactory(), custom_factory.get());
    EXPECT_EQ(rect->getSRID(), 4326);
  }
}

TEST_F(MinimumAreaRectangleTest,
       CurvedGeometryThrowsUnsupportedOperationException) {
  std::unique_ptr<Geometry> circular =
      reader_.read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  ASSERT_NE(circular, nullptr);
  EXPECT_THROW(MinimumAreaRectangle::getMinimumRectangle(circular.get()),
               UnsupportedOperationException);

  std::unique_ptr<Geometry> compound = reader_.read(
      "COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 0, 3 1))");
  ASSERT_NE(compound, nullptr);
  EXPECT_THROW(MinimumAreaRectangle::getMinimumRectangle(compound.get()),
               UnsupportedOperationException);
}

TEST_F(MinimumAreaRectangleTest, PublicConstructorsCompile) {
  static_assert(std::is_constructible_v<MinimumAreaRectangle, const Geometry*>);
  static_assert(
      std::is_constructible_v<MinimumAreaRectangle, const Geometry*, bool>);

  std::unique_ptr<Geometry> poly =
      reader_.read("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  ASSERT_NE(poly, nullptr);

  [[maybe_unused]] MinimumAreaRectangle mar_default(poly.get());
  [[maybe_unused]] MinimumAreaRectangle mar_convex(poly.get(), true);
  [[maybe_unused]] MinimumAreaRectangle mar_non_convex(poly.get(), false);
}

}  // namespace
}  // namespace geos_google
