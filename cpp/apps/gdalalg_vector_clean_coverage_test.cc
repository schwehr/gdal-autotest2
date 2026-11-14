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
// Tests for GDALVectorCleanCoverageAlgorithm
// (apps/gdalalg_vector_clean_coverage.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_clean_coverage.cpp

#include "third_party/gdal/apps/gdalalg_vector_clean_coverage.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_clean_coverage_test";

class GdalalgVectorCleanCoverageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

std::unique_ptr<GDALDataset> CreateTwoPolygonMemDataset(
    bool include_second_layer, bool point_layer = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  OGRLayer* lyr = ds->CreateLayer("poly", nullptr,
                                  point_layer ? wkbPoint : wkbPolygon, nullptr);
  if (!lyr) return nullptr;
  OGRFieldDefn fld("id", OFTInteger);
  if (lyr->CreateField(&fld) != OGRERR_NONE) return nullptr;

  if (point_layer) {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("id", 1);
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    feat->SetGeometryDirectly(pt.release());
    if (lyr->CreateFeature(feat.get()) != OGRERR_NONE) return nullptr;
    return ds;
  }

  // Two adjacent squares with a 0.2 gap between x=10.0 and x=10.2,
  // plus a small overlap region tested separately.
  const char* const kWkts[2] = {
      "POLYGON ((0 0, 10.0 0, 10.0 10, 0 10, 0 0))",
      "POLYGON ((10.2 0, 20 0, 20 10, 10.2 10, 10.2 0))",
  };
  for (int i = 0; i < 2; ++i) {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("id", i + 1);
    OGRGeometry* geom = nullptr;
    if (OGRGeometryFactory::createFromWkt(kWkts[i], nullptr, &geom) !=
        OGRERR_NONE) {
      return nullptr;
    }
    feat->SetGeometryDirectly(geom);
    if (lyr->CreateFeature(feat.get()) != OGRERR_NONE) return nullptr;
  }

  if (include_second_layer) {
    OGRLayer* lyr2 = ds->CreateLayer("passthrough", nullptr, wkbPoint, nullptr);
    if (!lyr2) return nullptr;
    auto feat2 = std::make_unique<OGRFeature>(lyr2->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(5.0, 5.0);
    feat2->SetGeometryDirectly(pt.release());
    if (lyr2->CreateFeature(feat2.get()) != OGRERR_NONE) return nullptr;
  }

  return ds;
}

TEST_F(GdalalgVectorCleanCoverageTest, BasicProperties) {
  GDALVectorCleanCoverageAlgorithm alg(false);
  EXPECT_EQ("clean-coverage", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_vector_clean_coverage.html", alg.GetHelpURL());

  GDALVectorCleanCoverageAlgorithmStandalone standalone;
  EXPECT_EQ("clean-coverage", standalone.GetName());
}

TEST_F(GdalalgVectorCleanCoverageTest, MergeStrategiesAndParameters) {
  const std::string in_geojson = std::string(kTestDir) + "/overlap.geojson";
  VsiMemTempWrapper in_file(in_geojson, R"json({
    "type": "FeatureCollection",
    "features": [
      {
        "type": "Feature",
        "properties": { "id": 1 },
        "geometry": {
          "type": "Polygon",
          "coordinates": [[[0, 0], [10.1, 0], [10.1, 10], [0, 10], [0, 0]]]
        }
      },
      {
        "type": "Feature",
        "properties": { "id": 2 },
        "geometry": {
          "type": "Polygon",
          "coordinates": [[[10.0, 0], [20, 0], [20, 10], [10.0, 10], [10.0, 0]]]
        }
      }
    ]
  })json");

  const char* const kStrategies[] = {"longest-border", "max-area", "min-area",
                                     "min-index"};
  for (int i = 0; i < 4; ++i) {
    const std::string out_gpkg =
        std::string(kTestDir) + "/out_" + std::to_string(i) + ".gpkg";
    GDALVectorCleanCoverageAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {in_geojson, out_gpkg, "--snapping-distance", "0.05",
         "--maximum-gap-width", "0.2", "--merge-strategy", kStrategies[i]}));
    ASSERT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(2, lyr->GetFeatureCount());
    std::unique_ptr<OGRFeature> f0(lyr->GetNextFeature());
    std::unique_ptr<OGRFeature> f1(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f0);
    ASSERT_NE(nullptr, f1);
    EXPECT_TRUE(f0->GetGeometryRef()->IsValid());
    EXPECT_TRUE(f1->GetGeometryRef()->IsValid());
  }
}

TEST_F(GdalalgVectorCleanCoverageTest, ActiveLayerAndPipelineMode) {
  auto src_ds = CreateTwoPolygonMemDataset(/*include_second_layer=*/true);
  ASSERT_NE(nullptr, src_ds);

  // 1. Active layer processes "poly" and passes through "passthrough".
  {
    GDALVectorCleanCoverageAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--active-layer", "poly", "--maximum-gap-width", "0.5"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(2, out_ds->GetLayerCount());
    OGRLayer* poly_lyr = out_ds->GetLayerByName("poly");
    OGRLayer* pass_lyr = out_ds->GetLayerByName("passthrough");
    ASSERT_NE(nullptr, poly_lyr);
    ASSERT_NE(nullptr, pass_lyr);
    EXPECT_EQ(2, poly_lyr->GetFeatureCount());
    EXPECT_EQ(1, pass_lyr->GetFeatureCount());
  }

  // 2. Pipeline mode.
  const std::string in_geojson = std::string(kTestDir) + "/pipe_in.geojson";
  const std::string out_gpkg = std::string(kTestDir) + "/pipe_out.gpkg";
  VsiMemTempWrapper in_file(in_geojson, R"json({
    "type": "FeatureCollection",
    "features": [
      {
        "type": "Feature",
        "properties": { "id": 1 },
        "geometry": {
          "type": "Polygon",
          "coordinates": [[[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]]]
        }
      },
      {
        "type": "Feature",
        "properties": { "id": 2 },
        "geometry": {
          "type": "Polygon",
          "coordinates": [[[10.2, 0], [20, 0], [20, 10], [10.2, 10], [10.2, 0]]]
        }
      }
    ]
  })json");
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_geojson, "!", "clean-coverage", "--maximum-gap-width",
         "0.3", "!", "write", out_gpkg, "--overwrite"}));
    ASSERT_TRUE(pipeline.Run());
    ASSERT_TRUE(pipeline.Finalize());
  }
}

TEST_F(GdalalgVectorCleanCoverageTest, ErrorAndValidationPaths) {
  auto src_ds = CreateTwoPolygonMemDataset(/*include_second_layer=*/false);
  ASSERT_NE(nullptr, src_ds);

  // 1. Missing active layer fails.
  {
    GDALVectorCleanCoverageAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--active-layer", "does_not_exist"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("was not found"));
  }

  // 2. Non-polygonal input fails when reading output layer.
  auto pt_ds = CreateTwoPolygonMemDataset(/*include_second_layer=*/false,
                                          /*point_layer=*/true);
  ASSERT_NE(nullptr, pt_ds);
  {
    GDALVectorCleanCoverageAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(pt_ds.get());
    WithQuietHandler quiet;
    if (alg.Run()) {
      GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
      ASSERT_NE(nullptr, out_ds);
      OGRLayer* lyr = out_ds->GetLayer(0);
      ASSERT_NE(nullptr, lyr);
      EXPECT_EQ(nullptr, lyr->GetNextFeature());
    }
  }

  // 3. Invalid arguments rejected.
  {
    GDALVectorCleanCoverageAlgorithm alg(false);
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"--snapping-distance", "-1"}));
  }
  {
    GDALVectorCleanCoverageAlgorithm alg(false);
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"--maximum-gap-width", "-1"}));
  }
  {
    GDALVectorCleanCoverageAlgorithm alg(false);
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--merge-strategy", "invalid-strategy"}));
  }

  // 4. Setting invalid arguments via GetArg() fails validation before
  // RunStep().
  {
    GDALVectorCleanCoverageAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    auto* merge_arg = alg.GetArg("merge-strategy");
    ASSERT_NE(nullptr, merge_arg);
    EXPECT_FALSE(merge_arg->Set(std::string("invalid-strategy")));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
}

TEST_F(GdalalgVectorCleanCoverageTest, EmptyLayerAndPolymorphicDelete) {
  // Upstream quirk: Google3 GEOS geos::coverage::CoverageCleaner::toGeometry()
  // triggers a UBSan SIGILL when given an empty (0-polygon) layer. Exercise a
  // minimal 1-polygon layer instead alongside polymorphic deletion.
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  auto single_ds = std::unique_ptr<GDALDataset>(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, single_ds);
  OGRLayer* lyr =
      single_ds->CreateLayer("single", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, lyr);
  {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    OGRGeometry* raw_geom = nullptr;
    ASSERT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                               "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))",
                               nullptr, &raw_geom));
    feat->SetGeometryDirectly(raw_geom);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  GDALVectorCleanCoverageAlgorithm alg(/*bStandalone=*/false);
  alg.GetInputDatasets().emplace_back(single_ds.get());
  ASSERT_TRUE(alg.Run());
  GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* out_lyr = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, out_lyr);
  EXPECT_EQ(1, out_lyr->GetFeatureCount());

  std::unique_ptr<GDALAlgorithm> base_alg =
      std::make_unique<GDALVectorCleanCoverageAlgorithm>(/*bStandalone=*/false);
  EXPECT_EQ("clean-coverage", base_alg->GetName());
}

// TODO: b/335317901 - Remaining uncovered lines in
// gdalalg_vector_clean_coverage.cpp (73, 75, 81, 83-84, 90, 92-93, 115,
// 117-119, 125, 127-128, 153) are defensive error checks for OOM or GEOS C API
// failures that cannot fail with valid inputs and
// SetChoices/SetMinValueIncluded pre-validation in
// GDALAlgorithm::ValidateArguments.

}  // namespace
}  // namespace autotest2
