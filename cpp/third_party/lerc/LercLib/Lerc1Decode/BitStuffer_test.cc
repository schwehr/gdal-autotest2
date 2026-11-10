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
// Unit tests for LercNS::BitStuffer
// (third_party/lerc/LercLib/Lerc1Decode/BitStuffer.cpp).
//
// Note: Lerc1Decode::BitStuffer::read() has no bounds parameter; never pass
// truncated payloads directly to it. Truncated streams are tested through
// CntZImage::read() / Lerc::Decode().
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Lerc1Decode/BitStuffer.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Lerc1Decode/BitStuffer.cpp

#include "third_party/lerc/LercLib/Lerc1Decode/BitStuffer.h"

#include <array>
#include <cstddef>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/lerc/LercLib/Defines.h"
#include "third_party/lerc/google/util/lerc_private_test_util.h"
#include "third_party/lerc/google/util/lerc_test_util.h"

#ifndef HAVE_LERC1_DECODE
#error "Lerc1Decode tests require HAVE_LERC1_DECODE"
#endif

namespace lerc {
namespace {

using ::LercNS::BitStuffer;
using ::LercNS::Byte;
using ::testing::Each;
using ::testing::ElementsAreArray;

class TestableBitStuffer final : public BitStuffer {
 public:
  using BitStuffer::numTailBytesNotNeeded;
  using BitStuffer::readUInt;
};

TEST(BitStufferTest, RoundTripZeroBitsAndMultipleBitWidthsAndTailSizes) {
  const BitStuffer decoder;
  constexpr unsigned int kSentinel = 0xDEADBEEFU;

  // 1. numBits == 0 (all zeros -> header only, no payload words).
  // BitStuffer::read() calls dataVec.resize(n, 0) without clearing existing
  // entries, so an empty vector zero-fills whereas a pre-sized vector keeps
  // stale sentinels (highlighting the upstream m_tmpDataVec reuse hazard).
  {
    const std::vector<unsigned int> zeros(7, 0);
    AlignedBitStuffer2Stream encoded;
    ASSERT_TRUE(EncodeSimpleV2Aligned(zeros, encoded));

    const Byte* ptr = encoded.data();
    std::vector<unsigned int> decoded;
    ASSERT_TRUE(decoder.read(&ptr, decoded));
    EXPECT_EQ(encoded.data() + encoded.byte_length, ptr);
    EXPECT_THAT(decoded, ElementsAreArray(zeros));

    const Byte* stale_ptr = encoded.data();
    std::vector<unsigned int> prefilled(zeros.size(), kSentinel);
    ASSERT_TRUE(decoder.read(&stale_ptr, prefilled));
    EXPECT_EQ(encoded.data() + encoded.byte_length, stale_ptr);
    EXPECT_THAT(prefilled, Each(kSentinel));
  }

  // 2. Odd & even bit widths exercising numTailBytesNotNeeded (0, 1, 2, 3),
  // exact 32-bit word wraps (bitPos == 32), and cross-word splits.
  const std::vector<std::vector<unsigned int>> inputs = {
      {1},                             // 1 bit, 1 elem -> 3 tail bytes unused
      {0, 1, 1, 0, 1},                 // 1 bit, 5 elems -> 3 tail bytes unused
      {1, 19, 3, 31, 0},               // 5 bits, 5 elems -> 0 tail bytes unused
      {1, 19, 3},                      // 5 bits, 3 elems -> 2 tail bytes unused
      {1, 19, 3, 27},                  // 5 bits, 4 elems -> 1 tail byte unused
      {0, 127, 255, 1, 42, 99, 7, 8},  // 8 bits, 8 elems -> bitPos == 32 exact
      {13, 4095, 8191, 1024, 7, 5555, 6000, 123, 8190, 2,
       3333},                    // 13 bits cross
      {0, 65535, 32768, 12345},  // 16 bits, 4 elems -> bitPos == 32 exact
      {0, 0x7FFFFFFFU, 1U, 0x40000001U, 0x12345678U},  // 31 bits max supported
  };

  for (const auto& expected : inputs) {
    AlignedBitStuffer2Stream encoded;
    ASSERT_TRUE(EncodeSimpleV2Aligned(expected, encoded));
    const Byte* ptr = encoded.data();
    std::vector<unsigned int> decoded(100, kSentinel);
    ASSERT_TRUE(decoder.read(&ptr, decoded));
    EXPECT_EQ(encoded.data() + encoded.byte_length, ptr);
    EXPECT_EQ(expected.size(), decoded.size());
    EXPECT_THAT(decoded, ElementsAreArray(expected));
  }
}

TEST(BitStufferTest, ReadHandlesOneTwoAndFourByteElementCounts) {
  const BitStuffer decoder;
  constexpr unsigned int kSentinel = 0xDEADBEEFU;

  // 2-byte numElements encoding (256..65535).
  {
    std::vector<unsigned int> values(300);
    for (std::size_t i = 0; i < values.size(); ++i) {
      values[i] = static_cast<unsigned int>((i * 17) & 63);
    }
    AlignedBitStuffer2Stream encoded;
    ASSERT_TRUE(EncodeSimpleV2Aligned(values, encoded));
    const Byte* ptr = encoded.data();
    std::vector<unsigned int> decoded(100, kSentinel);
    ASSERT_TRUE(decoder.read(&ptr, decoded));
    EXPECT_EQ(encoded.data() + encoded.byte_length, ptr);
    EXPECT_EQ(values.size(), decoded.size());
    EXPECT_THAT(decoded, ElementsAreArray(values));
  }

  // 4-byte numElements encoding (>= 65536). Use 1-bit values (~8 KB).
  {
    std::vector<unsigned int> values(65536, 0);
    for (std::size_t i = 0; i < values.size(); i += 3) {
      values[i] = 1;
    }
    AlignedBitStuffer2Stream encoded;
    ASSERT_TRUE(EncodeSimpleV2Aligned(values, encoded));
    const Byte* ptr = encoded.data();
    std::vector<unsigned int> decoded(100, kSentinel);
    ASSERT_TRUE(decoder.read(&ptr, decoded));
    EXPECT_EQ(encoded.data() + encoded.byte_length, ptr);
    EXPECT_EQ(values.size(), decoded.size());
    EXPECT_THAT(decoded, ElementsAreArray(values));
  }
}

TEST(BitStufferTest, ErrorGuardsAndProtectedHelpers) {
  const BitStuffer decoder;
  std::vector<unsigned int> out;

  // 1. Null cursor pointer and invalid bits67 == 3 (byte width 0 -> readUInt
  //    returns false without dereferencing payload).
  EXPECT_FALSE(decoder.read(nullptr, out));

  constexpr std::array<Byte, 1> kInvalidWidth = {
      static_cast<Byte>(kBitStufferInvalidCount | 1U)};
  const Byte* ptr = kInvalidWidth.data();
  EXPECT_FALSE(decoder.read(&ptr, out));

  // 2. numBits >= 32 in header fails before touching payload.
  // Header byte: bits 6..7 = 2 (1-byte element count), numBits = 32.
  constexpr std::array<Byte, 2> kBadNumBitsHeader = {
      static_cast<Byte>(kBitStufferOneByteCount | 32U), 4};
  ptr = kBadNumBitsHeader.data();
  EXPECT_FALSE(decoder.read(&ptr, out));

  // 3. Protected readUInt invalid width and numTailBytesNotNeeded table checks.
  constexpr std::array<Byte, 4> kDummy = {1, 2, 3, 4};
  const Byte* uptr = kDummy.data();
  unsigned int k = 0;
  EXPECT_FALSE(TestableBitStuffer::readUInt(&uptr, k, 3));
  EXPECT_EQ(kDummy.data(), uptr);

  // Header bits67 maps n = (bits67 == 0) ? 4 : 3 - bits67 (0->4B, 1->2B,
  // 2->1B, 3->0B invalid); readUInt accepts widths 1, 2, and 4 only.
  EXPECT_TRUE(TestableBitStuffer::readUInt(&uptr, k, 1));
  EXPECT_EQ(1U, k);
  EXPECT_TRUE(TestableBitStuffer::readUInt(&uptr, k, 2));
  EXPECT_EQ(0x0302U, k);

  EXPECT_EQ(0U, TestableBitStuffer::numTailBytesNotNeeded(4, 8));
  EXPECT_EQ(3U, TestableBitStuffer::numTailBytesNotNeeded(1, 5));
  EXPECT_EQ(2U, TestableBitStuffer::numTailBytesNotNeeded(3, 5));
  EXPECT_EQ(1U, TestableBitStuffer::numTailBytesNotNeeded(4, 5));
}

}  // namespace
}  // namespace lerc
