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
// Tests for GDALVectorEditAlgorithm (apps/gdalalg_vector_edit.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_edit.cpp
//   https://gdal.org/programs/gdal_vector_edit.html

#include "third_party/gdal/apps/gdalalg_vector_edit.h"

#include <memory>
#include <string>
#include <vector>

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
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_edit_test";

void CreateMultiLayerGpkg(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  ASSERT_EQ(CE_None, ds->SetMetadataItem("OLD_DS_KEY", "old_val"));
  ASSERT_EQ(CE_None, ds->SetMetadataItem("KEEP_DS_KEY", "keep"));

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  for (const char* name : {"layer1", "layer2"}) {
    OGRLayer* layer = ds->CreateLayer(name, &srs, wkbPoint, nullptr);
    ASSERT_NE(nullptr, layer);
    ASSERT_EQ(CE_None, layer->SetMetadataItem("OLD_LYR_KEY", "old_lyr"));
    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetGeometryDirectly(new OGRPoint(1.0, 2.0));
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }
}

class GdalalgVectorEditTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorEditTest, BasicPropertiesAndValidation) {
  GDALVectorEditAlgorithmStandalone alg;
  EXPECT_EQ("edit", alg.GetName());
  EXPECT_EQ("Edit metadata of a vector dataset.", alg.GetDescription());
  EXPECT_EQ("/programs/gdal_vector_edit.html", alg.GetHelpURL());

  {
    WithQuietHandler quiet;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--metadata", "INVALID_NO_EQUALS"}));
  }
  {
    GDALVectorEditAlgorithmStandalone alg2;
    WithQuietHandler quiet;
    EXPECT_FALSE(alg2.ParseCommandLineArguments(
        {"--layer-metadata", "INVALID_NO_EQUALS"}));
  }
}

TEST_F(GdalalgVectorEditTest, EditMetadataCrsGeomTypeAndActiveLayer) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateMultiLayerGpkg(in_gpkg));

  {
    GDALVectorEditAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--active-layer", "layer1", "--crs", "EPSG:3857", "--geometry-type",
         "MultiPoint", "--metadata", "NEW_DS_KEY=new_val", "--unset-metadata",
         "OLD_DS_KEY", "--layer-metadata", "NEW_LYR_KEY=new_lyr",
         "--unset-layer-metadata", "OLD_LYR_KEY", "--overwrite", in_gpkg,
         out_gpkg}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, out_ds);
  EXPECT_STREQ("new_val", out_ds->GetMetadataItem("NEW_DS_KEY"));
  EXPECT_EQ(nullptr, out_ds->GetMetadataItem("OLD_DS_KEY"));
  EXPECT_STREQ("keep", out_ds->GetMetadataItem("KEEP_DS_KEY"));

  // layer1 was edited.
  OGRLayer* l1 = out_ds->GetLayerByName("layer1");
  ASSERT_NE(nullptr, l1);
  EXPECT_EQ(wkbMultiPoint, l1->GetGeomType());
  EXPECT_STREQ("new_lyr", l1->GetMetadataItem("NEW_LYR_KEY"));
  EXPECT_EQ(nullptr, l1->GetMetadataItem("OLD_LYR_KEY"));
  OGRSpatialReference expected_3857;
  ASSERT_EQ(OGRERR_NONE, expected_3857.importFromEPSG(3857));
  expected_3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ASSERT_NE(nullptr, l1->GetSpatialRef());
  EXPECT_TRUE(l1->GetSpatialRef()->IsSame(&expected_3857));

  // layer2 was untouched.
  OGRLayer* l2 = out_ds->GetLayerByName("layer2");
  ASSERT_NE(nullptr, l2);
  EXPECT_EQ(wkbPoint, l2->GetGeomType());
  OGRSpatialReference expected_4326;
  ASSERT_EQ(OGRERR_NONE, expected_4326.importFromEPSG(4326));
  expected_4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ASSERT_NE(nullptr, l2->GetSpatialRef());
  EXPECT_TRUE(l2->GetSpatialRef()->IsSame(&expected_4326));
}

TEST_F(GdalalgVectorEditTest, UnsetCrsUnsetFidAndPipelineCapabilities) {
  const std::string in_gpkg = std::string(kTestDir) + "/in_fid.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateMultiLayerGpkg(in_gpkg));

  GDALVectorPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_gpkg, "!", "edit", "--crs", "none", "--unset-fid"}));
  ASSERT_TRUE(pipeline.Run());

  GDALDataset* out_ds = pipeline.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* l1 = out_ds->GetLayerByName("layer1");
  ASSERT_NE(nullptr, l1);

  EXPECT_STREQ("", l1->GetFIDColumn());
  EXPECT_EQ(nullptr, l1->GetSpatialRef());
  EXPECT_FALSE(l1->TestCapability(OLCRandomWrite));
  EXPECT_TRUE(l1->TestCapability(OLCStringsAsUTF8));

  auto feat = std::unique_ptr<OGRFeature>(l1->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_EQ(OGRNullFID, feat->GetFID());
  ASSERT_NE(nullptr, feat->GetGeometryRef());
  EXPECT_EQ(nullptr, feat->GetGeometryRef()->getSpatialReference());

  // Also check without --unset-fid to cover preserved GetFIDColumn().
  GDALVectorPipelineAlgorithm pipeline_keep_fid;
  ASSERT_TRUE(pipeline_keep_fid.ParseCommandLineArguments(
      {"read", in_gpkg, "!", "edit", "--metadata", "A=B"}));
  ASSERT_TRUE(pipeline_keep_fid.Run());
  OGRLayer* l1_keep =
      pipeline_keep_fid.GetOutputDataset().GetDatasetRef()->GetLayerByName(
          "layer1");
  ASSERT_NE(nullptr, l1_keep);
  EXPECT_STREQ("fid", l1_keep->GetFIDColumn());

  EXPECT_TRUE(pipeline.Finalize());
  EXPECT_TRUE(pipeline_keep_fid.Finalize());
}

}  // namespace
}  // namespace autotest2
