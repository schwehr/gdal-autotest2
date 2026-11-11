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
// Unit tests for PROJ legacy init-string expansion and PJ initialization
// (proj/src/init.cpp). Note: in google3 default non-GCE builds, proj.db is
// embedded in memory and curl networking is disabled.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/init.cpp

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

struct ParalistDeleter {
  PJ_CONTEXT* ctx;
  void operator()(paralist* list) const noexcept {
    if (list != nullptr) {
      free_params(ctx, list, 0);
    }
  }
};

using ScopedParalist = std::unique_ptr<paralist, ParalistDeleter>;

PJ* InitViaArgv(PJ_CONTEXT* ctx, std::initializer_list<const char*> args,
                int allow_init_epsg) {
  std::vector<std::string> storage;
  storage.reserve(args.size());
  for (const char* arg : args) {
    storage.emplace_back(arg);
  }
  std::vector<char*> ptrs;
  ptrs.reserve(storage.size());
  for (std::string& s : storage) {
    ptrs.push_back(s.data());
  }
  return pj_init_ctx_with_allow_init_epsg(ctx, static_cast<int>(ptrs.size()),
                                          ptrs.data(), allow_init_epsg);
}

TEST(InitTest, GetInitStringSectionParsingAndErrors) {
  absl::Cleanup cleanup = [] { pj_clear_initcache(); };
  pj_clear_initcache();

  InMemoryFileSystem fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  ASSERT_TRUE(fs.Install(ctx.get()));

  std::string sections_content =
      "\n"
      "# leading comment line\n"
      "<x> +proj=latlong <>\n"
      "<target_longer> +proj=latlong <>\n"
      "<other1> +proj=latlong <>\n"
      "<empty>\n"
      "<>\n"
      "<target> +proj=merc # inline comment stripped\n"
      "  +lon_0=10 +ellps=WGS84\n"
      "<> trailing text on closing tag\n";

  // Build a >5500-byte section using <1000-char lines so get_init_string
  // triggers buffer reallocation across multiple chunks.
  std::string big_sec = "<big_sec> +proj=merc +ellps=WGS84\n";
  std::string repeated_params = " ";
  for (int i = 0; i < 40; ++i) {
    repeated_params += " +lon_0=0";
  }
  repeated_params += "\n";
  for (int line = 0; line < 18; ++line) {
    big_sec += repeated_params;
  }
  big_sec += "<>\n";
  sections_content += big_sec;

  // Section closed by EOF (no '<' delimiter on subsequent lines).
  sections_content += "<eof_sec> +proj=merc\n  +ellps=WGS84";

  fs.AddTextFile("/mem/sections.init", sections_content);
  fs.AddTextFile("/mem/overlong.init",
                 "# " + std::string(1010, 'a') +
                     "\n<target> +proj=merc +ellps=WGS84 <>\n");

  // Missing colon in +init key.
  EXPECT_EQ(nullptr, pj_create_internal(ctx.get(), "+init=/mem/no_colon"));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_WRONG_SYNTAX, proj_context_errno(ctx.get()));

  // Over-long key exceeding MAX_PATH_FILENAME + ID_TAG_MAX + 2.
  const std::string long_init =
      "+init=/mem/" + std::string(MAX_PATH_FILENAME + ID_TAG_MAX + 10, 'a') +
      ":sec";
  EXPECT_EQ(nullptr, pj_create_internal(ctx.get(), long_init.c_str()));

  // Non-existent init file.
  EXPECT_EQ(nullptr, pj_create_internal(ctx.get(),
                                        "+init=/mem/does_not_exist.init:sec"));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_WRONG_SYNTAX, proj_context_errno(ctx.get()));

  // Init file with a line exceeding MAX_LINE_LENGTH before target section.
  EXPECT_EQ(nullptr,
            pj_create_internal(ctx.get(), "+init=/mem/overlong.init:target"));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_WRONG_SYNTAX, proj_context_errno(ctx.get()));

  // Section name not present in file (EOF reached while searching).
  EXPECT_EQ(nullptr, pj_create_internal(
                         ctx.get(), "+init=/mem/sections.init:nonexistent"));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_WRONG_SYNTAX, proj_context_errno(ctx.get()));

  // Empty section produces empty definition -> string_to_paralist returns null.
  EXPECT_EQ(nullptr,
            pj_create_internal(ctx.get(), "+init=/mem/sections.init:empty"));

  // Valid multi-line section with comments and skipped non-matching tags.
  {
    ScopedPj pj(
        pj_create_internal(ctx.get(), "+init=/mem/sections.init:target"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(proj_torad(10.0), pj->lam0, 1e-12);
  }

  // Double "init=" exercises the second strstr("init=") strip inside
  // get_init_string after get_init strips the first prefix.
  {
    ScopedPj pj(
        pj_create_internal(ctx.get(), "+init=init=/mem/sections.init:target"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(proj_torad(10.0), pj->lam0, 1e-12);
  }

  // Section spanning >5005 bytes forces buffer doubling in get_init_string.
  {
    ScopedPj pj(
        pj_create_internal(ctx.get(), "+init=/mem/sections.init:big_sec"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
  }

  // Section terminated at EOF without '<'.
  {
    ScopedPj pj(
        pj_create_internal(ctx.get(), "+init=/mem/sections.init:eof_sec"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
  }
}

TEST(InitTest, GetInitCacheEpsgIgnfAndFileFinder) {
  absl::Cleanup cleanup = [] { pj_clear_initcache(); };
  pj_clear_initcache();

  InMemoryFileSystem fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  ASSERT_TRUE(fs.Install(ctx.get()));
  fs.AddTextFile("/mem/custom.init",
                 "<merc_grs80> +proj=merc +lon_0=5 +ellps=GRS80 <>\n");

  // Null paralist input to pj_expand_init is a safe no-op.
  EXPECT_EQ(nullptr, pj_expand_init(ctx.get(), nullptr));

  // pj_expand_init without an "init=" prefix uses the whole token as key.
  {
    ScopedParalist raw(pj_mkparam("/mem/custom.init:merc_grs80"),
                       ParalistDeleter{ctx.get()});
    ASSERT_NE(nullptr, raw.get());
    paralist* expanded = pj_expand_init(ctx.get(), raw.get());
    ASSERT_EQ(raw.get(), expanded);
    EXPECT_NE(nullptr, pj_param_exists(expanded, "proj"));
  }

  // Second expansion hits the initcache populated above.
  {
    ScopedParalist raw(pj_mkparam("+init=/mem/custom.init:merc_grs80"),
                       ParalistDeleter{ctx.get()});
    ASSERT_NE(nullptr, raw.get());
    paralist* expanded = pj_expand_init(ctx.get(), raw.get());
    ASSERT_EQ(raw.get(), expanded);
    EXPECT_NE(nullptr, pj_param_exists(expanded, "ellps"));
  }

  // Parameters specified before/outside init expansion take precedence.
  {
    ScopedPj pj(pj_create_internal(
        ctx.get(), "+init=/mem/custom.init:merc_grs80 +ellps=bessel"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(6377397.155, pj->a, 1e-3);
  }

  // epsg: lookup disallowed when allow_init_epsg == 0 (first probe caches
  // ctx->epsg_file_exists = 0).
  {
    ScopedPj pj(InitViaArgv(ctx.get(), {"+init=epsg:32632"},
                            /*allow_init_epsg=*/0));
    EXPECT_EQ(nullptr, pj.get());
  }

  // epsg: lookup allowed when allow_init_epsg == 1 reuses cached epsg check and
  // synthesizes from proj.db.
  {
    ScopedPj pj(InitViaArgv(ctx.get(), {"+init=epsg:32632"},
                            /*allow_init_epsg=*/1));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
  }

  // Over-long epsg key (> 64 bytes) is rejected before buffer copy.
  {
    const std::string long_epsg = "+init=epsg:" + std::string(60, '1');
    ScopedPj pj(InitViaArgv(ctx.get(), {long_epsg.c_str()},
                            /*allow_init_epsg=*/1));
    EXPECT_EQ(nullptr, pj.get());
  }

  // Unknown EPSG code returns null when proj_create fails.
  {
    ScopedPj pj(InitViaArgv(ctx.get(), {"+init=epsg:99999999"},
                            /*allow_init_epsg=*/1));
    EXPECT_EQ(nullptr, pj.get());
  }

  // Engineering CRS (EPSG:5800) succeeds in proj_create but lacks a PROJ.4
  // string.
  {
    ScopedPj pj(InitViaArgv(ctx.get(), {"+init=epsg:5800"},
                            /*allow_init_epsg=*/1));
    EXPECT_EQ(nullptr, pj.get());
  }

  // IGNF: lookup falls back to proj.db when no "IGNF" init file exists.
  {
    ScopedPj pj(InitViaArgv(ctx.get(), {"+init=IGNF:LAMB93"},
                            /*allow_init_epsg=*/1));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
  }
  {
    ScopedPj pj(InitViaArgv(ctx.get(), {"+init=IGNF:NO_SUCH_CODE"},
                            /*allow_init_epsg=*/1));
    EXPECT_EQ(nullptr, pj.get());
  }

  // Use a fresh context with a custom file_finder resolving "epsg" to an
  // in-memory init file so exists == 1 falls through to get_init_string().
  pj_clear_initcache();
  InMemoryFileSystem fs_epsg;
  ScopedContext ctx_with_epsg_file = MakeContext();
  ASSERT_NE(nullptr, ctx_with_epsg_file.get());
  ScopedLogSuppressor suppress_epsg(ctx_with_epsg_file.get());
  ASSERT_TRUE(fs_epsg.Install(ctx_with_epsg_file.get()));
  fs_epsg.AddTextFile("/mem/epsg",
                      "<999001> +proj=merc +lon_0=12 +ellps=WGS84 <>\n");

  const auto epsg_finder = [](PJ_CONTEXT*, const char* filename,
                              void*) -> const char* {
    if (filename != nullptr && std::strcmp(filename, "epsg") == 0) {
      return "/mem/epsg";
    }
    return nullptr;
  };
  proj_context_set_file_finder(ctx_with_epsg_file.get(), epsg_finder, nullptr);

  ScopedPj custom_epsg_pj(InitViaArgv(ctx_with_epsg_file.get(),
                                      {"+init=epsg:999001"},
                                      /*allow_init_epsg=*/1));
  ASSERT_NE(nullptr, custom_epsg_pj.get());
  EXPECT_EQ(0, proj_errno(custom_epsg_pj.get()));
  EXPECT_NEAR(proj_torad(12.0), custom_epsg_pj->lam0, 1e-12);
}

TEST(InitTest, ArgumentCountPipelineInitAndDefaultEllipsoidBranches) {
  absl::Cleanup cleanup = [] { pj_clear_initcache(); };
  pj_clear_initcache();

  InMemoryFileSystem fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  ASSERT_TRUE(fs.Install(ctx.get()));
  fs.AddTextFile("/mem/pipe.init", "<s1> +proj=merc +ellps=WGS84 <>\n");

  // argc <= 0 branch.
  char dummy_arg[] = "+proj=merc";
  char* dummy_argv[] = {dummy_arg};
  EXPECT_EQ(nullptr, pj_init_ctx_with_allow_init_epsg(ctx.get(), 0, dummy_argv,
                                                      /*allow_init_epsg=*/0));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));

  // Nested top-level pipelines are rejected.
  EXPECT_EQ(nullptr,
            InitViaArgv(ctx.get(), {"+proj=pipeline", "proj=pipeline"}, 0));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_WRONG_SYNTAX, proj_context_errno(ctx.get()));

  // Multiple +init outside pipelines are rejected.
  EXPECT_EQ(
      nullptr,
      InitViaArgv(ctx.get(),
                  {"+init=/mem/pipe.init:s1", "init=/mem/pipe.init:s1"}, 0));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_WRONG_SYNTAX, proj_context_errno(ctx.get()));

  // Pipeline with a single +init inside a +step defers expansion to pipeline.
  {
    ScopedPj pj(InitViaArgv(
        ctx.get(), {"+proj=pipeline", "+step", "+init=/mem/pipe.init:s1"}, 0));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
  }

  // Missing +proj, empty +proj=, and unknown projection name.
  EXPECT_EQ(nullptr, InitViaArgv(ctx.get(), {"+ellps=WGS84"}, 0));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));

  EXPECT_EQ(nullptr, InitViaArgv(ctx.get(), {"+proj="}, 0));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));

  EXPECT_EQ(nullptr, InitViaArgv(ctx.get(), {"+proj=nonexistent_proj_xyz"}, 0));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));

  // append_default_ellipsoid_to_paralist branches:
  // 1. No ellipsoid info -> defaults to GRS80.
  {
    ScopedPj pj(InitViaArgv(ctx.get(), {"+proj=merc"}, 0));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(6378137.0, pj->a, 1e-6);
    EXPECT_GT(pj->es, 0.0);
  }

  // 2. +no_defs disables default ellipsoid: fails when need_ellps == 1,
  // falls back to WGS84 when need_ellps == 0.
  EXPECT_EQ(nullptr, InitViaArgv(ctx.get(), {"+proj=merc", "+no_defs"}, 0));
  {
    ScopedPj pj(InitViaArgv(ctx.get(), {"+proj=noop", "+no_defs"}, 0));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(6378137.0, pj->a, 1e-6);
  }
  // need_ellps == 0 with PIN->a already populated before invalid +f=-1 leaves
  // original errno set and restored at exit of pj_init_ctx.
  {
    ScopedPj pj(
        InitViaArgv(ctx.get(), {"+proj=noop", "+a=6378000", "+f=-1"}, 0));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }

  // 3. +datum=, +ellps=, +a= prevent appending ellps=GRS80.
  {
    ScopedPj pj(InitViaArgv(ctx.get(), {"+proj=merc", "+datum=WGS84"}, 0));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
  }
  {
    ScopedPj pj(InitViaArgv(ctx.get(), {"+proj=merc", "+ellps=intl"}, 0));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
  }
  {
    ScopedPj pj(InitViaArgv(ctx.get(), {"+proj=merc", "+a=6378000"}, 0));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(0.0, pj->es);
  }

  // 4. +b, +rf, +f, +e, +es alone each suppress ellps=GRS80 and fail ellps
  // setup because semimajor axis a is missing.
  constexpr const char* kShapeOnlyFlags[] = {
      "+b=6356752", "+rf=298.25", "+f=0.00335", "+e=0.08", "+es=0.0067",
  };
  for (const char* flag : kShapeOnlyFlags) {
    EXPECT_EQ(nullptr, InitViaArgv(ctx.get(), {"+proj=merc", flag}, 0)) << flag;
  }

  // +a with +f=1.0 passes ellps_shape (f >= 0) then fails
  // pj_calc_ellipsoid_params (f must be < 1.0).
  EXPECT_EQ(nullptr,
            InitViaArgv(ctx.get(), {"+proj=merc", "+a=6378137", "+f=1.0"}, 0));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
}

TEST(InitTest, DatumUpgradeGeocOverForceOverAndGeoidGrids) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  pj_clear_initcache();

  InMemoryFileSystem fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  ASSERT_TRUE(fs.Install(ctx.get()));

  // Invalid datum name fails pj_datum_set.
  EXPECT_EQ(nullptr,
            pj_create_internal(ctx.get(), "+proj=merc +datum=invalid_datum"));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));

  // PJD_3PARAM with 0,0,0 shifts and WGS84/GRS80 upgrades to PJD_WGS84.
  {
    ScopedPj pj(pj_create_internal(ctx.get(),
                                   "+proj=merc +ellps=WGS84 +towgs84=0,0,0"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(PJD_WGS84, pj->datum_type);
  }
  {
    ScopedPj pj(pj_create_internal(ctx.get(),
                                   "+proj=merc +ellps=GRS80 +towgs84=0,0,0"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(PJD_WGS84, pj->datum_type);
  }
  // Non-WGS84/GRS80 or non-zero shift remains PJD_3PARAM.
  {
    ScopedPj pj(pj_create_internal(ctx.get(),
                                   "+proj=merc +ellps=bessel +towgs84=0,0,0"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(PJD_3PARAM, pj->datum_type);
  }
  {
    ScopedPj pj(pj_create_internal(ctx.get(),
                                   "+proj=merc +ellps=WGS84 +towgs84=1,0,0"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(PJD_3PARAM, pj->datum_type);
  }

  // +geoc applies on ellipsoidal CRSs and stays disabled on spheres.
  {
    ScopedPj pj_ell(
        pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84 +geoc"));
    ASSERT_NE(nullptr, pj_ell.get());
    EXPECT_NE(0, pj_ell->geoc);

    ScopedPj pj_sph(
        pj_create_internal(ctx.get(), "+proj=merc +R=6371000 +geoc"));
    ASSERT_NE(nullptr, pj_sph.get());
    EXPECT_EQ(0, pj_sph->geoc);
  }

  // +over and ctx->forceOver both set P->over.
  {
    ScopedPj pj_over(
        pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84 +over"));
    ASSERT_NE(nullptr, pj_over.get());
    EXPECT_NE(0, pj_over->over);

    ctx->forceOver = true;
    absl::Cleanup reset_force_over = [&ctx] { ctx->forceOver = false; };
    ScopedPj pj_forced(
        pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_forced.get());
    EXPECT_NE(0, pj_forced->over);
  }

  // +geoidgrids marks parameter used in init.cpp and applies vgridshift.
  const float shifts[] = {2.0f, 2.0f, 2.0f, 2.0f};
  fs.AddFile("/mem/geoid.gtx",
             BuildGtxGrid(0.0, 0.0, 10.0, 10.0, 2, 2, shifts));

  ScopedPj pj_geoid(pj_create_internal(
      ctx.get(), "+proj=merc +ellps=WGS84 +geoidgrids=/mem/geoid.gtx"));
  ASSERT_NE(nullptr, pj_geoid.get());
  EXPECT_NE(0, pj_geoid->has_geoid_vgrids);

  const PJ_COORD input =
      proj_coord(proj_torad(5.0), proj_torad(5.0), 10.0, 0.0);
  const PJ_COORD fwd = proj_trans(pj_geoid.get(), PJ_FWD, input);
  EXPECT_EQ(0, proj_errno(pj_geoid.get()));
  EXPECT_NE(10.0, fwd.xyz.z);

  const PJ_COORD inv = proj_trans(pj_geoid.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj_geoid.get()));
  EXPECT_NEAR(input.lpz.lam, inv.lpz.lam, 1e-10);
  EXPECT_NEAR(input.lpz.phi, inv.lpz.phi, 1e-10);
  EXPECT_NEAR(10.0, inv.lpz.z, 1e-6);
}

TEST(InitTest, ParameterValidationLonWrapAxisOffsetsScaleUnitsAndPm) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // +lon_wrap valid and out-of-range (>= 10 * 360 deg).
  {
    ScopedPj pj(
        pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84 +lon_wrap=180"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_NE(0, pj->is_long_wrap_set);
    EXPECT_NEAR(std::numbers::pi, pj->long_wrap_center, 1e-12);
  }
  EXPECT_EQ(nullptr, pj_create_internal(
                         ctx.get(), "+proj=merc +ellps=WGS84 +lon_wrap=4000"));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));

  // +axis valid and invalid length/characters.
  {
    ScopedPj pj(
        pj_create_internal(ctx.get(), "+proj=merc +ellps=WGS84 +axis=neu"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_STREQ("neu", pj->axis);
  }
  constexpr const char* kBadAxes[] = {"ne", "enuu", "xnu", "exu", "enx"};
  for (const char* axis : kBadAxes) {
    const std::string def =
        std::string("+proj=merc +ellps=WGS84 +axis=") + axis;
    EXPECT_EQ(nullptr, pj_create_internal(ctx.get(), def.c_str())) << axis;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << axis;
  }

  // +lat_0 bounds, +lon_0, false offsets, +k_0/+k.
  EXPECT_EQ(nullptr, pj_create_internal(ctx.get(),
                                        "+proj=tmerc +lat_0=91 +ellps=WGS84"));
  EXPECT_EQ(nullptr, pj_create_internal(ctx.get(),
                                        "+proj=tmerc +lat_0=-91 +ellps=WGS84"));
  {
    ScopedPj pj(pj_create_internal(
        ctx.get(),
        "+proj=tmerc +lat_0=45 +lon_0=-90 +x_0=100 +y_0=200 +z_0=5 +t_0=2020 "
        "+k_0=0.9996 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_NEAR(proj_torad(45.0), pj->phi0, 1e-12);
    EXPECT_NEAR(proj_torad(-90.0), pj->lam0, 1e-12);
    EXPECT_DOUBLE_EQ(100.0, pj->x0);
    EXPECT_DOUBLE_EQ(200.0, pj->y0);
    EXPECT_DOUBLE_EQ(5.0, pj->z0);
    EXPECT_DOUBLE_EQ(2020.0, pj->t0);
    EXPECT_DOUBLE_EQ(0.9996, pj->k0);
  }
  {
    ScopedPj pj(
        pj_create_internal(ctx.get(), "+proj=tmerc +k=0.9995 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_DOUBLE_EQ(0.9995, pj->k0);
  }
  EXPECT_EQ(nullptr,
            pj_create_internal(ctx.get(), "+proj=tmerc +k_0=0 +ellps=WGS84"));
  EXPECT_EQ(nullptr,
            pj_create_internal(ctx.get(), "+proj=tmerc +k=-0.5 +ellps=WGS84"));

  // Horizontal units and +to_meter (scalar and numerator/denominator ratio).
  {
    ScopedPj pj_km(
        pj_create_internal(ctx.get(), "+proj=merc +units=km +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_km.get());
    EXPECT_DOUBLE_EQ(1000.0, pj_km->to_meter);
    EXPECT_NEAR(0.001, pj_km->fr_meter, 1e-12);
    EXPECT_DOUBLE_EQ(1000.0, pj_km->vto_meter);
    EXPECT_NEAR(0.001, pj_km->vfr_meter, 1e-12);

    ScopedPj pj_dm(
        pj_create_internal(ctx.get(), "+proj=merc +units=dm +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_dm.get());
    EXPECT_NEAR(0.1, pj_dm->to_meter, 1e-12);
    EXPECT_NEAR(10.0, pj_dm->fr_meter, 1e-12);

    ScopedPj pj_scalar(
        pj_create_internal(ctx.get(), "+proj=merc +to_meter=2.0 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_scalar.get());
    EXPECT_DOUBLE_EQ(2.0, pj_scalar->to_meter);
    EXPECT_DOUBLE_EQ(0.5, pj_scalar->fr_meter);

    ScopedPj pj_ratio(pj_create_internal(
        ctx.get(), "+proj=merc +to_meter=1200/3937 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_ratio.get());
    EXPECT_NEAR(1200.0 / 3937.0, pj_ratio->to_meter, 1e-12);
    EXPECT_NEAR(3937.0 / 1200.0, pj_ratio->fr_meter, 1e-12);
  }
  EXPECT_EQ(nullptr, pj_create_internal(
                         ctx.get(), "+proj=merc +units=badunit +ellps=WGS84"));
  EXPECT_EQ(nullptr, pj_create_internal(
                         ctx.get(), "+proj=merc +to_meter=1/0 +ellps=WGS84"));
  EXPECT_EQ(nullptr, pj_create_internal(
                         ctx.get(), "+proj=merc +to_meter=-2 +ellps=WGS84"));

  // Vertical units and +vto_meter (scalar and ratio).
  {
    ScopedPj pj_cm(
        pj_create_internal(ctx.get(), "+proj=merc +vunits=cm +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_cm.get());
    EXPECT_DOUBLE_EQ(1.0, pj_cm->to_meter);
    EXPECT_NEAR(0.01, pj_cm->vto_meter, 1e-12);
    EXPECT_NEAR(100.0, pj_cm->vfr_meter, 1e-12);

    ScopedPj pj_vscalar(pj_create_internal(
        ctx.get(), "+proj=merc +vto_meter=2.0 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_vscalar.get());
    EXPECT_DOUBLE_EQ(2.0, pj_vscalar->vto_meter);
    EXPECT_DOUBLE_EQ(0.5, pj_vscalar->vfr_meter);

    ScopedPj pj_vratio(pj_create_internal(
        ctx.get(), "+proj=merc +vto_meter=1/2 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_vratio.get());
    EXPECT_DOUBLE_EQ(0.5, pj_vratio->vto_meter);
    EXPECT_DOUBLE_EQ(2.0, pj_vratio->vfr_meter);
  }
  EXPECT_EQ(nullptr, pj_create_internal(
                         ctx.get(), "+proj=merc +vunits=badv +ellps=WGS84"));
  EXPECT_EQ(nullptr, pj_create_internal(
                         ctx.get(), "+proj=merc +vto_meter=1/0 +ellps=WGS84"));
  EXPECT_EQ(nullptr, pj_create_internal(
                         ctx.get(), "+proj=merc +vto_meter=-1 +ellps=WGS84"));

  // Prime meridians: named, numeric 0, DMS/numeric non-zero, and invalid.
  {
    ScopedPj pj_paris(
        pj_create_internal(ctx.get(), "+proj=merc +pm=paris +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_paris.get());
    EXPECT_NEAR(proj_torad(2.0 + 20.0 / 60.0 + 14.025 / 3600.0),
                pj_paris->from_greenwich, 1e-12);

    ScopedPj pj_zero(
        pj_create_internal(ctx.get(), "+proj=merc +pm=0 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_zero.get());
    EXPECT_DOUBLE_EQ(0.0, pj_zero->from_greenwich);

    ScopedPj pj_dms(
        pj_create_internal(ctx.get(), "+proj=merc +pm=5dW +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_dms.get());
    EXPECT_NEAR(proj_torad(-5.0), pj_dms->from_greenwich, 1e-12);

    ScopedPj pj_num(
        pj_create_internal(ctx.get(), "+proj=merc +pm=5.5 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj_num.get());
    EXPECT_NEAR(proj_torad(5.5), pj_num->from_greenwich, 1e-12);
  }
  EXPECT_EQ(nullptr, pj_create_internal(
                         ctx.get(), "+proj=merc +pm=atlantis +ellps=WGS84"));
  EXPECT_EQ(nullptr,
            pj_create_internal(ctx.get(), "+proj=merc +pm=0bad +ellps=WGS84"));
  EXPECT_EQ(nullptr, pj_create_internal(ctx.get(),
                                        "+proj=merc +pm=5.5bad +ellps=WGS84"));
}

TEST(InitTest, ConstructorFailuresArgvAndProjectionTransforms) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // Null argv rejected cleanly.
  EXPECT_EQ(nullptr, pj_create_argv_internal(ctx.get(), 1, nullptr));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));

  // Constructor returning nullptr directly.
  EXPECT_EQ(nullptr,
            pj_create_internal(ctx.get(), "+proj=utm +zone=99 +ellps=WGS84"));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));

  // Invalid boolean for +south (with explicit +algo=poder_engsager so
  // getAlgoFromParams does not clear ctx->last_errno) sets context errno while
  // utm setup still returns non-null PIN, hitting the proj_errno(PIN)
  // destruction branch.
  ScopedPj bad_south(InitViaArgv(ctx.get(),
                                 {"+proj=utm", "+zone=32", "+south=invalid",
                                  "+algo=poder_engsager", "+ellps=WGS84"},
                                 0));
  EXPECT_EQ(nullptr, bad_south.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));

  // Public proj_create_argv with mutable char buffers creates an ellipsoidal
  // projection supporting 2D/3D/4D roundtrips and projection factors.
  char arg0[] = "+proj=merc";
  char arg1[] = "+ellps=WGS84";
  char arg2[] = "+lon_0=10";
  char* argv[] = {arg0, arg1, arg2};
  ScopedPj merc_ell(proj_create_argv(ctx.get(), 3, argv));
  ASSERT_NE(nullptr, merc_ell.get());
  EXPECT_EQ(0, proj_errno(merc_ell.get()));

  const PJ_COORD lpzt =
      proj_coord(proj_torad(12.0), proj_torad(45.0), 123.5, 2020.25);
  const PJ_COORD xyzt = proj_trans(merc_ell.get(), PJ_FWD, lpzt);
  EXPECT_EQ(0, proj_errno(merc_ell.get()));
  EXPECT_TRUE(std::isfinite(xyzt.xyzt.x));
  EXPECT_TRUE(std::isfinite(xyzt.xyzt.y));
  EXPECT_DOUBLE_EQ(123.5, xyzt.xyzt.z);
  EXPECT_DOUBLE_EQ(2020.25, xyzt.xyzt.t);

  const PJ_COORD rt = proj_trans(merc_ell.get(), PJ_INV, xyzt);
  EXPECT_EQ(0, proj_errno(merc_ell.get()));
  EXPECT_NEAR(lpzt.lpzt.lam, rt.lpzt.lam, 1e-12);
  EXPECT_NEAR(lpzt.lpzt.phi, rt.lpzt.phi, 1e-12);
  EXPECT_DOUBLE_EQ(123.5, rt.lpzt.z);
  EXPECT_DOUBLE_EQ(2020.25, rt.lpzt.t);

  const PJ_FACTORS factors = proj_factors(merc_ell.get(), lpzt);
  EXPECT_EQ(0, proj_errno(merc_ell.get()));
  EXPECT_GT(factors.meridional_scale, 1.0);
  EXPECT_NEAR(factors.meridional_scale, factors.parallel_scale, 1e-9);

  // Spherical Mercator via proj_create + invalid latitude (> 90 deg) domain
  // error returning HUGE_VAL and setting PROJ_ERR_COORD_TRANSFM_INVALID_COORD.
  ScopedPj merc_sph(proj_create(ctx.get(), "+proj=merc +R=6371000"));
  ASSERT_NE(nullptr, merc_sph.get());
  EXPECT_EQ(0, proj_errno(merc_sph.get()));

  const PJ_COORD bad_lat = proj_coord(0.0, proj_torad(91.0), 0.0, 0.0);
  const PJ_COORD bad_out = proj_trans(merc_sph.get(), PJ_FWD, bad_lat);
  EXPECT_EQ(HUGE_VAL, bad_out.xy.x);
  EXPECT_EQ(HUGE_VAL, bad_out.xy.y);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(merc_sph.get()));
  proj_errno_reset(merc_sph.get());
  EXPECT_EQ(0, proj_errno(merc_sph.get()));

  const PJ_COORD valid_sph =
      proj_trans(merc_sph.get(), PJ_FWD,
                 proj_coord(proj_torad(15.0), proj_torad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc_sph.get()));
  EXPECT_TRUE(std::isfinite(valid_sph.xy.x));
  EXPECT_TRUE(std::isfinite(valid_sph.xy.y));
}

}  // namespace
}  // namespace proj
