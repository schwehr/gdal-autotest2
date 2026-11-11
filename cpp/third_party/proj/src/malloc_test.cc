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
// Unit tests for PROJ memory management, PJ allocation/destruction, state
// copying, and global cache cleanup (proj/src/malloc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/malloc.cpp

#include <array>
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

constexpr double kPi = std::numbers::pi;
constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(MallocTest, PjStrdupAndFreeParamsHandleEmptyMultiAndErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. pj_strdup duplicates empty, short, and long NUL-terminated strings.
  ScopedMalloc<char> dup_empty(pj_strdup(""));
  ASSERT_NE(nullptr, dup_empty.get());
  EXPECT_STREQ("", dup_empty.get());

  const std::string long_input(300, 'm');
  ScopedMalloc<char> dup_long(pj_strdup(long_input.c_str()));
  ASSERT_NE(nullptr, dup_long.get());
  EXPECT_EQ(long_input, dup_long.get());

  // 2. free_params handles nullptr and sets context errno.
  EXPECT_EQ(nullptr, free_params(ctx.get(), nullptr, 0));
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  paralist* head = pj_mkparam("+proj=merc");
  ASSERT_NE(nullptr, head);
  absl::Cleanup free_head_on_abort = [ctx_ptr = ctx.get(), &head] {
    if (head != nullptr) {
      free_params(ctx_ptr, head, 0);
    }
  };
  head->next = pj_mkparam("+ellps=WGS84");
  ASSERT_NE(nullptr, head->next);
  paralist* const raw_head = head;
  head = nullptr;
  EXPECT_EQ(nullptr, free_params(ctx.get(), raw_head,
                                 PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);
}

TEST(MallocTest, PjNewCopyStateFromAndProjDestroyBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. proj_destroy(nullptr) and PJ with null destructor are safe no-ops.
  EXPECT_EQ(nullptr, proj_destroy(nullptr));

  ScopedPj src(pj_new());
  ASSERT_NE(nullptr, src.get());
  src->ctx = ctx.get();
  EXPECT_EQ(pj_default_destructor, src->destructor);

  src->destructor = nullptr;
  EXPECT_EQ(nullptr, proj_destroy(src.get()));
  src->destructor = pj_default_destructor;

  // 2. copyStateFrom propagates all transformation policy flags.
  src->over = 1;
  src->errorIfBestTransformationNotAvailable = true;
  src->warnIfBestTransformationNotAvailable = true;
  src->skipNonInstantiable = false;

  ScopedPj dst(pj_new());
  ASSERT_NE(nullptr, dst.get());
  dst->ctx = ctx.get();
  dst->copyStateFrom(*src);
  EXPECT_EQ(1, dst->over);
  EXPECT_TRUE(dst->errorIfBestTransformationNotAvailable);
  EXPECT_TRUE(dst->warnIfBestTransformationNotAvailable);
  EXPECT_FALSE(dst->skipNonInstantiable);
}

TEST(MallocTest, DefaultDestructorHandlesNullErrorAndPopulatedPjFields) {
  InMemoryFileSystem fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  absl::Cleanup cleanup_all = [] {
    proj_errno_reset(nullptr);
    proj_cleanup();
  };

  // 1. Null PJ* with zero vs non-zero errlev updates default context safely.
  EXPECT_EQ(nullptr, pj_default_destructor(nullptr, 0));
  EXPECT_EQ(0, proj_context_errno(nullptr));
  EXPECT_EQ(nullptr, pj_default_destructor(nullptr, PROJ_ERR_OTHER));
  EXPECT_EQ(PROJ_ERR_OTHER, proj_context_errno(nullptr));
  proj_errno_reset(nullptr);

  // 2. Non-null PJ* with non-zero errlev updates P->ctx->last_errno.
  ScopedPj err_pj(pj_new());
  ASSERT_NE(nullptr, err_pj.get());
  err_pj->ctx = ctx.get();
  EXPECT_EQ(nullptr,
            pj_default_destructor(err_pj.release(), PROJ_ERR_OTHER_API_MISUSE));
  EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);

  // 3. Populated PJ instances teardown def_size, def_shape,
  // def_spherification, def_ellps, geod, def_full, axisswap, helmert, cart,
  // cart_wgs84, hgridshift, vgridshift, and cached_op_for_proj_factors.
  ASSERT_TRUE(fs.Install(ctx.get()));
  constexpr std::array<float, 4> kVShifts = {1.0f, 1.0f, 1.0f, 1.0f};
  fs.AddFile("/mem/malloc_v.gtx",
             BuildGtxGrid(/*west_deg=*/5.0, /*south_deg=*/45.0,
                          /*res_x_deg=*/1.0, /*res_y_deg=*/1.0, /*rows=*/2,
                          /*cols=*/2, kVShifts));
  constexpr std::array<float, 8> kHShifts = {0.0f, 0.0f, 0.0f, 0.0f,
                                             0.0f, 0.0f, 0.0f, 0.0f};
  fs.AddFile("/mem/malloc_h.ct2",
             BuildCTable2Grid(/*west_rad=*/DegToRad(5.0),
                              /*south_rad=*/DegToRad(45.0),
                              /*res_x_rad=*/DegToRad(1.0),
                              /*res_y_rad=*/DegToRad(1.0), /*cols=*/2,
                              /*rows=*/2, kHShifts));

  ScopedPj full_v(
      proj_create(ctx.get(),
                  "+proj=tmerc +ellps=intl +a=6378388 +rf=297 +R_A +axis=neu "
                  "+towgs84=1,2,3 +geoidgrids=/mem/malloc_v.gtx"));
  ASSERT_NE(nullptr, full_v.get());
  (void)proj_pj_info(full_v.get());  // Populates def_full for teardown.
  EXPECT_NE(nullptr, full_v->def_size);
  EXPECT_NE(nullptr, full_v->def_shape);
  EXPECT_NE(nullptr, full_v->def_spherification);
  EXPECT_NE(nullptr, full_v->def_ellps);
  EXPECT_NE(nullptr, full_v->geod);
  EXPECT_NE(nullptr, full_v->params);
  EXPECT_NE(nullptr, full_v->def_full);
  EXPECT_NE(nullptr, full_v->axisswap);
  EXPECT_NE(nullptr, full_v->helmert);
  EXPECT_NE(nullptr, full_v->cart);
  EXPECT_NE(nullptr, full_v->cart_wgs84);
  EXPECT_NE(nullptr, full_v->vgridshift);
  EXPECT_NE(nullptr, full_v->opaque);
  full_v.reset();

  ScopedPj full_h(proj_create(
      ctx.get(), "+proj=merc +ellps=WGS84 +nadgrids=/mem/malloc_h.ct2"));
  ASSERT_NE(nullptr, full_h.get());
  EXPECT_NE(nullptr, full_h->hgridshift);
  full_h.reset();

  ScopedPj crs_factors(
      proj_create(ctx.get(), "+proj=utm +zone=32 +datum=WGS84 +type=crs"));
  ASSERT_NE(nullptr, crs_factors.get());
  const PJ_FACTORS f = proj_factors(
      crs_factors.get(), proj_coord(DegToRad(9.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(crs_factors.get()));
  EXPECT_GT(f.parallel_scale, 0.0);
  EXPECT_GT(f.meridional_scale, 0.0);
  EXPECT_NE(nullptr, crs_factors->cached_op_for_proj_factors);
  crs_factors.reset();
}

TEST(MallocTest, ProjCleanupClearsGlobalStateWithAndWithoutCppContext) {
  absl::Cleanup cleanup_default = [] { proj_cleanup(); };

  // 1. Clean up prior to initializing cpp_context on default_ctx (or
  // idempotently if already initialized).
  PJ_CONTEXT* default_ctx = pj_get_default_ctx();
  default_ctx->iniFileLoaded = true;
  proj_cleanup();
  EXPECT_FALSE(default_ctx->iniFileLoaded);

  // 2. Ensure cpp_context != nullptr branch is exercised as well.
  ASSERT_NE(nullptr, default_ctx->get_cpp_context());
  default_ctx->iniFileLoaded = true;
  proj_cleanup();
  EXPECT_FALSE(default_ctx->iniFileLoaded);
}

}  // namespace
}  // namespace proj
