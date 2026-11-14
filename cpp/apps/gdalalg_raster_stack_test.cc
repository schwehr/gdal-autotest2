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
// Tests for GDALRasterStackAlgorithm (apps/gdalalg_raster_stack.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_stack.cpp

#include "third_party/gdal/apps/gdalalg_raster_stack.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_stack_test";

class GdalalgRasterStackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateBandTiff(const std::string& path, uint8_t value) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 2, 2, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALGeoTransform gt{0.0, 1.0, 0.0, 2.0, 0.0, -1.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  std::array<uint8_t, 4> data = {value, value, value, value};
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, data.data(), 2,
                                           2, GDT_Byte, 0, 0, nullptr));
}

TEST_F(GdalalgRasterStackTest, Properties) {
  GDALRasterStackAlgorithmStandalone alg;
  EXPECT_EQ("stack", alg.GetName());
  EXPECT_TRUE(alg.CanBeFirstStep());
}

TEST_F(GdalalgRasterStackTest, StackByFilenames) {
  const std::string b1 = std::string(kTestDir) + "/b1.tif";
  const std::string b2 = std::string(kTestDir) + "/b2.tif";
  const std::string out_tif = std::string(kTestDir) + "/stacked.tif";
  ASSERT_NO_FATAL_FAILURE(CreateBandTiff(b1, 10));
  ASSERT_NO_FATAL_FAILURE(CreateBandTiff(b2, 20));

  GDALRasterStackAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"-f", "GTiff", "--overwrite", b1, b2, out_tif}));
  EXPECT_TRUE(alg.Run());
  ASSERT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  EXPECT_EQ(2, out_ds->GetRasterCount());

  std::array<uint8_t, 4> vals1{};
  std::array<uint8_t, 4> vals2{};
  ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, 2, 2, vals1.data(), 2, 2, GDT_Byte, 0,
                         0, nullptr));
  ASSERT_EQ(CE_None, out_ds->GetRasterBand(2)->RasterIO(
                         GF_Read, 0, 0, 2, 2, vals2.data(), 2, 2, GDT_Byte, 0,
                         0, nullptr));
  EXPECT_EQ(10, vals1[0]);
  EXPECT_EQ(20, vals2[0]);
}

TEST_F(GdalalgRasterStackTest, StackByDatasetReferences) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  std::unique_ptr<GDALDataset> ds1(
      mem_drv->Create("", 2, 2, 1, GDT_Byte, nullptr));
  std::unique_ptr<GDALDataset> ds2(
      mem_drv->Create("", 2, 2, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds1);
  ASSERT_NE(nullptr, ds2);
  GDALGeoTransform gt{0.0, 1.0, 0.0, 2.0, 0.0, -1.0};
  ds1->SetGeoTransform(gt);
  ds2->SetGeoTransform(gt);

  GDALRasterStackAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments({"-f", "stream", "in1", "out"}));
  auto* input_arg = alg.GetArg(GDAL_ARG_NAME_INPUT);
  ASSERT_NE(nullptr, input_arg);
  std::vector<GDALArgDatasetValue> refs(2);
  refs[0].Set(ds1.get());
  refs[1].Set(ds2.get());
  ASSERT_TRUE(input_arg->Set(std::move(refs)));

  EXPECT_TRUE(alg.Run());
  GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, out_ds);
  EXPECT_EQ(2, out_ds->GetRasterCount());
}

TEST_F(GdalalgRasterStackTest, ErrorPaths) {
  const std::string b1 = std::string(kTestDir) + "/ok.tif";
  const std::string out_tif = std::string(kTestDir) + "/err.tif";
  ASSERT_NO_FATAL_FAILURE(CreateBandTiff(b1, 5));

  // 1. GetInputDatasetNames failure via nonexistent @list.
  {
    GDALRasterStackAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "GTiff", "@/vsimem/gdalalg_raster_stack_test/no_such.txt",
         out_tif}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot open"));
    CPLErrorReset();
  }

  // 2. GDALBuildVRT failure via nonexistent raster input.
  {
    GDALRasterStackAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "GTiff", "/vsimem/gdalalg_raster_stack_test/missing.tif",
         out_tif}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterStackTest, PipelineUsage) {
  const std::string b1 = std::string(kTestDir) + "/p1.tif";
  const std::string b2 = std::string(kTestDir) + "/p2.tif";
  const std::string out_tif = std::string(kTestDir) + "/p_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateBandTiff(b1, 3));
  ASSERT_NO_FATAL_FAILURE(CreateBandTiff(b2, 7));

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"stack", b1, b2, "!", "write", out_tif, "-f", "GTiff", "--overwrite"}));
  EXPECT_TRUE(pipeline.Run());
  ASSERT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  EXPECT_EQ(2, out_ds->GetRasterCount());
  EXPECT_EQ("", pipeline.GetOutputString());
}

}  // namespace
}  // namespace autotest2
