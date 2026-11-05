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

#include <functional>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_csv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class OgrSrsPciTest : public ::testing::Test {
 protected:
  void TearDown() override { OSRCleanup(); }
};

TEST_F(OgrSrsPciTest, CNullPointersAndCorruptData) {
  EXPECT_EQ(OGRERR_FAILURE,
            OSRImportFromPCI(nullptr, "LONG/LAT    D000", "METRE", nullptr));

  char* proj = nullptr;
  char* units = nullptr;
  double* params = nullptr;
  EXPECT_EQ(OGRERR_FAILURE, OSRExportToPCI(nullptr, &proj, &units, &params));

  OGRSpatialReference srs;
  // Null or too short projection string (< 16 chars)
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromPCI(nullptr, "METRE", nullptr));
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromPCI("SHORT", "METRE", nullptr));
}

TEST_F(OgrSrsPciTest, ImportProjectionsAndParameters) {
  OGRSpatialReference srs;

  double params[17] = {0};

  // LONG/LAT
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LONG/LAT    D000", "DEGREE", nullptr));

  // METER / METRE
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("METER           ", "METRE", nullptr));
  EXPECT_TRUE(srs.IsLocal());

  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("METRE           ", "METRE", nullptr));
  EXPECT_TRUE(srs.IsLocal());

  // FEET / FOOT
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("FEET            ", "METRE", nullptr));
  EXPECT_TRUE(srs.IsLocal());

  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("FOOT            ", "METRE", nullptr));
  EXPECT_TRUE(srs.IsLocal());

  // ACEA
  params[2] = -96.0;   // Central Meridian
  params[3] = 37.5;    // Lat Origin
  params[4] = 29.5;    // Std Parallel 1
  params[5] = 45.5;    // Std Parallel 2
  params[6] = 1000.0;  // False Easting
  params[7] = 2000.0;  // False Northing
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("ACEA        D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // AE
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("AE          D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // CASS
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("CASS        D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // EC
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("EC          D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // ER
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("ER          D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // GNO
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("GNO         D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // LAEA
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LAEA        D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // LCC
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LCC         D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // LCC_1SP
  params[8] = 0.9996;  // Scale factor
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LCC_1SP     D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // MC
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("MC          D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // MER: Web Mercator special case (D894 + params 0 except scale 1.0)
  double web_params[17] = {0};
  web_params[8] = 1.0;
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("MER         D894", "METRE", web_params));
  EXPECT_TRUE(srs.IsProjected());

  // MER: Normal Mercator
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("MER         D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // OG
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("OG          D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // OM: Azimuth mode (center line points are 0)
  params[10] = 0.0;
  params[11] = 0.0;
  params[12] = 0.0;
  params[13] = 0.0;
  params[14] = 45.0;  // Azimuth
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("OM          D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // OM: Two-point mode (center line points non-zero)
  params[10] = -120.0;
  params[11] = 30.0;
  params[12] = -110.0;
  params[13] = 40.0;
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("OM          D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // PC
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("PC          D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // PS
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("PS          D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // ROB
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("ROB         D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // SGDO
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("SGDO        D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // SG
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("SG          D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // SIN
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("SIN         D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // SPCS, SPIF, SPAF
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("SPCS 0101   D-01", "METRE", nullptr));
  EXPECT_TRUE(srs.IsProjected());

  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("SPIF 0101   D-02", "FEET", nullptr));
  EXPECT_TRUE(srs.IsProjected());

  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("SPAF 0101   D-01", "METRE", nullptr));
  EXPECT_TRUE(srs.IsProjected());

  // TM
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("TM          D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // UTM
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("UTM  11     D000", "METRE", nullptr));
  int bNorth = FALSE;
  EXPECT_EQ(11, srs.GetUTMZone(&bNorth));
  EXPECT_TRUE(bNorth);

  // UTM South
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("UTM  -11    D000", "METRE", nullptr));
  EXPECT_EQ(11, srs.GetUTMZone(&bNorth));
  EXPECT_FALSE(bNorth);

  // UTM with MGRS zone letters: North ('N' to 'X'), South ('C' to 'M'), Other
  // ('Z')
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("UTM  11   N D000", "METRE", nullptr));
  EXPECT_EQ(11, srs.GetUTMZone(&bNorth));
  EXPECT_TRUE(bNorth);

  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("UTM  11   G D000", "METRE", nullptr));
  EXPECT_EQ(11, srs.GetUTMZone(&bNorth));
  EXPECT_FALSE(bNorth);

  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("UTM  11   Z D000", "METRE", nullptr));
  EXPECT_EQ(11, srs.GetUTMZone(&bNorth));

  // VDG
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("VDG         D000", "METRE", params));
  EXPECT_TRUE(srs.IsProjected());

  // Unsupported projection -> Local CS
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("UNSUPPORTED D000", "METRE", nullptr));
  EXPECT_TRUE(srs.IsLocal());
}

TEST_F(OgrSrsPciTest, ImportDatumsAndEllipsoids) {
  const std::string kPciEllipsCsv =
      "E700,Custom Ellipsoid 700,6378200.0,6356800.0\n";

  const std::string kPciDatumCsv =
      "D701,Custom Datum 3P,E012,10,20,30,0,0,0,0,0\n"
      "D702,Custom Datum 7P Scale1,E012,10,20,30,0,0,0,0,0,1,2,3,2.0\n"
      "D703,Custom Datum 7P PPM,E700,15,25,35,0,0,0,0,0,4,5,6,5.5\n";

  VsiMemTempWrapper ellips_file("/vsimem/ogr_srs_pci_test/pci_ellips.txt",
                                kPciEllipsCsv);
  VsiMemTempWrapper datum_file("/vsimem/ogr_srs_pci_test/pci_datum.txt",
                               kPciDatumCsv);
  CPLPushFinderLocation("/vsimem/ogr_srs_pci_test");
  auto cleanup_finder = absl::MakeCleanup([] {
    CSVDeaccess(nullptr);
    CPLPopFinderLocation();
  });

  OGRSpatialReference srs;

  // In-code datums: D000 (WGS84), D-01 (NAD27), D001 (WGS72)
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LONG/LAT    D000", "DEGREE", nullptr));
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LONG/LAT    D-01", "DEGREE", nullptr));
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LONG/LAT    D001", "DEGREE", nullptr));

  // CSV Datums
  // D701: 3-param shift
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LONG/LAT    D701", "DEGREE", nullptr));
  double towgs84[7] = {0};
  EXPECT_EQ(OGRERR_NONE, srs.GetTOWGS84(towgs84, 7));
  EXPECT_DOUBLE_EQ(10.0, towgs84[0]);

  // D702: 7-param shift with scale ~ 1.000002 -> converted to ppm
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LONG/LAT    D702", "DEGREE", nullptr));
  EXPECT_EQ(OGRERR_NONE, srs.GetTOWGS84(towgs84, 7));
  EXPECT_NEAR(2.0, towgs84[6], 1e-3);

  // D703: 7-param shift with ppm scale and custom ellipsoid E700
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LONG/LAT    D703", "DEGREE", nullptr));
  EXPECT_EQ(OGRERR_NONE, srs.GetTOWGS84(towgs84, 7));
  EXPECT_DOUBLE_EQ(5.5, towgs84[6]);
  EXPECT_NEAR(6378200.0, srs.GetSemiMajor(), 1.0);

  // In-code Ellipsoid: E000, E012
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LONG/LAT    E000", "DEGREE", nullptr));
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LONG/LAT    E012", "DEGREE", nullptr));

  // Custom Ellipsoid E700 from pci_ellips.txt
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LONG/LAT    E700", "DEGREE", nullptr));
  EXPECT_NEAR(6378200.0, srs.GetSemiMajor(), 1.0);

  // Custom Spheroid E999 with params[0] and params[1]
  double params[17] = {0};
  params[0] = 6378300.0;
  params[1] = 6356900.0;
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LONG/LAT    E999", "DEGREE", params));
  EXPECT_DOUBLE_EQ(6378300.0, srs.GetSemiMajor());

  // E999 with params[0] == 0.0 -> falls back to WGS84
  params[0] = 0.0;
  EXPECT_EQ(OGRERR_NONE,
            srs.importFromPCI("LONG/LAT    E999", "DEGREE", params));
  EXPECT_DOUBLE_EQ(SRS_WGS84_SEMIMAJOR, srs.GetSemiMajor());
}

TEST_F(OgrSrsPciTest, ExportProjections) {
  char* pszProj = nullptr;
  char* pszUnits = nullptr;
  double* padfParams = nullptr;

  auto CleanupExport = [&]() {
    CPLFree(pszProj);
    pszProj = nullptr;
    CPLFree(pszUnits);
    pszUnits = nullptr;
    CPLFree(padfParams);
    padfParams = nullptr;
  };

  OGRSpatialReference srs;

  // Local CS with feet
  srs.Clear();
  srs.SetLocalCS("LocalFeet");
  srs.SetLinearUnits("FEET", 0.3048);
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("FEET"));
  CleanupExport();

  // Local CS with meter
  srs.Clear();
  srs.SetLocalCS("LocalMeter");
  srs.SetLinearUnits("METER", 1.0);
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("METER"));
  CleanupExport();

  // Geographic
  srs.Clear();
  srs.importFromEPSG(4326);
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("LONG/LAT"));
  CleanupExport();

  // All projected coordinate systems
  const struct {
    const char* expected_proj;
    std::function<void(OGRSpatialReference&)> setup;
  } kProjections[] = {
      {"ACEA",
       [](OGRSpatialReference& s) {
         s.SetACEA(29.5, 45.5, 37.5, -96.0, 0, 0);
       }},
      {"AE", [](OGRSpatialReference& s) { s.SetAE(37.5, -96.0, 0, 0); }},
      {"CASS", [](OGRSpatialReference& s) { s.SetCS(37.5, -96.0, 0, 0); }},
      {"EC",
       [](OGRSpatialReference& s) { s.SetEC(29.5, 45.5, 37.5, -96.0, 0, 0); }},
      {"ER",
       [](OGRSpatialReference& s) {
         s.SetEquirectangular2(0, -96.0, 37.5, 0, 0);
       }},
      {"GNO", [](OGRSpatialReference& s) { s.SetGnomonic(37.5, -96.0, 0, 0); }},
      {"LAEA", [](OGRSpatialReference& s) { s.SetLAEA(37.5, -96.0, 0, 0); }},
      {"LCC",
       [](OGRSpatialReference& s) { s.SetLCC(29.5, 45.5, 37.5, -96.0, 0, 0); }},
      {"LCC_1SP",
       [](OGRSpatialReference& s) { s.SetLCC1SP(37.5, -96.0, 0.9996, 0, 0); }},
      {"MC", [](OGRSpatialReference& s) { s.SetMC(37.5, -96.0, 0, 0); }},
      {"MER",
       [](OGRSpatialReference& s) { s.SetMercator(0.0, 0.0, 1.0, 0, 0); }},
      {"OG",
       [](OGRSpatialReference& s) { s.SetOrthographic(37.5, -96.0, 0, 0); }},
      {"OM",
       [](OGRSpatialReference& s) {
         s.SetHOM(37.5, -96.0, 45.0, 45.0, 0.9996, 0, 0);
       }},
      {"OM",
       [](OGRSpatialReference& s) {
         s.SetHOM2PNO(37.5, 30.0, -120.0, 40.0, -110.0, 0.9996, 0, 0);
       }},
      {"PC", [](OGRSpatialReference& s) { s.SetPolyconic(37.5, -96.0, 0, 0); }},
      {"PS", [](OGRSpatialReference& s) { s.SetPS(90.0, -96.0, 1.0, 0, 0); }},
      {"ROB", [](OGRSpatialReference& s) { s.SetRobinson(-96.0, 0, 0); }},
      {"SGDO",
       [](OGRSpatialReference& s) { s.SetOS(37.5, -96.0, 0.9996, 0, 0); }},
      {"SG",
       [](OGRSpatialReference& s) {
         s.SetStereographic(37.5, -96.0, 0.9996, 0, 0);
       }},
      {"SIN", [](OGRSpatialReference& s) { s.SetSinusoidal(-96.0, 0, 0); }},
      {"TM",
       [](OGRSpatialReference& s) { s.SetTM(37.5, -96.0, 0.9996, 0, 0); }},
      {"UTM", [](OGRSpatialReference& s) { s.SetUTM(11, TRUE); }},
      {"UTM", [](OGRSpatialReference& s) { s.SetUTM(11, FALSE); }},
      {"VDG", [](OGRSpatialReference& s) { s.SetVDG(-96.0, 0, 0); }},
      {"PIXEL", [](OGRSpatialReference& s) { s.SetProjection("Unsupported"); }},
  };

  for (const auto& proj_test : kProjections) {
    srs.Clear();
    srs.SetWellKnownGeogCS("WGS84");
    proj_test.setup(srs);
    EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams))
        << "Failed for " << proj_test.expected_proj;
    EXPECT_THAT(pszProj, HasSubstr(proj_test.expected_proj));
    CleanupExport();
  }

  // Web Mercator 3857 -> D894
  srs.Clear();
  srs.importFromEPSG(3857);
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("MER"));
  EXPECT_THAT(pszProj, HasSubstr("D894"));
  CleanupExport();
}

TEST_F(OgrSrsPciTest, ExportDatumsAndEllipsoids) {
  const std::string kPciEllipsCsv =
      "E700,Custom Ellipsoid 700,6378200.0,6356800.0\n";

  const std::string kPciDatumCsv =
      "D701,Custom Datum 3P,E012,10,20,30,0,0,0,0,0\n"
      "D702,Custom Datum 7P Scale1,E012,10,20,30,0,0,0,0,0,1,2,3,2.0\n"
      "D703,Custom Datum 7P PPM,E700,15,25,35,0,0,0,0,0,4,5,6,5.5\n";

  VsiMemTempWrapper ellips_file("/vsimem/ogr_srs_pci_test/pci_ellips.txt",
                                kPciEllipsCsv);
  VsiMemTempWrapper datum_file("/vsimem/ogr_srs_pci_test/pci_datum.txt",
                               kPciDatumCsv);
  CPLPushFinderLocation("/vsimem/ogr_srs_pci_test");
  auto cleanup_finder = absl::MakeCleanup([] {
    CSVDeaccess(nullptr);
    CPLPopFinderLocation();
  });

  char* pszProj = nullptr;
  char* pszUnits = nullptr;
  double* padfParams = nullptr;

  auto CleanupExport = [&]() {
    CPLFree(pszProj);
    pszProj = nullptr;
    CPLFree(pszUnits);
    pszUnits = nullptr;
    CPLFree(padfParams);
    padfParams = nullptr;
  };

  OGRSpatialReference srs;

  // Well known datums: NAD27 (D-01), NAD83 (D-02), WGS84 (D000)
  srs.Clear();
  srs.SetWellKnownGeogCS("NAD27");
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("D-01"));
  CleanupExport();

  srs.Clear();
  srs.SetWellKnownGeogCS("NAD83");
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("D-02"));
  CleanupExport();

  srs.Clear();
  srs.SetWellKnownGeogCS("WGS84");
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("D000"));
  CleanupExport();

  // EPSG Code lookup from asDatums (e.g. EPSG:4322 -> D001)
  srs.Clear();
  srs.importFromEPSG(4322);
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("D001"));
  CleanupExport();

  // Match in asEllips: International 1924 -> E004
  srs.Clear();
  srs.SetGeogCS("CustomGCS", "CustomDatum", "International 1924", 6378388.0,
                297.0);
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("E004"));
  CleanupExport();

  // Match in pci_ellips.txt: E700
  srs.Clear();
  srs.SetGeogCS("CustomGCS", "CustomDatum", "CustomEllips", 6378200.0,
                6378200.0 / (6378200.0 - 6356800.0));
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("E700"));
  CleanupExport();

  // Custom ellipsoid fallback -> E999
  srs.Clear();
  srs.SetGeogCS("CustomGCS", "CustomDatum", "UniqueEllips", 6379999.0, 300.0);
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("E999"));
  EXPECT_DOUBLE_EQ(6379999.0, padfParams[0]);
  CleanupExport();

  // Match in pci_datum.txt by datum name + ellipsoid: D703
  srs.Clear();
  srs.SetGeogCS("CustomGCS", "Custom Datum 7P PPM", "CustomEllips", 6378200.0,
                6378200.0 / (6378200.0 - 6356800.0));
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("D703"));
  CleanupExport();

  // Match in pci_datum.txt by 3-parameter TOWGS84: D701
  srs.Clear();
  srs.SetGeogCS("CustomGCS", "OtherDatum", "WGS 84", 6378137.0, 298.257223563);
  srs.SetTOWGS84(10, 20, 30);
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("D701"));
  CleanupExport();

  // Match in pci_datum.txt by 7-parameter TOWGS84: D702 (scale ~ 1.000002
  // -> 2.0 ppm)
  srs.Clear();
  srs.SetGeogCS("CustomGCS", "OtherDatum", "WGS 84", 6378137.0, 298.257223563);
  srs.SetTOWGS84(10, 20, 30, 1, 2, 3, 2.0);
  EXPECT_EQ(OGRERR_NONE, srs.exportToPCI(&pszProj, &pszUnits, &padfParams));
  EXPECT_THAT(pszProj, HasSubstr("D702"));
  CleanupExport();
}

}  // namespace
}  // namespace autotest2
