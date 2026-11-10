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
// Unit tests for LercNS::RLE (third_party/lerc/LercLib/RLE.cpp).
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/RLE.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/RLE.cpp

#include "third_party/lerc/LercLib/RLE.h"

#include <array>
#include <cstddef>
#include <memory>
#include <numeric>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

namespace lerc {
namespace {

using ::LercNS::Byte;
using ::LercNS::RLE;
using ::testing::Each;
using ::testing::ElementsAreArray;

TEST(RleTest, InvalidInputsAndSingleByteReturnExpectedSizesAndFailures) {
  const RLE rle;
  const Byte single = 0x5A;

  // 1. Null or zero-length inputs yield 0 compressed bytes and fail compress.
  EXPECT_EQ(0U, rle.computeNumBytesRLE(nullptr, 10));
  EXPECT_EQ(0U, rle.computeNumBytesRLE(&single, 0));

  Byte* raw_rle = nullptr;
  std::size_t rle_size = 999;
  EXPECT_FALSE(rle.compress(nullptr, 4, &raw_rle, rle_size));
  EXPECT_FALSE(rle.compress(&single, 0, &raw_rle, rle_size));

  // 2. Single-byte input encodes as 2B count + 1B literal + 2B EOF = 5B.
  EXPECT_EQ(5U, rle.computeNumBytesRLE(&single, 1));
  const bool ok = rle.compress(&single, 1, &raw_rle, rle_size);
  std::unique_ptr<Byte[]> owned_rle(raw_rle);
  ASSERT_TRUE(ok);
  EXPECT_EQ(5U, rle_size);

  Byte* raw_out = nullptr;
  std::size_t out_size = 0;
  const bool dec_ok =
      RLE::decompress(owned_rle.get(), rle_size, &raw_out, out_size);
  std::unique_ptr<Byte[]> owned_out(raw_out);
  ASSERT_TRUE(dec_ok);
  EXPECT_EQ(1U, out_size);
  EXPECT_EQ(single, owned_out[0]);
}

TEST(RleTest, MixedOddEvenAndShortRepeatRunsRoundTripWithVerify) {
  // Construct a pattern crossing all state-machine transitions:
  // - Odd prefix
  // - Run of 4 identical bytes (< m_minNumEven == 5): stays in odd mode
  // - Odd middle switching to an even run (>= 5 identical bytes)
  // - Even run switching back to odd, ending in an odd byte
  std::vector<Byte> input = {1, 2, 3};
  input.insert(input.end(), 4, 0x10);
  input.push_back(4);
  input.push_back(5);
  input.insert(input.end(), 9, 0x77);
  input.push_back(6);
  input.push_back(7);

  const RLE rle;
  const std::size_t predicted =
      rle.computeNumBytesRLE(input.data(), input.size());
  Byte* raw_rle = nullptr;
  std::size_t rle_size = 0;
  const bool comp_ok =
      rle.compress(input.data(), input.size(), &raw_rle, rle_size,
                   /*verify=*/true);
  std::unique_ptr<Byte[]> owned_rle(raw_rle);
  ASSERT_TRUE(comp_ok);
  EXPECT_EQ(predicted, rle_size);

  // Also test starting immediately in an even run and ending in an even run.
  std::vector<Byte> even_ends(8, 0xAA);
  even_ends.push_back(0x11);
  even_ends.insert(even_ends.end(), 7, 0xBB);

  Byte* raw_even_rle = nullptr;
  std::size_t even_rle_size = 0;
  const bool even_ok =
      rle.compress(even_ends.data(), even_ends.size(), &raw_even_rle,
                   even_rle_size, /*verify=*/false);
  std::unique_ptr<Byte[]> owned_even_rle(raw_even_rle);
  ASSERT_TRUE(even_ok);
  EXPECT_EQ(rle.computeNumBytesRLE(even_ends.data(), even_ends.size()),
            even_rle_size);

  Byte* raw_out = nullptr;
  std::size_t out_size = 0;
  const bool dec_ok =
      RLE::decompress(owned_even_rle.get(), even_rle_size, &raw_out, out_size);
  std::unique_ptr<Byte[]> owned_out(raw_out);
  ASSERT_TRUE(dec_ok);
  ASSERT_EQ(even_ends.size(), out_size);
  EXPECT_THAT(std::vector<Byte>(owned_out.get(), owned_out.get() + out_size),
              ElementsAreArray(even_ends));

  // Verify pre-allocated decompress succeeds when capacity exceeds out_size.
  std::vector<Byte> prealloc(even_ends.size() + 4, 0xCC);
  ASSERT_TRUE(RLE::decompress(owned_rle.get(), rle_size, prealloc.data(),
                              prealloc.size()));
  EXPECT_THAT(
      std::vector<Byte>(prealloc.begin(), prealloc.begin() + input.size()),
      ElementsAreArray(input));
  EXPECT_THAT(
      std::vector<Byte>(prealloc.begin() + input.size(), prealloc.end()),
      Each(0xCC));
}

TEST(RleTest, SplitsOddAndEvenRunsExceedingMaxShortCounter) {
  const RLE rle;

  // 1. Over 32767 non-repeating bytes triggers odd-counter rollover splitting.
  constexpr std::size_t kLongOddLen = 32770;
  std::vector<Byte> odd_input(kLongOddLen);
  std::iota(odd_input.begin(), odd_input.end(), Byte{0});
  // Ensure adjacent bytes differ everywhere despite uint8 wrap-around.
  for (std::size_t i = 1; i < odd_input.size(); ++i) {
    if (odd_input[i] == odd_input[i - 1]) {
      odd_input[i] ^= 1;
    }
  }

  Byte* raw_odd_rle = nullptr;
  std::size_t odd_rle_size = 0;
  const bool odd_ok = rle.compress(odd_input.data(), odd_input.size(),
                                   &raw_odd_rle, odd_rle_size);
  std::unique_ptr<Byte[]> owned_odd_rle(raw_odd_rle);
  ASSERT_TRUE(odd_ok);
  EXPECT_EQ(rle.computeNumBytesRLE(odd_input.data(), odd_input.size()),
            odd_rle_size);

  std::vector<Byte> odd_out(kLongOddLen, 0xFF);
  ASSERT_TRUE(RLE::decompress(owned_odd_rle.get(), odd_rle_size, odd_out.data(),
                              odd_out.size()));
  EXPECT_THAT(odd_out, ElementsAreArray(odd_input));

  // 2. Over 32767 identical bytes triggers even-counter rollover splitting.
  constexpr std::size_t kLongEvenLen = 32770;
  std::vector<Byte> even_input(kLongEvenLen, 0x3C);

  Byte* raw_even_rle = nullptr;
  std::size_t even_rle_size = 0;
  const bool even_ok = rle.compress(even_input.data(), even_input.size(),
                                    &raw_even_rle, even_rle_size);
  std::unique_ptr<Byte[]> owned_even_rle(raw_even_rle);
  ASSERT_TRUE(even_ok);
  EXPECT_EQ(rle.computeNumBytesRLE(even_input.data(), even_input.size()),
            even_rle_size);

  Byte* raw_even_out = nullptr;
  std::size_t even_out_size = 0;
  const bool even_dec_ok = RLE::decompress(owned_even_rle.get(), even_rle_size,
                                           &raw_even_out, even_out_size);
  std::unique_ptr<Byte[]> owned_even_out(raw_even_out);
  ASSERT_TRUE(even_dec_ok);
  ASSERT_EQ(kLongEvenLen, even_out_size);
  EXPECT_THAT(std::vector<Byte>(owned_even_out.get(),
                                owned_even_out.get() + even_out_size),
              Each(0x3C));
}

TEST(RleTest, DecompressRejectsInvalidStreamsTruncationsAndSmallBuffers) {
  const std::vector<Byte> sample = {10, 20, 20, 20, 20, 20, 20, 30, 40};
  const RLE rle;
  Byte* raw_rle = nullptr;
  std::size_t rle_size = 0;
  ASSERT_TRUE(rle.compress(sample.data(), sample.size(), &raw_rle, rle_size));
  std::unique_ptr<Byte[]> owned_rle(raw_rle);

  // 1. Null pointers and sub-header lengths fail cleanly.
  Byte* alloc_out = nullptr;
  std::size_t out_size = 0;
  std::vector<Byte> out_buf(sample.size(), 0x99);
  EXPECT_FALSE(RLE::decompress(nullptr, rle_size, &alloc_out, out_size));
  EXPECT_FALSE(RLE::decompress(owned_rle.get(), 1, &alloc_out, out_size));
  EXPECT_FALSE(
      RLE::decompress(nullptr, rle_size, out_buf.data(), out_buf.size()));
  EXPECT_FALSE(RLE::decompress(owned_rle.get(), rle_size, nullptr, 4));
  EXPECT_FALSE(
      RLE::decompress(owned_rle.get(), 1, out_buf.data(), out_buf.size()));

  // 2. Truncation at every byte offset short of complete stream fails.
  for (std::size_t trunc = 2; trunc < rle_size; ++trunc) {
    EXPECT_FALSE(RLE::decompress(owned_rle.get(), trunc, &alloc_out, out_size))
        << "allocating decompress unexpectedly succeeded at trunc=" << trunc;
    EXPECT_FALSE(
        RLE::decompress(owned_rle.get(), trunc, out_buf.data(), out_buf.size()))
        << "prealloc decompress unexpectedly succeeded at trunc=" << trunc;
  }

  // 3. Undersized pre-allocated buffer rejects stream without overflowing.
  EXPECT_FALSE(RLE::decompress(owned_rle.get(), rle_size, out_buf.data(),
                               sample.size() - 1));

  // 4. Empty-payload stream (only little-endian EOF marker -32768 = 0x00, 0x80)
  //    sets *arr to nullptr and returns false in allocating decompress.
  constexpr auto kEofOnlyStream = std::to_array<Byte>({0x00, 0x80});
  alloc_out = out_buf.data();
  out_size = 123;
  EXPECT_FALSE(RLE::decompress(kEofOnlyStream.data(), kEofOnlyStream.size(),
                               &alloc_out, out_size));
  EXPECT_EQ(nullptr, alloc_out);
  EXPECT_EQ(0U, out_size);

  // 5. Hand-crafted stream with cnt == 0 consumes 1 dummy byte cleanly in
  //    pre-allocated decompress (per zero-count guard against corrupted v<=2).
  constexpr auto kZeroCountStream =
      std::to_array<Byte>({0x00, 0x00, 0x42, 0x00, 0x80});
  EXPECT_TRUE(RLE::decompress(kZeroCountStream.data(), kZeroCountStream.size(),
                              out_buf.data(), out_buf.size()));
}

}  // namespace
}  // namespace lerc
