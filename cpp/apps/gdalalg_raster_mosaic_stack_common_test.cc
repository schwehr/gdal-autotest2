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
// Tests for GDALRasterMosaicStackCommonAlgorithm
// (apps/gdalalg_raster_mosaic_stack_common.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_mosaic_stack_common.cpp

#include "third_party/gdal/apps/gdalalg_raster_mosaic_stack_common.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_mosaic.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/apps/gdalalg_raster_stack.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
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

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_mosaic_stack_common_test";

class GdalalgRasterMosaicStackCommonTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateTile(const std::string& path, double origin_x, double origin_y,
                uint8_t val) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 2, 2, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALGeoTransform gt{origin_x, 1.0, 0.0, origin_y, 0.0, -1.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  std::array<uint8_t, 4> data = {val, val, val, val};
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, data.data(), 2,
                                           2, GDT_Byte, 0, 0, nullptr));
}

TEST_F(GdalalgRasterMosaicStackCommonTest, ResolutionValidation) {
  const std::string in_tif = std::string(kTestDir) + "/res_in.tif";
  const std::string out_vrt = std::string(kTestDir) + "/res_out.vrt";
  ASSERT_NO_FATAL_FAILURE(CreateTile(in_tif, 0.0, 2.0, 10));

  for (const char* valid_res :
       {"same", "average", "common", "highest", "lowest", "1,1"}) {
    GDALRasterStackAlgorithmStandalone alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"--resolution", valid_res, in_tif, out_vrt}));
  }

  for (const char* bad_res : {"1", "1,2,3", "abc,1", "1,abc", "0,1", "1,-2"}) {
    GDALRasterStackAlgorithmStandalone alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--resolution", bad_res, in_tif, out_vrt}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("resolution:"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterMosaicStackCommonTest, InputDatasetNameResolutionModes) {
  const std::string tile1 = std::string(kTestDir) + "/glob_1.tif";
  const std::string tile2 = std::string(kTestDir) + "/glob_2.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTile(tile1, 0.0, 2.0, 11));
  ASSERT_NO_FATAL_FAILURE(CreateTile(tile2, 2.0, 2.0, 22));

  // 1. @filename list input.
  {
    const std::string list_file = std::string(kTestDir) + "/inputs.txt";
    const std::string out_vrt = std::string(kTestDir) + "/from_list.vrt";
    VsiMemTempWrapper list_wrapper(list_file, tile1 + "\n" + tile2 + "\n");

    GDALRasterMosaicAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--overwrite", "@" + list_file, out_vrt}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_vrt.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterXSize());
  }

  // 2. Missing @filename triggers FileIO error.
  {
    const std::string out_vrt = std::string(kTestDir) + "/missing_list.vrt";
    GDALRasterMosaicAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"@/vsimem/gdalalg_raster_mosaic_stack_common_test/missing.txt",
         out_vrt}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot open"));
    CPLErrorReset();
  }

  // 3. Glob pattern input with progress callback.
  {
    const std::string out_vrt = std::string(kTestDir) + "/from_glob.vrt";
    const std::string pattern = std::string(kTestDir) + "/glob_*.tif";
    GDALRasterMosaicAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--overwrite", pattern, out_vrt}));
    auto progress_cb = [](double, const char*, void*) -> int { return 1; };
    EXPECT_TRUE(alg.Run(progress_cb, nullptr));
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_vrt.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterXSize());
  }

  // 4. Relative path with SetReferencePathForRelativePaths.
  {
    const std::string out_vrt = std::string(kTestDir) + "/rel_out.vrt";
    GDALRasterMosaicAlgorithmStandalone alg;
    alg.SetReferencePathForRelativePaths(kTestDir);
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--overwrite", "glob_1.tif", "glob_2.tif", out_vrt}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());
  }

  // 5. Mixing dataset by reference and by name fails.
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    std::unique_ptr<GDALDataset> mem_ds(
        mem_drv->Create("", 2, 2, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mem_ds);

    GDALRasterStackAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"-f", "stream", tile1, "dummy_out"}));
    auto* input_arg = alg.GetArg(GDAL_ARG_NAME_INPUT);
    ASSERT_NE(nullptr, input_arg);
    std::vector<GDALArgDatasetValue> mixed(2);
    mixed[0].Set(mem_ds.get());
    mixed[1].Set(tile1);
    ASSERT_TRUE(input_arg->Set(std::move(mixed)));

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("all by reference or all by name"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterMosaicStackCommonTest,
       BuildVrtOptionsStreamAndPipelineExecution) {
  const std::string t1 = std::string(kTestDir) + "/opt_1.tif";
  const std::string t2 = std::string(kTestDir) + "/opt_2.tif";
  const std::string out_tif = std::string(kTestDir) + "/opt_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTile(t1, 0.0, 4.0, 5));
  ASSERT_NO_FATAL_FAILURE(CreateTile(t2, 0.0, 4.0, 9));

  // 1. Standalone with all BuildVRT options + stream output format.
  {
    GDALRasterStackAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--resolution", "1,1", "--bbox", "0,2,2,4", "--tap",
         "--src-nodata", "0", "--dst-nodata", "255", "--band", "1",
         "--hide-nodata", "--absolute-path", t1, t2, "streamed"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(2, out_ds->GetRasterCount());
    EXPECT_EQ(2, out_ds->GetRasterXSize());
    EXPECT_EQ(2, out_ds->GetRasterYSize());
  }

  // 2. Standalone materialized to GTiff via GDALRasterWriteAlgorithm.
  {
    GDALRasterStackAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--overwrite", "-f", "GTiff", "--src-nodata", "0,0", "--dst-nodata",
         "255,255", t1, t2, out_tif}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(2, ds->GetRasterCount());
  }

  // 3. Non-standalone pipeline step (m_standaloneStep == false).
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"stack", t1, t2, "!", "info", "-f", "text"}));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_THAT(pipeline.GetOutputString(), HasSubstr("Band 2"));
  }
}

}  // namespace
}  // namespace autotest2
