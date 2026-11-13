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
// Constexpr lookup tables for
// //third_party/libgeotiff/google:geo_normalize_test.
//
// Note: Expected EPSG names, UOM values, and conversion method mappings below
// derive from the embedded PROJ `proj.db` in //third_party/proj4 and may need
// updating when //third_party/proj4 is upgraded.

#ifndef THIRD_PARTY_LIBGEOTIFF_GOOGLE_GEO_NORMALIZE_TEST_CONSTANTS_H_
#define THIRD_PARTY_LIBGEOTIFF_GOOGLE_GEO_NORMALIZE_TEST_CONSTANTS_H_

#include <array>
#include <cstdint>
#include <numbers>
#include <string_view>

#include "third_party/libgeotiff/geokeys.h"
#include "third_party/libgeotiff/geovalues.h"

namespace libgeotiff {

struct UomAngleCase {
  int code;
  std::string_view name;
  double in_degrees;
};

inline constexpr UomAngleCase kBuiltinUomAngleCases[] = {
    {.code = 9101, .name = "radian", .in_degrees = 180.0 / std::numbers::pi},
    {.code = 9102, .name = "degree", .in_degrees = 1.0},
    {.code = 9103, .name = "arc-minute", .in_degrees = 1.0 / 60.0},
    {.code = 9104, .name = "arc-second", .in_degrees = 1.0 / 3600.0},
    {.code = 9105, .name = "grad", .in_degrees = 0.9},
    {.code = 9106, .name = "gon", .in_degrees = 0.9},
    {.code = 9107, .name = "degree", .in_degrees = 1.0},
    {.code = 9108, .name = "degree", .in_degrees = 1.0},
    {.code = 9109,
     .name = "microradian",
     .in_degrees = 180.0 / (std::numbers::pi * 1000000.0)},
    {.code = 9110, .name = "degree", .in_degrees = 1.0},
    {.code = 9122, .name = "degree", .in_degrees = 1.0},
};

struct AngleStringCase {
  const char* input;
  int uom_angle;
  double expected_dd;
};

inline constexpr AngleStringCase kAngleStringCases[] = {
    {.input = "123.3045", .uom_angle = 9110, .expected_dd = 123.5125},
    {.input = "-123.3045", .uom_angle = 9110, .expected_dd = -123.5125},
    {.input = "12.30455",
     .uom_angle = 9110,
     .expected_dd = 12.5 + 45.5 / 3600.0},
    // Exercises single-digit minute/second padding when trailing digits stop.
    {.input = "12.3", .uom_angle = 9110, .expected_dd = 12.5},
    {.input = "12.304", .uom_angle = 9110, .expected_dd = 12.5 + 40.0 / 3600.0},
    {.input = "10", .uom_angle = 9110, .expected_dd = 10.0},
    {.input = "200", .uom_angle = 9105, .expected_dd = 180.0},
    {.input = "100", .uom_angle = 9106, .expected_dd = 90.0},
    {.input = "3.14159265358979323846",
     .uom_angle = 9101,
     .expected_dd = 180.0},
    {.input = "30", .uom_angle = 9103, .expected_dd = 0.5},
    {.input = "1800", .uom_angle = 9104, .expected_dd = 0.5},
    {.input = "45.25", .uom_angle = 9102, .expected_dd = 45.25},
    {.input = "45.25", .uom_angle = KvUserDefined, .expected_dd = 45.25},
    {.input = "45.25", .uom_angle = 0, .expected_dd = 45.25},
};

struct UtmPcsCase {
  int pcs;
  std::string_view expected_name;
  int16_t expected_proj_op;
  int16_t expected_geog_cs;
};

inline constexpr UtmPcsCase kUtmPcsCases[] = {
    {.pcs = 26710,
     .expected_name = "NAD27 / UTM zone 10N",
     .expected_proj_op = Proj_UTM_zone_10N,
     .expected_geog_cs = GCS_NAD27},
    {.pcs = 26910,
     .expected_name = "NAD83 / UTM zone 10N",
     .expected_proj_op = Proj_UTM_zone_10N,
     .expected_geog_cs = GCS_NAD83},
    {.pcs = 32210,
     .expected_name = "WGS 72 / UTM zone 10N",
     .expected_proj_op = Proj_UTM_zone_10N,
     .expected_geog_cs = GCS_WGS_72},
    {.pcs = 32410,
     .expected_name = "WGS 72BE / UTM zone 10N",
     .expected_proj_op = Proj_UTM_zone_10N,
     .expected_geog_cs = GCS_WGS_72BE},
    {.pcs = 32610,
     .expected_name = "WGS 84 / UTM zone 10N",
     .expected_proj_op = Proj_UTM_zone_10N,
     .expected_geog_cs = GCS_WGS_84},
    {.pcs = 32710,
     .expected_name = "WGS 84 / UTM zone 10S",
     .expected_proj_op = Proj_UTM_zone_10S,
     .expected_geog_cs = GCS_WGS_84},
};

// EPSG coordinate-operation conversion codes (< 32767) verified present in the
// embedded PROJ proj.db covering EPSGProjMethodToCTProjMethod and SetGTParamIds
// (plus unmapped method 5019).
inline constexpr std::array<int, 28> kEpsgConversionCodes = {
    5390, 3811, 19902, 3831,  5640, 19847, 3856,  4114,  3813, 17515,
    5587, 5478, 5889,  19917, 6753, 8440,  8061,  19937, 5471, 6929,
    3897, 5068, 19869, 4085,  4086, 19968, 19846, 5019,
};

struct CtParmOverride {
  geokey_t key;
  double value;
};

struct CtProjectionParmsCase {
  int16_t ct_projection;
  std::array<CtParmOverride, 7> inputs;
  int input_count;
  int16_t expected_parm_id0;
  double expected_parm0;
  int16_t expected_parm_id1;
  double expected_parm1;
};

inline constexpr CtProjectionParmsCase kCtProjectionParmsCases[] = {
    // 1. CT_Stereographic
    {.ct_projection = CT_Stereographic,
     .inputs = {{{ProjNatOriginLatGeoKey, 45.0},
                 {ProjNatOriginLongGeoKey, -70.0},
                 {ProjScaleAtNatOriginGeoKey, 0.9999},
                 {ProjFalseEastingGeoKey, 100.0},
                 {ProjFalseNorthingGeoKey, 200.0}}},
     .input_count = 5,
     .expected_parm_id0 = ProjCenterLatGeoKey,
     .expected_parm0 = 45.0,
     .expected_parm_id1 = ProjCenterLongGeoKey,
     .expected_parm1 = -70.0},
    {.ct_projection = CT_Stereographic,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = ProjCenterLatGeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjCenterLongGeoKey,
     .expected_parm1 = 0.0},
    // 2. CT_Mercator (with StdParallel1 vs omitted defaulting Scale=1.0)
    {.ct_projection = CT_Mercator,
     .inputs = {{{ProjNatOriginLatGeoKey, 0.0},
                 {ProjNatOriginLongGeoKey, -90.0},
                 {ProjStdParallel1GeoKey, 30.0},
                 {ProjScaleAtNatOriginGeoKey, 0.998}}},
     .input_count = 4,
     .expected_parm_id0 = ProjNatOriginLatGeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjNatOriginLongGeoKey,
     .expected_parm1 = -90.0},
    {.ct_projection = CT_Mercator,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = ProjNatOriginLatGeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjNatOriginLongGeoKey,
     .expected_parm1 = 0.0},
    // 3. CT_TransverseMercator / 1SP LCC / ObliqueStereographic / TM SO
    {.ct_projection = CT_LambertConfConic_1SP,
     .inputs = {{{ProjFalseOriginLatGeoKey, 45.0},
                 {ProjFalseOriginLongGeoKey, -90.0},
                 {ProjScaleAtCenterGeoKey, 0.9995},
                 {ProjCenterEastingGeoKey, 500.0},
                 {ProjCenterNorthingGeoKey, 600.0}}},
     .input_count = 5,
     .expected_parm_id0 = ProjNatOriginLatGeoKey,
     .expected_parm0 = 45.0,
     .expected_parm_id1 = ProjNatOriginLongGeoKey,
     .expected_parm1 = -90.0},
    {.ct_projection = CT_TransverseMercator,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = ProjNatOriginLatGeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjNatOriginLongGeoKey,
     .expected_parm1 = 0.0},
    // 4. CT_ObliqueMercator / CT_HotineObliqueMercatorAzimuthCenter
    {.ct_projection = CT_HotineObliqueMercatorAzimuthCenter,
     .inputs = {{{ProjCenterLatGeoKey, 46.9},
                 {ProjCenterLongGeoKey, 7.4},
                 {ProjAzimuthAngleGeoKey, 90.0},
                 {ProjRectifiedGridAngleGeoKey, 90.0},
                 {ProjScaleAtCenterGeoKey, 1.0}}},
     .input_count = 5,
     .expected_parm_id0 = ProjCenterLatGeoKey,
     .expected_parm0 = 46.9,
     .expected_parm_id1 = ProjCenterLongGeoKey,
     .expected_parm1 = 7.4},
    {.ct_projection = CT_ObliqueMercator,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = ProjCenterLatGeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjCenterLongGeoKey,
     .expected_parm1 = 0.0},
    // 5. CT_ObliqueMercator_Laborde
    {.ct_projection = CT_ObliqueMercator_Laborde,
     .inputs = {{{ProjCenterLatGeoKey, -18.9},
                 {ProjCenterLongGeoKey, 46.4},
                 {ProjAzimuthAngleGeoKey, 18.9},
                 {ProjScaleAtCenterGeoKey, 0.9995}}},
     .input_count = 4,
     .expected_parm_id0 = ProjCenterLatGeoKey,
     .expected_parm0 = -18.9,
     .expected_parm_id1 = ProjCenterLongGeoKey,
     .expected_parm1 = 46.4},
    {.ct_projection = CT_ObliqueMercator_Laborde,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = ProjCenterLatGeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjCenterLongGeoKey,
     .expected_parm1 = 0.0},
    // 6. CT_CassiniSoldner / CT_Polyconic
    {.ct_projection = CT_CassiniSoldner,
     .inputs = {{{ProjNatOriginLatGeoKey, 10.4},
                 {ProjNatOriginLongGeoKey, -61.3},
                 {ProjScaleAtNatOriginGeoKey, 1.0},
                 {ProjFalseOriginEastingGeoKey, 430000.0},
                 {ProjFalseOriginNorthingGeoKey, 325000.0}}},
     .input_count = 5,
     .expected_parm_id0 = ProjNatOriginLatGeoKey,
     .expected_parm0 = 10.4,
     .expected_parm_id1 = ProjNatOriginLongGeoKey,
     .expected_parm1 = -61.3},
    {.ct_projection = CT_Polyconic,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = ProjNatOriginLatGeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjNatOriginLongGeoKey,
     .expected_parm1 = 0.0},
    // 7. CT_AzimuthalEquidistant / Miller / Gnomonic / LAEA / Ortho / NZMG
    {.ct_projection = CT_AzimuthalEquidistant,
     .inputs = {{{ProjCenterLatGeoKey, 13.4}, {ProjCenterLongGeoKey, 144.7}}},
     .input_count = 2,
     .expected_parm_id0 = ProjCenterLatGeoKey,
     .expected_parm0 = 13.4,
     .expected_parm_id1 = ProjCenterLongGeoKey,
     .expected_parm1 = 144.7},
    {.ct_projection = CT_Orthographic,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = ProjCenterLatGeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjCenterLongGeoKey,
     .expected_parm1 = 0.0},
    // 8. CT_Equirectangular
    {.ct_projection = CT_Equirectangular,
     .inputs = {{{ProjCenterLatGeoKey, 0.0},
                 {ProjCenterLongGeoKey, 10.0},
                 {ProjStdParallel1GeoKey, 30.0}}},
     .input_count = 3,
     .expected_parm_id0 = ProjCenterLatGeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjCenterLongGeoKey,
     .expected_parm1 = 10.0},
    {.ct_projection = CT_Equirectangular,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = ProjCenterLatGeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjCenterLongGeoKey,
     .expected_parm1 = 0.0},
    // 9. CT_Robinson / CT_Sinusoidal / CT_VanDerGrinten
    {.ct_projection = CT_Robinson,
     .inputs = {{{ProjCenterLongGeoKey, 12.0}}},
     .input_count = 1,
     .expected_parm_id0 = 0,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjCenterLongGeoKey,
     .expected_parm1 = 12.0},
    {.ct_projection = CT_Sinusoidal,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = 0,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjCenterLongGeoKey,
     .expected_parm1 = 0.0},
    // 10. CT_PolarStereographic
    {.ct_projection = CT_PolarStereographic,
     .inputs = {{{ProjNatOriginLatGeoKey, 71.0},
                 {ProjStraightVertPoleLongGeoKey, -96.0},
                 {ProjScaleAtCenterGeoKey, 0.972}}},
     .input_count = 3,
     .expected_parm_id0 = ProjNatOriginLatGeoKey,
     .expected_parm0 = 71.0,
     .expected_parm_id1 = ProjStraightVertPoleLongGeoKey,
     .expected_parm1 = -96.0},
    {.ct_projection = CT_PolarStereographic,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = ProjNatOriginLatGeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjStraightVertPoleLongGeoKey,
     .expected_parm1 = 0.0},
    // 11. CT_LambertConfConic_2SP
    {.ct_projection = CT_LambertConfConic_2SP,
     .inputs = {{{ProjFalseOriginLatGeoKey, 37.0},
                 {ProjFalseOriginLongGeoKey, -120.0},
                 {ProjStdParallel1GeoKey, 33.0},
                 {ProjStdParallel2GeoKey, 45.0}}},
     .input_count = 4,
     .expected_parm_id0 = ProjFalseOriginLatGeoKey,
     .expected_parm0 = 37.0,
     .expected_parm_id1 = ProjFalseOriginLongGeoKey,
     .expected_parm1 = -120.0},
    {.ct_projection = CT_LambertConfConic_2SP,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = ProjFalseOriginLatGeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjFalseOriginLongGeoKey,
     .expected_parm1 = 0.0},
    // 12. CT_AlbersEqualArea / CT_EquidistantConic
    {.ct_projection = CT_AlbersEqualArea,
     .inputs = {{{ProjStdParallel1GeoKey, 29.5},
                 {ProjStdParallel2GeoKey, 45.5},
                 {ProjNatOriginLatGeoKey, 23.0},
                 {ProjNatOriginLongGeoKey, -96.0}}},
     .input_count = 4,
     .expected_parm_id0 = ProjStdParallel1GeoKey,
     .expected_parm0 = 29.5,
     .expected_parm_id1 = ProjStdParallel2GeoKey,
     .expected_parm1 = 45.5},
    {.ct_projection = CT_EquidistantConic,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = ProjStdParallel1GeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjStdParallel2GeoKey,
     .expected_parm1 = 0.0},
    // 13. CT_CylindricalEqualArea
    {.ct_projection = CT_CylindricalEqualArea,
     .inputs = {{{ProjStdParallel1GeoKey, 30.0},
                 {ProjNatOriginLongGeoKey, -15.0}}},
     .input_count = 2,
     .expected_parm_id0 = ProjStdParallel1GeoKey,
     .expected_parm0 = 30.0,
     .expected_parm_id1 = ProjNatOriginLongGeoKey,
     .expected_parm1 = -15.0},
    {.ct_projection = CT_CylindricalEqualArea,
     .inputs = {},
     .input_count = 0,
     .expected_parm_id0 = ProjStdParallel1GeoKey,
     .expected_parm0 = 0.0,
     .expected_parm_id1 = ProjNatOriginLongGeoKey,
     .expected_parm1 = 0.0},
};

}  // namespace libgeotiff

#endif  // THIRD_PARTY_LIBGEOTIFF_GOOGLE_GEO_NORMALIZE_TEST_CONSTANTS_H_
