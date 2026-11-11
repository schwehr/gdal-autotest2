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

// Unit tests for third_party/geos/src/geom/Dimension.cpp and
// third_party/geos/include/geos/geom/Dimension.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/Dimension.cpp

#include "third_party/geos/include/geos/geom/Dimension.h"

#include <array>
#include <limits>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Dimension;
using geos::util::IllegalArgumentException;
using ::testing::HasSubstr;

static_assert(Dimension::DONTCARE == -3);
static_assert(Dimension::True == -2);
static_assert(Dimension::False == -1);
static_assert(Dimension::P == 0);
static_assert(Dimension::L == 1);
static_assert(Dimension::A == 2);

struct DimensionValueCase {
  std::string_view name;
  int value;
  char expected_symbol;
};

constexpr auto kValidDimensionValues = std::to_array<DimensionValueCase>({
    {.name = "dontcare", .value = Dimension::DONTCARE, .expected_symbol = '*'},
    {.name = "true", .value = Dimension::True, .expected_symbol = 'T'},
    {.name = "false", .value = Dimension::False, .expected_symbol = 'F'},
    {.name = "point", .value = Dimension::P, .expected_symbol = '0'},
    {.name = "line", .value = Dimension::L, .expected_symbol = '1'},
    {.name = "area", .value = Dimension::A, .expected_symbol = '2'},
});

struct DimensionSymbolCase {
  std::string_view name;
  char symbol;
  Dimension::DimensionType expected_value;
  char canonical_symbol;
};

constexpr auto kValidDimensionSymbols = std::to_array<DimensionSymbolCase>({
    {.name = "dontcare",
     .symbol = '*',
     .expected_value = Dimension::DONTCARE,
     .canonical_symbol = '*'},
    {.name = "true_upper",
     .symbol = 'T',
     .expected_value = Dimension::True,
     .canonical_symbol = 'T'},
    {.name = "true_lower",
     .symbol = 't',
     .expected_value = Dimension::True,
     .canonical_symbol = 'T'},
    {.name = "false_upper",
     .symbol = 'F',
     .expected_value = Dimension::False,
     .canonical_symbol = 'F'},
    {.name = "false_lower",
     .symbol = 'f',
     .expected_value = Dimension::False,
     .canonical_symbol = 'F'},
    {.name = "point",
     .symbol = '0',
     .expected_value = Dimension::P,
     .canonical_symbol = '0'},
    {.name = "line",
     .symbol = '1',
     .expected_value = Dimension::L,
     .canonical_symbol = '1'},
    {.name = "area",
     .symbol = '2',
     .expected_value = Dimension::A,
     .canonical_symbol = '2'},
});

TEST(DimensionTest, ToDimensionSymbolValidValues) {
  for (const auto& test_case : kValidDimensionValues) {
    SCOPED_TRACE(test_case.name);
    EXPECT_EQ(Dimension::toDimensionSymbol(test_case.value),
              test_case.expected_symbol);
  }
}

TEST(DimensionTest, ToDimensionSymbolInvalidValuesThrow) {
  constexpr auto kInvalidValues = std::to_array<int>({
      -4,
      -100,
      3,
      42,
      static_cast<int>('0'),
      std::numeric_limits<int>::min(),
      std::numeric_limits<int>::max(),
  });

  for (int value : kInvalidValues) {
    SCOPED_TRACE(value);
    EXPECT_THROW(
        {
          try {
            Dimension::toDimensionSymbol(value);
          } catch (const IllegalArgumentException& e) {
            EXPECT_THAT(e.what(), HasSubstr("Unknown dimension value:"));
            throw;
          }
        },
        IllegalArgumentException);
  }
}

TEST(DimensionTest, ToDimensionValueValidSymbols) {
  for (const auto& test_case : kValidDimensionSymbols) {
    SCOPED_TRACE(test_case.name);
    EXPECT_EQ(Dimension::toDimensionValue(test_case.symbol),
              test_case.expected_value);
  }
}

TEST(DimensionTest, ToDimensionValueInvalidSymbolsThrow) {
  constexpr auto kInvalidSymbols = std::to_array<char>({
      '3',
      '9',
      '-',
      '+',
      ' ',
      '\0',
      '\n',
      'a',
      'x',
      static_cast<char>(127),
      static_cast<char>(-1),
  });

  for (char symbol : kInvalidSymbols) {
    SCOPED_TRACE(static_cast<int>(symbol));
    EXPECT_THROW(
        {
          try {
            Dimension::toDimensionValue(symbol);
          } catch (const IllegalArgumentException& e) {
            EXPECT_THAT(e.what(), HasSubstr("Unknown dimension symbol:"));
            throw;
          }
        },
        IllegalArgumentException);
  }
}

TEST(DimensionTest, RoundTripConversions) {
  for (const auto& test_case : kValidDimensionValues) {
    SCOPED_TRACE(test_case.name);
    const char symbol = Dimension::toDimensionSymbol(test_case.value);
    EXPECT_EQ(Dimension::toDimensionValue(symbol), test_case.value);
  }

  for (const auto& test_case : kValidDimensionSymbols) {
    SCOPED_TRACE(test_case.name);
    const int value = Dimension::toDimensionValue(test_case.symbol);
    EXPECT_EQ(Dimension::toDimensionSymbol(value), test_case.canonical_symbol);
  }
}

}  // namespace
}  // namespace geos_google
