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

class GTiffJPEGOverviewDSTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(driver_, nullptr);
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDriver* driver_ = nullptr;
};

// Tests tiled YCbCr & RGB JPEG implicit overviews, partial edge tiles,
// multi-band vs single-band IRasterIO, and color interpretation.
TEST_F(GTiffJPEGOverviewDSTest, TiledRgbAndYcbcrPartialEdgeTiles) {
  constexpr char kYcbcrFile[] = "/vsimem/jpeg_ovr_ycbcr.tif";
  constexpr char kRgbFile[] = "/vsimem/jpeg_ovr_rgb.tif";
  absl::Cleanup cleanup = [&]() {
    VSIUnlink(kYcbcrFile);
    VSIUnlink(kRgbFile);
  };

  constexpr int kWidth = 300;
  constexpr int kHeight = 300;

  // 1. Tiled YCbCr 300x300 (tiles 256x256 -> partial right & bottom tiles).
  {
    char** options = nullptr;
    options = CSLSetNameValue(options, "COMPRESS", "JPEG");
    options = CSLSetNameValue(options, "PHOTOMETRIC", "YCBCR");
    options = CSLSetNameValue(options, "TILED", "YES");
    options = CSLSetNameValue(options, "BLOCKXSIZE", "256");
    options = CSLSetNameValue(options, "BLOCKYSIZE", "256");
    GDALDataset* ds =
        driver_->Create(kYcbcrFile, kWidth, kHeight, 3, GDT_Byte, options);
    CSLDestroy(options);
    ASSERT_NE(ds, nullptr);

    std::vector<uint8_t> img(kWidth * kHeight * 3, 128);
    ASSERT_EQ(ds->RasterIO(GF_Write, 0, 0, kWidth, kHeight, img.data(), kWidth,
                           kHeight, GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    GDALClose(ds);

    GDALDataset* read_ds =
        static_cast<GDALDataset*>(GDALOpen(kYcbcrFile, GA_ReadOnly));
    ASSERT_NE(read_ds, nullptr);
    absl::Cleanup close_read = [&]() { GDALClose(read_ds); };

    GDALRasterBand* b1 = read_ds->GetRasterBand(1);
    EXPECT_EQ(b1->GetOverviewCount(), 0);  // Hidden outside IRasterIO
    GDALRasterBand* ovr_b1 = b1->GetOverview(0);
    ASSERT_NE(ovr_b1, nullptr);
    EXPECT_EQ(ovr_b1->GetColorInterpretation(), GCI_RedBand);

    GDALDataset* ovr_ds = ovr_b1->GetDataset();
    ASSERT_NE(ovr_ds, nullptr);
    EXPECT_STREQ(ovr_ds->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"),
                 "YCbCr JPEG");

    const int ovr_w = ovr_ds->GetRasterXSize();
    const int ovr_h = ovr_ds->GetRasterYSize();
    // Multi-band IRasterIO -> BlockBasedRasterIO, reads all 4 blocks including
    // partial right/bottom edge tiles.
    std::vector<uint8_t> ovr_rgb(ovr_w * ovr_h * 3, 0);
    EXPECT_EQ(
        ovr_ds->RasterIO(GF_Read, 0, 0, ovr_w, ovr_h, ovr_rgb.data(), ovr_w,
                         ovr_h, GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
        CE_None);
    // Uniform 128 source survives lossy JPEG within a small tolerance.
    EXPECT_NEAR(ovr_rgb[0], 128, 4);

    // Single-band IRasterIO -> GDALDataset::IRasterIO.
    std::vector<uint8_t> ovr_single(ovr_w * ovr_h, 0);
    int band_map[1] = {1};
    EXPECT_EQ(
        ovr_ds->RasterIO(GF_Read, 0, 0, ovr_w, ovr_h, ovr_single.data(), ovr_w,
                         ovr_h, GDT_Byte, 1, band_map, 0, 0, 0, nullptr),
        CE_None);
    EXPECT_NEAR(ovr_single[0], 128, 4);
  }

  // 2. Stripped RGB 256x256 (bAddAdobe == true, full width block).
  {
    char** options = nullptr;
    options = CSLSetNameValue(options, "COMPRESS", "JPEG");
    options = CSLSetNameValue(options, "PHOTOMETRIC", "RGB");
    options = CSLSetNameValue(options, "BLOCKYSIZE", "64");
    GDALDataset* ds = driver_->Create(kRgbFile, 256, 256, 3, GDT_Byte, options);
    CSLDestroy(options);
    ASSERT_NE(ds, nullptr);

    std::vector<uint8_t> img(256 * 256 * 3, 100);
    ASSERT_EQ(ds->RasterIO(GF_Write, 0, 0, 256, 256, img.data(), 256, 256,
                           GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    GDALClose(ds);

    GDALDataset* read_ds =
        static_cast<GDALDataset*>(GDALOpen(kRgbFile, GA_ReadOnly));
    ASSERT_NE(read_ds, nullptr);
    absl::Cleanup close_read = [&]() { GDALClose(read_ds); };

    GDALRasterBand* ovr_b1 = read_ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(ovr_b1, nullptr);
    GDALDataset* ovr_ds = ovr_b1->GetDataset();
    ASSERT_NE(ovr_ds, nullptr);
    EXPECT_STREQ(ovr_ds->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"),
                 "JPEG");

    std::vector<uint8_t> buf(128 * 128, 0);
    EXPECT_EQ(ovr_b1->RasterIO(GF_Read, 0, 0, 128, 128, buf.data(), 128, 128,
                               GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_NEAR(buf[0], 100, 4);
  }
}

// Tests PLANARCONFIG_SEPARATE, 4-band contiguous RGBA JPEG, and sparse blocks.
TEST_F(GTiffJPEGOverviewDSTest, SeparatePlanarFourBandAndSparse) {
  constexpr char kSepFile[] = "/vsimem/jpeg_ovr_sep.tif";
  constexpr char kFourBandFile[] = "/vsimem/jpeg_ovr_4band.tif";
  constexpr char kSparseFile[] = "/vsimem/jpeg_ovr_sparse.tif";
  absl::Cleanup cleanup = [&]() {
    VSIUnlink(kSepFile);
    VSIUnlink(kFourBandFile);
    VSIUnlink(kSparseFile);
  };

  // 1. PLANARCONFIG_SEPARATE (INTERLEAVE=BAND).
  {
    char** options = nullptr;
    options = CSLSetNameValue(options, "COMPRESS", "JPEG");
    options = CSLSetNameValue(options, "INTERLEAVE", "BAND");
    options = CSLSetNameValue(options, "TILED", "YES");
    options = CSLSetNameValue(options, "BLOCKXSIZE", "256");
    options = CSLSetNameValue(options, "BLOCKYSIZE", "256");
    GDALDataset* ds = driver_->Create(kSepFile, 256, 256, 3, GDT_Byte, options);
    CSLDestroy(options);
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> img(256 * 256 * 3, 90);
    ASSERT_EQ(ds->RasterIO(GF_Write, 0, 0, 256, 256, img.data(), 256, 256,
                           GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    GDALClose(ds);

    GDALDataset* read_ds =
        static_cast<GDALDataset*>(GDALOpen(kSepFile, GA_ReadOnly));
    ASSERT_NE(read_ds, nullptr);
    absl::Cleanup close_read = [&]() { GDALClose(read_ds); };

    GDALRasterBand* ovr_b1 = read_ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(ovr_b1, nullptr);
    GDALDataset* ovr_ds = ovr_b1->GetDataset();
    ASSERT_NE(ovr_ds, nullptr);
    std::vector<uint8_t> buf(128 * 128 * 3, 0);
    EXPECT_EQ(ovr_ds->RasterIO(GF_Read, 0, 0, 128, 128, buf.data(), 128, 128,
                               GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_NEAR(buf[0], 90, 4);
  }

  // 2. 4-band contiguous JPEG (GDAL_JPEG_TO_RGB=NO path).
  {
    char** options = nullptr;
    options = CSLSetNameValue(options, "COMPRESS", "JPEG");
    options = CSLSetNameValue(options, "PHOTOMETRIC", "RGB");
    options = CSLSetNameValue(options, "ALPHA", "YES");
    options = CSLSetNameValue(options, "TILED", "YES");
    options = CSLSetNameValue(options, "BLOCKXSIZE", "256");
    options = CSLSetNameValue(options, "BLOCKYSIZE", "256");
    GDALDataset* ds =
        driver_->Create(kFourBandFile, 256, 256, 4, GDT_Byte, options);
    CSLDestroy(options);
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> img(256 * 256 * 4, 200);
    ASSERT_EQ(ds->RasterIO(GF_Write, 0, 0, 256, 256, img.data(), 256, 256,
                           GDT_Byte, 4, nullptr, 0, 0, 0, nullptr),
              CE_None);
    GDALClose(ds);

    GDALDataset* read_ds =
        static_cast<GDALDataset*>(GDALOpen(kFourBandFile, GA_ReadOnly));
    ASSERT_NE(read_ds, nullptr);
    absl::Cleanup close_read = [&]() { GDALClose(read_ds); };

    GDALRasterBand* ovr_b1 = read_ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(ovr_b1, nullptr);
    std::vector<uint8_t> buf(128 * 128, 0);
    EXPECT_EQ(ovr_b1->ReadBlock(0, 0, buf.data()), CE_None);
    EXPECT_NEAR(buf[0], 200, 4);
  }

  // 3. Sparse JPEG (unwritten tile -> IsBlockAvailable false -> zero-filled).
  {
    char** options = nullptr;
    options = CSLSetNameValue(options, "COMPRESS", "JPEG");
    options = CSLSetNameValue(options, "TILED", "YES");
    options = CSLSetNameValue(options, "BLOCKXSIZE", "256");
    options = CSLSetNameValue(options, "BLOCKYSIZE", "256");
    options = CSLSetNameValue(options, "SPARSE_OK", "YES");
    GDALDataset* ds =
        driver_->Create(kSparseFile, 256, 256, 1, GDT_Byte, options);
    CSLDestroy(options);
    ASSERT_NE(ds, nullptr);
    GDALClose(ds);

    GDALDataset* read_ds =
        static_cast<GDALDataset*>(GDALOpen(kSparseFile, GA_ReadOnly));
    ASSERT_NE(read_ds, nullptr);
    absl::Cleanup close_read = [&]() { GDALClose(read_ds); };

    GDALRasterBand* ovr_b1 = read_ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(ovr_b1, nullptr);
    std::vector<uint8_t> buf(128 * 128, 77);
    EXPECT_EQ(ovr_b1->ReadBlock(0, 0, buf.data()), CE_None);
    EXPECT_EQ(buf[0], 0);
  }
}

// Tests large single-strip JPEG (> 2000 rows, > 64KB compressed) triggering
// both bIsSingleStripAsSplit and the /vsisparse/ path.
TEST_F(GTiffJPEGOverviewDSTest, LargeSingleStripSplitAndVsiSparse) {
  constexpr char kLargeStripFile[] = "/vsimem/jpeg_ovr_large_strip.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kLargeStripFile); };

  constexpr int kWidth = 256;
  constexpr int kHeight = 2016;  // > 2000 and multiple of 16 for JPEG

  char** options = nullptr;
  options = CSLSetNameValue(options, "COMPRESS", "JPEG");
  options = CSLSetNameValue(options, "JPEG_QUALITY", "100");
  options = CSLSetNameValue(options, "BLOCKYSIZE", "2016");
  GDALDataset* ds =
      driver_->Create(kLargeStripFile, kWidth, kHeight, 1, GDT_Byte, options);
  CSLDestroy(options);
  ASSERT_NE(ds, nullptr);

  // High-entropy noise so JPEG compressed strip exceeds 65536 bytes.
  std::vector<uint8_t> noise(kWidth * kHeight);
  uint32_t state = 0x12345678u;
  for (size_t i = 0; i < noise.size(); ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    noise[i] = static_cast<uint8_t>(state & 0xFF);
  }
  ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                           noise.data(), kWidth, kHeight,
                                           GDT_Byte, 0, 0, nullptr),
            CE_None);
  GDALClose(ds);

  vsi_l_offset file_len = 0;
  ASSERT_NE(VSIGetMemFileBuffer(kLargeStripFile, &file_len, FALSE), nullptr);
  EXPECT_GT(file_len, 65536u);

  GDALDataset* read_ds =
      static_cast<GDALDataset*>(GDALOpen(kLargeStripFile, GA_ReadOnly));
  ASSERT_NE(read_ds, nullptr);
  absl::Cleanup close_read = [&]() { GDALClose(read_ds); };

  GDALRasterBand* ovr_b1 = read_ds->GetRasterBand(1)->GetOverview(0);
  ASSERT_NE(ovr_b1, nullptr);

  int ovr_bx = 0, ovr_by = 0;
  ovr_b1->GetBlockSize(&ovr_bx, &ovr_by);
  EXPECT_EQ(ovr_by, 1);

  std::vector<uint8_t> row_buf(ovr_bx, 0);
  EXPECT_EQ(ovr_b1->ReadBlock(0, 0, row_buf.data()), CE_None);
  EXPECT_EQ(ovr_b1->ReadBlock(0, 10, row_buf.data()), CE_None);
}

}  // namespace
}  // namespace autotest2
