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

// Unit tests for GeoPackage raster band and tile dataset implementation.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gpkg/gdalgeopackagerasterband.cpp
//   https://gdal.org/drivers/raster/gpkg.html

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpkg/ogr_geopackage.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalgeopackagerasterband_test";

class GDALGeoPackageRasterBandTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    RegisterOGRGeoPackage();
    GDALRegister_PNG();
    GDALRegister_JPEG();
    GDALRegister_WEBP();
    GDALRegister_GTiff();
    GDALRegister_MEM();
  }

  static void TearDownTestSuite() { GDALDestroyDriverManager(); }

  void SetUp() override {
    VSIMkdir(kTestDir, 0755);
    CPLSetConfigOption("CPL_TMPDIR", kTestDir);
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
  }
};

TEST_F(GDALGeoPackageRasterBandTest, Byte1BandWithColorTable) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const std::string path = std::string(kTestDir) + "/byte1_ct.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=PNG", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 1, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    // Set color table.
    GDALColorTable ct;
    GDALColorEntry entry0 = {0, 0, 0, 0};      // transparent
    GDALColorEntry entry1 = {255, 0, 0, 255};  // red
    GDALColorEntry entry2 = {0, 255, 0, 255};  // green
    ct.SetColorEntry(0, &entry0);
    ct.SetColorEntry(1, &entry1);
    ct.SetColorEntry(2, &entry2);
    EXPECT_EQ(CE_None, band->SetColorTable(&ct));

    EXPECT_NE(nullptr, band->GetColorTable());
    EXPECT_EQ(GCI_PaletteIndex, band->GetColorInterpretation());

    // Write raster data.
    std::vector<GByte> buffer(256 * 256, 1);
    // Set some transparent pixels
    buffer[0] = 0;
    buffer[1] = 2;
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 256, 256, buffer.data(),
                                      256, 256, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Re-open with BAND_COUNT=1 to verify color table retrieval.
  {
    const char* open_opts[] = {"BAND_COUNT=1", nullptr};
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                   open_opts, nullptr)));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    GDALColorTable* ct = band->GetColorTable();
    ASSERT_NE(nullptr, ct);
    EXPECT_GE(ct->GetColorEntryCount(), 3);

    std::vector<GByte> read_buf(256 * 256, 0);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 256, 256, read_buf.data(),
                                      256, 256, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(0, read_buf[0]);
    EXPECT_EQ(2, read_buf[1]);
    EXPECT_EQ(1, read_buf[2]);
  }

  // Re-open with default options (expands 1-band paletted to 4-band RGBA).
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterCount());
    std::vector<GByte> g_buf(256 * 256, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 256, 256,
                                                      g_buf.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(255, g_buf[1]);
  }
}

TEST_F(GDALGeoPackageRasterBandTest, Byte4BandPNGAndPartialBandWrite) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const std::string path = std::string(kTestDir) + "/byte4_png.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=PNG", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 4, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    // Write all 4 bands.
    for (int b = 1; b <= 4; ++b) {
      std::vector<GByte> buf(256 * 256, static_cast<GByte>(b * 50));
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, 256, 256, buf.data(), 256, 256,
                             GDT_Byte, 0, 0, nullptr));
    }
    EXPECT_EQ(CE_None, ds->FlushCache(false));

    // Partial band write: write only band 1 (exercises !bAllDirty merging).
    std::vector<GByte> buf_b1(256 * 256, 99);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 256, 256,
                                                      buf_b1.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Re-open and verify band 1 is updated while band 2 is preserved.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> read_buf(256 * 256, 0);

    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 256, 256,
                                                      read_buf.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(99, read_buf[0]);

    EXPECT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 256, 256,
                                                      read_buf.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(100, read_buf[0]);
  }
}

TEST_F(GDALGeoPackageRasterBandTest, Byte3BandJPEG) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const std::string path = std::string(kTestDir) + "/byte3_jpeg.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=JPEG", "QUALITY=80", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 3, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    for (int b = 1; b <= 3; ++b) {
      std::vector<GByte> buf(256 * 256, static_cast<GByte>(b * 60));
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, 256, 256, buf.data(), 256, 256,
                             GDT_Byte, 0, 0, nullptr));
    }
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Re-open.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> read_buf(256 * 256, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 256, 256,
                                                      read_buf.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_NEAR(60, read_buf[0], 5);
  }
}

TEST_F(GDALGeoPackageRasterBandTest, Byte4BandWEBPAndSimulWebp3Band) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const std::string path = std::string(kTestDir) + "/byte4_webp.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=WEBP", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 4, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    for (int b = 1; b <= 4; ++b) {
      std::vector<GByte> buf(256 * 256, static_cast<GByte>(b * 40));
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, 256, 256, buf.data(), 256, 256,
                             GDT_Byte, 0, 0, nullptr));
    }
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Test with GPKG_SIMUL_WEBP_3BAND=TRUE.
  {
    CPLSetThreadLocalConfigOption("GPKG_SIMUL_WEBP_3BAND", "TRUE");
    absl::Cleanup reset_opt = [] {
      CPLSetThreadLocalConfigOption("GPKG_SIMUL_WEBP_3BAND", nullptr);
    };

    const std::string path3 = std::string(kTestDir) + "/byte3_webp.gpkg";
    const char* options[] = {"TILE_FORMAT=WEBP", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path3.c_str(), 256, 256, 3, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    for (int b = 1; b <= 3; ++b) {
      std::vector<GByte> buf(256 * 256, static_cast<GByte>(b * 50));
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, 256, 256, buf.data(), 256, 256,
                             GDT_Byte, 0, 0, nullptr));
    }
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }
}

TEST_F(GDALGeoPackageRasterBandTest, Byte4BandMixedPNGJPEG) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  // 512x256 dataset (2 tiles of 256x256).
  const std::string path = std::string(kTestDir) + "/byte4_mixed.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=PNG_JPEG", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 512, 256, 4, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    // Tile 1 (x: 0..255): fully opaque (alpha=255) -> written as JPEG.
    // Tile 2 (x: 256..511): partially transparent (alpha=128) -> written as
    // PNG.
    for (int b = 1; b <= 3; ++b) {
      std::vector<GByte> buf(512 * 256, 128);
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, 512, 256, buf.data(), 512, 256,
                             GDT_Byte, 0, 0, nullptr));
    }
    std::vector<GByte> alpha(512 * 256, 255);
    for (int y = 0; y < 256; ++y) {
      for (int x = 256; x < 512; ++x) {
        alpha[y * 512 + x] = 128;
      }
    }
    EXPECT_EQ(CE_None, ds->GetRasterBand(4)->RasterIO(GF_Write, 0, 0, 512, 256,
                                                      alpha.data(), 512, 256,
                                                      GDT_Byte, 0, 0, nullptr));

    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Re-open and verify.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> read_alpha(512 * 256, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(4)->RasterIO(
                           GF_Read, 0, 0, 512, 256, read_alpha.data(), 512, 256,
                           GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(255, read_alpha[0]);
    EXPECT_EQ(128, read_alpha[256]);
  }
}

TEST_F(GDALGeoPackageRasterBandTest, PNG8QuantizationDitherAndNoDither) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  // DITHER=YES
  {
    const std::string path = std::string(kTestDir) + "/png8_dither.gpkg";
    const char* options[] = {"TILE_FORMAT=PNG8", "DITHER=YES", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 4, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    std::vector<GByte> gradient(256 * 256);
    for (int i = 0; i < 256 * 256; ++i) {
      gradient[i] = static_cast<GByte>(i % 256);
    }
    for (int b = 1; b <= 3; ++b) {
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, 256, 256, gradient.data(), 256,
                             256, GDT_Byte, 0, 0, nullptr));
    }
    std::vector<GByte> alpha(256 * 256, 255);
    EXPECT_EQ(CE_None, ds->GetRasterBand(4)->RasterIO(GF_Write, 0, 0, 256, 256,
                                                      alpha.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // DITHER=NO
  {
    const std::string path = std::string(kTestDir) + "/png8_nodither.gpkg";
    const char* options[] = {"TILE_FORMAT=PNG8", "DITHER=NO", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 4, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    std::vector<GByte> gradient(256 * 256);
    for (int i = 0; i < 256 * 256; ++i) {
      gradient[i] = static_cast<GByte>(i % 256);
    }
    for (int b = 1; b <= 3; ++b) {
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, 256, 256, gradient.data(), 256,
                             256, GDT_Byte, 0, 0, nullptr));
    }
    std::vector<GByte> alpha(256 * 256, 255);
    EXPECT_EQ(CE_None, ds->GetRasterBand(4)->RasterIO(GF_Write, 0, 0, 256, 256,
                                                      alpha.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }
}

TEST_F(GDALGeoPackageRasterBandTest, GriddedCoverageInt16UInt16Float32) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  // 1. GDT_Int16 with PNG and TIFF.
  {
    const std::string path = std::string(kTestDir) + "/int16_png.gpkg";
    const char* options[] = {"TILE_FORMAT=PNG", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 1, GDT_Int16, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    band->SetNoDataValue(-32768.0);
    band->SetNoDataValue(std::numeric_limits<double>::quiet_NaN());
    band->SetNoDataValue(-32768.0);
    EXPECT_EQ(CE_None, band->SetOffset(10.0));
    EXPECT_EQ(CE_None, band->SetScale(0.5));

    std::vector<int16_t> buf(256 * 256, -500);
    buf[0] = 1234;
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 256, 256, buf.data(), 256,
                                      256, GDT_Int16, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Verify GDT_Int16 readback.
  {
    const std::string path = std::string(kTestDir) + "/int16_png.gpkg";
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    int bSuccess = 0;
    EXPECT_DOUBLE_EQ(-32768.0, band->GetNoDataValue(&bSuccess));
    EXPECT_TRUE(bSuccess);

    std::vector<int16_t> read_buf(256 * 256, 0);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 256, 256, read_buf.data(),
                                      256, 256, GDT_Int16, 0, 0, nullptr));
    EXPECT_EQ(1234, read_buf[0]);
    EXPECT_EQ(-500, read_buf[1]);
  }

  // 2. GDT_UInt16 with TIFF.
  {
    const std::string path = std::string(kTestDir) + "/uint16_tiff.gpkg";
    const char* options[] = {"TILE_FORMAT=TIFF", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 1, GDT_UInt16, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    band->SetNoDataValue(65535.0);

    std::vector<uint16_t> buf(256 * 256, 40000);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 256, 256, buf.data(), 256,
                                      256, GDT_UInt16, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // 3. GDT_Float32 with TIFF.
  {
    const std::string path = std::string(kTestDir) + "/float32_tiff.gpkg";
    const char* options[] = {"TILE_FORMAT=TIFF", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 1, GDT_Float32, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    band->SetNoDataValue(-9999.0);

    std::vector<float> buf(256 * 256, 3.14159f);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 256, 256, buf.data(), 256,
                                      256, GDT_Float32, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Verify GDT_Float32 readback.
  {
    const std::string path = std::string(kTestDir) + "/float32_tiff.gpkg";
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    int bSuccess = 0;
    EXPECT_FLOAT_EQ(-9999.0f,
                    static_cast<float>(band->GetNoDataValue(&bSuccess)));
    EXPECT_TRUE(bSuccess);

    std::vector<float> read_buf(256 * 256, 0.0f);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 256, 256, read_buf.data(),
                                      256, 256, GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(3.14159f, read_buf[0]);
  }

  // 4. GDT_Float32 with PNG (uses 16-bit PNG quantization with per-tile
  // scale/offset).
  {
    const std::string path = std::string(kTestDir) + "/float32_png.gpkg";
    const char* options[] = {"TILE_FORMAT=PNG", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 1, GDT_Float32, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    band->SetNoDataValue(-9999.0);

    std::vector<float> buf(256 * 256, 50.0f);
    buf[0] = 100.0f;
    buf[1] = -9999.0f;  // nodata
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 256, 256, buf.data(), 256,
                                      256, GDT_Float32, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Verify GDT_Float32 with PNG readback.
  {
    const std::string path = std::string(kTestDir) + "/float32_png.gpkg";
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    std::vector<float> read_buf(256 * 256, 0.0f);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 256, 256, read_buf.data(),
                                      256, 256, GDT_Float32, 0, 0, nullptr));
    EXPECT_NEAR(100.0f, read_buf[0], 0.1f);
    EXPECT_NEAR(50.0f, read_buf[2], 0.1f);
  }
}

TEST_F(GDALGeoPackageRasterBandTest, DataCoverageStatusAndEmptyTiles) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const std::string path = std::string(kTestDir) + "/coverage_status.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=PNG", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 512, 512, 1, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    // Write only tile (0, 0).
    std::vector<GByte> buf(256 * 256, 200);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 256, 256, buf.data(), 256,
                                      256, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));

    // Coverage status on written tile (0, 0).
    int status = band->GetDataCoverageStatus(0, 0, 256, 256);
    EXPECT_TRUE(status & GDAL_DATA_COVERAGE_STATUS_DATA);

    // Coverage status on unwritten tile (256, 256).
    status = band->GetDataCoverageStatus(256, 256, 256, 256);
    EXPECT_TRUE(status & GDAL_DATA_COVERAGE_STATUS_EMPTY);

    // Coverage status on whole dataset.
    status = band->GetDataCoverageStatus(0, 0, 512, 512);
    EXPECT_TRUE(status & GDAL_DATA_COVERAGE_STATUS_DATA);
    EXPECT_TRUE(status & GDAL_DATA_COVERAGE_STATUS_EMPTY);
  }
}

TEST_F(GDALGeoPackageRasterBandTest, BandMetadataStatisticsAndOverviews) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const std::string path = std::string(kTestDir) + "/meta_stats.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=PNG", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 512, 512, 1, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    std::vector<GByte> buf(512 * 512, 150);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 512, 512, buf.data(), 512,
                                      512, GDT_Byte, 0, 0, nullptr));

    // Metadata items.
    EXPECT_EQ(CE_None, band->SetMetadataItem("CUSTOM_KEY", "CUSTOM_VAL"));
    EXPECT_STREQ("CUSTOM_VAL", band->GetMetadataItem("CUSTOM_KEY"));

    // Domain metadata.
    char** meta = band->GetMetadata("");
    EXPECT_NE(nullptr, meta);
    EXPECT_NE(nullptr, ds->GetMetadata("IMAGE_STRUCTURE"));

    // Byte band SetNoDataValue: valid vs invalid
    EXPECT_EQ(CE_None, band->SetNoDataValue(0.0));
    EXPECT_EQ(CE_Failure, band->SetNoDataValue(-1.0));
    EXPECT_EQ(CE_Failure, band->SetNoDataValue(300.0));
    EXPECT_EQ(CE_Failure, band->SetNoDataValue(1.5));

    // Statistics computation and retrieval.
    double dfMin = 0, dfMax = 0, dfMean = 0, dfStdDev = 0;
    EXPECT_EQ(CE_None, band->ComputeStatistics(false, &dfMin, &dfMax, &dfMean,
                                               &dfStdDev, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(150.0, dfMin);
    EXPECT_DOUBLE_EQ(150.0, dfMax);

    EXPECT_EQ(CE_None, band->GetStatistics(false, false, &dfMin, &dfMax,
                                           nullptr, nullptr));

    auto* gpkg_band = dynamic_cast<GDALGeoPackageRasterBand*>(band);
    if (gpkg_band != nullptr) {
      gpkg_band->InvalidateStatistics();
      gpkg_band->AddImplicitStatistics(true);
      EXPECT_TRUE(gpkg_band->HaveStatsMetadataBeenSetInThisSession());
    }

    // Build overviews.
    int anOverviewList[1] = {2};
    EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, anOverviewList, 0,
                                          nullptr, nullptr, nullptr));
    EXPECT_EQ(1, band->GetOverviewCount());
    GDALRasterBand* ovr = band->GetOverview(0);
    ASSERT_NE(nullptr, ovr);
    EXPECT_EQ(256, ovr->GetXSize());
    EXPECT_EQ(256, ovr->GetYSize());

    EXPECT_EQ(nullptr, band->GetOverview(-1));
    EXPECT_EQ(nullptr, band->GetOverview(99));

    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }
}

TEST_F(GDALGeoPackageRasterBandTest, ShiftedTileWriting) {
  // Test shifted / unaligned tile writes.
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);
  GDALDriver* gpkg_driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, gpkg_driver);

  // Create source unaligned MEM dataset.
  std::unique_ptr<GDALDataset> mem_ds(
      mem_driver->Create("", 300, 300, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mem_ds);

  GDALGeoTransform gt = {10.5, 1.0, 0.0, 50.5, 0.0, -1.0};
  mem_ds->SetGeoTransform(gt);
  std::vector<GByte> buf(300 * 300, 77);
  EXPECT_EQ(CE_None, mem_ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 300, 300, buf.data(), 300, 300,
                         GDT_Byte, 0, 0, nullptr));

  const std::string path = std::string(kTestDir) + "/shifted.gpkg";
  const char* options[] = {"TILING_SCHEME=GoogleMapsCompatible",
                           "GPKG_FORCE_TEMPDB_COMPACTION=YES", nullptr};
  std::unique_ptr<GDALDataset> gpkg_ds(
      gpkg_driver->CreateCopy(path.c_str(), mem_ds.get(), false,
                              const_cast<char**>(options), nullptr, nullptr));
  ASSERT_NE(nullptr, gpkg_ds);
  EXPECT_EQ(CE_None, gpkg_ds->FlushCache(false));
}

TEST_F(GDALGeoPackageRasterBandTest, ColorInterpretation124Bands) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  // 1-band
  {
    const std::string path = std::string(kTestDir) + "/color_interp1.gpkg";
    std::unique_ptr<GDALDataset> ds(
        driver->Create(path.c_str(), 256, 256, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_EQ(GCI_GrayIndex, band->GetColorInterpretation());
    EXPECT_EQ(CE_None, band->SetColorInterpretation(GCI_Undefined));
    EXPECT_EQ(CE_None, band->SetColorInterpretation(GCI_GrayIndex));
    EXPECT_EQ(CE_None, band->SetColorInterpretation(GCI_PaletteIndex));
    {
      WithQuietHandler quiet_handler;
      EXPECT_EQ(CE_Warning, band->SetColorInterpretation(GCI_CyanBand));
    }
  }

  // 2-band (Gray + Alpha)
  {
    const std::string path = std::string(kTestDir) + "/color_interp2.gpkg";
    std::unique_ptr<GDALDataset> ds(
        driver->Create(path.c_str(), 256, 256, 2, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
    EXPECT_EQ(GCI_GrayIndex, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_AlphaBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex));
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(2)->SetColorInterpretation(GCI_AlphaBand));
    {
      WithQuietHandler quiet_handler;
      EXPECT_EQ(CE_Warning,
                ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand));
    }
  }

  // 4-band (RGBA)
  {
    const std::string path = std::string(kTestDir) + "/color_interp4.gpkg";
    std::unique_ptr<GDALDataset> ds(
        driver->Create(path.c_str(), 256, 256, 4, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
    EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, ds->GetRasterBand(3)->GetColorInterpretation());
    EXPECT_EQ(GCI_AlphaBand, ds->GetRasterBand(4)->GetColorInterpretation());
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand));
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand));
  }
}

TEST_F(GDALGeoPackageRasterBandTest, Byte2BandGrayAlphaWriteReadAndPartial) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const std::string path = std::string(kTestDir) + "/byte2_png.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=PNG", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 2, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    std::vector<GByte> gray(256 * 256, 120);
    std::vector<GByte> alpha(256 * 256, 200);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 256, 256,
                                                      gray.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 256, 256,
                                                      alpha.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));

    // Partial band write: write only band 1.
    std::vector<GByte> gray_part(256 * 256, 210);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 256, 256, gray_part.data(), 256, 256,
                           GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Re-open with BAND_COUNT=2 and verify both bands.
  {
    const char* open_opts[] = {"BAND_COUNT=2", nullptr};
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                   open_opts, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(2, ds->GetRasterCount());
    std::vector<GByte> read_buf(256 * 256, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 256, 256,
                                                      read_buf.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(210, read_buf[0]);

    EXPECT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 256, 256,
                                                      read_buf.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    // PNG driver writes Grey or RGB, so on readback alpha defaults to 255.
    EXPECT_EQ(255, read_buf[0]);
  }

  // Re-open with BAND_COUNT=4 (expands Grey+Alpha -> RGBA, line 660).
  {
    const char* open_opts[] = {"BAND_COUNT=4", nullptr};
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                   open_opts, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterCount());
    std::vector<GByte> read_buf(256 * 256, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 256, 256,
                                                      read_buf.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(210, read_buf[0]);
    EXPECT_EQ(CE_None, ds->GetRasterBand(4)->RasterIO(GF_Read, 0, 0, 256, 256,
                                                      read_buf.data(), 256, 256,
                                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(255, read_buf[0]);
  }
}

TEST_F(GDALGeoPackageRasterBandTest, Byte1BandPalettedJPEGAndRGBMapping) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const std::string path = std::string(kTestDir) + "/byte1_pal_jpeg.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=JPEG", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 1, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    GDALColorTable ct;
    GDALColorEntry red = {255, 0, 0, 255};
    GDALColorEntry green = {0, 255, 0, 255};
    GDALColorEntry blue = {0, 0, 255, 255};
    ct.SetColorEntry(0, &red);
    ct.SetColorEntry(1, &green);
    ct.SetColorEntry(2, &blue);
    EXPECT_EQ(CE_None, band->SetColorTable(&ct));

    std::vector<GByte> buf(256 * 256, 0);            // index 0: red
    for (int i = 0; i < 256 * 128; ++i) buf[i] = 1;  // index 1: green
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 256, 256, buf.data(), 256,
                                      256, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));

#if !defined(MEMORY_SANITIZER)
    // Upstream GDAL's GDALGPKGMBTilesLikePseudoDataset::ReadTile
    // (gdalgeopackagerasterband.cpp:603,616) reads c4 = pabyTileData[i + 3 *
    // nBlockPixels] unconditionally even when nTileBandCount == 3 (JPEG) and
    // passes c4 by value to GPKGFindBestEntry(), triggering
    // -fsanitize-memory-param-retval. Read back while ds is still open with
    // color table to exercise RGB to palette mapping
    std::vector<GByte> read_buf(256 * 256, 0);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 256, 256, read_buf.data(),
                                      256, 256, GDT_Byte, 0, 0, nullptr));
#endif
  }

  // Re-open with BAND_COUNT=1.
  {
    const char* open_opts[] = {"BAND_COUNT=1", nullptr};
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                   open_opts, nullptr)));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    std::vector<GByte> read_buf(256 * 256, 0);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 256, 256, read_buf.data(),
                                      256, 256, GDT_Byte, 0, 0, nullptr));
  }
}

TEST_F(GDALGeoPackageRasterBandTest, Byte1BandPalettedPartialTile) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  // 200x200 dataset with 256x256 tile size -> partial tile with transparency
  const std::string path = std::string(kTestDir) + "/byte1_partial.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=PNG", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 200, 200, 1, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    GDALColorTable ct;
    GDALColorEntry trans = {0, 0, 0, 0};
    GDALColorEntry white = {255, 255, 255, 255};
    ct.SetColorEntry(0, &trans);
    ct.SetColorEntry(1, &white);
    EXPECT_EQ(CE_None, band->SetColorTable(&ct));

    std::vector<GByte> buf(200 * 200, 1);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 200, 200, buf.data(), 200,
                                      200, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Re-open with BAND_COUNT=1. Partial tile with CT writes 4-band RGBA PNG,
  // so band 1 reads red component (255 for white).
  {
    const char* open_opts[] = {"BAND_COUNT=1", nullptr};
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                   open_opts, nullptr)));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    std::vector<GByte> read_buf(200 * 200, 0);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 200, 200, read_buf.data(),
                                      200, 200, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(255, read_buf[0]);
  }
}

TEST_F(GDALGeoPackageRasterBandTest, GriddedCoverageUInt16PNG) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const std::string path = std::string(kTestDir) + "/uint16_png.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=PNG", "OFFSET=5.0", "SCALE=2.0",
                             nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 1, GDT_UInt16, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    band->SetNoDataValue(65535.0);
    EXPECT_EQ(CE_None, band->SetOffset(5.0));
    EXPECT_EQ(CE_None, band->SetScale(2.0));

    std::vector<uint16_t> buf(256 * 256, 1000);
    buf[0] = 5000;
    buf[1] = 65535;  // nodata
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 256, 256, buf.data(), 256,
                                      256, GDT_UInt16, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Re-open and verify.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    std::vector<uint16_t> read_buf(256 * 256, 0);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 256, 256, read_buf.data(),
                                      256, 256, GDT_UInt16, 0, 0, nullptr));
    EXPECT_EQ(5000, read_buf[0]);
    EXPECT_EQ(65535, read_buf[1]);
    EXPECT_EQ(1000, read_buf[2]);
  }
}

TEST_F(GDALGeoPackageRasterBandTest, DeleteTileOnAllZeroOrNoData) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  // 1. Byte PNG dataset (512x256 = 2 tiles).
  const std::string path = std::string(kTestDir) + "/del_tile_byte.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=PNG", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 512, 256, 1, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    // Write tile 0 and tile 1 with non-zero data.
    std::vector<GByte> buf(512 * 256, 200);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 512, 256, buf.data(), 512,
                                      256, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));

    // Overwrite tile 0 with 0 (fully transparent), triggering DeleteTile.
    std::vector<GByte> zeros(256 * 256, 0);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 256, 256, zeros.data(),
                                      256, 256, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // 2. Float32 TIFF dataset: write nodata to entire tile.
  const std::string path_f32 = std::string(kTestDir) + "/del_tile_f32.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=TIFF", nullptr};
    std::unique_ptr<GDALDataset> ds(
        driver->Create(path_f32.c_str(), 512, 256, 1, GDT_Float32,
                       const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    band->SetNoDataValue(-9999.0);

    std::vector<float> buf(512 * 256, 12.5f);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 512, 256, buf.data(), 512,
                                      256, GDT_Float32, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));

    // Overwrite tile 0 with nodata values.
    std::vector<float> nodata_buf(256 * 256, -9999.0f);
    EXPECT_EQ(CE_None,
              band->RasterIO(GF_Write, 0, 0, 256, 256, nodata_buf.data(), 256,
                             256, GDT_Float32, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }
}

TEST_F(GDALGeoPackageRasterBandTest,
       ShiftedTileBlockReadWriteAndQuadrantMerge) {
  GDALDriver* gpkg_driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, gpkg_driver);

  const std::string path = std::string(kTestDir) + "/shifted_rw.gpkg";
  const char* options[] = {"MINX=-100", "MAXY=100",
                           "GPKG_FORCE_TEMPDB_COMPACTION=YES",
                           "GPKG_HUGE_TEMPDB=YES", nullptr};
  std::unique_ptr<GDALDataset> gpkg_ds(gpkg_driver->Create(
      path.c_str(), 600, 600, 1, GDT_Byte, const_cast<char**>(options)));
  ASSERT_NE(nullptr, gpkg_ds);
  GDALGeoTransform gt = {10.0, 1.0, 0.0, -10.0, 0.0, -1.0};
  ASSERT_EQ(CE_None, gpkg_ds->SetGeoTransform(gt));

  GDALRasterBand* band = gpkg_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // Write some data across blocks
  std::vector<GByte> all_buf(600 * 600, 88);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 600, 600, all_buf.data(),
                                    600, 600, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(CE_None, gpkg_ds->FlushCache(false));

  // Test block reads (left to right reading at constant row).
  std::vector<GByte> block_buf(256 * 256, 0);
  EXPECT_EQ(CE_None, band->ReadBlock(0, 0, block_buf.data()));
  EXPECT_EQ(CE_None, band->ReadBlock(1, 0, block_buf.data()));

  // Test block write.
  std::vector<GByte> write_block(256 * 256, 44);
  EXPECT_EQ(CE_None, band->WriteBlock(0, 0, write_block.data()));

  // Test writing small sub-windows to test quadrant flags merging.
  std::vector<GByte> sub_win(20 * 20, 99);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 5, 5, 20, 20, sub_win.data(), 20,
                                    20, GDT_Byte, 0, 0, nullptr));
  std::vector<GByte> sub_win2(20 * 20, 111);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 50, 50, 20, 20, sub_win2.data(),
                                    20, 20, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(CE_None, gpkg_ds->FlushCache(false));

  // Write only a small sub-window to create missing quadrants (nPartialFlags !=
  // nFullFlags), then flush to trigger reading the existing tile from the DB
  // and copying missing quadrants.
  std::vector<GByte> sub_win3(20 * 20, 123);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 15, 15, 20, 20, sub_win3.data(),
                                    20, 20, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(CE_None, gpkg_ds->FlushCache(false));
}

TEST_F(GDALGeoPackageRasterBandTest, OverviewResamplingMethodsAndDeletion) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const std::string path = std::string(kTestDir) + "/ovr_resampling.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=PNG", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 512, 512, 1, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    std::vector<GByte> buf(512 * 512);
    for (int i = 0; i < 512 * 512; ++i) buf[i] = static_cast<GByte>(i % 256);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 512, 512, buf.data(), 512,
                                      512, GDT_Byte, 0, 0, nullptr));

    int anOverviewList[1] = {2};
    // Build with AVERAGE
    EXPECT_EQ(CE_None, ds->BuildOverviews("AVERAGE", 1, anOverviewList, 0,
                                          nullptr, nullptr, nullptr));
    // Build with BILINEAR
    EXPECT_EQ(CE_None, ds->BuildOverviews("BILINEAR", 1, anOverviewList, 0,
                                          nullptr, nullptr, nullptr));
    // Build with CUBIC
    EXPECT_EQ(CE_None, ds->BuildOverviews("CUBIC", 1, anOverviewList, 0,
                                          nullptr, nullptr, nullptr));
    // Delete/clean overviews by passing nOverviews = 0
    EXPECT_EQ(CE_None, ds->BuildOverviews("NONE", 0, nullptr, 0, nullptr,
                                          nullptr, nullptr));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Re-open and verify overviews deleted
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_EQ(0, band->GetOverviewCount());
  }
}

TEST_F(GDALGeoPackageRasterBandTest, ImplicitStatisticsComputation) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const std::string path = std::string(kTestDir) + "/implicit_stats.gpkg";
  {
    const char* options[] = {"TILE_FORMAT=TIFF", nullptr};
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 512, 256, 1, GDT_Float32, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    band->SetNoDataValue(-9999.0);

    // Tile 1: min=10, max=50
    std::vector<float> tile1(256 * 256, 30.0f);
    tile1[0] = 10.0f;
    tile1[1] = 50.0f;
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 256, 256, tile1.data(),
                                      256, 256, GDT_Float32, 0, 0, nullptr));

    // Tile 2: min=20, max=80
    std::vector<float> tile2(256 * 256, 40.0f);
    tile2[0] = 20.0f;
    tile2[1] = 80.0f;
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 256, 0, 256, 256, tile2.data(),
                                      256, 256, GDT_Float32, 0, 0, nullptr));

    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Re-open in GA_ReadOnly and check implicit statistics.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    auto* gpkg_band = dynamic_cast<GDALGeoPackageRasterBand*>(band);
    if (gpkg_band != nullptr) {
      gpkg_band->AddImplicitStatistics(true);
    }
    const char* pszMin = band->GetMetadataItem("STATISTICS_MINIMUM");
    ASSERT_NE(nullptr, pszMin);
    std::string sMin = pszMin;
    const char* pszMax = band->GetMetadataItem("STATISTICS_MAXIMUM");
    ASSERT_NE(nullptr, pszMax);
    std::string sMax = pszMax;
    EXPECT_NEAR(10.0, CPLAtof(sMin.c_str()), 1e-4);
    EXPECT_NEAR(80.0, CPLAtof(sMax.c_str()), 1e-4);
  }
}

TEST_F(GDALGeoPackageRasterBandTest, ReadNonDirtyBandWhileOtherBandDirty) {
  const std::string path = std::string(kTestDir) + "/read_non_dirty_band.gpkg";
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const char* const options[] = {"TILE_FORMAT=PNG", nullptr};
  {
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 4, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    std::vector<GByte> buf(256 * 256);
    for (int b = 1; b <= 4; ++b) {
      std::fill(buf.begin(), buf.end(), static_cast<GByte>(b * 10));
      ASSERT_EQ(CE_None, ds->GetRasterBand(b)->WriteBlock(0, 0, buf.data()));
    }
    ASSERT_EQ(CE_None, ds->FlushCache(false));

    // Dirty band 1.
    std::fill(buf.begin(), buf.end(), static_cast<GByte>(99));
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, buf.data()));

    // Read band 2 while band 1 is dirty without flushing: exercises
    // poDS->m_papoTmpDirtyCachedBlocks.
    std::vector<GByte> read_buf(256 * 256);
    ASSERT_EQ(CE_None, ds->GetRasterBand(2)->ReadBlock(0, 0, read_buf.data()));
    EXPECT_EQ(20, read_buf[0]);
  }
}

TEST_F(GDALGeoPackageRasterBandTest,
       ShiftedTileFullAndPartialQuadrantMergeAndTempDBCompaction) {
  const std::string tmp_dir = std::string(kTestDir) + "/gpkg_tmp";
  CPLSetThreadLocalConfigOption("GPKG_FORCE_TEMPDB_COMPACTION", "YES");
  CPLSetThreadLocalConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  absl::Cleanup cfg_cleanup([] {
    CPLSetThreadLocalConfigOption("GPKG_FORCE_TEMPDB_COMPACTION", nullptr);
    CPLSetThreadLocalConfigOption("CPL_TMPDIR", nullptr);
  });

  const std::string path =
      std::string(kTestDir) + "/shifted_quadrant_merge.gpkg";
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const char* const options[] = {
      "TILE_FORMAT=PNG",
      "TILING_SCHEME=GoogleCRS84Quad",
      nullptr,
  };
  {
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 600, 600, 4, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);
    const double res = (360.0 / 256.0) / 4.0;
    GDALGeoTransform gt = {-180.0 + 50.0 * res, res, 0.0,
                           180.0 - 50.0 * res,  0.0, -res};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    // Write to band 1 block (0,0) -> partial tile stored in m_hTempDB.
    std::vector<GByte> buf(256 * 256, 100);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, buf.data()));

    // Read band 2 block (0,0) -> read from m_hTempDB.
    std::vector<GByte> read_buf(256 * 256, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(2)->ReadBlock(0, 0, read_buf.data()));
    EXPECT_EQ(CE_None, ds->GetRasterBand(2)->ReadBlock(1, 0, read_buf.data()));

    // Write all 4 quadrant blocks (0,0), (1,0), (0,1), (1,1) across all 4
    // bands -> completes 0xFFFF flags for interior tile (1,1).
    for (int b = 1; b <= 4; ++b) {
      std::fill(buf.begin(), buf.end(), static_cast<GByte>(b * 20));
      for (int by = 0; by <= 1; ++by) {
        for (int bx = 0; bx <= 1; ++bx) {
          ASSERT_EQ(CE_None,
                    ds->GetRasterBand(b)->WriteBlock(bx, by, buf.data()));
        }
      }
    }
    // Flush to main DB.
    EXPECT_EQ(CE_None, ds->FlushCache(false));

    // Now write only block (0,0) on band 1 and flush: exercises reading
    // existing tile from main DB and merging quadrants in
    // FlushRemainingShiftedTiles.
    std::fill(buf.begin(), buf.end(), static_cast<GByte>(255));
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, buf.data()));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Create a narrow dataset with small blocksize to trigger
  // DoPartialFlushOfPartialTilesIfNecessary.
  const std::string narrow_path =
      std::string(kTestDir) + "/shifted_narrow_partial_flush.gpkg";
  const char* const narrow_options[] = {
      "TILE_FORMAT=PNG",
      "BLOCKSIZE=64",
      nullptr,
  };
  {
    std::unique_ptr<GDALDataset> ds(
        driver->Create(narrow_path.c_str(), 128, 1024, 4, GDT_Byte,
                       const_cast<char**>(narrow_options)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
    std::vector<GByte> block_buf(64 * 64, 42);
    for (int b = 1; b <= 4; ++b) {
      ASSERT_EQ(CE_None,
                ds->GetRasterBand(b)->WriteBlock(0, 0, block_buf.data()));
    }
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

#if !defined(MEMORY_SANITIZER)
  // third_party/png's png_setup_up_row triggers MSan during WriteTileInternal
  // when writing shifted PNG tiles from partial buffer chunks.
  // Reopen with open options MINX/MINY/MAXX/MAXY to trigger shifted pixel
  // offsets.
  const char* const open_opts[] = {
      "MINX=10", "MINY=-1014", "MAXX=20", "MAXY=-10", nullptr,
  };
  {
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(narrow_path.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE,
                   nullptr, open_opts, nullptr)));
    if (ds != nullptr) {
      GDALRasterBlock* poBlock =
          ds->GetRasterBand(1)->GetLockedBlockRef(0, 0, TRUE);
      if (poBlock != nullptr) {
        poBlock->MarkDirty();
        poBlock->DropLock();
      }
      std::vector<GByte> block_buf(64 * 64, 55);
      for (int by = 0; by < 15; ++by) {
        for (int b = 1; b <= 4; ++b) {
          (void)ds->GetRasterBand(b)->WriteBlock(0, by, block_buf.data());
        }
      }
      EXPECT_EQ(CE_None, ds->FlushCache(false));
    }
  }
#endif
}

TEST_F(GDALGeoPackageRasterBandTest, Shifted1BandPalettedSubWindow) {
  const std::string path =
      std::string(kTestDir) + "/shifted_paletted_subwindow.gpkg";
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const char* const options[] = {
      "TILE_FORMAT=PNG",
      "TILING_SCHEME=GoogleCRS84Quad",
      nullptr,
  };
  {
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 400, 400, 1, GDT_Byte, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);
    const double res = (360.0 / 256.0) / 4.0;
    GDALGeoTransform gt = {-180.0 + 50.0 * res, res, 0.0,
                           180.0 - 50.0 * res,  0.0, -res};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    GDALColorTable ct;
    GDALColorEntry entry0 = {0, 0, 0, 0};
    GDALColorEntry entry1 = {255, 0, 0, 255};
    ct.SetColorEntry(0, &entry0);
    ct.SetColorEntry(1, &entry1);
    ASSERT_EQ(CE_None, band->SetColorTable(&ct));

    std::vector<GByte> buf(256 * 256, 1);
    EXPECT_EQ(CE_None, band->WriteBlock(0, 0, buf.data()));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Also create a 1-band WEBP dataset with color table containing a transparent
  // entry.
  const std::string webp_path =
      std::string(kTestDir) + "/shifted_paletted_webp.gpkg";
  const char* const webp_options[] = {
      "TILE_FORMAT=WEBP",
      "BLOCKSIZE=256",
      nullptr,
  };
  {
    std::unique_ptr<GDALDataset> ds(
        driver->Create(webp_path.c_str(), 256, 256, 1, GDT_Byte,
                       const_cast<char**>(webp_options)));
    if (ds != nullptr) {
      GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
      ds->SetGeoTransform(gt);
      GDALRasterBand* band = ds->GetRasterBand(1);
      if (band != nullptr) {
        GDALColorTable ct;
        GDALColorEntry entry0 = {0, 0, 0, 0};
        GDALColorEntry entry1 = {255, 0, 0, 255};
        ct.SetColorEntry(0, &entry0);
        ct.SetColorEntry(1, &entry1);
        band->SetColorTable(&ct);
        std::vector<GByte> buf(256 * 256, 1);
        EXPECT_EQ(CE_None, band->WriteBlock(0, 0, buf.data()));
        ds->FlushCache(false);
      }
    }
  }
}

TEST_F(GDALGeoPackageRasterBandTest,
       GriddedCoverageUInt16ScaleOffsetAndTransparentDelete) {
  const std::string path =
      std::string(kTestDir) + "/gridded_uint16_scale_offset.gpkg";
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  const char* const options[] = {
      "TILE_FORMAT=PNG",
      "BLOCKSIZE=256",
      "GRID_CELL_ENCODING=grid-value-is-area",
      nullptr,
  };
  {
    std::unique_ptr<GDALDataset> ds(driver->Create(
        path.c_str(), 256, 256, 1, GDT_UInt16, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    band->SetNoDataValue(65535.0);

    auto* gpkg_ds = dynamic_cast<GDALGeoPackageDataset*>(ds.get());
    if (gpkg_ds != nullptr) {
      gpkg_ds->SetGlobalOffsetScale(10.0, 2.0);
    }

    std::vector<GUInt16> data(256 * 256, 100);
    data[0] = 10;
    data[1] = 200;
    data[2] = 65535;  // nodata
    EXPECT_EQ(CE_None, band->WriteBlock(0, 0, data.data()));
    EXPECT_EQ(CE_None, ds->FlushCache(false));

    // Overwrite tile with all nodata -> triggers DeleteTile /
    // DeleteFromGriddedTileAncillary.
    std::fill(data.begin(), data.end(), 65535);
    EXPECT_EQ(CE_None, band->WriteBlock(0, 0, data.data()));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  // Test setting differing nodata on multi-band dataset -> error.
  const std::string multi_nodata_path =
      std::string(kTestDir) + "/multi_nodata.gpkg";
  {
    std::unique_ptr<GDALDataset> ds(driver->Create(
        multi_nodata_path.c_str(), 256, 256, 2, GDT_Byte, nullptr));
    if (ds != nullptr) {
      GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
      ds->SetGeoTransform(gt);
      ds->GetRasterBand(1)->SetNoDataValue(10.0);
      EXPECT_EQ(CE_Failure, ds->GetRasterBand(2)->SetNoDataValue(20.0));
    }
  }
}

// TODO: b/335317901 - Cover remaining tile codec failure and partial overview
// branches in gdalgeopackagerasterband.cpp.

}  // namespace
}  // namespace autotest2
