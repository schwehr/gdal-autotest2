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

// Unit tests for third_party/geos/src/operation/polygonize/BuildArea.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Each test uses isolated GeometryFactory::create() instances rather than
//   GeometryFactory::getDefaultInstance() for TSan safety.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/polygonize/BuildArea.cpp

#include "third_party/geos/include/geos/operation/polygonize/BuildArea.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <thread>  // NOLINT
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LineString;
using geos::geom::MultiPolygon;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::polygonize::BuildArea;
using geos::util::UnsupportedOperationException;

static_assert(std::is_default_constructible_v<BuildArea>);
static_assert(std::is_nothrow_destructible_v<BuildArea>);
static_assert(std::is_same_v<decltype(std::declval<BuildArea&>().build(
                                 std::declval<const Geometry*>())),
                             std::unique_ptr<Geometry>>);

struct EmptyBuildCase {
  std::string_view name;
  std::string_view wkt;
};

constexpr auto kEmptyBuildCases = std::to_array<EmptyBuildCase>({
    {
        .name = "empty_point",
        .wkt = "POINT EMPTY",
    },
    {
        .name = "point",
        .wkt = "POINT (3 4)",
    },
    {
        .name = "multipoint",
        .wkt = "MULTIPOINT ((0 0), (5 5))",
    },
    {
        .name = "empty_linestring",
        .wkt = "LINESTRING EMPTY",
    },
    {
        .name = "open_linestring",
        .wkt = "LINESTRING (0 0, 10 0, 10 10)",
    },
    {
        .name = "dangling_multilinestring",
        .wkt = "MULTILINESTRING ((0 0, 5 0), (5 0, 5 5))",
    },
    {
        .name = "empty_polygon",
        .wkt = "POLYGON EMPTY",
    },
    {
        .name = "empty_multipolygon",
        .wkt = "MULTIPOLYGON EMPTY",
    },
    {
        .name = "empty_collection",
        .wkt = "GEOMETRYCOLLECTION EMPTY",
    },
    {
        .name = "collection_without_rings",
        .wkt = "GEOMETRYCOLLECTION (POINT (1 1), LINESTRING (0 0, 2 2))",
    },
    {
        .name = "non_noded_self_intersecting_bowtie",
        .wkt = "LINESTRING (0 0, 10 10, 10 0, 0 10, 0 0)",
    },
    {
        .name = "crossing_open_lines",
        .wkt = "MULTILINESTRING ((0 0, 10 10), (0 10, 10 0))",
    },
    {
        .name = "degenerate_collapsed_ring",
        .wkt = "LINESTRING (0 0, 1 0, 0 0)",
    },
});

struct SinglePolygonCase {
  std::string_view name;
  std::string_view input_wkt;
  std::string_view expected_wkt;
  double expected_area;
};

constexpr auto kSinglePolygonCases = std::to_array<SinglePolygonCase>({
    {
        .name = "cw_closed_linestring",
        .input_wkt = "LINESTRING (0 0, 0 10, 10 10, 10 0, 0 0)",
        .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
        .expected_area = 100.0,
    },
    {
        .name = "ccw_closed_linestring",
        .input_wkt = "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)",
        .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
        .expected_area = 100.0,
    },
    {
        .name = "four_unordered_edges_forming_square",
        .input_wkt = "MULTILINESTRING ((10 0, 10 10), (0 0, 0 10), "
                     "(10 10, 0 10), (0 0, 10 0))",
        .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
        .expected_area = 100.0,
    },
    {
        .name = "square_with_attached_dangle",
        .input_wkt = "MULTILINESTRING ((0 0, 10 0, 10 10, 0 10, 0 0), "
                     "(0 0, -5 -5))",
        .expected_wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))",
        .expected_area = 100.0,
    },
    {
        .name = "triangle_polygon",
        .input_wkt = "POLYGON ((0 0, 4 0, 4 3, 0 0))",
        .expected_wkt = "POLYGON ((0 0, 4 0, 4 3, 0 0))",
        .expected_area = 6.0,
    },
    {
        .name = "mixed_collection_with_single_ring",
        .input_wkt = "GEOMETRYCOLLECTION (POINT (99 99), "
                     "LINESTRING (-2 -2, -1 -1), "
                     "LINESTRING (0 0, 5 0, 5 4, 0 4, 0 0))",
        .expected_wkt = "POLYGON ((0 0, 0 4, 5 4, 5 0, 0 0))",
        .expected_area = 20.0,
    },
});

class BuildAreaTest : public ::testing::Test {
 protected:
  BuildAreaTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  Geometry::Ptr Read(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(BuildAreaTest, EmptyAndNonArealInputsReturnEmptyCollection) {
  BuildArea builder;
  for (const EmptyBuildCase& tc : kEmptyBuildCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr input = Read(tc.wkt);
    input->setSRID(4326);

    Geometry::Ptr result = builder.build(input.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(),
              GeometryTypeId::GEOS_GEOMETRYCOLLECTION);
    EXPECT_TRUE(result->isEmpty());
    EXPECT_EQ(result->getNumGeometries(), 0u);
    EXPECT_DOUBLE_EQ(result->getArea(), 0.0);
    EXPECT_EQ(result->getSRID(), 4326);
  }
}

TEST_F(BuildAreaTest, SingleClosedRingAndComponentInputs) {
  BuildArea builder;
  for (const SinglePolygonCase& tc : kSinglePolygonCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr input = Read(tc.input_wkt);
    input->setSRID(3857);
    Geometry::Ptr expected = Read(tc.expected_wkt);

    Geometry::Ptr result = builder.build(input.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(result->getSRID(), 3857);
    EXPECT_NEAR(result->getArea(), tc.expected_area, 1e-9);

    result->normalize();
    expected->normalize();
    EXPECT_TRUE(result->equalsExact(expected.get(), 1e-9));
  }
}

TEST_F(BuildAreaTest, PolygonWithSingleHoleFromLineworkAndPolygon) {
  BuildArea builder;
  constexpr std::string_view kExpectedWkt =
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))";
  constexpr std::array<std::string_view, 2> kInputs = {
      "MULTILINESTRING ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2))",
      kExpectedWkt,
  };

  Geometry::Ptr expected = Read(kExpectedWkt);
  expected->normalize();

  for (std::string_view input_wkt : kInputs) {
    SCOPED_TRACE(input_wkt);
    Geometry::Ptr input = Read(input_wkt);
    input->setSRID(32610);

    Geometry::Ptr result = builder.build(input.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(result->getSRID(), 32610);
    EXPECT_NEAR(result->getArea(), 64.0, 1e-9);

    const auto* poly = dynamic_cast<const Polygon*>(result.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 1u);

    result->normalize();
    EXPECT_TRUE(result->equalsExact(expected.get(), 1e-9));
  }
}

TEST_F(BuildAreaTest, MultiEdgeHoleMatchesWithNonZeroStartOffset) {
  // Split the inner ring across two edges so Polygonizer constructs the CCW
  // hole starting at (2, 2) and the matching CW shell starting at (8, 8)
  // (offset == 2 in ringsEqualAnyDirection).
  Geometry::Ptr input = Read(
      "MULTILINESTRING ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8), (8 8, 2 8, 2 2))");
  Geometry::Ptr expected = Read(
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))");

  BuildArea builder;
  Geometry::Ptr result = builder.build(input.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_TRUE(result->isValid());
  EXPECT_NEAR(result->getArea(), 64.0, 1e-9);

  result->normalize();
  expected->normalize();
  EXPECT_TRUE(result->equalsExact(expected.get(), 1e-9));
}

TEST_F(BuildAreaTest, NestedRingsAlternateEvenAndOddAncestors) {
  BuildArea builder;

  // Three nested rings: outer L0 (kept), hole L1 (dropped), island L2 (kept).
  // Supply innermost-first to exercise sorting by envelope area.
  Geometry::Ptr three_levels = Read(
      "MULTILINESTRING ("
      "(4 4, 16 4, 16 16, 4 16, 4 4), "
      "(2 2, 18 2, 18 18, 2 18, 2 2), "
      "(0 0, 20 0, 20 20, 0 20, 0 0))");
  Geometry::Ptr expected_three = Read(
      "MULTIPOLYGON ("
      "((0 0, 0 20, 20 20, 20 0, 0 0), (2 2, 18 2, 18 18, 2 18, 2 2)), "
      "((4 4, 4 16, 16 16, 16 4, 4 4)))");

  Geometry::Ptr res_three = builder.build(three_levels.get());
  ASSERT_NE(res_three, nullptr);
  EXPECT_EQ(res_three->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(res_three->isValid());
  EXPECT_NEAR(res_three->getArea(), 400.0 - 256.0 + 144.0, 1e-9);

  res_three->normalize();
  expected_three->normalize();
  EXPECT_TRUE(res_three->equalsExact(expected_three.get(), 1e-9));

  // Four nested rings: L0 (0 parents), L1 (1), L2 (2), L3 (3).
  Geometry::Ptr four_levels = Read(
      "MULTILINESTRING ("
      "(0 0, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 18 2, 18 18, 2 18, 2 2), "
      "(4 4, 16 4, 16 16, 4 16, 4 4), "
      "(6 6, 14 6, 14 14, 6 14, 6 6))");
  Geometry::Ptr expected_four = Read(
      "MULTIPOLYGON ("
      "((0 0, 0 20, 20 20, 20 0, 0 0), (2 2, 18 2, 18 18, 2 18, 2 2)), "
      "((4 4, 4 16, 16 16, 16 4, 4 4), (6 6, 14 6, 14 14, 6 14, 6 6)))");

  Geometry::Ptr res_four = builder.build(four_levels.get());
  ASSERT_NE(res_four, nullptr);
  EXPECT_EQ(res_four->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(res_four->isValid());
  EXPECT_NEAR(res_four->getArea(), (400.0 - 256.0) + (144.0 - 64.0), 1e-9);

  const auto* mp = dynamic_cast<const MultiPolygon*>(res_four.get());
  ASSERT_NE(mp, nullptr);
  ASSERT_EQ(mp->getNumGeometries(), 2u);
  EXPECT_EQ(mp->getGeometryN(0)->getNumInteriorRing(), 1u);
  EXPECT_EQ(mp->getGeometryN(1)->getNumInteriorRing(), 1u);

  res_four->normalize();
  expected_four->normalize();
  EXPECT_TRUE(res_four->equalsExact(expected_four.get(), 1e-9));
}

TEST_F(BuildAreaTest, MultipleHolesExerciseParentReuseAndRingRejectionPaths) {
  // Outer shell [0,30] x [0,20] with three distinct holes:
  // - Hole A: [2,12] x [2,12] (5 pts) containing island [4,10] x [4,10] (5 pts)
  // - Hole B: triangle (16,2)-(24,2)-(20,10)-(16,2) (4 pts vs 5 pts)
  // - Hole C: [22,26] x [12,16] (5 pts, different envelope from island)
  Geometry::Ptr input = Read(
      "MULTILINESTRING ("
      "(0 0, 30 0, 30 20, 0 20, 0 0), "
      "(2 2, 12 2, 12 12, 2 12, 2 2), "
      "(4 4, 10 4, 10 10, 4 10, 4 4), "
      "(16 2, 24 2, 20 10, 16 2), "
      "(22 12, 26 12, 26 16, 22 16, 22 12))");
  Geometry::Ptr expected = Read(
      "MULTIPOLYGON ("
      "((0 0, 0 20, 30 20, 30 0, 0 0), "
      "(2 2, 12 2, 12 12, 2 12, 2 2), "
      "(16 2, 24 2, 20 10, 16 2), "
      "(22 12, 26 12, 26 16, 22 16, 22 12)), "
      "((4 4, 4 10, 10 10, 10 4, 4 4)))");

  BuildArea builder;
  Geometry::Ptr result = builder.build(input.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(result->isValid());
  // Area: 600 - 100 + 36 - 32 - 16 = 488.
  EXPECT_NEAR(result->getArea(), 488.0, 1e-9);

  result->normalize();
  expected->normalize();
  EXPECT_TRUE(result->equalsExact(expected.get(), 1e-9));
}

TEST_F(BuildAreaTest, SplitHoleExercisesIndexOfAndBackwardMismatchBranches) {
  BuildArea builder;
  Geometry::Ptr expected = Read("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
  expected->normalize();

  // Case A: Inner square [2,8]^2 is a hole of [0,10]^2, split by a kinked
  // diagonal starting at (4,6). The split faces have 5 points and envelope
  // [2,8]^2, matching the hole's point count and envelope, while (4,6) is not
  // in the hole (offset == NO_COORD_INDEX). CascadedPolygonUnion dissolves all
  // three faces back into [0,10]^2.
  Geometry::Ptr missing_first_vertex = Read(
      "MULTILINESTRING ("
      "(0 0, 10 0, 10 10, 0 10, 0 0), "
      "(4 6, 8 8), "
      "(4 6, 2 2), "
      "(2 2, 8 2, 8 8), "
      "(2 2, 2 8, 8 8))");

  Geometry::Ptr res_a = builder.build(missing_first_vertex.get());
  ASSERT_NE(res_a, nullptr);
  EXPECT_EQ(res_a->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_TRUE(res_a->isValid());
  EXPECT_NEAR(res_a->getArea(), 100.0, 1e-9);
  res_a->normalize();
  EXPECT_TRUE(res_a->equalsExact(expected.get(), 1e-9));

  // Case B: Inner 5-point face starts at (8,8) which is present in the hole at
  // offset == 2, fails forward check, matches initial backward vertices
  // ((8,2) and (2,2)), and mismatches at (4,6) != (2,8).
  Geometry::Ptr backward_mismatch = Read(
      "MULTILINESTRING ("
      "(0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8), "
      "(2 2, 4 6, 8 8), "
      "(2 2, 2 8, 8 8))");

  Geometry::Ptr res_b = builder.build(backward_mismatch.get());
  ASSERT_NE(res_b, nullptr);
  EXPECT_EQ(res_b->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_TRUE(res_b->isValid());
  EXPECT_NEAR(res_b->getArea(), 100.0, 1e-9);
  res_b->normalize();
  EXPECT_TRUE(res_b->equalsExact(expected.get(), 1e-9));
}

TEST_F(BuildAreaTest, DissolvesSharedEdgesAndKeepsDisjointOrVertexTouching) {
  BuildArea builder;

  // Adjacent squares sharing x=5 dissolve into one rectangle. OverlayNG retains
  // the noded collinear vertices at (5, 0) and (5, 10), so assert topological
  // equality and perimeter rather than exact vertex count.
  {
    Geometry::Ptr adjacent = Read(
        "MULTIPOLYGON ("
        "((0 0, 5 0, 5 10, 0 10, 0 0)), "
        "((5 0, 10 0, 10 10, 5 10, 5 0)))");
    Geometry::Ptr expected = Read("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
    Geometry::Ptr result = builder.build(adjacent.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    EXPECT_TRUE(result->isValid());
    const auto* poly = dynamic_cast<const Polygon*>(result.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 0u);
    EXPECT_NEAR(result->getArea(), 100.0, 1e-9);
    EXPECT_NEAR(result->getLength(), 40.0, 1e-9);
    EXPECT_TRUE(result->equals(expected.get()));
  }

  // Noded 2x2 grid of unit squares produces four faces whose interior edges
  // dissolve into a single 2x2 square.
  {
    Geometry::Ptr grid = Read(
        "MULTILINESTRING ("
        "(0 0, 1 0), (1 0, 2 0), (2 0, 2 1), (2 1, 2 2), "
        "(2 2, 1 2), (1 2, 0 2), (0 2, 0 1), (0 1, 0 0), "
        "(1 0, 1 1), (1 1, 1 2), (0 1, 1 1), (1 1, 2 1))");
    Geometry::Ptr expected = Read("POLYGON ((0 0, 0 2, 2 2, 2 0, 0 0))");
    Geometry::Ptr result = builder.build(grid.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    EXPECT_TRUE(result->isValid());
    const auto* poly = dynamic_cast<const Polygon*>(result.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 0u);
    EXPECT_NEAR(result->getArea(), 4.0, 1e-9);
    EXPECT_NEAR(result->getLength(), 8.0, 1e-9);
    EXPECT_TRUE(result->equals(expected.get()));
  }

  // Four adjacent rectangles forming a closed frame dissolve into one polygon
  // with an interior hole.
  {
    Geometry::Ptr frame = Read(
        "MULTIPOLYGON ("
        "((0 0, 3 0, 3 1, 0 1, 0 0)), "
        "((2 1, 3 1, 3 3, 2 3, 2 1)), "
        "((0 2, 2 2, 2 3, 0 3, 0 2)), "
        "((0 1, 1 1, 1 2, 0 2, 0 1)))");
    Geometry::Ptr expected =
        Read("POLYGON ((0 0, 0 3, 3 3, 3 0, 0 0), (1 1, 2 1, 2 2, 1 2, 1 1))");
    Geometry::Ptr result = builder.build(frame.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    EXPECT_TRUE(result->isValid());
    const auto* poly = dynamic_cast<const Polygon*>(result.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 1u);
    EXPECT_NEAR(result->getArea(), 8.0, 1e-9);
    EXPECT_TRUE(result->equals(expected.get()));
  }

  // Two squares touching only at corner (4, 4) remain a MultiPolygon.
  {
    Geometry::Ptr corner_touching = Read(
        "MULTILINESTRING ("
        "(0 0, 4 0, 4 4, 0 4, 0 0), "
        "(4 4, 8 4, 8 8, 4 8, 4 4))");
    Geometry::Ptr result = builder.build(corner_touching.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(result->getNumGeometries(), 2u);
    EXPECT_NEAR(result->getArea(), 32.0, 1e-9);
  }

  // Two disjoint polygons each with a hole produce a valid 2-part MultiPolygon.
  {
    Geometry::Ptr two_holed = Read(
        "MULTILINESTRING ("
        "(0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2), "
        "(20 0, 30 0, 30 10, 20 10, 20 0), (22 2, 28 2, 28 8, 22 8, 22 2))");
    Geometry::Ptr result = builder.build(two_holed.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
    EXPECT_TRUE(result->isValid());
    EXPECT_NEAR(result->getArea(), 128.0, 1e-9);
    const auto* mp = dynamic_cast<const MultiPolygon*>(result.get());
    ASSERT_NE(mp, nullptr);
    ASSERT_EQ(mp->getNumGeometries(), 2u);
    EXPECT_EQ(mp->getGeometryN(0)->getNumInteriorRing(), 1u);
    EXPECT_EQ(mp->getGeometryN(1)->getNumInteriorRing(), 1u);
  }

  // Two interior holes touching at a single noded vertex (5, 5).
  {
    Geometry::Ptr touching_holes = Read(
        "MULTILINESTRING ("
        "(0 0, 10 0, 10 10, 0 10, 0 0), "
        "(5 5, 2 2, 8 2, 5 5), "
        "(5 5, 8 8, 2 8, 5 5))");
    Geometry::Ptr result = builder.build(touching_holes.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    EXPECT_TRUE(result->isValid());
    EXPECT_NEAR(result->getArea(), 100.0 - 9.0 - 9.0, 1e-9);
    const auto* poly = dynamic_cast<const Polygon*>(result.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 2u);
  }

  // Two squares joined by a bridge (cut edge) discard the bridge.
  {
    Geometry::Ptr bridged = Read(
        "MULTILINESTRING ("
        "(4 0, 4 4, 0 4, 0 0, 4 0), "
        "(4 0, 8 0), "
        "(8 0, 12 0, 12 4, 8 4, 8 0))");
    Geometry::Ptr result = builder.build(bridged.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(result->getNumGeometries(), 2u);
    EXPECT_NEAR(result->getArea(), 32.0, 1e-9);
  }
}

TEST_F(BuildAreaTest, PreservesZCoordinatesAndSridAcrossRepeatedCalls) {
  PrecisionModel fixed_pm(100.0);
  GeometryFactory::Ptr custom_factory =
      GeometryFactory::create(&fixed_pm, 4326);
  WKTReader custom_reader(custom_factory.get());
  BuildArea builder;

  // Empty path preserves SRID.
  Geometry::Ptr empty_in = custom_reader.read("LINESTRING (0 0, 1 1)");
  EXPECT_EQ(empty_in->getSRID(), 4326);
  Geometry::Ptr empty_out = builder.build(empty_in.get());
  ASSERT_NE(empty_out, nullptr);
  EXPECT_TRUE(empty_out->isEmpty());
  EXPECT_EQ(empty_out->getSRID(), 4326);

  // Single polygon path preserves SRID and Z coordinates.
  Geometry::Ptr ring_z = custom_reader.read(
      "LINESTRING Z (0 0 10, 10 0 20, 10 10 30, 0 10 40, 0 0 10)");
  Geometry::Ptr single_out = builder.build(ring_z.get());
  ASSERT_NE(single_out, nullptr);
  EXPECT_EQ(single_out->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_EQ(single_out->getSRID(), 4326);
  EXPECT_TRUE(single_out->hasZ());
  EXPECT_FALSE(single_out->hasM());
  EXPECT_NEAR(single_out->getArea(), 100.0, 1e-9);
  std::unique_ptr<CoordinateSequence> coords = single_out->getCoordinates();
  ASSERT_GE(coords->size(), 4u);
  EXPECT_DOUBLE_EQ(coords->getOrdinate(0, CoordinateSequence::Z), 10.0);
  EXPECT_TRUE(std::isnan(coords->getOrdinate(0, CoordinateSequence::M)));

  // Multi-face path preserves SRID through CascadedPolygonUnion.
  Geometry::Ptr nested_in = custom_reader.read(
      "MULTILINESTRING ("
      "(0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2), "
      "(4 4, 6 4, 6 6, 4 6, 4 4))");
  Geometry::Ptr multi_out = builder.build(nested_in.get());
  ASSERT_NE(multi_out, nullptr);
  EXPECT_EQ(multi_out->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_EQ(multi_out->getSRID(), 4326);
  EXPECT_NEAR(multi_out->getArea(), 100.0 - 36.0 + 4.0, 1e-9);
}

TEST_F(BuildAreaTest, MultipleIslandsInSingleHoleAndPinchedHole) {
  BuildArea builder;

  // Outer L0 with hole L1 containing two sibling islands L2a and L2b, where
  // L2b contains a depth-3 lake L3 (skipped due to odd parent count).
  {
    Geometry::Ptr input = Read(
        "MULTILINESTRING ("
        "(0 0, 30 0, 30 20, 0 20, 0 0), "
        "(2 2, 28 2, 28 18, 2 18, 2 2), "
        "(4 4, 12 4, 12 16, 4 16, 4 4), "
        "(16 4, 26 4, 26 16, 16 16, 16 4), "
        "(18 6, 24 6, 24 14, 18 14, 18 6))");
    Geometry::Ptr result = builder.build(input.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(result->getNumGeometries(), 3u);
    EXPECT_NEAR(result->getArea(), (600.0 - 416.0) + 96.0 + (120.0 - 48.0),
                1e-9);
  }

  // Interior hole touching exterior shell at a single shared vertex (5, 0).
  {
    Geometry::Ptr input = Read(
        "MULTILINESTRING ("
        "(0 0, 5 0), "
        "(5 0, 10 0, 10 10, 0 10, 0 0), "
        "(5 0, 8 4, 2 4, 5 0))");
    Geometry::Ptr expected = Read(
        "POLYGON ((0 0, 0 10, 10 10, 10 0, 5 0, 0 0), "
        "(5 0, 8 4, 2 4, 5 0))");
    Geometry::Ptr result = builder.build(input.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    EXPECT_TRUE(result->isValid());
    const auto* poly = dynamic_cast<const Polygon*>(result.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 1u);
    EXPECT_NEAR(result->getArea(), 88.0, 1e-9);
    EXPECT_TRUE(result->equals(expected.get()));
  }
}

TEST_F(BuildAreaTest, OverlappingPolygonsAndNestedCollectionDissolve) {
  BuildArea builder;

  // Overlapping polygons are dissolved by CascadedPolygonUnion.
  {
    Geometry::Ptr overlapping = Read(
        "MULTIPOLYGON ("
        "((0 0, 6 0, 6 6, 0 6, 0 0)), "
        "((4 4, 10 4, 10 10, 4 10, 4 4)))");
    Geometry::Ptr expected =
        Read("POLYGON ((0 0, 6 0, 6 4, 10 4, 10 10, 4 10, 4 6, 0 6, 0 0))");
    Geometry::Ptr result = builder.build(overlapping.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    EXPECT_TRUE(result->isValid());
    EXPECT_NEAR(result->getArea(), 68.0, 1e-9);
    EXPECT_TRUE(result->equals(expected.get()));
  }

  // Nested GeometryCollection combining Polygon, MultiPolygon, and a dangle.
  {
    Geometry::Ptr nested = Read(
        "GEOMETRYCOLLECTION ("
        "POLYGON ((0 0, 5 0, 5 10, 0 10, 0 0)), "
        "MULTIPOLYGON (((5 0, 10 0, 10 10, 5 10, 5 0))), "
        "LINESTRING (100 100, 101 101))");
    Geometry::Ptr expected = Read("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
    Geometry::Ptr result = builder.build(nested.get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
    EXPECT_TRUE(result->isValid());
    EXPECT_NEAR(result->getArea(), 100.0, 1e-9);
    EXPECT_TRUE(result->equals(expected.get()));
  }
}

TEST_F(BuildAreaTest, ThrowsUnsupportedOperationOnCurvedGeometries) {
  BuildArea builder;
  constexpr std::array<std::string_view, 4> kCurvedWkts = {
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "COMPOUNDCURVE (CIRCULARSTRING (0 0, 2 0, 2 2), (2 2, 0 2, 0 0))",
      "CURVEPOLYGON (COMPOUNDCURVE (CIRCULARSTRING (0 0, 2 0, 2 2), "
      "(2 2, 0 2, 0 0)))",
      "GEOMETRYCOLLECTION (CIRCULARSTRING (0 0, 1 1, 2 0))",
  };

  for (std::string_view wkt : kCurvedWkts) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr curved = Read(wkt);
    EXPECT_THROW(builder.build(curved.get()), UnsupportedOperationException);
  }
}

TEST(BuildAreaThreadSafetyTest, ConcurrentBuildCallsAreThreadSafe) {
  constexpr int kNumThreads = 4;
  constexpr int kIterations = 10;

  auto make_square = [](const GeometryFactory* gf, double lo, double hi) {
    CoordinateSequence seq{
        CoordinateXY(lo, lo), CoordinateXY(hi, lo), CoordinateXY(hi, hi),
        CoordinateXY(lo, hi), CoordinateXY(lo, lo),
    };
    return gf->createLineString(seq);
  };

  std::vector<std::thread> workers;
  workers.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    workers.emplace_back([&, t]() {
      GeometryFactory::Ptr gf = GeometryFactory::create();
      for (int i = 0; i < kIterations; ++i) {
        std::vector<std::unique_ptr<LineString>> lines;
        lines.push_back(make_square(gf.get(), 0.0, 10.0));
        lines.push_back(make_square(gf.get(), 2.0, 8.0));
        lines.push_back(make_square(gf.get(), 4.0, 6.0));
        auto input = gf->createMultiLineString(std::move(lines));
        input->setSRID(1000 + t);

        BuildArea builder;
        Geometry::Ptr out = builder.build(input.get());
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(out->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
        EXPECT_TRUE(out->isValid());
        EXPECT_EQ(out->getSRID(), 1000 + t);
        EXPECT_NEAR(out->getArea(), 100.0 - 36.0 + 4.0, 1e-9);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
}

}  // namespace
}  // namespace geos_google
