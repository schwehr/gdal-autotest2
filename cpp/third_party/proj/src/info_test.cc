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
// Unit tests for PROJ library info and per-PJ metadata queries
// (proj/src/info.cpp). Note: in google3 default builds, proj.db is embedded in
// memory, PACKAGE_VERSION is "9.9.0", and proj_info() caches a process-global
// searchpath buffer read from pj_get_default_ctx().
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/info.cpp

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::HasSubstr;

std::string MakeGeocentricShiftWkt(std::string_view name,
                                   std::string_view accuracy_clause) {
  std::string wkt = "COORDINATEOPERATION[\"";
  wkt += name;
  wkt +=
      "\","
      "SOURCECRS[GEOGCRS[\"ED50\","
      "DATUM[\"European Datum 1950\","
      "ELLIPSOID[\"International 1924\",6378388,297,"
      "LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],"
      "AXIS[\"geodetic latitude (Lat)\",north,ORDER[1],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]],"
      "AXIS[\"geodetic longitude (Lon)\",east,ORDER[2],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]]],"
      "TARGETCRS[GEOGCRS[\"WGS 84\","
      "DATUM[\"World Geodetic System 1984\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],"
      "AXIS[\"geodetic latitude (Lat)\",north,ORDER[1],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]],"
      "AXIS[\"geodetic longitude (Lon)\",east,ORDER[2],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]]],"
      "METHOD[\"Geocentric translations (geog2D domain)\",ID[\"EPSG\",9603]],"
      "PARAMETER[\"X-axis translation\",-87,LENGTHUNIT[\"metre\",1],"
      "ID[\"EPSG\",8605]],"
      "PARAMETER[\"Y-axis translation\",-98,LENGTHUNIT[\"metre\",1],"
      "ID[\"EPSG\",8606]],"
      "PARAMETER[\"Z-axis translation\",-121,LENGTHUNIT[\"metre\",1],"
      "ID[\"EPSG\",8607]]";
  if (!accuracy_clause.empty()) {
    wkt += ',';
    wkt += accuracy_clause;
  }
  wkt += ']';
  return wkt;
}

TEST(InfoTest, ProjInfoSearchPathAllocationReuseReallocAndCleanup) {
  PJ_CONTEXT* default_ctx = pj_get_default_ctx();
  const std::vector<std::string> original_search_paths =
      default_ctx->search_paths;
  absl::Cleanup restore_default_ctx = [default_ctx, &original_search_paths] {
    default_ctx->set_search_paths(original_search_paths);
    pj_clear_proj_info();
  };

  // Start cleanly with no lingering cached searchpath buffer.
  pj_clear_proj_info();

  // 1. Empty default_ctx->search_paths queries pj_get_default_searchpaths().
  default_ctx->set_search_paths({});
  const PJ_INFO default_info = proj_info();
  EXPECT_EQ(PROJ_VERSION_MAJOR, default_info.major);
  EXPECT_EQ(PROJ_VERSION_MINOR, default_info.minor);
  EXPECT_EQ(PROJ_VERSION_PATCH, default_info.patch);
  EXPECT_STREQ(pj_get_release(), default_info.release);
  EXPECT_STREQ("9.9.0", default_info.version);
  EXPECT_NE(nullptr, default_info.searchpath);
  EXPECT_EQ(0, default_info.path_count);
  EXPECT_EQ(nullptr, default_info.paths);

  // 2. A list containing only an empty string leaves buf == nullptr so
  // info.searchpath falls back to the static empty string, and
  // pj_clear_proj_info skips freeing the static buffer.
  default_ctx->set_search_paths({""});
  const PJ_INFO empty_info = proj_info();
  EXPECT_STREQ("", empty_info.searchpath);
  EXPECT_EQ(1, empty_info.path_count);
  const PJ_INFO empty_again = proj_info();
  EXPECT_EQ(empty_info.searchpath, empty_again.searchpath);
  pj_clear_proj_info();

  // 3. Path list exercising empty-entry skip, initial allocation, in-capacity
  // reuse (*buf_size >= len), and reallocation (*buf_size < len, buf != null).
  const std::vector<std::string> multi_paths = {
      "",
      "/alpha_dir",
      "/b",
      "/very_long_search_path_entry_exceeding_initial_doubled_capacity_size",
  };
  default_ctx->set_search_paths(multi_paths);
  const PJ_INFO multi_info = proj_info();
  EXPECT_EQ(4, multi_info.path_count);
  ASSERT_NE(nullptr, multi_info.paths);
  EXPECT_STREQ("/alpha_dir", multi_info.paths[1]);
  ASSERT_NE(nullptr, multi_info.searchpath);
  const std::string first_searchpath = multi_info.searchpath;
  EXPECT_EQ(
      "/alpha_dir:/b:"
      "/very_long_search_path_entry_exceeding_initial_doubled_capacity_size",
      first_searchpath);

  // 4. Calling proj_info() again frees the previous heap searchpath buffer, and
  // double pj_clear_proj_info() cleanly frees then treats nullptr safely.
  const PJ_INFO second_info = proj_info();
  EXPECT_STREQ(first_searchpath.c_str(), second_info.searchpath);
  pj_clear_proj_info();
  pj_clear_proj_info();
}

TEST(InfoTest, ProjPjInfoCoversNullClassicCrsConversionAndOperations) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Null PJ* returns zero-initialized struct with accuracy == -1.0.
  const PJ_PROJ_INFO null_info = proj_pj_info(nullptr);
  EXPECT_EQ(nullptr, null_info.id);
  EXPECT_EQ(nullptr, null_info.description);
  EXPECT_EQ(nullptr, null_info.definition);
  EXPECT_EQ(0, null_info.has_inverse);
  EXPECT_DOUBLE_EQ(-1.0, null_info.accuracy);

  // 2. Classic invertible projection caches P->def_full on second call.
  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, merc.get());
  EXPECT_EQ(nullptr, merc->def_full);
  const PJ_PROJ_INFO merc_info1 = proj_pj_info(merc.get());
  EXPECT_NE(nullptr, merc->def_full);
  EXPECT_STREQ("merc", merc_info1.id);
  EXPECT_EQ(1, merc_info1.has_inverse);
  EXPECT_DOUBLE_EQ(-1.0, merc_info1.accuracy);
  EXPECT_THAT(merc_info1.definition, HasSubstr("proj=merc"));

  const std::string first_def(merc_info1.definition);
  const PJ_PROJ_INFO merc_info2 = proj_pj_info(merc.get());
  EXPECT_EQ(first_def, merc_info2.definition);

  // 3. Non-invertible projection reports has_inverse == 0.
  ScopedPj august(proj_create(ctx.get(), "+proj=august +R=6371000"));
  ASSERT_NE(nullptr, august.get());
  const PJ_PROJ_INFO august_info = proj_pj_info(august.get());
  EXPECT_STREQ("august", august_info.id);
  EXPECT_EQ(0, august_info.has_inverse);

  // 4. ISO CRS (not Conversion or CoordinateOperation): uses IdentifiedObject
  // name, id == nullptr, accuracy == -1.0, has_inverse == 0.
  ScopedPj wgs84(proj_create(ctx.get(), "EPSG:4326"));
  ASSERT_NE(nullptr, wgs84.get());
  const PJ_PROJ_INFO crs_info = proj_pj_info(wgs84.get());
  EXPECT_EQ(nullptr, crs_info.id);
  EXPECT_STREQ("WGS 84", crs_info.description);
  EXPECT_EQ(0, crs_info.has_inverse);
  EXPECT_DOUBLE_EQ(-1.0, crs_info.accuracy);

  // 5. Single Conversion returned directly by proj_create_crs_to_crs reports
  // accuracy == 0.0 and has_inverse == 1.
  ScopedPj conv(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4326", "EPSG:3857", nullptr));
  ASSERT_NE(nullptr, conv.get());
  const PJ_PROJ_INFO conv_info = proj_pj_info(conv.get());
  EXPECT_DOUBLE_EQ(0.0, conv_info.accuracy);
  EXPECT_EQ(1, conv_info.has_inverse);
  EXPECT_NE(nullptr, conv_info.description);
  EXPECT_STRNE("", conv_info.description);

  // 6. CoordinateOperation (Transformation) with numeric, absent, and
  // non-numeric OPERATIONACCURACY values.
  struct AccuracyCase {
    std::string_view name;
    std::string_view accuracy_clause;
    double expected_accuracy;
  };
  constexpr AccuracyCase kAccuracyCases[] = {
      {
          .name = "Test geocentric shift",
          .accuracy_clause = "OPERATIONACCURACY[2.5]",
          .expected_accuracy = 2.5,
      },
      {
          .name = "Test shift without accuracy",
          .accuracy_clause = "",
          .expected_accuracy = -1.0,
      },
      {
          .name = "Test shift non-numeric accuracy",
          .accuracy_clause = "OPERATIONACCURACY[\"unknown\"]",
          .expected_accuracy = -1.0,
      },
  };
  for (const auto& tc : kAccuracyCases) {
    const std::string wkt = MakeGeocentricShiftWkt(tc.name, tc.accuracy_clause);
    ScopedPj op(proj_create(ctx.get(), wkt.c_str()));
    ASSERT_NE(nullptr, op.get()) << tc.name;
    const PJ_PROJ_INFO info = proj_pj_info(op.get());
    ASSERT_NE(nullptr, info.description);
    EXPECT_EQ(tc.name, std::string_view(info.description));
    EXPECT_DOUBLE_EQ(tc.expected_accuracy, info.accuracy);
    EXPECT_EQ(1, info.has_inverse);
  }
}

TEST(InfoTest, ProjPjInfoAlternativeCoordinateOperationBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Multi-operation pipeline (ED50 EPSG:4230 -> WGS84 EPSG:4326): before
  // proj_trans selects a candidate, info reports placeholder metadata.
  ScopedPj multi(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4230", "EPSG:4326", nullptr));
  ASSERT_NE(nullptr, multi.get());
  ASSERT_GT(multi->alternativeCoordinateOperations.size(), 1U);
  EXPECT_EQ(-1, multi->iCurCoordOp);

  const PJ_PROJ_INFO before_trans = proj_pj_info(multi.get());
  EXPECT_STREQ("unknown", before_trans.id);
  EXPECT_STREQ("unavailable until proj_trans is called",
               before_trans.description);
  EXPECT_STREQ("unavailable until proj_trans is called",
               before_trans.definition);
  EXPECT_DOUBLE_EQ(-1.0, before_trans.accuracy);

  // EPSG:4230 axis order is (latitude, longitude) in degrees; Germany (50N,
  // 10E) selects a concrete transformation and updates iCurCoordOp >= 0.
  const PJ_COORD transformed =
      proj_trans(multi.get(), PJ_FWD, proj_coord(50.0, 10.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(multi.get()));
  EXPECT_NEAR(50.0, transformed.lp.lam, 0.05);
  EXPECT_GE(multi->iCurCoordOp, 0);

  const PJ_PROJ_INFO after_trans = proj_pj_info(multi.get());
  ASSERT_NE(nullptr, after_trans.id);
  EXPECT_STRNE("unknown", after_trans.id);
  EXPECT_STRNE("unavailable until proj_trans is called",
               after_trans.description);
  EXPECT_STRNE("unavailable until proj_trans is called",
               after_trans.definition);
  EXPECT_EQ(1, after_trans.has_inverse);

  // 2. Single instantiable alternative amidst non-instantiable alternatives
  // resolves directly to that candidate even when iCurCoordOp == -1.
  ScopedLogSuppressor suppress(ctx.get());
  ScopedPj container(pj_new());
  ASSERT_NE(nullptr, container.get());
  container->ctx = ctx.get();

  ScopedPj non_inst(pj_new());
  ASSERT_NE(nullptr, non_inst.get());
  non_inst->ctx = ctx.get();

  ScopedPj single_inst(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, single_inst.get());

  container->alternativeCoordinateOperations.reserve(2);
  container->alternativeCoordinateOperations.emplace_back(
      0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, non_inst.release(),
      "+proj=hgridshift +grids=/nonexistent/missing_grid.gsb", -1.0, 0.0,
      nullptr, nullptr, nullptr);
  container->alternativeCoordinateOperations.emplace_back(
      1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, single_inst.release(), "inst",
      0.0, 0.0, nullptr, nullptr, nullptr);

  const PJ_PROJ_INFO single_info = proj_pj_info(container.get());
  EXPECT_DOUBLE_EQ(-1.0, single_info.accuracy);
  EXPECT_EQ(1, single_info.has_inverse);
  EXPECT_STREQ("merc", single_info.id);
}

}  // namespace
}  // namespace proj

// Coverage notes (proj/src/info.cpp):
// - Uncovered defensive branches: null `app` guard in static path_append (line
//   53-54, unreachable from `std::string::c_str()`), calloc OOM fallback (line
//   67-70), and pj_get_def OOM nullptr check (line 240-241).
