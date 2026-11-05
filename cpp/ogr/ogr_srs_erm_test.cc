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
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

constexpr char kWgs84GeogCsWkt[] =
    "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
    "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0."
    "0174532925199433]]";

constexpr char kCustomGeogCsWkt[] =
    "GEOGCS[\"Custom\",DATUM[\"CUSTOM_DATUM\",SPHEROID[\"WGS "
    "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0."
    "0174532925199433]]";

constexpr char kTmProjWkt[] =
    "PROJCS[\"TM_PROJ\",PROJECTION[\"Transverse_Mercator\"],PARAMETER["
    "\"latitude_of_origin\",0],PARAMETER[\"central_meridian\",0],PARAMETER["
    "\"scale_factor\",1],PARAMETER[\"false_easting\",0],PARAMETER[\"false_"
    "northing\",0],UNIT[\"Meter\",1.0]]";

TEST(OgrSrsErmTest, ImportFromErm) {
  const std::string kEcwCsData =
      std::string("WGS84,") + kWgs84GeogCsWkt + "\n" + "CUSTOM_DATUM," +
      kCustomGeogCsWkt + "\n" + "TM_PROJ," + kTmProjWkt + "\n" +
      "LOCAL_PROJ,LOCAL_CS[\"Local\"]\n" +
      "NO_PROJ_KW,PROJCS[\"Bad\",UNIT[\"Meter\",1.0]]\n" +
      "NO_BRACKET,PROJCS[\"Bad\"\n";

  VsiMemTempWrapper ecw_cs("/vsimem/ogr_srs_erm_test/ecw_cs.wkt", kEcwCsData);
  CPLPushFinderLocation("/vsimem/ogr_srs_erm_test");
  auto cleanup_finder = absl::MakeCleanup([] { CPLPopFinderLocation(); });

  OGRSpatialReference srs;

  // RAW projection
  EXPECT_EQ(OGRERR_NONE, srs.importFromERM("RAW", "WGS84", "METERS"));

  // EPSG code in proj and in datum
  EXPECT_EQ(OGRERR_NONE, srs.importFromERM("EPSG:4326", "RAW", "METERS"));
  EXPECT_EQ(OGRERR_NONE, srs.importFromERM("RAW_NOT", "EPSG:4326", "METERS"));

  // Missing datum in ecw_cs.wkt
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS,
            srs.importFromERM("GEODETIC", "NONEXISTENT", "METERS"));

  // GEODETIC with valid datum
  EXPECT_EQ(OGRERR_NONE, srs.importFromERM("GEODETIC", "WGS84", "METERS"));
  EXPECT_TRUE(srs.IsGeographic());

  // Projected with METERS and FEET
  EXPECT_EQ(OGRERR_NONE, srs.importFromERM("TM_PROJ", "WGS84", "METERS"));
  EXPECT_TRUE(srs.IsProjected());

  EXPECT_EQ(OGRERR_NONE, srs.importFromERM("TM_PROJ", "WGS84", "FEET"));
  EXPECT_TRUE(srs.IsProjected());

  // LOCAL_CS
  EXPECT_EQ(OGRERR_NONE, srs.importFromERM("LOCAL_PROJ", "WGS84", "METERS"));
  EXPECT_TRUE(srs.IsLocal());

  // Projection not found or invalid format
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS,
            srs.importFromERM("NONEXISTENT_PROJ", "WGS84", "METERS"));
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS,
            srs.importFromERM("NO_BRACKET", "WGS84", "METERS"));
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS,
            srs.importFromERM("NO_PROJ_KW", "WGS84", "METERS"));

  // C API null check
  EXPECT_EQ(OGRERR_FAILURE,
            OSRImportFromERM(nullptr, "GEODETIC", "WGS84", "METERS"));

  // C API valid call
  OGRSpatialReferenceH hSRS = OSRNewSpatialReference(nullptr);
  ASSERT_NE(nullptr, hSRS);
  auto cleanup_srs =
      absl::MakeCleanup([hSRS] { OSRDestroySpatialReference(hSRS); });
  EXPECT_EQ(OGRERR_NONE, OSRImportFromERM(hSRS, "GEODETIC", "WGS84", "METERS"));
}

TEST(OgrSrsErmTest, ExportToErm) {
  const std::string kEcwCsData = std::string("WGS84,") + kWgs84GeogCsWkt +
                                 "\n" + "CUSTOM_DATUM," + kCustomGeogCsWkt +
                                 "\n" + "TM_PROJ," + kTmProjWkt + "\n";

  VsiMemTempWrapper ecw_cs("/vsimem/ogr_srs_erm_test/ecw_cs.wkt", kEcwCsData);
  CPLPushFinderLocation("/vsimem/ogr_srs_erm_test");
  auto cleanup_finder = absl::MakeCleanup([] { CPLPopFinderLocation(); });

  char szProj[32] = {};
  char szDatum[32] = {};
  char szUnits[32] = {};

  // Empty SRS
  OGRSpatialReference empty_srs;
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS,
            empty_srs.exportToERM(szProj, szDatum, szUnits));

  // C API null check
  EXPECT_EQ(OGRERR_FAILURE, OSRExportToERM(nullptr, szProj, szDatum, szUnits));

  // Geographic with custom datum in ecw_cs.wkt
  OGRSpatialReference custom_srs;
  custom_srs.importFromWkt(kCustomGeogCsWkt);
  EXPECT_EQ(OGRERR_NONE, custom_srs.exportToERM(szProj, szDatum, szUnits));
  EXPECT_STREQ("GEODETIC", szProj);
  EXPECT_STREQ("CUSTOM_DATUM", szDatum);

  // Well known EPSG geographic coordinate systems
  const int kEpsgCodes[] = {4326, 4322, 4267, 4269, 4277, 4278, 4201, 4202,
                            4203, 4209, 4210, 4275, 4283, 4284, 7844};
  for (int code : kEpsgCodes) {
    OGRSpatialReference srs;
    srs.importFromEPSG(code);
    EXPECT_EQ(OGRERR_NONE, srs.exportToERM(szProj, szDatum, szUnits)) << code;
    EXPECT_STREQ("GEODETIC", szProj) << code;
    EXPECT_STRNE("RAW", szDatum) << code;
  }

  // Unknown geographic datum without EPSG code
  OGRSpatialReference unknown_geog;
  unknown_geog.SetGeogCS("Unknown", "Unknown_Datum", "Spheroid", 6378137,
                         298.257);
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS,
            unknown_geog.exportToERM(szProj, szDatum, szUnits));

  // Projected UTM North
  OGRSpatialReference utm_north;
  utm_north.SetWellKnownGeogCS("WGS84");
  utm_north.SetUTM(11, TRUE);
  EXPECT_EQ(OGRERR_NONE, utm_north.exportToERM(szProj, szDatum, szUnits));
  EXPECT_STREQ("NUTM11", szProj);
  EXPECT_STREQ("WGS84", szDatum);
  EXPECT_STREQ("METERS", szUnits);

  // Projected UTM South
  OGRSpatialReference utm_south;
  utm_south.SetWellKnownGeogCS("WGS84");
  utm_south.SetUTM(11, FALSE);
  EXPECT_EQ(OGRERR_NONE, utm_south.exportToERM(szProj, szDatum, szUnits));
  EXPECT_STREQ("SUTM11", szProj);

  // Projected MGA (GDA94 zone 50 South)
  OGRSpatialReference mga_srs;
  mga_srs.importFromEPSG(28350);
  EXPECT_EQ(OGRERR_NONE, mga_srs.exportToERM(szProj, szDatum, szUnits));
  EXPECT_STREQ("MGA50", szProj);

  // Non-UTM matching ecw_cs.wkt PROJCS name
  std::string tm_wkt =
      std::string("PROJCS[\"TM_PROJ\",") + kWgs84GeogCsWkt +
      ",PROJECTION[\"Transverse_Mercator\"],PARAMETER["
      "\"latitude_of_origin\",0],PARAMETER[\"central_meridian\",0],PARAMETER["
      "\"scale_factor\",1],PARAMETER[\"false_easting\",0],PARAMETER[\"false_"
      "northing\",0],UNIT[\"Meter\",1.0]]";
  OGRSpatialReference tm_srs;
  EXPECT_EQ(OGRERR_NONE, tm_srs.importFromWkt(tm_wkt.c_str()));
  EXPECT_EQ(OGRERR_NONE, tm_srs.exportToERM(szProj, szDatum, szUnits));
  EXPECT_STREQ("TM_PROJ", szProj);

  // Non-UTM fallback to EPSG code (e.g. EPSG:3857)
  OGRSpatialReference epsg_proj;
  epsg_proj.importFromEPSG(3857);
  EXPECT_EQ(OGRERR_NONE, epsg_proj.exportToERM(szProj, szDatum, szUnits));
  EXPECT_STREQ("EPSG:3857", szProj);

  // Projected with US Foot units (e.g. State Plane NC ftUS)
  OGRSpatialReference feet_srs;
  EXPECT_EQ(OGRERR_NONE, feet_srs.importFromEPSG(2264));
  EXPECT_EQ(OGRERR_NONE, feet_srs.exportToERM(szProj, szDatum, szUnits));
  EXPECT_STREQ("FEET", szUnits);

  // Untranslatable projected SRS without EPSG or dict entry
  OGRSpatialReference unk_proj;
  unk_proj.SetProjCS("Unknown Projected");
  unk_proj.SetProjection("Mercator_1SP");
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS,
            unk_proj.exportToERM(szProj, szDatum, szUnits));
}

}  // namespace
}  // namespace autotest2
