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
// Tests for GDALRasterZonalStatsAlgorithm
// (apps/gdalalg_raster_zonal_stats.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_zonal_stats.cpp

#include "third_party/gdal/apps/gdalalg_raster_zonal_stats.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
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

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_zonal_stats_test";

class GdalalgRasterZonalStatsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateValueTiff(const std::string& path, const std::vector<float>& vals) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Float32, nullptr));
  ASSERT_THAT(ds, NotNull());
  // Extent [0, 4] x [0, 4], 1m pixel size.
  double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  OGRSpatialReference srs;
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(32632));
  ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs));
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 4, 4, const_cast<float*>(vals.data()),
                         4, 4, GDT_Float32, 0, 0, nullptr));
}

void CreatePolygonZonesGpkg(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds, NotNull());
  OGRSpatialReference srs;
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(32632));

  OGRLayer* lyr = ds->CreateLayer("zones_lyr", &srs, wkbPolygon, nullptr);
  ASSERT_THAT(lyr, NotNull());
  OGRFieldDefn id_field("zone_id", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&id_field));

  auto add_poly = [lyr](int zone_id, double xmin, double ymin, double xmax,
                        double ymax) {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("zone_id", zone_id);
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(xmin, ymin);
    ring->addPoint(xmax, ymin);
    ring->addPoint(xmax, ymax);
    ring->addPoint(xmin, ymax);
    ring->addPoint(xmin, ymin);
    auto poly = std::make_unique<OGRPolygon>();
    poly->addRingDirectly(ring.release());
    feat->SetGeometryDirectly(poly.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  };
  ASSERT_NO_FATAL_FAILURE(add_poly(1, 0.0, 2.0, 2.0, 4.0));
  ASSERT_NO_FATAL_FAILURE(add_poly(2, 2.0, 0.0, 4.0, 2.0));
}

void CreateZoneIdTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Int32, nullptr));
  ASSERT_THAT(ds, NotNull());
  double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  OGRSpatialReference srs;
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(32632));
  ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs));
  std::vector<int32_t> zones = {
      1, 1, 2, 2, 1, 1, 2, 2, 3, 3, 4, 4, 3, 3, 4, 4,
  };
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, zones.data(),
                                           4, 4, GDT_Int32, 0, 0, nullptr));
}

TEST_F(GdalalgRasterZonalStatsTest, PolygonZonesAllOptionsAndBothStrategies) {
  const std::string src_tif = std::string(kTestDir) + "/vals.tif";
  const std::string zones_gpkg = std::string(kTestDir) + "/zones.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out_poly.gpkg";
  const std::vector<float> vals = {
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
  };
  ASSERT_NO_FATAL_FAILURE(CreateValueTiff(src_tif, vals));
  ASSERT_NO_FATAL_FAILURE(CreatePolygonZonesGpkg(zones_gpkg));

  GDALRasterZonalStatsAlgorithmStandalone alg;
  EXPECT_EQ("zonal-stats", alg.GetName());
  EXPECT_EQ(GDAL_OF_RASTER, alg.GetInputType());
  EXPECT_EQ(GDAL_OF_VECTOR, alg.GetOutputType());

  ASSERT_TRUE(alg.ParseCommandLineArguments({
      "--band",          "1",
      "--zones",         zones_gpkg,
      "--zones-layer",   "zones_lyr",
      "--include-field", "zone_id",
      "--pixels",        "fractional",
      "--stat",          "count,sum,mean",
      "--strategy",      "raster",
      "--chunk-size",    "1MB",
      "--lco",           "SPATIAL_INDEX=NO",
      "--overwrite",     src_tif,
      out_gpkg,
  }));
  int progress_calls = 0;
  auto progress_cb = [](double, const char*, void* data) -> int {
    ++(*static_cast<int*>(data));
    return 1;
  };
  EXPECT_TRUE(alg.Run(progress_cb, &progress_calls));
  EXPECT_GT(progress_calls, 0);
  EXPECT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  OGRLayer* lyr = out_ds->GetLayer(0);
  ASSERT_THAT(lyr, NotNull());
  EXPECT_EQ(2, lyr->GetFeatureCount());
  auto feat = std::unique_ptr<OGRFeature>(lyr->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  EXPECT_EQ(1, feat->GetFieldAsInteger("zone_id"));
  // Top-left 2x2 pixels are 1, 2, 5, 6 -> sum = 14, mean = 3.5.
  EXPECT_DOUBLE_EQ(14.0, feat->GetFieldAsDouble("sum"));
  EXPECT_DOUBLE_EQ(3.5, feat->GetFieldAsDouble("mean"));
}

TEST_F(GdalalgRasterZonalStatsTest, RasterZonesWithWeightsAndNonStandalone) {
  const std::string src_tif = std::string(kTestDir) + "/src_w.tif";
  const std::string weights_tif = std::string(kTestDir) + "/weights.tif";
  const std::string zones_tif = std::string(kTestDir) + "/zones.tif";
  const std::vector<float> vals(16, 10.0f);
  const std::vector<float> weights(16, 2.0f);
  ASSERT_NO_FATAL_FAILURE(CreateValueTiff(src_tif, vals));
  ASSERT_NO_FATAL_FAILURE(CreateValueTiff(weights_tif, weights));
  ASSERT_NO_FATAL_FAILURE(CreateZoneIdTiff(zones_tif));

  std::unique_ptr<GDALDataset> src_ds(
      GDALDataset::Open(src_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(src_ds, NotNull());

  GDALRasterZonalStatsAlgorithm alg(/*bStandalone=*/false);
  alg.GetInputDatasets().emplace_back(src_ds.get());
  ASSERT_TRUE(alg.ParseCommandLineArguments({
      "--zones",
      zones_tif,
      "--zones-band",
      "1",
      "--weights",
      weights_tif,
      "--weights-band",
      "1",
      "--stat",
      "weighted_mean,weighted_sum",
  }));
  EXPECT_TRUE(alg.Run());
  GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  OGRLayer* lyr = out_ds->GetLayer(0);
  ASSERT_THAT(lyr, NotNull());
  EXPECT_EQ(4, lyr->GetFeatureCount());
}

TEST_F(GdalalgRasterZonalStatsTest, ErrorPaths) {
  const std::string src_tif = std::string(kTestDir) + "/err_src.tif";
  const std::string zones_tif = std::string(kTestDir) + "/err_zones.tif";
  ASSERT_NO_FATAL_FAILURE(
      CreateValueTiff(src_tif, std::vector<float>(16, 1.0f)));
  ASSERT_NO_FATAL_FAILURE(CreateZoneIdTiff(zones_tif));

  // 1. Unknown output extension without -f fails driver guessing.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterZonalStatsAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--zones",
        zones_tif,
        "--zones-band",
        "1",
        "--stat",
        "mean",
        src_tif,
        std::string(kTestDir) + "/out.unknownext",
    }));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot guess driver"));
  }

  // 2. Mutual exclusion between zones-band and zones-layer.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterZonalStatsAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({
        "--zones",
        zones_tif,
        "--zones-band",
        "1",
        "--zones-layer",
        "lyr",
        "--stat",
        "mean",
        src_tif,
        std::string(kTestDir) + "/unused.gpkg",
    }));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("mutually exclusive"));
  }

  // 3. Chunk-size without unit fails validation.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterZonalStatsAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({
        "--zones",
        zones_tif,
        "--stat",
        "mean",
        "--chunk-size",
        "1024",
        src_tif,
        std::string(kTestDir) + "/unused2.gpkg",
    }));
  }
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
