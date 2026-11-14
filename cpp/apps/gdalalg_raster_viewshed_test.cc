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
// Tests for GDALRasterViewshedAlgorithm (apps/gdalalg_raster_viewshed.cpp and
// apps/gdalalg_raster_viewshed.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_viewshed.cpp

#include "third_party/gdal/apps/gdalalg_raster_viewshed.h"

#include <algorithm>
#include <array>
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
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_viewshed_test";

void CreateFlatDemWithPeak(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 11, 11, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(32632));
  ds->SetSpatialRef(&srs);
  std::array<double, 6> gt = {0.0, 10.0, 0.0, 110.0, 0.0, -10.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt.data()));

  std::vector<float> elev(121, 0.0f);
  // Wall at x=7 blocking visibility to the right of x=7.
  for (int row = 0; row < 11; ++row) {
    elev[static_cast<size_t>(row) * 11 + 7] = 100.0f;
  }
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 11, 11, elev.data(),
                                           11, 11, GDT_Float32, 0, 0, nullptr));
}

class GdalalgRasterViewshedTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTestDir);
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
  }
};

TEST_F(GdalalgRasterViewshedTest, StandardModesAndAllOptions) {
#if defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__)
  // TODO(schwehr): Re-enable under TSan once upstream
  // GDALRasterBlock::TakeLock()/Touch() block-cache race is fixed
  // (ViewshedExecutor::run() unconditionally runs concurrent worker threads).
  GTEST_SKIP() << "Upstream GDALRasterBlock::TakeLock()/Touch() data race";
#endif
  const std::string dem_tif = std::string(kTestDir) + "/dem.tif";
  ASSERT_NO_FATAL_FAILURE(CreateFlatDemWithPeak(dem_tif));

  // 1. Normal mode with full options and 3D position.
  const std::string out_normal = std::string(kTestDir) + "/normal.tif";
  {
    GDALRasterViewshedAlgorithmStandalone alg;
    EXPECT_EQ("viewshed", alg.GetName());
    EXPECT_FALSE(alg.GetDescription().empty());
    EXPECT_FALSE(alg.GetHelpURL().empty());

    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--position=55,55,10", "--target-height=1", "--mode=normal",
         "--max-distance=80", "--min-distance=5", "--start-angle=0",
         "--end-angle=180", "--low-pitch=-45", "--high-pitch=45",
         "--curvature-coefficient=0.85", "--band=1", "--visible-value=255",
         "--invisible-value=0", "--out-of-range-value=128", "--dst-nodata=200",
         dem_tif, out_normal}));
    auto progress = [](double, const char*, void*) -> int { return 1; };
    EXPECT_TRUE(alg.Run(progress, nullptr));
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_normal.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    const int xsize = ds->GetRasterXSize();
    const int ysize = ds->GetRasterYSize();
    EXPECT_GT(xsize, 0);
    EXPECT_GT(ysize, 0);
    std::vector<uint8_t> vis(static_cast<size_t>(xsize) * ysize, 1);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, xsize, ysize, vis.data(), xsize,
                           ysize, GDT_Byte, 0, 0, nullptr));
    EXPECT_TRUE(std::any_of(vis.begin(), vis.end(),
                            [](uint8_t v) { return v == 255; }));
    EXPECT_TRUE(
        std::any_of(vis.begin(), vis.end(), [](uint8_t v) { return v == 0; }));
  }

  // 2. DEM and ground modes with 2D position.
  for (const std::string& mode : {"DEM", "ground"}) {
    const std::string out_path =
        std::string(kTestDir) + "/out_" + mode + ".tif";
    GDALRasterViewshedAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--position=55,55", "--mode=" + mode, dem_tif, out_path}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
    VSIStatBufL stat_buf;
    EXPECT_EQ(0, VSIStatL(out_path.c_str(), &stat_buf));
  }

  // 3. Inside GDALRasterPipelineAlgorithm.
  const std::string pipe_out = std::string(kTestDir) + "/pipe.tif";
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", dem_tif, "!", "viewshed", "--position=55,55", "!", "write",
         pipe_out, "--overwrite"}));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());
  }
  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(pipe_out.c_str(), &stat_buf));
}

TEST_F(GdalalgRasterViewshedTest, CumulativeMode) {
#if defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__)
  // TODO(schwehr): Re-enable under TSan once upstream
  // GDALRasterBlock::TakeLock()/Touch() block-cache race is fixed
  // (gdal::viewshed::Cumulative spawns worker pools even with -j 1).
  GTEST_SKIP() << "Upstream GDALRasterBlock::TakeLock()/Touch() data race";
#endif
  const std::string dem_tif = std::string(kTestDir) + "/cum_dem.tif";
  const std::string out_tif = std::string(kTestDir) + "/cum_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateFlatDemWithPeak(dem_tif));

  {
    GDALRasterViewshedAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--mode=cumulative",
                                               "--observer-spacing=5", "-j",
                                               "1", dem_tif, out_tif}));
    auto progress = [](double, const char*, void*) -> int { return 1; };
    EXPECT_TRUE(alg.Run(progress, nullptr));
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(11, ds->GetRasterXSize());
  EXPECT_EQ(11, ds->GetRasterYSize());
}

TEST_F(GdalalgRasterViewshedTest, ErrorPaths) {
  const std::string dem_tif = std::string(kTestDir) + "/err_dem.tif";
  ASSERT_NO_FATAL_FAILURE(CreateFlatDemWithPeak(dem_tif));

  struct ErrorCase {
    std::vector<std::string> args;
    std::string expected_msg;
  };
  const ErrorCase cases[] = {
      {.args = {"-f", "stream", "--position=55,55,10", "--height=5", dem_tif,
                "out"},
       .expected_msg = "Height can't be specified in both"},
      {.args = {"-f", "stream", dem_tif, "out"},
       .expected_msg = "Option 'position' must be specified"},
      {.args = {"-f", "stream", "--position=55,55", "--observer-spacing=2",
                dem_tif, "out"},
       .expected_msg = "Option 'observer-spacing' can't be used"},
      {.args = {"-f", "stream", "--position=55,55", "-j", "2", dem_tif, "out"},
       .expected_msg = "Option 'num-threads' can't be used"},
      {.args = {"-f", "stream", "--mode=cumulative", "--position=55,55",
                dem_tif, "out"},
       .expected_msg = "Option 'position' can't be used in cumulative mode"},
      {.args = {"-f", "stream", "--mode=cumulative", "--max-distance=50",
                dem_tif, "out"},
       .expected_msg =
           "Option 'max-distance' can't be used in cumulative mode"},
  };

  for (const auto& tc : cases) {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterViewshedAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(tc.args));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(tc.expected_msg));
    CPLErrorReset();
  }

  // Cumulative mode rejects in-memory MEM datasets.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr mem_ds(
      mem_drv->Create("", 4, 4, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, mem_ds);
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterViewshedAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.GetArg("mode")->Set("cumulative"));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("stream"));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT)->Set("out"));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("must be opened by name"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
