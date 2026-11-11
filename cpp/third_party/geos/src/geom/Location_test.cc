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

// Unit tests for third_party/geos/src/geom/Location.cpp and
// third_party/geos/include/geos/geom/Location.h.

#include "third_party/geos/include/geos/geom/Location.h"

#include <array>
#include <sstream>
#include <string_view>
#include <type_traits>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::geom::Location;

static_assert(std::is_enum_v<Location>);
static_assert(!std::is_convertible_v<Location, int>);
static_assert(std::is_same_v<std::underlying_type_t<Location>, char>);

TEST(LocationTest, EnumUnderlyingValuesMatchDe9imIndices) {
  EXPECT_EQ(static_cast<char>(Location::NONE), static_cast<char>(-1));
  EXPECT_EQ(static_cast<char>(Location::INTERIOR), static_cast<char>(0));
  EXPECT_EQ(static_cast<char>(Location::BOUNDARY), static_cast<char>(1));
  EXPECT_EQ(static_cast<char>(Location::EXTERIOR), static_cast<char>(2));

  EXPECT_NE(Location::NONE, Location::INTERIOR);
  EXPECT_NE(Location::INTERIOR, Location::BOUNDARY);
  EXPECT_NE(Location::BOUNDARY, Location::EXTERIOR);
  EXPECT_NE(Location::EXTERIOR, Location::NONE);
}

TEST(LocationTest, StreamOperatorFormatsAllKnownValues) {
  struct LocationCase {
    Location loc;
    std::string_view expected;
  };
  constexpr auto kCases = std::to_array<LocationCase>({
      {.loc = Location::EXTERIOR, .expected = "e"},
      {.loc = Location::BOUNDARY, .expected = "b"},
      {.loc = Location::INTERIOR, .expected = "i"},
      {.loc = Location::NONE, .expected = "-"},
  });

  for (const auto& tc : kCases) {
    std::ostringstream oss;
    std::ostream& returned = (oss << tc.loc);
    EXPECT_EQ(&returned, &oss);
    EXPECT_EQ(oss.str(), tc.expected);
  }
}

TEST(LocationTest, StreamOperatorSupportsChaining) {
  std::ostringstream oss;
  oss << Location::INTERIOR << Location::BOUNDARY << Location::EXTERIOR
      << Location::NONE << '|' << Location::EXTERIOR;
  EXPECT_EQ(oss.str(), "ibe-|e");
}

TEST(LocationTest, StreamOperatorIgnoresUnknownEnumValue) {
  const auto unknown = static_cast<Location>(99);
  std::ostringstream oss;
  oss << "prefix:" << unknown << ":suffix";
  EXPECT_TRUE(oss.good());
  EXPECT_EQ(oss.str(), "prefix::suffix");
}

}  // namespace
}  // namespace geos_google
