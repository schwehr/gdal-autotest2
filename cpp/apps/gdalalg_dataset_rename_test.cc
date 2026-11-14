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
// Tests for GDALDatasetRenameAlgorithm (apps/gdalalg_dataset_rename.cpp and
// apps/gdalalg_dataset_rename.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_dataset_rename.cpp

#include "third_party/gdal/apps/gdalalg_dataset_rename.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_dataset_rename_test";

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

void CreateTinyEHdr(const std::string& path, uint8_t fill_value) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("EHdr");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> pixels(16, fill_value);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

void WriteVsimemFile(const std::string& path, const std::string& content) {
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  ASSERT_EQ(content.size(), VSIFWriteL(content.data(), 1, content.size(), fp));
  ASSERT_EQ(0, VSIFCloseL(fp));
}

class GdalalgDatasetRenameTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_EHdr();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgDatasetRenameTest, BasicPropertiesAliasesAndMissingArgs) {
  auto alg = std::make_unique<GDALDatasetRenameAlgorithm>();
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("rename", alg->GetName());
  EXPECT_STREQ("Rename files of a dataset.", alg->GetDescription().c_str());
  EXPECT_STREQ("/programs/gdal_dataset_rename.html", alg->GetHelpURL().c_str());
  EXPECT_THAT(GDALDatasetRenameAlgorithm::GetAliasesStatic(),
              ElementsAre("ren", "mv"));

  WithQuietHandler quiet;
  EXPECT_FALSE(alg->ParseCommandLineArguments({}));
  EXPECT_FALSE(alg->ParseCommandLineArguments({"only_source.tif"}));
}

TEST_F(GdalalgDatasetRenameTest, RenameMultiFileDatasetAndVerify) {
  const std::string src_bil = std::string(kTestDir) + "/src.bil";
  const std::string dst_bil = std::string(kTestDir) + "/dst.bil";
  ASSERT_NO_FATAL_FAILURE(CreateTinyEHdr(src_bil, 42));

  GDALDatasetRenameAlgorithm alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments({src_bil, dst_bil}));
  EXPECT_TRUE(alg.Run());

  VSIStatBufL stat_buf;
  EXPECT_NE(0, VSIStatL(src_bil.c_str(), &stat_buf));
  EXPECT_NE(0,
            VSIStatL((std::string(kTestDir) + "/src.hdr").c_str(), &stat_buf));
  EXPECT_EQ(0, VSIStatL(dst_bil.c_str(), &stat_buf));
  EXPECT_EQ(0,
            VSIStatL((std::string(kTestDir) + "/dst.hdr").c_str(), &stat_buf));

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(dst_bil.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(4, ds->GetRasterXSize());
  EXPECT_EQ(4, ds->GetRasterYSize());
  uint8_t val = 0;
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                           GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(42, val);
}

TEST_F(GdalalgDatasetRenameTest, OverwriteExistingFileDatasetAndDirectory) {
  const std::string src_tif = std::string(kTestDir) + "/src.tif";
  const std::string dst_plain = std::string(kTestDir) + "/plain.bin";
  const std::string dst_tif = std::string(kTestDir) + "/existing.tif";
  const std::string dst_dir = std::string(kTestDir) + "/non_gdal_dir";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(src_tif, 99));
  ASSERT_NO_FATAL_FAILURE(WriteVsimemFile(dst_plain, "old content"));
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(dst_tif, 1));
  ASSERT_EQ(0, VSIMkdir(dst_dir.c_str(), 0755));

  // Existing destination without --overwrite fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetRenameAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({src_tif, dst_tif}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
    CPLErrorReset();
  }

  // Existing non-GDAL directory with --overwrite fails safely.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetRenameAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--overwrite", src_tif, dst_dir}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("not recognized as a valid GDAL dataset"));
    CPLErrorReset();
  }

  // Existing plain file with --overwrite succeeds.
  {
    GDALDatasetRenameAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--overwrite", "-f", "GTiff", src_tif, dst_plain}));
    EXPECT_TRUE(alg.Run());
  }

  // Existing GDAL dataset with --overwrite succeeds.
  {
    GDALDatasetRenameAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--overwrite", dst_plain, dst_tif}));
    EXPECT_TRUE(alg.Run());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(dst_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    uint8_t val = 0;
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(99, val);
  }
}

TEST_F(GdalalgDatasetRenameTest, NonExistentSourceAndInvalidFormatFail) {
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetRenameAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"-f", "NO_SUCH_DRIVER", "a.tif", "b.tif"}));
    CPLErrorReset();
  }

  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetRenameAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({std::string(kTestDir) + "/missing.tif",
                                       std::string(kTestDir) + "/target.tif"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
