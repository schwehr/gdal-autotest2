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

// Unit tests for third_party/geos/src/operation/relateng/DimensionLocation.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/relateng/DimensionLocation.cpp

#include "third_party/geos/include/geos/operation/relateng/DimensionLocation.h"

#include <array>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Location.h"

namespace geos_google {
namespace {

using geos::geom::Dimension;
using geos::geom::Location;
using geos::operation::relateng::DimensionLocation;

static_assert(DimensionLocation::EXTERIOR == 2);
static_assert(DimensionLocation::POINT_INTERIOR == 103);
static_assert(DimensionLocation::LINE_INTERIOR == 110);
static_assert(DimensionLocation::LINE_BOUNDARY == 111);
static_assert(DimensionLocation::AREA_INTERIOR == 120);
static_assert(DimensionLocation::AREA_BOUNDARY == 121);

struct LocationMappingCase {
  Location input;
  int expected;
};

struct DimLocToLocationCase {
  int dim_loc;
  Location expected;
};

struct DimLocToDimensionCase {
  int dim_loc;
  int expected;
};

struct DimLocWithExteriorCase {
  int dim_loc;
  int exterior_dim;
  int expected;
};

TEST(DimensionLocationTest, LocationArea) {
  constexpr auto kCases = std::to_array<LocationMappingCase>({
      {.input = Location::INTERIOR,
       .expected = DimensionLocation::AREA_INTERIOR},
      {.input = Location::BOUNDARY,
       .expected = DimensionLocation::AREA_BOUNDARY},
      {.input = Location::EXTERIOR, .expected = DimensionLocation::EXTERIOR},
      {.input = Location::NONE, .expected = DimensionLocation::EXTERIOR},
  });
  for (const auto& tc : kCases) {
    EXPECT_EQ(DimensionLocation::locationArea(tc.input), tc.expected);
  }
}

TEST(DimensionLocationTest, LocationLine) {
  constexpr auto kCases = std::to_array<LocationMappingCase>({
      {.input = Location::INTERIOR,
       .expected = DimensionLocation::LINE_INTERIOR},
      {.input = Location::BOUNDARY,
       .expected = DimensionLocation::LINE_BOUNDARY},
      {.input = Location::EXTERIOR, .expected = DimensionLocation::EXTERIOR},
      {.input = Location::NONE, .expected = DimensionLocation::EXTERIOR},
  });
  for (const auto& tc : kCases) {
    EXPECT_EQ(DimensionLocation::locationLine(tc.input), tc.expected);
  }
}

TEST(DimensionLocationTest, LocationPoint) {
  constexpr auto kCases = std::to_array<LocationMappingCase>({
      {.input = Location::INTERIOR,
       .expected = DimensionLocation::POINT_INTERIOR},
      {.input = Location::BOUNDARY, .expected = DimensionLocation::EXTERIOR},
      {.input = Location::EXTERIOR, .expected = DimensionLocation::EXTERIOR},
      {.input = Location::NONE, .expected = DimensionLocation::EXTERIOR},
  });
  for (const auto& tc : kCases) {
    EXPECT_EQ(DimensionLocation::locationPoint(tc.input), tc.expected);
  }
}

TEST(DimensionLocationTest, LocationFromDimLoc) {
  constexpr auto kCases = std::to_array<DimLocToLocationCase>({
      {.dim_loc = DimensionLocation::POINT_INTERIOR,
       .expected = Location::INTERIOR},
      {.dim_loc = DimensionLocation::LINE_INTERIOR,
       .expected = Location::INTERIOR},
      {.dim_loc = DimensionLocation::AREA_INTERIOR,
       .expected = Location::INTERIOR},
      {.dim_loc = DimensionLocation::LINE_BOUNDARY,
       .expected = Location::BOUNDARY},
      {.dim_loc = DimensionLocation::AREA_BOUNDARY,
       .expected = Location::BOUNDARY},
      {.dim_loc = DimensionLocation::EXTERIOR, .expected = Location::EXTERIOR},
      {.dim_loc = -1, .expected = Location::EXTERIOR},
      {.dim_loc = 0, .expected = Location::EXTERIOR},
      {.dim_loc = 999, .expected = Location::EXTERIOR},
  });
  for (const auto& tc : kCases) {
    EXPECT_EQ(DimensionLocation::location(tc.dim_loc), tc.expected);
  }
}

TEST(DimensionLocationTest, DimensionFromDimLoc) {
  constexpr auto kCases = std::to_array<DimLocToDimensionCase>({
      {.dim_loc = DimensionLocation::POINT_INTERIOR, .expected = Dimension::P},
      {.dim_loc = DimensionLocation::LINE_INTERIOR, .expected = Dimension::L},
      {.dim_loc = DimensionLocation::LINE_BOUNDARY, .expected = Dimension::L},
      {.dim_loc = DimensionLocation::AREA_INTERIOR, .expected = Dimension::A},
      {.dim_loc = DimensionLocation::AREA_BOUNDARY, .expected = Dimension::A},
      {.dim_loc = DimensionLocation::EXTERIOR, .expected = Dimension::False},
      {.dim_loc = -1, .expected = Dimension::False},
      {.dim_loc = 0, .expected = Dimension::False},
      {.dim_loc = 999, .expected = Dimension::False},
  });
  for (const auto& tc : kCases) {
    EXPECT_EQ(DimensionLocation::dimension(tc.dim_loc), tc.expected);
  }
}

TEST(DimensionLocationTest, DimensionWithExteriorDim) {
  constexpr auto kCases = std::to_array<DimLocWithExteriorCase>({
      {.dim_loc = DimensionLocation::EXTERIOR,
       .exterior_dim = Dimension::False,
       .expected = Dimension::False},
      {.dim_loc = DimensionLocation::EXTERIOR,
       .exterior_dim = Dimension::P,
       .expected = Dimension::P},
      {.dim_loc = DimensionLocation::EXTERIOR,
       .exterior_dim = Dimension::L,
       .expected = Dimension::L},
      {.dim_loc = DimensionLocation::EXTERIOR,
       .exterior_dim = Dimension::A,
       .expected = Dimension::A},
      {.dim_loc = DimensionLocation::EXTERIOR,
       .exterior_dim = Dimension::True,
       .expected = Dimension::True},
      {.dim_loc = DimensionLocation::EXTERIOR,
       .exterior_dim = Dimension::DONTCARE,
       .expected = Dimension::DONTCARE},
      {.dim_loc = DimensionLocation::POINT_INTERIOR,
       .exterior_dim = Dimension::A,
       .expected = Dimension::P},
      {.dim_loc = DimensionLocation::LINE_INTERIOR,
       .exterior_dim = Dimension::A,
       .expected = Dimension::L},
      {.dim_loc = DimensionLocation::LINE_BOUNDARY,
       .exterior_dim = Dimension::P,
       .expected = Dimension::L},
      {.dim_loc = DimensionLocation::AREA_INTERIOR,
       .exterior_dim = Dimension::P,
       .expected = Dimension::A},
      {.dim_loc = DimensionLocation::AREA_BOUNDARY,
       .exterior_dim = Dimension::P,
       .expected = Dimension::A},
      {.dim_loc = 999,
       .exterior_dim = Dimension::A,
       .expected = Dimension::False},
  });
  for (const auto& tc : kCases) {
    EXPECT_EQ(DimensionLocation::dimension(tc.dim_loc, tc.exterior_dim),
              tc.expected);
  }
}

}  // namespace
}  // namespace geos_google
