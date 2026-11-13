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
// Tests GeoKey/value name and code lookups (geo_names.c), including
// GeoTIFF v1.0 tables and v1.1 PROJ proj.db queries.
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geo_names.c

#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/libgeotiff/geokeys.h"
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/geovalues.h"
#include "third_party/libgeotiff/google/util/scoped_geotiff.h"
#include "third_party/libgeotiff/libxtiff/xtiffio.h"  // IWYU pragma: keep

namespace libgeotiff {
namespace {

using ::testing::AllOf;
using ::testing::HasSubstr;
using ::testing::StartsWith;

TEST(GeoNamesTest, KeyNameAndKeyCodeSupportV10AndV11) {
  // 1. V1.0 name and unknown code formatting (copied immediately because
  // GTIFKeyName returns a shared static buffer on misses).
  EXPECT_STREQ("GTModelTypeGeoKey", GTIFKeyName(GTModelTypeGeoKey));
  const std::string unknown_key = GTIFKeyName(static_cast<geokey_t>(65000));
  EXPECT_EQ("Unknown-65000", unknown_key);

  EXPECT_EQ(GeographicTypeGeoKey, GTIFKeyCode("GeographicTypeGeoKey"));
  // GeodeticCRSGeoKey only exists in _keyInfoV11, exercising V1.1 fallback.
  EXPECT_EQ(GeodeticCRSGeoKey, GTIFKeyCode("GeodeticCRSGeoKey"));
  EXPECT_EQ(1234, GTIFKeyCode("Unknown-1234"));
  EXPECT_EQ(5678, GTIFKeyCode("Code-5678"));
  EXPECT_EQ(-1, GTIFKeyCode("NoSuchGeoKey"));

  // 2. GTIFKeyNameEx distinguishes GeoTIFF 1.0 vs 1.1 key spellings.
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  ASSERT_EQ(1, GTIFSetVersionNumbers(handle.gtif.get(), 1, 1, 0));
  EXPECT_STREQ("GeographicTypeGeoKey",
               GTIFKeyNameEx(handle.gtif.get(), GeographicTypeGeoKey));
  const std::string ex_unknown_v10 =
      GTIFKeyNameEx(handle.gtif.get(), static_cast<geokey_t>(64999));
  EXPECT_EQ("Unknown-64999", ex_unknown_v10);

  ASSERT_EQ(1, GTIFSetVersionNumbers(handle.gtif.get(), 1, 1, 1));
  EXPECT_STREQ("GeodeticCRSGeoKey",
               GTIFKeyNameEx(handle.gtif.get(), GeodeticCRSGeoKey));
  const std::string ex_unknown_v11 =
      GTIFKeyNameEx(handle.gtif.get(), static_cast<geokey_t>(64998));
  EXPECT_EQ("Unknown-64998", ex_unknown_v11);
}

TEST(GeoNamesTest, TypeAndTagNamesRoundTripWithAliasesAndMisses) {
  EXPECT_STREQ("Byte", GTIFTypeName(TYPE_BYTE));
  EXPECT_STREQ("Short", GTIFTypeName(TYPE_SHORT));
  EXPECT_STREQ("Double", GTIFTypeName(TYPE_DOUBLE));
  EXPECT_STREQ("Ascii", GTIFTypeName(TYPE_ASCII));
  EXPECT_STREQ("Unknown", GTIFTypeName(TYPE_UNKNOWN));
  const std::string unknown_type = GTIFTypeName(static_cast<tagtype_t>(999));
  EXPECT_EQ("Unknown-999", unknown_type);

  EXPECT_EQ(TYPE_SHORT, GTIFTypeCode("Short"));
  EXPECT_EQ(TYPE_DOUBLE, GTIFTypeCode("Double"));
  EXPECT_EQ(99, GTIFTypeCode("Unknown-99"));
  EXPECT_EQ(77, GTIFTypeCode("Code-77"));
  EXPECT_EQ(-1, GTIFTypeCode("NoSuchType"));

  EXPECT_STREQ("ModelPixelScaleTag", GTIFTagName(TIFFTAG_GEOPIXELSCALE));
  EXPECT_STREQ("ModelTransformationTag", GTIFTagName(TIFFTAG_GEOTRANSMATRIX));
  EXPECT_STREQ("ModelTiepointTag", GTIFTagName(TIFFTAG_GEOTIEPOINTS));
  const std::string unknown_tag = GTIFTagName(12345);
  EXPECT_EQ("Unknown-12345", unknown_tag);

  EXPECT_EQ(TIFFTAG_GEOPIXELSCALE, GTIFTagCode("ModelPixelScaleTag"));
  EXPECT_EQ(TIFFTAG_GEOTRANSMATRIX, GTIFTagCode("ModelTransformationTag"));
  EXPECT_EQ(TIFFTAG_GEOTRANSMATRIX, GTIFTagCode("IntergraphMatrixTag"));
  EXPECT_EQ(-1, GTIFTagCode("NoSuchTag"));
}

TEST(GeoNamesTest, ValueNameAndCodeCoverEveryFindTableCategory) {
  struct TableCase {
    geokey_t key;
    int value;
    const char* expected_name;
  };
  constexpr TableCase kCases[] = {
      {.key = GeogLinearUnitsGeoKey,
       .value = Linear_Meter,
       .expected_name = "Linear_Meter"},
      {.key = ProjLinearUnitsGeoKey,
       .value = Linear_Foot,
       .expected_name = "Linear_Foot"},
      {.key = GeogAngularUnitsGeoKey,
       .value = Angular_Degree,
       .expected_name = "Angular_Degree"},
      {.key = GeogAzimuthUnitsGeoKey,
       .value = Angular_Radian,
       .expected_name = "Angular_Radian"},
      {.key = VerticalUnitsGeoKey,
       .value = Linear_Meter,
       .expected_name = "Linear_Meter"},
      {.key = GTModelTypeGeoKey,
       .value = ModelTypeProjected,
       .expected_name = "ModelTypeProjected"},
      {.key = GTRasterTypeGeoKey,
       .value = RasterPixelIsArea,
       .expected_name = "RasterPixelIsArea"},
      {.key = GeographicTypeGeoKey,
       .value = GCS_WGS_84,
       .expected_name = "GCS_WGS_84"},
      {.key = GeogGeodeticDatumGeoKey,
       .value = Datum_WGS84,
       .expected_name = "Datum_WGS84"},
      {.key = GeogEllipsoidGeoKey,
       .value = Ellipse_WGS_84,
       .expected_name = "Ellipse_WGS_84"},
      {.key = GeogPrimeMeridianGeoKey,
       .value = PM_Greenwich,
       .expected_name = "PM_Greenwich"},
      {.key = ProjectedCSTypeGeoKey,
       .value = PCS_WGS84_UTM_zone_10N,
       .expected_name = "PCS_WGS84_UTM_zone_10N"},
      {.key = ProjectionGeoKey,
       .value = Proj_UTM_zone_10N,
       .expected_name = "Proj_UTM_zone_10N"},
      {.key = ProjCoordTransGeoKey,
       .value = CT_TransverseMercator,
       .expected_name = "CT_TransverseMercator"},
      {.key = VerticalCSTypeGeoKey,
       .value = VertCS_WGS_84_ellipsoid,
       .expected_name = "VertCS_WGS_84_ellipsoid"},
      {.key = VerticalDatumGeoKey,
       .value = VDatumBase,
       .expected_name = "VDatumBase"},
      {.key = GTCitationGeoKey,
       .value = KvUserDefined,
       .expected_name = "User-Defined"},
      {.key = GTCitationGeoKey,
       .value = KvUndefined,
       .expected_name = "Undefined"},
  };
  for (const auto& tc : kCases) {
    const std::string name = GTIFValueName(tc.key, tc.value);
    EXPECT_EQ(tc.expected_name, name) << "key=" << tc.key;
    EXPECT_EQ(tc.value, GTIFValueCode(tc.key, tc.expected_name))
        << "key=" << tc.key;
  }

  const std::string missing_val = GTIFValueName(ProjectedCSTypeGeoKey, 999999);
  EXPECT_EQ("Unknown-999999", missing_val);
  EXPECT_EQ(-1, GTIFValueCode(ProjectedCSTypeGeoKey, "NoSuchValueName"));
  // KvUndefined is 0, so check an explicit value rather than EXPECT_FALSE.
  EXPECT_EQ(KvUndefined, GTIFValueCode(GTModelTypeGeoKey, "Undefined"));
}

TEST(GeoNamesTest, ValueNameExHandlesV10HardcodedAndV11ProjDatabaseQueries) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  // 1. GeoTIFF 1.0 uses hardcoded tables; misses fall back to proj.db.
  ASSERT_EQ(1, GTIFSetVersionNumbers(handle.gtif.get(), 1, 1, 0));
  EXPECT_STREQ("PCS_WGS84_UTM_zone_10N",
               GTIFValueNameEx(handle.gtif.get(), ProjectedCSTypeGeoKey,
                               PCS_WGS84_UTM_zone_10N));

  // 2. GeoTIFF 1.1 queries proj.db while preserving special hardcoded keys.
  ASSERT_EQ(1, GTIFSetVersionNumbers(handle.gtif.get(), 1, 1, 1));
  EXPECT_STREQ(
      "Undefined",
      GTIFValueNameEx(handle.gtif.get(), ProjectedCSTypeGeoKey, KvUndefined));
  EXPECT_STREQ(
      "User-Defined",
      GTIFValueNameEx(handle.gtif.get(), ProjectedCSTypeGeoKey, KvUserDefined));
  EXPECT_STREQ("ModelTypeProjected",
               GTIFValueNameEx(handle.gtif.get(), GTModelTypeGeoKey,
                               ModelTypeProjected));
  EXPECT_STREQ("RasterPixelIsPoint",
               GTIFValueNameEx(handle.gtif.get(), GTRasterTypeGeoKey,
                               RasterPixelIsPoint));
  EXPECT_STREQ("CT_TransverseMercator",
               GTIFValueNameEx(handle.gtif.get(), ProjCoordTransGeoKey,
                               CT_TransverseMercator));
  EXPECT_STREQ("VertCS_WGS_84_ellipsoid",
               GTIFValueNameEx(handle.gtif.get(), VerticalCSTypeGeoKey,
                               VertCS_WGS_84_ellipsoid));

  // Database lookups across every switch category in GTIFValueNameEx.
  EXPECT_THAT(GTIFValueNameEx(handle.gtif.get(), ProjLinearUnitsGeoKey, 9001),
              StartsWith("Code-9001 ("));
  EXPECT_THAT(
      GTIFValueNameEx(handle.gtif.get(), GeogGeodeticDatumGeoKey, 6326),
      AllOf(StartsWith("Code-6326 ("), HasSubstr("World Geodetic System")));
  EXPECT_THAT(GTIFValueNameEx(handle.gtif.get(), VerticalDatumGeoKey, 5103),
              StartsWith("Code-5103 ("));
  EXPECT_THAT(GTIFValueNameEx(handle.gtif.get(), GeogEllipsoidGeoKey, 7030),
              StartsWith("Code-7030 ("));
  EXPECT_THAT(GTIFValueNameEx(handle.gtif.get(), GeogPrimeMeridianGeoKey, 8901),
              StartsWith("Code-8901 ("));
  EXPECT_THAT(GTIFValueNameEx(handle.gtif.get(), GeographicTypeGeoKey, 4326),
              StartsWith("Code-4326 ("));
  EXPECT_THAT(GTIFValueNameEx(handle.gtif.get(), ProjectedCSTypeGeoKey, 32610),
              StartsWith("Code-32610 ("));
  EXPECT_THAT(GTIFValueNameEx(handle.gtif.get(), VerticalCSTypeGeoKey, 5703),
              StartsWith("Code-5703 ("));
  EXPECT_THAT(GTIFValueNameEx(handle.gtif.get(), ProjectionGeoKey, 16010),
              StartsWith("Code-16010 ("));

  // Unknown database codes and default key category produce Unknown-<code>.
  EXPECT_STREQ(
      "Unknown-999999",
      GTIFValueNameEx(handle.gtif.get(), ProjLinearUnitsGeoKey, 999999));
  EXPECT_STREQ(
      "Unknown-999999",
      GTIFValueNameEx(handle.gtif.get(), GeogGeodeticDatumGeoKey, 999999));
  EXPECT_STREQ("Unknown-12345",
               GTIFValueNameEx(handle.gtif.get(), GTCitationGeoKey, 12345));
}

}  // namespace
}  // namespace libgeotiff
