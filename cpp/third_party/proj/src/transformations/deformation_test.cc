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
// Unit tests for kinematic datum shifting via velocity grids
// (proj/src/transformations/deformation.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/deformation.cpp

#include <cmath>
#include <numbers>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(DeformationTest,
     ParameterValidationRejectsMissingDeprecatedOrExclusiveArgs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  constexpr float kCtableShifts[] = {0.0f, 0.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, 0.0f, 0.0f};
  fs.AddFile("/mem/valid.ct2",
             BuildCTable2Grid(DegToRad(-5.0), DegToRad(-5.0), DegToRad(10.0),
                              DegToRad(10.0), 2, 2, kCtableShifts));

  // 1. Neither +grids nor (+xy_grids and +z_grids) provided.
  ScopedPj no_grids(proj_create(ctx.get(), "+proj=deformation +dt=1.0"));
  EXPECT_EQ(nullptr, no_grids.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));

  ScopedPj only_xy(proj_create(
      ctx.get(), "+proj=deformation +xy_grids=/mem/valid.ct2 +dt=1.0"));
  EXPECT_EQ(nullptr, only_xy.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));

  ScopedPj only_z(
      proj_create(ctx.get(), "+proj=deformation +z_grids=null +dt=1.0"));
  EXPECT_EQ(nullptr, only_z.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));

  // 2. Missing files in +grids, +xy_grids, or +z_grids.
  ScopedPj bad_grids(proj_create(
      ctx.get(), "+proj=deformation +grids=/mem/missing.tif +dt=1.0"));
  EXPECT_EQ(nullptr, bad_grids.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));

  ScopedPj bad_xy_grids(proj_create(
      ctx.get(),
      "+proj=deformation +xy_grids=/mem/missing.ct2 +z_grids=null +dt=1.0"));
  EXPECT_EQ(nullptr, bad_xy_grids.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));

  ScopedPj bad_z_grids(proj_create(ctx.get(),
                                   "+proj=deformation +xy_grids=/mem/valid.ct2 "
                                   "+z_grids=/mem/missing.gtx +dt=1.0"));
  EXPECT_EQ(nullptr, bad_z_grids.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));

  // 3. Deprecated +t_obs parameter.
  ScopedPj deprecated_t_obs(
      proj_create(ctx.get(), "+proj=deformation +grids=null +t_obs=2020.0"));
  EXPECT_EQ(nullptr, deprecated_t_obs.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));

  // 4. Neither +dt nor +t_epoch specified.
  ScopedPj no_time_param(
      proj_create(ctx.get(), "+proj=deformation +grids=null"));
  EXPECT_EQ(nullptr, no_time_param.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));

  // 5. Mutually exclusive +dt and +t_epoch both specified.
  ScopedPj both_time_params(proj_create(
      ctx.get(), "+proj=deformation +grids=null +dt=1.0 +t_epoch=2020.0"));
  EXPECT_EQ(nullptr, both_time_params.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MUTUALLY_EXCLUSIVE_ARGS,
            proj_context_errno(ctx.get()));
}

TEST(DeformationTest, NullGridAndDestructorNullSafety) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=deformation +grids=null +dt=5.0 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));

  const PJ_COORD in = proj_coord(6371000.0, 0.0, 0.0, 2025.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.xyzt.x, fwd.xyzt.x, 1e-9);
  EXPECT_NEAR(in.xyzt.y, fwd.xyzt.y, 1e-9);
  EXPECT_NEAR(in.xyzt.z, fwd.xyzt.z, 1e-9);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.xyzt.x, inv.xyzt.x, 1e-9);
}

TEST(DeformationTest, GenericGeoTiffVelocityGrid3DAnd4DForwardAndInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // Permute band descriptions: sample 0 = north_velocity (2000 mm/yr = 2 m/yr),
  // sample 1 = up_velocity (3000 mm/yr = 3 m/yr), sample 2 = east_velocity
  // (1000 mm/yr = 1 m/yr).
  constexpr float kPixels[] = {
      2000.0f, 3000.0f, 1000.0f, 2000.0f, 3000.0f, 1000.0f,
      2000.0f, 3000.0f, 1000.0f, 2000.0f, 3000.0f, 1000.0f,
  };
  constexpr std::string_view kXml =
      "<GDALMetadata>"
      "<Item name=\"DESCRIPTION\" sample=\"0\">north_velocity</Item>"
      "<Item name=\"DESCRIPTION\" sample=\"1\">up_velocity</Item>"
      "<Item name=\"DESCRIPTION\" sample=\"2\">east_velocity</Item>"
      "<Item name=\"UNITTYPE\" sample=\"0\">millimetres per year</Item>"
      "<Item name=\"UNITTYPE\" sample=\"1\">millimetres per year</Item>"
      "<Item name=\"UNITTYPE\" sample=\"2\">millimetres per year</Item>"
      "</GDALMetadata>";
  fs.AddFile("/mem/vel3d.tif",
             BuildMinimalGeoTiff(/*width=*/2, /*height=*/2,
                                 /*samples_per_pixel=*/3, /*west=*/-10.0,
                                 /*north=*/10.0, /*res_x=*/20.0, /*res_y=*/20.0,
                                 kPixels, /*is_geographic=*/true, kXml));

  // 1. Test 4D with +t_epoch=2020.0 at (lon=0, lat=0) on sphere R=6371000.
  //    At (lon=0, lat=0): Up -> +X (+3 m/yr), East -> +Y (+1 m/yr),
  //    North -> +Z (+2 m/yr). For t=2022.0 (dt=2.0): dX=+6, dY=+2, dZ=+4.
  ScopedPj pj_epoch(proj_create(
      ctx.get(),
      "+proj=deformation +grids=/mem/vel3d.tif +t_epoch=2020.0 +R=6371000"));
  ASSERT_NE(nullptr, pj_epoch.get());

  const PJ_COORD in4d = proj_coord(6371000.0, 0.0, 0.0, 2022.0);
  const PJ_COORD fwd4d = proj_trans(pj_epoch.get(), PJ_FWD, in4d);
  EXPECT_EQ(0, proj_errno(pj_epoch.get()));
  EXPECT_NEAR(6371006.0, fwd4d.xyzt.x, 1e-6);
  EXPECT_NEAR(2.0, fwd4d.xyzt.y, 1e-6);
  EXPECT_NEAR(4.0, fwd4d.xyzt.z, 1e-6);

  // pj_deformation_reverse_shift evaluates z0 = delta(input).z once and uses
  // `out += dif` in its 10-step horizontal iteration, leaving a ~1.93 mm
  // residual in X/Y and ~3.8 um in Z for a 6 m shift on R=6371000.
  const PJ_COORD inv4d = proj_trans(pj_epoch.get(), PJ_INV, fwd4d);
  EXPECT_EQ(0, proj_errno(pj_epoch.get()));
  EXPECT_NEAR(in4d.xyzt.x, inv4d.xyzt.x, 3e-3);
  EXPECT_NEAR(in4d.xyzt.y, inv4d.xyzt.y, 3e-3);
  EXPECT_NEAR(in4d.xyzt.z, inv4d.xyzt.z, 1e-5);

  // 2. Missing time in 4D and 3D when +t_epoch is used (Q->dt == HUGE_VAL).
  const PJ_COORD no_t = proj_coord(6371000.0, 0.0, 0.0, HUGE_VAL);
  EXPECT_TRUE(std::isinf(proj_trans(pj_epoch.get(), PJ_FWD, no_t).xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_MISSING_TIME, proj_errno(pj_epoch.get()));
  proj_errno_reset(pj_epoch.get());

  EXPECT_TRUE(std::isinf(proj_trans(pj_epoch.get(), PJ_INV, no_t).xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_MISSING_TIME, proj_errno(pj_epoch.get()));
  proj_errno_reset(pj_epoch.get());

  const PJ_LPZ lpz_in = {6371000.0, 0.0, 0.0};
  EXPECT_TRUE(std::isinf(pj_fwd3d(lpz_in, pj_epoch.get()).x));
  const PJ_XYZ xyz_in = {6371000.0, 0.0, 0.0};
  EXPECT_TRUE(std::isinf(pj_inv3d(xyz_in, pj_epoch.get()).lam));

  // 3. Test 3D and 4D with explicit +dt=2.0.
  ScopedPj pj_dt(proj_create(
      ctx.get(), "+proj=deformation +grids=/mem/vel3d.tif +dt=2.0 +R=6371000"));
  ASSERT_NE(nullptr, pj_dt.get());

  const PJ_XYZ fwd3d = pj_fwd3d(lpz_in, pj_dt.get());
  EXPECT_EQ(0, proj_errno(pj_dt.get()));
  EXPECT_NEAR(6371006.0, fwd3d.x, 1e-6);
  EXPECT_NEAR(2.0, fwd3d.y, 1e-6);
  EXPECT_NEAR(4.0, fwd3d.z, 1e-6);

  const PJ_LPZ inv3d = pj_inv3d(fwd3d, pj_dt.get());
  EXPECT_EQ(0, proj_errno(pj_dt.get()));
  EXPECT_NEAR(6371000.0, inv3d.lam, 3e-3);
  EXPECT_NEAR(0.0, inv3d.phi, 3e-3);
  EXPECT_NEAR(0.0, inv3d.z, 1e-5);

  const PJ_COORD fwd4d_dt = proj_trans(pj_dt.get(), PJ_FWD, no_t);
  EXPECT_EQ(0, proj_errno(pj_dt.get()));
  EXPECT_NEAR(6371006.0, fwd4d_dt.xyzt.x, 1e-6);
  const PJ_COORD inv4d_dt = proj_trans(pj_dt.get(), PJ_INV, fwd4d_dt);
  EXPECT_EQ(0, proj_errno(pj_dt.get()));
  EXPECT_NEAR(6371000.0, inv4d_dt.xyzt.x, 3e-3);

  // 4. Coordinate outside grid extent (lon = 90 deg -> Y = 6371000) fails in
  //    4D forward, 4D inverse, and 3D forward.
  const PJ_COORD out_coord = proj_coord(0.0, 6371000.0, 0.0, 2022.0);
  EXPECT_TRUE(std::isinf(proj_trans(pj_dt.get(), PJ_FWD, out_coord).xyzt.x));
  EXPECT_TRUE(std::isinf(proj_trans(pj_dt.get(), PJ_INV, out_coord).xyzt.x));
  EXPECT_TRUE(std::isinf(pj_fwd3d({0.0, 6371000.0, 0.0}, pj_dt.get()).x));
}

TEST(DeformationTest, GenericGridValidationErrorsAndReverseEdgeBreakout) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 1. Grid with < 3 samples per pixel is rejected during transform.
  constexpr float kTwoSamples[] = {
      1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f,
  };
  fs.AddFile(
      "/mem/two_samples.tif",
      BuildMinimalGeoTiff(2, 2, 2, -10.0, 10.0, 20.0, 20.0, kTwoSamples));
  ScopedPj pj_two(proj_create(
      ctx.get(),
      "+proj=deformation +grids=/mem/two_samples.tif +dt=1.0 +R=6371000"));
  ASSERT_NE(nullptr, pj_two.get());
  const PJ_COORD pt = proj_coord(6371000.0, 0.0, 0.0, 2020.0);
  EXPECT_TRUE(std::isinf(proj_trans(pj_two.get(), PJ_FWD, pt).xyzt.x));

  // 2. Grid with unsupported unit (not "millimetres per year") is rejected.
  constexpr float kThreeSamples[] = {
      1.0f, 2.0f, 3.0f, 1.0f, 2.0f, 3.0f, 1.0f, 2.0f, 3.0f, 1.0f, 2.0f, 3.0f,
  };
  constexpr std::string_view kBadUnitXml =
      "<GDALMetadata>"
      "<Item name=\"UNITTYPE\" sample=\"0\">metre per year</Item>"
      "</GDALMetadata>";
  fs.AddFile("/mem/bad_unit.tif",
             BuildMinimalGeoTiff(2, 2, 3, -10.0, 10.0, 20.0, 20.0,
                                 kThreeSamples, true, kBadUnitXml));
  ScopedPj pj_unit(proj_create(
      ctx.get(),
      "+proj=deformation +grids=/mem/bad_unit.tif +dt=1.0 +R=6371000"));
  ASSERT_NE(nullptr, pj_unit.get());
  EXPECT_TRUE(std::isinf(proj_trans(pj_unit.get(), PJ_FWD, pt).xyzt.x));

  // 3. Projected (non-geographic) GeoTIFF fails in
  //    pj_bilinear_interpolation_three_samples.
  fs.AddFile("/mem/proj_grid.tif",
             BuildMinimalGeoTiff(2, 2, 3, -1.0, 1.0, 2.0, 2.0, kThreeSamples,
                                 /*is_geographic=*/false));
  ScopedPj pj_non_geog(proj_create(
      ctx.get(),
      "+proj=deformation +grids=/mem/proj_grid.tif +dt=1.0 +R=6371000"));
  ASSERT_NE(nullptr, pj_non_geog.get());
  EXPECT_TRUE(std::isinf(proj_trans(pj_non_geog.get(), PJ_FWD, pt).xyzt.x));

  // 4. Reverse shift stepping outside grid edge on iteration 1 breaks out of
  //    the loop cleanly (lines 233-234).
  constexpr float kLargeEastVel[] = {
      100000.0f, 0.0f, 0.0f, 100000.0f, 0.0f, 0.0f,
      100000.0f, 0.0f, 0.0f, 100000.0f, 0.0f, 0.0f,
  };
  fs.AddFile("/mem/edge_grid.tif",
             BuildMinimalGeoTiff(2, 2, 3, 0.0, 10.0, 10.0, 20.0, kLargeEastVel,
                                 /*is_geographic=*/true));
  ScopedPj pj_edge(proj_create(
      ctx.get(),
      "+proj=deformation +grids=/mem/edge_grid.tif +dt=100.0 +R=6371000"));
  ASSERT_NE(nullptr, pj_edge.get());
  // Point at lon = +0.001 deg (Y ~ +111 m), lat = 0: initial shift is +100 m/yr
  // east, so out.y = input.y - 100 * 100 steps to Y ~ -9889 m (lon < 0, outside
  // grid [0, 10]).
  const PJ_COORD near_west_edge = proj_coord(6371000.0, 111.0, 0.0, 2020.0);
  const PJ_COORD inv_edge = proj_trans(pj_edge.get(), PJ_INV, near_west_edge);
  EXPECT_FALSE(std::isinf(inv_edge.xyzt.x));
}

TEST(DeformationTest, LegacySplitXyAndZGridsForwardInverseAndOutOfGrid) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // Build CTable2 horizontal velocity grid (east=100 mm/yr, north=200 mm/yr)
  // and GTX vertical velocity grid (up=300 mm/yr, keeping |val| <= 1000 so
  // GTXVerticalShiftGrid::isNodata does not treat it as nodata) covering
  // [-10, 10] deg. With +dt=20.0, total shift is dE=+2 m, dN=+4 m, dU=+6 m.
  constexpr float kXyShifts[] = {
      100.0f, 200.0f, 100.0f, 200.0f, 100.0f, 200.0f, 100.0f, 200.0f,
  };
  fs.AddFile("/mem/xy_vel.ct2",
             BuildCTable2Grid(DegToRad(-10.0), DegToRad(-10.0), DegToRad(20.0),
                              DegToRad(20.0), 2, 2, kXyShifts));

  constexpr float kZShifts[] = {300.0f, 300.0f, 300.0f, 300.0f};
  fs.AddFile("/mem/z_vel.gtx",
             BuildGtxGrid(-10.0, -10.0, 20.0, 20.0, 2, 2, kZShifts));

  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=deformation +xy_grids=/mem/xy_vel.ct2 +z_grids=/mem/z_vel.gtx "
      "+dt=20.0 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Inside grid at (lon=0, lat=0): dX=+6, dY=+2, dZ=+4.
  const PJ_COORD in = proj_coord(6371000.0, 0.0, 0.0, 2020.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(6371006.0, fwd.xyzt.x, 1e-6);
  EXPECT_NEAR(2.0, fwd.xyzt.y, 1e-6);
  EXPECT_NEAR(4.0, fwd.xyzt.z, 1e-6);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.xyzt.x, inv.xyzt.x, 3e-3);
  EXPECT_NEAR(in.xyzt.y, inv.xyzt.y, 3e-3);
  EXPECT_NEAR(in.xyzt.z, inv.xyzt.z, 1e-5);

  // 2. Outside grid at (lon=-53.1 deg, lat=-31.0 deg) with PJ_LOG_DEBUG active
  //    exercises the PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID debug log path.
  //    Choosing X > 0, Y < 0, Z < 0 ensures -sp*cl, -sl, and cp*cl are all
  //    strictly positive so ENU->XYZ on HUGE_VAL yields +inf rather than 0*inf.
  ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);
  const PJ_COORD out_coord =
      proj_coord(3000000.0, -4000000.0, -3000000.0, 2020.0);
  const PJ_COORD out_res = proj_trans(pj.get(), PJ_FWD, out_coord);
  EXPECT_TRUE(std::isinf(out_res.xyzt.x));
  EXPECT_TRUE(capture.ContainsMessage("outside deformation model"));
}

}  // namespace
}  // namespace proj
