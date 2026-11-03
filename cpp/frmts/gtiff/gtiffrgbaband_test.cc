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
// Tests RGBA pseudo-band color interpretation, CMYK-to-RGBA decoding,
// coverage status, and sparse/error handling (gtiffrgbaband.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gtiffrgbaband.cpp

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

class GTiffRGBABandTest : public ::testing::Test {
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

// Tests stripped contiguous CMYK -> RGBA band conversion, partial final strip,
// color interpretations, coverage status, and write rejection.
TEST_F(GTiffRGBABandTest, StrippedContigPartialStripAndWriteReject) {
  constexpr char kFilename[] = "/vsimem/rgba_stripped_contig.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };

  constexpr int kWidth = 16;
  constexpr int kHeight = 20;  // 2 strips of 16 -> final strip has 4 rows

  {
    CPLStringList options;
    options.AddNameValue("PHOTOMETRIC", "CMYK");
    options.AddNameValue("INTERLEAVE", "PIXEL");
    options.AddNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(driver_->Create(kFilename, kWidth, kHeight, 4,
                                            GDT_Byte, options.List()));
    ASSERT_NE(nullptr, ds);

    std::vector<uint8_t> data(kWidth * kHeight, 50);
    for (int b = 1; b <= 4; ++b) {
      ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, kWidth, kHeight, data.data(),
                             kWidth, kHeight, GDT_Byte, 0, 0, nullptr));
    }
  }

  GDALDatasetUniquePtr read_ds(
      GDALDataset::Open(kFilename, GDAL_OF_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, read_ds);

  ASSERT_EQ(4, read_ds->GetRasterCount());
  EXPECT_EQ(GCI_RedBand, read_ds->GetRasterBand(1)->GetColorInterpretation());
  EXPECT_EQ(GCI_GreenBand, read_ds->GetRasterBand(2)->GetColorInterpretation());
  EXPECT_EQ(GCI_BlueBand, read_ds->GetRasterBand(3)->GetColorInterpretation());
  EXPECT_EQ(GCI_AlphaBand, read_ds->GetRasterBand(4)->GetColorInterpretation());

  GDALRasterBand* b1 = read_ds->GetRasterBand(1);
  int status = b1->GetDataCoverageStatus(0, 0, 1, 1, 0, nullptr);
  EXPECT_NE(0, status & GDAL_DATA_COVERAGE_STATUS_DATA);

  int block_x = 0;
  int block_y = 0;
  b1->GetBlockSize(&block_x, &block_y);
  std::vector<uint8_t> block_buf(block_x * block_y, 0);

  // IWriteBlock rejects writes on RGBA bands.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, b1->WriteBlock(0, 0, block_buf.data()));
    CPLErrorReset();
  }

  // Read full strip 0 and partial strip 1. libtiff converts CMYK to RGB as
  // r = (255 - K) * (255 - C) / 255, so C=M=Y=K=50 -> 205 * 205 / 255 = 164,
  // and the synthesized alpha is always 255.
  ASSERT_EQ(CE_None, b1->ReadBlock(0, 0, block_buf.data()));
  EXPECT_EQ(164, block_buf[0]);
  ASSERT_EQ(CE_None, b1->ReadBlock(0, 1, block_buf.data()));
  EXPECT_EQ(164, block_buf[0]);
  std::vector<uint8_t> alpha_buf(block_x * block_y, 0);
  ASSERT_EQ(CE_None,
            read_ds->GetRasterBand(4)->ReadBlock(0, 0, alpha_buf.data()));
  EXPECT_EQ(255, alpha_buf[0]);
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
    {
      CPLStringList options;
      options.AddNameValue("PHOTOMETRIC", "CMYK");
      options.AddNameValue("INTERLEAVE", "BAND");
      options.AddNameValue("BLOCKYSIZE", "16");
      GDALDatasetUniquePtr ds(
          driver_->Create(kFilename, 16, 16, 4, GDT_Byte, options.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<uint8_t> data(16 * 16, 20);
      for (int b = 1; b <= 4; ++b) {
        ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                               GF_Write, 0, 0, 16, 16, data.data(), 16, 16,
                               GDT_Byte, 0, 0, nullptr));
      }
    }

    GDALDatasetUniquePtr read_ds(
        GDALDataset::Open(kFilename, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, read_ds);
    std::vector<uint8_t> buf(16 * 16, 0);
    EXPECT_EQ(CE_None, read_ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
  }

  // 2. Sparse contiguous CMYK file (unwritten block -> IsBlockAvailable false).
  {
    {
      CPLStringList options;
      options.AddNameValue("PHOTOMETRIC", "CMYK");
      options.AddNameValue("SPARSE_OK", "YES");
      GDALDatasetUniquePtr ds(
          driver_->Create(kSparseContig, 16, 16, 4, GDT_Byte, options.List()));
      ASSERT_NE(nullptr, ds);
    }

    GDALDatasetUniquePtr read_ds(
        GDALDataset::Open(kSparseContig, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, read_ds);
    std::vector<uint8_t> buf(16 * 16, 0);
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure,
              read_ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
    CPLErrorReset();
  }

  // 3. Sparse separate CMYK file.
  {
    {
      CPLStringList options;
      options.AddNameValue("PHOTOMETRIC", "CMYK");
      options.AddNameValue("INTERLEAVE", "BAND");
      options.AddNameValue("SPARSE_OK", "YES");
      GDALDatasetUniquePtr ds(
          driver_->Create(kSparseSep, 16, 16, 4, GDT_Byte, options.List()));
      ASSERT_NE(nullptr, ds);
    }

    GDALDatasetUniquePtr read_ds(
        GDALDataset::Open(kSparseSep, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, read_ds);
    std::vector<uint8_t> buf(16 * 16, 0);
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure,
              read_ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
    CPLErrorReset();
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
    {
      CPLStringList options;
      options.AddNameValue("PHOTOMETRIC", "CMYK");
      options.AddNameValue("TILED", "YES");
      options.AddNameValue("BLOCKXSIZE", "16");
      options.AddNameValue("BLOCKYSIZE", "16");
      options.AddNameValue("COMPRESS", "LZW");
      GDALDatasetUniquePtr ds(
          driver_->Create(kTiledFile, 32, 16, 4, GDT_Byte, options.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<uint8_t> data(32 * 16);
      for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 73 + 11) % 256);
      }
      for (int b = 1; b <= 4; ++b) {
        ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                               GF_Write, 0, 0, 32, 16, data.data(), 32, 16,
                               GDT_Byte, 0, 0, nullptr));
      }
    }

    GDALDatasetUniquePtr read_ds(
        GDALDataset::Open(kTiledFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, read_ds);

    std::vector<uint8_t> buf(16 * 16, 0);
    ASSERT_EQ(CE_None, read_ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));

    // Corrupt tail of file (tile 1,0 LZW stream).
    vsi_l_offset len = 0;
    GByte* mem = VSIGetMemFileBuffer(kTiledFile, &len, FALSE);
    ASSERT_NE(nullptr, mem);
    ASSERT_GE(len, 100u);
    for (vsi_l_offset i = len - 100; i < len; ++i) {
      mem[i] = 0xFF;
    }

    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure,
              read_ds->GetRasterBand(1)->ReadBlock(1, 0, buf.data()));
    CPLErrorReset();
  }

  // 2. Stripped LZW CMYK corrupted strip -> TIFFReadRGBAStripExt fails.
  {
    {
      CPLStringList options;
      options.AddNameValue("PHOTOMETRIC", "CMYK");
      options.AddNameValue("BLOCKYSIZE", "16");
      options.AddNameValue("COMPRESS", "LZW");
      GDALDatasetUniquePtr ds(driver_->Create(kStrippedCorrupt, 16, 16, 4,
                                              GDT_Byte, options.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<uint8_t> data(16 * 16);
      for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 97 + 3) % 256);
      }
      for (int b = 1; b <= 4; ++b) {
        ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                               GF_Write, 0, 0, 16, 16, data.data(), 16, 16,
                               GDT_Byte, 0, 0, nullptr));
      }
    }

    GDALDatasetUniquePtr read_ds(
        GDALDataset::Open(kStrippedCorrupt, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, read_ds);

    vsi_l_offset len = 0;
    GByte* mem = VSIGetMemFileBuffer(kStrippedCorrupt, &len, FALSE);
    ASSERT_NE(nullptr, mem);
    ASSERT_GE(len, 100u);
    for (vsi_l_offset i = len - 100; i < len; ++i) {
      mem[i] = 0xFF;
    }

    WithQuietHandler quiet;
    std::vector<uint8_t> buf(16 * 16, 0);
    EXPECT_EQ(CE_Failure,
              read_ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
