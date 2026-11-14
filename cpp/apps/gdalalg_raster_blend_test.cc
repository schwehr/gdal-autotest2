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
// Tests for GDALRasterBlendAlgorithm (apps/gdalalg_raster_blend.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_blend.cpp

#include "third_party/gdal/apps/gdalalg_raster_blend.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_blend_test";

// Fill RGB/RGBA pixels with diverse colors hitting all 6 hue sectors in
// rgb_to_hs() / hsv_to_rgb() as well as grayscale and alpha edge cases.
std::unique_ptr<GDALDataset> CreateColorTiff(const std::string& path, int width,
                                             int height, int bands,
                                             GDALDataType dt = GDT_Byte,
                                             bool with_srs_and_gt = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), width, height, bands, dt, nullptr));
  if (!ds) return nullptr;

  if (with_srs_and_gt) {
    GDALGeoTransform gt(10.0, 2.0, 0.0, 20.0, 0.0, -2.0);
    EXPECT_EQ(CE_None, ds->SetGeoTransform(gt));
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.SetWellKnownGeogCS("WGS84"));
    EXPECT_EQ(CE_None, ds->SetSpatialRef(&srs));
  }

  if (dt == GDT_Byte && bands > 0) {
    const int n = width * height;
    std::vector<uint8_t> buf(n * bands);
    for (int i = 0; i < n; ++i) {
      const int sector = i % 9;
      uint8_t r = 0, g = 0, b = 0, a = 255;
      switch (sector) {
        case 0:
          r = 240;
          g = 40;
          b = 20;
          a = 0;
          break;  // max=r, g>=b, a=0
        case 1:
          r = 240;
          g = 200;
          b = 20;
          a = 128;
          break;  // max=r, g>=b
        case 2:
          r = 40;
          g = 240;
          b = 20;
          a = 255;
          break;  // max=g
        case 3:
          r = 20;
          g = 240;
          b = 200;
          a = 200;
          break;  // max=g
        case 4:
          r = 20;
          g = 40;
          b = 240;
          a = 64;
          break;  // max=b
        case 5:
          r = 200;
          g = 20;
          b = 240;
          a = 255;
          break;  // max=b
        case 6:
          r = 240;
          g = 20;
          b = 180;
          a = 180;
          break;  // max=r, g<b
        case 7:
          r = 128;
          g = 128;
          b = 128;
          a = 255;
          break;  // gray
        default:
          r = 0;
          g = 0;
          b = 0;
          a = 0;
          break;  // zero
      }
      if (bands == 1) {
        buf[i] = static_cast<uint8_t>((i * 29) % 256);
      } else if (bands == 2) {
        buf[i] = r;
        buf[n + i] = a;
      } else if (bands >= 3) {
        buf[i] = r;
        buf[n + i] = g;
        buf[2 * n + i] = b;
        if (bands >= 4) buf[3 * n + i] = a;
        for (int extra = 4; extra < bands; ++extra) {
          buf[extra * n + i] = 100;
        }
      }
    }
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Write, 0, 0, width, height, buf.data(), width,
                           height, GDT_Byte, bands, nullptr, 0, 0, 0, nullptr));
  }
  return ds;
}

std::unique_ptr<GDALDataset> CreateOverlayTiff(const std::string& path,
                                               int width, int height, int bands,
                                               GDALDataType dt = GDT_Byte) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), width, height, bands, dt, nullptr));
  if (!ds) return nullptr;

  if (dt == GDT_Byte && bands > 0) {
    const int n = width * height;
    std::vector<uint8_t> buf(n * bands);
    for (int i = 0; i < n; ++i) {
      for (int b = 0; b < bands; ++b) {
        if ((bands == 2 && b == 1) || (bands == 4 && b == 3)) {
          // Alpha channel with 0, partial, and 255 values.
          buf[b * n + i] = (i % 5 == 0) ? 0 : (i % 5 == 1 ? 255 : 120);
        } else {
          buf[b * n + i] = static_cast<uint8_t>((50 + i * 17 + b * 40) % 256);
        }
      }
    }
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Write, 0, 0, width, height, buf.data(), width,
                           height, GDT_Byte, bands, nullptr, 0, 0, 0, nullptr));
  }
  return ds;
}

class GdalalgRasterBlendTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterBlendTest, StandaloneProperties) {
  GDALRasterBlendAlgorithmStandalone alg;
  EXPECT_EQ("blend", alg.GetName());
  EXPECT_EQ("Blend/compose two raster datasets", alg.GetDescription());
}

TEST_F(GdalalgRasterBlendTest, ValidationErrors) {
  const std::string rgb_tif = std::string(kTestDir) + "/val_rgb.tif";
  const std::string gray_tif = std::string(kTestDir) + "/val_gray.tif";
  const std::string bands5_tif = std::string(kTestDir) + "/val_5band.tif";
  const std::string u16_tif = std::string(kTestDir) + "/val_u16.tif";
  const std::string diff_dim_tif = std::string(kTestDir) + "/val_diff.tif";

  ASSERT_NE(nullptr, CreateColorTiff(rgb_tif, 8, 4, 3));
  ASSERT_NE(nullptr, CreateColorTiff(gray_tif, 8, 4, 1));
  ASSERT_NE(nullptr, CreateColorTiff(bands5_tif, 8, 4, 5));
  ASSERT_NE(nullptr, CreateColorTiff(u16_tif, 8, 4, 3, GDT_UInt16));
  ASSERT_NE(nullptr, CreateOverlayTiff(diff_dim_tif, 6, 4, 3));

  // 1. Input with 5 bands rejected.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterBlendAlgorithmStandalone alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"-f", "MEM", bands5_tif, rgb_tif, ""}));
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Non-Byte input rejected.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterBlendAlgorithmStandalone alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"-f", "MEM", u16_tif, rgb_tif, ""}));
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 3. Overlay with 5 bands or UInt16 rejected.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterBlendAlgorithmStandalone alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"-f", "MEM", rgb_tif, bands5_tif, ""}));
    CPLErrorReset();
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterBlendAlgorithmStandalone alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"-f", "MEM", rgb_tif, u16_tif, ""}));
    CPLErrorReset();
  }

  // 4. Dimension mismatch rejected.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterBlendAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"-f", "MEM", rgb_tif, diff_dim_tif, ""}));
    CPLErrorReset();
  }

  // 5. hsv-value with 1-band input rejected.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterBlendAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--operator", "hsv-value", "-f", "MEM", gray_tif, gray_tif, ""}));
    CPLErrorReset();
  }

  // 6. Opacity out of range rejected.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterBlendAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--opacity", "101", "-f", "MEM", rgb_tif, rgb_tif, ""}));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterBlendTest, HsvValue3BandAnd4BandWith1BandOverlay) {
  // Width 24 exercises both SSE2 8-pixel loop and scalar tail, plus
  // interleaved and planar pixel spacing.
  const std::string rgb_tif = std::string(kTestDir) + "/hsv_rgb.tif";
  const std::string rgba_tif = std::string(kTestDir) + "/hsv_rgba.tif";
  const std::string gray_tif = std::string(kTestDir) + "/hsv_gray.tif";
  const std::string out_tif = std::string(kTestDir) + "/hsv_out.tif";

  ASSERT_NE(nullptr, CreateColorTiff(rgb_tif, 24, 4, 3, GDT_Byte, true));
  ASSERT_NE(nullptr, CreateColorTiff(rgba_tif, 24, 4, 4, GDT_Byte, true));
  ASSERT_NE(nullptr, CreateOverlayTiff(gray_tif, 24, 4, 1));

  // 1. Pipeline streaming for 3-band RGB + 1-band gray at opacity 100.
  {
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", rgb_tif, "!", "blend", "--operator", "hsv-value", gray_tif}));
    ASSERT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(24, ds->GetRasterXSize());
    EXPECT_EQ(4, ds->GetRasterYSize());
    EXPECT_EQ(3, ds->GetRasterCount());

    // GeoTransform and SpatialRef forwarded from color dataset.
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(10.0, gt[0]);
    EXPECT_NE(nullptr, ds->GetSpatialRef());

    // Color interpretation.
    EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, ds->GetRasterBand(3)->GetColorInterpretation());

    // Dataset-level planar read (nPixelSpace == 1 -> patch_value_line SSE2).
    std::vector<uint8_t> planar(24 * 4 * 3, 0);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 24, 4, planar.data(), 24, 4, GDT_Byte,
                           3, nullptr, 1, 24, 24 * 4, nullptr));

    // Cache hit with identical window and floating point extra arg.
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.bFloatingPointWindowValidity = TRUE;
    extra.dfXOff = 0.0;
    extra.dfYOff = 0.0;
    extra.dfXSize = 24.0;
    extra.dfYSize = 4.0;
    std::vector<uint8_t> interleaved(24 * 4 * 3, 0);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 24, 4, interleaved.data(), 24, 4,
                           GDT_Byte, 3, nullptr, 3, 24 * 3, 1, &extra));
    // Repeat identical floating-point window for cache hit branch.
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 24, 4, interleaved.data(), 24, 4,
                           GDT_Byte, 3, nullptr, 3, 24 * 3, 1, &extra));
    EXPECT_EQ(planar[0], interleaved[0]);
    EXPECT_EQ(planar[24 * 4], interleaved[1]);
    EXPECT_EQ(planar[2 * 24 * 4], interleaved[2]);

    // Band-level reads (nPixelSpace == 1 and nPixelSpace == 2) for bands 1..3.
    for (int b = 1; b <= 3; ++b) {
      std::vector<uint8_t> band_planar(24 * 4, 0);
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Read, 0, 0, 24, 4, band_planar.data(), 24, 4,
                             GDT_Byte, 1, 24, nullptr));
      std::vector<uint8_t> band_strided(24 * 4 * 2, 0);
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Read, 0, 0, 24, 4, band_strided.data(), 24, 4,
                             GDT_Byte, 2, 24 * 2, nullptr));
      EXPECT_EQ(band_planar[5], band_strided[10]);
    }

    // IReadBlock coverage.
    int bx = 0, by = 0;
    ds->GetRasterBand(1)->GetBlockSize(&bx, &by);
    std::vector<uint8_t> block_buf(static_cast<size_t>(bx) * by, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, block_buf.data()));
  }

  // 2. 4-band RGBA + 1-band gray in hsv-value (including band 4 alpha copy).
  {
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", rgba_tif, "!", "blend", "--operator", "hsv-value", gray_tif}));
    ASSERT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterCount());
    EXPECT_EQ(GCI_AlphaBand, ds->GetRasterBand(4)->GetColorInterpretation());

    std::vector<uint8_t> rgba_planar(24 * 4 * 4, 0);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 24, 4, rgba_planar.data(), 24, 4,
                           GDT_Byte, 4, nullptr, 1, 24, 24 * 4, nullptr));
    std::vector<uint8_t> alpha_band(24 * 4, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(4)->RasterIO(
                           GF_Read, 0, 0, 24, 4, alpha_band.data(), 24, 4,
                           GDT_Byte, 1, 24, nullptr));
    EXPECT_EQ(rgba_planar[3 * 24 * 4], alpha_band[0]);
  }

  // 3. Standalone write to GTiff.
  {
    GDALRasterBlendAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--operator", "hsv-value", rgb_tif,
                                       gray_tif, out_tif, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(3, out_ds->GetRasterCount());
  }
}

TEST_F(GdalalgRasterBlendTest, HsvValuePartialOpacityAndMultiBandOverlay) {
  const std::string rgba_tif = std::string(kTestDir) + "/hsv_m_rgba.tif";
  const std::string gray_ov_tif = std::string(kTestDir) + "/hsv_m_ov1.tif";
  const std::string rgb_ov_tif = std::string(kTestDir) + "/hsv_m_ov3.tif";
  const std::string rgba_ov_tif = std::string(kTestDir) + "/hsv_m_ov4.tif";

  ASSERT_NE(nullptr, CreateColorTiff(rgba_tif, 24, 4, 4));
  ASSERT_NE(nullptr, CreateOverlayTiff(gray_ov_tif, 24, 4, 1));
  ASSERT_NE(nullptr, CreateOverlayTiff(rgb_ov_tif, 24, 4, 3));
  ASSERT_NE(nullptr, CreateOverlayTiff(rgba_ov_tif, 24, 4, 4));

  // Partial opacity with 1-band overlay hits general HSV_VALUE gray path.
  {
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", rgba_tif, "!", "blend", "--operator", "hsv-value", "--opacity",
         "50", gray_ov_tif}));
    ASSERT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    for (int b = 1; b <= 3; ++b) {
      std::vector<uint8_t> buf(24 * 4, 0);
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Read, 0, 0, 24, 4, buf.data(), 24, 4, GDT_Byte,
                             1, 24, nullptr));
    }
  }

  // Partial opacity with 3-band overlay exercises general HSV_VALUE band path.
  {
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", rgba_tif, "!", "blend", "--operator", "hsv-value", "--opacity",
         "50", rgb_ov_tif}));
    ASSERT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    for (int b = 1; b <= 4; ++b) {
      std::vector<uint8_t> buf(24 * 4, 0);
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Read, 0, 0, 24, 4, buf.data(), 24, 4, GDT_Byte,
                             1, 24, nullptr));
    }
  }

  // 4-band overlay with alpha channel in hsv-value.
  {
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", rgba_tif, "!", "blend", "--operator", "hsv-value", "--opacity",
         "75", rgba_ov_tif}));
    ASSERT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> buf(24 * 4 * 4, 0);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 24, 4, buf.data(), 24, 4, GDT_Byte, 4,
                           nullptr, 1, 24, 24 * 4, nullptr));
  }
}

TEST_F(GdalalgRasterBlendTest, SrcOverCombinations) {
  const std::string c1 = std::string(kTestDir) + "/c1.tif";
  const std::string c2 = std::string(kTestDir) + "/c2.tif";
  const std::string c3 = std::string(kTestDir) + "/c3.tif";
  const std::string c4 = std::string(kTestDir) + "/c4.tif";
  const std::string o1 = std::string(kTestDir) + "/o1.tif";
  const std::string o2 = std::string(kTestDir) + "/o2.tif";
  const std::string o3 = std::string(kTestDir) + "/o3.tif";
  const std::string o4 = std::string(kTestDir) + "/o4.tif";

  ASSERT_NE(nullptr, CreateColorTiff(c1, 24, 4, 1));
  ASSERT_NE(nullptr, CreateColorTiff(c2, 24, 4, 2));
  ASSERT_NE(nullptr, CreateColorTiff(c3, 24, 4, 3));
  ASSERT_NE(nullptr, CreateColorTiff(c4, 24, 4, 4));
  ASSERT_NE(nullptr, CreateOverlayTiff(o1, 24, 4, 1));
  ASSERT_NE(nullptr, CreateOverlayTiff(o2, 24, 4, 2));
  ASSERT_NE(nullptr, CreateOverlayTiff(o3, 24, 4, 3));
  ASSERT_NE(nullptr, CreateOverlayTiff(o4, 24, 4, 4));

  // 1. 4-band RGBA + 4-band RGBA: SSE2 (nPixelSpace==1) and Generic
  // (nPixelSpace>1). Upstream bug at apps/gdalalg_raster_blend.cpp:951-985:
  // clang defines __GNUC__, so #if !(defined(HAVE_SSE2) && defined(__GNUC__))
  // strips out the 'else' before BlendSrcOverRGBA_Generic<false>; for widths
  // not divisible by 16 (>= 16), SSE2 returns i > 0 and advances nSrcIdx += i,
  // and Generic<false> indexes pabyOverlayA[i] on top of nSrcIdx (ASAN
  // heap-buffer-overflow). Use width 16 (multiple of 16) here so i == N == 16.
  {
    const std::string c4_16 = std::string(kTestDir) + "/c4_16.tif";
    const std::string o4_16 = std::string(kTestDir) + "/o4_16.tif";
    ASSERT_NE(nullptr, CreateColorTiff(c4_16, 16, 4, 4));
    ASSERT_NE(nullptr, CreateOverlayTiff(o4_16, 16, 4, 4));

    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments({"read", c4_16, "!", "blend",
                                                "--operator", "src-over",
                                                "--opacity", "60", o4_16}));
    ASSERT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);

    std::vector<uint8_t> planar(16 * 4 * 4, 0);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 16, 4, planar.data(), 16, 4, GDT_Byte,
                           4, nullptr, 1, 16, 16 * 4, nullptr));

    std::vector<uint8_t> interleaved(16 * 4 * 4, 0);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 16, 4, interleaved.data(), 16, 4,
                           GDT_Byte, 4, nullptr, 4, 16 * 4, 1, nullptr));
    EXPECT_EQ(planar[3], interleaved[3 * 4]);
  }

  // 2. 3-band RGB + 3-band RGB: SrcOverRGB (nPixelSpace==1) and strided.
  {
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", c3, "!", "blend", "--opacity", "50", o3}));
    ASSERT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> planar(24 * 4 * 3, 0);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 24, 4, planar.data(), 24, 4, GDT_Byte,
                           3, nullptr, 1, 24, 24 * 4, nullptr));
    std::vector<uint8_t> strided(24 * 4 * 2, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 24, 4, strided.data(), 24, 4,
                           GDT_Byte, 2, 24 * 2, nullptr));
    EXPECT_EQ(planar[7], strided[14]);
  }

  // 3. General src-over combinations:
  //    1+1, 2+2, 2+4 (RGBToGrayScale), 4+1, 3+4.
  struct Combo {
    const std::string& color;
    const std::string& overlay;
    int expected_bands;
  };
  const Combo combos[] = {
      {c1, o1, 1}, {c2, o2, 2}, {c2, o4, 2}, {c4, o1, 4}, {c3, o4, 3},
  };
  for (const auto& combo : combos) {
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", combo.color, "!", "blend", "--opacity", "40", combo.overlay}));
    ASSERT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(combo.expected_bands, ds->GetRasterCount());
    if (combo.expected_bands == 1) {
      EXPECT_EQ(GCI_GrayIndex, ds->GetRasterBand(1)->GetColorInterpretation());
    } else if (combo.expected_bands == 2) {
      EXPECT_EQ(GCI_GrayIndex, ds->GetRasterBand(1)->GetColorInterpretation());
      EXPECT_EQ(GCI_AlphaBand, ds->GetRasterBand(2)->GetColorInterpretation());
    }
    std::vector<uint8_t> buf(24 * 4 * combo.expected_bands, 0);
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 24, 4, buf.data(), 24, 4,
                                    GDT_Byte, combo.expected_bands, nullptr, 1,
                                    24, 24 * 4, nullptr));
  }
}

TEST_F(GdalalgRasterBlendTest, OverviewsAndIoErrors) {
  const std::string c3 = std::string(kTestDir) + "/ovr_c3.tif";
  const std::string o1 = std::string(kTestDir) + "/ovr_o1.tif";

  {
    auto ds_c = CreateColorTiff(c3, 16, 16, 3);
    ASSERT_NE(nullptr, ds_c);
    int ovr_levels[] = {2};
    ASSERT_EQ(CE_None,
              ds_c->BuildOverviews("NEAREST", 1, ovr_levels, 0, nullptr,
                                   nullptr, nullptr, nullptr));
  }
  {
    auto ds_o = CreateOverlayTiff(o1, 16, 16, 1);
    ASSERT_NE(nullptr, ds_o);
    int ovr_levels[] = {2};
    ASSERT_EQ(CE_None,
              ds_o->BuildOverviews("NEAREST", 1, ovr_levels, 0, nullptr,
                                   nullptr, nullptr, nullptr));
  }

  // Upstream UAF bug at apps/gdalalg_raster_blend.cpp:523-539: ~BlendDataset()
  // releases m_oColorDS/m_oOverlayDS in its destructor body before member
  // destruction of m_apoOverviews releases child overview sub-datasets owned by
  // parent GTiffDataset. Keep ds_c/ds_o open in an outer scope so parent
  // datasets outlive BlendDataset and its m_apoOverviews.
  GDALDatasetUniquePtr ds_c(GDALDataset::Open(c3.c_str(), GDAL_OF_RASTER));
  GDALDatasetUniquePtr ds_o(GDALDataset::Open(o1.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(nullptr, ds_c);
  ASSERT_NE(nullptr, ds_o);

  GDALRasterBlendAlgorithm alg(/*standaloneStep=*/false);
  alg.GetInputDatasets().emplace_back(ds_c.get());
  alg.GetArg("overlay")->Get<GDALArgDatasetValue>().Set(ds_o.get());
  ASSERT_TRUE(alg.ParseCommandLineArguments({"--operator", "hsv-value"}));
  ASSERT_TRUE(alg.Run());
  GDALDataset* ds = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, ds);

  EXPECT_EQ(1, ds->GetRasterBand(1)->GetOverviewCount());
  EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetOverview(-1));
  EXPECT_NE(nullptr, ds->GetRasterBand(1)->GetOverview(0));
  EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetOverview(1));

  // Downsampled dataset and band RasterIO trigger TryOverviewRasterIO.
  std::vector<uint8_t> ovr_buf(8 * 8 * 3, 0);
  EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 16, 16, ovr_buf.data(), 8, 8,
                                  GDT_Byte, 3, nullptr, 1, 8, 8 * 8, nullptr));
  std::vector<uint8_t> ovr_band_buf(8 * 8, 0);
  EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 16, 16,
                                                    ovr_band_buf.data(), 8, 8,
                                                    GDT_Byte, 1, 8, nullptr));

  // Writing to read-only BlendDataset / BlendBand triggers fallback &
  // m_ioError.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::vector<uint8_t> write_buf(16 * 16 * 3, 0);
    EXPECT_NE(CE_None,
              ds->RasterIO(GF_Write, 0, 0, 16, 16, write_buf.data(), 16, 16,
                           GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    // Subsequent call hits m_ioError fast failure.
    EXPECT_NE(CE_None,
              ds->RasterIO(GF_Write, 0, 0, 16, 16, write_buf.data(), 16, 16,
                           GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_NE(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 16, 16,
                                                      write_buf.data(), 16, 16,
                                                      GDT_Byte, 0, 0, nullptr));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
