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
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class GTiffSplitBitmapBandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(driver_, nullptr);
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDriver* driver_ = nullptr;
};

// Tests normal 1-bit split bitmap band operations: read, advance, rewind,
// coverage status, and write rejection.
TEST_F(GTiffSplitBitmapBandTest, ReadRewindCoverageAndWriteReject) {
  constexpr char kFilename[] = "/vsimem/split_bitmap.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 16;
  constexpr int kHeight = 2001;

  char** options = nullptr;
  options = CSLSetNameValue(options, "NBITS", "1");
  options = CSLSetNameValue(options, "COMPRESS", "PACKBITS");
  options = CSLSetNameValue(options, "BLOCKYSIZE", "2001");
  GDALDataset* ds =
      driver_->Create(kFilename, kWidth, kHeight, 1, GDT_Byte, options);
  CSLDestroy(options);
  ASSERT_NE(ds, nullptr);

  std::vector<uint8_t> full_img(kWidth * kHeight, 0);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      full_img[y * kWidth + x] = static_cast<uint8_t>((x + y) % 2);
    }
  }
  ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           full_img.data(), kWidth, kHeight,
                                           GDT_Byte, 0, 0, nullptr),
            CE_None);
  GDALClose(ds);

  GDALDataset* read_ds =
      static_cast<GDALDataset*>(GDALOpen(kFilename, GA_Update));
  ASSERT_NE(read_ds, nullptr);
  absl::Cleanup close_read_ds = [&]() { GDALClose(read_ds); };

  GDALRasterBand* band = read_ds->GetRasterBand(1);
  ASSERT_NE(band, nullptr);

  int block_x = 0, block_y = 0;
  band->GetBlockSize(&block_x, &block_y);
  ASSERT_EQ(block_x, kWidth);
  ASSERT_EQ(block_y, 1);

  int status = band->GetDataCoverageStatus(0, 0, 1, 1, 0, nullptr);
  EXPECT_TRUE(status & GDAL_DATA_COVERAGE_STATUS_DATA);

  // WriteBlock delegates to GTiffSplitBitmapBand::IWriteBlock and fails.
  std::vector<uint8_t> buf(kWidth, 0);
  {
    CPLErrorStateBackuper quiet_errors(CPLQuietErrorHandler);
    EXPECT_EQ(band->WriteBlock(0, 0, buf.data()), CE_Failure);
  }

  // Read line 0 (unpacks both 0 and 1 bits).
  ASSERT_EQ(band->ReadBlock(0, 0, buf.data()), CE_None);
  for (int x = 0; x < kWidth; ++x) {
    EXPECT_EQ(buf[x], static_cast<uint8_t>(x % 2)) << "x=" << x;
  }

  // Advance to line 5.
  ASSERT_EQ(band->ReadBlock(0, 5, buf.data()), CE_None);
  for (int x = 0; x < kWidth; ++x) {
    EXPECT_EQ(buf[x], static_cast<uint8_t>((x + 5) % 2)) << "x=" << x;
  }

  // Rewind to line 2.
  ASSERT_EQ(band->ReadBlock(0, 2, buf.data()), CE_None);
  for (int x = 0; x < kWidth; ++x) {
    EXPECT_EQ(buf[x], static_cast<uint8_t>((x + 2) % 2)) << "x=" << x;
  }
}

// Tests CCITTFAX4 Premature EOF handling and m_nLastLineValid short-circuit.
TEST_F(GTiffSplitBitmapBandTest, FaxPrematureEofShortCircuitsSubsequentLines) {
  constexpr char kFilename[] = "/vsimem/split_bitmap_fax_eof.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 64;
  constexpr int kHeight = 2001;

  char** options = nullptr;
  options = CSLSetNameValue(options, "NBITS", "1");
  options = CSLSetNameValue(options, "COMPRESS", "CCITTFAX4");
  options = CSLSetNameValue(options, "BLOCKYSIZE", "2001");
  GDALDataset* ds =
      driver_->Create(kFilename, kWidth, kHeight, 1, GDT_Byte, options);
  CSLDestroy(options);
  ASSERT_NE(ds, nullptr);

  std::vector<uint8_t> full_img(kWidth * kHeight, 0);
  for (size_t i = 0; i < full_img.size(); ++i) {
    full_img[i] = static_cast<uint8_t>(((i * 37) ^ (i >> 3)) & 1);
  }
  ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           full_img.data(), kWidth, kHeight,
                                           GDT_Byte, 0, 0, nullptr),
            CE_None);
  GDALClose(ds);

  GDALDataset* read_ds =
      static_cast<GDALDataset*>(GDALOpen(kFilename, GA_ReadOnly));
  ASSERT_NE(read_ds, nullptr);
  absl::Cleanup close_read_ds = [&]() { GDALClose(read_ds); };

  GDALRasterBand* band = read_ds->GetRasterBand(1);
  ASSERT_NE(band, nullptr);
  int block_x = 0, block_y = 0;
  band->GetBlockSize(&block_x, &block_y);
  ASSERT_EQ(block_y, 1);

  // Truncate the CCITTFAX4 strip in memory so Fax4Decode emits "Premature EOF".
  vsi_l_offset data_len = 0;
  GByte* mem_buf = VSIGetMemFileBuffer(kFilename, &data_len, FALSE);
  ASSERT_NE(mem_buf, nullptr);
  ASSERT_GT(data_len, 1000);

  VSILFILE* fp = VSIFOpenL(kFilename, "r+");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(VSIFTruncateL(fp, data_len / 2), 0);
  ASSERT_EQ(VSIFCloseL(fp), 0);

  CPLErrorStateBackuper quiet_errors(CPLQuietErrorHandler);
  std::vector<uint8_t> buf(kWidth, 0);
  // Reading near the end hits Premature EOF and sets m_nLastLineValid.
  EXPECT_EQ(band->ReadBlock(0, 1990, buf.data()), CE_Failure);
  // Subsequent read past m_nLastLineValid immediately returns CE_Failure.
  EXPECT_EQ(band->ReadBlock(0, 1995, buf.data()), CE_Failure);
}

}  // namespace
}  // namespace autotest2
