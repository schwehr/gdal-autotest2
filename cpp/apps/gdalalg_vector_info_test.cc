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
// Tests for "vector info" subcommand (apps/gdalalg_vector_info.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_info.cpp

#include "third_party/gdal/apps/gdalalg_vector_info.h"

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_info_test";
constexpr char kTmpDir[] = "/vsimem/gdalalg_vector_info_tmp";

class GdalalgVectorInfoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    ASSERT_EQ(0, VSIMkdir(kTmpDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTmpDir);
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
    VSIRmdirRecursive(kTmpDir);
    CPLErrorReset();
  }
};

void CreateTestGpkg(const std::string& path) {
  GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_THAT(gpkg_drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      gpkg_drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds, NotNull());

  OGRLayer* lyr1 = ds->CreateLayer("pts", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(lyr1, NotNull());
  OGRFieldDefn fld("id", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, lyr1->CreateField(&fld));

  for (int i = 1; i <= 3; ++i) {
    auto feat = std::make_unique<OGRFeature>(lyr1->GetLayerDefn());
    feat->SetField("id", i);
    auto pt = std::make_unique<OGRPoint>(static_cast<double>(i), 2.0);
    feat->SetGeometryDirectly(pt.release());
    ASSERT_EQ(OGRERR_NONE, lyr1->CreateFeature(feat.get()));
  }

  OGRLayer* lyr2 = ds->CreateLayer("other", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(lyr2, NotNull());
}

TEST_F(GdalalgVectorInfoTest, ValidationErrors) {
  const std::string gpkg = std::string(kTestDir) + "/val.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTestGpkg(gpkg));

  // 1. --sql and --where mutually exclusive.
  {
    GDALVectorInfoAlgorithmStandalone alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--sql", "SELECT * FROM pts", "--where", "id = 1", gpkg}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Option 'sql' and 'where' are mutually exclusive"));
  }

  // 2. --features and --summary mutually exclusive.
  {
    GDALVectorInfoAlgorithmStandalone alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--features", "--summary", gpkg}));
  }

  // 3. Invalid SQL causes Run() to fail.
  {
    GDALVectorInfoAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--sql", "SELECT * FROM missing", gpkg}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
  }
}

TEST_F(GdalalgVectorInfoTest, FormatsSummaryFeaturesLimitSqlWhereUpdate) {
  const std::string gpkg = std::string(kTestDir) + "/info.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTestGpkg(gpkg));

  // 1. Default format when called from command line -> text.
  {
    GDALVectorInfoAlgorithmStandalone alg;
    EXPECT_EQ("info", alg.GetName());
    alg.SetCalledFromCommandLine();
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--summary", gpkg}));
    ASSERT_TRUE(alg.Run());
    const std::string out = alg.GetOutputString();
    EXPECT_THAT(out, HasSubstr("pts"));
    EXPECT_THAT(out, HasSubstr("other"));
  }

  // 2. Default format when not called from command line -> json, with
  // --limit and --layer.
  {
    GDALVectorInfoAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--limit", "1", "--layer", "pts", gpkg}));
    ASSERT_TRUE(alg.Run());
    const std::string out = alg.GetOutputString();
    EXPECT_THAT(out, HasSubstr("\"name\":\"pts\""));
    EXPECT_THAT(out, Not(HasSubstr("\"name\":\"other\"")));
  }

  // 3. Explicit --output-format text --features --where.
  {
    GDALVectorInfoAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--output-format", "text", "--features", "--where", "id = 2", gpkg}));
    ASSERT_TRUE(alg.Run());
    const std::string out = alg.GetOutputString();
    EXPECT_THAT(out, HasSubstr("id (Integer) = 2"));
    EXPECT_THAT(out, Not(HasSubstr("id (Integer) = 1")));
  }

  // 4. --sql, --dialect, and --features.
  {
    GDALVectorInfoAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--output-format", "json", "--features", "--sql",
         "SELECT id FROM pts WHERE id = 3", "--dialect", "SQLite", gpkg}));
    ASSERT_TRUE(alg.Run());
    EXPECT_THAT(alg.GetOutputString(), HasSubstr("3"));
  }

  // 5. Deprecated --update emits warning, and --stdout flag works.
  {
    GDALVectorInfoAlgorithmStandalone alg;
    std::string captured_warnings;
    {
      CPLPushErrorHandlerEx(
          [](CPLErr err_class, CPLErrorNum, const char* msg) {
            if (err_class == CE_Warning && msg != nullptr) {
              auto* out =
                  static_cast<std::string*>(CPLGetErrorHandlerUserData());
              out->append(msg);
            }
          },
          &captured_warnings);
      struct PopHandler {
        ~PopHandler() { CPLPopErrorHandler(); }
      } pop_handler;
      CPLErrorReset();
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--update", "--summary", "--stdout", gpkg}));
    }
    EXPECT_THAT(captured_warnings, HasSubstr("deprecated"));
    EXPECT_TRUE(alg.Run());
  }
}

TEST_F(GdalalgVectorInfoTest, PipelineStep) {
  const std::string gpkg = std::string(kTestDir) + "/pipe.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTestGpkg(gpkg));

  GDALVectorPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", gpkg, "!", "info", "--summary", "--output-format", "json"}));
  ASSERT_TRUE(pipeline.Run());
  EXPECT_THAT(pipeline.GetOutputString(), HasSubstr("pts"));
}

}  // namespace
}  // namespace autotest2
