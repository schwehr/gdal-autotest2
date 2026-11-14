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
// Tests for GDALRasterContourAlgorithm (apps/gdalalg_raster_contour.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_contour.cpp

#include "third_party/gdal/apps/gdalalg_raster_contour.h"

#include <array>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_contour_test";

class GdalalgRasterContourTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    tmpdir_setter_ = std::make_unique<CPLConfigOptionSetter>(
        "CPL_TMPDIR", kTestDir, /*bSetOnlyIfUndefined=*/false);
  }

  void TearDown() override {
    tmpdir_setter_.reset();
    VSIRmdirRecursive(kTestDir);
  }

  std::unique_ptr<CPLConfigOptionSetter> tmpdir_setter_;
};

void CreateRampDem(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 2, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALGeoTransform gt{0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  std::array<float, 16> data = {
      0.0f,  5.0f,  10.0f, 15.0f,  //
      5.0f,  10.0f, 15.0f, 20.0f,  //
      10.0f, 15.0f, 20.0f, 25.0f,  //
      15.0f, 20.0f, 25.0f, -9999.0f,
  };
  for (int b = 1; b <= 2; ++b) {
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 4, 4, data.data(),
                                             4, 4, GDT_Float32, 0, 0, nullptr));
  }
}

TEST_F(GdalalgRasterContourTest, StepProperties) {
  GDALRasterContourAlgorithmStandalone alg;
  EXPECT_EQ("contour", alg.GetName());
  EXPECT_FALSE(alg.IsNativelyStreamingCompatible());
  EXPECT_EQ(GDAL_OF_RASTER, alg.GetInputType());
  EXPECT_EQ(GDAL_OF_VECTOR, alg.GetOutputType());
}

TEST_F(GdalalgRasterContourTest, IntervalAndAllLineOptions) {
  const std::string dem_tif = std::string(kTestDir) + "/dem.tif";
  const std::string out_gpkg = std::string(kTestDir) + "/contours.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateRampDem(dem_tif));

  GDALRasterContourAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments({"-i",
                                             dem_tif,
                                             "-o",
                                             out_gpkg,
                                             "-f",
                                             "GPKG",
                                             "--band",
                                             "2",
                                             "--elevation-name",
                                             "elev",
                                             "--3d",
                                             "--src-nodata",
                                             "-9999",
                                             "--interval",
                                             "10",
                                             "--offset",
                                             "2",
                                             "--output-layer",
                                             "my_contours",
                                             "--group-transactions",
                                             "100",
                                             "--co",
                                             "ADD_GPKG_OGR_CONTENTS=YES",
                                             "--lco",
                                             "SPATIAL_INDEX=NO"}));
  bool progress_called = false;
  auto progress_cb = [](double, const char*, void* data) -> int {
    *static_cast<bool*>(data) = true;
    return 1;
  };
  EXPECT_TRUE(alg.Run(progress_cb, &progress_called));
  EXPECT_TRUE(progress_called);
  ASSERT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* lyr = out_ds->GetLayerByName("my_contours");
  ASSERT_NE(nullptr, lyr);
  EXPECT_GT(lyr->GetFeatureCount(), 0);
  EXPECT_NE(-1, lyr->GetLayerDefn()->GetFieldIndex("elev"));
  EXPECT_TRUE(wkbHasZ(lyr->GetGeomType()));
}

TEST_F(GdalalgRasterContourTest, FixedLevelsAndPolygonize) {
  const std::string dem_tif = std::string(kTestDir) + "/dem_poly.tif";
  const std::string out_gpkg = std::string(kTestDir) + "/poly.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateRampDem(dem_tif));

  GDALRasterContourAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"-i", dem_tif, "-o", out_gpkg, "-f", "GPKG", "--levels", "5,15",
       "--polygonize", "--min-name", "elev_min", "--max-name", "elev_max",
       "--src-nodata", "-9999"}));
  EXPECT_TRUE(alg.Run());
  ASSERT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* lyr = out_ds->GetLayerByName("contour");
  ASSERT_NE(nullptr, lyr);
  EXPECT_GT(lyr->GetFeatureCount(), 0);
  EXPECT_NE(-1, lyr->GetLayerDefn()->GetFieldIndex("elev_min"));
  EXPECT_NE(-1, lyr->GetLayerDefn()->GetFieldIndex("elev_max"));
}

TEST_F(GdalalgRasterContourTest, ExponentialBaseLevels) {
  const std::string dem_tif = std::string(kTestDir) + "/dem_exp.tif";
  const std::string out_json = std::string(kTestDir) + "/exp.geojson";
  ASSERT_NO_FATAL_FAILURE(CreateRampDem(dem_tif));

  GDALRasterContourAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments({"-i", dem_tif, "-o", out_json,
                                             "-f", "GeoJSON", "--exp-base", "2",
                                             "--src-nodata", "-9999"}));
  EXPECT_TRUE(alg.Run());
  ASSERT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_json.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* lyr = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);
  EXPECT_GT(lyr->GetFeatureCount(), 0);
}

TEST_F(GdalalgRasterContourTest, ValidationErrors) {
  const std::string dem_tif = std::string(kTestDir) + "/dem_err.tif";
  const std::string out_gpkg = std::string(kTestDir) + "/err.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateRampDem(dem_tif));

  // 1. None of interval / levels / exp-base specified.
  {
    GDALRasterContourAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", dem_tif, "-o", out_gpkg, "-f", "GPKG"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("interval"));
    CPLErrorReset();
  }

  // 2. Non-positive interval rejected by SetMinValueExcluded(0) validation.
  {
    GDALRasterContourAlgorithmStandalone alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"-i", dem_tif, "-o", out_gpkg, "-f", "GPKG", "--interval", "-5"}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("> 0"));
    CPLErrorReset();
  }
  {
    GDALRasterContourAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", dem_tif, "-o", out_gpkg, "-f", "GPKG"}));
    alg.GetArg("interval")->Set(-5.0);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("> 0"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterContourTest, MixedPipelineStepWithGpkgTempFile) {
  const std::string dem_tif = std::string(kTestDir) + "/dem_pipe.tif";
  const std::string out_json = std::string(kTestDir) + "/pipe_out.geojson";
  ASSERT_NO_FATAL_FAILURE(CreateRampDem(dem_tif));

  auto pipeline =
      GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate("pipeline");
  ASSERT_NE(nullptr, pipeline);
  ASSERT_TRUE(pipeline->ParseCommandLineArguments(
      {"read", dem_tif, "!", "contour", "--interval", "10", "--src-nodata",
       "-9999", "!", "write", out_json, "-f", "GeoJSON", "--overwrite"}));
  EXPECT_TRUE(pipeline->Run());
  ASSERT_TRUE(pipeline->Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_json.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* lyr = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);
  EXPECT_GT(lyr->GetFeatureCount(), 0);
}

}  // namespace
}  // namespace autotest2
