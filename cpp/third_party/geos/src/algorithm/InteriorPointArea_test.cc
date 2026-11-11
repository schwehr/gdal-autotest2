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

// Unit tests for third_party/geos/src/algorithm/InteriorPointArea.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Tests WKTReader single-threadedly (CLocalizer is not thread-safe) and uses
//   CurrentThreadInterrupt for interruption checks.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/InteriorPointArea.cpp

#include "third_party/geos/include/geos/algorithm/InteriorPointArea.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"

namespace geos_google {
namespace {

using geos::algorithm::InteriorPointArea;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::util::CurrentThreadInterrupt;
using geos::util::GEOSException;

struct EmptyOrNonArealCase {
  std::string_view name;
  std::string_view wkt;
};

struct WktPointCase {
  std::string_view name;
  std::string_view wkt;
  double expected_x;
  double expected_y;
};

TEST(InteriorPointAreaTest, EmptyAndNonArealGeometriesReturnFalse) {
  constexpr auto kCases = std::to_array<EmptyOrNonArealCase>({
      {.name = "empty_polygon", .wkt = "POLYGON EMPTY"},
      {.name = "empty_multipolygon", .wkt = "MULTIPOLYGON EMPTY"},
      {.name = "empty_collection", .wkt = "GEOMETRYCOLLECTION EMPTY"},
      {.name = "point", .wkt = "POINT (5 5)"},
      {.name = "linestring", .wkt = "LINESTRING (0 0, 10 10)"},
      {.name = "multipoint", .wkt = "MULTIPOINT ((0 0), (5 5))"},
      {.name = "multilinestring",
       .wkt = "MULTILINESTRING ((0 0, 2 2), (3 3, 4 4))"},
      {.name = "mixed_non_areal_collection",
       .wkt = "GEOMETRYCOLLECTION (POINT (1 1), LINESTRING (0 0, 5 5), "
              "POLYGON EMPTY)"},
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> geom = reader.read(std::string(tc.wkt));
    ASSERT_NE(geom, nullptr);

    InteriorPointArea ipa(geom.get());
    Coordinate pt(123.0, 456.0, 789.0);
    EXPECT_FALSE(ipa.getInteriorPoint(pt));
    // Unset interior point leaves output parameter untouched.
    EXPECT_DOUBLE_EQ(pt.x, 123.0);
    EXPECT_DOUBLE_EQ(pt.y, 456.0);
    EXPECT_DOUBLE_EQ(pt.z, 789.0);
  }
}

TEST(InteriorPointAreaTest, SimplePolygonsVerticalAndSlopedEdges) {
  constexpr auto kCases = std::to_array<WktPointCase>({
      {
          .name = "axis_aligned_rectangle",
          .wkt = "POLYGON ((0 0, 10 0, 10 4, 0 4, 0 0))",
          .expected_x = 5.0,
          .expected_y = 2.0,
      },
      {
          .name = "slanted_parallelogram",
          .wkt = "POLYGON ((0 0, 4 0, 6 4, 2 4, 0 0))",
          .expected_x = 3.0,
          .expected_y = 2.0,
      },
      {
          .name = "triangle",
          .wkt = "POLYGON ((0 0, 10 0, 2 8, 0 0))",
          .expected_x = 3.5,
          .expected_y = 4.0,
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> geom = reader.read(std::string(tc.wkt));
    ASSERT_NE(geom, nullptr);

    InteriorPointArea ipa(geom.get());
    Coordinate pt(0.0, 0.0, 42.0);
    ASSERT_TRUE(ipa.getInteriorPoint(pt));
    EXPECT_DOUBLE_EQ(pt.x, tc.expected_x);
    EXPECT_DOUBLE_EQ(pt.y, tc.expected_y);
    EXPECT_TRUE(std::isnan(pt.z));

    std::unique_ptr<Point> pt_geom = factory->createPoint(pt);
    EXPECT_TRUE(geom->contains(pt_geom.get()));
  }
}

TEST(InteriorPointAreaTest, AsymmetricVerticesNarrowScanLineInterval) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Vertex Y ordinates {0, 2, 5, 8, 10} around centreY = 5 narrow [loY, hiY]
  // to [5, 8], yielding scan line Y = 6.5 and right intersection X = 8.0.
  std::unique_ptr<Geometry> poly =
      reader.read("POLYGON ((0 0, 8 0, 10 2, 6 5, 10 8, 8 10, 0 10, 0 0))");
  ASSERT_NE(poly, nullptr);

  InteriorPointArea ipa(poly.get());
  Coordinate pt;
  ASSERT_TRUE(ipa.getInteriorPoint(pt));
  EXPECT_DOUBLE_EQ(pt.x, 4.0);
  EXPECT_DOUBLE_EQ(pt.y, 6.5);

  std::unique_ptr<Point> pt_geom = factory->createPoint(pt);
  EXPECT_TRUE(poly->contains(pt_geom.get()));
}

TEST(InteriorPointAreaTest, PolygonsWithHolesAboveBelowAndCrossingScanLine) {
  constexpr auto kCases = std::to_array<WktPointCase>({
      {
          // Lower hole [1,3]x[1,3] narrows loY to 3; upper hole [1,3]x[7,9]
          // narrows hiY to 7. Scan line y = 5 skips both holes by envelope.
          .name = "holes_above_and_below_scan_line",
          .wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                 "(1 1, 3 1, 3 3, 1 3, 1 1), "
                 "(1 7, 3 7, 3 9, 1 9, 1 7))",
          .expected_x = 5.0,
          .expected_y = 5.0,
      },
      {
          // Hole at x in [2, 6] leaves [0, 2] (width 2) and [6, 20] (width 14).
          .name = "hole_splitting_scan_line_widest_right",
          .wkt = "POLYGON ((0 0, 20 0, 20 10, 0 10, 0 0), "
                 "(2 2, 6 2, 6 8, 2 8, 2 2))",
          .expected_x = 13.0,
          .expected_y = 5.0,
      },
      {
          // Hole at x in [14, 18] leaves [0, 14] (width 14) and [18, 20] (width
          // 2).
          .name = "hole_splitting_scan_line_widest_left",
          .wkt = "POLYGON ((0 0, 20 0, 20 10, 0 10, 0 0), "
                 "(14 2, 18 2, 18 8, 14 8, 14 2))",
          .expected_x = 7.0,
          .expected_y = 5.0,
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> poly = reader.read(std::string(tc.wkt));
    ASSERT_NE(poly, nullptr);

    InteriorPointArea ipa(poly.get());
    Coordinate pt;
    ASSERT_TRUE(ipa.getInteriorPoint(pt));
    EXPECT_DOUBLE_EQ(pt.x, tc.expected_x);
    EXPECT_DOUBLE_EQ(pt.y, tc.expected_y);

    std::unique_ptr<Point> pt_geom = factory->createPoint(pt);
    EXPECT_TRUE(poly->contains(pt_geom.get()));
  }
}

TEST(InteriorPointAreaTest, CombPolygonSelectsWidestOfMultipleIntervals) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Base [0, 20] x [0, 2] with three upward prongs of widths 2, 6, and 1.
  // Vertices have Y in {0, 2, 10}, so scan line Y = (2 + 10) / 2 = 6.0.
  std::unique_ptr<Geometry> comb = reader.read(
      "POLYGON ((20 0, 20 10, 19 10, 19 2, 11 2, 11 10, 5 10, 5 2, "
      "2 2, 2 10, 0 10, 0 0, 20 0))");
  ASSERT_NE(comb, nullptr);

  InteriorPointArea ipa(comb.get());
  Coordinate pt;
  ASSERT_TRUE(ipa.getInteriorPoint(pt));
  EXPECT_DOUBLE_EQ(pt.x, 8.0);
  EXPECT_DOUBLE_EQ(pt.y, 6.0);

  std::unique_ptr<Point> pt_geom = factory->createPoint(pt);
  EXPECT_TRUE(comb->contains(pt_geom.get()));
}

TEST(InteriorPointAreaTest, EdgesAndVerticesCoincidentWithScanLine) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const double eps = std::numeric_limits<double>::epsilon();

  // Because loY = 1.0 and hiY = 1.0 + eps, IEEE-754 tie-to-even rounding of
  // (hiY + loY) / 2.0 produces scanY == 1.0, exercising horizontal-on-scanline
  // and upward/downward endpoint-on-scanline crossing rules.
  std::unique_ptr<Polygon> poly = factory->createPolygon(CoordinateSequence{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{10.0, 0.0},
      CoordinateXY{10.0, 1.0},
      CoordinateXY{8.0, 1.0},
      CoordinateXY{5.0, 1.0 + eps},
      CoordinateXY{10.0, 2.0},
      CoordinateXY{0.0, 2.0},
      CoordinateXY{0.0, 1.0},
      CoordinateXY{0.0, 0.0},
  });
  ASSERT_NE(poly, nullptr);

  InteriorPointArea ipa(poly.get());
  Coordinate pt;
  ASSERT_TRUE(ipa.getInteriorPoint(pt));
  EXPECT_DOUBLE_EQ(pt.x, 4.0);
  EXPECT_DOUBLE_EQ(pt.y, 1.0);

  std::unique_ptr<Point> pt_geom = factory->createPoint(pt);
  EXPECT_TRUE(poly->contains(pt_geom.get()));
}

TEST(InteriorPointAreaTest, DegenerateZeroAreaPolygonsReturnVertexFallback) {
  constexpr auto kCases = std::to_array<WktPointCase>({
      {
          // All edges are horizontal along scanY = 5 => crossings is empty.
          .name = "collapsed_horizontal_segment",
          .wkt = "POLYGON ((2 5, 10 5, 6 5, 2 5))",
          .expected_x = 2.0,
          .expected_y = 5.0,
      },
      {
          // Vertical segment crossings have zero width (0.0 > 0.0 is false).
          .name = "collapsed_vertical_segment",
          .wkt = "POLYGON ((3 0, 3 4, 3 2, 3 0))",
          .expected_x = 3.0,
          .expected_y = 0.0,
      },
      {
          .name = "collapsed_single_point",
          .wkt = "POLYGON ((7 8, 7 8, 7 8, 7 8))",
          .expected_x = 7.0,
          .expected_y = 8.0,
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> geom = reader.read(std::string(tc.wkt));
    ASSERT_NE(geom, nullptr);

    InteriorPointArea ipa(geom.get());
    Coordinate pt;
    ASSERT_TRUE(ipa.getInteriorPoint(pt));
    EXPECT_DOUBLE_EQ(pt.x, tc.expected_x);
    EXPECT_DOUBLE_EQ(pt.y, tc.expected_y);
  }
}

TEST(InteriorPointAreaTest, MultiPolygonAndGeometryCollectionSelectWidest) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // MultiPolygon with widths 4, 12, then 6: selects the second polygon.
  {
    std::unique_ptr<Geometry> mp = reader.read(
        "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
        "((10 0, 22 0, 22 4, 10 4, 10 0)), "
        "((30 0, 36 0, 36 4, 30 4, 30 0)))");
    ASSERT_NE(mp, nullptr);

    InteriorPointArea ipa(mp.get());
    Coordinate pt;
    ASSERT_TRUE(ipa.getInteriorPoint(pt));
    EXPECT_DOUBLE_EQ(pt.x, 16.0);
    EXPECT_DOUBLE_EQ(pt.y, 2.0);

    std::unique_ptr<Point> pt_geom = factory->createPoint(pt);
    EXPECT_TRUE(mp->contains(pt_geom.get()));
  }

  // Positive-width polygon followed by zero-area collapsed polygon keeps first.
  {
    std::unique_ptr<Geometry> mp = reader.read(
        "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
        "((20 5, 30 5, 25 5, 20 5)))");
    ASSERT_NE(mp, nullptr);

    InteriorPointArea ipa(mp.get());
    Coordinate pt;
    ASSERT_TRUE(ipa.getInteriorPoint(pt));
    EXPECT_DOUBLE_EQ(pt.x, 2.0);
    EXPECT_DOUBLE_EQ(pt.y, 2.0);
  }

  // MultiPolygon of only zero-area polygons still returns first vertex.
  {
    std::unique_ptr<Geometry> mp = reader.read(
        "MULTIPOLYGON (((1 5, 9 5, 5 5, 1 5)), "
        "((20 5, 30 5, 25 5, 20 5)))");
    ASSERT_NE(mp, nullptr);

    InteriorPointArea ipa(mp.get());
    Coordinate pt;
    ASSERT_TRUE(ipa.getInteriorPoint(pt));
    EXPECT_DOUBLE_EQ(pt.x, 1.0);
    EXPECT_DOUBLE_EQ(pt.y, 5.0);
  }

  // Heterogeneous nested GeometryCollection picks widest polygon component.
  {
    std::unique_ptr<Geometry> gc = reader.read(
        "GEOMETRYCOLLECTION ("
        "POINT (100 100), "
        "LINESTRING (0 0, 100 0), "
        "POLYGON EMPTY, "
        "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
        "((10 0, 20 0, 20 2, 10 2, 10 0))), "
        "POLYGON ((0 10, 4 10, 4 14, 0 14, 0 10)))");
    ASSERT_NE(gc, nullptr);

    InteriorPointArea ipa(gc.get());
    Coordinate pt;
    ASSERT_TRUE(ipa.getInteriorPoint(pt));
    EXPECT_DOUBLE_EQ(pt.x, 15.0);
    EXPECT_DOUBLE_EQ(pt.y, 1.0);
  }
}

TEST(InteriorPointAreaTest, InterruptionThrowsDuringCollectionProcessing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> mp = reader.read(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((10 0, 14 0, 14 4, 10 4, 10 0)), "
      "((20 0, 24 0, 24 4, 20 4, 20 0)))");
  ASSERT_NE(mp, nullptr);

  struct InterruptState {
    int checks = 0;
    int interrupt_on_check = 2;
  } state;

  auto* prev_cb = CurrentThreadInterrupt::registerCallback(
      [](void* data) -> int {
        auto* s = static_cast<InterruptState*>(data);
        ++s->checks;
        return s->checks >= s->interrupt_on_check ? 1 : 0;
      },
      &state);
  absl::Cleanup restore_cb([prev_cb]() {
    CurrentThreadInterrupt::registerCallback(prev_cb, nullptr);
  });

  try {
    InteriorPointArea ipa(mp.get());
    FAIL() << "Expected InterruptedException to be thrown";
  } catch (const GEOSException& e) {
    EXPECT_NE(std::string_view(e.what()).find("Interrupted"),
              std::string_view::npos);
  }
  EXPECT_EQ(state.checks, 2);
}

}  // namespace
}  // namespace geos_google
