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
// Tests for GDALMdimConvertAlgorithm (apps/gdalalg_mdim_convert.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_mdim_convert.cpp
//   https://gdal.org/programs/gdal_mdim_convert.html

#include "third_party/gdal/apps/gdalalg_mdim_convert.h"

#include <cstdint>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalalgorithm.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::IsEmpty;
using ::testing::Not;

constexpr char kTestDir[] = "/vsimem/gdalalg_mdim_convert_test";

void CreateSampleZarr(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->CreateMultiDimensional(path.c_str(), nullptr, nullptr));
  ASSERT_NE(nullptr, ds);

  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);
  auto dim_y = root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 4);
  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "EAST", 4);
  ASSERT_NE(nullptr, dim_y);
  ASSERT_NE(nullptr, dim_x);

  const auto dt = GDALExtendedDataType::Create(GDT_Int32);
  auto y_var = root->CreateMDArray("y", {dim_y}, dt);
  ASSERT_NE(nullptr, y_var);
  const int32_t y_vals[4] = {0, 1, 2, 3};
  const GUInt64 sy[1] = {0};
  const size_t cy[1] = {4};
  const GInt64 sty[1] = {1};
  const GPtrDiff_t stry[1] = {1};
  ASSERT_TRUE(y_var->Write(sy, cy, sty, stry, dt, y_vals));
  ASSERT_TRUE(dim_y->SetIndexingVariable(y_var));

  auto arr = root->CreateMDArray("temp", {dim_y, dim_x}, dt);
  ASSERT_NE(nullptr, arr);
  std::vector<int32_t> vals(16);
  for (int i = 0; i < 16; ++i) vals[i] = i + 1;
  const GUInt64 start[2] = {0, 0};
  const size_t count[2] = {4, 4};
  const GInt64 step[2] = {1, 1};
  const GPtrDiff_t stride[2] = {4, 1};
  ASSERT_TRUE(arr->Write(start, count, step, stride, dt, vals.data()));

  auto sub = root->CreateGroup("subgrp");
  ASSERT_NE(nullptr, sub);
  auto sub_dim = sub->CreateDimension("t", "", "", 2);
  auto sub_arr = sub->CreateMDArray("pressure", {sub_dim}, dt);
  ASSERT_NE(nullptr, sub_arr);
  const int32_t pvals[2] = {100, 200};
  const GUInt64 s1[1] = {0};
  const size_t c1[1] = {2};
  const GInt64 st1[1] = {1};
  const GPtrDiff_t str1[1] = {1};
  ASSERT_TRUE(sub_arr->Write(s1, c1, st1, str1, dt, pvals));
}

class GdalalgMdimConvertTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_Zarr();
    GDALRegister_netCDF();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgMdimConvertTest, BasicPropertiesAndValidation) {
  GDALMdimConvertAlgorithm alg;
  EXPECT_EQ("convert", alg.GetName());
  EXPECT_EQ("Convert a multidimensional dataset.", alg.GetDescription());
  EXPECT_EQ("/programs/gdal_mdim_convert.html", alg.GetHelpURL());

  // Invalid array-option without '=' fails key=value validation.
  WithQuietHandler quiet;
  EXPECT_FALSE(alg.ParseCommandLineArguments({"--array-option", "BAD_OPT"}));
}

TEST_F(GdalalgMdimConvertTest, AutoCompleteArraysAndNetcdfArrayOptions) {
  const std::string src_zarr = std::string(kTestDir) + "/ac_src.zarr";
  ASSERT_NO_FATAL_FAILURE(CreateSampleZarr(src_zarr));

  // 1. Array autocomplete on Zarr dataset.
  {
    GDALMdimConvertAlgorithm alg;
    std::vector<std::string> args = {src_zarr, "--array", ""};
    const std::vector<std::string> choices =
        alg.GetAutoComplete(args, /*lastWordIsComplete=*/false,
                            /*showAllOptions=*/false);
    EXPECT_THAT(choices, Contains("/temp"));
    EXPECT_THAT(choices, Contains("/subgrp/pressure"));
  }

  // 2. array-option autocomplete on real NetCDF file (NetCDF driver exposes
  //    GDAL_DMD_MULTIDIM_ARRAY_OPENOPTIONLIST).
  const std::filesystem::path tmp_dir =
      std::filesystem::path(::testing::TempDir()) / "gdalalg_mdim_convert_nc";
  std::filesystem::create_directories(tmp_dir);
  absl::Cleanup rm_tmp = [&tmp_dir] {
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir, ec);
  };
  const std::string src_nc = (tmp_dir / "src.nc").string();
  {
    GDALMdimConvertAlgorithm to_nc;
    ASSERT_TRUE(to_nc.ParseCommandLineArguments(
        {"--of", "netCDF", "--overwrite", src_zarr, src_nc}));
    ASSERT_TRUE(to_nc.Run());
    EXPECT_TRUE(to_nc.Finalize());
  }

  {
    GDALMdimConvertAlgorithm alg;
    std::vector<std::string> args = {src_nc, "--array-option", ""};
    const std::vector<std::string> choices =
        alg.GetAutoComplete(args, /*lastWordIsComplete=*/false,
                            /*showAllOptions=*/false);
    EXPECT_THAT(choices, Not(IsEmpty()));
  }
}

TEST_F(GdalalgMdimConvertTest, ConvertOptionsSubsetScaleAxesGroupAndOverwrite) {
  const std::string src_zarr = std::string(kTestDir) + "/src.zarr";
  const std::string out_zarr = std::string(kTestDir) + "/out.zarr";
  const std::string grp_zarr = std::string(kTestDir) + "/grp.zarr";
  ASSERT_NO_FATAL_FAILURE(CreateSampleZarr(src_zarr));

  // 1. Convert single array with --subset, --scale-axes, -co, --strict,
  //    --array-option.
  {
    GDALMdimConvertAlgorithm alg;
    ASSERT_TRUE(alg.GetArg("subset")->Set(std::vector<std::string>{"y(0,3)"}));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--of", "Zarr", "--array", "/temp", "--array-option", "DUMMY=1",
         "--scale-axes", "y(2)", "--scale-axes", "x(2)", "--co",
         "FORMAT=ZARR_V2", "--strict", "--overwrite", src_zarr, out_zarr}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  {
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_zarr.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    auto arr = ds->GetRootGroup()->OpenMDArray("temp");
    ASSERT_NE(nullptr, arr);
    ASSERT_EQ(2U, arr->GetDimensionCount());
    EXPECT_EQ(2U, arr->GetDimensions()[0]->GetSize());
    EXPECT_EQ(2U, arr->GetDimensions()[1]->GetSize());
  }

  // 2. Without --overwrite on existing output fails at parse validation.
  {
    GDALMdimConvertAlgorithm alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--of", "Zarr", src_zarr, out_zarr}));
    EXPECT_THAT(CPLGetLastErrorMsg(), ::testing::HasSubstr("already exists"));
    CPLErrorReset();
  }

  // 3. Convert specific --group.
  {
    GDALMdimConvertAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--of", "Zarr", "--group", "/subgrp",
                                       "--overwrite", src_zarr, grp_zarr}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        grp_zarr.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetRootGroup()->OpenMDArray("pressure"));
  }
}

}  // namespace
}  // namespace autotest2
