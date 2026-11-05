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

#include <climits>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_p.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_csv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

TEST(OgrFromEpsgTest, GetEllipsoidInfo) {
  char* name = nullptr;
  double semi_major = 0.0;
  double inv_flattening = 0.0;

  // EPSG:7030 (WGS 84)
  EXPECT_EQ(OGRERR_NONE,
            OSRGetEllipsoidInfo(7030, &name, &semi_major, &inv_flattening));
  ASSERT_NE(nullptr, name);
  EXPECT_STREQ("WGS 84", name);
  EXPECT_NEAR(6378137.0, semi_major, 1e-4);
  EXPECT_NEAR(298.257223563, inv_flattening, 1e-6);
  CPLFree(name);

  // Without name pointer
  EXPECT_EQ(OGRERR_NONE,
            OSRGetEllipsoidInfo(7030, nullptr, &semi_major, &inv_flattening));

  // Invalid code
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS,
            OSRGetEllipsoidInfo(999999, nullptr, &semi_major, &inv_flattening));
}

TEST(OgrFromEpsgTest, SetStatePlaneValidation) {
  OGRSpatialReference srs;

  // C API null pointers
  EXPECT_EQ(OGRERR_FAILURE, OSRSetStatePlane(nullptr, 101, TRUE));
  EXPECT_EQ(OGRERR_FAILURE,
            OSRSetStatePlaneWithUnits(nullptr, 101, TRUE, "Foot", 0.3048));

  // Overflow check: !bNAD83 && nZone > INT_MAX - 10000
  EXPECT_EQ(OGRERR_FAILURE, srs.SetStatePlane(INT_MAX - 5000, FALSE));

  // Unknown zone without CSV entry - triggers fallback and warning
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE, srs.SetStatePlane(99999, TRUE));
    EXPECT_TRUE(srs.IsLocal());
    EXPECT_STREQ("State Plane Zone 99999 / NAD83",
                 srs.GetAttrValue("LOCAL_CS"));

    EXPECT_EQ(OGRERR_FAILURE, srs.SetStatePlane(99999, FALSE));
    EXPECT_TRUE(srs.IsLocal());
    EXPECT_STREQ("State Plane Zone 99999 / NAD27",
                 srs.GetAttrValue("LOCAL_CS"));
  }
}

TEST(OgrFromEpsgTest, SetStatePlaneWithCsv) {
  const std::string kStatePlaneCsv =
      "ID,EPSG_PCS_CODE\n"
      "101,26929\n"
      "10101,26729\n"
      "999,999999\n";

  VsiMemTempWrapper csv_file("/vsimem/ogr_fromepsg_test/stateplane.csv",
                             kStatePlaneCsv);
  CPLPushFinderLocation("/vsimem/ogr_fromepsg_test");
  auto cleanup_finder = absl::MakeCleanup([] {
    CSVDeaccess(nullptr);
    CPLPopFinderLocation();
  });

  OGRSpatialReference srs;

  // Standard NAD83
  EXPECT_EQ(OGRERR_NONE, srs.SetStatePlane(101, TRUE));
  EXPECT_TRUE(srs.IsProjected());
  EXPECT_STREQ("26929", srs.GetAuthorityCode("PROJCS"));

  // NAD83 with units override (should strip AUTHORITY node)
  EXPECT_EQ(OGRERR_NONE,
            srs.SetStatePlane(101, TRUE, "Foot_US", 0.3048006096012192));
  EXPECT_TRUE(srs.IsProjected());
  EXPECT_EQ(nullptr, srs.GetAuthorityCode("PROJCS"));
  EXPECT_NEAR(0.3048006096012192, srs.GetLinearUnits(), 1e-10);

  // Failure importing invalid EPSG code from table
  {
    WithQuietHandler quiet_handler;
    EXPECT_NE(OGRERR_NONE, srs.SetStatePlane(999, TRUE));
  }

  // C API wrappers
  EXPECT_EQ(OGRERR_NONE,
            OSRSetStatePlane(OGRSpatialReference::ToHandle(&srs), 101, TRUE));
  EXPECT_EQ(OGRERR_NONE,
            OSRSetStatePlaneWithUnits(OGRSpatialReference::ToHandle(&srs), 101,
                                      TRUE, "Foot_US", 0.3048006096012192));
}

TEST(OgrFromEpsgTest, AutoIdentifyEPSG) {
  // C API null check
  EXPECT_EQ(OGRERR_FAILURE, OSRAutoIdentifyEPSG(nullptr));

  // Geographic without GEOGCS authority
  {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    auto* node = srs.GetAttrNode("GEOGCS");
    ASSERT_NE(nullptr, node);
    int idx = node->FindChild("AUTHORITY");
    if (idx != -1) {
      node->DestroyChild(idx);
    }
    EXPECT_EQ(OGRERR_NONE, srs.AutoIdentifyEPSG());
    EXPECT_STREQ("4326", srs.GetAuthorityCode("GEOGCS"));
  }

  // UTM WGS84 North and South
  {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    srs.SetUTM(10, TRUE);
    auto* node = srs.GetAttrNode("PROJCS");
    ASSERT_NE(nullptr, node);
    int idx = node->FindChild("AUTHORITY");
    if (idx != -1) {
      node->DestroyChild(idx);
    }
    EXPECT_EQ(OGRERR_NONE, srs.AutoIdentifyEPSG());
    EXPECT_STREQ("32610", srs.GetAuthorityCode("PROJCS"));

    srs.Clear();
    srs.SetWellKnownGeogCS("WGS84");
    srs.SetUTM(10, FALSE);
    node = srs.GetAttrNode("PROJCS");
    ASSERT_NE(nullptr, node);
    idx = node->FindChild("AUTHORITY");
    if (idx != -1) {
      node->DestroyChild(idx);
    }
    EXPECT_EQ(OGRERR_NONE, srs.AutoIdentifyEPSG());
    EXPECT_STREQ("32710", srs.GetAuthorityCode("PROJCS"));
  }

  // UTM NAD27 North
  {
    OGRSpatialReference srs;
    srs.importFromEPSG(4267);
    srs.SetUTM(10, TRUE);
    EXPECT_EQ(OGRERR_NONE, srs.AutoIdentifyEPSG());
    EXPECT_STREQ("26710", srs.GetAuthorityCode("PROJCS"));
  }

  // UTM NAD83 North
  {
    OGRSpatialReference srs;
    srs.importFromEPSG(4269);
    srs.SetUTM(10, TRUE);
    EXPECT_EQ(OGRERR_NONE, srs.AutoIdentifyEPSG());
    EXPECT_STREQ("26910", srs.GetAuthorityCode("PROJCS"));
  }

  // UTM WGS72 North and South
  {
    OGRSpatialReference srs;
    srs.importFromEPSG(4322);
    srs.SetUTM(10, TRUE);
    EXPECT_EQ(OGRERR_NONE, srs.AutoIdentifyEPSG());
    EXPECT_STREQ("32210", srs.GetAuthorityCode("PROJCS"));

    srs.Clear();
    srs.importFromEPSG(4322);
    srs.SetUTM(10, FALSE);
    EXPECT_EQ(OGRERR_NONE, srs.AutoIdentifyEPSG());
    EXPECT_STREQ("32310", srs.GetAuthorityCode("PROJCS"));
  }

  // UTM with unrecognised datum
  {
    OGRSpatialReference srs;
    srs.SetGeogCS("Custom", "Custom", "Custom", 6378137, 298.257);
    srs.SetUTM(10, TRUE);
    EXPECT_EQ(OGRERR_UNSUPPORTED_SRS, srs.AutoIdentifyEPSG());
  }

  // Polar Stereographic on WGS84
  {
    // Arctic: lat_origin = 71 -> EPSG:3995
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    srs.SetPS(71.0, 0.0, 1.0, 0.0, 0.0);
    EXPECT_EQ(OGRERR_NONE, srs.AutoIdentifyEPSG());
    EXPECT_STREQ("3995", srs.GetAuthorityCode("PROJCS"));

    // Antarctic: lat_origin = -71 -> EPSG:3031
    srs.Clear();
    srs.importFromEPSG(4326);
    srs.SetPS(-71.0, 0.0, 1.0, 0.0, 0.0);
    EXPECT_EQ(OGRERR_NONE, srs.AutoIdentifyEPSG());
    EXPECT_STREQ("3031", srs.GetAuthorityCode("PROJCS"));
  }

  // Local / unidentifiable
  {
    OGRSpatialReference srs;
    srs.SetLocalCS("Local");
    EXPECT_EQ(OGRERR_UNSUPPORTED_SRS, srs.AutoIdentifyEPSG());
    EXPECT_EQ(OGRERR_UNSUPPORTED_SRS,
              OSRAutoIdentifyEPSG(OGRSpatialReference::ToHandle(&srs)));
  }
}

}  // namespace
}  // namespace autotest2
