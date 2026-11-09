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
// Tests internal bit-vector manipulation utilities: copy, shift, signed/unsigned
// integer extraction/insertion (LE and BE paths), set/clear, LSB/MSB search,
// increment, decrement, and bitwise negation (third_party/hdf5/src/H5Tbit.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tbit.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5TBIT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5T_FRIEND
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
using H5__Float16 = uint16_t;
#endif
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5TBIT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TBIT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

class H5TbitTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_GE(H5open(), 0); }
};

TEST_F(H5TbitTest, BitCopyCoversAlignmentWrapAndMiddlePaths) {
  // Zero size is a no-op.
  {
    std::array<uint8_t, 2> dst = {0xAA, 0x55};
    const std::array<uint8_t, 2> src = {0xFF, 0x00};
    H5T__bit_copy(dst.data(), 3, src.data(), 5, 0);
    EXPECT_THAT(dst, ElementsAre(0xAA, 0x55));
  }

  // Aligned copy: src_offset % 8 == 0, dst_offset % 8 == 0, middle fast-path.
  {
    const std::array<uint8_t, 4> src = {0x12, 0x34, 0x56, 0x78};
    std::array<uint8_t, 4> dst = {0xFF, 0xFF, 0xFF, 0xFF};
    H5T__bit_copy(dst.data(), 8, src.data(), 8, 19);
    // Bits [8..26] copied: byte 1 = 0x34, byte 2 = 0x56, bits [0..2] of byte 3 = 0x78 & 0x07.
    EXPECT_EQ(dst[0], 0xFF);
    EXPECT_EQ(dst[1], 0x34);
    EXPECT_EQ(dst[2], 0x56);
    EXPECT_EQ(dst[3], (0xFF & ~0x07) | (0x78 & 0x07));
  }

  // Case A in leading loop: src_offset wraps >= 8 while dst_offset stays < 8.
  {
    const std::array<uint8_t, 4> src = {0xE0, 0xAB, 0xCD, 0x00};
    std::array<uint8_t, 4> dst = {0x01, 0x00, 0x00, 0x00};
    // src_offset=5, dst_offset=1, size=10:
    //   lead: nbits = min(10, 7, 3) = 3 -> src_offset=8 (wraps), dst_offset=4 (<8), size=7
    //   tail: src_offset=0, dst_offset=4, size=7 -> dst wraps in iter 1 (4+4=8), iter 2 src=7, dst=3
    H5T__bit_copy(dst.data(), 1, src.data(), 5, 10);
    EXPECT_EQ(H5T__bit_get_d(dst.data(), 1, 10),
              H5T__bit_get_d(src.data(), 5, 10));
    EXPECT_EQ(dst[0] & 0x01, 0x01);  // Preserves bit 0.
  }

  // Case B in leading loop: dst_offset wraps >= 8 while src_offset stays < 8,
  // followed by shifted middle bytes (> 8 bits remaining) and partial tail.
  {
    const std::array<uint8_t, 5> src = {0xAA, 0x55, 0x3C, 0xF0, 0x0F};
    std::array<uint8_t, 5> dst{};
    dst.fill(0xA5);
    // src_offset=2, dst_offset=6, size=22:
    //   lead iter 1: nbits=2 -> src_offset=4 (<8), dst_offset=8 (wraps)
    //   lead iter 2: nbits=4 -> src_offset=8 (wraps), dst_offset=4 (<8)
    //   middle loop: size=16 (>8), shift=4
    //   tail loop: size=8 -> iter 1: nbits=min(8,4,8)=4 (dst wraps), iter 2: nbits=4
    H5T__bit_copy(dst.data(), 6, src.data(), 2, 22);
    EXPECT_EQ(H5T__bit_get_d(dst.data(), 6, 22),
              H5T__bit_get_d(src.data(), 2, 22));
    // Verify bits outside [6, 28) retained original 0xA5 pattern.
    EXPECT_EQ(dst[0] & 0x3F, 0xA5 & 0x3F);
    EXPECT_EQ(dst[3] >> 4, 0xA5 >> 4);
  }

  // Size == 8 with unaligned dst_offset=3, src_offset=0 skips middle loop (size > 8 is false)
  // and executes tail loop where BOTH src_offset and dst_offset wrap/don't-wrap branches hit.
  {
    const std::array<uint8_t, 2> src = {0xB7, 0x00};
    std::array<uint8_t, 3> dst = {0xFF, 0xFF, 0xFF};
    H5T__bit_copy(dst.data(), 3, src.data(), 0, 8);
    EXPECT_EQ(H5T__bit_get_d(dst.data(), 3, 8), 0xB7);
    EXPECT_EQ(dst[0] & 0x07, 0x07);
    EXPECT_EQ(dst[1] >> 3, 0xFF >> 3);
  }
}

TEST_F(H5TbitTest, BitShiftZeroFullLeftRightAndHeapBuffer) {
  // Zero shift leaves bits intact.
  {
    std::array<uint8_t, 3> buf = {0x12, 0x34, 0x56};
    EXPECT_GE(H5T__bit_shift(buf.data(), 0, 4, 16), 0);
    EXPECT_THAT(buf, ElementsAre(0x12, 0x34, 0x56));
  }

  // Shift >= size (both positive and negative) clears only the target field.
  {
    std::array<uint8_t, 2> buf = {0xFF, 0xFF};
    EXPECT_GE(H5T__bit_shift(buf.data(), 10, 3, 10), 0);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 0, 3), 0x07);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 3, 10), 0);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 13, 3), 0x07);

    buf = {0xFF, 0xFF};
    EXPECT_GE(H5T__bit_shift(buf.data(), -10, 3, 10), 0);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 0, 3), 0x07);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 3, 10), 0);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 13, 3), 0x07);
  }

  // Small left shift and right shift.
  {
    std::array<uint8_t, 4> buf = {0xFF, 0xFF, 0xFF, 0xFF};
    const uint64_t initial = 0x1A2B3ULL;
    H5T__bit_set_d(buf.data(), 5, 20, initial);
    EXPECT_GE(H5T__bit_shift(buf.data(), 4, 5, 20), 0);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 5, 20), (initial << 4) & 0xFFFFFULL);
    // Preserves outside bits.
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 0, 5), 0x1FULL);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 25, 7), 0x7FULL);

    H5T__bit_set_d(buf.data(), 5, 20, initial);
    EXPECT_GE(H5T__bit_shift(buf.data(), -4, 5, 20), 0);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 5, 20), initial >> 4);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 0, 5), 0x1FULL);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 25, 7), 0x7FULL);
  }

  // Large vector (> 512 bytes) exercises heap buffer allocation in H5WB_actual.
  {
    constexpr size_t kBits = 600 * 8;
    std::vector<uint8_t> buf(602, 0);
    H5T__bit_set_d(buf.data(), 4, 32, 0xDEADBEEFULL);
    EXPECT_GE(H5T__bit_shift(buf.data(), 13, 4, kBits), 0);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 4, 13), 0);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 17, 32), 0xDEADBEEFULL);

    EXPECT_GE(H5T__bit_shift(buf.data(), -13, 4, kBits), 0);
    EXPECT_EQ(H5T__bit_get_d(buf.data(), 4, 32), 0xDEADBEEFULL);
  }
}

TEST_F(H5TbitTest, BitGetAndSetDNativeAndSwappedBigEndian) {
  // Default LE path on Linux.
  std::array<uint8_t, 10> buf{};
  buf.fill(0xFF);
  H5T__bit_set_d(buf.data(), 3, 19, 0x5A3C1ULL);
  EXPECT_EQ(H5T__bit_get_d(buf.data(), 3, 19), 0x5A3C1ULL);
  EXPECT_EQ(H5T__bit_get_d(buf.data(), 3, 0), 0);

  buf.fill(0);
  H5T__bit_set_d(buf.data(), 5, 64, 0x0123456789ABCDEFULL);
  EXPECT_EQ(H5T__bit_get_d(buf.data(), 5, 64), 0x0123456789ABCDEFULL);

  // Temporarily exercise the H5T_ORDER_BE swap branch and restore on scope exit.
  const H5T_order_t saved_order = H5T_native_order_g;
  auto restore_order = absl::MakeCleanup(
      [saved_order]() { H5T_native_order_g = saved_order; });
  H5T_native_order_g = H5T_ORDER_BE;

  std::array<uint8_t, 9> be_buf{};
  const uint64_t pattern = 0x1122334455667788ULL;
  H5T__bit_set_d(be_buf.data(), 0, 64, pattern);
  EXPECT_EQ(be_buf[0], 0x11);
  EXPECT_EQ(be_buf[7], 0x88);
  EXPECT_EQ(H5T__bit_get_d(be_buf.data(), 0, 64), pattern);

  be_buf.fill(0);
  H5T__bit_set_d(be_buf.data(), 4, 64, pattern);
  EXPECT_EQ(H5T__bit_get_d(be_buf.data(), 4, 64), pattern);
}

TEST_F(H5TbitTest, BitSetTrueAndFalsePartialAndAligned) {
  // Size 0 is a no-op.
  std::array<uint8_t, 4> buf = {0x5A, 0x5A, 0x5A, 0x5A};
  H5T__bit_set(buf.data(), 3, 0, true);
  EXPECT_THAT(buf, ElementsAre(0x5A, 0x5A, 0x5A, 0x5A));

  // Unaligned start + middle byte + partial end: set then clear.
  buf.fill(0x00);
  H5T__bit_set(buf.data(), 3, 17, true);  // Bits [3..19] = 1.
  EXPECT_THAT(buf, ElementsAre(0xF8, 0xFF, 0x0F, 0x00));
  H5T__bit_set(buf.data(), 3, 17, false);
  EXPECT_THAT(buf, ElementsAre(0x00, 0x00, 0x00, 0x00));

  // Single-byte unaligned region preserves outer bits.
  buf.fill(0x81);
  H5T__bit_set(buf.data(), 2, 4, true);
  EXPECT_EQ(buf[0], 0xBD);
  H5T__bit_set(buf.data(), 2, 4, false);
  EXPECT_EQ(buf[0], 0x81);

  // Byte-aligned exact byte count skips both partial branches.
  buf.fill(0x00);
  H5T__bit_set(buf.data(), 8, 16, true);
  EXPECT_THAT(buf, ElementsAre(0x00, 0xFF, 0xFF, 0x00));
  H5T__bit_set(buf.data(), 8, 16, false);
  EXPECT_THAT(buf, ElementsAre(0x00, 0x00, 0x00, 0x00));
}

TEST_F(H5TbitTest, BitFindLsbAndMsbAllSectionsAndValues) {
  // Search for 1 bits across leading partial, skipped zero middle, and tail.
  std::array<uint8_t, 5> ones_buf = {0x00, 0x00, 0x00, 0x00, 0x00};

  // Miss across all three regions for value = true.
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 3, 25, H5T_BIT_LSB, true), -1);
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 3, 25, H5T_BIT_MSB, true), -1);
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 4, 0, H5T_BIT_LSB, true), -1);

  // Single bit in leading byte of LSB (offset=3, bit at absolute pos 6 -> rel 3).
  ones_buf = {0x40, 0x00, 0x00, 0x00, 0x00};
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 3, 25, H5T_BIT_LSB, true), 3);
  // Same bit seen from MSB ending section (lines 459-463).
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 3, 25, H5T_BIT_MSB, true), 3);

  // Bit in second middle byte (skipping first middle byte 0x00).
  ones_buf = {0x00, 0x00, 0x20, 0x00, 0x00};
  // Absolute pos = 16 + 5 = 21; offset=3, size=28 -> rel = 18.
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 3, 28, H5T_BIT_LSB, true), 18);
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 3, 28, H5T_BIT_MSB, true), 18);

  // Bit only in final partial byte for LSB (and top partial byte for MSB).
  ones_buf = {0x00, 0x00, 0x00, 0x04, 0x00};
  // Absolute pos = 24 + 2 = 26; offset=3, size=26 -> [3..28], bits in byte 3 are [24..28].
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 3, 26, H5T_BIT_LSB, true), 23);
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 3, 26, H5T_BIT_MSB, true), 23);

  // Aligned range where MSB top partial and bottom partial are skipped (offset=8, size=16).
  ones_buf = {0xFF, 0x00, 0x08, 0xFF, 0x00};
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 8, 16, H5T_BIT_LSB, true), 11);
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 8, 16, H5T_BIT_MSB, true), 11);

  // Short single-byte region for MSB where size <= 8 - (offset % 8).
  ones_buf = {0x1C, 0x00, 0x00, 0x00, 0x00};
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 2, 4, H5T_BIT_MSB, true), 2);
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 2, 2, H5T_BIT_MSB, true), 1);
  ones_buf = {0x04, 0x00, 0x00, 0x00, 0x00};
  EXPECT_EQ(H5T__bit_find(ones_buf.data(), 2, 2, H5T_BIT_MSB, true), 0);

  // Search for 0 bits across leading partial, skipped 0xFF middle, and tail.
  std::array<uint8_t, 5> zeros_buf = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_EQ(H5T__bit_find(zeros_buf.data(), 3, 25, H5T_BIT_LSB, false), -1);
  EXPECT_EQ(H5T__bit_find(zeros_buf.data(), 3, 25, H5T_BIT_MSB, false), -1);

  zeros_buf = {0xDF, 0xFF, 0xFF, 0xFF, 0xFF};  // Bit 5 is 0.
  EXPECT_EQ(H5T__bit_find(zeros_buf.data(), 3, 25, H5T_BIT_LSB, false), 2);
  EXPECT_EQ(H5T__bit_find(zeros_buf.data(), 3, 25, H5T_BIT_MSB, false), 2);

  zeros_buf = {0xFF, 0xEF, 0xFF, 0xFF, 0xFF};  // Bit 12 is 0.
  EXPECT_EQ(H5T__bit_find(zeros_buf.data(), 3, 25, H5T_BIT_LSB, false), 9);
  EXPECT_EQ(H5T__bit_find(zeros_buf.data(), 3, 25, H5T_BIT_MSB, false), 9);

  zeros_buf = {0xFF, 0xFF, 0xFF, 0xFB, 0xFF};  // Bit 26 is 0.
  EXPECT_EQ(H5T__bit_find(zeros_buf.data(), 3, 26, H5T_BIT_LSB, false), 23);
  EXPECT_EQ(H5T__bit_find(zeros_buf.data(), 3, 26, H5T_BIT_MSB, false), 23);
}

TEST_F(H5TbitTest, BitIncCarryAndOverflowPaths) {
  // Single partial byte: start=2, size=3 (size + start < 8).
  {
    uint8_t byte = 0xAA;  // bits [4:2] = 0b010
    EXPECT_FALSE(H5T__bit_inc(&byte, 2, 3));
    EXPECT_EQ((byte >> 2) & 0x07, 0x03);
    EXPECT_EQ(byte & 0xE3, 0xAA & 0xE3);

    byte = 0xFF;  // bits [4:2] = 0b111 -> wrap with carry true, outer bits preserved
    EXPECT_TRUE(H5T__bit_inc(&byte, 2, 3));
    EXPECT_EQ(byte, 0xFF & ~(0x07 << 2));
  }

  // Multi-byte starting unaligned (start=5, size=14 -> byte 0:3 bits, byte 1:8 bits, byte 2:3 bits).
  {
    std::array<uint8_t, 3> buf = {0x55, 0x00, 0xA0};
    // No carry out of byte 0.
    EXPECT_FALSE(H5T__bit_inc(buf.data(), 5, 14));
    EXPECT_EQ(buf[0], 0x55 + (1 << 5));
    EXPECT_EQ(buf[1], 0x00);

    // Carry propagates through byte 1 (0xFF -> 0x00) and stops in last partial byte.
    buf = {0xE0 | 0x15, 0xFF, 0xA2};
    EXPECT_FALSE(H5T__bit_inc(buf.data(), 5, 14));
    EXPECT_THAT(buf, ElementsAre(0x15, 0x00, 0xA3));

    // Overflow out of last partial byte preserves upper bits of buf[2].
    buf = {0xFF, 0xFF, 0xAF};  // low 3 bits of buf[2] are 0b111
    EXPECT_TRUE(H5T__bit_inc(buf.data(), 5, 14));
    EXPECT_THAT(buf, ElementsAre(0x1F, 0x00, 0xA8));
  }

  // Byte-aligned start=0, size=16 (skips first partial and last partial).
  {
    std::array<uint8_t, 2> buf = {0xFF, 0x04};
    EXPECT_FALSE(H5T__bit_inc(buf.data(), 0, 16));
    EXPECT_THAT(buf, ElementsAre(0x00, 0x05));

    buf = {0xFF, 0xFF};
    EXPECT_TRUE(H5T__bit_inc(buf.data(), 0, 16));
    EXPECT_THAT(buf, ElementsAre(0x00, 0x00));
  }
}

TEST_F(H5TbitTest, BitDecBorrowAndUnderflowPaths) {
  // Same-byte case: start=2, size=4.
  {
    uint8_t byte = 0xAA;  // bits [5:2] = 0b1010 -> 0b1001
    EXPECT_FALSE(H5T__bit_dec(&byte, 2, 4));
    EXPECT_EQ(byte, 0xA6);

    byte = 0xC3;  // bits [5:2] = 0b0000 -> wrap to 0b1111, keep outer 0xC3, borrow true
    EXPECT_TRUE(H5T__bit_dec(&byte, 2, 4));
    EXPECT_EQ(byte, 0xFF);
  }

  // Multi-byte starting at start=5, size=14 (byte 0:3 bits, byte 1:8 bits, byte 2:3 bits).
  {
    // First byte has nonzero bits >= pos -> no borrow out of byte 0.
    std::array<uint8_t, 3> buf = {0x20 | 0x0A, 0x55, 0xA3};
    EXPECT_FALSE(H5T__bit_dec(buf.data(), 5, 14));
    EXPECT_THAT(buf, ElementsAre(0x0A, 0x55, 0xA3));

    // First byte bits >= pos are zero -> borrow into byte 1 (nonzero -> stops).
    buf = {0x0A, 0x05, 0xA3};
    EXPECT_FALSE(H5T__bit_dec(buf.data(), 5, 14));
    EXPECT_THAT(buf, ElementsAre(0xEA, 0x04, 0xA3));

    // Borrow propagates through byte 1 == 0x00 into last partial byte (nonzero -> no wrap).
    buf = {0x0A, 0x00, 0xA3};
    EXPECT_TRUE(H5T__bit_dec(buf.data(), 5, 14));
    EXPECT_THAT(buf, ElementsAre(0xEA, 0xFF, 0xA2));

    // Borrow reaches last partial byte whose field bits are zero -> fixes upper bit wrap.
    buf = {0x0A, 0x00, 0xA0};
    EXPECT_TRUE(H5T__bit_dec(buf.data(), 5, 14));
    EXPECT_THAT(buf, ElementsAre(0xEA, 0xFF, 0xA7));
  }

  // Byte-aligned multi-byte exact boundary (start=0, size=16): last partial skipped.
  {
    std::array<uint8_t, 2> buf = {0x00, 0x00};
    EXPECT_TRUE(H5T__bit_dec(buf.data(), 0, 16));
    EXPECT_THAT(buf, ElementsAre(0xFF, 0xFF));
  }
}

TEST_F(H5TbitTest, BitNegSameBytePartialAndExactMultiByte) {
  // Same-byte negate: start=2, size=4 preserves outer bits.
  {
    uint8_t byte = 0xAA;  // 10_1010_10 -> 10_0101_10 = 0x96
    H5T__bit_neg(&byte, 2, 4);
    EXPECT_EQ(byte, 0x96);
  }

  // Multi-byte with partial start, middle byte, and partial end.
  {
    std::array<uint8_t, 3> buf = {0xA5, 0x0F, 0xCC};
    H5T__bit_neg(buf.data(), 3, 17);  // bits [3..19]: 5 bits of byte0, byte1, 4 bits of byte2
    EXPECT_THAT(buf, ElementsAre(0x5D, 0xF0, 0xC3));
  }

  // Multi-byte with exact middle bytes (size % 8 == 0): skips final partial branch.
  {
    std::array<uint8_t, 2> buf = {0x12, 0xED};
    H5T__bit_neg(buf.data(), 0, 16);
    EXPECT_THAT(buf, ElementsAre(0xED, 0x12));
  }
}

}  // namespace
}  // namespace hdf5
