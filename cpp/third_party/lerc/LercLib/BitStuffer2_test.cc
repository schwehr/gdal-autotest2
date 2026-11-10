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
// Unit tests for LercNS::BitStuffer2
// (third_party/lerc/LercLib/BitStuffer2.cpp).
//
// Note: BitStuff_Before_Lerc2v3 (lerc2Version < 3) casts *ppByte directly to
// unsigned int*. Tests for version < 3 align the destination cursor past the
// header so UBSan and strict-alignment targets execute cleanly.
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/BitStuffer2.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/BitStuffer2.cpp

#include "third_party/lerc/LercLib/BitStuffer2.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

namespace lerc {
namespace {

using ::LercNS::BitStuffer2;
using ::LercNS::Byte;
using ::testing::ElementsAreArray;

int HeaderBytesForCount(std::size_t count) {
  return count < 256 ? 2 : (count < 65536 ? 3 : 5);
}

std::vector<std::pair<unsigned int, unsigned int>> BuildSortedPairs(
    const std::vector<unsigned int>& values) {
  std::vector<std::pair<unsigned int, unsigned int>> sorted;
  sorted.reserve(values.size());
  for (unsigned int i = 0; i < values.size(); ++i) {
    sorted.emplace_back(values[i], i);
  }
  std::ranges::sort(sorted, [](const auto& a, const auto& b) {
    return a.first < b.first || (a.first == b.first && a.second < b.second);
  });
  return sorted;
}

TEST(BitStuffer2Test, EncodeSimpleRejectsInvalidInputs) {
  const BitStuffer2 bs;
  std::array<Byte, 64> buf{};
  Byte* dst = buf.data();

  EXPECT_FALSE(bs.EncodeSimple(nullptr, {1, 2}, 4));
  EXPECT_FALSE(bs.EncodeSimple(&dst, {}, 4));
  // Values >= 2^31 require 32 bits, which EncodeSimple rejects (numBits >= 32).
  EXPECT_FALSE(bs.EncodeSimple(&dst, {0, 0x80000000U}, 4));
  EXPECT_EQ(buf.data(), dst);
}

TEST(BitStuffer2Test, EncodeDecodeSimpleZeroBitsAndVariedElementCounts) {
  const BitStuffer2 bs;

  // 1. All zeros: numBits == 0 writes header only; Decode succeeds leaving
  //    caller-provided zeroed vector intact.
  const std::vector<unsigned int> zeros(12, 0U);
  const unsigned int needed_zero =
      BitStuffer2::ComputeNumBytesNeededSimple(zeros.size(), 0U);
  EXPECT_EQ(2U, needed_zero);
  std::vector<Byte> zero_blob(needed_zero, 0xCC);
  Byte* zero_dst = zero_blob.data();
  ASSERT_TRUE(bs.EncodeSimple(&zero_dst, zeros, 4));
  EXPECT_EQ(needed_zero,
            static_cast<unsigned int>(zero_dst - zero_blob.data()));

  std::vector<unsigned int> decoded(zeros.size(), 0U);
  const Byte* zero_src = zero_blob.data();
  std::size_t rem = zero_blob.size();
  ASSERT_TRUE(bs.Decode(&zero_src, rem, decoded, zeros.size(), 4));
  EXPECT_EQ(0U, rem);
  EXPECT_THAT(decoded, ElementsAreArray(zeros));

  // 2. Round-trip small (<256), medium (256..65535), and large (65536) counts
  //    plus varied bit-widths (1, 5, 13, 31) for both version 2 and version 4.
  const std::vector<std::size_t> counts = {7, 256, 65536};
  const std::vector<unsigned int> masks = {0x1U, 0x1FU, 0x1FFFU, 0x7FFFFFFFU};

  for (int version : {2, 4}) {
    for (std::size_t count : counts) {
      for (unsigned int mask : masks) {
        if (count == 65536 && mask != 0x1FU) {
          continue;  // Cover 4-byte numElements cleanly without redundant work.
        }
        std::vector<unsigned int> input(count);
        for (std::size_t i = 0; i < count; ++i) {
          input[i] = (mask == 0x7FFFFFFFU && i == count - 1)
                         ? 0x7FFFFFFFU
                         : (static_cast<unsigned int>(i * 2654435761U) & mask);
        }
        const unsigned int max_elem = *std::ranges::max_element(input);
        const unsigned int needed =
            BitStuffer2::ComputeNumBytesNeededSimple(count, max_elem);

        // Align payload start to 4 bytes for version < 3 so the upstream
        // unsigned int* cast in BitStuff_Before_Lerc2v3 stays aligned.
        const std::size_t hdr_bytes = HeaderBytesForCount(count);
        const std::size_t align_pad = (4 - (hdr_bytes % 4)) % 4;
        std::vector<uint32_t> aligned_storage((align_pad + needed + 8) / 4 + 1,
                                              0U);
        Byte* base =
            reinterpret_cast<Byte*>(aligned_storage.data()) + align_pad;
        Byte* dst = base;
        ASSERT_TRUE(bs.EncodeSimple(&dst, input, version))
            << "version=" << version << " count=" << count;
        const std::size_t written = static_cast<std::size_t>(dst - base);
        EXPECT_EQ(needed, written);

        std::vector<unsigned int> out(count, 0xDEADBEEFU);
        const Byte* src = base;
        std::size_t remaining = written;
        ASSERT_TRUE(bs.Decode(&src, remaining, out, count, version))
            << "version=" << version << " count=" << count;
        EXPECT_EQ(0U, remaining);
        EXPECT_THAT(out, ElementsAreArray(input));
      }
    }
  }
}

TEST(BitStuffer2Test,
     EncodeLutRejectsInvalidInputsAndHandlesBothCostDecisions) {
  const BitStuffer2 bs;
  std::array<Byte, 512> buf{};
  Byte* dst = buf.data();

  // 1. Null pointer, empty, non-zero min, 32-bit, >=255 distinct entries.
  //    All-zero input is excluded from EncodeLut because BitStuffer2::EncodeLut
  //    reads m_tmpLutVec.back() before checking nLut < 1 (aborting under
  //    hardened libc++); ComputeNumBytesNeededLut sets doLut = false instead.
  const auto valid_sorted = BuildSortedPairs({0, 5, 0, 5, 10, 0});
  EXPECT_FALSE(bs.EncodeLut(nullptr, valid_sorted, 4));
  EXPECT_FALSE(bs.EncodeLut(&dst, {}, 4));
  EXPECT_FALSE(bs.EncodeLut(&dst, BuildSortedPairs({1, 2, 3}), 4));
  EXPECT_FALSE(bs.EncodeLut(&dst, BuildSortedPairs({0, 0x80000000U}), 4));

  bool zero_do_lut = true;
  EXPECT_GT(BitStuffer2::ComputeNumBytesNeededLut(BuildSortedPairs({0, 0, 0}),
                                                  zero_do_lut),
            0U);
  EXPECT_FALSE(zero_do_lut);

  std::vector<unsigned int> too_many_unique(256);
  for (unsigned int i = 0; i < 256; ++i) {
    too_many_unique[i] = i;
  }
  EXPECT_FALSE(bs.EncodeLut(&dst, BuildSortedPairs(too_many_unique), 4));

  // 2. ComputeNumBytesNeededLut flips doLut false when values vary too much and
  //    true when values repeat heavily.
  bool do_lut = true;
  EXPECT_GT(BitStuffer2::ComputeNumBytesNeededLut(
                BuildSortedPairs({0, 1, 2, 3}), do_lut),
            0U);
  EXPECT_FALSE(do_lut);

  std::vector<unsigned int> repetitive(64, 0U);
  for (std::size_t i = 0; i < repetitive.size(); ++i) {
    repetitive[i] = static_cast<unsigned int>((i % 4) * 1000000U);
  }
  const unsigned int lut_bytes = BitStuffer2::ComputeNumBytesNeededLut(
      BuildSortedPairs(repetitive), do_lut);
  EXPECT_TRUE(do_lut);

  std::vector<Byte> lut_blob(lut_bytes + 8, 0U);
  Byte* lut_dst = lut_blob.data();
  ASSERT_TRUE(bs.EncodeLut(&lut_dst, BuildSortedPairs(repetitive), 4));
  EXPECT_EQ(lut_bytes, static_cast<unsigned int>(lut_dst - lut_blob.data()));

  std::vector<unsigned int> out(repetitive.size(), 999U);
  const Byte* lut_src = lut_blob.data();
  std::size_t remaining = lut_bytes;
  ASSERT_TRUE(bs.Decode(&lut_src, remaining, out, repetitive.size(), 4));
  EXPECT_EQ(0U, remaining);
  EXPECT_THAT(out, ElementsAreArray(repetitive));
}

TEST(BitStuffer2Test, EncodeDecodeLutVersion2AndMediumCountRoundTrip) {
  const BitStuffer2 bs;

  // Version 2 LUT round-trip with 4 non-zero 8-bit entries (30, 60, 120, 240)
  // keeps the LUT bit-stuffed payload exactly 4 bytes long so both
  // BitStuff_Before_Lerc2v3 calls start 4-byte aligned.
  std::vector<unsigned int> v2_vals(40, 0U);
  constexpr unsigned int kPalette[5] = {0U, 30U, 60U, 120U, 240U};
  for (std::size_t i = 0; i < v2_vals.size(); ++i) {
    v2_vals[i] = kPalette[i % 5];
  }
  bool do_lut = false;
  const unsigned int needed =
      BitStuffer2::ComputeNumBytesNeededLut(BuildSortedPairs(v2_vals), do_lut);
  ASSERT_TRUE(do_lut);

  // Header is 1 (numBitsByte) + 1 (nElem) + 1 (nLut+1) = 3 bytes -> pad 1 byte.
  std::array<uint32_t, 64> aligned_buf{};
  Byte* base = reinterpret_cast<Byte*>(aligned_buf.data()) + 1;
  Byte* dst = base;
  ASSERT_TRUE(bs.EncodeLut(&dst, BuildSortedPairs(v2_vals), 2));
  const std::size_t written = static_cast<std::size_t>(dst - base);
  EXPECT_EQ(needed, written);

  std::vector<unsigned int> out(v2_vals.size(), 0xFFFFU);
  const Byte* src = base;
  std::size_t rem = written;
  ASSERT_TRUE(bs.Decode(&src, rem, out, v2_vals.size(), 2));
  EXPECT_EQ(0U, rem);
  EXPECT_THAT(out, ElementsAreArray(v2_vals));

  // Corrupt the last index byte in the v2 LUT stream so an index exceeds
  // m_tmpLutVec.size() (checking the v2 out-of-range LUT guard).
  std::vector<Byte> bad_v2(base, base + written);
  bad_v2.back() = 0xFF;
  src = bad_v2.data();
  rem = bad_v2.size();
  EXPECT_FALSE(bs.Decode(&src, rem, out, v2_vals.size(), 2));

  // Also test version 4 LUT with 300 elements (2-byte numElem header).
  std::vector<unsigned int> medium(300);
  for (std::size_t i = 0; i < medium.size(); ++i) {
    medium[i] = (i % 3) * 50000U;
  }
  const unsigned int med_needed =
      BitStuffer2::ComputeNumBytesNeededLut(BuildSortedPairs(medium), do_lut);
  ASSERT_TRUE(do_lut);
  std::vector<Byte> med_blob(med_needed + 8, 0U);
  Byte* med_dst = med_blob.data();
  ASSERT_TRUE(bs.EncodeLut(&med_dst, BuildSortedPairs(medium), 4));
  EXPECT_EQ(med_needed, static_cast<unsigned int>(med_dst - med_blob.data()));

  std::vector<unsigned int> med_out(medium.size(), 1U);
  src = med_blob.data();
  rem = med_needed;
  ASSERT_TRUE(bs.Decode(&src, rem, med_out, medium.size(), 4));
  EXPECT_EQ(0U, rem);
  EXPECT_THAT(med_out, ElementsAreArray(medium));
}

TEST(BitStuffer2Test, DecodeRejectsMalformedHeadersAndAllTruncations) {
  const BitStuffer2 bs;
  std::vector<unsigned int> out(32, 0U);
  std::size_t rem = 0;
  const Byte dummy = 0;
  const Byte* src = &dummy;

  // 1. Null cursor or empty remaining bytes.
  EXPECT_FALSE(bs.Decode(nullptr, rem, out, 32, 4));
  EXPECT_FALSE(bs.Decode(&src, rem, out, 32, 4));

  // 2. Truncated numElements or numElements > maxElementCount.
  constexpr auto kShortCount = std::to_array<Byte>({0x03});  // expects 4B count
  src = kShortCount.data();
  rem = kShortCount.size();
  EXPECT_FALSE(bs.Decode(&src, rem, out, 32, 4));

  constexpr auto kOverMax =
      std::to_array<Byte>({0x80, 10});  // 1B count = 10 > max=5
  src = kOverMax.data();
  rem = kOverMax.size();
  EXPECT_FALSE(bs.Decode(&src, rem, out, 5, 4));

  // 3. Corrupted LUT headers: numBits == 0, truncated before nLutByte, nLut ==
  // 0.
  constexpr auto kLutZeroBits = std::to_array<Byte>({0xA0, 4, 2, 1});
  src = kLutZeroBits.data();
  rem = kLutZeroBits.size();
  EXPECT_FALSE(bs.Decode(&src, rem, out, 32, 2));

  constexpr auto kLutTruncBeforeNLut = std::to_array<Byte>({0xA3, 4});
  src = kLutTruncBeforeNLut.data();
  rem = kLutTruncBeforeNLut.size();
  EXPECT_FALSE(bs.Decode(&src, rem, out, 32, 4));

  constexpr auto kLutZeroEntries = std::to_array<Byte>({0xA3, 4, 1, 0, 0});
  src = kLutZeroEntries.data();
  rem = kLutZeroEntries.size();
  EXPECT_FALSE(bs.Decode(&src, rem, out, 32, 4));

  // 4. Truncate valid Simple and LUT payloads at every byte offset (< written)
  //    for both version 2 and version 4.
  const std::vector<unsigned int> simple_vals = {0, 3, 7, 15, 2, 19, 31, 4, 11};
  const std::vector<unsigned int> lut_vals = {0, 500, 0, 1000, 500, 0, 1000, 0};

  for (int version : {2, 4}) {
    std::array<uint32_t, 64> aligned_simple{};
    Byte* s_base = reinterpret_cast<Byte*>(aligned_simple.data()) + 2;
    Byte* s_dst = s_base;
    ASSERT_TRUE(bs.EncodeSimple(&s_dst, simple_vals, version));
    const std::size_t s_len = static_cast<std::size_t>(s_dst - s_base);
    for (std::size_t trunc = 0; trunc < s_len; ++trunc) {
      src = s_base;
      rem = trunc;
      EXPECT_FALSE(bs.Decode(&src, rem, out, simple_vals.size(), version))
          << "simple v" << version << " trunc=" << trunc;
    }

    // Use 4-byte aligned LUT values (nLut=2, numBits=10 -> 3 bytes, test
    // unaligned index in v4 and aligned LUT start in v2 with 4 entries).
    const std::vector<unsigned int> v2_lut_vals = {0, 30, 60, 120, 240,
                                                   0, 30, 60, 120, 240};
    const auto& lv = (version == 2) ? v2_lut_vals : lut_vals;
    std::array<uint32_t, 64> aligned_lut{};
    Byte* l_base = reinterpret_cast<Byte*>(aligned_lut.data()) + 1;
    Byte* l_dst = l_base;
    ASSERT_TRUE(bs.EncodeLut(&l_dst, BuildSortedPairs(lv), version));
    const std::size_t l_len = static_cast<std::size_t>(l_dst - l_base);
    for (std::size_t trunc = 0; trunc < l_len; ++trunc) {
      src = l_base;
      rem = trunc;
      EXPECT_FALSE(bs.Decode(&src, rem, out, lv.size(), version))
          << "lut v" << version << " trunc=" << trunc;
    }
  }
}

}  // namespace
}  // namespace lerc
