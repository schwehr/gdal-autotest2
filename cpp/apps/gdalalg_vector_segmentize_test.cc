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
// Tests for GDALVectorSegmentizeAlgorithm (apps/gdalalg_vector_segmentize.cpp
// and apps/gdalalg_vector_segmentize.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_segmentize.cpp

#include "third_party/gdal/apps/gdalalg_vector_segmentize.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
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
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_segmentize_test";

std::unique_ptr<GDALDataset> CreateSegmentizeDataset() {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  EXPECT_NE(nullptr, mem_drv);
  if (!mem_drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  EXPECT_NE(nullptr, ds);
  if (!ds) return nullptr;

  OGRLayer* lyr1 = ds->CreateLayer("lines", nullptr, wkbLineString, nullptr);
  EXPECT_NE(nullptr, lyr1);
  OGRGeomFieldDefn geom2("geom2", wkbLineString);
  EXPECT_EQ(OGRERR_NONE, lyr1->CreateGeomField(&geom2));

  {
    auto feat = std::make_unique<OGRFeature>(lyr1->GetLayerDefn());
    auto line1 = std::make_unique<OGRLineString>();
    line1->addPoint(0.0, 0.0);
    line1->addPoint(10.0, 0.0);
    EXPECT_EQ(OGRERR_NONE, feat->SetGeomFieldDirectly(0, line1.release()));

    auto line2 = std::make_unique<OGRLineString>();
    line2->addPoint(0.0, 0.0);
    line2->addPoint(0.0, 10.0);
    EXPECT_EQ(OGRERR_NONE, feat->SetGeomFieldDirectly(1, line2.release()));
    EXPECT_EQ(OGRERR_NONE, lyr1->CreateFeature(feat.get()));
  }
  // Feature with null geometries.
  {
    auto feat = std::make_unique<OGRFeature>(lyr1->GetLayerDefn());
    EXPECT_EQ(OGRERR_NONE, lyr1->CreateFeature(feat.get()));
  }

  OGRLayer* lyr2 = ds->CreateLayer("other", nullptr, wkbLineString, nullptr);
  EXPECT_NE(nullptr, lyr2);
  {
    auto feat = std::make_unique<OGRFeature>(lyr2->GetLayerDefn());
    auto line = std::make_unique<OGRLineString>();
    line->addPoint(0.0, 0.0);
    line->addPoint(10.0, 0.0);
    EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(line.release()));
    EXPECT_EQ(OGRERR_NONE, lyr2->CreateFeature(feat.get()));
  }

  return ds;
}

TEST(GdalalgVectorSegmentizeTest, PropertiesAndValidationErrors) {
  GDALVectorSegmentizeAlgorithmStandalone alg;
  EXPECT_EQ("segmentize", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_FALSE(alg.GetHelpURL().empty());

  WithQuietHandler quiet;
  EXPECT_FALSE(alg.ParseCommandLineArguments({"in.gpkg", "out.gpkg"}));
  EXPECT_FALSE(
      alg.ParseCommandLineArguments({"--max-length=0", "in.gpkg", "out.gpkg"}));
}

TEST(GdalalgVectorSegmentizeTest, SegmentizeAllAndFilteredFields) {
  GDALRegister_MEM();
  std::unique_ptr<GDALDataset> src_ds = CreateSegmentizeDataset();
  ASSERT_NE(nullptr, src_ds);

  // 1. Segmentize all geometry fields across all layers.
  {
    GDALVectorSegmentizeAlgorithm alg(/*standaloneStep=*/false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"2.5"}));
    EXPECT_TRUE(alg.Run());

    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr1 = out_ds->GetLayerByName("lines");
    ASSERT_NE(nullptr, lyr1);
    std::unique_ptr<OGRFeature> feat(lyr1->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(5, feat->GetGeomFieldRef(0)->toLineString()->getNumPoints());
    EXPECT_EQ(5, feat->GetGeomFieldRef(1)->toLineString()->getNumPoints());

    std::unique_ptr<OGRFeature> null_feat(lyr1->GetNextFeature());
    ASSERT_NE(nullptr, null_feat);
    EXPECT_EQ(nullptr, null_feat->GetGeomFieldRef(0));
  }

  // 2. Restrict to --active-layer=lines and --active-geometry=geom2.
  {
    ASSERT_NE(nullptr, src_ds->GetLayerByName("lines"));
    src_ds->GetLayerByName("lines")->ResetReading();
    GDALVectorSegmentizeAlgorithm alg(/*standaloneStep=*/false);
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--active-layer", "lines", "--active-geometry", "geom2", "2.5"}));
    EXPECT_TRUE(alg.Run());

    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr1 = out_ds->GetLayerByName("lines");
    ASSERT_NE(nullptr, lyr1);
    std::unique_ptr<OGRFeature> f1(lyr1->GetNextFeature());
    ASSERT_NE(nullptr, f1);
    EXPECT_EQ(2, f1->GetGeomFieldRef(0)->toLineString()->getNumPoints());
    EXPECT_EQ(5, f1->GetGeomFieldRef(1)->toLineString()->getNumPoints());

    OGRLayer* lyr2 = out_ds->GetLayerByName("other");
    ASSERT_NE(nullptr, lyr2);
    std::unique_ptr<OGRFeature> f2(lyr2->GetNextFeature());
    ASSERT_NE(nullptr, f2);
    EXPECT_EQ(2, f2->GetGeometryRef()->toLineString()->getNumPoints());
  }
}

TEST(GdalalgVectorSegmentizeTest, PipelineExecution) {
  GDALRegister_MEM();
  RegisterOGRGeoPackage();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_NE(nullptr, drv);
    GDALDatasetUniquePtr ds(
        drv->Create(in_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->CreateLayer("lines", nullptr, wkbLineString, nullptr);
    ASSERT_NE(nullptr, lyr);
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    auto line = std::make_unique<OGRLineString>();
    line->addPoint(0.0, 0.0);
    line->addPoint(10.0, 0.0);
    feat->SetGeometryDirectly(line.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "segmentize", "2", "!", "write", out_gpkg,
         "--overwrite"}));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());
  }

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* lyr = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);
  std::unique_ptr<OGRFeature> feat(lyr->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_EQ(6, feat->GetGeometryRef()->toLineString()->getNumPoints());
}

}  // namespace
}  // namespace autotest2
