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

#include "third_party/geos/include/geos/io/ByteOrderDataInStream.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/io/ByteOrderValues.h"
#include "third_party/geos/include/geos/io/ParseException.h"
#include "third_party/geos/include/geos/util/Machine.h"

namespace geos_google {
namespace {

using geos::io::ByteOrderDataInStream;
using geos::io::ByteOrderValues;
using geos::io::ParseException;

template <typename Fn>
void ExpectUnexpectedEof(Fn&& fn) {
  try {
    static_cast<void>(fn());
    FAIL() << "Expected geos::io::ParseException";
  } catch (const ParseException& e) {
    EXPECT_NE(std::string_view(e.what()).find("Unexpected EOF"),
              std::string_view::npos)
        << "Unexpected ParseException message: " << e.what();
  }
}

TEST(ByteOrderDataInStreamTest, DefaultConstructorIsEmptyAndThrowsOnAllReads) {
  ByteOrderDataInStream stream;
  EXPECT_EQ(stream.size(), std::size_t{0});

  ExpectUnexpectedEof([&]() { return stream.readByte(); });
  ExpectUnexpectedEof([&]() { return stream.readInt(); });
  ExpectUnexpectedEof([&]() { return stream.readUnsigned(); });
  ExpectUnexpectedEof([&]() { return stream.readLong(); });
  ExpectUnexpectedEof([&]() { return stream.readDouble(); });
  EXPECT_EQ(stream.size(), std::size_t{0});
}

TEST(ByteOrderDataInStreamTest, NullBufferWithZeroSizeThrowsOnReads) {
  ByteOrderDataInStream stream(nullptr, 0);
  EXPECT_EQ(stream.size(), std::size_t{0});

  ExpectUnexpectedEof([&]() { return stream.readByte(); });
  ExpectUnexpectedEof([&]() { return stream.readInt(); });
  ExpectUnexpectedEof([&]() { return stream.readUnsigned(); });
  ExpectUnexpectedEof([&]() { return stream.readLong(); });
  ExpectUnexpectedEof([&]() { return stream.readDouble(); });
}

TEST(ByteOrderDataInStreamTest, DefaultByteOrderMatchesMachineByteOrder) {
  const int machine_order = getMachineByteOrder();
  ASSERT_TRUE(machine_order == ByteOrderValues::ENDIAN_BIG ||
              machine_order == ByteOrderValues::ENDIAN_LITTLE);

  std::array<unsigned char, 12> buf{};
  ByteOrderValues::putInt(0x12345678, buf.data(), machine_order);
  ByteOrderValues::putDouble(-42.625, buf.data() + 4, machine_order);

  ByteOrderDataInStream stream(buf.data(), buf.size());
  EXPECT_EQ(stream.size(), std::size_t{12});
  EXPECT_EQ(stream.readInt(), 0x12345678);
  EXPECT_EQ(stream.size(), std::size_t{8});
  EXPECT_DOUBLE_EQ(stream.readDouble(), -42.625);
  EXPECT_EQ(stream.size(), std::size_t{0});
}

TEST(ByteOrderDataInStreamTest, ReadByteValuesAndExhaustion) {
  const std::array<unsigned char, 4> buf = {0x00, 0x7F, 0x80, 0xFF};
  ByteOrderDataInStream stream(buf.data(), buf.size());

  EXPECT_EQ(stream.size(), std::size_t{4});
  EXPECT_EQ(stream.readByte(), static_cast<unsigned char>(0x00));
  EXPECT_EQ(stream.size(), std::size_t{3});
  EXPECT_EQ(stream.readByte(), static_cast<unsigned char>(0x7F));
  EXPECT_EQ(stream.size(), std::size_t{2});
  EXPECT_EQ(stream.readByte(), static_cast<unsigned char>(0x80));
  EXPECT_EQ(stream.size(), std::size_t{1});
  EXPECT_EQ(stream.readByte(), static_cast<unsigned char>(0xFF));
  EXPECT_EQ(stream.size(), std::size_t{0});

  ExpectUnexpectedEof([&]() { return stream.readByte(); });
  EXPECT_EQ(stream.size(), std::size_t{0});
}

TEST(ByteOrderDataInStreamTest, ReadIntBigAndLittleEndian) {
  const std::array<unsigned char, 20> big_buf = {
      0x00, 0x00, 0x00, 0x00,  // 0
      0x01, 0x02, 0x03, 0x04,  // 0x01020304
      0xFF, 0xFF, 0xFF, 0xFF,  // -1
      0x7F, 0xFF, 0xFF, 0xFF,  // INT32_MAX
      0x80, 0x00, 0x00, 0x00,  // INT32_MIN
  };
  ByteOrderDataInStream big_stream(big_buf.data(), big_buf.size());
  big_stream.setOrder(ByteOrderValues::ENDIAN_BIG);

  EXPECT_EQ(big_stream.readInt(), 0);
  EXPECT_EQ(big_stream.readInt(), 0x01020304);
  EXPECT_EQ(big_stream.readInt(), -1);
  EXPECT_EQ(big_stream.readInt(), std::numeric_limits<int32_t>::max());
  EXPECT_EQ(big_stream.readInt(), std::numeric_limits<int32_t>::min());
  EXPECT_EQ(big_stream.size(), std::size_t{0});

  const std::array<unsigned char, 20> little_buf = {
      0x00, 0x00, 0x00, 0x00,  // 0
      0x04, 0x03, 0x02, 0x01,  // 0x01020304
      0xFF, 0xFF, 0xFF, 0xFF,  // -1
      0xFF, 0xFF, 0xFF, 0x7F,  // INT32_MAX
      0x00, 0x00, 0x00, 0x80,  // INT32_MIN
  };
  ByteOrderDataInStream little_stream(little_buf.data(), little_buf.size());
  little_stream.setOrder(ByteOrderValues::ENDIAN_LITTLE);

  EXPECT_EQ(little_stream.readInt(), 0);
  EXPECT_EQ(little_stream.readInt(), 0x01020304);
  EXPECT_EQ(little_stream.readInt(), -1);
  EXPECT_EQ(little_stream.readInt(), std::numeric_limits<int32_t>::max());
  EXPECT_EQ(little_stream.readInt(), std::numeric_limits<int32_t>::min());
  EXPECT_EQ(little_stream.size(), std::size_t{0});
}

TEST(ByteOrderDataInStreamTest, ReadUnsignedBigAndLittleEndian) {
  const std::array<unsigned char, 20> big_buf = {
      0x00, 0x00, 0x00, 0x00,  // 0
      0x01, 0x02, 0x03, 0x04,  // 0x01020304
      0x80, 0x00, 0x00, 0x00,  // 0x80000000
      0xDE, 0xAD, 0xBE, 0xEF,  // 0xDEADBEEF
      0xFF, 0xFF, 0xFF, 0xFF,  // UINT32_MAX
  };
  ByteOrderDataInStream big_stream(big_buf.data(), big_buf.size());
  big_stream.setOrder(ByteOrderValues::ENDIAN_BIG);

  EXPECT_EQ(big_stream.readUnsigned(), 0u);
  EXPECT_EQ(big_stream.readUnsigned(), 0x01020304u);
  EXPECT_EQ(big_stream.readUnsigned(), 0x80000000u);
  EXPECT_EQ(big_stream.readUnsigned(), 0xDEADBEEFu);
  EXPECT_EQ(big_stream.readUnsigned(), std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(big_stream.size(), std::size_t{0});

  const std::array<unsigned char, 20> little_buf = {
      0x00, 0x00, 0x00, 0x00,  // 0
      0x04, 0x03, 0x02, 0x01,  // 0x01020304
      0x00, 0x00, 0x00, 0x80,  // 0x80000000
      0xEF, 0xBE, 0xAD, 0xDE,  // 0xDEADBEEF
      0xFF, 0xFF, 0xFF, 0xFF,  // UINT32_MAX
  };
  ByteOrderDataInStream little_stream(little_buf.data(), little_buf.size());
  little_stream.setOrder(ByteOrderValues::ENDIAN_LITTLE);

  EXPECT_EQ(little_stream.readUnsigned(), 0u);
  EXPECT_EQ(little_stream.readUnsigned(), 0x01020304u);
  EXPECT_EQ(little_stream.readUnsigned(), 0x80000000u);
  EXPECT_EQ(little_stream.readUnsigned(), 0xDEADBEEFu);
  EXPECT_EQ(little_stream.readUnsigned(), std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(little_stream.size(), std::size_t{0});
}

TEST(ByteOrderDataInStreamTest, ReadLongBigAndLittleEndian) {
  const std::array<unsigned char, 40> big_buf = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // 0x0102030405060708
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // -1
      0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // INT64_MAX
      0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // INT64_MIN
  };
  ByteOrderDataInStream big_stream(big_buf.data(), big_buf.size());
  big_stream.setOrder(ByteOrderValues::ENDIAN_BIG);

  EXPECT_EQ(big_stream.readLong(), int64_t{0});
  EXPECT_EQ(big_stream.readLong(), int64_t{0x0102030405060708LL});
  EXPECT_EQ(big_stream.readLong(), int64_t{-1});
  EXPECT_EQ(big_stream.readLong(), std::numeric_limits<int64_t>::max());
  EXPECT_EQ(big_stream.readLong(), std::numeric_limits<int64_t>::min());
  EXPECT_EQ(big_stream.size(), std::size_t{0});

  const std::array<unsigned char, 40> little_buf = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // 0x0102030405060708
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // -1
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F,  // INT64_MAX
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,  // INT64_MIN
  };
  ByteOrderDataInStream little_stream(little_buf.data(), little_buf.size());
  little_stream.setOrder(ByteOrderValues::ENDIAN_LITTLE);

  EXPECT_EQ(little_stream.readLong(), int64_t{0});
  EXPECT_EQ(little_stream.readLong(), int64_t{0x0102030405060708LL});
  EXPECT_EQ(little_stream.readLong(), int64_t{-1});
  EXPECT_EQ(little_stream.readLong(), std::numeric_limits<int64_t>::max());
  EXPECT_EQ(little_stream.readLong(), std::numeric_limits<int64_t>::min());
  EXPECT_EQ(little_stream.size(), std::size_t{0});
}

TEST(ByteOrderDataInStreamTest, ReadDoubleBigAndLittleEndianAndSpecialValues) {
  static_assert(sizeof(double) == 8);
  const std::array<unsigned char, 56> big_buf = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // +0.0
      0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // -0.0
      0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 1.0
      0xC0, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // -2.5
      0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // +inf
      0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // -inf
      0x7F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // qNaN
  };
  ByteOrderDataInStream big_stream(big_buf.data(), big_buf.size());
  big_stream.setOrder(ByteOrderValues::ENDIAN_BIG);

  const double pos_zero = big_stream.readDouble();
  EXPECT_EQ(pos_zero, 0.0);
  EXPECT_FALSE(std::signbit(pos_zero));

  const double neg_zero = big_stream.readDouble();
  EXPECT_EQ(neg_zero, 0.0);
  EXPECT_TRUE(std::signbit(neg_zero));

  EXPECT_DOUBLE_EQ(big_stream.readDouble(), 1.0);
  EXPECT_DOUBLE_EQ(big_stream.readDouble(), -2.5);

  const double pos_inf = big_stream.readDouble();
  EXPECT_TRUE(std::isinf(pos_inf));
  EXPECT_FALSE(std::signbit(pos_inf));

  const double neg_inf = big_stream.readDouble();
  EXPECT_TRUE(std::isinf(neg_inf));
  EXPECT_TRUE(std::signbit(neg_inf));

  const double nan_val = big_stream.readDouble();
  EXPECT_TRUE(std::isnan(nan_val));
  EXPECT_EQ(big_stream.size(), std::size_t{0});

  std::array<unsigned char, 16> little_buf{};
  ByteOrderValues::putDouble(std::numbers::pi, little_buf.data(),
                             ByteOrderValues::ENDIAN_LITTLE);
  ByteOrderValues::putDouble(-0.0, little_buf.data() + 8,
                             ByteOrderValues::ENDIAN_LITTLE);
  ByteOrderDataInStream little_stream(little_buf.data(), little_buf.size());
  little_stream.setOrder(ByteOrderValues::ENDIAN_LITTLE);

  EXPECT_EQ(std::bit_cast<uint64_t>(little_stream.readDouble()),
            std::bit_cast<uint64_t>(std::numbers::pi));
  const double little_neg_zero = little_stream.readDouble();
  EXPECT_EQ(little_neg_zero, 0.0);
  EXPECT_TRUE(std::signbit(little_neg_zero));
  EXPECT_EQ(little_stream.size(), std::size_t{0});
}

TEST(ByteOrderDataInStreamTest, InsufficientBytesThrowsWithoutAdvancingStream) {
  for (std::size_t len = 0; len < 4; ++len) {
    const std::vector<unsigned char> buf(len, 0x42);
    ByteOrderDataInStream stream(buf.data(), buf.size());

    ExpectUnexpectedEof([&]() { return stream.readInt(); });
    EXPECT_EQ(stream.size(), len);

    ExpectUnexpectedEof([&]() { return stream.readUnsigned(); });
    EXPECT_EQ(stream.size(), len);

    for (std::size_t i = 0; i < len; ++i) {
      EXPECT_EQ(stream.readByte(), static_cast<unsigned char>(0x42));
    }
    EXPECT_EQ(stream.size(), std::size_t{0});
  }

  for (std::size_t len :
       {std::size_t{0}, std::size_t{1}, std::size_t{4}, std::size_t{7}}) {
    const std::vector<unsigned char> buf(len, 0x5A);
    ByteOrderDataInStream stream(buf.data(), buf.size());

    ExpectUnexpectedEof([&]() { return stream.readLong(); });
    EXPECT_EQ(stream.size(), len);

    ExpectUnexpectedEof([&]() { return stream.readDouble(); });
    EXPECT_EQ(stream.size(), len);

    if (len >= 4) {
      EXPECT_EQ(stream.readInt(), 0x5A5A5A5A);
      EXPECT_EQ(stream.size(), len - 4);
    }
  }
}

TEST(ByteOrderDataInStreamTest, SwitchByteOrderMidStreamAndMixedReads) {
  std::array<unsigned char, 25> buf{};
  buf[0] = 0x01;
  ByteOrderValues::putUnsigned(0x01020304u, buf.data() + 1,
                               ByteOrderValues::ENDIAN_BIG);
  ByteOrderValues::putInt(-123456, buf.data() + 5,
                          ByteOrderValues::ENDIAN_LITTLE);
  ByteOrderValues::putLong(0x1122334455667788LL, buf.data() + 9,
                           ByteOrderValues::ENDIAN_BIG);
  ByteOrderValues::putDouble(-9876.54321, buf.data() + 17,
                             ByteOrderValues::ENDIAN_LITTLE);

  ByteOrderDataInStream stream(buf.data(), buf.size());
  EXPECT_EQ(stream.size(), std::size_t{25});

  EXPECT_EQ(stream.readByte(), static_cast<unsigned char>(0x01));
  EXPECT_EQ(stream.size(), std::size_t{24});

  stream.setOrder(ByteOrderValues::ENDIAN_BIG);
  EXPECT_EQ(stream.readUnsigned(), 0x01020304u);
  EXPECT_EQ(stream.size(), std::size_t{20});

  stream.setOrder(ByteOrderValues::ENDIAN_LITTLE);
  EXPECT_EQ(stream.readInt(), -123456);
  EXPECT_EQ(stream.size(), std::size_t{16});

  stream.setOrder(ByteOrderValues::ENDIAN_BIG);
  EXPECT_EQ(stream.readLong(), int64_t{0x1122334455667788LL});
  EXPECT_EQ(stream.size(), std::size_t{8});

  stream.setOrder(ByteOrderValues::ENDIAN_LITTLE);
  EXPECT_DOUBLE_EQ(stream.readDouble(), -9876.54321);
  EXPECT_EQ(stream.size(), std::size_t{0});

  ExpectUnexpectedEof([&]() { return stream.readByte(); });
}

}  // namespace
}  // namespace geos_google
