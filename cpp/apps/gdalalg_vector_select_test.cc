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
// Tests for GDALVectorSelectAlgorithm (apps/gdalalg_vector_select.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_select.cpp

#include "third_party/gdal/apps/gdalalg_vector_select.h"

#include <memory>
#include <string>

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

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_select_test";

class GdalalgVectorSelectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

std::unique_ptr<GDALDataset> CreateTestDataset() {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  // Layer 1: default unnamed geometry field + 3 attribute fields.
  OGRLayer* lyr1 = ds->CreateLayer("layer1", nullptr, wkbPoint, nullptr);
  if (!lyr1) return nullptr;
  OGRFieldDefn f_id("id", OFTInteger);
  OGRFieldDefn f_name("Name", OFTString);
  OGRFieldDefn f_extra("extra", OFTReal);
  if (lyr1->CreateField(&f_id) != OGRERR_NONE ||
      lyr1->CreateField(&f_name) != OGRERR_NONE ||
      lyr1->CreateField(&f_extra) != OGRERR_NONE) {
    return nullptr;
  }

  for (int i = 0; i < 2; ++i) {
    auto feat = std::make_unique<OGRFeature>(lyr1->GetLayerDefn());
    feat->SetFID(i);
    feat->SetField("id", i + 1);
    feat->SetField("Name", i == 0 ? "alpha" : "beta");
    feat->SetField("extra", 1.5 + i);
    if (i == 0) {
      feat->SetStyleString("PEN(c:#FF0000)");
    }
    auto pt = std::make_unique<OGRPoint>(1.0 + i, 2.0 + i);
    feat->SetGeometryDirectly(pt.release());
    if (lyr1->CreateFeature(feat.get()) != OGRERR_NONE) return nullptr;
  }

  // Layer 2: named geometry fields geom_a and geom_b + 1 attribute field.
  OGRLayer* lyr2 = ds->CreateLayer("multi_geom", nullptr, wkbNone, nullptr);
  if (!lyr2) return nullptr;
  OGRFieldDefn f_tag("tag", OFTString);
  OGRGeomFieldDefn gf_a("geom_a", wkbPoint);
  OGRGeomFieldDefn gf_b("geom_b", wkbPoint);
  if (lyr2->CreateField(&f_tag) != OGRERR_NONE ||
      lyr2->CreateGeomField(&gf_a) != OGRERR_NONE ||
      lyr2->CreateGeomField(&gf_b) != OGRERR_NONE) {
    return nullptr;
  }
  auto feat2 = std::make_unique<OGRFeature>(lyr2->GetLayerDefn());
  feat2->SetField("tag", "hello");
  auto pa = std::make_unique<OGRPoint>(10.0, 20.0);
  auto pb = std::make_unique<OGRPoint>(30.0, 40.0);
  feat2->SetGeomFieldDirectly(0, pa.release());
  feat2->SetGeomFieldDirectly(1, pb.release());
  if (lyr2->CreateFeature(feat2.get()) != OGRERR_NONE) return nullptr;

  return ds;
}

TEST_F(GdalalgVectorSelectTest, BasicPropertiesAndMissingRequiredArgs) {
  GDALVectorSelectAlgorithm alg(false);
  EXPECT_EQ("select", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_vector_select.html", alg.GetHelpURL());

  GDALVectorSelectAlgorithmStandalone standalone;
  EXPECT_EQ("select", standalone.GetName());

  WithQuietHandler quiet;
  EXPECT_FALSE(alg.ParseCommandLineArguments({}));
}

TEST_F(GdalalgVectorSelectTest, IncludeFieldsDefaultGeomAndLayerMethods) {
  auto src_ds = CreateTestDataset();
  ASSERT_NE(nullptr, src_ds);

  GDALVectorSelectAlgorithm alg(false);
  alg.GetInputDatasets().emplace_back(src_ds.get());
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"--active-layer", "layer1", "--fields", "name,_ogr_geometry_"}));
  ASSERT_TRUE(alg.Run());

  GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, out_ds);
  EXPECT_EQ(2, out_ds->GetLayerCount());

  OGRLayer* out_lyr = out_ds->GetLayerByName("layer1");
  ASSERT_NE(nullptr, out_lyr);
  EXPECT_EQ(1, out_lyr->GetLayerDefn()->GetFieldCount());
  EXPECT_STREQ("Name", out_lyr->GetLayerDefn()->GetFieldDefn(0)->GetNameRef());
  EXPECT_EQ(1, out_lyr->GetLayerDefn()->GetGeomFieldCount());

  // Capabilities, extent, feature count with and without filter, GetFeature.
  EXPECT_TRUE(out_lyr->TestCapability(OLCRandomRead));
  EXPECT_TRUE(out_lyr->TestCapability(OLCFastFeatureCount));
  EXPECT_FALSE(out_lyr->TestCapability(OLCSequentialWrite));

  OGREnvelope env;
  EXPECT_EQ(OGRERR_NONE, out_lyr->GetExtent(&env, TRUE));
  EXPECT_DOUBLE_EQ(1.0, env.MinX);
  EXPECT_DOUBLE_EQ(2.0, env.MaxX);
  EXPECT_EQ(2, out_lyr->GetFeatureCount(TRUE));

  std::unique_ptr<OGRFeature> f0(out_lyr->GetFeature(0));
  ASSERT_NE(nullptr, f0);
  EXPECT_STREQ("alpha", f0->GetFieldAsString("Name"));
  EXPECT_STREQ("PEN(c:#FF0000)", f0->GetStyleString());
  EXPECT_EQ(nullptr, out_lyr->GetFeature(9999));

  // Attribute filter exercises non-fast GetFeatureCount fallback.
  ASSERT_EQ(OGRERR_NONE, out_lyr->SetAttributeFilter("Name = 'beta'"));
  EXPECT_FALSE(out_lyr->TestCapability(OLCFastFeatureCount));
  EXPECT_EQ(1, out_lyr->GetFeatureCount(TRUE));
  ASSERT_EQ(OGRERR_NONE, out_lyr->SetAttributeFilter(nullptr));

  // Passthrough layer preserved untouched.
  OGRLayer* pass_lyr = out_ds->GetLayerByName("multi_geom");
  ASSERT_NE(nullptr, pass_lyr);
  EXPECT_EQ(2, pass_lyr->GetLayerDefn()->GetGeomFieldCount());
}

TEST_F(GdalalgVectorSelectTest, IncludeNamedGeomIgnoreMissingAndExclude) {
  auto src_ds = CreateTestDataset();
  ASSERT_NE(nullptr, src_ds);

  // 1. Include named geometry fields and ignore missing field.
  // (Include both geom_a and geom_b because upstream IncludeFields maps
  // destination geom index rather than source geom index.)
  {
    GDALVectorSelectAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--active-layer", "multi_geom", "--fields",
         "geom_a,geom_b,nonexistent_field", "--ignore-missing-fields"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    ASSERT_TRUE(alg.Run());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("will be ignored"));
    CPLErrorReset();

    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* out_lyr = out_ds->GetLayerByName("multi_geom");
    ASSERT_NE(nullptr, out_lyr);
    EXPECT_EQ(0, out_lyr->GetLayerDefn()->GetFieldCount());
    EXPECT_EQ(2, out_lyr->GetLayerDefn()->GetGeomFieldCount());
    EXPECT_STREQ("geom_a",
                 out_lyr->GetLayerDefn()->GetGeomFieldDefn(0)->GetNameRef());
    EXPECT_STREQ("geom_b",
                 out_lyr->GetLayerDefn()->GetGeomFieldDefn(1)->GetNameRef());
    std::unique_ptr<OGRFeature> f(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, f);
    ASSERT_NE(nullptr, f->GetGeomFieldRef(0));
    ASSERT_NE(nullptr, f->GetGeomFieldRef(1));
    EXPECT_DOUBLE_EQ(10.0, f->GetGeomFieldRef(0)->toPoint()->getX());
    EXPECT_DOUBLE_EQ(30.0, f->GetGeomFieldRef(1)->toPoint()->getX());
  }

  // 2. Missing field without --ignore-missing-fields fails.
  {
    GDALVectorSelectAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--active-layer", "layer1", "--fields", "does_not_exist"}));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }

  // 3. Exclude fields + default geometry (_ogr_geometry_) on layer1.
  src_ds->GetLayerByName("layer1")->ResetReading();
  {
    GDALVectorSelectAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--active-layer", "layer1", "--exclude",
                                       "--fields", "extra,_ogr_geometry_"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* out_lyr = out_ds->GetLayerByName("layer1");
    ASSERT_NE(nullptr, out_lyr);
    EXPECT_EQ(2, out_lyr->GetLayerDefn()->GetFieldCount());
    EXPECT_EQ(0, out_lyr->GetLayerDefn()->GetGeomFieldCount());
    std::unique_ptr<OGRFeature> f(out_lyr->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_EQ(1, f->GetFieldAsInteger("id"));
  }

  // 4. Exclude named geometry field geom_a on multi_geom.
  {
    GDALVectorSelectAlgorithm alg(false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--active-layer", "multi_geom", "--exclude", "--fields", "geom_a"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* out_lyr = out_ds->GetLayerByName("multi_geom");
    ASSERT_NE(nullptr, out_lyr);
    EXPECT_EQ(1, out_lyr->GetLayerDefn()->GetFieldCount());
    EXPECT_EQ(1, out_lyr->GetLayerDefn()->GetGeomFieldCount());
    EXPECT_STREQ("geom_b",
                 out_lyr->GetLayerDefn()->GetGeomFieldDefn(0)->GetNameRef());
  }
}

TEST_F(GdalalgVectorSelectTest, PipelineMode) {
  const std::string in_geojson = std::string(kTestDir) + "/in.geojson";
  const std::string out_geojson = std::string(kTestDir) + "/out.geojson";
  VsiMemTempWrapper in_file(in_geojson, R"json({
    "type": "FeatureCollection",
    "features": [
      {
        "type": "Feature",
        "properties": { "keep": 42, "drop": 99 },
        "geometry": { "type": "Point", "coordinates": [1.0, 2.0] }
      }
    ]
  })json");

  GDALVectorPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_geojson, "!", "select", "keep", "!", "write", out_geojson,
       "--overwrite"}));
  ASSERT_TRUE(pipeline.Run());
  ASSERT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(out_geojson.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);
  EXPECT_EQ(1, lyr->GetLayerDefn()->GetFieldCount());
  EXPECT_STREQ("keep", lyr->GetLayerDefn()->GetFieldDefn(0)->GetNameRef());
}

}  // namespace
}  // namespace autotest2
