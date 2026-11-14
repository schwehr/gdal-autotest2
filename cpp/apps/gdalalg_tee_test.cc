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
// Tests for GDALTee algorithms (apps/gdalalg_tee.cpp and
// apps/gdalalg_tee.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_tee.cpp

#include "third_party/gdal/apps/gdalalg_tee.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_tee_test";

void CreateTinyTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> pixels(16, 11);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

TEST(GdalalgTeeTest, ConstructAndDestroyRaster) {
  auto alg = std::make_unique<GDALTeeRasterAlgorithm>();
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("tee", alg->GetName());
  EXPECT_FALSE(alg->IsNativelyStreamingCompatible());
  EXPECT_TRUE(alg->CanBeMiddleStep());
  EXPECT_TRUE(alg->CanBeLastStep());
  EXPECT_TRUE(alg->GeneratesFilesFromUserInput());
  EXPECT_FALSE(alg->HasOutputString());
}

TEST(GdalalgTeeTest, ConstructAndDestroyVector) {
  auto alg = std::make_unique<GDALTeeVectorAlgorithm>();
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("tee", alg->GetName());
  EXPECT_FALSE(alg->IsNativelyStreamingCompatible());
  EXPECT_TRUE(alg->CanBeMiddleStep());
  EXPECT_TRUE(alg->CanBeLastStep());
  EXPECT_TRUE(alg->GeneratesFilesFromUserInput());
  EXPECT_FALSE(alg->HasOutputString());
}

TEST(GdalalgTeeTest, BindFilenameAndCopyFilenameBindingsFrom) {
  GDALRegister_GTiff();
  GDALRegister_MEM();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = []() { VSIRmdirRecursive(kTestDir); };

  const std::string in_tif = std::string(kTestDir) + "/bind_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/bind_out.tif";
  CreateTinyTiff(in_tif);

  GDALRasterPipelineAlgorithm nested_without_output;
  ASSERT_TRUE(nested_without_output.ParseCommandLineArguments(
      {"read", in_tif, "!", "write", out_tif, "--overwrite"}));
  EXPECT_TRUE(nested_without_output.HasSteps());

  GDALRasterPipelineAlgorithm nested_with_output;
  ASSERT_TRUE(nested_with_output.ParseCommandLineArguments(
      {"read", in_tif, "!", "info"}));
  EXPECT_TRUE(nested_with_output.HasSteps());

  GDALTeeRasterAlgorithm source_alg;
  EXPECT_TRUE(
      source_alg.BindFilename("quiet_pipe", &nested_without_output, {"write"}));
  EXPECT_FALSE(source_alg.HasOutputString());
  EXPECT_TRUE(
      source_alg.BindFilename("nested_pipe", &nested_with_output, {"info"}));
  EXPECT_FALSE(
      source_alg.BindFilename("nested_pipe", &nested_with_output, {"info"}));
  EXPECT_TRUE(source_alg.HasOutputString());

  GDALTeeRasterAlgorithm copied_alg;
  EXPECT_FALSE(copied_alg.HasOutputString());
  copied_alg.CopyFilenameBindingsFrom(&source_alg);
  EXPECT_TRUE(copied_alg.HasOutputString());
}

TEST(GdalalgTeeTest, HasOutputStringResolvesStepSuffixesBeforeStepsExist) {
  {
    GDALRasterPipelineAlgorithm nested;
    EXPECT_FALSE(nested.HasSteps());
    GDALTeeRasterAlgorithm alg;
    ASSERT_TRUE(alg.BindFilename("p1", &nested, {"info"}));
    EXPECT_TRUE(alg.HasOutputString());
  }

  {
    GDALVectorPipelineAlgorithm nested;
    EXPECT_FALSE(nested.HasSteps());
    GDALTeeVectorAlgorithm alg;
    ASSERT_TRUE(alg.BindFilename("p1", &nested, {"info"}));
    EXPECT_TRUE(alg.HasOutputString());
  }

  {
    GDALVectorPipelineAlgorithm nested;
    EXPECT_FALSE(nested.HasSteps());
    GDALTeeVectorAlgorithm alg;
    ASSERT_TRUE(
        alg.BindFilename("p1", &nested, {"--unrecognized-token", "convert"}));
    EXPECT_FALSE(alg.HasOutputString());
  }
}

TEST(GdalalgTeeTest, RunRealRasterPipelineWithTee) {
  GDALRegister_GTiff();
  GDALRegister_MEM();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = []() { VSIRmdirRecursive(kTestDir); };

  const std::string in_tif = std::string(kTestDir) + "/tee_in.tif";
  const std::string side_tif = std::string(kTestDir) + "/tee_side.tif";
  const std::string out_tif = std::string(kTestDir) + "/tee_out.tif";
  CreateTinyTiff(in_tif);

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_tif, "!", "tee", "[", "write", side_tif, "--overwrite", "]",
       "!", "info"}));
  pipeline.SetCalledFromCommandLine();
  EXPECT_TRUE(pipeline.Run());
  const std::string output = pipeline.GetOutputString();
  EXPECT_NE(std::string::npos, output.find("tee_in.tif"));

  VSIStatBufL statbuf;
  EXPECT_EQ(0, VSIStatL(side_tif.c_str(), &statbuf));

  // Also test tee into another write step without output string.
  GDALRegister_VRT();
  GDALRasterPipelineAlgorithm pipeline2;
  ASSERT_TRUE(pipeline2.ParseCommandLineArguments(
      {"read", in_tif, "!", "tee", "[", "set-type", "--datatype", "UInt16", "!",
       "write", out_tif, "--overwrite", "]", "!", "write", side_tif,
       "--overwrite"}));
  EXPECT_TRUE(pipeline2.Run());
}

}  // namespace
}  // namespace autotest2
