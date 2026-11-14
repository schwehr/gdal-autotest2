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
// Tests for GDALVectorSetGeomTypeAlgorithm
// (apps/gdalalg_vector_set_geom_type.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_set_geom_type.cpp
//   https://gdal.org/programs/gdal_vector_set_geom_type.html

#include "third_party/gdal/apps/gdalalg_vector_set_geom_type.h"

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
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_set_geom_type_test";

void CreateSampleGpkg(const std::string& path, OGRwkbGeometryType geom_type,
                      const std::vector<std::string>& wkts) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("layer", nullptr, geom_type, nullptr);
  ASSERT_NE(nullptr, layer);
  for (const std::string& wkt : wkts) {
    OGRGeometry* raw_geom = nullptr;
    ASSERT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                               wkt.c_str(), nullptr, &raw_geom));
    std::unique_ptr<OGRGeometry> geom(raw_geom);
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetGeometryDirectly(geom.release());
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }
}

class GdalalgVectorSetGeomTypeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorSetGeomTypeTest, BasicPropertiesAndIncompatibleOptions) {
  GDALVectorSetGeomTypeAlgorithmStandalone alg;
  EXPECT_EQ("set-geom-type", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_vector_set_geom_type.html", alg.GetHelpURL());
  EXPECT_THAT(
      GDALVectorSetGeomTypeAlgorithm::GetAliasesStatic(),
      ElementsAre(GDALAlgorithmRegistry::HIDDEN_ALIAS_SEPARATOR, "set-type"));

  const std::string in_gpkg = std::string(kTestDir) + "/in_err.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out_err.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateSampleGpkg(in_gpkg, wkbPoint, {"POINT (1 2)"}));

  ASSERT_TRUE(
      alg.ParseCommandLineArguments({"--geometry-type", "MultiPoint", "--multi",
                                     "--overwrite", in_gpkg, out_gpkg}));
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(alg.Run());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot be used with"));
  CPLErrorReset();
}

TEST_F(GdalalgVectorSetGeomTypeTest, LayerOnlyFeatureOnlyAndExplicitType) {
  const std::string in_gpkg = std::string(kTestDir) + "/in_modes.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateSampleGpkg(in_gpkg, wkbPoint, {"POINT (1 2)"}));

  // 1. --layer-only changes layer defn but preserves feature geometry type.
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "set-geom-type", "--geometry-type", "MultiPoint",
         "--layer-only"}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* layer = pipeline.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(wkbMultiPoint, layer->GetGeomType());
    EXPECT_EQ(1, layer->GetFeatureCount(1));
    EXPECT_TRUE(layer->TestCapability(OLCFastFeatureCount));
    EXPECT_FALSE(layer->TestCapability(OLCRandomWrite));
    auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(wkbPoint, feat->GetGeometryRef()->getGeometryType());
    EXPECT_TRUE(pipeline.Finalize());
  }

  // 2. --feature-only keeps layer defn wkbPoint, converts feature to
  //    MultiPoint.
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "set-geom-type", "--geometry-type", "MultiPoint",
         "--feature-only"}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* layer = pipeline.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(wkbPoint, layer->GetGeomType());
    auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(wkbMultiPoint, feat->GetGeometryRef()->getGeometryType());
    EXPECT_TRUE(pipeline.Finalize());
  }
}

TEST_F(GdalalgVectorSetGeomTypeTest, MultiSingleLinearCurveAndDim) {
  const std::string pt_gpkg = std::string(kTestDir) + "/pt.gpkg";
  const std::string mpt_gpkg = std::string(kTestDir) + "/mpt.gpkg";
  const std::string line_gpkg = std::string(kTestDir) + "/line.gpkg";
  const std::string curve_gpkg = std::string(kTestDir) + "/curve.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateSampleGpkg(pt_gpkg, wkbPoint, {"POINT (1 2)"}));
  ASSERT_NO_FATAL_FAILURE(
      CreateSampleGpkg(mpt_gpkg, wkbMultiPoint, {"MULTIPOINT ((1 2))"}));
  ASSERT_NO_FATAL_FAILURE(
      CreateSampleGpkg(line_gpkg, wkbLineString, {"LINESTRING (0 0, 1 1)"}));
  ASSERT_NO_FATAL_FAILURE(CreateSampleGpkg(curve_gpkg, wkbCircularString,
                                           {"CIRCULARSTRING (0 0, 1 1, 2 0)"}));

  // 1. --multi + --dim XYZ.
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", pt_gpkg, "!", "set-geom-type", "--multi", "--dim", "XYZ"}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* layer = pipeline.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(wkbMultiPoint25D, layer->GetGeomType());
    auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(wkbMultiPoint25D, feat->GetGeometryRef()->getGeometryType());
    EXPECT_TRUE(pipeline.Finalize());
  }

  // 2. --single + --dim XYM, then --dim XYZM, then --dim XY.
  for (const char* dim : {"XYM", "XYZM", "XY"}) {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", mpt_gpkg, "!", "set-geom-type", "--single", "--dim", dim}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* layer = pipeline.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(wkbPoint, wkbFlatten(feat->GetGeometryRef()->getGeometryType()));
    EXPECT_TRUE(pipeline.Finalize());
  }

  // 3. --curve on LineString -> CompoundCurve.
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", line_gpkg, "!", "set-geom-type", "--curve"}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* layer = pipeline.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(wkbCompoundCurve, layer->GetGeomType());
    EXPECT_TRUE(pipeline.Finalize());
  }

  // 4. --linear on CircularString -> LineString.
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", curve_gpkg, "!", "set-geom-type", "--linear"}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* layer = pipeline.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(wkbLineString, layer->GetGeomType());
    EXPECT_TRUE(pipeline.Finalize());
  }
}

TEST_F(GdalalgVectorSetGeomTypeTest,
       SkipIncompatibleAndTriangleToMultiPolygon) {
  // 1. Mixed Point + LineString GPKG with --geometry-type Point --skip skips
  //    LineString.
  const std::string mixed_gpkg = std::string(kTestDir) + "/mixed.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateSampleGpkg(
      mixed_gpkg, wkbUnknown, {"POINT (1 2)", "LINESTRING (0 0, 1 1)"}));

  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", mixed_gpkg, "!", "set-geom-type", "--geometry-type", "Point",
         "--skip"}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* layer = pipeline.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_FALSE(layer->TestCapability(OLCFastFeatureCount));
    EXPECT_EQ(1, layer->GetFeatureCount(1));
    EXPECT_TRUE(pipeline.Finalize());
  }

  // 2. Triangle layer with --multi via MEM converts to MultiPolygon, and
  //    --active-geometry restricts conversion to the named geometry field.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  std::unique_ptr<GDALDataset> mem_ds(
      mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, mem_ds);
  OGRLayer* tri_layer = mem_ds->CreateLayer("tri", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, tri_layer);
  OGRGeomFieldDefn g1("geom1", wkbTriangle);
  OGRGeomFieldDefn g2("geom2", wkbPoint);
  ASSERT_EQ(OGRERR_NONE, tri_layer->CreateGeomField(&g1));
  ASSERT_EQ(OGRERR_NONE, tri_layer->CreateGeomField(&g2));

  {
    auto feat = std::make_unique<OGRFeature>(tri_layer->GetLayerDefn());
    OGRGeometry* raw_tri = nullptr;
    ASSERT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt(
                  "TRIANGLE ((0 0, 1 0, 0 1, 0 0))", nullptr, &raw_tri));
    feat->SetGeomFieldDirectly(0, raw_tri);
    feat->SetGeomFieldDirectly(1, new OGRPoint(5.0, 6.0));
    ASSERT_EQ(OGRERR_NONE, tri_layer->CreateFeature(feat.get()));
  }

  GDALVectorSetGeomTypeAlgorithmStandalone alg;
  alg.SetInputDataset(mem_ds.get());
  ASSERT_TRUE(alg.GetArg("multi")->Set(true));
  ASSERT_TRUE(alg.GetArg("active-geometry")->Set("geom1"));
  ASSERT_TRUE(alg.GetArg("active-layer")->Set("tri"));
  ASSERT_TRUE(alg.GetArg("output-format")->Set("stream"));
  ASSERT_TRUE(alg.GetArg("output")->SetDatasetName("streamed"));
  ASSERT_TRUE(alg.Run());
  OGRLayer* out_layer = alg.GetOutputDataset().GetDatasetRef()->GetLayer(0);
  ASSERT_NE(nullptr, out_layer);
  EXPECT_EQ(wkbMultiPolygon,
            out_layer->GetLayerDefn()->GetGeomFieldDefn(0)->GetType());
  EXPECT_EQ(wkbPoint,
            out_layer->GetLayerDefn()->GetGeomFieldDefn(1)->GetType());
  auto out_feat = std::unique_ptr<OGRFeature>(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, out_feat);
  EXPECT_EQ(wkbMultiPolygon, out_feat->GetGeomFieldRef(0)->getGeometryType());
  EXPECT_EQ(wkbPoint, out_feat->GetGeomFieldRef(1)->getGeometryType());
  EXPECT_TRUE(alg.Finalize());
}

}  // namespace
}  // namespace autotest2
