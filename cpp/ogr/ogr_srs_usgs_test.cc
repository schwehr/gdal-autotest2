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

#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class OgrSrsUsgsTest : public ::testing::Test {
 protected:
  void TearDown() override { OSRCleanup(); }
};

TEST_F(OgrSrsUsgsTest, CApiPointersAndErrors) {
  WithQuietHandler quiet_handler;
  OGRSpatialReference srs;
  double params[15] = {0};

  // Null handle checks
  EXPECT_EQ(OGRERR_FAILURE, OSRImportFromUSGS(nullptr, 0, 0, params, 0));
  long proj_sys = 0;
  long zone = 0;
  double *prj_param_out = nullptr;
  long datum = 0;
  EXPECT_EQ(OGRERR_FAILURE,
            OSRExportToUSGS(nullptr, &proj_sys, &zone, &prj_param_out, &datum));

  // Null params check
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromUSGS(8, 0, nullptr, 15, TRUE));
  EXPECT_EQ(OGRERR_CORRUPT_DATA, OSRImportFromUSGS(&srs, 8, 0, nullptr, 15));

  // Valid C API import and export
  EXPECT_EQ(OGRERR_NONE, OSRImportFromUSGS(&srs, 0, 0, params, 12));
  EXPECT_EQ(OGRERR_NONE,
            OSRExportToUSGS(&srs, &proj_sys, &zone, &prj_param_out, &datum));
  EXPECT_EQ(proj_sys, 0);
  EXPECT_EQ(datum, 12);
  CPLFree(prj_param_out);
}

TEST_F(OgrSrsUsgsTest, AngleUnpackingFormats) {
  double params[15] = {0};

  // USGS_ANGLE_DECIMALDEGREES
  {
    OGRSpatialReference srs;
    params[2] = 20.0;
    params[3] = 60.0;
    params[4] = -96.0;
    params[5] = 40.0;
    EXPECT_EQ(OGRERR_NONE,
              srs.importFromUSGS(3, 0, params, 12, USGS_ANGLE_DECIMALDEGREES));
    EXPECT_NEAR(srs.GetProjParm(SRS_PP_STANDARD_PARALLEL_1), 20.0, 1e-5);
    EXPECT_NEAR(srs.GetProjParm(SRS_PP_STANDARD_PARALLEL_2), 60.0, 1e-5);
  }

  // USGS_ANGLE_RADIANS
  {
    OGRSpatialReference srs;
    params[2] = 20.0 * M_PI / 180.0;
    params[3] = 60.0 * M_PI / 180.0;
    params[4] = -96.0 * M_PI / 180.0;
    params[5] = 40.0 * M_PI / 180.0;
    EXPECT_EQ(OGRERR_NONE,
              srs.importFromUSGS(3, 0, params, 12, USGS_ANGLE_RADIANS));
    EXPECT_NEAR(srs.GetProjParm(SRS_PP_STANDARD_PARALLEL_1), 20.0, 1e-5);
    EXPECT_NEAR(srs.GetProjParm(SRS_PP_STANDARD_PARALLEL_2), 60.0, 1e-5);
  }

  // USGS_ANGLE_PACKEDDMS
  {
    OGRSpatialReference srs;
    params[2] = CPLDecToPackedDMS(20.0);
    params[3] = CPLDecToPackedDMS(60.0);
    params[4] = CPLDecToPackedDMS(-96.0);
    params[5] = CPLDecToPackedDMS(40.0);
    EXPECT_EQ(OGRERR_NONE,
              srs.importFromUSGS(3, 0, params, 12, USGS_ANGLE_PACKEDDMS));
    EXPECT_NEAR(srs.GetProjParm(SRS_PP_STANDARD_PARALLEL_1), 20.0, 1e-5);
    EXPECT_NEAR(srs.GetProjParm(SRS_PP_STANDARD_PARALLEL_2), 60.0, 1e-5);
  }
}

TEST_F(OgrSrsUsgsTest, ImportAllProjections) {
  double p[15] = {0};

  // GEO (0)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(0, 0, p, 12));
    EXPECT_TRUE(srs.IsGeographic());
  }

  // UTM (1) - explicit north zone
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(1, 10, p, 12));
    int north = 0;
    EXPECT_EQ(srs.GetUTMZone(&north), 10);
    EXPECT_TRUE(north);
  }

  // UTM (1) - explicit south zone
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(1, -10, p, 12));
    int north = 1;
    EXPECT_EQ(srs.GetUTMZone(&north), 10);
    EXPECT_FALSE(north);
  }

  // UTM (1) - zone from padfPrjParams[2]
  {
    OGRSpatialReference srs;
    p[2] = 15;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(1, 0, p, 12));
    int north = 0;
    EXPECT_EQ(srs.GetUTMZone(&north), 15);
    p[2] = 0;
  }

  // UTM (1) - computed zone from lon/lat (north and south)
  {
    OGRSpatialReference srs_n;
    p[0] = CPLDecToPackedDMS(122.0);
    p[1] = CPLDecToPackedDMS(37.0);
    EXPECT_EQ(OGRERR_NONE, srs_n.importFromUSGS(1, 0, p, 12));
    int north = 0;
    EXPECT_EQ(srs_n.GetUTMZone(&north), 51);
    EXPECT_TRUE(north);

    OGRSpatialReference srs_s;
    p[0] = CPLDecToPackedDMS(-70.0);
    p[1] = CPLDecToPackedDMS(-33.0);
    EXPECT_EQ(OGRERR_NONE, srs_s.importFromUSGS(1, 0, p, 12));
    EXPECT_EQ(srs_s.GetUTMZone(&north), 19);
    EXPECT_FALSE(north);
    p[0] = 0;
    p[1] = 0;
  }

  // UTM (1) - invalid zone
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromUSGS(1, 100, p, 12));
    EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromUSGS(1, -100, p, 12));
  }

  // SPCS (2) - NAD27 (datum 0)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(2, 3601, p, 0));
  }

  // SPCS (2) - NAD83 (datum 8)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(2, 3601, p, 8));
  }

  // SPCS (2) - invalid datum warning
  {
    WithQuietHandler quiet_handler;
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(2, 3601, p, 12));
  }

  // ALBERS (3)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(3, 0, p, 12));
  }

  // LAMCC (4)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(4, 0, p, 12));
  }

  // MERCAT (5)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(5, 0, p, 12));
  }

  // PS (6)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(6, 0, p, 12));
  }

  // POLYC (7)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(7, 0, p, 12));
  }

  // EQUIDC (8) - A (padfPrjParams[8] == 0) and B (!= 0)
  {
    OGRSpatialReference srs_a;
    p[8] = 0.0;
    EXPECT_EQ(OGRERR_NONE, srs_a.importFromUSGS(8, 0, p, 12));

    OGRSpatialReference srs_b;
    p[8] = 1.0;
    EXPECT_EQ(OGRERR_NONE, srs_b.importFromUSGS(8, 0, p, 12));
    p[8] = 0.0;
  }

  // TM (9)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(9, 0, p, 12));
  }

  // STEREO (10)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(10, 0, p, 12));
  }

  // LAMAZ (11)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(11, 0, p, 12));
  }

  // AZMEQD (12)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(12, 0, p, 12));
  }

  // GNOMON (13)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(13, 0, p, 12));
  }

  // ORTHO (14)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(14, 0, p, 12));
  }

  // SNSOID (16)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(16, 0, p, 12));
  }

  // EQRECT (17)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(17, 0, p, 12));
  }

  // MILLER (18)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(18, 0, p, 12));
  }

  // VGRINT (19)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(19, 0, p, 12));
  }

  // HOM (20) - format A (padfPrjParams[12] == 0) and format B
  // (padfPrjParams[12] != 0)
  {
    OGRSpatialReference srs_a;
    p[12] = 0.0;
    EXPECT_EQ(OGRERR_NONE, srs_a.importFromUSGS(20, 0, p, 12));

    OGRSpatialReference srs_b;
    p[12] = 1.0;
    EXPECT_EQ(OGRERR_NONE, srs_b.importFromUSGS(20, 0, p, 12));
    p[12] = 0.0;
  }

  // ROBIN (21)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(21, 0, p, 12));
  }

  // MOLL (25)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(25, 0, p, 12));
  }

  // WAGIV (28)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(28, 0, p, 12));
  }

  // WAGVII (29)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(29, 0, p, 12));
  }

  // Unsupported projection (999) -> LocalCS
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(999, 0, p, 12));
    EXPECT_TRUE(srs.IsLocal());
  }
}

TEST_F(OgrSrsUsgsTest, ImportDatumBranches) {
  double p[15] = {0};

  // iDatum < 0 with padfPrjParams[0] > 0 and padfPrjParams[1] > 1.0
  // (semi-minor)
  {
    OGRSpatialReference srs;
    p[0] = 6378137.0;
    p[1] = 6356752.0;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(0, 0, p, -1));
    EXPECT_NEAR(srs.GetSemiMajor(), 6378137.0, 1.0);
    p[0] = 0;
    p[1] = 0;
  }

  // iDatum < 0 with padfPrjParams[0] > 0 and padfPrjParams[1] in (0, 1]
  // (eccentricity sq)
  {
    OGRSpatialReference srs;
    p[0] = 6378137.0;
    p[1] = 0.00669438;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(0, 0, p, -1));
    EXPECT_NEAR(srs.GetSemiMajor(), 6378137.0, 1.0);
    p[0] = 0;
    p[1] = 0;
  }

  // iDatum < 0 with padfPrjParams[0] > 0 and padfPrjParams[1] == 0 (sphere)
  {
    OGRSpatialReference srs;
    p[0] = 6378137.0;
    p[1] = 0.0;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(0, 0, p, -1));
    EXPECT_NEAR(srs.GetSemiMajor(), 6378137.0, 1.0);
    p[0] = 0;
  }

  // iDatum < 0 with padfPrjParams[0] == 0 and padfPrjParams[1] > 0 (Clarke
  // 1866)
  {
    OGRSpatialReference srs;
    p[0] = 0.0;
    p[1] = 1.0;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(0, 0, p, -1));
    EXPECT_THAT(srs.GetAttrValue("SPHEROID"), HasSubstr("Clarke"));
    p[1] = 0;
  }

  // iDatum < 0 with padfPrjParams[0] == 0 and padfPrjParams[1] == 0 (Sphere
  // 6370997m)
  {
    OGRSpatialReference srs;
    p[0] = 0.0;
    p[1] = 0.0;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(0, 0, p, -1));
    EXPECT_NEAR(srs.GetSemiMajor(), 6370997.0, 1.0);
  }

  // iDatum == 0 (Clarke 1866)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(0, 0, p, 0));
    EXPECT_THAT(srs.GetAttrValue("SPHEROID"), HasSubstr("Clarke"));
  }

  // iDatum == 8 (GRS 1980)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(0, 0, p, 8));
    EXPECT_THAT(srs.GetAttrValue("SPHEROID"), HasSubstr("GRS"));
  }

  // iDatum == 12 (WGS84)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(0, 0, p, 12));
    EXPECT_THAT(srs.GetAttrValue("SPHEROID"), HasSubstr("WGS"));
  }

  // iDatum == 3 (skipped ellipsoid code in aoEllipsUSGS)
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(0, 0, p, 3));
  }

  // iDatum out of range (100)
  {
    WithQuietHandler quiet_handler;
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(0, 0, p, 100));
  }
}

TEST_F(OgrSrsUsgsTest, ExportAllProjectionsAndDatums) {
  long proj_sys = 0;
  long zone = 0;
  double* prj_params = nullptr;
  long datum = 0;

  auto ExportAndCheck = [&](const OGRSpatialReference& srs, long expected_proj,
                            long expected_datum = -999) {
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToUSGS(&proj_sys, &zone, &prj_params, &datum));
    EXPECT_EQ(proj_sys, expected_proj);
    if (expected_datum != -999) {
      EXPECT_EQ(datum, expected_datum);
    }
    CPLFree(prj_params);
    prj_params = nullptr;
  };

  // Local CS -> GEO
  {
    OGRSpatialReference srs;
    srs.SetLocalCS("local");
    ExportAndCheck(srs, 0);
  }

  // Geographic (no projection) with NAD27, NAD83, WGS84
  {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("NAD27");
    ExportAndCheck(srs, 0, 0);

    srs.SetWellKnownGeogCS("NAD83");
    ExportAndCheck(srs, 0, 8);

    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 0, 12);
  }

  // ALBERS
  {
    OGRSpatialReference srs;
    srs.SetACEA(20.0, 60.0, 40.0, -96.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 3, 12);
  }

  // LAMCC
  {
    OGRSpatialReference srs;
    srs.SetLCC(20.0, 60.0, 40.0, -96.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 4, 12);
  }

  // MERCAT
  {
    OGRSpatialReference srs;
    srs.SetMercator(0.0, 0.0, 1.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 5, 12);
  }

  // PS
  {
    OGRSpatialReference srs;
    srs.SetPS(90.0, 0.0, 1.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 6, 12);
  }

  // POLYC
  {
    OGRSpatialReference srs;
    srs.SetPolyconic(0.0, 0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 7, 12);
  }

  // EQUIDC
  {
    OGRSpatialReference srs;
    srs.SetEC(20.0, 60.0, 40.0, -96.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 8, 12);
  }

  // UTM North and South
  {
    OGRSpatialReference srs_n;
    srs_n.SetUTM(10, TRUE);
    srs_n.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs_n, 1, 12);
    EXPECT_EQ(zone, 10);

    OGRSpatialReference srs_s;
    srs_s.SetUTM(10, FALSE);
    srs_s.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs_s, 1, 12);
    EXPECT_EQ(zone, -10);
  }

  // TM (non-UTM)
  {
    OGRSpatialReference srs;
    srs.SetTM(0.0, 0.0, 0.9996, 500000.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 9, 12);
  }

  // STEREO
  {
    OGRSpatialReference srs;
    srs.SetStereographic(0.0, 0.0, 1.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 10, 12);
  }

  // LAMAZ
  {
    OGRSpatialReference srs;
    srs.SetLAEA(0.0, 0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 11, 12);
  }

  // AZMEQD
  {
    OGRSpatialReference srs;
    srs.SetAE(0.0, 0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 12, 12);
  }

  // GNOMON
  {
    OGRSpatialReference srs;
    srs.SetGnomonic(0.0, 0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 13, 12);
  }

  // ORTHO
  {
    OGRSpatialReference srs;
    srs.SetOrthographic(0.0, 0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 14, 12);
  }

  // SNSOID
  {
    OGRSpatialReference srs;
    srs.SetSinusoidal(0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 16, 12);
  }

  // EQRECT
  {
    OGRSpatialReference srs;
    srs.SetEquirectangular(0.0, 0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 17, 12);
  }

  // MILLER
  {
    OGRSpatialReference srs;
    srs.SetMC(0.0, 0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 18, 12);
  }

  // VGRINT
  {
    OGRSpatialReference srs;
    srs.SetVDG(0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 19, 12);
  }

  // HOM (Azimuth and 2 Point)
  {
    OGRSpatialReference srs_azi;
    srs_azi.SetHOM(40.0, -96.0, 45.0, 0.0, 1.0, 0.0, 0.0);
    srs_azi.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs_azi, 20, 12);

    OGRSpatialReference srs_2p;
    srs_2p.SetHOM2PNO(40.0, -96.0, 20.0, -90.0, 60.0, 1.0, 0.0, 0.0);
    srs_2p.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs_2p, 20, 12);
  }

  // ROBIN
  {
    OGRSpatialReference srs;
    srs.SetRobinson(0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 21, 12);
  }

  // MOLL
  {
    OGRSpatialReference srs;
    srs.SetMollweide(0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 25, 12);
  }

  // WAGIV
  {
    OGRSpatialReference srs;
    srs.SetWagner(4, 0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 28, 12);
  }

  // WAGVII
  {
    OGRSpatialReference srs;
    srs.SetWagner(7, 0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 29, 12);
  }

  // Unsupported projection (Eckert IV) falls back to GEO
  {
    WithQuietHandler quiet_handler;
    OGRSpatialReference srs;
    srs.SetEckertIV(0.0, 0.0, 0.0);
    srs.SetWellKnownGeogCS("WGS84");
    ExportAndCheck(srs, 0, 12);
  }

  // Datum export: sphere (inv flattening == 0)
  {
    OGRSpatialReference srs;
    srs.SetGeogCS("Spherical", "Spherical", "Spherical", 1234567.0, 0.0);
    ExportAndCheck(srs, 0, -1);
  }

  // Datum export: custom spheroid (non-zero inv flattening)
  {
    OGRSpatialReference srs;
    srs.SetGeogCS("Custom", "Custom", "Custom", 1234567.0, 100.0);
    ExportAndCheck(srs, 0, -1);
  }

  // Datum export: matching ellipsoid in aoEllipsUSGS (e.g. Bessel 1841: index
  // 2)
  {
    double p[15] = {0};
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromUSGS(0, 0, p, 2));
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToUSGS(&proj_sys, &zone, &prj_params, &datum));
    EXPECT_EQ(datum, 2);
    CPLFree(prj_params);
  }

  // Datum export: no DATUM attribute
  {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToUSGS(&proj_sys, &zone, &prj_params, &datum));
    EXPECT_EQ(datum, -1);
    CPLFree(prj_params);
  }
}

}  // namespace
}  // namespace autotest2
