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
// Tests for GDALRasterAlgorithm (apps/gdalalg_raster.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster.cpp
//   https://gdal.org/programs/gdal_raster.html

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
using ::testing::IsSupersetOf;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_test";

void CreateTinyTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> pixels(16, 23);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

class GdalalgRasterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterTest, BasicPropertiesSubAlgorithmsAndUsage) {
  auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("raster");
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("raster", alg->GetName());
  EXPECT_EQ("Raster commands.", alg->GetDescription());
  EXPECT_EQ("/programs/gdal_raster.html", alg->GetHelpURL());
  EXPECT_TRUE(alg->HasSubAlgorithms());

  EXPECT_THAT(alg->GetSubAlgorithmNames(),
              IsSupersetOf({"info", "convert", "create", "edit", "pipeline",
                            "reproject", "select", "set-type", "sieve", "tile",
                            "zonal-stats"}));

  const std::string short_usage = alg->GetUsageForCLI(/*shortUsage=*/true, {});
  EXPECT_THAT(short_usage, HasSubstr("info"));
  EXPECT_THAT(short_usage, HasSubstr("convert"));

  const std::string full_usage = alg->GetUsageForCLI(/*shortUsage=*/false, {});
  EXPECT_THAT(full_usage, HasSubstr("--drivers"));
  EXPECT_THAT(full_usage, HasSubstr("pipeline"));

  const std::string json_usage = alg->GetUsageAsJSON();
  EXPECT_THAT(json_usage, HasSubstr("Raster commands."));
  EXPECT_THAT(json_usage, HasSubstr("\"convert\""));

  std::vector<std::string> ac_root;
  EXPECT_THAT(alg->GetAutoComplete(ac_root, /*lastWordIsComplete=*/true,
                                   /*showAllOptions=*/false),
              Contains("info"));

  std::vector<std::string> ac_opts = {"--"};
  EXPECT_THAT(alg->GetAutoComplete(ac_opts, /*lastWordIsComplete=*/false,
                                   /*showAllOptions=*/false),
              Contains("--drivers"));
}

TEST_F(GdalalgRasterTest, DriversOptionAndDirectRunError) {
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("raster");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"--drivers"}));
    ASSERT_TRUE(alg->Run());
    const auto* out_arg = alg->GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    const std::string output = out_arg->Get<std::string>();
    EXPECT_FALSE(output.empty());
    EXPECT_THAT(output, HasSubstr("GTiff"));
  }

  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("raster");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("The Run() method should not be called directly on the "
                  "\"gdal raster\" program."));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterTest, ExecuteSubcommands) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));

  // 1. raster info
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("raster");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"info", in_tif}));
    ASSERT_TRUE(alg->Run());
    const auto* out_arg =
        alg->GetActualAlgorithm().GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    EXPECT_THAT(out_arg->Get<std::string>(),
                HasSubstr("\"driverShortName\":\"GTiff\""));
  }

  // 2. raster convert
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("raster");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments(
        {"convert", in_tif, out_tif, "--overwrite"}));
    ASSERT_TRUE(alg->Run());
    ASSERT_TRUE(alg->Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(4, out_ds->GetRasterXSize());
    EXPECT_EQ(4, out_ds->GetRasterYSize());
  }
}

}  // namespace
}  // namespace autotest2
