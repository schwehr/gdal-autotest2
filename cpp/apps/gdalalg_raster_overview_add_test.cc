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
// Tests for "raster overview add" subcommand
// (apps/gdalalg_raster_overview_add.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_overview_add.cpp

#include "third_party/gdal/apps/gdalalg_raster_overview_add.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_overview.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_overview_add_test";
constexpr char kTmpDir[] = "/vsimem/gdalalg_raster_overview_add_tmp";

class GdalalgRasterOverviewAddTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    ASSERT_EQ(0, VSIMkdir(kTmpDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTmpDir);
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    GDALRegister_HFA();
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
    VSIRmdirRecursive(kTmpDir);
    CPLErrorReset();
  }
};

void CreateTestRaster(const std::string& driver_name, const std::string& path,
                      int width, int height, uint8_t fill_value) {
  GDALDriver* drv =
      GetGDALDriverManager()->GetDriverByName(driver_name.c_str());
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), width, height, 1, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height, fill_value);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, width, height, pixels.data(), width,
                         height, GDT_Byte, 0, 0, nullptr));
}

TEST_F(GdalalgRasterOverviewAddTest, ParentOverviewRunFails) {
  {
    GDALRasterOverviewAlgorithm overview_alg(/*standaloneStep=*/false);
    EXPECT_EQ("overview", overview_alg.GetName());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(overview_alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("should not be called directly"));
    CPLErrorReset();
  }
  {
    GDALRasterOverviewAlgorithmStandalone overview_alg;
    EXPECT_EQ("overview", overview_alg.GetName());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(overview_alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Required argument 'input'"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterOverviewAddTest, CreationOptionAutoComplete) {
  const std::string tif = std::string(kTestDir) + "/autocomplete.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestRaster("GTiff", tif, 16, 16, 10));

  GDALRasterOverviewAlgorithmAddStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments({tif}));
  GDALAlgorithmArg* co_arg = alg.GetArg(GDAL_ARG_NAME_CREATION_OPTION);
  ASSERT_THAT(co_arg, NotNull());
  EXPECT_THAT(co_arg->GetAutoCompleteChoices(""), Not(IsEmpty()));
  EXPECT_THAT(co_arg->GetAutoCompleteChoices("COMPRESS="), Not(IsEmpty()));
}

TEST_F(GdalalgRasterOverviewAddTest, InternalExternalAndReuseOverviews) {
  const std::string tif = std::string(kTestDir) + "/internal.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestRaster("GTiff", tif, 64, 64, 42));

  // 1. Add internal overviews with explicit levels, resampling, creation
  // option, and progress callback.
  {
    GDALRasterOverviewAlgorithmAddStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--levels", "2,4", "-r", "average", "--co", "COMPRESS=LZW", tif}));
    bool progress_called = false;
    auto progress_cb = [](double, const char*, void* user_data) -> int {
      *static_cast<bool*>(user_data) = true;
      return 1;
    };
    EXPECT_TRUE(alg.Run(progress_cb, &progress_called));
    EXPECT_TRUE(alg.Finalize());
    EXPECT_TRUE(progress_called);
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(2, ds->GetRasterBand(1)->GetOverviewCount());
    EXPECT_EQ(32, ds->GetRasterBand(1)->GetOverview(0)->GetXSize());
    EXPECT_EQ(16, ds->GetRasterBand(1)->GetOverview(1)->GetXSize());
  }

  // 2. Re-run without --levels or --resampling to reuse existing levels and
  // resampling metadata.
  {
    GDALRasterOverviewAlgorithmAddStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // 3. Add external overviews on a fresh GeoTIFF.
  const std::string ext_tif = std::string(kTestDir) + "/external.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestRaster("GTiff", ext_tif, 64, 64, 24));
  {
    GDALRasterOverviewAlgorithmAddStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--external", "--min-size", "16", ext_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  const std::string ovr_path = ext_tif + ".ovr";
  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(ovr_path.c_str(), &stat_buf));
}

TEST_F(GdalalgRasterOverviewAddTest, ExternalNotSupportedByHFARejected) {
  const std::string img = std::string(kTestDir) + "/test.img";
  ASSERT_NO_FATAL_FAILURE(CreateTestRaster("HFA", img, 32, 32, 5));

  GDALRasterOverviewAlgorithmAddStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments({"--external", img}));
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(alg.Run());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("does not support external overviews"));
}

TEST_F(GdalalgRasterOverviewAddTest, MinSizeNoOpWhenAlreadySmallEnough) {
  const std::string tif = std::string(kTestDir) + "/small.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestRaster("GTiff", tif, 16, 16, 7));

  GDALRasterOverviewAlgorithmAddStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments({tif}));
  EXPECT_TRUE(alg.Run());
  EXPECT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                        nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(0, ds->GetRasterBand(1)->GetOverviewCount());
}

TEST_F(GdalalgRasterOverviewAddTest, OverviewSrcAndMutualExclusion) {
  const std::string base_tif = std::string(kTestDir) + "/base.tif";
  const std::string ovr_src_tif = std::string(kTestDir) + "/ovr_src.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestRaster("GTiff", base_tif, 32, 32, 10));
  ASSERT_NO_FATAL_FAILURE(CreateTestRaster("GTiff", ovr_src_tif, 16, 16, 20));

  // Mutual exclusion between --overview-src and --levels.
  {
    GDALRasterOverviewAlgorithmAddStandalone alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--overview-src", ovr_src_tif, "--levels", "2", base_tif}));
  }

  // Valid --overview-src.
  {
    GDALRasterOverviewAlgorithmAddStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--overview-src", ovr_src_tif, base_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(base_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(1, ds->GetRasterBand(1)->GetOverviewCount());
  EXPECT_EQ(16, ds->GetRasterBand(1)->GetOverview(0)->GetXSize());
}

TEST_F(GdalalgRasterOverviewAddTest, PipelineVirtualOverviewStep) {
  const std::string in_tif = std::string(kTestDir) + "/pipe_in.tif";
  const std::string out_vrt = std::string(kTestDir) + "/pipe_out.vrt";
  ASSERT_NO_FATAL_FAILURE(CreateTestRaster("GTiff", in_tif, 64, 64, 33));

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_tif, "!", "overview", "add", "--levels", "2,4", "-r",
       "nearest", "!", "write", out_vrt, "--overwrite"}));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(out_vrt.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(2, ds->GetRasterBand(1)->GetOverviewCount());
}

}  // namespace
}  // namespace autotest2
