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

// Unit tests for third_party/geos/src/geom/IntersectionMatrix.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/IntersectionMatrix.cpp

#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"

#include <array>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Dimension;
using geos::geom::IntersectionMatrix;
using geos::geom::Location;
using geos::util::IllegalArgumentException;

static_assert(Dimension::DONTCARE < Dimension::True);
static_assert(Dimension::True < Dimension::False);
static_assert(Dimension::False < Dimension::P);
static_assert(Dimension::P < Dimension::L);
static_assert(Dimension::L < Dimension::A);
static_assert(static_cast<std::size_t>(Location::INTERIOR) == 0);
static_assert(static_cast<std::size_t>(Location::BOUNDARY) == 1);
static_assert(static_cast<std::size_t>(Location::EXTERIOR) == 2);

constexpr auto kLocations = std::to_array<Location>({
    Location::INTERIOR,
    Location::BOUNDARY,
    Location::EXTERIOR,
});

struct SingleMatchCase {
  int actual_dimension;
  char required_symbol;
  bool expected;
};

constexpr auto kSingleMatchCases = std::to_array<SingleMatchCase>({
    // '*' matches every dimension value.
    {.actual_dimension = Dimension::DONTCARE,
     .required_symbol = '*',
     .expected = true},
    {.actual_dimension = Dimension::True,
     .required_symbol = '*',
     .expected = true},
    {.actual_dimension = Dimension::False,
     .required_symbol = '*',
     .expected = true},
    {.actual_dimension = Dimension::P,
     .required_symbol = '*',
     .expected = true},
    {.actual_dimension = Dimension::L,
     .required_symbol = '*',
     .expected = true},
    {.actual_dimension = Dimension::A,
     .required_symbol = '*',
     .expected = true},
    {.actual_dimension = 3, .required_symbol = '*', .expected = true},
    {.actual_dimension = -4, .required_symbol = '*', .expected = true},
    // 'T' matches True and {0, 1, 2}, not False or DONTCARE.
    {.actual_dimension = Dimension::True,
     .required_symbol = 'T',
     .expected = true},
    {.actual_dimension = Dimension::P,
     .required_symbol = 'T',
     .expected = true},
    {.actual_dimension = Dimension::L,
     .required_symbol = 'T',
     .expected = true},
    {.actual_dimension = Dimension::A,
     .required_symbol = 'T',
     .expected = true},
    {.actual_dimension = Dimension::False,
     .required_symbol = 'T',
     .expected = false},
    {.actual_dimension = Dimension::DONTCARE,
     .required_symbol = 'T',
     .expected = false},
    // 'F' matches only False.
    {.actual_dimension = Dimension::False,
     .required_symbol = 'F',
     .expected = true},
    {.actual_dimension = Dimension::True,
     .required_symbol = 'F',
     .expected = false},
    {.actual_dimension = Dimension::DONTCARE,
     .required_symbol = 'F',
     .expected = false},
    {.actual_dimension = Dimension::P,
     .required_symbol = 'F',
     .expected = false},
    // '0', '1', '2' match only P, L, A respectively.
    {.actual_dimension = Dimension::P,
     .required_symbol = '0',
     .expected = true},
    {.actual_dimension = Dimension::L,
     .required_symbol = '0',
     .expected = false},
    {.actual_dimension = Dimension::False,
     .required_symbol = '0',
     .expected = false},
    {.actual_dimension = Dimension::True,
     .required_symbol = '0',
     .expected = false},
    {.actual_dimension = Dimension::L,
     .required_symbol = '1',
     .expected = true},
    {.actual_dimension = Dimension::P,
     .required_symbol = '1',
     .expected = false},
    {.actual_dimension = Dimension::A,
     .required_symbol = '1',
     .expected = false},
    {.actual_dimension = Dimension::True,
     .required_symbol = '1',
     .expected = false},
    {.actual_dimension = Dimension::A,
     .required_symbol = '2',
     .expected = true},
    {.actual_dimension = Dimension::L,
     .required_symbol = '2',
     .expected = false},
    {.actual_dimension = Dimension::False,
     .required_symbol = '2',
     .expected = false},
    {.actual_dimension = Dimension::True,
     .required_symbol = '2',
     .expected = false},
    // Lowercase or unrecognized symbols never match.
    {.actual_dimension = Dimension::True,
     .required_symbol = 't',
     .expected = false},
    {.actual_dimension = Dimension::False,
     .required_symbol = 'f',
     .expected = false},
    {.actual_dimension = Dimension::P,
     .required_symbol = 'X',
     .expected = false},
    {.actual_dimension = 3, .required_symbol = '3', .expected = false},
});

struct PatternMatchCase {
  std::string_view actual;
  std::string_view pattern;
  bool expected;
};

constexpr auto kPatternMatchCases = std::to_array<PatternMatchCase>({
    {.actual = "212101212", .pattern = "212101212", .expected = true},
    {.actual = "012012012", .pattern = "TTTTTTTTT", .expected = true},
    {.actual = "012TF*012", .pattern = "*********", .expected = true},
    {.actual = "212FF1FF2", .pattern = "T*****FF*", .expected = true},
    {.actual = "2FF1FF212", .pattern = "T*F**F***", .expected = true},
    {.actual = "tf*012tf*", .pattern = "TF*012TF*", .expected = true},
    {.actual = "0FFFFFFF2", .pattern = "0*F**FFF2", .expected = true},
    {.actual = "", .pattern = "FFFFFFFFF", .expected = true},
    {.actual = "2", .pattern = "2FFFFFFFF", .expected = true},
    // Mismatches at index 0, middle, and index 8.
    {.actual = "F12101212", .pattern = "212101212", .expected = false},
    {.actual = "2121F1212", .pattern = "212101212", .expected = false},
    {.actual = "21210121F", .pattern = "212101212", .expected = false},
    {.actual = "212FF1FF2", .pattern = "T*T**F***", .expected = false},
    {.actual = "0FFFFFFF2", .pattern = "tffffffff", .expected = false},
    {.actual = "*12101212", .pattern = "T12101212", .expected = false},
    {.actual = "T12101212", .pattern = "212101212", .expected = false},
});

struct MatrixPredicateCase {
  std::string_view matrix;
  bool expected;
};

constexpr auto kDisjointCases = std::to_array<MatrixPredicateCase>({
    {.matrix = "FFFFFFFFF", .expected = true},
    {.matrix = "FF2FF1212", .expected = true},
    {.matrix = "FFTFFT***", .expected = true},
    {.matrix = "0FFFFFFFF", .expected = false},
    {.matrix = "TFFFFFFFF", .expected = false},
    {.matrix = "*FFFFFFFF", .expected = false},
    {.matrix = "F1FFFFFFF", .expected = false},
    {.matrix = "FTFFFFFFF", .expected = false},
    {.matrix = "F*FFFFFFF", .expected = false},
    {.matrix = "FFF2FFFFF", .expected = false},
    {.matrix = "FFFTFFFFF", .expected = false},
    {.matrix = "FFF*FFFFF", .expected = false},
    {.matrix = "FFFF0FFFF", .expected = false},
    {.matrix = "FFFFTFFFF", .expected = false},
    {.matrix = "FFFF*FFFF", .expected = false},
});

struct DimPairPredicateCase {
  std::string_view matrix;
  int dim_a;
  int dim_b;
  bool expected;
};

constexpr auto kTouchesCases = std::to_array<DimPairPredicateCase>({
    // Valid dimension pairs with II=F and at least one of IB, BI, BB matching
    // T.
    {.matrix = "FTFFFFFFF",
     .dim_a = Dimension::A,
     .dim_b = Dimension::A,
     .expected = true},
    {.matrix = "FFFTFFFFF",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = true},
    {.matrix = "FFFFTFFFF",
     .dim_a = Dimension::L,
     .dim_b = Dimension::A,
     .expected = true},
    {.matrix = "F0FFFFFFF",
     .dim_a = Dimension::A,
     .dim_b = Dimension::L,
     .expected = true},
    {.matrix = "FFF1FFFFF",
     .dim_a = Dimension::P,
     .dim_b = Dimension::A,
     .expected = true},
    {.matrix = "FFFF2FFFF",
     .dim_a = Dimension::A,
     .dim_b = Dimension::P,
     .expected = true},
    {.matrix = "F01020102",
     .dim_a = Dimension::P,
     .dim_b = Dimension::L,
     .expected = true},
    {.matrix = "FT*******",
     .dim_a = Dimension::L,
     .dim_b = Dimension::P,
     .expected = true},
    // Valid dimension pairs failing II==F or missing boundary contact.
    {.matrix = "0TFFFFFFF",
     .dim_a = Dimension::A,
     .dim_b = Dimension::A,
     .expected = false},
    {.matrix = "TTFFFFFFF",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "*TFFFFFFF",
     .dim_a = Dimension::P,
     .dim_b = Dimension::A,
     .expected = false},
    {.matrix = "FFFFFFFFF",
     .dim_a = Dimension::A,
     .dim_b = Dimension::A,
     .expected = false},
    {.matrix = "F*F**F***",
     .dim_a = Dimension::L,
     .dim_b = Dimension::A,
     .expected = false},
    // Points never touch each other; empty/invalid dimensions return false.
    {.matrix = "FTFFFFFFF",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "FFFFTFFFF",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "FTFFFFFFF",
     .dim_a = Dimension::False,
     .dim_b = Dimension::A,
     .expected = false},
    {.matrix = "FTFFFFFFF",
     .dim_a = Dimension::A,
     .dim_b = Dimension::False,
     .expected = false},
    {.matrix = "FTFFFFFFF",
     .dim_a = Dimension::False,
     .dim_b = Dimension::False,
     .expected = false},
});

constexpr auto kCrossesCases = std::to_array<DimPairPredicateCase>({
    // dimA < dimB: T*T****** (II and IE match T).
    {.matrix = "0F0FFFFFF",
     .dim_a = Dimension::P,
     .dim_b = Dimension::L,
     .expected = true},
    {.matrix = "1F2FFFFFF",
     .dim_a = Dimension::P,
     .dim_b = Dimension::A,
     .expected = true},
    {.matrix = "T*T******",
     .dim_a = Dimension::L,
     .dim_b = Dimension::A,
     .expected = true},
    {.matrix = "FFT******",
     .dim_a = Dimension::P,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "*FT******",
     .dim_a = Dimension::P,
     .dim_b = Dimension::A,
     .expected = false},
    {.matrix = "TFF******",
     .dim_a = Dimension::L,
     .dim_b = Dimension::A,
     .expected = false},
    {.matrix = "TF*******",
     .dim_a = Dimension::L,
     .dim_b = Dimension::A,
     .expected = false},
    // dimA > dimB: T*****T** (II and EI match T).
    {.matrix = "0FFFFF0FF",
     .dim_a = Dimension::L,
     .dim_b = Dimension::P,
     .expected = true},
    {.matrix = "1FFFFF2FF",
     .dim_a = Dimension::A,
     .dim_b = Dimension::P,
     .expected = true},
    {.matrix = "T*****T**",
     .dim_a = Dimension::A,
     .dim_b = Dimension::L,
     .expected = true},
    {.matrix = "F*****T**",
     .dim_a = Dimension::L,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "******T**",
     .dim_a = Dimension::A,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "T*****F**",
     .dim_a = Dimension::A,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "T********",
     .dim_a = Dimension::A,
     .dim_b = Dimension::L,
     .expected = false},
    // (L, L): II must be 0 (Dimension::P).
    {.matrix = "0FFFFFFFF",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = true},
    {.matrix = "012101212",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = true},
    {.matrix = "1FFFFFFFF",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "2FFFFFFFF",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "TFFFFFFFF",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "FFFFFFFFF",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "*FFFFFFFF",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    // (P, P), (A, A), and empty dimensions never cross.
    {.matrix = "0*T***T**",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "0*T***T**",
     .dim_a = Dimension::A,
     .dim_b = Dimension::A,
     .expected = false},
    {.matrix = "0*T***T**",
     .dim_a = Dimension::False,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "0*T***T**",
     .dim_a = Dimension::L,
     .dim_b = Dimension::False,
     .expected = false},
});

constexpr auto kWithinCases = std::to_array<MatrixPredicateCase>({
    {.matrix = "0FFFFFFF2", .expected = true},
    {.matrix = "T*F**F***", .expected = true},
    {.matrix = "2FF1FF212", .expected = true},
    {.matrix = "F*F**F***", .expected = false},
    {.matrix = "**F**F***", .expected = false},
    {.matrix = "T*T**F***", .expected = false},
    {.matrix = "T****F***", .expected = false},
    {.matrix = "T*F**T***", .expected = false},
    {.matrix = "T*F******", .expected = false},
});

constexpr auto kContainsCases = std::to_array<MatrixPredicateCase>({
    {.matrix = "0FFFFFFF2", .expected = true},
    {.matrix = "T*****FF*", .expected = true},
    {.matrix = "212FF1FF2", .expected = true},
    {.matrix = "F*****FF*", .expected = false},
    {.matrix = "******FF*", .expected = false},
    {.matrix = "T*****TF*", .expected = false},
    {.matrix = "T******F*", .expected = false},
    {.matrix = "T*****FT*", .expected = false},
    {.matrix = "T*****F**", .expected = false},
});

constexpr auto kEqualsCases = std::to_array<DimPairPredicateCase>({
    {.matrix = "0FFFFFFF2",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = true},
    {.matrix = "1FFF1FFF2",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = true},
    {.matrix = "2FFF1FFF2",
     .dim_a = Dimension::A,
     .dim_b = Dimension::A,
     .expected = true},
    {.matrix = "T*F**FFF*",
     .dim_a = Dimension::A,
     .dim_b = Dimension::A,
     .expected = true},
    {.matrix = "T*F**FFF*",
     .dim_a = Dimension::False,
     .dim_b = Dimension::False,
     .expected = true},
    // Individual cell failures.
    {.matrix = "2FF1FF212",
     .dim_a = Dimension::A,
     .dim_b = Dimension::A,
     .expected = false},
    {.matrix = "FFFFFFFF2",
     .dim_a = Dimension::False,
     .dim_b = Dimension::False,
     .expected = false},
    {.matrix = "F*F**FFF*",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "**F**FFF*",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "T*F**FTF*",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "T*F**F*F*",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "T*T**FFF*",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "T****FFF*",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "T*F**FFT*",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "T*F**FF**",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "T*F**TFF*",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "T*F***FF*",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    // Mismatched geometry dimensions always return false.
    {.matrix = "T*F**FFF*",
     .dim_a = Dimension::P,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "T*F**FFF*",
     .dim_a = Dimension::L,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "T*F**FFF*",
     .dim_a = Dimension::P,
     .dim_b = Dimension::A,
     .expected = false},
    {.matrix = "T*F**FFF*",
     .dim_a = Dimension::A,
     .dim_b = Dimension::L,
     .expected = false},
});

constexpr auto kOverlapsCases = std::to_array<DimPairPredicateCase>({
    // (P, P) and (A, A): T*T***T**.
    {.matrix = "T*T***T**",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = true},
    {.matrix = "0F0FFF0F2",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = true},
    {.matrix = "212101212",
     .dim_a = Dimension::A,
     .dim_b = Dimension::A,
     .expected = true},
    {.matrix = "F*T***T**",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "T*F***T**",
     .dim_a = Dimension::A,
     .dim_b = Dimension::A,
     .expected = false},
    {.matrix = "T*T***F**",
     .dim_a = Dimension::A,
     .dim_b = Dimension::A,
     .expected = false},
    {.matrix = "T*****T**",
     .dim_a = Dimension::P,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "T*T******",
     .dim_a = Dimension::A,
     .dim_b = Dimension::A,
     .expected = false},
    // (L, L): 1*T***T** (II must be exactly Dimension::L == 1).
    {.matrix = "1*T***T**",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = true},
    {.matrix = "1010F0102",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = true},
    {.matrix = "0*T***T**",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "2*T***T**",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "T*T***T**",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "1*F***T**",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "1*T***F**",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "1*****T**",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "1*T******",
     .dim_a = Dimension::L,
     .dim_b = Dimension::L,
     .expected = false},
    // Unequal or empty dimensions never overlap.
    {.matrix = "1*T***T**",
     .dim_a = Dimension::P,
     .dim_b = Dimension::L,
     .expected = false},
    {.matrix = "1*T***T**",
     .dim_a = Dimension::L,
     .dim_b = Dimension::A,
     .expected = false},
    {.matrix = "1*T***T**",
     .dim_a = Dimension::A,
     .dim_b = Dimension::P,
     .expected = false},
    {.matrix = "1*T***T**",
     .dim_a = Dimension::False,
     .dim_b = Dimension::False,
     .expected = false},
});

constexpr auto kCoversCases = std::to_array<MatrixPredicateCase>({
    {.matrix = "T*****FF*", .expected = true},
    {.matrix = "*T****FF*", .expected = true},
    {.matrix = "***T**FF*", .expected = true},
    {.matrix = "****T*FF*", .expected = true},
    {.matrix = "0FFFFFFF2", .expected = true},
    {.matrix = "F1FFFFFF2", .expected = true},
    {.matrix = "FFF2FFFF2", .expected = true},
    {.matrix = "FFFF0FFF2", .expected = true},
    {.matrix = "FF*FF*FF*", .expected = false},
    {.matrix = "FFFFFFFF2", .expected = false},
    {.matrix = "T*****TF*", .expected = false},
    {.matrix = "T******F*", .expected = false},
    {.matrix = "T*****FT*", .expected = false},
    {.matrix = "T*****F**", .expected = false},
});

constexpr auto kCoveredByCases = std::to_array<MatrixPredicateCase>({
    {.matrix = "T*F**F***", .expected = true},
    {.matrix = "*TF**F***", .expected = true},
    {.matrix = "**FT*F***", .expected = true},
    {.matrix = "**F*TF***", .expected = true},
    {.matrix = "0FF**F***", .expected = true},
    {.matrix = "F0F**F***", .expected = true},
    {.matrix = "FFF1*F***", .expected = true},
    {.matrix = "FFFF2F***", .expected = true},
    {.matrix = "FF*FF*222", .expected = false},
    {.matrix = "T*T**F***", .expected = false},
    {.matrix = "T****F***", .expected = false},
    {.matrix = "T*F**T***", .expected = false},
    {.matrix = "T*F******", .expected = false},
});

TEST(IntersectionMatrixTest, DefaultConstructorInitializesAllFalse) {
  IntersectionMatrix im;
  EXPECT_EQ(im.toString(), "FFFFFFFFF");
  for (Location row : kLocations) {
    for (Location col : kLocations) {
      EXPECT_EQ(im.get(row, col), Dimension::False);
    }
  }
  EXPECT_TRUE(im.isDisjoint());
  EXPECT_FALSE(im.isIntersects());
}

TEST(IntersectionMatrixTest, StringConstructorSetsSymbolsAndPadsWithFalse) {
  IntersectionMatrix full("012TF*012");
  EXPECT_EQ(full.toString(), "012TF*012");
  EXPECT_EQ(full.get(Location::INTERIOR, Location::INTERIOR), Dimension::P);
  EXPECT_EQ(full.get(Location::INTERIOR, Location::BOUNDARY), Dimension::L);
  EXPECT_EQ(full.get(Location::INTERIOR, Location::EXTERIOR), Dimension::A);
  EXPECT_EQ(full.get(Location::BOUNDARY, Location::INTERIOR), Dimension::True);
  EXPECT_EQ(full.get(Location::BOUNDARY, Location::BOUNDARY), Dimension::False);
  EXPECT_EQ(full.get(Location::BOUNDARY, Location::EXTERIOR),
            Dimension::DONTCARE);

  IntersectionMatrix lowercase("tf*012tf*");
  EXPECT_EQ(lowercase.toString(), "TF*012TF*");

  IntersectionMatrix empty("");
  EXPECT_EQ(empty.toString(), "FFFFFFFFF");

  IntersectionMatrix partial("210");
  EXPECT_EQ(partial.toString(), "210FFFFFF");

  IntersectionMatrix overlength("012TF*012XYZ");
  EXPECT_EQ(overlength.toString(), "012TF*012");

  EXPECT_THROW(IntersectionMatrix("012345678"), IllegalArgumentException);
}

TEST(IntersectionMatrixTest, CopyConstructorCreatesIndependentCopy) {
  IntersectionMatrix orig("012TF*210");
  IntersectionMatrix copy(orig);
  EXPECT_EQ(copy.toString(), "012TF*210");

  orig.set(Location::INTERIOR, Location::INTERIOR, Dimension::A);
  orig.set(Location::EXTERIOR, Location::EXTERIOR, Dimension::False);
  EXPECT_EQ(orig.toString(), "212TF*21F");
  EXPECT_EQ(copy.toString(), "012TF*210");
}

TEST(IntersectionMatrixTest, SetAndGetIndividualElements) {
  IntersectionMatrix im;
  im.set(Location::INTERIOR, Location::INTERIOR, Dimension::P);
  im.set(Location::INTERIOR, Location::BOUNDARY, Dimension::L);
  im.set(Location::INTERIOR, Location::EXTERIOR, Dimension::A);
  im.set(Location::BOUNDARY, Location::INTERIOR, Dimension::True);
  im.set(Location::BOUNDARY, Location::BOUNDARY, Dimension::False);
  im.set(Location::BOUNDARY, Location::EXTERIOR, Dimension::DONTCARE);
  im.set(Location::EXTERIOR, Location::INTERIOR, Dimension::A);
  im.set(Location::EXTERIOR, Location::BOUNDARY, Dimension::L);
  im.set(Location::EXTERIOR, Location::EXTERIOR, Dimension::P);

  EXPECT_EQ(im.get(Location::INTERIOR, Location::INTERIOR), Dimension::P);
  EXPECT_EQ(im.get(Location::INTERIOR, Location::BOUNDARY), Dimension::L);
  EXPECT_EQ(im.get(Location::INTERIOR, Location::EXTERIOR), Dimension::A);
  EXPECT_EQ(im.get(Location::BOUNDARY, Location::INTERIOR), Dimension::True);
  EXPECT_EQ(im.get(Location::BOUNDARY, Location::BOUNDARY), Dimension::False);
  EXPECT_EQ(im.get(Location::BOUNDARY, Location::EXTERIOR),
            Dimension::DONTCARE);
  EXPECT_EQ(im.get(Location::EXTERIOR, Location::INTERIOR), Dimension::A);
  EXPECT_EQ(im.get(Location::EXTERIOR, Location::BOUNDARY), Dimension::L);
  EXPECT_EQ(im.get(Location::EXTERIOR, Location::EXTERIOR), Dimension::P);
  EXPECT_EQ(im.toString(), "012TF*210");
}

TEST(IntersectionMatrixTest, SetByStringUpdatesPrefixAndValidatesSymbols) {
  IntersectionMatrix im("222222222");
  im.set("01*");
  EXPECT_EQ(im.toString(), "01*222222");

  im.set("");
  EXPECT_EQ(im.toString(), "01*222222");

  im.set("012TF*012?");
  EXPECT_EQ(im.toString(), "012TF*012");

  EXPECT_THROW(im.set("012X56789"), IllegalArgumentException);
}

TEST(IntersectionMatrixTest, SetAllOverridesEveryCell) {
  IntersectionMatrix im("012TF*210");
  im.setAll(Dimension::DONTCARE);
  EXPECT_EQ(im.toString(), "*********");

  im.setAll(Dimension::A);
  EXPECT_EQ(im.toString(), "222222222");

  im.setAll(Dimension::True);
  EXPECT_EQ(im.toString(), "TTTTTTTTT");

  im.setAll(Dimension::False);
  EXPECT_EQ(im.toString(), "FFFFFFFFF");
}

TEST(IntersectionMatrixTest, SetAtLeastElementRespectsDimensionOrdering) {
  IntersectionMatrix im;
  im.setAll(Dimension::DONTCARE);

  constexpr auto kOrderedDims = std::to_array<int>({
      Dimension::True,
      Dimension::False,
      Dimension::P,
      Dimension::L,
      Dimension::A,
  });
  for (int dim : kOrderedDims) {
    im.setAtLeast(Location::BOUNDARY, Location::INTERIOR, dim);
    EXPECT_EQ(im.get(Location::BOUNDARY, Location::INTERIOR), dim);
  }

  // Lower or equal values must not overwrite the current maximum.
  for (int dim : {Dimension::A, Dimension::L, Dimension::P, Dimension::False,
                  Dimension::True, Dimension::DONTCARE}) {
    im.setAtLeast(Location::BOUNDARY, Location::INTERIOR, dim);
    EXPECT_EQ(im.get(Location::BOUNDARY, Location::INTERIOR), Dimension::A);
  }
}

TEST(IntersectionMatrixTest, SetAtLeastIfValidIgnoresLocationNone) {
  IntersectionMatrix im("FFFFFFFFF");
  im.setAtLeastIfValid(Location::NONE, Location::INTERIOR, Dimension::A);
  im.setAtLeastIfValid(Location::INTERIOR, Location::NONE, Dimension::A);
  im.setAtLeastIfValid(Location::NONE, Location::NONE, Dimension::A);
  EXPECT_EQ(im.toString(), "FFFFFFFFF");

  im.setAtLeastIfValid(Location::BOUNDARY, Location::EXTERIOR, Dimension::L);
  EXPECT_EQ(im.toString(), "FFFFF1FFF");

  im.setAtLeastIfValid(Location::BOUNDARY, Location::EXTERIOR, Dimension::P);
  EXPECT_EQ(im.toString(), "FFFFF1FFF");
}

TEST(IntersectionMatrixTest, SetAtLeastByStringTakesElementwiseMaximum) {
  IntersectionMatrix im("*TFFF012*");
  im.setAtLeast("F*T012012");
  EXPECT_EQ(im.toString(), "FTF012122");

  IntersectionMatrix partial("000000000");
  partial.setAtLeast("21*");
  EXPECT_EQ(partial.toString(), "210000000");

  partial.setAtLeast("");
  EXPECT_EQ(partial.toString(), "210000000");

  EXPECT_THROW(partial.setAtLeast("012Z"), IllegalArgumentException);
}

TEST(IntersectionMatrixTest, AddTakesElementwiseMaximumFromOtherMatrix) {
  IntersectionMatrix im1("*TF012*01");
  IntersectionMatrix im2("FT*2102FF");

  im1.add(&im2);
  EXPECT_EQ(im1.toString(), "FTF212201");
  EXPECT_EQ(im2.toString(), "FT*2102FF");

  im1.add(&im1);
  EXPECT_EQ(im1.toString(), "FTF212201");
}

TEST(IntersectionMatrixTest, MatchesSingleDimensionValueAndSymbol) {
  for (const SingleMatchCase& tc : kSingleMatchCases) {
    SCOPED_TRACE(tc.actual_dimension);
    SCOPED_TRACE(tc.required_symbol);
    EXPECT_EQ(
        IntersectionMatrix::matches(tc.actual_dimension, tc.required_symbol),
        tc.expected);
  }
}

TEST(IntersectionMatrixTest, MatchesPatternInstanceAndStaticOverloads) {
  for (const PatternMatchCase& tc : kPatternMatchCases) {
    SCOPED_TRACE(std::string(tc.actual) + " vs " + std::string(tc.pattern));
    const std::string actual(tc.actual);
    const std::string pattern(tc.pattern);
    IntersectionMatrix im(actual);
    EXPECT_EQ(im.matches(pattern), tc.expected);
    EXPECT_EQ(IntersectionMatrix::matches(actual, pattern), tc.expected);
  }

  IntersectionMatrix im("212101212");
  EXPECT_THROW((void)im.matches(""), IllegalArgumentException);
  EXPECT_THROW((void)im.matches("21210121"), IllegalArgumentException);
  EXPECT_THROW((void)im.matches("2121012122"), IllegalArgumentException);

  EXPECT_THROW((void)IntersectionMatrix::matches("212101212", "21210121"),
               IllegalArgumentException);
  EXPECT_THROW((void)IntersectionMatrix::matches("212101212", "2121012122"),
               IllegalArgumentException);
  EXPECT_THROW((void)IntersectionMatrix::matches("012345678", "*********"),
               IllegalArgumentException);
}

TEST(IntersectionMatrixTest, IsDisjointAndIsIntersects) {
  for (const MatrixPredicateCase& tc : kDisjointCases) {
    SCOPED_TRACE(std::string(tc.matrix));
    IntersectionMatrix im{std::string(tc.matrix)};
    EXPECT_EQ(im.isDisjoint(), tc.expected);
    EXPECT_EQ(im.isIntersects(), !tc.expected);
  }
}

TEST(IntersectionMatrixTest, IsTouches) {
  for (const DimPairPredicateCase& tc : kTouchesCases) {
    SCOPED_TRACE(std::string(tc.matrix));
    SCOPED_TRACE(tc.dim_a);
    SCOPED_TRACE(tc.dim_b);
    IntersectionMatrix im{std::string(tc.matrix)};
    EXPECT_EQ(im.isTouches(tc.dim_a, tc.dim_b), tc.expected);
  }
}

TEST(IntersectionMatrixTest, IsCrosses) {
  for (const DimPairPredicateCase& tc : kCrossesCases) {
    SCOPED_TRACE(std::string(tc.matrix));
    SCOPED_TRACE(tc.dim_a);
    SCOPED_TRACE(tc.dim_b);
    IntersectionMatrix im{std::string(tc.matrix)};
    EXPECT_EQ(im.isCrosses(tc.dim_a, tc.dim_b), tc.expected);
  }
}

TEST(IntersectionMatrixTest, IsWithinAndIsContains) {
  for (const MatrixPredicateCase& tc : kWithinCases) {
    SCOPED_TRACE(std::string(tc.matrix));
    IntersectionMatrix im{std::string(tc.matrix)};
    EXPECT_EQ(im.isWithin(), tc.expected);
  }
  for (const MatrixPredicateCase& tc : kContainsCases) {
    SCOPED_TRACE(std::string(tc.matrix));
    IntersectionMatrix im{std::string(tc.matrix)};
    EXPECT_EQ(im.isContains(), tc.expected);
  }
}

TEST(IntersectionMatrixTest, IsEquals) {
  for (const DimPairPredicateCase& tc : kEqualsCases) {
    SCOPED_TRACE(std::string(tc.matrix));
    SCOPED_TRACE(tc.dim_a);
    SCOPED_TRACE(tc.dim_b);
    IntersectionMatrix im{std::string(tc.matrix)};
    EXPECT_EQ(im.isEquals(tc.dim_a, tc.dim_b), tc.expected);
  }
}

TEST(IntersectionMatrixTest, IsOverlaps) {
  for (const DimPairPredicateCase& tc : kOverlapsCases) {
    SCOPED_TRACE(std::string(tc.matrix));
    SCOPED_TRACE(tc.dim_a);
    SCOPED_TRACE(tc.dim_b);
    IntersectionMatrix im{std::string(tc.matrix)};
    EXPECT_EQ(im.isOverlaps(tc.dim_a, tc.dim_b), tc.expected);
  }
}

TEST(IntersectionMatrixTest, IsCoversAndIsCoveredBy) {
  for (const MatrixPredicateCase& tc : kCoversCases) {
    SCOPED_TRACE(std::string(tc.matrix));
    IntersectionMatrix im{std::string(tc.matrix)};
    EXPECT_EQ(im.isCovers(), tc.expected);
  }
  for (const MatrixPredicateCase& tc : kCoveredByCases) {
    SCOPED_TRACE(std::string(tc.matrix));
    IntersectionMatrix im{std::string(tc.matrix)};
    EXPECT_EQ(im.isCoveredBy(), tc.expected);
  }
}

TEST(IntersectionMatrixTest, TransposeSwapsOffDiagonalAndPreservesDuality) {
  IntersectionMatrix im("01*T2F021");
  EXPECT_EQ(im.transpose(), &im);
  EXPECT_EQ(im.toString(), "0T0122*F1");
  EXPECT_EQ(im.transpose(), &im);
  EXPECT_EQ(im.toString(), "01*T2F021");

  IntersectionMatrix within_im("T*F**F***");
  EXPECT_TRUE(within_im.isWithin());
  EXPECT_FALSE(within_im.isContains());
  within_im.transpose();
  EXPECT_EQ(within_im.toString(), "T*****FF*");
  EXPECT_TRUE(within_im.isContains());
  EXPECT_FALSE(within_im.isWithin());

  IntersectionMatrix covered_by_im("*TF**F***");
  EXPECT_TRUE(covered_by_im.isCoveredBy());
  EXPECT_FALSE(covered_by_im.isCovers());
  covered_by_im.transpose();
  EXPECT_EQ(covered_by_im.toString(), "***T**FF*");
  EXPECT_TRUE(covered_by_im.isCovers());
  EXPECT_FALSE(covered_by_im.isCoveredBy());

  IntersectionMatrix crosses_im("0F0FFFFFF");
  EXPECT_TRUE(crosses_im.isCrosses(Dimension::P, Dimension::A));
  EXPECT_FALSE(crosses_im.isCrosses(Dimension::A, Dimension::P));
  crosses_im.transpose();
  EXPECT_EQ(crosses_im.toString(), "0FFFFF0FF");
  EXPECT_FALSE(crosses_im.isCrosses(Dimension::P, Dimension::A));
  EXPECT_TRUE(crosses_im.isCrosses(Dimension::A, Dimension::P));
}

TEST(IntersectionMatrixTest, ToStringAndStreamInsertion) {
  IntersectionMatrix im1("212FF1FF2");
  IntersectionMatrix im2("*********");

  std::ostringstream os;
  os << im1 << "|" << im2;
  EXPECT_EQ(im1.toString(), "212FF1FF2");
  EXPECT_EQ(os.str(), "212FF1FF2|*********");

  IntersectionMatrix invalid;
  invalid.set(Location::INTERIOR, Location::INTERIOR, 99);
  EXPECT_THROW((void)invalid.toString(), IllegalArgumentException);
  std::ostringstream invalid_os;
  EXPECT_THROW(invalid_os << invalid, IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
