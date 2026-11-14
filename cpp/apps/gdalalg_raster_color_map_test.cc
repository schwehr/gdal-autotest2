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
// Tests for GDALRasterColorMapAlgorithm (apps/gdalalg_raster_color_map.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_color_map.cpp

#include "third_party/gdal/apps/gdalalg_raster_color_map.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_colortable.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_color_map_test";

class GdalalgRasterColorMapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateGrayTiff(const std::string& path, int bands,
                    const std::vector<uint8_t>& pixels, bool with_color_table) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 2, 2, bands, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  if (with_color_table) {
    // Set color table before writing pixels so TIFF PhotometricInterpretation
    // can be set cleanly.
    GDALColorTable ct;
    GDALColorEntry e0{10, 20, 30, 255};
    GDALColorEntry e1{40, 50, 60, 255};
    ct.SetColorEntry(0, &e0);
    ct.SetColorEntry(1, &e1);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetColorTable(&ct));
  }
  for (int b = 1; b <= bands; ++b) {
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(b)->RasterIO(
                  GF_Write, 0, 0, 2, 2, const_cast<uint8_t*>(pixels.data()), 2,
                  2, GDT_Byte, 0, 0, nullptr));
  }
}

TEST_F(GdalalgRasterColorMapTest, StandalonePropertiesAndCanHandleNextStep) {
  GDALRasterColorMapAlgorithmStandalone alg;
  EXPECT_EQ("color-map", alg.GetName());
  EXPECT_NE(nullptr, alg.GetArg("band"));
  EXPECT_NE(nullptr, alg.GetArg("color-map"));
  EXPECT_NE(nullptr, alg.GetArg("add-alpha"));
  EXPECT_NE(nullptr, alg.GetArg("color-selection"));
}

TEST_F(GdalalgRasterColorMapTest, ExpandColorTableRgbAndRgba) {
  const std::string in_tif = std::string(kTestDir) + "/ct_in.tif";
  const std::string out_rgb = std::string(kTestDir) + "/ct_rgb.tif";
  const std::string out_rgba = std::string(kTestDir) + "/ct_rgba.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGrayTiff(in_tif, 1, {0, 1, 0, 1}, true));

  // 1. Expand color table to RGB (3 bands) with explicit exact color-selection.
  {
    GDALRasterColorMapAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"-i", in_tif, "-o", out_rgb, "-f",
                                       "GTiff", "--color-selection", "exact"}));
    CPLErrorReset();
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
    ASSERT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_rgb.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(3, out_ds->GetRasterCount());
    std::array<uint8_t, 4> band1{};
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 2, 2, band1.data(), 2, 2, GDT_Byte, 0,
                           0, nullptr));
    EXPECT_EQ(10, band1[0]);
    EXPECT_EQ(40, band1[1]);
  }

  // 2. Expand RGBA color table via VRT (GTiff color tables do not store alpha)
  // and trigger ignored color-selection warning.
  const std::string in_vrt = std::string(kTestDir) + "/ct_rgba_in.vrt";
  VsiMemTempWrapper in_vrt_wrapper(
      in_vrt,
      R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2">
  <VRTRasterBand dataType="Byte" band="1">
    <ColorInterp>Palette</ColorInterp>
    <ColorTable>
      <Entry c1="10" c2="20" c3="30" c4="255"/>
      <Entry c1="40" c2="50" c3="60" c4="128"/>
    </ColorTable>
    <SimpleSource>
      <SourceFilename relativeToVRT="0">/vsimem/gdalalg_raster_color_map_test/ct_in.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>)vrt");
  {
    GDALRasterColorMapAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_vrt, "-o", out_rgba, "-f", "GTiff", "--add-alpha",
         "--color-selection", "interpolate"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("color-selection"));
    CPLErrorReset();
    ASSERT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_rgba.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(4, out_ds->GetRasterCount());
    std::array<uint8_t, 4> alpha{};
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(4)->RasterIO(
                           GF_Read, 0, 0, 2, 2, alpha.data(), 2, 2, GDT_Byte, 0,
                           0, nullptr));
    EXPECT_EQ(255, alpha[0]);
    EXPECT_EQ(128, alpha[1]);
  }
}

TEST_F(GdalalgRasterColorMapTest, MissingColorTableAndNoColorMapFails) {
  const std::string in_tif = std::string(kTestDir) + "/no_ct.tif";
  const std::string out_tif = std::string(kTestDir) + "/no_ct_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGrayTiff(in_tif, 1, {0, 1, 2, 3}, false));

  GDALRasterColorMapAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"-i", in_tif, "-o", out_tif, "-f", "GTiff"}));

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(alg.Run());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("no color table"));
  CPLErrorReset();
}

TEST_F(GdalalgRasterColorMapTest, ColorMapFileSelectionModesAndBand) {
  const std::string in_tif = std::string(kTestDir) + "/dem.tif";
  const std::string cmap_file = std::string(kTestDir) + "/cmap.txt";
  ASSERT_NO_FATAL_FAILURE(CreateGrayTiff(in_tif, 2, {0, 50, 100, 150}, false));
  VsiMemTempWrapper cmap_wrapper(cmap_file,
                                 "0 0 0 0 255\n"
                                 "100 200 100 50 255\n");

  // 1. Interpolate mode (default).
  {
    const std::string out_tif = std::string(kTestDir) + "/interp.tif";
    GDALRasterColorMapAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-i", in_tif, "-o", out_tif,
                                               "-f", "GTiff", "--band", "2",
                                               "--color-map", cmap_file}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(3, ds->GetRasterCount());
    std::array<uint8_t, 4> red{};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, red.data(), 2,
                                             2, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(0, red[0]);
    EXPECT_EQ(100, red[1]);
    EXPECT_EQ(200, red[2]);
  }

  // 2. Exact mode with alpha.
  {
    const std::string out_tif = std::string(kTestDir) + "/exact_alpha.tif";
    GDALRasterColorMapAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_tif, "-o", out_tif, "-f", "GTiff", "--color-map", cmap_file,
         "--color-selection", "exact", "--add-alpha"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterCount());
    std::array<uint8_t, 4> alpha{};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(4)->RasterIO(GF_Read, 0, 0, 2, 2, alpha.data(),
                                             2, 2, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(255, alpha[0]);
    EXPECT_EQ(0, alpha[1]);
    EXPECT_EQ(255, alpha[2]);
  }

  // 3. Nearest mode.
  {
    const std::string out_tif = std::string(kTestDir) + "/nearest.tif";
    GDALRasterColorMapAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_tif, "-o", out_tif, "-f", "GTiff", "--color-map", cmap_file,
         "--color-selection", "nearest"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(3, ds->GetRasterCount());
    std::array<uint8_t, 4> red{};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, red.data(), 2,
                                             2, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(200, red[3]);
  }
}

TEST_F(GdalalgRasterColorMapTest, InvalidColorMapFileFails) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_vrt = std::string(kTestDir) + "/out.vrt";
  ASSERT_NO_FATAL_FAILURE(CreateGrayTiff(in_tif, 1, {0, 1, 2, 3}, false));

  GDALRasterColorMapAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"-i", in_tif, "-o", out_vrt, "-f", "VRT", "--color-map",
       "/vsimem/gdalalg_raster_color_map_test/missing_cmap.txt"}));
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(alg.Run());
  CPLErrorReset();
}

TEST_F(GdalalgRasterColorMapTest, PipelineWithWriteAndInfoSteps) {
  const std::string in_tif = std::string(kTestDir) + "/pipe_in.tif";
  const std::string ct_tif = std::string(kTestDir) + "/pipe_ct.tif";
  const std::string out_tif = std::string(kTestDir) + "/pipe_out.tif";
  const std::string out_ct_tif = std::string(kTestDir) + "/pipe_ct_out.tif";
  const std::string cmap_file = std::string(kTestDir) + "/pipe_cmap.txt";
  ASSERT_NO_FATAL_FAILURE(CreateGrayTiff(in_tif, 1, {0, 50, 100, 150}, false));
  ASSERT_NO_FATAL_FAILURE(CreateGrayTiff(ct_tif, 1, {0, 1, 0, 1}, true));
  VsiMemTempWrapper cmap_wrapper(cmap_file,
                                 "0 0 0 0 255\n"
                                 "100 200 100 50 255\n");

  // 1. Color map file directly into write step (exercises CanHandleNextStep
  // and ctxt.m_poNextUsableStep with format, creation options, and progress).
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "color-map", "--color-map", cmap_file, "!",
         "write", out_tif, "--of", "GTiff", "--co", "COMPRESS=NONE",
         "--overwrite"}));
    bool progress_called = false;
    auto progress_cb = [](double, const char*, void* data) -> int {
      *static_cast<bool*>(data) = true;
      return 1;
    };
    EXPECT_TRUE(pipeline.Run(progress_cb, &progress_called));
    EXPECT_TRUE(progress_called);
    ASSERT_TRUE(pipeline.Finalize());
  }

  // 2. Color table directly into write step.
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", ct_tif, "!", "color-map", "!", "write", out_ct_tif, "--of",
         "GTiff", "--overwrite"}));
    auto progress_cb = [](double, const char*, void*) -> int { return 1; };
    EXPECT_TRUE(pipeline.Run(progress_cb, nullptr));
    ASSERT_TRUE(pipeline.Finalize());
  }

  // 3. Color map into info (no next usable write step -> VRT output).
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments({"read", in_tif, "!",
                                                    "color-map", "--color-map",
                                                    cmap_file, "!", "info"}));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_THAT(pipeline.GetOutputString(), HasSubstr("\"band\":3"));
  }

  // 4. Write with stream format -> CanHandleNextStep returns false and pipeline
  // falls back to VRT + write.
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "color-map", "--color-map", cmap_file, "!",
         "write", out_tif, "--of", "stream"}));
    EXPECT_TRUE(pipeline.Run());
  }
}

}  // namespace
}  // namespace autotest2
