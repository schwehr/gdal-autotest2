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
// Tests for GDALVectorSimplifyAlgorithm (apps/gdalalg_vector_simplify.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_simplify.cpp

#include "third_party/gdal/apps/gdalalg_vector_simplify.h"

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

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_simplify_test";

class GdalalgVectorSimplifyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorSimplifyTest, BasicPropertiesAndValidation) {
  GDALVectorSimplifyAlgorithm alg(false);
  EXPECT_EQ("simplify", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_vector_simplify.html", alg.GetHelpURL());

  GDALVectorSimplifyAlgorithmStandalone standalone;
  EXPECT_EQ("simplify", standalone.GetName());

  {
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({}));
  }
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"-1.0"}));
  }
}

TEST_F(GdalalgVectorSimplifyTest, SimplifyLineAndGeomFieldAndNullGeom) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto src_ds = std::unique_ptr<GDALDataset>(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, src_ds);
  OGRLayer* lyr = src_ds->CreateLayer("lines", &srs, wkbLineString, nullptr);
  ASSERT_NE(nullptr, lyr);
  OGRGeomFieldDefn gf2("geom2", wkbLineString);
  ASSERT_EQ(OGRERR_NONE, lyr->CreateGeomField(&gf2));

  {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    OGRGeometry* g0 = nullptr;
    OGRGeometry* g1 = nullptr;
    ASSERT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                               "LINESTRING (0 0, 5 0.1, 10 0)", nullptr, &g0));
    ASSERT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                               "LINESTRING (0 0, 5 0.1, 10 0)", nullptr, &g1));
    feat->SetGeomFieldDirectly(0, g0);
    feat->SetGeomFieldDirectly(1, g1);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }
  {
    auto null_feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(null_feat.get()));
  }

  // 1. Simplify all geometry fields.
  {
    GDALVectorSimplifyAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"1.0"}));
    ASSERT_TRUE(alg.Run());

    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* out_lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, out_lyr);

    std::unique_ptr<OGRFeature> f0(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, f0);
    ASSERT_NE(nullptr, f0->GetGeomFieldRef(0));
    EXPECT_EQ(2, f0->GetGeomFieldRef(0)->toLineString()->getNumPoints());
    ASSERT_NE(nullptr, f0->GetGeomFieldRef(0)->getSpatialReference());
    EXPECT_TRUE(f0->GetGeomFieldRef(0)->getSpatialReference()->IsSame(&srs));
    EXPECT_EQ(2, f0->GetGeomFieldRef(1)->toLineString()->getNumPoints());

    std::unique_ptr<OGRFeature> f1(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, f1);
    EXPECT_EQ(nullptr, f1->GetGeomFieldRef(0));
  }

  // 2. Simplify only geom2 using --active-geometry.
  lyr->ResetReading();
  {
    GDALVectorSimplifyAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--active-geometry", "geom2", "1.0"}));
    ASSERT_TRUE(alg.Run());

    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    std::unique_ptr<OGRFeature> f0(out_ds->GetLayer(0)->GetNextFeature());
    ASSERT_NE(nullptr, f0);
    EXPECT_EQ(3, f0->GetGeomFieldRef(0)->toLineString()->getNumPoints());
    EXPECT_EQ(2, f0->GetGeomFieldRef(1)->toLineString()->getNumPoints());
  }
}

TEST_F(GdalalgVectorSimplifyTest, PipelineMode) {
  const std::string in_geojson = std::string(kTestDir) + "/in.geojson";
  const std::string out_geojson = std::string(kTestDir) + "/out.geojson";
  VsiMemTempWrapper in_file(in_geojson, R"json({
    "type": "FeatureCollection",
    "features": [
      {
        "type": "Feature",
        "properties": {},
        "geometry": {
          "type": "LineString",
          "coordinates": [[0, 0], [5, 0.1], [10, 0]]
        }
      }
    ]
  })json");

  GDALVectorPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_geojson, "!", "simplify", "1.0", "!", "write", out_geojson,
       "--overwrite"}));
  ASSERT_TRUE(pipeline.Run());
  ASSERT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(out_geojson.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  std::unique_ptr<OGRFeature> f(ds->GetLayer(0)->GetNextFeature());
  ASSERT_NE(nullptr, f);
  EXPECT_EQ(2, f->GetGeometryRef()->toLineString()->getNumPoints());
}

}  // namespace
}  // namespace autotest2
