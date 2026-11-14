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
// Tests for GDALVectorSimplifyCoverageAlgorithm
// (apps/gdalalg_vector_simplify_coverage.cpp and
// apps/gdalalg_vector_simplify_coverage.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_simplify_coverage.cpp

#include "third_party/gdal/apps/gdalalg_vector_simplify_coverage.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
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

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_simplify_coverage_test";

std::unique_ptr<GDALDataset> CreateCoverageDataset(bool include_point_layer) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  EXPECT_NE(nullptr, mem_drv);
  if (!mem_drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  EXPECT_NE(nullptr, ds);
  if (!ds) return nullptr;

  OGRLayer* poly_lyr = ds->CreateLayer("polys", nullptr, wkbPolygon, nullptr);
  EXPECT_NE(nullptr, poly_lyr);
  const char* wkts[] = {
      "POLYGON ((0 0, 5 0.1, 10 0, 10 5, 10 10, 5 10, 0 10, 0 0))",
      "POLYGON ((0 10, 5 10, 10 10, 10 20, 0 20, 0 10))",
  };
  for (const char* wkt : wkts) {
    OGRGeometry* g = nullptr;
    EXPECT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(wkt, nullptr, &g));
    auto feat = std::make_unique<OGRFeature>(poly_lyr->GetLayerDefn());
    EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(g));
    EXPECT_EQ(OGRERR_NONE, poly_lyr->CreateFeature(feat.get()));
  }

  if (include_point_layer) {
    OGRLayer* pts_lyr = ds->CreateLayer("pts", nullptr, wkbPoint, nullptr);
    EXPECT_NE(nullptr, pts_lyr);
    auto feat = std::make_unique<OGRFeature>(pts_lyr->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
    EXPECT_EQ(OGRERR_NONE, pts_lyr->CreateFeature(feat.get()));
  }

  return ds;
}

TEST(GdalalgVectorSimplifyCoverageTest, PropertiesAndValidation) {
  GDALVectorSimplifyCoverageAlgorithmStandalone alg;
  EXPECT_EQ("simplify-coverage", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_FALSE(alg.GetHelpURL().empty());

  WithQuietHandler quiet;
  EXPECT_FALSE(alg.ParseCommandLineArguments({"in.gpkg", "out.gpkg"}));
  EXPECT_FALSE(
      alg.ParseCommandLineArguments({"--tolerance=-1", "in.gpkg", "out.gpkg"}));
}

TEST(GdalalgVectorSimplifyCoverageTest, SimplifyAndPreserveBoundary) {
  GDALRegister_MEM();
  std::unique_ptr<GDALDataset> src_ds =
      CreateCoverageDataset(/*include_point_layer=*/true);
  ASSERT_NE(nullptr, src_ds);

  // 1. Simplify active layer 'polys' while passing through 'pts'.
  {
    GDALVectorSimplifyCoverageAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(src_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.GetArg("active-layer")->Set("polys"));
    ASSERT_TRUE(alg.GetArg("tolerance")->Set(1.0));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("stream"));
    EXPECT_TRUE(alg.Run());

    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    ASSERT_EQ(2, out_ds->GetLayerCount());
    OGRLayer* poly_lyr = out_ds->GetLayerByName("polys");
    ASSERT_NE(nullptr, poly_lyr);
    std::unique_ptr<OGRFeature> f1(poly_lyr->GetNextFeature());
    ASSERT_NE(nullptr, f1);
    EXPECT_LT(
        f1->GetGeometryRef()->toPolygon()->getExteriorRing()->getNumPoints(),
        8);
    OGRLayer* pts_lyr = out_ds->GetLayerByName("pts");
    ASSERT_NE(nullptr, pts_lyr);
    EXPECT_EQ(1, pts_lyr->GetFeatureCount());
  }

  // 2. --preserve-boundary keeps exterior collinear vertex on bottom edge.
  {
    GDALVectorSimplifyCoverageAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(src_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.GetArg("active-layer")->Set("polys"));
    ASSERT_TRUE(alg.GetArg("tolerance")->Set(1.0));
    ASSERT_TRUE(alg.GetArg("preserve-boundary")->Set(true));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("stream"));
    EXPECT_TRUE(alg.Run());

    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* poly_lyr = out_ds->GetLayerByName("polys");
    ASSERT_NE(nullptr, poly_lyr);
    std::unique_ptr<OGRFeature> f1(poly_lyr->GetNextFeature());
    ASSERT_NE(nullptr, f1);
    EXPECT_GE(
        f1->GetGeometryRef()->toPolygon()->getExteriorRing()->getNumPoints(),
        7);
  }
}

TEST(GdalalgVectorSimplifyCoverageTest, ErrorPathsAndPipeline) {
  GDALRegister_MEM();
  RegisterOGRGeoPackage();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  std::unique_ptr<GDALDataset> src_ds =
      CreateCoverageDataset(/*include_point_layer=*/true);
  ASSERT_NE(nullptr, src_ds);

  // 1. Specified active-layer not found fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVectorSimplifyCoverageAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(src_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.GetArg("active-layer")->Set("nonexistent"));
    ASSERT_TRUE(alg.GetArg("tolerance")->Set(1.0));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("stream"));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("was not found"));
    CPLErrorReset();
  }

  // 2. Non-polygonal layer fails PolygonsOnly() check.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVectorSimplifyCoverageAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(src_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.GetArg("active-layer")->Set("pts"));
    ASSERT_TRUE(alg.GetArg("tolerance")->Set(1.0));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("stream"));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 3. Pipeline execution writing GPKG.
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_NE(nullptr, drv);
    GDALDatasetUniquePtr ds(
        drv->Create(in_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->CreateLayer("polys", nullptr, wkbPolygon, nullptr);
    ASSERT_NE(nullptr, lyr);
    OGRGeometry* g = nullptr;
    ASSERT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                               "POLYGON ((0 0, 5 0.1, 10 0, 10 10, 0 10, 0 0))",
                               nullptr, &g));
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetGeometryDirectly(g);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "simplify-coverage", "1.0", "!", "write",
         out_gpkg, "--overwrite"}));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());
  }

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* lyr = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);
  EXPECT_EQ(1, lyr->GetFeatureCount());
}

}  // namespace
}  // namespace autotest2
