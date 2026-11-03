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
// Tests implicit JPEG overview dataset support for RGB/YCbCr, separate planar,
// 4-band, sparse, and large single-strip files (gtiffjpegoverviewds.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gtiffjpegoverviewds.cpp

#include <cstdint>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class GTiffJPEGOverviewDSTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_JPEG();
    CPLErrorReset();
    driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, driver_);
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
    {
      CPLStringList options;
      options.AddNameValue("COMPRESS", "JPEG");
      options.AddNameValue("PHOTOMETRIC", "YCBCR");
      options.AddNameValue("TILED", "YES");
      options.AddNameValue("BLOCKXSIZE", "256");
      options.AddNameValue("BLOCKYSIZE", "256");
      GDALDatasetUniquePtr ds(driver_->Create(kYcbcrFile, kWidth, kHeight, 3,
                                              GDT_Byte, options.List()));
      ASSERT_NE(nullptr, ds);

      std::vector<uint8_t> img(kWidth * kHeight * 3, 128);
      ASSERT_EQ(CE_None, ds->RasterIO(GF_Write, 0, 0, kWidth, kHeight,
                                      img.data(), kWidth, kHeight, GDT_Byte, 3,
                                      nullptr, 0, 0, 0, nullptr));
    }

    GDALDatasetUniquePtr read_ds(
        GDALDataset::Open(kYcbcrFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, read_ds);

    GDALRasterBand* b1 = read_ds->GetRasterBand(1);
    EXPECT_EQ(0, b1->GetOverviewCount());  // Hidden outside IRasterIO
    GDALRasterBand* ovr_b1 = b1->GetOverview(0);
    ASSERT_NE(nullptr, ovr_b1);
    EXPECT_EQ(GCI_RedBand, ovr_b1->GetColorInterpretation());

    GDALDataset* ovr_ds = ovr_b1->GetDataset();
    ASSERT_NE(nullptr, ovr_ds);
    EXPECT_STREQ("YCbCr JPEG",
                 ovr_ds->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"));

    const int ovr_w = ovr_ds->GetRasterXSize();
    const int ovr_h = ovr_ds->GetRasterYSize();
    // Multi-band IRasterIO -> BlockBasedRasterIO, reads all 4 blocks including
    // partial right/bottom edge tiles.
    std::vector<uint8_t> ovr_rgb(ovr_w * ovr_h * 3, 0);
    EXPECT_EQ(CE_None, ovr_ds->RasterIO(GF_Read, 0, 0, ovr_w, ovr_h,
                                        ovr_rgb.data(), ovr_w, ovr_h, GDT_Byte,
                                        3, nullptr, 0, 0, 0, nullptr));
    // Uniform 128 source survives lossy JPEG within a small tolerance.
    EXPECT_NEAR(128, ovr_rgb[0], 4);

    // Single-band IRasterIO -> GDALDataset::IRasterIO.
    std::vector<uint8_t> ovr_single(ovr_w * ovr_h, 0);
    int band_map[1] = {1};
    EXPECT_EQ(
        CE_None,
        ovr_ds->RasterIO(GF_Read, 0, 0, ovr_w, ovr_h, ovr_single.data(), ovr_w,
                         ovr_h, GDT_Byte, 1, band_map, 0, 0, 0, nullptr));
    EXPECT_NEAR(128, ovr_single[0], 4);
  }

  // 2. Stripped RGB 256x256 (bAddAdobe == true, full width block).
  {
    {
      CPLStringList options;
      options.AddNameValue("COMPRESS", "JPEG");
      options.AddNameValue("PHOTOMETRIC", "RGB");
      options.AddNameValue("BLOCKYSIZE", "64");
      GDALDatasetUniquePtr ds(
          driver_->Create(kRgbFile, 256, 256, 3, GDT_Byte, options.List()));
      ASSERT_NE(nullptr, ds);

      std::vector<uint8_t> img(256 * 256 * 3, 100);
      ASSERT_EQ(CE_None,
                ds->RasterIO(GF_Write, 0, 0, 256, 256, img.data(), 256, 256,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    }

    GDALDatasetUniquePtr read_ds(
        GDALDataset::Open(kRgbFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, read_ds);

    GDALRasterBand* ovr_b1 = read_ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr_b1);
    GDALDataset* ovr_ds = ovr_b1->GetDataset();
    ASSERT_NE(nullptr, ovr_ds);
    EXPECT_STREQ("JPEG",
                 ovr_ds->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"));

    std::vector<uint8_t> buf(128 * 128, 0);
    EXPECT_EQ(CE_None, ovr_b1->RasterIO(GF_Read, 0, 0, 128, 128, buf.data(),
                                        128, 128, GDT_Byte, 0, 0, nullptr));
    EXPECT_NEAR(100, buf[0], 4);
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
    {
      CPLStringList options;
      options.AddNameValue("COMPRESS", "JPEG");
      options.AddNameValue("INTERLEAVE", "BAND");
      options.AddNameValue("TILED", "YES");
      options.AddNameValue("BLOCKXSIZE", "256");
      options.AddNameValue("BLOCKYSIZE", "256");
      GDALDatasetUniquePtr ds(
          driver_->Create(kSepFile, 256, 256, 3, GDT_Byte, options.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<uint8_t> img(256 * 256 * 3, 90);
      ASSERT_EQ(CE_None,
                ds->RasterIO(GF_Write, 0, 0, 256, 256, img.data(), 256, 256,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    }

    GDALDatasetUniquePtr read_ds(
        GDALDataset::Open(kSepFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, read_ds);

    GDALRasterBand* ovr_b1 = read_ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr_b1);
    GDALDataset* ovr_ds = ovr_b1->GetDataset();
    ASSERT_NE(nullptr, ovr_ds);
    std::vector<uint8_t> buf(128 * 128 * 3, 0);
    EXPECT_EQ(CE_None,
              ovr_ds->RasterIO(GF_Read, 0, 0, 128, 128, buf.data(), 128, 128,
                               GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_NEAR(90, buf[0], 4);
  }

  // 2. 4-band contiguous JPEG (GDAL_JPEG_TO_RGB=NO path).
  {
    {
      CPLStringList options;
      options.AddNameValue("COMPRESS", "JPEG");
      options.AddNameValue("PHOTOMETRIC", "RGB");
      options.AddNameValue("ALPHA", "YES");
      options.AddNameValue("TILED", "YES");
      options.AddNameValue("BLOCKXSIZE", "256");
      options.AddNameValue("BLOCKYSIZE", "256");
      GDALDatasetUniquePtr ds(driver_->Create(kFourBandFile, 256, 256, 4,
                                              GDT_Byte, options.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<uint8_t> img(256 * 256 * 4, 200);
      ASSERT_EQ(CE_None,
                ds->RasterIO(GF_Write, 0, 0, 256, 256, img.data(), 256, 256,
                             GDT_Byte, 4, nullptr, 0, 0, 0, nullptr));
    }

    GDALDatasetUniquePtr read_ds(
        GDALDataset::Open(kFourBandFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, read_ds);

    GDALRasterBand* ovr_b1 = read_ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr_b1);
    std::vector<uint8_t> buf(128 * 128, 0);
    EXPECT_EQ(CE_None, ovr_b1->ReadBlock(0, 0, buf.data()));
    EXPECT_NEAR(200, buf[0], 4);
  }

  // 3. Sparse JPEG (unwritten tile -> IsBlockAvailable false -> zero-filled).
  {
    {
      CPLStringList options;
      options.AddNameValue("COMPRESS", "JPEG");
      options.AddNameValue("TILED", "YES");
      options.AddNameValue("BLOCKXSIZE", "256");
      options.AddNameValue("BLOCKYSIZE", "256");
      options.AddNameValue("SPARSE_OK", "YES");
      GDALDatasetUniquePtr ds(
          driver_->Create(kSparseFile, 256, 256, 1, GDT_Byte, options.List()));
      ASSERT_NE(nullptr, ds);
    }

    GDALDatasetUniquePtr read_ds(
        GDALDataset::Open(kSparseFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, read_ds);

    GDALRasterBand* ovr_b1 = read_ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr_b1);
    std::vector<uint8_t> buf(128 * 128, 77);
    EXPECT_EQ(CE_None, ovr_b1->ReadBlock(0, 0, buf.data()));
    EXPECT_EQ(0, buf[0]);
  }
}

// Tests large single-strip JPEG (> 2000 rows, > 64KB compressed) triggering
// both bIsSingleStripAsSplit and the /vsisparse/ path.
TEST_F(GTiffJPEGOverviewDSTest, LargeSingleStripSplitAndVsiSparse) {
  constexpr char kLargeStripFile[] = "/vsimem/jpeg_ovr_large_strip.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kLargeStripFile); };

  constexpr int kWidth = 256;
  constexpr int kHeight = 2016;  // > 2000 and multiple of 16 for JPEG

  {
    CPLStringList options;
    options.AddNameValue("COMPRESS", "JPEG");
    options.AddNameValue("JPEG_QUALITY", "100");
    options.AddNameValue("BLOCKYSIZE", "2016");
    GDALDatasetUniquePtr ds(driver_->Create(kLargeStripFile, kWidth, kHeight, 1,
                                            GDT_Byte, options.List()));
    ASSERT_NE(nullptr, ds);

    // High-entropy noise so JPEG compressed strip exceeds 65536 bytes.
    std::vector<uint8_t> noise(kWidth * kHeight);
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < noise.size(); ++i) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      noise[i] = static_cast<uint8_t>(state & 0xFF);
    }
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, noise.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0, nullptr));
  }

  vsi_l_offset file_len = 0;
  ASSERT_NE(nullptr, VSIGetMemFileBuffer(kLargeStripFile, &file_len, FALSE));
  EXPECT_GT(file_len, 65536u);

  GDALDatasetUniquePtr read_ds(
      GDALDataset::Open(kLargeStripFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, read_ds);

  GDALRasterBand* ovr_b1 = read_ds->GetRasterBand(1)->GetOverview(0);
  ASSERT_NE(nullptr, ovr_b1);

  int ovr_bx = 0;
  int ovr_by = 0;
  ovr_b1->GetBlockSize(&ovr_bx, &ovr_by);
  EXPECT_EQ(1, ovr_by);

  std::vector<uint8_t> row_buf(ovr_bx, 0);
  EXPECT_EQ(CE_None, ovr_b1->ReadBlock(0, 0, row_buf.data()));
  EXPECT_EQ(CE_None, ovr_b1->ReadBlock(0, 10, row_buf.data()));
}

// TODO: b/335317901 - Cover remaining defensive Open()/error branches in
// gtiffjpegoverviewds.cpp (~92.3% coverage today).
}  // namespace
}  // namespace autotest2
