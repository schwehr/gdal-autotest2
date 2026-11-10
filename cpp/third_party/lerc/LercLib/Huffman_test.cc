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
// Unit tests for LercNS::Huffman (third_party/lerc/LercLib/Huffman.cpp).
//
// Note: DecodeOneValue reads 4-8 bytes ahead via memcpy, so test bitstreams
// always include >= 8 trailing zero padding bytes counted in nBytesRemaining.
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Huffman.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Huffman.cpp

#include "third_party/lerc/LercLib/Huffman.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/lerc/LercLib/Defines.h"
#include "third_party/lerc/google/util/lerc_test_util.h"

namespace lerc {
namespace {

using ::LercNS::Byte;
using ::LercNS::Huffman;
using ::testing::ElementsAreArray;

// DecodeOneValue reads 4-8B ahead via memcpy, so append at least 8 zero pad
// bytes past any partially written uint32 word.
[[nodiscard]] constexpr std::size_t ReadAheadPaddingBytes(
    int bit_pos) noexcept {
  return (bit_pos > 0 ? sizeof(std::uint32_t) : 0U) + 8U;
}

TEST(HuffmanTest, ComputeCodesAndSizeRejectInvalidHistograms) {
  Huffman huffman;
  int num_bytes = 0;
  double avg_bpp = 0.0;

  // Empty, oversized (>= 2^15), zero-bin, and single-bin histograms fail.
  EXPECT_FALSE(huffman.ComputeCodes({}));
  EXPECT_FALSE(huffman.ComputeCompressedSize({}, num_bytes, avg_bpp));

  std::vector<int> oversized(1U << 15, 1);
  EXPECT_FALSE(huffman.ComputeCodes(oversized));
  EXPECT_FALSE(huffman.ComputeCompressedSize(oversized, num_bytes, avg_bpp));

  std::vector<int> all_zero(16, 0);
  EXPECT_FALSE(huffman.ComputeCodes(all_zero));

  std::vector<int> single_bin(16, 0);
  single_bin[5] = 42;
  EXPECT_FALSE(huffman.ComputeCodes(single_bin));

  // SetCodes validation and empty/oversized/zero-code code tables.
  EXPECT_FALSE(huffman.SetCodes({}));
  EXPECT_FALSE(huffman.SetCodes(
      std::vector<std::pair<std::uint16_t, unsigned int>>(1U << 15, {1, 0})));

  ASSERT_TRUE(huffman.SetCodes({{0, 0}, {0, 0}}));
  std::array<Byte, 64> tmp{};
  Byte* dst = tmp.data();
  int lut_bits = 0;
  EXPECT_FALSE(huffman.WriteCodeTable(&dst, 4));
  EXPECT_FALSE(huffman.BuildTreeFromCodes(lut_bits));
  EXPECT_FALSE(huffman.ComputeCompressedSize({1, 2}, num_bytes, avg_bpp));

  // SetCodes only checks size bounds; WriteCodeTable rejects lengths > 32 via
  // GetRange.
  ASSERT_TRUE(huffman.SetCodes({{33, 0}, {1, 1}}));
  EXPECT_FALSE(huffman.WriteCodeTable(&dst, 4));

  // Reset to a valid table paired with an all-zero histogram triggers
  // numElem==0.
  ASSERT_TRUE(huffman.ComputeCodes({10, 20, 30, 40}));
  EXPECT_FALSE(huffman.ComputeCompressedSize({0, 0, 0, 0}, num_bytes, avg_bpp));
}

TEST(HuffmanTest, ShallowLutAndWrapAroundRoundTripVersions2And4) {
  // Bins at opposite ends [0..2] and [253..255] with a 250-zero middle stretch
  // trigger wrap-around index range compression in GetRange().
  std::vector<int> histo(256, 0);
  histo[0] = 40;
  histo[1] = 25;
  histo[2] = 15;
  histo[253] = 10;
  histo[254] = 6;
  histo[255] = 4;

  Huffman encoder;
  ASSERT_TRUE(encoder.ComputeCodes(histo));

  int predicted_bytes = 0;
  double avg_bpp = 0.0;
  ASSERT_TRUE(encoder.ComputeCompressedSize(histo, predicted_bytes, avg_bpp));
  EXPECT_GT(predicted_bytes, 16);
  EXPECT_GT(avg_bpp, 0.0);

  const auto& codes = encoder.GetCodes();
  ASSERT_EQ(histo.size(), codes.size());

  const std::vector<int> symbols = {0, 1, 2, 253, 254, 255, 0, 255, 1, 254, 2};

  for (int version : {2, 4}) {
    // WriteCodeTable writes 16B header + 2B BitStuffer2 header for <256 bins
    // (18B total); align +2B for version 2 so BitStuff_Before_Lerc2v3 is
    // 4-byte aligned.
    std::vector<uint32_t> storage(256, 0U);
    Byte* base = reinterpret_cast<Byte*>(storage.data()) +
                 AlignPadTo4(16U + BitStuffer2HeaderBytes(histo.size() - 1U));
    Byte* dst = base;
    ASSERT_TRUE(encoder.WriteCodeTable(&dst, version));

    int bit_pos = 0;
    for (int sym : symbols) {
      ASSERT_TRUE(Huffman::PushValue(&dst, bit_pos, codes[sym].second,
                                     codes[sym].first));
    }
    // Include >= 8 bytes of zero tail after packed codes for LUT read-ahead.
    dst += ReadAheadPaddingBytes(bit_pos);
    const std::size_t total_len = static_cast<std::size_t>(dst - base);

    Huffman decoder;
    const Byte* src = base;
    std::size_t rem = total_len;
    ASSERT_TRUE(decoder.ReadCodeTable(&src, rem, version));

    int num_bits_lut = 0;
    ASSERT_TRUE(decoder.BuildTreeFromCodes(num_bits_lut));
    EXPECT_EQ(5, num_bits_lut);

    std::vector<int> decoded;
    decoded.reserve(symbols.size());
    int read_bit_pos = 0;
    for (std::size_t i = 0; i < symbols.size(); ++i) {
      int val = -1;
      ASSERT_TRUE(
          decoder.DecodeOneValue(&src, rem, read_bit_pos, num_bits_lut, val))
          << "version=" << version << " i=" << i;
      decoded.push_back(val);
    }
    EXPECT_THAT(decoded, ElementsAreArray(symbols));

    decoder.Clear();
    int ignored_lut = 0;
    EXPECT_FALSE(decoder.BuildTreeFromCodes(ignored_lut));
  }
}

TEST(HuffmanTest, DeepTreeHistogramUsesFallbackTreeAndWordCrossingDecode) {
  // 20-symbol Fibonacci counts yield code lengths up to 19 bits (> 12-bit LUT),
  // exercising BuildTreeFromCodes tree construction, ClearTree() reuse, and
  // DecodeOneValue tree traversal across 32-bit word boundaries.
  std::vector<int> histo(20, 0);
  int a = 1;
  int b = 1;
  for (int i = 0; i < 20; ++i) {
    histo[i] = a;
    int next = a + b;
    a = b;
    b = next;
  }

  Huffman encoder;
  ASSERT_TRUE(encoder.ComputeCodes(histo));
  const auto& codes = encoder.GetCodes();
  ASSERT_EQ(20U, codes.size());
  EXPECT_GT(codes[0].first, 12);

  int predicted_bytes = 0;
  double avg_bpp = 0.0;
  ASSERT_TRUE(encoder.ComputeCompressedSize(histo, predicted_bytes, avg_bpp));
  EXPECT_GT(predicted_bytes, 0);

  // Mix long codes (deep tree) and short codes so decoding crosses both LUT
  // slots and tree paths at many bitPos offsets (including bitPos == 31 wrap).
  const std::vector<int> symbols = {0,  1, 2, 3,  19, 0,  1, 18, 17, 0, 1, 2,
                                    16, 4, 5, 19, 0,  19, 1, 19, 2,  0, 3, 19};

  std::vector<Byte> stream(4096, 0U);
  Byte* dst = stream.data();
  ASSERT_TRUE(encoder.WriteCodeTable(&dst, 4));
  const std::size_t table_len = static_cast<std::size_t>(dst - stream.data());

  int bit_pos = 0;
  for (int sym : symbols) {
    ASSERT_TRUE(
        Huffman::PushValue(&dst, bit_pos, codes[sym].second, codes[sym].first));
  }
  dst += ReadAheadPaddingBytes(bit_pos);
  const std::size_t total_len = static_cast<std::size_t>(dst - stream.data());

  Huffman decoder;
  const Byte* src = stream.data();
  std::size_t rem = total_len;
  ASSERT_TRUE(decoder.ReadCodeTable(&src, rem, 4));

  int num_bits_lut = 0;
  ASSERT_TRUE(decoder.BuildTreeFromCodes(num_bits_lut));
  EXPECT_EQ(12, num_bits_lut);

  // Re-building the tree exercises ClearTree() cleanup on existing m_root.
  ASSERT_TRUE(decoder.BuildTreeFromCodes(num_bits_lut));
  EXPECT_EQ(12, num_bits_lut);

  std::vector<int> decoded;
  decoded.reserve(symbols.size());
  int read_bit_pos = 0;
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    int val = -1;
    ASSERT_TRUE(
        decoder.DecodeOneValue(&src, rem, read_bit_pos, num_bits_lut, val))
        << "i=" << i;
    decoded.push_back(val);
  }
  EXPECT_THAT(decoded, ElementsAreArray(symbols));

  // Byte-by-byte truncation of the serialized code table fails cleanly.
  for (std::size_t trunc = 0; trunc < table_len; ++trunc) {
    Huffman trunc_dec;
    const Byte* t_src = stream.data();
    std::size_t t_rem = trunc;
    EXPECT_FALSE(trunc_dec.ReadCodeTable(&t_src, t_rem, 4))
        << "trunc=" << trunc;
  }
}

TEST(HuffmanTest, ReadCodeTablePushAndDecodeRejectMalformedInputs) {
  Huffman huffman;
  const Byte* null_ptr = nullptr;
  std::size_t rem = 16;

  EXPECT_FALSE(huffman.WriteCodeTable(nullptr, 4));
  EXPECT_FALSE(huffman.ReadCodeTable(nullptr, rem, 4));
  EXPECT_FALSE(huffman.ReadCodeTable(&null_ptr, rem, 4));

  auto make_header = [](int ver, int size, int i0, int i1) {
    std::array<Byte, 24> buf{};
    const int hdr[4] = {ver, size, i0, i1};
    std::memcpy(buf.data(), hdr, sizeof(hdr));
    return buf;
  };

  const std::array<std::array<Byte, 24>, 5> bad_headers = {{
      make_header(1, 4, 0, 4),      // version < 2
      make_header(4, 4, 3, 3),      // i0 >= i1
      make_header(4, 4, -1, 2),     // i0 < 0
      make_header(4, 32769, 0, 4),  // size > m_maxHistoSize
      make_header(4, 4, 8, 9),      // wrap-around index >= size
  }};
  for (const auto& hdr : bad_headers) {
    const Byte* src = hdr.data();
    std::size_t hdr_rem = hdr.size();
    EXPECT_FALSE(huffman.ReadCodeTable(&src, hdr_rem, 4));
  }

  // PushValue bounds checks and exact 32-bit boundary crossing.
  std::array<Byte, 16> bits_buf{};
  Byte* dst = bits_buf.data();
  Byte* null_dst = nullptr;
  int bit_pos = 0;
  EXPECT_FALSE(Huffman::PushValue(nullptr, bit_pos, 1, 1));
  EXPECT_FALSE(Huffman::PushValue(&null_dst, bit_pos, 1, 1));
  bit_pos = -1;
  EXPECT_FALSE(Huffman::PushValue(&dst, bit_pos, 1, 1));
  bit_pos = 32;
  EXPECT_FALSE(Huffman::PushValue(&dst, bit_pos, 1, 1));
  bit_pos = 0;
  EXPECT_FALSE(Huffman::PushValue(&dst, bit_pos, 1, 0));
  EXPECT_FALSE(Huffman::PushValue(&dst, bit_pos, 1, 33));
  // Push 20 bits then 12 bits (exact 32 wrap), then 28 bits then 12 bits (split
  // across two uint32 words).
  EXPECT_TRUE(Huffman::PushValue(&dst, bit_pos, 0xABCDEU, 20));
  EXPECT_TRUE(Huffman::PushValue(&dst, bit_pos, 0x123U, 12));
  EXPECT_EQ(0, bit_pos);
  EXPECT_TRUE(Huffman::PushValue(&dst, bit_pos, 0x1234567U, 28));
  EXPECT_TRUE(Huffman::PushValue(&dst, bit_pos, 0x89AU, 12));
  EXPECT_EQ(8, bit_pos);
  EXPECT_EQ(bits_buf.data() + 8, dst);
  EXPECT_EQ(0xABCDE123U, ReadPod<std::uint32_t>(bits_buf, 0));
  EXPECT_EQ(0x12345678U, ReadPod<std::uint32_t>(bits_buf, 4));
  EXPECT_EQ(0x9A000000U, ReadPod<std::uint32_t>(bits_buf, 8));

  // DecodeOneValue error checks: null/bad bitPos/short remaining buffer.
  ASSERT_TRUE(huffman.SetCodes({{2, 0U}, {2, 1U}, {2, 2U}}));
  int lut_bits = 0;
  ASSERT_TRUE(huffman.BuildTreeFromCodes(lut_bits));
  std::array<Byte, 12> dec_buf{};
  const Byte* dec_src = dec_buf.data();
  int val = 0;
  int bad_pos = -1;
  rem = dec_buf.size();
  EXPECT_FALSE(huffman.DecodeOneValue(nullptr, rem, bit_pos, lut_bits, val));
  EXPECT_FALSE(huffman.DecodeOneValue(&null_ptr, rem, bit_pos, lut_bits, val));
  EXPECT_FALSE(huffman.DecodeOneValue(&dec_src, rem, bad_pos, lut_bits, val));
  bad_pos = 32;
  EXPECT_FALSE(huffman.DecodeOneValue(&dec_src, rem, bad_pos, lut_bits, val));
  rem = 3;
  bit_pos = 0;
  EXPECT_FALSE(huffman.DecodeOneValue(&dec_src, rem, bit_pos, lut_bits, val));

  // Missing second uint32 read-ahead when 32 - bitPos < numBitsLUT.
  rem = 4;
  bit_pos = 31;
  EXPECT_FALSE(huffman.DecodeOneValue(&dec_src, rem, bit_pos, lut_bits, val));

  // Code bit pattern 0b11 is unassigned in m_decodeLUT and has m_root==nullptr.
  dec_buf.fill(0xFF);
  rem = dec_buf.size();
  bit_pos = 0;
  EXPECT_FALSE(huffman.DecodeOneValue(&dec_src, rem, bit_pos, lut_bits, val));
}

}  // namespace
}  // namespace lerc
