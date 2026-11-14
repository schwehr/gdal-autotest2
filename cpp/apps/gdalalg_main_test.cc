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
// Tests for GDALMainAlgorithm (apps/gdalalg_main.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_main.cpp

#include "third_party/gdal/apps/gdalalg_main.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_main_test";

// Mock sub-algorithm with a non-colliding name for typo suggestion testing.
class MockSubAlgorithm final : public GDALAlgorithm {
 public:
  static constexpr const char* NAME = "mocksubcommand";
  static constexpr const char* DESCRIPTION = "Mock subcommand";
  static constexpr const char* HELP_URL = "";

  static std::vector<std::string> GetAliasesStatic() { return {}; }

  MockSubAlgorithm() : GDALAlgorithm(NAME, DESCRIPTION, HELP_URL) {}

 protected:
  bool RunImpl(GDALProgressFunc, void*) override { return true; }
};

void CreateTinyTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> pixels(16, 42);
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

void CreateTinyGeoJson(const std::string& path) {
  WriteVsimemFile(path,
                  R"json({
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [1.0, 2.0] },
      "properties": { "id": 1 }
    }
  ]
})json");
}

void CreateMixedRasterVectorGpkg(const std::string& path) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  std::unique_ptr<GDALDataset> mem_ds(
      mem_drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mem_ds);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  mem_ds->SetSpatialRef(&srs);
  double gt[6] = {0.0, 0.01, 0.0, 1.0, 0.0, -0.01};
  ASSERT_EQ(CE_None, mem_ds->SetGeoTransform(gt));

  GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, gpkg_drv);
  std::unique_ptr<GDALDataset> gpkg_ds(gpkg_drv->CreateCopy(
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
  update_ds.reset();
}

class GdalalgMainTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    auto& registry = GDALGlobalAlgorithmRegistry::GetSingleton();
    registry.Register<MockSubAlgorithm>();
  }

  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgMainTest, BasicPropertiesAndUsage) {
  GDALMainAlgorithm alg;
  EXPECT_STREQ("gdal", alg.GetName().c_str());
  EXPECT_STREQ("Main gdal entry point.", alg.GetDescription().c_str());
  EXPECT_STREQ("/programs/index.html", alg.GetHelpURL().c_str());

  const std::string usage = alg.GetUsageForCLI(/*shortUsage=*/false, {});
  EXPECT_NE(std::string::npos, usage.find("gdal <FILENAME>"));
}

TEST_F(GdalalgMainTest, VersionAndDriversArguments) {
  {
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"--version"}));
  }

  {
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"--drivers"}));
    EXPECT_TRUE(alg.Run());
    const std::string output =
        alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING)->Get<std::string>();
    EXPECT_FALSE(output.empty());
  }

  {
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.Run());
  }
}

TEST_F(GdalalgMainTest, DoubleDashOptions) {
  GDALMainAlgorithm alg;
  EXPECT_TRUE(alg.ParseCommandLineArguments({"--help"}));

  GDALMainAlgorithm alg_bad;
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(alg_bad.ParseCommandLineArguments({"--unknown_option_xyz"}));
  }
}

TEST_F(GdalalgMainTest, SubcommandAndTypoSuggestion) {
  // Valid subcommand.
  {
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"mocksubcommand"}));
  }

  // Subcommand typo triggering "Do you mean 'mocksubcommand'?"
  {
    GDALMainAlgorithm alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"mocksubcommane"}));
  }
}

TEST_F(GdalalgMainTest, ShortestFormPipelineRead) {
  const std::string tif_path = std::string(kTestDir) + "/pipe.tif";
  CreateTinyTiff(tif_path);

  const std::string geojson_path = std::string(kTestDir) + "/pipe.geojson";
  CreateTinyGeoJson(geojson_path);

  // Valid shortest-form raster pipeline read.
  {
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"read", tif_path, "!", "info"}));
    EXPECT_FALSE(alg.GetUsageForCLI(/*shortUsage=*/false, {}).empty());
  }

  // Valid shortest-form vector pipeline read.
  {
    GDALMainAlgorithm alg;
    EXPECT_TRUE(
        alg.ParseCommandLineArguments({"read", geojson_path, "!", "info"}));
    EXPECT_FALSE(alg.GetUsageForCLI(/*shortUsage=*/false, {}).empty());
  }

  // Normal failure falls through.
  {
    CPLErrorReset();
    GDALMainAlgorithm alg;
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(
          alg.ParseCommandLineArguments({"read", "/vsimem/no_such_input.tif"}));
    }
  }

  // Only "read" without second arg does not trigger pipeline branch.
  {
    GDALMainAlgorithm alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"read"}));
  }
}

TEST_F(GdalalgMainTest, ShortestFormInfoDirectFile) {
  const std::string tif_path = std::string(kTestDir) + "/direct.tif";
  CreateTinyTiff(tif_path);

  const std::string geojson_path = std::string(kTestDir) + "/direct.geojson";
  CreateTinyGeoJson(geojson_path);

  const std::string gpkg_path = std::string(kTestDir) + "/mixed_info.gpkg";
  CreateMixedRasterVectorGpkg(gpkg_path);

  const std::string invalid_path = std::string(kTestDir) + "/corrupt.bin";
  WriteVsimemFile(invalid_path, "not a dataset");

  // Raster path (no "layer" arg in info algorithm).
  {
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({tif_path}));
    EXPECT_NE(std::string::npos,
              alg.GetUsageForCLI(/*shortUsage=*/false, {}).find("raster"));
  }

  // Vector path ("layer" arg present in info algorithm).
  {
    GDALMainAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({geojson_path}));
    EXPECT_NE(std::string::npos,
              alg.GetUsageForCLI(/*shortUsage=*/false, {}).find("vector"));
  }

  // Raster/vector error message suppresses usage.
  {
    CPLErrorReset();
    GDALMainAlgorithm alg;
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.ParseCommandLineArguments({gpkg_path}));
    }
    EXPECT_TRUE(alg.GetUsageForCLI(/*shortUsage=*/false, {}).empty());
  }

  // Normal failure on unrecognized file falls through.
  {
    CPLErrorReset();
    GDALMainAlgorithm alg;
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.ParseCommandLineArguments({invalid_path}));
    }
  }

  // Non-existent file falls through.
  {
    GDALMainAlgorithm alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {std::string(kTestDir) + "/missing.tif"}));
  }
}

}  // namespace
}  // namespace autotest2
