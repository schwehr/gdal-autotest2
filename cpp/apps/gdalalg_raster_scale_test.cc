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
// Tests for "scale" step of "raster pipeline"
// (apps/gdalalg_raster_scale.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_scale.cpp

#include "third_party/gdal/apps/gdalalg_raster_scale.h"

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
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_scale_test";
constexpr char kTmpDir[] = "/vsimem/gdalalg_raster_scale_tmp";

class GdalalgRasterScaleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    ASSERT_EQ(0, VSIMkdir(kTmpDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTmpDir);
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
    VSIRmdirRecursive(kTmpDir);
    CPLErrorReset();
  }
};

std::unique_ptr<GDALDataset> CreateMemRaster(int bands) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create("", 4, 1, bands, GDT_Float32, nullptr));
  if (!ds) return nullptr;
  float vals[4] = {0.0f, 50.0f, 100.0f, 150.0f};
  for (int b = 1; b <= bands; ++b) {
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 4, 1, vals, 4, 1,
                                             GDT_Float32, 0, 0, nullptr));
  }
  return ds;
}

TEST_F(GdalalgRasterScaleTest, ValidationErrorsForIncompleteRanges) {
  auto src_ds = CreateMemRaster(1);
  ASSERT_THAT(src_ds, NotNull());

  struct ErrorCase {
    std::vector<std::string> args;
    const char* expected_msg;
  };
  const ErrorCase cases[] = {
      {.args = {"--src-min", "0"},
       .expected_msg = "src-max must be specified when src-min is specified"},
      {.args = {"--src-max", "100"},
       .expected_msg = "src-min must be specified when src-max is specified"},
      {.args = {"--dst-min", "0"},
       .expected_msg = "dst-max must be specified when dst-min is specified"},
      {.args = {"--dst-max", "255"},
       .expected_msg = "dst-min must be specified when dst-max is specified"},
  };

  for (const auto& tc : cases) {
    GDALRasterScaleAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(tc.args));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(tc.expected_msg));
  }
}

TEST_F(GdalalgRasterScaleTest, ScaleWithExplicitRangesExponentAndNoClip) {
  auto src_ds = CreateMemRaster(2);
  ASSERT_THAT(src_ds, NotNull());

  // 1. Explicit src/dst range, band=1, datatype=Float32, exponent=2.
  {
    GDALRasterScaleAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--datatype", "Float32", "--band", "1", "--src-min", "0", "--src-max",
         "100", "--dst-min", "0", "--dst-max", "10", "--exponent", "2"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(GDT_Float32, out_ds->GetRasterBand(1)->GetRasterDataType());

    float out[4] = {};
    ASSERT_EQ(CE_None,
              out_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 1, out, 4, 1,
                                                 GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(0.0f, out[0]);
    EXPECT_FLOAT_EQ(2.5f, out[1]);
    EXPECT_FLOAT_EQ(10.0f, out[2]);
    // Clipped at src-max=100 -> 10.0.
    EXPECT_FLOAT_EQ(10.0f, out[3]);
  }

  // 2. --dst-min / --dst-max without --src-min / --src-max, plus --no-clip.
  {
    GDALRasterScaleAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--dst-min", "0", "--dst-max", "30", "--no-clip"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    float out[4] = {};
    ASSERT_EQ(CE_None,
              out_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 1, out, 4, 1,
                                                 GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(0.0f, out[0]);
    EXPECT_FLOAT_EQ(10.0f, out[1]);
    EXPECT_FLOAT_EQ(20.0f, out[2]);
    EXPECT_FLOAT_EQ(30.0f, out[3]);
  }

  // 3. No-clip with src range so 150 scales past dst-max.
  {
    GDALRasterScaleAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--src-min", "0", "--src-max", "100", "--dst-min", "0", "--dst-max",
         "10", "--no-clip"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    float out[4] = {};
    ASSERT_EQ(CE_None,
              out_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 1, out, 4, 1,
                                                 GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(15.0f, out[3]);
  }
}

TEST_F(GdalalgRasterScaleTest, StandaloneAndPipeline) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  const std::string pipe_tif = std::string(kTestDir) + "/pipe.tif";

  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(gtiff_drv, NotNull());
  {
    std::unique_ptr<GDALDataset> ds(
        gtiff_drv->Create(in_tif.c_str(), 2, 2, 1, GDT_Float32, nullptr));
    ASSERT_THAT(ds, NotNull());
    float vals[4] = {0.0f, 1.0f, 2.0f, 3.0f};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, vals, 2, 2,
                                             GDT_Float32, 0, 0, nullptr));
  }

  // Standalone.
  {
    GDALRasterScaleAlgorithmStandalone alg;
    EXPECT_EQ("scale", alg.GetName());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--overwrite", in_tif, out_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // Pipeline.
  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_tif, "!", "scale", "--src-min", "0", "--src-max", "3",
       "--dst-min", "0", "--dst-max", "30", "!", "write", pipe_tif,
       "--overwrite"}));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(pipe_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  float out[4] = {};
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out, 2, 2,
                                           GDT_Float32, 0, 0, nullptr));
  EXPECT_FLOAT_EQ(0.0f, out[0]);
  EXPECT_FLOAT_EQ(30.0f, out[3]);
}

}  // namespace
}  // namespace autotest2
