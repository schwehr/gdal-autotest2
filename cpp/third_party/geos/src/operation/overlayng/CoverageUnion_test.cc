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

// Unit tests for third_party/geos/src/operation/overlayng/CoverageUnion.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/CoverageUnion.cpp

#include "third_party/geos/include/geos/operation/overlayng/CoverageUnion.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::operation::overlayng::CoverageUnion;
using geos::util::IllegalArgumentException;
using geos::util::TopologyException;
using ::testing::HasSubstr;

struct CoverageCase {
  std::string_view name;
  std::string_view input_wkt;
  std::string_view expected_wkt;
  double expected_area;
};

constexpr auto kPolygonalCoverageCases = std::to_array<CoverageCase>({
    {
        .name = "two_adjacent_squares",
        .input_wkt = "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
                     "((1 0, 2 0, 2 1, 1 1, 1 0)))",
        .expected_wkt = "POLYGON ((0 0, 2 0, 2 1, 0 1, 0 0))",
        .expected_area = 2.0,
    },
    {
        .name = "two_by_two_grid",
        .input_wkt = "MULTIPOLYGON ("
                     "((0 0, 1 0, 1 1, 0 1, 0 0)), "
                     "((1 0, 2 0, 2 1, 1 1, 1 0)), "
                     "((0 1, 1 1, 1 2, 0 2, 0 1)), "
                     "((1 1, 2 1, 2 2, 1 2, 1 1)))",
        .expected_wkt = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
        .expected_area = 4.0,
    },
    {
        .name = "ring_of_four_trapezoids_forming_hole",
        .input_wkt = "MULTIPOLYGON ("
                     "((0 0, 3 0, 2 1, 1 1, 0 0)), "
                     "((3 0, 3 3, 2 2, 2 1, 3 0)), "
                     "((3 3, 0 3, 1 2, 2 2, 3 3)), "
                     "((0 3, 0 0, 1 1, 1 2, 0 3)))",
        .expected_wkt = "POLYGON ((0 0, 3 0, 3 3, 0 3, 0 0), "
                        "(1 1, 1 2, 2 2, 2 1, 1 1))",
        .expected_area = 8.0,
    },
    {
        .name = "polygon_with_hole_filled_by_inner_polygon",
        .input_wkt = "MULTIPOLYGON ("
                     "((0 0, 3 0, 3 3, 0 3, 0 0), (1 1, 2 1, 2 2, 1 2, 1 1)), "
                     "((1 1, 2 1, 2 2, 1 2, 1 1)))",
        .expected_wkt = "POLYGON ((0 0, 3 0, 3 3, 0 3, 0 0))",
        .expected_area = 9.0,
    },
    {
        .name = "single_polygon",
        .input_wkt = "POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0))",
        .expected_wkt = "POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0))",
        .expected_area = 6.0,
    },
    {
        .name = "disjoint_polygons",
        .input_wkt = "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
                     "((3 0, 4 0, 4 1, 3 1, 3 0)))",
        .expected_wkt = "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
                        "((3 0, 4 0, 4 1, 3 1, 3 0)))",
        .expected_area = 2.0,
    },
});

void ExpectCoverageUnionTopologyError(const Geometry* coverage,
                                      std::string_view expected_substr) {
  try {
    static_cast<void>(CoverageUnion::geomunion(coverage));
    FAIL() << "Expected TopologyException containing \"" << expected_substr
           << "\"";
  } catch (const TopologyException& ex) {
    EXPECT_THAT(std::string(ex.what()),
                HasSubstr(std::string(expected_substr)));
  }
}

TEST(CoverageUnionTest, AreaPctDiffTolConstant) {
  static_assert(CoverageUnion::AREA_PCT_DIFF_TOL == 1e-6);
  EXPECT_DOUBLE_EQ(+CoverageUnion::AREA_PCT_DIFF_TOL, 1e-6);
}

TEST(CoverageUnionTest, UnionsValidPolygonalCoverages) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const CoverageCase& tc : kPolygonalCoverageCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr input = reader.read(std::string(tc.input_wkt));
    Geometry::Ptr expected = reader.read(std::string(tc.expected_wkt));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(expected, nullptr);

    Geometry::Ptr result = CoverageUnion::geomunion(input.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_NEAR(result->getArea(), tc.expected_area, 1e-9);
    EXPECT_TRUE(result->equals(expected.get()));
  }
}

TEST(CoverageUnionTest, UnionsGeometryCollectionOfPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr input = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0)))");
  Geometry::Ptr expected = reader.read("POLYGON ((0 0, 2 0, 2 1, 0 1, 0 0))");

  Geometry::Ptr result = CoverageUnion::geomunion(input.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_NEAR(result->getArea(), 2.0, 1e-9);
  EXPECT_TRUE(result->equals(expected.get()));

  Geometry::Ptr with_empty = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "POLYGON EMPTY)");
  Geometry::Ptr expected_single =
      reader.read("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  Geometry::Ptr with_empty_result = CoverageUnion::geomunion(with_empty.get());
  ASSERT_NE(with_empty_result, nullptr);
  EXPECT_TRUE(with_empty_result->isValid());
  EXPECT_NEAR(with_empty_result->getArea(), 1.0, 1e-9);
  EXPECT_TRUE(with_empty_result->equals(expected_single.get()));
}

TEST(CoverageUnionTest, PreservesZCoordinatesInPolygonalCoverage) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr input = reader.read(
      "MULTIPOLYGON Z ("
      "((0 0 1, 1 0 2, 1 1 3, 0 1 4, 0 0 1)), "
      "((1 0 2, 2 0 5, 2 1 6, 1 1 3, 1 0 2)))");
  Geometry::Ptr result = CoverageUnion::geomunion(input.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->hasZ());
  EXPECT_TRUE(result->isValid());
  EXPECT_NEAR(result->getArea(), 2.0, 1e-9);
}

TEST(CoverageUnionTest, UnionsLinearCoverageAndDissolvesCoincidentSegments) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr single_line = reader.read("LINESTRING (0 0, 1 1, 2 0)");
  Geometry::Ptr single_result = CoverageUnion::geomunion(single_line.get());
  ASSERT_NE(single_result, nullptr);
  EXPECT_EQ(single_result->getDimension(), 1);
  EXPECT_TRUE(single_result->equals(single_line.get()));

  Geometry::Ptr network = reader.read(
      "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0), (1 0, 1 1), (0 0, 1 0))");
  Geometry::Ptr expected =
      reader.read("MULTILINESTRING ((0 0, 1 0), (1 0, 2 0), (1 0, 1 1))");

  Geometry::Ptr result = CoverageUnion::geomunion(network.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getArea(), 0.0);
  EXPECT_NEAR(result->getLength(), 3.0, 1e-9);
  EXPECT_TRUE(result->equals(expected.get()));

  Geometry::Ptr line_gc = reader.read(
      "GEOMETRYCOLLECTION ("
      "LINESTRING (0 0, 1 0), "
      "LINESTRING (1 0, 2 0), "
      "LINESTRING (0 0, 1 0))");
  Geometry::Ptr expected_line_gc =
      reader.read("MULTILINESTRING ((0 0, 1 0), (1 0, 2 0))");
  Geometry::Ptr line_gc_result = CoverageUnion::geomunion(line_gc.get());
  ASSERT_NE(line_gc_result, nullptr);
  EXPECT_EQ(line_gc_result->getDimension(), 1);
  EXPECT_EQ(line_gc_result->getArea(), 0.0);
  EXPECT_NEAR(line_gc_result->getLength(), 2.0, 1e-9);
  EXPECT_TRUE(line_gc_result->equals(expected_line_gc.get()));

  Geometry::Ptr line_z = reader.read("LINESTRING Z (0 0 5, 1 0 10)");
  Geometry::Ptr line_z_result = CoverageUnion::geomunion(line_z.get());
  ASSERT_NE(line_z_result, nullptr);
  EXPECT_TRUE(line_z_result->hasZ());
  EXPECT_EQ(line_z_result->getDimension(), 1);
  EXPECT_EQ(line_z_result->getArea(), 0.0);
  EXPECT_NEAR(line_z_result->getLength(), 1.0, 1e-9);
  EXPECT_TRUE(line_z_result->equals(line_z.get()));
}

TEST(CoverageUnionTest, UnionsPuntalAndEmptyCoverages) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // CoverageUnion uses EdgeNodingBuilder (lines/polygons only), so 0D puntal
  // inputs produce an empty 0D point result with zero area.
  for (const std::string_view puntal_wkt :
       {"POINT (3 4)", "MULTIPOINT ((0 0), (1 1), (0 0))", "POINT EMPTY"}) {
    SCOPED_TRACE(puntal_wkt);
    Geometry::Ptr puntal = reader.read(std::string(puntal_wkt));
    Geometry::Ptr result = CoverageUnion::geomunion(puntal.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isEmpty());
    EXPECT_EQ(result->getDimension(), 0);
    EXPECT_EQ(result->getArea(), 0.0);
  }

  for (const std::string_view empty_wkt :
       {"LINESTRING EMPTY", "GEOMETRYCOLLECTION EMPTY", "POLYGON EMPTY",
        "MULTIPOLYGON EMPTY"}) {
    SCOPED_TRACE(empty_wkt);
    Geometry::Ptr empty_geom = reader.read(std::string(empty_wkt));
    Geometry::Ptr result = CoverageUnion::geomunion(empty_geom.get());
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isEmpty());
    EXPECT_EQ(result->getArea(), 0.0);
  }
}

TEST(CoverageUnionTest, ThrowsOnIncorrectlyNodedPolygonalInputs) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr std::string_view kIncorrectlyNodedMsg =
      "CoverageUnion cannot process incorrectly noded inputs";

  // Un-noded overlap sharing an edge and corner triggers a side-location
  // conflict inside OverlayNG::geomunion, which CoverageUnion re-throws with
  // an "incorrectly noded inputs" TopologyException.
  Geometry::Ptr un_noded = reader.read(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((0 0, 2 0, 1 1, 0 0)))");
  ExpectCoverageUnionTopologyError(un_noded.get(), kIncorrectlyNodedMsg);

  // Cancelling the exterior shell of a polygon with a hole leaves an unplaced
  // free hole ring in PolygonBuilder, also throwing TopologyException.
  Geometry::Ptr free_hole = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 1 3, 3 3, 3 1, 1 1)), "
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)))");
  ExpectCoverageUnionTopologyError(free_hole.get(), kIncorrectlyNodedMsg);

  // Two identical squares plus an adjacent square sharing edge (1 0, 1 1)
  // cancel two copies of the shared edge, leaving one segment oriented with
  // a conflicting side location relative to the adjacent square's chain.
  Geometry::Ptr three_way_shared_edge = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0)), "
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)))");
  ExpectCoverageUnionTopologyError(three_way_shared_edge.get(),
                                   kIncorrectlyNodedMsg);
}

TEST(CoverageUnionTest, ThrowsOnOverlappingDuplicatePolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr std::string_view kOverlappingMsg =
      "CoverageUnion cannot process overlapping inputs.";

  // Two identical polygons cancel all shared boundary segments, yielding an
  // empty result (area 0) while input area is 2.0.
  Geometry::Ptr identical_pair = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)))");
  ExpectCoverageUnionTopologyError(identical_pair.get(), kOverlappingMsg);

  // Duplicate pair plus a disjoint polygon cancels the duplicate pair cleanly,
  // leaving the disjoint polygon of area 1.0 against an input area of 3.0.
  Geometry::Ptr identical_pair_plus_disjoint = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "POLYGON ((3 0, 4 0, 4 1, 3 1, 3 0)))");
  ExpectCoverageUnionTopologyError(identical_pair_plus_disjoint.get(),
                                   kOverlappingMsg);

  // Cancelling an inner shell leaves a hole inside the outer polygon, yielding
  // area 32.0 against an input area of 64.0.
  Geometry::Ptr nested_hole_cancellation = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0)), "
      "POLYGON ((1 1, 5 1, 5 5, 1 5, 1 1)), "
      "POLYGON ((1 1, 5 1, 5 5, 1 5, 1 1), (2 2, 2 4, 4 4, 4 2, 2 2)))");
  ExpectCoverageUnionTopologyError(nested_hole_cancellation.get(),
                                   kOverlappingMsg);

  // Relative area difference ~2e-6 exceeds AREA_PCT_DIFF_TOL (1e-6).
  Geometry::Ptr just_above_tol = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1000 0, 1000 1000, 0 1000, 0 0)), "
      "POLYGON ((2000 0, 2001 0, 2001 1, 2000 1, 2000 0)), "
      "POLYGON ((2000 0, 2001 0, 2001 1, 2000 1, 2000 0)))");
  ExpectCoverageUnionTopologyError(just_above_tol.get(), kOverlappingMsg);
}

TEST(CoverageUnionTest, AllowsAreaDifferenceWithinTolerance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Two identical 0.5x0.5 squares (total cancelled area 0.5) alongside a
  // 1000x1000 square give relative area difference ~5e-7 <= 1e-6.
  Geometry::Ptr within_tol = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1000 0, 1000 1000, 0 1000, 0 0)), "
      "POLYGON ((2000 0, 2000.5 0, 2000.5 0.5, 2000 0.5, 2000 0)), "
      "POLYGON ((2000 0, 2000.5 0, 2000.5 0.5, 2000 0.5, 2000 0)))");
  Geometry::Ptr expected =
      reader.read("POLYGON ((0 0, 1000 0, 1000 1000, 0 1000, 0 0))");

  Geometry::Ptr result = CoverageUnion::geomunion(within_tol.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_NEAR(result->getArea(), 1000000.0, 1e-9);
  EXPECT_TRUE(result->equals(expected.get()));
}

TEST(CoverageUnionTest, ThrowsOnMixedDimensionGeometryCollection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const std::string_view mixed_wkt : {
           "GEOMETRYCOLLECTION ("
           "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
           "LINESTRING (1 0, 2 0))",
           "GEOMETRYCOLLECTION ("
           "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
           "POINT (2 2))",
           "GEOMETRYCOLLECTION ("
           "LINESTRING (0 0, 1 0), "
           "POINT (2 2))",
       }) {
    SCOPED_TRACE(mixed_wkt);
    Geometry::Ptr mixed = reader.read(std::string(mixed_wkt));
    EXPECT_THROW(static_cast<void>(CoverageUnion::geomunion(mixed.get())),
                 IllegalArgumentException);
  }
}

}  // namespace
}  // namespace geos_google
