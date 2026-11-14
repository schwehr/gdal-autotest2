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
// Tests for "select" step of "raster pipeline"
// (apps/gdalalg_raster_select.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_select.cpp

#include "third_party/gdal/apps/gdalalg_raster_select.h"

#include <cstdint>
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

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_select_test";
constexpr char kTmpDir[] = "/vsimem/gdalalg_raster_select_tmp";

class GdalalgRasterSelectTest : public ::testing::Test {
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

std::unique_ptr<GDALDataset> CreateThreeBandMemDataset() {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  auto ds =
      std::unique_ptr<GDALDataset>(drv->Create("", 2, 2, 3, GDT_Byte, nullptr));
  if (!ds) return nullptr;
  for (int b = 1; b <= 3; ++b) {
    std::vector<uint8_t> vals(4, static_cast<uint8_t>(b * 10));
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 2, 2, vals.data(),
                                             2, 2, GDT_Byte, 0, 0, nullptr));
  }
  return ds;
}

TEST_F(GdalalgRasterSelectTest, ValidationErrors) {
  auto src_ds = CreateThreeBandMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  // 1. Missing required band argument.
  {
    GDALRasterSelectAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments({}));
  }

  // 2. Invalid band specification (0, -1, non-numeric).
  for (const char* bad_band : {"0", "-1", "invalid"}) {
    GDALRasterSelectAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments({"--band", bad_band}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid band specification"));
  }

  // 3. Invalid mask specification.
  for (const char* bad_mask : {"0", "-2", "invalid"}) {
    GDALRasterSelectAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--band", "1", "--mask", bad_mask}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid mask band specification"));
  }

  // 4. Out-of-range band index fails during Run.
  {
    GDALRasterSelectAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--band", "99"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
  }
}

TEST_F(GdalalgRasterSelectTest, SelectSubsetReorderAndMask) {
  // 1. Reorder bands 3 and 1 on MEM dataset.
  {
    auto src_ds = CreateThreeBandMemDataset();
    ASSERT_THAT(src_ds, NotNull());

    GDALRasterSelectAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--band", "3,1"}));
    ASSERT_TRUE(alg.Run());

    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(2, out_ds->GetRasterCount());

    uint8_t val1 = 0;
    uint8_t val2 = 0;
    ASSERT_EQ(CE_None,
              out_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &val1, 1,
                                                 1, GDT_Byte, 0, 0, nullptr));
    ASSERT_EQ(CE_None,
              out_ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 1, 1, &val2, 1,
                                                 1, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(30, val1);
    EXPECT_EQ(10, val2);
  }

  // 2. Select mask:1 as a data band and set --mask none.
  const std::string masked_tif = std::string(kTestDir) + "/masked.tif";
  {
    GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_THAT(gtiff_drv, NotNull());
    std::unique_ptr<GDALDataset> ds(
        gtiff_drv->Create(masked_tif.c_str(), 2, 2, 2, GDT_Byte, nullptr));
    ASSERT_THAT(ds, NotNull());
    std::vector<uint8_t> b1(4, 15);
    std::vector<uint8_t> b2(4, 25);
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, b1.data(), 2,
                                             2, GDT_Byte, 0, 0, nullptr));
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 2, 2, b2.data(), 2,
                                             2, GDT_Byte, 0, 0, nullptr));
    ASSERT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
    std::vector<uint8_t> m(4, 255);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->GetMaskBand()->RasterIO(
                           GF_Write, 0, 0, 2, 2, m.data(), 2, 2, GDT_Byte, 0, 0,
                           nullptr));
  }

  {
    std::unique_ptr<GDALDataset> src_ds(
        GDALDataset::Open(masked_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_THAT(src_ds, NotNull());

    GDALRasterSelectAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--band", "1,mask:1", "--mask", "none"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(2, out_ds->GetRasterCount());
    uint8_t mask_pixel = 0;
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(2)->RasterIO(
                           GF_Read, 0, 0, 1, 1, &mask_pixel, 1, 1, GDT_Byte, 0,
                           0, nullptr));
    EXPECT_EQ(255, mask_pixel);
  }

  // 3. Use band 2 as mask via --mask 2.
  {
    auto src_ds = CreateThreeBandMemDataset();
    ASSERT_THAT(src_ds, NotNull());
    GDALRasterSelectAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--band", "1", "--mask", "2"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(1, out_ds->GetRasterCount());
    EXPECT_EQ(GMF_PER_DATASET, out_ds->GetRasterBand(1)->GetMaskFlags());
  }
}

TEST_F(GdalalgRasterSelectTest, StandaloneAndPipeline) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  const std::string pipe_tif = std::string(kTestDir) + "/pipe.tif";

  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(gtiff_drv, NotNull());
  {
    std::unique_ptr<GDALDataset> ds(
        gtiff_drv->Create(in_tif.c_str(), 2, 2, 2, GDT_Byte, nullptr));
    ASSERT_THAT(ds, NotNull());
    std::vector<uint8_t> b1(4, 11);
    std::vector<uint8_t> b2(4, 22);
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, b1.data(), 2,
                                             2, GDT_Byte, 0, 0, nullptr));
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 2, 2, b2.data(), 2,
                                             2, GDT_Byte, 0, 0, nullptr));
  }

  {
    GDALRasterSelectAlgorithmStandalone alg;
    EXPECT_EQ("select", alg.GetName());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--band", "2", "--overwrite", in_tif, out_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments({"read", in_tif, "!", "select",
                                                  "-b", "2", "!", "write",
                                                  pipe_tif, "--overwrite"}));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(pipe_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(1, ds->GetRasterCount());
  uint8_t val = 0;
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                           GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(22, val);
}

}  // namespace
}  // namespace autotest2
