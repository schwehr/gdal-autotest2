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

class GTiffSplitBandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(driver_, nullptr);
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDriver* driver_ = nullptr;
};

// Tests 1-band split band: read, rewind, coverage status, write block
// rejection. Uses PACKBITS compression so libtiff does not chop the single
// strip into multiple 8KB strips.
TEST_F(GTiffSplitBandTest, SingleBandReadAndWrite) {
  constexpr char kFilename[] = "/vsimem/split_band_1band.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 10;
  constexpr int kHeight = 2001;  // > 2000 triggers split band for single strip

  char** options = nullptr;
  options = CSLSetNameValue(options, "COMPRESS", "PACKBITS");
  options = CSLSetNameValue(options, "BLOCKYSIZE", "2001");
  GDALDataset* ds =
      driver_->Create(kFilename, kWidth, kHeight, 1, GDT_Byte, options);
  CSLDestroy(options);
  ASSERT_NE(ds, nullptr);

  std::vector<uint8_t> full_img(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      full_img[y * kWidth + x] = static_cast<uint8_t>((x + y) % 256);
    }
  }
  ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           full_img.data(), kWidth, kHeight,
                                           GDT_Byte, 0, 0, nullptr),
            CE_None);
  GDALClose(ds);

  // Open in GA_Update mode so GDALRasterBand::WriteBlock delegates to
  // GTiffSplitBand::IWriteBlock rather than rejecting at the base class level.
  GDALDataset* read_ds =
      static_cast<GDALDataset*>(GDALOpen(kFilename, GA_Update));
  ASSERT_NE(read_ds, nullptr);
  absl::Cleanup close_read_ds = [&]() { GDALClose(read_ds); };

  GDALRasterBand* band = read_ds->GetRasterBand(1);
  ASSERT_NE(band, nullptr);

  // Split band sets block size to (kWidth, 1).
  int block_x_size = 0;
  int block_y_size = 0;
  band->GetBlockSize(&block_x_size, &block_y_size);
  ASSERT_EQ(block_x_size, kWidth);
  ASSERT_EQ(block_y_size, 1);

  // Test coverage status.
  int status = band->GetDataCoverageStatus(0, 0, 1, 1, 0, nullptr);
  EXPECT_TRUE(status & GDAL_DATA_COVERAGE_STATUS_DATA);

  // Test IWriteBlock rejection on split band.
  std::vector<uint8_t> write_buf(kWidth, 0);
  {
    CPLErrorStateBackuper quiet_errors(CPLQuietErrorHandler);
    CPLErr write_err = band->WriteBlock(0, 0, write_buf.data());
    EXPECT_EQ(write_err, CE_Failure);
  }

  // Test read scanline 0.
  std::vector<uint8_t> read_buf(kWidth, 0);
  ASSERT_EQ(band->ReadBlock(0, 0, read_buf.data()), CE_None);
  for (int x = 0; x < kWidth; ++x) {
    EXPECT_EQ(read_buf[x], static_cast<uint8_t>(x % 256)) << "x=" << x;
  }

  // Test advancing scanlines: read scanline 5 (tests while loop).
  ASSERT_EQ(band->ReadBlock(0, 5, read_buf.data()), CE_None);
  for (int x = 0; x < kWidth; ++x) {
    EXPECT_EQ(read_buf[x], static_cast<uint8_t>((x + 5) % 256)) << "x=" << x;
  }

  // Test rewinding scanline: read scanline 2 (tests m_nLoadedBlock >=
  // nBlockYOff).
  ASSERT_EQ(band->ReadBlock(0, 2, read_buf.data()), CE_None);
  for (int x = 0; x < kWidth; ++x) {
    EXPECT_EQ(read_buf[x], static_cast<uint8_t>((x + 2) % 256)) << "x=" << x;
  }
}

// Tests contiguous multi-band split band (INTERLEAVE=PIXEL).
TEST_F(GTiffSplitBandTest, MultiBandContig) {
  constexpr char kFilename[] = "/vsimem/split_band_contig.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 8;
  constexpr int kHeight = 2001;

  char** options = nullptr;
  options = CSLSetNameValue(options, "COMPRESS", "PACKBITS");
  options = CSLSetNameValue(options, "BLOCKYSIZE", "2001");
  options = CSLSetNameValue(options, "INTERLEAVE", "PIXEL");
  GDALDataset* ds =
      driver_->Create(kFilename, kWidth, kHeight, 2, GDT_Byte, options);
  CSLDestroy(options);
  ASSERT_NE(ds, nullptr);

  std::vector<uint8_t> scanline_b1(kWidth, 42);
  std::vector<uint8_t> scanline_b2(kWidth, 84);
  ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, kWidth, 1,
                                           scanline_b1.data(), kWidth, 1,
                                           GDT_Byte, 0, 0, nullptr),
            CE_None);
  ASSERT_EQ(ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, kWidth, 1,
                                           scanline_b2.data(), kWidth, 1,
                                           GDT_Byte, 0, 0, nullptr),
            CE_None);
  GDALClose(ds);

  GDALDataset* read_ds =
      static_cast<GDALDataset*>(GDALOpen(kFilename, GA_ReadOnly));
  ASSERT_NE(read_ds, nullptr);
  absl::Cleanup close_read_ds = [&]() { GDALClose(read_ds); };

  GDALRasterBand* b1 = read_ds->GetRasterBand(1);
  GDALRasterBand* b2 = read_ds->GetRasterBand(2);
  ASSERT_NE(b1, nullptr);
  ASSERT_NE(b2, nullptr);

  int block_x = 0, block_y = 0;
  b1->GetBlockSize(&block_x, &block_y);
  ASSERT_EQ(block_y, 1);

  std::vector<uint8_t> buf1(kWidth, 0);
  std::vector<uint8_t> buf2(kWidth, 0);

  // Read band 1 line 0: allocates m_pabyBlockBuf, reads scanline, extracts b1.
  ASSERT_EQ(b1->ReadBlock(0, 0, buf1.data()), CE_None);
  EXPECT_EQ(buf1[0], 42);

  // Read band 2 line 0: cached scanline hit (m_nLoadedBlock == nBlockYOff),
  // jumps to extract_band_data and extracts b2.
  ASSERT_EQ(b2->ReadBlock(0, 0, buf2.data()), CE_None);
  EXPECT_EQ(buf2[0], 84);

  // Read band 1 line 1: advances scanline with already-allocated
  // m_pabyBlockBuf.
  ASSERT_EQ(b1->ReadBlock(0, 1, buf1.data()), CE_None);
}

// Tests separate multi-band split band (INTERLEAVE=BAND).
TEST_F(GTiffSplitBandTest, MultiBandSeparate) {
  constexpr char kFilename[] = "/vsimem/split_band_separate.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 8;
  constexpr int kHeight = 2001;

  char** options = nullptr;
  options = CSLSetNameValue(options, "COMPRESS", "PACKBITS");
  options = CSLSetNameValue(options, "BLOCKYSIZE", "2001");
  options = CSLSetNameValue(options, "INTERLEAVE", "BAND");
  GDALDataset* ds =
      driver_->Create(kFilename, kWidth, kHeight, 2, GDT_Byte, options);
  CSLDestroy(options);
  ASSERT_NE(ds, nullptr);

  std::vector<uint8_t> scanline_b1(kWidth, 10);
  std::vector<uint8_t> scanline_b2(kWidth, 20);
  ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, kWidth, 1,
                                           scanline_b1.data(), kWidth, 1,
                                           GDT_Byte, 0, 0, nullptr),
            CE_None);
  ASSERT_EQ(ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, kWidth, 1,
                                           scanline_b2.data(), kWidth, 1,
                                           GDT_Byte, 0, 0, nullptr),
            CE_None);
  GDALClose(ds);

  GDALDataset* read_ds =
      static_cast<GDALDataset*>(GDALOpen(kFilename, GA_ReadOnly));
  ASSERT_NE(read_ds, nullptr);
  absl::Cleanup close_read_ds = [&]() { GDALClose(read_ds); };

  GDALRasterBand* b1 = read_ds->GetRasterBand(1);
  GDALRasterBand* b2 = read_ds->GetRasterBand(2);
  ASSERT_NE(b1, nullptr);
  ASSERT_NE(b2, nullptr);

  int block_x = 0, block_y = 0;
  b1->GetBlockSize(&block_x, &block_y);
  ASSERT_EQ(block_y, 1);

  std::vector<uint8_t> buf1(kWidth, 0);
  std::vector<uint8_t> buf2(kWidth, 0);

  // Read band 1 line 0.
  ASSERT_EQ(b1->ReadBlock(0, 0, buf1.data()), CE_None);
  EXPECT_EQ(buf1[0], 10);

  // Read band 1 line 1 (same band, m_nLastBandRead == nBand).
  ASSERT_EQ(b1->ReadBlock(0, 1, buf1.data()), CE_None);

  // Read band 2 line 0: band switch resets m_nLoadedBlock to -1.
  ASSERT_EQ(b2->ReadBlock(0, 0, buf2.data()), CE_None);
  EXPECT_EQ(buf2[0], 20);

  // Switch back to band 1 line 1: band switch resets m_nLoadedBlock to -1.
  ASSERT_EQ(b1->ReadBlock(0, 1, buf1.data()), CE_None);
}

// Tests read failure handling in TIFFReadScanline.
TEST_F(GTiffSplitBandTest, ReadScanlineFailure) {
  constexpr char kFilename[] = "/vsimem/split_band_fail.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 32;
  constexpr int kHeight = 2001;

  char** options = nullptr;
  options = CSLSetNameValue(options, "COMPRESS", "LZW");
  options = CSLSetNameValue(options, "BLOCKYSIZE", "2001");
  GDALDataset* ds =
      driver_->Create(kFilename, kWidth, kHeight, 1, GDT_Byte, options);
  CSLDestroy(options);
  ASSERT_NE(ds, nullptr);

  // Fill with pseudo-random data so compressed strip is large.
  std::vector<uint8_t> full_img(kWidth * kHeight);
  for (size_t i = 0; i < full_img.size(); ++i) {
    full_img[i] = static_cast<uint8_t>((i * 131 + 17) % 256);
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

  // Corrupt/zero out the strip payload in memory so LZW decoding fails.
  vsi_l_offset data_len = 0;
  GByte* mem_buf = VSIGetMemFileBuffer(kFilename, &data_len, FALSE);
  ASSERT_NE(mem_buf, nullptr);
  ASSERT_GT(data_len, 2000);
  // Overwrite the middle/end of the file (strip data) with 0xFF (invalid LZW).
  for (vsi_l_offset i = data_len / 2; i < data_len; ++i) {
    mem_buf[i] = 0xFF;
  }

  CPLErrorStateBackuper quiet_errors(CPLQuietErrorHandler);
  std::vector<uint8_t> buf(kWidth, 0);
  CPLErr err = band->ReadBlock(0, 1999, buf.data());
  EXPECT_EQ(err, CE_Failure);
}

}  // namespace
}  // namespace autotest2
