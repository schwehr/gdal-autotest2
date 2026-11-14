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
// Tests for GDALVectorReprojectAlgorithm (apps/gdalalg_vector_reproject.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_reproject.cpp

#include "third_party/gdal/apps/gdalalg_vector_reproject.h"

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_reproject_test";

class GdalalgVectorReprojectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateGpkgWithTwoLayers(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs_4326;
  ASSERT_EQ(OGRERR_NONE, srs_4326.importFromEPSG(4326));
  srs_4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto add_point_layer = [&](const char* name, OGRSpatialReference* srs,
                             double x, double y) {
    OGRLayer* lyr = ds->CreateLayer(name, srs, wkbPoint, nullptr);
    ASSERT_NE(nullptr, lyr);
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(x, y);
    ASSERT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  };

  ASSERT_NO_FATAL_FAILURE(add_point_layer("l1", &srs_4326, 2.0, 49.0));
  ASSERT_NO_FATAL_FAILURE(add_point_layer("l2", &srs_4326, 3.0, 50.0));
}

TEST_F(GdalalgVectorReprojectTest, Properties) {
  GDALVectorReprojectAlgorithmStandalone alg;
  EXPECT_EQ("reproject", alg.GetName());
  EXPECT_NE(nullptr, alg.GetArg("src-crs"));
  EXPECT_NE(nullptr, alg.GetArg("dst-crs"));
  EXPECT_NE(nullptr, alg.GetArg("active-layer"));
}

TEST_F(GdalalgVectorReprojectTest, ReprojectUsingLayerSrsAndActiveLayer) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_all = std::string(kTestDir) + "/out_all.gpkg";
  const std::string out_active = std::string(kTestDir) + "/out_active.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateGpkgWithTwoLayers(in_gpkg));

  OGRSpatialReference expected_3857;
  ASSERT_EQ(OGRERR_NONE, expected_3857.importFromEPSG(3857));
  OGRSpatialReference expected_4326;
  ASSERT_EQ(OGRERR_NONE, expected_4326.importFromEPSG(4326));
  expected_4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // 1. Reproject all layers to EPSG:3857.
  {
    GDALVectorReprojectAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"-i", in_gpkg, "-o", out_all, "-f",
                                       "GPKG", "--dst-crs", "EPSG:3857"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_all.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(2, ds->GetLayerCount());
    OGRLayer* l1 = ds->GetLayerByName("l1");
    ASSERT_NE(nullptr, l1);
    ASSERT_NE(nullptr, l1->GetSpatialRef());
    EXPECT_TRUE(l1->GetSpatialRef()->IsSame(&expected_3857));
    std::unique_ptr<OGRFeature> feat(l1->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    const OGRPoint* pt = feat->GetGeometryRef()->toPoint();
    EXPECT_GT(pt->getX(), 200000.0);
    EXPECT_GT(pt->getY(), 6000000.0);
  }

  // 2. Reproject only active layer "l1", leaving "l2" untouched via
  // GDALVectorPipelinePassthroughLayer.
  {
    GDALVectorReprojectAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-i", in_gpkg, "-o", out_active,
                                               "-f", "GPKG", "--active-layer",
                                               "l1", "--t_srs", "EPSG:3857"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_active.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    OGRLayer* l1 = ds->GetLayerByName("l1");
    OGRLayer* l2 = ds->GetLayerByName("l2");
    ASSERT_NE(nullptr, l1);
    ASSERT_NE(nullptr, l2);
    EXPECT_TRUE(l1->GetSpatialRef()->IsSame(&expected_3857));
    EXPECT_TRUE(l2->GetSpatialRef()->IsSame(&expected_4326));
    std::unique_ptr<OGRFeature> f2(l2->GetNextFeature());
    ASSERT_NE(nullptr, f2);
    const OGRPoint* pt2 = f2->GetGeometryRef()->toPoint();
    EXPECT_NEAR(3.0, pt2->getX(), 1e-6);
    EXPECT_NEAR(50.0, pt2->getY(), 1e-6);
  }
}

TEST_F(GdalalgVectorReprojectTest, SrcCrsOverrideAndMissingSrsError) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  std::unique_ptr<GDALDataset> mem_ds(
      mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, mem_ds);
  OGRLayer* lyr = mem_ds->CreateLayer("no_srs", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, lyr);
  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  auto pt = std::make_unique<OGRPoint>(2.0, 49.0);
  ASSERT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
  ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));

  // 1. Missing layer SRS without --src-crs fails.
  {
    GDALVectorReprojectAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-o", "/vsimem/gdalalg_vector_reproject_test/unused.gpkg", "-f",
         "GPKG", "--dst-crs", "EPSG:3857"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("no spatial reference"));
    CPLErrorReset();
  }

  // 2. Explicit --src-crs (via alias --s_srs) succeeds.
  {
    const std::string out_gpkg = std::string(kTestDir) + "/with_src_crs.gpkg";
    GDALVectorReprojectAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(mem_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"-o", out_gpkg, "-f", "GPKG", "--s_srs",
                                       "EPSG:4326", "--dst-crs", "EPSG:3857"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    OGRLayer* out_lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, out_lyr);
    OGRSpatialReference expected_3857;
    ASSERT_EQ(OGRERR_NONE, expected_3857.importFromEPSG(3857));
    EXPECT_TRUE(out_lyr->GetSpatialRef()->IsSame(&expected_3857));
  }
}

TEST_F(GdalalgVectorReprojectTest, PipelineUsageAndInvalidCrs) {
  const std::string in_gpkg = std::string(kTestDir) + "/pipe_in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/pipe_out.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateGpkgWithTwoLayers(in_gpkg));

  GDALVectorPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_gpkg, "!", "reproject", "--dst-crs", "EPSG:3857", "!",
       "write", out_gpkg, "-f", "GPKG", "--overwrite"}));
  EXPECT_TRUE(pipeline.Run());
  ASSERT_TRUE(pipeline.Finalize());

  GDALVectorReprojectAlgorithmStandalone bad_alg;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(bad_alg.ParseCommandLineArguments(
      {"-i", in_gpkg, "-o", out_gpkg, "--dst-crs", "INVALID:999999"}));
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
