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
// Tests for gdalsrsinfo (apps/gdalsrsinfo.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalsrsinfo.cpp

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/apps/gdalsrsinfo_shim.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::autotest2_gdalsrsinfo::FindSRS;
using ::autotest2_gdalsrsinfo::PrintSRS;
using ::autotest2_gdalsrsinfo::PrintSRSOutputTypes;
using ::autotest2_gdalsrsinfo::RunGdalSrsInfoMain;
using ::testing::HasSubstr;
using ::testing::NotNull;

void EnsureDriversRegistered() {
  GDALRegister_MEM();
  GDALRegister_GTiff();
  RegisterOGRGeoJSON();
}

std::unique_ptr<GDALDataset> CreateGeoTiffWithSrs(const std::string& path,
                                                  int epsg_code) {
  EnsureDriversRegistered();
  auto* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create(path.c_str(), 10, 10, 1, GDT_Byte, nullptr));
  if (!ds) return nullptr;
  OGRSpatialReference srs;
  srs.importFromEPSG(epsg_code);
  ds->SetSpatialRef(&srs);
  ds->FlushCache(false);
  return ds;
}

TEST(GdalsrsinfoTest, UsageAndHelpFlags) {
  EXPECT_EQ(0, RunGdalSrsInfoMain({"gdalsrsinfo", "--help"}));
  EXPECT_EQ(0, RunGdalSrsInfoMain({"gdalsrsinfo", "-h"}));
  EXPECT_EQ(0, RunGdalSrsInfoMain({"gdalsrsinfo", "--utility_version"}));

  {
    WithQuietHandler quiet;
    // Missing input
    EXPECT_EQ(1, RunGdalSrsInfoMain({"gdalsrsinfo"}));
    // Unknown option
    EXPECT_EQ(1, RunGdalSrsInfoMain({"gdalsrsinfo", "-unknown_flag"}));
    // -o missing its argument
    EXPECT_EQ(1, RunGdalSrsInfoMain({"gdalsrsinfo", "-o"}));
    // Invalid SRS
    EXPECT_EQ(
        1, RunGdalSrsInfoMain({"gdalsrsinfo", "NOT_A_VALID_SRS_NAME_12345"}));
  }
}

TEST(GdalsrsinfoTest, OutputFormats) {
  constexpr auto kOutputTypes = std::to_array<std::string_view>({
      "default",
      "all",
      "wkt_all",
      "epsg",
      "proj4",
      "PROJJSON",
      "wkt1",
      "wkt_simple",
      "wkt_noct",
      "wkt_esri",
      "wkt2_2015",
      "wkt2",
      "wkt2_2018",
      "wkt2_2019",
      "mapinfo",
      "xml",
  });

  for (const auto& out_type : kOutputTypes) {
    EXPECT_EQ(0, RunGdalSrsInfoMain({"gdalsrsinfo", "-o", std::string(out_type),
                                     "EPSG:4326"}));
  }

  // -p with epsg and with other output types
  EXPECT_EQ(
      0, RunGdalSrsInfoMain({"gdalsrsinfo", "-p", "-o", "epsg", "EPSG:4326"}));
  EXPECT_EQ(
      0, RunGdalSrsInfoMain({"gdalsrsinfo", "-p", "-o", "proj4", "EPSG:4326"}));

  // Unsupported output type reports error but main exits 0 after printing
  {
    WithQuietHandler quiet;
    EXPECT_EQ(0, RunGdalSrsInfoMain(
                     {"gdalsrsinfo", "-o", "bogus_type", "EPSG:4326"}));
  }
}

TEST(GdalsrsinfoTest, ValidationAndSearchEpsg) {
  // Valid SRS
  EXPECT_EQ(0, RunGdalSrsInfoMain({"gdalsrsinfo", "-V", "EPSG:4326"}));
  // Validation failure with non-standard projection
  const std::string bad_proj_wkt =
      "PROJCS[\"unnamed\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
      "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM["
      "\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
      "PROJECTION[\"NonExistentProjection\"]]";
  EXPECT_EQ(0, RunGdalSrsInfoMain({"gdalsrsinfo", "-V", bad_proj_wkt}));
  // Find EPSG with default output
  EXPECT_EQ(0, RunGdalSrsInfoMain({"gdalsrsinfo", "-e", "EPSG:4326"}));
  // Find EPSG with -o all
  EXPECT_EQ(
      0, RunGdalSrsInfoMain({"gdalsrsinfo", "-e", "-o", "all", "EPSG:4326"}));
  // Pretty and single-line options
  EXPECT_EQ(0, RunGdalSrsInfoMain({"gdalsrsinfo", "-p", "EPSG:4326"}));
  EXPECT_EQ(0,
            RunGdalSrsInfoMain({"gdalsrsinfo", "--single-line", "EPSG:4326"}));
}

TEST(GdalsrsinfoTest, InputFromDatasetsAndPrjFiles) {
  constexpr char kDir[] = "/vsimem/gdalsrsinfo_test";
  constexpr char kRasterPath[] = "/vsimem/gdalsrsinfo_test/raster.tif";
  constexpr char kPrjPath[] = "/vsimem/gdalsrsinfo_test/test.prj";
  constexpr char kEsriPrjPath[] = "/vsimem/gdalsrsinfo_test/esri.prj";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };

  ASSERT_THAT(CreateGeoTiffWithSrs(kRasterPath, 32631), NotNull());

  // Raster dataset input
  EXPECT_EQ(0, RunGdalSrsInfoMain({"gdalsrsinfo", "-o", "proj4", kRasterPath}));

  // Standard .prj file
  OGRSpatialReference srs_wgs84;
  srs_wgs84.importFromEPSG(4326);
  char* wkt = nullptr;
  srs_wgs84.exportToWkt(&wkt);
  ASSERT_THAT(wkt, NotNull());
  VSIFCloseL(VSIFileFromMemBuffer(kPrjPath, reinterpret_cast<GByte*>(wkt),
                                  strlen(wkt), TRUE));

  EXPECT_EQ(0, RunGdalSrsInfoMain({"gdalsrsinfo", "-o", "wkt1", kPrjPath}));

  // ESRI .prj file and ESRI:: prefix
  char* esri_wkt = nullptr;
  const char* const esri_options[] = {"FORMAT=WKT1_ESRI", nullptr};
  srs_wgs84.exportToWkt(&esri_wkt, esri_options);
  ASSERT_THAT(esri_wkt, NotNull());
  VSIFCloseL(VSIFileFromMemBuffer(kEsriPrjPath,
                                  reinterpret_cast<GByte*>(esri_wkt),
                                  strlen(esri_wkt), TRUE));

  std::string esri_prefix_path = std::string("ESRI::") + kEsriPrjPath;
  EXPECT_EQ(0, RunGdalSrsInfoMain(
                   {"gdalsrsinfo", "-o", "wkt_esri", esri_prefix_path}));

  // Vector dataset input (triggers layer SRS branch in FindSRS)
  auto* geojson_drv = GetGDALDriverManager()->GetDriverByName("GeoJSON");
  ASSERT_THAT(geojson_drv, NotNull());
  constexpr char kGeoJsonInput[] = "/vsimem/gdalsrsinfo_test/vector.geojson";
  auto ds = std::unique_ptr<GDALDataset>(
      geojson_drv->Create(kGeoJsonInput, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds, NotNull());
  OGRSpatialReference srs_3857;
  srs_3857.importFromEPSG(3857);
  ds->CreateLayer("layer1", &srs_3857, wkbPoint, nullptr);
  ds->Close();
  ::testing::internal::CaptureStdout();
  EXPECT_EQ(0,
            RunGdalSrsInfoMain({"gdalsrsinfo", "-o", "proj4", kGeoJsonInput}));
  const std::string out_proj = ::testing::internal::GetCapturedStdout();
  EXPECT_THAT(out_proj, HasSubstr("+proj="));
}

TEST(GdalsrsinfoTest, FindSrsDirectApi) {
  EnsureDriversRegistered();

  OGRSpatialReference srs;

  // EPSG code string
  EXPECT_TRUE(FindSRS("EPSG:4326", srs));
  EXPECT_STREQ("WGS 84", srs.GetName());

  // URL prefix test: skips GDALOpenEx. Note //third_party/gdal compiles without
  // CURL_ENABLED, so CPLHTTPFetch fails hermetically without network access.
  OGRSpatialReference srs_url;
  absl::Cleanup reset_vsistdin = [] {
    CPLSetConfigOption("CPL_ALLOW_VSISTDIN", nullptr);
  };
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(FindSRS("http://spatialreference.org/nonexistent", srs_url));
  }
  CPLErrorReset();

  // Invalid string
  OGRSpatialReference srs_invalid;
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(FindSRS("INVALID_SRS_XYZ_123", srs_invalid));
  }

  // Raster dataset
  constexpr char kDir[] = "/vsimem/findsrs_test";
  constexpr char kRasterPath[] = "/vsimem/findsrs_test/test.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  ASSERT_THAT(CreateGeoTiffWithSrs(kRasterPath, 4326), NotNull());

  OGRSpatialReference srs_raster;
  EXPECT_TRUE(FindSRS(kRasterPath, srs_raster));
  EXPECT_STREQ("WGS 84", srs_raster.GetName());

  // Vector datasource with layer SRS
  EnsureDriversRegistered();
  auto* geojson_drv_find = GetGDALDriverManager()->GetDriverByName("GeoJSON");
  ASSERT_THAT(geojson_drv_find, NotNull());
  constexpr char kGeoJsonPath[] = "/vsimem/findsrs_test/layer.geojson";
  {
    auto ds = std::unique_ptr<GDALDataset>(
        geojson_drv_find->Create(kGeoJsonPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_THAT(ds, NotNull());
    OGRSpatialReference srs_layer;
    srs_layer.importFromEPSG(3857);
    ds->CreateLayer("layer1", &srs_layer, wkbPoint, nullptr);
    ds->Close();
  }

  OGRSpatialReference srs_vector;
  EXPECT_TRUE(FindSRS(kGeoJsonPath, srs_vector));

  // Corrupt .prj file
  constexpr char kBadPrj[] = "/vsimem/findsrs_test/corrupt.prj";
  const std::string corrupt_wkt = "INVALID_WKT{{";
  VsiMemTempWrapper bad_prj(kBadPrj, corrupt_wkt);
  OGRSpatialReference srs_bad;
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(FindSRS(kBadPrj, srs_bad));
  }
}

TEST(GdalsrsinfoTest, PrintSrsDirectApi) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));

  // Empty or null output type returns CE_None
  EXPECT_EQ(CE_None, PrintSRS(srs, nullptr, true, false));
  EXPECT_EQ(CE_None, PrintSRS(srs, "", true, false));

  // Various output types with bPrintSep = true and false, bPretty = true and
  // false
  EXPECT_EQ(CE_None,
            PrintSRS(srs, "proj4", /*bPretty=*/true, /*bPrintSep=*/true));
  EXPECT_EQ(CE_None,
            PrintSRS(srs, "proj4", /*bPretty=*/false, /*bPrintSep=*/false));

  EXPECT_EQ(CE_None,
            PrintSRS(srs, "PROJJSON", /*bPretty=*/true, /*bPrintSep=*/true));
  EXPECT_EQ(CE_None,
            PrintSRS(srs, "PROJJSON", /*bPretty=*/false, /*bPrintSep=*/false));

  EXPECT_EQ(CE_None,
            PrintSRS(srs, "wkt1", /*bPretty=*/true, /*bPrintSep=*/true));
  EXPECT_EQ(CE_None,
            PrintSRS(srs, "wkt1", /*bPretty=*/false, /*bPrintSep=*/false));

  EXPECT_EQ(CE_None,
            PrintSRS(srs, "wkt_simple", /*bPretty=*/true, /*bPrintSep=*/true));
  EXPECT_EQ(CE_None, PrintSRS(srs, "wkt_simple", /*bPretty=*/false,
                              /*bPrintSep=*/false));

  EXPECT_EQ(CE_None,
            PrintSRS(srs, "wkt_noct", /*bPretty=*/true, /*bPrintSep=*/true));
  EXPECT_EQ(CE_None,
            PrintSRS(srs, "wkt_noct", /*bPretty=*/false, /*bPrintSep=*/false));

  EXPECT_EQ(CE_None,
            PrintSRS(srs, "wkt_esri", /*bPretty=*/true, /*bPrintSep=*/true));
  EXPECT_EQ(CE_None,
            PrintSRS(srs, "wkt_esri", /*bPretty=*/false, /*bPrintSep=*/false));

  EXPECT_EQ(CE_None,
            PrintSRS(srs, "wkt2_2015", /*bPretty=*/true, /*bPrintSep=*/true));
  EXPECT_EQ(CE_None,
            PrintSRS(srs, "wkt2_2015", /*bPretty=*/false, /*bPrintSep=*/false));

  EXPECT_EQ(CE_None,
            PrintSRS(srs, "wkt2", /*bPretty=*/true, /*bPrintSep=*/true));
  EXPECT_EQ(CE_None,
            PrintSRS(srs, "wkt2_2019", /*bPretty=*/false, /*bPrintSep=*/false));
  EXPECT_EQ(CE_None,
            PrintSRS(srs, "wkt", /*bPretty=*/false, /*bPrintSep=*/false));

  EXPECT_EQ(CE_None,
            PrintSRS(srs, "mapinfo", /*bPretty=*/true, /*bPrintSep=*/true));
  EXPECT_EQ(CE_None,
            PrintSRS(srs, "mapinfo", /*bPretty=*/false, /*bPrintSep=*/false));

  EXPECT_EQ(CE_None,
            PrintSRS(srs, "xml", /*bPretty=*/true, /*bPrintSep=*/true));
  EXPECT_EQ(CE_None,
            PrintSRS(srs, "xml", /*bPretty=*/false, /*bPrintSep=*/false));

  // Unsupported output type returns CE_Failure
  {
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, PrintSRS(srs, "unsupported_type", true, false));
  }
}

TEST(GdalsrsinfoTest, PrintSrsOutputTypesDirectApi) {
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));

  const char* const types[] = {"proj4", "wkt2", nullptr};
  PrintSRSOutputTypes(srs, types, /*bPretty=*/true);
  PrintSRSOutputTypes(srs, types, /*bPretty=*/false);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdalsrsinfo.cpp.
