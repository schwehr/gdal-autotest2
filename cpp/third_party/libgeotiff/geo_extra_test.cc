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
// Tests UTM and State Plane PCS/Proj code mapping helpers (geo_extra.c).
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geo_extra.c

#include "testing/base/public/gunit.h"
#include "third_party/libgeotiff/geo_normalize.h"
#include "third_party/libgeotiff/geovalues.h"

namespace libgeotiff {
namespace {

TEST(GeoExtraTest, MapSysToPcsCoversUtmStatePlaneOverridesAndFallbacks) {
  struct Case {
    int map_sys;
    int datum;
    int zone;
    int expected_pcs;
  };
  // State Plane formulas in geo_extra.c: SP27 uses 10000 + nZone and SP83 uses
  // 10000 + nZone + 30 when not in StatePlaneTable[], with special EPSG
  // overrides for Tennessee CS27 (zone 4100 -> 2204 / 15302) and Kentucky
  // North CS83 (zone 1601 -> 2205 / 15303).
  constexpr Case kCases[] = {
      {.map_sys = MapSys_UTM_North,
       .datum = GCS_NAD27,
       .zone = 10,
       .expected_pcs = PCS_NAD27_UTM_zone_10N},
      {.map_sys = MapSys_UTM_North,
       .datum = GCS_NAD83,
       .zone = 15,
       .expected_pcs = PCS_NAD83_UTM_zone_15N},
      {.map_sys = MapSys_UTM_North,
       .datum = GCS_WGS_72,
       .zone = 1,
       .expected_pcs = PCS_WGS72_UTM_zone_1N},
      {.map_sys = MapSys_UTM_North,
       .datum = GCS_WGS_72BE,
       .zone = 20,
       .expected_pcs = PCS_WGS72BE_UTM_zone_20N},
      {.map_sys = MapSys_UTM_North,
       .datum = GCS_WGS_84,
       .zone = 33,
       .expected_pcs = PCS_WGS84_UTM_zone_33N},
      {.map_sys = MapSys_UTM_North,
       .datum = KvUserDefined,
       .zone = 10,
       .expected_pcs = KvUserDefined},

      {.map_sys = MapSys_UTM_South,
       .datum = GCS_WGS_72,
       .zone = 5,
       .expected_pcs = PCS_WGS72_UTM_zone_5S},
      {.map_sys = MapSys_UTM_South,
       .datum = GCS_WGS_72BE,
       .zone = 12,
       .expected_pcs = PCS_WGS72BE_UTM_zone_12S},
      {.map_sys = MapSys_UTM_South,
       .datum = GCS_WGS_84,
       .zone = 55,
       .expected_pcs = PCS_WGS84_UTM_zone_55S},
      {.map_sys = MapSys_UTM_South,
       .datum = GCS_NAD83,
       .zone = 10,
       .expected_pcs = KvUserDefined},

      {.map_sys = MapSys_State_Plane_27,
       .datum = GCS_NAD27,
       .zone = 101,
       .expected_pcs = PCS_NAD27_Alabama_East},
      {.map_sys = MapSys_State_Plane_27,
       .datum = GCS_NAD27,
       .zone = 4100,
       .expected_pcs = 2204},  // Tennessee CS27 override
      {.map_sys = MapSys_State_Plane_27,
       .datum = GCS_NAD27,
       .zone = 9999,
       .expected_pcs = 19999},  // 10000 + 9999 unmapped fallback

      {.map_sys = MapSys_State_Plane_83,
       .datum = GCS_NAD83,
       .zone = 101,
       .expected_pcs = PCS_NAD83_Alabama_East},
      {.map_sys = MapSys_State_Plane_83,
       .datum = GCS_NAD83,
       .zone = 1601,
       .expected_pcs = 2205},  // Kentucky North CS83 override
      {.map_sys = MapSys_State_Plane_83,
       .datum = GCS_NAD83,
       .zone = 9999,
       .expected_pcs = 20029},  // 10000 + 9999 + 30 unmapped fallback

      {.map_sys = KvUserDefined,
       .datum = GCS_WGS_84,
       .zone = 1,
       .expected_pcs = KvUserDefined},
  };
  for (const auto& tc : kCases) {
    EXPECT_EQ(tc.expected_pcs, GTIFMapSysToPCS(tc.map_sys, tc.datum, tc.zone))
        << "map_sys=" << tc.map_sys << " datum=" << tc.datum
        << " zone=" << tc.zone;
  }
}

TEST(GeoExtraTest, MapSysToProjCoversUtmStatePlaneOverridesAndFallbacks) {
  EXPECT_EQ(Proj_UTM_zone_10N, GTIFMapSysToProj(MapSys_UTM_North, 10));
  EXPECT_EQ(Proj_UTM_zone_22S, GTIFMapSysToProj(MapSys_UTM_South, 22));
  EXPECT_EQ(Proj_Alabama_CS27_East,
            GTIFMapSysToProj(MapSys_State_Plane_27, 101));
  // Tennessee CS27 (zone 4100 -> 15302) and Kentucky North CS83 (1601 ->
  // 15303).
  EXPECT_EQ(15302, GTIFMapSysToProj(MapSys_State_Plane_27, 4100));
  EXPECT_EQ(Proj_Alabama_CS83_East,
            GTIFMapSysToProj(MapSys_State_Plane_83, 101));
  EXPECT_EQ(15303, GTIFMapSysToProj(MapSys_State_Plane_83, 1601));
  EXPECT_EQ(KvUserDefined, GTIFMapSysToProj(KvUserDefined, 1));
}

TEST(GeoExtraTest, PcsToMapSysCoversAllUtmDatumsStatePlaneAndNullOutputs) {
  struct Case {
    int pcs;
    int expected_map_sys;
    int expected_datum;
    int expected_zone;
  };
  constexpr Case kCases[] = {
      {.pcs = PCS_NAD27_UTM_zone_10N,
       .expected_map_sys = MapSys_UTM_North,
       .expected_datum = GCS_NAD27,
       .expected_zone = 10},
      {.pcs = PCS_NAD83_UTM_zone_23N,
       .expected_map_sys = MapSys_UTM_North,
       .expected_datum = GCS_NAD83,
       .expected_zone = 23},
      {.pcs = PCS_WGS72_UTM_zone_1N,
       .expected_map_sys = MapSys_UTM_North,
       .expected_datum = GCS_WGS_72,
       .expected_zone = 1},
      {.pcs = PCS_WGS72_UTM_zone_60S,
       .expected_map_sys = MapSys_UTM_South,
       .expected_datum = GCS_WGS_72,
       .expected_zone = 60},
      {.pcs = PCS_WGS72BE_UTM_zone_3N,
       .expected_map_sys = MapSys_UTM_North,
       .expected_datum = GCS_WGS_72BE,
       .expected_zone = 3},
      {.pcs = PCS_WGS72BE_UTM_zone_19S,
       .expected_map_sys = MapSys_UTM_South,
       .expected_datum = GCS_WGS_72BE,
       .expected_zone = 19},
      {.pcs = PCS_WGS84_UTM_zone_32N,
       .expected_map_sys = MapSys_UTM_North,
       .expected_datum = GCS_WGS_84,
       .expected_zone = 32},
      {.pcs = PCS_WGS84_UTM_zone_18S,
       .expected_map_sys = MapSys_UTM_South,
       .expected_datum = GCS_WGS_84,
       .expected_zone = 18},
      {.pcs = PCS_SAD69_UTM_zone_20N,
       .expected_map_sys = MapSys_UTM_North,
       .expected_datum = KvUserDefined,
       .expected_zone = 20},
      {.pcs = PCS_SAD69_UTM_zone_21S,
       .expected_map_sys = MapSys_UTM_South,
       .expected_datum = KvUserDefined,
       .expected_zone = 21},
      {.pcs = PCS_NAD83_Alabama_East,
       .expected_map_sys = MapSys_State_Plane_83,
       .expected_datum = GCS_NAD83,
       .expected_zone = 101},
      {.pcs = PCS_NAD27_Alabama_East,
       .expected_map_sys = MapSys_State_Plane_27,
       .expected_datum = GCS_NAD27,
       .expected_zone = 101},
      {.pcs = Proj_California_CS83_1,
       .expected_map_sys = MapSys_State_Plane_83,
       .expected_datum = GCS_NAD83,
       .expected_zone = 401},
      {.pcs = Proj_California_CS27_I,
       .expected_map_sys = MapSys_State_Plane_27,
       .expected_datum = GCS_NAD27,
       .expected_zone = 401},
      {.pcs = 3857,  // Web Mercator: neither UTM nor State Plane.
       .expected_map_sys = KvUserDefined,
       .expected_datum = KvUserDefined,
       .expected_zone = KvUserDefined},
  };
  for (const auto& tc : kCases) {
    int datum = -1;
    int zone = -1;
    EXPECT_EQ(tc.expected_map_sys, GTIFPCSToMapSys(tc.pcs, &datum, &zone))
        << "pcs=" << tc.pcs;
    EXPECT_EQ(tc.expected_datum, datum) << "pcs=" << tc.pcs;
    EXPECT_EQ(tc.expected_zone, zone) << "pcs=" << tc.pcs;
  }

  // Null out-pointers are tolerated cleanly.
  EXPECT_EQ(MapSys_UTM_North,
            GTIFPCSToMapSys(PCS_WGS84_UTM_zone_10N, nullptr, nullptr));
}

TEST(GeoExtraTest, ProjToMapSysCoversUtmStatePlaneUnknownAndNullZone) {
  int zone = -1;
  EXPECT_EQ(MapSys_UTM_North, GTIFProjToMapSys(Proj_UTM_zone_15N, &zone));
  EXPECT_EQ(15, zone);

  EXPECT_EQ(MapSys_UTM_South, GTIFProjToMapSys(Proj_UTM_zone_42S, &zone));
  EXPECT_EQ(42, zone);

  EXPECT_EQ(MapSys_State_Plane_83,
            GTIFProjToMapSys(Proj_Alabama_CS83_East, &zone));
  EXPECT_EQ(101, zone);

  EXPECT_EQ(MapSys_State_Plane_27,
            GTIFProjToMapSys(Proj_Alabama_CS27_East, &zone));
  EXPECT_EQ(101, zone);

  EXPECT_EQ(KvUserDefined, GTIFProjToMapSys(KvUserDefined, &zone));
  EXPECT_EQ(KvUserDefined, zone);

  EXPECT_EQ(MapSys_UTM_North, GTIFProjToMapSys(Proj_UTM_zone_1N, nullptr));
}

}  // namespace
}  // namespace libgeotiff
