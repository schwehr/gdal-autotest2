// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Unit tests for LercNS::UnitTypes
// (third_party/lerc/LercLib/fpl_UnitTypes.cpp).
//
// Note: Upstream fpl_UnitTypes.cpp compiles with `#define FLT_TYPES_ONLY`, so
// integer UnitType paths in size/derivative/restore functions assert(0).
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/fpl_UnitTypes.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/fpl_UnitTypes.cpp

#include "third_party/lerc/LercLib/fpl_UnitTypes.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

namespace lerc {
namespace {

using ::LercNS::UnitType;
using ::LercNS::UnitTypes;
using ::testing::ElementsAreArray;

TEST(FplUnitTypesTest, SizeTypeAndUnitCodeMapSupportedTypes) {
  // Under upstream FLT_TYPES_ONLY, only FLOAT and DOUBLE are valid in size().
  EXPECT_EQ(4U, UnitTypes::size(UnitType::UNIT_TYPE_FLOAT));
  EXPECT_EQ(8U, UnitTypes::size(UnitType::UNIT_TYPE_DOUBLE));

  struct TypeMappingCase {
    int num_bytes;
    bool is_float;
    UnitType expected_type;
    unsigned char expected_code;
  };
  constexpr TypeMappingCase kCases[] = {
      {.num_bytes = 1,
       .is_float = false,
       .expected_type = UnitType::UNIT_TYPE_BYTE,
       .expected_code = 1},
      {.num_bytes = 2,
       .is_float = false,
       .expected_type = UnitType::UNIT_TYPE_SHORT,
       .expected_code = 2},
      {.num_bytes = 4,
       .is_float = false,
       .expected_type = UnitType::UNIT_TYPE_LONG,
       .expected_code = 3},
      {.num_bytes = 8,
       .is_float = false,
       .expected_type = UnitType::UNIT_TYPE_64_BIT,
       .expected_code = 4},
      {.num_bytes = 4,
       .is_float = true,
       .expected_type = UnitType::UNIT_TYPE_FLOAT,
       .expected_code = 5},
      {.num_bytes = 8,
       .is_float = true,
       .expected_type = UnitType::UNIT_TYPE_DOUBLE,
       .expected_code = 6},
  };

  for (const auto& tc : kCases) {
    const UnitType got = UnitTypes::type(tc.num_bytes, tc.is_float);
    EXPECT_EQ(tc.expected_type, got);
    EXPECT_EQ(tc.expected_code, UnitTypes::unitCode(got));
  }
}

TEST(FplUnitTypesTest, FloatBitTransformRoundTripsAllSpecialBitPatterns) {
  const std::array<uint32_t, 8> original = {
      std::bit_cast<uint32_t>(0.0f),
      std::bit_cast<uint32_t>(-0.0f),
      std::bit_cast<uint32_t>(1.5f),
      std::bit_cast<uint32_t>(-42.25f),
      std::bit_cast<uint32_t>(1e-42f),
      std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity()),
      std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN()),
      0xFFFFFFFFU,
  };

  auto buf = original;
  UnitTypes::doFloatTransform(buf.data(), buf.size());
  // Sign bit moves from bit 31 to bit 23 and exponent moves up by 1 bit.
  EXPECT_EQ(0x00800000U, buf[1]);  // -0.0f (0x80000000) -> bit 23 set.
  UnitTypes::undoFloatTransform(buf.data(), buf.size());
  EXPECT_THAT(buf, ElementsAreArray(original));
}

TEST(FplUnitTypesTest, FloatDerivativeBlockAndCrossRoundTripsAreBitExact) {
  constexpr std::size_t kCols = 5;
  constexpr std::size_t kRows = 4;
  std::vector<uint32_t> original(kCols * kRows);
  for (std::size_t r = 0; r < kRows; ++r) {
    for (std::size_t c = 0; c < kCols; ++c) {
      const float val = (r % 2 == 0 ? 1.0f : -1.0f) *
                        static_cast<float>((r + 1) * 3.25 + c * 0.5);
      original[r * kCols + c] = std::bit_cast<uint32_t>(val);
    }
  }
  UnitTypes::doFloatTransform(original.data(), original.size());

  // 1. 1-D setDerivative: nLevel=0 is a no-op; level 1 and 2 invert cleanly
  //    when interpreted as a single row by restoreBlockSequence.
  const std::vector<uint32_t> first_row(original.begin(),
                                        original.begin() + kCols);
  auto line = first_row;
  UnitTypes::setDerivative(UnitType::UNIT_TYPE_FLOAT, line.data(), kCols, 0);
  EXPECT_THAT(line, ElementsAreArray(first_row));
  UnitTypes::restoreBlockSequence(0, line.data(), kCols, 1,
                                  UnitType::UNIT_TYPE_FLOAT);
  EXPECT_THAT(line, ElementsAreArray(first_row));

  UnitTypes::setDerivative(UnitType::UNIT_TYPE_FLOAT, line.data(), kCols, 1, 1);
  EXPECT_NE(line, first_row);
  UnitTypes::setDerivative(UnitType::UNIT_TYPE_FLOAT, line.data(), kCols, 2, 2);
  EXPECT_NE(line, first_row);
  UnitTypes::restoreBlockSequence(2, line.data(), kCols, 1,
                                  UnitType::UNIT_TYPE_FLOAT);
  EXPECT_THAT(line, ElementsAreArray(first_row));

  // 2. setRowsDerivative / setBlockDerivative + restoreBlockSequence across
  //    phase 0, phase 1 -> phase 2, and level 0 no-op.
  {
    auto grid = original;
    UnitTypes::setBlockDerivative(UnitType::UNIT_TYPE_FLOAT, grid.data(), kCols,
                                  kRows, 0);
    EXPECT_THAT(grid, ElementsAreArray(original));

    UnitTypes::setBlockDerivative(UnitType::UNIT_TYPE_FLOAT, grid.data(), kCols,
                                  kRows, 2, 1);
    EXPECT_NE(grid, original);
    UnitTypes::restoreBlockSequence(2, grid.data(), kCols, kRows,
                                    UnitType::UNIT_TYPE_FLOAT);
    EXPECT_THAT(grid, ElementsAreArray(original));

    UnitTypes::setBlockDerivative(UnitType::UNIT_TYPE_FLOAT, grid.data(), kCols,
                                  kRows, 1, 1);
    EXPECT_NE(grid, original);
    auto phase1 = grid;
    UnitTypes::restoreBlockSequence(1, phase1.data(), kCols, kRows,
                                    UnitType::UNIT_TYPE_FLOAT);
    EXPECT_THAT(phase1, ElementsAreArray(original));

    UnitTypes::setBlockDerivative(UnitType::UNIT_TYPE_FLOAT, grid.data(), kCols,
                                  kRows, 2, 2);
    EXPECT_NE(grid, original);
    UnitTypes::restoreBlockSequence(2, grid.data(), kCols, kRows,
                                    UnitType::UNIT_TYPE_FLOAT);
    EXPECT_THAT(grid, ElementsAreArray(original));
  }

  // 3. setCrossDerivative + restoreCrossBytes across phases 0, 1, 2 & delta
  // 0/1/2.
  {
    auto grid = original;
    UnitTypes::setCrossDerivative(UnitType::UNIT_TYPE_FLOAT, grid.data(), kCols,
                                  kRows, 2, 0);
    EXPECT_NE(grid, original);
    UnitTypes::restoreCrossBytes(0, grid.data(), kCols, kRows,
                                 UnitType::UNIT_TYPE_FLOAT);
    EXPECT_NE(grid, original);
    UnitTypes::restoreCrossBytes(2, grid.data(), kCols, kRows,
                                 UnitType::UNIT_TYPE_FLOAT);
    EXPECT_THAT(grid, ElementsAreArray(original));

    UnitTypes::setCrossDerivative(UnitType::UNIT_TYPE_FLOAT, grid.data(), kCols,
                                  kRows, 2, 1);
    EXPECT_NE(grid, original);
    auto row_only = grid;
    UnitTypes::restoreCrossBytes(1, row_only.data(), kCols, kRows,
                                 UnitType::UNIT_TYPE_FLOAT);
    EXPECT_THAT(row_only, ElementsAreArray(original));

    UnitTypes::setCrossDerivative(UnitType::UNIT_TYPE_FLOAT, grid.data(), kCols,
                                  kRows, 2, 2);
    EXPECT_NE(grid, original);
    UnitTypes::restoreCrossBytes(2, grid.data(), kCols, kRows,
                                 UnitType::UNIT_TYPE_FLOAT);
    EXPECT_THAT(grid, ElementsAreArray(original));
  }
}

TEST(FplUnitTypesTest, DoubleDerivativeBlockAndCrossRoundTripsAreBitExact) {
  constexpr std::size_t kCols = 4;
  constexpr std::size_t kRows = 4;
  std::vector<uint64_t> original(kCols * kRows);
  for (std::size_t r = 0; r < kRows; ++r) {
    for (std::size_t c = 0; c < kCols; ++c) {
      const double val = (r + 1) * 100.125 - c * 0.0625;
      original[r * kCols + c] = std::bit_cast<uint64_t>(val);
    }
  }

  // 1. 1-D setDerivative and single-row restore.
  const std::vector<uint64_t> first_row(original.begin(),
                                        original.begin() + kCols);
  auto line = first_row;
  UnitTypes::setDerivative(UnitType::UNIT_TYPE_DOUBLE, line.data(), kCols, 2,
                           1);
  EXPECT_NE(line, first_row);
  UnitTypes::restoreBlockSequence(2, line.data(), kCols, 1,
                                  UnitType::UNIT_TYPE_DOUBLE);
  EXPECT_THAT(line, ElementsAreArray(first_row));

  // 2. Multi-row setRowsDerivative (phases 0, 1, 2) + restoreBlockSequence,
  //    using fresh grids per phase so phase checks stay independent.
  {
    auto grid = original;
    UnitTypes::setRowsDerivative(UnitType::UNIT_TYPE_DOUBLE, grid.data(), kCols,
                                 kRows, 2, 0);
    EXPECT_NE(grid, original);
    UnitTypes::restoreBlockSequence(2, grid.data(), kCols, kRows,
                                    UnitType::UNIT_TYPE_DOUBLE);
    EXPECT_THAT(grid, ElementsAreArray(original));
  }

  {
    auto grid = original;
    UnitTypes::setRowsDerivative(UnitType::UNIT_TYPE_DOUBLE, grid.data(), kCols,
                                 kRows, 2, 1);
    EXPECT_NE(grid, original);
    auto row_delta1 = grid;
    UnitTypes::restoreBlockSequence(1, row_delta1.data(), kCols, kRows,
                                    UnitType::UNIT_TYPE_DOUBLE);
    EXPECT_THAT(row_delta1, ElementsAreArray(original));

    UnitTypes::setRowsDerivative(UnitType::UNIT_TYPE_DOUBLE, grid.data(), kCols,
                                 kRows, 2, 2);
    EXPECT_NE(grid, original);
    UnitTypes::restoreBlockSequence(2, grid.data(), kCols, kRows,
                                    UnitType::UNIT_TYPE_DOUBLE);
    EXPECT_THAT(grid, ElementsAreArray(original));
  }

  // 3. Multi-row setCrossDerivative (phases 0, 1, 2) + restoreCrossBytes.
  {
    auto grid = original;
    UnitTypes::setCrossDerivative(UnitType::UNIT_TYPE_DOUBLE, grid.data(),
                                  kCols, kRows, 2, 0);
    EXPECT_NE(grid, original);
    UnitTypes::restoreCrossBytes(2, grid.data(), kCols, kRows,
                                 UnitType::UNIT_TYPE_DOUBLE);
    EXPECT_THAT(grid, ElementsAreArray(original));
  }

  {
    auto grid = original;
    UnitTypes::setCrossDerivative(UnitType::UNIT_TYPE_DOUBLE, grid.data(),
                                  kCols, kRows, 2, 1);
    EXPECT_NE(grid, original);
    auto cross_p1 = grid;
    UnitTypes::restoreCrossBytes(1, cross_p1.data(), kCols, kRows,
                                 UnitType::UNIT_TYPE_DOUBLE);
    EXPECT_THAT(cross_p1, ElementsAreArray(original));

    UnitTypes::setCrossDerivative(UnitType::UNIT_TYPE_DOUBLE, grid.data(),
                                  kCols, kRows, 2, 2);
    EXPECT_NE(grid, original);
    UnitTypes::restoreCrossBytes(2, grid.data(), kCols, kRows,
                                 UnitType::UNIT_TYPE_DOUBLE);
    EXPECT_THAT(grid, ElementsAreArray(original));
  }
}

}  // namespace
}  // namespace lerc
