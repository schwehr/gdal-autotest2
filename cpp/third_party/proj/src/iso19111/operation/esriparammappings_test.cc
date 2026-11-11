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
// Unit tests for ESRI projection and parameter name mappings to WKT2/EPSG
// (proj/src/iso19111/operation/esriparammappings.cpp). Note: in google3 default
// non-GCE builds, proj.db is embedded in memory and networking is disabled.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/operation/esriparammappings.cpp

#include "third_party/proj4/proj/src/iso19111/operation/esriparammappings.hpp"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <set>
#include <span>  // NOLINT(build/c++20)
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_constants.h"

namespace proj {
namespace {

using ::osgeo::proj::operation::ESRIMethodMapping;
using ::osgeo::proj::operation::ESRIParamMapping;
using ::osgeo::proj::operation::getEsriMappings;
using ::osgeo::proj::operation::getMappingsFromESRI;
using ::osgeo::proj::operation::paramsESRI_Equidistant_Cylindrical;
using ::osgeo::proj::operation::paramsESRI_Gauss_Kruger;
using ::osgeo::proj::operation::
    paramsESRI_Hotine_Oblique_Mercator_Azimuth_Center;
using ::osgeo::proj::operation::
    paramsESRI_Hotine_Oblique_Mercator_Azimuth_Natural_Origin;
using ::osgeo::proj::operation::paramsESRI_Plate_Carree;
using ::osgeo::proj::operation::paramsESRI_Rectified_Skew_Orthomorphic_Center;
using ::osgeo::proj::operation::
    paramsESRI_Rectified_Skew_Orthomorphic_Natural_Origin;
using ::osgeo::proj::operation::paramsESRI_Transverse_Mercator;
using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr size_t kMaxParams = 16;

struct ExpectedParamSpec {
  std::string_view esri_name;
  const char* wkt2_name;
  int epsg_code;
  std::string_view fixed_value;
  bool is_fixed_value;
};

struct ExpectedMethodSpec {
  std::string_view esri_name;
  std::string_view wkt2_name;
  int epsg_code;
};

std::span<const ESRIParamMapping> ParamSpan(const ESRIParamMapping* params) {
  size_t count = 0;
  while (count < kMaxParams && params[count].esri_name != nullptr) {
    ++count;
  }
  return {params, count};
}

void ExpectParamListMatches(const ESRIParamMapping* actual,
                            std::span<const ExpectedParamSpec> expected) {
  ASSERT_THAT(actual, NotNull());
  ASSERT_LT(expected.size(), kMaxParams);
  const auto span = ParamSpan(actual);
  ASSERT_EQ(expected.size(), span.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    ASSERT_THAT(span[i].esri_name, NotNull()) << "param index=" << i;
    EXPECT_EQ(expected[i].esri_name, span[i].esri_name) << "param index=" << i;
    if (expected[i].wkt2_name == nullptr) {
      EXPECT_EQ(nullptr, span[i].wkt2_name) << expected[i].esri_name;
    } else {
      ASSERT_THAT(span[i].wkt2_name, NotNull()) << expected[i].esri_name;
      EXPECT_STREQ(expected[i].wkt2_name, span[i].wkt2_name)
          << expected[i].esri_name;
    }
    EXPECT_EQ(expected[i].epsg_code, span[i].epsg_code)
        << expected[i].esri_name;
    ASSERT_THAT(span[i].fixed_value, NotNull()) << expected[i].esri_name;
    EXPECT_EQ(expected[i].fixed_value, span[i].fixed_value)
        << expected[i].esri_name;
    EXPECT_EQ(expected[i].is_fixed_value, span[i].is_fixed_value)
        << expected[i].esri_name;
  }
  // Verify sentinel terminator fields.
  const ESRIParamMapping& sentinel = actual[expected.size()];
  EXPECT_EQ(nullptr, sentinel.esri_name);
  EXPECT_EQ(nullptr, sentinel.wkt2_name);
  EXPECT_EQ(0, sentinel.epsg_code);
  ASSERT_THAT(sentinel.fixed_value, NotNull());
  EXPECT_STREQ("0.0", sentinel.fixed_value);
  EXPECT_FALSE(sentinel.is_fixed_value);
}

TEST(EsriParamMappingsTest, GetEsriMappingsReturnsStaticTableWithValidEntries) {
  // 1. Verify getEsriMappings overwrites arbitrary initial nElts values and
  // returns the same static storage across calls.
  size_t n_elts_zero = 0;
  const ESRIMethodMapping* mappings_a = getEsriMappings(n_elts_zero);
  size_t n_elts_sentinel = 12345;
  const ESRIMethodMapping* mappings_b = getEsriMappings(n_elts_sentinel);

  ASSERT_THAT(mappings_a, NotNull());
  EXPECT_EQ(mappings_a, mappings_b);
  EXPECT_EQ(n_elts_zero, n_elts_sentinel);
  // PROJ 9.9.0 defines 82 ESRI projection method mappings.
  EXPECT_GE(n_elts_zero, 82u);

  const std::span<const ESRIMethodMapping> table(mappings_a, n_elts_zero);
  for (size_t i = 0; i < table.size(); ++i) {
    const ESRIMethodMapping& entry = table[i];
    ASSERT_THAT(entry.esri_name, NotNull()) << "row=" << i;
    EXPECT_STRNE("", entry.esri_name) << "row=" << i;
    ASSERT_THAT(entry.wkt2_name, NotNull()) << "row=" << i;
    EXPECT_STRNE("", entry.wkt2_name) << "row=" << i;
    EXPECT_GE(entry.epsg_code, 0) << "row=" << i;
    ASSERT_THAT(entry.params, NotNull()) << "row=" << i;

    // 2. Verify every parameter array is sentinel-terminated, non-empty, starts
    // with False_Easting / False_Northing, has unique ESRI names, and has
    // consistent WKT2/EPSG/fixed_value metadata.
    size_t param_count = 0;
    std::set<std::string_view> seen_param_names;
    while (param_count < kMaxParams &&
           entry.params[param_count].esri_name != nullptr) {
      const ESRIParamMapping& param = entry.params[param_count];
      EXPECT_STRNE("", param.esri_name)
          << entry.esri_name << " param=" << param_count;
      EXPECT_TRUE(seen_param_names.insert(param.esri_name).second)
          << entry.esri_name << " duplicate param=" << param.esri_name;

      ASSERT_THAT(param.fixed_value, NotNull())
          << entry.esri_name << " param=" << param.esri_name;
      EXPECT_STRNE("", param.fixed_value);
      char* end = nullptr;
      const double parsed = std::strtod(param.fixed_value, &end);
      EXPECT_NE(param.fixed_value, end) << param.fixed_value;
      EXPECT_EQ('\0', *end) << param.fixed_value;
      EXPECT_TRUE(std::isfinite(parsed)) << param.fixed_value;

      if (param.wkt2_name == nullptr) {
        EXPECT_EQ(0, param.epsg_code)
            << entry.esri_name << " " << param.esri_name;
        EXPECT_FALSE(param.is_fixed_value)
            << entry.esri_name << " " << param.esri_name;
      } else {
        EXPECT_STRNE("", param.wkt2_name);
        EXPECT_GE(param.epsg_code, 0);
      }
      if (param.epsg_code != 0) {
        EXPECT_GT(param.epsg_code, 0);
        EXPECT_THAT(param.wkt2_name, NotNull());
      }
      if (param.is_fixed_value) {
        EXPECT_THAT(param.wkt2_name, NotNull());
      }
      ++param_count;
    }

    ASSERT_LT(param_count, kMaxParams) << entry.esri_name;
    ASSERT_GE(param_count, 2u) << entry.esri_name;
    EXPECT_STREQ("False_Easting", entry.params[0].esri_name) << entry.esri_name;
    EXPECT_STREQ("False_Northing", entry.params[1].esri_name)
        << entry.esri_name;
    EXPECT_EQ(nullptr, entry.params[param_count].esri_name);
    EXPECT_EQ(nullptr, entry.params[param_count].wkt2_name);
    EXPECT_EQ(0, entry.params[param_count].epsg_code);
    EXPECT_FALSE(entry.params[param_count].is_fixed_value);

    // 3. Verify every row in the static table is reachable by pointer identity
    // through getMappingsFromESRI.
    const std::vector<const ESRIMethodMapping*> by_name =
        getMappingsFromESRI(entry.esri_name);
    EXPECT_FALSE(by_name.empty()) << entry.esri_name;
    bool found_self = false;
    for (const ESRIMethodMapping* candidate : by_name) {
      if (candidate == &entry) {
        found_self = true;
      }
    }
    EXPECT_TRUE(found_self) << entry.esri_name << " row=" << i;
  }
}

TEST(EsriParamMappingsTest, GetMappingsFromEsriCaseInsensitiveAndNonMatches) {
  struct LookupCase {
    std::string_view query;
    std::string_view canonical_query;
    size_t expected_count;
  };
  constexpr LookupCase kCases[] = {
      {.query = "Transverse_Mercator",
       .canonical_query = "Transverse_Mercator",
       .expected_count = 1},
      {.query = "transverse_mercator",
       .canonical_query = "Transverse_Mercator",
       .expected_count = 1},
      {.query = "TRANSVERSE_MERCATOR",
       .canonical_query = "Transverse_Mercator",
       .expected_count = 1},
      {.query = "eQuIdIsTaNt_cYlInDrIcAl",
       .canonical_query = "Equidistant_Cylindrical",
       .expected_count = 1},
      {.query = "bEhRmAnN", .canonical_query = "Behrmann", .expected_count = 1},
      {.query = "plate_carree",
       .canonical_query = "Plate_Carree",
       .expected_count = 2},
      {.query = "kRoVaK", .canonical_query = "Krovak", .expected_count = 2},
      {.query = "LAMBERT_CONFORMAL_CONIC",
       .canonical_query = "Lambert_Conformal_Conic",
       .expected_count = 4},
      {.query = "GOODE_HOMOLOSINE",
       .canonical_query = "Goode_Homolosine",
       .expected_count = 3},
      {.query = "peirce_quincuncial",
       .canonical_query = "Peirce_Quincuncial",
       .expected_count = 2},
      // Empty, unknown, WKT2-spaced, prefix, suffix, and whitespace-padded
      // strings must not match.
      {.query = "", .canonical_query = "", .expected_count = 0},
      {.query = "Unknown_Projection",
       .canonical_query = "",
       .expected_count = 0},
      {.query = "Transverse Mercator",
       .canonical_query = "",
       .expected_count = 0},
      {.query = "Mercato", .canonical_query = "", .expected_count = 0},
      {.query = "Transverse_Mercator_Extra",
       .canonical_query = "",
       .expected_count = 0},
      {.query = " Plate_Carree", .canonical_query = "", .expected_count = 0},
      {.query = "Plate_Carree ", .canonical_query = "", .expected_count = 0},
      {.query = "Hotine_Oblique_Mercator",
       .canonical_query = "",
       .expected_count = 0},
  };

  for (const auto& tc : kCases) {
    const std::vector<const ESRIMethodMapping*> res =
        getMappingsFromESRI(std::string(tc.query));
    EXPECT_EQ(tc.expected_count, res.size()) << tc.query;
    if (tc.expected_count > 0) {
      const std::vector<const ESRIMethodMapping*> canonical =
          getMappingsFromESRI(std::string(tc.canonical_query));
      EXPECT_EQ(canonical, res) << tc.query;
    }
  }
}

TEST(EsriParamMappingsTest,
     GetMappingsFromEsriDisambiguatesMultiVariantMethods) {
  // 1. Plate_Carree: ellipsoidal (1028) followed by spherical (1029).
  {
    const auto matches = getMappingsFromESRI("Plate_Carree");
    ASSERT_EQ(2u, matches.size());
    EXPECT_STREQ(EPSG_NAME_METHOD_EQUIDISTANT_CYLINDRICAL,
                 matches[0]->wkt2_name);
    EXPECT_EQ(EPSG_CODE_METHOD_EQUIDISTANT_CYLINDRICAL, matches[0]->epsg_code);
    EXPECT_EQ(paramsESRI_Plate_Carree, matches[0]->params);

    EXPECT_STREQ(EPSG_NAME_METHOD_EQUIDISTANT_CYLINDRICAL_SPHERICAL,
                 matches[1]->wkt2_name);
    EXPECT_EQ(EPSG_CODE_METHOD_EQUIDISTANT_CYLINDRICAL_SPHERICAL,
              matches[1]->epsg_code);
    EXPECT_EQ(paramsESRI_Plate_Carree, matches[1]->params);
  }

  // 2. Lambert_Conformal_Conic: 1SP, 2SP (no scale), 2SP (Scale_Factor=1.0),
  // and 2SP Michigan (Ellipsoid scaling factor).
  {
    const auto matches = getMappingsFromESRI("Lambert_Conformal_Conic");
    ASSERT_EQ(4u, matches.size());

    EXPECT_STREQ(EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_1SP,
                 matches[0]->wkt2_name);
    EXPECT_EQ(EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_1SP,
              matches[0]->epsg_code);
    constexpr ExpectedParamSpec kLcc1Sp[] = {
        {"False_Easting", EPSG_NAME_PARAMETER_FALSE_EASTING,
         EPSG_CODE_PARAMETER_FALSE_EASTING, "0.0", false},
        {"False_Northing", EPSG_NAME_PARAMETER_FALSE_NORTHING,
         EPSG_CODE_PARAMETER_FALSE_NORTHING, "0.0", false},
        {"Central_Meridian", EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
         EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN, "0.0", false},
        {"Standard_Parallel_1", EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
         EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, "0.0", false},
        {"Scale_Factor", EPSG_NAME_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN,
         EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN, "0.0", false},
        {"Latitude_Of_Origin", EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
         EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, "0.0", false},
    };
    ExpectParamListMatches(matches[0]->params, kLcc1Sp);

    EXPECT_STREQ(EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_2SP,
                 matches[1]->wkt2_name);
    EXPECT_EQ(EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP,
              matches[1]->epsg_code);
    EXPECT_EQ(6u, ParamSpan(matches[1]->params).size());

    EXPECT_STREQ(EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_2SP,
                 matches[2]->wkt2_name);
    EXPECT_EQ(EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP,
              matches[2]->epsg_code);
    const auto lcc_alt3 = ParamSpan(matches[2]->params);
    ASSERT_EQ(7u, lcc_alt3.size());
    EXPECT_STREQ("Scale_Factor", lcc_alt3[5].esri_name);
    EXPECT_EQ(nullptr, lcc_alt3[5].wkt2_name);
    EXPECT_EQ(0, lcc_alt3[5].epsg_code);
    EXPECT_STREQ("1.0", lcc_alt3[5].fixed_value);

    EXPECT_STREQ(EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_2SP_MICHIGAN,
                 matches[3]->wkt2_name);
    EXPECT_EQ(EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP_MICHIGAN,
              matches[3]->epsg_code);
    const auto lcc_michigan = ParamSpan(matches[3]->params);
    ASSERT_EQ(7u, lcc_michigan.size());
    EXPECT_STREQ("Scale_Factor", lcc_michigan[5].esri_name);
    EXPECT_STREQ(EPSG_NAME_PARAMETER_ELLIPSOID_SCALE_FACTOR,
                 lcc_michigan[5].wkt2_name);
    EXPECT_EQ(EPSG_CODE_PARAMETER_ELLIPSOID_SCALE_FACTOR,
              lcc_michigan[5].epsg_code);
  }

  // 3. Krovak: standard South-West vs North-Orientated (X_Scale=-1, Rot=90).
  {
    const auto matches = getMappingsFromESRI("Krovak");
    ASSERT_EQ(2u, matches.size());
    EXPECT_STREQ(EPSG_NAME_METHOD_KROVAK, matches[0]->wkt2_name);
    EXPECT_EQ(EPSG_CODE_METHOD_KROVAK, matches[0]->epsg_code);
    const auto krovak1 = ParamSpan(matches[0]->params);
    ASSERT_EQ(10u, krovak1.size());
    EXPECT_STREQ("X_Scale", krovak1[7].esri_name);
    EXPECT_STREQ("1.0", krovak1[7].fixed_value);
    EXPECT_STREQ("Y_Scale", krovak1[8].esri_name);
    EXPECT_STREQ("1.0", krovak1[8].fixed_value);
    EXPECT_STREQ("XY_Plane_Rotation", krovak1[9].esri_name);
    EXPECT_STREQ("0.0", krovak1[9].fixed_value);

    EXPECT_STREQ(EPSG_NAME_METHOD_KROVAK_NORTH_ORIENTED, matches[1]->wkt2_name);
    EXPECT_EQ(EPSG_CODE_METHOD_KROVAK_NORTH_ORIENTED, matches[1]->epsg_code);
    const auto krovak2 = ParamSpan(matches[1]->params);
    ASSERT_EQ(10u, krovak2.size());
    EXPECT_STREQ("X_Scale", krovak2[7].esri_name);
    EXPECT_STREQ("-1.0", krovak2[7].fixed_value);
    EXPECT_STREQ("Y_Scale", krovak2[8].esri_name);
    EXPECT_STREQ("1.0", krovak2[8].fixed_value);
    EXPECT_STREQ("XY_Plane_Rotation", krovak2[9].esri_name);
    EXPECT_STREQ("90.0", krovak2[9].fixed_value);
  }

  // 4. Goode_Homolosine: Option 0.0 (uninterrupted), 1.0 (interrupted), 2.0
  // (interrupted ocean).
  {
    const auto matches = getMappingsFromESRI("Goode_Homolosine");
    ASSERT_EQ(3u, matches.size());
    constexpr std::string_view kExpectedMethods[] = {
        PROJ_WKT2_NAME_METHOD_GOODE_HOMOLOSINE,
        PROJ_WKT2_NAME_METHOD_INTERRUPTED_GOODE_HOMOLOSINE,
        PROJ_WKT2_NAME_METHOD_INTERRUPTED_GOODE_HOMOLOSINE_OCEAN,
    };
    constexpr std::string_view kExpectedOptions[] = {"0.0", "1.0", "2.0"};
    for (size_t i = 0; i < 3; ++i) {
      EXPECT_EQ(kExpectedMethods[i], matches[i]->wkt2_name);
      EXPECT_EQ(0, matches[i]->epsg_code);
      const auto params = ParamSpan(matches[i]->params);
      ASSERT_EQ(4u, params.size());
      EXPECT_STREQ("Option", params[3].esri_name);
      EXPECT_EQ(nullptr, params[3].wkt2_name);
      EXPECT_EQ(kExpectedOptions[i], params[3].fixed_value);
    }
  }

  // 5. Peirce_Quincuncial: Option 0.0 (Square) vs 1.0 (Diamond).
  {
    const auto matches = getMappingsFromESRI("Peirce_Quincuncial");
    ASSERT_EQ(2u, matches.size());
    EXPECT_STREQ(PROJ_WKT2_NAME_METHOD_PEIRCE_QUINCUNCIAL_SQUARE,
                 matches[0]->wkt2_name);
    EXPECT_EQ(0, matches[0]->epsg_code);
    const auto square_params = ParamSpan(matches[0]->params);
    ASSERT_EQ(6u, square_params.size());
    EXPECT_STREQ("Option", square_params[5].esri_name);
    EXPECT_STREQ("0.0", square_params[5].fixed_value);

    EXPECT_STREQ(PROJ_WKT2_NAME_METHOD_PEIRCE_QUINCUNCIAL_DIAMOND,
                 matches[1]->wkt2_name);
    EXPECT_EQ(0, matches[1]->epsg_code);
    const auto diamond_params = ParamSpan(matches[1]->params);
    ASSERT_EQ(6u, diamond_params.size());
    EXPECT_STREQ("Option", diamond_params[5].esri_name);
    EXPECT_STREQ("1.0", diamond_params[5].fixed_value);
  }
}

TEST(EsriParamMappingsTest, ExportedExternParamArraysMatchExpectedSpecs) {
  constexpr ExpectedParamSpec kPlateCarree[] = {
      {"False_Easting", EPSG_NAME_PARAMETER_FALSE_EASTING,
       EPSG_CODE_PARAMETER_FALSE_EASTING, "0.0", false},
      {"False_Northing", EPSG_NAME_PARAMETER_FALSE_NORTHING,
       EPSG_CODE_PARAMETER_FALSE_NORTHING, "0.0", false},
      {"Central_Meridian", EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN, "0.0", false},
  };
  ExpectParamListMatches(paramsESRI_Plate_Carree, kPlateCarree);
  EXPECT_EQ(paramsESRI_Plate_Carree,
            getMappingsFromESRI("Plate_Carree").front()->params);

  constexpr ExpectedParamSpec kEquidistantCylindrical[] = {
      {"False_Easting", EPSG_NAME_PARAMETER_FALSE_EASTING,
       EPSG_CODE_PARAMETER_FALSE_EASTING, "0.0", false},
      {"False_Northing", EPSG_NAME_PARAMETER_FALSE_NORTHING,
       EPSG_CODE_PARAMETER_FALSE_NORTHING, "0.0", false},
      {"Central_Meridian", EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN, "0.0", false},
      {"Standard_Parallel_1", EPSG_NAME_PARAMETER_LATITUDE_1ST_STD_PARALLEL,
       EPSG_CODE_PARAMETER_LATITUDE_1ST_STD_PARALLEL, "0.0", false},
  };
  ExpectParamListMatches(paramsESRI_Equidistant_Cylindrical,
                         kEquidistantCylindrical);
  EXPECT_EQ(paramsESRI_Equidistant_Cylindrical,
            getMappingsFromESRI("Equidistant_Cylindrical").front()->params);

  constexpr ExpectedParamSpec kTmercParams[] = {
      {"False_Easting", EPSG_NAME_PARAMETER_FALSE_EASTING,
       EPSG_CODE_PARAMETER_FALSE_EASTING, "0.0", false},
      {"False_Northing", EPSG_NAME_PARAMETER_FALSE_NORTHING,
       EPSG_CODE_PARAMETER_FALSE_NORTHING, "0.0", false},
      {"Central_Meridian", EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN, "0.0", false},
      {"Scale_Factor", EPSG_NAME_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN, "0.0", false},
      {"Latitude_Of_Origin", EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, "0.0", false},
  };
  ExpectParamListMatches(paramsESRI_Gauss_Kruger, kTmercParams);
  ExpectParamListMatches(paramsESRI_Transverse_Mercator, kTmercParams);
  EXPECT_EQ(paramsESRI_Gauss_Kruger,
            getMappingsFromESRI("Gauss_Kruger").front()->params);
  EXPECT_EQ(paramsESRI_Transverse_Mercator,
            getMappingsFromESRI("Transverse_Mercator").front()->params);

  constexpr ExpectedParamSpec kHomNatOrigin[] = {
      {"False_Easting", EPSG_NAME_PARAMETER_FALSE_EASTING,
       EPSG_CODE_PARAMETER_FALSE_EASTING, "0.0", false},
      {"False_Northing", EPSG_NAME_PARAMETER_FALSE_NORTHING,
       EPSG_CODE_PARAMETER_FALSE_NORTHING, "0.0", false},
      {"Scale_Factor", EPSG_NAME_PARAMETER_SCALE_FACTOR_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_SCALE_FACTOR_PROJECTION_CENTRE, "0.0", false},
      {"Azimuth", EPSG_NAME_PARAMETER_AZIMUTH_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_AZIMUTH_PROJECTION_CENTRE, "0.0", false},
      {"Longitude_Of_Center", EPSG_NAME_PARAMETER_LONGITUDE_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_LONGITUDE_PROJECTION_CENTRE, "0.0", false},
      {"Latitude_Of_Center", EPSG_NAME_PARAMETER_LATITUDE_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_LATITUDE_PROJECTION_CENTRE, "0.0", false},
  };
  ExpectParamListMatches(
      paramsESRI_Hotine_Oblique_Mercator_Azimuth_Natural_Origin, kHomNatOrigin);
  EXPECT_EQ(
      paramsESRI_Hotine_Oblique_Mercator_Azimuth_Natural_Origin,
      getMappingsFromESRI("Hotine_Oblique_Mercator_Azimuth_Natural_Origin")
          .front()
          ->params);

  constexpr ExpectedParamSpec kRsoNatOrigin[] = {
      {"False_Easting", EPSG_NAME_PARAMETER_FALSE_EASTING,
       EPSG_CODE_PARAMETER_FALSE_EASTING, "0.0", false},
      {"False_Northing", EPSG_NAME_PARAMETER_FALSE_NORTHING,
       EPSG_CODE_PARAMETER_FALSE_NORTHING, "0.0", false},
      {"Scale_Factor", EPSG_NAME_PARAMETER_SCALE_FACTOR_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_SCALE_FACTOR_PROJECTION_CENTRE, "0.0", false},
      {"Azimuth", EPSG_NAME_PARAMETER_AZIMUTH_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_AZIMUTH_PROJECTION_CENTRE, "0.0", false},
      {"Longitude_Of_Center", EPSG_NAME_PARAMETER_LONGITUDE_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_LONGITUDE_PROJECTION_CENTRE, "0.0", false},
      {"Latitude_Of_Center", EPSG_NAME_PARAMETER_LATITUDE_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_LATITUDE_PROJECTION_CENTRE, "0.0", false},
      {"XY_Plane_Rotation", EPSG_NAME_PARAMETER_ANGLE_RECTIFIED_TO_SKEW_GRID,
       EPSG_CODE_PARAMETER_ANGLE_RECTIFIED_TO_SKEW_GRID, "0.0", false},
  };
  ExpectParamListMatches(paramsESRI_Rectified_Skew_Orthomorphic_Natural_Origin,
                         kRsoNatOrigin);
  EXPECT_EQ(paramsESRI_Rectified_Skew_Orthomorphic_Natural_Origin,
            getMappingsFromESRI("Rectified_Skew_Orthomorphic_Natural_Origin")
                .front()
                ->params);

  constexpr ExpectedParamSpec kHomCenter[] = {
      {"False_Easting", EPSG_NAME_PARAMETER_EASTING_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_EASTING_PROJECTION_CENTRE, "0.0", false},
      {"False_Northing", EPSG_NAME_PARAMETER_NORTHING_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_NORTHING_PROJECTION_CENTRE, "0.0", false},
      {"Scale_Factor", EPSG_NAME_PARAMETER_SCALE_FACTOR_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_SCALE_FACTOR_PROJECTION_CENTRE, "0.0", false},
      {"Azimuth", EPSG_NAME_PARAMETER_AZIMUTH_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_AZIMUTH_PROJECTION_CENTRE, "0.0", false},
      {"Longitude_Of_Center", EPSG_NAME_PARAMETER_LONGITUDE_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_LONGITUDE_PROJECTION_CENTRE, "0.0", false},
      {"Latitude_Of_Center", EPSG_NAME_PARAMETER_LATITUDE_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_LATITUDE_PROJECTION_CENTRE, "0.0", false},
  };
  ExpectParamListMatches(paramsESRI_Hotine_Oblique_Mercator_Azimuth_Center,
                         kHomCenter);
  EXPECT_EQ(paramsESRI_Hotine_Oblique_Mercator_Azimuth_Center,
            getMappingsFromESRI("Hotine_Oblique_Mercator_Azimuth_Center")
                .front()
                ->params);

  constexpr ExpectedParamSpec kRsoCenter[] = {
      {"False_Easting", EPSG_NAME_PARAMETER_EASTING_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_EASTING_PROJECTION_CENTRE, "0.0", false},
      {"False_Northing", EPSG_NAME_PARAMETER_NORTHING_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_NORTHING_PROJECTION_CENTRE, "0.0", false},
      {"Scale_Factor", EPSG_NAME_PARAMETER_SCALE_FACTOR_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_SCALE_FACTOR_PROJECTION_CENTRE, "0.0", false},
      {"Azimuth", EPSG_NAME_PARAMETER_AZIMUTH_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_AZIMUTH_PROJECTION_CENTRE, "0.0", false},
      {"Longitude_Of_Center", EPSG_NAME_PARAMETER_LONGITUDE_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_LONGITUDE_PROJECTION_CENTRE, "0.0", false},
      {"Latitude_Of_Center", EPSG_NAME_PARAMETER_LATITUDE_PROJECTION_CENTRE,
       EPSG_CODE_PARAMETER_LATITUDE_PROJECTION_CENTRE, "0.0", false},
      {"XY_Plane_Rotation", EPSG_NAME_PARAMETER_ANGLE_RECTIFIED_TO_SKEW_GRID,
       EPSG_CODE_PARAMETER_ANGLE_RECTIFIED_TO_SKEW_GRID, "0.0", false},
  };
  ExpectParamListMatches(paramsESRI_Rectified_Skew_Orthomorphic_Center,
                         kRsoCenter);
  EXPECT_EQ(paramsESRI_Rectified_Skew_Orthomorphic_Center,
            getMappingsFromESRI("Rectified_Skew_Orthomorphic_Center")
                .front()
                ->params);
}

TEST(EsriParamMappingsTest, AllSingleVariantMethodsAndSpecialParamsMatch) {
  constexpr ExpectedMethodSpec kSingleVariantMethods[] = {
      {.esri_name = "Equidistant_Cylindrical",
       .wkt2_name = EPSG_NAME_METHOD_EQUIDISTANT_CYLINDRICAL,
       .epsg_code = EPSG_CODE_METHOD_EQUIDISTANT_CYLINDRICAL},
      {.esri_name = "Miller_Cylindrical",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_MILLER_CYLINDRICAL,
       .epsg_code = 0},
      {.esri_name = "Mercator",
       .wkt2_name = EPSG_NAME_METHOD_MERCATOR_VARIANT_B,
       .epsg_code = EPSG_CODE_METHOD_MERCATOR_VARIANT_B},
      {.esri_name = "Gauss_Kruger",
       .wkt2_name = EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
       .epsg_code = EPSG_CODE_METHOD_TRANSVERSE_MERCATOR},
      {.esri_name = "Transverse_Mercator",
       .wkt2_name = EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
       .epsg_code = EPSG_CODE_METHOD_TRANSVERSE_MERCATOR},
      {.esri_name = "Transverse_Mercator_Complex",
       .wkt2_name = EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
       .epsg_code = EPSG_CODE_METHOD_TRANSVERSE_MERCATOR},
      {.esri_name = "Albers",
       .wkt2_name = EPSG_NAME_METHOD_ALBERS_EQUAL_AREA,
       .epsg_code = EPSG_CODE_METHOD_ALBERS_EQUAL_AREA},
      {.esri_name = "Sinusoidal",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_SINUSOIDAL,
       .epsg_code = 0},
      {.esri_name = "Mollweide",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_MOLLWEIDE,
       .epsg_code = 0},
      {.esri_name = "Eckert_I",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_ECKERT_I,
       .epsg_code = 0},
      {.esri_name = "Eckert_II",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_ECKERT_II,
       .epsg_code = 0},
      {.esri_name = "Eckert_III",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_ECKERT_III,
       .epsg_code = 0},
      {.esri_name = "Eckert_IV",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_ECKERT_IV,
       .epsg_code = 0},
      {.esri_name = "Eckert_V",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_ECKERT_V,
       .epsg_code = 0},
      {.esri_name = "Eckert_VI",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_ECKERT_VI,
       .epsg_code = 0},
      {.esri_name = "Gall_Stereographic",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_GALL_STEREOGRAPHIC,
       .epsg_code = 0},
      {.esri_name = "Winkel_I", .wkt2_name = "Winkel I", .epsg_code = 0},
      {.esri_name = "Winkel_II", .wkt2_name = "Winkel II", .epsg_code = 0},
      {.esri_name = "Polyconic",
       .wkt2_name = EPSG_NAME_METHOD_AMERICAN_POLYCONIC,
       .epsg_code = EPSG_CODE_METHOD_AMERICAN_POLYCONIC},
      {.esri_name = "Quartic_Authalic",
       .wkt2_name = "Quartic Authalic",
       .epsg_code = 0},
      {.esri_name = "Loximuthal", .wkt2_name = "Loximuthal", .epsg_code = 0},
      {.esri_name = "Bonne",
       .wkt2_name = EPSG_NAME_METHOD_BONNE,
       .epsg_code = EPSG_CODE_METHOD_BONNE},
      {.esri_name = "Hotine_Oblique_Mercator_Two_Point_Natural_Origin",
       .wkt2_name =
           // NOLINTNEXTLINE(whitespace/line_length)
       PROJ_WKT2_NAME_METHOD_HOTINE_OBLIQUE_MERCATOR_TWO_POINT_NATURAL_ORIGIN,
       .epsg_code = 0},
      {.esri_name = "Stereographic",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_STEREOGRAPHIC,
       .epsg_code = 0},
      {.esri_name = "Polar_Stereographic_Variant_A",
       .wkt2_name = EPSG_NAME_METHOD_POLAR_STEREOGRAPHIC_VARIANT_A,
       .epsg_code = EPSG_CODE_METHOD_POLAR_STEREOGRAPHIC_VARIANT_A},
      {.esri_name = "Equidistant_Conic",
       .wkt2_name = EPSG_NAME_METHOD_EQUIDISTANT_CONIC,
       .epsg_code = EPSG_CODE_METHOD_EQUIDISTANT_CONIC},
      {.esri_name = "Cassini",
       .wkt2_name = EPSG_NAME_METHOD_CASSINI_SOLDNER,
       .epsg_code = EPSG_CODE_METHOD_CASSINI_SOLDNER},
      {.esri_name = "Van_der_Grinten_I",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_VAN_DER_GRINTEN,
       .epsg_code = 0},
      {.esri_name = "Robinson",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_ROBINSON,
       .epsg_code = 0},
      {.esri_name = "Two_Point_Equidistant",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_TWO_POINT_EQUIDISTANT,
       .epsg_code = 0},
      {.esri_name = "Azimuthal_Equidistant",
       .wkt2_name = EPSG_NAME_METHOD_AZIMUTHAL_EQUIDISTANT,
       .epsg_code = EPSG_CODE_METHOD_AZIMUTHAL_EQUIDISTANT},
      {.esri_name = "Lambert_Azimuthal_Equal_Area",
       .wkt2_name = EPSG_NAME_METHOD_LAMBERT_AZIMUTHAL_EQUAL_AREA,
       .epsg_code = EPSG_CODE_METHOD_LAMBERT_AZIMUTHAL_EQUAL_AREA},
      {.esri_name = "Cylindrical_Equal_Area",
       .wkt2_name = EPSG_NAME_METHOD_LAMBERT_CYLINDRICAL_EQUAL_AREA,
       .epsg_code = EPSG_CODE_METHOD_LAMBERT_CYLINDRICAL_EQUAL_AREA},
      {.esri_name = "Behrmann",
       .wkt2_name = EPSG_NAME_METHOD_LAMBERT_CYLINDRICAL_EQUAL_AREA,
       .epsg_code = EPSG_CODE_METHOD_LAMBERT_CYLINDRICAL_EQUAL_AREA},
      {.esri_name = "Hotine_Oblique_Mercator_Two_Point_Center",
       .wkt2_name =
           // NOLINTNEXTLINE(whitespace/line_length)
       PROJ_WKT2_NAME_METHOD_HOTINE_OBLIQUE_MERCATOR_TWO_POINT_NATURAL_ORIGIN,
       .epsg_code = 0},
      {.esri_name = "Hotine_Oblique_Mercator_Azimuth_Natural_Origin",
       .wkt2_name = EPSG_NAME_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_A,
       .epsg_code = EPSG_CODE_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_A},
      {.esri_name = "Hotine_Oblique_Mercator_Azimuth_Center",
       .wkt2_name = EPSG_NAME_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_B,
       .epsg_code = EPSG_CODE_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_B},
      {.esri_name = "Double_Stereographic",
       .wkt2_name = EPSG_NAME_METHOD_OBLIQUE_STEREOGRAPHIC,
       .epsg_code = EPSG_CODE_METHOD_OBLIQUE_STEREOGRAPHIC},
      {.esri_name = "New_Zealand_Map_Grid",
       .wkt2_name = EPSG_NAME_METHOD_NZMG,
       .epsg_code = EPSG_CODE_METHOD_NZMG},
      {.esri_name = "Orthographic",
       .wkt2_name = PROJ_WKT2_NAME_ORTHOGRAPHIC_SPHERICAL,
       .epsg_code = 0},
      {.esri_name = "Local",
       .wkt2_name = EPSG_NAME_METHOD_LOCAL_ORTHOGRAPHIC,
       .epsg_code = EPSG_CODE_METHOD_LOCAL_ORTHOGRAPHIC},
      {.esri_name = "Winkel_Tripel",
       .wkt2_name = "Winkel Tripel",
       .epsg_code = 0},
      {.esri_name = "Aitoff", .wkt2_name = "Aitoff", .epsg_code = 0},
      {.esri_name = "Flat_Polar_Quartic",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_FLAT_POLAR_QUARTIC,
       .epsg_code = 0},
      {.esri_name = "Craster_Parabolic",
       .wkt2_name = "Craster Parabolic",
       .epsg_code = 0},
      {.esri_name = "Gnomonic",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_GNOMONIC,
       .epsg_code = 0},
      {.esri_name = "Times",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_TIMES,
       .epsg_code = 0},
      {.esri_name = "Vertical_Near_Side_Perspective",
       .wkt2_name = EPSG_NAME_METHOD_VERTICAL_PERSPECTIVE,
       .epsg_code = EPSG_CODE_METHOD_VERTICAL_PERSPECTIVE},
      {.esri_name = "Stereographic_North_Pole",
       .wkt2_name = EPSG_NAME_METHOD_POLAR_STEREOGRAPHIC_VARIANT_B,
       .epsg_code = EPSG_CODE_METHOD_POLAR_STEREOGRAPHIC_VARIANT_B},
      {.esri_name = "Stereographic_South_Pole",
       .wkt2_name = EPSG_NAME_METHOD_POLAR_STEREOGRAPHIC_VARIANT_B,
       .epsg_code = EPSG_CODE_METHOD_POLAR_STEREOGRAPHIC_VARIANT_B},
      {.esri_name = "Rectified_Skew_Orthomorphic_Natural_Origin",
       .wkt2_name = EPSG_NAME_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_A,
       .epsg_code = EPSG_CODE_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_A},
      {.esri_name = "Rectified_Skew_Orthomorphic_Center",
       .wkt2_name = EPSG_NAME_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_B,
       .epsg_code = EPSG_CODE_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_B},
      {.esri_name = "Equidistant_Cylindrical_Ellipsoidal",
       .wkt2_name = EPSG_NAME_METHOD_EQUIDISTANT_CYLINDRICAL,
       .epsg_code = EPSG_CODE_METHOD_EQUIDISTANT_CYLINDRICAL},
      {.esri_name = "Laborde_Oblique_Mercator",
       .wkt2_name = EPSG_NAME_METHOD_LABORDE_OBLIQUE_MERCATOR,
       .epsg_code = EPSG_CODE_METHOD_LABORDE_OBLIQUE_MERCATOR},
      {.esri_name = "Gnomonic_Ellipsoidal",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_GNOMONIC,
       .epsg_code = 0},
      {.esri_name = "Wagner_IV",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_WAGNER_IV,
       .epsg_code = 0},
      {.esri_name = "Wagner_V",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_WAGNER_V,
       .epsg_code = 0},
      {.esri_name = "Wagner_VII",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_WAGNER_VII,
       .epsg_code = 0},
      {.esri_name = "Natural_Earth",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_NATURAL_EARTH,
       .epsg_code = 0},
      {.esri_name = "Natural_Earth_II",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_NATURAL_EARTH_II,
       .epsg_code = 0},
      {.esri_name = "Patterson",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_PATTERSON,
       .epsg_code = 0},
      {.esri_name = "Compact_Miller",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_COMPACT_MILLER,
       .epsg_code = 0},
      {.esri_name = "Geostationary_Satellite",
       .wkt2_name = PROJ_WKT2_NAME_METHOD_GEOSTATIONARY_SATELLITE_SWEEP_Y,
       .epsg_code = 0},
      {.esri_name = "Mercator_Auxiliary_Sphere",
       .wkt2_name = EPSG_NAME_METHOD_POPULAR_VISUALISATION_PSEUDO_MERCATOR,
       .epsg_code = EPSG_CODE_METHOD_POPULAR_VISUALISATION_PSEUDO_MERCATOR},
      {.esri_name = "Mercator_Variant_A",
       .wkt2_name = EPSG_NAME_METHOD_MERCATOR_VARIANT_A,
       .epsg_code = EPSG_CODE_METHOD_MERCATOR_VARIANT_A},
      {.esri_name = "Mercator_Variant_C",
       .wkt2_name = EPSG_NAME_METHOD_MERCATOR_VARIANT_B,
       .epsg_code = EPSG_CODE_METHOD_MERCATOR_VARIANT_B},
      {.esri_name = "Transverse_Cylindrical_Equal_Area",
       .wkt2_name = "Transverse Cylindrical Equal Area",
       .epsg_code = 0},
      {.esri_name = "IGAC_Plano_Cartesiano",
       .wkt2_name = EPSG_NAME_METHOD_COLOMBIA_URBAN,
       .epsg_code = EPSG_CODE_METHOD_COLOMBIA_URBAN},
      {.esri_name = "Equal_Earth",
       .wkt2_name = EPSG_NAME_METHOD_EQUAL_EARTH,
       .epsg_code = EPSG_CODE_METHOD_EQUAL_EARTH},
  };

  for (const auto& tc : kSingleVariantMethods) {
    const auto matches = getMappingsFromESRI(std::string(tc.esri_name));
    ASSERT_EQ(1u, matches.size()) << tc.esri_name;
    EXPECT_EQ(tc.esri_name, matches[0]->esri_name);
    EXPECT_EQ(tc.wkt2_name, matches[0]->wkt2_name) << tc.esri_name;
    EXPECT_EQ(tc.epsg_code, matches[0]->epsg_code) << tc.esri_name;
  }

  // Verify special parameter tables: Behrmann fixed defaults (0.0 / 30.0),
  // Cassini ignored Scale_Factor=1.0, Mercator_Auxiliary_Sphere
  // Auxiliary_Sphere_Type, Geostationary_Satellite Height/Option, and
  // IGAC_Plano_Cartesiano projection plane origin height.
  constexpr ExpectedParamSpec kBehrmannParams[] = {
      {"False_Easting", EPSG_NAME_PARAMETER_FALSE_EASTING,
       EPSG_CODE_PARAMETER_FALSE_EASTING, "0.0", false},
      {"False_Northing", EPSG_NAME_PARAMETER_FALSE_NORTHING,
       EPSG_CODE_PARAMETER_FALSE_NORTHING, "0.0", false},
      {"Central_Meridian", EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN, "0.0", true},
      {"Standard_Parallel_1", EPSG_NAME_PARAMETER_LATITUDE_1ST_STD_PARALLEL,
       EPSG_CODE_PARAMETER_LATITUDE_1ST_STD_PARALLEL, "30.0", true},
  };
  ExpectParamListMatches(getMappingsFromESRI("Behrmann").front()->params,
                         kBehrmannParams);

  constexpr ExpectedParamSpec kCassiniParams[] = {
      {"False_Easting", EPSG_NAME_PARAMETER_FALSE_EASTING,
       EPSG_CODE_PARAMETER_FALSE_EASTING, "0.0", false},
      {"False_Northing", EPSG_NAME_PARAMETER_FALSE_NORTHING,
       EPSG_CODE_PARAMETER_FALSE_NORTHING, "0.0", false},
      {"Central_Meridian", EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN, "0.0", false},
      {"Scale_Factor", nullptr, 0, "1.0", false},
      {"Latitude_Of_Origin", EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, "0.0", false},
  };
  ExpectParamListMatches(getMappingsFromESRI("Cassini").front()->params,
                         kCassiniParams);

  constexpr ExpectedParamSpec kGeosParams[] = {
      {"False_Easting", EPSG_NAME_PARAMETER_FALSE_EASTING,
       EPSG_CODE_PARAMETER_FALSE_EASTING, "0.0", false},
      {"False_Northing", EPSG_NAME_PARAMETER_FALSE_NORTHING,
       EPSG_CODE_PARAMETER_FALSE_NORTHING, "0.0", false},
      {"Longitude_Of_Center", EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN, "0.0", false},
      {"Height", "Satellite Height", 0, "0.0", false},
      {"Option", nullptr, 0, "0.0", false},
  };
  ExpectParamListMatches(
      getMappingsFromESRI("Geostationary_Satellite").front()->params,
      kGeosParams);

  constexpr ExpectedParamSpec kMercAuxSphereParams[] = {
      {"False_Easting", EPSG_NAME_PARAMETER_FALSE_EASTING,
       EPSG_CODE_PARAMETER_FALSE_EASTING, "0.0", false},
      {"False_Northing", EPSG_NAME_PARAMETER_FALSE_NORTHING,
       EPSG_CODE_PARAMETER_FALSE_NORTHING, "0.0", false},
      {"Central_Meridian", EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN, "0.0", false},
      {"Standard_Parallel_1", EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, "0.0", false},
      {"Auxiliary_Sphere_Type", nullptr, 0, "0.0", false},
  };
  ExpectParamListMatches(
      getMappingsFromESRI("Mercator_Auxiliary_Sphere").front()->params,
      kMercAuxSphereParams);

  constexpr ExpectedParamSpec kIgacParams[] = {
      {"False_Easting", EPSG_NAME_PARAMETER_FALSE_EASTING,
       EPSG_CODE_PARAMETER_FALSE_EASTING, "0.0", false},
      {"False_Northing", EPSG_NAME_PARAMETER_FALSE_NORTHING,
       EPSG_CODE_PARAMETER_FALSE_NORTHING, "0.0", false},
      {"Longitude_Of_Center", EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN, "0.0", false},
      {"Latitude_Of_Center", EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
       EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, "0.0", false},
      {"Height", EPSG_NAME_PARAMETER_PROJECTION_PLANE_ORIGIN_HEIGHT,
       EPSG_CODE_PARAMETER_PROJECTION_PLANE_ORIGIN_HEIGHT, "0.0", false},
  };
  ExpectParamListMatches(
      getMappingsFromESRI("IGAC_Plano_Cartesiano").front()->params,
      kIgacParams);
}

TEST(EsriParamMappingsTest, EndToEndWkt1EsriImportExportDisambiguatesVariants) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  struct WktRoundTripCase {
    std::string_view label;
    const char* wkt;
    std::string_view expected_method_name;
    const char* expected_method_code;
    int expected_param_count;
    std::string_view expected_esri_proj_substr;
    std::string_view expected_esri_param_substr;
  };

  constexpr WktRoundTripCase kCases[] = {
      // 1. Behrmann omits Central_Meridian and Standard_Parallel_1 to verify
      // is_fixed_value populates 0.0 and 30.0 degrees.
      {
          .label = "BehrmannFixedDefaults",
          .wkt = "PROJCS[\"Custom_Behrmann\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Behrmann\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name =
              EPSG_NAME_METHOD_LAMBERT_CYLINDRICAL_EQUAL_AREA,
          .expected_method_code = "9835",
          .expected_param_count = 4,
          .expected_esri_proj_substr = "PROJECTION[\"Behrmann\"]",
          .expected_esri_param_substr =
              "PARAMETER[\"Standard_Parallel_1\",30.0]",
      },
      // 2. Plate_Carree on ellipsoid vs sphere vs Equidistant_Cylindrical.
      {
          .label = "PlateCarreeEllipsoidal",
          .wkt = "PROJCS[\"Custom WGS84 Plate Carree\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Plate_Carree\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",0.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name = EPSG_NAME_METHOD_EQUIDISTANT_CYLINDRICAL,
          .expected_method_code = "1028",
          .expected_param_count = 4,
          .expected_esri_proj_substr = "PROJECTION[\"Plate_Carree\"]",
          .expected_esri_param_substr = "PARAMETER[\"Central_Meridian\",0.0]",
      },
      {
          .label = "PlateCarreeSpherical",
          .wkt = "PROJCS[\"Custom Sphere Plate Carree\","
                 "GEOGCS[\"GCS_unknown\","
                 "DATUM[\"D_unknown\",SPHEROID[\"Sphere\",6371000.0,0.0]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Plate_Carree\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",0.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name =
              EPSG_NAME_METHOD_EQUIDISTANT_CYLINDRICAL_SPHERICAL,
          .expected_method_code = "1029",
          .expected_param_count = 3,
          .expected_esri_proj_substr = "PROJECTION[\"Plate_Carree\"]",
          .expected_esri_param_substr = "PARAMETER[\"Central_Meridian\",0.0]",
      },
      {
          .label = "EquidistantCylindricalWithStdParallel",
          .wkt = "PROJCS[\"Custom_Equidistant_Cylindrical\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Equidistant_Cylindrical\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",0.0],"
                 "PARAMETER[\"Standard_Parallel_1\",30.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name = EPSG_NAME_METHOD_EQUIDISTANT_CYLINDRICAL,
          .expected_method_code = "1028",
          .expected_param_count = 4,
          .expected_esri_proj_substr =
              "PROJECTION[\"Equidistant_Cylindrical\"]",
          .expected_esri_param_substr =
              "PARAMETER[\"Standard_Parallel_1\",30.0]",
      },
      // 3. Transverse_Mercator vs Gauss_Kruger.
      {
          .label = "TransverseMercator",
          .wkt = "PROJCS[\"Custom_Transverse_Mercator\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Transverse_Mercator\"],"
                 "PARAMETER[\"False_Easting\",500000.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",11.0],"
                 "PARAMETER[\"Scale_Factor\",0.9996],"
                 "PARAMETER[\"Latitude_Of_Origin\",0.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name = EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
          .expected_method_code = "9807",
          .expected_param_count = 5,
          .expected_esri_proj_substr = "PROJECTION[\"Transverse_Mercator\"]",
          .expected_esri_param_substr = "PARAMETER[\"Central_Meridian\",11.0]",
      },
      {
          .label = "GaussKruger",
          .wkt = "PROJCS[\"Custom_Gauss_Kruger\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Gauss_Kruger\"],"
                 "PARAMETER[\"False_Easting\",3500000.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",9.0],"
                 "PARAMETER[\"Scale_Factor\",1.0],"
                 "PARAMETER[\"Latitude_Of_Origin\",0.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name = EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
          .expected_method_code = "9807",
          .expected_param_count = 5,
          .expected_esri_proj_substr = "PROJECTION[\"Gauss_Kruger\"]",
          .expected_esri_param_substr =
              "PARAMETER[\"False_Easting\",3500000.0]",
      },
      // 4. Lambert_Conformal_Conic 1SP, 2SP, 2SP with Scale_Factor=1.0, and
      // 2SP Michigan (Scale_Factor != 1.0).
      {
          .label = "Lambert1SP",
          .wkt = "PROJCS[\"Custom_LCC_1SP\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Lambert_Conformal_Conic\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",-90.0],"
                 "PARAMETER[\"Standard_Parallel_1\",45.0],"
                 "PARAMETER[\"Scale_Factor\",0.998046875],"
                 "PARAMETER[\"Latitude_Of_Origin\",45.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name = EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_1SP,
          .expected_method_code = "9801",
          .expected_param_count = 5,
          .expected_esri_proj_substr =
              "PROJECTION[\"Lambert_Conformal_Conic\"]",
          .expected_esri_param_substr =
              "PARAMETER[\"Scale_Factor\",0.998046875]",
      },
      {
          .label = "Lambert2SP",
          .wkt = "PROJCS[\"Custom_LCC_2SP\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Lambert_Conformal_Conic\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",-96.0],"
                 "PARAMETER[\"Standard_Parallel_1\",33.0],"
                 "PARAMETER[\"Standard_Parallel_2\",45.0],"
                 "PARAMETER[\"Latitude_Of_Origin\",39.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name = EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_2SP,
          .expected_method_code = "9802",
          .expected_param_count = 6,
          .expected_esri_proj_substr =
              "PROJECTION[\"Lambert_Conformal_Conic\"]",
          .expected_esri_param_substr =
              "PARAMETER[\"Standard_Parallel_2\",45.0]",
      },
      {
          .label = "Lambert2SPUnitScaleIgnored",
          .wkt = "PROJCS[\"Custom_LCC_2SP_UnitScale\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Lambert_Conformal_Conic\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",-96.0],"
                 "PARAMETER[\"Standard_Parallel_1\",33.0],"
                 "PARAMETER[\"Standard_Parallel_2\",45.0],"
                 "PARAMETER[\"Scale_Factor\",1.0],"
                 "PARAMETER[\"Latitude_Of_Origin\",39.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name = EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_2SP,
          .expected_method_code = "9802",
          .expected_param_count = 6,
          .expected_esri_proj_substr =
              "PROJECTION[\"Lambert_Conformal_Conic\"]",
          .expected_esri_param_substr =
              "PARAMETER[\"Standard_Parallel_2\",45.0]",
      },
      {
          .label = "Lambert2SPMichigan",
          .wkt = "PROJCS[\"Custom_LCC_2SP_Michigan\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Lambert_Conformal_Conic\"],"
                 "PARAMETER[\"False_Easting\",4000000.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",-84.375],"
                 "PARAMETER[\"Standard_Parallel_1\",42.125],"
                 "PARAMETER[\"Standard_Parallel_2\",43.625],"
                 "PARAMETER[\"Scale_Factor\",1.0078125],"
                 "PARAMETER[\"Latitude_Of_Origin\",41.5],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name =
              EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_2SP_MICHIGAN,
          .expected_method_code = "1051",
          .expected_param_count = 7,
          .expected_esri_proj_substr =
              "PROJECTION[\"Lambert_Conformal_Conic\"]",
          .expected_esri_param_substr = "PARAMETER[\"Scale_Factor\",1.0078125]",
      },
      // 5. Krovak standard vs North Orientated (via X_Scale=-1 / Rot=90 and
      // via _East_North CRS name suffix).
      {
          .label = "KrovakStandard",
          .wkt = "PROJCS[\"Custom_Krovak\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Krovak\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Pseudo_Standard_Parallel_1\",78.5],"
                 "PARAMETER[\"Scale_Factor\",0.9999],"
                 "PARAMETER[\"Azimuth\",30.25],"
                 "PARAMETER[\"Longitude_Of_Center\",24.875],"
                 "PARAMETER[\"Latitude_Of_Center\",49.5],"
                 "PARAMETER[\"X_Scale\",1.0],"
                 "PARAMETER[\"Y_Scale\",1.0],"
                 "PARAMETER[\"XY_Plane_Rotation\",0.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name = EPSG_NAME_METHOD_KROVAK,
          .expected_method_code = "9819",
          .expected_param_count = 7,
          .expected_esri_proj_substr = "PROJECTION[\"Krovak\"]",
          .expected_esri_param_substr = "PARAMETER[\"X_Scale\",1.0]",
      },
      {
          .label = "KrovakNorthOrientatedByParams",
          .wkt = "PROJCS[\"Custom_Krovak_North\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Krovak\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Pseudo_Standard_Parallel_1\",78.5],"
                 "PARAMETER[\"Scale_Factor\",0.9999],"
                 "PARAMETER[\"Azimuth\",30.25],"
                 "PARAMETER[\"Longitude_Of_Center\",24.875],"
                 "PARAMETER[\"Latitude_Of_Center\",49.5],"
                 "PARAMETER[\"X_Scale\",-1.0],"
                 "PARAMETER[\"Y_Scale\",1.0],"
                 "PARAMETER[\"XY_Plane_Rotation\",90.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name = EPSG_NAME_METHOD_KROVAK_NORTH_ORIENTED,
          .expected_method_code = "1041",
          .expected_param_count = 7,
          .expected_esri_proj_substr = "PROJECTION[\"Krovak\"]",
          .expected_esri_param_substr = "PARAMETER[\"X_Scale\",-1.0]",
      },
      {
          .label = "KrovakNorthOrientatedByName",
          .wkt = "PROJCS[\"Custom_Krovak_East_North\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Krovak\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Pseudo_Standard_Parallel_1\",78.5],"
                 "PARAMETER[\"Scale_Factor\",0.9999],"
                 "PARAMETER[\"Azimuth\",30.25],"
                 "PARAMETER[\"Longitude_Of_Center\",24.875],"
                 "PARAMETER[\"Latitude_Of_Center\",49.5],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name = EPSG_NAME_METHOD_KROVAK_NORTH_ORIENTED,
          .expected_method_code = "1041",
          .expected_param_count = 7,
          .expected_esri_proj_substr = "PROJECTION[\"Krovak\"]",
          .expected_esri_param_substr = "PARAMETER[\"XY_Plane_Rotation\",90.0]",
      },
      // 6. Goode_Homolosine Option 0.0 / 1.0 / 2.0.
      {
          .label = "GoodeOption0",
          .wkt = "PROJCS[\"Custom_Goode_Homolosine\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Goode_Homolosine\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",0.0],"
                 "PARAMETER[\"Option\",0.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name = PROJ_WKT2_NAME_METHOD_GOODE_HOMOLOSINE,
          .expected_method_code = nullptr,
          .expected_param_count = 3,
          .expected_esri_proj_substr = "PROJECTION[\"Goode_Homolosine\"]",
          .expected_esri_param_substr = "PARAMETER[\"Option\",0.0]",
      },
      {
          .label = "GoodeOption1",
          .wkt = "PROJCS[\"Custom_Goode_Homolosine_Land\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Goode_Homolosine\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",0.0],"
                 "PARAMETER[\"Option\",1.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name =
              PROJ_WKT2_NAME_METHOD_INTERRUPTED_GOODE_HOMOLOSINE,
          .expected_method_code = nullptr,
          .expected_param_count = 3,
          .expected_esri_proj_substr = "PROJECTION[\"Goode_Homolosine\"]",
          .expected_esri_param_substr = "PARAMETER[\"Option\",1.0]",
      },
      {
          .label = "GoodeOption2",
          .wkt = "PROJCS[\"Custom_Goode_Homolosine_Ocean\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Goode_Homolosine\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",0.0],"
                 "PARAMETER[\"Option\",2.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name =
              PROJ_WKT2_NAME_METHOD_INTERRUPTED_GOODE_HOMOLOSINE_OCEAN,
          .expected_method_code = nullptr,
          .expected_param_count = 3,
          .expected_esri_proj_substr = "PROJECTION[\"Goode_Homolosine\"]",
          .expected_esri_param_substr = "PARAMETER[\"Option\",2.0]",
      },
      // 7. Peirce_Quincuncial Option 0.0 (Square) vs 1.0 (Diamond).
      {
          .label = "PeirceSquare",
          .wkt = "PROJCS[\"Custom_Peirce_Quincuncial_Square\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Peirce_Quincuncial\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",20.0],"
                 "PARAMETER[\"Scale_Factor\",1.0],"
                 "PARAMETER[\"Latitude_Of_Origin\",0.0],"
                 "PARAMETER[\"Option\",0.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name =
              PROJ_WKT2_NAME_METHOD_PEIRCE_QUINCUNCIAL_SQUARE,
          .expected_method_code = nullptr,
          .expected_param_count = 5,
          .expected_esri_proj_substr = "PROJECTION[\"Peirce_Quincuncial\"]",
          .expected_esri_param_substr = "PARAMETER[\"Option\",0.0]",
      },
      {
          .label = "PeirceDiamond",
          .wkt = "PROJCS[\"Custom_Peirce_Quincuncial_Diamond\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Peirce_Quincuncial\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Central_Meridian\",20.0],"
                 "PARAMETER[\"Scale_Factor\",1.0],"
                 "PARAMETER[\"Latitude_Of_Origin\",0.0],"
                 "PARAMETER[\"Option\",1.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name =
              PROJ_WKT2_NAME_METHOD_PEIRCE_QUINCUNCIAL_DIAMOND,
          .expected_method_code = nullptr,
          .expected_param_count = 5,
          .expected_esri_proj_substr = "PROJECTION[\"Peirce_Quincuncial\"]",
          .expected_esri_param_substr = "PARAMETER[\"Option\",1.0]",
      },
      // 8. Hotine Oblique Mercator vs Rectified Skew Orthomorphic (Natural
      // Origin and Center variants).
      {
          .label = "HotineObliqueMercatorNaturalOrigin",
          .wkt =
              "PROJCS[\"Custom_HOM_NatOrigin\","
              "GEOGCS[\"GCS_WGS_1984\","
              "DATUM[\"D_WGS_1984\","
              "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
              "PRIMEM[\"Greenwich\",0.0],"
              "UNIT[\"Degree\",0.0174532925199433]],"
              "PROJECTION[\"Hotine_Oblique_Mercator_Azimuth_Natural_Origin\"],"
              "PARAMETER[\"False_Easting\",0.0],"
              "PARAMETER[\"False_Northing\",0.0],"
              "PARAMETER[\"Scale_Factor\",0.9996],"
              "PARAMETER[\"Azimuth\",45.25],"
              "PARAMETER[\"Longitude_Of_Center\",-100.0],"
              "PARAMETER[\"Latitude_Of_Center\",40.0],"
              "UNIT[\"Meter\",1.0]]",
          .expected_method_name =
              EPSG_NAME_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_A,
          .expected_method_code = "9812",
          .expected_param_count = 7,
          .expected_esri_proj_substr =
              "PROJECTION[\"Hotine_Oblique_Mercator_Azimuth_Natural_Origin\"]",
          .expected_esri_param_substr = "PARAMETER[\"Azimuth\",45.25]",
      },
      {
          .label = "RectifiedSkewOrthomorphicNaturalOrigin",
          .wkt = "PROJCS[\"Custom_RSO_NatOrigin\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Rectified_Skew_Orthomorphic_Natural_Origin\"],"
                 "PARAMETER[\"False_Easting\",0.0],"
                 "PARAMETER[\"False_Northing\",0.0],"
                 "PARAMETER[\"Scale_Factor\",0.99984],"
                 "PARAMETER[\"Azimuth\",45.25],"
                 "PARAMETER[\"Longitude_Of_Center\",102.25],"
                 "PARAMETER[\"Latitude_Of_Center\",4.0],"
                 "PARAMETER[\"XY_Plane_Rotation\",53.125],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name =
              EPSG_NAME_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_A,
          .expected_method_code = "9812",
          .expected_param_count = 7,
          .expected_esri_proj_substr =
              "PROJECTION[\"Rectified_Skew_Orthomorphic_Natural_Origin\"]",
          .expected_esri_param_substr =
              "PARAMETER[\"XY_Plane_Rotation\",53.125]",
      },
      {
          .label = "HotineObliqueMercatorCenter",
          .wkt = "PROJCS[\"Custom_HOM_Center\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Hotine_Oblique_Mercator_Azimuth_Center\"],"
                 "PARAMETER[\"False_Easting\",500000.0],"
                 "PARAMETER[\"False_Northing\",1000000.0],"
                 "PARAMETER[\"Scale_Factor\",0.9996],"
                 "PARAMETER[\"Azimuth\",45.25],"
                 "PARAMETER[\"Longitude_Of_Center\",-100.0],"
                 "PARAMETER[\"Latitude_Of_Center\",40.0],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name =
              EPSG_NAME_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_B,
          .expected_method_code = "9815",
          .expected_param_count = 7,
          .expected_esri_proj_substr =
              "PROJECTION[\"Hotine_Oblique_Mercator_Azimuth_Center\"]",
          .expected_esri_param_substr = "PARAMETER[\"Azimuth\",45.25]",
      },
      {
          .label = "RectifiedSkewOrthomorphicCenter",
          .wkt = "PROJCS[\"Custom_RSO_Center\","
                 "GEOGCS[\"GCS_WGS_1984\","
                 "DATUM[\"D_WGS_1984\","
                 "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
                 "PRIMEM[\"Greenwich\",0.0],"
                 "UNIT[\"Degree\",0.0174532925199433]],"
                 "PROJECTION[\"Rectified_Skew_Orthomorphic_Center\"],"
                 "PARAMETER[\"False_Easting\",500000.0],"
                 "PARAMETER[\"False_Northing\",1000000.0],"
                 "PARAMETER[\"Scale_Factor\",0.99984],"
                 "PARAMETER[\"Azimuth\",45.25],"
                 "PARAMETER[\"Longitude_Of_Center\",102.25],"
                 "PARAMETER[\"Latitude_Of_Center\",4.0],"
                 "PARAMETER[\"XY_Plane_Rotation\",53.125],"
                 "UNIT[\"Meter\",1.0]]",
          .expected_method_name =
              EPSG_NAME_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_B,
          .expected_method_code = "9815",
          .expected_param_count = 7,
          .expected_esri_proj_substr =
              "PROJECTION[\"Rectified_Skew_Orthomorphic_Center\"]",
          .expected_esri_param_substr =
              "PARAMETER[\"XY_Plane_Rotation\",53.125]",
      },
  };

  for (const auto& tc : kCases) {
    PROJ_STRING_LIST warnings_raw = nullptr;
    PROJ_STRING_LIST errors_raw = nullptr;
    ScopedPj crs(proj_create_from_wkt(ctx.get(), tc.wkt, nullptr, &warnings_raw,
                                      &errors_raw));
    ScopedStringList warnings(warnings_raw);
    ScopedStringList errors(errors_raw);
    ASSERT_THAT(crs.get(), NotNull()) << tc.label;
    EXPECT_EQ(nullptr, errors.get()) << tc.label;

    ScopedPj op(proj_crs_get_coordoperation(ctx.get(), crs.get()));
    ASSERT_THAT(op.get(), NotNull()) << tc.label;

    const char* method_name = nullptr;
    const char* method_auth = nullptr;
    const char* method_code = nullptr;
    ASSERT_EQ(
        1, proj_coordoperation_get_method_info(
               ctx.get(), op.get(), &method_name, &method_auth, &method_code))
        << tc.label;
    ASSERT_THAT(method_name, NotNull()) << tc.label;
    EXPECT_EQ(tc.expected_method_name, method_name) << tc.label;
    if (tc.expected_method_code == nullptr) {
      EXPECT_EQ(nullptr, method_code) << tc.label;
    } else {
      ASSERT_THAT(method_code, NotNull()) << tc.label;
      EXPECT_STREQ(tc.expected_method_code, method_code) << tc.label;
      ASSERT_THAT(method_auth, NotNull()) << tc.label;
      EXPECT_STREQ("EPSG", method_auth) << tc.label;
    }

    EXPECT_EQ(tc.expected_param_count,
              proj_coordoperation_get_param_count(ctx.get(), op.get()))
        << tc.label;

    const char* exported_ptr =
        proj_as_wkt(ctx.get(), crs.get(), PJ_WKT1_ESRI, nullptr);
    ASSERT_THAT(exported_ptr, NotNull()) << tc.label;
    const std::string exported_wkt(exported_ptr);
    EXPECT_THAT(exported_wkt, HasSubstr(tc.expected_esri_proj_substr))
        << tc.label;
    EXPECT_THAT(exported_wkt, HasSubstr(tc.expected_esri_param_substr))
        << tc.label;
  }
}

TEST(EsriParamMappingsTest, BehrmannFixedParallelProducesWorkingTransform) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // 1. Parse Behrmann ESRI WKT1 with omitted Standard_Parallel_1 and
  // Central_Meridian so is_fixed_value supplies 30.0 deg and 0.0 deg.
  constexpr char kBehrmannWkt[] =
      "PROJCS[\"Custom_Behrmann\","
      "GEOGCS[\"GCS_WGS_1984\","
      "DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
      "PRIMEM[\"Greenwich\",0.0],"
      "UNIT[\"Degree\",0.0174532925199433]],"
      "PROJECTION[\"Behrmann\"],"
      "PARAMETER[\"False_Easting\",0.0],"
      "PARAMETER[\"False_Northing\",0.0],"
      "UNIT[\"Meter\",1.0]]";

  ScopedPj proj_crs(
      proj_create_from_wkt(ctx.get(), kBehrmannWkt, nullptr, nullptr, nullptr));
  ASSERT_THAT(proj_crs.get(), NotNull());

  ScopedPj op(proj_crs_get_coordoperation(ctx.get(), proj_crs.get()));
  ASSERT_THAT(op.get(), NotNull());
  const int central_meridian_idx = proj_coordoperation_get_param_index(
      ctx.get(), op.get(), EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN);
  ASSERT_GE(central_meridian_idx, 0);
  double central_meridian_val = -1.0;
  ASSERT_EQ(1, proj_coordoperation_get_param(
                   ctx.get(), op.get(), central_meridian_idx, nullptr, nullptr,
                   nullptr, &central_meridian_val, nullptr, nullptr, nullptr,
                   nullptr, nullptr, nullptr));
  EXPECT_DOUBLE_EQ(0.0, central_meridian_val);

  const int std_parallel_idx = proj_coordoperation_get_param_index(
      ctx.get(), op.get(), EPSG_NAME_PARAMETER_LATITUDE_1ST_STD_PARALLEL);
  ASSERT_GE(std_parallel_idx, 0);
  double std_parallel_val = 0.0;
  ASSERT_EQ(1, proj_coordoperation_get_param(
                   ctx.get(), op.get(), std_parallel_idx, nullptr, nullptr,
                   nullptr, &std_parallel_val, nullptr, nullptr, nullptr,
                   nullptr, nullptr, nullptr));
  EXPECT_DOUBLE_EQ(30.0, std_parallel_val);

  // 2. Build a geographic->Behrmann transform normalized for (lon, lat)
  // degrees and verify forward/inverse round-trip reflects cos(30 deg) scaling.
  ScopedPj base_crs(proj_crs_get_geodetic_crs(ctx.get(), proj_crs.get()));
  ASSERT_THAT(base_crs.get(), NotNull());
  ScopedPj raw_xform(proj_create_crs_to_crs_from_pj(
      ctx.get(), base_crs.get(), proj_crs.get(), nullptr, nullptr));
  ASSERT_THAT(raw_xform.get(), NotNull());
  ScopedPj xform(proj_normalize_for_visualization(ctx.get(), raw_xform.get()));
  ASSERT_THAT(xform.get(), NotNull());

  const PJ_COORD fwd =
      proj_trans(xform.get(), PJ_FWD, proj_coord(10.0, 30.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(xform.get()));
  EXPECT_NEAR(964862.802509, fwd.xy.x, 1e-3);
  EXPECT_NEAR(3658789.324855, fwd.xy.y, 1e-3);

  const PJ_COORD inv = proj_trans(xform.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(xform.get()));
  EXPECT_NEAR(10.0, inv.xy.x, 1e-9);
  EXPECT_NEAR(30.0, inv.xy.y, 1e-9);
}

}  // namespace
}  // namespace proj
