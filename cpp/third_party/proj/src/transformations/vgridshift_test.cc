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
// Unit tests for vertical grid shifts (+proj=vgridshift) in
// proj/src/transformations/vgridshift.cpp.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/vgridshift.cpp

#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <span>  // NOLINT(build/c++20)
#include <vector>

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

TEST(VgridshiftTest, ParameterValidationErrorsAndDestructorNullGuard) {
  absl::Cleanup clear_cache = [] { pj_clear_vgridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);

  // 1. Missing +grids parameter returns PROJ_ERR_INVALID_OP_MISSING_ARG.
  ScopedPj missing_grids(proj_create(ctx.get(), "+proj=vgridshift"));
  EXPECT_EQ(nullptr, missing_grids.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("+grids parameter missing."));

  // 2. Non-existent required grid returns
  //    PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID.
  logs.Clear();
  proj_context_errno_set(ctx.get(), 0);
  ScopedPj missing_file(
      proj_create(ctx.get(), "+proj=vgridshift +grids=/mem/missing.gtx"));
  EXPECT_EQ(nullptr, missing_file.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("could not find required grid(s)."));

  // 3. Verify destructor nullptr guard.
  ScopedPj valid_null(proj_create(ctx.get(), "+proj=vgridshift +grids=null"));
  ASSERT_NE(nullptr, valid_null.get());
  EXPECT_EQ(nullptr, valid_null->destructor(nullptr, 0));
}

TEST(VgridshiftTest, ForwardAndInverse3D4DWithMultiplierKnownGridsAndReassign) {
  absl::Cleanup clear_cache = [] { pj_clear_vgridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogSuppressor silence(ctx.get());

  // 2x2 GTX grid covering [0, 10] deg lon x [0, 10] deg lat with uniform
  // +2.5 m geoid undulation.
  constexpr auto kShifts = std::to_array<float>({2.5f, 2.5f, 2.5f, 2.5f});
  fs.AddFile("/mem/geoid.gtx",
             BuildGtxGrid(/*west_deg=*/0.0, /*south_deg=*/0.0,
                          /*res_x_deg=*/10.0, /*res_y_deg=*/10.0,
                          /*rows=*/2, /*cols=*/2, kShifts));

  // 1. Default multiplier (-1.0): forward subtracts 2.5 m, inverse adds 2.5 m.
  ScopedPj pj1(
      proj_create(ctx.get(), "+proj=vgridshift +grids=/mem/geoid.gtx"));
  ASSERT_NE(nullptr, pj1.get());

  // Reassign context on an open grid list.
  ScopedContext ctx2 = MakeContext();
  ASSERT_NE(nullptr, ctx2.get());
  ASSERT_TRUE(fs.Install(ctx2.get()));
  proj_assign_context(pj1.get(), ctx2.get());
  proj_assign_context(pj1.get(), ctx.get());

  const PJ_COORD in = proj_coord(DegToRad(5.0), DegToRad(5.0), 100.0, 0.0);
  const PJ_COORD fwd1 = proj_trans(pj1.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj1.get()));
  EXPECT_NEAR(DegToRad(5.0), fwd1.lpz.lam, 1e-12);
  EXPECT_NEAR(DegToRad(5.0), fwd1.lpz.phi, 1e-12);
  EXPECT_NEAR(97.5, fwd1.lpz.z, 1e-6);

  const PJ_COORD inv1 = proj_trans(pj1.get(), PJ_INV, fwd1);
  EXPECT_EQ(0, proj_errno(pj1.get()));
  EXPECT_NEAR(100.0, inv1.lpz.z, 1e-6);

  // 2. Second creation with the same +grids string hits gKnownGridsVGridShift
  //    (setting defer_grid_opening = true) and custom +multiplier=2.0.
  ScopedPj pj2(proj_create(
      ctx.get(), "+proj=vgridshift +grids=/mem/geoid.gtx +multiplier=2.0"));
  ASSERT_NE(nullptr, pj2.get());
  const PJ_COORD fwd2 = proj_trans(pj2.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj2.get()));
  EXPECT_NEAR(105.0, fwd2.lpz.z, 1e-6);

  // 3. Out-of-grid coordinate fails with PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID.
  const PJ_COORD out_of_bounds =
      proj_trans(pj1.get(), PJ_FWD,
                 proj_coord(DegToRad(50.0), DegToRad(50.0), 100.0, 0.0));
  EXPECT_TRUE(std::isinf(out_of_bounds.lpz.z));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(pj1.get()));
}

TEST(VgridshiftTest, OptionalMissingGridAndTimeRestricted4DBrackets) {
  absl::Cleanup clear_cache = [] { pj_clear_vgridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 1. Optional missing grid (@/mem/opt_missing.gtx) leaves Q->grids empty and
  //    passes coordinates through unchanged in both forward and reverse.
  ScopedPj pj_opt(
      proj_create(ctx.get(), "+proj=vgridshift +grids=@/mem/opt_missing.gtx"));
  ASSERT_NE(nullptr, pj_opt.get());

  const PJ_COORD in = proj_coord(DegToRad(5.0), DegToRad(5.0), 42.0, 0.0);
  const PJ_COORD opt_fwd = proj_trans(pj_opt.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj_opt.get()));
  EXPECT_DOUBLE_EQ(42.0, opt_fwd.lpz.z);

  const PJ_COORD opt_inv = proj_trans(pj_opt.get(), PJ_INV, in);
  EXPECT_EQ(0, proj_errno(pj_opt.get()));
  EXPECT_DOUBLE_EQ(42.0, opt_inv.lpz.z);

  // 2. Time-restricted 4D transformation (+t_epoch=2010 +t_final=2020):
  //    Applied when t < t_epoch && t_final > t_epoch (e.g. t = 2005.0),
  //    skipped when t >= t_epoch (e.g. t = 2015.0).
  constexpr auto kShifts = std::to_array<float>({4.0f, 4.0f, 4.0f, 4.0f});
  fs.AddFile("/mem/time_geoid.gtx",
             BuildGtxGrid(0.0, 0.0, 10.0, 10.0, 2, 2, kShifts));

  ScopedPj pj_time(proj_create(ctx.get(),
                               "+proj=vgridshift +grids=/mem/time_geoid.gtx "
                               "+t_epoch=2010 +t_final=2020"));
  ASSERT_NE(nullptr, pj_time.get());

  // Inside time bracket (t = 2005.0 < 2010.0): shift is applied.
  const PJ_COORD active_in =
      proj_coord(DegToRad(5.0), DegToRad(5.0), 50.0, 2005.0);
  const PJ_COORD active_fwd = proj_trans(pj_time.get(), PJ_FWD, active_in);
  EXPECT_EQ(0, proj_errno(pj_time.get()));
  EXPECT_NEAR(46.0, active_fwd.lpzt.z, 1e-6);

  const PJ_COORD active_inv = proj_trans(pj_time.get(), PJ_INV, active_fwd);
  EXPECT_EQ(0, proj_errno(pj_time.get()));
  EXPECT_NEAR(50.0, active_inv.lpzt.z, 1e-6);

  // Outside time bracket (t = 2015.0 >= 2010.0): coordinate is unchanged.
  const PJ_COORD inactive_in =
      proj_coord(DegToRad(5.0), DegToRad(5.0), 50.0, 2015.0);
  const PJ_COORD inactive_fwd = proj_trans(pj_time.get(), PJ_FWD, inactive_in);
  EXPECT_EQ(0, proj_errno(pj_time.get()));
  EXPECT_DOUBLE_EQ(50.0, inactive_fwd.lpzt.z);

  const PJ_COORD inactive_inv = proj_trans(pj_time.get(), PJ_INV, inactive_in);
  EXPECT_EQ(0, proj_errno(pj_time.get()));
  EXPECT_DOUBLE_EQ(50.0, inactive_inv.lpzt.z);
}

TEST(VgridshiftTest, DeferredGridOpeningInForwardAndReverseAndVertconHack) {
  absl::Cleanup clear_cache = [] { pj_clear_vgridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogSuppressor silence(ctx.get());

  ctx->defer_grid_opening = true;

  constexpr auto kShifts = std::to_array<float>({3.0f, 3.0f, 3.0f, 3.0f});
  fs.AddFile("/mem/def_vgrid.gtx",
             BuildGtxGrid(0.0, 0.0, 10.0, 10.0, 2, 2, kShifts));

  const PJ_COORD pt = proj_coord(DegToRad(5.0), DegToRad(5.0), 20.0, 0.0);

  // 1. Deferred opening triggered via reverse_3d first (with +multiplier=0.001
  //    on a non-vertcon grid name to cover the strcmp mismatch branch in
  //    deal_with_vertcon_gtx_hack).
  {
    ScopedPj pj_inv_first(proj_create(
        ctx.get(),
        "+proj=vgridshift +grids=/mem/def_vgrid.gtx +multiplier=0.001"));
    ASSERT_NE(nullptr, pj_inv_first.get());
    const PJ_COORD inv = proj_trans(pj_inv_first.get(), PJ_INV, pt);
    EXPECT_EQ(0, proj_errno(pj_inv_first.get()));
    EXPECT_NEAR(20.0 - 0.003, inv.lpz.z, 1e-7);
  }

  // 2. Deferred opening error in forward_3d and reverse_3d (both initial open
  //    and subsequent replay of error_code_in_defer_grid_opening, plus
  //    deal_with_vertcon_gtx_hack when Q->grids is empty).
  {
    ScopedPj pj_err_fwd(proj_create(
        ctx.get(), "+proj=vgridshift +grids=vertconw.gtx +multiplier=0.001"));
    ASSERT_NE(nullptr, pj_err_fwd.get());

    const PJ_COORD err1 = proj_trans(pj_err_fwd.get(), PJ_FWD, pt);
    EXPECT_TRUE(std::isinf(err1.lpz.z));
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_errno(pj_err_fwd.get()));

    proj_errno_reset(pj_err_fwd.get());
    const PJ_COORD err2 = proj_trans(pj_err_fwd.get(), PJ_FWD, pt);
    EXPECT_TRUE(std::isinf(err2.lpz.z));
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_errno(pj_err_fwd.get()));
  }
  {
    ScopedPj pj_err_inv(proj_create(
        ctx.get(), "+proj=vgridshift +grids=/mem/missing_def_inv.gtx"));
    ASSERT_NE(nullptr, pj_err_inv.get());

    const PJ_COORD err1 = proj_trans(pj_err_inv.get(), PJ_INV, pt);
    EXPECT_TRUE(std::isinf(err1.lpz.z));
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_errno(pj_err_inv.get()));

    proj_errno_reset(pj_err_inv.get());
    const PJ_COORD err2 = proj_trans(pj_err_inv.get(), PJ_INV, pt);
    EXPECT_TRUE(std::isinf(err2.lpz.z));
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_errno(pj_err_inv.get()));
  }

  // 3. deal_with_vertcon_gtx_hack: when +grids=vertconw.gtx and
  //    +multiplier=0.001 resolves to a .tif file (via file finder), the
  //    multiplier is upgraded from 0.001 to 1.0.
  fs.AddFile("/mem/us_noaa_vertconw.tif",
             BuildMinimalGeoTiff(2, 2, 1, 0.0, 10.0, 10.0, 10.0, kShifts,
                                 /*is_geographic=*/true));
  proj_context_set_file_finder(
      ctx.get(),
      [](PJ_CONTEXT*, const char* file, void*) -> const char* {
        if (file != nullptr && std::strcmp(file, "vertconw.gtx") == 0) {
          return "/mem/us_noaa_vertconw.tif";
        }
        return nullptr;
      },
      nullptr);

  ScopedPj pj_vertcon(proj_create(
      ctx.get(), "+proj=vgridshift +grids=vertconw.gtx +multiplier=0.001"));
  ASSERT_NE(nullptr, pj_vertcon.get());
  const PJ_COORD vertcon_fwd = proj_trans(pj_vertcon.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(pj_vertcon.get()));
  // Multiplier became 1.0 instead of 0.001, so z increases by +3.0 m.
  EXPECT_NEAR(23.0, vertcon_fwd.lpz.z, 1e-6);
}

}  // namespace
}  // namespace proj
