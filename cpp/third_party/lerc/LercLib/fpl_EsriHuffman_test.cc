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
// Unit tests for LercNS::fpl_EsriHuffman
// (third_party/lerc/LercLib/fpl_EsriHuffman.cpp).
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/fpl_EsriHuffman.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/fpl_EsriHuffman.cpp

#include "third_party/lerc/LercLib/fpl_EsriHuffman.h"

#include <cstddef>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/lerc/LercLib/Defines.h"
#include "third_party/lerc/LercLib/Huffman.h"
#include "third_party/lerc/google/util/lerc_test_util.h"

// Global-namespace PackBits helpers defined in fpl_EsriHuffman.cpp.
bool decodePackBits(const unsigned char* ptr, std::size_t size,
                    std::size_t expected, unsigned char** output);
long encodePackBits(const unsigned char* ptr, std::size_t size,
                    unsigned char** output);
long getPackBitsSize(const unsigned char* ptr, std::size_t size, long* limit);

namespace lerc {
namespace {

using ::LercNS::fpl_EsriHuffman;
using ::testing::ElementsAreArray;

TEST(FplEsriHuffmanTest, ConstantInputUsesRleMode) {
  const std::vector<char> constant(64, 0x2A);
  EXPECT_EQ(
      6, fpl_EsriHuffman::getCompressedSize(constant.data(), constant.size()));

  unsigned char* raw_encoded = nullptr;
  const int enc_len = fpl_EsriHuffman::EncodeHuffman(
      constant.data(), constant.size(), &raw_encoded, /*use_rle=*/true);
  OwnedMallocBytes encoded(raw_encoded);
  ASSERT_EQ(6, enc_len);
  EXPECT_EQ(1, encoded.get()[0]);  // HUFFMAN_RLE
  EXPECT_EQ(0x2A, encoded.get()[1]);

  std::size_t rem = constant.size();
  unsigned char* raw_decoded = nullptr;
  const bool ok = fpl_EsriHuffman::DecodeHuffman(
      encoded.get(), static_cast<std::size_t>(enc_len), rem, &raw_decoded);
  OwnedMallocBytes decoded(raw_decoded);
  ASSERT_TRUE(ok);
  EXPECT_THAT(std::vector<char>(decoded.get(), decoded.get() + constant.size()),
              ElementsAreArray(constant));
}

TEST(FplEsriHuffmanTest, IncompressibleRunsSelectPackBitsOrNoEncodingMode) {
  // 256 byte values each repeated 16 times (4096B total): uniform histogram
  // makes Huffman >= input_len, while PackBits compacts runs cleanly.
  std::vector<char> run_input;
  run_input.reserve(256 * 16);
  for (int val = 0; val < 256; ++val) {
    run_input.insert(run_input.end(), 16, static_cast<char>(val));
  }
  EXPECT_EQ(
      static_cast<int>(run_input.size()) + 1,
      fpl_EsriHuffman::getCompressedSize(run_input.data(), run_input.size()));

  // 1. use_rle=true selects HUFFMAN_PACKBITS (flag 3).
  unsigned char* raw_pb = nullptr;
  const int pb_len = fpl_EsriHuffman::EncodeHuffman(
      run_input.data(), run_input.size(), &raw_pb, /*use_rle=*/true);
  OwnedMallocBytes pb_encoded(raw_pb);
  ASSERT_GT(pb_len, 0);
  ASSERT_LT(pb_len, static_cast<int>(run_input.size()));
  EXPECT_EQ(3, pb_encoded.get()[0]);  // HUFFMAN_PACKBITS

  std::size_t rem_pb = run_input.size();
  unsigned char* raw_pb_out = nullptr;
  const bool pb_ok = fpl_EsriHuffman::DecodeHuffman(
      pb_encoded.get(), static_cast<std::size_t>(pb_len), rem_pb, &raw_pb_out);
  OwnedMallocBytes pb_decoded(raw_pb_out);
  ASSERT_TRUE(pb_ok);
  EXPECT_THAT(
      std::vector<char>(pb_decoded.get(), pb_decoded.get() + run_input.size()),
      ElementsAreArray(run_input));

  // 2. use_rle=false falls back to HUFFMAN_NO_ENCODING (flag 2).
  unsigned char* raw_asis = nullptr;
  const int asis_len = fpl_EsriHuffman::EncodeHuffman(
      run_input.data(), run_input.size(), &raw_asis, /*use_rle=*/false);
  OwnedMallocBytes asis_encoded(raw_asis);
  ASSERT_EQ(static_cast<int>(run_input.size()) + 1, asis_len);
  EXPECT_EQ(2, asis_encoded.get()[0]);  // HUFFMAN_NO_ENCODING

  std::size_t rem_asis = run_input.size();
  unsigned char* raw_asis_out = nullptr;
  const bool asis_ok = fpl_EsriHuffman::DecodeHuffman(
      asis_encoded.get(), static_cast<std::size_t>(asis_len), rem_asis,
      &raw_asis_out);
  OwnedMallocBytes asis_decoded(raw_asis_out);
  ASSERT_TRUE(asis_ok);
  EXPECT_THAT(std::vector<char>(asis_decoded.get(),
                                asis_decoded.get() + run_input.size()),
              ElementsAreArray(run_input));
}

TEST(FplEsriHuffmanTest, SkewedBytesSelectNormalHuffmanAndDecodeRejectsErrors) {
  // Alternating bytes defeat PackBits runs while skew yields strong Huffman.
  std::vector<char> skewed(512);
  for (std::size_t i = 0; i < skewed.size(); ++i) {
    skewed[i] = static_cast<char>((i % 10 == 0) ? 1 : ((i % 25 == 0) ? 2 : 0));
  }
  const int est_len =
      fpl_EsriHuffman::getCompressedSize(skewed.data(), skewed.size());
  EXPECT_GT(est_len, 0);
  EXPECT_LT(est_len, static_cast<int>(skewed.size()));

  unsigned char* raw_huff = nullptr;
  const int huff_len = fpl_EsriHuffman::EncodeHuffman(
      skewed.data(), skewed.size(), &raw_huff, /*use_rle=*/true);
  OwnedMallocBytes huff_encoded(raw_huff);
  ASSERT_GT(huff_len, 0);
  ASSERT_EQ(est_len, huff_len);
  EXPECT_EQ(0, huff_encoded.get()[0]);  // HUFFMAN_NORMAL

  std::size_t rem = skewed.size();
  unsigned char* raw_out = nullptr;
  const bool dec_ok = fpl_EsriHuffman::DecodeHuffman(
      huff_encoded.get(), static_cast<std::size_t>(huff_len), rem, &raw_out);
  OwnedMallocBytes decoded(raw_out);
  ASSERT_TRUE(dec_ok);
  EXPECT_THAT(std::vector<char>(decoded.get(), decoded.get() + skewed.size()),
              ElementsAreArray(skewed));

  // DecodeHuffman null input check and truncated HUFFMAN_NORMAL stream checks.
  std::size_t bad_rem = 10;
  unsigned char* raw_err = nullptr;
  EXPECT_FALSE(fpl_EsriHuffman::DecodeHuffman(nullptr, 10, bad_rem, &raw_err));

  bad_rem = 4;  // < 16B Huffman code table header
  const bool short_tbl_ok =
      fpl_EsriHuffman::DecodeHuffman(huff_encoded.get(), 4, bad_rem, &raw_err);
  OwnedMallocBytes err_guard1(raw_err);
  EXPECT_FALSE(short_tbl_ok);

  // Truncated bit payload right after the code table triggers DecodeOneValue
  // failure without overreading the stream buffer.
  LercNS::Huffman probe;
  const LercNS::Byte* tbl_ptr = huff_encoded.get() + 1;
  std::size_t probe_rem = static_cast<std::size_t>(huff_len - 1);
  ASSERT_TRUE(probe.ReadCodeTable(&tbl_ptr, probe_rem, 5));
  const std::size_t tbl_bytes =
      static_cast<std::size_t>(tbl_ptr - huff_encoded.get());
  std::vector<unsigned char> trunc_payload(huff_encoded.get(),
                                           huff_encoded.get() + tbl_bytes + 2);
  raw_err = nullptr;
  bad_rem = trunc_payload.size();
  const bool short_pay_ok = fpl_EsriHuffman::DecodeHuffman(
      trunc_payload.data(), trunc_payload.size(), bad_rem, &raw_err);
  OwnedMallocBytes err_guard2(raw_err);
  EXPECT_FALSE(short_pay_ok);
}

TEST(FplEsriHuffmanTest, PackBitsHelpersCoverMaxRunSplitsPreallocAndFailures) {
  // Craft > 128 distinct bytes followed by > 128 repeated bytes and small tails
  // to cover literal_count==128 and repeat_count==128 flushes in PackBits.
  std::vector<unsigned char> data;
  for (int i = 0; i < 140; ++i) {
    data.push_back(static_cast<unsigned char>((i * 3) & 0xFF));
  }
  data.insert(data.end(), 150, 0x55);
  data.push_back(0x12);
  data.push_back(0x34);

  long no_limit = 0;
  const long predicted = getPackBitsSize(data.data(), data.size(), &no_limit);
  ASSERT_GT(predicted, 0);

  long small_limit = 10;
  EXPECT_EQ(-1, getPackBitsSize(data.data(), data.size(), &small_limit));

  // Auto-allocated output buffer (nullptr) and caller-provided buffer.
  unsigned char* raw_packed = nullptr;
  const long packed_len = encodePackBits(data.data(), data.size(), &raw_packed);
  OwnedMallocBytes packed_owned(raw_packed);
  EXPECT_EQ(predicted, packed_len);

  std::vector<unsigned char> caller_packed(data.size() * 2 + 1, 0);
  unsigned char* caller_ptr = caller_packed.data();
  EXPECT_EQ(packed_len, encodePackBits(data.data(), data.size(), &caller_ptr));

  unsigned char* raw_unpacked = nullptr;
  const bool dec_ok =
      decodePackBits(packed_owned.get(), static_cast<std::size_t>(packed_len),
                     data.size(), &raw_unpacked);
  OwnedMallocBytes unpacked_owned(raw_unpacked);
  ASSERT_TRUE(dec_ok);
  EXPECT_THAT(std::vector<unsigned char>(unpacked_owned.get(),
                                         unpacked_owned.get() + data.size()),
              ElementsAreArray(data));

  std::vector<unsigned char> caller_out(data.size(), 0);
  unsigned char* caller_out_ptr = caller_out.data();
  ASSERT_TRUE(decodePackBits(packed_owned.get(),
                             static_cast<std::size_t>(packed_len), data.size(),
                             &caller_out_ptr));
  EXPECT_THAT(caller_out, ElementsAreArray(data));

  // Bounds overflow checks inside decodePackBits (literal and repeat counts).
  unsigned char* raw_bad = nullptr;
  const unsigned char literal_overflow[] = {3, 1, 2, 3, 4};  // 4 bytes
  const bool lit_ok =
      decodePackBits(literal_overflow, sizeof(literal_overflow), 2, &raw_bad);
  OwnedMallocBytes bad_guard1(raw_bad);
  EXPECT_FALSE(lit_ok);

  raw_bad = nullptr;
  const unsigned char repeat_overflow[] = {130, 0xAA};  // 4 repeated bytes
  const bool rep_ok =
      decodePackBits(repeat_overflow, sizeof(repeat_overflow), 2, &raw_bad);
  OwnedMallocBytes bad_guard2(raw_bad);
  EXPECT_FALSE(rep_ok);
}

}  // namespace
}  // namespace lerc
