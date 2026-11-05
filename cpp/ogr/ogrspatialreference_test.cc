// Tests ogrspatialreference.cpp.
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include <clocale>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
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

using ::autotest2::IsSameAs;
using ::testing::Not;

TEST(OgrSpatialReferenceTest, NullConstructor) {
  OGRSpatialReference srs(nullptr);
  EXPECT_EQ(1, srs.GetReferenceCount());
  EXPECT_EQ(nullptr, srs.GetRoot());
  srs.Clear();
  EXPECT_EQ(1, srs.GetReferenceCount());
  EXPECT_EQ(nullptr, srs.GetRoot());
  EXPECT_FALSE(srs.IsGeographic());
  EXPECT_FALSE(srs.IsProjected());
  EXPECT_FALSE(srs.IsGeocentric());
  EXPECT_FALSE(srs.IsLocal());
  EXPECT_FALSE(srs.IsVertical());
  EXPECT_FALSE(srs.IsCompound());
}

TEST(OgrSpatialReferenceTest, CStringConstructor) {
  // Test construction with a short char *.
  OGRSpatialReference srs(R"WKT(
      GEOGCS["GCS_WGS_1966",
        DATUM["WGS_1966",
            SPHEROID["WGS_1966",6378145,298.25]],
        PRIMEM["Greenwich",0],
        UNIT["Degree",0.017453292519943295],
        AUTHORITY["EPSG","37001"]])WKT");
  EXPECT_EQ(1, srs.GetReferenceCount());
  EXPECT_NE(nullptr, srs.GetRoot());
}

TEST(OgrSpatialReferenceTest, Wgs84) {
  // Tests the most common coordinate system.
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));  // WGS84
  EXPECT_NE(nullptr, srs.GetRoot());
  EXPECT_EQ(1, srs.GetReferenceCount());

  EXPECT_TRUE(srs.IsGeographic());
  EXPECT_FALSE(srs.IsProjected());
  EXPECT_FALSE(srs.IsGeocentric());
  EXPECT_FALSE(srs.IsLocal());
  EXPECT_FALSE(srs.IsVertical());
  EXPECT_FALSE(srs.IsCompound());
}

TEST(OgrSpatialReferenceTest, ReferenceCounting) {
  // Tests basic operation of the custom ref counting for this class.
  // TODO(schwehr): Figure out what the symmantics for reference counting are.
  OGRSpatialReference srs;
  EXPECT_EQ(1, srs.GetReferenceCount());
  EXPECT_EQ(2, srs.Reference());
  EXPECT_EQ(2, srs.GetReferenceCount());
  EXPECT_EQ(1, srs.Dereference());
  EXPECT_EQ(1, srs.GetReferenceCount());
  // Calling srs.Release() here will crash.
  EXPECT_EQ(0, srs.Dereference());
  EXPECT_EQ(0, srs.GetReferenceCount());
  EXPECT_EQ(-1, srs.Dereference());
  EXPECT_EQ(-1, srs.GetReferenceCount());
  // Calling srs.Release() here will crash.
}

TEST(OgrSpatialReferenceTest, IsSameSphereoidPrecisionCheck) {
  // GDAL 2 rounds the last parameter of the SPHEROID.  Why?
  const std::string wkt_start = R"WKT(
      PROJCS["OSGB 1936 / British National Grid",
          GEOGCS["OSGB 1936",
              DATUM["OSGB_1936",
                  SPHEROID["Airy 1830",6377563.396,299.3249646)WKT";
  const std::string wkt_middle = "000043";
  const std::string wkt_end = R"WKT(
      ,
                      AUTHORITY["EPSG","7001"]],
                  TOWGS84[446.448,-125.157,542.06,0.15,0.247,0.842,-20.489],
                  AUTHORITY["EPSG","6277"]],
              PRIMEM["Greenwich",0,
                  AUTHORITY["EPSG","8901"]],
              UNIT["degree",0.0174532925199433,
                  AUTHORITY["EPSG","9122"]],
              AUTHORITY["EPSG","4277"]],
          PROJECTION["Transverse_Mercator"],
          PARAMETER["latitude_of_origin",49],
          PARAMETER["central_meridian",-2],
          PARAMETER["scale_factor",0.9996012717],
          PARAMETER["false_easting",400000],
          PARAMETER["false_northing",-100000],
          UNIT["metre",1,
              AUTHORITY["EPSG","9001"]],
          AXIS["Easting",EAST],
          AXIS["Northing",NORTH],
          AUTHORITY["EPSG","27700"]])WKT";

  OGRSpatialReference srs_a((wkt_start + wkt_end).c_str());
  OGRSpatialReference srs_b((wkt_start + wkt_middle + wkt_end).c_str());
  EXPECT_THAT(srs_a, IsSameAs(srs_b));
}

TEST(OgrSpatialReferenceTest, Gdal1AndGdal2ProjectionsAreTheSame) {
  const std::string wkt_gdal1 = R"WKT(
      PROJCS["OSGB 1936 / British National Grid",
          GEOGCS["OSGB 1936",
              DATUM["OSGB_1936",
                  SPHEROID["Airy 1830",6377563.396,299.3249646000043,
                      AUTHORITY["EPSG","7001"]],
                  AUTHORITY["EPSG","6277"]],
              PRIMEM["Greenwich",0],
              UNIT["degree",0.0174532925199433],
              AUTHORITY["EPSG","4277"]],
          PROJECTION["Transverse_Mercator"],
          PARAMETER["latitude_of_origin",49],
          PARAMETER["central_meridian",-2],
          PARAMETER["scale_factor",0.9996012717],
          PARAMETER["false_easting",400000],
          PARAMETER["false_northing",-100000],
          UNIT["metre",1,
              AUTHORITY["EPSG","9001"]],
               AUTHORITY["EPSG","27700"]])WKT";

  const std::string wkt_gdal2 = R"WKT(
      PROJCS["OSGB 1936 / British National Grid",
          GEOGCS["OSGB 1936",
              DATUM["OSGB_1936",
                  SPHEROID["Airy 1830",6377563.396,299.3249646,
                      AUTHORITY["EPSG","7001"]],
                  TOWGS84[446.448,-125.157,542.06,0.15,0.247,0.842,-20.489],
                  AUTHORITY["EPSG","6277"]],
              PRIMEM["Greenwich",0,
                  AUTHORITY["EPSG","8901"]],
              UNIT["degree",0.0174532925199433,
                  AUTHORITY["EPSG","9122"]],
              AUTHORITY["EPSG","4277"]],
          PROJECTION["Transverse_Mercator"],
          PARAMETER["latitude_of_origin",49],
          PARAMETER["central_meridian",-2],
          PARAMETER["scale_factor",0.9996012717],
          PARAMETER["false_easting",400000],
          PARAMETER["false_northing",-100000],
          UNIT["metre",1,
              AUTHORITY["EPSG","9001"]],
          AXIS["Easting",EAST],
          AXIS["Northing",NORTH],
          AUTHORITY["EPSG","27700"]])WKT";

  OGRSpatialReference srs_old(wkt_gdal1.c_str());
  OGRSpatialReference srs_new(wkt_gdal2.c_str());

  EXPECT_THAT(srs_new, IsSameAs(srs_old));
}

TEST(OgrSpatialReferenceTest, DifferByProj4Extension) {
  // Test IsSame() on SRS that differs only by their PROJ4 EXTENSION (besides
  // different EPSG codes).
  // Upstream: osr_epsg_12
  // https://trac.osgeo.org/gdal/ticket/7029

  // web_mercator
  OGRSpatialReference srs3857;
  EXPECT_EQ(OGRERR_NONE, srs3857.importFromEPSG(3857));

  // WGS84 Mercator
  OGRSpatialReference srs3395;
  EXPECT_EQ(OGRERR_NONE, srs3395.importFromEPSG(3395));

  EXPECT_THAT(srs3395, Not(IsSameAs(srs3857)));

  EXPECT_THAT(srs3857, IsSameAs(srs3857));
  EXPECT_THAT(srs3395, IsSameAs(srs3395));
}

TEST(OgrSpatialReferenceTest, InvalidCentralMeridian) {
  // Autofuzz POC from b/65413187
  const char kWkt[] = R"wkt(
      PROJCS["unnamed",GEOGCS["WGS 84",DATUM["WGS_1984",
      SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],
      AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],
      UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],
      AUTHORITY["EPSG","4326"]],PROJECTION["Transverse_Mercator"],
      PARAMETER["latitude_of_origin",0],
      PARAMETER["central_meridian",77777777777],
      PARAMETER["scale_factor",0.9996],
      PARAMETER["false_easting","nan"],
      PARAMETER["false_northing",0],UNIT["Go]",1]])wkt";
  OGRSpatialReference srs(kWkt);
  int north = TRUE;
  EXPECT_EQ(0, srs.GetUTMZone(&north));

  // This formerly triggered an ASAN violation.
  srs.morphToESRI();
}

TEST(OgrSpatialReferenceTest, ConstructorsAndBasics) {
  OGRSpatialReference srs1;
  EXPECT_EQ(OGRERR_NONE, srs1.importFromEPSG(4326));
  EXPECT_STREQ("WGS 84", srs1.GetName());
  EXPECT_STREQ("Earth", srs1.GetCelestialBodyName());

  // Copy constructor and copy assignment
  OGRSpatialReference srs2(srs1);
  EXPECT_STREQ("WGS 84", srs2.GetName());

  OGRSpatialReference srs3;
  srs3 = srs1;
  EXPECT_STREQ("WGS 84", srs3.GetName());

  // Move constructor and move assignment
  OGRSpatialReference srs4(std::move(srs2));
  EXPECT_STREQ("WGS 84", srs4.GetName());

  OGRSpatialReference srs5;
  srs5 = std::move(srs3);
  EXPECT_STREQ("WGS 84", srs5.GetName());

  // Clone
  std::unique_ptr<OGRSpatialReference> clone(srs1.Clone());
  ASSERT_THAT(clone, testing::NotNull());
  EXPECT_STREQ("WGS 84", clone->GetName());

  // dumpReadable
  srs1.dumpReadable();

  // Validate
  EXPECT_EQ(OGRERR_NONE, srs1.Validate());

  // SetCoordinateEpoch and GetCoordinateEpoch
  srs1.SetCoordinateEpoch(2020.5);
  EXPECT_DOUBLE_EQ(2020.5, srs1.GetCoordinateEpoch());

  // PromoteTo3D and DemoteTo2D
  EXPECT_EQ(OGRERR_NONE, srs1.PromoteTo3D(nullptr));
  EXPECT_EQ(3, srs1.GetAxesCount());
  EXPECT_EQ(OGRERR_NONE, srs1.DemoteTo2D(nullptr));
  EXPECT_EQ(2, srs1.GetAxesCount());

  // StripNodes and StripVertical
  if (srs1.GetRoot() != nullptr) {
    srs1.GetRoot()->StripNodes("AUTHORITY");
  }
  EXPECT_EQ(nullptr, srs1.GetAuthorityCode(nullptr));
  EXPECT_EQ(OGRERR_NONE, srs1.StripVertical());
  EXPECT_FALSE(srs1.StripTOWGS84IfKnownDatum());

  // GetAttrNode and GetAttrValue
  EXPECT_NE(nullptr, srs1.GetAttrNode("GEOGCS"));
  EXPECT_STREQ("WGS 84", srs1.GetAttrValue("GEOGCS"));
  EXPECT_EQ(nullptr, srs1.GetAttrNode("NON_EXISTING_NODE"));
  EXPECT_EQ(nullptr, srs1.GetAttrValue("NON_EXISTING_NODE"));

  // SetNode
  EXPECT_EQ(OGRERR_NONE, srs1.SetNode("GEOGCS|DATUM", "WGS_1984_TEST"));
  EXPECT_STREQ("WGS_1984_TEST", srs1.GetAttrValue("GEOGCS|DATUM"));
}

TEST(OgrSpatialReferenceTest, AxisAndUnitsManagement) {
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));

  // Area of Use
  double west = 0, south = 0, east = 0, north = 0;
  const char* pszAreaName = nullptr;
  EXPECT_TRUE(srs.GetAreaOfUse(&west, &south, &east, &north, &pszAreaName));
  EXPECT_NE(nullptr, pszAreaName);

  // Axes count and getting axes
  EXPECT_EQ(2, srs.GetAxesCount());
  OGRAxisOrientation orient = OAO_Other;
  const char* pszAxis0 = srs.GetAxis(nullptr, 0, &orient);
  EXPECT_NE(nullptr, pszAxis0);

  const char* pszAxis1 = srs.GetAxis("GEOGCS", 1, &orient);
  EXPECT_NE(nullptr, pszAxis1);
  EXPECT_EQ(nullptr, srs.GetAxis(nullptr, 10, &orient));
  EXPECT_NE(nullptr, OSRAxisEnumToName(orient));

  // SetAxes
  EXPECT_EQ(OGRERR_NONE,
            srs.SetAxes("GEOGCS", "Lat", OAO_North, "Long", OAO_East));

  // AxisMappingStrategy
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_EQ(OAMS_TRADITIONAL_GIS_ORDER, srs.GetAxisMappingStrategy());
  srs.SetAxisMappingStrategy(OAMS_AUTHORITY_COMPLIANT);
  EXPECT_EQ(OAMS_AUTHORITY_COMPLIANT, srs.GetAxisMappingStrategy());

  // DataAxisToSRSAxisMapping
  std::vector<int> mapping = srs.GetDataAxisToSRSAxisMapping();
  EXPECT_FALSE(mapping.empty());
  EXPECT_EQ(OGRERR_NONE, srs.SetDataAxisToSRSAxisMapping({2, 1}));

  // Units
  char* pszUnitName = nullptr;
  double dfScale = srs.GetAngularUnits(&pszUnitName);
  EXPECT_GT(dfScale, 0.0);
  EXPECT_EQ(OGRERR_NONE, srs.SetAngularUnits("degree", 0.0174532925199433));

  OGRSpatialReference srsProj;
  EXPECT_EQ(OGRERR_NONE, srsProj.importFromEPSG(3857));
  dfScale = srsProj.GetLinearUnits(&pszUnitName);
  EXPECT_DOUBLE_EQ(1.0, dfScale);
  EXPECT_EQ(OGRERR_NONE, srsProj.SetLinearUnits("metre", 1.0));
  EXPECT_EQ(OGRERR_NONE,
            srsProj.SetLinearUnitsAndUpdateParameters("metre", 1.0));
  EXPECT_EQ(OGRERR_NONE, srsProj.SetTargetLinearUnits("PROJCS", "metre", 1.0));

  // Ellipsoid parameters
  OGRErr eErr = OGRERR_NONE;
  double dfSemiMajor = srs.GetSemiMajor(&eErr);
  EXPECT_EQ(OGRERR_NONE, eErr);
  EXPECT_NEAR(6378137.0, dfSemiMajor, 1.0);
  double dfSemiMinor = srs.GetSemiMinor(&eErr);
  EXPECT_EQ(OGRERR_NONE, eErr);
  EXPECT_NEAR(6356752.3, dfSemiMinor, 1.0);
  double dfInvFlattening = srs.GetInvFlattening(&eErr);
  EXPECT_EQ(OGRERR_NONE, eErr);
  EXPECT_NEAR(298.257, dfInvFlattening, 0.1);

  // TOWGS84
  EXPECT_EQ(OGRERR_NONE, srs.SetTOWGS84(0, 0, 0, 0, 0, 0, 0));
  double adfCoeff[7] = {};
  EXPECT_EQ(OGRERR_NONE, srs.GetTOWGS84(adfCoeff, 7));

  // Authority
  EXPECT_EQ(OGRERR_NONE, srs.SetAuthority("GEOGCS", "EPSG", 4326));
  EXPECT_STREQ("4326", srs.GetAuthorityCode("GEOGCS"));
  EXPECT_STREQ("EPSG", srs.GetAuthorityName("GEOGCS"));
  EXPECT_EQ(nullptr, srs.GetAuthorityCode("INVALID_KEY"));
  EXPECT_EQ(nullptr, srs.GetAuthorityName("INVALID_KEY"));

  // EPSGTreatsAsLatLong and NorthingEasting
  EXPECT_TRUE(srs.EPSGTreatsAsLatLong());
  EXPECT_FALSE(srs.EPSGTreatsAsNorthingEasting());
}

TEST(OgrSpatialReferenceTest, SetFromUserInputAndFormats) {
  const char* const inputs[] = {
      "EPSG:4326",
      "EPSGA:4326",
      "WGS84",
      "WGS72",
      "NAD27",
      "NAD83",
      "urn:ogc:def:crs:EPSG::4326",
      "http://www.opengis.net/def/crs/EPSG/0/4326",
      "+proj=longlat +datum=WGS84 +no_defs",
      "+init=epsg:4326",
      "CRS:84",
  };

  for (const char* pszInput : inputs) {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.SetFromUserInput(pszInput)) << pszInput;
    EXPECT_TRUE(srs.IsGeographic());
  }

  // Error case
  OGRSpatialReference srsErr;
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_NE(OGRERR_NONE, srsErr.SetFromUserInput("NOT_A_VALID_CRS"));
  }

  // SetWellKnownGeogCS
  const char* const wellKnown[] = {"WGS84", "WGS72", "NAD27", "NAD83",
                                   "EPSG:4326"};
  for (const char* pszWK : wellKnown) {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.SetWellKnownGeogCS(pszWK));
    EXPECT_TRUE(srs.IsGeographic());
  }

  // Exports and imports
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));

  // WKT exports
  char* pszWkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, srs.exportToWkt(&pszWkt));
  ASSERT_THAT(pszWkt, testing::NotNull());
  CPLFree(pszWkt);

  EXPECT_EQ(OGRERR_NONE, srs.exportToPrettyWkt(&pszWkt, FALSE));
  ASSERT_THAT(pszWkt, testing::NotNull());
  CPLFree(pszWkt);

  const char* const papszWktOpts[] = {"FORMAT=WKT2_2019", nullptr};
  EXPECT_EQ(OGRERR_NONE, srs.exportToWkt(&pszWkt, papszWktOpts));
  ASSERT_THAT(pszWkt, testing::NotNull());
  CPLFree(pszWkt);

  // PROJJSON
  EXPECT_EQ(OGRERR_NONE, srs.exportToPROJJSON(&pszWkt, nullptr));
  ASSERT_THAT(pszWkt, testing::NotNull());
  OGRSpatialReference srsPJ;
  EXPECT_EQ(OGRERR_NONE, srsPJ.SetFromUserInput(pszWkt));
  EXPECT_TRUE(srsPJ.IsGeographic());
  CPLFree(pszWkt);

  // Proj4
  char* pszProj4 = nullptr;
  EXPECT_EQ(OGRERR_NONE, srs.exportToProj4(&pszProj4));
  ASSERT_THAT(pszProj4, testing::NotNull());
  OGRSpatialReference srsP4;
  EXPECT_EQ(OGRERR_NONE, srsP4.importFromProj4(pszProj4));
  CPLFree(pszProj4);

  // XML
  char* pszXML = nullptr;
  EXPECT_EQ(OGRERR_NONE, srs.exportToXML(&pszXML));
  ASSERT_THAT(pszXML, testing::NotNull());
  OGRSpatialReference srsXML;
  EXPECT_EQ(OGRERR_NONE, srsXML.importFromXML(pszXML));
  CPLFree(pszXML);

  // PCI
  char* pszProjPCI = nullptr;
  char* pszUnitsPCI = nullptr;
  double* padfParamsPCI = nullptr;
  EXPECT_EQ(OGRERR_NONE,
            srs.exportToPCI(&pszProjPCI, &pszUnitsPCI, &padfParamsPCI));
  OGRSpatialReference srsPCI;
  EXPECT_EQ(OGRERR_NONE,
            srsPCI.importFromPCI(pszProjPCI, pszUnitsPCI, padfParamsPCI));
  CPLFree(pszProjPCI);
  CPLFree(pszUnitsPCI);
  CPLFree(padfParamsPCI);

  // MapInfo
  char* pszMI = nullptr;
  EXPECT_EQ(OGRERR_NONE, srs.exportToMICoordSys(&pszMI));
  if (pszMI) {
    OGRSpatialReference srsMI;
    srsMI.importFromMICoordSys(pszMI);
    CPLFree(pszMI);
  }

  // Ozi
  const char* const papszOzi[] = {"OziExplorer Map Data File Version 2.2",
                                  "Map",
                                  "Image",
                                  "1 ,Map Code,",
                                  "WGS 84",
                                  "Map Projection,Latitude/Longitude,",
                                  "Projection Setup,0,0,0,0,0",
                                  nullptr};
  OGRSpatialReference srsOzi;
  EXPECT_EQ(OGRERR_NONE, srsOzi.importFromOzi(papszOzi));

  // ESRI morphing
  EXPECT_EQ(OGRERR_NONE, srs.morphToESRI());
  EXPECT_EQ(OGRERR_NONE, srs.morphFromESRI());

  // FindMatches
  int nEntries = 0;
  int* panConf = nullptr;
  OGRSpatialReferenceH* pahMatches =
      srs.FindMatches(nullptr, &nEntries, &panConf);
  EXPECT_GE(nEntries, 1);
  OSRFreeSRSArray(pahMatches);
  CPLFree(panConf);

  std::unique_ptr<OGRSpatialReference> bestMatch(srs.FindBestMatch(80));
  EXPECT_NE(nullptr, bestMatch);

  // AutoIdentifyEPSG
  EXPECT_EQ(OGRERR_NONE, srs.AutoIdentifyEPSG());
}

TEST(OgrSpatialReferenceTest, AllProjections) {
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");

  EXPECT_EQ(OGRERR_NONE, srs.SetUTM(10, TRUE));
  int bNorth = FALSE;
  EXPECT_EQ(10, srs.GetUTMZone(&bNorth));
  EXPECT_TRUE(bNorth);

  EXPECT_EQ(OGRERR_NONE, srs.SetStatePlane(1201, TRUE));
  EXPECT_EQ(OGRERR_NONE, srs.SetACEA(29.5, 45.5, 23.0, -96.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetAE(40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetBonne(40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetCEA(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetCS(40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEC(29.5, 45.5, 23.0, -96.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEckertIV(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEckertVI(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEquirectangular(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEquirectangular2(0.0, 0.0, 10.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetGS(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetGH(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetGEOS(0.0, 35785831.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            srs.SetGaussSchreiberTMercator(40.0, -100.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetGnomonic(40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetHOM(40.0, -100.0, 45.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            srs.SetHOM2PNO(40.0, -100.0, -90.0, 45.0, 45.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            srs.SetKrovak(49.5, 24.833, 30.288, 78.5, 0.9999, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetLAEA(40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetLCC(30.0, 35.0, 25.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetLCC1SP(25.0, -100.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetLCCB(30.0, 35.0, 25.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetMC(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetMercator(0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetMercator2SP(40.0, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetMollweide(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetNZMG(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            srs.SetOS(49.0, -2.0, 0.9996012717, 400000.0, -100000.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetOrthographic(40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetPolyconic(40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetPS(90.0, -100.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetRobinson(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetSinusoidal(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetStereographic(40.0, -100.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetSOC(40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetTM(0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            srs.SetTMVariant("Transverse_Mercator", 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetTMSO(0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetVDG(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetWagner(1, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetQSC(40.0, -100.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetSCH(40.0, -100.0, 45.0, 6378137.0));

  // Projection parameters
  EXPECT_EQ(OGRERR_NONE, srs.SetProjParm(SRS_PP_CENTRAL_MERIDIAN, -100.0));
  OGRErr eErr = OGRERR_NONE;
  EXPECT_DOUBLE_EQ(-100.0,
                   srs.GetProjParm(SRS_PP_CENTRAL_MERIDIAN, 0.0, &eErr));
  EXPECT_EQ(OGRERR_NONE, eErr);

  EXPECT_EQ(OGRERR_NONE, srs.SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, -100.0));
  EXPECT_DOUBLE_EQ(-100.0,
                   srs.GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, 0.0, &eErr));
  EXPECT_EQ(OGRERR_NONE, eErr);
}

TEST(OgrSpatialReferenceTest, CompoundLocalGeocentricVerticalAndQueries) {
  // Local CS
  OGRSpatialReference srsLocal;
  EXPECT_EQ(OGRERR_NONE, srsLocal.SetLocalCS("MyLocalCS"));
  EXPECT_TRUE(srsLocal.IsLocal());
  EXPECT_FALSE(srsLocal.IsGeographic());
  EXPECT_FALSE(srsLocal.IsProjected());

  // Geocentric CS
  OGRSpatialReference srsGeoc;
  EXPECT_EQ(OGRERR_NONE, srsGeoc.SetGeocCS("MyGeocCS"));
  EXPECT_TRUE(srsGeoc.IsGeocentric());

  // Vertical CS
  OGRSpatialReference srsVert;
  EXPECT_EQ(
      OGRERR_NONE,
      srsVert.SetVertCS("NAVD88 height", "North American Vertical Datum 1988"));
  EXPECT_TRUE(srsVert.IsVertical());

  // Compound CS
  OGRSpatialReference srsHoriz;
  EXPECT_EQ(OGRERR_NONE, srsHoriz.importFromEPSG(4326));
  OGRSpatialReference srsCompound;
  EXPECT_EQ(OGRERR_NONE,
            srsCompound.SetCompoundCS("WGS84 + NAVD88", &srsHoriz, &srsVert));
  EXPECT_TRUE(srsCompound.IsCompound());

  // Query functions
  EXPECT_TRUE(srsCompound.IsDynamic());
  EXPECT_FALSE(srsCompound.IsDerivedGeographic());
  EXPECT_FALSE(srsCompound.IsDerivedProjected());

  // IsSameGeogCS and IsSameVertCS
  OGRSpatialReference srsHoriz2;
  EXPECT_EQ(OGRERR_NONE, srsHoriz2.importFromEPSG(4326));
  EXPECT_TRUE(srsHoriz.IsSameGeogCS(&srsHoriz2));
  EXPECT_TRUE(srsVert.IsSameVertCS(&srsVert));
}

TEST(OgrSpatialReferenceTest, CAPISpatialReference) {
  OGRSpatialReferenceH hSRS = OSRNewSpatialReference(nullptr);
  ASSERT_THAT(hSRS, testing::NotNull());
  EXPECT_EQ(1, OGRSpatialReference::FromHandle(hSRS)->GetReferenceCount());
  EXPECT_EQ(2, OSRReference(hSRS));
  EXPECT_EQ(1, OSRDereference(hSRS));

  EXPECT_EQ(OGRERR_NONE, OSRImportFromEPSG(hSRS, 4326));
  EXPECT_TRUE(OSRIsGeographic(hSRS));
  EXPECT_FALSE(OSRIsProjected(hSRS));
  EXPECT_FALSE(OSRIsGeocentric(hSRS));
  EXPECT_FALSE(OSRIsVertical(hSRS));
  EXPECT_FALSE(OSRIsCompound(hSRS));
  EXPECT_FALSE(OSRIsLocal(hSRS));

  OGRSpatialReferenceH hClone = OSRClone(hSRS);
  ASSERT_THAT(hClone, testing::NotNull());
  EXPECT_TRUE(OSRIsSame(hSRS, hClone));
  EXPECT_TRUE(OSRIsSameGeogCS(hSRS, hClone));
  EXPECT_FALSE(OSRIsSameVertCS(hSRS, hClone));
  OSRDestroySpatialReference(hClone);

  // WKT and PROJJSON C API
  char* pszWkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, OSRExportToWkt(hSRS, &pszWkt));
  ASSERT_THAT(pszWkt, testing::NotNull());
  CPLFree(pszWkt);

  EXPECT_EQ(OGRERR_NONE, OSRExportToPrettyWkt(hSRS, &pszWkt, FALSE));
  ASSERT_THAT(pszWkt, testing::NotNull());
  CPLFree(pszWkt);

  EXPECT_EQ(OGRERR_NONE, OSRExportToPROJJSON(hSRS, &pszWkt, nullptr));
  ASSERT_THAT(pszWkt, testing::NotNull());
  CPLFree(pszWkt);

  // Proj4 C API
  char* pszProj4 = nullptr;
  EXPECT_EQ(OGRERR_NONE, OSRExportToProj4(hSRS, &pszProj4));
  ASSERT_THAT(pszProj4, testing::NotNull());
  EXPECT_EQ(OGRERR_NONE, OSRImportFromProj4(hSRS, pszProj4));
  CPLFree(pszProj4);

  // SetFromUserInput and SetWellKnownGeogCS C API
  EXPECT_EQ(OGRERR_NONE, OSRSetWellKnownGeogCS(hSRS, "WGS84"));
  EXPECT_EQ(OGRERR_NONE, OSRSetFromUserInput(hSRS, "EPSG:3857"));
  EXPECT_TRUE(OSRIsProjected(hSRS));

  // Axes C API
  EXPECT_EQ(2, OSRGetAxesCount(hSRS));
  OGRAxisOrientation orient = OAO_Other;
  EXPECT_NE(nullptr, OSRGetAxis(hSRS, nullptr, 0, &orient));
  OSRSetAxisMappingStrategy(hSRS, OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_EQ(OAMS_TRADITIONAL_GIS_ORDER, OSRGetAxisMappingStrategy(hSRS));

  // Projections C API
  EXPECT_EQ(OGRERR_NONE, OSRSetUTM(hSRS, 11, TRUE));
  int bNorth = FALSE;
  EXPECT_EQ(11, OSRGetUTMZone(hSRS, &bNorth));
  EXPECT_TRUE(bNorth);
  EXPECT_EQ(OGRERR_NONE, OSRSetStatePlane(hSRS, 1201, TRUE));

  EXPECT_EQ(OGRERR_NONE, OSRSetACEA(hSRS, 29.5, 45.5, 23.0, -96.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetAE(hSRS, 40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetBonne(hSRS, 40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetCEA(hSRS, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetCS(hSRS, 40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetEC(hSRS, 29.5, 45.5, 23.0, -96.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetEckertIV(hSRS, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetEckertVI(hSRS, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetEquirectangular(hSRS, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetEquirectangular2(hSRS, 0.0, 0.0, 10.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetGS(hSRS, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetGH(hSRS, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetGEOS(hSRS, 0.0, 35785831.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetGaussSchreiberTMercator(hSRS, 40.0, -100.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetGnomonic(hSRS, 40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetHOM(hSRS, 40.0, -100.0, 45.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetHOM2PNO(hSRS, 40.0, -100.0, -90.0, 45.0, 45.0,
                                       1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetKrovak(hSRS, 49.5, 24.833, 30.288, 78.5, 0.9999, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetLAEA(hSRS, 40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetLCC(hSRS, 30.0, 35.0, 25.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetLCC1SP(hSRS, 25.0, -100.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetLCCB(hSRS, 30.0, 35.0, 25.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetMC(hSRS, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetMercator(hSRS, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetMercator2SP(hSRS, 40.0, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetMollweide(hSRS, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetNZMG(hSRS, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetOS(hSRS, 49.0, -2.0, 0.9996012717, 400000.0, -100000.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetOrthographic(hSRS, 40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetPolyconic(hSRS, 40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetPS(hSRS, 90.0, -100.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetRobinson(hSRS, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetSinusoidal(hSRS, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetStereographic(hSRS, 40.0, -100.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetSOC(hSRS, 40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetTM(hSRS, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetTMVariant(hSRS, "Transverse_Mercator", 0.0, 0.0,
                                         1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetTMSO(hSRS, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetVDG(hSRS, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetWagner(hSRS, 1, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetQSC(hSRS, 40.0, -100.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetSCH(hSRS, 40.0, -100.0, 45.0, 6378137.0));

  // Parameters C API
  EXPECT_EQ(OGRERR_NONE, OSRSetProjParm(hSRS, SRS_PP_CENTRAL_MERIDIAN, -100.0));
  OGRErr eErr = OGRERR_NONE;
  EXPECT_DOUBLE_EQ(-100.0,
                   OSRGetProjParm(hSRS, SRS_PP_CENTRAL_MERIDIAN, 0.0, &eErr));
  EXPECT_EQ(OGRERR_NONE, eErr);

  EXPECT_EQ(OGRERR_NONE,
            OSRSetNormProjParm(hSRS, SRS_PP_CENTRAL_MERIDIAN, -100.0));
  EXPECT_DOUBLE_EQ(
      -100.0, OSRGetNormProjParm(hSRS, SRS_PP_CENTRAL_MERIDIAN, 0.0, &eErr));
  EXPECT_EQ(OGRERR_NONE, eErr);

  // Validate C API
  OGRSpatialReferenceH hEPSG = OSRNewSpatialReference(nullptr);
  EXPECT_EQ(OGRERR_NONE, OSRImportFromEPSG(hEPSG, 4326));
  EXPECT_EQ(OGRERR_NONE, OSRValidate(hEPSG));
  OSRDestroySpatialReference(hEPSG);

  // Database info list C API
  int nCRSCount = 0;
  OSRCRSInfo** papszCRSList =
      OSRGetCRSInfoListFromDatabase("EPSG", nullptr, &nCRSCount);
  EXPECT_GT(nCRSCount, 0);
  OSRDestroyCRSInfoList(papszCRSList);

  char** papszAuthList = OSRGetAuthorityListFromDatabase();
  ASSERT_THAT(papszAuthList, testing::NotNull());
  CSLDestroy(papszAuthList);

  // Null safety C API
  EXPECT_EQ(nullptr, OSRClone(nullptr));
  EXPECT_EQ(0, OSRReference(nullptr));
  EXPECT_EQ(0, OSRDereference(nullptr));
  OSRDestroySpatialReference(nullptr);
  EXPECT_FALSE(OSRIsGeographic(nullptr));
  EXPECT_FALSE(OSRIsProjected(nullptr));
  EXPECT_FALSE(OSRIsGeocentric(nullptr));
  EXPECT_FALSE(OSRIsVertical(nullptr));
  EXPECT_FALSE(OSRIsCompound(nullptr));
  EXPECT_FALSE(OSRIsLocal(nullptr));
  EXPECT_FALSE(OSRIsSame(nullptr, nullptr));
  EXPECT_FALSE(OSRIsSameGeogCS(nullptr, nullptr));
  EXPECT_FALSE(OSRIsSameVertCS(nullptr, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OSRValidate(nullptr));
  EXPECT_EQ(0, OSRGetAxesCount(nullptr));
  EXPECT_EQ(nullptr, OSRGetAxis(nullptr, nullptr, 0, nullptr));
  EXPECT_EQ(OAMS_CUSTOM, OSRGetAxisMappingStrategy(nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OSRExportToWkt(nullptr, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OSRExportToPrettyWkt(nullptr, nullptr, FALSE));
  EXPECT_EQ(OGRERR_FAILURE, OSRExportToPROJJSON(nullptr, nullptr, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OSRExportToProj4(nullptr, nullptr));

  OSRDestroySpatialReference(hSRS);
}

TEST(OgrSpatialReferenceTest, AllProjectionsExtended) {
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");

  EXPECT_EQ(OGRERR_NONE, srs.SetTPED(10.0, 20.0, 30.0, 40.0, 100.0, 200.0));
  EXPECT_TRUE(srs.IsProjected());

  EXPECT_EQ(OGRERR_NONE, srs.SetTMG(35.0, 33.0, 1000.0, 2000.0));
  EXPECT_TRUE(srs.IsProjected());

  EXPECT_EQ(OGRERR_NONE, srs.SetEckert(1, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEckert(2, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEckert(3, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEckert(4, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEckert(5, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEckert(6, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEckertIV(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEckertVI(0.0, 0.0, 0.0));

  EXPECT_EQ(OGRERR_NONE, srs.SetIGH());
  EXPECT_TRUE(srs.IsProjected());

  EXPECT_EQ(OGRERR_NONE,
            srs.SetHOMAC(4.0, 115.0, 53.0, 53.0, 0.99984, 0.0, 0.0));
  EXPECT_TRUE(srs.IsProjected());

  EXPECT_EQ(OGRERR_NONE, srs.SetLOM(4.0, 115.0, 53.0, 0.99984, 0.0, 0.0));
  EXPECT_TRUE(srs.IsProjected());

  EXPECT_EQ(OGRERR_NONE, srs.SetIWMPolyconic(40.0, 60.0, 10.0, 0.0, 0.0));
  EXPECT_TRUE(srs.IsProjected());

  EXPECT_EQ(OGRERR_NONE,
            srs.SetVerticalPerspective(0.0, 0.0, 0.0, 35785831.0, 0.0, 0.0));
  EXPECT_TRUE(srs.IsProjected());

  OGRSpatialReference srsGeog;
  EXPECT_EQ(OGRERR_NONE, srsGeog.importFromEPSG(4326));
  EXPECT_EQ(OGRERR_NONE,
            srsGeog.SetDerivedGeogCRSWithPoleRotationGRIBConvention(
                "Rotated GRIB", -30.0, 10.0, 0.0));
  EXPECT_TRUE(srsGeog.IsDerivedGeographic());

  OGRSpatialReference srsGeogCF;
  EXPECT_EQ(OGRERR_NONE, srsGeogCF.importFromEPSG(4326));
  EXPECT_EQ(OGRERR_NONE,
            srsGeogCF.SetDerivedGeogCRSWithPoleRotationNetCDFCFConvention(
                "Rotated NetCDF", 60.0, 10.0, 0.0));
  EXPECT_TRUE(srsGeogCF.IsDerivedGeographic());

  // C API projection calls
  OGRSpatialReferenceH hSRS = OSRNewSpatialReference(nullptr);
  OSRSetWellKnownGeogCS(hSRS, "WGS84");
  EXPECT_EQ(OGRERR_NONE,
            OSRSetTPED(hSRS, 10.0, 20.0, 30.0, 40.0, 100.0, 200.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetTMG(hSRS, 35.0, 33.0, 1000.0, 2000.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetEckert(hSRS, 4, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetEckertIV(hSRS, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetEckertVI(hSRS, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetIGH(hSRS));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetHOMAC(hSRS, 4.0, 115.0, 53.0, 53.0, 0.99984, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetIWMPolyconic(hSRS, 40.0, 60.0, 10.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetVerticalPerspective(hSRS, 0.0, 0.0, 0.0,
                                                   35785831.0, 0.0, 0.0));
  OSRDestroySpatialReference(hSRS);
}

TEST(OgrSpatialReferenceTest, WktExportOptionsAndConversions) {
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(32632));

  // Various exportToWkt options
  const char* const formats[] = {"WKT1_GDAL", "WKT1_ESRI", "WKT2_2015",
                                 "WKT2_2018", "WKT2_2019"};
  for (const char* fmt : formats) {
    const char* const options[] = {"FORMAT", fmt, "MULTILINE", "YES", nullptr};
    char* pszWkt = nullptr;
    EXPECT_EQ(OGRERR_NONE, srs.exportToWkt(&pszWkt, options));
    EXPECT_NE(nullptr, pszWkt);
    CPLFree(pszWkt);
  }

  // OSRExportToWktEx
  OGRSpatialReferenceH hSRS = OGRSpatialReference::ToHandle(&srs);
  const char* const opts[] = {"FORMAT=WKT2_2019", "MULTILINE=NO", nullptr};
  char* pszResult = nullptr;
  EXPECT_EQ(OGRERR_NONE, OSRExportToWktEx(hSRS, &pszResult, opts));
  EXPECT_NE(nullptr, pszResult);
  CPLFree(pszResult);

  // importFromWkt variations
  char* pszWktRaw = nullptr;
  EXPECT_EQ(OGRERR_NONE, srs.exportToWkt(&pszWktRaw));
  OGRSpatialReference srsFromWkt;
  EXPECT_EQ(OGRERR_NONE, srsFromWkt.importFromWkt(pszWktRaw));

  char* pszWktMut = CPLStrdup(pszWktRaw);
  char* pszPtr = pszWktMut;
  OGRSpatialReference srsFromWktPtr;
  EXPECT_EQ(OGRERR_NONE, srsFromWktPtr.importFromWkt(&pszPtr));
  CPLFree(pszWktMut);
  CPLFree(pszWktRaw);

  OGRSpatialReferenceH hImport = OSRNewSpatialReference(nullptr);
  const char* kSimpleWkt =
      "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
      "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0."
      "0174532925199433]]";
  char* pszWktC = CPLStrdup(kSimpleWkt);
  char* pszWktCPtr = pszWktC;
  EXPECT_EQ(OGRERR_NONE, OSRImportFromWkt(hImport, &pszWktCPtr));
  CPLFree(pszWktC);
  OSRDestroySpatialReference(hImport);

  // Error handling in importFromWkt
  OGRSpatialReference srsBad;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srsBad.importFromWkt("INVALID_WKT_STRING"));
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srsBad.importFromWkt(""));

  // convertToOtherProjection
  OGRSpatialReference srsMerc1;
  srsMerc1.SetWellKnownGeogCS("WGS84");
  srsMerc1.SetMercator(0.0, 0.0, 1.0, 0.0, 0.0);
  std::unique_ptr<OGRSpatialReference> srsMerc2(
      srsMerc1.convertToOtherProjection(SRS_PT_MERCATOR_2SP, nullptr));
  ASSERT_THAT(srsMerc2, testing::NotNull());
  std::unique_ptr<OGRSpatialReference> srsMercBack(
      srsMerc2->convertToOtherProjection(SRS_PT_MERCATOR_1SP, nullptr));
  ASSERT_THAT(srsMercBack, testing::NotNull());

  OGRSpatialReference srsLCC1;
  srsLCC1.SetWellKnownGeogCS("WGS84");
  srsLCC1.SetLCC1SP(25.0, -100.0, 1.0, 0.0, 0.0);
  std::unique_ptr<OGRSpatialReference> srsLCC2(srsLCC1.convertToOtherProjection(
      SRS_PT_LAMBERT_CONFORMAL_CONIC_2SP, nullptr));
  ASSERT_THAT(srsLCC2, testing::NotNull());
  std::unique_ptr<OGRSpatialReference> srsLCCBack(
      srsLCC2->convertToOtherProjection(SRS_PT_LAMBERT_CONFORMAL_CONIC_1SP,
                                        nullptr));
  ASSERT_THAT(srsLCCBack, testing::NotNull());

  OGRSpatialReferenceH hMerc1 = OGRSpatialReference::ToHandle(&srsMerc1);
  OGRSpatialReferenceH hConverted =
      OSRConvertToOtherProjection(hMerc1, SRS_PT_MERCATOR_2SP, nullptr);
  ASSERT_THAT(hConverted, testing::NotNull());
  OSRDestroySpatialReference(hConverted);

  // GetOGCURN
  OGRSpatialReference srsURN;
  EXPECT_EQ(OGRERR_NONE, srsURN.importFromEPSG(4326));
  char* pszURN = srsURN.GetOGCURN();
  EXPECT_NE(nullptr, pszURN);
  CPLFree(pszURN);

  // Compound URN
  OGRSpatialReference srsHoriz, srsVert, srsComp;
  srsHoriz.importFromEPSG(4326);
  srsVert.importFromEPSG(5703);
  srsComp.SetCompoundCS("Comp", &srsHoriz, &srsVert);
  char* pszCompURN = srsComp.GetOGCURN();
  if (pszCompURN) {
    CPLFree(pszCompURN);
  }

  // importFromURN
  OGRSpatialReference srsFromURN;
  EXPECT_EQ(OGRERR_NONE,
            srsFromURN.importFromURN("urn:ogc:def:crs:EPSG::4326"));

  // importFromUrl and OSRImportFromUrl
  OGRSpatialReference srsUrl;
  EXPECT_EQ(OGRERR_FAILURE, srsUrl.importFromUrl("not_a_url"));
  EXPECT_EQ(OGRERR_FAILURE, srsUrl.importFromUrl("http://example.com/srs"));
  OGRSpatialReferenceH hUrl = OSRNewSpatialReference(nullptr);
  EXPECT_EQ(OGRERR_FAILURE, OSRImportFromUrl(hUrl, "http://example.com/srs"));
  OSRDestroySpatialReference(hUrl);

  // importFromWMSAUTO
  OGRSpatialReference srsWMS;
  EXPECT_EQ(OGRERR_NONE, srsWMS.importFromWMSAUTO("AUTO:42001,99,8888"));

  // PROJJSON import/export
  char* pszProjJson = nullptr;
  EXPECT_EQ(OGRERR_NONE, srsURN.exportToPROJJSON(&pszProjJson, nullptr));
  ASSERT_THAT(pszProjJson, testing::NotNull());
  OGRSpatialReference srsProjJson;
  EXPECT_EQ(OGRERR_NONE, srsProjJson.SetFromUserInput(pszProjJson));
  CPLFree(pszProjJson);
}

TEST(OgrSpatialReferenceTest, UnitsParametersAndDatum) {
  OGRSpatialReferenceH hSRS = OSRNewSpatialReference(nullptr);
  EXPECT_EQ(OGRERR_NONE, OSRSetWellKnownGeogCS(hSRS, "WGS84"));

  // Angular units C API
  EXPECT_EQ(OGRERR_NONE,
            OSRSetAngularUnits(hSRS, "degree", 0.0174532925199433));
  char* pszUnit = nullptr;
  EXPECT_GT(OSRGetAngularUnits(hSRS, &pszUnit), 0.0);

  // Linear units C API
  EXPECT_EQ(OGRERR_NONE, OSRSetLinearUnits(hSRS, "metre", 1.0));
  EXPECT_DOUBLE_EQ(1.0, OSRGetLinearUnits(hSRS, &pszUnit));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetLinearUnitsAndUpdateParameters(hSRS, "foot", 0.3048));

  OGRSpatialReferenceH hProj = OSRNewSpatialReference(nullptr);
  EXPECT_EQ(OGRERR_NONE, OSRImportFromEPSG(hProj, 32632));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetTargetLinearUnits(hProj, "PROJCS", "metre", 1.0));
  EXPECT_DOUBLE_EQ(1.0, OSRGetTargetLinearUnits(hProj, "PROJCS", &pszUnit));
  EXPECT_DOUBLE_EQ(1.0,
                   OGRSpatialReference::FromHandle(hProj)->GetTargetLinearUnits(
                       "PROJCS", &pszUnit));
  OSRDestroySpatialReference(hProj);

  // Prime meridian C API and C++
  EXPECT_DOUBLE_EQ(0.0, OSRGetPrimeMeridian(hSRS, &pszUnit));
  OGRSpatialReference* poSRS = OGRSpatialReference::FromHandle(hSRS);
  EXPECT_DOUBLE_EQ(0.0, poSRS->GetPrimeMeridian(&pszUnit));

  // CS setting C API
  OGRSpatialReferenceH hLocal = OSRNewSpatialReference(nullptr);
  EXPECT_EQ(OGRERR_NONE, OSRSetLocalCS(hLocal, "Local System"));
  EXPECT_TRUE(OSRIsLocal(hLocal));
  OSRDestroySpatialReference(hLocal);

  OGRSpatialReferenceH hGeoc = OSRNewSpatialReference(nullptr);
  EXPECT_EQ(OGRERR_NONE, OSRSetGeocCS(hGeoc, "Geocentric System"));
  EXPECT_TRUE(OSRIsGeocentric(hGeoc));
  OSRDestroySpatialReference(hGeoc);

  EXPECT_EQ(OGRERR_NONE, OSRSetVertCS(hSRS, "VertCS", "Datum", 0));
  EXPECT_TRUE(OSRIsVertical(hSRS) || OSRIsCompound(hSRS));
  EXPECT_EQ(OGRERR_NONE, OSRSetProjCS(hSRS, "ProjCS"));
  EXPECT_EQ(OGRERR_NONE, OSRSetProjection(hSRS, "Transverse_Mercator"));
  EXPECT_TRUE(OSRIsProjected(hSRS));

  OGRSpatialReferenceH hOther = OSRNewSpatialReference(nullptr);
  EXPECT_EQ(OGRERR_NONE, OSRImportFromEPSG(hOther, 4326));
  EXPECT_EQ(OGRERR_NONE, OSRCopyGeogCSFrom(hSRS, hOther));
  EXPECT_EQ(OGRERR_NONE, OSRSetGeogCS(hSRS, "WGS 84", "WGS_1984", "WGS 84",
                                      6378137.0, 298.257223563, "Greenwich",
                                      0.0, "degree", 0.0174532925199433));
  OSRDestroySpatialReference(hOther);

  // Ellipsoid queries
  OGRErr eErr = OGRERR_NONE;
  EXPECT_GT(OSRGetSemiMajor(hSRS, &eErr), 6378000.0);
  EXPECT_GT(OSRGetSemiMinor(hSRS, &eErr), 6356000.0);
  EXPECT_GT(OSRGetInvFlattening(hSRS, &eErr), 290.0);
  EXPECT_GT(poSRS->GetEccentricity(), 0.08);
  EXPECT_GT(poSRS->GetSquaredEccentricity(), 0.006);
  EXPECT_NEAR(298.257, OSRCalcInvFlattening(6378137.0, 6356752.31424518), 0.1);

  // TOWGS84 C API
  EXPECT_EQ(OGRERR_NONE,
            OSRSetTOWGS84(hSRS, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0));
  double adfCoeff[7] = {};
  EXPECT_EQ(OGRERR_NONE, OSRGetTOWGS84(hSRS, adfCoeff, 7));
  EXPECT_DOUBLE_EQ(1.0, adfCoeff[0]);

  // StripTOWGS84IfKnownDatum
  poSRS->StripTOWGS84IfKnownDatumAndAllowed();
  poSRS->StripTOWGS84IfKnownDatum();

  // AddGuessedTOWGS84
  OGRSpatialReference srsED50;
  srsED50.importFromEPSG(4230);
  srsED50.AddGuessedTOWGS84();
  OGRSpatialReferenceH hED50 = OSRNewSpatialReference(nullptr);
  OSRImportFromEPSG(hED50, 4230);
  OSRAddGuessedTOWGS84(hED50);
  OSRDestroySpatialReference(hED50);

  // Extensions
  EXPECT_EQ(OGRERR_NONE,
            poSRS->SetExtension("GEOGCS", "TEST_EXT", "TestValue"));
  EXPECT_STREQ("TestValue", poSRS->GetExtension("GEOGCS", "TEST_EXT"));
  EXPECT_EQ(OGRERR_NONE, poSRS->SetExtension("GEOGCS", "TEST_EXT", "NewValue"));
  EXPECT_STREQ("NewValue", poSRS->GetExtension("GEOGCS", "TEST_EXT"));
  EXPECT_EQ(nullptr, poSRS->GetExtension("GEOGCS", "NON_EXISTING"));

  // Static parameter classifications
  EXPECT_TRUE(OGRSpatialReference::IsAngularParameter(SRS_PP_CENTRAL_MERIDIAN));
  EXPECT_TRUE(OGRSpatialReference::IsAngularParameter("longitude_of_center"));
  EXPECT_TRUE(OGRSpatialReference::IsAngularParameter("latitude_of_origin"));
  EXPECT_TRUE(OGRSpatialReference::IsAngularParameter("standard_parallel_1"));
  EXPECT_TRUE(OGRSpatialReference::IsAngularParameter(SRS_PP_AZIMUTH));
  EXPECT_TRUE(
      OGRSpatialReference::IsAngularParameter(SRS_PP_RECTIFIED_GRID_ANGLE));
  EXPECT_FALSE(OGRSpatialReference::IsAngularParameter("false_easting"));

  EXPECT_TRUE(
      OGRSpatialReference::IsLongitudeParameter(SRS_PP_CENTRAL_MERIDIAN));
  EXPECT_TRUE(OGRSpatialReference::IsLongitudeParameter("longitude_of_origin"));
  EXPECT_FALSE(OGRSpatialReference::IsLongitudeParameter("latitude_of_origin"));

  EXPECT_TRUE(OGRSpatialReference::IsLinearParameter(SRS_PP_FALSE_EASTING));
  EXPECT_TRUE(OGRSpatialReference::IsLinearParameter(SRS_PP_FALSE_NORTHING));
  EXPECT_TRUE(OGRSpatialReference::IsLinearParameter(SRS_PP_SATELLITE_HEIGHT));
  EXPECT_FALSE(OGRSpatialReference::IsLinearParameter(SRS_PP_CENTRAL_MERIDIAN));

  // Projection parameters exhaustively
  poSRS->SetProjCS("UTM");
  poSRS->SetProjection("Transverse_Mercator");
  const std::vector<const char*> params = {
      SRS_PP_CENTRAL_MERIDIAN,      SRS_PP_SCALE_FACTOR,
      SRS_PP_STANDARD_PARALLEL_1,   SRS_PP_STANDARD_PARALLEL_2,
      SRS_PP_PSEUDO_STD_PARALLEL_1, SRS_PP_LONGITUDE_OF_CENTER,
      SRS_PP_LATITUDE_OF_CENTER,    SRS_PP_FALSE_EASTING,
      SRS_PP_FALSE_NORTHING,        SRS_PP_AZIMUTH,
      SRS_PP_RECTIFIED_GRID_ANGLE,  SRS_PP_LANDSAT_NUMBER,
      SRS_PP_PATH_NUMBER,           SRS_PP_PERSPECTIVE_POINT_HEIGHT,
      SRS_PP_SATELLITE_HEIGHT,      SRS_PP_LATITUDE_OF_POINT_1,
      SRS_PP_LONGITUDE_OF_POINT_1,  SRS_PP_LATITUDE_OF_POINT_2,
      SRS_PP_LONGITUDE_OF_POINT_2,  SRS_PP_ZONE,
  };
  for (const char* param : params) {
    poSRS->SetProjParm(param, 123.456);
    EXPECT_DOUBLE_EQ(123.456, poSRS->GetProjParm(param, 0.0, &eErr));
    poSRS->SetNormProjParm(param, 789.012);
    EXPECT_DOUBLE_EQ(789.012, poSRS->GetNormProjParm(param, 0.0, &eErr));
  }

  // SetAttrValue C API
  EXPECT_EQ(OGRERR_NONE, OSRSetAttrValue(hSRS, "PROJCS|AUTHORITY", "CUSTOM"));

  // IsEmpty
  OGRSpatialReference srsEmpty;
  EXPECT_TRUE(srsEmpty.IsEmpty());
  EXPECT_FALSE(poSRS->IsEmpty());

  OSRDestroySpatialReference(hSRS);
}

TEST(OgrSpatialReferenceTest, AxisOrientationsAndMappings) {
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));

  // Axis orientations testing
  OGRAxisOrientation orient = OAO_Other;
  EXPECT_EQ(OGRERR_NONE,
            srs.SetAxes("GEOGCS", "Lat", OAO_North, "Long", OAO_East));
  srs.GetAxis(nullptr, 0, &orient);
  EXPECT_EQ(OAO_North, orient);
  srs.GetAxis(nullptr, 1, &orient);
  EXPECT_EQ(OAO_East, orient);

  EXPECT_EQ(OGRERR_NONE,
            srs.SetAxes("GEOGCS", "Lat", OAO_South, "Long", OAO_West));
  srs.GetAxis(nullptr, 0, &orient);
  EXPECT_EQ(OAO_South, orient);
  srs.GetAxis(nullptr, 1, &orient);
  EXPECT_EQ(OAO_West, orient);

  EXPECT_EQ(OGRERR_NONE, srs.SetAxes("GEOGCS", "Up", OAO_Up, "Down", OAO_Down));
  srs.GetAxis(nullptr, 0, &orient);
  EXPECT_EQ(OAO_Up, orient);
  srs.GetAxis(nullptr, 1, &orient);
  EXPECT_EQ(OAO_Down, orient);

  EXPECT_EQ(OGRERR_NONE,
            srs.SetAxes("GEOGCS", "Other", OAO_Other, "Other2", OAO_Other));
  srs.GetAxis(nullptr, 0, &orient);
  EXPECT_EQ(OAO_Other, orient);

  // C API axis setting
  OGRSpatialReferenceH hSRS = OGRSpatialReference::ToHandle(&srs);
  EXPECT_EQ(OGRERR_NONE,
            OSRSetAxes(hSRS, "GEOGCS", "Lat", OAO_North, "Long", OAO_East));
  EXPECT_NE(nullptr, OSRGetAxis(hSRS, nullptr, 0, &orient));
  EXPECT_EQ(OAO_North, orient);

  // Strategy and Mapping C API
  OSRSetAxisMappingStrategy(hSRS, OAMS_AUTHORITY_COMPLIANT);
  EXPECT_EQ(OAMS_AUTHORITY_COMPLIANT, OSRGetAxisMappingStrategy(hSRS));

  const int anMapping[] = {2, 1};
  EXPECT_EQ(OGRERR_NONE, OSRSetDataAxisToSRSAxisMapping(hSRS, 2, anMapping));
  int nCount = 0;
  const int* panMapping = OSRGetDataAxisToSRSAxisMapping(hSRS, &nCount);
  ASSERT_THAT(panMapping, testing::NotNull());
  EXPECT_EQ(2, nCount);
  EXPECT_EQ(2, panMapping[0]);
  EXPECT_EQ(1, panMapping[1]);

  // Promote/Demote C API
  EXPECT_EQ(OGRERR_NONE, OSRPromoteTo3D(hSRS, nullptr));
  EXPECT_EQ(3, OSRGetAxesCount(hSRS));
  EXPECT_EQ(OGRERR_NONE, OSRDemoteTo2D(hSRS, nullptr));
  EXPECT_EQ(2, OSRGetAxesCount(hSRS));

  // Coordinate epoch C API
  OSRSetCoordinateEpoch(hSRS, 2022.0);
  EXPECT_DOUBLE_EQ(2022.0, OSRGetCoordinateEpoch(hSRS));

  // UpdateCoordinateSystemFromGeogCRS
  OGRSpatialReference srsProj3D;
  srsProj3D.importFromEPSG(32632);
  srsProj3D.PromoteTo3D(nullptr);
  srsProj3D.UpdateCoordinateSystemFromGeogCRS();

  OGRSpatialReference srsProj2D;
  srsProj2D.importFromEPSG(32632);
  srsProj2D.UpdateCoordinateSystemFromGeogCRS();
}

TEST(OgrSpatialReferenceTest, EPSGAndMatching) {
  // Deprecated EPSG code
  OGRSpatialReference srsDep;
  EXPECT_EQ(OGRERR_NONE, srsDep.importFromEPSG(3785));

  // EPSGA
  OGRSpatialReference srsEPSGA;
  EXPECT_EQ(OGRERR_NONE, srsEPSGA.importFromEPSGA(4326));
  OGRSpatialReferenceH hEPSGA = OSRNewSpatialReference(nullptr);
  EXPECT_EQ(OGRERR_NONE, OSRImportFromEPSGA(hEPSGA, 4326));
  EXPECT_TRUE(OSREPSGTreatsAsLatLong(hEPSGA));
  OSRDestroySpatialReference(hEPSGA);

  // Northing/Easting ordering
  OGRSpatialReference srsNE;
  EXPECT_EQ(OGRERR_NONE, srsNE.importFromEPSG(3006));
  EXPECT_TRUE(srsNE.EPSGTreatsAsNorthingEasting());
  OGRSpatialReferenceH hNE = OGRSpatialReference::ToHandle(&srsNE);
  EXPECT_TRUE(OSREPSGTreatsAsNorthingEasting(hNE));

  // GetEPSGGeogCS
  OGRSpatialReference srsWGS84;
  srsWGS84.importFromEPSG(4326);
  EXPECT_EQ(4326, srsWGS84.GetEPSGGeogCS());

  // FindMatches and FindBestMatch
  OGRSpatialReference srsMatch;
  srsMatch.importFromEPSG(32632);
  int nEntries = 0;
  int* panConfidence = nullptr;
  OGRSpatialReferenceH* pahMatches =
      srsMatch.FindMatches(nullptr, &nEntries, &panConfidence);
  EXPECT_GT(nEntries, 0);
  OSRFreeSRSArray(pahMatches);
  CPLFree(panConfidence);

  std::unique_ptr<OGRSpatialReference> bestMatch(
      srsMatch.FindBestMatch(90, "EPSG", nullptr));
  EXPECT_NE(nullptr, bestMatch);

  // OSRFindMatches
  OGRSpatialReferenceH hMatch = OGRSpatialReference::ToHandle(&srsMatch);
  pahMatches = OSRFindMatches(hMatch, nullptr, &nEntries, &panConfidence);
  EXPECT_GT(nEntries, 0);
  OSRFreeSRSArray(pahMatches);
  CPLFree(panConfidence);

  // Wisconsin WKT
  OGRSpatialReference srsWis;
  srsWis.ImportFromESRIWisconsinWKT(nullptr, -90.0, 44.0, nullptr,
                                    "NAD_1983_HARN_WISCRS_Adams_County_Feet");

  // StatePlane
  OGRSpatialReference srsSP;
  EXPECT_EQ(OGRERR_NONE, srsSP.SetStatePlane(101, FALSE));
  EXPECT_EQ(OGRERR_NONE, srsSP.SetStatePlane(101, TRUE));
  EXPECT_EQ(OGRERR_NONE,
            srsSP.SetStatePlane(101, TRUE, "Foot", 0.304800609601219));
  srsSP.ImportFromESRIStatePlaneWKT(
      101, "NAD_1983", "Meter", 101,
      "NAD_1983_StatePlane_Alabama_East_FIPS_0101");

  // UTM zone query
  int bNorth = 0;
  OGRSpatialReference srsUTM;
  srsUTM.SetUTM(32, TRUE);
  EXPECT_EQ(32, srsUTM.GetUTMZone(&bNorth));
  EXPECT_TRUE(bNorth);
  OGRSpatialReferenceH hUTM = OGRSpatialReference::ToHandle(&srsUTM);
  EXPECT_EQ(32, OSRGetUTMZone(hUTM, &bNorth));

  // Morph to/from ESRI
  EXPECT_EQ(OGRERR_NONE, OSRMorphToESRI(hUTM));
  EXPECT_EQ(OGRERR_NONE, OSRMorphFromESRI(hUTM));

  // StripVertical and CloneGeogCS C API
  EXPECT_EQ(OGRERR_NONE, OSRStripVertical(hUTM));
  OGRSpatialReferenceH hClonedGeog = OSRCloneGeogCS(hUTM);
  ASSERT_THAT(hClonedGeog, testing::NotNull());
  OSRDestroySpatialReference(hClonedGeog);

  // OSRIsSameEx with options
  OGRSpatialReference srsA, srsB;
  srsA.importFromEPSG(4326);
  srsB.importFromEPSG(4326);
  const char* const sameOpts[] = {
      "IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES", "IGNORE_COORDINATE_EPOCH=YES",
      "CRITERION=EQUIVALENT", "IGNORE_COORDINATE_SYSTEM_TRANSFORMATION=YES",
      nullptr};
  EXPECT_TRUE(OSRIsSameEx(OGRSpatialReference::ToHandle(&srsA),
                          OGRSpatialReference::ToHandle(&srsB), sameOpts));

  // Dynamic, point motion, derived queries
  EXPECT_FALSE(OSRIsDerivedGeographic(hUTM));
  EXPECT_FALSE(OSRIsDerivedProjected(hUTM));
  EXPECT_FALSE(OSRHasPointMotionOperation(hUTM));
  srsUTM.HasPointMotionOperation();
  OSRIsDynamic(hUTM);

  // DestroySpatialReference static
  OGRSpatialReference* poTemp = new OGRSpatialReference();
  OGRSpatialReference::DestroySpatialReference(poTemp);

  // OSRCleanup
  OSRCleanup();
}

TEST(OgrSpatialReferenceTest, AdvancedEdgeCasesAndCoverage) {
  // Config options for axis mapping strategy
  CPLSetConfigOption("OSR_DEFAULT_AXIS_MAPPING_STRATEGY",
                     "AUTHORITY_COMPLIANT");
  OGRSpatialReference srsAC;
  EXPECT_EQ(OAMS_AUTHORITY_COMPLIANT, srsAC.GetAxisMappingStrategy());
  CPLSetConfigOption("OSR_DEFAULT_AXIS_MAPPING_STRATEGY",
                     "TRADITIONAL_GIS_ORDER");
  OGRSpatialReference srsTGO;
  EXPECT_EQ(OAMS_TRADITIONAL_GIS_ORDER, srsTGO.GetAxisMappingStrategy());
  CPLSetConfigOption("OSR_DEFAULT_AXIS_MAPPING_STRATEGY", nullptr);

  CPLSetConfigOption("OSR_STRIP_TOWGS84", "NO");
  OGRSpatialReference srsNoStrip;
  srsNoStrip.importFromEPSG(4326);
  srsNoStrip.SetTOWGS84(0, 0, 0, 0, 0, 0, 0);
  srsNoStrip.StripTOWGS84IfKnownDatumAndAllowed();
  CPLSetConfigOption("OSR_STRIP_TOWGS84", nullptr);

  CPLSetConfigOption("OSR_USE_NON_DEPRECATED", "NO");
  OGRSpatialReference srsDeprecated;
  srsDeprecated.importFromEPSG(3785);
  CPLSetConfigOption("OSR_USE_NON_DEPRECATED", nullptr);

  // exportToWkt options: SFSQL and OUTPUT_AXIS
  OGRSpatialReference srsWktOpts;
  srsWktOpts.importFromEPSG(32632);
  const char* const sfsqlOpts[] = {"FORMAT=SFSQL", nullptr};
  char* pszSFSQL = nullptr;
  srsWktOpts.exportToWkt(&pszSFSQL, sfsqlOpts);
  CPLFree(pszSFSQL);

  const char* const axisOpts1[] = {"OUTPUT_AXIS=AUTO", nullptr};
  const char* const axisOpts2[] = {"OUTPUT_AXIS=YES", nullptr};
  const char* const axisOpts3[] = {"OUTPUT_AXIS=NO", nullptr};
  srsWktOpts.exportToWkt(&pszSFSQL, axisOpts1);
  CPLFree(pszSFSQL);
  srsWktOpts.exportToWkt(&pszSFSQL, axisOpts2);
  CPLFree(pszSFSQL);
  srsWktOpts.exportToWkt(&pszSFSQL, axisOpts3);
  CPLFree(pszSFSQL);

  // SetAxes and GetAxis with specific target keys
  OGRSpatialReference srsWkt1Axes;
  srsWkt1Axes.importFromWkt(
      "GEOGCS[\"Custom\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
      "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0."
      "0174532925199433]]");
  OGRAxisOrientation orient = OAO_Other;
  srsWkt1Axes.SetAxes("GEOGCS", "Lat", OAO_North, "Long", OAO_East);
  srsWkt1Axes.GetAxis("GEOGCS", 0, &orient);
  EXPECT_EQ(OAO_North, orient);
  srsWkt1Axes.GetAxis("GEOGCS", 1, &orient);
  EXPECT_EQ(OAO_East, orient);

  srsWkt1Axes.SetAxes("GEOGCS", "Lat", OAO_South, "Long", OAO_West);
  srsWkt1Axes.GetAxis("GEOGCS", 0, &orient);
  EXPECT_EQ(OAO_South, orient);
  srsWkt1Axes.GetAxis("GEOGCS", 1, &orient);
  EXPECT_EQ(OAO_West, orient);

  srsWkt1Axes.SetAxes("GEOGCS", "Up", OAO_Up, "Down", OAO_Down);
  srsWkt1Axes.GetAxis("GEOGCS", 0, &orient);
  EXPECT_EQ(OAO_Up, orient);
  srsWkt1Axes.GetAxis("GEOGCS", 1, &orient);
  EXPECT_EQ(OAO_Down, orient);

  srsWkt1Axes.SetAxes("GEOGCS", "Other", OAO_Other, "Other2", OAO_Other);
  srsWkt1Axes.GetAxis("GEOGCS", 0, &orient);
  EXPECT_EQ(OAO_Other, orient);

  OGRSpatialReferenceH hWkt1Axes = OGRSpatialReference::ToHandle(&srsWkt1Axes);
  OSRSetAxes(hWkt1Axes, "GEOGCS", "Lat", OAO_North, "Long", OAO_East);
  OSRGetAxis(hWkt1Axes, "GEOGCS", 0, &orient);
  EXPECT_EQ(OAO_North, orient);

  // SetAuthority and GetAuthority
  OGRSpatialReference srsAuth;
  srsAuth.importFromEPSG(32632);
  srsAuth.SetAuthority(nullptr, "EPSG", 32632);
  EXPECT_STREQ("32632", srsAuth.GetAuthorityCode(nullptr));
  EXPECT_STREQ("EPSG", srsAuth.GetAuthorityName(nullptr));

  srsAuth.SetAuthority("GEOGCS", "EPSG", 4326);
  EXPECT_STREQ("4326", srsAuth.GetAuthorityCode("GEOGCS"));
  EXPECT_STREQ("EPSG", srsAuth.GetAuthorityName("GEOGCS"));

  srsAuth.SetAuthority("GEOGCS|DATUM", "EPSG", 6326);
  EXPECT_STREQ("6326", srsAuth.GetAuthorityCode("GEOGCS|DATUM"));
  EXPECT_STREQ("EPSG", srsAuth.GetAuthorityName("GEOGCS|DATUM"));

  srsAuth.SetAuthority("PROJCS|UNIT", "EPSG", 9001);
  EXPECT_STREQ("9001", srsAuth.GetAuthorityCode("PROJCS|UNIT"));
  EXPECT_STREQ("EPSG", srsAuth.GetAuthorityName("PROJCS|UNIT"));

  OGRSpatialReferenceH hAuth = OGRSpatialReference::ToHandle(&srsAuth);
  OSRSetAuthority(hAuth, "GEOGCS", "EPSG", 4326);
  EXPECT_STREQ("4326", OSRGetAuthorityCode(hAuth, "GEOGCS"));
  EXPECT_STREQ("EPSG", OSRGetAuthorityName(hAuth, "GEOGCS"));

  // SetTargetLinearUnits on GEOCCS and VERT_CS
  OGRSpatialReference srsGeoc;
  srsGeoc.SetGeocCS("Geocentric");
  EXPECT_EQ(OGRERR_NONE, srsGeoc.SetTargetLinearUnits("GEOCCS", "metre", 1.0));

  OGRSpatialReference srsVert;
  srsVert.SetVertCS("Vertical", "Datum", 0);
  EXPECT_EQ(OGRERR_NONE, srsVert.SetTargetLinearUnits("VERT_CS", "metre", 1.0));

  // UpdateCoordinateSystemFromGeogCRS promoting to 3D
  const char* kWkt2_3D_Base =
      "PROJCRS[\"UTM zone 32N\",\n"
      "    BASEGEOGCRS[\"WGS 84 (3D)\",\n"
      "        DATUM[\"World Geodetic System 1984\",\n"
      "            ELLIPSOID[\"WGS 84\",6378137,298.257223563]],\n"
      "        CS[ellipsoidal,3],\n"
      "            AXIS[\"latitude\",north],\n"
      "            AXIS[\"longitude\",east],\n"
      "            AXIS[\"ellipsoidal height\",up],\n"
      "            UNIT[\"metre\",1]],\n"
      "    CONVERSION[\"UTM zone 32N\",\n"
      "        METHOD[\"Transverse Mercator\"],\n"
      "        PARAMETER[\"Latitude of natural origin\",0],\n"
      "        PARAMETER[\"Longitude of natural origin\",9],\n"
      "        PARAMETER[\"Scale factor at natural origin\",0.9996],\n"
      "        PARAMETER[\"False easting\",500000],\n"
      "        PARAMETER[\"False northing\",0]],\n"
      "    CS[Cartesian,2],\n"
      "        AXIS[\"(E)\",east],\n"
      "        AXIS[\"(N)\",north],\n"
      "        UNIT[\"metre\",1]]";
  OGRSpatialReference srsPromoteCS;
  EXPECT_EQ(OGRERR_NONE, srsPromoteCS.importFromWkt(kWkt2_3D_Base));
  srsPromoteCS.UpdateCoordinateSystemFromGeogCRS();
  EXPECT_EQ(3, srsPromoteCS.GetAxesCount());

  // IsDerivedProjected
  const char* kWkt2_DerivedProj =
      "DERIVEDPROJCRS[\"Derived Projected\",\n"
      "    BASEPROJCRS[\"WGS 84 / UTM zone 32N\",\n"
      "        BASEGEOGCRS[\"WGS 84\",\n"
      "            DATUM[\"World Geodetic System 1984\",\n"
      "                ELLIPSOID[\"WGS 84\",6378137,298.257223563]],\n"
      "            CS[ellipsoidal,2],\n"
      "                AXIS[\"latitude\",north],\n"
      "                AXIS[\"longitude\",east],\n"
      "                UNIT[\"degree\",0.0174532925199433]],\n"
      "        CONVERSION[\"UTM zone 32N\",\n"
      "            METHOD[\"Transverse Mercator\"],\n"
      "            PARAMETER[\"Latitude of natural origin\",0],\n"
      "            PARAMETER[\"Longitude of natural origin\",9],\n"
      "            PARAMETER[\"Scale factor at natural origin\",0.9996],\n"
      "            PARAMETER[\"False easting\",500000],\n"
      "            PARAMETER[\"False northing\",0]],\n"
      "        CS[Cartesian,2],\n"
      "            AXIS[\"(E)\",east],\n"
      "            AXIS[\"(N)\",north],\n"
      "            UNIT[\"metre\",1]],\n"
      "    DERIVINGCONVERSION[\"Affine\",\n"
      "        METHOD[\"Affine parametric transformation\"],\n"
      "        PARAMETER[\"A0\",0],\n"
      "        PARAMETER[\"A1\",1],\n"
      "        PARAMETER[\"A2\",0],\n"
      "        PARAMETER[\"B0\",0],\n"
      "        PARAMETER[\"B1\",0],\n"
      "        PARAMETER[\"B2\",1]],\n"
      "    CS[Cartesian,2],\n"
      "        AXIS[\"x\",east],\n"
      "        AXIS[\"y\",north],\n"
      "        UNIT[\"metre\",1]]";
  OGRSpatialReference srsDerivedProj;
  if (srsDerivedProj.importFromWkt(kWkt2_DerivedProj) == OGRERR_NONE) {
    EXPECT_TRUE(srsDerivedProj.IsDerivedProjected());
    OGRSpatialReferenceH hDP = OGRSpatialReference::ToHandle(&srsDerivedProj);
    EXPECT_TRUE(OSRIsDerivedProjected(hDP));
  }

  // GetUTMZone branches
  int bNorth = 0;
  OGRSpatialReference srsUTMEdge;
  srsUTMEdge.SetUTM(32, TRUE);
  srsUTMEdge.PromoteTo3D(nullptr);
  EXPECT_EQ(32, srsUTMEdge.GetUTMZone(&bNorth));

  srsUTMEdge.DemoteTo2D(nullptr);
  srsUTMEdge.SetUTM(32, FALSE);
  EXPECT_EQ(32, srsUTMEdge.GetUTMZone(&bNorth));
  EXPECT_FALSE(bNorth);
  EXPECT_EQ(32, srsUTMEdge.GetUTMZone(nullptr));

  OGRSpatialReferenceH hUTMEdge = OGRSpatialReference::ToHandle(&srsUTMEdge);
  EXPECT_EQ(32, OSRGetUTMZone(hUTMEdge, nullptr));

  OGRSpatialReference srsNonUTM;
  srsNonUTM.SetWellKnownGeogCS("WGS84");
  srsNonUTM.SetLCC(30.0, 35.0, 25.0, -100.0, 0.0, 0.0);
  EXPECT_EQ(0, srsNonUTM.GetUTMZone(&bNorth));

  srsUTMEdge.SetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 10.0);
  EXPECT_EQ(0, srsUTMEdge.GetUTMZone(&bNorth));
  srsUTMEdge.SetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0);

  srsUTMEdge.SetProjParm(SRS_PP_SCALE_FACTOR, 1.0);
  EXPECT_EQ(0, srsUTMEdge.GetUTMZone(&bNorth));
  srsUTMEdge.SetProjParm(SRS_PP_SCALE_FACTOR, 0.9996);

  srsUTMEdge.SetNormProjParm(SRS_PP_FALSE_EASTING, 100000.0);
  EXPECT_EQ(0, srsUTMEdge.GetUTMZone(&bNorth));
  srsUTMEdge.SetNormProjParm(SRS_PP_FALSE_EASTING, 500000.0);

  srsUTMEdge.SetNormProjParm(SRS_PP_FALSE_NORTHING, 5000000.0);
  EXPECT_EQ(0, srsUTMEdge.GetUTMZone(&bNorth));
  srsUTMEdge.SetNormProjParm(SRS_PP_FALSE_NORTHING, 0.0);

  srsUTMEdge.SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, 200.0);
  EXPECT_EQ(0, srsUTMEdge.GetUTMZone(&bNorth));

  // CopyGeogCSFrom
  OGRSpatialReference srsDestProj;
  srsDestProj.importFromEPSG(32632);
  OGRSpatialReference srsSrcBound;
  srsSrcBound.importFromEPSG(4230);
  srsSrcBound.AddGuessedTOWGS84();
  EXPECT_EQ(OGRERR_NONE, srsDestProj.CopyGeogCSFrom(&srsSrcBound));

  OGRSpatialReference srsDestGeoc;
  srsDestGeoc.SetGeocCS("Geocentric");
  EXPECT_EQ(OGRERR_NONE, srsDestGeoc.CopyGeogCSFrom(&srsSrcBound));

  // StripVertical
  OGRSpatialReference srsCompoundStrip;
  OGRSpatialReference srsH, srsV;
  srsH.importFromEPSG(4326);
  srsV.importFromEPSG(5703);
  srsCompoundStrip.SetCompoundCS("Comp", &srsH, &srsV);
  EXPECT_TRUE(srsCompoundStrip.IsCompound());
  EXPECT_EQ(OGRERR_NONE, srsCompoundStrip.StripVertical());
  EXPECT_FALSE(srsCompoundStrip.IsCompound());
  EXPECT_TRUE(srsCompoundStrip.IsGeographic());

  // GetEPSGGeogCS datum name variants
  OGRSpatialReference srsG;
  srsG.SetGeogCS("Custom_WGS_84", "Custom_WGS_1984", "Spheroid", 6378137.0,
                 298.257223563);
  EXPECT_EQ(4326, srsG.GetEPSGGeogCS());

  srsG.SetGeogCS("Custom_WGS_72", "Custom_WGS_1972", "Spheroid", 6378135.0,
                 298.26);
  EXPECT_EQ(4322, srsG.GetEPSGGeogCS());

  srsG.SetGeogCS("Custom_NAD_83", "Custom_NAD_1983", "Spheroid", 6378137.0,
                 298.257222101);
  EXPECT_EQ(4269, srsG.GetEPSGGeogCS());

  srsG.SetGeogCS("Custom_NAD_27", "Custom_NAD_1927", "Spheroid", 6378206.4,
                 294.9786982);
  EXPECT_EQ(4267, srsG.GetEPSGGeogCS());

  srsG.SetGeogCS("Custom", "Custom", "Spheroid", 6378137.0, 298.257);
  srsG.SetAuthority("GEOGCS|DATUM", "EPSG", 6326);
  EXPECT_EQ(4326, srsG.GetEPSGGeogCS());

  // Well-known CRS variants
  OGRSpatialReference srsWK;
  EXPECT_EQ(OGRERR_NONE, srsWK.SetWellKnownGeogCS("CRS84"));
  EXPECT_EQ(OGRERR_NONE, srsWK.SetWellKnownGeogCS("CRS:84"));
  EXPECT_EQ(OGRERR_NONE, srsWK.SetWellKnownGeogCS("CRS83"));
  EXPECT_EQ(OGRERR_NONE, srsWK.SetWellKnownGeogCS("CRS:83"));

  // SetFromUserInput formats
  const char* const inputs2[] = {
      "CRS:84", "CRS:83", "WGS72",
      "NAD27",  "NAD83",  "urn:ogc:def:crs:OGC::CRS84",
  };
  for (const char* input : inputs2) {
    OGRSpatialReference srsIn;
    EXPECT_EQ(OGRERR_NONE, srsIn.SetFromUserInput(input)) << input;
  }

  // VSI file input
  constexpr char kVsiPath[] = "/vsimem/test_srs.prj";
  constexpr std::string_view kWktSimple =
      "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
      "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0."
      "0174532925199433]]";
  {
    VsiMemTempWrapper file(kVsiPath, std::string(kWktSimple));
    OGRSpatialReference srsVsi;
    EXPECT_EQ(OGRERR_NONE, srsVsi.SetFromUserInput(kVsiPath));
    EXPECT_EQ(OGRERR_NONE,
              srsVsi.SetFromUserInput(CPLSPrintf("ESRI::%s", kVsiPath)));
  }

  // OSRSetFromUserInputEx
  OGRSpatialReferenceH hUserInput = OSRNewSpatialReference(nullptr);
  EXPECT_EQ(OGRERR_NONE,
            OSRSetFromUserInputEx(hUserInput, "EPSG:4326", nullptr));
  EXPECT_EQ(OGRERR_FAILURE,
            OSRSetFromUserInputEx(nullptr, "EPSG:4326", nullptr));
  OSRDestroySpatialReference(hUserInput);
}

TEST(OgrSpatialReferenceTest, DeepCoverageTargeted) {
  // Compound CRS with Bound CRS components
  OGRSpatialReference srsED50;
  EXPECT_EQ(OGRERR_NONE, srsED50.importFromEPSG(4230));
  srsED50.AddGuessedTOWGS84();

  OGRSpatialReference srsVert;
  EXPECT_EQ(OGRERR_NONE, srsVert.importFromEPSG(5703));

  OGRSpatialReference srsCompBound;
  EXPECT_EQ(OGRERR_NONE, srsCompBound.SetCompoundCS("ED50 Bound + NAVD88",
                                                    &srsED50, &srsVert));
  EXPECT_TRUE(srsCompBound.IsCompound());
  EXPECT_EQ(3, srsCompBound.GetAxesCount());

  OGRAxisOrientation orient = OAO_Other;
  double dfConv = 0.0;
  EXPECT_NE(nullptr, srsCompBound.GetAxis(nullptr, 0, &orient, &dfConv));
  EXPECT_NE(nullptr, srsCompBound.GetAxis(nullptr, 1, &orient, &dfConv));
  EXPECT_NE(nullptr, srsCompBound.GetAxis(nullptr, 2, &orient, &dfConv));
  EXPECT_EQ(nullptr, srsCompBound.GetAxis(nullptr, 3, &orient, &dfConv));

  // Export to WKT1 with bound compound
  char* pszCompWkt = nullptr;
  const char* const wkt1Opts[] = {"FORMAT=WKT1_GDAL", nullptr};
  srsCompBound.exportToWkt(&pszCompWkt, wkt1Opts);
  if (pszCompWkt) {
    CPLFree(pszCompWkt);
  }

  // WKT1_SIMPLE with MULTILINE YES and NO
  const char* const wktSimpleOpts1[] = {"FORMAT=WKT1_SIMPLE", "MULTILINE=YES",
                                        nullptr};
  const char* const wktSimpleOpts2[] = {"FORMAT=WKT1_SIMPLE", "MULTILINE=NO",
                                        nullptr};
  srsED50.exportToWkt(&pszCompWkt, wktSimpleOpts1);
  CPLFree(pszCompWkt);
  srsED50.exportToWkt(&pszCompWkt, wktSimpleOpts2);
  CPLFree(pszCompWkt);

  // SFSQL with MULTILINE YES and NO
  const char* const sfsqlOpts1[] = {"FORMAT=SFSQL", "MULTILINE=YES", nullptr};
  const char* const sfsqlOpts2[] = {"FORMAT=SFSQL", "MULTILINE=NO", nullptr};
  srsED50.exportToWkt(&pszCompWkt, sfsqlOpts1);
  CPLFree(pszCompWkt);
  srsED50.exportToWkt(&pszCompWkt, sfsqlOpts2);
  CPLFree(pszCompWkt);

  // Fallback to WKT2 when WKT1 fails (e.g. rotated pole derived geographic)
  OGRSpatialReference srsRotated;
  srsRotated.importFromEPSG(4326);
  srsRotated.SetDerivedGeogCRSWithPoleRotationGRIBConvention("Rotated GRIB",
                                                             -30.0, 10.0, 0.0);
  char* pszRotWkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, srsRotated.exportToWkt(&pszRotWkt));
  EXPECT_NE(nullptr, pszRotWkt);
  CPLFree(pszRotWkt);

  // UTM South with WKT2_2018
  OGRSpatialReference srsUTMSouth;
  srsUTMSouth.SetUTM(32, FALSE);
  const char* const wkt2_2018Opts[] = {"FORMAT=WKT2_2018", nullptr};
  char* pszUTMWkt = nullptr;
  srsUTMSouth.exportToWkt(&pszUTMWkt, wkt2_2018Opts);
  if (pszUTMWkt) {
    CPLFree(pszUTMWkt);
  }

  // Compound with projected horizontal for EPSGTreatsAsNorthingEasting
  OGRSpatialReference srsSwedish;
  srsSwedish.importFromEPSG(3006);
  OGRSpatialReference srsCompSwedish;
  srsCompSwedish.SetCompoundCS("Swedish + Height", &srsSwedish, &srsVert);
  EXPECT_TRUE(srsCompSwedish.EPSGTreatsAsNorthingEasting());
  OGRSpatialReferenceH hCompSwedish =
      OGRSpatialReference::ToHandle(&srsCompSwedish);
  EXPECT_TRUE(OSREPSGTreatsAsNorthingEasting(hCompSwedish));

  OGRSpatialReference srsGeog4326;
  srsGeog4326.importFromEPSG(4326);
  OGRSpatialReference srsCompGeog;
  srsCompGeog.SetCompoundCS("Geog + Height", &srsGeog4326, &srsVert);
  EXPECT_FALSE(srsCompGeog.EPSGTreatsAsNorthingEasting());

  // EPSG import config options and ESRI fallback
  CPLSetConfigOption("OSR_ADD_TOWGS84_ON_IMPORT_FROM_EPSG", "YES");
  OGRSpatialReference srsImportTOWGS84;
  srsImportTOWGS84.importFromEPSG(4230);
  CPLSetConfigOption("OSR_ADD_TOWGS84_ON_IMPORT_FROM_EPSG", nullptr);

  OGRSpatialReference srsESRIFallback;
  srsESRIFallback.importFromEPSG(102100);
  EXPECT_TRUE(srsESRIFallback.IsProjected());

  OGRSpatialReference srsInvalidEPSG;
  EXPECT_EQ(OGRERR_FAILURE, srsInvalidEPSG.importFromEPSG(99999999));

  // Point motion operation
  OGRSpatialReference srsPointMotion;
  srsPointMotion.importFromEPSG(8254);
  srsPointMotion.HasPointMotionOperation();
  OGRSpatialReferenceH hPM = OGRSpatialReference::ToHandle(&srsPointMotion);
  OSRHasPointMotionOperation(hPM);
  EXPECT_FALSE(OSRHasPointMotionOperation(nullptr));

  // Empty SRS unit getters
  OGRSpatialReference srsEmpty;
  const char* pszUnitName = nullptr;
  EXPECT_DOUBLE_EQ(CPLAtof(SRS_UA_DEGREE_CONV),
                   srsEmpty.GetAngularUnits(&pszUnitName));
  EXPECT_STREQ("degree", pszUnitName);
  EXPECT_DOUBLE_EQ(1.0, srsEmpty.GetLinearUnits(&pszUnitName));
  EXPECT_STREQ("unknown", pszUnitName);
  EXPECT_DOUBLE_EQ(1.0, srsEmpty.GetTargetLinearUnits("PROJCS", &pszUnitName));
  EXPECT_STREQ("unknown", pszUnitName);

  char* pszUnitMut = nullptr;
  EXPECT_DOUBLE_EQ(0.0, OSRGetAngularUnits(nullptr, &pszUnitMut));

  // SetDataAxisToSRSAxisMapping validations
  OGRSpatialReference srsMapping;
  srsMapping.importFromEPSG(4326);
  EXPECT_EQ(OGRERR_FAILURE, srsMapping.SetDataAxisToSRSAxisMapping({}));
  EXPECT_EQ(OGRERR_FAILURE, srsMapping.SetDataAxisToSRSAxisMapping({1}));
  EXPECT_EQ(OGRERR_NONE, srsMapping.SetDataAxisToSRSAxisMapping({1, 2}));
  EXPECT_EQ(OGRERR_NONE, srsMapping.SetDataAxisToSRSAxisMapping({-2, 1}));

  OGRSpatialReferenceH hMapping = OGRSpatialReference::ToHandle(&srsMapping);
  const int anMap[] = {1};
  EXPECT_EQ(OGRERR_FAILURE,
            OSRSetDataAxisToSRSAxisMapping(hMapping, 0, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OSRSetDataAxisToSRSAxisMapping(hMapping, 1, anMap));
  EXPECT_EQ(OGRERR_FAILURE, OSRSetDataAxisToSRSAxisMapping(nullptr, 2, anMap));
  int nMapCount = 0;
  EXPECT_EQ(nullptr, OSRGetDataAxisToSRSAxisMapping(nullptr, &nMapCount));
  EXPECT_EQ(nullptr, OSRGetDataAxisToSRSAxisMapping(hMapping, nullptr));

  // OSRSetProjection projections
  OGRSpatialReferenceH hProjSet = OSRNewSpatialReference(nullptr);
  EXPECT_EQ(OGRERR_NONE,
            OSRSetProjection(hProjSet, SRS_PT_ALBERS_CONIC_EQUAL_AREA));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetProjection(hProjSet, SRS_PT_LAMBERT_CONFORMAL_CONIC_2SP));
  EXPECT_EQ(OGRERR_NONE, OSRSetProjection(hProjSet, SRS_PT_MERCATOR_1SP));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetProjection(hProjSet, SRS_PT_OBLIQUE_STEREOGRAPHIC));
  EXPECT_EQ(OGRERR_FAILURE, OSRSetProjection(nullptr, SRS_PT_MERCATOR_1SP));
  OSRDestroySpatialReference(hProjSet);

  // Incompatible CS changes
  OGRSpatialReference srsProjIncomp;
  srsProjIncomp.importFromEPSG(32632);
  EXPECT_EQ(OGRERR_FAILURE, srsProjIncomp.SetGeocCS("IncompGeoc"));
  EXPECT_EQ(OGRERR_FAILURE, srsProjIncomp.SetLocalCS("IncompLocal"));
  EXPECT_EQ(OGRERR_FAILURE, OSRSetGeocCS(nullptr, "Geoc"));
  EXPECT_EQ(OGRERR_FAILURE, OSRSetLocalCS(nullptr, "Local"));
  EXPECT_EQ(OGRERR_FAILURE, OSRSetVertCS(nullptr, "Vert", "Datum", 0));

  // OSRCalcInvFlattening edge cases
  EXPECT_DOUBLE_EQ(0.0, OSRCalcInvFlattening(6378137.0, 6378137.0));
  EXPECT_DOUBLE_EQ(0.0, OSRCalcInvFlattening(0.0, 0.0));
  EXPECT_GT(OSRCalcInvFlattening(6378137.0, 6356752.31424518), 290.0);

  // OSRFreeSRSArray null safety
  OSRFreeSRSArray(nullptr);

  // Handle conversion null safety
  EXPECT_EQ(nullptr, OGRSpatialReference::FromHandle(nullptr));
  EXPECT_EQ(nullptr, OGRSpatialReference::ToHandle(nullptr));

  // PROJJSON SetFromUserInput for multiple types
  const char* const kProjJsonTypes[] = {
      "{\"$schema\":\"https://proj.org/schemas/v0.7/"
      "projjson.schema.json\",\"type\":\"ProjectedCRS\",\"name\":\"Proj\","
      "\"base_crs\":{\"name\":\"WGS "
      "84\",\"datum\":{\"type\":\"GeodeticReferenceFrame\",\"name\":\"WGS "
      "84\",\"ellipsoid\":{\"name\":\"WGS "
      "84\",\"semi_major_axis\":6378137,\"inverse_flattening\":298.257223563}},"
      "\"coordinate_system\":{\"subtype\":\"ellipsoidal\",\"axis\":[{\"name\":"
      "\"Lat\",\"abbreviation\":\"lat\",\"direction\":\"north\",\"unit\":"
      "\"degree\"},{\"name\":\"Lon\",\"abbreviation\":\"lon\",\"direction\":"
      "\"east\",\"unit\":\"degree\"}]}},\"conversion\":{\"name\":\"UTM\","
      "\"method\":{\"name\":\"Transverse "
      "Mercator\"},\"parameters\":[{\"name\":\"Latitude of natural "
      "origin\",\"value\":0,\"unit\":\"degree\"},{\"name\":\"Longitude of "
      "natural origin\",\"value\":9,\"unit\":\"degree\"},{\"name\":\"Scale "
      "factor at natural "
      "origin\",\"value\":0.9996,\"unit\":\"unity\"},{\"name\":\"False "
      "easting\",\"value\":500000,\"unit\":\"metre\"},{\"name\":\"False "
      "northing\",\"value\":0,\"unit\":\"metre\"}]},\"coordinate_system\":{"
      "\"subtype\":\"Cartesian\",\"axis\":[{\"name\":\"E\",\"abbreviation\":"
      "\"E\",\"direction\":\"east\",\"unit\":\"metre\"},{\"name\":\"N\","
      "\"abbreviation\":\"N\",\"direction\":\"north\",\"unit\":\"metre\"}]}}",
      "{\"$schema\":\"https://proj.org/schemas/v0.7/"
      "projjson.schema.json\",\"type\":\"VerticalCRS\",\"name\":\"NAVD88 "
      "height\",\"datum\":{\"type\":\"VerticalReferenceFrame\",\"name\":"
      "\"North American Vertical Datum "
      "1988\"},\"coordinate_system\":{\"subtype\":\"vertical\",\"axis\":[{"
      "\"name\":\"Gravity-related "
      "height\",\"abbreviation\":\"H\",\"direction\":\"up\",\"unit\":\"metre\"}"
      "]}}",
      "{\"$schema\":\"https://proj.org/schemas/v0.7/"
      "projjson.schema.json\",\"type\":\"EngineeringCRS\",\"name\":\"A local "
      "CRS\",\"datum\":{\"type\":\"EngineeringDatum\",\"name\":\"A local "
      "datum\"},\"coordinate_system\":{\"subtype\":\"Cartesian\",\"axis\":[{"
      "\"name\":\"x\",\"abbreviation\":\"x\",\"direction\":\"east\",\"unit\":"
      "\"metre\"},{\"name\":\"y\",\"abbreviation\":\"y\",\"direction\":"
      "\"north\",\"unit\":\"metre\"}]}}",
  };
  for (const char* pj : kProjJsonTypes) {
    OGRSpatialReference srsPJ;
    srsPJ.SetFromUserInput(pj);
  }

  char* pszWGS84_JSON = nullptr;
  srsGeog4326.exportToPROJJSON(&pszWGS84_JSON, nullptr);
  if (pszWGS84_JSON) {
    OGRSpatialReference srsEnsemble;
    srsEnsemble.SetFromUserInput(pszWGS84_JSON);
    CPLFree(pszWGS84_JSON);
  }

  // GetAxis on Projected CRS targeting GEOGCS node
  OGRSpatialReference srsUTMAxes;
  srsUTMAxes.importFromEPSG(32632);
  srsUTMAxes.SetAxes("GEOGCS", "Lat", OAO_North, "Long", OAO_East);
  EXPECT_NE(nullptr, srsUTMAxes.GetAxis("GEOGCS", 0, &orient));
  EXPECT_EQ(OAO_North, orient);
  EXPECT_NE(nullptr, srsUTMAxes.GetAxis("GEOGCS", 1, &orient));
  EXPECT_EQ(OAO_East, orient);

  srsUTMAxes.SetAxes("GEOGCS", "Lat", OAO_South, "Long", OAO_West);
  srsUTMAxes.GetAxis("GEOGCS", 0, &orient);
  EXPECT_EQ(OAO_South, orient);
  srsUTMAxes.GetAxis("GEOGCS", 1, &orient);
  EXPECT_EQ(OAO_West, orient);

  srsUTMAxes.SetAxes("GEOGCS", "Up", OAO_Up, "Down", OAO_Down);
  srsUTMAxes.GetAxis("GEOGCS", 0, &orient);
  EXPECT_EQ(OAO_Up, orient);
  srsUTMAxes.GetAxis("GEOGCS", 1, &orient);
  EXPECT_EQ(OAO_Down, orient);

  srsUTMAxes.SetAxes("GEOGCS", "Other", OAO_Other, "Other", OAO_Other);
  srsUTMAxes.GetAxis("GEOGCS", 0, &orient);
  EXPECT_EQ(OAO_Other, orient);

  // FindProjParm
  OGRSpatialReference srsWkt2Param;
  srsWkt2Param.importFromEPSG(32632);
  srsWkt2Param.FindProjParm(SRS_PP_LATITUDE_OF_ORIGIN);
  srsWkt2Param.FindProjParm(SRS_PP_CENTRAL_MERIDIAN);

  OGRSpatialReference srsWkt1Center;
  srsWkt1Center.importFromWkt(
      "PROJCS[\"Custom\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
      "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0."
      "0174532925199433]],PROJECTION[\"Transverse_Mercator\"],PARAMETER["
      "\"longitude_of_center\",10.0],PARAMETER[\"latitude_of_center\",0.0],"
      "UNIT[\"metre\",1]]");
  srsWkt1Center.FindProjParm(SRS_PP_CENTRAL_MERIDIAN);
  srsWkt1Center.FindProjParm(SRS_PP_LATITUDE_OF_ORIGIN);

  OGRSpatialReference srsWkt1Origin;
  srsWkt1Origin.importFromWkt(
      "PROJCS[\"Custom\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
      "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0."
      "0174532925199433]],PROJECTION[\"Transverse_Mercator\"],PARAMETER["
      "\"longitude_of_origin\",10.0],UNIT[\"metre\",1]]");
  srsWkt1Origin.FindProjParm(SRS_PP_CENTRAL_MERIDIAN);

  // CloneGeogCS on Bound and Geocentric
  std::unique_ptr<OGRSpatialReference> geogFromBound(srsED50.CloneGeogCS());
  ASSERT_THAT(geogFromBound, testing::NotNull());

  OGRSpatialReference srsGeocentric4978;
  srsGeocentric4978.importFromEPSG(4978);
  std::unique_ptr<OGRSpatialReference> geogFromGeoc(
      srsGeocentric4978.CloneGeogCS());
  ASSERT_THAT(geogFromGeoc, testing::NotNull());
  OGRSpatialReferenceH hGeocCloned =
      OSRCloneGeogCS(OGRSpatialReference::ToHandle(&srsGeocentric4978));
  ASSERT_THAT(hGeocCloned, testing::NotNull());
  OSRDestroySpatialReference(hGeocCloned);

  // FindBestMatch
  OGRSpatialReference srsZeroTOWGS;
  srsZeroTOWGS.importFromEPSG(4326);
  srsZeroTOWGS.SetTOWGS84(0, 0, 0, 0, 0, 0, 0);
  std::unique_ptr<OGRSpatialReference> bestMatchZero(
      srsZeroTOWGS.FindBestMatch(90, "EPSG", nullptr));

  OGRSpatialReference srsWebMerc;
  srsWebMerc.importFromEPSG(3857);
  std::unique_ptr<OGRSpatialReference> bestMatchMerc(
      srsWebMerc.FindBestMatch(50, "EPSG", nullptr));

  // CRS:84 FindMatches with axis swap
  OGRSpatialReference srsCRS84;
  srsCRS84.SetWellKnownGeogCS("CRS:84");
  int nCRS84Matches = 0;
  int* panConf = nullptr;
  OGRSpatialReferenceH* pahCRS84Matches =
      srsCRS84.FindMatches(nullptr, &nCRS84Matches, &panConf);
  EXPECT_GT(nCRS84Matches, 0);
  OSRFreeSRSArray(pahCRS84Matches);
  CPLFree(panConf);

  // Compound CRS Authority codes
  EXPECT_NE(nullptr, srsCompBound.GetAuthorityCode("HORIZCRS"));
  EXPECT_NE(nullptr, srsCompBound.GetAuthorityName("HORIZCRS"));
  EXPECT_NE(nullptr, srsCompBound.GetAuthorityCode("VERTCRS"));
  EXPECT_NE(nullptr, srsCompBound.GetAuthorityName("VERTCRS"));

  // Compound StripVertical
  EXPECT_EQ(OGRERR_NONE, srsCompBound.StripVertical());
  EXPECT_FALSE(srsCompBound.IsCompound());

  // exportToWkt std::string
  std::string sWkt1 = srsUTMAxes.exportToWkt();
  EXPECT_FALSE(sWkt1.empty());
  std::string sWkt2 = srsUTMAxes.exportToWkt(wkt1Opts);
  EXPECT_FALSE(sWkt2.empty());

  // COORDINATEMETADATA in importFromWkt
  const char* kCoordMetadataWkt =
      "COORDINATEMETADATA[\n"
      "    GEOGCRS[\"WGS 84\",\n"
      "        DATUM[\"World Geodetic System 1984\",\n"
      "            ELLIPSOID[\"WGS 84\",6378137,298.257223563]],\n"
      "        CS[ellipsoidal,2],\n"
      "            AXIS[\"latitude\",north],\n"
      "            AXIS[\"longitude\",east],\n"
      "            UNIT[\"degree\",0.0174532925199433]],\n"
      "    EPOCH[2020.5]]";
  OGRSpatialReference srsCoordMeta;
  EXPECT_EQ(OGRERR_NONE, srsCoordMeta.importFromWkt(kCoordMetadataWkt));
  EXPECT_DOUBLE_EQ(2020.5, srsCoordMeta.GetCoordinateEpoch());

  // CENTER_LONG in importFromWkt
  const char* kWktCenterLong =
      "PROJCS[\"Custom\",\n"
      "    GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
      "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0."
      "0174532925199433]],\n"
      "    PROJECTION[\"Transverse_Mercator\"],\n"
      "    PARAMETER[\"CENTER_LONG\",10.0],\n"
      "    UNIT[\"metre\",1]]";
  char* pszCenterLongMut = CPLStrdup(kWktCenterLong);
  char* pszCLPtr = pszCenterLongMut;
  OGRSpatialReference srsCenterLong;
  EXPECT_EQ(OGRERR_NONE, srsCenterLong.importFromWkt(&pszCLPtr));
  CPLFree(pszCenterLongMut);

  // 3D projected GetProjParm
  OGRSpatialReference srsUTM3D;
  srsUTM3D.importFromEPSG(32632);
  srsUTM3D.PromoteTo3D(nullptr);
  OGRErr eErr3D = OGRERR_NONE;
  EXPECT_DOUBLE_EQ(9.0,
                   srsUTM3D.GetProjParm(SRS_PP_CENTRAL_MERIDIAN, 0.0, &eErr3D));
  EXPECT_EQ(OGRERR_NONE, eErr3D);

  // ImportFromESRIWisconsinWKT
  OGRSpatialReference srsWisTM;
  srsWisTM.ImportFromESRIWisconsinWKT(SRS_PT_TRANSVERSE_MERCATOR, -90.0, 44.0,
                                      "meters");
  OGRSpatialReference srsWisLCC;
  srsWisLCC.ImportFromESRIWisconsinWKT("Lambert_Conformal_Conic", -90.0, 44.0,
                                       "meters");
  srsWisLCC.ImportFromESRIWisconsinWKT("Lambert_Conformal_Conic", -90.0, 44.0,
                                       "Foot_US");
}

TEST(OgrSpatialReferenceTest, FinalPushToNinetyPercent) {
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");

  // Equirectangular non-zero origin
  EXPECT_EQ(OGRERR_NONE, srs.SetEquirectangular(10.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEquirectangular2(10.0, 0.0, 20.0, 0.0, 0.0));

  // Mercator2SP non-zero origin
  EXPECT_EQ(OGRERR_NONE, srs.SetMercator2SP(40.0, 10.0, 0.0, 0.0, 0.0));

  // Wagner variations
  for (int v = 2; v <= 7; ++v) {
    EXPECT_EQ(OGRERR_NONE, srs.SetWagner(v, 40.0, 0.0, 0.0));
  }
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS, srs.SetWagner(99, 0.0, 0.0, 0.0));

  OGRSpatialReferenceH hWagner = OSRNewSpatialReference(nullptr);
  OSRSetWellKnownGeogCS(hWagner, "WGS84");
  for (int v = 2; v <= 7; ++v) {
    EXPECT_EQ(OGRERR_NONE, OSRSetWagner(hWagner, v, 40.0, 0.0, 0.0));
  }
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS, OSRSetWagner(hWagner, 99, 0.0, 0.0, 0.0));
  OSRDestroySpatialReference(hWagner);

  // SetGeogCS on Geocentric CRS
  OGRSpatialReference srsGeocSet;
  srsGeocSet.SetGeocCS("Geoc");
  EXPECT_EQ(OGRERR_NONE, srsGeocSet.SetGeogCS("WGS 84", "WGS_1984", "WGS 84",
                                              6378137.0, 298.257223563));

  // GetPrimeMeridian on empty SRS
  OGRSpatialReference srsEmpty;
  const char* pszPMEmpty = nullptr;
  EXPECT_DOUBLE_EQ(0.0, srsEmpty.GetPrimeMeridian(&pszPMEmpty));
  EXPECT_STREQ(SRS_PM_GREENWICH, pszPMEmpty);

  // SetFromUserInput IAU authority and disallow file access
  srs.SetFromUserInput("IAU:2015:49900");
  srs.SetFromUserInput("IAU:49900");
  const char* const noFileOpts[] = {"ALLOW_FILE_ACCESS=NO", nullptr};
  EXPECT_EQ(OGRERR_FAILURE,
            srs.SetFromUserInput("/vsimem/nonexistent.prj", noFileOpts));

  // importFromUrl spatialreference.org pattern
  srs.importFromUrl("http://spatialreference.org/ref/epsg/4326/");
  srs.importFromUrl("https://spatialreference.org/ref/epsg/4326/");

  // PROJJSON multiple types in SetFromUserInput
  const char* const kTypes[] = {
      "{\"type\":\"DerivedProjectedCRS\",\"name\":\"x\"}",
      "{\"type\":\"DerivedGeodeticCRS\",\"name\":\"x\"}",
      "{\"type\":\"DerivedGeographicCRS\",\"name\":\"x\"}",
      "{\"type\":\"DerivedVerticalCRS\",\"name\":\"x\"}",
      "{\"type\":\"TemporalCRS\",\"name\":\"x\"}",
      "{\"type\":\"ParametricCRS\",\"name\":\"x\"}",
  };
  for (const char* t : kTypes) {
    OGRSpatialReference srsT;
    srsT.SetFromUserInput(t);
  }

  // Compound CRS with Bound projected and Bound vertical
  OGRSpatialReference srsProjBound;
  srsProjBound.importFromEPSG(32632);
  srsProjBound.SetTOWGS84(1, 2, 3, 4, 5, 6, 7);
  OGRSpatialReference srsVertBound;
  srsVertBound.importFromEPSG(5703);
  OGRSpatialReference srsCompPB;
  EXPECT_EQ(OGRERR_NONE, srsCompPB.SetCompoundCS("UTM+NAVD88", &srsProjBound,
                                                 &srsVertBound));
  const char* pszUnit = nullptr;
  EXPECT_GT(srsCompPB.GetLinearUnits(&pszUnit), 0.0);
  EXPECT_FALSE(srsCompPB.IsGeographic());
  EXPECT_TRUE(srsCompPB.IsProjected() || srsCompPB.IsCompound());
  EXPECT_EQ(OGRERR_NONE, srsCompPB.StripVertical());

  // CopyGeogCSFrom with TOWGS84 on source
  OGRSpatialReference srsDestProjTOWGS;
  srsDestProjTOWGS.importFromEPSG(32632);
  OGRSpatialReference srsSrcTOWGS;
  srsSrcTOWGS.importFromEPSG(4326);
  srsSrcTOWGS.SetTOWGS84(1, 2, 3, 4, 5, 6, 7);
  EXPECT_EQ(OGRERR_NONE, srsDestProjTOWGS.CopyGeogCSFrom(&srsSrcTOWGS));

  // exportToWkt WKT1_GDAL on non-bound compound
  OGRSpatialReference srsCompNonBound;
  OGRSpatialReference srsH4230, srsV5703;
  srsH4230.importFromEPSG(4230);
  srsV5703.importFromEPSG(5703);
  srsCompNonBound.SetCompoundCS("ED50 + NAVD88", &srsH4230, &srsV5703);
  const char* const wkt1Opts[] = {"FORMAT=WKT1_GDAL", nullptr};
  char* pszWktGDAL = nullptr;
  srsCompNonBound.exportToWkt(&pszWktGDAL, wkt1Opts);
  if (pszWktGDAL) {
    CPLFree(pszWktGDAL);
  }

  // All other projection setters exhaustively C++ and C API
  EXPECT_EQ(OGRERR_NONE, srs.SetBonne(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetCEA(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetCS(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetEC(0.0, 0.0, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetGS(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetGH(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetGEOS(0.0, 35785831.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            srs.SetGaussSchreiberTMercator(0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetGnomonic(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetHOM(0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            srs.SetHOM2PNO(0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetKrovak(0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetLAEA(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetLCC(0.0, 0.0, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetPolyconic(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetPS(0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetRobinson(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetSinusoidal(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetStereographic(0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetSOC(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetTM(0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            srs.SetTMVariant("Transverse_Mercator", 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetTMSO(0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetVDG(0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, srs.SetSCH(0.0, 0.0, 0.0, 6378137.0));

  OGRSpatialReferenceH hAllProj = OSRNewSpatialReference(nullptr);
  OSRSetWellKnownGeogCS(hAllProj, "WGS84");
  EXPECT_EQ(OGRERR_NONE, OSRSetBonne(hAllProj, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetCEA(hAllProj, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetCS(hAllProj, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetEC(hAllProj, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetGS(hAllProj, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetGH(hAllProj, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetGEOS(hAllProj, 0.0, 35785831.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetGaussSchreiberTMercator(hAllProj, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetGnomonic(hAllProj, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetHOM(hAllProj, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetHOM2PNO(hAllProj, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetKrovak(hAllProj, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetLAEA(hAllProj, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetLCC(hAllProj, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetPolyconic(hAllProj, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetPS(hAllProj, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetRobinson(hAllProj, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetSinusoidal(hAllProj, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetStereographic(hAllProj, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetSOC(hAllProj, 0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetTM(hAllProj, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetTMVariant(hAllProj, "Transverse_Mercator", 0.0,
                                         0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetTMSO(hAllProj, 0.0, 0.0, 1.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetVDG(hAllProj, 0.0, 0.0, 0.0));
  EXPECT_EQ(OGRERR_NONE, OSRSetSCH(hAllProj, 0.0, 0.0, 0.0, 6378137.0));
  OSRDestroySpatialReference(hAllProj);
}

TEST(OgrSpatialReferenceTest, FinalPushOverNinetyPercent) {
  // convertToOtherProjection and OSRConvertToOtherProjection
  OGRSpatialReference srsMerc1;
  srsMerc1.SetWellKnownGeogCS("WGS84");
  srsMerc1.SetMercator(0.0, 0.0, 1.0, 0.0, 0.0);
  OGRSpatialReference* poMerc2 =
      srsMerc1.convertToOtherProjection(SRS_PT_MERCATOR_2SP);
  ASSERT_NE(nullptr, poMerc2);
  poMerc2->Release();

  // With bound CRS
  srsMerc1.SetTOWGS84(0, 0, 0, 0, 0, 0, 0);
  OGRSpatialReference* poMerc2Bound =
      srsMerc1.convertToOtherProjection(SRS_PT_MERCATOR_2SP);
  ASSERT_NE(nullptr, poMerc2Bound);
  poMerc2Bound->Release();

  OGRSpatialReference srsMerc2;
  srsMerc2.SetWellKnownGeogCS("WGS84");
  srsMerc2.SetMercator2SP(40.0, 0.0, 0.0, 0.0, 0.0);
  OGRSpatialReference* poMerc1 =
      srsMerc2.convertToOtherProjection(SRS_PT_MERCATOR_1SP);
  ASSERT_NE(nullptr, poMerc1);
  poMerc1->Release();

  OGRSpatialReference srsLcc1;
  srsLcc1.SetWellKnownGeogCS("WGS84");
  srsLcc1.SetLCC1SP(40.0, 10.0, 1.0, 0.0, 0.0);
  OGRSpatialReference* poLcc2 =
      srsLcc1.convertToOtherProjection(SRS_PT_LAMBERT_CONFORMAL_CONIC_2SP);
  ASSERT_NE(nullptr, poLcc2);
  poLcc2->Release();

  OGRSpatialReference srsLcc2;
  srsLcc2.SetWellKnownGeogCS("WGS84");
  srsLcc2.SetLCC(30.0, 40.0, 35.0, 10.0, 0.0, 0.0);
  OGRSpatialReference* poLcc1 =
      srsLcc2.convertToOtherProjection(SRS_PT_LAMBERT_CONFORMAL_CONIC_1SP);
  ASSERT_NE(nullptr, poLcc1);
  poLcc1->Release();

  EXPECT_EQ(nullptr, srsMerc1.convertToOtherProjection(nullptr));
  EXPECT_EQ(nullptr, srsMerc1.convertToOtherProjection("INVALID"));

  // C API
  EXPECT_EQ(nullptr,
            OSRConvertToOtherProjection(nullptr, SRS_PT_MERCATOR_2SP, nullptr));
  OGRSpatialReferenceH hMerc2 = OSRConvertToOtherProjection(
      OGRSpatialReference::ToHandle(&srsMerc2), SRS_PT_MERCATOR_1SP, nullptr);
  ASSERT_NE(nullptr, hMerc2);
  OSRDestroySpatialReference(hMerc2);

  // FindMatches axis reordering branch
  OGRSpatialReference srsWgs84LonLat;
  EXPECT_EQ(OGRERR_NONE,
            srsWgs84LonLat.importFromWkt(
                "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
                "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT["
                "\"degree\","
                "0.0174532925199433],AXIS[\"Longitude\",EAST],AXIS["
                "\"Latitude\",NORTH]]"));
  srsWgs84LonLat.SetDataAxisToSRSAxisMapping({1, 2});
  int nMatchesAxis = 0;
  int* panConfAxis = nullptr;
  OGRSpatialReferenceH* pahMatchesAxis =
      srsWgs84LonLat.FindMatches(nullptr, &nMatchesAxis, &panConfAxis);
  OSRFreeSRSArray(pahMatchesAxis);
  CPLFree(panConfAxis);

  // FindBestMatch
  OGRSpatialReference srsWgs84TOWGS84;
  srsWgs84TOWGS84.importFromEPSG(4326);
  srsWgs84TOWGS84.SetTOWGS84(0, 0, 0, 0, 0, 0, 0);
  OGRSpatialReference* poBest1 = srsWgs84TOWGS84.FindBestMatch(90, "EPSG");
  if (poBest1) poBest1->Release();

  OGRSpatialReference* poBest2 = srsWgs84TOWGS84.FindBestMatch(0, nullptr);
  if (poBest2) poBest2->Release();

  OGRSpatialReference srsCustomBest;
  srsCustomBest.SetLocalCS("NonExistentCustomCoordinateSystemXYZ");
  EXPECT_EQ(nullptr, srsCustomBest.FindBestMatch(90, "EPSG"));

  OGRSpatialReference srsNoAuth;
  srsNoAuth.SetWellKnownGeogCS("WGS84");
  srsNoAuth.GetRoot()->StripNodes("AUTHORITY");
  OGRSpatialReference* poBestAuth = srsNoAuth.FindBestMatch(50, "EPSG");
  if (poBestAuth) poBestAuth->Release();

  // OSRIsSameEx
  EXPECT_EQ(0, OSRIsSameEx(nullptr, nullptr, nullptr));
  OGRSpatialReference srsSame1, srsSame2;
  srsSame1.importFromEPSG(4326);
  srsSame2.importFromEPSG(4326);
  EXPECT_EQ(1, OSRIsSameEx(OGRSpatialReference::ToHandle(&srsSame1),
                           OGRSpatialReference::ToHandle(&srsSame2), nullptr));

  // StripTOWGS84IfKnownDatum
  OGRSpatialReference srsNoBound;
  srsNoBound.importFromEPSG(4326);
  EXPECT_FALSE(srsNoBound.StripTOWGS84IfKnownDatum());

  OGRSpatialReference srsBoundKnown;
  srsBoundKnown.importFromEPSG(4326);
  srsBoundKnown.SetTOWGS84(0, 0, 0, 0, 0, 0, 0);
  EXPECT_TRUE(srsBoundKnown.StripTOWGS84IfKnownDatum());

  CPLSetConfigOption("OSR_STRIP_TOWGS84", "YES");
  OGRSpatialReference srsBoundAllowed;
  srsBoundAllowed.importFromEPSG(4326);
  srsBoundAllowed.SetTOWGS84(0, 0, 0, 0, 0, 0, 0);
  EXPECT_TRUE(srsBoundAllowed.StripTOWGS84IfKnownDatumAndAllowed());

  CPLSetConfigOption("OSR_STRIP_TOWGS84", "NO");
  EXPECT_FALSE(srsBoundAllowed.StripTOWGS84IfKnownDatumAndAllowed());
  CPLSetConfigOption("OSR_STRIP_TOWGS84", nullptr);

  // StripVertical with reuse existing bound CRS
  OGRSpatialReference srsCompBound;
  srsCompBound.SetFromUserInput("EPSG:25832+5783");
  srsCompBound.SetTOWGS84(1, 2, 3, 4, 5, 6, 7);
  EXPECT_EQ(OGRERR_NONE, srsCompBound.StripVertical());
  EXPECT_FALSE(srsCompBound.IsCompound());

  OGRSpatialReference srsNotComp;
  srsNotComp.importFromEPSG(4326);
  EXPECT_EQ(OGRERR_NONE, srsNotComp.StripVertical());

  EXPECT_EQ(OGRERR_FAILURE, OSRStripVertical(nullptr));
  OGRSpatialReferenceH hComp = OSRNewSpatialReference(nullptr);
  OSRSetFromUserInput(hComp, "EPSG:25832+5783");
  EXPECT_EQ(OGRERR_NONE, OSRStripVertical(hComp));
  OSRDestroySpatialReference(hComp);

  // GetEPSGGeogCS
  OGRSpatialReference srsEPSGGeog;
  srsEPSGGeog.importFromEPSG(4326);
  EXPECT_EQ(4326, srsEPSGGeog.GetEPSGGeogCS());

  OGRSpatialReference srsWgs84NoAuth;
  srsWgs84NoAuth.importFromEPSG(4326);
  srsWgs84NoAuth.GetRoot()->StripNodes("AUTHORITY");
  EXPECT_EQ(4326, srsWgs84NoAuth.GetEPSGGeogCS());

  EXPECT_EQ(-1, srsWgs84LonLat.GetEPSGGeogCS());

  OGRSpatialReference srsEmptyGeog;
  EXPECT_EQ(-1, srsEmptyGeog.GetEPSGGeogCS());

  OGRSpatialReference srsNorthWgs72;
  EXPECT_EQ(OGRERR_NONE,
            srsNorthWgs72.importFromWkt(
                "GEOGCS[\"WGS 72\",DATUM[\"WGS_1972\",SPHEROID[\"WGS "
                "72\",6378135,298.26]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\","
                "0.0174532925199433],AXIS[\"Latitude\",NORTH],AXIS["
                "\"Longitude\",EAST]]"));
  EXPECT_EQ(4322, srsNorthWgs72.GetEPSGGeogCS());

  OGRSpatialReference srsNorthNad27;
  EXPECT_EQ(OGRERR_NONE, srsNorthNad27.importFromWkt(
                             "GEOGCS[\"NAD27\",DATUM[\"North_American_Datum_"
                             "1927\",SPHEROID[\"Clarke "
                             "1866\",6378206.4,294.9786982]],PRIMEM["
                             "\"Greenwich\",0],UNIT[\"degree\","
                             "0.0174532925199433],AXIS[\"Latitude\",NORTH],"
                             "AXIS[\"Longitude\",EAST]]"));
  EXPECT_EQ(4267, srsNorthNad27.GetEPSGGeogCS());

  OGRSpatialReference srsNorthNad83;
  EXPECT_EQ(
      OGRERR_NONE,
      srsNorthNad83.importFromWkt(
          "GEOGCS[\"NAD83\",DATUM[\"North_American_Datum_1983\",SPHEROID[\"GRS "
          "1980\",6378137,298.257222101]],PRIMEM[\"Greenwich\",0],UNIT["
          "\"degree\","
          "0.0174532925199433],AXIS[\"Latitude\",NORTH],AXIS[\"Longitude\","
          "EAST]]"));
  EXPECT_EQ(4269, srsNorthNad83.GetEPSGGeogCS());

  OGRSpatialReference srsNorthDatum;
  EXPECT_EQ(OGRERR_NONE,
            srsNorthDatum.importFromWkt(
                "GEOGCS[\"CustomCRS\",DATUM[\"CustomDatum\",SPHEROID[\"GRS "
                "1980\",6378137,298.257222101],AUTHORITY[\"EPSG\",\"6269\"]],"
                "PRIMEM[\""
                "Greenwich\",0],UNIT[\"degree\",0.0174532925199433],AXIS["
                "\"Latitude\",NORTH],"
                "AXIS[\"Longitude\",EAST]]"));
  EXPECT_EQ(4269, srsNorthDatum.GetEPSGGeogCS());

  // EPSGTreatsAsLatLong
  OGRSpatialReference srsLatLong;
  srsLatLong.importFromEPSG(4326);
  EXPECT_TRUE(srsLatLong.EPSGTreatsAsLatLong());

  OGRSpatialReference srsProjLatLong;
  srsProjLatLong.importFromEPSG(32632);
  EXPECT_FALSE(srsProjLatLong.EPSGTreatsAsLatLong());

  EXPECT_FALSE(srsWgs84LonLat.EPSGTreatsAsLatLong());

  OGRSpatialReference srsCompLatLong;
  srsCompLatLong.SetFromUserInput("EPSG:4326+5703");
  EXPECT_TRUE(srsCompLatLong.EPSGTreatsAsLatLong());

  EXPECT_EQ(OGRERR_FAILURE, OSREPSGTreatsAsLatLong(nullptr));
  EXPECT_EQ(1,
            OSREPSGTreatsAsLatLong(OGRSpatialReference::ToHandle(&srsLatLong)));

  // OSRCalcSemiMinorFromInvFlattening and GetWGS84SRS and OSRCleanup
  EXPECT_DOUBLE_EQ(6378137.0,
                   OSRCalcSemiMinorFromInvFlattening(6378137.0, 0.0));
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_DOUBLE_EQ(-1.0, OSRCalcSemiMinorFromInvFlattening(-1.0, 298.257));
    EXPECT_DOUBLE_EQ(6378137.0,
                     OSRCalcSemiMinorFromInvFlattening(6378137.0, 0.5));
  }
  double dfSemiMinor =
      OSRCalcSemiMinorFromInvFlattening(6378137.0, 298.257223563);
  EXPECT_NEAR(6356752.3142, dfSemiMinor, 0.001);

  OGRSpatialReference* poStaticWGS84 = OGRSpatialReference::GetWGS84SRS();
  ASSERT_NE(nullptr, poStaticWGS84);
  EXPECT_TRUE(poStaticWGS84->IsGeographic());
  OSRCleanup();
  OGRSpatialReference* poStaticWGS84Again = OGRSpatialReference::GetWGS84SRS();
  ASSERT_NE(nullptr, poStaticWGS84Again);

  // SetDerivedGeogCRSWithPoleRotation*
  OGRSpatialReference srsRot1;
  srsRot1.importFromEPSG(4326);
  EXPECT_EQ(OGRERR_NONE,
            srsRot1.SetDerivedGeogCRSWithPoleRotationGRIBConvention(
                "Rotated GRIB", -30.0, 10.0, 0.0));
  EXPECT_TRUE(srsRot1.IsDerivedGeographic());

  OGRSpatialReference srsRot2;
  srsRot2.importFromEPSG(4326);
  EXPECT_EQ(OGRERR_NONE,
            srsRot2.SetDerivedGeogCRSWithPoleRotationNetCDFCFConvention(
                "Rotated NetCDF", 60.0, 10.0, 0.0));
  EXPECT_TRUE(srsRot2.IsDerivedGeographic());

  OGRSpatialReference srsProjRot;
  srsProjRot.importFromEPSG(32632);
  EXPECT_EQ(OGRERR_FAILURE,
            srsProjRot.SetDerivedGeogCRSWithPoleRotationGRIBConvention(
                "Rotated GRIB", -30.0, 10.0, 0.0));
  EXPECT_EQ(OGRERR_FAILURE,
            srsProjRot.SetDerivedGeogCRSWithPoleRotationNetCDFCFConvention(
                "Rotated NetCDF", 60.0, 10.0, 0.0));

  // SetCompoundCS error branches & C API
  OGRSpatialReference srsCompErr;
  OGRSpatialReference srsGeogComp, srsVertComp, srsLocalComp;
  srsGeogComp.importFromEPSG(4326);
  srsVertComp.importFromEPSG(5703);
  srsLocalComp.SetLocalCS("Local");

  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE,
              srsCompErr.SetCompoundCS("Fail1", &srsGeogComp, &srsGeogComp));
    EXPECT_EQ(OGRERR_FAILURE,
              srsCompErr.SetCompoundCS("Fail2", &srsLocalComp, &srsVertComp));
  }
  EXPECT_EQ(OGRERR_FAILURE,
            OSRSetCompoundCS(nullptr, "Fail", nullptr, nullptr));
  EXPECT_EQ(OGRERR_FAILURE,
            OSRSetCompoundCS(OGRSpatialReference::ToHandle(&srsCompErr), "Fail",
                             nullptr, nullptr));
  EXPECT_EQ(
      OGRERR_FAILURE,
      OSRSetCompoundCS(OGRSpatialReference::ToHandle(&srsCompErr), "Fail",
                       OGRSpatialReference::ToHandle(&srsGeogComp), nullptr));

  // SetAuthority with "GEOGCS" on projected CRS
  OGRSpatialReference srsProjAuth;
  srsProjAuth.importFromEPSG(32632);
  EXPECT_EQ(OGRERR_NONE, srsProjAuth.SetAuthority("GEOGCS", "EPSG", 4326));
  EXPECT_STREQ("32632", srsProjAuth.GetAuthorityCode(nullptr));
  EXPECT_STREQ("4326", srsProjAuth.GetAuthorityCode("GEOGCS"));

  OGRSpatialReference srsProjNoId;
  srsProjNoId.SetProjCS("Custom UTM");
  srsProjNoId.SetWellKnownGeogCS("WGS84");
  srsProjNoId.SetUTM(32, TRUE);
  EXPECT_EQ(OGRERR_NONE, srsProjNoId.SetAuthority("GEOGCS", "EPSG", 4326));
  EXPECT_STREQ("4326", srsProjNoId.GetAuthorityCode("GEOGCS"));

  EXPECT_EQ(OGRERR_NONE,
            OSRSetAuthority(OGRSpatialReference::ToHandle(&srsProjAuth),
                            "GEOGCS", "EPSG", 4326));

  // SetTargetLinearUnits and GetTargetLinearUnits
  OGRSpatialReference srsUnitsTarget;
  srsUnitsTarget.importFromEPSG(32632);
  EXPECT_EQ(OGRERR_NONE,
            srsUnitsTarget.SetTargetLinearUnits(
                "PROJCS", "Foot_US", 0.304800609601219, "EPSG", "9003"));
  EXPECT_EQ(OGRERR_FAILURE,
            srsUnitsTarget.SetTargetLinearUnits("PROJCS", "Foot_US", -1.0));
  EXPECT_EQ(OGRERR_FAILURE,
            srsUnitsTarget.SetTargetLinearUnits("NONEXISTENT", "Foot_US", 0.3));

  const char* pszTargetUnit = nullptr;
  EXPECT_NEAR(0.3048006096,
              srsUnitsTarget.GetTargetLinearUnits("PROJCS", &pszTargetUnit),
              1e-6);
  EXPECT_STREQ("Foot_US", pszTargetUnit);

  char* pszTargetUnitMut = nullptr;
  EXPECT_NEAR(0.3048006096,
              srsUnitsTarget.GetTargetLinearUnits("PROJCS", &pszTargetUnitMut),
              1e-6);

  EXPECT_DOUBLE_EQ(
      1.0, srsUnitsTarget.GetTargetLinearUnits("NONEXISTENT", &pszTargetUnit));

  EXPECT_EQ(OGRERR_NONE, OSRSetTargetLinearUnits(
                             OGRSpatialReference::ToHandle(&srsUnitsTarget),
                             "PROJCS", "Foot_US", 0.304800609601219));
  char* pszCUnit = nullptr;
  EXPECT_NEAR(
      0.3048006096,
      OSRGetTargetLinearUnits(OGRSpatialReference::ToHandle(&srsUnitsTarget),
                              "PROJCS", &pszCUnit),
      1e-6);
  EXPECT_DOUBLE_EQ(0.0, OSRGetTargetLinearUnits(nullptr, "PROJCS", nullptr));

  // 3D ellipsoidal CRS linear units
  OGRSpatialReference srs3DEllips;
  srs3DEllips.importFromEPSG(4979);
  const char* psz3DUnit = nullptr;
  EXPECT_DOUBLE_EQ(1.0, srs3DEllips.GetTargetLinearUnits(nullptr, &psz3DUnit));
  EXPECT_STREQ("metre", psz3DUnit);

  // SetNode double overload and edge cases
  OGRSpatialReference srsNodeTest;
  EXPECT_EQ(OGRERR_NONE, srsNodeTest.SetProjCS("NodeTest"));
  EXPECT_EQ(OGRERR_NONE, srsNodeTest.SetNode("PROJCS|PARAMETER_INT", 10.0));
  EXPECT_EQ(OGRERR_NONE, srsNodeTest.SetNode("PROJCS|PARAMETER_FLOAT", 10.5));
  EXPECT_EQ(OGRERR_NONE,
            srsNodeTest.SetNode("PROJCS|PARAMETER_NULL",
                                static_cast<const char*>(nullptr)));
  EXPECT_EQ(OGRERR_FAILURE, srsNodeTest.SetNode("", "X"));

  OGRSpatialReference srsEmptyNode;
  EXPECT_EQ(OGRERR_NONE, srsEmptyNode.SetNode("PROJCS", "MyProj"));
  OGRSpatialReference srsEmptyNode2;
  EXPECT_EQ(OGRERR_NONE, srsEmptyNode2.SetNode("LOCAL_CS|UNIT", "meter"));

  EXPECT_EQ(OGRERR_FAILURE, OSRSetAttrValue(nullptr, "PROJCS", "Val"));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetAttrValue(OGRSpatialReference::ToHandle(&srsNodeTest),
                            "PROJCS|PARAM", "123"));

  // SetFromUserInput extended forms
  OGRSpatialReference srsUIExt;
  EXPECT_EQ(OGRERR_NONE, srsUIExt.SetFromUserInput("   EPSG:4326"));
  EXPECT_EQ(OGRERR_NONE, srsUIExt.SetFromUserInput("EPSG:3157+4617"));
  EXPECT_EQ(OGRERR_FAILURE,
            srsUIExt.SetFromUserInput("EPSG:nonexistent+invalid"));
  EXPECT_EQ(OGRERR_NONE, srsUIExt.SetFromUserInput("AUTO:42001,9001,-100,45"));
  EXPECT_EQ(OGRERR_NONE,
            srsUIExt.SetFromUserInput("urn:adv:crs:ETRS89_UTM32*DE_DHHN92_NH"));
  EXPECT_EQ(OGRERR_NONE, srsUIExt.SetFromUserInput(
                             "urn:adv:crs:ETRS89_UTM32*DE_DHHN2016_NH"));
  EXPECT_EQ(OGRERR_NONE, srsUIExt.SetFromUserInput("fguuid:jgd2001.bl"));
  EXPECT_EQ(OGRERR_NONE, srsUIExt.SetFromUserInput("fguuid:jgd2011.bl"));
  EXPECT_EQ(OGRERR_NONE, srsUIExt.SetFromUserInput("fguuid:jgd2024.bl"));
  EXPECT_EQ(OGRERR_NONE, srsUIExt.SetFromUserInput("IGNF:LAMB93"));
  EXPECT_EQ(OGRERR_NONE, srsUIExt.SetFromUserInput("ESRI:102100"));

  // DICT:file,code
  {
    VsiMemTempWrapper dict_file("/vsimem/test_dict.txt", "<foo><bar/></foo>");
    srsUIExt.SetFromUserInput("DICT:/vsimem/test_dict.txt,123");
  }

  // Large file (> 30000 bytes)
  {
    VsiMemTempWrapper large_file("/vsimem/large_crs.txt",
                                 std::string(35000, 'X'));
    EXPECT_EQ(OGRERR_CORRUPT_DATA,
              srsUIExt.SetFromUserInput("/vsimem/large_crs.txt"));
  }

  // PROJ4 file without EXTENSION
  {
    VsiMemTempWrapper proj4_file("/vsimem/proj4_crs.txt",
                                 "+proj=utm +zone=32 +datum=WGS84\n");
    EXPECT_EQ(OGRERR_NONE, srsUIExt.SetFromUserInput("/vsimem/proj4_crs.txt"));
  }

  // ALLOW_NETWORK_ACCESS=NO option
  const char* apszNetOptions[] = {"ALLOW_NETWORK_ACCESS=NO", nullptr};
  EXPECT_EQ(OGRERR_FAILURE, srsUIExt.SetFromUserInput("http://example.com/crs",
                                                      apszNetOptions));

  // FindProjParm WKT2 query & 3D projected CRS
  OGRSpatialReference srsWkt2Param;
  EXPECT_EQ(
      OGRERR_NONE,
      srsWkt2Param.SetFromUserInput(
          "PROJCRS[\"WGS 84 / UTM zone 31N\","
          "BASEGEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\","
          "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
          "CONVERSION[\"UTM zone 31N\",METHOD[\"Transverse Mercator\"],"
          "PARAMETER[\"Latitude of natural "
          "origin\",0,ANGLEUNIT[\"degree\",0.0174532925199433]],"
          "PARAMETER[\"Longitude of natural "
          "origin\",3,ANGLEUNIT[\"degree\",0.0174532925199433]],"
          "PARAMETER[\"Scale factor at natural "
          "origin\",0.9996,SCALEUNIT[\"unity\",1]],"
          "PARAMETER[\"False easting\",500000,LENGTHUNIT[\"metre\",1]],"
          "PARAMETER[\"False northing\",0,LENGTHUNIT[\"metre\",1]]],"
          "CS[Cartesian,2],AXIS[\"(E)\",east,ORDER[1],LENGTHUNIT[\"metre\",1]],"
          "AXIS[\"(N)\",north,ORDER[2],LENGTHUNIT[\"metre\",1]]]"));
  OGRErr errParam = OGRERR_NONE;
  EXPECT_DOUBLE_EQ(0.0, srsWkt2Param.GetProjParm(SRS_PP_LATITUDE_OF_ORIGIN,
                                                 -1.0, &errParam));
  EXPECT_EQ(OGRERR_NONE, errParam);
  EXPECT_DOUBLE_EQ(
      3.0, srsWkt2Param.GetProjParm(SRS_PP_CENTRAL_MERIDIAN, -1.0, &errParam));
  EXPECT_EQ(OGRERR_NONE, errParam);

  // 3D projected CRS querying parameter
  OGRSpatialReference srs3DProjParam;
  srs3DProjParam.importFromEPSG(32632);
  srs3DProjParam.PromoteTo3D(nullptr);
  EXPECT_EQ(3, srs3DProjParam.GetAxesCount());
  EXPECT_DOUBLE_EQ(9.0, srs3DProjParam.GetProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                                   -1.0, &errParam));
  EXPECT_EQ(OGRERR_NONE, errParam);

  // GetAxesCount on compound with bound horizontal
  OGRSpatialReference srsCompAxes;
  srsCompAxes.SetFromUserInput("EPSG:32632+5703");
  srsCompAxes.SetTOWGS84(1, 2, 3, 4, 5, 6, 7);
  EXPECT_EQ(3, srsCompAxes.GetAxesCount());
  EXPECT_EQ(3, OSRGetAxesCount(OGRSpatialReference::ToHandle(&srsCompAxes)));
  EXPECT_EQ(0, OSRGetAxesCount(nullptr));
}

TEST(OgrSpatialReferenceTest, FinalPushToReachNinetyOnePercent) {
  // OSRGetCRSInfoListFromDatabase and OSRDestroyCRSInfoList
  OSRDestroyCRSInfoList(nullptr);
  int nCRSCount = 0;
  OSRCRSInfo** pCRSList =
      OSRGetCRSInfoListFromDatabase("IGNF", nullptr, &nCRSCount);
  EXPECT_NE(nullptr, pCRSList);
  EXPECT_GT(nCRSCount, 0);
  OSRDestroyCRSInfoList(pCRSList);

  // OSRGetAreaOfUse
  EXPECT_EQ(
      0, OSRGetAreaOfUse(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
  OGRSpatialReference srsAreaEPSG;
  srsAreaEPSG.importFromEPSG(4326);
  const char* pszAreaName = nullptr;
  double w = 0, s = 0, e = 0, n = 0;
  EXPECT_EQ(1, OSRGetAreaOfUse(OGRSpatialReference::ToHandle(&srsAreaEPSG), &w,
                               &s, &e, &n, &pszAreaName));

  // OSRNewSpatialReference invalid WKT
  EXPECT_EQ(nullptr, OSRNewSpatialReference("INVALID WKT"));

  // SetTOWGS84 replace existing bound CRS
  OGRSpatialReference srsDoubleTOWGS84;
  srsDoubleTOWGS84.importFromEPSG(4326);
  EXPECT_EQ(OGRERR_NONE, srsDoubleTOWGS84.SetTOWGS84(1, 2, 3, 4, 5, 6, 7));
  EXPECT_EQ(OGRERR_NONE, srsDoubleTOWGS84.SetTOWGS84(8, 9, 10, 11, 12, 13, 14));

  // SetProjection on GEOGCS
  OGRSpatialReference srsGeogProj;
  srsGeogProj.SetWellKnownGeogCS("WGS84");
  EXPECT_EQ(OGRERR_NONE, srsGeogProj.SetProjection(SRS_PT_TRANSVERSE_MERCATOR));
  EXPECT_TRUE(srsGeogProj.IsProjected());
  EXPECT_EQ(OGRERR_FAILURE,
            OSRSetProjection(nullptr, SRS_PT_TRANSVERSE_MERCATOR));
  OGRSpatialReferenceH hGeogProj = OSRNewSpatialReference(nullptr);
  OSRSetWellKnownGeogCS(hGeogProj, "WGS84");
  EXPECT_EQ(OGRERR_NONE,
            OSRSetProjection(hGeogProj, SRS_PT_TRANSVERSE_MERCATOR));
  OSRDestroySpatialReference(hGeogProj);

  // SetGeocCS on GEOGCS and rename
  OGRSpatialReference srsGeocFromGeog;
  srsGeocFromGeog.importFromEPSG(4326);
  EXPECT_EQ(OGRERR_NONE, srsGeocFromGeog.SetGeocCS("GeocFromGeog"));
  EXPECT_TRUE(srsGeocFromGeog.IsGeocentric());
  EXPECT_EQ(OGRERR_NONE, srsGeocFromGeog.SetGeocCS("RenamedGeoc"));

  // UpdateCoordinateSystemFromGeogCRS
  OGRSpatialReference srsProj3DGeog;
  EXPECT_EQ(
      OGRERR_NONE,
      srsProj3DGeog.SetFromUserInput(
          "PROJCRS[\"Proj with 3D base\","
          "BASEGEOGCRS[\"WGS 84 (3D)\",DATUM[\"World Geodetic System 1984\","
          "ELLIPSOID[\"WGS 84\",6378137,298.257223563]],"
          "CS[ellipsoidal,3],AXIS[\"lat\",north],AXIS[\"lon\",east],AXIS[\""
          "height\",up],UNIT[\"metre\",1]],"
          "CONVERSION[\"UTM 31N\",METHOD[\"Transverse Mercator\"],PARAMETER[\""
          "Latitude of natural origin\",0],"
          "PARAMETER[\"Longitude of natural origin\",3],PARAMETER[\"Scale "
          "factor at natural origin\",0.9996],"
          "PARAMETER[\"False easting\",500000],PARAMETER[\"False "
          "northing\",0]],"
          "CS[Cartesian,2],AXIS[\"easting\",east],AXIS[\"northing\",north],"
          "UNIT[\"metre\",1]]"));
  EXPECT_EQ(2, srsProj3DGeog.GetAxesCount());
  srsProj3DGeog.UpdateCoordinateSystemFromGeogCRS();
  EXPECT_EQ(3, srsProj3DGeog.GetAxesCount());

  // Compound with Bound Vertical CRS: tests IsVertical() and
  // GetDataAxisToSRSAxisMapping()
  OGRSpatialReference srsCompBoundVert;
  EXPECT_EQ(
      OGRERR_NONE,
      srsCompBoundVert.SetFromUserInput(
          "COMPOUNDCRS[\"Compound with Bound Vert\","
          "GEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\",ELLIPSOID[\""
          "WGS 84\",6378137,298.257223563]],"
          "CS[ellipsoidal,2],AXIS[\"latitude\",north],AXIS[\"longitude\",east],"
          "UNIT[\"degree\",0.0174532925199433]],"
          "BOUNDCRS["
          "SOURCECRS[VERTCRS[\"NAVD88\",VDATUM[\"North American Vertical Datum "
          "1988\"],CS[vertical,1],AXIS[\"up\",up],UNIT[\"metre\",1]]],"
          "TARGETCRS[VERTCRS[\"NAVD88 height\",VDATUM[\"North American "
          "Vertical Datum "
          "1988\"],CS[vertical,1],AXIS[\"up\",up],UNIT[\"metre\","
          "1]]],"
          "ABRIDGEDTRANSFORMATION[\"Transformation\",METHOD[\"Null\"]] ] ]"));
  EXPECT_TRUE(srsCompBoundVert.IsVertical());
  EXPECT_EQ(3, static_cast<int>(
                   srsCompBoundVert.GetDataAxisToSRSAxisMapping().size()));

  // exportToWkt with ADD_TOWGS84_ON_EXPORT_TO_WKT1 on compound CRS
  const char* const apszWktCompOpts[] = {
      "FORMAT=WKT1_GDAL", "ADD_TOWGS84_ON_EXPORT_TO_WKT1=YES", nullptr};
  char* pszWktCompBound = nullptr;
  OGRSpatialReference srsCompBoundExport;
  srsCompBoundExport.SetFromUserInput("EPSG:25832+5783");
  srsCompBoundExport.exportToWkt(&pszWktCompBound, apszWktCompOpts);
  CPLFree(pszWktCompBound);

  // exportToWkt fallback to WKT2 for Geocentric CRS
  OGRSpatialReference srsGeocExport;
  srsGeocExport.importFromEPSG(4978);
  char* pszWktGeoc = nullptr;
  srsGeocExport.exportToWkt(&pszWktGeoc);
  CPLFree(pszWktGeoc);

  // SetTargetLinearUnits on Compound CRS
  OGRSpatialReference srsCompTargetUnits;
  srsCompTargetUnits.SetFromUserInput("EPSG:32632+5703");
  EXPECT_EQ(OGRERR_NONE,
            srsCompTargetUnits.SetTargetLinearUnits("PROJCS", "Foot", 0.3048));
  EXPECT_EQ(OGRERR_NONE,
            srsCompTargetUnits.SetTargetLinearUnits("VERT_CS", "Foot", 0.3048));

  // ImportFromESRIWisconsinWKT with prjName and unitsName loop
  OGRSpatialReference srsWisTemplate;
  if (srsWisTemplate.ImportFromESRIWisconsinWKT(
          nullptr, -90.0, 44.0, nullptr,
          "NAD_1983_HARN_WISCRS_Adams_County_Feet") == OGRERR_NONE) {
    double cm = srsWisTemplate.GetProjParm(SRS_PP_CENTRAL_MERIDIAN);
    double lat = srsWisTemplate.GetProjParm(SRS_PP_LATITUDE_OF_ORIGIN);
    OGRSpatialReference srsWisLoop;
    EXPECT_EQ(OGRERR_NONE,
              srsWisLoop.ImportFromESRIWisconsinWKT(
                  "Transverse_Mercator", cm, lat, "US survey foot", nullptr));
  }
}

TEST(OgrSpatialReferenceTest, ReachingTargetCoverage) {
  // exportToProj4 on compound CRS
  OGRSpatialReference srsCompProj4;
  srsCompProj4.SetFromUserInput("EPSG:25832+5783");
  char* pszCompProj4 = nullptr;
  EXPECT_EQ(OGRERR_NONE, srsCompProj4.exportToProj4(&pszCompProj4));
  CPLFree(pszCompProj4);

  // srsCompBoundHoriz with IsDynamic, GetAxesCount, GetAxis
  OGRSpatialReference srsCompBoundHoriz;
  EXPECT_EQ(
      OGRERR_NONE,
      srsCompBoundHoriz.SetFromUserInput(
          "COMPOUNDCRS[\"Compound with Bound Horiz\","
          "BOUNDCRS["
          "SOURCECRS[PROJCRS[\"UTM\",BASEGEOGCRS[\"WGS 84\",DATUM[\"WGS "
          "84\",ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],CONVERSION[\""
          "UTM\",METHOD[\"Transverse Mercator\"]],CS[Cartesian,2],AXIS[\"e\","
          "east],AXIS[\"n\",north],UNIT[\"metre\",1]]],"
          "TARGETCRS[GEOGCRS[\"WGS 84\",DATUM[\"WGS 84\",ELLIPSOID[\"WGS "
          "84\",6378137,298.257223563]],CS[ellipsoidal,2],AXIS[\"lat\",north],"
          "AXIS[\"lon\",east],UNIT[\"degree\",0.0174532925199433]]],"
          "ABRIDGEDTRANSFORMATION[\"Transformation\",METHOD[\"Null\"]] ],"
          "VERTCRS[\"NAVD88\",VDATUM[\"North American Vertical Datum "
          "1988\"],CS[vertical,1],AXIS[\"up\",up],UNIT[\"metre\",1]] ]"));
  EXPECT_FALSE(srsCompBoundHoriz.IsDynamic());
  EXPECT_EQ(3, srsCompBoundHoriz.GetAxesCount());
  OGRAxisOrientation orient0 = OAO_Other;
  EXPECT_NE(nullptr, srsCompBoundHoriz.GetAxis(nullptr, 0, &orient0));
  OGRAxisOrientation orient2 = OAO_Other;
  EXPECT_NE(nullptr, srsCompBoundHoriz.GetAxis(nullptr, 2, &orient2));

  // srsProj3DNoId.GetAuthorityCode("PROJCS") and GetAuthorityName("PROJCS")
  OGRSpatialReference srsProj3DNoId;
  srsProj3DNoId.SetProjCS("Custom 3D Proj");
  srsProj3DNoId.SetWellKnownGeogCS("WGS84");
  srsProj3DNoId.SetUTM(32, TRUE);
  srsProj3DNoId.PromoteTo3D(nullptr);
  EXPECT_EQ(nullptr, srsProj3DNoId.GetAuthorityCode("PROJCS"));
  EXPECT_EQ(nullptr, srsProj3DNoId.GetAuthorityName("PROJCS"));

  // srs4979NoAuth.GetEPSGGeogCS()
  OGRSpatialReference srs4979NoAuth;
  srs4979NoAuth.importFromEPSG(4979);
  srs4979NoAuth.GetRoot()->StripNodes("AUTHORITY");
  EXPECT_EQ(4326, srs4979NoAuth.GetEPSGGeogCS());

  // UpdateCoordinateSystemFromGeogCRS edge cases
  OGRSpatialReference srs2DBase;
  srs2DBase.importFromEPSG(32632);
  srs2DBase.UpdateCoordinateSystemFromGeogCRS();
  OGRSpatialReference srsEmptyUpdate;
  srsEmptyUpdate.UpdateCoordinateSystemFromGeogCRS();

  // OGRsnPrintDouble locale comma replacement
  char* pszOldLocale = CPLStrdup(setlocale(LC_NUMERIC, nullptr));
  if (setlocale(LC_NUMERIC, "fr_FR.UTF-8") != nullptr ||
      setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr) {
    OGRSpatialReference srsLocale;
    srsLocale.SetProjCS("Test");
    srsLocale.SetNode("PROJCS|PARAM_LOCALE", 1.5);
    setlocale(LC_NUMERIC, pszOldLocale);
  }
  CPLFree(pszOldLocale);

  // FindMatches on simple WKT without authority
  OGRSpatialReference srsMulti;
  srsMulti.importFromWkt(
      "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
      "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\","
      "0.0174532925199433]]");
  int nEntriesMulti = 0;
  int* panConfMulti = nullptr;
  OGRSpatialReferenceH* pahMulti =
      srsMulti.FindMatches(nullptr, &nEntriesMulti, &panConfMulti);
  if (pahMulti) {
    OSRFreeSRSArray(pahMulti);
    CPLFree(panConfMulti);
  }

  // FindBestMatch with multiple candidates (Web Mercator without authority)
  OGRSpatialReference srsWebMerc;
  srsWebMerc.importFromEPSG(3857);
  srsWebMerc.GetRoot()->StripNodes("AUTHORITY");
  OGRSpatialReference* poBestEPSG = srsWebMerc.FindBestMatch(70, "EPSG");
  if (poBestEPSG) poBestEPSG->Release();
  OGRSpatialReference* poBestESRI = srsWebMerc.FindBestMatch(70, "ESRI");
  if (poBestESRI) poBestESRI->Release();

  // FindProjParm fallback to latitude_of_center and longitude_of_center
  OGRSpatialReference srsLAEA;
  srsLAEA.SetWellKnownGeogCS("WGS84");
  srsLAEA.SetLAEA(50.0, 10.0, 0.0, 0.0);
  OGRErr errLAEA = OGRERR_NONE;
  EXPECT_DOUBLE_EQ(
      50.0, srsLAEA.GetProjParm(SRS_PP_LATITUDE_OF_ORIGIN, -1.0, &errLAEA));
  EXPECT_DOUBLE_EQ(
      10.0, srsLAEA.GetProjParm(SRS_PP_CENTRAL_MERIDIAN, -1.0, &errLAEA));

  // FindProjParm fallback to longitude_of_origin
  OGRSpatialReference srsLonOrigin;
  srsLonOrigin.importFromWkt(
      "PROJCS[\"Custom\","
      "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
      "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\","
      "0.0174532925199433]],"
      "PROJECTION[\"Mercator_2SP\"],"
      "PARAMETER[\"longitude_of_origin\", 15.0],"
      "UNIT[\"metre\",1]]");
  EXPECT_DOUBLE_EQ(
      15.0, srsLonOrigin.GetProjParm(SRS_PP_CENTRAL_MERIDIAN, -1.0, &errLAEA));

  // FindProjParm WKT2 with projection centre parameters
  OGRSpatialReference srsWkt2Center;
  EXPECT_EQ(
      OGRERR_NONE,
      srsWkt2Center.SetFromUserInput(
          "PROJCRS[\"Custom WKT2 Centre\","
          "BASEGEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System "
          "1984\",ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
          "CONVERSION[\"Conv\",METHOD[\"Orthographic\"],"
          "PARAMETER[\"Latitude of projection "
          "centre\",45,ANGLEUNIT[\"degree\",0.0174532925199433]],"
          "PARAMETER[\"Longitude of projection "
          "centre\",12,ANGLEUNIT[\"degree\",0.0174532925199433]]],"
          "CS[Cartesian,2],AXIS[\"(E)\",east,ORDER[1],LENGTHUNIT[\"metre\",1]],"
          "AXIS[\"(N)\",north,ORDER[2],LENGTHUNIT[\"metre\",1]]]"));
  EXPECT_DOUBLE_EQ(45.0, srsWkt2Center.GetProjParm(SRS_PP_LATITUDE_OF_ORIGIN,
                                                   -1.0, &errLAEA));
  EXPECT_DOUBLE_EQ(
      12.0, srsWkt2Center.GetProjParm(SRS_PP_CENTRAL_MERIDIAN, -1.0, &errLAEA));
}

// TODO(schwehr): Write many more tests.

}  // namespace
}  // namespace autotest2
