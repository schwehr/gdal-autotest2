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
// Unit tests for ogrmitabspatialref.cpp.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrmitabspatialref.cpp

#include "third_party/gdal/ogr/ogrmitabspatialref.h"

#include <cstring>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(OGRMitabSpatialRefTest, ExtractCoordSysBounds) {
  double min_x = 0;
  double min_y = 0;
  double max_x = 0;
  double max_y = 0;

  EXPECT_FALSE(MITABExtractCoordSysBounds(nullptr, min_x, min_y, max_x, max_y));

  EXPECT_FALSE(MITABExtractCoordSysBounds("CoordSys Earth Projection 1, 104",
                                          min_x, min_y, max_x, max_y));

  EXPECT_FALSE(MITABExtractCoordSysBounds(
      "CoordSys Earth Projection 1, 104 Bounds (10, 20)", min_x, min_y, max_x,
      max_y));

  EXPECT_TRUE(MITABExtractCoordSysBounds(
      "CoordSys Earth Projection 1, 104 Bounds (10.5, 20.5) (30.5, 40.5)",
      min_x, min_y, max_x, max_y));
  EXPECT_DOUBLE_EQ(min_x, 10.5);
  EXPECT_DOUBLE_EQ(min_y, 20.5);
  EXPECT_DOUBLE_EQ(max_x, 30.5);
  EXPECT_DOUBLE_EQ(max_y, 40.5);

  EXPECT_TRUE(MITABExtractCoordSysBounds(
      "NonEarth Units \"m\" Bounds (-100, -200) (300, 400)", min_x, min_y,
      max_x, max_y));
  EXPECT_DOUBLE_EQ(min_x, -100.0);
  EXPECT_DOUBLE_EQ(min_y, -200.0);
  EXPECT_DOUBLE_EQ(max_x, 300.0);
  EXPECT_DOUBLE_EQ(max_y, 400.0);
}

TEST(OGRMitabSpatialRefTest, CoordSys2TABProjInfoErrorsAndBasics) {
  TABProjInfo proj;
  EXPECT_EQ(MITABCoordSys2TABProjInfo(nullptr, &proj), -1);

  {
    WithQuietHandler holder;
    EXPECT_EQ(MITABCoordSys2TABProjInfo("InvalidString", &proj), -1);
  }

  // Projection id offset adjustments: >= 1000, 2000, 3000
  EXPECT_EQ(
      MITABCoordSys2TABProjInfo("CoordSys Earth Projection 1008, 104", &proj),
      0);
  EXPECT_EQ(proj.nProjId, 8);

  EXPECT_EQ(
      MITABCoordSys2TABProjInfo("CoordSys Earth Projection 2008, 104", &proj),
      0);
  EXPECT_EQ(proj.nProjId, 8);

  EXPECT_EQ(
      MITABCoordSys2TABProjInfo("CoordSys Earth Projection 3008, 104", &proj),
      0);
  EXPECT_EQ(proj.nProjId, 8);

  // NonEarth Units with leading spaces
  EXPECT_EQ(
      MITABCoordSys2TABProjInfo("   CoordSys NonEarth Units \"km\"", &proj), 0);
  EXPECT_EQ(proj.nProjId, 0);
  EXPECT_EQ(proj.nUnitsId, 1);

  // NonEarth without explicit Units keyword
  EXPECT_EQ(MITABCoordSys2TABProjInfo("CoordSys NonEarth \"m\"", &proj), 0);
  EXPECT_EQ(proj.nProjId, 0);

  // Numeric units id
  EXPECT_EQ(MITABCoordSys2TABProjInfo(
                "CoordSys Earth Projection 8, 104, 7, 0, 0, 1, 0, 0", &proj),
            0);
  EXPECT_EQ(proj.nUnitsId, 7);

  // Unknown unit string
  EXPECT_EQ(MITABCoordSys2TABProjInfo(
                "CoordSys Earth Projection 1, 104, \"unknown_unit\"", &proj),
            0);

  // Unknown datum falls back to WGS84
  EXPECT_EQ(MITABCoordSys2TABProjInfo(
                "CoordSys Earth Projection 1, 99999, \"m\"", &proj),
            0);
  EXPECT_EQ(proj.nDatumId, 104);

  // Custom datum 999
  EXPECT_EQ(
      MITABCoordSys2TABProjInfo(
          "CoordSys Earth Projection 1, 999, 28, 10, -20, 30, \"m\"", &proj),
      0);
  EXPECT_EQ(proj.nEllipsoidId, 28);
  EXPECT_DOUBLE_EQ(proj.dDatumShiftX, 10.0);
  EXPECT_DOUBLE_EQ(proj.dDatumShiftY, -20.0);
  EXPECT_DOUBLE_EQ(proj.dDatumShiftZ, 30.0);

  // Custom datum 9999
  EXPECT_EQ(MITABCoordSys2TABProjInfo(
                "CoordSys Earth Projection 1, 9999, 28, 10, -20, 30, 1.5, 2.5, "
                "3.5, 4.5, 5.5, \"m\"",
                &proj),
            0);
  EXPECT_EQ(proj.nEllipsoidId, 28);
  EXPECT_DOUBLE_EQ(proj.dDatumShiftX, 10.0);
  EXPECT_DOUBLE_EQ(proj.adDatumParams[0], 1.5);
  EXPECT_DOUBLE_EQ(proj.adDatumParams[1], 2.5);
  EXPECT_DOUBLE_EQ(proj.adDatumParams[2], 3.5);
  EXPECT_DOUBLE_EQ(proj.adDatumParams[3], 4.5);
  EXPECT_DOUBLE_EQ(proj.adDatumParams[4], 5.5);
}

TEST(OGRMitabSpatialRefTest, NonEarthCoordSysRoundtrip) {
  const char* mif = "NonEarth Units \"m\" Bounds (0, 0) (1000, 2000)";
  OGRSpatialReference* poSRS = MITABCoordSys2SpatialRef(mif);
  ASSERT_NE(poSRS, nullptr);
  EXPECT_TRUE(poSRS->IsLocal());

  char* coordsys = MITABSpatialRef2CoordSys(poSRS);
  ASSERT_NE(coordsys, nullptr);
  EXPECT_TRUE(strstr(coordsys, "NonEarth Units \"m\"") != nullptr);
  CPLFree(coordsys);
  delete poSRS;
}

TEST(OGRMitabSpatialRefTest, LatLongWGS84Roundtrip) {
  const char* mif = "Earth Projection 1, 104";
  OGRSpatialReference* poSRS = MITABCoordSys2SpatialRef(mif);
  ASSERT_NE(poSRS, nullptr);
  EXPECT_TRUE(poSRS->IsGeographic());

  char* coordsys = MITABSpatialRef2CoordSys(poSRS);
  ASSERT_NE(coordsys, nullptr);
  EXPECT_STREQ(coordsys, "Earth Projection 1, 104");
  CPLFree(coordsys);
  delete poSRS;
}

TEST(OGRMitabSpatialRefTest, CustomDatum999Roundtrip) {
  const char* mif = "Earth Projection 1, 999, 28, 15, -25, 35";
  OGRSpatialReference* poSRS = MITABCoordSys2SpatialRef(mif);
  ASSERT_NE(poSRS, nullptr);

  char* coordsys = MITABSpatialRef2CoordSys(poSRS);
  ASSERT_NE(coordsys, nullptr);
  EXPECT_TRUE(strstr(coordsys, "Earth Projection 1, 9999, 28, 15, -25, 35") !=
                  nullptr ||
              strstr(coordsys, "Earth Projection 1, 999, 28, 15, -25, 35") !=
                  nullptr);
  CPLFree(coordsys);
  delete poSRS;
}

TEST(OGRMitabSpatialRefTest, CustomDatum9999PrimeMeridian) {
  // Test Paris prime meridian (~2.337229166667)
  const char* mif_paris =
      "Earth Projection 1, 9999, 28, 0, 0, 0, 0, 0, 0, 0, 2.337229166667";
  OGRSpatialReference* poSRS = MITABCoordSys2SpatialRef(mif_paris);
  ASSERT_NE(poSRS, nullptr);
  EXPECT_NEAR(poSRS->GetPrimeMeridian(), 2.337229166667, 1e-6);

  char* coordsys = MITABSpatialRef2CoordSys(poSRS);
  ASSERT_NE(coordsys, nullptr);
  CPLFree(coordsys);
  delete poSRS;

  // Test non-Greenwich other prime meridian
  const char* mif_other =
      "Earth Projection 1, 9999, 28, 0, 0, 0, 0, 0, 0, 0, 10.5";
  poSRS = MITABCoordSys2SpatialRef(mif_other);
  ASSERT_NE(poSRS, nullptr);
  EXPECT_DOUBLE_EQ(poSRS->GetPrimeMeridian(), 10.5);

  coordsys = MITABSpatialRef2CoordSys(poSRS);
  ASSERT_NE(coordsys, nullptr);
  CPLFree(coordsys);
  delete poSRS;
}

TEST(OGRMitabSpatialRefTest, KnownDatumWithTOWGS84Config) {
  CPLSetConfigOption("MITAB_SET_TOWGS84_ON_KNOWN_DATUM", "YES");
  // Test datum 74 (dfDatumParm0 == 0)
  OGRSpatialReference* poSRS =
      MITABCoordSys2SpatialRef("Earth Projection 1, 74");
  ASSERT_NE(poSRS, nullptr);
  double towgs[7] = {0};
  EXPECT_EQ(poSRS->GetTOWGS84(towgs), OGRERR_NONE);
  delete poSRS;

  // Test datum 1000 (dfDatumParm0 != 0, tests line 1305)
  poSRS = MITABCoordSys2SpatialRef("Earth Projection 1, 1000");
  ASSERT_NE(poSRS, nullptr);
  EXPECT_EQ(poSRS->GetTOWGS84(towgs), OGRERR_NONE);
  delete poSRS;

  CPLSetConfigOption("MITAB_SET_TOWGS84_ON_KNOWN_DATUM", nullptr);
}

TEST(OGRMitabSpatialRefTest, AllUnits) {
  const char* units[] = {"\"mi\"",        "\"km\"",         "\"in\"", "\"ft\"",
                         "\"yd\"",        "\"mm\"",         "\"cm\"", "\"m\"",
                         "\"survey ft\"", "\"nmi\"",        "\"li\"", "\"ch\"",
                         "\"rd\"",        "\"survey foot\""};

  for (const char* u : units) {
    std::string mif = std::string("Earth Projection 8, 104, ") + u +
                      ", -122, 47, 0.9996, 500000, 0";
    OGRSpatialReference* poSRS = MITABCoordSys2SpatialRef(mif.c_str());
    ASSERT_NE(poSRS, nullptr) << "Failed for unit " << u;

    char* coordsys = MITABSpatialRef2CoordSys(poSRS);
    ASSERT_NE(coordsys, nullptr);
    CPLFree(coordsys);
    delete poSRS;
  }

  // Unknown unit id in TABProjInfo
  TABProjInfo sProj;
  memset(&sProj, 0, sizeof(sProj));
  sProj.nProjId = 8;
  sProj.nUnitsId = 250;
  OGRSpatialReference* poSRS = TABFileGetSpatialRefFromTABProj(sProj);
  ASSERT_NE(poSRS, nullptr);
  delete poSRS;
}

TEST(OGRMitabSpatialRefTest, UnknownEllipsoidAndDefaultProjection) {
  // Ellipsoid 255 does not exist in asSpheroidInfoList
  TABProjInfo sProj;
  memset(&sProj, 0, sizeof(sProj));
  sProj.nProjId = 1;
  sProj.nEllipsoidId = 255;
  sProj.nDatumId = 999;
  sProj.dDatumShiftX = 10;
  OGRSpatialReference* poSRS = TABFileGetSpatialRefFromTABProj(sProj);
  ASSERT_NE(poSRS, nullptr);
  delete poSRS;

  // Datum with empty name in asDatumInfoList (Datum 0)
  memset(&sProj, 0, sizeof(sProj));
  sProj.nProjId = 1;
  sProj.nDatumId = 0;
  sProj.nEllipsoidId = 29;
  poSRS = TABFileGetSpatialRefFromTABProj(sProj);
  ASSERT_NE(poSRS, nullptr);
  delete poSRS;

  // Unknown projection method (e.g. 99)
  sProj.nProjId = 99;
  sProj.nEllipsoidId = 28;
  sProj.nDatumId = 104;
  poSRS = TABFileGetSpatialRefFromTABProj(sProj);
  ASSERT_NE(poSRS, nullptr);
  delete poSRS;
}

TEST(OGRMitabSpatialRefTest, SpecialCases) {
  // Google Mercator (datum=157, ellipsoid=54, proj=10)
  TABProjInfo sProj;
  memset(&sProj, 0, sizeof(sProj));
  sProj.nProjId = 10;
  sProj.nDatumId = 157;
  sProj.nEllipsoidId = 54;
  OGRSpatialReference* poSRS = TABFileGetSpatialRefFromTABProj(sProj);
  ASSERT_NE(poSRS, nullptr);
  delete poSRS;

  // France Lambert-93 (proj=3, datum=33, ellipsoid=0, lon=3, lat=46.5)
  memset(&sProj, 0, sizeof(sProj));
  sProj.nProjId = 3;
  sProj.nDatumId = 33;
  sProj.nEllipsoidId = 0;
  sProj.adProjParams[0] = 3.0;
  sProj.adProjParams[1] = 46.5;
  sProj.adProjParams[2] = 44.0;
  sProj.adProjParams[3] = 49.0;
  sProj.adProjParams[4] = 700000.0;
  sProj.adProjParams[5] = 6600000.0;
  poSRS = TABFileGetSpatialRefFromTABProj(sProj);
  ASSERT_NE(poSRS, nullptr);
  delete poSRS;

  // Predefined LCC EPSG 2154 lookup in TABFileGetSpatialRefFromTABProj
  // (hits lines 1402, 1403, 1438-1444)
  memset(&sProj, 0, sizeof(sProj));
  sProj.nProjId = 3;
  sProj.nDatumId = 33;
  sProj.nEllipsoidId = 0;
  sProj.adProjParams[0] = 3.0;
  sProj.adProjParams[1] = 46.5;
  sProj.adProjParams[2] = 44.0;
  sProj.adProjParams[3] = 49.0;
  sProj.adProjParams[4] = 700000.0;
  sProj.adProjParams[5] = 6600000.0;
  poSRS = TABFileGetSpatialRefFromTABProj(sProj);
  ASSERT_NE(poSRS, nullptr);
  delete poSRS;

  // Predefined LCC Estonian CS 1997 lookup (hits lines 1397-1398)
  memset(&sProj, 0, sizeof(sProj));
  sProj.nProjId = 3;
  sProj.nDatumId = 115;
  sProj.nEllipsoidId = 0;
  sProj.adProjParams[0] = 24.0;
  sProj.adProjParams[1] = 57.51755393056;
  sProj.adProjParams[2] = 58.0;
  sProj.adProjParams[3] = 59.3333333333333;
  sProj.adProjParams[4] = 500000.0;
  sProj.adProjParams[5] = 6375000.0;
  poSRS = TABFileGetSpatialRefFromTABProj(sProj);
  ASSERT_NE(poSRS, nullptr);
  delete poSRS;

  // Predefined LCC with nEPSGCode == 0 (hits line 1419)
  memset(&sProj, 0, sizeof(sProj));
  sProj.nProjId = 3;
  sProj.nDatumId = 33;
  sProj.nEllipsoidId = 0;
  sProj.adProjParams[0] = 15.5;
  sProj.adProjParams[1] = 0;
  sProj.adProjParams[2] = 56.6666666667;
  sProj.adProjParams[3] = 59.3333333333;
  poSRS = TABFileGetSpatialRefFromTABProj(sProj);
  ASSERT_NE(poSRS, nullptr);
  delete poSRS;

  // LCC with 1SP candidate (hits lines 1364-1366)
  memset(&sProj, 0, sizeof(sProj));
  sProj.nProjId = 3;
  sProj.nDatumId = 104;
  sProj.nEllipsoidId = 28;
  sProj.adProjParams[0] = 10.0;
  sProj.adProjParams[1] = 45.0;
  sProj.adProjParams[2] = 45.0;
  sProj.adProjParams[3] = 45.0;
  poSRS = TABFileGetSpatialRefFromTABProj(sProj);
  ASSERT_NE(poSRS, nullptr);
  delete poSRS;

  // Philippine Reference System 1992 (datum=1031, ellipsoid=8)
  memset(&sProj, 0, sizeof(sProj));
  sProj.nProjId = 1;
  sProj.nDatumId = 1031;
  sProj.nEllipsoidId = 8;
  poSRS = TABFileGetSpatialRefFromTABProj(sProj);
  ASSERT_NE(poSRS, nullptr);
  delete poSRS;

  // Kertau (datum=164, ellipsoid=40)
  memset(&sProj, 0, sizeof(sProj));
  sProj.nProjId = 1;
  sProj.nDatumId = 164;
  sProj.nEllipsoidId = 40;
  poSRS = TABFileGetSpatialRefFromTABProj(sProj);
  ASSERT_NE(poSRS, nullptr);
  delete poSRS;
}

TEST(OGRMitabSpatialRefTest, AllProjectionMethods) {
  // Struct to test all projection types in both directions
  struct ProjCase {
    int nProjId;
    const char* coordSys;
  };

  const ProjCase cases[] = {
      // Proj 2: Cylindrical Equal Area
      {2, "Earth Projection 2, 104, \"m\", 10, 20, 1000, 2000"},
      // Proj 3: LCC
      {3, "Earth Projection 3, 104, \"m\", -100, 40, 30, 45, 500000, 0"},
      // Proj 4: LAEA
      {4, "Earth Projection 4, 104, \"m\", 10, 50"},
      // Proj 5: Azimuthal Equidistant (polar)
      {5, "Earth Projection 5, 104, \"m\", 0, 90, 90"},
      // Proj 6: Equidistant Conic
      {6, "Earth Projection 6, 104, \"m\", 10, 40, 30, 45, 500000, 0"},
      // Proj 7: Hotine Oblique Mercator
      {7, "Earth Projection 7, 104, \"m\", 100, 4, 50, 0.9996, 500000, 0"},
      // Proj 8: Transverse Mercator
      {8, "Earth Projection 8, 104, \"m\", 10, 0, 0.9996, 500000, 0"},
      // Proj 9: Albers Conic Equal Area
      {9, "Earth Projection 9, 104, \"m\", -96, 23, 29.5, 45.5, 0, 0"},
      // Proj 10: Mercator
      {10, "Earth Projection 10, 104, \"m\", 0"},
      // Proj 11: Miller Cylindrical
      {11, "Earth Projection 11, 104, \"m\", 0"},
      // Proj 12: Robinson
      {12, "Earth Projection 12, 104, \"m\", 0"},
      // Proj 13: Mollweide
      {13, "Earth Projection 13, 104, \"m\", 0"},
      // Proj 14: Eckert IV
      {14, "Earth Projection 14, 104, \"m\", 0"},
      // Proj 15: Eckert VI
      {15, "Earth Projection 15, 104, \"m\", 0"},
      // Proj 16: Sinusoidal
      {16, "Earth Projection 16, 104, \"m\", 0"},
      // Proj 17: Gall Stereographic
      {17, "Earth Projection 17, 104, \"m\", 0"},
      // Proj 18: New Zealand Map Grid
      {18, "Earth Projection 18, 104, \"m\", 173, -41, 2510000, 6023150"},
      // Proj 19: LCC Belgium
      {19,
       "Earth Projection 19, 104, \"m\", 4.36, 90, 49.83, 51.16, 150000, "
       "5400000"},
      // Proj 20: Stereographic
      {20, "Earth Projection 20, 104, \"m\", 10, 50, 1.0, 0, 0"},
      // Proj 21: TM Danish System 34 Jylland-Fyn
      {21, "Earth Projection 21, 104, \"m\", 9, 0, 1.0, 500000, 0"},
      // Proj 22: TM Danish System 34 Sjaelland
      {22, "Earth Projection 22, 104, \"m\", 12, 0, 1.0, 500000, 0"},
      // Proj 23: TM Danish System 34/45 Bornholm
      {23, "Earth Projection 23, 104, \"m\", 15, 0, 1.0, 500000, 0"},
      // Proj 24: TM Finnish KKJ
      {24, "Earth Projection 24, 104, \"m\", 27, 0, 1.0, 3500000, 0"},
      // Proj 25: Swiss Oblique Mercator
      {25,
       "Earth Projection 25, 104, \"m\", 7.43958333333333, "
       "46.9524055555556, 600000, 200000"},
      // Proj 26: Regional Mercator
      {26, "Earth Projection 26, 104, \"m\", 0, 45"},
      // Proj 27: Polyconic
      {27, "Earth Projection 27, 104, \"m\", -96, 30, 0, 0"},
      // Proj 28: Azimuthal Equidistant (non-polar)
      {28, "Earth Projection 28, 104, \"m\", 10, 45, 90"},
      // Proj 29: LAEA (variant)
      {29, "Earth Projection 29, 104, \"m\", 10, 50"},
      // Proj 30: Cassini/Soldner
      {30, "Earth Projection 30, 104, \"m\", -61, 10.5, 0, 0"},
      // Proj 31: Oblique Stereographic
      {31, "Earth Projection 31, 104, \"m\", 19, 52, 0.9998, 500000, 500000"},
      // Proj 32: Krovak
      {32,
       "Earth Projection 32, 104, \"m\", 24.8333333333333, "
       "49.5, 30.2881397222222, 78.5, 0, 0"},
      // Proj 33: Equirectangular
      {33, "Earth Projection 33, 104, \"m\", 0, 0, 0, 0"},
      // Proj 34: Extended Transverse Mercator
      {34, "Earth Projection 34, 104, \"m\", 10, 0, 0.9996, 500000, 0"},
      // Proj 35: HOM with rectified grid angle
      {35,
       "Earth Projection 35, 104, \"m\", 100, 4, 50, 45, 0.9996, 500000, "
       "0"},
  };

  for (const auto& c : cases) {
    OGRSpatialReference* poSRS = MITABCoordSys2SpatialRef(c.coordSys);
    ASSERT_NE(poSRS, nullptr) << "Failed to parse: " << c.coordSys;

    char* coordsys = MITABSpatialRef2CoordSys(poSRS);
    ASSERT_NE(coordsys, nullptr) << "Failed to export: " << c.coordSys;
    CPLFree(coordsys);

    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(poSRS, sProj, nParamCount), 0)
        << "Failed TABFileGetTABProjFromSpatialRef for: " << c.coordSys;

    delete poSRS;
  }
}

TEST(OGRMitabSpatialRefTest, SpatialRef2CoordSysEdgeCases) {
  EXPECT_EQ(MITABSpatialRef2CoordSys(nullptr), nullptr);

  // Invalid MIF string returns nullptr in MITABCoordSys2SpatialRef (line 2330)
  {
    WithQuietHandler holder;
    EXPECT_EQ(MITABCoordSys2SpatialRef("InvalidCoordSysString"), nullptr);
  }

  // Unknown projection warning
  {
    WithQuietHandler holder;
    OGRSpatialReference oSRS;
    oSRS.SetProjection("UnknownProjectionName");
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
  }

  // EPSG:3857 (Google Mercator)
  {
    OGRSpatialReference oSRS;
    oSRS.importFromEPSG(3857);
    char* cs = MITABSpatialRef2CoordSys(&oSRS);
    ASSERT_NE(cs, nullptr);
    EXPECT_TRUE(strstr(cs, "Earth Projection 10, 157") != nullptr);
    CPLFree(cs);
  }

  // Proj4 extension for Google Mercator
  {
    OGRSpatialReference oSRS;
    oSRS.SetExtension(
        nullptr, "PROJ4",
        "+proj=merc +a=6378137 +b=6378137 +lat_ts=0.0 +lon_0=0.0 +x_0=0.0 "
        "+y_0=0 +k=1.0 +units=m +nadgrids=@null +wktext  +no_defs");
    char* cs = MITABSpatialRef2CoordSys(&oSRS);
    ASSERT_NE(cs, nullptr);
    CPLFree(cs);
  }

  // Proj4 extension alternate for Google Mercator (hits lines 2260-2261)
  {
    OGRSpatialReference oSRS;
    oSRS.SetExtension(
        nullptr, "PROJ4",
        "+proj=merc +a=6378137 +b=6378137 +lat_ts=0 +lon_0=0 +x_0=0 "
        "+y_0=0 +k=1 +units=m +nadgrids=@null +wktext +no_defs");
    char* cs = MITABSpatialRef2CoordSys(&oSRS);
    ASSERT_NE(cs, nullptr);
    CPLFree(cs);
  }

  // Swiss Oblique expressed as HOM Azimuth Center
  {
    OGRSpatialReference oSRS;
    oSRS.SetHOMAC(46.95, 7.43, 90.0, 90.0, 1.0, 600000, 200000);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nProjId, 25);
  }

  // Swiss Oblique Cylindrical directly (hits lines 1871-1878)
  {
    OGRSpatialReference oSRS;
    oSRS.SetSOC(46.95, 7.43, 600000, 200000);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nProjId, 25);
  }

  // TM variants 21, 22, 23, 24 (hits lines 1961-2008)
  for (const char* pt :
       {SRS_PT_TRANSVERSE_MERCATOR_MI_21, SRS_PT_TRANSVERSE_MERCATOR_MI_22,
        SRS_PT_TRANSVERSE_MERCATOR_MI_23, SRS_PT_TRANSVERSE_MERCATOR_MI_24}) {
    OGRSpatialReference oSRS;
    oSRS.SetProjection(pt);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
  }

  // LCC 1SP conversion in TABFileGetTABProjFromSpatialRef
  {
    OGRSpatialReference oSRS;
    oSRS.SetLCC1SP(46.5, 3.0, 1.0, 700000, 6600000);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nProjId, 3);
  }

  // Finnish KKJ datum special case
  {
    OGRSpatialReference oSRS;
    oSRS.SetTM(0, 27, 1.0, 3500000, 0);
    oSRS.SetGeogCS("KKJ", "Kartastokoordinaattijarjestelma_1966",
                   "International 1924", 6378388, 297);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nProjId, 24);
  }

  // Predefined LCC tables (e.g. Estonian CS 1997 EPSG 3301 with StdP swap)
  {
    OGRSpatialReference oSRS;
    oSRS.importFromEPSG(3301);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nProjId, 3);
  }

  // Predefined LCC with reversed standard parallels (e.g. EPSG 2154)
  {
    OGRSpatialReference oSRS;
    oSRS.importFromEPSG(2154);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nProjId, 3);
  }

  // Predefined LCC with non-reversed standard parallels (e.g. EPSG 2246)
  {
    OGRSpatialReference oSRS;
    oSRS.importFromEPSG(2246);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nProjId, 3);
  }

  // Mercator 1SP with lat_ts != 0 -> Regional Mercator (Proj 26, hits lines
  // 1829-1831)
  {
    OGRSpatialReference oSRS;
    oSRS.SetMercator(45.0, 0.0, 1.0, 0.0, 0.0);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nProjId, 26);
  }

  // Mercator 1SP with lat_ts == 0 (Proj 10)
  {
    OGRSpatialReference oSRS;
    oSRS.SetMercator(0.0, 0.0, 1.0, 0.0, 0.0);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nProjId, 10);
  }

  // Azimuthal Equidistant oblique -> Proj 28
  {
    OGRSpatialReference oSRS;
    oSRS.SetAE(45.0, 10.0, 0.0, 0.0);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nProjId, 28);
  }

  // Custom linear units (hits line 1569 and 2310)
  {
    OGRSpatialReference oSRS;
    oSRS.SetProjection(SRS_PT_TRANSVERSE_MERCATOR);
    oSRS.SetLinearUnits("CustomUnit", 42.0);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nUnitsId, 7);

    oSRS.SetLinearUnits("ZeroUnit", 0.0);
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
  }

  // Authority code on CRS or GEOGCS without datum authority (hits lines
  // 1597-1604)
  {
    OGRSpatialReference oSRS;
    oSRS.SetAuthority(nullptr, "EPSG", 4326);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
  }
  {
    OGRSpatialReference oSRS;
    oSRS.SetProjection(SRS_PT_TRANSVERSE_MERCATOR);
    oSRS.SetAuthority("GEOGCS", "EPSG", 4326);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
  }

  // WKT Datum starting with "MIF 9999," and "MIF 28"
  {
    OGRSpatialReference oSRS;
    oSRS.SetGeogCS("unnamed", "MIF 28", "Clarke 1866", 6378206.4, 294.9786982);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nDatumId, 28);
  }

  // Unknown MIF datum number (hits lines 2101, 2105-2106)
  {
    OGRSpatialReference oSRS;
    oSRS.SetGeogCS("unnamed", "MIF 99999", "Clarke 1866", 6378206.4,
                   294.9786982);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nDatumId, 104);
  }

  // MIF 999 with < 5 fields (hits lines 2138, 2142-2143)
  {
    OGRSpatialReference oSRS;
    oSRS.SetGeogCS("unnamed", "MIF 999,28", "Clarke 1866", 6378206.4,
                   294.9786982);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
  }

  // Unknown real datum with no TOWGS84 (hits lines 1480, 2169, 2173-2174)
  {
    OGRSpatialReference oSRS;
    oSRS.SetGeogCS("unnamed", "TotallyUnknownDatum", "Clarke 1866", 6378206.4,
                   294.9786982);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nDatumId, 104);
  }

  {
    OGRSpatialReference oSRS;
    oSRS.SetGeogCS("unnamed", "MIF 9999,28,10,20,30,1,2,3,4,5", "Clarke 1866",
                   6378206.4, 294.9786982);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nDatumId, 9999);
    EXPECT_DOUBLE_EQ(sProj.dDatumShiftX, 10.0);
  }

  // Custom datum with TOWGS84
  {
    OGRSpatialReference oSRS;
    oSRS.SetGeogCS("unnamed", "CustomDatum", "Bessel 1841", 6377397.155,
                   299.1528128);
    oSRS.SetTOWGS84(10, 20, 30, 1, 2, 3, 4);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nDatumId, 9999);
    EXPECT_DOUBLE_EQ(sProj.dDatumShiftX, 10.0);
  }

  // Custom datum with TOWGS84 matching spheroid by parameters (hits lines
  // 1505-1518)
  {
    OGRSpatialReference oSRS;
    oSRS.SetGeogCS("unnamed", "CustomDatum2", "UnknownSpheroidName", 6378137.0,
                   298.257223563);
    oSRS.SetTOWGS84(1, 2, 3, 4, 5, 6, 7);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nEllipsoidId, 28);
  }

  // Custom datum with TOWGS84 and unknown spheroid parameters (hits lines
  // 1521-1526)
  {
    OGRSpatialReference oSRS;
    oSRS.SetGeogCS("unnamed", "CustomDatum3", "UnknownSpheroidName2", 6370000.0,
                   300.0);
    oSRS.SetTOWGS84(1, 2, 3, 4, 5, 6, 7);
    TABProjInfo sProj;
    int nParamCount = 0;
    EXPECT_EQ(TABFileGetTABProjFromSpatialRef(&oSRS, sProj, nParamCount), 0);
    EXPECT_EQ(sProj.nEllipsoidId, 28);
  }
}

TEST(OGRMitabSpatialRefTest, BoundsFromCoordSysTable) {
  // Write a test coordsys table into /vsimem/
  constexpr char kVsiPath[] = "/vsimem/mitab_test_bounds.txt";
  constexpr std::string_view kTableContent =
      "CoordSys Earth Projection 8, 104, \"m\", 10, 0, 0.9996, 500000, 0 "
      "Bounds (100, 200) (300, 400)\n"
      "CoordSys Earth Projection 8, 74, \"m\", 15, 0, 0.9996, 500000, 0 "
      "Bounds (100.5, 200.5) (300.5, 400.5)\n";

  {
    VsiMemTempWrapper file(kVsiPath, std::string(kTableContent));

    EXPECT_EQ(MITABLoadCoordSysTable(kVsiPath), 0);

    // Integer bounds (hits lines 2433-2442)
    {
      OGRSpatialReference oSRS;
      oSRS.SetTM(0, 10, 0.9996, 500000, 0);
      oSRS.SetWellKnownGeogCS("WGS84");
      char* cs = MITABSpatialRef2CoordSys(&oSRS);
      ASSERT_NE(cs, nullptr);
      EXPECT_TRUE(strstr(cs, "Bounds (100, 200) (300, 400)") != nullptr);
      CPLFree(cs);
    }

    // Float bounds (hits lines 2445-2448)
    {
      OGRSpatialReference oSRS;
      oSRS.SetTM(0, 15, 0.9996, 500000, 0);
      oSRS.SetWellKnownGeogCS("NAD83");
      char* cs = MITABSpatialRef2CoordSys(&oSRS);
      ASSERT_NE(cs, nullptr);
      EXPECT_TRUE(strstr(cs,
                         "Bounds (100.500000, 200.500000) (300.500000, "
                         "400.500000)") != nullptr);
      CPLFree(cs);
    }

    MITABFreeCoordSysTable();
  }
}

}  // namespace
}  // namespace autotest2
