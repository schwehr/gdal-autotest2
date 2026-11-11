// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for libtiff/tif_thunder.c (COMPRESSION_THUNDERSCAN 4-bit codec).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

constexpr uint32_t kDefaultAllocBudget = 1U << 24;

// ThunderScan 2-bit code prefixes and skip markers.
constexpr uint8_t kThunderRunCode = 0x00;
constexpr uint8_t kThunderTwoBitDeltaCode = 0x40;
constexpr uint8_t kThunderThreeBitDeltaCode = 0x80;
constexpr uint8_t kThunderRawCode = 0xc0;

constexpr uint8_t kDelta2Zero = 0;
constexpr uint8_t kDelta2Plus1 = 1;
constexpr uint8_t kDelta2Skip = 2;
constexpr uint8_t kDelta2Minus1 = 3;

constexpr uint8_t kDelta3Zero = 0;
constexpr uint8_t kDelta3Plus1 = 1;
constexpr uint8_t kDelta3Plus2 = 2;
constexpr uint8_t kDelta3Plus3 = 3;
constexpr uint8_t kDelta3Skip = 4;
constexpr uint8_t kDelta3Minus3 = 5;
constexpr uint8_t kDelta3Minus2 = 6;
constexpr uint8_t kDelta3Minus1 = 7;

constexpr uint8_t EncodeRaw(uint8_t nibble) {
  return static_cast<uint8_t>(kThunderRawCode | (nibble & 0x0fU));
}

constexpr uint8_t EncodeRun(uint8_t count) {
  return static_cast<uint8_t>(kThunderRunCode | (count & 0x3fU));
}

constexpr uint8_t EncodeTwoBitDeltas(uint8_t d0, uint8_t d1, uint8_t d2) {
  return static_cast<uint8_t>(kThunderTwoBitDeltaCode | ((d0 & 0x03U) << 4) |
                              ((d1 & 0x03U) << 2) | (d2 & 0x03U));
}

constexpr uint8_t EncodeThreeBitDeltas(uint8_t d0, uint8_t d1) {
  return static_cast<uint8_t>(kThunderThreeBitDeltaCode | ((d0 & 0x07U) << 3) |
                              (d1 & 0x07U));
}

// Packs 4-bit pixel nibbles into bytes (MSB-first within each byte), matching
// ThunderDecode output ordering with 0 in any unused trailing low nibble.
std::vector<uint8_t> PackNibbles(const std::vector<uint8_t>& nibbles) {
  std::vector<uint8_t> packed((nibbles.size() + 1) / 2, 0);
  for (size_t i = 0; i < nibbles.size(); ++i) {
    const uint8_t val = static_cast<uint8_t>(nibbles[i] & 0x0fU);
    if ((i & 1U) == 0U) {
      packed[i / 2] = static_cast<uint8_t>(val << 4);
    } else {
      packed[i / 2] |= val;
    }
  }
  return packed;
}

struct ThunderTiffSpec {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t rows_per_strip = 1;
  uint16_t bits_per_sample = 4;
  uint16_t samples_per_pixel = 1;
  uint16_t planar_config = PLANARCONFIG_CONTIG;
  uint16_t photometric = PHOTOMETRIC_MINISBLACK;
  bool big_endian = false;
  bool big_tiff = false;
};

std::vector<uint8_t> BuildThunderTiff(
    const ThunderTiffSpec& spec,
    const std::vector<std::vector<uint8_t>>& strips) {
  RawTiffBuilder builder(spec.big_endian, spec.big_tiff);
  std::vector<uint64_t> offsets;
  std::vector<uint64_t> counts;
  offsets.reserve(strips.size());
  counts.reserve(strips.size());
  for (const std::vector<uint8_t>& strip : strips) {
    offsets.push_back(builder.AppendBlob(strip));
    counts.push_back(strip.size());
  }

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, spec.width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, spec.height),
      spec.samples_per_pixel > 1
          ? builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE,
                                   std::vector<uint16_t>(spec.samples_per_pixel,
                                                         spec.bits_per_sample))
          : builder.MakeShort(TIFFTAG_BITSPERSAMPLE, spec.bits_per_sample),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_THUNDERSCAN),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, spec.photometric),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, spec.samples_per_pixel),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, spec.rows_per_strip),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, spec.planar_config),
  };

  if (spec.big_tiff) {
    entries.push_back(builder.MakeLong8Array(TIFFTAG_STRIPOFFSETS, offsets));
    entries.push_back(builder.MakeLong8Array(TIFFTAG_STRIPBYTECOUNTS, counts));
  } else {
    std::vector<uint32_t> offsets32;
    std::vector<uint32_t> counts32;
    offsets32.reserve(offsets.size());
    counts32.reserve(counts.size());
    for (uint64_t off : offsets) {
      offsets32.push_back(static_cast<uint32_t>(off));
    }
    for (uint64_t cnt : counts) {
      counts32.push_back(static_cast<uint32_t>(cnt));
    }
    entries.push_back(builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offsets32));
    entries.push_back(builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, counts32));
  }

  builder.AddIfd(std::move(entries));
  return builder.Take();
}

bool HasDiagnosticContaining(const std::vector<DiagnosticRecord>& records,
                             std::string_view module, std::string_view substr) {
  return std::any_of(records.begin(), records.end(),
                     [&](const DiagnosticRecord& rec) {
                       return rec.module == module &&
                              rec.message.find(substr) != std::string::npos;
                     });
}

TEST(TifThunderTest, CodecRegisteredAndConfigured) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_THUNDERSCAN), 1);
  const TIFFCodec* codec = TIFFFindCODEC(COMPRESSION_THUNDERSCAN);
  ASSERT_NE(codec, nullptr);
  EXPECT_EQ(codec->scheme, COMPRESSION_THUNDERSCAN);
  EXPECT_EQ(std::string_view(codec->name), "ThunderScan");
  EXPECT_NE(codec->init, nullptr);
}

TEST(TifThunderTest, DecodesRawNibblesForEvenAndOddWidths) {
  struct RawWidthCase {
    std::string_view name;
    uint32_t width;
    std::vector<uint8_t> encoded;
    std::vector<uint8_t> expected_nibbles;
  };

  const std::array<RawWidthCase, 4> kCases = {{
      {.name = "EvenWidth18AllNibblesAndHighBitMask",
       .width = 18,
       .encoded = {EncodeRaw(0), EncodeRaw(1), EncodeRaw(2), EncodeRaw(3),
                   EncodeRaw(4), EncodeRaw(5), EncodeRaw(6), EncodeRaw(7),
                   EncodeRaw(8), EncodeRaw(9), EncodeRaw(10), EncodeRaw(11),
                   EncodeRaw(12), EncodeRaw(13), EncodeRaw(14), EncodeRaw(15),
                   0xd5, 0xff},
       .expected_nibbles = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                            15, 5, 15}},
      {.name = "OddWidth1",
       .width = 1,
       .encoded = {EncodeRaw(10)},
       .expected_nibbles = {10}},
      {.name = "OddWidth3",
       .width = 3,
       .encoded = {EncodeRaw(1), EncodeRaw(9), EncodeRaw(4)},
       .expected_nibbles = {1, 9, 4}},
      {.name = "OddWidth7",
       .width = 7,
       .encoded = {EncodeRaw(0), EncodeRaw(15), EncodeRaw(2), EncodeRaw(14),
                   EncodeRaw(3), EncodeRaw(13), EncodeRaw(7)},
       .expected_nibbles = {0, 15, 2, 14, 3, 13, 7}},
  }};

  for (const RawWidthCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const std::vector<std::vector<uint8_t>> strips = {tc.encoded};
    MemoryTiffStream stream(
        BuildThunderTiff({.width = tc.width, .height = 1}, strips));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    const std::vector<uint8_t> expected = PackNibbles(tc.expected_nibbles);
    ASSERT_EQ(TIFFScanlineSize(reader.get()),
              static_cast<tmsize_t>(expected.size()));

    // Pre-fill with 0xAA to verify odd-width padding nibbles are cleared to 0.
    std::vector<uint8_t> row(expected.size(), 0xaa);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
    EXPECT_EQ(row, expected);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifThunderTest, DecodesRunCodesAcrossEvenOddAndZeroCounts) {
  struct RunCase {
    std::string_view name;
    uint32_t width;
    std::vector<uint8_t> encoded;
    std::vector<uint8_t> expected_nibbles;
  };

  const std::array<RunCase, 6> kCases = {{
      {.name = "ZeroCountRunIsNoopAndInitialRunUsesZero",
       .width = 6,
       .encoded = {EncodeRun(0), EncodeRun(4), EncodeRaw(7), EncodeRun(0),
                   EncodeRaw(3)},
       .expected_nibbles = {0, 0, 0, 0, 7, 3}},
      {.name = "EvenPositionEvenRun",
       .width = 6,
       .encoded = {EncodeRaw(5), EncodeRaw(9), EncodeRun(4)},
       .expected_nibbles = {5, 9, 9, 9, 9, 9}},
      {.name = "EvenPositionOddRunTailMask",
       .width = 5,
       .encoded = {EncodeRaw(4), EncodeRaw(12), EncodeRun(3)},
       .expected_nibbles = {4, 12, 12, 12, 12}},
      {.name = "OddPositionSinglePixelRun",
       .width = 2,
       .encoded = {EncodeRaw(11), EncodeRun(1)},
       .expected_nibbles = {11, 11}},
      {.name = "OddPositionEvenRunTailMask",
       .width = 3,
       .encoded = {EncodeRaw(5), EncodeRun(2)},
       .expected_nibbles = {5, 5, 5}},
      {.name = "OddPositionOddRunThree",
       .width = 4,
       .encoded = {EncodeRaw(8), EncodeRun(3)},
       .expected_nibbles = {8, 8, 8, 8}},
  }};

  for (const RunCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const std::vector<std::vector<uint8_t>> strips = {tc.encoded};
    MemoryTiffStream stream(
        BuildThunderTiff({.width = tc.width, .height = 1}, strips));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    const std::vector<uint8_t> expected = PackNibbles(tc.expected_nibbles);
    std::vector<uint8_t> row(expected.size(), 0xaa);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
    EXPECT_EQ(row, expected);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }

  // Maximum 6-bit run length (63) after one raw pixel.
  const std::vector<std::vector<uint8_t>> max_run_strips = {
      {EncodeRaw(12), EncodeRun(63)}};
  MemoryTiffStream max_stream(
      BuildThunderTiff({.width = 64, .height = 1}, max_run_strips));
  UniqueTiff max_reader = max_stream.Open("r");
  ASSERT_NE(max_reader, nullptr);
  std::vector<uint8_t> max_row(32, 0);
  EXPECT_EQ(TIFFReadScanline(max_reader.get(), max_row.data(), 0, 0), 1);
  EXPECT_EQ(max_row, std::vector<uint8_t>(32, 0xcc));
  EXPECT_EQ(max_stream.diagnostics().error_count(), 0U);
}

TEST(TifThunderTest, DecodesTwoBitDeltasSkipsWraparoundAndTailTruncation) {
  struct TwoBitCase {
    std::string_view name;
    uint32_t width;
    std::vector<uint8_t> encoded;
    std::vector<uint8_t> expected_nibbles;
  };

  const std::array<TwoBitCase, 4> kCases = {{
      {.name = "AllDeltaCodesAndSkipsAcrossAllThreeSlots",
       .width = 10,
       .encoded =
           {
               EncodeRaw(5),
               EncodeTwoBitDeltas(kDelta2Plus1, kDelta2Zero, kDelta2Minus1),
               EncodeTwoBitDeltas(kDelta2Skip, kDelta2Plus1, kDelta2Skip),
               EncodeTwoBitDeltas(kDelta2Skip, kDelta2Skip, kDelta2Skip),
               EncodeTwoBitDeltas(kDelta2Minus1, kDelta2Skip, kDelta2Minus1),
               EncodeTwoBitDeltas(kDelta2Plus1, kDelta2Zero, kDelta2Plus1),
           },
       .expected_nibbles = {5, 6, 6, 5, 6, 5, 4, 5, 5, 6}},
      {.name = "ModularFourBitWraparound",
       .width = 4,
       .encoded =
           {
               EncodeRaw(0),
               EncodeTwoBitDeltas(kDelta2Minus1, kDelta2Zero, kDelta2Plus1),
           },
       .expected_nibbles = {0, 15, 15, 0}},
      {.name = "TrailingNonSkipSlotsIgnoredAtEvenMaxPixels",
       .width = 2,
       .encoded =
           {
               EncodeRaw(4),
               EncodeTwoBitDeltas(kDelta2Plus1, kDelta2Plus1, kDelta2Plus1),
           },
       .expected_nibbles = {4, 5}},
      {.name = "TrailingNonSkipSlotIgnoredAtOddMaxPixels",
       .width = 3,
       .encoded =
           {
               EncodeRaw(4),
               EncodeTwoBitDeltas(kDelta2Plus1, kDelta2Plus1, kDelta2Plus1),
           },
       .expected_nibbles = {4, 5, 6}},
  }};

  for (const TwoBitCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const std::vector<std::vector<uint8_t>> strips = {tc.encoded};
    MemoryTiffStream stream(
        BuildThunderTiff({.width = tc.width, .height = 1}, strips));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    const std::vector<uint8_t> expected = PackNibbles(tc.expected_nibbles);
    std::vector<uint8_t> row(expected.size(), 0xaa);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
    EXPECT_EQ(row, expected);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifThunderTest, DecodesThreeBitDeltasSkipsWraparoundAndTailTruncation) {
  struct ThreeBitCase {
    std::string_view name;
    uint32_t width;
    std::vector<uint8_t> encoded;
    std::vector<uint8_t> expected_nibbles;
  };

  const std::array<ThreeBitCase, 3> kCases = {{
      {.name = "AllDeltaCodesAndSkipsAcrossBothSlots",
       .width = 10,
       .encoded =
           {
               EncodeRaw(2),
               EncodeThreeBitDeltas(kDelta3Plus3, kDelta3Minus2),
               EncodeThreeBitDeltas(kDelta3Plus2, kDelta3Minus3),
               EncodeThreeBitDeltas(kDelta3Skip, kDelta3Skip),
               EncodeThreeBitDeltas(kDelta3Plus1, kDelta3Skip),
               EncodeThreeBitDeltas(kDelta3Skip, kDelta3Minus1),
               EncodeThreeBitDeltas(kDelta3Zero, kDelta3Plus3),
               EncodeRaw(9),
           },
       .expected_nibbles = {2, 5, 3, 5, 2, 3, 2, 2, 5, 9}},
      {.name = "ModularFourBitWraparound",
       .width = 3,
       .encoded =
           {
               EncodeRaw(1),
               EncodeThreeBitDeltas(kDelta3Minus3, kDelta3Plus3),
           },
       .expected_nibbles = {1, 14, 1}},
      {.name = "TrailingSlotIgnoredAtMaxPixels",
       .width = 2,
       .encoded =
           {
               EncodeRaw(6),
               EncodeThreeBitDeltas(kDelta3Plus2, kDelta3Plus3),
           },
       .expected_nibbles = {6, 8}},
  }};

  for (const ThreeBitCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const std::vector<std::vector<uint8_t>> strips = {tc.encoded};
    MemoryTiffStream stream(
        BuildThunderTiff({.width = tc.width, .height = 1}, strips));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    const std::vector<uint8_t> expected = PackNibbles(tc.expected_nibbles);
    std::vector<uint8_t> row(expected.size(), 0xaa);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
    EXPECT_EQ(row, expected);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifThunderTest, DecodesMultiRowAndMultiStripAcrossEndiannessAndBigTiff) {
  struct FormatCase {
    std::string_view name;
    bool big_endian;
    bool big_tiff;
  };
  constexpr std::array<FormatCase, 4> kFormats = {{
      {.name = "ClassicLe", .big_endian = false, .big_tiff = false},
      {.name = "ClassicBe", .big_endian = true, .big_tiff = false},
      {.name = "BigTiffLe", .big_endian = false, .big_tiff = true},
      {.name = "BigTiffBe", .big_endian = true, .big_tiff = true},
  }};

  constexpr uint32_t kWidth = 7;
  constexpr uint32_t kHeight = 5;
  constexpr uint32_t kRowsPerStrip = 3;

  // Row 0 ends with lastpixel = 14; Row 1 starts with EncodeRun(7) to verify
  // lastpixel resets to 0 on each new scanline within the same strip.
  const std::vector<uint8_t> strip0_encoded = {
      // Row 0: {2, 5, 5, 5, 4, 7, 14}
      EncodeRaw(2),
      EncodeThreeBitDeltas(kDelta3Plus3, kDelta3Zero),
      EncodeRun(1),
      EncodeRaw(4),
      EncodeRaw(7),
      EncodeRaw(14),
      // Row 1: {0, 0, 0, 0, 0, 0, 0}
      EncodeRun(7),
      // Row 2: {9, 10, 10, 9, 11, 8, 8}
      EncodeRaw(9),
      EncodeTwoBitDeltas(kDelta2Plus1, kDelta2Zero, kDelta2Minus1),
      EncodeThreeBitDeltas(kDelta3Plus2, kDelta3Minus3),
      EncodeRun(1),
  };
  const std::vector<uint8_t> strip1_encoded = {
      // Row 3: {15, 15, 15, 12, 13, 14, 15}
      EncodeRaw(15),
      EncodeRun(2),
      EncodeThreeBitDeltas(kDelta3Minus3, kDelta3Plus1),
      EncodeTwoBitDeltas(kDelta2Plus1, kDelta2Plus1, kDelta2Skip),
      // Row 4: {1, 2, 3, 4, 5, 6, 7}
      EncodeRaw(1),
      EncodeTwoBitDeltas(kDelta2Plus1, kDelta2Plus1, kDelta2Plus1),
      EncodeThreeBitDeltas(kDelta3Plus1, kDelta3Plus1),
      EncodeRaw(7),
  };
  const std::vector<std::vector<uint8_t>> strips = {strip0_encoded,
                                                    strip1_encoded};

  const std::array<std::vector<uint8_t>, kHeight> expected_rows = {{
      PackNibbles({2, 5, 5, 5, 4, 7, 14}),
      PackNibbles({0, 0, 0, 0, 0, 0, 0}),
      PackNibbles({9, 10, 10, 9, 11, 8, 8}),
      PackNibbles({15, 15, 15, 12, 13, 14, 15}),
      PackNibbles({1, 2, 3, 4, 5, 6, 7}),
  }};

  std::vector<uint8_t> expected_strip0;
  for (uint32_t r = 0; r < 3; ++r) {
    expected_strip0.insert(expected_strip0.end(), expected_rows[r].begin(),
                           expected_rows[r].end());
  }
  std::vector<uint8_t> expected_strip1;
  for (uint32_t r = 3; r < 5; ++r) {
    expected_strip1.insert(expected_strip1.end(), expected_rows[r].begin(),
                           expected_rows[r].end());
  }

  for (const FormatCase& fmt : kFormats) {
    SCOPED_TRACE(fmt.name);
    MemoryTiffStream stream(BuildThunderTiff({.width = kWidth,
                                              .height = kHeight,
                                              .rows_per_strip = kRowsPerStrip,
                                              .big_endian = fmt.big_endian,
                                              .big_tiff = fmt.big_tiff},
                                             strips));

    // 1. Encoded strip reads.
    {
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      EXPECT_EQ(TIFFIsByteSwapped(reader.get()), fmt.big_endian ? 1 : 0);

      uint16_t compression = 0;
      ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &compression),
                1);
      EXPECT_EQ(compression, COMPRESSION_THUNDERSCAN);

      std::vector<uint8_t> strip0_out(expected_strip0.size(), 0xaa);
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip0_out.data(),
                                     static_cast<tmsize_t>(-1)),
                static_cast<tmsize_t>(expected_strip0.size()));
      EXPECT_EQ(strip0_out, expected_strip0);

      std::vector<uint8_t> strip1_out(expected_strip1.size(), 0xaa);
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 1, strip1_out.data(),
                                     static_cast<tmsize_t>(-1)),
                static_cast<tmsize_t>(expected_strip1.size()));
      EXPECT_EQ(strip1_out, expected_strip1);
    }

    // 2. Sequential scanline reads across strip boundaries.
    {
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      for (uint32_t row_idx = 0; row_idx < kHeight; ++row_idx) {
        SCOPED_TRACE(row_idx);
        std::vector<uint8_t> row_out(expected_rows[row_idx].size(), 0xaa);
        EXPECT_EQ(TIFFReadScanline(reader.get(), row_out.data(), row_idx, 0),
                  1);
        EXPECT_EQ(row_out, expected_rows[row_idx]);
      }
    }

    // 3. Memory-mapped read and TIFFReadFromUserBuffer.
    stream.set_enable_mmap(true);
    {
      UniqueTiff mapped = stream.Open("rM");
      ASSERT_NE(mapped, nullptr);
      EXPECT_GT(stream.map_calls(), 0U);

      std::vector<uint8_t> strip0_out(expected_strip0.size(), 0xaa);
      EXPECT_EQ(TIFFReadEncodedStrip(mapped.get(), 0, strip0_out.data(),
                                     static_cast<tmsize_t>(-1)),
                static_cast<tmsize_t>(expected_strip0.size()));
      EXPECT_EQ(strip0_out, expected_strip0);

      std::vector<uint8_t> user_in = strip1_encoded;
      std::vector<uint8_t> user_out(expected_strip1.size(), 0xaa);
      EXPECT_EQ(TIFFReadFromUserBuffer(mapped.get(), 1, user_in.data(),
                                       static_cast<tmsize_t>(user_in.size()),
                                       user_out.data(),
                                       static_cast<tmsize_t>(user_out.size())),
                1);
      EXPECT_EQ(user_out, expected_strip1);
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifThunderTest, DecodesSeparatePlanarConfiguration) {
  constexpr uint32_t kWidth = 6;
  constexpr uint32_t kHeight = 2;
  const std::vector<uint8_t> plane0_encoded = {
      EncodeRaw(1), EncodeRaw(2), EncodeRaw(3), EncodeRaw(4),
      EncodeRaw(5), EncodeRaw(6), EncodeRaw(7), EncodeRun(5),
  };
  const std::vector<uint8_t> plane1_encoded = {
      EncodeRaw(15), EncodeRun(5),  EncodeRaw(10), EncodeRaw(11),
      EncodeRaw(12), EncodeRaw(13), EncodeRaw(14), EncodeRaw(15),
  };
  const std::vector<std::vector<uint8_t>> strips = {plane0_encoded,
                                                    plane1_encoded};

  MemoryTiffStream stream(
      BuildThunderTiff({.width = kWidth,
                        .height = kHeight,
                        .rows_per_strip = kHeight,
                        .bits_per_sample = 4,
                        .samples_per_pixel = 2,
                        .planar_config = PLANARCONFIG_SEPARATE},
                       strips));
  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);

  const std::vector<uint8_t> expected_s0_r0 = PackNibbles({1, 2, 3, 4, 5, 6});
  const std::vector<uint8_t> expected_s0_r1 = PackNibbles({7, 7, 7, 7, 7, 7});
  const std::vector<uint8_t> expected_s1_r0 =
      PackNibbles({15, 15, 15, 15, 15, 15});
  const std::vector<uint8_t> expected_s1_r1 =
      PackNibbles({10, 11, 12, 13, 14, 15});

  std::vector<uint8_t> row(3, 0);
  EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
  EXPECT_EQ(row, expected_s0_r0);
  EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 1, 0), 1);
  EXPECT_EQ(row, expected_s0_r1);
  EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 1), 1);
  EXPECT_EQ(row, expected_s1_r0);
  EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 1, 1), 1);
  EXPECT_EQ(row, expected_s1_r1);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifThunderTest, SetupDecodeRejectsNonFourBitsPerSample) {
  for (uint16_t bps : {uint16_t{1}, uint16_t{2}, uint16_t{8}, uint16_t{16}}) {
    SCOPED_TRACE(bps);
    const std::vector<std::vector<uint8_t>> strips = {
        {EncodeRaw(1), EncodeRaw(2), EncodeRaw(3), EncodeRaw(4)}};
    MemoryTiffStream stream(BuildThunderTiff(
        {.width = 4, .height = 1, .bits_per_sample = bps}, strips));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    const tmsize_t scanline_size = TIFFScanlineSize(reader.get());
    ASSERT_GT(scanline_size, 0);
    std::vector<uint8_t> row(static_cast<size_t>(scanline_size), 0x55);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "ThunderSetupDecode",
        "Thunder decoder only supports 4bits per sample"));

    stream.diagnostics().Clear();
    std::vector<uint8_t> strip(static_cast<size_t>(scanline_size), 0x55);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip.data(),
                                   static_cast<tmsize_t>(-1)),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "ThunderSetupDecode",
        "Thunder decoder only supports 4bits per sample"));

    stream.diagnostics().Clear();
    std::vector<uint8_t> user_in = strips[0];
    EXPECT_EQ(TIFFReadFromUserBuffer(reader.get(), 0, user_in.data(),
                                     static_cast<tmsize_t>(user_in.size()),
                                     strip.data(),
                                     static_cast<tmsize_t>(strip.size())),
              0);
    EXPECT_TRUE(HasDiagnosticContaining(
        stream.diagnostics().errors(), "ThunderSetupDecode",
        "Thunder decoder only supports 4bits per sample"));
  }
}

TEST(TifThunderTest, DecodeRowRejectsFractionalScanlines) {
  constexpr uint32_t kWidth = 8;
  constexpr uint32_t kHeight = 2;
  const std::vector<uint8_t> strip_encoded = {
      EncodeRaw(1), EncodeRaw(2), EncodeRaw(3), EncodeRaw(4), EncodeRaw(5),
      EncodeRaw(6), EncodeRaw(7), EncodeRaw(8), EncodeRaw(9), EncodeRun(7),
  };
  const std::vector<std::vector<uint8_t>> strips = {strip_encoded};
  MemoryTiffStream stream(BuildThunderTiff(
      {.width = kWidth, .height = kHeight, .rows_per_strip = kHeight}, strips));
  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  ASSERT_EQ(TIFFScanlineSize(reader.get()), 4);

  std::vector<uint8_t> buf(8, 0xaa);
  EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, buf.data(), 3),
            static_cast<tmsize_t>(-1));
  EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                      "ThunderDecodeRow",
                                      "Fractional scanlines cannot be read"));

  stream.diagnostics().Clear();
  EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, buf.data(), 5),
            static_cast<tmsize_t>(-1));
  EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                      "ThunderDecodeRow",
                                      "Fractional scanlines cannot be read"));

  // Exact single-scanline read from a 2-row strip succeeds.
  stream.diagnostics().Clear();
  std::vector<uint8_t> one_row(4, 0xaa);
  EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, one_row.data(), 4), 4);
  EXPECT_EQ(one_row, PackNibbles({1, 2, 3, 4, 5, 6, 7, 8}));
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  // Zero-byte read is an exact multiple of scanline size (0 % 4 == 0).
  EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, one_row.data(), 0), 0);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  // Fractional outsize in TIFFReadFromUserBuffer also fails in
  // ThunderDecodeRow.
  std::vector<uint8_t> user_in = strip_encoded;
  EXPECT_EQ(TIFFReadFromUserBuffer(reader.get(), 0, user_in.data(),
                                   static_cast<tmsize_t>(user_in.size()),
                                   buf.data(), 3),
            0);
  EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                      "ThunderDecodeRow",
                                      "Fractional scanlines cannot be read"));
}

TEST(TifThunderTest, DecodeRejectsNotEnoughAndTooMuchData) {
  // 1. Short data: 2 pixels decoded for width 6; tail bytes are zero-filled.
  {
    const std::vector<std::vector<uint8_t>> strips = {
        {EncodeRaw(9), EncodeRaw(3)}};
    MemoryTiffStream stream(
        BuildThunderTiff({.width = 6, .height = 1}, strips));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> row(3, 0xaa);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
    EXPECT_EQ(row, (std::vector<uint8_t>{0x93, 0x00, 0x00}));
    EXPECT_TRUE(
        HasDiagnosticContaining(stream.diagnostics().errors(), "ThunderDecode",
                                "Not enough data at scanline 0 (2 != 6)"));

    stream.diagnostics().Clear();
    std::vector<uint8_t> strip(3, 0xaa);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip.data(),
                                   static_cast<tmsize_t>(-1)),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(strip, (std::vector<uint8_t>{0x93, 0x00, 0x00}));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "ThunderDecode", "Not enough data"));
  }

  // 2. Single no-op byte produces 0 pixels -> whole row buffer zero-filled.
  {
    const std::vector<std::vector<uint8_t>> strips = {{EncodeRun(0)}};
    MemoryTiffStream stream(
        BuildThunderTiff({.width = 6, .height = 1}, strips));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> row(3, 0xaa);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
    EXPECT_EQ(row, (std::vector<uint8_t>{0x00, 0x00, 0x00}));
    EXPECT_TRUE(
        HasDiagnosticContaining(stream.diagnostics().errors(), "ThunderDecode",
                                "Not enough data at scanline 0 (0 != 6)"));
  }

  // 3. Multi-row strip where row 0 succeeds and row 1 runs out of raw bytes.
  {
    const std::vector<std::vector<uint8_t>> strips = {
        {EncodeRaw(1), EncodeRaw(2), EncodeRaw(3), EncodeRaw(4)}};
    MemoryTiffStream stream(BuildThunderTiff(
        {.width = 4, .height = 2, .rows_per_strip = 2}, strips));
    {
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      std::vector<uint8_t> strip(4, 0xaa);
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip.data(),
                                     static_cast<tmsize_t>(-1)),
                static_cast<tmsize_t>(-1));
      EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                          "ThunderDecode", "Not enough data"));
    }
    stream.diagnostics().Clear();
    {
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      std::vector<uint8_t> row(2, 0xaa);
      EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
      EXPECT_EQ(row, (std::vector<uint8_t>{0x12, 0x34}));
      EXPECT_EQ(stream.diagnostics().error_count(), 0U);

      EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 1, 0), -1);
      EXPECT_EQ(row, (std::vector<uint8_t>{0x00, 0x00}));
      EXPECT_TRUE(HasDiagnosticContaining(
          stream.diagnostics().errors(), "ThunderDecode",
          "Not enough data at scanline 1 (0 != 4)"));
    }
  }

  // 4. Non-emitting input in TIFFReadFromUserBuffer hits cc == 0 before
  // reaching width pixels (avoiding insize == 0 due to tif_rawdataloaded == 0
  // fallback in TIFFStartStrip).
  {
    const std::vector<std::vector<uint8_t>> strips = {
        {EncodeRaw(1), EncodeRaw(2), EncodeRaw(3), EncodeRaw(4)}};
    MemoryTiffStream stream(
        BuildThunderTiff({.width = 4, .height = 1}, strips));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 1> noop_in = {EncodeRun(0)};
    std::vector<uint8_t> out(2, 0xaa);
    EXPECT_EQ(TIFFReadFromUserBuffer(reader.get(), 0, noop_in.data(),
                                     static_cast<tmsize_t>(noop_in.size()),
                                     out.data(), 2),
              0);
    EXPECT_EQ(out, (std::vector<uint8_t>{0x00, 0x00}));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "ThunderDecode", "Not enough data"));
  }

  // 5. Run overflow ("Too much data") from even and odd pixel positions.
  struct OverflowCase {
    std::string_view name;
    uint32_t width;
    std::vector<uint8_t> encoded;
    std::vector<uint8_t> expected_partial_bytes;
    std::string_view expected_substr;
  };

  const std::array<OverflowCase, 3> kOverflowCases = {{
      {.name = "EvenPositionOverflow",
       .width = 4,
       .encoded = {EncodeRaw(5), EncodeRaw(6), EncodeRun(4)},
       .expected_partial_bytes = {0x56, 0x00},
       .expected_substr = "Too much data at scanline 0 (6 != 4)"},
      {.name = "OddPositionExactBoundaryOverflow",
       .width = 4,
       .encoded = {EncodeRaw(1), EncodeRaw(2), EncodeRaw(3), EncodeRun(2)},
       .expected_partial_bytes = {0x12, 0x33},
       .expected_substr = "Too much data at scanline 0 (5 != 4)"},
      {.name = "OddPositionOverflowZeroesTail",
       .width = 5,
       .encoded = {EncodeRaw(1), EncodeRaw(2), EncodeRaw(3), EncodeRun(4)},
       .expected_partial_bytes = {0x12, 0x33, 0x00},
       .expected_substr = "Too much data at scanline 0 (7 != 5)"},
  }};

  for (const OverflowCase& tc : kOverflowCases) {
    SCOPED_TRACE(tc.name);
    const std::vector<std::vector<uint8_t>> strips = {tc.encoded};
    MemoryTiffStream stream(
        BuildThunderTiff({.width = tc.width, .height = 1}, strips));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> row(tc.expected_partial_bytes.size(), 0xaa);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
    EXPECT_EQ(row, tc.expected_partial_bytes);
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "ThunderDecode", tc.expected_substr));

    stream.diagnostics().Clear();
    std::vector<uint8_t> strip(tc.expected_partial_bytes.size(), 0xaa);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip.data(),
                                   static_cast<tmsize_t>(-1)),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(stream.diagnostics().errors(),
                                        "ThunderDecode", tc.expected_substr));
  }
}

TEST(TifThunderTest, AllocationLimitsFailCleanlyWithoutLeaks) {
  DiagnosticRecorder recorder;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts, nullptr);
  recorder.Install(opts.get());
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), kDefaultAllocBudget);

  const std::vector<std::vector<uint8_t>> strips = {
      {EncodeRaw(3), EncodeRaw(7), EncodeRaw(11), EncodeRaw(15)}};
  MemoryTiffStream stream(BuildThunderTiff({.width = 4, .height = 1}, strips));
  UniqueTiff reader = stream.Open("r", opts.get());
  ASSERT_NE(reader, nullptr);

  {
    ScopedSingleMemAllocLimit tiny_single(reader.get(), 32);
    std::vector<uint8_t> row(2, 0);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
  }
  EXPECT_GT(recorder.error_count(), 0U);
  recorder.Clear();

  {
    ScopedCumulatedMemAllocRemaining tight_budget(reader.get(), 64);
    std::vector<uint8_t> row(2, 0);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
  }
  EXPECT_GT(recorder.error_count(), 0U);
  recorder.Clear();

  std::vector<uint8_t> row(2, 0);
  EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
  EXPECT_EQ(row, (std::vector<uint8_t>{0x37, 0xbf}));
  EXPECT_EQ(recorder.error_count(), 0U);
}

TEST(TifThunderTest, EncodeWithThunderScanReportsNotImplemented) {
  MemoryTiffStream stream;
  UniqueTiff writer = stream.Open("wl");
  ASSERT_NE(writer, nullptr);

  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{4}), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{2}), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{4}), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
            1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{2}), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                         uint16_t{PLANARCONFIG_CONTIG}),
            1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                         uint16_t{PHOTOMETRIC_MINISBLACK}),
            1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                         uint16_t{COMPRESSION_THUNDERSCAN}),
            1);

  std::array<uint8_t, 2> row = {0x12, 0x34};
  EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), -1);
  EXPECT_TRUE(HasDiagnosticContaining(
      stream.diagnostics().errors(), "memory.tiff",
      "ThunderScan scanline encoding is not implemented"));
}

}  // namespace
}  // namespace libtiff
