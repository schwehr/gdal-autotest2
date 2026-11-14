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
// Tests for GDALVectorFilterAlgorithm (apps/gdalalg_vector_filter.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_filter.cpp
//   https://gdal.org/programs/gdal_vector_filter.html

#include "third_party/gdal/apps/gdalalg_vector_filter.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_filter_test";

void CreateTestGpkg(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  auto add_points = [&](const char* layer_name) {
    OGRLayer* layer = ds->CreateLayer(layer_name, nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, layer);
    OGRFieldDefn field("val", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field));

    struct Pt {
      double x;
      double y;
      int val;
    };
    const Pt pts[] = {
        {0.5, 0.5, 1},
        {1.5, 1.5, 2},
        {5.0, 5.0, 3},
        {1.2, 0.8, 10},
    };
    for (const auto& p : pts) {
      auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
      feat->SetField("val", p.val);
      feat->SetGeometryDirectly(new OGRPoint(p.x, p.y));
      ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
    }
  };

  ASSERT_NO_FATAL_FAILURE(add_points("pts1"));
  ASSERT_NO_FATAL_FAILURE(add_points("pts2"));
}

class GdalalgVectorFilterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorFilterTest, BasicProperties) {
  GDALVectorFilterAlgorithmStandalone alg;
  EXPECT_EQ("filter", alg.GetName());
  EXPECT_EQ("Filter a vector dataset.", alg.GetDescription());
  EXPECT_EQ("/programs/gdal_vector_filter.html", alg.GetHelpURL());
}

TEST_F(GdalalgVectorFilterTest, BboxAndWhereStandalone) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTestGpkg(in_gpkg));

  // Filter by both bbox [0, 0, 2, 2] (excludes (5, 5)) and where "val >= 2"
  // (excludes (0.5, 0.5, 1)), leaving 2 features per layer.
  {
    GDALVectorFilterAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--bbox", "0,0,2,2", "--where",
                                               "val >= 2", "--overwrite",
                                               in_gpkg, out_gpkg}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, out_ds);
  EXPECT_EQ(2, out_ds->GetLayerCount());
  EXPECT_EQ(2, out_ds->GetLayerByName("pts1")->GetFeatureCount(1));
  EXPECT_EQ(2, out_ds->GetLayerByName("pts2")->GetFeatureCount(1));
}

TEST_F(GdalalgVectorFilterTest, ActiveLayerAndWhereFromFile) {
  const std::string in_gpkg = std::string(kTestDir) + "/in_active.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out_active.gpkg";
  const std::string where_file = std::string(kTestDir) + "/query.sql";
  ASSERT_NO_FATAL_FAILURE(CreateTestGpkg(in_gpkg));

  VsiMemTempWrapper sql_file(where_file, "-- SQL comment\nval = 10");

  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "filter", "--active-layer", "pts1", "--bbox",
         "0,0,2,2", "--where", "@" + where_file, "!", "write", out_gpkg,
         "--overwrite"}));
    ASSERT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());
  }

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, out_ds);
  EXPECT_EQ(1, out_ds->GetLayerByName("pts1")->GetFeatureCount(1));
  // pts2 is not active layer, so all 4 features remain.
  EXPECT_EQ(4, out_ds->GetLayerByName("pts2")->GetFeatureCount(1));
}

TEST_F(GdalalgVectorFilterTest, InvalidWhereFails) {
  const std::string in_gpkg = std::string(kTestDir) + "/in_bad.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out_bad.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateTestGpkg(in_gpkg));

  GDALVectorFilterAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"--where", "NONEXISTENT_COL = (", "--overwrite", in_gpkg, out_gpkg}));

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(alg.Run());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
