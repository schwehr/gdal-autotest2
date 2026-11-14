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
// Tests for GDALRasterSlopeAlgorithm (apps/gdalalg_raster_slope.cpp and
// apps/gdalalg_raster_slope.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_slope.cpp

#include "third_party/gdal/apps/gdalalg_raster_slope.h"

#include <cmath>
#include <memory>
#include <numbers>
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
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_slope_test";

std::unique_ptr<GDALDataset> CreateRampDem(const std::string& path = "") {
  GDALDriver* drv =
      GetGDALDriverManager()->GetDriverByName(path.empty() ? "MEM" : "GTiff");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 5, 5, 1, GDT_Float32, nullptr));
  if (!ds) return nullptr;
  double gt[6] = {0.0, 1.0, 0.0, 5.0, 0.0, -1.0};
  if (ds->SetGeoTransform(gt) != CE_None) return nullptr;

  std::vector<float> dem(25);
  for (int y = 0; y < 5; ++y) {
    for (int x = 0; x < 5; ++x) {
      dem[y * 5 + x] = static_cast<float>(x * 10.0);
    }
  }
  if (ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 5, 5, dem.data(), 5, 5,
                                     GDT_Float32, 0, 0, nullptr) != CE_None) {
    return nullptr;
  }
  return ds;
}

class GdalalgRasterSlopeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterSlopeTest, PropertiesAndArgumentValidation) {
  GDALRasterSlopeAlgorithm alg;
  EXPECT_EQ("slope", alg.GetName());
  EXPECT_STREQ("Generate a slope map", alg.GetDescription().c_str());
  EXPECT_STREQ("/programs/gdal_raster_slope.html", alg.GetHelpURL().c_str());

  // Invalid unit, gradient-alg, and non-positive xscale/yscale fail.
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"--unit", "radian"}));
  }
  {
    GDALRasterSlopeAlgorithm bad;
    WithQuietHandler quiet;
    EXPECT_FALSE(bad.ParseCommandLineArguments({"--gradient-alg", "Unknown"}));
  }
  {
    GDALRasterSlopeAlgorithm bad;
    WithQuietHandler quiet;
    EXPECT_FALSE(bad.ParseCommandLineArguments({"--xscale", "0"}));
  }
  {
    GDALRasterSlopeAlgorithm bad;
    WithQuietHandler quiet;
    EXPECT_FALSE(bad.ParseCommandLineArguments({"--yscale", "-1"}));
  }
}

TEST_F(GdalalgRasterSlopeTest, DefaultDegreesAndHornComputeEdges) {
  auto src = CreateRampDem();
  ASSERT_NE(nullptr, src);

  GDALRasterSlopeAlgorithm alg;
  alg.GetInputDatasets().emplace_back(src.get());
  ASSERT_TRUE(alg.ParseCommandLineArguments({}));
  EXPECT_TRUE(alg.Run());

  GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, out);
  EXPECT_EQ(5, out->GetRasterXSize());
  EXPECT_EQ(5, out->GetRasterYSize());

  std::vector<float> slope(25, -1.0f);
  ASSERT_EQ(CE_None,
            out->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 5, slope.data(),
                                            5, 5, GDT_Float32, 0, 0, nullptr));
  // Center pixel slope: dz/dx = 10, atan(10) in degrees.
  const double expected_deg = std::atan(10.0) * 180.0 / std::numbers::pi;
  EXPECT_NEAR(expected_deg, slope[2 * 5 + 2], 1e-3);
  // Edges are computed by default (not nodata -9999); corner (0,0) uses
  // one-sided difference dz/dx = 5, giving atan(5) degrees.
  EXPECT_NE(-9999.0f, slope[0]);
  EXPECT_NEAR(std::atan(5.0) * 180.0 / std::numbers::pi, slope[0], 1e-3);
}

TEST_F(GdalalgRasterSlopeTest, PercentScalesZevenbergenThorneAndNoEdges) {
  auto src = CreateRampDem();
  ASSERT_NE(nullptr, src);

  GDALRasterSlopeAlgorithm alg;
  alg.GetInputDatasets().emplace_back(src.get());
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"--band", "1", "--unit", "percent", "--xscale", "2", "--yscale", "2",
       "--gradient-alg", "ZevenbergenThorne", "--no-edges"}));
  EXPECT_TRUE(alg.Run());

  GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, out);

  int has_nodata = FALSE;
  const double nodata = out->GetRasterBand(1)->GetNoDataValue(&has_nodata);
  EXPECT_TRUE(has_nodata);

  std::vector<float> slope(25, 0.0f);
  ASSERT_EQ(CE_None,
            out->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 5, slope.data(),
                                            5, 5, GDT_Float32, 0, 0, nullptr));
  // With xscale=2, dx=10 becomes 5 -> 500% slope at center, nodata at border.
  EXPECT_NEAR(500.0, slope[2 * 5 + 2], 1e-2);
  EXPECT_FLOAT_EQ(static_cast<float>(nodata), slope[0]);
}

TEST_F(GdalalgRasterSlopeTest, StandaloneVrtRejectedAndPipelineExecution) {
  const std::string in_tif = std::string(kTestDir) + "/dem.tif";
  const std::string out_tif = std::string(kTestDir) + "/slope.tif";
  const std::string out_vrt = std::string(kTestDir) + "/slope.vrt";
  ASSERT_NE(nullptr, CreateRampDem(in_tif));

  // VRT output is rejected because SetOutputVRTCompatible(false).
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterSlopeAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({in_tif, out_vrt}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("VRT output is not supported"));
    CPLErrorReset();
  }

  // Standalone GTiff output succeeds.
  {
    GDALRasterSlopeAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--overwrite", in_tif, out_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // Pipeline execution succeeds.
  {
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_tif, "!", "slope", "--unit", "percent", "!", "write",
         out_tif, "--overwrite"}));
    EXPECT_TRUE(pipe.Run());
    EXPECT_TRUE(pipe.Finalize());
  }
}

}  // namespace
}  // namespace autotest2
