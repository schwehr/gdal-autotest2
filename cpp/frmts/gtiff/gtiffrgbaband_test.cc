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

class GTiffRGBABandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(driver_, nullptr);
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDriver* driver_ = nullptr;
};

// Tests stripped contiguous CMYK -> RGBA band conversion, partial final strip,
// color interpretations, coverage status, and write rejection.
TEST_F(GTiffRGBABandTest, StrippedContigPartialStripAndWriteReject) {
  constexpr char kFilename[] = "/vsimem/rgba_stripped_contig.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 16;
  constexpr int kHeight = 20;  // 2 strips of 16 -> final strip has 4 rows

  char** options = nullptr;
  options = CSLSetNameValue(options, "PHOTOMETRIC", "CMYK");
  options = CSLSetNameValue(options, "INTERLEAVE", "PIXEL");
  options = CSLSetNameValue(options, "BLOCKYSIZE", "16");
  GDALDataset* ds =
      driver_->Create(kFilename, kWidth, kHeight, 4, GDT_Byte, options);
  CSLDestroy(options);
  ASSERT_NE(ds, nullptr);

  std::vector<uint8_t> data(kWidth * kHeight, 50);
  for (int b = 1; b <= 4; ++b) {
    ASSERT_EQ(ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                             data.data(), kWidth, kHeight,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
  }
  GDALClose(ds);

  GDALDataset* read_ds =
      static_cast<GDALDataset*>(GDALOpen(kFilename, GA_Update));
  ASSERT_NE(read_ds, nullptr);
  absl::Cleanup close_read_ds = [&]() { GDALClose(read_ds); };

  ASSERT_EQ(read_ds->GetRasterCount(), 4);
  EXPECT_EQ(read_ds->GetRasterBand(1)->GetColorInterpretation(), GCI_RedBand);
  EXPECT_EQ(read_ds->GetRasterBand(2)->GetColorInterpretation(), GCI_GreenBand);
  EXPECT_EQ(read_ds->GetRasterBand(3)->GetColorInterpretation(), GCI_BlueBand);
  EXPECT_EQ(read_ds->GetRasterBand(4)->GetColorInterpretation(), GCI_AlphaBand);

  GDALRasterBand* b1 = read_ds->GetRasterBand(1);
  int status = b1->GetDataCoverageStatus(0, 0, 1, 1, 0, nullptr);
  EXPECT_TRUE(status & GDAL_DATA_COVERAGE_STATUS_DATA);

  int block_x = 0, block_y = 0;
  b1->GetBlockSize(&block_x, &block_y);
  std::vector<uint8_t> block_buf(block_x * block_y, 0);

  // IWriteBlock rejects writes on RGBA bands.
  {
    CPLErrorStateBackuper quiet_errors(CPLQuietErrorHandler);
    EXPECT_EQ(b1->WriteBlock(0, 0, block_buf.data()), CE_Failure);
  }

  // Read full strip 0 and partial strip 1. libtiff converts CMYK to RGB as
  // r = (255 - K) * (255 - C) / 255, so C=M=Y=K=50 -> 205 * 205 / 255 = 164,
  // and the synthesized alpha is always 255.
  ASSERT_EQ(b1->ReadBlock(0, 0, block_buf.data()), CE_None);
  EXPECT_EQ(block_buf[0], 164);
  ASSERT_EQ(b1->ReadBlock(0, 1, block_buf.data()), CE_None);
  EXPECT_EQ(block_buf[0], 164);
  std::vector<uint8_t> alpha_buf(block_x * block_y, 0);
  ASSERT_EQ(read_ds->GetRasterBand(4)->ReadBlock(0, 0, alpha_buf.data()),
            CE_None);
  EXPECT_EQ(alpha_buf[0], 255);
}

// Tests PLANARCONFIG_SEPARATE (INTERLEAVE=BAND) and missing sparse blocks.
TEST_F(GTiffRGBABandTest, SeparatePlanarAndSparseBlocks) {
  constexpr char kFilename[] = "/vsimem/rgba_separate.tif";
  constexpr char kSparseContig[] = "/vsimem/rgba_sparse_contig.tif";
  constexpr char kSparseSep[] = "/vsimem/rgba_sparse_sep.tif";
  absl::Cleanup cleanup = [&]() {
    VSIUnlink(kFilename);
    VSIUnlink(kSparseContig);
    VSIUnlink(kSparseSep);
  };

  // 1. Valid INTERLEAVE=BAND CMYK file.
  {
    char** options = nullptr;
    options = CSLSetNameValue(options, "PHOTOMETRIC", "CMYK");
    options = CSLSetNameValue(options, "INTERLEAVE", "BAND");
    options = CSLSetNameValue(options, "BLOCKYSIZE", "16");
    GDALDataset* ds = driver_->Create(kFilename, 16, 16, 4, GDT_Byte, options);
    CSLDestroy(options);
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> data(16 * 16, 20);
    for (int b = 1; b <= 4; ++b) {
      ASSERT_EQ(
          ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 16, 16, data.data(),
                                         16, 16, GDT_Byte, 0, 0, nullptr),
          CE_None);
    }
    GDALClose(ds);

    GDALDataset* read_ds =
        static_cast<GDALDataset*>(GDALOpen(kFilename, GA_ReadOnly));
    ASSERT_NE(read_ds, nullptr);
    absl::Cleanup close_read = [&]() { GDALClose(read_ds); };
    std::vector<uint8_t> buf(16 * 16, 0);
    EXPECT_EQ(read_ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()), CE_None);
  }

  // 2. Sparse contiguous CMYK file (unwritten block -> IsBlockAvailable false).
  {
    char** options = nullptr;
    options = CSLSetNameValue(options, "PHOTOMETRIC", "CMYK");
    options = CSLSetNameValue(options, "SPARSE_OK", "YES");
    GDALDataset* ds =
        driver_->Create(kSparseContig, 16, 16, 4, GDT_Byte, options);
    CSLDestroy(options);
    ASSERT_NE(ds, nullptr);
    GDALClose(ds);

    GDALDataset* read_ds =
        static_cast<GDALDataset*>(GDALOpen(kSparseContig, GA_ReadOnly));
    ASSERT_NE(read_ds, nullptr);
    absl::Cleanup close_read = [&]() { GDALClose(read_ds); };
    std::vector<uint8_t> buf(16 * 16, 0);
    CPLErrorStateBackuper quiet_errors(CPLQuietErrorHandler);
    EXPECT_EQ(read_ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()),
              CE_Failure);
  }

  // 3. Sparse separate CMYK file.
  {
    char** options = nullptr;
    options = CSLSetNameValue(options, "PHOTOMETRIC", "CMYK");
    options = CSLSetNameValue(options, "INTERLEAVE", "BAND");
    options = CSLSetNameValue(options, "SPARSE_OK", "YES");
    GDALDataset* ds = driver_->Create(kSparseSep, 16, 16, 4, GDT_Byte, options);
    CSLDestroy(options);
    ASSERT_NE(ds, nullptr);
    GDALClose(ds);

    GDALDataset* read_ds =
        static_cast<GDALDataset*>(GDALOpen(kSparseSep, GA_ReadOnly));
    ASSERT_NE(read_ds, nullptr);
    absl::Cleanup close_read = [&]() { GDALClose(read_ds); };
    std::vector<uint8_t> buf(16 * 16, 0);
    CPLErrorStateBackuper quiet_errors(CPLQuietErrorHandler);
    EXPECT_EQ(read_ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()),
              CE_Failure);
  }
}

// Tests tiled RGBA reading and read failures in both tiled and stripped modes.
TEST_F(GTiffRGBABandTest, TiledReadAndCorruptedReadFailures) {
  constexpr char kTiledFile[] = "/vsimem/rgba_tiled.tif";
  constexpr char kStrippedCorrupt[] = "/vsimem/rgba_stripped_corrupt.tif";
  absl::Cleanup cleanup = [&]() {
    VSIUnlink(kTiledFile);
    VSIUnlink(kStrippedCorrupt);
  };

  // 1. Tiled LZW CMYK: valid read of tile (0,0), then corrupt tile (1,0).
  {
    char** options = nullptr;
    options = CSLSetNameValue(options, "PHOTOMETRIC", "CMYK");
    options = CSLSetNameValue(options, "TILED", "YES");
    options = CSLSetNameValue(options, "BLOCKXSIZE", "16");
    options = CSLSetNameValue(options, "BLOCKYSIZE", "16");
    options = CSLSetNameValue(options, "COMPRESS", "LZW");
    GDALDataset* ds = driver_->Create(kTiledFile, 32, 16, 4, GDT_Byte, options);
    CSLDestroy(options);
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> data(32 * 16);
    for (size_t i = 0; i < data.size(); ++i) {
      data[i] = static_cast<uint8_t>((i * 73 + 11) % 256);
    }
    for (int b = 1; b <= 4; ++b) {
      ASSERT_EQ(
          ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 32, 16, data.data(),
                                         32, 16, GDT_Byte, 0, 0, nullptr),
          CE_None);
    }
    GDALClose(ds);

    GDALDataset* read_ds =
        static_cast<GDALDataset*>(GDALOpen(kTiledFile, GA_ReadOnly));
    ASSERT_NE(read_ds, nullptr);
    absl::Cleanup close_read = [&]() { GDALClose(read_ds); };

    std::vector<uint8_t> buf(16 * 16, 0);
    ASSERT_EQ(read_ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()), CE_None);

    // Corrupt tail of file (tile 1,0 LZW stream).
    vsi_l_offset len = 0;
    GByte* mem = VSIGetMemFileBuffer(kTiledFile, &len, FALSE);
    ASSERT_NE(mem, nullptr);
    ASSERT_GE(len, 100u);
    for (vsi_l_offset i = len - 100; i < len; ++i) {
      mem[i] = 0xFF;
    }

    CPLErrorStateBackuper quiet_errors(CPLQuietErrorHandler);
    EXPECT_EQ(read_ds->GetRasterBand(1)->ReadBlock(1, 0, buf.data()),
              CE_Failure);
  }

  // 2. Stripped LZW CMYK corrupted strip -> TIFFReadRGBAStripExt fails.
  {
    char** options = nullptr;
    options = CSLSetNameValue(options, "PHOTOMETRIC", "CMYK");
    options = CSLSetNameValue(options, "BLOCKYSIZE", "16");
    options = CSLSetNameValue(options, "COMPRESS", "LZW");
    GDALDataset* ds =
        driver_->Create(kStrippedCorrupt, 16, 16, 4, GDT_Byte, options);
    CSLDestroy(options);
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> data(16 * 16);
    for (size_t i = 0; i < data.size(); ++i) {
      data[i] = static_cast<uint8_t>((i * 97 + 3) % 256);
    }
    for (int b = 1; b <= 4; ++b) {
      ASSERT_EQ(
          ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 16, 16, data.data(),
                                         16, 16, GDT_Byte, 0, 0, nullptr),
          CE_None);
    }
    GDALClose(ds);

    GDALDataset* read_ds =
        static_cast<GDALDataset*>(GDALOpen(kStrippedCorrupt, GA_ReadOnly));
    ASSERT_NE(read_ds, nullptr);
    absl::Cleanup close_read = [&]() { GDALClose(read_ds); };

    vsi_l_offset len = 0;
    GByte* mem = VSIGetMemFileBuffer(kStrippedCorrupt, &len, FALSE);
    ASSERT_NE(mem, nullptr);
    ASSERT_GE(len, 100u);
    for (vsi_l_offset i = len - 100; i < len; ++i) {
      mem[i] = 0xFF;
    }

    CPLErrorStateBackuper quiet_errors(CPLQuietErrorHandler);
    std::vector<uint8_t> buf(16 * 16, 0);
    EXPECT_EQ(read_ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()),
              CE_Failure);
  }
}

}  // namespace
}  // namespace autotest2
