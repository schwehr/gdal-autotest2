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
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"
#include "third_party/lerc/LercLib/Defines.h"
#include "third_party/lerc/LercLib/Huffman.h"
#include "third_party/lerc/google/util/lerc_test_util.h"

// Global-namespace PackBits helpers defined in fpl_EsriHuffman.cpp (using
// upstream's `long` ABI signature).
// Note: DecodeHuffman's HUFFMAN_PACKBITS branch calls lerc_assert; negative
// PACKBITS decode cases are intentionally omitted here because this target
// compiles without -fexceptions (lerc_assert coverage lives in
// fpl_Lerc2Ext_test).
bool decodePackBits(const unsigned char* ptr, std::size_t size,
                    std::size_t expected, unsigned char** output);
long encodePackBits(const unsigned char* ptr,  // NOLINT(runtime/int)
                    std::size_t size, unsigned char** output);
long getPackBitsSize(const unsigned char* ptr,  // NOLINT(runtime/int)
                     std::size_t size,
                     long* limit);  // NOLINT(runtime/int)

namespace lerc {
namespace {

using ::LercNS::fpl_EsriHuffman;
using ::testing::ElementsAreArray;

// Mirrors HuffmanFirstByte in fpl_EsriHuffman.cpp.
enum class HuffmanMode : unsigned char {
  kNormal = 0,
  kRle = 1,
  kNoEncoding = 2,
  kPackBits = 3,
};

[[nodiscard]] int EncodeAndVerifyRoundTrip(
    absl::Span<const char> input, bool use_rle, HuffmanMode expected_mode,
    OwnedMallocBytes* out_encoded = nullptr) {
  unsigned char* raw_encoded = nullptr;
  const int enc_len = fpl_EsriHuffman::EncodeHuffman(input.data(), input.size(),
                                                     &raw_encoded, use_rle);
  OwnedMallocBytes encoded(raw_encoded);
  if (enc_len <= 0 || encoded == nullptr) {
    ADD_FAILURE() << "EncodeHuffman failed, enc_len=" << enc_len;
    return -1;
  }
  EXPECT_EQ(static_cast<unsigned char>(expected_mode), encoded.get()[0]);

  std::size_t rem = input.size();
  unsigned char* raw_decoded = nullptr;
  const bool ok = fpl_EsriHuffman::DecodeHuffman(
      encoded.get(), static_cast<std::size_t>(enc_len), rem, &raw_decoded);
  OwnedMallocBytes decoded(raw_decoded);
  EXPECT_TRUE(ok);
  if (ok && decoded != nullptr) {
    EXPECT_THAT(std::vector<char>(decoded.get(), decoded.get() + input.size()),
                ElementsAreArray(input));
  }
  if (out_encoded != nullptr) {
    *out_encoded = std::move(encoded);
  }
  return enc_len;
}

TEST(FplEsriHuffmanTest, ConstantInputUsesRleMode) {
  const std::vector<char> constant(64, 0x2A);
  EXPECT_EQ(
      6, fpl_EsriHuffman::getCompressedSize(constant.data(), constant.size()));

  OwnedMallocBytes encoded;
  const int enc_len = EncodeAndVerifyRoundTrip(constant, /*use_rle=*/true,
                                               HuffmanMode::kRle, &encoded);
  ASSERT_EQ(6, enc_len);
  EXPECT_EQ(0x2A, encoded.get()[1]);
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
  const int pb_len = EncodeAndVerifyRoundTrip(run_input, /*use_rle=*/true,
                                              HuffmanMode::kPackBits);
  ASSERT_GT(pb_len, 0);
  EXPECT_LT(pb_len, static_cast<int>(run_input.size()));

  // 2. use_rle=false falls back to HUFFMAN_NO_ENCODING (flag 2).
  const int asis_len = EncodeAndVerifyRoundTrip(run_input, /*use_rle=*/false,
                                                HuffmanMode::kNoEncoding);
  EXPECT_EQ(static_cast<int>(run_input.size()) + 1, asis_len);
}

TEST(FplEsriHuffmanTest, SkewedBytesSelectNormalHuffmanAndDecodeRejectsErrors) {
  // 9-zero runs keep getPackBitsSize exceeding limit = min(numBytes, len) so
  // it returns -1 and HUFFMAN_NORMAL wins.
  std::vector<char> skewed(512);
  for (std::size_t i = 0; i < skewed.size(); ++i) {
    skewed[i] = static_cast<char>((i % 10 == 0) ? 1 : ((i % 25 == 0) ? 2 : 0));
  }
  const int est_len =
      fpl_EsriHuffman::getCompressedSize(skewed.data(), skewed.size());
  EXPECT_GT(est_len, 0);
  EXPECT_LT(est_len, static_cast<int>(skewed.size()));

  OwnedMallocBytes huff_encoded;
  const int huff_len = EncodeAndVerifyRoundTrip(
      skewed, /*use_rle=*/true, HuffmanMode::kNormal, &huff_encoded);
  ASSERT_EQ(est_len, huff_len);

  // DecodeHuffman null input check and truncated HUFFMAN_NORMAL stream checks.
  // TODO(schwehr): Report upstream: DecodeHuffman HUFFMAN_NORMAL ignores
  // inCount and bounds ReadCodeTable/DecodeOneValue reads by
  // nBytesRemainingInOut (expected output size), not subtracting the flag byte.
  std::size_t bad_rem = 10;
  unsigned char* raw_err = nullptr;
  EXPECT_FALSE(fpl_EsriHuffman::DecodeHuffman(nullptr, 10, bad_rem, &raw_err));

  bad_rem = 4;  // < 16B Huffman code table header
  const bool short_tbl_ok =
      fpl_EsriHuffman::DecodeHuffman(huff_encoded.get(), 4, bad_rem, &raw_err);
  OwnedMallocBytes err_guard1(raw_err);
  EXPECT_FALSE(short_tbl_ok);

  // Truncated bit payload right after the code table triggers DecodeOneValue
  // failure without overreading trunc_payload. DecodeHuffman always passes
  // lerc2Version=5 to Huffman::ReadCodeTable. Keep bad_rem == trunc_payload
  // size (<= tbl_bytes + 3) so ReadCodeTable/DecodeOneValue cannot overread.
  LercNS::Huffman probe;
  const LercNS::Byte* tbl_ptr = huff_encoded.get() + 1;
  std::size_t probe_rem = static_cast<std::size_t>(huff_len - 1);
  ASSERT_TRUE(probe.ReadCodeTable(&tbl_ptr, probe_rem, /*lerc2Version=*/5));
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

  using PackBitsLong = decltype(encodePackBits(nullptr, 0, nullptr));
  PackBitsLong no_limit = 0;
  const PackBitsLong predicted =
      getPackBitsSize(data.data(), data.size(), &no_limit);
  ASSERT_GT(predicted, 0);

  PackBitsLong small_limit = 10;
  EXPECT_EQ(-1, getPackBitsSize(data.data(), data.size(), &small_limit));

  // Auto-allocated output buffer (nullptr) and caller-provided buffer.
  unsigned char* raw_packed = nullptr;
  const PackBitsLong packed_len =
      encodePackBits(data.data(), data.size(), &raw_packed);
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
