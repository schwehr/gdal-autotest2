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
// Tests for GDALRasterAspectAlgorithm (apps/gdalalg_raster_aspect.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_aspect.cpp

#include "third_party/gdal/apps/gdalalg_raster_aspect.h"

#include <cmath>
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
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_aspect_test";

void CreateDemTiff(const std::string& path, int bands = 1) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 5, 5, bands, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALGeoTransform gt(0.0, 10.0, 0.0, 50.0, 0.0, -10.0);
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

  // Sloped surface: higher in the north (top rows), lower in the south.
  std::vector<float> pixels(25);
  for (int y = 0; y < 5; ++y) {
    for (int x = 0; x < 5; ++x) {
      pixels[y * 5 + x] = static_cast<float>((4 - y) * 10 + x * 2);
    }
  }
  for (int b = 1; b <= bands; ++b) {
    ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                           GF_Write, 0, 0, 5, 5, pixels.data(), 5, 5,
                           GDT_Float32, 0, 0, nullptr));
  }
}

void CreateFlatTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 5, 5, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALGeoTransform gt(0.0, 10.0, 0.0, 50.0, 0.0, -10.0);
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  std::vector<float> pixels(25, 100.0f);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 5, 5, pixels.data(),
                                           5, 5, GDT_Float32, 0, 0, nullptr));
}

class GdalalgRasterAspectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterAspectTest, StandaloneProperties) {
  GDALRasterAspectAlgorithmStandalone alg;
  EXPECT_EQ("aspect", alg.GetName());
  EXPECT_EQ("Generate an aspect map", alg.GetDescription());
}

TEST_F(GdalalgRasterAspectTest, StandaloneDefaultsAndOptions) {
  const std::string in_tif = std::string(kTestDir) + "/dem.tif";
  const std::string out_default = std::string(kTestDir) + "/out_default.tif";
  const std::string out_trig = std::string(kTestDir) + "/out_trig.tif";
  ASSERT_NO_FATAL_FAILURE(CreateDemTiff(in_tif, 2));

  // 1. Default options (azimuth, Horn, compute edges).
  {
    GDALRasterAspectAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_default, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_default.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(5, out_ds->GetRasterXSize());
    EXPECT_EQ(5, out_ds->GetRasterYSize());
    EXPECT_EQ(1, out_ds->GetRasterCount());
    std::vector<float> buf(25, -9999.0f);
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 5, 5, buf.data(), 5, 5, GDT_Float32,
                           0, 0, nullptr));
    // Edge interpolation enabled by default, so corner pixel is not nodata.
    EXPECT_NE(-9999.0f, buf[0]);
    EXPECT_GE(buf[12], 0.0f);
    EXPECT_LE(buf[12], 360.0f);
  }

  // 2. Trigonometric angle, ZevenbergenThorne, band 2, no-edges.
  {
    GDALRasterAspectAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--band", "2", "--convention", "trigonometric-angle", "--gradient-alg",
         "ZevenbergenThorne", "--no-edges", in_tif, out_trig, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_trig.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, out_ds);
    std::vector<float> buf(25, 0.0f);
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 5, 5, buf.data(), 5, 5, GDT_Float32,
                           0, 0, nullptr));
    // With --no-edges, edge pixels receive nodata (-9999).
    EXPECT_FLOAT_EQ(-9999.0f, buf[0]);
    // Center pixel has a valid trigonometric angle.
    EXPECT_GE(buf[12], 0.0f);
    EXPECT_LE(buf[12], 360.0f);
  }
}

TEST_F(GdalalgRasterAspectTest, ZeroForFlat) {
  const std::string flat_tif = std::string(kTestDir) + "/flat.tif";
  const std::string out_flat = std::string(kTestDir) + "/out_flat.tif";
  ASSERT_NO_FATAL_FAILURE(CreateFlatTiff(flat_tif));

  GDALRasterAspectAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"--zero-for-flat", flat_tif, out_flat, "--overwrite"}));
  EXPECT_TRUE(alg.Run());
  EXPECT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_flat.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(nullptr, out_ds);
  std::vector<float> buf(25, -1.0f);
  ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, 5, 5, buf.data(), 5, 5, GDT_Float32, 0,
                         0, nullptr));
  EXPECT_FLOAT_EQ(0.0f, buf[12]);
}

TEST_F(GdalalgRasterAspectTest, PipelineExecution) {
  const std::string in_tif = std::string(kTestDir) + "/pipe_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/pipe_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateDemTiff(in_tif));

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_tif, "!", "aspect", "--convention", "azimuth", "!", "write",
       out_tif, "--overwrite"}));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(nullptr, out_ds);
  EXPECT_EQ(5, out_ds->GetRasterXSize());
  EXPECT_EQ(5, out_ds->GetRasterYSize());
}

TEST_F(GdalalgRasterAspectTest, InvalidArgumentsAndRunError) {
  const std::string in_tif = std::string(kTestDir) + "/err_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/err_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateDemTiff(in_tif));

  // 1. Invalid convention choice.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterAspectAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--convention", "invalid", in_tif, out_tif}));
  }

  // 2. Invalid gradient-alg choice.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterAspectAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--gradient-alg", "invalid", in_tif, out_tif}));
  }

  // 3. Out-of-range band is rejected during standalone argument validation.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterAspectAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--band", "99", "-f", "MEM", in_tif, "mem_out"}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Value of 'band'"));
    CPLErrorReset();
  }

  // 4. Out-of-range band in a pipeline fails during Run().
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "aspect", "--band", "99", "!", "write", "-f",
         "MEM", "mem_out"}));
    EXPECT_FALSE(pipeline.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Value of 'band'"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
