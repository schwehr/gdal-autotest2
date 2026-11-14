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
// Tests for GDALVectorReadAlgorithm (apps/gdalalg_vector_read.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_read.cpp

#include "third_party/gdal/apps/gdalalg_vector_read.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
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

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_read_test";

std::unique_ptr<GDALDataset> CreateTwoLayerGpkg(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  for (const char* name : {"layer_one", "layer_two"}) {
    OGRLayer* lyr = ds->CreateLayer(name, nullptr, wkbPoint, nullptr);
    if (!lyr) return nullptr;
    OGRFieldDefn fld("src", OFTString);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld));
    for (int i = 0; i < 2; ++i) {
      auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
      feat->SetField("src", name);
      auto pt = std::make_unique<OGRPoint>(static_cast<double>(i), 1.0);
      feat->SetGeometryDirectly(pt.release());
      EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
    }
  }
  return ds;
}

class GdalalgVectorReadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorReadTest, Properties) {
  GDALVectorReadAlgorithm alg;
  EXPECT_EQ("read", alg.GetName());
  EXPECT_EQ("Read a vector dataset.", alg.GetDescription());
  EXPECT_TRUE(alg.CanBeFirstStep());
}

TEST_F(GdalalgVectorReadTest, ReadAllLayersAndFilteredLayer) {
  const std::string gpkg = std::string(kTestDir) + "/two_layers.gpkg";
  ASSERT_NE(nullptr, CreateTwoLayerGpkg(gpkg));

  // 1. Without --input-layer: forwards source dataset directly.
  {
    GDALVectorReadAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({gpkg}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(2, out_ds->GetLayerCount());
  }

  // 2. With --input-layer selecting only 'layer_two': creates
  //    GDALVectorPipelineReadOutputDataset and filters GetNextFeature().
  {
    GDALVectorReadAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--input-layer", "layer_two", gpkg}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(1, out_ds->GetLayerCount());
    EXPECT_EQ(nullptr, out_ds->GetLayer(-1));
    ASSERT_NE(nullptr, out_ds->GetLayer(0));
    EXPECT_STREQ("layer_two", out_ds->GetLayer(0)->GetName());
    EXPECT_EQ(nullptr, out_ds->GetLayer(1));

    // TestCapability forwarding for ODsCRandomLayerRead and false otherwise.
    EXPECT_EQ(out_ds->TestCapability(ODsCRandomLayerRead),
              alg.GetInputDatasetRef()->TestCapability(ODsCRandomLayerRead));
    EXPECT_FALSE(out_ds->TestCapability(ODsCCreateLayer));

    // Iterate via dataset-level GetNextFeature (skips layer_one features),
    // then ResetReading() and iterate again.
    out_ds->ResetReading();
    int count = 0;
    while (true) {
      OGRLayer* belonging = nullptr;
      auto feat = std::unique_ptr<OGRFeature>(
          out_ds->GetNextFeature(&belonging, nullptr, nullptr, nullptr));
      if (!feat) break;
      ASSERT_NE(nullptr, belonging);
      EXPECT_STREQ("layer_two", belonging->GetName());
      EXPECT_STREQ("layer_two", feat->GetFieldAsString("src"));
      ++count;
    }
    EXPECT_EQ(2, count);
  }
}

TEST_F(GdalalgVectorReadTest, PipelineIntegrationAndMissingLayerError) {
  const std::string gpkg = std::string(kTestDir) + "/pipe.gpkg";
  ASSERT_NE(nullptr, CreateTwoLayerGpkg(gpkg));

  // Pipeline with read --input-layer.
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", "--input-layer", "layer_one", gpkg}));
    ASSERT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
    EXPECT_STREQ("layer_one", ds->GetLayer(0)->GetName());
  }

  // Non-existent --input-layer fails Run().
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorReadAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--input-layer", "no_such_layer", gpkg}));
    EXPECT_FALSE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
