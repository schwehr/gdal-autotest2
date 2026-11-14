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
// Tests for GDALConvertAlgorithm (apps/gdalalg_convert.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_convert.cpp
//   https://gdal.org/programs/gdal_convert.html

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

constexpr char kTestDir[] = "/vsimem/gdalalg_convert_test";

void CreateTinyTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
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

class GdalalgConvertTest : public ::testing::Test {
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

TEST_F(GdalalgConvertTest, BasicPropertiesAliasesAndUsage) {
  auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("convert");
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("convert", alg->GetName());
  EXPECT_THAT(alg->GetDescription(), HasSubstr("Convert a dataset"));
  EXPECT_EQ("/programs/gdal_convert.html", alg->GetHelpURL());
  EXPECT_THAT(alg->GetLongDescription(), HasSubstr("gdal raster convert"));
  EXPECT_THAT(alg->GetAliases(), Contains("translate"));

  auto alias_alg =
      GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("translate");
  ASSERT_NE(nullptr, alias_alg);
  EXPECT_EQ("convert", alias_alg->GetName());

  const std::string short_usage = alg->GetUsageForCLI(/*shortUsage=*/true, {});
  EXPECT_THAT(short_usage, HasSubstr("Usage:"));

  const std::string full_usage = alg->GetUsageForCLI(/*shortUsage=*/false, {});
  EXPECT_THAT(full_usage, HasSubstr("--output-format"));
  EXPECT_THAT(full_usage, HasSubstr("gdal raster convert"));

  const std::string json_usage = alg->GetUsageAsJSON();
  EXPECT_THAT(json_usage, HasSubstr("Convert a dataset"));

  std::vector<std::string> ac_opts = {"--"};
  EXPECT_THAT(alg->GetAutoComplete(ac_opts, /*lastWordIsComplete=*/false,
                                   /*showAllOptions=*/false),
              Contains("--output-format"));
}

TEST_F(GdalalgConvertTest, DirectRunAndErrorBranches) {
  {
    const std::string in_tif = std::string(kTestDir) + "/direct_in.tif";
    const std::string out_tif = std::string(kTestDir) + "/direct_out.tif";
    ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));

    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("convert");
    ASSERT_NE(nullptr, alg);
    ASSERT_TRUE(alg->GetArg("input")->Set(in_tif));
    ASSERT_TRUE(alg->GetArg("output")->Set(out_tif));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Run() method should not be called directly"));
    CPLErrorReset();
  }

  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("convert");
    ASSERT_NE(nullptr, alg);
    EXPECT_TRUE(alg->ParseCommandLineArguments({"--help"}));
    EXPECT_TRUE(alg->IsHelpRequested());
  }

  {
    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("convert");
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
    const std::string out_path = std::string(kTestDir) + "/mixed_out.tif";

    auto alg =
        GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("convert");
    ASSERT_NE(nullptr, alg);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->ParseCommandLineArguments({gpkg_path, out_path}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("has both raster and vector content"));
    EXPECT_TRUE(alg->GetUsageForCLI(/*shortUsage=*/false, {}).empty());
    CPLErrorReset();
  }
}

TEST_F(GdalalgConvertTest, DispatchRasterConvert) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));

  auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("convert");
  ASSERT_NE(nullptr, alg);
  ASSERT_TRUE(alg->ParseCommandLineArguments(
      {"-f", "GTiff", in_tif, out_tif, "--overwrite"}));
  EXPECT_THAT(alg->GetUsageForCLI(/*shortUsage=*/false, {}),
              HasSubstr("raster"));
  ASSERT_TRUE(alg->Run());
  ASSERT_TRUE(alg->Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, out_ds);
  EXPECT_EQ(4, out_ds->GetRasterXSize());
  EXPECT_EQ(4, out_ds->GetRasterYSize());
  EXPECT_EQ(1, out_ds->GetRasterCount());
  uint8_t pixel = 0;
  EXPECT_EQ(CE_None,
            out_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &pixel, 1,
                                               1, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(42, pixel);
}

TEST_F(GdalalgConvertTest, DispatchVectorConvert) {
  const std::string in_geojson = std::string(kTestDir) + "/in.geojson";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoJson(in_geojson));

  auto alg = GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("convert");
  ASSERT_NE(nullptr, alg);
  ASSERT_TRUE(alg->ParseCommandLineArguments(
      {"-f", "GPKG", in_geojson, out_gpkg, "--overwrite"}));
  EXPECT_THAT(alg->GetUsageForCLI(/*shortUsage=*/false, {}),
              HasSubstr("vector"));
  ASSERT_TRUE(alg->Run());
  ASSERT_TRUE(alg->Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, out_ds);
  ASSERT_EQ(1, out_ds->GetLayerCount());
  EXPECT_EQ(1, out_ds->GetLayer(0)->GetFeatureCount());
}

}  // namespace
}  // namespace autotest2
