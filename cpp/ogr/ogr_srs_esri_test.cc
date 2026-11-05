// Copyright 2017 Google Inc. All Rights Reserved.
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

#include <cstdlib>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

std::string Wkt(const OGRSpatialReference &srs) {
  char *wkt_tmp = nullptr;
  EXPECT_EQ(OGRERR_NONE, srs.exportToWkt(&wkt_tmp));
  if (wkt_tmp == nullptr) return "";
  std::string wkt(wkt_tmp);
  CPLFree(wkt_tmp);

  return wkt;
}

TEST(EsriTest, MorphToESRI) {
  OGRSpatialReference srs;
  srs.importFromEPSG(4202);
  EXPECT_STREQ("Australian_Geodetic_Datum_1966", srs.GetAttrValue("DATUM"));

  srs.morphToESRI();
  EXPECT_STREQ("D_Australian_1966", srs.GetAttrValue("DATUM"));
}

TEST(EsriTest, EmptyString) {
  // Totally bogus string.
  auto csl = VectorToCsl(std::vector<std::string>({""}));
  auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromESRI(csl));
}

TEST(EsriTest, OverRangeStatePlane) {
  WithQuietHandler error_handler;
  auto csl = VectorToCsl(
      std::vector<std::string>({"Projection STATEPLANE", "zone7777777777"}));
  auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromESRI(csl));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("zone"));
}

TEST(EsriTest, ImportFromEsriStatePlaneFlorida) {
  auto csl = VectorToCsl({
      "Projection    STATEPLANE",
      "Fipszone      903",
      "Datum         NAD83",
      "Spheroid      GRS80",
      "Units         FEET",
      "Zunits        NO",
      "Xshift        0.0",
      "Yshift        0.0",
      "Parameters    ",
      "",
  });
  auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
  std::string wkt = Wkt(srs);
  EXPECT_THAT(wkt, testing::HasSubstr("Florida"));
}

TEST(EsriTest, EquidistantConic) {
  // Rewrite of osr_esri_9.

  constexpr char original[] =
      R"wkt(PROJCS["edc",GEOGCS["GCS_North_American_1983",
      DATUM["D_North_American_1983",
      SPHEROID["GRS_1980",6378137.0,298.257222101]],
      PRIMEM["Greenwich",0.0],
      UNIT["Degree",0.0174532925199433]],
      PROJECTION["Equidistant_Conic"],
      PARAMETER["False_Easting",0.0],
      PARAMETER["False_Northing",0.0],
      PARAMETER["Central_Meridian",-96.0],
      PARAMETER["Standard_Parallel_1",29.5],
      PARAMETER["Standard_Parallel_2",45.5],
      PARAMETER["Latitude_Of_Origin",37.5],
      UNIT["Meter",1.0]])wkt";

  constexpr char expected[] =
      "PROJCS[\"edc\",GEOGCS[\"NAD83\","
      "DATUM[\"North_American_Datum_1983\","
      "SPHEROID[\"GRS 1980\",6378137,298.257222101,"
      "AUTHORITY[\"EPSG\",\"7019\"]],AUTHORITY[\"EPSG\",\"6269\"]],"
      "PRIMEM[\"Greenwich\",0],UNIT[\"Degree\",0.0174532925199433]],"
      "PROJECTION[\"Equidistant_Conic\"],"
      "PARAMETER[\"latitude_of_center\",37.5],"
      "PARAMETER[\"longitude_of_center\",-96],"
      "PARAMETER[\"standard_parallel_1\",29.5],"
      "PARAMETER[\"standard_parallel_2\",45.5],"
      "PARAMETER[\"false_easting\",0],"
      "PARAMETER[\"false_northing\",0],"
      "UNIT[\"metre\",1,AUTHORITY[\"EPSG\",\"9001\"]],"
      "AXIS[\"Easting\",EAST],AXIS[\"Northing\",NORTH]]";

  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.SetFromUserInput(original));
  EXPECT_EQ(OGRERR_NONE, srs.morphFromESRI());
  auto wkt = Wkt(srs);
  EXPECT_EQ(expected, wkt);
}

TEST(EsriTest, EquidistantConicBad) {
  // Autofuzz POC from b/65416453
  auto csl = VectorToCsl({
      "PROJECTIONLOCA?L_CSw?(  EQUIDISTANT_CONIC",
      "Paramet",
      "55555555555555",
  });
  auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
  OGRSpatialReference srs;

  WithQuietHandler error_handler;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromESRI(csl));
  char *wkt_tmp = nullptr;
  EXPECT_EQ(OGRERR_FAILURE, srs.exportToWkt(&wkt_tmp));
  free(wkt_tmp);
}

TEST(EsriTest, TruncatedWkt) {
  // From osr_esri_33 in https://trac.osgeo.org/gdal/changeset/40216
  OGRSpatialReference srs;
  srs.SetFromUserInput("PROJCS[]");
  EXPECT_EQ(OGRERR_NONE, srs.morphFromESRI());
  EXPECT_EQ(OGRERR_NONE, srs.morphToESRI());
}

TEST(EsriTest, OSRImportFromESRIApiAndNulls) {
  WithQuietHandler quiet_handler;
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_FAILURE, OSRImportFromESRI(nullptr, nullptr));

  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromESRI(nullptr));

  auto csl = VectorToCsl({"Projection GEOGRAPHIC"});
  auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
  EXPECT_EQ(OGRERR_NONE,
            OSRImportFromESRI(OGRSpatialReference::ToHandle(&srs), csl));
}

TEST(EsriTest, ImportFromEsriWktVariants) {
  {
    auto csl = VectorToCsl(
        {"GEOGCS[\"WGS 84\",",
         "DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],",
         "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsGeographic());
  }
  {
    auto csl = VectorToCsl(
        {"PROJCS[\"unnamed\",",
         "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
         "84\",6378137,298.257223563]],",
         "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],",
         "PROJECTION[\"Mercator_1SP\"],UNIT[\"metre\",1]]"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  {
    auto csl = VectorToCsl({"LOCAL_CS[\"Local\",UNIT[\"metre\",1]]"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsLocal());
  }
  {
    auto csl = VectorToCsl(
        {"COMPD_CS[\"Compound\",LOCAL_CS[\"Local\",UNIT[\"metre\",1]],VERT_CS["
         "\"Vertical\",VERT_DATUM[\"Datum\",2005],UNIT[\"metre\",1]]]"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsCompound());
  }
}

TEST(EsriTest, ImportFromEsriGdvAndComments) {
  auto csl = VectorToCsl({
      "Projection ALBERS",
      "Datum",
      "Parameters",
      "/* comment at start */",
      "45 30 15.0 /* positive DMS */",
      "-120 30 0.0 /* negative DMS */",
      "45 30 75.0 /* invalid second >= 60.0 resets to 0.0 */",
      "45 30 -5.0 /* invalid second < 0.0 resets to 0.0 */",
      "100.5 /* single token value */",
      "   /* empty / whitespace tokens */",
  });
  auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
  EXPECT_TRUE(srs.IsProjected());
}

TEST(EsriTest, ImportFromEsriGeographicDatums) {
  const std::vector<std::string> datums = {"NAD27", "NAD83", "WGS84", "WGS72",
                                           "EUR",   "ED50",  "GDA94"};
  for (const auto& datum : datums) {
    auto csl = VectorToCsl({"Projection GEOGRAPHIC", "Datum " + datum});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsGeographic());
  }
  OSRCleanup();
}

TEST(EsriTest, ImportFromEsriUtmVariants) {
  // With zone and North
  {
    auto csl = VectorToCsl({"Projection utm", "zone 10", "Yshift 0.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    int bNorth = FALSE;
    EXPECT_EQ(10, srs.GetUTMZone(&bNorth));
    EXPECT_TRUE(bNorth);
  }
  // With zone and South
  {
    auto csl = VectorToCsl({"Projection utm", "zone 10", "Yshift 10000000.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    int bNorth = TRUE;
    EXPECT_EQ(10, srs.GetUTMZone(&bNorth));
    EXPECT_FALSE(bNorth);
  }
  // Without zone, zone calculated from PARAM_1 and PARAM_2 (North)
  {
    auto csl = VectorToCsl({"Projection utm", "Parameters", "-123.0", "45.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    int bNorth = FALSE;
    EXPECT_EQ(10, srs.GetUTMZone(&bNorth));
    EXPECT_TRUE(bNorth);
  }
  // Without zone, zone calculated from PARAM_1 and PARAM_2 (South)
  {
    auto csl = VectorToCsl({"Projection utm", "Parameters", "-123.0", "-45.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    int bNorth = TRUE;
    EXPECT_EQ(10, srs.GetUTMZone(&bNorth));
    EXPECT_FALSE(bNorth);
  }
  OSRCleanup();
}

TEST(EsriTest, ImportFromEsriStatePlaneVariants) {
  // Zone with NAD27
  {
    auto csl =
        VectorToCsl({"Projection STATEPLANE", "zone 3101", "Datum NAD27"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  // Zone with NAD83
  {
    auto csl =
        VectorToCsl({"Projection STATEPLANE", "zone 3126", "Datum NAD83"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  // Zone not found in table (ESRIToUSGSZone returns 0)
  {
    auto csl =
        VectorToCsl({"Projection STATEPLANE", "zone 99999", "Datum NAD83"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
  }
  // Fipszone out of range
  {
    WithQuietHandler quiet_handler;
    auto csl = VectorToCsl(
        {"Projection STATEPLANE", "zone 0", "fipszone 999999999999"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromESRI(csl));
  }
  OSRCleanup();
}

TEST(EsriTest, ImportFromEsriVariousProjections) {
  // Great Britain Grid
  {
    auto csl = VectorToCsl({"Projection GREATBRITIAN_GRID", "Units 1.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  {
    auto csl = VectorToCsl({"Projection GREATBRITAIN_GRID", "Units 1.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  // Lambert Conic Conformal
  {
    auto csl = VectorToCsl({"Projection LAMBERT", "Parameters", "-120.0",
                            "45.0", "43.0", "47.0", "1000000.0", "0.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  // Lambert Azimuthal without radius of sphere of reference
  {
    auto csl = VectorToCsl({"Projection LAMBERT_AZIMUTHAL", "Parameters",
                            "-100.0", "45.0", "0.0", "0.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  // Lambert Azimuthal with radius of sphere of reference
  {
    auto csl =
        VectorToCsl({"Projection LAMBERT_AZIMUTHAL",
                     "/* radius of the sphere of reference */", "Parameters",
                     "6371000.0", "-100.0", "45.0", "0.0", "0.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  // Equidistant Conic 1 std parallel
  {
    auto csl = VectorToCsl({"Projection EQUIDISTANT_CONIC", "Parameters", "1",
                            "45.0", "-120.0", "0.0", "0.0", "0.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  // Equidistant Conic invalid count
  {
    WithQuietHandler quiet_handler;
    auto csl =
        VectorToCsl({"Projection EQUIDISTANT_CONIC", "Parameters", "-1"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromESRI(csl));
  }
  // Transverse Mercator
  {
    auto csl = VectorToCsl({"Projection TRANSVERSE", "Parameters", "0.9996",
                            "-123.0", "0.0", "500000.0", "0.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  // Polar Stereographic
  {
    auto csl = VectorToCsl(
        {"Projection POLAR", "Parameters", "-100.0", "90.0", "0.0", "0.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  // Mercator
  {
    auto csl = VectorToCsl(
        {"Projection MERCATOR", "Parameters", "0.0", "0.0", "0.0", "0.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  // Mercator Auxiliary Sphere
  {
    auto csl = VectorToCsl({"Projection Mercator_Auxiliary_Sphere"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  // Polyconic
  {
    auto csl = VectorToCsl(
        {"Projection POLYCONIC", "Parameters", "0.0", "-96.0", "0.0", "0.0"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsProjected());
  }
  // Unsupported projection -> Local CS
  {
    auto csl = VectorToCsl({"Projection UNKNOWN_PROJ", "Units 0.3048"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsLocal());
  }
  // Named linear unit
  {
    auto csl = VectorToCsl({"Projection UNKNOWN_PROJ", "Units Chains"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsLocal());
  }
  OSRCleanup();
}

TEST(EsriTest, ImportFromEsriSpheroids) {
  const std::vector<std::string> spheroids = {
      "INT1909",   "INTERNATIONAL1909", "AIRY",       "CLARKE1866", "GRS80",
      "KRASOVSKY", "KRASSOVSKY",        "KRASSOWSKY", "Bessel"};
  for (const auto& sph : spheroids) {
    auto csl = VectorToCsl({"Projection GEOGRAPHIC", "Spheroid " + sph});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsGeographic());
  }
  // Custom Parameters on spheroid
  {
    auto csl = VectorToCsl(
        {"Projection GEOGRAPHIC", "Parameters 6378137.0 6356752.3142"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsGeographic());
  }
  // Fallback to WGS84 when no datum/spheroid/parameters
  {
    auto csl = VectorToCsl({"Projection GEOGRAPHIC"});
    auto csl_cleaner = absl::MakeCleanup([csl] { CSLDestroy(csl); });
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    EXPECT_TRUE(srs.IsGeographic());
  }
  OSRCleanup();
}

TEST(EsriTest, ImportFromESRIStatePlaneWKT) {
  OGRSpatialReference srs;
  // File not found test
  EXPECT_EQ(OGRERR_FAILURE, srs.ImportFromESRIStatePlaneWKT(0, nullptr, nullptr,
                                                            32767, "CS_NAME"));

  // Create temporary in-memory dictionary file
  const char* pszDir = "/vsimem/esri_sp_dir";
  VSIMkdir(pszDir, 0755);
  const std::string dict_path =
      std::string(pszDir) + "/esri_StatePlane_extra.wkt";

  std::string wkt_content = "# Comment line\n";
  const std::vector<int> codes = {
      1010,  1011,   1012,   1013,   1014,   1015,   1016,   2010,   2011,
      2012,  2013,   2014,   2015,   2016,   4010,   4011,   4012,   4013,
      4014,  4015,   4016,   52000,  52001,  52002,  52003,  52004,  52005,
      52006, 102990, 102991, 102992, 102993, 102994, 102995, 102996,
  };
  for (int c : codes) {
    const std::string sc = std::to_string(c);
    wkt_content +=
        sc + ",PROJCS[\"CS_" + sc +
        "\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
        "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
        "UNIT[\"degree\",0.0174532925199433]],PROJECTION[\"Transverse_"
        "Mercator\"],"
        "PARAMETER[\"latitude_of_origin\",0],PARAMETER[\"central_meridian\",0],"
        "PARAMETER[\"scale_factor\",1],PARAMETER[\"false_easting\",0],"
        "PARAMETER[\"false_northing\",0],UNIT[\"metre\",1]]\n";
  }

  VSILFILE* fp = VSIFOpenL(dict_path.c_str(), "wb");
  ASSERT_THAT(fp, ::testing::NotNull());
  VSIFWriteL(wkt_content.data(), 1, wkt_content.size(), fp);
  VSIFCloseL(fp);

  CPLPushFinderLocation(pszDir);
  auto cleanup_finder = absl::MakeCleanup([&] {
    CPLPopFinderLocation();
    VSIUnlink(dict_path.c_str());
    VSIRmdir(pszDir);
  });

  // Find by csName
  EXPECT_EQ(OGRERR_NONE, srs.ImportFromESRIStatePlaneWKT(0, nullptr, nullptr,
                                                         32767, "CS_1010"));
  EXPECT_EQ(OGRERR_FAILURE, srs.ImportFromESRIStatePlaneWKT(
                                0, nullptr, nullptr, 32767, "NONEXISTENT"));

  // Find by pcsCode only
  EXPECT_EQ(OGRERR_NONE, srs.ImportFromESRIStatePlaneWKT(0, nullptr, nullptr,
                                                         2222, nullptr));
  EXPECT_EQ(OGRERR_NONE,
            srs.ImportFromESRIStatePlaneWKT(0, nullptr, "feet", 2222, nullptr));
  EXPECT_EQ(OGRERR_NONE, srs.ImportFromESRIStatePlaneWKT(
                             0, nullptr, "international_feet", 2222, nullptr));

  EXPECT_EQ(OGRERR_NONE, srs.ImportFromESRIStatePlaneWKT(0, nullptr, "meters",
                                                         2867, nullptr));
  EXPECT_EQ(OGRERR_NONE,
            srs.ImportFromESRIStatePlaneWKT(0, nullptr, "feet", 2867, nullptr));

  EXPECT_EQ(OGRERR_NONE, srs.ImportFromESRIStatePlaneWKT(
                             0, nullptr, "international_feet", 2870, nullptr));

  EXPECT_EQ(OGRERR_NONE, srs.ImportFromESRIStatePlaneWKT(0, nullptr, "feet",
                                                         32161, nullptr));
  EXPECT_EQ(OGRERR_NONE, srs.ImportFromESRIStatePlaneWKT(
                             0, nullptr, "international_feet", 32161, nullptr));

  EXPECT_EQ(OGRERR_NONE, srs.ImportFromESRIStatePlaneWKT(0, nullptr, "meters",
                                                         2994, nullptr));

  // Find by all inputs (code and datumName)
  EXPECT_EQ(OGRERR_FAILURE,
            srs.ImportFromESRIStatePlaneWKT(-1, "HARN", nullptr, 0, nullptr));
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE, srs.ImportFromESRIStatePlaneWKT(
                                  3126, nullptr, nullptr, 0, nullptr));
  }
  EXPECT_EQ(OGRERR_NONE, srs.ImportFromESRIStatePlaneWKT(
                             3126, "HARN", "international_feet", 0, nullptr));
  EXPECT_EQ(OGRERR_NONE,
            srs.ImportFromESRIStatePlaneWKT(3126, "HARN", "feet", 0, nullptr));

  EXPECT_EQ(OGRERR_NONE, srs.ImportFromESRIStatePlaneWKT(3126, "NAD83",
                                                         "meters", 0, nullptr));
  EXPECT_EQ(OGRERR_NONE, srs.ImportFromESRIStatePlaneWKT(
                             3126, "NAD83", "international_feet", 0, nullptr));
  EXPECT_EQ(OGRERR_NONE,
            srs.ImportFromESRIStatePlaneWKT(3126, "NAD83", "feet", 0, nullptr));

  EXPECT_EQ(OGRERR_NONE,
            srs.ImportFromESRIStatePlaneWKT(3126, "NAD27", "feet", 0, nullptr));

  EXPECT_EQ(OGRERR_FAILURE, srs.ImportFromESRIStatePlaneWKT(
                                3126, "UNKNOWN", nullptr, 0, nullptr));

  OSRCleanup();
}

}  // namespace
}  // namespace autotest2
