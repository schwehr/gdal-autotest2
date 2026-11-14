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
// Tests for GDALVectorLimitAlgorithm (apps/gdalalg_vector_limit.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_limit.cpp
//   https://gdal.org/programs/gdal_vector_limit.html

#include "third_party/gdal/apps/gdalalg_vector_limit.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_limit_test";

void CreateTwoLayerGpkg(const std::string& path, int num_features_layer1,
                        int num_features_layer2) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  auto add_layer = [&](const char* name, int count) {
    OGRLayer* layer = ds->CreateLayer(name, nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, layer);
    OGRFieldDefn field("idx", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field));
    for (int i = 0; i < count; ++i) {
      auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
      feat->SetField("idx", i);
      feat->SetGeometryDirectly(new OGRPoint(i, i));
      ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
    }
  };

  ASSERT_NO_FATAL_FAILURE(add_layer("layer1", num_features_layer1));
  ASSERT_NO_FATAL_FAILURE(add_layer("layer2", num_features_layer2));
}

class GdalalgVectorLimitTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorLimitTest, BasicPropertiesAndMissingRequiredArg) {
  GDALVectorLimitAlgorithm alg;
  EXPECT_EQ("limit", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_vector_pipeline.html", alg.GetHelpURL());

  WithQuietHandler quiet;
  EXPECT_FALSE(alg.ParseCommandLineArguments({}));
}

TEST_F(GdalalgVectorLimitTest, LimitAllLayersDirectInspection) {
  const std::string in_gpkg = std::string(kTestDir) + "/in_all.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTwoLayerGpkg(in_gpkg, 5, 2));

  GDALVectorPipelineAlgorithm pipeline;
  ASSERT_TRUE(
      pipeline.ParseCommandLineArguments({"read", in_gpkg, "!", "limit", "3"}));
  ASSERT_TRUE(pipeline.Run());

  GDALDataset* out_ds = pipeline.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, out_ds);
  ASSERT_EQ(2, out_ds->GetLayerCount());

  // layer1 has 5 source features -> limited to 3.
  OGRLayer* l1 = out_ds->GetLayerByName("layer1");
  ASSERT_NE(nullptr, l1);
  EXPECT_NE(nullptr, l1->GetLayerDefn());
  EXPECT_EQ(3, l1->GetFeatureCount(/*bForce=*/1));
  EXPECT_TRUE(l1->TestCapability(OLCStringsAsUTF8));
  EXPECT_EQ(1, l1->TestCapability(OLCFastFeatureCount));

  l1->ResetReading();
  int count1 = 0;
  while (auto feat = std::unique_ptr<OGRFeature>(l1->GetNextFeature())) {
    EXPECT_EQ(count1, feat->GetFieldAsInteger("idx"));
    ++count1;
  }
  EXPECT_EQ(3, count1);
  // Extra GetNextFeature after hitting limit returns nullptr.
  EXPECT_EQ(nullptr, l1->GetNextFeature());

  // Re-reading after ResetReading yields the same 3 features.
  l1->ResetReading();
  int recount = 0;
  while (auto feat = std::unique_ptr<OGRFeature>(l1->GetNextFeature())) {
    ++recount;
  }
  EXPECT_EQ(3, recount);

  // layer2 has 2 source features -> min(3, 2) = 2.
  OGRLayer* l2 = out_ds->GetLayerByName("layer2");
  ASSERT_NE(nullptr, l2);
  EXPECT_EQ(2, l2->GetFeatureCount(/*bForce=*/1));

  EXPECT_TRUE(pipeline.Finalize());
}

TEST_F(GdalalgVectorLimitTest, LimitActiveLayerWithWrite) {
  const std::string in_gpkg = std::string(kTestDir) + "/in_active.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out_active.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTwoLayerGpkg(in_gpkg, 5, 4));

  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "limit", "2", "--active-layer", "layer1", "!",
         "write", out_gpkg, "--overwrite"}));
    ASSERT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());
  }

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, out_ds);
  ASSERT_EQ(2, out_ds->GetLayerCount());

  OGRLayer* l1 = out_ds->GetLayerByName("layer1");
  ASSERT_NE(nullptr, l1);
  EXPECT_EQ(2, l1->GetFeatureCount(1));

  // layer2 was not the active layer, so all 4 features pass through.
  OGRLayer* l2 = out_ds->GetLayerByName("layer2");
  ASSERT_NE(nullptr, l2);
  EXPECT_EQ(4, l2->GetFeatureCount(1));
}

TEST_F(GdalalgVectorLimitTest, ZeroLimit) {
  const std::string in_gpkg = std::string(kTestDir) + "/in_zero.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTwoLayerGpkg(in_gpkg, 4, 1));

  GDALVectorPipelineAlgorithm pipeline;
  ASSERT_TRUE(
      pipeline.ParseCommandLineArguments({"read", in_gpkg, "!", "limit", "0"}));
  ASSERT_TRUE(pipeline.Run());

  GDALDataset* out_ds = pipeline.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* l1 = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, l1);
  EXPECT_EQ(0, l1->GetFeatureCount(1));
  EXPECT_EQ(nullptr, l1->GetNextFeature());
  EXPECT_TRUE(pipeline.Finalize());
}

}  // namespace
}  // namespace autotest2
