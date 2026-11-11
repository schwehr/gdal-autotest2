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
// Unit tests for pj_create_internal(), proj_create_argv(),
// pj_create_argv_internal(), and cs2cs emulation setup (proj/src/create.cpp).
// Note: in google3 default non-GCE builds, networking is disabled and proj.db
// is embedded in memory.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/create.cpp

#include <clocale>
#include <cmath>
#include <numbers>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double DegToRad(double deg) {
  return deg * (std::numbers::pi / 180.0);
}

TEST(CreateTest, CreateInternalParsesFreeFormatQuotesAndRejectsEmpty) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  absl::Cleanup reset_default = [] {
    proj_errno_reset(nullptr);
    proj_cleanup();
  };

  // 1. Empty and delimiter-only definitions yield argc == 0.
  for (const char* empty_def : {"", "   \t\n  ", "; ; + ;"}) {
    proj_context_errno_set(ctx.get(), 0);
    EXPECT_EQ(nullptr, pj_create_internal(ctx.get(), empty_def));
    EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
  }

  // 2. Free-format spacing, semicolons, and quoted strings with spaces.
  ScopedPj free_fmt(pj_create_internal(
      ctx.get(), R"(proj = merc ; lon_0 = 10 ; ellps = WGS84 )"
                 R"(+title="Mercator ""Quoted"" Title")"));
  ASSERT_NE(nullptr, free_fmt.get());
  EXPECT_EQ(0, proj_errno(free_fmt.get()));

  const PJ_COORD transformed =
      proj_trans(free_fmt.get(), PJ_FWD,
                 proj_coord(DegToRad(11.0), DegToRad(0.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(free_fmt.get()));
  EXPECT_GT(transformed.xy.x, 100000.0);
  EXPECT_NEAR(0.0, transformed.xy.y, 1e-6);

  // 3. Passing nullptr ctx falls back to the default context (+proj=noop avoids
  //    opening proj.db on the singleton default context).
  ScopedPj default_ctx_pj(pj_create_internal(nullptr, "+proj=noop"));
  ASSERT_NE(nullptr, default_ctx_pj.get());
  EXPECT_EQ(pj_get_default_ctx(), default_ctx_pj->ctx);
  EXPECT_EQ(0, proj_errno(default_ctx_pj.get()));
}

TEST(CreateTest, CreateArgvAndInternalArgvHandleNullArgvAndMutableBuffers) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  absl::Cleanup reset_default = [] {
    proj_errno_reset(nullptr);
    proj_cleanup();
  };

  // 1. Null argv fails with PROJ_ERR_INVALID_OP_MISSING_ARG on both functions.
  EXPECT_EQ(nullptr, proj_create_argv(ctx.get(), 0, nullptr));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);

  EXPECT_EQ(nullptr, pj_create_argv_internal(ctx.get(), 0, nullptr));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);

  // 2. Null ctx fallback in proj_create_argv and pj_create_argv_internal.
  EXPECT_EQ(nullptr, proj_create_argv(nullptr, 0, nullptr));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(nullptr));
  proj_errno_reset(nullptr);

  char noop_arg[] = "+proj=noop";
  char* noop_argv[] = {noop_arg};
  ScopedPj default_ctx_pj(pj_create_argv_internal(nullptr, 1, noop_argv));
  ASSERT_NE(nullptr, default_ctx_pj.get());
  EXPECT_EQ(pj_get_default_ctx(), default_ctx_pj->ctx);
  EXPECT_EQ(0, proj_errno(default_ctx_pj.get()));

  // 3. Mutable argv buffers including spaces trigger quoting in pj_make_args.
  char arg0[] = "proj=merc";
  char arg1[] = "lon_0=15";
  char arg2[] = "+ellps=WGS84";
  char arg3[] = "title=My Mercator";
  char* valid_argv[] = {arg0, arg1, arg2, arg3};
  ScopedPj pj(proj_create_argv(ctx.get(), 4, valid_argv));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_COORD fwd = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(16.0), DegToRad(0.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(fwd.xy.x, 100000.0);

  // 4. Unknown projection yields P == nullptr -> cs2cs_emulation_setup
  //    returns 0 cleanly.
  char bad_arg[] = "+proj=unknown_projection_xyz";
  char* bad_argv[] = {bad_arg};
  EXPECT_EQ(nullptr, pj_create_argv_internal(ctx.get(), 1, bad_argv));
  EXPECT_NE(0, proj_context_errno(ctx.get()));
}

TEST(CreateTest, InitEpsgFollowsProj4InitRulesOnContext) {
  absl::Cleanup cleanup_globals = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Disallowed by default: +init=epsg:4326 is rejected.
  EXPECT_EQ(0, proj_context_get_use_proj4_init_rules(ctx.get(), 0));
  EXPECT_EQ(nullptr, pj_create_internal(ctx.get(), "+init=epsg:4326"));
  EXPECT_NE(0, proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);

  // 2. Enabling proj4 init rules allows +init=epsg:4326 via internal creation.
  proj_context_use_proj4_init_rules(ctx.get(), 1);
  EXPECT_EQ(1, proj_context_get_use_proj4_init_rules(ctx.get(), 0));
  ScopedPj epsg_pj(pj_create_internal(ctx.get(), "+init=epsg:4326"));
  ASSERT_NE(nullptr, epsg_pj.get());
  EXPECT_EQ(0, proj_errno(epsg_pj.get()));
  EXPECT_EQ(1, epsg_pj->is_latlong);
}

TEST(CreateTest, Cs2csEmulationHandlesAxisRecursionBreakAndInvalidAxis) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. break_cs2cs_recursion skips axisswap, helmert, and cart sub-operations.
  ScopedPj no_recurse(
      pj_create_internal(ctx.get(),
                         "+proj=merc +ellps=intl +axis=neu +towgs84=10,20,30 "
                         "+break_cs2cs_recursion"));
  ASSERT_NE(nullptr, no_recurse.get());
  EXPECT_EQ(nullptr, no_recurse->axisswap);
  EXPECT_EQ(nullptr, no_recurse->helmert);
  EXPECT_EQ(nullptr, no_recurse->cart);
  EXPECT_EQ(nullptr, no_recurse->cart_wgs84);

  // 2. +axis=neu constructs P->axisswap with skip_fwd/inv prepare & finalize.
  ScopedPj neu(
      pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84 +axis=neu"));
  ASSERT_NE(nullptr, neu.get());
  ASSERT_NE(nullptr, neu->axisswap);
  EXPECT_EQ(1, neu->axisswap->skip_fwd_prepare);
  EXPECT_EQ(1, neu->axisswap->skip_fwd_finalize);
  EXPECT_EQ(1, neu->axisswap->skip_inv_prepare);
  EXPECT_EQ(1, neu->axisswap->skip_inv_finalize);

  // 3. +axis=enu also builds axisswap cleanly because p->param is "axis=enu".
  ScopedPj enu(
      pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84 +axis=enu"));
  ASSERT_NE(nullptr, enu.get());
  EXPECT_EQ(0, proj_errno(enu.get()));

  // 4. Duplicate axis ("eee") passes init.cpp character check but fails in
  //    axisswap, exercising cs2cs_emulation_setup's Q == nullptr failure exit.
  EXPECT_EQ(nullptr,
            pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84 +axis=eee"));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
}

TEST(CreateTest, Cs2csEmulationConfiguresGeoidGridsAndRejectsMissingGrid) {
  absl::Cleanup cleanup_globals = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  InMemoryFileSystem fs;
  constexpr float kGeoidShifts[] = {10.0f, 10.0f, 10.0f, 10.0f};
  fs.AddFile("/mem/geoid.gtx",
             BuildGtxGrid(-10.0, -10.0, 20.0, 20.0, 2, 2, kGeoidShifts));
  fs.AddFile("/mem/geoid space.gtx",
             BuildGtxGrid(-10.0, -10.0, 20.0, 20.0, 2, 2, kGeoidShifts));
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 1. Valid geoid grid attaches P->vgridshift with skip_prep_fin flags set.
  ScopedPj valid(pj_create_internal(
      ctx.get(), "+proj=merc +ellps=WGS84 +geoidgrids=/mem/geoid.gtx"));
  ASSERT_NE(nullptr, valid.get());
  ASSERT_NE(nullptr, valid->vgridshift);
  EXPECT_EQ(1, valid->vgridshift->skip_fwd_prepare);
  EXPECT_EQ(1, valid->vgridshift->skip_fwd_finalize);

  // 2. Grid path with spaces exercises pj_double_quote_string_param_if_needed.
  ScopedPj spaced(pj_create_internal(
      ctx.get(),
      R"(+proj=merc +ellps=WGS84 +geoidgrids="/mem/geoid space.gtx")"));
  ASSERT_NE(nullptr, spaced.get());
  EXPECT_NE(nullptr, spaced->vgridshift);

  // 3. Empty value and disable_grid_presence_check skip creating vgridshift.
  ScopedPj empty_val(
      pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84 +geoidgrids="));
  ASSERT_NE(nullptr, empty_val.get());
  EXPECT_EQ(nullptr, empty_val->vgridshift);

  ScopedPj disabled(
      pj_create_internal(ctx.get(),
                         "+proj=merc +ellps=WGS84 +geoidgrids=/mem/missing.gtx "
                         "+disable_grid_presence_check"));
  ASSERT_NE(nullptr, disabled.get());
  EXPECT_EQ(nullptr, disabled->vgridshift);

  // 4. Missing geoid grid fails vgridshift creation and tears down P cleanly.
  EXPECT_EQ(
      nullptr,
      pj_create_internal(
          ctx.get(), "+proj=merc +ellps=WGS84 +geoidgrids=/mem/missing.gtx"));
  EXPECT_NE(0, proj_context_errno(ctx.get()));
}

TEST(CreateTest, Cs2csEmulationConfiguresNadGridsAndSkipsTowgs84) {
  absl::Cleanup cleanup_globals = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  InMemoryFileSystem fs;
  constexpr float kHorizontalShifts[] = {
      0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
  };
  fs.AddFile("/mem/shift.ct2",
             BuildCTable2Grid(-0.2, -0.2, 0.4, 0.4, 2, 2, kHorizontalShifts));
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 1. Valid nadgrids creates hgridshift and ignores towgs84 on the same PJ.
  ScopedPj valid(pj_create_internal(
      ctx.get(),
      "+proj=merc +ellps=intl +nadgrids=/mem/shift.ct2 +towgs84=100,200,300"));
  ASSERT_NE(nullptr, valid.get());
  ASSERT_NE(nullptr, valid->hgridshift);
  EXPECT_EQ(1, valid->hgridshift->skip_inv_prepare);
  EXPECT_EQ(nullptr, valid->helmert);
  EXPECT_EQ(nullptr, valid->cart);
  EXPECT_EQ(nullptr, valid->cart_wgs84);

  // 2. Empty value and disable_grid_presence_check skip hgridshift creation.
  ScopedPj empty_val(
      pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84 +nadgrids="));
  ASSERT_NE(nullptr, empty_val.get());
  EXPECT_EQ(nullptr, empty_val->hgridshift);

  ScopedPj disabled(
      pj_create_internal(ctx.get(),
                         "+proj=merc +ellps=WGS84 +nadgrids=/mem/missing.ct2 "
                         "+disable_grid_presence_check"));
  ASSERT_NE(nullptr, disabled.get());
  EXPECT_EQ(nullptr, disabled->hgridshift);

  // 3. Missing nadgrids grid triggers Q == nullptr failure return.
  EXPECT_EQ(nullptr, pj_create_internal(
                         ctx.get(),
                         "+proj=merc +ellps=WGS84 +nadgrids=/mem/missing.ct2"));
  EXPECT_NE(0, proj_context_errno(ctx.get()));
}

TEST(CreateTest, Cs2csEmulationConfiguresTowgs84CartAndGeocentBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. All-zero towgs84 on WGS84 needs neither helmert nor cart operations.
  ScopedPj zero_wgs84(
      pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84 +towgs84=0,0,0"));
  ASSERT_NE(nullptr, zero_wgs84.get());
  EXPECT_EQ(nullptr, zero_wgs84->helmert);
  EXPECT_EQ(nullptr, zero_wgs84->cart);
  EXPECT_EQ(nullptr, zero_wgs84->cart_wgs84);

  // 2. All-zero towgs84 on a non-WGS84 ellipsoid sets do_cart=1 -> cart +
  //    cart_wgs84 are created without a helmert helper.
  ScopedPj zero_intl(
      pj_create_internal(ctx.get(), "+proj=merc +ellps=intl +towgs84=0,0,0"));
  ASSERT_NE(nullptr, zero_intl.get());
  EXPECT_EQ(nullptr, zero_intl->helmert);
  ASSERT_NE(nullptr, zero_intl->cart);
  ASSERT_NE(nullptr, zero_intl->cart_wgs84);
  EXPECT_EQ(1, zero_intl->cart->skip_fwd_prepare);
  EXPECT_EQ(1, zero_intl->cart_wgs84->skip_fwd_finalize);

  // 3. Non-zero 3-param and 7-param towgs84 build helmert (inheriting source
  //    ellipsoid) along with cart and cart_wgs84.
  ScopedPj shift_3p(pj_create_internal(
      ctx.get(), "+proj=merc +ellps=intl +towgs84=100,200,300"));
  ASSERT_NE(nullptr, shift_3p.get());
  ASSERT_NE(nullptr, shift_3p->helmert);
  ASSERT_NE(nullptr, shift_3p->cart);
  ASSERT_NE(nullptr, shift_3p->cart_wgs84);
  EXPECT_NEAR(shift_3p->a, shift_3p->helmert->a, 1e-6);
  EXPECT_EQ(1, shift_3p->helmert->skip_fwd_prepare);

  ScopedPj shift_7p(pj_create_internal(
      ctx.get(), "+proj=merc +ellps=intl +towgs84=10,20,30,0.5,-0.3,0.8,1.5"));
  ASSERT_NE(nullptr, shift_7p.get());
  EXPECT_NE(nullptr, shift_7p->helmert);
  EXPECT_NE(nullptr, shift_7p->cart);
  EXPECT_NE(nullptr, shift_7p->cart_wgs84);

  // 4. Geocentric operation sets P->cart and skips P->cart_wgs84.
  ScopedPj geocent(pj_create_internal(ctx.get(), "+proj=geocent +ellps=WGS84"));
  ASSERT_NE(nullptr, geocent.get());
  EXPECT_EQ(1, geocent->is_geocent);
  EXPECT_NE(nullptr, geocent->cart);
  EXPECT_EQ(nullptr, geocent->cart_wgs84);

  // 5. Quoted towgs84 embedding obsolete +transpose passes datum_set parsing
  //    but fails when passed to the child helmert constructor -> Q == nullptr.
  EXPECT_EQ(
      nullptr,
      pj_create_internal(
          ctx.get(), R"(+proj=merc +ellps=intl +towgs84="1,2,3 +transpose")"));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
}

TEST(CreateTest, Cs2csEmulationHandlesNonDotLocaleWhenAvailable) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  const char* prev_locale = std::setlocale(LC_NUMERIC, nullptr);
  const std::string saved_locale = prev_locale != nullptr ? prev_locale : "C";
  absl::Cleanup restore_locale = [&saved_locale] {
    std::setlocale(LC_NUMERIC, saved_locale.c_str());
  };

  for (const char* candidate : {"de_DE.UTF-8", "fr_FR.UTF-8", "de_DE.utf8"}) {
    if (std::setlocale(LC_NUMERIC, candidate) != nullptr) {
      break;
    }
  }

  ScopedPj pj(pj_create_internal(
      ctx.get(), "+proj=merc +ellps=intl +towgs84=100,200,300"));
  ASSERT_NE(nullptr, pj.get());
  ASSERT_NE(nullptr, pj->cart);
  EXPECT_NEAR(6378388.0, pj->cart->a, 1e-6);
  EXPECT_NEAR(0.006722670022333322, pj->cart->es, 1e-12);
}

// TODO(schwehr): Remaining uncovered lines in third_party/proj4/proj/src/:
// create.cpp: (1) allocation-failure OOM branches (lines 78-79, 96-97, 115-116,
// 188-189, 196-197, 228-230, 242-245, 276-278), (2) dead defensive check
// `if (n <= 8) return 0` at lines 147-148 (not reachable because datum_set.cpp
// reads the same first "towgs84" token, leaving datum_params[] zeroed if its
// length is <= 8), and (3) lines 183-184 in environments lacking a comma
// LC_NUMERIC system locale.

}  // namespace
}  // namespace proj
