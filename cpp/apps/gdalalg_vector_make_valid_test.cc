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
// Tests for GDALVectorMakeValidAlgorithm (apps/gdalalg_vector_make_valid.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_make_valid.cpp

#include "third_party/gdal/apps/gdalalg_vector_make_valid.h"

#include <memory>
#include <string>

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
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_make_valid_test";

class GdalalgVectorMakeValidTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void AddFeatureWkt(OGRLayer* lyr, const char* wkt) {
  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  if (wkt != nullptr) {
    OGRGeometry* geom = nullptr;
    ASSERT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt(wkt, nullptr, &geom));
    feat->SetGeometryDirectly(geom);
  }
  ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
}

TEST_F(GdalalgVectorMakeValidTest, BasicPropertiesAndInvalidMethod) {
  GDALVectorMakeValidAlgorithm alg(false);
  EXPECT_EQ("make-valid", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_vector_make_valid.html", alg.GetHelpURL());

  GDALVectorMakeValidAlgorithmStandalone standalone;
  EXPECT_EQ("make-valid", standalone.GetName());

  WithQuietHandler quiet;
  EXPECT_FALSE(alg.ParseCommandLineArguments({"--method", "invalid"}));
}

TEST_F(GdalalgVectorMakeValidTest, LineworkAndStructureMethodsAndKeepLowerDim) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto src_ds = std::unique_ptr<GDALDataset>(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, src_ds);
  OGRLayer* lyr = src_ds->CreateLayer("geoms", &srs, wkbUnknown, nullptr);
  ASSERT_NE(nullptr, lyr);

  // 0: invalid bowtie polygon with spike
  ASSERT_NO_FATAL_FAILURE(AddFeatureWkt(
      lyr, "POLYGON ((0 0, 10 10, 10 0, 0 10, 15 15, 0 10, 0 0))"));
  // 1: already valid polygon
  ASSERT_NO_FATAL_FAILURE(
      AddFeatureWkt(lyr, "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))"));
  // 2: null geometry
  ASSERT_NO_FATAL_FAILURE(AddFeatureWkt(lyr, nullptr));
  // 3: 3D invalid polygon (coordinateDimension == 3 -> untouched)
  ASSERT_NO_FATAL_FAILURE(AddFeatureWkt(
      lyr, "POLYGON Z ((0 0 0, 10 10 0, 10 0 0, 0 10 0, 0 0 0))"));
  // 4: GeometryCollection with invalid polygon + point
  ASSERT_NO_FATAL_FAILURE(AddFeatureWkt(
      lyr,
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 4 4, 4 0, 0 4, 0 0)), POINT (9 9))"));

  // 1. Default method = linework, keep-lower-dim = false.
  {
    GDALVectorMakeValidAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* out_lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, out_lyr);

    std::unique_ptr<OGRFeature> f0(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, f0);
    ASSERT_NE(nullptr, f0->GetGeometryRef());
    EXPECT_TRUE(f0->GetGeometryRef()->IsValid());
    ASSERT_NE(nullptr, f0->GetGeometryRef()->getSpatialReference());
    EXPECT_TRUE(f0->GetGeometryRef()->getSpatialReference()->IsSame(&srs));

    std::unique_ptr<OGRFeature> f1(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, f1);
    EXPECT_TRUE(f1->GetGeometryRef()->IsValid());

    std::unique_ptr<OGRFeature> f2(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, f2);
    EXPECT_EQ(nullptr, f2->GetGeometryRef());

    std::unique_ptr<OGRFeature> f3(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, f3);
    EXPECT_EQ(3, f3->GetGeometryRef()->getCoordinateDimension());

    std::unique_ptr<OGRFeature> f4(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, f4);
    EXPECT_TRUE(f4->GetGeometryRef()->IsValid());
  }

  // 2. Method = structure with --keep-lower-dim.
  {
    src_ds->GetLayer(0)->ResetReading();
    GDALVectorMakeValidAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--method", "structure", "--keep-lower-dim"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* out_lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, out_lyr);
    out_lyr->ResetReading();
    std::unique_ptr<OGRFeature> f0(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, f0);
    ASSERT_NE(nullptr, f0->GetGeometryRef());
    EXPECT_TRUE(f0->GetGeometryRef()->IsValid());
  }
}

TEST_F(GdalalgVectorMakeValidTest, SelectedGeomFieldAndPipeline) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  auto src_ds = std::unique_ptr<GDALDataset>(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, src_ds);
  OGRLayer* lyr = src_ds->CreateLayer("multi", nullptr, wkbUnknown, nullptr);
  ASSERT_NE(nullptr, lyr);
  OGRGeomFieldDefn gf2("geom2", wkbUnknown);
  ASSERT_EQ(OGRERR_NONE, lyr->CreateGeomField(&gf2));

  {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    OGRGeometry* g0 = nullptr;
    OGRGeometry* g1 = nullptr;
    ASSERT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt(
                  "POLYGON ((0 0, 4 4, 4 0, 0 4, 0 0))", nullptr, &g0));
    ASSERT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt(
                  "POLYGON ((0 0, 4 4, 4 0, 0 4, 0 0))", nullptr, &g1));
    feat->SetGeomFieldDirectly(0, g0);
    feat->SetGeomFieldDirectly(1, g1);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  // Only geom2 is repaired when --active-geometry geom2 is given.
  {
    GDALVectorMakeValidAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--active-geometry", "geom2"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    std::unique_ptr<OGRFeature> f(out_ds->GetLayer(0)->GetNextFeature());
    ASSERT_NE(nullptr, f);
    WithQuietHandler quiet;
    EXPECT_FALSE(f->GetGeomFieldRef(0)->IsValid());
    EXPECT_TRUE(f->GetGeomFieldRef(1)->IsValid());
  }

  // Pipeline mode with GeoJSON input -> GeoPackage output.
  const std::string in_geojson = std::string(kTestDir) + "/bowtie.geojson";
  const std::string out_gpkg = std::string(kTestDir) + "/valid.gpkg";
  VsiMemTempWrapper in_file(in_geojson, R"json({
    "type": "FeatureCollection",
    "features": [
      {
        "type": "Feature",
        "properties": { "id": 1 },
        "geometry": {
          "type": "Polygon",
          "coordinates": [[[0, 0], [4, 4], [4, 0], [0, 4], [0, 0]]]
        }
      }
    ]
  })json");
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_geojson, "!", "make-valid", "--method", "structure", "!",
         "write", out_gpkg, "--overwrite"}));
    ASSERT_TRUE(pipeline.Run());
    ASSERT_TRUE(pipeline.Finalize());
  }
}

}  // namespace
}  // namespace autotest2
