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
// third_party/geos/src/operation/overlay/validate/FuzzyPointLocator.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlay/validate/FuzzyPointLocator.cpp

#include "third_party/geos/include/geos/operation/overlay/validate/FuzzyPointLocator.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {

struct GetLineWorkTag {
  using type = std::unique_ptr<geos::geom::Geometry> (
      geos::operation::overlay::validate::FuzzyPointLocator::*)(
      const geos::geom::Geometry&);
};

template <typename Tag, typename Tag::type M>
struct FuzzyMemberAccessor {
  friend typename Tag::type GetMemberPtr(Tag) { return M; }
};

typename GetLineWorkTag::type GetMemberPtr(GetLineWorkTag);
template struct FuzzyMemberAccessor<
    GetLineWorkTag,
    &geos::operation::overlay::validate::FuzzyPointLocator::getLineWork>;

namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::io::WKTReader;
using geos::operation::overlay::validate::FuzzyPointLocator;

static_assert(!std::is_copy_constructible_v<FuzzyPointLocator>);
static_assert(!std::is_copy_assignable_v<FuzzyPointLocator>);

struct LocationCase {
  std::string_view name;
  double x;
  double y;
  Location expected;
};

class FuzzyPointLocatorTest : public ::testing::Test {
 protected:
  FuzzyPointLocatorTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  Geometry::Ptr ReadWkt(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(FuzzyPointLocatorTest,
       LocatesPointsRelativeToSimplePolygonWithTolerance) {
  Geometry::Ptr poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_NE(poly, nullptr);

  // Use dyadic tolerance 0.25 so exact distance equality (dist == tolerance)
  // is free of binary floating-point representation drift.
  FuzzyPointLocator locator(*poly, 0.25);

  constexpr auto kCases = std::to_array<LocationCase>({
      {.name = "center_interior",
       .x = 5.0,
       .y = 5.0,
       .expected = Location::INTERIOR},
      {.name = "far_exterior",
       .x = 15.0,
       .y = 5.0,
       .expected = Location::EXTERIOR},
      {.name = "on_vertex", .x = 0.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.name = "on_edge", .x = 5.0, .y = 0.0, .expected = Location::BOUNDARY},
      {.name = "just_inside_within_tol",
       .x = 0.125,
       .y = 5.0,
       .expected = Location::BOUNDARY},
      {.name = "just_outside_within_tol",
       .x = -0.125,
       .y = 5.0,
       .expected = Location::BOUNDARY},
      {.name = "near_corner_within_tol",
       .x = -0.125,
       .y = -0.125,
       .expected = Location::BOUNDARY},
      {.name = "near_corner_beyond_tol",
       .x = -0.25,
       .y = -0.25,
       .expected = Location::EXTERIOR},
      {.name = "exactly_at_tol_inside",
       .x = 0.25,
       .y = 5.0,
       .expected = Location::INTERIOR},
      {.name = "exactly_at_tol_outside",
       .x = -0.25,
       .y = 5.0,
       .expected = Location::EXTERIOR},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(locator.getLocation(Coordinate(c.x, c.y)), c.expected);
  }
}

TEST_F(FuzzyPointLocatorTest, LocatesPointsRelativeToPolygonWithHole) {
  Geometry::Ptr poly = ReadWkt(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(3 3, 3 7, 7 7, 7 3, 3 3))");
  ASSERT_NE(poly, nullptr);

  FuzzyPointLocator locator(*poly, 0.25);

  constexpr auto kCases = std::to_array<LocationCase>({
      {.name = "in_annulus_interior",
       .x = 1.5,
       .y = 5.0,
       .expected = Location::INTERIOR},
      {.name = "deep_in_hole_exterior",
       .x = 5.0,
       .y = 5.0,
       .expected = Location::EXTERIOR},
      {.name = "on_hole_boundary",
       .x = 3.0,
       .y = 5.0,
       .expected = Location::BOUNDARY},
      {.name = "inside_shell_near_hole",
       .x = 2.875,
       .y = 5.0,
       .expected = Location::BOUNDARY},
      {.name = "inside_hole_near_hole_edge",
       .x = 3.125,
       .y = 5.0,
       .expected = Location::BOUNDARY},
      {.name = "inside_hole_at_exact_tol",
       .x = 3.25,
       .y = 5.0,
       .expected = Location::EXTERIOR},
      {.name = "inside_shell_at_exact_tol",
       .x = 2.75,
       .y = 5.0,
       .expected = Location::INTERIOR},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(locator.getLocation(Coordinate(c.x, c.y)), c.expected);
  }
}

TEST_F(FuzzyPointLocatorTest, ZeroAndNegativeToleranceDelegateToExactLocator) {
  Geometry::Ptr poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_NE(poly, nullptr);

  for (double tol : {0.0, -0.5}) {
    SCOPED_TRACE(tol);
    FuzzyPointLocator locator(*poly, tol);
    EXPECT_EQ(locator.getLocation(Coordinate(0.0, 5.0)), Location::BOUNDARY);
    EXPECT_EQ(locator.getLocation(Coordinate(1e-6, 5.0)), Location::INTERIOR);
    EXPECT_EQ(locator.getLocation(Coordinate(-1e-6, 5.0)), Location::EXTERIOR);
  }
}

TEST_F(FuzzyPointLocatorTest, MultiPolygonWithMixedBoundaryTypes) {
  // First polygon has no holes (boundary is LineString); second has a hole
  // (boundary is MultiLineString), producing a GeometryCollection linework.
  Geometry::Ptr mp = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((10 0, 20 0, 20 10, 10 10, 10 0), (13 3, 13 7, 17 7, 17 3, 13 3)))");
  ASSERT_NE(mp, nullptr);

  FuzzyPointLocator locator(*mp, 0.25);

  constexpr auto kCases = std::to_array<LocationCase>({
      {.name = "first_poly_interior",
       .x = 2.0,
       .y = 2.0,
       .expected = Location::INTERIOR},
      {.name = "first_poly_near_edge",
       .x = 4.125,
       .y = 2.0,
       .expected = Location::BOUNDARY},
      {.name = "gap_exterior",
       .x = 7.0,
       .y = 5.0,
       .expected = Location::EXTERIOR},
      {.name = "second_poly_interior",
       .x = 11.5,
       .y = 5.0,
       .expected = Location::INTERIOR},
      {.name = "second_poly_hole_exterior",
       .x = 15.0,
       .y = 5.0,
       .expected = Location::EXTERIOR},
      {.name = "second_poly_near_hole",
       .x = 13.125,
       .y = 5.0,
       .expected = Location::BOUNDARY},
  });

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(locator.getLocation(Coordinate(c.x, c.y)), c.expected);
  }
}

TEST_F(FuzzyPointLocatorTest,
       GeometryCollectionIgnoresNonArealComponentsInLinework) {
  Geometry::Ptr gc = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POINT (40 0), "
      "LINESTRING (20 0, 30 0), "
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))");
  ASSERT_NE(gc, nullptr);

  FuzzyPointLocator locator(*gc, 0.25);

  // Near the polygonal component's boundary -> BOUNDARY via fuzzy linework.
  EXPECT_EQ(locator.getLocation(Coordinate(0.125, 5.0)), Location::BOUNDARY);
  EXPECT_EQ(locator.getLocation(Coordinate(5.0, 5.0)), Location::INTERIOR);

  // Near the LineString (but not on it): linework ignores Dimension::L, so
  // dist >= 0.25 and PointLocator reports EXTERIOR unless directly on it.
  EXPECT_EQ(locator.getLocation(Coordinate(25.0, 0.125)), Location::EXTERIOR);
  EXPECT_EQ(locator.getLocation(Coordinate(25.0, 0.0)), Location::INTERIOR);
  EXPECT_EQ(locator.getLocation(Coordinate(20.0, 0.0)), Location::BOUNDARY);

  // Near the Point vs exact Point match.
  EXPECT_EQ(locator.getLocation(Coordinate(40.0, 0.125)), Location::EXTERIOR);
  EXPECT_EQ(locator.getLocation(Coordinate(40.0, 0.0)), Location::INTERIOR);
}

TEST_F(FuzzyPointLocatorTest, EmptyAndNonPolygonalGeometries) {
  // When extracted linework is empty, DistanceOp::distance returns 0.0:
  // positive tolerance classifies every query point as BOUNDARY, while
  // zero tolerance delegates directly to PointLocator.
  for (std::string_view wkt :
       {"POLYGON EMPTY", "MULTIPOLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY",
        "POINT (1 2)", "LINESTRING (0 0, 10 0)"}) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr geom = ReadWkt(wkt);
    ASSERT_NE(geom, nullptr);
    FuzzyPointLocator positive_tol_locator(*geom, 0.25);
    EXPECT_EQ(positive_tol_locator.getLocation(Coordinate(100.0, 100.0)),
              Location::BOUNDARY);
  }

  Geometry::Ptr empty_poly = ReadWkt("POLYGON EMPTY");
  FuzzyPointLocator zero_empty(*empty_poly, 0.0);
  EXPECT_EQ(zero_empty.getLocation(Coordinate(0.0, 0.0)), Location::EXTERIOR);

  Geometry::Ptr point = ReadWkt("POINT (1 2)");
  FuzzyPointLocator zero_point(*point, 0.0);
  EXPECT_EQ(zero_point.getLocation(Coordinate(1.0, 2.0)), Location::INTERIOR);
  EXPECT_EQ(zero_point.getLocation(Coordinate(2.0, 2.0)), Location::EXTERIOR);

  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 10 0)");
  FuzzyPointLocator zero_line(*line, 0.0);
  EXPECT_EQ(zero_line.getLocation(Coordinate(0.0, 0.0)), Location::BOUNDARY);
  EXPECT_EQ(zero_line.getLocation(Coordinate(5.0, 0.0)), Location::INTERIOR);
  EXPECT_EQ(zero_line.getLocation(Coordinate(5.0, 1.0)), Location::EXTERIOR);
}

TEST_F(FuzzyPointLocatorTest,
       GetLineWorkExtractsPolygonBoundariesAndClonesNonPolygonalComponents) {
  Geometry::Ptr poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  FuzzyPointLocator poly_locator(*poly, 0.25);
  auto poly_fn = GetMemberPtr(GetLineWorkTag{});
  std::unique_ptr<Geometry> poly_linework = (poly_locator.*poly_fn)(*poly);
  ASSERT_NE(poly_linework, nullptr);
  EXPECT_EQ(poly_linework->getDimension(), geos::geom::Dimension::L);

  Geometry::Ptr mixed = ReadWkt(
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "LINESTRING (10 0, 12 0), POINT (20 20))");
  FuzzyPointLocator mixed_locator(*mixed, 0.25);
  std::unique_ptr<Geometry> mixed_linework = (mixed_locator.*poly_fn)(*mixed);
  ASSERT_NE(mixed_linework, nullptr);
  EXPECT_EQ(mixed_linework->getNumGeometries(), 3u);
}

}  // namespace
}  // namespace geos_google
