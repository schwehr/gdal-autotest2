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
// Unit tests for ISO-19111 method/parameter mapping tables and lookup helpers
// (proj/src/iso19111/operation/parammappings.cpp). Note: requires
// -DFROM_PROJ_CPP, -fexceptions, and static linkage (linkstatic = True) against
// :proj4_unbundled because parammappings.hpp exposes non-PROJ_DLL internal
// symbols and ISO-19111 constructors/formatters can throw
// ::osgeo::proj::util::Exception subclasses.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/operation/parammappings.cpp

#include "third_party/proj4/proj/src/iso19111/operation/parammappings.hpp"

#include <cstddef>
#include <list>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/coordinateoperation.hpp"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/metadata.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_constants.h"

namespace proj {
namespace {

namespace common = ::osgeo::proj::common;
namespace io = ::osgeo::proj::io;
namespace metadata = ::osgeo::proj::metadata;
namespace op = ::osgeo::proj::operation;
namespace util = ::osgeo::proj::util;

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::NotNull;

op::OperationMethodNNPtr MakeMethod(std::string_view name, int epsg_code = 0) {
  util::PropertyMap props;
  props.set(common::IdentifiedObject::NAME_KEY, std::string(name));
  if (epsg_code != 0) {
    props.set(metadata::Identifier::CODESPACE_KEY, metadata::Identifier::EPSG);
    props.set(metadata::Identifier::CODE_KEY, epsg_code);
  }
  return op::OperationMethod::create(
      props, std::vector<op::OperationParameterNNPtr>{});
}

op::OperationParameterNNPtr MakeParam(std::string_view name,
                                      int epsg_code = 0) {
  util::PropertyMap props;
  props.set(common::IdentifiedObject::NAME_KEY, std::string(name));
  if (epsg_code != 0) {
    props.set(metadata::Identifier::CODESPACE_KEY, metadata::Identifier::EPSG);
    props.set(metadata::Identifier::CODE_KEY, epsg_code);
  }
  return op::OperationParameter::create(props);
}

TEST(ParamMappingsTest, ExportedConstantsAndTableInvariants) {
  // 1. Verify exported WKT1 parameter name constants and
  // paramLatitudeNatOrigin.
  EXPECT_STREQ("latitude_of_origin", op::WKT1_LATITUDE_OF_ORIGIN);
  EXPECT_STREQ("central_meridian", op::WKT1_CENTRAL_MERIDIAN);
  EXPECT_STREQ("scale_factor", op::WKT1_SCALE_FACTOR);
  EXPECT_STREQ("false_easting", op::WKT1_FALSE_EASTING);
  EXPECT_STREQ("false_northing", op::WKT1_FALSE_NORTHING);
  EXPECT_STREQ("standard_parallel_1", op::WKT1_STANDARD_PARALLEL_1);
  EXPECT_STREQ("standard_parallel_2", op::WKT1_STANDARD_PARALLEL_2);
  EXPECT_STREQ("latitude_of_center", op::WKT1_LATITUDE_OF_CENTER);
  EXPECT_STREQ("longitude_of_center", op::WKT1_LONGITUDE_OF_CENTER);
  EXPECT_STREQ("azimuth", op::WKT1_AZIMUTH);
  EXPECT_STREQ("rectified_grid_angle", op::WKT1_RECTIFIED_GRID_ANGLE);

  EXPECT_STREQ(EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
               op::paramLatitudeNatOrigin.wkt2_name);
  EXPECT_EQ(EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
            op::paramLatitudeNatOrigin.epsg_code);
  EXPECT_EQ(op::WKT1_LATITUDE_OF_ORIGIN, op::paramLatitudeNatOrigin.wkt1_name);
  EXPECT_EQ(common::UnitOfMeasure::Type::ANGULAR,
            op::paramLatitudeNatOrigin.unit_type);
  EXPECT_STREQ("lat_0", op::paramLatitudeNatOrigin.proj_name);

  // 2. Validate structural invariants of gProjectionMethodMappings.
  std::size_t proj_count = 0;
  const op::MethodMapping* proj_mappings =
      op::getProjectionMethodMappings(proj_count);
  ASSERT_THAT(proj_mappings, NotNull());
  EXPECT_GT(proj_count, 50u);

  bool saw_null_wkt1 = false;
  bool saw_null_proj = false;
  for (std::size_t i = 0; i < proj_count; ++i) {
    const auto& entry = proj_mappings[i];
    ASSERT_THAT(entry.wkt2_name, NotNull()) << "index " << i;
    EXPECT_FALSE(std::string_view(entry.wkt2_name).empty()) << "index " << i;
    EXPECT_GE(entry.epsg_code, 0) << "index " << i;
    if (entry.wkt1_name == nullptr) {
      saw_null_wkt1 = true;
    }
    if (entry.proj_name_main == nullptr) {
      saw_null_proj = true;
    }
    ASSERT_THAT(entry.params, NotNull()) << "index " << i;
    for (std::size_t j = 0; entry.params[j] != nullptr; ++j) {
      ASSERT_LT(j, 32u) << "Missing nullptr sentinel at projection index " << i;
      ASSERT_THAT(entry.params[j]->wkt2_name, NotNull());
      EXPECT_FALSE(std::string_view(entry.params[j]->wkt2_name).empty());
      EXPECT_GE(entry.params[j]->epsg_code, 0);
    }
  }
  EXPECT_TRUE(saw_null_wkt1);
  EXPECT_TRUE(saw_null_proj);

  // 3. Validate gOtherMethodMappings contains both parameterless and
  // parameterized entries.
  std::size_t other_count = 0;
  const op::MethodMapping* other_mappings =
      op::getOtherMethodMappings(other_count);
  ASSERT_THAT(other_mappings, NotNull());
  EXPECT_GT(other_count, 30u);

  bool saw_null_params = false;
  bool saw_non_null_params = false;
  for (std::size_t i = 0; i < other_count; ++i) {
    const auto& entry = other_mappings[i];
    ASSERT_THAT(entry.wkt2_name, NotNull()) << "other index " << i;
    EXPECT_FALSE(std::string_view(entry.wkt2_name).empty())
        << "other index " << i;
    EXPECT_GE(entry.epsg_code, 0) << "other index " << i;
    if (entry.params == nullptr) {
      saw_null_params = true;
    } else {
      saw_non_null_params = true;
      for (std::size_t j = 0; entry.params[j] != nullptr; ++j) {
        ASSERT_LT(j, 32u) << "Missing nullptr sentinel at other index " << i;
        ASSERT_THAT(entry.params[j]->wkt2_name, NotNull());
        EXPECT_FALSE(std::string_view(entry.params[j]->wkt2_name).empty());
        EXPECT_GE(entry.params[j]->epsg_code, 0);
      }
    }
  }
  EXPECT_TRUE(saw_null_params);
  EXPECT_TRUE(saw_non_null_params);

  // 4. Validate getMethodNameCodes and getParamNameCodes lookup tables.
  std::size_t method_code_count = 0;
  const op::MethodNameCode* method_codes =
      op::getMethodNameCodes(method_code_count);
  ASSERT_THAT(method_codes, NotNull());
  EXPECT_GT(method_code_count, 60u);
  for (std::size_t i = 0; i < method_code_count; ++i) {
    ASSERT_THAT(method_codes[i].name, NotNull()) << "index " << i;
    EXPECT_FALSE(std::string_view(method_codes[i].name).empty());
    EXPECT_GT(method_codes[i].epsg_code, 0);
  }

  std::size_t param_code_count = 0;
  const op::ParamNameCode* param_codes =
      op::getParamNameCodes(param_code_count);
  ASSERT_THAT(param_codes, NotNull());
  EXPECT_GT(param_code_count, 60u);
  for (std::size_t i = 0; i < param_code_count; ++i) {
    ASSERT_THAT(param_codes[i].name, NotNull()) << "index " << i;
    EXPECT_FALSE(std::string_view(param_codes[i].name).empty());
    EXPECT_GT(param_codes[i].epsg_code, 0);
  }
}

TEST(ParamMappingsTest, GetMappingByEpsgCodeHandlesHitsZeroAndMisses) {
  struct HitCase {
    int epsg_code;
    std::string_view expected_wkt2;
    const char* expected_wkt1;
    const char* expected_proj_main;
    const char* expected_proj_aux;
  };
  constexpr HitCase kHits[] = {
      {
          .epsg_code = EPSG_CODE_METHOD_TRANSVERSE_MERCATOR,
          .expected_wkt2 = EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
          .expected_wkt1 = "Transverse_Mercator",
          .expected_proj_main = "tmerc",
          .expected_proj_aux = nullptr,
      },
      {
          .epsg_code = EPSG_CODE_METHOD_TRANSVERSE_MERCATOR_SOUTH_ORIENTATED,
          .expected_wkt2 =
              EPSG_NAME_METHOD_TRANSVERSE_MERCATOR_SOUTH_ORIENTATED,
          .expected_wkt1 = "Transverse_Mercator_South_Orientated",
          .expected_proj_main = "tmerc",
          .expected_proj_aux = "axis=wsu",
      },
      {
          // Returns the first Albers Equal Area entry (standard GDAL WKT1
          // name).
          .epsg_code = EPSG_CODE_METHOD_ALBERS_EQUAL_AREA,
          .expected_wkt2 = EPSG_NAME_METHOD_ALBERS_EQUAL_AREA,
          .expected_wkt1 = "Albers_Conic_Equal_Area",
          .expected_proj_main = "aea",
          .expected_proj_aux = nullptr,
      },
      {
          .epsg_code = EPSG_CODE_METHOD_EQUAL_EARTH,
          .expected_wkt2 = EPSG_NAME_METHOD_EQUAL_EARTH,
          .expected_wkt1 = nullptr,
          .expected_proj_main = "eqearth",
          .expected_proj_aux = nullptr,
      },
      {
          // Last entry in gProjectionMethodMappings verifies full table scan.
          .epsg_code = EPSG_CODE_METHOD_TRANSVERSE_MERCATOR_ZONED_GRID_SYSTEM,
          .expected_wkt2 =
              EPSG_NAME_METHOD_TRANSVERSE_MERCATOR_ZONED_GRID_SYSTEM,
          .expected_wkt1 = nullptr,
          .expected_proj_main = "tmerczoned",
          .expected_proj_aux = nullptr,
      },
      {
          // epsg_code == 0 matches the first projection entry without an EPSG
          // code.
          .epsg_code = 0,
          .expected_wkt2 = PROJ_WKT2_NAME_METHOD_TWO_POINT_EQUIDISTANT,
          .expected_wkt1 = "Two_Point_Equidistant",
          .expected_proj_main = "tpeqd",
          .expected_proj_aux = nullptr,
      },
  };

  for (const auto& tc : kHits) {
    const op::MethodMapping* mapping = op::getMapping(tc.epsg_code);
    ASSERT_THAT(mapping, NotNull()) << "epsg_code=" << tc.epsg_code;
    EXPECT_EQ(tc.epsg_code, mapping->epsg_code);
    EXPECT_EQ(tc.expected_wkt2, mapping->wkt2_name);
    if (tc.expected_wkt1 == nullptr) {
      EXPECT_EQ(nullptr, mapping->wkt1_name);
    } else {
      EXPECT_STREQ(tc.expected_wkt1, mapping->wkt1_name);
    }
    EXPECT_STREQ(tc.expected_proj_main, mapping->proj_name_main);
    if (tc.expected_proj_aux == nullptr) {
      EXPECT_EQ(nullptr, mapping->proj_name_aux);
    } else {
      EXPECT_STREQ(tc.expected_proj_aux, mapping->proj_name_aux);
    }
  }

  // Negative, unknown, and gOtherMethodMappings-only EPSG codes return nullptr.
  constexpr int kMisses[] = {
      -1,
      999999,
      EPSG_CODE_METHOD_MOLODENSKY,
      EPSG_CODE_METHOD_LONGITUDE_ROTATION,
      EPSG_CODE_METHOD_NTV2,
  };
  for (int code : kMisses) {
    EXPECT_EQ(nullptr, op::getMapping(code)) << "epsg_code=" << code;
  }
}

TEST(ParamMappingsTest,
     GetMappingByOperationMethodResolvesCodeNameAndFallback) {
  // 1. Explicit EPSG code matches even when the method name is non-standard.
  const auto custom_tm =
      MakeMethod("Custom TM Alias", EPSG_CODE_METHOD_TRANSVERSE_MERCATOR);
  const op::MethodMapping* by_explicit_id = op::getMapping(custom_tm.get());
  ASSERT_THAT(by_explicit_id, NotNull());
  EXPECT_EQ(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR, by_explicit_id->epsg_code);
  EXPECT_STREQ(EPSG_NAME_METHOD_TRANSVERSE_MERCATOR, by_explicit_id->wkt2_name);

  // 2. Method without an explicit identifier resolves EPSG code via
  // getMethodNameCodes.
  const auto implicit_lcc2sp = MakeMethod("lambert_conic_conformal_2sp");
  EXPECT_EQ(EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP,
            implicit_lcc2sp->getEPSGCode());
  const op::MethodMapping* by_implicit_id =
      op::getMapping(implicit_lcc2sp.get());
  ASSERT_THAT(by_implicit_id, NotNull());
  EXPECT_EQ(EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP,
            by_implicit_id->epsg_code);

  // 3. Explicit 3D code distinguishes Transverse Mercator (3D) from base 9807.
  const auto tm_3d = MakeMethod(EPSG_NAME_METHOD_TRANSVERSE_MERCATOR_3D,
                                EPSG_CODE_METHOD_TRANSVERSE_MERCATOR_3D);
  const op::MethodMapping* by_3d_id = op::getMapping(tm_3d.get());
  ASSERT_THAT(by_3d_id, NotNull());
  EXPECT_EQ(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR_3D, by_3d_id->epsg_code);

  // 4. Methods absent from methodNameCodesList have epsg_code == 0 and match by
  // name.
  const auto tpeqd = MakeMethod("two_point_equidistant");
  EXPECT_EQ(0, tpeqd->getEPSGCode());
  const op::MethodMapping* tpeqd_mapping = op::getMapping(tpeqd.get());
  ASSERT_THAT(tpeqd_mapping, NotNull());
  EXPECT_STREQ(PROJ_WKT2_NAME_METHOD_TWO_POINT_EQUIDISTANT,
               tpeqd_mapping->wkt2_name);

  // 5. Non-matching nonzero EPSG code misses the id check and falls through ||
  // to name.
  const auto moll_bad_id = MakeMethod(PROJ_WKT2_NAME_METHOD_MOLLWEIDE, 999999);
  EXPECT_EQ(999999, moll_bad_id->getEPSGCode());
  const op::MethodMapping* moll_mapping = op::getMapping(moll_bad_id.get());
  ASSERT_THAT(moll_mapping, NotNull());
  EXPECT_STREQ(PROJ_WKT2_NAME_METHOD_MOLLWEIDE, moll_mapping->wkt2_name);

  // 6. Unknown names, empty name, and non-projection methods return nullptr.
  EXPECT_EQ(nullptr, op::getMapping(MakeMethod("").get()));
  EXPECT_EQ(nullptr, op::getMapping(MakeMethod("Unknown Method").get()));
  EXPECT_EQ(nullptr,
            op::getMapping(MakeMethod("Unknown Method", 999999).get()));
  EXPECT_EQ(nullptr, op::getMapping(MakeMethod(EPSG_NAME_METHOD_MOLODENSKY,
                                               EPSG_CODE_METHOD_MOLODENSKY)
                                        .get()));
}

TEST(ParamMappingsTest,
     GetMappingFromWkt1MethodHandlesUtmPrefixAliasesAndNulls) {
  struct Wkt1Hit {
    std::string_view wkt1_name;
    int expected_epsg;
    std::string_view expected_wkt2;
    std::string_view expected_canonical_wkt1;
  };
  constexpr Wkt1Hit kHits[] = {
      {
          // OGC 12-063r5 C.4.2 "UTM zone ..." prefix special case.
          .wkt1_name = "UTM zone 32N",
          .expected_epsg = EPSG_CODE_METHOD_TRANSVERSE_MERCATOR,
          .expected_wkt2 = EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
          .expected_canonical_wkt1 = "Transverse_Mercator",
      },
      {
          .wkt1_name = "utm zone 1S",
          .expected_epsg = EPSG_CODE_METHOD_TRANSVERSE_MERCATOR,
          .expected_wkt2 = EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
          .expected_canonical_wkt1 = "Transverse_Mercator",
      },
      {
          .wkt1_name = "transverse mercator",
          .expected_epsg = EPSG_CODE_METHOD_TRANSVERSE_MERCATOR,
          .expected_wkt2 = EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
          .expected_canonical_wkt1 = "Transverse_Mercator",
      },
      {
          // Skips LCC 1SP Variant B (which has wkt1_name == nullptr) before LCC
          // 2SP.
          .wkt1_name = "Lambert_Conformal_Conic_2SP",
          .expected_epsg = EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP,
          .expected_wkt2 = EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_2SP,
          .expected_canonical_wkt1 = "Lambert_Conformal_Conic_2SP",
      },
      {
          .wkt1_name = "Two_Point_Equidistant",
          .expected_epsg = 0,
          .expected_wkt2 = PROJ_WKT2_NAME_METHOD_TWO_POINT_EQUIDISTANT,
          .expected_canonical_wkt1 = "Two_Point_Equidistant",
      },
      {
          .wkt1_name = "Laborde_Oblique_Mercator",
          .expected_epsg = EPSG_CODE_METHOD_LABORDE_OBLIQUE_MERCATOR,
          .expected_wkt2 = EPSG_NAME_METHOD_LABORDE_OBLIQUE_MERCATOR,
          .expected_canonical_wkt1 = "Laborde_Oblique_Mercator",
      },
  };

  for (const auto& tc : kHits) {
    const op::MethodMapping* mapping =
        op::getMappingFromWKT1(std::string(tc.wkt1_name));
    ASSERT_THAT(mapping, NotNull()) << tc.wkt1_name;
    EXPECT_EQ(tc.expected_epsg, mapping->epsg_code) << tc.wkt1_name;
    EXPECT_EQ(tc.expected_wkt2, mapping->wkt2_name) << tc.wkt1_name;
    EXPECT_EQ(tc.expected_canonical_wkt1, mapping->wkt1_name) << tc.wkt1_name;
  }

  // Oracle WKT spelling resolves to the second Albers Equal Area entry.
  const op::MethodMapping* albers_gdal =
      op::getMappingFromWKT1("Albers_Conic_Equal_Area");
  const op::MethodMapping* albers_oracle =
      op::getMappingFromWKT1("Albers_Conical_Equal_Area");
  ASSERT_THAT(albers_gdal, NotNull());
  ASSERT_THAT(albers_oracle, NotNull());
  EXPECT_NE(albers_gdal, albers_oracle);
  EXPECT_EQ(EPSG_CODE_METHOD_ALBERS_EQUAL_AREA, albers_oracle->epsg_code);
  EXPECT_STREQ("Albers_Conical_Equal_Area", albers_oracle->wkt1_name);

  // Underscore variant does not match ci_starts_with(..., "UTM zone"); methods
  // without a WKT1 representation or unknown names also return nullptr.
  constexpr std::string_view kMisses[] = {
      "",
      "UTM_zone_32N",
      "Equal_Earth",
      "Molodensky",
      "Nonexistent_WKT1_Method",
  };
  for (std::string_view name : kMisses) {
    EXPECT_EQ(nullptr, op::getMappingFromWKT1(std::string(name))) << name;
  }
}

TEST(ParamMappingsTest, GetMappingByWkt2NameSearchesProjectionAndOtherTables) {
  struct Wkt2Case {
    const char* query;
    int expected_epsg;
    std::string_view expected_wkt2;
    bool expect_params;
  };
  constexpr Wkt2Case kHits[] = {
      {
          .query = "Transverse Mercator",
          .expected_epsg = EPSG_CODE_METHOD_TRANSVERSE_MERCATOR,
          .expected_wkt2 = EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
          .expect_params = true,
      },
      {
          .query = "transverse-mercator",
          .expected_epsg = EPSG_CODE_METHOD_TRANSVERSE_MERCATOR,
          .expected_wkt2 = EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
          .expect_params = true,
      },
      {
          .query = EPSG_NAME_METHOD_EQUAL_EARTH,
          .expected_epsg = EPSG_CODE_METHOD_EQUAL_EARTH,
          .expected_wkt2 = EPSG_NAME_METHOD_EQUAL_EARTH,
          .expect_params = true,
      },
      {
          .query = EPSG_NAME_METHOD_TRANSVERSE_MERCATOR_ZONED_GRID_SYSTEM,
          .expected_epsg =
              EPSG_CODE_METHOD_TRANSVERSE_MERCATOR_ZONED_GRID_SYSTEM,
          .expected_wkt2 =
              EPSG_NAME_METHOD_TRANSVERSE_MERCATOR_ZONED_GRID_SYSTEM,
          .expect_params = true,
      },
      {
          // From gOtherMethodMappings (first Change of Vertical Unit entry).
          .query = EPSG_NAME_METHOD_CHANGE_VERTICAL_UNIT,
          .expected_epsg = EPSG_CODE_METHOD_CHANGE_VERTICAL_UNIT,
          .expected_wkt2 = EPSG_NAME_METHOD_CHANGE_VERTICAL_UNIT,
          .expect_params = true,
      },
      {
          // From gOtherMethodMappings with params == nullptr.
          .query = EPSG_NAME_METHOD_HEIGHT_DEPTH_REVERSAL,
          .expected_epsg = EPSG_CODE_METHOD_HEIGHT_DEPTH_REVERSAL,
          .expected_wkt2 = EPSG_NAME_METHOD_HEIGHT_DEPTH_REVERSAL,
          .expect_params = false,
      },
      {
          .query = "molodensky",
          .expected_epsg = EPSG_CODE_METHOD_MOLODENSKY,
          .expected_wkt2 = EPSG_NAME_METHOD_MOLODENSKY,
          .expect_params = true,
      },
      {
          .query = EPSG_NAME_METHOD_NTV2,
          .expected_epsg = EPSG_CODE_METHOD_NTV2,
          .expected_wkt2 = EPSG_NAME_METHOD_NTV2,
          .expect_params = true,
      },
      {
          .query = PROJ_WKT2_NAME_METHOD_POLE_ROTATION_NETCDF_CF_CONVENTION,
          .expected_epsg = 0,
          .expected_wkt2 =
              PROJ_WKT2_NAME_METHOD_POLE_ROTATION_NETCDF_CF_CONVENTION,
          .expect_params = true,
      },
      {
          // Last entry in gOtherMethodMappings exercises full-table scan.
          .query =
              // NOLINTNEXTLINE(whitespace/line_length)
          EPSG_NAME_METHOD_POINT_MOTION_GEOG3D_DOMAIN_USING_NEU_VELOCITY_GRID_NTV2_VEL,
          .expected_epsg =
              // NOLINTNEXTLINE(whitespace/line_length)
          EPSG_CODE_METHOD_POINT_MOTION_GEOG3D_DOMAIN_USING_NEU_VELOCITY_GRID_NTV2_VEL,
          .expected_wkt2 =
              // NOLINTNEXTLINE(whitespace/line_length)
          EPSG_NAME_METHOD_POINT_MOTION_GEOG3D_DOMAIN_USING_NEU_VELOCITY_GRID_NTV2_VEL,
          .expect_params = true,
      },
  };

  for (const auto& tc : kHits) {
    const op::MethodMapping* mapping = op::getMapping(tc.query);
    ASSERT_THAT(mapping, NotNull()) << tc.query;
    EXPECT_EQ(tc.expected_epsg, mapping->epsg_code) << tc.query;
    EXPECT_EQ(tc.expected_wkt2, mapping->wkt2_name) << tc.query;
    if (tc.expect_params) {
      EXPECT_THAT(mapping->params, NotNull()) << tc.query;
    } else {
      EXPECT_EQ(nullptr, mapping->params) << tc.query;
    }
  }

  // Note: getMapping(const char*) forwards to Identifier::isEquivalentName,
  // which assumes a non-null C string; pass empty and unknown strings here.
  EXPECT_EQ(nullptr, op::getMapping(""));
  EXPECT_EQ(nullptr, op::getMapping("Nonexistent Method Name"));
}

TEST(ParamMappingsTest, GetMappingsFromProjNameHandlesMultipleSingleAndEmpty) {
  const auto tmerc = op::getMappingsFromPROJName("tmerc");
  ASSERT_EQ(3u, tmerc.size());
  EXPECT_EQ(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR, tmerc[0]->epsg_code);
  EXPECT_EQ(nullptr, tmerc[0]->proj_name_aux);
  EXPECT_EQ(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR_3D, tmerc[1]->epsg_code);
  EXPECT_EQ(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR_SOUTH_ORIENTATED,
            tmerc[2]->epsg_code);
  EXPECT_STREQ("axis=wsu", tmerc[2]->proj_name_aux);

  const auto lcc = op::getMappingsFromPROJName("lcc");
  EXPECT_EQ(5u, lcc.size());

  const auto omerc = op::getMappingsFromPROJName("omerc");
  EXPECT_EQ(3u, omerc.size());

  const auto cea = op::getMappingsFromPROJName("cea");
  EXPECT_EQ(2u, cea.size());

  const auto eqearth = op::getMappingsFromPROJName("eqearth");
  ASSERT_EQ(1u, eqearth.size());
  EXPECT_EQ(EPSG_CODE_METHOD_EQUAL_EARTH, eqearth[0]->epsg_code);

  const auto tpeqd = op::getMappingsFromPROJName("tpeqd");
  ASSERT_EQ(1u, tpeqd.size());
  EXPECT_STREQ(PROJ_WKT2_NAME_METHOD_TWO_POINT_EQUIDISTANT,
               tpeqd[0]->wkt2_name);

  EXPECT_THAT(op::getMappingsFromPROJName(""), IsEmpty());
  EXPECT_THAT(op::getMappingsFromPROJName("axis=wsu"), IsEmpty());
  EXPECT_THAT(op::getMappingsFromPROJName("helmert"), IsEmpty());
  EXPECT_THAT(op::getMappingsFromPROJName("unknown_proj"), IsEmpty());
}

TEST(ParamMappingsTest, GetParamMappingByOperationParameterCoversAllTiers) {
  // 1. Method with params == nullptr immediately returns nullptr.
  const op::MethodMapping* no_params_method =
      op::getMapping(EPSG_NAME_METHOD_HEIGHT_DEPTH_REVERSAL);
  ASSERT_THAT(no_params_method, NotNull());
  ASSERT_EQ(nullptr, no_params_method->params);
  EXPECT_EQ(nullptr,
            op::getMapping(
                no_params_method,
                MakeParam(EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
                          EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN)));

  const op::MethodMapping* tm_mapping =
      op::getMapping(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR);
  ASSERT_THAT(tm_mapping, NotNull());

  // 2. Tier 1 (EPSG ID match): custom name with explicit ID 8801 matches lat_0.
  const op::ParamMapping* by_explicit_id = op::getMapping(
      tm_mapping, MakeParam("Custom Latitude Alias",
                            EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN));
  ASSERT_THAT(by_explicit_id, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
            by_explicit_id->epsg_code);
  EXPECT_STREQ("lat_0", by_explicit_id->proj_name);

  // Implicit EPSG ID lookup via gParamNameCodes ("Scale factor" -> 8805).
  const op::ParamMapping* by_implicit_id =
      op::getMapping(tm_mapping, MakeParam("Scale factor"));
  ASSERT_THAT(by_implicit_id, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN,
            by_implicit_id->epsg_code);
  EXPECT_STREQ("k", by_implicit_id->proj_name);

  // 3. Tier 2 (wkt2_name isEquivalentName):
  // Non-matching nonzero EPSG code misses Tier 1 and resolves in Tier 2.
  const op::ParamMapping* fallthrough_tier2 = op::getMapping(
      tm_mapping, MakeParam("latitude_of_natural_origin", 999999));
  ASSERT_THAT(fallthrough_tier2, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
            fallthrough_tier2->epsg_code);

  // Parameter absent from gParamNameCodes has epsg_code == 0, skips Tier 1, and
  // resolves in Tier 2.
  const op::MethodMapping* tpeqd_mapping =
      op::getMapping(PROJ_WKT2_NAME_METHOD_TWO_POINT_EQUIDISTANT);
  ASSERT_THAT(tpeqd_mapping, NotNull());
  const auto p_lat1 = MakeParam("latitude_of_1st_point");
  EXPECT_EQ(0, p_lat1->getEPSGCode());
  const op::ParamMapping* tpeqd_lat1 = op::getMapping(tpeqd_mapping, p_lat1);
  ASSERT_THAT(tpeqd_lat1, NotNull());
  EXPECT_STREQ("lat_1", tpeqd_lat1->proj_name);

  // 4. Tier 3 (areEquivalentParameters):
  // Parameter with epsg_code == 0 that differs from wkt2_name matches Tier 3.
  const auto p_central_parallel = MakeParam("Central_Parallel");
  EXPECT_EQ(0, p_central_parallel->getEPSGCode());
  const op::ParamMapping* eq_zero_id =
      op::getMapping(tm_mapping, p_central_parallel);
  ASSERT_THAT(eq_zero_id, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
            eq_zero_id->epsg_code);

  // Geostationary "height" maps to "Satellite Height" only via Tier 3.
  const op::MethodMapping* geos_mapping =
      op::getMapping(PROJ_WKT2_NAME_METHOD_GEOSTATIONARY_SATELLITE_SWEEP_Y);
  ASSERT_THAT(geos_mapping, NotNull());
  const op::ParamMapping* geos_h =
      op::getMapping(geos_mapping, MakeParam("height"));
  ASSERT_THAT(geos_h, NotNull());
  EXPECT_STREQ("Satellite Height", geos_h->wkt2_name);
  EXPECT_STREQ("h", geos_h->proj_name);

  // Parameter with valid EPSG code (8821 = Latitude of false origin) that is
  // not in TM's params fails Tier 1 and Tier 2, then matches lat_0 in Tier 3.
  const auto p_false_lat = MakeParam(EPSG_NAME_PARAMETER_LATITUDE_FALSE_ORIGIN);
  EXPECT_EQ(EPSG_CODE_PARAMETER_LATITUDE_FALSE_ORIGIN,
            p_false_lat->getEPSGCode());
  const op::ParamMapping* eq_nonzero_id =
      op::getMapping(tm_mapping, p_false_lat);
  ASSERT_THAT(eq_nonzero_id, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
            eq_nonzero_id->epsg_code);

  // 5. Complete misses and parameterized gOtherMethodMappings lookup.
  EXPECT_EQ(nullptr, op::getMapping(tm_mapping, MakeParam("")));
  EXPECT_EQ(nullptr, op::getMapping(tm_mapping, MakeParam("Unknown Param")));
  EXPECT_EQ(nullptr,
            op::getMapping(tm_mapping, MakeParam("Unknown Param", 999999)));

  const op::MethodMapping* molodensky =
      op::getMapping(EPSG_NAME_METHOD_MOLODENSKY);
  ASSERT_THAT(molodensky, NotNull());
  const op::ParamMapping* da_param = op::getMapping(
      molodensky, MakeParam(EPSG_NAME_PARAMETER_SEMI_MAJOR_AXIS_DIFFERENCE,
                            EPSG_CODE_PARAMETER_SEMI_MAJOR_AXIS_DIFFERENCE));
  ASSERT_THAT(da_param, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_SEMI_MAJOR_AXIS_DIFFERENCE,
            da_param->epsg_code);
  EXPECT_EQ(common::UnitOfMeasure::Type::LINEAR, da_param->unit_type);
  EXPECT_EQ(nullptr,
            op::getMapping(
                molodensky,
                MakeParam(EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
                          EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN)));
}

TEST(ParamMappingsTest,
     GetParamMappingFromWkt1CoversExactSkipEquivalentAndMiss) {
  const op::MethodMapping* tm_mapping =
      op::getMapping(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR);
  ASSERT_THAT(tm_mapping, NotNull());

  // 1. Exact & case/separator-insensitive WKT1 parameter matches on TM.
  const op::ParamMapping* lat0 =
      op::getMappingFromWKT1(tm_mapping, "latitude_of_origin");
  ASSERT_THAT(lat0, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, lat0->epsg_code);
  EXPECT_STREQ("lat_0", lat0->proj_name);

  const op::ParamMapping* lon0 =
      op::getMappingFromWKT1(tm_mapping, "CENTRAL MERIDIAN");
  ASSERT_THAT(lon0, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN, lon0->epsg_code);
  EXPECT_STREQ("lon_0", lon0->proj_name);

  const op::ParamMapping* k0 =
      op::getMappingFromWKT1(tm_mapping, "scale factor");
  ASSERT_THAT(k0, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN, k0->epsg_code);
  EXPECT_STREQ("k", k0->proj_name);

  // 2. Skip entries whose paramMapping->wkt1_name is nullptr (Mercator 1SP slot
  // 0).
  const op::MethodMapping* merc1sp =
      op::getMapping(EPSG_CODE_METHOD_MERCATOR_VARIANT_A);
  ASSERT_THAT(merc1sp, NotNull());
  ASSERT_EQ(nullptr, merc1sp->params[0]->wkt1_name);
  const op::ParamMapping* merc_lon0 =
      op::getMappingFromWKT1(merc1sp, "central_meridian");
  ASSERT_THAT(merc_lon0, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
            merc_lon0->epsg_code);
  EXPECT_EQ(nullptr, op::getMappingFromWKT1(merc1sp, "latitude_of_origin"));

  // 3. Fallback to areEquivalentParameters when isEquivalentName is false.
  const op::ParamMapping* tm_from_center_lat =
      op::getMappingFromWKT1(tm_mapping, "latitude_of_center");
  ASSERT_THAT(tm_from_center_lat, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
            tm_from_center_lat->epsg_code);

  const op::ParamMapping* tm_from_central_parallel =
      op::getMappingFromWKT1(tm_mapping, "Central_Parallel");
  ASSERT_THAT(tm_from_central_parallel, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
            tm_from_central_parallel->epsg_code);

  const op::ParamMapping* tm_from_center_lon =
      op::getMappingFromWKT1(tm_mapping, "longitude_of_center");
  ASSERT_THAT(tm_from_center_lon, NotNull());
  EXPECT_EQ(EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
            tm_from_center_lon->epsg_code);

  const op::MethodMapping* hom_two_pt = op::getMapping(
      PROJ_WKT2_NAME_METHOD_HOTINE_OBLIQUE_MERCATOR_TWO_POINT_NATURAL_ORIGIN);
  ASSERT_THAT(hom_two_pt, NotNull());
  const op::ParamMapping* hom_lat1 =
      op::getMappingFromWKT1(hom_two_pt, "Latitude_Of_1st_Point");
  ASSERT_THAT(hom_lat1, NotNull());
  EXPECT_STREQ("lat_1", hom_lat1->proj_name);

  // 4. Empty and unrecognized WKT1 parameter names return nullptr.
  EXPECT_EQ(nullptr, op::getMappingFromWKT1(tm_mapping, ""));
  EXPECT_EQ(nullptr,
            op::getMappingFromWKT1(tm_mapping, "unknown_wkt1_parameter"));
}

TEST(ParamMappingsTest,
     IntegrationConversionValidationWkt1AndTransformRoundTrip) {
  // 1. Create ISO-19111 Transverse Mercator conversion and validate its
  // parameters.
  util::PropertyMap tm_props;
  tm_props.set(common::IdentifiedObject::NAME_KEY, "UTM zone 32N");
  const auto tm_conv = op::Conversion::createTransverseMercator(
      tm_props, common::Angle(0.0), common::Angle(9.0), common::Scale(0.9996),
      common::Length(500000.0), common::Length(0.0));
  EXPECT_THAT(tm_conv->validateParameters(), IsEmpty());

  const auto wkt1_fmt =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  wkt1_fmt->setMultiLine(false);
  const std::string tm_method_wkt1 =
      tm_conv->method()->exportToWKT(wkt1_fmt.get());
  EXPECT_EQ("PROJECTION[\"Transverse_Mercator\",AUTHORITY[\"EPSG\",\"9807\"]]",
            tm_method_wkt1);

  // 2. Equal Earth has no WKT1 name, so WKT1 method export throws
  // FormattingException; an unknown operation method reports a validation error
  // via getMapping tables.
  util::PropertyMap eq_props;
  eq_props.set(common::IdentifiedObject::NAME_KEY, "Equal Earth");
  const auto eq_conv = op::Conversion::createEqualEarth(
      eq_props, common::Angle(0.0), common::Length(0.0), common::Length(0.0));
  EXPECT_THAT(eq_conv->validateParameters(), IsEmpty());
  const auto wkt1_fmt_eq =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THROW((void)eq_conv->method()->exportToWKT(wkt1_fmt_eq.get()),
               io::FormattingException);

  util::PropertyMap bad_conv_props;
  bad_conv_props.set(common::IdentifiedObject::NAME_KEY, "Invalid");
  const auto unknown_conv = op::Conversion::create(
      bad_conv_props, MakeMethod("Nonexistent Method Name"),
      std::vector<op::GeneralParameterValueNNPtr>{});
  const std::list<std::string> validation_errors =
      unknown_conv->validateParameters();
  EXPECT_THAT(validation_errors, Not(IsEmpty()));
  EXPECT_THAT(validation_errors.front(), HasSubstr("Unknown method"));

  // 3. Parse non-UTM TM WKT1 (no explicit AUTHORITY IDs) so getMappingFromWKT1
  // resolves method/parameters and Conversion::_exportToPROJString exports via
  // gProjectionMethodMappings; also parse a "UTM zone 32N" WKT1 method name.
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  const char* const kSingleLineWktOpts[] = {"MULTILINE=NO", nullptr};

  static constexpr char kCustomTmWkt1[] =
      "PROJCS[\"WGS 84 / Custom TM\","
      "GEOGCS[\"WGS 84\","
      "DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],"
      "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
      "PROJECTION[\"Transverse_Mercator\"],"
      "PARAMETER[\"latitude_of_origin\",0],"
      "PARAMETER[\"central_meridian\",10],"
      "PARAMETER[\"scale_factor\",0.9996],"
      "PARAMETER[\"false_easting\",500000],"
      "PARAMETER[\"false_northing\",0],"
      "UNIT[\"metre\",1],"
      "AXIS[\"Easting\",EAST],AXIS[\"Northing\",NORTH]]";

  PROJ_STRING_LIST raw_warnings = nullptr;
  PROJ_STRING_LIST raw_grammar_errors = nullptr;
  ScopedPj wkt1_crs(proj_create_from_wkt(ctx.get(), kCustomTmWkt1, nullptr,
                                         &raw_warnings, &raw_grammar_errors));
  ScopedStringList warnings(raw_warnings);
  ScopedStringList grammar_errors(raw_grammar_errors);
  ASSERT_THAT(wkt1_crs.get(), NotNull());
  EXPECT_EQ(nullptr, grammar_errors.get());
  EXPECT_EQ(nullptr, warnings.get());

  // Copy non-owning formatter pointers immediately.
  const char* wkt2_raw =
      proj_as_wkt(ctx.get(), wkt1_crs.get(), PJ_WKT2_2019, kSingleLineWktOpts);
  ASSERT_THAT(wkt2_raw, NotNull());
  const std::string wkt2_str(wkt2_raw);
  EXPECT_THAT(wkt2_str,
              HasSubstr("METHOD[\"Transverse Mercator\",ID[\"EPSG\",9807]]"));

  const char* proj_raw =
      proj_as_proj_string(ctx.get(), wkt1_crs.get(), PJ_PROJ_5, nullptr);
  ASSERT_THAT(proj_raw, NotNull());
  const std::string proj_str(proj_raw);
  EXPECT_THAT(proj_str, HasSubstr("+proj=tmerc"));
  EXPECT_THAT(proj_str, HasSubstr("+lon_0=10"));
  EXPECT_THAT(proj_str, HasSubstr("+k=0.9996"));
  EXPECT_THAT(proj_str, HasSubstr("+x_0=500000"));

  static constexpr char kUtmZoneWkt1[] =
      "PROJCS[\"WGS 84 / UTM zone 32N\","
      "GEOGCS[\"WGS 84\","
      "DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],"
      "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
      "PROJECTION[\"UTM zone 32N\"],"
      "PARAMETER[\"latitude_of_origin\",0],"
      "PARAMETER[\"central_meridian\",9],"
      "PARAMETER[\"scale_factor\",0.9996],"
      "PARAMETER[\"false_easting\",500000],"
      "PARAMETER[\"false_northing\",0],"
      "UNIT[\"metre\",1],"
      "AXIS[\"Easting\",EAST],AXIS[\"Northing\",NORTH]]";

  PROJ_STRING_LIST raw_utm_warnings = nullptr;
  PROJ_STRING_LIST raw_utm_grammar_errors = nullptr;
  ScopedPj utm_wkt1_crs(proj_create_from_wkt(ctx.get(), kUtmZoneWkt1, nullptr,
                                             &raw_utm_warnings,
                                             &raw_utm_grammar_errors));
  ScopedStringList utm_warnings(raw_utm_warnings);
  ScopedStringList utm_grammar_errors(raw_utm_grammar_errors);
  ASSERT_THAT(utm_wkt1_crs.get(), NotNull());
  EXPECT_EQ(nullptr, utm_grammar_errors.get());
  EXPECT_EQ(nullptr, utm_warnings.get());

  const char* utm_wkt2_raw = proj_as_wkt(ctx.get(), utm_wkt1_crs.get(),
                                         PJ_WKT2_2019, kSingleLineWktOpts);
  ASSERT_THAT(utm_wkt2_raw, NotNull());
  const std::string utm_wkt2_str(utm_wkt2_raw);
  EXPECT_THAT(utm_wkt2_str,
              HasSubstr("METHOD[\"Transverse Mercator\",ID[\"EPSG\",9807]]"));

  const char* utm_proj_raw =
      proj_as_proj_string(ctx.get(), utm_wkt1_crs.get(), PJ_PROJ_5, nullptr);
  ASSERT_THAT(utm_proj_raw, NotNull());
  const std::string utm_proj_str(utm_proj_raw);
  EXPECT_THAT(utm_proj_str, HasSubstr("+proj=utm"));
  EXPECT_THAT(utm_proj_str, HasSubstr("+zone=32"));

  // 4. Compare EPSG:4326 (lat, lon in degrees) -> WKT1 CRS against classic
  // +proj=tmerc (lon, lat in radians), then verify inverse round-trip.
  ScopedPj crs2crs(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4326", kCustomTmWkt1, nullptr));
  ASSERT_THAT(crs2crs.get(), NotNull());
  EXPECT_EQ(0, proj_errno(crs2crs.get()));

  ScopedPj proj_tmerc(
      proj_create(ctx.get(),
                  "+proj=tmerc +lat_0=0 +lon_0=10 +k=0.9996 +x_0=500000 +y_0=0 "
                  "+datum=WGS84"));
  ASSERT_THAT(proj_tmerc.get(), NotNull());

  constexpr double kLatDeg = 55.0;
  constexpr double kLonDeg = 12.0;
  // EPSG:4326 axis order: [0] = geodetic latitude (deg), [1] = longitude (deg).
  const PJ_COORD fwd =
      proj_trans(crs2crs.get(), PJ_FWD, proj_coord(kLatDeg, kLonDeg, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(crs2crs.get()));

  // Classic PROJ strings take radians in (lon, lat) order.
  const PJ_COORD ref = proj_trans(
      proj_tmerc.get(), PJ_FWD,
      proj_coord(proj_torad(kLonDeg), proj_torad(kLatDeg), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(proj_tmerc.get()));

  EXPECT_NEAR(ref.xy.x, fwd.xy.x, 1e-7);
  EXPECT_NEAR(ref.xy.y, fwd.xy.y, 1e-7);

  const PJ_COORD inv = proj_trans(crs2crs.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(crs2crs.get()));
  EXPECT_NEAR(kLatDeg, inv.v[0], 1e-9);
  EXPECT_NEAR(kLonDeg, inv.v[1], 1e-9);
}

}  // namespace
}  // namespace proj
