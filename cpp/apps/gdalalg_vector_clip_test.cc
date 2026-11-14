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
// Tests for GDALVectorClipAlgorithm (apps/gdalalg_vector_clip.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_clip.cpp

#include "third_party/gdal/apps/gdalalg_vector_clip.h"

#include <memory>
#include <string>
#include <vector>

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
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_clip_test";

std::unique_ptr<GDALDataset> CreatePolygonGpkg(const std::string& path,
                                               bool with_srs = true,
                                               bool two_layers = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  OGRSpatialReference srs;
  if (with_srs) {
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    EXPECT_EQ(OGRERR_NONE, srs.SetWellKnownGeogCS("WGS84"));
  }

  OGRLayer* lyr =
      ds->CreateLayer("polys", with_srs ? &srs : nullptr, wkbPolygon, nullptr);
  if (!lyr) return nullptr;
  OGRFieldDefn fld("name", OFTString);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld));

  // Feature 1: square [0,0]..[10,10]
  {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("name", "sq1");
    auto [geom, err] =
        OGRGeometryFactory::createFromWkt("POLYGON((0 0,10 0,10 10,0 10,0 0))");
    EXPECT_EQ(OGRERR_NONE, err);
    feat->SetGeometryDirectly(geom.release());
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  // Feature 2: disjoint square [20,20]..[30,30]
  {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("name", "disjoint");
    auto [geom, err] = OGRGeometryFactory::createFromWkt(
        "POLYGON((20 20,30 20,30 30,20 30,20 20))");
    EXPECT_EQ(OGRERR_NONE, err);
    feat->SetGeometryDirectly(geom.release());
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  // Feature 3: null geometry (exercising nullptr geometry skip).
  {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("name", "null_geom");
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  if (two_layers) {
    OGRLayer* lyr2 =
        ds->CreateLayer("other", with_srs ? &srs : nullptr, wkbPoint, nullptr);
    if (!lyr2) return nullptr;
    auto feat = std::make_unique<OGRFeature>(lyr2->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(50.0, 50.0);
    feat->SetGeometryDirectly(pt.release());
    EXPECT_EQ(OGRERR_NONE, lyr2->CreateFeature(feat.get()));
  }

  return ds;
}

std::unique_ptr<GDALDataset> CreateMemTypedLayer(OGRwkbGeometryType geom_type,
                                                 const char* wkt) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;
  OGRLayer* lyr = ds->CreateLayer("layer", nullptr, geom_type, nullptr);
  if (!lyr) return nullptr;
  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  auto [geom, err] = OGRGeometryFactory::createFromWkt(wkt);
  EXPECT_EQ(OGRERR_NONE, err);
  feat->SetGeometryDirectly(geom.release());
  EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  return ds;
}

class GdalalgVectorClipTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    RegisterOGRGeoPackage();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorClipTest, StandaloneProperties) {
  GDALVectorClipAlgorithmStandalone alg;
  EXPECT_EQ("clip", alg.GetName());
  EXPECT_EQ("Clip a vector dataset.", alg.GetDescription());
}

TEST_F(GdalalgVectorClipTest, ClipByBboxAndGeometryAndActiveLayer) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_bbox = std::string(kTestDir) + "/out_bbox.gpkg";
  const std::string out_wkt = std::string(kTestDir) + "/out_wkt.gpkg";
  ASSERT_NE(nullptr, CreatePolygonGpkg(in_gpkg, /*with_srs=*/true,
                                       /*two_layers=*/true));

  // 1. Standalone clip with --bbox, --bbox-crs, and --active-layer.
  //    Layer 'polys' is clipped; layer 'other' passes through unchanged.
  {
    GDALVectorClipAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--active-layer", "polys", "--bbox", "0,0,5,5", "--bbox-crs",
         "EPSG:4326", in_gpkg, out_bbox, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_bbox.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(2, out_ds->GetLayerCount());
    OGRLayer* polys = out_ds->GetLayerByName("polys");
    ASSERT_NE(nullptr, polys);
    EXPECT_EQ(1, polys->GetFeatureCount());
    auto feat = std::unique_ptr<OGRFeature>(polys->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_STREQ("sq1", feat->GetFieldAsString("name"));
    OGREnvelope env;
    feat->GetGeometryRef()->getEnvelope(&env);
    EXPECT_DOUBLE_EQ(0.0, env.MinX);
    EXPECT_DOUBLE_EQ(5.0, env.MaxX);

    OGRLayer* other = out_ds->GetLayerByName("other");
    ASSERT_NE(nullptr, other);
    EXPECT_EQ(1, other->GetFeatureCount());
  }

  // 2. Pipeline streaming with WKT --geometry and --geometry-crs +
  // capabilities.
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "clip", "--geometry",
         "POLYGON((2 2,8 2,8 8,2 8,2 2))", "--geometry-crs", "EPSG:4326"}));
    ASSERT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_TRUE(lyr->TestCapability(OLCStringsAsUTF8));
    EXPECT_FALSE(lyr->TestCapability(OLCRandomWrite));
    auto feat = std::unique_ptr<OGRFeature>(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    OGREnvelope env;
    feat->GetGeometryRef()->getEnvelope(&env);
    EXPECT_DOUBLE_EQ(2.0, env.MinX);
    EXPECT_DOUBLE_EQ(8.0, env.MaxX);
  }

  // 3. GeoJSON --geometry standalone write.
  {
    GDALVectorClipAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--geometry",
         R"({"type":"Polygon","coordinates":[[[1,1],[4,1],[4,4],[1,4],[1,1]]]})",
         in_gpkg, out_wkt, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
}

TEST_F(GdalalgVectorClipTest, ClipByLikeVectorAndRaster) {
  const std::string in_gpkg = std::string(kTestDir) + "/like_in.gpkg";
  const std::string like_gpkg = std::string(kTestDir) + "/like_vec.gpkg";
  const std::string like_tif = std::string(kTestDir) + "/like_ras.tif";
  const std::string like_nogt_tif = std::string(kTestDir) + "/like_nogt.tif";
  ASSERT_NE(nullptr, CreatePolygonGpkg(in_gpkg, true, false));
  ASSERT_NE(nullptr, CreatePolygonGpkg(like_gpkg, true, true));

  // Create raster with geotransform covering [0,6] x [0,6].
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, drv);
    std::unique_ptr<GDALDataset> ds(
        drv->Create(like_tif.c_str(), 6, 6, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt(0.0, 1.0, 0.0, 6.0, 0.0, -1.0);
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  }
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, drv);
    std::unique_ptr<GDALDataset> ds(
        drv->Create(like_nogt_tif.c_str(), 6, 6, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
  }

  // 1. --like vector with --like-layer and --like-where.
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "clip", "--like", like_gpkg, "--like-layer",
         "polys", "--like-where", "name='sq1'"}));
    EXPECT_TRUE(pipe.Run());
    GDALDataset* ds = pipe.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayer(0)->GetFeatureCount());
  }

  // 2. --like vector with --like-sql.
  {
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "clip", "--like", like_gpkg, "--like-sql",
         "SELECT * FROM polys WHERE name='sq1'"}));
    EXPECT_TRUE(pipe.Run());
  }

  // 3. --like raster without SRS warns, with geotransform clips.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "clip", "--like", like_tif}));
    EXPECT_TRUE(pipe.Run());
    CPLErrorReset();
  }

  // 4. --like raster without geotransform fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "clip", "--like", like_nogt_tif}));
    EXPECT_FALSE(pipe.Run());
    CPLErrorReset();
  }
}

TEST_F(GdalalgVectorClipTest, TranslateFeatureCollectionAndTypePromotions) {
  // 1. C-shaped polygon clipped by a rectangle yields MultiPolygon -> split
  //    into 2 Polygon features on a wkbPolygon layer.
  {
    auto src_ds = CreateMemTypedLayer(
        wkbPolygon, "POLYGON((0 0,10 0,10 2,2 2,2 8,10 8,10 10,0 10,0 0))");
    ASSERT_NE(nullptr, src_ds);

    GDALVectorClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--bbox", "4,0,10,10"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    int count = 0;
    for (const auto& feat : lyr) {
      ASSERT_NE(nullptr, feat->GetGeometryRef());
      EXPECT_EQ(wkbPolygon,
                wkbFlatten(feat->GetGeometryRef()->getGeometryType()));
      ++count;
    }
    EXPECT_EQ(2, count);
  }

  // 2. wkbMultiPolygon layer promotes single Polygon intersection to
  //    MultiPolygon.
  {
    auto src_ds = CreateMemTypedLayer(
        wkbMultiPolygon, "MULTIPOLYGON(((0 0,10 0,10 10,0 10,0 0)))");
    ASSERT_NE(nullptr, src_ds);

    GDALVectorClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--bbox", "0,0,5,5"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    auto feat =
        std::unique_ptr<OGRFeature>(out_ds->GetLayer(0)->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(wkbMultiPolygon,
              wkbFlatten(feat->GetGeometryRef()->getGeometryType()));
  }

  // 3. wkbGeometryCollection layer wraps intersection in GeometryCollection.
  {
    auto src_ds = CreateMemTypedLayer(
        wkbGeometryCollection,
        "GEOMETRYCOLLECTION(POLYGON((0 0,10 0,10 10,0 10,0 0)))");
    ASSERT_NE(nullptr, src_ds);

    GDALVectorClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--bbox", "0,0,5,5"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    auto feat =
        std::unique_ptr<OGRFeature>(out_ds->GetLayer(0)->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(wkbGeometryCollection,
              wkbFlatten(feat->GetGeometryRef()->getGeometryType()));
  }

  // 4. Touching boundary yields LineString/Point on wkbPolygon layer ->
  //    discarded as incompatible type.
  {
    auto src_ds =
        CreateMemTypedLayer(wkbPolygon, "POLYGON((0 0,5 0,5 5,0 5,0 0))");
    ASSERT_NE(nullptr, src_ds);

    GDALVectorClipAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--bbox", "5,0,10,5"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(0, out_ds->GetLayer(0)->GetFeatureCount());
  }
}

TEST_F(GdalalgVectorClipTest, ErrorPaths) {
  const std::string in_gpkg = std::string(kTestDir) + "/err.gpkg";
  ASSERT_NE(nullptr, CreatePolygonGpkg(in_gpkg, true, true));

  // 1. Neither --bbox, --geometry, nor --like provided.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments({"read", in_gpkg, "!", "clip"}));
    EXPECT_FALSE(pipe.Run());
    CPLErrorReset();
  }

  // 2. Invalid --geometry string.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "clip", "--geometry", "NOT_WKT_OR_JSON"}));
    EXPECT_FALSE(pipe.Run());
    CPLErrorReset();
  }

  // 3. Multi-layer --like without --like-layer or --like-sql fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", in_gpkg, "!", "clip", "--like", in_gpkg}));
    EXPECT_FALSE(pipe.Run());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
