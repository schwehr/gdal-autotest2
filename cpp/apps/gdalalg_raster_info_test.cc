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
// Tests for GDALRasterInfoAlgorithm (apps/gdalalg_raster_info.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_info.cpp

#include "third_party/gdal/apps/gdalalg_raster_info.h"

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
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_info_test";

class GdalalgRasterInfoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateTestTiff(const std::string& path, int size = 4, uint8_t fill = 42) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), size, size, 1, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());
  ds->SetMetadataItem("FOO", "BAR");
  ds->SetMetadataItem("CUSTOM_KEY", "CUSTOM_VAL", "CUSTOM_DOMAIN");
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(0));
  std::vector<uint8_t> pixels(size * size, fill);
  if (!pixels.empty()) {
    pixels[0] = 10;
  }
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, size, size,
                                                    pixels.data(), size, size,
                                                    GDT_Byte, 0, 0, nullptr));
}

TEST_F(GdalalgRasterInfoTest, DefaultFormatJsonVsText) {
  const std::string path = std::string(kTestDir) + "/basic.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestTiff(path));

  // Default via API (not called from command line) is JSON.
  {
    GDALRasterInfoAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({path}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(alg.GetOutputString(), HasSubstr("\"size\":["));
  }

  // Default via CLI (called from command line) is text.
  {
    GDALRasterInfoAlgorithmStandalone alg;
    alg.SetCalledFromCommandLine();
    ASSERT_TRUE(alg.ParseCommandLineArguments({path}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(alg.GetOutputString(), HasSubstr("Size is 4, 4"));
  }
}

TEST_F(GdalalgRasterInfoTest, AllStatsAndSuppressionFlagsJson) {
  const std::string path = std::string(kTestDir) + "/flags.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestTiff(path));

  // 1. Suppression flags (--no-gcp, --no-md, --no-ct, --no-fl, --no-nodata,
  // --no-mask) together with stats, hist, and checksum.
  {
    GDALRasterInfoAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--output-format",
        "json",
        "--min-max",
        "--stats",
        "--hist",
        "--no-gcp",
        "--no-md",
        "--no-ct",
        "--no-fl",
        "--no-nodata",
        "--no-mask",
        "--checksum",
        path,
    }));
    EXPECT_TRUE(alg.Run());
    const std::string out = alg.GetOutputString();
    EXPECT_THAT(out, HasSubstr("computedMin"));
    EXPECT_THAT(out, HasSubstr("histogram"));
    EXPECT_THAT(out, HasSubstr("checksum"));
    EXPECT_THAT(out, Not(HasSubstr("FOO")));
    EXPECT_THAT(out, Not(HasSubstr("\"files\":")));
    EXPECT_THAT(out, Not(HasSubstr("noDataValue")));
  }

  // 2. --list-mdd and --metadata-domain (without --no-md).
  {
    GDALRasterInfoAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--output-format",
        "json",
        "--list-mdd",
        "--metadata-domain",
        "CUSTOM_DOMAIN",
        path,
    }));
    EXPECT_TRUE(alg.Run());
    const std::string out = alg.GetOutputString();
    EXPECT_THAT(out, HasSubstr("metadataDomains"));
    EXPECT_THAT(out, HasSubstr("CUSTOM_VAL"));
  }
}

TEST_F(GdalalgRasterInfoTest, ApproxStatsAndTextFormat) {
  const std::string path = std::string(kTestDir) + "/approx.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestTiff(path));

  GDALRasterInfoAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments({
      "--output-format",
      "text",
      "--approx-stats",
      "--mdd",
      "CUSTOM_DOMAIN",
      path,
  }));
  EXPECT_TRUE(alg.Run());
  const std::string out = alg.GetOutputString();
  EXPECT_THAT(out, HasSubstr("Driver: GTiff/GeoTIFF"));
  EXPECT_THAT(out, HasSubstr("CUSTOM_KEY=CUSTOM_VAL"));
}

TEST_F(GdalalgRasterInfoTest, SubdatasetValidInvalidAndMissingTarget) {
  const std::string sub_path = std::string(kTestDir) + "/sub.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestTiff(sub_path, /*size=*/6, /*fill=*/99));

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_THAT(mem_drv, NotNull());
  std::unique_ptr<GDALDataset> container_ds(
      mem_drv->Create("", 2, 2, 1, GDT_Byte, nullptr));
  ASSERT_THAT(container_ds, NotNull());
  container_ds->SetMetadataItem("SUBDATASET_1_NAME", sub_path.c_str(),
                                "SUBDATASETS");
  container_ds->SetMetadataItem("SUBDATASET_1_DESC", "First subdataset",
                                "SUBDATASETS");
  container_ds->SetMetadataItem(
      "SUBDATASET_2_NAME", "/vsimem/gdalalg_raster_info_test/nonexistent.tif",
      "SUBDATASETS");
  container_ds->SetMetadataItem("SUBDATASET_2_DESC", "Missing subdataset",
                                "SUBDATASETS");

  // 1. Valid subdataset index 1.
  {
    GDALRasterInfoAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(container_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--subdataset", "1"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(alg.GetOutputString(), HasSubstr(sub_path));
    EXPECT_THAT(alg.GetOutputString(),
                HasSubstr("\"size\":[\n    6,\n    6\n  ]"));
  }

  // 2. Subdataset index pointing to non-existent dataset fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterInfoAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(container_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--subdataset", "2"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 3. Out-of-range subdataset index fails with message.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterInfoAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(container_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--subdataset", "3"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid value for 'subdataset' argument"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterInfoTest, PipelineIntegrationAndStdout) {
  const std::string path = std::string(kTestDir) + "/pipe.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestTiff(path));

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", path, "!", "info", "--output-format", "json"}));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_THAT(pipeline.GetOutputString(),
              HasSubstr("\"driverShortName\":\"GTiff\""));

  // Exercise --stdout branch.
  GDALRasterInfoAlgorithmStandalone stdout_alg;
  ASSERT_TRUE(stdout_alg.ParseCommandLineArguments(
      {"--output-format", "text", "--stdout", path}));
  EXPECT_TRUE(stdout_alg.Run());
  EXPECT_TRUE(stdout_alg.GetOutputString().empty());
}

}  // namespace
}  // namespace autotest2
