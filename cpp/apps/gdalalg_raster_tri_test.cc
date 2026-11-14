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
// Tests for GDALRasterTRIAlgorithm (apps/gdalalg_raster_tri.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_tri.cpp

#include "third_party/gdal/apps/gdalalg_raster_tri.h"

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

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_tri_test";

void CreateDemTiff(const std::string& path, int bands = 1) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 5, 5, bands, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALGeoTransform gt(0.0, 10.0, 0.0, 50.0, 0.0, -10.0);
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

  std::vector<float> pixels(25, 10.0f);
  // Peak at the center pixel (2, 2).
  pixels[12] = 50.0f;
  for (int b = 1; b <= bands; ++b) {
    ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                           GF_Write, 0, 0, 5, 5, pixels.data(), 5, 5,
                           GDT_Float32, 0, 0, nullptr));
  }
}

class GdalalgRasterTriTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterTriTest, StandaloneProperties) {
  GDALRasterTRIAlgorithmStandalone alg;
  EXPECT_EQ("tri", alg.GetName());
  EXPECT_EQ("Generate a Terrain Ruggedness Index (TRI) map",
            alg.GetDescription());
}

TEST_F(GdalalgRasterTriTest, RileyAndWilsonAlgorithms) {
  const std::string in_tif = std::string(kTestDir) + "/dem.tif";
  const std::string out_riley = std::string(kTestDir) + "/riley.tif";
  const std::string out_wilson = std::string(kTestDir) + "/wilson.tif";
  ASSERT_NO_FATAL_FAILURE(CreateDemTiff(in_tif, 2));

  // 1. Default Riley algorithm with edges computed.
  {
    GDALRasterTRIAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_riley, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_riley.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, out_ds);
    std::vector<float> buf(25, -9999.0f);
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 5, 5, buf.data(), 5, 5, GDT_Float32,
                           0, 0, nullptr));
    EXPECT_FLOAT_EQ(0.0f, buf[0]);
    EXPECT_GT(buf[12], 0.0f);
  }

  // 2. Wilson algorithm, band 2, --no-edges.
  {
    GDALRasterTRIAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--band", "2", "--algorithm",
                                               "Wilson", "--no-edges", in_tif,
                                               out_wilson, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_wilson.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, out_ds);
    std::vector<float> buf(25, 0.0f);
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 5, 5, buf.data(), 5, 5, GDT_Float32,
                           0, 0, nullptr));
    // Edge pixels receive nodata (-9999) when --no-edges is set.
    EXPECT_FLOAT_EQ(-9999.0f, buf[0]);
    EXPECT_GT(buf[12], 0.0f);
  }
}

TEST_F(GdalalgRasterTriTest, PipelineExecutionAndErrors) {
  const std::string in_tif = std::string(kTestDir) + "/pipe_dem.tif";
  const std::string out_tif = std::string(kTestDir) + "/pipe_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateDemTiff(in_tif));

  {
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_tif, "!", "tri", "--algorithm", "Riley", "!", "write",
         out_tif, "--overwrite"}));
    EXPECT_TRUE(pipe.Run());
    EXPECT_TRUE(pipe.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(5, out_ds->GetRasterXSize());
  }

  // Invalid algorithm choice.
  {
    WithQuietHandler quiet;
    GDALRasterTRIAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--algorithm", "invalid", in_tif, out_tif}));
  }

  // Out-of-range band fails validation during ParseCommandLineArguments.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterTRIAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--band", "99", "-f", "MEM", in_tif, "mem_out"}));
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Out-of-range band fails Run() when invoked inside a raster pipeline.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments({"read", in_tif, "!", "tri",
                                                "--band", "99", "!", "write",
                                                out_tif, "--overwrite"}));
    EXPECT_FALSE(pipe.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
