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
// Unit tests for horizontal grid shifts (+proj=hgridshift) in
// proj/src/transformations/hgridshift.cpp.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/hgridshift.cpp

#include <array>
#include <cmath>
#include <numbers>

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

TEST(HgridshiftTest, ParameterValidationErrorsAndDestructorNullGuard) {
  absl::Cleanup clear_cache = [] { pj_clear_hgridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);

  // 1. Missing +grids parameter returns PROJ_ERR_INVALID_OP_MISSING_ARG.
  ScopedPj missing_grids(proj_create(ctx.get(), "+proj=hgridshift"));
  EXPECT_EQ(nullptr, missing_grids.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("+grids parameter missing."));

  // 2. Non-existent required grid returns
  //    PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID.
  logs.Clear();
  proj_context_errno_set(ctx.get(), 0);
  ScopedPj missing_file(
      proj_create(ctx.get(), "+proj=hgridshift +grids=/mem/missing.ct2"));
  EXPECT_EQ(nullptr, missing_file.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("could not find required grid(s)."));

  // 3. Verify destructor nullptr guard.
  ScopedPj valid_null(proj_create(ctx.get(), "+proj=hgridshift +grids=null"));
  ASSERT_NE(nullptr, valid_null.get());
  EXPECT_EQ(nullptr, valid_null->destructor(nullptr, 0));
}

TEST(HgridshiftTest, ForwardAndInverseRoundTripKnownGridsCacheAndReassign) {
  absl::Cleanup clear_cache = [] { pj_clear_hgridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogSuppressor silence(ctx.get());

  // 2x2 CTable2 grid covering [0, 10] deg lon x [0, 10] deg lat with uniform
  // west-positive lon shift = -0.001 rad (+0.001 rad east) and
  // lat shift = +0.002 rad north.
  constexpr float kLonShiftWestPos = -0.001f;
  constexpr float kLatShift = 0.002f;
  constexpr auto kShifts = std::to_array<float>({
      kLonShiftWestPos,
      kLatShift,
      kLonShiftWestPos,
      kLatShift,
      kLonShiftWestPos,
      kLatShift,
      kLonShiftWestPos,
      kLatShift,
  });
  fs.AddFile("/mem/hshift.ct2",
             BuildCTable2Grid(/*west_rad=*/0.0, /*south_rad=*/0.0,
                              /*res_x_rad=*/DegToRad(10.0),
                              /*res_y_rad=*/DegToRad(10.0),
                              /*cols=*/2, /*rows=*/2, kShifts));

  // 1. First creation loads grid immediately and populates
  //    gKnownGridsHGridShift.
  ScopedPj pj1(
      proj_create(ctx.get(), "+proj=hgridshift +grids=/mem/hshift.ct2"));
  ASSERT_NE(nullptr, pj1.get());

  // Exercise pj_hgridshift_reassign_context on an open grid list.
  ScopedContext ctx2 = MakeContext();
  ASSERT_NE(nullptr, ctx2.get());
  ASSERT_TRUE(fs.Install(ctx2.get()));
  proj_assign_context(pj1.get(), ctx2.get());
  proj_assign_context(pj1.get(), ctx.get());

  const PJ_COORD in = proj_coord(DegToRad(5.0), DegToRad(5.0), 123.0, 0.0);
  const PJ_COORD fwd1 = proj_trans(pj1.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj1.get()));
  EXPECT_NEAR(DegToRad(5.0) + 0.001, fwd1.lpz.lam, 1e-8);
  EXPECT_NEAR(DegToRad(5.0) + 0.002, fwd1.lpz.phi, 1e-8);
  EXPECT_DOUBLE_EQ(123.0, fwd1.lpz.z);

  const PJ_COORD inv1 = proj_trans(pj1.get(), PJ_INV, fwd1);
  EXPECT_EQ(0, proj_errno(pj1.get()));
  EXPECT_NEAR(in.lpz.lam, inv1.lpz.lam, 1e-9);
  EXPECT_NEAR(in.lpz.phi, inv1.lpz.phi, 1e-9);
  EXPECT_DOUBLE_EQ(123.0, inv1.lpz.z);

  // 2. Second creation with the same +grids string hits isKnownGrid == true
  //    (setting defer_grid_opening = true) and opens on first transform.
  ScopedPj pj2(
      proj_create(ctx.get(), "+proj=hgridshift +grids=/mem/hshift.ct2"));
  ASSERT_NE(nullptr, pj2.get());
  const PJ_COORD fwd2 = proj_trans(pj2.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj2.get()));
  EXPECT_NEAR(fwd1.lpz.lam, fwd2.lpz.lam, 1e-12);
  EXPECT_NEAR(fwd1.lpz.phi, fwd2.lpz.phi, 1e-12);

  // 3. Out-of-grid coordinate fails with PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID.
  const PJ_COORD out_of_bounds = proj_trans(
      pj1.get(), PJ_FWD, proj_coord(DegToRad(50.0), DegToRad(50.0), 0.0, 0.0));
  EXPECT_TRUE(std::isinf(out_of_bounds.lpz.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(pj1.get()));
}

TEST(HgridshiftTest, OptionalMissingGridAndTimeRestricted4DBrackets) {
  absl::Cleanup clear_cache = [] { pj_clear_hgridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 1. Optional missing grid (@/mem/opt_missing.ct2) leaves Q->grids empty and
  //    passes coordinates through unchanged in both forward and reverse.
  ScopedPj pj_opt(
      proj_create(ctx.get(), "+proj=hgridshift +grids=@/mem/opt_missing.ct2"));
  ASSERT_NE(nullptr, pj_opt.get());

  const PJ_COORD in = proj_coord(DegToRad(5.0), DegToRad(6.0), 10.0, 0.0);
  const PJ_COORD opt_fwd = proj_trans(pj_opt.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj_opt.get()));
  EXPECT_DOUBLE_EQ(in.lpz.lam, opt_fwd.lpz.lam);
  EXPECT_DOUBLE_EQ(in.lpz.phi, opt_fwd.lpz.phi);

  const PJ_COORD opt_inv = proj_trans(pj_opt.get(), PJ_INV, in);
  EXPECT_EQ(0, proj_errno(pj_opt.get()));
  EXPECT_DOUBLE_EQ(in.lpz.lam, opt_inv.lpz.lam);
  EXPECT_DOUBLE_EQ(in.lpz.phi, opt_inv.lpz.phi);

  // 2. Time-restricted 4D transformation (+t_epoch=2010 +t_final=2020):
  //    Applied when t < t_epoch && t_final > t_epoch (e.g. t = 2005.0),
  //    skipped when t >= t_epoch (e.g. t = 2015.0).
  constexpr auto kShifts = std::to_array<float>({
      -0.001f,
      0.002f,
      -0.001f,
      0.002f,
      -0.001f,
      0.002f,
      -0.001f,
      0.002f,
  });
  fs.AddFile("/mem/time_hshift.ct2",
             BuildCTable2Grid(0.0, 0.0, DegToRad(10.0), DegToRad(10.0), 2, 2,
                              kShifts));

  ScopedPj pj_time(
      proj_create(ctx.get(),
                  "+proj=hgridshift +grids=/mem/time_hshift.ct2 +t_epoch=2010 "
                  "+t_final=2020"));
  ASSERT_NE(nullptr, pj_time.get());

  // Inside time bracket (t = 2005.0 < 2010.0): shift is applied.
  const PJ_COORD active_in =
      proj_coord(DegToRad(5.0), DegToRad(5.0), 0.0, 2005.0);
  const PJ_COORD active_fwd = proj_trans(pj_time.get(), PJ_FWD, active_in);
  EXPECT_EQ(0, proj_errno(pj_time.get()));
  EXPECT_NEAR(DegToRad(5.0) + 0.001, active_fwd.lpzt.lam, 1e-8);
  EXPECT_NEAR(DegToRad(5.0) + 0.002, active_fwd.lpzt.phi, 1e-8);

  const PJ_COORD active_inv = proj_trans(pj_time.get(), PJ_INV, active_fwd);
  EXPECT_EQ(0, proj_errno(pj_time.get()));
  EXPECT_NEAR(DegToRad(5.0), active_inv.lpzt.lam, 1e-9);
  EXPECT_NEAR(DegToRad(5.0), active_inv.lpzt.phi, 1e-9);

  // Outside time bracket (t = 2015.0 >= 2010.0): coordinate is unchanged.
  const PJ_COORD inactive_in =
      proj_coord(DegToRad(5.0), DegToRad(5.0), 0.0, 2015.0);
  const PJ_COORD inactive_fwd = proj_trans(pj_time.get(), PJ_FWD, inactive_in);
  EXPECT_EQ(0, proj_errno(pj_time.get()));
  EXPECT_DOUBLE_EQ(inactive_in.lpzt.lam, inactive_fwd.lpzt.lam);
  EXPECT_DOUBLE_EQ(inactive_in.lpzt.phi, inactive_fwd.lpzt.phi);

  const PJ_COORD inactive_inv = proj_trans(pj_time.get(), PJ_INV, inactive_in);
  EXPECT_EQ(0, proj_errno(pj_time.get()));
  EXPECT_DOUBLE_EQ(inactive_in.lpzt.lam, inactive_inv.lpzt.lam);
  EXPECT_DOUBLE_EQ(inactive_in.lpzt.phi, inactive_inv.lpzt.phi);
}

TEST(HgridshiftTest, DeferredGridOpeningInForwardAndReverseSuccessAndError) {
  absl::Cleanup clear_cache = [] { pj_clear_hgridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogSuppressor silence(ctx.get());

  ctx->defer_grid_opening = true;

  constexpr auto kShifts = std::to_array<float>({
      -0.001f,
      0.002f,
      -0.001f,
      0.002f,
      -0.001f,
      0.002f,
      -0.001f,
      0.002f,
  });
  fs.AddFile("/mem/def_hshift.ct2",
             BuildCTable2Grid(0.0, 0.0, DegToRad(10.0), DegToRad(10.0), 2, 2,
                              kShifts));

  const PJ_COORD pt = proj_coord(DegToRad(5.0), DegToRad(5.0), 0.0, 0.0);

  // 1. Deferred opening triggered via reverse_3d first.
  {
    ScopedPj pj_inv_first(
        proj_create(ctx.get(), "+proj=hgridshift +grids=/mem/def_hshift.ct2"));
    ASSERT_NE(nullptr, pj_inv_first.get());
    const PJ_COORD inv = proj_trans(pj_inv_first.get(), PJ_INV, pt);
    EXPECT_EQ(0, proj_errno(pj_inv_first.get()));
    EXPECT_NEAR(DegToRad(5.0) - 0.001, inv.lpz.lam, 1e-8);
    EXPECT_NEAR(DegToRad(5.0) - 0.002, inv.lpz.phi, 1e-8);
  }

  // 2. Deferred opening error in forward_3d (initial failure + cached replay).
  {
    ScopedPj pj_err_fwd(proj_create(
        ctx.get(), "+proj=hgridshift +grids=/mem/missing_def_fwd.ct2"));
    ASSERT_NE(nullptr, pj_err_fwd.get());

    const PJ_COORD err1 = proj_trans(pj_err_fwd.get(), PJ_FWD, pt);
    EXPECT_TRUE(std::isinf(err1.lpz.lam));
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_errno(pj_err_fwd.get()));

    proj_errno_reset(pj_err_fwd.get());
    const PJ_COORD err2 = proj_trans(pj_err_fwd.get(), PJ_FWD, pt);
    EXPECT_TRUE(std::isinf(err2.lpz.lam));
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_errno(pj_err_fwd.get()));
  }

  // 3. Deferred opening error in reverse_3d (initial failure + cached replay).
  {
    ScopedPj pj_err_inv(proj_create(
        ctx.get(), "+proj=hgridshift +grids=/mem/missing_def_inv.ct2"));
    ASSERT_NE(nullptr, pj_err_inv.get());

    const PJ_COORD err1 = proj_trans(pj_err_inv.get(), PJ_INV, pt);
    EXPECT_TRUE(std::isinf(err1.lpz.lam));
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_errno(pj_err_inv.get()));

    proj_errno_reset(pj_err_inv.get());
    const PJ_COORD err2 = proj_trans(pj_err_inv.get(), PJ_INV, pt);
    EXPECT_TRUE(std::isinf(err2.lpz.lam));
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_errno(pj_err_inv.get()));
  }
}

}  // namespace
}  // namespace proj
