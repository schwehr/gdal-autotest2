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
// Tests for GDALRasterResizeAlgorithm (apps/gdalalg_raster_resize.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_resize.cpp

#include "third_party/gdal/apps/gdalalg_raster_resize.h"

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
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_resize_test";

void CreateTiff(const std::string& path, int width, int height) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), width, height, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALGeoTransform gt(0.0, 2.0, 0.0, static_cast<double>(height) * 2.0, 0.0,
                      -2.0);
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height, 42);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, width, height, pixels.data(), width,
                         height, GDT_Byte, 0, 0, nullptr));
}

class GdalalgRasterResizeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterResizeTest, StandaloneProperties) {
  GDALRasterResizeAlgorithmStandalone alg;
  EXPECT_EQ("resize", alg.GetName());
  EXPECT_EQ(
      "Resize a raster dataset without changing the georeferenced extents.",
      alg.GetDescription());
}

TEST_F(GdalalgRasterResizeTest, SizePixelsAndPercentage) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_px = std::string(kTestDir) + "/out_px.tif";
  const std::string out_pct = std::string(kTestDir) + "/out_pct.tif";
  const std::string out_pct_sp = std::string(kTestDir) + "/out_pct_sp.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(in_tif, 20, 10));

  // 1. Explicit pixel size and bilinear resampling.
  {
    GDALRasterResizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--size", "10,5", "--resampling",
                                               "bilinear", in_tif, out_px,
                                               "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_px.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(10, ds->GetRasterXSize());
    EXPECT_EQ(5, ds->GetRasterYSize());
    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(4.0, gt[1]);
    EXPECT_DOUBLE_EQ(-4.0, gt[5]);
  }

  // 2. Percentage size (50%,50%) and zero-auto height (0,50%).
  {
    GDALRasterResizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--size", "50%,50%", "-r", "cubic", in_tif, out_pct, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_pct.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(10, ds->GetRasterXSize());
    EXPECT_EQ(5, ds->GetRasterYSize());
  }

  // 3. Percentage with space before '%' ("50 %") and auto width ("0").
  {
    GDALRasterResizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--size", "0,50 %", "-r", "near", in_tif, out_pct_sp, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_pct_sp.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(10, ds->GetRasterXSize());
    EXPECT_EQ(5, ds->GetRasterYSize());
  }
}

TEST_F(GdalalgRasterResizeTest, ResolutionAndPipelineStreaming) {
  const std::string in_tif = std::string(kTestDir) + "/res_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/res_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(in_tif, 20, 10));

  // 1. Standalone with --resolution.
  {
    GDALRasterResizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--resolution", "4,4", "--resampling", "average", in_tif, out_tif,
         "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(10, ds->GetRasterXSize());
    EXPECT_EQ(5, ds->GetRasterYSize());
  }

  // 2. Pipeline streaming (resize produces VRT output dataset).
  {
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(
        pipe.ParseCommandLineArguments({"read", in_tif, "!", "resize", "--size",
                                        "8,4", "--resampling", "lanczos"}));
    EXPECT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(8, ds->GetRasterXSize());
    EXPECT_EQ(4, ds->GetRasterYSize());
  }
}

TEST_F(GdalalgRasterResizeTest, ValidationAndRunErrors) {
  const std::string in_tif = std::string(kTestDir) + "/err_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/err_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(in_tif, 10, 10));

  // 1. Invalid size strings.
  for (const std::string& bad_size : {"abc,10", "10,-5", "10.5,10", "10px,10",
                                      "-10%,10", "9999999999999,10"}) {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterResizeAlgorithmStandalone alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--size", bad_size, in_tif, out_tif}))
        << "Expected failure for size: " << bad_size;
    CPLErrorReset();
  }

  // 2. Mutual exclusion between --size and --resolution.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterResizeAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--size", "5,5", "--resolution", "2,2", in_tif, out_tif}));
    CPLErrorReset();
  }

  // 3. Invalid resolution <= 0 or invalid resampling.
  {
    WithQuietHandler quiet;
    GDALRasterResizeAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--resolution", "0,2", in_tif, out_tif}));
  }
  {
    WithQuietHandler quiet;
    GDALRasterResizeAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--size", "5,5", "--resampling", "invalid", in_tif, out_tif}));
  }

  // 4. --size 0,0 passes validation but GDALTranslate fails at runtime.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterResizeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--size", "0,0", "-f", "MEM", in_tif, ""}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
