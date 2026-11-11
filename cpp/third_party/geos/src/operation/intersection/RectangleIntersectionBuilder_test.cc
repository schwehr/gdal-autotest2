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
// third_party/geos/src/operation/intersection/RectangleIntersectionBuilder.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - RectangleIntersectionBuilder has a private constructor and private methods
//   friended exclusively to RectangleIntersection, so its behavior is tested
//   through RectangleIntersection::clip() and clipBoundary().
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/intersection/RectangleIntersectionBuilder.cpp

#include "third_party/geos/include/geos/operation/intersection/RectangleIntersectionBuilder.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/intersection/Rectangle.h"
#include "third_party/geos/include/geos/operation/intersection/RectangleIntersection.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateSequence;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryFactory;
using ::geos::geom::GeometryTypeId;
using ::geos::geom::LinearRing;
using ::geos::geom::Polygon;
using ::geos::geom::PrecisionModel;
using ::geos::io::WKTReader;
using ::geos::operation::intersection::Rectangle;
using ::geos::operation::intersection::RectangleIntersection;
using ::geos::operation::intersection::RectangleIntersectionBuilder;
using ::geos::util::IllegalArgumentException;
using ::geos::util::UnsupportedOperationException;
using ::testing::HasSubstr;

static_assert(!std::is_default_constructible_v<RectangleIntersectionBuilder>);
static_assert(!std::is_constructible_v<RectangleIntersectionBuilder,
                                       const GeometryFactory&>);
static_assert(std::is_destructible_v<RectangleIntersectionBuilder>);
static_assert(!std::is_copy_assignable_v<RectangleIntersectionBuilder>);
static_assert(!std::is_move_assignable_v<RectangleIntersectionBuilder>);

struct ClipCase {
  std::string_view name;
  std::string_view input_wkt;
  std::string_view expected_wkt;
  GeometryTypeId expected_type;
  double expected_area = -1.0;
};

constexpr double kTolerance = 1e-9;

void ExpectNormalizedEqual(Geometry& actual, Geometry& expected,
                           GeometryTypeId expected_type,
                           double expected_area = -1.0) {
  EXPECT_EQ(actual.getGeometryTypeId(), expected_type);
  EXPECT_EQ(actual.isEmpty(), expected.isEmpty());
  if (!actual.isEmpty()) {
    EXPECT_TRUE(actual.isValid()) << "invalid result: " << actual.toText();
  }
  actual.normalize();
  expected.normalize();
  EXPECT_TRUE(actual.equalsExact(&expected, kTolerance))
      << "actual:   " << actual.toText() << "\nexpected: " << expected.toText();
  if (expected_area >= 0.0) {
    EXPECT_NEAR(actual.getArea(), expected_area, kTolerance);
  }
}

void RunClipCases(const auto& cases, const Rectangle& rect,
                  bool clip_boundary = false) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : cases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> input = reader.read(std::string(tc.input_wkt));
    std::unique_ptr<Geometry> expected =
        reader.read(std::string(tc.expected_wkt));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(expected, nullptr);

    std::unique_ptr<Geometry> actual =
        clip_boundary ? RectangleIntersection::clipBoundary(*input, rect)
                      : RectangleIntersection::clip(*input, rect);
    ASSERT_NE(actual, nullptr);
    ExpectNormalizedEqual(*actual, *expected, tc.expected_type,
                          tc.expected_area);
  }
}

TEST(RectangleIntersectionBuilderTest, BuildEmptyAndHomogeneousGeometries) {
  constexpr auto kBuildCases = std::to_array<ClipCase>({
      {
          .name = "empty_polygon",
          .input_wkt = "POLYGON EMPTY",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
          .expected_type = ::geos::geom::GEOS_GEOMETRYCOLLECTION,
          .expected_area = 0.0,
      },
      {
          .name = "disjoint_polygon",
          .input_wkt = "POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20))",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
          .expected_type = ::geos::geom::GEOS_GEOMETRYCOLLECTION,
          .expected_area = 0.0,
      },
      {
          .name = "point_on_edge_excluded",
          .input_wkt = "POINT (0 5)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
          .expected_type = ::geos::geom::GEOS_GEOMETRYCOLLECTION,
      },
      {
          .name = "single_point_inside",
          .input_wkt = "POINT (5 5)",
          .expected_wkt = "POINT (5 5)",
          .expected_type = ::geos::geom::GEOS_POINT,
      },
      {
          .name = "multipoint_filtered",
          .input_wkt = "MULTIPOINT ((2 2), (5 5), (20 20), (0 5))",
          .expected_wkt = "MULTIPOINT ((2 2), (5 5))",
          .expected_type = ::geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "single_linestring",
          .input_wkt = "LINESTRING (-5 5, 15 5)",
          .expected_wkt = "LINESTRING (0 5, 10 5)",
          .expected_type = ::geos::geom::GEOS_LINESTRING,
      },
      {
          .name = "multilinestring",
          .input_wkt = "MULTILINESTRING ((-5 2, 15 2), (-5 8, 15 8))",
          .expected_wkt = "MULTILINESTRING ((0 2, 10 2), (0 8, 10 8))",
          .expected_type = ::geos::geom::GEOS_MULTILINESTRING,
      },
      {
          .name = "contained_polygon",
          .input_wkt = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
          .expected_wkt = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 36.0,
      },
      {
          .name = "enclosing_polygon",
          .input_wkt = "POLYGON ((-4 -4, 14 -4, 14 14, -4 14, -4 -4))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 100.0,
      },
  });

  RunClipCases(kBuildCases, Rectangle(0.0, 0.0, 10.0, 10.0));
}

TEST(RectangleIntersectionBuilderTest, BuildMixedGeometryCollection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Rectangle rect(0.0, 0.0, 10.0, 10.0);

  std::unique_ptr<Geometry> input = reader.read(
      "GEOMETRYCOLLECTION ("
      "POINT (5 5), "
      "POINT (20 20), "
      "LINESTRING (-2 3, 12 3), "
      "POLYGON ((-2 6, 4 6, 4 9, -2 9, -2 6)))");
  std::unique_ptr<Geometry> expected = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 6, 4 6, 4 9, 0 9, 0 6)), "
      "LINESTRING (0 3, 10 3), "
      "POINT (5 5))");
  ASSERT_NE(input, nullptr);
  ASSERT_NE(expected, nullptr);

  std::unique_ptr<Geometry> actual = RectangleIntersection::clip(*input, rect);
  ASSERT_NE(actual, nullptr);
  EXPECT_EQ(actual->getNumGeometries(), 3u);
  ExpectNormalizedEqual(*actual, *expected,
                        ::geos::geom::GEOS_GEOMETRYCOLLECTION, 12.0);
}

TEST(RectangleIntersectionBuilderTest, ReconnectsSplitRingStartingInside) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Rectangle rect(0.0, 0.0, 10.0, 10.0);

  // Vertex (4 5) is inside rect; the ring exits Left at (0 3) and re-enters
  // Left at (0 7), producing two segments whose start/end meet at (4 5).
  constexpr std::string_view kCwWkt = "POLYGON ((4 5, -4 1, -4 9, 4 5))";
  constexpr std::string_view kCcwWkt = "POLYGON ((4 5, -4 9, -4 1, 4 5))";

  for (std::string_view wkt : {kCwWkt, kCcwWkt}) {
    SCOPED_TRACE(std::string(wkt));
    std::unique_ptr<Geometry> poly = reader.read(std::string(wkt));
    ASSERT_NE(poly, nullptr);

    std::unique_ptr<Geometry> boundary =
        RectangleIntersection::clipBoundary(*poly, rect);
    ASSERT_NE(boundary, nullptr);
    EXPECT_EQ(boundary->getGeometryTypeId(), ::geos::geom::GEOS_LINESTRING);
    EXPECT_EQ(boundary->getNumGeometries(), 1u);
    std::unique_ptr<Geometry> expected_line =
        reader.read("LINESTRING (0 7, 4 5, 0 3)");
    ExpectNormalizedEqual(*boundary, *expected_line,
                          ::geos::geom::GEOS_LINESTRING);

    std::unique_ptr<Geometry> clipped =
        RectangleIntersection::clip(*poly, rect);
    ASSERT_NE(clipped, nullptr);
    std::unique_ptr<Geometry> expected_poly =
        reader.read("POLYGON ((0 3, 0 7, 4 5, 0 3))");
    ExpectNormalizedEqual(*clipped, *expected_poly, ::geos::geom::GEOS_POLYGON,
                          8.0);
  }
}

TEST(RectangleIntersectionBuilderTest,
     ReconnectLeavesDisjointLinesStartingOutsideSeparate) {
  constexpr auto kDisjointCases = std::to_array<ClipCase>({
      {
          .name = "u_shape_clip_polygons",
          .input_wkt =
              "POLYGON ((-4 1, 4 1, 4 3, -2 3, -2 7, 4 7, 4 9, -4 9, -4 1))",
          .expected_wkt = "MULTIPOLYGON ("
                          "((0 1, 0 3, 4 3, 4 1, 0 1)), "
                          "((0 7, 0 9, 4 9, 4 7, 0 7)))",
          .expected_type = ::geos::geom::GEOS_MULTIPOLYGON,
          .expected_area = 16.0,
      },
  });
  RunClipCases(kDisjointCases, Rectangle(0.0, 0.0, 10.0, 10.0));

  constexpr auto kDisjointBoundaryCases = std::to_array<ClipCase>({
      {
          .name = "u_shape_clip_boundary",
          .input_wkt =
              "POLYGON ((-4 1, 4 1, 4 3, -2 3, -2 7, 4 7, 4 9, -4 9, -4 1))",
          .expected_wkt =
              "MULTILINESTRING ((0 1, 4 1, 4 3, 0 3), (0 7, 4 7, 4 9, 0 9))",
          .expected_type = ::geos::geom::GEOS_MULTILINESTRING,
      },
  });
  RunClipCases(kDisjointBoundaryCases, Rectangle(0.0, 0.0, 10.0, 10.0),
               /*clip_boundary=*/true);
}

TEST(RectangleIntersectionBuilderTest,
     DirectAndWrapAroundEdgeClosuresAcrossAllFourEdges) {
  constexpr auto kEdgeCases = std::to_array<ClipCase>({
      {
          .name = "direct_left_edge",
          .input_wkt = "POLYGON ((-4 2, -4 8, 4 5, -4 2))",
          .expected_wkt = "POLYGON ((0 3.5, 0 6.5, 4 5, 0 3.5))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 6.0,
      },
      {
          .name = "direct_top_edge",
          .input_wkt = "POLYGON ((2 14, 8 14, 5 6, 2 14))",
          .expected_wkt = "POLYGON ((3.5 10, 6.5 10, 5 6, 3.5 10))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 6.0,
      },
      {
          .name = "direct_right_edge",
          .input_wkt = "POLYGON ((14 8, 14 2, 6 5, 14 8))",
          .expected_wkt = "POLYGON ((10 3.5, 6 5, 10 6.5, 10 3.5))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 6.0,
      },
      {
          .name = "direct_bottom_edge",
          .input_wkt = "POLYGON ((8 -4, 2 -4, 5 4, 8 -4))",
          .expected_wkt = "POLYGON ((3.5 0, 5 4, 6.5 0, 3.5 0))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 6.0,
      },
      {
          .name = "wrap_around_from_left_notch",
          .input_wkt =
              "POLYGON ((-4 -4, -4 2, 4 5, -4 8, -4 14, 14 14, 14 -4, -4 -4))",
          .expected_wkt = "POLYGON ((0 0, 0 3.5, 4 5, 0 6.5, 0 10, 10 10, 10 "
                          "0, 0 0))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 94.0,
      },
      {
          .name = "wrap_around_from_top_notch",
          .input_wkt =
              "POLYGON ((-4 -4, -4 14, 2 14, 5 6, 8 14, 14 14, 14 -4, -4 -4))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 3.5 10, 5 6, 6.5 10, 10 10, "
                          "10 0, 0 0))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 94.0,
      },
      {
          .name = "wrap_around_from_right_notch",
          .input_wkt =
              "POLYGON ((-4 -4, -4 14, 14 14, 14 8, 6 5, 14 2, 14 -4, -4 -4))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 6.5, 6 5, 10 3.5, "
                          "10 0, 0 0))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 94.0,
      },
      {
          .name = "wrap_around_from_bottom_notch",
          .input_wkt =
              "POLYGON ((-4 -4, -4 14, 14 14, 14 -4, 8 -4, 5 4, 2 -4, -4 -4))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 6.5 0, 5 4, 3.5 "
                          "0, 0 0))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 94.0,
      },
      {
          .name = "wrap_around_arriving_at_top_left_corner",
          .input_wkt = "POLYGON ((-4 -4, -4 14, 0 10, 3 5, 5 10, 5 14, 14 14, "
                       "14 -4, -4 -4))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 3 5, 5 10, 10 10, 10 0, 0 0))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 87.5,
      },
      {
          .name = "wrap_around_arriving_at_top_left_corner_ccw",
          .input_wkt = "POLYGON ((-4 -4, 14 -4, 14 14, 5 14, 5 10, 3 5, 0 10, "
                       "-4 14, -4 -4))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 3 5, 5 10, 10 10, 10 0, 0 0))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 87.5,
      },
  });

  RunClipCases(kEdgeCases, Rectangle(0.0, 0.0, 10.0, 10.0));
}

TEST(RectangleIntersectionBuilderTest, NormalizeRingOrdersStartVertex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Rectangle rect(0.0, 0.0, 10.0, 10.0);

  struct NormalizeCase {
    std::string_view name;
    std::string_view input_wkt;
    double expected_first_x;
    double expected_first_y;
    double expected_area;
  };

  constexpr auto kCases = std::to_array<NormalizeCase>({
      {
          .name = "already_normalized_at_origin",
          .input_wkt = "POLYGON ((-4 -4, 4 4, 4 0, -4 -4))",
          .expected_first_x = 0.0,
          .expected_first_y = 0.0,
          .expected_area = 8.0,
      },
      {
          .name = "rotates_to_smaller_x",
          .input_wkt = "POLYGON ((14 8, 14 2, 6 5, 14 8))",
          .expected_first_x = 6.0,
          .expected_first_y = 5.0,
          .expected_area = 6.0,
      },
      {
          .name = "tie_breaks_equal_x_by_smaller_y",
          .input_wkt = "POLYGON ((-4 2, -4 8, 4 5, -4 2))",
          .expected_first_x = 0.0,
          .expected_first_y = 3.5,
          .expected_area = 6.0,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> input = reader.read(std::string(tc.input_wkt));
    ASSERT_NE(input, nullptr);

    std::unique_ptr<Geometry> actual =
        RectangleIntersection::clip(*input, rect);
    ASSERT_NE(actual, nullptr);
    ASSERT_EQ(actual->getGeometryTypeId(), ::geos::geom::GEOS_POLYGON);
    const auto* poly = dynamic_cast<const Polygon*>(actual.get());
    ASSERT_NE(poly, nullptr);
    const CoordinateSequence* ring =
        poly->getExteriorRing()->getCoordinatesRO();
    ASSERT_GE(ring->size(), 4u);
    EXPECT_DOUBLE_EQ(ring->getX(0), tc.expected_first_x);
    EXPECT_DOUBLE_EQ(ring->getY(0), tc.expected_first_y);
    EXPECT_DOUBLE_EQ(ring->getX(ring->size() - 1), tc.expected_first_x);
    EXPECT_DOUBLE_EQ(ring->getY(ring->size() - 1), tc.expected_first_y);
    EXPECT_NEAR(actual->getArea(), tc.expected_area, kTolerance);
  }
}

TEST(RectangleIntersectionBuilderTest,
     ReconnectPolygonsMergesMultipleBoundaryArcs) {
  constexpr auto kMergeCases = std::to_array<ClipCase>({
      {
          .name = "two_ears_merged_into_one_shell",
          .input_wkt = "POLYGON ((2 2, 2 8, 3 8, 3 12, 4 12, 4 8, 8 8, 8 6, "
                       "12 6, 12 5, 8 5, 8 2, 2 2))",
          .expected_wkt = "POLYGON ((2 2, 2 8, 3 8, 3 10, 4 10, 4 8, 8 8, "
                          "8 6, 10 6, 10 5, 8 5, 8 2, 2 2))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 40.0,
      },
      {
          .name = "two_ears_merged_ccw",
          .input_wkt = "POLYGON ((2 2, 8 2, 8 5, 12 5, 12 6, 8 6, 8 8, 4 8, "
                       "4 12, 3 12, 3 8, 2 8, 2 2))",
          .expected_wkt = "POLYGON ((2 2, 2 8, 3 8, 3 10, 4 10, 4 8, 8 8, "
                          "8 6, 10 6, 10 5, 8 5, 8 2, 2 2))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 40.0,
      },
  });

  RunClipCases(kMergeCases, Rectangle(0.0, 0.0, 10.0, 10.0));
}

TEST(RectangleIntersectionBuilderTest,
     ReconnectPolygonsWithBoundaryClippingHolesSelectsNearestLine) {
  constexpr std::string_view kExpectedMergedWkt =
      "POLYGON ((0 0, 0 3, 3 3, 3 7, 0 7, 0 10, 8 10, 8 0, 4 0, 4 2, 1 2, "
      "1 0, 0 0))";

  constexpr auto kHoleMergeCases = std::to_array<ClipCase>({
      {
          .name = "ccw_holes_far_then_near",
          .input_wkt = "POLYGON ((-4 -4, -4 14, 8 14, 8 -4, -4 -4), "
                       "(-2 3, 3 3, 3 7, -2 7, -2 3), "
                       "(1 -2, 4 -2, 4 2, 1 2, 1 -2))",
          .expected_wkt = kExpectedMergedWkt,
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 62.0,
      },
      {
          .name = "ccw_holes_near_then_far",
          .input_wkt = "POLYGON ((-4 -4, -4 14, 8 14, 8 -4, -4 -4), "
                       "(1 -2, 4 -2, 4 2, 1 2, 1 -2), "
                       "(-2 3, 3 3, 3 7, -2 7, -2 3))",
          .expected_wkt = kExpectedMergedWkt,
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 62.0,
      },
      {
          .name = "cw_holes_reversed_before_merge",
          .input_wkt = "POLYGON ((-4 -4, -4 14, 8 14, 8 -4, -4 -4), "
                       "(-2 3, -2 7, 3 7, 3 3, -2 3), "
                       "(1 -2, 1 2, 4 2, 4 -2, 1 -2))",
          .expected_wkt = kExpectedMergedWkt,
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 62.0,
      },
      {
          .name = "hole_starting_inside_rect_reconnected",
          .input_wkt = "POLYGON ((-4 -4, -4 14, 14 14, 14 -4, -4 -4), "
                       "(4 5, -4 9, -4 1, 4 5))",
          .expected_wkt = "POLYGON ((0 0, 0 3, 4 5, 0 7, 0 10, 10 10, 10 0, "
                          "0 0))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 92.0,
      },
  });

  RunClipCases(kHoleMergeCases, Rectangle(0.0, 0.0, 10.0, 10.0));

  constexpr auto kBoundaryHoleCases = std::to_array<ClipCase>({
      {
          .name = "clip_boundary_hole_starting_inside_rect",
          .input_wkt = "POLYGON ((-4 -4, -4 14, 14 14, 14 -4, -4 -4), "
                       "(4 5, -4 9, -4 1, 4 5))",
          .expected_wkt = "LINESTRING (0 7, 4 5, 0 3)",
          .expected_type = ::geos::geom::GEOS_LINESTRING,
      },
      {
          .name = "clip_boundary_intact_hole_inside_rect",
          .input_wkt = "POLYGON ((-4 -4, -4 14, 14 14, 14 -4, -4 -4), "
                       "(2 2, 8 2, 8 8, 2 8, 2 2))",
          .expected_wkt = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 36.0,
      },
  });

  RunClipCases(kBoundaryHoleCases, Rectangle(0.0, 0.0, 10.0, 10.0),
               /*clip_boundary=*/true);
}

TEST(RectangleIntersectionBuilderTest,
     AttachesIntactHolesToSingleAndMultiShells) {
  constexpr auto kHoleAttachCases = std::to_array<ClipCase>({
      {
          .name = "single_enclosing_shell_with_intact_hole",
          .input_wkt = "POLYGON ((-4 -4, -4 14, 14 14, 14 -4, -4 -4), "
                       "(2 2, 8 2, 8 8, 2 8, 2 2))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
                          "(2 2, 8 2, 8 8, 2 8, 2 2))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 64.0,
      },
      {
          .name = "two_shells_each_with_hole",
          .input_wkt =
              "POLYGON Z ((-4 1 0, 6 1 0, 6 4 0, -2 4 0, -2 6 0, 6 6 0, "
              "6 9 0, -4 9 0, -4 1 0), "
              "(1 2 0, 5 2 0, 5 3 0, 1 3 0, 1 2 0), "
              "(1 7 0, 5 7 0, 5 8 0, 1 8 0, 1 7 0))",
          .expected_wkt =
              "MULTIPOLYGON ("
              "((0 1, 0 4, 6 4, 6 1, 0 1), (1 2, 5 2, 5 3, 1 3, 1 2)), "
              "((0 6, 0 9, 6 9, 6 6, 0 6), (1 7, 5 7, 5 8, 1 8, 1 7)))",
          .expected_type = ::geos::geom::GEOS_MULTIPOLYGON,
          .expected_area = 28.0,
      },
      {
          .name = "two_shells_hole_only_in_second_shell",
          .input_wkt =
              "POLYGON Z ((-4 1 0, 6 1 0, 6 4 0, -2 4 0, -2 6 0, 6 6 0, "
              "6 9 0, -4 9 0, -4 1 0), "
              "(1 7 0, 5 7 0, 5 8 0, 1 8 0, 1 7 0))",
          .expected_wkt =
              "MULTIPOLYGON ("
              "((0 1, 0 4, 6 4, 6 1, 0 1)), "
              "((0 6, 0 9, 6 9, 6 6, 0 6), (1 7, 5 7, 5 8, 1 8, 1 7)))",
          .expected_type = ::geos::geom::GEOS_MULTIPOLYGON,
          .expected_area = 32.0,
      },
      {
          .name = "two_shells_with_clipped_and_intact_holes",
          .input_wkt =
              "POLYGON ((-4 1, 6 1, 6 4, -2 4, -2 6, 6 6, 6 9, -4 9, -4 1), "
              "(-1 2, 4 2, 4 3, -1 3, -1 2), "
              "(1 7, 5 7, 5 8, 1 8, 1 7))",
          .expected_wkt =
              "MULTIPOLYGON ("
              "((0 1, 0 2, 4 2, 4 3, 0 3, 0 4, 6 4, 6 1, 0 1)), "
              "((0 6, 0 9, 6 9, 6 6, 0 6), (1 7, 5 7, 5 8, 1 8, 1 7)))",
          .expected_type = ::geos::geom::GEOS_MULTIPOLYGON,
          .expected_area = 28.0,
      },
  });

  RunClipCases(kHoleAttachCases, Rectangle(0.0, 0.0, 10.0, 10.0));
}

TEST(RectangleIntersectionBuilderTest, NonZeroOffsetRectangle) {
  constexpr auto kOffsetCases = std::to_array<ClipCase>({
      {
          .name = "offset_ccw_polygon_crossing_corner",
          .input_wkt = "POLYGON ((-20 -10, 0 -10, 0 5, -20 5, -20 -10))",
          .expected_wkt =
              "POLYGON ((-10.5 -5.25, -10.5 5, 0 5, 0 -5.25, -10.5 -5.25))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 107.625,
      },
  });

  RunClipCases(kOffsetCases, Rectangle(-10.5, -5.25, 4.5, 9.75));
}

TEST(RectangleIntersectionBuilderTest,
     DestructorCleansUpBufferedPartsOnHoleEarlyReturn) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Rectangle rect(0.0, 0.0, 10.0, 10.0);

  // Hole 0 adds a Polygon to `parts.polygons`, and Hole 1 encloses `rect`,
  // returning early from clip_polygon_to_polygons before build()/release().
  {
    std::unique_ptr<Geometry> poly = reader.read(
        "POLYGON ((-20 -20, -20 20, 20 20, 20 -20, -20 -20), "
        "(2 2, 4 2, 4 4, 2 4, 2 2), "
        "(-5 -5, 15 -5, 15 15, -5 15, -5 -5))");
    ASSERT_NE(poly, nullptr);
    std::unique_ptr<Geometry> res = RectangleIntersection::clip(*poly, rect);
    ASSERT_NE(res, nullptr);
    EXPECT_TRUE(res->isEmpty());
  }

  // Exterior ring adds a LineString to `parts.lines`, and Hole 0 encloses
  // `rect`, returning early while `parts.lines` is non-empty.
  {
    std::unique_ptr<Geometry> poly = reader.read(
        "POLYGON ((-20 -20, -20 20, 8 20, 8 -20, -20 -20), "
        "(-5 -5, 15 -5, 15 15, -5 15, -5 -5))");
    ASSERT_NE(poly, nullptr);
    std::unique_ptr<Geometry> res = RectangleIntersection::clip(*poly, rect);
    ASSERT_NE(res, nullptr);
    EXPECT_TRUE(res->isEmpty());
  }
}

TEST(RectangleIntersectionBuilderTest,
     DestructorCleansUpBufferedPartsOnExceptionUnwind) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Rectangle rect(0.0, 0.0, 10.0, 10.0);

  // Buffer a Point, LineString, and Polygon before hitting CircularString.
  std::unique_ptr<Geometry> with_curve = reader.read(
      "GEOMETRYCOLLECTION ("
      "POINT (5 5), "
      "LINESTRING (1 1, 9 9), "
      "POLYGON ((2 2, 4 2, 4 4, 2 4, 2 2)), "
      "CIRCULARSTRING (0 0, 1 1, 2 0))");
  ASSERT_NE(with_curve, nullptr);
  EXPECT_THROW(RectangleIntersection::clip(*with_curve, rect),
               UnsupportedOperationException);

  // Buffer a Point, LineString, and Polygon before hitting a degenerate
  // polygon that throws in reconnectPolygons().
  std::unique_ptr<Geometry> with_bad_poly = reader.read(
      "GEOMETRYCOLLECTION ("
      "POINT (5 5), "
      "LINESTRING (1 1, 9 9), "
      "POLYGON ((2 2, 4 2, 4 4, 2 4, 2 2)), "
      "POLYGON ((5 5, -1 5, -2 5, 5 5)))");
  ASSERT_NE(with_bad_poly, nullptr);
  EXPECT_THROW(RectangleIntersection::clip(*with_bad_poly, rect),
               IllegalArgumentException);
}

TEST(RectangleIntersectionBuilderTest,
     ThrowsOnDegenerateClippedRingSmallerThanFourPoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Rectangle rect(0.0, 0.0, 10.0, 10.0);

  for (std::string_view wkt : {
           // Starts inside rect, exits and re-enters at (0 5), then
           // reconnect() merges both segments into a 3-point ring.
           std::string_view("POLYGON ((5 5, -1 5, -2 5, 5 5))"),
           // Starts on Left edge at (0 5) and clips to a single 3-point line.
           std::string_view("POLYGON ((0 5, 5 5, -1 5, 0 5))"),
           // Starts on Right edge at (10 5); normalize_ring() rotates the
           // 3-point ring before the size < 4 validation throws.
           std::string_view("POLYGON ((10 5, 5 5, 11 5, 10 5))"),
       }) {
    SCOPED_TRACE(std::string(wkt));
    std::unique_ptr<Geometry> poly = reader.read(std::string(wkt));
    ASSERT_NE(poly, nullptr);
    try {
      (void)RectangleIntersection::clip(*poly, rect);
      FAIL() << "Expected IllegalArgumentException";
    } catch (const IllegalArgumentException& e) {
      EXPECT_THAT(e.what(),
                  HasSubstr("Invalid number of points in LinearRing"));
    }
  }
}

TEST(RectangleIntersectionBuilderTest,
     ThrowsOnNonBoundaryCoordinateInDistance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  Rectangle rect(0.0, 0.0, 10.0, 10.0);

  // Case 1: clipped ring starts Inside so `endpos` is Inside.
  {
    auto closed = std::make_unique<CoordinateSequence>();
    closed->add(5.0, 6.0);
    closed->add(-2.0, 8.0);
    closed->add(2.0, 5.0);
    closed->add(5.0, 6.0);
    std::unique_ptr<LinearRing> ring =
        factory->createLinearRing(std::move(closed));

    CoordinateSequence unclosed;
    unclosed.add(5.0, 6.0);
    unclosed.add(-2.0, 8.0);
    unclosed.add(2.0, 5.0);
    unclosed.add(5.0, 4.0);
    ring->setPoints(&unclosed);

    std::unique_ptr<Polygon> poly = factory->createPolygon(std::move(ring));
    try {
      (void)RectangleIntersection::clip(*poly, rect);
      FAIL() << "Expected IllegalArgumentException";
    } catch (const IllegalArgumentException& e) {
      EXPECT_THAT(
          e.what(),
          HasSubstr("Can't compute distance to non-boundary position."));
    }
  }

  // Case 2: clipped ring enters at Left and ends Inside so `pos` is Inside.
  {
    auto closed = std::make_unique<CoordinateSequence>();
    closed->add(-2.0, 7.0);
    closed->add(5.0, 6.0);
    closed->add(2.0, 4.0);
    closed->add(-2.0, 7.0);
    std::unique_ptr<LinearRing> ring =
        factory->createLinearRing(std::move(closed));

    CoordinateSequence unclosed;
    unclosed.add(-2.0, 7.0);
    unclosed.add(5.0, 6.0);
    unclosed.add(5.0, 5.0);
    unclosed.add(2.0, 4.0);
    ring->setPoints(&unclosed);

    std::unique_ptr<Polygon> poly = factory->createPolygon(std::move(ring));
    try {
      (void)RectangleIntersection::clip(*poly, rect);
      FAIL() << "Expected IllegalArgumentException";
    } catch (const IllegalArgumentException& e) {
      EXPECT_THAT(
          e.what(),
          HasSubstr("Can't compute distance to non-boundary position."));
    }
  }
}

TEST(RectangleIntersectionBuilderTest, PreservesZCoordinatesInClipAndBoundary) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Rectangle rect(0.0, 0.0, 10.0, 10.0);

  std::unique_ptr<Geometry> poly_z =
      reader.read("POLYGON Z ((4 5 10, -4 1 2, -4 9 18, 4 5 10))");
  ASSERT_NE(poly_z, nullptr);

  std::unique_ptr<Geometry> boundary =
      RectangleIntersection::clipBoundary(*poly_z, rect);
  ASSERT_NE(boundary, nullptr);
  EXPECT_EQ(boundary->getGeometryTypeId(), ::geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(boundary->hasZ());
  std::unique_ptr<CoordinateSequence> coords = boundary->getCoordinates();
  ASSERT_EQ(coords->size(), 3u);
  EXPECT_DOUBLE_EQ(coords->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(coords->getY(0), 7.0);
  EXPECT_DOUBLE_EQ(coords->getOrdinate(0, CoordinateSequence::Z), 14.0);
  EXPECT_DOUBLE_EQ(coords->getX(1), 4.0);
  EXPECT_DOUBLE_EQ(coords->getY(1), 5.0);
  EXPECT_DOUBLE_EQ(coords->getOrdinate(1, CoordinateSequence::Z), 10.0);
  EXPECT_DOUBLE_EQ(coords->getX(2), 0.0);
  EXPECT_DOUBLE_EQ(coords->getY(2), 3.0);
  EXPECT_DOUBLE_EQ(coords->getOrdinate(2, CoordinateSequence::Z), 6.0);

  std::unique_ptr<Geometry> clipped =
      RectangleIntersection::clip(*poly_z, rect);
  ASSERT_NE(clipped, nullptr);
  EXPECT_EQ(clipped->getGeometryTypeId(), ::geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(clipped->hasZ());
  EXPECT_NEAR(clipped->getArea(), 8.0, kTolerance);
  std::unique_ptr<CoordinateSequence> clipped_coords =
      clipped->getCoordinates();
  ASSERT_EQ(clipped_coords->size(), 4u);
  for (std::size_t i = 0; i < clipped_coords->size(); ++i) {
    EXPECT_TRUE(
        std::isfinite(clipped_coords->getOrdinate(i, CoordinateSequence::Z)));
  }
}

TEST(RectangleIntersectionBuilderTest,
     ReconnectsThreeLinesFirstAndLastAndKeepsMiddle) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Rectangle rect(0.0, 0.0, 10.0, 10.0);

  // Starting inside at (5 5) and exiting/re-entering twice produces 3 lines;
  // reconnect() merges the 1st and 3rd at (5 5) while leaving the 2nd intact.
  constexpr std::string_view kCwWkt =
      "POLYGON ((5 5, -2 5, -2 7, 4 7, 4 12, 7 12, 7 5, 5 5))";
  constexpr std::string_view kCcwWkt =
      "POLYGON ((5 5, 7 5, 7 12, 4 12, 4 7, -2 7, -2 5, 5 5))";

  for (std::string_view wkt : {kCwWkt, kCcwWkt}) {
    SCOPED_TRACE(std::string(wkt));
    std::unique_ptr<Geometry> poly = reader.read(std::string(wkt));
    ASSERT_NE(poly, nullptr);

    std::unique_ptr<Geometry> boundary =
        RectangleIntersection::clipBoundary(*poly, rect);
    ASSERT_NE(boundary, nullptr);
    EXPECT_EQ(boundary->getNumGeometries(), 2u);
    std::unique_ptr<Geometry> expected_boundary = reader.read(
        "MULTILINESTRING ((7 10, 7 5, 5 5, 0 5), (0 7, 4 7, 4 10))");
    ExpectNormalizedEqual(*boundary, *expected_boundary,
                          ::geos::geom::GEOS_MULTILINESTRING);

    std::unique_ptr<Geometry> clipped =
        RectangleIntersection::clip(*poly, rect);
    ASSERT_NE(clipped, nullptr);
    std::unique_ptr<Geometry> expected_poly =
        reader.read("POLYGON ((0 5, 0 7, 4 7, 4 10, 7 10, 7 5, 5 5, 0 5))");
    ExpectNormalizedEqual(*clipped, *expected_poly, ::geos::geom::GEOS_POLYGON,
                          23.0);
  }
}

TEST(RectangleIntersectionBuilderTest, DiagonalCornerToCornerClosures) {
  constexpr auto kCornerCases = std::to_array<ClipCase>({
      {
          .name = "bottom_left_to_top_right_via_bottom_right",
          .input_wkt =
              "POLYGON ((-4 0, 0 0, 5 5, 10 10, 14 10, 14 -4, -4 -4, -4 0))",
          .expected_wkt = "POLYGON ((0 0, 5 5, 10 10, 10 0, 0 0))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 50.0,
      },
      {
          .name = "top_right_to_bottom_left_via_top_left",
          .input_wkt = "POLYGON ((-4 14, 14 14, 10 10, 5 5, 0 0, -4 0, -4 14))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 5 5, 0 0))",
          .expected_type = ::geos::geom::GEOS_POLYGON,
          .expected_area = 50.0,
      },
  });

  RunClipCases(kCornerCases, Rectangle(0.0, 0.0, 10.0, 10.0));
}

TEST(RectangleIntersectionBuilderTest,
     ClipsMultiPolygonAndPreservesFactorySrid) {
  PrecisionModel pm;
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 4326);
  WKTReader reader(factory.get());
  Rectangle rect(0.0, 0.0, 10.0, 10.0);

  // Disjoint MultiPolygon exercises the empty build() path with SRID preserved.
  {
    std::unique_ptr<Geometry> input = reader.read(
        "MULTIPOLYGON ("
        "((20 20, 30 20, 30 30, 20 30, 20 20)), "
        "((40 40, 50 40, 50 50, 40 50, 40 40)))");
    ASSERT_NE(input, nullptr);
    std::unique_ptr<Geometry> actual =
        RectangleIntersection::clip(*input, rect);
    ASSERT_NE(actual, nullptr);
    EXPECT_TRUE(actual->isEmpty());
    EXPECT_EQ(actual->getGeometryTypeId(),
              ::geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(actual->getSRID(), 4326);
  }

  // Only one component intersects: build() collapses to a single Polygon.
  {
    std::unique_ptr<Geometry> input = reader.read(
        "MULTIPOLYGON ("
        "((2 2, 6 2, 6 6, 2 6, 2 2)), "
        "((20 20, 30 20, 30 30, 20 30, 20 20)))");
    std::unique_ptr<Geometry> expected =
        reader.read("POLYGON ((2 2, 6 2, 6 6, 2 6, 2 2))");
    ASSERT_NE(input, nullptr);
    ASSERT_NE(expected, nullptr);
    std::unique_ptr<Geometry> actual =
        RectangleIntersection::clip(*input, rect);
    ASSERT_NE(actual, nullptr);
    EXPECT_EQ(actual->getSRID(), 4326);
    ExpectNormalizedEqual(*actual, *expected, ::geos::geom::GEOS_POLYGON, 16.0);
  }

  // Two intersecting components produce a MultiPolygon with SRID preserved.
  {
    std::unique_ptr<Geometry> input = reader.read(
        "MULTIPOLYGON ("
        "((-2 1, 4 1, 4 4, -2 4, -2 1)), "
        "((6 6, 12 6, 12 9, 6 9, 6 6)))");
    std::unique_ptr<Geometry> expected = reader.read(
        "MULTIPOLYGON ("
        "((0 1, 0 4, 4 4, 4 1, 0 1)), "
        "((6 6, 6 9, 10 9, 10 6, 6 6)))");
    ASSERT_NE(input, nullptr);
    ASSERT_NE(expected, nullptr);
    std::unique_ptr<Geometry> actual =
        RectangleIntersection::clip(*input, rect);
    ASSERT_NE(actual, nullptr);
    EXPECT_EQ(actual->getNumGeometries(), 2u);
    EXPECT_EQ(actual->getSRID(), 4326);
    ExpectNormalizedEqual(*actual, *expected, ::geos::geom::GEOS_MULTIPOLYGON,
                          24.0);
  }
}

}  // namespace
}  // namespace geos_google
