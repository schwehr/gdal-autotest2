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
#include "third_party/lerc/LercLib/Defines.h"

namespace lerc {
namespace {

using ::LercNS::Byte;
using ::LercNS::RLE;
using ::testing::Each;
using ::testing::ElementsAreArray;

[[nodiscard]] std::unique_ptr<Byte[]> CompressOwned(const Byte* arr,
                                                    std::size_t num_bytes,
                                                    std::size_t& out_size,
                                                    bool verify = false) {
  const RLE rle;
  Byte* raw_rle = nullptr;
  const bool ok = rle.compress(arr, num_bytes, &raw_rle, out_size, verify);
  // Arm ownership immediately: compress(..., verify=true) allocates before
  // verifying and leaves *arrRLE allocated if verification fails.
  std::unique_ptr<Byte[]> owned(raw_rle);
  if (!ok) {
    ADD_FAILURE() << "RLE::compress failed for num_bytes=" << num_bytes;
    return nullptr;
  }
  return owned;
}

TEST(RleTest, InvalidInputsAndSingleByteReturnExpectedSizesAndFailures) {
  const RLE rle;
  const Byte single = 0x5A;

  // 1. Null or zero-length inputs yield 0 compressed bytes and fail compress.
  EXPECT_EQ(0U, rle.computeNumBytesRLE(nullptr, 10));
  EXPECT_EQ(0U, rle.computeNumBytesRLE(&single, 0));

  Byte* raw_rle = nullptr;
  std::size_t rle_size = 999;
  EXPECT_FALSE(rle.compress(nullptr, 4, &raw_rle, rle_size));
  std::unique_ptr<Byte[]> cleanup_null(raw_rle);
  raw_rle = nullptr;
  EXPECT_FALSE(rle.compress(&single, 0, &raw_rle, rle_size));
  std::unique_ptr<Byte[]> cleanup_zero(raw_rle);

  // 2. Single-byte input encodes as 2B count + 1B literal + 2B EOF = 5B.
  EXPECT_EQ(5U, rle.computeNumBytesRLE(&single, 1));
  std::unique_ptr<Byte[]> owned_rle = CompressOwned(&single, 1, rle_size);
  ASSERT_NE(nullptr, owned_rle);
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
  // - 3-byte odd prefix + 4 identical bytes (< m_minNumEven == 5) + 2 odd bytes
  //   = 9 odd bytes -> 2B count + 9B literals = 11B
  // - Run of 9 identical bytes (>= 5) -> 2B count + 1B value = 3B
  // - 2 trailing odd bytes -> 2B count + 2B literals = 4B
  // - 2B EOF marker -> total 20B
  std::vector<Byte> input = {1, 2, 3};
  input.insert(input.end(), 4, 0x10);
  input.push_back(4);
  input.push_back(5);
  input.insert(input.end(), 9, 0x77);
  input.push_back(6);
  input.push_back(7);

  const RLE rle;
  EXPECT_EQ(20U, rle.computeNumBytesRLE(input.data(), input.size()));
  std::size_t rle_size = 0;
  std::unique_ptr<Byte[]> owned_rle =
      CompressOwned(input.data(), input.size(), rle_size, /*verify=*/true);
  ASSERT_NE(nullptr, owned_rle);
  EXPECT_EQ(20U, rle_size);

  // Also test starting immediately in an even run and ending in an even run:
  // 3B even + 3B odd + 3B even + 2B EOF = 11B.
  std::vector<Byte> even_ends(8, 0xAA);
  even_ends.push_back(0x11);
  even_ends.insert(even_ends.end(), 7, 0xBB);

  EXPECT_EQ(11U, rle.computeNumBytesRLE(even_ends.data(), even_ends.size()));
  std::size_t even_rle_size = 0;
  std::unique_ptr<Byte[]> owned_even_rle =
      CompressOwned(even_ends.data(), even_ends.size(), even_rle_size);
  ASSERT_NE(nullptr, owned_even_rle);
  EXPECT_EQ(11U, even_rle_size);

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
  std::vector<Byte> prealloc(input.size() + 4, 0xCC);
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

  // 1. 32770 bytes via uint8 wrap-around iota have distinct adjacent pairs
  //    everywhere, forcing an odd rollover split:
  //    (2 + 32767) + (2 + 3) + 2 EOF = 32776B.
  constexpr std::size_t kLongOddLen = 32770;
  std::vector<Byte> odd_input(kLongOddLen);
  std::iota(odd_input.begin(), odd_input.end(), Byte{0});

  EXPECT_EQ(32776U, rle.computeNumBytesRLE(odd_input.data(), odd_input.size()));
  std::size_t odd_rle_size = 0;
  std::unique_ptr<Byte[]> owned_odd_rle =
      CompressOwned(odd_input.data(), odd_input.size(), odd_rle_size);
  ASSERT_NE(nullptr, owned_odd_rle);
  EXPECT_EQ(32776U, odd_rle_size);

  std::vector<Byte> odd_out(kLongOddLen, 0xFF);
  ASSERT_TRUE(RLE::decompress(owned_odd_rle.get(), odd_rle_size, odd_out.data(),
                              odd_out.size()));
  EXPECT_THAT(odd_out, ElementsAreArray(odd_input));

  // 2. 32770 identical bytes forces an even-counter rollover split:
  //    3B (32767 run) + 3B (3 run) + 2B EOF = 8B.
  constexpr std::size_t kLongEvenLen = 32770;
  const std::vector<Byte> even_input(kLongEvenLen, 0x3C);

  EXPECT_EQ(8U, rle.computeNumBytesRLE(even_input.data(), even_input.size()));
  std::size_t even_rle_size = 0;
  std::unique_ptr<Byte[]> owned_even_rle =
      CompressOwned(even_input.data(), even_input.size(), even_rle_size);
  ASSERT_NE(nullptr, owned_even_rle);
  EXPECT_EQ(8U, even_rle_size);

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
  std::size_t rle_size = 0;
  std::unique_ptr<Byte[]> owned_rle =
      CompressOwned(sample.data(), sample.size(), rle_size);
  ASSERT_NE(nullptr, owned_rle);

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
  //    pre-allocated decompress without writing to output (zero-count guard
  //    against corrupted v<=2 streams).
  constexpr auto kZeroCountStream =
      std::to_array<Byte>({0x00, 0x00, 0x42, 0x00, 0x80});
  std::vector<Byte> zero_cnt_out(4, 0x99);
  EXPECT_TRUE(RLE::decompress(kZeroCountStream.data(), kZeroCountStream.size(),
                              zero_cnt_out.data(), zero_cnt_out.size()));
  EXPECT_THAT(zero_cnt_out, Each(0x99));
}

}  // namespace
}  // namespace lerc
