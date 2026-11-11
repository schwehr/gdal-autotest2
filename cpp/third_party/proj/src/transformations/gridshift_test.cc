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
// Unit tests for generic grid shifting (+proj=gridshift) in
// proj/src/transformations/gridshift.cpp.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/gridshift.cpp

#include <cmath>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <limits>
#include <numbers>
#include <optional>
#include <span>  // NOLINT(build/c++20)
#include <string>
#include <string_view>
#include <thread>  // NOLINT
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
constexpr double kDegToRad = kPi / 180.0;
constexpr double kArcSecToRad = kDegToRad / 3600.0;

constexpr double DegToRad(double deg) { return deg * kDegToRad; }

// Builds a uniform-value pixel array of size width * height * spp.
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

TEST(GridshiftTest, MissingGridsAndNonExistentGridFileFail) {
  absl::Cleanup clear_cache = [] { pj_clear_gridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);

  // 1. Missing +grids parameter returns PROJ_ERR_INVALID_OP_MISSING_ARG.
  ScopedPj missing_arg(proj_create(ctx.get(), "+proj=gridshift"));
  EXPECT_EQ(nullptr, missing_arg.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("+grids parameter missing."));

  // 2. Non-existent grid file returns
  // PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID.
  logs.Clear();
  proj_context_errno_set(ctx.get(), 0);
  ScopedPj missing_file(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/does_not_exist.tif"));
  EXPECT_EQ(nullptr, missing_file.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("could not find required grid(s)."));
}

TEST(GridshiftTest, CheckGridTypesValidationErrors) {
  absl::Cleanup clear_cache = [] { pj_clear_gridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  const std::vector<float> pixels2 =
      MakeUniformPixels(2, 2, std::span<const float>({1.0f, 2.0f}));
  const std::vector<float> pixels1 =
      MakeUniformPixels(2, 2, std::span<const float>({1.0f}));

  // 1. Missing TYPE metadata item.
  fs.AddFile("/mem/no_type.tif",
             BuildMinimalGeoTiff(2, 2, 2, 0.0, 10.0, 10.0, 10.0, pixels2,
                                 /*is_geographic=*/true,
                                 "<GDALMetadata></GDALMetadata>"));
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=gridshift +grids=/mem/no_type.tif"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_context_errno(ctx.get()));
    EXPECT_TRUE(logs.ContainsMessage("Missing TYPE metadata item in grid(s)."));
  }

  // 2. Unhandled TYPE metadata item.
  fs.AddFile(
      "/mem/bad_type.tif",
      BuildMinimalGeoTiff(2, 2, 2, 0.0, 10.0, 10.0, 10.0, pixels2,
                          /*is_geographic=*/true,
                          "<GDALMetadata><Item name=\"TYPE\">VELOCITY_3D</Item>"
                          "</GDALMetadata>"));
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=gridshift +grids=/mem/bad_type.tif"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_context_errno(ctx.get()));
    EXPECT_TRUE(logs.ContainsMessage("Unhandled value for TYPE metadata item"));
  }

  // 3. Constant offset with more than 1 grid is rejected.
  fs.AddFile("/mem/with_offset.tif",
             BuildMinimalGeoTiff(
                 2, 2, 2, 0.0, 10.0, 10.0, 10.0, pixels2,
                 /*is_geographic=*/false,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                 "<Item name=\"constant_offset\" sample=\"0\">100.0</Item>"
                 "<Item name=\"constant_offset\" sample=\"1\">200.0</Item>"
                 "</GDALMetadata>"));
  fs.AddFile("/mem/second_horiz.tif",
             BuildMinimalGeoTiff(2, 2, 2, 0.0, 10.0, 10.0, 10.0, pixels2,
                                 /*is_geographic=*/false,
                                 "<GDALMetadata>"
                                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                                 "</GDALMetadata>"));
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    ScopedPj pj(proj_create(
        ctx.get(),
        "+proj=gridshift +grids=/mem/with_offset.tif,/mem/second_horiz.tif"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_TRUE(logs.ContainsMessage(
        "Only one grid with shift offset is supported at a time."));
  }

  // 4. Non-numeric constant_offset on sample 0 and sample 1.
  fs.AddFile("/mem/bad_offset_x.tif",
             BuildMinimalGeoTiff(
                 2, 2, 2, 0.0, 10.0, 10.0, 10.0, pixels2,
                 /*is_geographic=*/false,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                 "<Item name=\"constant_offset\" sample=\"0\">bad_x</Item>"
                 "<Item name=\"constant_offset\" sample=\"1\">200.0</Item>"
                 "</GDALMetadata>"));
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=gridshift +grids=/mem/bad_offset_x.tif"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_TRUE(logs.ContainsMessage("Invalid offset value"));
  }

  fs.AddFile("/mem/bad_offset_y.tif",
             BuildMinimalGeoTiff(
                 2, 2, 2, 0.0, 10.0, 10.0, 10.0, pixels2,
                 /*is_geographic=*/false,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                 "<Item name=\"constant_offset\" sample=\"0\">100.0</Item>"
                 "<Item name=\"constant_offset\" sample=\"1\">bad_y</Item>"
                 "</GDALMetadata>"));
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=gridshift +grids=/mem/bad_offset_y.tif"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_TRUE(logs.ContainsMessage("Invalid offset value"));
  }

  // 5. Unsupported mix of multiple vertical grid types.
  fs.AddFile(
      "/mem/ellps_h.tif",
      BuildMinimalGeoTiff(2, 2, 1, 0.0, 10.0, 10.0, 10.0, pixels1,
                          /*is_geographic=*/true,
                          "<GDALMetadata>"
                          "<Item name=\"TYPE\">ELLIPSOIDAL_HEIGHT_OFFSET</Item>"
                          "<Item name=\"DESCRIPTION\" sample=\"0\">"
                          "ellipsoidal_height_offset</Item>"
                          "</GDALMetadata>"));
  fs.AddFile(
      "/mem/v2v.tif",
      BuildMinimalGeoTiff(
          2, 2, 1, 0.0, 10.0, 10.0, 10.0, pixels1,
          /*is_geographic=*/true,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">VERTICAL_OFFSET_VERTICAL_TO_VERTICAL</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"0\">vertical_offset</Item>"
          "</GDALMetadata>"));
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    ScopedPj pj(proj_create(
        ctx.get(), "+proj=gridshift +grids=/mem/ellps_h.tif,/mem/v2v.tif"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_TRUE(logs.ContainsMessage("Unsupported mix of grid types."));
  }
}

TEST(GridshiftTest, InterpolationAndCoordTypeParameterValidationAndIoUnits) {
  absl::Cleanup clear_cache = [] { pj_clear_gridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  const std::vector<float> pixels2 =
      MakeUniformPixels(2, 2, std::span<const float>({1.0f, 2.0f}));
  fs.AddFile("/mem/geog_horiz.tif",
             BuildMinimalGeoTiff(2, 2, 2, 0.0, 10.0, 10.0, 10.0, pixels2,
                                 /*is_geographic=*/true,
                                 "<GDALMetadata>"
                                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                                 "</GDALMetadata>"));
  fs.AddFile("/mem/proj_horiz.tif",
             BuildMinimalGeoTiff(2, 2, 2, 1000.0, 2000.0, 100.0, 100.0, pixels2,
                                 /*is_geographic=*/false,
                                 "<GDALMetadata>"
                                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                                 "</GDALMetadata>"));

  // 1. Invalid +interpolation parameter.
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    ScopedPj pj(proj_create(
        ctx.get(),
        "+proj=gridshift +grids=/mem/geog_horiz.tif +interpolation=cubic"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
    EXPECT_TRUE(logs.ContainsMessage("Unsupported value for +interpolation."));
  }

  // 2. Mismatched +coord_type=projected on geographic grid.
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    pj_clear_gridshift_knowngrids_cache();
    ScopedPj pj(proj_create(
        ctx.get(),
        "+proj=gridshift +grids=/mem/geog_horiz.tif +coord_type=projected"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
    EXPECT_TRUE(logs.ContainsMessage("known to not be projected"));
  }

  // 3. Mismatched +coord_type=geographic on projected grid.
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    pj_clear_gridshift_knowngrids_cache();
    ScopedPj pj(proj_create(
        ctx.get(),
        "+proj=gridshift +grids=/mem/proj_horiz.tif +coord_type=geographic"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
    EXPECT_TRUE(logs.ContainsMessage("known to be projected"));
  }

  // 4. Unsupported +coord_type value.
  {
    ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);
    pj_clear_gridshift_knowngrids_cache();
    ScopedPj pj(proj_create(
        ctx.get(),
        "+proj=gridshift +grids=/mem/geog_horiz.tif +coord_type=geocentric"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
    EXPECT_TRUE(logs.ContainsMessage("Unsupported value for +coord_type"));
  }

  // 5. First open without +coord_type sets PJ_IO_UNITS_WHATEVER, and populates
  //    gKnownGrids so the second open sets PJ_IO_UNITS_RADIANS / PROJECTED and
  //    defers grid opening until first transform. Call pj_create_internal
  //    directly for the first handle because proj_create's PROJStringParser
  //    validates via a temporary pj_create_internal pass before creating the
  //    returned PJ*.
  pj_clear_gridshift_knowngrids_cache();
  ScopedPj geog_first(pj_create_internal(
      ctx.get(), "+proj=gridshift +grids=/mem/geog_horiz.tif"));
  ASSERT_NE(nullptr, geog_first.get());
  EXPECT_EQ(PJ_IO_UNITS_WHATEVER, geog_first->left);
  EXPECT_EQ(PJ_IO_UNITS_WHATEVER, geog_first->right);

  ScopedPj geog_second(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/geog_horiz.tif"));
  ASSERT_NE(nullptr, geog_second.get());
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, geog_second->left);
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, geog_second->right);

  ScopedPj proj_with_coord_type(proj_create(
      ctx.get(),
      "+proj=gridshift +grids=/mem/proj_horiz.tif +coord_type=projected"));
  ASSERT_NE(nullptr, proj_with_coord_type.get());
  EXPECT_EQ(PJ_IO_UNITS_PROJECTED, proj_with_coord_type->left);
  EXPECT_EQ(PJ_IO_UNITS_PROJECTED, proj_with_coord_type->right);
}

TEST(GridshiftTest, DeferredGridOpeningReassignContextAndNullDestructor) {
  absl::Cleanup clear_cache = [] { pj_clear_gridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogSuppressor silence(ctx.get());

  ctx->defer_grid_opening = true;

  // 1. Deferred opening of a missing grid fails on first transform and caches
  //    m_error_code_in_defer_grid_opening for subsequent transforms.
  ScopedPj missing_deferred(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/missing_def.tif"));
  ASSERT_NE(nullptr, missing_deferred.get());
  const PJ_COORD fwd_err = proj_trans(missing_deferred.get(), PJ_FWD,
                                      proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(fwd_err.xyz.x));
  EXPECT_NE(0, proj_errno(missing_deferred.get()));

  proj_errno_reset(missing_deferred.get());
  const PJ_COORD inv_err = proj_trans(missing_deferred.get(), PJ_INV,
                                      proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inv_err.xyz.x));
  EXPECT_NE(0, proj_errno(missing_deferred.get()));

  // 2. Deferred opening of a grid that fails checkGridTypes (missing TYPE).
  const std::vector<float> pixels2 =
      MakeUniformPixels(2, 2, std::span<const float>({1.0f, 2.0f}));
  fs.AddFile("/mem/def_no_type.tif",
             BuildMinimalGeoTiff(2, 2, 2, 0.0, 10.0, 10.0, 10.0, pixels2,
                                 /*is_geographic=*/true,
                                 "<GDALMetadata></GDALMetadata>"));
  ScopedPj bad_type_deferred(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/def_no_type.tif"));
  ASSERT_NE(nullptr, bad_type_deferred.get());
  const PJ_COORD bad_type_out = proj_trans(bad_type_deferred.get(), PJ_FWD,
                                           proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(bad_type_out.xyz.x));

  // 3. Deferred opening of a valid grid, followed by proj_assign_context and
  //    direct null destructor check.
  fs.AddFile("/mem/def_valid.tif",
             BuildMinimalGeoTiff(
                 2, 2, 2, 0.0, 10.0, 10.0, 10.0,
                 MakeUniformPixels(2, 2, std::span<const float>({3.6f, 7.2f})),
                 /*is_geographic=*/true,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                 "</GDALMetadata>"));
  ScopedPj valid_deferred(proj_create(
      ctx.get(),
      "+proj=gridshift +grids=/mem/def_valid.tif +coord_type=geographic"));
  ASSERT_NE(nullptr, valid_deferred.get());
  EXPECT_EQ(nullptr, valid_deferred->destructor(nullptr, 0));

  const PJ_COORD in = proj_coord(DegToRad(5.0), DegToRad(5.0), 10.0, 0.0);
  const PJ_COORD fwd = proj_trans(valid_deferred.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(valid_deferred.get()));
  EXPECT_NEAR(DegToRad(5.0) + 7.2 * kArcSecToRad, fwd.lpz.lam, 1e-11);
  EXPECT_NEAR(DegToRad(5.0) + 3.6 * kArcSecToRad, fwd.lpz.phi, 1e-11);

  ScopedContext ctx2 = MakeContext();
  ASSERT_NE(nullptr, ctx2.get());
  ASSERT_TRUE(fs.Install(ctx2.get()));
  proj_assign_context(valid_deferred.get(), ctx2.get());

  const PJ_COORD inv = proj_trans(valid_deferred.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(valid_deferred.get()));
  EXPECT_NEAR(in.lpz.lam, inv.lpz.lam, 1e-11);
  EXPECT_NEAR(in.lpz.phi, inv.lpz.phi, 1e-11);
  proj_assign_context(valid_deferred.get(), ctx.get());
}

TEST(GridshiftTest, Geographic3DOffsetBilinearRoundTripAndNoZTransform) {
  absl::Cleanup clear_cache = [] { pj_clear_gridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 1. Standard GEOGRAPHIC_3D_OFFSET with sample 0 = latitude_offset,
  //    sample 1 = longitude_offset, sample 2 = ellipsoidal_height_offset
  //    (exercises swapXYInRes = true).
  const std::vector<float> pixels3 =
      MakeUniformPixels(3, 3, std::span<const float>({1.5f, -2.5f, 12.25f}));
  fs.AddFile(
      "/mem/geog3d_latlon.tif",
      BuildMinimalGeoTiff(
          3, 3, 3, 0.0, 10.0, 5.0, 5.0, pixels3,
          /*is_geographic=*/true,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">GEOGRAPHIC_3D_OFFSET</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"0\">latitude_offset</Item>"
          "<Item name=\"UNITTYPE\" sample=\"0\">arc-second</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"1\">longitude_offset</Item>"
          "<Item name=\"UNITTYPE\" sample=\"1\">arc-second</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"2\">"
          "ellipsoidal_height_offset</Item>"
          "<Item name=\"UNITTYPE\" sample=\"2\">metre</Item>"
          "</GDALMetadata>"));

  ScopedPj pj3d(proj_create(
      ctx.get(),
      "+proj=gridshift +grids=/mem/geog3d_latlon.tif +interpolation=bilinear"));
  ASSERT_NE(nullptr, pj3d.get());

  const PJ_COORD in1 = proj_coord(DegToRad(2.0), DegToRad(3.0), 100.0, 0.0);
  const PJ_COORD fwd1 = proj_trans(pj3d.get(), PJ_FWD, in1);
  EXPECT_EQ(0, proj_errno(pj3d.get()));
  EXPECT_NEAR(in1.lpz.lam - 2.5 * kArcSecToRad, fwd1.lpz.lam, 1e-11);
  EXPECT_NEAR(in1.lpz.phi + 1.5 * kArcSecToRad, fwd1.lpz.phi, 1e-11);
  EXPECT_NEAR(112.25, fwd1.lpz.z, 1e-6);

  // Transform a second point inside the same grid cell [0, 5] x [0, 5] to
  // exercise the gridInfo.lastIdxXY == indxy cached cell path.
  const PJ_COORD in2 = proj_coord(DegToRad(2.5), DegToRad(3.5), 50.0, 0.0);
  const PJ_COORD fwd2 = proj_trans(pj3d.get(), PJ_FWD, in2);
  EXPECT_EQ(0, proj_errno(pj3d.get()));
  EXPECT_NEAR(in2.lpz.lam - 2.5 * kArcSecToRad, fwd2.lpz.lam, 1e-11);
  EXPECT_NEAR(in2.lpz.phi + 1.5 * kArcSecToRad, fwd2.lpz.phi, 1e-11);
  EXPECT_NEAR(62.25, fwd2.lpz.z, 1e-6);

  const PJ_COORD inv1 = proj_trans(pj3d.get(), PJ_INV, fwd1);
  EXPECT_EQ(0, proj_errno(pj3d.get()));
  EXPECT_NEAR(in1.lpz.lam, inv1.lpz.lam, 1e-11);
  EXPECT_NEAR(in1.lpz.phi, inv1.lpz.phi, 1e-11);
  EXPECT_NEAR(in1.lpz.z, inv1.lpz.z, 1e-6);

  // 2. +no_z_transform skips the vertical sample while shifting X and Y.
  ScopedPj pj_no_z(proj_create(
      ctx.get(),
      "+proj=gridshift +grids=/mem/geog3d_latlon.tif +no_z_transform"));
  ASSERT_NE(nullptr, pj_no_z.get());
  const PJ_COORD fwd_no_z = proj_trans(pj_no_z.get(), PJ_FWD, in1);
  EXPECT_EQ(0, proj_errno(pj_no_z.get()));
  EXPECT_NEAR(fwd1.lpz.lam, fwd_no_z.lpz.lam, 1e-11);
  EXPECT_NEAR(fwd1.lpz.phi, fwd_no_z.lpz.phi, 1e-11);
  EXPECT_NEAR(in1.lpz.z, fwd_no_z.lpz.z, 1e-9);

  // 3. GEOGRAPHIC_3D_OFFSET with sample 0 = longitude_offset and
  //    sample 1 = latitude_offset (exercises swapXYInRes = false).
  fs.AddFile(
      "/mem/geog3d_lonlat.tif",
      BuildMinimalGeoTiff(
          3, 3, 3, 0.0, 10.0, 5.0, 5.0,
          MakeUniformPixels(3, 3, std::span<const float>({-2.5f, 1.5f, 8.0f})),
          /*is_geographic=*/true,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">GEOGRAPHIC_3D_OFFSET</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"0\">longitude_offset</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"1\">latitude_offset</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"2\">geoid_undulation</Item>"
          "</GDALMetadata>"));
  ScopedPj pj_lonlat(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/geog3d_lonlat.tif"));
  ASSERT_NE(nullptr, pj_lonlat.get());
  const PJ_COORD fwd_lonlat = proj_trans(pj_lonlat.get(), PJ_FWD, in1);
  EXPECT_EQ(0, proj_errno(pj_lonlat.get()));
  EXPECT_NEAR(in1.lpz.lam - 2.5 * kArcSecToRad, fwd_lonlat.lpz.lam, 1e-11);
  EXPECT_NEAR(in1.lpz.phi + 1.5 * kArcSecToRad, fwd_lonlat.lpz.phi, 1e-11);
  EXPECT_NEAR(108.0, fwd_lonlat.lpz.z, 1e-6);
}

TEST(GridshiftTest, BiquadraticInterpolationFor3DHorizontalAndVerticalGrids) {
  absl::Cleanup clear_cache = [] { pj_clear_gridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 4x4 grid covering [0, 30] x [0, 30] deg (res = 10 deg) so all 3x3 window
  // shift branches (frct <= 0.5 with indxy > 0, frct > 0.5, and indxy + 2 == 4)
  // are exercised.
  const std::vector<float> pixels_3d =
      MakeUniformPixels(4, 4, std::span<const float>({2.0f, 4.0f, 6.0f}));
  fs.AddFile(
      "/mem/biquad_3d.tif",
      BuildMinimalGeoTiff(
          4, 4, 3, 0.0, 30.0, 10.0, 10.0, pixels_3d,
          /*is_geographic=*/true,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">GEOGRAPHIC_3D_OFFSET</Item>"
          "<Item name=\"interpolation_method\">biquadratic</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"0\">latitude_offset</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"1\">longitude_offset</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"2\">"
          "ellipsoidal_height_offset</Item>"
          "</GDALMetadata>"));

  ScopedPj pj3d(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/biquad_3d.tif"));
  ASSERT_NE(nullptr, pj3d.get());

  struct QueryPoint {
    double lon_deg;
    double lat_deg;
  };
  constexpr QueryPoint kPoints[] = {
      {.lon_deg = 2.0, .lat_deg = 2.0},    // indxy = 0, frct = 0.2
      {.lon_deg = 12.0, .lat_deg = 12.0},  // indxy = 1 > 0, frct = 0.2 <= 0.5
      {.lon_deg = 12.5,
       .lat_deg = 12.5},  // same shifted 3x3 window (cache hit)
      {.lon_deg = 18.0, .lat_deg = 18.0},  // indxy = 1, frct = 0.8 > 0.5
      {.lon_deg = 28.0, .lat_deg = 28.0},  // indxy = 2 (indxy + 2 == width)
  };

  for (const auto& pt : kPoints) {
    const PJ_COORD in =
        proj_coord(DegToRad(pt.lon_deg), DegToRad(pt.lat_deg), 10.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj3d.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj3d.get()));
    EXPECT_NEAR(in.lpz.lam + 4.0 * kArcSecToRad, fwd.lpz.lam, 1e-11);
    EXPECT_NEAR(in.lpz.phi + 2.0 * kArcSecToRad, fwd.lpz.phi, 1e-11);
    EXPECT_NEAR(16.0, fwd.lpz.z, 1e-6);

    const PJ_COORD inv = proj_trans(pj3d.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj3d.get()));
    EXPECT_NEAR(in.lpz.lam, inv.lpz.lam, 1e-11);
    EXPECT_NEAR(in.lpz.phi, inv.lpz.phi, 1e-11);
    EXPECT_NEAR(in.lpz.z, inv.lpz.z, 1e-6);
  }

  // 2-sample HORIZONTAL_OFFSET with +interpolation=biquadratic (and via
  // +no_z_transform on 3D grid).
  ScopedPj pj2d(proj_create(
      ctx.get(),
      "+proj=gridshift +grids=/mem/biquad_3d.tif +interpolation=biquadratic "
      "+no_z_transform"));
  ASSERT_NE(nullptr, pj2d.get());
  const PJ_COORD fwd2d =
      proj_trans(pj2d.get(), PJ_FWD,
                 proj_coord(DegToRad(12.0), DegToRad(12.0), 10.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj2d.get()));
  EXPECT_NEAR(DegToRad(12.0) + 4.0 * kArcSecToRad, fwd2d.lpz.lam, 1e-11);
  EXPECT_NEAR(DegToRad(12.0) + 2.0 * kArcSecToRad, fwd2d.lpz.phi, 1e-11);
  EXPECT_NEAR(10.0, fwd2d.lpz.z, 1e-9);
  // Second point in same window hits cache in 2-sample biquadratic path.
  (void)proj_trans(pj2d.get(), PJ_FWD,
                   proj_coord(DegToRad(12.5), DegToRad(12.5), 10.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj2d.get()));

  // 1-sample vertical-only grid with biquadratic interpolation.
  fs.AddFile("/mem/biquad_1d.tif",
             BuildMinimalGeoTiff(
                 4, 4, 1, 0.0, 30.0, 10.0, 10.0,
                 MakeUniformPixels(4, 4, std::span<const float>({3.5f})),
                 /*is_geographic=*/true,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">ELLIPSOIDAL_HEIGHT_OFFSET</Item>"
                 "<Item name=\"interpolation_method\">biquadratic</Item>"
                 "<Item name=\"DESCRIPTION\" sample=\"0\">"
                 "ellipsoidal_height_offset</Item>"
                 "</GDALMetadata>"));
  ScopedPj pj1d(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/biquad_1d.tif"));
  ASSERT_NE(nullptr, pj1d.get());
  const PJ_COORD fwd1d =
      proj_trans(pj1d.get(), PJ_FWD,
                 proj_coord(DegToRad(12.0), DegToRad(12.0), 10.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj1d.get()));
  EXPECT_NEAR(13.5, fwd1d.lpz.z, 1e-6);
  // Second point in same window hits cache in 1-sample biquadratic path.
  const PJ_COORD inv1d = proj_trans(pj1d.get(), PJ_INV, fwd1d);
  EXPECT_EQ(0, proj_errno(pj1d.get()));
  EXPECT_NEAR(10.0, inv1d.lpz.z, 1e-6);
}

TEST(GridshiftTest, ProjectedHorizontalOffsetWithConstantOffsetsAndDefaults) {
  absl::Cleanup clear_cache = [] { pj_clear_gridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 1. Projected grid with explicit easting_offset, northing_offset, and
  //    constant_offset (S-JTSK style offset applied after forward shift and
  //    subtracted before inverse shift).
  fs.AddFile(
      "/mem/proj_offset.tif",
      BuildMinimalGeoTiff(
          3, 3, 2, 1000.0, 3000.0, 500.0, 500.0,
          MakeUniformPixels(3, 3, std::span<const float>({1.25f, -2.5f})),
          /*is_geographic=*/false,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"0\">easting_offset</Item>"
          "<Item name=\"UNITTYPE\" sample=\"0\">metre</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"1\">northing_offset</Item>"
          "<Item name=\"UNITTYPE\" sample=\"1\">metre</Item>"
          "<Item name=\"constant_offset\" sample=\"0\">10000.0</Item>"
          "<Item name=\"constant_offset\" sample=\"1\">20000.0</Item>"
          "</GDALMetadata>"));

  ScopedPj pj(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/proj_offset.tif"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_COORD in = proj_coord(1500.0, 2500.0, 50.0, 0.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1500.0 + 1.25 + 10000.0, fwd.xyz.x, 1e-6);
  EXPECT_NEAR(2500.0 - 2.5 + 20000.0, fwd.xyz.y, 1e-6);
  EXPECT_NEAR(50.0, fwd.xyz.z, 1e-6);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.xyz.x, inv.xyz.x, 1e-6);
  EXPECT_NEAR(in.xyz.y, inv.xyz.y, 1e-6);
  EXPECT_NEAR(in.xyz.z, inv.xyz.z, 1e-6);

  // 2. Projected grid without DESCRIPTION metadata defaults to sample 0 = X,
  //    sample 1 = Y.
  fs.AddFile("/mem/proj_default_desc.tif",
             BuildMinimalGeoTiff(
                 2, 2, 2, 100.0, 200.0, 100.0, 100.0,
                 MakeUniformPixels(2, 2, std::span<const float>({3.0f, 4.0f})),
                 /*is_geographic=*/false,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                 "</GDALMetadata>"));
  ScopedPj pj_def(proj_create(
      ctx.get(), "+proj=gridshift +grids=/mem/proj_default_desc.tif"));
  ASSERT_NE(nullptr, pj_def.get());
  const PJ_COORD fwd_def =
      proj_trans(pj_def.get(), PJ_FWD, proj_coord(150.0, 150.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_def.get()));
  EXPECT_NEAR(153.0, fwd_def.xyz.x, 1e-6);
  EXPECT_NEAR(154.0, fwd_def.xyz.y, 1e-6);
}

TEST(GridshiftTest, VerticalOnlyAndCombinedHorizontalVerticalAndMixed3DGrids) {
  absl::Cleanup clear_cache = [] { pj_clear_gridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogSuppressor silence(ctx.get());

  // 1. Standalone VERTICAL_OFFSET_GEOGRAPHIC_TO_VERTICAL and
  //    VERTICAL_OFFSET_VERTICAL_TO_VERTICAL grids.
  fs.AddFile(
      "/mem/g2v.tif",
      BuildMinimalGeoTiff(
          2, 2, 1, 0.0, 10.0, 10.0, 10.0,
          MakeUniformPixels(2, 2, std::span<const float>({4.5f})),
          /*is_geographic=*/true,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">VERTICAL_OFFSET_GEOGRAPHIC_TO_VERTICAL</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"0\">hydroid_height</Item>"
          "</GDALMetadata>"));
  fs.AddFile(
      "/mem/v2v.tif",
      BuildMinimalGeoTiff(
          2, 2, 1, 0.0, 10.0, 10.0, 10.0,
          MakeUniformPixels(2, 2, std::span<const float>({-1.5f})),
          /*is_geographic=*/true,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">VERTICAL_OFFSET_VERTICAL_TO_VERTICAL</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"0\">vertical_offset</Item>"
          "</GDALMetadata>"));

  for (const char* grid_path : {"/mem/g2v.tif", "/mem/v2v.tif"}) {
    const std::string def = std::string("+proj=gridshift +grids=") + grid_path;
    ScopedPj pj(proj_create(ctx.get(), def.c_str()));
    ASSERT_NE(nullptr, pj.get());
    const PJ_COORD in = proj_coord(DegToRad(5.0), DegToRad(5.0), 20.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));
    // Second query in same cell hits vertical-only bilinear cache.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(20.0, inv.lpz.z, 1e-6);
  }

  // 2. Combined HORIZONTAL_OFFSET + VERTICAL_OFFSET_GEOGRAPHIC_TO_VERTICAL and
  //    HORIZONTAL_OFFSET + VERTICAL_OFFSET_VERTICAL_TO_VERTICAL.
  fs.AddFile("/mem/h_wide.tif",
             BuildMinimalGeoTiff(
                 2, 2, 2, 0.0, 20.0, 20.0, 20.0,
                 MakeUniformPixels(2, 2, std::span<const float>({1.0f, 2.0f})),
                 /*is_geographic=*/true,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                 "</GDALMetadata>"));
  {
    ScopedPj pj_hg2v(proj_create(
        ctx.get(), "+proj=gridshift +grids=/mem/h_wide.tif,/mem/g2v.tif"));
    ASSERT_NE(nullptr, pj_hg2v.get());
    const PJ_COORD fwd =
        proj_trans(pj_hg2v.get(), PJ_FWD,
                   proj_coord(DegToRad(5.0), DegToRad(5.0), 10.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj_hg2v.get()));
    EXPECT_NEAR(14.5, fwd.lpz.z, 1e-6);

    // Point at (15, 15) is inside /mem/h_wide.tif [0, 20] but outside
    // /mem/g2v.tif [0, 10], failing in the second pass of apply().
    const PJ_COORD out_aux =
        proj_trans(pj_hg2v.get(), PJ_FWD,
                   proj_coord(DegToRad(15.0), DegToRad(15.0), 10.0, 0.0));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(pj_hg2v.get()));
    (void)out_aux;
  }
  {
    ScopedPj pj_hv2v(proj_create(
        ctx.get(), "+proj=gridshift +grids=/mem/h_wide.tif,/mem/v2v.tif"));
    ASSERT_NE(nullptr, pj_hv2v.get());
    const PJ_COORD fwd =
        proj_trans(pj_hv2v.get(), PJ_FWD,
                   proj_coord(DegToRad(5.0), DegToRad(5.0), 10.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj_hv2v.get()));
    EXPECT_NEAR(8.5, fwd.lpz.z, 1e-6);
  }

  // 3. Mix of GEOGRAPHIC_3D_OFFSET (on [-10, 0]) and HORIZONTAL_OFFSET +
  //    ELLIPSOIDAL_HEIGHT_OFFSET (on [0, 20]).
  fs.AddFile(
      "/mem/g3d_west.tif",
      BuildMinimalGeoTiff(
          2, 2, 3, -10.0, 10.0, 10.0, 10.0,
          MakeUniformPixels(2, 2, std::span<const float>({1.0f, 1.0f, 5.0f})),
          /*is_geographic=*/true,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">GEOGRAPHIC_3D_OFFSET</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"0\">latitude_offset</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"1\">longitude_offset</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"2\">"
          "ellipsoidal_height_offset</Item>"
          "</GDALMetadata>"));
  // Auxiliary vertical grid with unsupported unit so second-pass
  // grid_apply_internal returns out.x == HUGE_VAL on [10, 20], while valid on
  // [0, 10].
  fs.AddFile("/mem/ellps_east_valid.tif",
             BuildMinimalGeoTiff(
                 2, 2, 1, 0.0, 10.0, 10.0, 10.0,
                 MakeUniformPixels(2, 2, std::span<const float>({9.0f})),
                 /*is_geographic=*/true,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">ELLIPSOIDAL_HEIGHT_OFFSET</Item>"
                 "<Item name=\"DESCRIPTION\" sample=\"0\">"
                 "ellipsoidal_height_offset</Item>"
                 "</GDALMetadata>"));
  fs.AddFile("/mem/ellps_east_badunit.tif",
             BuildMinimalGeoTiff(
                 2, 2, 1, 10.0, 20.0, 10.0, 10.0,
                 MakeUniformPixels(2, 2, std::span<const float>({9.0f})),
                 /*is_geographic=*/true,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">ELLIPSOIDAL_HEIGHT_OFFSET</Item>"
                 "<Item name=\"DESCRIPTION\" sample=\"0\">"
                 "ellipsoidal_height_offset</Item>"
                 "<Item name=\"UNITTYPE\" sample=\"0\">foot</Item>"
                 "</GDALMetadata>"));

  ScopedPj pj_mix(
      proj_create(ctx.get(),
                  "+proj=gridshift +grids=/mem/g3d_west.tif,/mem/h_wide.tif,"
                  "/mem/ellps_east_valid.tif,/mem/ellps_east_badunit.tif"));
  ASSERT_NE(nullptr, pj_mix.get());

  // Point at (-5, 5) hits /mem/g3d_west.tif directly (bFoundGeog3DOffset =
  // true).
  const PJ_COORD mix_west =
      proj_trans(pj_mix.get(), PJ_FWD,
                 proj_coord(DegToRad(-5.0), DegToRad(5.0), 1.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_mix.get()));
  EXPECT_NEAR(6.0, mix_west.lpz.z, 1e-6);

  // Point at (+5, 5) misses GEOGRAPHIC_3D_OFFSET, falls back to
  // HORIZONTAL_OFFSET
  // (/mem/h_wide.tif) and then applies /mem/ellps_east_valid.tif in pass 2.
  const PJ_COORD mix_east = proj_trans(
      pj_mix.get(), PJ_FWD, proj_coord(DegToRad(5.0), DegToRad(5.0), 1.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_mix.get()));
  EXPECT_NEAR(10.0, mix_east.lpz.z, 1e-6);

  // Point at (-50, -50) misses both GEOGRAPHIC_3D_OFFSET and HORIZONTAL_OFFSET.
  proj_errno_reset(pj_mix.get());
  const PJ_COORD mix_out =
      proj_trans(pj_mix.get(), PJ_FWD,
                 proj_coord(DegToRad(-50.0), DegToRad(-50.0), 1.0, 0.0));
  EXPECT_TRUE(std::isinf(mix_out.lpz.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(pj_mix.get()));

  // Note: gridshiftData::apply() binds `std::string &type = m_mainGridType;`
  // by reference, so a fallback from GEOGRAPHIC_3D_OFFSET to HORIZONTAL_OFFSET
  // mutates `m_mainGridType` on that PJ instance while leaving
  // `m_mainGridTypeIsGeographic3DOffset` true. Use a dedicated
  // HORIZONTAL_OFFSET + ELLIPSOIDAL_HEIGHT_OFFSET instance to exercise pass-2
  // grid_apply_internal failure (out.x == HUGE_VAL) at (+15, 15).
  ScopedPj pj_h_ellps(proj_create(
      ctx.get(),
      "+proj=gridshift +grids=/mem/h_wide.tif,/mem/ellps_east_valid.tif,"
      "/mem/ellps_east_badunit.tif"));
  ASSERT_NE(nullptr, pj_h_ellps.get());
  const PJ_COORD mix_bad =
      proj_trans(pj_h_ellps.get(), PJ_FWD,
                 proj_coord(DegToRad(15.0), DegToRad(15.0), 1.0, 0.0));
  EXPECT_TRUE(std::isinf(mix_bad.lpz.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(pj_h_ellps.get()));
}

TEST(GridshiftTest, GridInterpolateRejectsUnsupportedUnitsSamplesAndNodata) {
  absl::Cleanup clear_cache = [] { pj_clear_gridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogSuppressor silence(ctx.get());

  const std::vector<float> px2 =
      MakeUniformPixels(4, 4, std::span<const float>({1.0f, 1.0f}));
  const std::vector<float> px1 =
      MakeUniformPixels(4, 4, std::span<const float>({1.0f}));

  struct BadGridCase {
    const char* path;
    int spp;
    bool is_geographic;
    const char* metadata_xml;
  };
  constexpr BadGridCase kBadGrids[] = {
      {
          .path = "/mem/bad_lat_unit.tif",
          .spp = 2,
          .is_geographic = true,
          .metadata_xml =
              "<GDALMetadata>"
              "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
              "<Item name=\"DESCRIPTION\" sample=\"0\">latitude_offset</Item>"
              "<Item name=\"UNITTYPE\" sample=\"0\">degree</Item>"
              "<Item name=\"DESCRIPTION\" sample=\"1\">longitude_offset</Item>"
              "</GDALMetadata>",
      },
      {
          .path = "/mem/bad_lon_unit.tif",
          .spp = 2,
          .is_geographic = true,
          .metadata_xml =
              "<GDALMetadata>"
              "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
              "<Item name=\"DESCRIPTION\" sample=\"0\">latitude_offset</Item>"
              "<Item name=\"DESCRIPTION\" sample=\"1\">longitude_offset</Item>"
              "<Item name=\"UNITTYPE\" sample=\"1\">radian</Item>"
              "</GDALMetadata>",
      },
      {
          .path = "/mem/bad_east_unit.tif",
          .spp = 2,
          .is_geographic = false,
          .metadata_xml =
              "<GDALMetadata>"
              "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
              "<Item name=\"DESCRIPTION\" sample=\"0\">easting_offset</Item>"
              "<Item name=\"UNITTYPE\" sample=\"0\">foot</Item>"
              "<Item name=\"DESCRIPTION\" sample=\"1\">northing_offset</Item>"
              "</GDALMetadata>",
      },
      {
          .path = "/mem/bad_north_unit.tif",
          .spp = 2,
          .is_geographic = false,
          .metadata_xml =
              "<GDALMetadata>"
              "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
              "<Item name=\"DESCRIPTION\" sample=\"0\">easting_offset</Item>"
              "<Item name=\"DESCRIPTION\" sample=\"1\">northing_offset</Item>"
              "<Item name=\"UNITTYPE\" sample=\"1\">foot</Item>"
              "</GDALMetadata>",
      },
      {
          .path = "/mem/missing_xy_samples.tif",
          .spp = 1,
          .is_geographic = true,
          .metadata_xml = "<GDALMetadata>"
                          "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                          "</GDALMetadata>",
      },
      {
          .path = "/mem/missing_z_sample.tif",
          .spp = 1,
          .is_geographic = true,
          .metadata_xml = "<GDALMetadata>"
                          "<Item name=\"TYPE\">ELLIPSOIDAL_HEIGHT_OFFSET</Item>"
                          "</GDALMetadata>",
      },
      {
          .path = "/mem/bad_meta_interp.tif",
          .spp = 2,
          .is_geographic = true,
          .metadata_xml = "<GDALMetadata>"
                          "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                          "<Item name=\"interpolation_method\">cubic</Item>"
                          "</GDALMetadata>",
      },
  };

  for (const auto& tc : kBadGrids) {
    fs.AddFile(tc.path, BuildMinimalGeoTiff(4, 4, tc.spp, 0.0, 30.0, 10.0, 10.0,
                                            tc.spp == 2 ? px2 : px1,
                                            tc.is_geographic, tc.metadata_xml));
    const std::string def = std::string("+proj=gridshift +grids=") + tc.path;
    ScopedPj pj(proj_create(ctx.get(), def.c_str()));
    ASSERT_NE(nullptr, pj.get());
    const PJ_COORD c = tc.is_geographic
                           ? proj_coord(DegToRad(5.0), DegToRad(5.0), 0.0, 0.0)
                           : proj_coord(5.0, 5.0, 0.0, 0.0);
    const PJ_COORD out = proj_trans(pj.get(), PJ_FWD, c);
    EXPECT_TRUE(std::isinf(out.xyz.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(pj.get()));
  }

  // Nodata handling in bilinear and biquadratic modes for 2-sample and 1-sample
  // grids.
  fs.AddFile(
      "/mem/nodata_2d.tif",
      BuildMinimalGeoTiff(
          4, 4, 2, 0.0, 30.0, 10.0, 10.0,
          MakeUniformPixels(4, 4, std::span<const float>({-9999.0f, -9999.0f})),
          /*is_geographic=*/true,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
          "</GDALMetadata>",
          /*nodata=*/-9999.0));
  fs.AddFile("/mem/nodata_1d.tif",
             BuildMinimalGeoTiff(
                 4, 4, 1, 0.0, 30.0, 10.0, 10.0,
                 MakeUniformPixels(4, 4, std::span<const float>({-9999.0f})),
                 /*is_geographic=*/true,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">ELLIPSOIDAL_HEIGHT_OFFSET</Item>"
                 "<Item name=\"DESCRIPTION\" sample=\"0\">"
                 "ellipsoidal_height_offset</Item>"
                 "</GDALMetadata>",
                 /*nodata=*/-9999.0));

  for (const char* interp : {"bilinear", "biquadratic"}) {
    for (const char* grid : {"/mem/nodata_2d.tif", "/mem/nodata_1d.tif"}) {
      const std::string def = std::string("+proj=gridshift +grids=") + grid +
                              " +interpolation=" + interp;
      ScopedPj pj(proj_create(ctx.get(), def.c_str()));
      ASSERT_NE(nullptr, pj.get());
      const PJ_COORD out =
          proj_trans(pj.get(), PJ_FWD,
                     proj_coord(DegToRad(5.0), DegToRad(5.0), 10.0, 0.0));
      if (std::string_view(grid) == "/mem/nodata_2d.tif") {
        EXPECT_TRUE(std::isinf(out.xyz.x));
        EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(pj.get()));
      } else {
        EXPECT_NEAR(10.0, out.lpz.z, 1e-9);
      }
    }
  }
}

TEST(GridshiftTest, BoundaryTolerancesWrapAroundGridSwitchingAndConvergence) {
  absl::Cleanup clear_cache = [] { pj_clear_gridshift_knowngrids_cache(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogSuppressor silence(ctx.get());

  // 1. Exact eastern/northern boundary (tmpInt == width/height) and tiny
  //    positive shift at (west, south) so PJ_INV guess has indxy == -1 with
  //    frct > 1 - 10 * REL_TOLERANCE_HGRIDSHIFT.
  fs.AddFile(
      "/mem/tiny_shift.tif",
      BuildMinimalGeoTiff(
          2, 2, 2, 0.0, 10.0, 10.0, 10.0,
          MakeUniformPixels(2, 2, std::span<const float>({0.01f, 0.01f})),
          /*is_geographic=*/true,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
          "</GDALMetadata>"));
  ScopedPj pj_tiny(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/tiny_shift.tif"));
  ASSERT_NE(nullptr, pj_tiny.get());

  // Exact upper-right corner (10 deg E, 10 deg N).
  const PJ_COORD ur_fwd =
      proj_trans(pj_tiny.get(), PJ_FWD,
                 proj_coord(DegToRad(10.0), DegToRad(10.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_tiny.get()));
  EXPECT_FALSE(std::isinf(ur_fwd.lpz.lam));

  // Inverse at lower-left corner (0 deg E, 0 deg N) pushes guess slightly
  // below 0 (indxy = -1, frct > 1 - 1e-4) and clamps back to indxy = 0.
  const PJ_COORD ll_inv =
      proj_trans(pj_tiny.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_tiny.get()));
  EXPECT_NEAR(-0.01 * kArcSecToRad, ll_inv.lpz.lam, 1e-11);
  EXPECT_NEAR(-0.01 * kArcSecToRad, ll_inv.lpz.phi, 1e-11);

  // 2. Inverse iteration switching between two adjacent grids, and falling off
  //    grid edge when no neighbor exists.
  //    /mem/switch_east.tif covers [0, 10] x [0, 10] with +3600 arcsec (+1 deg)
  //    longitude shift; /mem/switch_west.tif covers [-10, 0] x [0, 10] with
  //    +3600 arcsec (+1 deg) longitude shift.
  fs.AddFile(
      "/mem/switch_east.tif",
      BuildMinimalGeoTiff(
          2, 2, 2, 0.0, 10.0, 10.0, 10.0,
          MakeUniformPixels(2, 2, std::span<const float>({0.0f, 3600.0f})),
          /*is_geographic=*/true,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
          "</GDALMetadata>"));
  fs.AddFile(
      "/mem/switch_west.tif",
      BuildMinimalGeoTiff(
          2, 2, 2, -10.0, 10.0, 10.0, 10.0,
          MakeUniformPixels(2, 2, std::span<const float>({0.0f, 3600.0f})),
          /*is_geographic=*/true,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
          "</GDALMetadata>"));

  // With both grids: PJ_INV at (0.5 deg, 5 deg) starts in switch_east, guesses
  // -0.5 deg (outside switch_east), switches to switch_west, and converges.
  ScopedPj pj_switch(proj_create(
      ctx.get(),
      "+proj=gridshift +grids=/mem/switch_east.tif,/mem/switch_west.tif"));
  ASSERT_NE(nullptr, pj_switch.get());
  const PJ_COORD sw_inv =
      proj_trans(pj_switch.get(), PJ_INV,
                 proj_coord(DegToRad(0.5), DegToRad(5.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_switch.get()));
  EXPECT_NEAR(DegToRad(-0.5), sw_inv.lpz.lam, 1e-11);

  // With only switch_east (and large shifts in both directions): PJ_INV at
  // (0.5 deg, 0.5 deg) falls off the western/southern edge and uses the first
  // approximation; similarly for eastern/northern edge when shift is negative.
  fs.AddFile(
      "/mem/edge_neg.tif",
      BuildMinimalGeoTiff(
          2, 2, 2, 0.0, 10.0, 10.0, 10.0,
          MakeUniformPixels(2, 2, std::span<const float>({-3600.0f, -3600.0f})),
          /*is_geographic=*/true,
          "<GDALMetadata>"
          "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
          "</GDALMetadata>"));
  ScopedPj pj_edge_sw(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/switch_east.tif"));
  ASSERT_NE(nullptr, pj_edge_sw.get());
  const PJ_COORD edge_sw =
      proj_trans(pj_edge_sw.get(), PJ_INV,
                 proj_coord(DegToRad(0.5), DegToRad(0.5), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_edge_sw.get()));
  EXPECT_NEAR(DegToRad(-0.5), edge_sw.lpz.lam, 1e-11);

  ScopedPj pj_edge_ne(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/edge_neg.tif"));
  ASSERT_NE(nullptr, pj_edge_ne.get());
  const PJ_COORD edge_ne =
      proj_trans(pj_edge_ne.get(), PJ_INV,
                 proj_coord(DegToRad(9.5), DegToRad(9.5), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_edge_ne.get()));
  EXPECT_NEAR(DegToRad(10.5), edge_ne.lpz.lam, 1e-11);
  EXPECT_NEAR(DegToRad(10.5), edge_ne.lpz.phi, 1e-11);

  // 3. normalizeX longitude wraparound (+2*pi and -2*pi) and fullWorldLongitude
  //    HUGE_VAL guard in grid_apply_internal.
  fs.AddFile("/mem/antimeridian_pos.tif",
             BuildMinimalGeoTiff(
                 2, 2, 2, 170.0, 10.0, 20.0, 10.0,
                 MakeUniformPixels(2, 2, std::span<const float>({1.0f, 2.0f})),
                 /*is_geographic=*/true,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                 "</GDALMetadata>"));
  ScopedPj pj_anti_pos(proj_create(
      ctx.get(), "+proj=gridshift +grids=/mem/antimeridian_pos.tif +over"));
  ASSERT_NE(nullptr, pj_anti_pos.get());
  const PJ_COORD wrap_plus =
      proj_trans(pj_anti_pos.get(), PJ_FWD,
                 proj_coord(DegToRad(-185.0), DegToRad(5.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_anti_pos.get()));
  EXPECT_FALSE(std::isinf(wrap_plus.lpz.lam));

  fs.AddFile("/mem/antimeridian_neg.tif",
             BuildMinimalGeoTiff(
                 2, 2, 2, -190.0, 10.0, 20.0, 10.0,
                 MakeUniformPixels(2, 2, std::span<const float>({1.0f, 2.0f})),
                 /*is_geographic=*/true,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                 "</GDALMetadata>"));
  ScopedPj pj_anti_neg(proj_create(
      ctx.get(), "+proj=gridshift +grids=/mem/antimeridian_neg.tif +over"));
  ASSERT_NE(nullptr, pj_anti_neg.get());
  const PJ_COORD wrap_minus =
      proj_trans(pj_anti_neg.get(), PJ_FWD,
                 proj_coord(DegToRad(185.0), DegToRad(5.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_anti_neg.get()));
  EXPECT_FALSE(std::isinf(wrap_minus.lpz.lam));

  // Full-world longitude grid [-180, 180] accepts x = HUGE_VAL in
  // isPointInExtent and hits `if (in.x == HUGE_VAL) return in;` in
  // grid_apply_internal.
  fs.AddFile("/mem/world.tif",
             BuildMinimalGeoTiff(
                 4, 2, 2, -180.0, 90.0, 120.0, 180.0,
                 MakeUniformPixels(4, 2, std::span<const float>({1.0f, 1.0f})),
                 /*is_geographic=*/true,
                 "<GDALMetadata>"
                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                 "</GDALMetadata>"));
  ScopedPj pj_world(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/world.tif"));
  ASSERT_NE(nullptr, pj_world.get());
  const PJ_XYZ huge_in = pj_world->fwd3d({HUGE_VAL, 0.0, 0.0}, pj_world.get());
  EXPECT_TRUE(std::isinf(huge_in.x));

  // 4. Inverse iterator non-convergence
  // (PROJ_ERR_COORD_TRANSFM_NO_CONVERGENCE):
  //    On [0, 10] deg, set lon_shift(0) = -5 deg (-18000 arcsec) and
  //    lon_shift(10) = +5 deg (+18000 arcsec) so shift(x) = x - 5 deg.
  //    For in.x = 6 deg, guess oscillates 5 -> 6 -> 5 -> 6 inside [0, 10] for
  //    all MAX_ITERATIONS (10) steps without converging.
  const std::vector<float> osc_pixels = {
      // North row (x=0, x=10): (lat_shift, lon_shift)
      0.0f,
      -18000.0f,
      0.0f,
      18000.0f,
      // South row (x=0, x=10): (lat_shift, lon_shift)
      0.0f,
      -18000.0f,
      0.0f,
      18000.0f,
  };
  fs.AddFile("/mem/oscillate.tif",
             BuildMinimalGeoTiff(2, 2, 2, 0.0, 10.0, 10.0, 10.0, osc_pixels,
                                 /*is_geographic=*/true,
                                 "<GDALMetadata>"
                                 "<Item name=\"TYPE\">HORIZONTAL_OFFSET</Item>"
                                 "</GDALMetadata>"));
  ScopedPj pj_osc(
      proj_create(ctx.get(), "+proj=gridshift +grids=/mem/oscillate.tif"));
  ASSERT_NE(nullptr, pj_osc.get());
  const PJ_COORD no_conv = proj_trans(
      pj_osc.get(), PJ_INV, proj_coord(DegToRad(6.0), DegToRad(5.0), 0.0, 0.0));
  EXPECT_TRUE(std::isinf(no_conv.lpz.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_NO_CONVERGENCE, proj_errno(pj_osc.get()));
}

}  // namespace
}  // namespace proj
