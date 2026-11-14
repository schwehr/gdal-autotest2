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
// Tests for GDALRasterFootprintAlgorithm (apps/gdalalg_raster_footprint.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_footprint.cpp

#include "third_party/gdal/apps/gdalalg_raster_footprint.h"

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
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_footprint_test";

class GdalalgRasterFootprintTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CPLSetConfigOption("CPL_TMPDIR", "/vsimem/gdalalg_raster_footprint_test");
    GDALRegister_MEM();
    GDALRegister_GTiff();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
  }
};

void CreateTwoBandTestTiff(const std::string& path, bool build_overview) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 8, 8, 2, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());

  double gt[6] = {10.0, 0.1, 0.0, 20.0, 0.0, -0.1};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  OGRSpatialReference srs;
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs));

  // Band 1: non-zero in top-left 4x4, 0 elsewhere.
  std::vector<uint8_t> b1(64, 0);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      b1[y * 8 + x] = 100;
    }
  }
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 8, 8, b1.data(), 8,
                                           8, GDT_Byte, 0, 0, nullptr));

  // Band 2: non-zero in 4x4 shifted right by 2 pixels, 255 elsewhere.
  std::vector<uint8_t> b2(64, 255);
  for (int y = 0; y < 4; ++y) {
    for (int x = 2; x < 6; ++x) {
      b2[y * 8 + x] = 50;
    }
  }
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 8, 8, b2.data(), 8,
                                           8, GDT_Byte, 0, 0, nullptr));

  if (build_overview) {
    // GDALFootprint checks mask band overviews when --overview is specified;
    // create and populate a per-dataset mask band before building overviews.
    ASSERT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
    GDALRasterBand* mask_band = ds->GetRasterBand(1)->GetMaskBand();
    ASSERT_THAT(mask_band, NotNull());
    std::vector<uint8_t> mask(64, 0);
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        mask[y * 8 + x] = 255;
      }
    }
    ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, 8, 8, mask.data(), 8,
                                           8, GDT_Byte, 0, 0, nullptr));
    int ovr_list[1] = {2};
    ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, ovr_list, 0, nullptr,
                                          nullptr, nullptr, nullptr));
  }
}

TEST_F(GdalalgRasterFootprintTest, StandaloneMetadataAndDefaults) {
  GDALRasterFootprintAlgorithmStandalone alg;
  EXPECT_EQ("footprint", alg.GetName());
  EXPECT_FALSE(alg.IsNativelyStreamingCompatible());
  EXPECT_EQ(GDAL_OF_RASTER, alg.GetInputType());
  EXPECT_EQ(GDAL_OF_VECTOR, alg.GetOutputType());
  EXPECT_THAT(alg.GetUsageForCLI(false), HasSubstr("footprint"));
  EXPECT_THAT(alg.GetUsageAsJSON(), HasSubstr("combine-bands"));
}

TEST_F(GdalalgRasterFootprintTest, StandaloneBasicAndAllOptions) {
  const std::string in_tif = std::string(kTestDir) + "/input.tif";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  ASSERT_NO_FATAL_FAILURE(
      CreateTwoBandTestTiff(in_tif, /*build_overview=*/false));

  GDALRasterFootprintAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments({
      "--output-format",
      "GPKG",
      "--creation-option",
      "ADD_GPKG_OGR_CONTENTS=YES",
      "--layer-creation-option",
      "SPATIAL_INDEX=NO",
      "--output-layer",
      "custom_fp",
      "--band",
      "1,2",
      "--combine-bands",
      "intersection",
      "--src-nodata",
      "0,255",
      "--coordinate-system",
      "georeferenced",
      "--dst-crs",
      "EPSG:3857",
      "--split-multipolygons",
      "--convex-hull",
      "--densify-distance",
      "100.0",
      "--simplify-tolerance",
      "1.0",
      "--min-ring-area",
      "0.0",
      "--max-points",
      "unlimited",
      "--location-field",
      "src_path",
      "--absolute-path",
      in_tif,
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
  OGRLayer* lyr = out_ds->GetLayerByName("custom_fp");
  ASSERT_THAT(lyr, NotNull());
  EXPECT_GE(lyr->GetFeatureCount(), 1);
  auto feat = std::unique_ptr<OGRFeature>(lyr->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  EXPECT_GE(feat->GetFieldIndex("src_path"), 0);
  EXPECT_THAT(feat->GetFieldAsString("src_path"), HasSubstr("input.tif"));
  ASSERT_THAT(feat->GetGeometryRef(), NotNull());
  EXPECT_FALSE(feat->GetGeometryRef()->IsEmpty());
}

TEST_F(GdalalgRasterFootprintTest, PixelCoordinateSystemNoLocationAndAppend) {
  const std::string in_tif = std::string(kTestDir) + "/ovr.tif";
  const std::string out_gpkg = std::string(kTestDir) + "/pixel.gpkg";
  ASSERT_NO_FATAL_FAILURE(
      CreateTwoBandTestTiff(in_tif, /*build_overview=*/true));

  {
    GDALRasterFootprintAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "-f",
        "GPKG",
        "--overview",
        "0",
        "--coordinate-system",
        "pixel",
        "--max-points",
        "8",
        "--no-location-field",
        in_tif,
        out_gpkg,
    }));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // Append a second footprint to the existing GeoPackage.
  {
    GDALRasterFootprintAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--append",
        "--coordinate-system",
        "pixel",
        "--no-location-field",
        in_tif,
        out_gpkg,
    }));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  OGRLayer* lyr = out_ds->GetLayer(0);
  ASSERT_THAT(lyr, NotNull());
  EXPECT_EQ(2, lyr->GetFeatureCount());
  EXPECT_EQ(-1, lyr->GetLayerDefn()->GetFieldIndex("location"));
}

TEST_F(GdalalgRasterFootprintTest, NonStandaloneInMemoryExecution) {
  const std::string in_tif = std::string(kTestDir) + "/non_standalone.tif";
  ASSERT_NO_FATAL_FAILURE(
      CreateTwoBandTestTiff(in_tif, /*build_overview=*/false));
  std::unique_ptr<GDALDataset> src_ds(
      GDALDataset::Open(in_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(src_ds, NotNull());

  GDALRasterFootprintAlgorithm alg(/*standaloneStep=*/false);
  alg.GetInputDatasets().emplace_back(src_ds.get());
  ASSERT_TRUE(alg.ParseCommandLineArguments({"--combine-bands", "union"}));
  EXPECT_TRUE(alg.Run());
  GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(1, out_ds->GetLayerCount());
  OGRLayer* lyr = out_ds->GetLayer(0);
  ASSERT_THAT(lyr, NotNull());
  EXPECT_EQ(1, lyr->GetFeatureCount());
}

TEST_F(GdalalgRasterFootprintTest, ValidationErrors) {
  const std::string no_ovr_tif = std::string(kTestDir) + "/no_ovr.tif";
  const std::string has_ovr_tif = std::string(kTestDir) + "/has_ovr.tif";
  const std::string out_json = std::string(kTestDir) + "/unused.geojson";
  ASSERT_NO_FATAL_FAILURE(CreateTwoBandTestTiff(no_ovr_tif, false));
  ASSERT_NO_FATAL_FAILURE(CreateTwoBandTestTiff(has_ovr_tif, true));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterFootprintAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--max-points", "3", no_ovr_tif, out_json}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("max-points"));
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterFootprintAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--max-points", "not_a_num", no_ovr_tif, out_json}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("max-points"));
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterFootprintAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--overview", "0", no_ovr_tif, out_json}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("no overviews"));
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterFootprintAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--overview", "5", has_ovr_tif, out_json}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("overview levels"));
  }
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
