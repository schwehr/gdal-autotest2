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
// Tests for ogr2ogr CLI binary (apps/ogr2ogr_bin.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/ogr2ogr_bin.cpp
//   https://gdal.org/programs/ogr2ogr.html

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/apps/ogr2ogr_bin_shim.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::autotest2_ogr2ogr_bin::RunOgr2OgrMain;
using ::testing::NotNull;

constexpr std::string_view kGeoJsonPoints = R"json({
  "type": "FeatureCollection",
  "name": "pts",
  "crs": {
    "type": "name",
    "properties": { "name": "urn:ogc:def:crs:OGC:1.3:CRS84" }
  },
  "features": [
    {
      "type": "Feature",
      "properties": { "id": 1, "name": "alpha" },
      "geometry": { "type": "Point", "coordinates": [10.0, 20.0] }
    },
    {
      "type": "Feature",
      "properties": { "id": 2, "name": "beta" },
      "geometry": { "type": "Point", "coordinates": [11.0, 21.0] }
    }
  ]
})json";

void EnsureDriversRegistered() {
  RegisterOGRGeoJSON();
  RegisterOGRGeoPackage();
  RegisterOGRShape();
}

GDALDatasetUniquePtr CreatePointVectorDataset(const std::string& driver_name,
                                              const std::string& path,
                                              const std::string& layer_name) {
  EnsureDriversRegistered();
  GDALDriver* drv =
      GetGDALDriverManager()->GetDriverByName(driver_name.c_str());
  if (!drv) return nullptr;
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  OGRSpatialReference srs;
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  if (srs.importFromEPSG(4326) != OGRERR_NONE) return nullptr;

  OGRLayer* layer =
      ds->CreateLayer(layer_name.c_str(), &srs, wkbPoint, nullptr);
  if (!layer) return nullptr;

  OGRFieldDefn id_field("id", OFTInteger);
  OGRFieldDefn name_field("name", OFTString);
  if (layer->CreateField(&id_field) != OGRERR_NONE ||
      layer->CreateField(&name_field) != OGRERR_NONE) {
    return nullptr;
  }

  for (int i = 1; i <= 2; ++i) {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    feat->SetField("id", i);
    feat->SetField("name", i == 1 ? "alpha" : "beta");
    OGRPoint pt(10.0 + i, 20.0 + i);
    feat->SetGeometry(&pt);
    if (layer->CreateFeature(feat.get()) != OGRERR_NONE) return nullptr;
  }
  return ds;
}

TEST(Ogr2ogrBinTest, HelpFlagExitsZero) {
  EXPECT_EXIT(RunOgr2OgrMain({"ogr2ogr", "--help"}),
              ::testing::ExitedWithCode(0), "");
}

TEST(Ogr2ogrBinTest, GeneralOptionsAndUsageErrors) {
  EXPECT_EQ(0, RunOgr2OgrMain({"ogr2ogr", "--version"}));
  EXPECT_EQ(0, RunOgr2OgrMain({"ogr2ogr", "--help-general"}));
  EXPECT_EQ(0, RunOgr2OgrMain({"ogr2ogr", "--formats"}));

  WithQuietHandler quiet;
  EXPECT_EQ(1, RunOgr2OgrMain({"ogr2ogr", "--format", "NO_SUCH_FORMAT"}));
  // Missing arguments sets bShowUsageIfError = true and invokes Usage().
  EXPECT_EQ(1, RunOgr2OgrMain({"ogr2ogr"}));
  EXPECT_EQ(1, RunOgr2OgrMain({"ogr2ogr", "--unknown-flag"}));
  // Missing @sql file causes OptionsNew to return nullptr with
  // bShowUsageIfError = false.
  EXPECT_EQ(
      1, RunOgr2OgrMain({"ogr2ogr", "-sql", "@/vsimem/ogr2ogr_bin_missing.sql",
                         "/vsimem/out.gpkg", "/vsimem/in.geojson"}));
}

TEST(Ogr2ogrBinTest, MissingInputDatasourceFails) {
  constexpr char kDir[] = "/vsimem/ogr2ogr_bin_missing_test";
  constexpr char kOut[] = "/vsimem/ogr2ogr_bin_missing_test/out.geojson";
  constexpr char kMissing[] =
      "/vsimem/ogr2ogr_bin_missing_test/missing.geojson";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };

  WithQuietHandler quiet;
  EXPECT_EQ(1, RunOgr2OgrMain({"ogr2ogr", "-f", "GeoJSON", kOut, kMissing}));
}

TEST(Ogr2ogrBinTest, BasicTranslationAndVerifyOutput) {
  constexpr char kDir[] = "/vsimem/ogr2ogr_bin_basic_test";
  constexpr char kIn[] = "/vsimem/ogr2ogr_bin_basic_test/in.geojson";
  constexpr char kOutGpkg[] = "/vsimem/ogr2ogr_bin_basic_test/out.gpkg";
  constexpr char kOutJson[] = "/vsimem/ogr2ogr_bin_basic_test/out.geojson";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  VsiMemTempWrapper in_file(kIn, std::string(kGeoJsonPoints));

  // Quiet translation to GeoPackage.
  EXPECT_EQ(0, RunOgr2OgrMain({"ogr2ogr", "-q", "-f", "GPKG", kOutGpkg, kIn}));

  {
    EnsureDriversRegistered();
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(kOutGpkg, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                          nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(1, ds->GetLayerCount());
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_THAT(layer, NotNull());
    EXPECT_EQ(2, layer->GetFeatureCount());
    // GeoJSON promotes "id" to the FID column, which GPKG stores as its FID.
    EXPECT_STREQ("id", layer->GetFIDColumn());
    OGRFeatureUniquePtr feat(layer->GetNextFeature());
    ASSERT_THAT(feat, NotNull());
    EXPECT_EQ(1, feat->GetFID());
    EXPECT_STREQ("alpha", feat->GetFieldAsString("name"));
  }

  // Non-quiet translation exercises GDALTermProgress callback wiring.
  EXPECT_EQ(0, RunOgr2OgrMain({"ogr2ogr", "-f", "GeoJSON", kOutJson, kIn}));
  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(kOutJson, &stat_buf));
}

TEST(Ogr2ogrBinTest, StreamingStdoutOutput) {
  constexpr char kDir[] = "/vsimem/ogr2ogr_bin_stdout_test";
  constexpr char kIn[] = "/vsimem/ogr2ogr_bin_stdout_test/in.geojson";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  VsiMemTempWrapper in_file(kIn, std::string(kGeoJsonPoints));

  EXPECT_EQ(0,
            RunOgr2OgrMain({"ogr2ogr", "-f", "GeoJSON", "/vsistdout/", kIn}));
}

TEST(Ogr2ogrBinTest, SameSourceAndDestAppendAndFormatValidation) {
  constexpr char kDir[] = "/vsimem/ogr2ogr_bin_same_test";
  constexpr char kGpkg[] = "/vsimem/ogr2ogr_bin_same_test/data.gpkg";
  constexpr char kShpDir[] = "/vsimem/ogr2ogr_bin_same_test/shpdir";
  constexpr char kShp[] = "/vsimem/ogr2ogr_bin_same_test/shpdir/pts.shp";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  {
    auto gpkg_ds = CreatePointVectorDataset("GPKG", kGpkg, "pts");
    ASSERT_THAT(gpkg_ds, NotNull());
    VSIMkdir(kShpDir, 0755);
    auto shp_ds = CreatePointVectorDataset("ESRI Shapefile", kShp, "pts");
    ASSERT_THAT(shp_ds, NotNull());
  }

  // 1. Same src==dst with GPKG shares the update handle (bCloseODS = false).
  EXPECT_EQ(0, RunOgr2OgrMain({"ogr2ogr", "-q", "-update", "-nln", "pts2",
                               kGpkg, kGpkg, "pts"}));
  {
    EnsureDriversRegistered();
    auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        kGpkg, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    OGRLayer* layer = ds->GetLayerByName("pts2");
    ASSERT_THAT(layer, NotNull());
    EXPECT_EQ(2, layer->GetFeatureCount());
  }

  // 2. Same src==dst with GPKG and explicit valid -f GPKG passes driver check.
  EXPECT_EQ(0, RunOgr2OgrMain({"ogr2ogr", "-q", "-append", "-f", "GPKG", kGpkg,
                               kGpkg, "-nln", "pts2", "pts"}));
  {
    EnsureDriversRegistered();
    auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        kGpkg, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    OGRLayer* layer = ds->GetLayerByName("pts2");
    ASSERT_THAT(layer, NotNull());
    EXPECT_EQ(4, layer->GetFeatureCount());
  }

  // 3. Same src==dst with invalid -f prints available drivers and fails.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunOgr2OgrMain({"ogr2ogr", "-q", "-update", "-f",
                                 "BOGUS_DRIVER", kGpkg, kGpkg, "pts"}));
  }

  // 4. Same src==dst with ESRI Shapefile opens hDS separately from hODS.
  EXPECT_EQ(0, RunOgr2OgrMain({"ogr2ogr", "-q", "-update", "-nln", "pts2",
                               kShpDir, kShpDir, "pts"}));
  {
    EnsureDriversRegistered();
    auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        kShpDir, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    OGRLayer* layer = ds->GetLayerByName("pts2");
    ASSERT_THAT(layer, NotNull());
    EXPECT_EQ(2, layer->GetFeatureCount());
  }
}

TEST(Ogr2ogrBinTest, UsageErrorAndTranslateFailure) {
  constexpr char kDir[] = "/vsimem/ogr2ogr_bin_err_test";
  constexpr char kIn[] = "/vsimem/ogr2ogr_bin_err_test/in.geojson";
  constexpr char kOut[] = "/vsimem/ogr2ogr_bin_err_test/out.geojson";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  VsiMemTempWrapper in_file(kIn, std::string(kGeoJsonPoints));

  WithQuietHandler quiet;
  // -preserve_fid and -explodecollections together trigger bUsageError = TRUE.
  EXPECT_EQ(
      1, RunOgr2OgrMain({"ogr2ogr", "-q", "-preserve_fid",
                         "-explodecollections", "-f", "GeoJSON", kOut, kIn}));
  // Unknown output driver fails GDALVectorTranslate without bUsageError.
  EXPECT_EQ(
      1, RunOgr2OgrMain({"ogr2ogr", "-q", "-f", "NO_SUCH_DRIVER", kOut, kIn}));
}

}  // namespace
}  // namespace autotest2
