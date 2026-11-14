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
// Tests for GDALRasterSieveAlgorithm (apps/gdalalg_raster_sieve.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_sieve.cpp

#include "third_party/gdal/apps/gdalalg_raster_sieve.h"

#include <cstdint>
#include <filesystem>  // NOLINT
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
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_sieve_test";

class GdalalgRasterSieveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateTiffFromGrid(const std::string& path, int width, int height,
                        int bands, const std::vector<uint8_t>& band1_pixels) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), width, height, bands, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());
  std::vector<uint8_t> pixels = band1_pixels;
  for (int b = 1; b <= bands; ++b) {
    ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                           GF_Write, 0, 0, width, height, pixels.data(), width,
                           height, GDT_Byte, 0, 0, nullptr));
  }
}

TEST_F(GdalalgRasterSieveTest, FourConnectedVsEightConnectedAndProgress) {
  // 4x4 grid with a 3-pixel diagonal of value 2 on background 1.
  // 4-connected sees three 1-pixel polygons (removed at threshold 2);
  // 8-connected sees one 3-pixel polygon (kept at threshold 2).
  const std::vector<uint8_t> grid = {
      2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1,
  };
  const std::string in_tif = std::string(kTestDir) + "/diag.tif";
  const std::string out4_tif = std::string(kTestDir) + "/out4.tif";
  const std::string out8_tif = std::string(kTestDir) + "/out8.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiffFromGrid(in_tif, 4, 4, 1, grid));

  // 1. 4-connected sieve with progress callback.
  {
    GDALRasterSieveAlgorithmStandalone alg;
    EXPECT_EQ("sieve", alg.GetName());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--size-threshold", "2", in_tif, out4_tif, "--overwrite"}));
    int progress_calls = 0;
    auto progress_cb = [](double, const char*, void* data) -> int {
      ++(*static_cast<int*>(data));
      return 1;
    };
    EXPECT_TRUE(alg.Run(progress_cb, &progress_calls));
    EXPECT_GT(progress_calls, 0);
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out4_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    std::vector<uint8_t> buf(16, 0);
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 4, 4, buf.data(), 4, 4, GDT_Byte, 0,
                           0, nullptr));
    EXPECT_EQ(1, buf[0]);
    EXPECT_EQ(1, buf[5]);
  }

  // 2. 8-connected sieve preserves diagonal.
  {
    GDALRasterSieveAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--size-threshold",
        "2",
        "--connect-diagonal-pixels",
        in_tif,
        out8_tif,
        "--overwrite",
    }));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out8_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    std::vector<uint8_t> buf(16, 0);
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 4, 4, buf.data(), 4, 4, GDT_Byte, 0,
                           0, nullptr));
    EXPECT_EQ(2, buf[0]);
    EXPECT_EQ(2, buf[5]);
    EXPECT_EQ(2, buf[10]);
  }
}

TEST_F(GdalalgRasterSieveTest, MaskBandSecondBandAndPipeline) {
  const std::vector<uint8_t> grid = {
      5, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  };
  // Mask out pixel (0,0) so it is excluded from sieving.
  const std::vector<uint8_t> mask_grid = {
      0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  };
  const std::string in_tif = std::string(kTestDir) + "/in2band.tif";
  const std::string mask_tif = std::string(kTestDir) + "/mask.tif";
  const std::string out_tif = std::string(kTestDir) + "/masked_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiffFromGrid(in_tif, 4, 4, 2, grid));
  ASSERT_NO_FATAL_FAILURE(CreateTiffFromGrid(mask_tif, 4, 4, 1, mask_grid));

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments({
      "read",
      in_tif,
      "!",
      "sieve",
      "--band",
      "2",
      "--size-threshold",
      "4",
      "--mask",
      mask_tif,
      "!",
      "write",
      out_tif,
      "--overwrite",
  }));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  std::vector<uint8_t> buf(16, 0);
  ASSERT_EQ(CE_None,
            out_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 4, buf.data(),
                                               4, 4, GDT_Byte, 0, 0, nullptr));
  // Masked pixel (0,0) kept its original value 5.
  EXPECT_EQ(5, buf[0]);
}

TEST_F(GdalalgRasterSieveTest, ErrorPathsInvalidBandAndEmptyMask) {
  const std::vector<uint8_t> grid(16, 1);
  const std::string in_tif = std::string(kTestDir) + "/err_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/err_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiffFromGrid(in_tif, 4, 4, 1, grid));

  // 1. Out-of-range band rejected by ValidateBandArg() at parse time.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterSieveAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--band", "99", in_tif, out_tif, "--overwrite"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("less or equal than 1"));
    CPLErrorReset();
  }

  // 2. Unwritable temporary GTiff directory triggers CreateTemporaryCopy
  // failure in Run().
  {
    const std::string missing_tmpdir =
        (std::filesystem::path(::testing::TempDir()) /
         "nonexistent_sieve_tmpdir")
            .string();
    CPLConfigOptionSetter use_gtiff(
        "GDAL_RASTER_PIPELINE_USE_GTIFF_FOR_TEMP_DATASET", "YES",
        /*bSetOnlyIfUndefined=*/false);
    CPLConfigOptionSetter tmpdir("CPL_TMPDIR", missing_tmpdir.c_str(),
                                 /*bSetOnlyIfUndefined=*/false);

    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterSieveAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_tif, "--overwrite"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to create temporary dataset"));
    CPLErrorReset();
  }

  // 3. 0-band mask dataset triggers "Cannot get mask band."
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_THAT(mem_drv, NotNull());
    GDALDatasetUniquePtr empty_mask(
        mem_drv->Create("", 4, 4, 0, GDT_Byte, nullptr));
    ASSERT_THAT(empty_mask, NotNull());

    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterSieveAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("mask")->Set(empty_mask.get()));
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_tif, "--overwrite"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot get mask band."));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
