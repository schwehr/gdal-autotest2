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
// Unit tests for GSF beam-array compression and decompression
// (src/gsf_compress.c). Note: _LARGEFILE64_SOURCE is enabled and DISPLAY_SPINNER
// is disabled in third_party/gsf/BUILD.
//
// See also:
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf_compress.c
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf.h

#include "third_party/gsf/src/gsf.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gsf/google/util/gsf_internal.h"

namespace gsf {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

constexpr int kDefaultHandle = 1;
constexpr int kMaxHandle = GSF_MAX_OPEN_FILES;
constexpr unsigned int kShortSubrecordId =
    GSF_SWATH_BATHY_SUBRECORD_SECTOR_NUMBER_ARRAY;
constexpr unsigned int kDoubleSubrecordId =
    GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY;
constexpr uint8_t kVersion1Order0 = 1U << 5;

constexpr uint32_t ReadBe32(std::span<const uint8_t> bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) |
         (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) |
         static_cast<uint32_t>(bytes[3]);
}

constexpr std::array<uint8_t, 4> ToBe32(uint32_t value) {
  return {
      static_cast<uint8_t>((value >> 24) & 0xFFU),
      static_cast<uint8_t>((value >> 16) & 0xFFU),
      static_cast<uint8_t>((value >> 8) & 0xFFU),
      static_cast<uint8_t>(value & 0xFFU),
  };
}

// Appends a 1-byte version/order tag and big-endian 32-bit Simple-16 words
// into a payload buffer formatted for DecodeCompressed* APIs.
std::vector<uint8_t> MakePayload(uint8_t version, uint8_t order,
                                 std::span<const uint32_t> words) {
  std::vector<uint8_t> payload;
  payload.reserve(1 + words.size() * 4);
  payload.push_back(static_cast<uint8_t>((version << 5) | (order & 0x1FU)));
  for (uint32_t word : words) {
    const auto be = ToBe32(word);
    payload.insert(payload.end(), be.begin(), be.end());
  }
  return payload;
}

gsfScaleFactors MakeScaleFactors(unsigned int subrecord_id, double precision,
                                 int offset) {
  gsfScaleFactors sf{};
  EXPECT_EQ(GSF_NORMAL,
            gsfLoadScaleFactor(&sf, subrecord_id, GSF_ENABLE_COMPRESSION,
                               precision, offset));
  return sf;
}

// Wire-format bit-packing specification for the 16 Simple-16 selectors
// (w = 0..15). Each selector packs `count` non-negative integers totaling
// 28 data bits in one 32-bit word.
struct Simple16Spec {
  uint32_t selector;
  std::size_t count;
  std::array<uint8_t, 14> bits;
};

constexpr std::array<Simple16Spec, 16> kSimple16Specs = {{
    {.selector = 0,
     .count = 14,
     .bits = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}},
    {.selector = 1,
     .count = 12,
     .bits = {3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0}},
    {.selector = 2,
     .count = 10,
     .bits = {3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 0, 0, 0, 0}},
    {.selector = 3,
     .count = 9,
     .bits = {4, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0}},
    {.selector = 4,
     .count = 8,
     .bits = {4, 4, 4, 4, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0}},
    {.selector = 5,
     .count = 8,
     .bits = {3, 3, 3, 3, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0}},
    {.selector = 6,
     .count = 7,
     .bits = {4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0}},
    {.selector = 7,
     .count = 6,
     .bits = {5, 5, 5, 5, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0}},
    {.selector = 8,
     .count = 6,
     .bits = {4, 4, 5, 5, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0}},
    {.selector = 9,
     .count = 5,
     .bits = {6, 6, 6, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {.selector = 10,
     .count = 5,
     .bits = {5, 5, 6, 6, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {.selector = 11,
     .count = 4,
     .bits = {7, 7, 7, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {.selector = 12,
     .count = 3,
     .bits = {10, 9, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {.selector = 13,
     .count = 2,
     .bits = {14, 14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {.selector = 14,
     .count = 2,
     .bits = {15, 13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {.selector = 15,
     .count = 1,
     .bits = {28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
}};

uint32_t PackSimple16Word(const Simple16Spec& spec,
                          std::span<const uint32_t> values) {
  uint32_t word = spec.selector << 28;
  uint32_t shift = 0;
  for (std::size_t i = 0; i < spec.count; ++i) {
    const uint32_t mask = (1U << spec.bits[i]) - 1U;
    word |= (values[i] & mask) << shift;
    shift += spec.bits[i];
  }
  return word;
}

TEST(GsfCompressTest, EncodeUnsignedShortRejectsNonPositiveNumBeams) {
  constexpr std::array<int, 3> kInvalidBeamCounts = {0, -1, -12};
  constexpr std::array<unsigned short, 2> kInput = {10, 20};

  for (int num_beams : kInvalidBeamCounts) {
    std::array<uint8_t, 16> encoded{};
    encoded.fill(0xA5U);
    gsfError = GSF_NORMAL;
    absl::Cleanup reset_err = [] { gsfError = GSF_NORMAL; };

    EXPECT_EQ(-1,
              EncodeCompressedUnsignedShortArray(encoded.data(), kInput.data(),
                                                 num_beams, kShortSubrecordId));
    EXPECT_EQ(GSF_INVALID_NUM_BEAMS, gsfIntError());
    EXPECT_TRUE(std::all_of(encoded.begin(), encoded.end(),
                            [](uint8_t b) { return b == 0xA5U; }));
  }
}

TEST(GsfCompressTest, DecodeUnsignedShortRejectsInvalidCompressedSize) {
  constexpr std::array<int, 3> kInvalidSizes = {0, 1, -4};
  constexpr std::array<uint8_t, 4> kDummyPayload = {kVersion1Order0, 0, 0, 0};

  for (int size : kInvalidSizes) {
    unsigned short* out = nullptr;
    gsfError = GSF_NORMAL;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kShortSubrecordId - 1] = 0;
      gsfError = GSF_NORMAL;
    };

    EXPECT_EQ(-1, DecodeCompressedUnsignedShortArray(
                      &out, kDummyPayload.data(), /*num_beams=*/4, size,
                      kShortSubrecordId, kDefaultHandle));
    EXPECT_EQ(GSF_COMPRESSION_FAILED, gsfIntError());
    EXPECT_EQ(nullptr, out);
  }
}

TEST(GsfCompressTest, DecodeUnsignedShortRejectsNonPositiveNumBeams) {
  constexpr std::array<int, 3> kInvalidBeamCounts = {0, -1, -8};
  const std::vector<uint8_t> payload =
      MakePayload(/*version=*/1, /*order=*/0, std::array<uint32_t, 1>{0});

  for (int num_beams : kInvalidBeamCounts) {
    unsigned short* out = nullptr;
    gsfError = GSF_NORMAL;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kShortSubrecordId - 1] = 0;
      gsfError = GSF_NORMAL;
    };

    EXPECT_EQ(-1, DecodeCompressedUnsignedShortArray(
                      &out, payload.data(), num_beams,
                      static_cast<int>(payload.size()), kShortSubrecordId,
                      kDefaultHandle));
    EXPECT_EQ(GSF_INVALID_NUM_BEAMS, gsfIntError());
    EXPECT_EQ(nullptr, out);
  }
}

TEST(GsfCompressTest, DecodeUnsignedShortRejectsUnsupportedVersion) {
  constexpr std::array<unsigned short, 4> kInput = {10, 20, 30, 40};
  std::vector<uint8_t> encoded(5 + 4 * kInput.size());
  const int encoded_bytes = EncodeCompressedUnsignedShortArray(
      encoded.data(), kInput.data(), static_cast<int>(kInput.size()),
      kShortSubrecordId);
  ASSERT_GT(encoded_bytes, 4);

  // Version occupies the upper 3 bits of the first payload byte.
  for (uint8_t bad_version : {uint8_t{2}, uint8_t{7}}) {
    std::vector<uint8_t> payload(encoded.begin() + 4,
                                 encoded.begin() + encoded_bytes);
    payload[0] = static_cast<uint8_t>((bad_version << 5) | (payload[0] & 0x1F));

    unsigned short* out = nullptr;
    gsfError = GSF_NORMAL;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kShortSubrecordId - 1] = 0;
      gsfError = GSF_NORMAL;
    };

    EXPECT_EQ(-1, DecodeCompressedUnsignedShortArray(
                      &out, payload.data(), static_cast<int>(kInput.size()),
                      static_cast<int>(payload.size()), kShortSubrecordId,
                      kDefaultHandle));
    EXPECT_EQ(GSF_COMPRESSION_UNSUPPORTED, gsfIntError());
    // DecodeCompressedUnsignedShortArray allocates `out` before checking the
    // version byte and leaves freeing it to the caller/file table.
    EXPECT_NE(nullptr, out);
  }
}

TEST(GsfCompressTest, DecodeUnsignedShortRejectsTruncatedAndMismatchedPayload) {
  constexpr std::array<unsigned short, 8> kInput = {100, 102, 104, 106,
                                                    108, 110, 112, 114};
  std::vector<uint8_t> encoded(5 + 4 * kInput.size());
  const int encoded_bytes = EncodeCompressedUnsignedShortArray(
      encoded.data(), kInput.data(), static_cast<int>(kInput.size()),
      kShortSubrecordId);
  ASSERT_GT(encoded_bytes, 4);

  const int payload_size = encoded_bytes - 4;
  // 1. Asking for more beams than encoded fails RleDecode length check.
  {
    unsigned short* out = nullptr;
    gsfError = GSF_NORMAL;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kShortSubrecordId - 1] = 0;
      gsfError = GSF_NORMAL;
    };

    EXPECT_EQ(-1, DecodeCompressedUnsignedShortArray(
                      &out, encoded.data() + 4, /*num_beams=*/16, payload_size,
                      kShortSubrecordId, kDefaultHandle));
    EXPECT_EQ(GSF_COMPRESSION_FAILED, gsfIntError());
  }

  // 2. Dropping the trailing 32-bit word leaves n < num_beams.
  {
    ASSERT_GT(payload_size, 5);
    unsigned short* out = nullptr;
    gsfError = GSF_NORMAL;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kShortSubrecordId - 1] = 0;
      gsfError = GSF_NORMAL;
    };

    EXPECT_EQ(-1, DecodeCompressedUnsignedShortArray(
                      &out, encoded.data() + 4, static_cast<int>(kInput.size()),
                      payload_size - 4, kShortSubrecordId, kDefaultHandle));
    EXPECT_EQ(GSF_COMPRESSION_FAILED, gsfIntError());
  }
}

TEST(GsfCompressTest, DecodeUnsignedShortRejectsCorruptRleStreams) {
  // Each case keeps unpacked S16 count <= num_beams to stay strictly within
  // the allocated `out` buffer while exercising every RleDecode failure path.
  struct CorruptRleCase {
    int num_beams;
    uint32_t s16_word;
  };
  constexpr std::array<CorruptRleCase, 5> kCases = {{
      // 1. Sentinel 0 at index 0 triggers `if (i < 2) return (0)`.
      {.num_beams = 4, .s16_word = (15U << 28) | 0U},
      // 2. Run count c = 5 + 4 = 9 exceeds remaining k + 1 = 5 (`c > k + 1`).
      {.num_beams = 5, .s16_word = (12U << 28) | 1U | (5U << 10) | (0U << 19)},
      // 3. Sentinel triple with value == 0 triggers `if (in[i-2] == 0)`.
      {.num_beams = 5, .s16_word = (12U << 28) | 0U},
      // 4. RLE triple expands to k = -1 before earlier literal `[2]` (`k < 0`).
      {.num_beams = 5,
       .s16_word =
           (10U << 28) | 2U | (3U << 5) | (0U << 10) | (0U << 16) | (2U << 22)},
      // 5. Fewer RLE symbols than num_beams leaves k >= 0 at loop end.
      {.num_beams = 5, .s16_word = (15U << 28) | 1U},
  }};

  for (std::size_t idx = 0; idx < kCases.size(); ++idx) {
    SCOPED_TRACE(::testing::Message() << "corrupt RLE case " << idx);
    const std::vector<uint8_t> payload = MakePayload(
        /*version=*/1, /*order=*/0,
        std::array<uint32_t, 1>{kCases[idx].s16_word});
    unsigned short* out = nullptr;
    gsfError = GSF_NORMAL;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kShortSubrecordId - 1] = 0;
      gsfError = GSF_NORMAL;
    };

    EXPECT_EQ(-1, DecodeCompressedUnsignedShortArray(
                      &out, payload.data(), kCases[idx].num_beams,
                      static_cast<int>(payload.size()), kShortSubrecordId,
                      kDefaultHandle));
    EXPECT_EQ(GSF_COMPRESSION_FAILED, gsfIntError());
  }
}

TEST(GsfCompressTest, EncodeArrayRejectsIllegalScaleFactorMultiplier) {
  constexpr std::array<double, 3> kInput = {10.0, 20.0, 30.0};
  constexpr std::array<double, 2> kBadMultipliers = {0.0, 5.0e-7};

  for (double mult : kBadMultipliers) {
    gsfScaleFactors sf{};
    sf.scaleTable[kDoubleSubrecordId - 1].multiplier = mult;
    std::array<uint8_t, 32> encoded{};
    gsfError = GSF_NORMAL;
    absl::Cleanup reset_err = [] { gsfError = GSF_NORMAL; };

    EXPECT_EQ(-1, EncodeCompressedArray(encoded.data(), kInput.data(),
                                        static_cast<int>(kInput.size()), &sf,
                                        kDoubleSubrecordId));
    EXPECT_EQ(GSF_ILLEGAL_SCALE_FACTOR_MULTIPLIER, gsfIntError());
  }
}

TEST(GsfCompressTest, EncodeArrayRejectsNonPositiveNumBeams) {
  const gsfScaleFactors sf =
      MakeScaleFactors(kDoubleSubrecordId, /*precision=*/0.01, /*offset=*/0);
  constexpr std::array<double, 2> kInput = {10.0, 20.0};

  for (int num_beams : {0, -3}) {
    std::array<uint8_t, 16> encoded{};
    gsfError = GSF_NORMAL;
    absl::Cleanup reset_err = [] { gsfError = GSF_NORMAL; };

    EXPECT_EQ(-1, EncodeCompressedArray(encoded.data(), kInput.data(),
                                        num_beams, &sf, kDoubleSubrecordId));
    EXPECT_EQ(GSF_INVALID_NUM_BEAMS, gsfIntError());
  }
}

TEST(GsfCompressTest, EncodeArrayFailsWhenResidualsExceed28Bits) {
  const gsfScaleFactors sf =
      MakeScaleFactors(kDoubleSubrecordId, /*precision=*/1.0, /*offset=*/0);
  // Residual of +/-1.5e8 becomes ~3.0e8 (>= 2^28) after SignEncode/RleEncode,
  // forcing S16Pack -> CompressIntArray to return 0 and EncodeCompressedArray
  // to free its temporary buffer and return -1.
  constexpr std::array<std::array<double, 2>, 2> kOutOfRangeInputs = {{
      {0.0, 150000000.0},
      {0.0, -150000000.0},
  }};

  for (const auto& input : kOutOfRangeInputs) {
    std::array<uint8_t, 32> encoded{};
    gsfError = GSF_NORMAL;
    absl::Cleanup reset_err = [] { gsfError = GSF_NORMAL; };

    EXPECT_EQ(-1, EncodeCompressedArray(encoded.data(), input.data(),
                                        static_cast<int>(input.size()), &sf,
                                        kDoubleSubrecordId));
    EXPECT_EQ(GSF_COMPRESSION_FAILED, gsfIntError());
  }
}

TEST(GsfCompressTest, DecodeArrayRejectsIllegalScaleFactorMultiplier) {
  const std::vector<uint8_t> payload =
      MakePayload(/*version=*/1, /*order=*/0, std::array<uint32_t, 1>{0});
  for (double mult : {0.0, 9.9e-7}) {
    gsfScaleFactors sf{};
    sf.scaleTable[kDoubleSubrecordId - 1].multiplier = mult;
    double* out = nullptr;
    gsfError = GSF_NORMAL;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
      gsfError = GSF_NORMAL;
    };

    EXPECT_EQ(-1, DecodeCompressedArray(&out, payload.data(), /*num_beams=*/4,
                                        static_cast<int>(payload.size()), &sf,
                                        kDoubleSubrecordId, kDefaultHandle));
    EXPECT_EQ(GSF_ILLEGAL_SCALE_FACTOR_MULTIPLIER, gsfIntError());
    EXPECT_EQ(nullptr, out);
  }
}

TEST(GsfCompressTest, DecodeArrayRejectsNonPositiveNumBeams) {
  const gsfScaleFactors sf =
      MakeScaleFactors(kDoubleSubrecordId, /*precision=*/0.01, /*offset=*/0);
  const std::vector<uint8_t> payload =
      MakePayload(/*version=*/1, /*order=*/0, std::array<uint32_t, 1>{0});

  for (int num_beams : {0, -1}) {
    double* out = nullptr;
    gsfError = GSF_NORMAL;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
      gsfError = GSF_NORMAL;
    };

    EXPECT_EQ(-1, DecodeCompressedArray(&out, payload.data(), num_beams,
                                        static_cast<int>(payload.size()), &sf,
                                        kDoubleSubrecordId, kDefaultHandle));
    EXPECT_EQ(GSF_INVALID_NUM_BEAMS, gsfIntError());
    EXPECT_EQ(nullptr, out);
  }
}

TEST(GsfCompressTest, DecodeArrayRejectsUnsupportedVersion) {
  const gsfScaleFactors sf =
      MakeScaleFactors(kDoubleSubrecordId, /*precision=*/0.01, /*offset=*/0);
  constexpr std::array<double, 4> kInput = {10.0, 12.0, 14.0, 16.0};
  std::vector<uint8_t> encoded(5 + 4 * kInput.size());
  const int encoded_bytes = EncodeCompressedArray(
      encoded.data(), kInput.data(), static_cast<int>(kInput.size()), &sf,
      kDoubleSubrecordId);
  ASSERT_GT(encoded_bytes, 4);

  for (uint8_t bad_version : {uint8_t{2}, uint8_t{5}}) {
    std::vector<uint8_t> payload(encoded.begin() + 4,
                                 encoded.begin() + encoded_bytes);
    payload[0] = static_cast<uint8_t>((bad_version << 5) | (payload[0] & 0x1F));

    double* out = nullptr;
    gsfError = GSF_NORMAL;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
      gsfError = GSF_NORMAL;
    };

    EXPECT_EQ(-1, DecodeCompressedArray(&out, payload.data(),
                                        static_cast<int>(kInput.size()),
                                        static_cast<int>(payload.size()), &sf,
                                        kDoubleSubrecordId, kDefaultHandle));
    EXPECT_EQ(GSF_COMPRESSION_UNSUPPORTED, gsfIntError());
    EXPECT_NE(nullptr, out);
  }
}

TEST(GsfCompressTest, DecodeArrayRejectsTruncatedAndCorruptPayload) {
  const gsfScaleFactors sf =
      MakeScaleFactors(kDoubleSubrecordId, /*precision=*/1.0, /*offset=*/0);
  // Encodes with order = 1 and two packed Simple-16 words (total 17 bytes).
  constexpr std::array<double, 4> kInput = {35000.0, 55000.0, 54999.0, 55000.0};
  std::vector<uint8_t> encoded(5 + 4 * kInput.size());
  const int encoded_bytes = EncodeCompressedArray(
      encoded.data(), kInput.data(), static_cast<int>(kInput.size()), &sf,
      kDoubleSubrecordId);
  ASSERT_EQ(17, encoded_bytes);
  const int payload_size = encoded_bytes - 4;

  // 1. Larger num_beams than encoded triggers UncompressDoubleArray failure.
  {
    double* out = nullptr;
    gsfError = GSF_NORMAL;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
      gsfError = GSF_NORMAL;
    };

    EXPECT_EQ(-1, DecodeCompressedArray(&out, encoded.data() + 4,
                                        /*num_beams=*/10, payload_size, &sf,
                                        kDoubleSubrecordId, kDefaultHandle));
    EXPECT_EQ(GSF_COMPRESSION_FAILED, gsfIntError());
  }

  // 2. Dropping the second S16 word keeps m (2) >= order (1) while failing RLE.
  {
    double* out = nullptr;
    gsfError = GSF_NORMAL;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
      gsfError = GSF_NORMAL;
    };

    EXPECT_EQ(-1, DecodeCompressedArray(&out, encoded.data() + 4,
                                        static_cast<int>(kInput.size()),
                                        payload_size - 4, &sf,
                                        kDoubleSubrecordId, kDefaultHandle));
    EXPECT_EQ(GSF_COMPRESSION_FAILED, gsfIntError());
  }

  // 3. Corrupt RLE sentinel at index 0.
  {
    const std::vector<uint8_t> corrupt = MakePayload(
        /*version=*/1, /*order=*/0, std::array<uint32_t, 1>{15U << 28});
    double* out = nullptr;
    gsfError = GSF_NORMAL;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
      gsfError = GSF_NORMAL;
    };

    EXPECT_EQ(-1, DecodeCompressedArray(&out, corrupt.data(), /*num_beams=*/4,
                                        static_cast<int>(corrupt.size()), &sf,
                                        kDoubleSubrecordId, kDefaultHandle));
    EXPECT_EQ(GSF_COMPRESSION_FAILED, gsfIntError());
  }
}

TEST(GsfCompressTest, EncodeWritesBigEndianSubrecordHeaderAtBoundaryIds) {
  constexpr std::array<unsigned int, 2> kBoundarySubrecords = {
      GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
      GSF_SWATH_BATHY_SUBRECORD_TVG_ARRAY,
  };
  constexpr std::array<unsigned short, 5> kShortInput = {10, 20, 30, 40, 50};
  constexpr std::array<double, 5> kDoubleInput = {10.5, 20.5, 30.5, 40.5, 50.5};

  for (unsigned int subrecord_id : kBoundarySubrecords) {
    // 1. Verify unsigned short header and decode at boundary handle = 4.
    std::vector<uint8_t> short_encoded(5 + 4 * kShortInput.size());
    const int short_bytes = EncodeCompressedUnsignedShortArray(
        short_encoded.data(), kShortInput.data(),
        static_cast<int>(kShortInput.size()), static_cast<int>(subrecord_id));
    ASSERT_GT(short_bytes, 4);

    const uint32_t short_header = ReadBe32(short_encoded);
    EXPECT_EQ(subrecord_id, short_header >> 24);
    EXPECT_EQ(static_cast<uint32_t>(short_bytes - 4), short_header & 0xFFFFFFU);
    EXPECT_EQ(1U, short_encoded[4] >> 5);
    EXPECT_EQ(0U, (short_bytes - 5) % 4);

    unsigned short* short_out = nullptr;
    absl::Cleanup short_cleanup = [&] {
      std::free(short_out);
      arraySize[kMaxHandle - 1][subrecord_id - 1] = 0;
    };
    EXPECT_EQ(short_bytes - 4,
              DecodeCompressedUnsignedShortArray(
                  &short_out, short_encoded.data() + 4,
                  static_cast<int>(kShortInput.size()), short_bytes - 4,
                  static_cast<int>(subrecord_id), kMaxHandle));
    EXPECT_THAT(std::span<const unsigned short>(short_out, kShortInput.size()),
                ElementsAreArray(kShortInput));

    // 2. Verify double header and decode at boundary handle = 4.
    const gsfScaleFactors sf =
        MakeScaleFactors(subrecord_id, /*precision=*/0.1, /*offset=*/0);
    std::vector<uint8_t> double_encoded(5 + 4 * kDoubleInput.size());
    const int double_bytes =
        EncodeCompressedArray(double_encoded.data(), kDoubleInput.data(),
                              static_cast<int>(kDoubleInput.size()), &sf,
                              static_cast<int>(subrecord_id));
    ASSERT_GT(double_bytes, 4);

    const uint32_t double_header = ReadBe32(double_encoded);
    EXPECT_EQ(subrecord_id, double_header >> 24);
    EXPECT_EQ(static_cast<uint32_t>(double_bytes - 4),
              double_header & 0xFFFFFFU);
    EXPECT_EQ(1U, double_encoded[4] >> 5);

    double* double_out = nullptr;
    absl::Cleanup double_cleanup = [&] {
      std::free(double_out);
      arraySize[kMaxHandle - 1][subrecord_id - 1] = 0;
    };
    EXPECT_EQ(double_bytes - 4,
              DecodeCompressedArray(
                  &double_out, double_encoded.data() + 4,
                  static_cast<int>(kDoubleInput.size()), double_bytes - 4, &sf,
                  static_cast<int>(subrecord_id), kMaxHandle));
    EXPECT_THAT(std::span<const double>(double_out, kDoubleInput.size()),
                ElementsAreArray(kDoubleInput));
  }
}

TEST(GsfCompressTest, DecodeUnsignedShortReusesAndGrowsBuffer) {
  constexpr int kHandle = 2;
  constexpr std::array<unsigned short, 8> kInit = {11, 13, 15, 17,
                                                   19, 21, 23, 25};
  constexpr std::array<unsigned short, 4> kSmall = {91, 92, 93, 94};
  constexpr std::array<unsigned short, 16> kLarge = {
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

  auto encode = [](std::span<const unsigned short> data) {
    std::vector<uint8_t> buf(5 + 4 * data.size());
    const int n = EncodeCompressedUnsignedShortArray(
        buf.data(), data.data(), static_cast<int>(data.size()),
        kShortSubrecordId);
    EXPECT_GT(n, 4);
    buf.resize(static_cast<std::size_t>(n));
    return buf;
  };

  const std::vector<uint8_t> enc_init = encode(kInit);
  const std::vector<uint8_t> enc_small = encode(kSmall);
  const std::vector<uint8_t> enc_large = encode(kLarge);

  unsigned short* out = nullptr;
  absl::Cleanup cleanup = [&] {
    std::free(out);
    arraySize[kHandle - 1][kShortSubrecordId - 1] = 0;
  };

  // 1. Initial decode allocates buffer for 8 beams.
  ASSERT_EQ(
      static_cast<int>(enc_init.size() - 4),
      DecodeCompressedUnsignedShortArray(
          &out, enc_init.data() + 4, static_cast<int>(kInit.size()),
          static_cast<int>(enc_init.size() - 4), kShortSubrecordId, kHandle));
  ASSERT_NE(nullptr, out);
  unsigned short* const first_ptr = out;
  EXPECT_EQ(8, arraySize[kHandle - 1][kShortSubrecordId - 1]);
  EXPECT_THAT(std::span<const unsigned short>(out, kInit.size()),
              ElementsAreArray(kInit));

  // 2. Smaller decode (4 <= 8) reuses allocation in place.
  std::fill(out, out + kInit.size(), static_cast<unsigned short>(0xBEEF));
  ASSERT_EQ(
      static_cast<int>(enc_small.size() - 4),
      DecodeCompressedUnsignedShortArray(
          &out, enc_small.data() + 4, static_cast<int>(kSmall.size()),
          static_cast<int>(enc_small.size() - 4), kShortSubrecordId, kHandle));
  EXPECT_EQ(first_ptr, out);
  EXPECT_EQ(8, arraySize[kHandle - 1][kShortSubrecordId - 1]);
  EXPECT_THAT(std::span<const unsigned short>(out, kSmall.size()),
              ElementsAreArray(kSmall));

  // 3. Equal-size decode (8 <= 8) reuses allocation without realloc.
  ASSERT_EQ(
      static_cast<int>(enc_init.size() - 4),
      DecodeCompressedUnsignedShortArray(
          &out, enc_init.data() + 4, static_cast<int>(kInit.size()),
          static_cast<int>(enc_init.size() - 4), kShortSubrecordId, kHandle));
  EXPECT_EQ(first_ptr, out);
  EXPECT_THAT(std::span<const unsigned short>(out, kInit.size()),
              ElementsAreArray(kInit));

  // 4. Larger decode (16 > 8) triggers realloc + zero-init and grows arraySize.
  ASSERT_EQ(
      static_cast<int>(enc_large.size() - 4),
      DecodeCompressedUnsignedShortArray(
          &out, enc_large.data() + 4, static_cast<int>(kLarge.size()),
          static_cast<int>(enc_large.size() - 4), kShortSubrecordId, kHandle));
  EXPECT_EQ(16, arraySize[kHandle - 1][kShortSubrecordId - 1]);
  EXPECT_THAT(std::span<const unsigned short>(out, kLarge.size()),
              ElementsAreArray(kLarge));
}

TEST(GsfCompressTest, DecodeArrayReusesAndGrowsBuffer) {
  constexpr int kHandle = 3;
  const gsfScaleFactors sf =
      MakeScaleFactors(kDoubleSubrecordId, /*precision=*/0.1, /*offset=*/0);
  constexpr std::array<double, 8> kInit = {1.1, 1.3, 1.5, 1.7,
                                           1.9, 2.1, 2.3, 2.5};
  constexpr std::array<double, 4> kSmall = {-5.5, -2.5, 0.0, 4.5};
  constexpr std::array<double, 16> kLarge = {10.0, 10.2, 10.4, 10.6, 10.8, 11.0,
                                             11.2, 11.4, 11.6, 11.8, 12.0, 12.2,
                                             12.4, 12.6, 12.8, 13.0};

  auto encode = [&](std::span<const double> data) {
    std::vector<uint8_t> buf(5 + 4 * data.size());
    const int n = EncodeCompressedArray(buf.data(), data.data(),
                                        static_cast<int>(data.size()), &sf,
                                        kDoubleSubrecordId);
    EXPECT_GT(n, 4);
    buf.resize(static_cast<std::size_t>(n));
    return buf;
  };

  const std::vector<uint8_t> enc_init = encode(kInit);
  const std::vector<uint8_t> enc_small = encode(kSmall);
  const std::vector<uint8_t> enc_large = encode(kLarge);

  double* out = nullptr;
  absl::Cleanup cleanup = [&] {
    std::free(out);
    arraySize[kHandle - 1][kDoubleSubrecordId - 1] = 0;
  };

  // 1. Initial decode allocates buffer for 8 beams.
  ASSERT_EQ(static_cast<int>(enc_init.size() - 4),
            DecodeCompressedArray(&out, enc_init.data() + 4,
                                  static_cast<int>(kInit.size()),
                                  static_cast<int>(enc_init.size() - 4), &sf,
                                  kDoubleSubrecordId, kHandle));
  ASSERT_NE(nullptr, out);
  double* const first_ptr = out;
  EXPECT_EQ(8, arraySize[kHandle - 1][kDoubleSubrecordId - 1]);
  for (std::size_t i = 0; i < kInit.size(); ++i) {
    EXPECT_NEAR(kInit[i], out[i], 1e-12);
  }

  // 2. Smaller decode (4 <= 8) reuses allocation in place.
  std::fill(out, out + kInit.size(), -999.0);
  ASSERT_EQ(static_cast<int>(enc_small.size() - 4),
            DecodeCompressedArray(&out, enc_small.data() + 4,
                                  static_cast<int>(kSmall.size()),
                                  static_cast<int>(enc_small.size() - 4), &sf,
                                  kDoubleSubrecordId, kHandle));
  EXPECT_EQ(first_ptr, out);
  EXPECT_EQ(8, arraySize[kHandle - 1][kDoubleSubrecordId - 1]);
  for (std::size_t i = 0; i < kSmall.size(); ++i) {
    EXPECT_NEAR(kSmall[i], out[i], 1e-12);
  }

  // 3. Equal-size decode (8 <= 8) reuses allocation without realloc.
  ASSERT_EQ(static_cast<int>(enc_init.size() - 4),
            DecodeCompressedArray(&out, enc_init.data() + 4,
                                  static_cast<int>(kInit.size()),
                                  static_cast<int>(enc_init.size() - 4), &sf,
                                  kDoubleSubrecordId, kHandle));
  EXPECT_EQ(first_ptr, out);

  // 4. Larger decode (16 > 8) triggers realloc + memset and grows arraySize.
  ASSERT_EQ(static_cast<int>(enc_large.size() - 4),
            DecodeCompressedArray(&out, enc_large.data() + 4,
                                  static_cast<int>(kLarge.size()),
                                  static_cast<int>(enc_large.size() - 4), &sf,
                                  kDoubleSubrecordId, kHandle));
  EXPECT_EQ(16, arraySize[kHandle - 1][kDoubleSubrecordId - 1]);
  for (std::size_t i = 0; i < kLarge.size(); ++i) {
    EXPECT_NEAR(kLarge[i], out[i], 1e-12);
  }
}

TEST(GsfCompressTest, LpcOrdersZeroThroughThreeAndShortArraysRoundTrip) {
  struct LpcCase {
    int expected_order;
    std::vector<unsigned short> values;
  };
  const std::vector<LpcCase> cases = {
      // Single zero -> sum0 == 0 -> order 0.
      {.expected_order = 0, .values = {0}},
      // Single non-zero -> order 1; hits `n <= order` early return (1 <= 1).
      {.expected_order = 1, .values = {42}},
      // Two distinct values -> sum1 == sum2 == sum3 -> order 1.
      {.expected_order = 1, .values = {10, 20}},
      // Quadratic 3-point curve -> sum3 < sum2 < sum1 -> order 3 (n <= order).
      {.expected_order = 3, .values = {0, 10, 30}},
      // All zeros -> order 0.
      {.expected_order = 0, .values = {0, 0, 0, 0, 0, 0, 0, 0}},
      // Constant non-zero -> diff1 == 0 -> order 1.
      {.expected_order = 1, .values = {500, 500, 500, 500, 500, 500, 500, 500}},
      // Linear ramp -> diff2 == 0 after step 1 -> order 2.
      {.expected_order = 2, .values = {10, 20, 30, 40, 50, 60, 70, 80}},
      // Quadratic x[i] = i^2 -> diff3 == 0 after step 2 -> order 3.
      {.expected_order = 3, .values = {0, 1, 4, 9, 16, 25, 36, 49, 64, 81}},
  };

  const gsfScaleFactors sf =
      MakeScaleFactors(kDoubleSubrecordId, /*precision=*/1.0, /*offset=*/0);

  for (std::size_t idx = 0; idx < cases.size(); ++idx) {
    SCOPED_TRACE(::testing::Message()
                 << "LPC case " << idx
                 << " expected_order=" << cases[idx].expected_order);
    const int n = static_cast<int>(cases[idx].values.size());

    // 1. Round-trip via unsigned short API.
    std::vector<uint8_t> short_enc(5 + 4 * n);
    const int short_bytes = EncodeCompressedUnsignedShortArray(
        short_enc.data(), cases[idx].values.data(), n, kShortSubrecordId);
    ASSERT_GT(short_bytes, 4);
    EXPECT_EQ(cases[idx].expected_order, short_enc[4] & 0x1F);

    unsigned short* short_out = nullptr;
    absl::Cleanup short_cleanup = [&] {
      std::free(short_out);
      arraySize[kDefaultHandle - 1][kShortSubrecordId - 1] = 0;
    };
    ASSERT_EQ(short_bytes - 4,
              DecodeCompressedUnsignedShortArray(
                  &short_out, short_enc.data() + 4, n, short_bytes - 4,
                  kShortSubrecordId, kDefaultHandle));
    EXPECT_THAT(std::span<const unsigned short>(short_out, n),
                ElementsAreArray(cases[idx].values));

    // 2. Round-trip via double API.
    std::vector<double> double_in(cases[idx].values.begin(),
                                  cases[idx].values.end());
    std::vector<uint8_t> double_enc(5 + 4 * n);
    const int double_bytes = EncodeCompressedArray(
        double_enc.data(), double_in.data(), n, &sf, kDoubleSubrecordId);
    ASSERT_GT(double_bytes, 4);
    EXPECT_EQ(cases[idx].expected_order, double_enc[4] & 0x1F);

    double* double_out = nullptr;
    absl::Cleanup double_cleanup = [&] {
      std::free(double_out);
      arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
    };
    ASSERT_EQ(double_bytes - 4,
              DecodeCompressedArray(&double_out, double_enc.data() + 4, n,
                                    double_bytes - 4, &sf, kDoubleSubrecordId,
                                    kDefaultHandle));
    EXPECT_THAT(std::span<const double>(double_out, n),
                ElementsAreArray(double_in));
  }
}

TEST(GsfCompressTest, RleHandlesShortLongAndNonZeroResidualRuns) {
  std::vector<std::vector<unsigned short>> datasets;

  // 1. Mixed runs: lengths 1, 2, 3 (uncompressed literals) and 4, 5, 20 (RLE).
  std::vector<unsigned short> mixed = {10, 10, 10, 10, 10, 12,
                                       12, 14, 15, 15, 15};
  mixed.insert(mixed.end(), 20, 15);  // extends constant run past 4 -> RLE
  mixed.insert(mixed.end(), {25, 25, 25, 25, 9, 9, 9, 9, 9, 9});
  datasets.push_back(std::move(mixed));

  // 2. Non-zero repeated residuals (+5 four times) with overall order = 1.
  // Oscillating +/-20 swings keep sum1 < sum2 < sum3 so LpcOrder chooses 1,
  // sending four consecutive `+5` residuals into RleEncode.
  datasets.push_back(
      {1000, 1005, 1010, 1015, 1020, 1000, 1020, 1000, 1020, 1000, 1020});

  // 3. Long zero run (> 255) and long constant non-zero run (> 255).
  datasets.emplace_back(300, 0);
  datasets.emplace_back(300, 777);

  const gsfScaleFactors sf =
      MakeScaleFactors(kDoubleSubrecordId, /*precision=*/1.0, /*offset=*/0);

  for (std::size_t idx = 0; idx < datasets.size(); ++idx) {
    SCOPED_TRACE(::testing::Message() << "RLE dataset " << idx);
    const auto& input = datasets[idx];
    const int n = static_cast<int>(input.size());

    std::vector<uint8_t> short_enc(5 + 4 * n);
    const int short_bytes = EncodeCompressedUnsignedShortArray(
        short_enc.data(), input.data(), n, kShortSubrecordId);
    ASSERT_GT(short_bytes, 4);

    unsigned short* short_out = nullptr;
    absl::Cleanup short_cleanup = [&] {
      std::free(short_out);
      arraySize[kDefaultHandle - 1][kShortSubrecordId - 1] = 0;
    };
    ASSERT_EQ(short_bytes - 4,
              DecodeCompressedUnsignedShortArray(
                  &short_out, short_enc.data() + 4, n, short_bytes - 4,
                  kShortSubrecordId, kDefaultHandle));
    EXPECT_THAT(std::span<const unsigned short>(short_out, n),
                ElementsAreArray(input));

    std::vector<double> double_in(input.begin(), input.end());
    std::vector<uint8_t> double_enc(5 + 4 * n);
    const int double_bytes = EncodeCompressedArray(
        double_enc.data(), double_in.data(), n, &sf, kDoubleSubrecordId);
    ASSERT_GT(double_bytes, 4);

    double* double_out = nullptr;
    absl::Cleanup double_cleanup = [&] {
      std::free(double_out);
      arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
    };
    ASSERT_EQ(double_bytes - 4,
              DecodeCompressedArray(&double_out, double_enc.data() + 4, n,
                                    double_bytes - 4, &sf, kDoubleSubrecordId,
                                    kDefaultHandle));
    EXPECT_THAT(std::span<const double>(double_out, n),
                ElementsAreArray(double_in));
  }
}

TEST(GsfCompressTest, DoubleArrayConversionAppliesScaleOffsetRoundingAndClamp) {
  // 1. Scale/offset with positive & negative scaled ints and 0.501 rounding.
  const gsfScaleFactors sf =
      MakeScaleFactors(kDoubleSubrecordId, /*precision=*/0.01, /*offset=*/100);
  constexpr std::array<double, 8> kInput = {-150.25, -100.006, -100.004, -100.0,
                                            -99.996, -99.994,  -50.12,   12.34};
  constexpr std::array<double, 8> kExpected = {-150.25, -100.01, -100.0, -100.0,
                                               -100.0,  -99.99,  -50.12, 12.34};

  std::vector<uint8_t> encoded(5 + 4 * kInput.size());
  const int encoded_bytes = EncodeCompressedArray(
      encoded.data(), kInput.data(), static_cast<int>(kInput.size()), &sf,
      kDoubleSubrecordId);
  ASSERT_GT(encoded_bytes, 4);

  {
    double* out = nullptr;
    absl::Cleanup cleanup = [&] {
      std::free(out);
      arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
    };
    ASSERT_EQ(encoded_bytes - 4,
              DecodeCompressedArray(
                  &out, encoded.data() + 4, static_cast<int>(kInput.size()),
                  encoded_bytes - 4, &sf, kDoubleSubrecordId, kDefaultHandle));
    for (std::size_t i = 0; i < kExpected.size(); ++i) {
      EXPECT_NEAR(kExpected[i], out[i], 1e-12);
    }
  }

  // 2. Positive overflow clamping to INT32_MAX (2147483647) with n = 1.
  // (n = 1 hits `order = 1`, `n <= order` bypasses Simple-16 28-bit limit.)
  const gsfScaleFactors unit_sf =
      MakeScaleFactors(kDoubleSubrecordId, /*precision=*/1.0, /*offset=*/0);
  constexpr std::array<double, 1> kOverflowInput = {1.0e12};
  std::vector<uint8_t> clamp_encoded(5 + 4);
  const int clamp_bytes =
      EncodeCompressedArray(clamp_encoded.data(), kOverflowInput.data(), 1,
                            &unit_sf, kDoubleSubrecordId);
  ASSERT_EQ(9, clamp_bytes);

  double* clamp_out = nullptr;
  absl::Cleanup clamp_cleanup = [&] {
    std::free(clamp_out);
    arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
  };
  ASSERT_EQ(clamp_bytes - 4,
            DecodeCompressedArray(&clamp_out, clamp_encoded.data() + 4, 1,
                                  clamp_bytes - 4, &unit_sf, kDoubleSubrecordId,
                                  kDefaultHandle));
  EXPECT_DOUBLE_EQ(2147483647.0, clamp_out[0]);
}

TEST(GsfCompressTest, AllSixteenSimple16ModesRoundTripViaBothApis) {
  // Alternating-sign first-order residuals guarantee LpcOrder selects p = 1,
  // no run reaches length 4 (RleEncode adds +1 only), and S16Pack packs all
  // remaining residuals into one 32-bit word with selector w = 0..15.
  struct Simple16RoundTripCase {
    uint32_t expected_selector;
    std::size_t residual_count;
    std::array<int, 14> residuals;
  };
  constexpr std::array<Simple16RoundTripCase, 16> kCases = {{
      {.expected_selector = 0,
       .residual_count = 14,
       .residuals = {-1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1}},
      {.expected_selector = 1,
       .residual_count = 12,
       .residuals = {-2, 2, -2, 2, -1, 1, -1, 1, -1, 1, -1, 1}},
      {.expected_selector = 2,
       .residual_count = 10,
       .residuals = {-2, 2, -2, 2, -2, 2, -2, 2, -1, 1}},
      {.expected_selector = 3,
       .residual_count = 9,
       .residuals = {-4, 2, -2, 2, -2, 2, -2, 2, -2}},
      {.expected_selector = 4,
       .residual_count = 8,
       .residuals = {-4, 4, -4, 4, -2, 2, -2, 2}},
      {.expected_selector = 5,
       .residual_count = 8,
       .residuals = {-2, 2, -2, 2, -4, 4, -4, 4}},
      {.expected_selector = 6,
       .residual_count = 7,
       .residuals = {-4, 4, -5, 5, -6, 6, -7}},
      {.expected_selector = 7,
       .residual_count = 6,
       .residuals = {-8, 8, -8, 8, -4, 4}},
      {.expected_selector = 8,
       .residual_count = 6,
       .residuals = {-4, 4, -8, 8, -8, 8}},
      {.expected_selector = 9,
       .residual_count = 5,
       .residuals = {-16, 16, -16, 8, -8}},
      {.expected_selector = 10,
       .residual_count = 5,
       .residuals = {-8, 8, -16, 16, -16}},
      {.expected_selector = 11,
       .residual_count = 4,
       .residuals = {-32, 32, -33, 33}},
      {.expected_selector = 12,
       .residual_count = 3,
       .residuals = {-256, 128, -128}},
      {.expected_selector = 13,
       .residual_count = 2,
       .residuals = {-5000, 6000}},
      {.expected_selector = 14,
       .residual_count = 2,
       .residuals = {-10000, 2500}},
      {.expected_selector = 15, .residual_count = 1, .residuals = {20000}},
  }};

  const gsfScaleFactors sf =
      MakeScaleFactors(kDoubleSubrecordId, /*precision=*/1.0, /*offset=*/0);

  for (const auto& tc : kCases) {
    SCOPED_TRACE(::testing::Message()
                 << "Simple-16 selector w=" << tc.expected_selector);
    std::vector<unsigned short> short_in;
    short_in.reserve(1 + tc.residual_count);
    int cur = 35000;
    short_in.push_back(static_cast<unsigned short>(cur));
    for (std::size_t i = 0; i < tc.residual_count; ++i) {
      cur += tc.residuals[i];
      short_in.push_back(static_cast<unsigned short>(cur));
    }
    const int n = static_cast<int>(short_in.size());

    // 1. Unsigned short encode/decode verifies S16Pack & S16Unpack selector.
    std::vector<uint8_t> short_enc(5 + 4 * n);
    ASSERT_EQ(13, EncodeCompressedUnsignedShortArray(
                      short_enc.data(), short_in.data(), n, kShortSubrecordId));
    EXPECT_EQ(1, short_enc[4] & 0x1F);
    EXPECT_EQ(
        tc.expected_selector,
        ReadBe32(std::span<const uint8_t>(short_enc).subspan(9, 4)) >> 28);

    unsigned short* short_out = nullptr;
    absl::Cleanup short_cleanup = [&] {
      std::free(short_out);
      arraySize[kDefaultHandle - 1][kShortSubrecordId - 1] = 0;
    };
    ASSERT_EQ(9, DecodeCompressedUnsignedShortArray(
                     &short_out, short_enc.data() + 4, n, 9, kShortSubrecordId,
                     kDefaultHandle));
    EXPECT_THAT(std::span<const unsigned short>(short_out, n),
                ElementsAreArray(short_in));

    // 2. Double encode/decode verifies the same selector path via doubles.
    std::vector<double> double_in(short_in.begin(), short_in.end());
    std::vector<uint8_t> double_enc(5 + 4 * n);
    ASSERT_EQ(13, EncodeCompressedArray(double_enc.data(), double_in.data(), n,
                                        &sf, kDoubleSubrecordId));
    EXPECT_EQ(1, double_enc[4] & 0x1F);
    EXPECT_EQ(
        tc.expected_selector,
        ReadBe32(std::span<const uint8_t>(double_enc).subspan(9, 4)) >> 28);

    double* double_out = nullptr;
    absl::Cleanup double_cleanup = [&] {
      std::free(double_out);
      arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
    };
    ASSERT_EQ(
        9, DecodeCompressedArray(&double_out, double_enc.data() + 4, n, 9, &sf,
                                 kDoubleSubrecordId, kDefaultHandle));
    EXPECT_THAT(std::span<const double>(double_out, n),
                ElementsAreArray(double_in));
  }

  // Multi-word fall-through to selector 15 when left > 1 (40001 >= 32768 fails
  // selectors 12..14, packs via w = 15, then packs remaining 2 values via w =
  // 13).
  constexpr std::array<unsigned short, 4> kFallthrough = {35000, 55000, 54999,
                                                          55000};
  std::vector<uint8_t> enc(5 + 4 * kFallthrough.size());
  ASSERT_EQ(17, EncodeCompressedUnsignedShortArray(
                    enc.data(), kFallthrough.data(),
                    static_cast<int>(kFallthrough.size()), kShortSubrecordId));
  EXPECT_EQ(15U, ReadBe32(std::span<const uint8_t>(enc).subspan(9, 4)) >> 28);
  EXPECT_EQ(13U, ReadBe32(std::span<const uint8_t>(enc).subspan(13, 4)) >> 28);

  unsigned short* out = nullptr;
  absl::Cleanup cleanup = [&] {
    std::free(out);
    arraySize[kDefaultHandle - 1][kShortSubrecordId - 1] = 0;
  };
  ASSERT_EQ(13, DecodeCompressedUnsignedShortArray(
                    &out, enc.data() + 4, static_cast<int>(kFallthrough.size()),
                    13, kShortSubrecordId, kDefaultHandle));
  EXPECT_THAT(std::span<const unsigned short>(out, kFallthrough.size()),
              ElementsAre(35000, 55000, 54999, 55000));
}

TEST(GsfCompressTest, DirectDecodeExercisesAllSixteenSimple16UnpackCases) {
  const gsfScaleFactors sf =
      MakeScaleFactors(kDoubleSubrecordId, /*precision=*/1.0, /*offset=*/0);

  for (const auto& spec : kSimple16Specs) {
    SCOPED_TRACE(::testing::Message()
                 << "Direct S16Unpack selector w=" << spec.selector);

    // 1. Unsigned short direct decode: odd RLE values map to non-negative
    // shorts within [0, 65535].
    std::vector<uint32_t> short_rle(spec.count);
    std::vector<unsigned short> expected_shorts(spec.count);
    for (std::size_t i = 0; i < spec.count; ++i) {
      uint32_t val = 0;
      if (spec.selector == 15) {
        val = 40001U;  // SignDecode -> 20000
      } else if (spec.bits[i] == 2) {
        val = (i % 2 == 0) ? 3U : 1U;
      } else {
        const uint32_t max_val = (1U << spec.bits[i]) - 1U;
        val = (i % 2 == 0) ? max_val : (max_val - 2U);
      }
      short_rle[i] = val;
      expected_shorts[i] = static_cast<unsigned short>((val - 1U) / 2U);
    }

    const std::vector<uint8_t> short_payload = MakePayload(
        /*version=*/1, /*order=*/0,
        std::array<uint32_t, 1>{PackSimple16Word(spec, short_rle)});
    unsigned short* short_out = nullptr;
    absl::Cleanup short_cleanup = [&] {
      std::free(short_out);
      arraySize[kDefaultHandle - 1][kShortSubrecordId - 1] = 0;
    };
    ASSERT_EQ(
        static_cast<int>(short_payload.size()),
        DecodeCompressedUnsignedShortArray(
            &short_out, short_payload.data(), static_cast<int>(spec.count),
            static_cast<int>(short_payload.size()), kShortSubrecordId,
            kDefaultHandle));
    EXPECT_THAT(std::span<const unsigned short>(short_out, spec.count),
                ElementsAreArray(expected_shorts));

    // 2. Double direct decode: alternates odd/even RLE symbols to exercise
    // both positive and negative SignDecode outputs, including max 28-bit
    // (2^28 - 1) for selector 15.
    std::vector<uint32_t> double_rle(spec.count);
    std::vector<double> expected_doubles(spec.count);
    for (std::size_t i = 0; i < spec.count; ++i) {
      const uint32_t max_val = (1U << spec.bits[i]) - 1U;
      const uint32_t val = (i % 2 == 0) ? max_val : (max_val - 1U);
      double_rle[i] = val;
      const uint32_t s = val - 1U;
      expected_doubles[i] = (s & 1U) ? -static_cast<double>((s + 1U) / 2U)
                                     : static_cast<double>(s / 2U);
    }

    const std::vector<uint8_t> double_payload = MakePayload(
        /*version=*/1, /*order=*/0,
        std::array<uint32_t, 1>{PackSimple16Word(spec, double_rle)});
    double* double_out = nullptr;
    absl::Cleanup double_cleanup = [&] {
      std::free(double_out);
      arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
    };
    ASSERT_EQ(static_cast<int>(double_payload.size()),
              DecodeCompressedArray(&double_out, double_payload.data(),
                                    static_cast<int>(spec.count),
                                    static_cast<int>(double_payload.size()),
                                    &sf, kDoubleSubrecordId, kDefaultHandle));
    EXPECT_THAT(std::span<const double>(double_out, spec.count),
                ElementsAreArray(expected_doubles));
  }
  // Also check negative max 28-bit Simple-16 value (2^28 - 2 -> -134217727).
  const std::vector<uint8_t> neg_payload = MakePayload(
      /*version=*/1, /*order=*/0,
      std::array<uint32_t, 1>{(15U << 28) | ((1U << 28) - 2U)});
  double* neg_out = nullptr;
  absl::Cleanup neg_cleanup = [&] {
    std::free(neg_out);
    arraySize[kDefaultHandle - 1][kDoubleSubrecordId - 1] = 0;
  };
  ASSERT_EQ(static_cast<int>(neg_payload.size()),
            DecodeCompressedArray(&neg_out, neg_payload.data(), /*num_beams=*/1,
                                  static_cast<int>(neg_payload.size()), &sf,
                                  kDoubleSubrecordId, kDefaultHandle));
  EXPECT_DOUBLE_EQ(-134217727.0, neg_out[0]);
}

// TODO(schwehr): Remaining uncovered branches in src/gsf_compress.c require
// fault-injection/upstream fixes: calloc/realloc OOM returns, unfreed `out` on
// `m == 0` in EncodeCompressedUnsignedShortArray (lines 153-157), INT32_MIN
// clamping in ConvertArrayToInt (triggers signed-negation UB in LpcOrder),
// unreachable `else` branch in ConvertArrayToDouble (always aliased), and
// unreachable `default:` cases in LpcEncode/LpcDecode/S16Unpack.

}  // namespace
}  // namespace gsf
