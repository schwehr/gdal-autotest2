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
// Tests for GDALDatasetAlgorithm (apps/gdalalg_dataset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_dataset.cpp
//   https://gdal.org/programs/gdal_dataset.html

#include <cstdint>
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
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::UnorderedElementsAre;

constexpr char kTestDir[] = "/vsimem/gdalalg_dataset_test";

void CreateTinyTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> pixels(16, 7);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

class GdalalgDatasetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgDatasetTest, BasicPropertiesSubAlgorithmsAndUsage) {
  auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("dataset");
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("dataset", alg->GetName());
  EXPECT_EQ("Commands to manage datasets.", alg->GetDescription());
  EXPECT_EQ("/programs/gdal_dataset.html", alg->GetHelpURL());
  EXPECT_TRUE(alg->HasSubAlgorithms());

  EXPECT_THAT(alg->GetSubAlgorithmNames(),
              UnorderedElementsAre("identify", "copy", "rename", "delete"));

  const std::string short_usage = alg->GetUsageForCLI(/*shortUsage=*/true, {});
  EXPECT_THAT(short_usage, HasSubstr("identify"));
  EXPECT_THAT(short_usage, HasSubstr("copy"));

  const std::string full_usage = alg->GetUsageForCLI(/*shortUsage=*/false, {});
  EXPECT_THAT(full_usage, HasSubstr("rename"));
  EXPECT_THAT(full_usage, HasSubstr("delete"));

  const std::string json_usage = alg->GetUsageAsJSON();
  EXPECT_THAT(json_usage, HasSubstr("Commands to manage datasets."));
  EXPECT_THAT(json_usage, HasSubstr("\"identify\""));

  std::vector<std::string> ac_root;
  EXPECT_THAT(alg->GetAutoComplete(ac_root, /*lastWordIsComplete=*/true,
                                   /*showAllOptions=*/false),
              Contains("identify"));

  std::vector<std::string> ac_sub = {"identify", "--"};
  EXPECT_THAT(alg->GetAutoComplete(ac_sub, /*lastWordIsComplete=*/false,
                                   /*showAllOptions=*/false),
              Contains("--output-format"));
}

TEST_F(GdalalgDatasetTest, DirectRunAndUnknownSubcommand) {
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("dataset");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("The Run() method should not be called directly on the "
                  "\"gdal dataset\" program."));
    CPLErrorReset();
  }

  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("dataset");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->ParseCommandLineArguments({"no_such_subcommand"}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(GdalalgDatasetTest, ExecuteSubcommands) {
  const std::string src_tif = std::string(kTestDir) + "/src.tif";
  const std::string copy_tif = std::string(kTestDir) + "/copy.tif";
  const std::string renamed_tif = std::string(kTestDir) + "/renamed.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(src_tif));

  // 1. identify
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("dataset");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"identify", src_tif}));
    ASSERT_TRUE(alg->Run());
    const auto* out_arg =
        alg->GetActualAlgorithm().GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    EXPECT_THAT(out_arg->Get<std::string>(), HasSubstr("GTiff"));
  }

  // 2. copy
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("dataset");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"copy", src_tif, copy_tif}));
    ASSERT_TRUE(alg->Run());
    ASSERT_TRUE(alg->Finalize());

    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(copy_tif.c_str(), &sStat));
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(copy_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterXSize());
  }

  // 3. rename
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("dataset");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(
        alg->ParseCommandLineArguments({"rename", copy_tif, renamed_tif}));
    ASSERT_TRUE(alg->Run());
    ASSERT_TRUE(alg->Finalize());

    VSIStatBufL sStat;
    EXPECT_NE(0, VSIStatL(copy_tif.c_str(), &sStat));
    EXPECT_EQ(0, VSIStatL(renamed_tif.c_str(), &sStat));
  }

  // 4. delete
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("dataset");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"delete", renamed_tif}));
    ASSERT_TRUE(alg->Run());
    ASSERT_TRUE(alg->Finalize());

    VSIStatBufL sStat;
    EXPECT_NE(0, VSIStatL(renamed_tif.c_str(), &sStat));
  }
}

}  // namespace
}  // namespace autotest2
