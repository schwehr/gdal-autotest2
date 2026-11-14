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
// Tests for GDALDatasetCopyAlgorithm (apps/gdalalg_dataset_copy.cpp and
// apps/gdalalg_dataset_copy.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_dataset_copy.cpp

#include "third_party/gdal/apps/gdalalg_dataset_copy.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_dataset_copy_test";

class TestDatasetRenameSubAlgorithm final
    : public GDALDatasetCopyRenameCommonAlgorithm {
 public:
  TestDatasetRenameSubAlgorithm()
      : GDALDatasetCopyRenameCommonAlgorithm("rename-test", "Rename dataset",
                                             "") {}
};

void CreateTinyTiff(const std::string& path, uint8_t fill_value) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> pixels(16, fill_value);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

TEST(GdalalgDatasetCopyTest, BasicPropertiesAndAutoComplete) {
  GDALRegister_GTiff();

  GDALDatasetCopyAlgorithm alg;
  EXPECT_EQ("copy", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_FALSE(alg.GetHelpURL().empty());
  EXPECT_THAT(GDALDatasetCopyAlgorithm::GetAliasesStatic(), ElementsAre("cp"));

  std::vector<std::string> args = {"src.tif", "dst.tif", "--format", ""};
  std::vector<std::string> suggestions =
      alg.GetAutoComplete(args, /*lastWordIsComplete=*/true,
                          /*showAllOptions=*/false);
  EXPECT_THAT(suggestions, Contains("GTiff"));
}

TEST(GdalalgDatasetCopyTest, MissingAndInvalidArguments) {
  GDALRegister_GTiff();
  WithQuietHandler quiet;

  {
    GDALDatasetCopyAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({}));
  }
  {
    GDALDatasetCopyAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"only_source.tif"}));
  }
  {
    CPLErrorReset();
    GDALDatasetCopyAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--format=NONEXISTENT_DRV", "src.tif", "dst.tif"}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST(GdalalgDatasetCopyTest, CopyDatasetSuccessAndWithFormat) {
  GDALRegister_GTiff();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string src = std::string(kTestDir) + "/src.tif";
  const std::string dst1 = std::string(kTestDir) + "/dst1.tif";
  const std::string dst2 = std::string(kTestDir) + "/dst2.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(src, 77));

  {
    GDALDatasetCopyAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({src, dst1}));
    EXPECT_TRUE(alg.Run());
  }

  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(dst1.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(4, out_ds->GetRasterXSize());
    EXPECT_EQ(4, out_ds->GetRasterYSize());
    std::vector<uint8_t> pixels(16, 0);
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 4, 4, pixels.data(), 4, 4, GDT_Byte,
                           0, 0, nullptr));
    EXPECT_EQ(77, pixels[0]);
  }

  {
    GDALDatasetCopyAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--format", "GTiff", src, dst2}));
    EXPECT_TRUE(alg.Run());
  }

  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(dst2.c_str(), &stat_buf));
}

TEST(GdalalgDatasetCopyTest, OverwritePlainFileDatasetAndDirectory) {
  GDALRegister_GTiff();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string src = std::string(kTestDir) + "/src.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(src, 33));

  // 1. Existing plain file without --overwrite fails.
  const std::string existing_file = std::string(kTestDir) + "/existing.bin";
  VsiMemTempWrapper temp_file(existing_file, "dummy");
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetCopyAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({src, existing_file}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
    CPLErrorReset();
  }

  // 2. Existing plain file with --overwrite unlinks and succeeds.
  {
    GDALDatasetCopyAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--overwrite", src, existing_file}));
    EXPECT_TRUE(alg.Run());
  }

  // 3. Existing directory (not a GDAL dataset) with --overwrite fails.
  const std::string existing_dir = std::string(kTestDir) + "/non_gdal_dir";
  ASSERT_EQ(0, VSIMkdir(existing_dir.c_str(), 0755));
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetCopyAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--overwrite", src, existing_dir}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("not recognized as a valid GDAL dataset"));
    CPLErrorReset();
  }

  // 4. Existing GDAL dataset with --overwrite deletes and replaces it.
  const std::string existing_tif = std::string(kTestDir) + "/existing.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(existing_tif, 11));
  {
    GDALDatasetCopyAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--overwrite", src, existing_tif}));
    EXPECT_TRUE(alg.Run());
  }

  std::unique_ptr<GDALDataset> out_ds(GDALDataset::Open(
      existing_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  std::vector<uint8_t> pixels(16, 0);
  ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, 4, 4, pixels.data(), 4, 4, GDT_Byte, 0,
                         0, nullptr));
  EXPECT_EQ(33, pixels[0]);
}

TEST(GdalalgDatasetCopyTest, NonExistentSourceAndRenameBranch) {
  GDALRegister_GTiff();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetCopyAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({std::string(kTestDir) + "/missing.tif",
                                       std::string(kTestDir) + "/dst.tif"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // Exercise the rename branch in
  // GDALDatasetCopyRenameCommonAlgorithm::RunImpl.
  const std::string src = std::string(kTestDir) + "/rename_src.tif";
  const std::string dst = std::string(kTestDir) + "/rename_dst.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(src, 99));

  TestDatasetRenameSubAlgorithm rename_alg;
  ASSERT_TRUE(rename_alg.ParseCommandLineArguments({src, dst}));
  EXPECT_TRUE(rename_alg.Run());

  VSIStatBufL stat_buf;
  EXPECT_NE(0, VSIStatL(src.c_str(), &stat_buf));
  EXPECT_EQ(0, VSIStatL(dst.c_str(), &stat_buf));
}

}  // namespace
}  // namespace autotest2
