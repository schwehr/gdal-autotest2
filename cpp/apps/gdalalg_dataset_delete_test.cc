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
// Tests for GDALDatasetDeleteAlgorithm (apps/gdalalg_dataset_delete.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_dataset_delete.cpp
//   https://gdal.org/programs/gdal_dataset_delete.html

#include "third_party/gdal/apps/gdalalg_dataset_delete.h"

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
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_dataset_delete_test";

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

class GdalalgDatasetDeleteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgDatasetDeleteTest, BasicPropertiesAndValidation) {
  GDALDatasetDeleteAlgorithm alg;
  EXPECT_EQ("delete", alg.GetName());
  EXPECT_EQ("Delete dataset(s).", alg.GetDescription());
  EXPECT_EQ("/programs/gdal_dataset_delete.html", alg.GetHelpURL());
  EXPECT_THAT(GDALDatasetDeleteAlgorithm::GetAliasesStatic(),
              ElementsAre("rm", "remove"));

  // Missing required positional filename argument.
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({}));
  }

  // Invalid format fails validation.
  {
    GDALDatasetDeleteAlgorithm bad_fmt_alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(bad_fmt_alg.ParseCommandLineArguments(
        {"--format", "NON_EXISTENT_FORMAT",
         std::string(kTestDir) + "/dummy.tif"}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(GdalalgDatasetDeleteTest, FormatAutoComplete) {
  GDALDatasetDeleteAlgorithm alg;
  std::vector<std::string> args = {"--format", ""};
  const std::vector<std::string> choices =
      alg.GetAutoComplete(args, /*lastWordIsComplete=*/false,
                          /*showAllOptions=*/false);
  EXPECT_THAT(choices, Contains("GTiff"));
}

TEST_F(GdalalgDatasetDeleteTest, DeleteSingleAndMultipleDatasets) {
  const std::string tif1 = std::string(kTestDir) + "/one.tif";
  const std::string tif2 = std::string(kTestDir) + "/two.tif";
  const std::string tif3 = std::string(kTestDir) + "/three.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(tif1));
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(tif2));
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(tif3));

  VSIStatBufL statbuf;
  ASSERT_EQ(0, VSIStatL(tif1.c_str(), &statbuf));
  ASSERT_EQ(0, VSIStatL(tif2.c_str(), &statbuf));
  ASSERT_EQ(0, VSIStatL(tif3.c_str(), &statbuf));

  // Delete single file without specifying format.
  {
    GDALDatasetDeleteAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({tif1}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
    EXPECT_NE(0, VSIStatL(tif1.c_str(), &statbuf));
    EXPECT_EQ(0, VSIStatL(tif2.c_str(), &statbuf));
  }

  // Delete multiple files with explicit --format.
  {
    GDALDatasetDeleteAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-f", "GTiff", tif2, tif3}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
    EXPECT_NE(0, VSIStatL(tif2.c_str(), &statbuf));
    EXPECT_NE(0, VSIStatL(tif3.c_str(), &statbuf));
  }
}

TEST_F(GdalalgDatasetDeleteTest, DeleteNonExistentFails) {
  const std::string missing = std::string(kTestDir) + "/missing.tif";

  // 1. Without -f, GDALIdentifyDriver fails with "No identifiable driver".
  {
    GDALDatasetDeleteAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({missing}));

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("No identifiable driver"));
    CPLErrorReset();
  }

  // 2. With explicit -f GTiff, GDALDriver::Delete fails to open missing file.
  {
    GDALDatasetDeleteAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-f", "GTiff", missing}));

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
