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
// Unit tests for LercNS::BitMask (third_party/lerc/LercLib/BitMask.cpp).
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/BitMask.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/BitMask.cpp

#include "third_party/lerc/LercLib/BitMask.h"

#include <cstdint>

#include "testing/base/public/gunit.h"

namespace lerc {
namespace {

using ::LercNS::BitMask;
using ::LercNS::Byte;

TEST(BitMaskTest, DefaultConstructedMaskIsEmpty) {
  BitMask mask;
  const BitMask& const_mask = mask;

  EXPECT_EQ(0, mask.GetWidth());
  EXPECT_EQ(0, mask.GetHeight());
  EXPECT_EQ(0U, mask.Size());
  EXPECT_EQ(nullptr, mask.Bits());
  EXPECT_EQ(nullptr, const_mask.Bits());
  EXPECT_EQ(0, mask.CountValidBits());

  // Clear and SetSize(0, 0) remain no-ops on an empty mask.
  mask.Clear();
  EXPECT_TRUE(mask.SetSize(0, 0));
  EXPECT_EQ(nullptr, mask.Bits());
}

TEST(BitMaskTest, SetSizeHandlesConstructionSameSizeResizeAndInvalidDims) {
  // 1. Parameterized constructor allocates storage for 5x3 (15 bits -> 2
  // bytes).
  BitMask constructed(5, 3);
  EXPECT_EQ(5, constructed.GetWidth());
  EXPECT_EQ(3, constructed.GetHeight());
  EXPECT_EQ(2U, constructed.Size());
  ASSERT_NE(nullptr, constructed.Bits());

  // 2. Re-calling SetSize with identical dimensions reuses the same buffer.
  const Byte* original_ptr = constructed.Bits();
  EXPECT_TRUE(constructed.SetSize(5, 3));
  EXPECT_EQ(original_ptr, constructed.Bits());

  // 3. Zero or negative dimensions clear the mask; (0, 0) returns true while
  //    any partial-zero or negative dimension returns false.
  struct InvalidDimCase {
    int cols;
    int rows;
    bool expected_ok;
  };
  constexpr InvalidDimCase kInvalidCases[] = {
      {.cols = 0, .rows = 3, .expected_ok = false},
      {.cols = 5, .rows = 0, .expected_ok = false},
      {.cols = -1, .rows = 3, .expected_ok = false},
      {.cols = 3, .rows = -1, .expected_ok = false},
      {.cols = -2, .rows = -2, .expected_ok = false},
      {.cols = 0, .rows = 0, .expected_ok = true},
  };
  for (const auto& tc : kInvalidCases) {
    ASSERT_TRUE(constructed.SetSize(5, 3));
    EXPECT_EQ(tc.expected_ok, constructed.SetSize(tc.cols, tc.rows));
    EXPECT_EQ(0, constructed.GetWidth());
    EXPECT_EQ(0, constructed.GetHeight());
    EXPECT_EQ(0U, constructed.Size());
    EXPECT_EQ(nullptr, constructed.Bits());
  }
}

TEST(BitMaskTest, BitAccessAndCountValidBitsHandlePaddingBits) {
  EXPECT_EQ(Byte{0x80}, BitMask::Bit(0));
  EXPECT_EQ(Byte{0x01}, BitMask::Bit(7));
  EXPECT_EQ(Byte{0x80}, BitMask::Bit(8));

  // 5x3 = 15 bits occupies 2 bytes, leaving bit 15 as trailing padding.
  BitMask mask(5, 3);
  ASSERT_NE(nullptr, mask.Bits());

  mask.SetAllValid();
  EXPECT_EQ(15, mask.CountValidBits());

  // Clearing the unused padding bit still yields 15 valid bits.
  mask.SetInvalid(15);
  EXPECT_EQ(15, mask.CountValidBits());

  mask.SetAllInvalid();
  EXPECT_EQ(0, mask.CountValidBits());

  // Toggle bits via both linear-index and (row, col) overloads.
  mask.SetValid(0);
  mask.SetValid(2, 4);  // k = 14
  mask.SetValid(1, 2);  // k = 7
  EXPECT_EQ(1, mask.IsValid(0));
  EXPECT_EQ(1, mask.IsValid(0, 0));
  EXPECT_EQ(1, mask.IsValid(7));
  EXPECT_EQ(1, mask.IsValid(1, 2));
  EXPECT_EQ(1, mask.IsValid(14));
  EXPECT_EQ(1, mask.IsValid(2, 4));
  EXPECT_EQ(3, mask.CountValidBits());

  mask.SetInvalid(1, 2);
  EXPECT_EQ(0, mask.IsValid(7));
  mask.SetInvalid(14);
  EXPECT_EQ(0, mask.IsValid(2, 4));
  EXPECT_EQ(1, mask.CountValidBits());

  // Exact byte-aligned dimensions (4x2 = 8 bits) have zero padding bits.
  BitMask aligned_mask(4, 2);
  aligned_mask.SetAllValid();
  EXPECT_EQ(8, aligned_mask.CountValidBits());
}

TEST(BitMaskTest, CopyConstructorAndAssignmentDeepCopyAndHandleEmptyAndSelf) {
  BitMask src(5, 3);
  src.SetAllInvalid();
  src.SetValid(0);
  src.SetValid(1, 1);  // k = 6
  src.SetValid(2, 4);  // k = 14

  // 1. Copy construction produces an independent deep copy.
  BitMask copy_ctor(src);
  EXPECT_EQ(5, copy_ctor.GetWidth());
  EXPECT_EQ(3, copy_ctor.GetHeight());
  EXPECT_NE(src.Bits(), copy_ctor.Bits());
  EXPECT_EQ(3, copy_ctor.CountValidBits());

  src.SetAllValid();
  EXPECT_EQ(15, src.CountValidBits());
  EXPECT_EQ(3, copy_ctor.CountValidBits());
  EXPECT_EQ(0, copy_ctor.IsValid(1));

  // 2. Copy assignment onto a differently sized mask resizes and deep-copies.
  BitMask assigned(2, 2);
  assigned.SetAllValid();
  assigned = copy_ctor;
  EXPECT_EQ(5, assigned.GetWidth());
  EXPECT_EQ(3, assigned.GetHeight());
  EXPECT_EQ(3, assigned.CountValidBits());
  copy_ctor.SetInvalid(0);
  EXPECT_EQ(1, assigned.IsValid(0));

  // 3. Self-assignment via alias leaves storage and contents intact.
  const Byte* before_ptr = assigned.Bits();
  BitMask* alias = &assigned;
  *alias = assigned;
  EXPECT_EQ(before_ptr, assigned.Bits());
  EXPECT_EQ(3, assigned.CountValidBits());

  // 4. Copying and assigning from an empty mask leaves destination empty.
  BitMask empty;
  BitMask empty_copy(empty);
  EXPECT_EQ(0U, empty_copy.Size());
  EXPECT_EQ(nullptr, empty_copy.Bits());

  assigned = empty;
  EXPECT_EQ(0, assigned.GetWidth());
  EXPECT_EQ(0, assigned.GetHeight());
  EXPECT_EQ(nullptr, assigned.Bits());
}

}  // namespace
}  // namespace lerc
