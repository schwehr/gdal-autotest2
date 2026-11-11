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
// Unit tests for geocentric Cartesian grid shifts (+proj=xyzgridshift) in
// proj/src/transformations/xyzgridshift.cpp.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/xyzgridshift.cpp

#include <cmath>
#include <span>  // NOLINT(build/c++20)
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

// WGS84 equatorial prime-meridian surface point (lon=0, lat=0, h=0).
constexpr double kWgs84EquatorX = 6378137.0;

std::vector<float> MakeUniformPixels(int width, int height,
                                     std::span<const float> sample_values) {
  const int spp = static_cast<int>(sample_values.size());
  std::vector<float> pixels(static_cast<size_t>(width) * height * spp, 0.0f);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int s = 0; s < spp; ++s) {
        pixels[(static_cast<size_t>(y) * width + x) * spp + s] =
            sample_values[s];
      }
    }
  }
  return pixels;
}

TEST(XyzgridshiftTest, ParameterValidationErrorsAndDestructorNullGuard) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);

  // 1. Missing +grids parameter fails with PROJ_ERR_INVALID_OP_MISSING_ARG.
  ScopedPj missing_grids(proj_create(ctx.get(), "+proj=xyzgridshift"));
  EXPECT_EQ(nullptr, missing_grids.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("+grids parameter missing."));

  // 2. Unsupported +grid_ref value fails with
  // PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE.
  logs.Clear();
  proj_context_errno_set(ctx.get(), 0);
  ScopedPj bad_grid_ref(proj_create(
      ctx.get(), "+proj=xyzgridshift +grids=null +grid_ref=invalid_crs"));
  EXPECT_EQ(nullptr, bad_grid_ref.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("unusupported value for grid_ref"));

  // 3. Non-existent required grid fails with
  //    PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID.
  logs.Clear();
  proj_context_errno_set(ctx.get(), 0);
  ScopedPj missing_file(proj_create(
      ctx.get(), "+proj=xyzgridshift +grids=/mem/does_not_exist.tif"));
  EXPECT_EQ(nullptr, missing_file.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("could not find required grid(s)."));

  // 4. Verify destructor nullptr guard.
  ScopedPj valid_null(proj_create(
      ctx.get(), "+proj=xyzgridshift +grids=null +grid_ref=input_crs"));
  ASSERT_NE(nullptr, valid_null.get());
  EXPECT_EQ(nullptr, valid_null->destructor(nullptr, 0));
}

TEST(XyzgridshiftTest, NullGridLeavesCartesianCoordinatesUnchanged) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  for (const char* def : {
           "+proj=xyzgridshift +grids=null",
           "+proj=xyzgridshift +grids=null +grid_ref=input_crs",
           "+proj=xyzgridshift +grids=null +grid_ref=output_crs",
       }) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD in = proj_coord(kWgs84EquatorX, 1000.0, 2000.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(in.xyz.x, fwd.xyz.x);
    EXPECT_DOUBLE_EQ(in.xyz.y, fwd.xyz.y);
    EXPECT_DOUBLE_EQ(in.xyz.z, fwd.xyz.z);

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(in.xyz.x, inv.xyz.x);
    EXPECT_DOUBLE_EQ(in.xyz.y, inv.xyz.y);
    EXPECT_DOUBLE_EQ(in.xyz.z, inv.xyz.z);
  }
}

TEST(XyzgridshiftTest, InputCrsAndOutputCrsRoundTripWithMultiplierAndReassign) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // Build a 3-band GeoTIFF covering [-10, 10] deg lon x [-10, 10] deg lat with
  // uniform shifts (dx=10.0, dy=-20.0, dz=30.0) metres.
  const std::vector<float> pixels =
      MakeUniformPixels(3, 3, std::span<const float>({10.0f, -20.0f, 30.0f}));
  fs.AddFile("/mem/xyz_shift.tif",
             BuildMinimalGeoTiff(3, 3, 3, -10.0, 10.0, 10.0, 10.0, pixels,
                                 /*is_geographic=*/true));

  // 1. Default grid_ref=input_crs with +multiplier=2.0:
  //    PJ_FWD applies direct_adjustment (+20, -40, +60);
  //    PJ_INV applies iterative_adjustment (-20, +40, -60).
  ScopedPj pj_in(proj_create(
      ctx.get(),
      "+proj=xyzgridshift +grids=/mem/xyz_shift.tif +multiplier=2.0"));
  ASSERT_NE(nullptr, pj_in.get());

  // Exercise pj_xyzgridshift_reassign_context on an active grid list.
  ScopedContext ctx2 = MakeContext();
  ASSERT_NE(nullptr, ctx2.get());
  ASSERT_TRUE(fs.Install(ctx2.get()));
  proj_assign_context(pj_in.get(), ctx2.get());
  proj_assign_context(pj_in.get(), ctx.get());

  const PJ_COORD pt = proj_coord(kWgs84EquatorX, 0.0, 0.0, 0.0);
  const PJ_COORD fwd_in = proj_trans(pj_in.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(pj_in.get()));
  EXPECT_NEAR(kWgs84EquatorX + 20.0, fwd_in.xyz.x, 1e-6);
  EXPECT_NEAR(-40.0, fwd_in.xyz.y, 1e-6);
  EXPECT_NEAR(60.0, fwd_in.xyz.z, 1e-6);

  const PJ_COORD inv_in = proj_trans(pj_in.get(), PJ_INV, fwd_in);
  EXPECT_EQ(0, proj_errno(pj_in.get()));
  EXPECT_NEAR(pt.xyz.x, inv_in.xyz.x, 1e-6);
  EXPECT_NEAR(pt.xyz.y, inv_in.xyz.y, 1e-6);
  EXPECT_NEAR(pt.xyz.z, inv_in.xyz.z, 1e-6);

  // 2. grid_ref=output_crs:
  //    PJ_FWD applies iterative_adjustment (+10, -20, +30);
  //    PJ_INV applies direct_adjustment (-10, +20, -30).
  ScopedPj pj_out(proj_create(
      ctx.get(),
      "+proj=xyzgridshift +grids=/mem/xyz_shift.tif +grid_ref=output_crs"));
  ASSERT_NE(nullptr, pj_out.get());

  const PJ_COORD fwd_out = proj_trans(pj_out.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(pj_out.get()));
  EXPECT_NEAR(kWgs84EquatorX + 10.0, fwd_out.xyz.x, 1e-6);
  EXPECT_NEAR(-20.0, fwd_out.xyz.y, 1e-6);
  EXPECT_NEAR(30.0, fwd_out.xyz.z, 1e-6);

  const PJ_COORD inv_out = proj_trans(pj_out.get(), PJ_INV, fwd_out);
  EXPECT_EQ(0, proj_errno(pj_out.get()));
  EXPECT_NEAR(pt.xyz.x, inv_out.xyz.x, 1e-6);
  EXPECT_NEAR(pt.xyz.y, inv_out.xyz.y, 1e-6);
  EXPECT_NEAR(pt.xyz.z, inv_out.xyz.z, 1e-6);

  // 3. Point outside [-10, 10] deg grid (e.g. near North Pole: X=0, Y=0,
  // Z=6356752)
  //    fails in both direct_adjustment and iterative_adjustment.
  const PJ_COORD pole = proj_coord(0.0, 0.0, 6356752.3142, 0.0);
  const PJ_COORD out_direct = proj_trans(pj_in.get(), PJ_FWD, pole);
  EXPECT_TRUE(std::isinf(out_direct.xyz.x));
  proj_errno_reset(pj_in.get());

  const PJ_COORD out_iter = proj_trans(pj_in.get(), PJ_INV, pole);
  EXPECT_TRUE(std::isinf(out_iter.xyz.x));
}

TEST(XyzgridshiftTest, SampleDescriptionRemappingAndValidationFailures) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 1. Custom channel order via DESCRIPTION metadata and explicit unit=metre:
  //    sample 0 = z_translation (300), sample 1 = x_translation (100),
  //    sample 2 = y_translation (200).
  const std::vector<float> remapped_pixels =
      MakeUniformPixels(2, 2, std::span<const float>({300.0f, 100.0f, 200.0f}));
  fs.AddFile("/mem/remapped.tif",
             BuildMinimalGeoTiff(
                 2, 2, 3, -10.0, 10.0, 20.0, 20.0, remapped_pixels,
                 /*is_geographic=*/true,
                 "<GDALMetadata>"
                 "<Item name=\"DESCRIPTION\" sample=\"0\">z_translation</Item>"
                 "<Item name=\"DESCRIPTION\" sample=\"1\">x_translation</Item>"
                 "<Item name=\"DESCRIPTION\" sample=\"2\">y_translation</Item>"
                 "<Item name=\"UNITTYPE\" sample=\"1\">metre</Item>"
                 "</GDALMetadata>"));

  const PJ_COORD pt = proj_coord(kWgs84EquatorX, 0.0, 0.0, 0.0);
  {
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=xyzgridshift +grids=/mem/remapped.tif"));
    ASSERT_NE(nullptr, pj.get());
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, pt);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(kWgs84EquatorX + 100.0, fwd.xyz.x, 1e-5);
    EXPECT_NEAR(200.0, fwd.xyz.y, 1e-5);
    EXPECT_NEAR(300.0, fwd.xyz.z, 1e-5);
  }

  // 2. Grid with fewer than 3 samples per pixel logs an error and fails.
  const std::vector<float> two_band_pixels =
      MakeUniformPixels(2, 2, std::span<const float>({1.0f, 2.0f}));
  fs.AddFile("/mem/two_samples.tif",
             BuildMinimalGeoTiff(2, 2, 2, -10.0, 10.0, 20.0, 20.0,
                                 two_band_pixels, /*is_geographic=*/true));
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    ScopedPj pj(proj_create(ctx.get(),
                            "+proj=xyzgridshift +grids=/mem/two_samples.tif"));
    ASSERT_NE(nullptr, pj.get());
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, pt);
    EXPECT_TRUE(std::isinf(fwd.xyz.x));
    EXPECT_TRUE(
        logs.ContainsMessage("xyzgridshift: grid has not enough samples"));
  }

  // 3. Grid with UNITTYPE != "metre" logs an error and fails.
  fs.AddFile("/mem/bad_unit.tif",
             BuildMinimalGeoTiff(
                 2, 2, 3, -10.0, 10.0, 20.0, 20.0, remapped_pixels,
                 /*is_geographic=*/true,
                 "<GDALMetadata>"
                 "<Item name=\"UNITTYPE\" sample=\"0\">millimetre</Item>"
                 "</GDALMetadata>"));
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=xyzgridshift +grids=/mem/bad_unit.tif"));
    ASSERT_NE(nullptr, pj.get());
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, pt);
    EXPECT_TRUE(std::isinf(fwd.xyz.x));
    EXPECT_TRUE(logs.ContainsMessage(
        "xyzgridshift: Only unit=metre currently handled"));
  }

  // 4. Projected (non-geographic) grid fails in
  //    pj_bilinear_interpolation_three_samples.
  fs.AddFile("/mem/projected.tif",
             BuildMinimalGeoTiff(2, 2, 3, -10.0, 10.0, 20.0, 20.0,
                                 remapped_pixels, /*is_geographic=*/false));
  {
    ScopedLogSuppressor silence(ctx.get());
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=xyzgridshift +grids=/mem/projected.tif"));
    ASSERT_NE(nullptr, pj.get());
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, pt);
    EXPECT_TRUE(std::isinf(fwd.xyz.x));
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_errno(pj.get()));
  }
}

TEST(XyzgridshiftTest, DeferredGridOpeningSuccessAndCachedError) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogSuppressor silence(ctx.get());

  ctx->defer_grid_opening = true;

  // 1. Deferred opening of a valid grid succeeds on first transform.
  const std::vector<float> pixels =
      MakeUniformPixels(2, 2, std::span<const float>({5.0f, 6.0f, 7.0f}));
  fs.AddFile("/mem/deferred_ok.tif",
             BuildMinimalGeoTiff(2, 2, 3, -10.0, 10.0, 20.0, 20.0, pixels,
                                 /*is_geographic=*/true));

  ScopedPj pj_ok(
      proj_create(ctx.get(), "+proj=xyzgridshift +grids=/mem/deferred_ok.tif"));
  ASSERT_NE(nullptr, pj_ok.get());

  const PJ_COORD pt = proj_coord(kWgs84EquatorX, 0.0, 0.0, 0.0);
  const PJ_COORD fwd_ok = proj_trans(pj_ok.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(pj_ok.get()));
  EXPECT_NEAR(kWgs84EquatorX + 5.0, fwd_ok.xyz.x, 1e-6);
  EXPECT_NEAR(6.0, fwd_ok.xyz.y, 1e-6);
  EXPECT_NEAR(7.0, fwd_ok.xyz.z, 1e-6);

  // 2. Deferred opening of a missing grid fails on first transform and replays
  //    error_code_in_defer_grid_opening on subsequent transforms.
  ScopedPj pj_err(proj_create(
      ctx.get(), "+proj=xyzgridshift +grids=/mem/deferred_missing.tif"));
  ASSERT_NE(nullptr, pj_err.get());

  const PJ_COORD fwd_err1 = proj_trans(pj_err.get(), PJ_FWD, pt);
  EXPECT_TRUE(std::isinf(fwd_err1.xyz.x));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_errno(pj_err.get()));

  proj_errno_reset(pj_err.get());
  const PJ_COORD fwd_err2 = proj_trans(pj_err.get(), PJ_FWD, pt);
  EXPECT_TRUE(std::isinf(fwd_err2.xyz.x));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_errno(pj_err.get()));
}

}  // namespace
}  // namespace proj
