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

// Unit tests for libtiff/tif_fax3sm.c (generated CCITT T.4/T.6 lookup tables
// TIFFFaxMainTable[128], TIFFFaxWhiteTable[4096], TIFFFaxBlackTable[8192]) and
// in-memory CCITT Fax3/Fax4/RLE codec round-trip usage.
//
// Note: tif_fax3sm.c contains only const lookup table data (LF=0 in LCOV).
// tif_fax3.h declares TIFFFaxMainTable, TIFFFaxWhiteTable, and
// TIFFFaxBlackTable outside its extern "C" block, so tif_fax3.h must be
// included at global scope outside namespace libtiff.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
// Include tif_fax3.h after gmock/gunit so its GetBits macro does not collide
// with transitive headers under --config=no_modules.
#include "third_party/tiff/libtiff/tif_fax3.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;

constexpr size_t kMainTableSize = 128;
constexpr size_t kWhiteTableSize = 4096;
constexpr size_t kBlackTableSize = 8192;

// Converts a MSB-first CCITT bitstring (e.g., "00110101") into the LSB-first
// lookup index expected by BitAcc / GetBits() in tif_fax3.h.
constexpr uint16_t BitsMsbToLsbPrefix(std::string_view bits) {
  uint16_t prefix = 0;
  for (size_t i = 0; i < bits.size(); ++i) {
    if (bits[i] == '1') {
      prefix |= static_cast<uint16_t>(1U << i);
    }
  }
  return prefix;
}

struct MainCodeExpectation {
  std::string_view bits;
  unsigned char state;
  uint32_t param;
};

constexpr auto kMainTableCodes = std::to_array<MainCodeExpectation>({
    {.bits = "1", .state = S_V0, .param = 0},
    {.bits = "001", .state = S_Horiz, .param = 0},
    {.bits = "0001", .state = S_Pass, .param = 0},
    {.bits = "010", .state = S_VL, .param = 1},
    {.bits = "011", .state = S_VR, .param = 1},
    {.bits = "000010", .state = S_VL, .param = 2},
    {.bits = "000011", .state = S_VR, .param = 2},
    {.bits = "0000010", .state = S_VL, .param = 3},
    {.bits = "0000011", .state = S_VR, .param = 3},
    {.bits = "0000001", .state = S_Ext, .param = 0},
    {.bits = "0000000", .state = S_EOL, .param = 0},
});

struct HuffmanCodeExpectation {
  std::string_view bits;
  unsigned char state;
  uint32_t param;
};

constexpr auto kRepresentativeWhiteCodes =
    std::to_array<HuffmanCodeExpectation>({
        {.bits = "00110101", .state = S_TermW, .param = 0},
        {.bits = "000111", .state = S_TermW, .param = 1},
        {.bits = "0111", .state = S_TermW, .param = 2},
        {.bits = "1000", .state = S_TermW, .param = 3},
        {.bits = "1111", .state = S_TermW, .param = 7},
        {.bits = "00000010", .state = S_TermW, .param = 29},
        {.bits = "00110100", .state = S_TermW, .param = 63},
        {.bits = "11011", .state = S_MakeUpW, .param = 64},
        {.bits = "10010", .state = S_MakeUpW, .param = 128},
        {.bits = "0110111", .state = S_MakeUpW, .param = 256},
        {.bits = "011000", .state = S_MakeUpW, .param = 1664},
        {.bits = "010011011", .state = S_MakeUpW, .param = 1728},
        {.bits = "00000001000", .state = S_MakeUp, .param = 1792},
        {.bits = "000000011111", .state = S_MakeUp, .param = 2560},
        {.bits = "00000000000", .state = S_EOL, .param = 0},
    });

constexpr auto kRepresentativeBlackCodes =
    std::to_array<HuffmanCodeExpectation>({
        {.bits = "0000110111", .state = S_TermB, .param = 0},
        {.bits = "010", .state = S_TermB, .param = 1},
        {.bits = "11", .state = S_TermB, .param = 2},
        {.bits = "10", .state = S_TermB, .param = 3},
        {.bits = "000011000", .state = S_TermB, .param = 15},
        {.bits = "000001100111", .state = S_TermB, .param = 63},
        {.bits = "0000001111", .state = S_MakeUpB, .param = 64},
        {.bits = "000011001000", .state = S_MakeUpB, .param = 128},
        {.bits = "0000001101100", .state = S_MakeUpB, .param = 512},
        {.bits = "0000001100101", .state = S_MakeUpB, .param = 1728},
        {.bits = "00000001000", .state = S_MakeUp, .param = 1792},
        {.bits = "000000011111", .state = S_MakeUp, .param = 2560},
        {.bits = "00000000000", .state = S_EOL, .param = 0},
    });

// Verifies that every suffix extension of a table's prefix code produces the
// exact same {State, Width, Param} entry.
void VerifyPrefixExpansion(const TIFFFaxTabEnt* table, size_t table_bits,
                           std::string_view bits, unsigned char expected_state,
                           uint32_t expected_param) {
  const size_t width = bits.size();
  ASSERT_LE(width, table_bits);
  const uint16_t prefix = BitsMsbToLsbPrefix(bits);
  const size_t suffix_count = size_t{1} << (table_bits - width);
  for (size_t suffix = 0; suffix < suffix_count; ++suffix) {
    const size_t idx = prefix | (suffix << width);
    SCOPED_TRACE(testing::Message() << "bits=" << bits << " idx=" << idx);
    EXPECT_EQ(table[idx].State, expected_state);
    EXPECT_EQ(table[idx].Width, width);
    EXPECT_EQ(table[idx].Param, expected_param);
  }
}

// Writes and reads back a 1-bit-per-sample bilevel image in memory using
// scanline I/O and asserts exact pixel-for-pixel equality.
void VerifyBilevelScanlineRoundTrip(
    uint16_t compression, uint32_t width, uint32_t height,
    const std::vector<std::vector<uint8_t>>& rows,
    uint16_t fill_order = FILLORDER_MSB2LSB,
    uint16_t photometric = PHOTOMETRIC_MINISWHITE, uint32_t group3_options = 0,
    int fax_mode = -1, float y_resolution = 0.0f, const char* write_mode = "wl",
    uint32_t rows_per_strip = 0) {
  ASSERT_EQ(rows.size(), height);
  const size_t expected_row_bytes = (width + 7) / 8;
  for (uint32_t r = 0; r < height; ++r) {
    ASSERT_EQ(rows[r].size(), expected_row_bytes) << "row " << r;
  }

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open(write_mode);
    ASSERT_NE(writer.get(), nullptr);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, width), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, height), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, compression), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, photometric), 1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_FILLORDER, fill_order), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP,
                           rows_per_strip > 0 ? rows_per_strip : height),
              1);
    if (compression == COMPRESSION_CCITTFAX3 && group3_options != 0) {
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_GROUP3OPTIONS, group3_options), 1);
    }
    if (fax_mode >= 0) {
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_FAXMODE, fax_mode), 1);
    }
    if (y_resolution > 0.0f) {
      EXPECT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH), 1);
      EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_YRESOLUTION,
                             static_cast<double>(y_resolution)),
                1);
    }

    for (uint32_t r = 0; r < height; ++r) {
      SCOPED_TRACE(testing::Message() << "write row=" << r);
      std::vector<uint8_t> mutable_row = rows[r];
      ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row.data(), r, 0), 1);
    }
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader.get(), nullptr);

  uint16_t got_compression = 0;
  uint16_t got_bps = 0;
  uint16_t got_spp = 0;
  uint32_t got_width = 0;
  uint32_t got_height = 0;
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &got_compression),
            1);
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_BITSPERSAMPLE, &got_bps), 1);
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_SAMPLESPERPIXEL, &got_spp), 1);
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEWIDTH, &got_width), 1);
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGELENGTH, &got_height), 1);
  EXPECT_EQ(got_compression, compression);
  EXPECT_EQ(got_bps, 1);
  EXPECT_EQ(got_spp, 1);
  EXPECT_EQ(got_width, width);
  EXPECT_EQ(got_height, height);
  ASSERT_EQ(TIFFScanlineSize(reader.get()),
            static_cast<tmsize_t>(expected_row_bytes));

  std::vector<uint8_t> decoded(expected_row_bytes, 0);
  for (uint32_t r = 0; r < height; ++r) {
    SCOPED_TRACE(testing::Message() << "read row=" << r);
    std::fill(decoded.begin(), decoded.end(), 0);
    ASSERT_EQ(TIFFReadScanline(reader.get(), decoded.data(), r, 0), 1);
    EXPECT_EQ(decoded, rows[r]);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifFax3SmTest, TabEntStructLayoutMatchesAbiContract) {
  // tif_fax3.h documents that external consumers (e.g. HylaFAX) depend on the
  // exact layout and 32-bit Param width of TIFFFaxTabEnt.
  EXPECT_EQ(sizeof(TIFFFaxTabEnt), 8U);
  EXPECT_EQ(alignof(TIFFFaxTabEnt), 4U);
  EXPECT_EQ(offsetof(TIFFFaxTabEnt, State), 0U);
  EXPECT_EQ(offsetof(TIFFFaxTabEnt, Width), 1U);
  EXPECT_EQ(offsetof(TIFFFaxTabEnt, Param), 4U);

  const void* main_ptr = static_cast<const void*>(TIFFFaxMainTable);
  const void* white_ptr = static_cast<const void*>(TIFFFaxWhiteTable);
  const void* black_ptr = static_cast<const void*>(TIFFFaxBlackTable);
  EXPECT_NE(main_ptr, nullptr);
  EXPECT_NE(white_ptr, nullptr);
  EXPECT_NE(black_ptr, nullptr);
  EXPECT_NE(main_ptr, white_ptr);
  EXPECT_NE(main_ptr, black_ptr);
  EXPECT_NE(white_ptr, black_ptr);
}

TEST(TifFax3SmTest, MainTableCoversAllSevenBitPrefixesExactly) {
  std::array<bool, kMainTableSize> visited{};
  for (const auto& code : kMainTableCodes) {
    const size_t width = code.bits.size();
    ASSERT_GE(width, 1U);
    ASSERT_LE(width, 7U);
    const uint16_t prefix = BitsMsbToLsbPrefix(code.bits);
    const size_t suffix_count = size_t{1} << (7 - width);
    for (size_t suffix = 0; suffix < suffix_count; ++suffix) {
      const size_t idx = prefix | (suffix << width);
      SCOPED_TRACE(testing::Message()
                   << "bits=" << code.bits << " idx=" << idx);
      EXPECT_FALSE(visited[idx]) << "Overlapping prefix code";
      visited[idx] = true;
      EXPECT_EQ(TIFFFaxMainTable[idx].State, code.state);
      EXPECT_EQ(TIFFFaxMainTable[idx].Width, width);
      EXPECT_EQ(TIFFFaxMainTable[idx].Param, code.param);
    }
  }
  EXPECT_TRUE(
      std::all_of(visited.begin(), visited.end(), [](bool v) { return v; }));
}

TEST(TifFax3SmTest, WhiteTableEntriesSatisfyPrefixAndRangeInvariants) {
  std::set<uint32_t> term_runs;
  std::set<uint32_t> makeup_runs;
  std::set<uint32_t> common_makeup_runs;
  std::set<std::pair<uint16_t, uint8_t>> unique_prefixes;
  size_t null_count = 0;
  size_t eol_count = 0;

  for (size_t i = 0; i < kWhiteTableSize; ++i) {
    SCOPED_TRACE(testing::Message() << "idx=" << i);
    const TIFFFaxTabEnt& ent = TIFFFaxWhiteTable[i];
    switch (ent.State) {
      case S_Null:
        ++null_count;
        EXPECT_EQ(ent.Width, 0);
        EXPECT_EQ(ent.Param, 0U);
        EXPECT_EQ(i & 0xffU, 0U);
        EXPECT_NE(i & 0x7ffU, 0U);
        break;
      case S_EOL:
        ++eol_count;
        EXPECT_EQ(ent.Width, 11);
        EXPECT_EQ(ent.Param, 0U);
        EXPECT_EQ(i & 0x7ffU, 0U);
        break;
      case S_TermW:
        EXPECT_GE(ent.Width, 4);
        EXPECT_LE(ent.Width, 8);
        EXPECT_LE(ent.Param, 63U);
        term_runs.insert(ent.Param);
        break;
      case S_MakeUpW:
        EXPECT_GE(ent.Width, 5);
        EXPECT_LE(ent.Width, 9);
        EXPECT_GE(ent.Param, 64U);
        EXPECT_LE(ent.Param, 1728U);
        EXPECT_EQ(ent.Param % 64U, 0U);
        makeup_runs.insert(ent.Param);
        break;
      case S_MakeUp:
        EXPECT_GE(ent.Width, 11);
        EXPECT_LE(ent.Width, 12);
        EXPECT_GE(ent.Param, 1792U);
        EXPECT_LE(ent.Param, 2560U);
        EXPECT_EQ(ent.Param % 64U, 0U);
        common_makeup_runs.insert(ent.Param);
        break;
      default:
        ADD_FAILURE() << "Unexpected state " << static_cast<int>(ent.State);
        break;
    }

    if (ent.Width > 0) {
      ASSERT_LE(ent.Width, 12);
      const uint16_t mask = static_cast<uint16_t>((1U << ent.Width) - 1U);
      const uint16_t prefix = static_cast<uint16_t>(i & mask);
      unique_prefixes.emplace(prefix, ent.Width);

      const size_t suffix_count = size_t{1} << (12 - ent.Width);
      for (size_t suffix = 0; suffix < suffix_count; ++suffix) {
        const size_t other = prefix | (suffix << ent.Width);
        EXPECT_EQ(TIFFFaxWhiteTable[other].State, ent.State);
        EXPECT_EQ(TIFFFaxWhiteTable[other].Width, ent.Width);
        EXPECT_EQ(TIFFFaxWhiteTable[other].Param, ent.Param);
      }
    }
  }

  EXPECT_EQ(null_count, 14U);
  EXPECT_EQ(eol_count, 2U);
  EXPECT_EQ(term_runs.size(), 64U);
  for (uint32_t run = 0; run <= 63; ++run) {
    EXPECT_TRUE(term_runs.contains(run)) << "Missing white term run " << run;
  }
  EXPECT_EQ(makeup_runs.size(), 27U);
  for (uint32_t run = 64; run <= 1728; run += 64) {
    EXPECT_TRUE(makeup_runs.contains(run))
        << "Missing white makeup run " << run;
  }
  EXPECT_EQ(common_makeup_runs.size(), 13U);
  for (uint32_t run = 1792; run <= 2560; run += 64) {
    EXPECT_TRUE(common_makeup_runs.contains(run))
        << "Missing white common makeup run " << run;
  }

  size_t kraft_sum = null_count;
  for (const auto& [prefix, width] : unique_prefixes) {
    (void)prefix;
    kraft_sum += size_t{1} << (12 - width);
  }
  EXPECT_EQ(kraft_sum, kWhiteTableSize);
}

TEST(TifFax3SmTest, WhiteTableMatchesRepresentativeT4HuffmanCodes) {
  for (const auto& code : kRepresentativeWhiteCodes) {
    VerifyPrefixExpansion(TIFFFaxWhiteTable, 12, code.bits, code.state,
                          code.param);
  }
}

TEST(TifFax3SmTest, BlackTableEntriesSatisfyPrefixAndRangeInvariants) {
  std::set<uint32_t> term_runs;
  std::set<uint32_t> makeup_runs;
  std::set<uint32_t> common_makeup_runs;
  std::set<std::pair<uint16_t, uint8_t>> unique_prefixes;
  size_t null_count = 0;
  size_t eol_count = 0;

  for (size_t i = 0; i < kBlackTableSize; ++i) {
    SCOPED_TRACE(testing::Message() << "idx=" << i);
    const TIFFFaxTabEnt& ent = TIFFFaxBlackTable[i];
    switch (ent.State) {
      case S_Null:
        ++null_count;
        EXPECT_EQ(ent.Width, 0);
        EXPECT_EQ(ent.Param, 0U);
        EXPECT_EQ(i & 0xffU, 0U);
        EXPECT_NE(i & 0x7ffU, 0U);
        break;
      case S_EOL:
        ++eol_count;
        EXPECT_EQ(ent.Width, 11);
        EXPECT_EQ(ent.Param, 0U);
        EXPECT_EQ(i & 0x7ffU, 0U);
        break;
      case S_TermB:
        EXPECT_GE(ent.Width, 2);
        EXPECT_LE(ent.Width, 12);
        EXPECT_LE(ent.Param, 63U);
        term_runs.insert(ent.Param);
        break;
      case S_MakeUpB:
        EXPECT_GE(ent.Width, 10);
        EXPECT_LE(ent.Width, 13);
        EXPECT_GE(ent.Param, 64U);
        EXPECT_LE(ent.Param, 1728U);
        EXPECT_EQ(ent.Param % 64U, 0U);
        makeup_runs.insert(ent.Param);
        break;
      case S_MakeUp:
        EXPECT_GE(ent.Width, 11);
        EXPECT_LE(ent.Width, 12);
        EXPECT_GE(ent.Param, 1792U);
        EXPECT_LE(ent.Param, 2560U);
        EXPECT_EQ(ent.Param % 64U, 0U);
        common_makeup_runs.insert(ent.Param);
        break;
      default:
        ADD_FAILURE() << "Unexpected state " << static_cast<int>(ent.State);
        break;
    }

    if (ent.Width > 0) {
      ASSERT_LE(ent.Width, 13);
      const uint16_t mask = static_cast<uint16_t>((1U << ent.Width) - 1U);
      const uint16_t prefix = static_cast<uint16_t>(i & mask);
      unique_prefixes.emplace(prefix, ent.Width);

      const size_t suffix_count = size_t{1} << (13 - ent.Width);
      for (size_t suffix = 0; suffix < suffix_count; ++suffix) {
        const size_t other = prefix | (suffix << ent.Width);
        EXPECT_EQ(TIFFFaxBlackTable[other].State, ent.State);
        EXPECT_EQ(TIFFFaxBlackTable[other].Width, ent.Width);
        EXPECT_EQ(TIFFFaxBlackTable[other].Param, ent.Param);
      }
    }
  }

  EXPECT_EQ(null_count, 28U);
  EXPECT_EQ(eol_count, 4U);
  EXPECT_EQ(term_runs.size(), 64U);
  for (uint32_t run = 0; run <= 63; ++run) {
    EXPECT_TRUE(term_runs.contains(run)) << "Missing black term run " << run;
  }
  EXPECT_EQ(makeup_runs.size(), 27U);
  for (uint32_t run = 64; run <= 1728; run += 64) {
    EXPECT_TRUE(makeup_runs.contains(run))
        << "Missing black makeup run " << run;
  }
  EXPECT_EQ(common_makeup_runs.size(), 13U);
  for (uint32_t run = 1792; run <= 2560; run += 64) {
    EXPECT_TRUE(common_makeup_runs.contains(run))
        << "Missing black common makeup run " << run;
  }

  size_t kraft_sum = null_count;
  for (const auto& [prefix, width] : unique_prefixes) {
    (void)prefix;
    kraft_sum += size_t{1} << (13 - width);
  }
  EXPECT_EQ(kraft_sum, kBlackTableSize);
}

TEST(TifFax3SmTest, BlackTableMatchesRepresentativeT4HuffmanCodes) {
  for (const auto& code : kRepresentativeBlackCodes) {
    VerifyPrefixExpansion(TIFFFaxBlackTable, 13, code.bits, code.state,
                          code.param);
  }
}

TEST(TifFax3SmTest, Fax3OneDimensionalRoundTripExercisesLongAndShortRuns) {
  // Use 2720 pixels (340 bytes) per row so all-white and all-black rows require
  // extended make-up codes (>= 2560 + 128 + 32) alongside short terminating
  // runs and alternating bit patterns.
  constexpr uint32_t kWidth = 2720;
  constexpr uint32_t kRowBytes = kWidth / 8;
  constexpr uint32_t kHeight = 8;

  std::vector<std::vector<uint8_t>> rows(kHeight,
                                         std::vector<uint8_t>(kRowBytes, 0x00));
  // Row 0: all white (0x00).
  // Row 1: all black (0xff).
  std::fill(rows[1].begin(), rows[1].end(), 0xff);
  // Row 2: alternating single-pixel runs (0xaa).
  std::fill(rows[2].begin(), rows[2].end(), 0xaa);
  // Row 3: inverted alternating single-pixel runs (0x55).
  std::fill(rows[3].begin(), rows[3].end(), 0x55);
  // Row 4: 64-bit white run, 16-bit black run, remainder white.
  std::fill(rows[4].begin() + 8, rows[4].begin() + 10, 0xff);
  // Row 5: 1728-bit black prefix (216 bytes), then alternating bytes.
  std::fill(rows[5].begin(), rows[5].begin() + 216, 0xff);
  for (uint32_t c = 216; c < kRowBytes; ++c) {
    rows[5][c] = static_cast<uint8_t>((c & 1U) ? 0x0f : 0xf0);
  }
  // Row 6: 2560-bit white prefix (320 bytes), then 160 black bits.
  std::fill(rows[6].begin() + 320, rows[6].end(), 0xff);
  // Row 7: pseudo-random deterministic byte sequence.
  for (uint32_t c = 0; c < kRowBytes; ++c) {
    rows[7][c] = static_cast<uint8_t>((c * 73U + 19U) & 0xffU);
  }

  VerifyBilevelScanlineRoundTrip(COMPRESSION_CCITTFAX3, kWidth, kHeight, rows,
                                 FILLORDER_MSB2LSB, PHOTOMETRIC_MINISWHITE, 0,
                                 FAXMODE_CLASSF, 0.0f, "wl");
  VerifyBilevelScanlineRoundTrip(
      COMPRESSION_CCITTFAX3, kWidth, kHeight, rows, FILLORDER_LSB2MSB,
      PHOTOMETRIC_MINISBLACK, GROUP3OPT_FILLBITS, FAXMODE_CLASSIC, 0.0f, "wb");
  VerifyBilevelScanlineRoundTrip(COMPRESSION_CCITTFAX3, kWidth, kHeight, rows,
                                 FILLORDER_MSB2LSB, PHOTOMETRIC_MINISWHITE,
                                 GROUP3OPT_FILLBITS, FAXMODE_NORTC, 0.0f, "w8");
}

TEST(TifFax3SmTest, Fax3TwoDimensionalRoundTripExercisesAllMainTableModes) {
  constexpr uint32_t kWidth = 256;
  constexpr uint32_t kRowBytes = kWidth / 8;
  constexpr uint32_t kHeight = 10;

  std::vector<std::vector<uint8_t>> rows(kHeight,
                                         std::vector<uint8_t>(kRowBytes, 0x00));
  // Craft transitions between adjacent rows to trigger V0, VR(1..3), VL(1..3),
  // Pass (skipping a black run on the reference line), and Horizontal runs.
  rows[0][0] = 0xf0;  // bits 0..3 black
  rows[1][0] = 0xf0;  // V0 alignment with row 0
  rows[2][0] = 0xf8;  // VR(1): right edge moves 4 -> 5
  rows[3][0] = 0xff;  // VR(3): edge moves 5 -> 8
  rows[4][0] = 0xe0;  // VL(>3 -> Horizontal) or VL shifts
  rows[5][0] = 0xc0;  // VL(1): 3 -> 2
  rows[6][0] = 0x00;  // Pass / Horizontal transition (all white)
  rows[6][4] = 0xff;  // Black segment later in row 6
  rows[6][5] = 0xff;
  rows[7][0] = 0x00;   // Pass over row 6's black segment (bits 32..47)
  rows[8][10] = 0x3c;  // Horizontal white-first then black
  rows[8][11] = 0xff;
  rows[9][10] = 0x3c;  // V0 + black-first horizontal continuation
  rows[9][15] = 0xaa;

  VerifyBilevelScanlineRoundTrip(COMPRESSION_CCITTFAX3, kWidth, kHeight, rows,
                                 FILLORDER_MSB2LSB, PHOTOMETRIC_MINISWHITE,
                                 GROUP3OPT_2DENCODING, -1, 0.0f, "wl");
  VerifyBilevelScanlineRoundTrip(COMPRESSION_CCITTFAX3, kWidth, kHeight, rows,
                                 FILLORDER_LSB2MSB, PHOTOMETRIC_MINISWHITE,
                                 GROUP3OPT_2DENCODING | GROUP3OPT_FILLBITS,
                                 FAXMODE_CLASSIC, 200.0f, "wb");
}

TEST(TifFax3SmTest, Fax4StripAndTileRoundTripSucceeds) {
  constexpr uint32_t kWidth = 2704;
  constexpr uint32_t kRowBytes = kWidth / 8;
  constexpr uint32_t kHeight = 8;

  std::vector<std::vector<uint8_t>> rows(kHeight,
                                         std::vector<uint8_t>(kRowBytes, 0x00));
  rows[0][0] = 0xff;
  rows[1][0] = 0xfe;                                // VL(1)
  rows[2][0] = 0xfc;                                // VL(1)
  rows[3][0] = 0xff;                                // VR(2)
  rows[4][0] = 0x00;                                // Pass
  std::fill(rows[5].begin(), rows[5].end(), 0xff);  // Long 2704-bit black run.
  rows[6][100] = 0x55;
  rows[7][100] = 0xaa;

  VerifyBilevelScanlineRoundTrip(COMPRESSION_CCITTFAX4, kWidth, kHeight, rows,
                                 FILLORDER_MSB2LSB, PHOTOMETRIC_MINISWHITE, 0,
                                 -1, 0.0f, "wl");
  VerifyBilevelScanlineRoundTrip(COMPRESSION_CCITTFAX4, kWidth, kHeight, rows,
                                 FILLORDER_LSB2MSB, PHOTOMETRIC_MINISBLACK, 0,
                                 -1, 0.0f, "w8");

  // Also test tiled Group 4 write + read round-trip.
  constexpr uint32_t kTileW = 16;
  constexpr uint32_t kTileH = 16;
  constexpr uint32_t kImgW = 32;
  constexpr uint32_t kImgH = 32;
  const std::vector<uint8_t> expected_tile = []() {
    std::vector<uint8_t> buf((kTileW / 8) * kTileH, 0);
    for (size_t i = 0; i < buf.size(); ++i) {
      buf[i] = static_cast<uint8_t>((i * 37U + 11U) & 0xffU);
    }
    return buf;
  }();

  MemoryTiffStream tile_stream;
  {
    UniqueTiff writer = tile_stream.Open("wl");
    ASSERT_NE(writer.get(), nullptr);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kImgW), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kImgH), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, kTileW), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, kTileH), 1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{1}),
              1);
    EXPECT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4),
        1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
        1);
    EXPECT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);

    const uint32_t tile_count = TIFFNumberOfTiles(writer.get());
    ASSERT_EQ(tile_count, 4U);
    for (uint32_t t = 0; t < tile_count; ++t) {
      std::vector<uint8_t> mutable_tile = expected_tile;
      ASSERT_EQ(
          TIFFWriteEncodedTile(writer.get(), t, mutable_tile.data(),
                               static_cast<tmsize_t>(mutable_tile.size())),
          static_cast<tmsize_t>(expected_tile.size()));
    }
  }
  EXPECT_EQ(tile_stream.diagnostics().error_count(), 0U);

  UniqueTiff reader = tile_stream.Open("r");
  ASSERT_NE(reader.get(), nullptr);
  const tmsize_t tile_size = TIFFTileSize(reader.get());
  ASSERT_EQ(tile_size, static_cast<tmsize_t>(expected_tile.size()));
  std::vector<uint8_t> decoded_tile(static_cast<size_t>(tile_size), 0);
  for (uint32_t t = 0; t < TIFFNumberOfTiles(reader.get()); ++t) {
    SCOPED_TRACE(testing::Message() << "tile=" << t);
    std::fill(decoded_tile.begin(), decoded_tile.end(), 0);
    ASSERT_EQ(
        TIFFReadEncodedTile(reader.get(), t, decoded_tile.data(), tile_size),
        tile_size);
    EXPECT_EQ(decoded_tile, expected_tile);
  }
  EXPECT_EQ(tile_stream.diagnostics().error_count(), 0U);
}

TEST(TifFax3SmTest, CcittRleAndRleWordAlignedRoundTripSucceeds) {
  constexpr uint32_t kWidth = 128;
  constexpr uint32_t kRowBytes = kWidth / 8;
  constexpr uint32_t kHeight = 4;

  std::vector<std::vector<uint8_t>> rows(kHeight,
                                         std::vector<uint8_t>(kRowBytes, 0x00));
  std::fill(rows[1].begin(), rows[1].end(), 0xff);
  std::fill(rows[2].begin(), rows[2].end(), 0xc3);
  for (uint32_t c = 0; c < kRowBytes; ++c) {
    rows[3][c] = static_cast<uint8_t>((c * 29U + 7U) & 0xffU);
  }

  VerifyBilevelScanlineRoundTrip(COMPRESSION_CCITTRLE, kWidth, kHeight, rows,
                                 FILLORDER_MSB2LSB, PHOTOMETRIC_MINISWHITE);
  // Note: upstream Fax3DecodeRLE (libtiff/tif_fax3.c) discards up to 15 bits
  // including pre-read bytes from the next row when FAXMODE_WORDALIGN is used
  // with multiple rows per strip; use 1 row per strip for CCITTRLEW.
  VerifyBilevelScanlineRoundTrip(COMPRESSION_CCITTRLEW, kWidth, kHeight, rows,
                                 FILLORDER_MSB2LSB, PHOTOMETRIC_MINISWHITE, 0,
                                 -1, 0.0f, "wl", /*rows_per_strip=*/1);
}

TEST(TifFax3SmTest, CorruptAndTruncatedFaxStreamsFailOrReportDiagnostics) {
  auto build_single_strip_fax = [](uint16_t compression, uint32_t width,
                                   uint32_t height,
                                   std::vector<uint8_t> strip_bytes) {
    RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
    const uint64_t offset = builder.AppendBlob(strip_bytes);
    std::vector<RawIfdEntry> entries = {
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 1),
        builder.MakeShort(TIFFTAG_COMPRESSION, compression),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(offset)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, height),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(strip_bytes.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    };
    builder.AddIfd(std::move(entries));
    return builder.Take();
  };

  // 1. Truncated streams hitting premature EOF with BitsAvail == 0 return -1.
  {
    // Fax3 EOL (12 bits) + 2560-pixel white makeup code (12 bits) = 24 bits
    // for a 4096-pixel row -> BitsAvail == 0 at EndOfData() -> premature EOF.
    MemoryTiffStream stream(build_single_strip_fax(COMPRESSION_CCITTFAX3, 4096,
                                                   1, {0x00, 0x10, 0x1f}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    std::array<uint8_t, 512> row{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_GE(stream.diagnostics().warning_count(), 1U);
  }
  {
    // CCITTRLE: two 2560-pixel white makeup codes (24 bits) for an 8192-pixel
    // row -> BitsAvail == 0 at EndOfData() -> premature EOF.
    MemoryTiffStream stream(build_single_strip_fax(COMPRESSION_CCITTRLE, 8192,
                                                   1, {0x01, 0xf0, 0x1f}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    std::array<uint8_t, 1024> row{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_GE(stream.diagnostics().warning_count(), 1U);
  }
  {
    MemoryTiffStream stream(
        build_single_strip_fax(COMPRESSION_CCITTFAX4, 64, 1, {0x00}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    std::array<uint8_t, 8> row{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), -1);
  }

  // 2. Single 0x00 byte triggers zero-padded S_EOL on line 0 and returns 1
  // with premature-EOL warning for Fax3/CCITTRLE.
  for (uint16_t compression : {COMPRESSION_CCITTFAX3, COMPRESSION_CCITTRLE}) {
    SCOPED_TRACE(testing::Message() << "compression=" << compression);
    MemoryTiffStream stream(build_single_strip_fax(compression, 64, 1, {0x00}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    std::array<uint8_t, 8> row{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), 1);
    EXPECT_GE(stream.diagnostics().warning_count(), 1U);
  }

  // 3. Fax3 1D EOL followed by invalid white code prefix 000000001 (S_Null in
  // TIFFFaxWhiteTable) triggers "Bad code word" error diagnostic without
  // UB/leak.
  {
    // MSB-first: 00000000 0001_0000 0000_1000 00000000 -> EOL then invalid
    // code.
    MemoryTiffStream stream(build_single_strip_fax(COMPRESSION_CCITTFAX3, 64, 1,
                                                   {0x00, 0x10, 0x08, 0x00}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    std::array<uint8_t, 8> row{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), 1);
    ASSERT_GE(stream.diagnostics().error_count(), 1U);
    EXPECT_THAT(stream.diagnostics().errors().front().message,
                HasSubstr("Bad code word"));
  }

  // 4. Fax4 stream starting with S_Ext prefix 0000001 (index 64 in
  // TIFFFaxMainTable) triggers unsupported uncompressed extension diagnostic.
  {
    // MSB-first: 00000010 00000000.
    MemoryTiffStream stream(
        build_single_strip_fax(COMPRESSION_CCITTFAX4, 64, 1, {0x02, 0x00}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif.get(), nullptr);
    std::array<uint8_t, 8> row{};
    EXPECT_EQ(TIFFReadScanline(tif.get(), row.data(), 0, 0), 1);
    ASSERT_GE(stream.diagnostics().error_count(), 1U);
    EXPECT_THAT(stream.diagnostics().errors().front().message,
                HasSubstr("Uncompressed data (not supported)"));
  }
}

}  // namespace
}  // namespace libtiff
