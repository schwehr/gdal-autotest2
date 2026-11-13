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
// Tests PROJ.4 conversion and coordinate transforms (geotiff_proj4.c).
//
// In google3/third_party/libgeotiff/geotiff_proj4.c, GTIFSetFromProj4 lines
// 400-771 sit inside `#ifdef notdef`, leaving only `longlat`/`latlong`,
// `tmerc`, `utm`, and `lcc` (1SP/2SP) enabled.
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geotiff_proj4.c

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/libgeotiff/geo_normalize.h"
#include "third_party/libgeotiff/geokeys.h"
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/geovalues.h"
#include "third_party/libgeotiff/google/util/scoped_geotiff.h"

namespace libgeotiff {
namespace {

using ::testing::AllOf;
using ::testing::HasSubstr;
using ::testing::Not;

TEST(GeotiffProj4Test, SetFromProj4RejectsNullMissingProjAndUnsupported) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  EXPECT_EQ(0, GTIFSetFromProj4(handle.gtif.get(), nullptr));
  EXPECT_EQ(0, GTIFSetFromProj4(handle.gtif.get(), "+ellps=WGS84 +units=m"));

  // Upstream lines 400-771 are inside `#ifdef notdef`, so other PROJ methods
  // such as `merc` return 0 in GTIFSetFromProj4.
  EXPECT_EQ(0, GTIFSetFromProj4(handle.gtif.get(),
                                "+proj=merc +lat_ts=0 +lon_0=0 +datum=WGS84"));
}

TEST(GeotiffProj4Test,
     SetFromProj4TokenizesWhitespaceExponentsBooleansAndMaxTokenLimit) {
  // 1. Tabs, newlines, scientific-notation `+` exponents, and `+k_0` fallback.
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    constexpr char kTmercProj4[] =
        "+proj=tmerc\t+lat_0=37.0\n+lon_0=-122.0 +k_0=0.9996 +x_0=5e+05 "
        "+y_0=0 +ellps=WGS84 +units=meter";
    ASSERT_EQ(1, GTIFSetFromProj4(handle.gtif.get(), kTmercProj4));

    double scale = 0.0;
    double false_easting = 0.0;
    EXPECT_EQ(1, GTIFKeyGetDOUBLE(handle.gtif.get(), ProjScaleAtNatOriginGeoKey,
                                  &scale, 0, 1));
    EXPECT_EQ(1, GTIFKeyGetDOUBLE(handle.gtif.get(), ProjFalseEastingGeoKey,
                                  &false_easting, 0, 1));
    EXPECT_DOUBLE_EQ(0.9996, scale);
    EXPECT_DOUBLE_EQ(500000.0, false_easting);
  }

  // 2. Boolean `+south` followed by `+` exercises `=yes` expansion.
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    constexpr char kUtmSouthMid[] =
        "+proj=utm +zone=10 +south +ellps=WGS84 +units=m";
    ASSERT_EQ(1, GTIFSetFromProj4(handle.gtif.get(), kUtmSouthMid));

    double false_northing = 0.0;
    double lon_0 = 0.0;
    EXPECT_EQ(1, GTIFKeyGetDOUBLE(handle.gtif.get(), ProjFalseNorthingGeoKey,
                                  &false_northing, 0, 1));
    EXPECT_EQ(1, GTIFKeyGetDOUBLE(handle.gtif.get(), ProjNatOriginLongGeoKey,
                                  &lon_0, 0, 1));
    EXPECT_DOUBLE_EQ(10000000.0, false_northing);
    EXPECT_DOUBLE_EQ(-123.0, lon_0);
  }

  // 3. Boolean `+south` as the final token exercises exact-length lookup
  // (OSR_GSV returning ""), and trailing empty `+` exercises empty tail.
  {
    SimpleTagsGtif south_last = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, south_last.gtif);
    ASSERT_EQ(1, GTIFSetFromProj4(south_last.gtif.get(),
                                  "+proj=utm +zone=18 +south"));
    double false_northing = 0.0;
    EXPECT_EQ(1,
              GTIFKeyGetDOUBLE(south_last.gtif.get(), ProjFalseNorthingGeoKey,
                               &false_northing, 0, 1));
    EXPECT_DOUBLE_EQ(10000000.0, false_northing);

    SimpleTagsGtif north_trailing_plus = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, north_trailing_plus.gtif);
    ASSERT_EQ(1, GTIFSetFromProj4(north_trailing_plus.gtif.get(),
                                  "+proj=utm +zone=18 +"));
    false_northing = -1.0;
    EXPECT_EQ(1,
              GTIFKeyGetDOUBLE(north_trailing_plus.gtif.get(),
                               ProjFalseNorthingGeoKey, &false_northing, 0, 1));
    EXPECT_DOUBLE_EQ(0.0, false_northing);
  }

  // 4. Exceeding the 200-token cap truncates safely without buffer overflow.
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    std::string many_tokens = "+proj=utm +zone=10";
    for (int i = 0; i < 205; ++i) {
      many_tokens += " +extra=1";
    }
    EXPECT_EQ(1, GTIFSetFromProj4(handle.gtif.get(), many_tokens.c_str()));
  }
}

TEST(GeotiffProj4Test, SetFromProj4SupportsActiveProjections) {
  // 1. longlat / latlong leave GTModelTypeGeoKey unset while writing GCS/datum.
  for (const char* proj_str :
       {"+proj=longlat +datum=WGS84", "+proj=latlong +datum=NAD83"}) {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ASSERT_EQ(1, GTIFSetFromProj4(handle.gtif.get(), proj_str));
    EXPECT_EQ(
        0, GTIFKeyInfo(handle.gtif.get(), GTModelTypeGeoKey, nullptr, nullptr));
    uint16_t gcs = 0;
    EXPECT_EQ(1, GTIFKeyGetSHORT(handle.gtif.get(), GeographicTypeGeoKey, &gcs,
                                 0, 1));
    EXPECT_NE(0, gcs);
  }

  // 2. lcc with lat_0 == lat_1 selects CT_LambertConfConic_1SP.
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ASSERT_EQ(1, GTIFSetFromProj4(handle.gtif.get(),
                                  "+proj=lcc +lat_0=45 +lat_1=45 +lon_0=-90 "
                                  "+k=0.9999 +x_0=100 +y_0=200 +datum=NAD83"));
    uint16_t ct = 0;
    double scale = 0.0;
    EXPECT_EQ(
        1, GTIFKeyGetSHORT(handle.gtif.get(), ProjCoordTransGeoKey, &ct, 0, 1));
    EXPECT_EQ(CT_LambertConfConic_1SP, ct);
    EXPECT_EQ(1, GTIFKeyGetDOUBLE(handle.gtif.get(), ProjScaleAtNatOriginGeoKey,
                                  &scale, 0, 1));
    EXPECT_DOUBLE_EQ(0.9999, scale);
  }

  // 3. lcc with lat_0 != lat_1 selects CT_LambertConfConic_2SP.
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ASSERT_EQ(1,
              GTIFSetFromProj4(handle.gtif.get(),
                               "+proj=lcc +lat_0=37 +lat_1=33 +lat_2=45 "
                               "+lon_0=-120 +x_0=2000 +y_0=500 +datum=NAD27"));
    uint16_t ct = 0;
    double std_parallel_2 = 0.0;
    EXPECT_EQ(
        1, GTIFKeyGetSHORT(handle.gtif.get(), ProjCoordTransGeoKey, &ct, 0, 1));
    EXPECT_EQ(CT_LambertConfConic_2SP, ct);
    EXPECT_EQ(1, GTIFKeyGetDOUBLE(handle.gtif.get(), ProjStdParallel2GeoKey,
                                  &std_parallel_2, 0, 1));
    EXPECT_DOUBLE_EQ(45.0, std_parallel_2);
  }
}

TEST(GeotiffProj4Test, SetFromProj4MapsDatumsEllipsoidsAndLinearUnits) {
  // 1. Named datums write GeographicTypeGeoKey directly.
  struct DatumCase {
    const char* proj4;
    uint16_t expected_gcs;
  };
  constexpr DatumCase kDatumCases[] = {
      {.proj4 = "+proj=tmerc +datum=WGS84", .expected_gcs = GCS_WGS_84},
      {.proj4 = "+proj=tmerc +datum=NAD83", .expected_gcs = GCS_NAD83},
      {.proj4 = "+proj=tmerc +datum=NAD27", .expected_gcs = GCS_NAD27},
  };
  for (const auto& tc : kDatumCases) {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ASSERT_EQ(1, GTIFSetFromProj4(handle.gtif.get(), tc.proj4));
    uint16_t gcs = 0;
    EXPECT_EQ(1, GTIFKeyGetSHORT(handle.gtif.get(), GeographicTypeGeoKey, &gcs,
                                 0, 1));
    EXPECT_EQ(tc.expected_gcs, gcs);
  }

  // 2. Named ellipsoids write GeogEllipsoidGeoKey when datum is user-defined.
  struct EllipsoidCase {
    const char* proj4;
    uint16_t expected_ellipsoid;
  };
  constexpr EllipsoidCase kEllipsoidCases[] = {
      {.proj4 = "+proj=tmerc +ellps=WGS84",
       .expected_ellipsoid = Ellipse_WGS_84},
      {.proj4 = "+proj=tmerc +ellps=clrk66",
       .expected_ellipsoid = Ellipse_Clarke_1866},
      {.proj4 = "+proj=tmerc +ellps=clrk80",
       .expected_ellipsoid = Ellipse_Clarke_1880},
      {.proj4 = "+proj=tmerc +ellps=GRS80",
       .expected_ellipsoid = Ellipse_GRS_1980},
  };
  for (const auto& tc : kEllipsoidCases) {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ASSERT_EQ(1, GTIFSetFromProj4(handle.gtif.get(), tc.proj4));
    uint16_t ellipsoid = 0;
    EXPECT_EQ(1, GTIFKeyGetSHORT(handle.gtif.get(), GeogEllipsoidGeoKey,
                                 &ellipsoid, 0, 1));
    EXPECT_EQ(tc.expected_ellipsoid, ellipsoid);
  }

  // 3. Explicit +a/+b derives inverse flattening; +a alone writes sphere +b=a;
  // +a/+rf writes +rf directly.
  {
    SimpleTagsGtif ab_handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, ab_handle.gtif);
    ASSERT_EQ(1, GTIFSetFromProj4(ab_handle.gtif.get(),
                                  "+proj=tmerc +datum=unknown +ellps=unknown "
                                  "+a=6378137 +b=6356752.314245"));
    double semi_major = 0.0;
    double inv_flat = 0.0;
    EXPECT_EQ(1, GTIFKeyGetDOUBLE(ab_handle.gtif.get(), GeogSemiMajorAxisGeoKey,
                                  &semi_major, 0, 1));
    EXPECT_EQ(1, GTIFKeyGetDOUBLE(ab_handle.gtif.get(), GeogInvFlatteningGeoKey,
                                  &inv_flat, 0, 1));
    EXPECT_DOUBLE_EQ(6378137.0, semi_major);
    EXPECT_NEAR(298.257223563, inv_flat, 1e-6);

    SimpleTagsGtif sphere_handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, sphere_handle.gtif);
    ASSERT_EQ(1, GTIFSetFromProj4(sphere_handle.gtif.get(),
                                  "+proj=tmerc +a=6370000"));
    double semi_minor = 0.0;
    EXPECT_EQ(1, GTIFKeyGetDOUBLE(sphere_handle.gtif.get(),
                                  GeogSemiMinorAxisGeoKey, &semi_minor, 0, 1));
    EXPECT_DOUBLE_EQ(6370000.0, semi_minor);

    SimpleTagsGtif rf_handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, rf_handle.gtif);
    ASSERT_EQ(1, GTIFSetFromProj4(rf_handle.gtif.get(),
                                  "+proj=tmerc +a=6378000 +rf=300"));
    EXPECT_EQ(1, GTIFKeyGetDOUBLE(rf_handle.gtif.get(), GeogInvFlatteningGeoKey,
                                  &inv_flat, 0, 1));
    EXPECT_DOUBLE_EQ(300.0, inv_flat);
  }

  // 4. Linear units (`+units=` and `+to_meter=`).
  struct UnitCase {
    const char* proj4;
    uint16_t expected_unit;
    double expected_size;
  };
  constexpr UnitCase kUnitCases[] = {
      {.proj4 = "+proj=tmerc +units=meter",
       .expected_unit = Linear_Meter,
       .expected_size = 0.0},
      {.proj4 = "+proj=tmerc +units=m",
       .expected_unit = Linear_Meter,
       .expected_size = 0.0},
      {.proj4 = "+proj=tmerc +units=us-ft",
       .expected_unit = Linear_Foot_US_Survey,
       .expected_size = 0.0},
      {.proj4 = "+proj=tmerc +units=ft",
       .expected_unit = Linear_Foot,
       .expected_size = 0.0},
      {.proj4 = "+proj=tmerc +to_meter=0.5",
       .expected_unit = KvUserDefined,
       .expected_size = 0.5},
  };
  for (const auto& tc : kUnitCases) {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ASSERT_EQ(1, GTIFSetFromProj4(handle.gtif.get(), tc.proj4));
    uint16_t unit = 0;
    EXPECT_EQ(1, GTIFKeyGetSHORT(handle.gtif.get(), ProjLinearUnitsGeoKey,
                                 &unit, 0, 1));
    EXPECT_EQ(tc.expected_unit, unit);
    if (tc.expected_size > 0.0) {
      double unit_size = 0.0;
      EXPECT_EQ(1, GTIFKeyGetDOUBLE(handle.gtif.get(), ProjLinearUnitSizeGeoKey,
                                    &unit_size, 0, 1));
      EXPECT_DOUBLE_EQ(tc.expected_size, unit_size);
    }
  }
}

TEST(GeotiffProj4Test, GetProj4DefnHandlesNullAndUnsetDefn) {
  ScopedGtifMem from_null(GTIFGetProj4Defn(nullptr));
  ASSERT_NE(nullptr, from_null);
  EXPECT_STREQ("", from_null.get());

  ScopedDefn unset_defn(GTIFAllocDefn());
  ASSERT_NE(nullptr, unset_defn);
  unset_defn->DefnSet = 0;
  ScopedGtifMem from_unset(GTIFGetProj4Defn(unset_defn.get()));
  ASSERT_NE(nullptr, from_unset);
  EXPECT_STREQ("", from_unset.get());
}

TEST(GeotiffProj4Test, GetProj4DefnFormatsEllipsoidsAndLinearUnits) {
  // 1. Named ellipsoids, explicit axes, and omitted ellipsoid when 0.0.
  struct EllipsoidCase {
    int16_t ellipsoid;
    double semi_major;
    double semi_minor;
    std::string_view expected_substr;
    bool expect_ellps;
  };
  constexpr EllipsoidCase kEllipsoidCases[] = {
      {.ellipsoid = Ellipse_WGS_84,
       .semi_major = 0.0,
       .semi_minor = 0.0,
       .expected_substr = "+ellps=WGS84 ",
       .expect_ellps = true},
      {.ellipsoid = Ellipse_Clarke_1866,
       .semi_major = 0.0,
       .semi_minor = 0.0,
       .expected_substr = "+ellps=clrk66 ",
       .expect_ellps = true},
      {.ellipsoid = Ellipse_Clarke_1880,
       .semi_major = 0.0,
       .semi_minor = 0.0,
       .expected_substr = "+ellps=clrk80 ",
       .expect_ellps = true},
      {.ellipsoid = Ellipse_GRS_1980,
       .semi_major = 0.0,
       .semi_minor = 0.0,
       .expected_substr = "+ellps=GRS80 ",
       .expect_ellps = true},
      {.ellipsoid = KvUserDefined,
       .semi_major = 6378137.0,
       .semi_minor = 6356752.314,
       .expected_substr = "+a=6378137.000 +b=6356752.314 ",
       .expect_ellps = true},
      {.ellipsoid = KvUserDefined,
       .semi_major = 0.0,
       .semi_minor = 0.0,
       .expected_substr = "+proj=latlong ",
       .expect_ellps = false},
  };
  for (const auto& tc : kEllipsoidCases) {
    ScopedDefn defn(GTIFAllocDefn());
    ASSERT_NE(nullptr, defn);
    defn->DefnSet = 1;
    defn->Model = ModelTypeGeographic;
    defn->UOMLength = Linear_Meter;
    defn->Ellipsoid = tc.ellipsoid;
    defn->SemiMajor = tc.semi_major;
    defn->SemiMinor = tc.semi_minor;

    ScopedGtifMem proj4(GTIFGetProj4Defn(defn.get()));
    ASSERT_NE(nullptr, proj4);
    EXPECT_THAT(proj4.get(), HasSubstr(tc.expected_substr));
    if (!tc.expect_ellps) {
      EXPECT_THAT(proj4.get(),
                  AllOf(Not(HasSubstr("+ellps=")), Not(HasSubstr("+a="))));
    }
  }

  // 2. All 8 named linear units plus custom `+to_meter=`.
  struct UnitCase {
    int16_t uom_length;
    double in_meters;
    std::string_view expected_substr;
  };
  constexpr UnitCase kUnitCases[] = {
      {.uom_length = Linear_Meter,
       .in_meters = 1.0,
       .expected_substr = "+units=m "},
      {.uom_length = Linear_Foot,
       .in_meters = 0.3048,
       .expected_substr = "+units=ft "},
      {.uom_length = Linear_Foot_US_Survey,
       .in_meters = 1200.0 / 3937.0,
       .expected_substr = "+units=us-ft "},
      {.uom_length = Linear_Foot_Indian,
       .in_meters = 0.30479951,
       .expected_substr = "+units=ind-ft "},
      {.uom_length = Linear_Link,
       .in_meters = 0.201168,
       .expected_substr = "+units=link "},
      {.uom_length = Linear_Yard_Indian,
       .in_meters = 0.91439853,
       .expected_substr = "+units=ind-yd "},
      {.uom_length = Linear_Fathom,
       .in_meters = 1.8288,
       .expected_substr = "+units=fath "},
      {.uom_length = Linear_Mile_International_Nautical,
       .in_meters = 1852.0,
       .expected_substr = "+units=kmi "},
      {.uom_length = KvUserDefined,
       .in_meters = 0.25,
       .expected_substr = "+to_meter=0.2500000000"},
  };
  for (const auto& tc : kUnitCases) {
    ScopedDefn defn(GTIFAllocDefn());
    ASSERT_NE(nullptr, defn);
    defn->DefnSet = 1;
    defn->Model = ModelTypeGeographic;
    defn->Ellipsoid = Ellipse_WGS_84;
    defn->UOMLength = tc.uom_length;
    defn->UOMLengthInMeters = tc.in_meters;

    ScopedGtifMem proj4(GTIFGetProj4Defn(defn.get()));
    ASSERT_NE(nullptr, proj4);
    EXPECT_THAT(proj4.get(), HasSubstr(tc.expected_substr));
  }
}

TEST(GeotiffProj4Test, GetProj4DefnFormatsAllProjectionBranches) {
  struct ProjectionCase {
    int16_t model;
    int map_sys;
    int zone;
    int16_t ct_projection;
    std::array<double, 7> parms;
    std::string_view expected_substr;
    std::string_view extra_substr = "";
  };
  constexpr ProjectionCase kCases[] = {
      {.model = ModelTypeGeographic,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = KvUserDefined,
       .parms = {},
       .expected_substr = "+proj=latlong "},
      {.model = ModelTypeProjected,
       .map_sys = MapSys_UTM_North,
       .zone = 10,
       .ct_projection = CT_TransverseMercator,
       .parms = {},
       .expected_substr = "+proj=utm +zone=10 "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_TransverseMercator,
       .parms = {37.0, -122.0, 0.0, 0.0, 0.9996, 500000.0, 0.0},
       .expected_substr = "+proj=tmerc "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_ObliqueMercator_Laborde,
       .parms = {-18.9, 46.437, 18.9, 0.0, 0.9995, 400000.0, 800000.0},
       .expected_substr = "+proj=labrd "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_Mercator,
       .parms = {0.0, -90.0, 30.0, 0.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=merc +lat_ts=30."},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_Mercator,
       .parms = {0.0, -90.0, 0.0, 0.0, 0.999, 0.0, 0.0},
       .expected_substr = "+proj=merc ",
       .extra_substr = "+k=0.999"},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_CassiniSoldner,
       .parms = {10.4, -61.3, 0.0, 0.0, 1.0, 430000.0, 325000.0},
       .expected_substr = "+proj=cass "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_ObliqueStereographic,
       .parms = {52.156, 5.387, 0.0, 0.0, 0.9999079, 155000.0, 463000.0},
       .expected_substr = "+proj=stere ",
       .extra_substr = "+k=0.999908"},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_Stereographic,
       .parms = {45.0, -70.0, 0.0, 0.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=stere "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_PolarStereographic,
       .parms = {71.0, -96.0, 0.0, 0.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=stere +lat_0=90 "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_PolarStereographic,
       .parms = {-71.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=stere +lat_0=-90 "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_Equirectangular,
       .parms = {0.0, 10.0, 30.0, 0.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=eqc "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_Gnomonic,
       .parms = {40.0, -100.0, 0.0, 0.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=gnom "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_Orthographic,
       .parms = {40.0, -100.0, 0.0, 0.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=ortho "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_LambertAzimEqualArea,
       .parms = {52.0, 10.0, 0.0, 0.0, 1.0, 4321000.0, 3210000.0},
       .expected_substr = "+proj=laea "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_AzimuthalEquidistant,
       .parms = {13.4, 144.7, 0.0, 0.0, 1.0, 50000.0, 50000.0},
       .expected_substr = "+proj=aeqd "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_MillerCylindrical,
       .parms = {0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=mill ",
       .extra_substr = "+R_A "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_Polyconic,
       .parms = {0.0, -54.0, 0.0, 0.0, 1.0, 5000000.0, 10000000.0},
       .expected_substr = "+proj=poly "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_AlbersEqualArea,
       .parms = {29.5, 45.5, 23.0, -96.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=aea "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_EquidistantConic,
       .parms = {33.0, 45.0, 39.0, -96.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=eqdc "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_Robinson,
       .parms = {0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=robin "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_VanDerGrinten,
       .parms = {0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=vandg ",
       .extra_substr = "+R_A "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_Sinusoidal,
       .parms = {0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=sinu "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_LambertConfConic_2SP,
       .parms = {37.0, -120.0, 33.0, 45.0, 1.0, 2000.0, 500.0},
       .expected_substr = "+proj=lcc ",
       .extra_substr = "+lat_2=45."},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_LambertConfConic_1SP,
       .parms = {45.0, -90.0, 0.0, 0.0, 0.9999, 100.0, 200.0},
       .expected_substr = "+proj=lcc ",
       .extra_substr = "+k_0=0.9999"},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_CylindricalEqualArea,
       .parms = {30.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0},
       .expected_substr = "+proj=cea "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_NewZealandMapGrid,
       .parms = {-41.0, 173.0, 0.0, 0.0, 1.0, 2510000.0, 6023150.0},
       .expected_substr = "+proj=nzmg "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_ObliqueMercator,
       .parms = {4.0, 115.0, 53.3, 0.0, 0.99984, 590476.0, 442857.0},
       .expected_substr = "+proj=omerc "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_HotineObliqueMercatorAzimuthCenter,
       .parms = {46.9524055556, 7.4395833333, 90.0, 90.0, 1.0, 600000.0,
                 200000.0},
       .expected_substr = "+proj=somerc "},
      {.model = ModelTypeProjected,
       .map_sys = KvUserDefined,
       .zone = 0,
       .ct_projection = CT_HotineObliqueMercatorAzimuthCenter,
       .parms = {4.0, 102.25, 45.0, 45.0, 0.9998, 804671.0, 0.0},
       .expected_substr = "+proj=omerc ",
       .extra_substr = "+lonc=102.25"},
  };

  for (const auto& tc : kCases) {
    ScopedDefn defn(GTIFAllocDefn());
    ASSERT_NE(nullptr, defn);
    defn->DefnSet = 1;
    defn->Model = tc.model;
    defn->MapSys = tc.map_sys;
    defn->Zone = tc.zone;
    defn->CTProjection = tc.ct_projection;
    defn->Ellipsoid = Ellipse_WGS_84;
    defn->UOMLength = Linear_Meter;
    defn->UOMLengthInMeters = 1.0;
    std::copy(tc.parms.begin(), tc.parms.end(), defn->ProjParm);

    ScopedGtifMem proj4(GTIFGetProj4Defn(defn.get()));
    ASSERT_NE(nullptr, proj4);
    EXPECT_THAT(proj4.get(), HasSubstr(tc.expected_substr));
    if (!tc.extra_substr.empty()) {
      EXPECT_THAT(proj4.get(), HasSubstr(tc.extra_substr));
    }
  }

  // CT_TransvMercator_SouthOriented produces no +proj= clause and resets to "".
  ScopedDefn south_oriented(GTIFAllocDefn());
  ASSERT_NE(nullptr, south_oriented);
  south_oriented->DefnSet = 1;
  south_oriented->Model = ModelTypeProjected;
  south_oriented->MapSys = KvUserDefined;
  south_oriented->CTProjection = CT_TransvMercator_SouthOriented;
  south_oriented->Ellipsoid = Ellipse_WGS_84;
  south_oriented->UOMLength = Linear_Meter;
  ScopedGtifMem empty_so(GTIFGetProj4Defn(south_oriented.get()));
  ASSERT_NE(nullptr, empty_so);
  EXPECT_STREQ("", empty_so.get());
}

TEST(GeotiffProj4Test, Proj4FromAndToLatLongRoundTripUtmAndRejectUnsetDefn) {
  // 1. Round-trip two points between WGS84 lon/lat and UTM Zone 10N meters.
  ScopedDefn utm10(GTIFAllocDefn());
  ASSERT_NE(nullptr, utm10);
  utm10->DefnSet = 1;
  utm10->Model = ModelTypeProjected;
  utm10->MapSys = MapSys_UTM_North;
  utm10->Zone = 10;
  utm10->Ellipsoid = Ellipse_WGS_84;
  utm10->UOMLength = Linear_Meter;
  utm10->UOMLengthInMeters = 1.0;

  std::array<double, 2> x = {-123.0, -122.0};
  std::array<double, 2> y = {0.0, 37.5};
  ASSERT_EQ(1, GTIFProj4FromLatLong(utm10.get(), 2, x.data(), y.data()));
  EXPECT_NEAR(500000.0, x[0], 1e-6);
  EXPECT_NEAR(0.0, y[0], 1e-6);
  EXPECT_GT(x[1], 500000.0);
  EXPECT_GT(y[1], 4000000.0);

  ASSERT_EQ(1, GTIFProj4ToLatLong(utm10.get(), 2, x.data(), y.data()));
  EXPECT_NEAR(-123.0, x[0], 1e-6);
  EXPECT_NEAR(0.0, y[0], 1e-6);
  EXPECT_NEAR(-122.0, x[1], 1e-6);
  EXPECT_NEAR(37.5, y[1], 1e-6);

  // 2. Unset GTIFDefn yields empty PROJ string -> proj_create_crs_to_crs fails
  // (emits expected PROJ diagnostic to stderr on unset CRS).
  ScopedDefn unset(GTIFAllocDefn());
  ASSERT_NE(nullptr, unset);
  unset->DefnSet = 0;
  double bad_x = -122.0;
  double bad_y = 37.5;
  EXPECT_EQ(0, GTIFProj4FromLatLong(unset.get(), 1, &bad_x, &bad_y));
  EXPECT_EQ(0, GTIFProj4ToLatLong(unset.get(), 1, &bad_x, &bad_y));
}

// TODO(schwehr): Enable tests for additional projections if upstream removes
// `#ifdef notdef` around geotiff_proj4.c lines 400-771. Lines 1376 and 1433
// (`pszProjection == NULL`) are unreachable because GTIFGetProj4Defn always
// allocates at least "".

}  // namespace
}  // namespace libgeotiff
