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
// Tests dynamically sized bit-vectors (hdf/src/bitvect.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/bitvect.c

#include <cstdint>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

// GoogleTest's gtest.h defines SUCCEED() and FAIL() as function-like macros.
// Undefine them before including HDF4 headers so hdf.h can define its constants
// SUCCEED (0) and FAIL (-1) without macro redefinition errors under no_modules.
#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/bitvect_priv.h"
#include "third_party/hdf4/hdf/src/hdf.h"

namespace hdf4 {
namespace {

TEST(BitvectTest, BvNewRejectsInvalidSizes) {
  EXPECT_EQ(nullptr, bv_new(-2));
  EXPECT_EQ(nullptr, bv_new(0));
}

TEST(BitvectTest, BvNewUsesDefaultWhenNegativeOne) {
  bv_ptr b = bv_new(-1);
  ASSERT_NE(nullptr, b);
  absl::Cleanup clean_b = [b] { EXPECT_EQ(SUCCEED, bv_delete(b)); };

  EXPECT_EQ(BV_DEFAULT_BITS, bv_size(b));
}

TEST(BitvectTest, BvNewAllocatesCustomSize) {
  // Test bit count not divisible by 8.
  bv_ptr b1 = bv_new(15);
  ASSERT_NE(nullptr, b1);
  absl::Cleanup clean_b1 = [b1] { EXPECT_EQ(SUCCEED, bv_delete(b1)); };
  EXPECT_EQ(15, bv_size(b1));

  // Test bit count divisible by 8.
  bv_ptr b2 = bv_new(16);
  ASSERT_NE(nullptr, b2);
  absl::Cleanup clean_b2 = [b2] { EXPECT_EQ(SUCCEED, bv_delete(b2)); };
  EXPECT_EQ(16, bv_size(b2));
}

TEST(BitvectTest, BvDeleteHandlesNullGracefully) {
  EXPECT_EQ(FAIL, bv_delete(nullptr));
}

TEST(BitvectTest, BvSizeHandlesNullGracefully) {
  EXPECT_EQ(FAIL, bv_size(nullptr));
}

TEST(BitvectTest, BvSetAndGetBasicOperations) {
  bv_ptr b = bv_new(32);
  ASSERT_NE(nullptr, b);
  absl::Cleanup clean_b = [b] { EXPECT_EQ(SUCCEED, bv_delete(b)); };

  // All initial bits must be zero.
  for (int32_t i = 0; i < 32; ++i) {
    EXPECT_EQ(BV_FALSE, bv_get(b, i));
  }

  // Set bit 5 to true.
  EXPECT_EQ(SUCCEED, bv_set(b, 5, BV_TRUE));
  EXPECT_EQ(BV_TRUE, bv_get(b, 5));
  EXPECT_EQ(BV_FALSE, bv_get(b, 4));
  EXPECT_EQ(BV_FALSE, bv_get(b, 6));

  // Set bit 5 back to false.
  EXPECT_EQ(SUCCEED, bv_set(b, 5, BV_FALSE));
  EXPECT_EQ(BV_FALSE, bv_get(b, 5));
}

TEST(BitvectTest, BvSetAndGetInvalidInputs) {
  bv_ptr b = bv_new(16);
  ASSERT_NE(nullptr, b);
  absl::Cleanup clean_b = [b] { EXPECT_EQ(SUCCEED, bv_delete(b)); };

  EXPECT_EQ(FAIL, bv_set(nullptr, 0, BV_TRUE));
  EXPECT_EQ(FAIL, bv_set(b, -1, BV_TRUE));

  EXPECT_EQ(FAIL, bv_get(nullptr, 0));
  EXPECT_EQ(FAIL, bv_get(b, -1));

  // Getting bit >= bits_used returns BV_FALSE.
  EXPECT_EQ(BV_FALSE, bv_get(b, 100));
}

TEST(BitvectTest, BvSetExtendsWithinAllocatedChunk) {
  // Initial size 10 bits. Buffer is allocated in BV_CHUNK_SIZE (64 bytes = 512
  // bits). Setting bit 20 expands bits_used without realloc.
  bv_ptr b = bv_new(10);
  ASSERT_NE(nullptr, b);
  absl::Cleanup clean_b = [b] { EXPECT_EQ(SUCCEED, bv_delete(b)); };

  EXPECT_EQ(10, bv_size(b));
  EXPECT_EQ(SUCCEED, bv_set(b, 20, BV_TRUE));
  EXPECT_EQ(21, bv_size(b));
  EXPECT_EQ(BV_TRUE, bv_get(b, 20));
  EXPECT_EQ(BV_FALSE, bv_get(b, 15));
}

TEST(BitvectTest, BvSetExtendsBeyondAllocatedChunkWithRealloc) {
  // Initial size 8 bits (1 byte). Allocated chunk is 64 bytes = 512 bits.
  // Setting bit 1000 forces realloc.
  bv_ptr b = bv_new(8);
  ASSERT_NE(nullptr, b);
  absl::Cleanup clean_b = [b] { EXPECT_EQ(SUCCEED, bv_delete(b)); };

  EXPECT_EQ(8, bv_size(b));
  EXPECT_EQ(SUCCEED, bv_set(b, 1000, BV_TRUE));
  EXPECT_EQ(1001, bv_size(b));
  EXPECT_EQ(BV_TRUE, bv_get(b, 1000));
  EXPECT_EQ(BV_FALSE, bv_get(b, 999));
  EXPECT_EQ(BV_FALSE, bv_get(b, 0));
}

TEST(BitvectTest, BvFindNextZeroBasic) {
  EXPECT_EQ(FAIL, bv_find_next_zero(nullptr));

  bv_ptr b = bv_new(16);
  ASSERT_NE(nullptr, b);
  absl::Cleanup clean_b = [b] { EXPECT_EQ(SUCCEED, bv_delete(b)); };

  // Initial next zero is bit 0.
  EXPECT_EQ(0, bv_find_next_zero(b));

  // Set bit 0 to true -> next zero is 1.
  EXPECT_EQ(SUCCEED, bv_set(b, 0, BV_TRUE));
  EXPECT_EQ(1, bv_find_next_zero(b));

  // Fill entire first byte (bits 0..7).
  for (int32_t i = 0; i < 8; ++i) {
    EXPECT_EQ(SUCCEED, bv_set(b, i, BV_TRUE));
  }
  // Next zero should be bit 8 in the second byte.
  EXPECT_EQ(8, bv_find_next_zero(b));
}

TEST(BitvectTest, BvFindNextZeroWithSlushBits) {
  // 12 bits: 1 full byte (8 bits) + 4 slush bits.
  bv_ptr b = bv_new(12);
  ASSERT_NE(nullptr, b);
  absl::Cleanup clean_b = [b] { EXPECT_EQ(SUCCEED, bv_delete(b)); };

  // Fill first 8 bits (full byte).
  for (int32_t i = 0; i < 8; ++i) {
    EXPECT_EQ(SUCCEED, bv_set(b, i, BV_TRUE));
  }

  // Next zero should be bit 8 (first slush bit).
  EXPECT_EQ(8, bv_find_next_zero(b));

  // Set bit 8 to true.
  EXPECT_EQ(SUCCEED, bv_set(b, 8, BV_TRUE));
  // Next zero is bit 9.
  EXPECT_EQ(9, bv_find_next_zero(b));

  // Fill remaining slush bits 9, 10, 11.
  EXPECT_EQ(SUCCEED, bv_set(b, 9, BV_TRUE));
  EXPECT_EQ(SUCCEED, bv_set(b, 10, BV_TRUE));
  EXPECT_EQ(SUCCEED, bv_set(b, 11, BV_TRUE));

  // Note: Upstream quirk in bitvect.c:
  // slush_bits is masked with bv_bit_mask[bits_used - bytes_used * 8] (here
  // 0x0F), so slush_bits can never equal 255. Instead, bv_first_zero[0x0F]
  // returns 4, yielding bit 12 ((1 * 8) + 4) without extending bits_used.
  EXPECT_EQ(12, bv_find_next_zero(b));
  EXPECT_EQ(12, bv_size(b));
}

TEST(BitvectTest, BvFindNextZeroWhenAllFullBytesAreSet) {
  // Exactly 16 bits (2 full bytes, 0 slush bits).
  bv_ptr b = bv_new(16);
  ASSERT_NE(nullptr, b);
  absl::Cleanup clean_b = [b] { EXPECT_EQ(SUCCEED, bv_delete(b)); };

  for (int32_t i = 0; i < 16; ++i) {
    EXPECT_EQ(SUCCEED, bv_set(b, i, BV_TRUE));
  }

  // All 16 bits set, no slush bits. Extends to 17 bits and returns 16.
  EXPECT_EQ(16, bv_find_next_zero(b));
  EXPECT_EQ(17, bv_size(b));
  EXPECT_EQ(BV_FALSE, bv_get(b, 16));
}

TEST(BitvectTest, BvSetClearsBitAndUpdatesLastZeroCache) {
  bv_ptr b = bv_new(32);
  ASSERT_NE(nullptr, b);
  absl::Cleanup clean_b = [b] { EXPECT_EQ(SUCCEED, bv_delete(b)); };

  // Set first 16 bits to true.
  for (int32_t i = 0; i < 16; ++i) {
    EXPECT_EQ(SUCCEED, bv_set(b, i, BV_TRUE));
  }

  // Next zero is bit 16, and last_zero cache advances to byte 2.
  EXPECT_EQ(16, bv_find_next_zero(b));

  // Now clear bit 3 in byte 0. This should update last_zero cache back to byte
  // 0.
  EXPECT_EQ(SUCCEED, bv_set(b, 3, BV_FALSE));
  EXPECT_EQ(3, bv_find_next_zero(b));
}

}  // namespace
}  // namespace hdf4
