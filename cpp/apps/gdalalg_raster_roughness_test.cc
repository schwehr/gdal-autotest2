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
// Tests for GDALRasterRoughnessAlgorithm (apps/gdalalg_raster_roughness.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_roughness.cpp

#include "third_party/gdal/apps/gdalalg_raster_roughness.h"

#include <cstdint>
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
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_roughness_test";

class GdalalgRasterRoughnessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateTwoBandDem(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 3, 3, 2, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);

  // Band 1: flat surface (all 10), except center is 20 -> roughness = 10.
  const std::vector<float> b1 = {
      10, 10, 10,  //
      10, 20, 10,  //
      10, 10, 10,  //
  };
  // Band 2: flat surface (all 5), except center is 12 -> roughness = 7.
  const std::vector<float> b2 = {
      5, 5,  5,  //
      5, 12, 5,  //
      5, 5,  5,  //
  };
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 3, 3, const_cast<float*>(b1.data()), 3,
                         3, GDT_Float32, 0, 0, nullptr));
  ASSERT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(
                         GF_Write, 0, 0, 3, 3, const_cast<float*>(b2.data()), 3,
                         3, GDT_Float32, 0, 0, nullptr));
}

TEST_F(GdalalgRasterRoughnessTest, BasicProperties) {
  GDALRasterRoughnessAlgorithm alg(false);
  EXPECT_EQ("roughness", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_raster_roughness.html", alg.GetHelpURL());

  GDALRasterRoughnessAlgorithmStandalone standalone;
  EXPECT_EQ("roughness", standalone.GetName());
}

TEST_F(GdalalgRasterRoughnessTest, StandaloneWithAndWithoutEdgesAndBand) {
  const std::string in_tif = std::string(kTestDir) + "/dem.tif";
  const std::string out_edges = std::string(kTestDir) + "/edges.tif";
  const std::string out_no_edges = std::string(kTestDir) + "/no_edges.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTwoBandDem(in_tif));

  // 1. Default computes edges on band 1.
  {
    GDALRasterRoughnessAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({in_tif, out_edges}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_edges.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    std::vector<float> vals(9, -1.0f);
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 3, 3, vals.data(),
                                             3, 3, GDT_Float32, 0, 0, nullptr));
    // Center pixel has max diff |20 - 10| = 10.
    // Corner pixel (0, 0) extrapolates edge values via 2*a - b = 2*10 - 20 = 0
    // across (0, 1) -> (1, 1), yielding max diff |20 - 0| = 20.
    EXPECT_FLOAT_EQ(10.0f, vals[4]);
    EXPECT_FLOAT_EQ(20.0f, vals[0]);
  }

  // 2. --no-edges and --band 2 sets edges to nodata and uses band 2.
  {
    GDALRasterRoughnessAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--no-edges", "--band", "2", in_tif, out_no_edges}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_no_edges.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    int has_nodata = 0;
    const double nodata = ds->GetRasterBand(1)->GetNoDataValue(&has_nodata);
    ASSERT_EQ(1, has_nodata);
    std::vector<float> vals(9, 0.0f);
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 3, 3, vals.data(),
                                             3, 3, GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(7.0f, vals[4]);
    EXPECT_FLOAT_EQ(static_cast<float>(nodata), vals[0]);
  }
}

TEST_F(GdalalgRasterRoughnessTest, PipelineAndInvalidBand) {
  const std::string in_tif = std::string(kTestDir) + "/pipe_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/pipe_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTwoBandDem(in_tif));

  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments({"read", in_tif, "!",
                                                    "roughness", "!", "write",
                                                    out_tif, "--overwrite"}));
    ASSERT_TRUE(pipeline.Run());
    ASSERT_TRUE(pipeline.Finalize());
  }

  // Invalid band fails.
  {
    GDALRasterRoughnessAlgorithmStandalone alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--band", "99", in_tif, out_tif}));
  }
}

}  // namespace
}  // namespace autotest2
