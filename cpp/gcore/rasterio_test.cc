// Tests gcore/rasterio.cpp.
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_float.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

class TestDataset;

class TestRasterBand : public GDALRasterBand {
 public:
  TestRasterBand(TestDataset* poDSIn, int nBandIn, GDALDataType eDT, int nXSize,
                 int nYSize, int nBXSize, int nBYSize,
                 bool virtual_zero = false)
      : virtual_zero_(virtual_zero) {
    poDS = reinterpret_cast<GDALDataset*>(poDSIn);
    nBand = nBandIn;
    eDataType = eDT;
    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
    nBlockXSize = nBXSize;
    nBlockYSize = nBYSize;
    eAccess = GA_Update;
    if (nBlockXSize > 0 && nBlockYSize > 0) {
      InitBlockInfo();
    }
    if (!virtual_zero_ && nXSize > 0 && nYSize > 0) {
      const size_t dt_size = GDALGetDataTypeSizeBytes(eDT);
      data_.resize(static_cast<size_t>(nXSize) * nYSize * dt_size, 0);
    }
  }

  ~TestRasterBand() override { FlushCache(true); }

  void SetInvalidBlockSize() {
    nBlockXSize = 0;
    nBlockYSize = 0;
  }

  void SetFailReadBlock(bool fail) { fail_read_block_ = fail; }
  void SetFailWriteBlock(bool fail) { fail_write_block_ = fail; }
  void SetFailGetLockedBlock(bool fail, bool emit_custom_error = false) {
    fail_get_locked_block_ = fail;
    emit_custom_error_ = emit_custom_error;
  }

  GDALRasterBlock* GetLockedBlockRef(int nXBlockOff, int nYBlockOff,
                                     int bJustInitialize = FALSE) override {
    if (fail_get_locked_block_) {
      if (emit_custom_error_) {
        CPLError(CE_Failure, CPLE_AppDefined, "Custom GetLockedBlockRef error");
      }
      return nullptr;
    }
    return GDALRasterBand::GetLockedBlockRef(nXBlockOff, nYBlockOff,
                                             bJustInitialize);
  }

  CPLErr IReadBlock(int nBlockXOff, int nBlockYOff, void* pImage) override {
    if (fail_read_block_) {
      return CE_Failure;
    }
    const int dt_size = GDALGetDataTypeSizeBytes(eDataType);
    memset(pImage, 0, static_cast<size_t>(nBlockXSize) * nBlockYSize * dt_size);
    if (virtual_zero_) {
      return CE_None;
    }
    const int x0 = nBlockXOff * nBlockXSize;
    const int y0 = nBlockYOff * nBlockYSize;
    if (x0 >= nRasterXSize || y0 >= nRasterYSize) {
      return CE_None;
    }
    const int x_valid = std::min(nBlockXSize, nRasterXSize - x0);
    const int y_valid = std::min(nBlockYSize, nRasterYSize - y0);
    if (x_valid <= 0 || y_valid <= 0) {
      return CE_None;
    }
    for (int y = 0; y < y_valid; ++y) {
      const size_t src_offset =
          (static_cast<size_t>(y0 + y) * nRasterXSize + x0) * dt_size;
      const size_t nbytes = static_cast<size_t>(x_valid) * dt_size;
      if (src_offset + nbytes <= data_.size()) {
        memcpy(static_cast<GByte*>(pImage) +
                   static_cast<size_t>(y) * nBlockXSize * dt_size,
               data_.data() + src_offset, nbytes);
      }
    }
    return CE_None;
  }

  CPLErr IWriteBlock(int nBlockXOff, int nBlockYOff, void* pImage) override {
    if (fail_write_block_) {
      return CE_Failure;
    }
    if (virtual_zero_) {
      return CE_None;
    }
    const int dt_size = GDALGetDataTypeSizeBytes(eDataType);
    const int x0 = nBlockXOff * nBlockXSize;
    const int y0 = nBlockYOff * nBlockYSize;
    if (x0 >= nRasterXSize || y0 >= nRasterYSize) {
      return CE_None;
    }
    const int x_valid = std::min(nBlockXSize, nRasterXSize - x0);
    const int y_valid = std::min(nBlockYSize, nRasterYSize - y0);
    if (x_valid <= 0 || y_valid <= 0) {
      return CE_None;
    }
    for (int y = 0; y < y_valid; ++y) {
      const size_t dst_offset =
          (static_cast<size_t>(y0 + y) * nRasterXSize + x0) * dt_size;
      const size_t nbytes = static_cast<size_t>(x_valid) * dt_size;
      if (dst_offset + nbytes <= data_.size()) {
        memcpy(data_.data() + dst_offset,
               static_cast<const GByte*>(pImage) +
                   static_cast<size_t>(y) * nBlockXSize * dt_size,
               nbytes);
      }
    }
    return CE_None;
  }

  CPLErr CallIRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                       int nYSize, void* pData, int nBufXSize, int nBufYSize,
                       GDALDataType eBufType, GSpacing nPixelSpace,
                       GSpacing nLineSpace, GDALRasterIOExtraArg* psExtraArg) {
    return GDALRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                     pData, nBufXSize, nBufYSize, eBufType,
                                     nPixelSpace, nLineSpace, psExtraArg);
  }

  CPLErr CallRasterIOResampled(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                               int nXSize, int nYSize, void* pData,
                               int nBufXSize, int nBufYSize,
                               GDALDataType eBufType, GSpacing nPixelSpace,
                               GSpacing nLineSpace,
                               GDALRasterIOExtraArg* psExtraArg) {
    return GDALRasterBand::RasterIOResampled(
        eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize,
        eBufType, nPixelSpace, nLineSpace, psExtraArg);
  }

  CPLErr CallOverviewRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                              int nXSize, int nYSize, void* pData,
                              int nBufXSize, int nBufYSize,
                              GDALDataType eBufType, GSpacing nPixelSpace,
                              GSpacing nLineSpace,
                              GDALRasterIOExtraArg* psExtraArg) {
    return GDALRasterBand::OverviewRasterIO(
        eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize,
        eBufType, nPixelSpace, nLineSpace, psExtraArg);
  }

  CPLErr CallTryOverviewRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                                 int nXSize, int nYSize, void* pData,
                                 int nBufXSize, int nBufYSize,
                                 GDALDataType eBufType, GSpacing nPixelSpace,
                                 GSpacing nLineSpace,
                                 GDALRasterIOExtraArg* psExtraArg,
                                 int* pbTried) {
    return GDALRasterBand::TryOverviewRasterIO(
        eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize,
        eBufType, nPixelSpace, nLineSpace, psExtraArg, pbTried);
  }

  void AddOverview(GDALRasterBand* ovr) { overviews_.push_back(ovr); }
  int GetOverviewCount() override {
    return static_cast<int>(overviews_.size());
  }
  GDALRasterBand* GetOverview(int i) override {
    if (i < 0 || i >= static_cast<int>(overviews_.size())) return nullptr;
    return overviews_[i];
  }

  void SetTestColorTable(GDALColorTable* ct) { color_table_ = ct; }
  GDALColorTable* GetColorTable() override { return color_table_; }

  void SetTestNoData(double val) {
    has_nodata_ = true;
    nodata_ = val;
  }
  double GetNoDataValue(int* pbSuccess = nullptr) override {
    if (pbSuccess) *pbSuccess = has_nodata_ ? TRUE : FALSE;
    return nodata_;
  }

  void SetCoverageStatus(int status) { coverage_status_ = status; }
  int IGetDataCoverageStatus(int nXOff, int nYOff, int nXSize, int nYSize,
                             int nMaskFlagStop, double* pdfDataPct) override {
    if (pdfDataPct) {
      *pdfDataPct =
          (coverage_status_ & GDAL_DATA_COVERAGE_STATUS_DATA) ? 100.0 : 0.0;
    }
    return coverage_status_;
  }

  void SetSuggestedAccessPattern(GDALSuggestedBlockAccessPattern pattern) {
    access_pattern_ = pattern;
  }
  GDALSuggestedBlockAccessPattern GetSuggestedBlockAccessPattern()
      const override {
    return access_pattern_;
  }

  std::vector<GByte>& raw_data() { return data_; }

 private:
  bool virtual_zero_ = false;
  bool fail_read_block_ = false;
  bool fail_write_block_ = false;
  bool fail_get_locked_block_ = false;
  bool emit_custom_error_ = false;
  std::vector<GByte> data_;
  std::vector<GDALRasterBand*> overviews_;
  GDALColorTable* color_table_ = nullptr;
  bool has_nodata_ = false;
  double nodata_ = 0.0;
  int coverage_status_ = GDAL_DATA_COVERAGE_STATUS_DATA;
  GDALSuggestedBlockAccessPattern access_pattern_ = GSBAP_UNKNOWN;
};

class TestDataset : public GDALDataset {
 public:
  TestDataset(int nXSize, int nYSize) {
    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
    eAccess = GA_Update;
  }

  ~TestDataset() override { FlushCache(true); }

  void AddTestBand(GDALRasterBand* poBand) {
    SetBand(GetRasterCount() + 1, poBand);
  }

  CPLErr CallBlockBasedRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                                int nXSize, int nYSize, void* pData,
                                int nBufXSize, int nBufYSize,
                                GDALDataType eBufType, int nBandCount,
                                const int* panBandMap, GSpacing nPixelSpace,
                                GSpacing nLineSpace, GSpacing nBandSpace,
                                GDALRasterIOExtraArg* psExtraArg) {
    return GDALDataset::BlockBasedRasterIO(
        eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize,
        eBufType, nBandCount, panBandMap, nPixelSpace, nLineSpace, nBandSpace,
        psExtraArg);
  }

  CPLErr CallRasterIOResampled(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                               int nXSize, int nYSize, void* pData,
                               int nBufXSize, int nBufYSize,
                               GDALDataType eBufType, int nBandCount,
                               const int* panBandMap, GSpacing nPixelSpace,
                               GSpacing nLineSpace, GSpacing nBandSpace,
                               GDALRasterIOExtraArg* psExtraArg) {
    return GDALDataset::RasterIOResampled(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                          pData, nBufXSize, nBufYSize, eBufType,
                                          nBandCount, panBandMap, nPixelSpace,
                                          nLineSpace, nBandSpace, psExtraArg);
  }

  CPLErr CallTryOverviewRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                                 int nXSize, int nYSize, void* pData,
                                 int nBufXSize, int nBufYSize,
                                 GDALDataType eBufType, int nBandCount,
                                 const int* panBandMap, GSpacing nPixelSpace,
                                 GSpacing nLineSpace, GSpacing nBandSpace,
                                 GDALRasterIOExtraArg* psExtraArg,
                                 int* pbTried) {
    return GDALDataset::TryOverviewRasterIO(
        eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize,
        eBufType, nBandCount, panBandMap, nPixelSpace, nLineSpace, nBandSpace,
        psExtraArg, pbTried);
  }
};

int CPL_STDCALL FailingProgressCallback(double dfComplete,
                                        const char* /*pszMessage*/,
                                        void* pProgressArg) {
  double threshold = pProgressArg ? *static_cast<double*>(pProgressArg) : 0.0;
  return dfComplete >= threshold ? FALSE : TRUE;
}

TEST(RasterIoTest, GdalSwapWords) {
  // GdalSwapWords takes a block size of number of bytes (2, 4 or 8), a number
  // of blocks to swap and the number of bytes to advance.  If the number of
  // bytes to advance is 0, then the number of blocks must be 1.  This
  // function allows for gaps between swaps.
  // It only checks the input values for validity if DEBUG is defined.

  // Swapping single 2 byte value.
  {
    char buf[] = "123";
    GDALSwapWords(buf, 2, 1, 0);
    EXPECT_STREQ("213", buf);
  }

  {
    char buf[] = "12345";
    GDALSwapWords(buf, 4, 1, 0);
    EXPECT_STREQ("43215", buf);
  }

  {
    char buf[] = "1234567890";
    GDALSwapWords(buf, 8, 1, 0);
    EXPECT_STREQ("8765432190", buf);
  }

  // Swap 2 bytes, advance 2 bytes and swap 2 bytes.
  {
    char buf[] = "abcde";
    GDALSwapWords(buf, 2, 2, 2);
    EXPECT_STREQ("badce", buf);
  }

  // Swap 4 bytes, advance 4 bytes, swap 4 bytes.
  {
    char buf[] = "abcdefghi";
    GDALSwapWords(buf, 4, 2, 4);
    EXPECT_STREQ("dcbahgfei", buf);
  }

  {
    char buf[] = "abcdefghijklmnopq";
    GDALSwapWords(buf, 8, 2, 8);
    EXPECT_STREQ("hgfedcbaponmlkjiq", buf);
  }

  // Gap between words.
  {
    char buf[] = "123456";
    GDALSwapWords(buf, 2, 2, 3);
    EXPECT_STREQ("213546", buf);
  }

  {
    char buf[] = "1234567890";
    GDALSwapWords(buf, 4, 2, 5);
    EXPECT_STREQ("4321598760", buf);
  }

  {
    char buf[] = "1234567890123456789";
    GDALSwapWords(buf, 8, 2, 9);
    EXPECT_STREQ("8765432197654321089", buf);
  }
}

TEST(RasterIoTest, GdalSwapWordsExtended) {
  // WordSize = 1 (no-op)
  {
    char buf[] = "abcd";
    GDALSwapWords(buf, 1, 4, 1);
    EXPECT_STREQ("abcd", buf);
  }

  // WordCount = 0 with nullptr
  GDALSwapWords(nullptr, 4, 0, 4);

  // WordCount > 0 with nullptr (emits error via VALIDATE_POINTER0)
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALSwapWords(nullptr, 4, 1, 4);
  CPLPopErrorHandler();

  // Aligned and unaligned 4-byte and 8-byte swaps
  alignas(8) char aligned4[17] = "12345678abcdefgh";
  GDALSwapWords(aligned4, 4, 2, 4);
  EXPECT_EQ('4', aligned4[0]);
  EXPECT_EQ('1', aligned4[3]);
  EXPECT_EQ('8', aligned4[4]);
  EXPECT_EQ('5', aligned4[7]);

  // Unaligned 4-byte pointer with skip=4
  GDALSwapWords(aligned4 + 1, 4, 2, 4);

  // Aligned 8-byte pointer with skip=8
  alignas(8) char aligned8[25] = "12345678abcdefghABCDEFGH";
  GDALSwapWords(aligned8, 8, 2, 8);
  EXPECT_EQ('8', aligned8[0]);
  EXPECT_EQ('1', aligned8[7]);
  EXPECT_EQ('h', aligned8[8]);
  EXPECT_EQ('a', aligned8[15]);

  // Unaligned 8-byte pointer with skip=8
  GDALSwapWords(aligned8 + 1, 8, 2, 8);

  // GDALSwapWordsEx
  alignas(8) uint16_t ex_buf[4] = {0x0102, 0x0304, 0x0506, 0x0708};
  GDALSwapWordsEx(ex_buf, 2, 4, 2);
  EXPECT_EQ(0x0201, ex_buf[0]);
  EXPECT_EQ(0x0403, ex_buf[1]);
  EXPECT_EQ(0x0605, ex_buf[2]);
  EXPECT_EQ(0x0807, ex_buf[3]);
}

TEST(RasterIoTest, GdalCopyWordsAllTypesAndStrides) {
  const GDALDataType kAllTypes[] = {
      GDT_Byte,    GDT_Int8,     GDT_UInt16,   GDT_Int16,
      GDT_UInt32,  GDT_Int32,    GDT_UInt64,   GDT_Int64,
      GDT_Float16, GDT_Float32,  GDT_Float64,  GDT_CInt16,
      GDT_CInt32,  GDT_CFloat16, GDT_CFloat32, GDT_CFloat64,
  };

  // Prepare an aligned source buffer of 40 elements for each type, containing
  // normal values, negative values, large values, Inf, -Inf, NaN.
  constexpr int kCount = 40;
  const double kTestValues[12] = {
      0.0,   1.0,   -1.0,    42.5,    127.0,   128.0,
      255.0, 256.0, 32767.0, 32768.0, 65535.0, -32768.0,
  };

  alignas(16) GByte src_buf[kCount * 32];
  alignas(16) GByte dst_buf[kCount * 32];

  // Populate initial double buffer with interesting values including Inf/NaN.
  alignas(16) double init_doubles[kCount * 2];
  for (int i = 0; i < kCount * 2; ++i) {
    if (i < 12) {
      init_doubles[i] = kTestValues[i];
    } else if (i == 12) {
      init_doubles[i] = std::numeric_limits<double>::infinity();
    } else if (i == 13) {
      init_doubles[i] = -std::numeric_limits<double>::infinity();
    } else if (i == 14) {
      init_doubles[i] = std::numeric_limits<double>::quiet_NaN();
    } else if (i == 15) {
      init_doubles[i] = 3.0e9;
    } else if (i == 16) {
      init_doubles[i] = -3.0e9;
    } else {
      init_doubles[i] = static_cast<double>((i * 7) % 200);
    }
  }

  for (GDALDataType src_dt : kAllTypes) {
    const int src_size = GDALGetDataTypeSizeBytes(src_dt);
    // Initialize src_buf from init_doubles
    GDALCopyWords64(init_doubles, GDT_CFloat64, sizeof(double) * 2, src_buf,
                    src_dt, src_size, kCount);

    for (GDALDataType dst_dt : kAllTypes) {
      const int dst_size = GDALGetDataTypeSizeBytes(dst_dt);

      // 1. Packed stride (nWordCount = 40 -> exercises SIMD + scalar tail)
      memset(dst_buf, 0, sizeof(dst_buf));
      GDALCopyWords(src_buf, src_dt, src_size, dst_buf, dst_dt, dst_size,
                    kCount);

      // 2. Non-packed stride (2x size -> exercises GDALCopyWordsGenericT path)
      memset(dst_buf, 0, sizeof(dst_buf));
      GDALCopyWords64(src_buf, src_dt, src_size * 2, dst_buf, dst_dt,
                      dst_size * 2, 12);

      // 3. Single word (nWordCount = 1)
      GDALCopyWords64(src_buf, src_dt, src_size, dst_buf, dst_dt, dst_size, 1);

      // 4. Replicate single word (nSrcPixelStride = 0, nWordCount = 10)
      //    Both packed and non-packed destination stride.
      GDALCopyWords64(src_buf, src_dt, 0, dst_buf, dst_dt, dst_size, 10);
      GDALCopyWords64(src_buf, src_dt, 0, dst_buf, dst_dt, dst_size * 2, 10);
    }
  }

  // Test GDALFastCopy paths for 1-byte and 2-byte types with strides 1..5 *
  // sizeof(T)
  for (GDALDataType dt : {GDT_Byte, GDT_UInt16}) {
    const int sz = GDALGetDataTypeSizeBytes(dt);
    for (int src_mult = 1; src_mult <= 5; ++src_mult) {
      GDALCopyWords64(src_buf, dt, src_mult * sz, dst_buf, dt, 1 * sz, 36);
    }
    for (int dst_mult = 2; dst_mult <= 5; ++dst_mult) {
      GDALCopyWords64(src_buf, dt, 1 * sz, dst_buf, dt, dst_mult * sz, 36);
    }
    // Both src and dst strides > 1 * sz
    GDALCopyWords64(src_buf, dt, 2 * sz, dst_buf, dt, 3 * sz, 20);
  }

  // Test unaligned pointer / unaligned stride paths in GDALCopyWords64
  // Same type, different stride, unaligned
  GDALCopyWords64(src_buf + 1, GDT_Int32, 5, dst_buf + 1, GDT_Int32, 6, 8);
  // Different type, unaligned
  GDALCopyWords64(src_buf + 1, GDT_Int32, 5, dst_buf + 1, GDT_Float64, 9, 8);

  // Test GDT_Unknown / GDT_TypeCount error handling
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALCopyWords64(src_buf, GDT_Unknown, 1, dst_buf, GDT_Byte, 1, 4);
  GDALCopyWords64(src_buf, GDT_Byte, 1, dst_buf, GDT_TypeCount, 1, 4);
  CPLPopErrorHandler();
}

TEST(RasterIoTest, GdalCopyBitsAndExpandPackedBits) {
  // GDALCopyBits
  const GByte src_bits[4] = {0b10110011, 0b01011010, 0b11110000, 0b00001111};
  GByte dst_bits[4] = {0, 0, 0, 0};
  GDALCopyBits(src_bits, 0, 4, dst_bits, 0, 4, 4, 8);
  EXPECT_EQ(src_bits[0], dst_bits[0]);
  EXPECT_EQ(src_bits[1], dst_bits[1]);

  // With offset and step != bit_count
  memset(dst_bits, 0xFF, sizeof(dst_bits));
  GDALCopyBits(src_bits, 2, 6, dst_bits, 1, 5, 3, 4);

  // Nullptr check
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALCopyBits(nullptr, 0, 1, dst_bits, 0, 1, 1, 1);
  CPLPopErrorHandler();

  // GDALExpandPackedBitsToByteAt0Or255 and GDALExpandPackedBitsToByteAt0Or1
  // Use 149 bits (18 whole bytes + 5 trailing bits) to exercise SSE2 (16
  // bytes), whole-byte cleanup (2 bytes), and trailing bits (5 bits).
  constexpr size_t kNumBits = 149;
  std::vector<GByte> packed((kNumBits + 7) / 8);
  for (size_t i = 0; i < packed.size(); ++i) {
    packed[i] = static_cast<GByte>((i * 73 + 0xA5) & 0xFF);
  }
  std::vector<GByte> out_255(kNumBits, 123);
  std::vector<GByte> out_1(kNumBits, 123);

  GDALExpandPackedBitsToByteAt0Or255(packed.data(), out_255.data(), kNumBits);
  GDALExpandPackedBitsToByteAt0Or1(packed.data(), out_1.data(), kNumBits);

  for (size_t i = 0; i < kNumBits; ++i) {
    const bool bit = (packed[i / 8] & (1 << (7 - (i % 8)))) != 0;
    EXPECT_EQ(bit ? 255 : 0, out_255[i]) << "at bit " << i;
    EXPECT_EQ(bit ? 1 : 0, out_1[i]) << "at bit " << i;
  }
}

TEST(RasterIoTest, GdalCopyRasterIOExtraArg) {
  GDALRasterIOExtraArg dst;
  GDALCopyRasterIOExtraArg(&dst, nullptr);
  EXPECT_EQ(GRIORA_NearestNeighbour, dst.eResampleAlg);
  EXPECT_FALSE(dst.bFloatingPointWindowValidity);

  GDALRasterIOExtraArg src;
  INIT_RASTERIO_EXTRA_ARG(src);
  src.eResampleAlg = GRIORA_Bilinear;
  src.bFloatingPointWindowValidity = TRUE;
  src.dfXOff = 1.25;
  src.dfYOff = 2.5;
  src.dfXSize = 10.5;
  src.dfYSize = 20.5;
  src.nVersion = 2;
  src.bUseOnlyThisScale = true;

  GDALCopyRasterIOExtraArg(&dst, &src);
  EXPECT_EQ(GRIORA_Bilinear, dst.eResampleAlg);
  EXPECT_TRUE(dst.bFloatingPointWindowValidity);
  EXPECT_DOUBLE_EQ(1.25, dst.dfXOff);
  EXPECT_DOUBLE_EQ(2.5, dst.dfYOff);
  EXPECT_DOUBLE_EQ(10.5, dst.dfXSize);
  EXPECT_DOUBLE_EQ(20.5, dst.dfYSize);
  EXPECT_TRUE(dst.bUseOnlyThisScale);
}

TEST(RasterIoTest, GdalBufferHasOnlyNoData) {
  // 1. Optimized 0.0 path (nWidth == nLineStride, non-floating point)
  alignas(8) GByte zero_buf[64] = {0};
  // Unaligned start + > 8 bytes
  EXPECT_TRUE(GDALBufferHasOnlyNoData(zero_buf + 1, 0.0, 25, 1, 25, 1, 8,
                                      GSF_UNSIGNED_INT));
  // Non-zero in initial unaligned bytes
  zero_buf[2] = 1;
  EXPECT_FALSE(GDALBufferHasOnlyNoData(zero_buf + 1, 0.0, 25, 1, 25, 1, 8,
                                       GSF_UNSIGNED_INT));
  zero_buf[2] = 0;
  // Non-zero in middle 8-byte word
  zero_buf[12] = 1;
  EXPECT_FALSE(GDALBufferHasOnlyNoData(zero_buf + 1, 0.0, 25, 1, 25, 1, 8,
                                       GSF_UNSIGNED_INT));
  zero_buf[12] = 0;
  // Non-zero in trailing bytes
  zero_buf[25] = 1;
  EXPECT_FALSE(GDALBufferHasOnlyNoData(zero_buf + 1, 0.0, 25, 1, 25, 1, 8,
                                       GSF_UNSIGNED_INT));
  zero_buf[25] = 0;

  // 2. Test all bit depths and sample formats with non-zero nodata
  auto test_int_type = [](auto sample_val, int bits,
                          GDALBufferSampleFormat fmt) {
    using T = decltype(sample_val);
    std::vector<T> buf(4 * 4 * 2, sample_val);
    EXPECT_TRUE(GDALBufferHasOnlyNoData(
        buf.data(), static_cast<double>(sample_val), 4, 4, 4, 2, bits, fmt));
    // Fail fast 5-point check (corner)
    buf[0] = 0;
    EXPECT_FALSE(GDALBufferHasOnlyNoData(
        buf.data(), static_cast<double>(sample_val), 4, 4, 4, 2, bits, fmt));
    buf[0] = sample_val;
    // Pass 5-point check, fail interior pixel (row 0, col 1)
    buf[2] = 0;
    EXPECT_FALSE(GDALBufferHasOnlyNoData(
        buf.data(), static_cast<double>(sample_val), 4, 4, 4, 2, bits, fmt));
  };

  test_int_type(uint8_t{42}, 8, GSF_UNSIGNED_INT);
  test_int_type(int8_t{-42}, 8, GSF_SIGNED_INT);
  test_int_type(uint16_t{1000}, 16, GSF_UNSIGNED_INT);
  test_int_type(int16_t{-1000}, 16, GSF_SIGNED_INT);
  test_int_type(uint32_t{100000}, 32, GSF_UNSIGNED_INT);
  test_int_type(int32_t{-100000}, 32, GSF_SIGNED_INT);
  test_int_type(uint64_t{100000}, 64, GSF_UNSIGNED_INT);
  test_int_type(int64_t{-100000}, 64, GSF_SIGNED_INT);

  // Floating point (16, 32, 64) with finite value and NaN
  {
    std::vector<GFloat16> buf16(9, GFloat16(3.5f));
    EXPECT_TRUE(GDALBufferHasOnlyNoData(buf16.data(), 3.5, 3, 3, 3, 1, 16,
                                        GSF_FLOATING_POINT));
    std::vector<GFloat16> nan16(
        9, GFloat16(std::numeric_limits<float>::quiet_NaN()));
    EXPECT_TRUE(GDALBufferHasOnlyNoData(
        nan16.data(), std::numeric_limits<double>::quiet_NaN(), 3, 3, 3, 1, 16,
        GSF_FLOATING_POINT));
  }
  {
    std::vector<float> buf32(9, -12.5f);
    EXPECT_TRUE(GDALBufferHasOnlyNoData(buf32.data(), -12.5, 3, 3, 3, 1, 32,
                                        GSF_FLOATING_POINT));
    std::vector<float> nan32(9, std::numeric_limits<float>::quiet_NaN());
    EXPECT_TRUE(GDALBufferHasOnlyNoData(
        nan32.data(), std::numeric_limits<double>::quiet_NaN(), 3, 3, 3, 1, 32,
        GSF_FLOATING_POINT));
  }
  {
    std::vector<double> buf64(9, 123.456);
    EXPECT_TRUE(GDALBufferHasOnlyNoData(buf64.data(), 123.456, 3, 3, 3, 1, 64,
                                        GSF_FLOATING_POINT));
    std::vector<double> nan64(9, std::numeric_limits<double>::quiet_NaN());
    EXPECT_TRUE(GDALBufferHasOnlyNoData(
        nan64.data(), std::numeric_limits<double>::quiet_NaN(), 3, 3, 3, 1, 64,
        GSF_FLOATING_POINT));
  }

  // Unsupported bit depth
  EXPECT_FALSE(
      GDALBufferHasOnlyNoData(zero_buf, 5.0, 4, 4, 4, 1, 12, GSF_UNSIGNED_INT));
}

TEST(RasterIoTest, GdalDeinterleaveAndTranspose2D) {
  // GDALDeinterleave: 3-component and 4-component Byte, UInt16, and Float32
  constexpr size_t kIters = 35;
  {
    alignas(16) GByte src3[kIters * 3 + 4];
    for (size_t i = 0; i < sizeof(src3); ++i) src3[i] = static_cast<GByte>(i);
    alignas(16) GByte d0[kIters], d1[kIters], d2[kIters];
    void* dsts3[3] = {d0, d1, d2};
    // Aligned
    GDALDeinterleave(src3, GDT_Byte, 3, dsts3, GDT_Byte, kIters);
    for (size_t i = 0; i < kIters; ++i) {
      EXPECT_EQ(src3[3 * i + 0], d0[i]);
      EXPECT_EQ(src3[3 * i + 1], d1[i]);
      EXPECT_EQ(src3[3 * i + 2], d2[i]);
    }
    // Unaligned
    GDALDeinterleave(src3 + 1, GDT_Byte, 3, dsts3, GDT_Byte, kIters);
  }
  {
    alignas(16) GByte src4[kIters * 4];
    for (size_t i = 0; i < sizeof(src4); ++i) src4[i] = static_cast<GByte>(i);
    alignas(16) GByte d0[kIters], d1[kIters], d2[kIters], d3[kIters];
    void* dsts4[4] = {d0, d1, d2, d3};
    GDALDeinterleave(src4, GDT_Byte, 4, dsts4, GDT_Byte, kIters);
    for (size_t i = 0; i < kIters; ++i) {
      EXPECT_EQ(src4[4 * i + 0], d0[i]);
      EXPECT_EQ(src4[4 * i + 3], d3[i]);
    }
  }
  {
    std::vector<uint16_t> src_u16(kIters * 4, 7);
    std::vector<uint16_t> d0(kIters), d1(kIters), d2(kIters), d3(kIters);
    void* dsts[4] = {d0.data(), d1.data(), d2.data(), d3.data()};
    GDALDeinterleave(src_u16.data(), GDT_UInt16, 3, dsts, GDT_UInt16, kIters);
    GDALDeinterleave(src_u16.data(), GDT_UInt16, 4, dsts, GDT_UInt16, kIters);

    // Different source and dest types (fallback path)
    std::vector<float> f0(kIters), f1(kIters);
    void* fdsts[2] = {f0.data(), f1.data()};
    GDALDeinterleave(src_u16.data(), GDT_UInt16, 2, fdsts, GDT_Float32, kIters);
    EXPECT_FLOAT_EQ(7.0f, f0[0]);
  }

  // GDALTranspose2D special Byte paths: height=2, height=4, height=10
  {
    constexpr size_t kW = 36;
    std::vector<uint8_t> src_h2(kW * 2);
    for (size_t i = 0; i < src_h2.size(); ++i)
      src_h2[i] = static_cast<uint8_t>(i);
    std::vector<uint8_t> dst_h2(kW * 2, 0);
    GDALTranspose2D(src_h2.data(), GDT_Byte, dst_h2.data(), GDT_Byte, kW, 2);
    EXPECT_EQ(src_h2[1 * kW + 3], dst_h2[3 * 2 + 1]);

    std::vector<uint8_t> src_h4(kW * 4);
    for (size_t i = 0; i < src_h4.size(); ++i)
      src_h4[i] = static_cast<uint8_t>(i);
    std::vector<uint8_t> dst_h4(kW * 4, 0);
    GDALTranspose2D(src_h4.data(), GDT_Byte, dst_h4.data(), GDT_Byte, kW, 4);
    EXPECT_EQ(src_h4[2 * kW + 5], dst_h4[5 * 4 + 2]);

    std::vector<uint8_t> src_h10(10 * 10, 9);
    std::vector<uint8_t> dst_h10(10 * 10, 0);
    GDALTranspose2D(src_h10.data(), GDT_Byte, dst_h10.data(), GDT_Byte, 10, 10);
    EXPECT_EQ(9, dst_h10[55]);
  }

  // GDALTranspose2D across all 16x16 data type combinations
  const GDALDataType kAllTypes[] = {
      GDT_Byte,    GDT_Int8,     GDT_UInt16,   GDT_Int16,
      GDT_UInt32,  GDT_Int32,    GDT_UInt64,   GDT_Int64,
      GDT_Float16, GDT_Float32,  GDT_Float64,  GDT_CInt16,
      GDT_CInt32,  GDT_CFloat16, GDT_CFloat32, GDT_CFloat64,
  };
  alignas(16) GByte t_src[3 * 3 * 16];
  alignas(16) GByte t_dst[3 * 3 * 16];
  memset(t_src, 1, sizeof(t_src));
  for (GDALDataType s_dt : kAllTypes) {
    for (GDALDataType d_dt : kAllTypes) {
      GDALTranspose2D(t_src, s_dt, t_dst, d_dt, 3, 3);
    }
    GDALTranspose2D(t_src, GDT_Unknown, t_dst, s_dt, 3, 3);
  }
  GDALTranspose2D(t_src, GDT_Byte, t_dst, GDT_Unknown, 3, 3);
}

TEST(RasterIoTest, BestOverviewLevelAndOverviewRasterIO) {
  // Overview 0: larger than base (invalid, skipped)
  TestDataset ovr_ds0(120, 120);
  auto* ovr0 = new TestRasterBand(&ovr_ds0, 1, GDT_Byte, 120, 120, 120, 120);
  ovr_ds0.AddTestBand(ovr0);

  // Overview 1: 50x50 with RESAMPLING=AVERAGE_BIT2GRAYSCALE (skipped)
  TestDataset ovr_ds1(50, 50);
  auto* ovr1 = new TestRasterBand(&ovr_ds1, 1, GDT_Byte, 50, 50, 50, 50);
  ovr1->SetMetadataItem("RESAMPLING", "AVERAGE_BIT2GRAYSCALE");
  ovr_ds1.AddTestBand(ovr1);

  // Overview 2: 50x50 valid
  TestDataset ovr_ds2(50, 50);
  auto* ovr2 = new TestRasterBand(&ovr_ds2, 1, GDT_Byte, 50, 50, 50, 50);
  std::fill(ovr2->raw_data().begin(), ovr2->raw_data().end(), 77);
  ovr_ds2.AddTestBand(ovr2);

  // Overview 3: 25x25 valid
  TestDataset ovr_ds3(25, 25);
  auto* ovr3 = new TestRasterBand(&ovr_ds3, 1, GDT_Byte, 25, 25, 25, 25);
  std::fill(ovr3->raw_data().begin(), ovr3->raw_data().end(), 88);
  ovr_ds3.AddTestBand(ovr3);

  TestDataset ds(100, 100);
  auto* base_band = new TestRasterBand(&ds, 1, GDT_Byte, 100, 100, 100, 100);
  ds.AddTestBand(base_band);

  base_band->AddOverview(ovr0);
  base_band->AddOverview(ovr1);
  base_band->AddOverview(ovr2);
  base_band->AddOverview(ovr3);

  // 1. bUseOnlyThisScale = true -> returns -1
  {
    int xoff = 0, yoff = 0, xsize = 100, ysize = 100;
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.nVersion = 2;
    extra.bUseOnlyThisScale = true;
    EXPECT_EQ(-1, GDALBandGetBestOverviewLevel2(base_band, xoff, yoff, xsize,
                                                ysize, 25, 25, &extra));
  }

  // 2. Request 50x50 -> selects overview 2 (exact match)
  {
    int xoff = 10, yoff = 20, xsize = 90, ysize = 80;
    CPLPushErrorHandler(CPLQuietErrorHandler);
    int lvl = GDALBandGetBestOverviewLevel(base_band, xoff, yoff, xsize, ysize,
                                           45, 40);
    CPLPopErrorHandler();
    EXPECT_EQ(2, lvl);
    EXPECT_EQ(5, xoff);
    EXPECT_EQ(10, yoff);
    EXPECT_EQ(45, xsize);
    EXPECT_EQ(40, ysize);
  }

  // 3. Request with bFloatingPointWindowValidity = true and non-nearest
  {
    int xoff = 0, yoff = 0, xsize = 100, ysize = 100;
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.bFloatingPointWindowValidity = TRUE;
    extra.dfXOff = 4.0;
    extra.dfYOff = 8.0;
    extra.dfXSize = 80.0;
    extra.dfYSize = 80.0;
    EXPECT_EQ(3, GDALBandGetBestOverviewLevel2(base_band, xoff, yoff, xsize,
                                               ysize, 20, 20, &extra));
    EXPECT_DOUBLE_EQ(1.0, extra.dfXOff);
    EXPECT_DOUBLE_EQ(2.0, extra.dfYOff);

    // Without bFloatingPointWindowValidity, but eResampleAlg = GRIORA_Bilinear
    xoff = 4;
    yoff = 8;
    xsize = 80;
    ysize = 80;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Bilinear;
    CPLSetConfigOption("GDAL_OVERVIEW_OVERSAMPLING_THRESHOLD", "1.5");
    EXPECT_EQ(3, GDALBandGetBestOverviewLevel2(base_band, xoff, yoff, xsize,
                                               ysize, 20, 20, &extra));
    CPLSetConfigOption("GDAL_OVERVIEW_OVERSAMPLING_THRESHOLD", nullptr);
    EXPECT_TRUE(extra.bFloatingPointWindowValidity);
  }

  // 4. OverviewRasterIO and TryOverviewRasterIO on band and dataset
  {
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    std::vector<GByte> buf(25 * 25, 0);
    EXPECT_EQ(CE_None, base_band->CallOverviewRasterIO(
                           GF_Read, 0, 0, 100, 100, buf.data(), 25, 25,
                           GDT_Byte, 1, 25, &extra));
    EXPECT_EQ(88, buf[0]);

    // When no overview applies (100x100)
    EXPECT_EQ(CE_Failure, base_band->CallOverviewRasterIO(
                              GF_Read, 0, 0, 100, 100, buf.data(), 100, 100,
                              GDT_Byte, 1, 100, &extra));

    int bTried = FALSE;
    EXPECT_EQ(CE_None, base_band->CallTryOverviewRasterIO(
                           GF_Read, 0, 0, 100, 100, buf.data(), 25, 25,
                           GDT_Byte, 1, 25, &extra, &bTried));
    EXPECT_TRUE(bTried);

    EXPECT_EQ(CE_None, base_band->CallTryOverviewRasterIO(
                           GF_Read, 0, 0, 100, 100, buf.data(), 100, 100,
                           GDT_Byte, 1, 100, &extra, &bTried));
    EXPECT_FALSE(bTried);

    const int band_map[1] = {1};
    EXPECT_EQ(CE_None, ds.CallTryOverviewRasterIO(
                           GF_Read, 0, 0, 100, 100, buf.data(), 25, 25,
                           GDT_Byte, 1, band_map, 1, 25, 0, &extra, &bTried));
    EXPECT_TRUE(bTried);

    EXPECT_EQ(CE_None, ds.CallTryOverviewRasterIO(
                           GF_Read, 0, 0, 100, 100, buf.data(), 100, 100,
                           GDT_Byte, 1, band_map, 1, 100, 0, &extra, &bTried));
    EXPECT_FALSE(bTried);
  }
}

TEST(RasterIoTest, IRasterIOCases1And2AndErrors) {
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // Error case: eFlushBlockErr != CE_None on write
  {
    TestDataset ds(10, 10);
    auto* band = new TestRasterBand(&ds, 1, GDT_Byte, 10, 10, 10, 10);
    ds.AddTestBand(band);
    GByte val[100] = {5};
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    EXPECT_EQ(CE_None, band->CallIRasterIO(GF_Write, 0, 0, 10, 10, val, 10, 10,
                                           GDT_Byte, 1, 10, &extra));
    band->SetFailWriteBlock(true);
    EXPECT_TRUE(GDALRasterBlock::FlushCacheBlock(TRUE));
    band->SetFailWriteBlock(false);
    // Next write sees eFlushBlockErr != CE_None
    EXPECT_EQ(CE_Failure, band->CallIRasterIO(GF_Write, 0, 0, 10, 10, val, 1, 1,
                                              GDT_Byte, 1, 1, &extra));
  }

  // Error case: invalid block size
  {
    TestDataset ds(10, 10);
    auto* band = new TestRasterBand(&ds, 1, GDT_Byte, 10, 10, 10, 10);
    ds.AddTestBand(band);
    band->SetInvalidBlockSize();
    GByte val = 0;
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    EXPECT_EQ(CE_Failure, band->CallIRasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                              GDT_Byte, 1, 1, &extra));
  }

  // Case 1: nBlockXSize == GetXSize(), packed buffer, 1:1
  // Use height 10 with block height 4 so bottom tile (y=8..9) is a partial tile
  // (exercises bMemZeroBuffer = true when writing!).
  {
    TestDataset ds(10, 10);
    auto* band = new TestRasterBand(&ds, 1, GDT_Byte, 10, 10, 10, 4);
    ds.AddTestBand(band);
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);

    std::vector<GByte> write_buf(10 * 10, 42);
    EXPECT_EQ(CE_None,
              band->CallIRasterIO(GF_Write, 0, 0, 10, 10, write_buf.data(), 10,
                                  10, GDT_Byte, 1, 10, &extra));

    std::vector<GByte> read_buf(10 * 10, 0);
    EXPECT_EQ(CE_None,
              band->CallIRasterIO(GF_Read, 0, 0, 10, 10, read_buf.data(), 10,
                                  10, GDT_Byte, 1, 10, &extra));
    EXPECT_EQ(42, read_buf[99]);

    // Type conversion in Case 1 (read and write as UInt16)
    std::vector<uint16_t> u16_buf(10 * 10, 123);
    EXPECT_EQ(CE_None,
              band->CallIRasterIO(GF_Write, 0, 0, 10, 10, u16_buf.data(), 10,
                                  10, GDT_UInt16, 2, 20, &extra));
    std::fill(u16_buf.begin(), u16_buf.end(), 0);
    EXPECT_EQ(CE_None,
              band->CallIRasterIO(GF_Read, 0, 0, 10, 10, u16_buf.data(), 10, 10,
                                  GDT_UInt16, 2, 20, &extra));
    EXPECT_EQ(123, u16_buf[0]);

    // Progress failure in Case 1
    double stop_at = 0.0;
    extra.pfnProgress = FailingProgressCallback;
    extra.pProgressData = &stop_at;
    EXPECT_EQ(CE_Failure,
              band->CallIRasterIO(GF_Read, 0, 0, 10, 10, read_buf.data(), 10,
                                  10, GDT_Byte, 1, 10, &extra));
    INIT_RASTERIO_EXTRA_ARG(extra);

    // Block lock failure in Case 1
    band->SetFailGetLockedBlock(true, true);
    EXPECT_EQ(CE_Failure,
              band->CallIRasterIO(GF_Read, 0, 0, 10, 10, read_buf.data(), 10,
                                  10, GDT_Byte, 1, 10, &extra));
    band->SetFailGetLockedBlock(false);
  }

  // GDAL_NO_COSTLY_OVERVIEW config option
  {
    TestDataset ds(300, 300);
    auto* band = new TestRasterBand(&ds, 1, GDT_Byte, 300, 300, 64, 64);
    ds.AddTestBand(band);
    std::fill(band->raw_data().begin(), band->raw_data().end(), 99);
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    GByte out[4] = {1, 1, 1, 1};
    CPLSetConfigOption("GDAL_NO_COSTLY_OVERVIEW", "YES");
    EXPECT_EQ(CE_None, band->CallIRasterIO(GF_Read, 0, 0, 300, 300, out, 2, 2,
                                           GDT_Byte, 1, 2, &extra));
    CPLSetConfigOption("GDAL_NO_COSTLY_OVERVIEW", nullptr);
    EXPECT_EQ(0, out[0]);
  }

  // Case 2: Tiled band (nBlockXSize < GetXSize()), 1:1 read/write
  // Use 10x10 with 4x4 blocks so right/bottom blocks are partial tiles.
  {
    TestDataset ds(10, 10);
    auto* band = new TestRasterBand(&ds, 1, GDT_Byte, 10, 10, 4, 4);
    ds.AddTestBand(band);
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);

    std::vector<GByte> buf(10 * 10, 55);
    EXPECT_EQ(CE_None, band->CallIRasterIO(GF_Write, 0, 0, 10, 10, buf.data(),
                                           10, 10, GDT_Byte, 1, 10, &extra));
    std::fill(buf.begin(), buf.end(), 0);
    EXPECT_EQ(CE_None, band->CallIRasterIO(GF_Read, 0, 0, 10, 10, buf.data(),
                                           10, 10, GDT_Byte, 1, 10, &extra));
    EXPECT_EQ(55, buf[99]);

    // Type conversion in Case 2
    std::vector<uint16_t> u16_buf(10 * 10, 77);
    EXPECT_EQ(CE_None, band->CallIRasterIO(GF_Write, 1, 1, 8, 8, u16_buf.data(),
                                           8, 8, GDT_UInt16, 2, 16, &extra));
    std::fill(u16_buf.begin(), u16_buf.end(), 0);
    EXPECT_EQ(CE_None, band->CallIRasterIO(GF_Read, 1, 1, 8, 8, u16_buf.data(),
                                           8, 8, GDT_UInt16, 2, 16, &extra));
    EXPECT_EQ(77, u16_buf[0]);

    // Progress failure in Case 2
    double stop_at = 0.0;
    extra.pfnProgress = FailingProgressCallback;
    extra.pProgressData = &stop_at;
    EXPECT_EQ(CE_Failure, band->CallIRasterIO(GF_Read, 0, 0, 10, 10, buf.data(),
                                              10, 10, GDT_Byte, 1, 10, &extra));
    INIT_RASTERIO_EXTRA_ARG(extra);

    // GetLockedBlockRef failure in Case 2
    band->SetFailGetLockedBlock(true, true);
    EXPECT_EQ(CE_Failure, band->CallIRasterIO(GF_Read, 0, 0, 10, 10, buf.data(),
                                              10, 10, GDT_Byte, 1, 10, &extra));
    band->SetFailGetLockedBlock(false);
  }

  CPLPopErrorHandler();
}

TEST(RasterIoTest, IRasterIOCase3AndDownsamplingIntegerXFactor) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);

  // Test DownsamplingIntegerXFactor for data type sizes 1, 2, 4, 8, 16 and
  // cross-type conversion across multiple blocks in X (e.g. width 20, block 4,
  // buf width 10 -> nSrcXInc = 2).
  for (GDALDataType dt :
       {GDT_Byte, GDT_UInt16, GDT_Float32, GDT_Float64, GDT_CFloat64}) {
    TestDataset ds(20, 8);
    auto* band = new TestRasterBand(&ds, 1, dt, 20, 8, 4, 4);
    ds.AddTestBand(band);
    const int dt_size = GDALGetDataTypeSizeBytes(dt);
    std::vector<GByte> dst(10 * 4 * dt_size, 0);

    // Same type downsampling
    EXPECT_EQ(CE_None,
              band->CallIRasterIO(GF_Read, 0, 0, 20, 8, dst.data(), 10, 4, dt,
                                  dt_size, 10 * dt_size, &extra));

    // Cross-type downsampling
    GDALDataType other_dt = (dt == GDT_Byte) ? GDT_UInt16 : GDT_Byte;
    const int other_size = GDALGetDataTypeSizeBytes(other_dt);
    std::vector<GByte> dst_other(10 * 4 * other_size, 0);
    EXPECT_EQ(CE_None, band->CallIRasterIO(
                           GF_Read, 0, 0, 20, 8, dst_other.data(), 10, 4,
                           other_dt, other_size, 10 * other_size, &extra));

    // Non-integer X factor (upsampling / non-multiple downsampling: 20 -> 7)
    std::vector<GByte> dst_nonint(7 * 3 * dt_size, 0);
    EXPECT_EQ(CE_None,
              band->CallIRasterIO(GF_Read, 0, 0, 20, 8, dst_nonint.data(), 7, 3,
                                  dt, dt_size, 7 * dt_size, &extra));
    std::vector<GByte> dst_nonint_other(7 * 3 * other_size, 0);
    EXPECT_EQ(CE_None, band->CallIRasterIO(
                           GF_Read, 0, 0, 20, 8, dst_nonint_other.data(), 7, 3,
                           other_dt, other_size, 7 * other_size, &extra));
  }

  // Case 3 Write (upsampling/downsampling write, same type and cross-type)
  {
    TestDataset ds(12, 12);
    auto* band = new TestRasterBand(&ds, 1, GDT_Byte, 12, 12, 4, 4);
    ds.AddTestBand(band);
    std::vector<GByte> src_byte(4 * 4, 33);
    EXPECT_EQ(CE_None,
              band->CallIRasterIO(GF_Write, 0, 0, 12, 12, src_byte.data(), 4, 4,
                                  GDT_Byte, 1, 4, &extra));
    std::vector<uint16_t> src_u16(4 * 4, 44);
    EXPECT_EQ(CE_None,
              band->CallIRasterIO(GF_Write, 0, 0, 12, 12, src_u16.data(), 4, 4,
                                  GDT_UInt16, 2, 8, &extra));

    // Progress failure in Case 3 Write
    double stop_at = 0.0;
    extra.pfnProgress = FailingProgressCallback;
    extra.pProgressData = &stop_at;
    EXPECT_EQ(CE_Failure,
              band->CallIRasterIO(GF_Write, 0, 0, 12, 12, src_byte.data(), 4, 4,
                                  GDT_Byte, 1, 4, &extra));
    INIT_RASTERIO_EXTRA_ARG(extra);

    // Block lock failure in Case 3 Write and Read
    band->SetFailGetLockedBlock(true);
    EXPECT_EQ(CE_Failure,
              band->CallIRasterIO(GF_Write, 0, 0, 12, 12, src_byte.data(), 4, 4,
                                  GDT_Byte, 1, 4, &extra));
    EXPECT_EQ(CE_Failure,
              band->CallIRasterIO(GF_Read, 0, 0, 12, 12, src_byte.data(), 4, 4,
                                  GDT_Byte, 1, 4, &extra));
    EXPECT_EQ(CE_Failure,
              band->CallIRasterIO(GF_Read, 0, 0, 12, 12, src_byte.data(), 5, 5,
                                  GDT_Byte, 1, 5, &extra));
    band->SetFailGetLockedBlock(false);
  }

  // Paletted band with Bilinear and Complex band with Gauss fallback warnings
  {
    TestDataset ds(8, 8);
    auto* pal_band = new TestRasterBand(&ds, 1, GDT_Byte, 8, 8, 8, 8);
    GDALColorTable ct;
    pal_band->SetTestColorTable(&ct);
    ds.AddTestBand(pal_band);

    extra.eResampleAlg = GRIORA_Bilinear;
    GByte out[4 * 4];
    EXPECT_EQ(CE_None, pal_band->CallIRasterIO(GF_Read, 0, 0, 8, 8, out, 4, 4,
                                               GDT_Byte, 1, 4, &extra));

    TestDataset cds(8, 8);
    auto* cpx_band = new TestRasterBand(&cds, 1, GDT_CFloat32, 8, 8, 8, 8);
    cds.AddTestBand(cpx_band);
    extra.eResampleAlg = GRIORA_Gauss;
    float cout[4 * 4 * 2];
    EXPECT_EQ(CE_None, cpx_band->CallIRasterIO(GF_Read, 0, 0, 8, 8, cout, 4, 4,
                                               GDT_CFloat32, 8, 32, &extra));
  }

  CPLPopErrorHandler();
}

TEST(RasterIoTest, BandAndDatasetRasterIOResampled) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  const GDALRIOResampleAlg kAlgs[] = {
      GRIORA_Bilinear, GRIORA_Cubic, GRIORA_CubicSpline, GRIORA_Lanczos,
      GRIORA_Average,  GRIORA_RMS,   GRIORA_Mode,        GRIORA_Gauss,
  };

  // 1. Band RasterIOResampled on real data with NBITS, nodata mask (all valid,
  //    all nodata, partial nodata), type conversion, and floating-point window
  {
    TestDataset ds(16, 16);
    auto* band = new TestRasterBand(&ds, 1, GDT_Byte, 16, 16, 8, 8);
    band->SetMetadataItem("NBITS", "7", "IMAGE_STRUCTURE");
    band->SetTestNoData(0.0);
    ds.AddTestBand(band);

    for (GDALRIOResampleAlg alg : kAlgs) {
      GDALRasterIOExtraArg extra;
      INIT_RASTERIO_EXTRA_ARG(extra);
      extra.eResampleAlg = alg;
      extra.bFloatingPointWindowValidity = TRUE;
      extra.dfXOff = 2.0;
      extra.dfYOff = 2.0;
      extra.dfXSize = 12.0;
      extra.dfYSize = 12.0;

      // All pixels == 0 (nodata) -> mask is all 0 (bSkipResample = true)
      std::fill(band->raw_data().begin(), band->raw_data().end(), 0);
      std::vector<float> out_f(6 * 6, 0.0f);
      EXPECT_EQ(CE_None,
                band->CallIRasterIO(GF_Read, 2, 2, 12, 12, out_f.data(), 6, 6,
                                    GDT_Float32, 4, 24, &extra));

      // All pixels == 50 -> mask is all 255 (bNoDataMaskFullyOpaque = true)
      std::fill(band->raw_data().begin(), band->raw_data().end(), 50);
      EXPECT_EQ(CE_None,
                band->CallIRasterIO(GF_Read, 2, 2, 12, 12, out_f.data(), 6, 6,
                                    GDT_Float32, 4, 24, &extra));

      // Mixed nodata and valid pixels
      band->raw_data()[5 * 16 + 5] = 0;
      std::vector<GByte> out_b(6 * 6, 0);
      EXPECT_EQ(CE_None,
                band->CallIRasterIO(GF_Read, 2, 2, 12, 12, out_b.data(), 6, 6,
                                    GDT_Byte, 1, 6, &extra));
    }
  }

  // 2. Band RasterIOResampled on Complex data (triggers bUseWarp = true),
  //    both with dataset and standalone band (GetDataset() == nullptr) with
  //    nodata mask!
  {
    TestRasterBand standalone_cpx(nullptr, 1, GDT_CFloat32, 12, 12, 12, 12);
    standalone_cpx.SetTestNoData(0.0);
    for (GDALRIOResampleAlg alg :
         {GRIORA_Bilinear, GRIORA_Cubic, GRIORA_CubicSpline, GRIORA_Lanczos,
          GRIORA_Average, GRIORA_RMS}) {
      GDALRasterIOExtraArg extra;
      INIT_RASTERIO_EXTRA_ARG(extra);
      extra.eResampleAlg = alg;
      std::vector<float> out(6 * 6 * 2, 0.0f);
      EXPECT_EQ(CE_None, standalone_cpx.CallRasterIOResampled(
                             GF_Read, 0, 0, 12, 12, out.data(), 6, 6,
                             GDT_CFloat32, 8, 48, &extra));
    }
  }

  // 3. Dataset::RasterIOResampled with multiple bands, NBITS, and nodata masks
  {
    TestDataset ds(16, 16);
    auto* b1 = new TestRasterBand(&ds, 1, GDT_Byte, 16, 16, 8, 8);
    auto* b2 = new TestRasterBand(&ds, 2, GDT_Byte, 16, 16, 8, 8);
    b1->SetMetadataItem("NBITS", "7", "IMAGE_STRUCTURE");
    b1->SetTestNoData(0.0);
    b2->SetTestNoData(0.0);
    ds.AddTestBand(b1);
    ds.AddTestBand(b2);

    const int band_map[2] = {1, 2};
    for (GDALRIOResampleAlg alg : kAlgs) {
      GDALRasterIOExtraArg extra;
      INIT_RASTERIO_EXTRA_ARG(extra);
      extra.eResampleAlg = alg;
      extra.bFloatingPointWindowValidity = TRUE;
      extra.dfXOff = 2.0;
      extra.dfYOff = 2.0;
      extra.dfXSize = 12.0;
      extra.dfYSize = 12.0;

      std::vector<GByte> out(6 * 6 * 2, 0);
      // All nodata (mask == 0)
      std::fill(b1->raw_data().begin(), b1->raw_data().end(), 0);
      std::fill(b2->raw_data().begin(), b2->raw_data().end(), 0);
      EXPECT_EQ(CE_None, ds.CallRasterIOResampled(GF_Read, 2, 2, 12, 12,
                                                  out.data(), 6, 6, GDT_Byte, 2,
                                                  band_map, 1, 6, 36, &extra));

      // All opaque (mask == 255)
      std::fill(b1->raw_data().begin(), b1->raw_data().end(), 40);
      std::fill(b2->raw_data().begin(), b2->raw_data().end(), 80);
      EXPECT_EQ(CE_None, ds.CallRasterIOResampled(GF_Read, 2, 2, 12, 12,
                                                  out.data(), 6, 6, GDT_Byte, 2,
                                                  band_map, 1, 6, 36, &extra));

      // Partial nodata
      b1->raw_data()[4 * 16 + 4] = 0;
      EXPECT_EQ(CE_None, ds.CallRasterIOResampled(GF_Read, 2, 2, 12, 12,
                                                  out.data(), 6, 6, GDT_Byte, 2,
                                                  band_map, 1, 6, 36, &extra));
    }
  }

  // 4. Large virtual raster (> 1024x1024) to exercise chunk splitting loop in
  //    Band::RasterIOResampled and Dataset::RasterIOResampled
  {
    TestDataset big_ds(1100, 1100);
    auto* big_b1 =
        new TestRasterBand(&big_ds, 1, GDT_Byte, 1100, 1100, 1100, 10, true);
    big_ds.AddTestBand(big_b1);
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Bilinear;
    double stop_at = 0.0;
    extra.pfnProgress = FailingProgressCallback;
    extra.pProgressData = &stop_at;
    std::vector<GByte> out(4 * 4, 0);
    EXPECT_EQ(CE_Failure, big_b1->CallRasterIOResampled(
                              GF_Read, 0, 0, 1100, 1100, out.data(), 4, 4,
                              GDT_Byte, 1, 4, &extra));
    const int band_map[1] = {1};
    EXPECT_EQ(CE_Failure, big_ds.CallRasterIOResampled(
                              GF_Read, 0, 0, 1100, 1100, out.data(), 4, 4,
                              GDT_Byte, 1, band_map, 1, 4, 16, &extra));
  }

  CPLPopErrorHandler();
}

TEST(RasterIoTest, BlockBasedRasterIO) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);

  // 1. Mismatched block sizes and mismatched band data types fallback
  {
    TestDataset ds(8, 8);
    ds.AddTestBand(new TestRasterBand(&ds, 1, GDT_Byte, 8, 8, 4, 4));
    ds.AddTestBand(new TestRasterBand(&ds, 2, GDT_Byte, 8, 8, 8, 8));
    const int band_map[2] = {1, 2};
    std::vector<GByte> buf(8 * 8 * 2, 0);
    EXPECT_EQ(CE_None, ds.CallBlockBasedRasterIO(GF_Read, 0, 0, 8, 8,
                                                 buf.data(), 8, 8, GDT_Byte, 2,
                                                 band_map, 1, 8, 64, &extra));

    TestDataset ds_dt(8, 8);
    ds_dt.AddTestBand(new TestRasterBand(&ds_dt, 1, GDT_Byte, 8, 8, 4, 4));
    ds_dt.AddTestBand(new TestRasterBand(&ds_dt, 2, GDT_UInt16, 8, 8, 4, 4));
    EXPECT_EQ(CE_None, ds_dt.CallBlockBasedRasterIO(
                           GF_Read, 0, 0, 8, 8, buf.data(), 4, 4, GDT_Byte, 2,
                           band_map, 1, 4, 16, &extra));
  }

  // 2. Full resolution (1:1), subsampled read, upsampled write, downsampled
  //    write, non-nearest resampling, and overviews
  {
    TestDataset ovr_ds(8, 8);
    auto* ov1 = new TestRasterBand(&ovr_ds, 1, GDT_Byte, 8, 8, 4, 4);
    auto* ov2 = new TestRasterBand(&ovr_ds, 2, GDT_Byte, 8, 8, 4, 4);
    std::fill(ov1->raw_data().begin(), ov1->raw_data().end(), 11);
    std::fill(ov2->raw_data().begin(), ov2->raw_data().end(), 22);
    ovr_ds.AddTestBand(ov1);
    ovr_ds.AddTestBand(ov2);

    TestDataset ds(16, 16);
    auto* b1 = new TestRasterBand(&ds, 1, GDT_Byte, 16, 16, 8, 8);
    auto* b2 = new TestRasterBand(&ds, 2, GDT_Byte, 16, 16, 8, 8);
    ds.AddTestBand(b1);
    ds.AddTestBand(b2);
    b1->AddOverview(ov1);
    b2->AddOverview(ov2);

    const int band_map[2] = {1, 2};

    // 1:1 Write and Read (pixel-interleaved)
    std::vector<GByte> buf16(16 * 16 * 2, 9);
    EXPECT_EQ(CE_None, ds.CallBlockBasedRasterIO(
                           GF_Write, 0, 0, 16, 16, buf16.data(), 16, 16,
                           GDT_Byte, 2, band_map, 2, 32, 1, &extra));
    EXPECT_EQ(CE_None, ds.CallBlockBasedRasterIO(
                           GF_Read, 0, 0, 16, 16, buf16.data(), 16, 16,
                           GDT_Byte, 2, band_map, 2, 32, 1, &extra));

    // Downsampled write (falls back to BandBasedRasterIO)
    std::vector<GByte> buf8(8 * 8 * 2, 7);
    EXPECT_EQ(CE_None, ds.CallBlockBasedRasterIO(GF_Write, 0, 0, 16, 16,
                                                 buf8.data(), 8, 8, GDT_Byte, 2,
                                                 band_map, 1, 8, 64, &extra));

    // Upsampled write (hits general loop in BlockBasedRasterIO, both same type
    // and cross-type)
    EXPECT_EQ(CE_None, ds.CallBlockBasedRasterIO(GF_Write, 0, 0, 4, 4,
                                                 buf8.data(), 8, 8, GDT_Byte, 2,
                                                 band_map, 1, 8, 64, &extra));
    std::vector<uint16_t> buf8_u16(8 * 8 * 2, 13);
    EXPECT_EQ(CE_None, ds.CallBlockBasedRasterIO(
                           GF_Write, 0, 0, 4, 4, buf8_u16.data(), 8, 8,
                           GDT_UInt16, 2, band_map, 2, 16, 128, &extra));

    // Subsampled read using overviews (same type and cross-type)
    EXPECT_EQ(CE_None, ds.CallBlockBasedRasterIO(GF_Read, 0, 0, 16, 16,
                                                 buf8.data(), 8, 8, GDT_Byte, 2,
                                                 band_map, 1, 8, 64, &extra));
    EXPECT_EQ(11, buf8[0]);
    EXPECT_EQ(22, buf8[64]);

    EXPECT_EQ(CE_None, ds.CallBlockBasedRasterIO(
                           GF_Read, 0, 0, 16, 16, buf8_u16.data(), 8, 8,
                           GDT_UInt16, 2, band_map, 2, 16, 128, &extra));
    EXPECT_EQ(11, buf8_u16[0]);

    // Non-nearest resampling fallback
    extra.eResampleAlg = GRIORA_Bilinear;
    EXPECT_EQ(CE_None, ds.CallBlockBasedRasterIO(GF_Read, 0, 0, 16, 16,
                                                 buf8.data(), 8, 8, GDT_Byte, 2,
                                                 band_map, 1, 8, 64, &extra));
    INIT_RASTERIO_EXTRA_ARG(extra);

    // Block lock failure in general loop
    ov1->SetFailGetLockedBlock(true);
    EXPECT_EQ(CE_Failure, ds.CallBlockBasedRasterIO(
                              GF_Read, 0, 0, 16, 16, buf8.data(), 8, 8,
                              GDT_Byte, 2, band_map, 1, 8, 64, &extra));
    ov1->SetFailGetLockedBlock(false);
  }

  // 3. GDALDatasetGetBestOverviewLevel mismatched overview count / size / block
  {
    TestDataset ovr_ds(8, 8);
    auto* ov1 = new TestRasterBand(&ovr_ds, 1, GDT_Byte, 8, 8, 4, 4);
    auto* ov2_diff_block = new TestRasterBand(&ovr_ds, 2, GDT_Byte, 8, 8, 8, 8);
    ovr_ds.AddTestBand(ov1);
    ovr_ds.AddTestBand(ov2_diff_block);

    TestDataset ovr_ds_diff_size(6, 6);
    auto* ov2_diff_size =
        new TestRasterBand(&ovr_ds_diff_size, 1, GDT_Byte, 6, 6, 4, 4);
    ovr_ds_diff_size.AddTestBand(ov2_diff_size);

    TestDataset ds(16, 16);
    auto* b1 = new TestRasterBand(&ds, 1, GDT_Byte, 16, 16, 8, 8);
    auto* b2 = new TestRasterBand(&ds, 2, GDT_Byte, 16, 16, 8, 8);
    ds.AddTestBand(b1);
    ds.AddTestBand(b2);

    const int band_map[2] = {1, 2};
    std::vector<GByte> buf(8 * 8 * 2, 0);

    // Mismatched overview count (b1 has 1, b2 has 0)
    b1->AddOverview(ov1);
    EXPECT_EQ(CE_None, ds.CallBlockBasedRasterIO(GF_Read, 0, 0, 16, 16,
                                                 buf.data(), 8, 8, GDT_Byte, 2,
                                                 band_map, 1, 8, 64, &extra));

    // Mismatched overview block size
    b2->AddOverview(ov2_diff_block);
    EXPECT_EQ(CE_None, ds.CallBlockBasedRasterIO(GF_Read, 0, 0, 16, 16,
                                                 buf.data(), 8, 8, GDT_Byte, 2,
                                                 band_map, 1, 8, 64, &extra));

    // Mismatched overview dimensions
    TestDataset ds2(16, 16);
    auto* b2_1 = new TestRasterBand(&ds2, 1, GDT_Byte, 16, 16, 8, 8);
    auto* b2_2 = new TestRasterBand(&ds2, 2, GDT_Byte, 16, 16, 8, 8);
    b2_1->AddOverview(ov1);
    b2_2->AddOverview(ov2_diff_size);
    ds2.AddTestBand(b2_1);
    ds2.AddTestBand(b2_2);
    EXPECT_EQ(CE_None, ds2.CallBlockBasedRasterIO(GF_Read, 0, 0, 16, 16,
                                                  buf.data(), 8, 8, GDT_Byte, 2,
                                                  band_map, 1, 8, 64, &extra));
  }

  CPLPopErrorHandler();
}

TEST(RasterIoTest, CopyWholeRasterDatasetAndBand) {
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // Nullptr checks
  EXPECT_EQ(CE_Failure, GDALDatasetCopyWholeRaster(nullptr, nullptr, nullptr,
                                                   nullptr, nullptr));
  EXPECT_EQ(CE_Failure, GDALRasterBandCopyWholeRaster(nullptr, nullptr, nullptr,
                                                      nullptr, nullptr));

  // Mismatched dimensions
  {
    TestDataset d1(10, 10), d2(12, 10);
    d1.AddTestBand(new TestRasterBand(&d1, 1, GDT_Byte, 10, 10, 10, 10));
    d2.AddTestBand(new TestRasterBand(&d2, 1, GDT_Byte, 12, 10, 12, 10));
    EXPECT_EQ(CE_Failure,
              GDALDatasetCopyWholeRaster(&d1, &d2, nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, GDALRasterBandCopyWholeRaster(
                              d1.GetRasterBand(1), d2.GetRasterBand(1), nullptr,
                              nullptr, nullptr));
  }

  // Zero bands dataset
  {
    TestDataset d1(10, 10), d2(10, 10);
    EXPECT_EQ(CE_None,
              GDALDatasetCopyWholeRaster(&d1, &d2, nullptr, nullptr, nullptr));
  }

  // Multi-band dataset copy: band-interleaved, pixel-interleaved, compressed,
  // skip holes, JPEG2000 compression metadata, GSBAP_LARGEST_CHUNK_POSSIBLE,
  // and progress interruption.
  {
    TestDataset src(16, 16), dst(16, 16);
    auto* sb1 = new TestRasterBand(&src, 1, GDT_Byte, 16, 16, 8, 8);
    auto* sb2 = new TestRasterBand(&src, 2, GDT_Byte, 16, 16, 8, 8);
    std::fill(sb1->raw_data().begin(), sb1->raw_data().end(), 101);
    std::fill(sb2->raw_data().begin(), sb2->raw_data().end(), 202);
    sb1->SetSuggestedAccessPattern(GSBAP_LARGEST_CHUNK_POSSIBLE);
    src.AddTestBand(sb1);
    src.AddTestBand(sb2);

    auto* db1 = new TestRasterBand(&dst, 1, GDT_Byte, 16, 16, 8, 8);
    auto* db2 = new TestRasterBand(&dst, 2, GDT_Byte, 16, 16, 8, 8);
    dst.AddTestBand(db1);
    dst.AddTestBand(db2);

    // 1. Default band-interleaved with SKIP_HOLES=YES
    const char* opts_band[] = {"INTERLEAVE=BAND", "SKIP_HOLES=YES",
                               "COMPRESSED=YES", nullptr};
    EXPECT_EQ(CE_None, GDALDatasetCopyWholeRaster(&src, &dst, opts_band,
                                                  nullptr, nullptr));
    dst.FlushCache(false);
    EXPECT_EQ(101, db1->raw_data()[0]);
    EXPECT_EQ(202, db2->raw_data()[0]);

    // 2. Pixel-interleaved with SKIP_HOLES=YES and empty hole coverage
    sb1->SetCoverageStatus(GDAL_DATA_COVERAGE_STATUS_EMPTY);
    sb2->SetCoverageStatus(GDAL_DATA_COVERAGE_STATUS_EMPTY);
    const char* opts_pixel[] = {"INTERLEAVE=PIXEL", "SKIP_HOLES=YES",
                                "COMPRESSED=YES", nullptr};
    EXPECT_EQ(CE_None, GDALDatasetCopyWholeRaster(&src, &dst, opts_pixel,
                                                  nullptr, nullptr));

    // With data coverage and JPEG2000 compression metadata + small swath size
    sb1->SetCoverageStatus(GDAL_DATA_COVERAGE_STATUS_DATA);
    sb2->SetCoverageStatus(GDAL_DATA_COVERAGE_STATUS_DATA);
    src.SetMetadataItem("COMPRESSION", "JPEG2000", "IMAGE_STRUCTURE");
    src.SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
    CPLSetConfigOption("GDAL_SWATH_SIZE", "100");
    EXPECT_EQ(CE_None, GDALDatasetCopyWholeRaster(&src, &dst, opts_pixel,
                                                  nullptr, nullptr));
    CPLSetConfigOption("GDAL_SWATH_SIZE", nullptr);

    // 3. INTERLEAVE=ATTRIBUTES and invalid INTERLEAVE option
    const char* opts_attr[] = {"INTERLEAVE=ATTRIBUTES", nullptr};
    EXPECT_EQ(CE_None, GDALDatasetCopyWholeRaster(&src, &dst, opts_attr,
                                                  nullptr, nullptr));
    const char* opts_invalid[] = {"INTERLEAVE=INVALID_VALUE", nullptr};
    EXPECT_EQ(CE_None, GDALDatasetCopyWholeRaster(&src, &dst, opts_invalid,
                                                  nullptr, nullptr));

    // 4. Progress interruption at 0.0 and mid-way
    double stop_at = 0.0;
    EXPECT_EQ(CE_Failure,
              GDALDatasetCopyWholeRaster(&src, &dst, opts_band,
                                         FailingProgressCallback, &stop_at));
    stop_at = 0.4;
    EXPECT_EQ(CE_Failure,
              GDALDatasetCopyWholeRaster(&src, &dst, opts_band,
                                         FailingProgressCallback, &stop_at));
    EXPECT_EQ(CE_Failure,
              GDALDatasetCopyWholeRaster(&src, &dst, opts_pixel,
                                         FailingProgressCallback, &stop_at));

    // 5. GDALRasterBandCopyWholeRaster
    EXPECT_EQ(CE_None, GDALRasterBandCopyWholeRaster(sb1, db1, opts_band,
                                                     nullptr, nullptr));
    sb1->SetCoverageStatus(GDAL_DATA_COVERAGE_STATUS_EMPTY);
    EXPECT_EQ(CE_None, GDALRasterBandCopyWholeRaster(sb1, db1, opts_band,
                                                     nullptr, nullptr));
    sb1->SetCoverageStatus(GDAL_DATA_COVERAGE_STATUS_DATA);
    stop_at = 0.0;
    EXPECT_EQ(CE_Failure,
              GDALRasterBandCopyWholeRaster(sb1, db1, opts_band,
                                            FailingProgressCallback, &stop_at));
    stop_at = 0.5;
    EXPECT_EQ(CE_Failure,
              GDALRasterBandCopyWholeRaster(sb1, db1, opts_band,
                                            FailingProgressCallback, &stop_at));
  }

  CPLPopErrorHandler();
}

}  // namespace
