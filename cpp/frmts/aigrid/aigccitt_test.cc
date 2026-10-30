// Tests for aigccitt:
//   https://github.com/OSGeo/gdal/blob/master/frmts/aigrid/aigccitt.c
//
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

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/aigrid/aigrid.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

struct CapturedError {
  CPLErr err_class;
  CPLErrorNum err_num;
  std::string message;
};

void CPL_STDCALL CaptureErrorHandler(CPLErr err_class, CPLErrorNum err_num,
                                     const char* msg) {
  auto* errors =
      static_cast<std::vector<CapturedError>*>(CPLGetErrorHandlerUserData());
  if (errors != nullptr) {
    errors->push_back({err_class, err_num, msg != nullptr ? msg : ""});
  }
}

// Helper to pack CCITT G3 1D Huffman codes MSB-first into bytes.
// Because DecompressCCITTRLETile reverses each input byte via
// aig_TIFFBitRevTable, emitting bit `b` of `table_index` (for b = 0..width-1)
// MSB-first places `table_index` directly into the low `width` bits of
// BitAcc when decoded.
class BitWriter {
 public:
  void AppendCode(uint16_t table_index, int width) {
    for (int b = 0; b < width; ++b) {
      const uint8_t bit = static_cast<uint8_t>((table_index >> b) & 1u);
      current_byte_ |= static_cast<uint8_t>(bit << (7 - bits_in_byte_));
      ++bits_in_byte_;
      if (bits_in_byte_ == 8) {
        bytes_.push_back(current_byte_);
        current_byte_ = 0;
        bits_in_byte_ = 0;
      }
    }
  }

  void AlignToByte() {
    if (bits_in_byte_ > 0) {
      bytes_.push_back(current_byte_);
      current_byte_ = 0;
      bits_in_byte_ = 0;
    }
  }

  std::vector<unsigned char>& bytes() { return bytes_; }

 private:
  std::vector<unsigned char> bytes_;
  uint8_t current_byte_ = 0;
  int bits_in_byte_ = 0;
};

// Verified indices and bit widths in aig_TIFFFaxWhiteTable / BlackTable.
struct Code {
  uint16_t index;
  int width;
};

constexpr Code kWhiteEOL = {0, 11};
constexpr Code kWhite0 = {172, 8};
constexpr Code kWhite2 = {14, 4};
constexpr Code kWhite3 = {1, 4};
constexpr Code kWhite5 = {3, 4};
constexpr Code kWhite6 = {7, 4};
constexpr Code kWhite7 = {15, 4};
constexpr Code kWhite11 = {2, 5};
constexpr Code kWhite28 = {12, 7};
constexpr Code kWhiteMakeUp64 = {27, 5};
constexpr Code kWhiteMakeUp128 = {9, 5};
constexpr Code kWhiteInvalid = {256, 12};

constexpr Code kBlackEOL = {0, 11};
constexpr Code kBlack0 = {944, 10};
constexpr Code kBlack1 = {2, 3};
constexpr Code kBlack2 = {3, 2};
constexpr Code kBlack3 = {1, 2};
constexpr Code kBlack4 = {6, 3};
constexpr Code kBlack5 = {12, 4};
constexpr Code kBlack6 = {4, 4};
constexpr Code kBlackMakeUp64 = {960, 10};
constexpr Code kBlackMakeUp384 = {704, 12};
constexpr Code kBlackMakeUp1856 = {384, 11};
constexpr Code kBlackInvalid = {512, 13};

bool GetBit(const std::vector<unsigned char>& buf, int bit_index) {
  return (buf[bit_index >> 3] & (0x80u >> (bit_index & 7))) != 0;
}

TEST(AigccittTest, RunBufferTooSmall) {
  std::vector<CapturedError> errors;
  CPLPushErrorHandlerEx(CaptureErrorHandler, &errors);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  unsigned char src[4] = {0};
  unsigned char dst[256] = {0};
  // runs_buf is 4000 bytes; nBlockXSize * 2 + 3 > 4000 triggers failure.
  EXPECT_EQ(CE_Failure, DecompressCCITTRLETile(src, sizeof(src), dst,
                                               sizeof(dst), 1999, 1));
  ASSERT_FALSE(errors.empty());
  EXPECT_THAT(errors.back().message, HasSubstr("Run buffer too small"));
}

TEST(AigccittTest, FractionalScanlinesRejected) {
  std::vector<CapturedError> errors;
  CPLPushErrorHandlerEx(CaptureErrorHandler, &errors);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  unsigned char src[4] = {0};
  unsigned char dst[5] = {0};
  // nBlockXSize = 16 -> rowbytes = 2; nDstBytes = 5 is not a multiple of 2.
  EXPECT_EQ(CE_Failure,
            DecompressCCITTRLETile(src, sizeof(src), dst, sizeof(dst), 16, 2));
  ASSERT_FALSE(errors.empty());
  EXPECT_THAT(errors.back().message,
              HasSubstr("Fractional scanlines cannot be read"));
}

TEST(AigccittTest, MultiRowByteAlignedAndOddRunCount) {
  BitWriter writer;
  // Row 0 (16 px): 3 white, 5 black, 2 white, 6 black -> 0x1F, 0x3F.
  writer.AppendCode(kWhite3.index, kWhite3.width);
  writer.AppendCode(kBlack5.index, kBlack5.width);
  writer.AppendCode(kWhite2.index, kWhite2.width);
  writer.AppendCode(kBlack6.index, kBlack6.width);
  writer.AlignToByte();

  // Row 1 (16 px): 0 white, 4 black, 11 white, 1 black -> 0xF0, 0x01.
  writer.AppendCode(kWhite0.index, kWhite0.width);
  writer.AppendCode(kBlack4.index, kBlack4.width);
  writer.AppendCode(kWhite11.index, kWhite11.width);
  writer.AppendCode(kBlack1.index, kBlack1.width);
  writer.AlignToByte();

  // Row 2 (16 px, ends on white run -> odd run count): 7 white, 3 black, 6
  // white
  // -> 0000000_1 (0x01), 11_000000 (0xC0).
  writer.AppendCode(kWhite7.index, kWhite7.width);
  writer.AppendCode(kBlack3.index, kBlack3.width);
  writer.AppendCode(kWhite6.index, kWhite6.width);
  writer.AlignToByte();

  std::vector<unsigned char> dst(6, 0xAA);
  ASSERT_EQ(CE_None,
            DecompressCCITTRLETile(
                writer.bytes().data(), static_cast<int>(writer.bytes().size()),
                dst.data(), static_cast<int>(dst.size()), 16, 3));
  EXPECT_THAT(dst, testing::ElementsAre(0x1F, 0x3F, 0xF0, 0x01, 0x01, 0xC0));
}

TEST(AigccittTest, ZeroLengthWhiteAndBlackPairCollapsed) {
  BitWriter writer;
  // 8 px: 2 white, 2 black, 0 white, 0 black, 2 white, 2 black -> 00110011 =
  // 0x33.
  writer.AppendCode(kWhite2.index, kWhite2.width);
  writer.AppendCode(kBlack2.index, kBlack2.width);
  writer.AppendCode(kWhite0.index, kWhite0.width);
  writer.AppendCode(kBlack0.index, kBlack0.width);
  writer.AppendCode(kWhite2.index, kWhite2.width);
  writer.AppendCode(kBlack2.index, kBlack2.width);
  writer.AlignToByte();

  std::vector<unsigned char> dst(1, 0xFF);
  ASSERT_EQ(CE_None,
            DecompressCCITTRLETile(
                writer.bytes().data(), static_cast<int>(writer.bytes().size()),
                dst.data(), static_cast<int>(dst.size()), 8, 1));
  EXPECT_EQ(0x33, dst[0]);
}

TEST(AigccittTest, LongRunsMakeUpCodesAndWordAlignedFills) {
  // 600 px (75 bytes per row): exercises MakeUpW, MakeUpB, >16-byte longword
  // fills with unaligned starting byte pointers, and medium (<=15 byte) fills.
  constexpr int kWidth = 600;
  constexpr int kRowBytes = (kWidth + 7) / 8;
  BitWriter writer;

  // Row 0:
  //   11 white (x = 0..11)
  //   3 black  (x = 11..14)
  //   131 white = 128 + 3 (x = 14..145; >16 bytes, unaligned start)
  //   388 black = 384 + 4 (x = 145..533; >16 bytes, unaligned start)
  //   67 white  = 64 + 3  (x = 533..600; <=15 bytes)
  writer.AppendCode(kWhite11.index, kWhite11.width);
  writer.AppendCode(kBlack3.index, kBlack3.width);
  writer.AppendCode(kWhiteMakeUp128.index, kWhiteMakeUp128.width);
  writer.AppendCode(kWhite3.index, kWhite3.width);
  writer.AppendCode(kBlackMakeUp384.index, kBlackMakeUp384.width);
  writer.AppendCode(kBlack4.index, kBlack4.width);
  writer.AppendCode(kWhiteMakeUp64.index, kWhiteMakeUp64.width);
  writer.AppendCode(kWhite3.index, kWhite3.width);
  writer.AlignToByte();

  // Row 1: also exercise medium black fill (<=15 bytes: 64 + 5 = 69 black)
  // and byte-aligned start (x = 8):
  //   5 white (x = 0..5), 3 black (x = 5..8), 0 white,
  //   69 black = 64 + 5 (x = 8..77),
  //   523 white = 128*4 + 11 (x = 77..600).
  writer.AppendCode(kWhite5.index, kWhite5.width);
  writer.AppendCode(kBlack3.index, kBlack3.width);
  writer.AppendCode(kWhite0.index, kWhite0.width);
  writer.AppendCode(kBlackMakeUp64.index, kBlackMakeUp64.width);
  writer.AppendCode(kBlack5.index, kBlack5.width);
  for (int i = 0; i < 4; ++i) {
    writer.AppendCode(kWhiteMakeUp128.index, kWhiteMakeUp128.width);
  }
  writer.AppendCode(kWhite11.index, kWhite11.width);
  writer.AlignToByte();

  std::vector<unsigned char> dst(kRowBytes * 2, 0x55);
  ASSERT_EQ(CE_None,
            DecompressCCITTRLETile(
                writer.bytes().data(), static_cast<int>(writer.bytes().size()),
                dst.data(), static_cast<int>(dst.size()), kWidth, 2));

  // Verify Row 0 pixel transitions.
  for (int x = 0; x < 11; ++x) EXPECT_FALSE(GetBit(dst, x)) << "x=" << x;
  for (int x = 11; x < 14; ++x) EXPECT_TRUE(GetBit(dst, x)) << "x=" << x;
  for (int x = 14; x < 145; ++x) EXPECT_FALSE(GetBit(dst, x)) << "x=" << x;
  for (int x = 145; x < 533; ++x) EXPECT_TRUE(GetBit(dst, x)) << "x=" << x;
  for (int x = 533; x < 600; ++x) EXPECT_FALSE(GetBit(dst, x)) << "x=" << x;

  // Verify Row 1 pixel transitions.
  const std::vector<unsigned char> row1(dst.begin() + kRowBytes, dst.end());
  for (int x = 0; x < 5; ++x) EXPECT_FALSE(GetBit(row1, x)) << "x=" << x;
  for (int x = 5; x < 8; ++x) EXPECT_TRUE(GetBit(row1, x)) << "x=" << x;
  for (int x = 8; x < 77; ++x) EXPECT_TRUE(GetBit(row1, x)) << "x=" << x;
  for (int x = 77; x < 600; ++x) EXPECT_FALSE(GetBit(row1, x)) << "x=" << x;
}

TEST(AigccittTest, SharedMakeUpCode1856) {
  constexpr int kWidth = 1900;
  constexpr int kRowBytes = (kWidth + 7) / 8;
  BitWriter writer;
  // 11 white + (1856 + 3) black + 28 white + 2 black = 1900.
  writer.AppendCode(kWhite11.index, kWhite11.width);
  writer.AppendCode(kBlackMakeUp1856.index, kBlackMakeUp1856.width);
  writer.AppendCode(kBlack3.index, kBlack3.width);
  writer.AppendCode(kWhite28.index, kWhite28.width);
  writer.AppendCode(kBlack2.index, kBlack2.width);
  writer.AlignToByte();

  std::vector<unsigned char> dst(kRowBytes, 0);
  ASSERT_EQ(CE_None,
            DecompressCCITTRLETile(
                writer.bytes().data(), static_cast<int>(writer.bytes().size()),
                dst.data(), static_cast<int>(dst.size()), kWidth, 1));
  EXPECT_FALSE(GetBit(dst, 10));
  EXPECT_TRUE(GetBit(dst, 11));
  EXPECT_TRUE(GetBit(dst, 1869));
  EXPECT_FALSE(GetBit(dst, 1870));
  EXPECT_FALSE(GetBit(dst, 1897));
  EXPECT_TRUE(GetBit(dst, 1898));
  EXPECT_TRUE(GetBit(dst, 1899));
}

TEST(AigccittTest, EolExactPrematureAndMismatch) {
  // 1. Exact EOL when a0 == lastx (8 px: 5 white + 3 black + EOL).
  {
    std::vector<CapturedError> errors;
    CPLPushErrorHandlerEx(CaptureErrorHandler, &errors);
    absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

    BitWriter writer;
    writer.AppendCode(kWhite5.index, kWhite5.width);
    writer.AppendCode(kBlack3.index, kBlack3.width);
    writer.AppendCode(kWhiteEOL.index, kWhiteEOL.width);
    writer.AlignToByte();

    std::vector<unsigned char> dst(1, 0);
    EXPECT_EQ(CE_None, DecompressCCITTRLETile(
                           writer.bytes().data(),
                           static_cast<int>(writer.bytes().size()), dst.data(),
                           static_cast<int>(dst.size()), 8, 1));
    EXPECT_EQ(0x07, dst[0]);
    EXPECT_TRUE(errors.empty());
  }

  // 2. Premature EOL in white table (16 px: 3 white + 2 black + EOL).
  {
    std::vector<CapturedError> errors;
    CPLPushErrorHandlerEx(CaptureErrorHandler, &errors);
    absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

    BitWriter writer;
    writer.AppendCode(kWhite3.index, kWhite3.width);
    writer.AppendCode(kBlack2.index, kBlack2.width);
    writer.AppendCode(kWhiteEOL.index, kWhiteEOL.width);
    writer.AlignToByte();

    std::vector<unsigned char> dst(2, 0xFF);
    EXPECT_EQ(CE_None, DecompressCCITTRLETile(
                           writer.bytes().data(),
                           static_cast<int>(writer.bytes().size()), dst.data(),
                           static_cast<int>(dst.size()), 16, 1));
    // First 3 white, 2 black, remaining 11 padded white -> 0b00011000, 0x00.
    EXPECT_THAT(dst, testing::ElementsAre(0x18, 0x00));
    ASSERT_FALSE(errors.empty());
    EXPECT_THAT(errors.back().message, HasSubstr("Premature EOL"));
  }

  // 3. Premature EOL in black table (16 px: 3 white + BlackEOL; odd run count
  // before padding).
  {
    std::vector<CapturedError> errors;
    CPLPushErrorHandlerEx(CaptureErrorHandler, &errors);
    absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

    BitWriter writer;
    writer.AppendCode(kWhite3.index, kWhite3.width);
    writer.AppendCode(kBlackEOL.index, kBlackEOL.width);
    writer.AlignToByte();

    std::vector<unsigned char> dst(2, 0xFF);
    EXPECT_EQ(CE_None, DecompressCCITTRLETile(
                           writer.bytes().data(),
                           static_cast<int>(writer.bytes().size()), dst.data(),
                           static_cast<int>(dst.size()), 16, 1));
    EXPECT_THAT(dst, testing::ElementsAre(0x00, 0x00));
    ASSERT_FALSE(errors.empty());
    EXPECT_THAT(errors.back().message, HasSubstr("Premature EOL"));
  }

  // 4. Line length mismatch (8 px: 11 white -> a0 = 11 > 8).
  {
    std::vector<CapturedError> errors;
    CPLPushErrorHandlerEx(CaptureErrorHandler, &errors);
    absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

    BitWriter writer;
    writer.AppendCode(kWhite11.index, kWhite11.width);
    writer.AlignToByte();

    std::vector<unsigned char> dst(1, 0xFF);
    EXPECT_EQ(CE_None, DecompressCCITTRLETile(
                           writer.bytes().data(),
                           static_cast<int>(writer.bytes().size()), dst.data(),
                           static_cast<int>(dst.size()), 8, 1));
    EXPECT_EQ(0x00, dst[0]);
    ASSERT_FALSE(errors.empty());
    EXPECT_THAT(errors.back().message, HasSubstr("Line length mismatch"));
  }

  // 5. Pending RunLength flushed at CLEANUP_RUNS (64 px: MakeUpW(64) +
  // MakeUpW(64) + EOL -> RunLength == 128 > 64).
  {
    std::vector<CapturedError> errors;
    CPLPushErrorHandlerEx(CaptureErrorHandler, &errors);
    absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

    BitWriter writer;
    writer.AppendCode(kWhiteMakeUp64.index, kWhiteMakeUp64.width);
    writer.AppendCode(kWhiteMakeUp64.index, kWhiteMakeUp64.width);
    writer.AppendCode(kWhiteEOL.index, kWhiteEOL.width);
    writer.AlignToByte();

    std::vector<unsigned char> dst(8, 0xFF);
    EXPECT_EQ(CE_None, DecompressCCITTRLETile(
                           writer.bytes().data(),
                           static_cast<int>(writer.bytes().size()), dst.data(),
                           static_cast<int>(dst.size()), 64, 1));
    EXPECT_THAT(dst, testing::Each(0x00));
    ASSERT_FALSE(errors.empty());
    EXPECT_THAT(errors.back().message, HasSubstr("Line length mismatch"));
  }
}

TEST(AigccittTest, BadCodeWordsAndPrematureEof) {
  // 1. Invalid code word in white table.
  {
    std::vector<CapturedError> errors;
    CPLPushErrorHandlerEx(CaptureErrorHandler, &errors);
    absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

    BitWriter writer;
    writer.AppendCode(kWhiteInvalid.index, kWhiteInvalid.width);
    writer.AlignToByte();

    std::vector<unsigned char> dst(1, 0xFF);
    EXPECT_EQ(CE_None, DecompressCCITTRLETile(
                           writer.bytes().data(),
                           static_cast<int>(writer.bytes().size()), dst.data(),
                           static_cast<int>(dst.size()), 8, 1));
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ("Bad code word", errors.front().message);
  }

  // 2. Invalid code word in black table.
  {
    std::vector<CapturedError> errors;
    CPLPushErrorHandlerEx(CaptureErrorHandler, &errors);
    absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

    BitWriter writer;
    writer.AppendCode(kWhite3.index, kWhite3.width);
    writer.AppendCode(kBlackInvalid.index, kBlackInvalid.width);
    writer.AlignToByte();

    std::vector<unsigned char> dst(1, 0xFF);
    EXPECT_EQ(CE_None, DecompressCCITTRLETile(
                           writer.bytes().data(),
                           static_cast<int>(writer.bytes().size()), dst.data(),
                           static_cast<int>(dst.size()), 8, 1));
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ("Bad code word", errors.front().message);
  }

  // 3. Premature EOF on empty input.
  {
    std::vector<CapturedError> errors;
    CPLPushErrorHandlerEx(CaptureErrorHandler, &errors);
    absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

    unsigned char dummy = 0;
    std::vector<unsigned char> dst(1, 0xFF);
    EXPECT_EQ(CE_Failure,
              DecompressCCITTRLETile(&dummy, 0, dst.data(), 1, 8, 1));
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ("Premature EOF", errors.front().message);
  }
}

}  // namespace
}  // namespace autotest2
