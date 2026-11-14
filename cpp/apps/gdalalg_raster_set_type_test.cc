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
// Tests for "set-type" step of "raster pipeline"
// (apps/gdalalg_raster_set_type.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_set_type.cpp

#include "third_party/gdal/apps/gdalalg_raster_set_type.h"

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

using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_set_type_test";
constexpr char kTmpDir[] = "/vsimem/gdalalg_raster_set_type_tmp";

class GdalalgRasterSetTypeTest : public ::testing::Test {
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

std::unique_ptr<GDALDataset> CreateByteMemDataset() {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  auto ds =
      std::unique_ptr<GDALDataset>(drv->Create("", 2, 2, 1, GDT_Byte, nullptr));
  if (!ds) return nullptr;
  std::vector<uint8_t> pixels = {1, 2, 3, 4};
  EXPECT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, pixels.data(),
                                           2, 2, GDT_Byte, 0, 0, nullptr));
  return ds;
}

TEST_F(GdalalgRasterSetTypeTest, ValidationErrors) {
  auto src_ds = CreateByteMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  // Missing required --datatype.
  {
    GDALRasterSetTypeAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments({}));
  }

  // Unknown --datatype.
  {
    GDALRasterSetTypeAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments({"--datatype", "NotAType"}));
  }
}

TEST_F(GdalalgRasterSetTypeTest, ConvertDataTypes) {
  auto src_ds = CreateByteMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  struct TypeCase {
    const char* name;
    GDALDataType expected_type;
  };
  constexpr TypeCase kCases[] = {
      {.name = "UInt16", .expected_type = GDT_UInt16},
      {.name = "Int16", .expected_type = GDT_Int16},
      {.name = "Float32", .expected_type = GDT_Float32},
      {.name = "Float64", .expected_type = GDT_Float64},
  };

  for (const auto& tc : kCases) {
    GDALRasterSetTypeAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--datatype", tc.name}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(tc.expected_type, out_ds->GetRasterBand(1)->GetRasterDataType());

    double vals[4] = {};
    ASSERT_EQ(CE_None,
              out_ds->GetRasterBand(1)->RasterIO(
                  GF_Read, 0, 0, 2, 2, vals, 2, 2, GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(1.0, vals[0]);
    EXPECT_DOUBLE_EQ(4.0, vals[3]);
  }
}

TEST_F(GdalalgRasterSetTypeTest, StandaloneAndPipeline) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  const std::string pipe_tif = std::string(kTestDir) + "/pipe.tif";

  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(gtiff_drv, NotNull());
  {
    std::unique_ptr<GDALDataset> ds(
        gtiff_drv->Create(in_tif.c_str(), 2, 2, 1, GDT_Byte, nullptr));
    ASSERT_THAT(ds, NotNull());
    std::vector<uint8_t> vals = {10, 20, 30, 40};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, vals.data(),
                                             2, 2, GDT_Byte, 0, 0, nullptr));
  }

  {
    GDALRasterSetTypeAlgorithmStandalone alg;
    EXPECT_EQ("set-type", alg.GetName());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--datatype", "Float32", "--overwrite", in_tif, out_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_tif, "!", "set-type", "--datatype", "Int16", "!", "write",
       pipe_tif, "--overwrite"}));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(pipe_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(GDT_Int16, ds->GetRasterBand(1)->GetRasterDataType());
}

}  // namespace
}  // namespace autotest2
