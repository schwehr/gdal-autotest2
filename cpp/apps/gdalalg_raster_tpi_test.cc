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
// Tests for GDALRasterTPIAlgorithm (apps/gdalalg_raster_tpi.cpp and
// apps/gdalalg_raster_tpi.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_tpi.cpp

#include "third_party/gdal/apps/gdalalg_raster_tpi.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
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

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_tpi_test";

void CreateDemTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 3, 3, 2, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);

  std::array<double, 6> gt = {0.0, 1.0, 0.0, 3.0, 0.0, -1.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt.data()));

  // Band 1: center = 10, neighbors = 0 -> TPI at center = 10.
  std::array<float, 9> b1 = {0, 0, 0, 0, 10, 0, 0, 0, 0};
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 3, 3, b1.data(), 3,
                                           3, GDT_Float32, 0, 0, nullptr));

  // Band 2: center = 9, neighbors = 1 -> TPI at center = 8.
  std::array<float, 9> b2 = {1, 1, 1, 1, 9, 1, 1, 1, 1};
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 3, 3, b2.data(), 3,
                                           3, GDT_Float32, 0, 0, nullptr));
}

TEST(GdalalgRasterTpiTest, StandaloneComputeEdgesAndNoEdges) {
  GDALRegister_GTiff();
  GDALRegister_MEM();
  GDALRegister_VRT();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string in_tif = std::string(kTestDir) + "/dem.tif";
  const std::string out_edges = std::string(kTestDir) + "/tpi_edges.tif";
  const std::string out_no_edges = std::string(kTestDir) + "/tpi_noedges.tif";
  ASSERT_NO_FATAL_FAILURE(CreateDemTiff(in_tif));

  // 1. Default (compute edges) on band 1.
  {
    GDALRasterTPIAlgorithmStandalone alg;
    EXPECT_EQ("tpi", alg.GetName());
    EXPECT_FALSE(alg.GetDescription().empty());
    EXPECT_FALSE(alg.GetHelpURL().empty());

    ASSERT_TRUE(alg.ParseCommandLineArguments({in_tif, out_edges}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(GDALDataset::Open(
        out_edges.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, out_ds);
    std::array<float, 9> pixels{};
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 3, 3, pixels.data(), 3, 3,
                           GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(10.0f, pixels[4]);
    // Edge pixel (0,0) has finite interpolated value when compute_edges is on.
    EXPECT_FALSE(std::isnan(pixels[0]));
  }

  // 2. --no-edges and --band=2.
  {
    GDALRasterTPIAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--no-edges", "--band=2", in_tif, out_no_edges}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(GDALDataset::Open(
        out_no_edges.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, out_ds);
    int has_nd = 0;
    const double nd = out_ds->GetRasterBand(1)->GetNoDataValue(&has_nd);
    EXPECT_EQ(1, has_nd);
    std::array<float, 9> pixels{};
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 3, 3, pixels.data(), 3, 3,
                           GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(8.0f, pixels[4]);
    EXPECT_DOUBLE_EQ(nd, pixels[0]);
  }
}

TEST(GdalalgRasterTpiTest, PipelineAndInvalidBandError) {
  GDALRegister_GTiff();
  GDALRegister_MEM();
  GDALRegister_VRT();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string in_tif = std::string(kTestDir) + "/pipe_dem.tif";
  const std::string out_tif = std::string(kTestDir) + "/pipe_tpi.tif";
  ASSERT_NO_FATAL_FAILURE(CreateDemTiff(in_tif));

  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments({"read", in_tif, "!", "tpi",
                                                    "--band=2", "!", "write",
                                                    out_tif, "--overwrite"}));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, out_ds);
    float center = 0.0f;
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 1, 1, 1, 1, &center, 1, 1, GDT_Float32, 0,
                           0, nullptr));
    EXPECT_FLOAT_EQ(8.0f, center);
  }

  // Invalid band triggers ValidateBandArg failure during parsing.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTPIAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--band=9", in_tif, "unused"}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
