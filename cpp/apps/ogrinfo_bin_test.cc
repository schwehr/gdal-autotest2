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
// Tests for ogrinfo CLI binary (apps/ogrinfo_bin.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/ogrinfo_bin.cpp
//   https://gdal.org/programs/ogrinfo.html

#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/apps/ogrinfo_bin_shim.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::autotest2_ogrinfo_bin::RunOgrInfoMain;
using ::testing::NotNull;

constexpr std::string_view kGeoJsonPoints = R"json({
  "type": "FeatureCollection",
  "name": "pts",
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
  GDALRegister_GTiff();
  RegisterOGRGeoJSON();
  RegisterOGRGeoPackage();
}

GDALDatasetUniquePtr CreatePointGpkg(const std::string& path, bool with_layer) {
  EnsureDriversRegistered();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  if (!drv) return nullptr;
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds || !with_layer) return ds;

  OGRSpatialReference srs;
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  if (srs.importFromEPSG(4326) != OGRERR_NONE) return nullptr;

  OGRLayer* layer = ds->CreateLayer("pts", &srs, wkbPoint, nullptr);
  if (!layer) return nullptr;

  OGRFieldDefn id_field("id", OFTInteger);
  if (layer->CreateField(&id_field) != OGRERR_NONE) return nullptr;

  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  feat->SetField("id", 1);
  OGRPoint pt(10.0, 20.0);
  feat->SetGeometry(&pt);
  if (layer->CreateFeature(feat.get()) != OGRERR_NONE) return nullptr;
  return ds;
}

TEST(OgrinfoBinTest, HelpFlagExitsZero) {
  EXPECT_EXIT(RunOgrInfoMain({"ogrinfo", "--help"}),
              ::testing::ExitedWithCode(0), "");
}

TEST(OgrinfoBinTest, GeneralOptionsAndUsageErrors) {
  EXPECT_EQ(0, RunOgrInfoMain({"ogrinfo", "--version"}));
  EXPECT_EQ(0, RunOgrInfoMain({"ogrinfo", "--help-general"}));
  EXPECT_EQ(0, RunOgrInfoMain({"ogrinfo", "--formats"}));

  WithQuietHandler quiet;
  EXPECT_EQ(1, RunOgrInfoMain({"ogrinfo", "--format", "NO_SUCH_FORMAT"}));
  EXPECT_EQ(1, RunOgrInfoMain({"ogrinfo"}));
  EXPECT_EQ(1, RunOgrInfoMain({"ogrinfo", "--unknown-flag"}));
}

TEST(OgrinfoBinTest, OpenModesAndInfoOutput) {
  constexpr char kDir[] = "/vsimem/ogrinfo_bin_basic_test";
  constexpr char kGeoJson[] = "/vsimem/ogrinfo_bin_basic_test/pts.geojson";
  constexpr char kGpkg[] = "/vsimem/ogrinfo_bin_basic_test/pts.gpkg";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  VsiMemTempWrapper json_file(kGeoJson, std::string(kGeoJsonPoints));
  {
    auto gpkg_ds = CreatePointGpkg(kGpkg, /*with_layer=*/true);
    ASSERT_THAT(gpkg_ds, NotNull());
  }

  EXPECT_EQ(0, RunOgrInfoMain({"ogrinfo", "-so", "-al", kGeoJson}));
  EXPECT_EQ(0, RunOgrInfoMain({"ogrinfo", "-ro", "-al", kGeoJson}));
  EXPECT_EQ(0, RunOgrInfoMain({"ogrinfo", "-json", kGeoJson}));
  EXPECT_EQ(0, RunOgrInfoMain({"ogrinfo", "-update", "-so", "-al", kGpkg}));
  EXPECT_EQ(0, RunOgrInfoMain({"ogrinfo", "-sql", "SELECT * FROM pts", kGpkg}));
}

TEST(OgrinfoBinTest, RetryOpenModesEmptyGpkgAndReadOnlySql) {
  constexpr char kDir[] = "/vsimem/ogrinfo_bin_retry_test";
  constexpr char kEmptyGpkg[] = "/vsimem/ogrinfo_bin_retry_test/empty.gpkg";
  constexpr char kGeoJson[] = "/vsimem/ogrinfo_bin_retry_test/pts.geojson";
  constexpr char kMissing[] = "/vsimem/ogrinfo_bin_retry_test/missing.geojson";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  VsiMemTempWrapper json_file(kGeoJson, std::string(kGeoJsonPoints));
  {
    auto empty_gpkg = CreatePointGpkg(kEmptyGpkg, /*with_layer=*/false);
    ASSERT_THAT(empty_gpkg, NotNull());
  }

  // Empty GeoPackage fails read-only open, then succeeds on update retry.
  EXPECT_EQ(0, RunOgrInfoMain({"ogrinfo", kEmptyGpkg}));
  // With explicit -ro, empty GeoPackage does not retry update mode and fails.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunOgrInfoMain({"ogrinfo", "-ro", kEmptyGpkg}));
  }

  // Read-only GeoJSON with -sql fails update open, then retries read-only.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        0, RunOgrInfoMain({"ogrinfo", "-sql", "SELECT * FROM pts", kGeoJson}));
    EXPECT_EQ(0, RunOgrInfoMain(
                     {"ogrinfo", "-q", "-sql", "SELECT * FROM pts", kGeoJson}));
    EXPECT_EQ(1, RunOgrInfoMain({"ogrinfo", "-sql", "SELECT 1", kMissing}));
  }
}

TEST(OgrinfoBinTest, OpenFailuresAndRasterHint) {
  constexpr char kDir[] = "/vsimem/ogrinfo_bin_err_test";
  constexpr char kMissing[] = "/vsimem/ogrinfo_bin_err_test/missing.geojson";
  constexpr char kGarbage[] = "/vsimem/ogrinfo_bin_err_test/garbage.bin";
  // ogrinfo_bin.cpp calls VSIStat() (POSIX stat(), not VSIStatL()) before
  // printing the gdalinfo hint, so place raster.tif in ::testing::TempDir().
  const std::filesystem::path tif_path =
      std::filesystem::path(::testing::TempDir()) / "ogrinfo_bin_raster.tif";
  const std::string tif_str = tif_path.string();
  absl::Cleanup clean_dir = [kDir, tif_path] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
    std::error_code ec;
    std::filesystem::remove(tif_path, ec);
  };
  VsiMemTempWrapper garbage_file(kGarbage, "not a vector or raster file");
  {
    EnsureDriversRegistered();
    GDALDriver* tif_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_THAT(tif_drv, NotNull());
    GDALDatasetUniquePtr tif_ds(
        tif_drv->Create(tif_str.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_THAT(tif_ds, NotNull());
  }

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(1, RunOgrInfoMain({"ogrinfo", kMissing}));
  // Existing raster triggers "Did you intend to call gdalinfo?" diagnostic.
  testing::internal::CaptureStderr();
  EXPECT_EQ(1, RunOgrInfoMain({"ogrinfo", tif_str}));
  EXPECT_THAT(testing::internal::GetCapturedStderr(),
              ::testing::HasSubstr("Did you intend to call gdalinfo?"));
  // Existing non-vector, non-raster file skips raster hint.
  EXPECT_EQ(1, RunOgrInfoMain({"ogrinfo", kGarbage}));
  CPLErrorReset();
}

TEST(OgrinfoBinTest, VectorInfoFailureReturnsOne) {
  constexpr char kDir[] = "/vsimem/ogrinfo_bin_info_err_test";
  constexpr char kGeoJson[] = "/vsimem/ogrinfo_bin_info_err_test/pts.geojson";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  VsiMemTempWrapper json_file(kGeoJson, std::string(kGeoJsonPoints));

  WithQuietHandler quiet;
  // Requesting a nonexistent layer causes GDALVectorInfo() to return nullptr.
  EXPECT_EQ(1, RunOgrInfoMain({"ogrinfo", kGeoJson, "no_such_layer"}));
}

}  // namespace
}  // namespace autotest2
