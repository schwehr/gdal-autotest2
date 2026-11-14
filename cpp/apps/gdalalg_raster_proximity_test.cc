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
// Tests for GDALRasterProximityAlgorithm (apps/gdalalg_raster_proximity.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_proximity.cpp

#include "third_party/gdal/apps/gdalalg_raster_proximity.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_proximity_test";

class GdalalgRasterProximityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTestDir);
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    CPLSetConfigOption("GDAL_RASTER_PIPELINE_USE_GTIFF_FOR_TEMP_DATASET",
                       nullptr);
    VSIRmdirRecursive(kTestDir);
  }
};

void CreateSourceRaster(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALGeoTransform gt{0.0, 10.0, 0.0, 40.0, 0.0, -10.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

  std::array<uint8_t, 16> pixels{};
  pixels[0] = 1;
  pixels[15] = 2;
  for (int b = 1; b <= 2; ++b) {
    ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 4, 4,
                                                      pixels.data(), 4, 4,
                                                      GDT_Byte, 0, 0, nullptr));
  }
}

TEST_F(GdalalgRasterProximityTest, StepProperties) {
  GDALRasterProximityAlgorithmStandalone alg;
  EXPECT_EQ("proximity", alg.GetName());
  EXPECT_NE(nullptr, alg.GetArg("target-values"));
  EXPECT_NE(nullptr, alg.GetArg("distance-units"));
  EXPECT_NE(nullptr, alg.GetArg("max-distance"));
  EXPECT_NE(nullptr, alg.GetArg("fixed-value"));
  EXPECT_NE(nullptr, alg.GetArg("nodata"));
}

TEST_F(GdalalgRasterProximityTest, DefaultAndAllOptions) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_default = std::string(kTestDir) + "/out_default.tif";
  const std::string out_opts = std::string(kTestDir) + "/out_opts.tif";
  ASSERT_NO_FATAL_FAILURE(CreateSourceRaster(in_tif));

  // 1. Default options (Float32, pixel units).
  {
    GDALRasterProximityAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_tif, "-o", out_default, "-f", "GTiff"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_default.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GDT_Float32, ds->GetRasterBand(1)->GetRasterDataType());
    std::array<float, 16> out_vals{};
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 4, 4, out_vals.data(), 4, 4,
                           GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(0.0f, out_vals[0]);
    EXPECT_FLOAT_EQ(1.0f, out_vals[1]);
  }

  // 2. Explicit band, output-data-type, multiple target-values, geo units,
  // max-distance, fixed-value, nodata, and progress callback.
  {
    GDALRasterProximityAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-i",
                                               in_tif,
                                               "-o",
                                               out_opts,
                                               "-f",
                                               "GTiff",
                                               "--band",
                                               "2",
                                               "--output-data-type",
                                               "Float64",
                                               "--target-values",
                                               "1,2",
                                               "--distance-units",
                                               "geo",
                                               "--max-distance",
                                               "15",
                                               "--fixed-value",
                                               "50",
                                               "--nodata",
                                               "-1"}));
    bool progress_called = false;
    auto progress_cb = [](double, const char*, void* data) -> int {
      *static_cast<bool*>(data) = true;
      return 1;
    };
    EXPECT_TRUE(alg.Run(progress_cb, &progress_called));
    EXPECT_TRUE(progress_called);
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_opts.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GDT_Float64, ds->GetRasterBand(1)->GetRasterDataType());
    int has_nodata = FALSE;
    EXPECT_DOUBLE_EQ(-1.0, ds->GetRasterBand(1)->GetNoDataValue(&has_nodata));
    EXPECT_EQ(TRUE, has_nodata);

    std::array<double, 16> out_vals{};
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 4, 4, out_vals.data(), 4, 4,
                           GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(0.0, out_vals[0]);
    // Distance 10 <= max-distance 15 -> fixed-value 50.0; distance 20 > 15 ->
    // nodata -1.0.
    EXPECT_DOUBLE_EQ(50.0, out_vals[1]);
    EXPECT_DOUBLE_EQ(-1.0, out_vals[2]);
    EXPECT_DOUBLE_EQ(0.0, out_vals[15]);
  }

  // 3. Geo distance units without fixed-value reports actual geo distances.
  {
    const std::string out_geo = std::string(kTestDir) + "/out_geo.tif";
    GDALRasterProximityAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_tif, "-o", out_geo, "-f", "GTiff", "--target-values", "1",
         "--distance-units", "geo"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_geo.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::array<float, 16> out_vals{};
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 4, 4, out_vals.data(), 4, 4,
                           GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(0.0f, out_vals[0]);
    EXPECT_FLOAT_EQ(10.0f, out_vals[1]);
  }
}

TEST_F(GdalalgRasterProximityTest, ErrorAndPipelinePaths) {
  const std::string in_tif = std::string(kTestDir) + "/pipe_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/pipe_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateSourceRaster(in_tif));

  // 1. Progress callback returning 0 interrupts GDALComputeProximity.
  {
    GDALRasterProximityAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_tif, "-o", out_tif, "-f", "GTiff"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    auto cancel_cb = [](double, const char*, void*) -> int { return 0; };
    EXPECT_FALSE(alg.Run(cancel_cb, nullptr));
    CPLErrorReset();
  }

  // 2. Invalid arguments rejected at parse time.
  {
    GDALRasterProximityAlgorithmStandalone alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"-i", in_tif, "-o", out_tif, "--fixed-value", "-1"}));
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"-i", in_tif, "-o", out_tif, "--distance-units", "invalid"}));
    CPLErrorReset();
  }

  // 3. Pipeline execution with on-disk temp dataset.
  {
    CPLSetConfigOption("GDAL_RASTER_PIPELINE_USE_GTIFF_FOR_TEMP_DATASET",
                       "YES");
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "proximity", "--target-values", "1", "!", "write",
         out_tif, "-f", "GTiff", "--overwrite"}));
    EXPECT_TRUE(pipeline.Run());
    ASSERT_TRUE(pipeline.Finalize());
  }
}

}  // namespace
}  // namespace autotest2
