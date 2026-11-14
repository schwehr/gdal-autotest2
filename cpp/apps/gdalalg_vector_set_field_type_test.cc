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
// Tests for "set-field-type" step of "vector pipeline"
// (apps/gdalalg_vector_set_field_type.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_set_field_type.cpp

#include "third_party/gdal/apps/gdalalg_vector_set_field_type.h"

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_set_field_type_test";
constexpr char kTmpDir[] = "/vsimem/gdalalg_vector_set_field_type_tmp";

class GdalalgVectorSetFieldTypeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    ASSERT_EQ(0, VSIMkdir(kTmpDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTmpDir);
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
    VSIRmdirRecursive(kTmpDir);
    CPLErrorReset();
  }
};

std::unique_ptr<GDALDataset> CreateTestMemDataset() {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  OGRLayer* lyr1 = ds->CreateLayer("layer1", nullptr, wkbPoint, nullptr);
  if (!lyr1) return nullptr;
  OGRFieldDefn fd_str("str_num", OFTString);
  OGRFieldDefn fd_int("flag", OFTInteger);
  OGRFieldDefn fd_date("dt", OFTDate);
  EXPECT_EQ(OGRERR_NONE, lyr1->CreateField(&fd_str));
  EXPECT_EQ(OGRERR_NONE, lyr1->CreateField(&fd_int));
  EXPECT_EQ(OGRERR_NONE, lyr1->CreateField(&fd_date));

  auto feat1 = std::make_unique<OGRFeature>(lyr1->GetLayerDefn());
  feat1->SetFID(1);
  feat1->SetField("str_num", "42");
  feat1->SetField("flag", 1);
  feat1->SetField("dt", 2025, 3, 14, 0, 0, 0.0f, 0);
  auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
  feat1->SetGeometryDirectly(pt.release());
  EXPECT_EQ(OGRERR_NONE, lyr1->CreateFeature(feat1.get()));

  OGRLayer* lyr2 = ds->CreateLayer("layer2", nullptr, wkbPoint, nullptr);
  if (!lyr2) return nullptr;
  EXPECT_EQ(OGRERR_NONE, lyr2->CreateField(&fd_int));
  auto feat2 = std::make_unique<OGRFeature>(lyr2->GetLayerDefn());
  feat2->SetField("flag", 5);
  EXPECT_EQ(OGRERR_NONE, lyr2->CreateFeature(feat2.get()));

  return ds;
}

TEST_F(GdalalgVectorSetFieldTypeTest, SchemaOpenOptionLayer) {
  {
    GDALVectorSetFieldTypeAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field-name", "str_num", "--dst-field-type", "Integer64"}));
    CPLJSONObject obj = alg.Get_OGR_SCHEMA_OpenOption_Layer();
    EXPECT_EQ("*", obj.GetString("name"));
    EXPECT_EQ("Patch", obj.GetString("schemaType"));
    CPLJSONArray fields = obj.GetArray("fields");
    ASSERT_EQ(1, fields.Size());
    EXPECT_EQ("str_num", fields[0].GetString("name"));
    EXPECT_EQ("Integer64", fields[0].GetString("type"));
  }

  {
    GDALVectorSetFieldTypeAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--active-layer", "layer1",
                                               "--src-field-type", "Integer",
                                               "--field-type", "Boolean"}));
    CPLJSONObject obj = alg.Get_OGR_SCHEMA_OpenOption_Layer();
    EXPECT_EQ("layer1", obj.GetString("name"));
    CPLJSONArray fields = obj.GetArray("fields");
    ASSERT_EQ(1, fields.Size());
    EXPECT_EQ("Integer", fields[0].GetString("srcType"));
    EXPECT_EQ("Boolean", fields[0].GetString("subType"));
  }
}

TEST_F(GdalalgVectorSetFieldTypeTest, ValidationErrors) {
  auto src_ds = CreateTestMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  // 1. Missing required target type.
  {
    GDALVectorSetFieldTypeAlgorithm alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments({"--field-name", "str_num"}));
  }

  // 2. Mutual exclusion between --field-name and --src-field-type.
  {
    GDALVectorSetFieldTypeAlgorithm alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--field-name", "str_num", "--src-field-type", "String",
         "--dst-field-type", "Integer"}));
  }

  // 3. Non-existent --active-layer rejected at validation / Run.
  {
    GDALVectorSetFieldTypeAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--active-layer", "no_such_layer", "--src-field-type", "String",
         "--dst-field-type", "Integer"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot find layer"));
  }

  // 4. Non-existent --field-name rejected.
  {
    GDALVectorSetFieldTypeAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--active-layer", "layer1", "--field-name", "missing_field",
         "--dst-field-type", "Integer"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot find field"));
  }
}

TEST_F(GdalalgVectorSetFieldTypeTest,
       ConvertByNameBySrcTypePassThroughAndFail) {
  auto src_ds = CreateTestMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  // 1. Convert str_num String -> Integer on layer1, leaving layer2 intact.
  // Note: --active-layer is omitted here because upstream
  // GDALVectorSetFieldTypeAlgorithmLayer checks GetDescription() before
  // AddLayer sets it (apps/gdalalg_vector_set_field_type.cpp:134).
  {
    GDALVectorSetFieldTypeAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field-name", "str_num", "--dst-field-type", "Integer"}));
    ASSERT_TRUE(alg.Run());

    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    OGRLayer* out_lyr1 = out_ds->GetLayer(0);
    ASSERT_THAT(out_lyr1, NotNull());
    EXPECT_EQ(OFTInteger, out_lyr1->GetLayerDefn()->GetFieldDefn(0)->GetType());
    EXPECT_EQ(0, out_lyr1->TestCapability(OLCRandomWrite));
    EXPECT_EQ(src_ds->GetLayer(0)->TestCapability(OLCStringsAsUTF8),
              out_lyr1->TestCapability(OLCStringsAsUTF8));

    out_lyr1->ResetReading();
    auto feat1 = std::unique_ptr<OGRFeature>(out_lyr1->GetNextFeature());
    ASSERT_THAT(feat1, NotNull());
    EXPECT_EQ(1, feat1->GetFID());
    EXPECT_EQ(42, feat1->GetFieldAsInteger("str_num"));

    // layer2 is unchanged (no str_num field).
    OGRLayer* out_lyr2 = out_ds->GetLayer(1);
    ASSERT_THAT(out_lyr2, NotNull());
    out_lyr2->ResetReading();
    auto feat2 = std::unique_ptr<OGRFeature>(out_lyr2->GetNextFeature());
    ASSERT_THAT(feat2, NotNull());
    EXPECT_EQ(5, feat2->GetFieldAsInteger("flag"));
  }

  // 2. Pass-through when target type already matches (Integer -> Boolean).
  {
    src_ds->GetLayer(0)->ResetReading();
    GDALVectorSetFieldTypeAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field-name", "flag", "--dst-field-type", "Boolean"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    OGRLayer* out_lyr = out_ds->GetLayer(0);
    EXPECT_EQ(OFSTBoolean,
              out_lyr->GetLayerDefn()->GetFieldDefn(1)->GetSubType());
    out_lyr->ResetReading();
    auto feat = std::unique_ptr<OGRFeature>(out_lyr->GetNextFeature());
    ASSERT_THAT(feat, NotNull());
    EXPECT_EQ(1, feat->GetFieldAsInteger("flag"));
  }

  // 3. Convert all OFTInteger fields across all layers via --src-field-type.
  {
    src_ds->GetLayer(0)->ResetReading();
    src_ds->GetLayer(1)->ResetReading();
    GDALVectorSetFieldTypeAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--src-field-type", "Integer", "--dst-field-type", "Real"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(OFTReal,
              out_ds->GetLayer(0)->GetLayerDefn()->GetFieldDefn(1)->GetType());
    EXPECT_EQ(OFTReal,
              out_ds->GetLayer(1)->GetLayerDefn()->GetFieldDefn(0)->GetType());
  }

  // 4. Incompatible conversion (Date -> Integer) emits warning and omits
  // feature.
  {
    src_ds->GetLayer(0)->ResetReading();
    GDALVectorSetFieldTypeAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field-name", "dt", "--dst-field-type", "Integer"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    WithQuietHandler quiet;
    CPLErrorReset();
    out_ds->GetLayer(0)->ResetReading();
    auto feat =
        std::unique_ptr<OGRFeature>(out_ds->GetLayer(0)->GetNextFeature());
    EXPECT_EQ(nullptr, feat);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot convert field 'dt'"));
  }
}

TEST_F(GdalalgVectorSetFieldTypeTest, StandaloneAndPipeline) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  const std::string pipe_gpkg = std::string(kTestDir) + "/pipe.gpkg";

  {
    GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_THAT(gpkg_drv, NotNull());
    std::unique_ptr<GDALDataset> ds(
        gpkg_drv->Create(in_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_THAT(ds, NotNull());
    OGRLayer* lyr = ds->CreateLayer("pts", nullptr, wkbPoint, nullptr);
    ASSERT_THAT(lyr, NotNull());
    OGRFieldDefn fd("val", OFTString);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fd));
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("val", "100");
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    feat->SetGeometryDirectly(pt.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  // Standalone.
  {
    GDALVectorSetFieldTypeAlgorithmStandalone alg;
    EXPECT_EQ("set-field-type", alg.GetName());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--field-name", "val", "--dst-field-type", "Integer64", "--overwrite",
         in_gpkg, out_gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // Pipeline.
  GDALVectorPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_gpkg, "!", "set-field-type", "--field-name", "val",
       "--dst-field-type", "Integer", "!", "write", pipe_gpkg, "--overwrite"}));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(pipe_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  OGRLayer* lyr = ds->GetLayer(0);
  ASSERT_THAT(lyr, NotNull());
  EXPECT_EQ(OFTInteger, lyr->GetLayerDefn()->GetFieldDefn(0)->GetType());
  auto feat = std::unique_ptr<OGRFeature>(lyr->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  EXPECT_EQ(100, feat->GetFieldAsInteger("val"));
}

}  // namespace
}  // namespace autotest2
