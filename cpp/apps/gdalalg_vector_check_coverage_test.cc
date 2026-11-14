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
// Tests for GDALVectorCheckCoverageAlgorithm
// (apps/gdalalg_vector_check_coverage.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_check_coverage.cpp
//   https://gdal.org/programs/gdal_vector_check_coverage.html

#include "third_party/gdal/apps/gdalalg_vector_check_coverage.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
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

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_check_coverage_test";

void CreateGpkgFromWkts(const std::string& path, const char* layer_name,
                        OGRwkbGeometryType geom_type,
                        const std::vector<std::string>& wkts) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRLayer* layer = ds->CreateLayer(layer_name, &srs, geom_type, nullptr);
  ASSERT_NE(nullptr, layer);
  for (const std::string& wkt : wkts) {
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    if (!wkt.empty()) {
      OGRGeometry* raw_geom = nullptr;
      ASSERT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                                 wkt.c_str(), &srs, &raw_geom));
      feat->SetGeometryDirectly(raw_geom);
    }
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }
}

class GdalalgVectorCheckCoverageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorCheckCoverageTest, BasicProperties) {
  GDALVectorCheckCoverageAlgorithmStandalone alg;
  EXPECT_EQ("check-coverage", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_vector_check_coverage.html", alg.GetHelpURL());
}

TEST_F(GdalalgVectorCheckCoverageTest,
       ValidCoverageWithAndWithoutIncludeValid) {
  const std::string in_gpkg = std::string(kTestDir) + "/valid.gpkg";
  ASSERT_NO_FATAL_FAILURE(
      CreateGpkgFromWkts(in_gpkg, "polys", wkbPolygon,
                         {"POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))",
                          "POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0))"}));

  // 1. Default skips empty outputs -> 0 features in "invalid_edge".
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "check-coverage"}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* layer =
        pipeline.GetOutputDataset().GetDatasetRef()->GetLayerByName(
            "invalid_edge");
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(wkbMultiLineString, layer->GetGeomType());
    EXPECT_EQ(0, layer->GetFeatureCount(1));
    EXPECT_TRUE(pipeline.Finalize());
  }

  // 2. --include-valid keeps empty geometries for valid polygons.
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "check-coverage", "--include-valid"}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* layer =
        pipeline.GetOutputDataset().GetDatasetRef()->GetLayerByName(
            "invalid_edge");
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(2, layer->GetFeatureCount(1));
    EXPECT_TRUE(pipeline.Finalize());
  }
}

TEST_F(GdalalgVectorCheckCoverageTest, OverlapAndMaximumGapWidth) {
  // 1. Overlapping polygons produce invalid edges and preserve SRS.
  const std::string overlap_gpkg = std::string(kTestDir) + "/overlap.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/overlap_out.gpkg";
  ASSERT_NO_FATAL_FAILURE(
      CreateGpkgFromWkts(overlap_gpkg, "polys", wkbPolygon,
                         {"POLYGON ((0 0, 1.5 0, 1.5 1, 0 1, 0 0))",
                          "POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0))"}));

  {
    GDALVectorCheckCoverageAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--geometry-field", "geom", "--overwrite", overlap_gpkg, out_gpkg}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* layer = out_ds->GetLayerByName("invalid_edge");
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(2, layer->GetFeatureCount(1));
    OGRSpatialReference expected;
    ASSERT_EQ(OGRERR_NONE, expected.importFromEPSG(4326));
    expected.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ASSERT_NE(nullptr, layer->GetSpatialRef());
    EXPECT_TRUE(layer->GetSpatialRef()->IsSame(&expected));
  }

  // 2. Polygons with a 0.2 gap: flagged only when maximum-gap-width >= 0.2.
  const std::string gap_gpkg = std::string(kTestDir) + "/gap.gpkg";
  ASSERT_NO_FATAL_FAILURE(
      CreateGpkgFromWkts(gap_gpkg, "polys", wkbPolygon,
                         {"POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))",
                          "POLYGON ((1.2 0, 2.2 0, 2.2 1, 1.2 1, 1.2 0))"}));

  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", gap_gpkg, "!", "check-coverage", "--maximum-gap-width",
         "0.5"}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* layer =
        pipeline.GetOutputDataset().GetDatasetRef()->GetLayerByName(
            "invalid_edge");
    ASSERT_NE(nullptr, layer);
    EXPECT_GT(layer->GetFeatureCount(1), 0);
    EXPECT_TRUE(pipeline.Finalize());
  }
}

TEST_F(GdalalgVectorCheckCoverageTest, MultiLayerNonSpatialAndErrorPaths) {
  const std::string multi_gpkg = std::string(kTestDir) + "/multi.gpkg";
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_NE(nullptr, drv);
    std::unique_ptr<GDALDataset> ds(
        drv->Create(multi_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    OGRLayer* poly_layer =
        ds->CreateLayer("polys", nullptr, wkbPolygon, nullptr);
    ASSERT_NE(nullptr, poly_layer);
    {
      auto feat = std::make_unique<OGRFeature>(poly_layer->GetLayerDefn());
      OGRGeometry* raw_geom = nullptr;
      ASSERT_EQ(OGRERR_NONE,
                OGRGeometryFactory::createFromWkt(
                    "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))", nullptr, &raw_geom));
      feat->SetGeometryDirectly(raw_geom);
      ASSERT_EQ(OGRERR_NONE, poly_layer->CreateFeature(feat.get()));
    }

    OGRLayer* tbl_layer =
        ds->CreateLayer("attr_only", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, tbl_layer);
  }

  // 1. Multi-layer without --input-layer skips non-spatial layer and prefixes
  //    output layer name with "invalid_edge_".
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", multi_gpkg, "!", "check-coverage"}));
    ASSERT_TRUE(pipeline.Run());
    GDALDataset* out_ds = pipeline.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(1, out_ds->GetLayerCount());
    EXPECT_NE(nullptr, out_ds->GetLayerByName("invalid_edge_polys"));
    EXPECT_TRUE(pipeline.Finalize());
  }

  // 2. Explicit --input-layer pointing to non-spatial layer fails.
  {
    GDALVectorCheckCoverageAlgorithmStandalone alg;
    const std::string err_gpkg = std::string(kTestDir) + "/err_attr.gpkg";
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--input-layer", "attr_only", multi_gpkg, err_gpkg}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("has no geometry field"));
    CPLErrorReset();
  }

  // 3. Non-existent --geometry-field fails.
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", "--input-layer", "polys", multi_gpkg, "!", "check-coverage",
         "--geometry-field", "missing_geom"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(pipeline.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("does not exist"));
    CPLErrorReset();
  }

  // 4. Non-polygonal input fails PolygonsOnly check.
  const std::string pt_gpkg = std::string(kTestDir) + "/points.gpkg";
  ASSERT_NO_FATAL_FAILURE(
      CreateGpkgFromWkts(pt_gpkg, "pts", wkbPoint, {"POINT (0 0)"}));
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", pt_gpkg, "!", "check-coverage"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(pipeline.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("polygonal geometries"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
