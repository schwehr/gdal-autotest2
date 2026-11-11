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
// Unit tests for ISO-19111 C API wrappers in proj/src/iso19111/c_api.cpp.
// Note: in google3 default non-GCE builds, proj.db and non-skipped gridshift
// files are embedded in memory and networking is disabled by default.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/c_api.cpp

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/google/util/temp_file.h"
#include "third_party/proj4/proj/include/proj/internal/io_internal.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_constants.h"
#include "third_party/proj4/proj/src/proj_internal.h"
#include "third_party/sqlite/sqlite3.h"

namespace proj {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::IsNull;
using ::testing::Not;
using ::testing::NotNull;
using ::testing::StrEq;

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kGradToRad = std::numbers::pi / 200.0;
constexpr double kFootToMetre = 0.3048;

constexpr const char* kTimeCrsMeasureWkt =
    "TIMECRS[\"UTC Time\",TDATUM[\"UTC\",TIMEORIGIN[1970-01-01]],"
    "CS[TemporalMeasure,1],AXIS[\"t\",future,ORDER[1],TIMEUNIT[\"second\",1]]]";

constexpr const char* kTimeCrsCountWkt =
    "TIMECRS[\"FrameCount\",TDATUM[\"Origin\",TIMEORIGIN[1970-01-01]],"
    "CS[TemporalCount,1],AXIS[\"c\",future,ORDER[1]]]";

constexpr const char* kTimeCrsDateTimeWkt =
    "TIMECRS[\"ISO Date\",TDATUM[\"Gregorian\",TIMEORIGIN[1970-01-01]],"
    "CS[Temporal,1],AXIS[\"t\",future,ORDER[1]]]";

constexpr const char* kTemporalDatumWkt =
    "TDATUM[\"POSIX\",TIMEORIGIN[1970-01-01]]";

constexpr const char* kParametricCrsWkt =
    "PARAMETRICCRS[\"Pressure\",PDATUM[\"MSL\"],"
    "CS[parametric,1],AXIS[\"p\",up,ORDER[1],PARAMETRICUNIT[\"hPa\",100]]]";

constexpr const char* kParametricDatumWkt = "PDATUM[\"Mean Sea Level\"]";

constexpr const char* kOrdinalEngCrsWkt =
    "ENGCRS[\"Grid\",EDATUM[\"Local\"],CS[ordinal,2],"
    "AXIS[\"i\",unspecified,ORDER[1]],AXIS[\"j\",unspecified,ORDER[2]]]";

constexpr const char* kSphericalGeodCrsWkt =
    "GEODCRS[\"SphGeod\","
    "DATUM[\"WGS 84\",ELLIPSOID[\"WGS 84\",6378137,298.257223563]],"
    "CS[spherical,3],"
    "AXIS[\"latitude\",north,ORDER[1],ANGLEUNIT[\"degree\",0.0174532925199433]]"
    ","
    "AXIS[\"longitude\",east,ORDER[2],ANGLEUNIT[\"degree\",0.0174532925199433]]"
    ","
    "AXIS[\"radius\",up,ORDER[3],LENGTHUNIT[\"metre\",1]]]";

constexpr const char* kPointMotionOpWkt =
    "POINTMOTIONOPERATION[\"TestPM\","
    "SOURCECRS[GEOGCRS[\"WGS 84\","
    "DATUM[\"WGS 84\",ELLIPSOID[\"WGS 84\",6378137,298.257223563]],"
    "CS[ellipsoidal,3],"
    "AXIS[\"lat\",north,ORDER[1],ANGLEUNIT[\"degree\",0.0174532925199433]],"
    "AXIS[\"lon\",east,ORDER[2],ANGLEUNIT[\"degree\",0.0174532925199433]],"
    "AXIS[\"h\",up,ORDER[3],LENGTHUNIT[\"metre\",1]]]],"
    "METHOD[\"Point motion (geog3D domain) using NEU velocity grid "
    "(NTv2_Vel)\"],"
    "PARAMETERFILE[\"Point motion velocity grid file\",\"velocity_test.tif\"]]";

constexpr const char* kCoordMetaWgs84NoEpochWkt =
    "COORDINATEMETADATA[GEOGCRS[\"WGS 84\","
    "DATUM[\"WGS 84\",ELLIPSOID[\"WGS 84\",6378137,298.257223563]],"
    "CS[ellipsoidal,2],AXIS[\"lat\",north,ORDER[1]],AXIS[\"lon\",east,ORDER[2]]"
    ","
    "ANGLEUNIT[\"degree\",0.0174532925199433]]]";

size_t CountStringList(PROJ_STRING_LIST list) {
  if (list == nullptr) {
    return 0;
  }
  size_t count = 0;
  while (list[count] != nullptr) {
    ++count;
  }
  return count;
}

bool HasString(PROJ_STRING_LIST list, std::string_view target) {
  if (list == nullptr) {
    return false;
  }
  for (size_t i = 0; list[i] != nullptr; ++i) {
    if (std::string_view(list[i]) == target) {
      return true;
    }
  }
  return false;
}

std::string CopyAsWkt(PJ_CONTEXT* ctx, const PJ* obj, PJ_WKT_TYPE type,
                      const char* const* options = nullptr) {
  const char* wkt = proj_as_wkt(ctx, obj, type, options);
  return wkt != nullptr ? std::string(wkt) : std::string();
}

TEST(Iso19111CApiTest, AutocloseDatabasePathAndMetadata) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // proj_context_set_autoclose_database is a no-op retained for ABI compat.
  proj_context_set_autoclose_database(nullptr, 1);
  proj_context_set_autoclose_database(ctx.get(), 0);

  const char* db_path = proj_context_get_database_path(ctx.get());
  ASSERT_THAT(db_path, NotNull());

  EXPECT_THAT(proj_context_get_database_metadata(ctx.get(), nullptr), IsNull());
  const char* layout_major = proj_context_get_database_metadata(
      ctx.get(), "DATABASE.LAYOUT.VERSION.MAJOR");
  ASSERT_THAT(layout_major, NotNull());
  EXPECT_THAT(std::string_view(layout_major), Not(IsEmpty()));

  const char* epsg_version =
      proj_context_get_database_metadata(ctx.get(), "EPSG.VERSION");
  EXPECT_THAT(epsg_version, NotNull());

  EXPECT_THAT(
      proj_context_get_database_metadata(ctx.get(), "NO_SUCH_METADATA_KEY"),
      IsNull());

  ScopedStringList structure(
      proj_context_get_database_structure(ctx.get(), nullptr));
  ASSERT_THAT(structure.get(), NotNull());
  EXPECT_THAT(CountStringList(structure.get()), Gt(5u));
}

TEST(Iso19111CApiTest, SetDatabasePathRestoreAndAuxiliaryDatabase) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // Initialize cpp_context before testing failure-rollback branch.
  ASSERT_THAT(proj_context_get_database_path(ctx.get()), NotNull());

  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(0,
              proj_context_set_database_path(
                  ctx.get(), "/nonexistent/invalid_proj.db", nullptr, nullptr));
  }
  // Previous valid context was restored on failure.
  ScopedPj verify_crs(proj_create_from_database(ctx.get(), "EPSG", "4326",
                                                PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(verify_crs.get(), NotNull());

  // Build an on-disk SQLite auxiliary database using PROJ's schema and insert
  // statements, then attach it via proj_context_set_database_path.
  TempFile aux_db("iso19111_aux_test.db");
  ScopedStringList stmts(
      proj_context_get_database_structure(ctx.get(), nullptr));
  ASSERT_THAT(stmts.get(), NotNull());

  ScopedPj custom_utm(proj_create(
      ctx.get(),
      "+proj=utm +zone=31 +datum=WGS84 +units=m +no_defs +type=crs"));
  ASSERT_THAT(custom_utm.get(), NotNull());
  ScopedPj custom_named(
      proj_alter_name(ctx.get(), custom_utm.get(), "Google Test UTM 31N"));
  ASSERT_THAT(custom_named.get(), NotNull());

  ScopedInsertSession session(ctx.get(),
                              proj_insert_object_session_create(ctx.get()));
  ASSERT_TRUE(session);
  const char* const allowed_auths[] = {"EPSG", nullptr};
  ScopedStringList inserts(proj_get_insert_statements(
      ctx.get(), session.get(), custom_named.get(), "GOOGLE_TEST", "90031",
      /*numeric_codes=*/1, allowed_auths, nullptr));
  ASSERT_THAT(inserts.get(), NotNull());

  sqlite3* raw_db = nullptr;
  ASSERT_EQ(SQLITE_OK, sqlite3_open(aux_db.c_str(), &raw_db));
  absl::Cleanup close_db = [&raw_db] {
    if (raw_db != nullptr) {
      sqlite3_close(raw_db);
      raw_db = nullptr;
    }
  };
  auto exec_sql = [raw_db](const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(raw_db, sql, nullptr, nullptr, &err);
    std::string err_str = err != nullptr ? err : "";
    sqlite3_free(err);
    EXPECT_EQ(SQLITE_OK, rc) << "SQL error: " << err_str << " for: " << sql;
  };
  exec_sql("BEGIN;");
  for (size_t i = 0; stmts.get()[i] != nullptr; ++i) {
    exec_sql(stmts.get()[i]);
  }
  for (size_t i = 0; inserts.get()[i] != nullptr; ++i) {
    exec_sql(inserts.get()[i]);
  }
  exec_sql("COMMIT;");
  std::move(close_db).Invoke();

  const char* const aux_paths[] = {aux_db.c_str(), nullptr};
  EXPECT_EQ(1, proj_context_set_database_path(ctx.get(), nullptr, aux_paths,
                                              nullptr));
  ScopedPj loaded_from_aux(proj_create_from_database(
      ctx.get(), "GOOGLE_TEST", "90031", PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(loaded_from_aux.get(), NotNull());
  EXPECT_THAT(proj_get_name(loaded_from_aux.get()),
              StrEq("Google Test UTM 31N"));

  // Reset database path without aux DB.
  EXPECT_EQ(
      1, proj_context_set_database_path(ctx.get(), nullptr, nullptr, nullptr));
}

TEST(Iso19111CApiTest, InsertSessionSuggestCodeAndInsertStatements) {
  ScopedContext ctx = MakeContext();
  ScopedContext other_ctx = MakeContext();
  ScopedContext mismatch_ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ASSERT_THAT(other_ctx.get(), NotNull());
  ASSERT_THAT(mismatch_ctx.get(), NotNull());

  // Destroying nullptr session is a no-op; mismatched context logs an error.
  proj_insert_object_session_destroy(ctx.get(), nullptr);
  PJ_INSERT_SESSION* raw_session =
      proj_insert_object_session_create(mismatch_ctx.get());
  ASSERT_THAT(raw_session, NotNull());
  {
    // Starting a second session on the same context before stopping throws.
    ScopedLogSuppressor suppress(mismatch_ctx.get());
    EXPECT_THAT(proj_insert_object_session_create(mismatch_ctx.get()),
                IsNull());
  }
  {
    ScopedLogCapture capture(other_ctx.get(), PJ_LOG_ERROR);
    proj_insert_object_session_destroy(other_ctx.get(), raw_session);
    EXPECT_TRUE(capture.ContainsMessage("context different"));
  }
  // Create and close a valid insert session on ctx.
  {
    ScopedInsertSession valid_session(
        ctx.get(), proj_insert_object_session_create(ctx.get()));
    EXPECT_TRUE(valid_session);
  }

  ScopedPj proj_crs(proj_create(
      ctx.get(), "+proj=merc +lon_0=15 +datum=WGS84 +units=m +type=crs"));
  ASSERT_THAT(proj_crs.get(), NotNull());
  ScopedPj classic_pj(pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_THAT(classic_pj.get(), NotNull());
  ScopedPj proj_op(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_THAT(proj_op.get(), NotNull());
  ScopedPj time_crs(proj_create(ctx.get(), kTimeCrsMeasureWkt));
  ASSERT_THAT(time_crs.get(), NotNull());

  // proj_suggests_code_for happy paths (alphanumeric, numeric, and
  // TemporalCRS).
  ScopedProjString alpha_code(proj_suggests_code_for(
      ctx.get(), proj_crs.get(), "MYAUTH", /*numeric_code=*/0, nullptr));
  ASSERT_THAT(alpha_code.get(), NotNull());
  EXPECT_THAT(std::string_view(alpha_code.get()), Not(IsEmpty()));

  ScopedProjString numeric_code(proj_suggests_code_for(
      ctx.get(), proj_crs.get(), "MYAUTH", /*numeric_code=*/1, nullptr));
  ASSERT_THAT(numeric_code.get(), NotNull());
  EXPECT_THAT(std::string_view(numeric_code.get()), Not(IsEmpty()));

  ScopedProjString time_code(proj_suggests_code_for(
      ctx.get(), time_crs.get(), "MYAUTH", /*numeric_code=*/0, nullptr));
  ASSERT_THAT(time_code.get(), NotNull());
  EXPECT_THAT(time_code.get(), StrEq("UTC_TIME"));

  // proj_suggests_code_for error branches.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(
        proj_suggests_code_for(ctx.get(), nullptr, "MYAUTH", 0, nullptr),
        IsNull());
    EXPECT_THAT(
        proj_suggests_code_for(ctx.get(), proj_crs.get(), nullptr, 0, nullptr),
        IsNull());
    EXPECT_THAT(proj_suggests_code_for(ctx.get(), classic_pj.get(), "MYAUTH", 0,
                                       nullptr),
                IsNull());
    EXPECT_THAT(
        proj_suggests_code_for(ctx.get(), proj_op.get(), "MYAUTH", 0, nullptr),
        IsNull());
  }

  // proj_get_insert_statements with session=nullptr (TempSessionHolder).
  ScopedStringList stmts_no_session(proj_get_insert_statements(
      ctx.get(), nullptr, proj_crs.get(), "MYAUTH", "MERC15",
      /*numeric_codes=*/0, nullptr, nullptr));
  ASSERT_THAT(stmts_no_session.get(), NotNull());
  EXPECT_THAT(CountStringList(stmts_no_session.get()), Gt(0u));

  // Error branches for proj_get_insert_statements.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_get_insert_statements(ctx.get(), nullptr, nullptr,
                                           "MYAUTH", "1", 0, nullptr, nullptr),
                IsNull());
    EXPECT_THAT(proj_get_insert_statements(ctx.get(), nullptr, proj_crs.get(),
                                           nullptr, "1", 0, nullptr, nullptr),
                IsNull());
    EXPECT_THAT(
        proj_get_insert_statements(ctx.get(), nullptr, proj_crs.get(), "MYAUTH",
                                   nullptr, 0, nullptr, nullptr),
        IsNull());
    EXPECT_THAT(proj_get_insert_statements(ctx.get(), nullptr, classic_pj.get(),
                                           "MYAUTH", "1", 0, nullptr, nullptr),
                IsNull());
    EXPECT_THAT(proj_get_insert_statements(ctx.get(), nullptr, time_crs.get(),
                                           "MYAUTH", "1", 0, nullptr, nullptr),
                IsNull());
  }
}

TEST(Iso19111CApiTest, GuessWktDialectAndCreateFromWktBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(PJ_GUESSED_NOT_WKT,
              proj_context_guess_wkt_dialect(ctx.get(), nullptr));
  }
  EXPECT_EQ(PJ_GUESSED_NOT_WKT,
            proj_context_guess_wkt_dialect(ctx.get(), "+proj=longlat"));
  EXPECT_EQ(
      PJ_GUESSED_WKT2_2019,
      proj_context_guess_wkt_dialect(
          ctx.get(),
          "GEOGCRS[\"WGS 84\",DATUM[\"WGS 84\",ELLIPSOID[\"WGS 84\",6378137,"
          "298.257223563]],CS[ellipsoidal,2],AXIS[\"lat\",north,ORDER[1]],"
          "AXIS[\"lon\",east,ORDER[2]],ANGLEUNIT[\"degree\",0.0174532925199]"
          "]"));
  EXPECT_EQ(
      PJ_GUESSED_WKT2_2015,
      proj_context_guess_wkt_dialect(
          ctx.get(),
          "GEODCRS[\"WGS 84\",DATUM[\"WGS 84\",ELLIPSOID[\"WGS 84\",6378137,"
          "298.257223563]],CS[ellipsoidal,2],AXIS[\"lat\",north,ORDER[1]],"
          "AXIS[\"lon\",east,ORDER[2]],ANGLEUNIT[\"degree\",0.0174532925199]"
          "]"));
  EXPECT_EQ(
      PJ_GUESSED_WKT1_GDAL,
      proj_context_guess_wkt_dialect(
          ctx.get(),
          "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,"
          "298.257223563]],PRIMEM[\"Greenwich\",0],"
          "UNIT[\"degree\",0.0174532925199433],AUTHORITY[\"EPSG\",\"4326\"]]"));
  EXPECT_EQ(
      PJ_GUESSED_WKT1_ESRI,
      proj_context_guess_wkt_dialect(
          ctx.get(),
          "GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\","
          "6378137.0,298.257223563]],PRIMEM[\"Greenwich\",0.0],"
          "UNIT[\"Degree\",0.0174532925199433]]"));

  // Missing input & unknown option.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(
        proj_create_from_wkt(ctx.get(), nullptr, nullptr, nullptr, nullptr),
        IsNull());
    const char* const bad_opts[] = {"UNKNOWN_OPT=YES", nullptr};
    EXPECT_THAT(proj_create_from_wkt(ctx.get(), kTimeCrsMeasureWkt, bad_opts,
                                     nullptr, nullptr),
                IsNull());
  }

  // Options STRICT=YES/NO and UNSET_IDENTIFIERS_IF_INCOMPATIBLE_DEF=NO.
  const char* const strict_unset_opts[] = {
      "STRICT=NO", "UNSET_IDENTIFIERS_IF_INCOMPATIBLE_DEF=NO", nullptr};
  ScopedPj wgs84_4326(proj_create_from_database(ctx.get(), "EPSG", "4326",
                                                PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(wgs84_4326.get(), NotNull());
  const std::string wgs84_wkt =
      CopyAsWkt(ctx.get(), wgs84_4326.get(), PJ_WKT2_2019);
  {
    PROJ_STRING_LIST raw_warn = nullptr;
    PROJ_STRING_LIST raw_err = nullptr;
    ScopedPj parsed(proj_create_from_wkt(
        ctx.get(), wgs84_wkt.c_str(), strict_unset_opts, &raw_warn, &raw_err));
    ScopedStringList warn_guard(raw_warn);
    ScopedStringList err_guard(raw_err);
    ASSERT_THAT(parsed.get(), NotNull());
    EXPECT_EQ(0u, CountStringList(warn_guard.get()));
    EXPECT_EQ(0u, CountStringList(err_guard.get()));
  }

  // out_warnings for DerivedCRS with unrecognized method & parameter.
  constexpr const char* kDerivedWarnWkt =
      "PROJCRS[\"CustomProj\","
      "BASEGEOGCRS[\"WGS 84\","
      "DATUM[\"WGS 84\",ELLIPSOID[\"WGS 84\",6378137,298.257223563]],"
      "PRIMEM[\"Greenwich\",0]],"
      "CONVERSION[\"UnknownConv\","
      "METHOD[\"UnknownMethodName\"],"
      "PARAMETER[\"WeirdParam\",42,LENGTHUNIT[\"metre\",1]]],"
      "CS[Cartesian,2],"
      "AXIS[\"E\",east,ORDER[1]],AXIS[\"N\",north,ORDER[2]],"
      "LENGTHUNIT[\"metre\",1]]";
  {
    PROJ_STRING_LIST raw_warn = nullptr;
    PROJ_STRING_LIST raw_err = nullptr;
    ScopedPj derived_warn(proj_create_from_wkt(ctx.get(), kDerivedWarnWkt,
                                               nullptr, &raw_warn, &raw_err));
    ScopedStringList warn_guard(raw_warn);
    ScopedStringList err_guard(raw_err);
    ASSERT_THAT(derived_warn.get(), NotNull());
    EXPECT_THAT(CountStringList(warn_guard.get()), Gt(0u));
  }

  // out_warnings for SingleOperation with unrecognized method & string param.
  constexpr const char* kSingleOpWarnWkt =
      "CONVERSION[\"UnknownOp\","
      "METHOD[\"UnknownOpMethod\"],"
      "PARAMETER[\"text_param\",\"string_value\"]]";
  {
    PROJ_STRING_LIST raw_warn = nullptr;
    PROJ_STRING_LIST raw_err = nullptr;
    ScopedPj op_warn(proj_create_from_wkt(ctx.get(), kSingleOpWarnWkt, nullptr,
                                          &raw_warn, &raw_err));
    ScopedStringList warn_guard(raw_warn);
    ScopedStringList err_guard(raw_err);
    ASSERT_THAT(op_warn.get(), NotNull());
    EXPECT_THAT(CountStringList(warn_guard.get()), Gt(0u));

    const char* str_val = nullptr;
    const char* param_name = nullptr;
    EXPECT_EQ(
        1, proj_coordoperation_get_param(
               ctx.get(), op_warn.get(), 0, &param_name, nullptr, nullptr,
               nullptr, &str_val, nullptr, nullptr, nullptr, nullptr, nullptr));
    EXPECT_THAT(param_name, StrEq("text_param"));
    EXPECT_THAT(str_val, StrEq("string_value"));
  }

  // out_grammar_errors on WKT1 missing PRIMEM under STRICT=NO.
  constexpr const char* kIncompleteWkt1 =
      "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
      "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
      "UNIT[\"degree\",0.0174532925199433]]";
  {
    PROJ_STRING_LIST raw_warn = nullptr;
    PROJ_STRING_LIST raw_err = nullptr;
    ScopedPj wkt1_incomplete(proj_create_from_wkt(
        ctx.get(), kIncompleteWkt1, nullptr, &raw_warn, &raw_err));
    ScopedStringList warn_guard(raw_warn);
    ScopedStringList err_guard(raw_err);
    ASSERT_THAT(wkt1_incomplete.get(), NotNull());
    EXPECT_THAT(CountStringList(err_guard.get()), Gt(0u));
  }

  // Exception path during WKT parsing fills out_grammar_errors when provided,
  // or logs to ctx when out_grammar_errors == nullptr.
  {
    const char* const strict_yes[] = {"STRICT=YES", nullptr};
    PROJ_STRING_LIST raw_err = nullptr;
    EXPECT_THAT(proj_create_from_wkt(ctx.get(), "NOT_VALID_WKT[", strict_yes,
                                     nullptr, &raw_err),
                IsNull());
    ScopedStringList err_guard(raw_err);
    EXPECT_THAT(CountStringList(err_guard.get()), Gt(0u));
  }
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_create_from_wkt(ctx.get(), "NOT_VALID_WKT[", nullptr,
                                     nullptr, nullptr),
                IsNull());
  }
}

TEST(Iso19111CApiTest, CreateAndCloneObjects) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // proj_create various user input formats.
  ScopedPj by_urn(proj_create(ctx.get(), "urn:ogc:def:crs:EPSG::4326"));
  ASSERT_THAT(by_urn.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_GEOGRAPHIC_2D_CRS, proj_get_type(by_urn.get()));

  ScopedPj by_compound_name(proj_create(ctx.get(), "WGS 84 + EGM96 height"));
  ASSERT_THAT(by_compound_name.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_COMPOUND_CRS, proj_get_type(by_compound_name.get()));

  const char* json = proj_as_projjson(ctx.get(), by_urn.get(), nullptr);
  ASSERT_THAT(json, NotNull());
  ScopedPj by_json(proj_create(ctx.get(), json));
  ASSERT_THAT(by_json.get(), NotNull());

  // proj_create error branches (null, syntax error, unknown authority code).
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_create(ctx.get(), nullptr), IsNull());
    EXPECT_THAT(proj_create(ctx.get(), "not_a_valid_crs_string"), IsNull());
    EXPECT_NE(0, proj_context_errno(ctx.get()));
    EXPECT_THAT(proj_create(ctx.get(), "EPSG:99999999"), IsNull());
  }

  // proj_clone on nullptr, classic PJ without ISO obj, ISO PJ, and multi-op.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_clone(ctx.get(), nullptr), IsNull());
  }
  ScopedPj classic_pj(pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_THAT(classic_pj.get(), NotNull());
  EXPECT_THAT(proj_clone(ctx.get(), classic_pj.get()), IsNull());

  ScopedPj cloned_iso(proj_clone(ctx.get(), by_urn.get()));
  ASSERT_THAT(cloned_iso.get(), NotNull());
  EXPECT_EQ(
      1, proj_is_equivalent_to(by_urn.get(), cloned_iso.get(), PJ_COMP_STRICT));

  // Multi-operation PJ from proj_create_crs_to_crs
  // (alternativeCoordinateOperations).
  ScopedPj multi_op(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4267", "EPSG:4326", nullptr));
  ASSERT_THAT(multi_op.get(), NotNull());
  ScopedPj cloned_multi(proj_clone(ctx.get(), multi_op.get()));
  ASSERT_THAT(cloned_multi.get(), NotNull());
  EXPECT_EQ(1, proj_is_equivalent_to(multi_op.get(), cloned_multi.get(),
                                     PJ_COMP_STRICT));
}

TEST(Iso19111CApiTest, CreateFromDatabaseUomGridAndNameLookup) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // Error branches: missing inputs and nonexistent code.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_create_from_database(ctx.get(), nullptr, "4326",
                                          PJ_CATEGORY_CRS, 0, nullptr),
                IsNull());
    EXPECT_THAT(proj_create_from_database(ctx.get(), "EPSG", nullptr,
                                          PJ_CATEGORY_CRS, 0, nullptr),
                IsNull());
    EXPECT_THAT(proj_create_from_database(ctx.get(), "EPSG", "99999999",
                                          PJ_CATEGORY_CRS, 0, nullptr),
                IsNull());
  }

  // All 6 PJ_CATEGORY values.
  ScopedPj ellps(proj_create_from_database(ctx.get(), "EPSG", "7030",
                                           PJ_CATEGORY_ELLIPSOID, 0, nullptr));
  ASSERT_THAT(ellps.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_ELLIPSOID, proj_get_type(ellps.get()));

  ScopedPj pm(proj_create_from_database(
      ctx.get(), "EPSG", "8901", PJ_CATEGORY_PRIME_MERIDIAN, 0, nullptr));
  ASSERT_THAT(pm.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_PRIME_MERIDIAN, proj_get_type(pm.get()));

  ScopedPj datum(proj_create_from_database(ctx.get(), "EPSG", "6258",
                                           PJ_CATEGORY_DATUM, 0, nullptr));
  ASSERT_THAT(datum.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_GEODETIC_REFERENCE_FRAME, proj_get_type(datum.get()));

  ScopedPj crs(proj_create_from_database(ctx.get(), "EPSG", "4326",
                                         PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(crs.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_GEOGRAPHIC_2D_CRS, proj_get_type(crs.get()));

  ScopedPj ensemble(proj_create_from_database(
      ctx.get(), "EPSG", "6326", PJ_CATEGORY_DATUM_ENSEMBLE, 0, nullptr));
  ASSERT_THAT(ensemble.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_DATUM_ENSEMBLE, proj_get_type(ensemble.get()));

  // Coordinate operation with usePROJAlternativeGridNames = 0 and 1.
  ScopedPj op_official(proj_create_from_database(
      ctx.get(), "EPSG", "5661", PJ_CATEGORY_COORDINATE_OPERATION, 0, nullptr));
  ASSERT_THAT(op_official.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_TRANSFORMATION, proj_get_type(op_official.get()));

  ScopedPj op_alt(proj_create_from_database(
      ctx.get(), "EPSG", "5661", PJ_CATEGORY_COORDINATE_OPERATION, 1, nullptr));
  ASSERT_THAT(op_alt.get(), NotNull());

  // proj_uom_get_info_from_database across unit categories.
  struct UomCase {
    const char* code;
    const char* expected_category;
  };
  constexpr UomCase kUomCases[] = {
      {"9001", "linear"},  {"1026", "linear_per_time"},
      {"9102", "angular"}, {"1032", "angular_per_time"},
      {"1028", "scale"},   {"1030", "scale_per_time"},
      {"1029", "time"},
  };
  for (const auto& uom : kUomCases) {
    const char* name = nullptr;
    double factor = 0.0;
    const char* category = nullptr;
    EXPECT_EQ(1, proj_uom_get_info_from_database(ctx.get(), "EPSG", uom.code,
                                                 &name, &factor, &category));
    EXPECT_THAT(name, NotNull());
    EXPECT_GT(factor, 0.0);
    EXPECT_THAT(category, StrEq(uom.expected_category));
  }
  EXPECT_EQ(1, proj_uom_get_info_from_database(ctx.get(), "EPSG", "9001",
                                               nullptr, nullptr, nullptr));
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(0, proj_uom_get_info_from_database(ctx.get(), nullptr, "9001",
                                                 nullptr, nullptr, nullptr));
    EXPECT_EQ(0, proj_uom_get_info_from_database(ctx.get(), "EPSG", nullptr,
                                                 nullptr, nullptr, nullptr));
    EXPECT_EQ(0, proj_uom_get_info_from_database(ctx.get(), "EPSG", "99999999",
                                                 nullptr, nullptr, nullptr));
  }

  // proj_grid_get_info_from_database with all outputs, null outputs, errors.
  const char* full_name = nullptr;
  const char* package_name = nullptr;
  const char* url = nullptr;
  int direct_download = -1;
  int open_license = -1;
  int available = -1;
  EXPECT_EQ(1, proj_grid_get_info_from_database(
                   ctx.get(), "100800401.gsb", &full_name, &package_name, &url,
                   &direct_download, &open_license, &available));
  EXPECT_THAT(full_name, NotNull());
  EXPECT_THAT(package_name, NotNull());
  EXPECT_THAT(url, HasSubstr("es_cat_icgc_100800401.tif"));
  EXPECT_EQ(1, direct_download);
  EXPECT_EQ(1, open_license);
  EXPECT_EQ(1, proj_grid_get_info_from_database(ctx.get(), "100800401.gsb",
                                                nullptr, nullptr, nullptr,
                                                nullptr, nullptr, nullptr));
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(0, proj_grid_get_info_from_database(ctx.get(), nullptr, nullptr,
                                                  nullptr, nullptr, nullptr,
                                                  nullptr, nullptr));
    EXPECT_EQ(0, proj_grid_get_info_from_database(
                     ctx.get(), "no_such_grid_xyz.tif", nullptr, nullptr,
                     nullptr, nullptr, nullptr, nullptr));
  }

  // proj_query_geodetic_crs_from_datum.
  ScopedObjList geod_list_filtered(proj_query_geodetic_crs_from_datum(
      ctx.get(), "EPSG", "EPSG", "6326", "geographic 2D"));
  ASSERT_THAT(geod_list_filtered.get(), NotNull());
  EXPECT_GT(proj_list_get_count(geod_list_filtered.get()), 0);

  ScopedObjList geod_list_all(proj_query_geodetic_crs_from_datum(
      ctx.get(), nullptr, "EPSG", "6326", nullptr));
  ASSERT_THAT(geod_list_all.get(), NotNull());
  EXPECT_GE(proj_list_get_count(geod_list_all.get()),
            proj_list_get_count(geod_list_filtered.get()));
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_query_geodetic_crs_from_datum(ctx.get(), nullptr, nullptr,
                                                   "6326", nullptr),
                IsNull());
    EXPECT_THAT(proj_query_geodetic_crs_from_datum(ctx.get(), nullptr, "EPSG",
                                                   nullptr, nullptr),
                IsNull());
  }

  // proj_create_from_name: invalid argument combos, all PJ_TYPE switch cases,
  // approximate match, limitResultCount, and nullptr types.
  {
    ScopedLogSuppressor suppress(ctx.get());
    const PJ_TYPE one_type[] = {PJ_TYPE_PROJECTED_CRS};
    EXPECT_THAT(proj_create_from_name(ctx.get(), "EPSG", nullptr, nullptr, 0, 0,
                                      0, nullptr),
                IsNull());
    EXPECT_THAT(proj_create_from_name(ctx.get(), "EPSG", "WGS 84", one_type, 0,
                                      0, 0, nullptr),
                IsNull());
    EXPECT_THAT(proj_create_from_name(ctx.get(), "EPSG", "WGS 84", nullptr, 1,
                                      0, 0, nullptr),
                IsNull());
  }

  constexpr PJ_TYPE kAllTypes[] = {
      PJ_TYPE_ELLIPSOID,
      PJ_TYPE_PRIME_MERIDIAN,
      PJ_TYPE_GEODETIC_REFERENCE_FRAME,
      PJ_TYPE_DYNAMIC_GEODETIC_REFERENCE_FRAME,
      PJ_TYPE_VERTICAL_REFERENCE_FRAME,
      PJ_TYPE_DYNAMIC_VERTICAL_REFERENCE_FRAME,
      PJ_TYPE_DATUM_ENSEMBLE,
      PJ_TYPE_TEMPORAL_DATUM,
      PJ_TYPE_ENGINEERING_DATUM,
      PJ_TYPE_PARAMETRIC_DATUM,
      PJ_TYPE_CRS,
      PJ_TYPE_GEODETIC_CRS,
      PJ_TYPE_GEOCENTRIC_CRS,
      PJ_TYPE_GEOGRAPHIC_CRS,
      PJ_TYPE_GEOGRAPHIC_2D_CRS,
      PJ_TYPE_GEOGRAPHIC_3D_CRS,
      PJ_TYPE_VERTICAL_CRS,
      PJ_TYPE_PROJECTED_CRS,
      PJ_TYPE_DERIVED_PROJECTED_CRS,
      PJ_TYPE_COMPOUND_CRS,
      PJ_TYPE_ENGINEERING_CRS,
      PJ_TYPE_TEMPORAL_CRS,
      PJ_TYPE_BOUND_CRS,
      PJ_TYPE_OTHER_CRS,
      PJ_TYPE_CONVERSION,
      PJ_TYPE_TRANSFORMATION,
      PJ_TYPE_CONCATENATED_OPERATION,
      PJ_TYPE_OTHER_COORDINATE_OPERATION,
      PJ_TYPE_UNKNOWN,
      PJ_TYPE_COORDINATE_METADATA,
  };
  ScopedObjList empty_all_types(proj_create_from_name(
      ctx.get(), "EPSG", "zzzz_unlikely_object_name_xyz", kAllTypes,
      std::size(kAllTypes), /*approximateMatch=*/0, /*limitResultCount=*/1,
      nullptr));
  ASSERT_THAT(empty_all_types.get(), NotNull());
  EXPECT_EQ(0, proj_list_get_count(empty_all_types.get()));

  const PJ_TYPE geog2d_type[] = {PJ_TYPE_GEOGRAPHIC_2D_CRS};
  ScopedObjList wgs_by_name(proj_create_from_name(
      ctx.get(), "EPSG", "WGS 84", geog2d_type, 1, /*approximateMatch=*/0,
      /*limitResultCount=*/5, nullptr));
  ASSERT_THAT(wgs_by_name.get(), NotNull());
  EXPECT_GT(proj_list_get_count(wgs_by_name.get()), 0);

  ScopedObjList wgs_approx(proj_create_from_name(
      ctx.get(), nullptr, "WGS 84 / UTM zone 31N", nullptr, 0,
      /*approximateMatch=*/1, /*limitResultCount=*/3, nullptr));
  ASSERT_THAT(wgs_approx.get(), NotNull());
  EXPECT_GT(proj_list_get_count(wgs_approx.get()), 0);
}

TEST(Iso19111CApiTest, DatabaseEnumerationAndGeoidModels) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  ScopedStringList auths(proj_get_authorities_from_database(ctx.get()));
  ASSERT_THAT(auths.get(), NotNull());
  EXPECT_TRUE(HasString(auths.get(), "EPSG"));
  EXPECT_TRUE(HasString(auths.get(), "IAU_2015"));

  // proj_get_codes_from_database.
  ScopedStringList geog2d_codes(proj_get_codes_from_database(
      ctx.get(), "EPSG", PJ_TYPE_GEOGRAPHIC_2D_CRS, 0));
  ASSERT_THAT(geog2d_codes.get(), NotNull());
  EXPECT_TRUE(HasString(geog2d_codes.get(), "4326"));

  ScopedStringList with_depr_codes(proj_get_codes_from_database(
      ctx.get(), "EPSG", PJ_TYPE_PROJECTED_CRS, 1));
  ASSERT_THAT(with_depr_codes.get(), NotNull());
  EXPECT_GT(CountStringList(with_depr_codes.get()), 0u);
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_get_codes_from_database(ctx.get(), nullptr,
                                             PJ_TYPE_GEOGRAPHIC_2D_CRS, 0),
                IsNull());
    EXPECT_THAT(
        proj_get_codes_from_database(ctx.get(), "EPSG", PJ_TYPE_UNKNOWN, 0),
        IsNull());
  }

  // proj_get_celestial_body_list_from_database.
  int all_bodies_count = 0;
  ScopedCelestialBodyList all_bodies(proj_get_celestial_body_list_from_database(
      ctx.get(), nullptr, &all_bodies_count));
  ASSERT_THAT(all_bodies.get(), NotNull());
  EXPECT_GT(all_bodies_count, 1);

  ScopedCelestialBodyList iau_bodies(proj_get_celestial_body_list_from_database(
      ctx.get(), "IAU_2015", nullptr));
  ASSERT_THAT(iau_bodies.get(), NotNull());
  EXPECT_THAT(iau_bodies.get()[0], NotNull());

  // proj_get_crs_info_list_from_database with null/empty/filtered parameters.
  ScopedCrsInfoList unknown_auth_crs(proj_get_crs_info_list_from_database(
      ctx.get(), "UNKNOWN_AUTHORITY_XYZ", nullptr, nullptr));
  ASSERT_THAT(unknown_auth_crs.get(), NotNull());
  EXPECT_THAT(unknown_auth_crs.get()[0], IsNull());

  int iau_count = 0;
  ScopedCrsListParams params(proj_get_crs_list_parameters_create());
  ASSERT_THAT(params.get(), NotNull());
  params->celestial_body_name = "Moon";
  params->allow_deprecated = 1;
  ScopedCrsInfoList moon_crs(proj_get_crs_info_list_from_database(
      ctx.get(), "IAU_2015", params.get(), &iau_count));
  ASSERT_THAT(moon_crs.get(), NotNull());
  EXPECT_GT(iau_count, 0);

  // Full EPSG scan with NULL params hits every CRS type mapping branch.
  int all_epsg_count = 0;
  ScopedCrsInfoList all_epsg(proj_get_crs_info_list_from_database(
      ctx.get(), "EPSG", nullptr, &all_epsg_count));
  ASSERT_THAT(all_epsg.get(), NotNull());
  EXPECT_GT(all_epsg_count, 100);

  // Type filter + regular bbox with contains=1 and contains=0.
  ScopedCrsListParams epsg_params(proj_get_crs_list_parameters_create());
  const PJ_TYPE filter_types[] = {PJ_TYPE_GEOGRAPHIC_CRS, PJ_TYPE_GEODETIC_CRS,
                                  PJ_TYPE_COMPOUND_CRS, PJ_TYPE_VERTICAL_CRS,
                                  PJ_TYPE_PROJECTED_CRS};
  epsg_params->types = filter_types;
  epsg_params->typesCount = std::size(filter_types);
  epsg_params->bbox_valid = 1;
  epsg_params->west_lon_degree = -5.0;
  epsg_params->south_lat_degree = 42.0;
  epsg_params->east_lon_degree = 8.0;
  epsg_params->north_lat_degree = 51.0;
  epsg_params->crs_area_of_use_contains_bbox = 1;
  int contains_count = 0;
  ScopedCrsInfoList france_contains(proj_get_crs_info_list_from_database(
      ctx.get(), "EPSG", epsg_params.get(), &contains_count));
  ASSERT_THAT(france_contains.get(), NotNull());
  EXPECT_GT(contains_count, 0);

  epsg_params->crs_area_of_use_contains_bbox = 0;
  int intersects_count = 0;
  ScopedCrsInfoList france_intersects(proj_get_crs_info_list_from_database(
      ctx.get(), "EPSG", epsg_params.get(), &intersects_count));
  ASSERT_THAT(france_intersects.get(), NotNull());
  EXPECT_GE(intersects_count, contains_count);

  // Antimeridian bbox (west > east) exercises GeographicBoundingBox path.
  epsg_params->types = nullptr;
  epsg_params->typesCount = 0;
  epsg_params->west_lon_degree = 170.0;
  epsg_params->east_lon_degree = -170.0;
  epsg_params->south_lat_degree = 50.0;
  epsg_params->north_lat_degree = 75.0;
  epsg_params->crs_area_of_use_contains_bbox = 1;
  int antimeridian_contains_count = 0;
  ScopedCrsInfoList antimeridian_contains(proj_get_crs_info_list_from_database(
      ctx.get(), "EPSG", epsg_params.get(), &antimeridian_contains_count));
  ASSERT_THAT(antimeridian_contains.get(), NotNull());
  EXPECT_GT(antimeridian_contains_count, 0);

  epsg_params->crs_area_of_use_contains_bbox = 0;
  int antimeridian_intersects_count = 0;
  ScopedCrsInfoList antimeridian_intersects(
      proj_get_crs_info_list_from_database(ctx.get(), "EPSG", epsg_params.get(),
                                           &antimeridian_intersects_count));
  ASSERT_THAT(antimeridian_intersects.get(), NotNull());
  EXPECT_GE(antimeridian_intersects_count, antimeridian_contains_count);

  // proj_get_units_from_database.
  int linear_unit_count = 0;
  ScopedUnitInfoList linear_units(proj_get_units_from_database(
      ctx.get(), "EPSG", "linear", /*allow_deprecated=*/0, &linear_unit_count));
  ASSERT_THAT(linear_units.get(), NotNull());
  EXPECT_GT(linear_unit_count, 0);

  ScopedUnitInfoList all_units(proj_get_units_from_database(
      ctx.get(), nullptr, nullptr, /*allow_deprecated=*/1, nullptr));
  ASSERT_THAT(all_units.get(), NotNull());
  EXPECT_THAT(all_units.get()[0], NotNull());

  // proj_get_geoid_models_from_database.
  ScopedStringList geoid_models(
      proj_get_geoid_models_from_database(ctx.get(), "EPSG", "5703", nullptr));
  ASSERT_THAT(geoid_models.get(), NotNull());
  EXPECT_GT(CountStringList(geoid_models.get()), 0u);
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_get_geoid_models_from_database(ctx.get(), nullptr, "5703",
                                                    nullptr),
                IsNull());
    EXPECT_THAT(proj_get_geoid_models_from_database(ctx.get(), "EPSG", nullptr,
                                                    nullptr),
                IsNull());
  }
}

TEST(Iso19111CApiTest, IntrospectionIdentifyAndExports) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // Cover all PJ_TYPE enum values returned by proj_get_type().
  EXPECT_EQ(PJ_TYPE_UNKNOWN, proj_get_type(nullptr));
  ScopedPj classic_pj(pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_THAT(classic_pj.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_UNKNOWN, proj_get_type(classic_pj.get()));

  ScopedPj ellps(proj_create_from_database(ctx.get(), "EPSG", "7030",
                                           PJ_CATEGORY_ELLIPSOID, 0, nullptr));
  EXPECT_EQ(PJ_TYPE_ELLIPSOID, proj_get_type(ellps.get()));
  EXPECT_EQ(PJ_TYPE_ELLIPSOID, proj_get_type(ellps.get()));  // cached branch

  ScopedPj pm(proj_create_from_database(
      ctx.get(), "EPSG", "8901", PJ_CATEGORY_PRIME_MERIDIAN, 0, nullptr));
  EXPECT_EQ(PJ_TYPE_PRIME_MERIDIAN, proj_get_type(pm.get()));

  ScopedPj static_g_datum(proj_create_from_database(
      ctx.get(), "EPSG", "6258", PJ_CATEGORY_DATUM, 0, nullptr));
  EXPECT_EQ(PJ_TYPE_GEODETIC_REFERENCE_FRAME,
            proj_get_type(static_g_datum.get()));

  ScopedPj dyn_g_datum(proj_create_from_database(
      ctx.get(), "EPSG", "1061", PJ_CATEGORY_DATUM, 0, nullptr));
  EXPECT_EQ(PJ_TYPE_DYNAMIC_GEODETIC_REFERENCE_FRAME,
            proj_get_type(dyn_g_datum.get()));

  ScopedPj vert_crs(proj_create_from_database(ctx.get(), "EPSG", "5703",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(vert_crs.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_VERTICAL_CRS, proj_get_type(vert_crs.get()));
  ScopedPj static_v_datum(proj_crs_get_datum(ctx.get(), vert_crs.get()));
  ASSERT_THAT(static_v_datum.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_VERTICAL_REFERENCE_FRAME,
            proj_get_type(static_v_datum.get()));

  ScopedPj dyn_v_datum(proj_create_from_database(
      ctx.get(), "EPSG", "5208", PJ_CATEGORY_DATUM, 0, nullptr));
  EXPECT_EQ(PJ_TYPE_DYNAMIC_VERTICAL_REFERENCE_FRAME,
            proj_get_type(dyn_v_datum.get()));

  ScopedPj ensemble(proj_create_from_database(
      ctx.get(), "EPSG", "6326", PJ_CATEGORY_DATUM_ENSEMBLE, 0, nullptr));
  EXPECT_EQ(PJ_TYPE_DATUM_ENSEMBLE, proj_get_type(ensemble.get()));

  ScopedPj t_datum(proj_create_from_wkt(ctx.get(), kTemporalDatumWkt, nullptr,
                                        nullptr, nullptr));
  EXPECT_EQ(PJ_TYPE_TEMPORAL_DATUM, proj_get_type(t_datum.get()));

  ScopedPj eng_crs(proj_create_from_database(ctx.get(), "EPSG", "5800",
                                             PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(eng_crs.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_ENGINEERING_CRS, proj_get_type(eng_crs.get()));
  ScopedPj eng_datum(proj_crs_get_datum(ctx.get(), eng_crs.get()));
  ASSERT_THAT(eng_datum.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_ENGINEERING_DATUM, proj_get_type(eng_datum.get()));

  ScopedPj p_datum(proj_create_from_wkt(ctx.get(), kParametricDatumWkt, nullptr,
                                        nullptr, nullptr));
  EXPECT_EQ(PJ_TYPE_PARAMETRIC_DATUM, proj_get_type(p_datum.get()));

  ScopedPj geog2d(proj_create_from_database(ctx.get(), "EPSG", "4326",
                                            PJ_CATEGORY_CRS, 0, nullptr));
  EXPECT_EQ(PJ_TYPE_GEOGRAPHIC_2D_CRS, proj_get_type(geog2d.get()));

  ScopedPj geog3d(proj_create_from_database(ctx.get(), "EPSG", "4979",
                                            PJ_CATEGORY_CRS, 0, nullptr));
  EXPECT_EQ(PJ_TYPE_GEOGRAPHIC_3D_CRS, proj_get_type(geog3d.get()));

  ScopedPj geoc(proj_create_from_database(ctx.get(), "EPSG", "4978",
                                          PJ_CATEGORY_CRS, 0, nullptr));
  EXPECT_EQ(PJ_TYPE_GEOCENTRIC_CRS, proj_get_type(geoc.get()));

  ScopedPj sph_geod(proj_create_from_wkt(ctx.get(), kSphericalGeodCrsWkt,
                                         nullptr, nullptr, nullptr));
  EXPECT_EQ(PJ_TYPE_GEODETIC_CRS, proj_get_type(sph_geod.get()));

  ScopedPj proj_crs(proj_create_from_database(ctx.get(), "EPSG", "32631",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  EXPECT_EQ(PJ_TYPE_PROJECTED_CRS, proj_get_type(proj_crs.get()));

  ScopedPj conv(proj_crs_get_coordoperation(ctx.get(), proj_crs.get()));
  EXPECT_EQ(PJ_TYPE_CONVERSION, proj_get_type(conv.get()));

  ScopedPj cart_cs(proj_create_cartesian_2D_cs(
      ctx.get(), PJ_CART2D_EASTING_NORTHING, "metre", 1.0));
  ScopedPj affine_conv(proj_create_linear_affine_parametric_conversion(
      ctx.get(), "AffineTest", 10.0, nullptr, 0.0, 1.0, nullptr, 0.0, 0.0,
      nullptr, 0.0, 20.0, nullptr, 0.0, 0.0, nullptr, 0.0, 1.0, nullptr, 0.0));
  ScopedPj derived_proj(proj_create_derived_projected_crs(
      ctx.get(), "DerivedProjTest", proj_crs.get(), affine_conv.get(),
      cart_cs.get()));
  EXPECT_EQ(PJ_TYPE_DERIVED_PROJECTED_CRS, proj_get_type(derived_proj.get()));

  ScopedPj compound(proj_create_from_database(ctx.get(), "EPSG", "5498",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  EXPECT_EQ(PJ_TYPE_COMPOUND_CRS, proj_get_type(compound.get()));

  ScopedPj t_crs(proj_create_from_wkt(ctx.get(), kTimeCrsMeasureWkt, nullptr,
                                      nullptr, nullptr));
  EXPECT_EQ(PJ_TYPE_TEMPORAL_CRS, proj_get_type(t_crs.get()));

  ScopedPj p_crs(proj_create_from_wkt(ctx.get(), kParametricCrsWkt, nullptr,
                                      nullptr, nullptr));
  EXPECT_EQ(PJ_TYPE_OTHER_CRS, proj_get_type(p_crs.get()));

  ScopedPj transf(proj_create_from_database(
      ctx.get(), "EPSG", "1671", PJ_CATEGORY_COORDINATE_OPERATION, 0, nullptr));
  EXPECT_EQ(PJ_TYPE_TRANSFORMATION, proj_get_type(transf.get()));

  ScopedPj bound_src(proj_get_source_crs(ctx.get(), transf.get()));
  ScopedPj bound_tgt(proj_get_target_crs(ctx.get(), transf.get()));
  ASSERT_THAT(bound_src.get(), NotNull());
  ASSERT_THAT(bound_tgt.get(), NotNull());
  ScopedPj bound(proj_crs_create_bound_crs(ctx.get(), bound_src.get(),
                                           bound_tgt.get(), transf.get()));
  ASSERT_THAT(bound.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_BOUND_CRS, proj_get_type(bound.get()));

  ScopedPj concat(proj_create_from_database(
      ctx.get(), "EPSG", "3896", PJ_CATEGORY_COORDINATE_OPERATION, 0, nullptr));
  ASSERT_THAT(concat.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_CONCATENATED_OPERATION, proj_get_type(concat.get()));

  ScopedPj other_op(proj_create_from_wkt(ctx.get(), kPointMotionOpWkt, nullptr,
                                         nullptr, nullptr));
  ASSERT_THAT(other_op.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_OTHER_COORDINATE_OPERATION, proj_get_type(other_op.get()));

  ScopedPj coord_meta(proj_create_from_wkt(ctx.get(), kCoordMetaWgs84NoEpochWkt,
                                           nullptr, nullptr, nullptr));
  ASSERT_THAT(coord_meta.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_COORDINATE_METADATA, proj_get_type(coord_meta.get()));

  // proj_is_deprecated & proj_get_non_deprecated.
  EXPECT_EQ(0, proj_is_deprecated(nullptr));
  EXPECT_EQ(0, proj_is_deprecated(classic_pj.get()));
  EXPECT_EQ(0, proj_is_deprecated(geog2d.get()));
  ScopedPj deprecated_crs(proj_create_from_database(
      ctx.get(), "EPSG", "2156", PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(deprecated_crs.get(), NotNull());
  EXPECT_EQ(1, proj_is_deprecated(deprecated_crs.get()));

  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_get_non_deprecated(ctx.get(), nullptr), IsNull());
  }
  EXPECT_THAT(proj_get_non_deprecated(ctx.get(), ellps.get()), IsNull());
  ScopedObjList non_depr(
      proj_get_non_deprecated(ctx.get(), deprecated_crs.get()));
  ASSERT_THAT(non_depr.get(), NotNull());
  EXPECT_GT(proj_list_get_count(non_depr.get()), 0);

  // proj_is_equivalent_to & proj_is_equivalent_to_with_ctx.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(0, proj_is_equivalent_to_with_ctx(ctx.get(), nullptr,
                                                geog2d.get(), PJ_COMP_STRICT));
  }
  EXPECT_EQ(0, proj_is_equivalent_to(geog2d.get(), nullptr, PJ_COMP_STRICT));
  EXPECT_EQ(
      0, proj_is_equivalent_to(classic_pj.get(), geog2d.get(), PJ_COMP_STRICT));
  EXPECT_EQ(
      0, proj_is_equivalent_to(geog2d.get(), classic_pj.get(), PJ_COMP_STRICT));

  ScopedPj wgs84_lonlat(proj_create(ctx.get(), "OGC:CRS84"));
  ASSERT_THAT(wgs84_lonlat.get(), NotNull());
  EXPECT_EQ(0, proj_is_equivalent_to(geog2d.get(), wgs84_lonlat.get(),
                                     PJ_COMP_STRICT));
  EXPECT_EQ(0, proj_is_equivalent_to(geog2d.get(), wgs84_lonlat.get(),
                                     PJ_COMP_EQUIVALENT));
  EXPECT_EQ(1, proj_is_equivalent_to_with_ctx(
                   ctx.get(), geog2d.get(), wgs84_lonlat.get(),
                   PJ_COMP_EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS));

  // alternativeCoordinateOperations branch in proj_is_equivalent_to.
  ScopedPj multi_op_a(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4267", "EPSG:4326", nullptr));
  ScopedPj multi_op_b(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4267", "EPSG:4269", nullptr));
  ASSERT_THAT(multi_op_a.get(), NotNull());
  ASSERT_THAT(multi_op_b.get(), NotNull());
  EXPECT_EQ(1, proj_is_equivalent_to(multi_op_a.get(), multi_op_a.get(),
                                     PJ_COMP_STRICT));
  EXPECT_EQ(0, proj_is_equivalent_to(multi_op_a.get(), multi_op_b.get(),
                                     PJ_COMP_STRICT));
  ScopedPj multi_op_diff(proj_clone(ctx.get(), multi_op_a.get()));
  ASSERT_THAT(multi_op_diff.get(), NotNull());
  ASSERT_FALSE(multi_op_diff->alternativeCoordinateOperations.empty());
  multi_op_diff->alternativeCoordinateOperations[0].name = "modified_op_name";
  EXPECT_EQ(0, proj_is_equivalent_to(multi_op_a.get(), multi_op_diff.get(),
                                     PJ_COMP_STRICT));

  // Basic getters: proj_is_crs, proj_get_name, proj_get_remarks,
  // proj_get_id_auth_name, proj_get_id_code.
  EXPECT_EQ(0, proj_is_crs(nullptr));
  EXPECT_EQ(0, proj_is_crs(ellps.get()));
  EXPECT_EQ(1, proj_is_crs(geog2d.get()));

  EXPECT_THAT(proj_get_name(nullptr), IsNull());
  EXPECT_THAT(proj_get_name(classic_pj.get()), IsNull());
  EXPECT_THAT(proj_get_name(geog2d.get()), StrEq("WGS 84"));

  EXPECT_THAT(proj_get_remarks(nullptr), IsNull());
  EXPECT_THAT(proj_get_remarks(classic_pj.get()), IsNull());
  EXPECT_THAT(proj_get_remarks(geog2d.get()), NotNull());

  EXPECT_THAT(proj_get_id_auth_name(nullptr, 0), IsNull());
  EXPECT_THAT(proj_get_id_auth_name(classic_pj.get(), 0), IsNull());
  EXPECT_THAT(proj_get_id_auth_name(geog2d.get(), 0), StrEq("EPSG"));
  EXPECT_THAT(proj_get_id_auth_name(geog2d.get(), -1), IsNull());
  EXPECT_THAT(proj_get_id_auth_name(geog2d.get(), 99), IsNull());

  EXPECT_THAT(proj_get_id_code(nullptr, 0), IsNull());
  EXPECT_THAT(proj_get_id_code(classic_pj.get(), 0), IsNull());
  EXPECT_THAT(proj_get_id_code(geog2d.get(), 0), StrEq("4326"));
  EXPECT_THAT(proj_get_id_code(geog2d.get(), -1), IsNull());
  EXPECT_THAT(proj_get_id_code(geog2d.get(), 99), IsNull());

  // Domains, scope, area of use.
  EXPECT_EQ(0, proj_get_domain_count(nullptr));
  EXPECT_EQ(0, proj_get_domain_count(classic_pj.get()));
  ScopedPj multi_domain(proj_create_from_database(ctx.get(), "EPSG", "2393",
                                                  PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(multi_domain.get(), NotNull());
  EXPECT_EQ(2, proj_get_domain_count(multi_domain.get()));

  EXPECT_THAT(proj_get_scope(nullptr), IsNull());
  EXPECT_THAT(proj_get_scope(classic_pj.get()), IsNull());
  EXPECT_THAT(proj_get_scope(multi_domain.get()), NotNull());
  EXPECT_THAT(proj_get_scope_ex(multi_domain.get(), 1), NotNull());
  EXPECT_THAT(proj_get_scope_ex(multi_domain.get(), -1), IsNull());
  EXPECT_THAT(proj_get_scope_ex(multi_domain.get(), 2), IsNull());
  EXPECT_THAT(proj_get_scope(p_crs.get()), IsNull());

  // Never pass nullptr to proj_get_area_of_use; pass non-null classic_pj.
  EXPECT_EQ(0, proj_get_area_of_use(ctx.get(), classic_pj.get(), nullptr,
                                    nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(0,
            proj_get_area_of_use_ex(ctx.get(), multi_domain.get(), -1, nullptr,
                                    nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(0,
            proj_get_area_of_use_ex(ctx.get(), multi_domain.get(), 2, nullptr,
                                    nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(0, proj_get_area_of_use(ctx.get(), p_crs.get(), nullptr, nullptr,
                                    nullptr, nullptr, nullptr));
  double west = 0.0;
  double south = 0.0;
  double east = 0.0;
  double north = 0.0;
  const char* area_name = nullptr;
  EXPECT_EQ(1, proj_get_area_of_use(ctx.get(), geog2d.get(), &west, &south,
                                    &east, &north, &area_name));
  EXPECT_DOUBLE_EQ(-180.0, west);
  EXPECT_DOUBLE_EQ(-90.0, south);
  EXPECT_DOUBLE_EQ(180.0, east);
  EXPECT_DOUBLE_EQ(90.0, north);
  EXPECT_THAT(area_name, NotNull());

  // Extent without BBOX returns -1000 sentinel values and TRUE.
  constexpr const char* kExtentNoBboxWkt =
      "GEOGCRS[\"NoBBox\","
      "DATUM[\"WGS 84\",ELLIPSOID[\"WGS 84\",6378137,298.257223563]],"
      "CS[ellipsoidal,2],AXIS[\"lat\",north,ORDER[1]],AXIS[\"lon\",east,"
      "ORDER[2]],ANGLEUNIT[\"degree\",0.0174532925199],"
      "USAGE[SCOPE[\"Custom scope\"],AREA[\"Named area without bbox\"]]]";
  ScopedPj no_bbox_crs(proj_create_from_wkt(ctx.get(), kExtentNoBboxWkt,
                                            nullptr, nullptr, nullptr));
  ASSERT_THAT(no_bbox_crs.get(), NotNull());
  const char* no_bbox_area = nullptr;
  EXPECT_EQ(1, proj_get_area_of_use(ctx.get(), no_bbox_crs.get(), &west, &south,
                                    &east, &north, &no_bbox_area));
  EXPECT_DOUBLE_EQ(-1000.0, west);
  EXPECT_DOUBLE_EQ(-1000.0, south);
  EXPECT_DOUBLE_EQ(-1000.0, east);
  EXPECT_DOUBLE_EQ(-1000.0, north);
  EXPECT_THAT(no_bbox_area, StrEq("Named area without bbox"));

  // proj_as_wkt: all 6 dialects, options, and error cases.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_as_wkt(ctx.get(), nullptr, PJ_WKT2_2019, nullptr),
                IsNull());
    const char* const bad_opts[] = {"INVALID_OPT=1", nullptr};
    EXPECT_THAT(proj_as_wkt(ctx.get(), geog2d.get(), PJ_WKT2_2019, bad_opts),
                IsNull());
    EXPECT_THAT(proj_as_wkt(ctx.get(), t_crs.get(), PJ_WKT1_GDAL, nullptr),
                IsNull());
  }
  EXPECT_THAT(proj_as_wkt(ctx.get(), classic_pj.get(), PJ_WKT2_2019, nullptr),
              IsNull());

  const PJ_WKT_TYPE wkt_types[] = {PJ_WKT2_2015, PJ_WKT2_2015_SIMPLIFIED,
                                   PJ_WKT2_2019, PJ_WKT2_2019_SIMPLIFIED,
                                   PJ_WKT1_GDAL, PJ_WKT1_ESRI};
  for (PJ_WKT_TYPE wt : wkt_types) {
    EXPECT_THAT(proj_as_wkt(ctx.get(), proj_crs.get(), wt, nullptr), NotNull());
  }
  const char* const wkt1_gdal_3d_opts[] = {
      "MULTILINE=NO",
      "INDENTATION_WIDTH=2",
      "OUTPUT_AXIS=YES",
      "STRICT=NO",
      "ALLOW_ELLIPSOIDAL_HEIGHT_AS_VERTICAL_CRS=YES",
      nullptr};
  EXPECT_THAT(
      proj_as_wkt(ctx.get(), geog3d.get(), PJ_WKT1_GDAL, wkt1_gdal_3d_opts),
      NotNull());
  const char* const wkt1_esri_opts[] = {"OUTPUT_AXIS=NO",
                                        "ALLOW_LINUNIT_NODE=NO", nullptr};
  EXPECT_THAT(
      proj_as_wkt(ctx.get(), geog3d.get(), PJ_WKT1_ESRI, wkt1_esri_opts),
      NotNull());

  // proj_as_proj_string: PROJ_5, PROJ_4, options, non-exportable, errors.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_as_proj_string(ctx.get(), nullptr, PJ_PROJ_5, nullptr),
                IsNull());
    EXPECT_THAT(proj_as_proj_string(ctx.get(), static_g_datum.get(), PJ_PROJ_5,
                                    nullptr),
                IsNull());
    const char* const bad_opts[] = {"BAD_PROJ_OPT=1", nullptr};
    EXPECT_THAT(
        proj_as_proj_string(ctx.get(), proj_crs.get(), PJ_PROJ_5, bad_opts),
        IsNull());
    EXPECT_THAT(proj_as_proj_string(ctx.get(), t_crs.get(), PJ_PROJ_5, nullptr),
                IsNull());
  }
  const char* const proj_opts[] = {"MULTILINE=YES", "INDENTATION_WIDTH=4",
                                   "MAX_LINE_LENGTH=40", "USE_APPROX_TMERC=YES",
                                   nullptr};
  const char* proj5_str =
      proj_as_proj_string(ctx.get(), proj_crs.get(), PJ_PROJ_5, proj_opts);
  ASSERT_THAT(proj5_str, NotNull());
  EXPECT_THAT(proj5_str, HasSubstr("+approx"));
  EXPECT_THAT(
      proj_as_proj_string(ctx.get(), proj_crs.get(), PJ_PROJ_4, nullptr),
      NotNull());

  // proj_as_projjson: options, non-exportable, errors.
  ScopedPj proj_based_op(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_THAT(proj_based_op.get(), NotNull());
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_as_projjson(ctx.get(), nullptr, nullptr), IsNull());
    EXPECT_THAT(proj_as_projjson(ctx.get(), classic_pj.get(), nullptr),
                IsNull());
    const char* const bad_opts[] = {"BAD_JSON_OPT=1", nullptr};
    EXPECT_THAT(proj_as_projjson(ctx.get(), geog2d.get(), bad_opts), IsNull());
  }
  EXPECT_THAT(proj_as_projjson(ctx.get(), proj_based_op.get(), nullptr),
              HasSubstr("PROJ-based coordinate operation"));
  const char* const json_opts[] = {"MULTILINE=NO", "INDENTATION_WIDTH=4",
                                   "SCHEMA=", nullptr};
  const char* json_str = proj_as_projjson(ctx.get(), geog2d.get(), json_opts);
  ASSERT_THAT(json_str, NotNull());
  EXPECT_THAT(json_str, HasSubstr("WGS 84"));

  // proj_get_source_crs & proj_get_target_crs.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_get_source_crs(ctx.get(), classic_pj.get()), IsNull());
    EXPECT_THAT(proj_get_target_crs(ctx.get(), nullptr), IsNull());
    EXPECT_THAT(proj_get_target_crs(ctx.get(), classic_pj.get()), IsNull());
  }
  EXPECT_THAT(proj_get_source_crs(ctx.get(), nullptr), IsNull());
  ScopedPj standalone_conv(proj_create_conversion_utm(ctx.get(), 31, 1));
  ASSERT_THAT(standalone_conv.get(), NotNull());
  EXPECT_THAT(proj_get_source_crs(ctx.get(), standalone_conv.get()), IsNull());
  EXPECT_THAT(proj_get_target_crs(ctx.get(), standalone_conv.get()), IsNull());
  ScopedPj src_affine(proj_get_source_crs(ctx.get(), affine_conv.get()));
  ScopedPj tgt_affine(proj_get_target_crs(ctx.get(), affine_conv.get()));
  EXPECT_THAT(src_affine.get(), NotNull());
  EXPECT_THAT(tgt_affine.get(), NotNull());

  ScopedPj src_conv(proj_get_source_crs(ctx.get(), conv.get()));
  ScopedPj tgt_conv(proj_get_target_crs(ctx.get(), conv.get()));
  ASSERT_THAT(src_conv.get(), NotNull());
  ASSERT_THAT(tgt_conv.get(), NotNull());

  ScopedPj src_bound(proj_get_source_crs(ctx.get(), bound.get()));
  ScopedPj tgt_bound(proj_get_target_crs(ctx.get(), bound.get()));
  ASSERT_THAT(src_bound.get(), NotNull());
  ASSERT_THAT(tgt_bound.get(), NotNull());

  ScopedPj src_derived(proj_get_source_crs(ctx.get(), derived_proj.get()));
  ASSERT_THAT(src_derived.get(), NotNull());

  ScopedPj src_cm(proj_get_source_crs(ctx.get(), coord_meta.get()));
  ASSERT_THAT(src_cm.get(), NotNull());

  ScopedPj src_multi(proj_get_source_crs(ctx.get(), multi_op_a.get()));
  ScopedPj tgt_multi(proj_get_target_crs(ctx.get(), multi_op_a.get()));
  ASSERT_THAT(src_multi.get(), NotNull());
  ASSERT_THAT(tgt_multi.get(), NotNull());

  // proj_identify: valid, auth_name=nullptr, out_confidence=nullptr, errors.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_identify(ctx.get(), nullptr, "EPSG", nullptr, nullptr),
                IsNull());
    EXPECT_THAT(proj_identify(ctx.get(), ellps.get(), "EPSG", nullptr, nullptr),
                IsNull());
  }
  int* raw_confidence = nullptr;
  ScopedObjList id_res(
      proj_identify(ctx.get(), geog2d.get(), "EPSG", nullptr, &raw_confidence));
  ScopedIntList conf_guard(raw_confidence);
  ASSERT_THAT(id_res.get(), NotNull());
  EXPECT_GT(proj_list_get_count(id_res.get()), 0);
  ASSERT_THAT(conf_guard.get(), NotNull());
  EXPECT_GE(conf_guard.get()[0], 70);

  ScopedObjList id_no_conf(
      proj_identify(ctx.get(), no_bbox_crs.get(), nullptr, nullptr, nullptr));
  ASSERT_THAT(id_no_conf.get(), NotNull());
}

TEST(Iso19111CApiTest, CrsDatumEllipsoidCsAndAlterations) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  ScopedPj classic_pj(pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84"));
  ScopedPj geog2d(proj_create_from_database(ctx.get(), "EPSG", "4326",
                                            PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj geog3d(proj_create_from_database(ctx.get(), "EPSG", "4979",
                                            PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj geoc(proj_create_from_database(ctx.get(), "EPSG", "4978",
                                          PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj nad27(proj_create_from_database(ctx.get(), "EPSG", "4267",
                                           PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj itrf2014(proj_create_from_database(ctx.get(), "EPSG", "7912",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj proj_crs(proj_create_from_database(ctx.get(), "EPSG", "32631",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj vert_crs(proj_create_from_database(ctx.get(), "EPSG", "5703",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj vert_ens_crs(proj_create_from_database(ctx.get(), "EPSG", "9451",
                                                  PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj compound(proj_create_from_database(ctx.get(), "EPSG", "5498",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj moon_crs(proj_create_from_database(ctx.get(), "IAU_2015", "30100",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj t_crs(proj_create_from_wkt(ctx.get(), kTimeCrsMeasureWkt, nullptr,
                                      nullptr, nullptr));
  ScopedPj t_cnt(proj_create_from_wkt(ctx.get(), kTimeCrsCountWkt, nullptr,
                                      nullptr, nullptr));
  ScopedPj t_dt(proj_create_from_wkt(ctx.get(), kTimeCrsDateTimeWkt, nullptr,
                                     nullptr, nullptr));
  ScopedPj p_crs(proj_create_from_wkt(ctx.get(), kParametricCrsWkt, nullptr,
                                      nullptr, nullptr));
  ScopedPj ord_crs(proj_create_from_wkt(ctx.get(), kOrdinalEngCrsWkt, nullptr,
                                        nullptr, nullptr));
  ScopedPj sph_geod(proj_create_from_wkt(ctx.get(), kSphericalGeodCrsWkt,
                                         nullptr, nullptr, nullptr));
  ScopedPj ellps(proj_create_from_database(ctx.get(), "EPSG", "7030",
                                           PJ_CATEGORY_ELLIPSOID, 0, nullptr));
  ScopedPj pm_paris(proj_create_from_database(
      ctx.get(), "EPSG", "8903", PJ_CATEGORY_PRIME_MERIDIAN, 0, nullptr));
  ScopedPj conv(proj_crs_get_coordoperation(ctx.get(), proj_crs.get()));
  ScopedPj dyn_g_datum(proj_create_from_database(
      ctx.get(), "EPSG", "1061", PJ_CATEGORY_DATUM, 0, nullptr));
  ScopedPj dyn_v_datum(proj_create_from_database(
      ctx.get(), "EPSG", "5208", PJ_CATEGORY_DATUM, 0, nullptr));

  ASSERT_THAT(geog2d.get(), NotNull());
  ASSERT_THAT(geog3d.get(), NotNull());
  ASSERT_THAT(geoc.get(), NotNull());
  ASSERT_THAT(nad27.get(), NotNull());
  ASSERT_THAT(itrf2014.get(), NotNull());
  ASSERT_THAT(proj_crs.get(), NotNull());
  ASSERT_THAT(vert_crs.get(), NotNull());
  ASSERT_THAT(vert_ens_crs.get(), NotNull());
  ASSERT_THAT(compound.get(), NotNull());
  ASSERT_THAT(moon_crs.get(), NotNull());
  ASSERT_THAT(conv.get(), NotNull());

  // proj_crs_is_derived & proj_is_derived_crs.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(0, proj_crs_is_derived(ctx.get(), nullptr));
    EXPECT_EQ(0, proj_crs_is_derived(ctx.get(), ellps.get()));
  }
  EXPECT_EQ(0, proj_crs_is_derived(ctx.get(), geog2d.get()));
  EXPECT_EQ(1, proj_crs_is_derived(ctx.get(), proj_crs.get()));
  // proj_is_derived_crs requires non-null PJ*.
  EXPECT_EQ(0, proj_is_derived_crs(ctx.get(), classic_pj.get()));
  EXPECT_EQ(0, proj_is_derived_crs(ctx.get(), geog2d.get()));
  EXPECT_EQ(1, proj_is_derived_crs(ctx.get(), proj_crs.get()));

  // proj_crs_is_dynamic.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(0, proj_crs_is_dynamic(ctx.get(), nullptr));
    EXPECT_EQ(0, proj_crs_is_dynamic(ctx.get(), ellps.get()));
  }
  EXPECT_EQ(0, proj_crs_is_dynamic(ctx.get(), geog2d.get()));
  EXPECT_EQ(1, proj_crs_is_dynamic(ctx.get(), itrf2014.get()));

  // proj_crs_get_geodetic_crs & proj_crs_get_sub_crs.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_crs_get_geodetic_crs(ctx.get(), nullptr), IsNull());
    EXPECT_THAT(proj_crs_get_geodetic_crs(ctx.get(), ellps.get()), IsNull());
    EXPECT_THAT(proj_crs_get_geodetic_crs(ctx.get(), vert_crs.get()), IsNull());
    EXPECT_THAT(proj_crs_get_sub_crs(ctx.get(), nullptr, 0), IsNull());
    EXPECT_THAT(proj_crs_get_sub_crs(ctx.get(), geog2d.get(), 0), IsNull());
  }
  ScopedPj geod_from_proj(proj_crs_get_geodetic_crs(ctx.get(), proj_crs.get()));
  ASSERT_THAT(geod_from_proj.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_GEOGRAPHIC_2D_CRS, proj_get_type(geod_from_proj.get()));

  ScopedPj sub0(proj_crs_get_sub_crs(ctx.get(), compound.get(), 0));
  ScopedPj sub1(proj_crs_get_sub_crs(ctx.get(), compound.get(), 1));
  EXPECT_THAT(sub0.get(), NotNull());
  EXPECT_THAT(sub1.get(), NotNull());
  EXPECT_THAT(proj_crs_get_sub_crs(ctx.get(), compound.get(), -1), IsNull());
  EXPECT_THAT(proj_crs_get_sub_crs(ctx.get(), compound.get(), 2), IsNull());

  // Horizontal datum, datum, datum ensemble, forced datum.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_crs_get_horizontal_datum(ctx.get(), vert_crs.get()),
                IsNull());
    EXPECT_THAT(proj_crs_get_datum(ctx.get(), nullptr), IsNull());
    EXPECT_THAT(proj_crs_get_datum(ctx.get(), compound.get()), IsNull());
    EXPECT_THAT(proj_crs_get_datum_ensemble(ctx.get(), nullptr), IsNull());
    EXPECT_THAT(proj_crs_get_datum_ensemble(ctx.get(), compound.get()),
                IsNull());
    EXPECT_THAT(proj_crs_get_datum_forced(ctx.get(), nullptr), IsNull());
    EXPECT_THAT(proj_crs_get_datum_forced(ctx.get(), compound.get()), IsNull());
  }
  ScopedPj h_datum_nad27(proj_crs_get_horizontal_datum(ctx.get(), nad27.get()));
  ASSERT_THAT(h_datum_nad27.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_GEODETIC_REFERENCE_FRAME,
            proj_get_type(h_datum_nad27.get()));

  ScopedPj h_datum_wgs(proj_crs_get_horizontal_datum(ctx.get(), geog2d.get()));
  ASSERT_THAT(h_datum_wgs.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_DATUM_ENSEMBLE, proj_get_type(h_datum_wgs.get()));

  ScopedPj datum_nad27(proj_crs_get_datum(ctx.get(), nad27.get()));
  ASSERT_THAT(datum_nad27.get(), NotNull());
  EXPECT_THAT(proj_crs_get_datum_ensemble(ctx.get(), nad27.get()), IsNull());
  ScopedPj forced_nad27(proj_crs_get_datum_forced(ctx.get(), nad27.get()));
  EXPECT_THAT(forced_nad27.get(), NotNull());

  EXPECT_THAT(proj_crs_get_datum(ctx.get(), geog2d.get()), IsNull());
  ScopedPj ens_wgs(proj_crs_get_datum_ensemble(ctx.get(), geog2d.get()));
  ASSERT_THAT(ens_wgs.get(), NotNull());
  ScopedPj forced_wgs(proj_crs_get_datum_forced(ctx.get(), geog2d.get()));
  EXPECT_THAT(forced_wgs.get(), NotNull());

  ScopedPj ens_vert(proj_crs_get_datum_ensemble(ctx.get(), vert_ens_crs.get()));
  ASSERT_THAT(ens_vert.get(), NotNull());
  ScopedPj forced_vert(
      proj_crs_get_datum_forced(ctx.get(), vert_ens_crs.get()));
  EXPECT_THAT(forced_vert.get(), NotNull());

  // Datum ensemble member count, accuracy, member; dynamic datum epoch.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(0, proj_datum_ensemble_get_member_count(ctx.get(), nullptr));
    EXPECT_EQ(
        0, proj_datum_ensemble_get_member_count(ctx.get(), datum_nad27.get()));
    EXPECT_DOUBLE_EQ(-1.0,
                     proj_datum_ensemble_get_accuracy(ctx.get(), nullptr));
    EXPECT_DOUBLE_EQ(
        -1.0, proj_datum_ensemble_get_accuracy(ctx.get(), datum_nad27.get()));
    EXPECT_THAT(proj_datum_ensemble_get_member(ctx.get(), nullptr, 0),
                IsNull());
    EXPECT_THAT(proj_datum_ensemble_get_member(ctx.get(), datum_nad27.get(), 0),
                IsNull());
    EXPECT_THAT(proj_datum_ensemble_get_member(ctx.get(), ens_wgs.get(), -1),
                IsNull());
    EXPECT_THAT(proj_datum_ensemble_get_member(ctx.get(), ens_wgs.get(), 999),
                IsNull());
    EXPECT_DOUBLE_EQ(
        -1.0, proj_dynamic_datum_get_frame_reference_epoch(ctx.get(), nullptr));
    EXPECT_DOUBLE_EQ(-1.0, proj_dynamic_datum_get_frame_reference_epoch(
                               ctx.get(), datum_nad27.get()));
  }
  EXPECT_GT(proj_datum_ensemble_get_member_count(ctx.get(), ens_wgs.get()), 0);
  EXPECT_DOUBLE_EQ(2.0,
                   proj_datum_ensemble_get_accuracy(ctx.get(), ens_wgs.get()));
  ScopedPj member0(proj_datum_ensemble_get_member(ctx.get(), ens_wgs.get(), 0));
  EXPECT_THAT(member0.get(), NotNull());

  EXPECT_DOUBLE_EQ(2005.0, proj_dynamic_datum_get_frame_reference_epoch(
                               ctx.get(), dyn_g_datum.get()));
  EXPECT_DOUBLE_EQ(2000.0, proj_dynamic_datum_get_frame_reference_epoch(
                               ctx.get(), dyn_v_datum.get()));

  // Ellipsoid, celestial body, prime meridian.
  {
    ScopedLogSuppressor suppress(ctx.get());
    // Never pass nullptr to proj_get_ellipsoid / proj_get_celestial_body_name /
    // proj_get_prime_meridian; pass non-null wrong-type objects instead.
    EXPECT_THAT(proj_get_ellipsoid(ctx.get(), conv.get()), IsNull());
    EXPECT_THAT(proj_get_ellipsoid(ctx.get(), vert_crs.get()), IsNull());
    EXPECT_EQ(0, proj_ellipsoid_get_parameters(ctx.get(), nullptr, nullptr,
                                               nullptr, nullptr, nullptr));
    EXPECT_EQ(0,
              proj_ellipsoid_get_parameters(ctx.get(), pm_paris.get(), nullptr,
                                            nullptr, nullptr, nullptr));
    EXPECT_THAT(proj_get_celestial_body_name(ctx.get(), conv.get()), IsNull());
    EXPECT_THAT(proj_get_prime_meridian(ctx.get(), conv.get()), IsNull());
    EXPECT_THAT(proj_get_prime_meridian(ctx.get(), vert_crs.get()), IsNull());
    EXPECT_EQ(0, proj_prime_meridian_get_parameters(ctx.get(), nullptr, nullptr,
                                                    nullptr, nullptr));
    EXPECT_EQ(0, proj_prime_meridian_get_parameters(ctx.get(), ellps.get(),
                                                    nullptr, nullptr, nullptr));
  }
  ScopedPj ellps_from_crs(proj_get_ellipsoid(ctx.get(), geog2d.get()));
  ScopedPj ellps_from_datum(proj_get_ellipsoid(ctx.get(), datum_nad27.get()));
  ASSERT_THAT(ellps_from_crs.get(), NotNull());
  ASSERT_THAT(ellps_from_datum.get(), NotNull());

  double semi_major = 0.0;
  double semi_minor = 0.0;
  int is_computed = -1;
  double inv_flat = 0.0;
  EXPECT_EQ(1, proj_ellipsoid_get_parameters(ctx.get(), ellps_from_crs.get(),
                                             &semi_major, &semi_minor,
                                             &is_computed, &inv_flat));
  EXPECT_DOUBLE_EQ(6378137.0, semi_major);
  EXPECT_GT(semi_minor, 6350000.0);
  EXPECT_EQ(1, is_computed);
  EXPECT_NEAR(298.257223563, inv_flat, 1e-9);
  EXPECT_EQ(1, proj_ellipsoid_get_parameters(ctx.get(), ellps.get(), nullptr,
                                             nullptr, nullptr, nullptr));

  ScopedPj v_datum(proj_crs_get_datum(ctx.get(), vert_crs.get()));
  ASSERT_THAT(v_datum.get(), NotNull());
  EXPECT_THAT(proj_get_celestial_body_name(ctx.get(), geog2d.get()),
              StrEq("Earth"));
  EXPECT_THAT(proj_get_celestial_body_name(ctx.get(), moon_crs.get()),
              StrEq("Moon"));
  EXPECT_THAT(proj_get_celestial_body_name(ctx.get(), vert_crs.get()),
              StrEq("Earth"));
  EXPECT_THAT(proj_get_celestial_body_name(ctx.get(), ens_wgs.get()),
              StrEq("Earth"));
  EXPECT_THAT(proj_get_celestial_body_name(ctx.get(), datum_nad27.get()),
              StrEq("Earth"));
  EXPECT_THAT(proj_get_celestial_body_name(ctx.get(), v_datum.get()),
              StrEq("Earth"));
  EXPECT_THAT(proj_get_celestial_body_name(ctx.get(), ellps.get()),
              StrEq("Earth"));

  ScopedPj pm_from_crs(proj_get_prime_meridian(ctx.get(), geog2d.get()));
  ScopedPj pm_from_datum(proj_get_prime_meridian(ctx.get(), datum_nad27.get()));
  ASSERT_THAT(pm_from_crs.get(), NotNull());
  ASSERT_THAT(pm_from_datum.get(), NotNull());

  double pm_lon = 0.0;
  double pm_conv = 0.0;
  const char* pm_unit = nullptr;
  EXPECT_EQ(1, proj_prime_meridian_get_parameters(ctx.get(), pm_paris.get(),
                                                  &pm_lon, &pm_conv, &pm_unit));
  EXPECT_GT(pm_lon, 2.5);
  EXPECT_GT(pm_conv, 0.0);
  EXPECT_THAT(pm_unit, NotNull());
  EXPECT_EQ(1, proj_prime_meridian_get_parameters(ctx.get(), pm_from_crs.get(),
                                                  nullptr, nullptr, nullptr));

  // Coordinate system types and axis introspection.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_crs_get_coordinate_system(ctx.get(), nullptr), IsNull());
    EXPECT_THAT(proj_crs_get_coordinate_system(ctx.get(), compound.get()),
                IsNull());
    EXPECT_EQ(PJ_CS_TYPE_UNKNOWN, proj_cs_get_type(ctx.get(), nullptr));
    EXPECT_EQ(PJ_CS_TYPE_UNKNOWN, proj_cs_get_type(ctx.get(), ellps.get()));
    EXPECT_EQ(-1, proj_cs_get_axis_count(ctx.get(), nullptr));
    EXPECT_EQ(-1, proj_cs_get_axis_count(ctx.get(), ellps.get()));
    EXPECT_EQ(
        0, proj_cs_get_axis_info(ctx.get(), nullptr, 0, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(
        0, proj_cs_get_axis_info(ctx.get(), ellps.get(), 0, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr));
  }

  struct CsCheck {
    const PJ* crs;
    PJ_COORDINATE_SYSTEM_TYPE expected_type;
    int expected_axes;
  };
  const CsCheck kCsChecks[] = {
      {proj_crs.get(), PJ_CS_TYPE_CARTESIAN, 2},
      {geog2d.get(), PJ_CS_TYPE_ELLIPSOIDAL, 2},
      {vert_crs.get(), PJ_CS_TYPE_VERTICAL, 1},
      {sph_geod.get(), PJ_CS_TYPE_SPHERICAL, 3},
      {ord_crs.get(), PJ_CS_TYPE_ORDINAL, 2},
      {p_crs.get(), PJ_CS_TYPE_PARAMETRIC, 1},
      {t_dt.get(), PJ_CS_TYPE_DATETIMETEMPORAL, 1},
      {t_cnt.get(), PJ_CS_TYPE_TEMPORALCOUNT, 1},
      {t_crs.get(), PJ_CS_TYPE_TEMPORALMEASURE, 1},
  };
  for (const auto& check : kCsChecks) {
    ScopedPj cs(proj_crs_get_coordinate_system(ctx.get(), check.crs));
    ASSERT_THAT(cs.get(), NotNull());
    EXPECT_EQ(check.expected_type, proj_cs_get_type(ctx.get(), cs.get()));
    EXPECT_EQ(check.expected_axes, proj_cs_get_axis_count(ctx.get(), cs.get()));
  }

  ScopedPj cs_geog(proj_crs_get_coordinate_system(ctx.get(), geog2d.get()));
  ASSERT_THAT(cs_geog.get(), NotNull());
  const char* ax_name = nullptr;
  const char* ax_abbr = nullptr;
  const char* ax_dir = nullptr;
  double ax_conv = 0.0;
  const char* ax_unit = nullptr;
  const char* ax_auth = nullptr;
  const char* ax_code = nullptr;
  EXPECT_EQ(1, proj_cs_get_axis_info(ctx.get(), cs_geog.get(), 0, &ax_name,
                                     &ax_abbr, &ax_dir, &ax_conv, &ax_unit,
                                     &ax_auth, &ax_code));
  EXPECT_THAT(ax_dir, StrEq("north"));
  EXPECT_EQ(1,
            proj_cs_get_axis_info(ctx.get(), cs_geog.get(), 1, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr));
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(
        0, proj_cs_get_axis_info(ctx.get(), cs_geog.get(), -1, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(
        0, proj_cs_get_axis_info(ctx.get(), cs_geog.get(), 2, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr));
  }

  // Alteration APIs.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_alter_name(ctx.get(), nullptr, "X"), IsNull());
    EXPECT_THAT(proj_alter_name(ctx.get(), geog2d.get(), nullptr), IsNull());
    EXPECT_THAT(proj_alter_id(ctx.get(), nullptr, "A", "1"), IsNull());
    EXPECT_THAT(proj_crs_alter_geodetic_crs(ctx.get(), nullptr, nad27.get()),
                IsNull());
    EXPECT_THAT(
        proj_crs_alter_geodetic_crs(ctx.get(), proj_crs.get(), vert_crs.get()),
        IsNull());
    EXPECT_THAT(
        proj_crs_alter_geodetic_crs(ctx.get(), ellps.get(), nad27.get()),
        IsNull());
    EXPECT_THAT(
        proj_crs_alter_cs_angular_unit(ctx.get(), vert_crs.get(), "grad",
                                       kGradToRad, nullptr, nullptr),
        IsNull());
    EXPECT_THAT(proj_crs_alter_cs_linear_unit(ctx.get(), nullptr, "foot",
                                              kFootToMetre, nullptr, nullptr),
                IsNull());
    EXPECT_THAT(
        proj_crs_alter_parameters_linear_unit(
            ctx.get(), nullptr, "foot", kFootToMetre, nullptr, nullptr, 1),
        IsNull());
    EXPECT_THAT(proj_crs_promote_to_3D(ctx.get(), "bad", nullptr), IsNull());
    EXPECT_THAT(proj_crs_promote_to_3D(ctx.get(), "bad", ellps.get()),
                IsNull());
    ScopedPj t_crs_3d(proj_crs_promote_to_3D(ctx.get(), "bad", t_crs.get()));
    EXPECT_THAT(t_crs_3d.get(), NotNull());
    EXPECT_THAT(proj_crs_create_projected_3D_crs_from_2D(ctx.get(), "bad",
                                                         nullptr, geog3d.get()),
                IsNull());
    EXPECT_THAT(proj_crs_create_projected_3D_crs_from_2D(
                    ctx.get(), "bad", geog2d.get(), geog3d.get()),
                IsNull());
    EXPECT_THAT(proj_crs_create_projected_3D_crs_from_2D(
                    ctx.get(), "bad", proj_crs.get(), ellps.get()),
                IsNull());
    EXPECT_THAT(proj_crs_create_projected_3D_crs_from_2D(
                    ctx.get(), "bad", proj_crs.get(), geog2d.get()),
                IsNull());
    EXPECT_THAT(proj_crs_demote_to_2D(ctx.get(), "bad", nullptr), IsNull());
    EXPECT_THAT(proj_crs_demote_to_2D(ctx.get(), "bad", ellps.get()), IsNull());
  }
  EXPECT_THAT(proj_alter_name(ctx.get(), ellps.get(), "X"), IsNull());
  EXPECT_THAT(proj_alter_id(ctx.get(), ellps.get(), "A", "1"), IsNull());
  // Geocentric CRS is GeodeticCRS but not GeographicCRS.
  EXPECT_THAT(proj_crs_alter_cs_angular_unit(ctx.get(), geoc.get(), "grad",
                                             kGradToRad, nullptr, nullptr),
              IsNull());
  EXPECT_THAT(proj_crs_alter_cs_linear_unit(ctx.get(), ellps.get(), "foot",
                                            kFootToMetre, nullptr, nullptr),
              IsNull());
  EXPECT_THAT(
      proj_crs_alter_parameters_linear_unit(ctx.get(), geog2d.get(), "foot",
                                            kFootToMetre, nullptr, nullptr, 1),
      IsNull());

  ScopedPj renamed(proj_alter_name(ctx.get(), geog2d.get(), "CustomWGS84"));
  ASSERT_THAT(renamed.get(), NotNull());
  EXPECT_THAT(proj_get_name(renamed.get()), StrEq("CustomWGS84"));

  ScopedPj reid(proj_alter_id(ctx.get(), renamed.get(), "MYAUTH", "42"));
  ASSERT_THAT(reid.get(), NotNull());
  EXPECT_THAT(proj_get_id_auth_name(reid.get(), 0), StrEq("MYAUTH"));
  EXPECT_THAT(proj_get_id_code(reid.get(), 0), StrEq("42"));

  ScopedPj altered_geod(
      proj_crs_alter_geodetic_crs(ctx.get(), proj_crs.get(), nad27.get()));
  ASSERT_THAT(altered_geod.get(), NotNull());

  ScopedPj grad_geog(proj_crs_alter_cs_angular_unit(
      ctx.get(), geog2d.get(), "grad", kGradToRad, "EPSG", "9105"));
  ASSERT_THAT(grad_geog.get(), NotNull());
  ScopedPj deg_geog(proj_crs_alter_cs_angular_unit(
      ctx.get(), grad_geog.get(), nullptr, 0.0, nullptr, nullptr));
  ASSERT_THAT(deg_geog.get(), NotNull());

  ScopedPj ft_proj(proj_crs_alter_cs_linear_unit(
      ctx.get(), proj_crs.get(), "foot", kFootToMetre, "EPSG", "9002"));
  ASSERT_THAT(ft_proj.get(), NotNull());
  ScopedPj m_proj(proj_crs_alter_cs_linear_unit(
      ctx.get(), ft_proj.get(), nullptr, 0.0, nullptr, nullptr));
  ASSERT_THAT(m_proj.get(), NotNull());

  ScopedPj ft_param_conv(proj_crs_alter_parameters_linear_unit(
      ctx.get(), proj_crs.get(), "foot", kFootToMetre, "EPSG", "9002", 1));
  ASSERT_THAT(ft_param_conv.get(), NotNull());
  ScopedPj ft_param_raw(proj_crs_alter_parameters_linear_unit(
      ctx.get(), proj_crs.get(), "foot", kFootToMetre, nullptr, nullptr, 0));
  ASSERT_THAT(ft_param_raw.get(), NotNull());

  // Promote 2D CRS & CoordinateMetadata (with and without epoch) and demote.
  ScopedPj promoted_geog(
      proj_crs_promote_to_3D(ctx.get(), "WGS 84 3D", geog2d.get()));
  ASSERT_THAT(promoted_geog.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_GEOGRAPHIC_3D_CRS, proj_get_type(promoted_geog.get()));

  ScopedPj promoted_default_name(
      proj_crs_promote_to_3D(ctx.get(), nullptr, geog2d.get()));
  ASSERT_THAT(promoted_default_name.get(), NotNull());

  ScopedPj cm_with_epoch(
      proj_coordinate_metadata_create(ctx.get(), itrf2014.get(), 2020.0));
  ASSERT_THAT(cm_with_epoch.get(), NotNull());
  ScopedPj cm_with_epoch_3d(
      proj_crs_promote_to_3D(ctx.get(), nullptr, cm_with_epoch.get()));
  ASSERT_THAT(cm_with_epoch_3d.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_COORDINATE_METADATA, proj_get_type(cm_with_epoch_3d.get()));
  EXPECT_DOUBLE_EQ(2020.0, proj_coordinate_metadata_get_epoch(
                               ctx.get(), cm_with_epoch_3d.get()));

  ScopedPj cm_no_epoch(proj_create_from_wkt(
      ctx.get(), kCoordMetaWgs84NoEpochWkt, nullptr, nullptr, nullptr));
  ASSERT_THAT(cm_no_epoch.get(), NotNull());
  ScopedPj cm_no_epoch_3d(
      proj_crs_promote_to_3D(ctx.get(), "CM 3D", cm_no_epoch.get()));
  ASSERT_THAT(cm_no_epoch_3d.get(), NotNull());
  EXPECT_TRUE(std::isnan(
      proj_coordinate_metadata_get_epoch(ctx.get(), cm_no_epoch_3d.get())));

  ScopedPj proj3d_explicit(proj_crs_create_projected_3D_crs_from_2D(
      ctx.get(), "UTM 31N 3D", proj_crs.get(), geog3d.get()));
  ASSERT_THAT(proj3d_explicit.get(), NotNull());
  ScopedPj proj3d_auto(proj_crs_create_projected_3D_crs_from_2D(
      ctx.get(), nullptr, proj_crs.get(), nullptr));
  ASSERT_THAT(proj3d_auto.get(), NotNull());

  ScopedPj demoted_custom(
      proj_crs_demote_to_2D(ctx.get(), "Demoted", geog3d.get()));
  ASSERT_THAT(demoted_custom.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_GEOGRAPHIC_2D_CRS, proj_get_type(demoted_custom.get()));
  ScopedPj demoted_default(
      proj_crs_demote_to_2D(ctx.get(), nullptr, proj3d_explicit.get()));
  ASSERT_THAT(demoted_default.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_PROJECTED_CRS, proj_get_type(demoted_default.get()));
}

TEST(Iso19111CApiTest, ManualFactoriesConversionsAndBoundCrs) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // Mutable buffers required by non-const char* fields in PJ_AXIS_DESCRIPTION.
  char ax_x[] = "Easting";
  char ax_y[] = "Northing";
  char ax_z[] = "Up";
  char abbr_e[] = "E";
  char abbr_n[] = "N";
  char abbr_u[] = "U";
  char dir_e[] = "east";
  char dir_n[] = "north";
  char dir_u[] = "up";
  char unit_m[] = "metre";
  char unit_deg[] = "degree";
  char unit_hpa[] = "hPa";
  char unit_s[] = "second";
  char unit_unity[] = "unity";
  char bad_dir[] = "not_a_valid_direction";

  PJ_AXIS_DESCRIPTION cart_axes[3] = {
      {ax_x, abbr_e, dir_e, unit_m, 1.0, PJ_UT_LINEAR},
      {ax_y, abbr_n, dir_n, unit_m, 1.0, PJ_UT_LINEAR},
      {ax_z, abbr_u, dir_u, unit_m, 1.0, PJ_UT_LINEAR},
  };
  ScopedPj manual_cart2(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_CARTESIAN, 2, cart_axes));
  ASSERT_THAT(manual_cart2.get(), NotNull());
  EXPECT_EQ(PJ_CS_TYPE_CARTESIAN,
            proj_cs_get_type(ctx.get(), manual_cart2.get()));
  // Unnamed CartesianCS exercises proj_get_name() -> nullptr (no description).
  EXPECT_THAT(proj_get_name(manual_cart2.get()), IsNull());

  ScopedPj manual_cart3(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_CARTESIAN, 3, cart_axes));
  ASSERT_THAT(manual_cart3.get(), NotNull());

  PJ_AXIS_DESCRIPTION ellps_axes[3] = {
      {ax_x, abbr_e, dir_e, unit_deg, kDegToRad, PJ_UT_ANGULAR},
      {ax_y, abbr_n, dir_n, unit_deg, kDegToRad, PJ_UT_ANGULAR},
      {ax_z, abbr_u, dir_u, unit_m, 1.0, PJ_UT_LINEAR},
  };
  ScopedPj manual_ellps2(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_ELLIPSOIDAL, 2, ellps_axes));
  ScopedPj manual_ellps3(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_ELLIPSOIDAL, 3, ellps_axes));
  // Note: upstream c_api.cpp constructs EllipsoidalCS for PJ_CS_TYPE_SPHERICAL.
  ScopedPj manual_sph3(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_SPHERICAL, 3, ellps_axes));
  ASSERT_THAT(manual_ellps2.get(), NotNull());
  ASSERT_THAT(manual_ellps3.get(), NotNull());
  ASSERT_THAT(manual_sph3.get(), NotNull());
  EXPECT_EQ(PJ_CS_TYPE_ELLIPSOIDAL,
            proj_cs_get_type(ctx.get(), manual_sph3.get()));

  PJ_AXIS_DESCRIPTION vert_axis = {ax_z,   abbr_u, dir_u,
                                   unit_m, 1.0,    PJ_UT_LINEAR};
  ScopedPj manual_vert(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_VERTICAL, 1, &vert_axis));
  ASSERT_THAT(manual_vert.get(), NotNull());
  EXPECT_EQ(PJ_CS_TYPE_VERTICAL,
            proj_cs_get_type(ctx.get(), manual_vert.get()));

  PJ_AXIS_DESCRIPTION param_axis = {ax_z,     abbr_u, dir_u,
                                    unit_hpa, 100.0,  PJ_UT_PARAMETRIC};
  ScopedPj manual_param(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_PARAMETRIC, 1, &param_axis));
  ASSERT_THAT(manual_param.get(), NotNull());
  EXPECT_EQ(PJ_CS_TYPE_PARAMETRIC,
            proj_cs_get_type(ctx.get(), manual_param.get()));

  PJ_AXIS_DESCRIPTION ord_axes[2] = {
      {ax_x, abbr_e, dir_e, unit_unity, 1.0, PJ_UT_SCALE},
      {ax_y, abbr_n, dir_n, unit_unity, 1.0, PJ_UT_SCALE},
  };
  ScopedPj manual_ord(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_ORDINAL, 2, ord_axes));
  ASSERT_THAT(manual_ord.get(), NotNull());
  EXPECT_EQ(PJ_CS_TYPE_ORDINAL, proj_cs_get_type(ctx.get(), manual_ord.get()));

  char ax_t[] = "Time";
  char abbr_t[] = "t";
  char dir_f[] = "future";
  PJ_AXIS_DESCRIPTION time_axis = {ax_t,   abbr_t, dir_f,
                                   unit_s, 1.0,    PJ_UT_TIME};
  ScopedPj manual_dt(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_DATETIMETEMPORAL, 1, &time_axis));
  ScopedPj manual_cnt(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_TEMPORALCOUNT, 1, &time_axis));
  ScopedPj manual_meas(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_TEMPORALMEASURE, 1, &time_axis));
  ASSERT_THAT(manual_dt.get(), NotNull());
  ASSERT_THAT(manual_cnt.get(), NotNull());
  ASSERT_THAT(manual_meas.get(), NotNull());

  // proj_create_cs error branches (unknown type, wrong axis_count, bad dir).
  EXPECT_THAT(proj_create_cs(ctx.get(), PJ_CS_TYPE_UNKNOWN, 0, nullptr),
              IsNull());
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_create_cs(ctx.get(), PJ_CS_TYPE_CARTESIAN, 0, nullptr),
                IsNull());
    EXPECT_THAT(
        proj_create_cs(ctx.get(), PJ_CS_TYPE_ELLIPSOIDAL, 1, ellps_axes),
        IsNull());
    EXPECT_THAT(proj_create_cs(ctx.get(), PJ_CS_TYPE_VERTICAL, 2, cart_axes),
                IsNull());
    EXPECT_THAT(proj_create_cs(ctx.get(), PJ_CS_TYPE_SPHERICAL, 2, cart_axes),
                IsNull());
    EXPECT_THAT(proj_create_cs(ctx.get(), PJ_CS_TYPE_PARAMETRIC, 2, cart_axes),
                IsNull());
    EXPECT_THAT(
        proj_create_cs(ctx.get(), PJ_CS_TYPE_DATETIMETEMPORAL, 2, cart_axes),
        IsNull());
    EXPECT_THAT(
        proj_create_cs(ctx.get(), PJ_CS_TYPE_TEMPORALCOUNT, 2, cart_axes),
        IsNull());
    EXPECT_THAT(
        proj_create_cs(ctx.get(), PJ_CS_TYPE_TEMPORALMEASURE, 2, cart_axes),
        IsNull());
    PJ_AXIS_DESCRIPTION bad_axis = {ax_x,   abbr_e, bad_dir,
                                    unit_m, 1.0,    PJ_UT_LINEAR};
    EXPECT_THAT(proj_create_cs(ctx.get(), PJ_CS_TYPE_VERTICAL, 1, &bad_axis),
                IsNull());
  }

  // Convenience CS builders.
  const PJ_CARTESIAN_CS_2D_TYPE cart_2d_types[] = {
      PJ_CART2D_EASTING_NORTHING,
      PJ_CART2D_NORTHING_EASTING,
      PJ_CART2D_NORTH_POLE_EASTING_SOUTH_NORTHING_SOUTH,
      PJ_CART2D_SOUTH_POLE_EASTING_NORTH_NORTHING_NORTH,
      PJ_CART2D_WESTING_SOUTHING,
  };
  for (PJ_CARTESIAN_CS_2D_TYPE ct : cart_2d_types) {
    ScopedPj cs2d(proj_create_cartesian_2D_cs(ctx.get(), ct, nullptr, 0.0));
    ASSERT_THAT(cs2d.get(), NotNull());
  }
  ScopedPj cart2d_ft(proj_create_cartesian_2D_cs(
      ctx.get(), PJ_CART2D_EASTING_NORTHING, "foot", kFootToMetre));
  ASSERT_THAT(cart2d_ft.get(), NotNull());

  ScopedPj ellps2d_ll(proj_create_ellipsoidal_2D_cs(
      ctx.get(), PJ_ELLPS2D_LONGITUDE_LATITUDE, "grad", kGradToRad));
  ScopedPj ellps2d_nl(proj_create_ellipsoidal_2D_cs(
      ctx.get(), PJ_ELLPS2D_LATITUDE_LONGITUDE, nullptr, 0.0));
  ASSERT_THAT(ellps2d_ll.get(), NotNull());
  ASSERT_THAT(ellps2d_nl.get(), NotNull());

  ScopedPj ellps3d_ll(proj_create_ellipsoidal_3D_cs(
      ctx.get(), PJ_ELLPS3D_LONGITUDE_LATITUDE_HEIGHT, "radian", 1.0, "foot",
      kFootToMetre));
  ScopedPj ellps3d_nl(proj_create_ellipsoidal_3D_cs(
      ctx.get(), PJ_ELLPS3D_LATITUDE_LONGITUDE_HEIGHT, nullptr, 0.0, nullptr,
      0.0));
  ASSERT_THAT(ellps3d_ll.get(), NotNull());
  ASSERT_THAT(ellps3d_nl.get(), NotNull());

  // Geographic & Geocentric CRS constructors.
  ScopedPj geog_wgs_alias(proj_create_geographic_crs(
      ctx.get(), "CustomWgs", "WGS_1984", "WGS 84", 6378137.0, 298.257223563,
      nullptr, 0.0, nullptr, 0.0, ellps2d_nl.get()));
  ASSERT_THAT(geog_wgs_alias.get(), NotNull());

  ScopedPj geog_nad_alias(proj_create_geographic_crs(
      ctx.get(), "Nad83", "D_North_American_1983", "GRS 1980", 6378137.0,
      298.257222101, "Greenwich", 0.0, "degree", kDegToRad, ellps2d_nl.get()));
  ASSERT_THAT(geog_nad_alias.get(), NotNull());

  ScopedPj geog_sphere(proj_create_geographic_crs(
      ctx.get(), nullptr, nullptr, nullptr, 6371000.0, 0.0, nullptr, 2.5,
      "grad", kGradToRad, ellps2d_ll.get()));
  ASSERT_THAT(geog_sphere.get(), NotNull());

  // Verify spherical ellipsoid parameters path (is_semi_minor_computed == 1).
  ScopedPj sphere_ellps(proj_get_ellipsoid(ctx.get(), geog_sphere.get()));
  ASSERT_THAT(sphere_ellps.get(), NotNull());
  int sphere_computed = -1;
  double sphere_inv_f = -1.0;
  EXPECT_EQ(1, proj_ellipsoid_get_parameters(ctx.get(), sphere_ellps.get(),
                                             nullptr, nullptr, &sphere_computed,
                                             &sphere_inv_f));
  EXPECT_EQ(1, sphere_computed);
  EXPECT_DOUBLE_EQ(0.0, sphere_inv_f);

  // Non-Earth celestial body -> REFERENCE_MERIDIAN default PM name.
  ScopedPj moon_manual(proj_create_geographic_crs(
      ctx.get(), "ManualMoon", "MoonDatum", "MoonSphere", 1737400.0, 0.0,
      nullptr, 0.0, nullptr, 0.0, ellps2d_nl.get()));
  ASSERT_THAT(moon_manual.get(), NotNull());
  EXPECT_THAT(proj_get_celestial_body_name(ctx.get(), moon_manual.get()),
              StrEq("Moon"));

  // Never pass nullptr for ellipsoidal_cs; pass non-null manual_cart2.
  EXPECT_THAT(proj_create_geographic_crs(ctx.get(), "BadCS", "D", "E",
                                         6378137.0, 298.257223563, nullptr, 0.0,
                                         nullptr, 0.0, manual_cart2.get()),
              IsNull());
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_create_geographic_crs(ctx.get(), "NegRadius", "D", "E",
                                           -100.0, 0.0, nullptr, 0.0, nullptr,
                                           0.0, ellps2d_nl.get()),
                IsNull());
  }

  ScopedPj datum_6258(proj_create_from_database(ctx.get(), "EPSG", "6258",
                                                PJ_CATEGORY_DATUM, 0, nullptr));
  ScopedPj ens_6326(proj_create_from_database(
      ctx.get(), "EPSG", "6326", PJ_CATEGORY_DATUM_ENSEMBLE, 0, nullptr));
  ASSERT_THAT(datum_6258.get(), NotNull());
  ASSERT_THAT(ens_6326.get(), NotNull());

  ScopedPj geog_from_datum(proj_create_geographic_crs_from_datum(
      ctx.get(), "EtrsFromDatum", datum_6258.get(), ellps2d_nl.get()));
  ScopedPj geog_from_ens(proj_create_geographic_crs_from_datum(
      ctx.get(), "WgsFromEns", ens_6326.get(), ellps2d_nl.get()));
  ASSERT_THAT(geog_from_datum.get(), NotNull());
  ASSERT_THAT(geog_from_ens.get(), NotNull());
  EXPECT_THAT(proj_create_geographic_crs_from_datum(
                  ctx.get(), "BadCS", datum_6258.get(), manual_cart2.get()),
              IsNull());
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_create_geographic_crs_from_datum(
                    ctx.get(), "NullDatum", nullptr, ellps2d_nl.get()),
                IsNull());
    EXPECT_THAT(
        proj_create_geographic_crs_from_datum(
            ctx.get(), "NotADatum", manual_cart2.get(), ellps2d_nl.get()),
        IsNull());
  }

  ScopedPj geoc_manual(proj_create_geocentric_crs(
      ctx.get(), "CustomGeoc", "D", "E", 6378137.0, 298.257223563, nullptr, 0.0,
      nullptr, 0.0, "foot", kFootToMetre));
  ASSERT_THAT(geoc_manual.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_GEOCENTRIC_CRS, proj_get_type(geoc_manual.get()));
  ScopedPj geoc_from_datum(proj_create_geocentric_crs_from_datum(
      ctx.get(), "GeocDatum", datum_6258.get(), nullptr, 0.0));
  ScopedPj geoc_from_ens(proj_create_geocentric_crs_from_datum(
      ctx.get(), "GeocEns", ens_6326.get(), "metre", 1.0));
  ASSERT_THAT(geoc_from_datum.get(), NotNull());
  ASSERT_THAT(geoc_from_ens.get(), NotNull());
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_create_geocentric_crs_from_datum(ctx.get(), "Bad", nullptr,
                                                      nullptr, 0.0),
                IsNull());
    EXPECT_THAT(proj_create_geocentric_crs_from_datum(
                    ctx.get(), "Bad", manual_cart2.get(), nullptr, 0.0),
                IsNull());
    EXPECT_THAT(
        proj_create_geocentric_crs(ctx.get(), "Neg", "D", "E", -10.0, 0.0,
                                   nullptr, 0.0, nullptr, 0.0, nullptr, 0.0),
        IsNull());
  }

  // Derived geographic CRS (using Pole Rotation conversion).
  ScopedPj pole_conv(proj_create_conversion_pole_rotation_netcdf_cf_convention(
      ctx.get(), 39.25, -162.0, 18.0, "degree", kDegToRad));
  ASSERT_THAT(pole_conv.get(), NotNull());
  ScopedPj derived_geog(proj_create_derived_geographic_crs(
      ctx.get(), "RotatedLatLon", geog_from_datum.get(), pole_conv.get(),
      ellps2d_ll.get()));
  ASSERT_THAT(derived_geog.get(), NotNull());
  EXPECT_EQ(1, proj_is_derived_crs(ctx.get(), derived_geog.get()));
  EXPECT_THAT(proj_create_derived_geographic_crs(
                  ctx.get(), "WrongTypes", manual_cart2.get(), pole_conv.get(),
                  ellps2d_ll.get()),
              IsNull());

  // Vertical CRS (simple + _ex with ACCURACY & interpolation CRS branches).
  ScopedPj vert_simple(proj_create_vertical_crs(
      ctx.get(), "SimpleHeight", "CustomVDatum", "foot", kFootToMetre));
  ASSERT_THAT(vert_simple.get(), NotNull());

  const char* const vert_opts[] = {"ACCURACY=0.05", nullptr};
  ScopedPj vert_geoid_geog(proj_create_vertical_crs_ex(
      ctx.get(), "VertWithGeoid", "VDatum", "MYAUTH", "V1", nullptr, 0.0,
      "PROJ egm96_15.gtx", "MYAUTH", "G1", geog_from_ens.get(), vert_opts));
  ScopedPj vert_geoid_null_interp(proj_create_vertical_crs_ex(
      ctx.get(), "VertNoInterp", "VDatum", nullptr, nullptr, "metre", 1.0,
      "EGM96", nullptr, nullptr, nullptr, nullptr));
  ASSERT_THAT(vert_geoid_geog.get(), NotNull());
  ASSERT_THAT(vert_geoid_null_interp.get(), NotNull());

  // Compound and Engineering CRS (including "(deprecated)" suffix handling).
  ScopedPj manual_eng(proj_create_engineering_crs(ctx.get(), "LocalSite"));
  ScopedPj manual_eng_depr(
      proj_create_engineering_crs(ctx.get(), "LegacySite (deprecated)"));
  ASSERT_THAT(manual_eng.get(), NotNull());
  ASSERT_THAT(manual_eng_depr.get(), NotNull());
  EXPECT_THAT(proj_get_name(manual_eng_depr.get()), StrEq("LegacySite"));
  EXPECT_EQ(1, proj_is_deprecated(manual_eng_depr.get()));

  ScopedPj manual_comp(proj_create_compound_crs(
      ctx.get(), "HorizPlusVert", geog_from_datum.get(), vert_simple.get()));
  ASSERT_THAT(manual_comp.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_COMPOUND_CRS, proj_get_type(manual_comp.get()));
  EXPECT_THAT(proj_create_compound_crs(ctx.get(), "BadH", manual_cart2.get(),
                                       vert_simple.get()),
              IsNull());
  EXPECT_THAT(proj_create_compound_crs(ctx.get(), "BadV", geog_from_datum.get(),
                                       manual_cart2.get()),
              IsNull());
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_create_compound_crs(ctx.get(), "NullH", nullptr,
                                         vert_simple.get()),
                IsNull());
    EXPECT_THAT(proj_create_compound_crs(ctx.get(), "CompoundInCompound",
                                         manual_comp.get(), vert_simple.get()),
                IsNull());
  }

  // Projected CRS & DerivedProjectedCRS & horizontal derived conversion.
  ScopedPj utm_conv(proj_create_conversion_utm(ctx.get(), 31, 1));
  ASSERT_THAT(utm_conv.get(), NotNull());
  ScopedPj manual_proj(proj_create_projected_crs(
      ctx.get(), "ManualUTM31N", geog_from_datum.get(), utm_conv.get(),
      manual_cart2.get()));
  ASSERT_THAT(manual_proj.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_PROJECTED_CRS, proj_get_type(manual_proj.get()));
  EXPECT_THAT(proj_create_projected_crs(ctx.get(), "Bad", manual_cart2.get(),
                                        utm_conv.get(), manual_cart2.get()),
              IsNull());
  EXPECT_THAT(proj_create_projected_crs(ctx.get(), "Bad", geog_from_datum.get(),
                                        manual_cart2.get(), manual_cart2.get()),
              IsNull());
  EXPECT_THAT(proj_create_projected_crs(ctx.get(), "Bad", geog_from_datum.get(),
                                        utm_conv.get(), ellps2d_ll.get()),
              IsNull());
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_create_projected_crs(ctx.get(), "Null", nullptr,
                                          utm_conv.get(), manual_cart2.get()),
                IsNull());
  }

  ScopedPj affine_2d(proj_create_linear_affine_parametric_conversion(
      ctx.get(), nullptr, 100.0, "metre", 1.0, 1.0, "unity", 1.0, 0.0, nullptr,
      0.0, 200.0, nullptr, 0.0, 0.0, nullptr, 0.0, 1.0, "unity", 1.0));
  ScopedPj affine_3d(proj_create_linear_3D_affine_parametric_conversion(
      ctx.get(), "Affine3D", 1.0, nullptr, 0.0, 1.0, nullptr, 0.0, 0.0, nullptr,
      0.0, 0.0, nullptr, 0.0, 2.0, "metre", 1.0, 0.0, "unity", 1.0, 1.0,
      nullptr, 0.0, 0.0, nullptr, 0.0, 3.0, nullptr, 0.0, 0.0, nullptr, 0.0,
      0.0, nullptr, 0.0, 1.0, nullptr, 0.0));
  ASSERT_THAT(affine_2d.get(), NotNull());
  ASSERT_THAT(affine_3d.get(), NotNull());

  ScopedPj deriv_proj(proj_create_derived_projected_crs(
      ctx.get(), "ManualDerivProj", manual_proj.get(), affine_2d.get(),
      manual_cart2.get()));
  ASSERT_THAT(deriv_proj.get(), NotNull());
  EXPECT_THAT(
      proj_create_derived_projected_crs(ctx.get(), "Bad", geog_from_datum.get(),
                                        affine_2d.get(), manual_cart2.get()),
      IsNull());
  EXPECT_THAT(
      proj_create_derived_projected_crs(ctx.get(), "Bad", manual_proj.get(),
                                        manual_cart2.get(), manual_cart2.get()),
      IsNull());
  EXPECT_THAT(
      proj_create_derived_projected_crs(ctx.get(), "Bad", manual_proj.get(),
                                        affine_2d.get(), ellps2d_ll.get()),
      IsNull());
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(
        proj_create_derived_projected_crs(ctx.get(), "Null", nullptr,
                                          affine_2d.get(), manual_cart2.get()),
        IsNull());
  }

  // proj_crs_add_horizontal_derived_conversion on Projected, Compound, Bound.
  ScopedPj deriv_from_proj(proj_crs_add_horizontal_derived_conversion(
      ctx.get(), "DerivProj", manual_proj.get(), affine_2d.get(),
      manual_cart2.get()));
  ASSERT_THAT(deriv_from_proj.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_DERIVED_PROJECTED_CRS,
            proj_get_type(deriv_from_proj.get()));

  ScopedPj proj_plus_vert(proj_create_compound_crs(
      ctx.get(), "Proj+Vert", manual_proj.get(), vert_simple.get()));
  ASSERT_THAT(proj_plus_vert.get(), NotNull());
  ScopedPj deriv_from_comp(proj_crs_add_horizontal_derived_conversion(
      ctx.get(), "DerivComp", proj_plus_vert.get(), affine_2d.get(),
      manual_cart2.get()));
  ASSERT_THAT(deriv_from_comp.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_COMPOUND_CRS, proj_get_type(deriv_from_comp.get()));

  ScopedPj transf_proj_to_wgs(proj_create_transformation(
      ctx.get(), "ProjToWgs", "MYAUTH", "T1", manual_proj.get(),
      geog_from_ens.get(), nullptr, "Longitude rotation", "EPSG", "9601", 0,
      nullptr, 1.5));
  ASSERT_THAT(transf_proj_to_wgs.get(), NotNull());
  ScopedPj bound_proj(proj_crs_create_bound_crs(ctx.get(), manual_proj.get(),
                                                geog_from_ens.get(),
                                                transf_proj_to_wgs.get()));
  ASSERT_THAT(bound_proj.get(), NotNull());
  ScopedPj deriv_from_bound(proj_crs_add_horizontal_derived_conversion(
      ctx.get(), "DerivBound", bound_proj.get(), affine_2d.get(),
      manual_cart2.get()));
  ASSERT_THAT(deriv_from_bound.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_BOUND_CRS, proj_get_type(deriv_from_bound.get()));

  EXPECT_THAT(proj_crs_add_horizontal_derived_conversion(
                  ctx.get(), "NotProj", geog_from_datum.get(), affine_2d.get(),
                  manual_cart2.get()),
              IsNull());
  EXPECT_THAT(proj_crs_add_horizontal_derived_conversion(
                  ctx.get(), "NotCRS", manual_cart2.get(), affine_2d.get(),
                  manual_cart2.get()),
              IsNull());
  EXPECT_THAT(proj_crs_add_horizontal_derived_conversion(
                  ctx.get(), "NotConv", manual_proj.get(), manual_cart2.get(),
                  manual_cart2.get()),
              IsNull());
  EXPECT_THAT(proj_crs_add_horizontal_derived_conversion(
                  ctx.get(), "NotCart", manual_proj.get(), affine_2d.get(),
                  ellps2d_ll.get()),
              IsNull());
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(
        proj_crs_add_horizontal_derived_conversion(
            ctx.get(), "Null", nullptr, affine_2d.get(), manual_cart2.get()),
        IsNull());
  }

  // proj_create_conversion covering all 5 PJ_UNIT_TYPE values and
  // get_unit_category.
  const PJ_PARAM_DESCRIPTION params[7] = {
      {"ang_param", "MYAUTH", "P1", 10.0, "degree", kDegToRad, PJ_UT_ANGULAR},
      {"lin_param", nullptr, nullptr, 500.0, "metre", 1.0, PJ_UT_LINEAR},
      {"lin_per_t", nullptr, nullptr, 1.5, "metre per year", 1.0, PJ_UT_LINEAR},
      {"scale_p", nullptr, nullptr, 1.0, nullptr, 1.0, PJ_UT_SCALE},
      {"scale_t", nullptr, nullptr, 2.0, "ppb per year", 1e-9, PJ_UT_SCALE},
      {"time_p", nullptr, nullptr, 2020.0, "year", 31557600.0, PJ_UT_TIME},
      {"param_t", nullptr, nullptr, 100.0, "hPa per day", 1.0,
       PJ_UT_PARAMETRIC},
  };
  ScopedPj manual_conv(proj_create_conversion(ctx.get(), nullptr, nullptr,
                                              nullptr, nullptr, nullptr,
                                              nullptr, 7, params));
  ASSERT_THAT(manual_conv.get(), NotNull());
  EXPECT_EQ(7,
            proj_coordoperation_get_param_count(ctx.get(), manual_conv.get()));
  const char* u_cat = nullptr;
  EXPECT_EQ(1, proj_coordoperation_get_param(ctx.get(), manual_conv.get(), 2,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, &u_cat));
  EXPECT_THAT(u_cat, StrEq("linear_per_time"));
  EXPECT_EQ(1, proj_coordoperation_get_param(ctx.get(), manual_conv.get(), 4,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, &u_cat));
  EXPECT_THAT(u_cat, StrEq("scale_per_time"));
  EXPECT_EQ(1, proj_coordoperation_get_param(ctx.get(), manual_conv.get(), 5,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, &u_cat));
  EXPECT_THAT(u_cat, StrEq("time"));
  EXPECT_EQ(1, proj_coordoperation_get_param(ctx.get(), manual_conv.get(), 6,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, &u_cat));
  EXPECT_THAT(u_cat, StrEq("parametric_per_time"));

  // proj_create_transformation with interpolation CRS + errors.
  ScopedPj geog2d(proj_create_from_database(ctx.get(), "EPSG", "4326",
                                            PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj geog3d(proj_create_from_database(ctx.get(), "EPSG", "4979",
                                            PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj vert_crs(proj_create_from_database(ctx.get(), "EPSG", "5714",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(geog2d.get(), NotNull());
  ASSERT_THAT(geog3d.get(), NotNull());
  ASSERT_THAT(vert_crs.get(), NotNull());

  ScopedPj transf_with_interp(proj_create_transformation(
      ctx.get(), "TransfInterp", "MYAUTH", "T2", geog_from_datum.get(),
      geog_from_ens.get(), geog3d.get(), "NTv2", "EPSG", "9615", 1, params,
      -1.0));
  ASSERT_THAT(transf_with_interp.get(), NotNull());
  EXPECT_DOUBLE_EQ(-1.0, proj_coordoperation_get_accuracy(
                             ctx.get(), transf_with_interp.get()));
  EXPECT_DOUBLE_EQ(1.5, proj_coordoperation_get_accuracy(
                            ctx.get(), transf_proj_to_wgs.get()));
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(
        proj_create_transformation(ctx.get(), "Null", nullptr, nullptr, nullptr,
                                   geog_from_ens.get(), nullptr, "M", nullptr,
                                   nullptr, 0, nullptr, 1.0),
        IsNull());
    EXPECT_THAT(proj_create_transformation(ctx.get(), "BadSrc", nullptr,
                                           nullptr, manual_cart2.get(),
                                           geog_from_ens.get(), nullptr, "M",
                                           nullptr, nullptr, 0, nullptr, 1.0),
                IsNull());
    EXPECT_THAT(proj_create_transformation(ctx.get(), "BadTgt", nullptr,
                                           nullptr, geog_from_datum.get(),
                                           manual_cart2.get(), nullptr, "M",
                                           nullptr, nullptr, 0, nullptr, 1.0),
                IsNull());
    EXPECT_THAT(proj_create_transformation(
                    ctx.get(), "BadInterp", nullptr, nullptr,
                    geog_from_datum.get(), geog_from_ens.get(),
                    manual_cart2.get(), "M", nullptr, nullptr, 0, nullptr, 1.0),
                IsNull());
  }

  // proj_convert_conversion_to_other_method.
  ScopedPj merc_a_raw(proj_create_conversion_mercator_variant_a(
      ctx.get(), 0.0, 10.0, 0.99, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  ASSERT_THAT(merc_a_raw.get(), NotNull());
  ScopedPj proj_merc_a(proj_create_projected_crs(
      ctx.get(), "MercA", geog2d.get(), merc_a_raw.get(), manual_cart2.get()));
  ASSERT_THAT(proj_merc_a.get(), NotNull());
  ScopedPj merc_a(proj_crs_get_coordoperation(ctx.get(), proj_merc_a.get()));
  ASSERT_THAT(merc_a.get(), NotNull());
  ScopedPj merc_b_by_code(proj_convert_conversion_to_other_method(
      ctx.get(), merc_a.get(), EPSG_CODE_METHOD_MERCATOR_VARIANT_B, nullptr));
  ASSERT_THAT(merc_b_by_code.get(), NotNull());
  ScopedPj proj_merc_b(
      proj_create_projected_crs(ctx.get(), "MercB", geog2d.get(),
                                merc_b_by_code.get(), manual_cart2.get()));
  ASSERT_THAT(proj_merc_b.get(), NotNull());
  ScopedPj merc_b_attached(
      proj_crs_get_coordoperation(ctx.get(), proj_merc_b.get()));
  ASSERT_THAT(merc_b_attached.get(), NotNull());
  ScopedPj merc_a_by_name(proj_convert_conversion_to_other_method(
      ctx.get(), merc_b_attached.get(), 0,
      EPSG_NAME_METHOD_MERCATOR_VARIANT_A));
  ASSERT_THAT(merc_a_by_name.get(), NotNull());
  ScopedPj merc_b_by_name(proj_convert_conversion_to_other_method(
      ctx.get(), merc_a.get(), 0, EPSG_NAME_METHOD_MERCATOR_VARIANT_B));
  ASSERT_THAT(merc_b_by_name.get(), NotNull());

  ScopedPj lcc_1sp_raw(proj_create_conversion_lambert_conic_conformal_1sp(
      ctx.get(), 45.0, 2.0, 0.999, 500000.0, 0.0, "degree", kDegToRad, "metre",
      1.0));
  ASSERT_THAT(lcc_1sp_raw.get(), NotNull());
  ScopedPj proj_lcc_1sp(
      proj_create_projected_crs(ctx.get(), "Lcc1sp", geog2d.get(),
                                lcc_1sp_raw.get(), manual_cart2.get()));
  ASSERT_THAT(proj_lcc_1sp.get(), NotNull());
  ScopedPj lcc_1sp(proj_crs_get_coordoperation(ctx.get(), proj_lcc_1sp.get()));
  ASSERT_THAT(lcc_1sp.get(), NotNull());
  ScopedPj lcc_2sp_by_name(proj_convert_conversion_to_other_method(
      ctx.get(), lcc_1sp.get(), 0,
      EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_2SP));
  ASSERT_THAT(lcc_2sp_by_name.get(), NotNull());
  ScopedPj proj_lcc_2sp(
      proj_create_projected_crs(ctx.get(), "Lcc2sp", geog2d.get(),
                                lcc_2sp_by_name.get(), manual_cart2.get()));
  ASSERT_THAT(proj_lcc_2sp.get(), NotNull());
  ScopedPj lcc_2sp_attached(
      proj_crs_get_coordoperation(ctx.get(), proj_lcc_2sp.get()));
  ASSERT_THAT(lcc_2sp_attached.get(), NotNull());
  ScopedPj lcc_1sp_by_name(proj_convert_conversion_to_other_method(
      ctx.get(), lcc_2sp_attached.get(), 0,
      EPSG_NAME_METHOD_LAMBERT_CONIC_CONFORMAL_1SP));
  ASSERT_THAT(lcc_1sp_by_name.get(), NotNull());

  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_convert_conversion_to_other_method(ctx.get(), nullptr,
                                                        9805, nullptr),
                IsNull());
    EXPECT_THAT(proj_convert_conversion_to_other_method(ctx.get(), geog2d.get(),
                                                        9805, nullptr),
                IsNull());
  }
  EXPECT_THAT(proj_convert_conversion_to_other_method(ctx.get(), merc_a.get(),
                                                      0, nullptr),
              IsNull());
  EXPECT_THAT(proj_convert_conversion_to_other_method(ctx.get(), merc_a.get(),
                                                      999999, nullptr),
              IsNull());

  // Bound CRS creation variants & error cases.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_crs_create_bound_crs(ctx.get(), nullptr, geog2d.get(),
                                          transf_proj_to_wgs.get()),
                IsNull());
    EXPECT_THAT(
        proj_crs_create_bound_crs(ctx.get(), manual_cart2.get(), geog2d.get(),
                                  transf_proj_to_wgs.get()),
        IsNull());
    EXPECT_THAT(
        proj_crs_create_bound_crs(ctx.get(), manual_proj.get(),
                                  manual_cart2.get(), transf_proj_to_wgs.get()),
        IsNull());
    EXPECT_THAT(proj_crs_create_bound_crs(ctx.get(), manual_proj.get(),
                                          geog2d.get(), manual_cart2.get()),
                IsNull());
    EXPECT_THAT(proj_crs_create_bound_crs_to_WGS84(ctx.get(), nullptr, nullptr),
                IsNull());
    EXPECT_THAT(proj_crs_create_bound_crs_to_WGS84(ctx.get(),
                                                   manual_cart2.get(), nullptr),
                IsNull());
  }

  ScopedPj dhdn_crs(proj_create_from_database(ctx.get(), "EPSG", "4314",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(dhdn_crs.get(), NotNull());
  const char* const bound_opts_always[] = {"ALLOW_INTERMEDIATE_CRS=ALWAYS",
                                           nullptr};
  const char* const bound_opts_if_no_direct[] = {
      "ALLOW_INTERMEDIATE_CRS=IF_NO_DIRECT_TRANSFORMATION", nullptr};
  const char* const bound_opts_bad[] = {"BAD_BOUND_OPT=1", nullptr};
  ScopedPj dhdn_bound_always(proj_crs_create_bound_crs_to_WGS84(
      ctx.get(), dhdn_crs.get(), bound_opts_always));
  ScopedPj dhdn_bound_indirect(proj_crs_create_bound_crs_to_WGS84(
      ctx.get(), dhdn_crs.get(), bound_opts_if_no_direct));
  EXPECT_THAT(dhdn_bound_always.get(), NotNull());
  EXPECT_THAT(dhdn_bound_indirect.get(), NotNull());
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_crs_create_bound_crs_to_WGS84(ctx.get(), dhdn_crs.get(),
                                                   bound_opts_bad),
                IsNull());
  }

  ScopedPj bound_vert(proj_crs_create_bound_vertical_crs(
      ctx.get(), vert_crs.get(), geog3d.get(), "egm96_15.gtx"));
  ASSERT_THAT(bound_vert.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_BOUND_CRS, proj_get_type(bound_vert.get()));
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_crs_create_bound_vertical_crs(ctx.get(), nullptr,
                                                   geog3d.get(), "g.gtx"),
                IsNull());
    EXPECT_THAT(proj_crs_create_bound_vertical_crs(ctx.get(), geog2d.get(),
                                                   geog3d.get(), "g.gtx"),
                IsNull());
    EXPECT_THAT(proj_crs_create_bound_vertical_crs(ctx.get(), vert_crs.get(),
                                                   manual_cart2.get(), "g.gtx"),
                IsNull());
  }
}

TEST(Iso19111CApiTest, CreateAll71ConversionHelpers) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  auto check_conv = [](PJ* raw) {
    ScopedPj conv(raw);
    ASSERT_THAT(conv.get(), NotNull());
    EXPECT_EQ(PJ_TYPE_CONVERSION, proj_get_type(conv.get()));
  };

  check_conv(proj_create_conversion_utm(ctx.get(), 31, 1));
  check_conv(proj_create_conversion_utm(ctx.get(), 15, 0));
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_create_conversion_utm(ctx.get(), 0, 1), IsNull());
    EXPECT_THAT(proj_create_conversion_utm(ctx.get(), 61, 1), IsNull());
  }

  check_conv(proj_create_conversion_transverse_mercator(
      ctx.get(), 0.0, 3.0, 0.9996, 500000.0, 0.0, "degree", kDegToRad, "metre",
      1.0));
  check_conv(proj_create_conversion_gauss_schreiber_transverse_mercator(
      ctx.get(), 0.0, 3.0, 0.9996, 500000.0, 0.0, nullptr, 0.0, nullptr, 0.0));
  check_conv(proj_create_conversion_transverse_mercator_south_oriented(
      ctx.get(), 0.0, 29.0, 1.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_two_point_equidistant(
      ctx.get(), 10.0, 20.0, 30.0, 40.0, 0.0, 0.0, "degree", kDegToRad, "metre",
      1.0));
  check_conv(proj_create_conversion_tunisia_mapping_grid(
      ctx.get(), 40.0, 11.0, 500000.0, 300000.0, "grad", kGradToRad, "metre",
      1.0));
  check_conv(proj_create_conversion_tunisia_mining_grid(
      ctx.get(), 40.0, 11.0, 500000.0, 300000.0, "grad", kGradToRad, "metre",
      1.0));
  check_conv(proj_create_conversion_albers_equal_area(
      ctx.get(), 23.0, -96.0, 29.5, 45.5, 0.0, 0.0, "degree", kDegToRad,
      "metre", 1.0));
  check_conv(proj_create_conversion_lambert_conic_conformal_1sp(
      ctx.get(), 45.0, 3.0, 0.9999, 500000.0, 500000.0, "degree", kDegToRad,
      "metre", 1.0));
  check_conv(proj_create_conversion_lambert_conic_conformal_1sp_variant_b(
      ctx.get(), 45.0, 0.9999, 45.0, 3.0, 500000.0, 500000.0, "degree",
      kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_lambert_conic_conformal_2sp(
      ctx.get(), 45.0, 3.0, 33.0, 45.0, 500000.0, 0.0, "degree", kDegToRad,
      "foot", kFootToMetre));
  check_conv(proj_create_conversion_lambert_conic_conformal_2sp_michigan(
      ctx.get(), 43.3166, -84.3666, 44.1833, 45.7, 609601.2192, 0.0, 1.0000382,
      "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_lambert_conic_conformal_2sp_belgium(
      ctx.get(), 90.0, 4.3674867, 49.8333339, 51.1666672, 150000.0, 5400088.4,
      "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_azimuthal_equidistant(
      ctx.get(), 13.47, 144.75, 50000.0, 50000.0, "degree", kDegToRad, "metre",
      1.0));
  check_conv(proj_create_conversion_guam_projection(ctx.get(), 13.47, 144.75,
                                                    50000.0, 50000.0, "degree",
                                                    kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_bonne(ctx.get(), 45.0, 3.0, 0.0, 0.0,
                                          "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_lambert_cylindrical_equal_area_spherical(
      ctx.get(), 30.0, 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_lambert_cylindrical_equal_area(
      ctx.get(), 30.0, 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_cassini_soldner(
      ctx.get(), 10.44, -61.33, 430000.0, 325000.0, "degree", kDegToRad,
      "metre", 1.0));
  check_conv(proj_create_conversion_equidistant_conic(
      ctx.get(), 40.0, -96.0, 20.0, 60.0, 0.0, 0.0, "degree", kDegToRad,
      "metre", 1.0));
  check_conv(proj_create_conversion_eckert_i(ctx.get(), 0.0, 0.0, 0.0, "degree",
                                             kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_eckert_ii(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_eckert_iii(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_eckert_iv(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_eckert_v(ctx.get(), 0.0, 0.0, 0.0, "degree",
                                             kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_eckert_vi(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_equidistant_cylindrical(
      ctx.get(), 0.0, 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_equidistant_cylindrical_spherical(
      ctx.get(), 0.0, 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_gall(ctx.get(), 0.0, 0.0, 0.0, "degree",
                                         kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_goode_homolosine(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_interrupted_goode_homolosine(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_geostationary_satellite_sweep_x(
      ctx.get(), 0.0, 35785831.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_geostationary_satellite_sweep_y(
      ctx.get(), -75.0, 35785831.0, 0.0, 0.0, "degree", kDegToRad, "metre",
      1.0));
  check_conv(proj_create_conversion_gnomonic(
      ctx.get(), 45.0, 3.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_hotine_oblique_mercator_variant_a(
      ctx.get(), 45.0, 3.0, 30.0, 30.0, 0.9996, 0.0, 0.0, "degree", kDegToRad,
      "metre", 1.0));
  check_conv(proj_create_conversion_hotine_oblique_mercator_variant_b(
      ctx.get(), 45.0, 3.0, 30.0, 30.0, 0.9996, 500000.0, 0.0, "degree",
      kDegToRad, "metre", 1.0));
  check_conv(
      proj_create_conversion_hotine_oblique_mercator_two_point_natural_origin(
          ctx.get(), 45.0, 40.0, 0.0, 50.0, 10.0, 0.9996, 0.0, 0.0, "degree",
          kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_laborde_oblique_mercator(
      ctx.get(), -18.9, 46.4372, 18.9, 0.9995, 400000.0, 800000.0, "degree",
      kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_international_map_world_polyconic(
      ctx.get(), 3.0, 32.0, 36.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_krovak_north_oriented(
      ctx.get(), 49.5, 24.833333, 30.288139, 78.5, 0.9999, 0.0, 0.0, "degree",
      kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_krovak(ctx.get(), 49.5, 24.833333,
                                           30.288139, 78.5, 0.9999, 0.0, 0.0,
                                           "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_lambert_azimuthal_equal_area(
      ctx.get(), 52.0, 10.0, 4321000.0, 3210000.0, "degree", kDegToRad, "metre",
      1.0));
  check_conv(proj_create_conversion_miller_cylindrical(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_mercator_variant_a(
      ctx.get(), 0.0, 0.0, 1.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_mercator_variant_b(
      ctx.get(), 30.0, 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_popular_visualisation_pseudo_mercator(
      ctx.get(), 0.0, 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_mollweide(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_new_zealand_mapping_grid(
      ctx.get(), -41.0, 173.0, 2510000.0, 6023150.0, "degree", kDegToRad,
      "metre", 1.0));
  check_conv(proj_create_conversion_oblique_stereographic(
      ctx.get(), 52.15616, 5.38764, 0.9999079, 155000.0, 463000.0, "degree",
      kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_orthographic(
      ctx.get(), 45.0, 3.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_local_orthographic(
      ctx.get(), 45.0, 3.0, 15.0, 1.0, 0.0, 0.0, "degree", kDegToRad, "metre",
      1.0));
  check_conv(proj_create_conversion_american_polyconic(
      ctx.get(), 0.0, -54.0, 5000000.0, 10000000.0, "degree", kDegToRad,
      "metre", 1.0));
  check_conv(proj_create_conversion_polar_stereographic_variant_a(
      ctx.get(), 90.0, 0.0, 0.994, 2000000.0, 2000000.0, "degree", kDegToRad,
      "metre", 1.0));
  check_conv(proj_create_conversion_polar_stereographic_variant_b(
      ctx.get(), 71.0, 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_robinson(ctx.get(), 0.0, 0.0, 0.0, "degree",
                                             kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_sinusoidal(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_stereographic(
      ctx.get(), 0.0, 0.0, 1.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_van_der_grinten(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_wagner_i(ctx.get(), 0.0, 0.0, 0.0, "degree",
                                             kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_wagner_ii(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_wagner_iii(
      ctx.get(), 60.0, 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_wagner_iv(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_wagner_v(ctx.get(), 0.0, 0.0, 0.0, "degree",
                                             kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_wagner_vi(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_wagner_vii(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_quadrilateralized_spherical_cube(
      ctx.get(), 0.0, 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_spherical_cross_track_height(
      ctx.get(), 45.0, -75.0, 30.0, 1000.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_equal_earth(
      ctx.get(), 0.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  check_conv(proj_create_conversion_vertical_perspective(
      ctx.get(), 45.0, 3.0, 100.0, 500000.0, 0.0, 0.0, "degree", kDegToRad,
      "metre", 1.0));
  check_conv(proj_create_conversion_pole_rotation_grib_convention(
      ctx.get(), -30.0, 10.0, 5.0, nullptr, 0.0));
  check_conv(proj_create_conversion_pole_rotation_netcdf_cf_convention(
      ctx.get(), 39.25, -162.0, 18.0, "degree", kDegToRad));
}

TEST(Iso19111CApiTest, CoordinateOperationsAndFactoryContext) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // Null destroy calls for all list / handle cleanup wrappers.
  proj_int_list_destroy(nullptr);
  proj_celestial_body_list_destroy(nullptr);
  proj_string_list_destroy(nullptr);
  proj_get_crs_list_parameters_destroy(nullptr);
  proj_crs_info_list_destroy(nullptr);
  proj_unit_list_destroy(nullptr);
  proj_string_destroy(nullptr);
  proj_list_destroy(nullptr);
  proj_operation_factory_context_destroy(nullptr);

  ScopedPj utm31(proj_create_from_database(ctx.get(), "EPSG", "32631",
                                           PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(utm31.get(), NotNull());
  ScopedPj geog_wgs84(proj_create_from_database(ctx.get(), "EPSG", "4326",
                                                PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(geog_wgs84.get(), NotNull());
  ScopedPj nad27(proj_create_from_database(ctx.get(), "EPSG", "4267",
                                           PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(nad27.get(), NotNull());
  ScopedPj ellps(proj_create_from_database(ctx.get(), "EPSG", "7030",
                                           PJ_CATEGORY_ELLIPSOID, 0, nullptr));
  ASSERT_THAT(ellps.get(), NotNull());

  // createPropertyMapName "(deprecated)" suffix branch.
  ScopedPj dep_crs(
      proj_alter_name(ctx.get(), geog_wgs84.get(), "Test CRS (deprecated)"));
  ASSERT_THAT(dep_crs.get(), NotNull());
  EXPECT_EQ(1, proj_is_deprecated(dep_crs.get()));

  // proj_crs_get_coordoperation & proj_coordoperation_get_method_info.
  ScopedPj utm_conv(proj_crs_get_coordoperation(ctx.get(), utm31.get()));
  ASSERT_THAT(utm_conv.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_CONVERSION, proj_get_type(utm_conv.get()));

  const char* method_name = nullptr;
  const char* method_auth = nullptr;
  const char* method_code = nullptr;
  EXPECT_EQ(1, proj_coordoperation_get_method_info(ctx.get(), utm_conv.get(),
                                                   &method_name, &method_auth,
                                                   &method_code));
  EXPECT_THAT(method_name, StrEq("Transverse Mercator"));
  EXPECT_THAT(method_auth, StrEq("EPSG"));
  EXPECT_THAT(method_code, StrEq("9807"));
  EXPECT_EQ(1, proj_coordoperation_get_method_info(ctx.get(), utm_conv.get(),
                                                   nullptr, nullptr, nullptr));

  // Manual conversion + bound CRS branches.
  ScopedPj manual_conv(proj_create_conversion_mercator_variant_a(
      ctx.get(), 0.0, 0.0, 1.0, 0.0, 0.0, "degree", kDegToRad, "metre", 1.0));
  ASSERT_THAT(manual_conv.get(), NotNull());
  ScopedPj helmert_1671(proj_create_from_database(
      ctx.get(), "EPSG", "1671", PJ_CATEGORY_COORDINATE_OPERATION, 0, nullptr));
  ASSERT_THAT(helmert_1671.get(), NotNull());
  ScopedPj bound_nad27(proj_crs_create_bound_crs(
      ctx.get(), nad27.get(), geog_wgs84.get(), helmert_1671.get()));
  ASSERT_THAT(bound_nad27.get(), NotNull());
  ScopedPj bound_op(proj_crs_get_coordoperation(ctx.get(), bound_nad27.get()));
  ASSERT_THAT(bound_op.get(), NotNull());

  // Conversion without method IDs hits the empty method_ids branches.
  const char* custom_param_unit_name = "pascal";
  PJ_PARAM_DESCRIPTION param_descs[] = {
      {.name = "ParametricScale",
       .auth_name = nullptr,
       .code = nullptr,
       .value = 100.0,
       .unit_name = custom_param_unit_name,
       .unit_conv_factor = 1.0,
       .unit_type = PJ_UT_PARAMETRIC},
      {.name = "ParametricRate",
       .auth_name = nullptr,
       .code = nullptr,
       .value = 2.0,
       .unit_name = "pascal per year",
       .unit_conv_factor = 1.0,
       .unit_type = PJ_UT_PARAMETRIC},
  };
  ScopedPj param_conv(proj_create_conversion(
      ctx.get(), "ParametricConv", nullptr, nullptr, "CustomParamMethod",
      nullptr, nullptr, 2, param_descs));
  ASSERT_THAT(param_conv.get(), NotNull());
  EXPECT_EQ(1, proj_coordoperation_get_method_info(ctx.get(), param_conv.get(),
                                                   &method_name, &method_auth,
                                                   &method_code));
  EXPECT_THAT(method_name, StrEq("CustomParamMethod"));
  EXPECT_THAT(method_auth, IsNull());
  EXPECT_THAT(method_code, IsNull());

  // Time-dependent Helmert transformation EPSG:6276.
  ScopedPj transf_6276(proj_create_from_database(
      ctx.get(), "EPSG", "6276", PJ_CATEGORY_COORDINATE_OPERATION, 0, nullptr));
  ASSERT_THAT(transf_6276.get(), NotNull());
  EXPECT_EQ(1,
            proj_coordoperation_is_instantiable(ctx.get(), transf_6276.get()));
  EXPECT_EQ(0, proj_coordoperation_has_ballpark_transformation(
                   ctx.get(), transf_6276.get()));
  EXPECT_EQ(1, proj_coordoperation_requires_per_coordinate_input_time(
                   ctx.get(), transf_6276.get()));
  EXPECT_EQ(0, proj_coordoperation_requires_per_coordinate_input_time(
                   ctx.get(), helmert_1671.get()));

  // Ballpark transformation branch (name contains "ballpark").
  PJ_PARAM_DESCRIPTION ballpark_params[] = {{.name = "NullParam",
                                             .auth_name = nullptr,
                                             .code = nullptr,
                                             .value = 0.0,
                                             .unit_name = "degree",
                                             .unit_conv_factor = kDegToRad,
                                             .unit_type = PJ_UT_ANGULAR}};
  ScopedPj ballpark_transf(proj_create_transformation(
      ctx.get(), "Ballpark synthetic offset", nullptr, nullptr, nad27.get(),
      geog_wgs84.get(), nullptr, "CustomBallparkMethod", nullptr, nullptr, 1,
      ballpark_params, 100.0));
  ASSERT_THAT(ballpark_transf.get(), NotNull());
  EXPECT_EQ(1, proj_coordoperation_has_ballpark_transformation(
                   ctx.get(), ballpark_transf.get()));

  // Parameter introspection across measure, per-time, none, unknown, file.
  EXPECT_EQ(5, proj_coordoperation_get_param_count(ctx.get(), utm_conv.get()));
  EXPECT_EQ(
      2, proj_coordoperation_get_param_index(ctx.get(), utm_conv.get(),
                                             "scale factor at natural origin"));
  EXPECT_EQ(-1, proj_coordoperation_get_param_index(ctx.get(), utm_conv.get(),
                                                    "nonexistent_parameter"));

  const char* p_name = nullptr;
  const char* p_auth = nullptr;
  const char* p_code = nullptr;
  double p_val = 0.0;
  const char* p_str = nullptr;
  double u_factor = 0.0;
  const char* u_name = nullptr;
  const char* u_auth = nullptr;
  const char* u_code = nullptr;
  const char* u_cat = nullptr;
  EXPECT_EQ(1, proj_coordoperation_get_param(ctx.get(), utm_conv.get(), 2,
                                             &p_name, &p_auth, &p_code, &p_val,
                                             &p_str, &u_factor, &u_name,
                                             &u_auth, &u_code, &u_cat));
  EXPECT_THAT(p_auth, StrEq("EPSG"));
  EXPECT_THAT(p_code, StrEq("8805"));
  EXPECT_DOUBLE_EQ(0.9996, p_val);
  EXPECT_THAT(p_str, IsNull());
  EXPECT_DOUBLE_EQ(1.0, u_factor);
  EXPECT_THAT(u_cat, StrEq("scale"));
  EXPECT_EQ(1, proj_coordoperation_get_param(ctx.get(), utm_conv.get(), 0,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr));

  // Verify parametric and parametric_per_time categories.
  EXPECT_EQ(1, proj_coordoperation_get_param(ctx.get(), param_conv.get(), 0,
                                             nullptr, &p_auth, &p_code, &p_val,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, &u_cat));
  EXPECT_THAT(p_auth, IsNull());
  EXPECT_THAT(p_code, IsNull());
  EXPECT_THAT(u_cat, StrEq("parametric"));
  EXPECT_EQ(1, proj_coordoperation_get_param(ctx.get(), param_conv.get(), 1,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, &u_cat));
  EXPECT_THAT(u_cat, StrEq("parametric_per_time"));

  // Verify linear_per_time, angular_per_time, scale_per_time, time on
  // EPSG:6276.
  const int p_count_6276 =
      proj_coordoperation_get_param_count(ctx.get(), transf_6276.get());
  ASSERT_GT(p_count_6276, 5);
  bool saw_lin_t = false;
  bool saw_ang_t = false;
  bool saw_scale_t = false;
  bool saw_time = false;
  for (int i = 0; i < p_count_6276; ++i) {
    if (proj_coordoperation_get_param(
            ctx.get(), transf_6276.get(), i, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, &u_cat) == 1 &&
        u_cat != nullptr) {
      const std::string_view cat(u_cat);
      if (cat == "linear_per_time") saw_lin_t = true;
      if (cat == "angular_per_time") saw_ang_t = true;
      if (cat == "scale_per_time") saw_scale_t = true;
      if (cat == "time") saw_time = true;
    }
  }
  EXPECT_TRUE(saw_lin_t);
  EXPECT_TRUE(saw_ang_t);
  EXPECT_TRUE(saw_scale_t);
  EXPECT_TRUE(saw_time);

  // Verify "none" (unitless WKT2 PARAMETER) and "unknown" (PROJJSON Unit).
  ScopedPj none_unit_conv(
      proj_create(ctx.get(),
                  "CONVERSION[\"NoneUnitConv\",METHOD[\"CustomMethod\"],"
                  "PARAMETER[\"NoUnitParam\",42]]"));
  ASSERT_THAT(none_unit_conv.get(), NotNull());
  EXPECT_EQ(1, proj_coordoperation_get_param(ctx.get(), none_unit_conv.get(), 0,
                                             nullptr, nullptr, nullptr, &p_val,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, &u_cat));
  EXPECT_DOUBLE_EQ(42.0, p_val);
  EXPECT_THAT(u_cat, StrEq("unknown"));

  ScopedPj unknown_unit_conv(proj_create(
      ctx.get(),
      R"({"type":"Conversion","name":"UnkConv","method":{"name":"UnkM"},)"
      R"("parameters":[{"name":"UnkP","value":7,"unit":{"type":"Unit",)"
      R"("name":"arb","conversion_factor":1}}]})"));
  ASSERT_THAT(unknown_unit_conv.get(), NotNull());
  EXPECT_EQ(1, proj_coordoperation_get_param(ctx.get(), unknown_unit_conv.get(),
                                             0, nullptr, nullptr, nullptr,
                                             &p_val, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, &u_cat));
  EXPECT_THAT(u_cat, StrEq("unknown"));

  // FILENAME parameter & grid usage on EPSG:5661.
  ScopedPj ntv2_5661(proj_create_from_database(
      ctx.get(), "EPSG", "5661", PJ_CATEGORY_COORDINATE_OPERATION, 0, nullptr));
  ASSERT_THAT(ntv2_5661.get(), NotNull());
  EXPECT_EQ(1, proj_coordoperation_get_param(
                   ctx.get(), ntv2_5661.get(), 0, &p_name, nullptr, nullptr,
                   &p_val, &p_str, nullptr, &u_name, nullptr, nullptr, &u_cat));
  EXPECT_THAT(p_str, NotNull());
  EXPECT_THAT(u_name, IsNull());
  EXPECT_THAT(u_cat, IsNull());

  const int grid_count_5661 =
      proj_coordoperation_get_grid_used_count(ctx.get(), ntv2_5661.get());
  ASSERT_EQ(1, grid_count_5661);
  // Second call hits the cached gridsNeededAsked branch.
  EXPECT_EQ(grid_count_5661, proj_coordoperation_get_grid_used_count(
                                 ctx.get(), ntv2_5661.get()));
  const char* g_short = nullptr;
  const char* g_full = nullptr;
  const char* g_pkg = nullptr;
  const char* g_url = nullptr;
  int g_direct = 0;
  int g_open = 0;
  int g_avail = -1;
  EXPECT_EQ(1, proj_coordoperation_get_grid_used(
                   ctx.get(), ntv2_5661.get(), 0, &g_short, &g_full, &g_pkg,
                   &g_url, &g_direct, &g_open, &g_avail));
  EXPECT_THAT(g_short, StrEq("100800401.gsb"));
  EXPECT_THAT(g_full, NotNull());
  EXPECT_THAT(g_pkg, NotNull());
  EXPECT_THAT(g_url, NotNull());
  EXPECT_EQ(1, g_direct);
  EXPECT_EQ(1, g_open);
  EXPECT_EQ(1, proj_coordoperation_get_grid_used(
                   ctx.get(), ntv2_5661.get(), 0, nullptr, nullptr, nullptr,
                   nullptr, nullptr, nullptr, nullptr));

  // Grid check against an unknown filename without alternatives.
  ScopedPj missing_grid_pm(proj_create(ctx.get(), kPointMotionOpWkt));
  ASSERT_THAT(missing_grid_pm.get(), NotNull());
  EXPECT_EQ(1, proj_coordoperation_get_grid_used_count(ctx.get(),
                                                       missing_grid_pm.get()));
  EXPECT_EQ(
      0, proj_coordoperation_is_instantiable(ctx.get(), missing_grid_pm.get()));

  // Accuracy checks (valid, none, unknown string catch).
  EXPECT_DOUBLE_EQ(
      1.0, proj_coordoperation_get_accuracy(ctx.get(), helmert_1671.get()));
  EXPECT_LT(proj_coordoperation_get_accuracy(ctx.get(), utm_conv.get()), 0.0);
  ScopedPj unknown_acc_transf(proj_create(
      ctx.get(), R"({"type":"Transformation","name":"UnkAcc",)"
                 R"("source_crs":{"type":"GeographicCRS","name":"A",)"
                 R"("datum":{"type":"GeodeticReferenceFrame","name":"DA",)"
                 R"("ellipsoid":{"name":"WGS 84","semi_major_axis":6378137,)"
                 R"("inverse_flattening":298.257223563}},)"
                 R"("coordinate_system":{"subtype":"ellipsoidal","axis":[)"
                 R"({"name":"Lat","abbreviation":"lat","direction":"north",)"
                 R"("unit":"degree"},)"
                 R"({"name":"Lon","abbreviation":"lon","direction":"east",)"
                 R"("unit":"degree"}]}},)"
                 R"("target_crs":{"type":"GeographicCRS","name":"B",)"
                 R"("datum":{"type":"GeodeticReferenceFrame","name":"DB",)"
                 R"("ellipsoid":{"name":"WGS 84","semi_major_axis":6378137,)"
                 R"("inverse_flattening":298.257223563}},)"
                 R"("coordinate_system":{"subtype":"ellipsoidal","axis":[)"
                 R"({"name":"Lat","abbreviation":"lat","direction":"north",)"
                 R"("unit":"degree"},)"
                 R"({"name":"Lon","abbreviation":"lon","direction":"east",)"
                 R"("unit":"degree"}]}},)"
                 R"("method":{"name":"Null geographic offset"},)"
                 R"("parameters":[],)"
                 R"("accuracy":"unknown"})"));
  ASSERT_THAT(unknown_acc_transf.get(), NotNull());
  EXPECT_LT(
      proj_coordoperation_get_accuracy(ctx.get(), unknown_acc_transf.get()),
      0.0);

  // TOWGS84 extraction: 3-param, 7-param, incompatible Molodensky-Badekas.
  std::array<double, 7> towgs = {};
  EXPECT_EQ(1, proj_coordoperation_get_towgs84_values(
                   ctx.get(), helmert_1671.get(), towgs.data(), 3, 1));
  EXPECT_EQ(1, proj_coordoperation_get_towgs84_values(
                   ctx.get(), helmert_1671.get(), towgs.data(), 7, 1));
  ScopedPj helmert_1074(proj_create_from_database(
      ctx.get(), "EPSG", "1074", PJ_CATEGORY_COORDINATE_OPERATION, 0, nullptr));
  ASSERT_THAT(helmert_1074.get(), NotNull());
  EXPECT_EQ(1, proj_coordoperation_get_towgs84_values(
                   ctx.get(), helmert_1074.get(), towgs.data(), 7, 1));

  ScopedPj mb_1066(proj_create_from_database(
      ctx.get(), "EPSG", "1066", PJ_CATEGORY_COORDINATE_OPERATION, 0, nullptr));
  ASSERT_THAT(mb_1066.get(), NotNull());
  EXPECT_EQ(0, proj_coordoperation_get_towgs84_values(ctx.get(), mb_1066.get(),
                                                      towgs.data(), 7, 0));
  EXPECT_EQ(0, proj_coordoperation_get_towgs84_values(ctx.get(), utm_conv.get(),
                                                      towgs.data(), 7, 0));

  // Factory context creation & all 9 setters across every enum value.
  ScopedOpFactoryContext fctx_default(
      proj_create_operation_factory_context(ctx.get(), nullptr));
  ASSERT_THAT(fctx_default.get(), NotNull());
  ScopedOpFactoryContext fctx_any(
      proj_create_operation_factory_context(ctx.get(), "any"));
  ASSERT_THAT(fctx_any.get(), NotNull());
  ScopedOpFactoryContext fctx(
      proj_create_operation_factory_context(ctx.get(), "EPSG"));
  ASSERT_THAT(fctx.get(), NotNull());

  proj_operation_factory_context_set_desired_accuracy(ctx.get(), fctx.get(),
                                                      5.0);
  proj_operation_factory_context_set_desired_accuracy(ctx.get(), fctx.get(),
                                                      0.0);
  // Set by DB name when extent is nullptr, then rename existing extent.
  proj_operation_factory_context_set_area_of_interest_name(
      ctx.get(), fctx.get(), "France");
  proj_operation_factory_context_set_area_of_interest_name(
      ctx.get(), fctx.get(), "Renamed France Area");
  // Normal bbox + antimeridian crossing bbox.
  proj_operation_factory_context_set_area_of_interest(
      ctx.get(), fctx.get(), 170.0, -20.0, -170.0, 20.0);
  proj_operation_factory_context_set_area_of_interest(ctx.get(), fctx.get(),
                                                      -10.0, 40.0, 10.0, 52.0);

  for (PROJ_CRS_EXTENT_USE use :
       {PJ_CRS_EXTENT_NONE, PJ_CRS_EXTENT_BOTH, PJ_CRS_EXTENT_INTERSECTION,
        PJ_CRS_EXTENT_SMALLEST}) {
    proj_operation_factory_context_set_crs_extent_use(ctx.get(), fctx.get(),
                                                      use);
  }
  for (PROJ_SPATIAL_CRITERION crit :
       {PROJ_SPATIAL_CRITERION_STRICT_CONTAINMENT,
        PROJ_SPATIAL_CRITERION_PARTIAL_INTERSECTION}) {
    proj_operation_factory_context_set_spatial_criterion(ctx.get(), fctx.get(),
                                                         crit);
  }
  for (PROJ_GRID_AVAILABILITY_USE grid_use :
       {PROJ_GRID_AVAILABILITY_USED_FOR_SORTING,
        PROJ_GRID_AVAILABILITY_DISCARD_OPERATION_IF_MISSING_GRID,
        PROJ_GRID_AVAILABILITY_KNOWN_AVAILABLE,
        PROJ_GRID_AVAILABILITY_IGNORED}) {
    proj_operation_factory_context_set_grid_availability_use(
        ctx.get(), fctx.get(), grid_use);
  }
  proj_operation_factory_context_set_use_proj_alternative_grid_names(
      ctx.get(), fctx.get(), 0);
  proj_operation_factory_context_set_use_proj_alternative_grid_names(
      ctx.get(), fctx.get(), 1);
  for (PROJ_INTERMEDIATE_CRS_USE inter_use :
       {PROJ_INTERMEDIATE_CRS_USE_ALWAYS, PROJ_INTERMEDIATE_CRS_USE_NEVER,
        PROJ_INTERMEDIATE_CRS_USE_IF_NO_DIRECT_TRANSFORMATION}) {
    proj_operation_factory_context_set_allow_use_intermediate_crs(
        ctx.get(), fctx.get(), inter_use);
  }
  const char* const allowed_pivots[] = {"EPSG", "4258", "EPSG", "4326",
                                        nullptr};
  proj_operation_factory_context_set_allowed_intermediate_crs(
      ctx.get(), fctx.get(), allowed_pivots);
  proj_operation_factory_context_set_allowed_intermediate_crs(
      ctx.get(), fctx.get(), nullptr);
  proj_operation_factory_context_set_discard_superseded(ctx.get(), fctx.get(),
                                                        1);
  proj_operation_factory_context_set_allow_ballpark_transformations(
      ctx.get(), fctx.get(), 1);

  // proj_create_operations, proj_list_get*, proj_get_suggested_operation.
  ScopedOpFactoryContext fctx_na(
      proj_create_operation_factory_context(ctx.get(), nullptr));
  ASSERT_THAT(fctx_na.get(), NotNull());
  ScopedObjList ops_nad27(proj_create_operations(
      ctx.get(), nad27.get(), geog_wgs84.get(), fctx_na.get()));
  ASSERT_THAT(ops_nad27.get(), NotNull());
  const int op_count = proj_list_get_count(ops_nad27.get());
  ASSERT_GE(op_count, 1);
  ScopedPj first_op(proj_list_get(ctx.get(), ops_nad27.get(), 0));
  ASSERT_THAT(first_op.get(), NotNull());
  const int suggested = proj_get_suggested_operation(
      ctx.get(), ops_nad27.get(), PJ_FWD, proj_coord(40.0, -100.0, 0.0, 0.0));
  EXPECT_GE(suggested, 0);
  EXPECT_LT(suggested, op_count);
  EXPECT_EQ(0, proj_list_get_count(nullptr));

  // Single-operation list hits opList->objects.size() == 1 branch.
  ScopedPj ellps2d_cs(proj_create_ellipsoidal_2D_cs(
      ctx.get(), PJ_ELLPS2D_LATITUDE_LONGITUDE, nullptr, 0.0));
  ASSERT_THAT(ellps2d_cs.get(), NotNull());
  ScopedPj custom_geog(proj_create_geographic_crs(
      ctx.get(), "CustomGeog", "CustomDatum", "CustomEllps", 6378137.0,
      298.257223563, "Greenwich", 0.0, "degree", kDegToRad, ellps2d_cs.get()));
  ASSERT_THAT(custom_geog.get(), NotNull());
  ScopedObjList ops_single(proj_create_operations(
      ctx.get(), custom_geog.get(), geog_wgs84.get(), fctx_na.get()));
  ASSERT_THAT(ops_single.get(), NotNull());
  if (proj_list_get_count(ops_single.get()) == 1) {
    EXPECT_EQ(0, proj_get_suggested_operation(ctx.get(), ops_single.get(),
                                              PJ_FWD, proj_coord(0, 0, 0, 0)));
  }

  // CoordinateMetadata create/get_epoch + create_operations CM/CRS combos.
  ScopedPj itrf2014(proj_create_from_database(ctx.get(), "EPSG", "7912",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(itrf2014.get(), NotNull());
  ScopedPj cm_itrf(
      proj_coordinate_metadata_create(ctx.get(), itrf2014.get(), 2020.5));
  ASSERT_THAT(cm_itrf.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_COORDINATE_METADATA, proj_get_type(cm_itrf.get()));
  EXPECT_DOUBLE_EQ(
      2020.5, proj_coordinate_metadata_get_epoch(ctx.get(), cm_itrf.get()));

  ScopedPj cm_nrcan(
      proj_coordinate_metadata_create(ctx.get(), itrf2014.get(), 1997.0));
  ASSERT_THAT(cm_nrcan.get(), NotNull());
  EXPECT_DOUBLE_EQ(
      1997.0, proj_coordinate_metadata_get_epoch(ctx.get(), cm_nrcan.get()));

  ScopedPj cm_no_epoch(proj_create(ctx.get(), kCoordMetaWgs84NoEpochWkt));
  ASSERT_THAT(cm_no_epoch.get(), NotNull());
  EXPECT_TRUE(std::isnan(
      proj_coordinate_metadata_get_epoch(ctx.get(), cm_no_epoch.get())));

  ScopedObjList ops_cm_cm(proj_create_operations(
      ctx.get(), cm_itrf.get(), cm_nrcan.get(), fctx_na.get()));
  EXPECT_THAT(ops_cm_cm.get(), NotNull());
  ScopedObjList ops_cm_crs(proj_create_operations(
      ctx.get(), cm_itrf.get(), geog_wgs84.get(), fctx_na.get()));
  EXPECT_THAT(ops_cm_crs.get(), NotNull());
  ScopedObjList ops_crs_cm(proj_create_operations(
      ctx.get(), geog_wgs84.get(), cm_itrf.get(), fctx_na.get()));
  EXPECT_THAT(ops_crs_cm.get(), NotNull());
  ScopedObjList ops_no_epoch(proj_create_operations(
      ctx.get(), cm_no_epoch.get(), cm_no_epoch.get(), fctx_na.get()));
  EXPECT_THAT(ops_no_epoch.get(), NotNull());

  // proj_crs_has_point_motion_operation (true on EPSG:8255, false on 5703).
  ScopedPj nad83_csrs_v7(proj_create_from_database(
      ctx.get(), "EPSG", "8255", PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(nad83_csrs_v7.get(), NotNull());
  EXPECT_EQ(
      1, proj_crs_has_point_motion_operation(ctx.get(), nad83_csrs_v7.get()));
  ScopedPj navd88(proj_create_from_database(ctx.get(), "EPSG", "5703",
                                            PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(navd88.get(), NotNull());
  EXPECT_EQ(0, proj_crs_has_point_motion_operation(ctx.get(), navd88.get()));

  // proj_normalize_for_visualization on multi-op pipeline, CRS, and SingleOp.
  ScopedPj crs_to_crs(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4267", "EPSG:3857", nullptr));
  ASSERT_THAT(crs_to_crs.get(), NotNull());
  ScopedPj crs_to_crs_vis(
      proj_normalize_for_visualization(ctx.get(), crs_to_crs.get()));
  ASSERT_THAT(crs_to_crs_vis.get(), NotNull());

  ScopedPj geog_vis(
      proj_normalize_for_visualization(ctx.get(), geog_wgs84.get()));
  ASSERT_THAT(geog_vis.get(), NotNull());

  ScopedPj op_vis(
      proj_normalize_for_visualization(ctx.get(), helmert_1671.get()));
  ASSERT_THAT(op_vis.get(), NotNull());

  // proj_coordoperation_create_inverse on Conversion & Transformation.
  ScopedPj inv_conv(
      proj_coordoperation_create_inverse(ctx.get(), utm_conv.get()));
  ASSERT_THAT(inv_conv.get(), NotNull());
  ScopedPj inv_transf(
      proj_coordoperation_create_inverse(ctx.get(), helmert_1671.get()));
  ASSERT_THAT(inv_transf.get(), NotNull());

  // proj_concatoperation_get_step_count & proj_concatoperation_get_step.
  ScopedPj concat_3896(proj_create_from_database(
      ctx.get(), "EPSG", "3896", PJ_CATEGORY_COORDINATE_OPERATION, 0, nullptr));
  ASSERT_THAT(concat_3896.get(), NotNull());
  EXPECT_EQ(PJ_TYPE_CONCATENATED_OPERATION, proj_get_type(concat_3896.get()));
  const int step_count =
      proj_concatoperation_get_step_count(ctx.get(), concat_3896.get());
  ASSERT_EQ(2, step_count);
  ScopedPj step0(
      proj_concatoperation_get_step(ctx.get(), concat_3896.get(), 0));
  ScopedPj step1(
      proj_concatoperation_get_step(ctx.get(), concat_3896.get(), 1));
  ASSERT_THAT(step0.get(), NotNull());
  ASSERT_THAT(step1.get(), NotNull());

  // Error / negative / catch branches across CoordinateOperation & factory
  // APIs.
  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_THAT(proj_crs_get_coordoperation(ctx.get(), nullptr), IsNull());
    EXPECT_THAT(proj_crs_get_coordoperation(ctx.get(), ellps.get()), IsNull());

    EXPECT_EQ(
        0, proj_coordoperation_get_method_info(ctx.get(), nullptr, &method_name,
                                               &method_auth, &method_code));
    EXPECT_EQ(0, proj_coordoperation_get_method_info(ctx.get(), ellps.get(),
                                                     &method_name, &method_auth,
                                                     &method_code));

    EXPECT_EQ(0, proj_coordoperation_is_instantiable(ctx.get(), nullptr));
    EXPECT_EQ(0, proj_coordoperation_is_instantiable(ctx.get(), ellps.get()));
    EXPECT_EQ(
        0, proj_coordoperation_has_ballpark_transformation(ctx.get(), nullptr));
    EXPECT_EQ(0, proj_coordoperation_has_ballpark_transformation(ctx.get(),
                                                                 ellps.get()));
    EXPECT_EQ(0, proj_coordoperation_requires_per_coordinate_input_time(
                     ctx.get(), nullptr));
    EXPECT_EQ(0, proj_coordoperation_requires_per_coordinate_input_time(
                     ctx.get(), ellps.get()));

    EXPECT_EQ(0, proj_coordoperation_get_param_count(ctx.get(), nullptr));
    EXPECT_EQ(0, proj_coordoperation_get_param_count(ctx.get(), ellps.get()));
    EXPECT_EQ(-1, proj_coordoperation_get_param_index(ctx.get(), nullptr, "x"));
    EXPECT_EQ(-1, proj_coordoperation_get_param_index(ctx.get(), utm_conv.get(),
                                                      nullptr));
    EXPECT_EQ(-1, proj_coordoperation_get_param_index(ctx.get(), ellps.get(),
                                                      "scale"));
    EXPECT_EQ(0, proj_coordoperation_get_param(
                     ctx.get(), nullptr, 0, &p_name, nullptr, nullptr, nullptr,
                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(0,
              proj_coordoperation_get_param(
                  ctx.get(), ellps.get(), 0, &p_name, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(
        0, proj_coordoperation_get_param(
               ctx.get(), utm_conv.get(), -1, &p_name, nullptr, nullptr,
               nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(
        0, proj_coordoperation_get_param(
               ctx.get(), utm_conv.get(), 99, &p_name, nullptr, nullptr,
               nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));

    EXPECT_EQ(0, proj_coordoperation_get_grid_used_count(ctx.get(), nullptr));
    EXPECT_EQ(0,
              proj_coordoperation_get_grid_used_count(ctx.get(), ellps.get()));
    EXPECT_EQ(0, proj_coordoperation_get_grid_used(
                     ctx.get(), ntv2_5661.get(), -1, &g_short, nullptr, nullptr,
                     nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(0, proj_coordoperation_get_grid_used(
                     ctx.get(), ntv2_5661.get(), 1, &g_short, nullptr, nullptr,
                     nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(0, proj_coordoperation_get_grid_used(
                     ctx.get(), ellps.get(), 0, &g_short, nullptr, nullptr,
                     nullptr, nullptr, nullptr, nullptr));

    EXPECT_LT(proj_coordoperation_get_accuracy(ctx.get(), nullptr), 0.0);
    EXPECT_LT(proj_coordoperation_get_accuracy(ctx.get(), ellps.get()), 0.0);

    EXPECT_EQ(0, proj_coordoperation_get_towgs84_values(ctx.get(), nullptr,
                                                        towgs.data(), 7, 1));
    EXPECT_EQ(0, proj_coordoperation_get_towgs84_values(
                     ctx.get(), utm_conv.get(), towgs.data(), 7, 1));
    EXPECT_EQ(0, proj_coordoperation_get_towgs84_values(
                     ctx.get(), mb_1066.get(), towgs.data(), 7, 1));

    // Null / invalid factory context setters.
    proj_operation_factory_context_set_desired_accuracy(ctx.get(), nullptr,
                                                        1.0);
    proj_operation_factory_context_set_area_of_interest(ctx.get(), nullptr, 0,
                                                        0, 1, 1);
    proj_operation_factory_context_set_area_of_interest(ctx.get(), fctx.get(),
                                                        0.0, 50.0, 10.0, 40.0);
    proj_operation_factory_context_set_area_of_interest_name(ctx.get(), nullptr,
                                                             "France");
    proj_operation_factory_context_set_area_of_interest_name(
        ctx.get(), fctx.get(), nullptr);
    ScopedOpFactoryContext fctx_empty_area(
        proj_create_operation_factory_context(ctx.get(), nullptr));
    proj_operation_factory_context_set_area_of_interest_name(
        ctx.get(), fctx_empty_area.get(), "NoSuchAreaXYZ_123");
    proj_operation_factory_context_set_crs_extent_use(ctx.get(), nullptr,
                                                      PJ_CRS_EXTENT_NONE);
    proj_operation_factory_context_set_spatial_criterion(
        ctx.get(), nullptr, PROJ_SPATIAL_CRITERION_STRICT_CONTAINMENT);
    proj_operation_factory_context_set_grid_availability_use(
        ctx.get(), nullptr, PROJ_GRID_AVAILABILITY_IGNORED);
    proj_operation_factory_context_set_use_proj_alternative_grid_names(
        ctx.get(), nullptr, 1);
    proj_operation_factory_context_set_allow_use_intermediate_crs(
        ctx.get(), nullptr, PROJ_INTERMEDIATE_CRS_USE_NEVER);
    proj_operation_factory_context_set_allowed_intermediate_crs(
        ctx.get(), nullptr, allowed_pivots);
    proj_operation_factory_context_set_discard_superseded(ctx.get(), nullptr,
                                                          1);
    proj_operation_factory_context_set_allow_ballpark_transformations(
        ctx.get(), nullptr, 1);

    EXPECT_THAT(proj_create_operations(ctx.get(), nullptr, geog_wgs84.get(),
                                       fctx.get()),
                IsNull());
    EXPECT_THAT(
        proj_create_operations(ctx.get(), nad27.get(), nullptr, fctx.get()),
        IsNull());
    EXPECT_THAT(proj_create_operations(ctx.get(), nad27.get(), geog_wgs84.get(),
                                       nullptr),
                IsNull());
    EXPECT_THAT(proj_create_operations(ctx.get(), ellps.get(), geog_wgs84.get(),
                                       fctx.get()),
                IsNull());
    EXPECT_THAT(
        proj_create_operations(ctx.get(), nad27.get(), ellps.get(), fctx.get()),
        IsNull());

    EXPECT_THAT(proj_list_get(ctx.get(), nullptr, 0), IsNull());
    EXPECT_THAT(proj_list_get(ctx.get(), ops_nad27.get(), -1), IsNull());
    EXPECT_THAT(proj_list_get(ctx.get(), ops_nad27.get(), op_count), IsNull());

    // Non-operation PJ_OBJ_LIST passed to proj_get_suggested_operation.
    ScopedObjList non_op_list(proj_query_geodetic_crs_from_datum(
        ctx.get(), "EPSG", "EPSG", "6326", nullptr));
    ASSERT_THAT(non_op_list.get(), NotNull());
    EXPECT_EQ(-1, proj_get_suggested_operation(ctx.get(), non_op_list.get(),
                                               PJ_FWD, proj_coord(0, 0, 0, 0)));
    EXPECT_EQ(-1, proj_get_suggested_operation(ctx.get(), nullptr, PJ_FWD,
                                               proj_coord(0, 0, 0, 0)));

    // Non-CRS, non-CoordinateOperation for proj_normalize_for_visualization.
    EXPECT_THAT(proj_normalize_for_visualization(ctx.get(), ellps.get()),
                IsNull());

    // Inverse error paths + Conversion::inverse catch (scalar == 0 on 1069).
    EXPECT_THAT(proj_coordoperation_create_inverse(ctx.get(), nullptr),
                IsNull());
    EXPECT_THAT(proj_coordoperation_create_inverse(ctx.get(), ellps.get()),
                IsNull());
    PJ_PARAM_DESCRIPTION zero_scalar[] = {{.name = "Unit conversion scalar",
                                           .auth_name = "EPSG",
                                           .code = "1051",
                                           .value = 0.0,
                                           .unit_name = "unity",
                                           .unit_conv_factor = 1.0,
                                           .unit_type = PJ_UT_SCALE}};
    ScopedPj uninvertible_conv(proj_create_conversion(
        ctx.get(), "ZeroVertScale", nullptr, nullptr, "Change of Vertical Unit",
        "EPSG", "1069", 1, zero_scalar));
    ASSERT_THAT(uninvertible_conv.get(), NotNull());
    EXPECT_THAT(
        proj_coordoperation_create_inverse(ctx.get(), uninvertible_conv.get()),
        IsNull());

    // ConcatenatedOperation negative paths.
    EXPECT_EQ(0, proj_concatoperation_get_step_count(ctx.get(), nullptr));
    EXPECT_EQ(0, proj_concatoperation_get_step_count(ctx.get(), ellps.get()));
    EXPECT_THAT(proj_concatoperation_get_step(ctx.get(), nullptr, 0), IsNull());
    EXPECT_THAT(proj_concatoperation_get_step(ctx.get(), ellps.get(), 0),
                IsNull());
    EXPECT_THAT(proj_concatoperation_get_step(ctx.get(), concat_3896.get(), -1),
                IsNull());
    EXPECT_THAT(proj_concatoperation_get_step(ctx.get(), concat_3896.get(), 2),
                IsNull());

    // CoordinateMetadata & PointMotionOperation error paths.
    EXPECT_THAT(proj_coordinate_metadata_create(ctx.get(), nullptr, 2020.0),
                IsNull());
    EXPECT_THAT(proj_coordinate_metadata_create(ctx.get(), ellps.get(), 2020.0),
                IsNull());
    // Static CRS cannot hold an arbitrary epoch in CoordinateMetadata::create.
    EXPECT_THAT(
        proj_coordinate_metadata_create(ctx.get(), navd88.get(), 2020.0),
        IsNull());
    EXPECT_TRUE(
        std::isnan(proj_coordinate_metadata_get_epoch(ctx.get(), nullptr)));
    EXPECT_TRUE(
        std::isnan(proj_coordinate_metadata_get_epoch(ctx.get(), ellps.get())));

    EXPECT_EQ(0, proj_crs_has_point_motion_operation(ctx.get(), nullptr));
    EXPECT_EQ(0, proj_crs_has_point_motion_operation(ctx.get(), ellps.get()));
  }
}

TEST(Iso19111CApiTest, ContextCloneNetworkDeferAndOperationEpochs) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // Initialize ctx->cpp_context and clone the context (hits lines 157-161).
  EXPECT_THAT(proj_context_get_database_path(ctx.get()), NotNull());
  ScopedContext cloned_ctx(proj_context_clone(ctx.get()));
  ASSERT_THAT(cloned_ctx.get(), NotNull());

  // Verify proj_context_set_enable_network(1) remains patched to 0 in google3
  // default builds and pj_obj_create succeeds on a grid-backed operation.
  ScopedContext net_ctx = MakeContext();
  ASSERT_THAT(net_ctx.get(), NotNull());
  EXPECT_EQ(0, proj_context_set_enable_network(net_ctx.get(), 1));
  EXPECT_EQ(0, proj_context_is_network_enabled(net_ctx.get()));
  ScopedPj net_op(proj_create_from_database(net_ctx.get(), "EPSG", "1671",
                                            PJ_CATEGORY_COORDINATE_OPERATION, 0,
                                            nullptr));
  ASSERT_THAT(net_op.get(), NotNull());

  // CoordinateOperation instantiation with source-only and target-only epochs
  // in pj_obj_create (hits lines 236-248).
  ScopedPj itrf2014(proj_create_from_database(ctx.get(), "EPSG", "7912",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj wgs84(proj_create_from_database(ctx.get(), "EPSG", "4326",
                                           PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(itrf2014.get(), NotNull());
  ASSERT_THAT(wgs84.get(), NotNull());
  ScopedPj cm_itrf(
      proj_coordinate_metadata_create(ctx.get(), itrf2014.get(), 2020.5));
  ASSERT_THAT(cm_itrf.get(), NotNull());
  ScopedOpFactoryContext fctx(
      proj_create_operation_factory_context(ctx.get(), nullptr));
  ASSERT_THAT(fctx.get(), NotNull());

  ScopedObjList ops_cm_crs(proj_create_operations(ctx.get(), cm_itrf.get(),
                                                  wgs84.get(), fctx.get()));
  ASSERT_THAT(ops_cm_crs.get(), NotNull());
  ASSERT_GT(proj_list_get_count(ops_cm_crs.get()), 0);
  ScopedPj op_from_cm(proj_list_get(ctx.get(), ops_cm_crs.get(), 0));
  EXPECT_THAT(op_from_cm.get(), NotNull());

  ScopedObjList ops_crs_cm(proj_create_operations(ctx.get(), wgs84.get(),
                                                  cm_itrf.get(), fctx.get()));
  ASSERT_THAT(ops_crs_cm.get(), NotNull());
  ASSERT_GT(proj_list_get_count(ops_crs_cm.get()), 0);
  ScopedPj op_to_cm(proj_list_get(ctx.get(), ops_crs_cm.get(), 0));
  EXPECT_THAT(op_to_cm.get(), NotNull());
}

TEST(Iso19111CApiTest, DatabaseOpenExceptionCatchPaths) {
  ScopedContext valid_ctx = MakeContext();
  ASSERT_THAT(valid_ctx.get(), NotNull());
  ScopedPj valid_crs(proj_create_from_database(valid_ctx.get(), "EPSG", "4326",
                                               PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(valid_crs.get(), NotNull());

  // Point cpp_context at a nonexistent explicit database path so getDBcontext
  // throws FactoryException via SQLiteHandle::open.
  ScopedContext bad_db_ctx = MakeContext();
  ASSERT_THAT(bad_db_ctx.get(), NotNull());
  ScopedLogSuppressor suppress(bad_db_ctx.get());
  bad_db_ctx->cpp_context =
      new projCppContext(bad_db_ctx.get(), "/nonexistent_dir_xyz/bad_proj.db");

  EXPECT_THAT(proj_create(bad_db_ctx.get(), "EPSG:4326"), IsNull());
  EXPECT_THAT(proj_context_get_database_path(bad_db_ctx.get()), IsNull());
  EXPECT_THAT(proj_context_get_database_metadata(
                  bad_db_ctx.get(), "DATABASE.LAYOUT.VERSION.MAJOR"),
              IsNull());
  EXPECT_THAT(proj_context_get_database_structure(bad_db_ctx.get(), nullptr),
              IsNull());
  EXPECT_THAT(proj_create_from_database(bad_db_ctx.get(), "EPSG", "4326",
                                        PJ_CATEGORY_CRS, 0, nullptr),
              IsNull());
  EXPECT_EQ(0, proj_uom_get_info_from_database(bad_db_ctx.get(), "EPSG", "9001",
                                               nullptr, nullptr, nullptr));
  EXPECT_EQ(0, proj_grid_get_info_from_database(bad_db_ctx.get(), "ntv2_0.gsb",
                                                nullptr, nullptr, nullptr,
                                                nullptr, nullptr, nullptr));
  EXPECT_THAT(proj_query_geodetic_crs_from_datum(
                  bad_db_ctx.get(), "EPSG", "EPSG", "6326", "geographic 2D"),
              IsNull());
  EXPECT_THAT(proj_create_from_name(bad_db_ctx.get(), "EPSG", "WGS 84", nullptr,
                                    0, 0, 0, nullptr),
              IsNull());
  EXPECT_THAT(proj_get_non_deprecated(bad_db_ctx.get(), valid_crs.get()),
              IsNull());
  EXPECT_THAT(proj_identify(bad_db_ctx.get(), valid_crs.get(), "EPSG", nullptr,
                            nullptr),
              IsNull());
  EXPECT_THAT(proj_get_authorities_from_database(bad_db_ctx.get()), IsNull());
  EXPECT_THAT(proj_get_codes_from_database(bad_db_ctx.get(), "EPSG",
                                           PJ_TYPE_GEOGRAPHIC_2D_CRS, 0),
              IsNull());

  int count = 42;
  EXPECT_THAT(proj_get_celestial_body_list_from_database(bad_db_ctx.get(),
                                                         nullptr, &count),
              IsNull());
  EXPECT_EQ(0, count);

  count = 42;
  EXPECT_THAT(proj_get_crs_info_list_from_database(bad_db_ctx.get(), nullptr,
                                                   nullptr, &count),
              IsNull());
  EXPECT_EQ(0, count);

  count = 42;
  EXPECT_THAT(proj_get_units_from_database(bad_db_ctx.get(), nullptr, nullptr,
                                           0, &count),
              IsNull());
  EXPECT_EQ(0, count);

  ScopedOpFactoryContext fctx_no_db(
      proj_create_operation_factory_context(bad_db_ctx.get(), nullptr));
  ASSERT_THAT(fctx_no_db.get(), NotNull());
  proj_operation_factory_context_set_area_of_interest_name(
      bad_db_ctx.get(), fctx_no_db.get(), "World");

  EXPECT_THAT(proj_insert_object_session_create(bad_db_ctx.get()), IsNull());
  EXPECT_THAT(
      proj_get_insert_statements(bad_db_ctx.get(), nullptr, valid_crs.get(),
                                 "HOBU", "1", 0, nullptr, nullptr),
      IsNull());
  EXPECT_THAT(proj_suggests_code_for(bad_db_ctx.get(), valid_crs.get(), "HOBU",
                                     0, nullptr),
              IsNull());
  EXPECT_THAT(proj_get_geoid_models_from_database(bad_db_ctx.get(), "EPSG",
                                                  "5703", nullptr),
              IsNull());
  EXPECT_EQ(0, proj_crs_has_point_motion_operation(bad_db_ctx.get(),
                                                   valid_crs.get()));

  // Session created before DB failure, then used and destroyed after DB reset.
  ScopedContext sess_ctx = MakeContext();
  ASSERT_THAT(sess_ctx.get(), NotNull());
  ScopedLogSuppressor suppress_sess(sess_ctx.get());
  PJ_INSERT_SESSION* sess = proj_insert_object_session_create(sess_ctx.get());
  ASSERT_THAT(sess, NotNull());
  proj_context_delete_cpp_context(sess_ctx->cpp_context);
  sess_ctx->cpp_context =
      new projCppContext(sess_ctx.get(), "/nonexistent_dir_xyz/bad_proj.db");
  EXPECT_THAT(proj_get_insert_statements(sess_ctx.get(), sess, valid_crs.get(),
                                         "HOBU", "1", 0, nullptr, nullptr),
              IsNull());
  proj_insert_object_session_destroy(sess_ctx.get(), sess);
}

TEST(Iso19111CApiTest, AdditionalBranchesAndEdgeCases) {
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ScopedLogSuppressor suppress(ctx.get());

  // proj_create ParsingException when proj_context_errno(ctx) == 0 (636-637).
  proj_context_errno_set(ctx.get(), 0);
  EXPECT_THAT(proj_create(ctx.get(), "GEOGCRS[unclosed"), IsNull());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_WRONG_SYNTAX, proj_context_errno(ctx.get()));

  // Non-ObjectUsage PJ* passed to proj_get_domain_count and proj_get_scope_ex.
  ScopedPj wgs84(proj_create_from_database(ctx.get(), "EPSG", "4326",
                                           PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(wgs84.get(), NotNull());
  ScopedPj ellps(proj_get_ellipsoid(ctx.get(), wgs84.get()));
  ASSERT_THAT(ellps.get(), NotNull());
  EXPECT_EQ(0, proj_get_domain_count(ellps.get()));
  EXPECT_THAT(proj_get_scope_ex(ellps.get(), 0), IsNull());

  // 2D CRS promoted to 3D has a domain with an unset scope (hits line 1942).
  ScopedPj nad27(proj_create_from_database(ctx.get(), "EPSG", "4267",
                                           PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(nad27.get(), NotNull());
  ScopedPj nad27_renamed(
      proj_alter_name(ctx.get(), nad27.get(), "CustomNAD27"));
  ASSERT_THAT(nad27_renamed.get(), NotNull());
  ScopedPj nad27_3d(
      proj_crs_promote_to_3D(ctx.get(), "CustomNAD27 3D", nad27_renamed.get()));
  ASSERT_THAT(nad27_3d.get(), NotNull());
  EXPECT_GE(proj_get_domain_count(nad27_3d.get()), 1);
  EXPECT_THAT(proj_get_scope_ex(nad27_3d.get(), 0), IsNull());

  // proj_get_crs_info_list_from_database with auth_name=nullptr and bbox filter
  // skips entries with !info.bbox_valid (hits line 3096).
  ScopedCrsListParams bbox_params(proj_get_crs_list_parameters_create());
  ASSERT_THAT(bbox_params.get(), NotNull());
  bbox_params->bbox_valid = 1;
  bbox_params->west_lon_degree = -5.0;
  bbox_params->south_lat_degree = 42.0;
  bbox_params->east_lon_degree = 8.0;
  bbox_params->north_lat_degree = 51.0;
  ScopedCrsInfoList bbox_crs_list(proj_get_crs_info_list_from_database(
      ctx.get(), nullptr, bbox_params.get(), nullptr));
  EXPECT_THAT(bbox_crs_list.get(), NotNull());

  // createGeodeticReferenceFrame with '_' in datum_name matching official name
  // (hits line 3470).
  ScopedPj ellps2d_cs(proj_create_ellipsoidal_2D_cs(
      ctx.get(), PJ_ELLPS2D_LATITUDE_LONGITUDE, nullptr, 0.0));
  ASSERT_THAT(ellps2d_cs.get(), NotNull());
  ScopedPj underscore_datum_crs(proj_create_geographic_crs(
      ctx.get(), "Test NAD83", "North_American_Datum_1983", "GRS 1980",
      6378137.0, 298.257222101, nullptr, 0.0, nullptr, 0.0, ellps2d_cs.get()));
  EXPECT_THAT(underscore_datum_crs.get(), NotNull());

  // proj_crs_alter_geodetic_crs catch block when replacing horizontal component
  // of a CompoundCRS with a 3D GeographicCRS (hits lines 4030-4033).
  ScopedPj navd88(proj_create_from_database(ctx.get(), "EPSG", "5703",
                                            PJ_CATEGORY_CRS, 0, nullptr));
  ScopedPj wgs84_3d(proj_create_from_database(ctx.get(), "EPSG", "4979",
                                              PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(navd88.get(), NotNull());
  ASSERT_THAT(wgs84_3d.get(), NotNull());
  ScopedPj comp_geog_vert(proj_create_compound_crs(ctx.get(), "WGS84 + NAVD88",
                                                   wgs84.get(), navd88.get()));
  ASSERT_THAT(comp_geog_vert.get(), NotNull());
  EXPECT_THAT(proj_crs_alter_geodetic_crs(ctx.get(), comp_geog_vert.get(),
                                          wgs84_3d.get()),
              IsNull());

  // proj_crs_create_projected_3D_crs_from_2D with crs_name == nullptr and
  // explicit geog_3D_crs (hits line 4347).
  ScopedPj utm31n(proj_create_from_database(ctx.get(), "EPSG", "32631",
                                            PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(utm31n.get(), NotNull());
  ScopedPj utm31n_3d_unnamed(proj_crs_create_projected_3D_crs_from_2D(
      ctx.get(), nullptr, utm31n.get(), wgs84_3d.get()));
  EXPECT_THAT(utm31n_3d_unnamed.get(), NotNull());

  // proj_operation_factory_context_set_area_of_interest_name with exact single
  // match from DB (8169-8173) followed by renaming existing extent (8178-8183).
  ScopedPj rgf93_lambert(proj_create_from_database(
      ctx.get(), "EPSG", "2154", PJ_CATEGORY_CRS, 0, nullptr));
  ASSERT_THAT(rgf93_lambert.get(), NotNull());
  const char* exact_area_name = nullptr;
  ASSERT_EQ(
      1, proj_get_area_of_use(ctx.get(), rgf93_lambert.get(), nullptr, nullptr,
                              nullptr, nullptr, &exact_area_name));
  ASSERT_THAT(exact_area_name, NotNull());
  ScopedOpFactoryContext fctx_area(
      proj_create_operation_factory_context(ctx.get(), nullptr));
  ASSERT_THAT(fctx_area.get(), NotNull());
  proj_operation_factory_context_set_area_of_interest_name(
      ctx.get(), fctx_area.get(), exact_area_name);
  proj_operation_factory_context_set_area_of_interest_name(
      ctx.get(), fctx_area.get(), "Renamed Extent");

  // Multi-operation list passed to proj_get_suggested_operation (hits lines
  // 8536-8542 and 8682-8690).
  ScopedOpFactoryContext fctx_multi(
      proj_create_operation_factory_context(ctx.get(), nullptr));
  ASSERT_THAT(fctx_multi.get(), NotNull());
  proj_operation_factory_context_set_spatial_criterion(
      ctx.get(), fctx_multi.get(), PROJ_SPATIAL_CRITERION_PARTIAL_INTERSECTION);
  proj_operation_factory_context_set_grid_availability_use(
      ctx.get(), fctx_multi.get(), PROJ_GRID_AVAILABILITY_IGNORED);
  ScopedObjList ops_multi(proj_create_operations(
      ctx.get(), nad27.get(), wgs84.get(), fctx_multi.get()));
  ASSERT_THAT(ops_multi.get(), NotNull());
  ASSERT_GT(proj_list_get_count(ops_multi.get()), 1);
  const int suggested_idx = proj_get_suggested_operation(
      ctx.get(), ops_multi.get(), PJ_FWD, proj_coord(40.0, -100.0, 0.0, 0.0));
  EXPECT_GE(suggested_idx, 0);

  // proj_normalize_for_visualization on multi-alternative crs_to_crs where
  // target CRS has lat/lon axis order so swapTarget is true (hits 9265-9267).
  ScopedPj nad27_to_wgs84(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4267", "EPSG:4326", nullptr));
  ASSERT_THAT(nad27_to_wgs84.get(), NotNull());
  ScopedPj nad27_to_wgs84_vis(
      proj_normalize_for_visualization(ctx.get(), nad27_to_wgs84.get()));
  EXPECT_THAT(nad27_to_wgs84_vis.get(), NotNull());

  // proj_crs_add_horizontal_derived_conversion catch block when CompoundCRS
  // horizontal component is derived with a 3D CartesianCS (hits 10154-10156).
  ScopedPj comp_proj_vert(proj_create_compound_crs(ctx.get(), "UTM31N + NAVD88",
                                                   utm31n.get(), navd88.get()));
  ASSERT_THAT(comp_proj_vert.get(), NotNull());
  ScopedPj affine_conv(proj_create_linear_affine_parametric_conversion(
      ctx.get(), "Affine2D", 0.0, nullptr, 0.0, 1.0, nullptr, 0.0, 0.0, nullptr,
      0.0, 0.0, nullptr, 0.0, 0.0, nullptr, 0.0, 1.0, nullptr, 0.0));
  ASSERT_THAT(affine_conv.get(), NotNull());
  char name_e[] = "E";
  char abbrev_e[] = "E";
  char dir_east[] = "east";
  char name_n[] = "N";
  char abbrev_n[] = "N";
  char dir_north[] = "north";
  char name_h[] = "H";
  char abbrev_h[] = "H";
  char dir_up[] = "up";
  char unit_metre[] = "metre";
  PJ_AXIS_DESCRIPTION axes3d[3] = {{.name = name_e,
                                    .abbreviation = abbrev_e,
                                    .direction = dir_east,
                                    .unit_name = unit_metre,
                                    .unit_conv_factor = 1.0,
                                    .unit_type = PJ_UT_LINEAR},
                                   {.name = name_n,
                                    .abbreviation = abbrev_n,
                                    .direction = dir_north,
                                    .unit_name = unit_metre,
                                    .unit_conv_factor = 1.0,
                                    .unit_type = PJ_UT_LINEAR},
                                   {.name = name_h,
                                    .abbreviation = abbrev_h,
                                    .direction = dir_up,
                                    .unit_name = unit_metre,
                                    .unit_conv_factor = 1.0,
                                    .unit_type = PJ_UT_LINEAR}};
  ScopedPj cart3d_cs(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_CARTESIAN, 3, axes3d));
  ASSERT_THAT(cart3d_cs.get(), NotNull());
  EXPECT_THAT(proj_crs_add_horizontal_derived_conversion(
                  ctx.get(), "BadCompoundDerived", comp_proj_vert.get(),
                  affine_conv.get(), cart3d_cs.get()),
              IsNull());
}

}  // namespace
}  // namespace proj
