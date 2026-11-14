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
// Tests for GDALMdimAlgorithm (apps/gdalalg_mdim.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_mdim.cpp
//   https://gdal.org/programs/gdal_mdim.html

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::UnorderedElementsAre;

constexpr char kTestDir[] = "/vsimem/gdalalg_mdim_test";

void CreateTinyZarrDataset(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional(path.c_str(), nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);
  auto dim_x = root->CreateDimension("x", "", "", 4);
  ASSERT_NE(nullptr, dim_x);
  const auto dt = GDALExtendedDataType::Create(GDT_Float32);
  auto arr = root->CreateMDArray("temp", {dim_x}, dt);
  ASSERT_NE(nullptr, arr);
  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {4};
  const std::vector<float> vals = {1.0f, 2.0f, 3.0f, 4.0f};
  ASSERT_TRUE(arr->Write(start.data(), count.data(), nullptr, nullptr, dt,
                         vals.data()));
}

class GdalalgMdimTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_Zarr();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgMdimTest, BasicPropertiesSubAlgorithmsAndUsage) {
  auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("mdim");
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("mdim", alg->GetName());
  EXPECT_EQ("Multidimensional commands.", alg->GetDescription());
  EXPECT_EQ("/programs/gdal_mdim.html", alg->GetHelpURL());
  EXPECT_TRUE(alg->HasSubAlgorithms());

  EXPECT_THAT(alg->GetSubAlgorithmNames(),
              UnorderedElementsAre("info", "convert", "mosaic"));

  const std::string short_usage = alg->GetUsageForCLI(/*shortUsage=*/true, {});
  EXPECT_THAT(short_usage, HasSubstr("info"));
  EXPECT_THAT(short_usage, HasSubstr("convert"));

  const std::string full_usage = alg->GetUsageForCLI(/*shortUsage=*/false, {});
  EXPECT_THAT(full_usage, HasSubstr("--drivers"));
  EXPECT_THAT(full_usage, HasSubstr("mosaic"));

  const std::string json_usage = alg->GetUsageAsJSON();
  EXPECT_THAT(json_usage, HasSubstr("Multidimensional commands."));
  EXPECT_THAT(json_usage, HasSubstr("\"info\""));

  std::vector<std::string> ac_root;
  EXPECT_THAT(alg->GetAutoComplete(ac_root, /*lastWordIsComplete=*/true,
                                   /*showAllOptions=*/false),
              Contains("info"));

  std::vector<std::string> ac_opts = {"--"};
  EXPECT_THAT(alg->GetAutoComplete(ac_opts, /*lastWordIsComplete=*/false,
                                   /*showAllOptions=*/false),
              Contains("--drivers"));
}

TEST_F(GdalalgMdimTest, DriversOptionAndDirectRunError) {
  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("mdim");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"--drivers"}));
    ASSERT_TRUE(alg->Run());
    const auto* out_arg = alg->GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    const std::string output = out_arg->Get<std::string>();
    EXPECT_FALSE(output.empty());
    EXPECT_THAT(output, HasSubstr("Zarr"));
  }

  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("mdim");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("The Run() method should not be called directly on the "
                  "\"gdal mdim\" program."));
    CPLErrorReset();
  }
}

TEST_F(GdalalgMdimTest, ExecuteSubcommands) {
  const std::string in_zarr = std::string(kTestDir) + "/in.zarr";
  const std::string out_zarr = std::string(kTestDir) + "/out.zarr";
  ASSERT_NO_FATAL_FAILURE(CreateTinyZarrDataset(in_zarr));

  // 1. mdim info
  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("mdim");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"info", in_zarr}));
    ASSERT_TRUE(alg->Run());
    const auto* out_arg =
        alg->GetActualAlgorithm().GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    EXPECT_THAT(out_arg->Get<std::string>(), HasSubstr("\"temp\""));
  }

  // 2. mdim convert
  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("mdim");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments(
        {"convert", in_zarr, out_zarr, "--overwrite"}));
    ASSERT_TRUE(alg->Run());
    ASSERT_TRUE(alg->Finalize());

    std::unique_ptr<GDALDataset> out_ds(GDALDataset::Open(
        out_zarr.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    auto root = out_ds->GetRootGroup();
    ASSERT_NE(nullptr, root);
    EXPECT_THAT(root->GetMDArrayNames(), Contains("temp"));
  }
}

}  // namespace
}  // namespace autotest2
