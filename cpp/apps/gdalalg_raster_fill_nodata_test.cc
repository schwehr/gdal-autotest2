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
// Tests for GDALRasterFillNodataAlgorithm
// (apps/gdalalg_raster_fill_nodata.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_fill_nodata.cpp
//   https://gdal.org/programs/gdal_raster_fill_nodata.html

#include "third_party/gdal/apps/gdalalg_raster_fill_nodata.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_fill_nodata_test";

void CreateTiffWithHole(const std::string& path, int bands) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 5, 5, bands, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);

  for (int b = 1; b <= bands; ++b) {
    GDALRasterBand* band = ds->GetRasterBand(b);
    ASSERT_EQ(CE_None, band->SetNoDataValue(-9999.0));
    std::vector<float> pixels(25, static_cast<float>(10 * b));
    pixels[12] = -9999.0f;  // Center pixel (2, 2) is nodata.
    ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 5, 5, pixels.data(), 5, 5,
                                      GDT_Float32, 0, 0, nullptr));
  }
}

void CreateMaskTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 5, 5, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<unsigned char> mask(25, 1);
  mask[12] = 0;  // Mark center pixel invalid via mask.
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 5, 5, mask.data(), 5,
                                           5, GDT_Byte, 0, 0, nullptr));
}

class GdalalgRasterFillNodataTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    CPLSetConfigOption("CPL_TMPDIR", ::testing::TempDir().c_str());
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override {
    VSIRmdirRecursive(kTestDir);
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
  }
};

TEST_F(GdalalgRasterFillNodataTest, BasicPropertiesAndInvalidStrategy) {
  GDALRasterFillNodataAlgorithmStandalone alg;
  EXPECT_EQ("fill-nodata", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_raster_fill_nodata.html", alg.GetHelpURL());

  WithQuietHandler quiet;
  EXPECT_FALSE(alg.ParseCommandLineArguments({"--strategy", "invalid_strat"}));
}

TEST_F(GdalalgRasterFillNodataTest, InvdistAndNearestWithProgressAndMask) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string mask_tif = std::string(kTestDir) + "/mask.tif";
  const std::string out_invdist = std::string(kTestDir) + "/out_invdist.tif";
  const std::string out_nearest = std::string(kTestDir) + "/out_nearest.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiffWithHole(in_tif, 2));
  ASSERT_NO_FATAL_FAILURE(CreateMaskTiff(mask_tif));

  // 1. Default invdist on band 2 with smoothing and progress callback.
  {
    GDALRasterFillNodataAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--band", "2", "--max-distance", "10", "--smoothing-iterations", "1",
         "--strategy", "invdist", "--overwrite", in_tif, out_invdist}));
    bool progress_called = false;
    auto cb = [](double /*dfComplete*/, const char* /*pszMsg*/,
                 void* arg) -> int {
      *static_cast<bool*>(arg) = true;
      return 1;
    };
    ASSERT_TRUE(alg.Run(cb, &progress_called));
    EXPECT_TRUE(progress_called);
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_invdist.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetRasterCount());
    std::vector<float> buf(25, 0.0f);
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 5, buf.data(), 5,
                                             5, GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(20.0f, buf[12]);
  }

  // 2. Nearest strategy with explicit --mask inside raster pipeline.
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "fill-nodata", "--strategy", "nearest", "--mask",
         mask_tif, "-d", "5", "-s", "0", "!", "write", out_nearest,
         "--overwrite"}));
    ASSERT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_nearest.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    std::vector<float> buf(25, 0.0f);
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 5, buf.data(), 5,
                                             5, GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(10.0f, buf[12]);
  }
}

TEST_F(GdalalgRasterFillNodataTest, ErrorPathsInvalidBandAndEmptyMask) {
  const std::string in_tif = std::string(kTestDir) + "/err_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/err_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiffWithHole(in_tif, 1));

  // 1. Out-of-range band triggers ValidateBandArg failure during parsing.
  {
    GDALRasterFillNodataAlgorithmStandalone alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--band", "9", "--overwrite", in_tif, out_tif}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Mask dataset with 0 bands triggers "Cannot get mask band."
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  std::unique_ptr<GDALDataset> empty_mask(
      mem_drv->Create("", 5, 5, 0, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, empty_mask);
  {
    GDALRasterFillNodataAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("mask")->Set(empty_mask.get()));
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--overwrite", in_tif, out_tif}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot get mask band"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
