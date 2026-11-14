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
// Tests for GDALVectorBufferAlgorithm (apps/gdalalg_vector_buffer.cpp and
// apps/gdalalg_vector_buffer.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_buffer.cpp

#include "third_party/gdal/apps/gdalalg_vector_buffer.h"

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
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_buffer_test";

std::unique_ptr<GDALDataset> CreateTestVectorDataset() {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  OGRSpatialReference srs;
  if (srs.importFromEPSG(32632) != OGRERR_NONE) return nullptr;

  // Layer 1: two geometry fields, one feature with LineString + Point,
  // and one feature with null geometries.
  OGRLayer* lyr1 = ds->CreateLayer("lines", &srs, wkbUnknown, nullptr);
  if (!lyr1) return nullptr;
  OGRGeomFieldDefn geom2("g2", wkbPoint);
  geom2.SetSpatialRef(&srs);
  if (lyr1->CreateGeomField(&geom2) != OGRERR_NONE) return nullptr;

  {
    auto feat = std::make_unique<OGRFeature>(lyr1->GetLayerDefn());
    auto [line, err1] =
        OGRGeometryFactory::createFromWkt("LINESTRING (0 0, 10 0)");
    if (err1 != OGRERR_NONE) return nullptr;
    auto pt = std::make_unique<OGRPoint>(5.0, 5.0);
    if (feat->SetGeomFieldDirectly(0, line.release()) != OGRERR_NONE ||
        feat->SetGeomFieldDirectly(1, pt.release()) != OGRERR_NONE ||
        lyr1->CreateFeature(feat.get()) != OGRERR_NONE) {
      return nullptr;
    }
  }
  {
    auto feat_null = std::make_unique<OGRFeature>(lyr1->GetLayerDefn());
    if (lyr1->CreateFeature(feat_null.get()) != OGRERR_NONE) return nullptr;
  }

  // Layer 2: passthrough verification when --active-layer is set.
  OGRLayer* lyr2 = ds->CreateLayer("other", &srs, wkbPoint, nullptr);
  if (!lyr2) return nullptr;
  auto feat2 = std::make_unique<OGRFeature>(lyr2->GetLayerDefn());
  auto pt2 = std::make_unique<OGRPoint>(1.0, 1.0);
  if (feat2->SetGeometryDirectly(pt2.release()) != OGRERR_NONE ||
      lyr2->CreateFeature(feat2.get()) != OGRERR_NONE) {
    return nullptr;
  }
  return ds;
}

class GdalalgVectorBufferTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorBufferTest, PropertiesAndArgumentValidation) {
  GDALVectorBufferAlgorithm alg;
  EXPECT_EQ("buffer", alg.GetName());
  EXPECT_STREQ("Compute a buffer around geometries of a vector dataset.",
               alg.GetDescription().c_str());
  EXPECT_STREQ("/programs/gdal_vector_buffer.html", alg.GetHelpURL().c_str());

  // Missing required distance fails.
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({}));
  }
  // Invalid endcap-style, join-style, mitre-limit, quadrant-segments, side.
  {
    GDALVectorBufferAlgorithm bad;
    WithQuietHandler quiet;
    EXPECT_FALSE(
        bad.ParseCommandLineArguments({"1.0", "--endcap-style", "invalid"}));
  }
  {
    GDALVectorBufferAlgorithm bad;
    WithQuietHandler quiet;
    EXPECT_FALSE(
        bad.ParseCommandLineArguments({"1.0", "--join-style", "invalid"}));
  }
  {
    GDALVectorBufferAlgorithm bad;
    WithQuietHandler quiet;
    EXPECT_FALSE(bad.ParseCommandLineArguments({"1.0", "--mitre-limit", "-1"}));
  }
  {
    GDALVectorBufferAlgorithm bad;
    WithQuietHandler quiet;
    EXPECT_FALSE(
        bad.ParseCommandLineArguments({"1.0", "--quadrant-segments", "0"}));
  }
  {
    GDALVectorBufferAlgorithm bad;
    WithQuietHandler quiet;
    EXPECT_FALSE(bad.ParseCommandLineArguments({"1.0", "--side", "top"}));
  }
}

TEST_F(GdalalgVectorBufferTest, BufferOptionsActiveLayerAndActiveGeometry) {
  auto src = CreateTestVectorDataset();
  ASSERT_NE(nullptr, src);

  // Buffer only layer "lines", geom field "g2", with square endcap and mitre.
  GDALVectorBufferAlgorithm alg;
  alg.GetInputDatasets().emplace_back(src.get());
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"2.0", "--active-layer", "lines", "--active-geometry", "g2",
       "--endcap-style", "square", "--join-style", "mitre", "--mitre-limit",
       "3.0", "--quadrant-segments", "4"}));
  EXPECT_TRUE(alg.Run());

  GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, out);
  ASSERT_EQ(2, out->GetLayerCount());

  OGRLayer* out_lines = out->GetLayerByName("lines");
  ASSERT_NE(nullptr, out_lines);
  out_lines->ResetReading();
  std::unique_ptr<OGRFeature> f1(out_lines->GetNextFeature());
  ASSERT_NE(nullptr, f1);
  // First geometry field was not selected -> stays LineString.
  EXPECT_EQ(wkbLineString,
            wkbFlatten(f1->GetGeomFieldRef(0)->getGeometryType()));
  // Second geometry field was buffered -> becomes Polygon with preserved SRS.
  const OGRGeometry* g2 = f1->GetGeomFieldRef(1);
  ASSERT_NE(nullptr, g2);
  EXPECT_EQ(wkbPolygon, wkbFlatten(g2->getGeometryType()));
  ASSERT_NE(nullptr, g2->getSpatialReference());
  EXPECT_STREQ("32632", g2->getSpatialReference()->GetAuthorityCode(nullptr));

  // Second feature had null geometries -> stays null.
  std::unique_ptr<OGRFeature> f2(out_lines->GetNextFeature());
  ASSERT_NE(nullptr, f2);
  EXPECT_EQ(nullptr, f2->GetGeomFieldRef(0));
  EXPECT_EQ(nullptr, f2->GetGeomFieldRef(1));

  // Inactive layer "other" passed through unchanged.
  OGRLayer* out_other = out->GetLayerByName("other");
  ASSERT_NE(nullptr, out_other);
  out_other->ResetReading();
  std::unique_ptr<OGRFeature> f_other(out_other->GetNextFeature());
  ASSERT_NE(nullptr, f_other);
  EXPECT_EQ(wkbPoint, wkbFlatten(f_other->GetGeometryRef()->getGeometryType()));
}

TEST_F(GdalalgVectorBufferTest, SingleSidedLeftAndRightBuffers) {
  auto src = CreateTestVectorDataset();
  ASSERT_NE(nullptr, src);

  // Left-sided buffer on (0,0)-(10,0) extends into Y > 0.
  {
    GDALVectorBufferAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"2.0", "--active-layer", "lines", "--active-geometry", "",
         "--endcap-style", "flat", "--join-style", "bevel", "--side", "left"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out);
    OGRLayer* lyr = out->GetLayer(0);
    lyr->ResetReading();
    std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f);
    OGREnvelope env;
    f->GetGeomFieldRef(0)->getEnvelope(&env);
    EXPECT_NEAR(0.0, env.MinY, 1e-6);
    EXPECT_NEAR(2.0, env.MaxY, 1e-6);
  }

  // Right-sided buffer negates distance and extends into Y < 0.
  {
    GDALVectorBufferAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"2.0", "--active-layer", "lines", "--side", "right"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out);
    OGRLayer* lyr = out->GetLayer(0);
    lyr->ResetReading();
    std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f);
    OGREnvelope env;
    f->GetGeomFieldRef(0)->getEnvelope(&env);
    EXPECT_NEAR(-2.0, env.MinY, 1e-6);
    EXPECT_NEAR(0.0, env.MaxY, 1e-6);
  }
}

TEST_F(GdalalgVectorBufferTest, StandaloneAndPipelineExecution) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";

  GDALDriver* gpkg = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, gpkg);
  {
    std::unique_ptr<GDALDataset> created(
        gpkg->Create(in_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, created);
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(32632));
    OGRLayer* lyr = created->CreateLayer("pts", &srs, wkbPoint, nullptr);
    ASSERT_NE(nullptr, lyr);
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(500000.0, 4000000.0);
    ASSERT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  // Standalone execution.
  {
    GDALVectorBufferAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--overwrite", "--distance", "1.5", in_gpkg, out_gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // Vector pipeline execution.
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments({"read", in_gpkg, "!", "buffer",
                                                "1.5", "!", "write", out_gpkg,
                                                "--overwrite"}));
    EXPECT_TRUE(pipe.Run());
    EXPECT_TRUE(pipe.Finalize());
  }
}

}  // namespace
}  // namespace autotest2
