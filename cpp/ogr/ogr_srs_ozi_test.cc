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

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_csv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

TEST(OgrSrsOziTest, CNullCheckAndNotEnoughData) {
  // C API null handle
  const char* const kDummyLines[] = {"line1", nullptr};
  EXPECT_EQ(OGRERR_FAILURE, OSRImportFromOzi(nullptr, kDummyLines));

  OGRSpatialReference srs;

  // Less than 5 lines
  const char* const kFewLines[] = {"OziExplorer Map Data File Version 2.2",
                                   "Map Name", "File.tif", "1 ,Map Code,",
                                   nullptr};
  EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA, srs.importFromOzi(kFewLines));

  // Missing Map Projection or Projection Setup
  const char* const kMissingProjLines[] = {
      "OziExplorer Map Data File Version 2.2",
      "Map Name",
      "File.tif",
      "1 ,Map Code,",
      "WGS 84",
      "Point01,xy,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15",
      nullptr};
  EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA, srs.importFromOzi(kMissingProjLines));

  // Too few tokens in Map Projection
  const char* const kBadProjTokenLines[] = {
      "OziExplorer Map Data File Version 2.2",
      "Map Name",
      "File.tif",
      "1 ,Map Code,",
      "WGS 84",
      "Map Projection",
      "Projection Setup,0,0,0,0,0",
      nullptr};
  EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA, srs.importFromOzi(kBadProjTokenLines));
}

TEST(OgrSrsOziTest, ProjectionsAndDatums) {
  const std::string kOziDatumCsv =
      "NAME,ELLIPSOID_CODE,DELTAX,DELTAY,DELTAZ,EPSG_DATUM_CODE\n"
      "WGS 84,20,0,0,0,4326\n"
      "Custom Datum,20,10,20,30,\n"
      "Bad Ellipse Datum,99,0,0,0,\n";

  const std::string kOziEllipsCsv =
      "ELLIPSOID_CODE,NAME,A,INVF\n"
      "20,WGS 84,6378137,298.257223563\n";

  VsiMemTempWrapper datum_csv("/vsimem/ogr_srs_ozi_test/ozi_datum.csv",
                              kOziDatumCsv);
  VsiMemTempWrapper ellips_csv("/vsimem/ogr_srs_ozi_test/ozi_ellips.csv",
                               kOziEllipsCsv);
  CPLPushFinderLocation("/vsimem/ogr_srs_ozi_test");
  auto cleanup_finder = absl::MakeCleanup([] {
    CSVDeaccess(nullptr);
    CPLPopFinderLocation();
  });

  OGRSpatialReference srs;

  // Latitude/Longitude
  {
    const char* const kLines[] = {"OziExplorer Map Data File Version 2.2",
                                  "Map",
                                  "Image",
                                  "1 ,Map Code,",
                                  "WGS 84",
                                  "Map Projection,Latitude/Longitude,",
                                  "Projection Setup,0,0,0,0,0",
                                  nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kLines));
    EXPECT_TRUE(srs.IsGeographic());
    EXPECT_STREQ("4326", srs.GetAuthorityCode("GEOGCS"));
  }

  // Mercator with scale and default scale
  {
    const char* const kLines[] = {"OziExplorer Map Data File Version 2.2",
                                  "Map",
                                  "Image",
                                  "1 ,Map Code,",
                                  "WGS 84",
                                  "Map Projection,Mercator,",
                                  "Projection Setup,0.0,0.0,0.9996,0.0,0.0",
                                  nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kLines));
    EXPECT_TRUE(srs.IsProjected());

    // Empty scale token defaults to 1
    const char* const kLinesEmptyScale[] = {
        "OziExplorer Map Data File Version 2.2",
        "Map",
        "Image",
        "1 ,Map Code,",
        "WGS 84",
        "Map Projection,Mercator,",
        "Projection Setup,0.0,0.0,,0.0,0.0",
        nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kLinesEmptyScale));
  }

  // Transverse Mercator
  {
    const char* const kLines[] = {"OziExplorer Map Data File Version 2.2",
                                  "Map",
                                  "Image",
                                  "1 ,Map Code,",
                                  "WGS 84",
                                  "Map Projection,Transverse Mercator,",
                                  "Projection Setup,0.0,0.0,1.0,500000.0,0.0",
                                  nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kLines));
    EXPECT_TRUE(srs.IsProjected());
  }

  // Lambert Conformal Conic
  {
    const char* const kLines[] = {
        "OziExplorer Map Data File Version 2.2",
        "Map",
        "Image",
        "1 ,Map Code,",
        "WGS 84",
        "Map Projection,Lambert Conformal Conic,",
        "Projection Setup,30.0,60.0,0.0,0.0,0.0,20.0,40.0",
        nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kLines));
    EXPECT_TRUE(srs.IsProjected());
  }

  // Sinusoidal
  {
    const char* const kLines[] = {"OziExplorer Map Data File Version 2.2",
                                  "Map",
                                  "Image",
                                  "1 ,Map Code,",
                                  "WGS 84",
                                  "Map Projection,Sinusoidal,",
                                  "Projection Setup,0.0,10.0,0.0,0.0,0.0",
                                  nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kLines));
    EXPECT_TRUE(srs.IsProjected());
  }

  // Albers Equal Area
  {
    const char* const kLines[] = {
        "OziExplorer Map Data File Version 2.2",
        "Map",
        "Image",
        "1 ,Map Code,",
        "WGS 84",
        "Map Projection,Albers Equal Area,",
        "Projection Setup,23.0,0.0,0.0,0.0,0.0,29.5,45.5",
        nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kLines));
    EXPECT_TRUE(srs.IsProjected());
  }

  // UTM via Point calibration line (both valid and skipped lines)
  {
    const char* const kLines[] = {
        "OziExplorer Map Data File Version 2.2",
        "Map",
        "Image",
        "1 ,Map Code,",
        "WGS 84",
        "Map Projection,(UTM) Universal Transverse Mercator,",
        "Projection Setup,0,0,0,0,0",
        "Point01,xy,,,,,,,,,,,,,,",  // empty tokens, skipped
        "Point02,xy,1,x,y,lat,lon,x,y,x,y,x,y,11,N,N,N",
        nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kLines));
    EXPECT_TRUE(srs.IsProjected());
    int bNorth = FALSE;
    EXPECT_EQ(11, srs.GetUTMZone(&bNorth));
    EXPECT_TRUE(bNorth);
  }

  // UTM via MMPLL guessing: Standard, Norway, Svalbard
  {
    // Standard zone (lon 10, lat 45 -> zone 32)
    const char* const kLinesStandard[] = {
        "OziExplorer Map Data File Version 2.2",
        "Map",
        "Image",
        "1 ,Map Code,",
        "WGS 84",
        "Map Projection,(UTM) Universal Transverse Mercator,",
        "Projection Setup,0,0,0,0,0",
        "MMPLL,1,10.0,45.0",
        "MMPLL,2,11.0,46.0",
        "MMPLL,short",  // short token count, skipped
        nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kLinesStandard));
    int bNorth = FALSE;
    EXPECT_EQ(32, srs.GetUTMZone(&bNorth));

    // Norway exception (lat 60, lon 6 -> zone 32)
    const char* const kLinesNorway[] = {
        "OziExplorer Map Data File Version 2.2",
        "Map",
        "Image",
        "1 ,Map Code,",
        "WGS 84",
        "Map Projection,(UTM) Universal Transverse Mercator,",
        "Projection Setup,0,0,0,0,0",
        "MMPLL,1,5.0,59.0",
        "MMPLL,2,7.0,61.0",
        nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kLinesNorway));
    EXPECT_EQ(32, srs.GetUTMZone(&bNorth));

    // Svalbard exception (lat 75, lon 10 -> zone 33)
    const char* const kLinesSvalbard[] = {
        "OziExplorer Map Data File Version 2.2",
        "Map",
        "Image",
        "1 ,Map Code,",
        "WGS 84",
        "Map Projection,(UTM) Universal Transverse Mercator,",
        "Projection Setup,0,0,0,0,0",
        "MMPLL,1,8.0,74.0",
        "MMPLL,2,12.0,76.0",
        nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kLinesSvalbard));
    EXPECT_EQ(33, srs.GetUTMZone(&bNorth));

    // No MMPLL found
    const char* const kLinesNoMmpll[] = {
        "OziExplorer Map Data File Version 2.2",
        "Map",
        "Image",
        "1 ,Map Code,",
        "WGS 84",
        "Map Projection,(UTM) Universal Transverse Mercator,",
        "Projection Setup,0,0,0,0,0",
        nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kLinesNoMmpll));
  }

  // France Zones I, II, III, IV
  {
    const char* const kFrance1[] = {"OziExplorer Map Data File Version 2.2",
                                    "Map",
                                    "Image",
                                    "1 ,Map Code,",
                                    "WGS 84",
                                    "Map Projection,(I) France Zone I,",
                                    "Projection Setup,0,0,0,0,0",
                                    nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kFrance1));
    EXPECT_TRUE(srs.IsProjected());

    const char* const kFrance2[] = {"OziExplorer Map Data File Version 2.2",
                                    "Map",
                                    "Image",
                                    "1 ,Map Code,",
                                    "WGS 84",
                                    "Map Projection,(II) France Zone II,",
                                    "Projection Setup,0,0,0,0,0",
                                    nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kFrance2));

    const char* const kFrance3[] = {"OziExplorer Map Data File Version 2.2",
                                    "Map",
                                    "Image",
                                    "1 ,Map Code,",
                                    "WGS 84",
                                    "Map Projection,(III) France Zone III,",
                                    "Projection Setup,0,0,0,0,0",
                                    nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kFrance3));

    const char* const kFrance4[] = {"OziExplorer Map Data File Version 2.2",
                                    "Map",
                                    "Image",
                                    "1 ,Map Code,",
                                    "WGS 84",
                                    "Map Projection,(IV) France Zone IV,",
                                    "Projection Setup,0,0,0,0,0",
                                    nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kFrance4));
  }

  // Unsupported projection sets Local CS
  {
    const char* const kUnsupported[] = {"OziExplorer Map Data File Version 2.2",
                                        "Map",
                                        "Image",
                                        "1 ,Map Code,",
                                        "WGS 84",
                                        "Map Projection,UnsupportedProj,",
                                        "Projection Setup,0,0,0,0,0",
                                        nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kUnsupported));
    EXPECT_TRUE(srs.IsLocal());
  }

  // Non-EPSG datum lookup from CSV
  {
    const char* const kCustomDatum[] = {"OziExplorer Map Data File Version 2.2",
                                        "Map",
                                        "Image",
                                        "1 ,Map Code,",
                                        "Custom Datum",
                                        "Map Projection,Latitude/Longitude,",
                                        "Projection Setup,0,0,0,0,0",
                                        nullptr};
    EXPECT_EQ(OGRERR_NONE, srs.importFromOzi(kCustomDatum));
    EXPECT_TRUE(srs.IsGeographic());
  }

  // Error cases: Datum not found in CSV
  {
    WithQuietHandler quiet_handler;
    const char* const kMissingDatum[] = {
        "OziExplorer Map Data File Version 2.2",
        "Map",
        "Image",
        "1 ,Map Code,",
        "Unknown Datum Not In CSV",
        "Map Projection,Latitude/Longitude,",
        "Projection Setup,0,0,0,0,0",
        nullptr};
    EXPECT_EQ(OGRERR_FAILURE, srs.importFromOzi(kMissingDatum));
  }

  // Error cases: Datum with ellipsoid not in ellips.csv
  {
    WithQuietHandler quiet_handler;
    const char* const kBadEllipsDatum[] = {
        "OziExplorer Map Data File Version 2.2",
        "Map",
        "Image",
        "1 ,Map Code,",
        "Bad Ellipse Datum",
        "Map Projection,Latitude/Longitude,",
        "Projection Setup,0,0,0,0,0",
        nullptr};
    EXPECT_EQ(OGRERR_FAILURE, srs.importFromOzi(kBadEllipsDatum));
  }

  // Projection setup too short for Mercator (< 6 params)
  {
    const char* const kShortParams[] = {"OziExplorer Map Data File Version 2.2",
                                        "Map",
                                        "Image",
                                        "1 ,Map Code,",
                                        "WGS 84",
                                        "Map Projection,Mercator,",
                                        "Projection Setup,0,0",
                                        nullptr};
    EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA, srs.importFromOzi(kShortParams));
  }

  // C API wrapper with valid handle
  {
    const char* const kLines[] = {"OziExplorer Map Data File Version 2.2",
                                  "Map",
                                  "Image",
                                  "1 ,Map Code,",
                                  "WGS 84",
                                  "Map Projection,Latitude/Longitude,",
                                  "Projection Setup,0,0,0,0,0",
                                  nullptr};
    EXPECT_EQ(OGRERR_NONE,
              OSRImportFromOzi(OGRSpatialReference::ToHandle(&srs), kLines));
  }
}

}  // namespace
}  // namespace autotest2
