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
// third_party/geos/src/operation/overlay/validate/OffsetPointGenerator.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlay/validate/OffsetPointGenerator.cpp

#include "third_party/geos/include/geos/operation/overlay/validate/OffsetPointGenerator.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::operation::overlay::validate::OffsetPointGenerator;

static_assert(!std::is_copy_constructible_v<OffsetPointGenerator>);
static_assert(!std::is_copy_assignable_v<OffsetPointGenerator>);

struct ExpectedXY {
  double x;
  double y;
};

class OffsetPointGeneratorTest : public ::testing::Test {
 protected:
  OffsetPointGeneratorTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  Geometry::Ptr ReadWkt(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(OffsetPointGeneratorTest, SingleHorizontalSegmentOffsets) {
  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 10 0)");
  OffsetPointGenerator gen(*line, 2.0);

  std::unique_ptr<std::vector<Coordinate>> pts = gen.getPoints();
  ASSERT_NE(pts, nullptr);
  ASSERT_EQ(pts->size(), 2u);
  EXPECT_DOUBLE_EQ((*pts)[0].x, 5.0);
  EXPECT_DOUBLE_EQ((*pts)[0].y, 2.0);
  EXPECT_DOUBLE_EQ((*pts)[1].x, 5.0);
  EXPECT_DOUBLE_EQ((*pts)[1].y, -2.0);
}

TEST_F(OffsetPointGeneratorTest, SingleVerticalSegmentOffsets) {
  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 0 10)");
  OffsetPointGenerator gen(*line, 1.5);

  std::unique_ptr<std::vector<Coordinate>> pts = gen.getPoints();
  ASSERT_NE(pts, nullptr);
  ASSERT_EQ(pts->size(), 2u);
  EXPECT_DOUBLE_EQ((*pts)[0].x, -1.5);
  EXPECT_DOUBLE_EQ((*pts)[0].y, 5.0);
  EXPECT_DOUBLE_EQ((*pts)[1].x, 1.5);
  EXPECT_DOUBLE_EQ((*pts)[1].y, 5.0);
}

TEST_F(OffsetPointGeneratorTest, DiagonalSegmentUsesUnitNormalAtMidpoint) {
  // Segment (1, 2) -> (4, 6) has dx=3, dy=4, length=5, midpoint=(2.5, 4.0).
  Geometry::Ptr line = ReadWkt("LINESTRING (1 2, 4 6)");
  constexpr double kOffset = 2.5;
  OffsetPointGenerator gen(*line, kOffset);

  std::unique_ptr<std::vector<Coordinate>> pts = gen.getPoints();
  ASSERT_NE(pts, nullptr);
  ASSERT_EQ(pts->size(), 2u);

  // Left: (2.5 - 2.0, 4.0 + 1.5) = (0.5, 5.5)
  // Right: (2.5 + 2.0, 4.0 - 1.5) = (4.5, 2.5)
  EXPECT_DOUBLE_EQ((*pts)[0].x, 0.5);
  EXPECT_DOUBLE_EQ((*pts)[0].y, 5.5);
  EXPECT_DOUBLE_EQ((*pts)[1].x, 4.5);
  EXPECT_DOUBLE_EQ((*pts)[1].y, 2.5);

  const Coordinate midpoint(2.5, 4.0);
  EXPECT_DOUBLE_EQ(midpoint.distance((*pts)[0]), kOffset);
  EXPECT_DOUBLE_EQ(midpoint.distance((*pts)[1]), kOffset);
}

TEST_F(OffsetPointGeneratorTest,
       MultiSegmentLineStringProducesTwoPointsPerSegment) {
  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 4 0, 4 6, 0 6)");
  OffsetPointGenerator gen(*line, 0.5);

  std::unique_ptr<std::vector<Coordinate>> pts = gen.getPoints();
  ASSERT_NE(pts, nullptr);

  constexpr auto kExpected = std::to_array<ExpectedXY>({
      {.x = 2.0, .y = 0.5},
      {.x = 2.0, .y = -0.5},
      {.x = 3.5, .y = 3.0},
      {.x = 4.5, .y = 3.0},
      {.x = 2.0, .y = 5.5},
      {.x = 2.0, .y = 6.5},
  });
  ASSERT_EQ(pts->size(), kExpected.size());
  for (std::size_t i = 0; i < kExpected.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_DOUBLE_EQ((*pts)[i].x, kExpected[i].x);
    EXPECT_DOUBLE_EQ((*pts)[i].y, kExpected[i].y);
  }
}

TEST_F(OffsetPointGeneratorTest, ZeroAndNegativeOffsets) {
  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 10 0)");

  OffsetPointGenerator zero_gen(*line, 0.0);
  std::unique_ptr<std::vector<Coordinate>> zero_pts = zero_gen.getPoints();
  ASSERT_NE(zero_pts, nullptr);
  ASSERT_EQ(zero_pts->size(), 2u);
  EXPECT_DOUBLE_EQ((*zero_pts)[0].x, 5.0);
  EXPECT_DOUBLE_EQ((*zero_pts)[0].y, 0.0);
  EXPECT_DOUBLE_EQ((*zero_pts)[1].x, 5.0);
  EXPECT_DOUBLE_EQ((*zero_pts)[1].y, 0.0);

  OffsetPointGenerator neg_gen(*line, -3.0);
  std::unique_ptr<std::vector<Coordinate>> neg_pts = neg_gen.getPoints();
  ASSERT_NE(neg_pts, nullptr);
  ASSERT_EQ(neg_pts->size(), 2u);
  EXPECT_DOUBLE_EQ((*neg_pts)[0].x, 5.0);
  EXPECT_DOUBLE_EQ((*neg_pts)[0].y, -3.0);
  EXPECT_DOUBLE_EQ((*neg_pts)[1].x, 5.0);
  EXPECT_DOUBLE_EQ((*neg_pts)[1].y, 3.0);
}

TEST_F(OffsetPointGeneratorTest, MultiLineStringExtractsAllComponents) {
  Geometry::Ptr mls = ReadWkt("MULTILINESTRING ((0 0, 2 0), (10 0, 10 4))");
  OffsetPointGenerator gen(*mls, 1.0);

  std::unique_ptr<std::vector<Coordinate>> pts = gen.getPoints();
  ASSERT_NE(pts, nullptr);

  constexpr auto kExpected = std::to_array<ExpectedXY>({
      {.x = 1.0, .y = 1.0},
      {.x = 1.0, .y = -1.0},
      {.x = 9.0, .y = 2.0},
      {.x = 11.0, .y = 2.0},
  });
  ASSERT_EQ(pts->size(), kExpected.size());
  for (std::size_t i = 0; i < kExpected.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_DOUBLE_EQ((*pts)[i].x, kExpected[i].x);
    EXPECT_DOUBLE_EQ((*pts)[i].y, kExpected[i].y);
  }
}

TEST_F(OffsetPointGeneratorTest, PolygonWithHoleExtractsShellAndHoleRings) {
  Geometry::Ptr poly = ReadWkt(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 2 8, 8 8, 8 2, 2 2))");
  OffsetPointGenerator gen(*poly, 0.5);

  std::unique_ptr<std::vector<Coordinate>> pts = gen.getPoints();
  ASSERT_NE(pts, nullptr);

  constexpr auto kExpected = std::to_array<ExpectedXY>({
      // Exterior shell (4 segments)
      {.x = 5.0, .y = 0.5},
      {.x = 5.0, .y = -0.5},
      {.x = 9.5, .y = 5.0},
      {.x = 10.5, .y = 5.0},
      {.x = 5.0, .y = 9.5},
      {.x = 5.0, .y = 10.5},
      {.x = 0.5, .y = 5.0},
      {.x = -0.5, .y = 5.0},
      // Interior hole (4 segments)
      {.x = 1.5, .y = 5.0},
      {.x = 2.5, .y = 5.0},
      {.x = 5.0, .y = 8.5},
      {.x = 5.0, .y = 7.5},
      {.x = 8.5, .y = 5.0},
      {.x = 7.5, .y = 5.0},
      {.x = 5.0, .y = 1.5},
      {.x = 5.0, .y = 2.5},
  });
  ASSERT_EQ(pts->size(), kExpected.size());
  for (std::size_t i = 0; i < kExpected.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_DOUBLE_EQ((*pts)[i].x, kExpected[i].x);
    EXPECT_DOUBLE_EQ((*pts)[i].y, kExpected[i].y);
  }
}

TEST_F(OffsetPointGeneratorTest, MultiPolygonAndMixedGeometryCollection) {
  Geometry::Ptr mp = ReadWkt(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((10 0, 12 0, 12 2, 10 2, 10 0)))");
  OffsetPointGenerator mp_gen(*mp, 0.25);
  std::unique_ptr<std::vector<Coordinate>> mp_pts = mp_gen.getPoints();
  ASSERT_NE(mp_pts, nullptr);
  EXPECT_EQ(mp_pts->size(), 16u);

  Geometry::Ptr gc = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (100 100), LINESTRING (0 0, 4 0), "
      "POLYGON ((10 0, 14 0, 14 4, 10 4, 10 0)))");
  OffsetPointGenerator gc_gen(*gc, 0.5);
  std::unique_ptr<std::vector<Coordinate>> gc_pts = gc_gen.getPoints();
  ASSERT_NE(gc_pts, nullptr);
  EXPECT_EQ(gc_pts->size(), 10u);
}

TEST_F(OffsetPointGeneratorTest, EmptyAndPuntalGeometriesReturnNoPoints) {
  constexpr auto kEmptyOrPuntalWkts = std::to_array<std::string_view>({
      "POINT (1 2)",
      "MULTIPOINT ((0 0), (5 5))",
      "POINT EMPTY",
      "LINESTRING EMPTY",
      "POLYGON EMPTY",
      "MULTILINESTRING EMPTY",
      "MULTIPOLYGON EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
  });

  for (std::string_view wkt : kEmptyOrPuntalWkts) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr geom = ReadWkt(wkt);
    OffsetPointGenerator gen(*geom, 1.0);
    std::unique_ptr<std::vector<Coordinate>> pts = gen.getPoints();
    ASSERT_NE(pts, nullptr);
    EXPECT_TRUE(pts->empty());
  }
}

TEST_F(OffsetPointGeneratorTest, CanCallGetPointsRepeatedlyAfterOwnershipMove) {
  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 6 0)");
  OffsetPointGenerator gen(*line, 1.0);

  std::unique_ptr<std::vector<Coordinate>> first = gen.getPoints();
  std::unique_ptr<std::vector<Coordinate>> second = gen.getPoints();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(*first, *second);
}

}  // namespace
}  // namespace geos_google
