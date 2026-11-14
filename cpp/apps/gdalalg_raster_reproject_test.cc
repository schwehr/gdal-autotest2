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
// Tests for GDALRasterReprojectAlgorithm (apps/gdalalg_raster_reproject.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_reproject.cpp
//   https://gdal.org/programs/gdal_raster_reproject.html

#include "third_party/gdal/apps/gdalalg_raster_reproject.h"

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
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalalgorithm.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_reproject_test";

void CreateGeoTiff4326(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ds->SetSpatialRef(&srs);

  double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

  std::vector<uint8_t> pixels(100, 55);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 10,
                                                    pixels.data(), 10, 10,
                                                    GDT_Byte, 0, 0, nullptr));
}

class GdalalgRasterReprojectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterReprojectTest, BasicPropertiesAndAutoComplete) {
  GDALRasterReprojectAlgorithmStandalone alg;
  EXPECT_EQ("reproject", alg.GetName());
  EXPECT_EQ("Reproject a raster dataset.", alg.GetDescription());
  EXPECT_EQ("/programs/gdal_raster_reproject.html", alg.GetHelpURL());
  EXPECT_THAT(
      GDALRasterReprojectAlgorithm::GetAliasesStatic(),
      ElementsAre(GDALAlgorithmRegistry::HIDDEN_ALIAS_SEPARATOR, "warp"));

  const auto* wo_arg = alg.GetArg("warp-option");
  ASSERT_NE(nullptr, wo_arg);
  EXPECT_FALSE(wo_arg->GetAutoCompleteChoices("").empty());

  const auto* to_arg = alg.GetArg("transform-option");
  ASSERT_NE(nullptr, to_arg);
  EXPECT_FALSE(to_arg->GetAutoCompleteChoices("").empty());
}

TEST_F(GdalalgRasterReprojectTest, ValidationAndMutuallyExclusiveNumThreads) {
  const std::string in_tif = std::string(kTestDir) + "/val_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/val_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGeoTiff4326(in_tif));

  // Invalid key=value for warp-option and transform-option.
  {
    GDALRasterReprojectAlgorithmStandalone alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--warp-option", "NO_EQUALS", in_tif, out_tif}));
  }
  {
    GDALRasterReprojectAlgorithmStandalone alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--transform-option", "NO_EQUALS", in_tif, out_tif}));
  }

  // --num-threads conflicts with --warp-option NUM_THREADS=1.
  {
    GDALRasterReprojectAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--dst-crs", "EPSG:3857", "--num-threads", "2", "--warp-option",
         "NUM_THREADS=1", "--overwrite", in_tif, out_tif}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("mutually exclusive"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterReprojectTest, StandaloneOptionsAndResolutionTap) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_size = std::string(kTestDir) + "/out_size.tif";
  const std::string out_res = std::string(kTestDir) + "/out_res.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGeoTiff4326(in_tif));

  // 1. Exercise --src-crs, --dst-crs, --resampling, --size, --bbox,
  //    --bbox-crs, --src-nodata, --dst-nodata, --add-alpha, --wo, --to, --et.
  {
    GDALRasterReprojectAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--src-crs",
                                               "EPSG:4326",
                                               "--dst-crs",
                                               "EPSG:3857",
                                               "--resampling",
                                               "bilinear",
                                               "--size",
                                               "8,8",
                                               "--bbox",
                                               "2.0,48.9,2.1,49.0",
                                               "--bbox-crs",
                                               "EPSG:4326",
                                               "--src-nodata",
                                               "0",
                                               "--dst-nodata",
                                               "255",
                                               "--add-alpha",
                                               "--wo",
                                               "INIT_DEST=255",
                                               "--to",
                                               "METHOD=GEOTRANSFORM",
                                               "--et",
                                               "0.125",
                                               "--num-threads",
                                               "1",
                                               "--overwrite",
                                               in_tif,
                                               out_size}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_size.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(8, ds->GetRasterXSize());
    EXPECT_EQ(8, ds->GetRasterYSize());
    EXPECT_EQ(2, ds->GetRasterCount());
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    OGRSpatialReference expected;
    ASSERT_EQ(OGRERR_NONE, expected.importFromEPSG(3857));
    EXPECT_TRUE(srs->IsSame(&expected));
  }

  // 2. Exercise --resolution and --target-aligned-pixels.
  {
    GDALRasterReprojectAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--dst-crs", "EPSG:3857", "--resolution", "1000,1000", "--tap",
         "--overwrite", in_tif, out_res}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_res.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {};
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(1000.0, gt[1]);
    EXPECT_DOUBLE_EQ(-1000.0, gt[5]);
  }
}

TEST_F(GdalalgRasterReprojectTest, PipelineCanHandleNextStepAndVrtFallback) {
  const std::string in_tif = std::string(kTestDir) + "/pipe_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/pipe_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGeoTiff4326(in_tif));

  // 1. Pipeline with write GTiff step -> exercises CanHandleNextStep(write)
  //    with creation options and NUM_THREADS forwarding.
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "reproject", "--dst-crs", "EPSG:3857",
         "--num-threads", "2", "!", "write", out_tif, "--overwrite",
         "--creation-option", "COMPRESS=LZW"}));
    ASSERT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_GT(ds->GetRasterXSize(), 0);
  }

  // 2. Pipeline without write step -> exercises else branch (-of VRT).
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "reproject", "--dst-crs", "EPSG:3857"}));
    ASSERT_TRUE(pipeline.Run());
    GDALDataset* ds = pipeline.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    ASSERT_NE(nullptr, ds->GetDriver());
    EXPECT_STREQ("VRT", ds->GetDriver()->GetDescription());
    EXPECT_TRUE(pipeline.Finalize());
  }

  // 3. Pipeline with write --output-format stream -> CanHandleNextStep returns
  //    false for stream output.
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_tif, "!", "reproject", "--dst-crs", "EPSG:3857", "!",
         "write", "streamed", "--output-format", "stream"}));
    ASSERT_TRUE(pipeline.Run());
    EXPECT_NE(nullptr, pipeline.GetOutputDataset().GetDatasetRef());
    EXPECT_TRUE(pipeline.Finalize());
  }
}

}  // namespace
}  // namespace autotest2
