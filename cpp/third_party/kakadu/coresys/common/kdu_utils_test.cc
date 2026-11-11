// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests for coresys/common/kdu_utils.h.

#include "third_party/kakadu/vendor/coresys/common/kdu_utils.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_core {
namespace {

TEST(KduUtilsTest, DoubleToIntClamp) {
  EXPECT_EQ(0, double_to_int_clamp(0.0));
  EXPECT_EQ(42, double_to_int_clamp(42.9));
  EXPECT_EQ(-42, double_to_int_clamp(-42.9));
  EXPECT_EQ(0, double_to_int_clamp(std::numeric_limits<double>::quiet_NaN()));
  EXPECT_EQ(std::numeric_limits<int>::max(), double_to_int_clamp(1e20));
  EXPECT_EQ(std::numeric_limits<int>::max(),
            double_to_int_clamp(std::numeric_limits<double>::infinity()));
  EXPECT_EQ(std::numeric_limits<int>::min(), double_to_int_clamp(-1e20));
  EXPECT_EQ(std::numeric_limits<int>::min(),
            double_to_int_clamp(-std::numeric_limits<double>::infinity()));
}

TEST(KduUtilsTest, DoubleToFloatThrowsOnOutOfRangeOrNaN) {
  EXPECT_FLOAT_EQ(0.0f, double_to_float(0.0));
  EXPECT_FLOAT_EQ(1.5f, double_to_float(1.5));
  EXPECT_FLOAT_EQ(-2.5f, double_to_float(-2.5));
  EXPECT_FLOAT_EQ(std::numeric_limits<float>::max(),
                  double_to_float(std::numeric_limits<float>::max()));
  EXPECT_FLOAT_EQ(std::numeric_limits<float>::lowest(),
                  double_to_float(std::numeric_limits<float>::lowest()));

  auto expect_dead0 = [](double d) {
    try {
      (void)double_to_float(d);
      FAIL() << "Expected 0xDEAD0 for input " << d;
    } catch (int exc) {
      EXPECT_EQ(0xDEAD0, exc);
    }
  };
  expect_dead0(std::numeric_limits<double>::quiet_NaN());
  expect_dead0(1e100);
  expect_dead0(-1e100);
  expect_dead0(std::numeric_limits<double>::infinity());
  expect_dead0(-std::numeric_limits<double>::infinity());
}

TEST(KduUtilsTest, DoubleToFloatClamp) {
  EXPECT_FLOAT_EQ(1.5f, double_to_float_clamp(1.5));
  EXPECT_FLOAT_EQ(std::numeric_limits<float>::max(),
                  double_to_float_clamp(1e100));
  EXPECT_TRUE(std::isnan(
      double_to_float_clamp(std::numeric_limits<double>::quiet_NaN())));
  // TODO(b/309473610): double_to_float_clamp checks <= FLT_MIN rather than
  // FLT_LOWEST, so inputs <= +FLT_MIN (including 0 and negatives) are clamped
  // up to +FLT_MIN. Avoid asserting that buggy branch.
}

TEST(KduUtilsTest, KduReadIntegersAndShortBufferThrows) {
  auto bytes = std::to_array<kdu_byte>(
      {0x12, 0x34, 0x56, 0x78, 0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB});
  kdu_byte* bp = bytes.data();
  kdu_byte* end = bytes.data() + bytes.size();

  EXPECT_EQ(0x12, kdu_read(bp, end, 1));
  EXPECT_EQ(bytes.data() + 1, bp);

  EXPECT_EQ(0x3456, kdu_read(bp, end, 2));
  EXPECT_EQ(bytes.data() + 3, bp);

  EXPECT_EQ(0x780102, kdu_read(bp, end, 3));
  EXPECT_EQ(bytes.data() + 6, bp);

  EXPECT_EQ(0x0304AABB, kdu_read(bp, end, 4));
  EXPECT_EQ(end, bp);

  // Reading past end throws the original bp without advancing it.
  auto short_buf = std::to_array<kdu_byte>({0x11, 0x22});
  kdu_byte* short_bp = short_buf.data();
  try {
    (void)kdu_read(short_bp, short_buf.data() + short_buf.size(), 3);
    FAIL() << "Expected kdu_byte* exception on short buffer";
  } catch (kdu_byte* exc) {
    EXPECT_EQ(short_buf.data(), exc);
    EXPECT_EQ(short_buf.data(), short_bp);
  }
}

TEST(KduUtilsTest, KduReadFloatAndDouble) {
  // IEEE-754 big-endian: 1.5f = 0x3FC00000, -2.25f = 0xC0100000.
  auto fbytes = std::to_array<kdu_byte>(
      {0x3F, 0xC0, 0x00, 0x00, 0xC0, 0x10, 0x00, 0x00, 0x99});
  kdu_byte* bp = fbytes.data();
  kdu_byte* end = fbytes.data() + fbytes.size();
  EXPECT_FLOAT_EQ(1.5f, kdu_read_float(bp, end));
  EXPECT_EQ(fbytes.data() + 4, bp);
  EXPECT_FLOAT_EQ(-2.25f, kdu_read_float(bp, end));
  EXPECT_EQ(fbytes.data() + 8, bp);
  try {
    (void)kdu_read_float(bp, end);
    FAIL() << "Expected kdu_byte* exception on short float buffer";
  } catch (kdu_byte* exc) {
    EXPECT_EQ(fbytes.data() + 8, exc);
  }

  // IEEE-754 big-endian: -3.5 = 0xC00C000000000000.
  auto dbytes =
      std::to_array<kdu_byte>({0xC0, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  bp = dbytes.data();
  end = dbytes.data() + dbytes.size();
  EXPECT_DOUBLE_EQ(-3.5, kdu_read_double(bp, end));
  EXPECT_EQ(end, bp);
  try {
    (void)kdu_read_double(bp, end);
    FAIL() << "Expected kdu_byte* exception on short double buffer";
  } catch (kdu_byte* exc) {
    EXPECT_EQ(end, exc);
  }
}

TEST(KduUtilsTest, KduRound) {
  EXPECT_DOUBLE_EQ(0.0, kdu_round(0.0));
  EXPECT_DOUBLE_EQ(0.0, kdu_round(0.4));
  EXPECT_DOUBLE_EQ(1.0, kdu_round(0.6));
  EXPECT_DOUBLE_EQ(0.0, kdu_round(-0.4));
  EXPECT_DOUBLE_EQ(-1.0, kdu_round(-0.6));
}

TEST(KduUtilsTest, CeilRatioAndBadDenominator) {
  EXPECT_EQ(0, ceil_ratio(0, 1));
  EXPECT_EQ(1, ceil_ratio(1, 1));
  EXPECT_EQ(1, ceil_ratio(1, 10));
  EXPECT_EQ(10, ceil_ratio(10, 1));
  EXPECT_EQ(2, ceil_ratio(11, 10));
  EXPECT_EQ(0, ceil_ratio(-1, 4));
  EXPECT_EQ(-1, ceil_ratio(-4, 4));
  EXPECT_EQ(-1, ceil_ratio(-5, 4));

  // Regression coverage for b/339429949 (-INT_MIN overflow).
  EXPECT_EQ(ceil_ratio(std::numeric_limits<int>::min(), 1),
            std::numeric_limits<int>::min());

  for (int bad_den : {0, -1}) {
    try {
      (void)ceil_ratio(1, bad_den);
      FAIL() << "Expected 0xDEAD1 for den=" << bad_den;
    } catch (int exc) {
      EXPECT_EQ(0xDEAD1, exc);
    }
  }
}

TEST(KduUtilsTest, FloorRatioAndBadDenominator) {
  EXPECT_EQ(0, floor_ratio(0, 1));
  EXPECT_EQ(1, floor_ratio(1, 1));
  EXPECT_EQ(0, floor_ratio(1, 10));
  EXPECT_EQ(10, floor_ratio(10, 1));
  EXPECT_EQ(1, floor_ratio(11, 10));
  EXPECT_EQ(-1, floor_ratio(-1, 4));
  EXPECT_EQ(-1, floor_ratio(-4, 4));
  EXPECT_EQ(-2, floor_ratio(-5, 4));

  for (int bad_den : {0, -1}) {
    try {
      (void)floor_ratio(1, bad_den);
      FAIL() << "Expected 0xDEAD2 for den=" << bad_den;
    } catch (int exc) {
      EXPECT_EQ(0xDEAD2, exc);
    }
  }
}

TEST(KduUtilsTest, LongCeilRatioAndBadDenominator) {
  EXPECT_EQ(0, long_ceil_ratio(0, 1));
  EXPECT_EQ(1, long_ceil_ratio(1, 1));
  EXPECT_EQ(1, long_ceil_ratio(1, 10));
  EXPECT_EQ(10, long_ceil_ratio(10, 1));
  EXPECT_EQ(2, long_ceil_ratio(11, 10));
  EXPECT_EQ(-1, long_ceil_ratio(-5, 4));
  EXPECT_EQ(7, long_ceil_ratio(kdu_long{13} << 32, kdu_long{2} << 32));

  for (kdu_long bad_den : {kdu_long{0}, kdu_long{-1}}) {
    try {
      (void)long_ceil_ratio(1, bad_den);
      FAIL() << "Expected 0xDEAD3 for den=" << bad_den;
    } catch (int exc) {
      EXPECT_EQ(0xDEAD3, exc);
    }
  }
}

TEST(KduUtilsTest, LongFloorRatioAndBadDenominator) {
  EXPECT_EQ(0, long_floor_ratio(0, 1));
  EXPECT_EQ(1, long_floor_ratio(1, 1));
  EXPECT_EQ(0, long_floor_ratio(1, 10));
  EXPECT_EQ(10, long_floor_ratio(10, 1));
  EXPECT_EQ(1, long_floor_ratio(11, 10));
  EXPECT_EQ(-2, long_floor_ratio(-5, 4));
  EXPECT_EQ(6, long_floor_ratio(kdu_long{13} << 32, kdu_long{2} << 32));

  for (kdu_long bad_den : {kdu_long{0}, kdu_long{-1}}) {
    try {
      (void)long_floor_ratio(1, bad_den);
      FAIL() << "Expected 0xDEAD4 for den=" << bad_den;
    } catch (int exc) {
      EXPECT_EQ(0xDEAD4, exc);
    }
  }
}

TEST(KduUtilsTest, HexHexEncodeAndDecode) {
  constexpr char kRaw[] = "a b\n<?&>\x80!";
  // Measure without destination buffer first (skipping trailing "!\0").
  const int encoded_len =
      kdu_hex_hex_encode(kRaw, nullptr, kRaw + sizeof(kRaw) - 2, "?&");
  ASSERT_GT(encoded_len, 0);

  std::string encoded(static_cast<size_t>(encoded_len), '\0');
  EXPECT_EQ(encoded_len, kdu_hex_hex_encode(kRaw, encoded.data(),
                                            kRaw + sizeof(kRaw) - 2, "?&"));
  EXPECT_EQ("a%20b%0A%3C%3F%26%3E%80", encoded);

  // Decode in place, including lowercase/alpha hex and invalid %-sequences.
  std::string mixed = "a%20b%0a%3c%3F%26%3E%80%ab%ZZ%2";
  EXPECT_EQ(mixed.data(), kdu_hex_hex_decode(mixed.data()));
  EXPECT_STREQ("a b\n<?&>\x80\xAB%ZZ%2", mixed.c_str());

  // Verify src_lim termination in decode.
  char bounded[] = "hello%20world";
  EXPECT_STREQ("hello", kdu_hex_hex_decode(bounded, bounded + 5));
}

TEST(KduUtilsTest, CountLeadingAndTrailingZeros) {
  EXPECT_EQ(31U, kdu_count_leading_zeros(1U));
  EXPECT_EQ(30U, kdu_count_leading_zeros(2U));
  EXPECT_EQ(8U, kdu_count_leading_zeros(0x00FF0000U));
  EXPECT_EQ(0U, kdu_count_leading_zeros(0x80000000U));

  EXPECT_EQ(63ULL, kdu_count_leading_zeros64(1ULL));
  EXPECT_EQ(31ULL, kdu_count_leading_zeros64(1ULL << 32));
  EXPECT_EQ(4ULL, kdu_count_leading_zeros64(0x0F00000000000000ULL));
  EXPECT_EQ(0ULL, kdu_count_leading_zeros64(1ULL << 63));

  EXPECT_EQ(0U, kdu_count_trailing_zeros(1U));
  EXPECT_EQ(1U, kdu_count_trailing_zeros(2U));
  EXPECT_EQ(8U, kdu_count_trailing_zeros(0x0F00U));
  EXPECT_EQ(31U, kdu_count_trailing_zeros(0x80000000U));

  EXPECT_EQ(0ULL, kdu_count_trailing_zeros64(1ULL));
  EXPECT_EQ(8ULL, kdu_count_trailing_zeros64(0x0F00ULL));
  EXPECT_EQ(32ULL, kdu_count_trailing_zeros64(1ULL << 32));
  EXPECT_EQ(63ULL, kdu_count_trailing_zeros64(1ULL << 63));
}

TEST(KduMemSafe, Add) {
  EXPECT_EQ(0, kdu_memsafe_add(0, 0));

  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_add(0, std::numeric_limits<size_t>::max()));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_add(std::numeric_limits<size_t>::max(), 0));

  // KDU_SIZE_MAX can exceed KDU_INT64_MAX because size_t is unsigned.
  EXPECT_EQ(KDU_INT64_MAX - 1, kdu_memsafe_add(KDU_INT64_MAX - 2, 1));
  EXPECT_EQ(KDU_INT64_MAX - 1, kdu_memsafe_add(1, KDU_INT64_MAX - 2));

  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_add(std::numeric_limits<size_t>::max(), 1));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_add(1, std::numeric_limits<size_t>::max()));
  EXPECT_EQ(KDU_SIZE_MAX, kdu_memsafe_add(std::numeric_limits<size_t>::max(),
                                          std::numeric_limits<size_t>::max()));
}

TEST(KduMemSafe, Multiply) {
  EXPECT_EQ(0, kdu_memsafe_mul(0, 0));
  EXPECT_EQ(0, kdu_memsafe_mul(0, std::numeric_limits<size_t>::max()));
  EXPECT_EQ(0, kdu_memsafe_mul(std::numeric_limits<size_t>::max(), 0));

  EXPECT_EQ(KDU_SAFE_ROOT_SIZE_MAX, kdu_memsafe_mul(1, KDU_SAFE_ROOT_SIZE_MAX));
  EXPECT_EQ(KDU_SAFE_ROOT_SIZE_MAX, kdu_memsafe_mul(KDU_SAFE_ROOT_SIZE_MAX, 1));
  EXPECT_EQ((KDU_SAFE_ROOT_SIZE_MAX + 1) * 2,
            kdu_memsafe_mul(KDU_SAFE_ROOT_SIZE_MAX + 1, 2));

  EXPECT_EQ(KDU_SIZE_MAX, kdu_memsafe_mul(KDU_IDX_MAX, 2));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mul(1, std::numeric_limits<size_t>::max()));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mul(std::numeric_limits<size_t>::max(), 1));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mul(2, std::numeric_limits<size_t>::max()));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mul(std::numeric_limits<size_t>::max(), 2));
  EXPECT_EQ(KDU_SIZE_MAX, kdu_memsafe_mul(std::numeric_limits<size_t>::max(),
                                          std::numeric_limits<size_t>::max()));
}

TEST(KduMemSafe, MultiplyAndOffset) {
  EXPECT_EQ(0, kdu_memsafe_mulo(0, 0, 0));
  EXPECT_EQ(0, kdu_memsafe_mulo(1, 0, 0));
  EXPECT_EQ(0, kdu_memsafe_mulo(0, 1, 0));
  EXPECT_EQ(1, kdu_memsafe_mulo(0, 0, 1));

  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mulo(std::numeric_limits<size_t>::max(), 1, 0));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mulo(1, std::numeric_limits<size_t>::max(), 0));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mulo(0, 0, std::numeric_limits<size_t>::max()));

  EXPECT_EQ(KDU_INT64_MAX - 1, kdu_memsafe_mulo(KDU_INT64_MAX - 2, 1, 1));
  EXPECT_EQ(KDU_INT64_MAX - 1, kdu_memsafe_mulo(1, KDU_INT64_MAX - 2, 1));

  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mulo(1, std::numeric_limits<size_t>::max(), 1));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mulo(1, 1, std::numeric_limits<size_t>::max()));
  EXPECT_EQ(KDU_SIZE_MAX, kdu_memsafe_mulo(std::numeric_limits<size_t>::max(),
                                           std::numeric_limits<size_t>::max(),
                                           std::numeric_limits<size_t>::max()));
}

TEST(KduUtilsTest, RoundToInt32AndInt64Helpers) {
  // Valid helpers (note: float overloads round-half-up via +0.5f, whereas
  // double *_valid overloads invoke floor() directly without +0.5).
  EXPECT_EQ(2, kdu_round_to_int32_valid(1.7f));
  EXPECT_EQ(-2, kdu_round_to_int32_valid(-1.7f));
  EXPECT_EQ(2, kdu_round_to_int32_valid(2.2));
  EXPECT_EQ(-3, kdu_round_to_int32_valid(-2.2));

  EXPECT_EQ(kdu_int64{2}, kdu_round_to_int64_valid(1.7f));
  EXPECT_EQ(kdu_int64{-2}, kdu_round_to_int64_valid(-1.7f));
  EXPECT_EQ(kdu_int64{2}, kdu_round_to_int64_valid(2.2));
  EXPECT_EQ(kdu_int64{-3}, kdu_round_to_int64_valid(-2.2));

  // Clipped helpers.
  EXPECT_EQ(2, kdu_round_to_int32_clip(1.6f));
  EXPECT_EQ(-2, kdu_round_to_int32_clip(-1.6f));
  EXPECT_EQ(-1, kdu_round_to_int32_clip(-1.4f));
  EXPECT_EQ(KDU_INT32_MAX, kdu_round_to_int32_clip(1e20f));
  EXPECT_EQ(KDU_INT32_MIN, kdu_round_to_int32_clip(-1e20f));
  EXPECT_EQ(2, kdu_round_to_int32_clip(1.6));
  EXPECT_EQ(KDU_INT32_MAX, kdu_round_to_int32_clip(1e20));
  EXPECT_EQ(KDU_INT32_MIN, kdu_round_to_int32_clip(-1e20));

  EXPECT_EQ(kdu_int64{2}, kdu_round_to_int64_clip(1.6f));
  EXPECT_EQ(KDU_INT64_MAX, kdu_round_to_int64_clip(1e25f));
  EXPECT_EQ(KDU_INT64_MIN, kdu_round_to_int64_clip(-1e25f));
  EXPECT_EQ(kdu_int64{2}, kdu_round_to_int64_clip(1.6));
  EXPECT_EQ(KDU_INT64_MAX, kdu_round_to_int64_clip(1e25));
  EXPECT_EQ(KDU_INT64_MIN, kdu_round_to_int64_clip(-1e25));

  // Do-not-care helpers return 0 when outside representable range.
  EXPECT_EQ(3, kdu_round_to_int32_dnc(2.6f));
  EXPECT_EQ(0, kdu_round_to_int32_dnc(1e20f));
  EXPECT_EQ(0, kdu_round_to_int32_dnc(-1e20f));

  EXPECT_EQ(kdu_int64{3}, kdu_round_to_int64_dnc(2.6f));
  EXPECT_EQ(kdu_int64{0}, kdu_round_to_int64_dnc(1e25f));
  EXPECT_EQ(kdu_int64{0}, kdu_round_to_int64_dnc(-1e25f));
}

}  // namespace
}  // namespace kdu_core
