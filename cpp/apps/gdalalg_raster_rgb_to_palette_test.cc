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
// Tests for "raster rgb-to-palette" subcommand
// (apps/gdalalg_raster_rgb_to_palette.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_rgb_to_palette.cpp

#include "third_party/gdal/apps/gdalalg_raster_rgb_to_palette.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_rgb_to_palette_test";
constexpr char kTmpDir[] = "/vsimem/gdalalg_raster_rgb_to_palette_tmp";

class GdalalgRasterRGBToPaletteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    ASSERT_EQ(0, VSIMkdir(kTmpDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTmpDir);
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
    VSIRmdirRecursive(kTmpDir);
    CPLErrorReset();
  }
};

std::unique_ptr<GDALDataset> CreateMemRaster(
    int width, int height, int bands, GDALDataType dt,
    const std::vector<GDALColorInterp>& color_interps) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create("", width, height, bands, dt, nullptr));
  if (!ds) return nullptr;

  for (int b = 1; b <= bands; ++b) {
    GDALRasterBand* band = ds->GetRasterBand(b);
    if (!band) return nullptr;
    if (static_cast<size_t>(b - 1) < color_interps.size()) {
      EXPECT_EQ(CE_None, band->SetColorInterpretation(color_interps[b - 1]));
    }
    if (dt == GDT_Byte) {
      std::vector<uint8_t> buf(static_cast<size_t>(width) * height);
      for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<uint8_t>((i * 17 + b * 53) % 256);
      }
      EXPECT_EQ(CE_None,
                band->RasterIO(GF_Write, 0, 0, width, height, buf.data(), width,
                               height, GDT_Byte, 0, 0, nullptr));
    }
  }
  return ds;
}

TEST_F(GdalalgRasterRGBToPaletteTest, InputValidationErrorsAndWarnings) {
  // 1. Fewer than 3 bands fails.
  {
    auto ds = CreateMemRaster(4, 4, 2, GDT_Byte, {});
    ASSERT_THAT(ds, NotNull());
    GDALRasterRGBToPaletteAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("at least 3 bands"));
  }

  // 2. Non-byte band fails.
  {
    auto ds = CreateMemRaster(4, 4, 3, GDT_UInt16,
                              {GCI_RedBand, GCI_GreenBand, GCI_BlueBand});
    ASSERT_THAT(ds, NotNull());
    GDALRasterRGBToPaletteAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Non-byte band"));
  }

  // 3. Duplicate Red band fails.
  {
    auto ds = CreateMemRaster(4, 4, 3, GDT_Byte,
                              {GCI_RedBand, GCI_RedBand, GCI_BlueBand});
    ASSERT_THAT(ds, NotNull());
    GDALRasterRGBToPaletteAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Several Red bands found"));
  }

  // 4. 4-band RGBA and partial color interp emit warnings and succeed.
  {
    auto ds = CreateMemRaster(
        4, 4, 4, GDT_Byte,
        {GCI_RedBand, GCI_Undefined, GCI_Undefined, GCI_AlphaBand});
    ASSERT_THAT(ds, NotNull());
    GDALRasterRGBToPaletteAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(1, out_ds->GetRasterCount());
    EXPECT_THAT(out_ds->GetRasterBand(1)->GetColorTable(), NotNull());
  }
}

TEST_F(GdalalgRasterRGBToPaletteTest, MedianCutWithProgressAndColorCount) {
  auto ds = CreateMemRaster(8, 8, 3, GDT_Byte,
                            {GCI_BlueBand, GCI_GreenBand, GCI_RedBand});
  ASSERT_THAT(ds, NotNull());

  GDALRasterRGBToPaletteAlgorithm alg;
  alg.GetInputDatasets().emplace_back(ds.get());
  ASSERT_TRUE(alg.ParseCommandLineArguments({"--color-count", "16"}));

  bool progress_called = false;
  auto progress_cb = [](double, const char*, void* user_data) -> int {
    *static_cast<bool*>(user_data) = true;
    return 1;
  };
  ASSERT_TRUE(alg.Run(progress_cb, &progress_called));
  EXPECT_TRUE(progress_called);

  GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(1, out_ds->GetRasterCount());
  EXPECT_EQ(GDT_Byte, out_ds->GetRasterBand(1)->GetRasterDataType());
  const GDALColorTable* ct = out_ds->GetRasterBand(1)->GetColorTable();
  ASSERT_THAT(ct, NotNull());
  EXPECT_LE(ct->GetColorEntryCount(), 16);
}

TEST_F(GdalalgRasterRGBToPaletteTest, ColorMapFromDatasetAndTextFile) {
  auto ds = CreateMemRaster(4, 4, 3, GDT_Byte,
                            {GCI_RedBand, GCI_GreenBand, GCI_BlueBand});
  ASSERT_THAT(ds, NotNull());

  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(gtiff_drv, NotNull());

  // 1. GeoTIFF palette dataset.
  const std::string pal_tif = std::string(kTestDir) + "/palette.tif";
  {
    std::unique_ptr<GDALDataset> pal_ds(
        gtiff_drv->Create(pal_tif.c_str(), 2, 2, 1, GDT_Byte, nullptr));
    ASSERT_THAT(pal_ds, NotNull());
    GDALColorTable ct;
    GDALColorEntry c0{0, 0, 0, 255};
    GDALColorEntry c1{255, 255, 255, 255};
    ct.SetColorEntry(0, &c0);
    ct.SetColorEntry(1, &c1);
    EXPECT_EQ(CE_None, pal_ds->GetRasterBand(1)->SetColorTable(&ct));
  }

  {
    GDALRasterRGBToPaletteAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--color-map", pal_tif}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    const GDALColorTable* ct = out_ds->GetRasterBand(1)->GetColorTable();
    ASSERT_THAT(ct, NotNull());
    EXPECT_GE(ct->GetColorEntryCount(), 2);
  }

  // 2. GeoTIFF without a color table fails.
  const std::string no_pal_tif = std::string(kTestDir) + "/no_palette.tif";
  {
    std::unique_ptr<GDALDataset> no_pal_ds(
        gtiff_drv->Create(no_pal_tif.c_str(), 2, 2, 1, GDT_Byte, nullptr));
    ASSERT_THAT(no_pal_ds, NotNull());
  }
  {
    GDALRasterRGBToPaletteAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--color-map", no_pal_tif}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("does not contain a color table"));
  }

  // 3. Text color map file.
  const std::string pal_txt = std::string(kTestDir) + "/palette.txt";
  {
    const std::string content = "0 255 0 0\n1 0 255 0\n2 0 0 255\n";
    VSILFILE* fp = VSIFOpenL(pal_txt.c_str(), "wb");
    ASSERT_THAT(fp, NotNull());
    EXPECT_EQ(content.size(),
              VSIFWriteL(content.data(), 1, content.size(), fp));
    EXPECT_EQ(0, VSIFCloseL(fp));
  }
  {
    GDALRasterRGBToPaletteAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--color-map", pal_txt}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_THAT(out_ds->GetRasterBand(1)->GetColorTable(), NotNull());
  }

  // 4. Non-existent color map fails.
  {
    GDALRasterRGBToPaletteAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--color-map", std::string(kTestDir) + "/missing.txt"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
  }
}

TEST_F(GdalalgRasterRGBToPaletteTest, StandaloneAndPipeline) {
  const std::string in_tif = std::string(kTestDir) + "/in_rgb.tif";
  const std::string out_tif = std::string(kTestDir) + "/out_pal.tif";

  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(gtiff_drv, NotNull());
  {
    std::unique_ptr<GDALDataset> ds(
        gtiff_drv->Create(in_tif.c_str(), 4, 4, 3, GDT_Byte, nullptr));
    ASSERT_THAT(ds, NotNull());
    std::vector<uint8_t> pixels(16, 128);
    for (int b = 1; b <= 3; ++b) {
      ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, 4, 4, pixels.data(), 4, 4,
                             GDT_Byte, 0, 0, nullptr));
    }
  }

  // Standalone.
  {
    GDALRasterRGBToPaletteAlgorithmStandalone alg;
    EXPECT_EQ("rgb-to-palette", alg.GetName());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--color-count", "8", "--overwrite", in_tif, out_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // Pipeline.
  const std::string pipe_out = std::string(kTestDir) + "/pipe_pal.tif";
  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_tif, "!", "rgb-to-palette", "--color-count", "4", "!",
       "write", pipe_out, "--overwrite"}));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(pipe_out.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(1, ds->GetRasterCount());
  EXPECT_THAT(ds->GetRasterBand(1)->GetColorTable(), NotNull());
}

}  // namespace
}  // namespace autotest2
