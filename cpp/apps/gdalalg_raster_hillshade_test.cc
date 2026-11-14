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
// Tests for GDALRasterHillshadeAlgorithm (apps/gdalalg_raster_hillshade.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_hillshade.cpp

#include "third_party/gdal/apps/gdalalg_raster_hillshade.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/apps/gdalalg_raster_write.h"
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

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_hillshade_test";

void CreateDemTiff(const std::string& path, int bands = 1) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 5, 5, bands, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALGeoTransform gt(0.0, 10.0, 0.0, 50.0, 0.0, -10.0);
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

  std::vector<float> pixels(25);
  for (int y = 0; y < 5; ++y) {
    for (int x = 0; x < 5; ++x) {
      pixels[y * 5 + x] = static_cast<float>(y * 10 + x * 5);
    }
  }
  for (int b = 1; b <= bands; ++b) {
    ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                           GF_Write, 0, 0, 5, 5, pixels.data(), 5, 5,
                           GDT_Float32, 0, 0, nullptr));
  }
}

class GdalalgRasterHillshadeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterHillshadeTest, PropertiesAndCanHandleNextStep) {
  GDALRasterHillshadeAlgorithmStandalone alg;
  EXPECT_EQ("hillshade", alg.GetName());
  EXPECT_EQ("Generate a shaded relief map", alg.GetDescription());

  GDALRasterWriteAlgorithm write_tif;
  ASSERT_TRUE(write_tif.ParseCommandLineArguments(
      {"--output-format", "GTiff", std::string(kTestDir) + "/unused.tif"}));
  EXPECT_TRUE(alg.CanHandleNextStep(&write_tif));

  GDALRasterWriteAlgorithm write_stream;
  ASSERT_TRUE(write_stream.ParseCommandLineArguments(
      {"--output-format", "stream", ""}));
  EXPECT_FALSE(alg.CanHandleNextStep(&write_stream));

  GDALRasterHillshadeAlgorithm other_step;
  EXPECT_FALSE(alg.CanHandleNextStep(&other_step));
}

TEST_F(GdalalgRasterHillshadeTest, PipelineDirectWriteAndStreaming) {
  const std::string in_tif = std::string(kTestDir) + "/dem.tif";
  const std::string out_direct = std::string(kTestDir) + "/out_direct.tif";
  ASSERT_NO_FATAL_FAILURE(CreateDemTiff(in_tif, 2));

  // 1. Direct-write path via CanHandleNextStep() with write step + CO.
  {
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments({"read",
                                                in_tif,
                                                "!",
                                                "hillshade",
                                                "--band",
                                                "2",
                                                "--zfactor",
                                                "2.0",
                                                "--xscale",
                                                "1.0",
                                                "--yscale",
                                                "1.0",
                                                "--azimuth",
                                                "300",
                                                "--altitude",
                                                "40",
                                                "--gradient-alg",
                                                "ZevenbergenThorne",
                                                "--no-edges",
                                                "!",
                                                "write",
                                                "--output-format",
                                                "GTiff",
                                                "--creation-option",
                                                "COMPRESS=NONE",
                                                out_direct,
                                                "--overwrite"}));
    auto progress = [](double, const char*, void*) -> int { return 1; };
    EXPECT_TRUE(pipe.Run(progress, nullptr));
    EXPECT_TRUE(pipe.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_direct.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(5, out_ds->GetRasterXSize());
    EXPECT_EQ(5, out_ds->GetRasterYSize());
    std::vector<uint8_t> buf(25, 0);
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 5, 5, buf.data(), 5, 5, GDT_Byte, 0,
                           0, nullptr));
    // With --no-edges, edge pixel is nodata (0).
    EXPECT_EQ(0, buf[0]);
  }

  // 2. Streaming path (no write step -> -of stream).
  {
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(
        pipe.ParseCommandLineArguments({"read", in_tif, "!", "hillshade"}));
    EXPECT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> buf(25, 0);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 5, buf.data(), 5,
                                             5, GDT_Byte, 0, 0, nullptr));
  }
}

TEST_F(GdalalgRasterHillshadeTest, VariantsCombinedMultidirectionalIgor) {
  const std::string in_tif = std::string(kTestDir) + "/var_dem.tif";
  const std::string out_combined = std::string(kTestDir) + "/combined.tif";
  const std::string out_multi = std::string(kTestDir) + "/multi.tif";
  const std::string out_igor = std::string(kTestDir) + "/igor.tif";
  ASSERT_NO_FATAL_FAILURE(CreateDemTiff(in_tif));

  {
    GDALRasterHillshadeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--variant", "combined", in_tif, out_combined, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    GDALRasterHillshadeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--variant", "multidirectional", in_tif, out_multi, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    GDALRasterHillshadeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--variant", "Igor", in_tif, out_igor, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  for (const std::string& p : {out_combined, out_multi, out_igor}) {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(p.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(5, out_ds->GetRasterXSize());
  }
}

TEST_F(GdalalgRasterHillshadeTest, ErrorPaths) {
  const std::string in_tif = std::string(kTestDir) + "/err_dem.tif";
  const std::string out_tif = std::string(kTestDir) + "/err_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateDemTiff(in_tif));

  // 1. multidirectional with explicit azimuth fails in Run().
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterHillshadeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--variant", "multidirectional",
                                               "--azimuth", "180", "-f", "MEM",
                                               in_tif, ""}));
    EXPECT_FALSE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Igor with explicit altitude fails in Run().
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterHillshadeAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--variant", "Igor", "--altitude", "30", "-f", "MEM", in_tif, ""}));
    EXPECT_FALSE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 3. Invalid band fails band validation during ParseCommandLineArguments.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterHillshadeAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--band", "99", "-f", "MEM", in_tif, ""}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 4. Out-of-range altitude / zfactor / variant rejected at parse time.
  {
    WithQuietHandler quiet;
    GDALRasterHillshadeAlgorithmStandalone alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--altitude", "95", in_tif, out_tif}));
  }
  {
    WithQuietHandler quiet;
    GDALRasterHillshadeAlgorithmStandalone alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--zfactor", "0", in_tif, out_tif}));
  }
  {
    WithQuietHandler quiet;
    GDALRasterHillshadeAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--variant", "invalid", in_tif, out_tif}));
  }
}

}  // namespace
}  // namespace autotest2
