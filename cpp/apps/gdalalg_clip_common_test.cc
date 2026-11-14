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
// Tests for GDALClipCommon (apps/gdalalg_clip_common.cpp and
// apps/gdalalg_clip_common.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_clip_common.cpp

#include "third_party/gdal/apps/gdalalg_clip_common.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
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
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_clip_common_test";

class TestableClipCommon final : public GDALClipCommon {
 public:
  void SetBbox(std::vector<double> bbox) { m_bbox = std::move(bbox); }
  void SetBboxCrs(std::string crs) { m_bboxCrs = std::move(crs); }
  void SetGeometry(std::string geom) { m_geometry = std::move(geom); }
  void SetGeometryCrs(std::string crs) { m_geometryCrs = std::move(crs); }
  void SetLikeDataset(GDALDataset* ds) { m_likeDataset.Set(ds); }
  void SetLikeLayer(std::string layer) { m_likeLayer = std::move(layer); }
  void SetLikeSQL(std::string sql) { m_likeSQL = std::move(sql); }
  void SetLikeWhere(std::string where) { m_likeWhere = std::move(where); }

  std::pair<std::unique_ptr<OGRGeometry>, std::string> CallGetClipGeometry() {
    return GetClipGeometry();
  }
};

std::unique_ptr<GDALDataset> CreateMemVectorDataset() {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  return std::unique_ptr<GDALDataset>(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
}

void AddPolygonFeature(OGRLayer* layer, int id, const char* wkt) {
  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat->SetField("id", id);
  auto [geom, err] = OGRGeometryFactory::createFromWkt(wkt);
  ASSERT_EQ(OGRERR_NONE, err);
  ASSERT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(geom.release()));
  ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
}

class GdalalgClipCommonTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgClipCommonTest, MissingAllInputsReturnsError) {
  TestableClipCommon clip;
  auto [geom, err] = clip.CallGetClipGeometry();
  EXPECT_EQ(nullptr, geom);
  EXPECT_EQ("--bbox, --geometry or --like must be specified", err);
}

TEST_F(GdalalgClipCommonTest, BboxWithAndWithoutCrs) {
  {
    TestableClipCommon clip;
    clip.SetBbox({1.0, 2.0, 3.0, 4.0});
    auto [geom, err] = clip.CallGetClipGeometry();
    ASSERT_NE(nullptr, geom);
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(nullptr, geom->getSpatialReference());
    OGREnvelope env;
    geom->getEnvelope(&env);
    EXPECT_DOUBLE_EQ(1.0, env.MinX);
    EXPECT_DOUBLE_EQ(2.0, env.MinY);
    EXPECT_DOUBLE_EQ(3.0, env.MaxX);
    EXPECT_DOUBLE_EQ(4.0, env.MaxY);
  }

  {
    TestableClipCommon clip;
    clip.SetBbox({-10.0, -5.0, 10.0, 5.0});
    clip.SetBboxCrs("EPSG:4326");
    auto [geom, err] = clip.CallGetClipGeometry();
    ASSERT_NE(nullptr, geom);
    EXPECT_TRUE(err.empty());
    const OGRSpatialReference* srs = geom->getSpatialReference();
    ASSERT_NE(nullptr, srs);
    EXPECT_STREQ("4326", srs->GetAuthorityCode(nullptr));
  }
}

TEST_F(GdalalgClipCommonTest, GeometryWktGeoJsonAndInvalid) {
  // Valid WKT without CRS.
  {
    TestableClipCommon clip;
    clip.SetGeometry("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
    auto [geom, err] = clip.CallGetClipGeometry();
    ASSERT_NE(nullptr, geom);
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(nullptr, geom->getSpatialReference());
  }

  // Valid GeoJSON without explicit CRS defaults to WGS84.
  {
    TestableClipCommon clip;
    clip.SetGeometry(
        R"({"type":"Polygon","coordinates":[[[0,0],[1,0],[1,1],[0,1],[0,0]]]})");
    auto [geom, err] = clip.CallGetClipGeometry();
    ASSERT_NE(nullptr, geom);
    EXPECT_TRUE(err.empty());
    ASSERT_NE(nullptr, geom->getSpatialReference());
  }

  // Explicit geometry CRS overrides SRS.
  {
    TestableClipCommon clip;
    clip.SetGeometry("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
    clip.SetGeometryCrs("EPSG:32632");
    auto [geom, err] = clip.CallGetClipGeometry();
    ASSERT_NE(nullptr, geom);
    EXPECT_TRUE(err.empty());
    ASSERT_NE(nullptr, geom->getSpatialReference());
    EXPECT_STREQ("32632",
                 geom->getSpatialReference()->GetAuthorityCode(nullptr));
  }

  // Invalid geometry string.
  {
    TestableClipCommon clip;
    clip.SetGeometry("not a valid geometry");
    auto [geom, err] = clip.CallGetClipGeometry();
    EXPECT_EQ(nullptr, geom);
    EXPECT_THAT(err, HasSubstr("neither a valid WKT or GeoJSON geometry"));
  }
}

TEST_F(GdalalgClipCommonTest, LikeVectorDatasetBranches) {
  auto ds = CreateMemVectorDataset();
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));

  OGRLayer* lyr1 = ds->CreateLayer("lyr1", &srs, wkbUnknown, nullptr);
  ASSERT_NE(nullptr, lyr1);
  OGRFieldDefn id_field("id", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, lyr1->CreateField(&id_field));
  ASSERT_NO_FATAL_FAILURE(
      AddPolygonFeature(lyr1, 1, "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))"));
  ASSERT_NO_FATAL_FAILURE(
      AddPolygonFeature(lyr1, 2, "POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))"));

  // Single-layer check: create a second layer to trigger multi-layer error.
  OGRLayer* lyr2 = ds->CreateLayer("lyr2", &srs, wkbUnknown, nullptr);
  ASSERT_NE(nullptr, lyr2);
  ASSERT_EQ(OGRERR_NONE, lyr2->CreateField(&id_field));
  ASSERT_NO_FATAL_FAILURE(AddPolygonFeature(lyr2, 10, "POINT (5 5)"));

  // Multi-layer without --like-layer or --like-sql fails.
  {
    TestableClipCommon clip;
    clip.SetLikeDataset(ds.get());
    auto [geom, err] = clip.CallGetClipGeometry();
    EXPECT_EQ(nullptr, geom);
    EXPECT_THAT(err, HasSubstr("Only single layer dataset"));
  }

  // Nonexistent --like-layer fails.
  {
    TestableClipCommon clip;
    clip.SetLikeDataset(ds.get());
    clip.SetLikeLayer("missing_layer");
    auto [geom, err] = clip.CallGetClipGeometry();
    EXPECT_EQ(nullptr, geom);
    EXPECT_THAT(err, HasSubstr("Failed to identify source layer"));
  }

  // Valid --like-layer + --like-where filters to 1 polygon.
  {
    TestableClipCommon clip;
    clip.SetLikeDataset(ds.get());
    clip.SetLikeLayer("lyr1");
    clip.SetLikeWhere("id = 1");
    auto [geom, err] = clip.CallGetClipGeometry();
    ASSERT_NE(nullptr, geom);
    EXPECT_TRUE(err.empty());
    OGREnvelope env;
    geom->getEnvelope(&env);
    EXPECT_DOUBLE_EQ(0.0, env.MinX);
    EXPECT_DOUBLE_EQ(2.0, env.MaxX);
  }

  // Valid --like-sql unions both polygons.
  {
    TestableClipCommon clip;
    clip.SetLikeDataset(ds.get());
    clip.SetLikeSQL("SELECT * FROM lyr1");
    auto [geom, err] = clip.CallGetClipGeometry();
    ASSERT_NE(nullptr, geom);
    EXPECT_TRUE(err.empty());
    OGREnvelope env;
    geom->getEnvelope(&env);
    EXPECT_DOUBLE_EQ(0.0, env.MinX);
    EXPECT_DOUBLE_EQ(3.0, env.MaxX);
  }

  // Layer with only non-polygonal geometry emits warning and returns error.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    TestableClipCommon clip;
    clip.SetLikeDataset(ds.get());
    clip.SetLikeLayer("lyr2");
    auto [geom, err] = clip.CallGetClipGeometry();
    EXPECT_EQ(nullptr, geom);
    EXPECT_EQ("No clipping geometry found", err);
    CPLErrorReset();
  }

  // Layer with invalid self-intersecting polygon returns invalid geometry
  // error.
  OGRLayer* lyr_invalid =
      ds->CreateLayer("lyr_invalid", &srs, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, lyr_invalid);
  ASSERT_EQ(OGRERR_NONE, lyr_invalid->CreateField(&id_field));
  ASSERT_NO_FATAL_FAILURE(
      AddPolygonFeature(lyr_invalid, 1, "POLYGON ((0 0, 2 2, 2 0, 0 2, 0 0))"));
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    TestableClipCommon clip;
    clip.SetLikeDataset(ds.get());
    clip.SetLikeLayer("lyr_invalid");
    auto [geom, err] = clip.CallGetClipGeometry();
    EXPECT_EQ(nullptr, geom);
    EXPECT_THAT(err, HasSubstr("is invalid"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgClipCommonTest, LikeRasterAndEmptyDatasets) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  // Raster without geotransform fails.
  {
    std::unique_ptr<GDALDataset> raster_no_gt(
        mem_drv->Create("no_gt", 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, raster_no_gt);
    TestableClipCommon clip;
    clip.SetLikeDataset(raster_no_gt.get());
    auto [geom, err] = clip.CallGetClipGeometry();
    EXPECT_EQ(nullptr, geom);
    EXPECT_THAT(err, HasSubstr("has no geotransform matrix"));
  }

  // Raster with geotransform and SRS returns extent polygon.
  {
    std::unique_ptr<GDALDataset> raster_gt(
        mem_drv->Create("with_gt", 10, 20, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, raster_gt);
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(3857));
    raster_gt->SetSpatialRef(&srs);
    double gt[6] = {100.0, 2.0, 0.0, 200.0, 0.0, -3.0};
    ASSERT_EQ(CE_None, raster_gt->SetGeoTransform(gt));

    TestableClipCommon clip;
    clip.SetLikeDataset(raster_gt.get());
    auto [geom, err] = clip.CallGetClipGeometry();
    ASSERT_NE(nullptr, geom);
    EXPECT_TRUE(err.empty());
    ASSERT_NE(nullptr, geom->getSpatialReference());
    EXPECT_STREQ("3857",
                 geom->getSpatialReference()->GetAuthorityCode(nullptr));
    OGREnvelope env;
    geom->getEnvelope(&env);
    EXPECT_DOUBLE_EQ(100.0, env.MinX);
    EXPECT_DOUBLE_EQ(140.0, env.MinY);
    EXPECT_DOUBLE_EQ(120.0, env.MaxX);
    EXPECT_DOUBLE_EQ(200.0, env.MaxY);
  }

  // Empty dataset (0 layers, 0 bands) returns error.
  {
    auto empty_ds = CreateMemVectorDataset();
    ASSERT_NE(nullptr, empty_ds);
    TestableClipCommon clip;
    clip.SetLikeDataset(empty_ds.get());
    auto [geom, err] = clip.CallGetClipGeometry();
    EXPECT_EQ(nullptr, geom);
    EXPECT_EQ("Cannot get extent from clip dataset", err);
  }
}

}  // namespace
}  // namespace autotest2
