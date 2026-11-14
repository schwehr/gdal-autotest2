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
// Tests for GDALVectorAlgorithm (apps/gdalalg_vector.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector.cpp
//   https://gdal.org/programs/gdal_vector.html

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::IsSupersetOf;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_test";

void WriteVsimemFile(const std::string& path, const std::string& content) {
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  ASSERT_EQ(content.size(), VSIFWriteL(content.data(), 1, content.size(), fp));
}

void CreateTinyGeoJson(const std::string& path) {
  ASSERT_NO_FATAL_FAILURE(WriteVsimemFile(path,
                                          R"json({
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [1.0, 2.0] },
      "properties": { "name": "pt1" }
    }
  ]
})json"));
}

class GdalalgVectorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorTest, BasicPropertiesSubAlgorithmsAndUsage) {
  auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("vector");
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("vector", alg->GetName());
  EXPECT_EQ("Vector commands.", alg->GetDescription());
  EXPECT_EQ("/programs/gdal_vector.html", alg->GetHelpURL());
  EXPECT_TRUE(alg->HasSubAlgorithms());

  EXPECT_THAT(alg->GetSubAlgorithmNames(),
              IsSupersetOf({"info", "convert", "buffer", "clip", "filter",
                            "pipeline", "reproject", "select", "sql"}));

  const std::string short_usage = alg->GetUsageForCLI(/*shortUsage=*/true, {});
  EXPECT_THAT(short_usage, HasSubstr("info"));
  EXPECT_THAT(short_usage, HasSubstr("convert"));

  const std::string full_usage = alg->GetUsageForCLI(/*shortUsage=*/false, {});
  EXPECT_THAT(full_usage, HasSubstr("--drivers"));
  EXPECT_THAT(full_usage, HasSubstr("pipeline"));

  const std::string json_usage = alg->GetUsageAsJSON();
  EXPECT_THAT(json_usage, HasSubstr("Vector commands."));
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

TEST_F(GdalalgVectorTest, DriversOptionAndDirectRunError) {
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("vector");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"--drivers"}));
    ASSERT_TRUE(alg->Run());
    const auto* out_arg = alg->GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    const std::string output = out_arg->Get<std::string>();
    EXPECT_FALSE(output.empty());
    EXPECT_THAT(output, HasSubstr("GeoJSON"));
  }

  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("vector");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("The Run() method should not be called directly on the "
                  "\"gdal vector\" program."));
    CPLErrorReset();
  }
}

TEST_F(GdalalgVectorTest, ExecuteSubcommands) {
  const std::string in_geojson = std::string(kTestDir) + "/in.geojson";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoJson(in_geojson));

  // 1. vector info
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("vector");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"info", in_geojson}));
    ASSERT_TRUE(alg->Run());
    const auto* out_arg =
        alg->GetActualAlgorithm().GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    EXPECT_THAT(out_arg->Get<std::string>(), HasSubstr("GeoJSON"));
  }

  // 2. vector convert
  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("vector");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments(
        {"convert", in_geojson, out_gpkg, "--overwrite"}));
    ASSERT_TRUE(alg->Run());
    ASSERT_TRUE(alg->Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    ASSERT_EQ(1, out_ds->GetLayerCount());
    EXPECT_EQ(1, out_ds->GetLayer(0)->GetFeatureCount());
  }
}

}  // namespace
}  // namespace autotest2
