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

#include "third_party/geos/include/geos/io/ByteOrderValues.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::io::ByteOrderValues;

struct Int32Case {
  int32_t value;
  std::array<unsigned char, 4> big_endian;
  std::array<unsigned char, 4> little_endian;
};

struct Uint32Case {
  uint32_t value;
  std::array<unsigned char, 4> big_endian;
  std::array<unsigned char, 4> little_endian;
};

struct Int64Case {
  int64_t value;
  std::array<unsigned char, 8> big_endian;
  std::array<unsigned char, 8> little_endian;
};

constexpr std::array<Int32Case, 6> kInt32Cases = {{
    {0, {0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00}},
    {0x01020304, {0x01, 0x02, 0x03, 0x04}, {0x04, 0x03, 0x02, 0x01}},
    {-1, {0xFF, 0xFF, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF, 0xFF}},
    {std::numeric_limits<int32_t>::max(),
     {0x7F, 0xFF, 0xFF, 0xFF},
     {0xFF, 0xFF, 0xFF, 0x7F}},
    {std::numeric_limits<int32_t>::min(),
     {0x80, 0x00, 0x00, 0x00},
     {0x00, 0x00, 0x00, 0x80}},
    {static_cast<int32_t>(0x89ABCDEFu),
     {0x89, 0xAB, 0xCD, 0xEF},
     {0xEF, 0xCD, 0xAB, 0x89}},
}};

constexpr std::array<Uint32Case, 6> kUint32Cases = {{
    {0u, {0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00}},
    {0x01020304u, {0x01, 0x02, 0x03, 0x04}, {0x04, 0x03, 0x02, 0x01}},
    {0x7FFFFFFFu, {0x7F, 0xFF, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF, 0x7F}},
    {0x80000000u, {0x80, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x80}},
    {0x89ABCDEFu, {0x89, 0xAB, 0xCD, 0xEF}, {0xEF, 0xCD, 0xAB, 0x89}},
    {std::numeric_limits<uint32_t>::max(),
     {0xFF, 0xFF, 0xFF, 0xFF},
     {0xFF, 0xFF, 0xFF, 0xFF}},
}};

constexpr std::array<Int64Case, 6> kInt64Cases = {{
    {0,
     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x0102030405060708LL,
     {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08},
     {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}},
    {-1,
     {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
     {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
    {std::numeric_limits<int64_t>::max(),
     {0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
     {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F}},
    {std::numeric_limits<int64_t>::min(),
     {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}},
    {static_cast<int64_t>(0x8192A3B4C5D6E7F8ULL),
     {0x81, 0x92, 0xA3, 0xB4, 0xC5, 0xD6, 0xE7, 0xF8},
     {0xF8, 0xE7, 0xD6, 0xC5, 0xB4, 0xA3, 0x92, 0x81}},
}};

TEST(ByteOrderValuesTest, GetIntBigAndLittleEndian) {
  for (const auto& tc : kInt32Cases) {
    EXPECT_EQ(ByteOrderValues::getInt(tc.big_endian.data(),
                                      ByteOrderValues::ENDIAN_BIG),
              tc.value);
    EXPECT_EQ(ByteOrderValues::getInt(tc.little_endian.data(),
                                      ByteOrderValues::ENDIAN_LITTLE),
              tc.value);
  }
}

TEST(ByteOrderValuesTest, PutIntBigAndLittleEndian) {
  for (const auto& tc : kInt32Cases) {
    std::array<unsigned char, 6> be_buf = {0xAA, 0xCC, 0xCC, 0xCC, 0xCC, 0x55};
    ByteOrderValues::putInt(tc.value, be_buf.data() + 1,
                            ByteOrderValues::ENDIAN_BIG);
    EXPECT_EQ(be_buf[0], 0xAA);
    EXPECT_EQ(be_buf[5], 0x55);
    EXPECT_EQ((std::array<unsigned char, 4>{be_buf[1], be_buf[2], be_buf[3],
                                            be_buf[4]}),
              tc.big_endian);
    EXPECT_EQ(
        ByteOrderValues::getInt(be_buf.data() + 1, ByteOrderValues::ENDIAN_BIG),
        tc.value);

    std::array<unsigned char, 6> le_buf = {0xAA, 0xCC, 0xCC, 0xCC, 0xCC, 0x55};
    ByteOrderValues::putInt(tc.value, le_buf.data() + 1,
                            ByteOrderValues::ENDIAN_LITTLE);
    EXPECT_EQ(le_buf[0], 0xAA);
    EXPECT_EQ(le_buf[5], 0x55);
    EXPECT_EQ((std::array<unsigned char, 4>{le_buf[1], le_buf[2], le_buf[3],
                                            le_buf[4]}),
              tc.little_endian);
    EXPECT_EQ(ByteOrderValues::getInt(le_buf.data() + 1,
                                      ByteOrderValues::ENDIAN_LITTLE),
              tc.value);
  }
}

TEST(ByteOrderValuesTest, GetUnsignedBigAndLittleEndian) {
  for (const auto& tc : kUint32Cases) {
    EXPECT_EQ(ByteOrderValues::getUnsigned(tc.big_endian.data(),
                                           ByteOrderValues::ENDIAN_BIG),
              tc.value);
    EXPECT_EQ(ByteOrderValues::getUnsigned(tc.little_endian.data(),
                                           ByteOrderValues::ENDIAN_LITTLE),
              tc.value);
  }
}

TEST(ByteOrderValuesTest, PutUnsignedBigAndLittleEndian) {
  for (const auto& tc : kUint32Cases) {
    std::array<unsigned char, 6> be_buf = {0xAA, 0xCC, 0xCC, 0xCC, 0xCC, 0x55};
    ByteOrderValues::putUnsigned(tc.value, be_buf.data() + 1,
                                 ByteOrderValues::ENDIAN_BIG);
    EXPECT_EQ(be_buf[0], 0xAA);
    EXPECT_EQ(be_buf[5], 0x55);
    EXPECT_EQ((std::array<unsigned char, 4>{be_buf[1], be_buf[2], be_buf[3],
                                            be_buf[4]}),
              tc.big_endian);
    EXPECT_EQ(ByteOrderValues::getUnsigned(be_buf.data() + 1,
                                           ByteOrderValues::ENDIAN_BIG),
              tc.value);

    std::array<unsigned char, 6> le_buf = {0xAA, 0xCC, 0xCC, 0xCC, 0xCC, 0x55};
    ByteOrderValues::putUnsigned(tc.value, le_buf.data() + 1,
                                 ByteOrderValues::ENDIAN_LITTLE);
    EXPECT_EQ(le_buf[0], 0xAA);
    EXPECT_EQ(le_buf[5], 0x55);
    EXPECT_EQ((std::array<unsigned char, 4>{le_buf[1], le_buf[2], le_buf[3],
                                            le_buf[4]}),
              tc.little_endian);
    EXPECT_EQ(ByteOrderValues::getUnsigned(le_buf.data() + 1,
                                           ByteOrderValues::ENDIAN_LITTLE),
              tc.value);
  }
}

TEST(ByteOrderValuesTest, GetLongBigAndLittleEndian) {
  for (const auto& tc : kInt64Cases) {
    EXPECT_EQ(ByteOrderValues::getLong(tc.big_endian.data(),
                                       ByteOrderValues::ENDIAN_BIG),
              tc.value);
    EXPECT_EQ(ByteOrderValues::getLong(tc.little_endian.data(),
                                       ByteOrderValues::ENDIAN_LITTLE),
              tc.value);
  }
}

TEST(ByteOrderValuesTest, PutLongBigAndLittleEndian) {
  for (const auto& tc : kInt64Cases) {
    std::array<unsigned char, 10> be_buf = {0xAA, 0xCC, 0xCC, 0xCC, 0xCC,
                                            0xCC, 0xCC, 0xCC, 0xCC, 0x55};
    ByteOrderValues::putLong(tc.value, be_buf.data() + 1,
                             ByteOrderValues::ENDIAN_BIG);
    EXPECT_EQ(be_buf[0], 0xAA);
    EXPECT_EQ(be_buf[9], 0x55);
    EXPECT_EQ((std::array<unsigned char, 8>{be_buf[1], be_buf[2], be_buf[3],
                                            be_buf[4], be_buf[5], be_buf[6],
                                            be_buf[7], be_buf[8]}),
              tc.big_endian);
    EXPECT_EQ(ByteOrderValues::getLong(be_buf.data() + 1,
                                       ByteOrderValues::ENDIAN_BIG),
              tc.value);

    std::array<unsigned char, 10> le_buf = {0xAA, 0xCC, 0xCC, 0xCC, 0xCC,
                                            0xCC, 0xCC, 0xCC, 0xCC, 0x55};
    ByteOrderValues::putLong(tc.value, le_buf.data() + 1,
                             ByteOrderValues::ENDIAN_LITTLE);
    EXPECT_EQ(le_buf[0], 0xAA);
    EXPECT_EQ(le_buf[9], 0x55);
    EXPECT_EQ((std::array<unsigned char, 8>{le_buf[1], le_buf[2], le_buf[3],
                                            le_buf[4], le_buf[5], le_buf[6],
                                            le_buf[7], le_buf[8]}),
              tc.little_endian);
    EXPECT_EQ(ByteOrderValues::getLong(le_buf.data() + 1,
                                       ByteOrderValues::ENDIAN_LITTLE),
              tc.value);
  }
}

TEST(ByteOrderValuesTest, GetDoubleKnownIeee754Representations) {
  static_assert(std::numeric_limits<double>::is_iec559);
  static_assert(sizeof(double) == 8);

  const std::array<unsigned char, 8> zero_be = {0, 0, 0, 0, 0, 0, 0, 0};
  const double pos_zero =
      ByteOrderValues::getDouble(zero_be.data(), ByteOrderValues::ENDIAN_BIG);
  EXPECT_EQ(pos_zero, 0.0);
  EXPECT_FALSE(std::signbit(pos_zero));

  const std::array<unsigned char, 8> neg_zero_be = {0x80, 0, 0, 0, 0, 0, 0, 0};
  const std::array<unsigned char, 8> neg_zero_le = {0, 0, 0, 0, 0, 0, 0, 0x80};
  const double neg_zero_from_be = ByteOrderValues::getDouble(
      neg_zero_be.data(), ByteOrderValues::ENDIAN_BIG);
  const double neg_zero_from_le = ByteOrderValues::getDouble(
      neg_zero_le.data(), ByteOrderValues::ENDIAN_LITTLE);
  EXPECT_EQ(neg_zero_from_be, 0.0);
  EXPECT_TRUE(std::signbit(neg_zero_from_be));
  EXPECT_EQ(neg_zero_from_le, 0.0);
  EXPECT_TRUE(std::signbit(neg_zero_from_le));

  const std::array<unsigned char, 8> one_be = {0x3F, 0xF0, 0, 0, 0, 0, 0, 0};
  const std::array<unsigned char, 8> one_le = {0, 0, 0, 0, 0, 0, 0xF0, 0x3F};
  EXPECT_DOUBLE_EQ(
      ByteOrderValues::getDouble(one_be.data(), ByteOrderValues::ENDIAN_BIG),
      1.0);
  EXPECT_DOUBLE_EQ(
      ByteOrderValues::getDouble(one_le.data(), ByteOrderValues::ENDIAN_LITTLE),
      1.0);

  const std::array<unsigned char, 8> neg_two_half_be = {0xC0, 0x04, 0, 0,
                                                        0,    0,    0, 0};
  const std::array<unsigned char, 8> neg_two_half_le = {0, 0, 0,    0,
                                                        0, 0, 0x04, 0xC0};
  EXPECT_DOUBLE_EQ(ByteOrderValues::getDouble(neg_two_half_be.data(),
                                              ByteOrderValues::ENDIAN_BIG),
                   -2.5);
  EXPECT_DOUBLE_EQ(ByteOrderValues::getDouble(neg_two_half_le.data(),
                                              ByteOrderValues::ENDIAN_LITTLE),
                   -2.5);

  const std::array<unsigned char, 8> pos_inf_be = {0x7F, 0xF0, 0, 0,
                                                   0,    0,    0, 0};
  const double pos_inf = ByteOrderValues::getDouble(
      pos_inf_be.data(), ByteOrderValues::ENDIAN_BIG);
  EXPECT_TRUE(std::isinf(pos_inf));
  EXPECT_FALSE(std::signbit(pos_inf));

  const std::array<unsigned char, 8> neg_inf_le = {0, 0, 0,    0,
                                                   0, 0, 0xF0, 0xFF};
  const double neg_inf = ByteOrderValues::getDouble(
      neg_inf_le.data(), ByteOrderValues::ENDIAN_LITTLE);
  EXPECT_TRUE(std::isinf(neg_inf));
  EXPECT_TRUE(std::signbit(neg_inf));

  const std::array<unsigned char, 8> qnan_be = {0x7F, 0xF8, 0, 0, 0, 0, 0, 0};
  EXPECT_TRUE(std::isnan(
      ByteOrderValues::getDouble(qnan_be.data(), ByteOrderValues::ENDIAN_BIG)));
}

TEST(ByteOrderValuesTest, PutDoubleExactBytesAndBitExactRoundTrip) {
  static_assert(std::numeric_limits<double>::is_iec559);
  static_assert(sizeof(double) == 8);

  {
    std::array<unsigned char, 10> be_buf = {0xAA, 0xCC, 0xCC, 0xCC, 0xCC,
                                            0xCC, 0xCC, 0xCC, 0xCC, 0x55};
    ByteOrderValues::putDouble(-2.5, be_buf.data() + 1,
                               ByteOrderValues::ENDIAN_BIG);
    EXPECT_EQ(be_buf[0], 0xAA);
    EXPECT_EQ(be_buf[9], 0x55);
    EXPECT_EQ((std::array<unsigned char, 8>{be_buf[1], be_buf[2], be_buf[3],
                                            be_buf[4], be_buf[5], be_buf[6],
                                            be_buf[7], be_buf[8]}),
              (std::array<unsigned char, 8>{0xC0, 0x04, 0, 0, 0, 0, 0, 0}));

    std::array<unsigned char, 10> le_buf = {0xAA, 0xCC, 0xCC, 0xCC, 0xCC,
                                            0xCC, 0xCC, 0xCC, 0xCC, 0x55};
    ByteOrderValues::putDouble(-2.5, le_buf.data() + 1,
                               ByteOrderValues::ENDIAN_LITTLE);
    EXPECT_EQ(le_buf[0], 0xAA);
    EXPECT_EQ(le_buf[9], 0x55);
    EXPECT_EQ((std::array<unsigned char, 8>{le_buf[1], le_buf[2], le_buf[3],
                                            le_buf[4], le_buf[5], le_buf[6],
                                            le_buf[7], le_buf[8]}),
              (std::array<unsigned char, 8>{0, 0, 0, 0, 0, 0, 0x04, 0xC0}));
  }

  const std::array<double, 10> values = {
      0.0,
      -0.0,
      std::numbers::pi,
      -std::numbers::e,
      std::numeric_limits<double>::min(),
      std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
  };

  for (const double value : values) {
    const uint64_t expected_bits = std::bit_cast<uint64_t>(value);
    for (const int order :
         {ByteOrderValues::ENDIAN_BIG, ByteOrderValues::ENDIAN_LITTLE}) {
      std::array<unsigned char, 8> buf{};
      ByteOrderValues::putDouble(value, buf.data(), order);
      const double decoded = ByteOrderValues::getDouble(buf.data(), order);
      EXPECT_EQ(std::bit_cast<uint64_t>(decoded), expected_bits);
    }
  }

  const double qnan = std::numeric_limits<double>::quiet_NaN();
  for (const int order :
       {ByteOrderValues::ENDIAN_BIG, ByteOrderValues::ENDIAN_LITTLE}) {
    std::array<unsigned char, 8> buf{};
    ByteOrderValues::putDouble(qnan, buf.data(), order);
    const double decoded = ByteOrderValues::getDouble(buf.data(), order);
    EXPECT_TRUE(std::isnan(decoded));
    EXPECT_EQ(std::bit_cast<uint64_t>(decoded), std::bit_cast<uint64_t>(qnan));
  }
}

TEST(ByteOrderValuesTest, BigAndLittleEndianBytesAreReversed) {
  {
    std::array<unsigned char, 4> be{};
    std::array<unsigned char, 4> le{};
    ByteOrderValues::putInt(0x12345678, be.data(), ByteOrderValues::ENDIAN_BIG);
    ByteOrderValues::putInt(0x12345678, le.data(),
                            ByteOrderValues::ENDIAN_LITTLE);
    for (std::size_t i = 0; i < 4; ++i) {
      EXPECT_EQ(be[i], le[3 - i]);
    }
  }

  {
    std::array<unsigned char, 4> be{};
    std::array<unsigned char, 4> le{};
    ByteOrderValues::putUnsigned(0x89ABCDEFu, be.data(),
                                 ByteOrderValues::ENDIAN_BIG);
    ByteOrderValues::putUnsigned(0x89ABCDEFu, le.data(),
                                 ByteOrderValues::ENDIAN_LITTLE);
    for (std::size_t i = 0; i < 4; ++i) {
      EXPECT_EQ(be[i], le[3 - i]);
    }
  }

  {
    std::array<unsigned char, 8> be{};
    std::array<unsigned char, 8> le{};
    ByteOrderValues::putLong(0x0123456789ABCDEFLL, be.data(),
                             ByteOrderValues::ENDIAN_BIG);
    ByteOrderValues::putLong(0x0123456789ABCDEFLL, le.data(),
                             ByteOrderValues::ENDIAN_LITTLE);
    for (std::size_t i = 0; i < 8; ++i) {
      EXPECT_EQ(be[i], le[7 - i]);
    }
  }

  {
    std::array<unsigned char, 8> be{};
    std::array<unsigned char, 8> le{};
    ByteOrderValues::putDouble(123456.789012345, be.data(),
                               ByteOrderValues::ENDIAN_BIG);
    ByteOrderValues::putDouble(123456.789012345, le.data(),
                               ByteOrderValues::ENDIAN_LITTLE);
    for (std::size_t i = 0; i < 8; ++i) {
      EXPECT_EQ(be[i], le[7 - i]);
    }
  }
}

}  // namespace
}  // namespace geos_google
