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
// third_party/geos/src/operation/intersection/RectangleIntersection.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for
// UnsupportedOperationException
//   assertions.
// - Uses an isolated GeometryFactory per test fixture.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/intersection/RectangleIntersection.cpp

#include "third_party/geos/include/geos/operation/intersection/RectangleIntersection.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/io/WKTWriter.h"
#include "third_party/geos/include/geos/operation/intersection/Rectangle.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::MultiPoint;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::io::WKTWriter;
using geos::operation::intersection::Rectangle;
using geos::operation::intersection::RectangleIntersection;
using geos::util::UnsupportedOperationException;

struct WktClipCase {
  std::string_view name;
  std::string_view input_wkt;
  std::string_view expected_wkt;
};

class RectangleIntersectionTest : public ::testing::Test {
 protected:
  RectangleIntersectionTest()
      : factory_(GeometryFactory::create()),
        reader_(factory_.get()),
        rect_(0.0, 0.0, 10.0, 10.0) {}

  Geometry::Ptr ReadWkt(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  Geometry::Ptr Clip(std::string_view wkt, const Rectangle& rect) const {
    Geometry::Ptr geom = ReadWkt(wkt);
    return RectangleIntersection::clip(*geom, rect);
  }

  Geometry::Ptr Clip(std::string_view wkt) const { return Clip(wkt, rect_); }

  Geometry::Ptr ClipBoundary(std::string_view wkt,
                             const Rectangle& rect) const {
    Geometry::Ptr geom = ReadWkt(wkt);
    return RectangleIntersection::clipBoundary(*geom, rect);
  }

  Geometry::Ptr ClipBoundary(std::string_view wkt) const {
    return ClipBoundary(wkt, rect_);
  }

  void ExpectEmptyResult(const Geometry* actual) const {
    ASSERT_NE(actual, nullptr);
    EXPECT_TRUE(actual->isEmpty()) << WKTWriter().write(actual);
    EXPECT_EQ(actual->getGeometryTypeId(),
              GeometryTypeId::GEOS_GEOMETRYCOLLECTION);
  }

  void ExpectMatchesWkt(const Geometry* actual,
                        std::string_view expected_wkt) const {
    ASSERT_NE(actual, nullptr);
    Geometry::Ptr expected = ReadWkt(expected_wkt);
    ASSERT_NE(expected, nullptr);
    if (expected->isEmpty()) {
      ExpectEmptyResult(actual);
      return;
    }
    Geometry::Ptr actual_norm = actual->clone();
    Geometry::Ptr expected_norm = expected->clone();
    actual_norm->normalize();
    expected_norm->normalize();
    WKTWriter writer;
    EXPECT_EQ(actual->getGeometryTypeId(), expected->getGeometryTypeId())
        << "actual=" << writer.write(actual)
        << " expected=" << writer.write(expected.get());
    EXPECT_TRUE(actual_norm->equalsExact(expected_norm.get(), 1e-9))
        << "actual=" << writer.write(actual_norm.get())
        << " expected=" << writer.write(expected_norm.get());
  }

  void ExpectMatchesExactOrder(const Geometry* actual,
                               std::string_view expected_wkt) const {
    ASSERT_NE(actual, nullptr);
    Geometry::Ptr expected = ReadWkt(expected_wkt);
    ASSERT_NE(expected, nullptr);
    WKTWriter writer;
    EXPECT_TRUE(actual->equalsExact(expected.get(), 1e-9))
        << "actual=" << writer.write(actual)
        << " expected=" << writer.write(expected.get());
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
  Rectangle rect_;
};

TEST_F(RectangleIntersectionTest, ClipsPointVariants) {
  constexpr auto kCases = std::to_array<WktClipCase>({
      {
          .name = "empty_point",
          .input_wkt = "POINT EMPTY",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "strictly_inside",
          .input_wkt = "POINT (5 5)",
          .expected_wkt = "POINT (5 5)",
      },
      {
          .name = "outside_left",
          .input_wkt = "POINT (-1 5)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "outside_right",
          .input_wkt = "POINT (11 5)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "outside_bottom",
          .input_wkt = "POINT (5 -1)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "outside_top",
          .input_wkt = "POINT (5 11)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "on_left_edge_excluded",
          .input_wkt = "POINT (0 5)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "on_right_edge_excluded",
          .input_wkt = "POINT (10 5)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "on_bottom_edge_excluded",
          .input_wkt = "POINT (5 0)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "on_top_edge_excluded",
          .input_wkt = "POINT (5 10)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "on_corner_excluded",
          .input_wkt = "POINT (0 0)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    ExpectMatchesWkt(Clip(tc.input_wkt).get(), tc.expected_wkt);
    ExpectMatchesWkt(ClipBoundary(tc.input_wkt).get(), tc.expected_wkt);
  }
}

TEST_F(RectangleIntersectionTest, ClipsMultiPointVariants) {
  constexpr auto kCases = std::to_array<WktClipCase>({
      {
          .name = "empty_multipoint",
          .input_wkt = "MULTIPOINT EMPTY",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "all_outside_or_on_edge",
          .input_wkt = "MULTIPOINT ((-1 5), (0 5), (10 10), (15 2))",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "single_inside_collapses_to_point",
          .input_wkt = "MULTIPOINT ((-2 5), (5 5), (10 5))",
          .expected_wkt = "POINT (5 5)",
      },
      {
          .name = "multiple_inside",
          .input_wkt = "MULTIPOINT ((2 3), (0 0), (8 7), (12 4))",
          .expected_wkt = "MULTIPOINT ((2 3), (8 7))",
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    ExpectMatchesWkt(Clip(tc.input_wkt).get(), tc.expected_wkt);
    ExpectMatchesWkt(ClipBoundary(tc.input_wkt).get(), tc.expected_wkt);
  }
}

TEST_F(RectangleIntersectionTest, MultiPointSkipsEmptyPointComponent) {
  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory_->createPoint());
  pts.push_back(factory_->createPoint(Coordinate(4.0, 6.0)));
  std::unique_ptr<MultiPoint> mp = factory_->createMultiPoint(std::move(pts));

  Geometry::Ptr clipped = RectangleIntersection::clip(*mp, rect_);
  ExpectMatchesWkt(clipped.get(), "POINT (4 6)");

  Geometry::Ptr clipped_boundary =
      RectangleIntersection::clipBoundary(*mp, rect_);
  ExpectMatchesWkt(clipped_boundary.get(), "POINT (4 6)");
}

TEST_F(RectangleIntersectionTest, ClipsLineStringOutsideFastSkipAndInside) {
  constexpr auto kCases = std::to_array<WktClipCase>({
      {
          .name = "empty_linestring",
          .input_wkt = "LINESTRING EMPTY",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "strictly_inside",
          .input_wkt = "LINESTRING (2 2, 5 8, 8 3)",
          .expected_wkt = "LINESTRING (2 2, 5 8, 8 3)",
      },
      {
          .name = "isolated_edge_touches_stay_intact",
          .input_wkt = "LINESTRING (0 5, 5 5, 10 5)",
          .expected_wkt = "LINESTRING (0 5, 5 5, 10 5)",
      },
      {
          .name = "all_outside_left_multi_vertex",
          .input_wkt = "LINESTRING (-10 2, -5 5, -2 8)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "all_outside_right_multi_vertex",
          .input_wkt = "LINESTRING (20 2, 15 5, 12 8)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "all_outside_bottom_multi_vertex",
          .input_wkt = "LINESTRING (2 -10, 5 -5, 8 -2)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "all_outside_top_multi_vertex",
          .input_wkt = "LINESTRING (2 20, 5 15, 8 12)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "fast_skip_left_then_enter",
          .input_wkt = "LINESTRING (-10 5, -5 5, 5 5)",
          .expected_wkt = "LINESTRING (0 5, 5 5)",
      },
      {
          .name = "fast_skip_right_then_enter",
          .input_wkt = "LINESTRING (20 5, 15 5, 5 5)",
          .expected_wkt = "LINESTRING (10 5, 5 5)",
      },
      {
          .name = "fast_skip_bottom_then_enter",
          .input_wkt = "LINESTRING (5 -10, 5 -5, 5 5)",
          .expected_wkt = "LINESTRING (5 0, 5 5)",
      },
      {
          .name = "fast_skip_top_then_enter",
          .input_wkt = "LINESTRING (5 20, 5 15, 5 5)",
          .expected_wkt = "LINESTRING (5 10, 5 5)",
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    ExpectMatchesExactOrder(Clip(tc.input_wkt).get(), tc.expected_wkt);
    ExpectMatchesExactOrder(ClipBoundary(tc.input_wkt).get(), tc.expected_wkt);
  }
}

TEST_F(RectangleIntersectionTest, ClipsLineStringOutsideToOutsideAndCorners) {
  constexpr auto kCases = std::to_array<WktClipCase>({
      {
          .name = "horizontal_through",
          .input_wkt = "LINESTRING (-5 5, 15 5)",
          .expected_wkt = "LINESTRING (0 5, 10 5)",
      },
      {
          .name = "vertical_through",
          .input_wkt = "LINESTRING (5 -5, 5 15)",
          .expected_wkt = "LINESTRING (5 0, 5 10)",
      },
      {
          .name = "diagonal_bottom_to_right",
          .input_wkt = "LINESTRING (-2 -4, 12 10)",
          .expected_wkt = "LINESTRING (2 0, 10 8)",
      },
      {
          .name = "diagonal_top_to_left",
          .input_wkt = "LINESTRING (12 14, -4 -2)",
          .expected_wkt = "LINESTRING (8 10, 0 2)",
      },
      {
          .name = "diagonal_through_corners",
          .input_wkt = "LINESTRING (-2 12, 12 -2)",
          .expected_wkt = "LINESTRING (0 10, 10 0)",
      },
      {
          .name = "touches_single_corner_discarded",
          .input_wkt = "LINESTRING (-5 5, 5 15)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "misses_top_left_corner_discarded",
          .input_wkt = "LINESTRING (-5 8, 2 15)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "wraps_around_outside_all_edges",
          .input_wkt = "LINESTRING (-5 -5, -5 15, 15 15, 15 -5, -5 -5)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "outside_along_bottom_edge_discarded",
          .input_wkt = "LINESTRING (-5 0, 15 0)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "outside_along_right_edge_discarded",
          .input_wkt = "LINESTRING (10 -5, 10 15)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    ExpectMatchesExactOrder(Clip(tc.input_wkt).get(), tc.expected_wkt);
  }
}

TEST_F(RectangleIntersectionTest, ClipsLineStringEdgeTransitionsAndSplits) {
  constexpr auto kCases = std::to_array<WktClipCase>({
      {
          .name = "outside_to_exact_edge_endpoint_stops",
          .input_wkt = "LINESTRING (-5 5, 0 5)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "outside_to_exact_edge_endpoint_then_inside",
          .input_wkt = "LINESTRING (-5 5, 0 5, 5 5)",
          .expected_wkt = "LINESTRING (0 5, 5 5)",
      },
      {
          .name = "outside_along_bottom_edge_then_inside",
          .input_wkt = "LINESTRING (-5 0, 5 0, 5 5)",
          .expected_wkt = "LINESTRING (5 0, 5 5)",
      },
      {
          .name = "outside_to_opposite_edge_at_end",
          .input_wkt = "LINESTRING (-5 5, 10 5)",
          .expected_wkt = "LINESTRING (0 5, 10 5)",
      },
      {
          .name = "outside_to_different_edge_then_same_edge",
          .input_wkt = "LINESTRING (-5 5, 5 10, 8 10)",
          .expected_wkt = "LINESTRING (0 7.5, 5 10)",
      },
      {
          .name = "inside_to_outside",
          .input_wkt = "LINESTRING (5 5, 15 5)",
          .expected_wkt = "LINESTRING (5 5, 10 5)",
      },
      {
          .name = "edge_to_outside_same_edge_empty",
          .input_wkt = "LINESTRING (0 5, -5 5)",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "inside_to_edge_to_outside_same_edge",
          .input_wkt = "LINESTRING (2 5, 0 5, -5 5)",
          .expected_wkt = "LINESTRING (2 5, 0 5)",
      },
      {
          .name = "outside_to_edge_to_outside_same_edge",
          .input_wkt = "LINESTRING (-5 5, 10 5, 15 5)",
          .expected_wkt = "LINESTRING (0 5, 10 5)",
      },
      {
          .name = "edge_to_outside_different_edge_through_box",
          .input_wkt = "LINESTRING (0 5, 15 5)",
          .expected_wkt = "LINESTRING (0 5, 10 5)",
      },
      {
          .name = "inside_then_two_same_edge_points_then_inside",
          .input_wkt = "LINESTRING (5 5, 0 2, 0 8, 5 8)",
          .expected_wkt = "MULTILINESTRING ((5 5, 0 2), (0 8, 5 8))",
      },
      {
          .name = "start_two_same_edge_points_then_inside",
          .input_wkt = "LINESTRING (0 2, 0 8, 5 8)",
          .expected_wkt = "LINESTRING (0 8, 5 8)",
      },
      {
          .name = "inside_then_end_two_same_edge_points",
          .input_wkt = "LINESTRING (5 5, 0 2, 0 8)",
          .expected_wkt = "LINESTRING (5 5, 0 2)",
      },
      {
          .name = "inside_touches_different_edges_stays_connected",
          .input_wkt = "LINESTRING (-5 5, 5 5, 0 2, 5 10, 15 5)",
          .expected_wkt = "LINESTRING (0 5, 5 5, 0 2, 5 10, 10 7.5)",
      },
      {
          .name = "multiple_crossings_outside_and_back",
          .input_wkt = "LINESTRING (-5 2, 15 2, 15 8, -5 8)",
          .expected_wkt = "MULTILINESTRING ((0 2, 10 2), (10 8, 0 8))",
      },
      {
          .name = "inside_exits_and_reenters",
          .input_wkt = "LINESTRING (2 2, 5 2, 15 4, 15 6, 5 8, 2 8)",
          .expected_wkt =
              "MULTILINESTRING ((2 2, 5 2, 10 3), (10 7, 5 8, 2 8))",
      },
      {
          .name = "outside_to_inside_multiple_points_flush",
          .input_wkt = "LINESTRING (-5 5, 5 5, 8 5)",
          .expected_wkt = "LINESTRING (0 5, 5 5, 8 5)",
      },
      {
          .name = "outside_to_same_edge_pair_then_inside",
          .input_wkt = "LINESTRING (-5 5, 0 2, 0 8, 5 8)",
          .expected_wkt = "LINESTRING (0 8, 5 8)",
      },
      {
          .name = "outside_to_edge_cross_box_exit_same_edge",
          .input_wkt = "LINESTRING (-5 5, 0 2, 10 2, 15 2)",
          .expected_wkt = "LINESTRING (0 2, 10 2)",
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    ExpectMatchesExactOrder(Clip(tc.input_wkt).get(), tc.expected_wkt);
  }
}

TEST_F(RectangleIntersectionTest, ClipsLinearRingDirectly) {
  Geometry::Ptr inside_ring = ReadWkt("LINEARRING (2 2, 8 2, 8 8, 2 8, 2 2)");
  Geometry::Ptr clipped_inside =
      RectangleIntersection::clip(*inside_ring, rect_);
  ASSERT_NE(clipped_inside, nullptr);
  EXPECT_EQ(clipped_inside->getGeometryTypeId(),
            GeometryTypeId::GEOS_LINEARRING);
  ExpectMatchesExactOrder(clipped_inside.get(),
                          "LINEARRING (2 2, 8 2, 8 8, 2 8, 2 2)");

  Geometry::Ptr crossing_ring =
      ReadWkt("LINEARRING (-2 2, 8 2, 8 8, -2 8, -2 2)");
  Geometry::Ptr clipped_crossing =
      RectangleIntersection::clip(*crossing_ring, rect_);
  ExpectMatchesWkt(clipped_crossing.get(), "LINESTRING (0 2, 8 2, 8 8, 0 8)");
}

TEST_F(RectangleIntersectionTest, ClipsMultiLineStringVariants) {
  constexpr auto kCases = std::to_array<WktClipCase>({
      {
          .name = "empty_multilinestring",
          .input_wkt = "MULTILINESTRING EMPTY",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "all_outside",
          .input_wkt = "MULTILINESTRING ((-5 2, -1 8), (12 2, 18 8))",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "single_surviving_line_collapses",
          .input_wkt = "MULTILINESTRING ((-5 2, -1 8), (-5 5, 15 5))",
          .expected_wkt = "LINESTRING (0 5, 10 5)",
      },
      {
          .name = "mixed_inside_and_clipped",
          .input_wkt =
              "MULTILINESTRING ((2 2, 4 4), (-5 6, 15 6), (12 1, 18 9))",
          .expected_wkt = "MULTILINESTRING ((2 2, 4 4), (0 6, 10 6))",
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    ExpectMatchesWkt(Clip(tc.input_wkt).get(), tc.expected_wkt);
    ExpectMatchesWkt(ClipBoundary(tc.input_wkt).get(), tc.expected_wkt);
  }
}

TEST_F(RectangleIntersectionTest, ClipsPolygonToPolygons) {
  constexpr auto kCases = std::to_array<WktClipCase>({
      {
          .name = "empty_polygon",
          .input_wkt = "POLYGON EMPTY",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "strictly_inside",
          .input_wkt = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
          .expected_wkt = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
      },
      {
          .name = "strictly_outside",
          .input_wkt = "POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20))",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "touches_exterior_edge_only",
          .input_wkt = "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "covers_rectangle_completely",
          .input_wkt = "POLYGON ((-5 -5, 15 -5, 15 15, -5 15, -5 -5))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
      },
      {
          .name = "exact_match_rectangle",
          .input_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
      },
      {
          .name = "partial_overlap_ccw_shell",
          .input_wkt = "POLYGON ((5 -5, 15 -5, 15 15, 5 15, 5 -5))",
          .expected_wkt = "POLYGON ((5 0, 5 10, 10 10, 10 0, 5 0))",
      },
      {
          .name = "partial_overlap_cw_shell",
          .input_wkt = "POLYGON ((5 -5, 5 15, 15 15, 15 -5, 5 -5))",
          .expected_wkt = "POLYGON ((5 0, 5 10, 10 10, 10 0, 5 0))",
      },
      {
          .name = "u_shape_splits_into_multipolygon",
          .input_wkt =
              "POLYGON ((-5 1, 8 1, 8 3, -2 3, -2 7, 8 7, 8 9, -5 9, -5 1))",
          .expected_wkt = "MULTIPOLYGON (((0 1, 0 3, 8 3, 8 1, 0 1)), "
                          "((0 7, 0 9, 8 9, 8 7, 0 7)))",
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    ExpectMatchesWkt(Clip(tc.input_wkt).get(), tc.expected_wkt);
  }
}

TEST_F(RectangleIntersectionTest, ClipsPolygonWithHolesToPolygons) {
  constexpr auto kCases = std::to_array<WktClipCase>({
      {
          .name = "covers_rect_with_inside_hole",
          .input_wkt = "POLYGON ((-5 -5, 15 -5, 15 15, -5 15, -5 -5), "
                       "(2 2, 2 8, 8 8, 8 2, 2 2))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
                          "(2 2, 2 8, 8 8, 8 2, 2 2))",
      },
      {
          .name = "rect_completely_inside_hole",
          .input_wkt = "POLYGON ((-10 -10, 20 -10, 20 20, -10 20, -10 -10), "
                       "(-5 -5, -5 15, 15 15, 15 -5, -5 -5))",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "covers_rect_with_outside_hole",
          .input_wkt = "POLYGON ((-5 -5, 25 -5, 25 15, -5 15, -5 -5), "
                       "(15 2, 15 8, 20 8, 20 2, 15 2))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
      },
      {
          .name = "covers_rect_with_cw_clipped_hole",
          .input_wkt = "POLYGON ((-5 -5, 15 -5, 15 15, -5 15, -5 -5), "
                       "(5 2, 5 8, 12 8, 12 2, 5 2))",
          .expected_wkt =
              "POLYGON ((0 0, 0 10, 10 10, 10 8, 5 8, 5 2, 10 2, 10 0, 0 0))",
      },
      {
          .name = "covers_rect_with_ccw_clipped_hole",
          .input_wkt = "POLYGON ((-5 -5, 15 -5, 15 15, -5 15, -5 -5), "
                       "(5 2, 12 2, 12 8, 5 8, 5 2))",
          .expected_wkt =
              "POLYGON ((0 0, 0 10, 10 10, 10 8, 5 8, 5 2, 10 2, 10 0, 0 0))",
      },
      {
          .name = "clipped_shell_with_inside_hole",
          .input_wkt = "POLYGON ((-5 0, 8 0, 8 10, -5 10, -5 0), "
                       "(2 2, 2 8, 6 8, 6 2, 2 2))",
          .expected_wkt = "POLYGON ((0 0, 0 10, 8 10, 8 0, 0 0), "
                          "(2 2, 2 8, 6 8, 6 2, 2 2))",
      },
      {
          .name = "clipped_shell_with_clipped_hole",
          .input_wkt = "POLYGON ((-5 -5, 15 -5, 15 8, -5 8, -5 -5), "
                       "(5 2, 12 2, 12 6, 5 6, 5 2))",
          .expected_wkt =
              "POLYGON ((0 0, 0 8, 10 8, 10 6, 5 6, 5 2, 10 2, 10 0, 0 0))",
      },
      {
          .name = "hole_splits_rect_into_two_polygons",
          .input_wkt = "POLYGON ((-5 -5, 15 -5, 15 15, -5 15, -5 -5), "
                       "(-2 4, -2 6, 12 6, 12 4, -2 4))",
          .expected_wkt = "MULTIPOLYGON (((0 0, 0 4, 10 4, 10 0, 0 0)), "
                          "((0 6, 0 10, 10 10, 10 6, 0 6)))",
      },
      {
          .name = "u_shape_with_hole_attached_to_matching_arm",
          .input_wkt =
              "POLYGON ((-5 1, 8 1, 8 4, -2 4, -2 6, 8 6, 8 9, -5 9, -5 1), "
              "(2 2, 2 3, 6 3, 6 2, 2 2))",
          .expected_wkt = "MULTIPOLYGON (((0 1, 0 4, 8 4, 8 1, 0 1), "
                          "(2 2, 2 3, 6 3, 6 2, 2 2)), "
                          "((0 6, 0 9, 8 9, 8 6, 0 6)))",
      },
      {
          .name = "exact_match_hole_yields_empty",
          .input_wkt = "POLYGON ((-5 -5, 15 -5, 15 15, -5 15, -5 -5), "
                       "(0 0, 0 10, 10 10, 10 0, 0 0))",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    ExpectMatchesWkt(Clip(tc.input_wkt).get(), tc.expected_wkt);
  }
}

TEST_F(RectangleIntersectionTest, ClipsPolygonBoundaryToLinestrings) {
  constexpr auto kCases = std::to_array<WktClipCase>({
      {
          .name = "empty_polygon",
          .input_wkt = "POLYGON EMPTY",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "strictly_inside_remains_polygon",
          .input_wkt = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
          .expected_wkt = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
      },
      {
          .name = "strictly_outside_no_holes",
          .input_wkt = "POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20))",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "covers_rect_no_holes_empty",
          .input_wkt = "POLYGON ((-5 -5, 15 -5, 15 15, -5 15, -5 -5))",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "exact_rect_boundary_excluded",
          .input_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
      },
      {
          .name = "partial_overlap_starts_outside",
          .input_wkt = "POLYGON ((-5 2, 5 2, 5 8, -5 8, -5 2))",
          .expected_wkt = "LINESTRING (0 2, 5 2, 5 8, 0 8)",
      },
      {
          .name = "partial_overlap_starts_inside_reconnects",
          .input_wkt = "POLYGON ((5 2, 5 8, -5 8, -5 2, 5 2))",
          .expected_wkt = "LINESTRING (0 2, 5 2, 5 8, 0 8)",
      },
      {
          .name = "covers_rect_with_inside_and_clipped_and_outside_holes",
          .input_wkt = "POLYGON ((-10 -10, 20 -10, 20 20, -10 20, -10 -10), "
                       "(2 2, 2 4, 4 4, 4 2, 2 2), "
                       "(6 6, 12 6, 12 8, 6 8, 6 6), "
                       "(12 2, 15 2, 15 4, 12 4, 12 2))",
          .expected_wkt = "GEOMETRYCOLLECTION ("
                          "POLYGON ((2 2, 2 4, 4 4, 4 2, 2 2)), "
                          "LINESTRING (10 6, 6 6, 6 8, 10 8))",
      },
      {
          .name = "clipped_shell_and_inside_hole",
          .input_wkt = "POLYGON ((-5 1, 8 1, 8 9, -5 9, -5 1), "
                       "(2 3, 2 7, 6 7, 6 3, 2 3))",
          .expected_wkt = "GEOMETRYCOLLECTION ("
                          "POLYGON ((2 3, 2 7, 6 7, 6 3, 2 3)), "
                          "LINESTRING (0 1, 8 1, 8 9, 0 9))",
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    ExpectMatchesWkt(ClipBoundary(tc.input_wkt).get(), tc.expected_wkt);
  }
}

TEST_F(RectangleIntersectionTest, ClipBoundarySkipsEmptyInteriorRingInPolygon) {
  auto shell_seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          {-5.0, -5.0},
          {15.0, -5.0},
          {15.0, 15.0},
          {-5.0, 15.0},
          {-5.0, -5.0},
      });
  std::unique_ptr<LinearRing> shell =
      factory_->createLinearRing(std::move(shell_seq));
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory_->createLinearRing());
  std::unique_ptr<Polygon> poly =
      factory_->createPolygon(std::move(shell), std::move(holes));

  Geometry::Ptr clipped = RectangleIntersection::clipBoundary(*poly, rect_);
  ExpectEmptyResult(clipped.get());
}

TEST_F(RectangleIntersectionTest, ClipsMultiPolygonVariants) {
  constexpr std::string_view kMultiPoly =
      "MULTIPOLYGON ("
      "((1 6, 3 6, 3 8, 1 8, 1 6)), "
      "((5 -5, 15 -5, 15 5, 5 5, 5 -5)), "
      "((20 20, 30 20, 30 30, 20 30, 20 20)))";

  ExpectMatchesWkt(Clip("MULTIPOLYGON EMPTY").get(),
                   "GEOMETRYCOLLECTION EMPTY");
  ExpectMatchesWkt(ClipBoundary("MULTIPOLYGON EMPTY").get(),
                   "GEOMETRYCOLLECTION EMPTY");

  ExpectMatchesWkt(Clip(kMultiPoly).get(),
                   "MULTIPOLYGON ("
                   "((1 6, 3 6, 3 8, 1 8, 1 6)), "
                   "((5 0, 5 5, 10 5, 10 0, 5 0)))");

  ExpectMatchesWkt(ClipBoundary(kMultiPoly).get(),
                   "GEOMETRYCOLLECTION ("
                   "POLYGON ((1 6, 3 6, 3 8, 1 8, 1 6)), "
                   "LINESTRING (5 0, 5 5, 10 5))");
}

TEST_F(RectangleIntersectionTest, ClipsGeometryCollectionAndNestedCollections) {
  ExpectMatchesWkt(Clip("GEOMETRYCOLLECTION EMPTY").get(),
                   "GEOMETRYCOLLECTION EMPTY");
  ExpectMatchesWkt(ClipBoundary("GEOMETRYCOLLECTION EMPTY").get(),
                   "GEOMETRYCOLLECTION EMPTY");

  constexpr std::string_view kCollection =
      "GEOMETRYCOLLECTION ("
      "POINT (2 2), "
      "POINT (20 20), "
      "MULTIPOINT ((3 3), (12 12)), "
      "LINESTRING (-5 1, 15 1), "
      "MULTILINESTRING ((4 4, 6 4), (15 15, 20 20)), "
      "POLYGON ((7 6, 12 6, 12 9, 7 9, 7 6)), "
      "MULTIPOLYGON (((1 7, 3 7, 3 9, 1 9, 1 7))), "
      "GEOMETRYCOLLECTION (POINT (8 2), LINESTRING (5 -5, 5 3)))";

  ExpectMatchesWkt(Clip(kCollection).get(),
                   "GEOMETRYCOLLECTION ("
                   "POLYGON ((7 6, 7 9, 10 9, 10 6, 7 6)), "
                   "POLYGON ((1 7, 1 9, 3 9, 3 7, 1 7)), "
                   "LINESTRING (0 1, 10 1), "
                   "LINESTRING (4 4, 6 4), "
                   "LINESTRING (5 0, 5 3), "
                   "POINT (2 2), "
                   "POINT (3 3), "
                   "POINT (8 2))");

  ExpectMatchesWkt(ClipBoundary(kCollection).get(),
                   "GEOMETRYCOLLECTION ("
                   "POLYGON ((1 7, 1 9, 3 9, 3 7, 1 7)), "
                   "LINESTRING (0 1, 10 1), "
                   "LINESTRING (4 4, 6 4), "
                   "LINESTRING (10 6, 7 6, 7 9, 10 9), "
                   "LINESTRING (5 0, 5 3), "
                   "POINT (2 2), "
                   "POINT (3 3), "
                   "POINT (8 2))");
}

TEST_F(RectangleIntersectionTest, ThrowsOnUnsupportedCurvedGeometries) {
  Geometry::Ptr circular = ReadWkt("CIRCULARSTRING (0 0, 5 5, 10 0)");
  EXPECT_THROW(RectangleIntersection::clip(*circular, rect_),
               UnsupportedOperationException);
  EXPECT_THROW(RectangleIntersection::clipBoundary(*circular, rect_),
               UnsupportedOperationException);

  Geometry::Ptr compound =
      ReadWkt("COMPOUNDCURVE ((0 0, 5 5), CIRCULARSTRING (5 5, 7 7, 10 0))");
  EXPECT_THROW(RectangleIntersection::clip(*compound, rect_),
               UnsupportedOperationException);
  EXPECT_THROW(RectangleIntersection::clipBoundary(*compound, rect_),
               UnsupportedOperationException);

  Geometry::Ptr curve_poly =
      ReadWkt("CURVEPOLYGON (CIRCULARSTRING (2 2, 6 6, 2 10, -2 6, 2 2))");
  EXPECT_THROW(RectangleIntersection::clip(*curve_poly, rect_),
               UnsupportedOperationException);
  EXPECT_THROW(RectangleIntersection::clipBoundary(*curve_poly, rect_),
               UnsupportedOperationException);

  Geometry::Ptr multi_curve =
      ReadWkt("MULTICURVE (CIRCULARSTRING (0 0, 5 5, 10 0))");
  EXPECT_THROW(RectangleIntersection::clip(*multi_curve, rect_),
               UnsupportedOperationException);
  EXPECT_THROW(RectangleIntersection::clipBoundary(*multi_curve, rect_),
               UnsupportedOperationException);

  Geometry::Ptr multi_surface = ReadWkt(
      "MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (2 2, 6 6, 2 10, -2 6, 2 "
      "2)))");
  EXPECT_THROW(RectangleIntersection::clip(*multi_surface, rect_),
               UnsupportedOperationException);
  EXPECT_THROW(RectangleIntersection::clipBoundary(*multi_surface, rect_),
               UnsupportedOperationException);

  Geometry::Ptr nested_curve = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (5 5), CIRCULARSTRING (0 0, 5 5, 10 0))");
  EXPECT_THROW(RectangleIntersection::clip(*nested_curve, rect_),
               UnsupportedOperationException);
  EXPECT_THROW(RectangleIntersection::clipBoundary(*nested_curve, rect_),
               UnsupportedOperationException);
}

TEST_F(RectangleIntersectionTest, PreservesAndPopulatesZCoordinates) {
  Geometry::Ptr point_inside = ReadWkt("POINT Z (5 5 42.5)");
  Geometry::Ptr clipped_point =
      RectangleIntersection::clip(*point_inside, rect_);
  ASSERT_NE(clipped_point, nullptr);
  ASSERT_EQ(clipped_point->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
  const auto* pt = dynamic_cast<const Point*>(clipped_point.get());
  ASSERT_NE(pt, nullptr);
  EXPECT_TRUE(pt->hasZ());
  EXPECT_DOUBLE_EQ(pt->getZ(), 42.5);

  Geometry::Ptr point_outside = ReadWkt("POINT Z (-5 5 42.5)");
  Geometry::Ptr clipped_empty =
      RectangleIntersection::clip(*point_outside, rect_);
  ExpectEmptyResult(clipped_empty.get());

  Geometry::Ptr line_x = ReadWkt("LINESTRING Z (-5 5 10, 15 5 30)");
  Geometry::Ptr clipped_line_x = RectangleIntersection::clip(*line_x, rect_);
  ASSERT_NE(clipped_line_x, nullptr);
  EXPECT_TRUE(clipped_line_x->hasZ());
  std::unique_ptr<CoordinateSequence> line_x_coords =
      clipped_line_x->getCoordinates();
  ASSERT_EQ(line_x_coords->getSize(), 2u);
  EXPECT_DOUBLE_EQ(line_x_coords->getAt(0).x, 0.0);
  EXPECT_DOUBLE_EQ(line_x_coords->getAt(0).y, 5.0);
  EXPECT_DOUBLE_EQ(line_x_coords->getAt(0).z, 15.0);
  EXPECT_DOUBLE_EQ(line_x_coords->getAt(1).x, 10.0);
  EXPECT_DOUBLE_EQ(line_x_coords->getAt(1).y, 5.0);
  EXPECT_DOUBLE_EQ(line_x_coords->getAt(1).z, 25.0);

  Geometry::Ptr line_y = ReadWkt("LINESTRING Z (5 -5 10, 5 15 50)");
  Geometry::Ptr clipped_line_y =
      RectangleIntersection::clipBoundary(*line_y, rect_);
  ASSERT_NE(clipped_line_y, nullptr);
  EXPECT_TRUE(clipped_line_y->hasZ());
  std::unique_ptr<CoordinateSequence> line_y_coords =
      clipped_line_y->getCoordinates();
  ASSERT_EQ(line_y_coords->getSize(), 2u);
  EXPECT_DOUBLE_EQ(line_y_coords->getAt(0).y, 0.0);
  EXPECT_DOUBLE_EQ(line_y_coords->getAt(0).z, 20.0);
  EXPECT_DOUBLE_EQ(line_y_coords->getAt(1).y, 10.0);
  EXPECT_DOUBLE_EQ(line_y_coords->getAt(1).z, 40.0);

  Geometry::Ptr poly_z =
      ReadWkt("POLYGON Z ((-5 -5 10, 8 -5 10, 8 8 30, -5 8 30, -5 -5 10))");
  Geometry::Ptr clipped_poly_z = RectangleIntersection::clip(*poly_z, rect_);
  ExpectMatchesWkt(clipped_poly_z.get(), "POLYGON ((0 0, 0 8, 8 8, 8 0, 0 0))");
  EXPECT_TRUE(clipped_poly_z->hasZ());
  std::unique_ptr<CoordinateSequence> poly_z_coords =
      clipped_poly_z->getCoordinates();
  ASSERT_EQ(poly_z_coords->getSize(), 5u);
  for (std::size_t i = 0; i < poly_z_coords->getSize(); ++i) {
    const Coordinate& c = poly_z_coords->getAt(i);
    EXPECT_FALSE(std::isnan(c.z)) << "index=" << i;
    EXPECT_GE(c.z, 10.0);
    EXPECT_LE(c.z, 30.0);
  }

  Geometry::Ptr cover_poly_z =
      ReadWkt("POLYGON Z ((-5 -5 10, 15 -5 20, 15 15 30, -5 15 40, -5 -5 10))");
  Geometry::Ptr clipped_cover_z =
      RectangleIntersection::clip(*cover_poly_z, rect_);
  ExpectMatchesWkt(clipped_cover_z.get(),
                   "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
  EXPECT_TRUE(clipped_cover_z->hasZ());
  std::unique_ptr<CoordinateSequence> cover_z_coords =
      clipped_cover_z->getCoordinates();
  ASSERT_EQ(cover_z_coords->getSize(), 5u);
  for (std::size_t i = 0; i < cover_z_coords->getSize(); ++i) {
    const Coordinate& c = cover_z_coords->getAt(i);
    EXPECT_FALSE(std::isnan(c.z)) << "index=" << i;
    EXPECT_GE(c.z, 10.0);
    EXPECT_LE(c.z, 40.0);
  }
}

TEST_F(RectangleIntersectionTest, ClipsWithNegativeAndFractionalRectangle) {
  const Rectangle shifted(-10.5, -5.5, 4.5, 9.5);

  ExpectMatchesWkt(Clip("POINT (-2 3)", shifted).get(), "POINT (-2 3)");
  ExpectMatchesWkt(Clip("POINT (4.5 3)", shifted).get(),
                   "GEOMETRYCOLLECTION EMPTY");
  ExpectMatchesExactOrder(Clip("LINESTRING (-15.5 2, 9.5 2)", shifted).get(),
                          "LINESTRING (-10.5 2, 4.5 2)");
  ExpectMatchesWkt(
      Clip("POLYGON ((-20 -10, 10 -10, 10 20, -20 20, -20 -10))", shifted)
          .get(),
      "POLYGON ((-10.5 -5.5, -10.5 9.5, 4.5 9.5, 4.5 -5.5, -10.5 -5.5))");
}

}  // namespace
}  // namespace geos_google
