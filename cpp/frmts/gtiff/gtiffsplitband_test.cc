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
// Tests single-strip split scanline raster band reading, band switching,
// scanline rewinding, coverage status, and write rejection
// (gtiffsplitband.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gtiffsplitband.cpp

#include <cstdint>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class GTiffSplitBandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    CPLErrorReset();
    driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, driver_);
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

  {
    CPLStringList options;
    options.AddNameValue("COMPRESS", "PACKBITS");
    options.AddNameValue("BLOCKYSIZE", "2001");
    GDALDatasetUniquePtr ds(driver_->Create(kFilename, kWidth, kHeight, 1,
                                            GDT_Byte, options.List()));
    ASSERT_NE(nullptr, ds);

    std::vector<uint8_t> full_img(kWidth * kHeight);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        full_img[y * kWidth + x] = static_cast<uint8_t>((x + y) % 256);
      }
    }
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, full_img.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0, nullptr));
  }

  // Open in GDAL_OF_UPDATE mode so GDALRasterBand::WriteBlock delegates to
  // GTiffSplitBand::IWriteBlock rather than rejecting at the base class level.
  GDALDatasetUniquePtr read_ds(
      GDALDataset::Open(kFilename, GDAL_OF_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, read_ds);

  GDALRasterBand* band = read_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // Split band sets block size to (kWidth, 1).
  int block_x_size = 0;
  int block_y_size = 0;
  band->GetBlockSize(&block_x_size, &block_y_size);
  ASSERT_EQ(kWidth, block_x_size);
  ASSERT_EQ(1, block_y_size);

  // Test coverage status.
  int status = band->GetDataCoverageStatus(0, 0, 1, 1, 0, nullptr);
  EXPECT_NE(0, status & GDAL_DATA_COVERAGE_STATUS_DATA);

  // Test IWriteBlock rejection on split band.
  std::vector<uint8_t> write_buf(kWidth, 0);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, band->WriteBlock(0, 0, write_buf.data()));
    CPLErrorReset();
  }

  // Test read scanline 0.
  std::vector<uint8_t> read_buf(kWidth, 0);
  ASSERT_EQ(CE_None, band->ReadBlock(0, 0, read_buf.data()));
  for (int x = 0; x < kWidth; ++x) {
    EXPECT_EQ(static_cast<uint8_t>(x % 256), read_buf[x]) << "x=" << x;
  }

  // Test advancing scanlines: read scanline 5 (tests while loop).
  ASSERT_EQ(CE_None, band->ReadBlock(0, 5, read_buf.data()));
  for (int x = 0; x < kWidth; ++x) {
    EXPECT_EQ(static_cast<uint8_t>((x + 5) % 256), read_buf[x]) << "x=" << x;
  }

  // Test rewinding scanline: read scanline 2 (tests m_nLoadedBlock >=
  // nBlockYOff).
  ASSERT_EQ(CE_None, band->ReadBlock(0, 2, read_buf.data()));
  for (int x = 0; x < kWidth; ++x) {
    EXPECT_EQ(static_cast<uint8_t>((x + 2) % 256), read_buf[x]) << "x=" << x;
  }
}

// Tests contiguous multi-band split band (INTERLEAVE=PIXEL).
TEST_F(GTiffSplitBandTest, MultiBandContig) {
  constexpr char kFilename[] = "/vsimem/split_band_contig.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 8;
  constexpr int kHeight = 2001;

  {
    CPLStringList options;
    options.AddNameValue("COMPRESS", "PACKBITS");
    options.AddNameValue("BLOCKYSIZE", "2001");
    options.AddNameValue("INTERLEAVE", "PIXEL");
    GDALDatasetUniquePtr ds(driver_->Create(kFilename, kWidth, kHeight, 2,
                                            GDT_Byte, options.List()));
    ASSERT_NE(nullptr, ds);

    std::vector<uint8_t> scanline_b1(kWidth, 42);
    std::vector<uint8_t> scanline_b2(kWidth, 84);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, 1, scanline_b1.data(),
                           kWidth, 1, GDT_Byte, 0, 0, nullptr));
    ASSERT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(
                           GF_Write, 0, 0, kWidth, 1, scanline_b2.data(),
                           kWidth, 1, GDT_Byte, 0, 0, nullptr));
  }

  GDALDatasetUniquePtr read_ds(
      GDALDataset::Open(kFilename, GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, read_ds);

  GDALRasterBand* b1 = read_ds->GetRasterBand(1);
  GDALRasterBand* b2 = read_ds->GetRasterBand(2);
  ASSERT_NE(nullptr, b1);
  ASSERT_NE(nullptr, b2);

  int block_x = 0;
  int block_y = 0;
  b1->GetBlockSize(&block_x, &block_y);
  ASSERT_EQ(1, block_y);

  std::vector<uint8_t> buf1(kWidth, 0);
  std::vector<uint8_t> buf2(kWidth, 0);

  // Read band 1 line 0: allocates m_pabyBlockBuf, reads scanline, extracts b1.
  ASSERT_EQ(CE_None, b1->ReadBlock(0, 0, buf1.data()));
  EXPECT_EQ(42, buf1[0]);

  // Read band 2 line 0: cached scanline hit (m_nLoadedBlock == nBlockYOff),
  // jumps to extract_band_data and extracts b2.
  ASSERT_EQ(CE_None, b2->ReadBlock(0, 0, buf2.data()));
  EXPECT_EQ(84, buf2[0]);

  // Read band 1 line 1: advances scanline with already-allocated
  // m_pabyBlockBuf.
  ASSERT_EQ(CE_None, b1->ReadBlock(0, 1, buf1.data()));
}

// Tests separate multi-band split band (INTERLEAVE=BAND).
TEST_F(GTiffSplitBandTest, MultiBandSeparate) {
  constexpr char kFilename[] = "/vsimem/split_band_separate.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 8;
  constexpr int kHeight = 2001;

  {
    CPLStringList options;
    options.AddNameValue("COMPRESS", "PACKBITS");
    options.AddNameValue("BLOCKYSIZE", "2001");
    options.AddNameValue("INTERLEAVE", "BAND");
    GDALDatasetUniquePtr ds(driver_->Create(kFilename, kWidth, kHeight, 2,
                                            GDT_Byte, options.List()));
    ASSERT_NE(nullptr, ds);

    std::vector<uint8_t> scanline_b1(kWidth, 10);
    std::vector<uint8_t> scanline_b2(kWidth, 20);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, 1, scanline_b1.data(),
                           kWidth, 1, GDT_Byte, 0, 0, nullptr));
    ASSERT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(
                           GF_Write, 0, 0, kWidth, 1, scanline_b2.data(),
                           kWidth, 1, GDT_Byte, 0, 0, nullptr));
  }

  GDALDatasetUniquePtr read_ds(
      GDALDataset::Open(kFilename, GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, read_ds);

  GDALRasterBand* b1 = read_ds->GetRasterBand(1);
  GDALRasterBand* b2 = read_ds->GetRasterBand(2);
  ASSERT_NE(nullptr, b1);
  ASSERT_NE(nullptr, b2);

  int block_x = 0;
  int block_y = 0;
  b1->GetBlockSize(&block_x, &block_y);
  ASSERT_EQ(1, block_y);

  std::vector<uint8_t> buf1(kWidth, 0);
  std::vector<uint8_t> buf2(kWidth, 0);

  // Read band 1 line 0.
  ASSERT_EQ(CE_None, b1->ReadBlock(0, 0, buf1.data()));
  EXPECT_EQ(10, buf1[0]);

  // Read band 1 line 1 (same band, m_nLastBandRead == nBand).
  ASSERT_EQ(CE_None, b1->ReadBlock(0, 1, buf1.data()));

  // Read band 2 line 0: band switch resets m_nLoadedBlock to -1.
  ASSERT_EQ(CE_None, b2->ReadBlock(0, 0, buf2.data()));
  EXPECT_EQ(20, buf2[0]);

  // Switch back to band 1 line 1: band switch resets m_nLoadedBlock to -1.
  ASSERT_EQ(CE_None, b1->ReadBlock(0, 1, buf1.data()));
}

// Tests read failure handling in TIFFReadScanline.
TEST_F(GTiffSplitBandTest, ReadScanlineFailure) {
  constexpr char kFilename[] = "/vsimem/split_band_fail.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 32;
  constexpr int kHeight = 2001;

  {
    CPLStringList options;
    options.AddNameValue("COMPRESS", "LZW");
    options.AddNameValue("BLOCKYSIZE", "2001");
    GDALDatasetUniquePtr ds(driver_->Create(kFilename, kWidth, kHeight, 1,
                                            GDT_Byte, options.List()));
    ASSERT_NE(nullptr, ds);

    // Fill with pseudo-random data so compressed strip is large.
    std::vector<uint8_t> full_img(kWidth * kHeight);
    for (size_t i = 0; i < full_img.size(); ++i) {
      full_img[i] = static_cast<uint8_t>((i * 131 + 17) % 256);
    }
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, full_img.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0, nullptr));
  }

  GDALDatasetUniquePtr read_ds(
      GDALDataset::Open(kFilename, GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, read_ds);

  GDALRasterBand* band = read_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_x = 0;
  int block_y = 0;
  band->GetBlockSize(&block_x, &block_y);
  ASSERT_EQ(1, block_y);

  // Corrupt the strip payload in memory so LZW decoding fails.
  vsi_l_offset data_len = 0;
  GByte* mem_buf = VSIGetMemFileBuffer(kFilename, &data_len, FALSE);
  ASSERT_NE(nullptr, mem_buf);
  ASSERT_GT(data_len, 2000u);
  for (vsi_l_offset i = data_len / 2; i < data_len; ++i) {
    mem_buf[i] = 0xFF;
  }

  WithQuietHandler quiet;
  std::vector<uint8_t> buf(kWidth, 0);
  EXPECT_EQ(CE_Failure, band->ReadBlock(0, 1999, buf.data()));
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
