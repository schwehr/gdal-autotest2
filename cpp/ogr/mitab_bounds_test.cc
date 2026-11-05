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
// Unit tests for MapInfo TAB coordsys bounds lookup (mitab_bounds.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/mitab_bounds.cpp

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrmitabspatialref.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

TEST(MitabBoundsTest, InternalTableLookup) {
  // Free any previous table state
  MITABFreeCoordSysTable();
  CPLSetConfigOption("MITAB_BOUNDS_FILE", nullptr);

  // Test Lat/Lon projection lookup (entry 0 in gasBoundsList: nProjId = 1)
  TABProjInfo sProjLatLon;
  std::memset(&sProjLatLon, 0, sizeof(sProjLatLon));
  sProjLatLon.nProjId = 1;
  sProjLatLon.nEllipsoidId = 0;
  sProjLatLon.nUnitsId = 0;

  double dXMin = 0.0, dYMin = 0.0, dXMax = 0.0, dYMax = 0.0;
  EXPECT_TRUE(MITABLookupCoordSysBounds(&sProjLatLon, dXMin, dYMin, dXMax,
                                        dYMax,
                                        /*bOnlyUserTable=*/false));
  EXPECT_DOUBLE_EQ(-1000.0, dXMin);
  EXPECT_DOUBLE_EQ(-1000.0, dYMin);
  EXPECT_DOUBLE_EQ(1000.0, dXMax);
  EXPECT_DOUBLE_EQ(1000.0, dYMax);

  // bOnlyUserTable = true should fail when no external table is loaded
  EXPECT_FALSE(MITABLookupCoordSysBounds(&sProjLatLon, dXMin, dYMin, dXMax,
                                         dYMax,
                                         /*bOnlyUserTable=*/true));

  // Unknown projection ID should fail
  TABProjInfo sProjUnknown;
  std::memset(&sProjUnknown, 0, sizeof(sProjUnknown));
  sProjUnknown.nProjId = 250;
  EXPECT_FALSE(MITABLookupCoordSysBounds(&sProjUnknown, dXMin, dYMin, dXMax,
                                         dYMax,
                                         /*bOnlyUserTable=*/false));

  // Lookup entry with specific projection parameters
  // Entry 1 in gasBoundsList: nProjId = 2, nEllipsoidId = 29, nUnitsId = 0
  // adProjParams: {-85.5, 13, 0, 0, 0, 0}
  TABProjInfo sProjEntry1;
  std::memset(&sProjEntry1, 0, sizeof(sProjEntry1));
  sProjEntry1.nProjId = 2;
  sProjEntry1.nEllipsoidId = 29;
  sProjEntry1.nUnitsId = 0;
  sProjEntry1.adProjParams[0] = -85.5;
  sProjEntry1.adProjParams[1] = 13.0;

  EXPECT_TRUE(MITABLookupCoordSysBounds(&sProjEntry1, dXMin, dYMin, dXMax,
                                        dYMax,
                                        /*bOnlyUserTable=*/false));
  EXPECT_NEAR(-18500.719, dXMin, 0.001);
  EXPECT_NEAR(-4067.438, dYMin, 0.001);
  EXPECT_NEAR(30025.757, dXMax, 0.001);
  EXPECT_NEAR(4067.438, dYMax, 0.001);

  // Test tolerance matching (eps = 1e-6 in second loop iteration)
  // Slightly adjust adProjParams[0] by 1e-8 (fails eps=1e-12, passes eps=1e-6)
  TABProjInfo sProjTol;
  std::memset(&sProjTol, 0, sizeof(sProjTol));
  sProjTol.nProjId = 2;
  sProjTol.nEllipsoidId = 29;
  sProjTol.nUnitsId = 0;
  sProjTol.adProjParams[0] = -85.5 + 1e-8;
  sProjTol.adProjParams[1] = 13.0;
  EXPECT_TRUE(MITABLookupCoordSysBounds(&sProjTol, dXMin, dYMin, dXMax, dYMax,
                                        /*bOnlyUserTable=*/false));

  // Test datum matching with different datum shifts
  TABProjInfo sProjDatumDiff;
  std::memset(&sProjDatumDiff, 0, sizeof(sProjDatumDiff));
  sProjDatumDiff.nProjId = 2;
  sProjDatumDiff.nEllipsoidId = 29;
  sProjDatumDiff.nUnitsId = 0;
  sProjDatumDiff.dDatumShiftX = 999.0;
  sProjDatumDiff.adProjParams[0] = -85.5;
  sProjDatumDiff.adProjParams[1] = 13.0;
  EXPECT_FALSE(MITABLookupCoordSysBounds(&sProjDatumDiff, dXMin, dYMin, dXMax,
                                         dYMax, /*bOnlyUserTable=*/false));

  // Test ellipsoid mismatch
  sProjDatumDiff.dDatumShiftX = 0.0;
  sProjDatumDiff.nEllipsoidId = 99;
  EXPECT_FALSE(MITABLookupCoordSysBounds(&sProjDatumDiff, dXMin, dYMin, dXMax,
                                         dYMax, /*bOnlyUserTable=*/false));
}

TEST(MitabBoundsTest, LoadCoordSysTableErrorsAndEdgeCases) {
  MITABFreeCoordSysTable();

  // Non-existent file
  {
    WithQuietHandler quiet;
    EXPECT_EQ(0,
              MITABLoadCoordSysTable("/vsimem/nonexistent_mitab_bounds.txt"));
  }

  // File with comments, short lines, and missing bounds
  constexpr char kPath[] = "/vsimem/test_bounds_warn.txt";
  {
    constexpr std::string_view kTestContent =
        "# Comment line\n"
        "\n"
        "short\n"
        "# Another comment\n"
        "CoordSys Earth Projection 8, 24, \"m\", -63, 0, 0.9996, 500000, "
        "0\n";  // Missing bounds
    VsiMemTempWrapper file(kPath, std::string(kTestContent));

    WithQuietHandler quiet;
    EXPECT_EQ(0, MITABLoadCoordSysTable(kPath));
  }
  MITABFreeCoordSysTable();

  // Invalid CoordSys line
  {
    constexpr std::string_view kInvalidContent =
        "CoordSys NonExistentProjectionTypeThatFailsToParse 12345\n";
    VsiMemTempWrapper file(kPath, std::string(kInvalidContent));

    WithQuietHandler quiet;
    EXPECT_NE(0, MITABLoadCoordSysTable(kPath));
  }
  MITABFreeCoordSysTable();
}

TEST(MitabBoundsTest, SourceDestinationRemapping) {
  MITABFreeCoordSysTable();

  constexpr char kPath[] = "/vsimem/remap_bounds.txt";

  // Invalid Source format: missing '='
  {
    constexpr std::string_view kBadSource =
        "Source CoordSys Earth Projection 8, 24, \"m\", -63, 0, 0.9996, "
        "500000, "
        "0\n";
    VsiMemTempWrapper file(kPath, std::string(kBadSource));
    WithQuietHandler quiet;
    MITABLoadCoordSysTable(kPath);
  }
  MITABFreeCoordSysTable();

  // Source with unexpected Bounds and Destination missing '='
  {
    constexpr std::string_view kBadDest =
        "Source = CoordSys Earth Projection 8, 24, \"m\", -63, 0, 0.9996, "
        "500000, 0 "
        "Bounds (-100, -100) (100, 100)\n"
        "Destination CoordSys Earth Projection 8, 24, \"m\", -63, 0, 0.9996, "
        "500000, 0 "
        "Bounds (-200, -200) (200, 200)\n";
    VsiMemTempWrapper file(kPath, std::string(kBadDest));
    WithQuietHandler quiet;
    MITABLoadCoordSysTable(kPath);
  }
  MITABFreeCoordSysTable();

  // Source with bad CoordSys syntax
  {
    constexpr std::string_view kBadSourceCS =
        "Source = CoordSys Invalid 9999\n";
    VsiMemTempWrapper file(kPath, std::string(kBadSourceCS));
    WithQuietHandler quiet;
    MITABLoadCoordSysTable(kPath);
  }
  MITABFreeCoordSysTable();

  // Valid Source and Destination remapping
  {
    constexpr std::string_view kValidRemap =
        "Source = CoordSys Earth Projection 8, 24, \"m\", -63, 0, 0.9996, "
        "500000, 0\n"
        "Destination = CoordSys Earth Projection 8, 28, \"m\", -63, 0, 0.9996, "
        "500000, 0 "
        "Bounds (-5000000, -5000000) (5000000, 5000000)\n";
    VsiMemTempWrapper file(kPath, std::string(kValidRemap));

    EXPECT_EQ(0, MITABLoadCoordSysTable(kPath));

    // Perform lookup matching the Source projection
    TABProjInfo sQueryProj;
    std::memset(&sQueryProj, 0, sizeof(sQueryProj));
    EXPECT_EQ(
        0,
        MITABCoordSys2TABProjInfo(
            "CoordSys Earth Projection 8, 24, \"m\", -63, 0, 0.9996, 500000, 0",
            &sQueryProj));

    double dXMin = 0.0, dYMin = 0.0, dXMax = 0.0, dYMax = 0.0;
    EXPECT_TRUE(MITABLookupCoordSysBounds(&sQueryProj, dXMin, dYMin, dXMax,
                                          dYMax,
                                          /*bOnlyUserTable=*/true));
    EXPECT_DOUBLE_EQ(-5000000.0, dXMin);
    EXPECT_DOUBLE_EQ(-5000000.0, dYMin);
    EXPECT_DOUBLE_EQ(5000000.0, dXMax);
    EXPECT_DOUBLE_EQ(5000000.0, dYMax);

    // sQueryProj should have been updated to Destination's projection (datum
    // 28, ellipsoid 4)
    EXPECT_EQ(4, sQueryProj.nEllipsoidId);
    EXPECT_EQ(28, sQueryProj.nDatumId);
  }
  MITABFreeCoordSysTable();
}

TEST(MitabBoundsTest, TableReallocAndConfigOptionAutoReload) {
  MITABFreeCoordSysTable();

  // Generate a bounds file with 110 entries to trigger CPLRealloc (numEntries
  // += 100)
  constexpr char kPath[] = "/vsimem/large_bounds_table.txt";
  std::string content;
  for (int i = 0; i < 110; ++i) {
    char szBuf[256];
    snprintf(szBuf, sizeof(szBuf),
             "CoordSys Earth Projection 8, 24, \"m\", %d, 0, 0.9996, 500000, 0 "
             "Bounds (%d, %d) (%d, %d)\n",
             -180 + i, -1000000 * (i + 1), -1000000 * (i + 1),
             1000000 * (i + 1), 1000000 * (i + 1));
    content += szBuf;
  }

  VsiMemTempWrapper file(kPath, content);

  EXPECT_EQ(0, MITABLoadCoordSysTable(kPath));

  // Query entry 105 to verify reallocated entries are properly searched
  TABProjInfo sQueryProj;
  std::memset(&sQueryProj, 0, sizeof(sQueryProj));
  char szCoordSys[256];
  snprintf(szCoordSys, sizeof(szCoordSys),
           "CoordSys Earth Projection 8, 24, \"m\", %d, 0, 0.9996, 500000, 0",
           -180 + 105);
  EXPECT_EQ(0, MITABCoordSys2TABProjInfo(szCoordSys, &sQueryProj));

  double dXMin = 0.0, dYMin = 0.0, dXMax = 0.0, dYMax = 0.0;
  EXPECT_TRUE(MITABLookupCoordSysBounds(&sQueryProj, dXMin, dYMin, dXMax, dYMax,
                                        /*bOnlyUserTable=*/true));
  EXPECT_DOUBLE_EQ(-1000000.0 * 106, dXMin);
  EXPECT_DOUBLE_EQ(1000000.0 * 106, dXMax);

  // Now test MITAB_BOUNDS_FILE config option automatic loading
  CPLSetConfigOption("MITAB_BOUNDS_FILE", kPath);
  EXPECT_TRUE(MITABLookupCoordSysBounds(&sQueryProj, dXMin, dYMin, dXMax, dYMax,
                                        /*bOnlyUserTable=*/true));

  // Call again with same path to exercise the (strcmp == 0) branch
  EXPECT_TRUE(MITABLookupCoordSysBounds(&sQueryProj, dXMin, dYMin, dXMax, dYMax,
                                        /*bOnlyUserTable=*/true));

  // Now change MITAB_BOUNDS_FILE to a second file to test path switching
  constexpr char kPath2[] = "/vsimem/bounds_path2.txt";
  constexpr std::string_view kNewContent =
      "CoordSys Earth Projection 8, 24, \"m\", -75, 0, 0.9996, 500000, 0 "
      "Bounds (-999999, -999999) (999999, 999999)\n";
  VsiMemTempWrapper file2(kPath2, std::string(kNewContent));

  CPLSetConfigOption("MITAB_BOUNDS_FILE", kPath2);

  TABProjInfo sNewQuery;
  std::memset(&sNewQuery, 0, sizeof(sNewQuery));
  EXPECT_EQ(
      0,
      MITABCoordSys2TABProjInfo(
          "CoordSys Earth Projection 8, 24, \"m\", -75, 0, 0.9996, 500000, 0",
          &sNewQuery));
  EXPECT_TRUE(MITABLookupCoordSysBounds(&sNewQuery, dXMin, dYMin, dXMax, dYMax,
                                        /*bOnlyUserTable=*/true));
  EXPECT_DOUBLE_EQ(-999999.0, dXMin);

  // Clear config option and verify cleanup
  CPLSetConfigOption("MITAB_BOUNDS_FILE", nullptr);
  EXPECT_FALSE(MITABLookupCoordSysBounds(&sNewQuery, dXMin, dYMin, dXMax, dYMax,
                                         /*bOnlyUserTable=*/true));

  // Test non-existent file for MITAB_BOUNDS_FILE (exercises VSIStatL failure
  // and st_mtime=0)
  CPLSetConfigOption("MITAB_BOUNDS_FILE", "/vsimem/does_not_exist_stat.txt");
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(MITABLookupCoordSysBounds(&sNewQuery, dXMin, dYMin, dXMax,
                                           dYMax,
                                           /*bOnlyUserTable=*/true));
  }
  CPLSetConfigOption("MITAB_BOUNDS_FILE", nullptr);
  // Clear szPreviousMitabBoundsFile
  EXPECT_FALSE(MITABLookupCoordSysBounds(&sNewQuery, dXMin, dYMin, dXMax, dYMax,
                                         /*bOnlyUserTable=*/true));

  // Test custom 7-parameter datum in user table (exercises lines 20320-20332)
  constexpr char kCustomDatumPath[] = "/vsimem/custom_datum_bounds.txt";
  constexpr std::string_view kCustomDatumContent =
      "CoordSys Earth Projection 8, 9999, 28, 100, 200, 300, 1, 2, 3, 4, 5, "
      "\"m\", -63, 0, 0.9996, 500000, 0 Bounds (-50000, -50000) (50000, "
      "50000)\n";
  {
    VsiMemTempWrapper custom_file(kCustomDatumPath,
                                  std::string(kCustomDatumContent));

    EXPECT_EQ(0, MITABLoadCoordSysTable(kCustomDatumPath));

    TABProjInfo sCustomProj;
    std::memset(&sCustomProj, 0, sizeof(sCustomProj));
    EXPECT_EQ(0,
              MITABCoordSys2TABProjInfo(
                  "CoordSys Earth Projection 8, 9999, 28, 100, 200, 300, 1, 2, "
                  "3, 4, 5, \"m\", -63, 0, 0.9996, 500000, 0",
                  &sCustomProj));
    EXPECT_TRUE(MITABLookupCoordSysBounds(&sCustomProj, dXMin, dYMin, dXMax,
                                          dYMax,
                                          /*bOnlyUserTable=*/true));
    EXPECT_DOUBLE_EQ(-50000.0, dXMin);
    EXPECT_DOUBLE_EQ(50000.0, dXMax);
  }

  MITABFreeCoordSysTable();
}

}  // namespace
}  // namespace autotest2
