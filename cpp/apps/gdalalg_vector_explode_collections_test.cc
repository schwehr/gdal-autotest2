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
// Tests for GDALVectorExplodeCollectionsAlgorithm
// (apps/gdalalg_vector_explode_collections.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_explode_collections.cpp

#include "third_party/gdal/apps/gdalalg_vector_explode_collections.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
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

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_explode_collections_test";

class GdalalgVectorExplodeCollectionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

std::unique_ptr<GDALDataset> CreateMultiGeomCollectionMemDataset() {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  OGRSpatialReference srs;
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));

  OGRLayer* lyr =
      ds->CreateLayer("mixed", &srs, wkbGeometryCollection, nullptr);
  if (!lyr) return nullptr;
  OGRFieldDefn name_field("name", OFTString);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&name_field));

  // Feature 1: GeometryCollection(Point(1,2), LineString(0 0, 1 1)).
  {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("name", "gc");
    auto coll = std::make_unique<OGRGeometryCollection>();
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0.0, 0.0);
    ls->addPoint(1.0, 1.0);
    EXPECT_EQ(OGRERR_NONE, coll->addGeometryDirectly(pt.release()));
    EXPECT_EQ(OGRERR_NONE, coll->addGeometryDirectly(ls.release()));
    feat->SetGeometryDirectly(coll.release());
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  // Feature 2: Simple Point(5, 6) (not a collection).
  {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("name", "single");
    auto pt = std::make_unique<OGRPoint>(5.0, 6.0);
    feat->SetGeometryDirectly(pt.release());
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  return ds;
}

TEST_F(GdalalgVectorExplodeCollectionsTest, StandaloneExplodeAndLayerMethods) {
  auto src_ds = CreateMultiGeomCollectionMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  const std::string out_gpkg = std::string(kTestDir) + "/exploded.gpkg";
  GDALVectorExplodeCollectionsAlgorithmStandalone alg;
  EXPECT_EQ("explode-collections", alg.GetName());

  std::vector<GDALArgDatasetValue> inputs;
  inputs.emplace_back(src_ds.get());
  ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
  ASSERT_TRUE(alg.ParseCommandLineArguments({
      "--geometry-type",
      "GEOMETRY",
      "-f",
      "GPKG",
      "--lco",
      "SPATIAL_INDEX=NO",
      "--output",
      out_gpkg,
  }));
  EXPECT_TRUE(alg.Run());
  EXPECT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  OGRLayer* lyr = out_ds->GetLayer(0);
  ASSERT_THAT(lyr, NotNull());
  EXPECT_EQ(3, lyr->GetFeatureCount());
}

TEST_F(GdalalgVectorExplodeCollectionsTest,
       GeometryTypeFilterAndSkipOnTypeMismatch) {
  auto src_ds = CreateMultiGeomCollectionMemDataset();
  ASSERT_THAT(src_ds, NotNull());

  GDALVectorExplodeCollectionsAlgorithmStandalone alg;
  std::vector<GDALArgDatasetValue> inputs;
  inputs.emplace_back(src_ds.get());
  ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
  ASSERT_TRUE(alg.ParseCommandLineArguments({
      "--geometry-type",
      "POINT",
      "--skip-on-type-mismatch",
      "-f",
      "MEM",
      "",
  }));
  EXPECT_TRUE(alg.Run());

  GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  OGRLayer* out_lyr = out_ds->GetLayer(0);
  ASSERT_THAT(out_lyr, NotNull());
  EXPECT_EQ(wkbPoint, out_lyr->GetGeomType());
  // Feature 1's LineString is skipped; Feature 1's Point + Feature 2's Point
  // = 2.
  EXPECT_EQ(2, out_lyr->GetFeatureCount());
}

TEST_F(GdalalgVectorExplodeCollectionsTest,
       MultipleGeomFieldsAndLayerCapabilities) {
  // Create a MEM dataset with two MultiPoint geometry fields.
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds, NotNull());
  OGRLayer* lyr = ds->CreateLayer("two_geoms", nullptr, wkbMultiPoint, nullptr);
  ASSERT_THAT(lyr, NotNull());
  OGRGeomFieldDefn geom2("geom2", wkbMultiPoint);
  ASSERT_EQ(OGRERR_NONE, lyr->CreateGeomField(&geom2));

  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  auto mp1 = std::make_unique<OGRMultiPoint>();
  ASSERT_EQ(OGRERR_NONE, mp1->addGeometryDirectly(
                             std::make_unique<OGRPoint>(1, 1).release()));
  ASSERT_EQ(OGRERR_NONE, mp1->addGeometryDirectly(
                             std::make_unique<OGRPoint>(2, 2).release()));
  auto mp2 = std::make_unique<OGRMultiPoint>();
  ASSERT_EQ(OGRERR_NONE, mp2->addGeometryDirectly(
                             std::make_unique<OGRPoint>(3, 3).release()));
  ASSERT_EQ(OGRERR_NONE, mp2->addGeometryDirectly(
                             std::make_unique<OGRPoint>(4, 4).release()));
  feat->SetGeomFieldDirectly(0, mp1.release());
  feat->SetGeomFieldDirectly(1, mp2.release());
  ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));

  // 1. Explode all geometry fields (Cartesian product 2 x 2 = 4 features)
  //    and exercise CreateAlgLayer directly for layer methods.
  {
    GDALVectorExplodeCollectionsAlgorithm alg(/*standaloneStep=*/false);
    auto out_lyr = alg.CreateAlgLayer(*lyr);
    ASSERT_THAT(out_lyr, NotNull());
    EXPECT_EQ(lyr->TestCapability(OLCStringsAsUTF8),
              out_lyr->TestCapability(OLCStringsAsUTF8));
    EXPECT_FALSE(out_lyr->TestCapability(OLCRandomWrite));
    OGREnvelope env;
    EXPECT_EQ(OGRERR_NONE, out_lyr->GetExtent(&env, TRUE));

    int count = 0;
    while (auto f = std::unique_ptr<OGRFeature>(out_lyr->GetNextFeature())) {
      ++count;
      EXPECT_EQ(count, f->GetFID());
    }
    EXPECT_EQ(4, count);

    out_lyr->ResetReading();
    auto first = std::unique_ptr<OGRFeature>(out_lyr->GetNextFeature());
    ASSERT_THAT(first, NotNull());
    EXPECT_EQ(1, first->GetFID());
  }

  // 2. Select only geom2 by --active-geometry (explodes only geom2 -> 2 feats).
  {
    GDALVectorExplodeCollectionsAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--active-geometry", "geom2", "-f", "MEM", ""}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(2, out_ds->GetLayer(0)->GetFeatureCount());
  }

  // 3. Non-matching --active-geometry leaves features unexploded (1 feature).
  {
    GDALVectorExplodeCollectionsAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--active-geometry", "no_such_field", "-f", "MEM", ""}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(1, out_ds->GetLayer(0)->GetFeatureCount());
  }
}

TEST_F(GdalalgVectorExplodeCollectionsTest,
       PipelineAutoCompleteAndInvalidGeomType) {
  // Pipeline integration.
  const std::string in_json = std::string(kTestDir) + "/mp.geojson";
  const std::string out_json = std::string(kTestDir) + "/out.geojson";
  constexpr char kMultiPointJson[] = R"json({
    "type": "FeatureCollection",
    "features": [{
      "type": "Feature",
      "geometry": { "type": "MultiPoint", "coordinates": [[0, 0], [1, 1]] },
      "properties": {}
    }]
  })json";
  VsiMemTempWrapper in_file(in_json, kMultiPointJson);

  GDALVectorPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments({
      "read",
      in_json,
      "!",
      "explode-collections",
      "!",
      "write",
      out_json,
      "--overwrite",
  }));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_json.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(2, out_ds->GetLayer(0)->GetFeatureCount());

  // Autocomplete for --geometry-type.
  {
    GDALVectorExplodeCollectionsAlgorithmStandalone alg;
    std::vector<std::string> args = {"--geometry-type", "PO"};
    const auto suggestions = alg.GetAutoComplete(args, false, false);
    EXPECT_THAT(suggestions, Contains("POINT"));
    EXPECT_THAT(suggestions, Contains("POINTZ"));
    EXPECT_THAT(suggestions, Contains("POLYGON"));
  }

  // Invalid geometry type error.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorExplodeCollectionsAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--geometry-type", "NOT_A_GEOM_TYPE", in_json, "-f", "MEM", ""}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid geometry type"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
