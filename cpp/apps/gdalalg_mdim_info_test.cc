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
// Tests for GDALMdimInfoAlgorithm (apps/gdalalg_mdim_info.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_mdim_info.cpp

#include "third_party/gdal/apps/gdalalg_mdim_info.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_mdim_info_test";

class GdalalgMdimInfoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_Zarr();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

std::unique_ptr<GDALDataset> CreateTestMdimMemDataset() {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!mem_drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      mem_drv->CreateMultiDimensional("mem_mdim", nullptr, nullptr));
  if (!ds) return nullptr;
  auto root = ds->GetRootGroup();
  if (!root) return nullptr;

  auto dim_y = root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 2);
  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "EAST", 3);
  if (!dim_y || !dim_x) return nullptr;

  const auto dt = GDALExtendedDataType::Create(GDT_Int32);
  auto arr = root->CreateMDArray("temp", {dim_y, dim_x}, dt);
  if (!arr) return nullptr;
  const std::vector<int32_t> values = {1, 2, 3, 4, 5, 6};
  const GUInt64 start[2] = {0, 0};
  const size_t count[2] = {2, 3};
  if (!arr->Write(start, count, nullptr, nullptr, dt, values.data())) {
    return nullptr;
  }

  auto arr2 = root->CreateMDArray("other", {dim_x}, dt);
  if (!arr2) return nullptr;
  return ds;
}

TEST_F(GdalalgMdimInfoTest, BasicProperties) {
  GDALMdimInfoAlgorithm alg;
  EXPECT_EQ("info", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_mdim_info.html", alg.GetHelpURL());
}

TEST_F(GdalalgMdimInfoTest, DefaultAndOptionsRun) {
  auto ds = CreateTestMdimMemDataset();
  ASSERT_NE(nullptr, ds);

  // 1. Default JSON output.
  {
    GDALMdimInfoAlgorithm alg;
    auto* input_arg = alg.GetArg(GDAL_ARG_NAME_INPUT);
    ASSERT_NE(nullptr, input_arg);
    ASSERT_TRUE(input_arg->Set(ds.get()));
    ASSERT_TRUE(alg.Run());

    const auto* out_arg = alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    const std::string output = out_arg->Get<std::string>();
    EXPECT_THAT(output, HasSubstr("\"temp\""));
    EXPECT_THAT(output, HasSubstr("\"other\""));
  }

  // 2. Detailed, stats, limit, array, and array-option flags.
  {
    GDALMdimInfoAlgorithm alg;
    auto* input_arg = alg.GetArg(GDAL_ARG_NAME_INPUT);
    ASSERT_NE(nullptr, input_arg);
    ASSERT_TRUE(input_arg->Set(ds.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--detailed", "--stats", "--limit", "2", "--array", "temp",
         "--array-option", "FOO=BAR"}));
    ASSERT_TRUE(alg.Run());

    const std::string output =
        alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING)->Get<std::string>();
    EXPECT_THAT(output, HasSubstr("\"temp\""));
    EXPECT_THAT(output, HasSubstr("\"statistics\""));
    EXPECT_THAT(output, HasSubstr("\"values\""));
    EXPECT_THAT(output, testing::Not(HasSubstr("\"other\"")));
  }

  // 3. Stdout mode leaves output-string empty.
  {
    GDALMdimInfoAlgorithm alg;
    auto* input_arg = alg.GetArg(GDAL_ARG_NAME_INPUT);
    ASSERT_NE(nullptr, input_arg);
    ASSERT_TRUE(input_arg->Set(ds.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--stdout"}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(
        alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING)->Get<std::string>().empty());
  }
}

TEST_F(GdalalgMdimInfoTest, ValidationErrorsAndNonMultidimFailure) {
  // 1. Invalid array-option (missing '=') fails validation.
  {
    GDALMdimInfoAlgorithm alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--array-option", "INVALID_NO_EQUALS"}));
  }

  // 2. Missing required dataset fails.
  {
    GDALMdimInfoAlgorithm alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({}));
  }

  // 3. Non-multidimensional MEM dataset fails Run().
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  std::unique_ptr<GDALDataset> raster_ds(
      mem_drv->Create("", 2, 2, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, raster_ds);
  {
    GDALMdimInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(raster_ds.get()));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }
}

TEST_F(GdalalgMdimInfoTest, ArrayOptionAutoComplete) {
  const std::string zarr_path = std::string(kTestDir) + "/sample.zarr";
  GDALDriver* zarr_drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_NE(nullptr, zarr_drv);

  {
    std::unique_ptr<GDALDataset> zarr_ds(
        zarr_drv->CreateMultiDimensional(zarr_path.c_str(), nullptr, nullptr));
    ASSERT_NE(nullptr, zarr_ds);
    auto root = zarr_ds->GetRootGroup();
    ASSERT_NE(nullptr, root);
    auto dim_x = root->CreateDimension("x", std::string(), std::string(), 2);
    ASSERT_NE(nullptr, dim_x);
    auto arr = root->CreateMDArray("v", {dim_x},
                                   GDALExtendedDataType::Create(GDT_Byte));
    ASSERT_NE(nullptr, arr);
    const std::vector<uint8_t> vals = {1, 2};
    const GUInt64 start[1] = {0};
    const size_t count[1] = {2};
    ASSERT_TRUE(arr->Write(start, count, nullptr, nullptr,
                           GDALExtendedDataType::Create(GDT_Byte),
                           vals.data()));
  }

  const char* prev_meta =
      zarr_drv->GetMetadataItem(GDAL_DMD_MULTIDIM_ARRAY_OPENOPTIONLIST);
  const bool had_prev = (prev_meta != nullptr);
  const std::string saved_meta = had_prev ? prev_meta : "";
  zarr_drv->SetMetadataItem(
      GDAL_DMD_MULTIDIM_ARRAY_OPENOPTIONLIST,
      "<OptionList><Option name='TEST_OPT' type='string'/></OptionList>");
  absl::Cleanup restore_meta = [zarr_drv, saved_meta, had_prev] {
    zarr_drv->SetMetadataItem(GDAL_DMD_MULTIDIM_ARRAY_OPENOPTIONLIST,
                              had_prev ? saved_meta.c_str() : nullptr);
  };

  GDALMdimInfoAlgorithm alg;
  std::vector<std::string> args = {zarr_path, "--array-option", "TEST"};
  const std::vector<std::string> suggestions =
      alg.GetAutoComplete(args, /*lastWordIsComplete=*/false,
                          /*showAllOptions=*/false);
  EXPECT_THAT(suggestions, Contains(HasSubstr("TEST_OPT=")));
}

}  // namespace
}  // namespace autotest2
