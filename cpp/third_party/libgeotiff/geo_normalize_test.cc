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
// Tests EPSG / PROJ coordinate system normalization and formatting
// (geo_normalize.c).
//
// In google3/third_party/libgeotiff/geo_normalize.c:
// - `GDAL_COMPILATION` (`local_defines` on :libgeotiff) compiles out
//   `SetCSVFilenameHook` and makes `GTIFDeaccessCSV` a no-op (covered by
//   calling `GTIFDeaccessCSV()` below).
// - `GEO_NORMALIZE_DISABLE_TOWGS84` is not defined, so TOWGS84 parsing and
//   printing are active (verified by `#ifdef` guard below).
// - `//third_party/proj4` embeds `proj.db` via `-DPROJ4_EMBED_DB`.
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geo_normalize.c

#include "third_party/libgeotiff/geo_normalize.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"
#include "third_party/libgeotiff/geokeys.h"
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/geovalues.h"
#include "third_party/libgeotiff/google/geo_normalize_test_constants.h"
#include "third_party/libgeotiff/google/util/scoped_geotiff.h"
#include "third_party/proj4/proj/src/proj.h"

#ifdef GEO_NORMALIZE_DISABLE_TOWGS84
#error "Update geo_normalize_test.cc for disabled TOWGS84 support"
#endif

namespace libgeotiff {
namespace {

using ::testing::AllOf;
using ::testing::DoubleEq;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

struct ProjContextDeleter {
  void operator()(PJ_CONTEXT* ctx) const { proj_context_destroy(ctx); }
};

using ScopedProjContext = std::unique_ptr<PJ_CONTEXT, ProjContextDeleter>;

TEST(GeoNormalizeTest, LifecycleProjContextAndNoOpCsvHelpers) {
  // 1. Alloc/Free helpers accept valid and null pointers; GTIFDeaccessCSV is a
  // no-op under GDAL_COMPILATION.
  GTIFFreeDefn(nullptr);
  GTIFFreeMemory(nullptr);
  GTIFDeaccessCSV();

  ScopedDefn defn(GTIFAllocDefn());
  ASSERT_NE(nullptr, defn);
  EXPECT_EQ(0, defn->DefnSet);

  // 2. Lazy PROJ context creation and external attachment ownership rules.
  ScopedProjContext external_ctx(proj_context_create());
  ASSERT_NE(nullptr, external_ctx);

  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  int owned = -1;
  EXPECT_EQ(nullptr, GTIFGetPROJContext(handle.gtif.get(), 0, &owned));
  EXPECT_EQ(0, owned);

  void* created_ctx = GTIFGetPROJContext(handle.gtif.get(), 1, &owned);
  ASSERT_NE(nullptr, created_ctx);
  EXPECT_EQ(1, owned);
  EXPECT_EQ(created_ctx, GTIFGetPROJContext(handle.gtif.get(), 1, nullptr));

  // Attaching destroys the previously owned context and transfers ownership to
  // the caller. `handle` is declared after `external_ctx`, so GTIFFree always
  // runs before proj_context_destroy(external_ctx).
  GTIFAttachPROJContext(handle.gtif.get(), external_ctx.get());
  int attached_owned = -1;
  EXPECT_EQ(external_ctx.get(),
            GTIFGetPROJContext(handle.gtif.get(), 1, &attached_owned));
  EXPECT_EQ(0, attached_owned);

  // Re-attaching when own_pj_context == 0 skips destroying the prior external
  // context.
  GTIFAttachPROJContext(handle.gtif.get(), external_ctx.get());
}

TEST(GeoNormalizeTest, AngleConversionsAndDecToDmsHandleAllUnitsAndEdges) {
  // 1. GTIFAngleStringToDD across all safe UOM codes (never passing an
  // arbitrary unmapped code, which hits CPLAssert at geo_normalize.c:413).
  for (const auto& tc : kAngleStringCases) {
    EXPECT_NEAR(tc.expected_dd, GTIFAngleStringToDD(tc.input, tc.uom_angle),
                1e-9)
        << "input=" << tc.input << " uom=" << tc.uom_angle;
  }

  // 2. GTIFAngleToDD handles DMS packing within (-999.9, 999.9), pass-through
  // when >= 999.9 or KvUserDefined, and unit scaling.
  EXPECT_NEAR(123.5125, GTIFAngleToDD(123.3045, 9110), 1e-6);
  EXPECT_DOUBLE_EQ(1000.0, GTIFAngleToDD(1000.0, 9110));
  EXPECT_DOUBLE_EQ(12.5, GTIFAngleToDD(12.5, KvUserDefined));
  EXPECT_NEAR(180.0, GTIFAngleToDD(std::numbers::pi, 9101), 1e-9);
  EXPECT_DOUBLE_EQ(0.5, GTIFAngleToDD(30.0, 9103));
  EXPECT_DOUBLE_EQ(90.0, GTIFAngleToDD(100.0, 9105));

  // 3. GTIFDecToDMS formatting, out-of-range guards, and minute rollover.
  EXPECT_STREQ("", GTIFDecToDMS(361.0, "Lat", 2));
  EXPECT_STREQ("", GTIFDecToDMS(-361.0, "Long", 2));

  const std::string west = GTIFDecToDMS(-122.5, "Long", 2);
  EXPECT_THAT(west, HasSubstr("122d30'"));
  EXPECT_THAT(west, HasSubstr("W"));

  const std::string east = GTIFDecToDMS(10.25, "Long", 2);
  EXPECT_THAT(east, HasSubstr("10d15'"));
  EXPECT_THAT(east, HasSubstr("E"));

  const std::string south = GTIFDecToDMS(-33.5, "Lat", 2);
  EXPECT_THAT(south, HasSubstr("33d30'"));
  EXPECT_THAT(south, HasSubstr("S"));

  const std::string north_rollover = GTIFDecToDMS(45.999999, "Lat", 2);
  EXPECT_THAT(north_rollover, HasSubstr("46d 0'"));
  EXPECT_THAT(north_rollover, HasSubstr("N"));
}

TEST(GeoNormalizeTest, GetPcsInfoCoversUtmShortcutsDbAndInvalidCodes) {
  SCOPED_TRACE(::testing::Message() << "PROJ " << proj_info().version);
  ScopedProjContext ctx(proj_context_create());
  ASSERT_NE(nullptr, ctx);

  // 1. UTM fast-paths across all 5 built-in datums and N/S zones.
  for (const auto& tc : kUtmPcsCases) {
    char* raw_name = nullptr;
    int16_t proj_op = 0;
    int16_t uom = 0;
    int16_t gcs = 0;
    ASSERT_EQ(1, GTIFGetPCSInfo(tc.pcs, &raw_name, &proj_op, &uom, &gcs))
        << "pcs=" << tc.pcs;
    ScopedGtifMem name(raw_name);
    EXPECT_EQ(tc.expected_name, name.get());
    EXPECT_EQ(tc.expected_proj_op, proj_op);
    EXPECT_EQ(Linear_Meter, uom);
    EXPECT_EQ(tc.expected_geog_cs, gcs);
  }
  EXPECT_EQ(1, GTIFGetPCSInfoEx(ctx.get(), 32610, nullptr, nullptr, nullptr,
                                nullptr));

  // 2. Embedded proj.db lookup (EPSG:27582 NTF Paris / Lambert zone II) and
  // optional null out-pointers.
  {
    char* raw_name = nullptr;
    int16_t proj_op = 0;
    int16_t uom = 0;
    int16_t gcs = 0;
    ASSERT_EQ(1, GTIFGetPCSInfo(27582, &raw_name, &proj_op, &uom, &gcs));
    ScopedGtifMem name(raw_name);
    EXPECT_THAT(name.get(), HasSubstr("NTF"));
    EXPECT_EQ(18082, proj_op);
    EXPECT_EQ(Linear_Meter, uom);
    EXPECT_EQ(GCS_NTF_Paris, gcs);

    EXPECT_EQ(1, GTIFGetPCSInfoEx(ctx.get(), 27582, nullptr, nullptr, nullptr,
                                  nullptr));
  }

  // 3. Rejects KvUserDefined, non-projected CRS (EPSG:4326), and unknown codes.
  EXPECT_EQ(0, GTIFGetPCSInfoEx(ctx.get(), KvUserDefined, nullptr, nullptr,
                                nullptr, nullptr));
  EXPECT_EQ(
      0, GTIFGetPCSInfoEx(ctx.get(), 4326, nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(0, GTIFGetPCSInfoEx(ctx.get(), 999999, nullptr, nullptr, nullptr,
                                nullptr));
}

TEST(GeoNormalizeTest, GetGcsInfoCoversBuiltinsDbAndInvalidCodes) {
  SCOPED_TRACE(::testing::Message() << "PROJ " << proj_info().version);
  ScopedProjContext ctx(proj_context_create());
  ASSERT_NE(nullptr, ctx);

  struct GcsCase {
    int code;
    std::string_view expected_substr;
    int16_t expected_datum;
    int16_t expected_pm;
  };
  constexpr GcsCase kGcsCases[] = {
      {.code = GCS_NAD27,
       .expected_substr = "NAD27",
       .expected_datum = Datum_North_American_Datum_1927,
       .expected_pm = PM_Greenwich},
      {.code = GCS_NAD83,
       .expected_substr = "NAD83",
       .expected_datum = Datum_North_American_Datum_1983,
       .expected_pm = PM_Greenwich},
      {.code = GCS_WGS_84,
       .expected_substr = "WGS 84",
       .expected_datum = Datum_WGS84,
       .expected_pm = PM_Greenwich},
      {.code = GCS_WGS_72,
       .expected_substr = "WGS 72",
       .expected_datum = Datum_WGS72,
       .expected_pm = PM_Greenwich},
      {.code = GCS_EUREF89,
       .expected_substr = "ETRS89",
       .expected_datum = Datum_European_Reference_System_1989,
       .expected_pm = PM_Greenwich},
      {.code = GCS_NTF_Paris,
       .expected_substr = "NTF",
       .expected_datum = 6807,  // NTF Paris datum (Nouvelle Triangulation)
       .expected_pm = PM_Paris},
      {.code = 4979,  // WGS 84 Geographic 3D CRS
       .expected_substr = "WGS 84",
       .expected_datum = Datum_WGS84,
       .expected_pm = PM_Greenwich},
      {.code = 4978,  // WGS 84 Geocentric CRS
       .expected_substr = "WGS 84",
       .expected_datum = Datum_WGS84,
       .expected_pm = PM_Greenwich},
  };
  for (const auto& tc : kGcsCases) {
    char* raw_name = nullptr;
    int16_t datum = 0;
    int16_t pm = 0;
    int16_t uom_angle = 0;
    ASSERT_EQ(1, GTIFGetGCSInfo(tc.code, &raw_name, &datum, &pm, &uom_angle))
        << "gcs=" << tc.code;
    ScopedGtifMem name(raw_name);
    EXPECT_THAT(name.get(), HasSubstr(tc.expected_substr));
    EXPECT_EQ(tc.expected_datum, datum);
    EXPECT_EQ(tc.expected_pm, pm);
    EXPECT_NE(0, uom_angle);
  }
  EXPECT_EQ(1, GTIFGetGCSInfoEx(ctx.get(), GCS_WGS_84, nullptr, nullptr,
                                nullptr, nullptr));
  EXPECT_EQ(1, GTIFGetGCSInfoEx(ctx.get(), GCS_EUREF89, nullptr, nullptr,
                                nullptr, nullptr));
  EXPECT_EQ(0, GTIFGetGCSInfoEx(ctx.get(), KvUserDefined, nullptr, nullptr,
                                nullptr, nullptr));
  EXPECT_EQ(0, GTIFGetGCSInfoEx(ctx.get(), 32610, nullptr, nullptr, nullptr,
                                nullptr));
  EXPECT_EQ(0, GTIFGetGCSInfoEx(ctx.get(), 999999, nullptr, nullptr, nullptr,
                                nullptr));
}

TEST(GeoNormalizeTest, GetDatumInfoCoversBuiltinsDbAndInvalidCodes) {
  SCOPED_TRACE(::testing::Message() << "PROJ " << proj_info().version);
  ScopedProjContext ctx(proj_context_create());
  ASSERT_NE(nullptr, ctx);

  struct DatumCase {
    int code;
    int16_t expected_ellipsoid;
  };
  constexpr DatumCase kDatumCases[] = {
      {.code = Datum_North_American_Datum_1927,
       .expected_ellipsoid = Ellipse_Clarke_1866},
      {.code = Datum_North_American_Datum_1983,
       .expected_ellipsoid = Ellipse_GRS_1980},
      {.code = Datum_WGS84, .expected_ellipsoid = Ellipse_WGS_84},
      {.code = Datum_WGS72, .expected_ellipsoid = 7043},  // EPSG WGS 72
      {.code = Datum_European_Reference_System_1989,
       .expected_ellipsoid = Ellipse_GRS_1980},
      {.code = Datum_Nouvelle_Triangulation_Francaise,
       .expected_ellipsoid = Ellipse_Clarke_1880_IGN},
  };
  for (const auto& tc : kDatumCases) {
    char* raw_name = nullptr;
    int16_t ellipsoid = 0;
    ASSERT_EQ(1, GTIFGetDatumInfo(tc.code, &raw_name, &ellipsoid));
    ScopedGtifMem name(raw_name);
    EXPECT_EQ(tc.expected_ellipsoid, ellipsoid);
  }
  EXPECT_EQ(1, GTIFGetDatumInfoEx(ctx.get(), Datum_WGS84, nullptr, nullptr));
  EXPECT_EQ(1,
            GTIFGetDatumInfoEx(ctx.get(), Datum_European_Reference_System_1989,
                               nullptr, nullptr));
  EXPECT_EQ(0, GTIFGetDatumInfoEx(ctx.get(), KvUserDefined, nullptr, nullptr));
  EXPECT_EQ(
      0, GTIFGetDatumInfoEx(ctx.get(), 5103, nullptr, nullptr));  // NAVD88 VRF
  EXPECT_EQ(0, GTIFGetDatumInfoEx(ctx.get(), 999999, nullptr, nullptr));
}

TEST(GeoNormalizeTest, GetEllipsoidInfoCoversBuiltinsDbAndInvalidCodes) {
  SCOPED_TRACE(::testing::Message() << "PROJ " << proj_info().version);
  ScopedProjContext ctx(proj_context_create());
  ASSERT_NE(nullptr, ctx);

  constexpr int kEllipsoidCodes[] = {
      Ellipse_Clarke_1866,
      Ellipse_GRS_1980,
      Ellipse_WGS_84,
      7043,  // EPSG WGS 72 ellipsoid
      Ellipse_Airy_1830,
      Ellipse_International_1924,
  };
  for (int code : kEllipsoidCodes) {
    char* raw_name = nullptr;
    double semi_major = 0.0;
    double semi_minor = 0.0;
    ASSERT_EQ(1,
              GTIFGetEllipsoidInfo(code, &raw_name, &semi_major, &semi_minor));
    ScopedGtifMem name(raw_name);
    EXPECT_GT(semi_major, 6370000.0);
    EXPECT_GT(semi_minor, 6350000.0);
  }
  EXPECT_EQ(1, GTIFGetEllipsoidInfoEx(ctx.get(), Ellipse_WGS_84, nullptr,
                                      nullptr, nullptr));
  EXPECT_EQ(1, GTIFGetEllipsoidInfoEx(ctx.get(), Ellipse_Airy_1830, nullptr,
                                      nullptr, nullptr));
  EXPECT_EQ(0, GTIFGetEllipsoidInfoEx(ctx.get(), KvUserDefined, nullptr,
                                      nullptr, nullptr));
  EXPECT_EQ(
      0, GTIFGetEllipsoidInfoEx(ctx.get(), 999999, nullptr, nullptr, nullptr));
}

TEST(GeoNormalizeTest, GetPmInfoCoversGreenwichParisAndInvalidCodes) {
  SCOPED_TRACE(::testing::Message() << "PROJ " << proj_info().version);
  ScopedProjContext ctx(proj_context_create());
  ASSERT_NE(nullptr, ctx);

  char* raw_gw = nullptr;
  double offset_gw = -1.0;
  ASSERT_EQ(1, GTIFGetPMInfo(PM_Greenwich, &raw_gw, &offset_gw));
  ScopedGtifMem gw(raw_gw);
  EXPECT_STREQ("Greenwich", gw.get());
  EXPECT_DOUBLE_EQ(0.0, offset_gw);

  char* raw_paris = nullptr;
  double offset_paris = 0.0;
  ASSERT_EQ(1, GTIFGetPMInfo(PM_Paris, &raw_paris, &offset_paris));
  ScopedGtifMem paris(raw_paris);
  EXPECT_STREQ("Paris", paris.get());
  EXPECT_NEAR(2.3372291667, offset_paris, 1e-6);

  EXPECT_EQ(1, GTIFGetPMInfoEx(ctx.get(), PM_Greenwich, nullptr, nullptr));
  EXPECT_EQ(1, GTIFGetPMInfoEx(ctx.get(), PM_Paris, nullptr, nullptr));
  EXPECT_EQ(0, GTIFGetPMInfoEx(ctx.get(), KvUserDefined, nullptr, nullptr));
  EXPECT_EQ(0, GTIFGetPMInfoEx(ctx.get(), 999999, nullptr, nullptr));
}

TEST(GeoNormalizeTest, GetUomLengthInfoCoversBuiltinsDbAndInvalidCodes) {
  SCOPED_TRACE(::testing::Message() << "PROJ " << proj_info().version);
  ScopedProjContext ctx(proj_context_create());
  ASSERT_NE(nullptr, ctx);

  char* raw_m = nullptr;
  double in_m = 0.0;
  ASSERT_EQ(1, GTIFGetUOMLengthInfo(Linear_Meter, &raw_m, &in_m));
  ScopedGtifMem m_name(raw_m);
  EXPECT_STREQ("metre", m_name.get());
  EXPECT_DOUBLE_EQ(1.0, in_m);

  char* raw_ft = nullptr;
  double in_ft = 0.0;
  ASSERT_EQ(1, GTIFGetUOMLengthInfo(Linear_Foot, &raw_ft, &in_ft));
  ScopedGtifMem ft_name(raw_ft);
  EXPECT_STREQ("foot", ft_name.get());
  EXPECT_DOUBLE_EQ(0.3048, in_ft);

  char* raw_usft = nullptr;
  double in_usft = 0.0;
  ASSERT_EQ(1,
            GTIFGetUOMLengthInfo(Linear_Foot_US_Survey, &raw_usft, &in_usft));
  ScopedGtifMem usft_name(raw_usft);
  EXPECT_STREQ("US survey foot", usft_name.get());
  EXPECT_DOUBLE_EQ(12.0 / 39.37, in_usft);

  char* raw_km = nullptr;
  double in_km = 0.0;
  ASSERT_EQ(1, GTIFGetUOMLengthInfo(9036, &raw_km, &in_km));  // EPSG kilometre
  ScopedGtifMem km_name(raw_km);
  EXPECT_DOUBLE_EQ(1000.0, in_km);

  EXPECT_EQ(1,
            GTIFGetUOMLengthInfoEx(ctx.get(), Linear_Meter, nullptr, nullptr));
  EXPECT_EQ(1,
            GTIFGetUOMLengthInfoEx(ctx.get(), Linear_Foot, nullptr, nullptr));
  EXPECT_EQ(1, GTIFGetUOMLengthInfoEx(ctx.get(), Linear_Foot_US_Survey, nullptr,
                                      nullptr));
  EXPECT_EQ(1, GTIFGetUOMLengthInfoEx(ctx.get(), 9036, nullptr, nullptr));
  EXPECT_EQ(0,
            GTIFGetUOMLengthInfoEx(ctx.get(), KvUserDefined, nullptr, nullptr));
  EXPECT_EQ(0, GTIFGetUOMLengthInfoEx(ctx.get(), 999999, nullptr, nullptr));
}

TEST(GeoNormalizeTest, GetUomAngleInfoCoversBuiltinsDbAndInvalidCodes) {
  SCOPED_TRACE(::testing::Message() << "PROJ " << proj_info().version);
  ScopedProjContext ctx(proj_context_create());
  ASSERT_NE(nullptr, ctx);

  for (const auto& tc : kBuiltinUomAngleCases) {
    char* raw_name = nullptr;
    double in_deg = 0.0;
    ASSERT_EQ(1, GTIFGetUOMAngleInfo(tc.code, &raw_name, &in_deg));
    ScopedGtifMem name(raw_name);
    EXPECT_EQ(tc.name, name.get());
    EXPECT_NEAR(tc.in_degrees, in_deg, 1e-12);
  }

  char* raw_cmin = nullptr;
  double cmin_deg = 0.0;
  // EPSG:9112 is centesimal minute (0.01 grad = 0.009 degree).
  ASSERT_EQ(1, GTIFGetUOMAngleInfo(9112, &raw_cmin, &cmin_deg));
  ScopedGtifMem cmin_name(raw_cmin);
  EXPECT_NEAR(0.009, cmin_deg, 1e-9);

  EXPECT_EQ(1, GTIFGetUOMAngleInfoEx(ctx.get(), 9102, nullptr, nullptr));
  EXPECT_EQ(1, GTIFGetUOMAngleInfoEx(ctx.get(), 9112, nullptr, nullptr));
  EXPECT_EQ(0,
            GTIFGetUOMAngleInfoEx(ctx.get(), KvUserDefined, nullptr, nullptr));
  EXPECT_EQ(0, GTIFGetUOMAngleInfoEx(ctx.get(), 999999, nullptr, nullptr));
}

TEST(GeoNormalizeTest, GetProjTrfInfoCoversUtmDbConversionsAndInvalidCodes) {
  SCOPED_TRACE(::testing::Message() << "PROJ " << proj_info().version);
  ScopedProjContext ctx(proj_context_create());
  ASSERT_NE(nullptr, ctx);

  // 1. UTM North/South shortcuts with optional output pointers both set & null.
  constexpr int kUtmCodes[] = {Proj_UTM_zone_10N, Proj_UTM_zone_10S};
  for (int code : kUtmCodes) {
    char* raw_name = nullptr;
    int16_t method = 0;
    std::array<double, 7> parms = {};
    ASSERT_EQ(1, GTIFGetProjTRFInfo(code, &raw_name, &method, parms.data()));
    ScopedGtifMem name(raw_name);
    EXPECT_THAT(name.get(), HasSubstr("UTM zone 10"));
    EXPECT_EQ(9807, method);
    EXPECT_DOUBLE_EQ(-123.0, parms[1]);
    EXPECT_DOUBLE_EQ(code == Proj_UTM_zone_10N ? 0.0 : 10000000.0, parms[6]);

    EXPECT_EQ(1,
              GTIFGetProjTRFInfoEx(ctx.get(), code, nullptr, nullptr, nullptr));
  }

  // 2. Rejects KvUserDefined, missing codes, and non-conversion operations
  // (EPSG:1188 is a Helmert transformation rather than a PJ_TYPE_CONVERSION).
  EXPECT_EQ(0, GTIFGetProjTRFInfoEx(ctx.get(), KvUserDefined, nullptr, nullptr,
                                    nullptr));
  EXPECT_EQ(0,
            GTIFGetProjTRFInfoEx(ctx.get(), 999999, nullptr, nullptr, nullptr));
  EXPECT_EQ(0,
            GTIFGetProjTRFInfoEx(ctx.get(), 1188, nullptr, nullptr, nullptr));

  // 3. EPSG:19847 (Transverse Mercator 1SP / Brazilian TM zone) DB lookup.
  EXPECT_EQ(1,
            GTIFGetProjTRFInfoEx(ctx.get(), 19847, nullptr, nullptr, nullptr));
}

TEST(GeoNormalizeTest, GetDefnCoversAllProjTrfConversionCodes) {
  SCOPED_TRACE(::testing::Message() << "PROJ " << proj_info().version);

  for (int conv_code : kEpsgConversionCodes) {
    char* raw_name = nullptr;
    int16_t method = 0;
    std::array<double, 7> parms = {};
    ASSERT_EQ(1,
              GTIFGetProjTRFInfo(conv_code, &raw_name, &method, parms.data()))
        << "conv_code=" << conv_code;
    ScopedGtifMem name(raw_name);
    EXPECT_GT(method, 0);

    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                            ModelTypeProjected));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), ProjectionGeoKey, TYPE_SHORT, 1,
                            conv_code));
    ASSERT_EQ(1, GTIFWriteKeys(handle.gtif.get()));

    ScopedDefn defn(GTIFAllocDefn());
    ASSERT_NE(nullptr, defn);
    ASSERT_EQ(1, GTIFGetDefn(handle.gtif.get(), defn.get()))
        << "conv_code=" << conv_code;
    EXPECT_EQ(conv_code, defn->ProjCode);
    EXPECT_EQ(method, defn->Projection);
  }
}

TEST(GeoNormalizeTest, GetDefnNormalizesEmptyPcsJiffyOverridesAndUnits) {
  SCOPED_TRACE(::testing::Message() << "PROJ " << proj_info().version);
  // 1. Empty GTIF directory clears DefnSet and returns 0.
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ScopedDefn defn(GTIFAllocDefn());
    ASSERT_NE(nullptr, defn);
    defn->DefnSet = 1;
    EXPECT_EQ(0, GTIFGetDefn(handle.gtif.get(), defn.get()));
    EXPECT_EQ(0, defn->DefnSet);
  }

  // 2. PCS database normalization (27582 NTF Paris / Lambert zone II, in
  // grads).
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                            ModelTypeProjected));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), ProjectedCSTypeGeoKey,
                            TYPE_SHORT, 1, 27582));
    ASSERT_EQ(1, GTIFWriteKeys(handle.gtif.get()));

    ScopedDefn defn(GTIFAllocDefn());
    ASSERT_NE(nullptr, defn);
    ASSERT_EQ(1, GTIFGetDefn(handle.gtif.get(), defn.get()));
    EXPECT_EQ(1, defn->DefnSet);
    EXPECT_EQ(ModelTypeProjected, defn->Model);
    EXPECT_EQ(27582, defn->PCS);
    EXPECT_EQ(GCS_NTF_Paris, defn->GCS);
    EXPECT_EQ(6807, defn->Datum);
    EXPECT_EQ(Ellipse_Clarke_1880_IGN, defn->Ellipsoid);
    EXPECT_EQ(PM_Paris, defn->PM);
    EXPECT_EQ(9105, defn->UOMAngle);
    EXPECT_NEAR(0.9, defn->UOMAngleInDegrees, 1e-12);
    EXPECT_EQ(CT_LambertConfConic_1SP, defn->CTProjection);
  }

  // 3. PCS "jiffy fallback" when PCS code is not a ProjectedCRS in proj.db
  // (10101 maps to MapSys_State_Plane_27 -> Proj_Alabama_CS27_East).
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                            ModelTypeProjected));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), ProjectedCSTypeGeoKey,
                            TYPE_SHORT, 1, 10101));
    ASSERT_EQ(1, GTIFWriteKeys(handle.gtif.get()));

    ScopedDefn defn(GTIFAllocDefn());
    ASSERT_NE(nullptr, defn);
    ASSERT_EQ(1, GTIFGetDefn(handle.gtif.get(), defn.get()));
    EXPECT_EQ(10101, defn->ProjCode);
    EXPECT_EQ(GCS_NAD27, defn->GCS);
    EXPECT_EQ(MapSys_State_Plane_27, defn->MapSys);
  }

  // 4. Overrides: invalid/clamped GCS, custom ellipsoid + zero flattening
  // sphere, non-zero flattening, user-defined PM with angle conversion,
  // GeogTOWGS84GeoKey, GeogLinearUnitsGeoKey, and user-defined linear size.
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                            ModelTypeGeographic));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeographicTypeGeoKey, TYPE_SHORT,
                            1, 0));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogLinearUnitsGeoKey,
                            TYPE_SHORT, 1, Linear_Foot));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogAngularUnitsGeoKey,
                            TYPE_SHORT, 1, Angular_Grad));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogPrimeMeridianGeoKey,
                            TYPE_SHORT, 1, KvUserDefined));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogPrimeMeridianLongGeoKey,
                            TYPE_DOUBLE, 1, 10.0));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogSemiMajorAxisGeoKey,
                            TYPE_DOUBLE, 1, 6370000.0));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogInvFlatteningGeoKey,
                            TYPE_DOUBLE, 1, 0.0));
    std::array<double, 7> towgs84 = {1.0, 2.0, 3.0, 0.1, 0.2, 0.3, 1.5};
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogTOWGS84GeoKey, TYPE_DOUBLE,
                            7, towgs84.data()));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), ProjLinearUnitsGeoKey,
                            TYPE_SHORT, 1, KvUserDefined));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), ProjLinearUnitSizeGeoKey,
                            TYPE_DOUBLE, 1, 2.5));
    ASSERT_EQ(1, GTIFWriteKeys(handle.gtif.get()));

    ScopedDefn defn(GTIFAllocDefn());
    ASSERT_NE(nullptr, defn);
    ASSERT_EQ(1, GTIFGetDefn(handle.gtif.get(), defn.get()));
    EXPECT_EQ(KvUserDefined, defn->GCS);
    EXPECT_DOUBLE_EQ(6370000.0, defn->SemiMajor);
    EXPECT_DOUBLE_EQ(6370000.0, defn->SemiMinor);
    EXPECT_DOUBLE_EQ(9.0, defn->PMLongToGreenwich);  // 10 grad -> 9 deg
    EXPECT_EQ(7, defn->TOWGS84Count);
    EXPECT_THAT(defn->TOWGS84, ElementsAre(1.0, 2.0, 3.0, 0.1, 0.2, 0.3, 1.5));
    EXPECT_DOUBLE_EQ(2.5, defn->UOMLengthInMeters);
  }

  // 5. Non-zero inverse flattening + parameter unit normalization to meters
  // and decimal degrees inside GTIFFetchProjParms.
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                            ModelTypeProjected));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), ProjCoordTransGeoKey, TYPE_SHORT,
                            1, CT_TransverseMercator));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogAngularUnitsGeoKey,
                            TYPE_SHORT, 1, Angular_Grad));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), ProjLinearUnitsGeoKey,
                            TYPE_SHORT, 1, Linear_Foot));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogSemiMajorAxisGeoKey,
                            TYPE_DOUBLE, 1, 6378137.0));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogInvFlatteningGeoKey,
                            TYPE_DOUBLE, 1, 298.257223563));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), ProjNatOriginLatGeoKey,
                            TYPE_DOUBLE, 1, 50.0));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), ProjFalseEastingGeoKey,
                            TYPE_DOUBLE, 1, 1000.0));
    ASSERT_EQ(1, GTIFWriteKeys(handle.gtif.get()));

    ScopedDefn defn(GTIFAllocDefn());
    ASSERT_NE(nullptr, defn);
    ASSERT_EQ(1, GTIFGetDefn(handle.gtif.get(), defn.get()));
    EXPECT_NEAR(6356752.314245, defn->SemiMinor, 1e-4);
    EXPECT_DOUBLE_EQ(45.0, defn->ProjParm[0]);   // 50 grad -> 45 deg
    EXPECT_DOUBLE_EQ(304.8, defn->ProjParm[5]);  // 1000 ft -> 304.8 m
  }
}

TEST(GeoNormalizeTest, FetchProjParmsCoversAllCoordTransSwitchArms) {
  SCOPED_TRACE(::testing::Message() << "PROJ " << proj_info().version);
  for (const auto& tc : kCtProjectionParmsCases) {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                            ModelTypeProjected));
    ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), ProjCoordTransGeoKey, TYPE_SHORT,
                            1, tc.ct_projection));
    for (const CtParmOverride& input :
         absl::MakeConstSpan(tc.inputs).first(tc.input_count)) {
      ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), input.key, TYPE_DOUBLE, 1,
                              input.value));
    }
    ASSERT_EQ(1, GTIFWriteKeys(handle.gtif.get()));

    ScopedDefn defn(GTIFAllocDefn());
    ASSERT_NE(nullptr, defn);
    ASSERT_EQ(1, GTIFGetDefn(handle.gtif.get(), defn.get()))
        << "ct_projection=" << tc.ct_projection
        << " input_count=" << tc.input_count;
    EXPECT_EQ(tc.ct_projection, defn->CTProjection);
    EXPECT_EQ(7, defn->nParms);
    EXPECT_EQ(tc.expected_parm_id0, defn->ProjParmId[0]);
    EXPECT_THAT(defn->ProjParm[0], DoubleEq(tc.expected_parm0));
    EXPECT_EQ(tc.expected_parm_id1, defn->ProjParmId[1]);
    EXPECT_THAT(defn->ProjParm[1], DoubleEq(tc.expected_parm1));
  }
}

TEST(GeoNormalizeTest, PrintDefnAndPrintDefnExFormatPopulatedAndUnknownDefs) {
  SCOPED_TRACE(::testing::Message() << "PROJ " << proj_info().version);
  // 1. Unset GTIFDefn prints "No GeoKeys found.".
  {
    ScopedDefn unset(GTIFAllocDefn());
    ASSERT_NE(nullptr, unset);
    unset->DefnSet = 0;
    const std::string text =
        CaptureFileOutput([&](FILE* fp) { GTIFPrintDefn(unset.get(), fp); });
    EXPECT_STREQ("No GeoKeys found.\n", text.c_str());
  }

  // 2. Populated PCS + custom Azimuth parameter (`?` axis) + TOWGS84 + known
  // UOMLength via GTIFPrintDefnEx.
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ScopedDefn defn(GTIFAllocDefn());
    ASSERT_NE(nullptr, defn);
    defn->DefnSet = 1;
    defn->PCS = 27582;
    defn->ProjCode = 18082;
    defn->CTProjection = CT_HotineObliqueMercatorAzimuthCenter;
    defn->nParms = 7;
    defn->ProjParmId[0] = ProjCenterLatGeoKey;
    defn->ProjParm[0] = 46.9;
    defn->ProjParmId[1] = ProjCenterLongGeoKey;
    defn->ProjParm[1] = 7.4;
    defn->ProjParmId[2] =
        ProjAzimuthAngleGeoKey;  // neither Long nor Lat -> "?"
    defn->ProjParm[2] = 90.0;
    defn->ProjParmId[3] = 0;  // skipped unused slot
    defn->ProjParmId[4] = ProjScaleAtCenterGeoKey;
    defn->ProjParm[4] = 1.0;
    defn->ProjParmId[5] = ProjFalseEastingGeoKey;
    defn->ProjParm[5] = 600000.0;
    defn->GCS = GCS_WGS_84;
    defn->Datum = Datum_WGS84;
    defn->Ellipsoid = Ellipse_WGS_84;
    defn->SemiMajor = 6378137.0;
    defn->SemiMinor = 6356752.314;
    defn->PM = PM_Greenwich;
    defn->TOWGS84Count = 3;
    defn->TOWGS84[0] = 10.0;
    defn->TOWGS84[1] = 20.0;
    defn->TOWGS84[2] = 30.0;
    defn->UOMLength = Linear_Meter;
    defn->UOMLengthInMeters = 1.0;

    // Code 6807 above in GetGcsInfo is NTF Paris datum; here WGS 84 uses 6326.

    const std::string text = CaptureFileOutput(
        [&](FILE* fp) { GTIFPrintDefnEx(handle.gtif.get(), defn.get(), fp); });
    EXPECT_THAT(
        text,
        AllOf(HasSubstr("PCS = 27582"), HasSubstr("Projection = 18082"),
              HasSubstr("Projection Method:"),
              HasSubstr("ProjCenterLatGeoKey:"),
              HasSubstr("ProjCenterLongGeoKey:"),
              HasSubstr("ProjAzimuthAngleGeoKey: 90.000000 ( 90d 0' 0.00\"N)"),
              HasSubstr("ProjScaleAtCenterGeoKey:"),
              HasSubstr("ProjFalseEastingGeoKey:"), HasSubstr("GCS: 4326/"),
              HasSubstr("Datum: 6326/"), HasSubstr("Ellipsoid: 7030/"),
              HasSubstr("Prime Meridian: 8901/Greenwich"),
              HasSubstr("TOWGS84: 10,20,30"),
              HasSubstr("Projection Linear Units: 9001/metre")));
  }

  // 3. Unknown codes exercise "(unknown)" fallbacks and KvUserDefined UOM.
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);
    ScopedDefn unknown(GTIFAllocDefn());
    ASSERT_NE(nullptr, unknown);
    unknown->DefnSet = 1;
    unknown->PCS = 9999;
    unknown->ProjCode = 9999;
    unknown->CTProjection = 9999;
    unknown->nParms = 1;
    unknown->ProjParmId[0] = 65000;
    unknown->GCS = 9999;
    unknown->Datum = 9999;
    unknown->Ellipsoid = 9999;
    unknown->PM = 9999;
    unknown->UOMLength = KvUserDefined;
    unknown->UOMLengthInMeters = 2.0;

    const std::string user_uom_text = CaptureFileOutput([&](FILE* fp) {
      GTIFPrintDefnEx(handle.gtif.get(), unknown.get(), fp);
    });
    EXPECT_THAT(
        user_uom_text,
        AllOf(HasSubstr("PCS = 9999 (name unknown)"),
              HasSubstr("Projection = 9999 ()"),
              HasSubstr("GCS: 9999/(unknown)"),
              HasSubstr("Datum: 9999/(unknown)"),
              HasSubstr("Ellipsoid: 9999/(unknown)"),
              HasSubstr("Prime Meridian: 9999/(unknown)"),
              HasSubstr("Projection Linear Units: User-Defined (2.000000m)")));

    unknown->UOMLength = 9999;
    const std::string unknown_uom_text = CaptureFileOutput([&](FILE* fp) {
      GTIFPrintDefnEx(handle.gtif.get(), unknown.get(), fp);
    });
    EXPECT_THAT(unknown_uom_text,
                HasSubstr("Projection Linear Units: 9999/(unknown)"));
  }
}

// TODO(schwehr): Uncovered lines in geo_normalize.c fall into five groups:
// - Line 162: GTIFGetProjTRFInfoEx UTM default switch branch.
// - Lines 217-281, 515-585, 696-697, 770-771, 879-891, 1608, 1630-1631, 2425:
//   PROJ object-creation error unwinds and allocation/context OOM guards.
// - Lines 1178, 1303-1314, 1353-1359: EPSGProjMethodToCTProjMethod /
//   SetGTParamIds arms for EPSG 9814 (CT_ObliqueMercator_Rosenmund), EPSG 9805
//   (Mercator_2SP), and CT_SwissObliqueCylindrical.
// - Lines 2700-2720: offline UTM fallback bypassed when embedded `proj.db`
//   resolves `Proj_UTM_*` directly.
// - Lines 2843, 2854: unreachable `== NULL` guards after GTIFValueNameEx and
//   GTIFKeyName (both always return non-null strings).

}  // namespace
}  // namespace libgeotiff
