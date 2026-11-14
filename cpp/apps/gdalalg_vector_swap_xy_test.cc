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
// Tests for GDALVectorSwapXYAlgorithm (apps/gdalalg_vector_swap_xy.cpp and
// apps/gdalalg_vector_swap_xy.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_swap_xy.cpp

#include "third_party/gdal/apps/gdalalg_vector_swap_xy.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_swap_xy_test";

std::unique_ptr<GDALDataset> CreateSwapTestDataset() {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  OGRLayer* lyr1 = ds->CreateLayer("pts", nullptr, wkbUnknown, nullptr);
  if (!lyr1) return nullptr;
  OGRGeomFieldDefn geom2("g2", wkbPoint);
  if (lyr1->CreateGeomField(&geom2) != OGRERR_NONE) return nullptr;

  {
    auto feat = std::make_unique<OGRFeature>(lyr1->GetLayerDefn());
    auto [line, err] =
        OGRGeometryFactory::createFromWkt("LINESTRING (1 10, 3 30)");
    if (err != OGRERR_NONE) return nullptr;
    auto pt2 = std::make_unique<OGRPoint>(5.0, 50.0);
    if (feat->SetGeomFieldDirectly(0, line.release()) != OGRERR_NONE ||
        feat->SetGeomFieldDirectly(1, pt2.release()) != OGRERR_NONE ||
        lyr1->CreateFeature(feat.get()) != OGRERR_NONE) {
      return nullptr;
    }
  }
  {
    auto feat_null = std::make_unique<OGRFeature>(lyr1->GetLayerDefn());
    if (lyr1->CreateFeature(feat_null.get()) != OGRERR_NONE) return nullptr;
  }

  OGRLayer* lyr2 = ds->CreateLayer("untouched", nullptr, wkbPoint, nullptr);
  if (!lyr2) return nullptr;
  auto feat2 = std::make_unique<OGRFeature>(lyr2->GetLayerDefn());
  auto pt = std::make_unique<OGRPoint>(2.0, 20.0);
  if (feat2->SetGeometryDirectly(pt.release()) != OGRERR_NONE ||
      lyr2->CreateFeature(feat2.get()) != OGRERR_NONE) {
    return nullptr;
  }
  return ds;
}

class GdalalgVectorSwapXYTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorSwapXYTest, PropertiesSwapCoordinatesAndExtent) {
  auto src = CreateSwapTestDataset();
  ASSERT_NE(nullptr, src);

  GDALVectorSwapXYAlgorithm alg;
  EXPECT_EQ("swap-xy", alg.GetName());
  EXPECT_STREQ("Swap X and Y coordinates of geometries of a vector dataset.",
               alg.GetDescription().c_str());
  EXPECT_STREQ("/programs/gdal_vector_swap_xy.html", alg.GetHelpURL().c_str());

  alg.GetInputDatasets().emplace_back(src.get());
  ASSERT_TRUE(alg.ParseCommandLineArguments({}));
  EXPECT_TRUE(alg.Run());

  GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, out);
  ASSERT_EQ(2, out->GetLayerCount());

  OGRLayer* out_lyr = out->GetLayerByName("pts");
  ASSERT_NE(nullptr, out_lyr);

  // Verify IGetExtent swaps X and Y bounds.
  OGREnvelope env;
  EXPECT_EQ(OGRERR_NONE, out_lyr->GetExtent(&env, TRUE));
  EXPECT_DOUBLE_EQ(10.0, env.MinX);
  EXPECT_DOUBLE_EQ(30.0, env.MaxX);
  EXPECT_DOUBLE_EQ(1.0, env.MinY);
  EXPECT_DOUBLE_EQ(3.0, env.MaxY);

  // Verify feature coordinates swapped on both geometry fields.
  out_lyr->ResetReading();
  std::unique_ptr<OGRFeature> f1(out_lyr->GetNextFeature());
  ASSERT_NE(nullptr, f1);
  const OGRLineString* line = f1->GetGeomFieldRef(0)->toLineString();
  ASSERT_EQ(2, line->getNumPoints());
  EXPECT_DOUBLE_EQ(10.0, line->getX(0));
  EXPECT_DOUBLE_EQ(1.0, line->getY(0));
  EXPECT_DOUBLE_EQ(30.0, line->getX(1));
  EXPECT_DOUBLE_EQ(3.0, line->getY(1));

  const OGRPoint* pt2 = f1->GetGeomFieldRef(1)->toPoint();
  EXPECT_DOUBLE_EQ(50.0, pt2->getX());
  EXPECT_DOUBLE_EQ(5.0, pt2->getY());

  // Null geometry feature remains null.
  std::unique_ptr<OGRFeature> f2(out_lyr->GetNextFeature());
  ASSERT_NE(nullptr, f2);
  EXPECT_EQ(nullptr, f2->GetGeomFieldRef(0));
}

TEST_F(GdalalgVectorSwapXYTest, ActiveLayerAndActiveGeometryRestriction) {
  auto src = CreateSwapTestDataset();
  ASSERT_NE(nullptr, src);

  GDALVectorSwapXYAlgorithm alg;
  alg.GetInputDatasets().emplace_back(src.get());
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"--active-layer", "pts", "--active-geometry", "g2"}));
  EXPECT_TRUE(alg.Run());

  GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, out);

  OGRLayer* lyr1 = out->GetLayerByName("pts");
  ASSERT_NE(nullptr, lyr1);
  lyr1->ResetReading();
  std::unique_ptr<OGRFeature> f1(lyr1->GetNextFeature());
  ASSERT_NE(nullptr, f1);
  // First geometry field was not selected -> unchanged.
  const OGRLineString* line = f1->GetGeomFieldRef(0)->toLineString();
  EXPECT_DOUBLE_EQ(1.0, line->getX(0));
  EXPECT_DOUBLE_EQ(10.0, line->getY(0));
  // Selected geometry field "g2" was swapped.
  const OGRPoint* pt2 = f1->GetGeomFieldRef(1)->toPoint();
  EXPECT_DOUBLE_EQ(50.0, pt2->getX());
  EXPECT_DOUBLE_EQ(5.0, pt2->getY());

  // Inactive layer "untouched" passed through unchanged.
  OGRLayer* lyr2 = out->GetLayerByName("untouched");
  ASSERT_NE(nullptr, lyr2);
  lyr2->ResetReading();
  std::unique_ptr<OGRFeature> f2(lyr2->GetNextFeature());
  ASSERT_NE(nullptr, f2);
  const OGRPoint* pt_other = f2->GetGeometryRef()->toPoint();
  EXPECT_DOUBLE_EQ(2.0, pt_other->getX());
  EXPECT_DOUBLE_EQ(20.0, pt_other->getY());
}

TEST_F(GdalalgVectorSwapXYTest, StandaloneAndPipelineExecution) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";

  GDALDriver* gpkg = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, gpkg);
  {
    std::unique_ptr<GDALDataset> ds(
        gpkg->Create(in_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->CreateLayer("pts", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, lyr);
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(7.0, 77.0);
    ASSERT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  // Standalone execution.
  {
    GDALVectorSwapXYAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--overwrite", in_gpkg, out_gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out);
    std::unique_ptr<OGRFeature> f(out->GetLayer(0)->GetNextFeature());
    ASSERT_NE(nullptr, f);
    const OGRPoint* pt = f->GetGeometryRef()->toPoint();
    EXPECT_DOUBLE_EQ(77.0, pt->getX());
    EXPECT_DOUBLE_EQ(7.0, pt->getY());
  }

  // Vector pipeline execution.
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(
        pipe.ParseCommandLineArguments({"read", in_gpkg, "!", "swap-xy", "!",
                                        "write", out_gpkg, "--overwrite"}));
    EXPECT_TRUE(pipe.Run());
    EXPECT_TRUE(pipe.Finalize());
  }
}

}  // namespace
}  // namespace autotest2
