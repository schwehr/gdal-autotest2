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
// Tests for GDALVectorCheckGeometryAlgorithm
// (apps/gdalalg_vector_check_geometry.cpp and
// apps/gdalalg_vector_check_geometry.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_check_geometry.cpp

#include "third_party/gdal/apps/gdalalg_vector_check_geometry.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_check_geometry_test";

[[nodiscard]] bool AddGeometryFeature(OGRLayer* layer,
                                      std::unique_ptr<OGRGeometry> geom) {
  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  if (geom) {
    if (feat->SetGeometry(geom.get()) != OGRERR_NONE) {
      return false;
    }
  }
  return layer->CreateFeature(feat.get()) == OGRERR_NONE;
}

std::unique_ptr<GDALDataset> CreateTestDataset(bool multi_layer = false) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  EXPECT_NE(nullptr, mem_drv);
  if (!mem_drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  EXPECT_NE(nullptr, ds);
  if (!ds) return nullptr;

  auto* srs = new OGRSpatialReference();
  absl::Cleanup release_srs = [srs] { srs->Release(); };
  EXPECT_EQ(OGRERR_NONE, srs->importFromEPSG(4326));

  OGRLayer* layer = ds->CreateLayer("geoms", srs, wkbUnknown, nullptr);
  EXPECT_NE(nullptr, layer);
  if (!layer) return nullptr;

  // 1. Valid polygon.
  {
    OGRGeometry* g = nullptr;
    EXPECT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                               "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))", srs, &g));
    EXPECT_TRUE(AddGeometryFeature(layer, std::unique_ptr<OGRGeometry>(g)));
  }
  // 2. Self-intersecting bowtie polygon.
  {
    OGRGeometry* g = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt(
                  "POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))", srs, &g));
    EXPECT_TRUE(AddGeometryFeature(layer, std::unique_ptr<OGRGeometry>(g)));
  }
  // 3. Valid linestring.
  {
    OGRGeometry* g = nullptr;
    EXPECT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                               "LINESTRING (0 0, 5 5)", srs, &g));
    EXPECT_TRUE(AddGeometryFeature(layer, std::unique_ptr<OGRGeometry>(g)));
  }
  // 4. Non-simple self-intersecting linestring.
  {
    OGRGeometry* g = nullptr;
    EXPECT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                               "LINESTRING (0 0, 10 10, 0 10, 10 0)", srs, &g));
    EXPECT_TRUE(AddGeometryFeature(layer, std::unique_ptr<OGRGeometry>(g)));
  }
  // 5. Point (dimension < 1 triggers warning).
  {
    OGRGeometry* g = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt("POINT (1 2)", srs, &g));
    EXPECT_TRUE(AddGeometryFeature(layer, std::unique_ptr<OGRGeometry>(g)));
  }
  // 6. Null geometry.
  EXPECT_TRUE(AddGeometryFeature(layer, nullptr));
  // 7. Polygon with unclosed 2-point ring (fails exportToGEOS).
  {
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(1.0, 2.0);
    ring->addPoint(3.0, 4.0);
    poly->addRingDirectly(ring.release());
    EXPECT_TRUE(AddGeometryFeature(layer, std::move(poly)));
  }
  // 8. GeometryCollection containing a non-simple linestring.
  {
    OGRGeometry* g = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt(
                  "GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
                  "LINESTRING (0 0, 4 4, 0 4, 4 0))",
                  srs, &g));
    EXPECT_TRUE(AddGeometryFeature(layer, std::unique_ptr<OGRGeometry>(g)));
  }

  if (multi_layer) {
    OGRLayer* no_geom = ds->CreateLayer("no_geom", nullptr, wkbNone, nullptr);
    EXPECT_NE(nullptr, no_geom);
    OGRLayer* second = ds->CreateLayer("extra", srs, wkbPolygon, nullptr);
    EXPECT_NE(nullptr, second);
    OGRGeomFieldDefn geom2("geom2", wkbPolygon);
    EXPECT_EQ(OGRERR_NONE, second->CreateGeomField(&geom2));
    auto feat = std::make_unique<OGRFeature>(second->GetLayerDefn());
    OGRGeometry* g = nullptr;
    EXPECT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                               "POLYGON ((0 0, 2 2, 2 0, 0 2, 0 0))", srs, &g));
    EXPECT_EQ(OGRERR_NONE, feat->SetGeomFieldDirectly(1, g));
    EXPECT_EQ(OGRERR_NONE, second->CreateFeature(feat.get()));
  }

  return ds;
}

TEST(GdalalgVectorCheckGeometryTest, SingleLayerWithAndWithoutIncludeValid) {
  GDALRegister_MEM();
  RegisterOGRGeoPackage();

  std::unique_ptr<GDALDataset> src_ds = CreateTestDataset();
  ASSERT_NE(nullptr, src_ds);

  // 1. Default: only invalid features returned (bowtie, non-simple line,
  // unclosed ring, geometry collection).
  {
    WithQuietHandler quiet;
    GDALVectorCheckGeometryAlgorithmStandalone alg;
    EXPECT_EQ("check-geometry", alg.GetName());
    EXPECT_FALSE(alg.GetDescription().empty());
    EXPECT_FALSE(alg.GetHelpURL().empty());

    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(src_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("stream"));
    EXPECT_TRUE(alg.Run());

    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    ASSERT_EQ(1, out_ds->GetLayerCount());
    OGRLayer* out_lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, out_lyr);
    EXPECT_STREQ("error_location", out_lyr->GetName());
    EXPECT_EQ(0, out_lyr->TestCapability("RandomRead"));

    int count = 0;
    std::unique_ptr<OGRFeature> feat;
    while ((feat.reset(out_lyr->GetNextFeature())), feat != nullptr) {
      EXPECT_GE(feat->GetFieldIndex("error"), 0);
      EXPECT_NE(nullptr, feat->GetGeometryRef());
      ++count;
    }
    EXPECT_EQ(4, count);
    CPLErrorReset();
  }

  // Reset source layer reading cursor before reusing the MEM dataset.
  src_ds->GetLayer(0)->ResetReading();

  // 2. --include-valid returns all 8 features.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorCheckGeometryAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(src_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.GetArg("include-valid")->Set(true));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("stream"));
    EXPECT_TRUE(alg.Run());

    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* out_lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, out_lyr);
    int count = 0;
    std::unique_ptr<OGRFeature> feat;
    while ((feat.reset(out_lyr->GetNextFeature())), feat != nullptr) {
      ++count;
    }
    EXPECT_EQ(8, count);
    CPLErrorReset();
  }
}

TEST(GdalalgVectorCheckGeometryTest, MultiLayerGeomFieldPipelineAndErrors) {
  GDALRegister_MEM();
  RegisterOGRGeoPackage();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  std::unique_ptr<GDALDataset> multi_ds =
      CreateTestDataset(/*multi_layer=*/true);
  ASSERT_NE(nullptr, multi_ds);

  // 1. Multi-layer dataset skips wkbNone layer when input-layer is empty.
  {
    WithQuietHandler quiet;
    GDALVectorCheckGeometryAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(multi_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("stream"));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    ASSERT_EQ(2, out_ds->GetLayerCount());
    EXPECT_STREQ("error_location_geoms", out_ds->GetLayer(0)->GetName());
    EXPECT_STREQ("error_location_extra", out_ds->GetLayer(1)->GetName());
  }

  // 2. Select specific layer and geometry field geom2.
  {
    GDALVectorCheckGeometryAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(multi_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT_LAYER)
                    ->Set(std::vector<std::string>{"extra"}));
    ASSERT_TRUE(alg.GetArg("geometry-field")->Set("geom2"));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("stream"));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    ASSERT_EQ(1, out_ds->GetLayerCount());
    OGRLayer* lyr = out_ds->GetLayer(0);
    std::unique_ptr<OGRFeature> feat(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
  }

  // 3. Explicit layer with no geometry field fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVectorCheckGeometryAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(multi_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT_LAYER)
                    ->Set(std::vector<std::string>{"no_geom"}));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("stream"));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("has no geometry field"));
    CPLErrorReset();
  }

  // 4. Non-existent geometry field fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALVectorCheckGeometryAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(multi_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT_LAYER)
                    ->Set(std::vector<std::string>{"geoms"}));
    ASSERT_TRUE(alg.GetArg("geometry-field")->Set("nonexistent_geom"));
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("stream"));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("does not exist"));
    CPLErrorReset();
  }

  // 5. Pipeline execution writing GPKG.
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  {
    GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_NE(nullptr, gpkg_drv);
    GDALDatasetUniquePtr gpkg_ds(
        gpkg_drv->Create(in_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, gpkg_ds);
    OGRLayer* lyr = gpkg_ds->CreateLayer("poly", nullptr, wkbPolygon, nullptr);
    ASSERT_NE(nullptr, lyr);
    OGRGeometry* g = nullptr;
    ASSERT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt(
                  "POLYGON ((0 0, 4 4, 4 0, 0 4, 0 0))", nullptr, &g));
    ASSERT_TRUE(AddGeometryFeature(lyr, std::unique_ptr<OGRGeometry>(g)));
  }

  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "check-geometry", "!", "write", out_gpkg,
         "--overwrite"}));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());
  }

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* lyr = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);
  EXPECT_EQ(1, lyr->GetFeatureCount());
}

}  // namespace
}  // namespace autotest2
