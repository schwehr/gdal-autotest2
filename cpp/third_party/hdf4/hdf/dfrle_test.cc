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
// Tests for in-memory RLE image compression (hdf/src/dfrle.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/dfrle.c

#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"

namespace hdf4 {
namespace {

using ::testing::ElementsAreArray;

class DfrleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Reset static state in DFCIunrle
    uint8_t dummy = 0;
    DFCIunrle(&dummy, &dummy, 0, /*resetsave=*/1);
  }

  void TearDown() override {
    uint8_t dummy = 0;
    DFCIunrle(&dummy, &dummy, 0, /*resetsave=*/1);
  }
};

// 1. Empty buffer compression and decompression
TEST_F(DfrleTest, EmptyBuffer) {
  std::vector<uint8_t> src;
  std::vector<uint8_t> comp(10);
  int32 comp_len = DFCIrle(src.data(), comp.data(), 0);
  EXPECT_EQ(0, comp_len);

  std::vector<uint8_t> decomp(10);
  int32 consumed = DFCIunrle(comp.data(), decomp.data(), 0, /*resetsave=*/1);
  EXPECT_EQ(0, consumed);
}

// 2. Single character and two identical characters (treated as literals, not
// runs)
TEST_F(DfrleTest, OneAndTwoCharsAreLiterals) {
  // Single char 'A' -> count 1, 'A'
  const uint8_t one_char[] = {'A'};
  uint8_t comp[16] = {};
  int32 comp_len = DFCIrle(one_char, comp, 1);
  EXPECT_EQ(2, comp_len);
  EXPECT_EQ(1, comp[0]);
  EXPECT_EQ('A', comp[1]);

  uint8_t decomp[16] = {};
  int32 consumed = DFCIunrle(comp, decomp, 1, /*resetsave=*/1);
  EXPECT_EQ(2, consumed);
  EXPECT_EQ('A', decomp[0]);

  // Two chars "BB" -> count 2, 'B', 'B' (runs must be >= 3 chars in dfrle.c)
  const uint8_t two_chars[] = {'B', 'B'};
  comp_len = DFCIrle(two_chars, comp, 2);
  EXPECT_EQ(3, comp_len);
  EXPECT_EQ(2, comp[0]);
  EXPECT_EQ('B', comp[1]);
  EXPECT_EQ('B', comp[2]);

  std::memset(decomp, 0, sizeof(decomp));
  consumed = DFCIunrle(comp, decomp, 2, /*resetsave=*/1);
  EXPECT_EQ(3, consumed);
  EXPECT_EQ('B', decomp[0]);
  EXPECT_EQ('B', decomp[1]);
}

// 3. Three or more identical characters are compressed into a run
TEST_F(DfrleTest, RunOfThreeOrMore) {
  // Exactly 3 chars "CCC" -> token (128 | 3 = 131 = 0x83), 'C'
  const uint8_t three_chars[] = {'C', 'C', 'C'};
  uint8_t comp[16] = {};
  int32 comp_len = DFCIrle(three_chars, comp, 3);
  EXPECT_EQ(2, comp_len);
  EXPECT_EQ(0x83, comp[0]);
  EXPECT_EQ('C', comp[1]);

  uint8_t decomp[16] = {};
  int32 consumed = DFCIunrle(comp, decomp, 3, /*resetsave=*/1);
  EXPECT_EQ(2, consumed);
  EXPECT_EQ('C', decomp[0]);
  EXPECT_EQ('C', decomp[1]);
  EXPECT_EQ('C', decomp[2]);

  // 10 chars "DDDDDDDDDD" -> token (128 | 10 = 138 = 0x8A), 'D'
  std::vector<uint8_t> ten_chars(10, 'D');
  comp_len = DFCIrle(ten_chars.data(), comp, 10);
  EXPECT_EQ(2, comp_len);
  EXPECT_EQ(0x8A, comp[0]);
  EXPECT_EQ('D', comp[1]);

  std::vector<uint8_t> decomp_ten(10, 0);
  consumed = DFCIunrle(comp, decomp_ten.data(), 10, /*resetsave=*/1);
  EXPECT_EQ(2, consumed);
  EXPECT_THAT(decomp_ten, ElementsAreArray(ten_chars));
}

// 4. Long run exceeding 120 identical characters
// dfrle.c caps each run token at 120 (due to condition i + 120 > len)
TEST_F(DfrleTest, LongRunExceeding120Chars) {
  const int count = 250;
  std::vector<uint8_t> src(count, 'Z');
  std::vector<uint8_t> comp(count * 2);
  int32 comp_len = DFCIrle(src.data(), comp.data(), count);

  // 250 chars = 120 ('\xF8', 'Z') + 120 ('\xF8', 'Z') + 10 ('\x8A', 'Z') = 6
  // bytes
  EXPECT_EQ(6, comp_len);
  EXPECT_EQ(128 | 120, comp[0]);
  EXPECT_EQ('Z', comp[1]);
  EXPECT_EQ(128 | 120, comp[2]);
  EXPECT_EQ('Z', comp[3]);
  EXPECT_EQ(128 | 10, comp[4]);
  EXPECT_EQ('Z', comp[5]);

  std::vector<uint8_t> decomp(count, 0);
  int32 consumed =
      DFCIunrle(comp.data(), decomp.data(), count, /*resetsave=*/1);
  EXPECT_EQ(comp_len, consumed);
  EXPECT_THAT(decomp, ElementsAreArray(src));
}

// 5. Long literal sequence exceeding 120 unique characters
// dfrle.c splits literals when p - begp > 120 (lines 80-84)
TEST_F(DfrleTest, LongLiteralExceeding120Chars) {
  const int count = 250;
  std::vector<uint8_t> src(count);
  for (int i = 0; i < count; ++i) {
    // Avoid any 3 consecutive identical characters
    src[i] = static_cast<uint8_t>(i % 250);
  }

  std::vector<uint8_t> comp(count * 2);
  int32 comp_len = DFCIrle(src.data(), comp.data(), count);
  EXPECT_GT(comp_len, 0);

  std::vector<uint8_t> decomp(count, 0);
  int32 consumed =
      DFCIunrle(comp.data(), decomp.data(), count, /*resetsave=*/1);
  EXPECT_EQ(comp_len, consumed);
  EXPECT_THAT(decomp, ElementsAreArray(src));
}

// 6. Runs interleaved with literals
TEST_F(DfrleTest, RunsInterleavedWithLiterals) {
  // "ABC" (literals) + "DDDDD" (run of 5) + "EF" (literals) + "GGGG" (run of 4)
  std::string text = "ABCDDDDDEFGGGG";
  std::vector<uint8_t> src(text.begin(), text.end());
  std::vector<uint8_t> comp(src.size() * 2);
  int32 comp_len =
      DFCIrle(src.data(), comp.data(), static_cast<int32>(src.size()));
  EXPECT_GT(comp_len, 0);

  std::vector<uint8_t> decomp(src.size(), 0);
  int32 consumed = DFCIunrle(comp.data(), decomp.data(),
                             static_cast<int32>(src.size()), /*resetsave=*/1);
  EXPECT_EQ(comp_len, consumed);
  EXPECT_THAT(decomp, ElementsAreArray(src));
}

// 7. Consecutive different runs (hits p == begp after run)
TEST_F(DfrleTest, ConsecutiveDifferentRuns) {
  std::string text = "AAAAABBBBBCCCCCDDDDD";
  std::vector<uint8_t> src(text.begin(), text.end());
  std::vector<uint8_t> comp(src.size() * 2);
  int32 comp_len =
      DFCIrle(src.data(), comp.data(), static_cast<int32>(src.size()));

  // 4 runs of 5 chars = 4 * 2 bytes = 8 bytes total
  EXPECT_EQ(8, comp_len);

  std::vector<uint8_t> decomp(src.size(), 0);
  int32 consumed = DFCIunrle(comp.data(), decomp.data(),
                             static_cast<int32>(src.size()), /*resetsave=*/1);
  EXPECT_EQ(comp_len, consumed);
  EXPECT_THAT(decomp, ElementsAreArray(src));
}

// 8. Partial decompression of a run with saved state
// Verifies DFCIunrle static save buffer preserves unread run bytes across calls
TEST_F(DfrleTest, PartialDecompressionRunSavedState) {
  std::vector<uint8_t> src(10, 'X');
  std::vector<uint8_t> comp(16);
  int32 comp_len = DFCIrle(src.data(), comp.data(), 10);
  EXPECT_EQ(2, comp_len);

  // Request only 4 bytes with resetsave = 1
  std::vector<uint8_t> chunk1(4, 0);
  int32 consumed1 = DFCIunrle(comp.data(), chunk1.data(), 4, /*resetsave=*/1);
  EXPECT_EQ(2, consumed1);  // The entire 2-byte token was read from comp
  for (uint8_t b : chunk1) {
    EXPECT_EQ('X', b);
  }

  // Request remaining 6 bytes with resetsave = 0 (read from static save array)
  std::vector<uint8_t> chunk2(6, 0);
  int32 consumed2 =
      DFCIunrle(comp.data() + consumed1, chunk2.data(), 6, /*resetsave=*/0);
  EXPECT_EQ(0, consumed2);  // 0 compressed bytes consumed, came from save
  for (uint8_t b : chunk2) {
    EXPECT_EQ('X', b);
  }
}

// 9. Partial decompression of literals with saved state
// Verifies DFCIunrle static save buffer preserves unread literal bytes across
// calls
TEST_F(DfrleTest, PartialDecompressionLiteralSavedState) {
  std::string text = "0123456789";
  std::vector<uint8_t> src(text.begin(), text.end());
  std::vector<uint8_t> comp(16);
  int32 comp_len = DFCIrle(src.data(), comp.data(), 10);
  EXPECT_EQ(11, comp_len);  // 1 count byte + 10 data bytes

  // Request first 3 bytes with resetsave = 1
  std::vector<uint8_t> chunk1(3, 0);
  int32 consumed1 = DFCIunrle(comp.data(), chunk1.data(), 3, /*resetsave=*/1);
  EXPECT_EQ(11, consumed1);
  EXPECT_EQ('0', chunk1[0]);
  EXPECT_EQ('1', chunk1[1]);
  EXPECT_EQ('2', chunk1[2]);

  // Request remaining 7 bytes with resetsave = 0
  std::vector<uint8_t> chunk2(7, 0);
  int32 consumed2 =
      DFCIunrle(comp.data() + consumed1, chunk2.data(), 7, /*resetsave=*/0);
  EXPECT_EQ(0, consumed2);
  std::string rest(chunk2.begin(), chunk2.end());
  EXPECT_EQ("3456789", rest);
}

// 10. Multi-stage partial read that empties save buffer
TEST_F(DfrleTest, MultiStagePartialReadEmptiesSave) {
  std::vector<uint8_t> src(10, 'M');
  std::vector<uint8_t> comp(16);
  DFCIrle(src.data(), comp.data(), 10);

  // Read 4 bytes (6 saved)
  std::vector<uint8_t> b1(4);
  DFCIunrle(comp.data(), b1.data(), 4, /*resetsave=*/1);

  // Read 2 bytes from save (4 saved)
  std::vector<uint8_t> b2(2);
  DFCIunrle(comp.data(), b2.data(), 2, /*resetsave=*/0);
  for (uint8_t b : b2) EXPECT_EQ('M', b);

  // Read remaining 4 bytes from save (0 saved, resets savestart = saveend =
  // save)
  std::vector<uint8_t> b3(4);
  DFCIunrle(comp.data(), b3.data(), 4, /*resetsave=*/0);
  for (uint8_t b : b3) EXPECT_EQ('M', b);
}

}  // namespace
}  // namespace hdf4
