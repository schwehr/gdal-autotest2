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
// Tests for GDALInfoAlgorithm (apps/gdalalg_info.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_info.cpp
//   https://gdal.org/programs/gdal_info.html

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::UnorderedElementsAre;

constexpr char kTestDir[] = "/vsimem/gdalalg_info_test";

void CreateTinyTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> pixels(16, 15);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

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
      "properties": { "id": 1 }
    }
  ]
})json"));
}

void CreateMixedRasterVectorGpkg(const std::string& path) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr mem_ds(mem_drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mem_ds);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  mem_ds->SetSpatialRef(&srs);
  double gt[6] = {0.0, 0.01, 0.0, 1.0, 0.0, -0.01};
  ASSERT_EQ(CE_None, mem_ds->SetGeoTransform(gt));

  GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, gpkg_drv);
  GDALDatasetUniquePtr gpkg_ds(gpkg_drv->CreateCopy(
      path.c_str(), mem_ds.get(), FALSE, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, gpkg_ds);
  gpkg_ds.reset();

  std::unique_ptr<GDALDataset> update_ds(
      GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr,
                        nullptr, nullptr));
  ASSERT_NE(nullptr, update_ds);
  OGRLayer* layer = update_ds->CreateLayer("vec", &srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);
  ASSERT_EQ(CE_None, update_ds->FlushCache());
}

class GdalalgInfoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    RegisterOGRGeoJSON();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgInfoTest, BasicPropertiesAndUsage) {
  auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("info");
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("info", alg->GetName());
  EXPECT_THAT(alg->GetDescription(),
              HasSubstr("Return information on a dataset"));
  EXPECT_EQ("/programs/gdal_info.html", alg->GetHelpURL());
  EXPECT_THAT(alg->GetLongDescription(), HasSubstr("gdal raster info"));

  const std::string short_usage = alg->GetUsageForCLI(/*shortUsage=*/true, {});
  EXPECT_THAT(short_usage, HasSubstr("Usage:"));

  const std::string full_usage = alg->GetUsageForCLI(/*shortUsage=*/false, {});
  EXPECT_THAT(full_usage, HasSubstr("--output-format"));
  EXPECT_THAT(full_usage, HasSubstr("gdal raster info"));

  const std::string json_usage = alg->GetUsageAsJSON();
  EXPECT_THAT(json_usage, HasSubstr("Return information on a dataset"));

  std::vector<std::string> ac_opts = {"--"};
  EXPECT_THAT(alg->GetAutoComplete(ac_opts, /*lastWordIsComplete=*/false,
                                   /*showAllOptions=*/false),
              Contains("--output-format"));

  std::vector<std::string> ac_fmt = {"--output-format", ""};
  EXPECT_THAT(alg->GetAutoComplete(ac_fmt, /*lastWordIsComplete=*/false,
                                   /*showAllOptions=*/false),
              UnorderedElementsAre("json", "text"));
}

TEST_F(GdalalgInfoTest, DirectRunAndErrorBranches) {
  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("info");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Required argument 'input' has not been specified."));
    CPLErrorReset();
  }

  // Calling Run() directly after setting input without
  // ParseCommandLineArguments reaches GDALInfoAlgorithm::RunImpl() and rejects
  // direct execution.
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    GDALDatasetUniquePtr mem_ds(
        mem_drv->Create("", 2, 2, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mem_ds);
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("info");
    ASSERT_NE(nullptr, alg);
    auto* input_arg = alg->GetArg(GDAL_ARG_NAME_INPUT);
    ASSERT_NE(nullptr, input_arg);
    ASSERT_TRUE(input_arg->Set(mem_ds.get()));

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("should not be called directly"));
    CPLErrorReset();
  }

  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("info");
    ASSERT_NE(nullptr, alg);
    EXPECT_TRUE(alg->ParseCommandLineArguments({"--help"}));
    EXPECT_TRUE(alg->IsHelpRequested());
  }

  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("info");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->ParseCommandLineArguments(
        {std::string(kTestDir) + "/missing.tif"}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  {
    const std::string gpkg_path = std::string(kTestDir) + "/mixed.gpkg";
    ASSERT_NO_FATAL_FAILURE(CreateMixedRasterVectorGpkg(gpkg_path));

    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("info");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->ParseCommandLineArguments({gpkg_path}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("has both raster and vector content"));
    EXPECT_TRUE(alg->GetUsageForCLI(/*shortUsage=*/false, {}).empty());
    CPLErrorReset();
  }
}

TEST_F(GdalalgInfoTest, DispatchRasterInfo) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));

  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("info");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"-f", "text", in_tif}));
    EXPECT_THAT(alg->GetUsageForCLI(/*shortUsage=*/false, {}),
                HasSubstr("raster"));
    ASSERT_TRUE(alg->Run());
    const auto* out_arg =
        alg->GetActualAlgorithm().GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    EXPECT_THAT(out_arg->Get<std::string>(), HasSubstr("Driver: GTiff"));
  }

  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("info");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"-f", "json", in_tif}));
    ASSERT_TRUE(alg->Run());
    const auto* out_arg =
        alg->GetActualAlgorithm().GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    EXPECT_THAT(out_arg->Get<std::string>(),
                HasSubstr("\"driverShortName\":\"GTiff\""));
  }
}

TEST_F(GdalalgInfoTest, DispatchVectorInfo) {
  const std::string in_geojson = std::string(kTestDir) + "/in.geojson";
  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoJson(in_geojson));

  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("info");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"-f", "text", in_geojson}));
    EXPECT_THAT(alg->GetUsageForCLI(/*shortUsage=*/false, {}),
                HasSubstr("vector"));
    ASSERT_TRUE(alg->Run());
    const auto* out_arg =
        alg->GetActualAlgorithm().GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    EXPECT_THAT(out_arg->Get<std::string>(), HasSubstr("GeoJSON"));
  }

  {
    auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("info");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->ParseCommandLineArguments({"-f", "json", in_geojson}));
    ASSERT_TRUE(alg->Run());
    const auto* out_arg =
        alg->GetActualAlgorithm().GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
    ASSERT_NE(nullptr, out_arg);
    EXPECT_THAT(out_arg->Get<std::string>(),
                HasSubstr("\"driverShortName\":\"GeoJSON\""));
  }
}

}  // namespace
}  // namespace autotest2
