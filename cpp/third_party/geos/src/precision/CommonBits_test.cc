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

// Unit tests for third_party/geos/src/precision/CommonBits.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/precision/CommonBits.cpp

#include "third_party/geos/include/geos/precision/CommonBits.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::precision::CommonBits;

struct SignExpCase {
  std::string_view name;
  int64_t input;
  int64_t expected;
};

constexpr auto kSignExpCases = std::to_array<SignExpCase>({
    {.name = "zero", .input = 0, .expected = 0},
    {.name = "below_bit_52", .input = (int64_t{1} << 52) - 1, .expected = 0},
    {.name = "at_bit_52", .input = int64_t{1} << 52, .expected = 1},
    {.name = "ieee_one_pattern",
     .input = int64_t{0x3FF} << 52,
     .expected = 0x3FF},
    {
        .name = "int64_max",
        .input = std::numeric_limits<int64_t>::max(),
        .expected = 2047,
    },
    {.name = "minus_one", .input = -1, .expected = -1},
    {.name = "minus_bit_52", .input = -(int64_t{1} << 52), .expected = -1},
    {
        .name = "minus_bit_52_minus_one",
        .input = -(int64_t{1} << 52) - 1,
        .expected = -2,
    },
    {
        .name = "int64_min",
        .input = std::numeric_limits<int64_t>::min(),
        .expected = -2048,
    },
});

TEST(CommonBitsTest, SignExpBitsExtractsTopTwelveSignExtendedBits) {
  for (const auto& tc : kSignExpCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(CommonBits::signExpBits(tc.input), tc.expected);
  }
}

TEST(CommonBitsTest, GetBitReadsZeroToSixtyThree) {
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kBit52 = int64_t{1} << 52;

  constexpr std::array<int, 8> kBits = {0, 1, 31, 51, 52, 53, 62, 63};

  for (int bit : kBits) {
    SCOPED_TRACE(bit);
    EXPECT_EQ(CommonBits::getBit(0, bit), 0);
    EXPECT_EQ(CommonBits::getBit(-1, bit), 1);
  }

  EXPECT_EQ(CommonBits::getBit(kMin, 63), 1);
  EXPECT_EQ(CommonBits::getBit(kMin, 62), 0);
  EXPECT_EQ(CommonBits::getBit(kMin, 0), 0);

  EXPECT_EQ(CommonBits::getBit(kMax, 63), 0);
  EXPECT_EQ(CommonBits::getBit(kMax, 62), 1);
  EXPECT_EQ(CommonBits::getBit(kMax, 0), 1);

  EXPECT_EQ(CommonBits::getBit(kBit52, 52), 1);
  EXPECT_EQ(CommonBits::getBit(kBit52, 51), 0);
  EXPECT_EQ(CommonBits::getBit(kBit52, 53), 0);
}

struct CommonMantissaCase {
  std::string_view name;
  int64_t a;
  int64_t b;
  int expected;
};

constexpr auto kCommonMantissaCases = std::to_array<CommonMantissaCase>({
    {.name = "identical_zero", .a = 0, .b = 0, .expected = 52},
    {.name = "identical_nonzero",
     .a = 987654321,
     .b = 987654321,
     .expected = 52},
    {
        .name = "differ_only_above_bit_52",
        .a = 0x123,
        .b = (int64_t{7} << 53) | 0x123,
        .expected = 52,
    },
    {.name = "differ_at_bit_0", .a = 10, .b = 11, .expected = 52},
    {.name = "differ_at_bit_1", .a = 10, .b = 8, .expected = 51},
    {.name = "differ_at_bit_4", .a = 0, .b = 16, .expected = 48},
    {.name = "differ_at_bit_51", .a = 0, .b = int64_t{1} << 51, .expected = 1},
    {.name = "differ_at_bit_52", .a = 0, .b = int64_t{1} << 52, .expected = 0},
});

TEST(CommonBitsTest, NumCommonMostSigMantissaBitsCountsFromBit52) {
  for (const auto& tc : kCommonMantissaCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(CommonBits::numCommonMostSigMantissaBits(tc.a, tc.b),
              tc.expected);
    EXPECT_EQ(CommonBits::numCommonMostSigMantissaBits(tc.b, tc.a),
              tc.expected);
  }
}

struct ZeroLowerBitsCase {
  std::string_view name;
  int64_t bits;
  int n_bits;
  int64_t expected;
};

constexpr auto kZeroLowerBitsCases = std::to_array<ZeroLowerBitsCase>({
    {.name = "negative_count", .bits = 0x12345, .n_bits = -1, .expected = 0},
    {.name = "large_negative_count", .bits = -1, .n_bits = -100, .expected = 0},
    {.name = "count_64", .bits = -1, .n_bits = 64, .expected = 0},
    {.name = "count_above_64", .bits = 0x7FFF, .n_bits = 100, .expected = 0},
    {.name = "count_zero_preserves_positive",
     .bits = 0x12345,
     .n_bits = 0,
     .expected = 0x12345},
    {.name = "count_zero_preserves_negative",
     .bits = -1,
     .n_bits = 0,
     .expected = -1},
    {.name = "clear_low_bit", .bits = 7, .n_bits = 1, .expected = 6},
    {.name = "clear_low_16_bits",
     .bits = 0x12345678,
     .n_bits = 16,
     .expected = 0x12340000},
    {
        .name = "clear_low_52_bits",
        .bits = (int64_t{0x123} << 52) | 0xABCDE,
        .n_bits = 52,
        .expected = int64_t{0x123} << 52,
    },
    {
        .name = "clear_low_63_bits_negative",
        .bits = -1,
        .n_bits = 63,
        .expected = std::numeric_limits<int64_t>::min(),
    },
    {
        .name = "clear_low_63_bits_max",
        .bits = std::numeric_limits<int64_t>::max(),
        .n_bits = 63,
        .expected = 0,
    },
});

TEST(CommonBitsTest, ZeroLowerBitsHandlesBoundsAndMasks) {
  for (const auto& tc : kZeroLowerBitsCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(CommonBits::zeroLowerBits(tc.bits, tc.n_bits), tc.expected);
  }
}

TEST(CommonBitsTest, DefaultConstructedReturnsZero) {
  CommonBits cb;
  EXPECT_DOUBLE_EQ(cb.getCommon(), 0.0);
}

TEST(CommonBitsTest, SingleAddTruncatesToInt64) {
  {
    CommonBits cb;
    cb.add(1000000.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 1000000.0);
  }
  {
    CommonBits cb;
    cb.add(1000000.75);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 1000000.0);
  }
  {
    CommonBits cb;
    cb.add(-42.9);
    EXPECT_DOUBLE_EQ(cb.getCommon(), -42.0);
  }
  {
    CommonBits cb;
    cb.add(-0.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 0.0);
  }
}

TEST(CommonBitsTest, AccumulatesCommonPrefixBitsAcrossNumbers) {
  {
    CommonBits cb;
    cb.add(1000007.0);
    cb.add(1000005.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 1000006.0);
    cb.add(1000000.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 1000004.0);
  }
  {
    CommonBits cb;
    cb.add(10.0);
    cb.add(11.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 10.0);
    cb.add(12.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 8.0);
    cb.add(20.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 0.0);
  }
  {
    CommonBits cb;
    cb.add(1000000.25);
    cb.add(1000000.75);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 1000000.0);
  }
  {
    CommonBits cb;
    cb.add(-1001.0);
    cb.add(-1002.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), -1001.0);
    cb.add(-1000.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), -1008.0);
  }
}

TEST(CommonBitsTest, ResetsToZeroWhenSignOrUpperExponentBitsDiffer) {
  {
    CommonBits cb;
    cb.add(-1000.0);
    cb.add(1000.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 0.0);
    cb.add(-1000.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 0.0);
  }
  {
    CommonBits cb;
    cb.add(1000.0);
    cb.add(-1000.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 0.0);
  }
  {
    constexpr double kTwoPow52 = static_cast<double>(int64_t{1} << 52);
    CommonBits cb;
    cb.add(kTwoPow52 + 8.0);
    cb.add(kTwoPow52 + 12.0);
    EXPECT_DOUBLE_EQ(cb.getCommon(), kTwoPow52 + 8.0);

    cb.add(2.0 * kTwoPow52);
    EXPECT_DOUBLE_EQ(cb.getCommon(), 0.0);
  }
}

}  // namespace
}  // namespace geos_google
