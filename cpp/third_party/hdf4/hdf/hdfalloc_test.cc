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

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf4/hdf/src/hdf.h"

namespace hdf4 {
namespace {

TEST(HdfallocTest, MemfillZeroSizeOrZeroItems) {
  std::vector<uint8_t> dest = {1, 2, 3, 4};
  const uint8_t src = 99;

  // Zero num_items leaves dest untouched.
  EXPECT_EQ(dest.data(), HDmemfill(dest.data(), &src, sizeof(src), 0));
  EXPECT_THAT(dest, ::testing::ElementsAre(1, 2, 3, 4));

  // Zero item_size leaves dest untouched.
  EXPECT_EQ(dest.data(), HDmemfill(dest.data(), &src, 0, 4));
  EXPECT_THAT(dest, ::testing::ElementsAre(1, 2, 3, 4));
}

TEST(HdfallocTest, MemfillSingleItem) {
  uint32_t dest = 0;
  constexpr uint32_t kPattern = 0x12345678;

  EXPECT_EQ(&dest, HDmemfill(&dest, &kPattern, sizeof(kPattern), 1));
  EXPECT_EQ(dest, kPattern);
}

TEST(HdfallocTest, MemfillPowerOfTwoItems) {
  std::vector<uint16_t> dest(16, 0);
  constexpr uint16_t kPattern = 0xABCD;

  EXPECT_EQ(dest.data(),
            HDmemfill(dest.data(), &kPattern, sizeof(kPattern), dest.size()));
  for (uint16_t val : dest) {
    EXPECT_EQ(val, kPattern);
  }
}

TEST(HdfallocTest, MemfillNonPowerOfTwoItemsWithRemainder) {
  // Test 13 items with a 3-byte pattern.
  // This exercises the while loop doubling and the `if (items_left > 0)`
  // remainder branch.
  struct Pattern {
    uint8_t a, b, c;
    bool operator==(const Pattern& other) const {
      return a == other.a && b == other.b && c == other.c;
    }
  };
  constexpr Pattern kPattern = {0xAA, 0x55, 0xCC};

  constexpr size_t kNumItems = 13;
  std::vector<Pattern> dest(kNumItems, Pattern{0, 0, 0});

  EXPECT_EQ(dest.data(),
            HDmemfill(dest.data(), &kPattern, sizeof(Pattern), kNumItems));
  for (size_t i = 0; i < kNumItems; ++i) {
    EXPECT_EQ(dest[i], kPattern);
  }
}

TEST(HdfallocTest, MemfillVariousItemCounts) {
  constexpr uint8_t kVal = 42;
  for (size_t count = 1; count <= 33; ++count) {
    std::vector<uint8_t> dest(count, 0);
    EXPECT_EQ(dest.data(), HDmemfill(dest.data(), &kVal, sizeof(kVal), count));
    for (size_t i = 0; i < count; ++i) {
      EXPECT_EQ(dest[i], kVal);
    }
  }
}

TEST(HdfallocTest, StrncpyZeroLength) {
  char buf[8] = "hello";
  // len == 0 returns dest immediately and does not modify the buffer.
  EXPECT_EQ(buf, HIstrncpy(buf, "world", 0));
  EXPECT_STREQ(buf, "hello");
}

TEST(HdfallocTest, StrncpyLengthOne) {
  char buf[8] = "hello";
  // len == 1 only writes NUL terminator.
  EXPECT_EQ(buf, HIstrncpy(buf, "world", 1));
  EXPECT_EQ(buf[0], '\0');
  EXPECT_STREQ(buf, "");
}

TEST(HdfallocTest, StrncpyNegativeLength) {
  char buf[8] = "hello";
  // Negative length treats loop condition (len > 1) as false and writes NUL at
  // start.
  EXPECT_EQ(buf, HIstrncpy(buf, "world", -1));
  EXPECT_EQ(buf[0], '\0');
}

TEST(HdfallocTest, StrncpyExactAndTruncated) {
  char buf[6] = {};

  // Exact fit: source has 4 chars, len is 5.
  EXPECT_EQ(buf, HIstrncpy(buf, "test", 5));
  EXPECT_STREQ(buf, "test");

  // Truncation: source has 8 chars, len is 4 -> should copy 3 chars + NUL.
  EXPECT_EQ(buf, HIstrncpy(buf, "12345678", 4));
  EXPECT_STREQ(buf, "123");
}

TEST(HdfallocTest, StrncpySourceShorterThanLength) {
  char buf[16] = {};
  EXPECT_EQ(buf, HIstrncpy(buf, "short", 16));
  EXPECT_STREQ(buf, "short");
}

TEST(HdfallocTest, StrncpyEmptySource) {
  char buf[16] = "existing";
  EXPECT_EQ(buf, HIstrncpy(buf, "", 8));
  EXPECT_STREQ(buf, "");
}

}  // namespace
}  // namespace hdf4
